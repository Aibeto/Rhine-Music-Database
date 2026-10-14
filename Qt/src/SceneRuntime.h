#pragma once
#include <QHash>
#include <QJSEngine>
#include <QJsonObject>
#include <QObject>
// 两个平台使用同一 Qt JS 引擎和场景脚本，保持动画与实例数据一致。
class SceneRuntime : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready)
  public:
    SceneRuntime(QString root, QJSEngine *qml);
    ~SceneRuntime() override;
    bool ready() const;
    struct DrawData {
        QByteArray matrices, themes;
    };
    using DrawBuffers = QHash<int, DrawData>;
    DrawBuffers drawBuffers;
    struct FrameTimings {
        double scriptMs = 0;
        double jsonMs = 0;
        double buffersMs = 0;
        double summaryMs = 0;
    };
    FrameTimings timings;
    void profileFrames(bool value) {
        m_profileFrames = value;
    }
    // 传输回归只重编码同一个已模拟帧，不能用同时间再次推进原有弹簧。
    void captureOnly(bool value) {
        m_captureOnly = value;
    }
    bool changed() const {
        return m_changed;
    }
    void diagnostics(bool value) {
        if (m_diagnostics != value) {
            m_changed = true;
        }
        m_diagnostics = value;
    }
    void energyIdle(bool value) {
        call("energyIdle", {value});
    }
    Q_INVOKABLE bool init() {
        return ready();
    }
    Q_INVOKABLE QJSValue loadGeometry(QString asset);
    Q_INVOKABLE QJSValue frameFast(double time);
    Q_INVOKABLE QJSValue buffer(int) {
        return m_qml->newArray();
    }
    Q_INVOKABLE void setMode(QString value);
    Q_INVOKABLE void navigate(QString axis, int direction);
    Q_INVOKABLE void select(int index, QJSValue navigation = QJSValue());
    Q_INVOKABLE void musicClear();
    Q_INVOKABLE void musicIdentity(QString key);
    Q_INVOKABLE QJSValue pick(double x, double y);
    Q_INVOKABLE int selection();
    Q_INVOKABLE void pointer(QString type, double x, double y, double delta = 0);
    Q_INVOKABLE void resize(double w, double h);
    Q_INVOKABLE void theme(bool dark);
    Q_INVOKABLE void reduced(bool value);
    Q_INVOKABLE void openViewer();
    Q_INVOKABLE void coverFront();
    Q_INVOKABLE void coverBack();
    Q_INVOKABLE void viewerAction(QString action);
    Q_INVOKABLE void viewerKey(QString key);
    Q_INVOKABLE bool review(QString mode);
    Q_INVOKABLE void freeze(QJSValue time);
    void trimMemory();
    QJsonObject frame;
    QString failure;

  private:
    bool m_diagnostics = false;
    bool m_profileFrames = false;
    bool m_captureOnly = false;
    bool m_changed = true;
    QJSEngine context;
    QString m_root;
    QHash<int, QJsonObject> m_drawDefinitions;
    QJSValue runtime;
    QJSEngine *m_qml;
    QJSValue evaluate(QString code);
    QJSValue call(QString method, QVariantList values = {});
    QString json(QJSValue value);
};
