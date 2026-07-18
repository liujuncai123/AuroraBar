/**
 * @file PulseRingEffect.cpp
 * @brief 协奏子模式 7：脉冲环实现
 * @date 2026-07-18
 */
#include "PulseRingEffect.h"
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

PulseRingEffect::PulseRingEffect() { AURORA_TRACE("PulseRing", "Constructor"); }
PulseRingEffect::~PulseRingEffect() { AURORA_TRACE("PulseRing", "Destructor"); cleanup(); }

Result<void> PulseRingEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("PulseRing", "initialize() maxParticles={}", maxParticles);
    if (glInitFn) glInitFn();
    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }
    m_segmentEnergy.assign(m_segmentCount, 0.0f);
    m_segmentRole.assign(m_segmentCount, SegmentRole::HarmonicMain);
    m_lastEnergy.assign(m_segmentCount, 0.0f);
    // 安全：环上限（每段最多 5 个并存环）
    m_maxRings = m_segmentCount * 5;
    // 每环 32 段 × 2 层（主线 + 辉光）× 32 顶点
    m_maxVerts = m_maxRings * 32 * 2;
    m_vertices.reserve(m_maxVerts);
    m_rings.reserve(m_maxRings);
    return Result<void>::Ok();
}

void PulseRingEffect::setSegmentEnergy(int segIdx, float emaValue) {
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentEnergy[segIdx] = std::clamp(emaValue, 0.0f, 1.0f);
    }
}

void PulseRingEffect::setSegmentRole(int segIdx, SegmentRole role) {
    if (segIdx >= 0 && segIdx < m_segmentCount) m_segmentRole[segIdx] = role;
}

void PulseRingEffect::setTime(float t) {
    float dt = (m_prevTime > 0.0f) ? (t - m_prevTime) : 0.0f;
    // 安全：dt 钳制
    m_dt = std::clamp(dt, 0.0f, 0.1f);
    m_prevTime = t;
    m_time = t;
}

void PulseRingEffect::setAudioColor(const float rgb[3]) {
    m_audioColor[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    m_audioColor[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    m_audioColor[2] = std::clamp(rgb[2], 0.0f, 1.0f);
    m_musicColorEnabled = ParamStore::Instance().GetInt("concerto.musicColor") != 0;
}

unsigned PulseRingEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{}; glGetShaderInfoLog(s, 1023, nullptr, log);
        AURORA_ERROR("PulseRing", "{} shader: {}",
                     (type == GL_VERTEX_SHADER ? "VS" : "FS"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Result<void> PulseRingEffect::compileShaders() {
    AURORA_INFO("PulseRing", "compileShaders() begin");
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
        AURORA_ERROR("PulseRing", "Program link: {}", log);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }

    m_locProjection       = glGetUniformLocation(m_program, "uProjection");
    m_locScreenW         = glGetUniformLocation(m_program, "uScreenW");
    m_locScreenH         = glGetUniformLocation(m_program, "uScreenH");
    m_locAudioColor      = glGetUniformLocation(m_program, "uAudioColor");
    m_locMusicColorEnabled = glGetUniformLocation(m_program, "uMusicColorEnabled");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 m_maxVerts * sizeof(RingVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    GLsizei stride = sizeof(RingVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(RingVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(RingVertex, r)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(RingVertex, alpha)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(RingVertex, radiusNorm)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(RingVertex, edgeFactor)));
    glBindVertexArray(0);
    AURORA_INFO("PulseRing", "compileShaders() OK, maxRings={}", m_maxRings);
    return Result<void>::Ok();
}

void PulseRingEffect::cleanup() {
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_vao)     glDeleteVertexArrays(1, &m_vao);
        if (m_vbo)     glDeleteBuffers(1, &m_vbo);
        if (m_program) glDeleteProgram(m_program);
    }
    m_vao = m_vbo = m_program = 0;
    m_rings.clear();
    m_vertices.clear();
    m_vertCount = 0;
}

const char* PulseRingEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in float aAlpha;
layout(location=3) in float aRadiusNorm;
layout(location=4) in float aEdgeFactor;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
out vec3 vColor;
out float vAlpha;
out float vRadiusNorm;
out float vEdgeFactor;
void main() {
    // 🔧 [Qt6 迁移修复] framebuffer X+Y 翻转
    float ndcX = 1.0 - aPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    vColor = aColor;
    vAlpha = aAlpha;
    vRadiusNorm = aRadiusNorm;
    vEdgeFactor = aEdgeFactor;
}
)glsl";
}

