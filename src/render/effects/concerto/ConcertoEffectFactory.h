/**
 * @file ConcertoEffectFactory.h
 * @brief 协奏效果工厂——根据 subMode 创建对应 Effect
 * @date 2026-07-18
 * @details 工厂模式：subMode 0-7 各对应一个 Effect 类。
 *          编译失败时由调用方（ConcertoRenderer）回退到 subMode 0。
 * @note 线程安全：工厂本身无状态，可在任意线程调用 create()。
 */
#pragma once

#include "IConcertoEffect.h"
#include <memory>

/**
 * @class ConcertoEffectFactory
 * @brief 协奏效果工厂
 * @details 根据 subMode 创建对应 Effect 实例。
 *          非法 subMode 返回 nullptr，由调用方处理。
 */
class ConcertoEffectFactory {
public:
    /// @brief 根据 subMode 创建 Effect（0-7）
    /// @param subMode 子模式索引 [0, kMaxSubMode]
    /// @return 对应 Effect 实例；非法 subMode 返回 nullptr
    static std::unique_ptr<IConcertoEffect> create(int subMode);

    static constexpr int kMinSubMode = 0;  ///< 最小 subMode
    static constexpr int kMaxSubMode = 7;   ///< 最大 subMode
};
