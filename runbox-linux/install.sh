#!/usr/bin/env bash
# ============================================================
#  RunBox 一键安装脚本
#  用法: ./install.sh             正常安装并自动绑定 Win+R
#        ./install.sh --no-bind   只安装，不绑定快捷键
# ============================================================
set -e

GREEN='\033[0;32m'; YELLOW='\033[0;33m'; RED='\033[0;31m'; NC='\033[0m'
info() { echo -e "${GREEN}[信息]${NC} $*"; }
warn() { echo -e "${YELLOW}[提示]${NC} $*"; }
err()  { echo -e "${RED}[错误]${NC} $*" >&2; }

NO_BIND=0
for arg in "$@"; do
    case "$arg" in
        --no-bind) NO_BIND=1 ;;
        -h|--help)
            head -n 6 "$0" | grep '^#'
            exit 0
            ;;
        *) warn "忽略未知参数: $arg" ;;
    esac
done

cd "$(dirname "$0")"

if [ "$(id -u)" = "0" ]; then
    err "请以普通用户身份运行本脚本（需要 root 的步骤会自动调用 sudo）。"
    err "原因：全局快捷键必须绑定到当前桌面用户，而不是 root。"
    exit 1
fi
SUDO="sudo"

# ---------- 1. 依赖 ----------
install_deps() {
    info "正在安装编译依赖..."
    if command -v apt-get >/dev/null 2>&1; then
        $SUDO apt-get update -y
        $SUDO apt-get install -y build-essential pkg-config libgtk-3-dev libx11-dev xdg-utils
    elif command -v dnf >/dev/null 2>&1; then
        $SUDO dnf install -y gcc make pkgconfig gtk3-devel libX11-devel xdg-utils
    elif command -v yum >/dev/null 2>&1; then
        $SUDO yum install -y gcc make pkgconfig gtk3-devel libX11-devel xdg-utils
    elif command -v pacman >/dev/null 2>&1; then
        $SUDO pacman -S --needed --noconfirm base-devel pkgconf gtk3 libx11 xdg-utils
    elif command -v zypper >/dev/null 2>&1; then
        $SUDO zypper --non-interactive install gcc make pkg-config gtk3-devel libX11-devel xdg-utils
    elif command -v apk >/dev/null 2>&1; then
        $SUDO apk add build-base pkgconfig gtk+3.0-dev libx11-dev xdg-utils
    else
        err "未识别到支持的包管理器，请手动安装：gcc、make、pkg-config、GTK3 开发包、libX11 开发包、xdg-utils。"
        exit 1
    fi
}

have_cc=0
command -v cc  >/dev/null 2>&1 && have_cc=1
command -v gcc >/dev/null 2>&1 && have_cc=1
if ! pkg-config --exists gtk+-3.0 x11 2>/dev/null || [ "$have_cc" = "0" ] || ! command -v make >/dev/null 2>&1; then
    install_deps
fi
pkg-config --exists gtk+-3.0 x11 || { err "GTK3 开发环境仍不可用，请检查上面的依赖安装输出。"; exit 1; }

# ---------- 2. 编译 ----------
info "正在编译 RunBox..."
make clean >/dev/null 2>&1 || true
make

# ---------- 3. 安装 ----------
info "正在安装到系统 (/usr/local)..."
$SUDO make install
$SUDO update-desktop-database /usr/local/share/applications 2>/dev/null || true
$SUDO gtk-update-icon-cache -f /usr/local/share/icons/hicolor 2>/dev/null || true

# ---------- 4. 绑定全局快捷键 Super+R ----------
KEY_PATH="/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/runbox/"

bind_gnome() {
    command -v gsettings >/dev/null 2>&1 || return 1
    gsettings list-schemas 2>/dev/null | grep -qx 'org.gnome.settings-daemon.plugins.media-keys' || return 1
    local schema="org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:$KEY_PATH"
    gsettings set "$schema" name '运行 (RunBox)' || return 1
    gsettings set "$schema" command 'runbox' || return 1
    gsettings set "$schema" binding '<Super>r' || return 1
    local cur new
    cur=$(gsettings get org.gnome.settings-daemon.plugins.media-keys custom-keybindings)
    if ! printf '%s' "$cur" | grep -qF "$KEY_PATH"; then
        if [ "$cur" = "@as []" ]; then
            new="['$KEY_PATH']"
        else
            new="${cur%]}, '$KEY_PATH']"
        fi
        gsettings set org.gnome.settings-daemon.plugins.media-keys custom-keybindings "$new" || return 1
    fi
    return 0
}

bind_xfce() {
    command -v xfconf-query >/dev/null 2>&1 || return 1
    case "${XDG_CURRENT_DESKTOP:-}" in
        *XFCE*) ;;
        *) return 1 ;;
    esac
    local prop='/commands/custom/<Super>r'
    xfconf-query -c xfce4-keyboard-shortcuts -p "$prop" -s 'runbox' --create -t string 2>/dev/null \
        || xfconf-query -c xfce4-keyboard-shortcuts -p "$prop" -s 'runbox' || return 1
    return 0
}

bind_daemon() {
    [ "${XDG_SESSION_TYPE:-x11}" = "x11" ] || return 1
    [ -n "${DISPLAY:-}" ] || return 1
    mkdir -p "$HOME/.config/autostart"
    cat > "$HOME/.config/autostart/runbox-daemon.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=RunBox Hotkey Daemon
Comment=监听 Super+R 呼出运行对话框
Exec=runbox --daemon
Terminal=false
X-GNOME-Autostart-enabled=true
EOF
    if command -v pgrep >/dev/null 2>&1; then
        pgrep -f 'runbox --daemon' >/dev/null 2>&1 || nohup runbox --daemon >/dev/null 2>&1 &
    else
        nohup runbox --daemon >/dev/null 2>&1 &
    fi
    return 0
}

BIND_OK=""
if [ "$NO_BIND" = "1" ]; then
    warn "已跳过快捷键绑定（--no-bind）。"
elif bind_gnome; then
    BIND_OK="已通过桌面设置绑定快捷键 Super+R（GNOME/Unity/Budgie 等）"
elif bind_xfce; then
    BIND_OK="已通过 xfconf 绑定快捷键 Super+R（XFCE）"
elif bind_daemon; then
    BIND_OK="已启动快捷键守护进程（Super+R，X11），并已加入开机自启"
else
    warn "未能自动绑定快捷键。你可以："
    warn "  1) 在 系统设置 → 快捷键 中，把命令 runbox 绑定到 Super+R；"
    warn "  2) 或在 X11 会话中运行 runbox --daemon。"
fi

echo
info "安装完成！"
[ -n "$BIND_OK" ] && info "$BIND_OK"
echo
echo "使用方法："
echo "  · 按下 Win+R（Super+R）即可呼出\"运行\"对话框"
echo "  · 也可以在应用菜单中找到\"运行 (RunBox)\"打开"
echo "  · 命令行直接运行: runbox"
echo
command -v runbox >/dev/null 2>&1 || warn "注意：/usr/local/bin 可能不在 PATH 中，请重新登录后再试。"
