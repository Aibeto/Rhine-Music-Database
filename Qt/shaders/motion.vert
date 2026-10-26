#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"

// 运动矢量输入：每实例 128 字节 = 当前帧模型矩阵（4 列）+ 上一帧模型矩阵（4 列）。
// 与 surface 管线共用 32 字节顶点格式，只换实例布局，因此不透明/玻璃 draw 可以直接复用顶点缓冲。
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 currentModel0;
layout(location = 4) in vec4 currentModel1;
layout(location = 5) in vec4 currentModel2;
layout(location = 6) in vec4 currentModel3;
layout(location = 7) in vec4 previousModel0;
layout(location = 8) in vec4 previousModel1;
layout(location = 9) in vec4 previousModel2;
layout(location = 10) in vec4 previousModel3;

layout(location = 0) out vec2 v_velocity;

void main() {
    mat4 currentModel = mat4(currentModel0, currentModel1, currentModel2, currentModel3);
    mat4 previousModel = mat4(previousModel0, previousModel1, previousModel2, previousModel3);

    vec4 world = currentModel * vec4(position, 1.0);
    vec4 clip = g.projection * g.view * world;
    vec4 previousClip = g.shadow * previousModel * vec4(position, 1.0);
    gl_Position = correction * clip;

    // 专用全局块以 shadow 保存上一帧 VP、inverseProjection 保存当前未抖动 VP。
    // 光栅位置带采样抖动，速度只表达真实模型与相机运动，屏幕 UV 的 y 方向向下。
    vec4 unjittered = g.inverseProjection * world;
    vec2 ndc = unjittered.xy / unjittered.w;
    vec2 previousNdc = previousClip.xy / previousClip.w;
    vec2 delta = previousNdc - ndc;
    // NDC 差值 → 屏幕 UV 位移：x 方向 0.5 倍，y 向下取反。
    v_velocity = vec2(delta.x, -delta.y) * 0.5;
}
