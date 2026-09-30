/*
 * extra.cpp - extended Settings pages for ElevenDE:
 *   声音 / 时间与语言 / 默认应用 / 鼠标 / 电源 / 用户
 * Completes the desktop ecosystem alongside the core pages in main.cpp.
 * Backend tools used (all standard on Debian-family systems):
 *   pactl/amixer, timedatectl, xdg-mime, xset, systemctl, getent.
 */

#include "pages.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <pwd.h>
#include <unistd.h>

namespace ElevenSettings {

namespace {

/* ---------- shared helpers (mirrors main.cpp) ---------- */

QWidget *xCard(const QString &title, QWidget *body, const QString &subtitle = QString())
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

QLabel *xHeading(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setPointSizeF(19);
    f.setWeight(QFont::DemiBold);
    l->setFont(f);
    return l;
}

QWidget *xScroll(QWidget *inner)
{
    auto *sa = new QScrollArea();
    sa->setWidgetResizable(true);
    sa->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setWidget(inner);
    return sa;
}

QString runOut(const QString &prog, const QStringList &args)
{
    QProcess p;
    p.start(prog, args);
    if (!p.waitForStarted(2000) || !p.waitForFinished(5000))
        return QString();
    return QString::fromUtf8(p.readAllStandardOutput());
}

bool haveCmd(const QString &prog)
{
    QProcess p;
    p.start(QStringLiteral("sh"), { QStringLiteral("-c"),
                                     QStringLiteral("command -v %1").arg(prog) });
    if (!p.waitForStarted(2000) || !p.waitForFinished(3000))
        return false;
    return p.exitCode() == 0;
}

/* ---------- 声音 ---------- */

class SoundPage : public QWidget
{
public:
    SoundPage()
    {
        m_pactl = haveCmd(QStringLiteral("pactl"));

        auto *outer = new QWidget();
        outer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 20, 32, 20);
        v->setSpacing(8);
        auto *heading = xHeading(QStringLiteral("声音"));
        heading->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        v->addWidget(heading);

        /* volume */
        auto *volBody = new QWidget();
        auto *vh = new QHBoxLayout(volBody);
        vh->setContentsMargins(0, 0, 0, 0);
        vh->setSpacing(12);
        m_mute = new QCheckBox(QStringLiteral("静音"), volBody);
        m_mute->setFixedWidth(70);
        m_vol = new QSlider(Qt::Horizontal, volBody);
        m_vol->setRange(0, 100);
        m_vol->setTracking(true);
        m_vol->setSingleStep(1);
        m_vol->setPageStep(5);
        m_volLbl = new QLabel(QStringLiteral("--"), volBody);
        m_volLbl->setFixedWidth(48);
        vh->addWidget(m_mute);
        vh->addWidget(m_vol, 1);
        vh->addWidget(m_volLbl);
        auto *volumeCard = xCard(QStringLiteral("主音量"), volBody);
        volumeCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        v->addWidget(volumeCard);

        /* output device */
        auto *outBody = new QWidget();
        auto *oh = new QHBoxLayout(outBody);
        oh->setContentsMargins(0, 0, 0, 0);
        m_sink = new QComboBox(outBody);
        auto *applyBtn = new QPushButton(QStringLiteral("设为默认"), outBody);
        applyBtn->setProperty("accent", true);
        oh->addWidget(m_sink, 1);
        oh->addWidget(applyBtn);
        auto *outputCard = xCard(QStringLiteral("输出设备"), outBody,
                                 QStringLiteral("选择声音播放到哪里（pactl）"));
        outputCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        v->addWidget(outputCard);
        v->addStretch(1);

        m_volumeTimer.setSingleShot(true);
        QObject::connect(m_vol, &QSlider::valueChanged, this, [this](int value) {
            m_volLbl->setText(QStringLiteral("%1%").arg(value));
            m_volumeTimer.start(35);
        });
        QObject::connect(&m_volumeTimer, &QTimer::timeout, this, [this] {
            setVolume(m_vol->value());
        });
        QObject::connect(m_mute, &QCheckBox::toggled, this, &SoundPage::setMute);
        QObject::connect(applyBtn, &QPushButton::clicked, this, &SoundPage::applySink);

