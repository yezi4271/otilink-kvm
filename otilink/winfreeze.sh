#!/bin/bash
# winfreeze.sh —— Windows 光标"冻结"的客观测量：合成 HID 驱动 + 1ms GetCursorPos 采样
#
# 为什么需要它（2026-11-06 真机证据，见 /tmp/redesign-brief.md §1、NOTES §75.5）：
#   现状 Windows 剪贴板代理是 PowerShell 主循环（取帧一轮 1.3~9s）；麒麟向它写 64KB 帧时
#   在 SCSI 命令里阻塞 250~700ms。对拷线是**单队列**设备（queue_depth=1），HID 注入写
#   排在帧写后面 → 用户看到"鼠标突然冻住"（历史实测 100~1400ms 冻结，最长 2.49s）。
#   本脚本把"冻结"量成 MAXFREEZE：麒麟侧 hidlat 以合成 HID 鼠标包驱动 Windows 光标，
#   Windows 侧 cursortrace.exe 以 1ms 采样 GetCursorPos，取相邻两次**位置变化**的最大间隔。
#   修复前会抓到几百 ms~2.5s；修复后应 ≤50ms。
#
# 用法：
#   ./winfreeze.sh                    # 15s 合成 HID 驱动，MAXFREEZE 上限 50ms
#   ./winfreeze.sh 15 50 --driver=hidlat   # 同上（显式指定驱动方式）
#   ./winfreeze.sh 15 50 --driver=none     # 不用合成驱动：**请人在 15s 内移动麒麟/Windows 鼠标**
#   ./winfreeze.sh 15 50 --with-clip       # 驱动期间再触发 650KB 剪贴板传输（最坏时刻）
#
# 驱动方式 auto（默认）：otikm 日志当前处于"HID 模式：指针交给对端"→ 用 hidlat 合成驱动
#   （直写 HID 包经线缆到 Windows 真实 HID）；否则退化为 none 并提示。
#   ⚠️ 2026-11-06 实测：otikm 处于**本机控制态**时，直写 HID 包几乎推不动 Windows 光标
#   （怀疑线缆只在"指针已交给对端"时转发 HID）→ 那种情况下的冻结数字不可信，脚本会判"测量无效"。
#
# 判定口径（诚实优先）：
#   * 有效性：有效移动段 ≥3s 且位置变化次数 ≥100（否则"测不出冻结"= FAIL，不伪造数字）；
#   * MAXFREEZE 在"首个位置变化 +300ms ~ 末个位置变化 -300ms"的有效段内统计
#     （自动排除采样先启动/驱动后停止两头的无输入空闲）；
#   * 修复后期望 MAXFREEZE ≤ 50ms 且 FREEZES_OVER_50=0。
#
# 副作用：会移动真实 Windows 光标（合成 HID 或人手）；只写
#         C:\Users\Public\otilink-verify\ 与 /tmp/winfreeze-artifacts/；不动 otikm/代理进程。
# 退出码 = 失败项数（0 = 通过）。
set -u
export PATH="/mnt/c/Windows/System32/WindowsPowerShell/v1.0:/mnt/c/Windows/System32:$PATH"
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"

DRV=${1:-15}
LIM=${2:-50}
DRIVER=auto
WITH_CLIP=0
for a in "$@"; do
    case "$a" in
        --driver=*) DRIVER=${a#--driver=} ;;
        --with-clip) WITH_CLIP=1 ;;
    esac
done
case "$DRIVER" in auto|hidlat|none) ;; *) echo "未知 --driver=$DRIVER（auto|hidlat|none）" >&2; exit 2 ;; esac

CSC=/mnt/c/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe
WINTMP=/mnt/c/Users/Public/otilink-verify           # Windows 侧临时测试产物（不入库、不部署）
WINTM_W='C:\Users\Public\otilink-verify'
AGENT_LOG=/mnt/c/Users/Public/clip.log
ART=/tmp/winfreeze-artifacts
mkdir -p "$ART"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TS=$(date +%Y%m%d-%H%M%S)
CSV="$WINTMP/trace-$TS.csv"
CSV_W="$WINTM_W\\trace-$TS.csv"
DEV=""; HIDMODE=0

echo "=== 0/5 前提 ==="
khave 'echo up' && ok "麒麟 SSH 可达（$KY）" || bad "麒麟不可达：先跑 tools/gate.sh --pre"
NPROC=$(kssh 'pgrep -x otikm | wc -l' | tr -d ' ')
[ "${NPROC:-0}" = "1" ] && ok "麒麟 otikm 恰好 1 个（合成驱动/竞争都依赖它）" \
    || bad "麒麟 otikm 数量=${NPROC:-?}（应为 1）"
