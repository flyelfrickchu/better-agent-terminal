#pragma once
#include <QWidget>
#include <QPointer>
#include <QStringList>
#include <QJsonObject>

namespace KParts { class ReadOnlyPart; }
class ExternalTerminalPanel;
class TerminalPanel : public QWidget {
    Q_OBJECT
public:
    explicit TerminalPanel(const QString &directory, const QString &program = {},
                           const QStringList &arguments = {}, QWidget *parent = nullptr,
                           const QString &profile = {}, const QJsonObject &terminalSettings = {});
    ~TerminalPanel() override;
    bool available() const;
    void sendInput(const QString &text);
    QStringList availableProfiles() const;
signals:
    void exited();
private:
    QPointer<KParts::ReadOnlyPart> m_part;
    ExternalTerminalPanel *m_external = nullptr;
};
