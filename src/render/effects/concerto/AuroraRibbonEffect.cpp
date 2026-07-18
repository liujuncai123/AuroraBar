/**
 * @file AuroraRibbonEffect.cpp
 * @brief 协奏子模式 6：极光丝带实现
 * @date 2026-07-18
 */
#include "AuroraRibbonEffect.h"
#include "../../BorderGeometry.h"
#include "../../Camera.h"
#include "../../../segmentation/SegmentationManager.h"
#include "../../../core/CommandTypes.h"
#include "../../../logging/LoggerManager.h"
#include "../../../params/ParamStore.h"
#include <GL/glew.h>
#include <GL/wglew.h>
#include <algorithm>
#include <cmath>
#include <cstring>

AuroraRibbonEffect::AuroraRibbonEffect() { AURORA_TRACE("Aurora", "Constructor"); }
AuroraRibbonEffect::~AuroraRibbonEffect() { AURORA_TRACE("Aurora", "Destructor"); cleanup(); }

Result<void> AuroraRibbonEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("Aurora", "initialize() maxParticles={}", maxParticles);
    if (glInitFn) glInitFn();
    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }
    m_segmentEnergy.assign(m_segmentCount, 0.0f);
    m_segmentRole.assign(m_segmentCount, SegmentRole::HarmonicMain);
    m_samplesPerSegment = 32;
    // 每段 32 采样 × 2 顶点（上 + 下）
    m_maxVerts = m_segmentCount * m_samplesPerSegment * 2;
    m_vertices.reserve(m_maxVerts);
    return Result<void>::Ok();
}

void AuroraRibbonEffect::setSegmentEnergy(int segIdx, float emaValue) {
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentEnergy[segIdx] = std::clamp(emaValue, 0.0f, 1.0f);
    }
}

void AuroraRibbonEffect::setSegmentRole(int segIdx, SegmentRole role) {
    if (segIdx >= 0 && segIdx < m_segmentCount) m_segmentRole[segIdx] = role;
}

void AuroraRibbonEffect::setAudioColor(const float rgb[3]) {
    m_audioColor[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    m_audioColor[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    m_audioColor[2] = std::clamp(rgb[2], 0.0f, 1.0f);
    m_musicColorEnabled = ParamStore::Instance().GetInt("concerto.musicColor") != 0;
}

unsigned AuroraRibbonEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{}; glGetShaderInfoLog(s, 1023, nullptr, log);
        AURORA_ERROR("Aurora", "{} shader: {}",
                     (type == GL_VERTEX_SHADER ? "VS" : "FS"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Result<void> AuroraRibbonEffect::compileShaders() {
    AURORA_INFO("Aurora", "compileShaders() begin");
    unsigned vs = compileShader(GL_VERTEX_SHADER, vertexSource());
    unsigned fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource());
    if (!vs || !fs) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, "Shader compile failed"));
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    int linked = 0; glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!linked) {
        char log[1024]{}; glGetProgramInfoLog(m_program, 1023, nullptr, log);
        AURORA_ERROR("Aurora", "Program link: {}", log);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }

    m_locProjection       = glGetUniformLocation(m_program, "uProjection");
    m_locScreenW         = glGetUniformLocation(m_program, "uScreenW");
    m_locScreenH         = glGetUniformLocation(m_program, "uScreenH");
    m_locTime            = glGetUniformLocation(m_program, "uTime");
    m_locFlowSpeed       = glGetUniformLocation(m_program, "uFlowSpeed");
    m_locAudioColor      = glGetUniformLocation(m_program, "uAudioColor");
    m_locMusicColorEnabled = glGetUniformLocation(m_program, "uMusicColorEnabled");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 m_maxVerts * sizeof(AuroraVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    GLsizei stride = sizeof(AuroraVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(AuroraVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(AuroraVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(AuroraVertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(AuroraVertex, alpha)));
    glBindVertexArray(0);
    AURORA_INFO("Aurora", "compileShaders() OK, maxVerts={}", m_maxVerts);
    return Result<void>::Ok();
}

void AuroraRibbonEffect::cleanup() {
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_vao)     glDeleteVertexArrays(1, &m_vao);
        if (m_vbo)     glDeleteBuffers(1, &m_vbo);
        if (m_program) glDeleteProgram(m_program);
    }
    m_vao = m_vbo = m_program = 0;
    m_vertices.clear();
    m_vertCount = 0;
}

const char* AuroraRibbonEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec3 aColor;
layout(location=3) in float aAlpha;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
out vec2 vUV;
out vec3 vColor;
out float vAlpha;
void main() {
    // 🔧 [Qt6 迁移修复] framebuffer X+Y 翻转
    float ndcX = 1.0 - aPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
    vAlpha = aAlpha;
}
)glsl";
}

