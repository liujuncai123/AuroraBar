/**
 * @file BorderGeometry.h
 * @brief 四边边框 + 转角过渡几何计算
 * @date 2026-07-06
 * @details 计算屏幕四边边框的几何参数：
 *          - 四条边（下/右/上/左）各自宽度可调
 *          - 四角过渡区：宽边→窄边自然收窄/扩散
 *          - 总周长归一化 0.0~1.0
 *          - 任意位置通过 perimeter 坐标查找 (x, y)
 * @note 线程安全：渲染线程独占，不跨线程共享。
 */

#pragma once

#include <array>
#include <vector>
#include <cstddef>

/**
 * @struct BorderConfig
 * @brief 四边宽度配置
 */
struct BorderConfig {
    int top    = 80;
    int bottom = 80;
    int left   = 80;
    int right  = 80;
    int cornerTransition = 40;  ///< 转角过渡区像素长度
};

/**
 * @struct BorderPoint
 * @brief 边框上的一个采样点
 */
struct BorderPoint {
    double x = 0.0;           ///< 屏幕坐标 X（像素）
    double y = 0.0;           ///< 屏幕坐标 Y（像素）
    double perimeter = 0.0;   ///< 归一化周长位置 [0,1]
    double width = 0.0;       ///< 该位置的边框宽度（考虑转角过渡）
    int    edgeIndex = 0;     ///< 所属边索引 0=下,1=右,2=上,3=左
    float  nx = 0.0f;         ///< 内向法线 X（已归一化，角区平滑插值）
    float  ny = 0.0f;         ///< 内向法线 Y
};

/**
 * @class BorderGeometry
 * @brief 边框几何计算引擎
 * @details 输入屏幕尺寸+边框配置，输出边框路径采样点数组。
 *          转角过渡区实现平滑收窄/扩散（线性插值）。
 */
class BorderGeometry {
public:
    BorderGeometry();

    /**
     * @brief 根据屏幕分辨率和边框配置重新计算几何
     * @param cfg   边框配置
     * @param screenW 屏幕宽度（像素）
     * @param screenH 屏幕高度（像素）
     * @param sampleCount 采样密度（默认 200 点）
     */
    void compute(const BorderConfig& cfg, int screenW, int screenH,
                 int sampleCount = 200);

    /// @brief 总周长（归一化 = 1.0）
    double totalPerimeter() const { return 1.0; }

    /// @brief 采样点数组
    const std::vector<BorderPoint>& points() const { return m_points; }

    /// @brief 采样点数
    size_t pointCount() const { return m_points.size(); }

    /// @brief 屏幕宽度（像素）
    int screenW() const { return m_screenW; }

    /// @brief 屏幕高度（像素）
    int screenH() const { return m_screenH; }

    /// @brief 转角过渡区像素长度
    int cornerTransition() const { return m_cfg.cornerTransition; }

    /**
     * @brief 根据归一化位置查找最近采样点的索引
     * @param t 归一化位置 [0, 1]
     */
    size_t indexAt(double t) const;

private:
    std::vector<BorderPoint> m_points;
    int m_screenW = 1920;
    int m_screenH = 1080;
    BorderConfig m_cfg;
};
