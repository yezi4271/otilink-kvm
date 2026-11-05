#!/bin/sh
# sosguard.sh —— 取消麒麟内置 SOS 救援，避免它把整机拖死
#
# 背景（2026-11-05，见 NOTES §74）：
#   麒麟内核 kysos_key（builtin，不可卸载）把所有键盘上的 **左 Meta + H**
#   当作 SOS 组合键。按下后它等 20s，让用户态往
#   /sys/kos_rescue/rescue_feedback 写 1 确认；没人确认就"救援"：
#   把活动 VT 切到 tty5、拉起 getty@tty5 —— 结果 console_lock 死锁，
#   Xorg 把所有输入设备摘掉，桌面键盘/鼠标全死（用户看到的是"蓝屏/黑屏"）。
#   这台机器没有麒麟的用户态 SOS 处理器，所以每次误触都会炸；
#   而 **Windows 上 Win+H 是"语音输入"**，共享键盘时误触概率很高。
#
# 本脚本就是那个"用户态确认"：SOS 状态一离开 IDLE 就写 1，
# 救援被取消，不切 VT、不死锁。代价：SOS 救援在本机不再生效
# （它在本机本来也只会死锁，见 NOTES §71/§74）。
#
# 用法：sudo ./sosguard.sh          （systemd 见 otilink-sosguard.service）
# 环境：SOS_SYS / SOS_LOG / SOS_INTERVAL 可覆盖默认值
set -u

SYS=${SOS_SYS:-/sys/kos_rescue}
LOG=${SOS_LOG:-/var/log/otilink-sosguard.log}
INTERVAL=${SOS_INTERVAL:-0.2}

if [ ! -d "$SYS" ]; then
    echo "sosguard: $SYS 不存在（内核没有 kysos_key），无需运行"
    exit 0
fi

log() { printf '%s %s\n' "$(date '+%F %T')" "$*" >> "$LOG" 2>/dev/null; }

log "start: delay=$(cat "$SYS/rescue_delay" 2>/dev/null) state=$(sed -n 's/^Current state: //p' "$SYS/stats" 2>/dev/null)"
while :; do
    st=$(cat "$SYS/stats" 2>/dev/null) || { sleep 1; continue; }
    state=$(printf '%s\n' "$st" | sed -n 's/^Current state: //p')
    if [ -n "$state" ] && [ "$state" != "IDLE" ]; then
        if printf '1' > "$SYS/rescue_feedback" 2>/dev/null; then
            log "SOS 组合键（左 Meta+H）已被取消，state=$state"
        fi
    fi
    sleep "$INTERVAL"
done
