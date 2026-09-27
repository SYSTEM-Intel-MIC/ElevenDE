#include "filelist.h"
#include <cstdio>

#include <QAbstractItemView>
#include <QAction>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLinearGradient>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QTreeView>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

namespace {

const char kActOpen[]   = "open";
const char kActCopy[]   = "copy";

const char kActCopyPath[] = "copy_path";
const char kActCut[]    = "cut";
const char kActPaste[]  = "paste";
const char kActRename[] = "rename";
const char kActDelete[] = "delete";
const char kActDeletePerm[] = "delete_permanent";
const char kActNew[]    = "newdir";
const char kActNewFile[]= "newfile";
const char kActEdit[]   = "edit";
const char kActOpenDefault[]  = "open_default";
const char kActOpenText[]     = "open_text";
const char kActOpenTerm[]     = "open_term";
const char kActRefresh[]= "refresh";
const char kActProps[]  = "props";
const char kActIcon[]   = "view_icon";
const char kActList[]   = "view_list";
const char kActDetails[]= "view_details";
const char kActHidden[] = "show_hidden";
const char kSortName[]  = "sort_name";
const char kSortSize[]  = "sort_size";
const char kSortType[]  = "sort_type";
const char kSortDate[]  = "sort_date";

/* app-level clipboard for copy/cut inside this Explorer instance */
QStringList g_clip;
bool        g_clipCut = false;

const char *kViewKey = "viewMode";

bool copyTree(const QString &src, const QString &dst)
{
    QFileInfo info(src);
    if (!info.isDir())
        return QFile::copy(src, dst);
    if (!QDir().mkpath(dst))
        return false;
    QDir dir(src);
    const QStringList names = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    bool ok = true;
    for (const QString &n : names)
        ok = copyTree(src + QLatin1Char('/') + n, dst + QLatin1Char('/') + n) && ok;
    return ok;
}

QString uniqueTarget(const QString &dir, const QString &name)
{
    QFileInfo info(name);
    const QString base = info.completeBaseName();
    const QString ext  = info.suffix();
    QString candidate = dir + QLatin1Char('/') + name;
    int i = 2;
    while (QFileInfo::exists(candidate)) {
        QFileInfo i2(name);
        QString with = ext.isEmpty()
            ? base + QStringLiteral(" (%1)").arg(i)
            : base + QStringLiteral(" (%1).%2").arg(i).arg(ext);
        candidate = dir + QLatin1Char('/') + with;
        ++i;
    }
    return candidate;
}

/* freedesktop.org trash spec (what Nautilus/Thunar/gio use): move the file
 * into ~/.local/share/Trash/files and write a .trashinfo sidecar so it can
 * be restored / shown properly by any spec-compliant trash implementation.
 * Returns false when trashing is impossible; callers then fall back to a
 * confirmed permanent delete. */
bool moveToTrash(const QString &path)
{
    QFileInfo fi(path);
    if (!fi.exists())
        return false;
    const QString trash = QDir::homePath() + QStringLiteral("/.local/share/Trash");
    const QString files = trash + QStringLiteral("/files");
    const QString info  = trash + QStringLiteral("/info");
    if (!QDir().mkpath(files) || !QDir().mkpath(info))
        return false;

    const QFileInfo base(fi.fileName());
    const QString ext = base.suffix();
    QString name = fi.fileName();
    QString dest  = files + QLatin1Char('/') + name;
    int i = 1;
    while (QFileInfo::exists(dest)) {
        name = ext.isEmpty()
            ? base.completeBaseName() + QStringLiteral(" (%1)").arg(i++)
            : base.completeBaseName() + QStringLiteral(" (%1).%2").arg(i++).arg(ext);
        dest = files + QLatin1Char('/') + name;
    }
    if (!QFile::rename(path, dest))
        return false;

    QFile meta(info + QLatin1Char('/') + name + QStringLiteral(".trashinfo"));
    if (meta.open(QIODevice::WriteOnly)) {
        QByteArray b = "[Trash Info]\nPath=";
        b += fi.absoluteFilePath().toUtf8().toPercentEncoding(QByteArray("''/"));
        b += "\nDeletionDate=";
        b += QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8();
        b += "\n";
        meta.write(b);
        meta.close();
    }
    return true;
}

/* permanent (non-trash) recursive delete: files or directories */
bool removeFileRec(const QString &path)
{
    QFileInfo fi(path);
    if (!fi.exists())
        return false;
    return fi.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
}

} // namespace

/* fallback glyphs for the icon views when no icon theme provides
   QFileSystemModel icons (common on minimal/Kali installs).
   Gradient-shaded to look like real Windows folder / document icons. */
