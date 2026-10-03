#include "terminalpanel.h"
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <kde_terminal_interface.h>
#include <QLabel>
#include <QVBoxLayout>

TerminalPanel::TerminalPanel(const QString &directory, const QString &program,
                             const QStringList &arguments, QWidget *parent, const QString &profile) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    const auto factory = KPluginFactory::loadFactory(KPluginMetaData("kf6/parts/konsolepart"));
    if (factory.plugin) m_part = factory.plugin->create<KParts::ReadOnlyPart>(this);
    auto *terminal = m_part ? qobject_cast<TerminalInterface *>(m_part.data()) : nullptr;
    if (!terminal) {
        delete m_part.data();
        auto *message = new QLabel(tr("Konsole could not be loaded.\nInstall konsole-kpart and reopen this terminal."), this);
        message->setAlignment(Qt::AlignCenter);
        message->setWordWrap(true);
        layout->addWidget(message);
        return;
    }
    layout->addWidget(m_part->widget());
    if (!profile.isEmpty()) terminal->setCurrentProfile(profile);
    setFocusProxy(m_part->widget());
    connect(m_part, &QObject::destroyed, this, [this] { emit exited(); });
    // Konsole owns both the display and this local PTY. Do not also create a
    // Rust PTY for this tab: there must be exactly one process owner.
    if (program.isEmpty()) {
        terminal->showShellInDir(directory);
    } else {
        // openUrl starts a shell immediately. Use env's cwd option to start
        // the requested program directly, without shell interpolation or
        // changing the application's process-wide working directory.
        terminal->startProgram("/usr/bin/env", QStringList{"env", "--chdir=" + directory, program} + arguments);
    }
}

TerminalPanel::~TerminalPanel() {
    // KPart destruction during widget teardown is not a process-exit event.
    // Disconnect before QWidget destroys children, when ancestor tabs may
    // already be partially destroyed and this panel's members are gone.
    if (m_part) QObject::disconnect(m_part.data(), nullptr, this, nullptr);
}

void TerminalPanel::sendInput(const QString &text) {
    if (m_part) {
        if (auto *terminal = qobject_cast<TerminalInterface *>(m_part.data())) terminal->sendInput(text);
    }
}

QStringList TerminalPanel::availableProfiles() const {
    if (m_part) if (auto *terminal = qobject_cast<TerminalInterface *>(m_part.data())) return terminal->availableProfiles();
    return {};
}
