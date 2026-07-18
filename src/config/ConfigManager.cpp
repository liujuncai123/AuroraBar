/**
 * @file ConfigManager.cpp
 * @brief 配置管理器实现 — 多方案 JSON
 * @date 2026-07-06
 */

#include "ConfigManager.h"
#include "../params/ParamStore.h"
#include "../logging/LoggerManager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================
// 路径
// ============================================================

std::string ConfigManager::profilesDir() {
    const char* appData = std::getenv("APPDATA");
    if (!appData) appData = ".";
    return std::string(appData) + "/AuroraBar/profiles";
}

std::string ConfigManager::profilePath(const std::string& name) {
    return profilesDir() + "/" + name + ".json";
}

static std::string lastProfilePath() {
    return ConfigManager::profilesDir() + "/last_profile.txt";
}

// ============================================================
// 最近方案追踪
// ============================================================

void ConfigManager::saveLastProfile(const std::string& name) {
    fs::create_directories(profilesDir());
    std::ofstream f(lastProfilePath());
    if (f.is_open()) f << name;
}

std::string ConfigManager::loadLastProfile() {
    std::ifstream f(lastProfilePath());
    std::string name;
    if (f.is_open() && std::getline(f, name) && !name.empty()) {
        return name;
    }
    return "";
}

// ============================================================
// 初始化
// ============================================================

Result<void> ConfigManager::Init() {
    fs::create_directories(profilesDir());

    // 1. 尝试加载上次使用的方案
    std::string last = loadLastProfile();
    if (!last.empty() && fs::exists(profilePath(last))) {
        s_currentProfile = last;
        AURORA_INFO("ConfigManager", "Loading last profile: {}", last);
        auto& ps = ParamStore::Instance();
        try {
            std::ifstream f(profilePath(last));
            // 安全：检查文件是否可读，避免空文件导致 parse 异常
            if (!f.is_open() || f.peek() == std::ifstream::traits_type::eof()) {
                AURORA_WARN("ConfigManager", "Last profile file empty or unreadable: {}", last);
                goto try_default;
            }
            json j = json::parse(f);
            ps.LoadFromJson(j);
            ps.NotifyAllChanged();  // 通知订阅者方案已加载
            saveLastProfile(last);
            return Result<void>::Ok();
        } catch (const std::exception& e) {
            AURORA_ERROR("ConfigManager", "Parse last profile failed: {} — {}", last, e.what());
            // 回退到默认方案，不崩溃
        }
    }

    // 2. 尝试加载 "默认方案"
try_default:
    if (fs::exists(profilePath("默认方案"))) {
        s_currentProfile = "默认方案";
        AURORA_INFO("ConfigManager", "Loading default profile");
        auto& ps = ParamStore::Instance();
        try {
            std::ifstream f(profilePath("默认方案"));
            if (!f.is_open() || f.peek() == std::ifstream::traits_type::eof()) {
                AURORA_WARN("ConfigManager", "Default profile empty or unreadable, creating fresh");
                goto create_default;
            }
            json j = json::parse(f);
            ps.LoadFromJson(j);
            ps.NotifyAllChanged();  // 通知订阅者方案已加载
            saveLastProfile("默认方案");
            return Result<void>::Ok();
        } catch (const std::exception& e) {
            AURORA_ERROR("ConfigManager", "Parse default profile failed: {}", e.what());
            // 回退到创建新方案
        }
    }

    // 3. 无方案 → 创建默认方案（用内置默认值）
create_default:
    AURORA_INFO("ConfigManager", "No profiles found, creating default");
    s_currentProfile = "默认方案";
    auto& ps = ParamStore::Instance();
    // 参数已由 RegisterAllParams 初始化，直接保存
    json j = ps.SaveToJson();
    fs::create_directories(profilesDir());
    std::ofstream f(profilePath("默认方案"));
    f << j.dump(4) << std::endl;
    saveLastProfile("默认方案");
    return Result<void>::Ok();
}

// ============================================================
// 保存 / 列出
// ============================================================

