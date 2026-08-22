/*
 * elevende-settings - Windows 11 style Settings app for ElevenDE.
 *
 * Pages (Win11 layout, left nav + content area):
 *   系统      device name / CPU / RAM / disk / OS / kernel ("About")
 *   个性化    wallpaper picker (hot-reloads the shell), dark/light, accent
 *   显示      resolution via xrandr, backlight brightness
 *   网络      interface overview (state / MAC / IPv4 via getifaddrs)
 *   快捷键    Windows shortcut management -> Openbox rc.xml regeneration
 *
 * Settings persist to ~/.config/elevende/elevende.conf (QSettings).
 * Wallpaper changes are applied live: the file is copied to
 * ~/.local/share/elevende/wallpaper.png and the shell is poked with the
 * _ELEVENDE_RELOAD_WALLPAPER client message.
 */

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QMouseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTextEdit>
#include <QToolButton>
#include <QTimer>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <arpa/inet.h>

#ifdef ELEVENDE_HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
/* Xlib defines macros that collide with Qt enum names */
#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#endif

#include "../common/win11style.h"
#include "pages.h"

namespace {

const char *kShareDir = "/usr/local/share/elevende-shell";

QString userWallpaperPath()
{
    return QDir::homePath() + QStringLiteral("/.local/share/elevende/wallpaper.png");
}
QString userShortcutsPath()
{
    return QDir::homePath() + QStringLiteral("/.config/elevende/shortcuts.json");
}
QString systemShortcutsPath()
{
    return QStringLiteral("%1/shortcuts.json").arg(QLatin1String(kShareDir));
}

/* ---------- helpers ---------- */

QString readFirstLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readLine()).trimmed();
}

QString osReleaseValue(const QString &key)
{
    QFile f(QStringLiteral("/etc/os-release"));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(key + QLatin1Char('='))) {
            line = line.mid(key.size() + 1);
            if (line.startsWith('"') && line.endsWith('"'))
                line = line.mid(1, line.size() - 2);
            return line;
        }
    }
    return QString();
}

QString cpuModel()
{
    QFile f(QStringLiteral("/proc/cpuinfo"));
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("未知");
    /* /proc files stat() as size 0: QFile::atEnd() lies, loop on readLine */
    for (;;) {
        const QString line = QString::fromUtf8(f.readLine());
        if (line.isEmpty())
            break;
        if (line.startsWith(QStringLiteral("model name")))
            return line.mid(line.indexOf(':') + 1).trimmed();
    }
    return QStringLiteral("未知");
}

QString totalRam()
{
    QFile f(QStringLiteral("/proc/meminfo"));
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("未知");
    for (;;) {
        const QString line = QString::fromUtf8(f.readLine());
        if (line.isEmpty())
            break;
        if (line.startsWith(QStringLiteral("MemTotal:"))) {
            bool ok = false;
            const qint64 kb = line.section(':', 1).trimmed().section(' ', 0, 0).toLongLong(&ok);
            if (ok)
                return QStringLiteral("%1 GB").arg(kb / (1024.0 * 1024.0), 0, 'f', 1);
        }
    }
    return QStringLiteral("未知");
}

QString rootDiskSize()
{
    struct statvfs st;
    if (statvfs("/", &st) != 0)
        return QStringLiteral("未知");
    const double gb = (double)st.f_blocks * st.f_frsize / (1024.0 * 1024.0 * 1024.0);
    return QStringLiteral("%1 GB").arg(gb, 0, 'f', 1);
}

QString kernelVersion()
{
    struct utsname u;
    if (uname(&u) != 0)
        return QStringLiteral("未知");
    return QStringLiteral("%1 %2").arg(QString::fromUtf8(u.sysname), QString::fromUtf8(u.release));
}

void notifyShellWallpaperChanged()
{
#ifdef ELEVENDE_HAVE_X11
    Display *d = XOpenDisplay(nullptr);
    if (!d)
        return;
    const Atom type = XInternAtom(d, "_ELEVENDE_RELOAD_WALLPAPER", False);
    const Atom serialAtom = XInternAtom(d, "_ELEVENDE_WALLPAPER_SERIAL", False);
    static unsigned long serial = 0;
    ++serial;
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = DefaultRootWindow(d);
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    /* Send directly to the root window with mask 0. The shell owns the root
       event selection and must receive this client message even when another
       WM does not select SubstructureRedirectMask. */
    XSendEvent(d, DefaultRootWindow(d), False, 0, &ev);
    /* A root property change is selected by the shell and survives event-mask
       differences between X servers/WM setups.  It is a second, ordered
       notification after the atomic file rename, not a replacement for the
       legacy ClientMessage. */
    XChangeProperty(d, DefaultRootWindow(d), serialAtom, XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<unsigned char *>(&serial), 1);
    XSync(d, False);
    XCloseDisplay(d);
#endif
}

/* A rounded "card" container, Win11 settings style. */
QWidget *makeCard(const QString &title, QWidget *body, const QString &subtitle = QString())
{
    auto *card = new QWidget();
    card->setProperty("card", true);
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(16, 12, 16, 12);
    v->setSpacing(6);
    if (!title.isEmpty()) {
        auto *t = new QLabel(title, card);
        QFont f = t->font();
        f.setPointSizeF(10.5);
        f.setBold(true);
        t->setFont(f);
        v->addWidget(t);
    }
    if (!subtitle.isEmpty()) {
        auto *s = new QLabel(subtitle, card);
        s->setProperty("subtle", true);
        s->setWordWrap(true);
        v->addWidget(s);
    }
    if (body)
        v->addWidget(body);
    return card;
}

QLabel *pageHeading(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setPointSizeF(19);
    f.setWeight(QFont::DemiBold);
    l->setFont(f);
    return l;
}

QWidget *scrollOf(QWidget *inner)
{
    auto *sa = new QScrollArea();
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setWidget(inner);
    return sa;
}

/* ---------- 设置主页（Windows 11 卡片框架） ---------- */

