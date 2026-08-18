#!/usr/bin/env bash
# sas-screen 一键安装：编译 + 安装 + 屏蔽 systemd 重启
#                    + 自动检测当前 DE/WM 并完成全部绑定配置。
# 说明: 不写任何自启动项 —— 快捷键统一绑定 "sas-screen --show"，
#       按需冷启动并显示界面，登录后不会自动打开。
# 用法:
#   ./deploy/install.sh               普通用户安装到 ~/.local（推荐）
#   sudo ./deploy/install.sh          系统级安装到 /usr/local（自动把用户级配置交给原用户）
#   ./deploy/install.sh --user-only   仅执行用户级配置（供脚本内部重入，勿手动使用）
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT_DIR="$(pwd)"

GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RED=$'\033[31m'; NC=$'\033[0m'
say()  { printf "${GREEN}[sas]${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}[sas!]${NC} %s\n" "$*"; }

BIN_NAME="sas-screen"
BLOCK_START="# >>> SAS Screen (sas-screen) auto-config >>>"
BLOCK_END="# <<< SAS Screen (sas-screen) <<<"

USER_HOME="${HOME:-/root}"
USER_BIN="$USER_HOME/.local/bin"
USER_SHARE="$USER_HOME/.local/share/sas-screen"
SYS_SHARE="/usr/local/share/sas-screen"

# ---------- 配置块工具：按标记写入/移除，保证可重复安装 ----------
# Sway/Hyprland 用 # 注释；Niri 是 KDL 格式，必须用 // 注释
NIRI_BLOCK_START="// >>> SAS Screen (sas-screen) auto-config >>>"
NIRI_BLOCK_END="// <<< SAS Screen (sas-screen) <<<"

remove_block() {   # remove_block <file> [start] [end]
    local f="$1" s="${2:-$BLOCK_START}" e="${3:-$BLOCK_END}"
    [ -f "$f" ] || return 0
    sed -i "/^$(printf '%s' "$s" | sed 's/[.[\*^$/]/\\&/g')/,/^$(printf '%s' "$e" | sed 's/[.[\*^$/]/\\&/g')/d" "$f"
}
append_block() {   # append_block <file> [--markers <start> <end>] 内容行...
    local f="$1"; shift
    local s="$BLOCK_START" e="$BLOCK_END"
    if [ "${1:-}" = "--markers" ]; then
        s="${2:-$BLOCK_START}"; e="${3:-$BLOCK_END}"
        shift 3
    fi
    mkdir -p "$(dirname "$f")"
    remove_block "$f" "$s" "$e"
    printf '\n%s\n' "$s" >> "$f"
    printf '%s\n' "$@" >> "$f"
    printf '%s\n' "$e" >> "$f"
}

# ---------- 1. 编译并安装（系统级/用户级）----------
system_install() {
    command -v cmake >/dev/null 2>&1 || { err "未找到 cmake，请先安装: sudo pacman -S cmake / sudo apt install cmake"; exit 1; }
    say "编译..."
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$ROOT_DIR/build" -j"$(nproc)" >/dev/null || { err "编译失败"; exit 1; }

    if [ "$(id -u)" -eq 0 ]; then
        cmake --install "$ROOT_DIR/build" >/dev/null || exit 1
        mkdir -p "$SYS_SHARE"
        cp "$ROOT_DIR/config/sas-screen.json" "$SYS_SHARE/"
        say "已安装到 /usr/local（不写入自启动项，按快捷键按需启动）"
    else
        cmake --install "$ROOT_DIR/build" --prefix "$USER_HOME/.local" >/dev/null || exit 1
        mkdir -p "$USER_SHARE"
        cp "$ROOT_DIR/config/sas-screen.json" "$USER_SHARE/"
        say "已安装到 ~/.local（不写入自启动项，按快捷键按需启动）"
    fi
    mask_cad
}

mask_cad() {
    say "屏蔽 systemd 的 Ctrl+Alt+Delete(重启)默认行为..."
    if [ "$(id -u)" -eq 0 ]; then
        systemctl mask ctrl-alt-del.target 2>/dev/null || true
        systemctl daemon-reload 2>/dev/null || true
        say "  ctrl-alt-del.target 已 mask"
    else
        sudo systemctl mask ctrl-alt-del.target 2>/dev/null || \
            warn " 未屏蔽(需要 sudo): 可稍后运行 sudo ./deploy/deny-systemd-cad.sh"
    fi
}

