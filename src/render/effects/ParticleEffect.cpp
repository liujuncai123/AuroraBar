/**
 * @file ParticleEffect.cpp
 * @brief 粒子效果实现
 * @date 2026-07-06
 */
#include "ParticleEffect.h"
#include "../BorderGeometry.h"
#include "../../logging/LoggerManager.h"
#include <GL/glew.h>
#include <GL/wglew.h>
#include <algorithm>
#include <cstring>

ParticleEffect::ParticleEffect() { AURORA_TRACE("ParticleEffect", "Constructor"); }
ParticleEffect::~ParticleEffect() { AURORA_TRACE("ParticleEffect", "Destructor"); cleanup(); }

Result<void> ParticleEffect::initialize(std::function<void()> glInitFn, int maxParticles) {
    AURORA_INFO("ParticleEffect", "initialize() maxParticles={}", maxParticles);
    if (maxParticles < 100)
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInvalidArgument, "maxParticles must be >= 100"));
    m_particles.resize(maxParticles);
    m_activeCount = 0;
    if (glInitFn) glInitFn();
    AURORA_INFO("ParticleEffect", "initialize() OK capacity={}", maxParticles);
    return Result<void>::Ok();
}

void ParticleEffect::update(float dt) {
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    for (auto& p : m_particles) {
        if (p.isDead()) continue;
        // 生命 1:1 衰减（秒），生命=2.0 则 2 秒后死亡
        p.life -= dt;
        // 累计行进距离（像素）
        float pixelDist = std::abs(p.velocity) * dt * m_screenPerimeter;
        p.totalDistance += pixelDist;
        p.position += p.velocity * dt;
        if (p.position > 1.0f) p.position -= 1.0f;
        if (p.position < 0.0f) p.position += 1.0f;
        // 距离超限 → 立即死亡
        if (p.totalDistance > m_maxDistance) p.life = 0.0f;
    }
    recycleDead();
}

void ParticleEffect::render(const Camera& camera) {
    if (m_activeCount <= 0) return;
    if (!m_program || !m_vao || !m_vbo) {
        static int logCount = 0;
        if (logCount++ < 3) AURORA_WARN("ParticleEffect", "render: program={} vao={} vbo={}", m_program, m_vao, m_vbo);
        return;
    }
    // 安全：防止 activeCount 超过 VBO 容量导致缓冲区溢出崩溃
    int cap = capacity();
    if (m_activeCount > cap) {
        AURORA_WARN("ParticleEffect", "render: activeCount({}) > capacity({}), clamping", m_activeCount, cap);
        m_activeCount = cap;
    }

    glUseProgram(m_program);
    const auto& mat = camera.matrix();
    if (std::memcmp(mat.data(), m_cachedProj.data(), 16 * sizeof(float)) != 0) {
        glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, mat.data());
        m_cachedProj = mat;
    }
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, m_activeCount * sizeof(Particle), m_particles.data());
    glBindVertexArray(m_vao);
    glDrawArrays(GL_POINTS, 0, m_activeCount);
}

void ParticleEffect::cleanup() {
    // 安全：上下文丢失时 wglGetCurrentContext() 返回 NULL，
    //       此时 glDelete* 会导致 0xc0000005 访问违规崩溃。
    //       驱动在上下文丢失时已自动回收 GL 对象，仅需清零 ID 防止误用。
    bool glValid = (wglGetCurrentContext() != nullptr);
    if (glValid) {
        if (m_vao) { glDeleteVertexArrays(1, &m_vao); }
        if (m_vbo) { glDeleteBuffers(1, &m_vbo); }
        if (m_program) { glDeleteProgram(m_program); }
    }
    m_vao = 0; m_vbo = 0; m_program = 0;
    m_particles.clear();
    m_activeCount = 0;
}

