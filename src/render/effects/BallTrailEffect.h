/**
 * @file BallTrailEffect.h
 * @brief 弹球+彩虹拖尾视觉效果
 * @date 2026-07-07
 * @details 球：单点精灵+环形脉冲着色器（audioDelta 驱动环半径脉冲）
 *          拖尾：GL_POINTS 数组，彩虹色相随索引变化，越老越透明
 *          两个独立 shader program（ballProgram + trailProgram）
 * @note 线程安全：渲染线程独占。
 */

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "IEffect.h"
#include "../../core/Result.h"
#include "../Camera.h"
#include <GL/glew.h>
#include <vector>
#include <array>
#include <cstddef>

/// @brief 拖尾顶点数据（28 字节，GPU 友好）
struct TrailVertex {
    float x = 0.0f, y = 0.0f;       ///< 屏幕像素坐标
    float r = 1.0f, g = 1.0f, b = 1.0f;  ///< RGB 颜色
    float alpha = 1.0f;              ///< 透明度 [0,1]
    float size = 4.0f;               ///< 点大小（像素）
};

class BorderGeometry;

/**
 * @class BallTrailEffect
 * @brief 弹球+拖尾效果实现
 * @details 管理两个 shader program：
 *          - ballProgram：单点精灵，环形脉冲片段着色器
 *          - trailProgram：多点精灵，彩虹渐隐
 */
class BallTrailEffect : public IEffect {
public:
    BallTrailEffect();
    ~BallTrailEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxTrailPoints) override;
    void update(float dt) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "BallTrailEffect"; }
    int activeParticles() const override { return 1; }

    /// @brief 编译两个着色器（GL 上下文就绪后调用）
    Result<void> compileShaders();

    /// @brief 设置屏幕尺寸
    void setScreenSize(int w, int h) { m_screenW = w; m_screenH = h; }

    /// @brief 从 BorderGeometry 计算球屏幕坐标
    void computeBallScreenPos(const BorderGeometry& geo,
                              double perimeterPos, double normalDist);

    /// @brief 从 BorderGeometry 计算拖尾点的屏幕坐标
    void computeTrailScreenPos(const BorderGeometry& geo,
                               double perimeterPos, double normalDist,
                               float& outX, float& outY);

    /// @brief 更新球渲染状态
    void setBallState(float screenX, float screenY, float radius, float pulseRadius);

    /// @brief 上传拖尾顶点数据到 VBO
    void setTrailData(const TrailVertex* vertices, int count);

    /// @brief 获取球屏幕坐标（供外部查询）
    float ballScreenX() const { return m_ballScreenX; }
    float ballScreenY() const { return m_ballScreenY; }

    /// @brief HSL → RGB 转换（供 BounceBallRenderer 构建拖尾颜色）
    static void hslToRgb(float h, float s, float l, float& r, float& g, float& b);

private:
    /// @brief 编译单个着色器阶段
    static unsigned compileShader(unsigned type, const char* src);

    // === 球渲染资源 ===
    unsigned m_ballVAO = 0, m_ballVBO = 0;
    unsigned m_ballProgram = 0;
    int m_locBallProjection = -1;
    int m_locBallScreenPos = -1;
    int m_locBallRadius = -1;
    int m_locPulseRadius = -1;
    int m_locBallScreenW = -1;
    int m_locBallScreenH = -1;

    // === 拖尾渲染资源 ===
    unsigned m_trailVAO = 0, m_trailVBO = 0;
    unsigned m_trailProgram = 0;
    int m_locTrailProjection = -1;
    int m_locTrailScreenW = -1;
    int m_locTrailScreenH = -1;

    // === 运行时数据 ===
    float m_ballScreenX = 0.0f, m_ballScreenY = 0.0f;
    float m_ballRadius = 12.0f;
    float m_pulseRadius = 0.5f;
    int m_screenW = 2560, m_screenH = 1440;
    int m_trailCapacity = 300;
    int m_trailCount = 0;

    // 缓存上次设置的 uniform 值（避免每帧重复 glUniform*）
    std::array<float, 16> m_cachedTrailProj{};
    std::array<float, 16> m_cachedBallProj{};
    float m_cachedTrailSW = -1.0f, m_cachedTrailSH = -1.0f;
    float m_cachedBallSW = -1.0f,  m_cachedBallSH = -1.0f;

    // 着色器源码
    static const char* ballVertexSource();
    static const char* ballFragmentSource();
    static const char* trailVertexSource();
    static const char* trailFragmentSource();
};