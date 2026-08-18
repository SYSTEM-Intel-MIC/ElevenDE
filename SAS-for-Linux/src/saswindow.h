#pragma once

#include <QWidget>

#include <QPixmap>

class CommandMap;
class QMenu;
class QToolButton;

// Windows 11 风格"安全选项屏幕"(SAS)：
//  - X11: 自带毛玻璃（截屏模糊）；Wayland: 纯色暗背景（可配合 KWin 窗口规则启用真实模糊）
//  - Esc / 取消 关闭
class SasWindow : public QWidget {
    Q_OBJECT
public:
    explicit SasWindow(CommandMap *actions, QWidget *parent = nullptr);

public slots:
    void showSas();
    void hideSas();
    void showNetworkPanel();

protected:
    void paintEvent(QPaintEvent *ev) override;
    void keyPressEvent(QKeyEvent *ev) override;

private:
    void rebuildUi();
    void refreshBackground();
    QToolButton *makeFooterButton(const QPixmap &icon, const QString &text,
                                  const QString &tooltip, const QString &actionKey);

    CommandMap *m_actions = nullptr;
    QPixmap m_bg;          // 模糊后的屏幕截图（Wayland 下为空 → 纯色背景）
    QMenu *m_powerMenu = nullptr;
};