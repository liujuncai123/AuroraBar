/**
 * @file CrystalEffect.cpp
 * @brief 协奏子模式 0：晶柱升级实现
 * @date 2026-07-18
 * @details 顶点生成沿用旧 LightColumnEffect::generateCrystalSegmentColumns 逻辑
 *          （六边形棱柱 + 角色配色），shader 改为 3 层发光 + Reinhard。
 * @note 安全：所有段索引访问前均做越界检查；GL 资源在 cleanup() 中检查
 *          wglGetCurrentContext() 后释放，避免死上下文崩溃。
 */
#include "CrystalEffect.h"
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

CrystalEffect::CrystalEffect() { AURORA_TRACE("Crystal", "Constructor"); }
CrystalEffect::~CrystalEffect() { AURORA_TRACE("Crystal", "Destructor"); cleanup(); }

Result<void> CrystalEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("Crystal", "initialize() maxParticles={}", maxParticles);
    if (glInitFn) glInitFn();

    auto& segMgr = SegmentationManager::Instance();
    m_segmentCount = segMgr.totalSegments();
    if (m_segmentCount <= 0) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "No segments"));
    }
    m_segmentEnergy.assign(m_segmentCount, 0.0f);
    m_segmentRole.assign(m_segmentCount, SegmentRole::HarmonicMain);
    m_columnsPerSegment = std::max(3, maxParticles / m_segmentCount);
    m_crystalMaxVerts = m_segmentCount * m_columnsPerSegment * 54;
    m_vertices.reserve(m_crystalMaxVerts);
    return Result<void>::Ok();
}

void CrystalEffect::setSegmentEnergy(int segIdx, float emaValue) {
    // 安全：越界检查 + 范围 clamp
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentEnergy[segIdx] = std::clamp(emaValue, 0.0f, 1.0f);
    }
}

void CrystalEffect::setSegmentRole(int segIdx, SegmentRole role) {
    if (segIdx >= 0 && segIdx < m_segmentCount) {
        m_segmentRole[segIdx] = role;
    }
}

void CrystalEffect::setAudioColor(const float rgb[3]) {
    m_audioColor[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    m_audioColor[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    m_audioColor[2] = std::clamp(rgb[2], 0.0f, 1.0f);
    // 安全：ParamStore 未注册时 GetInt 返回默认值 0，musicColor 不启用
    m_musicColorEnabled = ParamStore::Instance().GetInt("concerto.musicColor") != 0;
}

unsigned CrystalEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{}; glGetShaderInfoLog(s, 1023, nullptr, log);
        AURORA_ERROR("Crystal", "{} shader compile: {}",
                     (type == GL_VERTEX_SHADER ? "VS" : "FS"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

Result<void> CrystalEffect::compileShaders() {
    AURORA_INFO("Crystal", "compileShaders() begin");
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
        AURORA_ERROR("Crystal", "Program link: {}", log);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }

    m_locProjection       = glGetUniformLocation(m_program, "uProjection");
    m_locScreenW         = glGetUniformLocation(m_program, "uScreenW");
    m_locScreenH         = glGetUniformLocation(m_program, "uScreenH");
    m_locTime            = glGetUniformLocation(m_program, "uTime");
    m_locAudioColor      = glGetUniformLocation(m_program, "uAudioColor");
    m_locMusicColorEnabled = glGetUniformLocation(m_program, "uMusicColorEnabled");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 m_crystalMaxVerts * sizeof(CrystalVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    GLsizei stride = sizeof(CrystalVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(CrystalVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(CrystalVertex, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(CrystalVertex, localH)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(CrystalVertex, r)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(CrystalVertex, alpha)));
    glBindVertexArray(0);
    AURORA_INFO("Crystal", "compileShaders() OK, maxVerts={}", m_crystalMaxVerts);
    return Result<void>::Ok();
}

void CrystalEffect::cleanup() {
    // 安全：上下文已死时禁止调用 glDelete*，否则触发访问冲突
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_vao)     glDeleteVertexArrays(1, &m_vao);
        if (m_vbo)     glDeleteBuffers(1, &m_vbo);
        if (m_program) glDeleteProgram(m_program);
    }
    m_vao = 0;
    m_vbo = 0;
    m_program = 0;
    m_vertices.clear();
    m_crystalVertCount = 0;
}

// ============================================================
// shader 源码：3 层发光 + Reinhard（替代 ACES，避免发灰）
// ============================================================

const char* CrystalEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in float aLocalH;
layout(location=3) in vec3 aColor;
layout(location=4) in float aAlpha;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
out float vLocalH;
out vec3 vColor;
out float vAlpha;
void main() {
    // 🔧 [Qt6 迁移修复] Qt6 DirectComposition framebuffer 是 bottom-right origin，
    // 需同时翻转 X 和 Y，否则画面上下颠倒 + 左右镜像
    float ndcX = 1.0 - aPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    vLocalH = aLocalH;
    vColor = aColor;
    vAlpha = aAlpha;
}
)glsl";
}

const char* CrystalEffect::fragmentSource() {
    return R"glsl(#version 330 core
in float vLocalH;
in vec3 vColor;
in float vAlpha;
uniform vec3 uAudioColor;
uniform int uMusicColorEnabled;
out vec4 fragColor;

// Reinhard 色调映射（替代 ACES，保饱和度）
vec3 reinhard(vec3 c) { return c / (1.0 + c); }

void main() {
    // 基色：musicColor 启用时用音频色，否则用顶点色
    vec3 baseColor = (uMusicColorEnabled == 1) ? uAudioColor : vColor;

    // ── 3 层发光（符合硬上限，替代旧 9 层叠加）──
    // 1. 柱身渐变：底冷青 → 顶白
    vec3 bottomColor = vec3(0.0, 0.78, 1.0);   // #00C8FF
    vec3 topColor    = vec3(0.94, 0.97, 1.0);  // #F0F8FF
    vec3 layer1 = mix(bottomColor, topColor, vLocalH);

    // 2. 顶部高光：vLocalH > 0.85 时叠白
    float topHL = smoothstep(0.85, 1.0, vLocalH);
    vec3 layer2 = vec3(1.0) * topHL * 0.5;

    // 3. 底部柔光：exp 衰减
    float bottomGlow = exp(-vLocalH * 4.0) * 0.3;
    vec3 layer3 = vec3(0.0, 0.5, 1.0) * bottomGlow;

    vec3 color = layer1 * 0.6 + layer2 + layer3;
    // 用 baseColor 调色（音乐驱动或段角色）
    color = mix(color, color * baseColor, 0.5);

    // Reinhard 色调映射（exposure 1.5 避免偏暗）
    color = reinhard(color * 1.5);

    fragColor = vec4(color, vAlpha);
}
)glsl";
}

