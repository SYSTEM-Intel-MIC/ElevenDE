#!/usr/bin/env bash
# 屏蔽 systemd 默认响应（重启）：需要 root 权限执行
#   sudo ./deny-systemd-cad.sh
# 原理：内核把 Ctrl+Alt+Delete 转成 SIGINT 发给 PID 1，
#       systemd 默认启动 ctrl-alt-del.target（= reboot）。
#       mask 之后按此组合键对 systemd 不再产生任何作用。
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行：sudo $0"
    exit 1
fi

systemctl mask ctrl-alt-del.target
systemctl daemon-reload

echo "√ 已屏蔽 ctrl-alt-del.target —— Ctrl+Alt+Delete 不再触发重启。"
echo "  恢复方式：sudo systemctl unmask ctrl-alt-del.target"