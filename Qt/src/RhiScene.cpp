#include <QtGlobal>
#include "RhiScene.h"
#ifdef Q_OS_DARWIN
#include "render/MetalFxBackend.h"
#endif
#include "render/MotionPass.h"
#include "render/RenderPolicy.h"
#include "render/ScenePostProcess.h"
#ifdef Q_OS_LINUX
#include "fg/FgFrameTap.h"
#endif
#include "fg/FgCapability.h"
#include <cstdio>
#include "DiscGeometry.h"
#include "Engine.h"
#include "Glyphs.h"
#include <QFile>
#include <QJsonDocument>
#include <QMatrix4x4>
#include <QPainter>
#include <QOffscreenSurface>
// Qt 6.8+ 的 qrhi.h 已提供各后端初始化参数，无需引用不存在的后端头文件。
#include <rhi/qrhi.h>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>) && !defined(Q_OS_DARWIN)
#include <QVulkanFunctions>
#include <QVulkanInstance>
#define RHINE_HAVE_VULKAN_TYPES 1
#else
#define RHINE_HAVE_VULKAN_TYPES 0
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

// 烘焙场景与最终合成共用唯一渲染入口。
namespace RhineRendering {
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
    PackedMatrix emitterWorld;
    PackedMatrix emitterInverse;
    QVector4D opticalLight;
    QVector4D emissionColor;
#ifdef Q_OS_LINUX
    PackedMatrix previousViewProjection;
    PackedMatrix currentViewProjection;
#endif
};
struct Surface {
    QVector4D color, physical, optics, attenuation, extra, flags, darkColor, emission;
};
struct Instance {
    PackedMatrix model;
    QVector4D state;
#ifdef Q_OS_LINUX
    // 上一帧模型矩阵：运动矢量随主渲染同一趟输出，不再需要第二趟几何。
    PackedMatrix previousModel;
#endif
};
#ifdef Q_OS_LINUX
static_assert(sizeof(Globals) == 832);
#else
static_assert(sizeof(Globals) == 704);
#endif
static_assert(sizeof(Surface) == 128);
#ifdef Q_OS_LINUX
static_assert(sizeof(Instance) == 144);
#else
static_assert(sizeof(Instance) == 80);
#endif
// 与主渲染路径一致：封面最高 1024²，带 mip 后单张约 5.3 MiB，预算需按此放宽。
static constexpr qint64 MUSIC_TEXTURE_BUDGET_BYTES = 64 * 1024 * 1024;
static QMatrix4x4 matrix(QJsonArray a, int offset = 0) {
    QMatrix4x4 m;
    for (int i = 0; i < 16; ++i) {
        m.data()[i] = float(a[offset + i].toDouble());
    }
    return m;
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
static QByteArray readOptional(QString root, QString name) {
    QFile f(root + "/" + name);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
static QShader shader(QString name) {
    name[0] = name[0].toUpper();
    QFile f(":/optical/optical" + name + ".qsb");
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
class Renderer {
  public:
    QRhi *rhi;
#ifdef Q_OS_DARWIN
    RhineRender::Settings settings;
    QJsonObject temporalFrame;
    RhineFg::FrameInputs *frameInputs = nullptr;
    std::unique_ptr<RhineRender::MetalFxTemporal> metalFx;
    QString implementation = QStringLiteral("off");
    QRhiTexture *postColor = nullptr;
#endif
    std::unique_ptr<RhineRender::MotionPass> motion;
#ifndef Q_OS_DARWIN
    std::unique_ptr<RhineRender::ScenePostProcess> postProcess;
    RhineFg::FrameInputs motionInputs;
#endif
    int sampleCount = 1;
    QString sizedAaMode;
    // A05：运动矢量附件当前是否真的分配了。它是目标形状的一部分，必须和尺寸/采样数/
    // AA 模式一起参与 resize() 的早退比较，否则切换 AA 模式后会沿用旧目标。
    bool motionTarget = false;
    QString root;
    std::vector<std::unique_ptr<QRhiResource>> resources, sized, transient;
    QHash<QString, Mesh> meshes;
    QRhiTexture *surfaceAtlas = nullptr;
    QRhiTexture *lightAtlas = nullptr;
    QRhiTexture *normalAtlas = nullptr;
    QRhiTexture *white = nullptr;
    QRhiTexture *depth = nullptr;
    QRhiBuffer *globalBuffer = nullptr, *floor = nullptr, *coverPlane = nullptr, *musicLabelPlane = nullptr,
               *discBody = nullptr, *discHub = nullptr, *caseDecoration = nullptr,
               *caseLetteringFront = nullptr, *caseLetteringSpine = nullptr;
    int discPrintCount = 0;
    int discBodyCount = 0;
    int discHubCount = 0;
    QRhiTexture *decorationTextureHigh = nullptr, *decorationTextureLow = nullptr,
                *placeholderCover = nullptr, *letteringTextureFront = nullptr,
                *letteringTextureSpine = nullptr;
    QRhiSampler *linear = nullptr;
    QRhiSampler *mipSampler = nullptr;
    QRhiSampler *volume = nullptr;
    Target color, target;
    QRhiResourceUpdateBatch *updates = nullptr;
    QHash<QString, QRhiGraphicsPipeline *> pipelines;
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
#ifdef Q_OS_LINUX
    // 运动矢量 MRT 所需的上一帧状态：逐 draw 的实例矩阵与上一帧 VP。
    QHash<int, QByteArray> previousInstanceMatrices;
    QMatrix4x4 previousViewProjection;
    QElapsedTimer frameClock;
    bool motionHistoryValid = false;
    QRhiTexture *motionVectors = nullptr;
    // 重投影插帧探针（RHINE_WARP_PROBE=1）：把当前场景颜色按运动矢量前推到 t 相位。
    // 与 FFX 插值不同，相位 t 可任意取（FFX 硬编码 0.5），且不需要第二趟几何。
    Target warp;
    QRhiShaderResourceBindings *warpBinding = nullptr;
    QRhiTexture *warpSource = nullptr;
    QRhiTexture *warpVectors = nullptr;
#endif
    QHash<QString, MaterialState> materials;
    QHash<QString, QRhiShaderResourceBindings *> postBindings;
    QHash<QString, QString> labelRevisions;
    QHash<QString, qint64> coverRevisions;
    SceneDrawBuffers nativeBuffers;
    quint64 generation = 0;
    quint64 bindingCreations = 0;
    quint64 coverUploads = 0;
    quint64 drawCalls = 0;
    QHash<QString, QRhiTexture *> labels, covers;
    QHash<qint64, QRhiTexture *> coverTextures;
    QRhiTexture *labelTexture = nullptr, *coverTexture = nullptr;
    QStringList textureOrder;
    QSet<QString> activeTextureKeys;
    QSize size;
    QSize viewSize;
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
        // uniform 每帧变化，只能走 Dynamic；类型与上传方式必须同时匹配。
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
                         bool resize = false) {
        return create(own(rhi->newTexture(format, s, 1, flags), resize));
    }
    QRhiTexture *texture3D(QSize s, int depth, QRhiTexture::Format format,
                           QRhiTexture::Flags flags = {}) {
        return create(own(rhi->newTexture(format, s.width(), s.height(), depth, 1, flags)));
    }
    void upload(QRhiTexture *t, QByteArray bytes) {
        QRhiTextureSubresourceUploadDescription data(bytes);
        updates->uploadTexture(t, QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, data)}));
    }
    /** 三维纹理按深度切片逐层上传；每层数据保持连续，避免后端推断错误的行跨度。 */
    void upload3D(QRhiTexture *t, const QByteArray &bytes, int depth) {
        const qsizetype slice = bytes.size() / depth;
        QVector<QRhiTextureUploadEntry> entries;
        entries.reserve(depth);
        for (int index = 0; index < depth; ++index) {
            entries.append(QRhiTextureUploadEntry(
                index, 0, QRhiTextureSubresourceUploadDescription(bytes.mid(index * slice, slice))));
        }
        QRhiTextureUploadDescription description;
        description.setEntries(entries.cbegin(), entries.cend());
        updates->uploadTexture(t, description);
    }
    /**
     * 逐层生成并上传 mip 链，与主渲染路径保持一致。
     * 后端对上传纹理的 generateMips 不一定填满每一层，缺层会让 GL 判为纹理不完整并采样为黑。
     */
    void uploadMipped(QRhiTexture *target, const QImage &image) {
        QVector<QRhiTextureUploadEntry> entries;
        auto level = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        for (int index = 0;; ++index) {
            entries.append(QRhiTextureUploadEntry(0, index, QRhiTextureSubresourceUploadDescription(level)));
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
    /**
     * 上传烘焙器生成的完整 mip 链。surface 的 mip 层在离线阶段按线性光平均，
     * 这里只按层切片，避免运行时再做一次 QImage 缩放破坏 sRGB/HDR 编码。
     */
    void uploadMippedBytes(QRhiTexture *target, const QByteArray &bytes, int size, bool compressed) {
        QVector<QRhiTextureUploadEntry> entries;
        qsizetype offset = 0;
        int width = size;
        int height = size;
        for (int level = 0;; ++level) {
            const qsizetype byteCount = compressed
                ? qsizetype(((width + 3) / 4) * ((height + 3) / 4) * 16)
                : qsizetype(width) * height * 4;
            if (offset + byteCount > bytes.size()) {
                qFatal("Truncated baked mip chain: level=%d need=%lld have=%lld", level,
                       static_cast<long long>(offset + byteCount), static_cast<long long>(bytes.size()));
            }
            entries.append(QRhiTextureUploadEntry(
                0, level, QRhiTextureSubresourceUploadDescription(bytes.mid(offset, byteCount))));
            offset += byteCount;
            if (width == 1 && height == 1) {
                break;
            }
            width = qMax(1, width / 2);
            height = qMax(1, height / 2);
        }
        if (offset != bytes.size()) {
            qWarning("Baked mip chain has %lld trailing bytes", static_cast<long long>(bytes.size() - offset));
        }
        QRhiTextureUploadDescription description;
        description.setEntries(entries.cbegin(), entries.cend());
        updates->uploadTexture(target, description);
    }
    Renderer(QString path, QRhi *device) : rhi(device), root(path) {
#ifndef Q_OS_DARWIN
        postProcess = std::make_unique<RhineRender::ScenePostProcess>(rhi);
#endif
        updates = rhi->nextResourceUpdateBatch();
        glyphs = std::make_unique<Glyphs>(root);
        globalBuffer =
            create(own(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Globals))));
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
        // 曲目标签与封面由 CPU 逐层上传 mip 链，采样需要三线性。
        mipSampler = sampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::ClampToEdge);
        // 三维光场的 W 轴必须 clamp，否则最顶层/最底层会跨切片重复。
        volume = create(own(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
                                            QRhiSampler::ClampToEdge)));
        if (!rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures)) {
            qFatal("RHI backend does not support 3D textures required by the baked light field");
        }
        // 表面图集重排为 1024²：正面/背面 448²，侧面 56x448，顶/底 448x56。
        // RGB 以 sRGB8 存储线性辐亮度/4，着色器手工解码后乘回 4；法线响应仍为 RGBA16F。
        // mip 链在烘焙器里按线性光生成；BC7 可用时走压缩路径，否则回退 RGBA8。
        const auto surfaceFlags = QRhiTexture::MipMapped | QRhiTexture::UsedAsCompressedAtlas;
        const QByteArray surfaceBc7 = readOptional(root, "archive-baked-surface-bc7.bin");
        const bool bc7Surface = !surfaceBc7.isEmpty() &&
                                rhi->isTextureFormatSupported(QRhiTexture::BC7, surfaceFlags);
        if (qEnvironmentVariableIsSet("RHINE_DEBUG_ATLAS")) {
            std::fprintf(stderr, "Surface atlas: %s, 1024^2, full mip chain\n",
                         bc7Surface ? "BC7" : "RGBA8");
            std::fflush(stderr);
        }
        if (bc7Surface) {
            surfaceAtlas = texture({1024, 1024}, QRhiTexture::BC7, surfaceFlags);
            uploadMippedBytes(surfaceAtlas, surfaceBc7, 1024, true);
        } else {
            surfaceAtlas = texture({1024, 1024}, QRhiTexture::RGBA8, QRhiTexture::MipMapped);
            uploadMippedBytes(surfaceAtlas, read(root, "archive-baked-surface.bin"), 1024, false);
        }
        // 光场改为 576x96x24 的三维纹理，硬件三线性插值替代手工两层采样。
        lightAtlas = texture3D({576, 96}, 24, QRhiTexture::RGBA8,
                               QRhiTexture::ThreeDimensional);
        upload3D(lightAtlas, read(root, "archive-baked-light.bin"), 24);
        normalAtlas = texture({128, 256}, QRhiTexture::RGBA16F);
        upload(normalAtlas, read(root, "archive-baked-normal.bin"));
        white = texture({1, 1}, QRhiTexture::RGBA8);
        upload(white, QByteArray(4, char(255)));
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
        // 与主渲染路径一致：背景按需分两档（低档 1 倍 / 高档 2 倍），文字独立成片 16 倍。
        const QColor ink(65, 70, 62, 170), accent(119, 123, 96, 195);
        const auto paintDecoration = [&](QPainter &painter) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(ink, 1.4));
            for (auto point : {QPointF(75, 60), QPointF(930, 60), QPointF(75, 685), QPointF(930, 685)}) {
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
        decorationTextureHigh = texture(decorationHigh.size(), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
        uploadMipped(decorationTextureHigh, decorationHigh);
        QImage decorationLow(1000, 740, QImage::Format_RGBA8888_Premultiplied);
        decorationLow.fill(Qt::transparent);
        {
            QPainter painter(&decorationLow);
            paintDecoration(painter);
        }
        decorationTextureLow = texture(decorationLow.size(), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
        uploadMipped(decorationTextureLow, decorationLow);
        // 装饰文字独立成片，按原 1000×740 画布坐标放大 16 倍绘制。
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
        letteringTextureFront = lettering(QRect(120, 664, 220, 46), [&](QPainter &text) {
            glyphs->text(text, "MUSIC ARCHIVE / 001", 130, 688, 15, "regular", .5, ink);
        });
        letteringTextureSpine = lettering(QRect(62, 348, 46, 128), [&](QPainter &text) {
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
            // 与主渲染路径一致的八分量顶点：位置、法线、UV。
            float data[] = {v.x(), v.y(), z, 0, 0, 1, uw ? (v.x() - ux) / uw : 0, uh ? (v.y() - uy) / uh : 0};
            vertices.insert(vertices.end(), std::begin(data), std::end(data));
        }
        return buffer(QByteArray(reinterpret_cast<const char *>(vertices.data()), vertices.size() * 4),
                      QRhiBuffer::VertexBuffer);
    }
#ifdef Q_OS_LINUX
    Target makeTarget(QRhiTexture *texture, QRhiTexture *depth = nullptr, QRhiTexture *resolve = nullptr,
                      QRhiTexture *vectors = nullptr) {
#else
    Target makeTarget(QRhiTexture *texture, QRhiTexture *depth = nullptr, QRhiTexture *resolve = nullptr) {
#endif
        QRhiTextureRenderTargetDescription desc;
        if (texture) {
            QRhiColorAttachment attachment(texture);
            if (resolve) {
                attachment.setResolveTexture(resolve);
            }
#ifdef Q_OS_LINUX
            if (vectors) {
                // 第二颜色附件：运动矢量与颜色同一趟栅格化输出。
                desc.setColorAttachments({attachment, QRhiColorAttachment(vectors)});
            } else {
                desc.setColorAttachments({attachment});
            }
#else
            desc.setColorAttachments({attachment});
#endif
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
    void clearPostBindings() {
        for (auto binding : std::as_const(postBindings)) {
            retire(binding);
        }
        postBindings.clear();
#ifdef Q_OS_DARWIN
        postColor = nullptr;
#endif
    }
    /** 仅分配颜色和深度；烘焙增强不创建实时透射、阴影、AO、景深或光晕目标。 */
    void resize(QSize value, bool needsReadbackTarget) {
#ifdef Q_OS_DARWIN
        const auto nextSize = settings.renderSize(value);
        if (viewSize == value && size == nextSize) {
            return;
        }
#else
        const auto nextSize = postProcess->renderSize(value);
        const int nextSamples = postProcess->samples();
        const auto nextMode = qEnvironmentVariable("RHINE_AA_MODE");
        // A05：只有确实有人读时才挂运动矢量附件。
        //   · 多采样目标本来就没有第二个 draw buffer 可写，维持原来的让路策略；
        //   · 单采样时看配置（AA 模式 + FG 开关）而不是 FSR 运行期可用性，否则能力
        //     探测翻一次就重建整帧目标；重投影探针也要读矢量图，不能一并裁掉。
        const bool wantMotion = nextSamples <= 1 &&
                                (postProcess->motionConfigured() ||
                                 qEnvironmentVariableIsSet("RHINE_WARP_PROBE"));
        if (viewSize == value && size == nextSize && sampleCount == nextSamples &&
            sizedAaMode == nextMode && motionTarget == wantMotion) {
            return;
        }
        if (motionTarget != wantMotion) {
            // 现场核对用：只在真的切换时打一行，不按帧刷屏。
            qInfo().noquote() << "运动矢量目标:" << (wantMotion ? "分配" : "不分配")
                              << "| AA" << nextMode << "| samples" << nextSamples;
        }
        sampleCount = nextSamples;
        sizedAaMode = nextMode;
        motionTarget = wantMotion;
#endif
        viewSize = value;
        size = nextSize;
        invalidateBindings();
        clearPostBindings();
        pipelines.clear();
        pipelineLayouts.clear();
        sized.clear();
        auto rtFlags = QRhiTexture::RenderTarget;
        depth = create(own(rhi->newTexture(QRhiTexture::D32F, size, sampleCount, rtFlags), true));
        auto attachment =
            create(own(rhi->newTexture(QRhiTexture::RGBA16F, size, sampleCount, rtFlags), true));
        auto resolved = sampleCount > 1 ? texture(size, QRhiTexture::RGBA16F, rtFlags, true) : nullptr;
#ifdef Q_OS_LINUX
        // 运动矢量只在 wantMotion 时分配：off/fxaa/msaa 或 FG 关闭时少一张 RGBA16F，
        // 也少一遍第二附件的写出带宽。是否带第二个颜色附件由 motionVectors 指针表达：
        // 为空时 makeTarget 走单附件，pipeline() 据此选 surfaceNoMrt 变体（GL 只有一个
        // draw buffer，不能在片元着色器里声明 location=1）。
        motionVectors = motionTarget
                            ? create(own(rhi->newTexture(QRhiTexture::RGBA16F, size, 1, rtFlags), true))
                            : nullptr;
        color = makeTarget(attachment, depth, resolved, motionVectors);
#else
        color = makeTarget(attachment, depth, resolved);
#endif
        target = {};
        if (needsReadbackTarget) {
            // 读回目标固定为窗口尺寸：超采样只放大内部目标，合成通道按目标像素尺寸缩回。
            target = makeTarget(
                texture(value, QRhiTexture::RGBA8, rtFlags | QRhiTexture::UsedAsTransferSource, true));
        }
    }
    /** 从选中壳体的原生矩阵定位光源，并将场景参数复制到本分支的独立常量缓冲区。 */
    Globals globals(QJsonObject f) {
        auto projection = matrix(f["projection"].toArray());
        const auto jitter = f["jitter"].toArray();
#ifdef Q_OS_DARWIN
        const bool temporal = settings.temporal();
#else
        const bool temporal = postProcess->temporal();
#endif
        if (temporal && jitter.size() == 2) {
            RhineRender::applyJitter(projection, {jitter[0].toDouble(), jitter[1].toDouble()}, size);
        }
        auto theme = n(f["draws"].toArray().first().toObject(), "theme");
        QMatrix4x4 emitter;
        bool hasEmitter = false;
        for (const auto &entry : f["draws"].toArray()) {
            const auto draw = entry.toObject();
            // 查看封面页的装配体部件不带"选中"标记，但同样是唱片壳，
            // 只认 musicSelected 会让整页 lightPower 归零、光照全丢。
            if (draw["name"] == "Frosted_Polymer" && !draw["shadowOnly"].toBool() &&
                (draw["musicSelected"].toBool() || f["viewer"].toBool())) {
                const auto bytes = matrixBytes(draw);
                if (bytes.size() >= 64) {
                    memcpy(emitter.data(), bytes.constData(), 64);
                    hasEmitter = true;
                    break;
                }
            }
        }
        const float lightPower = hasEmitter && f["opticalLightEnabled"].toBool(true) ? 1.f : 0.f;
        return {matrix(f["view"].toArray()),
                projection,
                {},
                {},
                {},
                {},
                v4(vector(f["eye"])),
                v4(vector(f["fogColor"])),
                {n(f, "fogNear"), n(f, "fogFar"), n(f, "focus"), n(f, "aperture")},
                {float(size.width()), float(size.height()), theme, f["viewer"].toBool() ? 1.f : 0.f},
                {n(f, "near"), n(f, "far"), n(f, "detail"), f["reference"].toBool() ? 1.f : 0.f},
                rhi->clipSpaceCorrMatrix(),
                {rhi->isYUpInFramebuffer() ? 1.f : 0.f, 0, 0, 0},
                emitter,
                emitter.inverted(),
                // 查看封面页不再叠加原版像素，光照强度无需再压低，否则整页偏暗发平。
                {lightPower, 1.f, 0.62f, 0.f},
                {1.f, 0.72f, 0.38f, 1.f}};
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
        // 诊断用：按绘制序号上色，把每个部件染成互不相同的纯色，用于在截图里认出
        // 某个像素究竟由哪个部件画出。仅在 RHINE_DEBUG_TINT 下生效。
        const bool tint = qEnvironmentVariableIsSet("RHINE_DEBUG_TINT");
        if (tint) {
            const int bits = d["debugIndex"].toInt();
            d["color"] =
                QJsonArray{(bits & 1) ? 1.0 : 0.15, (bits & 2) ? 1.0 : 0.15, (bits & 4) ? 1.0 : 0.15, 1.0};
        }
        bool optical = name.startsWith("Optical_Glass") || name == "Optical_Bridge_Glass";
        float type = d["mesh"].toInt() == -2                               ? 6
                     : d["mesh"].toInt() == -3 || d["mesh"].toInt() == -4  ? 8
                     : d["mesh"].toInt() == -6 || d["mesh"].toInt() == -7  ? 7
                     : d["label"].toBool()                                 ? 1
                     : name == "Frosted_Polymer" || name == "Optical_Film" ? 3
                     : name == "Ivory_Edges"                               ? 4
                     : name == "Optical_Diffuser"                          ? 5
                     : (name.startsWith("Optical_Film") || name.startsWith("Optical_Glass") ||
                        name == "Optical_Bridge_Glass")
                         ? 9
                     : !d["instanced"].toBool() && !d["floor"].toBool() ? 2
                                                                        : 0;
        if (tint) {
            type = 2;
        }
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
                {d["musicSelected"].toBool() ? 1.f : 0.f, 0, 0, 0}};
    }
    QList<QJsonObject> musicDraws(QJsonObject f) {
        // Optical_Film 是包在唱片外的一层光学薄膜，详情页与查看封面页靠它出玻璃反射；
        // 之前因为没有折射路径被整层丢弃，画面就少了一层。
        // 只丢弃确实会穿帮的部件；Optical_Edges / Subsurface_Optics / Optical_Film_Edge
        // 是最外那层透明光学包裹，丢掉它们外壳会少一层，必须保留。
        static const QStringList removed = {"Internal_Ceramic", "Amber_Optical_Inlay", "Champagne_Index",
                                            "Printed_Label"};
        QList<QJsonObject> result;
        for (auto v : f["draws"].toArray()) {
            auto d = v.toObject();
            const auto name = d["name"].toString();
            if (removed.contains(name)) {
                continue;
            }
            const bool shell =
                name == "Frosted_Polymer" || name == "Ivory_Edges" || name == "Optical_Diffuser";
            if (name == "Titanium_Fasteners") {
                // 金属反射的暗底色也固定烘焙，避免取消实时环境光后螺钉变成白色圆点。
                d["color"] = QJsonArray{0.065, 0.075, 0.08};
            }
            if (name.startsWith("Optical_Film") || name.startsWith("Optical_Glass") ||
                name == "Optical_Bridge_Glass") {
                // 薄膜只留高光与厚度色，本体几乎全透，必须走透明队列并关闭深度写入。
                d["transparent"] = true;
                d["depthWrite"] = false;
                d["side"] = 2;
                d["opacity"] = 0.35;
            }
            if (shell) {
                // 固定透光外观来自烘焙图集；只有选中外壳用普通透明混合显示动态封面。
                const bool selectedShell = name == "Frosted_Polymer" && d["musicSelected"].toBool();
                d["transmission"] = 0;
                // 背景三只壳体都是闭合实体，背面永远不可见：单面剔除省掉约一半三角形。
                // 选中卡片保留双面，透明 Frosted 壳与装配体部件仍需看到内侧。
                d["side"] = d["instanced"].toBool() ? 0 : 2;
                d["transparent"] = selectedShell;
                d["depthWrite"] = !selectedShell;
                double opacity = 1;
                if (selectedShell) {
                    opacity = detailMode ? 0.20 : 0.62;
                    if (f["viewer"].toBool()) {
                        opacity = 0.14;
                    }
                }
                d["opacity"] = opacity;
            }
            d["debugIndex"] = int(result.size());
            result << d;
            // 密集背景采用透光档案壳，不在每个占位实例中塞入不透明的白色盘体。
            if (name != "Frosted_Polymer" || d["shadowOnly"].toBool() || d["instanced"].toBool()) {
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
            if (const auto previous = labels.value(key)) {
                invalidateBindings(previous);
            }
            // 覆盖同一 key 时退休的是该 key 的旧对象，而非上一首当前曲目的对象。
            retire(labels.take(key));
            // 与主渲染路径一致：绘制按 1024×440 排版，上传前减半以匹配实际屏幕尺寸。
            const auto label =
                glyphs->label(value).scaled(512, 220, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            labels[key] = texture(label.size(), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
            uploadMipped(labels[key], label);
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
            if (const auto label = labels.value(old)) {
                invalidateBindings(label);
            }
            retire(labels.take(old));
            releaseCover(old);
            labelRevisions.remove(old);
        }
    }
    qint64 musicTextureBytes() const {
        // 与主渲染路径一致：mip 链使实际占用为基层的一又三分之一。
        const auto bytes = [](QRhiTexture *texture) {
            const auto dimensions = texture->pixelSize();
            return qint64(dimensions.width()) * dimensions.height() * 4 * 4 / 3;
        };
        qint64 total = 0;
        for (const auto texture : labels) {
            total += bytes(texture);
        }
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
    QRhiGraphicsPipeline *pipeline(QString kind, QRhiShaderResourceBindings *srb,
                                   QRhiRenderPassDescriptor *rp, bool back = false, bool doubleSided = false,
                                   bool depthWrite = true, bool blend = false) {
        auto key = kind + QString("/%1/%2/%3/%4/%5")
                              .arg(quintptr(rp))
                              .arg(back)
                              .arg(doubleSided)
                              .arg(depthWrite)
                              .arg(blend);
        if (pipelines.contains(key)) {
            return pipelines[key];
        }
        auto p = own(rhi->newGraphicsPipeline(), true);
        const bool geo = kind == "surface";
#ifdef Q_OS_LINUX
        // OpenGL 后端：场景目标只有一个 draw buffer，片元着色器不能声明 location=1 的第二个输出，
        // 否则驱动会丢弃整块 fragment 结果（实测 3D 层全黑）。Vulkan 走 MRT 单趟。
        const bool surfaceMrt = geo && motionVectors != nullptr &&
                                rhi->backend() != QRhi::OpenGLES2;
        const QString fragmentName =
            (geo && !surfaceMrt) ? QStringLiteral("surfaceNoMrt") : kind;
#endif
        p->setShaderStages({{QRhiShaderStage::Vertex, shader(geo ? "geometry.vert" : "quad.vert")},
#ifdef Q_OS_LINUX
                            {QRhiShaderStage::Fragment, shader(fragmentName + ".frag")}});
#else
                            {QRhiShaderStage::Fragment, shader(kind + ".frag")}});
#endif
        if (geo) {
            QRhiVertexInputLayout layout;
#ifdef Q_OS_LINUX
            layout.setBindings({{32}, {144, QRhiVertexInputBinding::PerInstance}});
#else
            layout.setBindings({{32}, {80, QRhiVertexInputBinding::PerInstance}});
#endif
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                  {0, 1, QRhiVertexInputAttribute::Float3, 12},
                                  {0, 2, QRhiVertexInputAttribute::Float2, 24},
                                  {1, 3, QRhiVertexInputAttribute::Float4, 0},
                                  {1, 4, QRhiVertexInputAttribute::Float4, 16},
                                  {1, 5, QRhiVertexInputAttribute::Float4, 32},
                                  {1, 6, QRhiVertexInputAttribute::Float4, 48},
#ifdef Q_OS_LINUX
                                  {1, 7, QRhiVertexInputAttribute::Float4, 64},
                                  {1, 8, QRhiVertexInputAttribute::Float4, 80},
                                  {1, 9, QRhiVertexInputAttribute::Float4, 96},
                                  {1, 10, QRhiVertexInputAttribute::Float4, 112},
                                  {1, 11, QRhiVertexInputAttribute::Float4, 128}});
#else
                                  {1, 7, QRhiVertexInputAttribute::Float4, 64}});
#endif
            p->setVertexInputLayout(layout);
            p->setDepthTest(true);
            p->setDepthWrite(depthWrite);
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
#ifdef Q_OS_LINUX
        // MRT：场景目标除颜色外还挂着运动矢量附件，必须为**每个**颜色附件声明 blend。
        // 只声明一个时 location=1 的写出会被静默丢弃（表现为运动矢量恒为 0）。
        if (surfaceMrt) {
            p->setTargetBlends({b, QRhiGraphicsPipeline::TargetBlend{}});
        } else {
            p->setTargetBlends({b});
        }
#else
        p->setTargetBlends({b});
#endif
        if (!pipelineLayouts.contains(kind)) {
            auto layout = own(rhi->newShaderResourceBindings(), true);
            const auto stages =
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
            std::vector<QRhiShaderResourceBinding> values = {
                QRhiShaderResourceBinding::uniformBuffer(0, stages, globalBuffer)};
            if (geo) {
                values.push_back(QRhiShaderResourceBinding::uniformBuffer(1, stages, globalBuffer));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage, surfaceAtlas, mipSampler));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    3, QRhiShaderResourceBinding::FragmentStage, lightAtlas, volume));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    4, QRhiShaderResourceBinding::FragmentStage, white, linear));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    5, QRhiShaderResourceBinding::FragmentStage, white, linear));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    6, QRhiShaderResourceBinding::FragmentStage, normalAtlas, linear));
            } else {
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage, color.texture, linear));
            }
            layout->setBindings(values.begin(), values.end());
            create(layout);
            pipelineLayouts[kind] = layout;
        }
        p->setSampleCount(geo ? sampleCount : 1);
        p->setShaderResourceBindings(pipelineLayouts[kind]);
        p->setRenderPassDescriptor(rp);
        create(p);
        return pipelines[key] = p;
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
    bool detailMode = false;
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
#ifdef Q_OS_LINUX
        // 上一帧实例按平移列配对：列表滚动后顺序会变，按索引直接配对会给出错误速度。
        QHash<QByteArray, int> previousTranslations;
        const auto previousBytes = previousInstanceMatrices.value(draw["id"].toInt());
        for (int index = 0; (index + 1) * 64 <= previousBytes.size(); ++index) {
            previousTranslations.insert(previousBytes.mid(index * 64 + 48, 16), index);
        }
        QHash<int, int> previousMatched;
        for (int index = 0; (index + 1) * 64 <= matrices.size(); ++index) {
            const int match = previousTranslations.value(matrices.mid(index * 64 + 48, 16), -1);
            if (match >= 0) {
                previousMatched.insert(index, match);
            }
        }