QWidget *homeActionRow(const QString &iconName, const QString &title, const QString &detail, int pageIndex)
{
    auto *row = new QWidget();
    row->setObjectName(QStringLiteral("settingsHomeRow"));
    row->setProperty("settingsPageIndex", pageIndex);
    row->setCursor(Qt::PointingHandCursor);
    row->setMinimumHeight(58);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->setProperty("homeRow", true);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(18, 12, 16, 12);
    h->setSpacing(14);
    auto *ico = new QLabel(row);
    ico->setAttribute(Qt::WA_TransparentForMouseEvents);
    ico->setProperty("homeGlyph", true);
    ico->setAlignment(Qt::AlignCenter);
    ico->setFixedSize(28, 28);
    ico->setPixmap(Win11Style::appIcon(iconName).pixmap(24, 24));
    auto *copy = new QWidget(row);
    copy->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *v = new QVBoxLayout(copy);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(1);
    auto *t = new QLabel(title, copy);
    t->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont tf = t->font(); tf.setPointSizeF(10.5); t->setFont(tf);
    t->setStyleSheet(QStringLiteral("color:#202020; background:transparent;"));
    auto *s = new QLabel(detail, copy);
    s->setAttribute(Qt::WA_TransparentForMouseEvents);
    s->setProperty("subtle", true);
    s->setStyleSheet(QStringLiteral("color:#6d6f78; background:transparent;"));
    v->addWidget(t);
    if (!detail.isEmpty()) v->addWidget(s);
    h->addWidget(ico);
    h->addWidget(copy, 1);
    auto *arrow = new QLabel(QStringLiteral("›"), row);
    arrow->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont af = arrow->font(); af.setPointSizeF(18); arrow->setFont(af);
    h->addWidget(arrow);
    return row;
}

QWidget *buildHomePage()
{
    auto *page = new QWidget();
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(38, 26, 38, 32);
    outer->setSpacing(16);
    auto *heading = pageHeading(QStringLiteral("主页"));
    QFont hf = heading->font(); hf.setPointSizeF(25); hf.setWeight(QFont::DemiBold); heading->setFont(hf);
    outer->addWidget(heading);

    auto *device = new QWidget();
    auto *dh = new QHBoxLayout(device);
    dh->setContentsMargins(0, 0, 0, 0); dh->setSpacing(16);
    auto *preview = new QLabel(device);
    preview->setAlignment(Qt::AlignCenter); preview->setFixedSize(128, 76);
    preview->setPixmap(Win11Style::appIcon(QStringLiteral("desktop-this-pc")).pixmap(56, 56));
    preview->setProperty("devicePreview", true);
    auto *copy = new QWidget(device); auto *cv = new QVBoxLayout(copy);
    cv->setContentsMargins(0, 3, 0, 3); cv->setSpacing(2);
    auto *host = new QLabel(QHostInfo::localHostName().toUpper(), copy);
    QFont hostf = host->font(); hostf.setPointSizeF(14); hostf.setBold(true); host->setFont(hostf);
    auto *desc = new QLabel(QStringLiteral("Lindows · ElevenDE"), copy); desc->setProperty("subtle", true);
    cv->addWidget(host); cv->addWidget(desc); cv->addStretch(1);
    dh->addWidget(preview); dh->addWidget(copy, 1);
    outer->addWidget(device);

    auto *columns = new QHBoxLayout(); columns->setSpacing(14);
    auto *left = new QVBoxLayout(); left->setSpacing(14);
    auto *right = new QVBoxLayout(); right->setSpacing(14);
    auto makeBody = [](const QList<QWidget *> &rows) {
        auto *body = new QWidget(); auto *v = new QVBoxLayout(body);
        v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
        for (QWidget *row : rows) v->addWidget(row);
        return body;
    };
    left->addWidget(makeCard(QStringLiteral("系统"), makeBody({
        homeActionRow(QStringLiteral("settings-nav-system"), QStringLiteral("系统信息"), QStringLiteral("设备名称、处理器、内存和磁盘"), 1),
        homeActionRow(QStringLiteral("settings-nav-display"), QStringLiteral("显示"), QStringLiteral("分辨率和亮度"), 3),
        homeActionRow(QStringLiteral("settings-nav-network"), QStringLiteral("网络和 Internet"), QStringLiteral("网络接口和连接状态"), 4)
    }), QStringLiteral("ElevenDE 已提供的系统功能")));
    right->addWidget(makeCard(QStringLiteral("个性化"), makeBody({
        homeActionRow(QStringLiteral("settings-nav-personalization"), QStringLiteral("背景"), QStringLiteral("壁纸设置与即时预览"), 2),
        homeActionRow(QStringLiteral("settings-nav-sound"), QStringLiteral("声音"), QStringLiteral("输出设备与音量"), 5),
        homeActionRow(QStringLiteral("settings-nav-shortcuts"), QStringLiteral("快捷键"), QStringLiteral("Windows 风格快捷键"), 6)
    }), QStringLiteral("只显示当前可配置功能")));
    left->addStretch(1); right->addStretch(1);
    columns->addLayout(left, 1); columns->addLayout(right, 1);
    outer->addLayout(columns, 1);
    return scrollOf(page);
}

/* ---------- 系统（关于） ---------- */

QWidget *buildAboutPage()
{
    auto *page = new QWidget();
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(32, 24, 32, 24);
    v->setSpacing(12);
    v->addWidget(pageHeading(QStringLiteral("系统 > 关于")));

    /* device name card */
    auto *nameRow = new QWidget();
    auto *hl = new QHBoxLayout(nameRow);
    hl->setContentsMargins(0, 0, 0, 0);
    const QString host = QHostInfo::localHostName();
    auto *nameLbl = new QLabel(QStringLiteral("设备名称：%1").arg(host));
    auto *copyBtn = new QPushButton(QStringLiteral("复制"));
    copyBtn->setFixedWidth(72);
    QObject::connect(copyBtn, &QPushButton::clicked, [host] {
        QApplication::clipboard()->setText(host);
    });
    hl->addWidget(nameLbl, 1);
    hl->addWidget(copyBtn);
    v->addWidget(makeCard(QStringLiteral("设备"), nameRow));

    /* specs card */
    auto *spec = new QWidget();
    auto *sg = new QGridLayout(spec);
    sg->setContentsMargins(0, 0, 0, 0);
    sg->setColumnStretch(1, 1);
    const QStringList keys = { QStringLiteral("处理器"), QStringLiteral("内存"),
                               QStringLiteral("磁盘 (根分区)"), QStringLiteral("操作系统"),
                               QStringLiteral("内核"), QStringLiteral("桌面环境") };
    const QStringList vals = { cpuModel(), totalRam(), rootDiskSize(),
                               osReleaseValue(QStringLiteral("PRETTY_NAME")),
                               kernelVersion(), QStringLiteral("ElevenDE 3.5.1 (X11)") };
    for (int i = 0; i < keys.size(); ++i) {
        auto *k = new QLabel(keys.at(i) + QStringLiteral("："), spec);
        k->setProperty("subtle", true);
        auto *val = new QLabel(vals.at(i), spec);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        val->setWordWrap(true);
        sg->addWidget(k, i, 0, Qt::AlignTop);
        sg->addWidget(val, i, 1, Qt::AlignTop);
    }
    v->addWidget(makeCard(QStringLiteral("设备规格"), spec));

    v->addWidget(makeCard(QStringLiteral("ElevenDE"),
                          new QLabel(QStringLiteral(
                              "ElevenDE 是一个 Windows 11 风格的开源 Linux 桌面环境，\n"
                              "基于自研 C/Xlib Shell + Openbox 窗口管理器。\n"
                              "GNU General Public License v3.0 or later.")),
                          QStringLiteral("版本 3.5.1")));
    v->addStretch(1);
    return scrollOf(page);
}

