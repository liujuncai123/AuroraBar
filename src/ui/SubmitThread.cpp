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
 * @file SubmitThread.cpp
 * @brief 独立提交线程实现
 * @date 2026-07-17
 */
#include "SubmitThread.h"
#include "../logging/LoggerManager.h"
#include <chrono>

SubmitThread::~SubmitThread() {
    stop();
}

// ============================================================
//  GDI 资源管理（提交线程专有，不与渲染线程共享）
// ============================================================

void SubmitThread::initGDI() {
    // 安全：GetDC(nullptr) 获取整个屏幕的 DC，用作 CreateCompatibleDC 的模板
    //       不用于读取像素，仅用于创建兼容的 DC/Bitmap
    m_screenDC = GetDC(nullptr);
    if (!m_screenDC) {
        AURORA_ERROR("SubmitThread", "GetDC(nullptr) failed");
        return;
    }
    m_memDC = CreateCompatibleDC(m_screenDC);
    if (!m_memDC) {
        AURORA_ERROR("SubmitThread", "CreateCompatibleDC failed");
        ReleaseDC(nullptr, m_screenDC);
        m_screenDC = nullptr;
        return;
    }
    m_hBmp = CreateCompatibleBitmap(m_screenDC, m_screenW, m_screenH);
    if (!m_hBmp) {
        AURORA_ERROR("SubmitThread", "CreateCompatibleBitmap {}x{} failed", m_screenW, m_screenH);
        DeleteDC(m_memDC);
        m_memDC = nullptr;
        ReleaseDC(nullptr, m_screenDC);
        m_screenDC = nullptr;
        return;
    }

    memset(&m_bi, 0, sizeof(m_bi));
    m_bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    m_bi.bmiHeader.biWidth  = m_screenW;
    m_bi.bmiHeader.biHeight = -m_screenH;   // 负值 = top-down DIB
    m_bi.bmiHeader.biPlanes = 1;
    m_bi.bmiHeader.biBitCount = 32;
    m_bi.bmiHeader.biCompression = BI_RGB;

    AURORA_INFO("SubmitThread", "GDI resources initialized {}x{}", m_screenW, m_screenH);
}

void SubmitThread::cleanupGDI() {
    // 安全：run() 主循环每次 SelectObject 后都恢复了 oldBmp，
    //       所以正常退出时 m_hBmp 已从 m_memDC 中取消选中，可直接删除。
    //       若 SelectObject 失败（oldBmp==NULL），m_hBmp 未被选中，同样安全。
    if (m_hBmp)  { DeleteObject(m_hBmp); m_hBmp = nullptr; }
    if (m_memDC) { DeleteDC(m_memDC); m_memDC = nullptr; }
    if (m_screenDC) { ReleaseDC(nullptr, m_screenDC); m_screenDC = nullptr; }
}

// ============================================================
//  生命周期管理
// ============================================================

bool SubmitThread::start(HWND hwnd, int screenW, int screenH) {
    m_hwnd = hwnd;
    m_screenW = screenW;
    m_screenH = screenH;
    m_buffer.resize(static_cast<size_t>(screenW) * screenH * 4);

    initGDI();
    if (!m_screenDC || !m_memDC || !m_hBmp) {
        AURORA_ERROR("SubmitThread", "GDI init failed");
        return false;
    }

    m_running = true;
    m_thread = std::thread(&SubmitThread::run, this);
    AURORA_INFO("SubmitThread", "Started ({}x{})", screenW, screenH);
    return true;
}

void SubmitThread::stop() {
    if (!m_running.load(std::memory_order_acquire)) return;

    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ready = true;  // 唤醒可能在等待 cv 的线程
    }
    m_cv.notify_one();

    // 安全：如果 UpdateLayeredWindow 在内核中阻塞（DWM 切换合成模式时），
    //       cv 通知无法唤醒线程。但 DWM 有内部超时（通常 5-10s），ULW 最终会返回。
    //       此处直接 join() 等待线程自然退出，不 detach。
    //       detach 的风险：线程从 ULW 返回后访问 m_running/m_memDC 等成员变量，
    //       若 SubmitThread 已析构则 use-after-free 崩溃。
    if (m_thread.joinable()) {
        m_thread.join();
    }

    cleanupGDI();
    AURORA_INFO("SubmitThread", "Stopped");
}

// ============================================================
//  渲染线程接口
// ============================================================

void SubmitThread::submit(const std::vector<uint8_t>& pixelData) {
    // 安全：如果上一帧还在处理中（m_ready=true），跳过本帧
    //       渲染线程不等待，继续下一帧。DWM 恢复后自动显示最新帧。
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ready) return;  // 上一帧还在提交，跳过

    m_buffer = pixelData;
    m_ready = true;
    m_cv.notify_one();
}

// ============================================================
//  提交线程主循环
// ============================================================

void SubmitThread::run() {
    // 安全：降低线程优先级，避免与渲染线程争抢 CPU
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    int submittedCount = 0;
    int skippedCount = 0;

    while (m_running.load(std::memory_order_acquire)) {
        // 等待新帧
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_ready || !m_running.load(std::memory_order_acquire);
            });
            if (!m_running.load(std::memory_order_acquire)) break;
        }

        // 安全：在此处调用 UpdateLayeredWindow 可能阻塞（DWM 切换合成模式时）
        //       但只阻塞本线程，渲染线程不受影响。
        //       DWM 恢复后 ULW 自动返回，下帧正常显示。
        if (IsWindow(m_hwnd)) {
            // SelectObject + SetDIBits + UpdateLayeredWindow
            HBITMAP oldBmp = (HBITMAP)SelectObject(m_memDC, m_hBmp);
            if (oldBmp) {
                SetDIBits(m_memDC, m_hBmp, 0, m_screenH,
                          m_buffer.data(), &m_bi, DIB_RGB_COLORS);

                BLENDFUNCTION blend{};
                blend.BlendOp = AC_SRC_OVER;
                blend.SourceConstantAlpha = 255;
                blend.AlphaFormat = AC_SRC_ALPHA;

                POINT ptSrc = {0, 0};
                SIZE  sz    = {m_screenW, m_screenH};
                POINT ptDst = {0, 0};

                // 这里就是整个架构中唯一可能阻塞的调用
                // Win+D 时 DWM 重建合成管线，此调用在内核中等待 DWM 响应
                // 但只阻塞提交线程，渲染线程继续 60fps 运行
                UpdateLayeredWindow(m_hwnd, m_screenDC, &ptDst, &sz,
                                   m_memDC, &ptSrc, 0, &blend, ULW_ALPHA);

                SelectObject(m_memDC, oldBmp);
                submittedCount++;
            } else {
                // SelectObject 失败（GDI 资源可能已损坏）
                AURORA_WARN("SubmitThread", "SelectObject failed — GDI resources may be corrupted");
                cleanupGDI();
                initGDI();
                if (!m_memDC || !m_hBmp) {
                    AURORA_ERROR("SubmitThread", "GDI recovery failed, stopping");
                    break;
                }
            }
        } else {
            // 窗口无效（可能被销毁），跳过
            skippedCount++;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ready = false;
        }
    }

    AURORA_INFO("SubmitThread", "Exiting — submitted={} skipped={}", submittedCount, skippedCount);
}