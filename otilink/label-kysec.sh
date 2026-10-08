#!/bin/sh
# label-kysec.sh —— 给本目录的二进制打 KySec 信任标签。
#
# 为什么必须做：麒麟的 KySec 把"用户自己编译/拷贝的二进制"视为不可信，
# 禁止它们读 ~/文档 这类受保护目录（实测 fopen 返回 errno=13 EACCES，
# 而同一路径用系统命令 head 能读、在 sg input 上下文里也能读）。
# 不信任的直接后果：复制图片时剪贴板里只有 file:// 路径，我们要读那个文件发图片，
# 却读不到 → 对端只收到一串路径。
#
# 注意：**标签是打在文件上的**，重新编译/重新拷贝后必须再打一次。
# 部署脚本里请调用本脚本（或在 install-kylin.sh 里调用）。
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"
n=0
for f in "$DIR"/*; do
    [ -f "$f" ] && [ -x "$f" ] || continue
    case "$f" in *.sh|*.c|*.h|*.md|*.xml) continue ;; esac
    if $SUDO kysec_set -n exectl -v trusted "$f" >/dev/null 2>&1; then
        n=$((n+1))
    fi
done
echo "已给 $n 个二进制打上 KySec 信任标签"