/* ---------- 个性化 ---------- */

class PersonalizationPage : public QWidget
{
public:
    PersonalizationPage()
    {
        auto *outer = new QWidget();
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 24, 32, 24);
        v->setSpacing(12);
        v->addWidget(pageHeading(QStringLiteral("个性化")));

        /* --- wallpaper --- */
        m_wallGrid = new QWidget();
        auto *wg = new QHBoxLayout(m_wallGrid);
        wg->setContentsMargins(0, 0, 0, 0);
        wg->setSpacing(10);
        v->addWidget(makeCard(QStringLiteral("背景"), m_wallGrid,
                              QStringLiteral("选择桌面壁纸，立即生效")));
        rebuildWallpapers();

        auto *pickBtn = new QPushButton(QStringLiteral("浏览图片…"));
        QObject::connect(pickBtn, &QPushButton::clicked, this, [this] {
            const QString p = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择壁纸"), QDir::homePath(),
                QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.svg)"));
            if (p.isEmpty())
                return;
            applyWallpaper(p);
        });
        v->addWidget(pickBtn, 0, Qt::AlignLeft);

        /* ElevenDE Settings is intentionally light-only. Color and accent
           controls are not exposed because the shell uses one consistent
           Windows 11 light surface across all built-in applications. */
        QSettings appearance(QStringLiteral("elevende"), QStringLiteral("elevende"));
        appearance.setValue(QStringLiteral("darkMode"), false);
        auto *lightNote = new QLabel(QStringLiteral("ElevenDE 使用统一的 Windows 11 浅色界面。"), outer);
        lightNote->setProperty("subtle", true);
        v->addWidget(lightNote);
        v->addStretch(1);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(scrollOf(outer));
    }

private:
    void rebuildWallpapers()
    {
        /* clear previous thumbnails (idempotent rebuild) */
        QLayout *lay = m_wallGrid->layout();
        while (QLayoutItem *it = lay->takeAt(0)) {
            if (it->widget())
                it->widget()->deleteLater();
            delete it;
        }
        QHBoxLayout *wg = qobject_cast<QHBoxLayout *>(lay);
        QStringList candidates;
        const QDir wdir(QStringLiteral("%1/wallpapers").arg(QLatin1String(kShareDir)));
        if (wdir.exists())
            for (const QString &n : wdir.entryList({ QStringLiteral("*.png") }, QDir::Files))
                candidates << wdir.absoluteFilePath(n);
        const QString custom = userWallpaperPath();
        QSettings wallpaperState(QStringLiteral("elevende"), QStringLiteral("elevende"));
        const QString selectedSource = wallpaperState.value(
            QStringLiteral("wallpaperSource")).toString();
        /* The active copy is always stored at `custom`.  Do not add it as a
           fourth card when it merely mirrors one of the built-in candidates;
           keep it visible only for an image picked from outside this list. */
        const bool activeCopyMirrorsBuiltin = candidates.contains(selectedSource);
        if (QFileInfo::exists(custom) && !activeCopyMirrorsBuiltin &&
            !candidates.contains(custom))
            candidates << custom;

        if (candidates.isEmpty()) {
            wg->addWidget(new QLabel(QStringLiteral("未找到壁纸（安装后将位于 %1/wallpapers）")
                                         .arg(QLatin1String(kShareDir)),
                                     m_wallGrid));
            return;
        }
        for (const QString &path : candidates) {
            auto *b = new QPushButton(m_wallGrid);
            b->setFixedSize(160, 90);
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(path);
            QPixmap pm(path);
            if (!pm.isNull()) {
                b->setIcon(QIcon(pm.scaled(156, 86, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)));
                b->setIconSize(QSize(156, 86));
            }
            b->setStyleSheet(QStringLiteral(
                "QPushButton { border:2px solid #d7dde5; border-radius:6px; padding:0; background:#ffffff; }"
                "QPushButton:hover { border:2px solid #0078D4; }"));
            QObject::connect(b, &QPushButton::clicked, this, [this, path] { applyWallpaper(path); });
            wg->addWidget(b);
        }
        wg->addStretch(1);
    }

    void applyWallpaper(const QString &src)
    {
        const QString dst = userWallpaperPath();
        QDir().mkpath(QFileInfo(dst).absolutePath());
        /* Copy through a temporary path then atomically replace the active
           wallpaper. The old code skipped copying whenever source and target
           merely shared a directory, leaving stale or missing wallpaper data. */
        if (QFileInfo(src).canonicalFilePath() != QFileInfo(dst).canonicalFilePath()) {
            const QString tmp = dst + QStringLiteral(".new");
            QFile::remove(tmp);
            if (!QFile::copy(src, tmp)) {
                QFile::remove(tmp);
                QMessageBox::critical(this, QStringLiteral("个性化"),
                                      QStringLiteral("无法写入 %1").arg(dst));
                return;
            }
            QFile::remove(dst);
            if (!QFile::rename(tmp, dst)) {
                QFile::remove(tmp);
                QMessageBox::critical(this, QStringLiteral("个性化"),
                                      QStringLiteral("无法写入 %1").arg(dst));
                return;
            }
        }
        QSettings wallpaperState(QStringLiteral("elevende"), QStringLiteral("elevende"));
        wallpaperState.setValue(QStringLiteral("wallpaperSource"), src);
        rebuildWallpapers();
        notifyShellWallpaperChanged();
    }

    QWidget *m_wallGrid;

};