const char* AuroraRibbonEffect::fragmentSource() {
    return R"glsl(#version 330 core
in vec2 vUV;
in vec3 vColor;
in float vAlpha;
uniform float uTime;
uniform float uFlowSpeed;
uniform vec3 uAudioColor;
uniform int uMusicColorEnabled;
out vec4 fragColor;

vec3 reinhard(vec3 c) { return c / (1.0 + c); }

void main() {
    // 沿周长流动的颜色循环：青 → 紫 → 品红 → 青
    float flowT = fract(vUV.x * 3.0 + uTime * uFlowSpeed);
    vec3 c1 = vec3(0.0, 0.88, 1.0);    // 青
    vec3 c2 = vec3(0.71, 0.0, 1.0);    // 紫
    vec3 c3 = vec3(1.0, 0.0, 0.67);    // 品红
    vec3 layer1;
    if (flowT < 0.33) {
        layer1 = mix(c1, c2, flowT / 0.33);
    } else if (flowT < 0.66) {
        layer1 = mix(c2, c3, (flowT - 0.33) / 0.33);
    } else {
        layer1 = mix(c3, c1, (flowT - 0.66) / 0.34);
    }

    vec3 baseColor = (uMusicColorEnabled == 1) ? uAudioColor : layer1;
    // 3 层发光：
    // 1. 主体（带中央最亮）
    float mid = 1.0 - abs(vUV.y - 0.5) * 2.0;
    vec3 layer2 = baseColor * mid;
    // 2. 边缘高光
    float edge = smoothstep(0.85, 1.0, vUV.y) + smoothstep(0.85, 1.0, 1.0 - vUV.y);
    vec3 layer3 = vec3(1.0) * edge * 0.3;
    // 3. 上方雾气（vUV.y 接近 1 时叠白雾）
    float fog = smoothstep(0.6, 1.0, vUV.y) * 0.2;
    vec3 layerFog = vec3(1.0) * fog;

    vec3 color = layer2 + layer3 + layerFog;
    color = reinhard(color * 1.5);
    fragColor = vec4(color, vAlpha);
}
)glsl";
}

