/**
 * @file QtOverlayWindow.cpp
 * @brief Qt6 透明叠加窗口实现（QWindow + GL 直渲）
 * @date 2026-07-18
 * @details
 *   1. 配置 QSurfaceFormat：3.3 CompatibilityProfile + alphaBufferSize(8) 启用透明
 *   2. setSurfaceType(OpenGLSurface) 让 QWindow 成为 GL drawable
 *   3. setGeometry(全屏) + show() 创建底层 HWND（必须在主线程）
 *   4. Win32 SetWindowLongPtr 设置 WS_EX_TRANSPARENT 鼠标穿透
 *   5. 创建 QtRenderThread 并传入 this（QWindow*），start() 启动
 *   6. 渲染线程自己 makeCurrent(this) + swapBuffers(this)，主线程不参与 GL
 */
#include "QtOverlayWindow.h"
#include "QtRenderThread.h"
#include "../logging/LoggerManager.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSurfaceFormat>
#include <QExposeEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QMetaObject>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

// ============================================================================
// 构造 / 析构
// ============================================================================

QtOverlayWindow::QtOverlayWindow(QWindow* parent)
    : QWindow(parent)
{
    AURORA_INFO("QtOverlay", "Constructor (QWindow + GL direct render)");

    // 安全：必须设为 OpenGLSurface，QWindow 才能成为 GL drawable
    setSurfaceType(QSurface::OpenGLSurface);

    // GL 格式：3.3 CompatibilityProfile（旧 shader 用固定管线语义）
    //   alphaBufferSize(8) → 启用 alpha 通道，配合 Qt6 DirectComposition 实现透明
    //   DoubleBuffer + SwapInterval(1) → VSync 防止撕裂
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    fmt.setAlphaBufferSize(8);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(1);  // VSync
    setFormat(fmt);

    // 安全：窗口 flags 配置
    //   🔧 [DWM 桌面空闲修复] 不使用 Qt::Tool
    //     原因：Qt::Tool 让 DWM 把窗口当作"工具窗口"，当所有非 Tool 窗口最小化时，
    //           DWM 进入"桌面空闲"模式，停止合成 Tool 窗口 → 屏幕黑屏但 swap chain 仍工作
    //           用户必须点击屏幕（任何输入）才能唤醒 DWM 重新合成
    //     修复：改用普通顶级窗口 + Win32 设置 WS_EX_TOOLWINDOW（防任务栏显示）
    //           + WS_EX_NOACTIVATE（防抢焦点）
    //           这样 DWM 把它当作普通窗口，不进入桌面空闲模式
    setFlags(Qt::FramelessWindowHint |
             Qt::WindowStaysOnTopHint);
}

QtOverlayWindow::~QtOverlayWindow()
{
    AURORA_INFO("QtOverlay", "Destructor");
    // 安全：先停止渲染线程（stopAndJoin 内部会 doneCurrent + 清理 GL 资源），
    //       再让 QWindow 析构销毁 HWND
    m_renderThread.reset();
}

// ============================================================================
// 初始化
// ============================================================================