        loadState();

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(outer);
    }

private:
    void volCmd(const QString &what, const QString &val)
    {
        if (m_pactl)
            QProcess::startDetached(QStringLiteral("pactl"),
                                    { QStringLiteral("set-sink-volume"),
                                      QStringLiteral("@DEFAULT_SINK@"), val });
        else
            QProcess::startDetached(QStringLiteral("amixer"),
                                    { QStringLiteral("-q"), QStringLiteral("sset"),
                                      QStringLiteral("Master"), val });
        Q_UNUSED(what);
    }
    void setVolume(int pct)
    {
        m_volLbl->setText(QStringLiteral("%1%").arg(pct));
        volCmd(QStringLiteral("vol"), QStringLiteral("%1%").arg(pct));
    }
    void setMute(bool on)
    {
        if (m_pactl)
            QProcess::startDetached(QStringLiteral("pactl"),
                                    { QStringLiteral("set-sink-mute"),
                                      QStringLiteral("@DEFAULT_SINK@"),
                                      on ? QStringLiteral("1") : QStringLiteral("0") });
        else
            QProcess::startDetached(QStringLiteral("amixer"),
                                    { QStringLiteral("-q"), QStringLiteral("sset"),
                                      QStringLiteral("Master"),
                                      on ? QStringLiteral("mute") : QStringLiteral("unmute") });
    }
    void applySink()
    {
        const QString name = m_sink->currentData().toString();
        if (name.isEmpty() || !m_pactl) {
            QMessageBox::information(this, QStringLiteral("声音"),
                                     QStringLiteral("需要 pactl（pulseaudio-utils / pipewire-pulse）才能切换输出设备。"));
            return;
        }
        QProcess::startDetached(QStringLiteral("pactl"),
                                { QStringLiteral("set-default-sink"), name });
    }
    void loadState()
    {
        if (m_pactl) {
            const QString info = runOut(QStringLiteral("pactl"),
                                        { QStringLiteral("get-sink-volume"),
                                          QStringLiteral("@DEFAULT_SINK@") });
            const int pct = info.indexOf('%') > 0
                ? info.left(info.indexOf('%')).trimmed().section(' ', -1).toInt()
                : -1;
            if (pct >= 0) {
                m_vol->setValue(pct);
                m_volLbl->setText(QStringLiteral("%1%").arg(pct));
            }
            const QString mute = runOut(QStringLiteral("pactl"),
                                        { QStringLiteral("get-sink-mute"),
                                          QStringLiteral("@DEFAULT_SINK@") });
            m_mute->setChecked(mute.contains(QStringLiteral("yes")));
            /* sinks */
            const QString sinks = runOut(QStringLiteral("pactl"),
                                         { QStringLiteral("list"), QStringLiteral("short"),
                                           QStringLiteral("sinks") });
            for (const QString &line : sinks.split('\n')) {
                const QStringList cols = line.split('\t', Qt::SkipEmptyParts);
                if (cols.size() >= 2)
                    m_sink->addItem(cols.value(1), cols.value(1));
            }
        } else if (haveCmd(QStringLiteral("amixer"))) {
            const QString s = runOut(QStringLiteral("amixer"),
                                     { QStringLiteral("sget"), QStringLiteral("Master") });
            const int a = s.indexOf('['), b = s.indexOf('%');
            if (a >= 0 && b > a) {
                const int pct = s.mid(a + 1, b - a - 1).toInt();
                m_vol->setValue(pct);
                m_volLbl->setText(QStringLiteral("%1%").arg(pct));
            }
            m_mute->setChecked(s.contains(QStringLiteral("[off]")));
            m_sink->addItem(QStringLiteral("系统默认 (amixer)"), QString());
        } else {
            m_vol->setEnabled(false);
            m_mute->setEnabled(false);
            m_sink->addItem(QStringLiteral("未检测到 pactl / amixer"), QString());
        }
    }

    bool m_pactl;
    QCheckBox *m_mute;
    QSlider *m_vol;
    QLabel *m_volLbl;
    QComboBox *m_sink;
    QTimer m_volumeTimer;
};

/* ---------- 时间 ---------- */

class DateTimePage : public QWidget
{
public:
    DateTimePage()
    {
        m_timedatectl = haveCmd(QStringLiteral("timedatectl"));

        auto *outer = new QWidget();
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 24, 32, 24);
        v->setSpacing(12);
        v->addWidget(xHeading(QStringLiteral("时间和语言")));

