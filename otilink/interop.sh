#!/bin/sh
# interop.sh —— 跨实现运行时互通测试：Linux otikm ↔ Windows agent（经 TCP，仅测试用）
#
# 验证内容（剪贴板两个方向）：
#   1) Linux 剪贴板文件变化 -> 守护进程发 CLIP -> agent 应用 -> Windows 剪贴板出现该内容
#   2) Windows 剪贴板变化  -> agent 发 CLIP  -> 守护进程应用 -> Linux 剪贴板文件出现该内容
#
# 注意（踩过的坑）：Windows 剪贴板内容由**拥有它的进程**维持，
# 所以设置剪贴板必须用一个"存活若干秒"的进程（Set-Clipboard; Start-Sleep）。
set -u
cd "$(dirname "$0")"
[ -x ./otikm ] || { echo "先 make"; exit 1; }
command -v powershell.exe >/dev/null 2>&1 || { echo "需要 powershell.exe 互操作"; exit 1; }

AGENT=$(wslpath -w ../windows/otiagent.ps1)
PORT=${INTEROP_PORT:-29350}
D=$(mktemp -d); trap 'rm -rf "$D"; kill $DPID 2>/dev/null' EXIT
printf 'linux-seed' > "$D/a.clip"

./otikm --transport "tcp-listen:$PORT" --clipboard --clip-file "$D/a.clip" \
        --clip-interval 200 --duration 26 --log "$D/a.log" --verbose >/dev/null 2>&1 &
DPID=$!
sleep 1.5
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$AGENT" -Tcp "127.0.0.1:$PORT" \
        -Clipboard -DurationSec 22 > "$D/agent.log" 2>&1 &
APID=$!
sleep 4

printf 'linux-to-windows-OK' > "$D/a.clip"
sleep 3
WIN=$(powershell.exe -NoProfile -Command "Get-Clipboard -Raw" 2>/dev/null | tr -d '\r' | head -1)

powershell.exe -NoProfile -Command "Start-Sleep -Seconds 2; Set-Clipboard -Value 'windows-to-linux-OK'; Start-Sleep -Seconds 14" >/dev/null 2>&1 &
sleep 9
LIN=$(cat "$D/a.clip")

# 阶段3：大载荷分块/重组（140000 字节 → 3 块，跨实现重组）
python3 -c "import sys; sys.stdout.write(''.join(chr(65+(i%26)) for i in range(140000)))" > "$D/a.clip"
sleep 8
BIGLEN=$(powershell.exe -NoProfile -Command "(Get-Clipboard -Raw).Length" 2>/dev/null | tr -d '\r' | head -1)
BIGMD5=$(powershell.exe -NoProfile -Command "\$t=Get-Clipboard -Raw; if (\$t) { ([BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes(\$t))) -replace '-','').ToLower() }" 2>/dev/null | tr -d '\r' | head -1)
LINMD5=$(python3 -c "import hashlib;print(hashlib.md5(open('$D/a.clip','rb').read()).hexdigest())")

wait $APID 2>/dev/null
kill $DPID 2>/dev/null

fail=0
chk() { if [ "$2" = 0 ]; then echo "  [PASS] $1"; else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }
echo "== 跨实现运行时互通（TCP 测试通道）=="
[ "$WIN" = "linux-to-windows-OK" ]; chk "方向1 Linux→Windows 剪贴板（实得 '$WIN'）" $?
[ "$LIN" = "windows-to-linux-OK" ]; chk "方向2 Windows→Linux 剪贴板（实得 '$LIN'）" $?
grep -q "CLIP 发送" "$D/agent.log"; chk "agent 确实发出了 CLIP" $?
grep -q "CLIP 应用远端剪贴板" "$D/a.log"; chk "守护进程确实应用了远端 CLIP" $?
[ "$BIGLEN" = "140000" ]; chk "大载荷（140000 字节）完整到达 Windows 剪贴板（实得 '$BIGLEN'）" $?
[ -n "$BIGMD5" ] && [ "$BIGMD5" = "$LINMD5" ]; chk "大载荷内容 MD5 一致（$BIGMD5）" $?
echo
if [ "$fail" = 0 ]; then echo "全部通过"; else echo "有失败（$fail 项）"; fi
exit $([ "$fail" = 0 ] && echo 0 || echo 1)
