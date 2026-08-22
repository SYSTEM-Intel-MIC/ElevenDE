#include "mainwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileIconProvider>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "filelist.h"
#include "thispc.h"

static const QString kPcPath = QStringLiteral("__thispc__");

static QIcon win11Icon(const QString &name, const QString &category,
                       QStyle::StandardPixmap fallback, QObject *owner = nullptr)
{
    Q_UNUSED(owner);
    const QString base = QStringLiteral("/usr/local/share/elevende-shell/icons/");
    const QStringList candidates = {
        base + QStringLiteral("64x64/") + category + QLatin1Char('/') + name + QStringLiteral(".png"),
        base + QStringLiteral("48x48/") + category + QLatin1Char('/') + name + QStringLiteral(".png"),
        base + QStringLiteral("scalable/") + category + QLatin1Char('/') + name + QStringLiteral(".svg")
    };
    for (const QString &path : candidates)
        if (QFile::exists(path))
            return QIcon(path);
    return QApplication::style()->standardIcon(fallback);
}

static QIcon win11AppIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    return win11Icon(name, QStringLiteral("apps"), fallback);
}

static QIcon win11PlaceIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    return win11Icon(name, QStringLiteral("places"), fallback);
}

static bool crashAnticsEnabled()
{
    return qEnvironmentVariable("EXPLORER_CRASH_RESTART") == QLatin1String("1");
}

MainWindow::MainWindow()
    : m_thisPc(nullptr)
    , m_files(nullptr)
    , m_stack(nullptr)
    , m_sidebar(nullptr)
    , m_sidebarExtra(nullptr)
    , m_titleLabel(nullptr)
    , m_backBtn(nullptr)
    , m_forwardBtn(nullptr)
    , m_upBtn(nullptr)
    , m_breadcrumbHost(nullptr)
    , m_breadcrumbLayout(nullptr)
    , m_navStack(nullptr)
    , m_address(nullptr)
    , m_search(nullptr)
    , m_status(nullptr)
    , m_navigationCount(0)
{
    setWindowTitle(QStringLiteral("此电脑"));
    setWindowIcon(win11AppIcon(QStringLiteral("system-file-manager"), QStyle::SP_FileDialogContentsView));
    resize(1280, 800);
    setMinimumSize(720, 480);

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("ExplorerSurface"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    /* single titlebar: let the window manager (Openbox) own the frame, so
       the app's fake in-window titlebar is not shown on top of it */
    root->addWidget(buildCommandBar());

    auto *middle = new QWidget(central);
    auto *middleLayout = new QHBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->setSpacing(0);
    middleLayout->addWidget(buildSidebar());

    m_stack = new QStackedWidget(middle);
    m_thisPc = new ThisPcView(m_stack);
    m_thisPc->setObjectName(QStringLiteral("ExplorerSurface"));
    m_files = new FileList(m_stack);
    m_files->setObjectName(QStringLiteral("ExplorerSurface"));
    m_stack->addWidget(m_thisPc);
    m_stack->addWidget(m_files);
    middleLayout->addWidget(m_stack, 1);
    root->addWidget(middle, 1);

    m_status = statusBar();
    m_status->setObjectName(QStringLiteral("StatusBar"));
    m_status->setSizeGripEnabled(false);
    m_status->showMessage(QStringLiteral("就绪"));

    setCentralWidget(central);

    connect(m_thisPc, &ThisPcView::driveActivated, this, &MainWindow::navigateTo);
    connect(m_files, &FileList::directoryActivated, this, &MainWindow::navigateTo);
    connect(m_files, &FileList::statusInfo, this,
            [this](const QString &text) { m_status->showMessage(text); });
    connect(m_sidebar, &QListWidget::itemActivated, this, &MainWindow::onSidebarActivated);
    connect(m_sidebar, &QListWidget::itemClicked, this, &MainWindow::onSidebarActivated);
    connect(m_sidebarExtra, &QListWidget::itemActivated, this, &MainWindow::onSidebarActivated);
    connect(m_sidebarExtra, &QListWidget::itemClicked, this, &MainWindow::onSidebarActivated);

    m_breadcrumbHost->installEventFilter(this);
    m_address->installEventFilter(this);

    m_notResponsive = new NotRespondingManager(this, this);

    navigateTo(kPcPath);
}

void MainWindow::openIn(const QString &path)
{
    QString target = path.trimmed();
    if (target.isEmpty() || target == QStringLiteral("/"))
        return;
    navigateTo(target);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_breadcrumbHost && event->type() == QEvent::MouseButtonPress) {
        enterAddressMode();
        return true;
    }
    if (obj == m_address && event->type() == QEvent::KeyPress) {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            leaveAddressMode();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

QWidget *MainWindow::buildTitleBar()
{
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("TitleBar"));
    bar->setFixedHeight(36);

    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 0, 0, 0);
    layout->setSpacing(8);

    auto *icon = new QLabel(bar);
    icon->setPixmap(win11PlaceIcon(QStringLiteral("desktop-this-pc"), QStyle::SP_ComputerIcon).pixmap(18, 18));
    layout->addWidget(icon);

    m_titleLabel = new QLabel(bar);
    m_titleLabel->setObjectName(QStringLiteral("TitleLabel"));
    layout->addWidget(m_titleLabel);

    layout->addStretch();

    auto makeBtn = [&](const QString &glyph, const char *name, const QString &tip) {
        auto *b = new QPushButton(glyph, bar);
        b->setObjectName(QString::fromLatin1(name));
        b->setFixedSize(44, 36);
        b->setToolTip(tip);
        b->setCursor(Qt::ArrowCursor);
        return b;
    };
    QPushButton *minBtn = makeBtn(QStringLiteral("\u2014"), "TitleBtn", tr("最小化"));
    QPushButton *maxBtn = makeBtn(QStringLiteral("\u2610"), "TitleBtn", tr("最大化"));
    QPushButton *closeBtn = makeBtn(QStringLiteral("\u2715"), "TitleBtnClose", tr("关闭"));

    connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(maxBtn, &QPushButton::clicked, this, [this]() {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        close();
    });

    layout->addWidget(minBtn);
    layout->addWidget(maxBtn);
    layout->addWidget(closeBtn);
    return bar;
}

