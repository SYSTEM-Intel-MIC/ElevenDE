/*
 * elevende-notepad - Windows 11 style Notepad for ElevenDE.
 *
 * Mirrors the classic Notepad interaction: menu bar (File/Edit/View),
 * a single document area, Find (Ctrl+F / F3), word-wrap toggle, zoom
 * (Ctrl+= / Ctrl+- / Ctrl+0), a status bar with Ln/Col, and "Save changes?"
 * on close. Encoding is UTF-8 first with a Latin-1 fallback for legacy files.
 */

#include <QApplication>
#include <QAction>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextStream>
#include <QVBoxLayout>

#include "../common/win11style.h"

namespace {

/* Read a file as UTF-8; fall back to Latin-1 if the bytes are not valid
 * UTF-8 (mirrors Notepad's legacy-ANSI handling). */
QString readFileText(const QString &path, bool *wasUtf8)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray raw = f.readAll();
    const QString utf8 = QString::fromUtf8(raw);
    /* QString::fromUtf8 replaces invalid bytes with U+FFFD; detect that. */
    if (utf8.contains(QChar(0xFFFD)) && !raw.contains("\xEF\xBF\xBD")) {
        if (wasUtf8) *wasUtf8 = false;
        return QString::fromLatin1(raw);
    }
    if (wasUtf8) *wasUtf8 = true;
    return utf8;
}

class FindBar : public QWidget
{
    Q_OBJECT
public:
    explicit FindBar(QPlainTextEdit *edit, QWidget *parent = nullptr)
        : QWidget(parent), m_edit(edit)
    {
        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->setSpacing(6);
        m_field = new QLineEdit(this);
        m_field->setPlaceholderText(QStringLiteral("查找内容…"));
        m_field->setClearButtonEnabled(true);
        auto *next = new QPushButton(QStringLiteral("查找下一个"), this);
        auto *close = new QPushButton(QStringLiteral("×"), this);
        close->setFixedWidth(32);
        close->setToolTip(QStringLiteral("关闭查找栏 (Esc)"));
        lay->addWidget(m_field, 1);
        lay->addWidget(next);
        lay->addWidget(close);
        setProperty("card", true);
        connect(next, &QPushButton::clicked, this, &FindBar::findNext);
        connect(m_field, &QLineEdit::returnPressed, this, &FindBar::findNext);
        connect(close, &QPushButton::clicked, this, &FindBar::hideBar);
    }
    void showBar()
    {
        show();
        m_field->setFocus();
        m_field->selectAll();
    }
    void hideBar()
    {
        hide();
        m_edit->setFocus();
    }
    void findNext()
    {
        const QString needle = m_field->text();
        if (needle.isEmpty())
            return;
        if (!m_edit->find(needle)) {
            /* wrap around from the top, like Notepad */
            QTextCursor cur = m_edit->textCursor();
            cur.movePosition(QTextCursor::Start);
            m_edit->setTextCursor(cur);
            if (!m_edit->find(needle))
                m_field->setStyleSheet(QStringLiteral("border-bottom:2px solid #C42B1C;"));
            else
                m_field->setStyleSheet(QString());
        } else {
            m_field->setStyleSheet(QString());
        }
    }
protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(e);
            if (ke->key() == Qt::Key_Escape) {
                hideBar();
                return true;
            }
        }
        return QWidget::event(e);
    }
private:
    QPlainTextEdit *m_edit;
    QLineEdit *m_field;
};

class Notepad : public QMainWindow
{
    Q_OBJECT
public:
    Notepad()
    {
        setWindowTitle(QStringLiteral("无标题 - 记事本"));
        setWindowIcon(Win11Style::appIcon(QStringLiteral("accessories-text-editor")));
        resize(760, 540);

        m_edit = new QPlainTextEdit(this);
        m_edit->setFrameShape(QFrame::NoFrame);
        m_edit->setTabStopDistance(4 * QFontMetricsF(font()).averageCharWidth());
        m_find = new FindBar(m_edit, this);
        m_find->hide();

        auto *central = new QWidget(this);
        auto *v = new QVBoxLayout(central);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        v->addWidget(m_find);
        v->addWidget(m_edit, 1);
        setCentralWidget(central);

        buildMenus();
        buildStatus();

        connect(m_edit, &QPlainTextEdit::modificationChanged,
                this, &Notepad::onModified);
        connect(m_edit, &QPlainTextEdit::cursorPositionChanged,
                this, &Notepad::updateCursorPos);
        connect(m_edit, &QPlainTextEdit::textChanged, this, &Notepad::updateCursorPos);
        updateCursorPos();
    }