void AuroraRibbonEffect::generateVertices() {
    if (!m_geometry || m_geometry->pointCount() < 3) {
        m_vertCount = 0;
        return;
    }
    m_vertices.clear();

    auto& segMgr = SegmentationManager::Instance();
    const auto& segs = segMgr.segments();
    const auto& pts = m_geometry->points();
    size_t totalPts = pts.size();

    auto& ps = ParamStore::Instance();
    float threshold = static_cast<float>(ps.GetDouble("concerto.threshold"));
    float alphaBase = static_cast<float>(ps.GetDouble("concerto.alphaBase"));
    m_flowSpeed = static_cast<float>(ps.GetDouble("concerto.flowSpeed"));

    constexpr float kBandHalfWidth = 20.0f;  // 带宽 40px / 2

    for (int segIdx = 0; segIdx < m_segmentCount && segIdx < static_cast<int>(segs.size()); ++segIdx) {
        const auto& seg = segs[segIdx];
        float energy = m_segmentEnergy[segIdx];
        if (energy < threshold) continue;

        int edgeIdx = seg.borderIndex;
        bool showEdge = (edgeIdx == 0 && ps.GetInt("concerto.showBottom") != 0) ||
                        (edgeIdx == 1 && ps.GetInt("concerto.showRight")  != 0) ||
                        (edgeIdx == 2 && ps.GetInt("concerto.showTop")    != 0) ||
                        (edgeIdx == 3 && ps.GetInt("concerto.showLeft")   != 0);
        if (!showEdge) continue;

        double segStart = seg.perimeterStart;
        double segEnd   = seg.perimeterEnd;
        double segLen   = segEnd - segStart;
        if (segLen <= 0.0) continue;

        float alpha = alphaBase + energy * (1.0f - alphaBase);

        for (int i = 0; i < m_samplesPerSegment; ++i) {
            float tu = static_cast<float>(i) / (m_samplesPerSegment - 1);
            double posT = segStart + segLen * tu;
            posT = std::clamp(posT, 0.0, 1.0);
            double idxFloat = posT * (totalPts - 1);
            size_t i0 = static_cast<size_t>(std::floor(idxFloat));
            size_t i1 = std::min(i0 + 1, totalPts - 1);
            double frac = idxFloat - i0;
            float bx = static_cast<float>(pts[i0].x + (pts[i1].x - pts[i0].x) * frac);
            float by = static_cast<float>(pts[i0].y + (pts[i1].y - pts[i0].y) * frac);
            float nx = static_cast<float>(pts[i0].nx + (pts[i1].nx - pts[i0].nx) * frac);
            float ny = static_cast<float>(pts[i0].ny + (pts[i1].ny - pts[i0].ny) * frac);

            // 复合波偏移
            float pos = tu * 7.0f;  // [0,7] 沿段方向
            float wave = std::sin(m_time * 1.0f + pos * 5.0f) * 10.0f
                       + std::sin(m_time * 0.5f + pos * 2.0f) * 20.0f
                       + std::sin(m_time * 2.0f + pos * 7.0f) * 5.0f;
            wave *= (0.5f + energy * 0.5f);  // 振幅按能量放大

            float baseOffset = wave;
            float upX = bx + nx * (baseOffset + kBandHalfWidth);
            float upY = by + ny * (baseOffset + kBandHalfWidth);
            float dnX = bx + nx * (baseOffset - kBandHalfWidth);
            float dnY = by + ny * (baseOffset - kBandHalfWidth);

            // TRIANGLE_STRIP：上 + 下交替
            m_vertices.push_back({upX, upY, tu, 1.0f, 1.0f, 1.0f, 1.0f, alpha});
            m_vertices.push_back({dnX, dnY, tu, 0.0f, 1.0f, 1.0f, 1.0f, alpha});
        }
    }
    m_vertCount = static_cast<int>(m_vertices.size());
}

void AuroraRibbonEffect::render(const Camera& camera) {
    if (!m_program || !m_vao || !m_vbo) return;
    generateVertices();
    if (m_vertCount == 0) return;
    if (m_vertCount > m_maxVerts) m_vertCount = m_maxVerts;

    glUseProgram(m_program);
    const auto& mat = camera.matrix();
    if (std::memcmp(mat.data(), m_cachedProj.data(), 16 * sizeof(float)) != 0) {
        glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, mat.data());
        m_cachedProj = mat;
    }
    if (std::fabs(m_cachedSW - m_screenW) > 0.5f || std::fabs(m_cachedSH - m_screenH) > 0.5f) {
        glUniform1f(m_locScreenW, static_cast<float>(m_screenW));
        glUniform1f(m_locScreenH, static_cast<float>(m_screenH));
        m_cachedSW = static_cast<float>(m_screenW);
        m_cachedSH = static_cast<float>(m_screenH);
    }
    glUniform1f(m_locTime, m_time);
    glUniform1f(m_locFlowSpeed, m_flowSpeed);
    if (m_locAudioColor >= 0) glUniform3fv(m_locAudioColor, 1, m_audioColor);
    if (m_locMusicColorEnabled >= 0) {
        glUniform1i(m_locMusicColorEnabled, m_musicColorEnabled ? 1 : 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    m_vertCount * sizeof(AuroraVertex),
                    m_vertices.data());
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, m_vertCount);
    glBindVertexArray(0);
}