bool QtOverlayWindow::initialize()
{
    AURORA_INFO("QtOverlay", "initialize()");

    // 全屏覆盖主屏幕
    QScreen* scr = QGuiApplication::primaryScreen();
    if (!scr) {
        AURORA_ERROR("QtOverlay", "No screen available");
        return false;
    }
    QRect geo = scr->geometry();
    setGeometry(geo);

    // 安全：DPI 缩放处理 — Qt::geometry() 返回逻辑像素（DPI 缩放后），
    //   但 glViewport / framebuffer 操作需要物理像素。
    //   例：2560x1440 屏幕 + 125% DPI → 逻辑 2048x1152 → 物理 2560x1440。
    //   若直接用逻辑像素做 glViewport，渲染只覆盖左下角一块。
    qreal dpr = scr->devicePixelRatio();
    int physW = static_cast<int>(std::round(geo.width()  * dpr));
    int physH = static_cast<int>(std::round(geo.height() * dpr));
    AURORA_INFO("QtOverlay", "Geometry logical={}x{} physical={}x{} (DPR={})",
                geo.width(), geo.height(), physW, physH, dpr);

    // create() 创建底层 HWND 但不显示（避免首帧黑屏）
    //   窗口将在渲染线程首帧完成后由 QtRenderThread 通过 invokeMethod 调用 show()
    create();

    // 手动设置 Win32 ExStyle（鼠标穿透 + 不在任务栏显示 + 不抢焦点 + 强制分层合成）
    //   - WS_EX_TRANSPARENT: 鼠标点击穿透到下层窗口
    //   - WS_EX_TOOLWINDOW: 不在任务栏/Alt+Tab 显示（替代 Qt::Tool）
    //   - WS_EX_NOACTIVATE: 防止窗口被激活抢焦点
    //   - WS_EX_LAYERED:
    //     🔧 [dGPU DWM 修复] 纯独显机器上，DWM 可能跳过合成透明工具窗口（用户实测：
    //       不开任务管理器 → DWM 跳过合成 → 黑屏；开任务管理器 → DWM 活跃 → 正常）。
    //       添加 WS_EX_LAYERED 后 DWM 强制使用分层合成路径，与任务管理器状态解耦。
    //       Qt6 的 DirectComposition surface 会覆盖 SetLayeredWindowAttributes，
    //       透明度仍由 DirectComposition 视觉树控制，不影响实际透明效果。
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE,
                         exStyle | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED);
        AURORA_INFO("QtOverlay", "ExStyle set (TRANSPARENT|TOOLWINDOW|NOACTIVATE|LAYERED), HWND=0x{:X}",
                    reinterpret_cast<uintptr_t>(hwnd));
    } else {
        AURORA_WARN("QtOverlay", "winId() returned null — mouse pass-through not set");
    }

    // 创建并启动渲染线程（独立线程，QWindow* 传入用于 makeCurrent / swapBuffers）
    //   注意：传物理像素 physW/physH 给渲染线程，glViewport 才能覆盖全屏
    m_renderThread = std::make_unique<QtRenderThread>(this, physW, physH, this);
    m_renderThread->start();

    // 启动 DWM 反 cloaking 守护定时器（防止最小化其他窗口时 overlay 被 DWM 隐藏）
    setupAntiCloak();

    AURORA_INFO("QtOverlay", "Initialized — QtRenderThread running (GL direct render)");
    OutputDebugStringA("=== QtOverlayWindow (QWindow + GL direct) initialized ===\n");
    return true;
}

// ============================================================================
// 公共接口
// ============================================================================

void QtOverlayWindow::setOverlayVisible(bool visible)
{
    setVisible(visible);
}

// ============================================================================
// 事件回调（首版仅记日志，渲染线程主循环自驱动）
// ============================================================================

void QtOverlayWindow::exposeEvent(QExposeEvent* event)
{
    AURORA_TRACE("QtOverlay", "exposeEvent isExposed={}", isExposed() ? 1 : 0);
    QWindow::exposeEvent(event);
}

void QtOverlayWindow::resizeEvent(QResizeEvent* event)
{
    AURORA_INFO("QtOverlay", "resizeEvent {}x{} (render thread viewport unchanged in v1)",
                event->size().width(), event->size().height());
    QWindow::resizeEvent(event);
}

// ============================================================================
// DWM 反 cloaking 守护
// ============================================================================

