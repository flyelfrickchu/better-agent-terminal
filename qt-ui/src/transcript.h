#pragma once
#include <QJsonObject>
#include <QSet>
#include <QStringList>

// Separate event adaptation from widgets so streaming/history can be checked
// against the renderer-facing contract without starting an agent.
class Transcript {
public:
    void stream(const QJsonObject &data);
    void message(const QJsonObject &message);
    void finishTurn();
    void clear();
    QString markdown() const;
    static QString contentText(const QJsonValue &content);
private:
    QStringList m_entries;
    QString m_stream;
    QString m_thinking;
    QSet<QString> m_ids;
};
