/*
 * ElevenDE 2.0 desktop shell -- C + native Xlib (+ Xft for text).
 *
 * Why C/Xlib: a raw Xlib client has the smallest possible runtime and attack
 * surface (no Python/Qt stack to fail at startup) and the proven input path,
 * which stays reliable on any X11 hardware and driver, VM or bare metal.
 *
 * Layout: one X connection; Openbox is the WM. The shell
 *   - paints the root window (wallpaper + desktop icons),
 *   - owns an override-redirect taskbar (bottom, always on top, clickable),
 *   - owns an override-redirect Start-menu popup.
 */
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif
#ifdef HAVE_XINPUT2
#include <X11/extensions/XInput2.h>
#endif
#include <png.h>
#ifdef HAVE_GDKPIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif
#include <dirent.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* XEMBED protocol v0 message type (avoid hard dependency on
   X11/extensions/Xembed.h, which some distros package differently) */
#ifndef XEMBED_EMBEDDED_NOTIFY
#define XEMBED_EMBEDDED_NOTIFY 0
#endif

#define BAR_H     48
#define MENU_W    600
#define MENU_H    580
#define ICON_W    96
#define ICON_H    104
#define ICON_GX   24
#define ICON_GY   24
#define ICON_CSX  (ICON_W + 14)
#define ICON_CSY  (ICON_H + 10)
#define MAX_TASKS 96
#define MAX_ICONS 96
#define MAX_HOT   96
#define NPIN_MAX 12
#define ROW_H     38
#define LIST_TOP_FILTER (88)
#define LIST_TOP_PINNED (330)
#define LIST_BOTTOM     (494)
#define PIN_COLS     6
#define PIN_CELL     92
#define PIN_TSZ      80
#define PIN_ROWY     100
#define PIN_TOP      104

typedef struct { unsigned long pixel; XftColor xft; } Color;
typedef struct { int x, y, w, h; } RRect;

Display *dpy;
int      scr;
Window   root;
Visual  *vis;
Colormap cmap;
GC       bgc;

XftFont *f_bar, *f_small, *f_tile;
XftDraw *xd_bar, *xd_menu, *xd_desk;

Color cc_bar, cc_lo, cc_hi, cc_text, cc_sub, cc_task, cc_menu,
      cc_accent, cc_search, cc_light, cc_logo1, cc_logo2, cc_logo3, cc_logo4,
      cc_selwash;

static unsigned long gcol[48];
static int           ngcol = 0;

static Window win_bar, win_menu, win_desk;
static int    scr_w, scr_h;

static RRect start_r, search_r, clock_r, edge_r, pill_r, im_r, net_r;
static char  im_label[8] = "ENG";      /* taskbar input-method indicator */
static int   im_ngroups = 1;
static Window win_vol = 0;                /* volume flyout               */
static int    vol_visible = 0, vol_hover = 0, vol_drag = 0;

/* system tray state (refreshed every 2 s) */
static int vol_level = 50, vol_muted = 0;
static int net_type = 0;                  /* 0 none, 1 wifi, 2 wired */
static int bat_present = 0, bat_level = 100, bat_charging = 0;
static time_t last_state_poll = 0;
static time_t wall_mtime = 0;             /* wallpaper file mtime watch  */
static long   wall_mtime_nsec = 0;        /* sub-second replace detection */
static off_t  wall_size = -1;             /* catches atomic same-second swaps */
static time_t desk_mtime = 0;             /* Desktop directory rescan watch */
static long   desk_mtime_nsec = 0;
static off_t  desk_size = -1;
static int    bar_dirty = 0;               /* taskbar needs a repaint     */

typedef struct { Window win; char title[128]; int active; } Task;
static Task tasks[MAX_TASKS];
static int  ntask = 0;
static Window g_active = None;
static KeyCode k_win1 = 0, k_win2 = 0;
static int super_down = 0;   /* Super physically held right now */
static int super_combo = 0;  /* another key was pressed during the hold */
static int    bar_pill_hover = -1;

typedef struct { int x, y; char label[64]; char path[512]; int is_dir; } Icon;
static Icon ic[MAX_ICONS];
static int  nic = 0;

static int    menu_visible = 0;
static int    menu_x = 0, menu_y = 0;
static RRect  menu_hot[MAX_HOT];
static const char *menu_cmd[MAX_HOT];
static int    nmenu_hot = 0;
static Color  pin_colors[NPIN_MAX];

static double last_click = 0.0;
static int    last_click_icon = -1;

static int active_task = -1;
static int bar_hover = -1;
static int pmx = 0, pmy = 0;        /* last pointer pos while over a popup */
static Window menu_open_act = None; /* active win when menu was opened */
static int    cancel_repeat = 0;   /* 兼容旧状态；不再后台反复干预窗口管理器 */
static int    show_desktop_active = 0;

static int desk_dirty = 0;

/* desktop icon drag state */
static int press_active = 0;
static int press_icon = -1;
static int press_x0 = 0, press_y0 = 0;
static int ic_orig_x = 0, ic_orig_y = 0;
static int sel_icon = -1;               /* last single-selected desktop icon */
static int hover_icon = -1;             /* desktop icon under the pointer */
static float sel_anim = 1.0f;           /* 0..1 selection-box scale progress */
static int   sel_anim_act = 0;          /* selection animation still running */

/* app search ------------------------------------------------------------ */
#define NAPP 512
typedef struct { char name[64]; char exec[96]; char icon[64]; } App;
static App apps[NAPP];
static int napps = 0;
static int res_idx[NAPP];
static int nres = 0;
static char menu_filt[64] = "";
static int search_focus = 0;
static int apps_scroll = 0;
static int sel_row = -1;
static int menu_hover_row = -1;
static RRect list_r;
static int  list_vis = 0;
static Color cc_sel, cc_hoverc;
static Pixmap wall_pm = None;
static int   wall_ok = 0;

/* standalone search popup ------------------------------------------------ */
#define SEARCH_W 560
#define SEARCH_H 440
static Window win_search;
static XftDraw *xd_search;
static int search_visible = 0;
static int search_x = 0, search_y = 0;
static int search_hover_row = -1;

/* flat icon kinds -------------------------------------------------------- */
enum {
    FI_FOLDER, FI_FOLDER_WIN, FI_PC, FI_HOME, FI_DRIVE, FI_TERM,
    FI_BROWSER, FI_EDITOR, FI_SETTINGS, FI_SEARCH, FI_POWER, FI_FILE
};

/* power submenu ---------------------------------------------------------- */
static Window win_power;
static XftDraw *xd_power;
static int power_visible = 0;
static RRect power_hot[5];
static int  npower_hot = 0;
static int  power_hover = -1;

/* pinned-tile context menu ---------------------------------------------- */
#define PINM_W 160
#define PINM_H 78
static Window win_pinm;
static XftDraw *xd_pinm;
static int pinm_visible = 0;
static int pinm_x = 0, pinm_y = 0;
static int pinm_pin = -1;          /* pin index the menu refers to */
static int pinm_hover = -1;
static RRect pinm_hot[2];
static int menu_tile_idx = -1;     /* hovered pinned tile (menu_hot index), -1 none */
static int menu_tile_prev = -1;    /* tile being animated OUT (-1 when idle) */
static float tile_anim = 1.0f;     /* 0..1 chip scale progress */
static int tile_anim_act = 0;      /* tile animation still running this frame */
static int menu_power_hover = 0;   /* pointer over the power button */
#ifdef HAVE_XINPUT2
static int xi_opcode = 0;          /* XInput2 extension opcode */
static int xi2_ok = 0;             /* raw button tracking available */
static int phys_btn = 0;           /* real physical button state (grab-proof) */
#endif

/* generic right-click context menu ------------------------------------ */
#define CTX_W  200
#define CTX_ROW 36
#define CTX_MAX 8
static Window    win_ctx;
static XftDraw  *xd_ctx;
static int      ctx_visible = 0, ctx_x = 0, ctx_y = 0;
static int      ctx_n = 0, ctx_h = 0, ctx_hover = -1;
static RRect    ctx_hot[CTX_MAX];
static const char *ctx_items[CTX_MAX];
static void    (*ctx_cb)(int idx, void *ud) = NULL;
static void    *ctx_ud = NULL;

/* desktop context menu ------------------------------------------------ */
#define DM_W 190
#define DM_ROW 34
#define DM_MAX 6
static Window win_dm;
static XftDraw *xd_dm;
static int dm_visible = 0, dm_x = 0, dm_y = 0;
static int dm_hover = -1, dm_n = 0, dm_h = 0;
static int dm_icon = -1;           /* icon index right-clicked (-1 = empty) */
static RRect dm_hot[DM_MAX];

/* system tray ------------------------------------------------------------ */
#define MAX_TRAY   8
#define TRAY_SLOT 26
static Window win_tray;
static XftDraw *xd_tray;
static XftDraw *xd_vol;
static Window tray_wins[MAX_TRAY];
static int    ntray = 0;
static int    tray_ok = 0;

/* clock calendar popup --------------------------------------------------- */
#define CAL_W 240
#define CAL_H 230
static Window win_cal;
static XftDraw *xd_cal;
static int cal_visible = 0;
static int cal_off = 0;
static RRect cal_prev, cal_next, cal_days[42];

/* prototypes so usage order does not matter */
static double now_sec(void);
static void   die(const char *m);
static Atom   atom(const char *n);
static void   set_prop_c32(Window w, const char *n, long *v, int cnt);
static void   close_task(Window w);
static void   wm_break_stuck(void);
static void   wm_cancel_move(void);
static void   wm_killmove(void);
static void   wm_ghost_watch(void);
static Color  make_color(const char *n);
static Window mk_owindow(int x, int y, int w, int h);
static void   fill_round(Drawable dr, GC g, int x, int y, int w, int h, int r, unsigned long px);
static void   fill(Drawable dr, GC g, int x, int y, int w, int h, unsigned long px);
static int    text_w(XftFont *f, const char *s);
static void   draw_str(Drawable dr, XftDraw *xd, XftFont *f, Color *c, int x, int y, const char *s);
static void   draw_str_c(Drawable dr, XftDraw *xd, XftFont *f, Color *c, int cx, int cy, int w, int h, const char *s);
static void   win_logo(Drawable dr, int x, int y, int size);
static void   activate_window(Window w);
static void   win_title(Window w, char *buf, int nbuf);
static int    refresh_tasks(void);
static Pixmap icon_for_png(const char *name, int size, const Color *bg);
static Pixmap icon_for_exact(const char *name, int size, const Color *bg);
static Pixmap icon_for_task(Window w, int size);
static void   win_class(Window w, char *buf, int nbuf);
static const char *app_icon_name(const char *cls);
static void   draw_icon(Drawable dr, Pixmap pm, int size, int cx, int cy,
                        int boxw, int boxh);
static void   load_apps(void);
static void   filter_apps(void);
static void   exec_strip(char *dst, const char *src);
static void   launch_app(int idx);
static void   menu_filter_changed(void);
static void   show_power(void);
static void   draw_power(void);
static void   handle_power_press(int x, int y);
static void   pinmenu_show(int pin, int ax, int ay);
static void   pinmenu_hide(void);
static void   draw_pinmenu(void);
static void   handle_pinmenu_press(int x, int y);
static void   ctx_show(const char *const items[], int n, int ax, int ay,
                       void (*cb)(int idx, void *ud), void *ud);
static void   ctx_hide(void);
static void   draw_ctx(void);
static void   handle_ctx_press(int x, int y);
static void   toggle_maximize(Window w);
static void   task_ctx_cb(int idx, void *ud);
static void   applist_ctx_cb(int idx, void *ud);
static void   dm_show(int ax, int ay, int icon);
static void   dm_hide(void);
static void   draw_dm(void);
static void   handle_dm_press(int x, int y);
static void   handle_menu_key(const XKeyEvent *e);
static void   handle_desk_motion(int x, int y);
static int    in_rect(RRect r, int x, int y);
static void   handle_desk_release(int x, int y);
static int    bar_task_at(int x, int y);
static void   tray_init(void);
static void   tray_dock(Window w);
static void   tray_remove(Window w);
static void   tray_layout(void);
static void   draw_tray(void);
static void   cal_toggle(void);
static void   cal_hide(void);
static void   draw_cal(void);
static void   handle_cal_press(int x, int y);
static void   draw_taskbar(void);
static void   sys_state_update(void);
static void   im_state_init(void);
static void   im_state_update(void);
static void   im_click(void);
static void   draw_vol_flyout(void);
static void   vol_show(void);
static void   vol_hide(void);
static void   handle_vol_press(int x, int y);
static void   handle_vol_motion(int x, int y);
static void   set_volume_pct(int pct);
static void   toggle_mute(void);
static void   paint_desktop(void);
static void   icon_layout(void);
static void   gen_icons(void);
static const char *file_icon_name(const char *name);
static void   launch_cmd(const char *cmd);
static void   open_path(const char *path);
static void   menu_hide(void);
static void menu_show(void);
static void menu_refresh(void);
static void   draw_menu(void);
static int    in_rect(RRect r, int x, int y);
static void   handle_bar_press(int but, int x, int y);
static void   handle_menu_press(int but, int x, int y);
static void   handle_root_press(int but, int x, int y);
static void   search_show(void);
static void   search_hide(void);
static void   draw_search(void);
static void   handle_search_press(int x, int y);
static void   handle_search_key(const XKeyEvent *ev);

/* ------------------------------------------------------------------ utils */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void die(const char *m) {
    fprintf(stderr, "elevende-shell: %s\n", m);
    exit(1);
}

static Atom atom(const char *n) { return XInternAtom(dpy, n, False); }

static void __attribute__((unused)) set_prop_c32(Window w, const char *n, long *v, int cnt) {
    XChangeProperty(dpy, w, atom(n), XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)v, cnt);
}

static Color make_color(const char *n) {
    Color c;
    if (!XftColorAllocName(dpy, vis, cmap, n, &c.xft)) {
        fprintf(stderr, "elevende-shell: color %s failed\n", n);
        c.pixel = BlackPixel(dpy, scr);
        memset(&c.xft, 0, sizeof c.xft);
        return c;
    }
    c.pixel = c.xft.pixel;
    return c;
}

static Window mk_owindow(int x, int y, int w, int h) {
    XSetWindowAttributes sa;
    memset(&sa, 0, sizeof sa);
    sa.override_redirect = True;
    sa.background_pixel = cc_bar.pixel;
    sa.backing_store = Always;     /* server repaints hidden parts -> no tear */
    sa.event_mask = ExposureMask | ButtonPressMask | KeyPressMask |
                    PointerMotionMask | EnterWindowMask | LeaveWindowMask;
    Window win = XCreateWindow(dpy, root, x, y, w, h, 0, CopyFromParent,
                               InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel |
                               CWBackingStore | CWEventMask, &sa);
    return win;
}

static void fill_round(Drawable dr, GC g, int x, int y, int w, int h, int r,
                       unsigned long px) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int rr = r * 2;
    XSetForeground(dpy, g, px);
    XFillRectangle(dpy, dr, g, x + r, y, w - rr, h);
    XFillRectangle(dpy, dr, g, x, y + r, w, h - rr);
    XFillArc(dpy, dr, g, x, y, rr, rr, 90 * 64, 90 * 64);
    XFillArc(dpy, dr, g, x + w - rr, y, rr, rr, 0, 90 * 64);
    XFillArc(dpy, dr, g, x, y + h - rr, rr, rr, 180 * 64, 90 * 64);
    XFillArc(dpy, dr, g, x + w - rr, y + h - rr, rr, rr, 270 * 64, 90 * 64);
}

static void fill(Drawable dr, GC g, int x, int y, int w, int h, unsigned long px) {
    XSetForeground(dpy, g, px);
    XFillRectangle(dpy, dr, g, x, y, w, h);
}

static XftFont *cjk_font(XftFont *base);

/* Truncate a UTF-8 label so it fits maxw pixels, appending an ellipsis.
 * Character-boundary aware (desktop labels are often CJK). */
/* Translucent rounded wash blended over the wallpaper pixels -- this is how
 * Win11 selection/hover rectangles look (tinted glass, not opaque blocks).
 * alpha is 0..256. Requires the wallpaper pixmap; falls back to a solid
 * fill when there is none. */
static void desk_tint(int x, int y, int w, int h, unsigned long col, int alpha,
                      int radius) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h - BAR_H) h = scr_h - BAR_H - y;
    if (w <= 0 || h <= 0) return;
    if (!wall_ok || !wall_pm) {
        fill_round(win_desk, bgc, x, y, w, h, radius, col);
        return;
    }
    XImage *im = XGetImage(dpy, wall_pm, x, y, (unsigned)w, (unsigned)h,
                           AllPlanes, ZPixmap);
    if (!im) {
        fill_round(win_desk, bgc, x, y, w, h, radius, col);
        return;
    }
    const int cr = (col >> 16) & 0xFF, cg = (col >> 8) & 0xFF, cb = col & 0xFF;
    for (int yy = 0; yy < h; yy++) {
        /* rounded-corner mask */
        int inrow = 1;
        if (yy < radius || yy >= h - radius) {
            inrow = 0;   /* corners handled per-pixel below */
        }
        for (int xx = 0; xx < w; xx++) {
            if (!inrow) {
                int dx = 0, dy = 0;
                if (yy < radius && xx < radius) { dx = radius - xx; dy = radius - yy; }
                else if (yy < radius && xx >= w - radius) { dx = xx - (w - radius - 1); dy = radius - yy; }
                else if (yy >= h - radius && xx < radius) { dx = radius - xx; dy = yy - (h - radius - 1); }
                else if (yy >= h - radius && xx >= w - radius) { dx = xx - (w - radius - 1); dy = yy - (h - radius - 1); }
                else { /* inside flat band */ }
                if (dx * dx + dy * dy > radius * radius) continue;
            }
            unsigned long px = XGetPixel(im, xx, yy);
            int pr = (px >> 16) & 0xFF, pg = (px >> 8) & 0xFF, pb = px & 0xFF;
            int nr = pr + (cr - pr) * alpha / 256;
            int ng = pg + (cg - pg) * alpha / 256;
            int nb = pb + (cb - pb) * alpha / 256;
            XPutPixel(im, xx, yy, (unsigned long)((nr << 16) | (ng << 8) | nb));
        }
    }
    XPutImage(dpy, win_desk, bgc, im, 0, 0, x, y, (unsigned)w, (unsigned)h);
    XDestroyImage(im);
}

static void fit_label(char *dst, int ndst, const char *s, XftFont *f, int maxw) {
    if (text_w(f, s) <= maxw) {
        snprintf(dst, (size_t)ndst, "%s", s);
        return;
    }
    static const char dot[] = "\xE2\x80\xA6";   /* ellipsis */
    const size_t len = strlen(s);
    char tmp[96];
    size_t best = 0, i = 0;
    while (i < len) {
        unsigned char b0 = (unsigned char)s[i];
        size_t clen = 1;
        if      ((b0 & 0xE0) == 0xC0) clen = 2;
        else if ((b0 & 0xF0) == 0xE0) clen = 3;
        else if ((b0 & 0xF8) == 0xF0) clen = 4;
        i += clen;
        if (i > sizeof tmp - 8) break;
        memcpy(tmp, s, i);
        memcpy(tmp + i, dot, 3);
        tmp[i + 3] = 0;
        if (text_w(f, tmp) <= maxw) best = i;
        else break;
    }
    if (best == 0 || best + 4 > (size_t)ndst) {
        snprintf(dst, (size_t)ndst, "%s", dot);
        return;
    }
    memcpy(dst, s, best);
    memcpy(dst + best, dot, 3);
    dst[best + 3] = 0;
}

static int text_w(XftFont *f, const char *s) {
    XftFont *cf = cjk_font(f);
    if (!cf) {                          /* no CJK font: measure as-is */
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, (int)strlen(s), &ext);
        return ext.xOff;
    }
    /* same per-glyph run split as draw_str(), so CJK glyphs are measured
     * with the CJK face and centred text really lines up under its icon */
    const unsigned char *p = (const unsigned char *)s;
    int pen = 0;
    while (*p) {
        unsigned char b0 = *p;
        FcChar32 cp;
        unsigned clen;
        if (b0 < 0x80)                 { cp = b0;        clen = 1; }
        else if ((b0 & 0xE0) == 0xC0)  { cp = b0 & 0x1F; clen = 2; }
        else if ((b0 & 0xF0) == 0xE0)  { cp = b0 & 0x0F; clen = 3; }
        else if ((b0 & 0xF8) == 0xF0)  { cp = b0 & 0x07; clen = 4; }
        else                           { cp = b0;        clen = 1; }
        for (unsigned k = 1; k < clen; k++) cp = (cp << 6) | (p[k] & 0x3F);
        int use_cjk = !XftCharExists(dpy, f, cp);
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
            if ((!XftCharExists(dpy, f, c2)) != use_cjk) break;
            q += cc;
        }
        int rlen = (int)(q - p);
        XftFont *tf = use_cjk ? cf : f;
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, tf, (const FcChar8 *)p, rlen, &ext);
        pen += ext.xOff;
        p = q;
    }
    return pen;
}

/* ---- CJK-aware text --------------------------------------------------
 * XftFontOpenName returns exactly ONE font; "sans-serif" therefore never
 * falls back to Noto for CJK codepoints and Chinese renders as boxes.
 * cjk_font() opens a CJK face sized to match `base`, and draw_str() walks
 * the UTF-8 string, using the CJK font only for glyphs the base lacks.  */
static XftFont *cjk_font(XftFont *base) {
    static XftFont *base_ck[8], *cjk_ck[8];
    static int n = 0;
    for (int i = 0; i < n; i++)
        if (base_ck[i] == base) return cjk_ck[i];
    if (n >= 8) return NULL;
    int px = (int)((base->ascent + base->descent) / 1.2f + 0.5f);
    if (px < 6) px = 6;
    if (px > 96) px = 96;
    XftFont *cf = NULL;
    char pat[128];
    static const char *families[] = { "Noto Sans CJK SC", "WenQuanYi Zen Hei",
                                      "AR PL UMing CN" };
    for (size_t i = 0; i < sizeof families / sizeof families[0] && !cf; i++) {
        snprintf(pat, sizeof pat, "%s:pixelsize=%d", families[i], px);
        cf = XftFontOpenName(dpy, scr, pat);
    }
    base_ck[n] = base;
    cjk_ck[n] = cf;
    n++;
    return cf;
}

static void draw_str(Drawable dr, XftDraw *xd, XftFont *f, Color *c,
                     int x, int y, const char *s) {
    (void)dr;
    XftFont *cf = cjk_font(f);
    if (!cf) {                      /* no CJK font at all: draw as-is */
        XftDrawStringUtf8(xd, &c->xft, f, x, y,
                          (const FcChar8 *)s, (int)strlen(s));
        return;
    }
    const unsigned char *p = (const unsigned char *)s;
    int pen = 0;
    while (*p) {
        unsigned char b0 = *p;
        FcChar32 cp;
        unsigned clen;
        if (b0 < 0x80)                 { cp = b0;        clen = 1; }
        else if ((b0 & 0xE0) == 0xC0)  { cp = b0 & 0x1F; clen = 2; }
        else if ((b0 & 0xF0) == 0xE0)  { cp = b0 & 0x0F; clen = 3; }
        else if ((b0 & 0xF8) == 0xF0)  { cp = b0 & 0x07; clen = 4; }
        else                           { cp = b0;        clen = 1; }
        for (unsigned k = 1; k < clen; k++) cp = (cp << 6) | (p[k] & 0x3F);
        int use_cjk = !XftCharExists(dpy, f, cp);
        /* extend the run while the same font serves the next glyph */
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
            if ((!XftCharExists(dpy, f, c2)) != use_cjk) break;
            q += cc;
        }
        int rlen = (int)(q - p);
        XftFont *tf = use_cjk ? cf : f;
        XftDrawStringUtf8(xd, &c->xft, tf, x + pen, y, (const FcChar8 *)p,
                          rlen);
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, tf, (const FcChar8 *)p, rlen, &ext);
        pen += ext.xOff;
        p = q;
    }
}

static void draw_str_c(Drawable dr, XftDraw *xd, XftFont *f, Color *c,
                       int cx, int cy, int w, int h, const char *s) {
    int tw = text_w(f, s);
    int x = cx + (w - tw) / 2;
    int y = cy + (h - (f->ascent + f->descent)) / 2 + f->ascent;
    draw_str(dr, xd, f, c, x, y, s);
}

static void win_logo(Drawable dr, int x, int y, int size) {
    int cw = size / 2, ch = size / 2;
    unsigned long cols[4] = { cc_logo1.pixel, cc_logo2.pixel,
                              cc_logo3.pixel, cc_logo4.pixel };
    int xo[4] = { 0, cw, 0, cw }, yo[4] = { 0, 0, ch, ch };
    for (int i = 0; i < 4; i++)
        fill_round(dr, bgc, x + xo[i], y + yo[i], cw, ch, 1, cols[i]);
}

/* ---- self-drawn flat icons ------------------------------------------
 * Guaranteed fallback so tiles never degrade to the gnome "?"/unknown-icon
 * or to a bare letter: simple monochrome silhouettes drawn with X primitives.
 * kind indexes are also used by the desktop icons below. */
enum { VI_FOLDER=0, VI_PC, VI_HOME, VI_DRIVE, VI_TERM, VI_BROWSER,
       VI_EDITOR, VI_SETTINGS, VI_FILE };

