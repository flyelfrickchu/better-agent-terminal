#include "externalterminal.h"
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWindow>
#include <QProcessEnvironment>
#include <cstring>
#ifdef BAT_HAVE_XCB
#include <xcb/xcb.h>
#include <csignal>
#include <unistd.h>
#include <cstdlib>
#endif

namespace {
bool splitArguments(const QString &text, QStringList &result) {
    // QProcess uses double quotes for grouping and triple quotes for a literal
    // quote. Reject an unfinished group rather than silently changing argv.
    bool quoted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.mid(i, 3) == "\"\"\"") i += 2;
        else if (text[i] == '"') quoted = !quoted;
    }
    if (quoted) return false;
    result = QProcess::splitCommand(text);
    return true;
}
}

struct ExternalTerminalPanel::Native {
#ifdef BAT_HAVE_XCB
    xcb_connection_t *connection = nullptr;
    xcb_window_t root = 0, window = 0;
    xcb_atom_t pid = 0, transient = 0, windowType = 0, normalType = 0;
    ~Native() { if (connection) xcb_disconnect(connection); }
    xcb_atom_t atom(const char *name) {
        auto *reply = xcb_intern_atom_reply(connection,
            xcb_intern_atom(connection, false, uint16_t(strlen(name)), name), nullptr);
        const xcb_atom_t value = reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
        free(reply); return value;
    }
    uint32_t property(xcb_window_t target, xcb_atom_t name, xcb_atom_t type) {
        auto *reply = xcb_get_property_reply(connection,
            xcb_get_property(connection, false, target, name, type, 0, 1), nullptr);
        uint32_t value = 0;
        if (reply && reply->format == 32 && xcb_get_property_value_length(reply) == 4)
            memcpy(&value, xcb_get_property_value(reply), sizeof(value));
        free(reply); return value;
    }
    xcb_window_t findWindow(xcb_window_t parent, qint64 group, int depth = 0) {
        if (depth > 4) return 0;
        auto *tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, parent), nullptr);
        if (!tree) return 0;
        xcb_window_t found = 0;
        const auto *children = xcb_query_tree_children(tree);
        for (int i = 0; i < xcb_query_tree_children_length(tree) && !found; ++i) {
            const auto child = children[i];
            if (child == window) continue;
            const auto owner = property(child, pid, XCB_ATOM_CARDINAL);
            // Match only the session we started. Never attach an existing
            // terminal just because its class or executable name matches.
            if (owner > 0 && ::getpgid(pid_t(owner)) == group) {
                const auto type = property(child, windowType, XCB_ATOM_ATOM);
                if ((type && type != normalType) || property(child, transient, XCB_ATOM_WINDOW)) continue;
                auto *attributes = xcb_get_window_attributes_reply(connection,
                    xcb_get_window_attributes(connection, child), nullptr);
                if (attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE &&
                    !attributes->override_redirect &&
                    attributes->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT) found = child;
                free(attributes);
            }
            // WM decoration frames have no PID. Do not walk every widget in
            // unrelated applications (especially over a remote X11 display).
            if (!found && !owner) found = findWindow(child, group, depth + 1);
        }
        free(tree); return found;
    }
#endif
};

QString ExternalTerminalPanel::platformError() {
#ifdef BAT_HAVE_XCB
    if (QGuiApplication::platformName() == "xcb") return {};
#else
    return tr("This build does not include X11 third-party terminal embedding. Use embedded Konsole.");
#endif
    return tr("Third-party terminal embedding requires BAT running with the X11 (xcb) backend. Use embedded Konsole on other backends.");
}

