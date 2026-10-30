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
#include <QMutex>
#include <QPainter>
#include <QOffscreenSurface>
#include <QQueue>
#include <QRunnable>
#include <QThreadPool>
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
// A08：封面合成与 mip 链生成搬到工作线程后的三个上限。
//  · COVER_PREP_MAX_INFLIGHT：同时在算的封面数。切歌很快时若无上限，会瞬间堆出十几份
//    5 MiB 的中间图；超过上限就让这一帧先不算，下一帧再发（不降级成同步）。
//  · COVER_UPLOAD_QUEUE_LIMIT：已经算完、等上传预算的成品条数上限，超出丢最旧的。
//  · COVER_LATENCY_SAMPLES：首次可见延迟只保留最近 N 次，探针报均值/最大值。
static constexpr int COVER_PREP_MAX_INFLIGHT = 4;
static constexpr int COVER_UPLOAD_QUEUE_LIMIT = 16;
static constexpr int COVER_LATENCY_SAMPLES = 8;
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
/**
 * 一张封面的预处理成品：整条 mip 链的像素首尾相接。
 * 交付契约是"要么整条链都在，要么判失败"——分层上传必须一次成型，缺层会被后端判成
 * 纹理不完整并采样为黑，所以渲染线程只认 valid()，不做"先传基层、后面补层"。
 */
struct CoverPrepareResult {
    qint64 revision = 0;      // QImage::cacheKey()，与 coverTextures 同一键空间
    quint64 serial = 0;       // 请求代号：过期判定看它，不看 revision 还在不在
    QString key;              // 发起请求的曲目，仅排障与统计
    int baseSize = 0;         // 基层边长；0 表示预处理失败
    int requestFrame = 0;     // 发起时的帧号，首次可见延迟由它算
    QVector<int> levelBytes;  // 每层字节数，顺序与 mip 层一致
    QByteArray pixels;        // 层内 RGBA8 行优先、不留行尾填充
    qint64 bytes = 0;         // == pixels.size()，上传预算按它计
    bool valid() const { return baseSize > 0 && bytes > 0 && !levelBytes.isEmpty(); }
};
/** 工作线程 → 渲染线程的唯一共享物：互斥保护的成品队列。 */
struct CoverMailbox {
    QMutex mutex;
    QQueue<CoverPrepareResult> queue;
};
/**
 * 单个封面的合成 + mip 链生成。
 * 只碰 QImage / QByteArray：不碰 QRhi、不碰 Renderer 成员、不碰 Glyphs（字形图集是
 * 渲染线程的 GPU 资源），因此可以安全地跑在任意线程上。
 */
class CoverPrepareTask : public QRunnable {
  public:
    CoverPrepareTask(std::shared_ptr<CoverMailbox> box, QImage source, int coverSize, qint64 revision,
                     quint64 serial, QString key, int requestFrame)
        : m_box(std::move(box)), m_source(std::move(source)), m_size(coverSize), m_revision(revision),
          m_serial(serial), m_key(std::move(key)), m_requestFrame(requestFrame) {}
    void run() override {
        auto result = prepare();
        // 无论成败都入队一条：失败也要让渲染线程知道该改走同步路径，不能静默丢封面。
        QMutexLocker lock(&m_box->mutex);
        m_box->queue.enqueue(std::move(result));
    }

