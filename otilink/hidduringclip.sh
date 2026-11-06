#!/bin/bash
# hidduringclip.sh —— 650KB 剪贴板传输期间"键鼠还卡不卡"的真机回归（麒麟主控，单队列设备争用）
#
# 为什么需要它（2026-11-06 真机证据，见 /tmp/redesign-brief.md §1、NOTES §75.5/§75.6）：
#   现状（旧 PowerShell 剪贴板代理）下，麒麟向 Windows 写 64KB 帧时会在 SCSI 命令里
#   **阻塞 250~700ms**；对拷线是单队列设备（/sys/block/sdb/device/queue_depth=1），
#   HID 注入写和剪贴板帧共用同一条 SCSI 管道，于是帧写把 HID 写堵在后面
#   → 用户看到"鼠标每几秒冻结一下"。实测基线：与 otikm 并发时 hidlat
#   **MAXGAP=372~482ms**；Windows 换原生快速读帧循环后同场景 **MAXGAP=14ms**。
#   这条回归把"边传剪贴板边动鼠标不卡"固化成数字：MAXGAP ≤ 40ms 且 HID 写零失败。
#
# 本脚本做什么（真机，麒麟；**不**停/起 otikm、**不**动 Windows 代理）：
#   1) 麒麟上编译 hidlat（推送仓库里的 hidlat.c，不依赖 ~/otilink 是否已同步）；
#   2) hidlat 与 otikm **并发**跑 N 秒（真竞争），同时第 2s 把 650KB 文本放进
#      麒麟剪贴板（setclip.sh），第 ~10s 可选再放一张图片；
#   3) 断言：MAXGAP ≤ 阈值、SEND_FAIL=0，并从**麒麟 otikm 日志**与 **Windows clip.log**
#      确认 650KB 确实下发/应用（否则"没传就没卡"是假阴性）。
#
# 用法：
#   ./hidduringclip.sh                    # 20s，MAXGAP 阈值 40ms，650000 字节文本
#   ./hidduringclip.sh 20 40 650000       # [秒数] [MAXGAP 阈值 ms] [文本字节数]
#   ./hidduringclip.sh 20 40 650000 --with-image   # 额外在 ~10s 放一张 testimg.png
#
# 副作用：会覆盖麒麟 X11 剪贴板（结束后写回一行短文本）；只写 /tmp 与
#         C:\Users\Public\otilink-verify\；不动 otikm / Windows 代理进程。
#
# 判定口径（诚实优先）：
#   * **硬断言**：MAXGAP ≤ 阈值、SEND_FAIL=0，以及"麒麟确实下发了 <BYTES>"（证明确实有负载）。
#   * Windows 侧"已应用远端剪贴板"是**附加证据**：Windows 锁屏时 Set-Clipboard/Get-Clipboard
#     必然抛 ExternalException（实测，见 NOTES §76.5），此时应用/读取一律只 WARN，不计 FAIL。
# 退出码 = 失败项数（0 = 通过）。日志：/tmp/hidduringclip-artifacts/
set -u
# 本仓库脚本依赖 powershell.exe；某些 agent/WSL shell 的 PATH 里没有 Windows 目录 → 兜底
export PATH="/mnt/c/Windows/System32/WindowsPowerShell/v1.0:/mnt/c/Windows/System32:$PATH"
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"

SECS=${1:-20}
LIMIT=${2:-40}
BYTES=${3:-650000}
WITH_IMAGE=0
for a in "$@"; do
    case "$a" in --with-image) WITH_IMAGE=1 ;; esac
done

AGENT_LOG=/mnt/c/Users/Public/clip.log          # Windows 剪贴板代理日志（/mnt/c 直读，避开编码坑）
ART=/tmp/hidduringclip-artifacts
mkdir -p "$ART"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
BIG=/tmp/otilink-big.txt
DEV=""

