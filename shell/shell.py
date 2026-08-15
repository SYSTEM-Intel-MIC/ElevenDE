#!/usr/bin/env python3
"""
ElevenDE shell — Windows 11 style taskbar, start menu and wallpaper.

One process hosts three layer-shell surfaces:
  * wallpaper  (background layer, Win11 "bloom" gradient)
  * taskbar    (bottom layer, exclusive zone)
  * start menu (overlay layer, exclusive keyboard while open)

Talks to the elevende compositor through the JSON-lines IPC socket.
"""

import json
import math
import os
import socket
from ctypes import CDLL

# gtk4-layer-shell must be loaded before libwayland-client (which gi pulls in)
CDLL("libgtk4-layer-shell.so.0")

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gtk4LayerShell", "1.0")
from gi.repository import GLib, Gio, Gtk, Gtk4LayerShell  # noqa: E402

try:
    import cairo
except ImportError:  # pragma: no cover
    cairo = None

RUNTIME_DIR = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
IPC_PATH = os.path.join(RUNTIME_DIR, "elevende-ipc.sock")
TASKBAR_H = 48
MENU_W, MENU_H = 620, 640

SHELL_DIR = os.path.dirname(os.path.abspath(__file__))


# ----------------------------- IPC client --------------------------------

class IpcClient:
    def __init__(self, on_event):
        self.on_event = on_event
        self.sock = None
        self.buf = b""
        self.watch_id = None
        self.connect()

    def connect(self):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(IPC_PATH)
            s.setblocking(False)
            self.sock = s
            self.watch_id = GLib.io_add_watch(
                s.fileno(), GLib.PRIORITY_DEFAULT,
                GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self._readable)
            return True
        except OSError:
            GLib.timeout_add_seconds(2, self._retry)
            return False

    def _retry(self):
        self.connect()
        return False

    def _readable(self, fd, cond):
        try:
            data = self.sock.recv(65536)
        except OSError:
            data = b""
        if not data:
            if self.watch_id:
                GLib.source_remove(self.watch_id)
            self.sock = None
            GLib.timeout_add_seconds(2, self._retry)
            return False
        self.buf += data
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            self.on_event(ev)
        return True

    def send(self, **kwargs):
        if not self.sock:
            return
        try:
            self.sock.send((json.dumps(kwargs) + "\n").encode())
        except OSError:
            pass


# --------------------------- app registry --------------------------------

class AppRegistry:
    """Maps app_id / window class to Gio.DesktopAppInfo + icon name."""

    def __init__(self):
        self.apps = []          # (name, DesktopAppInfo, icon_name)
        self.by_key = {}
        self._scan()

    def _scan(self):
        for info in Gio.AppInfo.get_all():
            if not isinstance(info, Gio.DesktopAppInfo):
                continue
            if info.get_nodisplay() or info.get_is_hidden():
                continue
            name = info.get_display_name()
            icon = info.get_icon()
            icon_name = icon.to_string() if icon else "application-x-executable"
            entry = (name, info, icon_name)
            self.apps.append(entry)
            path = info.get_filename() or ""
            stem = os.path.basename(path)[:-len(".desktop")] if path.endswith(".desktop") else ""
            for key in filter(None, {stem.lower(), (info.get_startup_wm_class() or "").lower(),
                                     name.lower()}):
                self.by_key.setdefault(key, entry)
        self.apps.sort(key=lambda e: GLib.utf8_collate_key(e[0], -1) if hasattr(GLib, 'utf8_collate_key') else e[0].lower())

    def lookup(self, app_id):
        if not app_id:
            return None
        return self.by_key.get(app_id.lower())

    def icon_name_for(self, app_id):
        entry = self.lookup(app_id)
        return entry[2] if entry else "application-x-executable"

    def pinned(self, limit=24):
        return self.apps[:limit]


# ------------------------------ wallpaper --------------------------------

