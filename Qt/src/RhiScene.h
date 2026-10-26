#pragma once

#include "SceneTypes.h"
#include "render/RenderConfiguration.h"
#include <QImage>
#include <QJsonObject>
#include <QQuickRhiItem>

class Engine;

/** 唯一的场景渲染入口：烘焙材质、唱片、时域输入及最终合成。 */
class RhiScene : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(Engine *engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(
        bool lightingEnabled READ lightingEnabled WRITE setLightingEnabled NOTIFY lightingEnabledChanged)
  public:
    explicit RhiScene(QQuickItem *parent = nullptr);
    Engine *engine() const {
        return m_engine;
    }
    void setEngine(Engine *engine);
    bool lightingEnabled() const {
        return m_lightingEnabled;
    }
    void setLightingEnabled(bool enabled);

  signals:
    void engineChanged();
    void lightingEnabledChanged();

  protected:
    QQuickRhiItemRenderer *createRenderer() override;

  private:
    Engine *m_engine = nullptr;
    bool m_lightingEnabled = true;
};

QImage renderReference(const QString &root, const QJsonObject &frame, const QJsonObject &item,
                       const QImage &cover, QSize size, const SceneDrawBuffers &buffers = {},
                       quint64 *drawCalls = nullptr, const RhineRender::Settings *settings = nullptr);
