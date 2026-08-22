/*
 * ElevenDE lock / login screen -- small C + Xlib program.
 *
 * Two roles:
 *   elevende-lock            lock the running session (Win+L or command)
 *   elevende-lock --login    password gate shown at session start
 *
 * The binary is installed setuid-root so it can read /etc/shadow (getspnam)
 * and verify the password with crypt(3). It grabs the keyboard and pointer,
 * paints an opaque full-screen panel (clock + user + password field) and
 * only releases input after the password matches (or the account has no
 * password set). Exit code 0 == unlocked.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>
#include <sys/stat.h>
#ifdef HAVE_GDKPIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif
#include <pwd.h>
#include <shadow.h>
#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>

#define PW_MAX 128

static Display *dpy;
static int     scr;
static Window  root, win;
static Cursor  pointer_cursor = None;
static Cursor  text_cursor = None;
static Visual *vis;
static Colormap cmap;
static GC       bgc;
static int      scr_w, scr_h;
static XftFont *f_clock, *f_label, *f_pw;
static Pixmap wall_pm = None;      /* darkened wallpaper, screen-sized */
static Pixmap avatar_pm = None;    /* official Fluent default account icon */
static Pixmap frame_pm = None;     /* 完整页面的离屏帧，避免直接逐项重绘闪烁 */
static int box_x, box_y, box_w, box_h;      /* password field geometry */
static int btn_x, btn_y, btn_s;             /* submit arrow button       */
static int pw_hover = 0;                    /* pointer is inside input   */
static int pw_focused = 1;                  /* keyboard always targets it */
static int caret_visible = 1;               /* visual blink phase        */

/* `login_user` is always the UNIX account used for authentication; the
 * display name comes from its GECOS field so the UI shows the name created by
 * the user, rather than a hard-coded account or environment placeholder. */
static char login_user[128] = "";
static char display_user[256] = "";
static char pw[PW_MAX];
static int  pwlen = 0;
static int  failed = 0;
static char msg[96] = "";
static int  login_mode = 0;


static Drawable paint_target(void) {
    return frame_pm ? (Drawable)frame_pm : (Drawable)win;
}

static void fill_rect(int x, int y, int w, int h, unsigned long px) {
    XSetForeground(dpy, bgc, px);
    XFillRectangle(dpy, paint_target(), bgc, x, y, w, h);
}

static void str_center(XftDraw *xd, XftFont *f, XftColor *col,
                       int cx, int y, const char *s) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, (int)strlen(s), &ext);
    XftDrawStringUtf8(xd, col, f, cx - ext.xOff / 2, y,
                      (const FcChar8 *)s, (int)strlen(s));
}

static unsigned long px(int r, int g, int b) {
    XColor c;
    char spec[24];
    snprintf(spec, sizeof spec, "#%02x%02x%02x", r, g, b);
    if (XParseColor(dpy, cmap, spec, &c) && XAllocColor(dpy, cmap, &c))
        return c.pixel;
    return BlackPixel(dpy, scr);
}

static void resolve_user_identity(void) {
    const char *env_user = getenv("USER");
    /* getuid() intentionally uses the real session owner even though the
       installed helper is setuid-root solely to read /etc/shadow. */
    struct passwd *pe = getpwuid(getuid());
    if (!pe && env_user && *env_user) pe = getpwnam(env_user);
    const char *account = pe && pe->pw_name && *pe->pw_name
                        ? pe->pw_name : (env_user && *env_user ? env_user : "user");
    snprintf(login_user, sizeof login_user, "%s", account);

    const char *gecos = pe ? pe->pw_gecos : NULL;
    size_t n = gecos ? strcspn(gecos, ",") : 0;
    while (n > 0 && (gecos[n - 1] == ' ' || gecos[n - 1] == '\t')) n--;
    if (n > 0)
        snprintf(display_user, sizeof display_user, "%.*s", (int)n, gecos);
    if (!display_user[0])
        snprintf(display_user, sizeof display_user, "%s", login_user);
}

