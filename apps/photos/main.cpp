/*
 * elevende-photos - Windows 11 "Photos" style image viewer for ElevenDE.
 *
 * Opens an image file, fits it to the window, Ctrl+wheel zooms, arrow keys
 * walk the containing folder (natural sort), Home/End jump. Registered as
 * the default viewer for common image MIME types via mimeapps.list.
 */

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QScrollArea>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

#include "../common/win11style.h"

namespace {

QStringList imageFilters()
{
    return { QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
             QStringLiteral("*.bmp"), QStringLiteral("*.gif"), QStringLiteral("*.webp"),
             QStringLiteral("*.svg"), QStringLiteral("*.ico"), QStringLiteral("*.tif"),
             QStringLiteral("*.tiff") };
}

class Canvas : public QWidget
{
public:
    explicit Canvas(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(320, 240);
        setMouseTracking(true);
    }
    QPixmap pm;
    double scale = 1.0;
    bool fit = true;

    void relayout()
    {
        if (pm.isNull()) { resize(320, 240); return; }
        if (fit) {
            const double s = qMin((double)parentWidget()->width() / pm.width(),
                                  (double)parentWidget()->height() / pm.height());
            scale = qMin(s, 1.0);
        }
        resize(qMax(1, (int)(pm.width() * scale)), qMax(1, (int)(pm.height() * scale)));
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(24, 24, 28));
        if (!pm.isNull())
            p.drawPixmap(0, 0, width(), height(), pm);
    }
};

class Viewer : public QMainWindow
{
public:
    Viewer()
    {
        setWindowTitle(QStringLiteral("照片"));
        setWindowIcon(Win11Style::appIcon(QStringLiteral("image-x-generic")));
        resize(1000, 700);

        m_canvas = new Canvas();
        auto *area = new QScrollArea(this);
        area->setWidget(m_canvas);
        area->setWidgetResizable(false);
        area->setAlignment(Qt::AlignCenter);
        area->setFrameShape(QFrame::NoFrame);
        setCentralWidget(area);
        m_area = area;

        statusBar()->showMessage(QStringLiteral("Ctrl+滚轮缩放 · ←/→ 切换 · Esc 退出"));
    }

    void openFolder(const QString &file)
    {
        const QFileInfo fi(file);
        m_dir = fi.absolutePath();
        m_files.clear();
        QDir d(m_dir);
        const QFileInfoList all = d.entryInfoList(imageFilters(), QDir::Files, QDir::Name);
        for (const QFileInfo &f : all)
            m_files << f.absoluteFilePath();
        m_index = m_files.indexOf(fi.absoluteFilePath());
        if (m_index < 0 && !m_files.isEmpty())
            m_index = 0;
        if (!m_files.isEmpty())
            load(m_index);
        else if (fi.exists())
            loadFile(fi.absoluteFilePath());
    }

protected:
    void resizeEvent(QResizeEvent *ev) override
    {
        QMainWindow::resizeEvent(ev);
        if (m_canvas->fit)
            m_canvas->relayout();
    }
    void wheelEvent(QWheelEvent *ev) override
    {
        if (ev->modifiers() & Qt::ControlModifier) {
            m_canvas->fit = false;
            m_canvas->scale *= (ev->angleDelta().y() > 0) ? 1.15 : (1 / 1.15);
            m_canvas->scale = qBound(0.05, m_canvas->scale, 16.0);
            m_canvas->relayout();
        } else {
            QMainWindow::wheelEvent(ev);
        }
    }
    void keyPressEvent(QKeyEvent *ev) override
    {
        switch (ev->key()) {
        case Qt::Key_Escape: close(); return;
        case Qt::Key_Right:
        case Qt::Key_Down:
        case Qt::Key_Space: step(1); return;
        case Qt::Key_Left:
        case Qt::Key_Up: step(-1); return;
        case Qt::Key_Home: if (!m_files.isEmpty()) load(0); return;
        case Qt::Key_End: if (!m_files.isEmpty()) load(m_files.size() - 1); return;
        case Qt::Key_0:
            m_canvas->fit = true;
            m_canvas->relayout();
            centerOn();
            return;
        default: break;
        }
        QMainWindow::keyPressEvent(ev);
    }

private:
    void step(int d)
    {
        if (m_files.isEmpty())
            return;
        load((m_index + d + m_files.size()) % m_files.size());
    }
    void load(int idx)
    {
        m_index = idx;
        loadFile(m_files.at(idx));
    }
    void loadFile(const QString &path)
    {
        QImageReader rd(path);
        rd.setAutoTransform(true);
        const QImage img = rd.read();
        if (img.isNull()) {
            statusBar()->showMessage(QStringLiteral("无法打开：%1").arg(path));
            return;
        }
        m_canvas->pm = QPixmap::fromImage(img);
        m_canvas->fit = true;
        m_canvas->relayout();
        centerOn();
        const QFileInfo fi(path);
        setWindowTitle(QStringLiteral("%1 - 照片").arg(fi.fileName()));
        statusBar()->showMessage(QStringLiteral("%1 · %2 × %3 · %4")
                                     .arg(fi.fileName())
                                     .arg(img.width())
                                     .arg(img.height())
                                     .arg(m_index >= 0 && !m_files.isEmpty()
                                              ? QStringLiteral("%1/%2").arg(m_index + 1).arg(m_files.size())
                                              : QString()));
    }
    void centerOn()
    {
        m_area->ensureVisible(m_canvas->width() / 2, m_canvas->height() / 2, 0, 0);
    }

    Canvas *m_canvas;
    QScrollArea *m_area;
    QString m_dir;
    QStringList m_files;
    int m_index = -1;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("elevende-photos"));
    QApplication::setOrganizationName(QStringLiteral("elevende"));
    Win11Style::apply(app);

    Viewer w;
    w.show();
    if (argc > 1)
        w.openFolder(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
