#include "transcript.h"
#include <QJsonArray>
#include <QJsonDocument>

QString Transcript::contentText(const QJsonValue &content) {
    if (content.isString()) return content.toString();
    QStringList parts;
    for (const auto &value : content.toArray()) {
        const auto block = value.toObject();
        if (block["type"] == "text") parts.append(block["text"].toString());
        else if (block["type"] == "thinking") parts.append(block["thinking"].toString());
    }
    return parts.join("\n\n");
}

void Transcript::stream(const QJsonObject &data) {
    // Subagent streams have their own tool row; do not mix them into the main answer.
    if (!data["parentToolUseId"].isNull() && !data["parentToolUseId"].isUndefined() &&
        !data["parentToolUseId"].toString().isEmpty()) return;
    m_stream += data["text"].toString();
    m_thinking += data["thinking"].toString();
}

void Transcript::message(const QJsonObject &message) {
    const QString id = message["uuid"].toString(message["id"].toString());
    if (!id.isEmpty() && m_ids.contains(id)) return;
    if (!id.isEmpty()) m_ids.insert(id);
    const QString role = message["role"].toString(message["type"].toString("assistant"));
    const auto nested = message["message"].toObject();
    const auto content = message.contains("content") ? message["content"] : nested["content"];
    const QString text = contentText(content);
    if (text.isEmpty()) return; // Tool-only messages must not discard an open text stream.
    if (role == "assistant") { m_stream.clear(); m_thinking.clear(); }
    m_entries.append(QString("### %1\n\n%2").arg(role == "user" ? "You" : role == "assistant" ? "Assistant" : "System", text));
}

void Transcript::finishTurn() {
    if (!m_stream.isEmpty()) m_entries.append("### Assistant\n\n" + m_stream);
    m_stream.clear();
    m_thinking.clear();
}
void Transcript::clear() { m_entries.clear(); m_stream.clear(); m_thinking.clear(); m_ids.clear(); }
QString Transcript::markdown() const {
    QString result = m_entries.join("\n\n---\n\n");
    if (!m_thinking.isEmpty()) result += "\n\n### Reasoning\n\n" + m_thinking;
    if (!m_stream.isEmpty()) result += "\n\n### Assistant\n\n" + m_stream;
    return result;
}
