/**
 * @file PhysicsState.h
 * @brief 二阶质量-弹簧-阻尼物理模拟系统
 * @date 2026-07-06
 * @details 将音频特征（突变）转换为平滑的视觉参数（连续），避免视觉跳变。
 *          每个 PhysicsState 实例独立模拟一个参数的物理响应。
 *
 *          物理公式：
 *            acceleration = (stiffness * (target - position) - damping * velocity) / mass
 *            velocity     += acceleration * dt
 *            position     += velocity * dt
 *
 *          相比 EMA（一阶低通），二阶系统可产生"欠阻尼振荡"（音乐感），
 *          且允许 position > 1.0 的 overshoot 体现音乐弹性。
 * @note 线程安全：单线程调用（逻辑线程独占），不跨线程共享。
 */

#pragma once

/// @brief 物理更新时间步长（约 47Hz 逻辑帧 = ~21ms）
inline constexpr double DEFAULT_DT = 1.0 / 48.0;

/**
 * @struct PhysicsParams
 * @brief 物理模拟三参数
 */
struct PhysicsParams {
    double mass      = 0.6;   ///< 质量：越大越迟钝（惯性大）
    double stiffness = 0.4;   ///< 刚度：越大越紧贴目标
    double damping   = 0.82;  ///< 阻尼：越大越不振（过阻尼）
    double nonlinearity = 0.3; ///< 非线性刚度：立方项系数，产生过冲+弹性回弹

    /// @brief 默认为谐波主角色
    static PhysicsParams harmonicMain()   { return {0.6, 0.4, 0.82, 0.3}; }
    static PhysicsParams harmonicAux()    { return {0.7, 0.3, 0.85, 0.25}; }
    static PhysicsParams harmonicWeak()   { return {0.9, 0.2, 0.88, 0.15}; }
    static PhysicsParams percussiveMain() { return {0.2, 0.8, 0.55, 0.5}; }
    static PhysicsParams percussiveAux()  { return {0.3, 0.6, 0.60, 0.4}; }
    static PhysicsParams percussiveWeak() { return {0.5, 0.4, 0.70, 0.25}; }
};

/**
 * @class PhysicsState
 * @brief 单个物理状态实例
 * @details 封装质量-弹簧-阻尼二阶系统的状态和更新逻辑。
 *          使用方式：
 *          1. 构造时传入 PhysicsParams
 *          2. 每帧 setTarget(value) + update(dt) → currentValue()
 *          3. resetParams() 可运行时调参（不跳变）
 */
class PhysicsState {
public:
    PhysicsState(const PhysicsParams& params = {});

    /**
     * @brief 设置目标值（音频特征驱动）
     * @param target 目标位置 (0.0~1.0)
     */
    void setTarget(double target);

    /**
     * @brief 更新物理模拟一帧
     * @param dt 时间步长（秒）
     * @return 平滑后的当前位置
     */
    double update(double dt = DEFAULT_DT);

    /// @brief 当前位置（平滑输出）
    double currentValue() const { return m_position; }

    /// @brief 当前速度（可用于驱动视觉效果）
    double currentVelocity() const { return m_velocity; }

    /**
     * @brief 运行时修改物理参数（不导致位置跳变）
     * @param params 新的物理参数
     */
    void resetParams(const PhysicsParams& params);

private:
    PhysicsParams m_params;
    double m_target   = 0.0;   ///< 目标位置
    double m_position = 0.0;   ///< 当前位置
    double m_velocity = 0.0;   ///< 当前速度
};
