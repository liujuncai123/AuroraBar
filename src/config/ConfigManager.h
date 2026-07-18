/**
 * @file ConfigManager.h
 * @brief JSON 配置管理器 — 多方案支持
 * @date 2026-07-06
 */

#pragma once

#include "../core/Result.h"
#include <string>
#include <vector>

class ConfigManager {
public:
    /// @brief 初始化：加载上次方案或创建默认方案
    static Result<void> Init();

    /// @brief 保存当前参数到当前方案文件
    static Result<void> SaveCurrent();

    /// @brief 方案目录路径
    static std::string profilesDir();

    /// @brief 列出所有方案名（不含扩展名）
    static std::vector<std::string> ListProfiles();

    /// @brief 获取当前方案名
    static std::string currentProfile() { return s_currentProfile; }

    /// @brief 创建新方案（保存当前参数）
    static Result<void> CreateProfile(const std::string& name);

    /// @brief 切换到指定方案
    static Result<void> SwitchProfile(const std::string& name);

    /// @brief 删除方案
    static Result<void> DeleteProfile(const std::string& name);

    /// @brief 导入方案（从外部 JSON 文件复制到方案目录）
    static Result<void> ImportProfile(const std::string& srcPath, const std::string& newName);

    /// @brief 导出方案到外部路径
    static Result<void> ExportProfile(const std::string& name, const std::string& dstPath);

private:
    static std::string profilePath(const std::string& name);
    static void saveLastProfile(const std::string& name);
    static std::string loadLastProfile();

    static inline std::string s_currentProfile = "默认方案";
};
