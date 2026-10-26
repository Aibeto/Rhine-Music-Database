#include "render/ScenePostProcess.h"
#include <QDebug>
#include <QJsonArray>
#include <QMatrix4x4>
#include <QScopeGuard>
#include <qfloat16.h>
#include <cstring>

#ifdef Q_OS_MACOS
/** 在真实 Metal 设备上验证可移植后处理的模式切换、颜色、历史与资源复用。 */
bool runScenePostProcessChecks() {
    const auto previousMode = qgetenv("RHINE_AA_MODE");
    const auto previousQuality = qgetenv("RHINE_FSR_QUALITY");
    const auto previousScale = qgetenv("RHINE_RENDER_SCALE");
    const auto previousSample = qgetenv("RHINE_SUPERSAMPLE");
    const auto previousEngine = qgetenv("RHINE_FSR_ENGINE");
    const auto restore = [](const char *name, const QByteArray &value) {
        if (value.isNull()) {
            qunsetenv(name);
        } else {
            qputenv(name, value);
        }
    };
    const auto restoreEnvironment = qScopeGuard([&] {
        restore("RHINE_AA_MODE", previousMode);
        restore("RHINE_FSR_QUALITY", previousQuality);
        restore("RHINE_RENDER_SCALE", previousScale);
        restore("RHINE_SUPERSAMPLE", previousSample);
        restore("RHINE_FSR_ENGINE", previousEngine);
    });
    qputenv("RHINE_RENDER_SCALE", "100");
    qputenv("RHINE_SUPERSAMPLE", "1");
    qputenv("RHINE_FSR_QUALITY", "quality");
    QRhiMetalInitParams parameters;
    std::unique_ptr<QRhi> device(QRhi::create(QRhi::Metal, &parameters));
    if (!device) {
        return false;
    }
    bool passed = true;
    {
        RhineRender::SceneResources resources(device.get());
        const QSize display(192, 144);
        auto color = resources.texture(display, QRhiTexture::RGBA8,
                                       QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
        auto target = resources.own(
            device->newTextureRenderTarget(QRhiTextureRenderTargetDescription{QRhiColorAttachment(color)}));
        target->setRenderPassDescriptor(resources.own(target->newCompatibleRenderPassDescriptor()));
        resources.create(target);
        RhineRender::ScenePostProcess post(device.get());
        const QColor expected = QColor::fromRgbF(.25, .5, .75);
        QMatrix4x4 projection;
        projection.perspective(34, 4.f / 3.f, 5, 300);
        QJsonArray values;
        for (int index = 0; index < 16; ++index) {
            values.append(projection.constData()[index]);
        }
        const QJsonObject frame{{"projection", values}};
        for (const auto &mode : {"fxaa", "taa", "fsr1", "fsr", "off"}) {
            qputenv("RHINE_AA_MODE", mode);
            const auto size = post.renderSize(display);
            auto vectors = resources.texture(size, QRhiTexture::RGBA16F);
            QRhiCommandBuffer *commands = nullptr;
            quint64 steadyResources = 0;
            for (int iteration = 0; iteration < 4; ++iteration) {
                if (device->beginOffscreenFrame(&commands) != QRhi::FrameOpSuccess) {
                    passed = false;
                    break;
                }
                post.prepare(size, display, frame);
                if (!post.active()) {
                    passed &= post.resources.empty() && post.inputTarget() == nullptr;
                    device->endOffscreenFrame();
                    continue;
                }
                if (iteration == 0) {
                    auto upload = device->nextResourceUpdateBatch();
                    const QByteArray zeros(size.width() * size.height() * 8, '\0');
                    upload->uploadTexture(vectors,
                                          QRhiTextureUploadDescription({QRhiTextureUploadEntry(
                                              0, 0, QRhiTextureSubresourceUploadDescription(zeros))}));
                    commands->resourceUpdate(upload);
                }
                commands->beginPass(post.inputTarget(), expected, {1, 0});
                commands->endPass();
                RhineFg::FrameInputs motion;
                motion.motionVectors = vectors;
                post.resolve(commands, target, motion);
                QRhiReadbackResult readback;
                auto batch = device->nextResourceUpdateBatch();
                batch->readBackTexture(QRhiReadbackDescription(color), &readback);
                commands->resourceUpdate(batch);
                device->endOffscreenFrame();
                device->finish();
                if (readback.data.size() != display.width() * display.height() * 4) {
                    passed = false;
                    continue;
                }
                const QImage image(reinterpret_cast<const uchar *>(readback.data.constData()),
                                   display.width(), display.height(), QImage::Format_RGBA8888);
                const auto actual = image.pixelColor(display.width() / 2, display.height() / 2);
                const bool correct = qAbs(actual.red() - expected.red()) <= 1 &&
                                     qAbs(actual.green() - expected.green()) <= 1 &&
                                     qAbs(actual.blue() - expected.blue()) <= 1;
                passed &= correct;
                if (!correct) {
                    qCritical() << "Post-process color regression" << mode << actual << expected;
                }
                if (iteration == 2) {
                    steadyResources = post.resources.size();
                } else if (iteration == 3) {
                    passed &= post.resources.size() == steadyResources;
                }
            }
        }
        // 纯色无法暴露错误采样坐标；四象限图案可检出 EASU 常量被后续 RCAS 写入覆盖。
        qputenv("RHINE_AA_MODE", "fsr1");
        const QSize render = post.renderSize(display);
        QRhiCommandBuffer *commands = nullptr;
        if (device->beginOffscreenFrame(&commands) != QRhi::FrameOpSuccess) {
            return false;
        }
        post.prepare(render, display, frame);
        const QColor quadrants[] = {QColor::fromRgbF(.75, .25, .25), QColor::fromRgbF(.25, .75, .25),
                                    QColor::fromRgbF(.25, .25, .75), QColor::fromRgbF(.75, .75, .25)};
        QByteArray pixels(render.width() * render.height() * 8, '\0');
        for (int y = 0; y < render.height(); ++y) {
            for (int x = 0; x < render.width(); ++x) {
                const auto &pixel =
                    quadrants[(y >= render.height() / 2 ? 2 : 0) + (x >= render.width() / 2 ? 1 : 0)];
                const qfloat16 channels[] = {qfloat16(float(pixel.redF())), qfloat16(float(pixel.greenF())),
                                             qfloat16(float(pixel.blueF())), qfloat16(1.f)};
                std::memcpy(pixels.data() + (y * render.width() + x) * 8, channels, 8);
            }
        }
        auto input = static_cast<QRhiTextureRenderTarget *>(post.inputTarget())
                         ->description()
                         .colorAttachmentAt(0)
                         ->texture();
        auto upload = device->nextResourceUpdateBatch();
        upload->uploadTexture(input, QRhiTextureUploadDescription({QRhiTextureUploadEntry(
                                         0, 0, QRhiTextureSubresourceUploadDescription(pixels))}));
        commands->resourceUpdate(upload);
        post.resolve(commands, target, {});
        QRhiReadbackResult pattern;
        auto batch = device->nextResourceUpdateBatch();
        batch->readBackTexture(QRhiReadbackDescription(color), &pattern);
        commands->resourceUpdate(batch);
        device->endOffscreenFrame();
        device->finish();
        if (pattern.data.size() != display.width() * display.height() * 4) {
            passed = false;
        } else {
            const QImage image(reinterpret_cast<const uchar *>(pattern.data.constData()), display.width(),
                               display.height(), QImage::Format_RGBA8888);
            for (int index = 0; index < 4; ++index) {
                const auto actual = image.pixelColor(display.width() * (index % 2 ? 3 : 1) / 4,
                                                     display.height() * (index / 2 ? 3 : 1) / 4);
                const auto expected = quadrants[index];
                const bool correct = qAbs(actual.red() - expected.red()) <= 2 &&
                                     qAbs(actual.green() - expected.green()) <= 2 &&
                                     qAbs(actual.blue() - expected.blue()) <= 2;
                passed &= correct;
                if (!correct) {
                    qCritical() << "FSR1 quadrant sampling regression" << index << actual << expected;
                }
            }
        }
    }
    qInfo() << "Portable scene post-process checks:" << (passed ? "passed" : "FAILED");
    return passed;
}
#endif
