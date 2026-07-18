/**
 * @file CycleRenderer.cpp
 * @brief 循环态渲染实现
 * @date 2026-07-06
 */

#include "CycleRenderer.h"
#include "../../logging/LoggerManager.h"
#include "../../params/ParamStore.h"
#include <algorithm>
#include <string_view>
#include <cmath>

CycleRenderer::CycleRenderer() {
    AURORA_TRACE("CycleRenderer", "Constructor");
}

CycleRenderer::~CycleRenderer() {
    AURORA_TRACE("CycleRenderer", "Destructor");
}

Result<void> CycleRenderer::initialize(const BorderGeometry* geometry,
                                       const Camera* camera,
                                       int maxParticles,
                                       std::function<void()> glInitFn)
{
    AURORA_INFO("CycleRenderer", "initialize() maxParticles={}", maxParticles);

    if (!geometry || !camera) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument,
            "geometry or camera is null"));
    }

    m_geometry = geometry;
    m_camera = camera;
    m_maxParticles = maxParticles;

    auto res = m_effect.initialize(glInitFn, maxParticles);
    if (res.isErr()) {
        AURORA_ERROR("CycleRenderer", "ParticleEffect init failed: {}", res.error().message);
        return res;
    }

    AURORA_INFO("CycleRenderer", "initialize() OK");
    return Result<void>::Ok();
}

void CycleRenderer::pushCommand(const RenderCommand& cmd) {
    switch (cmd.type) {
    case RenderCommand::Type::SegmentParam:
        // 收集各段平滑值作为振荡幅度
        if (cmd.segmentIndex >= 0 && cmd.segmentIndex < 16) {
            m_oscAmps[cmd.segmentIndex] = static_cast<float>(
                10.0 + std::min(40.0, cmd.paramValue * 200.0));  // 基线 10px，音乐 → 50px
        }
        break;
    case RenderCommand::Type::GlobalParam:
        if (std::string_view(cmd.paramName.data()) == "flowSpeed") {
            m_flowSpeed = std::max(0.1, std::min(2.0, cmd.paramValue));
        } else if (std::string_view(cmd.paramName.data()) == "emissionRate") {
            m_emissionRate = std::max(0.0, std::min(1.0, cmd.paramValue));
        } else if (std::string_view(cmd.paramName.data()) == "brightness") {
            m_brightness = std::max(0.0, std::min(1.0, cmd.paramValue));
        } else if (std::string_view(cmd.paramName.data()) == "colorScheme") {
            m_pendingColorIndex = static_cast<int>(cmd.paramValue);
        } else if (std::string_view(cmd.paramName.data()) == "particleCount") {
            m_pendingParticleCount = std::max(500, static_cast<int>(cmd.paramValue));
        } else if (std::string_view(cmd.paramName.data()) == "customColor.r") {
            m_customR = static_cast<float>(std::clamp(cmd.paramValue, 0.0, 1.0));
        } else if (std::string_view(cmd.paramName.data()) == "customColor.g") {
            m_customG = static_cast<float>(std::clamp(cmd.paramValue, 0.0, 1.0));
        } else if (std::string_view(cmd.paramName.data()) == "customColor.b") {
            m_customB = static_cast<float>(std::clamp(cmd.paramValue, 0.0, 1.0));
        }
        break;
    case RenderCommand::Type::DormantState:
        m_dormantCoeff = cmd.paramValue;  // 0=休眠, 1=活跃
        break;
    case RenderCommand::Type::Onset:
        m_onsetStrength = std::max(m_onsetStrength, cmd.paramValue);
        break;
    default:
        break;
    }
}

void CycleRenderer::renderFrame(double dt) {
    if (dt <= 0.0) dt = 1.0 / 60.0;

    // 累计时间（用于 shader 波形动画）
    m_time += static_cast<float>(dt);

    // Onset 衰减 + 发射率加成
    if (m_onsetStrength > 0.001) {
        m_onsetStrength *= exp(-dt * 8.0);  // 快速衰减（~0.25s 半衰期）
    } else {
        m_onsetStrength = 0.0;
    }
    double onsetBoost = 1.0 + m_onsetStrength * 3.0;  // onset 时最多 4 倍发射率
    m_onsetBoost = onsetBoost;

    // 同步 ParamStore 可调参数到 ParticleEffect
    auto& ps = ParamStore::Instance();
    m_effect.setMaxDistance(static_cast<float>(ps.GetInt("cycle.maxDistance")));

    // 应用待处理的粒子数变更 + 实时同步 ParamStore 粒子数
    constexpr int kMaxParticles = 10000;
    int targetCount = m_pendingParticleCount > 0 
        ? m_pendingParticleCount 
        : ps.GetInt("particleCount");
    // 安全：硬上限防止粒子数失控增长
    if (targetCount > kMaxParticles) {
        AURORA_WARN("CycleRenderer", "particleCount capped {} → {} (hard limit)", targetCount, kMaxParticles);
        targetCount = kMaxParticles;
    }
    if (targetCount != m_effect.capacity()) {
        m_effect.resize(targetCount);
    }
    m_pendingParticleCount = -1;

    // 应用颜色方案
    if (m_pendingColorIndex >= 0) {
        m_colorScheme = m_pendingColorIndex;
        m_pendingColorIndex = -1;
    }

    emitParticles(dt);
    m_effect.update(static_cast<float>(dt));
}

