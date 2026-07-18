/**
 * @file QtRenderThread.cpp
 * @brief 独立渲染线程实现（GL 直渲到 QWindow，无 FBO / QImage 中转）
 * @date 2026-07-18
 * @details 渲染在独立 QThread 中执行：
 *            1. 创建 QOpenGLContext
 *            2. makeCurrent(m_window) — 直接绑定到主线程创建的 QWindow
 *            3. glBindFramebuffer(GL_FRAMEBUFFER, 0) — 默认 framebuffer 即窗口
 *            4. glClear + renderParticles
 *            5. m_glContext->swapBuffers(m_window) — 直接呈现到屏幕
 *
 *          帧率控制：基于 steady_clock 计算 dt，sleep_for 控制 ~60fps。
 *          注意：SwapInterval(1) 启用 VSync 后，swapBuffers 会自动等待垂直同步，
 *                sleep_for 仅作为 VSync 之外的保险，防止 dt 过大时跑满 CPU。
 */
#include "QtRenderThread.h"
#include "../logging/LoggerManager.h"

// 安全：GLEW 必须在 Qt OpenGL 头文件之前包含
#include <GL/glew.h>

#include <QOpenGLContext>
#include <QWindow>
#include <QSurfaceFormat>
#include <QMetaObject>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

// 渲染器架构
#include "../render/Camera.h"
#include "../render/BorderGeometry.h"
#include "../render/modes/IModeRenderer.h"
#include "../render/modes/CycleRenderer.h"
#include "../render/modes/BounceBallRenderer.h"
#include "../render/modes/ConcertoRenderer.h"
#include "../core/CommandTypes.h"
#include "../core/SPSCQueue.h"
#include "../params/ParamStore.h"
#include "../segmentation/SegmentationManager.h"

// 全局 SPSC 渲染命令队列（声明在 main.cpp，LogicThread 生产，本线程消费）
extern SPSCQueue<RenderCommand, 64> g_renderQueue;

// ============================================================================
// 构造 / 析构
// ============================================================================

QtRenderThread::QtRenderThread(QWindow* window, int w, int h, QObject* parent)
    : QThread(parent), m_window(window), m_fboW(w), m_fboH(h)
{
    AURORA_INFO("QtRender", "Constructor window={} size={}x{}",
                reinterpret_cast<uintptr_t>(window), w, h);
}

QtRenderThread::~QtRenderThread()
{
    stopAndJoin();
    AURORA_INFO("QtRender", "Destructor done");
}

void QtRenderThread::stopAndJoin()
{
    if (!m_running.exchange(false)) return;  // 已经停止
    if (isRunning()) {
        if (!wait(2000)) {
            AURORA_WARN("QtRender", "Thread did not exit within 2s, terminating");
            terminate();
            wait(500);
        }
    }
}

// ============================================================================
// 线程入口
// ============================================================================

