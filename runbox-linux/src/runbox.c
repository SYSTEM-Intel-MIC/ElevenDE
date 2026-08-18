/*
 * RunBox —— Linux 上的 Windows 风格"运行"对话框
 *
 * 编译依赖: GTK3、libX11
 * 用法:
 *   runbox           打开运行对话框
 *   runbox --daemon  后台监听全局快捷键 Super+R（X11）
 *
 * MIT License
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#define RUNBOX_VERSION "1.0.0"
#define HISTORY_MAX 25

static GtkWidget *g_window;
static GtkWidget *g_combo;
static GtkWidget *g_entry;
static GtkWidget *g_chk_root;
static GtkWidget *g_chk_term;
static const char *g_self = "runbox";

/* ---------------- Windows 常用命令别名 ---------------- */

static const char *TERM_PROGS[] = {
    "x-terminal-emulator", "gnome-terminal", "konsole", "xfce4-terminal",
    "mate-terminal", "tilix", "terminator", "alacritty", "kitty", "xterm",
    NULL
};
static const char *EDIT_PROGS[] = {
    "elevende-notepad", "gedit", "kate", "mousepad", "pluma", "xed",
    "leafpad", "kwrite", NULL
};
static const char *CALC_PROGS[] = {
    "elevende-calc", "gnome-calculator", "kcalc", "mate-calc", "galculator", NULL
};
static const char *CTRL_PROGS[] = {
    "elevende-settings", "gnome-control-center", "systemsettings5", "systemsettings",
    "xfce4-settings-manager", NULL
};
static const char *FILE_PROGS[] = {
    "explorer.exe", "nautilus", "dolphin", "thunar", "caja", "pcmanfm", NULL
};
static const char *TASK_PROGS[] = {
    "elevende-taskmgr", "gnome-system-monitor", "plasma-systemmonitor",
    "xfce4-taskmanager", "mate-system-monitor", NULL
};
static const char *XDG_OPEN_CHECK[] = { "xdg-open", NULL };
static const char *PKEXEC_CHECK[] = { "pkexec", NULL };

typedef struct {
    const char *name;
    const char *const *candidates;
} Alias;

static const Alias ALIASES[] = {
    { "cmd",      TERM_PROGS },
    { "command",  TERM_PROGS },
    { "notepad",  EDIT_PROGS },
    { "calc",     CALC_PROGS },
    { "control",  CTRL_PROGS },
    { "explorer", FILE_PROGS },
    { "taskmgr",  TASK_PROGS },
    { NULL, NULL }
};

/* ---------------- 小工具 ---------------- */

static gchar *sh_quote(const gchar *s)
{
    GString *q = g_string_new("'");
    for (const gchar *p = s; *p; p++) {
        if (*p == '\'')
            g_string_append(q, "'\\''");
        else
            g_string_append_c(q, *p);
    }
    g_string_append_c(q, '\'');
    return g_string_free(q, FALSE);
}

static char *find_first(const char *const names[])
{
    for (int i = 0; names[i]; i++) {
        char *p = g_find_program_in_path(names[i]);
        if (p)
            return p;
    }
    return NULL;
}

static void show_error(const gchar *msg)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg);
    gtk_window_set_title(GTK_WINDOW(d), "运行");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* ---------------- 历史记录 ---------------- */

static gchar *history_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "runbox", "history", NULL);
}

static GList *load_history(void)
{
    GList *list = NULL;
    gchar *path = history_file();
    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *s = g_strstrip(g_strdup(lines[i]));
            if (*s)
                list = g_list_append(list, s);
            else
                g_free(s);
        }
        g_strfreev(lines);
        g_free(content);
    }
    g_free(path);
    return list;
}