static void draw_icon_kind(Drawable dr, int kind, int cx, int cy, int s,
                           unsigned long fg) {
    XSetForeground(dpy, bgc, fg);
    int u = s / 32; if (u < 1) u = 1;
    int x = cx - 16 * u, y = cy - 16 * u, w = 32 * u, h = 32 * u;
    int th = u * 2; if (th < 2) th = 2;
    switch (kind) {
    case VI_FOLDER: {                              /* open folder (Win11 look) */
        int tabw = 9 * u, tabh = 5 * u;
        fill(dr, bgc, x, y + tabh, tabw, tabh, fg);      /* tab */
        fill(dr, bgc, x + tabw, y, w - tabw, tabh + 2, fg);
        fill(dr, bgc, x, y + tabh, w, h - tabh, fg);     /* body */
        XSetForeground(dpy, bgc, 0x000000);              /* crease shadow */
        fill(dr, bgc, x + 4 * u, y + tabh + 4 * u, w - 8 * u, th, 0x000000);
        fill(dr, bgc, x + 6 * u, y + h - 5 * u, w - 12 * u, th, 0x000000);
        XSetForeground(dpy, bgc, fg);
        break;
    }
    case VI_PC: {                                    /* monitor + stand + screen */
        fill_round(dr, bgc, x + 2 * u, y, w - 4 * u, h - 6 * u, 2 * u, fg);
        fill(dr, bgc, cx - u, y + h - 6 * u, 2 * u, 3 * u, fg);
        fill(dr, bgc, cx - 5 * u, y + h - 3 * u, 10 * u, 2 * u, fg);
        XSetForeground(dpy, bgc, 0x000000);              /* recessed screen */
        fill(dr, bgc, x + 6 * u, y + 4 * u, w - 12 * u, h - 14 * u, 0);
        XSetForeground(dpy, bgc, fg);
        break;
    }
    case VI_HOME: {                                /* house */
        fill(dr, bgc, x + w / 2 - u, y, 2 * u, h * 5 / 12, fg);
        fill(dr, bgc, x, y + h * 5 / 12 - 2 * u, w, 4 * u, fg);
        fill(dr, bgc, x + 3 * u, y + h * 5 / 12 + 2 * u, w - 6 * u,
             h - h * 5 / 12 - 2 * u, fg);
        break;
    }
    case VI_DRIVE: {                               /* hard disk */
        fill_round(dr, bgc, x + 2 * u, y + 6 * u, w - 4 * u, h - 12 * u,
                   3 * u, fg);
        fill(dr, bgc, x + 6 * u, y + 12 * u, w - 12 * u, th, fg); /* platter */
        break;
    }
    case VI_TERM:                                  /* terminal window */
        fill_round(dr, bgc, x + u, y, w - 2 * u, h - 4 * u, 2 * u, fg);
        XSetForeground(dpy, bgc, 0x0);             /* hollow look via bg */
#if 0
        XDrawLine(dpy, dr, bgc, x + 3*u, y + 5*u, x + w - 3*u, y + 5*u);
        XDrawLine(dpy, dr, bgc, x + 6*u, y + 12*u, x + 10*u, y + 8*u);
        XDrawLine(dpy, dr, bgc, x + 10*u, y + 8*u, x + 6*u, y + 4*u);
#endif
        break;
    case VI_BROWSER: {                             /* globe */
        XSetLineAttributes(dpy, bgc, th, LineSolid, CapButt, JoinMiter);
        XDrawArc(dpy, dr, bgc, x + 2*u, y + 2*u, w - 4*u, h - 4*u,
                 0 * 64, 360 * 64);
        XDrawLine(dpy, dr, bgc, cx, y + 2*u, cx, y + h - 2*u);
        XDrawArc(dpy, dr, bgc, x + 2*u, y + h/2 - (h-4*u)/2, w - 4*u, h - 4*u,
                 0 * 64, 180 * 64);
        XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
        break;
    }
    case VI_EDITOR: {                              /* document + lines */
        fill_round(dr, bgc, x + 4*u, y, w - 8*u, h - 4*u, 3*u, fg);
        XDrawLine(dpy, dr, bgc, x + w/2 - 8*u, y + 12*u, x + w/2 + 4*u,
                  y + 12*u);
        XDrawLine(dpy, dr, bgc, x + w/2 - 8*u, y + 16*u, x + w/2 + 4*u,
                  y + 16*u);
        XDrawLine(dpy, dr, bgc, x + w/2 - 8*u, y + 20*u, x + w/2 + 2*u,
                  y + 20*u);
        XSetLineAttributes(dpy, bgc, th, LineSolid, CapButt, JoinMiter);
        XDrawLine(dpy, dr, bgc, x + w - 9*u, y + 3*u, x + w - 9*u, y + 8*u);
        XDrawLine(dpy, dr, bgc, x + w - 9*u, y + 8*u, x + w - 4*u, y + 8*u);
        XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
        break;
    }
    case VI_SETTINGS: {                            /* three sliders */
        for (int i = 0; i < 3; i++) {
            int ly = cy - 10*u + i * 10*u;
            fill(dr, bgc, x + 3*u, ly, w - 6*u, 2*u, fg);
            int kx = x + 5*u + (i == 0 ? 0 : i == 1 ? 11*u : 6*u) * 1;
            fill_round(dr, bgc, kx, ly - 3*u, 6*u, 8*u, 3*u, fg);
        }
        break;
    }
    case VI_FILE: {                                /* generic file */
        fill_round(dr, bgc, x + 4*u, y + 2*u, w - 8*u, h - 6*u, 3*u, fg);
        XSetForeground(dpy, bgc, 0);
        XDrawLine(dpy, dr, bgc, x + 8*u, y + 12*u, x + w - 8*u, y + 12*u);
        XDrawLine(dpy, dr, bgc, x + 8*u, y + 16*u, x + w - 8*u, y + 16*u);
        XSetForeground(dpy, bgc, fg);
        break;
    }
    default: break;
    }
    XFlush(dpy);
}

/* power symbol: ring with a vertical plug line, used in the Start menu */
static void draw_power_symbol(Drawable dr, int cx, int cy, int s,
                              unsigned long fg) {
    int th = s / 7; if (th < 2) th = 2;
    XSetForeground(dpy, bgc, fg);
    XSetLineAttributes(dpy, bgc, th, LineSolid, CapRound, JoinRound);
    int r = s / 2 - th;
    XDrawArc(dpy, dr, bgc, cx - r, cy - r, 2 * r, 2 * r, 130 * 64, 280 * 64);
    XDrawLine(dpy, dr, bgc, cx, cy - r - th, cx, cy + r / 3);
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
    XFlush(dpy);
}

/* ---------------------------------------------------------------- windows */
static void set_wm_state(Window w) {
    Atom s = atom("_NET_WM_STATE");
    Atom a[3] = { atom("_NET_WM_STATE_ABOVE"),
                  atom("_NET_WM_STATE_SKIP_TASKBAR"),
                  atom("_NET_WM_STATE_SKIP_PAGER") };
    XChangeProperty(dpy, w, s, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)a, 3);
    /* announce the taskbar as a dock and reserve the bottom strip, the
     * EWMH panel convention every DE (KDE/GNOME/xfce) uses via struts */
    Atom typ = atom("_NET_WM_WINDOW_TYPE_DOCK");
    XChangeProperty(dpy, w, atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&typ, 1);
    long strut[12] = { 0, 0, 0, BAR_H,
                       0, 0, 0, 0,
                       0, 0, 0, scr_w > 0 ? scr_w - 1 : 0 };
    XChangeProperty(dpy, w, atom("_NET_WM_STRUT_PARTIAL"), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 12);
}

static void close_task(Window w) {
    Atom wm_delete = atom("WM_DELETE_WINDOW");
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = atom("WM_PROTOCOLS");
    e.xclient.format = 32;
    e.xclient.data.l[0] = (long)wm_delete;
    e.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, w, False, NoEventMask, &e);
    wm_cancel_move();
    cancel_repeat = 15;            /* extend guard to ~2.2s for ghosts */
    XFlush(dpy);
}

static void activate_window(Window w) {
    wm_cancel_move();             /* break any stuck openbox move-grab first */
    cancel_repeat = 14;            /* keep it broken while the window rises */
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = atom("_NET_ACTIVE_WINDOW");
    e.xclient.format = 32;
    e.xclient.data.l[0] = 2;
    e.xclient.data.l[1] = CurrentTime;
    e.xclient.data.l[2] = 0;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask,
               &e);
    XFlush(dpy);
}

/* toggle maximized via EWMH; also nudges openbox out of any stuck move-grab */
static void toggle_maximize(Window w) {
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = atom("_NET_WM_STATE");
    e.xclient.format = 32;
    e.xclient.data.l[0] = 2;                         /* _NET_WM_STATE_TOGGLE */
    e.xclient.data.l[1] = (long)atom("_NET_WM_STATE_MAXIMIZED_VERT");
    e.xclient.data.l[2] = (long)atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    e.xclient.data.l[3] = 1;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask,
               &e);
    XFlush(dpy);
}

/* Grab-proof physical button state. XQueryPointer() reports the button as
 * DOWN while a WM pointer-grab (real move OR ghost move) is active, so it
 * cannot tell a genuine drag from a stuck "phantom" move. Raw XInput2 events
 * bypass grabs and report the true physical buttons; fall back to the core
 * query when XI2 is unavailable. */
static int __attribute__((unused)) buttons_down(void) {
#ifdef HAVE_XINPUT2
    if (xi2_ok) return phys_btn;
#endif
    Window cr, kid; int cx, cy, rx, ry; unsigned int bm = 0;
    XQueryPointer(dpy, root, &cr, &kid, &rx, &ry, &cx, &cy, &bm);
    return (bm & (Button1Mask | Button2Mask | Button3Mask)) != 0;
}

/* ---- Super key via XkbStateNotify (grab-free) --------------------------
 * Grabbing Super_L/Super_R on root (the old approach) silently broke every
 * Openbox Win+<key> binding: once another client grabs the Super keycode,
 * Openbox's W-* passive grabs stop firing. Instead we subscribe to
 * XkbStateNotify and watch the Mod4 bit -- no grab, no conflict, and XI2
 * raw key events still tell us whether Super was pressed alone. */
static int  xkb_ok = 0;
static int  xkb_event_base = 0;

static void xkb_init(void) {
    int opcode, ev, err;
    int maj = XkbMajorVersion, min = XkbMinorVersion;
    if (!XkbQueryExtension(dpy, &opcode, &ev, &err, &maj, &min)) return;
    if (!XkbSelectEvents(dpy, XkbUseCoreKbd, XkbStateNotifyMask,
                         XkbStateNotifyMask))
        return;
    xkb_event_base = ev;
    xkb_ok = 1;
}

static void xi2_init(void) {
#ifdef HAVE_XINPUT2
    int ev, err;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &err)) return;
    /* XI2 event masks are bit arrays: bit N lives in byte N/8. The old
     * code stuffed (1<<15)|(1<<16) into a single unsigned char, which
     * truncated to 0 and silently disabled raw-button tracking (ghost
     * drag recovery never fired). */
    unsigned char mask[4] = { 0, 0, 0, 0 };
    mask[XI_RawButtonPress   / 8] |= 1 << (XI_RawButtonPress   % 8);
    mask[XI_RawButtonRelease / 8] |= 1 << (XI_RawButtonRelease % 8);
    /* raw key events: grab-proof tracking so we can tell whether Super was
     * pressed ALONE (-> Start menu) or as part of a combo like Win+E */
    mask[XI_RawKeyPress      / 8] |= 1 << (XI_RawKeyPress      % 8);
    mask[XI_RawKeyRelease    / 8] |= 1 << (XI_RawKeyRelease    % 8);
    /* XIAllMasterDevices: master (logical) devices report paired press/release
     * events; raw slave-device events can arrive unpaired and would wedge
     * phys_btn permanently "down", blinding the ghost-move detector. */
    XIEventMask evm = { .deviceid = XIAllMasterDevices, .mask_len = 4, .mask = mask };
    if (XISelectEvents(dpy, root, &evm, 1) == Success) xi2_ok = 1;
#endif
}

/* ---- physical vs server button state ------------------------------------
 * Ghost moves start from a "phantom press": the server's core button state
 * stays DOWN after a real or synthetic release was dropped. Two views:
 *   core_btn_mask()  -- what the server believes (lies during a phantom)
 *   phys_pressed()   -- XI2 raw events, i.e. what is physically held */
static unsigned core_btn_mask(void) {
    Window cr, kid; int cx, cy, rx, ry; unsigned int bm = 0;
    XQueryPointer(dpy, root, &cr, &kid, &rx, &ry, &cx, &cy, &bm);
    return bm & (Button1Mask | Button2Mask | Button3Mask);
}

static int phys_pressed(void) {
#ifdef HAVE_XINPUT2
    if (xi2_ok) return phys_btn;
#endif
    return core_btn_mask() != 0;
}

/* Ask the WM to abort any interactive Move/Resize in progress. A dropped
 * button-release can leave openbox's move-grab live so the
 * next window "follows the mouse". The EWMH cancel alone only ends
 * message-started moves; the definitive kill (fake Escape) is only safe
 * once a stuck move is CONFIRMED by the sliding-window detector, because
 * then openbox is physically holding its keyboard grab. */
static void wm_cancel_move(void) {
    /* 保留调用点以兼容任务栏代码，但禁止 Shell 向窗口管理器发送任何
       move/resize 取消事件。真实拖动、标题栏按钮和应用点击必须完全由
       Openbox 与客户端处理。 */
}

/* Confirmed stuck-move killer. A synthetic button release ends the WM's
 * move/resize pointer grab at its root cause (the grab ends on the release
 * of the button that started it); the EWMH cancel additionally covers
 * message-started moves. Sending a release for a button nobody physically
 * holds is a no-op for legitimate drags, so this is strictly safe. */
static void __attribute__((unused)) wm_killmove(void) {
    /* 已废弃：不得再注入合成按钮释放或改变窗口管理器的交互状态。 */
}

/* A client window was just destroyed (closed). If openbox is mid
 * interactive move/resize -- e.g. the dragged window got closed via Alt+F4,
 * or a phantom button press is about to latch onto the window now sitting
 * under the pointer -- cancel the operation and purge stuck button state so
 * the NEXT window the user touches is not dragged along. When XI2 raw
 * tracking is live we only synthesize releases if no button is physically
 * held, so a genuine drag is never disturbed. */
static void __attribute__((unused)) wm_on_window_destroyed(void) {
    /* 窗口映射、关闭和重新布局不再触碰鼠标状态。 */
}

/* True while the pointer rests over one of our own (unmanaged) windows.
 * Used by the raw-button watcher to decide whether a physical press is
 * "inside our UI" (let our widgets handle it) or "outside" (close the
 * Start menu / search / flyouts -- Win11 behavior, implemented WITHOUT
 * holding any pointer grab). */
static int pointer_over_own_ui(void) {
    Window cr, kid; int cx, cy, rx, ry; unsigned int bm;
    if (!XQueryPointer(dpy, root, &cr, &kid, &rx, &ry, &cx, &cy, &bm))
        return 1;                            /* assume ours: don't close */
    return kid == win_bar || kid == win_desk || kid == win_menu ||
           kid == win_power || kid == win_pinm || kid == win_ctx ||
           kid == win_search || kid == win_cal || kid == win_vol;
}

static void __attribute__((unused)) wm_break_stuck(void) {
    /* 已废弃：Shell 不得抓取指针、伪造释放或清除 Openbox 的输入状态。 */
}

/* ---- autonomous ghost-move defense (3.1 rewrite) ------------------------
 * A "ghost move" is openbox dragging a window on a button nobody is
 * physically holding (a dropped release after a window was closed, or a
 * phantom press left by a driver). Symptoms: after opening/closing a
 * window, ANOTHER window follows the mouse.
 *
 * Defense runs at ~10 Hz, independent of any guard window:
 *   1. Phantom purge -- if the server believes a button is down while
 *      nothing is physically held, synthesize the missing release(s) and
 *      send _NET_WM_MOVERESIZE_CANCEL. This heals the root cause before it
 *      can latch onto the next window that appears under the pointer.
 *   2. Sliding detector -- geometry is tracked per WINDOW ID (task slots
 *      get reordered whenever the task list changes, so slot-indexed
 *      tracking produced garbage right at open/close, when ghosts bite).
 *      A window that moves/resizes for 2 consecutive samples (~200 ms)
 *      while nothing is physically held is a confirmed ghost: kill it.
 *      One-shot geometry changes (openbox placement on map, Win+arrow
 *      snaps, maximize) produce a single sample and are never touched. */

#define WM_TRACK_MAX 64
static Window wm_tw[WM_TRACK_MAX];
static int    wm_tx[WM_TRACK_MAX], wm_ty[WM_TRACK_MAX];
static int    wm_twd[WM_TRACK_MAX], wm_tht[WM_TRACK_MAX];
static int    wm_tslide[WM_TRACK_MAX];      /* consecutive sliding samples  */
static int    wm_ntw = 0;

static void __attribute__((unused)) wm_ghost_watch(void) {
    static struct timespec wm_last;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = (long)(ts.tv_sec - wm_last.tv_sec) * 1000 +
              (ts.tv_nsec - wm_last.tv_nsec) / 1000000;
    if (ms < 100) return;                   /* ~10 Hz */
    wm_last = ts;

    const int phys = phys_pressed();

    /* 1. phantom purge */
    if (!phys && core_btn_mask()) {
        fprintf(stderr, "[elevende] ghost defense: phantom purge (core=%x)\n",
                core_btn_mask());
        wm_killmove();
    }

    /* 2. sliding detector */
    Window seen[WM_TRACK_MAX];
    int ns = 0, ghost = 0;
    for (int i = 0; i < ntask && ns < WM_TRACK_MAX; i++) {
        Window w = tasks[i].win, ch, rr;
        int x = 0, y = 0, gx = 0, gy = 0;
        unsigned wd = 0, ht = 0, bw = 0, depth = 0;
        if (!XTranslateCoordinates(dpy, w, root, 0, 0, &x, &y, &ch))
            continue;
        if (!XGetGeometry(dpy, w, &rr, &gx, &gy, &wd, &ht, &bw, &depth))
            continue;
        seen[ns++] = w;

        int k = -1;
        for (int j = 0; j < wm_ntw; j++)
            if (wm_tw[j] == w) { k = j; break; }
        if (k < 0) {                        /* first sight: baseline only */
            if (wm_ntw < WM_TRACK_MAX) {
                k = wm_ntw++;
            } else {                        /* full: evict slot 0 (oldest-ish) */
                memmove(&wm_tw[0],    &wm_tw[1],    (WM_TRACK_MAX-1)*sizeof wm_tw[0]);
                memmove(&wm_tx[0],    &wm_tx[1],    (WM_TRACK_MAX-1)*sizeof wm_tx[0]);
                memmove(&wm_ty[0],    &wm_ty[1],    (WM_TRACK_MAX-1)*sizeof wm_ty[0]);
                memmove(&wm_twd[0],   &wm_twd[1],   (WM_TRACK_MAX-1)*sizeof wm_twd[0]);
                memmove(&wm_tht[0],   &wm_tht[1],   (WM_TRACK_MAX-1)*sizeof wm_tht[0]);
                memmove(&wm_tslide[0],&wm_tslide[1],(WM_TRACK_MAX-1)*sizeof wm_tslide[0]);
                k = WM_TRACK_MAX - 1;
            }
            wm_tw[k] = w;
            wm_tx[k] = x; wm_ty[k] = y; wm_twd[k] = (int)wd; wm_tht[k] = (int)ht;
            wm_tslide[k] = 0;
            continue;
        }
        if (phys) {                          /* genuine drag: baseline only */
            wm_tx[k] = x; wm_ty[k] = y; wm_twd[k] = (int)wd; wm_tht[k] = (int)ht;
            wm_tslide[k] = 0;
            continue;
        }
        const int moved = abs(x - wm_tx[k]) + abs(y - wm_ty[k]) > 6 ||
                          abs((int)wd - wm_twd[k]) + abs((int)ht - wm_tht[k]) > 6;
        wm_tx[k] = x; wm_ty[k] = y; wm_twd[k] = (int)wd; wm_tht[k] = (int)ht;
        if (moved) {
            if (++wm_tslide[k] >= 2) ghost = 1;   /* sustained slide: ghost */
        } else {
            wm_tslide[k] = 0;
        }
    }
    /* prune tracker entries whose window is gone */
    for (int j = wm_ntw - 1; j >= 0; j--) {
        int alive = 0;
        for (int s = 0; s < ns; s++) if (seen[s] == wm_tw[j]) { alive = 1; break; }
        if (!alive) {
            memmove(&wm_tw[j],    &wm_tw[j+1],    (size_t)(wm_ntw-j-1)*sizeof wm_tw[0]);
            memmove(&wm_tx[j],    &wm_tx[j+1],    (size_t)(wm_ntw-j-1)*sizeof wm_tx[0]);
            memmove(&wm_ty[j],    &wm_ty[j+1],    (size_t)(wm_ntw-j-1)*sizeof wm_ty[0]);
            memmove(&wm_twd[j],   &wm_twd[j+1],   (size_t)(wm_ntw-j-1)*sizeof wm_twd[0]);
            memmove(&wm_tht[j],   &wm_tht[j+1],   (size_t)(wm_ntw-j-1)*sizeof wm_tht[0]);
            memmove(&wm_tslide[j],&wm_tslide[j+1],(size_t)(wm_ntw-j-1)*sizeof wm_tslide[0]);
            wm_ntw--;
        }
    }
    if (ghost) {
        fprintf(stderr, "[elevende] ghost defense: sliding window killed\n");
        wm_killmove();
        if (cancel_repeat < 10) cancel_repeat = 10;
    }
}

static void win_title(Window w, char *buf, int nbuf) {
    Atom type;
    int fmt;
    unsigned long nitems, after;
    unsigned char *prop = NULL;
    buf[0] = 0;
    if (XGetWindowProperty(dpy, w, atom("_NET_WM_NAME"), 0, 256, False,
                           AnyPropertyType, &type, &fmt, &nitems, &after, &prop) ==
            Success && type != None && prop)
        snprintf(buf, nbuf, "%s", (const char *)prop);
    else if (XGetWindowProperty(dpy, w, XA_WM_NAME, 0, 256, False,
                                AnyPropertyType, &type, &fmt, &nitems, &after,
                                &prop) == Success && type != None && prop)
        snprintf(buf, nbuf, "%s", (const char *)prop);
    if (prop) XFree(prop);
}

static int refresh_tasks(void) {
    Atom type = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *prop = NULL;

    static Window last_wins[MAX_TASKS];
    static int     last_ntask = -1;
    static Window  last_active = None;

    ntask = 0;
    if (XGetWindowProperty(dpy, root, atom("_NET_CLIENT_LIST"), 0, 0x4000, False,
                           XA_WINDOW, &type, &fmt, &nitems, &after, &prop) == Success &&
        type != None && prop) {
        Window *ws = (Window *)prop;
        Atom wm_type_desktop = atom("_NET_WM_WINDOW_TYPE_DESKTOP");
        for (unsigned long i = 0; i < nitems && ntask < MAX_TASKS; i++) {
            if (ws[i] == win_bar || ws[i] == win_menu || ws[i] == win_desk)
                continue;
            /* ignore desktop-type windows too (pagers/other desktops) */
            Atom tp = None;
            int tfmt = 0;
            unsigned long tn = 0, ta = 0;
            unsigned char *tprop = NULL;
            if (XGetWindowProperty(dpy, ws[i], atom("_NET_WM_WINDOW_TYPE"),
                                   0, 4, False, XA_ATOM, &tp, &tfmt, &tn, &ta,
                                   &tprop) == Success && tprop) {
                if (tp == XA_ATOM && tn > 0 &&
                    *((Atom *)tprop) == wm_type_desktop) {
                    XFree(tprop);
                    continue;
                }
            }
            if (tprop) XFree(tprop);
            tasks[ntask].win = ws[i];
            win_title(ws[i], tasks[ntask].title, sizeof tasks[ntask].title);
            tasks[ntask].active = 0;
            ntask++;
        }
        XFree(prop);
    }
    Window sel = None;
    if (XGetWindowProperty(dpy, root, atom("_NET_ACTIVE_WINDOW"), 0, 1, False,
                           XA_WINDOW, &type, &fmt, &nitems, &after, &prop) == Success &&
        type != None && prop && nitems)
        sel = *((Window *)prop);
    if (prop) XFree(prop);
    g_active = sel;
    for (int i = 0; i < ntask; i++)
        tasks[i].active = (tasks[i].win == g_active);

    int changed = (ntask != last_ntask || g_active != last_active);
    if (!changed)
        for (int i = 0; i < ntask && i < last_ntask; i++)
            if (tasks[i].win != last_wins[i]) { changed = 1; break; }
    static int first = 1;
    if (changed || first) {
        first = 0;
        last_ntask = ntask;
        last_active = g_active;
        for (int i = 0; i < ntask; i++) last_wins[i] = tasks[i].win;
    }
    return changed;
}

/* ------------------------------------------------------------------ icons */
/*
 * Real program icons instead of bare letters:
 *   - taskbar: read per-window _NET_WM_ICON (ARGB), scale + draw;
 *   - start tiles & desktop: load themed PNGs (hicolor theme) via libpng.
 * PNGs are composited over their tile color in software, so corners are never
 * black on visuals without an alpha channel. All results are cached.
 */
typedef struct { unsigned char r, g, b; } RGB3;

static unsigned vshift(unsigned long mask) {
    unsigned s = 0;
    while (mask && !(mask & 1)) { mask >>= 1; s++; }
    return s;
}
static unsigned vpop(unsigned long mask) {
    unsigned n = 0;
    while (mask) { n += mask & 1; mask >>= 1; }
    return n;
}
static unsigned shR, shG, shB, bitsR, bitsG, bitsB;
static int  colors_ready = 0;

static void visual_bits(void) {
    if (colors_ready) return;
    shR = vshift(vis->red_mask);   bitsR = vpop(vis->red_mask);
    shG = vshift(vis->green_mask); bitsG = vpop(vis->green_mask);
    shB = vshift(vis->blue_mask);  bitsB = vpop(vis->blue_mask);
    colors_ready = 1;
}

static unsigned long vpx(int r, int g, int b) {
    visual_bits();
    if (vis->class == TrueColor)
        return ((unsigned long)(r >> (8 - bitsR)) << shR) |
               ((unsigned long)(g >> (8 - bitsG)) << shG) |
               ((unsigned long)(b >> (8 - bitsB)) << shB);
    XColor c;
    c.red = (unsigned short)(r << 8); c.green = (unsigned short)(g << 8);
    c.blue = (unsigned short)(b << 8); c.flags = DoRed | DoGreen | DoBlue;
    return (XAllocColor(dpy, cmap, &c)) ? c.pixel : 0;
}

static void set_rgb(RGB3 *o, const Color *c) {
    o->r = (unsigned char)(c->xft.color.red >> 8);
    o->g = (unsigned char)(c->xft.color.green >> 8);
    o->b = (unsigned char)(c->xft.color.blue >> 8);
}

static unsigned char blend_c(unsigned char fg, unsigned char bg, unsigned a) {
    return (unsigned char)((fg * a + bg * (255 - a)) / 255);
}

static void opaque_bbox(unsigned char *px, int w, int h,
                        int *x0, int *y0, int *x1, int *y1) {
    int l = w, t = h, r = -1, b = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (px[(y * w + x) * 4 + 3] >= 40) {
                if (x < l) l = x;
                if (x > r) r = x;
                if (y < t) t = y;
                if (y > b) b = y;
            }
    if (r < l) { *x0 = 0; *y0 = 0; *x1 = w; *y1 = h; return; }
    *x0 = l; *y0 = t; *x1 = r + 1; *y1 = b + 1;
}

