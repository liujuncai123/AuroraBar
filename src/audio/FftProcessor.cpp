/**
 * @file FftProcessor.cpp
 * @brief FFT 处理器实现
 * @date 2026-07-06
 */

#include "FftProcessor.h"
#include "../logging/LoggerManager.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
    constexpr size_t FRAME_SIZE = FftProcessor::FRAME_SIZE;
    constexpr size_t BIN_COUNT = FftProcessor::BIN_COUNT;
}

FftProcessor::FftProcessor() {
    AURORA_TRACE("FftProcessor", "Constructor");
}

FftProcessor::~FftProcessor() {
    AURORA_TRACE("FftProcessor", "Destructor");
    if (m_plan) {
        fftw_destroy_plan(m_plan);
    }
    if (m_input) {
        fftw_free(m_input);
    }
    if (m_output) {
        fftw_free(m_output);
    }
}

Result<void> FftProcessor::initialize(size_t bandCount) {
    AURORA_INFO("FftProcessor", "initialize() bandCount={}", bandCount);

    if (bandCount < 4 || bandCount > 64) {
        AURORA_ERROR("FftProcessor", "bandCount {} out of range [4,64]", bandCount);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument,
            "bandCount must be 4~64"));
    }

    m_bandCount = bandCount;

    // ---- 1. 分配 FFTW 对齐内存 ----
    m_input  = fftw_alloc_real(FRAME_SIZE);
    m_output = fftw_alloc_real(FRAME_SIZE + 2);  // r2c: 2*(N/2+1) doubles

    if (!m_input || !m_output) {
        AURORA_ERROR("FftProcessor", "fftw_alloc_real failed");
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError,
            "FFTW memory allocation failed"));
    }

    // ---- 2. 创建 FFTW plan ----
    m_plan = fftw_plan_dft_r2c_1d(
        static_cast<int>(FRAME_SIZE),
        m_input,
        reinterpret_cast<fftw_complex*>(m_output),
        FFTW_ESTIMATE);  // 快速估算（阶段 13 可切 MEASURE 优化）

    if (!m_plan) {
        AURORA_ERROR("FftProcessor", "fftw_plan_dft_r2c_1d failed");
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError,
            "FFTW plan creation failed"));
    }

    // ---- 3. 生成汉宁窗 ----
    for (size_t i = 0; i < FRAME_SIZE; ++i) {
        m_window[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (FRAME_SIZE - 1)));
    }

    // ---- 4. 生成对数频段合并映射 ----
    buildBandMap();

    AURORA_INFO("FftProcessor", "initialize() OK plan_created");
    return Result<void>::Ok();
}

void FftProcessor::process(const std::array<float, FRAME_SIZE>& samples,
                           std::vector<double>& spectrum)
{
    // 安全：防御未初始化调用（initialize() 失败或未调用时保护）
    if (!m_plan || !m_input || !m_output) {
        AURORA_ERROR("FftProcessor", "process() called before successful initialize()");
        spectrum.resize(m_bandCount);
        std::fill(spectrum.begin(), spectrum.end(), 0.0);
        return;
    }

    // ---- 1. 拷贝浮点样本 + 应用汉宁窗 ----
    for (size_t i = 0; i < FRAME_SIZE; ++i) {
        m_input[i] = static_cast<double>(samples[i]) * m_window[i];
    }

    // ---- 2. 执行 FFT ----
    fftw_execute(m_plan);

    // ---- 3. 计算幅度谱（仅正频率 bins 0..BIN_COUNT-1） ----
    // r2c 输出格式：re[0], re[1], im[1], re[2], im[2], ...
    double magnitude[BIN_COUNT];
    magnitude[0] = std::abs(m_output[0]) / FRAME_SIZE;  // DC

    for (size_t i = 1; i < BIN_COUNT; ++i) {
        double re = m_output[i];
        double im = m_output[FRAME_SIZE - i];  // r2c 存储格式
        magnitude[i] = std::sqrt(re * re + im * im) / FRAME_SIZE;
    }

    // ---- 4. 对数频段合并 ----
    // 每个 band 累积能量再归一化
    spectrum.resize(m_bandCount);
    std::fill(spectrum.begin(), spectrum.end(), 0.0);

    for (size_t bin = 0; bin < BIN_COUNT; ++bin) {
        size_t band = m_binToBand[bin];
        spectrum[band] += magnitude[bin];
    }

    // 每 band 除以 bin 数量做均值归一化
    for (size_t b = 0; b < m_bandCount; ++b) {
        if (m_bandBinCount[b] > 0) {
            spectrum[b] /= static_cast<double>(m_bandBinCount[b]);
        }
    }
}

void FftProcessor::buildBandMap() {
    m_binToBand.resize(BIN_COUNT);
    m_bandBinCount.resize(m_bandCount, 0);

    // 对数频率轴：从 20Hz 到 22050Hz (Nyquist@44100, 实际 48kHz Nyquist=24000)
    const double fMin = 20.0;
    const double fMax = 22050.0;
    const double logMin = std::log(fMin);
    const double logMax = std::log(fMax);

    for (size_t bin = 0; bin < BIN_COUNT; ++bin) {
        // bin → 频率 (假设 48kHz)
        double freq = static_cast<double>(bin) * 48000.0 / FRAME_SIZE;

        // 对数映射
        double logFreq = std::log(std::max(freq, fMin));
        double ratio = (logFreq - logMin) / (logMax - logMin);
        ratio = std::max(0.0, std::min(1.0, ratio));

        size_t band = static_cast<size_t>(ratio * (m_bandCount - 1));
        if (band >= m_bandCount) band = m_bandCount - 1;

        m_binToBand[bin] = band;
        m_bandBinCount[band]++;
    }
}
