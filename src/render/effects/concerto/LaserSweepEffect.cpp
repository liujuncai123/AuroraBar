/**
 * @file LaserSweepEffect.cpp
 * @brief 协奏子模式 5：激光线扫实现
 * @date 2026-07-18
 */
#include "LaserSweepEffect.h"
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

LaserSweepEffect::LaserSweepEffect() { AURORA_TRACE("Laser", "Constructor"); }
LaserSweepEffect::~LaserSweepEffect() { AURORA_TRACE("Laser", "Destructor"); cleanup(); }

Result<void> LaserSweepEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("Laser", "initialize() maxParticles={}", maxParticles);
    if (glInitFn) glInitFn();
    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }
    m_segmentEnergy.assign(m_segmentCount, 0.0f);
    m_segmentRole.assign(m_segmentCount, SegmentRole::HarmonicMain);
    // 拖尾最多 kLaserTrailMaxVerts 顶点（GL_LINE_STRIP），头部 1 个点
    m_maxLineVerts  = kLaserTrailMaxVerts;
    m_maxPointVerts = 1;
    m_lineVertices.reserve(m_maxLineVerts);
    m_pointVertices.reserve(m_maxPointVerts);
    return Result<void>::Ok();
}

void LaserSweepEffect::setSegmentEnergy(int segIdx, float emaValue) {
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentEnergy[segIdx] = std::clamp(emaValue, 0.0f, 1.0f);
    }
}

void LaserSweepEffect::setSegmentRole(int segIdx, SegmentRole role) {
    if (segIdx >= 0 && segIdx < m_segmentCount) m_segmentRole[segIdx] = role;
}

void LaserSweepEffect::setAudioColor(const float rgb[3]) {
    m_audioColor[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    m_audioColor[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    m_audioColor[2] = std::clamp(rgb[2], 0.0f, 1.0f);
    m_musicColorEnabled = ParamStore::Instance().GetInt("concerto.musicColor") != 0;
}

unsigned LaserSweepEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{}; glGetShaderInfoLog(s, 1023, nullptr, log);
        AURORA_ERROR("Laser", "{} shader: {}",
                     (type == GL_VERTEX_SHADER ? "VS" : "FS"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Result<void> LaserSweepEffect::compileShaders() {
    AURORA_INFO("Laser", "compileShaders() begin");
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
        AURORA_ERROR("Laser", "Program link: {}", log);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }

    m_locProjection       = glGetUniformLocation(m_program, "uProjection");
    m_locScreenW         = glGetUniformLocation(m_program, "uScreenW");
    m_locScreenH         = glGetUniformLocation(m_program, "uScreenH");
    m_locAudioColor      = glGetUniformLocation(m_program, "uAudioColor");
    m_locMusicColorEnabled = glGetUniformLocation(m_program, "uMusicColorEnabled");

    int maxVerts = std::max(m_maxLineVerts, m_maxPointVerts);
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 maxVerts * sizeof(LaserVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    GLsizei stride = sizeof(LaserVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(LaserVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(LaserVertex, r)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(LaserVertex, alpha)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(LaserVertex, along)));
    glBindVertexArray(0);
    AURORA_INFO("Laser", "compileShaders() OK");
    return Result<void>::Ok();
}

void LaserSweepEffect::cleanup() {
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_vao)     glDeleteVertexArrays(1, &m_vao);
        if (m_vbo)     glDeleteBuffers(1, &m_vbo);
        if (m_program) glDeleteProgram(m_program);
    }
    m_vao = m_vbo = m_program = 0;
    m_lineVertices.clear();
    m_pointVertices.clear();
    m_lineVertCount = m_pointVertCount = 0;
}

const char* LaserSweepEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in float aAlpha;
layout(location=3) in float aAlong;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
out vec3 vColor;
out float vAlpha;
out float vAlong;
void main() {
    // 🔧 [Qt6 迁移修复] framebuffer X+Y 翻转
    float ndcX = 1.0 - aPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    vColor = aColor;
    vAlpha = aAlpha;
    vAlong = aAlong;
}
)glsl";
}

const char* LaserSweepEffect::fragmentSource() {
    return R"glsl(#version 330 core
in vec3 vColor;
in float vAlpha;
in float vAlong;
uniform vec3 uAudioColor;
uniform int uMusicColorEnabled;
out vec4 fragColor;

vec3 reinhard(vec3 c) { return c / (1.0 + c); }

void main() {
    vec3 baseColor = (uMusicColorEnabled == 1) ? uAudioColor : vColor;
    // 3 层发光：
    // 1. 基色（品红）
    vec3 layer1 = baseColor;
    // 2. 越靠近头部（along→1）越亮
    float headBoost = pow(vAlong, 2.0);
    vec3 layer2 = vec3(1.0) * headBoost * 0.5;
    // 3. 头部白点（along == 1.0 时叠白）
    float headWhite = smoothstep(0.98, 1.0, vAlong);
    vec3 layer3 = vec3(1.0) * headWhite;

    vec3 color = layer1 + layer2 + layer3;
    color = reinhard(color * 1.5);
    fragColor = vec4(color, vAlpha);
}
)glsl";
}

