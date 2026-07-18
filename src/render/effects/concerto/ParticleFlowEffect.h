/**
 * @file ParticleFlowEffect.h
 * @brief 协奏子模式 3：粒子流
 * @date 2026-07-18
 * @details 沿边框周长方向流动的粒子：核心 4px + 光晕 12px（2 层）。
 *          每段按 energy*50/s 速率 spawn 粒子，速度 = flowSpeed * 0.1（perim/s），
 *          生命周期 2s，过期回收。
 *          颜色按段角色：HarmonicMain 暖橙，PercussiveMain 青紫。
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

/// @brief 粒子流顶点（8 floats：pos2 + color4 + size1 + ageNorm1）
struct ParticleFlowVertex {
    float x = 0.0f, y = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    float size = 4.0f;
    float ageNorm = 0.0f;   ///< [0,1]，0=新生 1=将死
};

class BorderGeometry;

/**
 * @class ParticleFlowEffect
 * @brief 协奏子模式 3：粒子流
 * @details 沿边框周长方向流动的粒子系统。spawn 速率与段能量成正比；
 *          每个粒子按 flowSpeed * 0.1 沿周长匀速移动，2s 后消亡。
 *          渲染时启用 GL_PROGRAM_POINT_SIZE，粒子大小 = size（核心4px / 光晕12px）。
 */
class ParticleFlowEffect : public IConcertoEffect {
public:
    ParticleFlowEffect();
    ~ParticleFlowEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "ParticleFlowEffect"; }
    int activeParticles() const override { return static_cast<int>(m_particles.size()); }

    Result<void> compileShaders() override;

    void setSegmentEnergy(int segIdx, float emaValue) override;
    void setSegmentRole(int segIdx, SegmentRole role) override;
    void setTime(float t) override;
    void setScreenSize(int w, int h) override { m_screenW = w; m_screenH = h; }
    void setBorderGeometry(const BorderGeometry* geo) override { m_geometry = geo; }
    void setAudioColor(const float rgb[3]) override;

private:
    struct Particle {
        float perimPos = 0.0f;     ///< 在周长上的位置 [0,1]
        float speed    = 0.0f;     ///< 周长方向速度（perim/s）
        float age      = 0.0f;     ///< 已存活秒数
        float life     = 2.0f;     ///< 寿命秒数
        float r = 1.0f, g = 1.0f, b = 1.0f;
    };

    static unsigned compileShader(unsigned type, const char* src);
    void updateParticles(float dt);
    void spawnParticles(float dt);
    void buildVertices();

    unsigned m_vao = 0, m_vbo = 0, m_program = 0;
    int m_locProjection = -1, m_locScreenW = -1, m_locScreenH = -1;
    int m_locAudioColor = -1, m_locMusicColorEnabled = -1;

    const BorderGeometry* m_geometry = nullptr;
    int m_screenW = 2560, m_screenH = 1440;
    float m_time = 0.0f;
    float m_prevTime = -1.0f;
    float m_dt = 0.0f;
    float m_flowSpeed = 0.4f;
    float m_audioColor[3] = {1.0f, 1.0f, 1.0f};
    bool m_musicColorEnabled = false;

    std::vector<float> m_segmentEnergy;
    std::vector<SegmentRole> m_segmentRole;
    std::vector<float> m_spawnAccum;   ///< 每段 spawn 累积器
    int m_segmentCount = 0;
    int m_maxParticles = 0;

    std::vector<Particle> m_particles;
    std::vector<ParticleFlowVertex> m_vertices;
    int m_vertCount = 0;
    int m_maxVerts = 0;

    static constexpr float kParticleSpawnRate = 50.0f;  ///< 粒子生成速率（粒子/秒/段，满能量时）

    std::array<float, 16> m_cachedProj{};
    float m_cachedSW = -1.0f, m_cachedSH = -1.0f;

    static const char* vertexSource();
    static const char* fragmentSource();
};