QWidget *MainWindow::buildCommandBar()
{
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("CommandBar"));
    bar->setFixedHeight(78);

    auto *root = new QVBoxLayout(bar);
    root->setContentsMargins(10, 5, 10, 4);
    root->setSpacing(4);

    auto *commands = new QHBoxLayout;
    commands->setContentsMargins(0, 0, 0, 0);
    commands->setSpacing(2);
    auto command = [&](const QString &text, const QString &iconName,
                        QStyle::StandardPixmap fallback, const QString &tip) {
        auto *b = new QToolButton(bar);
        b->setObjectName(QStringLiteral("CommandButton"));
        b->setText(text);
        b->setIcon(win11AppIcon(iconName, fallback));
        b->setIconSize(QSize(22, 22));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setAutoRaise(true);
        return b;
    };

    QToolButton *newBtn = command(tr("新建"), QStringLiteral("folder"), QStyle::SP_FileDialogNewFolder, tr("新建文件夹或文本文档"));
    QMenu *newMenu = new QMenu(newBtn);
    QAction *newFolder = newMenu->addAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder), tr("文件夹"));
    QAction *newText = newMenu->addAction(style()->standardIcon(QStyle::SP_FileIcon), tr("文本文档"));
    newBtn->setMenu(newMenu);
    /* Use one generous hit target instead of a tiny split-arrow zone. */
    newBtn->setPopupMode(QToolButton::InstantPopup);
    commands->addWidget(newBtn);

    auto separator = [&]() {
        auto *line = new QFrame(bar);
        line->setFrameShape(QFrame::VLine);
        line->setObjectName(QStringLiteral("CommandSeparator"));
        line->setFixedHeight(26);
        return line;
    };
    commands->addWidget(separator());
    QToolButton *cutBtn = command(QString(), QStringLiteral("edit-cut"), QStyle::SP_ArrowLeft, tr("剪切所选项目 (Ctrl+X)"));
    QToolButton *copyBtn = command(QString(), QStringLiteral("edit-copy"), QStyle::SP_FileDialogContentsView, tr("复制所选项目 (Ctrl+C)"));
    QToolButton *pasteBtn = command(QString(), QStringLiteral("edit-paste"), QStyle::SP_DialogApplyButton, tr("粘贴到当前文件夹 (Ctrl+V)"));
    QToolButton *renameBtn = command(QString(), QStringLiteral("edit-rename"), QStyle::SP_FileDialogDetailedView, tr("重命名所选项目 (F2)"));
    QToolButton *shareBtn = command(QString(), QStringLiteral("explorer-share"), QStyle::SP_DialogOpenButton, tr("共享"));
    QToolButton *deleteBtn = command(QString(), QStringLiteral("user-trash"), QStyle::SP_TrashIcon, tr("移入回收站 (Delete)"));
    const QList<QToolButton *> compactCommands = { cutBtn, copyBtn, pasteBtn, renameBtn, shareBtn, deleteBtn };
    for (QToolButton *button : compactCommands) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(22, 22));
        button->setFixedSize(38, 32);
        commands->addWidget(button);
    }
    commands->addWidget(separator());

    QToolButton *viewBtn = command(tr("查看"), QStringLiteral("view-grid"), QStyle::SP_FileDialogListView, tr("切换文件视图"));
    QMenu *viewMenu = new QMenu(viewBtn);
    QAction *viewIcon = viewMenu->addAction(tr("大图标"));
    QAction *viewList = viewMenu->addAction(tr("列表"));
    QAction *viewDetails = viewMenu->addAction(tr("详细信息"));
    viewMenu->addSeparator();
    QAction *showHidden = viewMenu->addAction(tr("显示隐藏文件"));
    showHidden->setCheckable(true);
    viewBtn->setMenu(viewMenu);
    viewBtn->setPopupMode(QToolButton::InstantPopup);
    commands->addWidget(viewBtn);

    QToolButton *sortBtn = command(tr("排序"), QStringLiteral("view-sort-ascending"), QStyle::SP_ArrowUp, tr("按名称、大小、类型或时间排序"));
    QMenu *sortMenu = new QMenu(sortBtn);
    QAction *sortName = sortMenu->addAction(tr("按名称"));
    QAction *sortSize = sortMenu->addAction(tr("按大小"));
    QAction *sortType = sortMenu->addAction(tr("按类型"));
    QAction *sortDate = sortMenu->addAction(tr("按修改时间"));
    sortBtn->setMenu(sortMenu);
    sortBtn->setPopupMode(QToolButton::InstantPopup);
    commands->addWidget(sortBtn);

    QToolButton *moreBtn = new QToolButton(bar);
    moreBtn->setObjectName(QStringLiteral("CommandButton"));
    moreBtn->setIcon(win11AppIcon(QStringLiteral("more-horizontal"), QStyle::SP_TitleBarUnshadeButton));
    moreBtn->setIconSize(QSize(20, 20));
    moreBtn->setToolTip(tr("更多"));
    moreBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    moreBtn->setFixedSize(42, 32);
    QMenu *moreMenu = new QMenu(moreBtn);
    QAction *refreshAction = moreMenu->addAction(tr("刷新"));
    QAction *pathAction = moreMenu->addAction(tr("复制路径"));
    QAction *propsAction = moreMenu->addAction(tr("属性"));
    moreBtn->setMenu(moreMenu);
    moreBtn->setPopupMode(QToolButton::InstantPopup);
    commands->addWidget(moreBtn);
    commands->addStretch(1);

    auto *navigation = new QHBoxLayout;
    navigation->setContentsMargins(0, 0, 0, 0);
    navigation->setSpacing(4);
    auto navBtn = [&](const QString &iconName, const char *name, const QString &tip) {
        auto *b = new QPushButton(bar);
        b->setObjectName(QString::fromLatin1(name));
        b->setIcon(win11AppIcon(iconName, QStyle::SP_ArrowLeft));
        b->setIconSize(QSize(20, 20));
        b->setFixedSize(32, 30);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    m_backBtn = navBtn(QStringLiteral("nav-back"), "NavBack", tr("后退 (Alt+左)"));
    m_forwardBtn = navBtn(QStringLiteral("nav-forward"), "NavForward", tr("前进 (Alt+右)"));
    m_upBtn = navBtn(QStringLiteral("nav-up"), "NavUp", tr("向上"));
    auto *refreshBtn = navBtn(QStringLiteral("nav-refresh"), "NavRefresh", tr("刷新"));
    auto *computerBtn = navBtn(QStringLiteral("nav-computer"), "NavComputer", tr("此电脑"));
    navigation->addWidget(m_backBtn);
    navigation->addWidget(m_forwardBtn);
    navigation->addWidget(m_upBtn);
    navigation->addWidget(refreshBtn);
    navigation->addWidget(computerBtn);
    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        if (m_files && m_stack->currentWidget() == m_files)
            m_files->refresh();
        else
            navigateTo(kPcPath);
    });
    connect(computerBtn, &QPushButton::clicked, this, [this]() { navigateTo(kPcPath); });

    m_navStack = new QStackedWidget(bar);
    m_navStack->setFixedHeight(28);
    m_breadcrumbHost = new QWidget(m_navStack);
    m_breadcrumbLayout = new QHBoxLayout(m_breadcrumbHost);
    m_breadcrumbLayout->setContentsMargins(8, 0, 8, 0);
    m_breadcrumbLayout->setSpacing(2);
    m_breadcrumbLayout->addStretch();
    m_breadcrumbHost->setCursor(Qt::PointingHandCursor);
    m_navStack->addWidget(m_breadcrumbHost);

    m_address = new QLineEdit(m_navStack);
    m_address->setObjectName(QStringLiteral("Address"));
    m_address->setClearButtonEnabled(true);
    m_address->setCursorPosition(0);
    m_address->setContextMenuPolicy(Qt::NoContextMenu);
    m_navStack->addWidget(m_address);
    navigation->addWidget(m_navStack, 1);

    m_search = new QLineEdit(bar);
    m_search->setObjectName(QStringLiteral("Search"));
    m_search->setPlaceholderText(QStringLiteral("在此电脑中搜索"));
    m_search->setFixedHeight(30);
    m_search->addAction(win11AppIcon(QStringLiteral("nav-search"), QStyle::SP_FileDialogContentsView),
                        QLineEdit::TrailingPosition);
    navigation->addWidget(m_search, 0);
    /* Windows 11 places the path/navigation row above the command row. */
    root->addLayout(navigation);
    root->addLayout(commands);

    connect(m_backBtn, &QPushButton::clicked, this, &MainWindow::goBack);
    connect(m_forwardBtn, &QPushButton::clicked, this, &MainWindow::goForward);
    connect(m_upBtn, &QPushButton::clicked, this, &MainWindow::goUp);
    connect(m_address, &QLineEdit::returnPressed, this, &MainWindow::onAddressEntered);
    connect(m_search, &QLineEdit::returnPressed, this, &MainWindow::onSearchEntered);
    connect(newFolder, &QAction::triggered, this, [this] { if (m_files) m_files->createFolder(); });
    connect(newText, &QAction::triggered, this, [this] { if (m_files) m_files->createTextDocument(); });
    connect(cutBtn, &QToolButton::clicked, this, [this] { if (m_files) m_files->cutSelected(); });
    connect(copyBtn, &QToolButton::clicked, this, [this] { if (m_files) m_files->copySelected(); });
    connect(pasteBtn, &QToolButton::clicked, this, [this] { if (m_files) m_files->pasteHere(); });
    connect(renameBtn, &QToolButton::clicked, this, [this] { if (m_files) m_files->renameSelected(); });
    connect(deleteBtn, &QToolButton::clicked, this, [this] { if (m_files) m_files->trashSelected(); });
    connect(viewIcon, &QAction::triggered, this, [this] { if (m_files) m_files->setIconView(); });
    connect(viewList, &QAction::triggered, this, [this] { if (m_files) m_files->setListView(); });
    connect(viewDetails, &QAction::triggered, this, [this] { if (m_files) m_files->setDetailsView(); });
    connect(showHidden, &QAction::triggered, this, [this](bool on) { if (m_files) m_files->setShowHidden(on); });
    connect(sortName, &QAction::triggered, this, [this] { if (m_files) m_files->sortByName(); });
    connect(sortSize, &QAction::triggered, this, [this] { if (m_files) m_files->sortBySize(); });
    connect(sortType, &QAction::triggered, this, [this] { if (m_files) m_files->sortByType(); });
    connect(sortDate, &QAction::triggered, this, [this] { if (m_files) m_files->sortByDate(); });
    connect(shareBtn, &QToolButton::clicked, this, [this] { if (m_files) m_files->copyCurrentPath(); });
    connect(refreshAction, &QAction::triggered, this, [this] { if (m_files) m_files->refresh(); });
    connect(pathAction, &QAction::triggered, this, [this] { if (m_files) m_files->copyCurrentPath(); });
    connect(propsAction, &QAction::triggered, this, [this] { if (m_files) m_files->showSelectedProperties(); });

    return bar;
}

