#!/bin/bash
# lib.sh —— 门禁/验证脚本的共用底座（只被 source，不单独执行）
#
# 约定（所有 re/tools 下的脚本都必须遵守，历史脚本也按这个风格写）：
#   * 每一项用 ok/bad/warn 报告；结束时调 summary 打印汇总并返回退出码（FAIL>0 = 失败）
#   * 退出码 = 失败项数（0 = 全过），便于 CI/agent 判定
#   * **不合并 ssh 的 stderr**：麒麟登录横幅 "Kylin V10 SP1" 会污染捕获值
#     （历史上导致文本比对/md5 全错，见 NOTES §44）
#   * 抓 Windows 输出用 pwsh_*，统一去掉 \r
set -u

RE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)      # re/
OTI_DIR="$RE_DIR/otilink"
WIN_DIR="$RE_DIR/windows"

# 麒麟地址与密码**不内置**（仓库可公开）：只在真正要用 SSH 的地方（kssh）校验，
# 这样 --static / --local 这类不需要麒麟的门禁在没有环境变量时也能跑。
KY=${KY:-}
KY_PASS=${KY_PASS:-}
KT=${KT:-60}                          # 单条远程命令超时（秒）

PASS=0; FAIL=0; WARN=0; KNOWN=0
STAGE=""
declare -a STAGE_ROWS=()

if [ -t 1 ]; then G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; B=$'\033[1m'; N=$'\033[0m'
else G=; R=; Y=; B=; N=; fi

stage()  { STAGE="$1"; printf '\n%s===== %s =====%s\n' "$B" "$1" "$N"; }
ok()     { PASS=$((PASS+1)); printf '  %s[PASS]%s %s\n' "$G" "$N" "$*"; }
bad()    { FAIL=$((FAIL+1)); printf '  %s[FAIL]%s %s\n' "$R" "$N" "$*"; }
warn()   { WARN=$((WARN+1)); printf '  %s[WARN]%s %s\n' "$Y" "$N" "$*"; }
known()  { KNOWN=$((KNOWN+1)); printf '  %s[KNOWN]%s %s\n' "$Y" "$N" "$*"; }
info()   { printf '  .. %s\n' "$*"; }
die()    { printf '%s[FATAL]%s %s\n' "$R" "$N" "$*" >&2; exit 2; }

# 一个阶段的整体结果（用于 gate.sh 的汇总表）
stage_result() {                       # stage_result <名字> <失败数>
    local f=$1 n=$2
    if [ "$n" -eq 0 ]; then STAGE_ROWS+=("PASS|$f"); else STAGE_ROWS+=("FAIL|$f"); fi
}
stage_known() { STAGE_ROWS+=("KNOWN|$1"); }    # 已登记的已知失败（不计入 FAIL）

# 已知问题表：<slug> = 原因（见 tools/known-issues.txt）
is_known() {                           # is_known <slug> → 打印原因，返回 0
    local f="$RE_DIR/tools/known-issues.txt" line
    [ -f "$f" ] || return 1
    line=$(grep -E "^[[:space:]]*$1[[:space:]]*=" "$f" 2>/dev/null | head -1)
    [ -n "$line" ] || return 1
    printf '%s' "${line#*=}" | sed 's/^ *//'
    return 0
}

summary() {                            # summary <标题> [--json 路径]
    local title=${1:-结果}
    local json="${2:-${GATE_JSON:-/tmp/otilink-gate.json}}"
    printf '\n%s=== %s ===%s\n' "$B" "$title" "$N"
    local row st nm
    for row in "${STAGE_ROWS[@]:-}"; do
        [ -n "$row" ] || continue
        st=${row%%|*}; nm=${row#*|}
        case "$st" in
            PASS)  printf '  %s%s%s  %s\n' "$G" "$st" "$N" "$nm" ;;
            KNOWN) printf '  %s%s%s  %s（已登记，不计入失败）\n' "$Y" "$st" "$N" "$nm" ;;
            *)     printf '  %s%s%s  %s\n' "$R" "$st" "$N" "$nm" ;;
        esac
    done
    printf '  PASS=%s  FAIL=%s  KNOWN=%s  WARN=%s\n' "$PASS" "$FAIL" "$KNOWN" "$WARN"
    if [ "$FAIL" -eq 0 ]; then printf '  %s★ %s%s\n' "$G" "$title" "$N"
    else printf '  %s✗ 有失败项，见上%s\n' "$R" "$N"; fi
    # 机器可读汇总：agent 必须把这个文件路径贴进结论
    {
        printf '{"title":"%s","pass":%s,"fail":%s,"known":%s,"warn":%s,"stages":[' "$title" "$PASS" "$FAIL" "$KNOWN" "$WARN"
        local first=1
        for row in "${STAGE_ROWS[@]:-}"; do
            [ -n "$row" ] || continue
            [ "$first" -eq 1 ] || printf ','
            first=0
            printf '{"stage":"%s","result":"%s"}' "${row#*|}" "${row%%|*}"
        done
        printf ']}\n'
    } > "$json" 2>/dev/null && printf '  报告: %s\n' "$json"
    [ "$FAIL" -gt 255 ] && return 255
    return "$FAIL"
}

