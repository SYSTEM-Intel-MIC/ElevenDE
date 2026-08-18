#!/usr/bin/env bash
# GNOME 会话: 把 Ctrl+Alt+Delete 绑定到 sas-screen --show
# 依赖: gsettings (gnome-settings-daemon 自定义快捷键)
set -euo pipefail

KEY='org.gnome.settings-daemon.plugins.media-keys'
ID='sas-screen'
SCHEMA="org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/${ID}/"
BIND="['/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/${ID}/']"

# 若已有自定义快捷键列表则追加，否则新建
EXISTING="$(gsettings get "$KEY" custom-keybindings 2>/dev/null || echo "@as []")"
if [[ "$EXISTING" == "@as []" ]]; then
    gsettings set "$KEY" custom-keybindings "$BIND"
else
    gsettings set "$KEY" custom-keybindings "$(echo "$EXISTING" | sed "s/]$/, '\/org\/gnome\/settings-daemon\/plugins\/media-keys\/custom-keybindings\/${ID}\/']/")"
fi

gsettings set "$SCHEMA" name 'SAS Screen'
gsettings set "$SCHEMA" command 'sas-screen --show'
gsettings set "$SCHEMA" binding '<Control><Alt>Delete'

echo "√ 已绑定 Ctrl+Alt+Delete → sas-screen --show"
echo "  移除: gsettings reset-recursively $SCHEMA"