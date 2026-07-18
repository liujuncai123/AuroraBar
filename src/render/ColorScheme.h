/**
 * @file ColorScheme.h
 * @brief 颜色方案管理（固定主题色 + 音频驱动色温）
 * @date 2026-07-06
 * @details 管理粒子/效果的渲染颜色：
 *          - 固定主题：3 套预置（极光青/熔岩橙/星云紫）
 *          - 音频驱动：频率 → 色温映射（低频暖、高频冷）
 * @note 线程安全：渲染线程独占，不跨线程共享。
 */

#pragma once

#include <array>
#include <string>
#include <vector>
#include <cstdint>

/// @brief 颜色模式
enum class ColorMode {
    FixedTheme,   ///< 固定主题色
    AudioDriven,  ///< 频率驱动色温
};

/// @brief RGB 颜色（0.0~1.0）
struct RgbColor {
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

/// @brief 预置主题
struct ThemeColor {
    std::string name;
    RgbColor    primary;
    RgbColor    secondary;
    RgbColor    glow;
};

/**
 * @class ColorScheme
 * @brief 颜色管理器
 * @details 提供颜色查询接口，支持固定主题和音频驱动两种模式。
 */
class ColorScheme {
public:
    ColorScheme();

    /// @brief 设置颜色模式
    void setMode(ColorMode mode) { m_mode = mode; }

    /// @brief 设置固定主题
    void setTheme(const std::string& themeName);

    /**
     * @brief 计算某频段的渲染颜色
     * @param freqCenter  该段对应频段中心频率（Hz）
     * @param energy      该段当前能量 (0~1)
     * @return RGB 颜色
     */
    RgbColor computeColor(float freqCenter, float energy) const;

    /// @brief 预置主题列表（供 GUI 使用）
    static const std::vector<ThemeColor>& presets();

    ColorMode mode() const { return m_mode; }

private:
    ColorMode  m_mode = ColorMode::FixedTheme;
    ThemeColor m_theme;   ///< 当前主题
};