#endif
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
            // state.w 标记"被聚焦的主体"：详情页与查看封面页只有焦点唱片用玻璃壳与主光。
            // 详情页背景仍是实例化的唱片墙，若按整帧置位会把这些卡片一起改掉。
            const bool hero = detailMode && !draw["instanced"].toBool();
            value.state = {n(draw, "quality"), n(draw, "clarity"), theme, hero ? 1.f : 0.f};
#ifdef Q_OS_LINUX
            value.previousModel = PackedMatrix();
            const auto previous = previousInstanceMatrices.value(draw["id"].toInt());
            const int previousIndex = previousMatched.value(i, -1);
            if (previousIndex >= 0 && (previousIndex + 1) * 64 <= previous.size()) {
                memcpy(value.previousModel.elements, previous.constData() + previousIndex * 64, 64);
            } else if ((i + 1) * 64 <= matrices.size()) {
                memcpy(value.previousModel.elements, matrices.constData() + i * 64, 64);
            }
#endif
            values.push_back(value);
        }
        const QByteArray bytes(reinterpret_cast<const char *>(values.data()),
                               values.size() * sizeof(Instance));
        return dynamicBuffer(state, bytes, QRhiBuffer::VertexBuffer);
    }
    struct Draw {
        QJsonObject data;
        QRhiBuffer *vertices, *indices, *instances;
        QRhiTexture *label;
        QRhiTexture *cover;
        int start;
        int count;
        QRhiShaderResourceBindings *bindings;
#ifdef Q_OS_LINUX
        // arena 内偏移；instances 为 nullptr 时生效（0 也合法，表示 arena 起点）。
        int instanceOffset = 0;
#endif
    };
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
            const auto material = surface(d);
            const auto mb = dynamicBuffer(
                state.uniform, QByteArray(reinterpret_cast<const char *>(&material), sizeof(material)),
                QRhiBuffer::UniformBuffer);
            const auto key =
                d["musicSelected"].toBool() ? musicItem["key"].toString() : d["musicKey"].toString();
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
            // 查看封面页的绘制来自装配体，key 不是曲目 key，直接查表会落回占位图，
            // 表现成"封面没贴上去、只看到烘焙壳体"。查不到时用当前曲目的封面。
            const auto currentCover = covers.value(musicItem["key"].toString("empty"), placeholderCover);
            const auto cover = mesh == -2 ? covers.value(key, currentCover) : placeholderCover;
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
                     QRhiShaderResourceBinding::uniformBuffer(1, stages, mb),
                     QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                               surfaceAtlas, mipSampler),
                     QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                               lightAtlas, volume),
                     QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage,
                                                               label, mipSampler),
                     QRhiShaderResourceBinding::sampledTexture(5, QRhiShaderResourceBinding::FragmentStage,
                                                               cover, mipSampler),
                     QRhiShaderResourceBinding::sampledTexture(6, QRhiShaderResourceBinding::FragmentStage,
                                                               normalAtlas, linear)},
                    false);
            }
            result.append({d, vertices, indices, instance(d), label, cover, start, count, state.bindings});
        }
        return result;
    }
    QHash<QByteArray, BufferState> batchInstances;
    /**
     * 把同几何、同材质的烘焙背景壳体合并为实例批次。
     * 透明与透射物体保留原排序；每个实例继续使用原来的矩阵、主题和揭示状态。
     */
    QList<Draw> batchDraws(const QList<Draw> &input) {
        QList<Draw> result;
        QList<QByteArray> bytes;
        QList<QByteArray> keys;
        QHash<QByteArray, int> groups;
        QHash<QByteArray, int> groupSlots;
        for (const auto &draw : input) {
            const auto &data = draw.data;
            const auto material = surface(data);
            const bool eligible =
                !data["transparent"].toBool() && material.flags.x() != 2 && data["depthWrite"].toBool();
            if (!eligible) {
                result.append(draw);
                bytes.append(QByteArray());
                keys.append(QByteArray());
                continue;
            }
            auto key = QByteArray("baked/") + QByteArray::number(quintptr(draw.vertices)) + '/' +
                       QByteArray::number(quintptr(draw.indices)) + '/' + QByteArray::number(draw.start) +
                       '/' + QByteArray::number(draw.count) + '/' + QByteArray::number(data["side"].toInt()) +
                       '/' + QByteArray::number(data["order"].toInt()) + '/' +
                       QByteArray::number(quintptr(draw.label)) + '/' +
                       QByteArray::number(quintptr(draw.cover)) + ':';
            const auto bufferKey = key;
            key.append(reinterpret_cast<const char *>(&material), sizeof(material));
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
    void geometry(QRhiCommandBuffer *cb, const QList<Draw> &draws) {
        // 所有后端共用同一条「先去重再提交」的路径：先把每个 draw 的 pipeline 算出来
        // （纯函数、无副作用），再按状态分组，避免每 draw 重复绑定同一套状态。
        // 原来这条路径只在 Linux 上生效，其余后端仍逐 draw 无条件绑定三样状态。
        struct Item {
            const Draw *draw;
            QRhiGraphicsPipeline *pass;
            bool transparent;
        };
        QList<Item> items;
        items.reserve(draws.size());
        for (const auto &draw : draws) {
            const auto &data = draw.data;
            if (data["shadowOnly"].toBool()) {
                continue;
            }
            const bool hashed = surface(data).flags.x() == 2;
            const bool blend = hashed || data["transparent"].toBool();
            const bool depthWrite = hashed || !blend || data["depthWrite"].toBool();
            auto *pass = pipeline("surface", draw.bindings, color.rp, false,
                                  data["side"].toInt() == 2, depthWrite, blend);
            items.append({&draw, pass, data["transparent"].toBool()});
        }
        // 不透明段按 pipeline 稳定排序（同 pipeline 内保持原顺序）；透明段原样接在后面。
        // 两段分段处理保证透明物体的混合顺序与改动前完全一致。
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
            if (a.transparent != b.transparent) {
                return !a.transparent;  // 不透明在前
            }
            if (a.transparent) {
                return false;  // 透明段保持原序（stable_sort 下相等即不改顺序）
            }
            return reinterpret_cast<quintptr>(a.pass) < reinterpret_cast<quintptr>(b.pass);
        });
        const QRhiViewport viewport{0, 0, float(size.width()), float(size.height())};
        bool viewportSet = false;
        QRhiGraphicsPipeline *boundPass = nullptr;
        QRhiShaderResourceBindings *boundBindings = nullptr;
        int pipelineBinds = 0, bindingsBinds = 0, viewportSets = 0;
        for (const auto &item : items) {
            const auto &draw = *item.draw;
            const auto &data = draw.data;
            if (item.pass != boundPass) {
                cb->setGraphicsPipeline(item.pass);
                ++pipelineBinds;
                boundPass = item.pass;
                // 视口是动态状态，绑定新 pipeline 后必须重设一次；同一 pipeline 连续绘制时
                // 尺寸不会变，不必每 draw 都调（原来每 draw 一次是纯浪费）。
                cb->setViewport(viewport);
                ++viewportSets;
                viewportSet = true;
            } else if (!viewportSet) {
                cb->setViewport(viewport);
                ++viewportSets;
                viewportSet = true;
            }
            if (draw.bindings != boundBindings) {
                cb->setShaderResources(draw.bindings);
                ++bindingsBinds;
                boundBindings = draw.bindings;
            }
            // 实例数据来自 arena 时用 offset 寻址；否则回落到 draw 自带的 buffer。
            const QRhiCommandBuffer::VertexInput inputs[] = {{draw.vertices, 0}, {draw.instances, 0}};
            cb->setVertexInput(0, 2, inputs, draw.indices, 0, QRhiCommandBuffer::IndexUInt32);
            if (draw.indices) {
                cb->drawIndexed(draw.count, data["count"].toInt(), draw.start);
            } else {
                cb->draw(draw.count, data["count"].toInt(), draw.start);
            }
            ++drawCalls;
        }
        // 排障：状态重绑次数（RHINE_DRAW_PROBE=1，每 120 帧一次）。去重路径现在全平台生效，
        // 探针也就跟着全平台可用；它只在环境变量打开时才做计数，默认零成本。
        static const bool drawProbe = qEnvironmentVariableIntValue("RHINE_DRAW_PROBE") == 1;
        static int probeCounter = 0;
        if (drawProbe && (++probeCounter % 120) == 0) {
            qInfo().noquote() << "绘制探针: draw" << items.size() << "| pipeline 绑定" << pipelineBinds
                              << "| srb 绑定" << bindingsBinds << "| viewport 设置" << viewportSets
                              << "| 实例buffer" << instances.size() << "| 批次buffer" << batchInstances.size();
        }
    }
    // 重投影探针（warpFrame）依赖 MRT 产出的运动矢量，而 motionVectors / warp 等成员只在
    // Linux 声明，所以这一整段仍归 Linux 分支；geometry() 的闭合 } 已经提到上面，
    // #ifdef 只圈住 warpFrame，别再让它跨进 geometry() 体内（非 Linux 会连带编译不过）。
