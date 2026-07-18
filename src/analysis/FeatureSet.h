/**
 * @file FeatureSet.h
 * @brief 特征提取结果数据结构（预分配，零堆分配）
 * @date 2026-07-06
 * @details 启动时一次性 Resize，运行中通过 [] 索引写入，绝不 push_back。
 *          包含 5 种音频特征：RMS、频段能量、谐波/打击分离、频谱变化率、响度分布。
 * @note 热路径零分配：所有 vector 在 Resize 后大小固定。
 */

#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

/**
 * @struct FeatureSet
 * @brief 音频特征集合（预分配容器）
 * @details 在逻辑线程启动时调用 Resize(bandCount) 一次性分配，
 *          运行时所有特征提取器通过 [] 索引写入对应元素，零堆分配。
 */
struct FeatureSet {
    /**
     * @brief 预分配所有特征数组
     * @param bandCount 对数频段数量
     * @note 仅启动时调用一次，运行中禁止调用
     */
    void resize(size_t bandCount) {
        bandEnergy.resize(bandCount, 0.0);
        harmonicEnergy.resize(bandCount, 0.0);
        percussiveEnergy.resize(bandCount, 0.0);
        spectralFlux.resize(bandCount, 0.0);
        loudnessRatio.resize(bandCount, 0.0);
    }

    double rms = 0.0;                       ///< RMS 总能量 (0.0~1.0)

    std::vector<double> bandEnergy;         ///< 频段能量 [bandCount]
    std::vector<double> harmonicEnergy;     ///< 谐波能量 [bandCount]
    std::vector<double> percussiveEnergy;   ///< 打击能量 [bandCount]
    std::vector<double> spectralFlux;       ///< 频谱变化率 [bandCount]
    std::vector<double> loudnessRatio;      ///< 响度分布比率 [bandCount]

    uint64_t timestamp = 0;                 ///< 对应 AudioFrame 的时间戳
};
