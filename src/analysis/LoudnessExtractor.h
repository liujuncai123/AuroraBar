/**
 * @file LoudnessExtractor.h
 * @brief 响度分布提取器
 * @date 2026-07-06
 * @details 计算每个频段能量占总能量的比率：
 *          ratio[i] = spectrum[i] / sum(spectrum)
 *          用于协奏态"响度分布模式"——每个边框内各频段竞争主导权。
 */

#pragma once

#include "IFeatureExtractor.h"
#include <numeric>

class LoudnessExtractor : public IFeatureExtractor {
public:
    void process(const std::vector<double>& spectrum,
                 const std::vector<double>& /*prevSpectrum*/,
                 FeatureSet& result) override
    {
        double total = std::accumulate(spectrum.begin(), spectrum.end(), 0.0);
        if (total < 1e-9) total = 1e-9;  // 安全：防除零

        const size_t n = spectrum.size();
        for (size_t i = 0; i < n; ++i) {
            result.loudnessRatio[i] = spectrum[i] / total;
        }
    }

    const char* name() const override { return "LoudnessExtractor"; }
};
