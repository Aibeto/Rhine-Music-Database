#version 440
#extension GL_GOOGLE_include_directive : enable
#include "opticalCommon.glsl"

// 调试变体：把运动矢量场直接可视化成灰度（越长越亮），用来验证 MRT 输出是否真实有效。
// 静止区域应为黑，运动物体/相机漂移处应出现亮区。
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(binding = 2) uniform sampler2D sceneColor;
layout(binding = 3) uniform sampler2D motionVectors;

void main() {
    vec2 motion = texture(motionVectors, rtUV(v_uv)).rg;
    float span = length(vec2(motion.x, -motion.y));
    float v = clamp(span * 4000.0, 0.0, 1.0);
    fragColor = vec4(v, v, v, 1.0);
}
