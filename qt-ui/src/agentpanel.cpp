#include "agentpanel.h"
#include "hostclient.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QScrollBar>
#include <QTextBrowser>
#include <QUuid>
#include <QVBoxLayout>

AgentPanel::AgentPanel(HostClient *host, const QString &directory, QWidget *parent)
    : QWidget(parent), m_host(host), m_directory(directory),
      m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    auto *layout = new QVBoxLayout(this);
    m_runtime = new QComboBox(this);
    m_runtime->addItem(tr("Claude"), "claude");
    m_runtime->addItem(tr("Codex"), "codex-agent");
    layout->addWidget(m_runtime);
    m_view = new QTextBrowser(this);
    m_view->setOpenExternalLinks(false);
    layout->addWidget(m_view, 1);
    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(tr("Message your agent…"));
    m_input->setMaximumHeight(140);
    layout->addWidget(m_input);
    auto *buttons = new QHBoxLayout;
    m_send = new QPushButton(tr("Send"), this);
    auto *stop = new QPushButton(tr("Stop"), this);
    buttons->addWidget(m_send);
    buttons->addWidget(stop);
    layout->addLayout(buttons);
    m_status = new QLabel(tr("Connect an agent host to begin"), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    connect(m_send, &QPushButton::clicked, this, &AgentPanel::send);
    connect(stop, &QPushButton::clicked, this, [this] {
        if (!m_started) return;
        m_host->invoke("claude:abort-session", {{"sessionId", m_sessionId}}, this,
            [this](const QJsonValue &, const QString &error) {
                if (!error.isEmpty()) m_status->setText(error);
            });
    });
    connect(m_host, &HostClient::eventReceived, this, &AgentPanel::event);
    connect(m_host, &HostClient::disconnected, this, [this] {
        // Profile pages are recreated when the host/profile binding changes.
        // Retain this ID for an ordinary reconnect to the same binding.
        m_started = false;
        m_attaching = false;
        m_busy = false;
        m_runtime->setEnabled(true);
        m_send->setEnabled(true);
        m_status->setText(tr("Disconnected. Reconnect to start a new session."));
    });
}

AgentPanel::AgentPanel(HostClient *host, const QJsonObject &descriptor, QWidget *parent)
    : AgentPanel(host, descriptor["cwd"].toString(), parent) {
    m_descriptor = descriptor;
    m_sessionId = descriptor["id"].toString(m_sessionId);
    const int runtime = m_runtime->findData(descriptor["agentPreset"].toString());
    if (runtime >= 0) m_runtime->setCurrentIndex(runtime);
    connect(m_runtime, &QComboBox::currentIndexChanged, this, [this] { emit descriptorChanged(); });
}

QJsonObject AgentPanel::descriptor() const {
    auto result = m_descriptor;
    result.insert("id", m_sessionId);
    result.insert("cwd", m_directory);
    result.insert("agentPreset", m_runtime->currentData().toString());
    return result;
}

void AgentPanel::attach() {
    if (!m_host->ready() || m_host->contextId().isEmpty() || m_attaching || m_started) return;
    m_attaching = true;
    m_send->setEnabled(false);
    m_host->invoke("claude:get-session-state", {{"sessionId", m_sessionId}}, this,
        [this](const QJsonValue &value, const QString &error) {
            m_attaching = false;
            m_send->setEnabled(true);
            if (!error.isEmpty()) { m_status->setText(error); return; }
            if (!value.isObject()) return;
            const auto state = value.toObject();
            m_started = true;
            m_busy = state["isStreaming"].toBool();
            m_runtime->setEnabled(false);
            m_send->setEnabled(!m_busy);
            m_transcript.clear();
            for (const auto &item : state["messages"].toArray()) m_transcript.message(item.toObject());
            m_transcript.stream({{"text", state["streamingText"]}, {"thinking", state["streamingThinking"]}});
            m_status->setText(m_busy ? tr("Working…") : tr("Ready"));
            render();
            if (state["pendingPermission"].isObject()) requestPermission(state["pendingPermission"].toObject());
            if (state["pendingAskUser"].isObject()) askUser(state["pendingAskUser"].toObject());
        });
}

void AgentPanel::stopSession() {
    if (!m_started || !m_host->ready()) return;
    // The callback owner is the host: closing this widget must not cancel
    // submission or leave a callback retaining the deleted panel.
    m_host->invoke("claude:stop-session", {{"sessionId", m_sessionId}}, m_host,
        [](const QJsonValue &, const QString &) {});
    m_started = false;
}

