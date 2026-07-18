/**
 * @file LaserSweepEffect.h
 * @brief 协奏子模式 5：激光线扫
 * @date 2026-07-18
 * @details 沿边框周长循环扫描的激光线：1px 主线 + 拖尾（渐变 alpha）+ 头部白点。
 *          3 层发光叠加呈现科幻扫描雷达风格。
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

/// @brief 激光顶点（7 floats：pos2 + color3 + alpha1 + along1）
struct LaserVertex {
    float x = 0.0f, y = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;
    float along = 0.0f;     ///< 在扫描线上的位置 [0,1]，0=尾部 1=头部
};

class BorderGeometry;

/**
 * @class LaserSweepEffect
 * @brief 协奏子模式 5：激光线扫
 * @details 沿屏幕四边周长循环扫描：扫描位置 = fmod(time * 0.5, 1.0)，
 *          拖尾长度 = 整体能量 * 300px，颜色品红 #FF00AA，头部白点。
 */
class LaserSweepEffect : public IConcertoEffect {
public:
    LaserSweepEffect();
    ~LaserSweepEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "LaserSweepEffect"; }
    int activeParticles() const override { return 1; }

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
    int m_segmentCount = 0;

    std::vector<LaserVertex> m_lineVertices;   ///< GL_LINE_STRIP（主线 + 拖尾）
    std::vector<LaserVertex> m_pointVertices;   ///< GL_POINTS（头部）
    int m_lineVertCount = 0;
    int m_pointVertCount = 0;
    int m_maxLineVerts = 0;
    int m_maxPointVerts = 0;

    static constexpr int kLaserTrailMaxVerts = 100;  ///< 激光拖尾最大顶点数

    std::array<float, 16> m_cachedProj{};
    float m_cachedSW = -1.0f, m_cachedSH = -1.0f;

    static const char* vertexSource();
    static const char* fragmentSource();
};
