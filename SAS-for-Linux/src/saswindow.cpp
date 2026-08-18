#include "saswindow.h"

#include "commandmap.h"
#include "networkdialog.h"

#if defined(SAS_HAS_XCB)
#include "x11grabber.h"
#endif

#include <xcb/xcb.h>
#include <QGraphicsBlurEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QPixmap paintIcon(int size, const std::function<void(QPainter &, qreal, qreal)> &draw)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    draw(p, size / 2.0, size / 2.0);
    p.end();
    return pm;
}

QPixmap iconNetwork(int size)
{
    return paintIcon(size, [size](QPainter &p, qreal cx, qreal cy) {
        QPen pen(QColor(255, 255, 255, 235), qMax(1.6, size / 8.0),
                 Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (qreal r : {size * 0.16, size * 0.34, size * 0.5}) {
            p.drawArc(QRectF(cx - r, cy - r, 2 * r, 2 * r), 120 * 16, 300 * 16);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 235));
        p.drawEllipse(QPointF(cx, cy), size * 0.09, size * 0.09);
    });
}

QPixmap iconPerson(int size)
{
    return paintIcon(size, [size](QPainter &p, qreal cx, qreal cy) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 235));
        p.drawEllipse(QPointF(cx, cy - size * 0.18), size * 0.14, size * 0.14);
        QPainterPath shoulders;
        shoulders.moveTo(cx - size * 0.30, cy + size * 0.42);
        shoulders.arcTo(QRectF(cx - size * 0.30, cy - size * 0.08,
                               size * 0.60, size * 0.48), 180, 180);
        shoulders.closeSubpath();
        p.drawPath(shoulders);
    });
}

QPixmap iconPower(int size)
{
    return paintIcon(size, [size](QPainter &p, qreal cx, qreal cy) {
        QPen pen(QColor(255, 255, 255, 235), qMax(1.8, size / 9.0),
                 Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(cx - size * 0.32, cy - size * 0.32,
                         size * 0.64, size * 0.64), 135 * 16, 270 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 235));
        p.drawRect(QRectF(cx - size * 0.045, cy - size * 0.38,
                          size * 0.09, size * 0.34));
    });
}

QPixmap iconInput(int size)
{
    return paintIcon(size, [size](QPainter &p, qreal cx, qreal cy) {
        QPen pen(QColor(255, 255, 255, 235), qMax(1.4, size / 10.0),
                 Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        qreal boxSize = size * 0.6;
        QRectF box(cx - boxSize / 2, cy - boxSize / 2, boxSize, boxSize);
        p.drawRoundedRect(box, size * 0.08, size * 0.08);

        QFont f = p.font();
        f.setPixelSize(qMax(10, int(size * 0.38)));
        f.setBold(true);
        p.setFont(f);
        p.drawText(box, Qt::AlignCenter, QStringLiteral("拼"));
    });
}

void runSystem(const QString &cmd, const QStringList &args)
{
    if (QStandardPaths::findExecutable(cmd).isEmpty())
        return;
    QProcess::startDetached(cmd, args);
}

} // namespace

SasWindow::SasWindow(CommandMap *actions, QWidget *parent)
    : QWidget(parent), m_actions(actions)
{
    Qt::WindowFlags flags = Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint;
#if defined(SAS_HAS_XCB)
    if (X11Grabber(this).x11Available())
        flags |= Qt::X11BypassWindowManagerHint;
#endif
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setObjectName(QStringLiteral("sasWindow"));
    setWindowTitle(QStringLiteral("SAS Screen"));
    setStyleSheet(QStringLiteral(R"(
        #sasButton {
            color: #ffffff;
            background-color: transparent;
            border: none;
            border-radius: 4px;
            font-size: 14px;
            font-weight: 400;
        }
        #sasButton:hover    { background-color: rgba(255,255,255,0.10); }
        #sasButton:pressed  { background-color: rgba(255,255,255,0.18); }
        #sasCancel {
            color: #ffffff;
            background-color: rgba(255,255,255,0.08);
            border: 1px solid rgba(255,255,255,0.30);
            border-radius: 4px;
            font-size: 14px;
            font-weight: 400;
        }
        #sasCancel:hover    { background-color: rgba(255,255,255,0.15); }
        QToolButton {
            color: rgba(255,255,255,0.90);
            background: transparent;
            border: none;
            border-radius: 6px;
            font-size: 10px;
        }
        QToolButton:hover   { background: rgba(255,255,255,0.10); }
        QToolButton::menu-indicator { image: none; }
        QMenu {
            background-color: rgba(24,32,48,245);
            border: 1px solid rgba(255,255,255,0.15);
            border-radius: 8px;
            color: #ffffff;
            padding: 6px;
        }
        QMenu::item { padding: 8px 32px; border-radius: 6px; }
        QMenu::item:selected { background-color: rgba(255,255,255,0.12); }
    )"));
    rebuildUi();
}