static QPixmap paintedGlyph(bool isDir, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal u = size / 64.0;

    if (isDir) {
        /* back panel + tab */
        QLinearGradient back(0, 0, 0, 64 * u);
        back.setColorAt(0.0, QColor(0xf5, 0xc9, 0x4f));
        back.setColorAt(1.0, QColor(0xe8, 0xaa, 0x2a));
        p.setPen(QColor(0xb8, 0x7e, 0x06));
        p.setBrush(back);
        p.drawRoundedRect(QRectF(3 * u, 20 * u, 58 * u, 40 * u), 5 * u, 5 * u);
        p.drawRect(QRectF(3 * u, 20 * u, 58 * u, 9 * u));
        /* tab */
        QPainterPath tab;
        tab.moveTo(10 * u, 18 * u);
        tab.lineTo(30 * u, 18 * u);
        tab.lineTo(34 * u, 24 * u);
        tab.lineTo(3 * u, 24 * u);
        tab.closeSubpath();
        p.setBrush(back); p.setPen(Qt::NoPen);
        p.drawPath(tab);
        /* front panel highlight */
        p.setBrush(Qt::NoBrush);
        p.setPen(QColor(0xff, 0xef, 0xb0));
        p.drawRoundedRect(QRectF(6 * u, 23 * u, 52 * u, 15 * u), 3 * u, 3 * u);
    } else {
        QLinearGradient sheet(0, 0, 0, 64 * u);
        sheet.setColorAt(0.0, QColor(0xfa, 0xfa, 0xfa));
        sheet.setColorAt(1.0, QColor(0xe3, 0xe5, 0xe8));
        p.setBrush(sheet);
        p.setPen(QColor(0x9a, 0x9e, 0xa4));
        QPainterPath doc;
        doc.moveTo(20 * u, 3 * u);
        doc.lineTo(42 * u, 3 * u);
        doc.lineTo(59 * u, 20 * u);
        doc.lineTo(59 * u, 59 * u);
        doc.lineTo(20 * u, 59 * u);
        doc.closeSubpath();
        p.drawPath(doc);
        /* folded corner */
        QPainterPath fold;
        fold.moveTo(42 * u, 3 * u);
        fold.lineTo(42 * u, 20 * u);
        fold.lineTo(59 * u, 20 * u);
        fold.closeSubpath();
        p.setBrush(QColor(0xc9, 0xcd, 0xd2)); p.setPen(Qt::NoPen);
        p.drawPath(fold);
        p.setPen(QColor(0xb5, 0xba, 0xc0));
        p.drawLine(QPointF(42 * u, 3 * u), QPointF(42 * u, 20 * u));
        p.drawLine(QPointF(42 * u, 20 * u), QPointF(59 * u, 20 * u));
        /* text lines */
        p.setPen(QColor(0x9a, 0xa0, 0xa8));
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 3; ++i) {
            p.drawLine(QPointF(25 * u, (30 + i * 9) * u), QPointF(49 * u, (30 + i * 9) * u));
            p.drawLine(QPointF(25 * u, (30 + i * 9) * u), QPointF(40 * u, (30 + i * 9) * u));
        }
    }
    p.end();
    return pm;
}

static QIcon packagedIcon(const QString &name, const QString &category)
{
    const QString base = QStringLiteral("/usr/local/share/elevende-shell/icons/");
    const QStringList sizes = { QStringLiteral("64x64"), QStringLiteral("48x48"), QStringLiteral("32x32"), QStringLiteral("24x24") };
    for (const QString &size : sizes) {
        const QString path = base + size + QLatin1Char('/') + category + QLatin1Char('/') + name + QStringLiteral(".png");
        if (QFileInfo::exists(path))
            return QIcon(path);
    }
    return QIcon();
}

static QIcon packagedFileIcon(const QFileInfo &fi)
{
    if (fi.isDir())
        return packagedIcon(QStringLiteral("folder"), QStringLiteral("apps"));
    const QString suffix = fi.suffix().toLower();
    if (suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg") || suffix == QStringLiteral("gif"))
        return packagedIcon(QStringLiteral("image"), QStringLiteral("mimetypes"));
    if (suffix == QStringLiteral("mp4") || suffix == QStringLiteral("mkv") || suffix == QStringLiteral("avi"))
        return packagedIcon(QStringLiteral("video-x-generic"), QStringLiteral("mimetypes"));
    if (suffix == QStringLiteral("txt") || suffix == QStringLiteral("md") || suffix == QStringLiteral("log") || suffix == QStringLiteral("conf"))
        return packagedIcon(QStringLiteral("text-x-generic"), QStringLiteral("mimetypes"));
    return packagedIcon(QStringLiteral("text-x-generic"), QStringLiteral("mimetypes"));
}

class FileList::Model : public QFileSystemModel
{
public:
    explicit Model(QObject *parent = nullptr) : QFileSystemModel(parent)
    {
        setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        setReadOnly(true);
        setNameFilters(QStringList());
        setNameFilterDisables(false);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role == Qt::DecorationRole) {
            const QFileInfo fi(filePath(index));
            const QIcon packaged = packagedFileIcon(fi);
            if (!packaged.isNull())
                return QVariant(packaged);
            /* Only use the platform icon provider if a packaged WindowsIcons
               asset is genuinely unavailable; never prefer a monochrome theme
               icon over the curated Windows asset. */
            const QVariant themed = QFileSystemModel::data(index, role);
            if (!themed.isNull()) {
                const QIcon ic = themed.value<QIcon>();
                if (!ic.isNull() && !ic.availableSizes().isEmpty() &&
                    !ic.pixmap(32, 32).isNull())
                    return themed;
            }
            static const QIcon fm = paintedGlyph(true, 64);
            static const QIcon fl = paintedGlyph(false, 64);
            return isDir(index) ? QVariant(fm) : QVariant(fl);
        }
        return QFileSystemModel::data(index, role);
    }

    /* --- drag source (3.5.1): files can be dragged OUT of Explorer ---------
     * Qt turns a view drag into an XDND drag on X11, so anything drop-capable
     * (the ElevenDE desktop shell, other file managers, browsers, editors)
     * receives the selection as text/uri-list. The mime payload is built here
     * from the file paths so it never depends on what the base class
     * serializes; setReadOnly(true) only disables drops, not drags. */
    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        Qt::ItemFlags f = QFileSystemModel::flags(index);
        if (index.isValid())
            f |= Qt::ItemIsDragEnabled;
        return f;
    }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        auto *md = new QMimeData;
        QList<QUrl> urls;
        for (const QModelIndex &idx : indexes) {
            if (!idx.isValid())
                continue;
            const QString p = filePath(idx);
            if (!p.isEmpty())
                urls.append(QUrl::fromLocalFile(p));
        }
        if (!urls.isEmpty())
            md->setUrls(urls);
        return md;
    }

    Qt::DropActions supportedDragActions() const override
    {
        return Qt::CopyAction | Qt::MoveAction;
    }
};

