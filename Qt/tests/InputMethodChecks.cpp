#include "render/PresentationWindow.h"

#include <QDebug>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <memory>

namespace {
bool check(bool passed, const char *message) {
    if (!passed) {
        qCritical() << "FAIL:" << message;
    }
    return passed;
}

bool commit(QObject *receiver, const QString &text) {
    QInputMethodEvent event;
    event.setCommitString(text);
    return QCoreApplication::sendEvent(receiver, &event);
}

/** 验证真实 TextInput 的预编辑、提交返回值与连续同文输入。 */
bool verifyComposition(QWindow &window, QQuickItem &field) {
    bool passed = true;
    QInputMethodEvent preedit(QStringLiteral("ceshi"), {});
    passed &= check(QCoreApplication::sendEvent(&window, &preedit), "Preedit is handled by the host");
    passed &= check(field.property("preeditText") == "ceshi" && field.property("text") == "",
                    "Preedit stays separate from committed text");
    passed &= check(commit(&window, QStringLiteral("测试")), "Host returns successful IME delivery");
    passed &= check(field.property("text") == QStringLiteral("测试") &&
                        field.property("preeditText").toString().isEmpty(),
                    "One commit inserts exactly once and clears preedit");

    // 相同中文连续提交必须保留，不能被去重器吞掉。
    passed &= check(commit(&window, QStringLiteral("测试")), "Consecutive identical commit is delivered");
    passed &= check(field.property("text") == QStringLiteral("测试测试"),
                    "Repeated commits are preserved without duplicate insertion");

    // 平台直接向 focusObject 投递时不再经过宿主转发。
    passed &=
        check(commit(window.focusObject(), QStringLiteral("字")), "Direct focus-object commit succeeds");
    passed &=
        check(field.property("text") == QStringLiteral("测试测试字"), "Direct commit inserts exactly once");
    return passed;
}

bool verifyGeometry(RhineRender::PresentationWindow &window, QQuickRenderControl &control,
                    QQuickItem &field) {
    bool passed = true;
    const auto matchesCursor = [&field] {
        const auto local = QInputMethod::queryFocusObject(Qt::ImCursorRectangle, QVariant()).toRectF();
        const auto expected = field.mapRectToScene(local);
        return !local.isEmpty() && QGuiApplication::inputMethod()->cursorRectangle() == expected;
    };
    passed &= check(matchesCursor(), "Candidate rectangle uses the focused item transform");
    field.parentItem()->setPosition(QPointF(80, 90));
    field.parentItem()->setScale(1.25);
    control.polishItems();
    passed &= check(matchesCursor(), "Candidate rectangle follows ancestor movement and scaling");
    QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImSurroundingText);
    passed &= check(QCoreApplication::sendEvent(&window, &query) && query.value(Qt::ImEnabled).toBool() &&
                        query.value(Qt::ImSurroundingText) == field.property("text"),
                    "Host preserves input-method query results");
    return passed;
}
} // namespace

/** 用无渲染设备的真实 Qt Quick 输入项覆盖跨平台宿主契约，不模拟系统输入法本身。 */
int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Item {
            width: 640
            height: 480
            Item {
                x: 20
                y: 30
                TextInput {
                    objectName: "field"
                    x: 40
                    y: 50
                    width: 240
                    height: 40
                    focus: true
                }
            }
        }
    )",
                      QUrl());
    std::unique_ptr<QObject> root(component.create());
    auto *rootItem = qobject_cast<QQuickItem *>(root.get());
    if (!check(rootItem != nullptr, "QML input fixture loads")) {
        qCritical() << component.errors();
        return 1;
    }
    RhineRender::PresentationWindow window;
    QQuickRenderControl control;
    auto overlay = std::make_unique<QQuickWindow>(&control);
    overlay->resize(640, 480);
    rootItem->setParentItem(overlay->contentItem());
    auto *field = rootItem->findChild<QQuickItem *>("field");
    field->forceActiveFocus();
    window.setInputWindow(overlay.get());
    window.resize(640, 480);
    window.show();
    window.requestActivate();
    QCoreApplication::processEvents();
    bool passed = check(QGuiApplication::focusWindow() == &window && QGuiApplication::focusObject() == field,
                        "Visible window publishes the offscreen input focus");
    passed &= verifyComposition(window, *field);
    passed &= verifyGeometry(window, control, *field);

    root.reset();
    overlay.reset();
    passed &= check(window.focusObject() == &window && QGuiApplication::focusObject() == &window,
                    "Destroying the overlay removes the platform focus reference");
    return passed ? 0 : 1;
}