QWidget *MainWindow::buildSidebar()
{
    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("Sidebar"));
    panel->setFixedWidth(220);

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(0);

    auto sectionTitle = [&](const QString &text) {
        auto *label = new QLabel(text, panel);
        label->setObjectName(QStringLiteral("SidebarSection"));
        return label;
    };

    auto makeList = [&](QWidget *parent) {
        auto *list = new QListWidget(parent);
        list->setObjectName(QStringLiteral("SidebarList"));
        list->setIconSize(QSize(20, 20));
        list->setSpacing(1);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setContextMenuPolicy(Qt::NoContextMenu);  /* no English default menu */
        return list;
    };
    auto addItem = [&](QListWidget *list, const QString &text, const QString &path,
                       const QString &iconName, QStyle::StandardPixmap fallback) {
        auto *item = new QListWidgetItem(list);
        /* Navigation locations are physical places, not applications. Use the
           curated colour WindowsIcons place namespace here; the old apps
           namespace caused Home/Desktop/Pictures/Trash to resolve to black
           Fluent glyphs while Documents happened to have a valid alias. */
        item->setIcon(win11PlaceIcon(iconName, fallback));
        item->setText(text);
        item->setData(Qt::UserRole, path);
    };

    layout->addWidget(sectionTitle(tr("快速访问")));
    m_sidebar = makeList(panel);
    addItem(m_sidebar, tr("主页"), QDir::homePath(), QStringLiteral("user-home"), QStyle::SP_DirHomeIcon);
    addItem(m_sidebar, tr("桌面"), QDir::homePath() + QStringLiteral("/Desktop"), QStringLiteral("desktop"), QStyle::SP_DirIcon);
    addItem(m_sidebar, tr("下载"), QDir::homePath() + QStringLiteral("/Downloads"), QStringLiteral("downloads"), QStyle::SP_DirIcon);
    addItem(m_sidebar, tr("文档"), QDir::homePath() + QStringLiteral("/Documents"), QStringLiteral("documents"), QStyle::SP_DirIcon);
    addItem(m_sidebar, tr("图片"), QDir::homePath() + QStringLiteral("/Pictures"), QStringLiteral("pictures"), QStyle::SP_DirIcon);
    layout->addWidget(m_sidebar, 1);

    layout->addSpacing(2);
    layout->addWidget(sectionTitle(tr("此电脑与设备")));
    m_sidebarExtra = makeList(panel);
    addItem(m_sidebarExtra, tr("此电脑"), kPcPath, QStringLiteral("desktop-this-pc"), QStyle::SP_ComputerIcon);
    addItem(m_sidebarExtra, tr("回收站"), QStringLiteral("__trash__"), QStringLiteral("user-trash"), QStyle::SP_TrashIcon);
    layout->addWidget(m_sidebarExtra);

    return panel;
}

