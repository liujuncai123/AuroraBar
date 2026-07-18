// ============================================================================
// ⚠️  DEPRECATED — 本文件已废弃，请勿在新代码中使用
// ----------------------------------------------------------------------------
//  原方案：Win32 原生透明窗口（WS_EX_LAYERED + UpdateLayeredWindow + wgl）
//  原因  ：Win+D / 手动最小化时 DWM 切换合成管线，UpdateLayeredWindow 在内核
//          中阻塞 + wglMakeCurrent 临时失败 → 黑屏 / 未响应（多轮修复未根治）
//  替代  ：src/ui/QtOverlayWindow.h —— Qt6 QWidget + WA_TranslucentBackground
//          + 离屏 QOpenGLContext/FBO（DirectComposition，DWM 切换稳定）
//  说明  ：文件保留供历史参考，不再参与主代码路径；新代码请勿 include。
// ============================================================================
#ifdef _MSC_VER
#pragma message("warning: " __FILE__ " is DEPRECATED — use QtOverlayWindow instead")
#endif

/**
 * @file OverlayWindow.h
 * @brief 真正透明叠加窗口——FBO（全分辨率）+ PBO 异步读回 + 独立提交线程
 * @date 2026-07-06
 * @details v1.4: 分离提交线程（SubmitThread）
 *   - UpdateLayeredWindow 在独立线程中执行，渲染线程永不阻塞
 *   - Win+D / DWM 合成切换时 ULW 卡住只卡提交线程，渲染继续 60fps
 *   - GDI 资源（DC/Bitmap）由提交线程独占，线程亲和，不跨线程共享
 * @note 安全：所有 GL 操作前需通过 isContextValid() / makeCurrent() 验证上下文
 */
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/glew.h>
#include <vector>
#include <cstdint>
#include <atomic>
#include <chrono>
#include "SubmitThread.h"

/// wglCreateContextAttribsARB 函数指针类型
typedef HGLRC (WINAPI *PWGLCREATECONTEXTATTRIBSARB)(HDC, HGLRC, const int*);

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();
    bool create(int borderTop = 100, int borderBottom = 100,
                int borderLeft = 100, int borderRight = 100);
    void destroy();
    bool makeCurrent();  ///< @return true 成功，false 上下文已失效
    /// @brief 解除当前上下文绑定（不调用 GL 操作，安全）
    void detachContext() { wglMakeCurrent(nullptr, nullptr); }
    /// @brief 绑定 FBO + 僵尸上下文检测
    /// @return true 可继续渲染；false 上下文僵尸化，调用方应标记 contextLost 跳过本帧
    bool beginFrame();
    /// @brief PBO 异步读回 → 通知 SubmitThread → 立即返回（不阻塞）
    void endFrame();
    void setVisible(bool visible);
    void cleanupGL();    ///< 清理 FBO/纹理/PBO/GL 上下文

    bool valid() const { return m_hglrc != nullptr; }
    bool isContextValid();              ///< 检测 OpenGL 上下文是否有效
    bool rebuildContext();             ///< 重建 GL 上下文和所有资源（上下文丢失时调用）
    int screenW() const { return m_screenW; }
    int screenH() const { return m_screenH; }
    HWND hwnd() const { return m_hwnd; }
    /// @brief 检查并清除"显示模式变化"标志（WM_DISPLAYCHANGE / WM_POWERBROADCAST 触发）
    /// @return true 表示系统显示状态发生过变化，调用方应触发上下文重建
    bool checkAndClearDisplayChanged() {
        return m_displayChanged.exchange(false, std::memory_order_acq_rel);
    }
    /// @brief 检查是否处于 DWM 过渡期（WM_DWMCOMPOSITIONCHANGED 触发）
    /// @return true 表示 DWM 正在切换合成模式，应跳过本帧渲染
    /// @note 过渡期间跳过渲染（100ms 防御性措施），过渡结束后不触发重建。
    ///       DWM 合成模式切换不影响 OpenGL 上下文（上下文绑定到窗口 DC，与 DWM 无关）。
    ///       过渡结束后 isContextValid() 通过 makeCurrent() 真正检测上下文有效性。
    ///       wglMakeCurrent() 在僵尸上下文上会返回 FALSE，不会挂起。
    /// @warning 不要在过渡结束时设置 m_displayChanged，那会导致不必要的上下文重建
    ///          和窗口隐藏（黑屏）。之前就是因为这个逻辑导致 Win+D 恢复后黑屏。
    bool isDwmTransitioning() const {
        if (!m_dwmTransitioning.load(std::memory_order_acquire)) return false;
        auto now = std::chrono::steady_clock::now();
        if (now - m_dwmTransitionTime < std::chrono::milliseconds(100)) {
            return true;
        }
        // 过渡结束，不触发重建。让 isContextValid() 通过 makeCurrent() 真正检测。
        m_dwmTransitioning.store(false, std::memory_order_release);
        return false;
    }
    /// @brief 标记 DWM 过渡开始（由 helperWndProc 在 WM_DWMCOMPOSITIONCHANGED 中调用）
    void markDwmTransition() {
        m_dwmTransitioning.store(true, std::memory_order_release);
        m_dwmTransitionTime = std::chrono::steady_clock::now();
    }

