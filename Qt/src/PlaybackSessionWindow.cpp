#include "PlaybackSessionWindow.h"
#include "AppStorage.h"
#include "Engine.h"
#include "EngineHolder.h"
#include "PlaybackConnection.h"
#include "render/PresentationShell.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonObject>
#include <QProcess>
#include <QScreen>
#include <QScopedValueRollback>
#include <utility>
#include <QThread>
#include <QTimer>
#include <QWindow>
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#include "InterfaceWindowMac.h"
#endif

/** 连接同一用户的唯一服务；独立启动的服务不随界面析构或标准输入关闭而退出。 */
bool connectPlaybackService(PlaybackConnection &connection, const QString &root, QString &error) {
    const auto endpoint = PlaybackConnection::endpoint();
    if (!connection.connectToService(endpoint, 100)) {
        const auto executable = PlaybackConnection::serviceExecutable();
        if (!QFileInfo::exists(executable) ||
            !QProcess::startDetached(
                executable, {"--resources", root, "--gui", QCoreApplication::applicationFilePath()})) {
            error = QStringLiteral("无法启动后台播放服务，请重新构建或安装完整应用");
            return false;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        while (!connection.connectToService(endpoint, 100)) {
            if (elapsed.elapsed() >= 10000) {
                error = QStringLiteral("后台播放服务启动超时：") + connection.error();
                return false;
            }
            QThread::msleep(50);
        }
    }
    const auto hello = connection.call("hello");
    if (hello["protocol"] != PlaybackConnection::ProtocolVersion ||
        hello["version"].toString() != QCoreApplication::applicationVersion()) {
        error = QStringLiteral("后台播放服务版本不一致，请从托盘退出旧版后重新启动");
        return false;
    }
    return true;
}

PlaybackSessionWindow::PlaybackSessionWindow(PlaybackConnection *connection, QWindow *window,
                                             PresentationShell *shell, EngineHolder *holder, QObject *parent)
    : QObject(parent), m_transition([this] { applyTransition(); }, this), m_connection(connection),
      m_window(window), m_shell(shell), m_holder(holder) {
    auto settings = AppStorage::settings();
    const auto savedGeometry = settings.value("interface/geometry").toRect();
    if (savedGeometry.isValid()) {
        for (auto screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersects(savedGeometry)) {
                m_window->setGeometry(savedGeometry);
                break;
            }
        }
    }
    m_normalGeometry = m_window->geometry();
    m_savedStates = Qt::WindowStates(settings.value("interface/states", 0).toInt()) &
                    (Qt::WindowMaximized | Qt::WindowFullScreen);
    if (m_savedStates) {
        m_window->setWindowStates(m_savedStates);
    }
    m_window->installEventFilter(this);
    QCoreApplication::instance()->installEventFilter(this);

    const auto notifyScreen = [this] { PlaybackSessionWindow::notifyScreen(); };
    notifyScreen();
    connect(m_window, &QWindow::screenChanged, this, notifyScreen);
    connect(m_window, &QWindow::windowStateChanged, this, [this, notifyScreen](Qt::WindowState state) {
        if (m_changingWindowState) {
            return;
        }
        notifyScreen();
        if (state == Qt::WindowMinimized) {
            requestSuspension();
        }
    });
    connect(m_window, &QWindow::visibilityChanged, this, [this](QWindow::Visibility visibility) {
        if (m_changingWindowState) {
            return;
        }
        if (visibility == QWindow::Hidden || visibility == QWindow::Minimized) {
            requestSuspension();
        } else {
            requestRestoration();
            m_savedStates = m_window->windowStates() & (Qt::WindowMaximized | Qt::WindowFullScreen);
        }
    });
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (m_changingWindowState) {
            return;
        }
        if (state == Qt::ApplicationHidden) {
            requestSuspension();
        } else if (state == Qt::ApplicationActive) {
            if (m_window->isVisible() && m_window->visibility() != QWindow::Minimized) {
                requestRestoration();
            }
        }
    });

    connect(m_connection, &PlaybackConnection::restoreRequested, this, [this] { requestRestoration(); });
    connect(m_connection, &PlaybackConnection::suspendRequested, this, [this] { requestSuspension(true); });
    connect(m_connection, &PlaybackConnection::activateRequested, this, [this] { requestRestoration(true); });
    connect(m_connection, &PlaybackConnection::closeWindowRequested, m_window, &QWindow::close,
            Qt::QueuedConnection);
    connect(
        m_connection, &PlaybackConnection::quitRequested, this, [] { QCoreApplication::quit(); },
        Qt::QueuedConnection);

    // 引擎重建时立即把新的业务对象交给呈现层，避免静态页切换与引擎生命周期交叉。
    connect(m_holder, &EngineHolder::started, this, [this](Engine *engine) { m_shell->setBackend(engine); });

    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
            [this] { saveInterfaceState(); });

    auto diagnostics = new QTimer(this);
    diagnostics->setInterval(200);
    connect(diagnostics, &QTimer::timeout, this, &PlaybackSessionWindow::publishRenderState);
    diagnostics->start();
    QTimer::singleShot(300, this, &PlaybackSessionWindow::publishRenderState);
}

