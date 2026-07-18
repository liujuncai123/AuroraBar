/**
 * @file ConcertoRenderer.h
 * @brief 协奏曲模式渲染器——独立 Effect 架构
 * @date 2026-07-18
 * @details 重写后：不再生成顶点，只负责 EMA 平滑 + Effect 调度。
 *          持有 std::unique_ptr<IConcertoEffect>，subMode 切换时由
 *          ConcertoEffectFactory 创建新实例。
 *          顶点生成下沉到各 Effect 内部，Renderer 只推送段能量/时间/颜色。
 * @note 线程安全：渲染线程独占。
 */

#pragma once

#include "IModeRenderer.h"
#include "../effects/concerto/IConcertoEffect.h"
#include "../effects/concerto/ConcertoEffectFactory.h"
#include "../../core/Result.h"
#include "../../segmentation/SegmentationManager.h"
#include <vector>
#include <functional>
#include <memory>

class BorderGeometry;
class Camera;

/**
 * @class ConcertoRenderer
 * @brief 协奏曲模式渲染器
 * @details 持有 IConcertoEffect 负责渲染，自己负责每段一阶 EMA 平滑和 Effect 调度。
 *          每帧：消费 SegmentParam 指令 → EMA 平滑 → 推送给 Effect → Effect.render()
 */
class ConcertoRenderer : public IModeRenderer {
public:
    ConcertoRenderer();
    ~ConcertoRenderer() override;

    Result<void> initialize(const BorderGeometry* geometry,
                            const Camera* camera,
                            int maxParticles,
                            std::function<void()> glInitFn) override;

    void pushCommand(const RenderCommand& cmd) override;
    void renderFrame(double dt) override;

    const char* name() const override { return "Concerto"; }
    int activeParticles() const override {
        return m_effect ? m_effect->activeParticles() : 0;
    }

    /// @brief 编译着色器（GL 上下文就绪后调用，已下沉到 switchSubMode 内部）
    Result<void> compileShaders();

    /// @brief 更新边框几何引用
    void updateBorder(const BorderGeometry& geo);

    /// @brief 清理 GL 资源
    /// @note 安全：显式 cleanup 供 RenderThread::switchMode 调用
    void cleanup() override {
        if (m_effect) m_effect->cleanup();
    }

private:
    /// @brief 切换子模式（旧 Effect cleanup → 工厂创建新实例 → 初始化）
    /// @return true 切换成功；false 切换失败（已回退到 subMode 0）
    /// @note 安全：失败时自动回退到 subMode 0；subMode 0 也失败则返回 false
    bool switchSubMode(int newSubMode);

    const BorderGeometry* m_geometry = nullptr;
    const Camera* m_camera = nullptr;
    std::unique_ptr<IConcertoEffect> m_effect;  ///< 当前激活的 Effect 实例
    int m_currentSubMode = -1;                  ///< 跟踪当前 subMode，用于切换检测

    std::function<void()> m_glInitFn;            ///< GL 初始化回调（供 switchSubMode 用）
    int m_maxParticles = 0;                      ///< 最大粒子数（供 switchSubMode 用）

    std::vector<double> m_zPulses;              ///< 每段当前 z 轴脉冲值（一阶 EMA 平滑）
    std::vector<double> m_rawTargets;           ///< 每段当前音频目标值（SegmentParam）
    int m_segmentCount = 0;

    double m_followSpeed = 15.0;                ///< EMA 跟随速度（越大越灵敏）
    float m_time = 0.0f;
    double m_dormantCoeff = 1.0;
};