const char* PulseRingEffect::fragmentSource() {
    return R"glsl(#version 330 core
in vec3 vColor;
in float vAlpha;
in float vRadiusNorm;
in float vEdgeFactor;
uniform vec3 uAudioColor;
uniform int uMusicColorEnabled;
out vec4 fragColor;

vec3 reinhard(vec3 c) { return c / (1.0 + c); }

void main() {
    vec3 baseColor = (uMusicColorEnabled == 1) ? uAudioColor : vColor;
    // 3 层发光：
    // 1. 主线（edgeFactor=1）：基色
    vec3 layer1 = baseColor * vEdgeFactor;
    // 2. 环外辉光（edgeFactor=0.3）：弱基色
    float glow = (1.0 - vEdgeFactor) * 0.5;
    vec3 layer2 = baseColor * glow;
    // 3. 扩散衰减（半径增大 alpha 衰减）
    float fade = 1.0 - vRadiusNorm;

    vec3 color = (layer1 + layer2) * fade;
    color = reinhard(color * 1.5);
    fragColor = vec4(color, vAlpha * fade);
}
)glsl";
}

void PulseRingEffect::updateRings(float dt) {
    if (dt <= 0.0f) return;
    constexpr float kExpandSpeed = 200.0f;  // 200 px/s 扩散
    for (auto& ring : m_rings) {
        ring.radius += kExpandSpeed * dt;
        ring.age += dt;
    }
    // 移除过期环
    m_rings.erase(
        std::remove_if(m_rings.begin(), m_rings.end(),
                       [](const Ring& r) { return r.age >= r.life; }),
        m_rings.end());
}

void PulseRingEffect::spawnRings(float dt) {
    if (dt <= 0.0f || !m_geometry) return;
    m_spawnTimer += dt;
    if (m_spawnTimer < 0.2f) return;  // 每 200ms 检测一次
    m_spawnTimer = 0.0f;

    // 安全：环上限检查
    if (static_cast<int>(m_rings.size()) >= m_maxRings) return;

    auto& segMgr = SegmentationManager::Instance();
    const auto& segs = segMgr.segments();
    const auto& pts = m_geometry->points();
    size_t totalPts = pts.size();

    for (int segIdx = 0; segIdx < m_segmentCount && segIdx < static_cast<int>(segs.size()); ++segIdx) {
        const auto& seg = segs[segIdx];
        float curE = m_segmentEnergy[segIdx];
        float lastE = m_lastEnergy[segIdx];

        // 峰值检测：当前 > 0.5 且当前 > 上次（上升沿）
        if (curE > 0.5f && curE > lastE) {
            if (static_cast<int>(m_rings.size()) >= m_maxRings) break;

            // 环中心 = 段中点
            double midT = (seg.perimeterStart + seg.perimeterEnd) * 0.5;
            midT = std::clamp(midT, 0.0, 1.0);
            double idxFloat = midT * (totalPts - 1);
            size_t i0 = static_cast<size_t>(std::floor(idxFloat));
            size_t i1 = std::min(i0 + 1, totalPts - 1);
            double frac = idxFloat - i0;
            float cx = static_cast<float>(pts[i0].x + (pts[i1].x - pts[i0].x) * frac);
            float cy = static_cast<float>(pts[i0].y + (pts[i1].y - pts[i0].y) * frac);

            // 段角色色
            SegmentRole role = m_segmentRole[segIdx];
            float cr, cg, cb;
            switch (role) {
            case SegmentRole::HarmonicMain:    cr=1.0f; cg=0.78f; cb=0.0f; break;  // 橙金
            case SegmentRole::HarmonicAux:     cr=1.0f; cg=0.67f; cb=0.0f; break;
            case SegmentRole::HarmonicWeak:    cr=0.8f; cg=0.5f;  cb=0.0f; break;
            case SegmentRole::PercussiveMain:  cr=0.0f; cg=0.88f; cb=1.0f; break;  // 青蓝
            case SegmentRole::PercussiveAux:   cr=0.4f; cg=0.0f;  cb=1.0f; break;
            case SegmentRole::PercussiveWeak:  cr=0.3f; cg=0.0f;  cb=0.7f; break;
            default:                           cr=cg=cb=0.7f;    break;
            }

            Ring ring;
            ring.cx = cx;
            ring.cy = cy;
            ring.radius = 5.0f;
            ring.age = 0.0f;
            ring.life = 1.0f;
            ring.r = cr; ring.g = cg; ring.b = cb;
            m_rings.push_back(ring);
        }
        m_lastEnergy[segIdx] = curE;
    }
}

