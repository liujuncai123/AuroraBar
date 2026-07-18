/**
 * @file CommandTypes.h
 * @brief 线程间通信数据类型定义（热路径零堆分配）
 * @date 2026-07-06
 * @details 定义 AudioFrame / RenderCommand / ControlCommand 三种 SPSC 队列数据结构。
 *          全部使用定长 std::array，无堆分配，确保采集/逻辑/渲染热路径零分配。
 * @note 这些类型在阶段 3 仅用于声明全局队列变量，
 *       阶段 4-7 将填充实际生产和消费逻辑。
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

// ============================================================
// AudioFrame：采集 → 逻辑 (SPSCQueue<AudioFrame, 8>)
// ============================================================

/// @brief 音频帧数据（WASAPI 环回采集输出）
struct AudioFrame {
    static constexpr size_t SAMPLES = 1024;
    static constexpr uint32_t SAMPLE_RATE = 48000;

    std::array<float, SAMPLES> samples{};   ///< 归一化 PCM (-1.0 ~ 1.0)
    uint64_t timestamp = 0;                  ///< QueryPerformanceCounter

    // 安全：sizeof ≈ 4KB，定长数组零堆分配
};

// ============================================================
// RenderCommand：逻辑 → 渲染 (SPSCQueue<RenderCommand, 64>)
// ============================================================

/// @brief 渲染指令（逻辑线程生成，渲染线程消费）
struct RenderCommand {
    enum class Type : uint8_t {
        GlobalParam,          ///< 全局参数更新
        SegmentParam,         ///< 某子段参数更新
        ModeChange,           ///< 模式切换
        EffectChange,         ///< 效果切换
        DormantState,         ///< 进入/退出休眠
        BorderConfig,         ///< 边框宽度变更 → 重建 BorderGeometry
        OverlayVisible,       ///< 叠加层可见性切换 (paramValue: 1=显示,0=隐藏)
        Onset,                ///< 节拍检测峰值触发
        MusicColor,           ///< 音乐驱动颜色更新（携带 RGB，由 LogicThread FFT 主频→色相计算）
    };

    Type type = Type::GlobalParam;
    int16_t segmentIndex = -1;              ///< -1=全局, ≥0=子段索引

    static constexpr size_t NAME_MAX = 64;
    std::array<char, NAME_MAX> paramName{};  ///< 参数名
    std::array<char, NAME_MAX> targetName{}; ///< 目标值名称

    double paramValue = 0.0;
    /// @brief MusicColor 类型的 RGB（每通道 [0,1]），仅 type==MusicColor 时有效
    /// @note 安全：其他 Type 忽略此字段；LogicThread 推送前需 EMA 平滑避免跳变
    std::array<float, 3> audioColor{};

    uint64_t timestamp = 0;

    // sizeof ≈ 164 bytes, 零堆分配
};

// ============================================================
// ControlCommand：主线程 → 逻辑 (SPSCQueue<ControlCommand, 16>)
// ============================================================

/// @brief 控制指令（主线程 GUI/托盘 → 逻辑线程）
struct ControlCommand {
    enum class Type : uint8_t {
        SetMode,             ///< 切换模式（循环/协奏）
        SetSubMode,          ///< 切换子模式
        SetEffect,           ///< 切换效果体
        SetParam,            ///< 设置参数
        Pause,               ///< 暂停
        Resume,              ///< 恢复
        Collapse,            ///< 收起边框
        Expand,              ///< 展开边框
    };

    Type type = Type::SetParam;
    std::array<char, 64> key{};              ///< 参数键名
    double value = 0.0;

    // sizeof ≈ 80 bytes, 零堆分配
};

// ============================================================
// AppState：全局状态枚举
// ============================================================

/// @brief 应用程序生命周期状态
enum class AppState {
    Dormant,     ///< 初始/休眠状态（低功耗呼吸）
    Waking,      ///< 正在从休眠过渡到活跃
    Active,      ///< 活跃可视化
    Sleeping,    ///< 正在从活跃过渡到休眠
    Stopping,    ///< 用户请求退出
};
