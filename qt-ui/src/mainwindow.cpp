#include "mainwindow.h"
#include "agentpanel.h"
#include "terminalpanel.h"
#include "settingsdialog.h"
#include "gitpanel.h"
#include <QTabBar>
#include <QSplitter>
#include <QStyle>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDrag>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QSettings>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeWidget>
#include <QUuid>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <array>
#include <functional>
#include <optional>
#include <utility>

namespace {
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

class ProjectMoveAnimation final : public QWidget {
public:
    struct Block { QRect before; QRect after; };

    explicit ProjectMoveAnimation(QWidget *viewport) : QWidget(viewport) {
        setObjectName("project-move-animation");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
        m_progress.setDuration(180);
        m_progress.setStartValue(0.0);
        m_progress.setEndValue(1.0);
        m_progress.setEasingCurve(QEasingCurve::OutCubic);
        connect(&m_progress, &QVariantAnimation::valueChanged, this, [this] { update(); });
        connect(&m_progress, &QVariantAnimation::finished, this, &ProjectMoveAnimation::clear);
    }

    void play(QPixmap before, QPixmap after, const std::array<Block, 2> &blocks) {
        m_before = std::move(before);
        m_after = std::move(after);
        m_blocks = blocks;
        setGeometry(parentWidget()->rect());
        raise();
        show();
        m_progress.start();
    }

    void clear() {
        m_progress.stop();
        hide();
        m_before = {};
        m_after = {};
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        QRect affected;
        for (const auto &block : m_blocks) affected = affected.united(block.before).united(block.after);
        painter.setClipRect(affected);
        painter.fillRect(affected, palette().base());
        const qreal progress = m_progress.currentValue().toReal();
        for (const auto &block : m_blocks) {
            const int y = qRound(block.before.top() + (block.after.top() - block.before.top()) * progress);
            painter.save();
            painter.setClipRect(block.after.translated(0, y - block.after.top()), Qt::IntersectClip);
            // The two captures cover rows clipped by the viewport at either end of the move.
            painter.drawPixmap(0, y - block.before.top(), m_before);
            painter.drawPixmap(0, y - block.after.top(), m_after);
            painter.restore();
        }
    }

private:
    QVariantAnimation m_progress;
    QPixmap m_before, m_after;
    std::array<Block, 2> m_blocks;
};

class WorkspaceProjectTree final : public QTreeWidget {
public:
    static constexpr int dragHandleColumn = 3;

