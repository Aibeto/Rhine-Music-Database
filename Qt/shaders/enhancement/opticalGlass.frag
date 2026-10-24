#version 440
#extension GL_GOOGLE_include_directive : enable
#include "opticalCommon.glsl"
/**
 * 透明亚克力外壳。
 * 这一层必须能采到"它背后的画面"，否则无论怎么调高光都只是贴片，不会有厚度。
 * 因此在最终合成之后单独画一遍：采样已完成的场景色，按法线做屏幕空间折射偏移，
 * 再叠掠射角反射与轮廓亮边。磨砂（clarity 低）时把折射采样扩散成多点平均。
 */
layout(location = 3) in float v_theme;
layout(location = 4) in float v_viewZ;
layout(location = 8) in float v_clarity;
layout(location = 9) in vec3 v_world;
layout(location = 10) in vec3 v_normal;
layout(location = 0) out vec4 fragColor;
const vec3 KEY_LIGHT = normalize(vec3(0.0, 0.55, 0.84));
void main() {
    const vec3 n = normalize(v_normal);
    const vec3 v = normalize(g.eye.xyz - v_world);
    // 外壳双面绘制：背面法线与视线相反，直接 clamp 会让 ndv 恒为 0、rim 全饱和，
    // 整面被当成轮廓刷白。取绝对值后正反两面都能得到正确的掠射分布。
    const float ndv = clamp(abs(dot(n, v)), 0.0, 1.0);
    const float fresnel = pow(1.0 - ndv, 5.0);
    const float rim = pow(1.0 - ndv, 2.2);
    const float frost = 1.0 - clamp(v_clarity, 0.0, 1.0);
    // 只有很窄的一圈轮廓才需要存在感，整个 rim 区间参与会让正面糊成白雾。
    const float edgeMask = smoothstep(0.86, 0.995, rim);
    const vec3 tint = mix(vec3(0.86, 0.88, 0.90), vec3(0.12, 0.14, 0.16), v_theme);
    const float spec =
        pow(max(dot(n, normalize(KEY_LIGHT + v)), 0.0), mix(110.0, 8.0, frost)) *
        mix(1.35, 0.35, frost);
    // 只输出叠加在这层玻璃上的高光与描边；折射交给背景自身，避免整帧替换。
    const vec3 color = tint * (0.02 + 0.50 * edgeMask) + vec3(spec);
    const float alpha = clamp(0.03 + edgeMask * 0.70 + fresnel * 0.12, 0.0, 0.85);
    fragColor = vec4(color, alpha);
}
