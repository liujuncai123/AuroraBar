/**
 * @file main.cpp
 * @brief AuroraBar 主入口
 * @date 2026-07-06
 */

// 强制独显运行（覆盖驱动默认集显选择）
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#include "mainwindow.h"

// 日志（提前 include，供 doQtExec 的主线程心跳使用 AURORA_INFO）
#include "src/logging/LoggerManager.h"

#include <QApplication>
#include <QSharedMemory>
#include <QMessageBox>
#include <QTimer>
#include <atomic>
#include <fstream>

#define NOMINMAX  // 禁用 windows.h 的 min/max 宏，防止污染 std::min/std::max
#include <windows.h>  // SetUnhandledExceptionFilter / _EXCEPTION_POINTERS
#include <mmsystem.h>    // timeBeginPeriod / timeEndPeriod（提升时钟精度至 1ms，支撑高帧率）
#pragma comment(lib, "winmm.lib")

/// @brief 全局异常过滤器 — 记录崩溃地址 + 栈回溯到独立文件
/// @details 使用 RtlCaptureStackBackTrace（kernel32.dll 原生 API，无需 DbgHelp.dll）
///          栈回溯不需要符号文件，仅输出原始地址，后续可通过 dumpbin + PDB 反查
static LONG WINAPI unhandledExceptionFilter(_EXCEPTION_POINTERS* ExceptionInfo) {
    std::ofstream crashLog("logs/crash.log", std::ios::app);
    if (crashLog.is_open()) {
        // 基础崩溃信息
        crashLog << "=== CRASH ===" << std::endl;
        crashLog << "ExceptionCode: 0x" << std::hex << ExceptionInfo->ExceptionRecord->ExceptionCode << std::dec << std::endl;
        crashLog << "ExceptionAddr: 0x" << std::hex 
                  << reinterpret_cast<uintptr_t>(ExceptionInfo->ExceptionRecord->ExceptionAddress) << std::dec << std::endl;

        // 访问违规额外信息（读/写/执行 + 目标地址）
        if (ExceptionInfo->ExceptionRecord->ExceptionCode == 0xc0000005 &&
            ExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
            const char* accessType = "?";
            switch (ExceptionInfo->ExceptionRecord->ExceptionInformation[0]) {
                case 0: accessType = "READ"; break;
                case 1: accessType = "WRITE"; break;
                case 8: accessType = "EXECUTE"; break;
            }
            crashLog << "AccessType: " << accessType << std::endl;
            crashLog << "AccessAddr: 0x" << std::hex
                      << ExceptionInfo->ExceptionRecord->ExceptionInformation[1] << std::dec << std::endl;
        }

        // 模块基址（用于计算偏移，配合 dumpbin + PDB 反查）
        HMODULE hExe = GetModuleHandleW(nullptr);
        crashLog << "ExeBase: 0x" << std::hex << reinterpret_cast<uintptr_t>(hExe) << std::dec << std::endl;

        // 栈回溯（最多 32 帧）+ 模块归属 + 偏移
        //   每帧记录: 地址 模块名+偏移，可直接用 dumpbin /disasm 反查
        void* stackTrace[32];
        USHORT frameCount = CaptureStackBackTrace(0, 32, stackTrace, nullptr);
        crashLog << "StackTrace (" << frameCount << " frames):" << std::endl;
        for (USHORT i = 0; i < frameCount; ++i) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(stackTrace[i]);
            HMODULE hMod = nullptr;
            char modPath[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(addr), &hMod) && hMod) {
                GetModuleFileNameA(hMod, modPath, MAX_PATH);
                const char* p = strrchr(modPath, '\\');
                const char* baseName = p ? p + 1 : modPath;
                crashLog << "  [" << i << "] 0x" << std::hex << addr << std::dec
                         << "  " << baseName << "+0x" << std::hex
                         << (addr - reinterpret_cast<uintptr_t>(hMod)) << std::dec << std::endl;
            } else {
                crashLog << "  [" << i << "] 0x" << std::hex << addr << std::dec << "  (unknown)" << std::endl;
            }
        }
        crashLog.close();
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "=== CRASH at 0x%p, code=0x%08X\n",
             ExceptionInfo->ExceptionRecord->ExceptionAddress,
             ExceptionInfo->ExceptionRecord->ExceptionCode);
    OutputDebugStringA(buf);

    return EXCEPTION_EXECUTE_HANDLER;  // 让系统弹出崩溃对话框
}

