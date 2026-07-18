/**
 * @file ParticleFlowEffect.cpp
 * @brief 协奏子模式 3：粒子流实现
 * @date 2026-07-18
 */
#include "ParticleFlowEffect.h"
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

ParticleFlowEffect::ParticleFlowEffect() { AURORA_TRACE("ParticleFlow", "Constructor"); }
ParticleFlowEffect::~ParticleFlowEffect() { AURORA_TRACE("ParticleFlow", "Destructor"); cleanup(); }

Result<void> ParticleFlowEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("ParticleFlow", "initialize() maxParticles={}", maxParticles);
    if (glInitFn) glInitFn();
    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }
    m_segmentEnergy.assign(m_segmentCount, 0.0f);
    m_segmentRole.assign(m_segmentCount, SegmentRole::HarmonicMain);
    m_spawnAccum.assign(m_segmentCount, 0.0f);
    // 安全：粒子上限，防止 OOM
    m_maxParticles = std::min(maxParticles, 10000);
    // 每个粒子渲染 2 个顶点（核心 + 光晕）
    m_maxVerts = m_maxParticles * 2;
    m_vertices.reserve(m_maxVerts);
    m_particles.reserve(m_maxParticles);
    return Result<void>::Ok();
}

void ParticleFlowEffect::setSegmentEnergy(int segIdx, float emaValue) {
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentEnergy[segIdx] = std::clamp(emaValue, 0.0f, 1.0f);
    }
}

void ParticleFlowEffect::setSegmentRole(int segIdx, SegmentRole role) {
    if (segIdx >= 0 && segIdx < m_segmentCount) m_segmentRole[segIdx] = role;
}

void ParticleFlowEffect::setTime(float t) {
    // 计算 dt（首次调用时为 0）
    float dt = (m_prevTime > 0.0f) ? (t - m_prevTime) : 0.0f;
    // 安全：dt 钳制，防止暂停后大跳变导致粒子瞬移
    m_dt = std::clamp(dt, 0.0f, 0.1f);
    m_prevTime = t;
    m_time = t;
}

void ParticleFlowEffect::setAudioColor(const float rgb[3]) {
    m_audioColor[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    m_audioColor[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    m_audioColor[2] = std::clamp(rgb[2], 0.0f, 1.0f);
    m_musicColorEnabled = ParamStore::Instance().GetInt("concerto.musicColor") != 0;
}

unsigned ParticleFlowEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{}; glGetShaderInfoLog(s, 1023, nullptr, log);
        AURORA_ERROR("ParticleFlow", "{} shader: {}",
                     (type == GL_VERTEX_SHADER ? "VS" : "FS"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Result<void> ParticleFlowEffect::compileShaders() {
    AURORA_INFO("ParticleFlow", "compileShaders() begin");
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
        AURORA_ERROR("ParticleFlow", "Program link: {}", log);
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
                 m_maxVerts * sizeof(ParticleFlowVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    GLsizei stride = sizeof(ParticleFlowVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(ParticleFlowVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(ParticleFlowVertex, r)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(ParticleFlowVertex, size)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(ParticleFlowVertex, ageNorm)));
    glBindVertexArray(0);
    AURORA_INFO("ParticleFlow", "compileShaders() OK, maxParticles={}", m_maxParticles);
    return Result<void>::Ok();
}

void ParticleFlowEffect::cleanup() {
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_vao)     glDeleteVertexArrays(1, &m_vao);
        if (m_vbo)     glDeleteBuffers(1, &m_vbo);
        if (m_program) glDeleteProgram(m_program);
    }
    m_vao = m_vbo = m_program = 0;
    m_particles.clear();
    m_vertices.clear();
    m_vertCount = 0;
}

const char* ParticleFlowEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec4 aColor;
layout(location=2) in float aSize;
layout(location=3) in float aAgeNorm;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
out vec4 vColor;
out float vAgeNorm;
void main() {
    // 🔧 [Qt6 迁移修复] framebuffer X+Y 翻转
    float ndcX = 1.0 - aPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    gl_PointSize = aSize;
    vColor = aColor;
    vAgeNorm = aAgeNorm;
}
)glsl";
}

const char* ParticleFlowEffect::fragmentSource() {
    return R"glsl(#version 330 core
in vec4 vColor;
in float vAgeNorm;
uniform vec3 uAudioColor;
uniform int uMusicColorEnabled;
out vec4 fragColor;

vec3 reinhard(vec3 c) { return c / (1.0 + c); }

void main() {
    // 点精灵圆形剔除（GL_POINTS 默认是方形）
    vec2 coord = gl_PointCoord - vec2(0.5);
    float r2 = dot(coord, coord);
    if (r2 > 0.25) discard;  // 圆外透明

    vec3 baseColor = (uMusicColorEnabled == 1) ? uAudioColor : vColor.rgb;
    // 衰减：新生 1.0，临死 0.0
    float fade = 1.0 - vAgeNorm;
    // 软边缘
    float softEdge = 1.0 - smoothstep(0.15, 0.25, r2);
    vec3 color = baseColor * fade * softEdge;
    color = reinhard(color * 1.5);
    fragColor = vec4(color, vColor.a * fade);
}
)glsl";
}

void ParticleFlowEffect::updateParticles(float dt) {
    if (dt <= 0.0f) return;
    for (auto& p : m_particles) {
        p.perimPos += p.speed * dt;
        if (p.perimPos > 1.0f) p.perimPos -= 1.0f;  // 环绕
        p.age += dt;
    }
    // 移除过期粒子
    m_particles.erase(
        std::remove_if(m_particles.begin(), m_particles.end(),
                       [](const Particle& p) { return p.age >= p.life; }),
        m_particles.end());
}