void SasWindow::rebuildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    outer->addStretch(3);

    auto *centerLayout = new QVBoxLayout;
    centerLayout->setAlignment(Qt::AlignHCenter);
    centerLayout->setSpacing(8);

    for (const QString &label : m_actions->labels()) {
        auto *btn = new QPushButton(label, this);
        btn->setObjectName(QStringLiteral("sasButton"));
        btn->setFixedSize(280, 44);
        btn->setCursor(Qt::PointingHandCursor);
        const bool exitAfter = m_actions->exitAfter(label);
        connect(btn, &QPushButton::clicked, this, [this, label, exitAfter] {
            m_actions->run(label);
            if (exitAfter)
                hideSas();
        });
        centerLayout->addWidget(btn, 0, Qt::AlignHCenter);
    }

    centerLayout->addSpacing(20);

    auto *cancel = new QPushButton(QStringLiteral("取消"), this);
    cancel->setObjectName(QStringLiteral("sasCancel"));
    cancel->setFixedSize(160, 40);
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &SasWindow::hideSas);
    centerLayout->addWidget(cancel, 0, Qt::AlignHCenter);

    outer->addLayout(centerLayout);

    outer->addStretch(3);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 24, 20);
    footer->addStretch(1);

    footer->addWidget(makeFooterButton(iconInput(26), QStringLiteral("拼"),
                                       QStringLiteral("输入法"), QStringLiteral("input")));
    footer->addWidget(makeFooterButton(iconNetwork(26), QStringLiteral("网络"),
                                       QStringLiteral("网络设置"), QStringLiteral("network")));
    footer->addWidget(makeFooterButton(iconPerson(26), QStringLiteral("无障碍"),
                                       QStringLiteral("无障碍设置"), QStringLiteral("accessibility")));

    m_powerMenu = new QMenu(this);
    QAction *sleep = m_powerMenu->addAction(QStringLiteral("睡眠"));
    QAction *shutdown = m_powerMenu->addAction(QStringLiteral("关机"));
    QAction *reboot = m_powerMenu->addAction(QStringLiteral("重启"));
    connect(sleep, &QAction::triggered, this, [this] {
        hideSas();
        runSystem(QStringLiteral("systemctl"), {QStringLiteral("suspend")});
    });
    connect(shutdown, &QAction::triggered, this, [this] {
        hideSas();
        runSystem(QStringLiteral("systemctl"), {QStringLiteral("poweroff")});
    });
    connect(reboot, &QAction::triggered, this, [this] {
        hideSas();
        runSystem(QStringLiteral("systemctl"), {QStringLiteral("reboot")});
    });

    auto *powerBtn = new QToolButton(this);
    powerBtn->setIcon(QIcon(iconPower(22)));
    powerBtn->setIconSize(QSize(22, 22));
    powerBtn->setText(QStringLiteral("电源"));
    powerBtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    powerBtn->setFixedSize(64, 60);
    powerBtn->setToolTip(QStringLiteral("电源"));
    powerBtn->setMenu(m_powerMenu);
    powerBtn->setPopupMode(QToolButton::InstantPopup);
    footer->addWidget(powerBtn);

    outer->addLayout(footer);
}

QToolButton *SasWindow::makeFooterButton(const QPixmap &icon, const QString &text,
                                         const QString &tooltip, const QString &actionKey)
{
    auto *btn = new QToolButton(this);
    btn->setIcon(QIcon(icon));
    btn->setIconSize(QSize(22, 22));
    btn->setText(text);
    btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    btn->setFixedSize(64, 60);
    btn->setToolTip(tooltip);
    connect(btn, &QToolButton::clicked, this, [this, actionKey] {
        if (actionKey == QLatin1String("network")) {
            NetworkDialog dlg(this);
            dlg.exec();
            return;
        }
        m_actions->runFooter(actionKey);
        hideSas();
    });
    return btn;
}

