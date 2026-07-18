/**
 * @file ConcertoEffectFactory.cpp
 * @brief 协奏效果工厂实现
 * @date 2026-07-18
 * @details 当前为骨架版本，所有 case 注释。
 *          后续 Task 4-12 逐步启用对应 Effect 的 include 和 case。
 */
#include "ConcertoEffectFactory.h"
#include "CrystalEffect.h"          // subMode 0
#include "SpectrumBarEffect.h"      // subMode 1
#include "FluidWaveEffect.h"        // subMode 2
#include "ParticleFlowEffect.h"    // subMode 3
#include "GridBeamEffect.h"         // subMode 4
#include "LaserSweepEffect.h"      // subMode 5
#include "AuroraRibbonEffect.h"    // subMode 6
#include "PulseRingEffect.h"       // subMode 7
#include "../../../logging/LoggerManager.h"

std::unique_ptr<IConcertoEffect> ConcertoEffectFactory::create(int subMode) {
    switch (subMode) {
    case 0: return std::make_unique<CrystalEffect>();
    case 1: return std::make_unique<SpectrumBarEffect>();
    case 2: return std::make_unique<FluidWaveEffect>();
    case 3: return std::make_unique<ParticleFlowEffect>();
    case 4: return std::make_unique<GridBeamEffect>();
    case 5: return std::make_unique<LaserSweepEffect>();
    case 6: return std::make_unique<AuroraRibbonEffect>();
    case 7: return std::make_unique<PulseRingEffect>();
    default:
        AURORA_WARN("ConcertoFactory", "Unknown subMode={}, returning nullptr", subMode);
        return nullptr;
    }
}
