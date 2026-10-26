#include "ScenePostProcess.h"
#include "../fsr/FsrSharpness.h"
#include <QFile>
#include <QJsonArray>
#include <cmath>
#include <cstring>

namespace RhineRender {
namespace {
bool isFsr(const QString &mode) {
    return mode == "fsr" || mode == "fsr1" || mode == "fsr3";
}
int sharpness() {
    if (qEnvironmentVariableIsEmpty("RHINE_FSR_SHARPNESS")) {
        return 50;
    }
    return qBound(0, qEnvironmentVariableIntValue("RHINE_FSR_SHARPNESS"), 100);
}
QShader shader(const QString &name) {
    QFile file(":/shaders/" + name + ".qsb");
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Missing post-process shader: %s", qPrintable(name));
    }
    return QShader::fromSerialized(file.readAll());
}
} // namespace

QSize ScenePostProcess::renderSize(QSize display) const {
    const auto mode = qEnvironmentVariable("RHINE_AA_MODE");
    const int supersample = qBound(1, qEnvironmentVariableIntValue("RHINE_SUPERSAMPLE"), 2);
    const int percent = qEnvironmentVariableIntValue("RHINE_RENDER_SCALE");
    const double scale = percent > 0 ? qBound(.5, percent / 100., 1.) : 1.;
    const double aaScale =
        mode == "ssaa" ? qBound(1., qEnvironmentVariableIntValue("RHINE_AA_SCALE") / 100., 2.) : 1.;
    double divisor = 1.;
    if (isFsr(mode)) {
        const auto quality = qEnvironmentVariable("RHINE_FSR_QUALITY");
        if (quality == "super") {
            divisor = 3.;
        } else if (quality == "performance") {
            divisor = 2.;
        } else if (quality == "balanced") {
            divisor = 1.7;
        } else if (quality == "quality") {
            divisor = 1.5;
        }
    }
    const double factor = supersample * scale * aaScale / divisor;
    return {qMax(1, qRound(display.width() * factor)), qMax(1, qRound(display.height() * factor))};
}
int ScenePostProcess::samples() const {
    if (qEnvironmentVariable("RHINE_AA_MODE") != "msaa") {
        return 1;
    }
    const int configured = qEnvironmentVariableIntValue("RHINE_MSAA");
    const int wanted = configured > 1 ? qBound(2, configured, 8) : 4;
    int best = 1;
    for (int candidate : rhi->supportedSampleCounts()) {
        if (candidate > best && candidate <= wanted) {
            best = candidate;
        }
    }
    return best;
}
bool ScenePostProcess::temporal() const {
    const auto mode = qEnvironmentVariable("RHINE_AA_MODE");
    return mode == "taa" || (isFsr(mode) && mode != "fsr1" && m_fsr && m_fsr->available());
}
bool ScenePostProcess::needsMotion() const {
    return temporal() || RhineFg::isEnabled();
}
bool ScenePostProcess::active() const {
    return m_mode == "fxaa" || m_mode == "taa" || isFsr(m_mode);
}
ScenePostProcess::Target ScenePostProcess::target(QSize size, QRhiTexture::Format format,
                                                  QRhiTexture::Flags flags) {
    auto color = texture(size, format, flags | QRhiTexture::RenderTarget);
    auto result =
        own(rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription{QRhiColorAttachment(color)}));
    result->setRenderPassDescriptor(own(result->newCompatibleRenderPassDescriptor()));
    create(result);
    return {color, result};
}

/** 在任何 pass 之前重建尺寸相关资源和 FSR 上下文；关闭模式立即释放历史。 */
void ScenePostProcess::prepare(QSize render, QSize display, const QJsonObject &frame) {
    const auto mode = qEnvironmentVariable("RHINE_AA_MODE");
    if (m_mode != mode || m_renderSize != render || m_displaySize != display) {
        m_bindings.clear();
        m_pipelines.clear();
        resources.clear();
        m_input = {};
        m_output = {};
        m_history[0] = {};
        m_history[1] = {};
        m_historyValid = false;
        m_historyIndex = 0;
        m_mode = mode;
        m_renderSize = render;
        m_displaySize = display;
        if (active()) {
            m_globals = create(own(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 544)));
            m_constants =
                create(own(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Fsr1Con))));
            m_sampler =
                create(own(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                           QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge)));
            m_input = target(render, isFsr(mode) ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8);
            if (isFsr(mode)) {
                m_output = target(display, QRhiTexture::RGBA16F, QRhiTexture::UsedWithLoadStore);
            }
            if (mode == "taa") {
                m_history[0] = target(render, QRhiTexture::RGBA16F);
                m_history[1] = target(render, QRhiTexture::RGBA16F);
            }
        }
    }
    if (frame["temporalReset"].toBool()) {
        m_historyValid = false;
    }
    const bool computeFsr = isFsr(mode) && mode != "fsr1";
    if (computeFsr) {
        if (!m_fsr) {
            m_fsr = std::make_unique<Fsr3Backend>();
        }
        const float milliseconds = m_clock.isValid() ? float(m_clock.nsecsElapsed()) / 1e6f : 16.6667f;
        m_clock.start();
        const auto projection = frame["projection"].toArray();
        if (projection.size() == 16) {
            const float c = float(projection[10].toDouble());
            const float translation = float(projection[14].toDouble());
            if (std::abs(c - 1.f) > 1e-6f && std::abs(c + 1.f) > 1e-6f) {
                m_fsr->setCamera(translation / (c - 1.f), translation / (c + 1.f), milliseconds);
            }
        }
        const auto jitter = frame["jitter"].toArray();
        // 投影偏移与 FSR 采样位置使用相反符号；无抖动时显式清零。
        const float jitterX = jitter.size() == 2 ? float(-jitter[0].toDouble()) : 0.f;
        const float jitterY = jitter.size() == 2 ? float(-jitter[1].toDouble()) : 0.f;
        m_fsr->setJitter(jitterX, jitterY);
        if (frame["temporalReset"].toBool()) {
            m_fsr->requestReset();
        }
        m_fsr->resize(rhi, render, display);
    } else if (m_fsr) {
        m_fsr.reset();
    }
}