/* ---------- 显示 ---------- */

struct RandRMode { QString name; int w = 0; int h = 0; };

class DisplayPage : public QWidget
{
public:
    DisplayPage()
    {
        auto *outer = new QWidget();
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 24, 32, 24);
        v->setSpacing(12);
        v->addWidget(pageHeading(QStringLiteral("显示")));

        /* resolution */
        auto *resBody = new QWidget();
        auto *rh = new QHBoxLayout(resBody);
        rh->setContentsMargins(0, 0, 0, 0);
        m_resCombo = new QComboBox(resBody);
        m_applyBtn = new QPushButton(QStringLiteral("应用"), resBody);
        m_applyBtn->setProperty("accent", true);
        rh->addWidget(m_resCombo, 1);
        rh->addWidget(m_applyBtn);
        v->addWidget(makeCard(QStringLiteral("显示分辨率"), resBody,
                              QStringLiteral("通过 xrandr 切换，需要系统已安装 x11-xserver-utils")));
        loadModes();
        QObject::connect(m_applyBtn, &QPushButton::clicked, this, &DisplayPage::applyMode);

        /* brightness */
        auto *brBody = new QWidget();
        auto *bh = new QHBoxLayout(brBody);
        bh->setContentsMargins(0, 0, 0, 0);
        m_bright = new QSlider(Qt::Horizontal, brBody);
        m_bright->setRange(1, 100);
        m_brightLbl = new QLabel(QStringLiteral("--"), brBody);
        m_brightLbl->setFixedWidth(48);
        bh->addWidget(m_bright, 1);
        bh->addWidget(m_brightLbl);
        v->addWidget(makeCard(QStringLiteral("亮度"), brBody,
                              QStringLiteral("笔记本内置屏幕背光（台式机外接显示器请用显示器按钮）")));
        loadBrightness();
        QObject::connect(m_bright, &QSlider::valueChanged, this, &DisplayPage::setBrightness);

        v->addStretch(1);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(scrollOf(outer));
    }

private:
    void loadModes()
    {
        m_output.clear();
        m_modes.clear();
        m_resCombo->clear();

        QProcess p;
        p.start(QStringLiteral("xrandr"), {});
        if (!p.waitForStarted(2000) || !p.waitForFinished(4000)) {
            m_resCombo->addItem(QStringLiteral("（xrandr 不可用）"));
            return;
        }
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        bool inConnected = false;
        for (const QString &line : out.split('\n')) {
            if (!line.startsWith(' ') && line.contains(" connected")) {
                m_output = line.section(' ', 0, 0);
                inConnected = true;
                continue;
            }
            if (!line.startsWith(' ')) {
                inConnected = false;
                continue;
            }
            if (!inConnected)
                continue;
            const QString t = line.trimmed();
            /* "1920x1080      60.00*+  59.94" */
            const QString name = t.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
            const QRegularExpression re(QStringLiteral("^(\\d+)x(\\d+)$"));
            const auto m = re.match(name);
            if (!m.hasMatch())
                continue;
            RandRMode mode;
            mode.name = name;
            mode.w = m.captured(1).toInt();
            mode.h = m.captured(2).toInt();
            m_modes << mode;
            m_resCombo->addItem(QStringLiteral("%1 × %2").arg(mode.w).arg(mode.h), name);
            if (t.contains('*'))
                m_resCombo->setCurrentIndex(m_resCombo->count() - 1);
        }
        if (m_resCombo->count() == 0)
            m_resCombo->addItem(QStringLiteral("（未检测到可切换的模式）"));
    }
    void applyMode()
    {
        const QString mode = m_resCombo->currentData().toString();
        if (mode.isEmpty() || m_output.isEmpty())
            return;
        QProcess::startDetached(QStringLiteral("xrandr"),
                                { QStringLiteral("--output"), m_output,
                                  QStringLiteral("--mode"), mode });
    }
    void loadBrightness()
    {
        m_blPath.clear();
        const QDir bl(QStringLiteral("/sys/class/backlight"));
        const QStringList devs = bl.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (devs.isEmpty()) {
            m_bright->setEnabled(false);
            m_brightLbl->setText(QStringLiteral("无"));
            return;
        }
        m_blPath = bl.absoluteFilePath(devs.first());
        const int max = readFirstLine(m_blPath + QStringLiteral("/max_brightness")).toInt();
        const int cur = readFirstLine(m_blPath + QStringLiteral("/brightness")).toInt();
        m_blMax = max > 0 ? max : 1;
        m_bright->setValue(cur * 100 / m_blMax);
        m_brightLbl->setText(QStringLiteral("%1%").arg(cur * 100 / m_blMax));
    }
    void setBrightness(int pct)
    {
        if (m_blPath.isEmpty())
            return;
        m_brightLbl->setText(QStringLiteral("%1%").arg(pct));
        QFile f(m_blPath + QStringLiteral("/brightness"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QByteArray::number(pct * m_blMax / 100));
            f.close();
        } else {
            m_brightLbl->setToolTip(QStringLiteral(
                "无写入权限。可添加 udev 规则：\n"
                "ACTION==\"add\", SUBSYSTEM==\"backlight\", RUN+=\"/bin/chgrp video /sys/class/backlight/%1/brightness\"")
                .arg(QFileInfo(m_blPath).fileName()));
        }
    }

    QComboBox *m_resCombo;
    QPushButton *m_applyBtn;
    QSlider *m_bright;
    QLabel *m_brightLbl;
    QString m_output;
    QString m_blPath;
    int m_blMax = 1;
    QList<RandRMode> m_modes;
};

/* ---------- 网络 ---------- */

