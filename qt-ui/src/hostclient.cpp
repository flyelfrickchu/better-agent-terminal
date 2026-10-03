#include "hostclient.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QUrlQuery>

HostClient::HostClient(QObject *parent) : QObject(parent) {
    m_keepalive.setInterval(20000);
    connect(&m_keepalive, &QTimer::timeout, this, [this] { m_socket.ping(); });
    m_authDeadline.setSingleShot(true);
    connect(&m_authDeadline, &QTimer::timeout, this, [this] {
        emit connectionError(tr("The host did not finish authentication."));
        m_socket.abort();
    });
    connect(&m_socket, &QWebSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
        const auto certificate = m_socket.sslConfiguration().peerCertificate();
        if (!certificateMatches(certificate.toDer(), m_fingerprint)) {
            emit connectionError(tr("The host certificate does not match its fingerprint."));
            m_socket.abort();
            return;
        }
        // Pin verification replaces CA/hostname trust for BAT's self-signed cert.
        // Expired certificates and all other TLS failures remain errors.
        QList<QSslError> allowed;
        for (const auto &error : errors) {
            if (error.error() == QSslError::SelfSignedCertificate ||
                error.error() == QSslError::SelfSignedCertificateInChain ||
                error.error() == QSslError::HostNameMismatch)
                allowed.append(error);
        }
        m_socket.ignoreSslErrors(allowed);
    });
    connect(&m_socket, &QWebSocket::connected, this, [this] {
        if (!certificateMatches(m_socket.sslConfiguration().peerCertificate().toDer(), m_fingerprint)) {
            emit connectionError(tr("The host certificate does not match its fingerprint."));
            m_socket.abort();
            return;
        }
        m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "auth"}, {"id", "qt-auth"}, {"token", m_token},
            {"protocols", QJsonArray{"bat-remote/v2"}}, {"compression", QJsonArray{}},
            {"args", QJsonArray{"Better Agent Terminal Qt"}}
        }).toJson(QJsonDocument::Compact)));
    });
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &HostClient::receive);
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        m_ready = false;
        m_contextId.clear();
        m_keepalive.stop();
        m_authDeadline.stop();
        failPending(tr("The host disconnected."));
        emit disconnected();
    });
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit connectionError(m_socket.errorString());
    });
}

QByteArray HostClient::normalizedFingerprint(QString fingerprint) {
    fingerprint.remove(':');
    const QByteArray hex = fingerprint.toLatin1().toLower();
    if (hex.size() != 64) return {};
    for (char c : hex) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    return hex;
}

bool HostClient::certificateMatches(const QByteArray &der, const QByteArray &fingerprint) {
    return !der.isEmpty() && fingerprint.size() == 64 &&
        QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex() == fingerprint;
}

QString HostClient::validateConnectionUrl(const QUrl &url) {
    const QUrlQuery query(url);
    if (!url.isValid() || url.scheme() != "wss" || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || (url.port(9876) <= 0))
        return tr("Use a wss://host:port connection URL from BAT.");
    if (query.queryItemValue("token", QUrl::FullyDecoded).isEmpty())
        return tr("The connection URL is missing its token.");
    if (normalizedFingerprint(query.queryItemValue("fp", QUrl::FullyDecoded)).isEmpty())
        return tr("The connection URL needs a SHA-256 certificate fingerprint.");
    return {};
}

void HostClient::connectTo(const QUrl &connectionUrl) {
    disconnectFromHost();
    const QString error = validateConnectionUrl(connectionUrl);
    if (!error.isEmpty()) { emit connectionError(error); return; }
    const QUrlQuery query(connectionUrl);
    m_token = query.queryItemValue("token", QUrl::FullyDecoded);
    m_fingerprint = normalizedFingerprint(query.queryItemValue("fp", QUrl::FullyDecoded));
    QUrl transport = connectionUrl;
    transport.setQuery(QString()); // Credentials are only sent inside the authenticated TLS socket.
    transport.setPort(connectionUrl.port(9876));
    m_authDeadline.start(15000);
    m_socket.open(transport);
}

void HostClient::disconnectFromHost() {
    m_ready = false;
    m_contextId.clear();
    m_token.clear();
    m_keepalive.stop();
    m_authDeadline.stop();
    m_socket.abort();
    failPending(tr("The host connection changed."));
}

void HostClient::invoke(const QString &channel, const QJsonObject &params, QObject *owner,
                        Reply reply, int timeoutMs, bool scoped) {
    if (!m_ready) { if (owner) reply({}, tr("Connect to a host first.")); return; }
    const QString id = QString("qt-%1").arg(++m_sequence);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    m_pending.insert(id, {owner, std::move(reply), timer});
    connect(timer, &QTimer::timeout, this, [this, id, channel] {
        finish(id, {}, tr("The host request timed out: %1").arg(channel));
    });
    timer->start(timeoutMs);
    QJsonObject frame{{"type", "invoke"}, {"id", id}, {"channel", channel}, {"params", params}};
    if (scoped && !m_contextId.isEmpty()) frame.insert("contextId", m_contextId);
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

void HostClient::receive(const QString &text) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return;
    const auto frame = doc.object();
    const QString type = frame["type"].toString();
    if (type == "auth-result") {
        m_authDeadline.stop();
        m_token.clear();
        if (!frame["result"].toBool() || frame["protocol"] != "bat-remote/v2" ||
            frame["compression"].toString("none") != "none") {
            emit connectionError(frame["error"].toString(tr("Host authentication failed.")));
            m_socket.abort();
            return;
        }
        m_ready = true;
        m_keepalive.start();
        emit connected();
    } else if (type == "invoke-result" || type == "invoke-error") {
        finish(frame["id"].toString(), frame["result"], frame["error"].toString());
    } else if (type == "event" && m_ready) {
        // A profile context is authoritative; never deliver a different profile's events.
        if (!m_contextId.isEmpty() && frame["contextId"].toString() != m_contextId) return;
        QString channel = frame["channel"].toString();
        if (channel.startsWith("agent:")) channel.replace(0, 6, "claude:");
        emit eventReceived(channel, frame["params"].toObject());
    }
}

void HostClient::finish(const QString &id, const QJsonValue &value, const QString &error) {
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const auto pending = it.value();
    m_pending.erase(it);
    pending.timer->stop();
    pending.timer->deleteLater();
    if (pending.owner) pending.reply(value, error);
}

void HostClient::failPending(const QString &error) {
    // Callbacks may enqueue work or destroy panels, so remove before notifying.
    const auto ids = m_pending.keys();
    for (const auto &id : ids) finish(id, {}, error);
}
