#!/bin/bash
# xferreg.sh —— 大文件流式传输**真机**回归（双向，md5 校验 + 吞吐 + 键鼠不受影响）
#
# 覆盖的是 otixfer 那条路（>8MB 的文件自动走分片流式，收端边收边落盘）：
#   ① Windows → 麒麟   md5 一致 + 麒麟剪贴板已设 uri-list
#   ② 麒麟 → Windows   md5 一致 + Windows 剪贴板已设 CF_HDROP
#   ③ 日志不变量：无真正帧解码失败、无回环
#   ④ 传输期间键鼠不卡（用 HID 包推 Windows 光标，检查位移是否正常）
#
# 用法： ./xferreg.sh            # 缺省 64MB（约 30 秒）
#        ./xferreg.sh 524288000  # 500MB（约 4 分钟）
#        ./xferreg.sh 0          # 只查环境
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
WIN=$(cd "$HERE/../windows" && pwd)
KY=${KY:-}
KY_PASS=${KY_PASS:-}
SIZE=${1:-67108864}
AGENT_LOG=/mnt/c/Users/Public/clip.log
PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }

ASKPASS=$(mktemp); chmod 700 "$ASKPASS"; printf '#!/bin/sh\necho %s\n' "$KY_PASS" > "$ASKPASS"
kssh() { SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force setsid -w timeout "${KT:-120}" \
         ssh -o ConnectTimeout=8 -o NumberOfPasswordPrompts=1 -o StrictHostKeyChecking=no "$KY" "$@"; }
trap 'rm -f "$ASKPASS"' EXIT
pwsh_cmd() { powershell.exe -NoProfile -Command "$1" 2>&1 | tr -d '\r'; }
pwsh_file() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$1")" "${@:2}" 2>&1 | tr -d '\r'; }
newlines_log() { wc -l < "$AGENT_LOG" 2>/dev/null | tr -d ' '; }
agent_since() { tail -n +"$(( ${1:-0} + 1 ))" "$AGENT_LOG" 2>/dev/null | tr -d '\r'; }

echo "=== 0/4 环境 ==="
q_count() { pwsh_cmd "(Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { \$_.ProcessId -ne \$PID -and \$_.CommandLine -like '*-File*otiagent.ps1*' } | Measure-Object).Count" | tail -1; }
[ "$(q_count)" -ge 1 ] && ok "剪贴板代理运行中" || bad "剪贴板代理没跑（re/windows/deploy.sh --restart）"
kssh 'pgrep -x otikm >/dev/null && echo yes' | grep -q yes && ok "麒麟 otikm 运行中" || bad "麒麟 otikm 未运行"
kssh 'grep -q "大文件传输: 已启用" /tmp/otikm.log && echo yes' | grep -q yes && ok "麒麟侧大文件传输已启用" || bad "麒麟 otikm 版本太旧（没有大文件传输）"
grep -q "大文件流式发送" "$AGENT_LOG" 2>/dev/null && ok "Windows 侧大文件传输已启用" || echo "  ... Windows 侧还没跑过大文件（继续测即可）"

if [ "${SIZE}" = "0" ]; then echo; echo "PASS=$PASS FAIL=$FAIL"; exit $((FAIL>0)); fi
echo "  测试大小：$SIZE 字节（$(( SIZE / 1048576 )) MB）"

LOG0=$(newlines_log)

# ---------------- ① Windows → 麒麟 ----------------
echo "=== 1/4 Windows → 麒麟 ==="
pwsh_cmd "
\$f='C:\Users\Public\otitest\xferreg.bin'
\$buf=New-Object byte[] 4194304
(New-Object System.Random 4242).NextBytes(\$buf)
\$fs=[IO.File]::Create(\$f)
\$w=0; while (\$w -lt $SIZE) { \$n=[Math]::Min(\$buf.Length, $SIZE - \$w); \$fs.Write(\$buf,0,\$n); \$w+=\$n }
\$fs.Close()
(Get-FileHash \$f -Algorithm MD5).Hash" > /tmp/xferreg_w2l_expected.txt
EXP=$(tail -1 /tmp/xferreg_w2l_expected.txt | tr 'A-Z' 'a-z')
echo "  源 md5=$EXP"
T0=$(date +%s)
pwsh_cmd "Set-Clipboard -Path 'C:\Users\Public\otitest\xferreg.bin'" >/dev/null
for i in $(seq 1 $(( SIZE / 4194304 + 90 ))); do
    sleep 2
    agent_since "$LOG0" | grep -qE "本批大文件全部完成|中止" && break
done
T=$(( $(date +%s) - T0 ))
agent_since "$LOG0" | grep -E "xfer" | tail -4 | sed 's/^/    /'
if agent_since "$LOG0" | grep -q "中止"; then bad "传输中止"; else
    GOT=$(kssh "f=\$(ls -t /tmp/otilink-files-*/xferreg.bin 2>/dev/null | head -1); [ -n \"\$f\" ] && md5sum < \"\$f\" | cut -d' ' -f1" | tail -1)
    [ "$EXP" = "$GOT" ] && ok "W→L md5 一致（${T} 秒，$(( SIZE / 1048576 / (T>0?T:1) )) MB/s 量级）" || bad "W→L md5 不一致：期望 $EXP 实收 $GOT"
    kssh 'DISPLAY=:0 xclip -selection clipboard -t text/uri-list -o 2>/dev/null' | grep -q "xferreg.bin" && ok "麒麟剪贴板已设 uri-list" || bad "麒麟剪贴板没设 uri-list"
