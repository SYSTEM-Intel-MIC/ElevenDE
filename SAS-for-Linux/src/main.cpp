#include "commandmap.h"
#include "saswindow.h"

#if defined(SAS_HAS_XCB)
#include "x11grabber.h"
#endif

#include <QApplication>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTextStream>
#include <QTimer>

#include <unistd.h>

namespace {

QString usage()
{
    return QStringLiteral(
        "用法: sas-screen [--show | --network] [--config <file>]\n"
        "  --show            启动后立即显示安全选项屏幕\n"
        "  --network         启动后立即显示 WLAN 连接面板\n"
        "  --config <file>   指定按钮 → 命令映射的 JSON 配置文件\n"
        "  (无参数)          常驻后台:\n"
        "                     - X11: 自动抓取全局 Ctrl+Alt+Delete\n"
        "                     - Wayland: 等待混成器快捷键(绑定 sas-screen --show)\n"
        "  Esc / 取消        关闭界面\n");
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("sas-screen"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setDesktopFileName(QStringLiteral("sas-screen.desktop"));
    app.setQuitOnLastWindowClosed(false);

    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
        QTextStream(stdout) << usage();
        return 0;
    }
    const bool showNow = args.contains(QStringLiteral("--show"))
                      || args.contains(QStringLiteral("-s"));
    const bool showNetwork = args.contains(QStringLiteral("--network"));
    QString configPath;
    const int cfgIdx = args.indexOf(QStringLiteral("--config"));
    if (cfgIdx >= 0 && cfgIdx + 1 < args.size())
        configPath = args.at(cfgIdx + 1);

    CommandMap actions = CommandMap::load(configPath);
    SasWindow window(&actions);

    // ---- 单实例 IPC：已有一个常驻实例时，把"显示"请求转发给它 ----
    const QString sockPath = QDir::temp().filePath(
        QStringLiteral("sas-screen-%1.sock").arg(static_cast<qint64>(getuid())));

    QLocalSocket probe;
    probe.connectToServer(sockPath);
    if (probe.waitForConnected(400)) {
        probe.write(showNetwork ? "network\n" : "show\n");
        probe.flush();
        probe.waitForBytesWritten(400);
        return 0;   // 常驻实例会弹出界面
    }

    QLocalServer ipc;
    if (!ipc.listen(sockPath)) {
        QLocalServer::removeServer(sockPath);
        if (!ipc.listen(sockPath)) {
            qWarning().noquote()
                << "无法监听 IPC:" << ipc.errorString() << "—— 可能已有实例在运行";
            return 1;
        }
    }
    QObject::connect(&ipc, &QLocalServer::newConnection, &app, [&app, &window, &ipc] {
        if (QLocalSocket *conn = ipc.nextPendingConnection()) {
            QObject::connect(conn, &QLocalSocket::readyRead, &app, [conn, &window] {
                const QByteArray request = conn->readAll().trimmed();
                if (request == "network")
                    window.showNetworkPanel();
                else
                    window.showSas();
            });
            QObject::connect(conn, &QLocalSocket::disconnected,
                             conn, &QLocalSocket::deleteLater);
        }
    });

    // ---- X11: 注册全局 Ctrl+Alt+Delete（Wayland 下自动跳过）----
#if defined(SAS_HAS_XCB)
    X11Grabber grabber;
    if (grabber.x11Available()) {
        if (grabber.start()) {
            QObject::connect(&grabber, &X11Grabber::activated,
                             &window, &SasWindow::showSas);
        } else {
            qWarning().noquote()
                << "无法注册 Ctrl+Alt+Delete 全局抓取。"
                << "可能已被桌面环境的快捷键占用(如 KDE 的注销)，请先在系统设置中移除该快捷键。";
        }
    }
#endif

    if (showNetwork)
        QTimer::singleShot(0, &window, &SasWindow::showNetworkPanel);
    else if (showNow)
        QTimer::singleShot(0, &window, &SasWindow::showSas);

    return QApplication::exec();
}