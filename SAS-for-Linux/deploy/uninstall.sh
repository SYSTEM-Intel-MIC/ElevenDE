#!/usr/bin/env bash
# sas-screen 一键卸载：移除二进制、配置、KWin 脚本/规则、GNOME 快捷键、
#                     Sway/Hyprland/Niri 配置块（含历史版本遗留的自启动文件）；
#                     可选恢复 systemd 默认行为。
# 用法: ./deploy/uninstall.sh
#       若当初用 root 安装到 /usr/local，请加 sudo: sudo ./deploy/uninstall.sh
set -uo pipefail

cd "$(dirname "$0")/.."

GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RED=$'\033[31m'; NC=$'\033[0m'
say()  { printf "${GREEN}[sas]${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}[sas!]${NC} %s\n" "$*"; }
err()  { printf "${RED}[sas-x]${NC} %s\n" "$*"; }

BLOCK_START="# >>> SAS Screen (sas-screen) auto-config >>>"
BLOCK_END="# <<< SAS Screen (sas-screen) <<<"
NIRI_BLOCK_START="// >>> SAS Screen (sas-screen) auto-config >>>"
NIRI_BLOCK_END="// <<< SAS Screen (sas-screen) <<<"
USER_HOME="${HOME:-/root}"

say "1/8 停止正在运行的实例并清理 IPC 套接字..."
pkill -x sas-screen 2>/dev/null || true
rm -f /tmp/sas-screen-*.sock

say "2/8 删除可执行文件与数据文件（含历史版本遗留的自启动项）..."
rm -f "$USER_HOME/.local/bin/sas-screen" 2>/dev/null
rm -rf "$USER_HOME/.local/share/sas-screen" 2>/dev/null
rm -f "$USER_HOME/.config/autostart/sas-screen.desktop" 2>/dev/null
if [ -f /usr/local/bin/sas-screen ]; then
    if [ -w /usr/local/bin ]; then
        rm -f /usr/local/bin/sas-screen /usr/local/share/sas-screen/sas-screen.json
        rmdir /usr/local/share/sas-screen 2>/dev/null || true
        rm -f /etc/xdg/autostart/sas-screen.desktop
    else
        warn "检测到系统级安装(/usr/local)，请再执行一次: sudo ./deploy/uninstall.sh"
    fi
fi

say "3/8 用户配置..."
if [ -d "$USER_HOME/.config/sas-screen" ]; then
    if [ -f "$USER_HOME/.config/sas-screen/config.json" ]; then
        mv -v "$USER_HOME/.config/sas-screen/config.json" \
              "$USER_HOME/.config/sas-screen/config.json.bak" 2>/dev/null \
        && say "自定义 config.json 已备份为 config.json.bak"
    fi
    rm -rf "$USER_HOME/.config/sas-screen"
fi

say "4/8 清理 KWin 脚本与窗口规则(KDE)..."
rm -rf "$USER_HOME/.local/share/kwin/scripts/sas-screen"
if [ -f "$USER_HOME/.config/kwinrulesrc" ]; then
    awk '/^\[SASScreenBlur\]/{skip=1;next} /^\[/&&skip{skip=0} !skip{print}' \
        "$USER_HOME/.config/kwinrulesrc" > "$USER_HOME/.config/kwinrulesrc.tmp" \
    && mv "$USER_HOME/.config/kwinrulesrc.tmp" "$USER_HOME/.config/kwinrulesrc"
fi
if command -v kwriteconfig6 >/dev/null 2>&1; then
    kwriteconfig6 --file kwinrc --group Plugins --key sas-screenEnabled --delete 2>/dev/null || true
elif command -v kwriteconfig5 >/dev/null 2>&1; then
    kwriteconfig5 --file kwinrc --group Plugins --key sas-screenEnabled --delete 2>/dev/null || true
else
    sed -i '/^sas-screenEnabled=/d' "$USER_HOME/.config/kwinrc" 2>/dev/null || true
fi
qdbus org.kde.KWin /KWin reconfigure 2>/dev/null || true

say "5/8 移除 GNOME 自定义快捷键..."
if command -v gsettings >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    python3 - <<'PY' 2>/dev/null || true
import ast, subprocess
key = 'org.gnome.settings-daemon.plugins.media-keys'
ID = '/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/sas-screen/'
try:
    cur = subprocess.check_output(['gsettings', 'get', key, 'custom-keybindings'],
                                  stderr=subprocess.DEVNULL).decode().strip()
    entries = ast.literal_eval(cur) if cur.startswith('[') else []
except Exception:
    entries = []
entries = [e for e in entries if 'sas-screen' not in e]
subprocess.run(['gsettings', 'set', key, 'custom-keybindings', str(entries)],
               stderr=subprocess.DEVNULL)
subprocess.run(['gsettings', 'reset-recursively',
                'org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:' + ID],
               stderr=subprocess.DEVNULL)
PY
fi

say "6/8 移除平铺 WM 配置块(Sway/Hyprland/Niri)..."
remove_wm_block() {
    local f="$1" s="$2" e="$3"
    if [ -f "$f" ] && grep -q "^$s" "$f" 2>/dev/null; then
        sed -i "/^$(printf '%s' "$s" | sed 's/[.[\*^$/]/\\&/g')/,/^$(printf '%s' "$e" | sed 's/[.[\*^$/]/\\&/g')/d" "$f"
        say "  已从 $f 移除自动配置块"
    fi
}
remove_wm_block "$USER_HOME/.config/sway/config" "$BLOCK_START" "$BLOCK_END"
remove_wm_block "$USER_HOME/.config/hypr/hyprland.conf" "$BLOCK_START" "$BLOCK_END"
remove_wm_block "$USER_HOME/.config/niri/config.kdl" "$NIRI_BLOCK_START" "$NIRI_BLOCK_END"
# 兜底：移除用户手动添加、未带标记的裸 include 行
if [ -f "$USER_HOME/.config/niri/config.kdl" ] \
    && grep -q 'include.*"sas-screen\.kdl"' "$USER_HOME/.config/niri/config.kdl" 2>/dev/null; then
    sed -i '/include.*"sas-screen\.kdl"/d' "$USER_HOME/.config/niri/config.kdl"
    say "  已从 config.kdl 移除残留的 include sas-screen.kdl 行"
fi
rm -f "$USER_HOME/.config/niri/sas-screen.kdl" \
    && say "  已删除 Niri 独立配置 sas-screen.kdl"

say "7/8 恢复 systemd 默认行为..."
read -rp "是否恢复 Ctrl+Alt+Delete 的原默认行为（重启）？[y/N] " ans
case "$ans" in
    y|Y|yes|YES)
        if [ "$(id -u)" -eq 0 ]; then
            systemctl unmask ctrl-alt-del.target 2>/dev/null || true
        else
            sudo systemctl unmask ctrl-alt-del.target 2>/dev/null || \
                warn "未恢复(需要 sudo): 可稍后运行 sudo systemctl unmask ctrl-alt-del.target"
        fi
        say "已恢复: Ctrl+Alt+Delete 将重新触发重启"
        ;;
    *) say "已跳过: ctrl-alt-del.target 保持 mask 状态";;
esac

say "8/8 完成。"
echo
say "卸载完毕 ✓  残留检查: command -v sas-screen 应无输出"