/* Filtering + sorting proxy used by all three views (the approach PCManFM /
 * Dolphin take): folders always sort first, then by the chosen column, and
 * the proxy hides dotfiles and enforces the name filter. Because the views
 * talk to the proxy, hiding/sorting/filtering is live with no extra refresh
 * plumbing. Sort/filter criteria live in this instance, set from FileList. */
class FileList::ModelProxy : public QSortFilterProxyModel
{
public:
    explicit ModelProxy(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setDynamicSortFilter(true);
        setSortCaseSensitivity(Qt::CaseInsensitive);
    }

    bool showHidden = false;
    QString nameFilter; /* lower-cased substring, empty = show all */

    /* invalidateFilter() is protected in QSortFilterProxyModel; expose it
     * so FileList can re-run the filter after toggling hidden/filter. */
    void refilter() { invalidateFilter(); }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        if (!idx.isValid())
            return false;
        const auto *fsm = static_cast<const QFileSystemModel *>(sourceModel());
        const QString name = fsm->fileName(idx);
        if (!showHidden && name.startsWith(QLatin1Char('.')))
            return false;
        if (!nameFilter.isEmpty() &&
            !name.contains(nameFilter, Qt::CaseInsensitive))
            return false;
        return true;
    }

    /* QSortFilterProxyModel maps `left`/`right` to source indexes; sort by
     * folders-first, then the active column with the natural ordering for
     * each column type. */
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const QFileSystemModel *m =
            static_cast<const QFileSystemModel *>(sourceModel());
        const bool lDir = m->isDir(left);
        const bool rDir = m->isDir(right);
        if (lDir != rDir)
            return lDir;                     /* directories first */
        const int col = left.column();
        switch (col) {
        case 1: {                            /* size (bytes), not the string */
            const qint64 ls = m->size(left), rs = m->size(right);
            if (ls != rs) return ls < rs;
            break;
        }
        case 3: {                            /* modification time */
            const QDateTime lt = m->fileInfo(left).lastModified();
            const QDateTime rt = m->fileInfo(right).lastModified();
            if (lt != rt) return lt < rt;
            break;
        }
        case 2: {                            /* type (suffix) */
            /* QFileSystemModel has no suffix(); derive it from fileInfo. */
            const QString l = m->fileInfo(left).suffix();
            const QString r = m->fileInfo(right).suffix();
            const int c = l.compare(r, Qt::CaseInsensitive);
            if (c) return c < 0;
            break;
        }
        default:
            break;
        }
        /* name tiebreak (and the plain name sort) */
        return m->data(left, Qt::DisplayRole).toString().compare(
                   m->data(right, Qt::DisplayRole).toString(),
                   Qt::CaseInsensitive) < 0;
    }
};

