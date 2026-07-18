/**
 * @file SegmentationManager.cpp
 * @brief 分段管理器实现
 * @date 2026-07-06
 */

#include "SegmentationManager.h"
#include "../logging/LoggerManager.h"

SegmentationManager& SegmentationManager::Instance() {
    static SegmentationManager instance;
    return instance;
}

void SegmentationManager::rebuild() {
    AURORA_INFO("Segmentation", "rebuild() subPerEdge={}", m_subPerEdge);
    buildCycleSegments();
}

void SegmentationManager::setSubSegmentsPerEdge(int n) {
    if (n < 3) n = 3;
    if (n > 8) n = 8;
    m_subPerEdge = n;
    AURORA_INFO("Segmentation", "setSubSegmentsPerEdge({})", n);
}

PhysicsParams SegmentationManager::getPhysicsPreset(int globalIndex) const {
    if (globalIndex < 0 || globalIndex >= static_cast<int>(m_segments.size())) {
        return PhysicsParams::harmonicMain();  // 安全：默认谐波主
    }

    switch (m_segments[globalIndex].role) {
    case SegmentRole::HarmonicMain:     return PhysicsParams::harmonicMain();
    case SegmentRole::HarmonicAux:      return PhysicsParams::harmonicAux();
    case SegmentRole::HarmonicWeak:     return PhysicsParams::harmonicWeak();
    case SegmentRole::PercussiveMain:   return PhysicsParams::percussiveMain();
    case SegmentRole::PercussiveAux:    return PhysicsParams::percussiveAux();
    case SegmentRole::PercussiveWeak:   return PhysicsParams::percussiveWeak();
    default:                            return PhysicsParams::harmonicMain();
    }
}

void SegmentationManager::buildCycleSegments() {
    constexpr int EDGES = 4;  // 下/右/上/左
    const int total = EDGES * m_subPerEdge;
    m_segments.clear();
    m_segments.reserve(total);

    // 角色轮转分布：谐波主、谐波辅、打击主、打击辅、谐波弱、打击弱...
    constexpr SegmentRole roleCycle[] = {
        SegmentRole::HarmonicMain,
        SegmentRole::HarmonicAux,
        SegmentRole::PercussiveMain,
        SegmentRole::PercussiveAux,
        SegmentRole::HarmonicWeak,
        SegmentRole::PercussiveWeak,
    };
    constexpr int ROLE_COUNT = sizeof(roleCycle) / sizeof(roleCycle[0]);

    // 四边等长分配周长（简化：每条边占 1/4）
    const double perimeterPerEdge = 1.0 / EDGES;
    const double perimeterPerSub  = perimeterPerEdge / m_subPerEdge;

    // 频段分配：32 个 FFT band 均分到 total 个段
    constexpr size_t FFT_BANDS = 32;

    for (int edge = 0; edge < EDGES; ++edge) {
        double edgeStart = edge * perimeterPerEdge;

        for (int sub = 0; sub < m_subPerEdge; ++sub) {
            int gIdx = edge * m_subPerEdge + sub;

            SegmentDescriptor seg;
            seg.borderIndex    = edge;
            seg.subIndex       = sub;
            seg.globalIndex    = gIdx;
            seg.role           = roleCycle[gIdx % ROLE_COUNT];
            seg.perimeterStart = edgeStart + sub * perimeterPerSub;
            seg.perimeterEnd   = seg.perimeterStart + perimeterPerSub;

            // 频段分配：按比例映射
            double ratioStart = static_cast<double>(gIdx)     / total;
            double ratioEnd   = static_cast<double>(gIdx + 1) / total;
            seg.freqBinStart  = static_cast<size_t>(ratioStart * FFT_BANDS);
            seg.freqBinEnd    = static_cast<size_t>(ratioEnd   * FFT_BANDS);
            if (seg.freqBinEnd > FFT_BANDS) seg.freqBinEnd = FFT_BANDS;
            if (seg.freqBinEnd <= seg.freqBinStart) seg.freqBinEnd = seg.freqBinStart + 1;

            m_segments.push_back(seg);
        }
    }

    AURORA_INFO("Segmentation", "buildCycleSegments() total={}", total);
}