void QtRenderThread::run()
{
    AURORA_INFO("QtRender", "run() start");

    // Step 1: 创建 GL 上下文（直接绑定到 m_window，无需离屏 surface）
    if (!initGL()) {
        AURORA_ERROR("QtRender", "initGL failed");
        return;
    }

    // Step 2: 创建渲染器架构
    if (!initRenderer()) {
        AURORA_ERROR("QtRender", "initRenderer failed");
        cleanupGL();
        return;
    }

    // 🔧 [启动黑屏修复] 首帧渲染后再显示窗口，避免 create()→show() 之间短暂黑屏
    //   根因：create() 只创建 HWND 不显示 → isExposed()=false → renderOneFrame 跳过渲染
    //   修复：渲染器就绪后通知主线程 show()，等待窗口暴露后再进入渲染循环
    QMetaObject::invokeMethod(m_window, [this]() {
        m_window->show();
        m_window->raise();
    }, Qt::QueuedConnection);
    // 等待主线程处理 show 事件（最多 500ms）
    for (int i = 0; i < 50 && !m_window->isExposed(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    AURORA_INFO("QtRender", "Window exposed={} after show", m_window->isExposed() ? 1 : 0);

    // Step 3: 渲染主循环
    auto lastTime = std::chrono::steady_clock::now();
    uint64_t frameCount = 0;

    while (m_running.load(std::memory_order_acquire)) {
        // 计算 dt
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastTime).count();
        if (dt > 0.1) dt = 1.0 / 60.0;  // 钳制大间隔（暂停恢复）
        lastTime = now;
        m_time += static_cast<float>(dt);

        // 消费渲染命令（LogicThread 在另一线程生产）
        pollRenderQueue();

        // 渲染一帧（makeCurrent + glBindFramebuffer(0) + render + swapBuffers）
        renderOneFrame(dt);

        // 帧率控制：从 ParamStore 读取 targetFps，动态适应高刷新率屏幕（120/144/240Hz）
        //   SwapInterval(1) 已在 QSurfaceFormat 中启用 VSync，swapBuffers 会等待垂直同步
        //   这里的 sleep 仅作为 dt 过大时的保险（避免空转跑满 CPU）
        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - now).count();
        // 安全：targetFps 范围已在 BuiltInDefaults.cpp 中 clamp 到 [30, 360]，防除零
        int targetFps = ParamStore::Instance().GetInt("targetFps");
        if (targetFps < 30) targetFps = 60;  // 兜底，防止异常值
        int targetMs = 1000 / targetFps;
        int remaining = targetMs - static_cast<int>(elapsed);
        if (remaining > 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining));
        }

        ++frameCount;
        if ((frameCount & 0xFF) == 0) {  // 每 256 帧打一次心跳
            AURORA_INFO("QtRender", "heartbeat frame={} active={}",
                        frameCount, m_renderer ? m_renderer->activeParticles() : -1);
        }
    }

    // Step 4: 清理
    destroyRenderer();
    cleanupGL();
    AURORA_INFO("QtRender", "run() exit, frames={}", frameCount);
}

// ============================================================================
// GL 上下文
// ============================================================================

bool QtRenderThread::initGL()
{
    AURORA_INFO("QtRender", "initGL()");

    if (!m_window) {
        AURORA_ERROR("QtRender", "initGL: m_window is null");
        return false;
    }

    // QSurfaceFormat 已在 QtOverlayWindow 构造时设置（包括 alpha + VSync），
    //   这里复用窗口的 format 保证上下文与窗口匹配
    // 注意：变量名不能用 fmt，会和 spdlog 依赖的 fmt 命名空间冲突
    QSurfaceFormat surfaceFmt = m_window->format();
    AURORA_INFO("QtRender", "GL surface format acquired (alpha={}, swap={})",
                surfaceFmt.alphaBufferSize(), surfaceFmt.swapInterval());

    m_glContext = std::make_unique<QOpenGLContext>();
    m_glContext->setFormat(surfaceFmt);
    if (!m_glContext->create()) {
        AURORA_ERROR("QtRender", "QOpenGLContext creation failed");
        m_glContext.reset();
        return false;
    }

    // 安全：makeCurrent 到 QWindow（跨线程 makeCurrent，Qt 官方支持）
    if (!m_glContext->makeCurrent(m_window)) {
        AURORA_ERROR("QtRender", "makeCurrent(m_window) failed");
        return false;
    }

    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        AURORA_ERROR("QtRender", "GLEW init failed: {}",
                     reinterpret_cast<const char*>(glewGetErrorString(err)));
        m_glContext->doneCurrent();
        return false;
    }

    const GLubyte* glVersion = glGetString(GL_VERSION);
    AURORA_INFO("QtRender", "GL ready, OpenGL Version: {}",
                reinterpret_cast<const char*>(glVersion));

    // 设置默认 GL 状态（全局保持）
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 🔧 [根因修复] CompatibilityProfile 下 gl_PointCoord 默认未启用，
    //   导致 fragment shader 中 gl_PointCoord 永远返回 (0,0)，discard 逻辑丢弃所有像素。
    //   必须 glEnable(GL_POINT_SPRITE) 才能让 gl_PointCoord 在点 sprite 内插值。
    //   Core Profile 下 gl_PointCoord 默认可用，但 Compatibility 下需要显式启用。
    glEnable(GL_POINT_SPRITE);
    glEnable(GL_PROGRAM_POINT_SIZE);

    // 设置 viewport（窗口尺寸）
    glViewport(0, 0, m_fboW, m_fboH);

    m_glContext->doneCurrent();
    AURORA_INFO("QtRender", "GL context ready on window {}x{}", m_fboW, m_fboH);
    return true;
}

