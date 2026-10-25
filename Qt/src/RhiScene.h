#pragma once
#include "SceneTypes.h"
#include <QImage>
#include <QJsonObject>
#include <QQuickRhiItem>
#include <QTimer>
class Engine;
class RhiScene : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(Engine *engine READ engine WRITE setEngine NOTIFY engineChanged)
  public:
    explicit RhiScene(QQuickItem *parent = nullptr);
    Engine *engine() const {
        return m_engine;
    }
    void setEngine(Engine *value);
  signals:
    void engineChanged();

  protected:
    QQuickRhiItemRenderer *createRenderer() override;

  private:
    Engine *m_engine = nullptr;
    // 时间性抗锯齿（FSR3/TAA）靠帧间抖动累积历史，渲染循环一休眠历史就不再推进，
    // 因此这些模式下需要一个常驻定时器把重绘请求续上；其它模式保持按需渲染。
    QTimer *m_temporalTimer = nullptr;
    // 上一次看到的 AA 模式。用于在模式切换的瞬间补一次重绘（让 resize() 跑到、
    // 新的渲染目标被创建），而不是让静态场景在 FSR1 下也常驻 60fps。
    QString m_temporalMode;
};
QImage renderReference(const QString &root, const QJsonObject &frame, const QJsonObject &item,
                       const QImage &cover, QSize size, const SceneDrawBuffers &buffers = {},
                       quint64 *drawCalls = nullptr);
