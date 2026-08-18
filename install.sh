#!/usr/bin/env bash
#  ElevenDE 3.5.1 installer (Windows 11 style X11 desktop environment)
#
#  Components built & installed:
#    - elevende-shell / elevende-lock / elevende-notifyd   (C/Xlib core shell)
#    - elevende-keybind                                    (shortcut engine)
#    - elevende-screenshot                                 (PrintScreen tool)
#    - explorer.exe          Explorer for Linux (Qt6 file manager)
#    - sas-screen            SAS for Linux (Ctrl+Alt+Del secure screen, Qt6)
#    - runbox                RunBox (Win+R dialog, GTK3)
#    - elevende-notepad / elevende-settings / elevende-taskmgr / elevende-calc
#                            (Qt6 Win11-style apps)
#
#  Session: console auto-login on tty1 -> startx /usr/local/bin/elevende-session
#  (an xsessions entry is installed too, for display managers).
#
#  Usage:  sudo ./install.sh
#          SET_DEFAULT_TARGET=no sudo ./install.sh   (keep graphical.target)

# Re-exec under bash when started with `sh install.sh` (dash on Kali/Debian
# lacks bash semantics this script relies on).
if [ -z "${BASH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi

set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="${BUILD_DIR:-/tmp/elevende-build}"
SET_DEFAULT_TARGET="${SET_DEFAULT_TARGET:-yes}"

SHARE="$PREFIX/share/elevende-shell"
BIN="$PREFIX/bin"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run with sudo" >&2
    exit 1
fi
LOGIN_USER="${SUDO_USER:-$(ls -1 /home 2>/dev/null | head -1)}"
[ -n "$LOGIN_USER" ] || LOGIN_USER="$(id -un)"

log() { echo -e "\033[1;34m==>\033[0m $*"; }
warn() { echo -e "\033[1;33mWARN:\033[0m $*" >&2; }
die() { echo -e "\033[1;31mERROR:\033[0m $*" >&2; exit 1; }

if [ -f /etc/os-release ]; then . /etc/os-release; ID="${ID:-linux}"; else ID=linux; fi
echo "Detected distro: $ID"

# ---- dependencies -------------------------------------------------------
pkg() {
    case "$ID" in
        debian|ubuntu|kali|linuxmint|pop|raspbian)
            apt-get install -y --no-install-recommends "$@" || true ;;
        fedora|rhel|centos|rocky|almalinux)
            dnf install -y "$@" || true ;;
        arch|manjaro)
            pacman -S --needed --noconfirm "$@" || true ;;
        *)
            warn "unknown distro, install deps manually: $*" ;;
    esac
}

log "installing dependencies"
case "$ID" in
    debian|ubuntu|kali|linuxmint|pop|raspbian)
        apt-get update || true
        # core X stack + WM + compositor
        # NOTE: independent groups on purpose -- one unavailable package
        # name (e.g. policykit-1 on kali-rolling) must not abort the rest.
        pkg xorg xinit openbox picom xterm
        pkg fonts-dejavu-core fonts-noto-color-emoji fonts-noto-cjk
        pkg adwaita-icon-theme hicolor-icon-theme
        pkg dbus-x11 x11-xserver-utils x11-utils alsa-utils
        # nmcli（NetworkManager）是任务栏 WLAN 面板的默认连接后端；
        # SAS 也兼容 iwd，但源码安装默认提供 NetworkManager。
        pkg network-manager
        pkg shared-mime-info file-roller
        # polkit agent: policykit-1 on Debian/Ubuntu, polkitd on kali-rolling
        pkg policykit-1 || pkg polkitd pkexec || true
        pkg build-essential gcc make cmake ninja-build pkg-config git
        pkg libx11-dev libxft-dev libfontconfig1-dev libfreetype-dev libpng-dev
        pkg libxtst-dev libxi-dev
        pkg libdbus-1-dev libcrypt-dev libgtk-3-dev
        pkg qt6-base-dev qt6-svg-dev qt6-tools-dev libgl1-mesa-dev
        pkg librsvg2-common
        # optional but recommended: pactl for the Settings > Sound page
        pkg pulseaudio-utils || true
        # gdk-pixbuf dev package name changed across Debian/Kali releases;
        # probe for it (needed by the shell to render SVG/PNG app icons).
        if ! pkg-config --exists gdk-pixbuf-2.0 2>/dev/null; then
            pkg libgdk-pixbuf-2.0-dev \
                || pkg libgdk-pixbuf2.0-dev \
                || true
        fi
        ;;
    fedora|rhel|centos|rocky|almalinux)
        pkg xorg-x11-xinit openbox picom xterm
        pkg gcc gcc-c++ make cmake ninja-build pkgconf git
        pkg libX11-devel libXft-devel fontconfig-devel freetype-devel libpng-devel
        pkg libXtst-devel libXi-devel
        pkg dbus-devel libcrypt-devel gtk3-devel
        pkg qt6-qtbase-devel qt6-qtsvg-devel mesa-libGL-devel
        pkg alsa-utils xorg-x11-utils librsvg2 NetworkManager ;;
    arch|manjaro)
        pkg xorg xorg-xinit openbox picom xterm
        pkg base-devel cmake ninja pkgconf git
        pkg libx11 libxft fontconfig libpng
        pkg libxtst libxi
        pkg dbus gtk3 qt6-base qt6-svg
        pkg alsa-utils librsvg networkmanager ;;
    *)
        warn "unsupported distro family; please install: Xorg, openbox, picom,"
        warn "gcc, make, cmake, pkg-config, X11/Xft/Xtst/Xi dev libs, dbus,"
        warn "gtk3 dev, Qt6 (base/svg) dev, gdk-pixbuf, libpng dev, NetworkManager or iwd"
        ;;
