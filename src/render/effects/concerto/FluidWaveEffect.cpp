/**
 * @file FluidWaveEffect.cpp
 * @brief 协奏子模式 2：流体波实现
 * @date 2026-07-18
 */
#include "FluidWaveEffect.h"
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

FluidWaveEffect::FluidWaveEffect() { AURORA_TRACE("FluidWave", "Constructor"); }
FluidWaveEffect::~FluidWaveEffect() { AURORA_TRACE("FluidWave", "Destructor"); cleanup(); }

Result<void> FluidWaveEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("FluidWave", "initialize() maxParticles={}", maxParticles);
    if (glInitFn) glInitFn();
    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }
    m_segmentEnergy.assign(m_segmentCount, 0.0f);
    m_segmentRole.assign(m_segmentCount, SegmentRole::HarmonicMain);
    m_samplesPerSegment = std::max(8, maxParticles / m_segmentCount);
    m_maxVerts = m_segmentCount * m_samplesPerSegment * 2;  // TRIANGLE_STRIP：底+顶交替
    m_vertices.reserve(m_maxVerts);
    return Result<void>::Ok();
}

void FluidWaveEffect::setSegmentEnergy(int segIdx, float emaValue) {
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentEnergy[segIdx] = std::clamp(emaValue, 0.0f, 1.0f);
    }
}

void FluidWaveEffect::setSegmentRole(int segIdx, SegmentRole role) {
    if (segIdx >= 0 && segIdx < m_segmentCount) m_segmentRole[segIdx] = role;
}

void FluidWaveEffect::setAudioColor(const float rgb[3]) {
    m_audioColor[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    m_audioColor[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    m_audioColor[2] = std::clamp(rgb[2], 0.0f, 1.0f);
    m_musicColorEnabled = ParamStore::Instance().GetInt("concerto.musicColor") != 0;
}

unsigned FluidWaveEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{}; glGetShaderInfoLog(s, 1023, nullptr, log);
        AURORA_ERROR("FluidWave", "{} shader: {}",
                     (type == GL_VERTEX_SHADER ? "VS" : "FS"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Result<void> FluidWaveEffect::compileShaders() {
    AURORA_INFO("FluidWave", "compileShaders() begin");
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
        AURORA_ERROR("FluidWave", "Program link: {}", log);
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
                 m_maxVerts * sizeof(FluidWaveVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    GLsizei stride = sizeof(FluidWaveVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(FluidWaveVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(FluidWaveVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(FluidWaveVertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(FluidWaveVertex, alpha)));
    glBindVertexArray(0);
    AURORA_INFO("FluidWave", "compileShaders() OK, maxVerts={}", m_maxVerts);
    return Result<void>::Ok();
}

void FluidWaveEffect::cleanup() {
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

const char* FluidWaveEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in float aU;
layout(location=2) in vec3 aColor;
layout(location=3) in float aAlpha;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
out float vU;
out vec3 vColor;
out float vAlpha;
void main() {
    // 🔧 [Qt6 迁移修复] framebuffer X+Y 翻转
    float ndcX = 1.0 - aPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    vU = aU; vColor = aColor; vAlpha = aAlpha;
}
)glsl";
}

const char* FluidWaveEffect::fragmentSource() {
    return R"glsl(#version 330 core
in float vU;
in vec3 vColor;
in float vAlpha;
uniform float uTime;
uniform float uFlowSpeed;
uniform vec3 uAudioColor;
uniform int uMusicColorEnabled;
out vec4 fragColor;

vec3 reinhard(vec3 c) { return c / (1.0 + c); }

void main() {
    // 流动颜色：紫 #B400FF → 青 #00E0FF
    float flowT = fract(vU + uTime * uFlowSpeed);
    vec3 c1 = vec3(0.71, 0.0, 1.0);   // 紫
    vec3 c2 = vec3(0.0, 0.88, 1.0);   // 青
    float blend = smoothstep(0.0, 0.5, flowT) + smoothstep(0.5, 1.0, flowT) - 0.5;
    vec3 layer1 = mix(c1, c2, blend);
    vec3 baseColor = (uMusicColorEnabled == 1) ? uAudioColor : layer1;
    vec3 color = baseColor * 0.7 + vColor * 0.3;
    color = reinhard(color * 1.5);
    fragColor = vec4(color, vAlpha);
}
)glsl";
}

void FluidWaveEffect::generateVertices() {
    if (!m_geometry || m_geometry->pointCount() < 3) {
        m_vertCount = 0;
        return;
    }
    m_vertices.clear();

    auto& segMgr = SegmentationManager::Instance();
    const auto& segs = segMgr.segments();
    const auto& pts = m_geometry->points();
    size_t totalPts = pts.size();
    int scrW = m_geometry->screenW();
    int scrH = m_geometry->screenH();
    (void)scrW; (void)scrH;  // 屏幕尺寸通过 setScreenSize 提供，此处保留备用

    auto& ps = ParamStore::Instance();
    float threshold = static_cast<float>(ps.GetDouble("concerto.threshold"));
    float alphaBase = static_cast<float>(ps.GetDouble("concerto.alphaBase"));
    m_flowSpeed = static_cast<float>(ps.GetDouble("concerto.flowSpeed"));

    constexpr float kPi = 3.141592653589793f;
    int segsPerEdge = static_cast<int>(segs.size()) / 4;
    if (segsPerEdge < 1) segsPerEdge = 4;

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
        float waveAmp = energy * 30.0f;

        for (int i = 0; i < m_samplesPerSegment; ++i) {
            float t = static_cast<float>(i) / (m_samplesPerSegment - 1);
            double posT = segStart + segLen * t;
            posT = std::clamp(posT, 0.0, 1.0);
            double idxFloat = posT * (totalPts - 1);
            size_t i0 = static_cast<size_t>(std::floor(idxFloat));
            size_t i1 = std::min(i0 + 1, totalPts - 1);
            double frac = idxFloat - i0;
            double bx = pts[i0].x + (pts[i1].x - pts[i0].x) * frac;
            double by = pts[i0].y + (pts[i1].y - pts[i0].y) * frac;
            float nx = static_cast<float>(pts[i0].nx + (pts[i1].nx - pts[i0].nx) * frac);
            float ny = static_cast<float>(pts[i0].ny + (pts[i1].ny - pts[i0].ny) * frac);

            // sin 复合波驱动偏移
            float wave = std::sin(m_time * 2.0f + t * 8.0f * kPi) * waveAmp
                       + std::sin(m_time * 5.0f + t * 3.0f * kPi) * waveAmp * 0.5f;

            float baseX = static_cast<float>(bx);
            float baseY = static_cast<float>(by);
            float tipX = baseX + nx * (wave + 15.0f);  // 距边 15px 起
            float tipY = baseY + ny * (wave + 15.0f);

            // TRIANGLE_STRIP：底点 + 顶点交替
            m_vertices.push_back({baseX, baseY, t, 1.0f, 1.0f, 1.0f, alpha * 0.3f});
            m_vertices.push_back({tipX, tipY, t, 1.0f, 1.0f, 1.0f, alpha});
        }
    }
    m_vertCount = static_cast<int>(m_vertices.size());
}

void FluidWaveEffect::render(const Camera& camera) {
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
                    m_vertCount * sizeof(FluidWaveVertex),
                    m_vertices.data());
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, m_vertCount);
    glBindVertexArray(0);
}