/* scale the RGBA image into a `size` x `size` pixmap blended over `bg` */
static Pixmap rgba_pm_from_src(unsigned char *px, int sw, int sh,
                               int x0, int y0, int bw, int bh,
                               int size, const RGB3 *bg) {
    if (bw < 1 || bh < 1 || size < 1) return None;
    Pixmap pm = XCreatePixmap(dpy, root, size, size, DefaultDepth(dpy, scr));
    if (!pm) return None;
    XImage *im = XCreateImage(dpy, vis, DefaultDepth(dpy, scr), ZPixmap, 0,
                              calloc(1, (size_t)size * size * 4), size, size,
                              32, 0);
    if (!im) { XFreePixmap(dpy, pm); return None; }
    for (int dy = 0; dy < size; dy++) {
        for (int dx = 0; dx < size; dx++) {
            float u = (float)dx / (size - 1), v = (float)dy / (size - 1);
            float sx = x0 + u * (bw - 1), sy = y0 + v * (bh - 1);
            int si = (int)sx, sj = (int)sy;
            float fx = sx - si, fy = sy - sj;
            int sp = si < sw - 1 ? si + 1 : si, sq = sj < sh - 1 ? sj + 1 : sj;
            unsigned char o[4];
            for (int c = 0; c < 4; c++) {
                float a = px[(sj * sw + si) * 4 + c], b2 = px[(sj * sw + sp) * 4 + c];
                float d = px[(sq * sw + si) * 4 + c], e = px[(sq * sw + sp) * 4 + c];
                float top = a + (b2 - a) * fx, bot = d + (e - d) * fx;
                o[c] = (unsigned char)(top + (bot - top) * fy);
            }
            unsigned a = o[3];
            XPutPixel(im, dx, dy, vpx(blend_c(o[0], bg->r, a),
                                      blend_c(o[1], bg->g, a),
                                      blend_c(o[2], bg->b, a)));
        }
    }
    XPutImage(dpy, pm, bgc, im, 0, 0, 0, 0, size, size);
    XDestroyImage(im);
    return pm;
}

#ifndef HAVE_GDKPIXBUF
/* PNG-only loader used when gdk-pixbuf is unavailable */
static Pixmap png_to_pm(const char *path, int size, const RGB3 *bg) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return None;
    unsigned char sig[8];
    if (fread(sig, 1, 8, fp) != 8 || !png_check_sig(sig, 8)) {
        fclose(fp);
        return None;
    }
    png_structp ps = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop pi = png_create_info_struct(ps);
    Pixmap pm = None;
    if (!ps || !pi) { fclose(fp); return None; }
    if (setjmp(png_jmpbuf(ps))) {
        png_destroy_read_struct(&ps, &pi, NULL);
        fclose(fp);
        if (pm) XFreePixmap(dpy, pm);
        return None;
    }
    png_init_io(ps, fp);
    png_set_sig_bytes(ps, 8);
    png_read_info(ps, pi);
    int w = png_get_image_width(ps, pi), h = png_get_image_height(ps, pi);
    int ct = png_get_color_type(ps, pi), bit = png_get_bit_depth(ps, pi);
    int will_alpha = (ct & PNG_COLOR_MASK_ALPHA) != 0 ||
                     (ct == PNG_COLOR_TYPE_PALETTE &&
                      png_get_tRNS(ps, pi, NULL, NULL, NULL));
    if (bit == 16) png_set_strip_16(ps);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(ps);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(ps);
    if (will_alpha || ct == PNG_COLOR_TYPE_PALETTE) png_set_tRNS_to_alpha(ps);
    if (bit < 8) png_set_packing(ps);
    if (!will_alpha) png_set_filler(ps, 0xff, PNG_FILLER_AFTER);
    png_read_update_info(ps, pi);
    unsigned char *px = malloc((size_t)w * h * 4);
    png_bytep *rows = malloc((size_t)h * sizeof(png_bytep));
    for (int y = 0; y < h; y++) rows[y] = px + (size_t)y * w * 4;
    png_read_image(ps, rows);
    png_read_end(ps, NULL);
    free(rows);
    int x0, y0, x1, y1;
    opaque_bbox(px, w, h, &x0, &y0, &x1, &y1);
    pm = rgba_pm_from_src(px, w, h, x0, y0, x1 - x0, y1 - y0, size, bg);
    free(px);
    png_destroy_read_struct(&ps, &pi, NULL);
    fclose(fp);
    return pm;
}
#endif /* !HAVE_GDKPIXBUF */

#ifdef HAVE_GDKPIXBUF
/*
 * gdk-pixbuf loader: reads PNG, SVG (via librsvg), JPG, XPM and more.
 * Handles the scaled-for-SVG case where gdk-pixbuf returns a buffer smaller
 * than `size` (aspect preserved) by centring it into the target pixmap.
 */
static Pixmap pixbuf_to_pm(const char *path, int size, const RGB3 *bg) {
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) {
        if (err) g_error_free(err);
        return None;
    }
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    int ha = gdk_pixbuf_get_has_alpha(pb);
    if (w < 1 || h < 1 || (nch != 3 && nch != 4)) {
        /* NOTE: RGB-without-alpha is fine (treated as opaque); rejecting it
         * used to turn JPEG/RGB icons into blank white boxes. */
        g_object_unref(pb);
        return None;
    }
    Pixmap pm = XCreatePixmap(dpy, root, size, size, DefaultDepth(dpy, scr));
    if (!pm) { g_object_unref(pb); return None; }
    XImage *im = XCreateImage(dpy, vis, DefaultDepth(dpy, scr), ZPixmap, 0,
                              calloc(1, (size_t)size * size * 4), size, size,
                              32, 0);
    if (!im) { XFreePixmap(dpy, pm); g_object_unref(pb); return None; }
    GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, size, size,
                                            GDK_INTERP_BILINEAR);
    if (sc) { g_object_unref(pb); pb = sc; }
    w = gdk_pixbuf_get_width(pb); h = gdk_pixbuf_get_height(pb);
    rs = gdk_pixbuf_get_rowstride(pb);
    nch = gdk_pixbuf_get_n_channels(pb);
    ha = gdk_pixbuf_get_has_alpha(pb);
    const guchar *pd = gdk_pixbuf_get_pixels(pb);
    int ox = (size - w) / 2, oy = (size - h) / 2;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int sy = y - oy, sx = x - ox;
            unsigned char r, g, b, a = 0;
            if (sy < 0 || sy >= h || sx < 0 || sx >= w) {
                r = g = b = 0;
            } else if (ha) {
                const guchar *p = pd + (size_t)sy * rs + (size_t)sx * nch;
                r = p[0]; g = p[1]; b = p[2]; a = p[3];
            } else {
                const guchar *p = pd + (size_t)sy * rs + (size_t)sx * nch;
                r = p[0]; g = p[1]; b = p[2]; a = 0xff;
            }
            XPutPixel(im, x, y, vpx(blend_c(r, bg->r, a),
                                    blend_c(g, bg->g, a),
                                    blend_c(b, bg->b, a)));
        }
    XPutImage(dpy, pm, bgc, im, 0, 0, 0, 0, size, size);
    XDestroyImage(im);
    g_object_unref(pb);
    return pm;
}

/* full-screen wallpaper from gdk-pixbuf (JPG/PNG/SVG/...): scale to the
 * desktop and repaint wall_pm so the wallpaper button actually works on a
 * stock Kali where nearly every background is a JPEG. */
static int wallpaper_from_pixbuf(const char *path) {
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) {
        if (err) g_error_free(err);
        return 0;
    }
    int dw = scr_w, dh = scr_h - BAR_H;
    GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, dw, dh, GDK_INTERP_BILINEAR);
    if (sc) { g_object_unref(pb); pb = sc; }
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    int ha = gdk_pixbuf_get_has_alpha(pb);
    const guchar *pd = gdk_pixbuf_get_pixels(pb);
    Pixmap pm = XCreatePixmap(dpy, root, dw, dh, DefaultDepth(dpy, scr));
    XImage *im = XCreateImage(dpy, vis, DefaultDepth(dpy, scr), ZPixmap, 0,
                              calloc(1, (size_t)dw * dh * 4), dw, dh, 32, 0);
    int ok = 0;
    if (pm && im) {
        for (int y = 0; y < h; y++) {
            const guchar *row = pd + (size_t)y * rs;
            for (int x = 0; x < w; x++) {
                const guchar *p = row + (size_t)x * nch;
                unsigned long v = ha
                    ? vpx(blend_c(p[0], 0, p[3]), blend_c(p[1], 0, p[3]),
                          blend_c(p[2], 0, p[3]))
                    : vpx(p[0], p[1], p[2]);
                XPutPixel(im, x, y, v);
            }
            if (w < dw) {                   /* stretch-fit: fill last column */
                const guchar *p = row + (size_t)(w - 1) * nch;
                unsigned long v = ha
                    ? vpx(blend_c(p[0], 0, p[3]), blend_c(p[1], 0, p[3]),
                          blend_c(p[2], 0, p[3]))
                    : vpx(p[0], p[1], p[2]);
                for (int x = w; x < dw; x++) XPutPixel(im, x, y, v);
            }
        }
        if (h < dh) {                       /* remaining bottom rows */
            const guchar *row = pd + (size_t)(h - 1) * rs;
            for (int y = h; y < dh; y++)
                for (int x = 0; x < dw; x++) {
                    int xx = x < w ? x : w - 1;
                    const guchar *p = row + (size_t)xx * nch;
                    unsigned long v = ha
                        ? vpx(blend_c(p[0], 0, p[3]), blend_c(p[1], 0, p[3]),
                              blend_c(p[2], 0, p[3]))
                        : vpx(p[0], p[1], p[2]);
                    XPutPixel(im, x, y, v);
                }
        }
        XPutImage(dpy, pm, bgc, im, 0, 0, 0, 0, dw, dh);
        ok = 1;
    }
    if (im) XDestroyImage(im);
    g_object_unref(pb);
    if (ok) {
        if (wall_pm) XFreePixmap(dpy, wall_pm);
        wall_pm = pm;
    } else {
        if (pm) XFreePixmap(dpy, pm);
    }
    return ok;
}
#endif /* HAVE_GDKPIXBUF */

/* single entry point: gdk-pixbuf (SVG/PNG/...) when built in, PNG otherwise */
/* -------- true-alpha icon rendering for the desktop --------------------
 * Desktop icons sit on the wallpaper, so baking them onto a solid color
 * (the old path) produced ugly opaque boxes. Instead we keep the scaled
 * RGBA pixels in a small cache and blend them over whatever is already on
 * the desktop window (wallpaper) at paint time. */

typedef struct {
    char key[320];
    int  size;
    int  w, h;
    unsigned char *rgba;      /* straight alpha, w*h*4 */
} IconRGBA;

#define IR_MAX 64
static IconRGBA ir_cache[IR_MAX];
static int      ir_n = 0;

static IconRGBA *icon_rgba_for_path(const char *path, int size) {
    char key[320];
    snprintf(key, sizeof key, "%s@%d", path, size);
    for (int i = 0; i < ir_n; i++)
        if (!strcmp(ir_cache[i].key, key)) return &ir_cache[i];

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) { if (err) g_error_free(err); return NULL; }
    GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, size, size, GDK_INTERP_BILINEAR);
    if (sc) { g_object_unref(pb); pb = sc; }
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    int ha = gdk_pixbuf_get_has_alpha(pb);
    if (w < 1 || h < 1 || (nch != 3 && nch != 4)) { g_object_unref(pb); return NULL; }

    IconRGBA *e;
    if (ir_n < IR_MAX) {
        e = &ir_cache[ir_n++];
    } else {                                   /* drop oldest */
        e = &ir_cache[0];
        memmove(&ir_cache[0], &ir_cache[1],
                (IR_MAX - 1) * sizeof ir_cache[0]);
        ir_n = IR_MAX - 1;
        e = &ir_cache[ir_n++];
    }
    snprintf(e->key, sizeof e->key, "%s", key);
    e->size = size;
    e->w = w; e->h = h;
    e->rgba = malloc((size_t)w * h * 4);
    if (!e->rgba) { ir_n--; return NULL; }
    const guchar *pd = gdk_pixbuf_get_pixels(pb);
    for (int y = 0; y < h; y++) {
        const guchar *row = pd + (size_t)y * rs;
        for (int x = 0; x < w; x++) {
            unsigned char *o = e->rgba + ((size_t)y * w + x) * 4;
            o[0] = row[x * nch + 0];
            o[1] = row[x * nch + 1];
            o[2] = row[x * nch + 2];
            o[3] = (nch == 4 && ha) ? row[x * nch + 3] : 0xFF;
        }
    }
    g_object_unref(pb);
    return e;
}

/* alpha-blend an RGBA icon onto the desktop window at (x, y) */
static void desk_draw_icon_alpha(IconRGBA *ib, int x, int y) {
    if (!ib || !ib->rgba) return;
    int w = ib->w, h = ib->h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h - BAR_H) h = scr_h - BAR_H - y;
    if (w <= 0 || h <= 0) return;
    XImage *im = XGetImage(dpy, win_desk, x, y, (unsigned)w, (unsigned)h,
                           AllPlanes, ZPixmap);
    if (!im) return;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            const unsigned char *o = ib->rgba + ((size_t)(yy + (y < 0 ? -y : 0)) * ib->w + (xx + (x < 0 ? -x : 0))) * 4;
            unsigned a = o[3];
            if (a == 0) continue;
            unsigned long px = XGetPixel(im, xx, yy);
            int pr = (px >> 16) & 0xFF, pg = (px >> 8) & 0xFF, pb2 = px & 0xFF;
            int nr, ng, nb;
            if (a == 0xFF) { nr = o[0]; ng = o[1]; nb = o[2]; }
            else {
                nr = o[0] + (pr - (int)o[0]) * (int)(255 - a) / 255;
                ng = o[1] + (pg - (int)o[1]) * (int)(255 - a) / 255;
                nb = o[2] + (pb2 - (int)o[2]) * (int)(255 - a) / 255;
            }
            XPutPixel(im, xx, yy, (unsigned long)((nr << 16) | (ng << 8) | nb));
        }
    }
    XPutImage(dpy, win_desk, bgc, im, 0, 0, x, y, (unsigned)w, (unsigned)h);
    XDestroyImage(im);
}

static Pixmap icon_for_path(const char *path, int size, const RGB3 *bg) {
    if (!path) return None;
#ifdef HAVE_GDKPIXBUF
    return pixbuf_to_pm(path, size, bg);
#else
    return png_to_pm(path, size, bg);
#endif
}

/* ---- full-screen wallpaper (gdk-pixbuf when built in, PNG otherwise) --- */
static int load_wallpaper(const char *path) {
#ifdef HAVE_GDKPIXBUF
    if (wallpaper_from_pixbuf(path)) return 1;
    /* gdk-pixbuf may lack compressed plugins: fall back to libpng below */
#endif
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    unsigned char sig[8];
    if (fread(sig, 1, 8, fp) != 8 || !png_check_sig(sig, 8)) { fclose(fp); return 0; }
    png_structp ps = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop pi = png_create_info_struct(ps);
    if (!ps || !pi) { fclose(fp); return 0; }
    if (setjmp(png_jmpbuf(ps))) {
        png_destroy_read_struct(&ps, &pi, NULL);
        fclose(fp);
        return 0;
    }
    png_init_io(ps, fp);
    png_set_sig_bytes(ps, 8);
    png_read_info(ps, pi);
    int w = png_get_image_width(ps, pi), h = png_get_image_height(ps, pi);
    int ct = png_get_color_type(ps, pi), bit = png_get_bit_depth(ps, pi);
    if (w < 1 || h < 1) { png_destroy_read_struct(&ps, &pi, NULL); fclose(fp); return 0; }
    if (bit == 16) png_set_strip_16(ps);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(ps);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(ps);
    if (ct & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(ps);
    if (bit < 8) png_set_packing(ps);
    png_read_update_info(ps, pi);
    unsigned char *px = malloc((size_t)w * h * 3);
    png_bytep *rows = malloc((size_t)h * sizeof(png_bytep));
    for (int y = 0; y < h; y++) rows[y] = px + (size_t)y * w * 3;
    png_read_image(ps, rows);
    png_read_end(ps, NULL);
    free(rows);
    png_destroy_read_struct(&ps, &pi, NULL);
    fclose(fp);

    int dw = scr_w, dh = scr_h - BAR_H;
    Pixmap pm = XCreatePixmap(dpy, root, dw, dh, DefaultDepth(dpy, scr));
    XImage *im = XCreateImage(dpy, vis, DefaultDepth(dpy, scr), ZPixmap, 0,
                              calloc(1, (size_t)dw * dh * 4), dw, dh, 32, 0);
    if (pm && im) {
        for (int y = 0; y < dh; y++) {
            int sy = (int)((y + 0.5f) * h / dh);
            if (sy >= h) sy = h - 1;
            for (int x = 0; x < dw; x++) {
                int sx = (int)((x + 0.5f) * w / dw);
                if (sx >= w) sx = w - 1;
                unsigned char *p3 = px + ((size_t)sy * w + sx) * 3;
                XPutPixel(im, x, y, vpx(p3[0], p3[1], p3[2]));
            }
        }
        XPutImage(dpy, pm, bgc, im, 0, 0, 0, 0, dw, dh);
        XDestroyImage(im);
        if (wall_pm) XFreePixmap(dpy, wall_pm);
        wall_pm = pm;
        free(px);
        return 1;
    }
    free(px);
    if (im) XDestroyImage(im);
    if (pm) XFreePixmap(dpy, pm);
    return 0;
}

static void wallpaper_init(void) {
    const char *home = getenv("HOME");
    char pb[1024];
    /* Reset before each reload: a failed replacement must fall back cleanly
       instead of preserving a stale success flag and stale desktop pixmap. */
    wall_ok = 0;
    if (home) {
        snprintf(pb, sizeof pb, "%s/.local/share/elevende/wallpaper.png", home);
        struct stat st;
        if (stat(pb, &st) == 0) {
            wall_mtime = st.st_mtime;
            wall_mtime_nsec = st.st_mtim.tv_nsec;
            wall_size = st.st_size;
            if (load_wallpaper(pb)) wall_ok = 1;
        } else {
            wall_mtime = 0;
            wall_mtime_nsec = 0;
            wall_size = -1;
        }
    }
    if (!wall_ok && load_wallpaper("/usr/local/share/elevende-shell/wallpaper.png"))
        wall_ok = 1;
}

static const char *theme_find(const char *name, int size) {
    static char buf[512];
    static const char *cats[] = { "apps", "places", "devices", "mimetypes" };
    static const char *exts[] = { ".png", ".svg" };
    int sizes[] = { size, size * 2, 48, 32, 24, 22, 64, 128, 96, 16, 256 };
    /* ElevenDE's own Win11-style icon set always wins */
    for (int s2 = 0; s2 < 11 && sizes[s2] <= 512; s2++)
        for (unsigned c = 0; c < sizeof cats / sizeof cats[0]; c++)
            for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
                snprintf(buf, sizeof buf,
                         "/usr/local/share/elevende-shell/icons/%dx%d/%s/%s%s",
                         sizes[s2], sizes[s2], cats[c], name, exts[e]);
                if (access(buf, R_OK) == 0) return buf;
            }
    for (unsigned c = 0; c < sizeof cats / sizeof cats[0]; c++)
        for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
            snprintf(buf, sizeof buf,
                     "/usr/local/share/elevende-shell/icons/scalable/%s/%s%s",
                     cats[c], name, exts[e]);
            if (access(buf, R_OK) == 0) return buf;
        }
    for (int s2 = 0; s2 < 8 && sizes[s2] <= 512; s2++) {
        for (unsigned c = 0; c < sizeof cats / sizeof cats[0]; c++)
            for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
                snprintf(buf, sizeof buf, "/usr/share/icons/hicolor/%dx%d/%s/%s%s",
                         sizes[s2], sizes[s2], cats[c], name, exts[e]);
                if (access(buf, R_OK) == 0) return buf;
            }
    }
    /* scalable dirs: modern themes (Adwaita) are SVG-only under "scalable/" */
    for (unsigned c = 0; c < sizeof cats / sizeof cats[0]; c++)
        for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
            snprintf(buf, sizeof buf, "/usr/share/icons/hicolor/scalable/%s/%s%s",
                     cats[c], name, exts[e]);
            if (access(buf, R_OK) == 0) return buf;
        }
    for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
        snprintf(buf, sizeof buf, "/usr/share/pixmaps/%s%s", name, exts[e]);
        if (access(buf, R_OK) == 0) return buf;
    }
    static const char *themes[] = { "Adwaita", "Papirus", "Kali", "gnome",
                                    "Adwaita-Dark", "hicolor" };
    for (unsigned t = 0; t < sizeof themes / sizeof themes[0]; t++)
        for (int s2 = 0; s2 < 8 && sizes[s2] <= 512; s2++)
            for (unsigned c = 0; c < sizeof cats / sizeof cats[0]; c++)
                for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
                    snprintf(buf, sizeof buf, "/usr/share/icons/%s/%dx%d/%s/%s%s",
                             themes[t], sizes[s2], sizes[s2], cats[c], name,
                             exts[e]);
                    if (access(buf, R_OK) == 0) return buf;
                }
    for (unsigned t = 0; t < sizeof themes / sizeof themes[0]; t++)
        for (unsigned c = 0; c < sizeof cats / sizeof cats[0]; c++)
            for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
                snprintf(buf, sizeof buf, "/usr/share/icons/%s/scalable/%s/%s%s",
                         themes[t], cats[c], name, exts[e]);
                if (access(buf, R_OK) == 0) return buf;
            }
    return NULL;
}

static const char *find_theme_png(const char *name, int size) {
    const char *r = theme_find(name, size);
    if (r) return r;
    /* generic fallbacks so tiles/rows never degrade to bare letters */
    static const char *generic[] = {
        "application-x-executable", "applications-other",
        "application-default-icon", "text-x-generic", "executable"
    };
    for (size_t i = 0; i < sizeof generic / sizeof generic[0]; i++) {
        r = theme_find(generic[i], size);
        if (r) return r;
    }
    return NULL;
}

typedef struct { char key[96]; Pixmap pm; } ICache;
#define IC_MAX 128
static ICache icache[IC_MAX];
static int    nicache = 0;

static Pixmap icache_find(const char *key) {
    for (int i = 0; i < nicache; i++)
        if (!strcmp(icache[i].key, key)) return icache[i].pm;
    return None;
}
static void icache_put(const char *key, Pixmap pm) {
    if (nicache >= IC_MAX) { nicache = 0; }
    snprintf(icache[nicache].key, sizeof icache[nicache].key, "%s", key);
    icache[nicache].pm = pm;
    nicache++;
}

/*
 * Find _NET_WM_ICON for a window: property holds [w,h,ARGB...] blocks
 * (premultiplied alpha) for several sizes; pick the best and scale.
 */
static long *_net_icon_data(Window w, int *nitems_out) {
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, w, atom("_NET_WM_ICON"), 0, 0x3fff, False,
                           XA_CARDINAL, &type, &fmt, &n, &after, &prop) ==
            Success && type == XA_CARDINAL && prop) {
        *nitems_out = (int)n;
        return (long *)prop;
    }
    if (prop) XFree(prop);
    return NULL;
}

static Pixmap neticon_pm(Window w, int size, const RGB3 *bg) {
    int nit = 0;
    long *data = _net_icon_data(w, &nit);
    if (!data || nit < 2) return None;
    int best = -1, bestw = 0, besth = 0, bestdc = 1 << 30, off = 0;
    while (off + 2 <= nit) {
        int iw = (int)data[off], ih = (int)data[off + 1];
        long need = (long)iw * ih;
        if (iw > 0 && ih > 0 && off + 2 + need <= nit) {
            int dc = (iw * ih > size * size) ? iw * ih - size * size
                                             : size * size - iw * ih;
            if (dc < bestdc) { bestdc = dc; best = off; bestw = iw; besth = ih; }
            off += 2 + (int)need;
        } else break;
    }
    if (best < 0) return None;
    /* build an RGBA buffer (premultiplied RGB present; convert to straight?) */
    unsigned char *px = malloc((size_t)bestw * besth * 4);
    long *src = data + best + 2;
    int opaque_n = 0, near_black_n = 0;
    for (int y = 0; y < besth; y++)
        for (int x = 0; x < bestw; x++) {
            unsigned long a = (unsigned long)src[(size_t)y * bestw + x];
            unsigned sh = 24, mid = 16, lo = 8;
            unsigned char alpha = (unsigned char)(a >> sh);
            unsigned char r = (unsigned char)(a >> mid);
            unsigned char g = (unsigned char)(a >> lo);
            unsigned char b = (unsigned char)a;
            int i = (y * bestw + x) * 4;
            if (alpha == 0) { px[i] = px[i+1] = px[i+2] = 0; px[i+3] = 0; continue; }
            /* un-premultiply */
            px[i]   = (unsigned char)((r * 255) / alpha);
            px[i+1] = (unsigned char)((g * 255) / alpha);
            px[i+2] = (unsigned char)((b * 255) / alpha);
            px[i+3] = alpha;
            opaque_n++;
            if (px[i] < 18 && px[i+1] < 18 && px[i+2] < 18) near_black_n++;
        }
    int x0, y0, x1, y1;
    opaque_bbox(px, bestw, besth, &x0, &y0, &x1, &y1);
    /* degenerate icon (blank/1px placeholder some apps set): fall through
     * to the themed/glyph fallback instead of drawing an empty box */
    if (x1 - x0 < 4 || y1 - y0 < 4 ||
        (opaque_n > 0 && near_black_n * 100 / opaque_n > 92)) {
        /* A solid black server placeholder is not an application icon. */
        free(px);
        XFree(data);
        return None;
    }
    Pixmap pm = rgba_pm_from_src(px, bestw, besth, x0, y0, x1 - x0, y1 - y0,
                                 size, bg);
    free(px);
    XFree(data);
    return pm;
}

static Pixmap icon_for_task(Window w, int size) {
    char key[80];
    snprintf(key, sizeof key, "W%lu@%d", (unsigned long)w, size);
    Pixmap pm = icache_find(key);
    if (pm) return pm;

    /* Known ElevenDE apps must prefer the packaged Fluent-style image. Qt/GTK
       clients sometimes publish a transparent or all-black _NET_WM_ICON during
       startup; choosing it first is what caused blank taskbar buttons. */
    char cls[64] = "";
    win_class(w, cls, sizeof cls);
    if (cls[0]) pm = icon_for_exact(app_icon_name(cls), size, &cc_task);
    if (!pm) {
        RGB3 bg;
        set_rgb(&bg, &cc_task);
        pm = neticon_pm(w, size, &bg);
    }
    if (pm) icache_put(key, pm);
    return pm;
}

static Pixmap icon_for_png(const char *name, int size, const Color *bg) {
    char key[160];
    snprintf(key, sizeof key, "P%s@%d@%08lx", name, size, bg->pixel);
    Pixmap pm = icache_find(key);
    if (pm) return pm;
    const char *path = find_theme_png(name, size);
    if (!path) return None;
    RGB3 fg;
    set_rgb(&fg, bg);
    pm = icon_for_path(path, size, &fg);
    if (pm) icache_put(key, pm);
    return pm;
}

/* exact lookup: NO generic "unknown app" fallback -- returns the themed icon
 * for `name` or None. Used by fixed tiles so they never degrade to the
 * generic gnome fallback icon. */
static Pixmap icon_for_exact(const char *name, int size, const Color *bg) {
    char key[160];
    snprintf(key, sizeof key, "E%s@%d@%08lx", name, size, bg->pixel);
    Pixmap pm = icache_find(key);
    if (pm) return pm;
    const char *path = theme_find(name, size);
    if (!path) return None;
    RGB3 fg;
    set_rgb(&fg, bg);
    pm = icon_for_path(path, size, &fg);
    if (pm) icache_put(key, pm);
    return pm;
}

