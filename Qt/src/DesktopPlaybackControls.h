#pragma once
#include "Host.h"
#include "MusicApi.h"
#include <QImage>
#include <QPointer>

/** 常驻桌面播放控制，只依赖服务快照与音乐 API，不依赖可销毁的界面引擎。 */
class DesktopPlaybackControls final : public QObject {
    Q_OBJECT
  public:
    DesktopPlaybackControls(QString root, PlaybackConnection *connection, QObject *parent = nullptr);
    QJsonObject state() const;
    QImage cover() const;
    void trigger(const QString &action);

  signals:
    void changed();

  private:
    void refresh();
    void refreshLike(quint64 generation);
    void toggleLike();
    MusicApi::Valid valid(quint64 generation);
    QPointer<PlaybackConnection> m_connection;
    Host m_host;
    MusicApi m_api;
    QJsonObject m_snapshot;
    QByteArray m_scope;
    QImage m_cover;
    QString m_coverUrl;
    quint64 m_generation = 0;
    quint64 m_coverGeneration = 0;
    bool m_connected = true;
    bool m_likeKnown = false;
    bool m_liked = false;
    bool m_writing = false;
};