    WorkspaceProjectTree(QWidget *parent, std::function<bool(const QString &, int)> moveProject)
        : QTreeWidget(parent), m_moveProject(std::move(moveProject)),
          m_animation(new ProjectMoveAnimation(viewport())) {
        setDragDropMode(InternalMove);
        setDropIndicatorShown(false);
    }

protected:
    void reset() override {
        setDragPosition(std::nullopt);
        m_animation->clear();
        QTreeWidget::reset();
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *handle = qobject_cast<QToolButton *>(watched);
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (handle && handle->isEnabled() && mouse->button() == Qt::LeftButton) {
                auto *project = itemAt(handle->mapTo(viewport(), mouse->position().toPoint()));
                if (project && itemWidget(project, dragHandleColumn) == handle) {
                    dragProject(project);
                    return true;
                }
            }
        }
        return QTreeWidget::eventFilter(watched, event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override {
        if (!isProjectDrag(event)) {
            event->ignore();
            return;
        }
        QTreeWidget::dragMoveEvent(event); // Keep the view's edge auto-scroll active.
        const QPoint point = event->position().toPoint();
        setDragPosition(point);
        event->setDropAction(Qt::MoveAction);
        event->setAccepted(m_guideY >= 0);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override {
        setDragPosition(std::nullopt);
        QTreeWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override {
        const auto location = isProjectDrag(event) ? dropLocation(event->position().toPoint()) : DropLocation{};
        // Clear Qt's drag state without letting the tree model reparent items.
        QDragLeaveEvent leave;
        dragLeaveEvent(&leave);
        event->ignore();
        if (location.index < 0) return;

        const int from = m_draggedProject.row();
        const int to = location.index > from ? location.index - 1 : location.index;
        const int firstShifted = from < to ? from + 1 : to;
        const int lastShifted = from < to ? to : from - 1;
        const int shift = from < to ? -1 : 1;
        const QRect movedBefore = projectRows(from, from);
        const QRect shiftedBefore = projectRows(firstShifted, lastShifted);
        auto before = viewport()->grab();
        const int scrollPosition = verticalScrollBar()->value();
        if (!m_moveProject(QString::fromUtf8(event->mimeData()->data(projectIdFormat)), location.index)) return;
        doItemsLayout();
        verticalScrollBar()->setValue(scrollPosition);
        m_animation->play(std::move(before), viewport()->grab(), {{
            {shiftedBefore, projectRows(firstShifted + shift, lastShifted + shift)},
            {movedBefore, projectRows(to, to)},
        }});
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void scrollContentsBy(int dx, int dy) override {
        m_animation->clear();
        QTreeWidget::scrollContentsBy(dx, dy);
        if (m_dragPosition) {
            setDragPosition(m_dragPosition);
            viewport()->update();
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        m_animation->clear();
        QTreeWidget::resizeEvent(event);
        if (m_dragPosition) setDragPosition(m_dragPosition);
    }

    void paintEvent(QPaintEvent *event) override {
        QTreeWidget::paintEvent(event);
        if (m_guideY < 0) return;
        QPainter painter(viewport());
        painter.fillRect(QRect(4, m_guideY - 1, viewport()->width() - 8, 3), palette().highlight());
    }

private:
    struct DropLocation { int index = -1; int y = -1; };

    void dragProject(QTreeWidgetItem *project) {
        m_animation->clear();
        m_draggedProject = indexFromItem(project);
        auto *data = mimeData({project});
        data->setData(projectIdFormat, project->data(0, Qt::UserRole).toString().toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(data);
        drag->setPixmap(projectPreview(project));
        drag->setHotSpot(QPoint(-12, -8));
        drag->exec(Qt::MoveAction);
        m_draggedProject = QPersistentModelIndex();
        setDragPosition(std::nullopt);
    }

    bool isProjectDrag(const QDropEvent *event) const {
        return m_draggedProject.isValid() && event->source() == this &&
            event->mimeData()->hasFormat(projectIdFormat);
    }

    QRect projectRows(int first, int last) const {
        const auto *end = topLevelItem(last);
        if (end->isExpanded() && end->childCount()) end = end->child(end->childCount() - 1);
        const int top = visualItemRect(topLevelItem(first)).top();
        const int bottom = visualItemRect(end).bottom();
        return QRect(0, top, viewport()->width(), bottom - top + 1);
    }

    DropLocation dropLocation(const QPoint &point) const {
        if (!m_draggedProject.isValid() || !viewport()->rect().contains(point)) return {};
        auto *target = itemAt(point);
        const bool below = !target || target->parent() || point.y() > visualItemRect(target).center().y();
        if (target && target->parent()) target = target->parent();
        const int row = target ? indexOfTopLevelItem(target) : topLevelItemCount() - 1;
        const int index = row + (below ? 1 : 0);
        const int from = m_draggedProject.row();
        if (index == from || index == from + 1) return {};
        const QRect rows = projectRows(row, row);
        const int y = below ? rows.bottom() + 1 : rows.top();
        return {index, qBound(1, y, qMax(1, viewport()->height() - 2))};
    }

    void setDragPosition(std::optional<QPoint> point) {
        m_dragPosition = point;
        const int y = point ? dropLocation(*point).y : -1;
        if (m_guideY == y) return;
        m_guideY = y;
        viewport()->update();
    }

    QPixmap projectPreview(const QTreeWidgetItem *project) const {
        constexpr int padding = 8;
        constexpr int iconSize = 16;
        const QFont font = project->font(1).resolve(this->font());
        const QFontMetrics metrics(font);
        const QString name = metrics.elidedText(project->text(1), Qt::ElideMiddle,
            qMax(96, viewport()->width() - iconSize - padding * 3));
        const QSize size(metrics.horizontalAdvance(name) + iconSize + padding * 3,
                         qMax(metrics.height(), iconSize) + padding * 2);
        const qreal scale = devicePixelRatioF();
        QPixmap preview(size * scale);
        preview.setDevicePixelRatio(scale);
        preview.fill(Qt::transparent);
        QPainter painter(&preview);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().highlight());
        painter.drawRoundedRect(QRect(QPoint(), size), 5, 5);
        const QIcon icon = project->icon(1).isNull() ? style()->standardIcon(QStyle::SP_DirIcon) : project->icon(1);
        icon.paint(&painter, QRect(padding, (size.height() - iconSize) / 2, iconSize, iconSize));
        painter.setFont(font);
        painter.setPen(palette().highlightedText().color());
        painter.drawText(QRect(padding * 2 + iconSize, 0, metrics.horizontalAdvance(name), size.height()),
                         Qt::AlignVCenter | Qt::AlignLeft, name);
        return preview;
    }

    static constexpr auto projectIdFormat = "application/x-bat-project-id";
    std::function<bool(const QString &, int)> m_moveProject;
    ProjectMoveAnimation *m_animation;
    QPersistentModelIndex m_draggedProject;
    std::optional<QPoint> m_dragPosition;
    int m_guideY = -1;
};
}

MainWindow::MainWindow(const QString &dataDirectory, QWidget *parent)
    : QMainWindow(parent), m_host(this), m_dataDirectory(dataDirectory) {
    setWindowTitle(tr("Better Agent Terminal — Qt"));
    setWindowIcon(QIcon::fromTheme("utilities-terminal"));
    resize(1280, 820);
    buildUi();
    QSettings preferences;
    restoreGeometry(preferences.value("geometry").toByteArray());
    restoreState(preferences.value("windowState").toByteArray());
    m_saveTimer.setSingleShot(true); m_saveTimer.setInterval(250);
    connect(&m_saveTimer, &QTimer::timeout, this, &MainWindow::saveSnapshot);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this] {
        if (!m_closing && m_connectionUrl.isValid()) m_host.connectTo(m_connectionUrl);
    });
    connect(&m_host, &HostClient::connected, this, [this] {
        m_reconnectDelay = 1000; report(tr("Connected to Rust host")); loadProfiles();
    });
    connect(&m_host, &HostClient::connectionError, this, &MainWindow::report);
    connect(&m_host, &HostClient::disconnected, this, [this] {
        if (m_closing) return;
        report(tr("Disconnected — reconnecting…"));
        m_profilePicker->setEnabled(false);
        m_saveTimer.stop();
        m_reconnectTimer.start(m_reconnectDelay);
        m_reconnectDelay = qMin(m_reconnectDelay * 2, 30000);
    });
    connect(&m_host, &HostClient::eventReceived, this, [this](const QString &channel, const QJsonObject &params) {
        if (channel == "workspace:reload") {
            // A successful save is also broadcast as a reload. Let the save
            // queue commit its reply without replacing newer pending edits.
            if (m_saveInFlight || m_saveTimer.isActive()) return;
            if (params["data"].isString()) {
                const auto data = QJsonDocument::fromJson(params["data"].toString().toUtf8());
                if (data.isObject()) applySnapshot(data.object());
            } else loadWorkspaces();
        } else if (channel == "fs:changed") refreshFiles();
        else if (channel == "profile:status" && params["status"] == "unavailable")
            report(tr("This profile is unavailable. Check its connection in BAT."));
    });
    connect(&m_server, &QProcess::readyReadStandardOutput, this, [this] {
        m_serverOutput += m_server.readAllStandardOutput();
        // Consume the host's connection banner in memory. Never log its token.
        const int start = m_serverOutput.indexOf("connect:");
        if (start >= 0) {
            const int end = m_serverOutput.indexOf('\n', start);
            if (end > start) {
                const QUrl url(QString::fromUtf8(m_serverOutput.mid(start + 8, end - start - 8)).trimmed());
                m_serverOutput.clear();
                if (HostClient::validateConnectionUrl(url).isEmpty()) connectToHost(url);
                else report(tr("The Rust host returned an invalid connection URL."));
            }
        }
        if (m_serverOutput.size() > 65536) m_serverOutput.clear();
    });
    connect(&m_server, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(m_server.readAllStandardError()).trimmed();
        if (!text.isEmpty()) report(text.right(500));
    });
    connect(&m_server, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { report(m_server.errorString()); });
    connect(&m_server, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (!m_closing) { m_reconnectTimer.stop(); report(tr("Rust host exited (%1). Use Connect to attach to another host.").arg(code)); }
    });
}

MainWindow::~MainWindow() {
    m_closing = true;
    m_host.disconnectFromHost();
    // An externally connected host is never terminated; QProcess owns only ours.
    if (m_server.state() != QProcess::NotRunning) {
        m_server.terminate();
        if (!m_server.waitForFinished(3000)) { m_server.kill(); m_server.waitForFinished(1000); }
    }
}

void MainWindow::buildUi() {
    auto *toolbar = addToolBar(tr("Workspace actions"));
    toolbar->setObjectName("workspace-toolbar");
    m_profilePicker = new QComboBox(toolbar); m_profilePicker->setMinimumWidth(160);
    m_profilePicker->setEnabled(false); toolbar->addWidget(m_profilePicker);
    connect(m_profilePicker, &QComboBox::activated, this, [this](int index) {
        if (hasRunningAgents() && QMessageBox::question(this, tr("Switch profile"),
            tr("Agent sessions are still running. Switch profiles and leave them on the host?")) != QMessageBox::Yes) {
            const QSignalBlocker blocker(m_profilePicker);
            m_profilePicker->setCurrentIndex(m_profilePicker->findData(m_profileId)); return;
        }
        openProfile(m_profilePicker->itemData(index).toString());
    });
    toolbar->addAction(QIcon::fromTheme("folder-new"), tr("Add workspace"), this, &MainWindow::addWorkspace);
    toolbar->addAction(QIcon::fromTheme("utilities-terminal"), tr("Terminal"), this, [this] { addTab("none"); });
    toolbar->addAction(tr("Claude"), this, [this] { addTab("claude-code"); });
    toolbar->addAction(tr("Codex"), this, [this] { addTab("codex-agent"); });
    toolbar->addSeparator();
    toolbar->addAction(tr("History"), this, &MainWindow::showHistory);
    toolbar->addAction(tr("Git"), this, &MainWindow::showGit);
    toolbar->addAction(tr("Settings"), this, &MainWindow::showSettings);
    toolbar->addAction(tr("Connect"), this, &MainWindow::showConnectionDialog);

    auto *workspaceDock = new QDockWidget(tr("Workspaces"), this);
    workspaceDock->setObjectName("workspaces-dock");
    auto *workspaceTitle = new QWidget(workspaceDock);
    auto *titleLayout = new QHBoxLayout(workspaceTitle);
    titleLayout->setContentsMargins(6, 2, 4, 2);
    titleLayout->addWidget(new QLabel(tr("Workspaces"), workspaceTitle));
    titleLayout->addStretch();
    auto *newProject = new QToolButton(workspaceTitle);
    newProject->setObjectName("add-project");
    newProject->setText("+"); newProject->setToolTip(tr("Add project"));
    newProject->setAccessibleName(tr("Add project"));
    titleLayout->addWidget(newProject);
    auto *floatWorkspaces = new QToolButton(workspaceTitle);
    floatWorkspaces->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));
    floatWorkspaces->setToolTip(tr("Float / dock Workspaces"));
    floatWorkspaces->setAccessibleName(floatWorkspaces->toolTip());
    titleLayout->addWidget(floatWorkspaces);
    connect(floatWorkspaces, &QToolButton::clicked, workspaceDock, [workspaceDock] {
        workspaceDock->setFloating(!workspaceDock->isFloating());
    });
    auto *closeWorkspaces = new QToolButton(workspaceTitle);
    closeWorkspaces->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    closeWorkspaces->setToolTip(tr("Hide Workspaces"));
    closeWorkspaces->setAccessibleName(closeWorkspaces->toolTip());
    titleLayout->addWidget(closeWorkspaces);
    connect(closeWorkspaces, &QToolButton::clicked, workspaceDock, &QDockWidget::close);
    workspaceDock->setTitleBarWidget(workspaceTitle);
    connect(newProject, &QToolButton::clicked, this, &MainWindow::addProject);
    m_workspaces = new WorkspaceProjectTree(workspaceDock, [this](const QString &id, int index) {
        return moveProject(id, index);
    });
    m_workspaces->setObjectName("workspace-projects");
    constexpr int dragHandleColumn = WorkspaceProjectTree::dragHandleColumn;
    m_workspaces->setColumnCount(dragHandleColumn + 1); m_workspaces->setHeaderHidden(true);
    m_workspaces->setRootIsDecorated(false);
    m_workspaces->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_workspaces->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_workspaces->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_workspaces->header()->setSectionResizeMode(dragHandleColumn, QHeaderView::ResizeToContents);
    m_workspaces->header()->setStretchLastSection(false);
    m_workspaces->header()->moveSection(dragHandleColumn, 0);
    m_workspaces->setTreePosition(1);
    m_workspaces->setContextMenuPolicy(Qt::CustomContextMenu);
    workspaceDock->setWidget(m_workspaces); addDockWidget(Qt::LeftDockWidgetArea, workspaceDock);
    connect(m_workspaces, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!item) return;
        auto *project = item->parent() ? item->parent() : item;
        m_projectId = project->data(0, Qt::UserRole).toString();
        auto qt = m_snapshot["qtUi"].toObject();
        qt["activeProjectId"] = m_projectId; m_snapshot["qtUi"] = qt;
        if (item->parent()) showWorkspace(item->data(0, Qt::UserRole).toString());
        if (!m_loading) scheduleSnapshotSave();
    });
    const auto saveExpansion = [this](QTreeWidgetItem *item) {
        if (m_loading || item->parent()) return;
        if (auto *toggle = qobject_cast<QToolButton *>(m_workspaces->itemWidget(item, 0)))
            toggle->setText(item->isExpanded() ? "−" : "+");
        auto qt = m_snapshot["qtUi"].toObject(); auto projects = qt["projects"].toArray();
        for (int i = 0; i < projects.size(); ++i) {
            auto project = projects[i].toObject();
            if (project["id"].toString() == item->data(0, Qt::UserRole).toString()) {
                project["collapsed"] = !item->isExpanded(); projects[i] = project;
            }
        }
        qt["projects"] = projects; m_snapshot["qtUi"] = qt; scheduleSnapshotSave();
    };
    connect(m_workspaces, &QTreeWidget::itemExpanded, this, saveExpansion);
    connect(m_workspaces, &QTreeWidget::itemCollapsed, this, saveExpansion);
    connect(m_workspaces, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        auto *item = m_workspaces->itemAt(point);
        if (item) m_workspaces->setCurrentItem(item);
        QMenu menu(this);
        if (item && item->parent()) {
            menu.addAction(tr("Rename workspace"), this, &MainWindow::renameWorkspace);
            menu.addAction(tr("Remove workspace"), this, &MainWindow::removeWorkspace);
        } else if (item) {
            menu.addAction(tr("Add workspace"), this, &MainWindow::addWorkspace);
            menu.addAction(tr("Rename project"), this, &MainWindow::renameProject);
            menu.addAction(tr("Remove project"), this, &MainWindow::removeProject);
        }
        menu.addAction(tr("Add project"), this, &MainWindow::addProject);
        menu.exec(m_workspaces->viewport()->mapToGlobal(point));
    });
    auto *fileDock = new QDockWidget(tr("Files"), this); fileDock->setObjectName("files-dock");
    m_files = new QTreeWidget(fileDock); m_files->setHeaderHidden(true);
    fileDock->setWidget(m_files); addDockWidget(Qt::LeftDockWidgetArea, fileDock);
    connect(m_files, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (!item->data(0, Qt::UserRole + 1).toBool()) return;
        if (item->data(0, Qt::UserRole + 2).toBool()) return;
        item->setData(0, Qt::UserRole + 2, true);
        loadDirectory(item, item->data(0, Qt::UserRole).toString());
    });
    connect(m_files, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (!item->data(0, Qt::UserRole + 1).toBool()) previewFile(item->data(0, Qt::UserRole).toString());
    });
    auto *workspaceView = new QWidget(this);
    auto *workspaceLayout = new QVBoxLayout(workspaceView);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    m_workspaceViews = new QTabBar(workspaceView);
    m_workspaceViews->setObjectName("workspace-views");
    m_workspaceViews->setExpanding(false);
    m_workspaceViews->addTab(tr("Terminal"));
    m_workspaceViews->addTab(tr("Git"));
    m_workspaceViews->setTabsClosable(true);
    m_workspaceViews->setTabButton(0, QTabBar::LeftSide, nullptr);
    m_workspaceViews->setTabButton(0, QTabBar::RightSide, nullptr);
    workspaceLayout->addWidget(m_workspaceViews);
    m_workspaceSplitter = new QSplitter(Qt::Vertical, workspaceView);
    m_gitPanel = new GitPanel(&m_host, m_workspaceSplitter);
    m_pages = new QStackedWidget(m_workspaceSplitter);
    m_workspaceSplitter->addWidget(m_gitPanel);
    m_workspaceSplitter->addWidget(m_pages);
    m_gitPanel->hide();
    workspaceLayout->addWidget(m_workspaceSplitter, 1);
    setCentralWidget(workspaceView);
    connect(m_workspaceViews, &QTabBar::currentChanged, this, [this](int index) {
        if (index == 1) {
            m_gitPanel->setWorkspace(currentDirectory());
            m_gitPanel->show();
            m_gitPanel->refresh();
            m_workspaceSplitter->setSizes({600, 250});
        } else m_gitPanel->hide();
    });
    connect(m_workspaceViews, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (index != 1) return;
        m_workspaceViews->setCurrentIndex(0);
        m_workspaceViews->removeTab(index);
    });
    auto *welcome = new QLabel(tr("Add a workspace to open a terminal or agent session."), m_pages);
    welcome->setAlignment(Qt::AlignCenter); welcome->setWordWrap(true); m_pages->addWidget(welcome);
    auto *previewDock = new QDockWidget(tr("File preview"), this); previewDock->setObjectName("preview-dock");
    m_preview = new QTextBrowser(previewDock); m_preview->setOpenExternalLinks(false);
    previewDock->setWidget(m_preview); addDockWidget(Qt::RightDockWidgetArea, previewDock); previewDock->hide();

    auto *fileMenu = menuBar()->addMenu(tr("File"));
    fileMenu->addAction(tr("Add project"), this, &MainWindow::addProject);
    auto *newWorkspace = fileMenu->addAction(tr("Add workspace"), this, &MainWindow::addWorkspace);
    newWorkspace->setShortcut(QKeySequence("Ctrl+Shift+O"));
    auto *newTerminal = fileMenu->addAction(tr("New terminal"), this, [this] { addTab("none"); });
    newTerminal->setShortcut(QKeySequence("Ctrl+Shift+T"));
    fileMenu->addAction(tr("Claude CLI in terminal"), this, [this] { addTab("claude-cli"); });
    fileMenu->addAction(tr("Codex CLI in terminal"), this, [this] { addTab("codex-cli"); });
    fileMenu->addSeparator(); fileMenu->addAction(tr("Connect to host…"), this, &MainWindow::showConnectionDialog);
    fileMenu->addAction(tr("Quit"), this, &QWidget::close)->setShortcut(QKeySequence::Quit);
    auto *viewMenu = menuBar()->addMenu(tr("View"));
    for (auto *dock : {workspaceDock, fileDock, previewDock}) viewMenu->addAction(dock->toggleViewAction());
    viewMenu->addAction(tr("Refresh files"), this, &MainWindow::refreshFiles)->setShortcut(QKeySequence::Refresh);
    auto *helpMenu = menuBar()->addMenu(tr("Help"));
    helpMenu->addAction(tr("About"), this, [this] {
        QMessageBox::about(this, tr("Better Agent Terminal — Qt"),
            tr("Version %1\n\n"
               "Native Qt 6 interface with embedded KDE Konsole.\n"
               "Agent sessions use the existing Rust host.\nDevelopment branch: dev_qt")
                .arg(QApplication::applicationVersion()));
    });
    report(tr("Starting Rust host…"));
}

