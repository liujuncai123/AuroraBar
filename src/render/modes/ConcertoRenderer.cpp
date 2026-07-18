/**
 * @file ConcertoRenderer.cpp
 * @brief 协奏曲模式渲染器实现——独立 Effect 架构
 * @date 2026-07-18
 * @details 重写后：不再生成顶点，只负责 EMA 平滑 + Effect 调度。
 *          顶点生成下沉到各 Effect 内部。
 * @note 安全：所有段索引访问前均做越界检查。
 */
#include "ConcertoRenderer.h"
#include "../BorderGeometry.h"
#include "../Camera.h"
#include "../../core/CommandTypes.h"
#include "../../logging/LoggerManager.h"
#include "../../params/ParamStore.h"
#include "../../segmentation/SegmentationManager.h"
#include <algorithm>
#include <cmath>
#include <cstring>

ConcertoRenderer::ConcertoRenderer() {
    AURORA_TRACE("Concerto", "Constructor");
}

ConcertoRenderer::~ConcertoRenderer() {
    AURORA_TRACE("Concerto", "Destructor");
    if (m_effect) m_effect->cleanup();
}

Result<void> ConcertoRenderer::initialize(const BorderGeometry* geometry,
                                          const Camera* camera,
                                          int maxParticles,
                                          std::function<void()> glInitFn) {
    AURORA_INFO("Concerto", "initialize() maxParticles={}", maxParticles);
    m_geometry = geometry;
    m_camera = camera;
    m_glInitFn = glInitFn;
    m_maxParticles = maxParticles;

    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }

    m_zPulses.clear();
    m_rawTargets.clear();
    m_zPulses.resize(m_segmentCount, 0.0);
    m_rawTargets.resize(m_segmentCount, 0.0);

    // 从 ParamStore 读取保存的 subMode（而非硬编码 0）
    int savedSubMode = ParamStore::Instance().GetInt("subMode");
    if (savedSubMode < ConcertoEffectFactory::kMinSubMode ||
        savedSubMode > ConcertoEffectFactory::kMaxSubMode) {
        savedSubMode = 0;  // 安全：非法值回退到默认
    }
    if (!switchSubMode(savedSubMode)) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError,
            "Failed to initialize saved subMode"));
    }
    return Result<void>::Ok();
}

bool ConcertoRenderer::switchSubMode(int newSubMode) {
    AURORA_INFO("Concerto", "switchSubMode {} → {}", m_currentSubMode, newSubMode);
    if (newSubMode < ConcertoEffectFactory::kMinSubMode ||
        newSubMode > ConcertoEffectFactory::kMaxSubMode) {
        AURORA_ERROR("Concerto", "Invalid subMode={}", newSubMode);
        return false;
    }

    // 旧 Effect 清理
    if (m_effect) {
        m_effect->cleanup();
        m_effect.reset();
    }

    // 工厂创建
    auto newEffect = ConcertoEffectFactory::create(newSubMode);
    if (!newEffect) {
        AURORA_ERROR("Concerto", "Factory returned nullptr for subMode={}", newSubMode);
        // 安全：回退到 subMode 0（除非已在 subMode 0）
        if (newSubMode != 0) {
            AURORA_WARN("Concerto", "Falling back to subMode 0");
            return switchSubMode(0);
        }
        return false;
    }

    // 初始化
    auto initResult = newEffect->initialize(m_glInitFn, m_maxParticles);
    if (!initResult.isOk()) {
        AURORA_ERROR("Concerto", "Effect initialize failed for subMode={}: {}",
                     newSubMode, initResult.error().message);
        if (newSubMode != 0) {
            AURORA_WARN("Concerto", "Falling back to subMode 0 after init failure");
            return switchSubMode(0);
        }
        return false;
    }

    // 编译 shader
    auto compileResult = newEffect->compileShaders();
    if (!compileResult.isOk()) {
        AURORA_ERROR("Concerto", "Shader compile failed for subMode={}: {}",
                     newSubMode, compileResult.error().message);
        if (newSubMode != 0) {
            AURORA_WARN("Concerto", "Falling back to subMode 0 after shader failure");
            return switchSubMode(0);
        }
        return false;
    }

    m_effect = std::move(newEffect);
    m_currentSubMode = newSubMode;

    // 同步运行时状态到新 Effect
    if (m_geometry) {
        m_effect->setScreenSize(m_geometry->screenW(), m_geometry->screenH());
        m_effect->setBorderGeometry(m_geometry);
    }
    // 同步段角色
    auto& segMgr = SegmentationManager::Instance();
    const auto& segs = segMgr.segments();
    for (int i = 0; i < m_segmentCount && i < static_cast<int>(segs.size()); ++i) {
        m_effect->setSegmentRole(i, segs[i].role);
    }
    return true;
}