echo "=== 0/4 前提 ==="
khave 'echo up' && ok "麒麟 SSH 可达（$KY）" || bad "麒麟不可达：先跑 tools/gate.sh --pre"
NPROC=$(kssh 'pgrep -x otikm | wc -l' | tr -d ' ')
[ "${NPROC:-0}" = "1" ] && ok "麒麟 otikm 恰好 1 个（并发竞争才有意义）" \
    || bad "麒麟 otikm 数量=${NPROC:-?}（应为 1；0 = 没跑，>1 = L17 两个主控）"

# 线缆设备自动发现（0ea0）：拔插后 sg 号会变，不能写死
DEV=$(kssh 'for s in /sys/class/scsi_generic/sg*; do d=$(readlink -f "$s/device" 2>/dev/null); while [ -n "$d" ] && [ "$d" != "/" ]; do [ -f "$d/idVendor" ] && break; d=$(dirname "$d"); done; if [ -f "$d/idVendor" ] && grep -qi 0ea0 "$d/idVendor" 2>/dev/null; then echo "/dev/$(basename "$s")"; break; fi; done')
[ -n "$DEV" ] && ok "线缆设备自动发现：$DEV（0ea0）" || bad "找不到线缆 SCSI 节点（0ea0）—— 插好了吗？"
khave 'command -v xclip >/dev/null && command -v xdotool >/dev/null' && ok "麒麟 xclip/xdotool 可用" \
    || bad "麒麟缺 xclip/xdotool"

# 旧代理才测得出现状卡顿；无论如何先记下两端 build/部署时间，便于基线 vs 修复后对照
WINCLIP_N=0
if [ -f "$AGENT_LOG" ]; then
    WINCLIP_N=$(count_clip)
    [ "${WINCLIP_N:-0}" -ge 1 ] && ok "Windows 剪贴板代理在跑（$WINCLIP_N 个）" \
        || bad "Windows 剪贴板代理没跑：剪贴板不会下发，本条测量无效（先 windows/deploy.sh --restart）"
else
    bad "看不到 $AGENT_LOG（Windows Public 目录/路径不对）"
fi
K_BUILD=$(kssh 'ls -l --time-style=+%F_%T ~/otilink/otikm 2>/dev/null | awk "{print \$6, \$7}"; md5sum ~/otilink/otikm 2>/dev/null | cut -c1-12' | tr '\n' ' ')
W_BUILD=$(grep -a -E 'build |PUMP rx=' "$AGENT_LOG" 2>/dev/null | tail -1 | cut -c1-140)
info "Kylin otikm build: ${K_BUILD:-?}"
info "Windows 代理 build: ${W_BUILD:-?}"

echo "=== 1/4 麒麟侧编译 hidlat（推送本仓库源码，验证 move_dx 向后兼容）==="
GB64=$(base64 -w0 "$OTI_DIR/hidlat.c")
kssh "printf '%s' '$GB64' | base64 -d > /tmp/hidlat.c && wc -c < /tmp/hidlat.c" > "$TMP/push.out" 2>/dev/null
if kssh 'cd /tmp && gcc -O2 -I$HOME/otilink -o /tmp/hidlat /tmp/hidlat.c $HOME/otilink/otilink.c $HOME/otilink/otiproto.c $HOME/otilink/otitrans.c $HOME/otilink/otihid.c -lpthread' > "$TMP/cc.out" 2>&1; then
    ok "hidlat 编译通过（用当前工作树的 hidlat.c）"
else
    bad "hidlat 编译失败：$(tail -3 "$TMP/cc.out" | tr '\n' ' ')"
fi

# 传输前记录日志位置：只看**新增行**，避免匹配到历史的大剪贴板记录
KLOG=/tmp/otikm.log
KSZ=$(kssh "wc -c < $KLOG 2>/dev/null" | tr -d ' ')
WSZ=0; [ -f "$AGENT_LOG" ] && WSZ=$(wc -c < "$AGENT_LOG" | tr -d ' ')
info "日志基线：kylin $KLOG=$KSZ bytes；Windows clip.log=$WSZ bytes"

