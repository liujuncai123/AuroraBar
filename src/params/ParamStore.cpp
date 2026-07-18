/**
 * @file ParamStore.cpp
 * @brief ParamStore 实现
 * @date 2026-07-06
 */

#include "ParamStore.h"
#include "../logging/LoggerManager.h"
#include <nlohmann/json.hpp>
#include <algorithm>

ParamStore& ParamStore::Instance() {
    static ParamStore instance;
    return instance;
}

void ParamStore::RegisterParam(const ParamDef& def) {
    std::unique_lock lock(m_mutex);
    ParamValue pv;
    pv.def = def;
    pv.value = def.defaultVal;
    m_params[def.key] = pv;
}

double ParamStore::GetDouble(const std::string& key) const {
    std::shared_lock lock(m_mutex);
    auto it = m_params.find(key);
    if (it == m_params.end()) {
        // 安全：未知key返回默认值，记录ERROR级别日志以便排查配置错误
        AURORA_ERROR("ParamStore", "GetDouble unknown key={}, returning 0.0", key);
        return 0.0;
    }
    return it->second.value;
}

int ParamStore::GetInt(const std::string& key) const {
    std::shared_lock lock(m_mutex);
    auto it = m_params.find(key);
    if (it == m_params.end()) {
        AURORA_ERROR("ParamStore", "GetInt unknown key={}, returning 0", key);
        return 0;
    }
    return static_cast<int>(it->second.value);
}

bool ParamStore::GetBool(const std::string& key) const {
    std::shared_lock lock(m_mutex);
    auto it = m_params.find(key);
    if (it == m_params.end()) {
        AURORA_ERROR("ParamStore", "GetBool unknown key={}, returning false", key);
        return false;
    }
    return it->second.value > 0.5;
}

void ParamStore::SetDouble(const std::string& key, double value) {
    std::unique_lock lock(m_mutex);
    auto it = m_params.find(key);
    if (it == m_params.end()) {
        AURORA_WARN("ParamStore", "SetDouble unknown key={}", key);
        return;
    }

    double clamped = clampValue(it->second.def, value);
    double old = it->second.value;
    it->second.value = clamped;

    if (old != clamped) {
        m_dirty.store(true, std::memory_order_release);  // 标记有未保存的修改
        // 迭代所有订阅，筛选匹配 key 的回调
        for (auto& [id, entry] : m_subscriptions) {
            if (entry.first == key) {
                entry.second(key, clamped);
            }
        }
    }
}

void ParamStore::SetInt(const std::string& key, int value) {
    SetDouble(key, static_cast<double>(value));
}

void ParamStore::SetBool(const std::string& key, bool value) {
    SetDouble(key, value ? 1.0 : 0.0);
}

const ParamDef* ParamStore::GetParamDef(const std::string& key) const {
    std::shared_lock lock(m_mutex);
    auto it = m_params.find(key);
    if (it == m_params.end()) return nullptr;
    return &it->second.def;
}

void ParamStore::ResetAll() {
    std::unique_lock lock(m_mutex);
    // 收集所有发生变化的参数及其新值，先更新再批量通知
    std::vector<std::pair<std::string, double>> changed;
    for (auto& [key, pv] : m_params) {
        double oldVal = pv.value;
        pv.value = pv.def.defaultVal;
        if (oldVal != pv.value)
            changed.emplace_back(key, pv.value);
    }
    // 批量通知订阅者（确保渲染线程等收到参数变更）
    for (auto& [key, newVal] : changed) {
        for (auto& [id, entry] : m_subscriptions) {
            if (entry.first == key)
                entry.second(key, newVal);
        }
    }
    if (!changed.empty())
        m_dirty.store(true, std::memory_order_release);  // 重置也是一种修改，需用户主动保存
    AURORA_INFO("ParamStore", "ResetAll: {} params changed, notified subscribers", changed.size());
}

int ParamStore::Subscribe(const std::string& key, ChangeCallback callback) {
    std::unique_lock lock(m_mutex);
    int id = m_nextSubId++;
    m_subscriptions[id] = {key, std::move(callback)};
    return id;
}

void ParamStore::Unsubscribe(int subscriptionId) {
    std::unique_lock lock(m_mutex);
    m_subscriptions.erase(subscriptionId);
}

Result<void> ParamStore::LoadFromJson(const json& j) {
    std::unique_lock lock(m_mutex);
    try {
        for (auto& [key, pv] : m_params) {
            if (pv.def.noExport) continue; // GPU 等硬件参数不从方案恢复
            if (j.contains(key)) {
                double val = j[key].get<double>();
                pv.value = clampValue(pv.def, val);
            } else {
                pv.value = pv.def.defaultVal;
            }
        }
        m_dirty.store(false, std::memory_order_release);  // 加载方案后标记为纯净
        return Result<void>::Ok();
    } catch (const std::exception& e) {
        AURORA_ERROR("ParamStore", "LoadFromJson failed: {}", e.what());
        return Result<void>::Err(
            MAKE_ERROR(ErrorCode::kConfigParseError, e.what()));
    }
}

json ParamStore::SaveToJson() const {
    std::shared_lock lock(m_mutex);
    json j;
    for (auto& [key, pv] : m_params) {
        if (pv.def.noExport) continue; // GPU 等硬件相关参数不导出
        j[key] = pv.value;
    }
    return j;
}

double ParamStore::clampValue(const ParamDef& def, double val) const {
    return std::clamp(val, def.minVal, def.maxVal);
}

void ParamStore::NotifyAllChanged() {
    // 在锁外收集回调，避免死锁（订阅者可能回调 ParamStore 方法）
    std::vector<std::pair<std::string, ChangeCallback>> toNotify;
    {
        std::shared_lock lock(m_mutex);
        for (auto& [key, pv] : m_params) {
            for (auto& [id, entry] : m_subscriptions) {
                if (entry.first == key) {
                    toNotify.push_back({key, entry.second});
                }
            }
        }
    }
    for (auto& [key, cb] : toNotify) {
        // 安全：cb 通常是轻量级原子操作或 Qt 排队调用，不会长时间阻塞
        cb(key, GetDouble(key));
    }
    AURORA_INFO("ParamStore", "NotifyAllChanged: {} notifications sent", toNotify.size());
}
