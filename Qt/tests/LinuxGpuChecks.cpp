// Linux 显卡选择里能离线验证的那一半：sysfs 解析、序号 → 环境变量的映射、
// 以及"序号不存在"时必须是空表（启动侧据此回退默认行为）。
// 这些逻辑写错的后果是"选错卡"或"配置失效后仍然注入变量"，只有用例拦得住。
#include "LinuxGraphics.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QVector>
#include <cstdio>

namespace {

bool check(bool result, const char *message)
{
    if (!result) {
        std::fprintf(stderr, "%s\n", message);
    }
    return result;
}

QString valueOf(const QVector<QPair<QString, QString>> &environment, const QString &name)
{
    for (const auto &entry : environment) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return {};
}

LinuxGpuDevice gpu(const QString &card, const QString &pci, quint32 vendor, const QString &driver)
{
    LinuxGpuDevice device;
    device.card = card;
    device.pciAddress = pci;
    device.vendor = vendor;
    device.driver = driver;
    return device;
}

/// 三个平台都要过：序号映射只看设备表，与真实硬件无关。
bool mapsIndexToDriverVariables()
{
    const QVector<LinuxGpuDevice> devices{
        gpu(QStringLiteral("card0"), QStringLiteral("0000:01:00.0"), 0x10de, QStringLiteral("nvidia")),
        gpu(QStringLiteral("card1"), QStringLiteral("0000:00:02.0"), 0x8086, QStringLiteral("i915")),
        gpu(QStringLiteral("card2"), QStringLiteral("0000:03:00.0"), 0x1002, QStringLiteral("amdgpu"))};
    bool ok = true;

    // 选中 NVIDIA：专有驱动不读 DRI_PRIME，只能靠 PRIME 离屏渲染 + 图层把它排到首位。
    const auto nvidia = RhineLinuxGpu::planEnvironment(devices, 0, true);
    ok &= check(valueOf(nvidia, QStringLiteral("__NV_PRIME_RENDER_OFFLOAD")) == QLatin1String("1"),
                "选中 NVIDIA 时必须启用 PRIME 离屏渲染");
    ok &= check(valueOf(nvidia, QStringLiteral("__VK_LAYER_NV_optimus")) == QLatin1String("NVIDIA_only"),
                "选中 NVIDIA 时图层必须把 NVIDIA 卡排到首位");
    ok &= check(valueOf(nvidia, QStringLiteral("DRI_PRIME")).isEmpty(),
                "NVIDIA 专有驱动不该收到 DRI_PRIME");

    // 选中 Intel 核显且系统装了 NVIDIA ICD：选卡 + 让非 NVIDIA 排首位。
    const auto intel = RhineLinuxGpu::planEnvironment(devices, 1, true);
    ok &= check(valueOf(intel, QStringLiteral("DRI_PRIME")) == QLatin1String("pci-0000_00_02_0"),
                "Mesa 卡必须用 PCI 地址形式的 DRI_PRIME");
    ok &= check(valueOf(intel, QStringLiteral("__VK_LAYER_NV_optimus")) == QLatin1String("non_NVIDIA_only"),
                "装了 NVIDIA ICD 时必须把非 NVIDIA 卡排到首位");

    // 没有 NVIDIA ICD 时不该多此一举注入图层变量。
    const auto intelOnly = RhineLinuxGpu::planEnvironment(devices, 1, false);
    ok &= check(valueOf(intelOnly, QStringLiteral("DRI_PRIME")) == QLatin1String("pci-0000_00_02_0"),
                "没有 NVIDIA ICD 时 DRI_PRIME 仍要注入");
    ok &= check(valueOf(intelOnly, QStringLiteral("__VK_LAYER_NV_optimus")).isEmpty(),
                "没有 NVIDIA ICD 时不该注入图层变量");

    // 序号不存在（换卡 / 拔卡 / 配置写坏）：空表 + 原因，启动侧据此保持驱动默认。
    QString reason;
    ok &= check(RhineLinuxGpu::planEnvironment(devices, 3, true, &reason).isEmpty(),
                "越界序号必须回退");
    ok &= check(!reason.isEmpty(), "回退时必须留下原因，日志才有据可查");
    ok &= check(RhineLinuxGpu::planEnvironment(devices, -1, true).isEmpty(), "负序号必须回退");
    ok &= check(RhineLinuxGpu::planEnvironment({}, 0, true).isEmpty(), "空设备表必须回退");

    // 读不出 vendor 的条目没有选择手段，也不能被选中。
    const QVector<LinuxGpuDevice> broken{gpu(QStringLiteral("card9"), QStringLiteral("0000:09:00.0"), 0,
                                             QStringLiteral("nvidia"))};
    ok &= check(RhineLinuxGpu::planEnvironment(broken, 0, true).isEmpty(), "缺少 vendor 的条目必须回退");
    return ok;
}

bool convertsPciAddress()
{
    bool ok = true;
    ok &= check(RhineLinuxGpu::pciPrimeSelector(QStringLiteral("0000:01:00.0")) ==
                    QLatin1String("pci-0000_01_00_0"),
                "PCI 地址必须换成 Mesa 的 pci- 形式");
    ok &= check(RhineLinuxGpu::pciPrimeSelector(QStringLiteral("0000:00:02.0")) ==
                    QLatin1String("pci-0000_00_02_0"),
                "核显的 PCI 地址同样要转换");
    // 来自 sysfs 以外的东西（或读文件时没去掉换行）一律拒绝，绝不猜。
    ok &= check(RhineLinuxGpu::pciPrimeSelector(QStringLiteral("0000:01:00.0\n")).isEmpty(),
                "带换行的地址必须被拒绝");
    ok &= check(RhineLinuxGpu::pciPrimeSelector(QStringLiteral("nvidia")).isEmpty(),
                "非 PCI 地址必须被拒绝");
    ok &= check(RhineLinuxGpu::pciPrimeSelector({}).isEmpty(), "空地址必须被拒绝");
    return ok;
}

#if defined(Q_OS_LINUX)
bool writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(content) == content.size();
}

