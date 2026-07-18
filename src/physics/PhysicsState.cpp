/**
 * @file PhysicsState.cpp
 * @brief 二阶物理模拟实现
 * @date 2026-07-06
 */

#include "PhysicsState.h"
#include "../logging/LoggerManager.h"
#include <algorithm>

PhysicsState::PhysicsState(const PhysicsParams& params)
    : m_params(params)
{
    AURORA_TRACE("PhysicsState", "Constructor mass={} stiffness={} damping={}",
                 m_params.mass, m_params.stiffness, m_params.damping);
}

void PhysicsState::setTarget(double target) {
    m_target = std::max(0.0, std::min(1.0, target));  // 安全：clamp
}

double PhysicsState::update(double dt) {
    // 安全：dt 异常保护
    if (dt <= 0.0 || dt > 1.0) {
        dt = DEFAULT_DT;
    }

    // 二阶物理更新
    // acceleration = (stiffness * (target - position) - damping * velocity) / mass
    double invMass = (m_params.mass > 1e-9) ? (1.0 / m_params.mass) : 1.0;
    double error = m_target - m_position;
    double linearForce = m_params.stiffness * error;
    // 非线性立方项：大误差时产生过冲，小误差时几乎无影响（error³ 衰减飞快）
    double cubicForce = m_params.nonlinearity * m_params.stiffness * error * error * error;
    double acceleration = (linearForce + cubicForce - m_params.damping * m_velocity) * invMass;

    // velocity += acceleration * dt
    m_velocity += acceleration * dt;

    // position += velocity * dt
    m_position += m_velocity * dt;

    // 安全：下限 clamp（不 clamp 上限，允许 overshoot 体现音乐感）
    if (m_position < 0.0) m_position = 0.0;

    return m_position;
}

void PhysicsState::resetParams(const PhysicsParams& params) {
    // 仅替换参数，保留当前位置和速度（不跳变）
    m_params = params;
    AURORA_TRACE("PhysicsState", "resetParams mass={} stiffness={} damping={}",
                 m_params.mass, m_params.stiffness, m_params.damping);
}