/* try several icon names in order; returns None if every one is missing */
static Pixmap icon_for_names(const char *const names[], int n, int size,
                             const Color *bg) {
    for (int i = 0; i < n && names[i]; i++) {
        Pixmap pm = icon_for_exact(names[i], size, bg);
        if (pm) return pm;
    }
    return None;
}

/* lowercase WM_CLASS res_name of a window ("" if unavailable) */
static void win_class(Window w, char *buf, int nbuf) {
    buf[0] = 0;
    XClassHint ch;
    memset(&ch, 0, sizeof ch);
    if (XGetClassHint(dpy, w, &ch) && ch.res_name)
        snprintf(buf, nbuf, "%s", ch.res_name);
    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
    for (char *p = buf; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += (char)('a' - 'A');
}

/* map common app classes to themed icon names */
static const char *app_icon_name(const char *cls) {
    static const struct { const char *cls, *icon; } tab[] = {
        { "xterm",            "utilities-terminal-symbolic" },
        { "urxvt",            "utilities-terminal-symbolic" },
        { "konsole",          "utilities-terminal-symbolic" },
        { "terminator",       "utilities-terminal-symbolic" },
        { "gnome-terminal",   "utilities-terminal-symbolic" },
        { "elevende-calc",    "accessories-calculator" },
        { "elevende-settings", "preferences-system" },
        { "elevende-taskmgr", "utilities-system-monitor" },
        { "elevende-notepad", "accessories-text-editor" },
        { "elevende-photos",  "image-x-generic" },
        { "explorer.exe",     "system-file-manager" },
        { "runbox",           "system-run" },
        { "firefox",          "firefox" },
        { "firefox-esr",      "firefox" },
        { "chromium",         "chromium" },
        { "google-chrome",    "chrome" },
        { "explorer",         "system-file-manager" },
        { "nautilus",         "system-file-manager" },
        { "nemo",             "system-file-manager" },
        { "thunar",           "system-file-manager" },
        { "gedit",            "accessories-text-editor" },
        { "kate",             "accessories-text-editor" },
        { "xed",              "accessories-text-editor" },
        { "vlc",              "vlc" },
        { "mpv",              "multimedia-video-player" },
        { "rhythmbox",        "multimedia-audio-player" },
        { "eog",              "image-viewer" },
        { "gimp",             "gimp" },
        { "inkscape",         "inkscape" },
        { "file-roller",      "file-archiver" },
        { "evince",           "x-office-document" },
        { "kcalc",            "accessories-calculator" },
        { "gnome-calculator", "accessories-calculator" },
        { "blender",          "blender" },
        { "code",             "code" },
        { "libreoffice-writer", "libreoffice-writer" },
        { "", "" }
    };
    for (int i = 0; tab[i].cls[0]; i++)
        if (!strcmp(cls, tab[i].cls)) return tab[i].icon;
    return cls;   /* fall back to raw class name as an icon name */
}

/* class -> vector glyph used when no themed icon exists at all, so the
 * taskbar NEVER falls back to the generic "unknown application" icon */
static int app_icon_kind(const char *cls) {
    static const struct { const char *sub; int kind; } tab[] = {
        { "terminal", VI_TERM }, { "xterm", VI_TERM }, { "konsole", VI_TERM },
        { "terminator", VI_TERM }, { "urxvt", VI_TERM }, { "bash", VI_TERM },
        { "explorer", VI_FOLDER }, { "nautilus", VI_FOLDER },
        { "nemo", VI_FOLDER }, { "thunar", VI_FOLDER }, { "pcmanfm", VI_FOLDER },
        { "krusader", VI_FOLDER }, { "dolphin", VI_FOLDER },
        { "firefox", VI_BROWSER }, { "chromium", VI_BROWSER },
        { "chrome", VI_BROWSER }, { "epiphany", VI_BROWSER },
        { "gedit", VI_EDITOR }, { "kate", VI_EDITOR }, { "code", VI_EDITOR },
        { "vim", VI_EDITOR }, { "emacs", VI_EDITOR }, { "libreoffice", VI_EDITOR },
        { "control-center", VI_SETTINGS }, { "settings", VI_SETTINGS },
        { NULL, VI_FILE }
    };
    for (int i = 0; tab[i].sub; i++)
        if (strstr(cls, tab[i].sub)) return tab[i].kind;
    return VI_FILE;
}

static void draw_icon(Drawable dr, Pixmap pm, int size, int cx, int cy,
                      int boxw, int boxh) {
    int x = cx + (boxw - size) / 2, y = cy + (boxh - size) / 2;
    XCopyArea(dpy, pm, dr, bgc, 0, 0, size, size, x, y);
}

/* ------------------------------------------------------------- system tray */
static int tray_mapped = 0;

static void tray_dock(Window w) {
    if (!tray_ok || !w) return;
    if (ntray >= MAX_TRAY) return;
    char tray_cls[64] = "";
    win_class(w, tray_cls, sizeof tray_cls);
    /* SAS/lock remain available through Ctrl+Alt+Del and Win+L, but should not
       occupy a permanent taskbar status slot with a lock glyph. */
    if (strstr(tray_cls, "lock") || strstr(tray_cls, "sas")) return;
    for (int i = 0; i < ntray; i++)
        if (tray_wins[i] == w) return;               /* already docked */

    /* XEmbed handshake */
    long info[2] = { 0, 0 };                        /* version, flags */
    XChangeProperty(dpy, w, atom("_XEMBED_INFO"), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)info, 2);
    XSelectInput(dpy, w, StructureNotifyMask);
    XReparentWindow(dpy, w, win_tray, 4 + ntray * TRAY_SLOT, 6);
    tray_wins[ntray++] = w;
    XResizeWindow(dpy, w, TRAY_SLOT - 4, TRAY_SLOT - 4);
    tray_layout();
    XMapWindow(dpy, w);
    XSync(dpy, False);

    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = atom("_XEMBED");
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = (long)win_tray;
    ev.xclient.data.l[4] = 0;
    XSendEvent(dpy, w, False, NoEventMask, &ev);
    XFlush(dpy);
}

static void tray_remove(Window w) {
    for (int i = 0; i < ntray; i++)
        if (tray_wins[i] == w) {
            memmove(&tray_wins[i], &tray_wins[i + 1],
                    (size_t)(ntray - i - 1) * sizeof(Window));
            ntray--;
            tray_layout();
            return;
        }
}

static void tray_layout(void) {
    if (!win_tray) return;
    int w = ntray ? ntray * TRAY_SLOT : 0;
    int x = pill_r.x - w - 6;
    if (ntray) {
        XMoveResizeWindow(dpy, win_tray, x, 0, w, BAR_H);
        if (!tray_mapped) { XMapWindow(dpy, win_tray); tray_mapped = 1; }
    } else if (tray_mapped) {
        XUnmapWindow(dpy, win_tray);
        tray_mapped = 0;
    }
    for (int i = 0; i < ntray; i++)
        XMoveResizeWindow(dpy, tray_wins[i], 4 + i * TRAY_SLOT, 6,
                          TRAY_SLOT - 4, TRAY_SLOT - 4);
    draw_tray();
}

static void draw_tray(void) {
    if (!win_tray) return;
    fill(win_tray, bgc, 0, 0, ntray * TRAY_SLOT, BAR_H, cc_bar.pixel);
    XFlush(dpy);
}

static void tray_init(void) {
    win_tray = XCreateSimpleWindow(dpy, win_bar, 0, 0, 1, BAR_H, 0,
                                   cc_bar.pixel, cc_bar.pixel);
    XSelectInput(dpy, win_tray, ExposureMask | ButtonPressMask);
    XSetWindowAttributes sa = { 0 };
    sa.override_redirect = True;
    XChangeWindowAttributes(dpy, win_tray, CWOverrideRedirect, &sa);
    xd_tray = XftDrawCreate(dpy, win_tray, vis, cmap);
    (void)xd_tray;

    XSetSelectionOwner(dpy, atom("_NET_SYSTEM_TRAY_S0"), win_tray, CurrentTime);
    XSync(dpy, False);
    tray_ok = (XGetSelectionOwner(dpy, atom("_NET_SYSTEM_TRAY_S0")) == win_tray);
}

/* ------------------------------------------------------------ calendar  */
static int cal_ym_base(int *y, int *m) {
    time_t tn = time(NULL);
    struct tm tmv;
    localtime_r(&tn, &tmv);
    int yy = tmv.tm_year + 1900, mo = tmv.tm_mon + cal_off;
    int mmm = mo % 12;
    yy += mo / 12;
    if (mmm < 0) { mmm += 12; yy--; }
    *y = yy; *m = mmm;
    return tmv.tm_mday;
}

static int cal_days_in_month(int y, int m) {
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int dm = d[m];
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) dm = 29;
    return dm;
}

static int cal_first_dow(int y, int m) {
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m;
    tmv.tm_mday = 1;
    tmv.tm_hour = 12;
    time_t t2 = mktime(&tmv);
    localtime_r(&t2, &tmv);
    return tmv.tm_wday;
}

static void cal_hide(void) {
    if (cal_visible && win_cal) XUnmapWindow(dpy, win_cal);
    cal_visible = 0;
}

static void cal_toggle(void) {
    if (cal_visible) { cal_hide(); return; }
    cal_off = 0;
    XMoveResizeWindow(dpy, win_cal, scr_w - CAL_W - 6,
                      scr_h - BAR_H - CAL_H - 4, CAL_W, CAL_H);
    XMapRaised(dpy, win_cal);
    draw_cal();
    XFlush(dpy);
    cal_visible = 1;
}

static void draw_cal(void) {
    if (!win_cal) return;
    fill_round(win_cal, bgc, 4, 4, CAL_W - 8, CAL_H - 8, 10, cc_menu.pixel);
    int yy, mm, today;
    today = cal_ym_base(&yy, &mm);
    char hdr[64];
    static const char *mname[12] = { "一月","二月","三月","四月","五月","六月",
        "七月","八月","九月","十月","十一月","十二月" };
    snprintf(hdr, sizeof hdr, "%s %d", mname[mm], yy);
    draw_str(win_cal, xd_cal, f_bar, &cc_text, 32, 28, hdr);
    draw_str(win_cal, xd_cal, f_small, &cc_sub, 12, 28, "\xe2\x97\x80");
    draw_str(win_cal, xd_cal, f_small, &cc_sub, CAL_W - 22, 28, "\xe2\x96\xb6");

    static const char *wd[7] = { "日","一","二","三","四","五","六" };
    for (int i = 0; i < 7; i++) {
        draw_str_c(win_cal, xd_cal, f_small, &cc_sub,
                   8 + i * (CAL_W - 16) / 7, 36, (CAL_W - 16) / 7, 20, wd[i]);
    }
    int fw = cal_first_dow(yy, mm);
    int dm = cal_days_in_month(yy, mm);
    int ncell = fw + dm > 35 ? 42 : 35;
    for (int c = 0; c < ncell && c < 42; c++) {
        int day = c - fw + 1;
        int cx = 8 + (c % 7) * (CAL_W - 16) / 7;
        int cy = 56 + (c / 7) * 27;
        cal_days[c] = (RRect){ cx, cy, (CAL_W - 16) / 7, 25 };
        if (day < 1 || day > dm) continue;
        if (day == today && cal_off == 0)
            fill_round(win_cal, bgc, cx + 3, cy + 2, (CAL_W - 16) / 7 - 6,
                       21, 6, cc_accent.pixel);
        char db[4];
        snprintf(db, sizeof db, "%d", day);
        draw_str_c(win_cal, xd_cal, f_small, &cc_text, cx, cy,
                   (CAL_W - 16) / 7, 25, db);
    }
    cal_prev = (RRect){ 6, 14, 18, 18 };
    cal_next = (RRect){ CAL_W - 24, 14, 18, 18 };
    XFlush(dpy);
}

static void handle_cal_press(int x, int y) {
    if (in_rect(cal_prev, x, y))  { cal_off--; draw_cal(); return; }
    if (in_rect(cal_next, x, y))  { cal_off++; draw_cal(); return; }
    for (int c = 0; c < 42; c++)
        if (in_rect(cal_days[c], x, y)) { cal_hide(); return; }
    cal_hide();
}

/* -------------------------------------------------------------- taskbar */
static int task_total_w(void) { return ntask > 0 ? (46 + 3) * ntask : 0; }

/* Win11-style: [start][search] + running apps sit centered as one cluster */
static int left_cluster_x0(void) {
    int grp = 42 + 8 + 150 + (ntask > 0 ? 8 + task_total_w() : 0);
    int x0 = (scr_w - grp) / 2;
    if (x0 < 8) x0 = 8;
    int max = clock_r.x - 12 - grp;
    if (x0 > max) x0 = max;
    if (x0 < 8) x0 = 8;
    return x0;
}

static int bar_task_at(int x, int y) {
    if (ntask <= 0) return -1;
    int tw = 46, th = 34, gap = 3;
    int x0 = left_cluster_x0() + 50 + 150 + 8;
    int ty = (BAR_H - th) / 2;
    for (int i = 0; i < ntask; i++) {
        int bx = x0 + i * (tw + gap);
        if (x >= bx && x < bx + tw && y >= ty && y < ty + th) return i;
    }
    return -1;
}

/* ============================ system tray state =========================
 * Volume (pactl or amixer), network (sysfs), battery (sysfs). Polled every
 * 2 s from the main loop; the taskbar pill renders the results. */

/* --------------------- input method indicator (Win11) ------------------
 * Shows the active input source in the taskbar (like Win11's 中/ENG).
 * Backend 1: fcitx5/fcitx (common on Kali/Debian with CJK input).
 * Backend 2: XKB keyboard groups (layout switching). */

static int im_have_fcitx = -1;

static void im_state_init(void) {
    im_have_fcitx = (system("command -v fcitx5-remote >/dev/null 2>&1") == 0 ||
                     system("command -v fcitx-remote >/dev/null 2>&1") == 0) ? 1 : 0;
    XkbDescRec *kbd = XkbGetMap(dpy, 0, XkbUseCoreKbd);
    if (kbd) {
        im_ngroups = 1;
        if (XkbGetNames(dpy, XkbGroupNamesMask, kbd) == Success && kbd->names) {
            int n = 0;
            for (int g = 0; g < XkbNumKbdGroups && g < 4; g++)
                if (kbd->names->groups[g] != None) n++;
            if (n > 1) im_ngroups = n;
        }
        XkbFreeKeyboard(kbd, 0, True);
    }
    im_state_update();
}

static void im_state_update(void) {
    if (im_have_fcitx == 1) {
        FILE *p = popen("fcitx5-remote 2>/dev/null || fcitx-remote 2>/dev/null", "r");
        if (p) {
            char b[16] = "";
            if (fgets(b, sizeof b, p)) {
                int st = atoi(b);
                if (st >= 1) {
                    snprintf(im_label, sizeof im_label, "%s", st >= 2 ? "中" : "ENG");
                    pclose(p);
                    return;
                }
            }
            pclose(p);
        }
    }
    XkbStateRec st;
    if (XkbGetState(dpy, XkbUseCoreKbd, &st) == Success) {
        XkbDescRec *kbd = XkbGetMap(dpy, 0, XkbUseCoreKbd);
        if (kbd && XkbGetNames(dpy, XkbGroupNamesMask, kbd) == Success &&
            kbd->names && st.group < 4 && kbd->names->groups[st.group] != None) {
            char *nm = XGetAtomName(dpy, kbd->names->groups[st.group]);
            if (nm) {
                if (!strcasecmp(nm, "chinese") || strstr(nm, "中"))
                    snprintf(im_label, sizeof im_label, "%s", "中");
                else {
                    int k = 0;
                    for (int i = 0; nm[i] && k < 3; i++)
                        if ((nm[i] >= 'a' && nm[i] <= 'z') ||
                            (nm[i] >= 'A' && nm[i] <= 'Z'))
                            im_label[k++] = (char)toupper((unsigned char)nm[i]);
                    im_label[k] = 0;
                    if (!k) snprintf(im_label, sizeof im_label, "ENG");
                }
                XFree(nm);
            }
        }
        if (kbd) XkbFreeKeyboard(kbd, 0, True);
    }
}

static void im_click(void) {
    if (im_have_fcitx == 1) {
        launch_cmd("fcitx5-remote -t 2>/dev/null || fcitx-remote -t 2>/dev/null");
    } else if (im_ngroups > 1) {
        XkbStateRec st;
        if (XkbGetState(dpy, XkbUseCoreKbd, &st) == Success)
            XkbLockGroup(dpy, XkbUseCoreKbd, (st.group + 1) % im_ngroups);
    }
    im_state_update();
    bar_dirty = 1;
}