void AgentPanel::send() {
    const auto prompt = m_input->toPlainText().trimmed();
    if (prompt.isEmpty() || m_busy || m_attaching) return;
    if (!m_host->ready() || m_host->contextId().isEmpty()) {
        m_status->setText(tr("Connect an agent host and wait for its profile to open."));
        return;
    }
    m_busy = true;
    m_send->setEnabled(false);
    m_runtime->setEnabled(false);
    if (m_started) { sendPrompt(prompt); return; }
    QJsonObject options{{"cwd", m_directory}, {"permissionMode", m_descriptor["permissionMode"].toString("default")}};
    for (const auto &key : {"model", "effort", "sdkSessionId", "codexSandboxMode", "codexApprovalPolicy"})
        if (m_descriptor.contains(key)) options.insert(key, m_descriptor[key]);
    if (m_runtime->currentData().toString() == "codex-agent") options.insert("agentPreset", "codex-agent");
    m_status->setText(tr("Starting session…"));
    QJsonObject params{{"sessionId", m_sessionId}, {"options", options}};
    const QString sdkSessionId = m_descriptor["sdkSessionId"].toString();
    if (!sdkSessionId.isEmpty()) params.insert("sdkSessionId", sdkSessionId);
    m_host->invoke(sdkSessionId.isEmpty() ? "claude:start-session" : "claude:resume-session", params, this,
        [this, prompt](const QJsonValue &, const QString &error) {
            if (!error.isEmpty()) {
                m_busy = false;
                m_send->setEnabled(true);
                m_runtime->setEnabled(true);
                m_status->setText(error);
                return;
            }
            m_started = true;
            sendPrompt(prompt);
        }, 120000);
}

void AgentPanel::sendPrompt(const QString &prompt) {
    m_status->setText(tr("Working…"));
    m_host->invoke("claude:send-message", {{"sessionId", m_sessionId}, {"prompt", prompt}}, this,
        [this, prompt](const QJsonValue &, const QString &error) {
            if (!error.isEmpty()) {
                m_busy = false;
                m_send->setEnabled(true);
                m_status->setText(error);
                return;
            }
            // The canonical user message arrives through claude:message.
            if (m_input->toPlainText().trimmed() == prompt) m_input->clear();
        }, 120000);
}

void AgentPanel::event(const QString &channel, const QJsonObject &params) {
    if (params["sessionId"].toString() != m_sessionId) return;
    if (channel == "claude:stream") m_transcript.stream(params["data"].toObject());
    else if (channel == "claude:message") m_transcript.message(params["message"].toObject());
    else if (channel == "claude:history") {
        m_transcript.clear();
        const auto items = params["items"].isArray() ? params["items"].toArray() : params["payload"].toArray();
        for (const auto &item : items) m_transcript.message(item.toObject());
    } else if (channel == "claude:turn-end") {
        m_transcript.finishTurn();
        m_busy = false;
        m_send->setEnabled(true);
        m_status->setText(tr("Ready"));
    } else if (channel == "claude:error") {
        m_busy = false;
        m_send->setEnabled(true);
        m_status->setText(params["error"].isString() ? params["error"].toString() : tr("Agent error"));
    } else if (channel == "claude:status") {
        const auto meta = params["meta"].toObject();
        const auto sdkSessionId = meta["sdkSessionId"].toString();
        if (!sdkSessionId.isEmpty() && m_descriptor["sdkSessionId"] != sdkSessionId) {
            m_descriptor.insert("sdkSessionId", sdkSessionId);
            emit descriptorChanged();
        }
        const auto status = meta["runtimeStatus"].toString();
        if (!status.isEmpty()) m_status->setText(status);
    } else if (channel == "claude:permission-request") {
        requestPermission(params["data"].toObject());
    } else if (channel == "claude:ask-user") {
        askUser(params["data"].toObject());
    }
    render();
}

