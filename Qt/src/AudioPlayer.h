#pragma once
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QVariantMap>
#include <functional>

// 原生音频只接收已由锁定上游取得的播放链接；不保存链接或账号信息。
class AudioPlayer {
  public:
    explicit AudioPlayer(std::function<void()> changed = {});
    bool load(QString input, bool offline, bool alternative = false, QString provider = "netease");
    void control(QString action, double value = 0);
    QVariantMap state() const;

  private:
    QAudioOutput output;
    QMediaPlayer player;
    int ended = 0;
    QString error;
};
