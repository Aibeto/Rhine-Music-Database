#pragma once
#include <QFile>
#include <QIODevice>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>
#include <functional>
#include <memory>

// HTTP 音频流，支持 Range 续传和断点恢复
class HttpAudioStream : public QIODevice {
  public:
    explicit HttpAudioStream(QUrl source, QObject *parent = nullptr);
    ~HttpAudioStream() override;
    bool start();
    void cancel();
    bool isSequential() const override;
    qint64 size() const override;
    qint64 pos() const override;
    bool seek(qint64 position) override;
    bool atEnd() const override;
    qint64 bytesAvailable() const override;
    QJsonObject diagnosticState() const;
    std::function<void()> ready;
    std::function<void()> failed;
    std::function<void()> transportChanged;

  protected:
    qint64 readData(char *data, qint64 maximum) override;
    qint64 writeData(const char *, qint64) override;

  private:
    friend class AudioPlayerRegression;
    struct State;
    class Worker;
    std::unique_ptr<State> state;
    QThread networkThread;
    Worker *worker = nullptr;
    QTemporaryFile cache;
    QFile reader;
    QUrl source;
    qint64 cursor = 0;
};
