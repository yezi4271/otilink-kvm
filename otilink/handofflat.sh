#!/bin/bash
# handofflat.sh —— 交接/回程"用户感知延迟"的客观测量（合成手势经真实线缆 + 双端时间戳）
#
# 为什么需要它（2026-11-06 真机，见 /tmp/redesign-brief.md §2.3、NOTES §75.5）：
#   用户报"鼠标在 Windows / 麒麟之间来回不顺畅"——这类主观描述必须先变成数字：
#     * 撞边→对端光标开始动（出去方向）
#     * 回推→本端光标恢复移动（回来方向）
#   现状 CURSOR 回传 ~500ms/条、消息泵在鼠标移动时被暂停（HID 优先窗口）→ 交接判据
#   时灵时不灵、偶尔"突然揪回"。修复后期望：单次交接/回程 < 100ms（硬上限 300ms）。
#
# 方法（不依赖人手，走真实线缆）：
#   * 合成 otikm 实例（--sim-input 手势）连续做 CYCLES 次"推出去→推回来"；
#   * Windows 侧 curstortrace.exe（1ms GetCursorPos，带绝对时间）记录对端光标何时开始动；
#   * 麒麟侧 25ms 轮询 `xdotool getmouselocation`（uptime 时间戳）记录本端光标何时恢复；
#   * otikm 日志 `[mono.毫秒] HID 模式：指针交给对端 / 拉回本机` 作为"判决时刻"；
#   * 跨机时钟做 NTP 式校准（取最小 RTT 的一轮，误差 = ±RTT/2，写进结论）。
#
# ⚠️ 会**短暂停止真实 otikm**、启动合成实例、并在本机注入 uinput 鼠标事件（动真实光标），
#    必须由 Lead 排硬件窗口后运行；脚本结束会恢复真实 otikm（run-kylin.sh）。
# ⚠️ 合成实例**不接 X**（env -u DISPLAY）：边缘判据走状态机累计位移，和 kmbret 一致。
#
# 用法：
#   ./handofflat.sh                 # 3 次往返（默认）
#   CYCLES=5 ./handofflat.sh        # 验收要求的 5 次往返
#   EXIT=right ./handofflat.sh      # 物理摆位相反（麒麟在 Windows 左边）时用
#   LIMIT=300 ./handofflat.sh       # 单次延迟硬上限（默认 300ms）
# 退出码 = 失败项数（0 = 通过）。
set -u
export PATH="/mnt/c/Windows/System32/WindowsPowerShell/v1.0:/mnt/c/Windows/System32:$PATH"
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"

CYCLES=${CYCLES:-3}
LIMIT=${LIMIT:-300}
EXIT=${EXIT:-left}                 # 从麒麟哪条边交出去（默认 left：麒麟屏在 Windows 右边）
KSMP=/tmp/handofflat-kcursor.txt   # 麒麟光标采样（uptime ms, x, y）
WINCSV_DIR=/mnt/c/Users/Public/otilink-verify
WINCSV_W='C:\Users\Public\otilink-verify'
CSC=/mnt/c/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe
ART=/tmp/handofflat-artifacts
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$ART"
KLOG=/tmp/handofflat-otikm.log
KINJ=/tmp/handofflat-inject.out
KCON=/tmp/handofflat-console.log
GEST=/tmp/handofflat-gesture.txt

if [ "$EXIT" = left ]; then EXIT_DX=-50; BACK_DX=2; BACK_NAME="右"; else EXIT_DX=50; BACK_DX=-2; BACK_NAME="左"; fi
# 出去：只多推一点（出去多远回来要推一样多）；回来：小步（旧判据要累计、新判据要贴边+外推）
BACK_STEPS=${BACK_STEPS:-400}
DRV_MS=$(( CYCLES * 4800 + 4000 ))      # 合成实例运行时长
TRACE_MS=$(( DRV_MS + 6000 ))

echo "=== 0/6 前提 ==="
khave 'echo up' && ok "麒麟 SSH 可达（$KY）" || bad "麒麟不可达：先跑 tools/gate.sh --pre"
khave 'test -x ~/otilink/otikm' && ok "麒麟 ~/otilink/otikm 存在" || bad "麒麟 ~/otilink/otikm 不存在（先 --deploy-kylin）"
khave 'command -v xdotool >/dev/null' && ok "麒麟 xdotool 可用（采样本端光标）" || bad "麒麟缺 xdotool"
[ -x "$CSC" ] && ok "csc.exe 可用（免管理员）" || bad "缺 csc.exe"
[ -f "$OTI_DIR/cursortrace.cs" ] && ok "采样器源码在（otilink/cursortrace.cs）" || bad "缺 otilink/cursortrace.cs"
[ "$(count_vendor)" = 0 ] && ok "厂商程序未运行（L1）" || bad "厂商程序在跑，先跑 windows/vendor-off.vbs"

