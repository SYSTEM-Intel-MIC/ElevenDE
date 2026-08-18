#include "networkdialog.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString signalBars(int signal)
{
    int n = 1;
    if (signal >= 25) n = 2;
    if (signal >= 50) n = 3;
    if (signal >= 75) n = 4;
    return QStringLiteral("█").repeated(n) + QStringLiteral("░").repeated(4 - n);
}

QString runSync(const QString &program, const QStringList &args,
                QString *stdErr = nullptr, int timeoutMs = 12000)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(500);
    }
    if (stdErr)
        *stdErr = QString::fromUtf8(p.readAllStandardError()).trimmed();
    return QString::fromUtf8(p.readAllStandardOutput());
}

} // namespace

NetworkDialog::NetworkDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("网络"));
    setMinimumWidth(460);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_list = new QListWidget(this);

    m_connectBtn = new QPushButton(QStringLiteral("连接"), this);
    m_disconnectBtn = new QPushButton(QStringLiteral("断开"), this);
    m_refreshBtn = new QPushButton(QStringLiteral("刷新"), this);
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(m_connectBtn);
    btnRow->addWidget(m_disconnectBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(m_refreshBtn);
    btnRow->addWidget(closeBtn);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(m_status);
    lay->addWidget(m_list, 1);
    lay->addLayout(btnRow);

    connect(m_connectBtn, &QPushButton::clicked, this, &NetworkDialog::connectCurrent);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &NetworkDialog::disconnectCurrent);
    connect(m_refreshBtn, &QPushButton::clicked, this, &NetworkDialog::refresh);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { connectCurrent(); });

    m_backend = detectBackend();
    refresh();
}

NetworkDialog::Backend NetworkDialog::detectBackend()
{
    QDBusInterface nm(QStringLiteral("org.freedesktop.NetworkManager"),
                      QStringLiteral("/org/freedesktop/NetworkManager"),
                      QStringLiteral("org.freedesktop.NetworkManager"),
                      QDBusConnection::systemBus());
    if (nm.isValid())
        return BackendNM;
    QDBusInterface iwd(QStringLiteral("net.connman.iwd"), QStringLiteral("/"),
                       QStringLiteral("net.connman.iwd.Manager"),
                       QDBusConnection::systemBus());
    if (iwd.isValid())
        return BackendIwd;
    return BackendNone;
}

void NetworkDialog::refresh()
{
    m_refreshBtn->setEnabled(false);
    switch (m_backend) {
    case BackendNM: loadNM(); break;
    case BackendIwd: loadIwd(); break;
    default:
        m_status->setText(QStringLiteral("未检测到网络后端（NetworkManager / iwd）"));
        break;
    }
    m_refreshBtn->setEnabled(true);
}

// ---------- NetworkManager: 通过 nmcli 子进程 ----------

void NetworkDialog::loadNM()
{
    const QString out = runSync(QStringLiteral("nmcli"),
        {QStringLiteral("-e"), QStringLiteral("no"), QStringLiteral("-t"),
         QStringLiteral("-f"), QStringLiteral("SSID,SIGNAL,SECURITY,IN-USE"),
         QStringLiteral("dev"), QStringLiteral("wifi"), QStringLiteral("list")});
    const QString devOut = runSync(QStringLiteral("nmcli"),
        {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("DEVICE,STATE,TYPE"),
         QStringLiteral("device"), QStringLiteral("status")});

    QString wifiDevice, currentSsid;
    for (const QString &line : devOut.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char(':'));
        if (f.size() >= 3 && f.at(2) == QLatin1String("wifi")
            && f.at(1).startsWith(QLatin1String("connected")))
            wifiDevice = f.at(0);
    }

    QVector<NetEntry> nets;
    for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char(':'));
        const QString ssid = f.value(0).trimmed();
        if (ssid.isEmpty())
            continue;   // 隐藏网络
        NetEntry e;
        e.ssid = ssid;
        e.signal = f.value(1).toInt();
        e.secured = !f.value(2).trimmed().isEmpty();
        e.inUse = f.value(3) == QLatin1String("*");
        if (e.inUse)
            currentSsid = ssid;
        nets << e;
    }
    std::sort(nets.begin(), nets.end(), [](const NetEntry &a, const NetEntry &b) {
        if (a.inUse != b.inUse)
            return a.inUse;
        return a.signal > b.signal;
    });

    QString info = QStringLiteral("后端: NetworkManager");
    if (!currentSsid.isEmpty())
        info += QStringLiteral("    已连接: %1").arg(currentSsid);
    else
        info += QStringLiteral("    未连接");
    if (!wifiDevice.isEmpty())
        info += QStringLiteral("（%1）").arg(wifiDevice);
    showNetworks(nets, info);
}

