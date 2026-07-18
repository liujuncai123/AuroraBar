/**
 * @file FluidWaveEffect.h
 * @brief 协奏子模式 2：流体波
 * @date 2026-07-18
 * @details 沿边框绘制 TRIANGLE_STRIP 带状流体，sin 复合波驱动顶点偏移；
 *          颜色沿周长流动（紫 → 青循环），Reinhard 色调映射。
 * @note 线程安全：渲染线程独占。
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "IConcertoEffect.h"
#include "../../../core/Result.h"
#include "../../Camera.h"
#include <GL/glew.h>
#include <vector>
#include <array>
#include <functional>

/// @brief 流体波顶点（7 floats：pos2 + u1 + color3 + alpha1）
struct FluidWaveVertex {
    float x = 0.0f, y = 0.0f;       ///< 屏幕像素坐标
    float u = 0.0f;                  ///< 沿周长归一化 [0,1]
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;
};

class BorderGeometry;

/**
 * @class FluidWaveEffect
 * @brief 协奏子模式 2：流体波
 * @details 沿屏幕四边生成带状 TRIANGLE_STRIP，顶点偏移由 sin 复合波驱动；
 *          颜色随时间在紫↔青之间流动循环。
 */
class FluidWaveEffect : public IConcertoEffect {
public:
    FluidWaveEffect();
    ~FluidWaveEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "FluidWaveEffect"; }
    int activeParticles() const override { return m_vertCount; }

    Result<void> compileShaders() override;

    void setSegmentEnergy(int segIdx, float emaValue) override;
    void setSegmentRole(int segIdx, SegmentRole role) override;
    void setTime(float t) override { m_time = t; }
    void setScreenSize(int w, int h) override { m_screenW = w; m_screenH = h; }
    void setBorderGeometry(const BorderGeometry* geo) override { m_geometry = geo; }
    void setAudioColor(const float rgb[3]) override;

private:
    static unsigned compileShader(unsigned type, const char* src);
    void generateVertices();

    unsigned m_vao = 0, m_vbo = 0, m_program = 0;
    int m_locProjection = -1, m_locScreenW = -1, m_locScreenH = -1;
    int m_locTime = -1, m_locFlowSpeed = -1;
    int m_locAudioColor = -1, m_locMusicColorEnabled = -1;

    const BorderGeometry* m_geometry = nullptr;
    int m_screenW = 2560, m_screenH = 1440;
    float m_time = 0.0f;
    float m_flowSpeed = 0.4f;
    float m_audioColor[3] = {1.0f, 1.0f, 1.0f};
    bool m_musicColorEnabled = false;

    std::vector<float> m_segmentEnergy;
    std::vector<SegmentRole> m_segmentRole;
    int m_segmentCount = 0;
    int m_samplesPerSegment = 32;
    std::vector<FluidWaveVertex> m_vertices;
    int m_vertCount = 0;
    int m_maxVerts = 0;

    std::array<float, 16> m_cachedProj{};
    float m_cachedSW = -1.0f, m_cachedSH = -1.0f;

    static const char* vertexSource();
    static const char* fragmentSource();
};
