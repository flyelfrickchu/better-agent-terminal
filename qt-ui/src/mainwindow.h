#pragma once
#include "hostclient.h"
#include <QMainWindow>
#include <QJsonArray>
#include <QProcess>
#include <QTimer>
#include <QUrl>

class QComboBox;
class QListWidget;
class QStackedWidget;
class QTabWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QCloseEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class CoreTest;
public:
    explicit MainWindow(const QString &dataDirectory, QWidget *parent = nullptr);
    ~MainWindow() override;
    void startLocalHost(const QString &serverBinary);
    void connectToHost(const QUrl &url);
    bool hostReady() const { return m_host.ready() && !m_host.contextId().isEmpty(); }
signals:
    void workspaceReady();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void buildUi();
    void loadProfiles();
    void openProfile(const QString &id);
    void loadWorkspaces();
    void applySnapshot(const QJsonObject &snapshot);
    void showWorkspace(const QString &id, bool save = true);
    void addWorkspace();
    void removeWorkspace();
    void renameWorkspace();
    void addTab(const QString &preset);
    void createTab(const QJsonObject &descriptor, bool save = true);
    void closeTab(QTabWidget *tabs, int index);
    void saveSnapshot();
    void refreshFiles();
    void loadDirectory(QTreeWidgetItem *parent, const QString &path);
    void previewFile(const QString &path);
    void showGit();
    void showSettings();
    void loadSettings();
    void applySettings(const QJsonObject &settings);
    void showHistory();
    void showConnectionDialog();
    QJsonObject activeWorkspace() const;
    QString currentDirectory() const;
    void report(const QString &message);
    bool hasRunningAgents() const;
    void clearPages();
    HostClient m_host;
    QProcess m_server;
    QByteArray m_serverOutput;
    QString m_dataDirectory;
    QUrl m_connectionUrl;
    QString m_profileId = "default";
    QString m_workspaceId;
    QString m_contextBinding;
    bool m_localConnection = true;
    bool m_localProfile = true;
    bool m_loading = false;
    bool m_saveInFlight = false;
    bool m_saveAgain = false;
    bool m_closing = false;
    int m_reconnectDelay = 1000;
    QJsonObject m_snapshot;
    QJsonObject m_settings;
    QJsonArray m_profiles;
    QComboBox *m_profilePicker;
    QListWidget *m_workspaces;
    QStackedWidget *m_pages;
    QTreeWidget *m_files;
    QTextBrowser *m_preview;
    QHash<QString, QTabWidget *> m_workspaceTabs;
    QTimer m_saveTimer;
    QTimer m_reconnectTimer;
    QString m_filesContext;
    QString m_filesDirectory;
    quint64 m_filesGeneration = 0;
};
