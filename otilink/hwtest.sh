#!/bin/bash
# hwtest.sh —— 键鼠共享**真机回归自测**（不需要人动手）
#
# 为什么要有它：这套系统跨三条链路（Windows 钩子 / 麒麟 X11 光标 / 线缆 HID），
#   靠"人肉点一下看看"回归太慢也容易漏。本脚本用**程序化输入 + 真实光标坐标**
#   把两个方向都验一遍，每一步都给出可判定的 PASS/FAIL。
#
# 依赖：WSL 侧能跑 powershell.exe；能 SSH 到麒麟；麒麟有 xdotool。
# 用法：  ./hwtest.sh          跑完整往返
#         ./hwtest.sh check    只查两侧进程/设备
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
WIN=$(cd "$HERE/../windows" && pwd)
KYLIN=${KYLIN:-${KY:-}}
AGENT_LOG=${AGENT_LOG:-C:\\Users\\Public\\agRT.log}
PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }

# SSH：自带 askpass（历史版本依赖 /tmp/kssh，那个文件一丢整个回归就废）
KY_PASS=${KY_PASS:-}
ASKPASS_HW=$(mktemp); chmod 700 "$ASKPASS_HW"; printf '#!/bin/sh\necho %s\n' "$KY_PASS" > "$ASKPASS_HW"
trap 'rm -f "$ASKPASS_HW"' EXIT

# wslpath 转换路径：直接写反斜杠会被 bash 吃掉
pwsh_file() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$1")" "${@:2}" 2>&1 | tr -d '\r'; }
pwsh_cmd()  { powershell.exe -NoProfile -Command "$1" 2>&1 | tr -d '\r'; }
kssh() { SSH_ASKPASS="$ASKPASS_HW" SSH_ASKPASS_REQUIRE=force setsid -w timeout "${KT:-60}" \
         ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$KYLIN" "$@" </dev/null 2>/dev/null; }
kpos() { kssh 'DISPLAY=:0 xdotool getmouselocation' | sed -n 's/^x:\([0-9-]*\) y:\([0-9-]*\).*/\1 \2/p' | head -1; }
kx()   { echo "${1%% *}"; }
kmove(){ kssh "DISPLAY=:0 xdotool mousemove $1 $2" >/dev/null; }
# 用 GetCursorPos（curlog.ps1）读，可靠；WinForms 的 Cursor.Position 受 DPI 虚拟化影响
winx() { pwsh_file "$WIN/curlog.ps1" -Secs 1 | tail -1 | awk '{print $3}'; }

echo "=== 0/4 环境 ==="
if [ -n "$(pwsh_cmd '(Get-Process otiagent2 -EA SilentlyContinue).Id' | tr -d '\r' | head -1)" ]; then
    ok "Windows 代理运行中"
else
    bad "Windows 代理未运行 —— 先执行: otiagent2.exe --edge left"
fi
[ -n "$(kssh 'pgrep -x otikm')" ] && ok "麒麟 otikm 运行中" || bad "麒麟 otikm 未运行 —— 先执行: run-kylin.sh"
P0=$(kpos); [ -n "${P0:-}" ] && ok "麒麟真实光标可读: $P0" || bad "读不到麒麟光标（xdotool / DISPLAY）"

# 上一次（可能被超时打断的）运行可能把代理留在 REMOTE，先发 F24 令牌让它回 LOCAL
kssh 'cd ~/otilink && ./otiprobe hidsend 2 000073000000000000000000 >/dev/null 2> /dev/sg3 >/dev/null 2>&11; sleep 0.2; ./otiprobe hidsend 2 000000000000000000000000 >/dev/null 2> /dev/sg3 >/dev/null 2>&11' >/dev/null
sleep 1

if [ "${1:-run}" = "check" ]; then
    echo; echo "检查完毕：PASS=$PASS FAIL=$FAIL"; exit $((FAIL>0))
fi

echo "=== 1/4 麒麟 → Windows（发 HID 包，看 Windows 光标）==="
# 先把 Windows 光标放到中间：它可能停在上一次测试留下的屏幕边缘，那样 +100 推不动
pwsh_file "$WIN/setcur.ps1" -X 1200 -Y 500 >/dev/null
sleep 1
B=$(winx)
BX=$(echo "$B" | tr -d ' ')
if [ "${BX:-0}" -gt 2000 ]; then   # 归位没生效（还贴在右边缘）→ 重试一次，否则 +100 推不动会造成假失败
    sleep 1; pwsh_file "$WIN/setcur.ps1" -X 1200 -Y 500 >/dev/null; sleep 1; B=$(winx)
fi
kssh 'cd ~/otilink && for i in 1 2 3; do ./otiprobe hidsend 1 006400000000000000000000 >/dev/null 2> /dev/sg3 >/dev/null 2>&11; sleep 0.2; done' >/dev/null
sleep 1
A=$(winx)
D=$(( ${A:-0} - ${B:-0} ))
[ "$D" -gt 20 ] && ok "Windows 光标右移 ${D}px（HID 通道通）" || bad "Windows 光标没动（Δ=$D）"

echo "=== 2/4 Windows → 麒麟 交接（Linux 在 Windows 右边 → 推 Windows 光标到**右**边缘）==="
kmove 300 500
# 第 1 步刚发过 HID 包，代理会因此抑制交接 1.5s（防"对端驱动时被自己误判"）；等它过期
sleep 2
pwsh_file "$WIN/pushright.ps1" | grep -q "restored" && ok "已把 Windows 光标推到右边缘" || bad "推边缘脚本失败"
sleep 2
P1=$(kpos); DX=$(( $(kx "$P1") - 300 ))
[ "$DX" -gt 20 ] && ok "麒麟光标跟着右移 ${DX}px（交接成功）" || bad "麒麟光标没跟动（Δ=$DX —— 代理可能没进 REMOTE）"

echo "=== 2b/4 按键转发（REMOTE 下点击 → 麒麟应收到 BTN_LEFT）==="
# 按键走低级钩子、位移走原始输入；这里必须真点一次才能覆盖按键路径
# 注意：这里**不能**用 kssh（它内部套了 grep 管道，grep 非 TTY 时会缓冲输出，
# 5 秒后日志文件里还是空的 → 假失败）。直接调 ssh 让 stdout 落到文件。
( KT=45 kssh "cd ~/otilink && sg input -c './hidmon /dev/input/by-id/usb-_Android+Mac_*-if01-event-mouse 9'" > /tmp/hwtest_btn.log 2>&1 ) &
for i in $(seq 1 25); do grep -q "hidmon: 监听" /tmp/hwtest_btn.log 2>/dev/null && break; sleep 0.4; done
grep -q "hidmon: 监听" /tmp/hwtest_btn.log || bad "hidmon 没起来（按键检查不可信）"
# 点击必须发生在 REMOTE 下：先确认代理确实在 REMOTE，否则补推一次边缘
MODE=$(pwsh_cmd "(Get-Content '$AGENT_LOG' -EA SilentlyContinue | Select-String 'REMOTE|LOCAL' | Select-Object -Last 1).Line" | head -1)
case "$MODE" in
    *REMOTE*) : ;;
    *) pwsh_file "$WIN/pushright.ps1" >/dev/null 2>&1; sleep 2 ;;