static void save_history(const gchar *cmd)
{
    GList *list = load_history();

    for (GList *l = list; l;) {
        if (g_ascii_strcasecmp(l->data, cmd) == 0) {
            GList *next = l->next;
            g_free(l->data);
            list = g_list_delete_link(list, l);
            l = next;
        } else {
            l = l->next;
        }
    }

    list = g_list_prepend(list, g_strdup(cmd));
    while (g_list_length(list) > HISTORY_MAX) {
        GList *last = g_list_last(list);
        g_free(last->data);
        list = g_list_delete_link(list, last);
    }

    gchar *dir = g_build_filename(g_get_user_config_dir(), "runbox", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GString *out = g_string_new(NULL);
    for (GList *l = list; l; l = l->next)
        g_string_append_printf(out, "%s\n", (char *)l->data);

    gchar *path = history_file();
    g_file_set_contents(path, out->str, -1, NULL);
    g_string_free(out, TRUE);
    g_free(path);
    g_list_free_full(list, g_free);
}

/* ---------------- 执行逻辑 ---------------- */

static gchar *expand_tilde(const gchar *path)
{
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0'))
        return g_build_filename(g_get_home_dir(), path + 1, NULL);
    return g_strdup(path);
}

static void do_run(void)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(g_entry));
    gchar *cmd = g_strstrip(g_strdup(text));
    if (!*cmd) {
        g_free(cmd);
        return;
    }

    gboolean as_root = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_chk_root));
    gboolean in_term = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_chk_term));

    /* 1) Windows 常用命令别名 */
    gchar *resolved = NULL;
    gboolean resolved_is_term = FALSE;
    for (int i = 0; ALIASES[i].name; i++) {
        if (g_ascii_strcasecmp(cmd, ALIASES[i].name) == 0) {
            resolved = find_first(ALIASES[i].candidates);
            if (ALIASES[i].candidates == TERM_PROGS)
                resolved_is_term = TRUE;
            if (!resolved) {
                gchar *msg = g_strdup_printf(
                    "找不到与 \"%s\" 对应的程序。\n请确认系统已安装相应软件。", cmd);
                show_error(msg);
                g_free(msg);
                g_free(cmd);
                return;
            }
            break;
        }
    }

    gchar *cmdline = NULL;

    if (!resolved &&
        (g_str_has_prefix(cmd, "http://") || g_str_has_prefix(cmd, "https://") ||
         g_str_has_prefix(cmd, "www."))) {
        /* 网址 → 默认浏览器 */
        if (!find_first(XDG_OPEN_CHECK)) {
            show_error("未找到 xdg-open，请先安装 xdg-utils。");
            g_free(cmd);
            return;
        }
        gchar *q = sh_quote(cmd);
        cmdline = g_strdup_printf("xdg-open %s", q);
        g_free(q);
    } else if (!resolved && (strchr(cmd, '/') || cmd[0] == '~')) {
        /* 看起来是路径，且文件存在 → 用默认程序打开（类似 Windows） */
        gchar *p = expand_tilde(cmd);
        if (g_file_test(p, G_FILE_TEST_EXISTS)) {
            gchar *q = sh_quote(p);
            cmdline = g_strdup_printf("xdg-open %s", q);
            g_free(q);
        }
        g_free(p);
        /* 不存在则继续按普通命令处理 */
    }

    if (!cmdline)
        cmdline = g_strdup(resolved ? resolved : cmd);

    /* 2) 在终端中运行 */
    if (in_term && !resolved_is_term) {
        gchar *term = find_first(TERM_PROGS);
        if (!term) {
            show_error("未找到可用的终端程序。");
            g_free(cmdline);
            g_free(resolved);
            g_free(cmd);
            return;
        }
        gchar *inner = g_strdup_printf(
            "%s\necho\nprintf '程序已结束，按回车键关闭窗口...'\nread _", cmdline);
        gchar *q = sh_quote(inner);
        gchar *old = cmdline;
        if (strstr(term, "gnome-terminal") || strstr(term, "tilix"))
            cmdline = g_strdup_printf("%s -- bash -c %s", term, q);
        else
            cmdline = g_strdup_printf("%s -e bash -c %s", term, q);
        g_free(old);
        g_free(inner);
        g_free(q);
        g_free(term);
    }

    /* 3) 以管理员身份运行 */
    if (as_root) {
        if (!find_first(PKEXEC_CHECK)) {
            show_error("未找到 pkexec，请先安装 polkit。");
            g_free(cmdline);
            g_free(resolved);
            g_free(cmd);
            return;
        }
        gchar *old = cmdline;
        cmdline = g_strdup_printf("pkexec %s", old);
        g_free(old);
    }

    GError *err = NULL;
    gboolean ok = g_spawn_command_line_async(cmdline, &err);
    if (!ok) {
        gchar *msg = g_strdup_printf("无法运行 \"%s\"：\n%s", cmd,
                                     err ? err->message : "未知错误");
        show_error(msg);
        g_free(msg);
        if (err)
            g_error_free(err);
    } else {
        save_history(text);
        gtk_main_quit();
    }

    g_free(cmdline);
    g_free(resolved);
    g_free(cmd);
}

/* ---------------- 界面 ---------------- */

