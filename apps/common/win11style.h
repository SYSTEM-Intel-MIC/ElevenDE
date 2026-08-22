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
#include <QEvent>
#include <QObject>
#include <QWidget>
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
    /* Lindows intentionally uses the fixed Windows 11 default blue. The
       Settings app no longer exposes unsupported custom color controls. */
    return defaultAccent();
}

inline bool darkMode()
{
    /* Light-only surface, matching the Lindows Settings contract. */
    return false;
}

/* A complete Fluent-style stylesheet for the current accent/dark setting. */
inline QString materialTexturePath()
{
    const QString system = QStringLiteral("/usr/local/share/elevende-shell/material/mica-noise.png");
    if (QFileInfo::exists(system)) return system;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/../share/elevende-shell/material/mica-noise.png");
}

class AcrylicWindowFilter final : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            if (auto *window = qobject_cast<QWidget *>(watched); window && window->isWindow()) {
                /* Do not make normal application windows translucent. On X11
                   this can leave an ARGB parent over the child widget tree,
                   producing blank content and swallowing interaction. The
                   Mica-like surface is rendered by opaque QSS layers instead. */
                window->setAttribute(Qt::WA_TranslucentBackground, false);
                window->setProperty("micaMaterial", false);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

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
    const QString material = materialTexturePath();
    const QString micaBase = dark ? QStringLiteral("rgba(32, 36, 43, 218)")
                                  : QStringLiteral("rgba(246, 248, 252, 224)");
    const QString micaCard = dark ? QStringLiteral("rgba(47, 53, 63, 220)")
                                  : QStringLiteral("rgba(255, 255, 255, 218)");

    QString qss;
    qss += QStringLiteral(
        "* { font-family: 'Segoe UI', 'Noto Sans CJK SC', 'Microsoft YaHei', sans-serif; }\n"
        "QWidget { background-color: %1; color: %2; font-size: 13px; }\n"
        "QMainWindow, QDialog, QWidget[micaMaterial=\"true\"] { background-color: %14; background-image: url(%15); border: 1px solid rgba(255,255,255,0.10); }\n"
        "QFrame[card=\"true\"], QWidget[card=\"true\"] { background-color: %16; background-image: url(%15); }\n"
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
        "QCheckBox::indicator:checked { background: %9; border-color: %9; image: url(/usr/local/share/elevende-shell/icons/32x32/apps/checkmark.png); }\n"

        /* Sliders */
        "QSlider { min-height: 24px; }"
        "QSlider::groove:horizontal { height: 4px; background: %5; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: %9; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %9; border: 2px solid %9; width: 16px; height: 16px;"
        " margin: -6px 0; border-radius: 8px; }\n"

        /* Progress */
        "QProgressBar { background: %4; border: 1px solid %5; border-radius: 6px; text-align: center; }"
        "QProgressBar::chunk { background: %9; border-radius: 5px; }\n"

        /* Global Fluent navigation surfaces */
        "QWidget[navRail=\"true\"] { background-color: %13; border-right: 1px solid %5; }\n"
        "QWidget[navRail=\"true\"] QToolButton { background: transparent; color: %2; border: none; border-radius: 8px; font-size: 14px; padding: 8px 12px; text-align: left; }\n"
        "QWidget[navRail=\"true\"] QToolButton:hover { background-color: %7; }\n"
        "QWidget[navRail=\"true\"] QToolButton:checked { background-color: %9; color: %10; }\n"
        "QWidget[navRail=\"true\"] QToolButton:focus { outline: none; border: 1px solid %9; }\n"
        /* Performance cards / Task Manager */
        "QWidget[graphCell=\"true\"] { background-color: %13; border: 1px solid %5; border-radius: 3px; }\n"
        "QWidget[graphCell=\"true\"] QLabel { padding-left: 6px; }\n"
        "QStatusBar { background: %1; color: %3; border-top: 1px solid %5; padding: 3px 8px; }\n"
        "QStatusBar QLabel { margin-left: 12px; }\n"

        /* Tooltips */
        "QToolTip { background-color: %6; color: %2; border: 1px solid %5; padding: 4px 8px; border-radius: 4px; }\n"
    ).arg(bg, text, sub, card, border, card2, hover, pressed,
          accent.name(), accentTxt,
          accent.lighter(115).name(), accent.darker(120).name(), field, micaBase, material, micaCard);

    return qss;
}

/* Resolve a first-party app icon without depending on an external icon-cache
 * rebuild. The absolute SVG path also gives Qt a valid _NET_WM_ICON, which
 * keeps taskbar buttons from degrading to a black/blank placeholder. */
inline QIcon appIcon(const QString &name)
{
    const QStringList pngRoots = {
        QStringLiteral("/usr/local/share/elevende-shell/icons/64x64/apps/"),
        QStringLiteral("/usr/local/share/elevende-shell/icons/48x48/apps/"),
        QStringLiteral("/usr/local/share/elevende-shell/icons/32x32/apps/"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/elevende-shell/icons/64x64/apps/")
    };
    for (const QString &root : pngRoots) {
        /* Official WindowsIcons PNGs are authoritative. SVG is never tried
           before them because the generated aliases can be blank or stale. */
        const QString png = root + name + QStringLiteral(".png");
        if (QFileInfo::exists(png)) return QIcon(png);
    }
    const QStringList svgRoots = {
        QStringLiteral("/usr/local/share/elevende-shell/icons/scalable/apps/"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/elevende-shell/icons/scalable/apps/")
    };
    for (const QString &root : svgRoots) {
        const QString svg = root + name + QStringLiteral(".svg");
        if (QFileInfo::exists(svg)) return QIcon(svg);
    }
    return QIcon::fromTheme(name);
}

/* Install the Fusion base style + our Win11 stylesheet onto the app. */
inline void apply(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setStyleSheet(stylesheet());
    /* Make top-level Qt windows compositor-friendly. The GLX picom profile
     * supplies the real backdrop blur; the SVG supplies deterministic noise
     * and keeps the surface attractive when GLX blur is unavailable. */
    /* The filter keeps ordinary Qt client content opaque on X11; blur is a
       compositor-only decoration effect and must never cover controls. */
    app.installEventFilter(new AcrylicWindowFilter(&app));

    QFont f = app.font();
    f.setPointSizeF(10.0);
    app.setFont(f);
}

} // namespace Win11Style
