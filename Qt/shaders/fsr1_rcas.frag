#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"

// FSR1 RCAS（Robust Contrast Adaptive Sharpening）：3x3 邻域的对比度自适应锐化。
//
// 公式来源：AMD FidelityFX SDK v1.1.4
//   FidelityFX-SDK/sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h
//     FsrRcasCon()  常量打包
//     FsrRcasF()    3x3 主循环
//   经典 FSR1 v1.20210629 中对应 FsrRcasCon()/FsrRcasF()。
//
// 与 AMD 参考实现的差异：
//   1. AMD 用 ffxApproximateReciprocalMedium 近似倒数；GLSL 用精确除法，
//      质量更高，代价是几条 ALU。
//   2. 近似倒数对 0 返回有限大值，GLSL 的 1.0/0.0 是 inf、0*inf 是 NaN，
//      所以可能为零的分母都夹到极小值。
//   3. 常量由 Qt/src/fsr/Fsr1Constants.h 按 AMD 的 4xvec4 布局生成。
//
// 方向：输入与输出同分辨率；rtUV() 把 v_uv 换成纹理内存行序（GL 下翻转）。
layout(std140, binding = 1) uniform Fsr1Block {
    vec4 uCon0; // {sharpness, 0, 0, 0}，sharpness 已按 exp2(-stops) 换算
    vec4 uCon1; // {sharpness, sharpness, 0, 0}，AMD 的 packHalf2x16 解包，32 位路径不读
    vec4 uCon2; // 保留，与参考实现的 4xvec4 布局一致
    vec4 uCon3; // 保留，与参考实现的 4xvec4 布局一致
};
layout(binding = 2) uniform sampler2D color;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// SDK 的 ffx_fsr1_rcas.h 默认定义 FSR_RCAS_DENOISE=1。关掉能省一次乘法，
// 但更容易把胶片颗粒/抖动噪声当成边缘来锐化。
#define FSR_RCAS_DENOISE 1
// 超过这个限幅的结果会显得不自然（AMD 注释：sharpening 的可用上限）。
const float FSR_RCAS_LIMIT = 0.25 - (1.0 / 16.0);

// 取一个纹素。AMD 依赖采样器的 ClampToEdge；texelFetch 不经过采样器，手动夹紧。
vec3 rcasLoad(ivec2 size, ivec2 p) {
    return texelFetch(color, clamp(p, ivec2(0), size - 1), 0).rgb;
}

void main() {
    vec2 uv = rtUV(v_uv);
    ivec2 size = textureSize(color, 0);
    ivec2 ip = ivec2(uv * vec2(size));

    // 最小 3x3 邻域：
    //    b
    //  d e f
    //    h
    vec3 tB = rcasLoad(size, ip + ivec2(0, -1));
    vec3 tD = rcasLoad(size, ip + ivec2(-1, 0));
    vec3 tE = rcasLoad(size, ip);
    vec3 tF = rcasLoad(size, ip + ivec2(1, 0));
    vec3 tH = rcasLoad(size, ip + ivec2(0, 1));

    // 亮度 x2。
    float lB = tB.b * .5 + (tB.r * .5 + tB.g);
    float lD = tD.b * .5 + (tD.r * .5 + tD.g);
    float lE = tE.b * .5 + (tE.r * .5 + tE.g);
    float lF = tF.b * .5 + (tF.r * .5 + tF.g);
    float lH = tH.b * .5 + (tH.r * .5 + tH.g);

    // 噪声检测：3x3 高通，按局部对比度归一化，平坦区域自动减弱锐化。
    float nz = .25 * lB + .25 * lD + .25 * lF + .25 * lH - lE;
    float lMax = max(max(max(lB, lD), max(lE, lF)), lH);
    float lMin = min(min(min(lB, lD), min(lE, lF)), lH);
    float range = lMax - lMin;
    nz = saturate(abs(nz) * (range > 0.0 ? 1.0 / range : 0.0));
    nz = -.5 * nz + 1.0;

    // 环上 4 个邻居的最小/最大值（不是逐 tap，而是 4 倍极值，避免 MSAA 台阶误判）。
    vec3 mn4 = min(min(tB, tD), min(tF, tH));
    vec3 mx4 = max(max(tB, tD), max(tF, tH));

    // 峰值范围：1 - 4*x 的线性映射。
    const vec2 peakC = vec2(1.0, -4.0);
    // 限幅。分母在纯黑（mx4=0）或纯白（mn4=1）时为 0：
    // 夹到极小值后 hitMin/hitMax 自然得到 0，输出保持原像素。
    vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1e-8));
    vec3 hitMax = (peakC.x - mx4) / min(4.0 * mn4 + peakC.y, vec3(-1e-8));
    vec3 lobe = max(-hitMin, hitMax);
    // 取三通道最强的负瓣，限制在 [-FSR_RCAS_LIMIT, 0]，再乘锐度。
    lobe = max(vec3(-FSR_RCAS_LIMIT), min(vec3(max(max(lobe.r, lobe.g), lobe.b)), vec3(0.0))) *
           uCon0.x;
#ifdef FSR_RCAS_DENOISE
    // 按噪声检测结果减弱锐化。
    lobe *= nz;
#endif

    // 解算。4*lobe+1 在 [0.25, 1]，不会为零。
    vec3 rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 pix = (lobe * (tB + tD + tF + tH) + tE) * rcpL;
    fragColor = vec4(pix, 1);
}