static const gchar *CSS =
    "window.runbox { background-color: #f0f0f0; }"
    ".runbox-titlebar { background-color: #ffffff; border-bottom: 1px solid #e3e3e3;"
    "  box-shadow: none; min-height: 30px; }"
    ".runbox-titlebar label { color: #111111; font-weight: normal; }"
    ".runbox-titlebar button { border: none; box-shadow: none; background: transparent;"
    "  border-radius: 0; min-width: 40px; }"
    ".runbox-titlebar button:hover { background: #e5e5e5; }"
    "window.runbox button { border-radius: 2px; padding: 4px 10px; }"
    "window.runbox button.runbox-ok { background: #0078d7; border-color: #0078d7;"
    "  color: #ffffff; }"
    "window.runbox button.runbox-ok:hover { background: #1a86dc; }"
    "window.runbox button.runbox-ok:active { background: #006cbe; }"
    "window.runbox combobox entry { background: #ffffff; border: 1px solid #7a7a7a;"
    "  border-radius: 0; box-shadow: none; }"
    "window.runbox combobox entry:focus { border-color: #0078d7; }";

static void on_browse(GtkButton *btn, gpointer user)
{
    (void)btn;
    (void)user;
    GtkWidget *d = gtk_file_chooser_dialog_new("浏览", GTK_WINDOW(g_window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "取消", GTK_RESPONSE_CANCEL,
        "确定", GTK_RESPONSE_ACCEPT,
        NULL);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        gchar *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (fn) {
            gchar *q = sh_quote(fn);
            gtk_entry_set_text(GTK_ENTRY(g_entry), q);
            g_free(q);
            g_free(fn);
        }
    }
    gtk_widget_destroy(d);
}

static void activate_run(GtkEntry *e, gpointer u)
{
    (void)e;
    (void)u;
    do_run();
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer u)
{
    (void)w;
    (void)u;
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

static void build_ui(void)
{
    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "运行");
    gtk_window_set_icon_name(GTK_WINDOW(g_window), "runbox");
    gtk_window_set_default_icon_name("system-run");
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_position(GTK_WINDOW(g_window), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(g_window), 480, -1);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(g_window, "key-press-event", G_CALLBACK(on_key), NULL);

    /* 标题栏交给窗口管理器（ElevenDE 的 Win11 主题统一绘制）；
     * 之前的 GTK CSD 头栏会和 WM 标题栏叠成两层，按钮样式也错乱。 */
    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_window), root_box);

    /* 上半部分：图标 + 说明 + 输入框 */
    GtkWidget *grid = gtk_grid_new();
    gtk_widget_set_margin_start(grid, 20);
    gtk_widget_set_margin_end(grid, 20);
    gtk_widget_set_margin_top(grid, 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    GtkWidget *icon = gtk_image_new_from_icon_name("system-run", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 36);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), icon, 0, 0, 1, 2);

    GtkWidget *label = gtk_label_new(
        "输入程序、文件夹、文档或互联网资源的名称，系统将为您打开它。");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_size_request(label, 330, -1);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 1, 0, 2, 1);

    GtkWidget *open_label = gtk_label_new_with_mnemonic("打开(_O):");
    gtk_label_set_xalign(GTK_LABEL(open_label), 0);
    gtk_grid_attach(GTK_GRID(grid), open_label, 1, 1, 1, 1);

    g_combo = gtk_combo_box_text_new_with_entry();
    g_entry = gtk_bin_get_child(GTK_BIN(g_combo));
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_entry),
        "例如：gedit、firefox、calc、~/文档");
    gtk_widget_set_hexpand(g_combo, TRUE);
    gtk_widget_set_size_request(g_combo, 300, -1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(open_label), g_entry);
    gtk_grid_attach(GTK_GRID(grid), g_combo, 2, 1, 1, 1);

    GList *hist = load_history();
    for (GList *l = hist; l; l = l->next)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_combo), l->data);
    g_list_free_full(hist, g_free);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_combo), -1);
    gtk_entry_set_text(GTK_ENTRY(g_entry), "");

    gtk_box_pack_start(GTK_BOX(root_box), grid, FALSE, FALSE, 0);

    /* 选项复选框 */
    GtkWidget *checks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_start(checks, 68);
    gtk_widget_set_margin_top(checks, 6);
    g_chk_root = gtk_check_button_new_with_label("以管理员身份运行");
    g_chk_term = gtk_check_button_new_with_label("在终端中运行");
    gtk_box_pack_start(GTK_BOX(checks), g_chk_root, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(checks), g_chk_term, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_box), checks, FALSE, FALSE, 0);

    /* 按钮行：确定 / 取消 / 浏览 */
    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btnbox, GTK_ALIGN_END);
    gtk_widget_set_margin_start(btnbox, 20);
    gtk_widget_set_margin_end(btnbox, 20);
    gtk_widget_set_margin_top(btnbox, 12);
    gtk_widget_set_margin_bottom(btnbox, 18);

    GtkWidget *b_ok = gtk_button_new_with_label("确定");
    GtkWidget *b_cancel = gtk_button_new_with_label("取消");
    GtkWidget *b_browse = gtk_button_new_with_mnemonic("浏览(_B)");
    gtk_widget_set_size_request(b_ok, 92, -1);
    gtk_widget_set_size_request(b_cancel, 92, -1);
    gtk_widget_set_size_request(b_browse, 92, -1);
    gtk_widget_set_can_default(b_ok, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b_ok), "runbox-ok");
    gtk_window_set_default(GTK_WINDOW(g_window), b_ok);

    g_signal_connect(b_ok, "clicked", G_CALLBACK(activate_run), NULL);
    g_signal_connect_swapped(b_cancel, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(b_browse, "clicked", G_CALLBACK(on_browse), NULL);
    g_signal_connect(g_entry, "activate", G_CALLBACK(activate_run), NULL);

    gtk_box_pack_end(GTK_BOX(btnbox), b_browse, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), b_cancel, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), b_ok, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_box), btnbox, FALSE, FALSE, 0);
}

