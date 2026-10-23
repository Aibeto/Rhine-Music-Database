#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>

// 更新网络与音乐账号完全隔离；只检查官方发布，更新操作交给浏览器。
class AppUpdater : public QObject {
    Q_OBJECT
  public:
    explicit AppUpdater(bool offline, QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    ~AppUpdater() override;
    void check();
    void cancel();
    void openRelease();
    bool busy() const;
    QString status() const;
    QString latestVersion() const;
    bool available() const;
    static QString currentVersion();
    static QString commitTitle();
    static QString platformAsset();
    static bool isNewer(const QString &candidate, const QString &current);
    static QJsonObject selectRelease(const QJsonObject &release, const QString &platform,
                                     const QString &current, QString *error);

  signals:
    void changed();
    void updateAvailable();

  private:
    bool m_offline;
    QNetworkAccessManager m_defaultNetwork;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QJsonObject m_release;
    QString m_status = "尚未检查更新";
    qint64 m_retryAtUnix = 0;
    void fail(const QString &message);
    void fetchRelease(bool useApi);
    QString responseError(QNetworkReply *reply, int code, const QByteArray &body);
    QString rateLimitStatus() const;
};