void MainWindow::startLocalHost(const QString &serverBinary) {
    m_localConnection = true;
    if (!QFileInfo::exists(serverBinary)) {
        report(tr("Rust host not found. Run pnpm run qt:build or pass --server=/path/to/bat-server.")); return;
    }
    // Reserve an available port, then release immediately before process start.
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0)) { report(reservation.errorString()); return; }
    const quint16 port = reservation.serverPort(); reservation.close();
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("BAT_TOKEN"); env.remove("BAT_TOKEN_FILE"); env.remove("CREDENTIALS_DIRECTORY");
    env.insert("BAT_TAURI_DATA_DIR", m_dataDirectory);
    m_server.setProcessEnvironment(env);
    m_server.setProgram(serverBinary);
    m_server.setArguments({QString("--port=%1").arg(port), "--bind=localhost", "--data-dir=" + m_dataDirectory});
    m_server.start();
}

void MainWindow::connectToHost(const QUrl &url) {
    const QString error = HostClient::validateConnectionUrl(url);
    if (!error.isEmpty()) { report(error); return; }
    m_reconnectTimer.stop();
    m_connectionUrl = url;
    m_host.connectTo(url);
}

void MainWindow::loadProfiles() {
    m_host.invoke("profile:list", {}, this, [this](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) { report(error); return; }
        m_profiles = value.isArray() ? value.toArray() : value.toObject()["profiles"].toArray();
        const QSignalBlocker blocker(m_profilePicker); m_profilePicker->clear();
        for (const auto &entry : m_profiles) {
            const auto profile = entry.toObject();
            m_profilePicker->addItem(profile["name"].toString(), profile["id"].toString());
        }
        int selected = m_profilePicker->findData(m_profileId);
        if (selected < 0 && m_profilePicker->count()) { selected = 0; m_profileId = m_profilePicker->itemData(0).toString(); }
        m_profilePicker->setCurrentIndex(selected); m_profilePicker->setEnabled(true);
        if (selected >= 0) openProfile(m_profileId);
    }, 30000, false);
}