Result<void> ParticleEffect::compileShaders() {
    unsigned vs = compileShader(GL_VERTEX_SHADER, vertexSource());
    unsigned fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource());
    if (!vs || !fs) return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, "Shader compile failed"));
    m_program = glCreateProgram();
    glAttachShader(m_program, vs); glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    int linked = 0; glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!linked) {
        char log[512]{}; glGetProgramInfoLog(m_program, 511, nullptr, log);
        AURORA_ERROR("ParticleEffect", "Link: {}", log);
        return Result<void>::Err(MAKE_ERROR(ErrorCode::kInternalError, log));
    }
    m_locProjection = glGetUniformLocation(m_program, "uProjection");
    m_locScreenW = glGetUniformLocation(m_program, "uScreenW");
    m_locScreenH = glGetUniformLocation(m_program, "uScreenH");
    m_locCount = glGetUniformLocation(m_program, "uBorderCount");
    m_locBorderNormal = glGetUniformLocation(m_program, "uBorderNormal");
    m_locTime = glGetUniformLocation(m_program, "uTime");
    m_locOscAmp = glGetUniformLocation(m_program, "uOscAmp");
    m_locSpreadWidth = glGetUniformLocation(m_program, "uSpreadWidth");
    AURORA_INFO("ParticleEffect", "Shader uniforms: proj={} sw={} sh={} count={} normal={} time={} oscAmp={} spread={}",
                m_locProjection, m_locScreenW, m_locScreenH, m_locCount,
                m_locBorderNormal, m_locTime, m_locOscAmp, m_locSpreadWidth);
    glEnable(GL_PROGRAM_POINT_SIZE);  // OpenGL 3.3 Core 必须显式启用，否则 gl_PointSize 被忽略
    glGenVertexArrays(1, &m_vao); glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_particles.size() * sizeof(Particle), nullptr, GL_DYNAMIC_DRAW);
    auto stride = static_cast<GLsizei>(sizeof(Particle));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Particle, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Particle, life));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Particle, r));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Particle, size));
    glBindVertexArray(0);
    glUseProgram(m_program);
    glUniform1f(m_locScreenW, 2560.f); glUniform1f(m_locScreenH, 1440.f);
    glUniform1i(m_locCount, 64);
    // uBorder 由 updateBorderUniform() 在 RenderThread::onInitialize 中设置
    AURORA_INFO("ParticleEffect", "compileShaders() OK");
    return Result<void>::Ok();
}

void ParticleEffect::updateBorderUniform(const BorderGeometry& geo) {
    // 🔧 [Qt 迁移修复] 移除 wglGetCurrentContext() 检查：
    //   旧 Win32 方案下 wgl 句柄有效；但 Qt6 QOpenGLContext 在某些配置下用 EGL/ANGLE，
    //   wglGetCurrentContext() 返回 NULL，导致 uBorder 永不设置 → 粒子全画在 (0,0)。
    //   Qt 的 makeCurrent 已保证 GL 上下文有效，m_program 检查已足够。
    if (!m_program) {
        AURORA_WARN("ParticleEffect", "updateBorderUniform: m_program=0, skipping");
        return;
    }
    const auto& pts = geo.points();
    size_t totalPts = pts.size();
    if (totalPts < 3) return;

    float border[128]{};     // 64 points × 2 floats
    float normal[128]{};     // 64 normals × 2 floats

    for (int i = 0; i < 64; ++i) {
        double t = static_cast<double>(i) / 63.0;
        size_t srcIdx = static_cast<size_t>(t * (totalPts - 1));
        if (srcIdx >= totalPts) srcIdx = totalPts - 1;
        border[i * 2]     = static_cast<float>(pts[srcIdx].x);
        border[i * 2 + 1] = static_cast<float>(pts[srcIdx].y);

        // 使用预计算的平滑法线（角区已插值），不再自己算
        normal[i * 2]     = pts[srcIdx].nx;
        normal[i * 2 + 1] = pts[srcIdx].ny;
    }

    glUseProgram(m_program);
    int locBorder = glGetUniformLocation(m_program, "uBorder");
    glUniform2fv(locBorder, 64, border);
    glUniform2fv(m_locBorderNormal, 64, normal);
    glUniform1f(m_locScreenW, static_cast<float>(geo.screenW()));
    glUniform1f(m_locScreenH, static_cast<float>(geo.screenH()));
    // 存储屏幕周长，用于 update() 中距离累计
    m_screenPerimeter = 2.0f * (static_cast<float>(geo.screenW()) + static_cast<float>(geo.screenH()));
}

// 从 BorderGeometry 重新计算屏幕周长
void ParticleEffect::setScreenPerimeter(const BorderGeometry& geo) {
    const auto& pts = geo.points();
    double perimeter = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        double dx = pts[i].x - pts[i-1].x;
        double dy = pts[i].y - pts[i-1].y;
        perimeter += std::sqrt(dx*dx + dy*dy);
    }
    m_screenPerimeter = static_cast<float>(perimeter);
}