class WallpaperWindow(Gtk.Window):
    def __init__(self):
        super().__init__()
        Gtk4LayerShell.init_for_window(self)
        Gtk4LayerShell.set_namespace(self, "ed-wallpaper")
        Gtk4LayerShell.set_layer(self, Gtk4LayerShell.Layer.BACKGROUND)
        for edge in (Gtk4LayerShell.Edge.TOP, Gtk4LayerShell.Edge.BOTTOM,
                     Gtk4LayerShell.Edge.LEFT, Gtk4LayerShell.Edge.RIGHT):
            Gtk4LayerShell.set_anchor(self, edge, True)
        Gtk4LayerShell.set_exclusive_zone(self, -1)

        area = Gtk.DrawingArea()
        area.set_draw_func(self._draw)
        area.set_hexpand(True)
        area.set_vexpand(True)
        self.set_child(area)

    @staticmethod
    def _radial(cr, cx, cy, r, stops):
        g = cairo.RadialGradient(cx, cy, 0, cx, cy, r)
        for off, col in stops:
            g.add_color_stop_rgba(off, *col)
        cr.set_source(g)
        cr.paint()

    def _draw(self, area, cr, w, h):
        if cairo is None:
            cr.set_source_rgb(0.05, 0.1, 0.3)
            cr.paint()
            return
        # deep blue base
        g = cairo.LinearGradient(0, 0, w * 0.3, h)
        g.add_color_stop_rgb(0, 0.045, 0.075, 0.24)
        g.add_color_stop_rgb(1, 0.015, 0.03, 0.12)
        cr.set_source(g)
        cr.paint()

        cx, cy = w * 0.52, h * 0.52
        base = min(w, h)

        # outer petals
        for i, ang in enumerate([0.4, 1.5, 2.7, 3.9, 5.1]):
            px = cx + math.cos(ang) * base * 0.20
            py = cy + math.sin(ang) * base * 0.16
            self._radial(cr, px, py, base * 0.42,
                         [(0, (0.10, 0.28, 0.78, 0.50)),
                          (0.55, (0.08, 0.20, 0.60, 0.28)),
                          (1, (0, 0, 0, 0))])
        # inner petals brighter
        for ang in [0.9, 2.2, 3.5, 4.7, 5.9]:
            px = cx + math.cos(ang) * base * 0.10
            py = cy + math.sin(ang) * base * 0.08
            self._radial(cr, px, py, base * 0.30,
                         [(0, (0.22, 0.52, 0.98, 0.55)),
                          (1, (0, 0, 0, 0))])
        # bright core
        self._radial(cr, cx, cy, base * 0.22,
                     [(0, (0.55, 0.80, 1.0, 0.85)),
                      (0.4, (0.30, 0.60, 1.0, 0.5)),
                      (1, (0, 0, 0, 0))])


# ------------------------------ start logo -------------------------------

class WinLogo(Gtk.DrawingArea):
    """The four-pane Windows 11 logo."""

    def __init__(self, size=18, color=(1, 1, 1, 1)):
        super().__init__()
        self.size = size
        self.color = color
        self.set_size_request(size + 6, size + 6)
        self.set_draw_func(self._draw)

    def _draw(self, area, cr, w, h):
        s = self.size
        gap = max(1.5, s * 0.08)
        cell = (s - gap) / 2
        x0 = (w - s) / 2
        y0 = (h - s) / 2
        cr.set_source_rgba(*self.color)
        r = cell * 0.18
        for ix in range(2):
            for iy in range(2):
                x = x0 + ix * (cell + gap)
                y = y0 + iy * (cell + gap)
                cr.rectangle(x, y, cell, cell)
        cr.fill()


# ------------------------------- taskbar ---------------------------------