  private:
    CoverPrepareResult prepare() {
        CoverPrepareResult result;
        result.revision = m_revision;
        result.serial = m_serial;
        result.key = m_key;
        result.requestFrame = m_requestFrame;
        if (m_source.isNull() || m_size <= 0) {
            return result;  // 无封面占位图要用 Glyphs，留给渲染线程同步画
        }
        // 合成口径必须与同步路径逐像素一致，否则异步/同步两条路会画出不同的封面。
        QImage pixels(m_size, m_size, QImage::Format_RGBA8888_Premultiplied);
        if (pixels.isNull()) {
            return result;
        }
        pixels.fill(QColor::fromRgbF(.36, .45, .45));
        {
            QPainter painter(&pixels);
            painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
            const auto dimensions = m_source.size().scaled(m_size, m_size, Qt::KeepAspectRatioByExpanding);
            painter.drawImage(QRectF((m_size - dimensions.width()) / 2.,
                                     (m_size - dimensions.height()) / 2., dimensions.width(),
                                     dimensions.height()),
                              m_source);
        }  // QPainter 必须先析构：它自己 detach，之后才能安全读像素
        // 预留 4/3 容量：整条链约 5.6 MiB，逐层追加不预留会反复重分配。
        result.pixels.reserve(qsizetype(m_size) * m_size * 4 * 4 / 3);
        // 减半规则与 uploadMipped 完全相同（IgnoreAspectRatio + SmoothTransformation），
        // 层数也由同一个循环决定，不另行按位宽推断。
        auto level = pixels;
        for (;;) {
            const int width = level.width();
            const int height = level.height();
            const int stride = int(level.bytesPerLine());
            if (width <= 0 || height <= 0) {
                break;
            }
            result.levelBytes.append(width * height * 4);
            if (stride == width * 4) {
                result.pixels.append(reinterpret_cast<const char *>(level.constBits()),
                                     qsizetype(width) * height * 4);
            } else {
                // 32 位格式下 QImage 的行跨度恒为 width*4；仍按行搬运，免得某个平台的
                // 行对齐把行尾填充当成像素传进纹理。
                for (int y = 0; y < height; ++y) {
                    result.pixels.append(
                        reinterpret_cast<const char *>(level.constBits() + qsizetype(y) * stride),
                        qsizetype(width) * 4);
                }
            }
            if (width == 1 && height == 1) {
                break;
            }
            level = level.scaled(qMax(1, width / 2), qMax(1, height / 2), Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
            if (level.isNull()) {
                break;
            }
        }
        result.baseSize = m_size;
        result.bytes = result.pixels.size();
        return result;
    }
    std::shared_ptr<CoverMailbox> m_box;
    QImage m_source;
    int m_size = 512;
    qint64 m_revision = 0;
    quint64 m_serial = 0;
    QString m_key;
    int m_requestFrame = 0;
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
    // 运动矢量 MRT 所需的上一帧状态。
    //  previousInstanceMatrices：主路径，key = drawId<<32 | 实例 id，按稳定 id 精确配对；
    //    实例列表每帧都按相机距离重排，只有 id 能表达"同一条实例"。
    //    值存定长 PackedMatrix 而不是 QByteArray：每帧要写上千条，64 字节的 QByteArray
    //    每条都是一次堆分配，而定长值直接躺在 QHash 的节点里，清空重填能复用节点。
    //  previousInstanceBlocks：旧路径（按平移列配对）要按上一帧的下标取矩阵，只有整块字节
    //    保留得住顺序，所以两张表并存；没有实例 id 的 draw（地台）也只能走这条。
    QHash<quint64, PackedMatrix> previousInstanceMatrices;
    QHash<int, QByteArray> previousInstanceBlocks;
    // 上一帧逐 draw 记录到的实例条数：与当前 ids 条数不一致就说明可见集合刚变过，
    // id 表必然缺项，这一帧整块退回平移列配对，避免大面积 zero-velocity。
    QHash<int, int> previousInstanceCounts;
    QMatrix4x4 previousViewProjection;
    QElapsedTimer frameClock;
    bool motionHistoryValid = false;
    // 配对探针（RHINE_MOTION_PROBE=1）：本帧按 id 命中 / 按平移列回退命中 / 完全没配上的实例数。
    quint64 motionByIdHits = 0, motionFallbackHits = 0, motionMisses = 0;
    int motionProbeFrames = 0;
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
    // ── A08 封面预处理：线程模型见 CoverPrepareTask；这里只有渲染线程可见的状态 ──────
    /** 发起预处理的结果。Busy 只是"这一帧不排队"，绝不能因此退回同步路径。 */
    enum class PrepStart { Started, Busy, Unavailable };
    struct CoverPrepRequest {
        quint64 serial = 0;  // 递增请求代号，过期判定的主依据
        QString key;         // 发起请求的曲目
        int frame = 0;       // 发起时的帧号
    };
    // 邮箱用 shared_ptr 持有：切场景销毁 Renderer 时在途任务还在跑，它们写的是这个
    // 邮箱而不是 Renderer 成员，最后一个引用释放前内存一直有效，不会出现悬垂写。
    std::shared_ptr<CoverMailbox> mailbox = std::make_shared<CoverMailbox>();
    QHash<qint64, CoverPrepRequest> coverPrep;  // revision → 在途请求（同一 cacheKey 只发一次）
    QHash<QString, qint64> coverWanted;         // key → 该 key 想要的 revision
    QSet<qint64> coverSyncFallback;             // 预处理失败的 revision：改走同步路径
    QQueue<CoverPrepareResult> uploadQueue;     // 已算完、等上传预算的成品
    quint64 coverSerial = 0;
    bool coverAsyncDisabled = false;  // 线程池拒绝过任务：此后一律同步
    // 单帧离屏参考渲染（renderReference）只跑一帧，没有"后续帧"可以等异步结果，
    // 由调用方置位强制同步，否则参考图里的封面会退化成占位图。
    bool forceSynchronousCover = false;
    // 每帧封面 mip 上传字节上限；0 表示不限。构造时读一次：运行期改环境变量没意义，
    // 也避免每帧 getenv。默认 8 MiB，一张 1024² 含 mip 约 5.6 MiB，正好放行一张大封面。
    qint64 uploadBudget = readUploadBudget();
    qint64 uploadedThisFrame = 0;
    int frameIndex = 0;
    quint64 prepCompleted = 0;      // 探针：已完成数
    quint64 droppedStale = 0;       // 探针：过期丢弃数
    quint64 droppedQueue = 0;       // 探针：队列溢出丢弃数
    quint64 deferredFrames = 0;     // 探针：本周期内因预算顺延过上传的帧数
    QVector<int> firstVisibleFrames;
    int probeFrames = 0;
    bool coverProbe = qEnvironmentVariableIntValue("RHINE_TEXTURE_PROBE") == 1;
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
#ifdef Q_OS_LINUX
        // 走到这里说明颜色目标真的要重建（尺寸/采样数/AA 模式变了）：上一帧的 VP 与实例矩阵
        // 都对应旧目标，继续拿它们算重投影会在新目标的第一帧里得到完全错误的方向。
        // 早退分支在上面，所以静止画面不会每帧清表。
        previousInstanceMatrices.clear();
        previousInstanceBlocks.clear();
        previousInstanceCounts.clear();
        motionHistoryValid = false;
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
    /**
     * RHINE_TEXTURE_UPLOAD_BUDGET_MB：每帧封面 mip 上传字节上限，默认 8 MiB，0 = 不限。
     * 只认整数兆字节；非数字/未设置都退回默认值，免得一个笔误把上传彻底关死。
     */
    static qint64 readUploadBudget() {
        bool ok = false;
        const int megabytes = qEnvironmentVariableIntValue("RHINE_TEXTURE_UPLOAD_BUDGET_MB", &ok);
        return ok ? qMax(0, megabytes) * qint64(1024 * 1024) : qint64(8) * 1024 * 1024;
    }
    /**
     * 把某个 key 的封面切到 coverTextures 里已经就绪的那张。
     * 旧纹理只在这个 key 是最后一个引用者时才退休：同专辑多曲、以及所有无封面曲目
     * （cacheKey 恒为 0）共享同一张纹理，提前退休会让别人的 SRB 指向已释放对象。
     */
    void bindCoverTexture(const QString &key, qint64 revision) {
        const auto previousRevision = coverRevisions.value(key);
        const auto previous = covers.value(key);
        const auto next = coverTextures.value(revision);
        covers[key] = next;
        coverRevisions[key] = revision;
        coverWanted.remove(key);
        if (previous && previous != next) {
            retireCoverRevision(key, previousRevision, previous);
        }
    }
    /** 某 key 不再使用这张封面纹理：仍有别的 key 指向它时只解绑引用，不退休对象。 */
    void retireCoverRevision(const QString &key, qint64 revision, QRhiTexture *texture) {
        if (!texture) {
            return;
        }
        for (auto it = covers.cbegin(); it != covers.cend(); ++it) {
            if (it.key() != key && it.value() == texture) {
                return;
            }
        }
        invalidateBindings(texture);
        if (coverTextures.value(revision) == texture) {
            coverTextures.remove(revision);
        }
        retire(texture);
    }
    /** 同步路径（A08 之前的原样实现）：合成、逐层生成 mip、上传、换绑，一帧内完成。 */
    void uploadCoverSync(const QString &key, const QImage &image) {
        const qint64 coverRevision = image.cacheKey();
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
        bindCoverTexture(key, coverRevision);
    }
    /**
     * 发起一次封面预处理。同一 cacheKey 由 coverPrep 去重，调用方已保证不会重入。
     * Busy 表示"在算的太多，这一帧先不发"，下一帧还会再试；Unavailable 才是真的没法
     * 异步（线程池拒绝），调用方必须退回同步路径。
     */
    PrepStart startCoverPrep(const QString &key, const QImage &image, int coverSize, qint64 revision) {
        if (coverPrep.size() >= COVER_PREP_MAX_INFLIGHT) {
            return PrepStart::Busy;
        }
        const CoverPrepRequest request{++coverSerial, key, frameIndex};
        auto task = new CoverPrepareTask(mailbox, image, coverSize, revision, request.serial, key,
                                         request.frame);
        // 用 tryStart 而不是 start：线程池正在析构时必须能看出来，否则封面会静默消失。
        if (!QThreadPool::globalInstance()->tryStart(task)) {
            // 失败时 Qt 对 runnable 的所有权在不同版本里不一致（有的版本会替调用方删），
            // 这里不 delete：最坏是关停瞬间漏一个 QRunnable，换的是绝不重复释放。
            coverAsyncDisabled = true;
            qWarning("封面预处理线程池不可用，封面回退同步路径");
            return PrepStart::Unavailable;
        }
        coverPrep.insert(revision, request);
        return PrepStart::Started;
    }
    /** 把整条 mip 链一次性传给 GPU：任何一层缺席都会让后端判纹理不完整、采样成黑。 */
    QRhiTexture *uploadCoverTexture(const CoverPrepareResult &result) {
        auto cover =
            texture(QSize(result.baseSize, result.baseSize), QRhiTexture::RGBA8, QRhiTexture::MipMapped);
        QVector<QRhiTextureUploadEntry> entries;
        entries.reserve(result.levelBytes.size());
        qsizetype offset = 0;
        for (int level = 0; level < result.levelBytes.size(); ++level) {
            const auto byteCount = result.levelBytes.at(level);
            entries.append(QRhiTextureUploadEntry(
                0, level, QRhiTextureSubresourceUploadDescription(result.pixels.mid(offset, byteCount))));
            offset += byteCount;
        }
        QRhiTextureUploadDescription description;
        description.setEntries(entries.cbegin(), entries.cend());
        updates->uploadTexture(cover, description);
        return cover;
    }
    /**
     * 按每帧字节预算上传已就绪的封面。预算 0 表示不限。
     * 队首那条即使单张就超预算也放行（前提是本帧还没传过别的），否则小预算配大封面会
     * 永久饿死：它永远轮不到，又永远排在最前面。
     */
    void uploadPendingCovers() {
        while (!uploadQueue.isEmpty()) {
            const auto &head = uploadQueue.head();
            if (uploadBudget > 0 && uploadedThisFrame > 0 &&
                uploadedThisFrame + head.bytes > uploadBudget) {
                break;  // 剩下的留给后续帧，成品留在队列里不丢
            }
            auto result = uploadQueue.dequeue();
            // 排队期间可能已经切歌/淘汰：成品代表"算完那一刻"的意图，不一定是"现在"的。
            // 没人再要就别建纹理，否则会留下一张 textureOrder 永远选不到、也就永远不淘汰的
            // 孤儿纹理（textureOrder 里根本没有这个 key）。
            if (coverTextures.contains(result.revision) ||
                !coverWanted.values().contains(result.revision)) {
                ++droppedStale;
                continue;
            }
            const auto cost = result.bytes;
            const auto cover = uploadCoverTexture(result);
            coverTextures[result.revision] = cover;
            ++coverUploads;
            // 同一个 revision 可能同时被多首同专辑曲目等着，一次全部换绑。
            for (const auto &key : coverWanted.keys()) {
                if (coverWanted.value(key) == result.revision) {
                    bindCoverTexture(key, result.revision);
                }
            }
            // 首次可见延迟：请求帧 → 真正成为该 key 封面的帧。
            firstVisibleFrames.append(frameIndex - result.requestFrame);
            while (firstVisibleFrames.size() > COVER_LATENCY_SAMPLES) {
                firstVisibleFrames.removeFirst();
            }
            uploadedThisFrame += cost;
            if (uploadBudget > 0 && uploadedThisFrame >= uploadBudget) {
                break;
            }
        }
        if (!uploadQueue.isEmpty()) {
            ++deferredFrames;
        }
    }
    /**
     * 每帧一次：工作线程回来的成品先判过期，再入队，最后按预算上传。
     * 只在这里创建 QRhi 纹理，工作线程永远不碰 GPU 对象。
     */
    void drainCoverPrep() {
        // 预算按帧重置：这是"每帧最多传多少"的上限，不是全局总量。
        uploadedThisFrame = 0;
        QQueue<CoverPrepareResult> incoming;
        {
            QMutexLocker lock(&mailbox->mutex);
            incoming.swap(mailbox->queue);
        }
        for (auto &result : incoming) {
            // 过期判定看三件事：请求代号还是不是当前那一个（切歌/淘汰会作废代号）、
            // 是否还有 key 想要这个 revision、以及是不是已经有别的路径把它做出来了。
            const auto request = coverPrep.value(result.revision);
            const bool current = request.serial != 0 && request.serial == result.serial;
            if (current) {
                // 每个在途请求恰好收一条结果，收到就注销。放在判活之前：既不注销会被
                // coverWorkPending() 当成"还在路上"而永久请求新帧，也不给重发留机会。
                coverPrep.remove(result.revision);
            }
            if (!current || coverTextures.contains(result.revision) ||
                !coverWanted.values().contains(result.revision)) {
                ++droppedStale;
                continue;  // 整块 QByteArray 随 result 析构一起释放
            }
            if (!result.valid()) {
                // 工作线程失败（内存不足 / 缩放产出空图）：记下这个 revision 改走同步，
                // 封面不能静默消失，更不能让空纹理顶上去。
                coverSyncFallback.insert(result.revision);
                // 撤掉"想要"登记，下一帧 updateMusic 才会重新进入派发、走到同步兜底。
                // 留着的话它会把这次失败当成"仍在处理中"，封面就永远停在占位图。
                for (auto it = coverWanted.begin(); it != coverWanted.end();) {
                    if (it.value() == result.revision) {
                        it = coverWanted.erase(it);
                    } else {
                        ++it;
                    }
                }
                qWarning("封面预处理失败（revision=%lld），改走同步路径",
                         static_cast<long long>(result.revision));
                continue;
            }
            ++prepCompleted;
            uploadQueue.enqueue(std::move(result));
        }
        while (uploadQueue.size() > COVER_UPLOAD_QUEUE_LIMIT) {
            uploadQueue.dequeue();
            ++droppedQueue;
        }
        uploadPendingCovers();
        reportCoverProbe();
    }
    /** 还有预处理在路上、或成品没传完：需要再要一帧，否则结果会一直躺在队列里。 */
    bool coverWorkPending() const { return !uploadQueue.isEmpty() || !coverPrep.isEmpty(); }
    /** RHINE_TEXTURE_PROBE=1 时每 120 帧一行；字段名固定，方便直接 grep/喂脚本。 */
    void reportCoverProbe() {
        if (!coverProbe || (++probeFrames % 120) != 0) {
            return;
        }
        int total = 0, worst = 0;
        for (const int value : firstVisibleFrames) {
            total += value;
            worst = qMax(worst, value);
        }
        const auto average =
            firstVisibleFrames.isEmpty() ? 0. : double(total) / firstVisibleFrames.size();
        qInfo().noquote() << "纹理探针: prepAsync"
                          << QStringLiteral("%1/%2").arg(prepCompleted).arg(coverPrep.size())
                          << "| uploaded" << uploadedThisFrame << "字节 | deferred"
                          << QStringLiteral("%1 条 / 顺延 %2 帧").arg(uploadQueue.size()).arg(deferredFrames)
                          << "| dropped" << (droppedStale + droppedQueue)
                          << QStringLiteral("(过期 %1 / 队列 %2)").arg(droppedStale).arg(droppedQueue)
                          << "| firstVisibleFrames"
                          << QStringLiteral("平均 %1 最大 %2 样本 %3")
                                 .arg(average, 0, 'f', 2)
                                 .arg(worst)
                                 .arg(firstVisibleFrames.size())
                          << "| budget"
                          << (uploadBudget > 0 ? QStringLiteral("%1 字节").arg(uploadBudget)
                                               : QStringLiteral("不限"))
                          << "| 同步兜底" << (coverAsyncDisabled ? "开" : "关");
        deferredFrames = 0;
    }
    /**
     * 作废某 key 名下的在途预处理：结果回来时判不过请求代号，直接丢弃。
     * 只在"这张图已经没人要"时调用——还被别的 key 要着的 revision 必须留着。
     */
    void discardCoverPrepFor(const QString &key) {
        for (auto it = coverPrep.begin(); it != coverPrep.end();) {
            if (it.value().key == key && !coverWanted.values().contains(it.key())) {
                it = coverPrep.erase(it);
            } else {
                ++it;
            }
        }
    }
    void releaseCover(const QString &key) {
        const auto revision = coverRevisions.take(key);
        const auto previous = covers.take(key);
        // 这个 key 不再需要自己的封面了：连带撤掉"想要"登记和在途请求。
        // 否则迟到的结果会新建一张无人引用、又永远不会被淘汰的纹理（textureOrder 里
        // 已经没有这个 key，淘汰循环永远选不到它）。
        coverWanted.remove(key);
        discardCoverPrepFor(key);
        // 同专辑曲目及无封面的曲目共享纹理，最后一个引用退出后才退休 GPU 对象。
        if (!previous) {
            return;
        }
        retireCoverRevision(key, revision, previous);
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
        // 登记的意图已经不是当前这张图了：撤销意图并作废在途请求，否则迟到的结果会把
        // 已经切走的封面又贴回来。有图才判（默认值取当前 revision，没登记时本分支不触发）；
        // 空图沿用原语义——一旦该 key 有了封面，后续空图不再改写它。
        if (!image.isNull() && coverWanted.value(key, coverRevision) != coverRevision) {
            coverWanted.remove(key);
            discardCoverPrepFor(key);
        }
        const bool coverChanging =
            !covers.contains(key) || (!image.isNull() && coverRevisions.value(key) != coverRevision);
        // 已经为这个 revision 登记过意图（在途/排队/等预算）就什么都不做。
        // 若每帧重新发起，上一张结果会被判成过期并被丢掉，封面永远上不了屏。
        if (coverChanging && coverWanted.value(key, qint64(-1)) != coverRevision) {
            // 先登记"这个 key 想要哪个 revision"：异步结果回来时靠它判断还有人要不要，
            // 也是整条链唯一一处声明意图的地方。旧封面此刻不退休——换绑时由
            // bindCoverTexture 一并处理，这样等待期间画面仍是上一张封面而不是占位灰块。
            coverWanted[key] = coverRevision;
            if (coverTextures.contains(coverRevision)) {
                // 同专辑别的曲目（或无封面共享的 revision 0）已经做好了，直接复用，不重算。
                bindCoverTexture(key, coverRevision);
            } else if (!coverPrep.contains(coverRevision)) {
                // 同一 cacheKey 只发起一次预处理：切走又切回来时请求还挂着，等它就是。
                const auto coverSize = image.isNull() ? 512 : std::clamp(image.width(), 512, 1024);
                // 三种情况必须走同步：无封面占位要用 Glyphs（共享的 GPU 字形图集，跨线程会
                // 竞态）；离屏参考渲染只跑一帧，等不到异步结果；线程池已拒绝过任务或这个
                // revision 预处理失败过。同步路径的产出与异步完全一致，只是当帧完成。
                const bool synchronous = image.isNull() || forceSynchronousCover || coverAsyncDisabled ||
                                         coverSyncFallback.contains(coverRevision);
                const auto started = synchronous ? PrepStart::Unavailable
                                                 : startCoverPrep(key, image, coverSize, coverRevision);
                if (started == PrepStart::Busy) {
                    // 在算的太多，这一帧先不让它排队：撤销登记，下一帧再试。
                    // 这是软的拥塞信号，绝不能当成"异步不可用"而退回同步路径。
                    coverWanted.remove(key);
                } else if (started == PrepStart::Unavailable) {
                    uploadCoverSync(key, image);  // 同步兜底，当帧完成
                }
                // Started：本帧什么都不做，继续显示上一张可用封面；材质侧若拿不到这个 key
                // 会自动回落 placeholderCover，绝不出现"只有基层、缺 mip"的半成品纹理。
            }
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
        const int drawId = draw["id"].toInt();
        const int count = draw["count"].toInt();
        const auto matrices = matrixBytes(draw);
        // 取引用而不是 QHash::value()：后者会把整块实例矩阵按值拷一遍，循环里每实例再查一次
        // 就是几百次整块拷贝。none 只用来顶住"本帧没有这个 draw"的空绑定。
        static const SceneDrawData none;
        const auto nativeFound = nativeBuffers.constFind(drawId);
        const SceneDrawData &native = nativeFound != nativeBuffers.constEnd() ? *nativeFound : none;
#ifdef Q_OS_LINUX
        // 探针开关只读一次环境变量；默认关闭时下面的计数分支整段是死代码（零成本）。
        static const bool motionProbe = qEnvironmentVariableIntValue("RHINE_MOTION_PROBE") == 1;
        const auto &ids = native.ids;
        // 主路径的准入条件：本帧带实例 id，且上一帧同一 draw 记录到的条数与之一致。
        // 条数不一致说明可见格子刚增删过，上一帧 id 表必然缺项；这时整块退回旧路径，
        // 比"按 id 只配上一半、另一半速度归零"更不容易看到撕裂。
        const bool byId = !ids.isEmpty() && previousInstanceCounts.value(drawId, -1) == int(ids.size());
        // 旧路径：按平移列配对。列表滚动后顺序会变，按索引直接配对会给出错误速度。
        // 保留给没有实例 id 的 draw（地台）以及上面条数对不上的那一帧。
        static const QByteArray noBlock;
        const auto blockFound = previousInstanceBlocks.constFind(drawId);
        const QByteArray &previousBytes =
            blockFound != previousInstanceBlocks.constEnd() ? *blockFound : noBlock;
        QHash<QByteArray, int> previousTranslations;
        QHash<int, int> previousMatched;
        if (!byId) {
            for (int index = 0; (index + 1) * 64 <= previousBytes.size(); ++index) {
                previousTranslations.insert(previousBytes.mid(index * 64 + 48, 16), index);
            }
            for (int index = 0; (index + 1) * 64 <= matrices.size(); ++index) {
                const int match = previousTranslations.value(matrices.mid(index * 64 + 48, 16), -1);
                if (match >= 0) {
                    previousMatched.insert(index, match);
                }
            }
        }
#endif
        const auto themes = draw["themes"].toArray();
        std::vector<Instance> values;
        values.reserve(count);
        for (int i = 0; i < count; ++i) {
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
            bool paired = false;
            if (byId && i < ids.size()) {
                // key = drawId<<32 | 实例 id：两个维度压成一把哈希，省掉每组再挂一层 QHash。
                const auto exact =
                    previousInstanceMatrices.constFind(quint64(quint32(drawId)) << 32 | quint64(ids[i]));
                if (exact != previousInstanceMatrices.constEnd()) {
                    value.previousModel = *exact;
                    paired = true;
                    if (motionProbe) {
                        ++motionByIdHits;
                    }
                }
            }
            if (!paired) {
                const int previousIndex = previousMatched.value(i, -1);
                if (previousIndex >= 0 && (previousIndex + 1) * 64 <= previousBytes.size()) {
                    memcpy(value.previousModel.elements, previousBytes.constData() + previousIndex * 64, 64);
                    paired = true;
                    if (motionProbe) {
                        ++motionFallbackHits;
                    }
                }
            }
            if (!paired) {
                // 完全没配上就退回当前矩阵（速度 0）：宁可静一帧，也不要拿别的实例的矩阵
                // 算出一片乱流方向的矢量。
                if ((i + 1) * 64 <= matrices.size()) {
                    memcpy(value.previousModel.elements, matrices.constData() + i * 64, 64);
                }
                if (motionProbe) {
                    ++motionMisses;
                }
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
        // 帧号与成品回收入口：两者都必须在 updateMusic 之前，本帧就绪的封面才能当帧换绑，
        // 否则每次切歌都要白等一帧（首次可见延迟直接翻倍）。
        ++frameIndex;
        drainCoverPrep();
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
        previousInstanceBlocks.clear();
        previousInstanceCounts.clear();
        // 没有运动矢量附件时（AA 模式让路或 FG 关闭）previousModel 根本没人读，
        // 每帧上千条记录纯属开销，直接跳过。重新分配目标一定会走 resize()，
        // 那里已经把三张表清空，所以不会出现"跳过若干帧后接着用跨目标重建的旧矩阵"。
        if (motionTarget) {
            for (const auto &draw : data) {
                const int drawId = draw.data["id"].toInt();
                // 整块字节始终按 draw 记一份：旧路径要按上一帧的下标取矩阵，只有它保得住顺序。
                const auto bytes = matrixBytes(draw.data);
                previousInstanceBlocks.insert(drawId, bytes);
                const auto native = nativeBuffers.constFind(drawId);
                if (native == nativeBuffers.constEnd() || native->ids.isEmpty()) {
                    continue;  // 没有实例 id 的 draw（地台）下一帧只能走平移列配对。
                }
                const auto &ids = native->ids;
                PackedMatrix previous;
                int recorded = 0;
                for (int index = 0; index < int(ids.size()); ++index) {
                    if ((index + 1) * 64 > bytes.size()) {
                        break;
                    }
                    memcpy(previous.elements, bytes.constData() + index * 64, sizeof(previous.elements));
                    previousInstanceMatrices.insert(quint64(quint32(drawId)) << 32 | quint64(ids[index]),
                                                    previous);
                    ++recorded;
                }
                // 条数按"真正写进去的"记：矩阵字节比 id 少时下一帧这条闸门就该判 false，
                // 而不是被 ids.size() 蒙过去、让后半截实例静默丢速度。
                previousInstanceCounts.insert(drawId, recorded);
            }
        }
        motionHistoryValid = true;
        // 配对探针：每 120 帧一行，说明这一帧到底有多少实例是按 id 配上的、
        // 退回平移列配上的、以及完全没配上（速度按 0 处理）的。
        // 静止画面下应当全部是"按 id"；滚动时允许回退计数短暂占多数。
        static const bool motionProbe = qEnvironmentVariableIntValue("RHINE_MOTION_PROBE") == 1;
        if (motionProbe && ++motionProbeFrames >= 120) {
            qInfo().noquote() << "运动矢量配对: 按 id" << qint64(motionByIdHits) << "| 平移列回退"
                              << qint64(motionFallbackHits) << "| 未配对" << qint64(motionMisses)
                              << "| MRT" << (motionTarget ? "开" : "关");
            motionProbeFrames = 0;
            motionByIdHits = motionFallbackHits = motionMisses = 0;
        }
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
        // 引擎静止时会休眠、不再出帧；封面预处理回来的成品必须有人把它取走并上传，
        // 所以只要还有活儿在路上就主动再要一帧（update() 是渲染线程的安全入口）。
        if (renderer->coverWorkPending()) {
            update();
        }
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
            // A03：本帧新建过 SRB，说明有新资源真的落地了（异步封面/模型上传完成，或
            // 渲染目标重建后重新绑定）。这类变化在引擎的帧字段里看不出来，主线程的
            // 帧签名比较永远判不到，只能由发现它的渲染线程回报一次；否则 3D 层要等
            // 兜底间隔（20 帧）才重画，表现就是"封面比歌名慢半拍"。
            // 用 QueuedConnection：Engine 住在主线程，而这里是渲染线程。
            if (bindings > 0 && backend) {
                QMetaObject::invokeMethod(backend, "requestSceneContent", Qt::QueuedConnection);
            }
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
    // 参考图只渲一帧就 readback，没有后续帧可以等异步成品；强制同步，否则截图里的封面
    // 会退化成占位图（这类离屏截图是对比用的基准，画面必须与实时路径一致）。
    renderer.forceSynchronousCover = true;
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