void MainWindow::openProfile(const QString &id) {
    if (!m_host.ready() || m_loading || m_saveInFlight || m_saveTimer.isActive()) {
        report(tr("Wait for the current workspace save before switching profiles.")); return;
    }
    m_loading = true;
    const QString previousContext = m_host.contextId();
    m_host.invoke("profile:open", {{"profileId", id}}, this,
        [this, id, previousContext](const QJsonValue &value, const QString &error) {
            m_loading = false;
            if (!error.isEmpty()) {
                report(error);
                const QSignalBlocker blocker(m_profilePicker);
                m_profilePicker->setCurrentIndex(m_profilePicker->findData(m_profileId)); return;
            }
            const auto context = value.toObject();
            if (context["status"] != "ready") { report(tr("Profile unavailable")); return; }
            const bool sameBinding = m_contextBinding == context["bindingKey"].toString();
            if (!sameBinding) clearPages();
            m_contextBinding = context["bindingKey"].toString();
            m_host.setContextId(context["contextId"].toString()); m_profileId = id;
            if (!previousContext.isEmpty()) m_host.invoke("profile:close", {{"contextId", previousContext}}, this,
                [](const QJsonValue &, const QString &) {}, 30000, false);
            m_localProfile = false;
            for (const auto &entry : m_profiles) if (entry.toObject()["id"] == id)
                m_localProfile = m_localConnection && entry.toObject()["type"] != "remote";
            loadSettings();
        }, 30000, false);
}

void MainWindow::loadWorkspaces(bool preserveLocalTabs) {
    if (!m_host.ready() || m_host.contextId().isEmpty() || m_saveInFlight) return;
    m_host.invoke("workspace:load", {{"profileId", m_profileId}}, this,
        [this, preserveLocalTabs](const QJsonValue &value, const QString &error) {
            if (!error.isEmpty()) { report(error); return; }
            if (value.isNull() || value.isUndefined()) applySnapshot(QJsonObject{{"workspaces", QJsonArray{}}, {"terminals", QJsonArray{}}}, preserveLocalTabs);
            else {
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(value.toString().toUtf8(), &parseError);
                if (!document.isObject() || parseError.error != QJsonParseError::NoError) {
                    report(tr("Workspace data is invalid; it has not been overwritten.")); return;
                }
                applySnapshot(document.object(), preserveLocalTabs);
            }
            emit workspaceReady();
        });
}

QString MainWindow::defaultProjectName() const {
    QSet<QString> names;
    for (const auto &entry : m_snapshot["qtUi"].toObject()["projects"].toArray())
        names.insert(entry.toObject()["name"].toString().toCaseFolded());
    for (int i = 0; i < 100; ++i) {
        const auto name = QString("Project_%1").arg(i, 2, 10, QLatin1Char('0'));
        if (!names.contains(name.toCaseFolded())) return name;
    }
    return {};
}

void MainWindow::normalizeProjects() {
    auto qt = m_snapshot["qtUi"].toObject();
    QSet<QString> workspaceIds, assigned, projectIds;
    for (const auto &entry : m_snapshot["workspaces"].toArray())
        workspaceIds.insert(entry.toObject()["id"].toString());
    QJsonArray projects;
    for (const auto &entry : qt["projects"].toArray()) {
        auto project = entry.toObject();
        const QString id = project["id"].toString();
        if (id.isEmpty() || projectIds.contains(id)) continue;
        projectIds.insert(id);
        QJsonArray members;
        for (const auto &member : project["workspaceIds"].toArray()) {
            const QString workspaceId = member.toString();
            if (!workspaceIds.contains(workspaceId) || assigned.contains(workspaceId)) continue;
            assigned.insert(workspaceId); members.append(workspaceId);
        }
        project["workspaceIds"] = members; projects.append(project);
    }
    QJsonArray ungrouped;
    for (const auto &entry : m_snapshot["workspaces"].toArray()) {
        const QString id = entry.toObject()["id"].toString();
        if (!assigned.contains(id)) { ungrouped.append(id); assigned.insert(id); }
    }
    if (!ungrouped.isEmpty()) {
        int fallback = -1;
        for (int i = 0; i < projects.size(); ++i)
            if (projects[i].toObject()["id"] == "default-project") fallback = i;
        if (fallback < 0 && !projects.isEmpty() && defaultProjectName().isEmpty()) fallback = 0;
        if (fallback < 0) {
            qt["projects"] = projects; m_snapshot["qtUi"] = qt;
            projects.append(QJsonObject{{"id", "default-project"}, {"name", defaultProjectName()},
                {"workspaceIds", ungrouped}});
        } else {
            auto project = projects[fallback].toObject(); auto members = project["workspaceIds"].toArray();
            for (const auto &member : ungrouped) members.append(member);
            project["workspaceIds"] = members; projects[fallback] = project;
        }
    }
    qt["projects"] = projects; m_snapshot["qtUi"] = qt;
}

