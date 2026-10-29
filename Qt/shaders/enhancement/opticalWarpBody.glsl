// 重投影插帧的着色器主体，由各相位变体定义 WARP_T 后 include。
//
// 运动矢量编码（opticalGeometry.vert:41）：
//   v_velocity = vec2(prevNdc.x - ndc.x, ndc.y - prevNdc.y) * 0.5
// 其中 * 0.5 已把 NDC 增量换算成 UV 增量，y 因纹理行序与 NDC 朝向相反而取反号。
// 因此「当前像素往哪儿回看上一帧」= uv + vec2(v.x, -v.y) * WARP_T。
//
// 注意：相位刻意用编译期常量而不是 uniform——本机的 Intel ANV 驱动在
// 「同一管线同时绑两个 std140 uniform block」时会 SIGFPE 崩溃（已在管线创建阶段复现），
// 因此相位走多份烘焙变体，运行期按需选管线。

#ifndef WARP_T
#define WARP_T 0.5
#endif

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(binding = 2) uniform sampler2D sceneColor;
layout(binding = 3) uniform sampler2D motionVectors;

// 矢量长度回退阈值（UV 单位）：超出说明是刚被遮挡又出现的区域或矢量异常，
// 直接回退到当前帧颜色，避免整块画面被拉成长条纹。
const float MAX_SPAN = 0.08;

void main() {
    vec2 uv = v_uv;
    vec2 motion = texture(motionVectors, rtUV(uv)).rg;
    vec2 back = vec2(motion.x, -motion.y);
    if (length(back) > MAX_SPAN) {
        fragColor = texture(sceneColor, rtUV(uv));
        return;
    }
    fragColor = texture(sceneColor, rtUV(uv + back * WARP_T));
}
