/**
 * @file ColorScheme.cpp
 * @brief 颜色方案实现
 * @date 2026-07-06
 */

#include "ColorScheme.h"
#include "../logging/LoggerManager.h"
#include <algorithm>
#include <cmath>

ColorScheme::ColorScheme() {
    m_theme = presets()[0];  // 默认极光青
    AURORA_TRACE("ColorScheme", "Constructor theme={}", m_theme.name);
}

void ColorScheme::setTheme(const std::string& themeName) {
    for (auto& t : presets()) {
        if (t.name == themeName) {
            m_theme = t;
            AURORA_INFO("ColorScheme", "setTheme({})", themeName);
            return;
        }
    }
    AURORA_WARN("ColorScheme", "Theme '{}' not found, keeping current", themeName);
}

RgbColor ColorScheme::computeColor(float freqCenter, float energy) const {
    if (m_mode == ColorMode::FixedTheme) {
        // 混合 primary + glow，偏向能量
        RgbColor c;
        float glowWeight = std::max(0.0f, std::min(1.0f, energy));
        c.r = m_theme.primary.r * (1.0f - glowWeight) + m_theme.glow.r * glowWeight;
        c.g = m_theme.primary.g * (1.0f - glowWeight) + m_theme.glow.g * glowWeight;
        c.b = m_theme.primary.b * (1.0f - glowWeight) + m_theme.glow.b * glowWeight;
        return c;
    } else {
        // 音频驱动：频率 → 色温
        // 低频(80Hz)→暖色(橙红), 高频(8000Hz)→冷色(蓝紫)
        const float fMin = 80.0f, fMax = 8000.0f;
        float t = (std::log(freqCenter) - std::log(fMin)) 
                / (std::log(fMax) - std::log(fMin));
        t = std::max(0.0f, std::min(1.0f, t));

        RgbColor c;
        c.r = 1.0f - t * 0.9f;       // 低频红→高频暗红
        c.g = 0.3f + t * 0.2f;       // 轻微绿偏移
        c.b = t * 0.9f + 0.1f;       // 低频暗蓝→高频亮蓝
        // 能量提升饱和度
        float sat = 0.7f + energy * 0.3f;
        c.r *= sat; c.g *= sat; c.b *= sat;
        return c;
    }
}

const std::vector<ThemeColor>& ColorScheme::presets() {
    static const std::vector<ThemeColor> s_presets = {
        {"aurora_cyan",   {0.0f, 0.85f, 0.85f}, {0.0f, 0.5f, 0.5f}, {0.0f, 1.0f, 1.0f}},
        {"lava_orange",   {1.0f, 0.45f, 0.1f},  {0.7f, 0.2f, 0.0f}, {1.0f, 0.7f, 0.3f}},
        {"nebula_purple", {0.55f, 0.2f, 0.9f},  {0.3f, 0.1f, 0.5f}, {0.7f, 0.4f, 1.0f}},
    };
    return s_presets;
}
