/**
 * @file PulseRingEffect.h
 * @brief 协奏子模式 7：脉冲环
 * @date 2026-07-18
 * @details 段能量峰值超阈值时从段中心发射扩散环，半径以 200px/s 扩散，
 *          alpha 从 1.0 衰减。3 层发光：环主线 / 环外辉光 / 环内柔光。
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

/// @brief 脉冲环顶点（8 floats：pos2 + color3 + alpha1 + radiusInfo1 + edgeFactor1）
struct RingVertex {
    float x = 0.0f, y = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;
    float radiusNorm = 0.0f;   ///< [0,1] 半径归一化（基于最大半径）
    float edgeFactor = 1.0f;   ///< 边缘加权（1=主线，0.3=辉光）
};

class BorderGeometry;

/**
 * @class PulseRingEffect
 * @brief 协奏子模式 7：脉冲环
 * @details 每段能量峰值 > 0.5 时发射扩散环，每 200ms 检测一次；
 *          环以 200px/s 扩散，寿命 1.0s。环主线 + 外圈辉光 2 层发光。
 */
class PulseRingEffect : public IConcertoEffect {
public:
    PulseRingEffect();
    ~PulseRingEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "PulseRingEffect"; }
    int activeParticles() const override { return static_cast<int>(m_rings.size()); }

    Result<void> compileShaders() override;

    void setSegmentEnergy(int segIdx, float emaValue) override;
    void setSegmentRole(int segIdx, SegmentRole role) override;
    void setTime(float t) override;
    void setScreenSize(int w, int h) override { m_screenW = w; m_screenH = h; }
    void setBorderGeometry(const BorderGeometry* geo) override { m_geometry = geo; }
    void setAudioColor(const float rgb[3]) override;

private:
    struct Ring {
        float cx = 0.0f, cy = 0.0f;   ///< 环中心（屏幕像素）
        float radius = 5.0f;          ///< 当前半径
        float age = 0.0f;             ///< 已存活秒数
        float life = 1.0f;            ///< 寿命秒数
        float r = 1.0f, g = 1.0f, b = 1.0f;
    };

    static unsigned compileShader(unsigned type, const char* src);
    void updateRings(float dt);
    void spawnRings(float dt);
    void buildVertices();

    unsigned m_vao = 0, m_vbo = 0, m_program = 0;
    int m_locProjection = -1, m_locScreenW = -1, m_locScreenH = -1;
    int m_locAudioColor = -1, m_locMusicColorEnabled = -1;

    const BorderGeometry* m_geometry = nullptr;
    int m_screenW = 2560, m_screenH = 1440;
    float m_time = 0.0f;
    float m_prevTime = -1.0f;
    float m_dt = 0.0f;
    float m_spawnTimer = 0.0f;
    float m_audioColor[3] = {1.0f, 1.0f, 1.0f};
    bool m_musicColorEnabled = false;

    std::vector<float> m_segmentEnergy;
    std::vector<SegmentRole> m_segmentRole;
    std::vector<float> m_lastEnergy;   ///< 上次能量值，用于峰值检测
    int m_segmentCount = 0;
    int m_maxRings = 0;

    std::vector<Ring> m_rings;
    std::vector<RingVertex> m_vertices;
    int m_vertCount = 0;
    int m_maxVerts = 0;

    std::array<float, 16> m_cachedProj{};
    float m_cachedSW = -1.0f, m_cachedSH = -1.0f;

    static const char* vertexSource();
    static const char* fragmentSource();
};