QWidget *buildNetworkPage()
{
    auto *page = new QWidget();
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(32, 24, 32, 24);
    v->setSpacing(12);
    v->addWidget(pageHeading(QStringLiteral("网络")));

    auto *table = new QTableWidget(0, 4);
    table->setHorizontalHeaderLabels({ QStringLiteral("接口"), QStringLiteral("状态"),
                                       QStringLiteral("MAC"), QStringLiteral("IPv4") });
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);

    /* IPv4 addresses via getifaddrs */
    QHash<QString, QString> ipv4;
    struct ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *it = ifs; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
                continue;
            char buf[INET_ADDRSTRLEN] = { 0 };
            auto *sa = reinterpret_cast<struct sockaddr_in *>(it->ifa_addr);
            inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof buf);
            ipv4.insert(QString::fromUtf8(it->ifa_name), QString::fromUtf8(buf));
        }
        freeifaddrs(ifs);
    }

    const QDir net(QStringLiteral("/sys/class/net"));
    const QStringList ifaces = net.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : ifaces) {
        const QString state = readFirstLine(net.absoluteFilePath(name + QStringLiteral("/operstate")));
        const QString mac = readFirstLine(net.absoluteFilePath(name + QStringLiteral("/address")));
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(name));
        auto *st = new QTableWidgetItem(state == QStringLiteral("up") ? QStringLiteral("已连接") : QStringLiteral("未连接"));
        table->setItem(row, 1, st);
        table->setItem(row, 2, new QTableWidgetItem(mac));
        table->setItem(row, 3, new QTableWidgetItem(ipv4.value(name, QStringLiteral("—"))));
    }

    v->addWidget(makeCard(QStringLiteral("网络接口"), table,
                          QStringLiteral("只读概览。Wi-Fi 连接请使用 Ctrl+Alt+Delete 安全选项屏幕右下角的「网络」面板，或 nmcli。")));
    v->addStretch(1);
    return scrollOf(page);
}

/* ---------- 快捷键 ---------- */

struct ShortcutDef {
    QString id;
    QString name;
    QString keys;     /* Openbox syntax: W-e, A-F4, C-A-Delete */
    QString type;     /* exec | wm | sas */
    QString command;
    bool enabled = true;
};

QString prettyKeys(const QString &k)
{
    QString out;
    const QStringList parts = k.split('-', Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        if (!out.isEmpty())
            out += QStringLiteral(" + ");
        if (p == QStringLiteral("W")) out += QStringLiteral("Win");
        else if (p == QStringLiteral("A")) out += QStringLiteral("Alt");
        else if (p == QStringLiteral("C")) out += QStringLiteral("Ctrl");
        else if (p == QStringLiteral("S")) out += QStringLiteral("Shift");
        else if (p == QStringLiteral("XF86AudioRaiseVolume")) out += QStringLiteral("音量+");
        else if (p == QStringLiteral("XF86AudioLowerVolume")) out += QStringLiteral("音量-");
        else if (p == QStringLiteral("XF86AudioMute")) out += QStringLiteral("静音");
        else if (p == QStringLiteral("Print")) out += QStringLiteral("PrintScreen");
        else if (p == QStringLiteral("Delete")) out += QStringLiteral("Delete");
        else if (p == QStringLiteral("Escape")) out += QStringLiteral("Esc");
        else if (p == QStringLiteral("Tab")) out += QStringLiteral("Tab");
        else if (p == QStringLiteral("Up")) out += QStringLiteral("↑");
        else if (p == QStringLiteral("Down")) out += QStringLiteral("↓");
        else if (p == QStringLiteral("Left")) out += QStringLiteral("←");
        else if (p == QStringLiteral("Right")) out += QStringLiteral("→");
        else out += p;
    }
    return out;
}

class ShortcutsPage : public QWidget
{
public:
    ShortcutsPage()
    {
        auto *outer = new QWidget();
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 24, 32, 24);
        v->setSpacing(12);
        v->addWidget(pageHeading(QStringLiteral("键盘快捷键")));

        auto *hint = new QLabel(QStringLiteral(
            "默认采用 Windows 习惯按键。双击「按键」列可修改；「启用」列控制开关。\n"
            "Ctrl+Alt+Delete 安全选项屏幕由 SAS Screen 常驻监听，不受此处开关影响。\n"
            "修改后请点击「应用」，将重新生成 Openbox 配置并热重载。"));
        hint->setProperty("subtle", true);
        v->addWidget(hint);

        m_table = new QTableWidget(0, 4);
        m_table->setHorizontalHeaderLabels({ QStringLiteral("功能"), QStringLiteral("按键"),
                                             QStringLiteral("命令 / 行为"), QStringLiteral("启用") });
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        m_table->verticalHeader()->setVisible(false);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        v->addWidget(m_table, 1);

        auto *btnRow = new QWidget();
        auto *bh = new QHBoxLayout(btnRow);
        bh->setContentsMargins(0, 0, 0, 0);
        m_applyBtn = new QPushButton(QStringLiteral("应用"), btnRow);
        m_applyBtn->setProperty("accent", true);
        auto *resetBtn = new QPushButton(QStringLiteral("恢复 Windows 默认"), btnRow);
        bh->addWidget(m_applyBtn);
        bh->addWidget(resetBtn);
        bh->addStretch(1);
        v->addWidget(btnRow);

        QObject::connect(m_applyBtn, &QPushButton::clicked, this, &ShortcutsPage::apply);
        QObject::connect(resetBtn, &QPushButton::clicked, this, &ShortcutsPage::resetDefaults);
        QObject::connect(m_table, &QTableWidget::cellDoubleClicked, this, &ShortcutsPage::editKey);

        load();

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(outer);
    }