#ifdef Q_OS_LINUX
    /** 重投影中间帧探针：用主渲染 MRT 产出的运动矢量把当前场景颜色前推到 t 相位，
     *  零额外几何、相位可任意取，用来评估自研 4 倍插帧的成本与画面质量。 */
    /** 重投影中间帧：用主渲染 MRT 产出的运动矢量把当前场景颜色回看到 t 相位，
     *  零额外几何。相位走多份烘焙变体而不是 uniform——本机 Intel ANV 驱动在
     *  同一管线同时绑定两个 std140 uniform block 时会在建管线阶段 SIGFPE。 */
    void warpFrame(QRhiCommandBuffer *cb, QRhiTexture *source) {
        if (!qEnvironmentVariableIsSet("RHINE_WARP_PROBE") || !source || !motionVectors ||
            size.isEmpty()) {
            return;
        }
        // 目标与绑定都按「尺寸 / 输入纹理」变化重建：场景目标在 resize 时会被换掉，
        // 绑定里残留的旧指针会直接崩，所以显式比对三个输入。
        const bool sizeChanged = !warp.texture || warp.texture->pixelSize() != size;
        if (sizeChanged) {
            warp.texture = create(own(rhi->newTexture(QRhiTexture::RGBA16F, size, 1,
                                                      QRhiTexture::RenderTarget)));
            warp.rt = own(rhi->newTextureRenderTarget({QRhiColorAttachment(warp.texture)}));
            auto descriptor = own(warp.rt->newCompatibleRenderPassDescriptor());
            warp.rt->setRenderPassDescriptor(descriptor);
            create(warp.rt);
            warp.rp = warp.rt->renderPassDescriptor();
            qWarning("重投影探针: 目标 %dx%d", size.width(), size.height());
        }
        if (sizeChanged || warpSource != source || warpVectors != motionVectors) {
            const auto stages = QRhiShaderResourceBinding::VertexStage |
                                QRhiShaderResourceBinding::FragmentStage;
            warpBinding = bindings(
                {QRhiShaderResourceBinding::uniformBuffer(0, stages, globalBuffer),
                 QRhiShaderResourceBinding::sampledTexture(
                     2, QRhiShaderResourceBinding::FragmentStage, source, linear),
                 QRhiShaderResourceBinding::sampledTexture(
                     3, QRhiShaderResourceBinding::FragmentStage, motionVectors, linear)});
            warpSource = source;
            warpVectors = motionVectors;
        }
        const auto phase = qEnvironmentVariableIsSet("RHINE_WARP_T")
                               ? QString::fromLocal8Bit(qgetenv("RHINE_WARP_T"))
                               : QStringLiteral("0.5");
        const QString kind = phase == "0.25"   ? QStringLiteral("warpEarly")
                             : phase == "0.75" ? QStringLiteral("warpLate")
                             : phase == "debug" ? QStringLiteral("warpDebug")
                                                : QStringLiteral("warp");
        auto *pass = pipeline(kind, warpBinding, warp.rp);
        cb->beginPass(warp.rt, Qt::black, {1, 0});
        cb->setGraphicsPipeline(pass);
        cb->setShaderResources(warpBinding);
        cb->setViewport({0, 0, float(size.width()), float(size.height())});
        cb->draw(3);
        ++drawCalls;
        cb->endPass();
    }