static void fill_round_lock(int x, int y, int w, int h, int r, unsigned long p) {
    XSetForeground(dpy, bgc, p);
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int rr = r * 2;
    Drawable dst = paint_target();
    XFillRectangle(dpy, dst, bgc, x + r, y, w - rr, h);
    XFillRectangle(dpy, dst, bgc, x, y + r, w, h - rr);
    XFillArc(dpy, dst, bgc, x, y, rr, rr, 90 * 64, 90 * 64);
    XFillArc(dpy, dst, bgc, x + w - rr, y, rr, rr, 0, 90 * 64);
    XFillArc(dpy, dst, bgc, x, y + h - rr, rr, rr, 180 * 64, 90 * 64);
    XFillArc(dpy, dst, bgc, x + w - rr, y + h - rr, rr, rr, 270 * 64, 90 * 64);
}

/* load the desktop wallpaper, darken it (Win11 logon dims the background)
 * and keep it as a screen-sized pixmap */
static void load_wallpaper(void) {
#ifdef HAVE_GDKPIXBUF
    const char *home = getenv("HOME");
    char path[1024] = "";
    if (home) snprintf(path, sizeof path,
                       "%s/.local/share/elevende/wallpaper.png", home);
    struct stat st;
    if (!path[0] || stat(path, &st) != 0)
        snprintf(path, sizeof path,
                 "/usr/local/share/elevende-shell/wallpapers/wallpaper-lindows-light.png");
    if (stat(path, &st) != 0) return;
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) { if (err) g_error_free(err); return; }
    GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, scr_w, scr_h,
                                            GDK_INTERP_BILINEAR);
    if (sc) { g_object_unref(pb); pb = sc; }
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    const guchar *pd = gdk_pixbuf_get_pixels(pb);
    XImage *im = XCreateImage(dpy, vis, DefaultDepth(dpy, scr), ZPixmap, 0,
                              malloc((size_t)scr_w * scr_h * 4), scr_w, scr_h,
                              32, 0);
    if (!im) { g_object_unref(pb); return; }
    for (int y = 0; y < h && y < scr_h; y++) {
        const guchar *row = pd + (size_t)y * rs;
        for (int x = 0; x < w && x < scr_w; x++) {
            /* darken to 42% so the white logon UI stays readable */
            int r = row[x * nch + 0] * 42 / 100;
            int g = row[x * nch + 1] * 42 / 100;
            int b = row[x * nch + 2] * 42 / 100;
            char *d = im->data + y * im->bytes_per_line + x * 4;
            if (im->byte_order == LSBFirst) {
                d[0] = (char)b; d[1] = (char)g; d[2] = (char)r; d[3] = 0;
            } else {
                d[0] = 0; d[1] = (char)r; d[2] = (char)g; d[3] = (char)b;
            }
        }
    }
    wall_pm = XCreatePixmap(dpy, root, scr_w, scr_h, DefaultDepth(dpy, scr));
    XPutImage(dpy, wall_pm, bgc, im, 0, 0, 0, 0, scr_w, scr_h);
    XDestroyImage(im);
    g_object_unref(pb);
#else
    (void)wall_pm;
#endif
}

/* Load the packaged Fluent Person Filled glyph into a small pixmap.  Alpha is
 * composited against the avatar disc colour here because XCopyArea has no
 * alpha channel; this keeps anti-aliased edges clean instead of producing a
 * rectangular dark halo. */