static char *run_line(const char *cmd) {
    static char buf[256];
    buf[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return buf;
    if (!fgets(buf, sizeof buf, p)) buf[0] = 0;
    pclose(p);
    return buf;
}

static void sys_state_update(void) {
    /* volume */
    if (access("/usr/bin/pactl", X_OK) == 0 ||
        access("/bin/pactl", X_OK) == 0) {
        char *l = run_line("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null");
        char *pc = strchr(l, '%');
        if (pc) {
            while (pc > l && (*pc < '0' || *pc > '9')) pc--;
            char *end = pc + 1;
            while (pc >= l && *pc >= '0' && *pc <= '9') pc--;
            vol_level = atoi(pc + 1);
            (void)end;
        }
        char *m = run_line("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null");
        vol_muted = strstr(m, "yes") != NULL;
    } else {
        char *s = run_line("amixer sget Master 2>/dev/null");
        char *pc = strchr(s, '%');
        if (pc) {
            while (pc > s && (*pc < '0' || *pc > '9')) pc--;
            while (pc >= s && *pc >= '0' && *pc <= '9') pc--;
            vol_level = atoi(pc + 1);
        }
        vol_muted = strstr(s, "[off]") != NULL;
    }
    if (vol_level < 0) vol_level = 0;
    if (vol_level > 100) vol_level = 100;

    /* network: wifi if any wireless iface is up, else any wired up iface */
    net_type = 0;
    DIR *nd = opendir("/sys/class/net");
    if (nd) {
        struct dirent *ne;
        int any_up = 0, wifi_up = 0;
        while ((ne = readdir(nd))) {
            if (ne->d_name[0] == '.') continue;
            if (!strcmp(ne->d_name, "lo")) continue;
            char p[512], st[64] = "";
            snprintf(p, sizeof p, "/sys/class/net/%s/operstate", ne->d_name);
            FILE *f = fopen(p, "r");
            if (f) { if (fgets(st, sizeof st, f)) st[strcspn(st, "\n")] = 0; fclose(f); }
            int up = !strcmp(st, "up") || !strcmp(st, "unknown");
            struct stat wsb;
            snprintf(p, sizeof p, "/sys/class/net/%s/wireless", ne->d_name);
            int wifi = (stat(p, &wsb) == 0);
            if (up) { any_up = 1; if (wifi) wifi_up = 1; }
        }
        closedir(nd);
        net_type = wifi_up ? 1 : (any_up ? 2 : 0);
    }

    /* battery */
    bat_present = 0;
    DIR *bd = opendir("/sys/class/power_supply");
    if (bd) {
        struct dirent *be;
        while ((be = readdir(bd))) {
            if (strncmp(be->d_name, "BAT", 3)) continue;
            char p[512], v[64] = "";
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", be->d_name);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            if (fgets(v, sizeof v, f)) bat_level = atoi(v);
            fclose(f);
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", be->d_name);
            f = fopen(p, "r");
            v[0] = 0;
            if (f) { if (fgets(v, sizeof v, f)) v[strcspn(v, "\n")] = 0; fclose(f); }
            bat_charging = !strcmp(v, "Charging") || !strcmp(v, "Full");
            bat_present = 1;
            break;
        }
        closedir(bd);
    }
    if (vol_visible) draw_vol_flyout();
    bar_dirty = 1;
}

static void set_volume_pct(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char cmd[160];
    if (access("/usr/bin/pactl", X_OK) == 0 || access("/bin/pactl", X_OK) == 0)
        snprintf(cmd, sizeof cmd, "pactl set-sink-volume @DEFAULT_SINK@ %d%%", pct);
    else
        snprintf(cmd, sizeof cmd, "amixer -q sset Master %d%%", pct);
    launch_cmd(cmd);
    vol_level = pct;
    if (pct > 0 && vol_muted) toggle_mute();
    if (vol_visible) draw_vol_flyout();
    bar_dirty = 1;
}

static void toggle_mute(void) {
    if (access("/usr/bin/pactl", X_OK) == 0 || access("/bin/pactl", X_OK) == 0)
        launch_cmd("pactl set-sink-mute @DEFAULT_SINK@ toggle");
    else
        launch_cmd("amixer -q sset Master toggle");
    vol_muted = !vol_muted;
    if (vol_visible) draw_vol_flyout();
    bar_dirty = 1;
}

/* ---------------- tray glyphs (vector, Win11 style) -------------------- */

static void draw_wifi_glyph(Drawable dr, int cx, int cy, int s,
                            unsigned long fg, int bars) {
    XSetForeground(dpy, bgc, fg);
    XSetLineAttributes(dpy, bgc, 2, LineSolid, CapRound, JoinRound);
    /* arcs radiating from the dot at the bottom */
    int dy = cy + s / 3;
    for (int i = 1; i <= 3; i++) {
        if (bars < i) break;
        int r = s / 6 + i * s / 5;
        XDrawArc(dpy, dr, bgc, cx - r, dy - r, 2 * r, 2 * r, 25 * 64, 130 * 64);
    }
    XFillArc(dpy, dr, bgc, cx - 2, dy - 2, 4, 4, 0, 360 * 64);
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
}

static void draw_wired_glyph(Drawable dr, int cx, int cy, int s, unsigned long fg) {
    XSetForeground(dpy, bgc, fg);
    XSetLineAttributes(dpy, bgc, 2, LineSolid, CapRound, JoinMiter);
    int w = s, h = s * 2 / 3;
    XDrawRectangle(dpy, dr, bgc, cx - w / 2, cy - h / 2 - 1, w, h);
    XDrawLine(dpy, dr, bgc, cx, cy + h / 2 - 1, cx, cy + h / 2 + 3);
    XDrawLine(dpy, dr, bgc, cx - w / 4, cy + h / 2 + 3, cx + w / 4, cy + h / 2 + 3);
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
}

static void draw_vol_glyph(Drawable dr, int cx, int cy, int s,
                           unsigned long fg, int level, int muted) {
    XSetForeground(dpy, bgc, fg);
    int bw = s / 3, bh = s / 3;            /* speaker body */
    int x0 = cx - s / 2;
    XFillRectangle(dpy, dr, bgc, x0, cy - bh / 2, bw, bh);
    XPoint horn[3] = {
        { (short)(x0 + bw), (short)(cy - bh / 2) },
        { (short)(x0 + bw + s / 4), (short)(cy - s / 2 + 1) },
        { (short)(x0 + bw + s / 4), (short)(cy + s / 2 - 1) },
    };
    XFillPolygon(dpy, dr, bgc, horn, 3, Convex, CoordModeOrigin);
    XFillRectangle(dpy, dr, bgc, x0 + bw, cy - bh / 2, s / 4, bh);
    int vx = x0 + bw + s / 4 + 2;
    if (muted) {
        XSetLineAttributes(dpy, bgc, 2, LineSolid, CapRound, JoinRound);
        XDrawLine(dpy, dr, bgc, vx + 1, cy - 4, vx + 8, cy + 4);
        XDrawLine(dpy, dr, bgc, vx + 8, cy - 4, vx + 1, cy + 4);
        XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
        return;
    }
    XSetLineAttributes(dpy, bgc, 2, LineSolid, CapRound, JoinRound);
    int waves = level > 66 ? 3 : level > 33 ? 2 : level > 0 ? 1 : 0;
    for (int i = 1; i <= waves; i++) {
        int r = 2 + i * 3;
        XDrawArc(dpy, dr, bgc, vx - r, cy - r, 2 * r, 2 * r, -45 * 64, 90 * 64);
    }
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
}

static void draw_bat_glyph(Drawable dr, int cx, int cy, int s,
                           unsigned long fg, int level, int charging) {
    XSetForeground(dpy, bgc, fg);
    int w = s + 4, h = s * 2 / 3;
    int x0 = cx - w / 2 - 1, y0 = cy - h / 2;
    XSetLineAttributes(dpy, bgc, 2, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(dpy, dr, bgc, x0, y0, w - 3, h);
    XFillRectangle(dpy, dr, bgc, x0 + w - 3, cy - 3, 3, 6);      /* nub */
    int fw = (w - 7) * level / 100;
    if (fw > 0) XFillRectangle(dpy, dr, bgc, x0 + 2, y0 + 2, fw, h - 4);
    if (charging) {
        XPoint bolt[4] = {
            { (short)(cx + 2), (short)(y0 - 2) },
            { (short)(cx - 3), (short)(cy + 1) },
            { (short)(cx + 0), (short)(cy + 1) },
            { (short)(cx - 2), (short)(y0 + h + 2) },
        };
        XSetForeground(dpy, bgc, cc_accent.pixel);
        XDrawLines(dpy, dr, bgc, bolt, 4, CoordModeOrigin);
    }
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
}

/* ------------------------- volume flyout ------------------------------- */

#define VOL_W 280
#define VOL_H 72
#define VOL_SLIDER_X 54
#define VOL_SLIDER_W (VOL_W - 54 - 58)

static void draw_vol_flyout(void) {
    if (!win_vol) return;
    fill_round(win_vol, bgc, 0, 0, VOL_W, VOL_H, 10, cc_menu.pixel);
    /* speaker icon (click = mute) */
    draw_vol_glyph(win_vol, 30, VOL_H / 2, 22, cc_text.pixel, vol_level, vol_muted);
    /* slider track */
    int sy = VOL_H / 2, sx = VOL_SLIDER_X, sw = VOL_SLIDER_W;
    XSetForeground(dpy, bgc, cc_task.pixel);
    XFillRectangle(dpy, win_vol, bgc, sx, sy - 2, sw, 4);
    int fw = sw * vol_level / 100;
    XSetForeground(dpy, bgc, cc_accent.pixel);
    XFillRectangle(dpy, win_vol, bgc, sx, sy - 2, fw, 4);
    XFillArc(dpy, win_vol, bgc, sx + fw - 6, sy - 6, 12, 12, 0, 360 * 64);
    /* percent text */
    char pc[16];
    snprintf(pc, sizeof pc, vol_muted ? "静音" : "%d%%", vol_level);
    draw_str(win_vol, xd_vol, f_bar, &cc_text,
             VOL_W - 50, VOL_H / 2 + f_bar->ascent / 2 - 1, pc);
    XFlush(dpy);
}

static void vol_show(void) {
    if (!win_vol) {
        win_vol = mk_owindow(0, 0, VOL_W, VOL_H);
        xd_vol = XftDrawCreate(dpy, win_vol, vis, cmap);
        XSelectInput(dpy, win_vol, ExposureMask | ButtonPressMask |
                                   ButtonReleaseMask | PointerMotionMask);
    }
    sys_state_update();
    int x = pill_r.x + pill_r.w / 2 - VOL_W / 2;
    if (x + VOL_W > scr_w - 8) x = scr_w - VOL_W - 8;
    if (x < 8) x = 8;
    int y = scr_h - BAR_H - VOL_H - 8;
    XMoveResizeWindow(dpy, win_vol, x, y, VOL_W, VOL_H);
    XMapRaised(dpy, win_vol);
    draw_vol_flyout();
    vol_visible = 1;
}

static void vol_hide(void) {
    if (vol_visible && win_vol) XUnmapWindow(dpy, win_vol);
    vol_visible = 0;
    vol_drag = 0;
}

static void vol_slider_from_x(int x) {
    int pct = (x - VOL_SLIDER_X) * 100 / VOL_SLIDER_W;
    set_volume_pct(pct);
}

static void handle_vol_press(int x, int y) {
    (void)y;
    if (x < VOL_SLIDER_X - 8) { toggle_mute(); return; }
    if (x >= VOL_SLIDER_X - 8 && x <= VOL_SLIDER_X + VOL_SLIDER_W + 8) {
        vol_drag = 1;
        vol_slider_from_x(x);
    }
}

static void handle_vol_motion(int x, int y) {
    (void)y;
    vol_hover = (x < VOL_SLIDER_X - 8);
    if (vol_drag) vol_slider_from_x(x);
}

static void draw_taskbar(void) {
    if (!win_bar) return;
    fill(win_bar, bgc, 0, 0, scr_w, BAR_H, cc_bar.pixel);

    int x0 = left_cluster_x0();
    start_r  = (RRect){ x0, (BAR_H - 36) / 2, 42, 36 };
    search_r = (RRect){ x0 + 50, (BAR_H - 36) / 2, 150, 36 };

    if (in_rect(start_r, pmx, pmy))
        fill_round(win_bar, bgc, start_r.x, start_r.y, start_r.w, start_r.h,
                   8, cc_hoverc.pixel);
    win_logo(win_bar, start_r.x + (start_r.w - 28) / 2,
             start_r.y + (start_r.h - 28) / 2, 28);

    fill_round(win_bar, bgc, search_r.x, search_r.y, search_r.w, search_r.h,
               16, cc_search.pixel);
    if (in_rect(search_r, pmx, pmy))
        fill_round(win_bar, bgc, search_r.x + 1, search_r.y + 1,
                   search_r.w - 2, search_r.h - 2, 15, cc_hoverc.pixel);
    draw_str(win_bar, xd_bar, f_bar, &cc_text,
             search_r.x + 12, search_r.y + search_r.h / 2 + f_bar->ascent / 2 - 1,
             "搜索");

    int tw = 46, th = 34, gap = 3;
    int tx0 = x0 + 50 + 150 + 8;
    int ty = (BAR_H - th) / 2;
    for (int i = 0; i < ntask; i++) {
        int bx = tx0 + i * (tw + gap);
        if (active_task == i) {
            fill_round(win_bar, bgc, bx, ty + th - 1, tw, 3, 2,
                       cc_accent.pixel);
        }
        unsigned long col = cc_task.pixel;
        if (i == bar_hover) col = cc_hoverc.pixel;
        fill_round(win_bar, bgc, bx, ty, tw, th, 4, col);
        Pixmap pm = icon_for_task(tasks[i].win, 24);
        char cls[64] = "";
        if (!pm) {
            win_class(tasks[i].win, cls, sizeof cls);
            if (cls[0])
                pm = icon_for_exact(app_icon_name(cls), 24, &cc_task);
        }
        if (pm) {
            draw_icon(win_bar, pm, 24, bx, ty - 1, tw, th);
        } else {
            draw_icon_kind(win_bar, app_icon_kind(cls), bx + tw / 2,
                           ty + th / 2 - 1, 20, cc_light.pixel);
        }
    }

    /* ---- right cluster: [tray icons][network][audio/battery][IME][clock][|desk] ---- */
    net_r = (RRect){ pill_r.x + 4, pill_r.y, 26, pill_r.h };
    if (in_rect(pill_r, pmx, pmy) && !in_rect(net_r, pmx, pmy))
        fill_round(win_bar, bgc, pill_r.x, pill_r.y, pill_r.w, pill_r.h,
                   8, cc_hoverc.pixel);
    if (in_rect(net_r, pmx, pmy))
        fill_round(win_bar, bgc, net_r.x, net_r.y, net_r.w, net_r.h,
                   8, cc_hoverc.pixel);
    int ic_y = BAR_H / 2;
    int slot = pill_r.x + 4;
    const int SLOT_W = 26;
    if (net_type == 1)      draw_wifi_glyph(win_bar, slot + SLOT_W / 2, ic_y, 18,
                                            cc_text.pixel, 3);
    else if (net_type == 2) draw_wired_glyph(win_bar, slot + SLOT_W / 2, ic_y, 16,
                                             cc_text.pixel);
    else                    draw_wifi_glyph(win_bar, slot + SLOT_W / 2, ic_y, 18,
                                            cc_sub.pixel, 0);
    slot += SLOT_W;
    draw_vol_glyph(win_bar, slot + SLOT_W / 2, ic_y, 18, cc_text.pixel,
                   vol_level, vol_muted);
    slot += SLOT_W;
    if (bat_present)
        draw_bat_glyph(win_bar, slot + SLOT_W / 2, ic_y, 16, cc_text.pixel,
                       bat_level, bat_charging);

    /* input method indicator (Win11 中/ENG), left of the clock */
    if (in_rect(im_r, pmx, pmy))
        fill_round(win_bar, bgc, im_r.x, im_r.y, im_r.w, im_r.h, 8,
                   cc_hoverc.pixel);
    {
        int iw = text_w(f_small, im_label);
        draw_str(win_bar, xd_bar, f_small, &cc_text,
                 im_r.x + (im_r.w - iw) / 2,
                 BAR_H / 2 + f_small->ascent / 2 - 1, im_label);
    }

    /* clock: time over date, right-aligned like Win11 */
    char t[32], d[32];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    strftime(t, sizeof t, "%H:%M", lt);
    snprintf(d, sizeof d, "%d/%d/%d", lt->tm_year + 1900, lt->tm_mon + 1,
             lt->tm_mday);
    if (in_rect(clock_r, pmx, pmy))
        fill_round(win_bar, bgc, clock_r.x, clock_r.y + 4, clock_r.w,
                   clock_r.h - 8, 8, cc_hoverc.pixel);
    int tw2 = text_w(f_small, t);
    int dw2 = text_w(f_small, d);
    int rw  = tw2 > dw2 ? tw2 : dw2;
    int rx  = clock_r.x + clock_r.w - 8 - rw;
    draw_str(win_bar, xd_bar, f_small, &cc_text, rx, BAR_H / 2 - 3, t);
    draw_str(win_bar, xd_bar, f_small, &cc_sub,
             clock_r.x + clock_r.w - 8 - dw2, BAR_H / 2 + 12, d);

    /* show-desktop strip */
    if (in_rect(edge_r, pmx, pmy))
        fill(win_bar, bgc, edge_r.x - 2, 0, edge_r.w + 2, BAR_H, cc_hoverc.pixel);
    XSetForeground(dpy, bgc, cc_light.pixel);
    XDrawLine(dpy, win_bar, bgc, edge_r.x, 6, edge_r.x, BAR_H - 6);
    XFlush(dpy);
}

/* ------------------------------------------------------------- desktop */
/*
 * The desktop (wallpaper + icons) is an ordinary window that Openbox manages
 * as the EWMH desktop window (_NET_WM_WINDOW_TYPE_DESKTOP). The WM stacks it
 * below every client window and resizes it for us, so we never fight the WM
 * for stacking order. We never select events on the ROOT window either:
 * Openbox already selects ButtonPress there, and X allows only one client to
 * select button/key events on a window -- a second selection raises BadAccess.
 * Owning the desktop window gives us reliable clicks and Expose redraws.
 */
static void paint_desktop(void) {
    if (!win_desk) return;
    if (wall_ok && wall_pm) {
        XCopyArea(dpy, wall_pm, win_desk, bgc, 0, 0, scr_w, scr_h - BAR_H, 0, 0);
        XFlush(dpy);
    } else {
        for (int i = 0; i < ngcol; i++) {
            int y = i * scr_h / ngcol;
            fill(win_desk, bgc, 0, y, scr_w, scr_h / ngcol + 1, gcol[i]);
        }
    }
    for (int i = 0; i < nic; i++) {
        if (i == sel_icon) {                 /* Win11 whole-cell selection box */
            /* eased pop-in, same feel as the Start-menu hover chip */
            /* A restrained opacity fade reads more like Windows 11 than a
             * bouncing card. It also avoids redraw jitter while selecting many
             * desktop icons on lower-end X11 systems. */
            float ease = sel_anim * (2.0f - sel_anim);
            int fw = ICON_W + 8, fh = ICON_H + 8;
            int fx = ic[i].x - 4, fy = ic[i].y - 4;
            desk_tint(fx, fy, fw, fh, 0xFFFFFF, 36 + (int)(26 * ease), 10);
            XSetForeground(dpy, bgc, cc_sel.pixel);
            XSetLineAttributes(dpy, bgc, 2, LineSolid, CapRound, JoinRound);
            int r = 10;
            XDrawArc(dpy, win_desk, bgc, fx + 1, fy + 1, 2 * r, 2 * r, 90 * 64, 90 * 64);
            XDrawArc(dpy, win_desk, bgc, fx + fw - 2 * r - 1, fy + 1, 2 * r, 2 * r, 0, 90 * 64);
            XDrawArc(dpy, win_desk, bgc, fx + 1, fy + fh - 2 * r - 1, 2 * r, 2 * r, 180 * 64, 90 * 64);
            XDrawArc(dpy, win_desk, bgc, fx + fw - 2 * r - 1, fy + fh - 2 * r - 1, 2 * r, 2 * r, 270 * 64, 90 * 64);
            XDrawLine(dpy, win_desk, bgc, fx + r + 1, fy + 1, fx + fw - r - 1, fy + 1);
            XDrawLine(dpy, win_desk, bgc, fx + r + 1, fy + fh - 1, fx + fw - r - 1, fy + fh - 1);
            XDrawLine(dpy, win_desk, bgc, fx + 1, fy + r + 1, fx + 1, fy + fh - r - 1);
            XDrawLine(dpy, win_desk, bgc, fx + fw - 1, fy + r + 1, fx + fw - 1, fy + fh - r - 1);
            XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
        }
        /* Win11 hover wash (under the selection ring when both apply) */
        if (i == hover_icon && i != sel_icon)
            desk_tint(ic[i].x - 4, ic[i].y - 4, ICON_W + 8, ICON_H + 8,
                      0xFFFFFF, 36, 10);
        const char *nm = ic[i].is_dir ? "folder" : file_icon_name(ic[i].label);
        int kind = ic[i].is_dir ? VI_FOLDER : VI_FILE;
        if (i == 0) { nm = "computer";               kind = VI_PC; }
        else if (i == 1) { nm = "user-home";         kind = VI_HOME; }
        /* true-alpha rendering over the wallpaper (no opaque icon boxes) */
        const char *ipath = theme_find(nm, 64);
        IconRGBA *ib = ipath ? icon_rgba_for_path(ipath, 64) : NULL;
        if (ib)
            desk_draw_icon_alpha(ib, ic[i].x + 16, ic[i].y + 2);
        else
            draw_icon_kind(win_desk, kind, ic[i].x + 48, ic[i].y + 34, 56,
                           cc_light.pixel);
        /* label: truncated with an ellipsis when too long, then drawn with
         * a soft dark halo so it stays readable on any wallpaper */
        char label_fit[96];
        fit_label(label_fit, sizeof label_fit, ic[i].label, f_small, ICON_W - 6);
        const int ly = ic[i].y + 74;
        draw_str_c(win_desk, xd_desk, f_small, &cc_lo, ic[i].x + 1, ly + 1,
                   ICON_W, 18, label_fit);
        draw_str_c(win_desk, xd_desk, f_small, &cc_lo, ic[i].x,     ly + 1,
                   ICON_W, 18, label_fit);
        draw_str_c(win_desk, xd_desk, f_small, &cc_lo, ic[i].x + 1, ly,
                   ICON_W, 18, label_fit);
        draw_str_c(win_desk, xd_desk, f_small, &cc_text, ic[i].x,   ly,
                   ICON_W, 18, label_fit);
    }
    XFlush(dpy);
}

static Window mk_desktop_window(int w, int h) {
    XSetWindowAttributes sa;
    memset(&sa, 0, sizeof sa);
    sa.background_pixel = cc_lo.pixel;
    sa.backing_store = Always;      /* server restores hidden regions itself */
    sa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask | EnterWindowMask | LeaveWindowMask;
    Window d = XCreateWindow(dpy, root, 0, 0, w, h, 0, CopyFromParent,
                             InputOutput, CopyFromParent,
                             CWBackPixel | CWBackingStore | CWEventMask, &sa);
    Atom type = atom("_NET_WM_WINDOW_TYPE_DESKTOP");
    XChangeProperty(dpy, d, atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&type, 1);
    Atom below = atom("_NET_WM_STATE_BELOW");
    XChangeProperty(dpy, d, atom("_NET_WM_STATE"), XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&below, 1);
    XStoreName(dpy, d, "ElevenDE Desktop");
    XSetWMProtocols(dpy, d, NULL, 0);
    return d;
}

/* saved icon positions (~/.config/elevende/icons2, path-tab-keyed) so dragged
 * icons stay where the user put them across refreshes and logins */
static int    icsv_x[MAX_ICONS], icsv_y[MAX_ICONS];
static char   icsv_path[MAX_ICONS][512];
static int    nicsv = 0;
static int    icsv_loaded = 0;

static const char *icons_pos_path(void) {
    static char buf[512];
    const char *home = getenv("HOME");
    if (home) snprintf(buf, sizeof buf, "%s/.config/elevende/icons2", home);
    else snprintf(buf, sizeof buf, "/tmp/elevende-icons2");
    return buf;
}

/* ensure ~/.config/elevende exists (icons2/pins2 live there) */
static void ensure_config_dir(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char d[512];
    snprintf(d, sizeof d, "%s/.config", home);
    mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/.config/elevende", home);
    mkdir(d, 0755);
}
static void icons_pos_load(void) {
    if (icsv_loaded) return;
    icsv_loaded = 1;
    nicsv = 0;
    FILE *f = fopen(icons_pos_path(), "r");
    if (!f) return;
    char ln[1024];
    while (fgets(ln, sizeof ln, f) && nicsv < MAX_ICONS) {
        char *x = strtok(ln, "\t");
        char *y = strtok(NULL, "\t");
        char *p = strtok(NULL, "\t");
        if (!x || !y || !p) continue;
        icsv_x[nicsv] = atoi(x);
        icsv_y[nicsv] = atoi(y);
        snprintf(icsv_path[nicsv], sizeof icsv_path[0], "%s", p);
        nicsv++;
    }
    fclose(f);
}
static void icons_pos_save(void) {
    ensure_config_dir();
    const char *p = icons_pos_path();
    char dir[512];
    snprintf(dir, sizeof dir, "%s", p);
    for (char *s = dir + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(dir, 0755); *s = '/'; }
    mkdir(dir, 0755);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nic; i++)
        fprintf(f, "%d\t%d\t%s\n", ic[i].x, ic[i].y, ic[i].path);
    fclose(f);
}
/* restore a saved position for each icon that has one */
static void icons_pos_apply(void) {
    icons_pos_load();
    for (int i = 0; i < nic; i++)
        for (int s = 0; s < nicsv; s++)
            if (!strcmp(ic[i].path, icsv_path[s])) {
                ic[i].x = icsv_x[s];
                ic[i].y = icsv_y[s];
                break;
            }
}

static void icon_layout(void) {
    int x = ICON_GX, y = ICON_GY;
    for (int i = 0; i < nic; i++) {
        ic[i].x = x;
        ic[i].y = y;
        y += ICON_CSY;
        if (y + ICON_H > scr_h - BAR_H - 20) { y = ICON_GY; x += ICON_CSX; }
    }
    icons_pos_apply();
}

static int is_interesting_fs(const char *fs) {
    return !strcmp(fs, "ext4") || !strcmp(fs, "ext3") || !strcmp(fs, "ext2") ||
           !strcmp(fs, "xfs") || !strcmp(fs, "btrfs") || !strcmp(fs, "f2fs") ||
           !strcmp(fs, "vfat") || !strcmp(fs, "ntfs") || !strcmp(fs, "exfat");
}

/* Map a file name to a freedesktop-style mimetype icon so desktop files
 * look like their Windows counterparts (zip folder, picture, media...). */
static const char *file_icon_name(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return "text-x-generic";
    dot++;
    char ext[16];
    size_t n = strlen(dot);
    if (n >= sizeof ext) return "text-x-generic";
    for (size_t i = 0; i <= n; i++)
        ext[i] = (char)tolower((unsigned char)dot[i]);
    static const struct { const char *e, *icon; } map[] = {
        { "zip",    "package-x-generic" }, { "rar",   "package-x-generic" },
        { "7z",     "package-x-generic" }, { "tar",   "package-x-generic" },
        { "gz",     "package-x-generic" }, { "bz2",   "package-x-generic" },
        { "xz",     "package-x-generic" }, { "deb",   "package-x-generic" },
        { "rpm",    "package-x-generic" }, { "cab",   "package-x-generic" },
        { "iso",    "media-optical" },
        { "png",    "image-x-generic" },   { "jpg",   "image-x-generic" },
        { "jpeg",   "image-x-generic" },   { "gif",   "image-x-generic" },
        { "bmp",    "image-x-generic" },   { "webp",  "image-x-generic" },
        { "svg",    "image-x-generic" },
        { "mp4",    "video-x-generic" },   { "mkv",   "video-x-generic" },
        { "avi",    "video-x-generic" },   { "mov",   "video-x-generic" },
        { "webm",   "video-x-generic" },
        { "mp3",    "audio-x-generic" },   { "flac",  "audio-x-generic" },
        { "wav",    "audio-x-generic" },   { "ogg",   "audio-x-generic" },
        { "m4a",    "audio-x-generic" },
        { "pdf",    "application-pdf" },
        { "doc",    "application-msword" },{ "docx",  "application-msword" },
        { "xls",    "x-office-spreadsheet" }, { "xlsx", "x-office-spreadsheet" },
        { "ppt",    "x-office-presentation" }, { "pptx", "x-office-presentation" },
        { "txt",    "text-x-generic" },    { "md",    "text-x-generic" },
        { "log",    "text-x-generic" },    { "ini",   "text-x-generic" },
        { "conf",   "text-x-generic" },    { "json",  "text-x-generic" },
        { "sh",     "text-x-script" },     { "py",    "text-x-script" },
        { "c",      "text-x-script" },     { "h",     "text-x-script" },
        { "cpp",    "text-x-script" },     { "js",    "text-x-script" },
        { "exe",    "application-x-executable" },
        { "desktop","application-x-executable" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(ext, map[i].e))
            return map[i].icon;
    return "text-x-generic";
}

static void gen_icons(void) {
    nic = 0;
    const char *home = getenv("HOME") ? getenv("HOME") : "/";

    snprintf(ic[nic].label, sizeof ic[nic].label, "此电脑");
    snprintf(ic[nic].path, sizeof ic[nic].path, "/");
    ic[nic++].is_dir = 1;

    snprintf(ic[nic].label, sizeof ic[nic].label, "主目录");
    snprintf(ic[nic].path, sizeof ic[nic].path, "%s", home);
    ic[nic++].is_dir = 1;

    FILE *f = fopen("/etc/mtab", "r");
    if (f) {
        char ln[1024];
        while (fgets(ln, sizeof ln, f) && nic < MAX_ICONS) {
            char dev[256], mp[256], ty[64];
            if (sscanf(ln, "%255s %255s %63s", dev, mp, ty) != 3) continue;
            if (!is_interesting_fs(ty)) continue;
            const char *base = strrchr(mp, '/');
            base = base ? base + 1 : mp;
            snprintf(ic[nic].label, sizeof ic[nic].label, "%s",
                     *base ? base : "本地磁盘");
            snprintf(ic[nic].path, sizeof ic[nic].path, "%s", mp);
            ic[nic++].is_dir = 1;
        }
        fclose(f);
    }

    char dir[512];
    snprintf(dir, sizeof dir, "%s/Desktop", home);
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL && nic < MAX_ICONS) {
            if (!de->d_name[0] || de->d_name[0] == '.') continue;
            snprintf(ic[nic].label, sizeof ic[nic].label, "%s", de->d_name);
            snprintf(ic[nic].path, sizeof ic[nic].path, "%s/%s", dir, de->d_name);
            ic[nic].is_dir = de->d_type == DT_DIR;
            nic++;
        }
        closedir(dp);
    }
}

/* --------------------------------------------------------------- launch */
static void launch_cmd(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* ------------------------------------------------ XDG autostart          */
static void autostart_scan(const char *dir) {
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (!de->d_name[0] || de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 9 || strcmp(de->d_name + len - 8, ".desktop")) continue;
        char path[1024], ex[192] = "";
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int skip = 0;
        char only_show[256] = "", not_show[256] = "", tryexe[256] = "";
        char ln[512];
        while (fgets(ln, sizeof ln, f)) {
            char *p = ln;
            while (*p == ' ' || *p == '\t') p++;
            if (!strncmp(p, "Hidden=", 7) && strstr(p + 7, "true")) skip = 1;
            else if (!strncmp(p, "OnlyShowIn=", 11))
                snprintf(only_show, sizeof only_show, "%s", p + 11);
            else if (!strncmp(p, "NotShowIn=", 10))
                snprintf(not_show, sizeof not_show, "%s", p + 10);
            else if (!strncmp(p, "TryExec=", 8))
                snprintf(tryexe, sizeof tryexe, "%s", p + 8);
            else if (!strncmp(p, "Exec=", 5) && !ex[0]) {
                snprintf(ex, sizeof ex, "%s", p + 5);
                size_t nl = strlen(ex);
                while (nl && (ex[nl-1] == '\n' || ex[nl-1] == '\r')) ex[--nl] = 0;
            }
        }
        fclose(f);
        /* EWMH autostart filtering: XFCE/GNOME-only helpers (xfce4-notifyd,
         * indicator applets...) used to start inside ElevenDE and dock
         * unexpected icons into the tray. Honor OnlyShowIn/NotShowIn. */
        char *nl2;
        if ((nl2 = strchr(only_show, '\n'))) *nl2 = 0;
        if ((nl2 = strchr(not_show, '\n'))) *nl2 = 0;
        if ((nl2 = strchr(tryexe, '\n'))) *nl2 = 0;
        if (only_show[0] && !strstr(only_show, "ElevenDE")) skip = 1;
        if (not_show[0] && strstr(not_show, "ElevenDE")) skip = 1;
        if (tryexe[0]) {
            char probe[300];
            snprintf(probe, sizeof probe, "command -v %s >/dev/null 2>&1", tryexe);
            if (system(probe) != 0) skip = 1;
        }
        if (skip || !ex[0]) continue;
        char cmd[256];
        exec_strip(cmd, ex);
        if (cmd[0]) launch_cmd(cmd);
    }
    closedir(dp);
}

static void autostart_run(void) {
    const char *home = getenv("HOME");
    if (home) {
        char d[512];
        snprintf(d, sizeof d, "%s/.config/autostart", home);
        autostart_scan(d);
    }
    autostart_scan("/etc/xdg/autostart");
}

static void open_path(const char *path) {
    char cmd[1024];
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(cmd, sizeof cmd, "explorer.exe '%s'", path);
    } else {
        /* files go to the DEFAULT application via xdg-open (MIME based).
         * The old "explorer.exe || xdg-open" never reached xdg-open because
         * explorer.exe happily "opens" any path -- which is why archives
         * and documents popped up in the file manager. */
        snprintf(cmd, sizeof cmd, "xdg-open '%s' || explorer.exe '%s'",
                 path, path);
    }
    launch_cmd(cmd);
    menu_hide();
}

/* --------------------------------------------------------------- app list */
static const char *ci_strstr(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (i == nl) return hay;
    }
    return NULL;
}

static void exec_strip(char *dst, const char *src) {
    int d = 0;
    for (int s = 0; src[s] && d < 90; s++) {
        if (src[s] == '%' && src[s + 1]) { s++; continue; }
        dst[d++] = src[s];
    }
    dst[d] = 0;
}

static void parse_desktop_dir(const char *dir) {
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (napps >= NAPP) break;
        if (!de->d_name[0] || de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 9 || strcmp(de->d_name + len - 8, ".desktop")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char nm[96] = "", zh[96] = "", ex[128] = "", ic[64] = "";
        int nodisplay = 0, hidden = 0, app_type = 0;
        char ln[1024];
        while (fgets(ln, sizeof ln, f)) {
            char *p = ln;
            while (*p == ' ' || *p == '\t') p++;
            if (!strncmp(p, "Type=", 5)) {
                if (!strncmp(p + 5, "Application", 11)) app_type = 1;
            } else if (!strncmp(p, "Name=", 5) && !nm[0]) {
                snprintf(nm, sizeof nm, "%s", p + 5);
            } else if (!strncmp(p, "Name[", 5) && strncmp(p, "Name[en", 7) &&
                       !zh[0]) {
                char *eq = strchr(p, '=');
                if (eq) snprintf(zh, sizeof zh, "%s", eq + 1);
            } else if (!strncmp(p, "Exec=", 5) && !ex[0]) {
                snprintf(ex, sizeof ex, "%s", p + 5);
            } else if (!strncmp(p, "Icon=", 5) && !ic[0]) {
                snprintf(ic, sizeof ic, "%s", p + 5);
            } else if (!strncmp(p, "NoDisplay=", 10)) {
                if (strstr(p + 10, "true")) nodisplay = 1;
            } else if (!strncmp(p, "Hidden=", 7)) {
                if (strstr(p + 7, "true")) hidden = 1;
            }
        }
        fclose(f);
        if (!app_type || nodisplay || hidden) continue;
        size_t nl = strlen(nm);
        while (nl && (nm[nl-1] == '\n' || nm[nl-1] == '\r')) nm[--nl] = 0;
        if (!nm[0] && zh[0]) snprintf(nm, sizeof nm, "%s", zh);
        if (!nm[0]) continue;
        nl = strlen(ex);
        while (nl && (ex[nl-1] == '\n' || ex[nl-1] == '\r')) ex[--nl] = 0;
        if (!ex[0]) continue;
        App *a = &apps[napps];
        snprintf(a->name, sizeof a->name, "%s", nm);
        exec_strip(a->exec, ex);
        nl = strlen(ic);
        while (nl && (ic[nl-1] == '\n' || ic[nl-1] == '\r')) ic[--nl] = 0;
        if (ic[0]) snprintf(a->icon, sizeof a->icon, "%s", ic);
        napps++;
    }
    closedir(dp);
}