// ============================================================
// 顶点生成：六边形棱柱（沿用旧 LightColumnEffect::generateCrystalSegmentColumns）
// ============================================================

void CrystalEffect::generateVertices() {
    if (!m_geometry || m_geometry->pointCount() < 3) {
        m_crystalVertCount = 0;
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

    constexpr int kHexSides = 6;
    constexpr float kPi = 3.141592653589793f;

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
        int cornerZone = std::min(m_geometry->cornerTransition(),
                                  static_cast<int>(nominalWidth * 0.3));
        float hexRadius = std::max(nominalWidth * 0.6f, 0.01f);

        struct { float u, v; } hexUV[kHexSides];
        for (int i = 0; i < kHexSides; ++i) {
            float angle = 2.0f * kPi * static_cast<float>(i) / kHexSides;
            hexUV[i].u = hexRadius * std::cos(angle);
            hexUV[i].v = hexRadius * std::sin(angle);
        }

        int localIdx = segIdx % segsPerEdge;
        float zPrev = zPulse, zNext = zPulse;
        if (segsPerEdge > 1) {
            int base = segIdx - localIdx;
            if (localIdx > 0)               zPrev = m_segmentEnergy[base + localIdx - 1];
            if (localIdx + 1 < segsPerEdge) zNext = m_segmentEnergy[base + localIdx + 1];
        }

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

            // 角区跳过，避免柱体挤在转角
            if (edgeIdx == 0 || edgeIdx == 2) {
                if (bx < cornerZone || bx > scrW - cornerZone) continue;
            }

            double colRatio = static_cast<double>(c) / std::max(1, m_columnsPerSegment - 1);
            double baseZ;
            if (colRatio < 0.5) {
                double w = (0.5 - colRatio) * 2.0;
                baseZ = zPrev * w + zPulse * (1.0 - w);
            } else {
                double w = (colRatio - 0.5) * 2.0;
                baseZ = zPulse * (1.0 - w) + zNext * w;
            }
            double subDist = 0.5 - 0.5 * std::cos(colRatio * kPi * 0.85);
            double heightFactor = 0.12 + subDist * 0.88;
            double colZPulse = baseZ * heightFactor;
            double wobble = std::sin((segIdx * m_columnsPerSegment + c) * 2.3999632297 + m_time * 1.3) * 0.08;
            colZPulse = std::max(0.0, colZPulse * (1.0 + wobble));

            float nx = static_cast<float>(pts[i0].nx + (pts[i1].nx - pts[i0].nx) * frac);
            float ny = static_cast<float>(pts[i0].ny + (pts[i1].ny - pts[i0].ny) * frac);
            float baseX = static_cast<float>(bx);
            float baseY = static_cast<float>(by);
            float rawHeight = static_cast<float>(bw) * static_cast<float>(colZPulse);
            constexpr float kMinH = 3.0f;
            float height = rawHeight < kMinH
                ? rawHeight + (kMinH - rawHeight) * (1.0f - std::exp(-rawHeight / kMinH))
                : rawHeight;
            float tipX = baseX + nx * height;
            float tipY = baseY + ny * height;
            float topZOffset = height * 0.12f;

            // 段角色色（主职责暖色，副职责冷色）
            SegmentRole role = m_segmentRole[segIdx];
            float cr, cg, cb;
            switch (role) {
            case SegmentRole::HarmonicMain:    cr=1.0f; cg=0.78f; cb=0.0f; break;  // 橙金
            case SegmentRole::HarmonicAux:     cr=1.0f; cg=0.67f; cb=0.0f; break;
            case SegmentRole::HarmonicWeak:    cr=0.8f; cg=0.5f;  cb=0.0f; break;
            case SegmentRole::PercussiveMain:  cr=0.0f; cg=0.88f; cb=1.0f; break;  // 青蓝
            case SegmentRole::PercussiveAux:   cr=0.4f; cg=0.0f;  cb=1.0f; break;  // 紫
            case SegmentRole::PercussiveWeak:  cr=0.3f; cg=0.0f;  cb=0.7f; break;
            default:                           cr=cg=cb=0.5f;    break;
            }
            float alpha = alphaBase + static_cast<float>(colZPulse) * (1.0f - alphaBase);

            struct { float bx, by, tx, ty; } hexW[kHexSides];
            for (int i = 0; i < kHexSides; ++i) {
                float hu = hexUV[i].u, hv = hexUV[i].v;
                hexW[i].bx = baseX + hu * nx - hv * ny;
                hexW[i].by = baseY + hu * ny + hv * nx;
                hexW[i].tx = tipX  + hu * nx - hv * ny;
                hexW[i].ty = tipY  + hu * ny + hv * nx;
            }

            // 侧面 6 × 2 三角形
            for (int s = 0; s < kHexSides; ++s) {
                int sNext = (s + 1) % kHexSides;
                float b0x = hexW[s].bx,     b0y = hexW[s].by;
                float b1x = hexW[sNext].bx, b1y = hexW[sNext].by;
                float t0x = hexW[s].tx,     t0y = hexW[s].ty;
                float t1x = hexW[sNext].tx, t1y = hexW[sNext].ty;
                float e1x = b1x - b0x, e1y = b1y - b0y;
                float e2x = tipX - baseX, e2y = tipY - baseY;
                float fnx = e1y * topZOffset, fny = -e1x * topZOffset;
                float fnz = e1x * e2y - e1y * e2x;
                float lenSq = fnx*fnx + fny*fny + fnz*fnz;
                if (lenSq < 1e-9f) {
                    fnx = nx; fny = ny; fnz = 0.01f;
                    lenSq = fnx*fnx + fny*fny + fnz*fnz;
                }
                float invLen = 1.0f / std::sqrt(lenSq);
                fnx *= invLen; fny *= invLen; fnz *= invLen;

                m_vertices.push_back({b0x, b0y, 0.0f,       fnx, fny, fnz, 0.0f, cr, cg, cb, alpha});
                m_vertices.push_back({b1x, b1y, 0.0f,       fnx, fny, fnz, 0.0f, cr, cg, cb, alpha});
                m_vertices.push_back({t0x, t0y, topZOffset, fnx, fny, fnz, 1.0f, cr, cg, cb, alpha});
                m_vertices.push_back({b1x, b1y, 0.0f,       fnx, fny, fnz, 0.0f, cr, cg, cb, alpha});
                m_vertices.push_back({t1x, t1y, topZOffset, fnx, fny, fnz, 1.0f, cr, cg, cb, alpha});
                m_vertices.push_back({t0x, t0y, topZOffset, fnx, fny, fnz, 1.0f, cr, cg, cb, alpha});
            }
            // 顶盖 6 三角形扇形（围绕 tipX/tipY）
            for (int s = 0; s < kHexSides; ++s) {
                int sNext = (s + 1) % kHexSides;
                m_vertices.push_back({tipX, tipY,             topZOffset, 0.0f, 0.0f, 1.0f, 1.0f, cr, cg, cb, alpha});
                m_vertices.push_back({hexW[s].tx, hexW[s].ty, topZOffset, 0.0f, 0.0f, 1.0f, 1.0f, cr, cg, cb, alpha});
                m_vertices.push_back({hexW[sNext].tx, hexW[sNext].ty, topZOffset, 0.0f, 0.0f, 1.0f, 1.0f, cr, cg, cb, alpha});
            }
        }
    }
    m_crystalVertCount = static_cast<int>(m_vertices.size());
}

void CrystalEffect::render(const Camera& camera) {
    if (!m_program || !m_vao || !m_vbo) return;
    generateVertices();
    if (m_crystalVertCount == 0) return;
    // 安全：顶点数钳制，避免越界写 VBO
    if (m_crystalVertCount > m_crystalMaxVerts) {
        AURORA_WARN("Crystal", "vertCount {} > max {}, clamping",
                    m_crystalVertCount, m_crystalMaxVerts);
        m_crystalVertCount = m_crystalMaxVerts;
    }

    glUseProgram(m_program);

    // 投影矩阵缓存（避免每帧上传）
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
    if (m_locAudioColor >= 0) {
        glUniform3fv(m_locAudioColor, 1, m_audioColor);
    }
    if (m_locMusicColorEnabled >= 0) {
        glUniform1i(m_locMusicColorEnabled, m_musicColorEnabled ? 1 : 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    m_crystalVertCount * sizeof(CrystalVertex),
                    m_vertices.data());
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, m_crystalVertCount);
    glBindVertexArray(0);
}