esac

command -v pkg-config >/dev/null || die "pkg-config is required but missing"
command -v cmake >/dev/null || die "cmake is required but missing"

# verify the libraries the build actually needs; auto-install any missing
check_libs() {
    MISSING=""
    pkg-config --exists xtst  || MISSING="$MISSING libxtst-dev"
    pkg-config --exists xi    || MISSING="$MISSING libxi-dev"
    pkg-config --exists x11   || MISSING="$MISSING libx11-dev"
    pkg-config --exists xft   || MISSING="$MISSING libxft-dev"
    pkg-config --exists Qt6Widgets || MISSING="$MISSING qt6-base-dev"
    pkg-config --exists gtk+-3.0   || MISSING="$MISSING libgtk-3-dev"
    pkg-config --exists dbus-1     || MISSING="$MISSING libdbus-1-dev"
}
check_libs
if [ -n "$MISSING" ]; then
    log "auto-installing missing dev libraries:$MISSING"
    case "$ID" in
        debian|ubuntu|kali|linuxmint|pop|raspbian)
            for m in $MISSING; do
                apt-get install -y --no-install-recommends "$m" \
                    || warn "could not install $m"
            done ;;
        fedora|rhel|centos|rocky|almalinux)
            dnf install -y $MISSING || warn "could not install:$MISSING" ;;
        arch|manjaro)
            pacman -S --needed --noconfirm $MISSING || warn "could not install:$MISSING" ;;
        *) warn "please install manually:$MISSING" ;;
    esac
    check_libs
fi
if [ -n "$MISSING" ]; then
    warn "still missing dev libraries:$MISSING"
    if echo "$MISSING" | grep -q libxtst; then
        warn "without libxtst the anti-ghost-move defense will be DISABLED!"
    fi
    die "cannot continue without the required libraries"
fi

mkdir -p "$BUILD_DIR"

# ---- 1. native C shell ---------------------------------------------------
log "normalizing source timestamps (avoids make clock-skew warnings)"
find "$SRC_DIR" -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" \
    -o -name "CMakeLists.txt" -o -name "Makefile" \) -exec touch {} +

log "building the C/Xlib shell"
find "$SRC_DIR/shell" -type f -exec touch {} +
make -C "$SRC_DIR/shell"

# ---- 2. shortcut engine + screenshot tool (C) ---------------------------
log "building elevende-keybind and elevende-screenshot"
gcc -O2 -Wall -Wextra $(pkg-config --cflags x11) \
    -o "$BUILD_DIR/elevende-keybind" "$SRC_DIR/tools/keybind/keybind.c" \
    $(pkg-config --libs x11)
gcc -O2 -Wall -Wextra $(pkg-config --cflags x11 libpng) \
    -o "$BUILD_DIR/elevende-screenshot" "$SRC_DIR/apps/screenshot/screenshot.c" \
    $(pkg-config --libs x11 libpng)

