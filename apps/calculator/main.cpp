/*
 * elevende-calc - Windows 11 style standard calculator for ElevenDE.
 *
 * Layout mirrors Win11 Calculator: big right-aligned display with a small
 * pending-expression line, then the 4x6 grid (%, CE, C, ⌫ / 1/x, x², √x, ÷
 * / 7 8 9 × / 4 5 6 − / 1 2 3 + / ±, 0, ., =). Full keyboard support.
 */

#include <QApplication>
#include <QFont>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstring>

#include "../common/win11style.h"

namespace {

class Calculator : public QWidget
{
    Q_OBJECT
public:
    Calculator()
    {
        setWindowTitle(QStringLiteral("计算器"));
        setWindowIcon(Win11Style::appIcon(QStringLiteral("accessories-calculator")));
        setFixedSize(340, 500);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(10, 10, 10, 12);
        root->setSpacing(6);

        m_expr = new QLabel(QStringLiteral(" "), this);
        m_expr->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_expr->setProperty("subtle", true);
        QFont ef = m_expr->font();
        ef.setPointSizeF(10.5);
        m_expr->setFont(ef);

        m_disp = new QLabel(QStringLiteral("0"), this);
        m_disp->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QFont df = m_disp->font();
        df.setPointSizeF(34);
        df.setWeight(QFont::DemiBold);
        m_disp->setFont(df);
        m_disp->setMinimumHeight(64);

        root->addWidget(m_expr);
        root->addWidget(m_disp);

        auto *grid = new QGridLayout();
        grid->setSpacing(4);

        struct Key { const char *label; int role; };
        /* role: 0 digit-ish handled by label, see below for specials */
        const char *rows[6][4] = {
            { "%", "CE", "C", "\xE2\x8C\xAB" },      /* %, CE, C, backspace */
            { "1/x", "x\xC2\xB2", "\xE2\x88\x9Ax", "\xC3\xB7" },
            { "7", "8", "9", "\xC3\x97" },
            { "4", "5", "6", "\xE2\x88\x92" },
            { "1", "2", "3", "+" },
            { "\xC2\xB1", "0", ".", "=" },
        };
        for (int r = 0; r < 6; ++r) {
            for (int c = 0; c < 4; ++c) {
                QPushButton *b = new QPushButton(QString::fromUtf8(rows[r][c]), this);
                b->setMinimumHeight(52);
                QFont bf = b->font();
                bf.setPointSizeF(13.5);
                b->setFont(bf);
                const bool isDigit = QString::fromUtf8(rows[r][c]).length() == 1 &&
                                     ((rows[r][c][0] >= '0' && rows[r][c][0] <= '9') ||
                                      rows[r][c][0] == '.');
                const bool isEquals = qstrcmp(rows[r][c], "=") == 0;
                if (isEquals)
                    b->setProperty("accent", true);
                if (isDigit || isEquals) {
                    /* digits + '=' get the raised card look */
                    b->setStyleSheet(b->styleSheet());
                }
                grid->addWidget(b, r, c);
                connect(b, &QPushButton::clicked, this,
                        [this, label = QString::fromUtf8(rows[r][c])] { onKey(label); });
            }
        }
        root->addLayout(grid, 1);
    }

protected:
    void keyPressEvent(QKeyEvent *ev) override
    {
        const QString t = ev->text();
        if (!t.isEmpty() && t.length() == 1) {
            const QChar ch = t.at(0);
            if (ch.isDigit() || ch == '.') { onKey(t); return; }
            switch (ch.toLatin1()) {
            case '+': onKey(QStringLiteral("+")); return;
            case '-': onKey(QStringLiteral("−")); return;
            case '*': onKey(QStringLiteral("×")); return;
            case '/': onKey(QStringLiteral("÷")); return;
            case '%': onKey(QStringLiteral("%")); return;
            case '\r':
            case '=': onKey(QStringLiteral("=")); return;
            }
        }
        switch (ev->key()) {
        case Qt::Key_Backspace: onKey(QStringLiteral("⌫")); return;
        case Qt::Key_Escape: onKey(QStringLiteral("C")); return;
        case Qt::Key_Delete: onKey(QStringLiteral("CE")); return;
        default: break;
        }
        QWidget::keyPressEvent(ev);
    }

private:
    void onKey(const QString &k)
    {
        if (k.length() == 1 && k.at(0).isDigit()) { digit(k.at(0).digitValue()); return; }
        if (k == QStringLiteral(".")) { dot(); return; }
        if (k == QStringLiteral("±")) { negate(); return; }
        if (k == QStringLiteral("⌫")) { backspace(); return; }
        if (k == QStringLiteral("C")) { clearAll(); return; }
        if (k == QStringLiteral("CE")) { clearEntry(); return; }
        if (k == QStringLiteral("%")) { percent(); return; }
        if (k == QStringLiteral("1/x")) { unary([](double v) { return 1.0 / v; }, QStringLiteral("1/(%1)")); return; }
        if (k == QStringLiteral("x²")) { unary([](double v) { return v * v; }, QStringLiteral("sqr(%1)")); return; }
        if (k == QStringLiteral("√x")) { unary([](double v) { return std::sqrt(v); }, QStringLiteral("√(%1)")); return; }
        if (k == QStringLiteral("+") || k == QStringLiteral("−") ||
            k == QStringLiteral("×") || k == QStringLiteral("÷")) {
            binop(k.at(0));
            return;
        }
        if (k == QStringLiteral("=")) { equals(); return; }
    }