Result<void> ConcertoRenderer::compileShaders() {
    // shader 编译已在 switchSubMode 内完成
    return Result<void>::Ok();
}

void ConcertoRenderer::updateBorder(const BorderGeometry& geo) {
    m_geometry = &geo;
    if (m_effect && m_geometry->pointCount() > 0) {
        m_effect->setScreenSize(m_geometry->screenW(), m_geometry->screenH());
        m_effect->setBorderGeometry(m_geometry);
    }
}

void ConcertoRenderer::pushCommand(const RenderCommand& cmd) {
    switch (cmd.type) {
    case RenderCommand::Type::SegmentParam: {
        int segIdx = cmd.segmentIndex;
        if (segIdx >= 0 && segIdx < m_segmentCount) {
            m_rawTargets[segIdx] = std::clamp(cmd.paramValue, 0.0, 1.0);
        }
        break;
    }
    case RenderCommand::Type::DormantState:
        m_dormantCoeff = std::clamp(cmd.paramValue, 0.0, 1.0);
        break;
    case RenderCommand::Type::GlobalParam: {
        if (std::strcmp(cmd.paramName.data(), "subMode") == 0) {
            int newSubMode = static_cast<int>(cmd.paramValue);
            AURORA_INFO("Concerto", "pushCommand subMode={} current={}", newSubMode, m_currentSubMode);
            if (newSubMode != m_currentSubMode) {
                bool ok = switchSubMode(newSubMode);
                AURORA_INFO("Concerto", "switchSubMode({}) result={} effect={}",
                            newSubMode, ok ? 1 : 0,
                            m_effect ? m_effect->name() : "null");
            }
        }
        break;
    }
    case RenderCommand::Type::MusicColor: {
        if (m_effect) {
            m_effect->setAudioColor(cmd.audioColor.data());
        }
        break;
    }
    default:
        break;
    }
}

void ConcertoRenderer::renderFrame(double dt) {
    if (dt <= 0.0) dt = 1.0 / 60.0;
    if (!m_geometry || m_geometry->pointCount() < 3) return;
    if (!m_effect) return;

    m_time += static_cast<float>(dt);

    auto& ps = ParamStore::Instance();
    m_followSpeed = ps.GetDouble("concerto.followSpeed");

    // 段数变化同步
    int newSegCount = static_cast<int>(SegmentationManager::Instance().segments().size());
    if (newSegCount != m_segmentCount && newSegCount > 0) {
        AURORA_INFO("Concerto", "Segment count changed {} → {}", m_segmentCount, newSegCount);
        m_segmentCount = newSegCount;
        m_zPulses.resize(m_segmentCount, 0.0);
        m_rawTargets.resize(m_segmentCount, 0.0);
        // 同步新段角色
        const auto& segs = SegmentationManager::Instance().segments();
        for (int i = 0; i < m_segmentCount && i < static_cast<int>(segs.size()); ++i) {
            m_effect->setSegmentRole(i, segs[i].role);
        }
    }

    // EMA 平滑（集中处理，Effect 只接收平滑后的值）
    double emaFactor = std::min(1.0, dt * m_followSpeed);
    double maxEnergy = 0.0;
    for (int i = 0; i < m_segmentCount; ++i) {
        double target = m_rawTargets[i] * m_dormantCoeff;
        m_zPulses[i] += (target - m_zPulses[i]) * emaFactor;
        m_effect->setSegmentEnergy(i, static_cast<float>(m_zPulses[i]));
        if (m_zPulses[i] > maxEnergy) maxEnergy = m_zPulses[i];
    }

    // 推送全局状态
    m_effect->setTime(m_time);
    m_effect->setScreenSize(m_geometry->screenW(), m_geometry->screenH());
    m_effect->setBorderGeometry(m_geometry);

    // 诊断日志：每 64 帧输出一次关键信息
    static int diagCounter = 0;
    if (++diagCounter % 64 == 0) {
        AURORA_INFO("Concerto", "renderFrame effect={} subMode={} maxEnergy={:.4f} dormant={:.3f} dt={:.4f} emaFactor={:.3f}",
                    m_effect->name(), m_currentSubMode, maxEnergy, m_dormantCoeff, dt, emaFactor);
    }

    // 渲染
    if (m_camera) {
        m_effect->render(*m_camera);
    }
}