void CycleRenderer::emitParticles(double dt) {
    // 无声时零生成：emissionRate 或 dormantCoeff 过低 → 不生成粒子
    double effectiveRate = m_emissionRate * std::max(0.0, m_dormantCoeff);
    if (effectiveRate < 0.05) {
        m_emitAccum = 0.0;
        return;
    }

    // 从 ParamStore 读取可调参数
    auto& ps = ParamStore::Instance();
    double particleLife = ps.GetDouble("cycle.particleLife");
    int sizeMin        = ps.GetInt("cycle.sizeMin");
    int sizeMax        = ps.GetInt("cycle.sizeMax");
    // 相互钳制：sizeMin 不能超过 sizeMax
    if (sizeMin > sizeMax) {
        ps.SetInt("cycle.sizeMin", sizeMax);
        sizeMin = sizeMax;
    }
    if (sizeMax < sizeMin) {
        ps.SetInt("cycle.sizeMax", sizeMin);
        sizeMax = sizeMin;
    }
    int emitMultiplier = ps.GetInt("cycle.emitMultiplier");

    // 生成率：emitMultiplier 粒/秒 × effectiveRate × onsetBoost
    double emitPerSec = static_cast<double>(emitMultiplier) * effectiveRate * m_onsetBoost;
    m_emitAccum += emitPerSec * dt;

    // 快速 xorshift32 PRNG（避免 rand() 的锁和慢速）
    auto randf = [this]() {
        m_rngState ^= m_rngState << 13;
        m_rngState ^= m_rngState >> 17;
        m_rngState ^= m_rngState << 5;
        return static_cast<float>(m_rngState) / 4294967296.0f;
    };

    while (m_emitAccum >= 1.0) {
        m_emitAccum -= 1.0;

        float pos = randf();
        float vel = m_flowSpeed * (0.15f + 0.15f * randf());
        float life = static_cast<float>(particleLife);
        float size = static_cast<float>(sizeMin)
                     + static_cast<float>(sizeMax - sizeMin) * static_cast<float>(m_brightness);

        // 按位置映射到频段，获取对应颜色
        int segIdx = std::min(15, static_cast<int>(pos * 16.0f));
        float amp = m_oscAmps[segIdx] / 50.0f;  // 归一化振幅 [0,1]
        float br = std::max(0.15f, static_cast<float>(m_brightness) * amp);

        // 频段→颜色：根据颜色方案
        float r, g, b;
        if (m_colorScheme == 0) {
            // 极光青：青绿系
            r = 0.0f; g = 0.6f + amp * 0.4f; b = 0.5f + amp * 0.5f;
            r *= br; g *= br; b *= br;
        } else if (m_colorScheme == 1) {
            // 熔岩橙：暖橙红系
            r = 0.8f + amp * 0.2f; g = 0.15f + amp * 0.5f; b = 0.02f + amp * 0.1f;
            r *= br; g *= br; b *= br;
        } else if (m_colorScheme == 2) {
            // 星云紫：紫品红系
            r = 0.4f + amp * 0.5f; g = 0.05f + amp * 0.15f; b = 0.6f + amp * 0.4f;
            r *= br; g *= br; b *= br;
        } else if (m_colorScheme == 3) {
            // 音频驱动：频率映射颜色（默认）
            if (segIdx < 4) {
                float t = segIdx / 4.0f;
                r = 1.0f; g = 0.2f + t * 0.6f; b = 0.05f + t * 0.15f;
            } else if (segIdx < 8) {
                float t = (segIdx - 4) / 4.0f;
                r = 1.0f - t * 0.7f; g = 0.8f + t * 0.2f; b = 0.2f - t * 0.15f;
            } else if (segIdx < 12) {
                float t = (segIdx - 8) / 4.0f;
                r = 0.3f - t * 0.3f; g = 1.0f; b = 0.05f + t * 0.75f;
            } else {
                float t = (segIdx - 12) / 4.0f;
                r = 0.0f + t * 0.5f; g = 1.0f - t * 0.8f; b = 0.8f + t * 0.2f;
            }
            r *= br; g *= br; b *= br;
        } else {
            // 手动选择：使用用户自定义颜色
            r = m_customR * br;
            g = m_customG * br;
            b = m_customB * br;
        }

        m_effect.spawn(pos, vel, life, size, br * r, br * g, br * b);
    }
}
