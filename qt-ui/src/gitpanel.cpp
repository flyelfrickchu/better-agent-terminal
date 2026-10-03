#include "gitpanel.h"
#include "hostclient.h"
#include <QDateTime>
#include <QEvent>
#include <QHeaderView>
#include <QDir>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QTextOption>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QtMath>

DiffHighlighter::DiffHighlighter(QPlainTextEdit *editor)
    : QSyntaxHighlighter(editor->document()), m_editor(editor) { editor->installEventFilter(this); }
bool DiffHighlighter::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::PaletteChange) rehighlight();
    return QSyntaxHighlighter::eventFilter(object, event);
}

QTextCharFormat DiffHighlighter::lineFormat(const QString &line, bool dark) {
    QTextCharFormat format;
    if (line.startsWith("diff ") || line.startsWith("index ") || line.startsWith("---") || line.startsWith("+++")) {
        format.setForeground(QColor(dark ? "#9299a3" : "#66707d"));
    } else if (line.startsWith('+')) {
        format.setForeground(QColor(dark ? "#4ec9b0" : "#087d55"));
        format.setBackground(QColor(30, 160, 110, 28));
    } else if (line.startsWith('-')) {
        format.setForeground(QColor(dark ? "#ff746c" : "#bf302a"));
        format.setBackground(QColor(220, 60, 50, 28));
    } else if (line.startsWith("@@")) {
        format.setForeground(QColor(dark ? "#569cd6" : "#1768a7"));
    }
    return format;
}
void DiffHighlighter::highlightBlock(const QString &text) {
    const auto format = lineFormat(text, m_editor->palette().color(QPalette::Base).lightness() < 128);
    if (!format.isEmpty()) setFormat(0, text.size(), format);
}

namespace {
class WrappedItemDelegate : public QStyledItemDelegate {
public:
    explicit WrappedItemDelegate(QAbstractItemView *view) : QStyledItemDelegate(view), m_view(view) {}
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option); initStyleOption(&opt, index);
        QTextDocument document; prepare(document, opt, width(index));
        return QSize(width(index), qCeil(document.size().height()) + 8);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option); initStyleOption(&opt, index);
        QTextDocument document; prepare(document, opt, option.rect.width());
        opt.text.clear();
        const auto *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette = opt.palette;
        if (opt.state & QStyle::State_Selected)
            context.palette.setColor(QPalette::Text, opt.palette.color(QPalette::HighlightedText));
        painter->save(); painter->setClipRect(option.rect);
        painter->translate(option.rect.topLeft() + QPoint(6, 4));
        document.documentLayout()->draw(painter, context); painter->restore();
    }
private:
    int width(const QModelIndex &index) const {
        if (auto *tree = qobject_cast<QTreeWidget *>(m_view)) return tree->columnWidth(index.column());
        return m_view->viewport()->width();
    }
    static void prepare(QTextDocument &document, const QStyleOptionViewItem &opt, int width) {
        document.setDocumentMargin(0); document.setDefaultFont(opt.font);
        QTextOption textOption; textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document.setDefaultTextOption(textOption); document.setPlainText(opt.text);
        document.setTextWidth(qMax(1, width - 12));
    }
    QAbstractItemView *m_view;
};
QString decodePath(QString value) {
    if (!value.startsWith('"') || !value.endsWith('"')) return value;
    const auto bytes = value.mid(1, value.size() - 2).toUtf8();
    QByteArray result;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != '\\' || i + 1 >= bytes.size()) { result.append(bytes[i]); continue; }
        const char escaped = bytes[++i];
        if (escaped >= '0' && escaped <= '7') {
            int octal = escaped - '0';
            for (int count = 1; count < 3 && i + 1 < bytes.size() && bytes[i + 1] >= '0' && bytes[i + 1] <= '7'; ++count)
                octal = octal * 8 + bytes[++i] - '0';
            result.append(static_cast<char>(octal));
        } else if (escaped == 'n') result.append('\n');
        else if (escaped == 't') result.append('\t');
        else if (escaped == 'r') result.append('\r');
        else result.append(escaped);
    }
    return QString::fromUtf8(result);
}
QString contentText(const QJsonValue &value, QString &error) {
    if (value.isString()) return value.toString();
    error = value.toObject()["error"].toString();
    return value.toObject()["content"].toString();
}
}

