#pragma once

#include "MotionPass.h"
#include "../fsr/Fsr3Backend.h"
#include "../fsr/Fsr1Constants.h"

namespace RhineRender {

/** 唯一场景的跨平台 AA/FSR；只管理后处理，不持有另一套场景或材质。 */
class ScenePostProcess : public SceneResources {
  public:
    explicit ScenePostProcess(QRhi *device) : SceneResources(device) {
    }
    QSize renderSize(QSize display) const;
    int samples() const;
    bool temporal() const;
    bool needsMotion() const;
    bool active() const;
    void prepare(QSize render, QSize display, const QJsonObject &frame);
    QRhiRenderTarget *inputTarget() const {
        return m_input.target;
    }
    void resolve(QRhiCommandBuffer *commands, QRhiRenderTarget *destination,
                 const RhineFg::FrameInputs &motion);
    quint64 bindingCreations = 0;
    quint64 drawCalls = 0;

  private:
    struct Target {
        QRhiTexture *texture = nullptr;
        QRhiTextureRenderTarget *target = nullptr;
    };
    Target target(QSize size, QRhiTexture::Format format, QRhiTexture::Flags flags = {});
    void quad(QRhiCommandBuffer *commands, const QString &kind, QRhiRenderTarget *destination,
              const QList<QRhiTexture *> &textures);
    void fsrQuad(QRhiCommandBuffer *commands, const QString &kind, QRhiRenderTarget *destination,
                 QRhiTexture *source, const Fsr1Con &constants);
    QString m_mode;
    QSize m_renderSize;
    QSize m_displaySize;
    Target m_input;
    Target m_output;
    Target m_history[2];
    int m_historyIndex = 0;
    bool m_historyValid = false;
    QRhiBuffer *m_globals = nullptr;
    QRhiBuffer *m_constants = nullptr;
    QRhiBuffer *m_rcasConstants = nullptr;
    QRhiSampler *m_sampler = nullptr;
    QHash<QString, QRhiShaderResourceBindings *> m_bindings;
    QHash<QString, QRhiGraphicsPipeline *> m_pipelines;
    std::unique_ptr<Fsr3Backend> m_fsr;
    QElapsedTimer m_clock;
};
} // namespace RhineRender