void NetworkDialog::connectCurrent()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const int idx = item->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_networks.size())
        return;
    connectEntry(m_networks.at(idx));
}

void NetworkDialog::connectEntry(const NetEntry &e)
{
    if (m_backend == BackendNM) {
        QStringList args = {QStringLiteral("-e"), QStringLiteral("no"), QStringLiteral("-t"),
                            QStringLiteral("dev"), QStringLiteral("wifi"),
                            QStringLiteral("connect"), e.ssid};
        QStringList nmArgs = args;
        if (e.secured) {
            bool ok = false;
            const QString pw = QInputDialog::getText(
                this, QStringLiteral("连接 %1").arg(e.ssid), QStringLiteral("Wi-Fi 密码:"),
                QLineEdit::Password, QString(), &ok);
            if (!ok)
                return;
            if (!pw.isEmpty())
                nmArgs << QStringLiteral("password") << pw;
        }
        QString err;
        runSync(QStringLiteral("nmcli"), nmArgs, &err);
        if (!err.isEmpty() && err.contains(QLatin1String("Secrets were required"))) {
            bool ok = false;
            const QString pw = QInputDialog::getText(
                this, QStringLiteral("连接 %1").arg(e.ssid), QStringLiteral("Wi-Fi 密码:"),
                QLineEdit::Password, QString(), &ok);
            if (!ok)
                return;
            runSync(QStringLiteral("nmcli"), args << QStringLiteral("password") << pw, &err);
            err.clear();
            if (pw.isEmpty())
                err = QStringLiteral("需要密码");
        }
        if (!err.isEmpty())
            QMessageBox::warning(this, QStringLiteral("连接失败"), err);
    } else if (m_backend == BackendIwd) {
        QString psk;
        if (e.secured) {
            bool ok = false;
            psk = QInputDialog::getText(this, QStringLiteral("连接 %1").arg(e.ssid),
                                        QStringLiteral("Wi-Fi 密码:"),
                                        QLineEdit::Password, QString(), &ok);
            if (!ok)
                return;
        }
        QDBusInterface net(QStringLiteral("net.connman.iwd"), e.netPath,
                           QStringLiteral("net.connman.iwd.Network"),
                           QDBusConnection::systemBus());
        const QDBusReply<void> reply = net.call(QStringLiteral("Connect"), psk);
        if (!reply.isValid())
            QMessageBox::warning(this, QStringLiteral("连接失败"), reply.error().message());
    }
    refresh();
}

void NetworkDialog::disconnectCurrent()
{
    if (m_backend == BackendNM) {
        const QString devOut = runSync(QStringLiteral("nmcli"),
            {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("DEVICE,STATE,TYPE"),
             QStringLiteral("device"), QStringLiteral("status")});
        QString wifiDevice;
        for (const QString &line : devOut.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QStringList f = line.split(QLatin1Char(':'));
            if (f.size() >= 3 && f.at(2) == QLatin1String("wifi")
                && f.at(1).startsWith(QLatin1String("connected")))
                wifiDevice = f.at(0);
        }
        if (wifiDevice.isEmpty())
            return;
        runSync(QStringLiteral("nmcli"),
                {QStringLiteral("dev"), QStringLiteral("disconnect"), wifiDevice});
    } else if (m_backend == BackendIwd) {
        for (const NetEntry &e : m_networks) {
            if (e.inUse) {
                QDBusInterface net(QStringLiteral("net.connman.iwd"), e.netPath,
                                   QStringLiteral("net.connman.iwd.Network"),
                                   QDBusConnection::systemBus());
                net.call(QStringLiteral("Disconnect"));
                break;
            }
        }
    }
    refresh();
}

