#!/bin/bash
# sosreg.sh —— 麒麟 SOS 救援回归：左 Meta+H 不得把整机拖死
#
# 为什么需要它（2026-11-05，用户两次报障「麒麟蓝屏 / 键盘输入不了」）：
#   麒麟内核 kysos_key（builtin，不可卸载）把所有键盘上的 **左 Meta + H** 当 SOS
#   组合键：按下后它等 20s，让用户态往 /sys/kos_rescue/rescue_feedback 写 1 确认；
#   没人确认就"救援"——把活动 VT 切到 tty5、拉起 getty@tty5 → console_lock 死锁
#   → Xorg 把全部输入设备摘掉 → 桌面键盘鼠标全死（用户看到的是"蓝屏/黑屏"）。
#   这台机器没有麒麟的用户态 SOS 处理器；而 **Windows 上 Win+H 是"语音输入"**，
#   共享键盘时误触就会炸。otilink/sosguard.sh（systemd: otilink-sosguard）负责
#   立即确认，取消救援。
#
# 本回归断言（注入一次组合键，守卫必须把它取消掉）：
#   1) /sys/kos_rescue 存在（内核有 kysos_key）
#   2) sosguard 在跑 —— **不在跑就中止，绝不注入**（否则会真触发救援死锁）
#   3) 内核确实收到组合键：SOS triggered 增加
#   4) 守卫已确认：Feedback received 增加，Timeout count 不增加，状态回 IDLE
#   5) 活动 VT 没被切走（尤其不能变 tty5）
#   6) X 键盘仍挂在 master 上（没被摘掉）
#
# 用法：./sosreg.sh    约 15 秒；注入的按键来自 uinput 虚拟设备，不动用户真键鼠。
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"

stat() { kssh 'cat /sys/kos_rescue/stats' 2>/dev/null; }
field() { printf '%s\n' "$1" | sed -n "s/^$2: //p" | head -1; }

echo "=== 1/4 前提（不满足就中止，绝不注入）==="
khave 'test -d /sys/kos_rescue' \
    && ok "内核有 kysos_key（/sys/kos_rescue 存在）" \
    || bad "没有 /sys/kos_rescue：不是麒麟定制内核，本回归不适用"

GUARD=$(kssh 'systemctl is-active otilink-sosguard 2>/dev/null' | tr -d ' ')
if [ "$GUARD" = "active" ]; then
    ok "sosguard 在跑（systemd active）"
else
    bad "sosguard 没在跑：守卫不在时注入会真触发救援死锁 —— 已中止。先 sudo sh install-kylin.sh"
fi

if kssh 'cd ~/otilink && make sosinject >/dev/null 2>&1 && test -x ./sosinject' >/dev/null 2>&1; then
    ok "sosinject 已编译（与源码同步）"
else
    bad "sosinject 编译失败（需要 gcc；看 ~/otilink/sosinject.c）"
fi

if [ "$FAIL" -ne 0 ]; then
    summary "SOS 救援回归（sosreg）"
    exit "$FAIL"
fi

echo "=== 2/4 注入前基线 ==="
S0=$(stat); SOS0=$(field "$S0" 'SOS triggered'); FB0=$(field "$S0" 'Feedback received')
TO0=$(field "$S0" 'Timeout count')
VT0=$(kssh 'cat /sys/class/tty/tty0/active' | tr -d ' ')
X0=$(kssh 'DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority xinput list 2>/dev/null | grep -c "SIGMACHIP USB Keyboard.*\[slave  keyboard"' | tr -d ' ')
info "triggered=$SOS0 feedback=$FB0 timeout=$TO0 VT=$VT0 X键盘=$X0"

echo "=== 3/4 注入左 Meta+H，等守卫确认 ==="
kssh 'cd ~/otilink && ./sosinject --wait 1' >/dev/null 2>&1
sleep 2
S1=$(stat); SOS1=$(field "$S1" 'SOS triggered'); FB1=$(field "$S1" 'Feedback received')
TO1=$(field "$S1" 'Timeout count'); ST1=$(field "$S1" 'Current state')
info "triggered=$SOS1 feedback=$FB1 timeout=$TO1 state=$ST1"

[ "${SOS1:-0}" -gt "${SOS0:-0}" ] 2>/dev/null \
    && ok "内核确实收到 SOS 组合键（triggered $SOS0→$SOS1）" \
    || bad "内核没收到组合键（triggered $SOS0→$SOS1）：sos-handler 没连上虚拟键盘？"
[ "${FB1:-0}" -gt "${FB0:-0}" ] 2>/dev/null \
    && ok "守卫已确认，救援被取消（feedback $FB0→$FB1）" \
    || bad "守卫没确认（feedback $FB0→$FB1）：20s 后会切 VT 并死锁"
[ "${TO1:-1}" -eq "${TO0:-0}" ] 2>/dev/null \
    && ok "没有救援超时（timeout 仍为 $TO0）" \
    || bad "出现救援超时（$TO0→$TO1）"
[ "$ST1" = "IDLE" ] \
    && ok "状态已回 IDLE" \
    || bad "状态不是 IDLE（$ST1）"

echo "=== 4/4 副作用检查（VT 与 X 输入设备）==="
VT1=$(kssh 'cat /sys/class/tty/tty0/active' | tr -d ' ')
X1=$(kssh 'DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority xinput list 2>/dev/null | grep -c "SIGMACHIP USB Keyboard.*\[slave  keyboard"' | tr -d ' ')
[ "$VT1" = "$VT0" ] \
    && ok "活动 VT 未变（$VT0）" \
    || bad "VT 被切走（$VT0 → $VT1）—— 守卫没起作用"
[ "$X1" = "$X0" ] && [ "${X1:-0}" -ge 1 ] \
    && ok "X 键盘仍挂在 master 上（$X1 个）" \
    || bad "X 键盘被摘掉（$X0 → $X1）—— 桌面会收不到按键"

summary "SOS 救援回归（sosreg）"
