#!/usr/bin/env bash
#  ElevenDE 3.5.1 deb builder
#
#  Compiles every component (C shell, keybind engine, screenshot tool,
#  Explorer file manager, SAS secure screen, RunBox, Qt app suite) and
#  packages everything into elevende_3.5.1_amd64.deb.
#
#  Usage:
#      ./build-deb.sh                 # build deps are auto-installed via apt
#      sudo ./build-deb.sh            # if apt needs root
#
#  Output:  elevende_3.5.1_amd64.deb  (install with: sudo apt install ./xxx.deb)

set -euo pipefail

# Re-exec under bash when started with `sh build-deb.sh`
if [ -z "${BASH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_VERSION="3.5.1"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
BUILD_DIR="${BUILD_DIR:-/tmp/elevende-deb-build}"
PKG_ROOT="$BUILD_DIR/pkgroot"
DEB_NAME="elevende_${PKG_VERSION}_${ARCH}.deb"
PREFIX="/usr/local"
BIN="$PREFIX/bin"
SHARE="$PREFIX/share/elevende-shell"

log()  { echo -e "\033[1;34m==>\033[0m $*"; }
warn() { echo -e "\033[1;33mWARN:\033[0m $*" >&2; }
die()  { echo -e "\033[1;31mERROR:\033[0m $*" >&2; exit 1; }

# ---- 0. detect distro family ----------------------------------------------
# NOTE: os-release defines its own VERSION variable; keep ours under
# PKG_VERSION so sourcing it cannot clobber the package version.
if [ -f /etc/os-release ]; then . /etc/os-release; ID="${ID:-linux}"; else ID=linux; fi
VERSION="$PKG_VERSION"

pkg_install() {   # tolerant installer, one group at a time
    case "$ID" in
        debian|ubuntu|kali|linuxmint|pop|raspbian)
            apt-get install -y --no-install-recommends "$@" || warn "apt failed for: $*" ;;
        fedora|rhel|centos|rocky|almalinux)
            dnf install -y "$@" || warn "dnf failed for: $*" ;;
        arch|manjaro)
            pacman -S --needed --noconfirm "$@" || warn "pacman failed for: $*" ;;
        *) warn "unknown distro; install manually: $*" ;;
    esac
}

# ---- 1. build dependencies --------------------------------------------------
log "installing build dependencies"
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        die "need root (or sudo) to install build dependencies"
    fi
else
    SUDO=""
fi

case "$ID" in
    debian|ubuntu|kali|linuxmint|pop|raspbian)
        $SUDO apt-get update || true
        $SUDO apt-get install -y --no-install-recommends \
            build-essential gcc g++ make cmake ninja-build pkg-config git \
            dpkg-dev || die "cannot install base build tools"
        pkg_install libx11-dev libxft-dev libfontconfig1-dev libfreetype-dev libpng-dev
        pkg_install libxtst-dev libxi-dev
        pkg_install libdbus-1-dev libcrypt-dev libgtk-3-dev
        pkg_install qt6-base-dev qt6-svg-dev qt6-tools-dev libgl1-mesa-dev
        pkg_install libgdk-pixbuf-2.0-dev librsvg2-bin
        ;;
    fedora|rhel|centos|rocky|almalinux)
        $SUDO dnf install -y gcc gcc-c++ make cmake ninja-build pkgconf git dpkg \
            libX11-devel libXft-devel fontconfig-devel freetype-devel libpng-devel \
            libXtst-devel libXi-devel dbus-devel libcrypt-devel gtk3-devel \
            qt6-qtbase-devel qt6-qtsvg-devel mesa-libGL-devel \
            gdk-pixbuf2-devel librsvg2-tools || die "dependency install failed"
        ;;
    arch|manjaro)
        $SUDO pacman -S --needed --noconfirm base-devel cmake ninja pkgconf git dpkg \
            libx11 libxft fontconfig libpng libxtst libxi dbus gtk3 \
            qt6-base qt6-svg gdk-pixbuf2 librsvg || die "dependency install failed"
        ;;
    *) die "unsupported distro: $ID" ;;
esac

command -v pkg-config >/dev/null || die "pkg-config missing"
command -v cmake      >/dev/null || die "cmake missing"
command -v dpkg-deb   >/dev/null || die "dpkg-deb missing (install dpkg-dev)"

