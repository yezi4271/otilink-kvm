#!/bin/sh
# sgtest.sh —— 用 scsi_debug 造一个真实内核 SCSI 设备，验证 otilink 的 SG_IO 层
# 需要 root：sudo sh sgtest.sh
# 说明：scsi_debug 是内存盘，加载/卸载即来即走，不留持久改动。
set -u
cd "$(dirname "$0")"
if [ "$(id -u)" != 0 ]; then
    echo "需要 root（SG_IO 要读写 /dev/sgN）：sudo sh sgtest.sh"
    exit 1
fi
if ! modprobe scsi_debug dev_size_mb=32 2>/dev/null; then
    echo "modprobe scsi_debug 失败（内核无该模块？）"
    exit 1
fi
cleanup() { rmmod scsi_debug 2>/dev/null; }
trap cleanup EXIT INT TERM
sleep 1

SG=""
for g in /sys/class/scsi_generic/sg*; do
    [ -e "$g" ] || continue
    m=$(cat "$g/device/model" 2>/dev/null || true)
    case "$m" in
        *scsi_debug*) SG="/dev/$(basename "$g")" ;;
    esac
done
if [ -z "$SG" ]; then
    echo "没找到 scsi_debug 的 sg 节点"
    exit 1
fi
echo "scsi_debug 设备: $SG"
./probe sgtest --dev "$SG"
