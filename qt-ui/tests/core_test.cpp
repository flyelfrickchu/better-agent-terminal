#include "hostclient.h"
#include "transcript.h"
#include "terminalpanel.h"
#include "mainwindow.h"
#include "settingsdialog.h"
#include "gitpanel.h"
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
#include <KParts/ReadOnlyPart>
#include <kde_terminal_interface.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <QWebSocketServer>

class CoreTest : public QObject {
    Q_OBJECT
private slots:
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
    void tlsProtocolAndLifecycle() {
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
        bool rejectSettingsSave = true;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            peer = server.nextPendingConnection(); peer->setParent(&server);
            requestPath = peer->requestUrl().toString();
            connect(peer, &QWebSocket::textMessageReceived, this, [&](const QString &text) {
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
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)));
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
QTEST_MAIN(CoreTest)
#include "core_test.moc"
