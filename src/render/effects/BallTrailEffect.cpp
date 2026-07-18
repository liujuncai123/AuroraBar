/**
 * @file BallTrailEffect.cpp
 * @brief 弹球+彩虹拖尾效果实现
 * @date 2026-07-07
 */
#include "BallTrailEffect.h"
#include "../BorderGeometry.h"
#include "../../logging/LoggerManager.h"
#include <GL/glew.h>
#include <GL/wglew.h>
#include <algorithm>
#include <cmath>
#include <cstring>

BallTrailEffect::BallTrailEffect() { AURORA_TRACE("BallTrailEffect", "Constructor"); }
BallTrailEffect::~BallTrailEffect() { AURORA_TRACE("BallTrailEffect", "Destructor"); cleanup(); }

Result<void> BallTrailEffect::initialize(std::function<void()> glInitFn, int maxTrailPoints) {
    AURORA_INFO("BallTrailEffect", "initialize() maxTrailPoints={}", maxTrailPoints);
    if (maxTrailPoints < 50)
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "maxTrailPoints must be >= 50"));
    m_trailCapacity = maxTrailPoints;
    if (glInitFn) glInitFn();
    return Result<void>::Ok();
}

void BallTrailEffect::update(float /*dt*/) {
    // 球位置由外部 setBallState 更新，拖尾由 setTrailData 更新
}

void BallTrailEffect::render(const Camera& camera) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 1. 渲染拖尾
    if (m_trailCount > 0 && m_trailProgram && m_trailVAO) {
        glUseProgram(m_trailProgram);
        const auto& mat = camera.matrix();
        if (std::memcmp(mat.data(), m_cachedTrailProj.data(), 16 * sizeof(float)) != 0) {
            glUniformMatrix4fv(m_locTrailProjection, 1, GL_FALSE, mat.data());
            m_cachedTrailProj = mat;
        }
        float sw = static_cast<float>(m_screenW), sh = static_cast<float>(m_screenH);
        if (sw != m_cachedTrailSW) { glUniform1f(m_locTrailScreenW, sw); m_cachedTrailSW = sw; }
        if (sh != m_cachedTrailSH) { glUniform1f(m_locTrailScreenH, sh); m_cachedTrailSH = sh; }

        glBindVertexArray(m_trailVAO);
        glDrawArrays(GL_POINTS, 0, m_trailCount);
    }

    // 2. 渲染球
    if (m_ballProgram && m_ballVAO) {
        glUseProgram(m_ballProgram);
        const auto& mat = camera.matrix();
        if (std::memcmp(mat.data(), m_cachedBallProj.data(), 16 * sizeof(float)) != 0) {
            glUniformMatrix4fv(m_locBallProjection, 1, GL_FALSE, mat.data());
            m_cachedBallProj = mat;
        }
        glUniform2f(m_locBallScreenPos, m_ballScreenX, m_ballScreenY);
        glUniform1f(m_locBallRadius, m_ballRadius);
        glUniform1f(m_locPulseRadius, m_pulseRadius);
        float sw = static_cast<float>(m_screenW), sh = static_cast<float>(m_screenH);
        if (sw != m_cachedBallSW) { glUniform1f(m_locBallScreenW, sw); m_cachedBallSW = sw; }
        if (sh != m_cachedBallSH) { glUniform1f(m_locBallScreenH, sh); m_cachedBallSH = sh; }

        glBindVertexArray(m_ballVAO);
        glDrawArrays(GL_POINTS, 0, 1);
    }

    glDisable(GL_BLEND);
}

void BallTrailEffect::cleanup() {
    // 安全：上下文丢失时 wglGetCurrentContext() 返回 NULL，
    //       此时 glDelete* 会导致 0xc0000005 访问违规崩溃。
    //       驱动在上下文丢失时已自动回收 GL 对象，仅需清零 ID 防止误用。
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_ballVAO) { glDeleteVertexArrays(1, &m_ballVAO); }
        if (m_ballVBO) { glDeleteBuffers(1, &m_ballVBO); }
        if (m_ballProgram) { glDeleteProgram(m_ballProgram); }
        if (m_trailVAO) { glDeleteVertexArrays(1, &m_trailVAO); }
        if (m_trailVBO) { glDeleteBuffers(1, &m_trailVBO); }
        if (m_trailProgram) { glDeleteProgram(m_trailProgram); }
    }
    m_ballVAO = 0; m_ballVBO = 0; m_ballProgram = 0;
    m_trailVAO = 0; m_trailVBO = 0; m_trailProgram = 0;
    m_trailCount = 0;
}