echo "=== 1/6 跨机时钟校准（Kylin CLOCK_MONOTONIC → Windows 绝对毫秒）==="
# 每轮：WSL 取 Windows 侧时间近似（同一台机器，WSL 与 Windows 共用硬件时钟），ssh 取麒麟 wall+mono
# 取 RTT 最小的一轮作为标定；误差 = ±RTT/2（跑到 tailscale/relay 上会更大，写进结论）。
CAL=""; BEST_RTT=999999
for i in 1 2 3 4 5 6 7; do
    T0=$(date +%s%3N)
    K=$(kssh 'date +%s%3N; awk "{printf \"%d\", \$1*1000}" /proc/uptime' 2>/dev/null | tr '\n' ' ')
    T1=$(date +%s%3N)
    set -- $K
    KW=${1:-}; KM=${2:-}
    [ -n "$KW" ] && [ -n "$KM" ] || continue
    RTT=$(( T1 - T0 )); MID=$(( (T0 + T1) / 2 ))
    # C = 麒麟单调时钟(mono) 与 Windows 绝对时间 的映射：win_ms ≈ mono_ms + (wall-mono) - (wall-W) = mono + KW-KM-KW+MID
    CI=$(( KM + (KW - KM) - (KW - MID) ))     # 等价：win = mono + MID - KM
    if [ "$RTT" -lt "$BEST_RTT" ]; then BEST_RTT=$RTT; CAL=$(( MID - KM )); fi
done
UNC=$(( BEST_RTT / 2 ))
if [ -n "$CAL" ]; then
    info "时钟映射：win_ms ≈ kylin_mono_ms + ${CAL}（标定 RTT=${BEST_RTT}ms → 误差 ±${UNC}ms；WSL/Windows 共用硬件时钟）"
    [ "$UNC" -le 150 ] && ok "跨机时钟不确定度 ±${UNC}ms（够测 <300ms）" \
        || warn "跨机时钟不确定度 ±${UNC}ms 偏大：出去方向延迟仅供参考，回来方向（同机时钟）仍精确"
else
    bad "时钟校准失败（ssh 取时失败）"
fi

echo "=== 2/6 Windows 采样器 + 麒麟光标采样器 ==="
mkdir -p "$WINCSV_DIR"; cp "$OTI_DIR/cursortrace.cs" "$WINCSV_DIR/cursortrace.cs"
( "$CSC" /nologo /optimize+ /target:exe "/out:$WINCSV_W\\cursortrace.exe" "$WINCSV_W\\cursortrace.cs" ) > "$TMP/csc.out" 2>&1 \
    && ok "cursortrace.exe 编译通过" || bad "cursortrace.exe 编译失败：$(tail -2 "$TMP/csc.out" | tr '\n' ' ')"
WCSV="$WINCSV_DIR/handoff-$ (date +%H%M%S).csv" 2>/dev/null; WCSV="$WINCSV_DIR/handoff-$(date +%H%M%S).csv"
WCSV_W="$WINCSV_W\\$(basename "$WCSV")"
pwsh_file "$WIN_DIR/setcur.ps1" -X 1200 -Y 500 >/dev/null 2>&1
( cd "$WINCSV_DIR" && ./cursortrace.exe "$TRACE_MS" "$WCSV_W" 0 0 > "$TMP/trace.out" 2>&1 ) &
TP=$!
# 麒麟本端光标采样：uptime ms + 坐标（25ms 一轮；用于"回推→本端恢复"的耗时）
kssh "rm -f $KSMP; (while :; do printf '%s ' \$(awk '{printf \"%d\", \$1*1000}' /proc/uptime); DISPLAY=:0 xdotool getmouselocation 2>/dev/null | sed -n 's/^x:\([0-9-]*\) y:\([0-9-]*\).*/\1 \2/p' | head -1; sleep 0.02; done) > $KSMP 2>/dev/null" >/dev/null 2>&1 &
SP=$!
sleep 1

echo "=== 3/6 合成手势（${CYCLES} 次往返，出去 ${EXIT_DX}px/步，回来 ${BACK_DX}px/步 ×${BACK_STEPS}）==="
{
    for _c in $(seq 1 "$CYCLES"); do
        for _ in $(seq 1 24); do echo "mouse $EXIT_DX 0 0 0"; done
        echo "sleep 1200"
        for _ in $(seq 1 "$BACK_STEPS"); do echo "mouse $BACK_DX 0 0 0"; done
        echo "sleep 900"
    done
    echo "sleep 1500"
} > "$TMP/gesture.txt"
GB64=$(base64 -w0 "$TMP/gesture.txt")
kssh "printf '%s' '$GB64' | base64 -d > $GEST && wc -l < $GEST" >/dev/null 2>&1
info "手势 ${CYCLES} 轮 / $(wc -l < "$TMP/gesture.txt") 行 → $GEST"

