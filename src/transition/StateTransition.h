/**
 * @file StateTransition.h
 * @brief 应用状态过渡引擎（Dormant ↔ Active 状态机）
 * @date 2026-07-06
 * @details 管理 AuroraBar 的生命周期状态过渡：
 *          - Dormant → Active（检测到音频，1.5s easeOutBack）
 *          - Active → Dormant（静音 3 秒后，3.0s easeInOutCubic）
 *          过渡参数由 TransitionManager 驱动，输出为统一的平滑系数 [0,1]。
 * @note 线程安全：单线程调用（逻辑线程独占）。
 */

#pragma once

#include "ParameterTransition.h"

/// @brief 生命周期状态
enum class LifecycleState {
    Dormant,   ///< 休眠呼吸态
    Active,    ///< 活跃可视化
};

/**
 * @class StateTransition
 * @brief 状态过渡管理器
 * @details 监听 RMS 值，自动触发 Dormant ↔ Active 过渡。
 *          - 当 RMS > 阈值且处于 Dormant → 启动 Waking 过渡（1.5s easeOutBack）
 *          - 当 RMS < 阈值持续静音时长且处于 Active → 启动 Sleeping 过渡（3.0s easeInOutCubic）
 *          输出 outCoefficient [0,1]：Dormant=0, Active=1，过渡平滑插值。
 */
class StateTransition {
public:
    /// @brief 配置参数
    struct Config {
        double rmsThreshold   = 0.01;  ///< RMS 阈值，低于此值视为静音
        double silenceTimeout = 3.0;   ///< 静音持续多少秒后进入休眠
        double wakeDuration   = 1.5;   ///< 休眠→活跃过渡时长
        double sleepDuration  = 3.0;   ///< 活跃→休眠过渡时长
        double dormantTarget  = 0.0;   ///< 休眠态目标系数（0=完全消失, >0=保持呼吸）
    };

    StateTransition(const Config& cfg = {});

    /**
     * @brief 每帧更新（由 LogicThread 调用）
     * @param rms    当前 RMS 值
     * @param dt     帧间隔（秒）
     * @return 输出系数 [0,1]（Dormant=0, Active=1）
     */
    double update(double rms, double dt);

    /// @brief 当前生命周期状态
    LifecycleState state() const { return m_state; }

    /// @brief 是否正在过渡中
    bool isTransitioning() const { return m_transitioning; }

    /// @brief 输出系数（平滑过渡值 0~1）
    double coefficient() const { return m_coeff.currentValue(); }

    /// @brief 动态更新 RMS 阈值
    void setRmsThreshold(double t) { m_cfg.rmsThreshold = t; }

    /// @brief 动态更新静音超时
    void setSilenceTimeout(double t) { m_cfg.silenceTimeout = t; }

    /// @brief 设置休眠态目标系数（0=完全消失, >0=保持呼吸）
    void setDormantTarget(double t) { m_cfg.dormantTarget = t; }

private:
    Config            m_cfg;
    LifecycleState    m_state          = LifecycleState::Dormant;
    bool              m_transitioning  = false;
    double            m_silentTimer    = 0.0;    ///< 连续静音计时器
    ParameterTransition m_coeff;                 ///< 输出系数过渡 [0,1]
};
