// ============================================================================
// ⚠️  DEPRECATED — 本文件已废弃，请勿在新代码中使用
// ----------------------------------------------------------------------------
//  原方案：协奏曲模式单一 Effect（LightColumnEffect）实现 4 种子模式
//         （晶柱 / 光柱 / 频谱 / 均衡器），9 层 glow + ACES 色调映射。
//  问题  ：架构耦合（一个 Effect 承担 4 种子模式），9 层叠加过曝发灰，
//          视觉效果"老土、模糊"，用户反馈"看瞎了"。
//  替代  ：已重写为独立 Effect 架构 ——
//         src/render/effects/concerto/CrystalEffect (subMode 0 晶柱升级)
//         src/render/effects/concerto/SpectrumBarEffect (subMode 1 频谱柱)
//         src/render/effects/concerto/FluidWaveEffect (subMode 2 流体波)
//         src/render/effects/concerto/ParticleFlowEffect (subMode 3 粒子流)
//         src/render/effects/concerto/GridBeamEffect (subMode 4 网格光带)
//         src/render/effects/concerto/LaserSweepEffect (subMode 5 激光线扫)
//         src/render/effects/concerto/AuroraRibbonEffect (subMode 6 极光丝带)
//         src/render/effects/concerto/PulseRingEffect (subMode 7 脉冲环)
//         新架构：每子模式独立 Effect + 独立 shader，3 层发光 + Reinhard 色调映射。
//  说明  ：文件保留供历史参考，ConcertoRenderer 已切换到新 Effect 架构，
//         新代码请勿 include 本文件。CMakeLists 仍编译以保持兼容性。
// ============================================================================
#ifdef _MSC_VER
#pragma message("warning: " __FILE__ " is DEPRECATED — replaced by concerto/*Effect (independent Effect architecture)")
#endif

/**
 * @file LightColumnEffect.h
 * @brief [DEPRECATED] 光柱 / 晶柱视觉效果——协奏曲模式用（已被 concerto/*Effect 替代）
 * @date 2026-07-11
 * @details 支持两种渲染管线：
 *          1. 扁平光柱（legacy，subMode 1-3）：2D 矩形 + 高斯光晕
 *          2. 晶柱边境（crystal，subMode 0/默认）：六边形棱柱 + 菲涅尔折射
 * @note 线程安全：渲染线程独占。
 */

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "IEffect.h"
#include "../../core/Result.h"
#include "../Camera.h"
#include <GL/glew.h>
#include <vector>
#include <array>
#include <functional>

/// @brief 扁平光柱顶点（32 字节，GPU 友好）
struct ColumnVertex {
    float x = 0.0f, y = 0.0f;
    float localX = 0.0f;
    float localY = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;
};

static_assert(sizeof(ColumnVertex) == 32, "ColumnVertex must be 32 bytes");

/// @brief 晶柱顶点（48 字节，3D + 法线 + 颜色）
struct CrystalVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float localH = 0.0f;     ///< 柱内高度 [0,1]，0=底部 1=顶端
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;
};

static_assert(sizeof(CrystalVertex) == 44, "CrystalVertex must be 44 bytes");

class BorderGeometry;

/**
 * @class LightColumnEffect
 * @brief 光柱 / 晶柱效果实现
 * @details 管理着色器和 VAO/VBO。外部（ConcertoRenderer）负责生成顶点数据。
 */
class LightColumnEffect : public IEffect {
public:
    LightColumnEffect();
    ~LightColumnEffect() override;

    Result<void> initialize(std::function<void()> glInitFn, int maxParticles) override;
    void render(const Camera& camera) override;
    void cleanup() override;

    const char* name() const override { return "LightColumnEffect"; }
    int activeParticles() const override { return m_columnCount / 4; }

    /// @brief 编译着色器（GL 上下文就绪后调用）
    Result<void> compileShaders();

    /// @brief 设置屏幕尺寸（pixels）
    void setScreenSize(int w, int h) { m_screenW = w; m_screenH = h; }

    /// @brief 设置子模式（0=晶柱 1=霓虹 2=火焰 3=均衡器）
    void setSubMode(int mode);
    void setInnerGlow(float val) { m_innerGlow = val; }
    void setFlowSpeed(float val) { m_flowSpeed = val; }

    /// @brief 设置时间（秒，用于动画效果）
    void setTime(float t) { m_time = t; }

    /// @brief 上传扁平光柱顶点数据到 VBO
    void setColumns(const ColumnVertex* vertices, int count);

    /// @brief 上传晶柱顶点数据到晶柱 VBO
    void setCrystalColumns(const CrystalVertex* vertices, int count);

    /// @brief 当前光柱顶点数
    int columnVertexCount() const { return m_columnCount; }

private:
    static unsigned compileShader(unsigned type, const char* src);
    void compileFlatPipeline();
    Result<void> compileCrystalShaders();

    // ── 扁平光柱管线 ──
    unsigned m_vao = 0;
    unsigned m_vbo = 0;
    unsigned m_program = 0;

    int m_locProjection = -1;
    int m_locScreenW = -1;
    int m_locScreenH = -1;
    int m_locSubMode = -1;
    int m_locTime    = -1;
    int m_locInnerGlow = -1;

    // ── 晶柱管线 ──
    unsigned m_crystalVao = 0;
    unsigned m_crystalVbo = 0;
    unsigned m_crystalProg = 0;
    int m_cLocProjection = -1;
    int m_cLocScreenW    = -1;
    int m_cLocScreenH    = -1;
    int m_cLocTime       = -1;
    int m_cLocInnerGlow  = -1;
    int m_cLocFlowSpeed  = -1;
    int m_crystalVertCount = 0;
    int m_crystalMaxVerts  = 0;

    // 缓存上次设置的 uniform 值，避免每帧重复 glUniform*
    std::array<float, 16> m_cachedProj{};
    float m_cachedScreenW = -1.0f, m_cachedScreenH = -1.0f;

    int m_screenW = 1920;
    int m_screenH = 1440;
    int m_subMode = 0;
    float m_time = 0.0f;
    float m_innerGlow = 1.0f;
    float m_flowSpeed = 0.4f;  ///< 流动亮带速度
    int m_maxVertices = 0;
    int m_columnCount = 0;

    static const char* vertexSource();
    static const char* fragmentSource();
    static const char* crystalVertexSource();
    static const char* crystalFragmentSource();
};