FileList::FileList(QWidget *parent)
    : QWidget(parent)
    , m_model(new Model(this))
    , m_proxy(new ModelProxy(this))
    , m_stack(new QStackedWidget(this))
    , m_iconView(new QListView(m_stack))
    , m_listView(new QListView(m_stack))
    , m_treeView(new QTreeView(m_stack))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_model->setRootPath(QStringLiteral("/"));

    auto setupList = [](QListView *v, QListView::ViewMode mode) {
        v->setViewMode(mode);
        v->setResizeMode(QListView::Adjust);
        v->setEditTriggers(QAbstractItemView::NoEditTriggers);
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setUniformItemSizes(mode == QListView::ListMode);
        v->setFrameShape(QFrame::NoFrame);
        v->setSpacing(mode == QListView::IconMode ? 16 : 4);
        v->setModelColumn(0);
    };
    setupList(m_iconView, QListView::IconMode);
    setupList(m_listView, QListView::ListMode);
    m_iconView->setIconSize(QSize(56, 56));
    m_iconView->setWordWrap(true);
    m_iconView->setGridSize(QSize(104, 96));
    m_listView->setIconSize(QSize(20, 20));
    m_listView->setWordWrap(false);

    m_treeView->setRootIsDecorated(true);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setIndentation(18);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_treeView->setAlternatingRowColors(false);
    m_treeView->setAnimated(true);   /* expand/collapse glide (3.5.1) */
    m_treeView->setSortingEnabled(true);
    QHeaderView *h = m_treeView->header();
    h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    h->setSectionResizeMode(QHeaderView::Interactive);
    h->setStretchLastSection(true);
    h->setMinimumSectionSize(64);

    m_iconView->setModel(m_proxy);
    m_listView->setModel(m_proxy);
    m_treeView->setModel(m_proxy);

    m_proxy->setSourceModel(m_model);
    m_proxy->sort(SortName, Qt::AscendingOrder);

    /* Drag OUT of Explorer (3.5.1). DragOnly = views are pure drag sources:
     * XDND carries the selection to the desktop shell or any other drop-aware
     * program (xterm itself predates XDND and cannot receive drops). Drops
     * INTO Explorer stay disabled while the model is read-only. */
    for (QAbstractItemView *v : { (QAbstractItemView *)m_iconView,
                                  (QAbstractItemView *)m_listView,
                                  (QAbstractItemView *)m_treeView }) {
        v->setDragEnabled(true);
        v->setDragDropMode(QAbstractItemView::DragOnly);
        v->setDefaultDropAction(Qt::CopyAction);
    }

    auto wireSelection = [this](QAbstractItemView *v) {
        connect(v->selectionModel(), &QItemSelectionModel::selectionChanged,
                this, &FileList::onSelectionChanged);
    };
    wireSelection(m_iconView);
    wireSelection(m_listView);
    wireSelection(m_treeView);

    m_stack->addWidget(m_iconView);
    m_stack->addWidget(m_listView);
    m_stack->addWidget(m_treeView);
    layout->addWidget(m_stack);

    connect(m_iconView, &QListView::doubleClicked, this, &FileList::onOpenIndex);
    connect(m_listView, &QListView::doubleClicked, this, &FileList::onOpenIndex);
    connect(m_treeView, &QTreeView::doubleClicked, this, &FileList::onOpenIndex);

    /* QFileSystemModel loads directory contents asynchronously. The first
     * updateStatus() call in showDirectory() therefore sees 0 rows; re-run
     * the root mapping + status once the folder is actually populated. */
    connect(m_model, &QFileSystemModel::directoryLoaded, this,
            [this](const QString &path) {
                if (path != m_current)
                    return;
                setRootAll();
                updateStatus();
            });

    setContextMenuPolicy(Qt::NoContextMenu);
    m_iconView->setContextMenuPolicy(Qt::NoContextMenu);
    m_listView->setContextMenuPolicy(Qt::NoContextMenu);
    m_treeView->setContextMenuPolicy(Qt::NoContextMenu);
    /* Context menu is handled centrally by FileList::eventFilter, installed on
     * every view AND every viewport. It catches BOTH the Qt-synthesized
     * QEvent::ContextMenu and the raw right-button MouseButtonPress (some X11
     * setups never synthesize the former; the raw press always arrives).
     * NoContextMenu stops the widget-level default path, so nothing double
     * fires. */
    for (QObject *o : { (QObject *)m_iconView, (QObject *)m_iconView->viewport(),
                        (QObject *)m_listView, (QObject *)m_listView->viewport(),
                        (QObject *)m_treeView, (QObject *)m_treeView->viewport() })
        o->installEventFilter(this);

    m_mode = IconView;
    QSettings s(QStringLiteral("ExplorerForLinux"), QStringLiteral("Explorer"));
    int saved = s.value(QLatin1String(kViewKey), (int)IconView).toInt();
    setViewMode(saved >= ListView && saved <= DetailsView
                    ? (ViewMode)saved
                    : IconView);
    m_showHidden = s.value(QLatin1String("showHidden"), false).toBool();
    m_proxy->showHidden = m_showHidden;
    m_proxy->refilter();

    /* Windows Explorer keyboard conventions (Dolphin/PCManFM use the same
     * keys). WidgetShortcut: only while a view inside the file list has focus,
     * so typing in the address/search bar is never hijacked.
     * Qt6 note: the new-style connect() cannot take a const char* SLOT()
     * string, so use the old-style string-based connect here. */
    auto shortcut = [this](const QKeySequence &seq, const char *slot) {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::WidgetShortcut);
        connect(sc, SIGNAL(activated()), this, slot);
    };
    shortcut(QKeySequence(Qt::Key_F2),                 SLOT(renameSelected()));
    shortcut(QKeySequence(Qt::Key_Delete),             SLOT(trashSelected()));
    shortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_Delete), SLOT(deleteSelectedPermanent()));
    shortcut(QKeySequence(Qt::Key_Return),             SLOT(openSelected()));
    shortcut(QKeySequence(Qt::Key_Enter),              SLOT(openSelected()));
    shortcut(QKeySequence(Qt::Key_Backspace),          SLOT(goUpOne()));
    shortcut(QKeySequence(Qt::ControlModifier | Qt::Key_C),       SLOT(copySelected()));
    shortcut(QKeySequence(Qt::ControlModifier | Qt::Key_X),       SLOT(cutSelected()));
    shortcut(QKeySequence(Qt::ControlModifier | Qt::Key_V),       SLOT(pasteHere()));
    shortcut(QKeySequence(Qt::ControlModifier | Qt::Key_H),       SLOT(toggleHidden()));
    shortcut(QKeySequence(Qt::ControlModifier | Qt::Key_A),       SLOT(selectAllViews()));
}

QWidget *FileList::currentViewWidget() const
{
    return m_stack->currentWidget();
}

QModelIndex FileList::currentIndexAt(const QPoint &pos)
{
    QModelIndex idx;
    if (m_stack->currentWidget() == m_iconView)
        idx = m_iconView->indexAt(pos);
    else if (m_stack->currentWidget() == m_listView)
        idx = m_listView->indexAt(pos);
    else
        idx = m_treeView->indexAt(pos);
    if (!idx.isValid())
        return QModelIndex();
    return idx;
}

void FileList::setRootAll()
{
    /* the views talk to the proxy, so the raw QFileSystemModel index of the
     * current folder must be mapped through it */
    const QModelIndex src = m_model->index(m_current);
    if (!src.isValid())
        return;                       /* folder vanished mid-load: keep old root */
    m_proxyRoot = m_proxy->mapFromSource(src);
    if (!m_proxyRoot.isValid())
        return;
    m_iconView->setRootIndex(m_proxyRoot);
    m_listView->setRootIndex(m_proxyRoot);
    m_treeView->setRootIndex(m_proxyRoot);
    m_treeView->expand(m_proxyRoot);
}