QString GitPanel::filePath(const QJsonObject &entry) {
    QString path = entry["file"].toString();
    const QString status = entry["status"].toString();
    if (status.contains('R') || status.contains('C')) {
        if (path.contains('\t')) path = path.section('\t', -1);
        else {
            bool quoted = false, escaped = false;
            for (qsizetype i = 0; i + 3 < path.size(); ++i) {
                if (escaped) { escaped = false; continue; }
                if (path[i] == '\\') { escaped = true; continue; }
                if (path[i] == '"') quoted = !quoted;
                if (!quoted && path.mid(i, 4) == " -> ") { path = path.mid(i + 4); break; }
            }
        }
    }
    return decodePath(path);
}
QColor GitPanel::statusColor(const QString &status, bool dark) {
    if (status.contains('U') || status.contains('D')) return QColor(dark ? "#ff746c" : "#bf302a");
    if (status.contains('R') || status.contains('C')) return QColor(dark ? "#569cd6" : "#1768a7");
    if (status.contains('M')) return QColor(dark ? "#e9b44c" : "#9b6500");
    if (status.contains('A') || status == "??") return QColor(dark ? "#4ec9b0" : "#087d55");
    return QColor(dark ? "#9299a3" : "#66707d");
}

GitPanel::GitPanel(HostClient *host, QWidget *parent) : QWidget(parent), m_host(host) {
    setObjectName("git-panel");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(this); layout->addWidget(splitter, 1);
    auto *commitsPane = new QWidget(splitter); auto *commitsLayout = new QVBoxLayout(commitsPane);
    auto *header = new QHBoxLayout; header->addWidget(new QLabel(tr("Commits"), commitsPane));
    m_refresh = new QPushButton(tr("Refresh"), commitsPane); m_refresh->setObjectName("git-refresh"); header->addWidget(m_refresh);
    commitsLayout->addLayout(header);
    m_commits = new QListWidget(commitsPane); m_commits->setObjectName("git-commits"); commitsLayout->addWidget(m_commits);
    m_commits->setItemDelegate(new WrappedItemDelegate(m_commits));
    m_commits->setWordWrap(true);
    m_commits->setTextElideMode(Qt::ElideNone);
    m_commits->setResizeMode(QListView::Adjust);
    m_commits->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *filesPane = new QWidget(splitter); auto *filesLayout = new QVBoxLayout(filesPane);
    m_fileCount = new QLabel(tr("Files"), filesPane); filesLayout->addWidget(m_fileCount);
    m_files = new QTreeWidget(filesPane); m_files->setObjectName("git-files");
    m_files->setHeaderHidden(true); m_files->setColumnCount(2); m_files->setRootIsDecorated(false);
    m_files->setColumnWidth(0, 35); filesLayout->addWidget(m_files);
    m_files->setItemDelegate(new WrappedItemDelegate(m_files));
    connect(m_files->header(), &QHeaderView::sectionResized, m_files, [this] { m_files->doItemsLayout(); });
    m_files->setWordWrap(true);
    m_files->setTextElideMode(Qt::ElideNone);
    m_files->setUniformRowHeights(false);
    m_files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_files->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_previews = new QTabWidget(splitter); m_previews->setObjectName("git-previews");
    m_previews->setDocumentMode(true);
    auto editor = [this] {
        auto *view = new QPlainTextEdit(m_previews); view->setReadOnly(true);
        view->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        view->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        return view;
    };
    m_diff = editor(); m_diff->setObjectName("git-diff"); new DiffHighlighter(m_diff);
    m_content = editor(); m_content->setObjectName("git-file-content");
    m_previews->addTab(m_diff, tr("Diff")); m_previews->addTab(m_content, tr("File"));
    m_previews->setTabToolTip(1, tr("Current working copy of the selected file"));
    splitter->addWidget(commitsPane); splitter->addWidget(filesPane); splitter->addWidget(m_previews);
    splitter->setSizes({300, 230, 650}); splitter->setStretchFactor(2, 1);
    m_status = new QLabel(tr("Select a workspace to browse Git."), this); m_status->setWordWrap(true); layout->addWidget(m_status);
    connect(m_refresh, &QPushButton::clicked, this, &GitPanel::refresh);
    connect(m_commits, &QListWidget::currentRowChanged, this, [this] { selectCommit(); });
    connect(m_files, &QTreeWidget::currentItemChanged, this, [this] { selectFile(); });
    connect(m_host, &HostClient::disconnected, this, [this] {
        ++m_generation; m_refresh->setEnabled(true); m_status->setText(tr("Host disconnected. Reconnect and refresh Git."));
    });
}

