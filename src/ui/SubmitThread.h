// ============================================================================
// ⚠️  DEPRECATED — 本文件已废弃，请勿在新代码中使用
// ----------------------------------------------------------------------------
//  原方案：独立提交线程隔离 UpdateLayeredWindow 的 DWM 阻塞（旧 Win32 方案）
//  原因  ：UpdateLayeredWindow 在 DWM 切换时阻塞的本质问题无法靠线程隔离
//          根治，叠加层仍会黑屏。已整体放弃 Win32 透明窗口方案。
//  替代  ：src/ui/QtOverlayWindow —— Qt6 DirectComposition 方案，无 ULW。
//  说明  ：文件保留供历史参考，不再参与主代码路径；新代码请勿 include。
// ============================================================================
#ifdef _MSC_VER
#pragma message("warning: " __FILE__ " is DEPRECATED — use QtOverlayWindow instead")
#endif

/**
 * @file SubmitThread.h
 * @brief 独立提交线程 — 在专有线程中执行 UpdateLayeredWindow，隔离 DWM 阻塞
 * @date 2026-07-17
 * @details Win+D 时 DWM 切换合成模式，UpdateLayeredWindow 可能在内核中
 *          无限等待 DWM 响应（请求被 DWM 重建合成管线时丢弃）。
 *          将 ULW 与渲染线程分离：渲染线程永不阻塞，DWM 恢复后自动刷新。
 *
 *          架构：
 *          ┌─ 渲染线程 ─┐      ┌─ 提交线程（本类）─┐
 *          │ glReadPixels │      │ 等 m_ready         │
 *          │ → m_dibBuf   │ copy │ SetDIBits(自有DC)  │
 *          │ submit() ────┼─────→│ UpdateLayeredWindow│ ← 卡住只卡这里
 *          │ 继续下一帧   │      │ m_ready = false    │
 *          └──────────────┘      └────────────────────┘
 *
 * @note 安全：GDI 对象线程亲和，提交线程拥有自己的 DC/Bitmap，不与渲染线程共享。
 *           提交线程不持有任何 GL 资源，上下文丢失不会影响它。
 *           用 condition_variable 等待，空闲时 CPU 占用 ≈ 0%。
 */
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

class SubmitThread {
public:
    SubmitThread() = default;
    ~SubmitThread();

    // 禁止拷贝
    SubmitThread(const SubmitThread&) = delete;
    SubmitThread& operator=(const SubmitThread&) = delete;

    /**
     * @brief 启动提交线程并初始化 GDI 资源
     * @param hwnd 分层窗口句柄（UpdateLayeredWindow 的目标）
     * @param screenW 屏幕宽度
     * @param screenH 屏幕高度
     * @return true 成功
     */
    bool start(HWND hwnd, int screenW, int screenH);

    /**
     * @brief 提交新帧（渲染线程调用，永不阻塞）
     * @param pixelData BGRA 像素数据（screenW * screenH * 4 bytes）
     * @note 如果上一帧还在处理中，跳过本帧（渲染线程不等待）
     */
    void submit(const std::vector<uint8_t>& pixelData);

    /// @brief 停止提交线程并释放 GDI 资源
    void stop();

    /// @brief 检查提交线程是否正在运行
    bool running() const { return m_running.load(std::memory_order_acquire); }

    /// @brief 是否有待处理的帧
    bool pending() const { return m_ready.load(std::memory_order_acquire); }

private:
    void run();             ///< 线程主循环
    void initGDI();         ///< 初始化 GDI 资源
    void cleanupGDI();      ///< 清理 GDI 资源

    HWND m_hwnd = nullptr;
    int  m_screenW = 0, m_screenH = 0;

    // 提交线程拥有的 GDI 资源（线程亲和，不与渲染线程共享）
    HDC       m_screenDC = nullptr;
    HDC       m_memDC = nullptr;
    HBITMAP   m_hBmp = nullptr;
    BITMAPINFO m_bi{};

    // 共享缓冲区
    std::vector<uint8_t> m_buffer;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_ready{false};  // 原子变量：pending() 无锁读取，submit()/run() 在 mutex 下写入

    std::atomic<bool> m_running{false};
    std::thread m_thread;
};