/**
 * @file TransitionManager.h
 * @brief 统一过渡调度中心
 * @date 2026-07-06
 * @details 作为 LogicThread 中所有视觉参数过渡的唯一仲裁者。
 *          管理：StateTransition（状态过渡）+ 多个 ParameterTransition（参数过渡）。
 *          每个 PhysicsState 对应一个 ParameterTransition，确保所有参数变化平滑。
 * @note 线程安全：单线程调用（逻辑线程独占）。
 */

#pragma once

#include "StateTransition.h"
#include "ParameterTransition.h"
#include <vector>
#include <cstddef>

/**
 * @class TransitionManager
 * @brief 统一过渡调度中心
 * @details 每帧由 LogicThread 调用 update()，内部推进：
 *          1. StateTransition（RMS → 状态系数 [0,1]）
 *          2. N 个 ParameterTransition（PhysicsState 平滑输出 → 最终渲染参数）
 *
 *          过渡三层：
 *          第一层 PhysicsState（防音频突变）→ 第二层 ParameterTransition（防参数跳变）
 *          → 第三层 StateTransition（防状态割裂）
 */
class TransitionManager {
public:
    TransitionManager();

    /**
     * @brief 初始化/重建：按段数分配参数过渡实例
     * @param segmentCount 段总数
     */
    void rebuild(size_t segmentCount);

    /**
     * @brief 每帧更新所有过渡
     * @param rms 当前 RMS 值
     * @param physicsValues 各 PhysicsState 的原始输出
     * @param dt 帧间隔（秒）
     */
    void update(double rms, const std::vector<double>& physicsValues, double dt);

    /// @brief 状态过渡系数 [0,1]（Dormant→Active）
    double stateCoeff() const { return m_stateTrans.coefficient(); }

    /// @brief 当前生命周期状态
    LifecycleState lifeState() const { return m_stateTrans.state(); }

    /// @brief 获取第 i 个参数的平滑输出
    double smoothedValue(size_t i) const;

    /// @brief 参数过渡个数
    size_t paramCount() const { return m_paramTrans.size(); }

    /// @brief 动态更新休眠阈值
    void setStateRmsThreshold(double t) { m_stateTrans.setRmsThreshold(t); }

    /// @brief 动态更新休眠延迟
    void setStateSilenceTimeout(double t) { m_stateTrans.setSilenceTimeout(t); }

    /// @brief 设置休眠目标系数（保持呼吸 vs 渐隐）
    void setDormantTarget(double t) { m_stateTrans.setDormantTarget(t); }

private:
    StateTransition                    m_stateTrans;      ///< 状态过渡（Dormant↔Active）
    std::vector<ParameterTransition>   m_paramTrans;      ///< 每段一个参数过渡
    std::vector<double>                m_lastTargets;     ///< 上次目标值（防重复 setTarget）
};