void MainWindow::rebuildWorkspaceTree() {
    const QSignalBlocker blocker(m_workspaces);
    m_workspaces->clear();
    const auto projects = m_snapshot["qtUi"].toObject()["projects"].toArray();
    QHash<QString, QJsonObject> workspaces;
    for (const auto &entry : m_snapshot["workspaces"].toArray()) {
        const auto workspace = entry.toObject(); workspaces.insert(workspace["id"].toString(), workspace);
    }
    bool selectedProjectExists = false;
    for (const auto &entry : projects)
        if (entry.toObject()["id"] == m_projectId) selectedProjectExists = true;
    if (!selectedProjectExists) {
        m_projectId.clear();
        for (const auto &entry : projects) {
            const auto project = entry.toObject();
            if (project["workspaceIds"].toArray().contains(m_workspaceId)) m_projectId = project["id"].toString();
        }
        if (m_projectId.isEmpty() && !projects.isEmpty()) m_projectId = projects.first().toObject()["id"].toString();
    }
    for (const auto &entry : projects) {
        const auto project = entry.toObject(); const QString id = project["id"].toString();
        auto *group = new QTreeWidgetItem(m_workspaces);
        group->setFlags(group->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
        group->setData(0, Qt::UserRole, id); group->setText(1, project["name"].toString());
        group->setIcon(1, QIcon::fromTheme("folder"));
        auto font = group->font(1); font.setBold(true); group->setFont(1, font);
        auto *handle = new QToolButton(m_workspaces);
        handle->setText(QStringLiteral("＝"));
        handle->setToolTip(tr("Drag to reorder project"));
        handle->setAccessibleName(tr("Reorder %1").arg(project["name"].toString()));
        handle->setAutoRaise(true);
        handle->setMinimumWidth(24);
        handle->setFocusPolicy(Qt::NoFocus);
        handle->setEnabled(projects.size() > 1);
        if (handle->isEnabled()) handle->setCursor(Qt::OpenHandCursor);
        handle->installEventFilter(m_workspaces);
        m_workspaces->setItemWidget(group, WorkspaceProjectTree::dragHandleColumn, handle);
        const bool expanded = !project["collapsed"].toBool();
        auto *toggle = new QToolButton(m_workspaces);
        toggle->setText(expanded ? "−" : "+");
        toggle->setAccessibleName(tr("Expand or collapse %1").arg(project["name"].toString()));
        toggle->setToolTip(tr("Expand / collapse project")); toggle->setAutoRaise(true);
        m_workspaces->setItemWidget(group, 0, toggle);
        connect(toggle, &QToolButton::clicked, this, [group] { group->setExpanded(!group->isExpanded()); });
        auto *add = new QToolButton(m_workspaces);
        add->setText("+"); add->setToolTip(tr("Add workspace to %1").arg(project["name"].toString()));
        add->setAccessibleName(add->toolTip()); add->setAutoRaise(true);
        m_workspaces->setItemWidget(group, 2, add);
        connect(add, &QToolButton::clicked, this, [this, group] {
            m_workspaces->setCurrentItem(group); addWorkspace();
        });
        if (id == m_projectId) m_workspaces->setCurrentItem(group);
        for (const auto &member : project["workspaceIds"].toArray()) {
            const QString workspaceId = member.toString(); const auto workspace = workspaces.value(workspaceId);
            auto *item = new QTreeWidgetItem(group);
            item->setFlags(item->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
            item->setText(1, workspace["alias"].toString(workspace["name"].toString()));
            item->setData(0, Qt::UserRole, workspaceId);
            item->setToolTip(1, workspace["folderPath"].toString());
            item->setIcon(1, QIcon::fromTheme("utilities-terminal"));
            if (id == m_projectId && workspaceId == m_workspaceId) m_workspaces->setCurrentItem(item);
        }
        group->setExpanded(expanded);
    }
}

bool MainWindow::moveProject(const QString &id, int index) {
    if (m_loading) return false;
    auto qt = m_snapshot["qtUi"].toObject();
    auto projects = qt["projects"].toArray();
    for (int from = 0; from < projects.size(); ++from) {
        if (projects[from].toObject()["id"] != id) continue;
        const int to = index > from ? index - 1 : index;
        if (to == from) return false;

        const auto project = projects.takeAt(from);
        projects.insert(to, project);
        qt["projects"] = projects;
        m_snapshot["qtUi"] = qt;
        rebuildWorkspaceTree();
        scheduleSnapshotSave();
        return true;
    }
    return false;
}

void MainWindow::addProject() {
    if (!hostReady()) { report(tr("Connect to a host first.")); return; }
    if (m_loading || m_saveInFlight) { report(tr("Wait for the current workspace save.")); return; }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Add project"), tr("Project name:"),
        QLineEdit::Normal, defaultProjectName(), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    auto qt = m_snapshot["qtUi"].toObject(); auto projects = qt["projects"].toArray();
    for (const auto &entry : projects) if (entry.toObject()["name"].toString().compare(name, Qt::CaseInsensitive) == 0) {
        report(tr("A project with that name already exists.")); return;
    }
    m_projectId = newId();
    projects.append(QJsonObject{{"id", m_projectId}, {"name", name}, {"workspaceIds", QJsonArray{}}});
    qt["projects"] = projects; qt["activeProjectId"] = m_projectId;
    m_snapshot["qtUi"] = qt; saveSnapshot();
}

void MainWindow::renameProject() {
    if (!hostReady() || m_loading || m_saveInFlight) return;
    auto qt = m_snapshot["qtUi"].toObject(); auto projects = qt["projects"].toArray();
    const QString id = m_projectId;
    int index = -1;
    for (int i = 0; i < projects.size(); ++i) if (projects[i].toObject()["id"] == id) index = i;
    if (index < 0) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename project"), tr("Project name:"),
        QLineEdit::Normal, projects[index].toObject()["name"].toString(), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    for (int i = 0; i < projects.size(); ++i)
        if (i != index && projects[i].toObject()["name"].toString().compare(name, Qt::CaseInsensitive) == 0) {
            report(tr("A project with that name already exists.")); return;
        }
    auto project = projects[index].toObject(); project["name"] = name; projects[index] = project;
    qt["projects"] = projects; m_snapshot["qtUi"] = qt; saveSnapshot();
}

void MainWindow::removeProject() {
    if (!hostReady() || m_loading) return;
    const QString id = m_projectId;
    const QString context = m_host.contextId();
    QJsonObject selected;
    for (const auto &entry : m_snapshot["qtUi"].toObject()["projects"].toArray())
        if (entry.toObject()["id"] == id) selected = entry.toObject();
    if (selected.isEmpty()) return;
    QMessageBox confirmation(QMessageBox::Question, tr("Remove project"),
        tr("Remove project \"%1\" and its %2 workspace(s)? Their Qt tabs will close and agent sessions will stop. Files will remain on disk.")
            .arg(selected["name"].toString()).arg(selected["workspaceIds"].toArray().size()),
        QMessageBox::Yes | QMessageBox::No, this);
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.setDefaultButton(QMessageBox::No);
    if (confirmation.exec() != QMessageBox::Yes) return;
    // The modal dialog processes host events. Re-read current state and never
    // apply a confirmation to another profile. Selection autosaves can start
    // while the menu or dialog is open; saveSnapshot queues this removal after
    // any pending save and prevents its older reply from restoring the project.
    if (!hostReady() || m_host.contextId() != context || m_loading) return;
    auto qt = m_snapshot["qtUi"].toObject(); auto projects = qt["projects"].toArray();
    int index = -1;
    for (int i = 0; i < projects.size(); ++i)
        if (projects[i].toObject()["id"] == id) index = i;
    if (index < 0) return;
    QSet<QString> removed;
    for (const auto &member : projects[index].toObject()["workspaceIds"].toArray()) removed.insert(member.toString());
    projects.removeAt(index);
    auto workspaces = m_snapshot["workspaces"].toArray();
    for (int i = workspaces.size() - 1; i >= 0; --i)
        if (removed.contains(workspaces[i].toObject()["id"].toString())) workspaces.removeAt(i);
    for (const auto &workspaceId : removed) {
        auto *tabs = m_workspaceTabs.take(workspaceId);
        if (!tabs) continue;
        for (int i = 0; i < tabs->count(); ++i)
            if (auto *agent = qobject_cast<AgentPanel *>(tabs->widget(i))) agent->stopSession();
        m_pages->removeWidget(tabs); delete tabs;
    }
    if (removed.contains(m_workspaceToInitialize)) m_workspaceToInitialize.clear();
    if (removed.contains(m_workspaceId))
        m_workspaceId = workspaces.isEmpty() ? QString() : workspaces.first().toObject()["id"].toString();
    m_projectId = projects.isEmpty() ? QString() : projects.first().toObject()["id"].toString();
    for (const auto &entry : projects)
        if (entry.toObject()["workspaceIds"].toArray().contains(m_workspaceId)) m_projectId = entry.toObject()["id"].toString();
    qt["projects"] = projects; qt["activeProjectId"] = m_projectId;
    m_snapshot["qtUi"] = qt; m_snapshot["workspaces"] = workspaces;
    if (removed.contains(m_snapshot["activeWorkspaceId"].toString())) m_snapshot["activeWorkspaceId"] = m_workspaceId;
    showWorkspace(m_workspaceId, false);
    rebuildWorkspaceTree();
    saveSnapshot();
}

void MainWindow::applySnapshot(const QJsonObject &snapshot, bool preserveLocalTabs) {
    m_loading = true;
    m_snapshot = snapshot;
    normalizeProjects();
    QSet<QString> ids;
    for (const auto &entry : snapshot["workspaces"].toArray()) {
        const auto workspace = entry.toObject(); const QString id = workspace["id"].toString();
        ids.insert(id);
        if (!m_workspaceTabs.contains(id)) {
            auto *tabs = new QTabWidget(m_pages); tabs->setTabsClosable(true); tabs->setMovable(true);
            tabs->setDocumentMode(true); m_pages->addWidget(tabs); m_workspaceTabs[id] = tabs;
            connect(tabs, &QTabWidget::tabCloseRequested, this, [this, tabs](int index) { closeTab(tabs, index); });
            connect(tabs, &QTabWidget::currentChanged, this, [this] { if (!m_loading) scheduleSnapshotSave(); });
        }
    }
    for (const auto &id : m_workspaceTabs.keys()) if (!ids.contains(id)) {
        auto *tabs = m_workspaceTabs.take(id); m_pages->removeWidget(tabs); delete tabs;
    }
    // Qt descriptors are additive. Preserve all legacy terminal records and
    // unknown workspace fields so opening this branch never drops saved data.
    const auto qt = snapshot["qtUi"].toObject();
    const auto savedTabs = qt["tabs"].toArray();
    QSet<QString> savedIds;
    for (const auto &entry : savedTabs) savedIds.insert(entry.toObject()["id"].toString());
    // Legacy clients and snapshots do not carry Qt tabs. Only an explicit
    // Qt tab list can remove local sessions; absence is not an empty list.
    // Rolling back a rejected save must not terminate unsaved local shells.
    if (qt["tabs"].isArray() && !preserveLocalTabs) for (auto *tabs : std::as_const(m_workspaceTabs)) {
        for (int i = tabs->count() - 1; i >= 0; --i) {
            auto *widget = tabs->widget(i);
            if (!savedIds.contains(widget->property("tabId").toString())) {
                tabs->removeTab(i); delete widget;
            }
        }
    }
    for (const auto &entry : savedTabs) createTab(entry.toObject(), false);
    for (auto *tabs : std::as_const(m_workspaceTabs)) for (int i = 0; i < tabs->count(); ++i) {
        if (auto *agent = qobject_cast<AgentPanel *>(tabs->widget(i))) agent->attach();
    }
    QString selected = qt["activeWorkspaceId"].toString(snapshot["activeWorkspaceId"].toString());
    const auto workspaces = snapshot["workspaces"].toArray();
    if (!ids.contains(selected)) selected = workspaces.isEmpty() ? QString() : workspaces.first().toObject()["id"].toString();
    m_projectId = m_snapshot["qtUi"].toObject()["activeProjectId"].toString();
    showWorkspace(selected, false);
    rebuildWorkspaceTree();
    m_loading = false;
    report(tr("Ready · %1 · Konsole is the default terminal").arg(m_profilePicker->currentText()));
}

void MainWindow::showWorkspace(const QString &id, bool save) {
    const bool changed = m_workspaceId != id;
    m_workspaceId = id;
    if (save) {
        auto qt = m_snapshot["qtUi"].toObject(); qt["activeWorkspaceId"] = id; m_snapshot["qtUi"] = qt;
    }
    if (auto *tabs = m_workspaceTabs.value(id)) m_pages->setCurrentWidget(tabs);
    else m_pages->setCurrentIndex(0);
    setWindowTitle(tr("%1 — Better Agent Terminal Qt").arg(activeWorkspace()["name"].toString("Workspaces")));
    m_gitPanel->setWorkspace(currentDirectory());
    if (m_filesContext != m_host.contextId() || m_filesDirectory != currentDirectory()) refreshFiles();
    if (save && changed && !m_loading) scheduleSnapshotSave();
}
QJsonObject MainWindow::activeWorkspace() const {
    for (const auto &entry : m_snapshot["workspaces"].toArray()) if (entry.toObject()["id"] == m_workspaceId) return entry.toObject();
    return {};
}
QString MainWindow::currentDirectory() const { return activeWorkspace()["folderPath"].toString(); }

void MainWindow::addWorkspace() {
    if (!hostReady()) { report(tr("Connect to a host first.")); return; }
    if (m_loading || m_saveInFlight) { report(tr("Wait for the current workspace save.")); return; }
    const QString projectId = m_projectId;
    const QString context = m_host.contextId();
    QString directory;
    if (m_localProfile) directory = QFileDialog::getExistingDirectory(this, tr("Workspace folder"), QDir::homePath());
    else directory = QInputDialog::getText(this, tr("Remote workspace"), tr("Folder path on the host:"));
    if (directory.isEmpty()) return;
    m_host.invoke("fs:isDirectory", {{"path", directory}}, this,
        [this, directory, projectId, context](const QJsonValue &value, const QString &error) {
            if (context != m_host.contextId()) return;
            if (!error.isEmpty()) { report(error); return; }
            if (!value.toBool()) { report(tr("The host cannot find that directory.")); return; }
            if (m_saveInFlight) { report(tr("Wait for the current workspace save.")); return; }
            const QString id = newId();
            auto workspaces = m_snapshot["workspaces"].toArray();
            workspaces.append(QJsonObject{{"id", id}, {"name", QFileInfo(directory).fileName()},
                {"folderPath", directory}, {"createdAt", QDateTime::currentMSecsSinceEpoch()}});
            m_snapshot["workspaces"] = workspaces;
            auto qt = m_snapshot["qtUi"].toObject(); auto projects = qt["projects"].toArray();
            QString targetProject;
            for (int i = 0; i < projects.size(); ++i) {
                auto project = projects[i].toObject();
                if (project["id"] != projectId) continue;
                auto members = project["workspaceIds"].toArray(); members.append(id);
                project["workspaceIds"] = members; project["collapsed"] = false; projects[i] = project;
                targetProject = projectId;
            }
            if (targetProject.isEmpty() && !projects.isEmpty()) {
                auto project = projects[0].toObject();
                targetProject = project["id"].toString();
                auto members = project["workspaceIds"].toArray(); members.append(id);
                project["workspaceIds"] = members; project["collapsed"] = false; projects[0] = project;
            }
            if (targetProject.isEmpty()) {
                targetProject = newId();
                projects.append(QJsonObject{{"id", targetProject}, {"name", defaultProjectName()}, {"workspaceIds", QJsonArray{id}}});
            }
            qt["projects"] = projects; qt["activeProjectId"] = targetProject;
            qt["activeWorkspaceId"] = id; m_snapshot["qtUi"] = qt;
            m_workspaceToInitialize = id;
            saveSnapshot();
        });
}
void MainWindow::renameWorkspace() {
    if (!hostReady() || currentDirectory().isEmpty()) return;
    const QString name = QInputDialog::getText(this, tr("Rename workspace"), tr("Name:"), QLineEdit::Normal, activeWorkspace()["name"].toString());
    if (name.trimmed().isEmpty()) return;
    auto workspaces = m_snapshot["workspaces"].toArray();
    for (int i = 0; i < workspaces.size(); ++i) {
        auto workspace = workspaces[i].toObject();
        if (workspace["id"] == m_workspaceId) { workspace["name"] = name; workspace.remove("alias"); workspaces[i] = workspace; }
    }
    m_snapshot["workspaces"] = workspaces; saveSnapshot();
}
void MainWindow::removeWorkspace() {
    if (!hostReady() || currentDirectory().isEmpty()) return;
    if (QMessageBox::question(this, tr("Remove workspace"), tr("Remove this workspace and close its Qt tabs? Files will remain on disk.")) != QMessageBox::Yes) return;
    auto workspaces = m_snapshot["workspaces"].toArray();
    for (int i = workspaces.size() - 1; i >= 0; --i) if (workspaces[i].toObject()["id"] == m_workspaceId) workspaces.removeAt(i);
    m_snapshot["workspaces"] = workspaces;
    auto *tabs = m_workspaceTabs.take(m_workspaceId);
    if (tabs) {
        for (int i = 0; i < tabs->count(); ++i) if (auto *agent = qobject_cast<AgentPanel *>(tabs->widget(i))) agent->stopSession();
        m_pages->removeWidget(tabs); delete tabs;
    }
    m_workspaceId.clear(); saveSnapshot();
}

void MainWindow::addTab(const QString &preset) {
    if (!hostReady() || currentDirectory().isEmpty()) { report(tr("Select a workspace first.")); return; }
    if ((preset == "none" || preset.endsWith("-cli")) && !m_localProfile) {
        report(tr("Embedded terminals are available for local workspaces. Use agent chat for remote profiles.")); return;
    }
    const bool terminal = preset == "none" || preset.endsWith("-cli");
    QJsonObject descriptor{{"id", newId()}, {"workspaceId", m_workspaceId}, {"cwd", currentDirectory()},
        {"kind", terminal ? "terminal" : "agent"}, {"agentPreset", preset},
        {"title", preset == "none" ? "Konsole" : preset.startsWith("codex") ? "Codex" : "Claude"},
        {"permissionMode", "default"}};
    if (terminal) {
        const auto qt = m_settings["qtUi"].toObject();
        QJsonObject terminalSettings;
        for (const auto &key : {"terminalBackend", "terminalExecutable", "terminalArguments", "terminalCommandArguments", "konsoleProfile"})
            if (qt.contains(key)) terminalSettings.insert(key, qt[key]);
        descriptor.insert("terminalSettings", terminalSettings);
        if (preset == "none" && qt["terminalBackend"] == "external")
            descriptor.insert("title", QFileInfo(qt["terminalExecutable"].toString()).fileName());
    }
    if (!terminal) {
        const bool codex = preset.startsWith("codex");
        const QString model = m_settings[codex ? "defaultCodexModel" : "defaultClaudeModel"].toString();
        const QString effort = m_settings[codex ? "defaultCodexEffort" : "defaultEffort"].toString();
        if (!model.isEmpty()) descriptor.insert("model", model);
        if (!effort.isEmpty()) descriptor.insert("effort", effort);
    }
    createTab(descriptor);
}

void MainWindow::createTab(const QJsonObject &descriptor, bool save) {
    auto *tabs = m_workspaceTabs.value(descriptor["workspaceId"].toString());
    if (!tabs) return;
    const QString id = descriptor["id"].toString(); if (id.isEmpty()) return;
    for (int i = 0; i < tabs->count(); ++i) if (tabs->widget(i)->property("tabId").toString() == id) return;
    QWidget *panel;
    if (descriptor["kind"] == "terminal") {
        if (!m_localProfile) return;
        const QString preset = descriptor["agentPreset"].toString();
        QString program;
        QStringList arguments;
        if (preset == "claude-cli" || preset == "codex-cli") {
            // env resolves the user's installed CLI using PATH, without a shell command string.
            program = "/usr/bin/env"; arguments = {preset == "claude-cli" ? "claude" : "codex"};
        }
        // Legacy descriptors always remain Konsole tabs. New descriptors save
        // their own backend so a settings change cannot replace a live shell.
        const auto terminalSettings = descriptor["terminalSettings"].toObject();
        auto *terminal = new TerminalPanel(descriptor["cwd"].toString(), program, arguments, tabs,
            terminalSettings["konsoleProfile"].toString(m_settings["qtUi"].toObject()["konsoleProfile"].toString()), terminalSettings);
        connect(terminal, &TerminalPanel::exited, this, [this, tabs, terminal] {
            if (m_closing) return;
            if (m_settings["closeTerminalAfterProcessExit"].toBool()) {
                const int index = tabs->indexOf(terminal);
                if (index >= 0) { tabs->removeTab(index); terminal->deleteLater(); if (!m_loading) scheduleSnapshotSave(); }
                return;
            }
            const int index = tabs->indexOf(terminal); if (index >= 0) tabs->setTabText(index, QObject::tr("Exited"));
        });
        panel = terminal;
    } else {
        auto *agent = new AgentPanel(&m_host, descriptor, tabs); panel = agent;
        connect(agent, &AgentPanel::descriptorChanged, this, [this] { if (!m_loading) scheduleSnapshotSave(); });
        connect(agent, &AgentPanel::attentionRequired, this, [this](const QString &title) {
            QApplication::alert(this); report(title);
        });
        agent->attach();
    }
    panel->setProperty("tabId", id); panel->setProperty("descriptor", descriptor.toVariantMap());
    const int index = tabs->addTab(panel, descriptor["title"].toString());
    tabs->setTabToolTip(index, descriptor["cwd"].toString());
    if (save) { tabs->setCurrentIndex(index); scheduleSnapshotSave(); }
}

void MainWindow::closeTab(QTabWidget *tabs, int index) {
    auto *panel = tabs->widget(index); if (!panel) return;
    auto *agent = qobject_cast<AgentPanel *>(panel);
    if ((!agent || agent->busy()) && QMessageBox::question(this, tr("Close tab"),
        tr("Close this tab and stop its session?")) != QMessageBox::Yes) return;
    if (agent) agent->stopSession();
    tabs->removeTab(index); delete panel; scheduleSnapshotSave();
}

void MainWindow::scheduleSnapshotSave() {
    if (m_loading) return;
    // Mark edits immediately; the reply can arrive before the debounce fires.
    if (m_saveInFlight) m_saveAgain = true;
    m_saveTimer.start();
}

void MainWindow::saveSnapshot() {
    if (!hostReady() || m_loading) return;
    if (m_saveInFlight) { m_saveAgain = true; return; }
    m_saveTimer.stop(); m_saveAgain = false;
    m_saveInFlight = true;
    QJsonArray descriptors;
    for (auto *tabs : std::as_const(m_workspaceTabs)) for (int i = 0; i < tabs->count(); ++i) {
        auto *panel = tabs->widget(i);
        auto descriptor = QJsonObject::fromVariantMap(panel->property("descriptor").toMap());
        if (auto *agent = qobject_cast<AgentPanel *>(panel)) descriptor = agent->descriptor();
        descriptors.append(descriptor);
    }
    auto qt = m_snapshot["qtUi"].toObject(); qt["tabs"] = descriptors;
    qt["activeWorkspaceId"] = m_workspaceToInitialize.isEmpty() ? m_workspaceId : m_workspaceToInitialize;
    m_snapshot["qtUi"] = qt;
    const QJsonObject proposed = m_snapshot;
    const QString data = QString::fromUtf8(QJsonDocument(proposed).toJson(QJsonDocument::Compact));
    m_host.invoke("workspace:save", {{"profileId", m_profileId}, {"data", data}}, this,
        [this, proposed](const QJsonValue &, const QString &error) {
            m_saveInFlight = false;
            if (!error.isEmpty()) {
                m_saveAgain = false; m_saveTimer.stop(); m_workspaceToInitialize.clear();
                report(error); loadWorkspaces(true); return;
            }
            if (m_saveAgain) { m_saveAgain = false; saveSnapshot(); }
            else {
                applySnapshot(proposed);
                const QString created = std::exchange(m_workspaceToInitialize, {});
                if (!created.isEmpty() && m_workspaceId == created) {
                    if (m_localProfile) {
                        const int count = qBound(1, m_settings["defaultTerminalCount"].toInt(1), 10);
                        for (int i = 0; i < count; ++i) addTab("none");
                    }
                    if (m_settings["createDefaultAgentTerminal"].toBool()) addTab(m_settings["defaultAgent"].toString("claude-code"));
                }
            }
        });
}

void MainWindow::refreshFiles() {
    const QString directory = currentDirectory();
    const bool changed = m_filesContext != m_host.contextId() || m_filesDirectory != directory;
    m_filesContext = m_host.contextId();
    m_filesDirectory = directory;
    ++m_filesGeneration;
    // A new workspace must not display the previous folder. For a refresh of
    // the same folder, keep the current tree until its replacement arrives.
    if (changed || !hostReady() || directory.isEmpty()) m_files->clear();
    if (!hostReady() || directory.isEmpty()) return;
    loadDirectory(nullptr, currentDirectory());
}
void MainWindow::loadDirectory(QTreeWidgetItem *parent, const QString &path) {
    const QString context = m_host.contextId(); const QString workspace = m_workspaceId;
    const quint64 generation = m_filesGeneration;
    // Tree items are not QObjects. Re-find by path after asynchronous reads,
    // rather than capturing a pointer that a workspace switch could delete.
    m_host.invoke("fs:readdir", {{"path", path}}, this,
        [this, context, workspace, path, generation, nested = parent != nullptr](const QJsonValue &value, const QString &error) {
            if (context != m_host.contextId() || workspace != m_workspaceId || generation != m_filesGeneration) return;
            if (!error.isEmpty()) { report(error); return; }
            // Apply a complete reply in one paint, rather than showing an
            // empty tree during the network round trip.
            m_files->setUpdatesEnabled(false);
            QTreeWidgetItem *target = nullptr;
            if (nested) {
                QTreeWidgetItemIterator it(m_files);
                while (*it) { if ((*it)->data(0, Qt::UserRole).toString() == path) { target = *it; break; } ++it; }
                if (!target) { m_files->setUpdatesEnabled(true); return; }
                while (target->childCount()) delete target->takeChild(0);
            } else m_files->clear();
            for (const auto &entry : value.toArray()) {
                const auto file = entry.toObject();
                auto *item = target ? new QTreeWidgetItem(target) : new QTreeWidgetItem(m_files);
                const bool directory = file["isDirectory"].toBool(file["is_directory"].toBool());
                item->setText(0, file["name"].toString()); item->setData(0, Qt::UserRole, file["path"]);
                item->setData(0, Qt::UserRole + 1, directory);
                item->setIcon(0, QIcon::fromTheme(directory ? "folder" : "text-x-generic"));
                if (directory) new QTreeWidgetItem(item, {tr("Loading…")});
            }
            m_files->setUpdatesEnabled(true);
        });
}
void MainWindow::previewFile(const QString &path) {
    const QString context = m_host.contextId();
    m_host.invoke("fs:readFile", {{"path", path}}, this,
        [this, context, path](const QJsonValue &value, const QString &error) {
            if (context != m_host.contextId()) return;
            if (!error.isEmpty()) { report(error); return; }
            const QString text = value.toString();
            if (path.endsWith(".md")) m_preview->setMarkdown(text);
            else m_preview->setPlainText(text);
            if (auto *dock = qobject_cast<QDockWidget *>(m_preview->parentWidget())) { dock->setWindowTitle(path); dock->show(); }
        });
}
void MainWindow::showGit() {
    if (m_workspaceViews->count() == 1) m_workspaceViews->addTab(tr("Git"));
    if (m_workspaceViews->currentIndex() == 1) m_gitPanel->refresh();
    else m_workspaceViews->setCurrentIndex(1);
}
void MainWindow::showSettings() {
    QStringList profiles;
    for (auto *terminal : findChildren<TerminalPanel *>()) {
        profiles = terminal->availableProfiles();
        if (!profiles.isEmpty()) break;
    }
    auto *dialog = new SettingsDialog(&m_host, profiles, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &SettingsDialog::settingsSaved, this, &MainWindow::applySettings);
    dialog->show();
}
void MainWindow::loadSettings() {
    const QString context = m_host.contextId();
    m_settings = {};
    m_host.invoke("settings:load", {}, this, [this, context](const QJsonValue &value, const QString &error) {
        if (context != m_host.contextId()) return;
        QJsonObject settings; QString parseError;
        if (!error.isEmpty() || !SettingsDialog::decodeSettings(value, settings, parseError)) {
            report(error.isEmpty() ? parseError : error);
            loadWorkspaces(); return;
        }
        applySettings(settings);
        loadWorkspaces();
    });
}
void MainWindow::applySettings(const QJsonObject &settings) {
    m_settings = settings;
    const auto qt = settings["qtUi"].toObject();
    QFont font = QApplication::font();
    if (!qt["fontFamily"].toString().isEmpty()) font.setFamily(qt["fontFamily"].toString());
    if (qt.contains("fontSize")) font.setPointSize(qBound(8, qt["fontSize"].toInt(10), 32));
    setFont(font);
    QPalette palette = QApplication::palette();
    const QString theme = qt["theme"].toString("system");
    if (theme == "dark" || theme == "light") {
        const bool dark = theme == "dark";
        const QColor background(dark ? "#25272b" : "#f3f4f5");
        const QColor base(dark ? "#181a1e" : "#ffffff");
        const QColor text(dark ? "#e6e8eb" : "#202124");
        for (const auto role : {QPalette::Window, QPalette::Button, QPalette::AlternateBase}) palette.setColor(role, background);
        palette.setColor(QPalette::Base, base);
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(role, text);
        palette.setColor(QPalette::Highlight, QColor("#3d7edb"));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(dark ? "#858990" : "#8c9096"));
    }
    setPalette(palette);
}
void MainWindow::showHistory() {
    if (!hostReady() || currentDirectory().isEmpty()) return;
    auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Session history")); dialog->resize(700, 450);
    auto *layout = new QVBoxLayout(dialog); auto *list = new QListWidget(dialog); layout->addWidget(list);
    const QString cwd = currentDirectory(); const QString workspaceId = m_workspaceId;
    m_host.invoke("agent:list-sessions", {{"cwd", cwd}}, dialog,
        [list](const QJsonValue &value, const QString &error) {
            if (!error.isEmpty()) { new QListWidgetItem(error, list); return; }
            const QJsonArray sessions = value.isArray() ? value.toArray() : value.toObject()["sessions"].toArray();
            for (const auto &entry : sessions) {
                const auto session = entry.toObject();
                auto *item = new QListWidgetItem(session["summary"].toString(session["firstPrompt"].toString(session["id"].toString(session["sessionId"].toString()))), list);
                item->setData(Qt::UserRole, session.toVariantMap());
            }
        });
    connect(list, &QListWidget::itemDoubleClicked, dialog, [this, dialog, cwd, workspaceId](QListWidgetItem *item) {
        const auto session = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
        const QString sdkId = session["sessionId"].toString(session["id"].toString());
        if (sdkId.isEmpty()) return;
        createTab({{"id", newId()}, {"workspaceId", workspaceId}, {"cwd", cwd}, {"kind", "agent"},
            {"agentPreset", session["agentPreset"].toString("claude-code")}, {"sdkSessionId", sdkId},
            {"title", "Resumed session"}, {"permissionMode", "default"}});
        dialog->accept();
    });
    dialog->show();
}
void MainWindow::showConnectionDialog() {
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Connect to BAT host"),
        tr("Paste the connection URL from the host:"), QLineEdit::Password, {}, &ok);
    if (!ok || text.isEmpty()) return;
    const QUrl url(text); const QString error = HostClient::validateConnectionUrl(url);
    if (!error.isEmpty()) { QMessageBox::warning(this, tr("Connection URL"), error); return; }
    if (hasRunningAgents() && QMessageBox::question(this, tr("Change host"),
        tr("Leave running sessions on the current host and connect to another?")) != QMessageBox::Yes) return;
    m_saveTimer.stop(); clearPages(); m_contextBinding.clear();
    m_localConnection = false; connectToHost(url);
}
void MainWindow::report(const QString &message) { statusBar()->showMessage(message); }
bool MainWindow::hasRunningAgents() const {
    for (auto *tabs : m_workspaceTabs) for (int i = 0; i < tabs->count(); ++i)
        if (auto *agent = qobject_cast<AgentPanel *>(tabs->widget(i)); agent && agent->busy()) return true;
    return false;
}
void MainWindow::clearPages() {
    m_loading = true;
    for (auto *tabs : std::as_const(m_workspaceTabs)) { m_pages->removeWidget(tabs); delete tabs; }
    m_workspaceTabs.clear(); m_workspaces->clear(); m_files->clear();
    m_gitPanel->setWorkspace({});
    m_filesContext.clear(); m_filesDirectory.clear(); ++m_filesGeneration;
    m_workspaceId.clear(); m_projectId.clear(); m_workspaceToInitialize.clear(); m_snapshot = {}; m_loading = false;
}
void MainWindow::closeEvent(QCloseEvent *event) {
    const bool hasTerminals = !findChildren<TerminalPanel *>().isEmpty();
    if (!m_closing && (hasRunningAgents() || hasTerminals) && QMessageBox::question(this, tr("Quit"),
        tr("Quit and close local terminals? Agent sessions on an external host will remain there.")) != QMessageBox::Yes) {
        event->ignore(); return;
    }
    if (m_saveTimer.isActive() || m_saveInFlight) {
        m_saveTimer.stop(); saveSnapshot();
        report(tr("Saving workspaces before closing…"));
        // Finish asynchronously instead of blocking the UI/network event loop.
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, timer] {
            if (m_saveInFlight) return;
            timer->stop(); timer->deleteLater();
            m_closing = true; close();
        });
        timer->start(100); event->ignore(); return;
    }
    QSettings preferences; preferences.setValue("geometry", saveGeometry()); preferences.setValue("windowState", saveState());
    m_closing = true; m_reconnectTimer.stop(); event->accept();
    // Hiding the last window can leave the event loop alive while KDE jobs
    // hold quit locks. Explicit Quit must reach our destructor and stop the
    // owned host and terminals. Queue it until this close event has finished.
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}
