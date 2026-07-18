/**
 * @file AudioCaptureThread.h
 * @brief 音频采集线程——WASAPI 环回采集 + 输出设备热插拔自动恢复
 * @date 2026-07-06
 * @details 继承 ThreadBase，拥有 WasapiCapture 实例。
 *          阶段 4：完整 WASAPI 环回采集 + 重试降级。
 *          阶段 12：设备热插拔（IMMNotificationClient）→ 自动重初始化。
 *          数据流：WASAPI 事件 → tryReadFrame → g_audioQueue.tryPush。
 *          // 安全：COM 资源 RAII；采集失败自动降级为呼吸态，不崩溃。
 * @note 线程安全：通过 SPSC 无锁队列与逻辑线程通信，无 mutex。
 */

#pragma once

#include "../core/ThreadBase.h"
#include "../core/SPSCQueue.h"
#include "../logging/LoggerManager.h"
#include "WasapiCapture.h"
#include <memory>
#include <chrono>

// 前向声明全局队列（main.cpp 中定义）
struct AudioFrame;
template<typename T, size_t C> class SPSCQueue;
extern SPSCQueue<AudioFrame, 32> g_audioQueue;

// SEH 包装器（实现在 src/core/ThreadSEH.cpp）
void AudioCaptureThread_onRun_SEH(class AudioCaptureThread* self);

/// @brief 重初始化冷却时间（秒），防止设备频繁切换时反复重建
constexpr auto kReinitCooldown = std::chrono::seconds(2);
/// @brief 设备失效后最大重试次数（冷却期内）
constexpr int kMaxReinitAttempts = 5;
/// @brief 超过最大重试后的长冷却时间（秒），给用户时间插回设备
constexpr auto kLongReinitCooldown = std::chrono::seconds(30);

/**
 * @class AudioCaptureThread
 * @brief 音频采集线程
 * @details 拥有 WASAPI 采集实例，在 onRun 中执行事件驱动采集循环。
 *          优先级高于逻辑/渲染线程，确保音频数据不丢失。
 *          采集失败时自动重试，持续失败则设置全局降级标志。
 *          支持输出设备热插拔：IMMNotificationClient 检测设备变更 → 自动重初始化。
 */
class AudioCaptureThread : public ThreadBase {
public:
    AudioCaptureThread()
        : ThreadBase("AudioCapture") {}

