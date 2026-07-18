/**
 * @file Camera.cpp
 * @brief 相机投影矩阵实现
 * @date 2026-07-06
 */

#include "Camera.h"
#include "../logging/LoggerManager.h"
#include <cmath>

Camera::Camera() {
    AURORA_TRACE("Camera", "Constructor {}x{}", m_width, m_height);
}

void Camera::setScreenSize(int width, int height) {
    m_width  = width;
    m_height = height;
    m_aspect = (height > 0) ? static_cast<double>(width) / height : 16.0 / 9.0;
    m_dirty  = true;
}

void Camera::setCurvature(double curvature) {
    m_curvature = std::max(0.0, std::min(1.0, curvature));
    m_dirty = true;
}

const Camera::Matrix4& Camera::update() {
    if (!m_dirty) return m_proj;

    // 边框视觉器使用屏幕坐标 → NDC 直接映射，不做额外变换
    // 曲率值 m_curvature 由 RMS 实时驱动（0.30 活跃→0.85 安静），
    // 供着色器通过 uniform 获取实现透视缩放等深度效果
    m_proj.fill(0.0f);
    m_proj[0]  = 1.0f;
    m_proj[5]  = 1.0f;
    m_proj[10] = 1.0f;
    m_proj[15] = 1.0f;

    m_dirty = false;
    return m_proj;
}
