/**
 * @file WasapiCapture.h
 * @brief WASAPI 环回音频采集实现
 * @date 2026-07-06
 * @details 基于 Windows Audio Session API (WASAPI) 的环回模式（LOOPBACK），
 *          捕获系统全局音频输出。事件驱动模型，48kHz / IEEE Float 格式。
 *          // 安全：COM 资源通过智能指针/RAII 管理，析构自动释放。
 * @note 仅支持 Windows Vista+；环回模式采集系统音频，不采集麦克风。
 */

#pragma once

#include "AudioCapture.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <memory>
#include <cstdint>
#include <vector>

// COM 智能指针释放器（RAII）
struct ComDeleter {
    void operator()(IUnknown* p) const { if (p) p->Release(); }
};

template<typename T>
using ComPtr = std::unique_ptr<T, ComDeleter>;

/**
 * @class WasapiCapture
 * @brief WASAPI 环回采集实现
 * @details 通过 IMMDeviceEnumerator 获取默认音频渲染端点，
 *          以 AUDCLNT_STREAMFLAGS_LOOPBACK 模式打开，捕获系统播放的音频。
 *          事件驱动：创建 Auto-Reset Event，数据就绪时 WASAPI 触发事件。
 *
 *          采集管线：
 *          1. CoInitializeEx → 2. IMMDeviceEnumerator → 3. IAudioClient(LOOPBACK)
 *          → 4. IAudioCaptureClient → 5. 事件等待 → 6. GetBuffer/ReleaseBuffer
 */
class WasapiCapture : public IAudioCapture, public IMMNotificationClient {
public:
    WasapiCapture();
    ~WasapiCapture() override;

    // -- 禁止拷贝 --
    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    // ---- IAudioCapture 接口 ----
    Result<void> initialize(uint32_t sampleRate = 48000,
                            uint16_t channels = 2) override;

    Result<void> startCapture() override;
    void        stopCapture() override;
    bool        tryReadFrame(AudioFrame& frame) override;
    HANDLE      getEventHandle() const override;
    bool        isActive() const override;

    // ---- 设备热插拔 ----
    /// @brief 检测当前采集设备是否已失效（拔出/切换输出设备后为 true）
    bool        isDeviceInvalidated() const;

    // ---- IMMNotificationClient（设备变更通知） ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

private:
    /**
     * @brief 释放所有 COM 资源
     */
    void releaseResources();

    // ---- COM 接口 ----
    ComPtr<IMMDeviceEnumerator>  m_enumerator;
    ComPtr<IMMDevice>            m_device;
    ComPtr<IAudioClient>         m_audioClient;
    ComPtr<IAudioCaptureClient>  m_captureClient;

    // ---- 事件句柄 ----
    HANDLE m_eventHandle = nullptr;          ///< Auto-Reset Event，WASAPI 数据就绪信号

    // ---- 状态 ----
    std::atomic<bool>  m_active{false};              ///< 是否正在采集
    std::atomic<bool>  m_deviceInvalidated{false};   ///< 设备已失效，需重新初始化 // 安全：原子标志，跨 COM 回调线程安全
    std::atomic<ULONG> m_comRefCount{0};             ///< COM 引用计数（非 self-delete 模式，生命周期由 unique_ptr 管理）
    uint32_t           m_sampleRate = 48000;         ///< 采集采样率
    uint16_t          m_channels = 2;        ///< 采集通道数
    uint32_t          m_bufferFrames = 0;    ///< WASAPI 引擎周期帧数

    // ---- 内部缓冲 ----
    /// @brief 累积缓冲区（存放 WASAPI 返回的碎片数据，凑满 1024 帧后输出）
    std::vector<float>  m_accumBuffer;
    size_t              m_accumCount = 0;    ///< 当前累积帧数
};
