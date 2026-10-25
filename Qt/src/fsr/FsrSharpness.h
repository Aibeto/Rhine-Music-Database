#pragma once
#include <QSize>
#include <algorithm>
#include <cmath>
/**
 * 锐化强度按实际放大倍数收敛。
 *
 * FSR 的 EASU/RCAS 在放大倍率越高时，边缘超调越容易被放大成亮环与散点：内部按 1/4 分辨率
 * 渲染再放大 4 倍时，封面页上的装饰细线先被打碎，80% 的锐化再把碎片拉成一圈亮点——这就是
 * 暗色封面上"边缘锯齿带光点"的来源。用户设置值本身不改（设置页显示的仍是原始百分比），
 * 只在运行时按 1/√放大倍数 衰减：1 倍（原生 AA）保持原值、2 倍约 71%、3 倍约 58%、4 倍 50%，
 * 8 倍 35%。
 *
 * 之所以用平方根而不是线性：RCAS 的超调幅度随放大倍数近似线性增长，而人眼对边缘亮环的
 * 感知是对数量级的，取平方根能在压制亮环的同时保住大部分锐度手感。
 */
namespace RhineFsr {
inline double sharpnessScaleForUpscale(QSize internal, QSize display)
{
    if (internal.isEmpty() || display.isEmpty() || internal.width() <= 0
        || display.width() <= internal.width()) {
        return 1.0;
    }
    const double ratio = double(display.width()) / double(internal.width());
    return std::clamp(1.0 / std::sqrt(ratio), 0.25, 1.0);
}
/** 把设置里的锐化百分比换算成当前放大倍数下的有效百分比。 */
inline int effectiveSharpnessPercent(int percent, QSize internal, QSize display)
{
    const int bounded = std::clamp(percent, 0, 100);
    const double scaled = double(bounded) * sharpnessScaleForUpscale(internal, display);
    return int(std::lround(std::clamp(scaled, 0.0, 100.0)));
}
} // namespace RhineFsr
