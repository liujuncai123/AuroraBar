// ============================================================================
// ⚠️  DEPRECATED — 本文件已废弃，请勿在新代码中使用
// ----------------------------------------------------------------------------
//  原方案：基于 OverlayWindow（Win32 原生透明窗口）的独立渲染线程
//  原因  ：依赖的 OverlayWindow/SubmitThread 已废弃，整体 Win32 方案因 Win+D
//          / 最小化黑屏问题无法根治而放弃。
//  替代  ：将迁移到 Qt6 方案 —— 基于 QtOverlayWindow + QTimer 驱动渲染
//          （CycleRenderer/BounceBallRenderer/ConcertoRenderer 逻辑会复用）。
//  说明  ：文件保留供历史参考，不再参与主代码路径；新代码请勿 include。
//          Camera/BorderGeometry/ColorScheme/modes/* 等仍可继续使用。
// ============================================================================
#ifdef _MSC_VER
#pragma message("warning: " __FILE__ " is DEPRECATED — will be replaced by Qt-based renderer")
#endif

/**
 * @file RenderThread.h
 * @brief 渲染线程——OverlayWindow 透明边框 OpenGL 渲染
 * @date 2026-07-06
 * @details 使用 OverlayWindow（WS_EX_LAYERED + TOPMOST + TRANSPARENT）
 *          实现桌面叠加层效果。支持模式切换（Cycle ↔ BounceBall）。
 *          v1.2: unique_ptr<IModeRenderer> 支持运行时模式切换
 */
#pragma once
#include "../core/ThreadBase.h"
#include "../core/SPSCQueue.h"
#include "../logging/LoggerManager.h"
#include <chrono>
#include <atomic>

// 直接声明 OutputDebugStringA，避免 windows.h 的 min/max 宏污染
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
#include "../core/CommandTypes.h"
#include "../segmentation/SegmentationManager.h"
#include "../params/ParamStore.h"
#include "Camera.h"
#include "BorderGeometry.h"
#include "ColorScheme.h"
#include "modes/IModeRenderer.h"
#include "modes/CycleRenderer.h"
#include "modes/BounceBallRenderer.h"
#include "modes/ConcertoRenderer.h"
#include "../ui/OverlayWindow.h"
#include <GL/wglew.h>  // 安全：必须在 OverlayWindow.h（含 <GL/glew.h>）之后
#include <memory>

template<typename T, size_t C> class SPSCQueue;
extern SPSCQueue<RenderCommand, 64> g_renderQueue;

// 前向声明：SEH 保护的函数（实现在 RenderFrameSEH.cpp，独立编译单元避免 MSVC C2712）
void RenderFrameSEH_Impl(OverlayWindow& window, Camera& camera,
                         IModeRenderer* renderer, bool needParticles,
                         std::atomic<bool>* pContextLost);
void RenderThread_onRun_SEH(class RenderThread* self);

class RenderThread : public ThreadBase {
public:
    RenderThread() : ThreadBase("Render") {}

    void setBorders(int top, int bottom, int left, int right) {
        m_borderTop = top; m_borderBottom = bottom;
        m_borderLeft = left; m_borderRight = right;
    }

protected:
    Result<void> onInitialize() override {
        AURORA_INFO("Render", "onInitialize()");
        if (!m_window.create(m_borderTop, m_borderBottom, m_borderLeft, m_borderRight))
            return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, "OverlayWindow failed"));
        m_window.setVisible(true);

        m_camera.setScreenSize(m_window.screenW(), m_window.screenH());
        m_camera.setCurvature(0.5);

        BorderConfig cfg{m_borderTop, m_borderBottom, m_borderLeft, m_borderRight, 40};
        m_geometry.compute(cfg, m_window.screenW(), m_window.screenH(), 200);

        // 直接读取已保存的模式，创建对应渲染器（不再默认粒子模式）
        int savedMode = ParamStore::Instance().GetInt("mode");
        AURORA_INFO("Render", "onInitialize() savedMode={}", savedMode);
        switchMode(savedMode);

        AURORA_INFO("Render", "onInitialize() OK {}x{}", m_window.screenW(), m_window.screenH());
        return Result<void>::Ok();
    }

    void onRun() override {
        RenderThread_onRun_SEH(this);
        if (m_contextLost) {
            AURORA_ERROR("Render", "SEH caught crash in onRun, scheduling recovery");
        }
    }