static void load_apps(void) {
    napps = 0;
    const char *home = getenv("HOME");
    char dir[512];
    if (home) {
        snprintf(dir, sizeof dir, "%s/.local/share/applications", home);
        parse_desktop_dir(dir);
    }
    parse_desktop_dir("/usr/local/share/applications");
    parse_desktop_dir("/usr/share/applications");
    /* dedupe by exec (keep first occurrence) */
    for (int i = 0; i < napps; i++)
        for (int j = i + 1; j < napps; j++)
            if (!strcmp(apps[i].exec, apps[j].exec)) {
                memmove(&apps[j], &apps[j + 1],
                        (size_t)(napps - j - 1) * sizeof(App));
                napps--;
                j--;
            }
}

static void filter_apps(void) {
    nres = 0;
    for (int i = 0; i < napps && nres < NAPP; i++)
        if (!menu_filt[0] || ci_strstr(apps[i].name, menu_filt) ||
            ci_strstr(apps[i].exec, menu_filt))
            res_idx[nres++] = i;
    /* sort by name, a-z */
    for (int i = 1; i < nres; i++)
        for (int j = i; j > 0 &&
                    strcmp(apps[res_idx[j-1]].name, apps[res_idx[j]].name) > 0; j--) {
            int t = res_idx[j - 1]; res_idx[j-1] = res_idx[j]; res_idx[j] = t;
        }
    if (apps_scroll >= nres) apps_scroll = nres > 0 ? nres - 1 : 0;
    if (sel_row >= nres) sel_row = nres - 1;
}

static void launch_app(int idx) {
    if (idx < 0 || idx >= nres) return;
    char cmd[512];
    snprintf(cmd, sizeof cmd, "sh -c '%s'", apps[res_idx[idx]].exec);
    launch_cmd(cmd);
    menu_hide();
    search_hide();
}

static int list_rows_vis(void);
static int list_top_pins(void);

static void menu_filter_changed(void) {
    filter_apps();
    sel_row = 0;
    apps_scroll = 0;
    list_vis = list_rows_vis();
    draw_menu();
}

/* --------------------------------------------------------------- pinned */
typedef struct {
    char label[64];
    char cmd[224];
    char color[16];
    char icons[6][64];        /* themed icon names, best first ("",.. = none) */
    int  kind;                /* guaranteed vector-gloss fallback */
} Pinned;
static const Pinned pins_def[6] = {
    { "文件管理器", "explorer.exe", "#2f6db1",
      { "system-file-manager", "org.gnome.Nautilus", "nautilus", "folder",
        "drive-harddisk", "" }, VI_PC },
    { "终端", "xterm", "#4b5563",
      { "utilities-terminal", "xterm", "org.gnome.Terminal",
        "applications-utilities", "", "" }, VI_TERM },
    { "浏览器", "x-www-browser", "#f07822",
      { "web-browser", "firefox", "chromium", "www-browser",
        "internet-web-browser", "" }, VI_BROWSER },
    { "记事本", "elevende-notepad", "#6b7280",
      { "accessories-text-editor", "text-editor", "text-x-generic",
        "", "", "" }, VI_EDITOR },
    { "设置", "elevende-settings", "#8a8f98",
      { "preferences-system", "org.gnome.Settings", "gnome-control-center",
        "setup", "", "" }, VI_SETTINGS },
    { "主目录", "xdg-open $HOME", "#7c6fb2",
      { "user-home", "folder-home", "go-home", "home", "", "" }, VI_HOME },
};
static Pinned pins[NPIN_MAX];
static int    pin_visible[NPIN_MAX];
static int    npin_slots = 6;            /* number of configured slots      */
static int    npin_tiles = 0;            /* number of visible pinned tiles  */
static int    pin_slot[MAX_HOT];         /* menu hot index -> pin index     */
static const char *const pin_palette[] = {
    "#2f6db1", "#4b5563", "#f07822", "#6b7280", "#8a8f98", "#7c6fb2",
    "#3d8b40", "#c9423f", "#5b6ea8"
};
static int pin_pal_i = 6;

static void pin_reset_defaults(void) {
    npin_slots = NPIN_MAX;
    for (int i = 0; i < NPIN_MAX; i++) {
        if (i < 6) {
            snprintf(pins[i].label, sizeof pins[i].label, "%s", pins_def[i].label);
            snprintf(pins[i].cmd,   sizeof pins[i].cmd,   "%s", pins_def[i].cmd);
            snprintf(pins[i].color, sizeof pins[i].color, "%s", pins_def[i].color);
            pins[i].kind = pins_def[i].kind;
            for (int k = 0; k < 6; k++)
                snprintf(pins[i].icons[k], 64, "%s", pins_def[i].icons[k]);
        } else {
            snprintf(pins[i].color, sizeof pins[i].color, "%s",
                     pin_palette[i % (int)(sizeof pin_palette / sizeof *pin_palette)]);
        }
        pin_visible[i] = (i < 6) ? 1 : 0;
    }
    npin_slots = 6;
}

static void pin_colors_refresh(void) {
    for (int i = 0; i < NPIN_MAX; i++)
        pin_colors[i] = make_color(pins[i].color[0] ? pins[i].color : "#8a8f98");
}

static void pinmenu_hide(void);
static const char *pin_persist_path(void) {
    static char buf[512];
    const char *home = getenv("HOME");
    if (home) snprintf(buf, sizeof buf, "%s/.config/elevende/pins2", home);
    else snprintf(buf, sizeof buf, "/tmp/elevende-pins2");
    return buf;
}
static void pin_persist_save(void) {
    const char *p = pin_persist_path();
    /* create PARENT directories only -- the old code mkdir()'d the full path
     * including the file name, so ~/.config/elevende/pins2 ended up being a
     * DIRECTORY and the pins were never persisted (reset on every reboot) */
    char dir[512];
    snprintf(dir, sizeof dir, "%s", p);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        for (char *s = dir + 1; *s; s++)
            if (*s == '/') { *s = 0; mkdir(dir, 0755); *s = '/'; }
        mkdir(dir, 0755);
    }
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d\n", npin_slots);
    for (int i = 0; i < npin_slots; i++) {
        fprintf(f, "%d\t%s\t%s\t%d\t%s", pin_visible[i], pins[i].label,
                pins[i].cmd, pins[i].kind, pins[i].color);
        for (int k = 0; k < 6; k++) fprintf(f, "\t%s", pins[i].icons[k]);
        fputc('\n', f);
    }
    fclose(f);
}
static void pin_persist_load(void) {
    FILE *f = fopen(pin_persist_path(), "r");
    if (!f) return;
    int cnt = 0;
    if (fscanf(f, "%d\n", &cnt) != 1) { fclose(f); return; }
    if (cnt < 1 || cnt > NPIN_MAX) cnt = NPIN_MAX;
    for (int i = 0; i < cnt; i++) {
        char ln[2048];
        if (!fgets(ln, sizeof ln, f)) break;
        char *vis = strtok(ln, "\t");
        char *lab = strtok(NULL, "\t");
        char *cmd = strtok(NULL, "\t");
        char *kin = strtok(NULL, "\t");
        char *col = strtok(NULL, "\t");
        if (!vis || !lab || !cmd || !kin || !col) break;
        pin_visible[i] = atoi(vis);
        snprintf(pins[i].label, sizeof pins[i].label, "%s", lab);
        /* migrate obsolete placeholder commands from older installs */
        if      (!strcmp(cmd, "xterm -e 'echo settings'")) cmd = "elevende-settings";
        else if (!strcmp(cmd, "gedit"))                    cmd = "elevende-notepad";
        else if (!strcmp(cmd, "xdg-open https://www.kali.org/")) cmd = "x-www-browser";
        snprintf(pins[i].cmd,   sizeof pins[i].cmd,   "%s", cmd);
        pins[i].kind = atoi(kin);
        snprintf(pins[i].color, sizeof pins[i].color, "%s", col);
        for (int k = 0; k < 6; k++) {
            char *ic = strtok(NULL, "\t");
            if (ic) snprintf(pins[i].icons[k], 64, "%s", ic);
            else pins[i].icons[k][0] = 0;
        }
    }
    npin_slots = cnt;
    fclose(f);
}

/* pin an application (by app-list index res_idx[ai]) to the Start menu */
static int pin_app_add(int ai) {
    if (ai < 0 || ai >= nres) return -1;
    App *a = &apps[res_idx[ai]];
    for (int i = 0; i < npin_slots; i++)
        if (pin_visible[i] && !strcmp(pins[i].cmd, a->exec))
            return i;                       /* already pinned */
    int slot = -1;
    for (int i = 0; i < npin_slots && slot < 0; i++)
        if (!pin_visible[i]) slot = i;
    if (slot < 0 && npin_slots < NPIN_MAX) { slot = npin_slots; npin_slots++; }
    if (slot < 0) return -1;
    snprintf(pins[slot].label, sizeof pins[slot].label, "%s", a->name);
    snprintf(pins[slot].cmd,   sizeof pins[slot].cmd,   "%s", a->exec);
    snprintf(pins[slot].color, sizeof pins[slot].color, "%s",
             pin_palette[pin_pal_i++ % (int)(sizeof pin_palette / sizeof *pin_palette)]);
    pins[slot].kind = app_icon_kind(a->exec);
    int k = 0;
    if (a->icon[0]) snprintf(pins[slot].icons[k++], 64, "%s", a->icon);
    const char *derived = app_icon_name(a->exec);
    if (derived && derived[0]) snprintf(pins[slot].icons[k++], 64, "%s", derived);
    static const struct { int kind; const char *nm; } fb[] = {
        { VI_BROWSER, "web-browser" }, { VI_TERM, "utilities-terminal" },
        { VI_EDITOR, "accessories-text-editor" }, { VI_SETTINGS, "preferences-system" },
        { VI_PC, "computer" }, { VI_HOME, "user-home" },
        { VI_FOLDER, "folder" }, { VI_FILE, "text-x-generic" },
    };
    for (unsigned f = 0; f < sizeof fb / sizeof *fb && k < 6; f++)
        if (fb[f].kind == pins[slot].kind)
            snprintf(pins[slot].icons[k++], 64, "%s", fb[f].nm);
    while (k < 6) pins[slot].icons[k++][0] = 0;
    pin_visible[slot] = 1;
    pin_persist_save();
    return slot;
}

/* is app-list slot ai currently pinned? */
static int pin_app_pinned(int ai) {
    if (ai < 0 || ai >= nres) return 0;
    for (int i = 0; i < npin_slots; i++)
        if (pin_visible[i] && !strcmp(pins[i].cmd, apps[res_idx[ai]].exec))
            return 1;
    return 0;
}

/* unpin the pin that runs exec */
static void pin_unpin_by_exec(const char *exec) {
    for (int i = 0; i < npin_slots; i++)
        if (pin_visible[i] && !strcmp(pins[i].cmd, exec)) {
            pin_visible[i] = 0;
            pin_persist_save();
            return;
        }
}

/* build the icon-name pointer list for pin i (icon_for_names skips NULL) */
static void pin_icon_names(int i, const char **out) {
    for (int k = 0; k < 6; k++)
        out[k] = pins[i].icons[k][0] ? pins[i].icons[k] : NULL;
}

/* ----------------------------------------------------------------- menu */
static RRect cur_list_rect(void) {
    int ty = menu_filt[0] ? LIST_TOP_FILTER : list_top_pins();
    return (RRect){ 24, ty, MENU_W - 48, LIST_BOTTOM - ty };
}

static int list_rows_vis(void) { return cur_list_rect().h / ROW_H; }

/* 6-wide x 2-row compact pinned grid; 12 pins fill two Win11-sized rows and
   the "所有应用" list always sits at the fixed top */
static int list_top_pins(void) { return LIST_TOP_PINNED; }

static void power_hide(void) {
    if (power_visible && win_power) XUnmapWindow(dpy, win_power);
    power_visible = 0;
    power_hover = -1;
}

static void menu_hide(void) {
    power_hide();
    pinmenu_hide();
    ctx_hide();
    if (menu_visible && win_menu) XUnmapWindow(dpy, win_menu);
    menu_visible = 0;
    search_focus = 0;
    menu_tile_idx = -1;
    menu_tile_prev = -1;
    menu_hover_row = -1;
    menu_power_hover = 0;
    tile_anim_act = 0;
}

#define POWER_W   200
#define POWER_ROW 40
#define POWER_H   (12 + 4 * POWER_ROW)

static unsigned long px2(const char *hex) {
    XColor c;
    if (XParseColor(dpy, cmap, hex, &c) && XAllocColor(dpy, cmap, &c))
        return c.pixel;
    return 0;
}

static void show_power(void) {
    power_hide();
    int w = POWER_W, h = POWER_H;
    /* place the flyout to the RIGHT of the Start menu (no overlap at all),
       vertically aligned with the power button */
    int x = menu_x + MENU_W + 6;
    int y = menu_y + MENU_H - h - 4;
    if (x + w > scr_w) x = scr_w - w - 8;
    XMoveResizeWindow(dpy, win_power, x, y, w, h);
    npower_hot = 0;
    for (int i = 0; i < 4; i++) {
        power_hot[npower_hot] = (RRect){ 6, 6 + i * POWER_ROW,
                                         POWER_W - 12, POWER_ROW };
        npower_hot++;
    }
    XMapRaised(dpy, win_power);
    power_hover = -1;
    draw_power();
    XFlush(dpy);
    power_visible = 1;
}

/* crescent moon (sleep) */
static void draw_moon(Drawable dr, int cx, int cy, int s, unsigned long fg) {
    XSetForeground(dpy, bgc, fg);
    XFillArc(dpy, dr, bgc, cx - s / 2, cy - s / 2, s, s, 0, 360 * 64);
    XSetForeground(dpy, bgc, cc_menu.pixel);
    XFillArc(dpy, dr, bgc, cx - s / 2 + s / 3, cy - s / 2 - s / 6, s, s, 0, 360 * 64);
}

/* circular arrow (restart) */
static void draw_restart_icon(Drawable dr, int cx, int cy, int s, unsigned long fg) {
    int th = s / 7; if (th < 2) th = 2;
    XSetForeground(dpy, bgc, fg);
    XSetLineAttributes(dpy, bgc, th, LineSolid, CapRound, JoinRound);
    int r = s / 2 - th;
    XDrawArc(dpy, dr, bgc, cx - r, cy - r, 2 * r, 2 * r, 300 * 64, 240 * 64);
    /* arrowhead at the start of the arc (upper right) */
    XPoint tri[3] = {
        { (short)(cx + r - th),     (short)(cy - th * 2) },
        { (short)(cx + r + th * 2), (short)(cy - th) },
        { (short)(cx + r - th),     (short)(cy + th) },
    };
    XFillPolygon(dpy, dr, bgc, tri, 3, Convex, CoordModeOrigin);
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
}

/* door with outgoing arrow (log out) */
static void draw_logout_icon(Drawable dr, int cx, int cy, int s, unsigned long fg) {
    int th = s / 8; if (th < 2) th = 2;
    XSetForeground(dpy, bgc, fg);
    XSetLineAttributes(dpy, bgc, th, LineSolid, CapRound, JoinRound);
    int hw = s / 2, hh = s / 2;
    /* door frame open on the right side */
    XDrawLine(dpy, dr, bgc, cx - hw / 2, cy - hh, cx + hw / 4, cy - hh);
    XDrawLine(dpy, dr, bgc, cx - hw / 2, cy + hh, cx + hw / 4, cy + hh);
    XDrawLine(dpy, dr, bgc, cx - hw / 2, cy - hh, cx - hw / 2, cy + hh);
    /* arrow going out to the right */
    XDrawLine(dpy, dr, bgc, cx - hw / 4, cy, cx + hw, cy);
    XDrawLine(dpy, dr, bgc, cx + hw, cy, cx + hw - th * 2, cy - th * 2);
    XDrawLine(dpy, dr, bgc, cx + hw, cy, cx + hw - th * 2, cy + th * 2);
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
}

static void draw_power(void) {
    if (!win_power) return;
    /* same look as the SAS secure-screen power menu:
     * dark blue card rgba(24,32,48), 1px light border, 8px radius,
     * 40px rows with rgba(255,255,255,12%) hover */
    fill_round(win_power, bgc, 0, 0, POWER_W, POWER_H, 8, px2("#182030"));
    XSetForeground(dpy, bgc, px2("#3b4251"));
    XSetLineAttributes(dpy, bgc, 1, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(dpy, win_power, bgc, 0, 0, POWER_W - 1, POWER_H - 1);
    static const char *items[4] = { "睡眠", "关机", "重启", "注销" };
    for (int i = 0; i < 4; i++) {
        const int row_y = 6 + i * POWER_ROW, row_h = POWER_ROW;
        if (power_hover == i)
            fill_round(win_power, bgc, 6, row_y, POWER_W - 12, row_h - 2, 6,
                       px2("#333b49"));
        const int gcx = 30, gcy = row_y + row_h / 2;
        if      (i == 0) draw_moon(win_power, gcx, gcy, 18, cc_text.pixel);
        else if (i == 1) draw_power_symbol(win_power, gcx, gcy, 18, cc_text.pixel);
        else if (i == 2) draw_restart_icon(win_power, gcx, gcy, 18, cc_text.pixel);
        else             draw_logout_icon(win_power, gcx, gcy, 18, cc_text.pixel);
        const int base_y = row_y + (row_h + f_small->ascent - f_small->descent) / 2;
        draw_str(win_power, xd_power, f_small, &cc_text, 52, base_y, items[i]);
    }
}

static void handle_power_press(int x, int y) {
    for (int i = 0; i < npower_hot; i++)
        if (in_rect(power_hot[i], x, y)) {
            if      (i == 0) { launch_cmd("systemctl suspend");  menu_hide(); }
            else if (i == 1) { launch_cmd("systemctl poweroff"); menu_hide(); }
            else if (i == 2) { launch_cmd("systemctl reboot");   menu_hide(); }
            else if (i == 3) { launch_cmd("openbox --exit");     menu_hide(); }
            return;
        }
    power_hide();
}

static void pinmenu_show(int pin, int ax, int ay) {
    pinm_pin = pin;
    pinm_hover = -1;
    int w = PINM_W, h = PINM_H;
    int x = ax, y = ay + 4;
    if (x + w > scr_w) x = scr_w - w - 4;
    if (y + h > scr_h) y = ay - h - 4;
    pinm_x = x;
    pinm_y = y;
    XMoveResizeWindow(dpy, win_pinm, x, y, w, h);
    pinm_hot[0] = (RRect){ 0, 2, w, 36 };
    pinm_hot[1] = (RRect){ 0, 40, w, 36 };
    XMapRaised(dpy, win_pinm);
    draw_pinmenu();
    XFlush(dpy);
    pinm_visible = 1;
}

static void pinmenu_hide(void) {
    if (pinm_visible && win_pinm) XUnmapWindow(dpy, win_pinm);
    pinm_visible = 0;
    pinm_pin = -1;
}

static void draw_pinmenu(void) {
    if (!win_pinm) return;
    fill_round(win_pinm, bgc, 0, 0, PINM_W, PINM_H, 8, cc_menu.pixel);
    const char *items[2] = { "打开", "取消固定" };
    for (int i = 0; i < 2; i++) {
        int ry = i * 38 + 2;
        if (pinm_hover == i)
            fill_round(win_pinm, bgc, 8, ry, PINM_W - 16, 36, 6,
                       cc_hoverc.pixel);
        draw_str(win_pinm, xd_pinm, f_small, &cc_text, 16,
                 ry + 18 + f_small->ascent / 2, items[i]);
    }
}

static void handle_pinmenu_press(int x, int y) {
    for (int i = 0; i < 2; i++)
        if (in_rect(pinm_hot[i], x, y)) {
            if (i == 0 && pinm_pin >= 0 && pinm_pin < npin_slots) {
                launch_cmd(pins[pinm_pin].cmd);
                menu_hide();
            } else if (i == 1 && pinm_pin >= 0 && pinm_pin < npin_slots) {
                pin_visible[pinm_pin] = 0;
                pin_persist_save();
                pinmenu_hide();
                menu_refresh();
            }
            return;
        }
    pinmenu_hide();
}

/* ------------------------------------------------------- generic context menu */
static void ctx_show(const char *const items[], int n, int ax, int ay,
                     void (*cb)(int idx, void *ud), void *ud) {
    if (n > CTX_MAX) n = CTX_MAX;
    ctx_n = n;
    ctx_cb = cb;
    ctx_ud = ud;
    ctx_hover = -1;
    int w = CTX_W, h = n * CTX_ROW + 8;
    int x = ax + 2, y = ay + 2;
    if (x + w > scr_w) x = scr_w - w - 4;
    if (x < 0) x = 0;
    if (y + h > scr_h - BAR_H) y = scr_h - BAR_H - h - 4;  /* never under taskbar */
    if (y < 0) y = 0;
    ctx_x = x;
    ctx_y = y;
    ctx_h = h;
    for (int i = 0; i < n; i++) {
        ctx_items[i] = items[i];
        ctx_hot[i] = (RRect){ 0, 4 + i * CTX_ROW, w, CTX_ROW - 2 };
    }
    XMoveResizeWindow(dpy, win_ctx, x, y, w, h);
    XMapRaised(dpy, win_ctx);
    draw_ctx();
    XFlush(dpy);
    ctx_visible = 1;
}

static void ctx_hide(void) {
    if (ctx_visible && win_ctx) XUnmapWindow(dpy, win_ctx);
    ctx_visible = 0;
    ctx_cb = NULL;
    ctx_ud = NULL;
    ctx_hover = -1;
}

static void draw_ctx(void) {
    if (!win_ctx) return;
    fill_round(win_ctx, bgc, 0, 0, CTX_W, ctx_h, 8, cc_menu.pixel);
    for (int i = 0; i < ctx_n; i++) {
        int ry = 4 + i * CTX_ROW;
        if (ctx_hover == i)
            fill_round(win_ctx, bgc, 6, ry + 1, CTX_W - 12, CTX_ROW - 4, 6,
                       cc_hoverc.pixel);
        draw_str(win_ctx, xd_ctx, f_small, &cc_text, 16,
                 ry + CTX_ROW / 2 + f_small->ascent / 2 - 1, ctx_items[i]);
    }
}

static void handle_ctx_press(int x, int y) {
    int idx = -1;
    for (int i = 0; i < ctx_n; i++)
        if (in_rect(ctx_hot[i], x, y)) { idx = i; break; }
    void (*cb)(int, void *) = ctx_cb;
    void *ud = ctx_ud;
    ctx_hide();
    if (cb && idx >= 0) cb(idx, ud);
}
static const char *desk_dir(void) {
    static char buf[512];
    snprintf(buf, sizeof buf, "%s/Desktop", getenv("HOME") ? getenv("HOME") : "/");
    return buf;
}

static int desk_item(const char *path) {
    return !strncmp(path, desk_dir(), strlen(desk_dir())) &&
           path[strlen(desk_dir())] == '/';
}

static void rm_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (!de->d_name[0] || de->d_name[0] == '.') continue;
                char c[1024];
                snprintf(c, sizeof c, "%s/%s", p, de->d_name);
                rm_tree(c);
            }
            closedir(d);
        }
        rmdir(p);
    } else remove(p);
}

static void icons_reload(void) {
    gen_icons();
    icon_layout();
    struct stat st;
    if (stat(desk_dir(), &st) == 0) {
        desk_mtime = st.st_mtime;
        desk_mtime_nsec = st.st_mtim.tv_nsec;
        desk_size = st.st_size;
    }
    desk_dirty = 1;
}



static int is_image_file(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    static const char *exts[] = { ".png", ".jpg", ".jpeg", ".bmp",
                                  ".webp", ".gif" };
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++)
        if (!strcasecmp(dot, exts[i])) return 1;
    return 0;
}

static void set_wallpaper_file(const char *path) {
    const char *home = getenv("HOME");
    if (!home) return;
    char dir[1024], dst[1024], cmd[1600];
    snprintf(dir, sizeof dir, "%s/.local/share/elevende", home);
    mkdir(dir, 0755);
    snprintf(dst, sizeof dst, "%s/wallpaper.png", dir);
    /* copy then tell the shell (ourselves) to reload; notify the user */
    snprintf(cmd, sizeof cmd, "cp -f '%s' '%s'", path, dst);
    if (system(cmd) == 0) {
        wall_ok = 0;
        wallpaper_init();
        desk_dirty = 1;
        char ncmd[1900];
        snprintf(ncmd, sizeof ncmd,
                 "notify-send 'ElevenDE' '已更换桌面壁纸' 2>/dev/null");
        launch_cmd(ncmd);
    }
}

static void dm_show(int ax, int ay, int icon) {
    dm_icon = icon;
    dm_hover = -1;
    dm_n = 6;
    if (icon >= 0) {
        dm_n = 1;                                   /* 打开 */
        if (!ic[icon].is_dir && is_image_file(ic[icon].path)) dm_n++;  /* 设为壁纸 */
        if (desk_item(ic[icon].path)) dm_n++;       /* 删除 */
    }
    int w = DM_W, h = dm_n * DM_ROW + 8;
    int x = ax + 4, y = ay + 4;
    if (x + w > scr_w) x = scr_w - w - 4;
    if (y + h > scr_h - BAR_H) y = scr_h - BAR_H - h - 4;
    dm_x = x;
    dm_y = y;
    dm_h = h;
    for (int i = 0; i < dm_n; i++)
        dm_hot[i] = (RRect){ 0, 4 + i * DM_ROW, w, DM_ROW - 2 };
    XMoveResizeWindow(dpy, win_dm, x, y, w, h);
    XMapRaised(dpy, win_dm);
    draw_dm();
    XFlush(dpy);
    dm_visible = 1;
}

static void dm_hide(void) {
    if (dm_visible && win_dm) XUnmapWindow(dpy, win_dm);
    dm_visible = 0;
    dm_icon = -1;
    dm_hover = -1;
}

static void draw_dm(void) {
    if (!win_dm) return;
    fill_round(win_dm, bgc, 0, 0, DM_W, dm_h, 8, cc_menu.pixel);
    static const char *items_blank[] = { "新建文件夹", "新建文本文档", "打开终端",
                                         "打开文件管理器", "刷新", "设置壁纸" };
    static const char *items_icon[3];
    if (dm_icon >= 0) {
        int k = 0;
        items_icon[k++] = "打开";
        if (!ic[dm_icon].is_dir && is_image_file(ic[dm_icon].path))
            items_icon[k++] = "设为桌面背景";
        if (desk_item(ic[dm_icon].path))
            items_icon[k++] = "删除";
    }
    const char **items = (dm_icon >= 0) ? items_icon : items_blank;
    for (int i = 0; i < dm_n; i++) {
        int ry = 4 + i * DM_ROW;
        if (dm_hover == i)
            fill_round(win_dm, bgc, 6, ry + 1, DM_W - 12, DM_ROW - 4, 6,
                       cc_hoverc.pixel);
        draw_str(win_dm, xd_dm, f_small, &cc_text, 16,
                 ry + DM_ROW / 2 + f_small->ascent / 2 - 1, items[i]);
    }
}

