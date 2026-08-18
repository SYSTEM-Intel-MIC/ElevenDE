#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <cstdio>
#include <ctime>

#include "mainwindow.h"
#include "style.h"

#include "theme.h"

static QString pickIconTheme()
{
    const QStringList names   = QStringLiteral("Papirus Adwaita gnome Numix Faenza hicolor")
                                    .split(QLatin1Char(' '));
    const QStringList rootdirs = { QStringLiteral("/usr/share/icons"),
                                   QDir::homePath() + QStringLiteral("/.icons"),
                                   QDir::homePath() + QStringLiteral("/.local/share/icons") };
    for (const QString &name : names) {
        for (const QString &root : rootdirs) {
            const QString dir = root + QLatin1Char('/') + name;
            if (QDir(dir + QStringLiteral("/index.theme")).exists())
                return name;
        }
    }
    return {};
}

#include <csetjmp>
#include <csignal>

/* Crash diagnostics: the user reported occasional explorer.exe crashes.
 * Log Qt fatal/qWarning messages plus POSIX death signals to
 * /tmp/explorer-crash.log so the next crash is diagnosable. */
static FILE *g_crashlog = nullptr;

static void qt_msg_logger(QtMsgType type, const QMessageLogContext &ctx,
                          const QString &msg)
{
    Q_UNUSED(ctx);
    if (!g_crashlog) return;
    const char *lvl = "dbg";
    switch (type) {
    case QtDebugMsg: lvl = "dbg"; break;
    case QtInfoMsg: lvl = "inf"; break;
    case QtWarningMsg: lvl = "WRN"; break;
    case QtCriticalMsg: lvl = "CRT"; break;
    case QtFatalMsg: lvl = "FATAL"; break;
    }
    fprintf(g_crashlog, "[%s] %s\n", lvl, msg.toUtf8().constData());
    fflush(g_crashlog);
}

static void crash_signal_handler(int sig)
{
    if (g_crashlog) {
        fprintf(g_crashlog, "explorer.exe killed by signal %d\n", sig);
        fflush(g_crashlog);
    }
    _exit(128 + sig);
}

int main(int argc, char **argv)
{
    g_crashlog = fopen("/tmp/explorer-crash.log", "a");
    if (g_crashlog) {
        time_t now = time(nullptr);
        fprintf(g_crashlog, "=== explorer.exe session start %s", ctime(&now));
        fflush(g_crashlog);
    }
    qInstallMessageHandler(qt_msg_logger);
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGBUS, crash_signal_handler);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("explorer.exe"));
    app.setApplicationDisplayName(QStringLiteral("Lindows 资源管理器"));

    const QString theme = pickIconTheme();
    if (!theme.isEmpty())
        QIcon::setThemeName(theme);
    app.setWindowIcon(QIcon(QStringLiteral("/usr/local/share/elevende-shell/icons/scalable/apps/system-file-manager.svg")));

    ColorScheme scheme = detectColorScheme(&app);
    app.setStyleSheet(win11StyleSheet(scheme == ColorScheme::Dark));

    MainWindow window;

    /* explorer.exe <path>: open that directory directly (the shell passes
       the desktop-icon path here); no argument keeps the 此电脑 default */
    if (argc > 1) {
        const QString arg = QString::fromLocal8Bit(argv[1]);
        if (arg.startsWith(QLatin1Char('/')))
            window.openIn(arg);
    }

    window.show();

    return app.exec();
}