/// @brief 非 SEH 辅助函数 — Qt 事件循环 + 主线程心跳（可用 C++ 对象）
/// @note 心跳停止 → 主线程阻塞；心跳正常 → 主线程活着
static int doQtExec(QApplication& app) {
    static std::atomic<uint64_t> beatCount{0};
    QTimer heartbeat;
    heartbeat.setInterval(1000);
    QObject::connect(&heartbeat, &QTimer::timeout, []() {
        uint64_t b = ++beatCount;
        AURORA_INFO("Main", "heartbeat #{} (主线程存活)", b);
    });
    heartbeat.start();
    return app.exec();
}

/// @brief SEH 保护的 Qt 事件循环（独立函数避免 MSVC C2712）
/// @return QApplication::exec() 返回值，崩溃时返回 1
static int qtExecSEH(QApplication& app) {
    __try {
        return doQtExec(app);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // 安全：主线程崩溃时，尝试优雅退出
        // 不在此处写日志（spdlog 可能已损坏），由 unhandledExceptionFilter 记录
        OutputDebugStringA("=== Main thread SEH caught crash, exiting\n");
        return 1;
    }
}

// 核心基础设施
#include "src/core/Result.h"
#include "src/core/SPSCQueue.h"
#include "src/core/ThreadBase.h"
#include "src/core/CommandTypes.h"

// 线程头文件
// [MIGRATION] RenderThread.h 已废弃，渲染由 QtOverlayWindow 在主线程处理
#include "src/analysis/LogicThread.h"
#include "src/audio/AudioCaptureThread.h"

// Qt6 版透明叠加窗口（QWindow + GL 直渲）
#include "src/ui/QtOverlayWindow.h"

// 配置
#include "src/config/ConfigManager.h"
#include "src/params/BuiltInDefaults.h"
#include "src/params/ParamStore.h"

// ============================================================
// 全局 SPSC 无锁队列（线程间通信唯一通道）
// ============================================================

/// @brief 采集 → 逻辑：音频帧队列（容量 32，满载丢弃最旧）
SPSCQueue<AudioFrame, 32> g_audioQueue;

/// @brief 逻辑 → 渲染：渲染指令队列（容量 64，满载丢弃最旧）
SPSCQueue<RenderCommand, 64> g_renderQueue;

/// @brief 主线程 → 逻辑：控制指令队列（容量 16，满载丢弃最旧）
SPSCQueue<ControlCommand, 16> g_controlQueue;



// ============================================================
// 全局原子状态（跨线程无锁读取）
// ============================================================

std::atomic<AppState> g_appState{AppState::Dormant};   ///< 应用生命周期状态
std::atomic<bool>      g_audioActive{false};            ///< WASAPI 是否在采集
std::atomic<bool>      g_collapsed{false};              ///< 边框收起状态
std::atomic<uint64_t>  g_frameCount{0};                 ///< 渲染帧计数（调试用）

