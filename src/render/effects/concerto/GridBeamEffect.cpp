/**
 * @file GridBeamEffect.cpp
 * @brief 协奏子模式 4：网格光带实现
 * @date 2026-07-18
 */
#include "GridBeamEffect.h"
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

GridBeamEffect::GridBeamEffect() { AURORA_TRACE("GridBeam", "Constructor"); }
GridBeamEffect::~GridBeamEffect() { AURORA_TRACE("GridBeam", "Destructor"); cleanup(); }

Result<void> GridBeamEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("GridBeam", "initialize() maxParticles={}", maxParticles);
    if (glInitFn) glInitFn();
    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }
    m_segmentEnergy.assign(m_segmentCount, 0.0f);
    m_segmentRole.assign(m_segmentCount, SegmentRole::HarmonicMain);
    // 每段：5 网格线 × 2 顶点 + 2 边界 × 2 顶点 = 28 顶点（lines）
    // 每段：32 采样 × 2 顶点 = 64 顶点（strip）
    m_maxLineVerts  = m_segmentCount * 28;
    m_maxStripVerts = m_segmentCount * 64;
    m_lineVertices.reserve(m_maxLineVerts);
    m_stripVertices.reserve(m_maxStripVerts);
    return Result<void>::Ok();
}

void GridBeamEffect::setSegmentEnergy(int segIdx, float emaValue) {
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentEnergy[segIdx] = std::clamp(emaValue, 0.0f, 1.0f);
    }
}

void GridBeamEffect::setSegmentRole(int segIdx, SegmentRole role) {
    if (segIdx >= 0 && segIdx < m_segmentCount) m_segmentRole[segIdx] = role;
}

void GridBeamEffect::setAudioColor(const float rgb[3]) {
    m_audioColor[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    m_audioColor[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    m_audioColor[2] = std::clamp(rgb[2], 0.0f, 1.0f);
    m_musicColorEnabled = ParamStore::Instance().GetInt("concerto.musicColor") != 0;
}

unsigned GridBeamEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{}; glGetShaderInfoLog(s, 1023, nullptr, log);
        AURORA_ERROR("GridBeam", "{} shader: {}",
                     (type == GL_VERTEX_SHADER ? "VS" : "FS"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Result<void> GridBeamEffect::compileShaders() {
    AURORA_INFO("GridBeam", "compileShaders() begin");
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
        AURORA_ERROR("GridBeam", "Program link: {}", log);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }

    m_locProjection       = glGetUniformLocation(m_program, "uProjection");
    m_locScreenW         = glGetUniformLocation(m_program, "uScreenW");
    m_locScreenH         = glGetUniformLocation(m_program, "uScreenH");
    m_locTime            = glGetUniformLocation(m_program, "uTime");
    m_locAudioColor      = glGetUniformLocation(m_program, "uAudioColor");
    m_locMusicColorEnabled = glGetUniformLocation(m_program, "uMusicColorEnabled");

    // 单 VAO/VBO 容纳 lines + strip（运行时按 max(line,strip) 分配）
    int maxVerts = std::max(m_maxLineVerts, m_maxStripVerts);
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 maxVerts * sizeof(GridVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    GLsizei stride = sizeof(GridVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GridVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GridVertex, r)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GridVertex, alpha)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GridVertex, localU)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(GridVertex, localV)));
    glBindVertexArray(0);
    AURORA_INFO("GridBeam", "compileShaders() OK, maxLine={} maxStrip={}",
                m_maxLineVerts, m_maxStripVerts);
    return Result<void>::Ok();
}

void GridBeamEffect::cleanup() {
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_vao)     glDeleteVertexArrays(1, &m_vao);
        if (m_vbo)     glDeleteBuffers(1, &m_vbo);
        if (m_program) glDeleteProgram(m_program);
    }
    m_vao = m_vbo = m_program = 0;
    m_lineVertices.clear();
    m_stripVertices.clear();
    m_lineVertCount = m_stripVertCount = 0;
}

const char* GridBeamEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in float aAlpha;
layout(location=3) in float aLocalU;
layout(location=4) in float aLocalV;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
out vec3 vColor;
out float vAlpha;
out float vLocalU;
out float vLocalV;
void main() {
    // 🔧 [Qt6 迁移修复] framebuffer X+Y 翻转
    float ndcX = 1.0 - aPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    vColor = aColor;
    vAlpha = aAlpha;
    vLocalU = aLocalU;
    vLocalV = aLocalV;
}
)glsl";
}

