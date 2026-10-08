#!/bin/bash
# hidlat.sh —— 键鼠"过几秒卡一下"真机回归：与 otikm **同时**跑 hidlat，量 HID 写间隔。
#
# 为什么要它：HID 包与帧管道共用同一台单队列设备，任何大块/失败的设备操作
# （真机案例：保活 dummy 帧 64KB，单次 ~1.3 秒且 rc=526080）都会堵住输入路径。
# 这条回归把"卡顿"变成数字：maxgap 超过阈值就是 FAIL。
#
# 用法： ./hidlat.sh [秒数] [最大可接受间隔 ms]      （缺省 14 秒 / 200ms）
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"

SECS=${1:-14}
MAXGAP=${2:-200}

stage "1/3 前提"
khave 'pgrep -x otikm >/dev/null' && ok "麒麟 otikm 在跑（必须与它并发才算真实竞争）" \
    || bad "麒麟 otikm 没跑：先 sg input -c \"DISPLAY=:0 nohup ~/otilink/run-kylin.sh > /tmp/rk.log 2>&1 &\""
# 线缆设备自动发现：扫 sysfs 找 idVendor=0ea0 的 SCSI generic 节点
# （/proc/<pid>/fd 读不到符号链接目标 —— dumpable 受限；而且拔插后 sg 号会变）
DEV=$(kssh 'for s in /sys/class/scsi_generic/sg*; do d=$(readlink -f "$s/device" 2>/dev/null); while [ -n "$d" ] && [ "$d" != "/" ]; do [ -f "$d/idVendor" ] && break; d=$(dirname "$d"); done; if [ -f "$d/idVendor" ] && grep -qi 0ea0 "$d/idVendor" 2>/dev/null; then echo "/dev/$(basename "$s")"; break; fi; done')
[ -n "${DEV:-}" ] && ok "线缆设备自动发现：$DEV（0ea0）" || bad "找不到线缆 SCSI 节点（0ea0）—— 插好了吗？"

stage "2/3 在麒麟上编译 hidlat 并运行"
if kssh 'cd ~/otilink && gcc -O2 -I. -o /tmp/hidlat hidlat.c otilink.c otiproto.c otitrans.c otihid.c -lpthread' ; then
    ok "hidlat 编译通过（用麒麟侧的当前源码）"
else
    bad "hidlat 编译失败（~/otilink 里的源码/头文件同步了吗？）"
fi
OUT=$(kssh "/tmp/hidlat $DEV $SECS $MAXGAP")
echo "$OUT" | tail -12 | sed 's/^/      /'

stage "3/3 判定"
G=$(echo "$OUT" | sed -n 's/^MAXGAP=\([0-9]*\).*/\1/p' | tail -1)
NF=$(echo "$OUT" | sed -n 's/.*SEND_FAIL=\([0-9]*\).*/\1/p' | tail -1)
if [ -z "${G:-}" ]; then
    bad "拿不到 MAXGAP（hidlat 没跑起来？）"
else
    [ "$G" -le "$MAXGAP" ] && ok "HID 写最大间隔 ${G}ms ≤ ${MAXGAP}ms（无卡顿）" \
        || bad "HID 写最大间隔 ${G}ms > ${MAXGAP}ms：有设备级操作堵住输入路径（看上面 GAP 行）"
    [ "${NF:-0}" -eq 0 ] && ok "HID 写没有失败（SEND_FAIL=0）" \
        || bad "HID 写失败 ${NF} 次（设备/会话有问题）"
fi
summary "键鼠卡顿回归（hidlat）"
