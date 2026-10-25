#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"
float viewZ(float depth, Globals g) {
    return g.clip.x * g.clip.y / ((g.clip.y - g.clip.x) * depth - g.clip.y);
}
layout(binding = 2) uniform sampler2D color;
layout(binding = 3) uniform sampler2D ao;
layout(binding = 4) uniform sampler2D depth;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
vec4 effect() {
    // 直接读取原生深度附件，避免额外的 RGBA 打包深度图及半精度量化误差。
    float z = viewZ(texture(depth, rtUV(v_uv)).r, g), b = clamp((g.fog.z + z) * g.fog.w, -.011, .011);
    vec2 aspect = vec2(1, g.viewport.x / g.viewport.y);
    // AO 在模糊之后统一乘一次：AO 自身已经是低频（4x4 盒式模糊），先模糊颜色再乘 AO
    // 与先乘后模糊观感一致，但省掉一个全屏的合成通道（约 84MB/帧 的读写往返）。
    float occlusion = aoBlurred(ao, v_uv);
    vec3 c = texture(color, rtUV(v_uv)).rgb;
    // 失焦半径不足 1 像素时，采样全部落在同一个纹素附近，直接返回单次采样。
    // 0.4 是 offsets[] 的最大分量；x/y 方向换算成纹素后同为 |b|*0.4*宽度。
    if (abs(b) * .4 * float(textureSize(color, 0).x) < 1.0) {
        return vec4(c * occlusion, 1);
    }
    // 20 个偏移 + 中心：原 40 个点按环取一半，外环 8 点保留，内环交替取轴向/对角，
    // 采样数减半而弥散盘分布仍然均匀（半径上限只有约 5 个半分辨率纹素）。
    const vec2 offsets[20] = vec2[20](
        vec2(0.0, -0.4), vec2(0.29, -0.29), vec2(0.4, -0.0), vec2(0.29, 0.29), vec2(0.0, 0.4),
        vec2(-0.29, -0.29), vec2(-0.4, -0.0), vec2(-0.29, 0.29), vec2(0.135, -0.333),
        vec2(0.333, 0.135), vec2(-0.135, -0.333), vec2(-0.333, 0.135), vec2(0.28, -0.0),
        vec2(0.0, 0.28), vec2(-0.28, -0.0), vec2(0.0, -0.28), vec2(0.116, -0.116),
        vec2(0.116, 0.116), vec2(-0.116, -0.116), vec2(-0.116, 0.116));
    for (int i = 0; i < 20; i++) {
        c += texture(color, rtUV(v_uv + offsets[i] * aspect * b)).rgb;
    }
    return vec4(c / 21 * occlusion, 1);
}
void main() {
    fragColor = effect();
}