    void openPath(const QString &path) { doOpen(path); }

protected:
    void closeEvent(QCloseEvent *ev) override
    {
        if (guardUnsaved())
            ev->accept();
        else
            ev->ignore();
    }

private slots:
    void newFile()
    {
        if (!guardUnsaved()) return;
        m_edit->clear();
        m_path.clear();
        m_edit->document()->setModified(false);
        refreshTitle();
    }
    void openFile()
    {
        if (!guardUnsaved()) return;
        const QString p = QFileDialog::getOpenFileName(
            this, QStringLiteral("打开"), QString(),
            QStringLiteral("文本文件 (*.txt *.md *.log *.ini *.conf);;所有文件 (*)"));
        if (!p.isEmpty())
            doOpen(p);
    }
    void saveFile() { doSave(false); }
    void saveAs() { doSave(true); }
    void insertDateTime()
    {
        m_edit->insertPlainText(
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy/M/d HH:mm")));
    }
    void toggleWrap(bool on) { m_edit->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth
                                                           : QPlainTextEdit::NoWrap); }
    void zoomIn() { m_edit->zoomIn(1); syncZoomLabel(); }
    void zoomOut() { m_edit->zoomOut(1); syncZoomLabel(); }
    void zoomReset()
    {
        QFont f = m_edit->font();
        f.setPointSize(11);
        m_edit->setFont(f);
        syncZoomLabel();
    }

private:
    void buildMenus()
    {
        auto *mb = menuBar();

        auto *mFile = mb->addMenu(QStringLiteral("文件(&F)"));
        mFile->addAction(QStringLiteral("新建(&N)"), this, &Notepad::newFile,
                         QKeySequence(QStringLiteral("Ctrl+N")));
        mFile->addAction(QStringLiteral("打开(&O)…"), this, &Notepad::openFile,
                         QKeySequence(QStringLiteral("Ctrl+O")));
        mFile->addSeparator();
        mFile->addAction(QStringLiteral("保存(&S)"), this, &Notepad::saveFile,
                         QKeySequence(QStringLiteral("Ctrl+S")));
        mFile->addAction(QStringLiteral("另存为(&A)…"), this, &Notepad::saveAs,
                         QKeySequence(QStringLiteral("Ctrl+Shift+S")));
        mFile->addSeparator();
        mFile->addAction(QStringLiteral("退出(&X)"), this, &QWidget::close,
                         QKeySequence(QStringLiteral("Alt+F4")));

        auto *mEdit = mb->addMenu(QStringLiteral("编辑(&E)"));
        mEdit->addAction(QStringLiteral("撤销(&U)"), m_edit, &QPlainTextEdit::undo,
                         QKeySequence(QStringLiteral("Ctrl+Z")));
        mEdit->addAction(QStringLiteral("剪切(&T)"), m_edit, &QPlainTextEdit::cut,
                         QKeySequence(QStringLiteral("Ctrl+X")));
        mEdit->addAction(QStringLiteral("复制(&C)"), m_edit, &QPlainTextEdit::copy,
                         QKeySequence(QStringLiteral("Ctrl+C")));
        mEdit->addAction(QStringLiteral("粘贴(&P)"), m_edit, &QPlainTextEdit::paste,
                         QKeySequence(QStringLiteral("Ctrl+V")));
        mEdit->addAction(QStringLiteral("删除(&L)"), m_edit, [this] {
            QTextCursor c = m_edit->textCursor();
            if (c.hasSelection()) c.removeSelectedText();
            else c.deleteChar();
        }, QKeySequence(QStringLiteral("Del")));
        mEdit->addSeparator();
        mEdit->addAction(QStringLiteral("查找(&F)…"), this, [this] { m_find->showBar(); },
                         QKeySequence(QStringLiteral("Ctrl+F")));
        mEdit->addAction(QStringLiteral("查找下一个(&N)"), m_find, &FindBar::findNext,
                         QKeySequence(QStringLiteral("F3")));
        mEdit->addSeparator();
        mEdit->addAction(QStringLiteral("全选(&A)"), m_edit, &QPlainTextEdit::selectAll,
                         QKeySequence(QStringLiteral("Ctrl+A")));
        mEdit->addAction(QStringLiteral("时间/日期(&D)"), this, &Notepad::insertDateTime,
                         QKeySequence(QStringLiteral("F5")));

        auto *mView = mb->addMenu(QStringLiteral("查看(&V)"));
        mView->addAction(QStringLiteral("放大(&I)"), this, &Notepad::zoomIn,
                         QKeySequence(QStringLiteral("Ctrl+=")));
        mView->addAction(QStringLiteral("缩小(&O)"), this, &Notepad::zoomOut,
                         QKeySequence(QStringLiteral("Ctrl+-")));
        mView->addAction(QStringLiteral("恢复默认缩放(&R)"), this, &Notepad::zoomReset,
                         QKeySequence(QStringLiteral("Ctrl+0")));
        m_wrapAct = mView->addAction(QStringLiteral("自动换行(&W)"));
        m_wrapAct->setCheckable(true);
        m_wrapAct->setChecked(true);
        connect(m_wrapAct, &QAction::toggled, this, &Notepad::toggleWrap);
    }