        /* clock */
        m_clock = new QLabel();
        QFont cf = m_clock->font();
        cf.setPointSizeF(30);
        cf.setWeight(QFont::DemiBold);
        m_clock->setFont(cf);
        auto *clockCard = new QWidget();
        clockCard->setProperty("card", true);
        auto *cv = new QVBoxLayout(clockCard);
        cv->setContentsMargins(16, 16, 16, 16);
        cv->addWidget(m_clock);
        v->addWidget(clockCard);
        m_timer = new QTimer(this);
        QObject::connect(m_timer, &QTimer::timeout, this, &DateTimePage::tick);
        m_timer->start(1000);
        tick();

        /* timezone */
        auto *tzBody = new QWidget();
        auto *th = new QHBoxLayout(tzBody);
        th->setContentsMargins(0, 0, 0, 0);
        m_tz = new QComboBox(tzBody);
        m_tz->setEditable(true);
        m_tz->lineEdit()->setPlaceholderText(QStringLiteral("如 Asia/Shanghai"));
        auto *tzBtn = new QPushButton(QStringLiteral("应用"), tzBody);
        th->addWidget(m_tz, 1);
        th->addWidget(tzBtn);
        v->addWidget(xCard(QStringLiteral("时区"), tzBody));
        if (m_timedatectl) {
            const QString cur = runOut(QStringLiteral("timedatectl"),
                                       { QStringLiteral("show"),
                                         QStringLiteral("-p"), QStringLiteral("Timezone"),
                                         QStringLiteral("--value") }).trimmed();
            m_tz->setCurrentText(cur.isEmpty() ? QStringLiteral("Asia/Shanghai") : cur);
            const QString zones = runOut(QStringLiteral("timedatectl"),
                                         { QStringLiteral("list-timezones") });
            for (const QString &z : zones.split('\n'))
                if (!z.trimmed().isEmpty())
                    m_tz->addItem(z.trimmed());
            m_tz->setCurrentText(cur);
        }
        QObject::connect(tzBtn, &QPushButton::clicked, this, [this] {
            const QString z = m_tz->currentText().trimmed();
            if (z.isEmpty())
                return;
            QProcess p;
            p.start(QStringLiteral("timedatectl"), { QStringLiteral("set-timezone"), z });
            p.waitForFinished(4000);
            if (p.exitCode() != 0)
                QMessageBox::warning(this, QStringLiteral("时区"),
                                     QStringLiteral("设置失败（可能需要管理员权限）：\n%1")
                                         .arg(QString::fromUtf8(p.readAllStandardError())));
        });

        /* NTP */
        if (m_timedatectl) {
            auto *ntpBody = new QWidget();
            auto *nh = new QHBoxLayout(ntpBody);
            nh->setContentsMargins(0, 0, 0, 0);
            m_ntp = new QCheckBox(QStringLiteral("自动设置时间（网络 NTP）"), ntpBody);
            nh->addWidget(m_ntp);
            nh->addStretch(1);
            v->addWidget(xCard(QStringLiteral("同步"), ntpBody));
            const QString st = runOut(QStringLiteral("timedatectl"),
                                      { QStringLiteral("show"), QStringLiteral("-p"),
                                        QStringLiteral("NTP"), QStringLiteral("--value") });
            m_ntp->setChecked(st.trimmed() == QStringLiteral("yes"));
            QObject::connect(m_ntp, &QCheckBox::toggled, this, [](bool on) {
                QProcess::startDetached(QStringLiteral("timedatectl"),
                                        { QStringLiteral("set-ntp"),
                                          on ? QStringLiteral("true") : QStringLiteral("false") });
            });
        }

