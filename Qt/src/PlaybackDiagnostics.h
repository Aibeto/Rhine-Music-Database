#pragma once
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>

/** 只接收固定事件和数值白名单；业务字符串、地址与设备身份没有进入记录的入口。 */
class PlaybackDiagnostics {
  public:
    void start();
    void stop();
    void clear();
    void record(const QString &event, const QJsonObject &values);
    QJsonObject report() const;
    int session() const {
        return m_session;
    }
    static QJsonObject numericFields(const QJsonObject &values);

  private:
    bool m_recording = false;
    QElapsedTimer m_clock;
    QJsonArray m_events;
    int m_omitted = 0;
    int m_session = 0;
};
