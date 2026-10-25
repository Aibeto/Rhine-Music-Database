#version 440
layout(std140, binding = 0) uniform FgBlock {
    mat4 correction;
    vec4 params; // x: 采样时是否翻转 v（纹理原点与帧缓冲原点不一致时用）
};
// baseTex：FG 插值帧，或在 reset 帧 / FG 关闭时直接用的当前帧颜色。
layout(binding = 1) uniform sampler2D baseTex;
// uiTex：Qt 渲染出来的叠加层，预乘 alpha（QML 的透明窗口按预乘合成）。
layout(binding = 2) uniform sampler2D uiTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
void main() {
    vec2 uv = params.x > .5 ? vec2(v_uv.x, 1.0 - v_uv.y) : v_uv;
    vec4 ui = texture(uiTex, uv);
    vec3 base = texture(baseTex, uv).rgb;
    // 预乘 alpha 的 source-over：dst = src + dst * (1 - src.a)。
    // 和 Qt 自己把叠加层画到窗口上的结果一致——FG 只是把"画在哪"从窗口换成了
    // 交换链图像，颜色空间与混合公式不能跟着变，否则开关 FG 会出现亮度跳变。
    fragColor = vec4(ui.rgb + base * (1.0 - ui.a), 1.0);
}