        v->addStretch(1);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(xScroll(outer));
    }

private:
    void tick()
    {
        m_clock->setText(QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy/MM/dd  dddd  HH:mm:ss")));
    }
    bool m_timedatectl;
    QLabel *m_clock;
    QTimer *m_timer;
    QComboBox *m_tz;
    QCheckBox *m_ntp;
};

/* ---------- 默认应用 ---------- */

struct AppEntry { QString desktop; QString name; };

class DefaultAppsPage : public QWidget
{
public:
    DefaultAppsPage()
    {
        auto *outer = new QWidget();
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 24, 32, 24);
        v->setSpacing(12);
        v->addWidget(xHeading(QStringLiteral("默认应用")));
        v->addWidget(new QLabel(QStringLiteral(
            "双击文件时用哪个程序打开，由文件类型（MIME）决定。在这里为每种类型选择默认程序。")));

        scanApps();

        /* Categories deliberately cover every type a double-click actually
           produces in the shell (Office documents, mail links, discs and
           archives pulled off a USB stick, camera photos...), not just the
           four classic ones -- otherwise "open with" silently falls back to
           a random handler for half the files on the desktop. */
        struct Cat { const char *label; QStringList mimes; };
        const Cat cats[] = {
            { "网页浏览器", { QStringLiteral("x-scheme-handler/http"),
                             QStringLiteral("x-scheme-handler/https"),
                             QStringLiteral("x-scheme-handler/ftp"),
                             QStringLiteral("text/html"),
                             QStringLiteral("application/xhtml+xml") } },
            { "电子邮件", { QStringLiteral("x-scheme-handler/mailto"),
                           QStringLiteral("x-scheme-handler/smtp") } },
            { "Word 文档", { QStringLiteral("application/msword"),
                            QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document"),
                            QStringLiteral("application/vnd.oasis.opendocument.text"),
                            QStringLiteral("application/rtf") } },
            { "Excel 表格", { QStringLiteral("application/vnd.ms-excel"),
                             QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"),
                             QStringLiteral("application/vnd.oasis.opendocument.spreadsheet"),
                             QStringLiteral("text/csv") } },
            { "PowerPoint 演示", {
                             QStringLiteral("application/vnd.ms-powerpoint"),
                             QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.presentation"),
                             QStringLiteral("application/vnd.oasis.opendocument.presentation") } },
            { "图像查看器", { QStringLiteral("image/png"),
                             QStringLiteral("image/jpeg"),
                             QStringLiteral("image/gif"),
                             QStringLiteral("image/webp"),
                             QStringLiteral("image/bmp"),
                             QStringLiteral("image/svg+xml"),
                             QStringLiteral("image/tiff"),
                             QStringLiteral("image/x-icon") } },
            { "视频播放器", { QStringLiteral("video/mp4"),
                             QStringLiteral("video/x-matroska"),
                             QStringLiteral("video/webm"),
                             QStringLiteral("video/x-msvideo"),
                             QStringLiteral("video/quicktime"),
                             QStringLiteral("video/mpeg"),
                             QStringLiteral("video/x-flv"),
                             QStringLiteral("video/ogg") } },
            { "音频播放器", { QStringLiteral("audio/mpeg"),
                             QStringLiteral("audio/flac"),
                             QStringLiteral("audio/ogg"),
                             QStringLiteral("audio/x-wav"),
                             QStringLiteral("audio/wav"),
                             QStringLiteral("audio/aac"),
                             QStringLiteral("audio/mp4"),
                             QStringLiteral("audio/x-m4a"),
                             QStringLiteral("audio/webm"),
                             QStringLiteral("audio/x-opus+ogg") } },
            { "压缩文件", { QStringLiteral("application/zip"),
                           QStringLiteral("application/x-7z-compressed"),
                           QStringLiteral("application/x-rar"),
                           QStringLiteral("application/x-rar-compressed"),
                           QStringLiteral("application/vnd.rar"),
                           QStringLiteral("application/x-tar"),
                           QStringLiteral("application/gzip"),
                           QStringLiteral("application/x-gzip"),
                           QStringLiteral("application/x-bzip2"),
                           QStringLiteral("application/x-xz"),
                           QStringLiteral("application/x-zip-compressed") } },
            { "文本编辑器", { QStringLiteral("text/plain"),
                             QStringLiteral("text/markdown"),
                             QStringLiteral("text/x-python"),
                             QStringLiteral("text/x-c"),
                             QStringLiteral("text/x-shellscript"),
                             QStringLiteral("application/json"),
                             QStringLiteral("application/xml"),
                             QStringLiteral("text/x-ini"),
                             QStringLiteral("application/x-yaml") } },
            { "PDF 阅读器", { QStringLiteral("application/pdf") } },
            { "磁盘映像", { QStringLiteral("application/x-iso9660-image"),
                           QStringLiteral("application/x-cd-image"),
                           QStringLiteral("application/x-raw-disk-image"),
                           QStringLiteral("application/vnd.ms-cab-compressed") } },
        };

