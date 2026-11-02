#include "hostclient.h"
#include "transcript.h"
#include "terminalpanel.h"
#include "externalterminal.h"
#include "mainwindow.h"
#include "agentpanel.h"
#include "settingsdialog.h"
#include "gitpanel.h"
#include "x11input.h"
#include <QClipboard>
#include <QScopeGuard>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStackedWidget>
#include <QAbstractButton>
#include <QTextBlock>
#include <QTextLayout>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QToolButton>
#include <QInputDialog>
#include <KParts/ReadOnlyPart>
#include <kde_terminal_interface.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QEventLoopLocker>
#include <QTextStream>
#include <QAction>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <QWebSocketServer>
#include <QMessageBox>
#include <QMenu>
#include <QLabel>
#include <QWindow>
#include <QSaveFile>
#include <QRegularExpression>
#include <QVBoxLayout>

class CoreTest : public QObject {
    Q_OBJECT
public:
    static int runQuitProbe(QApplication &app, bool cancelFirst, bool pendingSave) {
        QTemporaryDir directory;
        if (!directory.isValid()) return 10;
        // KDE jobs may hold this lock after the last window is hidden. Quit
        // must explicitly leave the event loop so owned processes are cleaned up.
        QEventLoopLocker backgroundJob;
        MainWindow window(directory.path());
        window.m_server.start("/bin/sleep", {"60"});
        if (!window.m_server.waitForStarted()) return 11;
        auto *tabs = new QTabWidget(window.m_pages);
        window.m_pages->addWidget(tabs);
        window.m_workspaceTabs.insert("workspace", tabs);
        window.createTab({{"id", "terminal"}, {"workspaceId", "workspace"},
            {"kind", "terminal"}, {"cwd", directory.path()}}, false);
        auto *panel = qobject_cast<TerminalPanel *>(tabs->widget(0));
        auto *part = panel ? panel->findChild<KParts::ReadOnlyPart *>() : nullptr;
        auto *terminal = part ? qobject_cast<TerminalInterface *>(part) : nullptr;
        if (!terminal) return 12;
        QAction *quit = nullptr;
        for (auto *action : window.findChildren<QAction *>())
            if (action->shortcut() == QKeySequence(QKeySequence::Quit)) quit = action;
        if (!quit) return 13;
        int confirmations = 0;
        bool saveCompleted = !pendingSave;
        QTimer answers;
        connect(&answers, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QMessageBox *>(app.activeModalWidget());
            if (!dialog) return;
            const bool cancel = cancelFirst && confirmations == 0;
            ++confirmations;
            dialog->button(cancel ? QMessageBox::No : QMessageBox::Yes)->click();
            if (cancel) QTimer::singleShot(0, &window, [&] {
                if (!window.isVisible() || window.m_server.state() != QProcess::Running) {
                    app.exit(14); return;
                }
                QTextStream(stdout) << "CANCEL_PRESERVED" << Qt::endl;
                quit->trigger();
            });
        });
        answers.start(10);
        QTimer ready;
        connect(&ready, &QTimer::timeout, &window, [&] {
            const auto shellPid = terminal->terminalProcessId();
            if (shellPid <= 0) return;
            ready.stop();
            QTextStream(stdout) << "HOST_PID=" << window.m_server.processId()
                                << "\nSHELL_PID=" << shellPid << Qt::endl;
            if (pendingSave) {
                window.m_saveInFlight = true;
                QTimer::singleShot(250, &window, [&] {
                    saveCompleted = true;
                    window.m_saveInFlight = false;
                });
            }
            quit->trigger();
        });
        ready.start(10);
        QTimer::singleShot(4000, &app, [&] {
            QTextStream(stdout) << "QUIT_TIMEOUT hidden=" << !window.isVisible() << Qt::endl;
            app.exit(42);
        });
        window.show();
        const int result = app.exec();
        QTextStream(stdout) << "CONFIRMATIONS=" << confirmations << Qt::endl;
        if (!saveCompleted) return 15;
        return result;
    }
private slots:
    void quitExitsWithBackgroundJobs_data() {
        QTest::addColumn<bool>("cancelFirst");
        QTest::addColumn<bool>("pendingSave");
        QTest::newRow("confirmed") << false << false;
        QTest::newRow("cancel-then-confirm") << true << false;
        QTest::newRow("wait-for-save") << false << true;
    }
    void quitExitsWithBackgroundJobs() {
        QFETCH(bool, cancelFirst);
        QFETCH(bool, pendingSave);
        QTemporaryDir config;
        QVERIFY(config.isValid());
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("XDG_CONFIG_HOME", config.path());
        child.setProcessEnvironment(env);
        child.start(QCoreApplication::applicationFilePath(),
            {"--quit-probe", cancelFirst ? "cancel" : "confirm", pendingSave ? "saving" : "saved"});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(10000));
        const auto output = child.readAll();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, output.constData());
        QVERIFY2(output.contains(cancelFirst ? "CONFIRMATIONS=2" : "CONFIRMATIONS=1"), output.constData());
        if (cancelFirst) QVERIFY2(output.contains("CANCEL_PRESERVED"), output.constData());
#ifdef Q_OS_LINUX
        for (const auto &prefix : {QByteArray("HOST_PID="), QByteArray("SHELL_PID=")}) {
            const int start = output.indexOf(prefix);
            QVERIFY2(start >= 0, output.constData());
            const auto pid = output.mid(start + prefix.size()).split('\n').first().trimmed();
            QVERIFY(pid.toLongLong() > 0);
            QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists("/proc/" + QString::fromLatin1(pid)), 2000);
        }
