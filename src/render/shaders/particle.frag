#version 330 core

in float vLife;
in vec3  vColor;

out vec4 fragColor;

void main() {
    // 径向渐变：中心白 → 边缘透明
    float dist = length(gl_PointCoord - vec2(0.5));
    float alpha = 1.0 - smoothstep(0.0, 0.5, dist);

    // 生命衰减
    alpha *= vLife;

    // 软发光：中心高亮
    float glow = 1.0 - dist * 1.5;
    glow = max(0.0, glow);

    vec3 finalColor = vColor * (0.3 + glow * 0.7);
    fragColor = vec4(finalColor, alpha);
}
