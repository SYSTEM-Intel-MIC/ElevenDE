#pragma once

#include <QDialog>
#include <QStringList>
#include <QVector>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// 应用内 Wi-Fi 切换面板：
//   - 自动检测后端: NetworkManager(nmcli) 或 iwd(D-Bus)
//   - 列出附近网络(信号强度/加密)、连接(需密码时弹输入框)、断开、刷新
class NetworkDialog : public QDialog {
    Q_OBJECT
public:
    explicit NetworkDialog(QWidget *parent = nullptr);

private slots:
    void refresh();
    void connectCurrent();
    void disconnectCurrent();

private:
    enum Backend { BackendNone, BackendNM, BackendIwd };

    struct NetEntry {
        QString ssid;
        QString device;
        int signal = 0;
        bool secured = false;
        bool inUse = false;
        QString netPath;   // iwd: D-Bus 网络对象路径
    };

    Backend detectBackend();
    void loadNM();
    void loadIwd();
    void showNetworks(const QVector<NetEntry> &nets, const QString &currentInfo);
    void connectEntry(const NetEntry &e);

    QLabel *m_status = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QPushButton *m_disconnectBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QVector<NetEntry> m_networks;
    Backend m_backend = BackendNone;
};