QString ExternalTerminalPanel::validate(const QJsonObject &settings) {
    const QString executable = settings["terminalExecutable"].toString().trimmed();
    if (executable.isEmpty()) return tr("Choose an installed terminal executable.");
    if (executable.contains('/') && !QDir::isAbsolutePath(executable))
        return tr("Use an absolute executable path or a program name from PATH.");
    const QString resolved = QStandardPaths::findExecutable(executable);
    if (resolved.isEmpty() || !QFileInfo(resolved).isFile())
        return tr("Terminal executable was not found or is not executable: %1").arg(executable);
    QStringList arguments;
    if (!splitArguments(settings["terminalArguments"].toString(), arguments) ||
        !splitArguments(settings["terminalCommandArguments"].toString("-e {command}"), arguments))
        return tr("Close the double quotes in terminal arguments.");
    if (arguments.count("{command}") != 1)
        return tr("Command arguments must contain {command} once as a separate argument.");
    return {};
}

bool ExternalTerminalPanel::launchCommand(const QJsonObject &settings, const QString &directory,
    const QString &program, const QStringList &programArguments,
    QString &executable, QStringList &arguments, QString &error) {
    error = validate(settings);
    if (!error.isEmpty()) return false;
    if (!QFileInfo(directory).isDir()) { error = tr("Workspace directory does not exist: %1").arg(directory); return false; }
    executable = QStandardPaths::findExecutable(settings["terminalExecutable"].toString().trimmed());
    splitArguments(settings["terminalArguments"].toString(), arguments);
    // Split before substitution so spaces, quotes and shell metacharacters in
    // paths stay literal. No shell interprets user-supplied arguments.
    for (auto &argument : arguments) argument.replace("{cwd}", directory);
    if (!program.isEmpty()) {
        QStringList commandArguments;
        splitArguments(settings["terminalCommandArguments"].toString("-e {command}"), commandArguments);
        for (auto argument : commandArguments) {
            if (argument == "{command}") { arguments.append(program); arguments.append(programArguments); }
            else { argument.replace("{cwd}", directory); arguments.append(argument); }
        }
    }
    return true;
}

ExternalTerminalPanel::ExternalTerminalPanel(const QJsonObject &settings, const QString &directory,
    const QString &program, const QStringList &arguments, QWidget *parent)
    : QWidget(parent), m_native(std::make_unique<Native>()) {
    auto *box = new QVBoxLayout(this); box->setContentsMargins(0, 0, 0, 0);
    m_status = new QLabel(tr("Starting embedded terminal…"), this);
    m_status->setObjectName("externalTerminalStatus");
    m_status->setAlignment(Qt::AlignCenter); m_status->setWordWrap(true); box->addWidget(m_status);
    QString executable, error; QStringList launchArguments;
    error = platformError();
    if (!error.isEmpty() || !launchCommand(settings, directory, program, arguments, executable, launchArguments, error)) {
        m_done = true; m_status->setText(error); return;
    }
#ifdef BAT_HAVE_XCB
    int screenNumber = 0;
    m_native->connection = xcb_connect(nullptr, &screenNumber);
    if (xcb_connection_has_error(m_native->connection)) {
        m_done = true; m_status->setText(tr("Cannot connect to the X11 display.")); return;
    }
    auto screen = xcb_setup_roots_iterator(xcb_get_setup(m_native->connection));
    for (int i = 0; i < screenNumber && screen.rem; ++i) xcb_screen_next(&screen);
    if (!screen.rem) { m_done = true; m_status->setText(tr("X11 screen is unavailable.")); return; }
    m_native->root = screen.data->root;
    m_native->pid = m_native->atom("_NET_WM_PID");
    m_native->transient = m_native->atom("WM_TRANSIENT_FOR");
    m_native->windowType = m_native->atom("_NET_WM_WINDOW_TYPE");
    m_native->normalType = m_native->atom("_NET_WM_WINDOW_TYPE_NORMAL");
    m_process.setChildProcessModifier([] { ::setsid(); });
#endif
    auto environment = QProcessEnvironment::systemEnvironment();
    // AppImage/SDK library and plugin paths belong to BAT, not to installed
    // applications (which can use different Qt/GTK versions).
    for (const auto *key : {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                            "PYTHONHOME", "PYTHONPATH"}) environment.remove(key);
    environment.insert("GDK_BACKEND", "x11"); environment.insert("QT_QPA_PLATFORM", "xcb");
    environment.insert("WINIT_UNIX_BACKEND", "x11"); environment.remove("WAYLAND_DISPLAY");
    m_process.setProcessEnvironment(environment); m_process.setWorkingDirectory(directory);
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_output.append(m_process.readAllStandardOutput());
        m_output = m_output.right(4096);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail(tr("Cannot start terminal: %1").arg(m_process.errorString()));
    });
    connect(&m_process, &QProcess::started, this, [this] {
        m_processGroup = m_process.processId(); m_startTime.start(); m_poll.start(50);
    });
    connect(&m_poll, &QTimer::timeout, this, &ExternalTerminalPanel::pollWindow);
    m_process.start(executable, launchArguments);
}

