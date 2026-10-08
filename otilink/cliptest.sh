#!/bin/sh
# cliptest.sh —— 剪贴板同步集成测试（两个真实 daemon 经 AF_UNIX 对跑）
#
# 用文件模拟剪贴板，依次验证：
#   1) A→B 同步
#   2) B→A 同步（双向）
#   3) 防回环：收到远端内容并应用后，不得再回发（各自只主动发送自己产生的内容）
#   4) 超大内容（140000 字节 = 3 块）跨块重组后逐字节一致
set -u
cd "$(dirname "$0")"
[ -x ./otikm ] || { echo "先 make"; exit 1; }

D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
BIG="$D/big.bin"
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*7+3)%256 for i in range(140000)))" > "$BIG"

./otikm --transport "listen:$D/s" --clipboard --clip-file "$D/a.clip" \
       --clip-interval 150 --duration 9 --log "$D/a.log" --verbose &
sleep 0.4
./otikm --transport "connect:$D/s" --clipboard --clip-file "$D/b.clip" \
       --clip-interval 150 --duration 9 --log "$D/b.log" --verbose &
fail=0
chk() { if [ "$2" = 0 ]; then echo "  [PASS] $1"; else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }
# 注意：断言必须"当场"做——测试自己会覆盖剪贴板文件，事后再查会查到后来的内容
step() { sleep "$1"; }

echo "== 剪贴板集成测试 =="
sleep 1.2
printf 'hello-otilink' > "$D/a.clip"          # A 侧产生内容
sleep 1.2
grep -q 'hello-otilink' "$D/b.clip"; chk "A→B 同步生效（B 剪贴板 == A 内容）" $?
printf 'world-otilink' > "$D/b.clip"          # B 侧产生内容
sleep 1.2
grep -q 'world-otilink' "$D/a.clip"; chk "B→A 同步生效（双向）" $?
cp "$BIG" "$D/b.clip"                         # B 侧产生大内容
sleep 2.5
cmp -s "$BIG" "$D/a.clip";           chk "140000 字节大内容跨块重组后逐字节一致" $?
wait

echo "--- A 日志（CLIP 相关）---"; grep CLIP "$D/a.log" | sed 's/^/    /'
echo "--- B 日志（CLIP 相关）---"; grep CLIP "$D/b.log" | sed 's/^/    /' 
asend=$(grep -c 'CLIP 发送' "$D/a.log"); bsend=$(grep -c 'CLIP 发送' "$D/b.log")
[ "$asend" = 1 ]; chk "A 只主动发送过 1 次（收到即应用、不回发）实得 $asend" $?
[ "$bsend" = 2 ]; chk "B 只主动发送过 2 次（world + 大对象，未回发 hello）实得 $bsend" $?

echo
if [ "$fail" = 0 ]; then echo "全部通过"; else echo "有失败（$fail 项）"; fi
exit $([ "$fail" = 0 ] && echo 0 || echo 1)
