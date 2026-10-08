#!/bin/sh
# itest.sh —— 双实例集成测试：两个 otikm 经 AF_UNIX 对跑，验证真实 daemon 接线
#
# 角色模型（have_control ≡ 指针在本机）：
#   A 初始持有指针 → 推到右边缘 → 发 SWITCH(remote)，A 转为驱动侧并独占转发
#   B 收到 SWITCH(remote) → 指针在 B → B 本地消费（不转发），把收到的注入本地
#   驱动侧 A 按热键 → 发 SWITCH(local) 把指针拉回 → A 恢复本地消费
#
# 剧本 A: mouse 5000（撞右边缘）→ key30 按下/抬起 → sleep 400 → 热键 29 → key31
# 剧本 B: 只躺着收（sleep 2500）
set -u
cd "$(dirname "$0")"
BIN=./otikm
[ -x "$BIN" ] || { echo "先 make"; exit 1; }

D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT

cat > "$D/a.txt" <<'EOF'
mouse 5000 0 0 0
key 30 1
key 30 0
sleep 400
key 29 1
key 31 1
EOF

cat > "$D/b.txt" <<'EOF'
sleep 2500
EOF

"$BIN" --transport "listen:$D/s" --sim-input "$D/a.txt" --sim-inject "$D/a.out" \
       --screen 1920x1080 --edge 4 --hotkey 29 --duration 4 --delay 60 \
       --log "$D/a.log" --verbose &
PA=$!
sleep 0.4
"$BIN" --transport "connect:$D/s" --sim-input "$D/b.txt" --sim-inject "$D/b.out" \
       --hotkey 29 --duration 4 --delay 60 --log "$D/b.log" --verbose &
PB=$!
wait $PA; wait $PB

fail=0
chk() { if [ "$2" = 0 ]; then echo "  [PASS] $1"; else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }

echo "== 双实例集成测试 =="
echo "--- A 日志 ---"; sed 's/^/    /' "$D/a.log"
echo "--- B 日志 ---"; sed 's/^/    /' "$D/b.log"
echo "--- B 注入 ---"; sed 's/^/    /' "$D/b.out"
echo "--- A 注入 ---"; sed 's/^/    /' "$D/a.out"

grep -q "SENT SWITCH side=remote" "$D/a.log";  chk "A 撞右边缘发出 SWITCH(remote)" $?
grep -q "SENT key code=30" "$D/a.log";         chk "A 在驱动侧转发 key 30" $?
grep -q "key 30 1" "$D/b.out";                 chk "B 注入 key 30（A→B 转发生效）" $?
grep -q "RECV SWITCH side=remote" "$D/b.log";  chk "B 收到 SWITCH(remote)" $?
grep -q "指针在本机" "$D/b.log";               chk "B 状态：指针在本机" $?
grep -q "SENT SWITCH side=local" "$D/a.log";   chk "A 按热键发出 SWITCH(local) 拉回指针" $?
grep -q "指针拉回本机" "$D/a.log";             chk "A 状态迁移：恢复本地消费" $?
grep -q "SENT key code=31" "$D/a.log";         [ $? -ne 0 ]; chk "A 未转发 key 31（已恢复本地）" $?
grep -q "LOCAL key" "$D/a.log";                chk "A 把 key 31 当本地事件消费" $?
grep -q "key 31 1" "$D/b.out";                 [ $? -ne 0 ]; chk "B 未收到 key 31" $?
[ ! -s "$D/a.out" ];                           chk "A 注入文件为空（无需注入）" $?

echo
if [ "$fail" = 0 ]; then echo "全部通过"; else echo "有失败（$fail 项）"; fi
exit $([ "$fail" = 0 ] && echo 0 || echo 1)