static void load_default_avatar(void) {
#ifdef HAVE_GDKPIXBUF
    /* The dedicated copy is installed beside the shell data.  It is the
       primary path for this setuid login helper; the icon-tree path remains a
       compatibility fallback for developer builds and older installations. */
    const char *paths[] = {
        "/usr/local/share/elevende-shell/login-user-avatar.png",
        "/usr/local/share/elevende-shell/icons/96x96/apps/login-user-avatar.png",
        NULL
    };
    GdkPixbuf *pb = NULL;
    for (int i = 0; paths[i] && !pb; i++) {
        GError *err = NULL;
        pb = gdk_pixbuf_new_from_file_at_scale(paths[i], 74, 74, TRUE, &err);
        if (err) g_error_free(err);
    }
    if (!pb) return;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    int ha = gdk_pixbuf_get_has_alpha(pb);
    const guchar *pd = gdk_pixbuf_get_pixels(pb);
    if (w < 1 || h < 1 || (nch != 3 && nch != 4)) { g_object_unref(pb); return; }
    XImage *im = XCreateImage(dpy, vis, DefaultDepth(dpy, scr), ZPixmap, 0,
                              malloc((size_t)w * h * 4), w, h, 32, 0);
    if (!im) { g_object_unref(pb); return; }
    const int br = 43, bg = 58, bb = 105;  /* avatar-disc base colour */
    for (int y = 0; y < h; y++) {
        const guchar *row = pd + (size_t)y * rs;
        for (int x = 0; x < w; x++) {
            int a = ha ? row[x * nch + 3] : 255;
            int r = (row[x * nch + 0] * a + br * (255 - a)) / 255;
            int g = (row[x * nch + 1] * a + bg * (255 - a)) / 255;
            int b = (row[x * nch + 2] * a + bb * (255 - a)) / 255;
            char *d = im->data + y * im->bytes_per_line + x * 4;
            if (im->byte_order == LSBFirst) {
                d[0] = (char)b; d[1] = (char)g; d[2] = (char)r; d[3] = 0;
            } else {
                d[0] = 0; d[1] = (char)r; d[2] = (char)g; d[3] = (char)b;
            }
        }
    }
    avatar_pm = XCreatePixmap(dpy, root, w, h, DefaultDepth(dpy, scr));
    XPutImage(dpy, avatar_pm, bgc, im, 0, 0, 0, 0, w, h);
    XDestroyImage(im);
    g_object_unref(pb);
#endif
}

