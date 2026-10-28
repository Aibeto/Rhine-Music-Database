#include "DeferredWindowTransition.h"
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>

namespace {
class NestedLoopProbe : public QObject {
    Q_OBJECT
  public:
    std::function<void()> inspect;
    Q_INVOKABLE void enter() {
        QEventLoop nested;
        QTimer::singleShot(0, &nested, &QEventLoop::quit);
        nested.exec();
        inspect();
    }
};
}

/** 真实 QML 信号回调与嵌套事件循环中，迁移不能同步销毁当前对象。 */
bool runWindowTransitionChecks() {
    QQmlEngine engine;
    QPointer<QObject> root;
    int transitions = 0;
    bool finishedBeforeDeletion = false;
    DeferredWindowTransition gate([&] {
        ++transitions;
        finishedBeforeDeletion = root && root->property("finished").toBool();
        delete root.data();
    });
    NestedLoopProbe probe;
    bool nestedProtected = false;
    probe.inspect = [&] { nestedProtected = root && transitions == 0; };
    engine.rootContext()->setContextProperty("transitionGate", &gate);
    engine.rootContext()->setContextProperty("nestedProbe", &probe);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQml
        QtObject {
            property bool finished: false
            signal triggered()
            onTriggered: {
                transitionGate.request();
                transitionGate.request();
                nestedProbe.enter();
                finished = true;
            }
        }
    )",
                      QUrl("qrc:/WindowTransitionFixture.qml"));
    root = component.create();
    if (!root) {
        qCritical() << component.errors();
        return false;
    }
    bool handlerProtected = false;
    QEventLoop harness;
    QTimer::singleShot(0, &harness, [&] {
        QMetaObject::invokeMethod(root, "triggered", Qt::DirectConnection);
        handlerProtected = root && root->property("finished").toBool() && transitions == 0;
        QTimer::singleShot(0, &harness, &QEventLoop::quit);
    });
    harness.exec();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const bool deferredDeletion = !root && transitions == 1 && finishedBeforeDeletion;
    // 被更新的意图取消时，旧请求不能在下一轮事件循环再执行。
    gate.request();
    gate.cancel();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const bool passed = nestedProtected && handlerProtected && deferredDeletion && transitions == 1;
    qInfo() << "QML window transition boundary:" << passed;
    return passed;
}

#include "WindowTransitionChecks.moc"