unsigned ParticleEffect::compileShader(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]{}; glGetShaderInfoLog(s, 511, nullptr, log);
        AURORA_ERROR("ParticleEffect", "{} shader: {}", (type==GL_VERTEX_SHADER?"Vertex":"Fragment"), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool ParticleEffect::spawn(float position, float velocity, float life,
                           float size, float r, float g, float b) {
    // 安全：先回收死粒子，避免池满时误判
    for (auto& p : m_particles) {
        if (p.isDead()) {
            p = {position, velocity, life, size, r, g, b};
            m_activeCount++;
            return true;
        }
    }
    // 安全：池满且已达硬上限时拒绝生成，不覆盖旧粒子（防止反馈循环）
    constexpr int kMaxCapacity = 10000;
    if (static_cast<int>(m_particles.size()) >= kMaxCapacity) {
        static int warnCount = 0;
        if (++warnCount <= 5) {
            AURORA_WARN("ParticleEffect", "spawn rejected — pool full at {} (hard limit={})",
                        m_particles.size(), kMaxCapacity);
        }
        return false;
    }
    // 池满但未达硬上限：覆盖最旧粒子（旧行为，保持兼容）
    if (!m_particles.empty()) {
        m_particles[0] = {position, velocity, life, size, r, g, b};
        return true;
    }
    return false;
}

void ParticleEffect::recycleDead() {
    int writeIdx = 0;
    for (int i = 0; i < static_cast<int>(m_particles.size()); ++i) {
        if (!m_particles[i].isDead()) {
            if (i != writeIdx) m_particles[writeIdx] = m_particles[i];
            ++writeIdx;
        }
    }
    m_activeCount = writeIdx;
}

void ParticleEffect::resize(int newCapacity) {
    // 安全：硬上限 10000 粒子，防止无限增长导致 OOM / GPU 超时崩溃
    constexpr int kMaxCapacity = 10000;
    if (newCapacity < 100) return;
    if (newCapacity > kMaxCapacity) {
        AURORA_WARN("ParticleEffect", "resize capped {} → {} (hard limit={})",
                    newCapacity, kMaxCapacity, kMaxCapacity);
        newCapacity = kMaxCapacity;
    }
    if (newCapacity == capacity()) return;

    // 先回收死粒子，保留活跃的
    recycleDead();

    // 调整 CPU 端数组
    m_particles.resize(newCapacity);

    // 调整 GPU VBO
    // 🔧 [Qt 迁移修复] 移除 wglGetCurrentContext() 检查：
    //   resize() 总在 renderFrame → renderOneFrame 中调用（已 makeCurrent），
    //   Qt 方案下 wgl 句柄可能为 NULL（EGL/ANGLE），但 GL 调用本身有效。
    //   m_vbo 检查已足够防止未初始化时崩溃。
    if (m_vbo) {
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, newCapacity * sizeof(Particle), nullptr, GL_DYNAMIC_DRAW);
    }

    AURORA_INFO("ParticleEffect", "resize {} → {} (active={})",
                capacity() > newCapacity ? "shrink" : "grow",
                newCapacity, m_activeCount);
}