# ---- 3. Explorer for Linux (Qt6 file manager) ----------------------------
log "building Explorer for Linux"
EXPLORER_SRC="$SRC_DIR/Explorer-for-Linux"
[ -f "$EXPLORER_SRC/CMakeLists.txt" ] || die "Explorer source incomplete"
# Upstream install() references Wayland-only targets; guard for X11-only builds.
python3 - <<PY
import re, pathlib
p = pathlib.Path("$EXPLORER_SRC") / "CMakeLists.txt"
t = p.read_text()
t, n = re.subn(
    r"install\(TARGETS winlogin explorer crashguard killall\s*\n\s*RUNTIME DESTINATION bin\)",
    "if(TARGET explorer)\n  install(TARGETS explorer RUNTIME DESTINATION bin)\nendif()",
    t,
)
if n:
    p.write_text(t)
    print("patched Explorer install()")
PY
cmake -B "$BUILD_DIR/explorer" -S "$EXPLORER_SRC" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$BUILD_DIR/explorer-cmake.log" 2>&1 \
    || { tail -20 "$BUILD_DIR/explorer-cmake.log"; die "explorer cmake failed"; }
cmake --build "$BUILD_DIR/explorer" -j"$(nproc 2>/dev/null || echo 4)" --target explorer
EXPLORER_BIN="$BUILD_DIR/explorer/bin/explorer.exe"
[ -x "$EXPLORER_BIN" ] || die "explorer.exe not built"

# ---- 4. SAS for Linux (Ctrl+Alt+Del secure screen, Qt6) ------------------
log "building SAS for Linux"
SAS_SRC="$SRC_DIR/SAS-for-Linux"
if [ -f "$SAS_SRC/CMakeLists.txt" ]; then
    cmake -B "$BUILD_DIR/sas" -S "$SAS_SRC" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$BUILD_DIR/sas-cmake.log" 2>&1 \
        || { tail -20 "$BUILD_DIR/sas-cmake.log"; warn "SAS build failed - skipping"; SAS_SRC=""; }
    if [ -n "$SAS_SRC" ]; then
        cmake --build "$BUILD_DIR/sas" -j"$(nproc 2>/dev/null || echo 4)" \
            || { warn "SAS build failed - skipping"; SAS_SRC=""; }
    fi
else
    warn "SAS source missing - skipping"
    SAS_SRC=""
fi

# ---- 5. RunBox (Win+R dialog, GTK3) --------------------------------------
log "building RunBox"
RUNBOX_SRC="$SRC_DIR/runbox-linux"
if [ -f "$RUNBOX_SRC/Makefile" ]; then
    make -C "$RUNBOX_SRC" > "$BUILD_DIR/runbox.log" 2>&1 \
        || { tail -10 "$BUILD_DIR/runbox.log"; warn "runbox build failed - skipping"; RUNBOX_SRC=""; }
else
    warn "runbox source missing - skipping"
    RUNBOX_SRC=""
fi

# ---- 6. Qt application suite (notepad/settings/taskmgr/calculator) -------
log "building the Qt app suite"
APPS_OK=1
cmake -B "$BUILD_DIR/apps" -S "$SRC_DIR/apps" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$BUILD_DIR/apps-cmake.log" 2>&1 \
    || { tail -20 "$BUILD_DIR/apps-cmake.log"; warn "apps cmake failed"; APPS_OK=0; }
if [ "$APPS_OK" = 1 ]; then
    cmake --build "$BUILD_DIR/apps" -j"$(nproc 2>/dev/null || echo 4)" \
        || { warn "apps build failed"; APPS_OK=0; }
fi

