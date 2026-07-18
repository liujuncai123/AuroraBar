// ============================================================================
// ⚠️  DEPRECATED — 本文件已废弃，请勿在新代码中使用
// ----------------------------------------------------------------------------
//  原方案、问题、替代方案详见 LightColumnEffect.h 顶部说明。
//  本 .cpp 仍被 CMakeLists 编译以保持兼容性，但不再被 ConcertoRenderer 使用。
// ============================================================================
#ifdef _MSC_VER
#pragma message("warning: " __FILE__ " is DEPRECATED — replaced by concerto/*Effect implementations")
#endif

/**
 * @file LightColumnEffect.cpp
 * @brief [DEPRECATED] 光柱 / 晶柱效果实现（已被 concerto/*Effect 替代）
 * @date 2026-07-11
 */
#include "LightColumnEffect.h"
#include "../../logging/LoggerManager.h"
#include <GL/wglew.h>
#include <cstring>

// 直接声明 OutputDebugStringA，避免 windows.h 的 min/max 宏污染
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);

LightColumnEffect::LightColumnEffect() { AURORA_TRACE("LightColumn", "Constructor"); }
LightColumnEffect::~LightColumnEffect() { AURORA_TRACE("LightColumn", "Destructor"); cleanup(); }

Result<void> LightColumnEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("LightColumn", "initialize() maxParticles={}", maxParticles);
    m_maxVertices = maxParticles * 6;
    m_crystalMaxVerts = maxParticles * 54;  // 每晶柱 54 顶点（6 面 × 6 顶点 + 顶盖 18 顶点）
    if (glInitFn) glInitFn();
    return Result<void>::Ok();
}

void LightColumnEffect::setSubMode(int mode) {
    m_subMode = mode;
}

// ─────────────────────────────────────────────
//  1. 扁平光柱管线编译
// ─────────────────────────────────────────────
void LightColumnEffect::compileFlatPipeline() {
    AURORA_TRACE("LightColumn", "compileFlatPipeline BEGIN");
    OutputDebugStringA("=== compileFlatPipeline: entry\n");
    unsigned vs = compileShader(GL_VERTEX_SHADER, vertexSource());
    OutputDebugStringA("=== compileFlatPipeline: VS compiled\n");
    unsigned fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource());
    OutputDebugStringA("=== compileFlatPipeline: FS compiled\n");
    if (!vs || !fs) {
        AURORA_WARN("LightColumn", "Flat shader compile failed — flat pipeline unavailable");
        return;
    }

    OutputDebugStringA("=== compileFlatPipeline: creating program\n");
    m_program = glCreateProgram();
    glAttachShader(m_program, vs); glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    int linked = 0; glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!linked) {
        char log[512]{}; glGetProgramInfoLog(m_program, 511, nullptr, log);
        AURORA_ERROR("LightColumn", "Flat link failed: {}", log);
        glDeleteProgram(m_program); m_program = 0;
        return;
    }

    m_locProjection = glGetUniformLocation(m_program, "uProjection");
    m_locScreenW    = glGetUniformLocation(m_program, "uScreenW");
    m_locScreenH    = glGetUniformLocation(m_program, "uScreenH");
    m_locSubMode    = glGetUniformLocation(m_program, "uSubMode");
    m_locTime       = glGetUniformLocation(m_program, "uTime");
    m_locInnerGlow  = glGetUniformLocation(m_program, "uInnerGlow");

    glGenVertexArrays(1, &m_vao); glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_maxVertices * sizeof(ColumnVertex), nullptr, GL_DYNAMIC_DRAW);

    auto stride = static_cast<GLsizei>(sizeof(ColumnVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(ColumnVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(ColumnVertex, localX));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(ColumnVertex, localY));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(ColumnVertex, r));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(ColumnVertex, alpha));
    glBindVertexArray(0);

    AURORA_INFO("LightColumn", "Flat pipeline OK, maxVertices={}", m_maxVertices);
}

