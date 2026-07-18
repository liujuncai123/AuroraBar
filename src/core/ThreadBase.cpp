/**
 * @file ThreadBase.cpp
 * @brief ThreadBase 实现
 * @date 2026-07-06
 */

#include "ThreadBase.h"
#include "../logging/LoggerManager.h"

namespace {
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds(1);
}

ThreadBase::ThreadBase(std::string name)
    : m_name(std::move(name))
{
    AURORA_TRACE("ThreadBase", "Constructor name={}", m_name);
}

ThreadBase::~ThreadBase() {
    // 安全：不在析构函数中写日志。
    //   原因：析构函数在 main() 返回后的静态析构阶段执行，
    //   此时 LoggerManager 可能已被 shutdown()（spdlog::default_logger() 返回 nullptr），
    //   调用 AURORA_TRACE/AURORA_INFO 会导致 0xc0000005 空指针崩溃。
    if (m_thread.joinable()) {
        requestStop();
        join();
    }
}

Result<void> ThreadBase::start() {
    AURORA_INFO("ThreadBase", "start() name={}", m_name);

    if (m_state.load(std::memory_order_acquire) != ThreadState::Uninitialized) {
        AURORA_ERROR("ThreadBase", "start() failed name={} reason=already_started", m_name);
        return Result<void>::Err(
            MAKE_ERROR(ErrorCode::kInternalError,
                "Thread " + m_name + " already started"));
    }

    m_state.store(ThreadState::Initializing, std::memory_order_release);
    m_thread = std::thread(&ThreadBase::threadFunc, this);

    AURORA_TRACE("ThreadBase", "start() ok name={}", m_name);
    return Result<void>::Ok();
}

void ThreadBase::requestStop() {
    AURORA_INFO("ThreadBase", "requestStop() name={}", m_name);

    auto expected = ThreadState::Running;
    m_state.compare_exchange_strong(expected, ThreadState::Stopping,
        std::memory_order_release, std::memory_order_acquire);

    expected = ThreadState::Paused;
    m_state.compare_exchange_strong(expected, ThreadState::Stopping,
        std::memory_order_release, std::memory_order_acquire);

    expected = ThreadState::Initializing;
    m_state.compare_exchange_strong(expected, ThreadState::Stopping,
        std::memory_order_release, std::memory_order_acquire);
}

void ThreadBase::requestPause() {
    AURORA_INFO("ThreadBase", "requestPause() name={}", m_name);
    auto expected = ThreadState::Running;
    m_state.compare_exchange_strong(expected, ThreadState::Pausing,
        std::memory_order_release, std::memory_order_acquire);
}

void ThreadBase::requestResume() {
    AURORA_INFO("ThreadBase", "requestResume() name={}", m_name);
    auto expected = ThreadState::Paused;
    m_state.compare_exchange_strong(expected, ThreadState::Running,
        std::memory_order_release, std::memory_order_acquire);
}

void ThreadBase::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    AURORA_INFO("ThreadBase", "join() name={} done", m_name);
}

bool ThreadBase::timedJoin(int timeoutMs) {
    if (!m_thread.joinable()) return true;

    // Phase 1: 短眠让线程自然完成当前迭代
    // 2ms 覆盖 Logic (1ms sleep) 和 Render (先检查 shouldRun 再睡)
    constexpr int PHASE1_SLEEP_MS = 2;
    const int phase1Sleep = (timeoutMs / 2 < PHASE1_SLEEP_MS) ? (timeoutMs / 2) : PHASE1_SLEEP_MS;
    std::this_thread::sleep_for(std::chrono::milliseconds(phase1Sleep));

    // Phase 2: yield 轮询剩余时间，不受 Windows 时钟分辨率 (15.6ms) 影响
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (m_state.load(std::memory_order_acquire) == ThreadState::Stopped) {
            m_thread.join();
            AURORA_INFO("ThreadBase", "timedJoin() name={} ok ({}ms)", m_name, timeoutMs);
            return true;
        }
        // yield 让出剩余时间片，不依赖时钟粒度：
        //   - 有线程在等 CPU → 调度器立刻切换，目标线程更快 clean up
        //   - 没有其他线程 → 立刻返回，继续轮询
        //   - 不会导致饥饿：yield 不改变线程的就绪状态，下一滴答正常调度
        std::this_thread::yield();
    }

    // 超时：最后一次确认（刚好在 deadline 瞬间停的边界情况）
    if (m_state.load(std::memory_order_acquire) == ThreadState::Stopped) {
        m_thread.join();
        AURORA_INFO("ThreadBase", "timedJoin() name={} ok (just after deadline)", m_name);
        return true;
    }

    // 真超时了：detach 保平安，不阻塞主线程
    AURORA_WARN("ThreadBase", "timedJoin() timeout name={} timeoutMs={}, detaching",
                m_name, timeoutMs);
    m_thread.detach();
    return false;
}

void ThreadBase::threadFunc() {
    AURORA_TRACE("ThreadBase", "threadFunc() enter name={}", m_name);

    // ---- 初始化（SEH 保护，防止 onInitialize 崩溃导致 std::terminate） ----
    m_state.store(ThreadState::Initializing, std::memory_order_release);
    if (!safeInitThread(this)) {
        AURORA_ERROR("ThreadBase", "onInitialize() failed or crashed name={}", m_name);
        m_state.store(ThreadState::Stopped, std::memory_order_release);
        return;
    }

    // 使用 CAS 而非 store，防止覆盖 requestStop 在 onInitialize 期间设置的 Stopping
    {
        auto expected = ThreadState::Initializing;
        if (!m_state.compare_exchange_strong(expected, ThreadState::Running,
                std::memory_order_release, std::memory_order_acquire)) {
            // 状态已被外部改为 Stopping，跳过主循环直接清理
            AURORA_INFO("ThreadBase", "threadFunc() stop requested during init, skipping run name={}", m_name);
            goto cleanup;
        }
    }

    AURORA_INFO("ThreadBase", "threadFunc() running name={}", m_name);

    // ---- 主循环 ----
    while (true) {
        auto s = m_state.load(std::memory_order_acquire);

        if (s == ThreadState::Stopping) break;

        if (s == ThreadState::Pausing) {
            onPause();
            m_state.store(ThreadState::Paused, std::memory_order_release);

            while (true) {
                s = m_state.load(std::memory_order_acquire);
                if (s == ThreadState::Running || s == ThreadState::Stopping) break;
                std::this_thread::sleep_for(POLL_INTERVAL);
            }
            if (s == ThreadState::Stopping) break;
            onResume();
        }

        if (s == ThreadState::Running) {
            onRun();
        }
    }

    // ---- 清理 ----
cleanup:
    AURORA_INFO("ThreadBase", "threadFunc() cleaning up name={}", m_name);
    onCleanup();
    m_state.store(ThreadState::Stopped, std::memory_order_release);
}
