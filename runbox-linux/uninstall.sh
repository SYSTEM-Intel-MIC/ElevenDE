#!/usr/bin/env bash
# ============================================================
#  RunBox 卸载脚本：移除程序文件、快捷键绑定与守护进程
# ============================================================
set -e

GREEN='\033[0;32m'; YELLOW='\033[0;33m'; RED='\033[0;31m'; NC='\033[0m'
info() { echo -e "${GREEN}[信息]${NC} $*"; }
warn() { echo -e "${YELLOW}[提示]${NC} $*"; }
err()  { echo -e "${RED}[错误]${NC} $*" >&2; }

cd "$(dirname "$0")"

if [ "$(id -u)" = "0" ]; then
    err "请以普通用户身份运行本脚本（需要 root 的步骤会自动调用 sudo）。"
    exit 1
fi
SUDO="sudo"

KEY_PATH="/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/runbox/"

# 1) 移除 GNOME 系快捷键
if command -v gsettings >/dev/null 2>&1 \
    && gsettings list-schemas 2>/dev/null | grep -qx 'org.gnome.settings-daemon.plugins.media-keys'; then
    cur=$(gsettings get org.gnome.settings-daemon.plugins.media-keys custom-keybindings 2>/dev/null || echo "@as []")
    if printf '%s' "$cur" | grep -qF "$KEY_PATH"; then
        if [ "$cur" = "['$KEY_PATH']" ]; then
            new="@as []"
        else
            new=$(printf '%s' "$cur" | sed "s|, '$KEY_PATH'||; s|'$KEY_PATH', ||")
        fi
        gsettings set org.gnome.settings-daemon.plugins.media-keys custom-keybindings "$new" || true
        info "已移除 GNOME 快捷键绑定。"
    fi
    gsettings reset-recursively "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:$KEY_PATH" 2>/dev/null || true
fi

# 2) 移除 XFCE 快捷键
if command -v xfconf-query >/dev/null 2>&1; then
    case "${XDG_CURRENT_DESKTOP:-}" in
        *XFCE*)
            xfconf-query -c xfce4-keyboard-shortcuts -p '/commands/custom/<Super>r' --reset 2>/dev/null || true
            info "已移除 XFCE 快捷键绑定。"
            ;;
    esac
fi

# 3) 移除守护进程
rm -f "$HOME/.config/autostart/runbox-daemon.desktop"
pkill -f 'runbox --daemon' 2>/dev/null || true

# 4) 移除程序文件
$SUDO make uninstall

# 5) 用户配置（历史记录）
if [ -d "$HOME/.config/runbox" ]; then
    rm -rf "$HOME/.config/runbox"
    info "已清除历史记录。"
fi

info "RunBox 已完全卸载。"