void AgentPanel::requestPermission(const QJsonObject &data) {
    const QString toolId = data["toolUseId"].toString();
    if (toolId.isEmpty()) return;
    emit attentionRequired(tr("Agent needs permission"));
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Agent permission — %1").arg(data["toolName"].toString()));
    auto *layout = new QVBoxLayout(dialog);
    auto *reason = new QLabel(data["decisionReason"].toString(), dialog);
    reason->setWordWrap(true);
    layout->addWidget(reason);
    auto *details = new QPlainTextEdit(dialog);
    details->setReadOnly(true);
    details->setPlainText(QString::fromUtf8(QJsonDocument(data["input"].toObject()).toJson(QJsonDocument::Indented)));
    layout->addWidget(details);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const QString session = m_sessionId;
    connect(dialog, &QDialog::finished, this, [this, session, toolId, data](int response) {
        QJsonObject result{{"behavior", response == QDialog::Accepted ? "allow" : "deny"}};
        if (response == QDialog::Accepted) result.insert("updatedInput", data["input"]);
        else result.insert("message", tr("Permission denied by user."));
        m_host->invoke("claude:resolve-permission", {{"sessionId", session}, {"toolUseId", toolId}, {"result", result}}, this,
            [this](const QJsonValue &, const QString &error) { if (!error.isEmpty()) m_status->setText(error); });
    });
    connect(m_host, &HostClient::disconnected, dialog, &QDialog::reject);
    connect(m_host, &HostClient::eventReceived, dialog, [dialog, session, toolId](const QString &channel, const QJsonObject &params) {
        if (channel == "claude:permission-resolved" && params["sessionId"] == session && params["toolUseId"] == toolId) {
            dialog->blockSignals(true);
            dialog->close();
        }
    });
    dialog->resize(550, 350);
    dialog->open();
}

void AgentPanel::askUser(const QJsonObject &data) {
    const QString toolId = data["toolUseId"].toString();
    const auto questions = data["questions"].toArray();
    if (toolId.isEmpty() || questions.isEmpty()) return;
    emit attentionRequired(tr("Agent has a question"));
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Agent questions"));
    auto *layout = new QVBoxLayout(dialog);
    struct Answer { QString question; QList<QCheckBox *> options; QPlainTextEdit *text; bool multi; };
    QList<Answer> answers;
    for (const auto &value : questions) {
        const auto question = value.toObject();
        const auto title = question["question"].toString();
        auto *label = new QLabel(title, dialog);
        label->setWordWrap(true);
        layout->addWidget(label);
        Answer answer{title, {}, new QPlainTextEdit(dialog), question["multiSelect"].toBool()};
        for (const auto &optionValue : question["options"].toArray()) {
            const auto option = optionValue.toObject();
            auto *check = new QCheckBox(option["label"].toString(), dialog);
            check->setToolTip(option["description"].toString());
            if (!answer.multi) {
                const auto previous = answer.options;
                connect(check, &QCheckBox::toggled, dialog, [check, previous](bool checked) {
                    if (checked) for (auto *other : previous) other->setChecked(false);
                });
                for (auto *other : previous) connect(other, &QCheckBox::toggled, check, [check](bool checked) {
                    if (checked) check->setChecked(false);
                });
            }
            answer.options.append(check);
            layout->addWidget(check);
        }
        answer.text->setPlaceholderText(tr("Or write your answer…"));
        answer.text->setMaximumHeight(75);
        layout->addWidget(answer.text);
        answers.append(answer);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);
    const QString session = m_sessionId;
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, answers, session, toolId] {
        QJsonObject result;
        for (const auto &answer : answers) {
            QStringList values;
            for (auto *check : answer.options) if (check->isChecked()) values.append(check->text());
            const auto text = answer.text->toPlainText().trimmed();
            if (!text.isEmpty()) { if (!answer.multi) values.clear(); values.append(text); }
            if (values.isEmpty()) { m_status->setText(tr("Answer every question before submitting.")); return; }
            result.insert(answer.question, values.join(", "));
        }
        m_host->invoke("claude:resolve-ask-user", {{"sessionId", session}, {"toolUseId", toolId}, {"answers", result}}, this,
            [this, guard = QPointer<QDialog>(dialog)](const QJsonValue &, const QString &error) {
                if (!error.isEmpty()) { m_status->setText(error); return; }
                if (guard) guard->accept();
            });
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::rejected, this, [this, session] {
        m_host->invoke("claude:abort-session", {{"sessionId", session}}, this,
            [this](const QJsonValue &, const QString &error) { if (!error.isEmpty()) m_status->setText(error); });
    });
    connect(m_host, &HostClient::disconnected, dialog, &QDialog::reject);
    connect(m_host, &HostClient::eventReceived, dialog, [dialog, session, toolId](const QString &channel, const QJsonObject &params) {
        if (channel == "claude:ask-user-resolved" && params["sessionId"] == session && params["toolUseId"] == toolId) {
            dialog->blockSignals(true);
            dialog->close();
        }
    });
    dialog->resize(550, 400);
    dialog->open();
}

void AgentPanel::render() {
    auto *scroll = m_view->verticalScrollBar();
    const bool atBottom = scroll->value() >= scroll->maximum() - 8;
    const int position = scroll->value();
    m_view->setMarkdown(m_transcript.markdown());
    scroll->setValue(atBottom ? scroll->maximum() : position);
}
