/**
 * @file HpssExtractor.h
 * @brief 谐波/打击分离提取器（Harmonic-Percussive Source Separation）
 * @date 2026-07-06
 * @details 使用帧间中值滤波分离谐波（持续）和打击（瞬态）成分：
 *          - 谐波 = 沿时间轴的中值滤波（保留稳定成分）
 *          - 打击 = 原始 - 谐波（保留瞬态成分）
 *          对每个频段独立处理，维持可配置的历史帧缓冲区。
 */

#pragma once

#include "IFeatureExtractor.h"
#include <vector>
#include <deque>
#include <algorithm>

class HpssExtractor : public IFeatureExtractor {
public:
    void process(const std::vector<double>& spectrum,
                 const std::vector<double>& /*prevSpectrum*/,
                 FeatureSet& result) override
    {
        const size_t n = spectrum.size();

        // 保证历史缓冲区大小匹配
        if (m_history.size() != n) {
            m_history.clear();
            for (size_t i = 0; i < n; ++i) {
                m_history.emplace_back();
            }
        }

        for (size_t i = 0; i < n; ++i) {
            auto& q = m_history[i];
            q.push_back(spectrum[i]);
            if (q.size() > HISTORY_SIZE) {
                q.pop_front();
            }

            // 中值滤波 → 谐波
            double harmonic = median(q);
            double percussive = spectrum[i] - harmonic;
            if (percussive < 0.0) percussive = 0.0;

            result.harmonicEnergy[i]   = harmonic;
            result.percussiveEnergy[i] = percussive;
        }
    }

    const char* name() const override { return "HpssExtractor"; }

private:
    /// @brief 计算 deque 的中值
    static double median(std::deque<double>& q) {
        if (q.empty()) return 0.0;
        std::vector<double> sorted(q.begin(), q.end());
        std::sort(sorted.begin(), sorted.end());
        size_t mid = sorted.size() / 2;
        if (sorted.size() % 2 == 0) {
            return (sorted[mid - 1] + sorted[mid]) / 2.0;
        }
        return sorted[mid];
    }

    static constexpr size_t HISTORY_SIZE = 5;  ///< 中值滤波窗口帧数
    std::vector<std::deque<double>> m_history; ///< 每频段的历史值队列
};
