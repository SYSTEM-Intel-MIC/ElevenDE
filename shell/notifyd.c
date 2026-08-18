/*
 * ElevenDE notification daemon -- org.freedesktop.Notifications over D-Bus,
 * rendering Win11/KDE-style popups top-right with plain Xlib + Xft.
 *
 * It owns the session-bus name, serves Notify/CloseNotification/
 * GetCapabilities/GetServerInformation, and draws override-redirect popups.
 * Purely optional companion of elevende-shell (shell works without it).
 */
#define DBUS_API_SUBJECT_TO_CHANGE 1

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define MAXN 6
#define PW   340
#define PAD  12
#define DEFAULT_MS 5000

typedef struct {
    Window  win;
    unsigned id;
    char    title[160];
    char    body[512];
    int     h;
    double  expire;
} N;

static Display  *dpy;
static int       scr;
static int       scr_w;   /* root window width (scr alone is the screen NUMBER) */
static Window    root;
static GC        gc;
static XftFont  *ft, *fs;
static XftDraw  *xd;
static XftColor  col_text, col_sub, col_bg, col_accent;

static N         nots[MAXN];
static int       nnot = 0;
static unsigned  next_id = 100;
static int       running = 1;
static DBusConnection *g_conn = NULL;

static void release_slot(int i);
static unsigned add_notification(const char *title, const char *body, int to);

/* 通知仅以右上角弹窗呈现；不再创建任务栏通知按钮。 */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static unsigned long fg_pixel(const char *name, unsigned long fallback) {
    XColor c;
    if (XParseColor(dpy, DefaultColormap(dpy, scr), name, &c) &&
        XAllocColor(dpy, DefaultColormap(dpy, scr), &c))
        return c.pixel;
    return fallback;
}

/* CJK-aware text: Xft picks a single font for "sans-serif"; fall back to a
   CJK face for glyphs the base font lacks. */
static XftFont *cjk_font(XftFont *base) {
    static XftFont *bases[8], *cjk[8];
    static int n = 0;
    for (int i = 0; i < n; i++)
        if (bases[i] == base) return cjk[i];
    if (n >= 8) return NULL;
    int px = (int)((base->ascent + base->descent) / 1.2f + 0.5f);
    if (px < 6) px = 6;
    if (px > 96) px = 96;
    char pat[128];
    XftFont *cf = NULL;
    static const char *fams[] = { "Noto Sans CJK SC", "WenQuanYi Zen Hei",
                                  "AR PL UMing CN" };
    for (size_t i = 0; i < sizeof fams / sizeof fams[0] && !cf; i++) {
        snprintf(pat, sizeof pat, "%s:pixelsize=%d", fams[i], px);
        cf = XftFontOpenName(dpy, scr, pat);
    }
    bases[n] = base;
    cjk[n] = cf;
    n++;
    return cf;
}

static void draw_utf8(XftDraw *xd, XftFont *base, XftColor *col, int x, int y,
                      const char *s) {
    XftFont *cf = cjk_font(base);
    if (!cf) {
        XftDrawStringUtf8(xd, col, base, x, y, (const FcChar8 *)s,
                          (int)strlen(s));
        return;
    }
    const unsigned char *p = (const unsigned char *)s;
    int pen = 0;
    while (*p) {
        unsigned char b0 = *p;
        FcChar32 cp;
        unsigned clen;
        if (b0 < 0x80)                { cp = b0;        clen = 1; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; clen = 2; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; clen = 3; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; clen = 4; }
        else                          { cp = b0;        clen = 1; }
        for (unsigned k = 1; k < clen; k++) cp = (cp << 6) | (p[k] & 0x3F);
        int use_cjk = !XftCharExists(dpy, base, cp);
        const unsigned char *q = p;
        while (*q) {
            unsigned char bb = *q;
            FcChar32 c2;
            unsigned cc;
            if (bb < 0x80)                { c2 = bb;        cc = 1; }
            else if ((bb & 0xE0) == 0xC0) { c2 = bb & 0x1F; cc = 2; }
            else if ((bb & 0xF0) == 0xE0) { c2 = bb & 0x0F; cc = 3; }
            else if ((bb & 0xF8) == 0xF0) { c2 = bb & 0x07; cc = 4; }
            else                          { c2 = bb;        cc = 1; }
            for (unsigned k = 1; k < cc; k++) c2 = (c2 << 6) | (q[k] & 0x3F);
            if ((!XftCharExists(dpy, base, c2)) != use_cjk) break;
            q += cc;
        }
        int rlen = (int)(q - p);
        XftFont *tf = use_cjk ? cf : base;
        XftDrawStringUtf8(xd, col, tf, x + pen, y, (const FcChar8 *)p, rlen);
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, tf, (const FcChar8 *)p, rlen, &ext);
        pen += ext.xOff;
        p = q;
    }
}

