/*
 * elevende-taskmgr - Windows 11 style Task Manager for ElevenDE.
 *
 * Layout follows the Windows 11 Task Manager (see also SysMonTask):
 * a left icon rail (Processes / Performance / Details), a toolbar with
 * filter + "End task", a status bar with system totals, and a Performance
 * page with live CPU / Memory line graphs drawn from /proc samples.
 */

#include <QApplication>
#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <pwd.h>
#include <signal.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "../common/win11style.h"

namespace {

/* ============================ /proc sampling ============================ */

struct ProcSample {
    qint64 pid = 0;
    QString name;
    QString user;
    double cpuPct = 0.0;
    qint64 rssKb = 0;
    quint64 cpuTicks = 0;
    char state = '?';
};

static int g_ncores = qMax(1, QThread::idealThreadCount());

static quint64 readTotalCpuTicks()
{
    QFile f(QStringLiteral("/proc/stat"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray line = f.readLine();
    const QList<QByteArray> parts = line.simplified().split(' ');
    quint64 sum = 0;
    for (int i = 1; i < parts.size(); ++i)
        sum += parts.at(i).toULongLong();
    return sum;
}

static bool readProc(qint64 pid, ProcSample &out)
{
    const QByteArray raw = [&] {
        QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    const int lp = raw.indexOf('(');
    const int rp = raw.lastIndexOf(')');
    if (lp < 0 || rp < 0 || rp < lp)
        return false;
    out.pid = pid;
    out.name = QString::fromLocal8Bit(raw.mid(lp + 1, rp - lp - 1));
    if (out.name.isEmpty())
        out.name = QStringLiteral("[进程 %1]").arg(pid);

    const QList<QByteArray> f3 = raw.mid(rp + 2).split(' ');
    if (f3.size() < 22)
        return false;
    out.state = f3.at(0).isEmpty() ? '?' : f3.at(0).at(0);
    out.cpuTicks = f3.at(11).toULongLong() + f3.at(12).toULongLong();
    out.rssKb = f3.at(21).toLongLong() * (sysconf(_SC_PAGESIZE) / 1024);

    QFile sf(QStringLiteral("/proc/%1/status").arg(pid));
    if (sf.open(QIODevice::ReadOnly)) {
        for (;;) {
            const QByteArray l = sf.readLine();
            if (l.isEmpty())
                break;
            if (l.startsWith("Uid:")) {
                const QList<QByteArray> u = l.split('\t');
                if (u.size() >= 2) {
                    const uid_t uid = (uid_t)u.at(1).trimmed().toUInt();
                    if (struct passwd *pw = getpwuid(uid))
                        out.user = QString::fromLocal8Bit(pw->pw_name);
                    else
                        out.user = QString::number(uid);
                }
                break;
            }
        }
    }
    return true;
}

static QHash<qint64, ProcSample> scanAll()
{
    QHash<qint64, ProcSample> out;
    const QStringList entries = QDir(QStringLiteral("/proc"))
                                    .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &e : entries) {
        bool ok = false;
        const qint64 pid = e.toLongLong(&ok);
        if (!ok)
            continue;
        ProcSample s;
        if (readProc(pid, s))
            out.insert(pid, s);
    }
    return out;
}

static QString cpuModelName()
{
    QFile f(QStringLiteral("/proc/cpuinfo"));
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("未知处理器");
    for (;;) {
        const QString line = QString::fromUtf8(f.readLine());
        if (line.isEmpty())
            break;
        if (line.startsWith(QStringLiteral("model name")))
            return line.mid(line.indexOf(':') + 1).trimmed();
    }
    return QStringLiteral("未知处理器");
}

/* ============================ columns ============================ */

enum Col { ColName = 0, ColPid, ColUser, ColCpu, ColMem, ColCount };

class ProcModel : public QStandardItemModel
{
    Q_OBJECT
public:
    explicit ProcModel(QObject *parent = nullptr) : QStandardItemModel(parent)
    {
        setColumnCount(ColCount);
        setHorizontalHeaderLabels({ QStringLiteral("名称"), QStringLiteral("PID"),
                                    QStringLiteral("用户"), QStringLiteral("CPU"),
                                    QStringLiteral("内存") });
    }
    void refresh()
    {
        static QHash<qint64, ProcSample> prev;
        static quint64 prevTotal = 0;

        const quint64 totalNow = readTotalCpuTicks();
        const quint64 totalDelta = (prevTotal && totalNow > prevTotal) ? (totalNow - prevTotal) : 0;
        QHash<qint64, ProcSample> now = scanAll();

        double totalCpu = 0;
        qint64 totalRss = 0;
        for (auto it = now.begin(); it != now.end(); ++it) {
            ProcSample &s = it.value();
            const auto pit = prev.constFind(it.key());
            if (pit != prev.constEnd() && totalDelta > 0) {
                const quint64 d = s.cpuTicks > pit->cpuTicks ? s.cpuTicks - pit->cpuTicks : 0;
                s.cpuPct = 100.0 * (double)d * g_ncores / (double)totalDelta;
                if (s.cpuPct > 100.0 * g_ncores)
                    s.cpuPct = 100.0 * g_ncores;
            }
            totalCpu += s.cpuPct;
            totalRss += s.rssKb;
        }

        removeRows(0, rowCount());
        for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
            const ProcSample &s = it.value();
            auto *name = new QStandardItem(s.name);
            name->setData(s.pid, Qt::UserRole);
            auto *cpu = new QStandardItem(QStringLiteral("%1%").arg(s.cpuPct, 0, 'f', 1));
            cpu->setData(s.cpuPct, Qt::UserRole + 1);
            auto *mem = new QStandardItem(
                s.rssKb >= 1024 * 1024
                    ? QStringLiteral("%1 GB").arg(s.rssKb / (1024.0 * 1024.0), 0, 'f', 2)
                    : QStringLiteral("%1 MB").arg(s.rssKb / 1024.0, 0, 'f', 1));
            mem->setData(s.rssKb, Qt::UserRole + 1);
            appendRow({ name, new QStandardItem(QString::number(s.pid)),
                        new QStandardItem(s.user), cpu, mem });
        }

        prev = now;
        prevTotal = totalNow;
        emit refreshed(rowCount(), totalCpu, totalRss);
    }

signals:
    void refreshed(int count, double totalCpu, qint64 totalRssKb);
};

class FilterProxy : public QSortFilterProxyModel
{
public:
    explicit FilterProxy(QObject *parent = nullptr) : QSortFilterProxyModel(parent)
    {
        setSortRole(Qt::UserRole + 1);
        setFilterCaseSensitivity(Qt::CaseInsensitive);
        setFilterKeyColumn(ColName);
    }
protected:
    bool lessThan(const QModelIndex &l, const QModelIndex &r) const override
    {
        if (l.column() == ColCpu || l.column() == ColMem)
            return l.data(Qt::UserRole + 1).toDouble() < r.data(Qt::UserRole + 1).toDouble();
        return QSortFilterProxyModel::lessThan(l, r);
    }
};

/* ============================ graph widget ============================ */

class Graph : public QWidget
{
public:
    explicit Graph(const QColor &line, QWidget *parent = nullptr)
        : QWidget(parent), m_line(line)
    {
        setMinimumHeight(150);
    }
    void push(double pct)
    {
        m_data.append(qBound(0.0, pct, 100.0));
        while (m_data.size() > 120)
            m_data.removeFirst();
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(32, 32, 38));
        /* grid */
        p.setPen(QPen(QColor(60, 60, 68), 1));
        for (int i = 1; i < 4; ++i)
            p.drawLine(0, height() * i / 4, width(), height() * i / 4);
        if (m_data.size() < 2)
            return;
        const double step = (double)width() / 119.0;
        QPainterPath path;
        for (int i = 0; i < m_data.size(); ++i) {
            const double x = width() - (m_data.size() - 1 - i) * step;
            const double y = height() - 4 - (height() - 8) * m_data.at(i) / 100.0;
            if (i == 0)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
        }
        /* fill under the line */
        QPainterPath fill = path;
        fill.lineTo(width(), height());
        fill.lineTo(path.elementAt(0).x, height());
        fill.closeSubpath();
        QColor fc = m_line;
        fc.setAlpha(40);
        p.fillPath(fill, fc);
        p.setPen(QPen(m_line, 2));
        p.drawPath(path);
    }
private:
    QColor m_line;
    QList<double> m_data;
};

/* ============================ performance page ============================ */

class PerformancePage : public QWidget
{
public:
    PerformancePage()
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(16, 16, 16, 16);
        root->setSpacing(12);