        for (const Cat &c : cats)
            v->addWidget(buildCatCard(QString::fromUtf8(c.label), c.mimes));

        v->addStretch(1);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(xScroll(outer));
    }

private:
    void scanApps()
    {
        const QStringList dirs = { QStringLiteral("/usr/share/applications"),
                                   QStringLiteral("/usr/local/share/applications"),
                                   QDir::homePath() + QStringLiteral("/.local/share/applications") };
        for (const QString &dir : dirs) {
            const QFileInfoList files = QDir(dir).entryInfoList({ QStringLiteral("*.desktop") }, QDir::Files);
            for (const QFileInfo &fi : files) {
                QFile f(fi.absoluteFilePath());
                if (!f.open(QIODevice::ReadOnly))
                    continue;
                QString name, mimes;
                bool hidden = false;
                while (!f.atEnd()) {
                    const QString line = QString::fromUtf8(f.readLine()).trimmed();
                    if (line.startsWith(QLatin1String("Name=")) && name.isEmpty())
                        name = line.mid(5);
                    else if (line.startsWith(QLatin1String("MimeType=")))
                        mimes = line.mid(9);
                    else if (line.startsWith(QLatin1String("NoDisplay=true")) ||
                             line.startsWith(QLatin1String("Hidden=true")))
                        hidden = true;
                }
                if (name.isEmpty() || mimes.isEmpty() || hidden)
                    continue;
                AppEntry e{ fi.fileName(), name };
                for (const QString &m : mimes.split(';', Qt::SkipEmptyParts))
                    m_byMime[m.trimmed()].append(e);
            }
        }
    }

    QWidget *buildCatCard(const QString &label, const QStringList &mimes)
    {
        auto *body = new QWidget();
        auto *h = new QHBoxLayout(body);
        h->setContentsMargins(0, 0, 0, 0);
        auto *combo = new QComboBox(body);
        combo->setProperty("mimes", mimes);

        /* collect candidate apps across the category's mimes */
        QMap<QString, QString> seen;   /* desktop -> display name */
        for (const QString &m : mimes)
            for (const AppEntry &e : m_byMime.value(m))
                if (!seen.contains(e.desktop))
                    seen.insert(e.desktop, e.name);
        /* current default of the first mime goes first */
        const QString curDef = runOut(QStringLiteral("xdg-mime"),
                                      { QStringLiteral("query"), QStringLiteral("default"),
                                        mimes.first() }).trimmed();
        for (auto it = seen.constBegin(); it != seen.constEnd(); ++it)
            combo->addItem(QStringLiteral("%1 (%2)").arg(it.value(), it.key()), it.key());
        if (!curDef.isEmpty()) {
            const int idx = combo->findData(curDef);
            if (idx >= 0)
                combo->setCurrentIndex(idx);
            else
                combo->insertItem(0, QStringLiteral("%1（当前）").arg(curDef), curDef);
        }

        auto *btn = new QPushButton(QStringLiteral("设为默认"), body);
        btn->setProperty("accent", true);
        h->addWidget(combo, 1);
        h->addWidget(btn);

        QObject::connect(btn, &QPushButton::clicked, this, [this, combo, mimes, label] {
            const QString desktop = combo->currentData().toString();
            if (desktop.isEmpty())
                return;
            int ok = 0;
            for (const QString &m : mimes) {
                QProcess p;
                p.start(QStringLiteral("xdg-mime"),
                        { QStringLiteral("default"), desktop, m });
                if (p.waitForStarted(2000) && p.waitForFinished(4000) && p.exitCode() == 0)
                    ok++;
            }
            QMessageBox::information(this, QStringLiteral("默认应用"),
                                     ok == mimes.size()
                                         ? QStringLiteral("「%1」已设为 %2 的默认程序。").arg(combo->currentText(), label)
                                         : QStringLiteral("部分类型设置失败（%1/%2）。").arg(ok).arg(mimes.size()));
        });

        return xCard(label, body);
    }

