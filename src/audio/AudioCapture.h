/**
 * @file AudioCapture.h
 * @brief 音频采集抽象接口
 * @date 2026-07-06
 * @details 定义音频采集的统一抽象接口，支持 WASAPI 环回实现和 Mock 测试。
 *          遵循 ADR-002：所有可能失败的方法返回 Result<void>。
 * @note 线程安全：接口设计为单线程调用（采集线程独占），不跨线程共享。
 *       // 安全：调用者需先 initialize() 成功后才能 startCapture()。
 */

#pragma once

#include "../core/Result.h"
#include "../core/CommandTypes.h"  // AudioFrame
#include <cstdint>
#include <windows.h>

/**
 * @class IAudioCapture
 * @brief 音频采集抽象接口
 * @details 定义采集设备的统一生命周期：initialize → startCapture → tryReadFrame → stopCapture。
 *          WASAPI 环回实现继承此接口；Mock 实现用于单元测试。
 */
class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;

    /**
     * @brief 初始化采集设备
     * @param sampleRate 采样率（默认 48000 Hz）
     * @param channels   通道数（默认 2，立体声）
     * @return 成功返回 Ok()；失败返回 Error（设备不可用、格式不支持等）
     * @pre COM 已初始化（CoInitializeEx）
     */
    virtual Result<void> initialize(uint32_t sampleRate = 48000,
                                    uint16_t channels = 2) = 0;

    /**
     * @brief 启动音频采集
     * @return 成功返回 Ok()；失败返回 Error
     * @pre initialize() 已成功调用
     */
    virtual Result<void> startCapture() = 0;

    /**
     * @brief 停止音频采集
     * @pre startCapture() 已调用
     */
    virtual void stopCapture() = 0;

    /**
     * @brief 非阻塞读取一帧音频数据
     * @param[out] frame 接收 AudioFrame 数据
     * @return true 成功读取；false 无数据可读
     * @note 调用方应先 WaitForSingleObject(getEventHandle()) 等待数据就绪
     */
    virtual bool tryReadFrame(AudioFrame& frame) = 0;

    /**
     * @brief 获取 WASAPI 事件句柄（供 WaitForSingleObject 使用）
     * @return 事件句柄，未初始化时返回 NULL
     */
    virtual HANDLE getEventHandle() const = 0;

    /**
     * @brief 采集是否处于活跃状态
     */
    virtual bool isActive() const = 0;
};