        m_timer = new QTimer(this);

        /* CPU card */
        auto *cpuCard = new QWidget();
        cpuCard->setProperty("card", true);
        auto *cv = new QVBoxLayout(cpuCard);
        cv->setContentsMargins(16, 12, 16, 12);
        m_cpuTitle = new QLabel(QStringLiteral("CPU"), cpuCard);
        QFont tf = m_cpuTitle->font();
        tf.setPointSizeF(12);
        tf.setBold(true);
        m_cpuTitle->setFont(tf);
        m_cpuPct = new QLabel(QStringLiteral("0%"), cpuCard);
        QFont pf = m_cpuPct->font();
        pf.setPointSizeF(22);
        m_cpuPct->setFont(pf);
        m_cpuGraph = new Graph(QColor(0x4C, 0xC2, 0xFF), cpuCard);
        m_cpuStats = new QLabel(cpuCard);
        m_cpuStats->setProperty("subtle", true);
        auto *ch = new QHBoxLayout();
        ch->addWidget(m_cpuTitle);
        ch->addStretch(1);
        ch->addWidget(m_cpuPct);
        cv->addLayout(ch);
        cv->addWidget(m_cpuGraph, 1);
        cv->addWidget(m_cpuStats);
        root->addWidget(cpuCard, 3);

