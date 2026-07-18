/**
 * @file SegmentationManager.h
 * @brief 两层分段协调器
 * @date 2026-07-06
 * @details 统一协调所有下游模块对"当前有多少段、每段角色、位置"的理解。
 *          两层分段 = 第一层粗分段（4 边）+ 第二层细分段（每边 N 个子段）。
 *          作为分段数据的唯一真实来源（Single Source of Truth）。
 *
 *          消费者：
 *          - PhysicsState:  根据段数创建 N 个实例
 *          - TransitionManager:  根据段数创建 N 个参数过渡
 *          - CycleRenderer: 查询段的几何位置
 *          - ColorScheme:   查询段的频段归属决定颜色
 *
 * @note 单例模式：全局唯一实例，多模块通过 Instance() 访问。
 */

#pragma once

#include "../physics/PhysicsState.h"
#include <vector>
#include <cstdint>
#include <cstddef>

/**
 * @enum SegmentRole
 * @brief 子段的职责角色
 */
enum class SegmentRole {
    HarmonicMain,       ///< 谐波主角色
    HarmonicAux,        ///< 谐波辅助角色
    HarmonicWeak,       ///< 谐波弱角色
    PercussiveMain,     ///< 打击主角色
    PercussiveAux,      ///< 打击辅助角色
    PercussiveWeak,     ///< 打击弱角色
    FrequencyBand,      ///< 频率分段（频谱变化率/响度分布模式用）
};

/**
 * @struct SegmentDescriptor
 * @brief 单个子段的完整描述
 */
struct SegmentDescriptor {
    int borderIndex = 0;      ///< 0=下, 1=右, 2=上, 3=左
    int subIndex    = 0;      ///< 边框内子段索引 (0..N-1)
    int globalIndex = 0;      ///< 全局唯一段索引 (0..totalSegments-1)
    SegmentRole role = SegmentRole::HarmonicMain;

    double perimeterStart = 0.0;  ///< 在总周长上的起始位置 (0.0~1.0)
    double perimeterEnd   = 0.0;  ///< 在总周长上的结束位置 (0.0~1.0)

    size_t freqBinStart = 0;      ///< 对应 FFT 频段起始索引
    size_t freqBinEnd   = 0;      ///< 对应 FFT 频段结束索引
};

/**
 * @class SegmentationManager
 * @brief 分段管理器（单例）
 * @details 管理两层分段的构建、查询和变更通知。
 *          循环态：4 边 × N 段 = 全局段数组。
 *          协奏态（V2）：增加角色分配逻辑。
 */
class SegmentationManager {
public:
    static SegmentationManager& Instance();

    /// @brief 根据当前模式重建分段
    void rebuild();

    /// @return 当前全局段总数
    int totalSegments() const { return static_cast<int>(m_segments.size()); }

    /// @return 所有段描述（只读）
    const std::vector<SegmentDescriptor>& segments() const { return m_segments; }

    /**
     * @brief 根据段索引获取物理参数预设
     * @param globalIndex 全局段索引
     * @return 该段角色的物理参数
     */
    PhysicsParams getPhysicsPreset(int globalIndex) const;

    /// @brief 设置每边细分段数（3~6）
    void setSubSegmentsPerEdge(int n);

    /// @return 每边细分段数
    int subSegmentsPerEdge() const { return m_subPerEdge; }

private:
    SegmentationManager() = default;

    /// @brief 循环态分段构建
    void buildCycleSegments();

    int m_subPerEdge = 4;                           ///< 每边子段数
    std::vector<SegmentDescriptor> m_segments;      ///< 段描述数组
};
