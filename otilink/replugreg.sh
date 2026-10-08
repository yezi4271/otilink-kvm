#!/bin/bash
# replugreg.sh —— 线缆**拔插自愈**回归（不需要人手拔线）
#
# 为什么要有它（第 43 轮真实故障）：
#   用户按 §8 急救"两端重拔插"后，otikm **段错误退出** → 没人再发 F24 回程令牌 →
#   Windows 侧永久卡在 REMOTE、鼠标被吞。根因是 keepalive_thread 在循环外把
#   &a->tx->dev 缓存成裸指针，而 reopen_transport() 会 free 掉上一代 transport
#   （约 4MB 且是 mmap 出来的，free 即 munmap）→ heap-use-after-free。
#   ASan 实证与修复见 re/NOTES.md §53。
#   同一轮还发现 run-kylin.sh 把 /dev/sgN 写死（拔插后启动即失败）。
#
# 做法：用 sysfs 的 USB unbind/bind **模拟拔插**，跑 N 轮。每轮要求：
#   拔线后 otikm 存活 → 插回后存活 → 传输重新就绪。跑完再查无 segfault。
#   修复前的二进制会在**第 2 轮**崩（第 1 次成功重开只置 tx_old，第 2 次才 free）。
#
# 依赖：能 SSH 到麒麟（带 askpass）；麒麟侧 sudo（写 sysfs）；otikm 由 run-kylin.sh 拉起。
# 用法：  ./replugreg.sh         跑 3 轮（缺省）
#         ./replugreg.sh 5       跑 5 轮
# 退出码 = 失败项数（0 = 全过）。
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
KYLIN=${KYLIN:-${KY:-}}
KY_PASS=${KY_PASS:-}
ROUNDS=${1:-3}
LOG=${OTIKM_LOG:-/tmp/otikm.log}
RUN_KYLIN=${RUN_KYLIN:-~/otilink/run-kylin.sh}

PASS=0; FAIL=0; WARN=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }
warn() { echo "  [WARN] $*"; WARN=$((WARN+1)); }

ASKPASS=$(mktemp); chmod 700 "$ASKPASS"
printf '#!/bin/sh\necho %s\n' "$KY_PASS" > "$ASKPASS"
trap 'rm -f "$ASKPASS"' EXIT
# 注意：不合并 stderr（麒麟登录横幅会污染捕获值，见 AGENTS.md L10）
kssh()  { SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force setsid -w timeout 40 \
          ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$KYLIN" "$@" </dev/null 2>/dev/null; }
ksudo() { kssh "echo $KY_PASS | sudo -S -p '' bash -c \"$1\""; }
start_otikm() { kssh "sg input -c \"DISPLAY=:0 nohup $RUN_KYLIN > /tmp/rk.log 2>&1 &\"" >/dev/null 2>&1; sleep 7; }

echo "=== 0) 前提 ==="
if ! kssh true >/dev/null 2>&1; then echo "  [FAIL] SSH 不可达 $KYLIN"; echo; echo "  PASS=0  FAIL=1  WARN=0"; exit 1; fi
ok "SSH 可达 $KYLIN"
T0=$(kssh 'date "+%Y-%m-%d %H:%M:%S"' | tr -d ' ')   # 只查本次回归期间的崩溃，别把历史 segfault 算进来

USBDEV=$(kssh 'for d in /sys/bus/usb/devices/*/; do [ -f "$d/idVendor" ] && [ "$(cat "$d/idVendor" 2>/dev/null)" = 0ea0 ] && basename "$d"; done | head -1' | tr -d ' ')
case "$USBDEV" in
    ?*) ok "线缆已枚举（USB 拓扑 $USBDEV）" ;;
    *)  bad "找不到 0ea0:2213 —— 线缆没插好？后续跳过" ;;
esac

if kssh 'pgrep -x otikm >/dev/null'; then
    ok "otikm 运行中"
else
    warn "otikm 没在跑，先拉起"
    start_otikm
    kssh 'pgrep -x otikm >/dev/null' && ok "已拉起 otikm" || bad "拉不起来（看 /tmp/rk.log）"
fi

echo "=== 1) 启动脚本不得写死线缆设备号 ==="
if grep -q 'cable:/dev/sg' "$HERE/run-kylin.sh"; then
    bad "run-kylin.sh 仍写死 cable:/dev/sgN —— 拔插/换口后启动必失败"
else
    ok "run-kylin.sh 用裸 cable（自发现）"