#endif
    void composite(QRhiCommandBuffer *cb, QRhiRenderTarget *destination) {
#ifndef Q_OS_DARWIN
        auto finalDestination = destination;
        if (postProcess->active()) {
            destination = postProcess->inputTarget();
        }
#endif
#ifdef Q_OS_DARWIN
        QRhiTexture *sceneColor = color.texture;
        implementation = QStringLiteral("off");
        if (settings.temporal()) {
            if (!metalFx) {
                metalFx = std::make_unique<RhineRender::MetalFxTemporal>(rhi);
            }
            const auto offset = temporalFrame["jitter"].toArray();
            const QPointF jitter =
                offset.size() == 2 ? QPointF(offset[0].toDouble(), offset[1].toDouble()) : QPointF();
            sceneColor = metalFx->resolve(cb, color.texture, motion ? motion->depth() : nullptr,
                                          motion ? motion->vectors() : nullptr, viewSize, jitter,
                                          temporalFrame["temporalReset"].toBool());
            implementation = metalFx->active() ? QStringLiteral("metalfx-temporal")
                                               : QStringLiteral("metalfx-unavailable");
        } else if (metalFx) {
            clearPostBindings();
            metalFx.reset();
        }
        if (postColor != sceneColor) {
            clearPostBindings();
            postColor = sceneColor;
        }
#endif
#ifdef Q_OS_LINUX
#ifdef Q_OS_DARWIN
        auto &binding = postBindings["final/" + QString::number(quintptr(sceneColor))];
#else
        // RHINE_WARP_SHOW=1 时最终合成改读重投影结果，用于肉眼判断拉丝。
        const bool showWarp = qEnvironmentVariableIsSet("RHINE_WARP_SHOW") && warp.texture &&
                              qEnvironmentVariableIsSet("RHINE_WARP_PROBE");
        auto &binding = postBindings[showWarp
                                         ? "final/warp/" + QString::number(quintptr(warp.texture))
                                         : QStringLiteral("final")];
#endif
#else
#ifdef Q_OS_DARWIN
        auto &binding = postBindings["final/" + QString::number(quintptr(sceneColor))];
#else
        auto &binding = postBindings["final"];
#endif
#endif
        if (!binding) {
            const auto stages =
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
            binding = bindings(
                {QRhiShaderResourceBinding::uniformBuffer(0, stages, globalBuffer),
                 QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
#ifdef Q_OS_LINUX
#ifdef Q_OS_DARWIN
                                                           sceneColor,
#else
                                                           showWarp ? warp.texture : color.texture,
#endif
#else
#ifdef Q_OS_DARWIN
                                                           sceneColor,
#else
                                                           color.texture,
#endif
#endif
                                                           linear)});
        }
