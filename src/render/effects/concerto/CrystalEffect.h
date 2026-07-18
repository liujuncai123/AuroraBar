/**
 * @file CrystalEffect.h
 * @brief 协奏子模式 0：晶柱升级
 * @date 2026-07-18
 * @details 沿边框排列六边形棱柱，3 层发光（柱身渐变 + 顶部高光 + 底部柔光）。
 *          替代旧 LightColumnEffect 晶柱模式的 9 层叠加 + ACES。
 *          用 Reinhard 色调映射保留饱和度。
 * @note 线程安全：渲染线程独占，不跨线程共享。
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

/// @brief 晶柱顶点（11 floats：pos3 + normal3 + localH1 + color3 + alpha1）
struct CrystalVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float localH = 0.0f;     ///< 柱内高度 [0,1]
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;
};

class BorderGeometry;

/**
 * @class CrystalEffect
 * @brief 协奏子模式 0：晶柱升级
 * @details 沿屏幕四边排列六边形棱柱，按段能量决定柱高；
 *          3 层发光叠加（柱身底冷→顶白渐变 + 顶部高光 + 底部柔光）；
 *          Reinhard 色调映射替代 ACES，保留饱和度避免发灰。
 * @note 安全：所有 segIdx 越界检查；emaValue 范围 clamp；GL 资源 RAII 释放。
 */
class CrystalEffect : public IConcertoEffect {
public:
    CrystalEffect();
    ~CrystalEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "CrystalEffect"; }
    int activeParticles() const override { return m_crystalVertCount / 54; }

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

    std::vector<float> m_segmentEnergy;   ///< 每段能量 [0,1]
    std::vector<SegmentRole> m_segmentRole;
    int m_segmentCount = 0;
    int m_columnsPerSegment = 10;

    std::vector<CrystalVertex> m_vertices;
    int m_crystalVertCount = 0;
    int m_crystalMaxVerts = 0;

    std::array<float, 16> m_cachedProj{};
    float m_cachedSW = -1.0f, m_cachedSH = -1.0f;

    static const char* vertexSource();
    static const char* fragmentSource();
};