private:
    static QJsonArray readShortcutsArray(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
        return d.object().value(QStringLiteral("shortcuts")).toArray();
    }

    void load()
    {
        /* merge: system defaults, then user overrides by id */
        QMap<QString, ShortcutDef> merged;
        auto absorb = [&](const QJsonArray &arr) {
            for (const QJsonValue &jv : arr) {
                const QJsonObject o = jv.toObject();
                ShortcutDef d;
                d.id = o.value(QStringLiteral("id")).toString();
                d.name = o.value(QStringLiteral("name")).toString();
                d.keys = o.value(QStringLiteral("keys")).toString();
                d.type = o.value(QStringLiteral("type")).toString();
                d.command = o.value(QStringLiteral("command")).toString();
                d.enabled = o.value(QStringLiteral("enabled")).toBool(true);
                if (!d.id.isEmpty())
                    merged.insert(d.id, d);
            }
        };
        absorb(readShortcutsArray(systemShortcutsPath()));
        absorb(readShortcutsArray(userShortcutsPath()));

        m_defs.clear();
        for (auto it = merged.constBegin(); it != merged.constEnd(); ++it)
            m_defs << it.value();

        m_table->setRowCount(0);
        for (const ShortcutDef &d : m_defs) {
            const int row = m_table->rowCount();
            m_table->insertRow(row);
            m_table->setItem(row, 0, new QTableWidgetItem(d.name));
            auto *keyItem = new QTableWidgetItem(prettyKeys(d.keys));
            keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
            m_table->setItem(row, 1, keyItem);
            auto *cmdItem = new QTableWidgetItem(
                d.type == QStringLiteral("wm") ? QStringLiteral("窗口管理：%1").arg(d.command)
                : d.type == QStringLiteral("sas") ? QStringLiteral("SAS Screen 常驻监听")
                                                  : d.command);
            cmdItem->setFlags(cmdItem->flags() & ~Qt::ItemIsEditable);
            m_table->setItem(row, 2, cmdItem);
            auto *chk = new QTableWidgetItem(d.enabled ? QStringLiteral("✔") : QString());
            chk->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            chk->setCheckState(d.enabled ? Qt::Checked : Qt::Unchecked);
            m_table->setItem(row, 3, chk);
        }
    }

    void editKey(int row, int col)
    {
        if (col != 1 || row < 0 || row >= m_defs.size())
            return;
        ShortcutDef &d = m_defs[row];
        if (d.type == QStringLiteral("sas")) {
            QMessageBox::information(this, QStringLiteral("快捷键"),
                                     QStringLiteral("Ctrl+Alt+Delete 由 SAS Screen 常驻监听，不能在此修改。"));
            return;
        }
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("修改快捷键"));
        auto *vl = new QVBoxLayout(&dlg);
        auto *lbl = new QLabel(QStringLiteral("为「%1」按下新的组合键（支持 Win/Alt/Ctrl/Shift + 普通键）：").arg(d.name), &dlg);
        lbl->setWordWrap(true);
        auto *edit = new QLineEdit(&dlg);
        edit->setPlaceholderText(QStringLiteral("点击此处后按键…"));
        edit->installEventFilter(this);
        m_grabEdit = edit;
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        vl->addWidget(lbl);
        vl->addWidget(edit);
        vl->addWidget(bb);
        QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() == QDialog::Accepted && !m_pendingKeys.isEmpty()) {
            d.keys = m_pendingKeys;
            m_table->item(row, 1)->setText(prettyKeys(d.keys));
        }
        m_pendingKeys.clear();
        m_grabEdit = nullptr;
    }

    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (m_grabEdit && o == m_grabEdit && e->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(e);
            const int key = ke->key();
            if (key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Shift ||
                key == Qt::Key_Meta || key == Qt::Key_unknown)
                return true;   /* modifier alone: wait for the real key */
            QString combo;
            const Qt::KeyboardModifiers mods = ke->modifiers();
            if (mods & Qt::ControlModifier) combo += QStringLiteral("C-");
            if (mods & Qt::AltModifier) combo += QStringLiteral("A-");
            if (mods & Qt::ShiftModifier) combo += QStringLiteral("S-");
            if (mods & Qt::MetaModifier) combo += QStringLiteral("W-");
            combo += keyToOpenbox(key, ke->nativeVirtualKey());
            m_grabEdit->setText(prettyKeys(combo));
            m_pendingKeys = combo;
            return true;
        }
        return QWidget::eventFilter(o, e);
    }

    static QString keyToOpenbox(int qtKey, quint32 native)
    {
        switch (qtKey) {
        case Qt::Key_Return: return QStringLiteral("Return");
        case Qt::Key_Escape: return QStringLiteral("Escape");
        case Qt::Key_Tab: return QStringLiteral("Tab");
        case Qt::Key_Space: return QStringLiteral("space");
        case Qt::Key_Delete: return QStringLiteral("Delete");
        case Qt::Key_Home: return QStringLiteral("Home");
        case Qt::Key_End: return QStringLiteral("End");
        case Qt::Key_PageUp: return QStringLiteral("Prior");
        case Qt::Key_PageDown: return QStringLiteral("Next");
        case Qt::Key_Up: return QStringLiteral("Up");
        case Qt::Key_Down: return QStringLiteral("Down");
        case Qt::Key_Left: return QStringLiteral("Left");
        case Qt::Key_Right: return QStringLiteral("Right");
        case Qt::Key_Print: return QStringLiteral("Print");
        case Qt::Key_VolumeUp: return QStringLiteral("XF86AudioRaiseVolume");
        case Qt::Key_VolumeDown: return QStringLiteral("XF86AudioLowerVolume");
        case Qt::Key_VolumeMute: return QStringLiteral("XF86AudioMute");
        case Qt::Key_F1: case Qt::Key_F2: case Qt::Key_F3: case Qt::Key_F4:
        case Qt::Key_F5: case Qt::Key_F6: case Qt::Key_F7: case Qt::Key_F8:
        case Qt::Key_F9: case Qt::Key_F10: case Qt::Key_F11: case Qt::Key_F12:
            return QStringLiteral("F%1").arg(qtKey - Qt::Key_F1 + 1);
        default: break;
        }
        Q_UNUSED(native);
        const QString t = QKeySequence(qtKey).toString();
        return t.isEmpty() ? QStringLiteral("space") : t;
    }

    void writeUserJson()
    {
        QJsonArray arr;
        for (const ShortcutDef &d : m_defs) {
            QJsonObject o;
            o.insert(QStringLiteral("id"), d.id);
            o.insert(QStringLiteral("name"), d.name);
            o.insert(QStringLiteral("keys"), d.keys);
            o.insert(QStringLiteral("type"), d.type);
            o.insert(QStringLiteral("command"), d.command);
            o.insert(QStringLiteral("enabled"), d.enabled);
            arr.append(o);
        }
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("shortcuts"), arr);

        QDir().mkpath(QFileInfo(userShortcutsPath()).absolutePath());
        QFile f(userShortcutsPath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::critical(this, QStringLiteral("快捷键"),
                                  QStringLiteral("无法写入 %1").arg(userShortcutsPath()));
            return;
        }
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }

    void apply()
    {
        /* sync check states back into defs */
        for (int row = 0; row < m_table->rowCount() && row < m_defs.size(); ++row)
            m_defs[row].enabled = m_table->item(row, 3)->checkState() == Qt::Checked;
        writeUserJson();
        QProcess p;
        p.start(QStringLiteral("elevende-keybind"), { QStringLiteral("--apply") });
        if (!p.waitForStarted(2000)) {
            /* not on PATH yet (first run before install): try the full path */
            p.start(QStringLiteral("/usr/local/bin/elevende-keybind"), { QStringLiteral("--apply") });
        }
        p.waitForFinished(8000);
        if (p.exitCode() == 0)
            QMessageBox::information(this, QStringLiteral("快捷键"),
                                     QStringLiteral("已应用。Openbox 配置已重新生成并热重载。"));
        else
            QMessageBox::warning(this, QStringLiteral("快捷键"),
                                 QStringLiteral("elevende-keybind 执行失败：\n%1")
                                     .arg(QString::fromUtf8(p.readAllStandardError())));
    }

    void resetDefaults()
    {
        const auto r = QMessageBox::question(this, QStringLiteral("快捷键"),
                                             QStringLiteral("删除自定义快捷键并恢复 Windows 默认？"),
                                             QMessageBox::Yes | QMessageBox::No);
        if (r != QMessageBox::Yes)
            return;
        QFile::remove(userShortcutsPath());
        load();
        apply();
    }

    QTableWidget *m_table;
    QPushButton *m_applyBtn;
    QLineEdit *m_grabEdit = nullptr;
    QString m_pendingKeys;
    QList<ShortcutDef> m_defs;
};

