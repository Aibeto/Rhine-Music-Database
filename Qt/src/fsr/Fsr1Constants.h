#pragma once

// FSR1（EASU + RCAS）常量打包，供 QRhi 的 fragment uniform buffer 使用。
//
// 公式来源（两份头文件的数学完全一致，只是函数名不同）：
//   - 经典 FSR1 v1.20210629：output/ffx-install/share/rhine/reference/fsr1/ffx_fsr1.h
//       FsrEasuCon()  （行 156）
//       FsrRcasCon()  （行 662）
//   - 仓库内 FidelityFX SDK v1.1.4：FidelityFX-SDK/sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h
//       ffxFsrPopulateEasuConstants()（行 42，FsrEasuCon 的新名字）
//       FsrRcasCon()                 （行 661）
//
// AMD 的常量缓冲是 uint4：写入时用 asuint() 把 float 的位模式存成 uint，着色器里再用
// asfloat() 读回。本项目 uniform block 是 4 个 vec4，直接存 float 数值即可——
// asuint()/asfloat() 往返是恒等变换，着色器读到的仍是同一个 float，不需要再做位转换。
//
// 纯头文件：只依赖 QtCore 的 QSize、QtGui 的 QVector4D（都是值类型，不涉及 GUI 运行时）
// 和 <cmath>。RhiScene.cpp 可以直接 include。

#include <QtGlobal>
#include <QSize>
#include <QVector4D>
#include <cmath>

// 与 std140 的 4xvec4 布局一一对应：con0/con1/con2/con3 依次落在偏移 0/16/32/48。
struct Fsr1Con {
    QVector4D con0;
    QVector4D con1;
    QVector4D con2;
    QVector4D con3;
};
static_assert(sizeof(QVector4D) == 4 * sizeof(float), "QVector4D 必须是 4 个连续 float");
static_assert(sizeof(Fsr1Con) == 16 * sizeof(float), "Fsr1Con 必须正好是 4 个连续的 vec4");

namespace Fsr1Constants {

// EASU：输入渲染分辨率 -> 输出显示分辨率。
//
// 对应 FsrEasuCon(con0..con3,
//                 inputViewportInPixelsX/Y, inputSizeInPixelsX/Y,
//                 outputSizeInPixelsX/Y)。
// 本项目没有动态分辨率偏移，inputViewport 与 inputSize 相同，所以只需要两个尺寸；
// inputSize 必须与绑定的输入纹理尺寸一致（着色器会用它反推输出尺寸）。
inline Fsr1Con easu(const QSize &inputSize, const QSize &outputSize)
{
    const float iw = float(qMax(1, inputSize.width()));
    const float ih = float(qMax(1, inputSize.height()));
    const float ow = float(qMax(1, outputSize.width()));
    const float oh = float(qMax(1, outputSize.height()));
    // AMD 的 ARcpF1 是精确倒数（不是近似倒数）。
    const float rx = 1.f / ow;
    const float ry = 1.f / oh;
    const float ix = 1.f / iw;
    const float iy = 1.f / ih;

    Fsr1Con c;
    // 输出整数像素位置 -> 输入像素位置。
    c.con0 = QVector4D(iw * rx, ih * ry, .5f * iw * rx - .5f, .5f * ih * ry - .5f);
    // 输入像素位置 -> 归一化图像空间；gather4 的中心从 'F' 的左上角开始。
    c.con1 = QVector4D(ix, iy, ix, -iy);
    // 这些偏移从 (0) 而不是 'F' 算起。
    c.con2 = QVector4D(-ix, 2.f * iy, ix, 2.f * iy);
    c.con3 = QVector4D(0.f, 4.f * iy, 0.f, 0.f);
    return c;
}

// RCAS：锐化常量。
//
// 对应 FsrRcasCon(con, sharpness)：sharpness 以“档”为单位，
// 0 = 最大锐度，N > 0 表示锐度减半 N 次；内部换算成 exp2(-sharpness)。
// AMD 的 RCAS 是 3x3 空间滤波，与分辨率无关，outputSize 只用于保持调用点对称，
// 不参与常量计算。
inline Fsr1Con rcas(const QSize &outputSize, float sharpness)
{
    Q_UNUSED(outputSize);
    const float s = std::exp2(-sharpness);

    Fsr1Con c;
    // AMD con[0] = asuint(sharpness)；着色器里对应 uCon0.x。
    c.con0 = QVector4D(s, 0.f, 0.f, 0.f);
    // AMD con[1] = packHalf2x16({sharpness, sharpness})，只有 16 位路径会读；
    // 32 位路径不读，这里给出解包后的值，保持 4xvec4 布局与参考实现一致。
    c.con1 = QVector4D(s, s, 0.f, 0.f);
    c.con2 = QVector4D(0.f, 0.f, 0.f, 0.f);
    c.con3 = QVector4D(0.f, 0.f, 0.f, 0.f);
    return c;
}

} // namespace Fsr1Constants
