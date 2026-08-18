#pragma once
/*
 * win11style.h - shared Windows 11 look & feel for ElevenDE Qt apps.
 *
 * Every Qt app in the suite calls Win11Style::apply(app) right after
 * constructing the QApplication. That installs a Fluent/Win11-inspired
 * palette + QSS so Notepad, Settings, Task Manager and Calculator all feel
 * like one family. Honors the system accent color stored by the Settings
 * app (~/.config/elevende/elevende.conf), falling back to Win11 default
 * blue #0078D4.
 */

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QFileInfo>
#include <QSettings>
#include <QString>
#include <QStyle>
#include <QStyleFactory>

namespace Win11Style {

/* Win11 default accent (the "Windows default blue"). */
inline QColor defaultAccent() { return QColor(QStringLiteral("#0078D4")); }

/* Read the accent the user picked in the Settings app, if any. */
inline QColor accentColor()
{
    QSettings s(QStringLiteral("elevende"), QStringLiteral("elevende"));
    const QString v = s.value(QStringLiteral("accent"), QString()).toString();
    if (!v.isEmpty() && QColor::isValidColor(v))
        return QColor(v);
    return defaultAccent();
}

inline bool darkMode()
{
    QSettings s(QStringLiteral("elevende"), QStringLiteral("elevende"));
    return s.value(QStringLiteral("darkMode"), true).toBool();
}

/* A complete Fluent-style stylesheet for the current accent/dark setting. */
inline QString stylesheet()
{
    const QColor accent = accentColor();
    const bool dark = darkMode();

    const QString bg      = dark ? QStringLiteral("#202020") : QStringLiteral("#F3F3F3");
    const QString card    = dark ? QStringLiteral("#2D2D2D") : QStringLiteral("#FAFAFA");
    const QString card2   = dark ? QStringLiteral("#333333") : QStringLiteral("#FFFFFF");
    const QString text    = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1B1B1B");
    const QString sub     = dark ? QStringLiteral("#9E9E9E") : QStringLiteral("#5C5C5C");
    const QString border  = dark ? QStringLiteral("#3D3D3D") : QStringLiteral("#E5E5E5");
    const QString hover   = dark ? QStringLiteral("#3A3A3A") : QStringLiteral("#E9E9E9");
    const QString pressed = dark ? QStringLiteral("#404040") : QStringLiteral("#DDDDDD");
    const QString field   = dark ? QStringLiteral("#1F1F1F") : QStringLiteral("#FFFFFF");
    const QString accentTxt = QStringLiteral("#FFFFFF");

    QString qss;
    qss += QStringLiteral(
        "* { font-family: 'Segoe UI', 'Noto Sans CJK SC', 'Microsoft YaHei', sans-serif; }\n"
        "QWidget { background-color: %1; color: %2; font-size: 13px; }\n"
        "QLabel { background: transparent; }\n"
        "QLabel[subtle=\"true\"] { color: %3; }\n"

        /* Cards */
        "QFrame[card=\"true\"], QWidget[card=\"true\"] {"
        " background-color: %4; border: 1px solid %5; border-radius: 8px; }\n"

        /* Buttons */
        "QPushButton { background-color: %6; color: %2; border: 1px solid %5;"
        " border-radius: 6px; padding: 6px 14px; min-height: 20px; }"
        "QPushButton:hover { background-color: %7; }"
        "QPushButton:pressed { background-color: %8; }"
        "QPushButton:disabled { color: %3; background-color: %4; }"
        "QPushButton[accent=\"true\"] { background-color: %9; color: %10; border: 1px solid %9; }"
        "QPushButton[accent=\"true\"]:hover { background-color: %11; }"
        "QPushButton[accent=\"true\"]:pressed { background-color: %12; }\n"

        /* Inputs */
        "QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox {"
        " background-color: %13; color: %2; border: 1px solid %5;"
        " border-bottom: 2px solid %5; border-radius: 4px; padding: 5px 8px; selection-background-color: %9; }"
        "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus {"
        " border-bottom: 2px solid %9; }\n"

        /* Lists / trees / tables */
        "QListView, QTreeView, QTableView, QListWidget, QTreeWidget, QTableWidget {"
        " background-color: %13; color: %2; border: 1px solid %5; border-radius: 6px;"
        " alternate-background-color: %4; }"
        "QListView::item, QTreeView::item, QListWidget::item, QTableView::item {"
        " padding: 5px; border-radius: 4px; }"
        "QListView::item:hover, QListWidget::item:hover, QTableView::item:hover { background: %7; }"
        "QListView::item:selected, QListWidget::item:selected,"
        " QTableView::item:selected, QTreeView::item:selected { background: %9; color: %10; }\n"

        /* Headers */
        "QHeaderView::section { background-color: %4; color: %3; border: none;"
        " border-bottom: 1px solid %5; padding: 6px; }"
        "QTableCornerButton::section { background: %4; border: none; }\n"

        /* ComboBox */
        "QComboBox { background-color: %6; border: 1px solid %5; border-radius: 6px;"
        " padding: 5px 10px; min-height: 20px; }"
        "QComboBox:hover { background-color: %7; }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background-color: %6; color: %2;"
        " border: 1px solid %5; selection-background-color: %9; }\n"

        /* Scrollbars (thin Fluent) */
        "QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }"
        "QScrollBar::handle:vertical { background: %5; border-radius: 5px; min-height: 30px; }"
        "QScrollBar::handle:vertical:hover { background: %3; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }"
        "QScrollBar::handle:horizontal { background: %5; border-radius: 5px; min-width: 30px; }"
        "QScrollBar::handle:horizontal:hover { background: %3; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }\n"

        /* Menus (Notepad) */
        "QMenuBar { background-color: %1; color: %2; border-bottom: 1px solid %5; }"
        "QMenuBar::item { background: transparent; padding: 5px 10px; border-radius: 4px; }"
        "QMenuBar::item:selected { background: %7; }"
        "QMenu { background-color: %6; color: %2; border: 1px solid %5; border-radius: 8px; padding: 4px; }"
        "QMenu::item { padding: 6px 24px; border-radius: 4px; }"
        "QMenu::item:selected { background-color: %7; }"
        "QMenu::separator { height: 1px; background: %5; margin: 4px 8px; }\n"

        /* Tabs */
        "QTabWidget::pane { border: 1px solid %5; border-radius: 6px; }"
        "QTabBar::tab { padding: 6px 14px; border: 1px solid %5; border-radius: 6px; margin-right: 2px; background: %4; }"
        "QTabBar::tab:selected { background: %9; color: %10; }\n"

        /* Group / check / radio */
        "QCheckBox, QRadioButton { spacing: 8px; }"
        "QCheckBox::indicator, QRadioButton::indicator { width: 18px; height: 18px; }"
        "QCheckBox::indicator { border: 1px solid %3; border-radius: 4px; background: %13; }"
        "QCheckBox::indicator:checked { background: %9; border-color: %9; }\n"

        /* Sliders */
        "QSlider::groove:horizontal { height: 4px; background: %5; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %9; width: 16px; height: 16px;"
        " margin: -6px 0; border-radius: 8px; }\n"

        /* Progress */
        "QProgressBar { background: %4; border: 1px solid %5; border-radius: 6px; text-align: center; }"
        "QProgressBar::chunk { background: %9; border-radius: 5px; }\n"

        /* Status bar */
        "QStatusBar { background: %1; color: %3; border-top: 1px solid %5; }\n"

        /* Tooltips */
        "QToolTip { background-color: %6; color: %2; border: 1px solid %5; padding: 4px 8px; border-radius: 4px; }\n"
    ).arg(bg, text, sub, card, border, card2, hover, pressed,
          accent.name(), accentTxt,
          accent.lighter(115).name(), accent.darker(120).name(), field);

    return qss;
}

/* Resolve a first-party app icon without depending on an external icon-cache
 * rebuild. The absolute SVG path also gives Qt a valid _NET_WM_ICON, which
 * keeps taskbar buttons from degrading to a black/blank placeholder. */
inline QIcon appIcon(const QString &name)
{
    const QStringList roots = {
        QStringLiteral("/usr/local/share/elevende-shell/icons/scalable/apps/"),
        QStringLiteral("/usr/local/share/elevende-shell/icons/64x64/apps/"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/elevende-shell/icons/scalable/apps/")
    };
    for (const QString &root : roots) {
        const QString svg = root + name + QStringLiteral(".svg");
        if (QFileInfo::exists(svg)) return QIcon(svg);
        const QString png = root + name + QStringLiteral(".png");
        if (QFileInfo::exists(png)) return QIcon(png);
    }
    return QIcon::fromTheme(name);
}

/* Install the Fusion base style + our Win11 stylesheet onto the app. */
inline void apply(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setStyleSheet(stylesheet());

    QFont f = app.font();
    f.setPointSizeF(10.0);
    app.setFont(f);
}

} // namespace Win11Style