echo "=== 2/4 并发压测：hidlat ${SECS}s + 第 2s 放 ${BYTES} 字节文本 ==="
# hidlat 在后台与 otikm 抢单队列设备；它不碰剪贴板，只写 HID 包（设备级延迟测量）
( kssh "/tmp/hidlat $DEV $SECS $LIMIT" > "$TMP/hidlat.out" 2>&1 ) &
HP=$!
sleep 2
# 650KB 文本：按 setclip.sh 的约定，xclip 子进程要脱离 ssh 通道（setclip.sh 内部已 setsid）
if kssh "head -c $BYTES /dev/zero | tr '\\0' 'A' > $BIG && wc -c < $BIG" > "$TMP/big.out" 2>/dev/null; then
    GOT=$(tr -d ' ' < "$TMP/big.out" | tail -1)
    [ "${GOT:-0}" = "$BYTES" ] && ok "麒麟 /tmp/otilink-big.txt = $GOT 字节" \
        || warn "大文本字节数 ${GOT:-0}（期望 $BYTES）"
    kssh "DISPLAY=:0 sh ~/otilink/setclip.sh $BIG" >/dev/null 2>&1
    if [ -f "$AGENT_LOG" ] || [ "${WINCLIP_N:-0}" -ge 1 ]; then
        info "已在 t≈2s 把 ${BYTES} 字节文本放进麒麟剪贴板（触发 otikm 下发 10×64KB 帧）"
    fi
else
    bad "无法在麒麟生成 $BIG"
fi
if [ "$WITH_IMAGE" = 1 ] && [ -f "$OTI_DIR/testimg.png" ]; then
    sleep 6
    kssh "cat > /tmp/otilink-testimg.png" < "$OTI_DIR/testimg.png" 2>/dev/null
    kssh "DISPLAY=:0 sh ~/otilink/setclip.sh /tmp/otilink-testimg.png image/png" >/dev/null 2>&1
    info "已在 t≈8s 把 testimg.png（$(wc -c < "$OTI_DIR/testimg.png") 字节）放进剪贴板（附加压力）"
fi
wait "$HP"
HIDOUT=$(cat "$TMP/hidlat.out")
echo "$HIDOUT" | grep -E '^GAP|^MAXGAP' | tail -10 | sed 's/^/      /'

echo "=== 3/4 剪贴板是否真的下发/应用（避免"没传就没卡"的假阴性）==="
# 麒麟侧：otikm 日志新增行里应有 "CLIP 发送 <BYTES> 字节" 或 "重发 <BYTES>"
KNEW=$(kssh "tail -c +$(( ${KSZ:-0} + 1 )) $KLOG 2>/dev/null" | tr -d '\r')
printf '%s' "$KNEW" > "$ART/otikm.log.delta"
K_SEND=$(printf '%s' "$KNEW" | grep -a -c -E "CLIP 发送 ${BYTES} 字节|重发 ${BYTES} 字节|CLIP 发送 ${BYTES} " || true)
[ "${K_SEND:-0}" -ge 1 ] && ok "麒麟 otikm 已下发 ${BYTES} 字节（日志新增 $K_SEND 行 CLIP 发送/重发）" \
    || bad "麒麟 otikm 日志新增里没有下发 ${BYTES} 字节的 CLIP 行（见 $ART/otikm.log.delta）"
printf '%s' "$KNEW" | grep -a -E 'CLIP 发送|CLIP 应用|重发' | tail -4 | sed 's/^/      /'

# Windows 侧：clip.log 新增行应有 "CLIP 已应用远端剪贴板 <BYTES> 字节"。
# ⚠️ 这一项是**附加证据，不是硬断言**：Windows 锁屏时剪贴板 API 必然不可用（NOTES §76.5），
#    所以"应用/读取"结果只 ok/warn/info，绝不计 FAIL。
WLOCK="UNKNOWN"
WLOCK=$(pwsh_cmd 'if (Get-Process LogonUI -EA SilentlyContinue) { "LOCK" } else { "UNLOCK" }' 2>/dev/null | tail -1 | tr -d ' \r')
if [ "${WLOCK:-}" = "LOCK" ]; then
    warn "Windows 疑似锁屏（LogonUI 在跑）：剪贴板'已应用'证据不可用 → 只 WARN 不计 FAIL（NOTES §76.5；解锁后应重跑并辅以 clipreg.sh）"
    warn "本次判定只依据：MAXGAP / SEND_FAIL / 麒麟侧已下发（锁屏下应用/ACK 无法验证）"