class TaskbarWindow(Gtk.Window):
    def __init__(self, shell):
        super().__init__()
        self.shell = shell
        self.app_buttons = {}   # window id -> (button, indicator)

        self.set_default_size(1280, TASKBAR_H)
        Gtk4LayerShell.init_for_window(self)
        Gtk4LayerShell.set_namespace(self, "ed-taskbar")
        Gtk4LayerShell.set_layer(self, Gtk4LayerShell.Layer.TOP)
        Gtk4LayerShell.set_exclusive_zone(self, TASKBAR_H)
        for edge in (Gtk4LayerShell.Edge.BOTTOM, Gtk4LayerShell.Edge.LEFT,
                     Gtk4LayerShell.Edge.RIGHT):
            Gtk4LayerShell.set_anchor(self, edge, True)

        root_bg = Gtk.Box()
        root_bg.add_css_class("ed-taskbar-root")
        root_bg.set_hexpand(True)
        root_bg.set_vexpand(True)
        self.set_child(root_bg)

        bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        bar.set_valign(Gtk.Align.FILL)
        bar.set_vexpand(True)
        root_bg.append(bar)
        self.bar_box = bar

        # ---- centered group (Win11 style) ----
        center = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        center.set_halign(Gtk.Align.CENTER)
        center.set_margin_start(8)
        center.set_margin_end(8)

        self.start_btn = Gtk.Button()
        self.start_btn.set_child(WinLogo(17))
        self.start_btn.set_tooltip_text("开始")
        self.start_btn.connect("clicked", lambda b: shell.toggle_start_menu())
        center.append(self.start_btn)

        search = Gtk.Button()
        sbox = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        sbox.append(Gtk.Image.new_from_icon_name("system-search-symbolic"))
        lab = Gtk.Label(label="搜索")
        lab.add_css_class("ed-muted")
        sbox.append(lab)
        search.set_child(sbox)
        search.add_css_class("ed-search")
        search.connect("clicked", lambda b: shell.toggle_start_menu(focus_search=True))
        center.append(search)

        self.apps_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=2)
        center.append(self.apps_box)

        # ---- right cluster (Windows 11 system tray) ----
        # Win11: [network+volume+battery pill] [clock/date] [show-desktop sliver]
        right = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=2)
        right.set_margin_end(1)

        tray_pill = Gtk.Button()
        tray_icons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        for icon in ("network-wireless-symbolic", "audio-volume-high-symbolic",
                     "battery-full-symbolic"):
            img = Gtk.Image.new_from_icon_name(icon)
            img.set_pixel_size(15)
            tray_icons.append(img)
        tray_pill.set_child(tray_icons)
        tray_pill.set_tooltip_text("网络、声音和电池（M2 接入快速设置）")
        right.append(tray_pill)

        self.clock = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.clock_time = Gtk.Label(label="--:--")
        self.clock_date = Gtk.Label(label="----/--/--")
        for lab_ in (self.clock_time, self.clock_date):
            lab_.set_halign(Gtk.Align.END)
            self.clock.append(lab_)
        self.clock.add_css_class("ed-clock")
        clock_btn = Gtk.Button()
        clock_btn.set_child(self.clock)
        clock_btn.set_tooltip_text("日期和时间")
        right.append(clock_btn)

        show_desktop = Gtk.Button()
        show_desktop.add_css_class("ed-showdesktop")
        show_desktop.set_tooltip_text("显示桌面")
        show_desktop.connect("clicked", lambda b: self.shell.toggle_show_desktop())
        right.append(show_desktop)

        right.set_valign(Gtk.Align.CENTER)

        # ---- layout: overlay centers the icon group on the full width while
        # the tray stays pinned to the right edge (exactly like Windows 11) ----
        overlay = Gtk.Overlay()
        overlay.set_vexpand(True)
        bar.append(overlay)

        center_wrap = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        center_wrap.set_hexpand(True)
        sp_l = Gtk.Box(); sp_l.set_hexpand(True)
        sp_r = Gtk.Box(); sp_r.set_hexpand(True)
        center_wrap.append(sp_l)
        center_wrap.append(center)
        center_wrap.append(sp_r)
        overlay.set_child(center_wrap)

        right.set_halign(Gtk.Align.END)
        overlay.add_overlay(right)

        self.center_box = center
        self.center_wrap = center_wrap

        GLib.timeout_add_seconds(1, self._tick)
        self._tick()

    def _tick(self):
        dt = GLib.DateTime.new_now_local()
        self.clock_time.set_text(dt.format("%H:%M"))
        self.clock_date.set_text(dt.format("%Y/%m/%d"))
        return True

    # ---- window buttons ----

    def add_window(self, wid, title, app_id):
        if wid in self.app_buttons:
            return
        icon = self.shell.registry.icon_name_for(app_id)
        btn = Gtk.Button()
        stack = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        img = Gtk.Image.new_from_icon_name(icon)
        img.set_pixel_size(24)
        stack.set_halign(Gtk.Align.CENTER)
        stack.append(img)
        ind = Gtk.DrawingArea()
        ind.set_size_request(16, 3)
        ind.add_css_class("ed-indicator")
        ind.set_draw_func(self._draw_indicator)
        stack.append(ind)
        btn.set_child(stack)
        btn.set_tooltip_text(title or app_id or "窗口")
        btn.connect("clicked", lambda b: self.shell.on_taskbar_button(wid))
        self.apps_box.append(btn)
        self.app_buttons[wid] = (btn, ind, False)  # (btn, indicator, focused)
        self._refresh_indicator(wid)

    def _draw_indicator(self, area, cr, w, h):
        wid = None
        for k, (_, ind, _) in self.app_buttons.items():
            if ind is area:
                wid = k
                break
        focused = wid is not None and self.app_buttons[wid][2]
        visible = wid is not None
        if not visible:
            return
        if focused:
            cr.set_source_rgb(0.30, 0.76, 1.0)
            cr.rectangle(0, 0, w, h)
        else:
            cr.set_source_rgba(1, 1, 1, 0.45)
            cr.rectangle(w / 2 - 3, 0, 6, h)
        cr.fill()

    def remove_window(self, wid):
        if wid not in self.app_buttons:
            return
        btn, _, _ = self.app_buttons.pop(wid)
        self.apps_box.remove(btn)

    def set_focused(self, wid):
        for k, (_, ind, _) in list(self.app_buttons.items()):
            self.app_buttons[k] = (self.app_buttons[k][0], ind, k == wid)
            self._refresh_indicator(k)

    def _refresh_indicator(self, wid):
        _, ind, _ = self.app_buttons[wid]
        ind.queue_draw()

    def set_title(self, wid, title):
        if wid in self.app_buttons:
            self.app_buttons[wid][0].set_tooltip_text(title)

    def set_running_style(self, wid, running):
        if wid in self.app_buttons:
            btn = self.app_buttons[wid][0]
            if running:
                btn.add_css_class("running")
            else:
                btn.remove_css_class("running")