void FileList::showDirectory(const QString &path)
{
    m_current = path;
    setRootAll();
    m_iconView->verticalScrollBar()->setValue(0);
    m_listView->verticalScrollBar()->setValue(0);
    m_treeView->verticalScrollBar()->setValue(0);
    updateStatus();
}

QString FileList::currentDirectory() const
{
    return m_current;
}

QAbstractItemView *FileList::currentView()
{
    return qobject_cast<QAbstractItemView *>(currentViewWidget());
}

QStringList FileList::selectedPaths() const
{
    QStringList out;
    /* defensive: model may be mid-refresh; only collect valid rows */
    QAbstractItemView *v = qobject_cast<QAbstractItemView *>(currentViewWidget());
    if (!v || !v->selectionModel())
        return out;
    const auto idxs = v->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : idxs) {
        if (idx.column() != 0)
            continue;                        /* one entry per row, not per column */
        const QString p = idx.data(QFileSystemModel::FilePathRole).toString();
        if (!p.isEmpty() && !out.contains(p))
            out << p;
    }
    return out;
}

void FileList::updateStatus()
{
    const int n = m_proxy->rowCount(m_proxyRoot);
    const int sel = static_cast<int>(selectedPaths().size());
    QString text = tr("%1 项").arg(n);
    if (sel)
        text += tr(" · 已选 %1").arg(sel);
    if (!m_current.isEmpty()) {
        QStorageInfo si(m_current);
        if (si.isValid() && si.bytesAvailable() > 0)
            text += tr(" · 可用 %1 GB")
                        .arg(double(si.bytesAvailable()) / (1024.0 * 1024.0 * 1024.0),
                             0, 'f', 1);
    }
    emit statusInfo(text);
}

void FileList::onSelectionChanged()
{
    updateStatus();
}

void FileList::setShowHidden(bool on)
{
    if (m_showHidden == on)
        return;
    m_showHidden = on;
    m_proxy->showHidden = on;
    m_proxy->refilter();
    QSettings s(QStringLiteral("ExplorerForLinux"), QStringLiteral("Explorer"));
    s.setValue(QLatin1String("showHidden"), on);
    updateStatus();
}

void FileList::toggleHidden()
{
    setShowHidden(!m_showHidden);
}

void FileList::applyNameFilter(const QString &filter)
{
    m_proxy->nameFilter = filter.trimmed().toLower();
    m_proxy->refilter();
    updateStatus();
}

void FileList::setSort(SortColumn column, Qt::SortOrder order)
{
    m_sortCol = column;
    m_sortOrder = order;
    m_proxy->sort(int(column), order);
    if (m_mode == DetailsView)
        m_treeView->header()->setSortIndicator(int(column), order); /* cosmetic sync */
}

void FileList::openSelected()
{
    const QStringList s = selectedPaths();
    if (!s.isEmpty())
        openFile(s.first());
}

void FileList::renameSelected()
{
    const QStringList sel = selectedPaths();
    if (sel.isEmpty())
        return;
    const QString filePath = sel.first();
    QFileInfo fi(filePath);
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("重命名"), tr("新名称:"),
                                         QLineEdit::Normal, fi.fileName(), &ok);
    if (!ok || name.isEmpty() || name == fi.fileName())
        return;
    if (QFile::rename(filePath, fi.dir().absoluteFilePath(name)))
        refreshViews();
    else
        QMessageBox::warning(this, tr("重命名"), tr("无法重命名该文件"));
}

void FileList::trashSelected()
{
    const QStringList sel = selectedPaths();
    if (sel.isEmpty())
        return;
    int failed = 0;
    for (const QString &p : sel)
        if (!moveToTrash(p))
            ++failed;
    if (failed && QMessageBox::warning(
                      this, tr("删除"),
                      tr("%1 项无法移入回收站。\n是否永久删除？").arg(failed),
                      QMessageBox::Yes | QMessageBox::No,
                      QMessageBox::No) == QMessageBox::Yes)
        for (const QString &p : sel)
            removeFileRec(p);
    refreshViews();
    updateStatus();
}