        /* Memory card */
        auto *memCard = new QWidget();
        memCard->setProperty("card", true);
        auto *mv = new QVBoxLayout(memCard);
        mv->setContentsMargins(16, 12, 16, 12);
        auto *memTitle = new QLabel(QStringLiteral("内存"), memCard);
        memTitle->setFont(tf);
        m_memPct = new QLabel(QStringLiteral("0%"), memCard);
        m_memPct->setFont(pf);
        m_memGraph = new Graph(QColor(0x6B, 0x69, 0xD6), memCard);
        m_memStats = new QLabel(memCard);
        m_memStats->setProperty("subtle", true);
        auto *mh = new QHBoxLayout();
        mh->addWidget(memTitle);
        mh->addStretch(1);
        mh->addWidget(m_memPct);
        mv->addLayout(mh);
        mv->addWidget(m_memGraph, 1);
        mv->addWidget(m_memStats);
        root->addWidget(memCard, 3);

        m_cpuStats->setText(QStringLiteral("%1 · %2 核 · 运行时间 %3")
                                .arg(cpuModelName())
                                .arg(g_ncores)
                                .arg(uptimeStr()));
        m_timer->setInterval(1000);
        QObject::connect(m_timer, &QTimer::timeout, this, &PerformancePage::sample);
        m_timer->start();
        sample();
    }

private:
    static QString uptimeStr()
    {
        struct sysinfo si;
        if (sysinfo(&si) != 0)
            return QStringLiteral("?");
        const long s = si.uptime;
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600, 2, 10, QLatin1Char('0'))
            .arg((s % 3600) / 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60, 2, 10, QLatin1Char('0'));
    }
    void sample()
    {
        static quint64 prevTotal = 0;
        const quint64 now = readTotalCpuTicks();
        double cpuPct = 0;
        if (prevTotal && now > prevTotal) {
            /* busy = total delta - idle delta; approximate with total only */
            QFile f(QStringLiteral("/proc/stat"));
            if (f.open(QIODevice::ReadOnly)) {
                const QList<QByteArray> parts = f.readLine().simplified().split(' ');
                if (parts.size() > 4) {
                    const quint64 idle = parts.at(4).toULongLong();
                    static quint64 prevIdle = 0;
                    const quint64 dt = now - prevTotal;
                    const quint64 di = idle > prevIdle ? idle - prevIdle : 0;
                    cpuPct = dt ? 100.0 * (double)(dt - di) / dt : 0;
                    prevIdle = idle;
                }
            }
        }
        prevTotal = now;
        m_cpuPct->setText(QStringLiteral("%1%").arg(cpuPct, 0, 'f', 0));
        m_cpuGraph->push(cpuPct);
        m_cpuStats->setText(QStringLiteral("%1 · %2 核 · 运行时间 %3")
                                .arg(cpuModelName())
                                .arg(g_ncores)
                                .arg(uptimeStr()));

        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            const double usedGb = (si.totalram - si.freeram) / (1024.0 * 1024 * 1024);
            const double totGb = si.totalram / (1024.0 * 1024 * 1024);
            const double pct = totGb > 0 ? usedGb / totGb * 100.0 : 0;
            m_memPct->setText(QStringLiteral("%1%").arg(pct, 0, 'f', 0));
            m_memGraph->push(pct);
            m_memStats->setText(QStringLiteral("已用 %1 GB / 共 %2 GB")
                                    .arg(usedGb, 0, 'f', 1)
                                    .arg(totGb, 0, 'f', 1));
        }
    }

    QTimer *m_timer;
    QLabel *m_cpuTitle;
    QLabel *m_cpuPct;
    QLabel *m_cpuStats;
    Graph *m_cpuGraph;
    QLabel *m_memPct;
    QLabel *m_memStats;
    Graph *m_memGraph;
};

