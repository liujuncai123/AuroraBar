/**
 * @file IFeatureExtractor.h
 * @brief 音频特征提取器抽象接口
 * @date 2026-07-06
 * @details 策略模式：每种特征（RMS/频段能量/HPSS/变化率/响度）为独立实现。
 *          虚接口允许新增特征提取器而不修改已有代码（开闭原则）。
 * @note 线程安全：单线程调用（逻辑线程独占）。
 */

#pragma once

#include "FeatureSet.h"
#include <vector>
#include <cstddef>

/**
 * @class IFeatureExtractor
 * @brief 特征提取器抽象接口
 * @details 接收 FFT 对数频段能量（当前帧 + 前一帧），将结果写入预分配的 FeatureSet。
 *          所有实现必须是纯计算、无 I/O、无锁。
 */
class IFeatureExtractor {
public:
    virtual ~IFeatureExtractor() = default;

    /**
     * @brief 处理一帧频谱数据
     * @param spectrum     当前帧的对数频段能量 [bandCount]
     * @param prevSpectrum 前一帧的对数频段能量（HPSS/变化率需要）
     * @param[out] result  预分配的 FeatureSet，通过索引写入
     * @pre result 已调用 resize(bandCount)
     */
    virtual void process(const std::vector<double>& spectrum,
                         const std::vector<double>& prevSpectrum,
                         FeatureSet& result) = 0;

    /// @brief 提取器名称（调试用）
    virtual const char* name() const = 0;
};
