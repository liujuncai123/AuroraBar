/**
 * @file LoggerManager.cpp
 * @brief LoggerManager spdlog 实现
 * @date 2026-07-06
 */

#include "LoggerManager.h"
#include <spdlog/sinks/null_sink.h>
#include <unordered_map>
#include <mutex>

// 安全：spdlog 关闭后，getLogger 返回的 null sink logger
//   持有一个静态的 null_sink_mt，所有日志调用被静默丢弃，
//   防止静态析构阶段 AURORA_INFO/AURORA_TRACE 因 spdlog::default_logger()==nullptr 崩溃
static std::shared_ptr<spdlog::logger> s_nullLogger;
static std::mutex s_nullLoggerMutex;  // 安全：保护 s_nullLogger 并发访问

static std::shared_ptr<spdlog::logger> getOrCreateNullLogger() {
    std::lock_guard<std::mutex> lock(s_nullLoggerMutex);
    if (!s_nullLogger) {
        s_nullLogger = std::make_shared<spdlog::logger>(
            "null",
            std::make_shared<spdlog::sinks::null_sink_mt>());
    }
    return s_nullLogger;
}

// 安全：s_loggers 被多线程并发访问（主线程/GUI + 渲染线程 + 采集线程 + 逻辑线程）
//   Win+D 显示时多线程同时打日志，并发修改 std::unordered_map 会导致内部 std::string 损坏
//   → 0xc0000005 空指针解引用崩溃（crash.log 中 AccessAddr: 0x0）
//   s_loggersMutex 保护所有读写操作
static std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> s_loggers;
static std::mutex s_loggersMutex;

void LoggerManager::init(const std::string& logDir) {
    if (s_initialized) return;
    s_initialized = true;

    std::filesystem::create_directories(logDir);

    s_threadPool = std::make_shared<spdlog::details::thread_pool>(8192, 1);

    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        logDir + "/aurorabar.log", 1024 * 1024 * 5, 3);

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    auto defaultLogger = std::make_shared<spdlog::async_logger>(
        "default",
        spdlog::sinks_init_list{fileSink, consoleSink},
        s_threadPool,
        spdlog::async_overflow_policy::block);

    // 安全：根据构建类型动态设置日志等级
    //   Debug（含 _DEBUG / NDEBUG 未定义）：info，保留诊断心跳/命令跟踪，便于排查问题
    //   Release（NDEBUG 已定义，CMake 自动注入）：warn，仅输出警告/错误，
    //     减少 Release 版本 IO 开销和 log 文件膨胀（高频 INFO 日志如渲染心跳被静默）
    //   TRACE 级别永远不启用（生产环境无需此级噪音）
#ifdef NDEBUG
    constexpr const char* kLogLevelName = "warn";
    defaultLogger->set_level(spdlog::level::warn);
    defaultLogger->flush_on(spdlog::level::warn);  // Release：仅 warn 及以上立即刷新
#else
    constexpr const char* kLogLevelName = "info";
    defaultLogger->set_level(spdlog::level::info);
    defaultLogger->flush_on(spdlog::level::info);  // Debug：所有日志立即刷新，防崩溃丢日志
#endif
    defaultLogger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    spdlog::set_default_logger(defaultLogger);

    AURORA_INFO("LoggerManager", "Logger initialized, path={} level={}", logDir, kLogLevelName);
}

std::shared_ptr<spdlog::logger> LoggerManager::getLogger(const std::string& module) {
    // 安全：先加锁查找，避免并发修改 s_loggers 导致内部 std::string 损坏
    {
        std::lock_guard<std::mutex> lock(s_loggersMutex);
        auto it = s_loggers.find(module);
        if (it != s_loggers.end()) return it->second;
    }

    // 安全：spdlog::shutdown() 后 default_logger() 返回 nullptr
    //   此时返回 null sink logger，不写日志也不崩溃
    auto defLogger = spdlog::default_logger();
    if (!defLogger) {
        return getOrCreateNullLogger();
    }

    auto logger = defLogger->clone(module);

    // 安全：插入新 logger 时加锁，防止并发插入导致 map 内部状态损坏
    {
        std::lock_guard<std::mutex> lock(s_loggersMutex);
        // 双重检查：可能另一个线程已经插入了
        auto it = s_loggers.find(module);
        if (it != s_loggers.end()) return it->second;
        s_loggers[module] = logger;
    }

    return logger;
}

void LoggerManager::shutdown() {
    AURORA_INFO("LoggerManager", "Shutting down...");
    {
        std::lock_guard<std::mutex> lock(s_loggersMutex);
        s_loggers.clear();
    }
    s_threadPool.reset();
    spdlog::shutdown();
}