void MainWindow::navigateTo(const QString &path)
{
    if (path == m_currentLocation)
        return;
    if (!m_currentLocation.isEmpty())
        m_back.push(m_currentLocation);
    m_forward.clear();

    m_currentLocation = path;
    if (path == kPcPath) {
        m_stack->setCurrentWidget(m_thisPc);
        setViewTitle(QStringLiteral("此电脑"));
    } else if (path == QStringLiteral("__trash__")) {
        if (QDir home = QDir::home(); home.exists(".local/share/Trash/files")) {
            navigateTo(home.absoluteFilePath(".local/share/Trash/files"));
            return;
        }
        m_stack->setCurrentWidget(m_files);
        m_files->showDirectory(QStringLiteral("/dev/null"));
        setViewTitle(QStringLiteral("回收站"));
    } else {
        QDir dir(path);
        if (!dir.exists()) {
            m_status->showMessage(tr("找不到路径: ") + path, 4000);
            m_back.pop();
            return;
        }
        m_stack->setCurrentWidget(m_files);
        m_files->showDirectory(path);
        setViewTitle(dir.dirName().isEmpty() ? path : dir.dirName());
    }
    updateBreadcrumb(path);
    updateNavButtons();

    if (path != kPcPath && path != QStringLiteral("__trash__"))
        fakeLoad(path, false);
}

