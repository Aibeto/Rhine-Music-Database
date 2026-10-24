#include "RhiScene.h"
#include "DiscGeometry.h"
#include "Engine.h"
#include "Glyphs.h"
#include "fsr/Fsr1Constants.h"
#include "fsr/Fsr3Backend.h"
#include <QFile>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QPainter>
// Qt 6.8+ 的 qrhi.h 末尾会引入 rhi/qrhi_platform.h，各后端初始化参数由那里声明；
// 但其中 Vulkan 结构体要求 (QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>))，
// 只看 Qt 特性会在无 Vulkan SDK 的机器上编译出引用未声明类型的代码（Windows runner 即如此，
// 报 QVulkanInstance 不完整类型、QRhiVulkanInitParams 与 VK_SUCCESS 未定义）。
#include <rhi/qrhi.h>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>) && !defined(Q_OS_MACOS)
#include <QVulkanFunctions>
#include <QVulkanInstance>
#define RHINE_HAVE_VULKAN_TYPES 1
#else
#define RHINE_HAVE_VULKAN_TYPES 0
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

// 同一渲染通道和 GLSL 方程经 Qt Shader Tools 编译为 Metal / HLSL / SPIR-V。
struct PackedMatrix {
    float elements[16];
    PackedMatrix(const QMatrix4x4 &value = QMatrix4x4()) {
        std::copy_n(value.constData(), 16, elements);
    }
};
struct Globals {
    PackedMatrix view, projection, shadow, inverseProjection, aoProjection, aoInverseProjection;
    QVector4D eye, fogColor, fog, viewport, clip;
    PackedMatrix correction;
    QVector4D backend;
};
struct Surface {
    QVector4D color, physical, optics, attenuation, extra, flags, darkColor, atlas;
};
struct Instance {
    PackedMatrix model;
    QVector4D state;
};
static_assert(sizeof(Globals) == 544);
static_assert(sizeof(Surface) == 128);
static_assert(sizeof(Instance) == 80);
// 封面按源图最高取 1024²，带 mip 链后单张约 5.3 MiB；预算过小时每切一首都要重传并重建 mip。
static constexpr qint64 MUSIC_TEXTURE_BUDGET_BYTES = 64 * 1024 * 1024;
static QMatrix4x4 matrix(QJsonArray a, int offset = 0) {
    QMatrix4x4 m;
    for (int i = 0; i < 16; ++i) {
        m.data()[i] = float(a[offset + i].toDouble());
    }
    return m;
}
// FSR3 亚像素抖动：Engine 把像素偏移写进 frame["jitter"]（x 向右、y 向下，单位像素）。
// 偏移只作用在投影矩阵的平移项上：投影 (0,2)/(1,2) 对 NDC 的贡献分别为 -1/+1，
// 因此 x 取负、y 取正。相机与 CPU 侧拾取/模拟完全不受影响；关闭时该键不存在，矩阵不变。
static void applyJitter(QMatrix4x4 &projection, QPointF offset, QSize size) {
    if (offset.isNull() || size.isEmpty()) {
        return;
    }
    projection(0, 2) -= float(2 * offset.x() / size.width());
    projection(1, 2) += float(2 * offset.y() / size.height());
}
static QVector3D vector(QJsonValue a) {
    auto v = a.toArray();
    return {float(v[0].toDouble()), float(v[1].toDouble()), float(v[2].toDouble())};
}
static float n(QJsonObject o, const char *k) {
    return float(o[k].toDouble());
}
static QVector4D v4(QVector3D v, float w = 0) {
    return {v, w};
}
static QByteArray read(QString root, QString name) {
    QFile f(root + "/" + name);
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("Missing render resource: %s", qPrintable(name));
    }
    return f.readAll();
}
static QShader shader(QString name) {
    QFile f(":/shaders/" + name + ".qsb");
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("Missing baked shader: %s", qPrintable(name));
    }
    auto s = QShader::fromSerialized(f.readAll());
    if (!s.isValid()) {
        qFatal("Invalid shader");
    }
    return s;
}
struct Mesh {
    QRhiBuffer *vertices = nullptr, *indices = nullptr;
    QJsonArray parts;
};
struct Target {
    QRhiTexture *texture = nullptr;
    QRhiTextureRenderTarget *rt = nullptr;
    QRhiRenderPassDescriptor *rp = nullptr;
};
/**
 * FSR 统一模式。设置页只有一个 "fsr" 选项，内部按后端能力自动选择实现：
 * 能用 FFX 计算后端时走 FSR3，否则走 FSR1 的 EASU/RCAS 片元实现。
 * 旧的 fsr1 / fsr3 取值继续接受，避免升级后已保存的偏好失效。
 */
static bool isFsrMode(const QString &aaMode) {
    return aaMode == "fsr" || aaMode == "fsr1" || aaMode == "fsr3";
}
/**
 * FSR 超分档位对应的内部渲染缩小倍数。
 * 档位决定"按窗口分辨率的几分之一渲染"，再由 FSR 升采样回窗口分辨率：
 * 超级性能 1/3、性能 1/2、平衡 1/1.7、高质量 1/1.5、原生 AA 1/1（不缩小）。
 * 非 FSR 模式返回 1，不影响既有的超采样/渲染分辨率链路。
 */
static double fsrQualityDivisor(const QString &aaMode) {
    if (!isFsrMode(aaMode)) {
        return 1.;
    }
    const auto quality = qEnvironmentVariable("RHINE_FSR_QUALITY");
    if (quality == "super") {
        return 3.;
    }
    if (quality == "performance") {
        return 2.;
    }
    if (quality == "balanced") {
        return 1.7;
    }
    if (quality == "quality") {
        return 1.5;
    }
    // native（含未设置）：不缩小，等价于原生 AA。
    return 1.;
}
/**
 * FSR 锐化强度 0–100，默认 50。
 * 两个后端的方向相反，换算各自在调用点完成：
 *   FSR3 的 sharpness 是 0=不锐化、1=最锐；
 *   FSR1 的 RCAS 以"档"为单位，0=最锐、值越大越柔。
 */