#endif
    }
    void gitStaysInsideWorkspaceWithSessions() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        auto *sessions = window.m_pages;
        auto *sessionTabs = new QTabWidget(sessions);
        auto *session = new QLineEdit("Session input is retained", sessionTabs);
        sessionTabs->addTab(session, "Existing session");
        sessions->addWidget(sessionTabs);
        sessions->setCurrentWidget(sessionTabs);
        const auto dialogs = window.findChildren<QDialog *>().size();
        window.showGit();
        QCOMPARE(window.m_workspaceViews->currentIndex(), 1);
        QCOMPARE(window.findChildren<QDialog *>().size(), dialogs);
        QVERIFY(!window.m_gitPanel->isWindow());
        QCOMPARE(window.m_gitPanel->parentWidget(), window.m_workspaceSplitter);
        QCOMPARE(window.m_pages, sessions);
        QCOMPARE(sessions->currentWidget(), sessionTabs);
        QCOMPARE(session->text(), QString("Session input is retained"));
        window.m_workspaceViews->setCurrentIndex(0);
        QVERIFY(window.m_gitPanel->isHidden());
        QCOMPARE(window.m_pages, sessions);
        QCOMPARE(sessions->currentWidget(), sessionTabs);
        QCOMPARE(sessionTabs->count(), 1);
        window.showGit();
        auto *close = qobject_cast<QAbstractButton *>(window.m_workspaceViews->tabButton(1, QTabBar::RightSide));
        if (!close) close = qobject_cast<QAbstractButton *>(window.m_workspaceViews->tabButton(1, QTabBar::LeftSide));
        QVERIFY(close);
        close->click();
        QCOMPARE(window.m_workspaceViews->count(), 1);
        QCOMPARE(window.m_workspaceViews->currentIndex(), 0);
        QVERIFY(window.m_gitPanel->isHidden());
        QCOMPARE(sessions->currentWidget(), sessionTabs);
        QCOMPARE(session->text(), QString("Session input is retained"));
        QVERIFY(!window.m_workspaceViews->tabButton(0, QTabBar::LeftSide));
        QVERIFY(!window.m_workspaceViews->tabButton(0, QTabBar::RightSide));
        window.showGit();
        QCOMPARE(window.m_workspaceViews->count(), 2);
        QCOMPARE(window.m_workspaceViews->currentIndex(), 1);
    }
    void gitLongTextWrapsWithinColumns() {
        HostClient host;
        GitPanel panel(&host);
        panel.resize(1000, 500);
        panel.show();
        auto *commits = panel.findChild<QListWidget *>("git-commits");
        auto *files = panel.findChild<QTreeWidget *>("git-files");
        auto *diff = panel.findChild<QPlainTextEdit *>("git-diff");
        auto *content = panel.findChild<QPlainTextEdit *>("git-file-content");
        const QString longText(600, 'a');
        auto *commit = new QListWidgetItem(longText, commits);
        auto *file = new QTreeWidgetItem(files, {"M", longText});
        diff->setPlainText("+" + longText);
        content->setPlainText(longText);
        QTRY_VERIFY(diff->document()->firstBlock().layout()->lineCount() > 1);
        QTRY_VERIFY(commits->visualItemRect(commit).height() > commits->fontMetrics().height() * 2);
        QTRY_VERIFY(files->visualItemRect(file).height() > files->fontMetrics().height() * 2);
        QCOMPARE(diff->toPlainText(), "+" + longText);
        panel.findChild<QTabWidget *>("git-previews")->setCurrentIndex(1);
        QTRY_VERIFY(content->document()->firstBlock().layout()->lineCount() > 1);
        QCOMPARE(content->toPlainText(), longText);
    }
    void gitDiffColorsAndQuotedPaths() {
        for (bool dark : {false, true}) {
            const auto add = DiffHighlighter::lineFormat("+new", dark);
            const auto remove = DiffHighlighter::lineFormat("-old", dark);
            const auto header = DiffHighlighter::lineFormat("+++ b/file", dark);
            const auto hunk = DiffHighlighter::lineFormat("@@ -1 +1 @@", dark);
            QVERIFY(!add.isEmpty()); QVERIFY(!remove.isEmpty()); QVERIFY(!hunk.isEmpty());
            QVERIFY(add.foreground().color() != remove.foreground().color());
            QVERIFY(add.foreground().color() != header.foreground().color());
            QVERIFY(DiffHighlighter::lineFormat(" unchanged", dark).isEmpty());
        }
        QCOMPARE(GitPanel::filePath({{"status", "M"}, {"file", "a file.cpp"}}), QString("a file.cpp"));
        QCOMPARE(GitPanel::filePath({{"status", "R100"}, {"file", "old.cpp\tnew.cpp"}}), QString("new.cpp"));
        QCOMPARE(GitPanel::filePath({{"status", "R"}, {"file", "\"old name\" -> \"new name\""}}), QString("new name"));
        QCOMPARE(GitPanel::filePath({{"status", "M"}, {"file", "\"caf\\303\\251.txt\""}}), QString::fromUtf8("café.txt"));
    }
    void settingsHandleFreshAndInvalidHosts() {
        QJsonObject settings; QString error;
        QVERIFY(SettingsDialog::decodeSettings(QJsonValue(QJsonValue::Null), settings, error));
        QVERIFY(settings.isEmpty());
        QVERIFY(SettingsDialog::decodeSettings(QString("{\"fontSize\":14}"), settings, error));
        QCOMPARE(settings["fontSize"].toInt(), 14);
        QVERIFY(SettingsDialog::decodeSettings(settings, settings, error));
        QVERIFY(!SettingsDialog::decodeSettings(QString("bad json"), settings, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!SettingsDialog::decodeSettings(QString("[]"), settings, error));
    }
    void settingsPreserveUnrelatedConfiguration() {
        HostClient host;
        SettingsDialog dialog(&host, {"Default", "Development"});
        const QJsonArray statusline{QJsonObject{{"id", "model"}, {"color", "#123456"}}};
        const QJsonObject original{{"statuslineItems", statusline}, {"unknownFutureSetting", 42},
            {"qtUi", QJsonObject{{"unknownNativeSetting", true}, {"theme", "dark"}}},
            {"defaultClaudeModel", "old-model"}};
        dialog.setSettings(original);
        dialog.findChild<QLineEdit *>("defaultClaudeModel")->setText("new-model");
        dialog.findChild<QSpinBox *>("defaultTerminalCount")->setValue(3);
        auto *theme = dialog.findChild<QComboBox *>("qt.theme");
        theme->setCurrentIndex(theme->findData("light"));
        const auto updated = dialog.settings();
        QCOMPARE(updated["defaultClaudeModel"].toString(), QString("new-model"));
        QCOMPARE(updated["defaultTerminalCount"].toInt(), 3);
        QCOMPARE(updated["statuslineItems"].toArray(), statusline);
        QCOMPARE(updated["unknownFutureSetting"].toInt(), 42);
        QVERIFY(updated["qtUi"].toObject()["unknownNativeSetting"].toBool());
        QCOMPARE(updated["qtUi"].toObject()["theme"].toString(), QString("light"));
        QVERIFY(!dialog.findChild<QPushButton *>("saveSettings")->isEnabled());
    }
    void thirdPartyTerminalSettingsRoundTrip() {
        HostClient host;
        SettingsDialog dialog(&host, {});
        auto *backend = dialog.findChild<QComboBox *>("qt.terminalBackend");
        QVERIFY2(backend, "Settings must offer an embedded third-party terminal backend");
        QCOMPARE(backend->currentData().toString(), QString("konsole"));
        auto *executable = dialog.findChild<QLineEdit *>("qt.terminalExecutable");
        auto *arguments = dialog.findChild<QLineEdit *>("qt.terminalArguments");
        auto *commandArguments = dialog.findChild<QLineEdit *>("qt.terminalCommandArguments");
        QVERIFY(executable); QVERIFY(arguments); QVERIFY(commandArguments);
        dialog.setSettings({{"qtUi", QJsonObject{{"terminalBackend", "external"},
            {"terminalExecutable", "/opt/My Terminal/terminal"},
            {"terminalArguments", "--cwd \"{cwd}\""},
            {"terminalCommandArguments", "-e {command}"}, {"future", 42}}}});
        QCOMPARE(executable->text(), QString("/opt/My Terminal/terminal"));
        executable->setText("wezterm");
        const auto saved = dialog.settings()["qtUi"].toObject();
        QCOMPARE(saved["terminalBackend"].toString(), QString("external"));
        QCOMPARE(saved["terminalExecutable"].toString(), QString("wezterm"));
        QCOMPARE(saved["terminalArguments"].toString(), QString("--cwd \"{cwd}\""));
        QCOMPARE(saved["terminalCommandArguments"].toString(), QString("-e {command}"));
        QCOMPARE(saved["future"].toInt(), 42);
    }
    void terminalArgumentsKeepPathsAndCommandsLiteral() {
        QTemporaryDir base;
        const QString cwd = base.filePath("a space \"quoted\" $(touch injected)");
        QVERIFY(QDir().mkpath(cwd));
        QJsonObject settings{{"terminalExecutable", "/bin/echo"},
            {"terminalArguments", "--cwd=\"{cwd}\" --label \"two words\""},
            {"terminalCommandArguments", "-- {command}"}};
        QString executable, error; QStringList arguments;
        QVERIFY(ExternalTerminalPanel::launchCommand(settings, cwd, "/opt/my cli",
            {"literal;value", "two words"}, executable, arguments, error));
        QCOMPARE(arguments, (QStringList{"--cwd=" + cwd, "--label", "two words", "--", "/opt/my cli", "literal;value", "two words"}));
        QVERIFY(ExternalTerminalPanel::launchCommand(settings, cwd, {}, {}, executable, arguments, error));
        QCOMPARE(arguments.size(), 3); // No command separator for a plain shell.
        settings["terminalArguments"] = "\"unfinished";
        QVERIFY(!ExternalTerminalPanel::validate(settings).isEmpty());
        settings["terminalArguments"] = "";
        settings["terminalCommandArguments"] = "-e prefix{command}";
        QVERIFY(!ExternalTerminalPanel::validate(settings).isEmpty());
        settings["terminalCommandArguments"] = "-e {command}";
        settings["terminalExecutable"] = "/does/not/exist";
        QVERIFY(!ExternalTerminalPanel::validate(settings).isEmpty());
        QVERIFY(!QFileInfo::exists(base.filePath("injected")));
    }
    void thirdPartyWindowEmbedsResizesRestoresAndCleansUp() {
        if (QGuiApplication::platformName() != "xcb" || !ExternalTerminalPanel::platformError().isEmpty())
            QSKIP("Requires the X11 backend; run under Xvfb with QT_QPA_PLATFORM=xcb");
        QTemporaryDir directory;
        QProcess unrelated; unrelated.start("/bin/sleep", {"60"}); QVERIFY(unrelated.waitForStarted());
        MainWindow window(directory.path());
        auto *tabs = new QTabWidget(window.m_pages);
        window.m_pages->addWidget(tabs); window.m_workspaceTabs.insert("workspace", tabs);
        // The test binary uses an isolated Qt/KDE SDK. Restore that SDK only
        // for this fixture, after BAT strips its private runtime environment.
        const QString probeArguments = QString("\"LD_LIBRARY_PATH=%1\" \"QT_PLUGIN_PATH=%2\" \"%3\" --terminal-window-probe {cwd} \"two words\"")
            .arg(qEnvironmentVariable("LD_LIBRARY_PATH"), qEnvironmentVariable("QT_PLUGIN_PATH"), QCoreApplication::applicationFilePath());
        const QJsonObject configuration{{"terminalBackend", "external"},
            {"terminalExecutable", "/usr/bin/env"},
            {"terminalArguments", probeArguments + " temporary-splash"},
            {"terminalCommandArguments", "-- {command}"}};
        const QJsonObject descriptor{{"id", "third-party"}, {"kind", "terminal"},
            {"workspaceId", "workspace"}, {"cwd", directory.path()}, {"terminalSettings", configuration}};
        window.createTab(descriptor, false); window.m_pages->setCurrentWidget(tabs); window.show();
        auto *panel = qobject_cast<TerminalPanel *>(tabs->widget(0)); QVERIFY(panel);
        QTRY_VERIFY_WITH_TIMEOUT(panel->available(), 5000);
        QTest::qWait(700); QVERIFY(panel->available()); // A closing splash must not stop the terminal.
        QVERIFY(!panel->findChild<KParts::ReadOnlyPart *>());
        auto *container = panel->findChild<QWidget *>("externalTerminalContainer"); QVERIFY(container);
        auto readProbe = [&] {
            QFile file(directory.filePath("terminal-probe.json"));
            if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};
            return QJsonDocument::fromJson(file.readAll()).object();
        };
        QTRY_VERIFY(!readProbe().isEmpty());
        const qint64 pid = readProbe()["pid"].toInteger(); QVERIFY(pid > 0);
        QCOMPARE(readProbe()["cwd"].toString(), directory.path());
        QCOMPARE(readProbe()["args"].toArray().first().toString(), QString("two words"));
        QTRY_VERIFY(container->isVisible());
        const int before = container->width(); window.resize(window.width() + 120, window.height() + 80);
        QTRY_VERIFY(container->width() > before);
        QTRY_COMPARE(readProbe()["width"].toInt(), container->width());
        QTRY_VERIFY(readProbe()["parent"].toString() != readProbe()["root"].toString());
        window.m_settings = {{"qtUi", QJsonObject{{"terminalBackend", "konsole"}}}};
        window.createTab(descriptor, false); QCOMPARE(tabs->count(), 1); // No duplicate instance on reload.
        QVERIFY(panel->available());
        QSignalSpy exited(panel, &TerminalPanel::exited);
        window.clearPages();
        QTRY_VERIFY(!QFileInfo::exists(QString("/proc/%1").arg(pid)));
        QCOMPARE(exited.count(), 0);
        QCOMPARE(unrelated.state(), QProcess::Running);
        // Restore after a settings change still uses the saved third-party app.
        tabs = new QTabWidget(window.m_pages); window.m_pages->addWidget(tabs);
        window.m_workspaceTabs.insert("workspace", tabs);
        window.createTab(descriptor, false);
        panel = qobject_cast<TerminalPanel *>(tabs->widget(0)); QVERIFY(panel);
        QTRY_VERIFY_WITH_TIMEOUT(panel->available(), 5000);
        QSignalSpy restoredExited(panel, &TerminalPanel::exited);
        QFile exitRequest(directory.filePath("exit-terminal")); QVERIFY(exitRequest.open(QIODevice::WriteOnly)); exitRequest.close();
        QTRY_COMPARE_WITH_TIMEOUT(restoredExited.count(), 1, 5000);
        QVERIFY(!panel->available());
        unrelated.kill(); QVERIFY(unrelated.waitForFinished());
    }
    void unsupportedTerminalShowsAnErrorWithoutLaunching() {
        if (QGuiApplication::platformName() == "xcb") QSKIP("Checks the non-X11 failure path");
        QTemporaryDir directory;
        TerminalPanel panel(directory.path(), {}, {}, nullptr, {},
            {{"terminalBackend", "external"}, {"terminalExecutable", "/bin/echo"},
             {"terminalCommandArguments", "-e {command}"}});
        QVERIFY(!panel.available());
        const auto *status = panel.findChild<QLabel *>("externalTerminalStatus");
        QVERIFY(status); QVERIFY(status->text().contains("X11"));
        QVERIFY(!panel.findChild<KParts::ReadOnlyPart *>());
    }
    void thirdPartyExtraWindowsAreStopped() {
        if (QGuiApplication::platformName() != "xcb" || !ExternalTerminalPanel::platformError().isEmpty())
            QSKIP("Requires X11");
        QTemporaryDir directory;
        const auto arguments = QString("\"LD_LIBRARY_PATH=%1\" \"QT_PLUGIN_PATH=%2\" \"%3\" --terminal-window-probe {cwd} extra-window")
            .arg(qEnvironmentVariable("LD_LIBRARY_PATH"), qEnvironmentVariable("QT_PLUGIN_PATH"), QCoreApplication::applicationFilePath());
        TerminalPanel panel(directory.path(), {}, {}, nullptr, {}, {{"terminalBackend", "external"},
            {"terminalExecutable", "/usr/bin/env"}, {"terminalArguments", arguments},
            {"terminalCommandArguments", "-e {command}"}});
        panel.show();
        const auto *status = panel.findChild<QLabel *>("externalTerminalStatus"); QVERIFY(status);
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("multiple windows"), 5000);
        QVERIFY(!panel.available());
        QFile file(directory.filePath("terminal-probe.json")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto pid = QJsonDocument::fromJson(file.readAll()).object()["pid"].toInteger();
        QVERIFY(pid > 0); QTRY_VERIFY(!QFileInfo::exists(QString("/proc/%1").arg(pid)));
    }
    void installedThirdPartyTerminal_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("closeWhileRunning");
        const auto names = qEnvironmentVariable("BAT_TEST_TERMINALS").split(',', Qt::SkipEmptyParts);
        if (names.isEmpty()) QTest::newRow("opt-in") << QString() << false;
        for (const auto &name : names) {
            QTest::newRow(qPrintable(name + "-exit")) << name << false;
            QTest::newRow(qPrintable(name + "-close-tab")) << name << true;
        }
    }
    void installedThirdPartyTerminal() {
        QFETCH(QString, name);
        QFETCH(bool, closeWhileRunning);
        if (name.isEmpty()) QSKIP("Set BAT_TEST_TERMINALS=wezterm,ghostty for installed-app integration tests");
        QVERIFY2(ExternalTerminalPanel::platformError().isEmpty(), "Installed-app integration requires X11");
        QTemporaryDir directory;
        QJsonObject settings{{"terminalBackend", "external"}, {"terminalExecutable", name},
            {"terminalArguments", name == "wezterm"
                ? "--config enable_wayland=false start --always-new-process --cwd {cwd}"
                : "--gtk-single-instance=false --working-directory={cwd}"},
            {"terminalCommandArguments", name == "wezterm" ? "-- {command}" : "-e {command}"}};
        auto panel = std::make_unique<TerminalPanel>(directory.path(), "/bin/sh", QStringList{"-c",
            "pwd > actual-cwd.txt; echo $$ > shell-pid.txt; while [ ! -e exit-terminal ]; do sleep 0.1; done"}, nullptr, QString(), settings);
        panel->resize(640, 480); panel->show();
        QTRY_VERIFY_WITH_TIMEOUT(panel->available() || !panel->findChild<QLabel *>("externalTerminalStatus")->text().contains("Starting"), 20000);
        QVERIFY2(panel->available(), qPrintable(panel->findChild<QLabel *>("externalTerminalStatus")->text()));
        QFile output(directory.filePath("actual-cwd.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(output.exists(), 5000); QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(output.readAll()).trimmed(), directory.path());
        const auto *container = panel->findChild<QWidget *>("externalTerminalContainer"); QVERIFY(container);
        const int width = container->width(); panel->resize(800, 600); QTRY_VERIFY(container->width() > width);
        QFile shellPidFile(directory.filePath("shell-pid.txt")); QVERIFY(shellPidFile.open(QIODevice::ReadOnly));
        const auto shellPid = shellPidFile.readAll().trimmed().toLongLong(); QVERIFY(shellPid > 0);
        if (closeWhileRunning) {
            panel.reset();
            auto shellRunning = [&] {
                QFile status(QString("/proc/%1/status").arg(shellPid));
                return status.open(QIODevice::ReadOnly) && !status.readAll().contains("State:\tZ");
            };
            QTRY_VERIFY_WITH_TIMEOUT(!shellRunning(), 5000);
            return;
        }
        QSignalSpy exited(panel.get(), &TerminalPanel::exited);
        QFile request(directory.filePath("exit-terminal")); QVERIFY(request.open(QIODevice::WriteOnly)); request.close();
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QVERIFY(!panel->available());
    }
    void weztermMouseSelectionAndPaste() {
        if (!qEnvironmentVariable("BAT_TEST_TERMINALS").split(',').contains("wezterm"))
            QSKIP("Set BAT_TEST_TERMINALS=wezterm for native mouse/clipboard integration");
        QVERIFY(ExternalTerminalPanel::platformError().isEmpty());
        X11Input input;
        QVERIFY2(input.available(), "X11 and XTEST are required for native input");
        const int doubleClickInterval = QApplication::doubleClickInterval();
        const auto restoreClickInterval = qScopeGuard([doubleClickInterval] {
            QApplication::setDoubleClickInterval(doubleClickInterval);
        });
        // Ensure rapid returns to the same BAT field exercise Qt's distinct
        // double-click event even if the foreign client handled intervening clicks.
        QApplication::setDoubleClickInterval(2000);
        QTemporaryDir directory;
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *edit = new QLineEdit(&window); layout->addWidget(edit);
        const QJsonObject settings{{"terminalBackend", "external"}, {"terminalExecutable", "wezterm"},
            {"terminalArguments", "--skip-config --config enable_wayland=false --config enable_tab_bar=false --config font_size=14 --config window_padding={left=0,right=0,top=0,bottom=0} start --always-new-process --cwd {cwd}"},
            {"terminalCommandArguments", "-- {command}"}};
        auto *panel = new TerminalPanel(directory.path(), "/bin/sh", {"-c",
            "stty -echo -icanon; printf '\033[2J\033[HSELECT_ME\\n'; touch ready; dd bs=1 count=9 of=primary.txt 2>/dev/null; dd bs=1 count=14 of=clipboard.txt 2>/dev/null; dd bs=1 count=14 of=tab-paste.txt 2>/dev/null; while [ ! -e exit-terminal ]; do sleep 0.1; done"}, &window, {}, settings);
        layout->addWidget(panel); window.resize(700, 400); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(panel->available(), 20000);
        QTRY_VERIFY(QFile::exists(directory.filePath("ready")));
        auto *container = panel->findChild<QWidget *>("externalTerminalContainer"); QVERIFY(container);
        // Start with keyboard focus in BAT, as when returning from a toolbar,
        // workspace tree or agent input. Mouse selection must transfer it back.
        edit->setFocus(); input.focus(window.winId());
        QTest::qWait(200);
        input.move(container->mapToGlobal(QPoint(5, 12)));
        input.press(1); QTest::qWait(50);
        input.move(container->mapToGlobal(QPoint(110, 12)));
        QTest::qWait(100); input.release(1);
        QTRY_COMPARE_WITH_TIMEOUT(QApplication::clipboard()->text(QClipboard::Selection).trimmed(), QString("SELECT_ME"), 3000);
        const auto terminalWindow = input.focusedWindow();
        // Returning to BAT must restore native keyboard focus there as well.
        input.move(edit->mapToGlobal(edit->rect().center())); input.press(1); input.release(1);
        QTest::qWait(100);
        input.keyEvent("a", true); input.keyEvent("a", false);
        QTRY_COMPARE(edit->text(), QString("a"));
        // Middle-click must focus and paste on the first click after leaving
        // BAT's input field; clipboard delivery itself is asynchronous.
        input.move(container->mapToGlobal(QPoint(40, 12)));
        input.press(2); input.release(2);
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(directory.filePath("primary.txt")).size(), 9, 3000);
        QFile primary(directory.filePath("primary.txt")); QVERIFY(primary.open(QIODevice::ReadOnly));
        QCOMPARE(primary.readAll(), QByteArray("SELECT_ME"));
        QApplication::clipboard()->setText("CLIPBOARD_TEXT");
        input.keyEvent("Control_L", true); input.keyEvent("Shift_L", true);
        input.keyEvent("v", true); input.keyEvent("v", false);
        input.keyEvent("Shift_L", false); input.keyEvent("Control_L", false);
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(directory.filePath("clipboard.txt")).size(), 14, 3000);
        QFile clipboard(directory.filePath("clipboard.txt")); QVERIFY(clipboard.open(QIODevice::ReadOnly));
        QCOMPARE(clipboard.readAll(), QByteArray("CLIPBOARD_TEXT"));
        QCOMPARE(edit->text(), QString("a"));
        // Keyboard navigation back into the container needs the same native
        // focus handoff as a click, without changing WezTerm's own key bindings.
        edit->clear();
        input.move(edit->mapToGlobal(edit->rect().center())); input.press(1); input.release(1);
        QTest::qWait(100);
        // Scrolling over the terminal is not a request to leave BAT's editor.
        input.move(container->mapToGlobal(QPoint(40, 12))); QTest::qWait(100);
        input.press(4); input.release(4);
        QTest::qWait(100);
        input.keyEvent("c", true); input.keyEvent("c", false);
        QTRY_COMPARE(edit->text(), QString("c"));
        input.keyEvent("Tab", true); input.keyEvent("Tab", false);
        QTRY_COMPARE(input.focusedWindow(), terminalWindow);
        input.keyEvent("Control_L", true); input.keyEvent("Shift_L", true);
        input.keyEvent("v", true); input.keyEvent("v", false);
        input.keyEvent("Shift_L", false); input.keyEvent("Control_L", false);
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(directory.filePath("tab-paste.txt")).size(), 14, 3000);
        QFile tabPaste(directory.filePath("tab-paste.txt")); QVERIFY(tabPaste.open(QIODevice::ReadOnly));
        QCOMPARE(tabPaste.readAll(), QByteArray("CLIPBOARD_TEXT"));
        // A click behind an application-modal dialog must not focus the
        // foreign client or be replayed into its terminal content.
        QDialog dialog(&window);
        auto *dialogLayout = new QVBoxLayout(&dialog);
        auto *dialogEdit = new QLineEdit(&dialog); dialogLayout->addWidget(dialogEdit);
        dialog.setWindowModality(Qt::ApplicationModal); dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog)); input.focus(dialog.winId());
        QTRY_COMPARE(QApplication::activeModalWidget(), &dialog);
        input.move(container->mapToGlobal(QPoint(40, 12))); input.press(1); input.release(1);
        QTest::qWait(100);
        QVERIFY(input.focusedWindow() != terminalWindow);
        input.keyEvent("b", true); input.keyEvent("b", false);
        QTRY_COMPARE(dialogEdit->text(), QString("b"));
        dialog.close();
    }
    void legacyWorkspacesAppearUnderDefaultProject() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        const QJsonObject workspace{{"id", "w1"}, {"name", "Existing"}, {"folderPath", directory.path()}};
        window.applySnapshot({{"workspaces", QJsonArray{workspace}}});
        auto *tree = window.findChild<QTreeWidget *>("workspace-projects");
        QVERIFY2(tree, "Workspaces must display a Project / Workspace tree");
        QCOMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->text(1), QString("Project_00"));
        QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->child(0)->text(1), QString("Existing"));
        QCOMPARE(window.m_snapshot["workspaces"].toArray().first().toObject(), workspace);
        QVERIFY(!window.m_saveTimer.isActive());
    }
    void projectGroupsRestoreMembershipAndEmptyGroups() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        const QJsonArray projects{
            QJsonObject{{"id", "p1"}, {"name", "Frontend"}, {"workspaceIds", QJsonArray{"w2"}}, {"future", 42}},
            QJsonObject{{"id", "p2"}, {"name", "Backend"}, {"workspaceIds", QJsonArray{"w1"}}},
            QJsonObject{{"id", "p3"}, {"name", "Empty"}, {"workspaceIds", QJsonArray{}}}};
        window.applySnapshot({{"workspaces", QJsonArray{
            QJsonObject{{"id", "w1"}, {"name", "API"}, {"folderPath", directory.path()}},
            QJsonObject{{"id", "w2"}, {"name", "UI"}, {"folderPath", directory.path()}}}},
            {"qtUi", QJsonObject{{"projects", projects}, {"activeProjectId", "p3"},
                {"activeWorkspaceId", "w1"}, {"futureQtField", true}}}});
        auto *tree = window.findChild<QTreeWidget *>("workspace-projects");
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(0)->child(0)->text(1), QString("UI"));
        QCOMPARE(tree->topLevelItem(1)->child(0)->text(1), QString("API"));
        QCOMPARE(tree->topLevelItem(2)->childCount(), 0);
        QCOMPARE(tree->currentItem(), tree->topLevelItem(2));
        QCOMPARE(window.m_workspaceId, QString("w1"));
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        QCOMPARE(window.m_workspaceId, QString("w2"));
        auto qt = window.m_snapshot["qtUi"].toObject();
        QVERIFY(qt["futureQtField"].toBool());
        QCOMPARE(qt["projects"].toArray().first().toObject()["future"].toInt(), 42);
        window.applySnapshot(window.m_snapshot);
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->currentItem()->text(1), QString("UI"));
    }
    void projectExpansionAndDefaultNames() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        QJsonArray projects;
        for (int i = 0; i < 100; ++i)
            projects.append(QJsonObject{{"id", QString::number(i)},
                {"name", QString("Project_%1").arg(i, 2, 10, QLatin1Char('0'))},
                {"workspaceIds", i == 0 ? QJsonArray{"w1"} : QJsonArray{}}});
        window.applySnapshot({{"workspaces", QJsonArray{
            QJsonObject{{"id", "w1"}, {"name", "One"}, {"folderPath", directory.path()}}}},
            {"qtUi", QJsonObject{{"projects", projects}, {"future", true}}}});
        QVERIFY(window.defaultProjectName().isEmpty());
        projects.removeAt(7);
        auto snapshot = window.m_snapshot; auto qt = snapshot["qtUi"].toObject();
        qt["projects"] = projects; snapshot["qtUi"] = qt; window.applySnapshot(snapshot);
        QCOMPARE(window.defaultProjectName(), QString("Project_07"));
        auto *tree = window.m_workspaces;
        auto *group = tree->topLevelItem(0);
        auto *toggle = qobject_cast<QToolButton *>(tree->itemWidget(group, 0));
        QVERIFY(toggle);
        QCOMPARE(toggle->text(), QString::fromUtf8("−"));
        toggle->click();
        QVERIFY(!group->isExpanded());
        QCOMPARE(toggle->text(), QString("+"));
        const auto collapsed = window.m_snapshot;
        window.applySnapshot(collapsed);
        group = tree->topLevelItem(0);
        QVERIFY(!group->isExpanded());
        toggle = qobject_cast<QToolButton *>(tree->itemWidget(group, 0));
        toggle->click();
        QVERIFY(group->isExpanded());
        QCOMPARE(toggle->text(), QString::fromUtf8("−"));
        QVERIFY(window.m_snapshot["qtUi"].toObject()["future"].toBool());
    }
    void invalidProjectMembershipDoesNotHideOrDuplicateWorkspaces() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.applySnapshot({{"workspaces", QJsonArray{
            QJsonObject{{"id", "w1"}, {"name", "One"}}, QJsonObject{{"id", "w2"}, {"name", "Two"}}}},
            {"qtUi", QJsonObject{{"projects", QJsonArray{
                QJsonObject{{"id", "p1"}, {"name", "Custom"}, {"workspaceIds", QJsonArray{"w1", "w1", "deleted"}}},
                QJsonObject{{"id", "p2"}, {"name", "Other"}, {"workspaceIds", QJsonArray{"w1"}}}}}}}});
        auto *tree = window.m_workspaces;
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(1)->childCount(), 0);
        QCOMPARE(tree->topLevelItem(2)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(2)->child(0)->text(1), QString("Two"));
    }
    void snapshotDoesNotScheduleAnotherSave() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        const QJsonObject snapshot{{"workspaces", QJsonArray{
            QJsonObject{{"id", "workspace-1"}, {"name", "Test"}, {"folderPath", directory.path()}}}},
            {"qtUi", QJsonObject{{"activeWorkspaceId", "workspace-1"}, {"tabs", QJsonArray{}}}}};
        window.applySnapshot(snapshot);
        QVERIFY(!window.m_saveTimer.isActive());
        const auto generation = window.m_filesGeneration;
        window.applySnapshot(snapshot);
        QVERIFY(!window.m_saveTimer.isActive());
        QCOMPARE(window.m_filesGeneration, generation);
        window.showWorkspace("workspace-1");
        QVERIFY(!window.m_saveTimer.isActive());
    }
    void legacyWorkspaceReloadKeepsLiveKonsole() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        const QJsonObject workspace{{"id", "workspace-1"}, {"name", "Test"}, {"folderPath", directory.path()}};
        const QJsonObject descriptor{{"id", "terminal-1"}, {"workspaceId", "workspace-1"},
            {"kind", "terminal"}, {"cwd", directory.path()}};
        const QJsonObject qt{{"activeWorkspaceId", "workspace-1"}, {"tabs", QJsonArray{descriptor}}};
        const QJsonObject snapshot{{"workspaces", QJsonArray{workspace}}, {"qtUi", qt}};
        window.applySnapshot(snapshot);
        auto *tabs = window.m_workspaceTabs.value("workspace-1");
        QPointer<TerminalPanel> panel = qobject_cast<TerminalPanel *>(tabs->widget(0));
        QVERIFY(panel); QVERIFY(panel->available());
        auto *terminal = qobject_cast<TerminalInterface *>(panel->findChild<KParts::ReadOnlyPart *>());
        QVERIFY(terminal);
        QTRY_VERIFY_WITH_TIMEOUT(terminal->terminalProcessId() > 0, 5000);
        const auto pid = terminal->terminalProcessId();
        // Legacy snapshots have no authority over the local Qt session list.
        window.applySnapshot({{"workspaces", QJsonArray{workspace}}});
        QVERIFY(panel);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->widget(0), panel.data());
        QCOMPARE(terminal->terminalProcessId(), pid);
        panel->sendInput("printf alive > reload-alive.txt\n");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(directory.filePath("reload-alive.txt")), 5000);
        window.applySnapshot(snapshot);
        QCOMPARE(tabs->widget(0), panel.data());
        // An explicit empty Qt tab list still closes the saved terminal.
        window.applySnapshot({{"workspaces", QJsonArray{workspace}},
            {"qtUi", QJsonObject{{"tabs", QJsonArray{}}}}});
        QVERIFY(!panel);
        QCOMPARE(tabs->count(), 0);
    }
    void validatesPinnedUrls() {
        const QString fingerprint(64, 'a');
        QVERIFY(HostClient::validateConnectionUrl(QUrl("wss://localhost:9876/?token=test&fp=" + QString("AA%3A").repeated(31) + "AA")).isEmpty());
        QVERIFY(HostClient::validateConnectionUrl(QUrl("wss://localhost:9876?token=secret&fp=" + fingerprint)).isEmpty());
        QVERIFY(!HostClient::validateConnectionUrl(QUrl("ws://localhost?token=secret&fp=" + fingerprint)).isEmpty());
        QVERIFY(!HostClient::validateConnectionUrl(QUrl("wss://localhost?fp=" + fingerprint)).isEmpty());
        QVERIFY(!HostClient::validateConnectionUrl(QUrl("wss://localhost?token=secret&fp=bad")).isEmpty());
        QVERIFY(!HostClient::validateConnectionUrl(QUrl("wss://user:password@localhost?token=secret&fp=" + fingerprint)).isEmpty());
        QCOMPARE(HostClient::normalizedFingerprint(QString("AA:").repeated(31) + "AA"), QByteArray(64, 'a'));
        QVERIFY(HostClient::normalizedFingerprint(QString(64, 'z')).isEmpty());
        QVERIFY(!HostClient::certificateMatches({}, QByteArray(64, 'a')));
    }
    void streamIsReplacedByFinalMessage() {
        Transcript transcript;
        transcript.message({{"id", "u1"}, {"role", "user"}, {"content", "Hello"}});
        transcript.stream({{"text", "Good "}});
        transcript.stream({{"text", "morning"}});
        QVERIFY(transcript.markdown().contains("Good morning"));
        transcript.message({{"id", "a1"}, {"role", "assistant"}, {"content", "Good morning"}});
        transcript.message({{"id", "a1"}, {"role", "assistant"}, {"content", "Good morning"}});
        transcript.finishTurn();
        QCOMPARE(transcript.markdown().count("Good morning"), 1);
        QCOMPARE(transcript.markdown().count("### You"), 1);
    }
    void toolAndSubagentEventsDoNotEraseMainStream() {
        Transcript transcript;
        transcript.stream({{"text", "Main answer"}});
        transcript.stream({{"text", "Subagent"}, {"parentToolUseId", "task-1"}});
        transcript.message({{"id", "tool-1"}, {"role", "assistant"},
            {"content", QJsonArray{QJsonObject{{"type", "tool_use"}, {"id", "t1"}}}}});
        QVERIFY(transcript.markdown().contains("Main answer"));
        QVERIFY(!transcript.markdown().contains("Subagent"));
        transcript.finishTurn();
        QCOMPARE(transcript.markdown().count("Main answer"), 1);
    }
    void terminalTeardownDoesNotEmitProcessExit() {
        QTemporaryDir directory;
        for (bool autoClose : {false, true}) {
            auto *window = new MainWindow(directory.path());
            window->m_settings.insert("closeTerminalAfterProcessExit", autoClose);
            auto *tabs = new QTabWidget(window->m_pages);
            window->m_pages->addWidget(tabs);
            window->m_workspaceTabs.insert("workspace", tabs);
            window->createTab({{"id", "terminal"}, {"workspaceId", "workspace"},
                {"kind", "terminal"}, {"cwd", directory.path()}}, false);
            auto *terminal = qobject_cast<TerminalPanel *>(tabs->widget(0));
            QVERIFY(terminal); QVERIFY(terminal->available());
            int exits = 0;
            connect(terminal, &TerminalPanel::exited, this, [&exits] { ++exits; });
            // Exercise workspace removal while the window is alive, then app
            // teardown with another live shell and the real exit callback.
            window->clearPages();
            QCOMPARE(exits, 0);
            tabs = new QTabWidget(window->m_pages);
            window->m_pages->addWidget(tabs);
            window->m_workspaceTabs.insert("workspace", tabs);
            window->createTab({{"id", "terminal"}, {"workspaceId", "workspace"},
                {"kind", "terminal"}, {"cwd", directory.path()}}, false);
            terminal = qobject_cast<TerminalPanel *>(tabs->widget(0));
            QVERIFY(terminal); QVERIFY(terminal->available());
            connect(terminal, &TerminalPanel::exited, this, [&exits] { ++exits; });
            delete window;
            QCOMPARE(exits, 0);
        }
    }
    void konsoleShellAndProgram() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TerminalPanel panel(directory.path());
        QVERIFY(panel.available());
        auto *part = panel.findChild<KParts::ReadOnlyPart *>();
        QVERIFY(part);
        auto *terminal = qobject_cast<TerminalInterface *>(part);
        QVERIFY(terminal);
        QTRY_VERIFY_WITH_TIMEOUT(terminal->terminalProcessId() > 0, 5000);
        panel.sendInput("pwd > bat-qt-cwd.txt\n");
        QFile output(directory.filePath("bat-qt-cwd.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(output.exists(), 5000);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(output.readAll()).trimmed(), directory.path());
        QSignalSpy exited(&panel, &TerminalPanel::exited);
        panel.sendInput("exit\n");
        QTRY_COMPARE_WITH_TIMEOUT(exited.count(), 1, 5000);
        QVERIFY(!panel.available());
        TerminalPanel program(directory.path(), "/bin/sh", {"-c", "pwd > program-cwd.txt"});
        QVERIFY(program.available());
        QFile programOutput(directory.filePath("program-cwd.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(programOutput.exists(), 5000);
        QVERIFY(programOutput.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(programOutput.readAll()).trimmed(), directory.path());
    }
    void tlsProtocolAndLifecycle_data() {
        QTest::addColumn<bool>("thirdPartyTerminal");
        QTest::addColumn<QString>("thirdPartyExecutable");
        QTest::newRow("konsole") << false << QString();
        QTest::newRow("third-party-default") << true << QString();
        for (const auto &name : qEnvironmentVariable("BAT_TEST_TERMINALS").split(',', Qt::SkipEmptyParts))
            QTest::newRow(qPrintable("third-party-default-" + name)) << true << name;
    }
    void tlsProtocolAndLifecycle() {
        QFETCH(bool, thirdPartyTerminal);
        QFETCH(QString, thirdPartyExecutable);
        if (thirdPartyTerminal && !ExternalTerminalPanel::platformError().isEmpty())
            QSKIP("Third-party project removal requires X11; run under Xvfb with QT_QPA_PLATFORM=xcb");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString certPath = directory.path() + "/cert.pem";
        const QString keyPath = directory.path() + "/key.pem";
        QProcess openssl;
        openssl.start("openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-keyout", keyPath, "-out", certPath});
        QVERIFY(openssl.waitForFinished(15000)); QCOMPARE(openssl.exitCode(), 0);
        QFile certFile(certPath), keyFile(keyPath);
        QVERIFY(certFile.open(QIODevice::ReadOnly)); QVERIFY(keyFile.open(QIODevice::ReadOnly));
        const QSslCertificate certificate(certFile.readAll());
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(certificate); ssl.setPrivateKey(QSslKey(keyFile.readAll(), QSsl::Rsa));
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        QWebSocketServer server("BAT test", QWebSocketServer::SecureMode);
        server.setSslConfiguration(ssl);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));
        QPointer<QWebSocket> peer;
        QString requestPath;
        QString seenContext;
        QJsonObject savedSettings;
        QJsonObject savedWorkspace;
        QStringList stoppedSessions;
        bool delayWorkspaceSaves = false;
        bool rejectNextWorkspaceSave = false;
        int workspaceLoads = 0;
        QList<QJsonObject> pendingWorkspaceReplies;
        bool rejectSettingsSave = true;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto *connection = server.nextPendingConnection(); connection->setParent(&server); peer = connection;
            requestPath = peer->requestUrl().toString();
            connect(connection, &QWebSocket::textMessageReceived, this, [&, connection](const QString &text) {
                const auto frame = QJsonDocument::fromJson(text.toUtf8()).object();
                QJsonObject response;
                if (frame["type"] == "auth") {
                    QCOMPARE(frame["token"].toString(), QString("test-token"));
                    response = {{"type", "auth-result"}, {"id", frame["id"]}, {"result", true},
                        {"protocol", "bat-remote/v2"}, {"compression", "none"}};
                } else if (frame["channel"] == "test:echo") {
                    seenContext = frame["contextId"].toString();
                    response = {{"type", "invoke-result"}, {"id", frame["id"]}, {"result", frame["params"]}};
                } else if (frame["channel"] == "settings:load") {
                    response = {{"type", "invoke-result"}, {"id", frame["id"]},
                        {"result", "{\"defaultClaudeModel\":\"original\",\"preserved\":true}"}};
                } else if (frame["channel"] == "settings:save") {
                    if (rejectSettingsSave) response = {{"type", "invoke-error"}, {"id", frame["id"]}, {"error", "Save failed"}};
                    else {
                        savedSettings = QJsonDocument::fromJson(frame["params"].toObject()["data"].toString().toUtf8()).object();
                        response = {{"type", "invoke-result"}, {"id", frame["id"]}, {"result", true}};
                    }
                } else if (frame["channel"] == "workspace:save") {
                    if (rejectNextWorkspaceSave) {
                        rejectNextWorkspaceSave = false;
                        response = {{"type", "invoke-error"}, {"id", frame["id"]}, {"error", "Save failed"}};
                        connection->sendTextMessage(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)));
                        return;
                    }
                    savedWorkspace = QJsonDocument::fromJson(frame["params"].toObject()["data"].toString().toUtf8()).object();
                    response = {{"type", "invoke-result"}, {"id", frame["id"]}, {"result", true}};
                    const QJsonObject echo{{"type", "event"}, {"contextId", frame["contextId"]},
                        {"channel", "workspace:reload"}, {"params", QJsonObject{{"data", frame["params"].toObject()["data"]}}}};
                    if (delayWorkspaceSaves) {
                        pendingWorkspaceReplies.append(QJsonObject{{"echo", echo}, {"reply", response}});
                        return;
                    }
                    connection->sendTextMessage(QString::fromUtf8(QJsonDocument(echo).toJson(QJsonDocument::Compact)));
                } else if (frame["channel"] == "claude:get-session-state") {
                    response = {{"type", "invoke-result"}, {"id", frame["id"]},
                        {"result", QJsonObject{{"isStreaming", true}, {"messages", QJsonArray{}}}}};
                } else if (frame["channel"] == "claude:stop-session") {
                    stoppedSessions.append(frame["params"].toObject()["sessionId"].toString());
                    response = {{"type", "invoke-result"}, {"id", frame["id"]}, {"result", true}};
                } else if (frame["channel"] == "fs:isDirectory") {
                    response = {{"type", "invoke-result"}, {"id", frame["id"]}, {"result", true}};
                } else if (frame["channel"] == "workspace:load") {
                    ++workspaceLoads;
                    response = {{"type", "invoke-result"}, {"id", frame["id"]},
                        {"result", QString::fromUtf8(QJsonDocument(savedWorkspace).toJson(QJsonDocument::Compact))}};
                } else if (frame["channel"].toString().startsWith("git:")) {
                    const auto channel = frame["channel"].toString();
                    QJsonValue result;
                    if (channel == "git:getRoot") result = "/repo";
                    else if (channel == "git:status") result = QJsonArray{
                        QJsonObject{{"status", "M"}, {"file", "edited.cpp"}},
                        QJsonObject{{"status", "??"}, {"file", "new file.txt"}}};
                    else if (channel == "git:log") result = QJsonArray{QJsonObject{
                        {"hash", QString(40, 'a')}, {"message", "Test commit"}, {"author", "Author"}, {"date", "2026-10-03T12:00:00Z"}}};
                    else if (channel == "git:diff-files") result = QJsonArray{QJsonObject{{"status", "M"}, {"file", "committed.cpp"}}};
                    else if (channel == "git:diff") result = frame["params"].toObject()["filePath"] == "new file.txt"
                        ? QString() : QString("diff --git a/file b/file\n--- a/file\n+++ b/file\n@@ -1 +1 @@\n-old\n+new");
                    response = {{"type", "invoke-result"}, {"id", frame["id"]}, {"result", result}};
                } else if (frame["channel"] == "fs:readFile") {
                    response = {{"type", "invoke-result"}, {"id", frame["id"]}, {"result", QJsonObject{{"content", "new content\n"}}}};
                } else return; // Exercise timeouts / abandoned callers.
                connection->sendTextMessage(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)));
            });
        });
        QUrl url(QString("wss://localhost:%1").arg(server.serverPort()));
        QUrlQuery query; query.addQueryItem("token", "test-token");
        query.addQueryItem("fp", QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex()));
        url.setQuery(query);
        HostClient client;
        QSignalSpy connected(&client, &HostClient::connected), events(&client, &HostClient::eventReceived);
        client.connectTo(url);
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 10000);
        QVERIFY(!requestPath.contains("test-token"));
        client.setContextId("context-1");
        bool echoed = false;
        client.invoke("test:echo", {{"text", "hello"}}, this, [&](const QJsonValue &value, const QString &error) {
            QVERIFY(error.isEmpty()); QCOMPARE(value.toObject()["text"].toString(), QString("hello")); echoed = true;
        });
        QTRY_VERIFY(echoed); QCOMPARE(seenContext, QString("context-1"));
        SettingsDialog settingsDialog(&client, {});
        auto *saveButton = settingsDialog.findChild<QPushButton *>("saveSettings");
        QTRY_VERIFY(saveButton->isEnabled());
        QCOMPARE(settingsDialog.findChild<QLineEdit *>("defaultClaudeModel")->text(), QString("original"));
        settingsDialog.findChild<QLineEdit *>("defaultClaudeModel")->setText("changed");
        QSignalSpy settingsSaved(&settingsDialog, &SettingsDialog::settingsSaved);
        saveButton->click();
        QTRY_VERIFY(saveButton->isEnabled());
        QCOMPARE(settingsSaved.count(), 0);
        QCOMPARE(settingsDialog.findChild<QLineEdit *>("defaultClaudeModel")->text(), QString("changed"));
        rejectSettingsSave = false;
        saveButton->click();
        QTRY_COMPARE(settingsSaved.count(), 1);
        QCOMPARE(savedSettings["defaultClaudeModel"].toString(), QString("changed"));
        QVERIFY(savedSettings["preserved"].toBool());
        // Exercise the real dialogs, controls and workspace:save contract.
        const auto originalPeer = peer;
        MainWindow window(directory.path());
        QObject::disconnect(&window.m_host, &HostClient::connected, &window, nullptr);
        window.m_host.connectTo(url);
        QTRY_VERIFY(window.m_host.ready());
        window.m_host.setContextId("context-1");
        window.m_localProfile = false;
        window.applySnapshot({{"workspaces", QJsonArray{}}, {"qtUi", QJsonObject{{"futureOption", 42}}}});
        QString offeredName;
        const auto answerName = [&](const QString &name, bool accept = true) {
            QTimer::singleShot(0, &window, [&, name, accept] {
                auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                offeredName = dialog->textValue();
                if (accept) { dialog->setTextValue(name); dialog->accept(); }
                else dialog->reject();
            });
        };
        auto *addProject = window.findChild<QToolButton *>("add-project");
        QVERIFY(addProject);
        answerName("Project_00"); addProject->click();
        QCOMPARE(offeredName, QString("Project_00"));
        QTRY_COMPARE(window.m_workspaces->topLevelItemCount(), 1);
        QCOMPARE(savedWorkspace["qtUi"].toObject()["projects"].toArray().size(), 1);
        answerName({}, false); addProject->click();
        QCOMPARE(offeredName, QString("Project_01"));
        QCOMPARE(window.m_workspaces->topLevelItemCount(), 1);
        answerName(" Backend "); addProject->click();
        QTRY_COMPARE(window.m_workspaces->topLevelItemCount(), 2);
        QCOMPARE(window.m_workspaces->topLevelItem(1)->text(1), QString("Backend"));
        answerName("Services"); window.renameProject();
        QTRY_COMPARE(window.m_workspaces->topLevelItem(1)->text(1), QString("Services"));
        answerName("project_00"); addProject->click();
        QCOMPARE(window.m_workspaces->topLevelItemCount(), 2);
        QCOMPARE(window.m_snapshot["qtUi"].toObject()["projects"].toArray().size(), 2);
        auto *addWorkspace = qobject_cast<QToolButton *>(window.m_workspaces->itemWidget(
            window.m_workspaces->topLevelItem(1), 2));
        QVERIFY(addWorkspace);
        answerName(directory.path()); addWorkspace->click();
        // Switching selection during validation must not change the target project.
        window.m_workspaces->setCurrentItem(window.m_workspaces->topLevelItem(0));
        QTRY_COMPARE(window.m_workspaces->topLevelItem(1)->childCount(), 1);
        QCOMPARE(window.m_workspaces->topLevelItem(0)->childCount(), 0);
        QCOMPARE(savedWorkspace["qtUi"].toObject()["futureOption"].toInt(), 42);
        auto *toggle = qobject_cast<QToolButton *>(window.m_workspaces->itemWidget(
            window.m_workspaces->topLevelItem(1), 0));
        QVERIFY(toggle); toggle->click();
        QTRY_VERIFY(savedWorkspace["qtUi"].toObject()["projects"].toArray()[1].toObject()["collapsed"].toBool());
        MainWindow reopened(directory.path());
        reopened.applySnapshot(savedWorkspace);
        QCOMPARE(reopened.m_workspaces->topLevelItemCount(), 2);
        QCOMPARE(reopened.m_workspaces->topLevelItem(1)->text(1), QString("Services"));
        QCOMPARE(reopened.m_workspaces->topLevelItem(1)->childCount(), 1);
        QVERIFY(!reopened.m_workspaces->topLevelItem(1)->isExpanded());
        const auto releaseWorkspaceReply = [&] {
            const auto pending = pendingWorkspaceReplies.takeFirst();
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(pending["echo"].toObject()).toJson(QJsonDocument::Compact)));
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(pending["reply"].toObject()).toJson(QJsonDocument::Compact)));
        };
        // A second click less than 250 ms before the reply must survive both
        // the original save's reload echo and its completion callback.
        delayWorkspaceSaves = true;
        auto *firstToggle = qobject_cast<QToolButton *>(window.m_workspaces->itemWidget(
            window.m_workspaces->topLevelItem(0), 0));
        QVERIFY(firstToggle); firstToggle->click();
        QTRY_COMPARE(pendingWorkspaceReplies.size(), 1);
        toggle = qobject_cast<QToolButton *>(window.m_workspaces->itemWidget(window.m_workspaces->topLevelItem(1), 0));
        toggle->click();
        releaseWorkspaceReply();
        QTRY_COMPARE(pendingWorkspaceReplies.size(), 1);
        QVERIFY(window.m_workspaces->topLevelItem(1)->isExpanded());
        delayWorkspaceSaves = false; releaseWorkspaceReply();
        QTRY_VERIFY(!window.m_saveInFlight);
        QVERIFY(window.m_workspaces->topLevelItem(1)->isExpanded());
        QVERIFY(!savedWorkspace["qtUi"].toObject()["projects"].toArray()[1].toObject()["collapsed"].toBool());
        // A terminal opened while a new workspace is being saved must retain
        // its process and enter the next proposal, rather than be closed by
        // the earlier proposal's Qt tab list.
        delayWorkspaceSaves = true;
        const QString existingWorkspace = window.m_workspaceId;
        answerName(directory.path()); window.addWorkspace();
        QTRY_COMPARE(pendingWorkspaceReplies.size(), 1);
        window.m_localProfile = true;
        window.createTab({{"id", "during-workspace-save"}, {"workspaceId", existingWorkspace},
            {"kind", "terminal"}, {"cwd", directory.path()}});
        auto *tabs = window.m_workspaceTabs.value(existingWorkspace);
        QPointer<TerminalPanel> terminal = qobject_cast<TerminalPanel *>(tabs->widget(0));
        QVERIFY(terminal); QVERIFY(terminal->available());
        releaseWorkspaceReply();
        QTRY_COMPARE(pendingWorkspaceReplies.size(), 1);
        QVERIFY(terminal);
        QCOMPARE(savedWorkspace["qtUi"].toObject()["tabs"].toArray().size(), 1);
        window.m_localProfile = false; // Avoid adding default local tabs in this protocol test.
        delayWorkspaceSaves = false; releaseWorkspaceReply();
        QTRY_VERIFY(!window.m_saveInFlight);
        QVERIFY(terminal);
        QCOMPARE(window.m_workspaceTabs.value(existingWorkspace)->widget(0), terminal.data());
        QVERIFY(!window.m_saveAgain);
        window.m_localProfile = true;
        window.createTab({{"id", "unsaved-after-error"}, {"workspaceId", existingWorkspace},
            {"kind", "terminal"}, {"cwd", directory.path()}});
        QPointer<TerminalPanel> unsaved = qobject_cast<TerminalPanel *>(tabs->widget(1));
        QVERIFY(unsaved); QVERIFY(unsaved->available());
        rejectNextWorkspaceSave = true;
        window.saveSnapshot();
        QTRY_COMPARE(workspaceLoads, 1);
        QTRY_COMPARE(window.m_snapshot["qtUi"].toObject()["tabs"].toArray().size(), 1);
        QVERIFY(terminal); QVERIFY(unsaved);
        QCOMPARE(tabs->count(), 2);
        unsaved->sendInput("printf survived > failed-save-alive.txt\n");
        QTRY_VERIFY(QFile::exists(directory.filePath("failed-save-alive.txt")));
        // Project removal must be reachable from the real context menu,
        // confirm before deleting, and leave other projects and files alone.
        QTRY_VERIFY(!window.m_saveInFlight);
        auto removalSnapshot = window.m_snapshot;
        auto removalQt = removalSnapshot["qtUi"].toObject();
        const auto removalWorkspaces = removalSnapshot["workspaces"].toArray();
        QCOMPARE(removalWorkspaces.size(), 2);
        QString otherWorkspace;
        for (const auto &entry : removalWorkspaces)
            if (entry.toObject()["id"] != existingWorkspace) otherWorkspace = entry.toObject()["id"].toString();
        QVERIFY(!otherWorkspace.isEmpty());
        removalQt["projects"] = QJsonArray{
            QJsonObject{{"id", "remove-me"}, {"name", "Remove me"}, {"workspaceIds", QJsonArray{existingWorkspace}}},
            QJsonObject{{"id", "keep-me"}, {"name", "Keep me"}, {"workspaceIds", QJsonArray{otherWorkspace}}, {"future", 42}},
            QJsonObject{{"id", "empty"}, {"name", "Empty"}, {"workspaceIds", QJsonArray{}}}};
        removalQt["activeProjectId"] = "remove-me";
        removalSnapshot["qtUi"] = removalQt;
        window.applySnapshot(removalSnapshot, true);
        window.m_saveTimer.stop();
        auto *otherTabs = window.m_workspaceTabs.value(otherWorkspace);
        QPointer<QWidget> otherPanel = new QWidget(otherTabs);
        otherPanel->setProperty("tabId", "keep-tab");
        otherPanel->setProperty("descriptor", QVariantMap{{"id", "keep-tab"}, {"workspaceId", otherWorkspace}});
        otherTabs->addTab(otherPanel, "Keep tab");
        window.createTab({{"id", "remove-agent"}, {"workspaceId", existingWorkspace}, {"kind", "agent"},
            {"cwd", directory.path()}, {"agentPreset", "claude-code"}}, false);
        QPointer<AgentPanel> removedAgent = qobject_cast<AgentPanel *>(tabs->widget(2));
        QVERIFY(removedAgent);
        QTRY_VERIFY(removedAgent->busy());
        window.show();
        QPointer<TerminalPanel> externalTerminal;
        qint64 externalPid = 0;
        if (thirdPartyTerminal) {
            // Change the default, then open a tab through the normal UI path.
            // Existing Konsole tabs must coexist with the third-party terminal.
            auto settings = window.m_settings;
            settings["qtUi"] = QJsonObject{{"terminalBackend", "external"},
                {"terminalExecutable", "/usr/bin/env"},
                {"terminalArguments", QString("\"LD_LIBRARY_PATH=%1\" \"QT_PLUGIN_PATH=%2\" \"%3\" --terminal-window-probe {cwd}")
                    .arg(qEnvironmentVariable("LD_LIBRARY_PATH"), qEnvironmentVariable("QT_PLUGIN_PATH"), QCoreApplication::applicationFilePath())},
                {"terminalCommandArguments", "-- {command}"}};
            if (!thirdPartyExecutable.isEmpty()) {
                auto terminalSettings = settings["qtUi"].toObject();
                terminalSettings["terminalExecutable"] = thirdPartyExecutable;
                terminalSettings["terminalArguments"] = (thirdPartyExecutable == "wezterm"
                    ? QString("--config enable_wayland=false start --always-new-process --cwd {cwd} --")
                    : QString("--gtk-single-instance=false --working-directory={cwd} -e"))
                    + " /bin/sh -c \"echo $$ > project-terminal-pid.txt; while [ ! -e exit-terminal ]; do sleep 0.1; done\"";
                settings["qtUi"] = terminalSettings;
            }
            window.applySettings(settings);
            window.showWorkspace(existingWorkspace, false);
            window.addTab("none");
            externalTerminal = qobject_cast<TerminalPanel *>(tabs->widget(3));
            QVERIFY(externalTerminal);
            QTRY_VERIFY_WITH_TIMEOUT(externalTerminal->available(), 20000);
            QFile probe(directory.filePath(thirdPartyExecutable.isEmpty() ? "terminal-probe.json" : "project-terminal-pid.txt"));
            QTRY_VERIFY_WITH_TIMEOUT(probe.exists(), 5000);
            QVERIFY(probe.open(QIODevice::ReadOnly));
            const auto output = probe.readAll();
            externalPid = thirdPartyExecutable.isEmpty()
                ? QJsonDocument::fromJson(output).object()["pid"].toInteger() : output.trimmed().toLongLong();
            QVERIFY(externalPid > 0);
            QTRY_VERIFY(!window.m_saveInFlight);
        }
        const auto removeFromMenu = [&](int index, QMessageBox::StandardButton answer,
                                        bool saveDuringConfirmation = false, bool saveBeforeAction = false) {
            bool found = false;
            bool confirmed = false;
            bool savePendingAtConfirmation = false;
            QTimer answerTimer;
            answerTimer.setSingleShot(true);
            connect(&answerTimer, &QTimer::timeout, &window, [&] {
                savePendingAtConfirmation = window.m_saveInFlight && !pendingWorkspaceReplies.isEmpty();
                auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (dialog) { confirmed = true; dialog->button(answer)->click(); }
            });
            auto *group = window.m_workspaces->topLevelItem(index);
            window.m_workspaces->scrollToItem(group);
            QTimer::singleShot(0, &window, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                QAction *remove = nullptr;
                for (auto *action : menu->actions())
                    if (action->text() == "Remove project") remove = action;
                menu->close();
                if (!remove) return;
                found = true;
                if (saveBeforeAction) window.saveSnapshot();
                if (saveDuringConfirmation) window.scheduleSnapshotSave();
                answerTimer.start(saveDuringConfirmation ? 350 : 0);
                remove->trigger();
            });
            window.m_workspaces->customContextMenuRequested(window.m_workspaces->visualItemRect(group).center());
            return found && confirmed && (!saveDuringConfirmation || savePendingAtConfirmation);
        };
        QVERIFY2(removeFromMenu(0, QMessageBox::No), "Project context menu must offer Remove project");
        delayWorkspaceSaves = true;
        QVERIFY2(removeFromMenu(0, QMessageBox::No, false, true),
                 "Remove project must open confirmation even while a selection save is pending");
        QTRY_COMPARE(pendingWorkspaceReplies.size(), 1);
        delayWorkspaceSaves = false;
        releaseWorkspaceReply();
        QTRY_VERIFY(!window.m_saveInFlight);
        QCOMPARE(window.m_snapshot["workspaces"].toArray(), removalWorkspaces);
        QCOMPARE(window.m_workspaces->topLevelItemCount(), 3);
        QVERIFY(terminal); QVERIFY(unsaved);
        QVERIFY(removedAgent); QVERIFY(stoppedSessions.isEmpty());
        if (thirdPartyTerminal) QVERIFY(externalTerminal && externalTerminal->available());
        // A human can leave the menu/confirmation open beyond the autosave
        // debounce. The confirmation must still remove the selected project,
        // and the older save's reply must not resurrect its tabs or workspaces.
        delayWorkspaceSaves = true;
        QVERIFY(removeFromMenu(0, QMessageBox::Yes, true));
        QCOMPARE(window.m_workspaces->topLevelItemCount(), 2);
        QVERIFY(window.m_saveInFlight);
        QCOMPARE(pendingWorkspaceReplies.size(), 1);
        releaseWorkspaceReply();
        QTRY_COMPARE(pendingWorkspaceReplies.size(), 1);
        delayWorkspaceSaves = false;
        releaseWorkspaceReply();
        QTRY_VERIFY(!window.m_saveInFlight);
        QTRY_COMPARE(window.m_workspaces->topLevelItemCount(), 2);
        QVERIFY(!terminal); QVERIFY(!unsaved);
        QVERIFY(!removedAgent);
        if (thirdPartyTerminal) {
            QVERIFY(!externalTerminal);
            const auto processRunning = [&] {
                QFile status(QString("/proc/%1/status").arg(externalPid));
                return status.open(QIODevice::ReadOnly) && !status.readAll().contains("State:\tZ");
            };
            QTRY_VERIFY(!processRunning());
        }
        QTRY_COMPARE(stoppedSessions, QStringList{"remove-agent"});
        QVERIFY(otherPanel);
        QVERIFY(!window.m_workspaceTabs.contains(existingWorkspace));
        QCOMPARE(window.m_workspaceTabs.value(otherWorkspace), otherTabs);
        QCOMPARE(window.m_workspaceId, otherWorkspace);
        QCOMPARE(window.m_projectId, QString("keep-me"));
        QCOMPARE(savedWorkspace["workspaces"].toArray().size(), 1);
        QCOMPARE(savedWorkspace["workspaces"].toArray().first().toObject()["id"].toString(), otherWorkspace);
        QCOMPARE(savedWorkspace["qtUi"].toObject()["projects"].toArray().first().toObject()["future"].toInt(), 42);
        QCOMPARE(savedWorkspace["qtUi"].toObject()["tabs"].toArray().size(), 1);
        QVERIFY(QFile::exists(directory.filePath("failed-save-alive.txt")));
        reopened.applySnapshot(savedWorkspace);
        QCOMPARE(reopened.m_workspaces->topLevelItemCount(), 2);
        QCOMPARE(reopened.m_workspaces->topLevelItem(0)->childCount(), 1);
        QVERIFY(removeFromMenu(1, QMessageBox::Yes)); // Empty groups are removable too.
        QTRY_VERIFY(!window.m_saveInFlight);
        QTRY_COMPARE(window.m_workspaces->topLevelItemCount(), 1);
        QVERIFY(otherPanel);
        QVERIFY(removeFromMenu(0, QMessageBox::Yes)); // Removing the last group stays empty on reload.
        QTRY_VERIFY(!window.m_saveInFlight);
        QTRY_COMPARE(window.m_workspaces->topLevelItemCount(), 0);
        QVERIFY(!otherPanel);
        QVERIFY(savedWorkspace["workspaces"].toArray().isEmpty());
        QVERIFY(savedWorkspace["qtUi"].toObject()["projects"].toArray().isEmpty());
        QVERIFY(savedWorkspace["qtUi"].toObject()["activeProjectId"].toString().isEmpty());
        reopened.applySnapshot(savedWorkspace);
        QCOMPARE(reopened.m_workspaces->topLevelItemCount(), 0);
        window.m_host.disconnectFromHost();
        peer = originalPeer;
        GitPanel gitPanel(&client);
        gitPanel.setWorkspace("/repo/subfolder"); gitPanel.refresh();
        auto *commits = gitPanel.findChild<QListWidget *>("git-commits");
        auto *files = gitPanel.findChild<QTreeWidget *>("git-files");
        auto *diff = gitPanel.findChild<QPlainTextEdit *>("git-diff");
        auto *fileContent = gitPanel.findChild<QPlainTextEdit *>("git-file-content");
        QTRY_COMPARE(commits->count(), 2);
        QTRY_COMPARE(files->topLevelItemCount(), 2);
        QTRY_VERIFY(diff->toPlainText().contains("+new"));
        QTRY_VERIFY(fileContent->toPlainText().contains("new content"));
        files->setCurrentItem(files->topLevelItem(1));
        QTRY_VERIFY(diff->toPlainText().contains("+++ b/new file.txt"));
        QVERIFY(diff->toPlainText().contains("+new content"));
        commits->setCurrentRow(1);
        QTRY_COMPARE(files->topLevelItemCount(), 1);
        QCOMPARE(files->topLevelItem(0)->text(1), QString("committed.cpp"));
        QTRY_VERIFY(diff->toPlainText().contains("-old"));
        const auto sendEvent = [&](const QString &context) {
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "event"},
                {"contextId", context}, {"channel", "agent:stream"}, {"params", QJsonObject{{"sessionId", "s1"}}}}).toJson()));
        };
        sendEvent("foreign-context"); sendEvent("context-1");
        QTRY_COMPARE(events.count(), 1);
        QCOMPARE(events.at(0).at(0).toString(), QString("claude:stream"));
        bool timedOut = false;
        client.invoke("test:hang", {}, this, [&](const QJsonValue &, const QString &error) {
            QVERIFY(error.contains("timed out")); timedOut = true;
        }, 40);
        QTRY_VERIFY(timedOut);
        bool abandonedCalled = false;
        auto *owner = new QObject;
        client.invoke("test:hang", {}, owner, [&](const QJsonValue &, const QString &) { abandonedCalled = true; }, 40);
        delete owner;
        QTest::qWait(80); QVERIFY(!abandonedCalled);
        bool disconnectedReply = false;
        client.invoke("test:hang", {}, this, [&](const QJsonValue &, const QString &error) {
            QVERIFY(!error.isEmpty()); disconnectedReply = true;
        });
        client.disconnectFromHost(); QVERIFY(disconnectedReply); QVERIFY(!client.ready());
        // A valid TLS cert with a wrong BAT fingerprint must never authenticate.
        query.removeAllQueryItems("fp"); query.addQueryItem("fp", QString(64, 'a')); url.setQuery(query);
        HostClient wrongPin;
        QSignalSpy pinErrors(&wrongPin, &HostClient::connectionError), pinConnected(&wrongPin, &HostClient::connected);
        wrongPin.connectTo(url);
        QTRY_VERIFY_WITH_TIMEOUT(pinErrors.count() > 0, 10000);
        QCOMPARE(pinConnected.count(), 0);
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc >= 3 && QByteArray(argv[1]) == "--terminal-window-probe") {
        QWidget window; auto *layout = new QVBoxLayout(&window);
        if (app.arguments().contains("temporary-splash")) {
            auto *splash = new QWidget(nullptr, Qt::SplashScreen);
            splash->setAttribute(Qt::WA_DeleteOnClose); splash->resize(100, 100); splash->show();
            QTimer::singleShot(500, splash, &QWidget::close);
        }
        if (app.arguments().contains("extra-window")) {
            auto *extra = new QWidget; extra->setAttribute(Qt::WA_DeleteOnClose);
            QTimer::singleShot(500, extra, &QWidget::show);
        }
        layout->addWidget(new QLineEdit(&window)); window.resize(300, 200); window.show();
        const auto record = [&] {
            QProcess info;
            info.start("xwininfo", {"-id", QString::number(window.winId()), "-tree"});
            info.waitForFinished(500);
            const auto tree = QString::fromLocal8Bit(info.readAllStandardOutput());
            const auto parent = QRegularExpression("Parent window id: (0x[0-9a-fA-F]+)").match(tree).captured(1);
            const auto root = QRegularExpression("Root window id: (0x[0-9a-fA-F]+)").match(tree).captured(1);
            QSaveFile file(QString::fromLocal8Bit(argv[2]) + "/terminal-probe.json");
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(QJsonObject{{"cwd", QDir::currentPath()},
                    {"pid", QCoreApplication::applicationPid()}, {"width", window.width()},
                    {"parent", parent}, {"root", root},
                    {"args", QJsonArray::fromStringList(app.arguments().mid(3))}}).toJson());
                file.commit();
            }
            if (QFileInfo::exists(QString::fromLocal8Bit(argv[2]) + "/exit-terminal")) app.quit();
        };
        QTimer timer; QObject::connect(&timer, &QTimer::timeout, &app, record);
        timer.start(100); record();
        return app.exec();
    }
    if (argc == 4 && QByteArray(argv[1]) == "--quit-probe")
        return CoreTest::runQuitProbe(app, QByteArray(argv[2]) == "cancel", QByteArray(argv[3]) == "saving");
    CoreTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "core_test.moc"