static void fill_round(Drawable d, int x, int y, int w, int h, int r,
                       unsigned long p) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int rr = r * 2;
    XSetForeground(dpy, gc, p);
    XFillRectangle(dpy, d, gc, x + r, y, w - rr, h);
    XFillRectangle(dpy, d, gc, x, y + r, w, h - rr);
    XFillArc(dpy, d, gc, x, y, rr, rr, 90 * 64, 90 * 64);
    XFillArc(dpy, d, gc, x + w - rr, y, rr, rr, 0, 90 * 64);
    XFillArc(dpy, d, gc, x, y + h - rr, rr, rr, 180 * 64, 90 * 64);
    XFillArc(dpy, d, gc, x + w - rr, y + h - rr, rr, rr, 270 * 64, 90 * 64);
}

static void emit_closed(unsigned id, unsigned reason) {
    if (!g_conn) return;
    DBusMessage *s = dbus_message_new_signal(
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
        "NotificationClosed");
    if (!s) return;
    dbus_message_append_args(s, DBUS_TYPE_UINT32, &id,
                             DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
    dbus_connection_send(g_conn, s, NULL);
    dbus_message_unref(s);
}

static void release_slot(int i) {
    if (i < 0 || i >= nnot) return;
    if (nots[i].win) XDestroyWindow(dpy, nots[i].win);
    memmove(&nots[i], &nots[i + 1], (size_t)(nnot - i - 1) * sizeof(N));
    nnot--;
}

static void make_window(N *n) {
    int body_lines = 1;
    for (const char *p = n->body; *p; p++) if (*p == '\n') body_lines++;
    if (body_lines > 5) body_lines = 5;
    int lh = fs->ascent + fs->descent + 5;
    int th = ft->ascent + ft->descent;
    n->h = PAD + th + 6 + body_lines * lh + PAD;
    n->win = XCreateSimpleWindow(dpy, root, 0, 0, PW, n->h, 1,
                                 fg_pixel("#2b2d36", 0), 0);
    XSetWindowAttributes sa = { 0 };
    sa.override_redirect = True;
    XChangeWindowAttributes(dpy, n->win, CWOverrideRedirect, &sa);
    XSelectInput(dpy, n->win, ExposureMask | ButtonPressMask);
}

static void draw_one(const N *n) {
    if (!n->win) return;
    int w = PW, h = n->h;
    fill_round(n->win, 0, 0, w - 1, h - 1, 10, col_bg.pixel);
    /* KDE-style accent strip on the left */
    XSetForeground(dpy, gc, col_accent.pixel);
    XFillRectangle(dpy, n->win, gc, 2, 10, 4, h - 20);
    xd = XftDrawCreate(dpy, n->win, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr));
    draw_utf8(xd, ft, &col_text, PAD + 6, 14 + ft->ascent, n->title);
    int y = 14 + ft->ascent + ft->descent + 8;
    char linebuf[256];
    const char *p = n->body;
    const char *e;
    int lines = 0;
    while (*p && lines < 5) {
        e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof linebuf) len = sizeof linebuf - 1;
        memcpy(linebuf, p, len);
        linebuf[len] = 0;
        draw_utf8(xd, fs, &col_sub, PAD + 6, y, linebuf);
        y += fs->ascent + fs->descent + 5;
        lines++;
        if (!e) break;
        p = e + 1;
    }
    XftDrawDestroy(xd);
    XFlush(dpy);
}

