#!/usr/bin/env bash
# KDE Plasma: 安装 KWin 脚本(快捷键) + 窗口模糊规则(毛玻璃)
# 安装后到 系统设置 → 窗口管理 → KWin 脚本 勾选 "SAS Screen" 并应用
set -euo pipefail

SCRIPT_DIR="$HOME/.local/share/kwin/scripts/sas-screen"
mkdir -p "$SCRIPT_DIR/contents/code"
cp -v "$(dirname "$0")/kwin-sas-screen/metadata.desktop" "$SCRIPT_DIR/"
cp -v "$(dirname "$0")/kwin-sas-screen/contents/code/main.js" "$SCRIPT_DIR/contents/code/"

# KWin 窗口规则: 为 sas-screen 窗口启用 KWin 毛玻璃(Blur 特效)，X11/Wayland 通用
RULE="$HOME/.config/kwinrulesrc"
if ! grep -q "SASScreenBlur" "$RULE" 2>/dev/null; then
    cat >> "$RULE" <<'EOF'

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
    echo "√ 已写入窗口模糊规则: $RULE"
else
    echo "· 模糊规则已存在，跳过"
fi

# 立即生效（重新加载 KWin 配置；失败则忽略）
qdbus org.kde.KWin /KWin reconfigure 2>/dev/null || true

echo ""
echo "完成！下一步："
echo "  1) 系统设置 → 窗口管理 → KWin 脚本 → 勾选 SAS Screen → 应用"
echo "     (或临时启用: qdbus org.kde.KWin /Scripting loadScript $SCRIPT_DIR/contents/code/main.js)"
echo "  2) 若 KDE 自带快捷键占用了 Ctrl+Alt+Delete(注销/重启)，"
echo "     请到 系统设置 → 快捷键 中删除该绑定"
echo "  3) 确认已运行: sudo systemctl mask ctrl-alt-del.target"