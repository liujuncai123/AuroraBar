/**
 * @file SpectrumBarEffect.cpp
 * @brief 协奏子模式 1：极简频谱柱实现
 * @date 2026-07-18
 * @details Apple Music 风格：极细矩形条 + 3 层发光 + 残影回落。
 * @note 安全：段索引越界检查；GL 资源在 cleanup 中检查 wglGetCurrentContext 后释放。
 */
#include "SpectrumBarEffect.h"
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

SpectrumBarEffect::SpectrumBarEffect() { AURORA_TRACE("Spectrum", "Constructor"); }
SpectrumBarEffect::~SpectrumBarEffect() { AURORA_TRACE("Spectrum", "Destructor"); cleanup(); }

Result<void> SpectrumBarEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("Spectrum", "initialize() maxParticles={}", maxParticles);
    if (glInitFn) glInitFn();

    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }
    m_segmentEnergy.assign(m_segmentCount, 0.0f);
    m_segmentRole.assign(m_segmentCount, SegmentRole::HarmonicMain);
    m_peakHistory.assign(m_segmentCount, 0.0f);
    m_columnsPerSegment = std::max(3, maxParticles / m_segmentCount);
    // 每柱 12 顶点（6 主体 + 6 残影），预留充足 buffer
    m_maxVerts = m_segmentCount * m_columnsPerSegment * 12;
    m_vertices.reserve(m_maxVerts);
    return Result<void>::Ok();
}

void SpectrumBarEffect::setSegmentEnergy(int segIdx, float emaValue) {
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentEnergy[segIdx] = std::clamp(emaValue, 0.0f, 1.0f);
    }
}

void SpectrumBarEffect::setSegmentRole(int segIdx, SegmentRole role) {
    if (segIdx >= 0 && segIdx < m_segmentCount) m_segmentRole[segIdx] = role;
}

