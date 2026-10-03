#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
class HostClient;
class QLabel;
class QListWidget;
class QTreeWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;

class DiffHighlighter : public QSyntaxHighlighter {
public:
    explicit DiffHighlighter(QPlainTextEdit *editor);
    static QTextCharFormat lineFormat(const QString &line, bool dark);
protected:
    void highlightBlock(const QString &text) override;
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    QPlainTextEdit *m_editor;
};

class GitPanel : public QWidget {
    Q_OBJECT
public:
    explicit GitPanel(HostClient *host, QWidget *parent = nullptr);
    void setWorkspace(const QString &directory);
    void refresh();
    static QString filePath(const QJsonObject &entry);
    static QColor statusColor(const QString &status, bool dark);
private:
    bool current(quint64 generation) const;
    void populateCommits();
    void selectCommit();
    void populateFiles(const QJsonArray &files);
    void selectFile();
    void readFile(bool asDiff, quint64 generation);
    HostClient *m_host;
    QString m_directory;
    QString m_context;
    QString m_root;
    QString m_commit;
    QString m_file;
    QString m_fileStatus;
    quint64 m_generation = 0;
    int m_pending = 0;
    QJsonArray m_statusEntries;
    QJsonArray m_logEntries;
    QLabel *m_status;
    QLabel *m_fileCount;
    QListWidget *m_commits;
    QTreeWidget *m_files;
    QTabWidget *m_previews;
    QPlainTextEdit *m_diff;
    QPlainTextEdit *m_content;
    QPushButton *m_refresh;
};