echo "=== 4/6 停真机 otikm → 跑合成实例（--inject：本端注入真实光标，便于测回程延迟）==="
kssh 'pkill -x otikm' >/dev/null 2>&1
sleep 1
khave 'pgrep -x otikm' && bad "旧 otikm 没停掉（L17：先确保为空再起合成实例）" || ok "真机 otikm 已停（测试独占线缆）"
kssh "rm -f $KLOG $KINJ; cd ~/otilink && sg input -c \"env -u DISPLAY nohup ./otikm --auto --transport cable --keepalive 5000 --config \$HOME/.config/otilink/kvm.conf --role master --edges left,right --no-grab --no-clipboard --inject --sim-input $GEST --log $KLOG --delay 6 --duration $DRV_MS > $KCON 2>&1 &\"" >/dev/null 2>&1
sleep 2
KP=$(kssh 'pgrep -x otikm | tail -1')
[ -n "${KP:-}" ] && ok "合成实例已启动（pid $KP，${DRV_MS}ms）" || bad "合成实例没起来（看 $KCON）"
for _ in $(seq 1 60); do
    [ -n "${KP:-}" ] || break
    khave "kill -0 $KP" || break
    sleep 1
done
if [ -n "${KP:-}" ] && khave "kill -0 $KP"; then
    bad "合成实例没按时退出（>${DRV_MS}ms+30s）"; kssh "kill -9 $KP" >/dev/null 2>&1
else
    ok "合成实例已自行退出"
fi
kssh "cat $KLOG" > "$TMP/otikm.log" 2>/dev/null
kssh 'pkill -x -f "while :; do printf"' >/dev/null 2>&1 || true   # 采样器可能还在跑，下面用文件内容判
wait "$TP" 2>/dev/null || true

echo "=== 5/6 分析（日志判决时刻 + 双端光标变化）==="
# otikm 日志时间戳 = CLOCK_MONOTONIC 秒 % 100000 + 毫秒 → 用 /proc/uptime 还原成 monotonic ms
KHANDOFF=$(grep -a -n 'HID 模式：指针交给对端' "$TMP/otikm.log" | wc -l | tr -d ' ')
KBACK=$(grep -a -n 'HID 模式：指针拉回本机' "$TMP/otikm.log" | wc -l | tr -d ' ')
[ "${KHANDOFF:-0}" -ge "$CYCLES" ] && ok "交出去 ${KHANDOFF} 次（要求 ≥${CYCLES}）" \
    || bad "只交出去 ${KHANDOFF} 次（要求 ≥${CYCLES}；看 $KLOG / $KCON）"
[ "${KBACK:-0}" -ge "$CYCLES" ] && ok "回程 ${KBACK} 次（要求 ≥${CYCLES}）" \
    || bad "只回程 ${KBACK} 次（要求 ≥${CYCLES}）"
info "日志：$TMP/otikm.log → $ART/otikm.log"

# 解析：判决时刻（mono ms）
logts_to_mono() {   # $1 = "sec.mmm" → mono_ms（处理 %100000 回绕）
    awk -v ts="$1" -v up="${2:-0}" 'BEGIN{
        gsub(/,/,".",ts); s=ts+0; base=int(up/100000)*100000;
        m=base+s; if (m > up*1000+60000) m-=100000; printf "%d", m;
    }'
}
UPMS=$(kssh "awk '{printf \"%d\", \$1*1000}' /proc/uptime" 2>/dev/null | tr -d ' ')
[ -n "${UPMS:-}" ] || UPMS=0

# Windows 光标变化（t_ms,x,y,abs_ms → 用 abs_ms 与 win 时间轴）
awk -F, '!/^#/ && NF>=4 {print $4}' "$WCSV" > "$TMP/wchange_abs.txt" 2>/dev/null
# 麒麟本端光标采样：uptime_ms x y
awk 'NF>=3 {print $1, $2, $3}' "$KSMP" > "$TMP/ksamples.txt" 2>/dev/null
info "Windows 光标变化 $(wc -l < "$TMP/wchange_abs.txt" 2>/dev/null || echo 0) 次；本端采样 $(wc -l < "$TMP/ksamples.txt" 2>/dev/null || echo 0) 行"