// ─────────────────────────────────────────────
//  2. 晶柱管线编译 & 上传
// ─────────────────────────────────────────────
Result<void> LightColumnEffect::compileCrystalShaders() {
    AURORA_TRACE("LightColumn", "compileCrystalShaders BEGIN");
    OutputDebugStringA("=== compileCrystalShaders: entry\n");
    unsigned vs = compileShader(GL_VERTEX_SHADER, crystalVertexSource());
    if (!vs) {
        AURORA_ERROR("LightColumn", "Crystal vertex shader compile failed");
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, "Crystal vertex shader compile failed"));
    }
    AURORA_TRACE("LightColumn", "Crystal VS compiled OK");
    OutputDebugStringA("=== compileCrystalShaders: VS ok, compiling FS\n");
    unsigned fs = compileShader(GL_FRAGMENT_SHADER, crystalFragmentSource());
    if (!fs) {
        AURORA_ERROR("LightColumn", "Crystal fragment shader compile failed");
        glDeleteShader(vs);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, "Crystal fragment shader compile failed"));
    }
    AURORA_TRACE("LightColumn", "Crystal FS compiled OK, linking…");
    m_crystalProg = glCreateProgram();
    glAttachShader(m_crystalProg, vs); glAttachShader(m_crystalProg, fs);
    glLinkProgram(m_crystalProg);
    int linked = 0; glGetProgramiv(m_crystalProg, GL_LINK_STATUS, &linked);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!linked) {
        char log[512]{}; glGetProgramInfoLog(m_crystalProg, 511, nullptr, log);
        AURORA_ERROR("LightColumn", "Crystal link failed: {}", log);
        glDeleteProgram(m_crystalProg); m_crystalProg = 0;
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }
    AURORA_TRACE("LightColumn", "Crystal program linked, setting up VAO/VBO…");
    m_cLocProjection = glGetUniformLocation(m_crystalProg, "uProjection");
    m_cLocScreenW    = glGetUniformLocation(m_crystalProg, "uScreenW");
    m_cLocScreenH    = glGetUniformLocation(m_crystalProg, "uScreenH");
    m_cLocTime       = glGetUniformLocation(m_crystalProg, "uTime");
    m_cLocInnerGlow  = glGetUniformLocation(m_crystalProg, "uInnerGlow");
    m_cLocFlowSpeed  = glGetUniformLocation(m_crystalProg, "uFlowSpeed");

    glGenVertexArrays(1, &m_crystalVao); glGenBuffers(1, &m_crystalVbo);
    glBindVertexArray(m_crystalVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_crystalVbo);
    glBufferData(GL_ARRAY_BUFFER, m_crystalMaxVerts * sizeof(CrystalVertex), nullptr, GL_DYNAMIC_DRAW);

    auto cstride = static_cast<GLsizei>(sizeof(CrystalVertex));
    // location 0: vec3 position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, cstride, (void*)offsetof(CrystalVertex, x));
    // location 1: vec3 normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, cstride, (void*)offsetof(CrystalVertex, nx));
    // location 2: float localH
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, cstride, (void*)offsetof(CrystalVertex, localH));
    // location 3: vec3 color
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, cstride, (void*)offsetof(CrystalVertex, r));
    // location 4: float alpha
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, cstride, (void*)offsetof(CrystalVertex, alpha));
    glBindVertexArray(0);

    AURORA_INFO("LightColumn", "Crystal pipeline OK, maxVerts={}", m_crystalMaxVerts);
    return Result<void>::Ok();
}

void LightColumnEffect::setCrystalColumns(const CrystalVertex* vertices, int count) {
    AURORA_TRACE("LightColumn", "setCrystalColumns count={}", count);
    // 安全：GL 入口点必须检查上下文有效性
    //   project_memory 约定：所有 GL 入口点（含 setColumns/setCrystalColumns）需 wglGetCurrentContext 检查
    //   上下文丢失时调用 glBufferSubData 虽不立即崩溃但会静默失败，提前返回避免无效状态
    if (!wglGetCurrentContext()) return;
    if (!m_crystalVbo || count <= 0) return;
    if (count > m_crystalMaxVerts) {
        m_crystalMaxVerts = count * 2;
        glBindBuffer(GL_ARRAY_BUFFER, m_crystalVbo);
        glBufferData(GL_ARRAY_BUFFER, m_crystalMaxVerts * sizeof(CrystalVertex), nullptr, GL_DYNAMIC_DRAW);
        AURORA_INFO("LightColumn", "Crystal VBO resized to {} vertices", m_crystalMaxVerts);
    }
    m_crystalVertCount = count;
    glBindBuffer(GL_ARRAY_BUFFER, m_crystalVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(CrystalVertex), vertices);
}