    void digit(int d)
    {
        if (m_fresh) { m_entry.clear(); m_fresh = false; }
        if (m_entry == QStringLiteral("0")) m_entry.clear();
        if (m_entry.length() >= 16) return;
        m_entry += QChar('0' + d);
        render();
    }
    void dot()
    {
        if (m_fresh) { m_entry.clear(); m_fresh = false; }
        if (m_entry.isEmpty()) m_entry = QStringLiteral("0");
        if (!m_entry.contains('.')) m_entry += '.';
        render();
    }
    void negate()
    {
        if (m_entry.isEmpty() || m_entry == QStringLiteral("0")) return;
        if (m_entry.startsWith('-')) m_entry.remove(0, 1);
        else m_entry.prepend('-');
        render();
    }
    void backspace()
    {
        if (m_fresh) return;
        m_entry.chop(1);
        if (m_entry.isEmpty() || m_entry == QStringLiteral("-")) m_entry = QStringLiteral("0");
        render();
    }
    void clearAll()
    {
        m_entry = QStringLiteral("0");
        m_acc = 0; m_pending = 0; m_fresh = true;
        m_expr->setText(QStringLiteral(" "));
        render();
    }
    void clearEntry()
    {
        m_entry = QStringLiteral("0");
        m_fresh = true;
        render();
    }
    void percent()
    {
        const double v = value();
        const double r = (m_pending != 0) ? (m_acc * v / 100.0) : (v / 100.0);
        m_entry = format(r);
        m_fresh = true;
        render();
    }
    void unary(double (*fn)(double), const QString &label)
    {
        const double v = value();
        const double r = fn(v);
        if (!std::isfinite(r)) { error(); return; }
        m_expr->setText(label.arg(format(v)));
        m_entry = format(r);
        m_fresh = true;
        render();
    }
    void binop(QChar op)
    {
        if (m_pending != 0 && !m_fresh) {
            if (!applyPending(value())) return;
        } else {
            m_acc = value();
        }
        m_pending = op.unicode();
        m_expr->setText(QStringLiteral("%1 %2").arg(format(m_acc), QString(op)));
        m_fresh = true;
    }
    void equals()
    {
        if (m_pending == 0) {
            m_expr->setText(QStringLiteral("%1=").arg(format(value())));
            return;
        }
        const double rhs = m_fresh ? m_acc : value();
        m_expr->setText(QStringLiteral("%1 %2 %3 =")
                            .arg(format(m_acc), QString(QChar(m_pending)), format(rhs)));
        if (!applyPending(rhs)) return;
        m_entry = format(m_acc);
        m_pending = 0;
        m_fresh = true;
        render();
    }
    bool applyPending(double rhs)
    {
        double r = 0;
        switch (m_pending) {
        case '+': r = m_acc + rhs; break;
        case 0x2212: r = m_acc - rhs; break;   /* − */
        case 0x00D7: r = m_acc * rhs; break;   /* × */
        case 0x00F7:
            if (rhs == 0) { error(); return false; }
            r = m_acc / rhs;
            break;
        default: r = rhs; break;
        }
        if (!std::isfinite(r)) { error(); return false; }
        m_acc = r;
        return true;
    }

    double value() const
    {
        bool ok = false;
        const double v = m_entry.toDouble(&ok);
        return ok ? v : 0.0;
    }
    static QString format(double v)
    {
        if (std::isnan(v) || std::isinf(v)) return QStringLiteral("错误");
        /* trim trailing zeros like Win11 calc */
        QString s = QString::number(v, 'g', 15);
        return s;
    }
    void render() { m_disp->setText(m_entry.isEmpty() ? QStringLiteral("0") : m_entry); }
    void error()
    {
        m_disp->setText(QStringLiteral("除数不能为零"));
        m_entry = QStringLiteral("0");
        m_acc = 0; m_pending = 0; m_fresh = true;
        m_expr->setText(QStringLiteral(" "));
    }

    QLabel *m_disp;
    QLabel *m_expr;
    QString m_entry = QStringLiteral("0");
    double m_acc = 0;
    int m_pending = 0;
    bool m_fresh = true;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("elevende-calc"));
    QApplication::setOrganizationName(QStringLiteral("elevende"));
    Win11Style::apply(app);

    Calculator w;
    w.show();
    return app.exec();
}

#include "main.moc"