    QHash<QString, QList<AppEntry>> m_byMime;
};

/* ---------- 鼠标 ---------- */

class MousePage : public QWidget
{
public:
    MousePage()
    {
        auto *outer = new QWidget();
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 24, 32, 24);
        v->setSpacing(12);
        v->addWidget(xHeading(QStringLiteral("鼠标")));

        auto *body = new QWidget();
        auto *h = new QHBoxLayout(body);
        h->setContentsMargins(0, 0, 0, 0);
        m_speed = new QSlider(Qt::Horizontal, body);
        m_speed->setRange(1, 20);
        m_speed->setValue(10);
        mLbl = new QLabel(QStringLiteral("10"), body);
        mLbl->setFixedWidth(30);
        h->addWidget(new QLabel(QStringLiteral("指针速度"), body));
        h->addWidget(m_speed, 1);
        h->addWidget(mLbl);
        v->addWidget(xCard(QStringLiteral("指针"), body,
                           QStringLiteral("通过 xset 调整加速度（立即生效）")));
        QObject::connect(m_speed, &QSlider::valueChanged, this, [this](int val) {
            mLbl->setText(QString::number(val));
            /* Win11 1..20 -> xset acceleration numerator/denominator */
            const double acc = val <= 10 ? val / 10.0 : 1.0 + (val - 10) / 5.0;
            QProcess::startDetached(QStringLiteral("xset"),
                                    { QStringLiteral("m"),
                                      QString::number(acc, 'f', 2), QStringLiteral("4") });
        });

        auto *note = new QLabel(QStringLiteral(
            "提示：游戏或精确绘图场景可把速度调低；高分辨率屏幕建议 12 以上。"), outer);
        note->setProperty("subtle", true);
        note->setWordWrap(true);
        v->addWidget(note);
        v->addStretch(1);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(xScroll(outer));
    }

private:
    QSlider *m_speed;
    QLabel *mLbl;
};

/* ---------- 电源 ---------- */

class PowerPage : public QWidget
{
public:
    PowerPage()
    {
        auto *outer = new QWidget();
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 24, 32, 24);
        v->setSpacing(12);
        v->addWidget(xHeading(QStringLiteral("电源")));

        /* battery info if present */
        QString bat;
        const QDir ps(QStringLiteral("/sys/class/power_supply"));
        for (const QString &d : ps.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString type = readFile(ps.absoluteFilePath(d + QStringLiteral("/type")));
            if (type.trimmed() == QStringLiteral("Battery")) {
                const QString cap = readFile(ps.absoluteFilePath(d + QStringLiteral("/capacity")));
                const QString st = readFile(ps.absoluteFilePath(d + QStringLiteral("/status")));
                bat = QStringLiteral("电池 %1%（%2）")
                          .arg(cap.trimmed(),
                               st.trimmed() == QStringLiteral("Charging") ? QStringLiteral("充电中")
                                                                          : st.trimmed());
                break;
            }
        }
        if (!bat.isEmpty())
            v->addWidget(xCard(QStringLiteral("电池"), new QLabel(bat)));

        /* actions */
        auto *body = new QWidget();
        auto *h = new QHBoxLayout(body);
        h->setContentsMargins(0, 0, 0, 0);
        auto *sus = new QPushButton(QStringLiteral("睡眠"), body);
        auto *reb = new QPushButton(QStringLiteral("重启"), body);
        auto *off = new QPushButton(QStringLiteral("关机"), body);
        off->setProperty("accent", true);
        h->addWidget(sus);
        h->addWidget(reb);
        h->addWidget(off);
        h->addStretch(1);
        v->addWidget(xCard(QStringLiteral("电源操作"), body));

        QObject::connect(sus, &QPushButton::clicked, [] {
            QProcess::startDetached(QStringLiteral("systemctl"), { QStringLiteral("suspend") });
        });
        QObject::connect(reb, &QPushButton::clicked, [this] {
            if (confirm(QStringLiteral("确定要重启吗？")))
                QProcess::startDetached(QStringLiteral("systemctl"), { QStringLiteral("reboot") });
        });
        QObject::connect(off, &QPushButton::clicked, [this] {
            if (confirm(QStringLiteral("确定要关机吗？")))
                QProcess::startDetached(QStringLiteral("systemctl"), { QStringLiteral("poweroff") });
        });