void ParticleFlowEffect::spawnParticles(float dt) {
    if (dt <= 0.0f || !m_geometry) return;
    auto& ps = ParamStore::Instance();
    m_flowSpeed = static_cast<float>(ps.GetDouble("concerto.flowSpeed"));

    // 安全：粒子上限检查
    if (static_cast<int>(m_particles.size()) >= m_maxParticles) return;

    for (int segIdx = 0; segIdx < m_segmentCount; ++segIdx) {
        float energy = m_segmentEnergy[segIdx];
        float threshold = static_cast<float>(ps.GetDouble("concerto.threshold"));
        if (energy < threshold) continue;

        // spawn 速率 = energy * kParticleSpawnRate /s
        m_spawnAccum[segIdx] += energy * kParticleSpawnRate * dt;
        int spawnCount = static_cast<int>(m_spawnAccum[segIdx]);
        m_spawnAccum[segIdx] -= spawnCount;

        // 段角色色
        SegmentRole role = m_segmentRole[segIdx];
        float cr, cg, cb;
        switch (role) {
        case SegmentRole::HarmonicMain:    cr=1.0f; cg=0.78f; cb=0.0f; break;
        case SegmentRole::HarmonicAux:     cr=1.0f; cg=0.67f; cb=0.0f; break;
        case SegmentRole::HarmonicWeak:    cr=0.8f; cg=0.5f;  cb=0.0f; break;
        case SegmentRole::PercussiveMain:  cr=0.4f; cg=0.0f;  cb=1.0f; break;
        case SegmentRole::PercussiveAux:   cr=0.6f; cg=0.0f;  cb=0.8f; break;
        case SegmentRole::PercussiveWeak:  cr=0.3f; cg=0.0f;  cb=0.7f; break;
        default:                           cr=cg=cb=0.7f;    break;
        }

        // 粒子初始位置 = 段中点
        auto& segMgr = SegmentationManager::Instance();
        const auto& segs = segMgr.segments();
        if (segIdx >= static_cast<int>(segs.size())) continue;
        const auto& seg = segs[segIdx];
        float startPerim = static_cast<float>(seg.perimeterStart);
        float segLen    = static_cast<float>(seg.perimeterEnd - seg.perimeterStart);

        for (int i = 0; i < spawnCount; ++i) {
            if (static_cast<int>(m_particles.size()) >= m_maxParticles) return;
            Particle p;
            p.perimPos = startPerim + (static_cast<float>(i) / spawnCount) * segLen;
            p.speed = m_flowSpeed * 0.1f;
            p.age = 0.0f;
            p.life = 2.0f;
            p.r = cr; p.g = cg; p.b = cb;
            m_particles.push_back(p);
        }
    }
}

void ParticleFlowEffect::buildVertices() {
    if (!m_geometry || m_geometry->pointCount() < 3) {
        m_vertCount = 0;
        return;
    }
    m_vertices.clear();

    const auto& pts = m_geometry->points();
    size_t totalPts = pts.size();

    for (const auto& p : m_particles) {
        double t = std::clamp(static_cast<double>(p.perimPos), 0.0, 1.0);
        double idxFloat = t * (totalPts - 1);
        size_t i0 = static_cast<size_t>(std::floor(idxFloat));
        size_t i1 = std::min(i0 + 1, totalPts - 1);
        double frac = idxFloat - i0;
        float x = static_cast<float>(pts[i0].x + (pts[i1].x - pts[i0].x) * frac);
        float y = static_cast<float>(pts[i0].y + (pts[i1].y - pts[i0].y) * frac);
        float ageNorm = std::clamp(p.age / p.life, 0.0f, 1.0f);
        float fade = 1.0f - ageNorm;

        // ── 2 层发光（核心 + 光晕）──
        // 1. 核心 4px，alpha 高
        m_vertices.push_back({x, y, p.r, p.g, p.b, fade, 4.0f, ageNorm});
        // 2. 光晕 12px，alpha 减半
        m_vertices.push_back({x, y, p.r, p.g, p.b, fade * 0.4f, 12.0f, ageNorm});
    }
    m_vertCount = static_cast<int>(m_vertices.size());
}

void ParticleFlowEffect::render(const Camera& camera) {
    if (!m_program || !m_vao || !m_vbo) {
        static int warnCount = 0;
        if (++warnCount <= 5)
            AURORA_WARN("ParticleFlow", "render skip: prog={} vao={} vbo={}", m_program, m_vao, m_vbo);
        return;
    }

    // 更新粒子状态
    updateParticles(m_dt);
    spawnParticles(m_dt);
    buildVertices();

    if (m_vertCount == 0) {
        static int emptyCount = 0;
        if (++emptyCount <= 5)
            AURORA_INFO("ParticleFlow", "render skip: vertCount=0 particles={} dt={:.4f} geo={}",
                        m_particles.size(), m_dt, reinterpret_cast<uintptr_t>(m_geometry));
        return;
    }
    if (m_vertCount > m_maxVerts) {
        AURORA_WARN("ParticleFlow", "vertCount {} > max {}, clamping",
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
                    m_vertCount * sizeof(ParticleFlowVertex),
                    m_vertices.data());
    glBindVertexArray(m_vao);

    // 安全：启用 PROGRAM_POINT_SIZE 让 gl_PointSize 生效
    GLboolean prevPointSizeEnabled = glIsEnabled(GL_PROGRAM_POINT_SIZE);
    if (!prevPointSizeEnabled) glEnable(GL_PROGRAM_POINT_SIZE);

    glDrawArrays(GL_POINTS, 0, m_vertCount);

    if (!prevPointSizeEnabled) glDisable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(0);
}