DEV=$(kssh 'for s in /sys/class/scsi_generic/sg*; do d=$(readlink -f "$s/device" 2>/dev/null); while [ -n "$d" ] && [ "$d" != "/" ]; do [ -f "$d/idVendor" ] && break; d=$(dirname "$d"); done; if [ -f "$d/idVendor" ] && grep -qi 0ea0 "$d/idVendor" 2>/dev/null; then echo "/dev/$(basename "$s")"; break; fi; done')
[ -n "$DEV" ] && ok "线缆设备自动发现：$DEV（0ea0）" || bad "找不到线缆 SCSI 节点（0ea0）"
[ -x "$CSC" ] && ok "csc.exe 可用（.NET Framework 4，免管理员）" || bad "缺 csc.exe：$CSC"
[ -f "$OTI_DIR/cursortrace.cs" ] && ok "采样器源码在（otilink/cursortrace.cs）" || bad "缺 otilink/cursortrace.cs"
K_BUILD=$(kssh 'ls -l --time-style=+%F_%T ~/otilink/otikm 2>/dev/null | awk "{print \$6, \$7}"; md5sum ~/otilink/otikm 2>/dev/null | cut -c1-12' | tr '\n' ' ')
W_BUILD=$(grep -a -E 'build |PUMP rx=' "$AGENT_LOG" 2>/dev/null | tail -1 | cut -c1-140)
info "Kylin otikm build: ${K_BUILD:-?}"
info "Windows 代理 build: ${W_BUILD:-?}"

# 驱动方式判定：直写 HID 包是否真能驱动对端光标，取决于 otikm 是否已把指针交给对端
LAST_MODE=$(kssh "grep -a 'HID 模式：指针交给对端\|HID 模式：指针拉回本机' /tmp/otikm.log 2>/dev/null | tail -1" | tr -d '\r')
case "$LAST_MODE" in
    *交给对端*) HIDMODE=1; ok "otikm 当前处于 HID 模式（指针在对端）→ 可用合成 HID 驱动" ;;
    *)          HIDMODE=0; info "otikm 当前未把指针交给对端（$(printf '%s' "$LAST_MODE" | cut -c1-60)）" ;;
esac
if [ "$DRIVER" = auto ]; then
    if [ "$HIDMODE" = 1 ]; then DRIVER=hidlat; else DRIVER=none; fi
fi
case "$DRIVER" in
    hidlat)
        [ "$HIDMODE" = 1 ] && ok "驱动方式 = hidlat（合成 HID）" \
            || warn "驱动方式 = hidlat，但 otikm 不在 HID 模式：直写 HID 可能推不动对端光标（测量可能无效）"
        ;;
    none)
        warn "驱动方式 = none：**请人在 ${DRV}s 内持续移动鼠标**（真实路径），否则本次测量无效"
        ;;
esac

echo "=== 1/5 编译 Windows 采样器（csc，免管理员）==="
mkdir -p "$WINTMP"
cp "$OTI_DIR/cursortrace.cs" "$WINTMP/cursortrace.cs"
if ( "$CSC" /nologo /optimize+ /target:exe "/out:$WINTM_W\\cursortrace.exe" "$WINTM_W\\cursortrace.cs" ) > "$TMP/csc.out" 2>&1; then
    ok "cursortrace.exe 编译通过（$(stat -c%s "$WINTMP/cursortrace.exe" 2>/dev/null || echo ?) 字节）"
else
    bad "cursortrace.exe 编译失败：$(tail -3 "$TMP/csc.out" | tr '\n' ' ')"
fi

if [ "$DRIVER" = hidlat ]; then
    echo "=== 2/5 麒麟侧编译 hidlat（推送本仓库源码）==="
    GB64=$(base64 -w0 "$OTI_DIR/hidlat.c")
    kssh "printf '%s' '$GB64' | base64 -d > /tmp/hidlat.c" >/dev/null 2>&1
    if kssh 'cd /tmp && gcc -O2 -I$HOME/otilink -o /tmp/hidlat /tmp/hidlat.c $HOME/otilink/otilink.c $HOME/otilink/otiproto.c $HOME/otilink/otitrans.c $HOME/otilink/otihid.c -lpthread' > "$TMP/cc.out" 2>&1; then
        ok "hidlat 编译通过（合成驱动 move_dx=10 chunk=20）"
    else
        bad "hidlat 编译失败：$(tail -3 "$TMP/cc.out" | tr '\n' ' ')"
    fi
