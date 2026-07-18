/**
 * @file SpectrumBarEffect.h
 * @brief 协奏子模式 1：极简频谱柱（Apple Music 风格）
 * @date 2026-07-18
 * @details 沿边框排列极细矩形条，纯色无光晕。
 *          3 层：主体渐变 + 顶部 1px 高亮 + 底部反射阴影。
 *          残影机制：柱顶位置在 200ms 内缓慢回落（peakHistory 衰减）。
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

/// @brief 频谱柱顶点（7 floats：pos2 + localY1 + color3 + alpha1）
struct SpectrumBarVertex {
    float x = 0.0f, y = 0.0f;        ///< 屏幕像素坐标
    float localY = 0.0f;             ///< [0,1]：0=底部 1=顶部
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;
};

class BorderGeometry;

/**
 * @class SpectrumBarEffect
 * @brief 协奏子模式 1：极简频谱柱
 * @details 沿屏幕四边排列细矩形条；3 层发光（主体渐变 + 顶部高亮 + 底部反射）；
 *          残影机制：每段柱顶位置在 200ms 内缓慢回落，呈现"频谱拖尾"感。
 * @note 安全：所有 segIdx 越界检查；emaValue 范围 clamp；GL 资源 RAII 释放。
 */
class SpectrumBarEffect : public IConcertoEffect {
public:
    SpectrumBarEffect();
    ~SpectrumBarEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "SpectrumBarEffect"; }
    int activeParticles() const override { return m_vertCount / 6; }

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
    int m_locAudioColor = -1, m_locMusicColorEnabled = -1;

    const BorderGeometry* m_geometry = nullptr;
    int m_screenW = 2560, m_screenH = 1440;
    float m_time = 0.0f;
    float m_audioColor[3] = {1.0f, 1.0f, 1.0f};
    bool m_musicColorEnabled = false;

    std::vector<float> m_segmentEnergy;
    std::vector<SegmentRole> m_segmentRole;
    std::vector<float> m_peakHistory;  ///< 残影：每段柱顶历史最大值
    int m_segmentCount = 0;
    int m_columnsPerSegment = 10;

    std::vector<SpectrumBarVertex> m_vertices;
    int m_vertCount = 0;
    int m_maxVerts = 0;

    std::array<float, 16> m_cachedProj{};
    float m_cachedSW = -1.0f, m_cachedSH = -1.0f;

    static const char* vertexSource();
    static const char* fragmentSource();
};
