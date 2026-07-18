/**
 * @file IModeRenderer.h
 * @brief 模式渲染抽象接口
 * @date 2026-07-06
 * @details 定义渲染模式的统一接口："怎么动"（循环/协奏）。
 *          每个模式持有 IEffect* 控制"长什么样"（粒子/光流）。
 *          模式与效果解耦 = 切换模式不改效果代码，反之亦然。
 * @note 线程安全：渲染线程独占，不跨线程共享。
 */

#pragma once

#include "../../core/Result.h"
#include "../../core/CommandTypes.h"  // RenderCommand
#include <cstdint>
#include <functional>

// 前向声明
class BorderGeometry;
class Camera;

/**
 * @class IModeRenderer
 * @brief 渲染模式接口
 * @details 处理 RenderCommand 队列中的指令，更新粒子/效果状态，执行帧渲染。
 *          循环态沿边框流转；协奏态（V2）在各段独立跳动。
 */
class IModeRenderer {
public:
    virtual ~IModeRenderer() = default;

    /**
     * @brief 初始化渲染模式
     * @param geometry  边框几何数据
     * @param camera    相机投影矩阵
     * @param maxParticles  最大粒子数
     * @return 成功 Ok()；失败 Error
     */
    virtual Result<void> initialize(const BorderGeometry* geometry,
                                    const Camera* camera,
                                    int maxParticles,
                                    std::function<void()> glInitFn) = 0;

    /**
     * @brief 处理一个渲染指令
     * @param cmd 从 g_renderQueue 接收的指令
     */
    virtual void pushCommand(const RenderCommand& cmd) = 0;

    /**
     * @brief 渲染一帧
     * @param dt 帧间隔（秒）
     */
    virtual void renderFrame(double dt) = 0;

    /**
     * @brief 渲染粒子层（仅粒子模式 CycleRenderer 真正实现，其他模式默认空操作）
     * @param camera 相机引用
     * @note 安全：替代原 RenderFrameSEH 中的 static_cast<CycleRenderer*>，
     *              避免模式切换竞态导致类型不匹配的未定义行为
     */
    virtual void renderParticles(const Camera& camera) { (void)camera; }

    /**
     * @brief 清理 GL 资源（shader/VBO/VAO）
     * @note 安全：必须支持在上下文丢失时调用（内部应检查 wglGetCurrentContext）
     */
    virtual void cleanup() {}

    /// @brief 模式名称
    virtual const char* name() const = 0;

    /// @brief 当前活跃粒子数
    virtual int activeParticles() const = 0;
};
