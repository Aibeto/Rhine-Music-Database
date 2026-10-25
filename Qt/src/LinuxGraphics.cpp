#include "LinuxGraphics.h"

#include "AppStorage.h"
#include "GraphicsBackend.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtGlobal>
#include <algorithm>

bool LinuxGpuDevice::usable() const
{
    // vendor 为 0 说明没读出 PCI 供应商（simpledrm、虚拟显示等）：
    // 这种条目既没有可用的选择手段，也不该占掉一个序号。
    return vendor != 0 && !pciAddress.isEmpty();
}

namespace {
/// sysfs 的规范 PCI 地址（0000:01:00.0）。用 \A..\z 而不是 ^..$：PCRE 的 $ 允许结尾多一个换行，
/// 那样刚从文件里读出来的 "0000:01:00.0\n" 会被当成合法值。
const QRegularExpression &pciPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]\\z"));
    return pattern;
}

QString readTrimmed(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromLatin1(file.readAll()).trimmed();
}

/// 取符号链接目标的名字：card0/device → "0000:01:00.0"，card0/device/driver → "nvidia"。
QString linkTargetName(const QString &path)
{
    const auto target = QFileInfo(path).symLinkTarget();
    return target.isEmpty() ? QString() : QFileInfo(target).fileName();
}
} // namespace

