/**
 * @file ParameterTransition.cpp
 * @brief 参数过渡实现
 * @date 2026-07-06
 */

#include "ParameterTransition.h"
#include "../logging/LoggerManager.h"
#include <algorithm>
#include <cmath>

ParameterTransition::ParameterTransition(double initialValue)
    : m_current(initialValue)
    , m_target(initialValue)
{
    AURORA_TRACE("ParameterTransition", "Constructor init={}", initialValue);
}

void ParameterTransition::setTarget(double target, double duration, EasingType easing) {
    if (m_settled) {
        // 从当前值开始新过渡
        m_startValue = m_current;
    } else {
        // 过渡中途改目标：从当前位置开始（不跳变）
        m_startValue = m_current;
    }
    m_target   = target;
    m_duration = (duration > 0.001) ? duration : 0.001;  // 安全：防除零
    m_easing   = easing;
    m_elapsed  = 0.0;
    m_settled  = false;

    AURORA_TRACE("ParameterTransition", "setTarget {} -> {} dur={}",
                 m_startValue, m_target, m_duration);
}

double ParameterTransition::update(double dt) {
    if (dt <= 0.0) dt = 1.0 / 60.0;  // 安全
    if (m_settled) return m_current;

    m_elapsed += dt;

    if (m_elapsed >= m_duration) {
        m_current = m_target;
        m_settled = true;
        return m_current;
    }

    // 归一化时间 [0, 1] → 通过缓动函数 → lerp
    double t = m_elapsed / m_duration;
    double eased = Easing::apply(m_easing, t);
    m_current = m_startValue + (m_target - m_startValue) * eased;

    return m_current;
}

bool ParameterTransition::isSettled() const {
    return m_settled;
}

void ParameterTransition::snapToTarget() {
    m_current = m_target;
    m_settled = true;
}