# ---- wallpapers ------------------------------------------------------------
log "installing wallpapers"
mkdir -p "$SHARE/wallpapers"
if ls "$SRC_DIR"/assets/wallpapers/*.png >/dev/null 2>&1; then
    install -m644 "$SRC_DIR"/assets/wallpapers/*.png "$SHARE/wallpapers/"
elif command -v python3 >/dev/null 2>&1; then
    warn "no prebuilt wallpapers; generating with python3 (takes a few minutes)"
    ( cd "$SRC_DIR/assets" && python3 gen_wallpapers.py ) || warn "wallpaper generation failed"
    install -m644 "$SRC_DIR"/assets/wallpapers/*.png "$SHARE/wallpapers/" 2>/dev/null || true
fi

# ---- Win11 icon set ---------------------------------------------------------
log "installing the Win11 icon set"
# Rebuild from the versioned SVG sources so all application aliases are
# available even when the source archive deliberately omits generated PNGs.
if command -v rsvg-convert >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    ( cd "$SRC_DIR/assets" && python3 gen_icons.py &&
      rm -rf icons &&
      for f in icons-svg/*.svg; do
          n=$(basename "$f" .svg)
          for s in 32 48 64 96 128; do
              mkdir -p "icons/${s}x${s}/apps"
              rsvg-convert -w "$s" -h "$s" "$f" -o "icons/${s}x${s}/apps/$n.png"
          done
          mkdir -p "icons/scalable/apps"
          cp "$f" "icons/scalable/apps/$n.svg"
      done ) || warn "icon generation failed"
fi
if [ -d "$SRC_DIR/assets/icons" ]; then
    mkdir -p "$SHARE/icons"
    cp -r "$SRC_DIR/assets/icons/." "$SHARE/icons/"
fi

# ---- default file associations (MIME) --------------------------------------
log "installing default application associations"
install -Dm644 "$SRC_DIR/assets/mimeapps.list" /usr/share/applications/mimeapps.list
# per-user merge (xdg prefers ~/.config/mimeapps.list when present)
if [ -n "$LOGIN_USER" ]; then
    UHOME=$(getent passwd "$LOGIN_USER" | cut -d: -f6)
    if [ -n "$UHOME" ]; then
        mkdir -p "$UHOME/.config"
        if [ ! -f "$UHOME/.config/mimeapps.list" ]; then
            cp "$SRC_DIR/assets/mimeapps.list" "$UHOME/.config/mimeapps.list"
            chown "$LOGIN_USER":"$(id -gn "$LOGIN_USER")" "$UHOME/.config/mimeapps.list" 2>/dev/null || true
        fi
    fi
fi

# ---- install everything ----------------------------------------------------
log "installing ElevenDE files"

# core shell binaries
install -Dm755 "$SRC_DIR/shell/elevende-shell" "$BIN/elevende-shell"
[ -x "$SRC_DIR/shell/elevende-notifyd" ] && install -m755 "$SRC_DIR/shell/elevende-notifyd" "$BIN/elevende-notifyd"
# login/lock screen: setuid-root so it may read /etc/shadow
[ -x "$SRC_DIR/shell/elevende-lock" ] && install -m4755 "$SRC_DIR/shell/elevende-lock" "$BIN/elevende-lock"

# C tools
install -m755 "$BUILD_DIR/elevende-keybind" "$BIN/elevende-keybind"
install -m755 "$BUILD_DIR/elevende-screenshot" "$BIN/elevende-screenshot"

# file manager / SAS / runbox
install -m755 "$EXPLORER_BIN" "$BIN/explorer.exe"
if [ -n "$SAS_SRC" ] && [ -x "$BUILD_DIR/sas/sas-screen" ]; then
    install -m755 "$BUILD_DIR/sas/sas-screen" "$BIN/sas-screen"
fi
if [ -n "$RUNBOX_SRC" ] && [ -x "$RUNBOX_SRC/runbox" ]; then
    install -m755 "$RUNBOX_SRC/runbox" "$BIN/runbox"
fi

# Qt apps
if [ "$APPS_OK" = 1 ]; then
    for app in elevende-notepad elevende-calc elevende-taskmgr elevende-settings elevende-photos; do
        [ -x "$BUILD_DIR/apps/$app" ] && install -m755 "$BUILD_DIR/apps/$app" "$BIN/$app"
    done
fi

# window manager config: template (source of truth) + static fallback rc.xml
install -Dm644 "$SRC_DIR/wm/rc.xml"          "$SHARE/rc.xml"
install -Dm644 "$SRC_DIR/wm/rc.template.xml" "$SHARE/rc.template.xml"
install -Dm644 "$SRC_DIR/wm/shortcuts.json"  "$SHARE/shortcuts.json"
install -Dm644 "$SRC_DIR/wm/sas-config.json" "$SHARE/sas-config.json"
install -Dm644 "$SRC_DIR/wm/menu.xml"        "$SHARE/menu.xml"
install -Dm644 "$SRC_DIR/wm/picom.conf"      "$SHARE/picom.conf"
mkdir -p "$PREFIX/share/themes/ElevenDE/openbox-3"
install -m644 "$SRC_DIR"/wm/ElevenDE/openbox-3/* "$PREFIX/share/themes/ElevenDE/openbox-3/"

# session entry
install -Dm755 "$SRC_DIR/session/elevende-session" "$BIN/elevende-session"
install -Dm644 "$SRC_DIR/assets/xresources.elevende" /etc/X11/Xresources.d/elevende
install -Dm644 "$SRC_DIR/assets/elevende.desktop" /usr/share/xsessions/elevende.desktop

# Upgrade migration: stale per-user rc.xml files preserve the pre-3.4.2 mouse
# bindings. Remove only files without the revision marker; the next session
# regenerates them from the installed template and preserves current shortcuts.
USER_HOME="$(getent passwd "$LOGIN_USER" 2>/dev/null | cut -d: -f6)"
[ -n "$USER_HOME" ] || USER_HOME="/home/$LOGIN_USER"
USER_RC="$USER_HOME/.config/elevende/rc.xml"
if [ -f "$USER_RC" ] && ! grep -q 'ELEVENDE-RC-REV: 3.4.2-input-routing' "$USER_RC"; then
    log "removing legacy Openbox input configuration for $LOGIN_USER"
    rm -f "$USER_RC"
fi

# application menu entries (start menu scans these directories)
mkdir -p "$PREFIX/share/applications"
for d in explorer xterm notepad settings taskmgr calculator runbox sas-screen photos; do
    [ -f "$SRC_DIR/assets/$d.desktop" ] && install -m644 "$SRC_DIR/assets/$d.desktop" "$PREFIX/share/applications/"
done

# ---- Ctrl+Alt+Delete: hand it to SAS instead of rebooting ----------------
if command -v systemctl >/dev/null 2>&1; then
    log "masking ctrl-alt-del.target (SAS screen will handle the key)"
    systemctl mask ctrl-alt-del.target >/dev/null 2>&1 || true
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

# ---- replace the display manager: boot straight into ElevenDE --------------
# A running GDM/LightDM/SDDM would take over the boot and start the OLD
# desktop instead. Disable them; the console auto-login below starts X.
# (Set KEEP_DISPLAY_MANAGER=yes to skip this step.)
if [ "${KEEP_DISPLAY_MANAGER:-no}" != "yes" ] && command -v systemctl >/dev/null 2>&1; then
    for dm in gdm3 gdm lightdm sddm slim; do
        if systemctl list-unit-files 2>/dev/null | grep -q "^$dm\.service"; then
            log "disabling display manager: $dm (ElevenDE takes over the boot)"
            systemctl disable "$dm.service" >/dev/null 2>&1 || true
        fi
    done
fi

# ---- TTY auto-login + startx hook -----------------------------------------
log "configuring tty1 auto-login for user '$LOGIN_USER'"
mkdir -p /etc/systemd/system/getty@tty1.service.d
cat > /etc/systemd/system/getty@tty1.service.d/autologin.conf <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $LOGIN_USER --noclear tty1 linux
EOF
systemctl enable getty@tty1.service >/dev/null 2>&1 || true

cat > /etc/profile.d/elevende-session.sh <<EOF
# ElevenDE: start the desktop automatically on the tty1 console login.
# Force a UTF-8 locale so terminals/apps render CJK and special chars.
export LANG=C.UTF-8
export LC_ALL=C.UTF-8
if [ -z "\${DISPLAY:-}" ] && [ "\${XDG_VTNR:-}" = "1" ] && [ "\$(id -un)" = "$LOGIN_USER" ]; then
    exec startx /usr/local/bin/elevende-session
fi
EOF

if [ "$SET_DEFAULT_TARGET" = "yes" ]; then
    log "setting default boot target to multi-user (boots to console)"
    systemctl set-default multi-user.target >/dev/null 2>&1 || true
fi

# ---- cleanup old installations ---------------------------------------------
rm -f /usr/share/wayland-sessions/elevende.desktop 2>/dev/null || true

log "done."
echo
echo "ElevenDE 3.3 installed. Reboot: it boots to a console, auto-logs-in"
echo "as '$LOGIN_USER' and starts the desktop via startx."
echo
echo "  Windows shortcuts (manage them in Settings -> 快捷键, Win+I):"
echo "    Win           start menu        Win+E   file explorer"
echo "    Win+R         run dialog        Win+I   settings"
echo "    Win+D         show desktop      Win+L   lock"
echo "    Win+arrows    snap/max/min      Alt+F4  close"
echo "    Alt+Tab       switch windows    Ctrl+Shift+Esc  task manager"
echo "    Win+Shift+N   notepad           PrintScreen     screenshot"
echo "    Ctrl+Alt+Del  secure options screen (SAS)"
echo
echo "  Built-in apps: explorer, notepad, settings, task manager, calculator,"
echo "  runbox, sas-screen. Wallpapers: Settings -> 个性化."
echo
echo "Troubleshooting logs: /tmp/elevende-shell.log /tmp/openbox.log"
echo "                      /tmp/elevende-notifyd.log /tmp/elevende-sas.log"
echo "                      /tmp/elevende-keybind.log"
