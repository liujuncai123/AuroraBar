/**
 * @file WasapiCapture.cpp
 * @brief WASAPI 环回采集实现
 * @date 2026-07-06
 */

#include "WasapiCapture.h"
#include "../logging/LoggerManager.h"

#include <functiondiscoverykeys_devpkey.h>
#include <combaseapi.h>

namespace {
    constexpr size_t FRAME_SAMPLES = AudioFrame::SAMPLES;
}

// ============================================================
// 构造 / 析构
// ============================================================

WasapiCapture::WasapiCapture() {
    AURORA_TRACE("WasapiCapture", "Constructor");
    // COM 初始化交给 AudioCaptureThread 在 onInitialize 中做
}

WasapiCapture::~WasapiCapture() {
    AURORA_TRACE("WasapiCapture", "Destructor");
    stopCapture();
    releaseResources();
}

// ============================================================
// initialize — WASAPI 设备初始化
// ============================================================

Result<void> WasapiCapture::initialize(uint32_t sampleRate, uint16_t channels) {
    AURORA_INFO("WasapiCapture",
                "initialize() sampleRate={} channels={}", sampleRate, channels);

    // 安全：参数合法性检查，防止除零和无效配置
    if (sampleRate == 0) {
        AURORA_ERROR("WasapiCapture", "sampleRate must not be 0");
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument,
            "sampleRate must not be 0"));
    }
    if (channels == 0 || channels > 8) {
        AURORA_ERROR("WasapiCapture", "channels {} out of range [1,8]", channels);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument,
            "channels must be 1~8"));
    }

    m_sampleRate = sampleRate;
    m_channels = channels;

    HRESULT hr = S_OK;

    // ---- 1. 创建设备枚举器 ----
    {
        IMMDeviceEnumerator* rawEnumerator = nullptr;
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&rawEnumerator));
        m_enumerator.reset(rawEnumerator);
    }
    if (FAILED(hr)) {
        AURORA_ERROR("WasapiCapture",
                     "CoCreateInstance(MMDeviceEnumerator) failed hr=0x{:08X}", hr);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "Failed to create MMDeviceEnumerator"));
    }

    // ---- 2. 获取默认音频渲染端点 ----
    {
        IMMDevice* rawDevice = nullptr;
        hr = m_enumerator->GetDefaultAudioEndpoint(
            eRender, eConsole, &rawDevice);
        m_device.reset(rawDevice);
    }
    if (FAILED(hr)) {
        AURORA_ERROR("WasapiCapture",
                     "GetDefaultAudioEndpoint failed hr=0x{:08X}", hr);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "No default audio render device found"));
    }

    // ---- 3. 激活 IAudioClient ----
    {
        IAudioClient* rawClient = nullptr;
        hr = m_device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&rawClient));
        m_audioClient.reset(rawClient);
    }
    if (FAILED(hr)) {
        AURORA_ERROR("WasapiCapture",
                     "IMMDevice::Activate(IAudioClient) failed hr=0x{:08X}", hr);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "Failed to activate IAudioClient"));
    }

    // ---- 4. 配置 WAVEFORMATEX（IEEE Float, 立体声） ----
    WAVEFORMATEXTENSIBLE wfExt = {};
    wfExt.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfExt.Format.nChannels       = channels;
    wfExt.Format.nSamplesPerSec  = sampleRate;
    wfExt.Format.wBitsPerSample  = 32;
    wfExt.Format.nBlockAlign     = (wfExt.Format.wBitsPerSample / 8) * channels;
    wfExt.Format.nAvgBytesPerSec = wfExt.Format.nSamplesPerSec * wfExt.Format.nBlockAlign;
    wfExt.Format.cbSize          = 22;  // WAVEFORMATEXTENSIBLE 额外字节
    wfExt.Samples.wValidBitsPerSample = 32;
    wfExt.dwChannelMask          = (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                                   : SPEAKER_FRONT_CENTER;
    wfExt.SubFormat              = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    WAVEFORMATEX* pwfx = reinterpret_cast<WAVEFORMATEX*>(&wfExt);

    // 请求约 21ms 的引擎周期（1024 / 48000 ≈ 21.3ms）
    REFERENCE_TIME hnsRequestedDuration = static_cast<REFERENCE_TIME>(
        (10000000ULL * FRAME_SAMPLES) / sampleRate);

    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsRequestedDuration,
        0,  // 共享模式下必须为 0
        pwfx,
        nullptr);

    if (FAILED(hr)) {
        AURORA_ERROR("WasapiCapture",
                     "IAudioClient::Initialize failed hr=0x{:08X}", hr);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "IAudioClient::Initialize failed — device may not support loopback"));
    }

    // ---- 5. 获取缓冲区大小 ----
    hr = m_audioClient->GetBufferSize(&m_bufferFrames);
    if (FAILED(hr)) {
        AURORA_ERROR("WasapiCapture",
                     "GetBufferSize failed hr=0x{:08X}", hr);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "Failed to get WASAPI buffer size"));
    }
    AURORA_INFO("WasapiCapture", "WASAPI buffer frames={}", m_bufferFrames);

    // ---- 6. 获取 IAudioCaptureClient ----
    {
        IAudioCaptureClient* rawCaptureClient = nullptr;
        hr = m_audioClient->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&rawCaptureClient));
        m_captureClient.reset(rawCaptureClient);
    }

    if (FAILED(hr)) {
        AURORA_ERROR("WasapiCapture",
                     "GetService(IAudioCaptureClient) failed hr=0x{:08X}", hr);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "Failed to get IAudioCaptureClient"));
    }

    // ---- 7. 创建事件句柄 ----
    m_eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_eventHandle) {
        AURORA_ERROR("WasapiCapture", "CreateEventW failed");
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "CreateEventW failed"));
    }

    hr = m_audioClient->SetEventHandle(m_eventHandle);
    if (FAILED(hr)) {
        AURORA_ERROR("WasapiCapture",
                     "SetEventHandle failed hr=0x{:08X}", hr);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "SetEventHandle failed"));
    }

    // ---- 8. 预分配累积缓冲区 ----
    m_accumBuffer.resize(FRAME_SAMPLES * 2);  // 留 2x 余量
    m_accumCount = 0;

    // ---- 9. 重置设备失效标志 + 注册设备变更通知 ----
    m_deviceInvalidated.store(false, std::memory_order_release);

    hr = m_enumerator->RegisterEndpointNotificationCallback(
        static_cast<IMMNotificationClient*>(this));
    if (FAILED(hr)) {
        AURORA_WARN("WasapiCapture",
                     "RegisterEndpointNotificationCallback failed hr=0x{:08X}, hotplug unavailable", hr);
        // 非致命：采集仍可用，只是无法自动恢复
    }

    AURORA_INFO("WasapiCapture", "initialize() OK");
    return Result<void>::Ok();
}