void SpectrumBarEffect::setAudioColor(const float rgb[3]) {
    m_audioColor[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    m_audioColor[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    m_audioColor[2] = std::clamp(rgb[2], 0.0f, 1.0f);
    m_musicColorEnabled = ParamStore::Instance().GetInt("concerto.musicColor") != 0;
}

unsigned SpectrumBarEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{}; glGetShaderInfoLog(s, 1023, nullptr, log);
        AURORA_ERROR("Spectrum", "{} shader: {}",
                     (type == GL_VERTEX_SHADER ? "VS" : "FS"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Result<void> SpectrumBarEffect::compileShaders() {
    AURORA_INFO("Spectrum", "compileShaders() begin");
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
        AURORA_ERROR("Spectrum", "Program link: {}", log);
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
                 m_maxVerts * sizeof(SpectrumBarVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    GLsizei stride = sizeof(SpectrumBarVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(SpectrumBarVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(SpectrumBarVertex, localY)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(SpectrumBarVertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(SpectrumBarVertex, alpha)));
    glBindVertexArray(0);
    AURORA_INFO("Spectrum", "compileShaders() OK, maxVerts={}", m_maxVerts);
    return Result<void>::Ok();
}

void SpectrumBarEffect::cleanup() {
    // 安全：上下文已死时禁止 glDelete*
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

// ============================================================
// shader：3 层发光 + Reinhard + Qt6 X/Y 翻转
// ============================================================

const char* SpectrumBarEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in float aLocalY;
layout(location=2) in vec3 aColor;
layout(location=3) in float aAlpha;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
out float vLocalY;
out vec3 vColor;
out float vAlpha;
void main() {
    // 🔧 [Qt6 迁移修复] framebuffer 是 bottom-right origin，X/Y 同时翻转
    float ndcX = 1.0 - aPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    vLocalY = aLocalY;
    vColor = aColor;
    vAlpha = aAlpha;
}
)glsl";
}

const char* SpectrumBarEffect::fragmentSource() {
    return R"glsl(#version 330 core
in float vLocalY;
in vec3 vColor;
in float vAlpha;
uniform vec3 uAudioColor;
uniform int uMusicColorEnabled;
out vec4 fragColor;

vec3 reinhard(vec3 c) { return c / (1.0 + c); }

void main() {
    vec3 baseColor = (uMusicColorEnabled == 1) ? uAudioColor : vColor;
    // ── 3 层发光 ──
    // 1. 主体渐变：底暗顶亮
    vec3 layer1 = baseColor * (0.3 + 0.7 * vLocalY);
    // 2. 顶部 1px 高亮（vLocalY 接近 1 时）
    float topHL = smoothstep(0.95, 1.0, vLocalY);
    vec3 layer2 = vec3(1.0) * topHL * 0.8;
    // 3. 底部反射阴影
    float bottomRef = (1.0 - vLocalY) * 0.3;
    vec3 layer3 = baseColor * bottomRef * 0.5;

    vec3 color = layer1 + layer2 + layer3;
    color = reinhard(color * 1.5);
    fragColor = vec4(color, vAlpha);
}
)glsl";
}

// ============================================================
// 顶点生成：细矩形柱 + 残影线
// ============================================================

void SpectrumBarEffect::generateVertices() {
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
    double screenPerim = 2.0 * (scrW + scrH);

    auto& ps = ParamStore::Instance();
    float threshold   = static_cast<float>(ps.GetDouble("concerto.threshold"));
    float alphaBase   = static_cast<float>(ps.GetDouble("concerto.alphaBase"));
    m_columnsPerSegment = ps.GetInt("concerto.columnsPerSeg");

    int segsPerEdge = static_cast<int>(segs.size()) / 4;
    if (segsPerEdge < 1) segsPerEdge = 4;

    // 残影更新：peakHistory 衰减 + 取 max
    // 安全：假设帧间隔 16ms（60fps），decay=0.2 对应 200ms 衰减一帧
    constexpr float kDecayPerFrame = 0.2f * 0.016f;
    for (int i = 0; i < m_segmentCount; ++i) {
        m_peakHistory[i] = std::max(m_segmentEnergy[i],
                                    m_peakHistory[i] - kDecayPerFrame);
    }

    for (int segIdx = 0; segIdx < m_segmentCount && segIdx < static_cast<int>(segs.size()); ++segIdx) {
        const auto& seg = segs[segIdx];
        float zPulse = m_segmentEnergy[segIdx];
        if (zPulse < threshold) continue;

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

        double segPixelLen = segLen * screenPerim;
        float nominalWidth = static_cast<float>(segPixelLen / m_columnsPerSegment);
        float columnWidth  = nominalWidth * 1.2f;
        int cornerZone = std::min(m_geometry->cornerTransition(),
                                  static_cast<int>(nominalWidth * 0.3));

        for (int c = 0; c < m_columnsPerSegment; ++c) {
            double t = segStart + segLen * (c + 0.5) / m_columnsPerSegment;
            t = std::clamp(t, 0.0, 1.0);
            double idxFloat = t * (totalPts - 1);
            size_t i0 = static_cast<size_t>(std::floor(idxFloat));
            size_t i1 = std::min(i0 + 1, totalPts - 1);
            double frac = idxFloat - i0;

            double bx = pts[i0].x + (pts[i1].x - pts[i0].x) * frac;
            double by = pts[i0].y + (pts[i1].y - pts[i0].y) * frac;
            double bw = pts[i0].width + (pts[i1].width - pts[i0].width) * frac;

            if (edgeIdx == 0 || edgeIdx == 2) {
                if (bx < cornerZone || bx > scrW - cornerZone) continue;
            }

            float colRatio = static_cast<float>(c) / std::max(1, m_columnsPerSegment - 1);
            float subDist = 0.5f - 0.5f * std::cos(colRatio * 3.141592653589793f * 0.85f);
            float heightFactor = 0.12f + subDist * 0.88f;
            float colZ = zPulse * heightFactor;
            colZ = std::max(0.0f, colZ);
            float rawH = static_cast<float>(bw) * colZ;
            constexpr float kMinH = 3.0f;
            float height = rawH < kMinH
                ? rawH + (kMinH - rawH) * (1.0f - std::exp(-rawH / kMinH))
                : rawH;

            float peakH = m_peakHistory[segIdx] * height;  // 残影高度

            float nx = static_cast<float>(pts[i0].nx + (pts[i1].nx - pts[i0].nx) * frac);
            float ny = static_cast<float>(pts[i0].ny + (pts[i1].ny - pts[i0].ny) * frac);
            float baseX = static_cast<float>(bx);
            float baseY = static_cast<float>(by);
            float tipX = baseX + nx * height;
            float tipY = baseY + ny * height;
            float peakX = baseX + nx * peakH;
            float peakY = baseY + ny * peakH;

            float hw = columnWidth * 0.5f;
            float px = -ny * hw, py = nx * hw;

            // 颜色：青 #00E0FF + 高能量偏白
            float whiteMix = std::min(1.0f, colZ * 1.5f);
            float cr = (0.0f  * (1.0f - whiteMix)) + (1.0f * whiteMix);
            float cg = (0.88f * (1.0f - whiteMix)) + (1.0f * whiteMix);
            float cb = 1.0f;
            float alpha = alphaBase + colZ * (1.0f - alphaBase);

            // 主体矩形（2 三角形 = 6 顶点）
            m_vertices.push_back({baseX + px, baseY + py, 0.0f, cr, cg, cb, alpha});
            m_vertices.push_back({baseX - px, baseY - py, 0.0f, cr, cg, cb, alpha});
            m_vertices.push_back({tipX  + px, tipY  + py, 1.0f, cr, cg, cb, alpha});
            m_vertices.push_back({baseX - px, baseY - py, 0.0f, cr, cg, cb, alpha});
            m_vertices.push_back({tipX  - px, tipY  - py, 1.0f, cr, cg, cb, alpha});
            m_vertices.push_back({tipX  + px, tipY  + py, 1.0f, cr, cg, cb, alpha});

            // 残影 1px 高亮线（peak 位置，alpha 减半）
            m_vertices.push_back({peakX + px, peakY + py, 0.5f,  1.0f, 1.0f, 1.0f, alpha * 0.5f});
            m_vertices.push_back({peakX - px, peakY - py, 0.5f,  1.0f, 1.0f, 1.0f, alpha * 0.5f});
            m_vertices.push_back({peakX + px, peakY + py, 0.55f, 1.0f, 1.0f, 1.0f, alpha * 0.5f});
            m_vertices.push_back({peakX - px, peakY - py, 0.5f,  1.0f, 1.0f, 1.0f, alpha * 0.5f});
            m_vertices.push_back({peakX - px, peakY - py, 0.55f, 1.0f, 1.0f, 1.0f, alpha * 0.5f});
            m_vertices.push_back({peakX + px, peakY + py, 0.55f, 1.0f, 1.0f, 1.0f, alpha * 0.5f});
        }
    }
    m_vertCount = static_cast<int>(m_vertices.size());
}

void SpectrumBarEffect::render(const Camera& camera) {
    if (!m_program || !m_vao || !m_vbo) return;
    generateVertices();
    if (m_vertCount == 0) return;
    // 安全：顶点数钳制
    if (m_vertCount > m_maxVerts) {
        AURORA_WARN("Spectrum", "vertCount {} > max {}, clamping",
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
    if (m_locAudioColor >= 0) {
        glUniform3fv(m_locAudioColor, 1, m_audioColor);
    }
    if (m_locMusicColorEnabled >= 0) {
        glUniform1i(m_locMusicColorEnabled, m_musicColorEnabled ? 1 : 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    m_vertCount * sizeof(SpectrumBarVertex),
                    m_vertices.data());
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, m_vertCount);
    glBindVertexArray(0);
}