# ---------- 2. DE/WM 检测 ----------
detect_env() {
    SESSION="unknown"
    if [ -n "${XDG_SESSION_TYPE:-}" ]; then
        SESSION="$XDG_SESSION_TYPE"
    elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
        SESSION="wayland"
    elif [ -n "${DISPLAY:-}" ]; then
        SESSION="x11"
    fi

    DE="unknown"
    dl=$(printf '%s' "${XDG_CURRENT_DESKTOP:-}" | tr '[:upper:]' '[:lower:]')
    case "$dl" in
        *gnome*)     DE=gnome ;;
        *plasma*|*kde*) DE=kde ;;
        *hyprland*)  DE=hyprland ;;
        *sway*)      DE=sway ;;
        *niri*)      DE=niri ;;
        *xfce*)      DE=xfce ;;
        *cinnamon*)  DE=cinnamon ;;
    esac

    if [ "$DE" = unknown ]; then
        for p in gnome-shell plasmashell Hyprland sway niri; do
            if pgrep -x "$p" >/dev/null 2>&1; then
                case "$p" in
                    gnome-shell) DE=gnome ;; plasmashell) DE=kde ;;
                    Hyprland)    DE=hyprland ;;
                    sway)        DE=sway ;;
                    niri)        DE=niri ;;
                esac
                break
            fi
        done
    fi
    if [ "$DE" = unknown ]; then
        [ -f "$USER_HOME/.config/hypr/hyprland.conf" ] && DE=hyprland
        [ -f "$USER_HOME/.config/sway/config" ] && DE=sway
        [ -f "$USER_HOME/.config/niri/config.kdl" ] && DE=niri
    fi
}

# ---------- 3. 各 DE/WM 自动配置 ----------
config_gnome() {
    say "GNOME 检测到: 配置 Ctrl+Alt+Delete 自定义快捷键..."
    command -v gsettings >/dev/null 2>&1 || { warn "未找到 gsettings，请手动配置"; return; }
    python3 - <<'PY' || warn "gsettings 配置失败（需在图形会话内运行 install.sh）"
import ast, subprocess
key = 'org.gnome.settings-daemon.plugins.media-keys'
ID = '/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/sas-screen/'
schema = 'org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:' + ID
def g(cmd, *args):
    return subprocess.run(['gsettings'] + list(cmd) + list(args),
                          capture_output=True, text=True)
cur = g(['get', key, 'custom-keybindings']).stdout.strip()
entries = ast.literal_eval(cur) if cur.startswith('[') else []
if ID not in entries:
    entries.append(ID)
    g(['set', key, 'custom-keybindings'], str(entries))
g(['set', schema, 'name'], "'SAS Screen'")
g(['set', schema, 'command'], "'sas-screen --show'")
g(['set', schema, 'binding'], "'<Control><Alt>Delete'")
PY
    say "  Ctrl+Alt+Delete → sas-screen --show 已绑定"
}

config_kde() {
    say "KDE 检测到: 安装 KWin 脚本(快捷键) + 毛玻璃窗口规则..."
    local script="$USER_HOME/.local/share/kwin/scripts/sas-screen"
    mkdir -p "$script/contents/code"
    cp "$ROOT_DIR/deploy/kde/kwin-sas-screen/metadata.desktop" "$script/"
    cp "$ROOT_DIR/deploy/kde/kwin-sas-screen/contents/code/main.js" "$script/contents/code/"

    local rule="$USER_HOME/.config/kwinrulesrc"
    mkdir -p "$(dirname "$rule")"
    if [ -f "$rule" ]; then
        awk '/^\[SASScreenBlur\]/{skip=1;next} /^\[/&&skip{skip=0} !skip{print}' \
            "$rule" > "$rule.tmp" && mv "$rule.tmp" "$rule"
    fi
    cat >> "$rule" <<'EOF'

[SASScreenBlur]
Description=SAS Screen blur (毛玻璃)
blur=true
blurrule=2
wmclass=sas-screen
wmclassmatch=1
keepabove=true
keepaboveaffected=1
skipswitcher=true
skipswitcheraffected=1
EOF

    if command -v kwriteconfig6 >/dev/null 2>&1; then
        kwriteconfig6 --file kwinrc --group Plugins --key sas-screenEnabled true
    elif command -v kwriteconfig5 >/dev/null 2>&1; then
        kwriteconfig5 --file kwinrc --group Plugins --key sas-screenEnabled true
    else
        touch "$USER_HOME/.config/kwinrc"
        printf '\n[Plugins]\nsas-screenEnabled=true\n' >> "$USER_HOME/.config/kwinrc"
    fi
    qdbus org.kde.KWin /KWin reconfigure 2>/dev/null || \
        warn "  KWin 未重载: 请到 系统设置→窗口管理→KWin 脚本 勾选 SAS Screen 并应用"
    say "  KWin 脚本 + Blur 规则已就绪"
}