// ============================================================
// startCapture / stopCapture
// ============================================================

Result<void> WasapiCapture::startCapture() {
    AURORA_INFO("WasapiCapture", "startCapture()");

    if (!m_audioClient) {
        AURORA_ERROR("WasapiCapture", "startCapture() called before initialize()");
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError,
            "WasapiCapture not initialized"));
    }

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) {
        AURORA_ERROR("WasapiCapture",
                     "IAudioClient::Start failed hr=0x{:08X}", hr);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kAudioDeviceError,
            "IAudioClient::Start failed"));
    }

    m_active.store(true, std::memory_order_release);
    AURORA_INFO("WasapiCapture", "startCapture() OK");
    return Result<void>::Ok();
}

void WasapiCapture::stopCapture() {
    if (!m_active.load(std::memory_order_acquire)) {
        return;
    }

    AURORA_INFO("WasapiCapture", "stopCapture()");

    if (m_audioClient) {
        m_audioClient->Stop();
    }
    m_active.store(false, std::memory_order_release);
}

// ============================================================
// tryReadFrame — 非阻塞读取
// ============================================================

bool WasapiCapture::tryReadFrame(AudioFrame& frame) {
    if (!m_captureClient) {
        return false;
    }

    bool hasData = false;

    // WASAPI 可能在一个信号周期内返回多个 buffer packet
    // 循环读取直到 GetBuffer 返回 AUDCNT_S_BUFFER_EMPTY
    while (true) {
        BYTE*   data = nullptr;
        UINT32  framesAvailable = 0;
        DWORD   flags = 0;
        UINT64  devicePosition = 0;
        UINT64  qpcPosition = 0;

        HRESULT hr = m_captureClient->GetBuffer(
            &data, &framesAvailable, &flags, &devicePosition, &qpcPosition);

        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                m_deviceInvalidated.store(true, std::memory_order_release);
                AURORA_WARN("WasapiCapture",
                            "Device invalidated (hr=0x{:08X}) → reinit needed", hr);
            } else {
                AURORA_ERROR("WasapiCapture",
                             "GetBuffer failed hr=0x{:08X}", hr);
            }
            return false;
        }

        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            break;  // 没有更多数据
        }

        // 🔧 不再信任 AUDCLNT_BUFFERFLAGS_SILENT 标志（部分驱动误报）
        // 统一按实际 PCM 数据处理
        {
            bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            // 安全：当 flags 含 SILENT 时，部分驱动会返回 data=NULL
            //       Microsoft 文档：silent buffer 时 data 指针可能为 NULL
            //       此时直接跳过数据访问，填充零样本，避免空指针解引用崩溃
            float* samples = reinterpret_cast<float*>(data);
            bool anyNonZero = false;

            if (samples == nullptr) {
                // data 为 NULL：填充零样本到累积缓冲（保持时间戳连续性）
                // 安全：手动计算最小值，避免 std::min 与 Windows min 宏冲突
                UINT32 space = static_cast<UINT32>(m_accumBuffer.size() - m_accumCount);
                UINT32 fillFrames = (framesAvailable < space) ? framesAvailable : space;
                for (UINT32 f = 0; f < fillFrames; ++f) {
                    m_accumBuffer[m_accumCount++] = 0.0f;
                }
                AURORA_WARN("WasapiCapture",
                            "GetBuffer returned NULL data (silent), filled {} zero samples",
                            fillFrames);
            } else {
                for (UINT32 f = 0; f < framesAvailable; ++f) {
                    if (m_accumCount >= m_accumBuffer.size()) break;

                    float mono = 0.0f;
                    for (uint16_t ch = 0; ch < m_channels; ++ch) {
                        mono += samples[f * m_channels + ch];
                    }
                    mono /= static_cast<float>(m_channels);
                    m_accumBuffer[m_accumCount++] = mono;
                    if (mono != 0.0f) anyNonZero = true;
                }
            }

            m_captureClient->ReleaseBuffer(framesAvailable);

            if (!isSilent || anyNonZero) {
                hasData = true;
            }
        }
    }

    // 如果累积缓冲够 1024 帧，输出一帧
    if (m_accumCount >= FRAME_SAMPLES) {
        for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
            frame.samples[i] = m_accumBuffer[i];
        }
        frame.timestamp = 0;  // 阶段 5 补充 QPC 时间戳

        // 移除已输出的数据
        size_t remaining = m_accumCount - FRAME_SAMPLES;
        for (size_t i = 0; i < remaining; ++i) {
            m_accumBuffer[i] = m_accumBuffer[FRAME_SAMPLES + i];
        }
        m_accumCount = remaining;
        return true;
    }

    return false;
}

