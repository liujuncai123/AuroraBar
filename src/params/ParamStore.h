/**
 * @file ParamStore.h
 * @brief 统一参数存储 — 项目中所有可调参数的唯一真实数据源
 * @date 2026-07-06
 * @details 发布-订阅模式，任何模块通过 subscribe() 感知参数变化，
 *          无需轮询。内部使用 std::shared_mutex（读写锁）。
 *          // 安全：Set 时自动 clamp 到合法范围。
 *          // 性能：Get（共享读）高频无阻塞，Set（独占写）低频。
 */

#pragma once

#include "ParamDef.h"
#include "../core/Result.h"
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class ParamStore {
public:
    static ParamStore& Instance();

    // ---- 注册 (启动时一次性调用) ----

    void RegisterParam(const ParamDef& def);

    // ---- 读写 ----

    double GetDouble(const std::string& key) const;
    int    GetInt(const std::string& key) const;
    bool   GetBool(const std::string& key) const;

    void SetDouble(const std::string& key, double value);
    void SetInt(const std::string& key, int value);
    void SetBool(const std::string& key, bool value);

    /// @brief 重置所有参数为默认值
    void ResetAll();

    // ---- 订阅 ----

    using ChangeCallback = std::function<void(const std::string& key, double newValue)>;

    /// @brief 订阅参数变更，返回订阅 ID（用于取消）
    /// @note 回调在 Set 调用的线程上同步执行，应轻量
    int Subscribe(const std::string& key, ChangeCallback callback);
    void Unsubscribe(int subscriptionId);

    // ---- 获取元数据 ----

    /// @brief 获取参数定义（用于动态构建 UI）
    /// @return nullptr 如果 key 不存在
    const ParamDef* GetParamDef(const std::string& key) const;

    // ---- 序列化 ----

    Result<void> LoadFromJson(const json& j);
    json SaveToJson() const;

    /// @brief 通知所有订阅者参数已变更（用于 LoadFromJson 后触发同步）
    void NotifyAllChanged();

    // ---- 脏标记：追踪是否有未保存的修改 ----
    /// @brief 是否有未保存的参数修改
    bool isDirty() const { return m_dirty.load(std::memory_order_acquire); }
    /// @brief 清除脏标记（保存/加载方案后调用）
    void markClean() { m_dirty.store(false, std::memory_order_release); }
    /// @brief 标记为脏（Set 调用时自动触发，无需手动调用）
    void markDirty() { m_dirty.store(true, std::memory_order_release); }

private:
    ParamStore() = default;

    struct ParamValue {
        ParamDef def;
        double value;
    };

    double clampValue(const ParamDef& def, double val) const;

    std::unordered_map<std::string, ParamValue> m_params;
    std::unordered_map<int, std::pair<std::string, ChangeCallback>> m_subscriptions;
    mutable std::shared_mutex m_mutex;
    int m_nextSubId = 1;
    std::atomic<bool> m_dirty{false};  ///< 是否有未保存的参数修改
};
