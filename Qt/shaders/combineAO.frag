#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"
float viewZ(float depth, Globals g) {
    return g.clip.x * g.clip.y / ((g.clip.y - g.clip.x) * depth - g.clip.y);
}
vec4 packDepth(float v) {
    if (v <= 0) {
        return vec4(0);
    }
    if (v >= 1) {
        return vec4(1);
    }
    float integer;
    float a = modf(v * 16777216.0, integer);
    float b = modf(integer / 256.0, integer);
    float green = modf(integer / 256.0, integer);
    return vec4(integer / 255.0, green * 256.0 / 255.0, b * 256.0 / 255.0, a);
}
float unpackDepth(vec4 v) {
    return dot(v, vec4(255.0 / 256, 255.0 / 65536, 255.0 / 16777216, 1.0 / 16777216));
}
layout(binding = 2) uniform sampler2D color;
layout(binding = 3) uniform sampler2D ao;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
vec4 effect() {
    // 4x4 盒式模糊；半径按 AO 自身纹素计算，因此 AO 目标分辨率变化时模糊的屏幕半径会自动跟随。
    // 原实现分成 blurAO 与 combineAO 两个全屏通道，合并后省一次全屏往返。
    vec2 px = 1.0 / vec2(textureSize(ao, 0).x, textureSize(ao, 0).y);
    // 4 次双线性采样等价于原来的 4x4 点采样：采样点落在纹素角上时，双线性权重正好是
    // 相邻 2x2 四个纹素各 1/4，四次平均即 16 点均值。AO 纹理在 quad() 里绑定的是 linear，
    // 所以结果等价，而每像素的 AO 读取从 16 次降到 4 次。
    const vec2 offsets[4] =
        vec2[4](vec2(-1.5, -0.5), vec2(0.5, -0.5), vec2(-1.5, 1.5), vec2(0.5, 1.5));
    float occlusion = 0;
    for (int i = 0; i < 4; i++) {
        occlusion += texture(ao, rtUV(v_uv + offsets[i] * px)).r;
    }
    return vec4(texture(color, rtUV(v_uv)).rgb * (occlusion / 4), 1);
}
void main() {
    fragColor = effect();
}