# ------------------------------ start menu -------------------------------

class StartMenuWindow(Gtk.Window):
    def __init__(self, shell):
        super().__init__()
        self.shell = shell
        self.set_default_size(MENU_W, MENU_H)
        Gtk4LayerShell.init_for_window(self)
        Gtk4LayerShell.set_namespace(self, "ed-startmenu")
        Gtk4LayerShell.set_layer(self, Gtk4LayerShell.Layer.OVERLAY)
        Gtk4LayerShell.set_anchor(self, Gtk4LayerShell.Edge.BOTTOM, True)
        Gtk4LayerShell.set_margin(self, Gtk4LayerShell.Edge.BOTTOM, TASKBAR_H + 12)

        outer = Gtk.Box()
        outer.add_css_class("ed-startmenu-root")
        outer.set_hexpand(True)
        outer.set_vexpand(True)
        self.set_child(outer)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        root.set_hexpand(True)
        root.set_vexpand(True)
        outer.append(root)

        pad = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        pad.set_margin_top(28)
        pad.set_margin_start(32)
        pad.set_margin_end(32)
        root.append(pad)

        self.search = Gtk.Entry()
        self.search.set_placeholder_text("搜索应用、设置和文档")
        self.search.add_css_class("ed-startmenu-search")
        self.search.connect("activate", self._on_search_activate)
        self.search.connect("changed", self._on_search_changed)
        pad.append(self.search)

        h2 = Gtk.Label(label="已固定")
        h2.set_halign(Gtk.Align.START)
        h2.add_css_class("ed-sm-h2")
        pad.append(h2)

        self.grid = Gtk.FlowBox()
        self.grid.set_max_children_per_line(6)
        self.grid.set_min_children_per_line(6)
        self.grid.set_homogeneous(True)
        self.grid.set_selection_mode(Gtk.SelectionMode.NONE)
        self.grid.set_column_spacing(4)
        self.grid.set_row_spacing(4)
        pad.append(self.grid)

        self._fill_grid("")

        # footer
        footer = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        footer.add_css_class("ed-sm-footer")
        footer.set_margin_top(12)
        footer.set_size_request(-1, 56)
        root.append(footer)

        user = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        user.set_margin_start(40)
        user.set_valign(Gtk.Align.CENTER)
        avatar = Gtk.Image.new_from_icon_name("avatar-default-symbolic")
        avatar.set_pixel_size(28)
        user.append(avatar)
        user.append(Gtk.Label(label="用户"))
        footer.append(user)

        power = Gtk.Button()
        power.set_child(Gtk.Image.new_from_icon_name("system-shutdown-symbolic"))
        power.set_halign(Gtk.Align.END)
        power.set_hexpand(True)
        power.set_margin_end(40)
        power.set_valign(Gtk.Align.CENTER)
        power.set_tooltip_text("电源")
        footer.append(power)

        keyctl = Gtk.EventControllerKey()
        keyctl.connect("key-pressed", self._on_key)
        self.add_controller(keyctl)

    def _on_key(self, ctl, keyval, keycode, state):
        if keyval == 65307:  # Escape
            self.shell.hide_start_menu()
            return True
        return False

    def _clear_grid(self):
        while True:
            child = self.grid.get_first_child()
            if child is None:
                break
            self.grid.remove(child)

    def _fill_grid(self, query):
        self._clear_grid()
        q = query.strip().lower()
        if q:
            entries = [e for e in self.shell.registry.apps if q in e[0].lower()]
        else:
            entries = self.shell.registry.pinned(24)
        if not entries:
            lab = Gtk.Label(label="未找到匹配的应用")
            lab.add_css_class("ed-muted")
            self.grid.append(lab)
            return
        for name, info, icon in entries[:24]:
            btn = Gtk.Button()
            box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
            img = Gtk.Image.new_from_icon_name(icon)
            img.set_pixel_size(40)
            lab = Gtk.Label(label=name)
            lab.set_max_width_chars(10)
            lab.set_ellipsize(3)  # PANGO_ELLIPSIZE_END
            box.append(img)
            box.append(lab)
            btn.set_child(box)
            btn.add_css_class("ed-appcell")
            btn.set_tooltip_text(name)
            btn.connect("clicked", lambda b, i=info: self._launch(i))
            self.grid.append(btn)

    def _launch(self, info):
        try:
            info.launch([], None)
        except GLib.Error as e:
            print("launch failed:", e)
        self.shell.hide_start_menu()

    def _on_search_activate(self, entry):
        # launch the first visible match
        child = self.grid.get_child_at_index(0)
        if child is not None:
            child.activate()

    def _on_search_changed(self, entry):
        self._fill_grid(entry.get_text())

    def open_menu(self, focus_search=True, margin_left=None):
        """margin_left: align the menu's left edge to the taskbar icon group
        (Windows 11 behaviour). None falls back to screen-center."""
        if margin_left is not None:
            Gtk4LayerShell.set_anchor(self, Gtk4LayerShell.Edge.LEFT, True)
            Gtk4LayerShell.set_margin(self, Gtk4LayerShell.Edge.LEFT, margin_left)
        else:
            Gtk4LayerShell.set_anchor(self, Gtk4LayerShell.Edge.LEFT, False)
        Gtk4LayerShell.set_keyboard_mode(self, Gtk4LayerShell.KeyboardMode.EXCLUSIVE)
        self.set_visible(True)
        if focus_search:
            self.search.set_text("")
            self.search.grab_focus()

    def close_menu(self):
        Gtk4LayerShell.set_keyboard_mode(self, Gtk4LayerShell.KeyboardMode.NONE)
        self.set_visible(False)


