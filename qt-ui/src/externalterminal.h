#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>

class QLabel;
class ExternalTerminalPanel : public QWidget {
    Q_OBJECT
public:
    ExternalTerminalPanel(const QJsonObject &settings, const QString &directory,
                          const QString &program, const QStringList &arguments, QWidget *parent);
    ~ExternalTerminalPanel() override;
    static QString platformError();
    static QString validate(const QJsonObject &settings);
    static bool launchCommand(const QJsonObject &settings, const QString &directory,
                              const QString &program, const QStringList &programArguments,
                              QString &executable, QStringList &arguments, QString &error);
    bool available() const;
signals:
    void exited();
private:
    void pollWindow();
    void fail(const QString &message);
    void stopProcess();
    struct Native;
    std::unique_ptr<Native> m_native;
    QProcess m_process;
    QTimer m_poll;
    QElapsedTimer m_startTime;
    QLabel *m_status;
    QWidget *m_container = nullptr;
    qint64 m_processGroup = 0;
    bool m_done = false;
    QByteArray m_output;
};