#ifdef Q_OS_LINUX
#ifndef Q_OS_DARWIN
        // 探针：最终合成前先产出一张重投影中间帧（默认关闭，RHINE_WARP_PROBE=1 才走）。
        warpFrame(cb, color.texture);
#endif
#endif
        cb->beginPass(destination, Qt::black, {1, 0});
        cb->setGraphicsPipeline(pipeline("finalFragment", binding, destination->renderPassDescriptor()));
        cb->setShaderResources(binding);
        const auto dimensions = destination->pixelSize();
        cb->setViewport({0, 0, float(dimensions.width()), float(dimensions.height())});
        cb->draw(3);
        ++drawCalls;
        cb->endPass();
#ifndef Q_OS_DARWIN
        if (postProcess->active()) {
            postProcess->resolve(cb, finalDestination, motionInputs);
        }
#endif
#ifdef Q_OS_LINUX
    }
    /** 主渲染已经输出运动矢量，这里只把本帧输入交给 FG 宿主（不再有第二趟几何）。 */
    void publishMotionInputs(const QJsonObject &frame) {
        RhineFg::FrameInputs inputs;
        inputs.depth = depth;
        inputs.motionVectors = motionVectors;
        inputs.renderSize = size;
        inputs.displaySize = viewSize;
        const auto jitter = frame["jitter"].toArray();
        inputs.jitter =
            jitter.size() == 2 ? QPointF(jitter[0].toDouble(), jitter[1].toDouble()) : QPointF();
        inputs.reset = frame["temporalReset"].toBool();
        inputs.frameTimeMs = frameClock.isValid() ? float(frameClock.nsecsElapsed()) / 1e6f : 16.6667f;
        frameClock.start();
        const auto projection = matrix(frame["projection"].toArray());
        const float c = projection(2, 2);
        const float translation = projection(2, 3);
        if (std::abs(c - 1.f) > 1e-6f && std::abs(c + 1.f) > 1e-6f) {
            inputs.nearPlane = translation / (c - 1.f);
            inputs.farPlane = translation / (c + 1.f);
        }
        inputs.fovYRadians = 2.f * std::atan(1.f / projection(1, 1));
        RhineFg::publish(inputs);
#endif
    }
    /** 同一份几何生成深度和运动矢量，供时域处理与呈现层使用。 */
    void velocityPass(QRhiCommandBuffer *cb, const QList<Draw> &draws, const QJsonObject &frame) {
#ifdef Q_OS_DARWIN
        const bool required =
            settings.temporal() || (settings.interpolation && RhineFg::capability().interpolationAvailable);
#else
        auto frameInputs = &motionInputs;
        const bool required = postProcess->needsMotion();
#endif
        if (!required) {
            motion.reset();
            if (frameInputs) {
                const auto serial = frameInputs->serial;
                *frameInputs = {};
                frameInputs->serial = serial;
            }
            return;
        }
        if (!motion) {
            motion = std::make_unique<RhineRender::MotionPass>(rhi);
        }
        QList<RhineRender::MotionDraw> inputs;
        for (const auto &draw : draws) {
            const auto &data = draw.data;
            if (data["shadowOnly"].toBool() ||
                (data["transparent"].toBool() && n(data, "transmission") <= .000001)) {
                continue;
            }
            inputs.append({data["id"].toInt(), draw.vertices, draw.indices, matrixBytes(data), draw.start,
                           draw.count, data["count"].toInt(), data["side"].toInt() == 2});
        }
        motion->render(cb, size, frame, inputs, frameInputs, viewSize);
#ifndef Q_OS_DARWIN
        RhineFg::publish(motionInputs);
#endif
    }
    /**
     * 仅提交一遍几何和一次最终合成；表面与邻壳柔光只读取构建时烘焙的共享贴图。
     * 帧数据和原生矩阵只读；临时绑定与实例缓冲区必须活到本帧命令提交结束。
     */
    void render(QRhiCommandBuffer *cb, QJsonObject frame, QSize dimensions, QJsonObject item, QImage cover,
                QRhiRenderTarget *finalTarget = nullptr, const Engine::MusicTextures &musicTextures = {}) {
        transient.clear();
#ifdef Q_OS_DARWIN
        temporalFrame = frame;
#endif
        if (qEnvironmentVariableIsSet("RHINE_DEBUG_DRAWS")) {
            qWarning("scene render mode=%s size=%dx%d", qPrintable(frame["mode"].toString()),
                     dimensions.width(), dimensions.height());
        }
        if (!updates) {
            updates = rhi->nextResourceUpdateBatch();
        }
        resize(dimensions, !finalTarget);
#ifdef Q_OS_LINUX
        QElapsedTimer dbgPre;
        dbgPre.start();
#endif
#ifndef Q_OS_DARWIN
        postProcess->prepare(size, viewSize, frame);
#endif
#ifdef Q_OS_LINUX
        const double dbgPostMs = double(dbgPre.nsecsElapsed()) / 1e6;
        dbgPre.restart();
#endif
        auto g = globals(frame);
#ifdef Q_OS_LINUX
        // 速度用未抖动 VP（光栅位置仍带抖动），口径与 motion.vert 一致。
        const auto unjitteredVp = matrix(frame["view"].toArray()) * matrix(frame["projection"].toArray());
        g.currentViewProjection = PackedMatrix(unjitteredVp);
        g.previousViewProjection =
            motionHistoryValid ? PackedMatrix(previousViewProjection) : PackedMatrix(unjitteredVp);
#endif
        updates->updateDynamicBuffer(globalBuffer, 0, sizeof(g), &g);
        ++generation;
        // 每帧重置 arena：实例数据按帧重建，偏移从 0 开始。
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
#ifdef Q_OS_LINUX
        const double dbgTexMs = double(dbgPre.nsecsElapsed()) / 1e6;
        dbgPre.restart();
#endif
        // 开场抽出的就是详情主体，跨模式时保持同一套玻璃、主光和实例状态。
        const auto mode = frame["mode"].toString();
        detailMode = mode == "detail" || mode == "boot" || frame["viewer"].toBool();
        auto draws = musicDraws(frame);
        // 保留可见与归位唱片资源，复用材质绑定和实例缓冲。
        auto data = prepare(draws, mode == "detail" || mode == "boot");
#ifdef Q_OS_LINUX
        const double dbgListMs = double(dbgPre.nsecsElapsed()) / 1e6;
        // 排障计时：把"构建 draw 列表"与"命令录制"分开量，只统计不改行为。
        static QElapsedTimer dbgClock;
        static double dbgBuild = 0, dbgRecord = 0;
        static double dbgPostSum = 0, dbgTexSum = 0, dbgListSum = 0;
        static int dbgFrames = 0;
        dbgClock.start();
#endif
        if (qEnvironmentVariableIsSet("RHINE_DEBUG_DRAWS")) {
            for (const auto &draw : data) {
                std::fprintf(
                    stderr, "draw #%02d %-26s mesh=%d type=%.0f side=%.0f opacity=%.2f transparent=%d\n",
                    draw.data["debugIndex"].toInt(), qPrintable(draw.data["name"].toString()),
                    draw.data["mesh"].toInt(), surface(draw.data).flags.x(), draw.data["side"].toDouble(),
                    draw.data["opacity"].toDouble(), draw.data["transparent"].toBool() ? 1 : 0);
            }
            std::fflush(stderr);
        }
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
        QList<Draw> opaque;
        QList<Draw> transparent;
        for (const auto &draw : data) {
            if (draw.data["shadowOnly"].toBool()) {
                continue;
            }
            if (draw.data["transparent"].toBool()) {
                transparent.append(draw);
            } else {
                opaque.append(draw);
            }
        }
        const auto sort = [](auto &draws) {
#ifdef Q_OS_LINUX
            if (draws.size() < 2) {
                return;
            }
            // order 预取：原实现把 QJsonObject 查找放进比较器，每次比较做两次哈希查找，
            // n log n 次比较就是几千次/帧。这里每个 draw 只查一次，再按索引稳定排序。
            QList<int> keys;
            keys.reserve(draws.size());
            for (const auto &draw : draws) {
                keys.append(draw.data["order"].toInt());
            }
            QList<int> order;
            order.resize(draws.size());
            for (int i = 0; i < order.size(); ++i) {
                order[i] = i;
            }
            std::stable_sort(order.begin(), order.end(), [&keys](int first, int second) {
                return keys[first] < keys[second];
#else
            std::stable_sort(draws.begin(), draws.end(), [](const auto &first, const auto &second) {
                return first.data["order"].toInt() < second.data["order"].toInt();
#endif
            });
#ifdef Q_OS_LINUX
            QList<Draw> sorted;
            sorted.reserve(draws.size());
            for (int index : order) {
                sorted.append(draws[index]);
            }
            draws = sorted;
#endif
        };
        sort(opaque);
        sort(transparent);
        if (frame["batchDraws"].toBool(true)) {
            opaque = batchDraws(opaque);
        }
        for (auto it = batchInstances.begin(); it != batchInstances.end();) {
            if (it->used != generation) {
                retire(it->buffer);
                it = batchInstances.erase(it);
            } else {
                ++it;
            }
        }
#ifdef Q_OS_LINUX
        const QRhiBuffer *arenaBuffer = nullptr;
#endif
        cb->resourceUpdate(updates);
        updates = nullptr;
        const auto background = vector(frame["background"]);
        cb->beginPass(color.rt, QColor::fromRgbF(background.x(), background.y(), background.z()), {1, 0});
#ifdef Q_OS_LINUX
        const double dbgBuildMs = double(dbgClock.nsecsElapsed()) / 1e6;
        QElapsedTimer dbgRecordClock;
        dbgRecordClock.start();
#endif
        geometry(cb, opaque);
        geometry(cb, transparent);
#ifdef Q_OS_LINUX
        dbgPostSum += dbgPostMs;
        dbgTexSum += dbgTexMs;
        dbgListSum += dbgListMs;
        dbgBuild += dbgBuildMs;
        dbgRecord += double(dbgRecordClock.nsecsElapsed()) / 1e6;
        if (++dbgFrames >= 20 && qEnvironmentVariableIsSet("RHINE_FG_DIAG")) {
            qWarning("3D内部: 后处理 %.1f  纹理 %.1f  draw列表 %.1f  构建 %.1f  录制 %.1f ms (%d 帧均值)",
                     dbgPostSum / dbgFrames, dbgTexSum / dbgFrames, dbgListSum / dbgFrames,
                     dbgBuild / dbgFrames, dbgRecord / dbgFrames, dbgFrames);
            dbgBuild = dbgRecord = 0;
            dbgPostSum = dbgTexSum = dbgListSum = 0;
            dbgFrames = 0;
        }
#endif
        cb->endPass();
#ifdef Q_OS_LINUX
        // 运动矢量已随主渲染输出（MRT），这里只发布输入并保存上一帧状态。
        publishMotionInputs(frame);
        previousViewProjection = unjitteredVp;
        previousInstanceMatrices.clear();
        for (const auto &draw : data) {
            previousInstanceMatrices.insert(draw.data["id"].toInt(), matrixBytes(draw.data));
        }
        motionHistoryValid = true;
#else
        velocityPass(cb, data, frame);
#endif
        composite(cb, finalTarget ? finalTarget : target.rt);
    }
};
class ItemRenderer : public QQuickRhiItemRenderer {
    Engine *backend = nullptr;
#ifdef Q_OS_DARWIN
    QRhi *capabilityDevice = nullptr;
    Engine *capabilityEngine = nullptr;
#endif
    std::unique_ptr<Renderer> renderer;
    QJsonObject frame, item;
    SceneDrawBuffers buffers;
    Engine::MusicTextures musicTextures;
    QImage cover;
    QString root;

  public:
    ~ItemRenderer() override {
        if (backend) {
            backend->recordResourceRelease();
        }
    }
    void synchronize(QQuickRhiItem *value) override {
        auto scene = static_cast<RhiScene *>(value);
        auto engine = scene->engine();
        backend = engine;
        if (engine) {
#ifdef Q_OS_DARWIN
            if (rhi() && (capabilityDevice != rhi() || capabilityEngine != engine)) {
                const bool temporalAvailable = RhineRender::MetalFxTemporal::supported(rhi());
                engine->renderConfiguration.setNativeTemporalAvailable(temporalAvailable);
                RhineFg::capabilityMutable().temporalAvailable = temporalAvailable;
                capabilityDevice = rhi();
                capabilityEngine = engine;
            }
#endif
            frame = engine->frame();
            frame["opticalLightEnabled"] = scene->lightingEnabled();
            frame["geometryRevision"] = double(engine->geometryRevision());
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
        if (qEnvironmentVariableIsSet("RHINE_DEBUG_DRAWS")) {
            std::fprintf(stderr, "scene item render empty=%d mode=%s time=%.2f\n", frame.isEmpty() ? 1 : 0,
                         qPrintable(frame["mode"].toString()), frame["time"].toDouble());
            std::fflush(stderr);
        }
        // 系统可能先请求绘制再生成首帧，相机未就绪时只清空目标，禁止读取空矩阵。
        const bool cameraReady = frame.value("projection").toArray().size() == 16 &&
                                 frame.value("aoProjection").toArray().size() == 16 &&
                                 frame.value("view").toArray().size() == 16;
        if (!cameraReady || (frame.value("mode") == "boot" && frame.value("time").toDouble() < 21.9)) {
            cb->beginPass(renderTarget(), Qt::transparent, {1, 0});
            cb->endPass();
            return;
        }
        if (!renderer) {
            renderer = std::make_unique<Renderer>(root, rhi());
        }
#ifdef Q_OS_DARWIN
        renderer->settings = backend->renderConfiguration.snapshot();
        renderer->frameInputs = &backend->metalFxInputs;
#endif
        renderer->nativeBuffers = buffers;
        const auto previousBindings = renderer->bindingCreations;
        const auto previousUploads = renderer->coverUploads;
        const auto previousDrawCalls = renderer->drawCalls;
#ifndef Q_OS_DARWIN
        const auto previousPostBindings = renderer->postProcess->bindingCreations;
        const auto previousPostDraws = renderer->postProcess->drawCalls;
#endif
        renderer->render(cb, frame, renderTarget()->pixelSize(), item, cover, renderTarget(), musicTextures);
#ifdef Q_OS_DARWIN
        backend->renderTelemetry.publish(renderer->size, renderer->viewSize, 1, renderer->implementation,
                                         true);
#endif
        if (backend) {
            quint64 resources =
                renderer->resources.size() + renderer->sized.size() + renderer->transient.size();
            quint64 bindings = renderer->bindingCreations - previousBindings;
            quint64 draws = renderer->drawCalls - previousDrawCalls;
            if (renderer->motion) {
                resources += renderer->motion->resources.size();
            }
#ifndef Q_OS_DARWIN
            resources += renderer->postProcess->resources.size();
            bindings += renderer->postProcess->bindingCreations - previousPostBindings;
            draws += renderer->postProcess->drawCalls - previousPostDraws;
#endif
            // GPU 耗时和"这个耗时可不可信"必须一起上报：只报数值时，0 既可能是设备
            // 不支持、也可能是创建 QRhi 时没请求时间戳、还可能是这一帧查询结果没回来，
            // 三种情况的排查方向完全不同（A01）。
            const bool gpuTimingSupported = rhi()->isFeatureSupported(QRhi::Timestamps);
            const double gpuTimeMs =
                gpuTimingSupported ? cb->lastCompletedGpuTime() * 1000.0 : 0.0;
            backend->recordRender(resources, bindings, renderer->musicTextureBytes(),
                                  renderer->coverUploads - previousUploads, draws, gpuTimeMs,
                                  RhineRender::classifyGpuTiming(gpuTimingSupported,
                                                                 RhineRender::timestampsRequested(),
                                                                 gpuTimeMs > 0));
            if (gpuTimeMs > 0 && qEnvironmentVariableIsSet("RHINE_DEBUG_GPU")) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    std::fprintf(stderr, "GPU frame time: %.3f ms (performance %.3f ms)\n", gpuTimeMs,
                                 backend->performance().value("gpuTimeMs").toDouble());
                    std::fflush(stderr);
                }
            }
        }
    }
};
} // namespace RhineRendering