private:
    bool initGLResources();            ///< 初始化 FBO/PBO 等 GL 资源
    /// @brief 辅助窗口的 WndProc，处理系统显示/电源消息
    static LRESULT CALLBACK helperWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND   m_hwnd = nullptr, m_glHelperWnd = nullptr;
    HDC    m_hdc = nullptr;
    HGLRC  m_hglrc = nullptr;
    int    m_screenW = 1920, m_screenH = 1080;
    int    m_borderTop = 100, m_borderBottom = 100;
    int    m_borderLeft = 100, m_borderRight = 100;

    // 安全：缓存 wglCreateContextAttribsARB 函数指针（在 create() 时一次性获取）
    //       避免 rebuildContext() 调用 wglMakeCurrent(旧僵尸上下文) 阻塞卡死
    PWGLCREATECONTEXTATTRIBSARB m_pfnWglCreateContextAttribsARB = nullptr;
    WNDPROC m_origHelperProc = nullptr;  ///< 原始 DefWindowProc，用于链式调用

    // FBO 相关（全分辨率）
    unsigned m_fbo = 0, m_fboTex = 0;
    std::vector<uint8_t> m_dibBuf;   ///< 像素缓冲区（glReadPixels 写入，提交线程拷贝）
    size_t m_dibSize = 0;

    // PBO 双缓冲异步读回（消除 GPU stall，不影响画质）
    // 安全：使用 glMapBufferRange + GL_MAP_UNSYNCHRONIZED_BIT 读取 PBO，永不阻塞。
    //       双缓冲确保读取的 PBO 不是 GPU 正在写入的（正常情况下）。
    //       GPU 不响应时可能读到旧数据（花屏），但不会挂起。
    unsigned m_pbo[2] = {0, 0};
    int     m_pboWriteIdx = 0;
    int     m_readyFrames = 0;

    // 安全：显示状态变化标志，由 WndProc 设置，isContextValid() 检查
    //       WM_DISPLAYCHANGE / WM_POWERBROADCAST / WM_DEVICECHANGE 触发
    //       这些是真正的显示状态变化，需要重建 GL 上下文
    //       注意：WM_WININICHANGE 不触发此标志（太普遍，与显示状态无关）
    //       mutable：保留以兼容历史代码，当前无 const 方法修改此变量
    mutable std::atomic<bool> m_displayChanged{false};

    // 安全：DWM 合成模式切换标志（WM_DWMCOMPOSITIONCHANGED 触发）
    //       仅在 DWM 被显式禁用/启用时发送，Win+D 不触发此消息。
    //       过渡期间跳过本帧渲染（100ms 防御性措施），过渡结束后不触发重建。
    //       isContextValid() 通过 makeCurrent() 真正检测上下文有效性。
    //       ⚠️ 不要在过渡结束时设置 m_displayChanged，那会导致不必要的上下文重建和黑屏。
    //       mutable + atomic：主线程（WndProc）写，渲染线程（isDwmTransitioning）读
    mutable std::atomic<bool> m_dwmTransitioning{false};
    mutable std::chrono::steady_clock::time_point m_dwmTransitionTime{};

    // 安全：本帧有效性标志，由 beginFrame() 设置，endFrame() 检查
    //       beginFrame 检测到僵尸上下文时置 false，endFrame 据此跳过 glReadPixels
    //       防止在僵尸上下文上 glReadPixels 无限挂起（不被 SEH 捕获）
    bool m_frameValid = false;

    // 独立提交线程 — UpdateLayeredWindow 在专有线程中执行
    // 优点：Win+D / DWM 合成切换时 ULW 阻塞只影响提交线程，渲染线程继续 60fps
    SubmitThread m_submitThread;
};