static void paint(void) {
    Drawable dst = paint_target();
    int cx = scr_w / 2;

    /* background: darkened wallpaper or Win11-dark gradient fallback */
    if (wall_pm) {
        XCopyArea(dpy, wall_pm, dst, bgc, 0, 0, scr_w, scr_h, 0, 0);
    } else {
        for (int y = 0; y < scr_h; y++) {
            int k = y * 26 / scr_h;                    /* #0e1a2b -> #040910 */
            fill_rect(0, y, scr_w, 1, px(14 - k / 2, 26 - k, 43 - k));
        }
    }

    XftColor ctxt, csub, cerr, cbtn;
    ctxt.pixel  = px(236, 236, 236);
    csub.pixel  = px(190, 195, 200);
    cerr.pixel  = px(255, 153, 153);
    cbtn.pixel  = px(255, 255, 255);
    ctxt.color.red = 0xecec; ctxt.color.green = 0xecec; ctxt.color.blue = 0xecec; ctxt.color.alpha = 0xffff;
    csub.color.red = 0xbebe; csub.color.green = 0xc3c3; csub.color.blue = 0xc8c8; csub.color.alpha = 0xffff;
    cerr.color.red = 0xffff; cerr.color.green = 0x9999; cerr.color.blue = 0x9999; cerr.color.alpha = 0xffff;
    cbtn.color.red = 0xffff; cbtn.color.green = 0xffff; cbtn.color.blue = 0xffff; cbtn.color.alpha = 0xffff;

    XftDraw *xd = XftDrawCreate(dpy, dst, vis, cmap);

    /* Windows 11-style account disc with the official Fluent Person Filled
       icon.  The fallback is deliberately minimal and is reached only if a
       damaged installation is missing the packaged asset. */
    int av_r = 56;
    int av_cy = scr_h / 2 - 138;
    XSetForeground(dpy, bgc, px(0x25, 0x35, 0x63));
    XFillArc(dpy, dst, bgc, cx - av_r, av_cy - av_r, av_r * 2, av_r * 2,
             0, 360 * 64);
    XSetForeground(dpy, bgc, px(0x2b, 0x3a, 0x69));
    XFillArc(dpy, dst, bgc, cx - av_r + 2, av_cy - av_r + 2,
             av_r * 2 - 4, av_r * 2 - 4, 0, 360 * 64);
    if (avatar_pm)
        XCopyArea(dpy, avatar_pm, dst, bgc, 0, 0, 74, 74, cx - 37, av_cy - 37);
    /* No textual fallback: a damaged asset must never show a literal “?” on
       the login page. The dedicated packaged icon above is the normal path. */

    /* The display name comes from the user's own system account metadata. */
    str_center(xd, f_clock, &ctxt, cx, av_cy + av_r + 54, display_user);

    /* password field (Win11: focused translucent field + submit arrow) */
    box_w = 320; box_h = 46;
    box_x = cx - box_w / 2;
    box_y = av_cy + av_r + 96;
    fill_round_lock(box_x, box_y, box_w, box_h, 8,
                    px(pw_hover ? 0x28 : 0x1f, pw_hover ? 0x34 : 0x26,
                       pw_hover ? 0x45 : 0x33));
    XSetForeground(dpy, bgc, pw_focused ? px(0x88, 0xc5, 0xff)
                                        : px(0x55, 0x60, 0x70));
    XSetLineAttributes(dpy, bgc, pw_focused ? 2 : 1, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(dpy, dst, bgc, box_x + 1, box_y + 1, box_w - 3, box_h - 3);
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
    /* submit button */
    btn_s = box_h - 10;
    btn_x = box_x + box_w - btn_s - 5;
    btn_y = box_y + 5;
    fill_round_lock(btn_x, btn_y, btn_s, btn_s, 4, px(0x33, 0x3d, 0x4d));
    /* arrow glyph */
    int acx = btn_x + btn_s / 2, acy = btn_y + btn_s / 2;
    XSetForeground(dpy, bgc, px(230, 230, 230));
    XSetLineAttributes(dpy, bgc, 2, LineSolid, CapRound, JoinRound);
    XDrawLine(dpy, dst, bgc, acx - 7, acy, acx + 6, acy);
    XDrawLine(dpy, dst, bgc, acx + 6, acy, acx + 1, acy - 5);
    XDrawLine(dpy, dst, bgc, acx + 6, acy, acx + 1, acy + 5);

    /* password dots */
    char dots[PW_MAX * 3 + 1];
    int n = 0;
    for (int i = 0; i < pwlen && i < 24; i++) {
        dots[n++] = (char)0xE2; dots[n++] = (char)0x97; dots[n++] = (char)0x8F;
    }
    dots[n] = 0;
    int dots_w = 0;
    if (n) {
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, f_pw, (const FcChar8 *)dots, n, &ext);
        dots_w = ext.xOff;
    }
    int text_x = box_x + 16;
    if (n) {
        XftDrawStringUtf8(xd, &ctxt, f_pw, text_x,
                          box_y + box_h / 2 + f_pw->ascent / 2 - 1,
                          (const FcChar8 *)dots, n);
    } else {
        XftDrawStringUtf8(xd, &csub, f_pw, text_x,
                          box_y + box_h / 2 + f_pw->ascent / 2 - 1,
                          (const FcChar8 *)"密码", 6);
    }
    /* The caret follows the hidden dots and blinks only while the password
       field is focused, avoiding a static line that looks like a rendering bug. */
    if (pw_focused && caret_visible) {
        XSetForeground(dpy, bgc, ctxt.pixel);
        XFillRectangle(dpy, dst, bgc, text_x + dots_w + 2,
                       box_y + 11, 2, box_h - 22);
    }

    if (failed) {
        const char *m = msg[0] ? msg : "密码错误";
        str_center(xd, f_label, &cerr, cx, box_y + box_h + 26, m);
    } else {
        str_center(xd, f_label, &csub, cx, box_y + box_h + 26,
                   login_mode ? "输入密码以登录到 ElevenDE"
                              : "输入密码以解锁");
    }

    /* bottom hint */
    str_center(xd, f_label, &csub, cx, scr_h - 34,
               "Enter 确认 · Backspace 删除 · Esc 清空");
    XftDrawDestroy(xd);
    if (frame_pm)
        XCopyArea(dpy, frame_pm, win, bgc, 0, 0, scr_w, scr_h, 0, 0);
    XFlush(dpy);
}

