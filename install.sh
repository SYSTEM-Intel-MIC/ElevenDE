#!/bin/sh
# ============================================================
#  ElevenDE automatic installer (cross-distro)
#
#  Supported families:
#    · Debian / Ubuntu / Linux Mint ............ apt
#    · Fedora / RHEL / CentOS / Rocky .......... dnf
#    · Arch / Manjaro / EndeavourOS ............ pacman
#    · openSUSE ................................ zypper
#    · Alpine .................................. apk
#
#  Strategy:
#    1. detect distro + package manager
#    2. install build/runtime dependencies (best effort per family)
#    3. require wlroots 0.17.x  — use the distro package when the
#       version matches, otherwise build wlroots 0.17.1 from source
#       into /usr/local (rolling-release distros ship newer wlroots)
#    4. require gtk4-layer-shell — distro package if available
#       (Fedora ships it), otherwise build from source
#    5. compile the compositor, install binary/shell/session
#
#  Usage:  sudo ./install.sh
# ============================================================

set -e

PREFIX=/usr/local
SRC_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR=${BUILD_DIR:-/tmp/elevende-build}
WORK_DIR=${WORK_DIR:-/tmp/elevende-deps}
WLROOTS_VER=0.17.1
SUDO=""

log()  { printf '\033[1;34m[elevende]\033[0m %s\n' "$1"; }
warn() { printf '\033[1;33m[elevende]\033[0m %s\n' "$1"; }
die()  { printf '\033[1;31m[elevende]\033[0m %s\n' "$1"; exit 1; }

[ "$(id -u)" = 0 ] || SUDO=sudo
mkdir -p "$WORK_DIR"

# ------------------------------------------------------------
# 1. distro detection
# ------------------------------------------------------------
[ -f /etc/os-release ] || die "/etc/os-release not found — unsupported system"
. /etc/os-release
DISTRO="$ID"
FAMILY=""
case "$DISTRO" in
    debian|ubuntu|linuxmint|pop|elementary|zorin) FAMILY=debian ;;
    fedora|rhel|centos|rocky|almalinux)           FAMILY=fedora ;;
    arch|manjaro|endeavouros|garuda)              FAMILY=arch ;;
    opensuse*|sles)                               FAMILY=suse ;;
    alpine)                                       FAMILY=alpine ;;
    *)
        for like in $ID_LIKE; do
            case "$like" in
                debian*|ubuntu*)  FAMILY=debian ;;
                rhel*|fedora*)    FAMILY=fedora ;;
                arch*)            FAMILY=arch ;;
                suse*)            FAMILY=suse ;;
            esac
            [ -n "$FAMILY" ] && break
        done
        ;;
esac
[ -n "$FAMILY" ] || die "unsupported distribution: $DISTRO (PRs welcome!)"
log "detected: $PRETTY_NAME (family: $FAMILY)"

pkg_install() { # $@ = packages; failures tolerated (some names vary by release)
    case "$FAMILY" in
        debian) $SUDO apt-get install -y --no-install-recommends "$@" ;;
        fedora) $SUDO dnf install -y "$@" ;;
        arch)   $SUDO pacman -S --needed --noconfirm "$@" ;;
        suse)   $SUDO zypper --non-interactive install "$@" ;;
        alpine) $SUDO apk add "$@" ;;
    esac
}

pkg_install_strict() { # abort on failure
    pkg_install "$@" || die "failed to install: $*"
}

log "updating package index..."
case "$FAMILY" in
    debian) $SUDO apt-get update -qq ;;
    fedora) $SUDO dnf makecache -qq || true ;;
    arch)   $SUDO pacman -Sy ;;
    suse)   $SUDO zypper --non-interactive refresh || true ;;
    alpine) $SUDO apk update ;;
esac