void QtOverlayWindow::setupAntiCloak()
{
    // 🔧 [dGPU DWM 修复 v2] 自适应轮询，替代固定 33ms 轮询
    //   DWM cloaking 状态变化不会发送窗口消息，必须轮询 DwmGetWindowAttribute。
    //   策略（自适应双态）：
    //     正常态：1000ms 间隔（99.9% 时间），CPU 开销 ~0.001%
    //     异常态：检测到 cloaked → 立即切到 33ms 高频，持续 2 秒（60 帧），CPU 开销 ~0.015%
    //     2 秒内稳定 → 自动恢复 1000ms
    //   对比固定 33ms：
    //     - 正常态 CPU 开销降低 30 倍（1 次/秒 vs 30 次/秒）
    //     - 异常态响应相同（33ms）
    AURORA_INFO("QtOverlay", "setupAntiCloak() — adaptive: 1000ms normal, 33ms burst on cloaked");

    m_antiCloakTimer = std::make_unique<QTimer>();
    QTimer* timer = m_antiCloakTimer.get();
    timer->setInterval(1000);  // 默认慢速

    // 自适应状态（lambda 内用 static 跨回调保持）
    // 注意：此 lambda 在 QtOverlayWindow 生命周期内不会被销毁后重建，
    //       static 变量安全（同一对象同一 timer 同一回调）
    QObject::connect(timer, &QTimer::timeout, this, [this, timer]() {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd) return;

        BOOL isCloaked = FALSE;
        HRESULT hrGet = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
        bool isVisible = IsWindowVisible(hwnd) != FALSE;

        // ── 自适应状态机 ──
        static bool   inBurst = false;   // 是否处于高频 burst 模式
        static int    stableFrames = 0;  // 连续正常帧数（burst 模式下计数）
        static int    burstRecoverCount = 0;
        constexpr int kBurstMs = 33;     // 高频间隔
        constexpr int kStableThreshold = 60;  // 连续 60 帧正常后恢复慢速（~2秒）

        if (SUCCEEDED(hrGet) && isCloaked) {
            // ── 异常：立即切 burst 模式 ──
            if (!inBurst) {
                inBurst = true;
                stableFrames = 0;
                timer->setInterval(kBurstMs);
                AURORA_WARN("QtOverlay", "DWM cloaked! Switching to 33ms burst mode");
            }

            BOOL cloak = FALSE;
            DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
            if (!isVisible) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);

            if (burstRecoverCount++ < 10) {
                AURORA_WARN("QtOverlay", "Burst recover #{}", burstRecoverCount);
            }
            stableFrames = 0;  // 重置稳定计数
            return;
        }

        if (SUCCEEDED(hrGet) && !isCloaked && isVisible) {
            // ── 正常 ──
            if (inBurst) {
                ++stableFrames;
                if (stableFrames >= kStableThreshold) {
                    inBurst = false;
                    stableFrames = 0;
                    burstRecoverCount = 0;
                    timer->setInterval(1000);
                    AURORA_INFO("QtOverlay", "Stable for {} frames, back to 1000ms", kStableThreshold);
                }
            }
            // 正常态 + 非 burst：不执行任何操作，纯跳过
            return;
        }

        // ── 窗口不可见但未 cloaked（罕见边缘情况）──
        if (!isVisible) {
            // 也切 burst 模式
            if (!inBurst) {
                inBurst = true;
                stableFrames = 0;
                timer->setInterval(kBurstMs);
            }
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
            stableFrames = 0;
        }
    });
    m_antiCloakTimer->start();
}

// ============================================================================
// 原生 Win32 消息处理
// ============================================================================

bool QtOverlayWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    if (eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        switch (msg->message) {
        case WM_DWMCOMPOSITIONCHANGED:
            // DWM 合成器状态变化（如 DWM 重启 / 主题切换）
            AURORA_INFO("QtOverlay", "WM_DWMCOMPOSITIONCHANGED — forcing re-show");
            QMetaObject::invokeMethod(this, [this]() {
                show();
                raise();
            }, Qt::QueuedConnection);
            break;

        case WM_DISPLAYCHANGE:
            // 显示配置变化（分辨率 / 多显示器布局）
            AURORA_INFO("QtOverlay", "WM_DISPLAYCHANGE — forcing re-show");
            QMetaObject::invokeMethod(this, [this]() {
                show();
                raise();
            }, Qt::QueuedConnection);
            break;

        case WM_WININICHANGE:
            // 系统参数变化（含"显示桌面"切换的 Shell 通知）
            // lParam 指向字符串，可能是 "Shell" / "WindowsTheme" 等
            if (msg->lParam) {
                const char* section = reinterpret_cast<const char*>(msg->lParam);
                AURORA_INFO("QtOverlay", "WM_WININICHANGE section='{}' — forcing re-show", section);
            } else {
                AURORA_INFO("QtOverlay", "WM_WININICHANGE (no section) — forcing re-show");
            }
            QMetaObject::invokeMethod(this, [this]() {
                show();
                raise();
                HWND hwnd = reinterpret_cast<HWND>(winId());
                if (hwnd) {
                    BOOL cloak = FALSE;
                    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                }
            }, Qt::QueuedConnection);
            break;

        default:
            break;
        }
    }
    return QWindow::nativeEvent(eventType, message, result);
}
