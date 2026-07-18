/**
 * @file SpectralFluxExtractor.h
 * @brief 频谱变化率提取器
 * @date 2026-07-06
 * @details 计算当前帧与前一帧每个频段的能量差：
 *          flux[i] = |spectrum[i] - prevSpectrum[i]|
 *          变化率高的频段在协奏态"变化率模式"下驱动抖动动画。
 */

#pragma once

#include "IFeatureExtractor.h"
#include <cmath>
#include <algorithm>

class SpectralFluxExtractor : public IFeatureExtractor {
public:
    void process(const std::vector<double>& spectrum,
                 const std::vector<double>& prevSpectrum,
                 FeatureSet& result) override
    {
        const size_t n = spectrum.size();
        for (size_t i = 0; i < n && i < prevSpectrum.size(); ++i) {
            result.spectralFlux[i] = std::abs(spectrum[i] - prevSpectrum[i]);
        }
    }

    const char* name() const override { return "SpectralFluxExtractor"; }
};
