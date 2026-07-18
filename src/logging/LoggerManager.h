/**
 * @file LoggerManager.h
 * @brief spdlog 统一日志管理器
 * @date 2026-07-06
 * @details 遵循 ADR-001：所有模块通过 LoggerManager 获取 logger，禁止直接创建。
 *          异步模式（反模式 #3 规避），日志路径 %APPDATA%/AuroraBar/logs/
 *          // 安全：日志内容不输出私密信息。
 *          日志格式：[模块名] 消息
 */

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>
#include <filesystem>

class LoggerManager {
public:
    static void init(const std::string& logDir);

    static std::shared_ptr<spdlog::logger> getLogger(const std::string& module);

    static void shutdown();

private:
    static inline std::shared_ptr<spdlog::details::thread_pool> s_threadPool;
    static inline bool s_initialized = false;
};

#define AURORA_TRACE(mod, ...) do { \
    auto _aurora_l = LoggerManager::getLogger(mod); \
    if (_aurora_l) _aurora_l->trace(__VA_ARGS__); \
} while(0)

#define AURORA_INFO(mod, ...) do { \
    auto _aurora_l = LoggerManager::getLogger(mod); \
    if (_aurora_l) _aurora_l->info(__VA_ARGS__); \
} while(0)

#define AURORA_WARN(mod, ...) do { \
    auto _aurora_l = LoggerManager::getLogger(mod); \
    if (_aurora_l) _aurora_l->warn(__VA_ARGS__); \
} while(0)

#define AURORA_ERROR(mod, ...) do { \
    auto _aurora_l = LoggerManager::getLogger(mod); \
    if (_aurora_l) _aurora_l->error(__VA_ARGS__); \
} while(0)
