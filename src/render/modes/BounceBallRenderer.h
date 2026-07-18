/**
 * @file BounceBallRenderer.h
 * @brief 弹球模式渲染器——音频驱动弹球+彩虹拖尾（支持双球）
 * @date 2026-07-07
 * @details 弹球沿边框顺时针移动，在边框管道内上下弹跳。
 *          音频 RMS 驱动切向速度（安静=0），法向为一阶低通直接跟踪目标位置。
 *          拖尾按切向距离采样，球停则无新拖尾。
 *          双球模式下第二个球在对面（offset 0.5 周长）。
 * @note 线程安全：渲染线程独占。
 */

#pragma once

#include "IModeRenderer.h"
#include "../effects/BallTrailEffect.h"
#include "../../core/Result.h"
#include <vector>
#include <functional>

class BorderGeometry;
class Camera;

class BounceBallRenderer : public IModeRenderer {
public:
    BounceBallRenderer();
    ~BounceBallRenderer() override;

    Result<void> initialize(const BorderGeometry* geometry,
                            const Camera* camera,
                            int maxParticles,
                            std::function<void()> glInitFn) override;

    void pushCommand(const RenderCommand& cmd) override;
    void renderFrame(double dt) override;

    const char* name() const override { return "BounceBall"; }
    int activeParticles() const override { return m_dualMode ? 2 : 1; }

    Result<void> compileShaders();
    void updateBorder(const BorderGeometry& geo);
    double currentBorderWidth() const;

    /// @brief 清理 GL 资源
    /// @note 安全：显式 cleanup 供 RenderThread::switchMode 调用，符合 project_memory 约定
    void cleanup() override { m_effect.cleanup(); }

private:
    struct TrailPoint {
        double perimeterPos = 0.0;
        double normalDist = 0.0;
        double timestamp = 0.0;
    };

    struct BallState {
        double perimeterPos = 0.0;
        double normalDist = 50.0;
        std::vector<TrailPoint> trail;
        int trailWrite = 0;
        int trailCount = 0;
        std::vector<TrailVertex> trailVertices;
    };

    void sampleTrail(BallState& ball);
    void buildTrailVertices(BallState& ball);
    void renderBall(BallState& ball, double rmsNorm);

    // === 基础设施引用 ===
    const BorderGeometry* m_geometry = nullptr;
    const Camera* m_camera = nullptr;
    BallTrailEffect m_effect;

    // === 球物理状态（球1主控，球2跟随偏移） ===
    BallState m_ball1, m_ball2;
    double m_tangentialVel = 0.0;
    double m_smoothedTargetVel = 0.0;
    double m_pulseAmount = 0.03;
    double m_rawRms = 0.0;

    // === 可配置参数 ===
    double m_kTangentSpeed = 0.15;
    double m_kFollowSpeed = 25.0;
    double m_rmsSensitivity = 8.0;
    int    m_trailCapacity = 300;
    double m_trailMaxAge = 5.0;
    bool   m_dualMode = false;

    // === 运行时 ===
    float m_time = 0.0f;
    double m_dormantCoeff = 1.0;
    double m_onsetStrength = 0.0;
};