const char* GridBeamEffect::fragmentSource() {
    return R"glsl(#version 330 core
in vec3 vColor;
in float vAlpha;
in float vLocalU;
in float vLocalV;
uniform float uTime;
uniform vec3 uAudioColor;
uniform int uMusicColorEnabled;
out vec4 fragColor;

vec3 reinhard(vec3 c) { return c / (1.0 + c); }

void main() {
    vec3 baseColor = (uMusicColorEnabled == 1) ? uAudioColor : vColor;
    // 3 层发光：
    // 1. 主体基色
    vec3 layer1 = baseColor;
    // 2. 沿光带方向中部增亮（localV 在中间最亮）
    float midHL = 1.0 - abs(vLocalV - 0.5) * 2.0;
    vec3 layer2 = vec3(1.0) * midHL * 0.3;
    // 3. 沿光带流动的扫描高光
    float scan = smoothstep(0.45, 0.5, fract(vLocalU * 4.0 - uTime * 0.5)) *
                 smoothstep(0.55, 0.5, fract(vLocalU * 4.0 - uTime * 0.5));
    vec3 layer3 = vec3(1.0) * scan * 0.5;

    vec3 color = layer1 + layer2 + layer3;
    color = reinhard(color * 1.5);
    fragColor = vec4(color, vAlpha);
}
)glsl";
}

