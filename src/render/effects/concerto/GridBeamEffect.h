/**
 * @file GridBeamEffect.h
 * @brief 协奏子模式 4：网格光带
 * @date 2026-07-18
 * @details 每段绘制 5 条等距垂直网格线 + 段中央 1 条光带 + 段边界 2 条亮线。
 *          3 层发光：网格线深青 / 光带亮青 / 边界白色高能爆亮。
 *          单 VAO/VBO 内分两段 draw：GL_LINES（网格+边界）+ GL_TRIANGLE_STRIP（光带）。
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

/// @brief 网格光带顶点（8 floats：pos2 + normal2 + localU1 + localV1 + color3 + alpha1）
struct GridVertex {
    float x = 0.0f, y = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;
    float localU = 0.0f;   ///< 沿光带方向 [0,1]
    float localV = 0.0f;   ///< 沿带宽方向 [0,1]
};

class BorderGeometry;

/**
 * @class GridBeamEffect
 * @brief 协奏子模式 4：网格光带
 * @details 每段沿边框排列 5 条等距网格线 + 1 条段中央光带（按 zPulse 控制长度）+
 *          2 条段边界亮线。3 层发光叠加呈现科技感网格 HUD 风格。
 */
class GridBeamEffect : public IConcertoEffect {
public:
    GridBeamEffect();
    ~GridBeamEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "GridBeamEffect"; }
    int activeParticles() const override { return m_lineVertCount / 2; }

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
    int m_locTime = -1, m_locAudioColor = -1, m_locMusicColorEnabled = -1;

    const BorderGeometry* m_geometry = nullptr;
    int m_screenW = 2560, m_screenH = 1440;
    float m_time = 0.0f;
    float m_audioColor[3] = {1.0f, 1.0f, 1.0f};
    bool m_musicColorEnabled = false;

    std::vector<float> m_segmentEnergy;
    std::vector<SegmentRole> m_segmentRole;
    int m_segmentCount = 0;

    std::vector<GridVertex> m_lineVertices;    ///< GL_LINES 用（网格+边界）
    std::vector<GridVertex> m_stripVertices;    ///< GL_TRIANGLE_STRIP 用（光带）
    int m_lineVertCount = 0;
    int m_stripVertCount = 0;
    int m_maxLineVerts = 0;
    int m_maxStripVerts = 0;

    std::array<float, 16> m_cachedProj{};
    float m_cachedSW = -1.0f, m_cachedSH = -1.0f;

    static const char* vertexSource();
    static const char* fragmentSource();
};