void LaserSweepEffect::generateVertices() {
    if (!m_geometry || m_geometry->pointCount() < 3) {
        m_lineVertCount = 0;
        m_pointVertCount = 0;
        return;
    }
    m_lineVertices.clear();
    m_pointVertices.clear();

    const auto& pts = m_geometry->points();
    size_t totalPts = pts.size();
    int scrW = m_geometry->screenW();
    int scrH = m_geometry->screenH();
    double screenPerim = 2.0 * (scrW + scrH);

    // 整体能量 = max(m_segmentEnergy)（激光强度由全局能量驱动）
    float globalEnergy = 0.0f;
    for (int i = 0; i < m_segmentCount; ++i) {
        globalEnergy = std::max(globalEnergy, m_segmentEnergy[i]);
    }
    float threshold = static_cast<float>(ParamStore::Instance().GetDouble("concerto.threshold"));
    if (globalEnergy < threshold) {
        m_lineVertCount = 0;
        m_pointVertCount = 0;
        return;
    }

    // 扫描位置 = fmod(time * 0.5, 1.0)
    float scanPos = std::fmod(m_time * 0.5f, 1.0f);
    if (scanPos < 0.0f) scanPos += 1.0f;

    // 拖尾长度 = globalEnergy * 300px（按周长归一化）
    float tailPx = globalEnergy * 300.0f;
    float tailPerim = tailPx / static_cast<float>(screenPerim);

    // 颜色：品红 #FF00AA
    constexpr float kMagR = 1.0f, kMagG = 0.0f, kMagB = 0.67f;
    constexpr float kWhiteR = 1.0f, kWhiteG = 1.0f, kWhiteB = 1.0f;

    // ── 1. 主线 + 拖尾（GL_LINE_STRIP，沿边框周长方向）──
    int tailSamples = 32;
    for (int i = 0; i < tailSamples; ++i) {
        float along = static_cast<float>(i) / (tailSamples - 1);  // 0=尾部 1=头部
        float perimPos = scanPos - tailPerim * (1.0f - along);
        // 环绕到 [0,1]
        if (perimPos < 0.0f) perimPos += 1.0f;
        if (perimPos > 1.0f) perimPos -= 1.0f;
        perimPos = std::clamp(perimPos, 0.0f, 1.0f);

        double idxFloat = perimPos * (totalPts - 1);
        size_t i0 = static_cast<size_t>(std::floor(idxFloat));
        size_t i1 = std::min(i0 + 1, totalPts - 1);
        double frac = idxFloat - i0;
        float x = static_cast<float>(pts[i0].x + (pts[i1].x - pts[i0].x) * frac);
        float y = static_cast<float>(pts[i0].y + (pts[i1].y - pts[i0].y) * frac);

        // 拖尾 alpha 渐变：尾部 0，头部 1
        float alpha = along * (0.3f + globalEnergy * 0.7f);
        m_lineVertices.push_back({x, y, kMagR, kMagG, kMagB, alpha, along});
    }

    // ── 2. 头部白点（GL_POINTS）──
    double idxFloat = scanPos * (totalPts - 1);
    size_t i0 = static_cast<size_t>(std::floor(idxFloat));
    size_t i1 = std::min(i0 + 1, totalPts - 1);
    double frac = idxFloat - i0;
    float headX = static_cast<float>(pts[i0].x + (pts[i1].x - pts[i0].x) * frac);
    float headY = static_cast<float>(pts[i0].y + (pts[i1].y - pts[i0].y) * frac);
    m_pointVertices.push_back({headX, headY, kWhiteR, kWhiteG, kWhiteB, 1.0f, 1.0f});

    m_lineVertCount  = static_cast<int>(m_lineVertices.size());
    m_pointVertCount = static_cast<int>(m_pointVertices.size());
}

void LaserSweepEffect::render(const Camera& camera) {
    if (!m_program || !m_vao || !m_vbo) {
        static int warnCount = 0;
        if (++warnCount <= 5)
            AURORA_WARN("Laser", "render skip: prog={} vao={} vbo={}", m_program, m_vao, m_vbo);
        return;
    }
    generateVertices();
    if (m_lineVertCount == 0) {
        static int emptyCount = 0;
        if (++emptyCount <= 5)
            AURORA_INFO("Laser", "render skip: lineVertCount=0 time={:.2f} geo={}",
                        m_time, reinterpret_cast<uintptr_t>(m_geometry));
        return;
    }

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
    if (m_locAudioColor >= 0) glUniform3fv(m_locAudioColor, 1, m_audioColor);
    if (m_locMusicColorEnabled >= 0) {
        glUniform1i(m_locMusicColorEnabled, m_musicColorEnabled ? 1 : 0);
    }

    glBindVertexArray(m_vao);

    // ── Draw 1: 主线 + 拖尾（GL_LINE_STRIP）──
    if (m_lineVertCount > 0) {
        int lineCount = std::min(m_lineVertCount, m_maxLineVerts);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        lineCount * sizeof(LaserVertex),
                        m_lineVertices.data());
        glDrawArrays(GL_LINE_STRIP, 0, lineCount);
    }

    // ── Draw 2: 头部白点（GL_POINTS）──
    if (m_pointVertCount > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        m_pointVertCount * sizeof(LaserVertex),
                        m_pointVertices.data());
        glDrawArrays(GL_POINTS, 0, m_pointVertCount);
    }

    glBindVertexArray(0);
}