Result<void> BallTrailEffect::compileShaders() {
    // === 球着色器 ===
    unsigned bvs = compileShader(GL_VERTEX_SHADER, ballVertexSource());
    unsigned bfs = compileShader(GL_FRAGMENT_SHADER, ballFragmentSource());
    if (!bvs || !bfs) return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, "Ball shader compile failed"));

    m_ballProgram = glCreateProgram();
    glAttachShader(m_ballProgram, bvs); glAttachShader(m_ballProgram, bfs);
    glLinkProgram(m_ballProgram);
    int linked = 0; glGetProgramiv(m_ballProgram, GL_LINK_STATUS, &linked);
    glDeleteShader(bvs); glDeleteShader(bfs);
    if (!linked) {
        char log[512]{}; glGetProgramInfoLog(m_ballProgram, 511, nullptr, log);
        AURORA_ERROR("BallTrailEffect", "Ball link: {}", log);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }

    m_locBallProjection = glGetUniformLocation(m_ballProgram, "uProjection");
    m_locBallScreenPos  = glGetUniformLocation(m_ballProgram, "uBallScreenPos");
    m_locBallRadius     = glGetUniformLocation(m_ballProgram, "uBallRadius");
    m_locPulseRadius    = glGetUniformLocation(m_ballProgram, "uPulseRadius");
    m_locBallScreenW    = glGetUniformLocation(m_ballProgram, "uScreenW");
    m_locBallScreenH    = glGetUniformLocation(m_ballProgram, "uScreenH");

    glGenVertexArrays(1, &m_ballVAO); glGenBuffers(1, &m_ballVBO);
    glBindVertexArray(m_ballVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_ballVBO);
    float dummy = 0.0f;
    glBufferData(GL_ARRAY_BUFFER, sizeof(float), &dummy, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    AURORA_INFO("BallTrailEffect", "Ball shader OK: proj={} pos={} radius={} pulse={} sw={} sh={}",
                m_locBallProjection, m_locBallScreenPos, m_locBallRadius,
                m_locPulseRadius, m_locBallScreenW, m_locBallScreenH);

    // === 拖尾着色器 ===
    unsigned tvs = compileShader(GL_VERTEX_SHADER, trailVertexSource());
    unsigned tfs = compileShader(GL_FRAGMENT_SHADER, trailFragmentSource());
    if (!tvs || !tfs) return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, "Trail shader compile failed"));

    m_trailProgram = glCreateProgram();
    glAttachShader(m_trailProgram, tvs); glAttachShader(m_trailProgram, tfs);
    glLinkProgram(m_trailProgram);
    glGetProgramiv(m_trailProgram, GL_LINK_STATUS, &linked);
    glDeleteShader(tvs); glDeleteShader(tfs);
    if (!linked) {
        char log[512]{}; glGetProgramInfoLog(m_trailProgram, 511, nullptr, log);
        AURORA_ERROR("BallTrailEffect", "Trail link: {}", log);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }

    m_locTrailProjection = glGetUniformLocation(m_trailProgram, "uProjection");
    m_locTrailScreenW    = glGetUniformLocation(m_trailProgram, "uScreenW");
    m_locTrailScreenH    = glGetUniformLocation(m_trailProgram, "uScreenH");

    glGenVertexArrays(1, &m_trailVAO); glGenBuffers(1, &m_trailVBO);
    glBindVertexArray(m_trailVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_trailVBO);
    glBufferData(GL_ARRAY_BUFFER, m_trailCapacity * sizeof(TrailVertex), nullptr, GL_DYNAMIC_DRAW);
    auto stride = static_cast<GLsizei>(sizeof(TrailVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(TrailVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(TrailVertex, r));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(TrailVertex, alpha));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(TrailVertex, size));
    glBindVertexArray(0);

    AURORA_INFO("BallTrailEffect", "Trail shader OK: proj={} sw={} sh={} capacity={}",
                m_locTrailProjection, m_locTrailScreenW, m_locTrailScreenH, m_trailCapacity);

    glEnable(GL_PROGRAM_POINT_SIZE);
    return Result<void>::Ok();
}

void BallTrailEffect::computeBallScreenPos(const BorderGeometry& geo,
                                           double perimeterPos, double normalDist) {
    computeTrailScreenPos(geo, perimeterPos, normalDist, m_ballScreenX, m_ballScreenY);
}

void BallTrailEffect::computeTrailScreenPos(const BorderGeometry& geo,
                                            double perimeterPos, double normalDist,
                                            float& outX, float& outY) {
    const auto& pts = geo.points();
    size_t totalPts = pts.size();
    if (totalPts < 3) { outX = 0; outY = 0; return; }

    double t = std::clamp(perimeterPos, 0.0, 1.0);
    double idxFloat = t * (totalPts - 1);
    size_t i0 = static_cast<size_t>(std::floor(idxFloat));
    size_t i1 = std::min(i0 + 1, totalPts - 1);
    double frac = idxFloat - i0;

    // 插值边框基准点
    double bx = pts[i0].x + (pts[i1].x - pts[i0].x) * frac;
    double by = pts[i0].y + (pts[i1].y - pts[i0].y) * frac;

    // 使用预计算的平滑法线（角区已插值）
    float nx = static_cast<float>(pts[i0].nx + (pts[i1].nx - pts[i0].nx) * frac);
    float ny = static_cast<float>(pts[i0].ny + (pts[i1].ny - pts[i0].ny) * frac);

    outX = static_cast<float>(bx + nx * normalDist);
    outY = static_cast<float>(by + ny * normalDist);
}

