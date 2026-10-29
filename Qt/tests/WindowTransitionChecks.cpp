#include "DeferredWindowTransition.h"
#include "Engine.h"
#include "EngineHolder.h"
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

/** 销毁实际引擎再重建，验证未落盘的界面状态与查看器视角不会回到启动默认值。 */
bool runEngineResumeChecks(const QString &resources) {
    EngineHolder holder(resources, true, nullptr, nullptr);
    if (!holder.start()) {
        return false;
    }
    auto engine = holder.engine();
    engine->app->music.keyword = QStringLiteral("未落盘的搜索");
    engine->app->hud.openModal("search");
    engine->app->hud.query = QStringLiteral("恢复输入");
    engine->app->hud.tab = "history";
    engine->scene.select(7);
    engine->scene.setMode("detail");
    for (int index = 1; index <= 120; ++index) {
        engine->scene.sample(index / 30.0);
    }
    engine->scene.openViewer();
    engine->scene.viewerAction("explode");
    engine->scene.viewerKey("ArrowLeft");
    engine->scene.sample(4.5);
    const auto before = engine->scene.sample(4.5, false);
    QPointer<Engine> old = engine;
    holder.stop();
    bool passed = !old && !holder.live();
    holder.start();
    engine = holder.engine();
    const auto after = engine->scene.sample(4.5, false);
    passed = passed && engine->restoredSession() && engine->scene.selection() == 7 &&
             engine->app->music.keyword == QStringLiteral("未落盘的搜索") &&
             engine->app->hud.modal == "search" && engine->app->hud.query == QStringLiteral("恢复输入") &&
             engine->app->hud.tab == "history" && before["viewer"].toBool() &&
             after["viewer"] == before["viewer"] && after["viewerState"] == before["viewerState"] &&
             after["view"] == before["view"] && after["projection"] == before["projection"];
    // 重建后真正执行 HUD 动作，不能只验证静态序列化字段。
    engine->action("close-modal");
    passed = passed && engine->app->hud.closing;
    qInfo() << "Engine resume continuity:" << passed;
    return passed;
}
