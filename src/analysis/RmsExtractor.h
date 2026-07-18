/**
 * @file RmsExtractor.h
 * @brief RMS 总能量提取器
 * @date 2026-07-06
 */

#pragma once

#include "IFeatureExtractor.h"
#include <cmath>

/**
 * @brief 计算全频段 RMS（均方根）能量
 * @details RMS = sqrt(mean(spectrum²))，归一化到 0~1 范围。
 *          驱动循环态的流速/曲率/亮度等全局参数。
 */
class RmsExtractor : public IFeatureExtractor {
public:
    void process(const std::vector<double>& spectrum,
                 const std::vector<double>& /*prevSpectrum*/,
                 FeatureSet& result) override
    {
        double sumSq = 0.0;
        for (double v : spectrum) {
            sumSq += v * v;
        }
        result.rms = std::sqrt(sumSq / static_cast<double>(spectrum.size()));
    }

    const char* name() const override { return "RmsExtractor"; }
};
