/**
 * @file ParticleEffect.h
 * @brief 粒子流视觉效果
 * @date 2026-07-06
 * @details 基于点精灵（glDrawArrays GL_POINTS）的粒子效果。
 *          每个粒子沿边框路径运动，有生命周期，死亡后回收到对象池。
 *          着色器：径向渐变 + 生命衰减 = 软发光粒子。
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

/// @brief 粒子数据（32 字节，GPU 友好）
struct Particle {
    float position = 0.0f;       ///< 边框归一化位置 [0,1]
    float velocity = 0.0f;       ///< 沿边框速度
    float life     = 0.0f;       ///< 剩余生命（秒）；0=死亡
    float size     = 4.0f;       ///< 粒子大小（像素）
    float r = 1.0f, g = 1.0f, b = 1.0f;  ///< RGB 颜色
    float totalDistance = 0.0f;  ///< 累计行进距离（像素），超限即死亡

    bool isDead() const { return life <= 0.0f; }
};

static_assert(sizeof(Particle) == 32, "Particle must be 32 bytes for GPU alignment");

/**
 * @class ParticleEffect
 * @brief 粒子效果实现
 * @details 管理粒子生命周期：emit（生成）→ update（衰减+移动）→ kill（回收）。
 *          使用预分配数组（对象池），零堆分配。
 *          渲染：glDrawArrays(GL_POINTS, 0, activeCount)。
 */
class ParticleEffect : public IEffect {
public:
    ParticleEffect();
    ~ParticleEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void update(float dt) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "ParticleEffect"; }
    int activeParticles() const override { return m_activeCount; }

    /// @brief 生成一个新粒子
    bool spawn(float position, float velocity, float life, float size,
               float r, float g, float b);

    /// @brief 编译着色器（GL 上下文就绪后调用）
    Result<void> compileShaders();

    /// @brief 更新边框采样点 uniform（每帧/分辨率变化时调用）
    void updateBorderUniform(const class BorderGeometry& geo);
    void setScreenPerimeter(const class BorderGeometry& geo);

    /// @brief 更新着色器时间 uniform（每帧调用）
    void setTime(float t) {
        if (m_program) {
            glUseProgram(m_program);
            glUniform1f(m_locTime, t);
        }
    }

    /// @brief 更新各段振荡幅度 uniform（16 段，音频频段驱动）
    void setOscillationAmps(const float amps[16]) {
        if (m_program && m_locOscAmp >= 0) {
            glUseProgram(m_program);
            glUniform1fv(m_locOscAmp, 16, amps);
        }
    }

    /// @brief 设置粒子横向散布宽度
    void setSpreadWidth(float w) {
        if (m_program && m_locSpreadWidth >= 0) {
            glUseProgram(m_program);
            glUniform1f(m_locSpreadWidth, w);
        }
    }

    /// @brief 设置粒子最大行进距离（像素），超限即死亡
    void setMaxDistance(float dist) { m_maxDistance = dist; }

    /// @brief 字节大小
    static constexpr size_t particleSize = sizeof(Particle);

    /// @brief 粒子池容量
    int capacity() const { return static_cast<int>(m_particles.size()); }

    /// @brief 动态调整粒子池大小（保留现有活跃粒子，重分配 VBO）
    void resize(int newCapacity);

private:

    /// @brief 编译单个着色器阶段
    static unsigned compileShader(unsigned type, const char* src);

    /// @brief 回收死粒子
    void recycleDead();

    // 粒子数据
    std::vector<Particle> m_particles;    ///< 粒子池（预分配）
    int m_activeCount = 0;                ///< 活跃粒子数
    float m_screenPerimeter = 8000.0f;    ///< 屏幕周长（px），用于距离累计
    float m_maxDistance = 500.0f;         ///< 粒子最大行进距离（px），超限即死亡

    // OpenGL 资源
    unsigned m_vao = 0;
    unsigned m_vbo = 0;
    unsigned m_program = 0;
    unsigned m_vertShader = 0;
    unsigned m_fragShader = 0;

    // uniform locations
    int m_locProjection = -1;
    int m_locScreenW = -1;
    int m_locScreenH = -1;
    int m_locCount = -1;
    int m_locBorderNormal = -1;
    int m_locTime = -1;
    int m_locOscAmp = -1;
    int m_locSpreadWidth = -1;

    // 着色器源码（定义在 .cpp 中）
    static const char* vertexSource();
    static const char* fragmentSource();

    // 缓存上次投影矩阵（避免每帧重复 glUniformMatrix4fv）
    std::array<float, 16> m_cachedProj{};
};
