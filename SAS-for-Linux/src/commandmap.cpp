#include "commandmap.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

Action CommandMap::parseAction(const QJsonObject &obj)
{
    Action a;
    a.command = obj.value(QStringLiteral("command")).toString();
    const QJsonArray args = obj.value(QStringLiteral("args")).toArray();
    for (const QJsonValue &v : args)
        a.args << v.toString();
    const QJsonArray fb = obj.value(QStringLiteral("fallback")).toArray();
    for (const QJsonValue &v : fb) {
        if (v.isObject()) {
            const Action f = parseAction(v.toObject());
            if (!f.command.isEmpty())
                a.fallbacks << (QStringList() << f.command << f.args);
        }
    }
    a.exitAfter = obj.value(QStringLiteral("exitAfter")).toBool(true);
    return a;
}

CommandMap CommandMap::defaults()
{
    CommandMap m;
    m.m_actions.insert(QStringLiteral("锁定"), {QStringLiteral("loginctl"),
        {QStringLiteral("lock-session")}, {}, true});
    m.m_actions.insert(QStringLiteral("注销"), {QStringLiteral("loginctl"),
        {QStringLiteral("terminate-session"), QStringLiteral("self")},
        {{QStringLiteral("qdbus6"), QStringLiteral("org.kde.ksmserver"), QStringLiteral("/KSMServer"),
          QStringLiteral("logout"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0")},
         {QStringLiteral("qdbus"), QStringLiteral("org.kde.ksmserver"), QStringLiteral("/KSMServer"),
          QStringLiteral("logout"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0")},
         {QStringLiteral("gnome-session-quit"), QStringLiteral("--logout"), QStringLiteral("--no-prompt")},
         {QStringLiteral("niri"), QStringLiteral("msg"), QStringLiteral("action"), QStringLiteral("quit")},
         {QStringLiteral("swaymsg"), QStringLiteral("exit")},
         {QStringLiteral("hyprctl"), QStringLiteral("dispatch"), QStringLiteral("exit")}}, true});
    const QString term = defaultTerminal();
    m.m_actions.insert(QStringLiteral("更改密码"), {term,
        termInvokeArgs(term, {QStringLiteral("passwd")}),
        {{QStringLiteral("gnome-terminal"), QStringLiteral("--"), QStringLiteral("passwd")},
         {QStringLiteral("xterm"), QStringLiteral("-e"), QStringLiteral("passwd")}}, true});
    m.m_actions.insert(QStringLiteral("任务管理器"), {QStringLiteral("missioncenter"),
        {},
        {{QStringLiteral("gnome-system-monitor")}, {QStringLiteral("ksysguard")},
         {QStringLiteral("xfce4-taskmanager")}}, true});

    m.m_order << QStringLiteral("锁定") << QStringLiteral("注销")
              << QStringLiteral("更改密码") << QStringLiteral("任务管理器");

    m.m_footer.insert(QStringLiteral("input"), {QStringLiteral("ibus"),
        {QStringLiteral("engine")},
        {{QStringLiteral("fcitx5"), QStringLiteral("-n"), QStringLiteral("keyboard")}}, false});
    m.m_footer.insert(QStringLiteral("network"), {QStringLiteral(""),
        {}, {}, false});
    m.m_footer.insert(QStringLiteral("accessibility"), {QStringLiteral("plasma-open-settings"),
        {QStringLiteral("accessibility")},
        {{QStringLiteral("systemsettings"), QStringLiteral("kcm_access")},
         {QStringLiteral("gnome-control-center"), QStringLiteral("universal-access")}}, false});
    return m;
}

CommandMap CommandMap::load(const QString &overridePath)
{
    QStringList candidates;
    if (!overridePath.isEmpty())
        candidates << overridePath;
    candidates << (QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                   + QStringLiteral("/sas-screen/config.json"));
    candidates << (QCoreApplication::applicationDirPath()
                   + QStringLiteral("/../share/sas-screen/sas-screen.json"));

    for (const QString &path : candidates) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject root = doc.object();
        CommandMap m;
        const QJsonObject btns = root.value(QStringLiteral("buttons")).toObject();
        for (auto it = btns.begin(); it != btns.end(); ++it) {
            if (!it.value().isObject())
                continue;
            const Action a = parseAction(it.value().toObject());
            if (a.command.isEmpty())
                continue;
            m.m_actions.insert(it.key(), a);
            m.m_order << it.key();
        }
        const QJsonObject footer = root.value(QStringLiteral("footer")).toObject();
        for (auto it = footer.begin(); it != footer.end(); ++it) {
            if (!it.value().isObject())
                continue;
            m.m_footer.insert(it.key(), parseAction(it.value().toObject()));
        }
        if (!m.m_order.isEmpty())
            return m;
    }
    return defaults();
}

bool CommandMap::launch(const Action &a)
{
    if (a.command.isEmpty())
        return false;
    QString cmd = a.command;
    QStringList args = a.args;
    if (QStandardPaths::findExecutable(cmd).isEmpty()) {
        bool found = false;
        for (const QStringList &fb : a.fallbacks) {
            if (fb.isEmpty())
                continue;
            if (!QStandardPaths::findExecutable(fb.first()).isEmpty()) {
                cmd = fb.first();
                args = fb.mid(1);
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return QProcess::startDetached(cmd, args);
}

bool CommandMap::run(const QString &label) const
{
    return launch(m_actions.value(label));
}

bool CommandMap::runFooter(const QString &key) const
{
    return launch(m_footer.value(key));
}

QString CommandMap::defaultTerminal()
{
    const QString fromEnv = qEnvironmentVariable("TERMINAL");
    if (!fromEnv.isEmpty()) {
        const QString exe = fromEnv.split(QLatin1Char(' '), Qt::SkipEmptyParts).value(0);
        if (!exe.isEmpty() && !QStandardPaths::findExecutable(exe).isEmpty())
            return exe;
    }
    static const char *cands[] = {"xdg-terminal-exec", "kitty", "alacritty", "wezterm",
                                  "foot", "gnome-terminal", "konsole",
                                  "xfce4-terminal", "xterm"};
    for (const char *c : cands) {
        const QString name = QLatin1String(c);
        if (!QStandardPaths::findExecutable(name).isEmpty())
            return name;
    }
    return QStringLiteral("xterm");
}

QStringList CommandMap::termInvokeArgs(const QString &terminal, const QStringList &cmd)
{
    if (terminal.contains(QLatin1String("xdg-terminal-exec"))
        || terminal.contains(QLatin1String("gnome-terminal")))
        return cmd;   // 两者都直接把要执行的命令作为参数
    QStringList args{QStringLiteral("-e")};
    args += cmd;
    return args;
}