/// 造一台多卡机器：card0 = NVIDIA 专有驱动、card1 = Intel 核显、card2 = AMD 独显、
/// card10 = virtio 显卡（用来验证序号按数字排序，而不是按字符串）。
/// 同时放两个必须被忽略的条目：显示连接器 card0-DP-1，以及没有 PCI 父设备的 card3。
bool makeGpu(const QString &root, const QString &card, const QString &pci, const QString &vendor,
             const QString &driver)
{
    const auto devicePath = root + QStringLiteral("/pci/") + pci;
    if (!QDir().mkpath(devicePath) || !QDir().mkpath(root + QStringLiteral("/drivers/") + driver) ||
        !QDir().mkpath(root + QStringLiteral("/drm/") + card)) {
        return false;
    }
    if (!writeFile(devicePath + QStringLiteral("/vendor"), vendor.toLatin1() + '\n')) {
        return false;
    }
    const auto deviceLink = root + QStringLiteral("/drm/") + card + QStringLiteral("/device");
    if (!QFile::link(devicePath, deviceLink)) {
        return false;
    }
    return QFile::link(root + QStringLiteral("/drivers/") + driver, deviceLink + QStringLiteral("/driver"));
}

bool parsesSysfs()
{
    QTemporaryDir root;
    if (!root.isValid()) {
        return check(false, "临时目录创建失败");
    }
    bool ok = true;
    const auto base = root.path();
    ok &= check(makeGpu(base, QStringLiteral("card0"), QStringLiteral("0000:01:00.0"),
                        QStringLiteral("0x10de"), QStringLiteral("nvidia")),
                "构造 card0 失败");
    ok &= check(makeGpu(base, QStringLiteral("card1"), QStringLiteral("0000:00:02.0"),
                        QStringLiteral("0x8086"), QStringLiteral("i915")),
                "构造 card1 失败");
    ok &= check(makeGpu(base, QStringLiteral("card2"), QStringLiteral("0000:03:00.0"),
                        QStringLiteral("0x1002"), QStringLiteral("amdgpu")),
                "构造 card2 失败");
    ok &= check(makeGpu(base, QStringLiteral("card10"), QStringLiteral("0000:04:00.0"),
                        QStringLiteral("0x1af4"), QStringLiteral("virtio_gpu")),
                "构造 card10 失败");
    // 连接器条目与没有 PCI 父设备的条目都不算显卡。
    ok &= check(QDir().mkpath(base + QStringLiteral("/drm/card0-DP-1")), "构造连接器条目失败");
    ok &= check(QDir().mkpath(base + QStringLiteral("/drm/card3/device")), "构造无 vendor 条目失败");

    const auto devices = RhineLinuxGpu::enumerateDevices(base + QStringLiteral("/drm"));
    // 序号按 cardN 的数字排序：card2 必须在 card10 前面，字符串排序会把两者顺序颠倒。
    ok &= check(devices.size() == 4, "只应识别出四块 PCI 显卡（连接器与无 vendor 条目被忽略）");
    if (devices.size() == 4) {
        ok &= check(devices.at(0).card == QLatin1String("card0") &&
                        devices.at(0).pciAddress == QLatin1String("0000:01:00.0") &&
                        devices.at(0).vendor == 0x10de &&
                        devices.at(0).driver == QLatin1String("nvidia"),
                    "序号 0 应是 NVIDIA 卡");
        ok &= check(devices.at(1).vendor == 0x8086 && devices.at(1).driver == QLatin1String("i915"),
                    "序号 1 应是 Intel 核显");
        ok &= check(devices.at(2).card == QLatin1String("card2") &&
                        devices.at(2).pciAddress == QLatin1String("0000:03:00.0"),
                    "序号 2 应是 card2，数字排序不能让 card10 插到它前面");
        ok &= check(devices.at(3).card == QLatin1String("card10") &&
                        devices.at(3).pciAddress == QLatin1String("0000:04:00.0"),
                    "序号 3 应是 card10");
    }
    ok &= check(RhineLinuxGpu::enumerateDevices(base + QStringLiteral("/missing")).isEmpty(),
                "不存在的 sysfs 根目录必须返回空表");

    // ICD 探测：文件名带 nvidia 的清单才算装了 NVIDIA 驱动。
    ok &= check(!RhineLinuxGpu::nvidiaIcdInstalled(base + QStringLiteral("/icd")),
                "目录不存在时必须认为没装 NVIDIA ICD");
    ok &= check(QDir().mkpath(base + QStringLiteral("/icd")), "构造 ICD 目录失败");
    ok &= check(writeFile(base + QStringLiteral("/icd/intel_icd.json"), "{}"),
                "写入 Intel ICD 失败");
    ok &= check(!RhineLinuxGpu::nvidiaIcdInstalled(base + QStringLiteral("/icd")),
                "只有 Intel ICD 时不该认为装了 NVIDIA 驱动");
    ok &= check(writeFile(base + QStringLiteral("/icd/nvidia_icd.json"), "{}"),
                "写入 NVIDIA ICD 失败");
    ok &= check(RhineLinuxGpu::nvidiaIcdInstalled(base + QStringLiteral("/icd")),
                "有 nvidia_icd.json 时必须认为装了 NVIDIA 驱动");
    return ok;
}
#endif

} // namespace

int main()
{
    bool ok = true;
    ok &= mapsIndexToDriverVariables();
    ok &= convertsPciAddress();
#if defined(Q_OS_LINUX)
    ok &= parsesSysfs();
#endif
    return ok ? 0 : 1;
}