/* ---------- 主窗口 ---------- */

class SettingsWindow : public QWidget
{
public:
    explicit SettingsWindow(int initialPage = 0)
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setWindowTitle(QStringLiteral("设置"));
        setWindowIcon(Win11Style::appIcon(QStringLiteral("preferences-system")));
        resize(1280, 820);
        setMinimumSize(1040, 700);
        setProperty("settingsRoot", true);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        /* Settings is intentionally a light Windows 11 surface. Do not inherit
           the desktop dark palette here; it creates a black/white split. */
        setProperty("forceLightSurface", true);
        setStyleSheet(QStringLiteral(
            "QWidget[settingsRoot=true] { background: #f5f6fb; color: #202020; }"
            "QWidget[topBar=true] { background: #f7f8fc; border-bottom: 1px solid #e5e8ef; }"
            "QToolButton[windowControl=true] { background: transparent; color: #202020; border: none; border-radius: 0; font-size: 15px; }"
            "QToolButton[windowControl=true]:hover { background: #e5e8ef; }"
            "QToolButton[windowClose=true]:hover { background: #c42b1c; color: white; }"
            "QWidget[settingsSidebar=true] { background: #f3f4f9; border-right: 1px solid #e5e8ef; }"
            "QWidget[settingsContent=true] { background: #f5f6fb; }"
            "QLabel { color: #202020; background: transparent; }"
            "QLabel[subtle=true] { color: #6d6f78; }"
            "QLabel[link=true] { color: #0067c0; }"
            "QLabel[accountAvatar=true] { background: qradialgradient(cx:.30, cy:.25, radius:1, stop:0 #b8d8f6, stop:.45 #8fb5ec, stop:1 #6f75cb); color: white; border-radius: 31px; }"
            "QLabel[devicePreview=true] { background: #ddeef8; color: #1681c3; border: 1px solid #c9dce9; border-radius: 8px; }"
            "QLabel[deviceState=true] { color: #202020; min-width: 120px; }"
            "QLabel[cloudMark=true] { color: #0b75d1; }"
            "QFrame[usageBar=true] { background: #0879d1; border-radius: 2px; max-width: 130px; }"
            "QWidget[homeRow=true] { background: transparent; border-top: 1px solid #e9ebf0; }"
            "QWidget[homeRow=true]:hover { background: #f7f9fc; }"
            "QLabel[homeGlyph=true] { color: #166fc3; }"
            "QLineEdit[settingsSearch=true] { background: #ffffff; color: #202020; border: 1px solid #dfe3ea; border-bottom: 2px solid #0879d1; border-radius: 20px; padding: 8px 16px; }"
            "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit { background: #ffffff; color: #202020; border: 1px solid #d1d5db; }"
            "QPushButton { background: #ffffff; color: #202020; border: 1px solid #d1d5db; border-radius: 5px; }"
            "QPushButton:hover { background: #f3f6fa; }"
            "QPushButton#settingsHomeRow { text-align: left; padding: 0; border: none; border-radius: 0; background: transparent; }"
            "QPushButton#settingsHomeRow:hover { background: #f7f9fc; }"));

        auto *top = new QWidget(this);
        top->setProperty("topBar", true);
        top->setFixedHeight(64);
        top->installEventFilter(this);
        auto *th = new QHBoxLayout(top);
        th->setContentsMargins(18, 8, 20, 8); th->setSpacing(16);
        auto *back = new QLabel(QStringLiteral("‹"), top);
        QFont backf = back->font(); backf.setPointSizeF(26); back->setFont(backf);
        back->setFixedWidth(24); back->setAlignment(Qt::AlignCenter);
        auto *title = new QLabel(QStringLiteral("设置"), top);
        QFont titlef = title->font(); titlef.setPointSizeF(11); titlef.setWeight(QFont::DemiBold); title->setFont(titlef);
        auto *search = new QLineEdit(top);
        search->setObjectName(QStringLiteral("settingsSearch"));
        search->setProperty("settingsSearch", true);
        search->setPlaceholderText(QStringLiteral("⌕  查找设置"));
        search->setFixedWidth(520); search->setFixedHeight(38);
        back->installEventFilter(this); title->installEventFilter(this);
        th->addWidget(back); th->addWidget(title); th->addStretch(1); th->addWidget(search);
        th->addSpacing(12);
        auto *minimize = new QToolButton(top); minimize->setProperty("windowControl", true); minimize->setText(QStringLiteral("—")); minimize->setFixedSize(44, 48);
        auto *maximize = new QToolButton(top); maximize->setProperty("windowControl", true); maximize->setText(QStringLiteral("□")); maximize->setFixedSize(44, 48);
        auto *close = new QToolButton(top); close->setProperty("windowControl", true); close->setProperty("windowClose", true); close->setText(QStringLiteral("×")); close->setFixedSize(44, 48);
        QObject::connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
        QObject::connect(maximize, &QToolButton::clicked, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
        QObject::connect(close, &QToolButton::clicked, this, &QWidget::close);
        th->addWidget(minimize); th->addWidget(maximize); th->addWidget(close);
        root->addWidget(top);

        auto *body = new QWidget(this);
        body->setProperty("settingsContent", true);
        auto *bodyLayout = new QHBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0); bodyLayout->setSpacing(0);
        auto *sidebar = new QWidget(body);
        sidebar->setProperty("settingsSidebar", true); sidebar->setFixedWidth(286);
        auto *sv = new QVBoxLayout(sidebar);
        sv->setContentsMargins(18, 22, 12, 18); sv->setSpacing(14);
        auto *account = new QWidget(sidebar);
        auto *ah = new QHBoxLayout(account); ah->setContentsMargins(0, 0, 0, 0); ah->setSpacing(12);
        auto *avatar = new QLabel(QStringLiteral("L"), account);
        avatar->setProperty("accountAvatar", true); avatar->setAlignment(Qt::AlignCenter); avatar->setFixedSize(62, 62);
        QFont avf = avatar->font(); avf.setPointSizeF(22); avf.setWeight(QFont::DemiBold); avatar->setFont(avf);
        auto *accountText = new QWidget(account); auto *atv = new QVBoxLayout(accountText);
        atv->setContentsMargins(0, 5, 0, 0); atv->setSpacing(2);
        auto *name = new QLabel(qEnvironmentVariable("USER", QStringLiteral("Lindows 用户")), accountText);
        QFont nf = name->font(); nf.setPointSizeF(10.5); nf.setBold(true); name->setFont(nf);
        auto *mail = new QLabel(QStringLiteral("本地账户"), accountText); mail->setProperty("subtle", true);
        atv->addWidget(name); atv->addWidget(mail); atv->addStretch(1);
        ah->addWidget(avatar); ah->addWidget(accountText, 1); sv->addWidget(account);