Result<void> LightColumnEffect::compileShaders() {
    OutputDebugStringA("=== compileShaders: entry\n");
    compileFlatPipeline();
    OutputDebugStringA("=== compileShaders: flat pipeline done\n");
    compileCrystalShaders();  // 不阻塞——晶柱失败时扁平光柱仍可用
    OutputDebugStringA("=== compileShaders: crystal done\n");
    if (!m_program && !m_crystalProg) {
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, "Both shader pipelines failed"));
    }
    return Result<void>::Ok();
}

// ─────────────────────────────────────────────
//  3. 顶点上传
// ─────────────────────────────────────────────
void LightColumnEffect::setColumns(const ColumnVertex* vertices, int count) {
    // 安全：GL 入口点必须检查上下文有效性（同 setCrystalColumns）
    if (!wglGetCurrentContext()) return;
    if (!m_vbo || count <= 0) return;
    if (count > m_maxVertices) {
        m_maxVertices = count * 2;
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, m_maxVertices * sizeof(ColumnVertex), nullptr, GL_DYNAMIC_DRAW);
        AURORA_INFO("LightColumn", "Flat VBO resized to {} vertices", m_maxVertices);
    }
    m_columnCount = count;
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(ColumnVertex), vertices);
}

// ─────────────────────────────────────────────
//  4. 渲染调度
// ─────────────────────────────────────────────
void LightColumnEffect::render(const Camera& camera) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (m_subMode == 0) {
        // ── 晶柱边境管线 ──
        if (m_crystalVertCount > 0 && m_crystalProg && m_crystalVao) {
            glUseProgram(m_crystalProg);
            const auto& mat = camera.matrix();
            if (m_cLocProjection >= 0 && std::memcmp(mat.data(), m_cachedProj.data(), 16 * sizeof(float)) != 0) {
                glUniformMatrix4fv(m_cLocProjection, 1, GL_FALSE, mat.data());
                m_cachedProj = mat;
            }
            if (m_cLocScreenW >= 0 && m_screenW != m_cachedScreenW) {
                glUniform1f(m_cLocScreenW, static_cast<float>(m_screenW));
                m_cachedScreenW = static_cast<float>(m_screenW);
            }
            if (m_cLocScreenH >= 0 && m_screenH != m_cachedScreenH) {
                glUniform1f(m_cLocScreenH, static_cast<float>(m_screenH));
                m_cachedScreenH = static_cast<float>(m_screenH);
            }
            if (m_cLocTime >= 0)      glUniform1f(m_cLocTime, m_time);
            if (m_cLocInnerGlow >= 0) glUniform1f(m_cLocInnerGlow, m_innerGlow);
            if (m_cLocFlowSpeed >= 0) glUniform1f(m_cLocFlowSpeed, m_flowSpeed);
            glBindVertexArray(m_crystalVao);
            glDrawArrays(GL_TRIANGLES, 0, m_crystalVertCount);
        }
    } else {
        // ── 扁平光柱管线（subMode 1/2/3） ──
        if (m_columnCount > 0 && m_program && m_vao) {
            glUseProgram(m_program);
            const auto& mat = camera.matrix();
            if (m_locProjection >= 0 && std::memcmp(mat.data(), m_cachedProj.data(), 16 * sizeof(float)) != 0) {
                glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, mat.data());
                m_cachedProj = mat;
            }
            if (m_locScreenW >= 0 && m_screenW != m_cachedScreenW) {
                glUniform1f(m_locScreenW, static_cast<float>(m_screenW));
                m_cachedScreenW = static_cast<float>(m_screenW);
            }
            if (m_locScreenH >= 0 && m_screenH != m_cachedScreenH) {
                glUniform1f(m_locScreenH, static_cast<float>(m_screenH));
                m_cachedScreenH = static_cast<float>(m_screenH);
            }
            if (m_locSubMode >= 0) glUniform1i(m_locSubMode, m_subMode);
            if (m_locTime >= 0)    glUniform1f(m_locTime, m_time);
            if (m_locInnerGlow >= 0) glUniform1f(m_locInnerGlow, m_innerGlow);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_TRIANGLES, 0, m_columnCount);
        }
    }
    glDisable(GL_BLEND);
}