public:
    /// @brief onRun 的实际逻辑体（被 SEH 包装调用，避免 C2712）
    void onRunBody() {
        m_frameCount++;
        auto frameStart = std::chrono::steady_clock::now();

        // ════════════════════════════════════════════════════════════
        // Phase 1: 上下文健康检查 + 自动恢复
        //   m_contextLost: SEH 捕获帧中崩溃时置 true
        //   isContextValid(): wglMakeCurrent 检测返回 false 时触发
        // ════════════════════════════════════════════════════════════
        // 安全：DWM 合成模式切换过渡期（WM_DWMCOMPOSITIONCHANGED 触发，100ms）
        //       跳过本帧渲染（防御性措施），不触发恢复（不隐藏/显示窗口）。
        //       Win+D 不触发此路径（不发送 WM_DWMCOMPOSITIONCHANGED）。
        //       SubmitThread 独立处理 ULW（DWM 切换时阻塞，恢复后自动返回）。
        if (m_window.isDwmTransitioning()) {
            return;
        }

        if (m_contextLost || !m_window.isContextValid()) {
            // 安全：上下文丢失时立即隐藏叠加层，防止黑屏覆盖整个桌面
            //       关闭最后一个窗口 / 切换显示模式 / GPU TDR 都会触发此路径
            m_window.setVisible(false);
            AURORA_WARN("Render", "Overlay hidden — context lost, recovering...");

            // 安全：先重建 GL 上下文，再销毁旧渲染器。
            //       rebuildContext() 内部通过 wglDeleteContext 自动清理旧 GL 对象。
            //       重建后先 detachContext() 再 m_renderer.reset()，
            //       防止旧渲染器的 cleanup() 在新上下文中误删 GL 对象。
            if (m_window.rebuildContext()) {
                // 安全：重建成功后先解除上下文绑定，再销毁旧渲染器。
                //       旧渲染器的 cleanup() 中会调用 glDelete*，如果在新上下文中执行，
                //       会用旧上下文的 GL 对象 ID 误删新上下文中的有效资源。
                //       detachContext() 后 wglGetCurrentContext() 返回 nullptr，
                //       旧渲染器的 cleanup() 跳过所有 glDelete* 调用。
                //       wglDeleteContext 已自动清理旧上下文中的所有 GL 对象。
                m_window.detachContext();
                m_renderer.reset();
                // 重新绑定新上下文，创建新渲染器
                m_window.makeCurrent();
                switchMode(m_currentMode);
                m_camera.setScreenSize(m_window.screenW(), m_window.screenH());
                BorderConfig cfg{m_borderTop, m_borderBottom, m_borderLeft, m_borderRight, 40};
                m_geometry.compute(cfg, m_window.screenW(), m_window.screenH(), 200);
                m_contextLost = false;
                m_recoverFailCount = 0;  // 恢复成功，重置失败计数
                // 恢复后重新显示叠加层
                m_window.setVisible(true);
                AURORA_INFO("Render", "Overlay restored — context recovered, mode={}", m_currentMode);
            } else {
                // 安全：连续失败退避策略，避免 GPU 切换期间快速重试耗尽 CPU
                //       1-3 次：100ms（快速恢复尝试）
                //       4-10 次：500ms（GPU 切换中）
                //       >10 次：2000ms（持续异常，避免空转）
                m_recoverFailCount++;
                int sleepMs = (m_recoverFailCount <= 3) ? 100 :
                              (m_recoverFailCount <= 10) ? 500 : 2000;
                AURORA_ERROR("Render", "Context recovery FAILED #{}, sleep {}ms, will retry",
                             m_recoverFailCount, sleepMs);
                m_contextLost = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                if (!shouldRun()) return;
                return;
            }
        }

        // ════════════════════════════════════════════════════════════
        // Phase 2: 处理渲染命令（不涉及 GL 操作）
        // ════════════════════════════════════════════════════════════
        RenderCommand cmd;
        while (g_renderQueue.tryPop(cmd)) {
            switch (cmd.type) {
            case RenderCommand::Type::ModeChange:
                switchMode(static_cast<int>(cmd.paramValue));
                break;
            case RenderCommand::Type::BorderConfig: {
                std::string name(cmd.paramName.data());
                if (name == "top")    m_borderTop    = static_cast<int>(cmd.paramValue);
                if (name == "bottom") m_borderBottom = static_cast<int>(cmd.paramValue);
                if (name == "left")   m_borderLeft   = static_cast<int>(cmd.paramValue);
                if (name == "right")  m_borderRight  = static_cast<int>(cmd.paramValue);

                BorderConfig cfg{m_borderTop, m_borderBottom, m_borderLeft, m_borderRight, 40};
                m_geometry.compute(cfg, m_window.screenW(), m_window.screenH(), 200);
                if (auto* cr = dynamic_cast<CycleRenderer*>(m_renderer.get()))
                    cr->updateBorder(m_geometry);
                else if (auto* bb = dynamic_cast<BounceBallRenderer*>(m_renderer.get()))
                    bb->updateBorder(m_geometry);
                else if (auto* co = dynamic_cast<ConcertoRenderer*>(m_renderer.get()))
                    co->updateBorder(m_geometry);
                AURORA_INFO("Render", "BorderConfig t={} b={} l={} r={}",
                            m_borderTop, m_borderBottom, m_borderLeft, m_borderRight);
                break;
            }
            case RenderCommand::Type::OverlayVisible:
                m_window.setVisible(cmd.paramValue > 0.5);
                AURORA_INFO("Render", "OverlayVisible={}", cmd.paramValue > 0.5);
                break;
            default:
                if (std::strcmp(cmd.paramName.data(), "curvatureDepth") == 0) {
                    m_camera.setCurvature(cmd.paramValue);
                    break;
                }
                // 安全：m_renderer 可能为 nullptr
                //   触发场景：1) savedMode 非 0/1/2 时 switchMode 不创建渲染器
                //             2) onInitialize 阶段命令已入队但渲染器未就绪
                //             3) 模式切换失败后 m_renderer 被 reset
                //   不检查直接解引用会导致 0xc0000005 访问违规（与 crash.log 吻合）
                if (m_renderer) {
                    m_renderer->pushCommand(cmd);
                } else {
                    AURORA_WARN("Render", "pushCommand dropped — renderer null, cmdType={}",
                                static_cast<int>(cmd.type));
                }
                break;
            }
        }

        // ════════════════════════════════════════════════════════════
        // Phase 3: 渲染前 makeCurrent
        //   安全：makeCurrent 失败时区分"临时失败"和"真正丢失"：
        //     - 临时失败（DWM 切换）：跳过本帧，不隐藏窗口，DWM 恢复后成功
        //     - 真正丢失（连续失败 >30 帧/约500ms）：设置 m_contextLost，下帧重建
        //   原因：wglMakeCurrent 在 DWM 切换（Win+D/最小化）后可能临时失败，
        //         如果立即设置 m_contextLost 会触发 setVisible(false) → 黑屏！
        // ════════════════════════════════════════════════════════════
        if (!m_window.makeCurrent()) {
            m_makeCurrentFailCount++;
            if (m_makeCurrentFailCount > 30) {
                // 连续失败超过 30 帧（约 500ms），认为上下文真正丢失
                AURORA_ERROR("Render", "makeCurrent failed {} times — context truly lost",
                             m_makeCurrentFailCount);
                m_contextLost = true;
            } else {
                AURORA_WARN("Render", "makeCurrent failed #{} — skipping frame (DWM transition?)",
                            m_makeCurrentFailCount);
            }
            return;  // 跳过本帧
        }
        m_makeCurrentFailCount = 0;  // 成功，重置计数器

        // ════════════════════════════════════════════════════════════
        // Phase 4: SEH 保护的 OpenGL 渲染块
        //   提取到 renderFrameSEH() 避免 MSVC C2712（见下文实现）
        //   __try 捕获"僵尸上下文"导致 GL 调用崩溃的 <1% 场景
        // ════════════════════════════════════════════════════════════
        renderFrameSEH();

        if (!shouldRun()) return;

        // ════════════════════════════════════════════════════════════
        // Phase 5: 帧时间感知的帧率控制
        // ════════════════════════════════════════════════════════════
        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart).count();
        int fps = ParamStore::Instance().GetInt("targetFps");
        int targetMs = (fps > 0 && fps <= 360) ? (1000 / fps) : 16;
        int remaining = targetMs - static_cast<int>(elapsed);
        if (remaining > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining));
        }
    }

    void onCleanup() override {
        // 安全：makeCurrent() 可能因上下文丢失而失败，
        //       此时跳过 GL 清理（驱动已自动回收），仅释放 C++ 资源
        if (m_window.makeCurrent()) {
            if (m_renderer) {
                // 安全：统一通过虚函数 cleanup() 调用，覆盖所有渲染器类型
                m_renderer->cleanup();
                m_renderer.reset();
            }
            m_window.cleanupGL();
        } else {
            // 安全：上下文不可用，仅清零 GL 对象 ID 防止误用，不调用 glDelete*
            AURORA_WARN("Render", "onCleanup: makeCurrent failed, skipping GL cleanup");
            m_renderer.reset();  // 析构函数中 cleanup() 会检测 wglGetCurrentContext()==NULL 跳过 glDelete*
        }
        AURORA_INFO("Render", "Cleanup frames={}", m_frameCount);
    }

