/**
 * @file ThreadBase.h
 * @brief 统一线程生命周期管理基类
 * @date 2026-07-06
 * @details 所有工作线程（采集/逻辑/渲染）继承此类，保证一致的启停行为。
 *          子类仅需实现 onInitialize / onRun / onCleanup 三个纯虚函数。
 * @note 线程安全：m_state 为 std::atomic，跨线程无竞争。
 *       // 安全：构造函数不做可能失败的操作（反模式 #8），
 *       初始化逻辑放在 onInitialize() 中，返回 Result<void>。
 */

#pragma once

#include "Result.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <string>

// ============================================================
// 线程状态
// ============================================================
enum class ThreadState {
    Uninitialized,
    Initializing,
    Running,
    Pausing,
    Paused,
    Stopping,
    Stopped
};

// ============================================================
// ThreadBase
// ============================================================
class ThreadBase {
public:
    virtual ~ThreadBase();

    /**
     * @brief 启动线程
     * @return 成功返回 Ok；onInitialize 失败返回 Error
     * @note 调用后线程进入 Initializing → Running，
     *       线程函数中循环调用 onRun() 直到 requestStop() 被调用
     */
    Result<void> start();

    /**
     * @brief 异步请求停止 (不阻塞)
     * @note 设置状态为 Stopping，主循环应在下一帧退出
     */
    void requestStop();

    /**
     * @brief 请求暂停 (不阻塞)
     * @note 设置状态为 Pausing，主循环应在下一帧调用 onPause()
     */
    void requestPause();

    /**
     * @brief 请求恢复 (不阻塞)
     */
    void requestResume();

    /**
     * @brief 等待线程退出 (阻塞)
     * @note 确保在调用前已 requestStop()
     */
    void join();

    /**
     * @brief 等待线程退出，超时则 detach（标准化关机，永不卡死主线程）
     * @param timeoutMs 超时毫秒数
     * @return true = 线程正常退出，false = 超时已 detach
     * @note
     *   Phase 1: sleep(2ms) → 给线程自然的退出窗口
     *   Phase 2: yield() 轮询 → 不依赖时钟分辨率，目标线程停了我们立刻知道
     *   超时 → detach() + 日志报警 → 返回 false
     */
    bool timedJoin(int timeoutMs);

    /**
     * @brief SEH 安全地执行 onInitialize（实现在 ThreadSEH.cpp）
     * @param thread 线程实例
     * @return true 初始化成功，false onInitialize 崩溃或返回错误
     * @note 这是 public static 方法，可供 ThreadSEH.cpp 调用而不违反访问权限
     */
    static bool safeInitThread(ThreadBase* thread);

    /// @brief 当前线程状态
    ThreadState state() const {
        return m_state.load(std::memory_order_acquire);
    }

    /// @brief 线程名称 (调试用)
    const std::string& name() const { return m_name; }

protected:
    explicit ThreadBase(std::string name);

    // ---------- 子类必须实现 ----------

    /**
     * @brief 线程启动前的初始化 (在线程上下文中执行)
     * @return 成功返回 Ok()，失败返回 Error（线程将不进入 Running）
     * @note 构造函数中不做可能失败的操作，全部放在这里
     */
    virtual Result<void> onInitialize() = 0;

    /**
     * @brief 线程主循环体 (在 while(shouldRun()) 中循环调用)
     * @note 每次迭代应 < 5ms，避免阻塞
     */
    virtual void onRun() = 0;

    /**
     * @brief 线程停止后的清理 (在线程退出前执行)
     */
    virtual void onCleanup() = 0;

    // ---------- 子类可选覆写 ----------

    /// @brief 暂停回调
    virtual void onPause() {}

    /// @brief 恢复回调
    virtual void onResume() {}

    /**
     * @brief 主循环是否应继续
     * @return true = 继续运行；false = 停止
     */
    bool shouldRun() const {
        auto s = m_state.load(std::memory_order_acquire);
        return s == ThreadState::Running || s == ThreadState::Pausing;
    }

private:
    void threadFunc();  // 线程入口函数

    /// @brief SEH 安全初始化的内部辅助（无 C++ 临时对象，在 ThreadSEH.cpp 中实现）
    static bool initThreadHelper(ThreadBase* thread);

    std::string m_name;
    std::atomic<ThreadState> m_state{ThreadState::Uninitialized};
    std::thread m_thread;
};
