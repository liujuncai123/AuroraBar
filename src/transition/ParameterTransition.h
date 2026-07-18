/**
 * @file ParameterTransition.h
 * @brief 单参数平滑过渡（Lerp + 缓动）
 * @date 2026-07-06
 * @details 封装单个参数的目标值平滑过渡逻辑：
 *          - SetTarget(value, duration, easing): 设定目标
 *          - Update(dt): 每帧推进过渡
 *          - CurrentValue(): 获取当前过渡值
 *          - IsSettled(): 是否已达目标
 *          过渡中可随时修改目标，不跳变，转向新目标。
 * @note 线程安全：单线程调用（逻辑线程独占）。
 */

#pragma once

#include "EasingFunctions.h"

class ParameterTransition {
public:
    ParameterTransition(double initialValue = 0.0);

    /**
     * @brief 设置新的目标值
     * @param target   目标值
     * @param duration 过渡时长（秒）
     * @param easing   缓动曲线类型
     */
    void setTarget(double target, double duration, EasingType easing);

    /**
     * @brief 推进一步过渡
     * @param dt 帧间隔（秒）
     * @return 当前平滑值
     */
    double update(double dt);

    /// @brief 当前过渡值
    double currentValue() const { return m_current; }

    /// @brief 是否已到达目标（接近到 0.001 以内）
    bool isSettled() const;

    /// @brief 立即跳到目标值（无过渡）
    void snapToTarget();

private:
    double     m_current    = 0.0;
    double     m_target     = 0.0;
    double     m_startValue = 0.0;    ///< 本次过渡的起始值
    double     m_duration   = 0.5;    ///< 过渡时长（秒）
    double     m_elapsed    = 0.0;    ///< 已流逝时间（秒）
    EasingType m_easing     = EasingType::EaseOutCubic;
    bool       m_settled    = true;
};