# verify required libs (auto-install anything missing)
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
            for m in $MISSING; do $SUDO apt-get install -y --no-install-recommends "$m" || true; done ;;
        fedora|rhel|centos|rocky|almalinux)
            $SUDO dnf install -y $MISSING || true ;;
        arch|manjaro)
            $SUDO pacman -S --needed --noconfirm $MISSING || true ;;
    esac
    check_libs
fi
[ -z "$MISSING" ] || die "still missing dev libraries:$MISSING"

# ---- 2. build all components ------------------------------------------------
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$PKG_ROOT"

log "normalizing source timestamps"
find "$SRC_DIR" -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" \
    -o -name "CMakeLists.txt" -o -name "Makefile" \) -exec touch {} +

log "building the C/Xlib shell"
make -C "$SRC_DIR/shell"

log "building elevende-keybind + elevende-screenshot"
gcc -O2 -Wall -Wextra $(pkg-config --cflags x11) \
    -o "$BUILD_DIR/elevende-keybind" "$SRC_DIR/tools/keybind/keybind.c" \
    $(pkg-config --libs x11)
gcc -O2 $(pkg-config --cflags x11 libpng) \
    -o "$BUILD_DIR/elevende-screenshot" "$SRC_DIR/apps/screenshot/screenshot.c" \
    $(pkg-config --libs x11 libpng)

log "building Explorer for Linux"
# Upstream install() references Wayland-only targets; guard for X11-only builds.
python3 - "$SRC_DIR/Explorer-for-Linux/CMakeLists.txt" <<'PY'
import re, pathlib, sys
p = pathlib.Path(sys.argv[1])
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
cmake -S "$SRC_DIR/Explorer-for-Linux" -B "$BUILD_DIR/explorer" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    > "$BUILD_DIR/explorer-cmake.log" 2>&1 || { tail -20 "$BUILD_DIR/explorer-cmake.log"; die "explorer cmake failed"; }
cmake --build "$BUILD_DIR/explorer" -j"$(nproc)" --target explorer \
    || die "explorer build failed"

log "building SAS for Linux"
SAS_OK=1
cmake -S "$SRC_DIR/SAS-for-Linux" -B "$BUILD_DIR/sas" \
      -DCMAKE_BUILD_TYPE=Release > "$BUILD_DIR/sas-cmake.log" 2>&1 \
    && cmake --build "$BUILD_DIR/sas" -j"$(nproc)" > "$BUILD_DIR/sas-build.log" 2>&1 \
    || { warn "SAS build failed (skipping)"; SAS_OK=0; }

log "building RunBox"
RUNBOX_OK=1
make -C "$SRC_DIR/runbox-linux" > "$BUILD_DIR/runbox.log" 2>&1 \
    || { warn "runbox build failed (skipping)"; RUNBOX_OK=0; }

log "building the Qt app suite"
APPS_OK=1
cmake -S "$SRC_DIR/apps" -B "$BUILD_DIR/apps" \
      -DCMAKE_BUILD_TYPE=Release > "$BUILD_DIR/apps-cmake.log" 2>&1 \
    && cmake --build "$BUILD_DIR/apps" -j"$(nproc)" > "$BUILD_DIR/apps-build.log" 2>&1 \
    || { warn "app suite build failed (skipping)"; APPS_OK=0; }

# ---- 3. stage files into pkgroot -------------------------------------------
log "staging package contents"
cd "$SRC_DIR"

inst() { install "$@"; }   # install(1) into PKG_ROOT via absolute dest paths

# binaries
install -Dm755 shell/elevende-shell  "$PKG_ROOT$BIN/elevende-shell"
[ -x shell/elevende-notifyd ] && install -m755 shell/elevende-notifyd "$PKG_ROOT$BIN/elevende-notifyd"
install -m4755 shell/elevende-lock   "$PKG_ROOT$BIN/elevende-lock"   # setuid-root: reads /etc/shadow
install -m755 "$BUILD_DIR/elevende-keybind"    "$PKG_ROOT$BIN/elevende-keybind"
install -m755 "$BUILD_DIR/elevende-screenshot" "$PKG_ROOT$BIN/elevende-screenshot"
install -m755 "$BUILD_DIR/explorer/bin/explorer.exe" "$PKG_ROOT$BIN/explorer.exe"
[ "$SAS_OK" = 1 ]    && [ -x "$BUILD_DIR/sas/sas-screen" ] && install -m755 "$BUILD_DIR/sas/sas-screen" "$PKG_ROOT$BIN/sas-screen"
[ "$RUNBOX_OK" = 1 ] && [ -x "$SRC_DIR/runbox-linux/runbox" ] && install -m755 "$SRC_DIR/runbox-linux/runbox" "$PKG_ROOT$BIN/runbox"
if [ "$APPS_OK" = 1 ]; then
    for app in elevende-notepad elevende-calc elevende-taskmgr elevende-settings elevende-photos; do
        [ -x "$BUILD_DIR/apps/$app" ] && install -m755 "$BUILD_DIR/apps/$app" "$PKG_ROOT$BIN/$app"
    done