// ============================================================
// getEventHandle / isActive
// ============================================================

HANDLE WasapiCapture::getEventHandle() const {
    return m_eventHandle;
}

bool WasapiCapture::isActive() const {
    return m_active.load(std::memory_order_acquire);
}

// ============================================================
// releaseResources
// ============================================================

void WasapiCapture::releaseResources() {
    AURORA_TRACE("WasapiCapture", "releaseResources()");

    // 先注销设备变更通知，避免 COM 在释放后回调已销毁对象
    if (m_enumerator) {
        m_enumerator->UnregisterEndpointNotificationCallback(
            static_cast<IMMNotificationClient*>(this));
    }

    if (m_eventHandle) {
        CloseHandle(m_eventHandle);
        m_eventHandle = nullptr;
    }

    // COM 智能指针自动 Release
    m_captureClient.reset();
    m_audioClient.reset();
    m_device.reset();
    m_enumerator.reset();
}

// ============================================================
// IMMNotificationClient 实现（设备变更通知）
// ============================================================

HRESULT WasapiCapture::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppvObject = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG WasapiCapture::AddRef() {
    return ++m_comRefCount;
}

ULONG WasapiCapture::Release() {
    ULONG r = --m_comRefCount;
    // 不由 COM 管理生命周期（unique_ptr 托管），不自删除
    return r;
}

HRESULT WasapiCapture::OnDeviceStateChanged(LPCWSTR, DWORD) { return S_OK; }
HRESULT WasapiCapture::OnDeviceAdded(LPCWSTR) { return S_OK; }
HRESULT WasapiCapture::OnDeviceRemoved(LPCWSTR) { return S_OK; }

HRESULT WasapiCapture::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) {
    // 只关心渲染设备的默认设备变更
    if (flow == eRender && role == eConsole) {
        m_deviceInvalidated.store(true, std::memory_order_release);
        AURORA_INFO("WasapiCapture",
                    "Default render device changed → reinit pending");
    }
    return S_OK;
}

HRESULT WasapiCapture::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) { return S_OK; }

// ============================================================
// isDeviceInvalidated
// ============================================================

bool WasapiCapture::isDeviceInvalidated() const {
    return m_deviceInvalidated.load(std::memory_order_acquire);
}
