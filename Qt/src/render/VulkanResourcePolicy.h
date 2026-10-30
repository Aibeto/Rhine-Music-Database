#pragma once

#include <QtCore/QtGlobal>

namespace RhineRender {

/**
 * D02：动态 UBO arena 的纯策略。
 *
 * 只做算术与边界判断，不碰 QRhi，方便离线验证：这些公式错一位就是"越界读 GPU 内存"
 * 或者"驱动直接丢弃绑定"，而它们本来可以不出现在驱动里。
 */

/// 对齐后的元素步长。alignment 必须是 2 的幂（QRhi::ubufAlignment() 保证）；非法时返回 0，
/// 调用方据此回退到逐材质 buffer。
inline quint32 arenaStride(quint32 elementSize, quint32 alignment)
{
    if (elementSize == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return 0;
    }
    const quint64 stride = (quint64(elementSize) + alignment - 1) / alignment * alignment;
    if (stride > 0xffffffffull) {
        return 0;
    }
    return quint32(stride);
}

/**
 * 单次绑定的暴露范围是否合法：
 *  · range 不能超过 MaxUniformBufferRange（它限制的是"一次绑定暴露多少"，不是 arena 总量）；
 *  · offset 必须按对齐步长落位，offset + range 不能越过 buffer 容量，且加法不能溢出。
 */
inline bool dynamicOffsetFits(quint64 offset, quint64 range, quint64 capacity, quint64 maxRange,
                              quint64 alignment = 0)
{
    if (range == 0 || range > maxRange) {
        return false;
    }
    if (alignment > 1 && (offset % alignment) != 0) {
        return false;
    }
    if (offset > capacity || range > capacity - offset) {
        return false;
    }
    return true;
}

/// 至少装下 needSlots 个 stride 的容量（字节）；超过 maxBytes 返回 0，表示这一档不可用。
inline quint64 arenaCapacityFor(quint32 stride, quint32 needSlots, quint64 maxBytes)
{
    if (stride == 0 || needSlots == 0) {
        return 0;
    }
    const quint64 bytes = quint64(stride) * quint64(needSlots);
    if (bytes == 0 || bytes > maxBytes) {
        return 0;
    }
    return bytes;
}

} // namespace RhineRender