void SasWindow::refreshBackground()
{
    m_bg = QPixmap();

    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QPixmap shot = screen->grabWindow(0);
    if (shot.isNull() || shot.width() <= 0)
        return;   // Wayland 下通常拿不到其它窗口内容 → 使用纯色暗背景

    // 缩小 4 倍模糊后再放大，兼顾效果与性能（等效全尺寸模糊半径 ≈ 40）
    const qreal scale = 0.25;
    const QSize smallSize(qMax(1, int(shot.width() * scale)),
                          qMax(1, int(shot.height() * scale)));
    const QPixmap small = shot.scaled(smallSize, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);

    QGraphicsScene scene;
    QGraphicsPixmapItem item(small);
    QGraphicsBlurEffect effect;
    effect.setBlurRadius(10.0);
    item.setGraphicsEffect(&effect);
    scene.addItem(&item);

    QPixmap out(smallSize);
    out.fill(Qt::transparent);
    QPainter p(&out);
    scene.render(&p);
    p.end();
    m_bg = out;
}

void SasWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (m_bg.isNull()) {
        p.fillRect(rect(), QColor(20, 52, 72, 255));
    } else {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(rect(), m_bg);
        /* heavier veil (Win11 Ctrl+Alt+Del is nearly opaque): keeps the
         * taskbar/content underneath from bleeding through the overlay */
        p.fillRect(rect(), QColor(16, 40, 60, 226));
    }
}

void SasWindow::showSas()
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen)
        setGeometry(screen->geometry());

    refreshBackground();
    showFullScreen();

    /* 让 picom/WM 把 SAS 渲染在 DOCK(任务栏) 之上：
     * 标记为 NOTIFICATION 类型 + ABOVE 状态（Win11 安全屏幕覆盖一切）。
     * 用独立 xcb 连接设置属性，不依赖 Qt 平台头 */
    {
        xcb_connection_t *conn = xcb_connect(nullptr, nullptr);
        if (conn && !xcb_connection_has_error(conn)) {
            auto intern = [conn](const char *name) -> xcb_atom_t {
                xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
                xcb_intern_atom_reply_t *rp = xcb_intern_atom_reply(conn, ck, nullptr);
                xcb_atom_t a = rp ? rp->atom : XCB_ATOM_NONE;
                free(rp);
                return a;
            };
            xcb_atom_t typeProp = intern("_NET_WM_WINDOW_TYPE");
            xcb_atom_t notif    = intern("_NET_WM_WINDOW_TYPE_NOTIFICATION");
            xcb_atom_t stateProp = intern("_NET_WM_STATE");
            xcb_atom_t above     = intern("_NET_WM_STATE_ABOVE");
            if (typeProp != XCB_ATOM_NONE && notif != XCB_ATOM_NONE)
                xcb_change_property(conn, XCB_PROP_MODE_REPLACE, (xcb_window_t)winId(),
                                    typeProp, XCB_ATOM_ATOM, 32, 1, &notif);
            if (stateProp != XCB_ATOM_NONE && above != XCB_ATOM_NONE)
                xcb_change_property(conn, XCB_PROP_MODE_REPLACE, (xcb_window_t)winId(),
                                    stateProp, XCB_ATOM_ATOM, 32, 1, &above);
            xcb_flush(conn);
            xcb_disconnect(conn);
        }
    }

    /* 覆盖任务栏靠窗口类型标记（NOTIFICATION + ABOVE，picom 据此把 SAS
     * 渲染在 DOCK 面板之上）。不再周期性 raise：定时重抬会和 picom 的
     * 重排互相打架，造成 SAS 与任务栏交替闪烁。 */
    raise();
    activateWindow();

#if defined(SAS_HAS_XCB)
    if (X11Grabber(this).x11Available()) {
        // Qt 自带键盘抓取（在 Qt 自己的 X 连接上执行，事件正常流入 Qt 事件循环）：
        // 全屏覆盖 + 键盘全局抢占；鼠标无需抓取（全屏窗口已盖住所有其它窗口，
        // 且抓鼠标会把所有指针事件导向顶层窗口，导致子按钮不可点击）。
        grabKeyboard();
    }
#endif
}

void SasWindow::hideSas()
{
#if defined(SAS_HAS_XCB)
    if (X11Grabber(this).x11Available())
        releaseKeyboard();
#endif
    hide();
}

void SasWindow::showNetworkPanel()
{
    /* 可从任务栏 IPC 单独调用。该对话框自身会自动识别 NetworkManager
       或 iwd，并处理网络扫描、密码输入、连接和断开。 */
    NetworkDialog dlg(nullptr);
    dlg.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    dlg.exec();
}

void SasWindow::keyPressEvent(QKeyEvent *ev)
{
    if (ev->key() == Qt::Key_Escape)
        hideSas();
    else
        QWidget::keyPressEvent(ev);
}