RhiScene::RhiScene(QQuickItem *parent) : QQuickRhiItem(parent) {
    setAlphaBlending(true);
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
    return new RhineRendering::ItemRenderer;
}
void RhiScene::setLightingEnabled(bool enabled) {
    if (m_lightingEnabled == enabled) {
        return;
    }
    m_lightingEnabled = enabled;
    emit lightingEnabledChanged();
    update();
}

/**
 * 使用同一份场景数据生成参考图，供离线渲染比较使用。
 */
QImage renderReference(const QString &root, const QJsonObject &frame, const QJsonObject &item,
                       const QImage &cover, QSize size, const SceneDrawBuffers &buffers, quint64 *drawCalls,
                       const RhineRender::Settings *settings) {
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
#ifdef Q_OS_DARWIN
    if (!rhi) {
        QRhiMetalInitParams params;
        rhi.reset(QRhi::create(QRhi::Metal, &params));
    }
#elif defined(Q_OS_WIN) && RHINE_ENABLE_D3D12_BACKEND
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
    RhineRendering::Renderer renderer(root, rhi.get());
    renderer.nativeBuffers = buffers;
#ifdef Q_OS_DARWIN
    if (settings) {
        renderer.settings = *settings;
    }
#else
    Q_UNUSED(settings);
#endif
    auto renderFrame = frame;
    renderFrame["opticalLightEnabled"] = true;
    renderer.render(cb, renderFrame, size, item, cover);
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