fi

# wm config + theme
install -Dm644 wm/rc.xml          "$PKG_ROOT$SHARE/rc.xml"
install -Dm644 wm/rc.template.xml "$PKG_ROOT$SHARE/rc.template.xml"
install -Dm644 wm/shortcuts.json  "$PKG_ROOT$SHARE/shortcuts.json"
install -Dm644 wm/sas-config.json "$PKG_ROOT$SHARE/sas-config.json"
install -Dm644 wm/menu.xml        "$PKG_ROOT$SHARE/menu.xml"
install -Dm644 wm/picom.conf      "$PKG_ROOT$SHARE/picom.conf"
mkdir -p "$PKG_ROOT$PREFIX/share/themes/ElevenDE/openbox-3"
install -m644 wm/ElevenDE/openbox-3/* "$PKG_ROOT$PREFIX/share/themes/ElevenDE/openbox-3/"

# session + X resources + xsessions entry
install -Dm755 session/elevende-session "$PKG_ROOT$BIN/elevende-session"
install -Dm644 assets/xresources.elevende "$PKG_ROOT/etc/X11/Xresources.d/elevende"
install -Dm644 assets/elevende.desktop "$PKG_ROOT/usr/share/xsessions/elevende.desktop"

# application menu entries
mkdir -p "$PKG_ROOT$PREFIX/share/applications"
for d in explorer xterm notepad settings taskmgr calculator runbox sas-screen photos; do
    [ -f "assets/$d.desktop" ] && install -m644 "assets/$d.desktop" "$PKG_ROOT$PREFIX/share/applications/"
done

# icons + wallpapers
# Always rebuild the local Win11-style icon theme from source SVGs. This makes
# app-entry aliases in assets/icons-svg available in the DEB, including on
# minimal systems without an existing icon theme cache.
if command -v rsvg-convert >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    (
        cd assets
        python3 gen_icons.py
        rm -rf icons
        for f in icons-svg/*.svg; do
            n="$(basename "$f" .svg)"
            for s in 32 48 64 96 128; do
                mkdir -p "icons/${s}x${s}/apps"
                rsvg-convert -w "$s" -h "$s" "$f" -o "icons/${s}x${s}/apps/${n}.png"
            done
            mkdir -p "icons/scalable/apps"
            cp "$f" "icons/scalable/apps/${n}.svg"
        done
    )
fi
if [ -d assets/icons ]; then
    mkdir -p "$PKG_ROOT$SHARE/icons"
    cp -r assets/icons/. "$PKG_ROOT$SHARE/icons/"
fi
mkdir -p "$PKG_ROOT$SHARE/wallpapers"
if ls assets/wallpapers/*.png >/dev/null 2>&1; then
    install -m644 assets/wallpapers/*.png "$PKG_ROOT$SHARE/wallpapers/"
fi

# autologin helper (opt-in; see postinst message)
install -Dm755 tools/elevende-setup-autologin "$PKG_ROOT$BIN/elevende-setup-autologin"

# open-source build material: ship the exact scripts and documentation used to
# produce this binary package, so an installed DEB remains auditable/rebuildable.
DOC_DIR="$PREFIX/share/doc/elevende"
install -Dm755 build-deb.sh "$PKG_ROOT$DOC_DIR/build-deb.sh"
install -Dm755 install.sh "$PKG_ROOT$DOC_DIR/install.sh"
install -Dm644 README.md "$PKG_ROOT$DOC_DIR/README.md"
[ -f TESTING.md ] && install -Dm644 TESTING.md "$PKG_ROOT$DOC_DIR/TESTING.md"
[ -f assets/icons-svg/THIRD_PARTY_NOTICES.md ] && install -Dm644 assets/icons-svg/THIRD_PARTY_NOTICES.md "$PKG_ROOT$DOC_DIR/THIRD_PARTY_NOTICES.md"
install -Dm644 LICENSE "$PKG_ROOT$DOC_DIR/LICENSE"

# ---- 4. DEBIAN metadata -----------------------------------------------------
log "writing DEBIAN metadata"
mkdir -p "$PKG_ROOT/DEBIAN"

# compute shared-library deps automatically
shlibs_depends=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    ( cd "$PKG_ROOT" && mkdir -p DEBIAN
      dpkg-shlibdeps -T > /tmp/shlibs.$$ 2>/dev/null || true
      if [ -f /tmp/shlibs.$$ ]; then
          shlibs_depends=$(sed -n 's/^shlibs:Depends=//p' /tmp/shlibs.$$ | head -1)
          rm -f /tmp/shlibs.$$
      fi
    ) || true
fi

# non-library runtime dependencies (X stack, WM, fonts, MIME database...)
# NetworkManager supplies nmcli for the taskbar WLAN panel. iwd is accepted
# as an alternative backend by sas-screen on installations that use it.
extra_deps="xorg, xinit, openbox, picom, xterm, dbus-x11, x11-xserver-utils, x11-utils, alsa-utils, network-manager | iwd, shared-mime-info, adwaita-icon-theme, hicolor-icon-theme, fonts-dejavu-core, fonts-noto-cjk, fonts-noto-color-emoji, librsvg2-common, libxtst6, libxi6, policykit-1 | polkitd"
if [ -n "$shlibs_depends" ]; then
    DEPENDS="$shlibs_depends, $extra_deps"
else
    # fallback: explicit runtime library packages
    DEPENDS="libx11-6, libxft2, libfontconfig1, libpng16-16, libxtst6, libxi6, libdbus-1-3, libcrypt1, libgtk-3-0t64 | libgtk-3-0, libqt6widgets6, libqt6gui6, libqt6core6, libqt6network6, libqt6dbus6, libgdk-pixbuf-2.0-0, $extra_deps"
fi

cat > "$PKG_ROOT/DEBIAN/control" <<EOF
Package: elevende
Version: $PKG_VERSION
Architecture: $ARCH
Maintainer: ElevenDE Project <elevende@localhost>
Depends: $DEPENDS
Section: x11
Priority: optional
Description: Windows 11 style desktop environment for Linux
 ElevenDE is a Windows 11 styled X11 desktop environment: a native C/Xlib
 shell (taskbar, start menu, tray, calendar, notifications, login/lock
 screen), Openbox window manager with a Win11 dark theme, a Qt6 file
 manager (Explorer), SAS secure attention screen (Ctrl+Alt+Del), RunBox
 (Win+R), and a suite of Win11 styled apps (settings, notepad, task
 manager, calculator, photos).
EOF

cat > "$PKG_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
# mask Ctrl+Alt+Delete reboot (the SAS screen handles the key instead)
if command -v systemctl >/dev/null 2>&1; then
    systemctl mask ctrl-alt-del.target >/dev/null 2>&1 || true
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
# refresh desktop database so the start menu sees the new entries
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/local/share/applications >/dev/null 2>&1 || true
fi
echo ""
echo "ElevenDE installed."
echo "  - To boot straight into ElevenDE:  sudo elevende-setup-autologin"
echo "  - Or select the 'ElevenDE' session in your display manager."
echo "  - Manual start:  startx /usr/local/bin/elevende-session"
EOF
chmod 755 "$PKG_ROOT/DEBIAN/postinst"

cat > "$PKG_ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "purge" ]; then
    rm -f /etc/profile.d/elevende-session.sh
    rm -rf /etc/systemd/system/getty@tty1.service.d/autologin.conf
    if command -v systemctl >/dev/null 2>&1; then
        systemctl unmask ctrl-alt-del.target >/dev/null 2>&1 || true
        systemctl daemon-reload >/dev/null 2>&1 || true
    fi
fi
EOF
chmod 755 "$PKG_ROOT/DEBIAN/postrm"

# ---- 5. build the .deb ------------------------------------------------------
log "building $DEB_NAME"
dpkg-deb --build --root-owner-group "$PKG_ROOT" "$SRC_DIR/$DEB_NAME"

log "done."
echo ""
echo "Package:  $SRC_DIR/$DEB_NAME"
echo "Install:  sudo apt install ./$DEB_NAME"
echo "Then:     sudo elevende-setup-autologin   (boot straight into ElevenDE)"
