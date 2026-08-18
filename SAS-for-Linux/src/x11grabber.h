#pragma once

#include <QObject>

class QWidget;

// 独立的 X 连接（与 Qt 的事件连接分离，避免互相吞事件）：
//  - start(): 在 root 上注册被动抓取 Ctrl+Alt+Delete (XGrabKey)
//  - grabModal()/releaseModal(): 显示全屏界面时主动抢占键盘/鼠标焦点
class X11Grabber : public QObject {
    Q_OBJECT
public:
    explicit X11Grabber(QObject *parent = nullptr);
    ~X11Grabber() override;

    bool x11Available() const;   // 当前 Qt 是否运行在 X11 (xcb) 平台
    bool start();                // 注册全局 Ctrl+Alt+Delete，成功返回 true

signals:
    void activated();            // 用户按下了 Ctrl+Alt+Delete

private:
    void poll();                 // 轮询本连接上的 X 事件

    class Private;
    Private *d = nullptr;
};