QVector<LinuxGpuDevice> RhineLinuxGpu::enumerateDevices(const QString &drmRoot)
{
    const QDir root(drmRoot);
    if (!root.exists()) {
        return {};
    }
    QVector<QPair<int, LinuxGpuDevice>> ordered;
    const auto entries =
        root.entryList({QStringLiteral("card*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &entry : entries) {
        // "card0" → 0；"card0-DP-1" 这类连接器条目在这里转换失败，正好被排除。
        bool numeric = false;
        const int cardNumber = entry.mid(4).toInt(&numeric);
        if (!numeric) {
            continue;
        }
        const auto devicePath = root.filePath(entry + QStringLiteral("/device"));
        const auto vendor = readTrimmed(devicePath + QStringLiteral("/vendor"));
        if (vendor.size() < 3 || !vendor.startsWith(QLatin1String("0x"))) {
            continue; // 没有 PCI 父设备，不参与选卡
        }
        LinuxGpuDevice device;
        device.card = entry;
        device.pciAddress = linkTargetName(devicePath);
        device.vendor = vendor.mid(2).toUInt(nullptr, 16);
        device.driver = linkTargetName(devicePath + QStringLiteral("/driver"));
        ordered.append({cardNumber, device});
    }
    // 序号按 cardN 数字升序：字符串排序会把 card10 排到 card2 前面。
    std::sort(ordered.begin(), ordered.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    QVector<LinuxGpuDevice> devices;
    devices.reserve(ordered.size());
    for (const auto &entry : ordered) {
        devices.append(entry.second);
    }
    return devices;
}

bool RhineLinuxGpu::nvidiaIcdInstalled(const QString &icdRoot)
{
    const QDir root(icdRoot);
    if (!root.exists()) {
        return false;
    }
    // 清单文件名随发行版与架构变化（nvidia_icd.json、nvidia_icd.x86_64.json），按名字匹配即可。
    return !root.entryList({QStringLiteral("*nvidia*.json")}, QDir::Files).isEmpty();
}

QString RhineLinuxGpu::pciPrimeSelector(const QString &pciAddress)
{
    if (!pciPattern().match(pciAddress).hasMatch()) {
        return {};
    }
    // Mesa 文档的语法是 DRI_PRIME=pci-0000_02_00_0：冒号与点换成下划线。
    auto selector = pciAddress;
    selector.replace(QLatin1Char(':'), QLatin1Char('_'));
    selector.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("pci-") + selector;
}

QVector<QPair<QString, QString>> RhineLinuxGpu::planEnvironment(const QVector<LinuxGpuDevice> &devices,
                                                               int index, bool nvidiaIcd, QString *reason)
{
    const auto refuse = [reason](const QString &why) {
        if (reason) {
            *reason = why;
        }
        return QVector<QPair<QString, QString>>{};
    };
    if (index < 0) {
        return refuse(QStringLiteral("no GPU index was requested"));
    }
    if (index >= devices.size()) {
        return refuse(QStringLiteral("GPU %1 is not present (%2 detected)").arg(index).arg(devices.size()));
    }
    const auto &device = devices.at(index);
    if (!device.usable()) {
        return refuse(QStringLiteral("GPU %1 has no PCI address, so it cannot be selected").arg(index));
    }
    const auto selector = pciPrimeSelector(device.pciAddress);
    if (selector.isEmpty()) {
        return refuse(
            QStringLiteral("GPU %1 reports an unexpected PCI address \"%2\"").arg(index).arg(device.pciAddress));
    }

    QVector<QPair<QString, QString>> environment;
    if (device.driver == QLatin1String("nvidia")) {
        // 专有驱动不读 DRI_PRIME：先启用 PRIME 离屏渲染，再让图层把 NVIDIA 卡排到首位。
        environment.append({QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("1")});
        environment.append({QStringLiteral("__VK_LAYER_NV_optimus"), QStringLiteral("NVIDIA_only")});
        // 只有 X11 的 GLX 读它；Wayland 会忽略，留着不影响。
        environment.append({QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("nvidia")});
        return environment;
    }
    // Mesa 卡：device-select 图层会把这块卡排到物理设备列表首位，Vulkan 与 OpenGL 共用这条路径。
    environment.append({QStringLiteral("DRI_PRIME"), selector});
    if (nvidiaIcd) {
        // 装了 NVIDIA ICD 时列表里还躺着那块卡，需要图层把非 NVIDIA 的排到它前面。
        environment.append({QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("1")});
        environment.append({QStringLiteral("__VK_LAYER_NV_optimus"), QStringLiteral("non_NVIDIA_only")});
    }
    return environment;
}

void initializeLinuxGraphics()
{
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariableIntValue("RHINE_SOFTWARE_GPU") == 1) {
        // 与 Windows 侧一致：用户点名要软件渲染时，不要替他挑卡。
        qInfo() << "RHINE_SOFTWARE_GPU is set; leaving Linux GPU selection to the driver.";
        return;
    }

    const auto devices = RhineLinuxGpu::enumerateDevices();
    // 设置页只显示序号，枚举结果必须趁启动时定好；RhineCore 读不到 sysfs。
    RhineGraphics::setGpuDeviceCount(static_cast<int>(devices.size()));
    RhineGraphics::setGpuAppliedIndex(-1);
    for (int index = 0; index < devices.size(); ++index) {
        const auto &device = devices.at(index);
        qInfo().noquote() << QStringLiteral("GPU %1: pci=%2 vendor=0x%3 driver=%4")
                                 .arg(index)
                                 .arg(device.pciAddress)
                                 .arg(device.vendor, 4, 16, QLatin1Char('0'))
                                 .arg(device.driver.isEmpty() ? QStringLiteral("-") : device.driver);
    }

    const auto overrideText = qEnvironmentVariable("RHINE_GPU_INDEX").trimmed();
    const auto gpuSettings = AppStorage::settings();
    // 这里的读取发生在 QGuiApplication 之前：把落点打进日志，万一将来配置路径算错，
    // 症状会立刻表现为"设置页保存了但启动读不到"，一眼就能对上。
    qInfo().noquote() << "GPU preference file:" << gpuSettings.fileName();
    const auto storedText = gpuSettings.value(QStringLiteral("panel/gpu-index")).toString().trimmed();
    QString requested = storedText;
    QString source = QStringLiteral("panel/gpu-index");
    if (!overrideText.isEmpty()) {
        // 显式环境变量只对本次启动生效，不写回配置——与 RHINE_GRAPHICS 的约定一致。
        requested = overrideText;
        source = QStringLiteral("RHINE_GPU_INDEX");
    }
    if (requested.isEmpty()) {
        return; // 没有选择：完全保持驱动的默认行为
    }

    bool numeric = false;
    const int index = requested.toInt(&numeric);
    if (!numeric) {
        qWarning().noquote()
            << QStringLiteral("Ignoring non-numeric GPU index \"%1\" from %2").arg(requested, source);
        return;
    }

    QString reason;
    const auto environment =
        RhineLinuxGpu::planEnvironment(devices, index, RhineLinuxGpu::nvidiaIcdInstalled(), &reason);
    if (environment.isEmpty()) {
        // 换过卡、拔了 eGPU、或配置被写坏：不注入任何变量，行为回到历史版本的默认路径。
        qWarning().noquote()
            << QStringLiteral("GPU selection from %1 is not applied: %2").arg(source, reason);
        return;
    }

    for (const auto &entry : environment) {
        const auto name = entry.first.toUtf8();
        const auto value = entry.second.toUtf8();
        if (!qEnvironmentVariableIsEmpty(name.constData())) {
            // 外面已经显式指定的变量优先：便于用 DRI_PRIME=... 之类做对照实验。
            qInfo().noquote() << QStringLiteral("Keeping %1 from the environment").arg(entry.first);
            continue;
        }
        qputenv(name.constData(), value);
    }

    if (devices.at(index).driver == QLatin1String("nvidia")) {
        const auto nvidiaCount =
            std::count_if(devices.cbegin(), devices.cend(), [](const LinuxGpuDevice &device) {
                return device.driver == QLatin1String("nvidia");
            });
        if (nvidiaCount > 1) {
            // 排序只能表达"要 NVIDIA"或"不要 NVIDIA"，两张 NVIDIA 卡之间它区分不了。
            qWarning() << "Multiple NVIDIA GPUs detected; the NVIDIA layer cannot tell them apart.";
        }
    }

    RhineGraphics::setGpuAppliedIndex(index);
    qInfo().noquote() << QStringLiteral("Selected GPU %1 (pci=%2 driver=%3) from %4")
                             .arg(index)
                             .arg(devices.at(index).pciAddress)
                             .arg(devices.at(index).driver)
                             .arg(source);
#endif
}
