/**
 * @file ThreadSEH.cpp
 * @brief LogicThread + AudioCaptureThread 的 SEH 崩溃保护 + 线程安全启动
 * @date 2026-07-15
 * @details 与 RenderFrameSEH.cpp 原理相同：将 __try/__except 块隔离在
 *          不含 C++ 临时对象的独立编译单元中，绕过 MSVC C2712 限制。
 * @note 线程安全：本文件函数在各线程的 onRun() 中调用，不额外加锁。
 */
#include "../analysis/LogicThread.h"
#include "../audio/AudioCaptureThread.h"
#include "../core/ThreadBase.h"

/**
 * @brief SEH 保护的 LogicThread::onRun() 包装
 * @param self LogicThread 实例指针
 * @pre 调用前 LogicThread 已初始化
 * @post 正常：执行一帧逻辑处理；崩溃：标记 crashed 让线程跳过本帧
 * @note 本函数不含 C++ 临时对象，绕过 MSVC C2712 限制
 */
void LogicThread_onRun_SEH(LogicThread* self) {
    __try {
        self->onRunBody();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        self->markCrashed();
    }
}

/**
 * @brief SEH 保护的 AudioCaptureThread::onRun() 包装
 * @param self AudioCaptureThread 实例指针
 * @pre 调用前 AudioCaptureThread 已初始化
 * @post 正常：执行一帧音频采集；崩溃：标记 crashed 让线程跳过本帧
 * @note 本函数不含 C++ 临时对象，绕过 MSVC C2712 限制
 */
void AudioCaptureThread_onRun_SEH(AudioCaptureThread* self) {
    __try {
        self->onRunBody();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        self->markCrashed();
    }
}

// ============================================================
// 线程安全启动（SEH 保护的 thread.start()）
//   onInitialize() 是线程初始化最脆弱的环节（OpenGL/COM 调用），
//   一旦崩溃会导致 std::terminate → 进程直接死亡。
//   本函数确保 onInitialize 中的崩溃被 SEH 捕获，不会无日志退出。
// ============================================================

/// @brief 非 SEH 辅助函数 — 可以安全使用 C++ 对象
static bool doStartThread(ThreadBase* thread) {
    auto result = thread->start();
    return result.isOk();
}

/**
 * @brief SEH 保护的线程启动
 * @param thread 线程实例指针
 * @return true 启动成功，false 启动失败或 onInitialize 崩溃
 * @note 本函数在 __try 中不包含任何 C++ 临时对象，绕过 MSVC C2712
 * @note 崩溃时通过 OutputDebugStringA 输出调试信息（不依赖 spdlog 异步日志）
 */
bool safeThreadStart(ThreadBase* thread) {
    __try {
        return doStartThread(thread);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("=== CRASH: Thread onInitialize crashed (SEH caught by safeThreadStart)\n");
        return false;
    }
}

// ============================================================
// 线程初始化 SEH 保护
//   在 ThreadBase::threadFunc() 中调用，与 onInitialize 在同一个线程上下文中，
//   确保 onInitialize() 中的崩溃被捕获而非触发 std::terminate。
// ============================================================

/// @brief 非 SEH 辅助函数 — 线程初始化（ThreadBase 静态成员，可访问 protected onInitialize）
bool ThreadBase::initThreadHelper(ThreadBase* thread) {
    auto result = thread->onInitialize();
    return result.isOk();
}

/**
 * @brief SEH 保护的线程初始化
 * @param thread 线程实例指针
 * @return true 初始化成功，false onInitialize 崩溃
 * @note 这是 ThreadBase::safeInitThread 的实现，在 __try 中不包含 C++ 临时对象
 */
bool ThreadBase::safeInitThread(ThreadBase* thread) {
    __try {
        return initThreadHelper(thread);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("=== CRASH: onInitialize crashed (SEH caught in thread context)\n");
        return false;
    }
}