void QtRenderThread::cleanupGL()
{
    // 安全：先 makeCurrent 才能安全 doneCurrent；如果 makeCurrent 失败说明上下文已损坏，
    //       直接 reset 即可（驱动会自动回收资源）
    if (m_glContext) {
        if (m_window && m_glContext->makeCurrent(m_window)) {
            m_glContext->doneCurrent();
        }
        m_glContext.reset();  // RAII 自动 delete
    }
}

// ============================================================================
// 渲染器
// ============================================================================

bool QtRenderThread::initRenderer()
{
    AURORA_INFO("QtRender", "initRenderer()");

    if (!m_glContext || !m_glContext->makeCurrent(m_window)) {
        AURORA_ERROR("QtRender", "initRenderer: makeCurrent failed");
        return false;
    }

    m_camera = std::make_unique<Camera>();
    m_camera->setScreenSize(m_fboW, m_fboH);
    m_camera->setCurvature(0.5);

    // 安全：从 ParamStore 读取 border 初值，避免启动瞬间用 m_borderTop=100 默认值
    //   后续 LogicThread 启动时也会推送 BorderConfig 命令（订阅变更用），不冲突
    auto& ps = ParamStore::Instance();
    m_borderTop    = ps.GetInt("borderWidth.top");
    m_borderBottom = ps.GetInt("borderWidth.bottom");
    m_borderLeft   = ps.GetInt("borderWidth.left");
    m_borderRight  = ps.GetInt("borderWidth.right");
    AURORA_INFO("QtRender", "initRenderer: border t={} b={} l={} r={}",
                m_borderTop, m_borderBottom, m_borderLeft, m_borderRight);

    m_geometry = std::make_unique<BorderGeometry>();
    applyBorderConfig();

    int savedMode = ParamStore::Instance().GetInt("mode");
    AURORA_INFO("QtRender", "initRenderer: savedMode={}", savedMode);
    switchMode(savedMode);

    m_glContext->doneCurrent();
    return m_renderer != nullptr;
}

void QtRenderThread::destroyRenderer()
{
    if (!m_glContext || !m_glContext->makeCurrent(m_window)) return;
    if (m_renderer) {
        m_renderer->cleanup();
        m_renderer.reset();
    }
    m_currentMode = -1;
    m_glContext->doneCurrent();
}

void QtRenderThread::switchMode(int mode)
{
    AURORA_INFO("QtRender", "switchMode({})", mode);

    // 安全：调用方必须保证 GL 已 makeCurrent
    if (m_renderer) {
        m_renderer->cleanup();
        m_renderer.reset();
    }

    if (mode == 0) {
        AURORA_INFO("QtRender", "switchMode → Cycle");
        m_renderer = std::make_unique<CycleRenderer>();
        int nParticles = ParamStore::Instance().GetInt("particleCount");
        m_renderer->initialize(m_geometry.get(), m_camera.get(), nParticles, nullptr);
        auto* cr = static_cast<CycleRenderer*>(m_renderer.get());
        cr->compileShaders();
        cr->updateBorder(*m_geometry);
    } else if (mode == 1) {
        AURORA_INFO("QtRender", "switchMode → BounceBall");
        m_renderer = std::make_unique<BounceBallRenderer>();
        m_renderer->initialize(m_geometry.get(), m_camera.get(), 300, nullptr);
        auto* bb = static_cast<BounceBallRenderer*>(m_renderer.get());
        bb->compileShaders();
        bb->updateBorder(*m_geometry);
    } else if (mode == 2) {
        AURORA_INFO("QtRender", "switchMode → Concerto");
        m_renderer = std::make_unique<ConcertoRenderer>();
        int maxColumns = 10 * static_cast<int>(SegmentationManager::Instance().segments().size());
        m_renderer->initialize(m_geometry.get(), m_camera.get(), maxColumns, nullptr);
        auto* co = static_cast<ConcertoRenderer*>(m_renderer.get());
        co->compileShaders();
        co->updateBorder(*m_geometry);
    } else {
        AURORA_WARN("QtRender", "switchMode: unknown mode={}, fallback to Cycle", mode);
        mode = 0;
        m_renderer = std::make_unique<CycleRenderer>();
        int nParticles = ParamStore::Instance().GetInt("particleCount");
        m_renderer->initialize(m_geometry.get(), m_camera.get(), nParticles, nullptr);
        auto* cr = static_cast<CycleRenderer*>(m_renderer.get());
        cr->compileShaders();
        cr->updateBorder(*m_geometry);
    }
    m_currentMode = mode;
}

