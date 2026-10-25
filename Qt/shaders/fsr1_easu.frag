#version 440
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"

// FSR1 EASU（Edge-Adaptive Spatial Upsampling）：输入渲染分辨率，输出显示分辨率。
//
// 公式来源：AMD FidelityFX SDK v1.1.4
//   FidelityFX-SDK/sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h
//     ffxFsrPopulateEasuConstants()  常量打包
//     ffxFsrEasuFloat()              12-tap 主循环
//     fsrEasuSetFloat()              梯度方向/长度
//     fsrEasuTapFloat()              Lanczos2 近似权重
//   经典 FSR1 v1.20210629 中对应 FsrEasuCon()/FsrEasuF()。
//
// 与 AMD 参考实现的差异（都是等价手段，不改变数学结果）：
//   1. AMD 用 gather4 取 2x2 纹素；GLSL 150/300es 没有 textureGather，
//      这里用 texelFetch 取 12 个唯一纹素，等价于把 4 次 gather 逐通道展开。
//   2. AMD 的 ffxApproximateReciprocal 是位运算近似，0 的倒数是有限大值；
//      GLSL 的 1.0/0.0 是 inf，0*inf 会变 NaN。所有可能为零的分母都显式判断。
//   3. 常量由 Qt/src/fsr/Fsr1Constants.h 按 AMD 的 4xvec4 布局生成。
//
// 方向：v_uv 是左上原点；rtUV() 把它换成纹理内存行序（GL 下翻转）。
// EASU 的 12 个 tap 都在纹理内存空间里取，所以整数坐标必须从 rtUV(v_uv) 推出。
layout(std140, binding = 1) uniform Fsr1Block {
    vec4 uCon0; // {inputW/outputW, inputH/outputH, .5*inputW/outputW-.5, .5*inputH/outputH-.5}
    vec4 uCon1; // {1/inputW, 1/inputH, 1/inputW, -1/inputH}
    vec4 uCon2; // {-1/inputW, 2/inputH, 1/inputW, 2/inputH}
    vec4 uCon3; // {0, 4/inputH, 0, 0}
};
layout(binding = 2) uniform sampler2D color;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// 取一个纹素。AMD 的 gather4 依赖采样器的 ClampToEdge 地址模式；
// texelFetch 不经过采样器，所以这里手动夹紧到纹理边界。
vec3 easuLoad(ivec2 size, ivec2 p) {
    return texelFetch(color, clamp(p, ivec2(0), size - 1), 0).rgb;
}

// fsrEasuTapFloat：把偏移旋转到梯度方向、按各向异性拉伸，再乘 Lanczos2 近似权重。
void easuTap(inout vec3 colorSum, inout float weightSum, vec2 offset, vec2 dir, vec2 len2,
             float lob, float clp, vec3 tapColor) {
    // 按梯度方向旋转偏移。
    vec2 r;
    r.x = offset.x * dir.x + offset.y * dir.y;
    r.y = offset.x * (-dir.y) + offset.y * dir.x;
    // 各向异性拉伸。
    r *= len2;
    // 距离平方。
    float d2 = r.x * r.x + r.y * r.y;
    // 角上的 tap 可能落到窗口外，夹到窗口末端。
    d2 = min(d2, clp);
    // Lanczos2 的无 sin/rcp/sqrt 近似：
    //   (25/16*(2/5*x^2-1)^2 - (25/16-1)) * (1/4*x^2-1)^2
    // 第二个因子的负瓣强度由 lob 控制。
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    // 加权平均。
    colorSum += tapColor * w;
    weightSum += w;
}

// fsrEasuSetFloat：累加一个双线性角的梯度方向和长度。
// AMD 用编译期 bool 选角；GLSL 直接传权重，四个角各调一次。
void easuSet(inout vec2 dir, inout float len, vec2 pp, float weight,
             float lA, float lB, float lC, float lD, float lE) {
    // 方向是 '+' 差分，长度取 'c' 两侧平均的绝对值。
    float dc = lD - lC;
    float cb = lC - lB;
    float lengthX = max(abs(dc), abs(cb));
    // 平坦区域 dc=cb=0：AMD 的近似倒数返回大有限值，这里取 0，结果同样是“无边缘”。
    lengthX = lengthX > 0.0 ? 1.0 / lengthX : 0.0;
    float directionX = lD - lB;
    dir.x += directionX * weight;
    lengthX = saturate(abs(directionX) * lengthX);
    lengthX *= lengthX;
    len += lengthX * weight;

    // y 轴重复一遍。
    float ec = lE - lC;
    float ca = lC - lA;
    float lengthY = max(abs(ec), abs(ca));
    lengthY = lengthY > 0.0 ? 1.0 / lengthY : 0.0;
    float directionY = lE - lA;
    dir.y += directionY * weight;
    lengthY = saturate(abs(directionY) * lengthY);
    lengthY *= lengthY;
    len += lengthY * weight;
}