static unsigned add_notification(const char *title, const char *body,
                                 int timeout_ms) {
    if (nnot >= MAXN) {
        emit_closed(nots[0].id, 3);
        release_slot(0);
    }
    if (nnot >= MAXN) return 0;
    N *n = &nots[nnot];
    memset(n, 0, sizeof *n);
    n->id = next_id++;
    snprintf(n->title, sizeof n->title, "%s", title);
    snprintf(n->body, sizeof n->body, "%s", body);
    n->expire = now_ms() + (timeout_ms > 0 ? timeout_ms : DEFAULT_MS);
    make_window(n);
    nnot++;

    /* stack: move every popup into place */
    int y = 10;
    for (int i = 0; i < nnot; i++) {
        XMoveWindow(dpy, nots[i].win, scr_w - PW - 12, y);
        XMapRaised(dpy, nots[i].win);
        y += nots[i].h + 8;
    }
    /* Paint AFTER mapping: some X servers (Xvfb, and real servers without
     * backing store) discard drawing done to an unmapped window. */
    draw_one(n);
    XFlush(dpy);
    return n->id;
}

static void close_id(unsigned id) {
    for (int i = 0; i < nnot; i++)
        if (nots[i].id == id) {
            emit_closed(id, 2);
            release_slot(i);
            return;
        }
}

