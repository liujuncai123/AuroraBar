/**
 * @file GlContext.cpp
 * @brief WGL OpenGL 3.3 上下文实现
 * @date 2026-07-06
 */

#include "GlContext.h"
#include "../logging/LoggerManager.h"
#include <GL/glew.h>

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB         0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);

GlContext::GlContext() { AURORA_TRACE("GlContext", "Constructor"); }
GlContext::~GlContext() { AURORA_TRACE("GlContext", "Destructor"); destroy(); }

bool GlContext::create(int width, int height) {
    m_width = width; m_height = height;

    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AuroraBar_GL";
    RegisterClassExA(&wc);

    m_hwnd = CreateWindowExA(0, "AuroraBar_GL", "GL",
                             WS_POPUP, 0, 0, width, height,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!m_hwnd) {
        AURORA_ERROR("GlContext", "CreateWindowEx failed");
        return false;
    }

    m_hdc = GetDC(m_hwnd);

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    int pf = ChoosePixelFormat(m_hdc, &pfd);
    SetPixelFormat(m_hdc, pf, &pfd);

    // 创建临时上下文以获取 wglCreateContextAttribsARB
    HGLRC tempCtx = wglCreateContext(m_hdc);
    wglMakeCurrent(m_hdc, tempCtx);

    auto wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

    if (wglCreateContextAttribsARB) {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_FLAGS_ARB, 0,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        m_hglrc = wglCreateContextAttribsARB(m_hdc, nullptr, attribs);
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(tempCtx);

    if (!m_hglrc) {
        AURORA_ERROR("GlContext", "wglCreateContextAttribsARB failed (GL 3.3 required)");
        return false;
    }

    makeCurrent();
    glewExperimental = GL_TRUE;
    GLenum glewErr = glewInit();
    if (glewErr != GLEW_OK) {
        AURORA_ERROR("GlContext", "glewInit failed: {}", reinterpret_cast<const char*>(glewGetErrorString(glewErr)));
        return false;
    }
    AURORA_INFO("GlContext", "WGL 3.3 Core created {}x{}", width, height);
    return true;
}

void GlContext::destroy() {
    if (m_hglrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(m_hglrc); m_hglrc = nullptr; }
    if (m_hdc)   { ReleaseDC(m_hwnd, m_hdc); m_hdc = nullptr; }
    if (m_hwnd)  { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

void GlContext::makeCurrent() {
    if (m_hdc && m_hglrc) wglMakeCurrent(m_hdc, m_hglrc);
}

void GlContext::swapBuffers() {
    if (m_hdc) SwapBuffers(m_hdc);
}

unsigned GlContext::compileProgram(const char* vertSrc, const char* fragSrc) {
    auto compile = [](unsigned type, const char* src) -> unsigned {
        unsigned s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(s, 511, nullptr, log); AURORA_ERROR("GlContext", "Shader compile: {}", log); glDeleteShader(s); return 0; }
        return s;
    };
    unsigned vs = compile(GL_VERTEX_SHADER, vertSrc);
    unsigned fs = compile(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) return 0;

    unsigned prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    int ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!ok) { char log[512]; glGetProgramInfoLog(prog, 511, nullptr, log); AURORA_ERROR("GlContext", "Program link: {}", log); glDeleteProgram(prog); return 0; }
    return prog;
}