void MainWindow::goBack()
{
    if (m_back.isEmpty())
        return;
    m_forward.push(m_currentLocation);
    QString target = m_back.pop();
    m_currentLocation = target;
    navigateToNoPush(target);
}

void MainWindow::goForward()
{
    if (m_forward.isEmpty())
        return;
    QString target = m_forward.pop();
    m_back.push(m_currentLocation);
    navigateToNoPush(target);
}

void MainWindow::goUp()
{
    if (m_currentLocation == kPcPath || m_currentLocation == QStringLiteral("__trash__"))
        return;
    QDir dir(m_currentLocation);
    if (dir.cdUp()) {
        QString parent = dir.absolutePath();
        if (parent == m_currentLocation)
            parent = kPcPath;
        navigateTo(parent);
    } else {
        navigateTo(kPcPath);
    }
}

void MainWindow::navigateToNoPush(const QString &path)
{
    m_currentLocation = path;
    if (path == kPcPath) {
        m_stack->setCurrentWidget(m_thisPc);
        setViewTitle(QStringLiteral("此电脑"));
    } else {
        m_stack->setCurrentWidget(m_files);
        m_files->showDirectory(path);
        QDir dir(path);
        setViewTitle(dir.dirName().isEmpty() ? path : dir.dirName());
    }
    updateBreadcrumb(path);
    updateNavButtons();
    fakeLoad(path, false);
}

