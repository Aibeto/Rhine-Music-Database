#include "BridgeProcess.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>

/** 在 iOS 宿主中加载真实协议 bundle，验证嵌入式 Node、传输及会话重建；请求不访问网络。 */
bool runIOSBridgeChecks(const QString &root) {
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return false;
    }
    BridgeProcess bridge;
    bool ok = true;
    for (int session = 0; session < 2; ++session) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QByteArray output;
        bool received = false;
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        const auto started = QObject::connect(&bridge, &BridgeProcess::started, &loop, [&] {
            const QJsonObject request{{"id", "embedded-check"},
                                      {"route", "/local/lyrics"},
                                      {"params", QJsonObject{{"content", "[00:01.00]嵌入式运行时"}}}};
            bridge.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        });
        const auto ready = QObject::connect(&bridge, &BridgeProcess::readyReadStandardOutput, &loop, [&] {
            output += bridge.readAllStandardOutput();
            if (output.contains('\n')) {
                received = true;
                loop.quit();
            }
        });
        const auto finished = QObject::connect(&bridge, &BridgeProcess::finished, &loop, &QEventLoop::quit);
        timeout.start(30000);
        bridge.start(root, temporary.path(), session == 0 ? "standard" : "lite");
        loop.exec();
        QObject::disconnect(started);
        QObject::disconnect(ready);
        QObject::disconnect(finished);
        const auto response = QJsonDocument::fromJson(output.trimmed()).object();
        ok &= received && response["id"] == "embedded-check" &&
              response["body"].toObject()["code"].toInt() == 200;
        bridge.kill();
        if (!ok) {
            break;
        }
    }
    qInfo() << "iOS embedded Node, packaged Worker and session replacement:" << (ok ? "passed" : "FAILED");
    return ok;
}