        m_nav = new QListWidget(sidebar);
        m_nav->setIconSize(QSize(22, 22)); m_nav->setSpacing(2);
        m_nav->setStyleSheet(QStringLiteral(
            "QListWidget { background: transparent; color: #202020; border: none; }"
            "QListWidget::item { color: #202020; padding: 9px 12px; border-radius: 6px; margin: 1px 0; }"
            "QListWidget::item:hover { background: #e9edf5; }"
            "QListWidget::item:selected { background: #e5e9f2; color: #1f1f1f; border-left: 3px solid #0879d1; }"));
        struct NavItem { const char *iconName; const char *label; };
        const NavItem items[] = {
            { "settings-nav-home", "主页" }, { "settings-nav-system", "系统" },
            { "settings-nav-personalization", "个性化" }, { "settings-nav-display", "显示" },
            { "settings-nav-network", "网络和 Internet" }, { "settings-nav-sound", "声音" },
            { "settings-nav-shortcuts", "快捷键" }, { "settings-nav-time", "时间和语言" },
            { "settings-nav-apps", "默认应用" }, { "settings-nav-mouse", "鼠标" },
            { "settings-nav-power", "电源" }, { "settings-nav-users", "用户" }
        };
        for (const NavItem &it : items) {
            auto *item = new QListWidgetItem(Win11Style::appIcon(QString::fromUtf8(it.iconName)),
                                               QString::fromUtf8(it.label));
            item->setSizeHint(QSize(0, 42));
            m_nav->addItem(item);
        }
        sv->addWidget(m_nav, 1);

        m_stack = new QStackedWidget(body);
        m_stack->addWidget(buildHomePage());
        m_stack->addWidget(buildAboutPage());
        m_stack->addWidget(new PersonalizationPage());
        m_stack->addWidget(new DisplayPage());
        m_stack->addWidget(buildNetworkPage());
        m_stack->addWidget(ElevenSettings::buildSoundPage());
        m_stack->addWidget(new ShortcutsPage());
        m_stack->addWidget(ElevenSettings::buildDateTimePage());
        m_stack->addWidget(ElevenSettings::buildDefaultAppsPage());
        m_stack->addWidget(ElevenSettings::buildMousePage());
        m_stack->addWidget(ElevenSettings::buildPowerPage());
        m_stack->addWidget(ElevenSettings::buildUsersPage());
        QObject::connect(m_nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
        QObject::connect(m_nav, &QListWidget::currentRowChanged, this, [search](int row) {
            if (row == 0) {
                search->clear();
                search->clearFocus();
            }
        });
        for (QWidget *row : findChildren<QWidget *>(QStringLiteral("settingsHomeRow")))
            row->installEventFilter(this);
        if (initialPage < 0 || initialPage >= m_nav->count()) initialPage = 0;
        m_nav->setCurrentRow(initialPage);
        bodyLayout->addWidget(sidebar);
        bodyLayout->addWidget(m_stack, 1);
        root->addWidget(body, 1);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress &&
            watched && (watched->property("topBar").toBool() ||
                        watched->parent() && watched->parent()->property("topBar").toBool())) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragOffset = me->globalPosition().toPoint() - frameGeometry().topLeft();
                m_dragging = true;
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove && m_dragging) {
            auto *me = static_cast<QMouseEvent *>(event);
            move(me->globalPosition().toPoint() - m_dragOffset);
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && m_dragging) {
            m_dragging = false;
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease &&
            watched && watched->objectName() == QStringLiteral("settingsHomeRow")) {
            const int target = watched->property("settingsPageIndex").toInt();
            if (target >= 0 && target < m_nav->count()) {
                m_nav->setCurrentRow(target);
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QListWidget *m_nav;
    QStackedWidget *m_stack;
    QPoint m_dragOffset;
    bool m_dragging = false;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("elevende-settings"));
    QApplication::setOrganizationName(QStringLiteral("elevende"));
    Win11Style::apply(app);

    /* --page home|about|personalization|display|network|sound|shortcuts|
       datetime|defaults|mouse|power|users (or a numeric index) deep-links
       into the Windows 11-style Settings shell. */
    int page = 0;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if ((args.at(i) == QLatin1String("--page") ||
             args.at(i) == QLatin1String("-p")) && i + 1 < args.size()) {
            const QString p = args.at(++i).toLower();
            static const char *order[] = {
                "home", "about", "personalization", "display", "network", "sound",
                "shortcuts", "datetime", "defaults", "mouse", "power", "users"
            };
            bool isNum = false;
            const int num = p.toInt(&isNum);
            if (isNum)
                page = num;
            else
                for (size_t k = 0; k < sizeof order / sizeof order[0]; ++k)
                    if (p == QLatin1String(order[k])) { page = (int)k; break; }
        }
    }
    SettingsWindow w(page);
    w.show();
    return app.exec();
}