/* ---------------- 全局快捷键守护进程（X11） ---------------- */

static int run_daemon(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr,
            "RunBox: 无法连接 X 服务器。\n"
            "Wayland 环境下请在桌面设置的快捷键中把命令 runbox 绑定到 Super+R。\n");
        return 1;
    }

    Window root = DefaultRootWindow(d);
    KeyCode kc = XKeysymToKeycode(d, XK_r);
    if (!kc) {
        fprintf(stderr, "RunBox: 无法解析按键 r。\n");
        return 1;
    }

    /* 兼容 NumLock / CapsLock 状态 */
    unsigned int combos[] = {
        Mod4Mask,
        Mod4Mask | Mod2Mask,
        Mod4Mask | LockMask,
        Mod4Mask | Mod2Mask | LockMask,
    };
    int grabbed = 0;
    for (size_t i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
        if (XGrabKey(d, kc, combos[i], root, True, GrabModeAsync, GrabModeAsync))
            grabbed++;
    }
    if (!grabbed) {
        fprintf(stderr, "RunBox: Super+R 已被其他程序占用，无法注册全局快捷键。\n");
        return 1;
    }

    XSelectInput(d, root, KeyPressMask);
    XSync(d, False);
    signal(SIGCHLD, SIG_IGN);

    printf("RunBox 守护进程已启动：按 Super+R 呼出运行对话框。\n");
    fflush(stdout);

    XEvent ev;
    while (1) {
        XNextEvent(d, &ev);
        if (ev.type != KeyPress)
            continue;
        if (ev.xkey.keycode != kc)
            continue;
        if (!(ev.xkey.state & Mod4Mask))
            continue;
        if (ev.xkey.state & (ControlMask | Mod1Mask))
            continue;
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execlp("runbox", "runbox", (char *)NULL);
            execl(g_self, "runbox", (char *)NULL);
            _exit(127);
        }
    }
    return 0;
}

/* ---------------- 入口 ---------------- */

static void usage(void)
{
    printf(
        "RunBox %s —— Linux 上的 Windows 风格\"运行\"对话框\n"
        "\n"
        "用法:\n"
        "  runbox            打开运行对话框\n"
        "  runbox --daemon   后台运行，监听全局快捷键 Super+R（仅 X11）\n"
        "  runbox --version  显示版本\n"
        "  runbox --help     显示本帮助\n",
        RUNBOX_VERSION);
}

static gboolean on_focus_in_refocus(GtkWidget *w, GdkEvent *ev, gpointer ud)
{
    (void)w; (void)ev;
    gtk_widget_grab_focus(GTK_WIDGET(ud));
    return FALSE;
}

int main(int argc, char **argv)
{
    if (argc > 0 && argv[0])
        g_self = argv[0];
    setlocale(LC_ALL, "");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0)
            return run_daemon();
        if (strcmp(argv[i], "--version") == 0) {
            printf("RunBox %s\n", RUNBOX_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
    }

    gtk_init(&argc, &argv);

    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    build_ui();
    gtk_widget_show_all(g_window);
    gtk_window_present(GTK_WINDOW(g_window));
    gtk_widget_grab_focus(g_entry);
    /* 重新获得焦点时确保输入框可打字（配合 WM 的 focusNew 行为） */
    g_signal_connect(g_window, "focus-in-event",
                     G_CALLBACK(on_focus_in_refocus), g_entry);
    gtk_main();

    return 0;
}