void LightColumnEffect::cleanup() {
    // 安全：上下文丢失时 wglGetCurrentContext() 返回 NULL，
    //       此时 glDelete* 会导致 0xc0000005 访问违规崩溃。
    //       驱动在上下文丢失时已自动回收 GL 对象，仅需清零 ID 防止误用。
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_vao) { glDeleteVertexArrays(1, &m_vao); }
        if (m_vbo) { glDeleteBuffers(1, &m_vbo); }
        if (m_program) { glDeleteProgram(m_program); }
        if (m_crystalVao) { glDeleteVertexArrays(1, &m_crystalVao); }
        if (m_crystalVbo) { glDeleteBuffers(1, &m_crystalVbo); }
        if (m_crystalProg) { glDeleteProgram(m_crystalProg); }
    }
    m_vao = 0; m_vbo = 0; m_program = 0;
    m_crystalVao = 0; m_crystalVbo = 0; m_crystalProg = 0;
    m_columnCount = 0;
    m_crystalVertCount = 0;
}

unsigned LightColumnEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]{}; glGetShaderInfoLog(s, 511, nullptr, log);
        AURORA_ERROR("LightColumn", "{} shader: {}",
            (type == GL_VERTEX_SHADER ? "Vertex" : "Fragment"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// ====================================================================
//  扁平光柱着色器源码（subMode 1/2/3）
// ====================================================================

const char* LightColumnEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in float aLocalX;
layout(location=2) in float aLocalY;
layout(location=3) in vec3 aColor;
layout(location=4) in float aAlpha;

uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
uniform float uTime;
uniform int uSubMode;

out float vLocalX;
out float vLocalY;
out vec3 vColor;
out float vAlpha;
out float vSwayOffset;

float hash(float x, float y) {
    return fract(sin(x * 127.1 + y * 311.7) * 43758.5453);
}

void main() {
    float px = aPos.x;
    float py = aPos.y;

    if (uSubMode == 2 && aLocalY > 0.1) {
        float swaySeed = px + py * 0.7;
        float sway = sin(uTime * 3.5 + swaySeed * 0.05) * 8.0
                   + sin(uTime * 5.7 + swaySeed * 0.13) * 5.0
                   + hash(px, py) * aLocalY * 12.0;
        vSwayOffset = sway * aLocalY;
        px += vSwayOffset;
    } else {
        vSwayOffset = 0.0;
    }

    float ndcX = px / uScreenW * 2.0 - 1.0;
    float ndcY = 1.0 - py / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    vLocalX = aLocalX;
    vLocalY = aLocalY;
    vColor = aColor;
    vAlpha = aAlpha;
}
)glsl";
}

