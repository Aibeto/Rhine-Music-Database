#pragma once

#include "../fg/FgFrameTap.h"
#include "SceneResources.h"
#include <QJsonObject>
#include <QElapsedTimer>
#include <QMatrix4x4>

namespace RhineRender {
struct MotionDraw {
    int id = 0;
    QRhiBuffer *vertices = nullptr;
    QRhiBuffer *indices = nullptr;
    QByteArray matrices;
    int first = 0;
    int verticesCount = 0;
    int instancesCount = 0;
    bool doubleSided = false;
};

/** 共用深度和速度输出；历史按实例稳定位置匹配，并包含上一帧相机变换。 */
class MotionPass : public SceneResources {
  public:
    explicit MotionPass(QRhi *rhi) : SceneResources(rhi) {
    }
    void render(QRhiCommandBuffer *commands, QSize dimensions, const QJsonObject &frame,
                const QList<MotionDraw> &draws, RhineFg::FrameInputs *output, QSize display);
    QRhiTexture *depth() const {
        return m_depth;
    }
    QRhiTexture *vectors() const {
        return m_vectors;
    }

  private:
    void resize(QSize dimensions);
    QRhiGraphicsPipeline *pipeline(bool doubleSided);
    QSize m_size;
    QRhiTexture *m_depth = nullptr;
    QRhiTexture *m_vectors = nullptr;
    QRhiTextureRenderTarget *m_target = nullptr;
    QRhiBuffer *m_globals = nullptr;
    QRhiShaderResourceBindings *m_bindings = nullptr;
    QRhiGraphicsPipeline *m_pipelines[2] = {};
    QHash<int, BufferState> m_instances;
    QHash<int, QByteArray> m_previousModels;
    QMatrix4x4 m_previousViewProjection;
    bool m_historyValid = false;
    QElapsedTimer m_clock;
};

// 像素抖动只修改 GPU 投影，不影响拾取和场景状态。
inline void applyJitter(QMatrix4x4 &projection, QPointF offset, QSize size) {
    if (!offset.isNull() && !size.isEmpty()) {
        projection(0, 2) -= float(2 * offset.x() / size.width());
        projection(1, 2) += float(2 * offset.y() / size.height());
    }
}
} // namespace RhineRender
