#!/bin/sh
# volumeclip-linux.sh —— 用对拷线的 1MB 共享卷做剪贴板同步（Linux 侧）
#
# 背景：真机实测显示数据管道写（0xD9/0x2A）在两端都被 CHECK CONDITION 拒绝，
#       而设备暴露一个 1MB FAT 卷（2213 人格下是 "VirtualLink"，2208 人格下是 "Transfer line"）。
#       若该卷两端共享，剪贴板可用最朴素的文件读写同步 —— 不需要管理员、不需要私有 SCSI 命令。
#
# 用法：
#   sudo mkdir -p /mnt/otilink
#   sudo mount -t vfat /dev/sdX1 /mnt/otilink      # 找到对拷线那个 1MB 分区（lsblk 看 label）
#   sh volumeclip-linux.sh /mnt/otilink            # Ctrl+C 退出
#
# 约定（与 Windows 侧 volumeclip.ps1 对称）：
#   <卷>/otilink_clip_lin.txt  ← 本机（Linux）写
#   <卷>/otilink_clip_win.txt  ← 对端（Windows）写
set -u
VOL=${1:-/mnt/otilink}
INTERVAL=${2:-0.5}

[ -d "$VOL" ] || { echo "卷挂载点不存在: $VOL"; exit 1; }
OUT="$VOL/otilink_clip_lin.txt"
IN="$VOL/otilink_clip_win.txt"

# 选择剪贴板后端
if [ -n "${WAYLAND_DISPLAY:-}" ] && command -v wl-paste >/dev/null 2>&1; then
    CLIP_GET="wl-paste --no-newline"
    CLIP_SET="wl-copy"
elif [ -n "${DISPLAY:-}" ] && command -v xclip >/dev/null 2>&1; then
    CLIP_GET="xclip -selection clipboard -o"
    CLIP_SET="xclip -selection clipboard -i"
else
    echo "找不到剪贴板后端（需要 wl-clipboard 或 xclip + 图形会话）"
    exit 1
fi

echo "共享卷剪贴板（Linux 侧）：卷=$VOL 本机写=$OUT 对端读=$IN 间隔=${INTERVAL}s"
last_out=""
last_in=""

while :; do
    # 1) 本机剪贴板 → 写自己的文件
    cur=$($CLIP_GET 2>/dev/null || true)
    if [ -n "$cur" ]; then
        h=$(printf '%s' "$cur" | md5sum | cut -d' ' -f1)
        if [ "$h" != "$last_out" ]; then
            last_out="$h"
            printf '%s' "$cur" > "$OUT"
            echo "  → 已写本机剪贴板 $(printf '%s' "$cur" | wc -c) 字节"
        fi
    fi

    # 2) 对端文件 → 本机剪贴板
    if [ -f "$IN" ]; then
        peer=$(cat "$IN" 2>/dev/null || true)
        if [ -n "$peer" ]; then
            hp=$(printf '%s' "$peer" | md5sum | cut -d' ' -f1)
            if [ "$hp" != "$last_in" ]; then
                last_in="$hp"
                if [ "$peer" != "$cur" ]; then
                    printf '%s' "$peer" | $CLIP_SET
                    echo "  ← 已应用对端剪贴板 $(printf '%s' "$peer" | wc -c) 字节"
                fi
            fi
        fi
    fi

    sleep "$INTERVAL"
done
