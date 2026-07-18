/**
 * @file TransitionManager.cpp
 * @brief 统一过渡调度实现
 * @date 2026-07-06
 */

#include "TransitionManager.h"
#include "../logging/LoggerManager.h"
#include "../params/ParamStore.h"

TransitionManager::TransitionManager()
    : m_stateTrans(StateTransition::Config{})
{
    AURORA_TRACE("TransitionManager", "Constructor");
}

void TransitionManager::rebuild(size_t segmentCount) {
    AURORA_INFO("TransitionManager", "rebuild() segments={}", segmentCount);
    m_paramTrans.clear();
    m_paramTrans.reserve(segmentCount);
    m_lastTargets.resize(segmentCount, -1.0);  // -1 确保首次必定触发 setTarget
    for (size_t i = 0; i < segmentCount; ++i) {
        m_paramTrans.emplace_back(0.0);
    }
}

void TransitionManager::update(double rms,
                                const std::vector<double>& physicsValues,
                                double dt)
{
    // 1. 状态过渡（RMS → Dormant/Active 系数）
    m_stateTrans.update(rms, dt);

    // 2. 参数过渡（PhysicsState 输出 → 最终平滑值）
    const size_t n = std::min(physicsValues.size(), m_paramTrans.size());
    double duration = ParamStore::Instance().GetDouble("transition.duration");
    for (size_t i = 0; i < n; ++i) {
        // 只在目标值变化 > 0.001 时重新设定（避免每帧重置过渡进度）
        if (std::abs(physicsValues[i] - m_lastTargets[i]) > 0.001) {
            m_paramTrans[i].setTarget(physicsValues[i], duration, EasingType::EaseOutCubic);
            m_lastTargets[i] = physicsValues[i];
        }
        m_paramTrans[i].update(dt);
    }
}

double TransitionManager::smoothedValue(size_t i) const {
    if (i < m_paramTrans.size()) {
        return m_paramTrans[i].currentValue();
    }
    return 0.0;
}
