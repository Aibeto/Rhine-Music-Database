#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"

// 时间性抗锯齿：把上一帧的历史结果按运动矢量重投影到当前帧，用当前帧的
// 3×3 邻域把历史颜色夹紧，再与当前帧混合。
// 当前帧本身带着 Engine 产生的亚像素抖动，8 个相位轮转，因此累积后的历史
// 相当于在每个像素内做了多次采样——这是 TAA 相对单帧 FXAA 的增益来源。
layout(binding = 2) uniform sampler2D currentColor; // 本帧色调映射结果（抖动过）
layout(binding = 3) uniform sampler2D historyColor; // 上一帧的 TAA 结果
layout(binding = 4) uniform sampler2D velocity;     // RG = 屏幕 UV 位移（current → previous）

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// 历史权重取 0.9，约等于 10 帧累积。抖动按 8 个相位循环，10 帧略长于一个周期，
// 每个子像素位置都能被采到；再往上加会明显拖影，往下减则收敛不到 8x 的采样密度。
const float HISTORY_WEIGHT = 0.9;

void main() {
    vec3 current = texture(currentColor, rtUV(v_uv)).rgb;
    // velocity 存的是 uv_previous - uv_current（见 velocity.vert），所以历史位置是加。
    // 抖动在速度差值里相消，这里不需要再补偿抖动相位。
    vec2 historyUV = v_uv + texture(velocity, rtUV(v_uv)).rg;

    vec3 history = current;
    // 位移把历史 UV 推出屏幕时没有可用历史（新露出的区域），直接输出当前帧。
    if (all(greaterThanEqual(historyUV, vec2(0.0))) && all(lessThanEqual(historyUV, vec2(1.0)))) {
        vec2 texel = 1.0 / vec2(textureSize(currentColor, 0));
        // 邻域夹紧：历史颜色必须落在当前帧 3×3 范围内。遮挡/解遮挡处的重投影会
        // 采到被遮挡物或背景的颜色，夹紧把这类错误样本压回合理区间，从而抑制鬼影；
        // 代价是真实的高频变化在 1~2 帧内被抹平，对 100Hz 输出不可察觉。
        vec3 low = current;
        vec3 high = current;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0) {
                    continue;
                }
                vec3 neighbour = texture(currentColor, rtUV(v_uv + vec2(x, y) * texel)).rgb;
                low = min(low, neighbour);
                high = max(high, neighbour);
            }
        }
        history = clamp(texture(historyColor, rtUV(historyUV)).rgb, low, high);
    }
    fragColor = vec4(mix(current, history, HISTORY_WEIGHT), 1.0);
}
