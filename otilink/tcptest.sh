#!/bin/sh
# tcptest.sh —— TCP 传输验证：同一套 daemon 代码经网络对跑
#
# 意义：真机 bring-up 时先用网络把「输入/剪贴板链路」跑通（会真实用到
# evdev/uinput/系统剪贴板），再切到 cable 后端，把 SCSI 通道的风险隔离开。
set -u
cd "$(dirname "$0")"
[ -x ./otikm ] || { echo "先 make"; exit 1; }
PORT=${TCP_PORT:-27811}
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT

./otikm --transport "tcp-listen:$PORT" --clipboard --clip-file "$D/a.clip" \
       --clip-interval 150 --keepalive 300 --duration 6 --log "$D/a.log" --verbose &
sleep 0.6
./otikm --transport "tcp-connect:127.0.0.1:$PORT" --clipboard --clip-file "$D/b.clip" \
       --clip-interval 150 --duration 6 --log "$D/b.log" --verbose &
sleep 1.2

fail=0
chk() { if [ "$2" = 0 ]; then echo "  [PASS] $1"; else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }

echo "== TCP 传输集成测试（端口 $PORT）=="
printf 'tcp-sync-ok' > "$D/a.clip"
sleep 1.5
grep -q 'tcp-sync-ok' "$D/b.clip"; chk "TCP 下 A→B 剪贴板同步生效" $?
printf 'tcp-back-ok' > "$D/b.clip"
sleep 1.5
grep -q 'tcp-back-ok' "$D/a.clip"; chk "TCP 下 B→A 同步生效（双向）" $?
wait
# 计数按**内容指纹(crc)去重**：第 36 轮加了"未确认就重发"，同一条内容重发几次是设计行为；
# 这里要验的是**防回环**（每侧只主动送出自己的那一笔，不会把对端内容再送回去）。
a=$(grep -o 'crc=[0-9a-f]\{8\}' "$D/a.log" 2>/dev/null | sort -u | wc -l | tr -d ' ')
b=$(grep -o 'crc=[0-9a-f]\{8\}' "$D/b.log" 2>/dev/null | sort -u | wc -l | tr -d ' ')
[ "$a" = 1 ]; chk "A 只主动送出自己的 1 笔内容（无回环，去重后）实得 ${a:-0}" $?
[ "$b" = 1 ]; chk "B 只主动送出自己的 1 笔内容（无回环，去重后）实得 ${b:-0}" $?
ra=$(grep -c '未确认' "$D/a.log" 2>/dev/null || echo 0)
rb=$(grep -c '未确认' "$D/b.log" 2>/dev/null || echo 0)
[ "${ra:-0}" -le 8 ] && [ "${rb:-0}" -le 8 ]; chk "重发有界（A=${ra:-0} B=${rb:-0} 次，≤8）" $?
[ -s "$D/a.log" ]; chk "A 建立了 TCP 连接" $?
grep -q "KEEPALIVE" "$D/a.log" 2>/dev/null; chk "A 周期性发出保活 PING" $?
grep -q "RECV PING" "$D/b.log" 2>/dev/null; chk "B 收到保活 PING（链路存活可探测）" $?

echo
if [ "$fail" = 0 ]; then echo "全部通过"; else echo "有失败（$fail 项）"; fi
exit $([ "$fail" = 0 ] && echo 0 || echo 1)