static void handle_dm_press(int x, int y) {
    int idx = -1;
    for (int i = 0; i < dm_n; i++)
        if (in_rect(dm_hot[i], x, y)) { idx = i; break; }
    int icon = dm_icon;
    dm_hide();
    if (idx < 0) return;

    if (icon >= 0) {
        int has_bg = !ic[icon].is_dir && is_image_file(ic[icon].path);
        int has_del = desk_item(ic[icon].path);
        if (idx == 0) open_path(ic[icon].path);
        else if (has_bg && idx == 1) set_wallpaper_file(ic[icon].path);
        else if ((has_bg ? idx == 2 : idx == 1) && has_del) {
            rm_tree(ic[icon].path);
            if (access(ic[icon].path, F_OK) == 0) {
                char cmd[1200];                    /* fallback for sticky dirs */
                snprintf(cmd, sizeof cmd, "rm -rf -- '%s'", ic[icon].path);
                launch_cmd(cmd);
            }
            icons_reload();
        }
        return;
    }
    switch (idx) {
    case 0: {                                    /* new folder */
        char p[1024];
        for (int i = 1; i <= 100; i++) {
            snprintf(p, sizeof p, "%s/新建文件夹%d", desk_dir(), i);
            if (mkdir(p, 0755) == 0) break;
        }
        icons_reload();
        break;
    }
    case 1: {                                    /* new text file */
        char p[1024];
        for (int i = 1; i <= 100; i++) {
            snprintf(p, sizeof p, "%s/新建文档%d.txt", desk_dir(), i);
            FILE *f = fopen(p, "r");
            if (!f) {
                f = fopen(p, "w");
                if (f) fclose(f);
                break;
            }
            fclose(f);
        }
        icons_reload();
        break;
    }
    case 2: launch_cmd("xterm"); break;
    case 3: open_path(desk_dir()); break;
    case 4: icons_reload(); break;
    case 5:                                      /* 个性化(壁纸) -> 设置页 */
        launch_cmd("elevende-settings --page personalization");
        break;
    }
}

static void menu_build_geometry(void) {
    nmenu_hot = 0;
    /* search pill (index 0) */
    menu_hot[nmenu_hot] = (RRect){ 20, 20, MENU_W - 40, 40 };
    menu_cmd[nmenu_hot++] = NULL;
    npin_tiles = 0;
    for (int i = 0; i < npin_slots; i++) {
        if (!pin_visible[i]) continue;
        int col = npin_tiles % PIN_COLS, row = npin_tiles / PIN_COLS;
        int fx = 24 + col * PIN_CELL, ty = PIN_TOP + row * PIN_ROWY;
        /* full responsive cell (icon + label), the hover/click hit region */
        menu_hot[nmenu_hot] = (RRect){ fx, ty, PIN_CELL, PIN_TSZ + 18 };
        menu_cmd[nmenu_hot] = pins[i].cmd;
        pin_slot[nmenu_hot] = i;
        nmenu_hot++;
        npin_tiles++;
    }
    /* power button (last hot slot, after the visible pinned tiles) */
    menu_hot[nmenu_hot] = (RRect){ MENU_W - 76, MENU_H - 66, 52, 52 };
    menu_cmd[nmenu_hot++] = NULL;

    list_r = cur_list_rect();
    list_vis = list_rows_vis();
}

/* re-layout the open Start menu after a pin/unpin without re-running the
 * open animation or stealing focus (the old behaviour visibly flickered) */
static void menu_refresh(void) {
    if (!menu_visible) return;
    menu_build_geometry();
    draw_menu();
    XFlush(dpy);
}

static void menu_show(void) {
    if (menu_visible) { draw_menu(); return; }
    cal_hide();
    search_hide();
    dm_hide();
    pmx = pmy = -1;
    menu_filt[0] = 0;
    search_focus = 0;
    filter_apps();
    menu_x = (scr_w - MENU_W) / 2;
    menu_y = scr_h - BAR_H - MENU_H - 4;
    XMoveResizeWindow(dpy, win_menu, menu_x, menu_y, MENU_W, MENU_H);

    menu_build_geometry();

    /* Win11-style slide-up: open just above the taskbar and glide to the
     * final position (ease-out). All hit regions are window-local, so the
     * hover geometry stays exact while the window itself moves. */
    int y0 = menu_y + 70;
    XMoveResizeWindow(dpy, win_menu, menu_x, y0, MENU_W, MENU_H);
    XMapRaised(dpy, win_menu);
    XSetInputFocus(dpy, win_menu, RevertToPointerRoot, CurrentTime);
    menu_open_act = g_active;
    menu_visible = 1;
    draw_menu();
    XFlush(dpy);
    for (int f = 1; f <= 7; f++) {
        double t = f / 7.0;
        double e = 1.0 - (1.0 - t) * (1.0 - t);   /* ease-out quadratic */
        XMoveResizeWindow(dpy, win_menu, menu_x,
                          y0 + (int)((menu_y - y0) * e), MENU_W, MENU_H);
        XFlush(dpy);
        usleep(20000);
    }
    XMoveResizeWindow(dpy, win_menu, menu_x, menu_y, MENU_W, MENU_H);
    XFlush(dpy);
    /* NOTE: no pointer grab here. 3.3 grabbed the pointer while the menu was
     * open, but an X pointer grab is server-global: it starved Openbox and
     * every application of ALL clicks (title bars, app windows, taskbar
     * buttons stopped working). Outside-click dismissal is implemented with
     * an XI2 raw-button watcher instead (see GenericEvent handling). */
}

static void draw_menu_list(const char *header, RRect *lr) {
    draw_str(win_menu, xd_menu, f_small, &cc_sub, lr->x, lr->y - 18, header);
    char cnt[32];
    snprintf(cnt, sizeof cnt, "%d 个应用", nres);
    draw_str(win_menu, xd_menu, f_small, &cc_sub, MENU_W - 64, lr->y - 18, cnt);
    int nvis = list_vis;
    if (nres < 1 || apps_scroll >= nres) return;
    if (apps_scroll + nvis > nres) nvis = nres - apps_scroll;
    for (int r = 0; r < nvis; r++) {
        int ai = res_idx[apps_scroll + r];
        int ry = lr->y + r * ROW_H;
        int sel = (sel_row == r);
        int hov = (menu_hover_row == r);
        const Color *rowc = sel ? &cc_sel : (hov ? &cc_hoverc : &cc_menu);
        if (sel || hov)
            fill_round(win_menu, bgc, lr->x - 8, ry, MENU_W - 64, ROW_H - 2, 8,
                       rowc->pixel);
        Pixmap pm = icon_for_png(apps[ai].icon, 26, rowc);
        if (pm) draw_icon(win_menu, pm, 26, lr->x - 8, ry - 2, 40, ROW_H - 2);
        draw_str(win_menu, xd_menu, f_small, (sel || hov) ? &cc_light : &cc_text,
                 lr->x + 40, ry + ROW_H / 2 + f_small->ascent / 2 - 1, apps[ai].name);
    }
    if (nres > list_vis) {
        int track_h = lr->h, th = track_h * list_vis / nres;
        if (th < 16) th = 16;
        int ty = lr->y + track_h * apps_scroll / nres;
        fill_round(win_menu, bgc, MENU_W - 34, ty, 4, th, 2, cc_sub.pixel);
    }
}

static void draw_menu(void) {
    if (!win_menu) return;
    list_vis = list_rows_vis();
    fill_round(win_menu, bgc, 4, 4, MENU_W - 8, MENU_H - 8, 14, cc_menu.pixel);

    int pill_w = MENU_W - 40;
    fill_round(win_menu, bgc, 20, 20, pill_w, 40, 20, cc_search.pixel);
    char shown[96];
    if (menu_filt[0]) {
        snprintf(shown, sizeof shown, "%s", menu_filt);
    } else snprintf(shown, sizeof shown, "搜索");
    draw_str(win_menu, xd_menu, f_bar, &cc_text, 36,
             40 + f_bar->ascent / 2 - 1, shown);
    if (search_focus) {
        int cw = text_w(f_bar, shown);
        fill(win_menu, bgc, 36 + cw + 3, 32, 1, 18, cc_text.pixel);
    }

    if (menu_filt[0]) {
        RRect r = { 24, LIST_TOP_FILTER, MENU_W - 48, LIST_BOTTOM - LIST_TOP_FILTER };
        draw_menu_list("搜索结果", &r);
    } else {
        draw_str(win_menu, xd_menu, f_small, &cc_sub, 32, 82, "已固定");
        if (npin_tiles == 0)
            draw_str(win_menu, xd_menu, f_small, &cc_sub, 32, 116,
                     "没有固定项（右键任意应用可固定）");
        for (int s = 0; s < npin_tiles; s++) {
            int   i    = pin_slot[1 + s];
            RRect cell = menu_hot[1 + s];
            int fx = cell.x, ty = cell.y;                 /* full cell */
            int hot = (1 + s == menu_tile_idx);           /* grow-in tile */
            int out = (1 + s == menu_tile_prev);          /* shrink-out tile */
            int show = hot || out;
            float k = show ? tile_anim : 0.0f;
            float sc = 1.0f;
            if (show) {
                float ease = 1.0f - (1.0f - k) * (1.0f - k);   /* ease-out */
                sc = 0.86f + 0.14f * ease;
            }
            int cw = (int)(PIN_TSZ * sc), ch = cw;
            int cx = fx + (cell.w - cw) / 2;              /* chip, centered in cell */
            int cy = ty + (PIN_TSZ - ch) / 2;
            if (show)                                        /* Win11 hover chip */
                fill_round(win_menu, bgc, cx, cy, cw, ch, 10, cc_hoverc.pixel);
            /* icon, chip and label all share the same centre line */
            const char *nm[6];
            pin_icon_names(i, nm);
            Pixmap pm = icon_for_names(nm, 6, 44, show ? &cc_hoverc : &cc_menu);
            if (pm) draw_icon(win_menu, pm, 44, cx, cy, cw, ch);
            else if (!show)
                draw_icon_kind(win_menu, pins[i].kind, fx + PIN_CELL / 2,
                               ty + PIN_TSZ / 2, 40, cc_light.pixel);
            draw_str_c(win_menu, xd_menu, f_small, show ? &cc_light : &cc_text,
                       fx, ty + PIN_TSZ + 4, PIN_CELL, 14, pins[i].label);
        }
        draw_menu_list("所有应用", &list_r);
    }

    char who[64];
    snprintf(who, sizeof who, "%s", getenv("USER") ? getenv("USER") : "user");
    draw_str(win_menu, xd_menu, f_small, &cc_text, 32, MENU_H - 40, who);
    /* Win11-style circular power button */
    if (menu_power_hover)
        fill_round(win_menu, bgc, MENU_W - 72, MENU_H - 62, 44, 44, 22,
                   cc_hoverc.pixel);
    draw_power_symbol(win_menu, MENU_W - 50, MENU_H - 40, 20, cc_text.pixel);
}

/* ----------------------------------------------------------- dispatch */
static int in_rect(RRect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* right-click the taskbar button of a running app */
static void task_ctx_cb(int idx, void *ud) {
    int i = (int)(long)ud;
    if (i < 0 || i >= ntask) return;
    Window w = tasks[i].win;
    switch (idx) {
    case 0: activate_window(w); break;                        /* 还原   */
    case 1: XIconifyWindow(dpy, w, scr); break;               /* 最小化 */
    case 2: toggle_maximize(w); break;                        /* 最大化 */
    case 3: close_task(w); break;                             /* 关闭   */
    }
    menu_hide();
}

/* right-click a Start-menu app row */
static void applist_ctx_cb(int idx, void *ud) {
    int ai = (int)(long)ud;
    if (idx == 0) {
        if (ai >= 0 && ai < nres) { launch_app(ai); return; }
    } else if (idx == 1) {
        if (pin_app_pinned(ai))
            pin_unpin_by_exec(apps[res_idx[ai]].exec);
        else
            pin_app_add(ai);
        menu_refresh();            /* rebuild tiles around the new state */
    }
}

static void handle_bar_press(int but, int x, int y) {
    if (but == Button3) {                          /* taskbar context menu */
        cal_hide();
        menu_hide();
        int i = bar_task_at(x, y);
        if (i < 0) return;
        wm_cancel_move();
        cancel_repeat = 8;
        const char *items[4] = { "还原", "最小化", "最大化", "关闭" };
        ctx_show(items, 4, x, scr_h - BAR_H + y, task_ctx_cb,
                 (void *)(long)i);
        return;
    }
    if (but == Button2) {                       /* middle click: close window */
        cal_hide();
        wm_cancel_move();
        cancel_repeat = 8;
        int i = bar_task_at(x, y);
        if (i >= 0) { close_task(tasks[i].win); menu_hide(); }
        return;
    }
    if (in_rect(im_r, x, y)) { im_click(); return; }
    if (in_rect(net_r, x, y)) {
        /* 使用 SAS 已有的 NetworkManager/iwd WLAN 面板；它会扫描、提示
           密码并执行真实连接，而不是只显示一个无效的状态图标。 */
        menu_hide();
        vol_hide();
        launch_cmd("sas-screen --network");
        return;
    }
    if (in_rect(pill_r, x, y)) {
        menu_hide();
        if (but == Button4) { set_volume_pct(vol_level + 5); return; }
        if (but == Button5) { set_volume_pct(vol_level - 5); return; }
        if (vol_visible) vol_hide(); else vol_show();
        return;
    }
    if (in_rect(clock_r, x, y)) { menu_hide(); vol_hide(); cal_toggle(); return; }
    if (cal_visible) cal_hide();
    if (vol_visible && !in_rect(pill_r, x, y)) vol_hide();
    if (in_rect(start_r, x, y)) {
        if (menu_visible) menu_hide(); else menu_show();
        return;
    }
    if (in_rect(search_r, x, y)) {
        if (search_visible) search_hide();
        else search_show();
        return;
    }
    if (in_rect(edge_r, x, y)) {
        /* EWMH 要求以 ClientMessage 通知窗口管理器；仅写根属性不会
           触发 Openbox 最小化/还原窗口，因此原“显示桌面”按钮无反应。 */
        XEvent e;
        memset(&e, 0, sizeof e);
        e.xclient.type = ClientMessage;
        e.xclient.window = root;
        e.xclient.message_type = atom("_NET_SHOWING_DESKTOP");
        e.xclient.format = 32;
        show_desktop_active = !show_desktop_active;
        e.xclient.data.l[0] = show_desktop_active;
        e.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &e);
        XFlush(dpy);
        return;
    }

    int i = bar_task_at(x, y);
    if (i >= 0) {
        wm_cancel_move();
        if (tasks[i].active) XIconifyWindow(dpy, tasks[i].win, scr);
        else activate_window(tasks[i].win);
        menu_hide();
    }
}

static void handle_menu_press(int but, int x, int y) {
    cal_hide();
    for (int i = 0; i < nmenu_hot; i++) {
        if (in_rect(menu_hot[i], x, y)) {
            if (i == 0) {                          /* search pill */
                if (but == Button3) { menu_hide(); }
                else { search_focus = 1; draw_menu(); }
            }
            else if (i >= 1 && i <= npin_tiles && menu_cmd[i]) {
                if (but == Button3)
                    pinmenu_show(pin_slot[i], menu_x + menu_hot[i].x,
                                 menu_y + menu_hot[i].y + menu_hot[i].h + 8);
                else {
                    launch_cmd(menu_cmd[i]);
                    menu_hide();
                }
            } else if (i == 1 + npin_tiles) {      /* power button */
                if (but == Button3) menu_hide();
                else show_power();
            }
            return;
        }
    }
    /* list rows */
    int lty = menu_filt[0] ? LIST_TOP_FILTER : list_top_pins();
    int lh  = LIST_BOTTOM - lty;
    if (y >= lty && y < lty + lh && x >= 16 && x < MENU_W - 24) {
        int row = (y - lty) / ROW_H + apps_scroll;
        if (row < nres) {
            if (but == Button3) {
                const char *items[2] = { "打开", "固定到开始菜单" };
                if (pin_app_pinned(row))
                    items[1] = "取消固定";
                ctx_show(items, 2, menu_x + x, menu_y + y, applist_ctx_cb,
                         (void *)(long)row);
            } else {
                launch_app(row);
            }
            return;
        }
    }
    /* right-click on any other empty region of the Start menu: close it
     * (Win11 behavior; fixes the old "pass-through" feel) */
    if (but == Button3) menu_hide();
}

static void handle_menu_key(const XKeyEvent *ev) {
    KeySym ks = XLookupKeysym((XKeyEvent *)ev, 0);
    int row;

    if (ks == XK_Escape) {
        if (menu_filt[0]) { menu_filt[0] = 0; menu_filter_changed(); }
        else menu_hide();
        return;
    }
    if (!menu_visible) return;

    switch (ks) {
    case XK_Return:
        if (nres > 0) launch_app(sel_row >= 0 ? sel_row : 0);
        return;
    case XK_Down:
        if (nres > 0) {
            int nv = list_vis < nres ? list_vis : nres;
            row = (sel_row < 0 ? 0 : sel_row + 1);
            if (row >= nres) row = nres - 1;
            if (row >= apps_scroll + nv) apps_scroll = row - nv + 1;
            sel_row = row;
            draw_menu();
        }
        return;
    case XK_Up:
        if (sel_row > 0) {
            row = sel_row - 1;
            if (row < apps_scroll) apps_scroll = row;
            sel_row = row;
            draw_menu();
        }
        return;
    case XK_BackSpace: {
        size_t n = strlen(menu_filt);
        if (n > 0) { menu_filt[n-1] = 0; menu_filter_changed(); }
        return;
    }
    case XK_Home:  sel_row = 0; apps_scroll = 0; draw_menu(); return;
    case XK_End:   if (nres) { sel_row = nres-1; apps_scroll = nres > list_vis ? nres - list_vis : 0; } draw_menu(); return;
    case XK_Page_Up:   sel_row -= list_vis; if (sel_row < 0) sel_row = 0; draw_menu(); return;
    case XK_Page_Down: if (nres) { sel_row += list_vis; if (sel_row >= nres) sel_row = nres-1; } draw_menu(); return;
    default: break;
    }

    if ((ks >= XK_space && ks <= XK_asciitilde) || ks >= 0x100) {
        char mb[16];
        int n = XLookupString((XKeyEvent *)ev, mb, sizeof mb, NULL, NULL);
        if (n == 0) return;
        size_t len = strlen(menu_filt);
        int space = (int)(sizeof menu_filt - len - 4);
        if (space < n) return;
        memcpy(menu_filt + len, mb, (size_t)n);
        menu_filt[len + n] = 0;
        menu_filter_changed();
    }
}

/* ------------------------------------------------------------- search popup */
static void search_show(void) {
    cal_hide();
    power_hide();
    menu_hide();
    pmx = pmy = -1;
    menu_filt[0] = 0;
    search_focus = 1;
    filter_apps();
    sel_row = 0;
    apps_scroll = 0;
    search_x = (scr_w - SEARCH_W) / 2;
    search_y = scr_h - BAR_H - SEARCH_H - 4;
    XMoveResizeWindow(dpy, win_search, search_x, search_y, SEARCH_W, SEARCH_H);
    XMapRaised(dpy, win_search);
    XSetInputFocus(dpy, win_search, RevertToPointerRoot, CurrentTime);
    search_visible = 1;
    draw_search();
    XFlush(dpy);
}

static void search_hide(void) {
    if (search_visible && win_search) XUnmapWindow(dpy, win_search);
    search_visible = 0;
}

static void draw_search(void) {
    if (!win_search) return;
    fill_round(win_search, bgc, 4, 4, SEARCH_W - 8, SEARCH_H - 8, 14,
               cc_menu.pixel);

    fill_round(win_search, bgc, 20, 20, SEARCH_W - 40, 40, 20, cc_search.pixel);
    char shown[96];
    if (menu_filt[0]) snprintf(shown, sizeof shown, "%s", menu_filt);
    else snprintf(shown, sizeof shown, "搜索应用");
    draw_str(win_search, xd_search, f_bar, &cc_text, 36,
             40 + f_bar->ascent / 2 - 1, shown);
    if (search_focus) {
        int cw = text_w(f_bar, shown);
        fill(win_search, bgc, 36 + cw + 3, 32, 1, 18, cc_text.pixel);
    }

    RRect lr = { 24, 80, SEARCH_W - 48, SEARCH_H - 80 - 12 };
    if (nres < 1) {
        draw_str(win_search, xd_search, f_small, &cc_sub, 36, 112,
                 "没有找到匹配的应用");
        XFlush(dpy);
        return;
    }
    int nvis = lr.h / ROW_H;
    if (apps_scroll + nvis > nres) nvis = nres - apps_scroll;
    for (int r = 0; r < nvis; r++) {
        int ai = res_idx[apps_scroll + r];
        int ry = lr.y + r * ROW_H;
        int sel = (sel_row == r);
        int hov = (search_hover_row == r);
        const Color *rowc = sel ? &cc_sel : (hov ? &cc_hoverc : &cc_menu);
        if (sel || hov)
            fill_round(win_search, bgc, lr.x - 8, ry, SEARCH_W - 64, ROW_H - 2,
                       8, rowc->pixel);
        Pixmap pm = icon_for_png(apps[ai].icon, 26, rowc);
        if (pm) draw_icon(win_search, pm, 26, lr.x - 8, ry - 2, 40, ROW_H - 2);
        draw_str(win_search, xd_search, f_small, (sel || hov) ? &cc_light : &cc_text,
                 lr.x + 40, ry + ROW_H / 2 + f_small->ascent / 2 - 1,
                 apps[ai].name);
    }
    if (nres > nvis) {
        int track_h = lr.h, th = track_h * nvis / nres;
        if (th < 16) th = 16;
        int ty = lr.y + track_h * apps_scroll / nres;
        fill_round(win_search, bgc, SEARCH_W - 34, ty, 4, th, 2, cc_sub.pixel);
    }
    XFlush(dpy);
}

static void handle_search_press(int x, int y) {
    cal_hide();
    int lty = 80, lh = SEARCH_H - 80 - 12;
    if (y >= lty && y < lty + lh && x >= 16 && x < SEARCH_W - 24) {
        int row = (y - lty) / ROW_H + apps_scroll;
        if (row < nres) launch_app(row);
        return;
    }
    search_focus = 1;
    draw_search();
}

static void handle_search_key(const XKeyEvent *ev) {
    KeySym ks = XLookupKeysym((XKeyEvent *)ev, 0);
    int row;

    switch (ks) {
    case XK_Escape:
        if (menu_filt[0]) { menu_filt[0] = 0; filter_apps(); draw_search(); }
        else search_hide();
        return;
    case XK_Return:
        if (nres > 0) launch_app(sel_row >= 0 ? sel_row : 0);
        return;
    case XK_Down:
        if (nres > 0) {
            int nv = (list_vis = SEARCH_H / ROW_H) < nres ? list_vis : nres;
            row = (sel_row < 0 ? 0 : sel_row + 1);
            if (row >= nres) row = nres - 1;
            if (row >= apps_scroll + nv) apps_scroll = row - nv + 1;
            sel_row = row;
            draw_search();
        }
        return;
    case XK_Up:
        if (sel_row > 0) {
            row = sel_row - 1;
            if (row < apps_scroll) apps_scroll = row;
            sel_row = row;
            draw_search();
        }
        return;
    case XK_BackSpace: {
        size_t n = strlen(menu_filt);
        if (n > 0) {
            menu_filt[n - 1] = 0;
            filter_apps();
            sel_row = 0; apps_scroll = 0;
            draw_search();
        }
        return;
    }
    case XK_Home: sel_row = 0; apps_scroll = 0; draw_search(); return;
    case XK_End:
        if (nres) { sel_row = nres - 1; apps_scroll = nres > 12 ? nres - 12 : 0; }
        draw_search();
        return;
    case XK_Page_Up:   sel_row -= 12; if (sel_row < 0) sel_row = 0; draw_search(); return;
    case XK_Page_Down: if (nres) { sel_row += 12; if (sel_row >= nres) sel_row = nres - 1; } draw_search(); return;
    default: break;
    }

    if ((ks >= XK_space && ks <= XK_asciitilde) || ks >= 0x100) {
        char mb[16];
        int n = XLookupString((XKeyEvent *)ev, mb, sizeof mb, NULL, NULL);
        if (n == 0) return;
        size_t len = strlen(menu_filt);
        int space = (int)(sizeof menu_filt - len - 4);
        if (space < n) return;
        memcpy(menu_filt + len, mb, (size_t)n);
        menu_filt[len + n] = 0;
        filter_apps();
        sel_row = 0; apps_scroll = 0;
        draw_search();
    }
}

static void handle_root_press(int but, int x, int y) {
    if (menu_visible && y < scr_h - BAR_H) { menu_hide(); return; }
    if (search_visible && y < scr_h - BAR_H) { search_hide(); return; }
    if (dm_visible) { dm_hide(); if (but != Button1) return; }
    int icon = -1;
    for (int i = 0; i < nic; i++)
        if (x >= ic[i].x && x < ic[i].x + ICON_W &&
            y >= ic[i].y && y < ic[i].y + ICON_H) { icon = i; break; }
    if (but == Button3) {                          /* desktop context menu */
        press_active = 0;
        press_icon = -1;
        dm_show(x, y, icon);
        return;
    }
    press_active = icon >= 0;
    press_icon = icon;
    sel_icon = icon;
    if (icon >= 0) { sel_anim = 0.0f; sel_anim_act = 1; }   /* pop-in like Start menu */
    press_x0 = x;
    press_y0 = y;
    if (icon >= 0) { ic_orig_x = ic[icon].x; ic_orig_y = ic[icon].y; }
}

static void handle_desk_motion(int x, int y) {
    if (!press_active || press_icon < 0) {
        /* Win11-style hover highlight while just moving over the desktop */
        int h = -1;
        for (int i = 0; i < nic; i++)
            if (x >= ic[i].x && x < ic[i].x + ICON_W &&
                y >= ic[i].y && y < ic[i].y + ICON_H) { h = i; break; }
        if (h != hover_icon) { hover_icon = h; desk_dirty = 1; }
        return;
    }
    int nx = ic_orig_x + (x - press_x0);
    int ny = ic_orig_y + (y - press_y0);
    /* snap to the icon lattice like Windows "auto arrange to grid" */
    ic[press_icon].x = ICON_GX + ((nx - ICON_GX + ICON_CSX / 2) / ICON_CSX) * ICON_CSX;
    ic[press_icon].y = ICON_GY + ((ny - ICON_GY + ICON_CSY / 2) / ICON_CSY) * ICON_CSY;
    desk_dirty = 1;
}

static void handle_desk_release(int x, int y) {
    if (!press_active) { press_active = 0; return; }
    press_active = 0;
    if (press_icon < 0) return;
    int moved = abs(x - press_x0) + abs(y - press_y0);
    if (moved >= 8) {
        icons_pos_save();                /* remember the new grid position */
        press_icon = -1;
        return;
    }
    double t = now_sec();
    if (press_icon == last_click_icon && t - last_click < 0.5) {
        open_path(ic[press_icon].path);
        last_click_icon = -1;
    } else {
        last_click_icon = press_icon;
        last_click = t;
    }
    press_icon = -1;
}

/* ----------------------------------------------------------------- main */

