#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"
layout(binding = 2) uniform sampler2D color;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
// FXAA 3.11：在色调映射之后的 LDR 图像上做边缘检测与方向性混合。
// 只读 5 次（四角 + 中心）用于求边缘方向，再沿方向补两次采样，
// 是当前带宽预算下性价比最高的抗锯齿方案。
vec3 fxaa(vec2 uv) {
    vec2 rcp = 1.0 / vec2(textureSize(color, 0));
    vec3 nw = texture(color, rtUV(uv + vec2(-1, -1) * rcp)).rgb;
    vec3 ne = texture(color, rtUV(uv + vec2(1, -1) * rcp)).rgb;
    vec3 sw = texture(color, rtUV(uv + vec2(-1, 1) * rcp)).rgb;
    vec3 se = texture(color, rtUV(uv + vec2(1, 1) * rcp)).rgb;
    vec3 mid = texture(color, rtUV(uv)).rgb;
    const vec3 luma = vec3(.299, .587, .114);
    float lnw = dot(nw, luma), lne = dot(ne, luma), lsw = dot(sw, luma), lse = dot(se, luma),
          lm = dot(mid, luma);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    // 对比度低于阈值时说明是平坦区域，直接返回中心像素。
    if (lmax - lmin < max(.0312, lmax * .125)) {
        return mid;
    }
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * (.25 * .03125), 1.0 / 128.0);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, vec2(-8.0), vec2(8.0)) * rcp;
    vec3 a = .5 * (texture(color, rtUV(uv + dir * (1.0 / 3.0 - .5))).rgb +
                   texture(color, rtUV(uv + dir * (2.0 / 3.0 - .5))).rgb);
    vec3 b = a * .5 + .25 * (texture(color, rtUV(uv - dir * .5)).rgb +
                             texture(color, rtUV(uv + dir * .5)).rgb);
    float lb = dot(b, luma);
    return (lb < lmin || lb > lmax) ? a : b;
}
void main() {
    fragColor = vec4(fxaa(v_uv), 1);
}
