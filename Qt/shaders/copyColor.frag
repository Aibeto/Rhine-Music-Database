#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"
layout(binding = 2) uniform sampler2D src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
void main() {
    // 全屏直拷：替代 copyTexture 的跨格式拷贝。Vulkan 的 copyImage 要求格式兼容，
    // RGBA16F 源拷到 RGBA8 目标会产生未定义数据（实测整屏粉红偏色）；
    // 走渲染 pass 让 GPU 在采样时完成格式转换，任何后端结果一致。
    // v_uv 是左上原点，而纹理坐标的 Y 方向随后端不同（GL 的 framebuffer 原点在左下），
    // 必须与其它全屏通道一样经 rtUV 校正。漏掉这一步会让 GL 下的抓屏内容整体上下
    // 颠倒——透过玻璃看到的唱片倒置、面内自转方向相反。
    fragColor = texture(src, rtUV(v_uv));
}
