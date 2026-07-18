/**
 * @file Camera.h
 * @brief 倾斜视角相机 + 曲率投影矩阵
 * @date 2026-07-06
 * @details 从斜上方俯视边框的透视相机，支持动态曲率控制。
 *          曲率模拟 CRT 显示器的凹陷效果：RMS 高 → 曲面平（浅凹），RMS 低 → 曲面深（深凹）。
 * @note 线程安全：渲染线程独占，不跨线程共享。
 */

#pragma once

#include <array>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @class Camera
 * @brief 倾斜视角透视相机
 * @details 生成 4×4 透视投影矩阵，包含：
 *          - 倾斜视角（从斜上方 30° 俯视）
 *          - 动态曲率（RMS 驱动，改变视口弧面深度）
 *          - 宽高比自适应（基于屏幕分辨率）
 */
class Camera {
public:
    /// @brief 4×4 投影矩阵（列主序，兼容 OpenGL）
    using Matrix4 = std::array<float, 16>;

    Camera();

    /**
     * @brief 设置屏幕分辨率
     */
    void setScreenSize(int width, int height);

    /**
     * @brief 设置曲率系数
     * @param curvature 0.0=完全平坦, 1.0=最深凹陷
     */
    void setCurvature(double curvature);

    /**
     * @brief 重建投影矩阵
     * @return 列主序 4×4 矩阵
     */
    const Matrix4& update();

    /// @brief 当前投影矩阵
    const Matrix4& matrix() const { return m_proj; }

    /// @brief 当前曲率
    double curvature() const { return m_curvature; }

    /// @brief 宽高比
    double aspectRatio() const { return m_aspect; }

private:
    Matrix4 m_proj{};           ///< 当前投影矩阵
    int     m_width  = 1920;    ///< 屏幕宽
    int     m_height = 1080;    ///< 屏幕高
    double  m_aspect = 16.0/9.0;
    double  m_curvature = 0.5;  ///< 曲率 0~1
    bool    m_dirty = true;
};