const char* ParticleEffect::vertexSource() {
    return R"glsl(#version 330 core
layout(location=0) in float aPosition;
layout(location=1) in float aLife;
layout(location=2) in vec3 aColor;
layout(location=3) in float aSize;
uniform mat4 uProjection;
uniform float uScreenW;
uniform float uScreenH;
uniform int uBorderCount;
uniform vec2 uBorder[64];
uniform vec2 uBorderNormal[64];
uniform float uTime;
uniform float uOscAmp[16];
uniform float uSpreadWidth;  // 粒子横向散布宽度（px）
out float vLife;
out vec3 vColor;
out vec2 vScreenDir;  // 屏幕空间运动方向（归一化）
void main() {
    // 在 uBorder 中线性插值找对应像素坐标
    float idx = aPosition * float(uBorderCount - 1);
    int i0 = int(floor(idx));
    int i1 = min(i0 + 1, uBorderCount - 1);
    float frac = idx - float(i0);
    vec2 pixelPos = mix(uBorder[i0], uBorder[i1], frac);
    vec2 normal   = normalize(mix(uBorderNormal[i0], uBorderNormal[i1], frac));

    // 运动方向：采样前方 2% 处的边框位置求切线
    float ahead = min(aPosition + 0.02, 1.0);
    float aidx = ahead * float(uBorderCount - 1);
    int ai0 = int(floor(aidx));
    int ai1 = min(ai0 + 1, uBorderCount - 1);
    float af = aidx - float(ai0);
    vec2 posAhead = mix(uBorder[ai0], uBorder[ai1], af);
    vScreenDir = normalize(posAhead - pixelPos);

    // 集体弦波：沿边框行进的波形（8 个波长，5Hz 行波，步长短细节多）
    float wave = sin(aPosition * 6.28318 * 8.0 - uTime * 5.0);
    float globalAmp = uOscAmp[0];
    float osc = globalAmp * wave;

    // 多泳道：5 条平行弦线，用 aLife（终身不变）分配泳道
    float laneIdx = floor(fract(aLife * 123.456) * 5.0);  // 0~4，终身不变
    float laneOffset = (laneIdx / 4.0 * 2.0 - 1.0) * 0.8;  // -0.8~0.8
    float spread = laneOffset * uSpreadWidth;

    pixelPos += normal * (spread + osc);

    // 像素坐标 → NDC
    //   🔧 [Qt 迁移修复] Qt6 QWindow + DirectComposition 透明窗口下，framebuffer
    //   实测是 bottom-right origin（相当于标准 OpenGL 旋转 180°）：
    //     - 不翻转 → 左右反上下反（用户实测确认）
    //     - 只翻转 Y → 上下正但左右反
    //     - 同时翻转 X 和 Y → 左右正上下正 ✓
    //   旧 Win32 方案下 framebuffer 是标准 bottom-left origin，只需翻转 Y。
    //   旧方案已 DEPRECATED，此处按 Qt 方案同时翻转 X 和 Y。
    float ndcX = 1.0 - pixelPos.x / uScreenW * 2.0;
    float ndcY = 1.0 - pixelPos.y / uScreenH * 2.0;
    gl_Position = uProjection * vec4(ndcX, ndcY, 0.0, 1.0);
    gl_PointSize = aSize * aLife;
    vLife = aLife; vColor = aColor;
}
)glsl";
}

const char* ParticleEffect::fragmentSource() {
    return R"glsl(#version 330 core
in float vLife;
in vec3 vColor;
in vec2 vScreenDir;
out vec4 fragColor;

// ACES 色调映射
vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    // 丢弃圆形外的像素（消除正方形背景）
    if (length(gl_PointCoord - vec2(0.5)) > 0.5) discard;

    vec2 coord = gl_PointCoord - vec2(0.5);

    // 投影到运动方向：正=前方（头），负=后方（尾）
    float along = dot(coord, vScreenDir);
    float across = length(coord - along * vScreenDir);

    // 头部：紧凑圆形光晕（实体化：更锐利的边缘）
    float headDist = sqrt(along * along * 2.0 + across * across * 2.0);
    float headAlpha = 1.0 - smoothstep(0.0, 0.28, headDist);

    // 尾部：拉伸椭圆光晕（实体化：更强的不透明度）
    float tailDist = across * 1.3;
    float tailLen = max(0.0, -along);
    float tailAlpha = (1.0 - tailLen / 0.55) * (1.0 - smoothstep(0.0, 0.30, tailDist));
    tailAlpha = max(0.0, tailAlpha) * 0.75;

    float alpha = max(headAlpha, tailAlpha);
    // 实体化：最少 35% 不透明度，不完全消退
    alpha *= (0.35 + 0.65 * vLife);

    // 外发光环（bloom 效果）
    float outerGlow = exp(-headDist * 1.8) * 0.3;
    alpha += outerGlow * vLife;

    // 内核心
    float core = exp(-headDist * headDist * 30.0) * 0.9;
    vec3 coreColor = vec3(1.0, 1.0, 0.95);

    // 尾光偏冷色调
    vec3 tailColor = vColor * 0.5 + vec3(0.3, 0.2, 0.9) * 0.5;
    float headWeight = headAlpha / max(0.001, headAlpha + tailAlpha);
    vec3 c = mix(tailColor, vColor, headWeight);
    c = mix(c, coreColor, core);
    c *= (0.3 + alpha * 0.7);

    // ACES 色调映射
    c = aces(c);

    fragColor = vec4(c, clamp(alpha, 0.0, 1.0));
}
)glsl";
}