/* ============================ processes page ============================ */

class ProcessesPage : public QWidget
{
    Q_OBJECT
public:
    explicit ProcessesPage(ProcModel *model, FilterProxy *proxy)
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 8);
        root->setSpacing(8);

        /* toolbar */
        auto *bar = new QWidget();
        auto *hl = new QHBoxLayout(bar);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(8);
        m_filter = new QLineEdit(bar);
        m_filter->setPlaceholderText(QStringLiteral("搜索进程…"));
        m_filter->setClearButtonEnabled(true);
        m_endBtn = new QPushButton(QStringLiteral("结束任务"), bar);
        m_endBtn->setProperty("accent", true);
        m_endBtn->setEnabled(false);
        hl->addWidget(m_filter, 1);
        hl->addWidget(m_endBtn);
        root->addWidget(bar);

        m_tree = new QTreeView();
        m_tree->setModel(proxy);
        m_tree->setRootIsDecorated(false);
        m_tree->setAlternatingRowColors(true);
        m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_tree->setSortingEnabled(true);
        m_tree->sortByColumn(ColCpu, Qt::DescendingOrder);
        m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
        m_tree->setColumnWidth(ColName, 260);
        m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
        root->addWidget(m_tree, 1);

        QObject::connect(m_filter, &QLineEdit::textChanged, proxy, &FilterProxy::setFilterFixedString);
        QObject::connect(m_endBtn, &QPushButton::clicked, this, [this] { killSelected(SIGTERM); });
        QObject::connect(m_tree, &QTreeView::customContextMenuRequested, this, [this](const QPoint &pos) {
            QMenu menu(this);
            auto *end = menu.addAction(QStringLiteral("结束任务 (&E)"));
            auto *kill = menu.addAction(QStringLiteral("强制结束 (&K)"));
            QAction *picked = menu.exec(m_tree->viewport()->mapToGlobal(pos));
            if (picked == end)
                killSelected(SIGTERM);
            else if (picked == kill)
                killSelected(SIGKILL);
        });
        QObject::connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged,
                         this, [this] {
                             const bool has = m_tree->selectionModel()->hasSelection();
                             m_endBtn->setEnabled(has);
                         });
    }