/* Xlib's default error handler kills the process on any Bad* error (a stale
 * window id, or a focus on a window that unmaps mid-transition during WM
 * churn).  Swallow benign errors so the desktop never just
 * vanishes;  the only fatal state left is a lost X connection (XIOError). */
static int xerr(struct _XDisplay *d, XErrorEvent *e) {
    /* not logged on purpose: harmless races (BadWindow) happen frequently */
    (void)d; (void)e;
    return 0;
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    XSetErrorHandler(xerr);
    dpy = XOpenDisplay(NULL);
    if (!dpy) die("cannot open display");
    scr = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);
    vis = DefaultVisual(dpy, scr);
    cmap = DefaultColormap(dpy, scr);
    scr_w = XWidthOfScreen(ScreenOfDisplay(dpy, scr));
    scr_h = XHeightOfScreen(ScreenOfDisplay(dpy, scr));
    bgc = XCreateGC(dpy, root, 0, NULL);

    /* the shell is a self-contained desktop: disable the legacy X screen
       saver so the desktop can never blank unexpectedly; idle/power blanking
       is the DE's own policy, not the X server's */
    XSetScreenSaver(dpy, 0, 0, 0, 0);

    cc_bar     = make_color("#1a1f2b");
    cc_lo      = make_color("#10406e");
    cc_hi      = make_color("#1b5c96");
    cc_text    = make_color("#ececec");
    cc_sub     = make_color("#9aa4ab");
    cc_task    = make_color("#2b3240");
    cc_menu    = make_color("#17191f");
    cc_accent  = make_color("#0f6cbd");
    cc_search  = make_color("#333333");
    cc_light   = make_color("#e8e8e8");
    cc_hoverc  = make_color("#3c5a99");
    cc_sel     = make_color("#3c5a99");
    cc_selwash = make_color("#2b3a66");
    cc_logo1   = make_color("#4da6ff");
    cc_logo2   = make_color("#6fd08c");
    cc_logo3   = make_color("#f2a53c");
    cc_logo4   = make_color("#f26b6b");
    for (int i = 0; i < NPIN_MAX; i++) pin_colors[i] = make_color("#8a8f98");
    pin_reset_defaults();
    pin_colors_refresh();
    pin_persist_load();
    pin_colors_refresh();

    ngcol = (int)(sizeof gcol / sizeof gcol[0]);
    for (int i = 0; i < ngcol; i++) {
        float t = (float)i / (ngcol - 1);
        XColor c;
        c.red   = (unsigned short)((float)cc_lo.xft.color.red   * (1 - t) +
                                   (float)cc_hi.xft.color.red   * t);
        c.green = (unsigned short)((float)cc_lo.xft.color.green * (1 - t) +
                                   (float)cc_hi.xft.color.green * t);
        c.blue  = (unsigned short)((float)cc_lo.xft.color.blue  * (1 - t) +
                                   (float)cc_hi.xft.color.blue  * t);
        c.flags = DoRed | DoGreen | DoBlue;
        gcol[i] = XAllocColor(dpy, cmap, &c) ? c.pixel : cc_lo.pixel;
    }

    f_bar   = XftFontOpenName(dpy, scr, "sans-serif:bold:pixelsize=13");
    f_small = XftFontOpenName(dpy, scr, "sans-serif:pixelsize=11");
    f_tile  = XftFontOpenName(dpy, scr, "sans-serif:bold:pixelsize=22");
    if (!f_bar || !f_small || !f_tile) die("font init failed");

    win_bar = mk_owindow(0, scr_h - BAR_H, scr_w, BAR_H);
    xd_bar  = XftDrawCreate(dpy, win_bar, vis, cmap);
    /* also swallow button releases so a held button released over the bar
       never reaches openbox / the root window and starts a ghost Move */
    XSelectInput(dpy, win_bar, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                               KeyPressMask | PointerMotionMask |
                               EnterWindowMask | LeaveWindowMask);

    start_r  = (RRect){ 8,    (BAR_H - 36) / 2, 42, 36 };
    search_r = (RRect){ 58,   (BAR_H - 36) / 2, 150, 36 };
    edge_r   = (RRect){ scr_w - 9, 0, 9, BAR_H };
    clock_r  = (RRect){ scr_w - 9 - 92, 0, 88, BAR_H };
    im_r     = (RRect){ clock_r.x - 4 - 44, (BAR_H - 36) / 2, 44, 36 };
    pill_r   = (RRect){ im_r.x - 4 - 82, (BAR_H - 36) / 2, 82, 36 };
    net_r    = (RRect){ pill_r.x + 4, pill_r.y, 26, pill_r.h };

    win_menu = mk_owindow(0, 0, MENU_W, MENU_H);
    xd_menu  = XftDrawCreate(dpy, win_menu, vis, cmap);

    win_power = mk_owindow(0, 0, POWER_W, POWER_H);
    xd_power  = XftDrawCreate(dpy, win_power, vis, cmap);

    win_pinm = mk_owindow(0, 0, PINM_W, PINM_H);
    xd_pinm  = XftDrawCreate(dpy, win_pinm, vis, cmap);

    win_ctx = mk_owindow(0, 0, CTX_W, CTX_MAX * CTX_ROW + 8);
    xd_ctx  = XftDrawCreate(dpy, win_ctx, vis, cmap);

    win_dm = mk_owindow(0, 0, DM_W, DM_MAX * DM_ROW + 8);
    xd_dm  = XftDrawCreate(dpy, win_dm, vis, cmap);

    win_search = mk_owindow(search_x, search_y, SEARCH_W, SEARCH_H);
    xd_search  = XftDrawCreate(dpy, win_search, vis, cmap);

    win_cal = mk_owindow(0, 0, CAL_W, CAL_H);
    xd_cal  = XftDrawCreate(dpy, win_cal, vis, cmap);

    tray_init();
    im_state_init();

    win_desk = mk_desktop_window(scr_w, scr_h - BAR_H);
    xd_desk  = XftDrawCreate(dpy, win_desk, vis, cmap);

    set_wm_state(win_bar);

    wallpaper_init();
    load_apps();
    gen_icons();
    icon_layout();
    autostart_run();
    paint_desktop();

    XMapRaised(dpy, win_desk);
    XMapRaised(dpy, win_bar);
    XFlush(dpy);

    k_win1 = XKeysymToKeycode(dpy, XK_Super_L);
    k_win2 = XKeysymToKeycode(dpy, XK_Super_R);

    xkb_init();                      /* grab-free Super key state tracking */
    xi2_init();                      /* grab-proof physical button/key tracking */
    fprintf(stderr, "[elevende] Xkb stateNotify %s: Super key tracking\n",
            xkb_ok ? "enabled" : "UNAVAILABLE (Super menu toggle disabled)");
#ifdef HAVE_XTEST
    fprintf(stderr, "[elevende] XTest enabled: ghost move/resize recovery armed\n");
#else
    fprintf(stderr, "[elevende] WARNING: built WITHOUT XTest -- ghost move/resize recovery DISABLED (install libxtst-dev and rebuild)\n");
#endif
#ifdef HAVE_XINPUT2
    if (xi2_ok)
        fprintf(stderr, "[elevende] XInput2 enabled: raw button tracking active\n");
    else
        fprintf(stderr, "[elevende] WARNING: XInput2 unavailable at runtime\n");
#endif

    time_t last_poll = 0;
    int dirty = 1, act_changed = 1;
    int last_min = -1;

    for (;;) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.window == win_bar)      dirty = 1;
                else if (ev.xexpose.window == win_desk) desk_dirty = 1;
                else if (ev.xexpose.window == win_menu) draw_menu();
                else if (ev.xexpose.window == win_power) draw_power();
                else if (ev.xexpose.window == win_pinm) draw_pinmenu();
                else if (ev.xexpose.window == win_ctx) draw_ctx();
                else if (ev.xexpose.window == win_dm) draw_dm();
                else if (ev.xexpose.window == win_tray) draw_tray();
                else if (ev.xexpose.window == win_search) draw_search();
                else if (ev.xexpose.window == win_cal) draw_cal();
                else if (ev.xexpose.window == win_vol) draw_vol_flyout();
                break;
            case ClientMessage:
                if (tray_ok && ev.xclient.message_type ==
                              atom("_NET_SYSTEM_TRAY_OPCODE") &&
                    ev.xclient.data.l[1] == 0)          /* REQUEST_DOCK */
                    tray_dock((Window)ev.xclient.data.l[2]);
                else if (ev.xclient.message_type ==
                         atom("_ELEVENDE_RELOAD_WALLPAPER")) {
                    /* the Settings app switched the wallpaper: re-read
                     * ~/.local/share/elevende/wallpaper.png (or the system
                     * fallback) and repaint the desktop next pass. */
                    wall_ok = 0;
                    wallpaper_init();
                    desk_dirty = 1;
                }
                break;
            case MapNotify:
                /* 外部窗口映射只由 Openbox 负责；Shell 不改变任何输入状态。 */
                break;
            case DestroyNotify:
                for (int i = 0; i < ntray; i++)
                    if (tray_wins[i] == ev.xdestroywindow.window)
                        tray_remove(ev.xdestroywindow.window);
                /* 销毁外部窗口不能触发合成输入或窗口管理器命令。 */
                break;
            case UnmapNotify:
                if (ev.xunmap.window == win_cal) cal_visible = 0;
                break;
            case ReparentNotify:
                if (ntray && ev.xreparent.parent != win_tray)
                    for (int i = 0; i < ntray; i++)
                        if (tray_wins[i] == ev.xreparent.window)
                            tray_remove(ev.xreparent.window);
                break;
            case ButtonPress:
                /* while the power flyout is up, clicking the menu must only
                   close the flyout -- never fall through and open an app */
                if (power_visible && ev.xbutton.window == win_menu) {
                    power_hide();
                    break;
                }
                if (power_visible && ev.xbutton.window != win_power)
                    power_hide();
                if (ctx_visible && ev.xbutton.window != win_ctx) {
                    ctx_hide();
                    break;   /* modal menu: the dismiss click must NOT fall through */
                }
                if (pinm_visible && ev.xbutton.window != win_pinm) {
                    pinmenu_hide();
                    break;
                }
                if (dm_visible && ev.xbutton.window != win_dm)
                    dm_hide();
                /* presses redirected to root by our grab = presses outside
                 * the Start menu / search: dismiss them, swallow the click */
                if (menu_visible && ev.xbutton.window == root) {
                    menu_hide();
                    break;
                }
                if (search_visible && !menu_visible &&
                    ev.xbutton.window == root) {
                    search_hide();
                    break;
                }
                if (ev.xbutton.window == win_ctx) {
                    handle_ctx_press(ev.xbutton.x, ev.xbutton.y);
                    break;
                }
                if (ev.xbutton.window == win_dm) {
                    handle_dm_press(ev.xbutton.x, ev.xbutton.y);
                    break;
                }
                if (ev.xbutton.window == win_cal) {
                    handle_cal_press(ev.xbutton.x, ev.xbutton.y);
                    break;
                }
                if (ev.xbutton.window == win_vol) {
                    if (ev.xbutton.button == Button4)
                        set_volume_pct(vol_level + 5);
                    else if (ev.xbutton.button == Button5)
                        set_volume_pct(vol_level - 5);
                    else
                        handle_vol_press(ev.xbutton.x, ev.xbutton.y);
                    break;
                }
                if (ev.xbutton.window == win_tray) break; /* tray client windows
                                                            take their own clicks */
                if (ev.xbutton.window == win_power) {
                    handle_power_press(ev.xbutton.x, ev.xbutton.y);
                    break;
                }
                if (ev.xbutton.window == win_search) {
                    handle_search_press(ev.xbutton.x, ev.xbutton.y);
                    dirty = 1;
                    break;
                }
                if (ev.xbutton.window == win_pinm) {
                    handle_pinmenu_press(ev.xbutton.x, ev.xbutton.y);
                    break;
                }
                if (ev.xbutton.window == win_menu) {
                    if (ev.xbutton.button == Button4) {          /* wheel up */
                        if (apps_scroll > 0) { apps_scroll--; draw_menu(); }
                    } else if (ev.xbutton.button == Button5) {   /* wheel down */
                        if (apps_scroll + list_vis < nres) { apps_scroll++; draw_menu(); }
                    } else handle_menu_press((int)ev.xbutton.button,
                                             ev.xbutton.x, ev.xbutton.y);
                    dirty = 1;
                    break;
                }
                if      (ev.xbutton.window == win_bar)
                    handle_bar_press((int)ev.xbutton.button, ev.xbutton.x, ev.xbutton.y);
                else if (ev.xbutton.window == win_desk)
                    handle_root_press((int)ev.xbutton.button,
                                      ev.xbutton.x, ev.xbutton.y);
                dirty = 1;
                break;
            case ButtonRelease:
                if (ev.xbutton.window == win_vol) { vol_drag = 0; break; }
                if (ev.xbutton.window == win_bar) break;   /* consume, no leak */
                if (ev.xbutton.window == win_desk)
                    handle_desk_release(ev.xbutton.x, ev.xbutton.y);
                break;
            case KeyPress:
                if (ev.xkey.window == win_power && power_visible) {
                    if (XLookupKeysym(&ev.xkey, 0) == XK_Escape) power_hide();
                    break;
                }
                if (ev.xkey.window == win_pinm && pinm_visible) {
                    if (XLookupKeysym(&ev.xkey, 0) == XK_Escape) pinmenu_hide();
                    break;
                }
                if (ev.xkey.window == win_ctx && ctx_visible) {
                    if (XLookupKeysym(&ev.xkey, 0) == XK_Escape) ctx_hide();
                    break;
                }
                if (ev.xkey.window == win_dm && dm_visible) {
                    if (XLookupKeysym(&ev.xkey, 0) == XK_Escape) dm_hide();
                    break;
                }
                if (ev.xkey.window == win_menu || menu_visible)
                    handle_menu_key(&ev.xkey);
                else if (ev.xkey.window == win_search || search_visible) {
                    if (ev.xkey.keycode != k_win1 && ev.xkey.keycode != k_win2)
                        handle_search_key(&ev.xkey);
                }
                else if (XLookupKeysym(&ev.xkey, 0) == XK_Escape) menu_hide();
                break;
            case MotionNotify:
                if (ev.xmotion.window == win_desk) {
                    handle_desk_motion(ev.xmotion.x, ev.xmotion.y);
                    break;
                }
                if (ev.xmotion.window == win_vol) {
                    handle_vol_motion(ev.xmotion.x, ev.xmotion.y);
                    break;
                }
                if (ev.xmotion.window == win_bar) {
                    int h = bar_task_at(ev.xmotion.x, ev.xmotion.y);
                    if (h != bar_hover) { bar_hover = h; dirty = 1; }
                    int hp = in_rect(start_r, ev.xmotion.x, ev.xmotion.y) ||
                             in_rect(search_r, ev.xmotion.x, ev.xmotion.y);
                    if (hp != bar_pill_hover) { bar_pill_hover = hp; dirty = 1; }
                    pmx = ev.xmotion.x; pmy = ev.xmotion.y;
                } else if (ev.xmotion.window == win_menu) {
                    pmx = ev.xmotion.x; pmy = ev.xmotion.y;
                    RRect lr = cur_list_rect();
                    int r = -1;
                    if (ev.xmotion.y >= lr.y && ev.xmotion.y < lr.y + lr.h)
                        r = (ev.xmotion.y - lr.y) / ROW_H;
                    int tidx = -1;
                    for (int i = 1; i <= npin_tiles; i++)
                        if (in_rect(menu_hot[i], ev.xmotion.x, ev.xmotion.y))
                            { tidx = i; break; }
                    int ph = in_rect(menu_hot[1 + npin_tiles],
                                     ev.xmotion.x, ev.xmotion.y);
                    int changed = 0;
                    if (tidx != menu_tile_idx) {
                        if (tidx < 0) menu_tile_prev = menu_tile_idx;
                        else menu_tile_prev = -1;      /* new tile grows alone */
                        menu_tile_idx = tidx;
                        tile_anim = (tidx >= 0) ? 0.0f : 1.0f;
                        tile_anim_act = 1;
                        menu_hover_row = r;
                        menu_power_hover = ph;
                        changed = 1;
                    } else if (r != menu_hover_row) {
                        menu_hover_row = r; changed = 1;
                    } else if (ph != menu_power_hover) {
                        menu_power_hover = ph; changed = 1;
                    }
                    if (changed) draw_menu();
                } else if (ev.xmotion.window == win_search) {
                    RRect lr = { 24, 80, SEARCH_W - 48, SEARCH_H - 80 - 12 };
                    int r = -1;
                    if (ev.xmotion.y >= lr.y && ev.xmotion.y < lr.y + lr.h)
                        r = (ev.xmotion.y - lr.y) / ROW_H;
                    if (r != search_hover_row) {
                        search_hover_row = r;
                        draw_search();
                    }
                    pmx = ev.xmotion.x; pmy = ev.xmotion.y;
                } else if (ev.xmotion.window == win_power) {
                    int hv = -1;
                    for (int i = 0; i < npower_hot; i++)
                        if (in_rect(power_hot[i], ev.xmotion.x, ev.xmotion.y))
                            { hv = i; break; }
                    if (hv != power_hover) { power_hover = hv; draw_power(); }
                } else if (ev.xmotion.window == win_pinm && pinm_visible) {
                    int hv = -1;
                    for (int i = 0; i < 2; i++)
                        if (in_rect(pinm_hot[i], ev.xmotion.x, ev.xmotion.y))
                            { hv = i; break; }
                    if (hv != pinm_hover) { pinm_hover = hv; draw_pinmenu(); }
                } else if (ev.xmotion.window == win_ctx && ctx_visible) {
                    int hv = -1;
                    for (int i = 0; i < ctx_n; i++)
                        if (in_rect(ctx_hot[i], ev.xmotion.x, ev.xmotion.y))
                            { hv = i; break; }
                    if (hv != ctx_hover) { ctx_hover = hv; draw_ctx(); }
                } else if (ev.xmotion.window == win_dm && dm_visible) {
                    int hv = -1;
                    for (int i = 0; i < dm_n; i++)
                        if (in_rect(dm_hot[i], ev.xmotion.x, ev.xmotion.y))
                            { hv = i; break; }
                    if (hv != dm_hover) { dm_hover = hv; draw_dm(); }
                }
                break;
            case LeaveNotify:
                if (ev.xcrossing.window == win_bar &&
                    (bar_hover != -1 || bar_pill_hover)) {
                    bar_hover = -1; bar_pill_hover = -1; dirty = 1;
                } else if (ev.xcrossing.window == win_power && power_hover != -1) {
                    power_hover = -1; draw_power();
                } else if (ev.xcrossing.window == win_pinm && pinm_hover != -1) {
                    pinm_hover = -1; draw_pinmenu();
                } else if (ev.xcrossing.window == win_ctx && ctx_hover != -1) {
                    ctx_hover = -1; draw_ctx();
                } else if (ev.xcrossing.window == win_dm && dm_hover != -1) {
                    dm_hover = -1; draw_dm();
                } else if (ev.xcrossing.window == win_menu &&
                           (menu_hover_row != -1 || menu_tile_idx != -1 ||
                            menu_power_hover)) {
                    menu_hover_row = -1;
                    menu_power_hover = 0;
                    menu_tile_prev = menu_tile_idx;
                    menu_tile_idx = -1;
                    tile_anim = 1.0f;
                    tile_anim_act = 1;
                    draw_menu();
                } else if (ev.xcrossing.window == win_search && search_hover_row != -1) {
                    search_hover_row = -1; draw_search();
                }
                if (ev.xcrossing.window == win_desk && hover_icon != -1) {
                    hover_icon = -1; desk_dirty = 1;
                }
                break;
            case GenericEvent: {
#ifdef HAVE_XINPUT2
                XGenericEventCookie *ck = &ev.xcookie;
                if (ck->extension == xi_opcode && XGetEventData(dpy, ck)) {
                    if (ck->evtype == XI_RawButtonPress) phys_btn = 1;
                    else if (ck->evtype == XI_RawButtonRelease) phys_btn = 0;
                    else if (ck->evtype == XI_RawKeyPress && ck->data) {
                        XIRawEvent *re = (XIRawEvent *)ck->data;
                        if (super_down && re->detail != k_win1 &&
                            re->detail != k_win2)
                            super_combo = 1;   /* Win+<something> combo */
                    }
                    else if (ck->evtype == XI_RawButtonPress) {
                        /* Win11 behavior without any pointer grab: a
                         * physical press OUTSIDE our UI closes the Start
                         * menu / search / flyouts. Raw events see every
                         * press regardless of which window receives it. */
                        if ((menu_visible || search_visible || ctx_visible ||
                             pinm_visible) && !pointer_over_own_ui())
                            menu_hide();     /* hides power/pinm/ctx too */
                    }
                    XFreeEventData(dpy, ck);
                }
#endif
                break;
            }
            default:
                if (xkb_ok && ev.type == xkb_event_base) {
                    XkbEvent *xe = (XkbEvent *)&ev;
                    if (xe->any.xkb_type == XkbStateNotify) {
                        int s = (xe->state.mods & Mod4Mask) != 0;
                        if (s && !super_down) {
                            super_down = 1;
                            super_combo = 0;
                        } else if (!s && super_down) {
                            super_down = 0;
                            /* Windows behavior: the Start menu toggles on
                             * Super RELEASE, and only when Super was pressed
                             * alone (XI2 raw keys detect the combos) */
                            if (super_combo) {
                                if (menu_visible) menu_hide();
                            } else {
                                if (menu_visible) menu_hide(); else menu_show();
                            }
                            super_combo = 0;
                        }
                    }
                }
                break;
            }
        }

        /* 不在后台注入鼠标释放或反复发送 moveresize-cancel。旧的“幽灵
           拖动防御”会与 Openbox 的真实拖动竞争，导致标题栏、窗口控制和
           应用按钮看似有动画却无法点击。 */
        if (cancel_repeat > 0)
            cancel_repeat = 0;

        if (tile_anim_act && menu_visible) {
            float st = 0.18f;
            if (menu_tile_idx >= 0) {
                tile_anim += st;
                if (tile_anim >= 1.0f) { tile_anim = 1.0f; tile_anim_act = 0; }
            } else if (menu_tile_prev >= 0) {
                tile_anim -= st;
                if (tile_anim <= 0.0f) {
                    tile_anim = 0.0f; menu_tile_prev = -1; tile_anim_act = 0;
                }
            } else tile_anim_act = 0;
            draw_menu();
        }

        if (sel_anim_act) {
            sel_anim += 0.28f;
            if (sel_anim >= 1.0f) { sel_anim = 1.0f; sel_anim_act = 0; }
            desk_dirty = 1;
        }

        time_t now = time(NULL);
        if (now >= last_state_poll + 2) {
            last_state_poll = now;
            sys_state_update();
            im_state_update();
            /* wallpaper hot-reload: the Settings app (or any tool) may swap
             * ~/.local/share/elevende/wallpaper.png at any time; pick the
             * change up within 2 s even if the client message was missed */
            const char *home = getenv("HOME");
            if (home) {
                char wp[1024];
                snprintf(wp, sizeof wp, "%s/.local/share/elevende/wallpaper.png",
                         home);
                struct stat wst;
                if (stat(wp, &wst) == 0) {
                    if (wall_mtime &&
                        (wst.st_mtime != wall_mtime ||
                         wst.st_mtim.tv_nsec != wall_mtime_nsec ||
                         wst.st_size != wall_size)) {
                        wallpaper_init();
                        desk_dirty = 1;
                    } else {
                        wall_mtime = wst.st_mtime;
                        wall_mtime_nsec = wst.st_mtim.tv_nsec;
                        wall_size = wst.st_size;
                    }
                }
                /* The desktop may be changed outside ElevenDE (file manager,
                   terminal, cloud sync). Watch the directory metadata so icon
                   creation/deletion/rename takes effect without a restart. */
                struct stat dst;
                if (stat(desk_dir(), &dst) == 0) {
                    if (desk_mtime &&
                        (dst.st_mtime != desk_mtime ||
                         dst.st_mtim.tv_nsec != desk_mtime_nsec ||
                         dst.st_size != desk_size)) {
                        icons_reload();
                    } else {
                        desk_mtime = dst.st_mtime;
                        desk_mtime_nsec = dst.st_mtim.tv_nsec;
                        desk_size = dst.st_size;
                    }
                }
            }
        }
        if (now != last_poll) {
            int ch = refresh_tasks();
            if (ch) { dirty = 1; act_changed = 1; cancel_repeat = 12; }
            if (act_changed) {
        /* Windows keeps the Start menu topmost: if a window was raised
         * while the menu is open, bring the menu (and its flyouts) back
         * above it instead of letting it hide behind the window. */
        if (menu_visible && win_menu && !ctx_visible) XMapRaised(dpy, win_menu);
        if (ctx_visible && win_ctx) XMapRaised(dpy, win_ctx);
        if (search_visible && win_search) XMapRaised(dpy, win_search);
        active_task = -1;
        for (int i = 0; i < ntask; i++)
            if (tasks[i].active) { active_task = i; break; }
        /* an external app grabbed focus -> close the Start menu;
         * the desktop window counts as ourselves, not an app */
        if (menu_visible && g_active && g_active != win_desk &&
            g_active != menu_open_act &&
            g_active != win_bar && g_active != win_menu)
            menu_hide();
        if (search_visible && g_active && g_active != win_desk &&
            g_active != win_bar && g_active != win_search)
            search_hide();
        act_changed = 0;
        cancel_repeat = 12; /* external focus change might be a ghost move trigger */
    }
            last_poll = now;
        }
        int minute = (int)(now / 60);
        if (dirty || bar_dirty || minute != last_min) {
            draw_taskbar();
            last_min = minute;
            bar_dirty = 0;
        }
        if (desk_dirty) {                   /* repaint every pass -> smooth drag */
            paint_desktop();
            desk_dirty = 0;
        }

        if (menu_visible && !power_visible && !pinm_visible && !dm_visible &&
            !ctx_visible)
            XRaiseWindow(dpy, win_menu);
        if (ctx_visible) XRaiseWindow(dpy, win_ctx);   /* never under menu */
        if (search_visible && !power_visible) XRaiseWindow(dpy, win_search);
        if (power_visible) XRaiseWindow(dpy, win_power);
        if (pinm_visible) XRaiseWindow(dpy, win_pinm);      /* above menu */
        if (dm_visible) XRaiseWindow(dpy, win_dm);          /* above menu */
        if (ctx_visible) XRaiseWindow(dpy, win_ctx);        /* on top */
        /* 任务栏只在启动时置顶；这里绝不能每帧抬升，否则会与 SAS 的
           全屏安全覆盖层争夺堆叠顺序并造成闪烁。 */
        if (ctx_visible) XRaiseWindow(dpy, win_ctx);        /* above taskbar */
        if (dirty) dirty = 0;
        XFlush(dpy);

        struct timeval tv = { 0, (press_active || tile_anim_act || sel_anim_act) ? 16000 : 150000 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(dpy), &fds);
        select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, &tv);
    }
}