void MainWindow::updateBreadcrumb(const QString &path)
{
    while (QLayoutItem *item = m_breadcrumbLayout->takeAt(0))
        delete item->widget();

    auto addCrumb = [&](const QString &label, const QString &target, bool last) {
        auto *b = new QPushButton(label, m_breadcrumbHost);
        b->setObjectName(last ? QStringLiteral("CrumbLast") : QStringLiteral("Crumb"));
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, target]() { navigateTo(target); });
        m_breadcrumbLayout->addWidget(b);
    };
    auto addSep = [&]() {
        auto *sep = new QLabel(m_breadcrumbHost);
        sep->setObjectName(QStringLiteral("CrumbSep"));
        sep->setPixmap(win11AppIcon(QStringLiteral("nav-chevron-right"), QStyle::SP_ArrowRight).pixmap(14, 14));
        sep->setFixedSize(16, 20);
        sep->setAlignment(Qt::AlignCenter);
        m_breadcrumbLayout->addWidget(sep);
    };

    addCrumb(QStringLiteral("此电脑"), kPcPath, path == kPcPath);
    if (path != kPcPath) {
        addSep();
        QStringList parts = QDir(path).absolutePath().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString accum;
        for (int i = 0; i < parts.size(); ++i) {
            accum = accum.isEmpty() ? QStringLiteral("/") + parts[i] : accum + QLatin1Char('/') + parts[i];
            if (i == parts.size() - 1) {
                addCrumb(parts[i], accum, true);
            } else {
                addCrumb(parts[i], accum, false);
            }
            if (i != parts.size() - 1)
                addSep();
        }
    }
}

