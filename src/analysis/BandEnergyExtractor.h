/**
 * @file BandEnergyExtractor.h
 * @brief 频段能量提取器
 * @date 2026-07-06
 * @details 将 FFT 对数频段能量直接写入 FeatureSet::bandEnergy[]，
 *          每个频段独立归一化到 0~1 范围（基于历史最大值）。
 */

#pragma once

#include "IFeatureExtractor.h"
#include <algorithm>
#include <vector>

class BandEnergyExtractor : public IFeatureExtractor {
public:
    void process(const std::vector<double>& spectrum,
                 const std::vector<double>& /*prevSpectrum*/,
                 FeatureSet& result) override
    {
        const size_t n = spectrum.size();

        // 动态扩展历史最大值数组
        if (m_maxHistory.size() != n) {
            m_maxHistory.resize(n, 1e-6);
        }

        for (size_t i = 0; i < n; ++i) {
            // 指数衰减跟踪最大值
            m_maxHistory[i] = std::max(spectrum[i], m_maxHistory[i] * 0.999);
            result.bandEnergy[i] = (m_maxHistory[i] > 1e-9)
                ? spectrum[i] / m_maxHistory[i]
                : 0.0;
        }
    }

    const char* name() const override { return "BandEnergyExtractor"; }

private:
    std::vector<double> m_maxHistory;  ///< 每个频段的历史最大值（指数衰减）
};
