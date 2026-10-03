#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <functional>

// Uses the existing bat-remote/v2 contract. Session ownership stays in Rust.
class HostClient : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(const QJsonValue &, const QString &)>;
    explicit HostClient(QObject *parent = nullptr);
    void connectTo(const QUrl &connectionUrl);
    void disconnectFromHost();
    bool ready() const { return m_ready; }
    QString contextId() const { return m_contextId; }
    void setContextId(const QString &id) { m_contextId = id; }
    void invoke(const QString &channel, const QJsonObject &params, QObject *owner,
                Reply reply, int timeoutMs = 30000, bool scoped = true);
    static QByteArray normalizedFingerprint(QString fingerprint);
    static QString validateConnectionUrl(const QUrl &url);
    static bool certificateMatches(const QByteArray &certificateDer, const QByteArray &fingerprint);

signals:
    void connected();
    void disconnected();
    void connectionError(const QString &message);
    void eventReceived(const QString &channel, const QJsonObject &params);

private:
    struct Pending { QPointer<QObject> owner; Reply reply; QTimer *timer; };
    void receive(const QString &text);
    void finish(const QString &id, const QJsonValue &value, const QString &error);
    void failPending(const QString &error);
    QWebSocket m_socket;
    QTimer m_keepalive;
    QTimer m_authDeadline;
    QHash<QString, Pending> m_pending;
    QByteArray m_fingerprint;
    QString m_token;
    QString m_contextId;
    quint64 m_sequence = 0;
    bool m_ready = false;
};