# ------------------------------------------------------------
# 2. dependencies
# ------------------------------------------------------------
log "installing toolchain + compositor dependencies..."
case "$FAMILY" in
    debian)
        pkg_install_strict meson ninja-build pkg-config gcc g++ git ca-certificates \
            libwayland-dev wayland-protocols libxkbcommon-dev libinput-dev \
            libdrm-dev libgbm-dev libseat-dev libpixman-1-dev libudev-dev \
            libegl-dev libgles-dev libgtk-4-dev python3-gi gir1.2-gtk-4.0 \
            libgirepository1.0-dev gobject-introspection xwayland desktop-file-utils
        pkg_install libwlroots-dev libdisplay-info-dev hwdata \
            libxcb1-dev libxcb-render0-dev libxcb-xfixes0-dev libxcb-icccm4-dev \
            libxcb-composite0-dev libxcb-res0-dev libxcb-ewmh-dev \
            gtk-4-examples xterm adwaita-icon-theme fonts-noto-cjk \
            python3-gi-cairo grim ;;
    fedora)
        pkg_install_strict meson ninja-build pkgconf-pkg-config gcc gcc-c++ git \
            wayland-devel wayland-protocols-devel libxkbcommon-devel \
            libinput-devel libdrm-devel mesa-libgbm-devel libseat-devel \
            pixman-devel systemd-devel mesa-libEGL-devel mesa-libGLES-devel \
            gtk4-devel python3-gobject gobject-introspection-devel \
            xorg-x11-server-Xwayland desktop-file-utils
        pkg_install wlroots-devel libdisplay-info-devel hwdata \
            libxcb-devel xcb-util-wm-devel \
            gtk4-layer-shell-devel python3-pycairo \
            xterm adwaita-icon-theme google-noto-sans-cjk-ttc-fonts grim ;;
    arch)
        pkg_install_strict meson ninja pkgconf gcc git wayland wayland-protocols \
            libxkbcommon libinput libdrm mesa libseat pixman systemd \
            gtk4 python-gobject gobject-introspection xorg-xwayland \
            desktop-file-utils
        pkg_install wlroots libdisplay-info hwdata xcb-util-wm \
            python-cairo xterm adwaita-icons noto-fonts-cjk grim ;;
    suse)
        pkg_install_strict meson ninja pkg-config gcc gcc-c++ git \
            wayland-devel wayland-protocols-devel libxkbcommon-devel \
            libinput-devel libdrm-devel Mesa-libgbm-devel libseat-devel \
            pixman-devel systemd-devel Mesa-libEGL-devel Mesa-libGLES-devel \
            gtk4-devel python3-gobject gobject-introspection-devel \
            desktop-file-utils
        pkg_install wlroots-devel xwayland libdisplay-info-devel hwdata \
            python3-cairo xterm adwaita-icon-theme noto-sans-cjk-fonts grim ;;
    alpine)
        pkg_install_strict meson ninja pkgconf gcc musl-dev git \
            wayland-dev wayland-protocols libxkbcommon-dev libinput-dev \
            libdrm-dev mesa-gbm libseat-dev pixman-dev eudev-dev \
            mesa-egl mesa-gles gtk4.0-dev py3-gobject3 \
            gobject-introspection-dev xwayland-server desktop-file-utils
        pkg_install wlroots-dev libdisplay-info-dev hwdata \
            py3-cairo xterm adwaita-icon-theme font-noto-cjk grim ;;
esac

command -v meson >/dev/null || die "meson missing — install it and re-run"
command -v ninja >/dev/null || die "ninja missing — install it and re-run"

# ------------------------------------------------------------
# 3. wlroots 0.17.x (distro package or source build)
# ------------------------------------------------------------
wlroots_ok() {
    pkg-config --exists 'wlroots >= 0.17.0' 2>/dev/null && \
    ! pkg-config --exists 'wlroots >= 0.18.0' 2>/dev/null
}

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:${PKG_CONFIG_PATH:-}"

if wlroots_ok; then
    log "using system wlroots $(pkg-config --modversion wlroots)"