config_hyprland() {
    say "Hyprland 检测到: 写入 hyprland.conf..."
    append_block "$USER_HOME/.config/hypr/hyprland.conf" \
        'bind = CONTROL ALT, Delete, exec, sas-screen --show' \
        'windowrule = keepabove, class:^(sas-screen)$' \
        'windowrule = blur, class:^(sas-screen)$' \
        'windowrule = opacity 0.92 0.92, class:^(sas-screen)$'
    hyprctl reload >/dev/null 2>&1 && say "  已生效 (hyprctl reload)" \
        || warn "  请手动执行 hyprctl reload 生效"
}

config_sway() {
    say "Sway 检测到: 写入 sway 配置..."
    append_block "$USER_HOME/.config/sway/config" \
        'bindsym Control+Alt+Delete exec sas-screen --show'
    swaymsg reload >/dev/null 2>&1 && say "  已生效 (swaymsg reload)" \
        || warn "  请手动按 Mod+Shift+C 重载配置"
}

config_niri() {
    local niri_dir="$USER_HOME/.config/niri"
    local niri_extra="$niri_dir/sas-screen.kdl"
    say "Niri 检测到: 写入独立配置 sas-screen.kdl 并在 config.kdl 中 include..."
    append_block "$niri_extra" --markers "$NIRI_BLOCK_START" "$NIRI_BLOCK_END" \
        'binds {' \
        '    Ctrl+Alt+Delete { spawn "sas-screen" "--show"; }' \
        '}'
    append_block "$niri_dir/config.kdl" --markers "$NIRI_BLOCK_START" "$NIRI_BLOCK_END" \
        'include "sas-screen.kdl"'
    niri msg action reload-config >/dev/null 2>&1 && say "  已生效" \
        || warn "  请手动按 Mod+Shift+R 重载配置"
}

# ---------- 4. 用户级配置：检测 + 绑定 ----------
user_install() {
    detect_env
    say "检测结果: 会话=$SESSION  桌面=$DE"

    case "$DE-$SESSION" in
        hyprland-*)   config_hyprland ;;
        sway-*)       config_sway ;;
        niri-*)       config_niri ;;
        gnome-*)      config_gnome ;;
        kde-*)        config_kde ;;
        *-x11)        say "X11 会话: 运行中的实例自动注册全局 Ctrl+Alt+Delete；未识别 DE 时不写自启，请把 sas-screen 加入你的登录启动器" ;;
        xfce-*|cinnamon-*)
            warn "XFCE/Cinnamon 未内置自动绑定，请手动添加全局快捷键执行 sas-screen --show" ;;
        *)
            warn "未识别 DE/WM（会话=$SESSION）。"
            warn "  X11 会话可正常使用（自动热键）；Wayland 请参考 README 或 deploy/wayland/*.config.hint 手动绑定" ;;
    esac

    echo
    say "安装完成 ✓"
    echo "  程序:      $(command -v $BIN_NAME || echo "$USER_BIN/$BIN_NAME")"
    echo "  触发键:    Ctrl+Alt+Delete（Esc/取消 关闭界面）"
    echo "  按钮命令:  ~/.config/sas-screen/config.json（可选，默认已内置常用命令）"
    echo "  卸载:      ./deploy/uninstall.sh"
}

main() {
    case "${1:-}" in
        --user-only)
            user_install ;;
        *)
            system_install
            if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
                say "系统级安装完成，将用户级配置交给 ${SUDO_USER} 执行..."
                sudo -H -u "$SUDO_USER" "$0" --user-only
            else
                user_install
            fi ;;
    esac
}

main "$@"
