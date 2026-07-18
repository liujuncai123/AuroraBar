/**
 * @file FftProcessor.h
 * @brief FFT 处理器 — 1024 点实数 FFT + 汉宁窗 + 对数频段合并
 * @date 2026-07-06
 * @details 基于 FFTW3 双精度实现，输入归一化 PCM (-1.0~1.0)，输出对数频段能量。
 *          启动时创建 FFTW plan（预分配），运行中仅调用 fftw_execute，零堆分配。
 * @note 线程安全：单线程调用（逻辑线程独占），不跨线程共享。
 */

#pragma once

#include "../core/Result.h"
#include <vector>
#include <array>
#include <cstdint>
#include <cstddef>
#include <fftw3.h>

/**
 * @class FftProcessor
 * @brief 1024 点实数 FFT 处理器
 * @details 输入 1024 个归一化 float 样本 → 汉宁窗 → FFT → 对数频段能量。
 *          内部使用 fftw_plan_dft_r2c_1d，双精度运算。
 *          对数频段合并将 512 个线性 bin 合并为指定数量的对数间隔频段。
 *
 *          使用方式：
 *          1. initialize(bandCount) — 创建 FFTW plan
 *          2. process(samples) → spectrum[bandCount] — 每次调用
 */
class FftProcessor {
public:
    FftProcessor();
    ~FftProcessor();

    // 禁止拷贝
    FftProcessor(const FftProcessor&) = delete;
    FftProcessor& operator=(const FftProcessor&) = delete;

    /// @brief 输入帧大小
    static constexpr size_t FRAME_SIZE = 1024;

    /// @brief FFT 输出 bin 数（实数 FFT = N/2+1）
    static constexpr size_t BIN_COUNT = FRAME_SIZE / 2 + 1;

    /**
     * @brief 初始化 FFTW plan 和汉宁窗
     * @param bandCount 对数频段数量（16~32，默认 32）
     * @return 成功 Ok()；失败 Error
     */
    Result<void> initialize(size_t bandCount = 32);

    /**
     * @brief 处理一帧音频数据
     * @param samples  输入：1024 个归一化 float (-1.0~1.0)
     * @param spectrum 输出：对数频段能量 (0.0~1.0)，大小 = bandCount
     * @pre initialize() 已调用，spectrum.size() == bandCount
     */
    void process(const std::array<float, FRAME_SIZE>& samples,
                 std::vector<double>& spectrum);

    /// @brief 当前频段数
    size_t bandCount() const { return m_bandCount; }

private:
    /// @brief 生成对数频段合并映射表
    void buildBandMap();

    fftw_plan     m_plan = nullptr;         ///< FFTW plan（预分配）
    double*       m_input = nullptr;        ///< FFTW 输入缓冲（对齐内存）
    double*       m_output = nullptr;       ///< FFTW 输出缓冲（对齐内存）
    size_t        m_bandCount = 32;         ///< 对数频段数
    std::array<double, FRAME_SIZE> m_window{};  ///< 汉宁窗系数

    /// @brief 频段映射：每个线性 bin → 所属对数频段索引
    std::vector<size_t> m_binToBand;

    /// @brief 每个对数频段的线性 bin 数量（用于归一化）
    std::vector<size_t> m_bandBinCount;
};