private:
    QList<qint64> selectedPids() const
    {
        QList<qint64> out;
        const QModelIndexList rows = m_tree->selectionModel()->selectedRows(ColName);
        for (const QModelIndex &idx : rows)
            out << idx.data(Qt::UserRole).toLongLong();
        return out;
    }
    void killSelected(int sig)
    {
        const QList<qint64> pids = selectedPids();
        if (pids.isEmpty())
            return;
        if (sig == SIGKILL &&
            QMessageBox::question(this, QStringLiteral("任务管理器"),
                                  QStringLiteral("确定要强制结束选中的 %1 个进程吗？\n未保存的数据将会丢失。")
                                      .arg(pids.size()),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        QStringList failed;
        for (qint64 pid : pids)
            if (::kill((pid_t)pid, sig) != 0)
                failed << QString::number(pid);
        if (!failed.isEmpty())
            QMessageBox::warning(this, QStringLiteral("任务管理器"),
                                 QStringLiteral("以下进程无法结束（可能是系统进程）：\n%1")
                                     .arg(failed.join(QStringLiteral(", "))));
    }

    QLineEdit *m_filter;
    QPushButton *m_endBtn;
    QTreeView *m_tree;
};

/* ============================ details page ============================ */

class DetailsPage : public QWidget
{
public:
    DetailsPage()
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 8);
        root->setSpacing(8);
        auto *hint = new QLabel(QStringLiteral("全部进程（PID / 状态 / 用户 / CPU / 内存）"), this);
        hint->setProperty("subtle", true);
        m_model = new QStandardItemModel(this);
        m_model->setHorizontalHeaderLabels({ QStringLiteral("PID"), QStringLiteral("名称"),
                                             QStringLiteral("状态"), QStringLiteral("用户"),
                                             QStringLiteral("CPU"), QStringLiteral("内存") });
        m_view = new QTreeView(this);
        m_view->setModel(m_model);
        m_view->setRootIsDecorated(false);
        m_view->setAlternatingRowColors(true);
        m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        root->addWidget(hint);
        root->addWidget(m_view, 1);

        m_timer = new QTimer(this);
        QObject::connect(m_timer, &QTimer::timeout, this, &DetailsPage::refresh);
        m_timer->start(2000);
        refresh();
    }

private:
    void refresh()
    {
        static QHash<qint64, quint64> prevTicks;
        static quint64 prevTotal = 0;
        const quint64 totalNow = readTotalCpuTicks();
        const quint64 dt = (prevTotal && totalNow > prevTotal) ? totalNow - prevTotal : 0;
        const QHash<qint64, ProcSample> all = scanAll();

        m_model->removeRows(0, m_model->rowCount());
        QHash<qint64, quint64> newTicks;
        for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
            const ProcSample &s = it.value();
            double pct = 0;
            const auto pt = prevTicks.constFind(s.pid);
            if (pt != prevTicks.constEnd() && dt > 0 && s.cpuTicks > *pt)
                pct = 100.0 * (double)(s.cpuTicks - *pt) * g_ncores / (double)dt;
            newTicks.insert(s.pid, s.cpuTicks);
            QString st;
            switch (s.state) {
            case 'R': st = QStringLiteral("运行中"); break;
            case 'S': st = QStringLiteral("睡眠"); break;
            case 'D': st = QStringLiteral("等待 I/O"); break;
            case 'Z': st = QStringLiteral("僵尸"); break;
            case 'T': st = QStringLiteral("已停止"); break;
            default: st = QString(QChar(s.state)); break;
            }
            m_model->appendRow({
                new QStandardItem(QString::number(s.pid)),
                new QStandardItem(s.name),
                new QStandardItem(st),
                new QStandardItem(s.user),
                new QStandardItem(QStringLiteral("%1%").arg(pct, 0, 'f', 1)),
                new QStandardItem(QStringLiteral("%1 MB").arg(s.rssKb / 1024.0, 0, 'f', 1)),
            });
        }
        prevTicks = newTicks;
        prevTotal = totalNow;
    }

    QStandardItemModel *m_model;
    QTreeView *m_view;
    QTimer *m_timer;
};

/* ============================ main window ============================ */