void MainWindow::updateNavButtons()
{
    m_backBtn->setEnabled(!m_back.isEmpty());
    m_forwardBtn->setEnabled(!m_forward.isEmpty());
    m_upBtn->setEnabled(m_currentLocation != kPcPath);
}

void MainWindow::enterAddressMode()
{
    m_address->setText(m_currentLocation);
    m_navStack->setCurrentWidget(m_address);
    m_address->setFocus(Qt::MouseFocusReason);
    m_address->selectAll();
}

void MainWindow::leaveAddressMode()
{
    m_navStack->setCurrentWidget(m_breadcrumbHost);
}

void MainWindow::fakeLoad(const QString &path, bool force)
{
    ++m_navigationCount;
    bool heavy = path == QStringLiteral("/") || QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden).size() > 3000;
    bool randomTrap = crashAnticsEnabled()
        && ((QRandomGenerator::global()->bounded(100) < 12) || (m_navigationCount % 15 == 14));
    if (!force && !heavy && !randomTrap)
        return;

    m_status->showMessage(QStringLiteral("正在计算项目大小…"));
    {
        QEventLoop loop;
        QTimer::singleShot(heavy ? 2200 : 900, &loop, &QEventLoop::quit);
        loop.exec();
    }
    m_status->showMessage(QStringLiteral("就绪"));
    m_notResponsive->trigger();
}

void MainWindow::onAddressEntered()
{
    QString text = m_address->text().trimmed();
    if (text.isEmpty()) {
        leaveAddressMode();
        return;
    }
    navigateTo(text);
    leaveAddressMode();
}

void MainWindow::onSearchEntered()
{
    m_status->showMessage(QStringLiteral("正在搜索 \"") + m_search->text().trimmed() + QStringLiteral("\" …"));
    {
        QEventLoop loop;
        QTimer::singleShot(1600, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if ((m_search->text().trimmed().isEmpty() ? 1 : 0)
        || (crashAnticsEnabled() && QRandomGenerator::global()->bounded(100) < 60))
        m_notResponsive->trigger();
}

void MainWindow::onSidebarActivated(QListWidgetItem *item)
{
    if (!item)
        return;
    QString target = item->data(Qt::UserRole).toString();
    navigateTo(target);
}

void MainWindow::setViewTitle(const QString &title)
{
    const QString fullTitle = title + QStringLiteral(" — Lindows 资源管理器");
    setWindowTitle(fullTitle);
    if (m_titleLabel)
        m_titleLabel->setText(fullTitle);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (crashAnticsEnabled())
        m_notResponsive->trigger();
    event->accept();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (crashAnticsEnabled() && !m_chaosStarted) {
        m_chaosStarted = true;
        QTimer::singleShot(900, this, [this]() { m_notResponsive->trigger(); });
    }
}