#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"
layout(binding = 2) uniform sampler2D source;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
void main() {
    // 输出是输入的一半分辨率，双线性采样在输出像素中心正好覆盖 2x2 输入纹素，
    // 等价于一次盒式降采样。逐级降采样得到真正的模糊金字塔，
    // 避免稀疏点采样在硬边缘上留下台阶。
    fragColor = vec4(texture(source, rtUV(v_uv)).rgb, 1);
}