    void buildStatus()
    {
        m_posLabel = new QLabel(this);
        m_zoomLabel = new QLabel(QStringLiteral("100%"), this);
        m_encLabel = new QLabel(QStringLiteral("UTF-8"), this);
        statusBar()->addPermanentWidget(m_posLabel);
        statusBar()->addPermanentWidget(m_zoomLabel);
        statusBar()->addPermanentWidget(m_encLabel);
    }

    void syncZoomLabel()
    {
        const int pct = qRound(QFontMetricsF(m_edit->font()).height() /
                               QFontMetricsF(QFont(QStringLiteral("Sans"), 11)).height() * 100.0);
        m_zoomLabel->setText(QStringLiteral("%1%").arg(pct));
    }

    void updateCursorPos()
    {
        QTextCursor c = m_edit->textCursor();
        const int ln = c.blockNumber() + 1;
        const int col = c.columnNumber() + 1;
        m_posLabel->setText(QStringLiteral("第 %1 行，第 %2 列").arg(ln).arg(col));
    }

    void refreshTitle()
    {
        const QString base = m_path.isEmpty()
            ? QStringLiteral("无标题")
            : QFileInfo(m_path).fileName();
        const QString mod = m_edit->document()->isModified() ? QStringLiteral("*") : QString();
        setWindowTitle(QStringLiteral("%1%2 - 记事本").arg(mod, base));
    }

    void onModified() { refreshTitle(); }

    bool guardUnsaved()
    {
        if (!m_edit->document()->isModified())
            return true;
        const auto r = QMessageBox::question(
            this, QStringLiteral("记事本"),
            QStringLiteral("是否保存对文档的更改？"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (r == QMessageBox::Save)
            return doSave(false);
        return r == QMessageBox::Discard;
    }

    bool doSave(bool forceDialog)
    {
        QString path = m_path;
        if (path.isEmpty() || forceDialog) {
            path = QFileDialog::getSaveFileName(
                this, QStringLiteral("保存"), path.isEmpty() ? QStringLiteral("untitled.txt") : path,
                QStringLiteral("文本文档 (*.txt);;所有文件 (*)"));
            if (path.isEmpty())
                return false;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::critical(this, QStringLiteral("记事本"),
                                  QStringLiteral("无法写入文件：\n%1").arg(path));
            return false;
        }
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << m_edit->toPlainText();
        f.close();
        m_path = path;
        m_edit->document()->setModified(false);
        refreshTitle();
        return true;
    }

    void doOpen(const QString &path)
    {
        bool utf8 = true;
        const QString text = readFileText(path, &utf8);
        m_edit->setPlainText(text);
        m_path = path;
        m_edit->document()->setModified(false);
        m_encLabel->setText(utf8 ? QStringLiteral("UTF-8") : QStringLiteral("ANSI"));
        refreshTitle();
    }

    QPlainTextEdit *m_edit;
    FindBar *m_find;
    QLabel *m_posLabel;
    QLabel *m_zoomLabel;
    QLabel *m_encLabel;
    QAction *m_wrapAct;
    QString m_path;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("elevende-notepad"));
    QApplication::setOrganizationName(QStringLiteral("elevende"));
    Win11Style::apply(app);

    Notepad w;
    w.show();
    if (argc > 1)
        w.openPath(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}

#include "main.moc"