// ============================================================
/// @brief 优雅停止所有工作线程（先停生产端，最后停消费端）
// ============================================================
// [MIGRATION] 移除 RenderThread 参数 — 渲染由 QtOverlayWindow 在主线程处理，
//             其析构会自动停止 QTimer 和清理 GL 资源，无需在此处关闭
static void shutdownThreads(/* [MIGRATION] RenderThread& renderThread, */
                            LogicThread& logicThread,
                            AudioCaptureThread& audioThread)
{
    AURORA_INFO("Main", "Initiating shutdown sequence...");
    g_appState.store(AppState::Stopping, std::memory_order_release);

    // 停止顺序：Audio → Logic（渲染由 QtOverlayWindow 析构处理）
    AURORA_INFO("Main", "Stopping AudioCaptureThread...");
    audioThread.requestStop();

    AURORA_INFO("Main", "Stopping LogicThread...");
    logicThread.requestStop();

    // [MIGRATION] RenderThread 关闭逻辑已移除（QtOverlayWindow 由 Qt 自动清理）
    // AURORA_INFO("Main", "Stopping RenderThread...");
    // renderThread.requestStop();

    // 超时等待线程退出（100ms 内没停就 detach，永不卡死主线程）
    constexpr int THREAD_STOP_TIMEOUT_MS = 100;

    struct { ThreadBase& t; const char* name; } threads[] = {
        {audioThread, "AudioCaptureThread"},
        {logicThread, "LogicThread"},
        // [MIGRATION] RenderThread 已移除
        // {renderThread, "RenderThread"}
    };

    for (auto& item : threads) {
        bool ok = item.t.timedJoin(THREAD_STOP_TIMEOUT_MS);
        if (ok) {
            AURORA_INFO("Main", "{} stopped (state={})",
                        item.name, static_cast<int>(item.t.state()));
        } else {
            AURORA_WARN("Main", "{} did not stop within {}ms, force-detached",
                        item.name, THREAD_STOP_TIMEOUT_MS);
        }
    }

    AURORA_INFO("Main", "All threads stopped cleanly. Frames rendered: {}",
                g_frameCount.load());
}