const char* LightColumnEffect::fragmentSource() {
    return R"glsl(#version 330 core
in float vLocalX;
in float vLocalY;
in vec3 vColor;
in float vAlpha;
in float vSwayOffset;

uniform int   uSubMode;
uniform float uTime;
uniform float uInnerGlow;

out vec4 fragColor;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 hsv2rgb(float h, float s, float v) {
    vec3 c = vec3(h * 6.0, s, v);
    vec3 rgb = clamp(abs(mod(c.x + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return c.z * mix(vec3(1.0), rgb, c.y);
}

void main() {
    float glow, tipFade, alpha;
    vec3 color;

    if (uSubMode == 1) {
        float hue = vLocalX * 0.5 + 0.5 + uTime * 0.02;
        color = hsv2rgb(fract(hue), 0.9, 1.0);
        float coreG = exp(-vLocalX * vLocalX * 30.0);
        float innerG = exp(-vLocalX * vLocalX * 10.0);
        float mainG = exp(-vLocalX * vLocalX * 4.0);
        float outerG = exp(-vLocalX * vLocalX * 1.5);
        float ambG = exp(-vLocalX * vLocalX * 0.6);
        glow = coreG * 0.5 + innerG * 0.7 + mainG * 0.8 + outerG * 0.4 + ambG * 0.2;
        tipFade = 1.0 - vLocalY * vLocalY * vLocalY * 0.8;
        alpha = glow * tipFade * vAlpha;
        color = mix(color, vec3(1.0), coreG * 0.7);
        float lightDir = 0.5 + vLocalX * 0.5;
        color *= (0.55 + lightDir * 0.45);
        float specular = pow(clamp(vLocalX + 0.7, 0.0, 1.0), 3.0) * 0.25;
        color += vec3(1.0, 0.95, 0.8) * specular * coreG;
        float tipFlash = 0.0;
        if (vLocalY > 0.85) {
            float tipT = (vLocalY - 0.85) / 0.15;
            float sparkle = 0.5 + 0.5 * sin(uTime * 8.0 + vLocalX * 80.0 + vLocalY * 30.0);
            tipFlash = exp(-tipT * tipT * 6.0) * sparkle * 0.6;
        }
        color += vec3(1.0, 1.0, 0.9) * tipFlash;
        alpha += tipFlash * 0.4;
        float baseGlow = exp(-(1.0 - vLocalY) * 4.0) * 0.45 * uInnerGlow;
        color += vec3(1.0, 0.9, 0.6) * baseGlow;
        float fresnel = 1.0 - abs(vLocalX) * 0.35;
        color *= fresnel;
        color *= (0.7 + vLocalY * 0.3);
    } else if (uSubMode == 2) {
        vec3 fireBottom = vec3(1.0, 0.95, 0.8);
        vec3 fireMid    = vec3(1.0, 0.5, 0.05);
        vec3 fireTop    = vec3(0.6, 0.05, 0.0);
        color = mix(fireBottom, fireMid, smoothstep(0.0, 0.4, vLocalY));
        color = mix(color,     fireTop,   smoothstep(0.4, 0.95, vLocalY));
        float flicker = 0.85 + 0.15 * sin(uTime * 8.0 + vLocalY * 20.0)
                              + 0.08 * sin(uTime * 23.0 + vLocalY * 57.0);
        flicker = clamp(flicker, 0.7, 1.3);
        float coreG = exp(-vLocalX * vLocalX * 30.0);
        float mainG = exp(-vLocalX * vLocalX * 5.0);
        float outerG = exp(-vLocalX * vLocalX * 1.5);
        glow = coreG * 0.6 + mainG * 0.8 + outerG * 0.35;
        tipFade = 1.0 - vLocalY * vLocalY * 0.7;
        alpha = glow * tipFade * vAlpha * flicker;
        color = mix(color, vec3(1.0, 0.95, 0.7), coreG * 0.5);
        float lightDir = 0.5 + vLocalX * 0.5;
        color *= (0.55 + lightDir * 0.45);
        float baseGlow = exp(-(1.0 - vLocalY) * 4.0) * 0.4 * uInnerGlow;
        color += vec3(1.0, 0.8, 0.3) * baseGlow;
        float fresnel = 1.0 - abs(vLocalX) * 0.35;
        color *= fresnel;
        color *= (0.7 + vLocalY * 0.3);
    } else {
        // subMode=3: 均衡器
        float band = floor(vLocalY * 3.0);
        float bandLocal = fract(vLocalY * 3.0);
        vec3 bandColor;
        if (band < 0.5) {
            bandColor = mix(vec3(0.8, 0.1, 0.05), vec3(1.0, 0.4, 0.1), bandLocal);
        } else if (band < 1.5) {
            bandColor = mix(vec3(0.1, 0.8, 0.1), vec3(0.6, 1.0, 0.2), bandLocal);
        } else {
            bandColor = mix(vec3(0.1, 0.3, 1.0), vec3(0.6, 0.2, 1.0), bandLocal);
        }
        color = bandColor;
        float coreG = exp(-vLocalX * vLocalX * 25.0);
        float mainG = exp(-vLocalX * vLocalX * 5.0);
        float outerG = exp(-vLocalX * vLocalX * 1.5);
        glow = coreG * 0.5 + mainG * 0.75 + outerG * 0.3;
        alpha = glow * vAlpha;
        float bandEdge = abs(bandLocal - 0.5);
        if (bandEdge > 0.48) alpha *= 0.85;
        float lightDir = 0.5 + vLocalX * 0.5;
        color *= (0.55 + lightDir * 0.45);
        float specular = pow(clamp(vLocalX + 0.7, 0.0, 1.0), 3.0) * 0.2;
        color += vec3(1.0, 0.95, 0.8) * specular * coreG;
        float baseGlow = exp(-(1.0 - vLocalY) * 6.0) * 0.35 * uInnerGlow;
        color += vec3(1.0, 0.85, 0.5) * baseGlow;
        float fresnel = 1.0 - abs(vLocalX) * 0.3;
        color *= fresnel;
        color *= (0.7 + vLocalY * 0.3);
    }
    color = aces(color);
    if (alpha < 0.01) discard;
    fragColor = vec4(color, clamp(alpha, 0.0, 1.0));
}
)glsl";
}

// ====================================================================
//  晶柱着色器源码 — subMode=0
// ====================================================================

const char* LightColumnEffect::crystalVertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in vec3 aPos;      // 屏幕像素空间 XY + 深度 Z
layout(location=1) in vec3 aNormal;   // 法线
layout(location=2) in float aLocalH;  // 柱内高度 [0,1]
layout(location=3) in vec3 aColor;    // RGB
layout(location=4) in float aAlpha;   // Alpha

uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
uniform float uTime;

out vec3 vWorldPos;   // 屏幕像素空间坐标（与扁平光柱保持一致）
out vec3 vNormal;     // 法线
out float vLocalH;    // 柱内高度
out vec3 vColor;      // 颜色
out float vAlpha;     // Alpha

void main() {
    // 将屏幕像素 XY 转为 NDC，与扁平光柱管线一致
    float ndcX = aPos.x / uScreenW * 2.0 - 1.0;
    float ndcY = 1.0 - aPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, aPos.z, 1.0);
    vWorldPos = aPos;    // 片段着色器仍用像素空间做纹理计算
    vNormal   = aNormal;
    vLocalH   = aLocalH;
    vColor    = aColor;
    vAlpha    = aAlpha;
}
)glsl";
}