esac
pwsh_file "$WIN/click.ps1" >/dev/null
sleep 5
grep -q "BTN_LEFT * = 1" /tmp/hwtest_btn.log && ok "左键按下已送达麒麟（按键路径通）" || bad "麒麟没收到按键（检查代理的钩子按键转发）"
grep -q "BTN_RIGHT * = 1" /tmp/hwtest_btn.log && ok "右键按下已送达麒麟" || bad "右键没送达"

echo "=== 2c/4 剪贴板（两方向）==="
# 走协议管道（otikm 侧 + otiagent.ps1 的 -Clipboard），与键鼠的 HID 通道互不干扰
pwsh_cmd "Set-Clipboard -Value 'OTITEST-W2L-42'" >/dev/null
sleep 4
kclip=$(kssh 'DISPLAY=:0 xclip -selection clipboard -o' | head -1)
[ "$kclip" = "OTITEST-W2L-42" ] && ok "Windows → 麒麟 剪贴板 OK" || bad "Windows→麒麟 剪贴板失败（收到 '$kclip'）"
kssh 'printf "OTITEST-L2W-99" | DISPLAY=:0 xclip -selection clipboard' >/dev/null
sleep 4
wclip=$(pwsh_cmd 'Get-Clipboard -Raw' | head -1 | tr -d '\r')
[ "$wclip" = "OTITEST-L2W-99" ] && ok "麒麟 → Windows 剪贴板 OK" || bad "麒麟→Windows 剪贴板失败（收到 '$wclip'）"

echo "=== 3/4 麒麟 → Windows 交还（Windows 在 Linux 左边 → 把麒麟光标推到**左**边缘）==="
kmove 1600 500
pwsh_file "$WIN/hidpkt.ps1" -Payload "008100000000000000000000" -Type 1 -Count 12 -GapMs 100 >/dev/null
sleep 3
kssh 'tail -40 /tmp/otikm.log' | grep -q "撞边(" && ok "otikm 检测到撞边并发出交还" || bad "otikm 没检测到撞边"
M=$(pwsh_cmd "(Get-Content '$AGENT_LOG' -EA SilentlyContinue | Select-String 'LOCAL' | Select-Object -Last 1).Line" | head -1)
[ -n "${M:-}" ] && ok "Windows 代理已回到 LOCAL（$M）" || bad "代理没有回到 LOCAL"

echo "=== 4/4 结果 ==="
echo "  PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "  ★ 双向键鼠共享真机自测全部通过" || echo "  ✗ 有失败项，见上"
exit $((FAIL>0))