void QtRenderThread::applyBorderConfig()
{
    if (!m_geometry) return;
    // 安全：cornerTransition 使用 BorderConfig 结构体默认值（40），
    //       sampleCount 使用 compute() 默认值（200），不再重复硬编码
    BorderConfig cfg{m_borderTop, m_borderBottom, m_borderLeft, m_borderRight};
    m_geometry->compute(cfg, m_fboW, m_fboH);

    if (m_renderer) {
        if (auto* cr = dynamic_cast<CycleRenderer*>(m_renderer.get()))
            cr->updateBorder(*m_geometry);
        else if (auto* bb = dynamic_cast<BounceBallRenderer*>(m_renderer.get()))
            bb->updateBorder(*m_geometry);
        else if (auto* co = dynamic_cast<ConcertoRenderer*>(m_renderer.get()))
            co->updateBorder(*m_geometry);
    }
}

// ============================================================================
// 命令队列
// ============================================================================

void QtRenderThread::pollRenderQueue()
{
    RenderCommand cmd;
    while (g_renderQueue.tryPop(cmd)) {
        handleCommand(cmd);
    }
}

void QtRenderThread::handleCommand(const RenderCommand& cmd)
{
    switch (cmd.type) {
    case RenderCommand::Type::ModeChange:
        if (m_glContext && m_glContext->makeCurrent(m_window)) {
            switchMode(static_cast<int>(cmd.paramValue));
            m_glContext->doneCurrent();
        }
        break;

    case RenderCommand::Type::BorderConfig: {
        std::string name(cmd.paramName.data());
        if (name == "top")    m_borderTop    = static_cast<int>(cmd.paramValue);
        if (name == "bottom") m_borderBottom = static_cast<int>(cmd.paramValue);
        if (name == "left")   m_borderLeft   = static_cast<int>(cmd.paramValue);
        if (name == "right")  m_borderRight  = static_cast<int>(cmd.paramValue);
        if (m_glContext && m_glContext->makeCurrent(m_window)) {
            applyBorderConfig();
            m_glContext->doneCurrent();
        }
        AURORA_INFO("QtRender", "BorderConfig t={} b={} l={} r={}",
                    m_borderTop, m_borderBottom, m_borderLeft, m_borderRight);
        break;
    }

    case RenderCommand::Type::OverlayVisible:
        // 渲染线程不直接控制窗口可见性，由主线程处理
        // 转发到主线程需要额外信号，这里简单忽略（窗口可见性由 setOverlayVisible 控制）
        AURORA_INFO("QtRender", "OverlayVisible={} (handled by main thread)",
                    cmd.paramValue > 0.5);
        break;

    case RenderCommand::Type::GlobalParam: {
        // 🔧 [子模式修复] subMode 切换会触发 compileShaders()，需要 GL 上下文
        //   其他 GlobalParam（如 concerto.* 参数）不需要 makeCurrent，直接 pushCommand
        if (std::strcmp(cmd.paramName.data(), "subMode") == 0) {
            if (m_glContext && m_glContext->makeCurrent(m_window)) {
                if (m_renderer) {
                    m_renderer->pushCommand(cmd);
                }
                m_glContext->doneCurrent();
            } else {
                AURORA_WARN("QtRender", "subMode switch skipped — makeCurrent failed");
            }
            break;
        }
        if (std::strcmp(cmd.paramName.data(), "curvatureDepth") == 0) {
            if (m_camera) m_camera->setCurvature(cmd.paramValue);
            break;
        }
        // 其他 GlobalParam 直接透传（不需要 GL 上下文）
        if (m_renderer) {
            m_renderer->pushCommand(cmd);
        } else {
            AURORA_WARN("QtRender", "pushCommand dropped — renderer null, cmdType={}",
                        static_cast<int>(cmd.type));
        }
        break;
    }

    default:
        if (m_renderer) {
            m_renderer->pushCommand(cmd);
        } else {
            AURORA_WARN("QtRender", "pushCommand dropped — renderer null, cmdType={}",
                        static_cast<int>(cmd.type));
        }
        break;
    }
}

