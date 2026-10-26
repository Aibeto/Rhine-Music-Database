// 后处理和运动输入共用的矩阵布局；与 CPU 的 544 字节全局块对应。
struct Globals {
    mat4 view;
    mat4 projection;
    mat4 shadow;
    mat4 inverseProjection;
    mat4 aoProjection;
    mat4 aoInverseProjection;
    vec4 eye;
    vec4 fogColor;
    vec4 fog;
    vec4 viewport;
    vec4 clip;
};
layout(std140, binding = 0) uniform GlobalBlock {
    Globals g;
    mat4 correction;
    vec4 backend;
};
vec2 rtUV(vec2 uv) {
    return backend.x > 0.5 ? vec2(uv.x, 1 - uv.y) : uv;
}
#define saturate(x) clamp(x, 0.0, 1.0)
