/**
 * @file EasingFunctions.h
 * @brief 缓动函数库
 * @date 2026-07-06
 * @details 至少 5 种缓动曲线，用于 ParameterTransition 和 StateTransition。
 *          所有函数输入 t ∈ [0, 1]，输出 ∈ [0, 1]（easeOutBack 允许少量 overshoot）。
 * @note 线程安全：纯函数，无状态，任意线程调用。
 */

#pragma once

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/// @brief 缓动类型枚举
enum class EasingType {
    Linear,
    EaseOutCubic,
    EaseInOutCubic,
    EaseOutBack,
    EaseInOutQuad,
};

/// @brief 缓动函数（纯函数集合）
namespace Easing {

    inline double linear(double t) {
        return t;
    }

    /// easeOutCubic: 1 - (1-t)³ — 快速开始，缓慢结束
    inline double easeOutCubic(double t) {
        double f = 1.0 - t;
        return 1.0 - f * f * f;
    }

    /// easeInOutCubic: 前半加速后半减速 — 平滑对称
    inline double easeInOutCubic(double t) {
        if (t < 0.5)
            return 4.0 * t * t * t;
        double f = -2.0 * t + 2.0;
        return 0.5 * f * f * f + 1.0;
    }

    /// easeOutBack: 超出目标后回弹 — 音乐感"弹醒"效果
    inline double easeOutBack(double t) {
        constexpr double c1 = 1.70158;
        constexpr double c3 = c1 + 1.0;
        double f = t - 1.0;
        return 1.0 + c3 * f * f * f + c1 * f * f;
    }

    /// easeInOutQuad: 二次缓动 — 轻快切换感
    inline double easeInOutQuad(double t) {
        if (t < 0.5)
            return 2.0 * t * t;
        double f = -2.0 * t + 2.0;
        return 1.0 - 0.5 * f * f;
    }

    /// @brief 根据类型分发缓动函数
    inline double apply(EasingType type, double t) {
        switch (type) {
        case EasingType::Linear:         return linear(t);
        case EasingType::EaseOutCubic:   return easeOutCubic(t);
        case EasingType::EaseInOutCubic: return easeInOutCubic(t);
        case EasingType::EaseOutBack:    return easeOutBack(t);
        case EasingType::EaseInOutQuad:  return easeInOutQuad(t);
        default:                         return t;
        }
    }
}
