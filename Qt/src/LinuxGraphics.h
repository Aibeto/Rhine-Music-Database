#pragma once
#include <QPair>
#include <QString>
#include <QVector>

/**
 * Linux 显卡选择。
 *
 * Windows 有 DXGI 加 QT_D3D_ADAPTER_INDEX 这一条直路，Linux 没有等价入口：
 * Qt 的 RHI 总是用枚举到的第一块卡，所以「选中第 N 块卡」只能翻译成各驱动认识的
 * 环境变量，让目标卡排到物理设备列表首位——
 *   - Mesa 系（i915 / xe / amdgpu / nouveau）：DRI_PRIME=pci-0000_01_00_0，
 *     对 OpenGL 与 Vulkan 都生效（Vulkan 侧由 VkLayer_MESA_device_select 实现）；
 *   - NVIDIA 专有驱动：__NV_PRIME_RENDER_OFFLOAD=1 启用 VK_LAYER_NV_optimus 图层，
 *     再由 __VK_LAYER_NV_optimus=NVIDIA_only / non_NVIDIA_only 决定哪一类卡排首位
 *     （NVIDIA 驱动 README 第 34 章：该图层只做排序，不隐藏设备）。
 *
 * 序号是 /sys/class/drm 里 cardN 的顺序（0 基），设置页显示与保存的都是它。
 * 显示型号在这里是错的：换过卡之后型号必然变化，而序号会在启动时被判定为失效并回退，
 * 这正是用户要求的语义。
 */
struct LinuxGpuDevice
{
    QString card;       ///< sysfs 里的名字，例如 "card0"
    QString pciAddress; ///< "0000:01:00.0"；没有 PCI 父设备的条目不会进入列表
    quint32 vendor = 0; ///< 0x10de / 0x8086 / 0x1002
    QString driver;     ///< nvidia / i915 / xe / amdgpu / nouveau

    /// 能定位到 PCI 地址才算可用：选卡变量全部以 PCI 地址或驱动名为依据。
    bool usable() const;
};

namespace RhineLinuxGpu {
/// 枚举 DRM 显卡；按 cardN 数字升序，`card0-DP-1` 这类连接器条目会被忽略。
QVector<LinuxGpuDevice> enumerateDevices(const QString &drmRoot = QStringLiteral("/sys/class/drm"));
/// 是否装了 NVIDIA 的 Vulkan ICD：装了才需要额外的排序变量把非 NVIDIA 卡顶到首位。
bool nvidiaIcdInstalled(const QString &icdRoot = QStringLiteral("/usr/share/vulkan/icd.d"));
/// "0000:01:00.0" → "pci-0000_01_00_0"（Mesa 的 DRI_PRIME 语法）；格式不符返回空。
QString pciPrimeSelector(const QString &pciAddress);
/**
 * 把序号翻译成要注入的环境变量。序号非法（越界、无法定位 PCI、列表为空）时返回空表，
 * 并通过 reason 说明原因——那正是"显卡已更换 / 已拔掉"的情形，调用方据此回退默认行为。
 */
QVector<QPair<QString, QString>> planEnvironment(const QVector<LinuxGpuDevice> &devices, int index,
                                                 bool nvidiaIcd, QString *reason = nullptr);
} // namespace RhineLinuxGpu

/**
 * 启动时调用，必须早于 QGuiApplication 与任何 QQuickWindow：RHI 在创建窗口时就定下用哪块卡。
 * 读 panel/gpu-index（或本次启动的 RHINE_GPU_INDEX），校验后注入环境变量；
 * 序号失效时不注入任何变量，保持驱动默认行为——与没有这个功能的历史版本完全一致。
 */
void initializeLinuxGraphics();
