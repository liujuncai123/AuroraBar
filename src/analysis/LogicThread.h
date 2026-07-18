/**
 * @file LogicThread.h
 * @brief 逻辑处理线程——FFT → 特征提取 → 物理模拟 → 过渡 → 生成 RenderCommand
 * @date 2026-07-06
 * @details 继承 ThreadBase，是音频数据到渲染指令的桥梁。
 *          阶段 5-7：FFT + 5 特征提取器 + PhysicsState + TransitionManager。
 *          完整管线：g_audioQueue → FFT → Extractors → Physics → Transition → (阶段10: RenderCommand)
 * @note 线程安全：通过 SPSC 无锁队列与采集/渲染线程通信。
 *       热路径零堆分配：FeatureSet 预分配 + spectra 复用 + 栈上物理/过渡计算。
 */

#pragma once

#include "../core/ThreadBase.h"
#include "../core/CommandTypes.h"
#include "../core/SPSCQueue.h"
#include "../logging/LoggerManager.h"
#include "../params/ParamStore.h"
#include "../audio/FftProcessor.h"
#include "../physics/PhysicsState.h"
#include "../segmentation/SegmentationManager.h"
#include "../transition/TransitionManager.h"
#include "FeatureSet.h"
#include "IFeatureExtractor.h"
#include "RmsExtractor.h"
#include "BandEnergyExtractor.h"
#include "HpssExtractor.h"
#include "SpectralFluxExtractor.h"
#include "LoudnessExtractor.h"
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>
#include <array>

// 前向声明
struct AudioFrame;
template<typename T, size_t C> class SPSCQueue;
extern SPSCQueue<AudioFrame, 32> g_audioQueue;
extern SPSCQueue<RenderCommand, 64> g_renderQueue;
extern SPSCQueue<ControlCommand, 16> g_controlQueue;

// SEH 包装器（实现在 src/core/ThreadSEH.cpp）
void LogicThread_onRun_SEH(class LogicThread* self);

class LogicThread : public ThreadBase {
public:
    LogicThread() : ThreadBase("Logic") {}