static DBusHandlerResult filter(DBusConnection *conn, DBusMessage *msg,
                                void *data) {
    (void)conn; (void)data;
    /* Dispatch by member name. dbus_message_is_method_call() must never be
     * called with a NULL member (libdbus assertion abort), and some clients
     * omit the interface field, so match on member + optional interface. */
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *member = dbus_message_get_member(msg);
    if (!member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(msg);
    if (iface && strcmp(iface, "org.freedesktop.Notifications") != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *rep = NULL;

    if (!strcmp(member, "GetCapabilities")) {
        rep = dbus_message_new_method_return(msg);
        if (rep) {
            DBusMessageIter it, sub;
            dbus_message_iter_init_append(rep, &it);
            const char *caps[] = { "body", "icon-static" };
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &sub);
            for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) {
                const char *c = caps[i];
                dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &c);
            }
            dbus_message_iter_close_container(&it, &sub);
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (!strcmp(member, "GetServerInformation")) {
        rep = dbus_message_new_method_return(msg);
        if (rep) {
            const char *name = "ElevenDE Notifications";
            const char *vendor = "ElevenDE";
            const char *ver = "1.0";
            const char *spec = "1.2";
            dbus_message_append_args(rep,
                DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &vendor,
                DBUS_TYPE_STRING, &ver, DBUS_TYPE_STRING, &spec,
                DBUS_TYPE_INVALID);
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (!strcmp(member, "CloseNotification")) {
        unsigned id = 0;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id,
                                  DBUS_TYPE_INVALID) && id)
            close_id(id);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (!strcmp(member, "Notify")) {
        DBusMessageIter it;
        if (!dbus_message_iter_init(msg, &it))
            return DBUS_HANDLER_RESULT_HANDLED;
        char *app = NULL, *icon = NULL, *summary = NULL, *body = NULL;
        unsigned replaces = 0;
        int32_t timeout = -1;
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
            dbus_message_iter_get_basic(&it, &app);
            dbus_message_iter_next(&it);
        }
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
            dbus_message_iter_get_basic(&it, &replaces);
            dbus_message_iter_next(&it);
        }
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
            dbus_message_iter_get_basic(&it, &icon);
            dbus_message_iter_next(&it);
        }
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
            dbus_message_iter_get_basic(&it, &summary);
            dbus_message_iter_next(&it);
        }
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
            dbus_message_iter_get_basic(&it, &body);
            dbus_message_iter_next(&it);
        }
        dbus_message_iter_next(&it);          /* actions array */
        dbus_message_iter_next(&it);          /* hints dict  */
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID)
            dbus_message_iter_get_basic(&it, &timeout);

        if (summary && *summary) {
            if (replaces) close_id(replaces);
            unsigned id = add_notification(summary, body ? body : "",
                                           (int)timeout);
            rep = dbus_message_new_method_return(msg);
            if (rep) {
                dbus_message_append_args(rep, DBUS_TYPE_UINT32, &id,
                                         DBUS_TYPE_INVALID);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "notifyd: no display\n"); return 1; }
    scr = DefaultScreen(dpy);
    scr_w = DisplayWidth(dpy, scr);
    root = DefaultRootWindow(dpy);
    gc = XCreateGC(dpy, root, 0, NULL);

    ft = XftFontOpenName(dpy, scr, "sans-serif:bold:pixelsize=13");
    fs = XftFontOpenName(dpy, scr, "sans-serif:pixelsize=11");
    if (!ft || !fs) { fprintf(stderr, "notifyd: fonts\n"); return 1; }

    XftColorAllocName(dpy, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr),
                      "#ececec", &col_text);
    XftColorAllocName(dpy, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr),
                      "#9aa4ab", &col_sub);
    XftColorAllocName(dpy, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr),
                      "#2b2d36", &col_bg);
    XftColorAllocName(dpy, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr),
                      "#0f6cbd", &col_accent);

    DBusError err;
    dbus_error_init(&err);
    g_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!g_conn) {
        fprintf(stderr, "notifyd: dbus: %s\n", err.message ? err.message : "?");
        return 1;
    }
    int name_ok = dbus_bus_request_name(
        g_conn, "org.freedesktop.Notifications",
        DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "notifyd: name: %s\n", err.message);
        dbus_error_free(&err);
    }
    if (name_ok != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "notifyd: another daemon owns the name\n");
        return 0;
    }
    dbus_connection_add_filter(g_conn, filter, NULL, NULL);
    dbus_connection_set_exit_on_disconnect(g_conn, FALSE);

    int dbus_fd = -1;
    dbus_connection_get_unix_fd(g_conn, &dbus_fd);
    while (running) {
        fd_set rset;
        FD_ZERO(&rset);
        int maxfd = ConnectionNumber(dpy);
        FD_SET(ConnectionNumber(dpy), &rset);
        if (dbus_fd >= 0) {
            FD_SET(dbus_fd, &rset);
            if (dbus_fd > maxfd) maxfd = dbus_fd;
        }
        struct timeval tv = { 0, 100000 };
        select(maxfd + 1, &rset, NULL, NULL, &tv);

        if (FD_ISSET(ConnectionNumber(dpy), &rset))
            while (XPending(dpy)) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == ButtonPress) {
                    for (int i = 0; i < nnot; i++)
                        if (nots[i].win == ev.xbutton.window) {
                            emit_closed(nots[i].id, 2);
                            release_slot(i);
                            break;
                        }
                } else if (ev.type == Expose) {
                    for (int i = 0; i < nnot; i++)
                        if (nots[i].win == ev.xexpose.window)
                            draw_one(&nots[i]);
                }
            }

        /* Drain pending DBus traffic. NOTE: dbus_connection_read_write_dispatch()
         * returns TRUE while the connection is ALIVE, not "messages pending" —
         * the old `while (...) ;` wrapper spun forever here and starved the X
         * event path (toasts never repainted on Expose, never expired, and
         * could not be clicked away). One non-blocking pass per iteration is
         * correct: select() above wakes us when more data arrives. */
        dbus_connection_read_write_dispatch(g_conn, 0);

        double nowt = now_ms();
        int removed = 0;
        for (int i = 0; i < nnot; i++)
            if (nowt >= nots[i].expire) {
                emit_closed(nots[i].id, 1);
                release_slot(i);
                removed = 1;
                break;
            }
        if (removed) {
            int y = 10;
            for (int i = 0; i < nnot; i++) {
                XMoveWindow(dpy, nots[i].win, scr_w - PW - 12, y);
                y += nots[i].h + 8;
            }
            XFlush(dpy);
        }
    }
    return 0;
}