else
    echo "=== 2/5 跳过麒麟驱动编译（--driver=none）==="
    info "本次由真人移动鼠标产生位移"
fi

echo "=== 3/5 采样（${DRV}s 驱动窗口，1ms GetCursorPos）==="
TRACE_MS=$(( DRV * 1000 + 6000 ))       # 留 6s 余量给 ssh 启动/收尾；统计会按实际位移段裁剪
# 光标归位：给合成驱动留出左右余量（不会因为贴屏幕边而"位移看不出来"）
pwsh_file "$WIN_DIR/setcur.ps1" -X 1200 -Y 500 >/dev/null 2>&1
W0=$(wcur); [ -n "${W0:-}" ] && info "Windows 光标已归位到 1200,500（当前 $W0）"
( cd "$WINTMP" && ./cursortrace.exe "$TRACE_MS" "$CSV_W" 800 800 > "$TMP/trace.out" 2>&1 ) &
TP=$!
sleep 1.0
if [ "$DRIVER" = hidlat ]; then
    # 合成驱动：单向 move_dx=10、每 chunk=20 包换向（每 200ms 一折返，光标在 1200,500 附近来回；
    # 每包交替会因对端合并/加速只挪 1~2px，不能用）。
    ( kssh "/tmp/hidlat $DEV $DRV $LIM 10 20" > "$TMP/hidlat.out" 2>&1 ) &
    DP=$!
    if [ "$WITH_CLIP" = 1 ]; then
        sleep 1.5
        kssh "head -c 650000 /dev/zero | tr '\\0' 'A' > /tmp/otilink-big.txt && DISPLAY=:0 sh ~/otilink/setclip.sh /tmp/otilink-big.txt" >/dev/null 2>&1
        info "已触发 650KB 剪贴板传输（最坏时刻冻结测量）"
    fi
    wait "$DP"
    cat "$TMP/hidlat.out" | grep -E '^GAP|^MAXGAP' | tail -8 | sed 's/^/      /'
else
    [ "$WITH_CLIP" = 1 ] && { sleep 1.5; kssh "head -c 650000 /dev/zero | tr '\\0' 'A' > /tmp/otilink-big.txt && DISPLAY=:0 sh ~/otilink/setclip.sh /tmp/otilink-big.txt" >/dev/null 2>&1; info "已触发 650KB 剪贴板传输"; }
    info "等待 ${DRV}s 真人移动鼠标…"
    sleep "$DRV"
fi
wait "$TP"
cat "$TMP/trace.out" | sed 's/^/      /'
cp "$CSV" "$ART/" 2>/dev/null; cp "$TMP/trace.out" "$ART/trace.out" 2>/dev/null
[ -f "$TMP/hidlat.out" ] && cp "$TMP/hidlat.out" "$ART/hidlat.out" 2>/dev/null