class TaskManager : public QWidget
{
    Q_OBJECT
public:
    TaskManager()
    {
        setWindowTitle(QStringLiteral("任务管理器"));
        setWindowIcon(Win11Style::appIcon(QStringLiteral("utilities-system-monitor")));
        resize(860, 640);

        auto *root = new QHBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        /* ---- left icon rail (Win11 style) ---- */
        auto *rail = new QWidget();
        rail->setFixedWidth(56);
        rail->setStyleSheet(QStringLiteral(
            "QWidget { background: rgba(255,255,255,0.03); border-right: 1px solid #3d3d3d; }"
            "QToolButton { background: transparent; border: none; border-radius: 6px;"
            " font-size: 17px; padding: 8px 0; margin: 2px 8px; color: #cfcfcf; }"
            "QToolButton:hover { background: rgba(255,255,255,0.07); }"
            "QToolButton:checked { background: rgba(255,255,255,0.10); color: #ffffff; }"));
        auto *rv = new QVBoxLayout(rail);
        rv->setContentsMargins(0, 10, 0, 10);
        rv->setSpacing(2);

        struct RailItem { const char *glyph; const char *tip; };
        const RailItem items[] = {
            { "\xE2\x96\xA6", "进程" },      /* ▦ */
            { "\xF0\x9F\x93\x88", "性能" },  /* 📈 */
            { "\xE2\x89\xA1", "详细信息" },  /* ≡ */
        };
        auto *group = new QButtonGroup(this);
        group->setExclusive(true);
        for (size_t i = 0; i < sizeof items / sizeof items[0]; ++i) {
            auto *b = new QToolButton(rail);
            b->setText(QString::fromUtf8(items[i].glyph));
            b->setToolTip(QString::fromUtf8(items[i].tip));
            b->setCheckable(true);
            b->setFixedHeight(40);
            b->setCursor(Qt::PointingHandCursor);
            rv->addWidget(b);
            group->addButton(b, (int)i);
            if (i == 0)
                b->setChecked(true);
        }
        rv->addStretch(1);

        /* ---- pages ---- */
        m_stack = new QStackedWidget();
        m_model = new ProcModel(this);
        m_proxy = new FilterProxy(this);
        m_proxy->setSourceModel(m_model);
        m_stack->addWidget(new ProcessesPage(m_model, m_proxy));
        m_stack->addWidget(new PerformancePage());
        m_stack->addWidget(new DetailsPage());

        QObject::connect(group, QOverload<int>::of(&QButtonGroup::idClicked),
                         m_stack, &QStackedWidget::setCurrentIndex);

        /* ---- status bar ---- */
        auto *right = new QWidget();
        auto *rvv = new QVBoxLayout(right);
        rvv->setContentsMargins(0, 0, 0, 0);
        rvv->setSpacing(0);
        rvv->addWidget(m_stack, 1);
        m_status = new QStatusBar();
        m_cnt = new QLabel();
        m_cpu = new QLabel();
        m_mem = new QLabel();
        m_status->addPermanentWidget(m_cnt);
        m_status->addPermanentWidget(m_cpu);
        m_status->addPermanentWidget(m_mem);
        rvv->addWidget(m_status);

        root->addWidget(rail);
        root->addWidget(right, 1);

        QObject::connect(m_model, &ProcModel::refreshed, this, &TaskManager::onRefreshed);
        m_model->refresh();
        m_timer = new QTimer(this);
        QObject::connect(m_timer, &QTimer::timeout, m_model, &ProcModel::refresh);
        m_timer->start(2000);
    }

private slots:
    void onRefreshed(int count, double totalCpu, qint64 totalRssKb)
    {
        struct sysinfo si;
        sysinfo(&si);
        m_cnt->setText(QStringLiteral("进程: %1").arg(count));
        m_cpu->setText(QStringLiteral("CPU: %1%").arg(totalCpu, 0, 'f', 0));
        m_mem->setText(QStringLiteral("内存: %1 / %2 MB")
                           .arg(totalRssKb / 1024)
                           .arg(si.totalram / (1024 * 1024)));
    }

private:
    QStackedWidget *m_stack;
    ProcModel *m_model;
    FilterProxy *m_proxy;
    QStatusBar *m_status;
    QLabel *m_cnt;
    QLabel *m_cpu;
    QLabel *m_mem;
    QTimer *m_timer;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("elevende-taskmgr"));
    QApplication::setOrganizationName(QStringLiteral("elevende"));
    Win11Style::apply(app);

    TaskManager w;
    w.show();
    return app.exec();
}

#include "main.moc"