static int grab_all(void) {
    /* keyboard grab is mandatory; the pointer grab is best-effort -- the
     * Start menu (or any client) may hold an active pointer grab, and we
     * still lock fine without it because the full-screen panel is on top
     * and receives every click anyway. */
    int kb_ok = 0;
    for (int i = 0; i < 60 && !kb_ok; i++) {
        if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync,
                          CurrentTime) == GrabSuccess)
            kb_ok = 1;
        else
            usleep(50000);
    }
    if (!kb_ok) return 0;
    for (int i = 0; i < 20; i++) {
        /* 登录页是唯一允许抓取指针的窗口；抓取对象和光标都显式指向
           登录覆盖层，防止根窗口继承空光标或吞掉提交按钮点击。 */
        if (XGrabPointer(dpy, win, False,
                         PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, win, pointer_cursor, CurrentTime)
                == GrabSuccess)
            return 1;
        usleep(50000);
    }
    fprintf(stderr, "lock: pointer grab unavailable, continuing (keyboard locked)\n");
    return 1;
}

static void release_all(void) {
    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XFlush(dpy);
}

static int check_pw(const char *u, const char *p) {
    struct passwd *pe = getpwnam(u);
    if (!pe) return -1;
    const char *hash = pe->pw_passwd;
    if (!hash || !*hash) return 1;                   /* no password set */
    if (!strcmp(hash, "x") || !strcmp(hash, "*")) {
        /* Debian keeps the authoritative value in shadow.  An empty
           `sp_pwdp` is a valid no-password account and must remain empty;
           the previous conditional left `hash` as literal "x" and rejected
           an otherwise valid empty login. */
        struct spwd *sp = getspnam(u);
        if (!sp || !sp->sp_pwdp) return -1;
        hash = sp->sp_pwdp;
    }
    if (!hash || !*hash) return 1;
    if (hash[0] == '!' || hash[0] == '*') return 0;  /* locked account */
    char *c = crypt(p, hash);
    return (c && !strcmp(c, hash)) ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--login")) login_mode = 1;
    signal(SIGCHLD, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "lock: cannot open display\n"); return 1; }
    scr    = DefaultScreen(dpy);
    root   = DefaultRootWindow(dpy);
    vis    = DefaultVisual(dpy, scr);
    cmap   = DefaultColormap(dpy, scr);
    scr_w  = XWidthOfScreen(ScreenOfDisplay(dpy, scr));
    scr_h  = XHeightOfScreen(ScreenOfDisplay(dpy, scr));
    bgc    = XCreateGC(dpy, root, 0, NULL);
    pointer_cursor = XCreateFontCursor(dpy, XC_left_ptr);
    text_cursor = XCreateFontCursor(dpy, XC_xterm);

    f_clock  = XftFontOpenName(dpy, scr, "Noto Sans CJK SC:pixelsize=26");
    f_label  = XftFontOpenName(dpy, scr, "Noto Sans CJK SC:pixelsize=13");
    f_pw     = XftFontOpenName(dpy, scr, "Noto Sans CJK SC:pixelsize=18");
    if (!f_clock)  f_clock  = XftFontOpenName(dpy, scr, "sans-serif:pixelsize=26");
    if (!f_label)  f_label  = XftFontOpenName(dpy, scr, "sans-serif:pixelsize=13");
    if (!f_pw)     f_pw     = XftFontOpenName(dpy, scr, "sans-serif:pixelsize=18");

    /* dedicated full-screen panel so the lock stays above all other windows
       even while Openbox/compositor keep running underneath */
    {
        XSetWindowAttributes sa;
        memset(&sa, 0, sizeof sa);
        sa.override_redirect = True;
        sa.backing_store = Always;
        sa.cursor = pointer_cursor;
        sa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                        PointerMotionMask | LeaveWindowMask;
        win = XCreateWindow(dpy, root, 0, 0, scr_w, scr_h, 0, CopyFromParent,
                            InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackingStore | CWCursor | CWEventMask,
                            &sa);
        /* NOTIFICATION type + ABOVE state: compositor renders the lock
         * above DOCK panels (taskbar), like Win11's lock screen */
        Atom tprop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
        Atom notif = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
        Atom sprop = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
        XChangeProperty(dpy, win, tprop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&notif, 1);
        XChangeProperty(dpy, win, sprop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&above, 1);
        XMapRaised(dpy, win);
        frame_pm = XCreatePixmap(dpy, win, scr_w, scr_h, DefaultDepth(dpy, scr));
        XFlush(dpy);
    }

    resolve_user_identity();
    load_wallpaper();
    load_default_avatar();
    paint();                                        /* draw before grabbing */

    if (!grab_all()) {
        fprintf(stderr, "lock: failed to grab input\n");
        return 1;
    }

    long long last_caret_ms = 0;
    struct timeval caret_tv;
    gettimeofday(&caret_tv, NULL);
    last_caret_ms = (long long)caret_tv.tv_sec * 1000 + caret_tv.tv_usec / 1000;

    for (;;) {
        /* The only periodic visual change is the input caret. Rendering stays
           in the off-screen pixmap and is copied in one operation, so the
           wallpaper/login page remains stable instead of visibly flashing. */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose && ev.xexpose.window == win) {
                if (ev.xexpose.count == 0) {
                    if (frame_pm)
                        XCopyArea(dpy, frame_pm, win, bgc, 0, 0, scr_w, scr_h, 0, 0);
                    else
                        paint();
                    XFlush(dpy);
                }
                continue;
            }
            if (ev.type == MotionNotify) {
                const int mx = ev.xmotion.x, my = ev.xmotion.y;
                const int inside = mx >= box_x && mx < btn_x - 2 &&
                                   my >= box_y && my < box_y + box_h;
                if (inside != pw_hover) {
                    pw_hover = inside;
                    XDefineCursor(dpy, win, inside ? text_cursor : pointer_cursor);
                    paint();
                }
                continue;
            }
            if (ev.type == LeaveNotify) {
                if (pw_hover) {
                    pw_hover = 0;
                    XDefineCursor(dpy, win, pointer_cursor);
                    paint();
                }
                continue;
            }
            if (ev.type == ButtonPress) {
                int mx = ev.xbutton.x, my = ev.xbutton.y;
                if (mx >= box_x && mx < btn_x - 2 &&
                    my >= box_y && my < box_y + box_h) {
                    pw_focused = 1;
                    caret_visible = 1;
                    XDefineCursor(dpy, win, text_cursor);
                    paint();
                    continue;
                }
                if (mx >= btn_x && mx < btn_x + btn_s &&
                    my >= btn_y && my < btn_y + btn_s) {
                    /* clicked the submit arrow: act like Enter */
                    XKeyEvent fake = ev.xkey;
                    (void)fake;
                    goto do_enter;
                }
                continue;
            }
            if (ev.type != KeyPress) continue;
            pw_focused = 1;
            caret_visible = 1;
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Return) {
            do_enter:
                if (pwlen == 0) {
                    /* allow Enter with an empty password only when the
                     * account has no password set at all */
                    int r0 = check_pw(login_user, "");
                    if (r0 == 1) {
                        release_all();
                        XSync(dpy, False);
                        return 0;
                    }
                    failed = 1;
                    snprintf(msg, sizeof msg, "%s", "请输入密码");
                    paint();
                    continue;
                }
                char pwz[PW_MAX + 1];
                memcpy(pwz, pw, pwlen); pwz[pwlen] = 0;
                int r = check_pw(login_user, pwz);
                if (r == 1) {                        /* unlocked */
                    release_all();
                    XSync(dpy, False);
                    return 0;
                }
                failed = 1;
                snprintf(msg, sizeof msg, "%s",
                         r < 0 ? "用户不存在或不可用" : "密码错误，请重试");
                pwlen = 0; pw[0] = 0;
                paint();
            } else if (ks == XK_BackSpace) {
                if (pwlen > 0) pwlen--;
                paint();
            } else if (ks == XK_Escape) {
                pwlen = 0;
                paint();
            } else if (ks >= XK_space && ks <= XK_asciitilde) {
                char mb[16];
                int k = XLookupString(&ev.xkey, mb, sizeof mb, NULL, NULL);
                if (k > 0 && pwlen + k <= PW_MAX) {
                    memcpy(pw + pwlen, mb, (size_t)k);
                    pwlen += k;
                    paint();
                }
            }
        }
        struct timeval tv = { 0, 120000 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(dpy), &fds);
        select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, &tv);
        gettimeofday(&caret_tv, NULL);
        long long now_ms = (long long)caret_tv.tv_sec * 1000 + caret_tv.tv_usec / 1000;
        if (pw_focused && now_ms - last_caret_ms >= 500) {
            caret_visible = !caret_visible;
            last_caret_ms = now_ms;
            paint();
        }
    }
    return 0;
}