ExternalTerminalPanel::~ExternalTerminalPanel() {
    m_poll.stop(); QObject::disconnect(&m_process, nullptr, this, nullptr);
    stopProcess();
    // Release the foreign window wrapper before its X11 client/connection.
    delete m_container;
}

bool ExternalTerminalPanel::available() const { return !m_done && m_container; }

void ExternalTerminalPanel::stopProcess() {
#ifdef BAT_HAVE_XCB
    if (m_processGroup > 0) {
        ::kill(-pid_t(m_processGroup), SIGTERM);
        if (m_process.state() != QProcess::NotRunning) m_process.waitForFinished(300);
        ::kill(-pid_t(m_processGroup), SIGKILL);
        m_processGroup = 0;
    }
#endif
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill(); m_process.waitForFinished(300);
    }
}

void ExternalTerminalPanel::fail(const QString &message) {
    if (m_done) return;
    m_done = true; m_poll.stop(); stopProcess();
    delete m_container; m_container = nullptr;
    m_status->setText(message); m_status->show();
    // Keep startup errors visible even when automatic tab closure is enabled.
}

void ExternalTerminalPanel::pollWindow() {
#ifdef BAT_HAVE_XCB
    if (m_done) return;
    if (xcb_connection_has_error(m_native->connection)) { fail(tr("The X11 display disconnected.")); return; }
    if (m_native->window) {
        auto *reply = xcb_get_geometry_reply(m_native->connection,
            xcb_get_geometry(m_native->connection, m_native->window), nullptr);
        const bool exists = reply; free(reply);
        if (!exists) {
            m_done = true; m_poll.stop(); stopProcess();
            delete m_container; m_container = nullptr;
            m_status->setText(tr("Terminal exited.")); m_status->show(); emit exited();
        } else if (m_native->findWindow(m_native->root, m_processGroup)) {
            fail(tr("The terminal opened multiple windows. Configure one window per process for BAT embedding, then reopen this tab."));
        }
        return;
    }
    const auto window = m_native->findWindow(m_native->root, m_processGroup);
    if (window) {
        auto *foreign = QWindow::fromWinId(WId(window));
        if (!foreign) { fail(tr("This terminal window cannot be embedded.")); return; }
        m_native->window = window;
        m_container = QWidget::createWindowContainer(foreign, this);
        m_container->setObjectName("externalTerminalContainer");
        m_container->setFocusPolicy(Qt::StrongFocus); setFocusProxy(m_container);
        layout()->addWidget(m_container); m_status->hide(); m_container->show();
        m_poll.setInterval(250);
    } else if (m_startTime.elapsed() > 15000 ||
               (m_process.state() == QProcess::NotRunning && m_process.exitCode() != 0)) {
        fail(tr("No embeddable X11 terminal window appeared. Use a new-process option and an X11-capable terminal.\n%1")
            .arg(QString::fromLocal8Bit(m_output).trimmed()));
    }
#endif
}