void GridBeamEffect::generateVertices() {
    if (!m_geometry || m_geometry->pointCount() < 3) {
        m_lineVertCount = 0;
        m_stripVertCount = 0;
        return;
    }
    m_lineVertices.clear();
    m_stripVertices.clear();

    auto& segMgr = SegmentationManager::Instance();
    const auto& segs = segMgr.segments();
    const auto& pts = m_geometry->points();
    size_t totalPts = pts.size();
    int scrW = m_geometry->screenW();
    int scrH = m_geometry->screenH();
    double screenPerim = 2.0 * (scrW + scrH);

    auto& ps = ParamStore::Instance();
    float threshold = static_cast<float>(ps.GetDouble("concerto.threshold"));
    float alphaBase = static_cast<float>(ps.GetDouble("concerto.alphaBase"));

    // 颜色：网格深青 #00384A，光带亮青 #00E0FF，边界白
    constexpr float kGridR = 0.0f,   kGridG = 0.22f, kGridB = 0.29f;
    constexpr float kBeamR = 0.0f,   kBeamG = 0.88f, kBeamB = 1.0f;
    constexpr float kEdgeR = 1.0f,   kEdgeG = 1.0f,  kEdgeB = 1.0f;

    int segsPerEdge = static_cast<int>(segs.size()) / 4;
    if (segsPerEdge < 1) segsPerEdge = 4;

    constexpr int kGridLinesPerSeg = 5;
    constexpr int kStripSamples = 32;
    constexpr float kStripHalfWidth = 4.0f;  // 光带半宽 4px

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

        double segPixelLen = segLen * screenPerim;
        (void)segPixelLen;  // 预留给未来光带宽度计算
        float alpha = alphaBase + energy * (1.0f - alphaBase);

        // ── 1. 5 条等距网格线（GL_LINES）──
        for (int i = 0; i < kGridLinesPerSeg; ++i) {
            float t = static_cast<float>(i + 1) / (kGridLinesPerSeg + 1);
            double posT = segStart + segLen * t;
            posT = std::clamp(posT, 0.0, 1.0);
            double idxFloat = posT * (totalPts - 1);
            size_t i0 = static_cast<size_t>(std::floor(idxFloat));
            size_t i1 = std::min(i0 + 1, totalPts - 1);
            double frac = idxFloat - i0;
            float x = static_cast<float>(pts[i0].x + (pts[i1].x - pts[i0].x) * frac);
            float y = static_cast<float>(pts[i0].y + (pts[i1].y - pts[i0].y) * frac);
            float nx = static_cast<float>(pts[i0].nx + (pts[i1].nx - pts[i0].nx) * frac);
            float ny = static_cast<float>(pts[i0].ny + (pts[i1].ny - pts[i0].ny) * frac);

            // 网格线短：从边框向内延伸 8px
            float len = 8.0f + energy * 6.0f;
            float x1 = x + nx * len;
            float y1 = y + ny * len;
            m_lineVertices.push_back({x,  y,  kGridR, kGridG, kGridB, alpha * 0.5f, t, 0.0f});
            m_lineVertices.push_back({x1, y1, kGridR, kGridG, kGridB, alpha * 0.5f, t, 1.0f});
        }

        // ── 2. 段边界 2 条亮线（GL_LINES，沿边框方向短小）──
        for (int b = 0; b < 2; ++b) {
            double posT = (b == 0) ? segStart : segEnd;
            posT = std::clamp(posT, 0.0, 1.0);
            double idxFloat = posT * (totalPts - 1);
            size_t i0 = static_cast<size_t>(std::floor(idxFloat));
            size_t i1 = std::min(i0 + 1, totalPts - 1);
            double frac = idxFloat - i0;
            float x = static_cast<float>(pts[i0].x + (pts[i1].x - pts[i0].x) * frac);
            float y = static_cast<float>(pts[i0].y + (pts[i1].y - pts[i0].y) * frac);
            float nx = static_cast<float>(pts[i0].nx + (pts[i1].nx - pts[i0].nx) * frac);
            float ny = static_cast<float>(pts[i0].ny + (pts[i1].ny - pts[i0].ny) * frac);

            float len = 12.0f + energy * 10.0f;
            float x1 = x + nx * len;
            float y1 = y + ny * len;
            // 边界高能爆亮：alpha 随 energy
            float edgeAlpha = alpha + energy * 0.5f;
            m_lineVertices.push_back({x,  y,  kEdgeR, kEdgeG, kEdgeB, edgeAlpha, 0.0f, 0.0f});
            m_lineVertices.push_back({x1, y1, kEdgeR, kEdgeG, kEdgeB, edgeAlpha, 1.0f, 1.0f});
        }

        // ── 3. 段中央光带（GL_TRIANGLE_STRIP）──
        // 光带长度 = zPulse * 段长度 * 0.8
        double beamLen = segLen * 0.8f * energy;
        double beamStart = segStart + (segLen - beamLen) * 0.5;
        double beamEnd = beamStart + beamLen;
        if (beamEnd <= beamStart) continue;

        for (int i = 0; i < kStripSamples; ++i) {
            float tu = static_cast<float>(i) / (kStripSamples - 1);
            double posT = beamStart + (beamEnd - beamStart) * tu;
            posT = std::clamp(posT, 0.0, 1.0);
            double idxFloat = posT * (totalPts - 1);
            size_t i0 = static_cast<size_t>(std::floor(idxFloat));
            size_t i1 = std::min(i0 + 1, totalPts - 1);
            double frac = idxFloat - i0;
            float x = static_cast<float>(pts[i0].x + (pts[i1].x - pts[i0].x) * frac);
            float y = static_cast<float>(pts[i0].y + (pts[i1].y - pts[i0].y) * frac);
            float nx = static_cast<float>(pts[i0].nx + (pts[i1].nx - pts[i0].nx) * frac);
            float ny = static_cast<float>(pts[i0].ny + (pts[i1].ny - pts[i0].ny) * frac);

            // 沿边框法线两侧扩展
            float px = -ny * kStripHalfWidth;
            float py =  nx * kStripHalfWidth;
            // 顶点 + 底点交替（TRIANGLE_STRIP）
            m_stripVertices.push_back({x + px, y + py, kBeamR, kBeamG, kBeamB, alpha, tu, 0.0f});
            m_stripVertices.push_back({x - px, y - py, kBeamR, kBeamG, kBeamB, alpha, tu, 1.0f});
        }
    }
    m_lineVertCount  = static_cast<int>(m_lineVertices.size());
    m_stripVertCount = static_cast<int>(m_stripVertices.size());
}

void GridBeamEffect::render(const Camera& camera) {
    if (!m_program || !m_vao || !m_vbo) return;
    generateVertices();
    if (m_lineVertCount == 0 && m_stripVertCount == 0) return;

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
    if (m_locAudioColor >= 0) glUniform3fv(m_locAudioColor, 1, m_audioColor);
    if (m_locMusicColorEnabled >= 0) {
        glUniform1i(m_locMusicColorEnabled, m_musicColorEnabled ? 1 : 0);
    }

    glBindVertexArray(m_vao);

    // ── Draw 1: GL_LINES（网格 + 边界）──
    if (m_lineVertCount > 0) {
        // 安全：clamp
        int lineCount = std::min(m_lineVertCount, m_maxLineVerts);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        lineCount * sizeof(GridVertex),
                        m_lineVertices.data());
        glDrawArrays(GL_LINES, 0, lineCount);
    }

    // ── Draw 2: GL_TRIANGLE_STRIP（光带）──
    if (m_stripVertCount > 0) {
        int stripCount = std::min(m_stripVertCount, m_maxStripVerts);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        stripCount * sizeof(GridVertex),
                        m_stripVertices.data());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, stripCount);
    }

    glBindVertexArray(0);
}