// ---------- iwd: 直接走 D-Bus ----------

void NetworkDialog::loadIwd()
{
    QDBusInterface manager(QStringLiteral("net.connman.iwd"), QStringLiteral("/"),
                           QStringLiteral("net.connman.iwd.Manager"),
                           QDBusConnection::systemBus());
    const QDBusReply<QDBusArgument> devReply = manager.call(QStringLiteral("GetDevices"));
    if (!devReply.isValid()) {
        m_status->setText(QStringLiteral("iwd 读取设备失败: %1")
                              .arg(devReply.error().message()));
        return;
    }

    QVector<NetEntry> nets;
    QString current;
    QDBusArgument devArg = devReply.value();
    devArg.beginMap();
    while (!devArg.atEnd()) {
        QDBusObjectPath devPath;
        QVariantMap devProps;
        devArg.beginMapEntry();
        devArg >> devPath;
        devArg >> devProps;
        devArg.endMapEntry();

        if (devProps.value(QStringLiteral("Mode")).toString() != QLatin1String("station"))
            continue;

        QDBusInterface station(QStringLiteral("net.connman.iwd"), devPath.path(),
                               QStringLiteral("net.connman.iwd.Station"),
                               QDBusConnection::systemBus());
        const QDBusReply<QDBusArgument> netReply = station.call(QStringLiteral("GetNetworks"));
        if (!netReply.isValid())
            continue;

        QDBusArgument netArg = netReply.value();
        netArg.beginMap();
        while (!netArg.atEnd()) {
            QDBusObjectPath netPath;
            QVariantMap props;
            netArg.beginMapEntry();
            netArg >> netPath;
            netArg >> props;
            netArg.endMapEntry();

            NetEntry e;
            e.ssid = props.value(QStringLiteral("Name")).toString();
            e.signal = props.value(QStringLiteral("SignalStrength")).toInt();
            e.secured = props.value(QStringLiteral("Security")).toString()
                            != QLatin1String("open");
            e.inUse = props.value(QStringLiteral("Connected")).toBool();
            e.netPath = netPath.path();
            if (e.ssid.isEmpty())
                continue;
            if (e.inUse)
                current = e.ssid;
            nets << e;
        }
        netArg.endMap();
    }
    devArg.endMap();

    std::sort(nets.begin(), nets.end(), [](const NetEntry &a, const NetEntry &b) {
        if (a.inUse != b.inUse)
            return a.inUse;
        return a.signal > b.signal;
    });

    QString info = QStringLiteral("后端: iwd");
    info += current.isEmpty() ? QStringLiteral("    未连接")
                              : QStringLiteral("    已连接: %1").arg(current);
    showNetworks(nets, info);

    // 触发各 station 设备扫描，数秒后自动刷新列表
    QDBusArgument devArg2 = devReply.value();
    devArg2.beginMap();
    while (!devArg2.atEnd()) {
        QDBusObjectPath devPath;
        QVariantMap devProps;
        devArg2.beginMapEntry();
        devArg2 >> devPath;
        devArg2 >> devProps;
        devArg2.endMapEntry();
        if (devProps.value(QStringLiteral("Mode")).toString() == QLatin1String("station")) {
            QDBusInterface st(QStringLiteral("net.connman.iwd"), devPath.path(),
                              QStringLiteral("net.connman.iwd.Station"),
                              QDBusConnection::systemBus());
            st.call(QStringLiteral("Scan"));
        }
    }
    devArg2.endMap();
    QTimer::singleShot(2500, this, [this] { refresh(); });
}

void NetworkDialog::showNetworks(const QVector<NetEntry> &nets, const QString &currentInfo)
{
    m_networks = nets;
    m_list->clear();
    int idx = 0;
    for (const NetEntry &e : nets) {
        QString text = e.ssid;
        if (e.secured)
            text += QStringLiteral("  [加密]");
        if (e.inUse)
            text += QStringLiteral("  ★");
        text += QStringLiteral("    %1").arg(signalBars(e.signal));
        auto *item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, idx++);
    }
    m_status->setText(currentInfo);
    m_disconnectBtn->setEnabled(!nets.isEmpty());
}