        v->addWidget(xCard(QStringLiteral("合盖与电源键行为"), new QLabel(QStringLiteral(
            "由 systemd-logind 管理，编辑 /etc/systemd/lid.conf 与 logind.conf：\n"
            "HandleLidSwitch=suspend / HandlePowerKey=poweroff\n"
            "修改后执行 systemctl restart systemd-logind"))));

        v->addStretch(1);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(xScroll(outer));
    }

private:
    static QString readFile(const QString &p)
    {
        QFile f(p);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }
    bool confirm(const QString &q)
    {
        return QMessageBox::question(this, QStringLiteral("电源"), q,
                                     QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes;
    }
};

/* ---------- 用户 ---------- */

class UsersPage : public QWidget
{
public:
    UsersPage()
    {
        auto *outer = new QWidget();
        auto *v = new QVBoxLayout(outer);
        v->setContentsMargins(32, 24, 32, 24);
        v->setSpacing(12);
        v->addWidget(xHeading(QStringLiteral("用户")));

        struct passwd *pw = getpwuid(getuid());
        auto *info = new QWidget();
        auto *g = new QGridLayout(info);
        g->setContentsMargins(0, 0, 0, 0);
        g->setColumnStretch(1, 1);
        const QStringList ks = { QStringLiteral("用户名"), QStringLiteral("用户 ID"),
                                 QStringLiteral("主目录"), QStringLiteral("登录 Shell") };
        const QStringList vs = {
            pw ? QString::fromUtf8(pw->pw_name) : QStringLiteral("?"),
            QString::number(getuid()),
            pw ? QString::fromUtf8(pw->pw_dir) : QDir::homePath(),
            pw ? QString::fromUtf8(pw->pw_shell) : QString()
        };
        for (int i = 0; i < ks.size(); ++i) {
            auto *k = new QLabel(ks.at(i) + QStringLiteral("："), info);
            k->setProperty("subtle", true);
            auto *val = new QLabel(vs.at(i), info);
            val->setTextInteractionFlags(Qt::TextSelectableByMouse);
            g->addWidget(k, i, 0, Qt::AlignTop);
            g->addWidget(val, i, 1, Qt::AlignTop);
        }
        v->addWidget(xCard(QStringLiteral("当前用户"), info));

        auto *pwBtn = new QPushButton(QStringLiteral("更改密码…"));
        QObject::connect(pwBtn, &QPushButton::clicked, [] {
            QProcess::startDetached(QStringLiteral("xterm"),
                                    { QStringLiteral("-title"), QStringLiteral("更改密码"),
                                      QStringLiteral("-e"), QStringLiteral("passwd") });
        });
        v->addWidget(pwBtn, 0, Qt::AlignLeft);

        /* autologin status */
        QFile al(QStringLiteral("/etc/systemd/system/getty@tty1.service.d/autologin.conf"));
        QString alTxt = al.open(QIODevice::ReadOnly) ? QString::fromUtf8(al.readAll()) : QString();
        const bool autoLogin = alTxt.contains(QStringLiteral("--autologin"));
        v->addWidget(xCard(QStringLiteral("自动登录"), new QLabel(
            autoLogin ? QStringLiteral("已启用：开机自动登录并进入 ElevenDE。")
                      : QStringLiteral("未启用。运行 sudo ./install.sh 可配置 tty1 自动登录。"))));

        v->addStretch(1);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(xScroll(outer));
    }
};

} // namespace

/* ---------- exported builders ---------- */

QWidget *buildSoundPage() { return new SoundPage(); }
QWidget *buildDateTimePage() { return new DateTimePage(); }
QWidget *buildDefaultAppsPage() { return new DefaultAppsPage(); }
QWidget *buildMousePage() { return new MousePage(); }
QWidget *buildPowerPage() { return new PowerPage(); }
QWidget *buildUsersPage() { return new UsersPage(); }

} // namespace ElevenSettings