TOUT_LIST=""; TBACK_LIST=""
N_OUT_OK=0; N_BACK_OK=0; N_OUT=0; N_BACKM=0
i=0
while IFS= read -r line; do
    i=$((i+1))
    ts=$(printf '%s' "$line" | sed -n 's/^\[\([0-9]*\.[0-9]*\)\].*/\1/p')
    [ -n "$ts" ] || continue
    mono=$(logts_to_mono "$ts" "$UPMS")
    win=$(( mono + CAL ))                     # 映射到 Windows 绝对 ms（误差 ±UNC）
    # 出去：Windows 光标在判决后第一次变化
    FIRST_W=$(awk -v w="$win" '$1 >= w {print $1; exit}' "$TMP/wchange_abs.txt" 2>/dev/null)
    if [ -n "$FIRST_W" ]; then
        d=$(( FIRST_W - win )); N_OUT=$((N_OUT+1))
        TOUT_LIST="$TOUT_LIST $d"; [ "$d" -le "$LIMIT" ] && N_OUT_OK=$((N_OUT_OK+1))
    fi
done < <(grep -a 'HID 模式：指针交给对端' "$TMP/otikm.log")

i=0
while IFS= read -r line; do
    i=$((i+1))
    ts=$(printf '%s' "$line" | sed -n 's/^\[\([0-9]*\.[0-9]*\)\].*/\1/p')
    [ -n "$ts" ] || continue
    mono=$(logts_to_mono "$ts" "$UPMS")
    # 回程：本端光标（uptime ms 轴）在判决后第一次位置变化
    FIRST_K=$(awk -v m="$mono" '
        BEGIN{px="";py=""}
        { t=$1+0; x=$2; y=$3;
          if (py != "" && (x != px || y != py) && t >= m) { print t; exit }
          px=x; py=y
        }' "$TMP/ksamples.txt" 2>/dev/null)
    if [ -n "$FIRST_K" ]; then
        d=$(( FIRST_K - mono )); N_BACKM=$((N_BACKM+1))
        TBACK_LIST="$TBACK_LIST $d"; [ "$d" -le "$LIMIT" ] && N_BACK_OK=$((N_BACK_OK+1))
    fi
done < <(grep -a 'HID 模式：指针拉回本机' "$TMP/otikm.log")

info "出去方向延迟(ms)：${TOUT_LIST:-无}"
info "回来方向延迟(ms)：${TBACK_LIST:-无}"

echo "=== 6/6 判定 ==="
[ "$N_OUT" -ge 1 ] && [ "$N_OUT_OK" -eq "$N_OUT" ] && ok "撞边→对端开始动：全部 ${N_OUT} 次 ≤ ${LIMIT}ms${UNC:+（跨机时钟 ±${UNC}ms）}" \
    || bad "撞边→对端开始动：${N_OUT_OK}/${N_OUT} 次 ≤ ${LIMIT}ms（原始：${TOUT_LIST:-无}；时钟误差 ±${UNC:-?}ms）"
[ "$N_BACKM" -ge 1 ] && [ "$N_BACK_OK" -eq "$N_BACKM" ] && ok "回推→本端恢复：全部 ${N_BACKM} 次 ≤ ${LIMIT}ms（同机时钟）" \
    || bad "回推→本端恢复：${N_BACK_OK}/${N_BACKM} 次 ≤ ${LIMIT}ms（原始：${TBACK_LIST:-无}）"
[ "$N_OUT" -ge "$CYCLES" ] && [ "$N_BACKM" -ge "$CYCLES" ] && ok "往返 ${CYCLES} 次都测到延迟样本" \
    || warn "只测到出去 ${N_OUT} / 回来 ${N_BACKM} 个延迟样本（<${CYCLES}；看日志$KLOG）"

cp "$TMP/otikm.log" "$ART/" 2>/dev/null; cp "$KSMP" "$ART/" 2>/dev/null; cp "$WCSV" "$ART/" 2>/dev/null
cp "$TMP/trace.out" "$ART/trace.out" 2>/dev/null

echo "=== 恢复真机 otikm（run-kylin.sh）==="
kssh 'cd ~/otilink && sg input -c "DISPLAY=:0 nohup ./run-kylin.sh > /tmp/rk.log 2>&1 &"' >/dev/null 2>&1
for _ in $(seq 1 15); do [ -n "$(kssh 'pgrep -x otikm')" ] && break; sleep 1; done
[ -n "$(kssh 'pgrep -x otikm')" ] && ok "真机 otikm 已恢复" || bad "真机 otikm 没恢复：手动跑 ~/otilink/run-kylin.sh"

summary "交接/回程延迟（handofflat）"
