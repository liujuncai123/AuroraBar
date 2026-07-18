// ============================================================================
// ⚠️  DEPRECATED — 本文件已废弃，请勿在新代码中使用
// ----------------------------------------------------------------------------
//  原方案：Win32 原生透明窗口（WS_EX_LAYERED + UpdateLayeredWindow + wgl）
//  原因  ：Win+D / 手动最小化时 DWM 切换合成管线，UpdateLayeredWindow 在内核
//          中阻塞 + wglMakeCurrent 临时失败 → 黑屏 / 未响应（多轮修复未根治）
//  替代  ：src/ui/QtOverlayWindow.cpp —— Qt6 QWidget + WA_TranslucentBackground
//          + 离屏 QOpenGLContext/FBO（DirectComposition，DWM 切换稳定）
//  说明  ：文件保留供历史参考，不再参与主代码路径；新代码请勿 include。
// ============================================================================
#ifdef _MSC_VER
#pragma message("warning: " __FILE__ " is DEPRECATED — use QtOverlayWindow instead")
#endif

/**
 * @file OverlayWindow.cpp
 * @brief 全分辨率 FBO 离屏渲染 + PBO 异步读回 + 独立提交线程
 * @date 2026-07-12（v3 — 分离提交线程，根治 Win+D 阻塞）
 * @details v3 架构：
 *   ┌─ 渲染线程（本类）─┐        ┌─ 提交线程（SubmitThread）─┐
 *   │ FBO 离屏渲染       │        │ 等 m_ready                │
 *   │ glReadPixels → PBO │        │ SetDIBits(自有 DC)        │
 *   │ → m_dibBuf         │ copy   │ UpdateLayeredWindow       │ ← 卡住只卡这里
 *   │ submit() ──────────┼───────→│ m_ready = false           │
 *   │ 继续下一帧（不阻塞）│        └──────────────────────────┘
 *   └────────────────────┘
 *
 *   Win+D 时 DWM 重建合成管线，提交线程中的 UpdateLayeredWindow 在内核中阻塞，
 *   但渲染线程完全不受影响，继续 60fps 渲染。DWM 恢复后自动刷新。
 */
#include "OverlayWindow.h"
#include "../logging/LoggerManager.h"
#include <GL/wglew.h>

OverlayWindow::OverlayWindow() { AURORA_TRACE("OverlayWindow", "Constructor"); }
OverlayWindow::~OverlayWindow() {
    // 安全：不在析构函数中写日志（LoggerManager 可能已 shutdown）
    destroy();
}