// ============================================================================
// 单帧渲染 — GL 直渲到窗口
// ============================================================================

void QtRenderThread::renderOneFrame(double dt)
{
    // 安全：窗口未暴露（最小化/被遮挡/桌面空闲模式）时跳过渲染
    //   Qt 官方建议：isExposed=false 时不要调 makeCurrent/swapBuffers，否则可能阻塞或无效。
    //   但用户实测发现：DWM 桌面空闲模式下窗口仍 isExposed=true，只是 DWM 不主动合成。
    //   因此这里不能完全依赖 isExposed 跳过，需要配合下面的 DwmFlush 强制合成。
    if (!m_window->isExposed()) {
        // 窗口未暴露，跳过本帧（不调 swapBuffers，避免阻塞渲染线程）
        static int skipCount = 0;
        if (skipCount++ < 5) {
            AURORA_WARN("QtRender", "renderOneFrame: window not exposed, skip frame #{}",
                        skipCount);
        }
        return;
    }

    if (!m_glContext || !m_glContext->makeCurrent(m_window)) {
        AURORA_WARN("QtRender", "renderOneFrame: makeCurrent failed");
        return;
    }

    // 安全：绑定窗口默认 framebuffer（0 = 窗口本身）
    //   注意：QWindow 在 Qt6 DirectComposition 下，默认 framebuffer 自带 alpha 通道
    //   （由 QSurfaceFormat::setAlphaBufferSize(8) 启用），无需额外 FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_fboW, m_fboH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // 透明清屏（alpha=0）
    glClear(GL_COLOR_BUFFER_BIT);

    // 混合状态（保险，防止外部状态污染）
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // 注意：GL_POINT_SPRITE / GL_PROGRAM_POINT_SIZE 是全局状态，initGL 已启用，无需每帧重设

    // 渲染器调用
    if (m_camera && m_renderer) {
        m_camera->update();
        m_renderer->renderFrame(dt);
        m_renderer->renderParticles(*m_camera);
    }

    // 清错误队列（防止旧错误堆积导致后续诊断误判）
    while (glGetError() != GL_NO_ERROR) { /* discard */ }

    // 直接呈现到屏幕（SwapInterval=1 时自动 VSync 等待）
    m_glContext->swapBuffers(m_window);

    // 🔧 [DWM 桌面空闲修复] 强制 DWM 立即合成这一帧
    //   根因：用户最小化最后一个普通窗口 → DWM 进入"桌面空闲"模式 → DWM 停止主动合成
    //         → swap chain 内容已经更新，但 DWM 不刷新到屏幕 → overlay 看起来"黑屏"
    //         → 用户点击屏幕（任何输入）唤醒 DWM 重新合成 → overlay 恢复可见
    //   修复：每帧 swapBuffers 后调用 DwmFlush() 强制 DWM 立即合成，绕过 DWM 空闲检测
    //   开销：DwmFlush 是同步阻塞调用，等待 DWM 完成一次合成（~1ms），可接受
    //   注意：DwmFlush 跨线程调用是安全的（DWM API 本身线程安全）
    DwmFlush();

    m_glContext->doneCurrent();
}
