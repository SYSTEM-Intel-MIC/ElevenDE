#pragma once

#include <QModelIndex>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

class QAbstractItemView;
class QAction;
class QEvent;
class QFileSystemModel;
class QListView;
class QStackedWidget;
class QTreeView;

class FileList : public QWidget
{
    Q_OBJECT
public:
    enum ViewMode { IconView = 0, ListView = 1, DetailsView = 2 };
    /* sort columns: mirrors the QFileSystemModel columns (0 name, 1 size,
     * 2 type, 3 date), sorted folders-first via the proxy */
    enum SortColumn { SortName = 0, SortSize = 1, SortType = 2, SortDate = 3 };

    explicit FileList(QWidget *parent = nullptr);

    void showDirectory(const QString &path);
    QString currentDirectory() const;

    ViewMode viewMode() const { return m_mode; }
    void setViewMode(ViewMode mode);

    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool on);

    /* substring filter applied to the current folder (case-insensitive) */
    void applyNameFilter(const QString &filter);

    QStringList selectedPaths() const;
    QAbstractItemView *currentView();

public slots:
    /* NOTE: toggleHidden must be a slot -- the Ctrl+H QShortcut connects to
     * it with the old-style string-based connect() */
    void toggleHidden();
    void openSelected();
    void renameSelected();
    void trashSelected();
    void deleteSelectedPermanent();
    void copySelected();
    void cutSelected();
    void pasteHere();
    void selectAllViews();
    void goUpOne();
    void refresh();
    void createFolder();
    void createTextDocument();
    void copyCurrentPath();
    void showSelectedProperties();
    void setIconView();
    void setListView();
    void setDetailsView();
    void sortByName();
    void sortBySize();
    void sortByType();
    void sortByDate();

signals:
    void directoryActivated(const QString &path);
    /* an item should be opened for editing etc. -> navigate/launch handled by
     * the window */
    void statusInfo(const QString &text); /* "N 项 · 已选 M · 可用 X GB" */

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onOpenIndex(const QModelIndex &index);
    void onContextMenu(const QPoint &pos);
    void onSelectionChanged();

private:
    QWidget *currentViewWidget() const;
    QModelIndex currentIndexAt(const QPoint &pos);
    void setRootAll();
    void refreshViews();
    void openFile(const QString &path);
    void onMenuAction(QAction *action, const QString &filePath);
    bool isFileView(QObject *obj) const;
    void onContextMenuFrom(QWidget *src, const QPoint &pos);
    void setSort(SortColumn column, Qt::SortOrder order);
    void updateStatus();
    /* touch input (3.5.1): long-press = context menu, tap = select,
       double-tap = open, drag = scroll the list under the finger */
    bool handleTouchEvent(QObject *obj, class QTouchEvent *ev);
    void touchReset();

    class Model;
    class ModelProxy;
    Model *m_model;
    ModelProxy *m_proxy;
    QStackedWidget *m_stack;
    QListView *m_iconView;
    QListView *m_listView;
    QTreeView *m_treeView;
    QString m_current;
    QModelIndex m_proxyRoot;
    QStringList m_menuTargets; /* files the currently-open context menu acts on */
    ViewMode m_mode = IconView;
    SortColumn m_sortCol = SortName;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    bool m_showHidden = false;
    bool m_menuOpen = false;
    /* one-finger gesture state shared by the three views */
    class QTimer *m_touchTimer = nullptr;
    QPoint m_touchOrigin;      /* viewport coords where the finger landed */
    QPoint m_touchLast;        /* last reported position                  */
    bool m_touchScrolling = false;
    bool m_touchMenuFired = false;
    bool m_touchActive = false;
    qint64 m_tapTimeMs = 0;    /* timestamp of the previous tap           */
    QModelIndex m_tapIndex;    /* item of the previous tap                */
};