else
    sys_ver=$(pkg-config --modversion wlroots 2>/dev/null || echo "none")
    warn "system wlroots ($sys_ver) is not 0.17.x — building wlroots $WLROOTS_VER from source"
    pkg_install libdisplay-info-dev hwdata libliftoff-dev 2>/dev/null || true
    cd "$WORK_DIR"
    if [ ! -d wlroots-$WLROOTS_VER ]; then
        git clone --depth 1 --branch $WLROOTS_VER \
            https://gitlab.freedesktop.org/wlroots/wlroots.git wlroots-$WLROOTS_VER
    fi
    cd wlroots-$WLROOTS_VER
    rm -rf build
    meson setup build --prefix=$PREFIX -Dexamples=false -Dxwayland=enabled
    ninja -C build
    $SUDO ninja -C build install
    $SUDO ldconfig 2>/dev/null || true
    wlroots_ok || die "wlroots $WLROOTS_VER build failed"
    log "wlroots $WLROOTS_VER installed to $PREFIX"
fi

# ------------------------------------------------------------
# 4. gtk4-layer-shell (distro package or source build)
# ------------------------------------------------------------
if pkg-config --exists gtk4-layer-shell-0; then
    log "using system gtk4-layer-shell $(pkg-config --modversion gtk4-layer-shell-0)"
else
    log "building gtk4-layer-shell from source..."
    cd "$WORK_DIR"
    if [ ! -d gtk4-layer-shell ]; then
        git clone --depth 1 https://github.com/wmww/gtk4-layer-shell.git
    fi
    cd gtk4-layer-shell
    rm -rf build
    meson setup build --prefix=$PREFIX -Dexamples=false -Ddocs=false -Dvapi=false
    ninja -C build
    $SUDO ninja -C build install
    $SUDO ldconfig 2>/dev/null || true
    pkg-config --exists gtk4-layer-shell-0 || die "gtk4-layer-shell build failed"
fi

# ------------------------------------------------------------
# 5. build + install ElevenDE
# ------------------------------------------------------------
log "compiling the ElevenDE compositor..."
cd "$SRC_DIR"
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    meson setup "$BUILD_DIR" "$SRC_DIR/compositor"
else
    meson configure "$BUILD_DIR" >/dev/null 2>&1 || meson setup "$BUILD_DIR" "$SRC_DIR/compositor"
fi
ninja -C "$BUILD_DIR"

log "installing..."
$SUDO install -Dm755 "$BUILD_DIR/elevende" "$PREFIX/bin/elevende"
$SUDO install -dm755 "$PREFIX/share/elevende/shell"
$SUDO install -Dm644 "$SRC_DIR/shell/shell.py"   "$PREFIX/share/elevende/shell/shell.py"
$SUDO install -Dm644 "$SRC_DIR/shell/style.css"  "$PREFIX/share/elevende/shell/style.css"
$SUDO install -Dm755 "$SRC_DIR/session/elevende-session" "$PREFIX/bin/elevende-session"
$SUDO install -Dm644 "$SRC_DIR/session/elevende.desktop" \
    "$PREFIX/share/wayland-sessions/elevende.desktop"
$SUDO install -Dm644 "$SRC_DIR/assets/xterm.desktop" \
    /usr/local/share/applications/xterm.desktop
$SUDO install -Dm644 "$SRC_DIR/assets/gtk4-demo.desktop" \
    /usr/local/share/applications/gtk4-demo.desktop
update-desktop-database /usr/local/share/applications 2>/dev/null || true

log "done."
echo
echo "  ▸ Log out, pick “ElevenDE” on the login screen, log back in."
echo "  ▸ Headless test:"
echo "      export XDG_RUNTIME_DIR=/tmp/ed WLR_BACKENDS=headless WLR_RENDERER=pixman"
echo "      mkdir -p \$XDG_RUNTIME_DIR"
echo "      $PREFIX/bin/elevende -s 'python3 $PREFIX/share/elevende/shell/shell.py'"
echo