void FileList::deleteSelectedPermanent()
{
    const QStringList sel = selectedPaths();
    if (sel.isEmpty())
        return;
    if (QMessageBox::warning(
            this, tr("永久删除"),
            tr("确定要永久删除选定的 %1 项吗？此操作无法撤销。").arg(sel.size()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;
    for (const QString &p : sel)
        removeFileRec(p);
    refreshViews();
    updateStatus();
}

void FileList::copySelected()
{
    const QStringList s = selectedPaths();
    if (!s.isEmpty()) {
        g_clip = s;
        g_clipCut = false;
    }
}

void FileList::cutSelected()
{
    const QStringList s = selectedPaths();
    if (!s.isEmpty()) {
        g_clip = s;
        g_clipCut = true;
    }
}

void FileList::pasteHere()
{
    const QStringList todo = g_clip;
    const bool cut = g_clipCut;
    g_clip.clear();
    g_clipCut = false;
    if (todo.isEmpty())
        return;
    for (const QString &src : todo) {
        QFileInfo fi(src);
        const QString target = uniqueTarget(m_current, fi.fileName());
        if (cut)
            QFile::rename(src, target);
        else
            copyTree(src, target);
    }
    refreshViews();
    updateStatus();
}

void FileList::selectAllViews()
{
    if (QAbstractItemView *v = currentView())
        v->selectAll();
}

void FileList::goUpOne()
{
    QDir d(m_current);
    if (d.cdUp() && d.absolutePath() != m_current)
        emit directoryActivated(d.absolutePath());
}

void FileList::setViewMode(ViewMode mode)
{
    m_mode = mode;
    m_stack->setCurrentWidget(mode == IconView          ? (QWidget *)m_iconView
                              : mode == ListView        ? (QWidget *)m_listView
                                                        : (QWidget *)m_treeView);
    if (mode == DetailsView) {
        m_treeView->header()->resizeSection(0, 320);
        m_treeView->header()->resizeSection(3, 150);
    }
    QSettings s(QStringLiteral("ExplorerForLinux"), QStringLiteral("Explorer"));
    s.setValue(QLatin1String(kViewKey), (int)mode);
}

void FileList::refreshViews()
{
    m_model->setRootPath(QStringLiteral("/"));   /* ask the model to reload  */
    setRootAll();
}

void FileList::refresh()
{
    refreshViews();
    updateStatus();
}

void FileList::createFolder()
{
    if (m_current.isEmpty() || !QDir(m_current).exists())
        return;
    QString base = tr("新建文件夹");
    QString target = uniqueTarget(m_current, base);
    if (!QDir().mkdir(target)) {
        QMessageBox::warning(this, tr("新建文件夹"), tr("无法在此位置创建文件夹。"));
        return;
    }
    refresh();
}

void FileList::createTextDocument()
{
    if (m_current.isEmpty() || !QDir(m_current).exists())
        return;
    const QString target = uniqueTarget(m_current, tr("新建文本文档.txt"));
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("新建文本文档"), tr("无法在此位置创建文本文档。"));
        return;
    }
    file.close();
    refresh();
}

void FileList::copyCurrentPath()
{
    if (!m_current.isEmpty())
        QApplication::clipboard()->setText(m_current);
}

void FileList::showSelectedProperties()
{
    const QStringList selected = selectedPaths();
    if (selected.isEmpty()) {
        QMessageBox::information(this, tr("属性"), tr("位置: %1").arg(m_current));
        return;
    }
    if (selected.size() > 1) {
        QMessageBox::information(this, tr("属性"), tr("已选择 %1 项\n位置: %2")
                                 .arg(selected.size()).arg(m_current));
        return;
    }
    const QFileInfo fi(selected.first());
    QMessageBox::information(this, tr("属性"),
        tr("名称: %1\n位置: %2\n类型: %3\n大小: %4\n修改时间: %5")
            .arg(fi.fileName())
            .arg(fi.absolutePath())
            .arg(fi.isDir() ? tr("文件夹") : (fi.suffix().isEmpty() ? tr("文件") : fi.suffix().toUpper() + tr(" 文件")))
            .arg(fi.isDir() ? tr("—") : QLocale().formattedDataSize(fi.size()))
            .arg(fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
}

void FileList::setIconView() { setViewMode(IconView); }
void FileList::setListView() { setViewMode(ListView); }
void FileList::setDetailsView() { setViewMode(DetailsView); }
void FileList::sortByName() { setSort(SortName, Qt::AscendingOrder); }
void FileList::sortBySize() { setSort(SortSize, Qt::AscendingOrder); }
void FileList::sortByType() { setSort(SortType, Qt::AscendingOrder); }
void FileList::sortByDate() { setSort(SortDate, Qt::DescendingOrder); }

void FileList::onOpenIndex(const QModelIndex &index)
{
    if (!index.isValid()) return;
    if (!index.isValid())
        return;
    QFileInfo fi(index.data(QFileSystemModel::FilePathRole).toString());
    if (fi.isDir())
        emit directoryActivated(fi.absoluteFilePath());
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absoluteFilePath()));
}

void FileList::openFile(const QString &path)
{
    QFileInfo fi(path);
    if (fi.isDir())
        emit directoryActivated(fi.absoluteFilePath());
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool FileList::eventFilter(QObject *obj, QEvent *event)
{
    if (m_menuOpen)
        return QWidget::eventFilter(obj, event);  /* one menu per gesture */
    if (isFileView(obj) && event->type() == QEvent::ContextMenu) {
        auto w = static_cast<QWidget *>(obj);
        onContextMenuFrom(w, static_cast<QContextMenuEvent *>(event)->pos());
        return true;                    /* consumed: no double menu */
    }
    /* Raw right-button press. Qt synthesizes QContextMenuEvent from it, but on
     * some X11 setups never synthesize the ContextMenu event while
     * the underlying ButtonPress always does - so this is the reliable trigger.
     * The m_menuOpen guard lets a later synthesized ContextMenu for the same
     * press fall through harmlessly instead of opening a second menu. */
    if (isFileView(obj) && event->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent *>(event)->button() == Qt::RightButton) {
        auto w = static_cast<QWidget *>(obj);
        onContextMenuFrom(w, static_cast<QMouseEvent *>(event)->pos());
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

/* pos may be relative to a view OR to its viewport; normalize it to viewport
 * coordinates (what currentIndexAt() expects). Filters are installed on both,
 * so the right-click can arrive through either. */
void FileList::onContextMenuFrom(QWidget *src, const QPoint &pos)
{
    auto *sa = qobject_cast<QAbstractScrollArea *>(src);
    QWidget *vp = sa ? sa->viewport() : src;
    onContextMenu(vp->mapFromGlobal(src->mapToGlobal(pos)));
}

bool FileList::isFileView(QObject *obj) const
{
    return obj == m_iconView || obj == m_listView || obj == m_treeView ||
           obj == m_iconView->viewport() || obj == m_listView->viewport() ||
           obj == m_treeView->viewport();
}

void FileList::onContextMenu(const QPoint &pos)
{
    QModelIndex idx = currentIndexAt(pos);
    /* pos is viewport-relative; the popup must be positioned in global
     * coords. Map through the viewport itself (its origin differs from the
     * view by the header height in details view). */
    auto *vw = qobject_cast<QAbstractScrollArea *>(currentViewWidget());
    QWidget *vp = vw ? vw->viewport() : currentViewWidget();
    QMenu menu(this);
    m_menuTargets.clear();

    if (idx.isValid()) {
        const QString path =
            idx.data(QFileSystemModel::FilePathRole).toString();  // proxy path
        QFileInfo fi(path);
        bool isDir = fi.isDir();

        /* multi-select semantics (Nautilus/Dolphin style): right-clicking an
         * already-selected row keeps the whole selection as the action target */
        QAbstractItemView *v = currentView();
        if (v && v->selectionModel()) {
            if (v->selectionModel()->isSelected(idx) &&
                v->selectionModel()->selectedRows(0).size() > 1) {
                m_menuTargets = selectedPaths();
            } else {
                v->selectionModel()->clearSelection();
                v->selectionModel()->select(idx, QItemSelectionModel::Select |
                                                     QItemSelectionModel::Rows);
                v->setCurrentIndex(idx);
                m_menuTargets = QStringList(path);
            }
        } else {
            m_menuTargets = QStringList(path);
        }
        const bool multi = m_menuTargets.size() > 1;

        auto add = [&](const QString &text, const char *id) {
            QAction *a = menu.addAction(text);
            a->setProperty("act", QLatin1String(id));
            a->setProperty("path", path);
            return a;
        };
        auto addSep = [&]() { menu.addSeparator(); };

        add(tr("打开"), kActOpen);
        addSep();
        add(tr("复制"), kActCopy);
        add(tr("剪切"), kActCut);
        QAction *paste = add(tr("粘贴"), kActPaste);
        paste->setEnabled(!g_clip.isEmpty());
        if (!multi)
            add(tr("复制路径"), kActCopyPath);
        addSep();
        if (!multi)
            add(tr("重命名"), kActRename);
        add(tr("删除"), kActDelete);                 /* moves to trash */
        if (!multi)
            add(tr("永久删除"), kActDeletePerm);
        addSep();
        if (!isDir) {
            QMenu *openWith = menu.addMenu(tr("打开方式"));
            auto ow = [&](const QString &text, const char *id) {
                QAction *a = openWith->addAction(text);
                a->setProperty("act", QLatin1String(id));
                a->setProperty("path", path);
                return a;
            };
            ow(tr("默认程序"), kActOpenDefault);
            ow(tr("文本编辑器 (gedit)"), kActOpenText);
            ow(tr("终端中打开 (xterm)"), kActOpenTerm);
            add(tr("编辑"), kActEdit);
            addSep();
        }

        QMenu *view = menu.addMenu(tr("查看"));
        auto viewAction = [&](const QString &text, const char *id) {
            QAction *a = view->addAction(text);
            a->setProperty("act", QLatin1String(id));
            a->setCheckable(true);
            a->setChecked(
                (QLatin1String(id) == QLatin1String("view_icon")     && m_mode == IconView) ||
                (QLatin1String(id) == QLatin1String("view_list")     && m_mode == ListView) ||
                (QLatin1String(id) == QLatin1String("view_details")  && m_mode == DetailsView));
            return a;
        };
        viewAction(tr("大图标"), kActIcon);
        viewAction(tr("列表"), kActList);
        viewAction(tr("详细信息"), kActDetails);
        view->addSeparator();
        QMenu *sortM = view->addMenu(tr("排序方式"));
        auto sortAction = [&](const QString &text, const char *id, SortColumn col) {
            QAction *a = sortM->addAction(text);
            a->setProperty("act", QLatin1String(id));
            a->setCheckable(true);
            a->setChecked(m_sortCol == col);
            return a;
        };
        sortAction(tr("名称"), kSortName, SortName);
        sortAction(tr("大小"), kSortSize, SortSize);
        sortAction(tr("类型"), kSortType, SortType);
        sortAction(tr("修改时间"), kSortDate, SortDate);
        view->addSeparator();
        QAction *hidden = view->addAction(tr("显示隐藏文件"));
        hidden->setProperty("act", QLatin1String(kActHidden));
        hidden->setCheckable(true);
        hidden->setChecked(m_showHidden);

        add(tr("刷新"), kActRefresh);
        addSep();
        if (!multi)
            add(tr("属性"), kActProps);

        m_menuOpen = true;
        QAction *picked = menu.exec(vp->mapToGlobal(pos));
        m_menuOpen = false;
        if (picked)
            onMenuAction(picked, path);
        return;
    }

    /* blank area */
    auto add = [&](const QString &text, const char *id) {
        QAction *a = menu.addAction(text);
        a->setProperty("act", QLatin1String(id));
        return a;
    };
    add(tr("刷新"), kActRefresh);
    add(tr("新建文件夹"), kActNew);
    add(tr("新建文本文档"), kActNewFile);
    menu.addSeparator();
    QAction *paste = add(tr("粘贴"), kActPaste);
    paste->setEnabled(!g_clip.isEmpty());
    menu.addSeparator();
    add(tr("在终端打开"), kActOpenTerm);
    menu.addSeparator();

    QMenu *view = menu.addMenu(tr("查看"));
    auto viewAction = [&](const QString &text, const char *id) {
        QAction *a = view->addAction(text);
        a->setProperty("act", QLatin1String(id));
        a->setCheckable(true);
        a->setChecked(
            (QLatin1String(id) == QLatin1String("view_icon")    && m_mode == IconView) ||
            (QLatin1String(id) == QLatin1String("view_list")    && m_mode == ListView) ||
            (QLatin1String(id) == QLatin1String("view_details") && m_mode == DetailsView));
        return a;
    };
    viewAction(tr("大图标"), kActIcon);
    viewAction(tr("列表"), kActList);
    viewAction(tr("详细信息"), kActDetails);
    view->addSeparator();
    QMenu *sortM = view->addMenu(tr("排序方式"));
    auto sortAction = [&](const QString &text, const char *id, SortColumn col) {
        QAction *a = sortM->addAction(text);
        a->setProperty("act", QLatin1String(id));
        a->setCheckable(true);
        a->setChecked(m_sortCol == col);
        return a;
    };
    sortAction(tr("名称"), kSortName, SortName);
    sortAction(tr("大小"), kSortSize, SortSize);
    sortAction(tr("类型"), kSortType, SortType);
    sortAction(tr("修改时间"), kSortDate, SortDate);
    view->addSeparator();
    QAction *hidden = view->addAction(tr("显示隐藏文件"));
    hidden->setProperty("act", QLatin1String(kActHidden));
    hidden->setCheckable(true);
    hidden->setChecked(m_showHidden);

    m_menuOpen = true;
    QAction *picked = menu.exec(vp->mapToGlobal(pos));
    m_menuOpen = false;
    if (picked)
        onMenuAction(picked, QString());
}

void FileList::onMenuAction(QAction *action, const QString &filePath)
{
    const QString id = action->property("act").toString();

    if (id == QLatin1String(kActRefresh)) {
        refreshViews();
        return;
    }
    if (id == QLatin1String(kActNew)) {
        QString p;
        for (int i = 1; i <= 100; ++i) {
            p = m_current + QStringLiteral("/新建文件夹%1").arg(i);
            if (QDir().mkdir(p))
                break;
        }
        refreshViews();
        return;
    }
    if (id == QLatin1String(kActNewFile)) {
        QString p;
        for (int i = 1; i <= 100; ++i) {
            p = m_current + QStringLiteral("/新建文本文档%1.txt").arg(i);
            QFile f(p);
            if (f.open(QIODevice::WriteOnly)) {
                f.close();
                break;
            }
        }
        refreshViews();
        return;
    }
    if (id == QLatin1String(kActIcon))    { setViewMode(IconView);    return; }
    if (id == QLatin1String(kActList))    { setViewMode(ListView);    return; }
    if (id == QLatin1String(kActDetails)) { setViewMode(DetailsView); return; }
    if (id == QLatin1String(kSortName))   { setSort(SortName, m_sortOrder);   return; }
    if (id == QLatin1String(kSortSize))   { setSort(SortSize, m_sortOrder);   return; }
    if (id == QLatin1String(kSortType))   { setSort(SortType, m_sortOrder);   return; }
    if (id == QLatin1String(kSortDate))   { setSort(SortDate, m_sortOrder);   return; }

    if (id == QLatin1String(kActOpen)) {
        for (const QString &p : m_menuTargets)
            openFile(p);
        return;
    }
    if (id == QLatin1String(kActEdit) || id == QLatin1String(kActOpenText)) {
        QProcess::startDetached(QStringLiteral("gedit"), QStringList() << filePath);
        return;
    }
    if (id == QLatin1String(kActOpenDefault)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
        return;
    }
    if (id == QLatin1String(kActOpenTerm)) {
        /* a file => terminal in its parent folder; blank-area menu (empty
         * path) => terminal in the current folder */
        QString dir = filePath.isEmpty()
                                ? m_current
                                : QFileInfo(filePath).absolutePath();
        QProcess::startDetached(
            QStringLiteral("xterm"),
            QStringList() << QStringLiteral("-e") << QStringLiteral("bash")
                          << QStringLiteral("-lc")
                          << QStringLiteral("cd '%1' 2>/dev/null; exec bash")
                                 .arg(dir.replace(QLatin1Char('\''),
                                                  QStringLiteral("'\\''"))));
        return;
    }
    if (id == QLatin1String(kActCopy)) {
        g_clip = m_menuTargets;
        g_clipCut = false;
        return;
    }
    if (id == QLatin1String(kActCopyPath)) {
        QApplication::clipboard()->setText(filePath);
        return;
    }
    if (id == QLatin1String(kActCut)) {
        g_clip = m_menuTargets;
        g_clipCut = true;
        return;
    }
    if (id == QLatin1String(kActPaste)) {
        pasteHere();
        return;
    }
    if (id == QLatin1String(kActRename)) {
        renameSelected();
        return;
    }
    if (id == QLatin1String(kActDelete)) {
        trashSelected();
        return;
    }
    if (id == QLatin1String(kActDeletePerm)) {
        if (!filePath.isEmpty()) {
            if (QMessageBox::warning(
                    this, tr("永久删除"),
                    tr("确定要永久删除“%1”吗？此操作无法撤销。").arg(filePath),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes)
                return;
            removeFileRec(filePath);
            refreshViews();
        }
        return;
    }
    if (id == QLatin1String(kActHidden)) {
        toggleHidden();
        return;
    }
    if (id == QLatin1String(kActProps)) {
        QFileInfo fi(filePath);
        QMessageBox::information(
            this, tr("属性"),
            tr("名称: %1\n位置: %2\n大小: %3 KB\n修改时间: %4")
                .arg(fi.fileName())
                .arg(fi.absolutePath())
                .arg(fi.isDir() ? QStringLiteral("-") : QString::number(fi.size() / 1024))
                .arg(fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        return;
    }
}