void ScenePostProcess::quad(QRhiCommandBuffer *commands, const QString &kind, QRhiRenderTarget *destination,
                            const QList<QRhiTexture *> &textures) {
    QString bindingKey = kind;
    for (auto texture : textures) {
        bindingKey += '/' + QString::number(quintptr(texture));
    }
    auto &binding = m_bindings[bindingKey];
    if (!binding) {
        binding = own(rhi->newShaderResourceBindings());
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        QList<QRhiShaderResourceBinding> values{
            QRhiShaderResourceBinding::uniformBuffer(0, stages, m_globals)};
        if (kind.startsWith("fsr1_")) {
            values.append(QRhiShaderResourceBinding::uniformBuffer(
                1, QRhiShaderResourceBinding::FragmentStage, m_constants));
        }
        for (int index = 0; index < textures.size(); ++index) {
            values.append(QRhiShaderResourceBinding::sampledTexture(
                2 + index, QRhiShaderResourceBinding::FragmentStage, textures[index], m_sampler));
        }
        binding->setBindings(values.cbegin(), values.cend());
        create(binding);
        ++bindingCreations;
    }
    const auto pipelineKey = kind + '/' + QString::number(quintptr(destination->renderPassDescriptor()));
    auto &pipeline = m_pipelines[pipelineKey];
    if (!pipeline) {
        pipeline = own(rhi->newGraphicsPipeline());
        pipeline->setShaderStages({{QRhiShaderStage::Vertex, shader("quad.vert")},
                                   {QRhiShaderStage::Fragment, shader(kind + ".frag")}});
        pipeline->setTargetBlends({QRhiGraphicsPipeline::TargetBlend{}});
        pipeline->setShaderResourceBindings(binding);
        pipeline->setRenderPassDescriptor(destination->renderPassDescriptor());
        create(pipeline);
    }
    commands->beginPass(destination, Qt::black, {1, 0});
    commands->setGraphicsPipeline(pipeline);
    commands->setShaderResources(binding);
    const auto size = destination->pixelSize();
    commands->setViewport({0, 0, float(size.width()), float(size.height())});
    commands->draw(3);
    ++drawCalls;
    commands->endPass();
}
void ScenePostProcess::fsrQuad(QRhiCommandBuffer *commands, const QString &kind,
                               QRhiRenderTarget *destination, QRhiTexture *source, const Fsr1Con &constants) {
    auto batch = rhi->nextResourceUpdateBatch();
    batch->updateDynamicBuffer(m_constants, 0, sizeof(constants), &constants);
    commands->resourceUpdate(batch);
    quad(commands, kind, destination, {source});
}

/** 对新场景的最终颜色执行互斥 AA/FSR；计算派发失败时仍输出有效场景。 */
void ScenePostProcess::resolve(QRhiCommandBuffer *commands, QRhiRenderTarget *destination,
                               const RhineFg::FrameInputs &motion) {
    QByteArray globals(544, '\0');
    const auto correction = rhi->clipSpaceCorrMatrix();
    std::memcpy(globals.data() + 464, correction.constData(), 64);
    const float backend = rhi->isYUpInFramebuffer() ? 1.f : 0.f;
    std::memcpy(globals.data() + 528, &backend, 4);
    auto batch = rhi->nextResourceUpdateBatch();
    batch->updateDynamicBuffer(m_globals, 0, globals.size(), globals.constData());
    commands->resourceUpdate(batch);
    if (m_mode == "fxaa") {
        quad(commands, "aaFragment", destination, {m_input.texture});
    } else if (m_mode == "taa") {
        if (!m_historyValid) {
            quad(commands, "copyColor", m_history[m_historyIndex].target, {m_input.texture});
        } else {
            const int next = 1 - m_historyIndex;
            quad(commands, "taa", m_history[next].target,
                 {m_input.texture, m_history[m_historyIndex].texture, motion.motionVectors});
            m_historyIndex = next;
        }
        quad(commands, "copyColor", destination, {m_history[m_historyIndex].texture});
        m_historyValid = true;
    } else if (isFsr(m_mode)) {
        const bool compute = m_fsr && m_fsr->available();
        bool resolved = false;
        if (compute) {
            commands->beginExternal();
            resolved = m_fsr->dispatch(rhi, commands, m_input.texture, motion.depth, motion.motionVectors,
                                       m_output.texture);
            commands->endExternal();
        }
        qputenv("RHINE_FSR_ENGINE", resolved ? "fsr3" : "shader");
        if (resolved) {
            quad(commands, "copyColor", destination, {m_output.texture});
        } else {
            fsrQuad(commands, "fsr1_easu", m_output.target, m_input.texture,
                    Fsr1Constants::easu(m_renderSize, m_displaySize));
            const int effective =
                RhineFsr::effectiveSharpnessPercent(sharpness(), m_renderSize, m_displaySize);
            fsrQuad(commands, "fsr1_rcas", destination, m_output.texture,
                    Fsr1Constants::rcas(m_displaySize, float(100 - effective) / 100.f * 2.f));
        }
    }
}
} // namespace RhineRender
