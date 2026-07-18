/**
 * @file IConcertoEffect.h
 * @brief 协奏模式效果基类接口
 * @date 2026-07-18
 * @details 继承 IEffect，新增协奏特定的接口：段能量、段角色、
 *          时间、屏幕尺寸、边框几何、音乐驱动颜色。
 *          ConcertoRenderer 集中做 EMA 平滑，Effect 只接收平滑后的值。
 * @note 线程安全：渲染线程独占，不跨线程共享。
 */
#pragma once

#include "../IEffect.h"
#include "../../../segmentation/SegmentationManager.h"  // SegmentRole

class BorderGeometry;
class Camera;

/**
 * @class IConcertoEffect
 * @brief 协奏模式效果基类
 * @details 所有协奏子模式 Effect 的统一接口。
 *          ConcertoRenderer 通过此接口与具体 Effect 解耦，subMode 切换时
 *          由 ConcertoEffectFactory 创建新实例并替换。
 * @note 安全：所有 set* 方法均由渲染线程在 makeCurrent 后调用，
 *              Effect 内部应做 segIdx 越界检查与 emaValue 范围 clamp。
 */
class IConcertoEffect : public IEffect {
public:
    /// @brief 设置某段 EMA 平滑后的能量 [0,1]
    /// @param segIdx 段索引 [0, segmentCount)
    /// @param emaValue EMA 平滑后的能量值
    /// @pre 调用方已完成 EMA 平滑（ConcertoRenderer 集中处理）
    virtual void setSegmentEnergy(int segIdx, float emaValue) = 0;

    /// @brief 设置某段角色（谐波/打击）
    /// @param segIdx 段索引
    /// @param role SegmentRole 枚举值
    virtual void setSegmentRole(int segIdx, SegmentRole role) = 0;

    /// @brief 设置全局时间（秒）
    virtual void setTime(float t) = 0;

    /// @brief 设置屏幕尺寸（物理像素）
    virtual void setScreenSize(int w, int h) = 0;

    /// @brief 设置边框几何引用（不持有所有权，调用方保证生命周期）
    virtual void setBorderGeometry(const BorderGeometry* geo) = 0;

    /// @brief 设置音乐驱动颜色
    /// @param rgb 长度 3 的 float 数组，每通道 [0,1]
    /// @note 仅当 ParamStore 中 concerto.musicColor=1 时由 LogicThread 推送
    virtual void setAudioColor(const float rgb[3]) = 0;

    /// @brief 编译 shader（GL 上下文就绪后调用）
    /// @return 成功 Ok()；失败 Error（含 shader 编译错误日志）
    /// @note 安全：失败时调用方应回退到 subMode 0
    virtual Result<void> compileShaders() = 0;
};