    /// @brief 逻辑帧计数
    size_t frameCount() const { return m_frameCount; }
    /// @brief 供 SEH 包装函数标记崩溃，跳过本帧
    void markCrashed() { m_crashed = true; }

protected:
    Result<void> onInitialize() override {
        AURORA_INFO("Logic", "onInitialize()");

        // ---- 订阅 ParamStore 变更 ----
        auto sub = [this](const std::string& key, double val) {
            m_paramsDirty.store(true, std::memory_order_release);
        };
        auto& ps = ParamStore::Instance();
        m_subIds.push_back(ps.Subscribe("borderWidth.top",    sub));
        m_subIds.push_back(ps.Subscribe("borderWidth.bottom", sub));
        m_subIds.push_back(ps.Subscribe("borderWidth.left",   sub));
        m_subIds.push_back(ps.Subscribe("borderWidth.right",  sub));
        m_subIds.push_back(ps.Subscribe("colorScheme",        sub));
        m_subIds.push_back(ps.Subscribe("customColor.r",     sub));
        m_subIds.push_back(ps.Subscribe("customColor.g",     sub));
        m_subIds.push_back(ps.Subscribe("customColor.b",     sub));
        m_subIds.push_back(ps.Subscribe("particleCount",      sub));
        m_subIds.push_back(ps.Subscribe("physics.mass",       sub));
        m_subIds.push_back(ps.Subscribe("physics.stiffness",  sub));
        m_subIds.push_back(ps.Subscribe("physics.damping",    sub));
        m_subIds.push_back(ps.Subscribe("physics.nonlinearity", sub));
        m_subIds.push_back(ps.Subscribe("border.pulseAmount", sub));
        m_subIds.push_back(ps.Subscribe("dormantBehavior",    sub));
        m_subIds.push_back(ps.Subscribe("dormantThreshold",   sub));
        m_subIds.push_back(ps.Subscribe("dormantDelay",       sub));
        // BounceBall 物理参数
        m_subIds.push_back(ps.Subscribe("bb.kTangentSpeed",     sub));
        m_subIds.push_back(ps.Subscribe("bb.kFollowSpeed",      sub));
        m_subIds.push_back(ps.Subscribe("bb.rmsSensitivity",    sub));
        m_subIds.push_back(ps.Subscribe("bb.trailLength",        sub));
        m_subIds.push_back(ps.Subscribe("bb.trailMaxAge",        sub));
        m_subIds.push_back(ps.Subscribe("bb.dualMode",          sub));
        // 段数变化 → 重建分段 + 物理状态
        m_subIds.push_back(ps.Subscribe("segmentsPerEdge",       sub));
        AURORA_INFO("Logic", "Subscribed to {} ParamStore keys", m_subIds.size());

        auto& segMgr = SegmentationManager::Instance();
        segMgr.rebuild();

        m_fft = std::make_unique<FftProcessor>();
        auto fftRes = m_fft->initialize(32);
        if (fftRes.isErr()) {
            AURORA_ERROR("Logic", "FftProcessor init failed: {}", fftRes.error().message);
            return fftRes;
        }

        m_features.resize(m_fft->bandCount());
        m_spectrum.resize(m_fft->bandCount());
        m_prevSpectrum.resize(m_fft->bandCount());

        m_extractors.push_back(std::make_unique<RmsExtractor>());
        m_extractors.push_back(std::make_unique<BandEnergyExtractor>());
        m_extractors.push_back(std::make_unique<HpssExtractor>());
        m_extractors.push_back(std::make_unique<SpectralFluxExtractor>());
        m_extractors.push_back(std::make_unique<LoudnessExtractor>());

        int segCount = segMgr.totalSegments();
        m_physicsStates.reserve(segCount);
        for (int i = 0; i < segCount; ++i) {
            m_physicsStates.emplace_back(segMgr.getPhysicsPreset(i));
        }

        // ---- 协奏模式：HPSS 历史最大值归一化 ----
        m_harmonicMaxHist.resize(segCount, 1e-6);
        m_percussiveMaxHist.resize(segCount, 1e-6);

        // ---- 过渡管理器 ----
        m_transition.rebuild(segCount);

        AURORA_INFO("Logic", "onInitialize() OK extractors={} segments={}",
                    m_extractors.size(), segCount);
        return Result<void>::Ok();
    }

