/**
 * @file CycleRenderer.h
 * @brief 循环态渲染器
 * @date 2026-07-06
 * @details 实现 IModeRenderer 接口，控制粒子沿边框环形流转。
 *          接收 RenderCommand 更新参数，驱动 ParticleEffect 生成/更新粒子。
 *          粒子速度 = flowSpeed（受 RMS 驱动），生成率 = emissionRate（受 RMS 驱动）。
 * @note 线程安全：渲染线程独占。
 */

#pragma once

#include "IModeRenderer.h"
#include "../effects/ParticleEffect.h"
#include "../../core/CommandTypes.h"
#include "../Camera.h"
#include "../BorderGeometry.h"
#include <memory>
#include <cstdint>
#include <functional>
#include <array>

class CycleRenderer : public IModeRenderer {
public:
    CycleRenderer();
    ~CycleRenderer() override;

    Result<void> initialize(const BorderGeometry* geometry,
                            const Camera* camera,
                            int maxParticles,
                            std::function<void()> glInitFn) override;

    void pushCommand(const RenderCommand& cmd) override;
    void renderFrame(double dt) override;
    const char* name() const override { return "CycleRenderer"; }
    int activeParticles() const override { return m_effect.activeParticles(); }

    /// @brief 编译粒子着色器（GL 上下文就绪后调用）
    Result<void> compileShaders() { return m_effect.compileShaders(); }

    /// @brief 更新边框采样点 uniform
    void updateBorder(const BorderGeometry& geo) { m_effect.updateBorderUniform(geo); m_effect.setScreenPerimeter(geo); }

    /// @brief 渲染粒子到当前帧缓冲
    void renderParticles(const Camera& cam) override {
        m_effect.setTime(m_time);
        m_effect.setOscillationAmps(m_oscAmps.data());
        m_effect.setSpreadWidth(m_spreadWidth);
        m_effect.render(cam);
    }

    /// @brief 清理 GL 资源
    void cleanup() override { m_effect.cleanup(); }

private:
    /// @brief 生成新粒子（按发射率）
    void emitParticles(double dt);

    ParticleEffect m_effect;
    const BorderGeometry* m_geometry = nullptr;
    const Camera* m_camera = nullptr;

    // 运行时参数
    double m_flowSpeed    = 0.4;   ///< 流转速度
    double m_emissionRate = 0.8;   ///< 粒子生成率 (0~1，1=全速)
    double m_brightness   = 0.7;   ///< 亮度
    double m_emitAccum    = 0.0;   ///< 生成累积器
    int    m_maxParticles = 2000;

    // 待处理参数变更（由 pushCommand 设置，renderFrame 消费）
    int    m_pendingColorIndex      = -1;  ///< 待切换颜色方案索引，-1=无变更
    int    m_pendingParticleCount   = -1;  ///< 待调整粒子数，-1=无变更
    int    m_colorScheme            = 3;   ///< 当前颜色方案（0=极光青 1=熔岩橙 2=星云紫 3=音频驱动）
    double m_dormantCoeff           = 1.0; ///< 休眠系数 0=休眠 1=活跃（DormantState 驱动）
    double m_onsetStrength         = 0.0; ///< Onset 节拍强度（衰减型）
    double m_onsetBoost           = 1.0; ///< Onset 发射率加成
    float m_customR = 0.0f, m_customG = 1.0f, m_customB = 0.8f; ///< 手动颜色
    float  m_time                   = 0.0f;///< 累计时间（驱动 shader 波形动画）
    uint32_t m_rngState = 12345;          ///< xorshift32 PRNG 状态
    std::array<float, 16> m_oscAmps{};    ///< 各段振荡幅度（16 段，音频驱动）
    float  m_spreadWidth            = 80.0f;///< 粒子横向散布宽度（px）
};