void main() {
    // 归一化纹理坐标（后端方向修正）。
    vec2 uv = rtUV(v_uv);
    // AMD 的 pp = ip * con0.xy + con0.zw，ip 是输出整数像素。
    // 片元着色器没有现成的 ip：uv = (ip + .5) / outputSize，
    // 由 con0/con1 反推 outputSize = inputSize / con0.xy，取整即得 ip。
    // 反推误差远小于半个像素，不会改变取整结果。
    vec2 inputSize = 1.0 / uCon1.xy;
    vec2 outputSize = inputSize / uCon0.xy;
    ivec2 ip = ivec2(uv * outputSize);
    vec2 pp = vec2(ip) * uCon0.xy + uCon0.zw;
    vec2 fp = floor(pp);
    pp -= fp;

    ivec2 size = textureSize(color, 0);
    ivec2 p = ivec2(fp);
    // 12 个 tap，位置相对 'f'；对应 AMD 注释里的
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 tB = easuLoad(size, p + ivec2(0, -1));
    vec3 tC = easuLoad(size, p + ivec2(1, -1));
    vec3 tI = easuLoad(size, p + ivec2(-1, 1));
    vec3 tJ = easuLoad(size, p + ivec2(0, 1));
    vec3 tF = easuLoad(size, p);
    vec3 tE = easuLoad(size, p + ivec2(-1, 0));
    vec3 tK = easuLoad(size, p + ivec2(1, 1));
    vec3 tL = easuLoad(size, p + ivec2(2, 1));
    vec3 tH = easuLoad(size, p + ivec2(2, 0));
    vec3 tG = easuLoad(size, p + ivec2(1, 0));
    vec3 tO = easuLoad(size, p + ivec2(1, 2));
    vec3 tN = easuLoad(size, p + ivec2(0, 2));

    // 最省的多通道近似亮度（亮度 x2）。
    float lB = tB.b * .5 + (tB.r * .5 + tB.g);
    float lC = tC.b * .5 + (tC.r * .5 + tC.g);
    float lI = tI.b * .5 + (tI.r * .5 + tI.g);
    float lJ = tJ.b * .5 + (tJ.r * .5 + tJ.g);
    float lF = tF.b * .5 + (tF.r * .5 + tF.g);
    float lE = tE.b * .5 + (tE.r * .5 + tE.g);
    float lK = tK.b * .5 + (tK.r * .5 + tK.g);
    float lL = tL.b * .5 + (tL.r * .5 + tL.g);
    float lH = tH.b * .5 + (tH.r * .5 + tH.g);
    float lG = tG.b * .5 + (tG.r * .5 + tG.g);
    float lO = tO.b * .5 + (tO.r * .5 + tO.g);
    float lN = tN.b * .5 + (tN.r * .5 + tN.g);

    // 按双线性角累加；权重就是 pp 在该角上的双线性权重。
    vec2 dir = vec2(0);
    float len = 0;
    easuSet(dir, len, pp, (1 - pp.x) * (1 - pp.y), lB, lE, lF, lG, lJ);
    easuSet(dir, len, pp, pp.x * (1 - pp.y), lC, lF, lG, lH, lK);
    easuSet(dir, len, pp, (1 - pp.x) * pp.y, lF, lI, lJ, lK, lN);
    easuSet(dir, len, pp, pp.x * pp.y, lG, lJ, lK, lL, lO);

    // 归一化方向，并清理接近零的情况。
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zero = dirR < 1.0 / 32768.0;
    dirR = zero ? 1.0 : inversesqrt(dirR);
    dir.x = zero ? 1.0 : dir.x;
    dir *= dirR;

    // {0..2} -> {0..1}，再平方成形。
    len = len * .5;
    len *= len;

    // 对角方向把核拉伸到 sqrt(2)。
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    // 旋转后的各向异性长度：x 从 1 插值到 stretch，y 从 1 插值到 2 倍。
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - .5 * len);
    // 窗口从 +/-sqrt(2) 扩到略超 2。
    float lob = .5 + ((1.0 / 4.0 - .04) - .5) * len;
    // 距离平方的裁剪点设到窗口末端。
    float clp = 1.0 / lob;

    // 4 个最近邻的最小/最大值，用来做 dering。
    vec3 min4 = min(min(tF, tG), min(tJ, tK));
    vec3 max4 = max(max(tF, tG), max(tJ, tK));

    // 累加 12 个 tap。
    vec3 colorSum = vec3(0);
    float weightSum = 0;
    easuTap(colorSum, weightSum, vec2(0, -1) - pp, dir, len2, lob, clp, tB);
    easuTap(colorSum, weightSum, vec2(1, -1) - pp, dir, len2, lob, clp, tC);
    easuTap(colorSum, weightSum, vec2(-1, 1) - pp, dir, len2, lob, clp, tI);
    easuTap(colorSum, weightSum, vec2(0, 1) - pp, dir, len2, lob, clp, tJ);
    easuTap(colorSum, weightSum, vec2(0, 0) - pp, dir, len2, lob, clp, tF);
    easuTap(colorSum, weightSum, vec2(-1, 0) - pp, dir, len2, lob, clp, tE);
    easuTap(colorSum, weightSum, vec2(1, 1) - pp, dir, len2, lob, clp, tK);
    easuTap(colorSum, weightSum, vec2(2, 1) - pp, dir, len2, lob, clp, tL);
    easuTap(colorSum, weightSum, vec2(2, 0) - pp, dir, len2, lob, clp, tH);
    easuTap(colorSum, weightSum, vec2(1, 0) - pp, dir, len2, lob, clp, tG);
    easuTap(colorSum, weightSum, vec2(1, 2) - pp, dir, len2, lob, clp, tO);
    easuTap(colorSum, weightSum, vec2(0, 2) - pp, dir, len2, lob, clp, tN);

    // 归一化并做 dering（夹到 4 个最近邻的范围）。
    vec3 pix = clamp(colorSum / weightSum, min4, max4);
    fragColor = vec4(pix, 1);
}