    void onRun() override {
        LogicThread_onRun_SEH(this);
        if (m_crashed) {
            m_crashed = false;
            AURORA_ERROR("Logic", "SEH caught crash in onRun, skipping frame");
            return;
        }
    }

public:
    /// @brief onRun 的实际逻辑体（被 SEH 包装调用，避免 C2712）
    void onRunBody() {
        // ---- ControlCommand 消费 ----
        {
            ControlCommand cc;
            if (g_controlQueue.tryPop(cc)) {
                switch (cc.type) {
                case ControlCommand::Type::Collapse: {
                    RenderCommand rc;
                    rc.type = RenderCommand::Type::OverlayVisible;
                    rc.paramValue = 0.0;
                    g_renderQueue.tryPush(rc);
                    break;
                }
                case ControlCommand::Type::Expand: {
                    RenderCommand rc;
                    rc.type = RenderCommand::Type::OverlayVisible;
                    rc.paramValue = 1.0;
                    g_renderQueue.tryPush(rc);
                    break;
                }
                case ControlCommand::Type::SetMode: {
                    RenderCommand rc;
                    rc.type = RenderCommand::Type::ModeChange;
                    rc.paramValue = cc.value;
                    if (!g_renderQueue.tryPush(rc))
                        AURORA_WARN("Logic", "SetMode → render queue full, command lost");
                    AURORA_INFO("Logic", "SetMode → {}", static_cast<int>(cc.value));
                    break;
                }
                case ControlCommand::Type::SetSubMode: {
                    RenderCommand rc;
                    rc.type = RenderCommand::Type::GlobalParam;
                    std::fill(rc.paramName.begin(), rc.paramName.end(), 0);
                    constexpr char kName[] = "subMode";
                    std::copy(kName, kName + std::char_traits<char>::length(kName), rc.paramName.begin());
                    rc.paramValue = cc.value;
                    if (!g_renderQueue.tryPush(rc))
                        AURORA_WARN("Logic", "SetSubMode → render queue full, command lost");
                    AURORA_INFO("Logic", "SetSubMode → {}", static_cast<int>(cc.value));
                    break;
                }
                default: break;
                }
            }
        }

        // ---- 参数变更同步 ----
        if (m_paramsDirty.exchange(false, std::memory_order_acquire)) {
            syncSegmentation();
            generateParamCommands();
        }

        AudioFrame frame;
        if (!g_audioQueue.tryPop(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return;
        }

        // ---- FFT + 特征提取 ----
        m_fft->process(frame.samples, m_spectrum);
        for (auto& ext : m_extractors) {
            ext->process(m_spectrum, m_prevSpectrum, m_features);
        }
        m_features.timestamp = frame.timestamp;

        // ---- 音乐驱动颜色：FFT 主频 → 色相 → RGB → EMA 平滑 → 推送 ----
        // 安全：函数内部检查 concerto.musicColor 开关 + mode==2，未启用时直接返回
        computeMusicColor();

        // ---- Onset 检测（频谱通量峰值） ----
        {
            double fluxSum = 0.0;
            for (auto v : m_features.spectralFlux) fluxSum += v;
            // 指数衰减的历史平均值
            m_fluxAvg = m_fluxAvg * 0.95 + fluxSum * 0.05;
            // 峰值超过历史平均 2.5 倍时触发 onset
            if (fluxSum > m_fluxAvg * 2.5 && fluxSum > 0.1) {
                RenderCommand onsetCmd;
                onsetCmd.type = RenderCommand::Type::Onset;
                onsetCmd.paramValue = std::min(1.0, fluxSum / (m_fluxAvg * 2.5 + 0.01));
                g_renderQueue.tryPush(onsetCmd);
            }
        }

        // ---- 物理模拟：根据模式选择驱动源 ----
        auto& ps = ParamStore::Instance();
        int mode = ps.GetInt("mode");
        size_t nBands = m_features.bandEnergy.size();
        size_t nSegs  = m_physicsStates.size();
        auto& segMgr = SegmentationManager::Instance();
        const auto& segs = segMgr.segments();

        // 协奏模式：跳过 PhysicsState，直接使用 driveValue（ConcertoRenderer 自带 EMA）
        std::vector<double> rawValues(m_physicsStates.size(), 0.0);

        for (size_t i = 0; i < nSegs; ++i) {
            double driveValue = 0.0;

            if (mode == 2) {
                // === 协奏模式：根据段角色选择 HPSS 驱动源 ===
                if (i < segs.size()) {
                    const auto& seg = segs[i];
                    size_t bStart = seg.freqBinStart;
                    size_t bEnd = std::min(seg.freqBinEnd, nBands);

                    auto avgEnergy = [&](const std::vector<double>& energies) -> double {
                        if (bStart >= bEnd) return 0.0;
                        double sum = 0.0;
                        for (size_t b = bStart; b < bEnd; ++b) sum += energies[b];
                        return sum / static_cast<double>(bEnd - bStart);
                    };

                    double harmonic   = avgEnergy(m_features.harmonicEnergy);
                    double percussive = avgEnergy(m_features.percussiveEnergy);
                    double band       = avgEnergy(m_features.bandEnergy);

                    // 归一化：谐波/打击能量用历史最大值归一化到 [0,1]（与 BandEnergy 一致）
                    // 安全：防除零，初始值 1e-6
                    if (i < m_harmonicMaxHist.size()) {
                        m_harmonicMaxHist[i]   = std::max(harmonic,   m_harmonicMaxHist[i]   * 0.999);
                        m_percussiveMaxHist[i] = std::max(percussive, m_percussiveMaxHist[i] * 0.999);
                        harmonic   = (m_harmonicMaxHist[i]   > 1e-9) ? harmonic   / m_harmonicMaxHist[i]   : 0.0;
                        percussive = (m_percussiveMaxHist[i] > 1e-9) ? percussive / m_percussiveMaxHist[i] : 0.0;
                    }

                    // 角色混合：主职责 70% + 辅职责 20% + 总能量 10%
                    switch (seg.role) {
                    case SegmentRole::HarmonicMain:
                        driveValue = harmonic * 0.7 + percussive * 0.2 + band * 0.1; break;
                    case SegmentRole::HarmonicAux:
                        driveValue = harmonic * 0.65 + percussive * 0.25 + band * 0.1; break;
                    case SegmentRole::HarmonicWeak:
                        driveValue = harmonic * 0.5 + percussive * 0.35 + band * 0.15; break;
                    case SegmentRole::PercussiveMain:
                        driveValue = percussive * 0.7 + harmonic * 0.2 + band * 0.1; break;
                    case SegmentRole::PercussiveAux:
                        driveValue = percussive * 0.65 + harmonic * 0.25 + band * 0.1; break;
                    case SegmentRole::PercussiveWeak:
                        driveValue = percussive * 0.5 + harmonic * 0.35 + band * 0.15; break;
                    default:
                        driveValue = band; break;
                    }
                }

                // 协奏模式：跳过 PhysicsState，直接使用 driveValue（ConcertoRenderer 自带 EMA）
                rawValues[i] = driveValue;
                // 仍更新 PhysicsState 以保持状态一致（模式切换时不会跳变）
                m_physicsStates[i].setTarget(driveValue);
                m_physicsStates[i].update();
            } else {
                // === 循环/弹球模式：原有 bandEnergy 映射（2 频段→1 段） ===
                // 安全：nBands 是 size_t（无符号），nBands - 1 在 nBands==0 时下溢为 SIZE_MAX
                //       必须先检查 nBands > 0，否则 std::min(b0+1, SIZE_MAX) = b0+1 导致越界
                size_t b0 = i * 2;
                if (nBands > 0 && b0 < nBands) {
                    size_t b1 = (b0 + 1 < nBands) ? (b0 + 1) : (nBands - 1);
                    driveValue = (m_features.bandEnergy[b0] + m_features.bandEnergy[b1]) * 0.5;
                }

                m_physicsStates[i].setTarget(driveValue);
                m_physicsStates[i].update();
                rawValues[i] = m_physicsStates[i].currentValue();
            }
        }
        // 使用缩放后的 RMS（×200），避免休眠态误判（原始 RMS 太小，0.001~0.01）
        double rmsScaled = std::max(0.0, std::min(1.0, m_features.rms * 200.0));

        // 同步休眠参数（每帧检查，响应 UI 变更）
        m_transition.setStateRmsThreshold(ps.GetDouble("dormantThreshold"));
        m_transition.setStateSilenceTimeout(ps.GetDouble("dormantDelay"));
        // 休眠行为：保持呼吸→0.15，渐隐→0.0
        m_transition.setDormantTarget(ps.GetInt("dormantBehavior") == 0 ? 0.15 : 0.0);

        m_transition.update(rmsScaled, rawValues, 1.0 / 48.0);

        // ---- 休眠态联动：状态变化时发送 DormantState RenderCommand ----
        {
            auto newState = m_transition.lifeState();
            double newCoeff = m_transition.stateCoeff();
            if (newState != m_lastLifeState || std::abs(newCoeff - m_lastStateCoeff) > 0.05) {
                RenderCommand dormCmd;
                dormCmd.type = RenderCommand::Type::DormantState;
                dormCmd.paramValue = newCoeff;
                g_renderQueue.tryPush(dormCmd);
                m_lastLifeState = newState;
                m_lastStateCoeff = newCoeff;
            }
        }

        m_prevSpectrum = m_spectrum;

        // ---- 生成 RenderCommand → g_renderQueue ----
        RenderCommand cmd;
        cmd.timestamp = m_frameCount++;
        cmd.type = RenderCommand::Type::SegmentParam;

        // 每段一个 RenderCommand
        size_t segCount = (mode == 2) 
            ? m_physicsStates.size()                    // 协奏：全部段
            : std::min(m_physicsStates.size(), static_cast<size_t>(16));  // 循环：限 16
        for (size_t i = 0; i < segCount; ++i) {
            // 协奏模式：跳过 TransitionManager 参数平滑，ConcertoRenderer 自带 EMA
            cmd.paramValue = (mode == 2)
                ? rawValues[i]
                : m_transition.smoothedValue(static_cast<int>(i));
            cmd.segmentIndex = static_cast<int16_t>(i);
            g_renderQueue.tryPush(cmd);
        }

        // 发送 RMS 驱动的全局参数（必须填 paramName，CycleRenderer 靠它匹配）
        cmd.type = RenderCommand::Type::GlobalParam;
        cmd.segmentIndex = -1;

        auto pushNamed = [&](const char* name, double val) {
            cmd.paramName.fill(0);
            size_t len = std::strlen(name);
            std::copy(name, name + len, cmd.paramName.begin());
            cmd.paramValue = val;
            g_renderQueue.tryPush(cmd);
        };

        pushNamed("flowSpeed",    0.01 + rmsScaled * 0.8);    // 安静 0.01 → 音乐 0.81
        pushNamed("emissionRate", rmsScaled);                  // 完全由音乐驱动：无声=0，有声=0~1
        pushNamed("brightness",   0.5 + rmsScaled * 0.5);    // 安静 0.5(可见) → 音乐 1.0(满亮)
        pushNamed("rawRms",       m_features.rms);             // BounceBall 模式：原始 RMS 用于计算差值推力
        pushNamed("curvatureDepth", 0.85 - rmsScaled * 0.55); // 曲率 RMS 驱动：安静→深凹(0.85)，激烈→浅凹(0.30)
    }

protected:
    void onCleanup() override {
        auto& ps = ParamStore::Instance();
        for (int id : m_subIds) ps.Unsubscribe(id);
        m_subIds.clear();
        m_extractors.clear();
        m_physicsStates.clear();
        m_fft.reset();
        AURORA_INFO("Logic", "Cleanup complete");
    }

private:
    /// @brief 同步分段状态：段数变化时重建物理状态、过渡器、HPSS 历史
    void syncSegmentation() {
        auto& segMgr = SegmentationManager::Instance();
        auto& ps = ParamStore::Instance();
        int newPerEdge = ps.GetInt("segmentsPerEdge");
        if (segMgr.subSegmentsPerEdge() != newPerEdge) {
            segMgr.setSubSegmentsPerEdge(newPerEdge);
            segMgr.rebuild();
            int newCount = segMgr.totalSegments();
            AURORA_INFO("Logic", "Segmentation rebuilt: {} edges × {} = {} segments",
                        4, newPerEdge, newCount);

            // 重建物理状态数组
            m_physicsStates.clear();
            m_physicsStates.reserve(newCount);
            for (int i = 0; i < newCount; ++i) {
                m_physicsStates.emplace_back(segMgr.getPhysicsPreset(i));
            }

            // 重建 HPSS 历史最大值
            m_harmonicMaxHist.clear();
            m_percussiveMaxHist.clear();
            m_harmonicMaxHist.resize(newCount, 1e-6);
            m_percussiveMaxHist.resize(newCount, 1e-6);

            // 重建过渡管理器
            m_transition.rebuild(newCount);
        }
    }

    /// @brief 读取 ParamStore 当前值，生成 RenderCommand 推送到 g_renderQueue
    void generateParamCommands() {
        auto& ps = ParamStore::Instance();
        RenderCommand cmd;

        // 边框宽度 → BorderConfig（四边分别推送）
        auto pushBorder = [&](const char* edge, double val) {
            cmd.type = RenderCommand::Type::BorderConfig;
            std::fill(cmd.paramName.begin(), cmd.paramName.end(), 0);
            size_t len = std::strlen(edge);
            std::copy(edge, edge + len, cmd.paramName.begin());
            cmd.paramValue = val;
            g_renderQueue.tryPush(cmd);
        };
        pushBorder("top",    static_cast<double>(ps.GetInt("borderWidth.top")));
        pushBorder("bottom", static_cast<double>(ps.GetInt("borderWidth.bottom")));
        pushBorder("left",   static_cast<double>(ps.GetInt("borderWidth.left")));
        pushBorder("right",  static_cast<double>(ps.GetInt("borderWidth.right")));

        // 颜色方案
        cmd.type = RenderCommand::Type::GlobalParam;
        std::fill(cmd.paramName.begin(), cmd.paramName.end(), 0);
        auto nameColor = "colorScheme";
        std::copy(nameColor, nameColor + 12, cmd.paramName.begin());
        cmd.paramValue = ps.GetDouble("colorScheme");
        g_renderQueue.tryPush(cmd);

        // 粒子数量
        std::fill(cmd.paramName.begin(), cmd.paramName.end(), 0);
        auto nameParticle = "particleCount";
        std::copy(nameParticle, nameParticle + 14, cmd.paramName.begin());
        cmd.paramValue = static_cast<double>(ps.GetInt("particleCount"));
        g_renderQueue.tryPush(cmd);

        // 更新物理预设（LogicThread 自身使用）
        double mass      = ps.GetDouble("physics.mass");
        double stiffness = ps.GetDouble("physics.stiffness");
        double damping   = ps.GetDouble("physics.damping");
        double nonlinearity = ps.GetDouble("physics.nonlinearity");
        for (auto& p : m_physicsStates) {
            p.resetParams(PhysicsParams{mass, stiffness, damping, nonlinearity});
        }

        // BounceBall 物理参数 → GlobalParam 转发给渲染线程
        auto pushBB = [&](const char* name, double val) {
            cmd.type = RenderCommand::Type::GlobalParam;
            std::fill(cmd.paramName.begin(), cmd.paramName.end(), 0);
            size_t len = std::strlen(name);
            std::copy(name, name + len, cmd.paramName.begin());
            cmd.paramValue = val;
            g_renderQueue.tryPush(cmd);
        };
        pushBB("bb.kTangentSpeed",     ps.GetDouble("bb.kTangentSpeed"));
        pushBB("bb.kFollowSpeed",      ps.GetDouble("bb.kFollowSpeed"));
        pushBB("bb.rmsSensitivity",    ps.GetDouble("bb.rmsSensitivity"));
        pushBB("bb.trailLength",        static_cast<double>(ps.GetInt("bb.trailLength")));
        pushBB("bb.trailMaxAge",        ps.GetDouble("bb.trailMaxAge"));
        pushBB("bb.dualMode",          static_cast<double>(ps.GetInt("bb.dualMode")));
        pushBB("physics.nonlinearity",   ps.GetDouble("physics.nonlinearity"));
        pushBB("border.pulseAmount",     ps.GetDouble("border.pulseAmount"));

        // 手动颜色 → CycleRenderer
        pushBB("customColor.r",         ps.GetDouble("customColor.r"));
        pushBB("customColor.g",         ps.GetDouble("customColor.g"));
        pushBB("customColor.b",         ps.GetDouble("customColor.b"));

        AURORA_INFO("Logic", "generateParamCommands() top={} color={} particles={} physics=({},{},{})",
                    ps.GetInt("borderWidth.top"),
                    static_cast<int>(ps.GetDouble("colorScheme")),
                    ps.GetInt("particleCount"),
                    mass, stiffness, damping);
    }

    std::unique_ptr<FftProcessor> m_fft;
    FeatureSet                    m_features;
    std::vector<double>           m_spectrum;
    std::vector<double>           m_prevSpectrum;
    std::vector<std::unique_ptr<IFeatureExtractor>> m_extractors;
    std::vector<PhysicsState>     m_physicsStates;
    TransitionManager             m_transition;  ///< 统一过渡调度
    size_t                        m_frameCount = 0;

    // 协奏模式：HPSS 每段历史最大值（指数衰减归一化，与 BandEnergyExtractor 算法一致）
    std::vector<double> m_harmonicMaxHist;    ///< 每段谐波能量历史最大值
    std::vector<double> m_percussiveMaxHist;  ///< 每段打击能量历史最大值

    // ParamStore 订阅
    std::vector<int>    m_subIds;           ///< 订阅 ID 列表
    std::atomic<bool>   m_paramsDirty{false};  ///< 参数变更标记

    // 休眠态追踪
    LifecycleState m_lastLifeState = LifecycleState::Dormant;
    double         m_lastStateCoeff = 0.0;

    // Onset 检测
    double m_fluxAvg = 0.0;  ///< 频谱通量历史平均值
    bool m_crashed = false;  ///< SEH 崩溃标记，跳过本帧

    // ============================================================
    // 音乐驱动颜色（Task 16）：FFT 主频 → 色相 → RGB → EMA 平滑
    // ============================================================

    /// @brief 计算 FFT 主频并映射为色相→RGB，EMA 平滑后推送 MusicColor 命令
    /// @details 算法链路：
    ///          1. 在对数频段谱上找峰值 band（20Hz~22050Hz 对数映射到 32 bands）
    ///          2. 反向计算 band 中心频率
    ///          3. 频率→色相对数映射：20Hz→red(0°), 2000Hz→cyan(180°), 8000Hz→purple(280°)
    ///          4. HSV→RGB (S=0.85, V=1.0)
    ///          5. EMA 平滑（α=0.1，慢响应避免闪烁）
    ///          6. 推送 RenderCommand::MusicColor 到 g_renderQueue
    /// @pre  m_spectrum 已通过 m_fft->process() 填充
    /// @post m_audioColorEMA 更新；g_renderQueue 收到一条 MusicColor 命令
    /// @note 安全：非协奏模式或 spectrum 未就绪时跳过；开关关闭时跳过 RGB 计算
    ///       但仍推送命令，确保 Effect 内部 m_musicColorEnabled 与 ParamStore 同步
    ///       （否则开关从开→关后 Effect 仍用旧 audioColor 着色，无法回到顶点色）
    ///       线程安全：逻辑线程独占调用，无外部并发
    void computeMusicColor() {
        // 安全：非协奏模式时跳过（节省 CPU，避免无效推送）
        auto& ps = ParamStore::Instance();
        if (ps.GetInt("mode") != 2) return;
        // 安全：spectrum 未就绪时跳过
        if (m_spectrum.empty()) return;

        // 开关状态：开启时计算 RGB，关闭时跳过计算（但下方仍推送以同步 Effect 开关状态）
        const bool enabled = ps.GetInt("concerto.musicColor") != 0;

        if (enabled) {
            // ---- 1. 在对数频段谱上找峰值（跳过 band 0 即 DC/极低频）----
            size_t peakBand = 1;
            double peakMag = 0.0;
            for (size_t b = 1; b < m_spectrum.size(); ++b) {
                if (m_spectrum[b] > peakMag) {
                    peakMag = m_spectrum[b];
                    peakBand = b;
                }
            }
            // 峰值过小：不更新 EMA（保持上一次颜色，避免静音时颜色乱跳）
            if (peakMag >= 0.01) {
                // ---- 2. 反向计算 band 中心频率 ----
                // FftProcessor::buildBandMap 的对数映射：log(20) → log(22050)
                // band 索引均匀分布在 [logMin, logMax] 上，中心位置 = (b + 0.5) / (bandCount - 1)
                const double kLogMin = std::log(20.0);
                const double kLogMax = std::log(22050.0);
                const double bandCountF = static_cast<double>(m_spectrum.size());
                const double logFreq = kLogMin
                    + (static_cast<double>(peakBand) + 0.5) / (bandCountF - 1.0)
                    * (kLogMax - kLogMin);
                const double peakFreq = std::exp(logFreq);

                // ---- 3. 频率 → 色相对数映射（20Hz→0°, 8000Hz→280°）----
                constexpr double kFreqMin = 20.0;
                constexpr double kFreqMax = 8000.0;
                const double clampedFreq = std::max(kFreqMin, std::min(kFreqMax, peakFreq));
                const double hueRatio =
                    (std::log(clampedFreq) - std::log(kFreqMin))
                    / (std::log(kFreqMax) - std::log(kFreqMin));
                const float hue = static_cast<float>(hueRatio * 280.0);  // 0°~280°

                // ---- 4. HSV → RGB (S=0.85, V=1.0) ----
                const float s = 0.85f, v = 1.0f;
                const float c = v * s;
                const float hp = hue / 60.0f;
                const float x = c * (1.0f - std::abs(std::fmod(hp, 2.0f) - 1.0f));
                float r = 0.0f, g = 0.0f, bb = 0.0f;
                if      (hp < 1.0f) { r = c; g = x; bb = 0; }
                else if (hp < 2.0f) { r = x; g = c; bb = 0; }
                else if (hp < 3.0f) { r = 0; g = c; bb = x; }
                else if (hp < 4.0f) { r = 0; g = x; bb = c; }
                else if (hp < 5.0f) { r = x; g = 0; bb = c; }
                else                { r = c; g = 0; bb = x; }
                const float m = v - c;
                r += m; g += m; bb += m;

                // ---- 5. EMA 平滑（α=0.1，慢响应避免闪烁）----
                m_audioColorEMA[0] = m_audioColorEMA[0] * 0.9f + r  * 0.1f;
                m_audioColorEMA[1] = m_audioColorEMA[1] * 0.9f + g  * 0.1f;
                m_audioColorEMA[2] = m_audioColorEMA[2] * 0.9f + bb * 0.1f;
            }
        }

        // ---- 6. 始终推送 MusicColor 命令到渲染队列 ----
        // 安全：开关关闭时也推送（RGB 保持上次值），触发 Effect::setAudioColor 同步
        //       m_musicColorEnabled=false，让 shader 回到顶点色。否则开关从开→关后
        //       Effect 仍用旧 audioColor 着色，行为不符合预期。
        // 安全：tryPush 失败时仅丢弃这一帧颜色（不影响其他命令）
        RenderCommand cmd;
        cmd.type = RenderCommand::Type::MusicColor;
        cmd.audioColor = m_audioColorEMA;
        if (!g_renderQueue.tryPush(cmd)) {
            AURORA_TRACE("Logic", "MusicColor push skipped: render queue full");
        }
    }

    /// @brief 音乐驱动颜色的 EMA 平滑状态（RGB，每通道 [0,1]）
    /// @note 初始为白色 {1,1,1}，避免启动时画面全黑
    std::array<float, 3> m_audioColorEMA = {1.0f, 1.0f, 1.0f};
};
