/**
 * @file StateTransition.cpp
 * @brief 状态过渡实现
 * @date 2026-07-06
 */

#include "StateTransition.h"
#include "../logging/LoggerManager.h"

StateTransition::StateTransition(const Config& cfg)
    : m_cfg(cfg)
    , m_coeff(0.0)  // Dormant 起始
{
    AURORA_TRACE("StateTransition", "Constructor threshold={}", m_cfg.rmsThreshold);
}

double StateTransition::update(double rms, double dt) {
    if (dt <= 0.0) dt = 1.0 / 48.0;

    bool hasSound = (rms > m_cfg.rmsThreshold);

    switch (m_state) {

    case LifecycleState::Dormant:
        if (hasSound) {
            AURORA_INFO("StateTransition", "Dormant → Active (rms={:.4f})", rms);
            m_state = LifecycleState::Active;
            m_transitioning = true;
            m_silentTimer = 0.0;
            m_coeff.setTarget(1.0, m_cfg.wakeDuration, EasingType::EaseOutBack);
        }
        break;

    case LifecycleState::Active:
        if (!hasSound) {
            m_silentTimer += dt;
            if (m_silentTimer >= m_cfg.silenceTimeout && !m_transitioning) {
                AURORA_INFO("StateTransition", "Active → Dormant (silence {:.1f}s)",
                            m_silentTimer);
                m_state = LifecycleState::Dormant;
                m_transitioning = true;
                m_coeff.setTarget(m_cfg.dormantTarget, m_cfg.sleepDuration, EasingType::EaseInOutCubic);
            }
        } else {
            // 有声 → 重置静音计时器
            m_silentTimer = 0.0;
            // 如果正在过渡回 Dormant，但又有声音了 → 重新转向 Active
            if (m_transitioning && m_state == LifecycleState::Dormant) {
                AURORA_INFO("StateTransition", "Re-waking during sleep transition");
                m_state = LifecycleState::Active;
                m_coeff.setTarget(1.0, m_cfg.wakeDuration * 0.5, EasingType::EaseOutCubic);
            }
        }
        break;
    }

    double val = m_coeff.update(dt);
    m_transitioning = !m_coeff.isSettled();
    return val;
}