# ---------------------------------------------------------------- SSH（麒麟）
ASKPASS=$(mktemp); chmod 700 "$ASKPASS"
printf '#!/bin/sh\necho %s\n' "$KY_PASS" > "$ASKPASS"
trap 'rm -f "$ASKPASS"' EXIT

# 注意：**不要**给这条命令加 2>&1 —— 登录横幅在 stderr 上，合并进来就污染返回值
kssh() {
    [ -n "$KY" ] || die "请先 export KY=kylin@<麒麟IP>（或 tailscale 名，如 kylin-pc）"
    [ -n "$KY_PASS" ] || die "请先 export KY_PASS=<麒麟登录密码>（本仓库不内置密码）"
    SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force setsid -w timeout "$KT" \
        ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$KY" "$@" </dev/null 2>/dev/null
}

# 兼容只认 /tmp/kssh 的历史脚本（hwtest.sh 的 hidmon 段）
ensure_kssh() {
    [ -x /tmp/kssh ] && return 0
    { printf '#!/bin/sh\n'
      printf 'SSH_ASKPASS=%s SSH_ASKPASS_REQUIRE=force setsid -w timeout "${KT:-60}" \\\n' "$ASKPASS"
      printf '  ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 %s "$@" </dev/null\n' "$KY"
    } > /tmp/kssh
    chmod 700 /tmp/kssh
}

khave() { kssh "$1" >/dev/null 2>&1; }
kpos()  { kssh 'DISPLAY=:0 xdotool getmouselocation' | sed -n 's/^x:\([0-9-]*\) y:\([0-9-]*\).*/\1 \2/p' | head -1; }
kmove() { kssh "DISPLAY=:0 xdotool mousemove $1 $2" >/dev/null 2>&1 || true; }
kstatus() { kssh 'pgrep -x otikm | tr "\n" " "'; }

# ---------------------------------------------------------------- Windows 侧
pwsh_cmd()  { powershell.exe -NoProfile -Command "$1" 2>&1 | tr -d '\r'; }
pwsh_file() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$1")" "${@:2}" 2>&1 | tr -d '\r'; }
winlog()    { pwsh_cmd 'Get-Content C:\Users\Public\km.log -Tail 200 -EA SilentlyContinue' 2>/dev/null; }
winclip()   { pwsh_cmd 'Get-Content C:\Users\Public\clip.log -Tail 200 -EA SilentlyContinue' 2>/dev/null; }

# Windows 光标坐标（GetCursorPos，P/Invoke —— WinForms 的 Cursor.Position 受 DPI 虚拟化影响，别用）
wcur() {
    pwsh_cmd 'Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class GP { [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p); }
"@; $p = New-Object GP+P; [void][GP]::GetCursorPos([ref]$p); "$($p.x),$($p.y)"' | tail -1
}
wcur_x() { wcur | cut -d, -f1; }

# 我们的 Windows 进程清单 / 厂商进程清单（"同时只能有一个实现占用线缆"是硬约束）
count_proc()  { pwsh_cmd "((Get-Process $1 -EA SilentlyContinue | Measure-Object).Count)" | tail -1 | tr -d ' '; }
count_vendor(){ pwsh_cmd "((Get-Process LEWD,LinkEngKM,MacKMLink,SKLoader -EA SilentlyContinue | Measure-Object).Count)" | tail -1 | tr -d ' '; }
# 注意排除 $PID：这条查询自己的命令行里就含 'otiagent.ps1'（会把查询自身算进去）
count_clip()  { pwsh_cmd "(Get-CimInstance Win32_Process | Where-Object { \$_.ProcessId -ne \$PID -and \$_.CommandLine -match '-File\s+\"?[^\"]*otiagent\.ps1' } | Measure-Object).Count" | tail -1 | tr -d ' '; }

# 解析回归脚本输出里的 PASS=/FAIL=
parse_pf() { sed -n 's/.*PASS=\([0-9]*\).*FAIL=\([0-9]*\).*/\1 \2/p' "$1" | tail -1; }