bool OverlayWindow::create(int borderTop, int borderBottom,
                           int borderLeft, int borderRight) {
    m_borderTop = borderTop; m_borderBottom = borderBottom;
    m_borderLeft = borderLeft; m_borderRight = borderRight;
    m_screenW = GetSystemMetrics(SM_CXSCREEN);
    m_screenH = GetSystemMetrics(SM_CYSCREEN);

    // 1. 创建隐藏辅助窗口（获取 GL 上下文）
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc); wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AuroraGLHelper";
    RegisterClassExA(&wc);

    HWND hwndGL = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        "AuroraGLHelper", "", WS_POPUP,
        0, 0, m_screenW, m_screenH,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwndGL) { AURORA_ERROR("OverlayWindow", "GL helper window failed"); return false; }

    HDC hdcGL = GetDC(hwndGL);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    pfd.cAlphaBits = 8; pfd.cDepthBits = 24;
    SetPixelFormat(hdcGL, ChoosePixelFormat(hdcGL, &pfd), &pfd);

    HGLRC tempCtx = wglCreateContext(hdcGL);
    wglMakeCurrent(hdcGL, tempCtx);

    typedef HGLRC (WINAPI *PWGLCREATE)(HDC, HGLRC, const int*);
    auto wglCreate = (PWGLCREATE)wglGetProcAddress("wglCreateContextAttribsARB");
    m_pfnWglCreateContextAttribsARB = wglCreate;
    if (wglCreate) {
        int attrs[] = { 0x2091,3, 0x2092,3,
                        0x2094, 0x00000004,
                        0x9126, 0x00000001,
                        0 };
        m_hglrc = wglCreate(hdcGL, nullptr, attrs);
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(tempCtx);
    m_hdc = hdcGL;
    m_glHelperWnd = hwndGL;

    if (!m_hglrc) { AURORA_ERROR("OverlayWindow", "GL 3.3 failed"); return false; }

    SetWindowLongPtrA(m_glHelperWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_origHelperProc = (WNDPROC)SetWindowLongPtrA(m_glHelperWnd, GWLP_WNDPROC,
                                                   reinterpret_cast<LONG_PTR>(&OverlayWindow::helperWndProc));

    makeCurrent();
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { AURORA_ERROR("OverlayWindow", "glewInit failed"); return false; }

    if (!initGLResources()) {
        AURORA_ERROR("OverlayWindow", "GL resources init failed");
        return false;
    }

    // 5. 创建可见窗口（WS_EX_LAYERED，全屏尺寸）
    wc.lpszClassName = "AuroraBarOverlay";
    RegisterClassExA(&wc);
    m_hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        "AuroraBarOverlay", "AuroraBar", WS_POPUP,
        0, 0, m_screenW, m_screenH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!m_hwnd) { AURORA_ERROR("OverlayWindow", "CreateWindowEx failed"); return false; }

    // 6. 启动独立提交线程
    if (!m_submitThread.start(m_hwnd, m_screenW, m_screenH)) {
        AURORA_ERROR("OverlayWindow", "SubmitThread start failed");
        return false;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    AURORA_INFO("OverlayWindow", "FBO {}x{} created + SubmitThread active", m_screenW, m_screenH);
    return true;
}

bool OverlayWindow::initGLResources() {
    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    glGenTextures(1, &m_fboTex);
    glBindTexture(GL_TEXTURE_2D, m_fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_screenW, m_screenH,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fboTex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        AURORA_ERROR("OverlayWindow", "FBO incomplete");
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glGenBuffers(2, m_pbo);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, m_screenW * m_screenH * 4, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    m_dibSize = m_screenW * m_screenH * 4;
    m_dibBuf.resize(m_dibSize);

    return true;
}

bool OverlayWindow::isContextValid() {
    // 安全：真正的显示状态变化（分辨率/刷新率/电源/设备）→ 需要重建上下文
    //       WM_DISPLAYCHANGE / WM_POWERBROADCAST / WM_DEVICECHANGE 触发
    //       注意：WM_WININICHANGE 不在此列，DWM 合成切换不影响 GL 上下文
    if (m_displayChanged.exchange(false, std::memory_order_acq_rel)) {
        AURORA_WARN("OverlayWindow", "Display state changed — context rebuild required");
        return false;
    }

    // DWM 过渡期间（Win+D 隐藏/恢复）跳过本帧渲染（防御性措施）
    if (isDwmTransitioning()) {
        return false;
    }

    // 安全：只检查上下文句柄是否存在，不调用 makeCurrent()。
    //       原因：wglMakeCurrent 在 DWM 切换（Win+D/最小化）后可能临时失败（返回 FALSE），
    //       但这不代表上下文真的丢失——DWM 恢复后 makeCurrent 会成功。
    //       如果在这里调用 makeCurrent 并返回 false，RenderThread 会
    //       调用 setVisible(false) 隐藏窗口 → 黑屏！
    //       让 onRunBody() 的 Phase 3 makeCurrent 处理，失败时跳过这一帧（不隐藏窗口）。
    if (!m_hglrc || !m_hdc) return false;

    return true;
}

bool OverlayWindow::rebuildContext() {
    AURORA_WARN("OverlayWindow", "Context lost, attempting rebuild...");

    cleanupGL();

    if (!m_hdc) {
        m_hdc = GetDC(m_glHelperWnd);
        if (!m_hdc) {
            AURORA_ERROR("OverlayWindow", "Rebuild: GetDC failed");
            return false;
        }
    }

    if (m_pfnWglCreateContextAttribsARB) {
        int attrs[] = { 0x2091,3, 0x2092,3,
                        0x2094, 0x00000004,
                        0x9126, 0x00000001,
                        0 };
        m_hglrc = m_pfnWglCreateContextAttribsARB(m_hdc, nullptr, attrs);
    } else {
        AURORA_WARN("OverlayWindow", "Rebuild: cached proc unavailable, fallback to wglCreateContext");
        m_hglrc = wglCreateContext(m_hdc);
    }

    if (!m_hglrc) {
        AURORA_ERROR("OverlayWindow", "Rebuild: GL context creation failed");
        return false;
    }

    makeCurrent();
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { AURORA_ERROR("OverlayWindow", "Rebuild: glewInit failed"); return false; }

    m_fbo = 0; m_fboTex = 0;
    m_pbo[0] = m_pbo[1] = 0;
    m_pboWriteIdx = 0;
    m_readyFrames = 0;

    if (!initGLResources()) {
        AURORA_ERROR("OverlayWindow", "Rebuild: GL resources init failed");
        cleanupGL();
        return false;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    AURORA_INFO("OverlayWindow", "Context rebuilt OK {}x{}", m_screenW, m_screenH);
    return true;
}

void OverlayWindow::cleanupGL() {
    // 安全：不调用 glDelete*，避免在僵尸上下文上挂起。
    //       wglDeleteContext 会自动清理该上下文中的所有 GL 对象（FBO/纹理/PBO），无需手动释放。
    //       仅清零 C++ 侧 ID，防止后续误用。
    m_pbo[0] = m_pbo[1] = 0;
    m_fbo = 0; m_fboTex = 0;
    if (m_hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(m_hglrc);
        m_hglrc = nullptr;
    }
}

void OverlayWindow::destroy() {
    // 安全：先停止提交线程，再清理 GL 资源
    m_submitThread.stop();

    if (m_glHelperWnd && m_origHelperProc) {
        SetWindowLongPtrA(m_glHelperWnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(m_origHelperProc));
        SetWindowLongPtrA(m_glHelperWnd, GWLP_USERDATA, 0);
        m_origHelperProc = nullptr;
    }
    if (m_hdc)   { ReleaseDC(m_glHelperWnd, m_hdc); m_hdc = nullptr; }
    if (m_glHelperWnd) { DestroyWindow(m_glHelperWnd); m_glHelperWnd = nullptr; }
    if (m_hwnd)  { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

LRESULT CALLBACK OverlayWindow::helperWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(
        GetWindowLongPtrA(hWnd, GWLP_USERDATA));
    if (self) {
        switch (msg) {
        case WM_DISPLAYCHANGE:
            self->m_displayChanged.store(true, std::memory_order_release);
            AURORA_INFO("OverlayWindow", "WM_DISPLAYCHANGE: resolution/refresh changed");
            break;
        case WM_POWERBROADCAST:
            if (wParam == 7 || wParam == 18 || wParam == 4) {
                self->m_displayChanged.store(true, std::memory_order_release);
                AURORA_INFO("OverlayWindow", "WM_POWERBROADCAST: power event wParam={}",
                            static_cast<unsigned long>(wParam));
            }
            break;
        case WM_DEVICECHANGE:
            self->m_displayChanged.store(true, std::memory_order_release);
            AURORA_INFO("OverlayWindow", "WM_DEVICECHANGE: device config changed");
            break;
        case WM_WININICHANGE: {
            // 安全：WM_WININICHANGE 在很多场景下发送（壁纸/颜色/主题/环境变量改变等），
            //       绝大多数与 DWM 合成模式切换无关，也不影响 OpenGL 上下文。
            //       之前错误地对此消息调用 markDwmTransition()，导致 500ms 黑屏。
            //       现在只记录日志，不做任何处理。
            //       上下文有效性由 isContextValid() → makeCurrent() 真正检测。
            const char* section = "(null)";
            if (lParam) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(lParam), &mbi, sizeof(mbi)) >= sizeof(mbi) &&
                    (mbi.State & MEM_COMMIT) &&
                    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                    section = reinterpret_cast<const char*>(lParam);
                } else {
                    section = "(invalid_ptr)";
                }
            }
            AURORA_INFO("OverlayWindow", "WM_WININICHANGE section={} (ignored)", section);
            break;
        }
        case WM_DWMCOMPOSITIONCHANGED:
            // 安全：DWM 合成状态真正变化（如 DWM 被禁用/启用），标记短暂过渡期。
            //       Win+D 不会触发此消息，只有显式切换 DWM 合成模式才会。
            //       过渡期缩短为 100ms（仅跳过几帧防御性措施），不触发上下文重建。
            self->markDwmTransition();
            AURORA_INFO("OverlayWindow", "WM_DWMCOMPOSITIONCHANGED — dwmTransitioning=true (100ms)");
            break;
        }
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

bool OverlayWindow::makeCurrent() {
    if (!m_hdc || !m_hglrc) return false;
    return wglMakeCurrent(m_hdc, m_hglrc) == TRUE;
}

bool OverlayWindow::beginFrame() {
    if (!wglGetCurrentContext()) {
        AURORA_WARN("OverlayWindow", "beginFrame: no current context");
        m_frameValid = false;
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_screenW, m_screenH);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    m_frameValid = true;
    return true;
}

void OverlayWindow::endFrame() {
    if (!m_frameValid) return;
    if (!wglGetCurrentContext()) {
        m_frameValid = false;
        return;
    }

    // === Phase 1: 从上一帧的 PBO 取回数据（永不阻塞） ===
    // 安全：使用 glMapBufferRange + GL_MAP_UNSYNCHRONIZED_BIT，告诉驱动不要等待 GPU。
    //       这避免了 glMapBuffer 在 GPU 不响应时（DWM 切换/长时间运行后）无限挂起。
    //       代价：GPU 未完成写入时可能读到旧/不完整数据（短暂花屏），但不会挂起。
    //       双缓冲 PBO 确保正常情况下读取的 PBO 已被 GPU 完成写入（上一帧的）。
    int readIdx = (m_pboWriteIdx + 1) % 2;
    if (m_readyFrames >= 2) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readIdx]);
        void* ptr = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, m_dibSize,
                                      GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (ptr) {
            memcpy(m_dibBuf.data(), ptr, m_dibSize);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_submitThread.submit(m_dibBuf);
    }

    // === Phase 2: 当前帧异步写入 PBO ===
    // glReadPixels with PBO（pixels=nullptr）是异步的，不阻塞
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_pboWriteIdx]);
    glReadPixels(0, 0, m_screenW, m_screenH, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_pboWriteIdx = readIdx;
    if (m_readyFrames < 2) m_readyFrames++;
}

void OverlayWindow::setVisible(bool visible) {
    if (m_hwnd) ShowWindow(m_hwnd, visible ? SW_SHOW : SW_HIDE);
}