// ============================================================
int main(int argc, char* argv[])
{
    OutputDebugStringA("[STARTUP] LoggerManager::init\n");
    // ---------- 日志系统 ----------
    LoggerManager::init("logs");

    // 激活全局异常过滤器，硬崩溃时写入 logs/crash.log + OutputDebugString
    SetUnhandledExceptionFilter(unhandledExceptionFilter);

    // 提升系统时钟分辨率至 1ms，支撑高帧率模式（targetFps ≥ 60 时 sleep_for 需精确到 <16ms）
    // Windows 默认时钟 ~15.6ms，不加此调用时 120fps/8ms sleep 实际 ≈ 16ms，帧率被锁在 ~64fps
    // timeBeginPeriod 是系统全局调用，但多数桌面环境（浏览器/音频等）已提至 1ms，净影响可忽略
    timeBeginPeriod(1);

    AURORA_INFO("Main", "AuroraBar starting...");

    OutputDebugStringA("[STARTUP] RegisterAllParams + ConfigManager::Init\n");
    // ---------- 参数注册 + 配置加载 ----------
    RegisterAllParams();
    auto initRes = ConfigManager::Init();
    if (initRes.isErr()) {
        AURORA_WARN("Main", "Config init warning: {}", initRes.error().message);
    }
    AURORA_INFO("Main", "Config loaded, profile={}", ConfigManager::currentProfile());

    OutputDebugStringA("[STARTUP] QApplication init\n");
    // ---------- Qt 初始化 ----------
    QApplication a(argc, argv);
    a.setApplicationName("AuroraBar");
    a.setOrganizationName("AuroraBar");
    // 安全：托盘应用只有一个隐藏的 MainWindow + Tool 窗口
    // Qt 默认在"最后非 Tool 窗口关闭时退出"，会误杀托盘应用
    a.setQuitOnLastWindowClosed(false);

    // ---------- 单例检测 ----------
    static QSharedMemory singleton("AuroraBar_SingleInstance_2026");
    if (!singleton.create(1)) {
        QMessageBox::warning(nullptr, "AuroraBar",
            "AuroraBar 已在运行中。\n请查看系统托盘图标。");
        LoggerManager::shutdown();
        return 0;
    }

    // ---------- 装配线程 ----------
    // [MIGRATION] RenderThread 已废弃，渲染逻辑迁移到 QtOverlayWindow（主线程）
    // RenderThread       renderThread;
    LogicThread        logicThread;
    AudioCaptureThread audioThread;

    // ---------- Qt 托盘（提前创建，立即显示图标） ----------
    OutputDebugStringA("[STARTUP] MainWindow creation\n");
    MainWindow w;
    QObject::connect(&w, &MainWindow::overlayToggled, [](bool visible) {
        ControlCommand cc;
        cc.type = visible ? ControlCommand::Type::Expand : ControlCommand::Type::Collapse;
        g_controlQueue.tryPush(cc);
    });

    // ---------- 创建 QtOverlayWindow（替代 RenderThread） ----------
    //   渲染在独立 QtRenderThread，QWindow 直接 GL 直渲（makeCurrent + swapBuffers）
    //   LogicThread 仍通过 g_renderQueue 发送 RenderCommand
    //   启动顺序：QtOverlay（消费端）→ Logic → Audio（先开消费端，最后开生产端）
    OutputDebugStringA("[STARTUP] Creating QtOverlayWindow...\n");
    QtOverlayWindow overlay;
    if (!overlay.initialize()) {
        AURORA_ERROR("Main", "QtOverlayWindow init failed");
        LoggerManager::shutdown();
        return 1;
    }

    // 推送已保存的模式到控制队列
    {
        ControlCommand modeCmd;
        modeCmd.type = ControlCommand::Type::SetMode;
        modeCmd.value = static_cast<double>(ParamStore::Instance().GetInt("mode"));
        g_controlQueue.tryPush(modeCmd);
        AURORA_INFO("Main", "Initial mode={} pushed to control queue", modeCmd.value);
    }

    // 安全：使用 SEH 保护的 safeInitThread（在 ThreadBase::threadFunc 内部）
    //       onInitialize() 已在工作线程上下文中被 SEH 保护
    // [MIGRATION] RenderThread 启动块已移除（QtOverlayWindow 已在上面 initialize）
    // {
    //     OutputDebugStringA("[STARTUP] Starting RenderThread...\n");
    //     auto rr = renderThread.start();
    //     if (rr.isErr()) {
    //         AURORA_ERROR("Main", "RenderThread start failed: {}", rr.error().message);
    //         LoggerManager::shutdown();
    //         return 1;
    //     }
    //     AURORA_INFO("Main", "RenderThread started (state={})",
    //                 static_cast<int>(renderThread.state()));
    // }
    {
        OutputDebugStringA("[STARTUP] Starting LogicThread...\n");
        auto lr = logicThread.start();
        if (lr.isErr()) {
            AURORA_ERROR("Main", "LogicThread start failed: {}", lr.error().message);
            // [MIGRATION] 不再清理 renderThread
            // renderThread.requestStop();
            // renderThread.join();
            LoggerManager::shutdown();
            return 1;
        }
        AURORA_INFO("Main", "LogicThread started (state={})",
                    static_cast<int>(logicThread.state()));
    }
    {
        OutputDebugStringA("[STARTUP] Starting AudioCaptureThread...\n");
        auto ar = audioThread.start();
        if (ar.isErr()) {
            AURORA_ERROR("Main", "AudioCaptureThread start failed: {}", ar.error().message);
            logicThread.requestStop();
            logicThread.join();
            // [MIGRATION] 不再清理 renderThread
            // renderThread.requestStop();
            // renderThread.join();
            LoggerManager::shutdown();
            return 1;
        }
        AURORA_INFO("Main", "AudioCaptureThread started (state={})",
                    static_cast<int>(audioThread.state()));
    }

    OutputDebugStringA("[STARTUP] All threads started, entering Qt event loop\n");
    AURORA_INFO("Main", "All threads running (Main + QtOverlay + Logic + Audio)");

    // ---------- Qt 事件循环（SEH 保护） ----------
    int ret = qtExecSEH(a);

    // ---------- 优雅停止 ----------
    // [MIGRATION] shutdownThreads 移除 renderThread 参数（overlay 由 Qt 自动析构）
    shutdownThreads(/* [MIGRATION] renderThread, */ logicThread, audioThread);

    // 恢复系统时钟分辨率（与 main 开头 timeBeginPeriod(1) 配对）
    timeEndPeriod(1);

    // ---------- 退出前保存配置 ----------
    ConfigManager::SaveCurrent();
    AURORA_INFO("Main", "Config saved, profile={}", ConfigManager::currentProfile());

    AURORA_INFO("Main", "AuroraBar exiting (code={})", ret);
    LoggerManager::shutdown();
    return ret;
}
