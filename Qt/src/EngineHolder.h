#pragma once

#include <QObject>
#include <QString>
#include <memory>

class Engine;
class FrameProfiler;
class PlaybackConnection;
class QQmlEngine;
class QQmlContext;

/**
 * 业务引擎与性能诊断器的生命周期所有者。
 * 性能模式挂起时整体销毁（只保留窗口与静态页），恢复时在同一窗口重建；
 * 播放状态由后台服务持有，界面重建不会中断音频或队列。
 */
class EngineHolder : public QObject {
    Q_OBJECT
  public:
    EngineHolder(QString root, bool offline, QQmlContext *context, PlaybackConnection *connection,
                 QString profilePath = {}, QObject *parent = nullptr);
    ~EngineHolder() override;

    /// 建立引擎并发布到 QML 上下文；已存在时直接返回。
    bool start();
    /// 断开 QML 上下文并销毁引擎；随后延迟向系统归还未用内存页。
    void stop();
    Engine *engine() const {
        return m_engine.get();
    }
    FrameProfiler *profiler() const {
        return m_profiler.get();
    }
    bool live() const {
        return m_engine != nullptr;
    }

  signals:
    /// 引擎就绪，调用方可重新绑定渲染层。
    void started(Engine *engine);
    /// 引擎即将销毁，调用方必须先断开所有引用。
    void stopping(Engine *engine);

  private:
    bool startInternal();
    QString m_root;
    bool m_offline;
    QQmlContext *m_context;
    PlaybackConnection *m_connection;
    QString m_profilePath;
    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<FrameProfiler> m_profiler;
};
