#version 330 core

layout (location = 0) in vec2  aPosition;   // 粒子归一化边框位置
layout (location = 1) in float aLife;       // 剩余生命 [0,1]
layout (location = 2) in vec3  aColor;      // RGB 颜色
layout (location = 3) in float aSize;       // 粒子大小

uniform mat4 uProjection;       // 相机投影矩阵
uniform float uScreenWidth;     // 屏幕像素宽
uniform float uScreenHeight;    // 屏幕像素高
uniform vec2  uBorderSample[200]; // 边框采样点（归一化坐标 → 屏幕像素）
uniform int   uSampleCount = 200;

out float vLife;
out vec3  vColor;

void main() {
    // 根据位置在边框采样点中查找屏幕坐标
    float t = clamp(aPosition.x, 0.0, 0.999);
    int idx = int(t * float(uSampleCount - 1));

    // 简单线性插值
    vec2 p0 = uBorderSample[idx];
    vec2 p1 = uBorderSample[min(idx + 1, uSampleCount - 1)];
    float frac = fract(t * float(uSampleCount - 1));
    vec2 screenPos = mix(p0, p1, frac);

    // 归一化到 [-1, 1]
    vec2 ndc;
    ndc.x = (screenPos.x / uScreenWidth)  * 2.0 - 1.0;
    ndc.y = (screenPos.y / uScreenHeight) * 2.0 - 1.0;

    gl_Position = uProjection * vec4(ndc, 0.0, 1.0);
    gl_PointSize = aSize * aLife;  // 生命衰减 → 逐渐缩小

    vLife = aLife;
    vColor = aColor;
}