bool GitPanel::current(quint64 generation) const {
    return generation == m_generation && m_context == m_host->contextId();
}
void GitPanel::setWorkspace(const QString &directory) {
    if (directory == m_directory && m_context == m_host->contextId()) return;
    m_directory = directory; m_context = m_host->contextId();
    ++m_generation; m_root.clear(); m_commit.clear(); m_file.clear();
    { const QSignalBlocker commits(m_commits), files(m_files); m_commits->clear(); m_files->clear(); }
    m_diff->clear(); m_content->clear(); m_fileCount->setText(tr("Files"));
    m_status->setText(directory.isEmpty() ? tr("Select a workspace to browse Git.") : tr("Refresh to load Git history."));
    m_refresh->setEnabled(true);
    if (isVisible()) refresh();
}
void GitPanel::refresh() {
    if (!m_host->ready() || m_context.isEmpty() || m_directory.isEmpty()) {
        m_status->setText(tr("Connect to a host and select a workspace to browse Git.")); return;
    }
    const auto generation = ++m_generation;
    m_refresh->setEnabled(false); m_status->setText(tr("Loading Git…"));
    m_commits->setEnabled(false); m_files->setEnabled(false);
    m_host->invoke("git:getRoot", {{"cwd", m_directory}}, this,
        [this, generation](const QJsonValue &value, const QString &error) {
            if (!current(generation)) return;
            m_root = value.toString();
            if (!error.isEmpty() || m_root.isEmpty()) {
                m_refresh->setEnabled(true);
                m_status->setText(error.isEmpty() ? tr("This workspace is not a Git repository.") : error); return;
            }
            m_pending = 2; m_statusEntries = {}; m_logEntries = {};
            for (const auto &channel : {QString("git:status"), QString("git:log")}) {
                m_host->invoke(channel, {{"cwd", m_directory}, {"count", 100}}, this,
                    [this, generation, channel](const QJsonValue &result, const QString &requestError) {
                        if (!current(generation)) return;
                        if (!requestError.isEmpty()) m_status->setText(requestError);
                        if (channel == "git:status") m_statusEntries = result.toArray(); else m_logEntries = result.toArray();
                        if (--m_pending == 0) {
                            m_refresh->setEnabled(true); m_commits->setEnabled(true); m_files->setEnabled(true);
                            populateCommits();
                        }
                    });
            }
        });
}
void GitPanel::populateCommits() {
    const QSignalBlocker blocker(m_commits);
    m_commits->clear();
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    auto *working = new QListWidgetItem(tr("● Uncommitted Changes\n%1 files changed").arg(m_statusEntries.size()), m_commits);
    working->setData(Qt::UserRole, "working"); working->setForeground(statusColor("M", dark));
    for (const auto &value : m_logEntries) {
        const auto commit = value.toObject();
        const QString hash = commit["hash"].toString();
        const auto date = QDateTime::fromString(commit["date"].toString(), Qt::ISODate);
        auto *item = new QListWidgetItem(commit["message"].toString() + "\n" + hash.left(7) + "  " +
            commit["author"].toString() + "  " + (date.isValid() ? date.date().toString(Qt::ISODate) : commit["date"].toString()), m_commits);
        item->setData(Qt::UserRole, hash); item->setToolTip(commit["message"].toString() + "\n" + hash);
    }
    m_commits->setCurrentRow(0);
    selectCommit();
}
void GitPanel::selectCommit() {
    if (!m_commits->currentItem() || !m_host->ready()) return;
    m_commit = m_commits->currentItem()->data(Qt::UserRole).toString();
    m_file.clear(); m_diff->clear(); m_content->clear();
    { const QSignalBlocker blocker(m_files); m_files->clear(); }
    m_files->setEnabled(false);
    const auto generation = ++m_generation;
    if (m_commit == "working") { populateFiles(m_statusEntries); return; }
    m_status->setText(tr("Loading changed files…"));
    m_host->invoke("git:diff-files", {{"cwd", m_directory}, {"commitHash", m_commit}}, this,
        [this, generation](const QJsonValue &value, const QString &error) {
            if (!current(generation)) return;
            if (!error.isEmpty()) { m_status->setText(error); return; }
            populateFiles(value.toArray());
        });
}
void GitPanel::populateFiles(const QJsonArray &files) {
    m_files->setEnabled(true);
    { const QSignalBlocker blocker(m_files); m_files->clear();
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        for (const auto &value : files) {
            const auto entry = value.toObject(); const QString path = filePath(entry);
            auto *item = new QTreeWidgetItem(m_files, {entry["status"].toString(), path});
            item->setData(0, Qt::UserRole, path); item->setData(0, Qt::UserRole + 1, entry["status"]);
            item->setForeground(0, statusColor(entry["status"].toString(), dark)); item->setToolTip(1, path);
        }
    }
    m_fileCount->setText(tr("Files · %1").arg(files.size()));
    m_status->setText(files.isEmpty() ? tr("No changed files.") : tr("Select a file to view its diff."));
    if (m_files->topLevelItemCount()) m_files->setCurrentItem(m_files->topLevelItem(0));
}
void GitPanel::selectFile() {
    auto *item = m_files->currentItem(); if (!item || !m_host->ready()) return;
    m_file = item->data(0, Qt::UserRole).toString(); m_fileStatus = item->data(0, Qt::UserRole + 1).toString();
    m_previews->setCurrentIndex(0); m_diff->setPlainText(tr("Loading diff…")); m_content->setPlainText(tr("Loading file…"));
    const auto generation = ++m_generation;
    readFile(false, generation);
    m_host->invoke("git:diff", {{"cwd", m_directory}, {"commitHash", m_commit}, {"filePath", m_file}}, this,
        [this, generation](const QJsonValue &value, const QString &error) {
            if (!current(generation)) return;
            if (!error.isEmpty()) { m_diff->setPlainText(error); return; }
            const auto diff = value.toString();
            if (diff.isEmpty() && m_commit == "working" && (m_fileStatus == "??" || m_fileStatus.contains('A'))) {
                readFile(true, generation); return;
            }
            m_diff->setPlainText(diff.isEmpty() ? tr("No text diff available for this file.") : diff);
            m_status->setText(m_file);
        });
}
void GitPanel::readFile(bool asDiff, quint64 generation) {
    const QString file = m_file;
    m_host->invoke("fs:readFile", {{"filePath", QDir(m_root).filePath(file)}}, this,
        [this, generation, file, asDiff](const QJsonValue &value, const QString &error) {
            if (!current(generation)) return;
            QString readError; const QString content = contentText(value, readError);
            auto *view = asDiff ? m_diff : m_content;
            if (!error.isEmpty() || !readError.isEmpty()) { view->setPlainText(error.isEmpty() ? readError : error); return; }
            if (!asDiff) { view->setPlainText(content); return; }
            const auto lines = content.split('\n'); QStringList additions;
            for (const auto &line : lines) additions.append("+" + line);
            m_diff->setPlainText(QString("diff --git a/%1 b/%1\nnew file\n--- /dev/null\n+++ b/%1\n@@ -0,0 +1,%2 @@\n%3")
                .arg(file).arg(lines.size()).arg(additions.join('\n')));
            m_status->setText(file);
        });
}
