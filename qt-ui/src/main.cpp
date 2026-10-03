#include "mainwindow.h"
#include "terminalpanel.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QStatusBar>
#include <QTextStream>
#include <QRegularExpression>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("better-agent-terminal-qt");
    QApplication::setOrganizationName("BetterAgentTerminal");
    QApplication::setApplicationVersion("0.1.0-dev");
    QCommandLineParser parser;
    parser.setApplicationDescription("Native Qt UI with KDE Konsole and the BAT Rust host");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"server", "Rust bat-server executable", "path"});
    parser.addOption({"connect", "Connect to an existing host using its wss URL", "url"});
    parser.addOption({"data-dir", "Local Rust host state directory", "path"});
    parser.addOption({"smoke-test", "Check embedded Konsole and the local Rust connection, then exit"});
    parser.process(app);
    QTemporaryDir smokeDirectory;
    const bool smoke = parser.isSet("smoke-test");
    QString dataDirectory = parser.value("data-dir");
    if (dataDirectory.isEmpty()) dataDirectory = qEnvironmentVariable("BAT_QT_DATA_DIR");
    if (dataDirectory.isEmpty()) dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (smoke) {
        if (!smokeDirectory.isValid()) return 1;
        dataDirectory = smokeDirectory.path();
    }
    QString server = parser.value("server");
    if (server.isEmpty()) server = qEnvironmentVariable("BAT_QT_SERVER");
    if (server.isEmpty()) {
        const QStringList candidates{QCoreApplication::applicationDirPath() + "/bat-server",
            QString(BAT_SOURCE_DIR) + "/src-tauri/target/qt/debug/bat-server",
            QString(BAT_SOURCE_DIR) + "/src-tauri/target/release/bat-server"};
        for (const auto &candidate : candidates) if (QFileInfo::exists(candidate)) { server = candidate; break; }
    }
    MainWindow window(dataDirectory);
    window.show();
    if (parser.isSet("connect")) window.connectToHost(QUrl(parser.value("connect")));
    else window.startLocalHost(server);
    if (smoke) {
        auto *terminal = new TerminalPanel(dataDirectory);
        terminal->show();
        if (!terminal->available()) return 2;
        // Exercise terminal input and shell cwd rather than just plugin loading.
        QTimer::singleShot(1000, terminal, [terminal] { terminal->sendInput("printf '%s' \"$PWD\" > konsole-smoke.txt\n"); });
        auto *poll = new QTimer(&app);
        QObject::connect(poll, &QTimer::timeout, &app, [&app, &window, dataDirectory] {
            QFile marker(dataDirectory + "/konsole-smoke.txt");
            if (!window.hostReady() || !marker.open(QIODevice::ReadOnly)) return;
            if (QString::fromUtf8(marker.readAll()) != dataDirectory) { app.exit(3); return; }
            app.exit(0);
        });
        poll->start(100);
        QTimer::singleShot(20000, &app, [&app, &window] {
            auto message = window.statusBar()->currentMessage();
            message.replace(QRegularExpression("token=[^&\\s]+"), "token=[redacted]");
            QTextStream(stderr) << "Qt smoke test timed out: " << message << '\n';
            app.exit(4);
        });
        QObject::connect(&app, &QCoreApplication::aboutToQuit, terminal, &QObject::deleteLater);
    }
    return app.exec();
}
