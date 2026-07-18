/**
 * @file IEffect.h
 * @brief 视觉效果抽象接口
 * @date 2026-07-06
 * @details 定义视觉效果的统一接口："长什么样"（粒子/光流/液体）。
 *          每个效果负责自己的着色器、粒子数据、渲染调用。
 * @note 线程安全：渲染线程独占。
 */

#pragma once

#include "../../core/Result.h"
#include "../Camera.h"
#include <functional>

class IEffect {
public:
    virtual ~IEffect() = default;

    /**
     * @brief 初始化效果（编译着色器、分配缓冲）
     * @param glInitFn 在 GL 上下文就绪后调用的初始化函数
     * @param maxParticles 最大粒子数
     * @return 成功 Ok()；失败 Error
     */
    virtual Result<void> initialize(std::function<void()> glInitFn,
                                    int maxParticles) = 0;

    /**
     * @brief 更新粒子状态（每帧）
     * @param dt 帧间隔
     */
    virtual void update(float /*dt*/) {}

    /**
     * @brief 渲染当前帧
     * @param camera 相机投影矩阵
     */
    virtual void render(const Camera& /*camera*/) {}

    /// @brief 清理 GL 资源
    virtual void cleanup() = 0;

    /// @brief 效果名称
    virtual const char* name() const = 0;

    /// @brief 当前活跃粒子数
    virtual int activeParticles() const { return 0; }
};