echo "=== 4/5 分析（按实际位移段裁剪，排除驱动前后的无输入空闲）==="
# CSV 行：t_ms,x,y,abs_ms；只在位置变化时记一行。有效段 = 首个变化 +300ms ~ 末个变化 -300ms。
STATS=$(awk -F, '
    /^#/ { next }
    NF >= 3 { n++; t[n] = $1 + 0; x[n] = $2 + 0 }
    END {
        if (n < 2) { printf "MOVES=%d ACTIVE_MS=0 MAXFREEZE=-1 FREEZES_OVER_50=0 FREEZES_OVER_100=0 XSPAN=0\n", n; exit }
        xmin = x[1]; xmax = x[1];
        for (i = 1; i <= n; i++) { if (x[i] < xmin) xmin = x[i]; if (x[i] > xmax) xmax = x[i] }
        first = t[1] + 300; last = t[n] - 300;
        act = last - first; if (act < 0) act = 0;
        mx = 0; o50 = 0; o100 = 0; gaps = 0;
        for (i = 2; i <= n; i++) {
            if (t[i-1] >= first && t[i] <= last) {
                g = t[i] - t[i-1]; gaps++;
                if (g > mx) mx = g;
                if (g > 50) o50++;
                if (g > 100) o100++;
            }
        }
        printf "MOVES=%d ACTIVE_MS=%d GAPS=%d MAXFREEZE=%d FREEZES_OVER_50=%d FREEZES_OVER_100=%d XSPAN=%d\n",
               n, act, gaps, mx, o50, o100, xmax - xmin;
    }' "$CSV" 2>/dev/null)
echo "      $STATS"
MAXFREEZE=$(printf '%s' "$STATS" | sed -n 's/.*MAXFREEZE=\([-0-9]*\).*/\1/p')
OVER50=$(printf '%s' "$STATS" | sed -n 's/.*FREEZES_OVER_50=\([0-9]*\).*/\1/p')
MOVES=$(printf '%s' "$STATS" | sed -n 's/.*MOVES=\([0-9]*\).*/\1/p')
ACT=$(printf '%s' "$STATS" | sed -n 's/.*ACTIVE_MS=\([0-9]*\).*/\1/p')
XSPAN=$(printf '%s' "$STATS" | sed -n 's/.*XSPAN=\([0-9]*\).*/\1/p')

# 有效性：驱动真的把光标带起来了吗？（否则"测不出冻结"不能当 PASS）
MINMOVE=100; MINACT=$(( DRV * 1000 * 7 / 10 ))
if [ "${MOVES:-0}" -ge "$MINMOVE" ] && [ "${ACT:-0}" -ge "$MINACT" ] && [ "${MAXFREEZE:--1}" -ge 0 ]; then
    ok "采样有效：位移 ${ACT}ms / ${MOVES} 次位置变化（≥${MINACT}ms / ≥${MINMOVE} 次）"
else
    bad "采样无效：位移 ${ACT:-0}ms / ${MOVES:-0} 次变化（要求 ≥${MINACT}ms / ≥${MINMOVE} 次）——驱动没把光标带起来（otikm 不在 HID 模式？驱动/线缆？看 $ART）"
fi

echo "=== 5/5 判定 ==="
if [ "${MAXFREEZE:--1}" -lt 0 ]; then
    bad "拿不到 MAXFREEZE（采样无效，见上）"
else
    [ "$MAXFREEZE" -le "$LIM" ] && ok "Windows 光标最大冻结 ${MAXFREEZE}ms ≤ ${LIM}ms（用户不该感到卡顿）" \
        || bad "Windows 光标最大冻结 ${MAXFREEZE}ms > ${LIM}ms：HID 写被单队列设备堵住（看 $ART/trace-*.csv）"
    [ "${OVER50:-1}" -eq 0 ] && ok "没有 >50ms 的冻结（FREEZES_OVER_50=0）" \
        || bad "有 ${OVER50} 次 >50ms 的冻结（最长 ${MAXFREEZE}ms）"
    [ "${XSPAN:-0}" -ge 20 ] && ok "光标确实被驱动：采样期间 X 跨度 ${XSPAN}px" \
        || warn "X 跨度仅 ${XSPAN:-0}px：位移太小，冻结数字可能受采样粒度影响（建议用 --driver=none + 真人移动复核）"
fi
if [ -f "$TMP/hidlat.out" ]; then
    HG=$(sed -n 's/^MAXGAP=\([0-9]*\).*/\1/p' "$TMP/hidlat.out" | tail -1)
    HF=$(sed -n 's/.*SEND_FAIL=\([0-9]*\).*/\1/p' "$TMP/hidlat.out" | tail -1)
    info "交叉证据（麒麟侧同一时刻）：HID 写 MAXGAP=${HG:-?}ms SEND_FAIL=${HF:-?}"
    [ "${HF:-0}" -eq 0 ] || warn "麒麟侧 HID 写失败 ${HF} 次（设备/会话异常，冻结数字需谨慎）"
fi
if [ "$WITH_CLIP" = 1 ]; then
    WCLIP_LEN=$(pwsh_cmd '$t = Get-Clipboard -Raw -Format Text -EA SilentlyContinue; if ($t -ne $null) { $t.Length } else { "none" }' | tail -1 | tr -d ' ')
    [ "${WCLIP_LEN:-none}" = "650000" ] && ok "最坏时刻场景成立：650KB 文本已到 Windows 剪贴板" \
        || warn "650KB 文本未确认到达 Windows 剪贴板（长度=${WCLIP_LEN:-?}）；这轮不是最坏时刻"
    kssh "printf 'OTILINK winfreeze done' | DISPLAY=:0 xclip -selection clipboard" >/dev/null 2>&1
fi

if [ "$FAIL" -gt 0 ]; then
    info "失败现场已保存：$ART/（trace csv / trace.out / hidlat.out）"
fi
summary "Windows 光标冻结（winfreeze）"
