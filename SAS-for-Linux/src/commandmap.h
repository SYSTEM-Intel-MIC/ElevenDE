#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct Action {
    QString command;
    QStringList args;
    QVector<QStringList> fallbacks;   // command 不存在时的备用命令（各自带 args）
    bool exitAfter = true;            // 点击后是否关闭 SAS 界面
};

class CommandMap {
public:
    // 配置查找顺序：--config 参数 → ~/.config/sas-screen/config.json
    //               → <安装目录>/../share/sas-screen/sas-screen.json → 内置默认值
    static CommandMap load(const QString &overridePath = QString());

    bool has(const QString &label) const { return m_actions.contains(label); }
    QStringList labels() const { return m_order; }
    bool exitAfter(const QString &label) const { return m_actions.value(label).exitAfter; }
    bool run(const QString &label) const;          // 启动外部命令，返回是否成功启动

    Action footerAction(const QString &key) const { return m_footer.value(key); }
    bool runFooter(const QString &key) const;

    // 用户默认终端：$TERMINAL → xdg-terminal-exec → 常见终端列表
    static QString defaultTerminal();
    // 按终端生成"以它执行某命令"的参数（xdg-terminal-exec/gnome-terminal 语法不同）
    static QStringList termInvokeArgs(const QString &terminal, const QStringList &cmd);

private:
    static Action parseAction(const QJsonObject &obj);
    static CommandMap defaults();
    static bool launch(const Action &a);

    QHash<QString, Action> m_actions;
    QHash<QString, Action> m_footer;
    QVector<QString> m_order;
};