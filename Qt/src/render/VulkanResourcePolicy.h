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


/// 实例池的容量规划：现有容量够用就原样返回，不够就按需翻倍（至少 4 MiB），
/// 超过 maxBytes 返回 0 表示这一档不可用（调用方回退到逐 draw buffer）。
inline quint64 instancePoolBytes(quint64 needBytes, quint64 currentBytes, quint64 maxBytes = 64ull << 20)
{
    const quint64 floorBytes = 4ull << 20;
    if (needBytes > maxBytes) {
        return 0;
    }
    if (currentBytes >= needBytes && currentBytes > 0) {
        return currentBytes;
    }
    quint64 bytes = currentBytes > 0 ? currentBytes : floorBytes;
    while (bytes < needBytes && bytes < maxBytes) {
        bytes *= 2;
    }
    if (bytes < needBytes || bytes > maxBytes) {
        return 0;
    }
    return bytes;
}

/**
 * 池化写是否需要真正上传：内容没变且落点没变才可跳过。
 * 只比内容不比起始偏移会让"顺序变了但单块内容相同"的帧读到上一位的数据。
 */
inline bool poolWriteNeeded(bool contentEqual, quint64 offset, quint64 previousOffset)
{
    return !contentEqual || offset != previousOffset;
}


/**
 * D04：深度是否必须保留。
 *
 * 只有确定没人读时才允许 DoNotStore；任一消费者存在就照旧 store。这里刻意保守：
 * 判断不了就当有人读——一次错误的 DONT_CARE 表现为"某一帧之后深度/时域全花"，很难回查。
 */
inline bool depthStoreRequired(bool motionConfigured, bool temporal, bool warpProbe, bool metalFxActive)
{
    return motionConfigured || temporal || warpProbe || metalFxActive;
}

} // namespace RhineRender