Result<void> ConfigManager::SaveCurrent() {
    auto& ps = ParamStore::Instance();
    json j = ps.SaveToJson();
    fs::create_directories(profilesDir());
    std::ofstream f(profilePath(s_currentProfile));
    if (!f.is_open())
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kConfigParseError, "Cannot write"));
    f << j.dump(4) << std::endl;
    saveLastProfile(s_currentProfile);
    ps.markClean();  // 保存后清除脏标记
    AURORA_INFO("ConfigManager", "Saved profile: {}", s_currentProfile);
    return Result<void>::Ok();
}

std::vector<std::string> ConfigManager::ListProfiles() {
    std::vector<std::string> names;
    if (!fs::exists(profilesDir())) return names;
    for (auto& entry : fs::directory_iterator(profilesDir())) {
        if (entry.path().extension() == ".json") {
            names.push_back(entry.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

// ============================================================
// 创建 / 切换 / 删除
// ============================================================

Result<void> ConfigManager::CreateProfile(const std::string& name) {
    if (name.empty())
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "Empty name"));
    if (fs::exists(profilePath(name)))
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "Already exists"));

    auto& ps = ParamStore::Instance();
    json j = ps.SaveToJson();
    fs::create_directories(profilesDir());
    std::ofstream f(profilePath(name));
    f << j.dump(4) << std::endl;
    AURORA_INFO("ConfigManager", "Created profile: {}", name);
    return Result<void>::Ok();
}

Result<void> ConfigManager::SwitchProfile(const std::string& name) {
    if (!fs::exists(profilePath(name)))
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "Profile not found"));

    // 加载新方案（不自动保存当前方案——由用户决定是否保存）
    auto& ps = ParamStore::Instance();
    try {
        std::ifstream f(profilePath(name));
        // 安全：检查文件是否可读
        if (!f.is_open() || f.peek() == std::ifstream::traits_type::eof()) {
            AURORA_ERROR("ConfigManager", "SwitchProfile: file empty or unreadable: {}", name);
            return Result<void>::Err(MAKE_ERROR(ErrorCode::kConfigParseError, "Profile file empty or unreadable"));
        }
        json j = json::parse(f);
        ps.LoadFromJson(j);
    } catch (const std::exception& e) {
        AURORA_ERROR("ConfigManager", "SwitchProfile parse failed: {} — {}", name, e.what());
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kConfigParseError, e.what()));
    }
    ps.NotifyAllChanged();
    s_currentProfile = name;
    saveLastProfile(name);
    AURORA_INFO("ConfigManager", "Switched to profile: {}", name);
    return Result<void>::Ok();
}

Result<void> ConfigManager::DeleteProfile(const std::string& name) {
    if (name == s_currentProfile)
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "Cannot delete active profile"));
    if (!fs::exists(profilePath(name)))
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "Not found"));
    fs::remove(profilePath(name));
    AURORA_INFO("ConfigManager", "Deleted profile: {}", name);
    return Result<void>::Ok();
}

// ============================================================
// 导入 / 导出
// ============================================================

Result<void> ConfigManager::ImportProfile(const std::string& srcPath, const std::string& newName) {
    if (!fs::exists(srcPath))
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "Source not found"));
    if (fs::exists(profilePath(newName)))
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "Name already exists"));

    // 验证 JSON 合法性
    try {
        std::ifstream f(srcPath);
        json j = json::parse(f);
        fs::create_directories(profilesDir());
        std::ofstream out(profilePath(newName));
        out << j.dump(4) << std::endl;
    } catch (...) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kConfigParseError, "Invalid JSON"));
    }
    AURORA_INFO("ConfigManager", "Imported profile: {} → {}", srcPath, newName);
    return Result<void>::Ok();
}

Result<void> ConfigManager::ExportProfile(const std::string& name, const std::string& dstPath) {
    if (!fs::exists(profilePath(name)))
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "Profile not found"));
    fs::copy_file(profilePath(name), dstPath, fs::copy_options::overwrite_existing);
    AURORA_INFO("ConfigManager", "Exported profile: {} → {}", name, dstPath);
    return Result<void>::Ok();
}