# ----------------------------- alt-tab overlay ---------------------------

class AltTabWindow(Gtk.Window):
    """Win11 style Alt-Tab switcher overlay (display only, steals no focus)."""

    def __init__(self, shell):
        super().__init__()
        self.shell = shell
        Gtk4LayerShell.init_for_window(self)
        Gtk4LayerShell.set_namespace(self, "ed-alttab")
        Gtk4LayerShell.set_layer(self, Gtk4LayerShell.Layer.OVERLAY)
        Gtk4LayerShell.set_keyboard_mode(self, Gtk4LayerShell.KeyboardMode.NONE)

        self.outer = Gtk.Box()
        self.outer.add_css_class("ed-alttab-root")
        self.set_child(self.outer)
        self.set_visible(False)

    def update(self, order, selected_id):
        while True:
            child = self.outer.get_first_child()
            if child is None:
                break
            self.outer.remove(child)
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        self.outer.append(row)
        for wid in order:
            info = self.shell.windows.get(wid)
            if not info:
                continue
            item = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
            item.add_css_class("ed-alttab-item")
            if wid == selected_id:
                item.add_css_class("selected")
            img = Gtk.Image.new_from_icon_name(
                self.shell.registry.icon_name_for(info.get("app_id", "")))
            img.set_pixel_size(36)
            lab = Gtk.Label(label=(info.get("title") or info.get("app_id") or "")[:28])
            item.append(img)
            item.append(lab)
            row.append(item)

    def open_overlay(self, order, selected_id):
        self.update(order, selected_id)
        self.set_visible(True)

    def close_overlay(self):
        self.set_visible(False)