void BallTrailEffect::setBallState(float screenX, float screenY, float radius, float pulseRadius) {
    m_ballScreenX = screenX;
    m_ballScreenY = screenY;
    m_ballRadius = radius;
    m_pulseRadius = std::clamp(pulseRadius, 0.0f, 1.0f);
}

void BallTrailEffect::setTrailData(const TrailVertex* vertices, int count) {
    // 安全：GL 入口点必须检查上下文有效性（project_memory 约定）
    //   上下文丢失时 glBufferSubData 静默失败，提前返回避免无效状态
    if (!wglGetCurrentContext()) { m_trailCount = 0; return; }
    if (!m_trailVBO || count <= 0) { m_trailCount = 0; return; }
    m_trailCount = std::min(count, m_trailCapacity);
    glBindBuffer(GL_ARRAY_BUFFER, m_trailVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, m_trailCount * sizeof(TrailVertex), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

unsigned BallTrailEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]{}; glGetShaderInfoLog(s, 511, nullptr, log);
        AURORA_ERROR("BallTrailEffect", "{} shader: {}",
                     (type == GL_VERTEX_SHADER ? "Vertex" : "Fragment"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void BallTrailEffect::hslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    auto hue2rgb = [](float p, float q, float t) {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f / 2.0f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };

    if (s < 0.001f) { r = g = b = l; return; }
    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    r = hue2rgb(p, q, h + 1.0f / 3.0f);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.0f / 3.0f);
}

// ============================================================
// 着色器源码
// ============================================================

const char* BallTrailEffect::ballVertexSource() {
    return R"glsl(#version 330 core
uniform vec2 uBallScreenPos;
uniform float uBallRadius;
uniform float uScreenW;
uniform float uScreenH;
uniform mat4 uProjection;
void main() {
    vec2 screenPos = uBallScreenPos;
    float ndcX = screenPos.x / uScreenW * 2.0 - 1.0;
    float ndcY = 1.0 - screenPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    gl_PointSize = uBallRadius * 6.0;
}
)glsl";
}

const char* BallTrailEffect::ballFragmentSource() {
    return R"glsl(#version 330 core
uniform float uPulseRadius;
uniform float uTime;
out vec4 fragColor;

// ACES 色调映射
vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    if (length(gl_PointCoord - vec2(0.5)) > 0.5) discard;
    float dist = length(gl_PointCoord - vec2(0.5)) * 2.0;

    // 球心：白色实心圆（实体化：完全不透明核心）
    float core = 1.0 - smoothstep(0.0, 0.28, dist);

    // 脉冲环：位置随 audioDelta 在 0.4~0.7 之间变化
    float ringPos = 0.4 + uPulseRadius * 0.3;
    float ring = exp(-((dist - ringPos) * (dist - ringPos)) / 0.015);
    ring *= smoothstep(0.2, 0.85, dist);

    // 外发光（实体化：更强）
    float glow = exp(-dist * 2.5) * 0.35;

    // 颜色：中心白 → 环 cyan → 外围透明
    vec3 color = mix(vec3(1.0, 1.0, 1.0), vec3(0.2, 1.0, 0.8), ring * 0.8);
    float alpha = core * 1.0 + ring * 0.9 + glow;

    color = aces(color);
    fragColor = vec4(color, alpha);
}
)glsl";
}

const char* BallTrailEffect::trailVertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aScreenPos;
layout(location=1) in vec3 aColor;
layout(location=2) in float aAlpha;
layout(location=3) in float aSize;
uniform float uScreenW;
uniform float uScreenH;
uniform mat4 uProjection;
out vec3 vColor;
out float vAlpha;
void main() {
    float ndcX = aScreenPos.x / uScreenW * 2.0 - 1.0;
    float ndcY = 1.0 - aScreenPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    gl_PointSize = aSize;
    vColor = aColor;
    vAlpha = aAlpha;
}
)glsl";
}

const char* BallTrailEffect::trailFragmentSource() {
    return R"glsl(#version 330 core
in vec3 vColor;
in float vAlpha;
out vec4 fragColor;

// ACES 色调映射
vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    if (length(gl_PointCoord - vec2(0.5)) > 0.5) discard;
    float dist = length(gl_PointCoord - vec2(0.5));
    // 实体化：更锐利的边缘 + 最低不透明度底线
    float alpha = 1.0 - smoothstep(0.0, 0.4, dist);
    alpha = max(0.25, alpha);
    alpha *= vAlpha;
    float glow = max(0.0, 1.0 - dist * 2.0);
    vec3 c = vColor * (0.4 + glow * 0.6);
    c = aces(c);
    fragColor = vec4(c, alpha);
}
)glsl";
}