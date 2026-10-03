#pragma once
#include "transcript.h"
#include <QWidget>
class HostClient;
class QTextBrowser;
class QPlainTextEdit;
class QComboBox;
class QLabel;
class QPushButton;

class AgentPanel : public QWidget {
    Q_OBJECT
public:
    AgentPanel(HostClient *host, const QString &directory, QWidget *parent = nullptr);
    AgentPanel(HostClient *host, const QJsonObject &descriptor, QWidget *parent = nullptr);
    void attach();
    void stopSession();
    bool busy() const { return m_busy; }
    QJsonObject descriptor() const;
signals:
    void descriptorChanged();
    void attentionRequired(const QString &title);
private:
    void send();
    void sendPrompt(const QString &prompt);
    void event(const QString &channel, const QJsonObject &params);
    void render();
    void requestPermission(const QJsonObject &data);
    void askUser(const QJsonObject &data);
    HostClient *m_host;
    QString m_directory;
    QString m_sessionId;
    QJsonObject m_descriptor;
    bool m_attaching = false;
    bool m_started = false;
    bool m_busy = false;
    Transcript m_transcript;
    QTextBrowser *m_view;
    QPlainTextEdit *m_input;
    QComboBox *m_runtime;
    QLabel *m_status;
    QPushButton *m_send;
};