    /// @brief 获取内部采集器（调试用）
    IAudioCapture* capture() { return m_capture.get(); }
    /// @brief 供 SEH 包装函数标记崩溃，跳过本帧
    void markCrashed() { m_crashed = true; }

protected:
    /**
     * @brief 初始化 COM + WASAPI 采集设备，带重试
     * @return 成功返回 Ok()；失败返回 Error
     * @pre LoggerManager 已初始化
     */
    Result<void> onInitialize() override {
        // ---- COM 初始化（采集线程为 MTA 模式） ----
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            AURORA_ERROR("AudioCapture",
                         "CoInitializeEx failed hr=0x{:08X}", hr);
            return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
                "COM initialization failed"));
        }

        // 记录初始化参数（热插拔恢复时复用）
        m_storedSampleRate = 48000;
        m_storedChannels   = 2;

        // ---- 创建 WASAPI 采集实例 ----
        m_capture = std::make_unique<WasapiCapture>();

        // ---- 初始化 WASAPI，最多重试 3 次 ----
        for (int attempt = 1; attempt <= 3; ++attempt) {
            auto res = m_capture->initialize(m_storedSampleRate, m_storedChannels);
            if (res.isOk()) {
                AURORA_INFO("AudioCapture",
                            "WASAPI initialized (attempt {})", attempt);
                break;
            }

            AURORA_WARN("AudioCapture",
                        "WASAPI init attempt {} failed: {}",
                        attempt, res.error().message);

            if (attempt == 3) {
                AURORA_ERROR("AudioCapture",
                             "WASAPI init failed after 3 attempts, entering degraded mode");
                // 降级运行：线程运行但不采集
                m_capture.reset();
                return Result<void>::Ok();  // 不崩溃，允许降级运行
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // ---- 启动采集 ----
        if (m_capture) {
            auto startRes = m_capture->startCapture();
            if (startRes.isErr()) {
                AURORA_ERROR("AudioCapture",
                             "startCapture failed: {}", startRes.error().message);
                m_capture.reset();
                return Result<void>::Ok();  // 降级
            }
        }

        return Result<void>::Ok();
    }

    /**
     * @brief 采集主循环（事件驱动 + 设备热插拔自动恢复）
     * @note 每帧先检测设备失效标志，失效则自动重组 WASAPI。
     *       WaitForSingleObject 等待 WASAPI 数据就绪，
     *       10ms 超时允许线程检查 shouldRun() 停止信号。
     *       数据入队到全局 g_audioQueue。
     */
    void onRun() override {
        AudioCaptureThread_onRun_SEH(this);
        if (m_crashed) {
            m_crashed = false;
            AURORA_ERROR("AudioCapture", "SEH caught crash in onRun, skipping frame");
            return;
        }
    }

public:
    /// @brief onRun 的实际逻辑体（被 SEH 包装调用，避免 C2712）
    void onRunBody() {
        // ═══════════════════════════════════════════════════
        // ── 设备失效自动恢复 ──
        // ═══════════════════════════════════════════════════
        // 进入恢复状态的条件：
        //   1. m_capture 存在且 isDeviceInvalidated() == true（COM 回调触发）
        //   2. m_reinitAttempts > 0（上一轮重试失败，在恢复序列中）
        //   3. m_reinitAttempts >= kMaxReinitAttempts 且长冷却已过
        bool deviceTriggered = (m_capture && m_capture->isDeviceInvalidated());
        bool inRetrySequence  = (m_reinitAttempts > 0);
        bool longCooldownElapsed = (m_reinitAttempts >= kMaxReinitAttempts &&
                                     m_nextRetryTime.time_since_epoch().count() > 0 &&
                                     std::chrono::steady_clock::now() >= m_nextRetryTime);

        if (deviceTriggered || inRetrySequence || longCooldownElapsed) {
            auto now = std::chrono::steady_clock::now();

            // 首次触发：初始化恢复序列
            if (deviceTriggered && !inRetrySequence) {
                m_reinitAttempts = 1;
                m_lastReinitAttempt = now - kReinitCooldown; // 立即允许第一次尝试
            }

            // 超过最大重试 → 长冷却等待，给用户时间插设备
            if (m_reinitAttempts >= kMaxReinitAttempts) {
                if (!longCooldownElapsed) {
                    // 还在长冷却中
                    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                    return;
                }
                // 长冷却已过，重置计数器，尝试新一轮恢复
                AURORA_INFO("AudioCapture",
                            "Long cooldown elapsed, retrying reinit");
                m_reinitAttempts = 1;
                // 绕过 2s 短冷却，立即尝试
                m_lastReinitAttempt = now - kReinitCooldown;
            }

            // 短冷却检查（2s 防抖）
            if (now - m_lastReinitAttempt < kReinitCooldown) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return;
            }
            m_lastReinitAttempt = now;

            // ── 执行重初始化 ──
            AURORA_WARN("AudioCapture",
                        "Reinitializing WASAPI (attempt {}/{})",
                        m_reinitAttempts, kMaxReinitAttempts);

            // 销毁旧实例（失败残留或失效设备）
            if (m_capture) {
                m_capture->stopCapture();
                m_capture.reset();
            }

            // 创建新实例并初始化
            m_capture = std::make_unique<WasapiCapture>();
            auto res = m_capture->initialize(m_storedSampleRate, m_storedChannels);
            if (res.isOk()) {
                auto startRes = m_capture->startCapture();
                if (startRes.isOk()) {
                    // ✅ 恢复成功
                    m_reinitAttempts = 0;
                    m_nextRetryTime = {};
                    AURORA_INFO("AudioCapture", "Reinit OK → capture resumed");
                    return;
                }
                AURORA_ERROR("AudioCapture",
                             "Reinit startCapture failed: {}",
                             startRes.error().message);
            } else {
                AURORA_ERROR("AudioCapture",
                             "Reinit initialize failed: {}",
                             res.error().message);
            }

            // ❌ 本次尝试失败
            // 销毁失败的实例，以便下次循环重新创建
            if (m_capture) {
                m_capture->stopCapture();
                m_capture.reset();
            }

            ++m_reinitAttempts;

            if (m_reinitAttempts >= kMaxReinitAttempts) {
                // 超过最大重试，进入长冷却
                m_nextRetryTime = std::chrono::steady_clock::now() + kLongReinitCooldown;
                AURORA_ERROR("AudioCapture",
                             "Reinit failed after {} attempts, next retry in {}s",
                             kMaxReinitAttempts,
                             std::chrono::duration_cast<std::chrono::seconds>(
                                 kLongReinitCooldown).count());
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return;
        }

        // ── 降级模式 ──
        if (!m_capture || !m_capture->isActive()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return;
        }

        HANDLE hEvent = m_capture->getEventHandle();
        if (!hEvent) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return;
        }

        // 等待 WASAPI 数据就绪（10ms 超时，快速响应停止信号）
        DWORD waitResult = WaitForSingleObject(hEvent, 10);

        if (waitResult == WAIT_OBJECT_0) {
            // 数据就绪 — 尽可能多地读取 AudioFrame 并入队
            AudioFrame frame;
            while (m_capture->tryReadFrame(frame)) {
                g_audioQueue.tryPush(frame);
            }
        }
        // WAIT_TIMEOUT: 无新数据，继续循环检查 shouldRun()
    }

protected:
    /**
     * @brief 清理 WASAPI 资源和 COM
     */
    void onCleanup() override {
        if (m_capture) {
            m_capture->stopCapture();
            m_capture.reset();
        }
        CoUninitialize();
        AURORA_INFO("AudioCapture", "Cleanup complete");
    }

private:
    std::unique_ptr<WasapiCapture> m_capture;     ///< WASAPI 采集实例（RAII）

    // ---- 热插拔恢复状态 ----
    uint32_t m_storedSampleRate = 48000;           ///< 初始化时的采样率（恢复时复用）
    uint16_t m_storedChannels   = 2;               ///< 初始化时的通道数（恢复时复用）
    int      m_reinitAttempts   = 0;               ///< 当前重试计数，成功后归零 // 安全：仅采集线程读写
    std::chrono::steady_clock::time_point m_lastReinitAttempt{}; ///< 上次重试时间，控制冷却
    std::chrono::steady_clock::time_point m_nextRetryTime{};     ///< 超过最大重试后的下次重试时间
    bool m_crashed = false;  ///< SEH 崩溃标记，跳过本帧
};