fi

if [ -n "$USBDEV" ]; then
# 第 46 轮：角色是**协商**出来的，抓取设备集合随之不同 —— 断言必须按角色分开：
#   * 被驱动侧(slave)：抓的是**线缆 HID** → 拔插线缆必须触发"输入抓取已重开"（§54 的老断言）；
#   * 主控端(master)：抓的是**本机键鼠** → 拔插线缆**不影响**本机设备（不该要求重开），
#     要断言的是"抓取设备仍在 + 没有重开失败刷屏"。
KROLE=$(kssh 'grep -a -m1 -o "角色引导(auto): [^（]*" '"$LOG"' 2>/dev/null' | tail -1)
case "$KROLE" in
    *主控*) ROLE=master ;;
    *被驱动*) ROLE=slave ;;
    *) ROLE=unknown ;;
esac
echo "=== 2) 模拟拔插 $ROUNDS 轮（USB unbind/bind）  当前角色=$ROLE ==="
for i in $(seq 1 "$ROUNDS"); do
    N0=$(kssh "wc -l < $LOG 2>/dev/null" | tr -d ' ')
    case "$N0" in ''|*[!0-9]*) N0=0 ;; esac

    ksudo "echo $USBDEV > /sys/bus/usb/drivers/usb/unbind"
    sleep 7
    if kssh 'pgrep -x otikm >/dev/null'; then ok "第 $i 轮：拔线后存活"; else bad "第 $i 轮：拔线后 otikm 已死（查 journalctl | grep segfault）"; fi

    ksudo "echo $USBDEV > /sys/bus/usb/drivers/usb/bind"
    ready=0
    for _ in $(seq 1 25); do
        sleep 1
        if kssh "tail -n +$((N0+1)) $LOG 2>/dev/null | grep -q '传输已重新就绪'"; then ready=1; break; fi
    done
    [ "$ready" -eq 1 ] && ok "第 $i 轮：插回后传输重新就绪（auto 自发现）" || bad "第 $i 轮：插回后 25s 内未重新就绪"

    # 输入抓取自愈：判据按角色（见上面说明）。
    if [ "$ROLE" = slave ]; then
        if kssh "tail -n +$((N0+1)) $LOG 2>/dev/null | grep -q '输入抓取已重开'"; then
            ok "第 $i 轮：输入抓取已自愈重开（被驱动侧抓线缆 HID）"
        else
            bad "第 $i 轮：输入抓取没有重开（撞边/热键会失效，见 NOTES §54）"
        fi
    else
        # 主控端：抓的是本机设备，线缆拔插不该影响它；要求"没出现重开失败刷屏"
        if kssh "tail -n +$((N0+1)) $LOG 2>/dev/null | grep -q 'ERR 输入抓取重开失败'"; then
            bad "第 $i 轮：主控端出现"输入抓取重开失败"（本机设备不该受线缆拔插影响）"
        else
            ok "第 $i 轮：主控端抓本机设备，未受线缆拔插影响（$ROLE）"
        fi
    fi

    if kssh 'pgrep -x otikm >/dev/null'; then ok "第 $i 轮：插回后存活"; else bad "第 $i 轮：插回后 otikm 已死"; fi
done

echo "=== 3) 本次回归期间无崩溃 ==="
case "${T0:-}" in ''|*[!0-9:-]*) T0="" ;; esac
if [ -n "$T0" ]; then
    SEGV=$(kssh "journalctl --since '$T0' --no-pager 2>/dev/null | grep -c 'otikm.*segfault'" | tr -d ' ')
else
    SEGV=$(kssh "journalctl -n 500 --no-pager 2>/dev/null | grep -c 'otikm.*segfault'" | tr -d ' ')
fi
case "$SEGV" in ''|*[!0-9]*) SEGV=0 ;; esac
[ "$SEGV" -eq 0 ] && ok "本次回归期间无 otikm segfault" || bad "本次回归期间有 $SEGV 次 otikm segfault"
fi

echo "=== 4) 收尾：保证 otikm 在跑 ==="
if ! kssh 'pgrep -x otikm >/dev/null'; then
    warn "otikm 不在跑，重新拉起"
    start_otikm
fi
kssh 'pgrep -x otikm >/dev/null' && ok "otikm 最终在跑" || bad "otikm 最终没跑起来"

echo
echo "  PASS=$PASS  FAIL=$FAIL  WARN=$WARN"
exit "$FAIL"