public:
    Camera& camera() { return m_camera; }
    BorderGeometry& geometry() { return m_geometry; }
    ColorScheme& colors() { return m_colors; }
    IModeRenderer* renderer() { return m_renderer.get(); }
    uint64_t frameCount() const { return m_frameCount; }
    void markContextLost() { m_contextLost = true; }  ///< 供 SEH 包装函数标记崩溃

private:
    /// @brief 运行时切换渲染模式
    void switchMode(int mode) {
        // 安全：上下文丢失时跳过 GL 清理（驱动已自动回收），仅清零 ID
        //   所有渲染器现在都有 cleanup() override，统一通过虚函数调用
        if (m_renderer) {
            if (wglGetCurrentContext()) {
                // 安全：通过虚函数统一调用 cleanup()，避免 dynamic_cast 遗漏类型
                //   原代码只对 CycleRenderer 调用 cleanup()，BounceBall/Concerto
                //   依赖析构函数兜底，但析构发生在 reset() 之后，上下文可能已变
                m_renderer->cleanup();
            }
            m_renderer.reset();  // 析构函数中 cleanup() 会检测上下文有效性（幂等）
        }

        if (mode == 0) {
            AURORA_INFO("Render", "switchMode → Cycle");
            m_renderer = std::make_unique<CycleRenderer>();
            int nParticles = ParamStore::Instance().GetInt("particleCount");
            m_renderer->initialize(&m_geometry, &m_camera, nParticles, nullptr);
            auto* cr = static_cast<CycleRenderer*>(m_renderer.get());
            cr->compileShaders();
            cr->updateBorder(m_geometry);
            m_needParticles = true;
        } else if (mode == 1) {
            AURORA_INFO("Render", "switchMode → BounceBall");
            m_renderer = std::make_unique<BounceBallRenderer>();
            m_renderer->initialize(&m_geometry, &m_camera, 300, nullptr);
            auto* bb = static_cast<BounceBallRenderer*>(m_renderer.get());
            bb->compileShaders();
            bb->updateBorder(m_geometry);
            m_needParticles = false;
        } else if (mode == 2) {
            AURORA_INFO("Render", "switchMode → Concerto");
            OutputDebugStringA("=== SWITCHMODE: creating ConcertoRenderer\n");
            m_renderer = std::make_unique<ConcertoRenderer>();
            OutputDebugStringA("=== SWITCHMODE: ConcertoRenderer created\n");
            // maxParticles 复用为总光柱数（每段 10 柱 × N 段）
            int maxColumns = 10 * static_cast<int>(SegmentationManager::Instance().segments().size());
            OutputDebugStringA("=== SWITCHMODE: calling initialize\n");
            m_renderer->initialize(&m_geometry, &m_camera, maxColumns, nullptr);
            OutputDebugStringA("=== SWITCHMODE: initialize done, calling compileShaders\n");
            auto* co = static_cast<ConcertoRenderer*>(m_renderer.get());
            co->compileShaders();
            OutputDebugStringA("=== SWITCHMODE: compileShaders done\n");
            co->updateBorder(m_geometry);
            OutputDebugStringA("=== SWITCHMODE: updateBorder done\n");
            m_needParticles = false;
        }
        m_currentMode = mode;
    }

    /// @brief SEH 保护的渲染帧 — 委托给 RenderFrameSEH.cpp（独立编译单元避免 MSVC C2712）
    ///   RenderFrameSEH_Impl 在独立 .cpp 中编译，不含 C++ 临时变量，可安全使用 __try
    ///   若 SEH 捕获到外部程序导致的 GPU/上下文崩溃，标记 m_contextLost 让下帧恢复
    void renderFrameSEH() {
        RenderFrameSEH_Impl(m_window, m_camera, m_renderer.get(),
                            m_needParticles, &m_contextLost);
        if (m_contextLost) {
            // 安全：SEH 捕获崩溃后立即隐藏叠加层，防止黑屏/花屏覆盖桌面
            //       下帧 Phase 1 会重建上下文后恢复显示
            m_window.setVisible(false);
            AURORA_ERROR("Render", "SEH caught — overlay hidden, recovery scheduled for next frame");
        }
    }

    OverlayWindow m_window;
    Camera m_camera; BorderGeometry m_geometry;
    ColorScheme m_colors;
    std::unique_ptr<IModeRenderer> m_renderer;
    int m_currentMode = 0;
    uint64_t m_frameCount = 0;
    std::atomic<bool> m_contextLost{false};  ///< SEH 捕获帧中崩溃时置 true，下帧恢复
    int m_recoverFailCount = 0;             ///< 连续恢复失败次数（指数退避策略用）
    int m_makeCurrentFailCount = 0;        ///< makeCurrent 连续失败次数（区分临时失败 vs 真正丢失）
    bool m_needParticles = true;            ///< 当前模式是否需要粒子渲染（避免 SEH 函数内用 dynamic_cast 触发 C2712）
    int m_borderTop = 100, m_borderBottom = 100, m_borderLeft = 100, m_borderRight = 100;
};