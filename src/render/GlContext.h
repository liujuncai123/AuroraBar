/**
 * @file GlContext.h
 * @brief 最小化 WGL OpenGL 3.3 Core 上下文封装
 * @date 2026-07-06
 * @details 创建隐藏 Win32 窗口 + WGL 上下文，提供编译着色器/上传数据/绘制能力。
 *          不依赖 Qt，纯 Win32 + WGL，可在任意线程使用。
 * @note 线程安全：单线程独占（创建/使用/销毁在同一线程）。
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/glew.h>
#include <string>

/// @brief WGL OpenGL 3.3 上下文
class GlContext {
public:
    GlContext();
    ~GlContext();

    /// @brief 创建隐藏窗口 + WGL 上下文
    bool create(int width = 640, int height = 480);

    /// @brief 销毁 GL 资源
    void destroy();

    /// @brief 激活上下文
    void makeCurrent();

    /// @brief 交换缓冲区
    void swapBuffers();

    /// @brief 编译着色器程序
    unsigned compileProgram(const char* vertSrc, const char* fragSrc);

    /// @brief 是否有效
    bool valid() const { return m_hglrc != nullptr; }

    int width()  const { return m_width; }
    int height() const { return m_height; }

private:
    HWND   m_hwnd   = nullptr;
    HDC    m_hdc    = nullptr;
    HGLRC  m_hglrc  = nullptr;
    int    m_width  = 640;
    int    m_height = 480;
};
