#include "style.h"

#include <QApplication>
#include <QStyle>

QString win11StyleSheet(bool dark)
{
    if (dark) {
        return QStringLiteral(R"(
QMainWindow { background: #1B1B1B; }
QWidget { color: #E8E8E8; font-family: "Segoe UI", "Noto Sans CJK SC", sans-serif; font-size: 13px; }
QMenu { background: #252526; color: #E8E8E8; border: 1px solid #454545; }
QMenu::item { background: transparent; padding: 5px 22px 5px 12px; color: #E8E8E8; }
QMenu::item:selected { background: #3F3F46; color: #FFFFFF; }
QMenu::item:disabled { color: #6A6A6A; }
QMenu::separator { height: 1px; background: #3A3A3A; margin: 4px 10px; }
QMenu::indicator { width: 14px; height: 14px; }
QMessageBox, QInputDialog { background: #252526; color: #E8E8E8; }
QMessageBox QLabel, QInputDialog QLabel { color: #E8E8E8; }
QMessageBox QPushButton, QInputDialog QPushButton { background: #3A3A3A; border: 1px solid #555555; }
#TitleBar { background: #1F1F1F; }
#TitleLabel { font-size: 12px; color: #D8D8D8; }
#CommandBar { background: #252526; border-bottom: 1px solid #333333; }
#CommandButton { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px 8px; color: #E8E8E8; }
#CommandButton:hover, #CommandButton:menu-open { background: #36363A; border-color: #4B4B50; }
#CommandButton:pressed { background: #47474E; }
#Sidebar { background: #202020; border-right: 1px solid #2E2E2E; }
#SidebarSection { font-size: 10px; color: #9E9E9E; font-weight: bold; padding: 3px 4px 1px 4px; }
#SidebarList { background: transparent; border: none; }
#SidebarList::item { padding: 3px 6px; border-radius: 4px; margin: 0 1px; }
#SidebarList::item:hover { background: #303030; }
#SidebarList::item:selected { background: #3F3F46; color: #FFFFFF; }
#StatusBar { background: #252526; border-top: 1px solid #333333; color: #B0B0B0; font-size: 12px; }
QLineEdit { background: #2B2B2B; border: 1px solid #454545; border-radius: 4px; color: #E8E8E8; padding: 1px 6px; selection-background-color: #4A6BB5; }
QLineEdit:focus { border: 1px solid #5488E0; background: #333333; }
QTreeView { background: #1B1B1B; border: none; color: #E8E8E8; }
QTreeView::item { padding: 1px 1px; }
QTreeView::item:hover { background: #2E2E2E; }
QTreeView::item:selected { background: #3A3A4E; color: #FFFFFF; }
QHeaderView::section { background: #1B1B1B; border: none; border-bottom: 1px solid #333333; border-right: 1px solid #2A2A2A; padding: 3px 6px; color: #C8C8C8; font-weight: normal; }
QPushButton { background: transparent; border: none; border-radius: 4px; padding: 4px 10px; color: #E8E8E8; }
QPushButton:hover { background: #3A3A3A; }
QPushButton:pressed { background: #4A4A4A; }
QPushButton:disabled { color: #666666; }
#TitleBtn { border-radius: 0; }
#TitleBtn:hover { background: #3A3A3A; }
#TitleBtnClose:hover { background: #C42B1C; color: white; }
#NavBack, #NavForward, #NavUp { font-size: 12px; }
#NavBack:hover, #NavForward:hover, #NavUp:hover { background: #333333; }
#Crumb { color: #B8B8B8; padding: 2px 6px; }
#Crumb:hover { background: #3A3A3A; color: #E8E8E8; }
#CrumbLast { color: #E8E8E8; font-weight: 600; padding: 2px 6px; }
#CrumbSep { color: #9E9E9E; }
#Search { background: #2B2B2B; }
#OverlayBox { background: rgba(60, 60, 60, 235); border: 1px solid #666666; border-radius: 8px; }
#OverlayBox QLabel { color: #F0F0F0; }
#OverlayBox QPushButton { background: #454545; border: 1px solid #666666; border-radius: 4px; padding: 5px 14px; }
#OverlayBox QPushButton:hover { background: #555555; }
QScrollBar:vertical { background: transparent; width: 10px; }
QScrollBar::handle:vertical { background: #4A4A4A; border-radius: 5px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; }
QScrollBar::handle:horizontal { background: #4A4A4A; border-radius: 5px; min-width: 30px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
)");
    }
    return QStringLiteral(R"(
QMainWindow { background: #FFFFFF; }
QWidget { color: #1F1F1F; font-family: "Segoe UI", "Noto Sans CJK SC", sans-serif; font-size: 13px; }
QMenu { background: #FFFFFF; color: #1F1F1F; border: 1px solid #D0D0D0; }
QMenu::item { background: transparent; padding: 5px 22px 5px 12px; color: #1F1F1F; }
QMenu::item:selected { background: #CDE3FA; color: #1F1F1F; }
QMenu::item:disabled { color: #A0A0A0; }
QMenu::separator { height: 1px; background: #E0E0E0; margin: 4px 10px; }
QMenu::indicator { width: 14px; height: 14px; }
QMessageBox, QInputDialog { background: #FFFFFF; color: #1F1F1F; }
QMessageBox QLabel, QInputDialog QLabel { color: #1F1F1F; }
QMessageBox QPushButton, QInputDialog QPushButton { background: #F3F3F3; border: 1px solid #D0D0D0; border-radius: 4px; padding: 4px 12px; }
QMessageBox QPushButton:hover, QInputDialog QPushButton:hover { background: #E5E5E5; }
#TitleBar { background: #F9F9F9; }
#TitleLabel { font-size: 12px; color: #3F3F3F; }
#CommandBar { background: #F3F3F3; border-bottom: 1px solid #E0E0E0; }
#CommandButton { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px 8px; color: #1F1F1F; }
#CommandButton:hover, #CommandButton:menu-open { background: #E7F1FB; border-color: #C7E0F4; }
#CommandButton:pressed { background: #CFE8FA; }
#Sidebar { background: #F9F9F9; border-right: 1px solid #E6E6E6; }
#SidebarSection { font-size: 10px; color: #7A7A7A; font-weight: bold; padding: 3px 4px 1px 4px; }
#SidebarList { background: transparent; border: none; }
#SidebarList::item { padding: 3px 6px; border-radius: 4px; margin: 0 1px; }
#SidebarList::item:hover { background: #E8F0FA; }
#SidebarList::item:selected { background: #CDE3FA; color: #1F1F1F; }
#StatusBar { background: #F3F3F3; border-top: 1px solid #E0E0E0; color: #616161; font-size: 12px; }
QLineEdit { background: #FFFFFF; border: 1px solid #C5C5C5; border-radius: 4px; color: #1F1F1F; padding: 1px 6px; selection-background-color: #B3D7F2; }
QLineEdit:focus { border: 1px solid #4C8BDC; background: #FFFFFF; }
QTreeView { background: #FFFFFF; border: none; color: #1F1F1F; }
QTreeView::item { padding: 1px 1px; }
QTreeView::item:hover { background: #F0F6FC; }
QTreeView::item:selected { background: #CBE4F6; color: #1F1F1F; }
QHeaderView::section { background: #FFFFFF; border: none; border-bottom: 1px solid #E0E0E0; border-right: 1px solid #ECECEC; padding: 3px 6px; color: #616161; font-weight: normal; }
QPushButton { background: transparent; border: none; border-radius: 4px; padding: 4px 10px; color: #1F1F1F; }
QPushButton:hover { background: #E8E8E8; }
QPushButton:pressed { background: #D8D8D8; }
QPushButton:disabled { color: #A0A0A0; }
#TitleBtn { border-radius: 0; }
#TitleBtn:hover { background: #E5E5E5; }
#TitleBtnClose:hover { background: #E81123; color: white; }
#NavBack, #NavForward, #NavUp { font-size: 12px; }
#NavBack:hover, #NavForward:hover, #NavUp:hover { background: #E8E8E8; }
#Crumb { color: #5A5A5A; padding: 2px 6px; }
#Crumb:hover { background: #E8E8E8; color: #1F1F1F; }
#CrumbLast { color: #1F1F1F; font-weight: 600; padding: 2px 6px; }
#CrumbSep { color: #B0B0B0; }
#Search { background: #FFFFFF; }
#OverlayBox { background: rgba(255, 255, 255, 242); border: 1px solid #C5C5C5; border-radius: 8px; }
#OverlayBox QLabel { color: #1F1F1F; }
#OverlayBox QPushButton { background: #FFFFFF; border: 1px solid #C5C5C5; border-radius: 4px; padding: 5px 14px; }
#OverlayBox QPushButton:hover { background: #F0F0F0; }
QScrollBar:vertical { background: transparent; width: 10px; }
QScrollBar::handle:vertical { background: #C8C8C8; border-radius: 5px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; }
QScrollBar::handle:horizontal { background: #C8C8C8; border-radius: 5px; min-width: 30px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
)");
}