const char* LightColumnEffect::crystalFragmentSource() {
    return R"glsl(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in float vLocalH;
in vec3 vColor;
in float vAlpha;

uniform float uTime;
uniform float uInnerGlow;
uniform float uFlowSpeed;

out vec4 fragColor;

// ═══════════════════════════════════════════════════════════
//  ACES 简易色调映射
// ═══════════════════════════════════════════════════════════
vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// ═══════════════════════════════════════════════════════════
//  解析噪声：正弦组合，产生不规则折射感
// ═══════════════════════════════════════════════════════════
float noise3D(vec3 p) {
    float n  = sin(p.x * 12.9898 + p.y * 78.233 + p.z * 45.164);
    n       *= sin(p.y * 39.346 + p.z * 12.459 + p.x * 23.678);
    n       *= sin(p.z * 57.123 + p.x * 34.891 + p.y * 67.432);
    n       += sin(p.x * 93.212 + p.y * 21.754 + p.z * 17.839) * 0.7;
    n       += sin(p.y * 56.987 + p.z * 43.121 + p.x * 88.346) * 0.5;
    return n * 0.35 + 0.5;  // 映射到 [0.15, 0.85]
}

void main() {
    // ═══════════════════════════════════════════════════════
    //  1. 解析噪声扰动表面法线 —— 不规则折射感
    // ═══════════════════════════════════════════════════════
    vec3 noiseCoord = vWorldPos * 0.012 + uTime * 0.15;
    float n1 = noise3D(noiseCoord);
    float n2 = noise3D(noiseCoord + vec3(5.3, 2.7, 9.1));

    // 在表面切线空间施加微小扰动
    vec3 T1 = normalize(
        abs(vNormal.x) < 0.9
            ? cross(vNormal, vec3(1.0, 0.0, 0.0))
            : cross(vNormal, vec3(0.0, 1.0, 0.0))
    );
    vec3 T2 = cross(vNormal, T1);
    vec3 N = normalize(vNormal + T1 * (n1 - 0.5) * 0.3 + T2 * (n2 - 0.5) * 0.3);

    // ═══════════════════════════════════════════════════════
    //  2. 世界空间菲涅尔反射 —— 边缘亮 / 中心透，强度 0.7
    // ═══════════════════════════════════════════════════════
    vec3 V = vec3(0.0, 0.0, 1.0);  // 正交投影视线方向
    float NdotV = abs(dot(N, V));
    float fresnel = 0.7 * pow(1.0 - NdotV, 3.5);

    // ═══════════════════════════════════════════════════════
    //  3. 半兰伯特漫反射 + 菲涅尔高光
    // ═══════════════════════════════════════════════════════
    vec3 L = normalize(vec3(0.25, -0.55, 0.8));  // 主光源方向（略偏右上方）
    float NdotL = dot(N, L);
    float halfLambert = NdotL * 0.5 + 0.5;
    halfLambert *= halfLambert;  // 柔化衰减

    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float specFresnel = fresnel * 0.55;
    // NdotH^80 用乘法链替代 pow（7 mul vs 1 pow，快 3-5×）
    float n2_  = NdotH * NdotH;
    float n4  = n2_ * n2_;
    float n8  = n4 * n4;
    float n16 = n8 * n8;
    float n32 = n16 * n16;
    float n64 = n32 * n32;
    float n80 = n64 * n16;
    float specular = specFresnel * n80;

    // ═══════════════════════════════════════════════════════
    //  4. 底暖 → 顶冷 渐变基底色
    // ═══════════════════════════════════════════════════════
    vec3 bottom = vColor * 1.15;
    vec3 top    = vColor * vec3(0.55, 0.65, 1.2);
    vec3 base = mix(bottom, top, vLocalH * vLocalH);

    // ═══════════════════════════════════════════════════════
    //  5. 两条沿高度方向流动的亮带（uFlowSpeed 控速，平滑边缘）
    // ═══════════════════════════════════════════════════════
    float phase = uTime * uFlowSpeed;
    float bandA = fract(vLocalH * 3.0 + phase * 0.55);
    float bandB = fract(vLocalH * 3.0 + phase * 0.55 + 0.5);
    // 三角波：中心 = 1，边缘 = 0
    float shapeA = 1.0 - abs(bandA - 0.5) * 2.0;
    float shapeB = 1.0 - abs(bandB - 0.5) * 2.0;
    float bandGlow = smoothstep(0.0, 0.1, shapeA) + smoothstep(0.0, 0.1, shapeB);

    // ═══════════════════════════════════════════════════════
    //  6. 根部暖色指数衰减光池（localH < 0.15）
    // ═══════════════════════════════════════════════════════
    float inRoot = step(vLocalH, 0.15);
    float rootDecay = exp(-vLocalH * 25.0) * inRoot;
    vec3  rootWarm  = vColor * vec3(1.6, 0.7, 0.3);
    float rootGlow  = rootDecay * 0.85 * uInnerGlow;

    // ═══════════════════════════════════════════════════════
    //  7. 光照合成
    // ═══════════════════════════════════════════════════════
    vec3 ambient = base * 0.22;
    vec3 diffuse = base * halfLambert * 0.9;

    vec3 lit = ambient + diffuse;
    lit += base * bandGlow * 0.7;                   // 流动亮带叠加
    lit += rootWarm * rootGlow;                      // 根部暖色光池
    lit += vColor * fresnel * 1.05;                  // 菲涅尔边缘发光
    lit += vec3(1.0, 0.92, 0.78) * specular;         // 菲涅尔高光

    // ═══════════════════════════════════════════════════════
    //  8. ACES 色调映射
    // ═══════════════════════════════════════════════════════
    lit = aces(lit);

    float alpha = (0.55 + fresnel * 0.45) * vAlpha;
    if (alpha < 0.01) discard;
    fragColor = vec4(lit, clamp(alpha, 0.0, 1.0));
}
)glsl";
}
