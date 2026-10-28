#pragma once
#include "MusicPlayer.h"
#include "InterfaceSession.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QPointer>

/** 音频与协议的唯一所有者；此对象不创建窗口、QML 引擎、场景或图形设备。 */
class PlaybackService : public QObject {
    Q_OBJECT
  public:
    PlaybackService(QString root, QString guiExecutable, bool offline, QObject *parent = nullptr);
    ~PlaybackService() override;
    bool listen(const QString &endpoint);
    void showInterface();
    void shutdown();

  private:
    struct Client {
        QByteArray buffer;
        QJsonObject previous;
    };
    Host m_host;
    MusicApi m_api;
    MusicPlayer m_player;
    QLocalServer m_server;
    QHash<QLocalSocket *, Client> m_clients;
    QPointer<QLocalSocket> m_gui;
    InterfaceSession m_interfaceSession;
    QTimer m_poll;
    QTimer m_save;
    QElapsedTimer m_clock;
    int m_selectionSerial = 0;
    QJsonObject m_selectedTrack;
    bool m_stopping = false;
    void read(QLocalSocket *socket);
    void dispatch(QLocalSocket *socket, const QJsonObject &request);
    QJsonObject hostCommand(const QString &method, const QJsonObject &arguments, bool &known);
    bool playerCommand(const QString &method, const QJsonObject &arguments);
    bool lyricsCommand(const QString &method, const QJsonObject &arguments);
    void send(QLocalSocket *socket, const QJsonObject &message);
    void publish();
    void restoreSession();
    void saveSession();
};