# -------------------------------- shell ----------------------------------

class Shell:
    def __init__(self):
        self.app = Gtk.Application(application_id="org.elevende.shell")
        self.app.connect("activate", self._activate)
        self.windows = {}       # wid -> dict(title, app_id, minimized)
        self.focused_id = 0
        self.menu_open = False
        self.alttab_active = False
        self.mru = []           # most-recently-used window order
        self.registry = AppRegistry()
        self.taskbar = None
        self.menu = None
        self.ipc = None

    # ---- lifecycle ----

    def _activate(self, app):
        css = Gtk.CssProvider()
        css_path = os.path.join(SHELL_DIR, "style.css")
        try:
            with open(css_path, "rb") as f:
                css.load_from_data(f.read())
        except Exception as e:
            print("css load failed:", e)
        from gi.repository import Gdk
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        self.wallpaper = WallpaperWindow()
        self.wallpaper.set_application(app)
        self.wallpaper.present()

        self.taskbar = TaskbarWindow(self)
        self.taskbar.set_application(app)
        self.taskbar.present()

        self.menu = StartMenuWindow(self)
        self.menu.set_application(app)
        self.menu.set_visible(False)

        self.alttab = AltTabWindow(self)
        self.alttab.set_application(app)

        self.ipc = IpcClient(self._on_ipc_event)

        if os.environ.get("ED_AUTOMENU") == "1":
            GLib.timeout_add_seconds(8, self._auto_menu)
        if os.environ.get("ED_AUTOALTTAB") == "1":
            GLib.timeout_add_seconds(9, self._auto_alttab)

    def _auto_menu(self):
        self.toggle_start_menu()
        return False

    def _auto_alttab(self):
        self.alttab.open_overlay(self.mru, self.focused_id)
        GLib.timeout_add_seconds(6, lambda: (self.alttab.close_overlay(), False)[1])
        return False

    # ---- ipc events ----

    def _on_ipc_event(self, ev):
        t = ev.get("event")
        if t == "hello":
            self.windows = {}
            if self.taskbar:
                for wid in list(self.taskbar.app_buttons.keys()):
                    self.taskbar.remove_window(wid)
            for w in ev.get("windows", []):
                self._add_window(w["id"], w.get("title", ""), w.get("app_id", ""))
            self.focused_id = ev.get("focused", 0)
            if self.taskbar:
                self.taskbar.set_focused(self.focused_id)
        elif t == "window_added":
            self._add_window(ev["id"], ev.get("title", ""), ev.get("app_id", ""))
        elif t == "window_removed":
            self.windows.pop(ev["id"], None)
            if ev["id"] in self.mru:
                self.mru.remove(ev["id"])
            if self.taskbar:
                self.taskbar.remove_window(ev["id"])
            if self.focused_id == ev["id"]:
                self.focused_id = 0
        elif t == "window_title":
            if ev["id"] in self.windows:
                self.windows[ev["id"]]["title"] = ev.get("title", "")
            if self.taskbar:
                self.taskbar.set_title(ev["id"], ev.get("title", ""))
        elif t == "window_focused":
            self.focused_id = ev.get("id", 0)
            if self.focused_id:
                self._touch_mru(self.focused_id)
            if self.taskbar:
                self.taskbar.set_focused(self.focused_id)
            if self.menu_open and self.focused_id != 0:
                self.hide_start_menu()
            if self.alttab_active and self.focused_id:
                self.alttab.open_overlay(self.mru, self.focused_id)
        elif t == "window_state":
            w = self.windows.get(ev["id"])
            if w:
                w["minimized"] = ev.get("minimized", False)
            if self.taskbar:
                self.taskbar.set_running_style(ev["id"], not ev.get("minimized", False))
        elif t == "super":
            self.toggle_start_menu()
        elif t == "alt_tab":
            self.alttab_active = bool(ev.get("active", False))
            if self.alttab_active:
                self.alttab.open_overlay(self.mru, self.focused_id)
            else:
                self.alttab.close_overlay()

    def _add_window(self, wid, title, app_id):
        self.windows[wid] = {"title": title, "app_id": app_id, "minimized": False}
        if wid not in self.mru:
            self.mru.insert(0, wid)
        if self.taskbar:
            self.taskbar.add_window(wid, title, app_id)

    def _touch_mru(self, wid):
        if wid in self.mru:
            self.mru.remove(wid)
        self.mru.insert(0, wid)

    def toggle_show_desktop(self):
        """Windows key + D behaviour: minimize everything, or restore."""
        visible = [w for w in self.windows.values() if not w.get("minimized")]
        if visible:
            for wid, w in self.windows.items():
                if not w.get("minimized"):
                    self.ipc.send(cmd="minimize", id=wid)
        else:
            for wid in self.windows:
                self.ipc.send(cmd="restore", id=wid)

    # ---- actions ----

    def on_taskbar_button(self, wid):
        w = self.windows.get(wid)
        if not w:
            return
        if w.get("minimized"):
            self.ipc.send(cmd="restore", id=wid)
        elif self.focused_id == wid:
            self.ipc.send(cmd="minimize", id=wid)
        else:
            self.ipc.send(cmd="focus", id=wid)

    def _menu_margin_left(self):
        """Left edge of the taskbar icon group, in screen coordinates."""
        try:
            tb = self.taskbar
            ca = tb.center_box.get_allocation()
            wa = tb.center_wrap.get_allocation()
            if ca.width <= 0:
                return None
            x = wa.x + ca.x
            screen_w = tb.get_width() or 1280
            return max(8, min(int(x), screen_w - MENU_W - 8))
        except Exception:
            return None

    def toggle_start_menu(self, focus_search=True):
        if self.menu_open:
            self.hide_start_menu()
        else:
            self.menu.open_menu(focus_search=focus_search,
                                margin_left=self._menu_margin_left())
            self.menu_open = True

    def hide_start_menu(self):
        if self.menu_open:
            self.menu.close_menu()
            self.menu_open = False

    def run(self):
        return self.app.run([])


if __name__ == "__main__":
    import sys
    sys.exit(Shell().run())
