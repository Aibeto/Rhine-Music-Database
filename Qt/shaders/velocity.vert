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

void main()
{
    mat4 currentModel = mat4(currentModel0, currentModel1, currentModel2, currentModel3);
    mat4 previousModel = mat4(previousModel0, previousModel1, previousModel2, previousModel3);

    vec4 eye = g.view * currentModel * vec4(position, 1.0);
    vec4 previousEye = g.view * previousModel * vec4(position, 1.0);
    vec4 clip = g.projection * eye;
    vec4 previousClip = g.projection * previousEye;

    gl_Position = correction * clip;

    // 屏幕 UV 约定与 quad.vert / copyColor.frag 一致：y 向下。这里不乘 correction，
    // 因为运动矢量是同一坐标系下前后两帧的差值，后端翻转会相消；抖动也在差值中相消。
    vec2 ndc = clip.xy / clip.w;
    vec2 previousNdc = previousClip.xy / previousClip.w;
    vec2 delta = previousNdc - ndc;
    // NDC 差值 → 屏幕 UV 位移：x 方向 0.5 倍，y 向下取反。
    v_velocity = vec2(delta.x, -delta.y) * 0.5;
}