fi

# ---------------- ② 麒麟 → Windows ----------------
echo "=== 2/4 麒麟 → Windows ==="
kssh "head -c $SIZE /dev/urandom > /tmp/xferreg_k.bin; printf 'file:///tmp/xferreg_k.bin\n' > /tmp/xferreg_k.uri; md5sum < /tmp/xferreg_k.bin" > /tmp/xferreg_l2w_expected.txt
EXP=$(grep -oE '^[0-9a-f]{32}' /tmp/xferreg_l2w_expected.txt | head -1)
LOG1=$(newlines_log)
T0=$(date +%s)
kssh "cd ~/otilink && ./setclip.sh /tmp/xferreg_k.uri text/uri-list" >/dev/null
for i in $(seq 1 $(( SIZE / 4194304 + 90 ))); do
    sleep 2
    agent_since "$LOG1" | grep -qE "接收完成|中止" && break
done
T=$(( $(date +%s) - T0 ))
agent_since "$LOG1" | grep -E "xfer" | tail -4 | sed 's/^/    /'
GOT=$(pwsh_cmd "
\$d = Get-ChildItem \$env:TEMP -Directory -Filter 'otilink_files_*' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (\$d) { \$f = Join-Path \$d.FullName 'xferreg_k.bin'; if (Test-Path \$f) { (Get-FileHash \$f -Algorithm MD5).Hash } else { 'nofile' } } else { 'nodir' }" | tail -1 | tr 'A-Z' 'a-z')
[ "$EXP" = "$GOT" ] && ok "L→W md5 一致（${T} 秒）" || bad "L→W md5 不一致：期望 $EXP 实收 $GOT"
pwsh_cmd "((Get-Clipboard -Format FileDropList) -join ';')" | grep -q "xferreg_k.bin" && ok "Windows 剪贴板已设文件拖放" || bad "Windows 剪贴板没设文件拖放"

# ---------------- ③ 传输期间键鼠 ----------------
echo "=== 3/4 传输期间键鼠不卡（HID 推光标）==="
LOG2=$(newlines_log)
pwsh_cmd "Set-Clipboard -Path 'C:\Users\Public\otitest\xferreg.bin'" >/dev/null   # 再传一次，边传边测
# 先发 F24 令牌把控制权强制拉回 Windows（hwtest.sh 同款前置动作）：
# 否则可能停在 REMOTE，HID 包推不动 Windows 光标 → 假失败（实测踩过）
kssh 'cd ~/otilink && ./otiprobe hidsend 2 000073000000000000000000 >/dev/null 2> /dev/sg3 >/dev/null 2>&11; sleep 0.2; ./otiprobe hidsend 2 000000000000000000000000 >/dev/null 2> /dev/sg3 >/dev/null 2>&11' >/dev/null
sleep 1
for i in $(seq 1 30); do sleep 2; agent_since "$LOG2" | grep -q "片（" && break; done
RUNNING=$(agent_since "$LOG2" | grep -c "片（")
pwsh_file "$WIN/setcur.ps1" -X 1200 -Y 500 >/dev/null; sleep 1
B=$(pwsh_file "$WIN/curlog.ps1" -Secs 1 | tail -1 | awk '{print $3}')
kssh 'cd ~/otilink && for i in 1 2 3; do ./otiprobe hidsend 1 006400000000000000000000 >/dev/null 2> /dev/sg3 >/dev/null 2>&11; sleep 0.2; done' >/dev/null
A=$(pwsh_file "$WIN/curlog.ps1" -Secs 1 | tail -1 | awk '{print $3}')
D=$(( ${A:-0} - ${B:-0} ))
if [ "${RUNNING:-0}" -lt 1 ]; then
    echo "  ... 传输没跑起来（剪贴板没变），本条仅供参考"
fi
[ "$D" -gt 100 ] && ok "传输中光标仍正常移动 Δ=${D}px（传输中采样 $RUNNING 次）" || bad "传输中光标几乎不动（Δ=$D）—— 键鼠被大文件拖住了"

# ---------------- ④ 日志不变量 ----------------
echo "=== 4/4 日志不变量 ==="
NEW=$(agent_since "$LOG0")
echo "$NEW" | grep -q "解包失败" && { bad "出现真正的帧解码失败"; echo "$NEW" | grep "解包失败" | head -2; } || ok "无真正的帧解码失败"
# OpenClipboard 会被别的进程（比如我们自己的测试脚本 Set-Clipboard）**瞬时**占住 →
# 偶发一两次是正常的（代码会跳过本轮、300ms 后重试）。持续出现才是问题。
NCLIP=$(echo "$NEW" | grep -c "有文件却读不出来")
[ "${NCLIP:-0}" -le 5 ] && ok "文件剪贴板读取正常（瞬时失败 ${NCLIP} 次，属预期）" || bad "文件剪贴板读取异常（$NCLIP 次）"
N=$(echo "$NEW" | grep -c "大文件流式发送")
[ "${N:-0}" -le 4 ] && ok "大文件发送 $N 次（无回环风暴）" || bad "大文件发送 $N 次，疑似回环"

echo; echo "=== 结果 ==="
echo "  PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "  ★ 大文件流式传输（双向）通过" || echo "  ✗ 有失败项，见上"
exit $((FAIL>0))
