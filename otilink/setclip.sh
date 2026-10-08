#!/bin/sh
# setclip.sh <file> [mime] —— 把文件内容放进 X11 剪贴板后立刻返回（不阻塞调用方/ssh）。
# 两个坑：
#  1) xclip 会 fork 子进程持有选区；子进程若还挂在 ssh 通道上，ssh 会一直等到它退出
#     （实测 60s 超时）→ 三个 fd 全部重定向到 /dev/null 并 setsid 脱离。
#  2) 用户脚本（kysec 视为不可信）**不能**用 shell 重定向读 ~/文档（EACCES）→ 一律走
#     `cat`（系统可信二进制）喂给 xclip。
F="$1"; M="${2:-}"; D="${DISPLAY:-:0}"
if [ -n "$M" ]; then
    setsid sh -c "cat '$F' | DISPLAY=$D xclip -selection clipboard -t '$M' -i" >/dev/null 2>&1 < /dev/null &
else
    setsid sh -c "cat '$F' | DISPLAY=$D xclip -selection clipboard -i" >/dev/null 2>&1 < /dev/null &
fi
sleep 0.6
exit 0