PlaybackSessionWindow::~PlaybackSessionWindow() = default;

bool PlaybackSessionWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == QCoreApplication::instance() && event->type() == QEvent::Quit) {
        m_explicitExit = true;
        m_transition.cancel();
    }
    if (watched == m_window && (event->type() == QEvent::Move || event->type() == QEvent::Resize) &&
        !m_changingWindowState && m_window->visibility() == QWindow::Windowed) {
        m_normalGeometry = m_window->geometry();
    }
    if (watched == m_window && event->type() == QEvent::Expose && m_restoreWhenExposed &&
        m_window->isExposed() && m_window->visibility() != QWindow::Minimized) {
        requestRestoration();
    }
    if (watched == m_window && event->type() == QEvent::Close && !m_explicitExit) {
        m_window->hide();
        requestSuspension();
        event->ignore();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

/** 原生事件与服务通知只提交意图；手动收起不能被同批被动恢复覆盖。 */
void PlaybackSessionWindow::requestSuspension(bool automatic) {
    if (m_explicitExit || (automatic && m_intent == Intent::Activate)) {
        return;
    }
    if (!automatic || m_intent != Intent::SuspendManually) {
        m_intent = automatic ? Intent::SuspendAutomatically : Intent::SuspendManually;
    }
    m_transition.request();
}

void PlaybackSessionWindow::requestRestoration(bool activate) {
    if (m_explicitExit || (!activate && m_intent == Intent::SuspendManually)) {
        return;
    }
    if (activate || m_intent != Intent::Activate) {
        m_intent = activate ? Intent::Activate : Intent::Restore;
    }
    m_transition.request();
}

/** 所有入口在同一迁移边界收敛，原生消息处理期间不卸载 QML 或图形设备。 */
void PlaybackSessionWindow::applyTransition() {
    if (m_explicitExit) {
        return;
    }
    const auto intent = std::exchange(m_intent, Intent::None);
    if (intent == Intent::SuspendManually || intent == Intent::SuspendAutomatically) {
        suspendRendering(intent == Intent::SuspendAutomatically);
    } else if (intent == Intent::Restore || intent == Intent::Activate) {
        restoreRendering(intent == Intent::Activate);
    }
}

/** 挂起为静态页：先切换呈现层，再销毁业务引擎；窗口与原生句柄保持不变。 */
void PlaybackSessionWindow::suspendRendering(bool automatic) {
    // 关闭与最小化会同时触发窗口状态和可见性回调，只上报一次挂起意图。
    if (!m_suspensionReported || m_automaticSuspension != automatic) {
        m_suspensionReported = true;
        m_automaticSuspension = automatic;
        m_connection->call("interface.release", {{"automatic", automatic}});
    }
    if (m_suspended) {
        return;
    }
    m_restoreWhenExposed = false;
    m_suspended = true;
    const auto release = [this] {
        if (!m_shell->suspendToStatic()) {
            m_suspended = false;
            qWarning() << "挂起失败：" << m_shell->lastError();
            return;
        }
        m_shell->setBackend(nullptr);
        m_holder->stop();
    };
#ifdef Q_OS_MACOS
    drainInterfaceResources(release);
#else
    release();
#endif

    if (!m_suspended) {
        m_suspensionReported = false;
        m_connection->call("interface.resumed");
        return;
    }

    // 原生静态页已经销毁完整设备，不再隐藏、重显或改窗口标志来催促回收。
    publishRenderState();
}

/** 在同一窗口重建业务引擎与真实界面；服务端音频与队列不受影响。 */
void PlaybackSessionWindow::restoreRendering(bool activate) {
    // 主动打开只在这里额外显示并激活窗口；后续暴露检查、引擎与输入恢复完全共用。
    if (activate) {
        QScopedValueRollback changingState(m_changingWindowState, true);
        const auto states = m_window->windowStates() & ~Qt::WindowMinimized;
        m_window->setWindowStates(states);
        m_window->setVisible(true);
        m_window->setWindowStates(states);
        m_window->raise();
        m_window->requestActivate();
    }
    if (!m_window->isVisible() || !m_window->isExposed() || m_window->visibility() == QWindow::Minimized) {
        m_restoreWhenExposed = true;
        return;
    }
    m_restoreWhenExposed = false;
    if (!m_suspended) {
        return;
    }
    m_suspended = false;
    m_suspensionReported = false;
    if (!m_holder->start()) {
        m_suspended = true;
        return;
    }
    auto engine = m_holder->engine();
    engine->resize(m_window->width(), m_window->height());
    engine->setVisible(true);
    engine->start();
    engine->step(engine->animationTime());
    if (!m_shell->resumeFromStatic()) {
        m_suspended = true;
        m_shell->setBackend(nullptr);
        m_holder->stop();
        qWarning() << "恢复失败：" << m_shell->lastError();
        return;
    }
    m_connection->call("interface.resumed");
    publishRenderState();
}

void PlaybackSessionWindow::notifyScreen() {
    if (!m_window->screen()) {
        return;
    }
    const auto screen = m_window->screen();
    const auto geometry = screen->geometry();
    m_connection->call("interface.screen", {{"screen", screen->name()},
                                            {"x", geometry.x()},
                                            {"y", geometry.y()},
                                            {"width", geometry.width()},
                                            {"height", geometry.height()}});
    m_connection->call("interface.fullscreen",
                       {{"value", m_window->windowStates().testFlag(Qt::WindowFullScreen)}});
}

void PlaybackSessionWindow::saveInterfaceState() {
    auto settings = AppStorage::settings();
    settings.setValue("interface/geometry", m_normalGeometry);
    settings.setValue("interface/states", int(m_savedStates));
    settings.sync();
}

/**
 * 读取窗口、呈现层与实际引擎诊断供离线验收；
 * 挂起时引擎可能已销毁，这些计数字段按零上报，不把请求状态当成回收证据。
 */
void PlaybackSessionWindow::publishRenderState() {
    const auto engine = m_holder->engine();
    QJsonObject report;
    if (engine) {
        report = QJsonObject::fromVariantMap(engine->performance());
    } else {
        report = {{"steps", 0}, {"renderFrames", 0}, {"timerActive", false}, {"gpuResources", 0}};
    }
    const auto shellState = m_shell->renderState();
    report["suspended"] = m_suspended;
    report["liveContent"] = !m_suspended;
    report["shell"] = true;
    report["graphicsDevice"] = shellState.value("graphicsDevice").toBool();
    report["nativeStaticImage"] = shellState.value("nativeStaticImage").toBool();
#ifdef Q_OS_MACOS
    task_vm_info_data_t memory{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&memory), &count) ==
        KERN_SUCCESS) {
        report["physicalFootprintBytes"] = double(memory.phys_footprint);
    }
#endif
    report["windowId"] = QString::number(quintptr(m_window->winId()));
    report["windowFrames"] = QJsonValue::fromVariant(shellState.value("presented"));
    report["presented"] = QJsonValue::fromVariant(shellState.value("presented"));
    report["sceneFrames"] = QJsonValue::fromVariant(shellState.value("sceneFrames"));
    m_connection->call("interface.renderState", report);
}