fi
WAPPLY=0
if [ -f "$AGENT_LOG" ]; then
    WNEW=$(tail -c +"$(( ${WSZ:-0} + 1 ))" "$AGENT_LOG" 2>/dev/null | tr -d '\r')
    printf '%s' "$WNEW" > "$ART/clip.log.delta"
    WAPPLY=$(printf '%s' "$WNEW" | grep -a -c -E "已应用远端剪贴板 ${BYTES} 字节|已应用远端.*${BYTES} 字节" || true)
fi
# 独立判据：直接读 Windows 剪贴板内容长度/字符（不依赖日志格式）
WCLIP_LEN=""
WCLIP_LEN=$(pwsh_cmd '$t = Get-Clipboard -Raw -Format Text -EA SilentlyContinue; if ($t -ne $null) { $t.Length } else { "none" }' | tail -1 | tr -d ' ')
[ "${WCLIP_LEN:-none}" = "$BYTES" ] && ok "Windows 剪贴板实际内容长度 = $BYTES（已应用）" \
    || warn "Windows 剪贴板实际长度=${WCLIP_LEN:-?}（期望 $BYTES；日志判据 ${WAPPLY:-0} 行）"
[ "${WAPPLY:-0}" -ge 1 ] && ok "Windows clip.log 新增 ${WAPPLY} 行「已应用远端 ${BYTES} 字节」" \
    || info "clip.log 没有匹配的应用行（可能日志格式随新代理变了；以上面直接读剪贴板为准）"

echo "=== 4/4 判定 ==="
G=$(printf '%s' "$HIDOUT" | sed -n 's/^MAXGAP=\([0-9]*\).*/\1/p' | tail -1)
NF=$(printf '%s' "$HIDOUT" | sed -n 's/.*SEND_FAIL=\([0-9]*\).*/\1/p' | tail -1)
if [ -z "${G:-}" ]; then
    bad "拿不到 MAXGAP（hidlat 没跑起来/设备不可用？见 $ART）"
else
    [ "$G" -le "$LIMIT" ] && ok "650KB 剪贴板传输期间 HID 写最大间隔 ${G}ms ≤ ${LIMIT}ms" \
        || bad "650KB 剪贴板传输期间 HID 写最大间隔 ${G}ms > ${LIMIT}ms：帧写把单队列设备堵住了（看上面 GAP 行）"
    [ "${NF:-1}" -eq 0 ] && ok "HID 写没有失败（SEND_FAIL=0）" \
        || bad "HID 写失败 ${NF} 次（设备/会话有问题，MAXGAP 不可信）"
fi
# 附带证据：SLOWOP / 慢写（新 transport 才有），有就打印，便于对照
SLOW=$(printf '%s' "$KNEW" | grep -a -c 'SLOWOP' || true)
info "otikm 日志新增 SLOWOP 行：${SLOW:-0}（新 transport 下应见到帧写慢自动让路 HID）"

# 失败现场落盘
cp "$TMP/hidlat.out" "$ART/hidlat.out" 2>/dev/null
cp "$TMP/cc.out" "$ART/hidlat-cc.log" 2>/dev/null
if [ "$FAIL" -gt 0 ]; then
    kssh "tail -c +$(( ${KSZ:-0} + 1 )) $KLOG 2>/dev/null" > "$ART/otikm.log.delta" 2>/dev/null
    [ -f "$AGENT_LOG" ] && tail -c +"$(( ${WSZ:-0} + 1 ))" "$AGENT_LOG" > "$ART/clip.log.delta" 2>/dev/null
    info "失败现场已保存到 $ART/（hidlat.out / otikm.log.delta / clip.log.delta）"
fi
# 收尾：别把 650KB 留在剪贴板（下一步测试/用户操作都受影响）；只写一行短文本
kssh "printf 'OTILINK hidduringclip done' | DISPLAY=:0 xclip -selection clipboard" >/dev/null 2>&1

summary "650KB 剪贴板期间键鼠卡顿回归（hidduringclip）"
