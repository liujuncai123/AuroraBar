/**
 * @file QtRenderThread.h
 * @brief 独立渲染线程 — 直接渲染到 QWindow（GL 直渲，无 FBO / QImage 中转）
 * @date 2026-07-18
 * @details 渲染在独立 QThread 中执行，GL 上下文不阻塞主线程。
 *          每帧 makeCurrent(m_window) → glBindFramebuffer(0) → 渲染 → swapBuffers。
 *
 *          架构：
 *          ┌─ QtRenderThread（独立 QThread）─────────────────┐
 *          │ QOpenGLContext.makeCurrent(m_window)            │
 *          │ glBindFramebuffer(GL_FRAMEBUFFER, 0)            │ ← 默认 framebuffer = 窗口
 *          │ Camera.update()                                │
 *          │ m_renderer->renderFrame(dt) + renderParticles() │
 *          │ m_glContext->swapBuffers(m_window)              │ ← 直接呈现
 *          │ pollRenderQueue() 消费 g_renderQueue            │
 *          └────────────────────────────────────────────────┘
 *                            ↑ m_window 由主线程 QtOverlayWindow 持有
 *
 *          优势：
 *            1. GL 渲染不阻塞主线程（Qt 事件循环流畅）
 *            2. 直接渲染到窗口默认 framebuffer，无 GPU→CPU→GPU 往返
 *            3. swapBuffers 在 DWM 切换时即使阻塞也只卡渲染线程，不影响主线程
 *
 * @note 安全：GL 上下文在本线程 run() 中创建和使用，不跨线程共享。
 *             QWindow* 是借用，不持有；生命周期由 QtOverlayWindow 保证
 *             （QtOverlayWindow 析构时先 stopAndJoin 本线程再销毁 QWindow）。
 */
#pragma once

#include <QThread>
#include <memory>
#include <atomic>

class QOpenGLContext;
class QWindow;
class Camera;
class BorderGeometry;
class IModeRenderer;
struct RenderCommand;

class QtRenderThread : public QThread {
    Q_OBJECT
public:
    /// @brief 构造
    /// @param window  目标 QWindow（主线程已 show() 创建 HWND，本线程 makeCurrent 用）
    /// @param w       窗口宽（像素）
    /// @param h       窗口高（像素）
    /// @param parent  QObject 父对象
    QtRenderThread(QWindow* window, int w, int h, QObject* parent = nullptr);
    ~QtRenderThread() override;

    /// @brief 停止渲染循环并等待线程退出
    void stopAndJoin();

protected:
    void run() override;

private:
    // ---------- GL 上下文 ----------
    bool initGL();
    void cleanupGL();

    // ---------- 渲染器 ----------
    bool initRenderer();
    void destroyRenderer();
    void switchMode(int mode);
    void applyBorderConfig();

    // ---------- 命令队列 ----------
    void pollRenderQueue();
    void handleCommand(const RenderCommand& cmd);

    // ---------- 单帧渲染 ----------
    void renderOneFrame(double dt);

    // ---------- 配置 ----------
    QWindow* m_window;          ///< 借用：目标窗口（主线程持有，本线程 makeCurrent 用）
    int m_fboW;                 ///< 渲染目标宽（= 窗口宽，命名保留兼容旧日志）
    int m_fboH;                 ///< 渲染目标高（= 窗口高）
    std::atomic<bool> m_running{true};

    // ---------- GL 资源（run() 中创建） ----------
    // 安全：RAII 管理 QOpenGLContext，避免 initGL 异常时泄漏
    std::unique_ptr<QOpenGLContext> m_glContext;
    // 注意：不再有 m_surface / m_fbo / m_fboTex
    //       渲染目标就是窗口本身（默认 framebuffer 0）

    // ---------- 渲染器架构（run() 中创建） ----------
    std::unique_ptr<Camera>          m_camera;
    std::unique_ptr<BorderGeometry>  m_geometry;
    std::unique_ptr<IModeRenderer>   m_renderer;
    int  m_currentMode = -1;

    // ---------- 边框配置 ----------
    int m_borderTop = 100, m_borderBottom = 100;
    int m_borderLeft = 100, m_borderRight = 100;

    // ---------- 渲染状态 ----------
    float m_time = 0.0f;
};