void PulseRingEffect::buildVertices() {
    m_vertices.clear();

    constexpr int kRingSegments = 32;
    constexpr float kTwoPi = 6.283185307179586f;
    constexpr float kMaxRadius = 200.0f;  // 与扩散速度 × 寿命匹配

    for (const auto& ring : m_rings) {
        float radiusNorm = std::clamp(ring.radius / kMaxRadius, 0.0f, 1.0f);
        float baseAlpha = 1.0f - (ring.age / ring.life);

        // ── 1. 环主线（GL_LINE_STRIP，edgeFactor=1）──
        for (int i = 0; i < kRingSegments; ++i) {
            float theta = kTwoPi * static_cast<float>(i) / kRingSegments;
            float x = ring.cx + ring.radius * std::cos(theta);
            float y = ring.cy + ring.radius * std::sin(theta);
            m_vertices.push_back({x, y, ring.r, ring.g, ring.b,
                                  baseAlpha, radiusNorm, 1.0f});
        }

        // ── 2. 环外辉光（GL_LINE_STRIP，半径+10px，alpha 减半，edgeFactor=0.3）──
        float glowRadius = ring.radius + 10.0f;
        for (int i = 0; i < kRingSegments; ++i) {
            float theta = kTwoPi * static_cast<float>(i) / kRingSegments;
            float x = ring.cx + glowRadius * std::cos(theta);
            float y = ring.cy + glowRadius * std::sin(theta);
            m_vertices.push_back({x, y, ring.r, ring.g, ring.b,
                                  baseAlpha * 0.5f, radiusNorm, 0.3f});
        }
    }
    m_vertCount = static_cast<int>(m_vertices.size());
}

void PulseRingEffect::render(const Camera& camera) {
    if (!m_program || !m_vao || !m_vbo) {
        static int warnCount = 0;
        if (++warnCount <= 5)
            AURORA_WARN("PulseRing", "render skip: prog={} vao={} vbo={}", m_program, m_vao, m_vbo);
        return;
    }

    updateRings(m_dt);
    spawnRings(m_dt);
    buildVertices();

    if (m_vertCount == 0) {
        static int emptyCount = 0;
        if (++emptyCount <= 5)
            AURORA_INFO("PulseRing", "render skip: vertCount=0 rings={} dt={:.4f} geo={}",
                        m_rings.size(), m_dt, reinterpret_cast<uintptr_t>(m_geometry));
        return;
    }
    if (m_vertCount > m_maxVerts) {
        AURORA_WARN("PulseRing", "vertCount {} > max {}, clamping",
                    m_vertCount, m_maxVerts);
        m_vertCount = m_maxVerts;
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

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    m_vertCount * sizeof(RingVertex),
                    m_vertices.data());
    glBindVertexArray(m_vao);

    // 每环 32 顶点主线 + 32 顶点辉光，连续画所有环
    // 安全：使用 GL_LINE_LOOP 让每环首尾相连（每 32 顶点一组）
    int ringsCount = static_cast<int>(m_rings.size());
    constexpr int kRingSegments = 32;
    for (int i = 0; i < ringsCount; ++i) {
        int baseIdx = i * kRingSegments * 2;
        // 主线（GL_LINE_LOOP 闭合）
        if (baseIdx + kRingSegments <= m_vertCount) {
            glDrawArrays(GL_LINE_LOOP, baseIdx, kRingSegments);
        }
        // 辉光（GL_LINE_LOOP 闭合）
        if (baseIdx + kRingSegments * 2 <= m_vertCount) {
            glDrawArrays(GL_LINE_LOOP, baseIdx + kRingSegments, kRingSegments);
        }
    }
    glBindVertexArray(0);
}
