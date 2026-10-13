#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in float v_height;
layout(location = 4) in float v_viewZ;
layout(location = 5) in float v_quality;
layout(location = 6) in float v_clarity;
layout(location = 7) in float v_theme;
layout(location = 8) in vec4 v_shadow;
Out interpolated() {
    return Out(gl_FragCoord, v_world, v_normal, v_uv, v_height, v_viewZ, v_quality, v_clarity, v_theme,
               v_shadow);
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
layout(location = 0) out vec4 normalColor;
layout(location = 1) out vec4 depthColor;
void main() {
    vec3 normal = normalize((g.view * vec4(v_normal, 0)).xyz);
    normalColor = vec4(normal * .5 + .5, 1);
    depthColor = packDepth(gl_FragCoord.z);
}