static int fsrSharpnessPercent() {
    if (qEnvironmentVariableIsEmpty("RHINE_FSR_SHARPNESS")) {
        return 50;
    }
    return qBound(0, qEnvironmentVariableIntValue("RHINE_FSR_SHARPNESS"), 100);
}
class Renderer {
  public:
    QRhi *rhi;
    QString root;
    std::vector<std::unique_ptr<QRhiResource>> resources, sized, transient;
    QHash<QString, Mesh> meshes;
    QRhiTexture *environment = nullptr, *dfg = nullptr, *white = nullptr, *noise = nullptr,
                *capture = nullptr, *depth = nullptr, *normalDepth = nullptr, *shadow = nullptr;
    QRhiBuffer *globalBuffer = nullptr, *kernel = nullptr, *fsr1Constants = nullptr,
               *floor = nullptr, *coverPlane = nullptr,
               *musicLabelPlane = nullptr, *discBody = nullptr, *discHub = nullptr,
               *caseDecoration = nullptr, *caseLetteringFront = nullptr, *caseLetteringSpine = nullptr;
    int discPrintCount = 0;
    int discBodyCount = 0;
    int discHubCount = 0;
    QRhiTexture *decorationTextureHigh = nullptr, *decorationTextureLow = nullptr,
                *placeholderCover = nullptr, *letteringTextureFront = nullptr,
                *letteringTextureSpine = nullptr;
    // 标签图集页：每页 8×18 个 256×112 槽位，列表标签批次按页合并。
    struct AtlasPage {
        QRhiTexture *texture;
        QImage staging;
        QVector<int> freeCells;
        // 有槽位写入后需要重建 mip 链；每页每帧最多重建一次。
        bool dirty = false;
    };
    struct LabelSlotRecord {
        int page = -1;
        int cell = -1;
    };
    QList<AtlasPage> atlasPages;
    QHash<QString, LabelSlotRecord> labelSlots;
    QRhiSampler *linear = nullptr, *nearest = nullptr, *mipSampler = nullptr, *repeat = nullptr,
                *shadowSampler = nullptr;
    Target color, captureTarget, captureHalf, captureQuarter, captureEighth, normal, ao, bokeh, ldr, target;
    // FSR3 运动矢量目标：RGBA16F、与 color 同尺寸，复用 color 的 D32F 深度做可见性测试。
    Target velocity;
    // FSR3 超分的输入与输出。输入是渲染分辨率的色调映射结果（替代直接合成到窗口），
    // 输出是显示分辨率的升采样结果，再经一次拷贝落到窗口或读回目标。
    Target fsrInput, fsrOutput;
    std::unique_ptr<Fsr3Backend> fsr3;
    // 上一帧写入 FSR3 的抖动偏移；投影矩阵与本值必须同源，否则重投影会偏。
    QPointF jitterPixels;
    // FSR3 的帧间隔由渲染器自己计时：帧协议里没有时间戳，而 frameTimeDelta 只影响
    // 时间性启发式，用实测间隔比用固定 16.67 更稳。
    QElapsedTimer fsrClock;
    // MSAA：几何 pass 的采样数（1 表示关闭）。只有 color/depth/velocity 三个目标变多重
    // 采样，全屏通道始终单采样，所以采样数是渲染器级状态而不是逐目标状态。
    int sampleCount = 1;
    // 设备支持的采样数列表，构造时查一次（渲染器生命周期内设备不会变）。
    QList<int> supportedSamples;
    // TAA 历史：与 color 同尺寸的 RGBA16F 双缓冲。taaIndex 指向最近一次有效历史所在的
    // 缓冲，taaValid 为假表示内容未定义——首帧、尺寸/模式变化和 temporalReset 之后
    // 都必须重新播种，否则会把上一场景的画面拖进来。
    Target taaHistory[2];
    int taaIndex = 0;
    bool taaValid = false;
    QRhiTextureRenderTarget *loaded = nullptr, *shadowTarget = nullptr;
    QRhiRenderPassDescriptor *shadowPass = nullptr;
    QRhiResourceUpdateBatch *updates = nullptr;
    QHash<quint64, QRhiGraphicsPipeline *> pipelines;
    QHash<QString, QRhiShaderResourceBindings *> pipelineLayouts;
    struct BufferState {
        QRhiBuffer *buffer = nullptr;
        QByteArray bytes;
        quint64 used = 0;
    };
    struct MaterialState {
        BufferState uniform;
        QRhiShaderResourceBindings *bindings = nullptr;
        QRhiTexture *label = nullptr, *cover = nullptr;
        quint64 used = 0;
    };
    QHash<int, BufferState> instances;
    // 速度 pass 的实例缓冲：每实例 128 字节 = 当前帧模型矩阵 + 上一帧模型矩阵。
    QHash<int, BufferState> velocityInstances;
    // draw id → 上一帧模型矩阵（每实例 64 字节）。draw id 在帧间稳定，作为这里的主键。
    QHash<int, QByteArray> previousModels;
    QHash<QString, MaterialState> materials;
    QHash<QString, QRhiShaderResourceBindings *> postBindings;
    QHash<QString, QString> labelRevisions;
    QHash<QString, qint64> coverRevisions;
    SceneDrawBuffers nativeBuffers;
    quint64 generation = 0;
    quint64 bindingCreations = 0;
    quint64 coverUploads = 0;
    quint64 drawCalls = 0;
    QByteArray shadowRevision;
    bool shadowValid = false;
    QHash<QString, QRhiTexture *> labels, covers;
    QHash<qint64, QRhiTexture *> coverTextures;
    QRhiTexture *labelTexture = nullptr, *coverTexture = nullptr;
    QStringList textureOrder;
    QSet<QString> activeTextureKeys;
    QSize size;
    // 上一次重建渲染目标时用的 AA 模式；模式变化同样触发重建。
    QString sizedAaMode;
    QSize viewSize;
    // 帧序号，用于把阴影贴图限制为隔帧更新。
    quint64 frameIndex = 0;
    // AO 与景深模糊使用的半分辨率尺寸（超采样后的内部尺寸再减半）。
    QSize post;
    std::unique_ptr<Glyphs> glyphs;
    QJsonObject musicItem;
    QString revision;
    template <class T> T *own(T *value, bool resize = false, bool temporary = false) {
        (temporary ? transient : resize ? sized : resources).emplace_back(value);
        return value;
    }
    template <class T> T *create(T *value) {
        if (!value->create()) {
            qFatal("RHI resource allocation failed");
        }
        return value;
    }
    QRhiBuffer *buffer(const QByteArray &data, QRhiBuffer::UsageFlags usage, bool temporary = false) {
        // 统一缓冲一律用 Dynamic：内容逐帧改写，且 Immutable 的初值上传路径在部分后端
        // （尤其 D3D 系）要求类型与上传方式严格配对，Dynamic 是唯一处处成立的选择。
        const bool uniform = usage.testFlag(QRhiBuffer::UniformBuffer);
        auto b =
            own(rhi->newBuffer(uniform ? QRhiBuffer::Dynamic : QRhiBuffer::Immutable, usage, data.size()),
                false, temporary);
        if (!b->create()) {
            qFatal("RHI buffer allocation failed: backend=%d uniform=%d size=%lld", int(rhi->backend()),
                   int(uniform), static_cast<long long>(data.size()));
        }
        if (uniform) {
            updates->updateDynamicBuffer(b, 0, data.size(), data.constData());
        } else {
            updates->uploadStaticBuffer(b, data.constData());
        }
        return b;
    }
    QRhiTexture *texture(QSize s, QRhiTexture::Format format, QRhiTexture::Flags flags = {},
                         bool resize = false, int samples = 1) {
        auto t = own(rhi->newTexture(format, s, 1, flags), resize);
        // 采样数必须在 create() 之前设定：后端在创建时就按采样数分配图像，之后改不回去。
        t->setSampleCount(samples);
        return create(t);
    }
    void upload(QRhiTexture *t, QByteArray bytes) {
        QRhiTextureSubresourceUploadDescription data(bytes);
        updates->uploadTexture(t, QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, data)}));
    }
    /**
     * 逐层生成并上传 mip 链。
     * 不能用 generateMips：部分后端对上传纹理不会填满每一层，而 GL 会把 mip 链不完整的纹理
     * 整体判为不完整并采样为黑——这会让封面直接消失而不是仅仅变模糊。CPU 生成可保证每层都有内容。
     */
    void uploadMipped(QRhiTexture *target, const QImage &image) {
        QVector<QRhiTextureUploadEntry> entries;
        auto level = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        for (int index = 0;; ++index) {
            entries.append(
                QRhiTextureUploadEntry(0, index, QRhiTextureSubresourceUploadDescription(level)));
            if (level.width() == 1 && level.height() == 1) {
                break;
            }
            level = level.scaled(qMax(1, level.width() / 2), qMax(1, level.height() / 2),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        QRhiTextureUploadDescription description;
        description.setEntries(entries.cbegin(), entries.cend());
        updates->uploadTexture(target, description);
    }
    Renderer(QString path, QRhi *device) : rhi(device), root(path) {
        // 采样数能力只查一次：设备在渲染器生命周期内不变，而 resize() 每帧都要判断
        // 采样数是否需要重建目标，不能每次都去问后端。
        supportedSamples = rhi->supportedSampleCounts();
        updates = rhi->nextResourceUpdateBatch();
        glyphs = std::make_unique<Glyphs>(root);
        globalBuffer =
            create(own(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Globals))));
        kernel = buffer(read(root, "ao-kernel.bin"), QRhiBuffer::UniformBuffer);
        // FSR1 的 EASU/RCAS 常量：4 个 vec4 共 64 字节，逐帧按输入/输出尺寸重算。
        // 这里只建缓冲并给全零初值，避免首帧在写入前被采样到未初始化数据。
        fsr1Constants = buffer(QByteArray(sizeof(Fsr1Con), '\0'), QRhiBuffer::UniformBuffer);
        for (auto pair :
             {std::pair{"cassette", "archive-cassette"}, std::pair{"assembly", "archive-assembly"}}) {
            meshes[pair.first] = {
                buffer(read(root, QString(pair.second) + ".bin"), QRhiBuffer::VertexBuffer),
                buffer(read(root, QString(pair.second) + "-indices.bin"), QRhiBuffer::IndexBuffer),
                QJsonDocument::fromJson(read(root, QString(pair.second) + ".json"))
                    .object()["meshes"]
                    .toArray()};
        }
        auto sampler = [&](QRhiSampler::Filter filter, QRhiSampler::Filter mip,
                           QRhiSampler::AddressMode address) {
            return create(own(rhi->newSampler(filter, filter, mip, address, address)));
        };
        linear = sampler(QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge);
        nearest = sampler(QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge);
        mipSampler = sampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::ClampToEdge);
        repeat = sampler(QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::Repeat);
        shadowSampler = own(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        shadowSampler->setTextureCompareOp(QRhiSampler::LessOrEqual);
        create(shadowSampler);
        environment = texture({768, 1024}, QRhiTexture::RGBA16F);
        upload(environment, read(root, "room-pmrem.bin"));
        dfg = texture({16, 16}, QRhiTexture::RGBA16F);
        auto rg = read(root, "dfg-lut.bin");
        QByteArray rgba;
        rgba.reserve(2048);
        for (int i = 0; i < rg.size(); i += 4) {
            rgba.append(rg.constData() + i, 4);
            rgba.append(QByteArray(4, 0));
        }
        upload(dfg, rgba);
        white = texture({1, 1}, QRhiTexture::RGBA8);
        upload(white, QByteArray(4, char(255)));
        noise = texture({4, 4}, QRhiTexture::R32F);
        upload(noise, read(root, "ao-noise.bin"));
        floor = plane(-100, -100, 100, 100, 0, 0, 0, 0, 0);
        // 盘面、盘边和中心透明环共享外壳中心；原网格及变换保持不变。
        auto print =
            DiscGeometry::annulus(DiscGeometry::HubRadius, DiscGeometry::PrintRadius, .105f, .105f, false);
        auto body = DiscGeometry::annulus(DiscGeometry::HoleRadius, DiscGeometry::Radius, .10f, .055f);
        auto hub = DiscGeometry::annulus(DiscGeometry::HoleRadius, DiscGeometry::HubRadius, .112f, .055f);
        coverPlane = buffer(print.bytes(), QRhiBuffer::VertexBuffer);
        discBody = buffer(body.bytes(), QRhiBuffer::VertexBuffer);
        discHub = buffer(hub.bytes(), QRhiBuffer::VertexBuffer);
        discPrintCount = print.count();
        discBodyCount = body.count();
        discHubCount = hub.count();
        musicLabelPlane = plane(-.83, .07, -.03, .29, 0, -.83, .07, .8, .22);
        caseDecoration = plane(-2.5, 0, 2.5, 3.7, .222, -2.5, 0, 5, 3.7);
        // 背景图案按需分两档：卡片在屏幕上很小时（资源库列表）用低档省采样带宽，
        // 拉近到详情才切高档保住细节。绘制共用同一套 1 倍坐标，靠画布缩放派生两档，
        // 切换阈值取 detail 过渡值的一半——飞行途中一次切换，内容相同不可察觉。
        // 文字仍由独立薄片承担（见下方 lettering），密度与背景解耦。
        const QColor ink(65, 70, 62, 170), accent(119, 123, 96, 195);
        const auto paintDecoration = [&](QPainter &painter) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(ink, 1.4));
            for (auto point :
                 {QPointF(75, 60), QPointF(930, 60), QPointF(75, 685), QPointF(930, 685)}) {
                painter.drawLine(point + QPointF(-10, 0), point + QPointF(10, 0));
                painter.drawLine(point + QPointF(0, -10), point + QPointF(0, 10));
            }
            painter.drawEllipse(QRectF(875, 574, 55, 55));
            painter.drawEllipse(QRectF(891, 574, 23, 55));
            painter.drawLine(875, 601, 930, 601);
            painter.fillRect(QRectF(875, 654, 25, 5), accent);
        };
        QImage decorationHigh(2000, 1480, QImage::Format_RGBA8888_Premultiplied);
        decorationHigh.fill(Qt::transparent);
        {
            QPainter painter(&decorationHigh);
            painter.scale(2, 2);
            paintDecoration(painter);
        }
        decorationTextureHigh =
            texture(decorationHigh.size(), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
        uploadMipped(decorationTextureHigh, decorationHigh);
        QImage decorationLow(1000, 740, QImage::Format_RGBA8888_Premultiplied);
        decorationLow.fill(Qt::transparent);
        {
            QPainter painter(&decorationLow);
            paintDecoration(painter);
        }
        decorationTextureLow =
            texture(decorationLow.size(), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
        uploadMipped(decorationTextureLow, decorationLow);
        // 文字单独以 16 倍密度绘制：缩放后笔画仍有足够采样，屏幕上的字才立得住。
        // 区域按原 1000×740 画布的像素坐标框定，绘制时用同一套坐标，只是整体放大八倍。
        const auto lettering = [&](QRect source, auto &&draw) {
            QImage image(source.size() * 16, QImage::Format_RGBA8888_Premultiplied);
            image.fill(Qt::transparent);
            QPainter text(&image);
            text.setRenderHint(QPainter::Antialiasing);
            text.scale(16, 16);
            text.translate(-source.x(), -source.y());
            draw(text);
            text.end();
            auto result = texture(image.size(), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
            uploadMipped(result, image);
            return result;
        };
        // 横向标题：原基线 (130, 688)、字号 15。
        letteringTextureFront =
            lettering(QRect(120, 664, 220, 46), [&](QPainter &text) {
                glyphs->text(text, "MUSIC ARCHIVE / 001", 130, 688, 15, "regular", .5, ink);
            });
        // 纵向脊标：原起点 (75, 465)、字号 21、逆时针 90 度。
        letteringTextureSpine =
            lettering(QRect(62, 348, 46, 128), [&](QPainter &text) {
                text.translate(75, 465);
                text.rotate(-90);
                glyphs->text(text, "RHINE LAB", 0, 0, 21, "demibold", 2, ink);
            });
        // 几何按原画布像素换算：200 px = 1 单位，x 从 -2.5 起、y 从 3.7 向下。
        caseLetteringFront = plane(-1.9, 0.15, -0.8, 0.38, .2225, -1.9, 0.15, 1.1, 0.23);
        caseLetteringSpine = plane(-2.19, 1.32, -1.96, 1.96, .2225, -2.19, 1.32, 0.23, 0.64);
        QImage placeholder(512, 512, QImage::Format_RGBA8888_Premultiplied);
        placeholder.fill(QColor(94, 115, 112));
        QPainter placeholderPainter(&placeholder);
        placeholderPainter.setRenderHint(QPainter::Antialiasing);
        placeholderPainter.setPen(QPen(QColor(185, 196, 178), 2));
        placeholderPainter.drawEllipse(QRectF(52, 52, 408, 408));
        glyphs->text(placeholderPainter, "RHINE LAB", 130, 340, 34, "demibold", 1, QColor(215, 220, 204));
        placeholderPainter.end();
        placeholderCover = texture(placeholder.size(), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
        uploadMipped(placeholderCover, placeholder);
    }
    ~Renderer() {
        if (updates) {
            updates->release();
        }
    }
    QRhiBuffer *plane(float x0, float y0, float x1, float y1, float z, float ux, float uy, float uw,
                      float uh) {
        std::vector<float> vertices;
        for (auto v : {QVector2D{x0, y0}, QVector2D{x1, y0}, QVector2D{x1, y1}, QVector2D{x0, y0},
                       QVector2D{x1, y1}, QVector2D{x0, y1}}) {
            float data[] = {v.x(),
                            v.y(),
                            z,
                            0,
                            0,
                            1,
                            uw ? (v.x() - ux) / uw : 0,
                            uh ? (v.y() - uy) / uh : 0};
            vertices.insert(vertices.end(), std::begin(data), std::end(data));
        }
        return buffer(QByteArray(reinterpret_cast<const char *>(vertices.data()), vertices.size() * 4),
                      QRhiBuffer::VertexBuffer);
    }
    /**
     * 建立渲染目标。resolve 非空时 color 附件是多重采样纹理，渲染通道结束时解析到
     * resolve 纹理（QRhi 在 endPass 里做 resolve，不需要额外的拷贝通道）。
     * 返回的 Target.texture 一律指向“可采样”的那一张——有 resolve 时就是解析结果，
     * 下游通道因此完全不需要知道自己拿到的是不是 MSAA 的画面。
     */
    Target makeTarget(QRhiTexture *texture, QRhiTexture *depth = nullptr, QRhiTexture *second = nullptr,
                      QRhiTexture *resolve = nullptr) {
        QRhiTextureRenderTargetDescription desc;
        if (texture) {
            QRhiColorAttachment attachment(texture);
            if (resolve) {
                attachment.setResolveTexture(resolve);
            }
            if (second) {
                desc.setColorAttachments({attachment, QRhiColorAttachment(second)});
            } else {
                desc.setColorAttachments({attachment});
            }
        }
        desc.setDepthTexture(depth);
        auto rt = own(rhi->newTextureRenderTarget(desc), true);
        auto rp = own(rt->newCompatibleRenderPassDescriptor(), true);
        rt->setRenderPassDescriptor(rp);
        create(rt);
        return {resolve ? resolve : texture, rt, rp};
    }
    void invalidateBindings(QRhiTexture *texture = nullptr) {
        for (auto &state : materials) {
            // 换曲只影响采样该纹理的材质；窗口尺寸变化才需要整批重新绑定。
            if (texture && state.label != texture && state.cover != texture) {
                continue;
            }
            retire(state.bindings);
            state.bindings = nullptr;
        }
    }
    /**
     * 选出 MSAA 实际使用的采样数。
     * 目标 4x，可用 RHINE_MSAA 覆盖（2/4/8），实际取设备支持列表里不超过目标值的最大值。
     * 6.12 的 isTextureFormatSupported 没有采样数参数，所以只能分两步校验：
     * 格式本身可作渲染目标（RGBA16F 与 D32F），加上采样数在 supportedSampleCounts 里。
     * 任一步不满足就退回单采样——宁可没抗锯齿，也不要创建不出目标。
     */
    int msaaSampleCount() const {
        if (qEnvironmentVariable("RHINE_AA_MODE") != "msaa") {
            return 1;
        }
        const int configured = qEnvironmentVariableIntValue("RHINE_MSAA");
        const int wanted = configured > 1 ? qBound(2, configured, 8) : 4;
        if (!rhi->isTextureFormatSupported(QRhiTexture::RGBA16F, QRhiTexture::RenderTarget) ||
            !rhi->isTextureFormatSupported(QRhiTexture::D32F, QRhiTexture::RenderTarget)) {
            return 1;
        }
        int best = 1;
        for (int candidate : supportedSamples) {
            if (candidate > best && candidate <= wanted) {
                best = candidate;
            }
        }
        return best;
    }
    void resize(QSize value, bool needsReadbackTarget) {
        // 渲染分辨率 = 窗口 × 超采样倍率 × 渲染百分比。最终合成通道按目标像素尺寸设视口，
        // 内部目标小于窗口时由 GPU 双线性放大——集成显卡上最直接的一档性能/画质交换。
        const auto sample = qBound(1, qEnvironmentVariableIntValue("RHINE_SUPERSAMPLE"), 2);
        const int percent = qEnvironmentVariableIntValue("RHINE_RENDER_SCALE");
        const double renderScale = percent > 0 ? qBound(.5, percent / 100.0, 1.0) : 1.0;
        // 抗锯齿倍率只对 SSAA 生效，与渲染分辨率相乘而不是互相覆盖。
        const auto aaMode = qEnvironmentVariable("RHINE_AA_MODE");
        const int aaPercent = qEnvironmentVariableIntValue("RHINE_AA_SCALE");
        const double aaScale = aaMode == "ssaa" ? qBound(1.0, aaPercent / 100.0, 2.0) : 1.0;
        // FSR3 需要额外两级目标；切换 AA 模式不改变尺寸，因此必须把它计入重建条件，
        // 否则设置页从 FSR3 切走（或切到 FSR3）时目标不会被创建或不会被释放。
        // FSR3 与 FSR1 都需要"渲染分辨率中间目标 → 显示分辨率输出"这两级目标；
        // FSR3 走计算着色器后端，FSR1 走 EASU/RCAS 两个片元通道，但目标布局相同。
        const bool fsrMode = isFsrMode(aaMode);
        const bool taaMode = aaMode == "taa";
        // FSR 档位参与内部尺寸：整条场景按缩小后的分辨率渲染，再由 FSR 升采样回窗口分辨率。
        // 档位变化会改变 nextSize，因此下面的重建判断会自动覆盖它，不需要额外的比较项。
        const double fsrDivisor = fsrQualityDivisor(aaMode);
        // MSAA 采样数由设备能力决定，可用 RHINE_MSAA 覆盖。它不改变尺寸，所以必须
        // 和 AA 模式一起参与重建判断，否则改了采样数目标不会跟着重建。
        const int nextSamples = msaaSampleCount();
        const QSize nextSize(qMax(1, qRound(value.width() * sample * renderScale * aaScale / fsrDivisor)),
                             qMax(1, qRound(value.height() * sample * renderScale * aaScale / fsrDivisor)));
        // 同时比较窗口尺寸和算出来的内部尺寸：设置里改分辨率时无需重启，
        // 下一帧就会按新尺寸重建全部渲染目标。
        if (viewSize == value && size == nextSize && sizedAaMode == aaMode &&
            sampleCount == nextSamples) {
            return;
        }
        // 诊断：RHINE_DEBUG_FSR=1 时把渲染器实际采用的档位/尺寸打出来，
        // 便于判断"设置改了但画面没变"是渲染器没吃到还是别的原因。
        if (qEnvironmentVariableIsSet("RHINE_DEBUG_FSR")) {
            qWarning() << "FSR state: mode" << aaMode << "quality"
                       << qEnvironmentVariable("RHINE_FSR_QUALITY") << "divisor" << fsrDivisor
                       << "size" << nextSize << "view" << value << "sharpness"
                       << fsrSharpnessPercent();
        }
        sizedAaMode = aaMode;
        sampleCount = nextSamples;
        viewSize = value;
        size = nextSize;
        // AO、法线与景深模糊都只在这个尺寸上工作。
        post = QSize(qMax(1, size.width() / 2), qMax(1, size.height() / 2));
        invalidateBindings();
        postBindings.clear();
        shadowRevision.clear();
        shadowValid = false;
        pipelines.clear();
        pipelineLayouts.clear();
        sized.clear();
        auto rtFlags = QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource;
        // MSAA 只作用于几何 pass：color/depth/velocity 变多重采样，其余目标（AO、景深、
        // 法线、阴影、LDR、FSR 中间结果、读回）一律保持单采样。
        // 多重采样附件不需要“可作拷贝源/采样源”的用途——真正的采样都发生在解析之后。
        const bool multisample = sampleCount > 1;
        const auto msFlags = QRhiTexture::RenderTarget;
        // 深度附件的采样数必须与颜色附件一致，所以 depth 跟着 color 一起变多重采样。
        // 它不解析：合成通道改用已有的半分辨率 normalDepth（见 render() 里的 cocDepth），
        // 因此 MSAA 不需要额外的深度解析通道，也不需要重绘几何。
        depth = texture(size, QRhiTexture::D32F, multisample ? msFlags : rtFlags, true, sampleCount);
        normalDepth = texture(post, QRhiTexture::D32F, rtFlags, true);
        shadow = texture({2048, 2048}, QRhiTexture::D32F, rtFlags, true);
        auto st = makeTarget(nullptr, shadow);
        shadowTarget = st.rt;
        shadowPass = st.rp;
        // color 在 MSAA 下是“多重采样附件 + 单采样解析结果”。makeTarget 让 Target.texture
        // 指向解析结果，于是抓屏、景深、合成三处的采样代码一行都不用改。
        auto colorAttachment =
            texture(size, QRhiTexture::RGBA16F, multisample ? msFlags : rtFlags, true, sampleCount);
        auto colorResolve = multisample ? texture(size, QRhiTexture::RGBA16F, rtFlags, true) : nullptr;
        color = makeTarget(colorAttachment, depth, nullptr, colorResolve);
        // 运动矢量目标与 color 同尺寸、同深度附件：速度 pass 只做深度测试不写深度，
        // PreserveDepthStencilContents 保证它读到的是不透明/玻璃阶段留下的深度。
        // 深度在 MSAA 下是多重的，颜色附件采样数必须与它一致，所以 velocity 同样 MS + resolve；
        // 下游（FSR3 / TAA）采样的是解析后的单采样纹理。
        auto velocityAttachment =
            texture(size, QRhiTexture::RGBA16F, multisample ? msFlags : rtFlags, true, sampleCount);
        auto velocityResolve = multisample ? texture(size, QRhiTexture::RGBA16F, rtFlags, true) : nullptr;
        {
            QRhiColorAttachment attachment(velocityAttachment);
            if (velocityResolve) {
                attachment.setResolveTexture(velocityResolve);
            }
            QRhiTextureRenderTargetDescription desc;
            desc.setColorAttachments({attachment});
            desc.setDepthTexture(depth);
            velocity.rt = own(rhi->newTextureRenderTarget(
                                  desc, QRhiTextureRenderTarget::PreserveDepthStencilContents),
                              true);
            velocity.rp = own(velocity.rt->newCompatibleRenderPassDescriptor(), true);
            velocity.rt->setRenderPassDescriptor(velocity.rp);
            create(velocity.rt);
        }
        velocity.texture = velocityResolve ? velocityResolve : velocityAttachment;
        // 不为全屏抓屏生成 mip：后端 blit 不支持时整张纹理会被判为不完整并采样为黑，
        // detail 模式的玻璃外壳就会变成一层不透明黑膜。玻璃的柔和模糊改由采样端的多点平均承担。
        capture = texture(size, QRhiTexture::RGBA8, rtFlags, true);
        captureTarget = makeTarget(capture);
        // 抓屏模糊金字塔：玻璃按粗糙度在层级间采样，比稀疏点采样平滑且更省带宽。
        captureHalf = makeTarget(texture(QSize(qMax(1, size.width() / 2), qMax(1, size.height() / 2)),
                                         QRhiTexture::RGBA8, rtFlags, true));
        captureQuarter = makeTarget(texture(QSize(qMax(1, size.width() / 4), qMax(1, size.height() / 4)),
                                            QRhiTexture::RGBA8, rtFlags, true));
        captureEighth = makeTarget(texture(QSize(qMax(1, size.width() / 8), qMax(1, size.height() / 8)),
                                           QRhiTexture::RGBA8, rtFlags, true));
        // AO 和景深共用真实 D32F 深度，不再额外打包、写入和解包四通道深度图。
        normal = makeTarget(texture(post, QRhiTexture::RGBA8, rtFlags, true), normalDepth);
        // AO 和模糊只使用红通道，R16F 保留同样的半精度数值，减少四通道显存和带宽。
        const auto aoFormat = rhi->isTextureFormatSupported(QRhiTexture::R16F, rtFlags)
                                  ? QRhiTexture::R16F
                                  : QRhiTexture::RGBA16F;
        // AO、法线与景深模糊都走半分辨率（post 在 resize 开头算好）：AO 的 16 次深度采样、
        // 法线的整场景光栅、景深的 41 次颜色采样都从全屏像素降到 1/4。模糊半径按 AO 自身
        // 纹素计算，半分辨率下屏幕半径自动翻倍，与早期独立 blurAO 通道的观感一致。
        ao = makeTarget(texture(post, aoFormat, rtFlags, true));
        // 景深结果只有半分辨率，最终合成按弥散圆把它与全分辨率颜色混合。
        // 此目标不挂载深度附件，因此可安全采样 normalDepth，不形成读写反馈。
        bokeh = makeTarget(texture(post, QRhiTexture::RGBA16F, rtFlags, true));
        // 抗锯齿在色调映射之后进行，需要一个 LDR 中间目标。FXAA 与 TAA 互斥、共用它：
        // TAA 要同时读“当前帧”和“历史帧”，当前帧不能直接写进最终目标。
        ldr = makeTarget(texture(size, QRhiTexture::RGBA8, rtFlags, true));
        // TAA 历史双缓冲只在 taa 模式下创建，切走时随 sized 一起释放（不常驻显存）。
        // 新建纹理的内容未定义，taaValid 必须复位，首帧会走播种路径。
        if (taaMode) {
            taaHistory[0] = makeTarget(texture(size, QRhiTexture::RGBA16F, rtFlags, true));
            taaHistory[1] = makeTarget(texture(size, QRhiTexture::RGBA16F, rtFlags, true));
        } else {
            taaHistory[0] = {};
            taaHistory[1] = {};
        }
        taaIndex = 0;
        taaValid = false;
        // FSR3 的两级目标：fsrInput 按渲染分辨率接收色调映射结果，fsrOutput 按窗口分辨率
        // 接收升采样结果。输出要能被计算着色器当 UAV 写入，QRhi 的 UsedWithLoadStore 在
        // Vulkan 后端正好映射到 VK_IMAGE_USAGE_STORAGE_BIT。
        if (fsrMode) {
            fsrInput = makeTarget(texture(size, QRhiTexture::RGBA16F, rtFlags, true));
            // 只有 FSR3 用计算着色器把结果当 UAV 写，才需要 UsedWithLoadStore
            // （Vulkan 上映射到 VK_IMAGE_USAGE_STORAGE_BIT）。FSR1 是纯片元路径，
            // 多带一个 storage 用途只会让驱动选另一条布局——Intel/Mesa 对 16F 目标的
            // 存储/采样本来就有已知缺陷，没必要冒这个风险。
            // 计算后端把结果当 UAV 写，需要 storage 用途；片元实现不用它，带上也无害。
            const auto outputFlags = rtFlags | QRhiTexture::UsedWithLoadStore;
            fsrOutput = makeTarget(texture(value, QRhiTexture::RGBA16F, outputFlags, true));
        } else {
            fsrInput = {};
            fsrOutput = {};
        }
        target = {};
        if (needsReadbackTarget) {
            // 读回目标固定为窗口尺寸：超采样只放大内部目标，最终合成通道按目标像素尺寸
            // 设视口完成缩回，读回图不会变成 2 倍。
            target = makeTarget(texture(value, QRhiTexture::RGBA8, rtFlags, true));
        }
        auto desc = color.rt->description();
        loaded =
            own(rhi->newTextureRenderTarget(desc, QRhiTextureRenderTarget::PreserveColorContents |
                                                      QRhiTextureRenderTarget::PreserveDepthStencilContents),
                true);
        loaded->setRenderPassDescriptor(color.rp);
        create(loaded);
    }
    Globals globals(QJsonObject f) {
        QMatrix4x4 light;
        light.lookAt({-6, 14, -5}, {0, 0, 0}, {0, 1, 0});
        QMatrix4x4 ortho;
        ortho.fill(0);
        ortho(0, 0) = 1 / 16.f;
        ortho(1, 1) = 1 / 15.f;
        ortho(2, 2) = -1 / 44.9f;
        ortho(2, 3) = -.1f / 44.9f;
        ortho(3, 3) = 1;
        QMatrix4x4 bias;
        bias(0, 0) = .5;
        bias(1, 1) = -.5;
        bias(0, 3) = .5;
        bias(1, 3) = .5;
        auto projection = matrix(f["projection"].toArray());
        auto aoProjection = matrix(f["aoProjection"].toArray());
        // RHINE_JITTER=1 时 Engine 按 Halton(2,3) 的 8 个相位写 frame["jitter"]；关闭时无此键。
        // AO 深度重建使用同一偏移，否则抖动后的颜色与 AO 会错位。
        const auto jitter = f["jitter"].toArray();
        if (jitter.size() == 2) {
            const QPointF offset(jitter[0].toDouble(), jitter[1].toDouble());
            applyJitter(projection, offset, size);
            applyJitter(aoProjection, offset, size);
        }
        auto theme = n(f["draws"].toArray().first().toObject(), "theme");
        return {matrix(f["view"].toArray()),
                projection,
                bias * ortho * light,
                projection.inverted(),
                aoProjection,
                aoProjection.inverted(),
                v4(vector(f["eye"])),
                v4(vector(f["fogColor"])),
                {n(f, "fogNear"), n(f, "fogFar"), n(f, "focus"), n(f, "aperture")},
                {float(size.width()), float(size.height()), theme, f["viewer"].toBool() ? 1.f : 0.f},
                {n(f, "near"), n(f, "far"), n(f, "detail"), f["reference"].toBool() ? 1.f : 0.f},
                rhi->clipSpaceCorrMatrix(),
                {rhi->isYUpInFramebuffer() ? 1.f : 0.f, 0, 0, 0}};
    }
    QVector3D dark(QString name) {
        static const QHash<QString, uint32_t> colors = {
            {"Frosted_Polymer", 0x626b70},    {"Ivory_Edges", 0x687277},   {"Optical_Diffuser", 0x192226},
            {"Titanium_Fasteners", 0xb1b9bb}, {"Index_Inlay", 0xc6a36b},   {"Printed_Label", 0x303a3e},
            {"Subsurface_Optics", 0x939e9f},  {"Optical_Edges", 0xbbc3bc}, {"Carbon_Ink", 0xb6bdb8}};
        auto hex = colors.value(name, name.contains("Orange") ? 0xbb8850 : 0x969f9f);
        auto channel = [](uint32_t n) {
            float v = n / 255.f;
            return v <= .04045f ? v / 12.92f : float(pow((v + .055f) / 1.055f, 2.4f));
        };
        return {channel((hex >> 16) & 255), channel((hex >> 8) & 255), channel(hex & 255)};
    }
    Surface surface(QJsonObject d) {
        auto name = d["name"].toString();
        bool optical = name.startsWith("Optical_Glass") || name == "Optical_Bridge_Glass";
        float type = d["mesh"].toInt() == -2                            ? 6
                     : d["mesh"].toInt() == -6 || d["mesh"].toInt() == -7 ? 7
                     : d["label"].toBool()                              ? 1
                     : name == "Frosted_Polymer"                        ? 3
                     : name == "Ivory_Edges"                            ? 4
                     : name == "Optical_Diffuser"                       ? 5
                     : !d["instanced"].toBool() && !d["floor"].toBool() ? 2
                                                                        : 0;
        return {v4(vector(d["color"]), n(d, "opacity")),
                {n(d, "roughness"), n(d, "metalness"), n(d, "transmission"), n(d, "thickness")},
                {n(d, "ior"), n(d, "attenuationDistance"), name == "Index_Inlay" ? 1.f : 0.f,
                 d["sharpCover"].toBool() ? 1.f : 0.f},
                v4(vector(d["attenuation"])),
                {n(d, "clearcoat"), n(d, "clearcoatRoughness"),
                 type == 6 ? float(std::cos(n(d, "discAngle"))) : (optical ? 1.f : 0.f),
                 type == 6 ? float(std::sin(n(d, "discAngle"))) : 0.f},
                {type, n(d, "side"), n(d, "subdued"), d["floor"].toBool() ? 1.f : 0.f},
                v4(dark(name), d["instanced"].toBool() ? 1.f : 0.f),
                {0, 0, 1, 1}};
    }
    QList<QJsonObject> musicDraws(QJsonObject f) {
        static const QStringList removed = {"Internal_Ceramic",  "Optical_Edges",       "Subsurface_Optics",
                                            "Optical_Film_Edge", "Amber_Optical_Inlay", "Optical_Film",
                                            "Champagne_Index",   "Printed_Label"};
        QList<QJsonObject> result;
        for (auto v : f["draws"].toArray()) {
            auto d = v.toObject();
            const auto name = d["name"].toString();
            if (removed.contains(name)) {
                continue;
            }
            // 背板完全不透光；只修改背板，正面磨砂盖板继续沿用当前材质。
            if (name == "Optical_Diffuser" && !d["shadowOnly"].toBool()) {
                d["color"] = QJsonArray{.95, .95, .92};
                d["transmission"] = 0;
                d["transparent"] = false;
                d["opacity"] = 1;
                d["roughness"] = .15;
                d["thickness"] = .018;
                d["attenuation"] = QJsonArray{1, 1, 1};
                d["attenuationDistance"] = 8;
                d["depthWrite"] = true;
            }
            if (name == "Frosted_Polymer" && !d["instanced"].toBool()) {
                // 资料库轻磨砂；详情状态透明
                const double quality = n(d, "quality");
                d["thickness"] = .028 * quality + n(d, "thickness") * (1 - quality);
                d["transmission"] = .97 * quality + n(d, "transmission") * (1 - quality);
                if (f["mode"] == "detail" && d["musicSelected"].toBool() && !f["reference"].toBool() &&
                    !f["viewer"].toBool()) {
                    // 曾在此把外壳切到 0.985 高透射并标记 sharpCover（textureLod 直采
                    // level 0）以露出封面，但该采样路径在部分后端渲染成黑膜——列表点击
                    // 动画期间同样的磨砂外壳却始终正常。详情模式因此保持磨砂材质，
                    // 与动画终点的状态一致，不做材质切换。
                    d["clarity"] = 1;
                }
            }
            result << d;
            if (name != "Frosted_Polymer" || d["shadowOnly"].toBool()) {
                continue;
            }
            auto part = d;
            part.remove("sharpCover");
            part["color"] = QJsonArray{1, 1, 1};
            part["roughness"] = .36;
            part["metalness"] = 0;
            part["transmission"] = 0;
            part["thickness"] = 0;
            part["opacity"] = 1;
            part["clearcoat"] = .12;
            part["clearcoatRoughness"] = .25;
            part["transparent"] = false;
            part["depthWrite"] = true;
            part["order"] = 0;
            part["side"] = 2;
            part["castShadow"] = false;
            part["discAngle"] = d["musicSelected"].toBool() ? f["discAngle"].toDouble() : 0;
            part["name"] = "Music_Disc_Print";
            part["mesh"] = -2;
            result << part;
            part["name"] = "Music_Disc_Silver";
            part["mesh"] = -3;
            part["color"] = QJsonArray{.68, .72, .74};
            part["metalness"] = .82;
            part["roughness"] = .22;
            result << part;
            part["name"] = "Music_Disc_Hub";
            part["mesh"] = -4;
            part["color"] = QJsonArray{.96, .97, .96};
            part["metalness"] = 0;
            part["transmission"] = .65;
            part["thickness"] = .02;
            part["roughness"] = .13;
            part["depthWrite"] = false;
            result << part;
            part["name"] = "Music_Case_Marks";
            part["mesh"] = -5;
            part["transmission"] = 0;
            part["transparent"] = true;
            part["label"] = true;
            part["order"] = 50;
            result << part;
        }
        return result;
    }
    void retire(QRhiResource *texture) {
        if (!texture) {
            return;
        }
        auto it =
            std::find_if(resources.begin(), resources.end(), [&](auto &v) { return v.get() == texture; });
        if (it != resources.end()) {
            it->release()->deleteLater();
            resources.erase(it);
        }
    }
    void releaseCover(const QString &key) {
        const auto revision = coverRevisions.take(key);
        const auto previous = covers.take(key);
        // 同专辑曲目及无封面的曲目共享纹理，最后一个引用退出后才退休 GPU 对象。
        if (!previous || covers.values().contains(previous)) {
            return;
        }
        invalidateBindings(previous);
        coverTextures.remove(revision);
        retire(previous);
    }
    void updateMusic(const QJsonObject &value, QImage image) {
        const auto key = value["key"].toString("empty");
        const auto labelRevision = value["title"].toString() + "|" + value["artist"].toString() + "|" +
                                   value["kind"].toString() + "|" + value["id"].toString();
        const qint64 coverRevision = image.cacheKey();
        musicItem = value;
        textureOrder.removeAll(key);
        textureOrder << key;
        if (!labels.contains(key) || labelRevisions.value(key) != labelRevision) {
            // 标签图集：每页 2048×2016 装 8×18 张 256×112 标签，列表卡片的标签批次
            // 从每卡一个 draw 合并到每页一个——资源库 606 draws 的最大来源就此收敛。
            // 同 key 重画复用同槽，纹理对象不变，无需失效绑定；页纹理随图集页生命周期。
            if (!labelSlots.contains(key)) {
                int page = -1;
                int cell = -1;
                for (int index = 0; index < atlasPages.size() && cell < 0; ++index) {
                    if (!atlasPages.at(index).freeCells.isEmpty()) {
                        page = index;
                        cell = atlasPages[index].freeCells.takeLast();
                    }
                }
                if (cell < 0 && atlasPages.size() < 4) {
                    QImage staging(2048, 2016, QImage::Format_RGBA8888_Premultiplied);
                    staging.fill(Qt::transparent);
                    QVector<int> freeCells;
                    for (int index = 143; index >= 0; --index) {
                        freeCells.append(index);
                    }
                    AtlasPage created{texture(staging.size(), QRhiTexture::RGBA8,
                                              QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips),
                                      staging, freeCells, true};
                    // 新页必须完整初始化：未上传过内容的纹理在部分驱动上采样为未定义值。
                    updates->uploadTexture(created.texture, created.staging);
                    atlasPages.append(created);
                    page = atlasPages.size() - 1;
                    cell = atlasPages[page].freeCells.takeLast();
                }
                if (cell < 0) {
                    labelRevisions[key] = labelRevision;
                    return;
                }
                labelSlots[key] = {page, cell};
            }
            const auto slot = labelSlots.value(key);
            auto &bound = atlasPages[slot.page];
            const auto label = glyphs->label(value).scaled(256, 112, Qt::IgnoreAspectRatio,
                                                           Qt::SmoothTransformation);
            {
                QPainter painter(&bound.staging);
                painter.drawImage((slot.cell % 8) * 256, (slot.cell / 8) * 112, label);
                painter.end();
            }
            // 只上传槽位矩形：整页 16 MB，槽位仅 114 KB。
            QRhiTextureSubresourceUploadDescription region(
                bound.staging.copy((slot.cell % 8) * 256, (slot.cell / 8) * 112, 256, 112)
                    .convertToFormat(QImage::Format_RGBA8888_Premultiplied));
            region.setDestinationTopLeft(QPoint((slot.cell % 8) * 256, (slot.cell / 8) * 112));
            updates->uploadTexture(bound.texture,
                                   QRhiTextureUploadDescription(
                                       {QRhiTextureUploadEntry(0, 0, region)}));
            bound.dirty = true;
            labels[key] = bound.texture;
            labelRevisions[key] = labelRevision;
        }
        if (!covers.contains(key) || (!image.isNull() && coverRevisions.value(key) != coverRevision)) {
            releaseCover(key);
            if (!coverTextures.contains(coverRevision)) {
                // 高分辨率封面直接进入贴图
                const auto coverSize = image.isNull() ? 512 : std::clamp(image.width(), 512, 1024);
                QImage pixels(coverSize, coverSize, QImage::Format_RGBA8888_Premultiplied);
                pixels.fill(QColor::fromRgbF(.36, .45, .45));
                QPainter painter(&pixels);
                painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
                if (!image.isNull()) {
                    const auto dimensions =
                        image.size().scaled(coverSize, coverSize, Qt::KeepAspectRatioByExpanding);
                    painter.drawImage(QRectF((coverSize - dimensions.width()) / 2.,
                                             (coverSize - dimensions.height()) / 2., dimensions.width(),
                                             dimensions.height()),
                                      image);
                } else {
                    painter.setPen(QPen(QColor::fromRgbF(.7, .72, .64), 2));
                    painter.drawEllipse(QRectF(52, 52, 408, 408));
                    glyphs->text(painter, "RHINE LAB", 156, 370, 28, "demibold", 1, QColor(215, 220, 204));
                }
                painter.end();
                const auto cover = texture(pixels.size(), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
                uploadMipped(cover, pixels);
                coverTextures[coverRevision] = cover;
                ++coverUploads;
            }
            covers[key] = coverTextures.value(coverRevision);
            coverRevisions[key] = coverRevision;
        }
        labelTexture = labels.value(key);
        coverTexture = covers.value(key);
        // 按实际 RGBA8 像素占用限制缓存；仍在动画中的纹理只在归位后参与淘汰。
        while (textureOrder.size() > 32 || musicTextureBytes() > MUSIC_TEXTURE_BUDGET_BYTES) {
            const auto candidate =
                std::find_if(textureOrder.begin(), textureOrder.end(),
                             [&](const auto &key) { return !activeTextureKeys.contains(key); });
            if (candidate == textureOrder.end()) {
                break;
            }
            const auto old = *candidate;
            textureOrder.erase(candidate);
            // 标签槽位归还给图集页（页纹理被同页其他标签共享，不退休）。
            if (labelSlots.contains(old)) {
                const auto slotRecord = labelSlots.take(old);
                atlasPages[slotRecord.page].freeCells.append(slotRecord.cell);
            }
            labels.remove(old);
            releaseCover(old);
            labelRevisions.remove(old);
        }
    }
    qint64 musicTextureBytes() const {
        // 图集页按整页计（同页标签共享纹理），封面按各自带 mip 的实际占用计。
        // labels[key] 现在指向图集页纹理，不能按条目重复计入。
        qint64 total = qint64(atlasPages.size()) * 2048 * 2016 * 4;
        const auto bytes = [](QRhiTexture *texture) {
            const auto dimensions = texture->pixelSize();
            return qint64(dimensions.width()) * dimensions.height() * 4 * 4 / 3;
        };
        for (const auto texture : coverTextures) {
            total += bytes(texture);
        }
        return total;
    }
    QRhiShaderResourceBindings *bindings(std::vector<QRhiShaderResourceBinding> list,
                                         bool temporary = false) {
        ++bindingCreations;
        auto b = own(rhi->newShaderResourceBindings(), false, temporary);
        b->setBindings(list.begin(), list.end());
        return create(b);
    }
    QRhiGraphicsPipeline *pipeline(const QString &kind, QRhiShaderResourceBindings *srb,
                                   QRhiRenderPassDescriptor *rp, bool back = false, bool doubleSided = false,
                                   bool depthWrite = true, bool blend = false) {
        // 管线只由 kind、渲染通道和四个开关决定。列表页每帧 300+ draw，原先每个 draw 都要
        // 拼一次 QString 键（约十次分配 + 字符串哈希），换成纯数值键后这部分开销基本消失。
        static const QHash<QString, quint64> kinds = {{"surface", 0},       {"shadow", 1},
                                                      {"normal", 2},        {"aoFragment", 3},
                                                      {"combineAO", 4},     {"bokehFragment", 5},
                                                      {"copyColor", 6},     {"finalFragment", 7},
                                                      {"surfaceHashed", 9}, {"aaFragment", 10},
                                                      {"downsample", 11},   {"velocity", 12},
                                                      {"taa", 13},          {"fsr1_easu", 14},
                                                      {"fsr1_rcas", 15}};
        quint64 key = quint64(quintptr(rp)) * 0x9E3779B97F4A7C15ull;
        // 采样数也进键：重建目标时 rp 指针会变，本来就不会命中旧管线，这里再加一道保险。
        key ^= kinds.value(kind, 8) | (quint64(back) << 4) | (quint64(doubleSided) << 5) |
               (quint64(depthWrite) << 6) | (quint64(blend) << 7) | (quint64(sampleCount) << 8);
        if (const auto it = pipelines.constFind(key); it != pipelines.constEnd()) {
            return *it;
        }
        auto p = own(rhi->newGraphicsPipeline(), true);
        const bool geo =
            kind == "surface" || kind == "surfaceHashed" || kind == "normal" || kind == "shadow";
        const bool velocityKind = kind == "velocity";
        p->setShaderStages(
            {{QRhiShaderStage::Vertex,
              shader(velocityKind ? "velocity.vert"
                                  : geo ? (kind == "shadow" ? "shadow.vert" : "geometry.vert")
                                        : "quad.vert")},
             {QRhiShaderStage::Fragment, shader(kind + ".frag")}});
        if (geo) {
            QRhiVertexInputLayout layout;
            layout.setBindings({{32}, {80, QRhiVertexInputBinding::PerInstance}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                  {0, 1, QRhiVertexInputAttribute::Float3, 12},
                                  {0, 2, QRhiVertexInputAttribute::Float2, 24},
                                  {1, 3, QRhiVertexInputAttribute::Float4, 0},
                                  {1, 4, QRhiVertexInputAttribute::Float4, 16},
                                  {1, 5, QRhiVertexInputAttribute::Float4, 32},
                                  {1, 6, QRhiVertexInputAttribute::Float4, 48},
                                  {1, 7, QRhiVertexInputAttribute::Float4, 64}});
            p->setVertexInputLayout(layout);
            p->setDepthTest(true);
            p->setDepthWrite(depthWrite);
            p->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            p->setCullMode(back          ? QRhiGraphicsPipeline::Front
                           : doubleSided ? QRhiGraphicsPipeline::None
                                         : QRhiGraphicsPipeline::Back);
            p->setFrontFace(QRhiGraphicsPipeline::CCW);
        }
        if (velocityKind) {
            // 顶点格式不变（32 字节），实例步长翻倍到 128 字节：前 64 字节当前矩阵，
            // 后 64 字节上一帧矩阵。深度只测试不写入，避免与颜色 pass 的深度产生竞争。
            QRhiVertexInputLayout layout;
            layout.setBindings({{32}, {128, QRhiVertexInputBinding::PerInstance}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                  {0, 1, QRhiVertexInputAttribute::Float3, 12},
                                  {0, 2, QRhiVertexInputAttribute::Float2, 24},
                                  {1, 3, QRhiVertexInputAttribute::Float4, 0},
                                  {1, 4, QRhiVertexInputAttribute::Float4, 16},
                                  {1, 5, QRhiVertexInputAttribute::Float4, 32},
                                  {1, 6, QRhiVertexInputAttribute::Float4, 48},
                                  {1, 7, QRhiVertexInputAttribute::Float4, 64},
                                  {1, 8, QRhiVertexInputAttribute::Float4, 80},
                                  {1, 9, QRhiVertexInputAttribute::Float4, 96},
                                  {1, 10, QRhiVertexInputAttribute::Float4, 112}});
            p->setVertexInputLayout(layout);
            p->setDepthTest(true);
            p->setDepthWrite(false);
            p->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            p->setCullMode(back          ? QRhiGraphicsPipeline::Front
                           : doubleSided ? QRhiGraphicsPipeline::None
                                         : QRhiGraphicsPipeline::Back);
            p->setFrontFace(QRhiGraphicsPipeline::CCW);
        }
        QRhiGraphicsPipeline::TargetBlend b;
        if (blend) {
            b.enable = true;
            b.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            b.srcAlpha = QRhiGraphicsPipeline::One;
            b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        }
        if (kind == "normal") {
            p->setTargetBlends({b, b});
        } else if (kind != "shadow" && kind != "velocity") {
            p->setTargetBlends({b});
        }
        // 管线的布局对象必须比管线活得久，不能持有本帧临时绑定。
        if (!pipelineLayouts.contains(kind)) {
            auto layout = own(rhi->newShaderResourceBindings(), true);
            auto stage = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
            std::vector<QRhiShaderResourceBinding> values = {QRhiShaderResourceBinding::uniformBuffer(
                0,
                kind == "shadow" || kind == "velocity" ? QRhiShaderResourceBinding::VertexStage : stage,
                globalBuffer)};
            if (kind != "shadow" && kind != "normal" && kind != "velocity") {
                // FSR1 的 EASU/RCAS 在 binding 1 要的是 4 个 vec4 的常量块（输入/输出尺寸
                // 与锐化强度），与 AO 的 kernel 完全无关；共用会直接读到 AO 核数据。
                const bool fsr1Kind = kind == "fsr1_easu" || kind == "fsr1_rcas";
                values.push_back(QRhiShaderResourceBinding::uniformBuffer(
                    1, QRhiShaderResourceBinding::FragmentStage,
                    fsr1Kind ? fsr1Constants : kernel));
            }
            // 抖动淡入变体与主表面共用同一套绑定布局。
            const bool surfaceKind = kind == "surface" || kind == "surfaceHashed";
            int count = surfaceKind                                     ? 9
                        : kind == "shadow" || kind == "normal" || kind == "velocity" ? 0
                        : kind == "aoFragment"                           ? 3
                        : kind == "combineAO"                            ? 2
                        : kind == "bokehFragment"                        ? 3
                        : kind == "finalFragment"                        ? 4
                        : kind == "taa"                                  ? 3
                                                                         : 1;
            for (int i = 0; i < count; ++i) {
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    2 + i, QRhiShaderResourceBinding::FragmentStage,
                    surfaceKind && i == 2 ? shadow : white,
                    surfaceKind && i == 2 ? shadowSampler : linear));
            }
            layout->setBindings(values.begin(), values.end());
            create(layout);
            pipelineLayouts[kind] = layout;
        }
        // 管线采样数必须与它写入的附件一致：MSAA 只作用于几何 pass（surface/velocity），
        // 阴影与法线目标是单采样，所有全屏通道同样单采样。
        const bool msaaKind = kind == "surface" || kind == "surfaceHashed" || kind == "velocity";
        p->setSampleCount(msaaKind ? sampleCount : 1);
        p->setShaderResourceBindings(pipelineLayouts[kind]);
        p->setRenderPassDescriptor(rp);
        create(p);
        pipelines.insert(key, p);
        return p;
    }
    QRhiBuffer *dynamicBuffer(BufferState &state, const QByteArray &bytes, QRhiBuffer::UsageFlags usage) {
        if (!state.buffer || state.buffer->size() < bytes.size()) {
            retire(state.buffer);
            const int capacity = qMax(256, int(bytes.size()) * 2);
            state.buffer = create(own(rhi->newBuffer(QRhiBuffer::Dynamic, usage, capacity)));
            state.bytes.clear();
        }
        if (state.bytes != bytes) {
            // 单个连续区间覆盖实际变化；容量增长之外不再创建每帧临时 GPU 对象。
            qsizetype first = 0, last = bytes.size();
            if (state.bytes.size() == bytes.size()) {
                while (first < last && state.bytes[first] == bytes[first]) {
                    ++first;
                }
                while (last > first && state.bytes[last - 1] == bytes[last - 1]) {
                    --last;
                }
            }
            first = (first / 4) * 4;
            last = qMin(bytes.size(), ((last + 3) / 4) * 4);
            if (last > first) {
                updates->updateDynamicBuffer(state.buffer, int(first), int(last - first),
                                             bytes.constData() + first);
            }
            state.bytes = bytes;
        }
        state.used = generation;
        return state.buffer;
    }
    QByteArray matrixBytes(const QJsonObject &draw) const {
        const auto native = nativeBuffers.constFind(draw["id"].toInt());
        if (native != nativeBuffers.constEnd()) {
            return native->matrices;
        }
        QByteArray result;
        for (const auto &value : draw["matrices"].toArray()) {
            const float number = float(value.toDouble());
            result.append(reinterpret_cast<const char *>(&number), 4);
        }
        return result;
    }
    QRhiBuffer *instance(QJsonObject draw) {
        auto &state = instances[draw["id"].toInt()];
        if (state.used == generation) {
            return state.buffer;
        }
        const auto matrices = matrixBytes(draw);
        const auto native = nativeBuffers.value(draw["id"].toInt());
        const auto themes = draw["themes"].toArray();
        std::vector<Instance> values;
        values.reserve(draw["count"].toInt());
        for (int i = 0; i < draw["count"].toInt(); ++i) {
            Instance value;
            if ((i + 1) * 64 <= matrices.size()) {
                memcpy(value.model.elements, matrices.constData() + i * 64, 64);
            }
            float theme = n(draw, "theme");
            if ((i + 1) * 4 <= native.themes.size()) {
                memcpy(&theme, native.themes.constData() + i * 4, 4);
            } else if (i < themes.size()) {
                theme = float(themes[i].toDouble());
            }
            value.state = {n(draw, "quality"), n(draw, "clarity"), theme, 0};
            values.push_back(value);
        }
        const QByteArray bytes(reinterpret_cast<const char *>(values.data()),
                               values.size() * sizeof(Instance));
        return dynamicBuffer(state, bytes, QRhiBuffer::VertexBuffer);
    }
    // 新 shader 由 qt_add_shaders 注册后才会出现在 qrc 里；未注册时跳过速度 pass，
    // 主渲染器仍可启动（因此本文件不依赖 CMakeLists.txt 的改动）。
    static bool velocityShadersAvailable() {
        static const bool available = QFile::exists(":/shaders/velocity.vert.qsb") &&
                                      QFile::exists(":/shaders/velocity.frag.qsb");
        return available;
    }
    bool velocityEligible(const QJsonObject &d) {
        if (d["shadowOnly"].toBool()) {
            return false;
        }
        // 透明/抖动淡入的 draw 不产生速度，目标保持清 0；玻璃即使带 transparent 标记，
        // 只要 transmission > 0 就参与。
        if (d["transparent"].toBool() && n(d, "transmission") <= .000001) {
            return false;
        }
        return surface(d).flags.x() != 2;
    }
    struct Draw {
        QJsonObject data;
        QRhiBuffer *vertices, *indices, *instances;
        QRhiTexture *label;
        QRhiTexture *cover;
        int start;
        int count;
        QRhiShaderResourceBindings *bindings;
    };
    // 128 字节/实例：当前矩阵 + 上一帧矩阵。首帧、实例数变化或新出现的实例把
    // “上一帧”退化为当前矩阵，位移严格为 0。
    QRhiBuffer *velocityInstance(const Draw &draw) {
        auto &state = velocityInstances[draw.data["id"].toInt()];
        if (state.used == generation) {
            return state.buffer;
        }
        const auto current = matrixBytes(draw.data);
        const auto previous = previousModels.value(draw.data["id"].toInt());
        const int count = draw.data["count"].toInt();
        QByteArray bytes;
        bytes.reserve(count * 128);
        const auto append = [&](const QByteArray &source, int index) {
            const int offset = index * 64;
            if (index >= 0 && offset + 64 <= source.size()) {
                bytes.append(source.constData() + offset, 64);
            } else {
                bytes.append(64, '\0');
            }
        };
        // 归档墙每帧按相机距离重排实例顺序（NativeSceneUpdate.cpp），按索引配对会得到
        // 错误位移。世界平移（矩阵第 4 列）是实例的稳定特征，用它把当前实例对到上一帧。
        QHash<QByteArray, int> previousByTranslation;
        if (!previous.isEmpty() && (count > 1 || previous.size() > 64)) {
            previousByTranslation.reserve(count);
            for (int i = 0; (i + 1) * 64 <= previous.size(); ++i) {
                previousByTranslation.insert(previous.mid(i * 64 + 48, 16), i);
            }
        }
        for (int i = 0; i < count; ++i) {
            append(current, i);
            if (previous.isEmpty()) {
                append(current, i);
                continue;
            }
            int match = -1;
            if (!previousByTranslation.isEmpty()) {
                const auto found = previousByTranslation.constFind(current.mid(i * 64 + 48, 16));
                if (found != previousByTranslation.constEnd()) {
                    match = found.value();
                }
            } else if ((i + 1) * 64 <= previous.size()) {
                match = i;
            }
            // 新实例（例如刚进入视野的卡片）没有上一帧位置：用当前矩阵，位移为 0。
            append(match >= 0 ? previous : current, match >= 0 ? match : i);
        }
        return dynamicBuffer(state, bytes, QRhiBuffer::VertexBuffer);
    }
    QList<Draw> prepare(const QList<QJsonObject> &draws, bool decorationHigh) {
        QList<Draw> result;
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        for (auto d : draws) {
            if (d["count"].toInt() <= 0) {
                continue;
            }
            int start = 0;
            int count = 6;
            int mesh = d["mesh"].toInt();
            QRhiBuffer *vertices = floor, *indices = nullptr;
            if (mesh == -2) {
                vertices = coverPlane;
                count = discPrintCount;
            } else if (mesh == -3) {
                vertices = discBody;
                count = discBodyCount;
            } else if (mesh == -4) {
                vertices = discHub;
                count = discHubCount;
            } else if (mesh == -5) {
                vertices = caseDecoration;
            } else if (mesh == -6) {
                vertices = caseLetteringFront;
            } else if (mesh == -7) {
                vertices = caseLetteringSpine;
            } else if (mesh >= 0 && meshes.contains(d["asset"].toString()) &&
                       mesh < meshes[d["asset"].toString()].parts.size()) {
                auto m = meshes[d["asset"].toString()];
                auto part = m.parts[mesh].toObject();
                start = part["start"].toInt();
                count = part["count"].toInt();
                vertices = m.vertices;
                indices = m.indices;
            } else if (d["label"].toBool()) {
                vertices = musicLabelPlane;
            } else if (!d["floor"].toBool()) {
                continue;
            }
            const auto resourceKey =
                QString::number(d["id"].toInt()) + "/" + QString::number(mesh) + "/" + d["name"].toString();
            auto &state = materials[resourceKey];
            state.used = generation;
            const auto key =
                d["musicSelected"].toBool() ? musicItem["key"].toString() : d["musicKey"].toString();
            auto material = surface(d);
            // 标签图集：把槽位换算成 uv 缩放与偏移，随材质 uniform 下发。
            // 非图集采样的部件（装饰印记、装饰文字、封面）保持全图 {0,0,1,1}。
            if (d["label"].toBool()) {
                const auto slot = labelSlots.value(key, {-1, -1});
                if (slot.page >= 0) {
                    material.atlas = {float(slot.cell % 8) / 8.f, float(slot.cell / 8) / 18.f,
                                      1.f / 8.f, 1.f / 18.f};
                }
            }
            const auto mb = dynamicBuffer(
                state.uniform, QByteArray(reinterpret_cast<const char *>(&material), sizeof(material)),
                QRhiBuffer::UniformBuffer);
            QRhiTexture *label = white;
            if (mesh == -5) {
                label = decorationHigh ? decorationTextureHigh : decorationTextureLow;
            } else if (mesh == -6) {
                label = letteringTextureFront;
            } else if (mesh == -7) {
                label = letteringTextureSpine;
            } else if (d["label"].toBool()) {
                label = labels.value(key, labelTexture ? labelTexture : white);
            }
            // 着色器只有盘面读取封面，只有标签读取文字；其他表面绑定稳定的占位纹理。
            const auto cover = mesh == -2 ? covers.value(key, placeholderCover) : placeholderCover;
            // 缓存命中仍可能换曲，同一个 draw 的绑定必须跟随实际纹理指针。
            if (state.label != label || state.cover != cover) {
                retire(state.bindings);
                state.bindings = nullptr;
                state.label = label;
                state.cover = cover;
            }
            if (!state.bindings) {
                state.bindings = bindings(
                    {QRhiShaderResourceBinding::uniformBuffer(0, stages, globalBuffer),
                     QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::FragmentStage,
                                                              mb),
                     QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                               environment, linear),
                     QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                               capture, linear),
                     QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage,
                                                               shadow, shadowSampler),
                     // 文字与装饰图集缩小时必须走 mip，否则细笔画会闪烁成锯齿。
                     QRhiShaderResourceBinding::sampledTexture(5, QRhiShaderResourceBinding::FragmentStage,
                                                               label, mipSampler),
                     QRhiShaderResourceBinding::sampledTexture(6, QRhiShaderResourceBinding::FragmentStage,
                                                               dfg, linear),
                     QRhiShaderResourceBinding::sampledTexture(7, QRhiShaderResourceBinding::FragmentStage,
                                                               cover, mipSampler),
                     QRhiShaderResourceBinding::sampledTexture(8, QRhiShaderResourceBinding::FragmentStage,
                                                               captureHalf.texture, linear),
                     QRhiShaderResourceBinding::sampledTexture(9, QRhiShaderResourceBinding::FragmentStage,
                                                               captureQuarter.texture, linear),
                     QRhiShaderResourceBinding::sampledTexture(10, QRhiShaderResourceBinding::FragmentStage,
                                                               captureEighth.texture, linear)},
                    false);
            }
            result.append({d, vertices, indices, instance(d), label, cover, start, count, state.bindings});
        }
        // 图集有槽位更新时重建 mip 链：每页每帧最多一次，避免逐标签重复重建。
        for (auto &page : atlasPages) {
            if (page.dirty) {
                updates->generateMips(page.texture);
                page.dirty = false;
            }
        }
        return result;
    }
    QHash<QByteArray, BufferState> batchInstances;
    /**
     * 把同几何的阴影物体和完全相同材质的不透明物体合成实例批次。
     * 透明与透射物体保留原排序；每个实例继续使用原来的矩阵、主题和揭示状态。
     */
    QList<Draw> batchDraws(const QList<Draw> &input, bool shadowPass, bool normalPass = false) {
        QList<Draw> result;
        QList<QByteArray> bytes;
        QList<QByteArray> keys;
        QHash<QByteArray, int> groups;
        QHash<QByteArray, int> groupSlots;
        for (const auto &draw : input) {
            const auto &data = draw.data;
            if (shadowPass && !(data["castShadow"].toBool() || data["shadowOnly"].toBool())) {
                continue;
            }
            if (!shadowPass && data["shadowOnly"].toBool()) {
                continue;
            }
            const auto material = surface(data);
            const bool eligible = shadowPass || normalPass ||
                                  (!data["transparent"].toBool() && n(data, "transmission") <= .000001 &&
                                   material.flags.x() != 2 && data["depthWrite"].toBool());
            if (!eligible) {
                result.append(draw);
                bytes.append(QByteArray());
                keys.append(QByteArray());
                continue;
            }
            QByteArray prefix = "opaque/";
            if (shadowPass) {
                prefix = "shadow/";
            } else if (normalPass) {
                prefix = "normal/";
            }
            auto key = prefix + QByteArray::number(quintptr(draw.vertices)) + '/' +
                       QByteArray::number(quintptr(draw.indices)) + '/' + QByteArray::number(draw.start) +
                       '/' + QByteArray::number(draw.count) + '/' +
                       QByteArray::number(normalPass ? 0 : data["side"].toInt());
            if (!shadowPass && !normalPass) {
                key += '/' + QByteArray::number(data["order"].toInt()) + '/' +
                       QByteArray::number(quintptr(draw.label)) + '/' +
                       QByteArray::number(quintptr(draw.cover)) + ':';
            }
            const auto bufferKey = key;
            if (!shadowPass && !normalPass) {
                key.append(reinterpret_cast<const char *>(&material), sizeof(material));
            }
            auto group = groups.constFind(key);
            int index;
            if (group == groups.constEnd()) {
                index = result.size();
                groups.insert(key, index);
                result.append(draw);
                result.last().data["count"] = 0;
                bytes.append(QByteArray());
                // 材质插值每帧变化，但批次缓冲区按几何和槽位复用，不能按材质值反复创建。
                const int slot = groupSlots[bufferKey]++;
                keys.append(bufferKey + '/' + QByteArray::number(slot));
            } else {
                index = group.value();
            }
            bytes[index].append(instances.value(data["id"].toInt()).bytes);
            result[index].data["count"] = result[index].data["count"].toInt() + data["count"].toInt();
        }
        for (int i = 0; i < result.size(); ++i) {
            if (!keys[i].isEmpty()) {
                result[i].instances =
                    dynamicBuffer(batchInstances[keys[i]], bytes[i], QRhiBuffer::VertexBuffer);
            }
        }
        return result;
    }
    void geometry(QRhiCommandBuffer *cb, const QList<Draw> &draws, QString kind, QRhiRenderPassDescriptor *rp,
                  bool back = false) {
        for (const auto &draw : draws) {
            auto d = draw.data;
            if (d["shadowOnly"].toBool() && kind != "shadow") {
                continue;
            }
            if (kind == "shadow" && !(d["castShadow"].toBool() || d["shadowOnly"].toBool())) {
                continue;
            }
            const bool hashed = surface(d).flags.x() == 2;
            const bool blend =
                kind == "surface" && (hashed || d["transparent"].toBool() || n(d, "transmission") > .000001);
            const bool write = kind != "surface" || hashed || !blend || d["depthWrite"].toBool();
            // 抖动淡入的 draw 走带 discard 的变体；主不透明管线必须保持无 discard，
            // 硬件才会启用 early-Z——不透明实例的前到后排序才有意义。
            const auto pipelineKind =
                (kind == "surface" && hashed) ? QStringLiteral("surfaceHashed") : kind;
            cb->setGraphicsPipeline(pipeline(pipelineKind, draw.bindings, rp, back,
                                             kind != "normal" && d["side"].toInt() == 2, write, blend));
            // 绑定与管线必须一一对应：RHI 会按首次使用时建立的映射校验后续绑定的
            // 布局。阴影/法线阶段只绑定全局参数，不能复用材质的纹理绑定，
            // 否则深度贴图会被误读成环境光贴图。
            cb->setShaderResources(kind == "surface" ? draw.bindings : pipelineLayouts.value(kind));
            // 法线只喂半分辨率的 AO，按 post 尺寸光栅，省掉 3/4 的像素。
            auto s = kind == "shadow" ? QSize(2048, 2048) : kind == "normal" ? post : size;
            cb->setViewport({0, 0, float(s.width()), float(s.height())});
            const QRhiCommandBuffer::VertexInput inputs[] = {{draw.vertices, 0}, {draw.instances, 0}};
            cb->setVertexInput(0, 2, inputs, draw.indices, 0, QRhiCommandBuffer::IndexUInt32);
            if (draw.indices) {
                cb->drawIndexed(draw.count, d["count"].toInt(), draw.start);
            } else {
                cb->draw(draw.count, d["count"].toInt(), draw.start);
            }
            ++drawCalls;
        }
    }
    // 速度 pass：在 loaded（玻璃/透明）之后、法线/AO 之前。目标先清 0，跳过的
    // 透明/抖动 draw 自然保持 0；只在不透明与玻璃 draw 上光栅。
    void velocityPass(QRhiCommandBuffer *cb, const QList<Draw> &draws) {
        if (!velocityShadersAvailable()) {
            return;
        }
        QList<const Draw *> eligible;
        for (const auto &draw : draws) {
            if (velocityEligible(draw.data)) {
                eligible.append(&draw);
            }
        }
        // 速度实例缓冲走独立更新批次：主更新批次在颜色 pass 前已经提交。
        auto batch = rhi->nextResourceUpdateBatch();
        updates = batch;
        QList<QRhiBuffer *> buffers;
        buffers.reserve(eligible.size());
        for (const auto *draw : eligible) {
            buffers.append(velocityInstance(*draw));
        }
        updates = nullptr;
        cb->resourceUpdate(batch);
        cb->beginPass(velocity.rt, Qt::black, {1, 0});
        for (int i = 0; i < eligible.size(); ++i) {
            const auto &draw = *eligible[i];
            const auto &d = draw.data;
            cb->setGraphicsPipeline(pipeline("velocity", draw.bindings, velocity.rp, false,
                                             d["side"].toInt() == 2, false, false));
            cb->setShaderResources(pipelineLayouts.value("velocity"));
            cb->setViewport({0, 0, float(size.width()), float(size.height())});
            const QRhiCommandBuffer::VertexInput inputs[] = {{draw.vertices, 0}, {buffers[i], 0}};
            cb->setVertexInput(0, 2, inputs, draw.indices, 0, QRhiCommandBuffer::IndexUInt32);
            if (draw.indices) {
                cb->drawIndexed(draw.count, d["count"].toInt(), draw.start);
            } else {
                cb->draw(draw.count, d["count"].toInt(), draw.start);
            }
            ++drawCalls;
        }
        cb->endPass();
    }
    void quad(QRhiCommandBuffer *cb, QString kind, QRhiRenderTarget *rt, QRhiRenderPassDescriptor *rp,
              QList<QRhiTexture *> textures, QSize viewportSize = QSize()) {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        // FSR1 的两个通道在 binding 1 绑自己的常量块，其余全屏通道绑 AO kernel。
        const bool fsr1Kind = kind == "fsr1_easu" || kind == "fsr1_rcas";
        std::vector<QRhiShaderResourceBinding> list = {
            QRhiShaderResourceBinding::uniformBuffer(0, stages, globalBuffer),
            QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::FragmentStage,
                                                     fsr1Kind ? fsr1Constants : kernel)};
        for (int i = 0; i < textures.size(); ++i) {
            auto s = (kind == "aoFragment"                    ? (i == 2 ? repeat : nearest)
                      : kind == "bokehFragment" && i == 2     ? nearest
                      : kind == "finalFragment" && i == 3     ? nearest
                                                              : linear);
            list.push_back(QRhiShaderResourceBinding::sampledTexture(
                2 + i, QRhiShaderResourceBinding::FragmentStage, textures[i], s));
        }
        // 查看封面使用当前颜色纹理，资料库使用景深纹理；缓存必须区分实际输入。
        // 否则切换查看器后会继续采样上一张资料库画面。
        QString bindingKey = kind;
        for (auto texture : textures) {
            bindingKey += "/" + QString::number(quintptr(texture), 16);
        }
        auto &sb = postBindings[bindingKey];
        if (!sb) {
            sb = own(rhi->newShaderResourceBindings(), true);
            sb->setBindings(list.begin(), list.end());
            create(sb);
        }
        cb->beginPass(rt, Qt::black, {1, 0});
        cb->setGraphicsPipeline(pipeline(kind, sb, rp));
        cb->setShaderResources(sb);
        // 视口默认跟随超采样后的内部尺寸；最终合成到窗口时由调用方传入窗口尺寸以完成缩回。
        const auto vp = viewportSize.isEmpty() ? size : viewportSize;
        cb->setViewport({0, 0, float(vp.width()), float(vp.height())});
        cb->draw(3);
        ++drawCalls;
        cb->endPass();
    }
    // FSR1 的常量逐帧变化（输入/输出尺寸、锐化强度），必须在 beginPass 之前用独立的上传
    // 批次写入；quad() 的 SRB 缓存只区分 kind 与纹理指针，不含常量内容，不能靠它刷新。
    void quadFsr1(QRhiCommandBuffer *cb, QString kind, QRhiRenderTarget *rt,
                  QRhiRenderPassDescriptor *rp, QRhiTexture *source, QSize viewportSize,
                  const Fsr1Con &constants) {
        auto batch = rhi->nextResourceUpdateBatch();
        batch->updateDynamicBuffer(fsr1Constants, 0, sizeof(Fsr1Con), &constants);
        cb->resourceUpdate(batch);
        quad(cb, kind, rt, rp, {source}, viewportSize);
    }
    void render(QRhiCommandBuffer *cb, QJsonObject frame, QSize dimensions, QJsonObject item, QImage cover,
                QRhiRenderTarget *finalTarget = nullptr, const Engine::MusicTextures &musicTextures = {}) {
        transient.clear();
        ++generation;
        // FSR 重置（Engine::resetTemporalState）：丢弃上一帧矩阵，本帧速度按 0 输出。
        if (frame["temporalReset"].toBool()) {
            previousModels.clear();
            // TAA 历史同样作废：场景切换/相机跳变后旧历史与当前帧没有对应关系，
            // 继续累积会把上一场景的画面拖进来。
            taaValid = false;
        }
        if (!updates) {
            updates = rhi->nextResourceUpdateBatch();
        }
        resize(dimensions, !finalTarget);
        // FSR3 的上下文与目标尺寸绑定，必须在任何渲染通道之外重建。相机 near/far 直接从
        // 投影矩阵反解（Rhine 用标准 OpenGL 式透视：m[10]=c, m[14]=d，near=d/(c-1)、
        // far=d/(c+1)），这样列表页与查看封面页各自不同的 near/far 都能自动跟上。
        const auto aaMode = qEnvironmentVariable("RHINE_AA_MODE");
        // 统一的 "fsr" 先尝试 FFX 计算后端；旧的 fsr1 明确只用片元实现。
        const bool fsrRequested = isFsrMode(aaMode) && aaMode != "fsr1";
        if (fsrRequested && !fsr3) {
            fsr3 = std::make_unique<Fsr3Backend>();
        }
        if (fsr3) {
            if (fsrRequested) {
                float frameTimeMs = 16.6667f;
                if (fsrClock.isValid()) {
                    frameTimeMs = float(fsrClock.restart()) / 1e6f;
                } else {
                    fsrClock.start();
                }
                float nearPlane = 0.1f, farPlane = 5000.f;
                const auto projection = frame["projection"].toArray();
                if (projection.size() == 16) {
                    const float c = float(projection.at(10).toDouble());
                    const float d = float(projection.at(14).toDouble());
                    if (std::abs(c - 1.f) > 1e-6f && std::abs(c + 1.f) > 1e-6f) {
                        nearPlane = d / (c - 1.f);
                        farPlane = d / (c + 1.f);
                    }
                }
                fsr3->setCamera(nearPlane, farPlane, frameTimeMs);
                const auto jitter = frame["jitter"].toArray();
                jitterPixels = jitter.size() == 2
                                   ? QPointF(jitter.at(0).toDouble(), jitter.at(1).toDouble())
                                   : QPointF();
                // 符号：applyJitter 把投影的平移项改成 (0,2) -= 2x/w、(1,2) += 2y/h，换算到
                // 屏幕像素（y 向下）是 (-x, -y)；而 FSR3 的 Jitter() 定义为采样位置被 +Jitter
                // 偏移过（ffx_fsr3upscaler_upsample.h 里用 (pos + 0.5) - Jitter() 反解），
                // 所以这里要传相反数，否则重投影相位反了会糊。
                fsr3->setJitter(float(-jitterPixels.x()), float(-jitterPixels.y()));
                if (frame["temporalReset"].toBool()) {
                    fsr3->requestReset();
                }
                fsr3->resize(rhi, size, viewSize);
            } else {
                // 切走 FSR3 时释放上下文，避免常驻显存与后台设备状态。
                fsr3->release();
            }
        }
        activeTextureKeys.clear();
        activeTextureKeys.insert(item["key"].toString("empty"));
        for (auto it = musicTextures.begin(); it != musicTextures.end(); ++it) {
            activeTextureKeys.insert(it.key());
        }
        // 恢复图形设备时先还原仍可见的旧唱片，最后绑定当前曲目。
        for (auto it = musicTextures.begin(); it != musicTextures.end(); ++it) {
            if (it.key() != item["key"].toString("empty") && !labels.contains(it.key())) {
                updateMusic(it->item, it->cover);
            }
        }
        updateMusic(item, cover);
        auto g = globals(frame);
        updates->updateDynamicBuffer(globalBuffer, 0, sizeof(g), &g);
        // 背景图案在详情模式用高档，资源库列表等远景用低档。
        const bool decorationHigh = frame["mode"].toString() == "detail";
        auto data = prepare(musicDraws(frame), decorationHigh);
        for (auto it = materials.begin(); it != materials.end();) {
            if (it->used != generation) {
                retire(it->bindings);
                retire(it->uniform.buffer);
                it = materials.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = instances.begin(); it != instances.end();) {
            if (it->used != generation) {
                retire(it->buffer);
                it = instances.erase(it);
            } else {
                ++it;
            }
        }
        const bool useBatches = frame["batchDraws"].toBool(true);
        auto shadowDraws = useBatches ? batchDraws(data, true) : data;
        QByteArray shadowInputs;
        for (const auto &draw : data) {
            if (draw.data["castShadow"].toBool() || draw.data["shadowOnly"].toBool()) {
                const auto identity = QByteArray::number(draw.data["id"].toInt()) + "/" +
                                      draw.data["asset"].toString().toUtf8() + "/" +
                                      QByteArray::number(draw.data["mesh"].toInt()) + "/" +
                                      QByteArray::number(draw.data["count"].toInt()) + ":";
                shadowInputs.append(identity);
                shadowInputs.append(matrixBytes(draw.data));
            }
        }
        ++frameIndex;
        // 阴影贴图每两帧最多更新一次：转动时模型矩阵逐帧变化，但隔帧的阴影差异肉眼不可见，
        // 阴影 pass 的几何提交与深度写入开销因此减半。首帧必须生成，否则阴影为空。
        const bool shadowStale = !shadowValid || shadowInputs != shadowRevision;
        if (shadowStale && (!shadowValid || (frameIndex & 1) == 0)) {
            // 合批缓冲区与其他实例更新一起提交，阴影只需按几何而非按卡片发出指令。
            cb->resourceUpdate(updates);
            updates = nullptr;
            cb->beginPass(shadowTarget, Qt::black, {1, 0});
            geometry(cb, shadowDraws, "shadow", shadowPass);
            cb->endPass();
            shadowRevision = shadowInputs;
            shadowValid = true;
        }
        QList<Draw> opaque, glass, transparent;
        for (auto d : data) {
            if (n(d.data, "transmission") > .000001) {
                glass << d;
            } else if (d.data["transparent"].toBool()) {
                transparent << d;
            } else {
                opaque << d;
            }
        }
        auto sort = [](auto &v) {
            std::stable_sort(v.begin(), v.end(), [](auto &a, auto &b) {
                return a.data["order"].toInt() < b.data["order"].toInt();
            });
        };
        sort(opaque);
        sort(glass);
        sort(transparent);
        // 阴影未重绘时更新批次仍未提交；颜色通道需要的实例缓冲区在此准备。
        if (!updates) {
            updates = rhi->nextResourceUpdateBatch();
        }
        if (useBatches) {
            opaque = batchDraws(opaque, false);
        }
        QList<Draw> normalDraws;
        if (!frame["viewer"].toBool()) {
            normalDraws =
                useBatches && frame["batchNormal"].toBool(true) ? batchDraws(data, false, true) : data;
        }
        for (auto it = batchInstances.begin(); it != batchInstances.end();) {
            if (it->used != generation) {
                retire(it->buffer);
                it = batchInstances.erase(it);
            } else {
                ++it;
            }
        }
        cb->resourceUpdate(updates);
        updates = nullptr;
        auto bg = vector(frame["background"]);
        cb->beginPass(color.rt, QColor::fromRgbF(bg.x(), bg.y(), bg.z()), {1, 0});
        geometry(cb, opaque, "surface", color.rp);
        cb->endPass();
        auto copy = rhi->nextResourceUpdateBatch();
        // 单层透射基准：玻璃正面采样不透明阶段的结果。
        // 抓屏不用 copyTexture——Vulkan 的 copyImage 要求格式兼容，RGBA16F 源拷到
        // RGBA8 目标产生未定义数据（实测整屏粉红），改走渲染 pass 由 GPU 完成转换。
        if (!glass.isEmpty()) {
            quad(cb, "copyColor", captureTarget.rt, captureTarget.rp, {color.texture});
            // 逐级盒式降采样，玻璃透射按粗糙度在其中取层级。
            quad(cb, "downsample", captureHalf.rt, captureHalf.rp, {capture},
                 captureHalf.rt->pixelSize());
            quad(cb, "downsample", captureQuarter.rt, captureQuarter.rp, {captureHalf.texture},
                 captureQuarter.rt->pixelSize());
            quad(cb, "downsample", captureEighth.rt, captureEighth.rp, {captureQuarter.texture},
                 captureEighth.rt->pixelSize());
        }
        cb->resourceUpdate(copy);
        cb->beginPass(loaded, Qt::black, {1, 0});
        geometry(cb, glass, "surface", color.rp);
        geometry(cb, transparent, "surface", color.rp);
        cb->endPass();
        // 运动矢量在颜色与玻璃都落盘后生成：深度缓冲此时已经是最终可见表面，
        // 速度 pass 只测深度不写颜色/深度，不会干扰后续 AO 与合成。
        velocityPass(cb, data);
        if (!frame["viewer"].toBool()) {
            cb->beginPass(normal.rt, Qt::white, {1, 0});
            geometry(cb, normalDraws, "normal", normal.rp);
            cb->endPass();
            // AO 目标已减半，视口必须跟随目标尺寸。
            quad(cb, "aoFragment", ao.rt, ao.rp, {normal.texture, normalDepth, noise}, post);
            // 不再单独合成 AO：景深通道直接对颜色做模糊，最后再乘 AO，
            // 省掉一个全屏 pass（约 84MB/帧 的读写往返）。
            quad(cb, "bokehFragment", bokeh.rt, bokeh.rp, {color.texture, ao.texture, normalDepth}, post);
        }
        auto destination = finalTarget ? finalTarget : target.rt;
        const bool viewer = frame["viewer"].toBool();
        // 抗锯齿互斥单选：
        //   fxaa  → 合成到 LDR 中间目标，再走 FXAA 输出到目标；
        //   taa   → 合成到 LDR 中间目标，按运动矢量与历史累积后再拷到目标；
        //   fsr3  → 合成到渲染分辨率的中间目标，外部派发 FSR3 升采样，再拷贝到目标；
        //   fsr1  → 合成到渲染分辨率的中间目标，EASU 升采样 + RCAS 锐化后写目标；
        //   其余  → 直接合成到目标（SSAA/MSAA 在采样阶段生效）。
        const bool fxaa = aaMode == "fxaa";
        const bool taa = aaMode == "taa" && taaHistory[0].rt && taaHistory[1].rt;
        const bool fsr3Active = fsrRequested && fsr3 && fsr3->available() && fsrInput.rt && fsrOutput.rt;
        // 计算后端不可用时，任何 FSR 模式都退回 EASU/RCAS 片元实现：超分与锐化照常生效。
        // 这样设置页只需要一个 "FSR" 选项，具体实现由后端能力决定。
        const bool fsr1Active = isFsrMode(aaMode) && !fsr3Active && fsrInput.rt && fsrOutput.rt;
        if (isFsrMode(aaMode)) {
            // 把实际生效的实现告诉监视窗，避免"选了 FSR 却不知道跑的是哪条路径"。
            qputenv("RHINE_FSR_ENGINE", fsr3Active ? "fsr3" : "shader");
        }
        // 弥散圆权重用的深度：MSAA 下 depth 是多采样附件，不能直接采样。CoC 是低频信号，
        // 半分辨率的 normalDepth 完全够用，因此不需要额外的深度解析通道，也不用重绘几何。
        // viewer 分支本来就不读深度，同样传它即可。
        QRhiTexture *cocDepth = sampleCount > 1 ? normalDepth : depth;
        const auto compositeList =
            viewer ? QList<QRhiTexture *>{color.texture, color.texture, white, cocDepth}
                   : QList<QRhiTexture *>{color.texture, bokeh.texture, ao.texture, cocDepth};
        if (fsr3Active) {
            // 输入必须是渲染分辨率：视口留空即用内部尺寸 size，不缩到窗口。
            quad(cb, "finalFragment", fsrInput.rt, fsrInput.rp, compositeList, QSize());
            // FSR3 在外部命令里派发：此时没有活动渲染通道，QRhi 不介入这段的布局，
            // SDK 的 Vulkan 后端会为每个资源排自己的屏障。
            cb->beginExternal();
            const bool upscaled = fsr3->dispatch(rhi, cb, fsrInput.texture, depth, velocity.texture,
                                                 fsrOutput.texture);
            cb->endExternal();
            // 升采样成功就拷贝结果，失败则退回渲染分辨率的中间结果（画面仍正确，只是没超分）。
            quad(cb, "copyColor", destination, destination->renderPassDescriptor(),
                 {upscaled ? fsrOutput.texture : fsrInput.texture}, destination->pixelSize());
        } else if (fsr1Active) {
            // FSR1 是纯片元方案，不需要像 FSR3 那样交出命令缓冲区：EASU 先把渲染分辨率的
            // 合成结果升采样到显示分辨率，RCAS 再在这个尺寸上锐化并直接写目标。
            // 锐化强度用 AMD 的 stops 语义（0 = 最锐，数值越大越弱），0.2 是肉眼可辨
            // 但不至于出白边的档位；RCAS 与分辨率无关，所以常量只需输出尺寸。
            quad(cb, "finalFragment", fsrInput.rt, fsrInput.rp, compositeList, QSize());
            quadFsr1(cb, "fsr1_easu", fsrOutput.rt, fsrOutput.rp, fsrInput.texture,
                     fsrOutput.rt->pixelSize(), Fsr1Constants::easu(size, viewSize));
            quadFsr1(cb, "fsr1_rcas", destination, destination->renderPassDescriptor(),
                     fsrOutput.texture, destination->pixelSize(),
                     // 界面上的锐化百分比与 RCAS 的"档"方向相反：0% 最柔（2 档），100% 最锐（0 档）。
                     Fsr1Constants::rcas(destination->pixelSize(),
                                         float(100 - fsrSharpnessPercent()) / 100.f * 2.f));
        } else if (taa) {
            // TAA 要同时读“当前帧”和“历史帧”，所以当前帧先落到 ldr（与 FXAA 共用的中间目标）。
            quad(cb, "finalFragment", ldr.rt, ldr.rp, compositeList, QSize());
            const int history = taaIndex;
            if (!taaValid) {
                // 首帧 / 尺寸或模式变化 / temporalReset：历史内容无效。直接输出当前帧，
                // 并把它播种成历史，下一帧的重投影与邻域夹紧就有完整输入。这样 taa.frag
                // 不需要“是否重置”的参数，绑定布局保持为纯粹的三张纹理。
                quad(cb, "copyColor", destination, destination->renderPassDescriptor(), {ldr.texture},
                     destination->pixelSize());
                quad(cb, "copyColor", taaHistory[history].rt, taaHistory[history].rp, {ldr.texture});
            } else {
                const int output = 1 - history;
                // 重投影对齐历史 → 3×3 邻域夹紧抑制鬼影 → 与当前帧混合，结果写进另一张历史。
                quad(cb, "taa", taaHistory[output].rt, taaHistory[output].rp,
                     {ldr.texture, taaHistory[history].texture, velocity.texture});
                // 历史纹理不能同时当窗口目标，所以再拷一次到窗口/读回目标。
                quad(cb, "copyColor", destination, destination->renderPassDescriptor(),
                     {taaHistory[output].texture}, destination->pixelSize());
                taaIndex = output;
            }
            taaValid = true;
        } else {
            const bool aa = fxaa;
            auto finalTargetRt = aa ? ldr.rt : destination;
            quad(cb, "finalFragment", finalTargetRt,
                 aa ? ldr.rp : destination->renderPassDescriptor(), compositeList,
                 aa ? QSize() : destination->pixelSize());
            if (aa) {
                quad(cb, "aaFragment", destination, destination->renderPassDescriptor(),
                     {ldr.texture}, destination->pixelSize());
            }
        }
        // 速度历史必须在本帧速度 pass 之后滚动，否则 velocityPass 会把当前矩阵当成上一帧，
        // 所有位移恒为 0。draw id 帧间稳定；同一 id 的多个 draw（唱片部件）共用矩阵，
        // 重复写入是幂等的。shader 未注册时整段跳过，保持现有渲染路径零额外开销。
        if (velocityShadersAvailable()) {
            QSet<int> velocityIds;
            for (const auto &draw : data) {
                const auto id = draw.data["id"].toInt();
                previousModels.insert(id, matrixBytes(draw.data));
                velocityIds.insert(id);
            }
            for (auto it = previousModels.begin(); it != previousModels.end();) {
                if (!velocityIds.contains(it.key())) {
                    it = previousModels.erase(it);
                } else {
                    ++it;
                }
            }
            // 速度实例缓冲要到本帧速度 pass 才构建，因此清理必须放在 pass 之后；
            // 若放在 prepare() 之后会把上一帧缓冲当成未使用而每帧重建。
            for (auto it = velocityInstances.begin(); it != velocityInstances.end();) {
                if (it->used != generation) {
                    retire(it->buffer);
                    it = velocityInstances.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
};
class ItemRenderer : public QQuickRhiItemRenderer {
    Engine *backend = nullptr;
    std::unique_ptr<Renderer> renderer;
    QJsonObject frame, item;
    SceneDrawBuffers buffers;
    Engine::MusicTextures musicTextures;
    QImage cover;
    QString root;

  public:
    void synchronize(QQuickRhiItem *value) override {
        auto scene = static_cast<RhiScene *>(value);
        auto engine = scene->engine();
        backend = engine;
        if (engine) {
            frame = engine->frame();
            buffers = engine->drawBuffers();
            musicTextures = engine->musicTextures();
            item = engine->current();
            cover = engine->cover();
            root = engine->root();
        }
    }
    void initialize(QRhiCommandBuffer *) override {
        if (renderer && renderer->rhi != rhi()) {
            renderer.reset();
        }
    }
    void render(QRhiCommandBuffer *cb) override {
        if (frame.isEmpty() || (frame["mode"] == "boot" && frame["time"].toDouble() < 21.9)) {
            cb->beginPass(renderTarget(), Qt::transparent, {1, 0});
            cb->endPass();
            return;
        }
        if (!renderer) {
            renderer = std::make_unique<Renderer>(root, rhi());
        }
        renderer->nativeBuffers = buffers;
        const auto previousBindings = renderer->bindingCreations;
        const auto previousUploads = renderer->coverUploads;
        const auto previousDrawCalls = renderer->drawCalls;
        renderer->render(cb, frame, renderTarget()->pixelSize(), item, cover, renderTarget(), musicTextures);
        if (backend) {
            backend->recordRender(
                renderer->resources.size() + renderer->sized.size() + renderer->transient.size(),
                renderer->bindingCreations - previousBindings, renderer->musicTextureBytes(),
                renderer->coverUploads - previousUploads, renderer->drawCalls - previousDrawCalls);
        }
    }
};
RhiScene::RhiScene(QQuickItem *parent) : QQuickRhiItem(parent) {
    setAlphaBlending(true);
    // 时间性抗锯齿（FSR3/TAA）必须逐帧推进历史，否则抖动相位与历史累积都停住。
    // 定时器常驻但只在对应模式下真正发重绘：这样切换 AA 模式不需要重启，
    // 其它模式下也只是每 16ms 读一次环境变量。
    m_temporalTimer = new QTimer(this);
    m_temporalTimer->setInterval(16);
    connect(m_temporalTimer, &QTimer::timeout, this, [this] {
        const auto mode = qEnvironmentVariable("RHINE_AA_MODE");
        // FSR3/TAA 靠帧间累积，必须持续重绘。其余模式只在"模式刚变化"时补一次重绘：
        // 切到 FSR1 后如果不触发 update()，resize() 不会跑、fsrInput/fsrOutput 不会创建，
        // fsr1Active 就恒为 false，模式看起来像没生效。
        if (mode == "fsr3" || mode == "taa" || mode != m_temporalMode) {
            m_temporalMode = mode;
            update();
        }
    });
    m_temporalTimer->start();
}
void RhiScene::setEngine(Engine *value) {
    if (value == m_engine) {
        return;
    }
    if (m_engine) {
        disconnect(m_engine, nullptr, this, nullptr);
    }
    m_engine = value;
    if (value) {
        connect(value, &Engine::sceneUpdated, this, [this] { update(); });
    }
    emit engineChanged();
    update();
}
QQuickRhiItemRenderer *RhiScene::createRenderer() {
    return new ItemRenderer;
}
/**
 * 使用同一份场景数据生成参考图，供离线渲染比较使用。
 */
QImage renderReference(const QString &root, const QJsonObject &frame, const QJsonObject &item,
                       const QImage &cover, QSize size, const SceneDrawBuffers &buffers, quint64 *drawCalls) {
    // Vulkan 实例与离屏表面都必须比 QRhi 活得久：先声明者后析构。
#if RHINE_HAVE_VULKAN_TYPES
    QVulkanInstance vulkan;
    vulkan.setApiVersion(QVersionNumber(1, 0));
    bool vulkanReady = false;
    if (vulkan.create()) {
        // 与 main.cpp 的探测一致：直接枚举物理设备，不依赖 QVulkanWindow。
        quint32 devices = 0;
        vulkanReady = vulkan.functions()->vkEnumeratePhysicalDevices(vulkan.vkInstance(), &devices,
                                                                     nullptr) == VK_SUCCESS &&
                      devices > 0;
    }
#endif
    // 离屏表面先声明，析构顺序才能晚于 QRhi：GL 后端在析构时会切回上下文。
    std::unique_ptr<QOffscreenSurface> surface;
    std::unique_ptr<QRhi> rhi;
#if RHINE_HAVE_VULKAN_TYPES
    if (vulkanReady) {
        QRhiVulkanInitParams params;
        params.inst = &vulkan;
        rhi.reset(QRhi::create(QRhi::Vulkan, &params,
                               qEnvironmentVariableIntValue("RHINE_SOFTWARE_GPU") == 1
                                   ? QRhi::PreferSoftwareRenderer
                                   : QRhi::Flags{}));
    }
#endif
#ifdef Q_OS_MACOS
    if (!rhi) {
        QRhiMetalInitParams params;
        rhi.reset(QRhi::create(QRhi::Metal, &params));
    }
#elif defined(Q_OS_WIN)
    if (!rhi) {
        // Windows 上不再回退 D3D11：项目已彻底移除该后端（FSR3 只支持 DX12/Vulkan，
        // 保留 D3D11 会让同一份着色器与管线在两条完全不同的能力集上各走一遍）。
        QRhiD3D12InitParams params;
        rhi.reset(QRhi::create(QRhi::D3D12, &params,
                               qEnvironmentVariableIntValue("RHINE_SOFTWARE_GPU") == 1
                                   ? QRhi::PreferSoftwareRenderer
                                   : QRhi::Flags{}));
    }
#else
    if (!rhi) {
        // 着色器按 GLSL 150 打包（GL 3.2 核心档）；默认格式会退到 2.1 并找不到可用版本。
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setDepthBufferSize(24);
        format.setStencilBufferSize(8);
        surface.reset(QRhiGles2InitParams::newFallbackSurface(format));
        QRhiGles2InitParams params;
        params.format = format;
        params.fallbackSurface = surface.get();
        rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
    }
#endif
    if (!rhi) {
        qFatal("No RHI device for reference render");
    }
    QRhiCommandBuffer *cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
        qFatal("Offscreen frame failed");
    }
    Renderer renderer(root, rhi.get());
    renderer.nativeBuffers = buffers;
    renderer.render(cb, frame, size, item, cover);
    if (drawCalls) {
        *drawCalls = renderer.drawCalls;
    }
    QRhiReadbackResult result;
    bool completed = false;
    result.completed = [&] { completed = true; };
    auto batch = rhi->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(renderer.target.texture), &result);
    cb->resourceUpdate(batch);
    rhi->endOffscreenFrame();
    rhi->finish();
    if (!completed) {
        qFatal("RHI readback failed");
    }
    QImage image(reinterpret_cast<const uchar *>(result.data.constData()), result.pixelSize.width(),
                 result.pixelSize.height(), QImage::Format_RGBA8888);
    if (!rhi->isYUpInFramebuffer()) {
        return image.copy();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return image.flipped(Qt::Vertical);
#else
    return image.mirrored(false, true);
#endif
}
