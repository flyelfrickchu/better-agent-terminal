#pragma once
#include <QWidget>
#include <QPointer>
#include <QStringList>

namespace KParts { class ReadOnlyPart; }
class TerminalPanel : public QWidget {
    Q_OBJECT
public:
    explicit TerminalPanel(const QString &directory, const QString &program = {},
                           const QStringList &arguments = {}, QWidget *parent = nullptr,
                           const QString &profile = {});
    bool available() const { return !m_part.isNull(); }
    void sendInput(const QString &text);
    QStringList availableProfiles() const;
signals:
    void exited();
private:
    QPointer<KParts::ReadOnlyPart> m_part;
};
