/**
 * @file QtOverlayWindow.h
 * @brief Qt6 透明叠加窗口 — QWindow + GL 直渲
 * @date 2026-07-18
 * @details 本类是 QWindow 子类，本身即 OpenGLDrawable。
 *          渲染线程直接 makeCurrent(this) + glBindFramebuffer(0) + swapBuffers(this)，
 *          不再经过 FBO / glReadPixels / QImage / QPainter 中转。
 *
 *          架构：
 *          ┌─ QtRenderThread（独立线程）──────────────┐
 *          │ QOpenGLContext.makeCurrent(m_window)     │
 *          │ glBindFramebuffer(GL_FRAMEBUFFER, 0)     │
 *          │ glClear + renderParticles                │
 *          │ m_glContext->swapBuffers(m_window)       │ ← 直接呈现到屏幕
 *          └──────────────────────────────────────────┘
 *                            ↑ 共享 QWindow* (主线程创建)
 *          ┌─ QtOverlayWindow : public QWindow（主线程）┐
 *          │ setSurfaceType(OpenGLSurface)             │
 *          │ QSurfaceFormat 3.3 CompatibilityProfile    │
 *          │   + alphaBufferSize(8) (透明)              │
 *          │ setFlags(Frameless | StaysOnTop | Tool)    │
 *          │ show() 创建 HWND                           │
 *          │ WS_EX_TRANSPARENT 鼠标穿透                 │
 *          └────────────────────────────────────────────┘
 *
 *          优势：
 *            1. GL 直接渲染到窗口默认 framebuffer，无 GPU→CPU→GPU 往返
 *            2. swapBuffers 只阻塞渲染线程，主线程不卡
 *            3. Qt6 DirectComposition 透明合成，Win+D / 最小化 稳定
 *
 * @note 安全：本类不持有任何 GL 资源，所有 GL 调用在 QtRenderThread 中执行。
 *             QWindow 必须在主线程 show() 创建 HWND 后才能传给渲染线程 makeCurrent。
 */
#pragma once

#include <QWindow>
#include <memory>

class QtRenderThread;
class QTimer;

class QtOverlayWindow : public QWindow {
    Q_OBJECT
public:
    explicit QtOverlayWindow(QWindow* parent = nullptr);
    ~QtOverlayWindow() override;

    /// @brief 初始化：配置 GL 格式 + 创建透明窗口 + 启动渲染线程
    /// @return true 成功；false 失败
    bool initialize();

    /// @brief 设置叠加层可见性（主线程）
    void setOverlayVisible(bool visible);

protected:
    /// @brief 窗口暴露事件（最小化/恢复/遮挡变化时 Qt 触发）
    /// @note  首版仅记日志，渲染线程主循环自驱动，不依赖 expose 触发重绘
    void exposeEvent(QExposeEvent* event) override;

    /// @brief 窗口尺寸变化事件（屏幕分辨率变化）
    /// @note  首版仅记日志，不主动通知渲染线程；分辨率变化属罕见场景，后续再加同步
    void resizeEvent(QResizeEvent* event) override;

    /// @brief 原生 Win32 消息处理
    /// @details 捕获 DWM 合成状态变化 / 显示配置变化消息，
    ///          收到时强制重新 show + raise，防止 DWM 在桌面状态变化时把 overlay cloaking。
    ///          典型场景：用户最小化最后一个普通窗口 → DWM 进入"显示桌面"模式 →
    ///          overlay 被 cloaking（屏幕看不到但 swap chain 仍工作）。
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    /// @brief 启动 DWM 反 cloaking 守护定时器
    /// @details 每 500ms 检查窗口状态，强制：
    ///           1. DwmSetWindowAttribute(DWMWA_CLOAK, FALSE) — 取消 DWM 隐藏
    ///           2. SetWindowPos(HWND_TOPMOST) — 强制 Z-order 顶层
    ///          防止 DWM 在桌面空闲时把 overlay 标记为 cloaked。
    void setupAntiCloak();

    std::unique_ptr<QtRenderThread> m_renderThread;  ///< 渲染线程（独立 GL 上下文）
    std::unique_ptr<QTimer> m_antiCloakTimer;        ///< DWM 反 cloaking 守护定时器
};
