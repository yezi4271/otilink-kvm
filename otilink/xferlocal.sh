#!/bin/sh
# xferlocal.sh —— 大文件流式传输的**本机回归**（不走线缆、不需要 Windows）
#
# 原理：两个 otikm 用 AF_UNIX socketpair 对跑，剪贴板用 file 后端模拟。
#   A 的"剪贴板"文件里放一行 file://<大文件> → A 判定为大文件 → 走 otixfer 分片流式
#   B 收到后流式落盘到 /tmp/otilink-files-<pid>/<name>，并把 uri-list 写回自己的剪贴板文件
# 断言：B 收到的文件与源文件 **md5 一致**、字节数一致。
#
# 为什么要有它：真机回归（走线缆 + Windows 侧 PS 实现）一次几十秒到几分钟，
# 逻辑改动应当先在这里跑通 —— 这是"改一行等一分钟"和"改一行等一秒"的区别。
#
# 用法： ./xferlocal.sh [字节数]        缺省 32MB
set -u
cd "$(dirname "$0")"
BIN=${OTIKM:-./otikm}
[ -x "$BIN" ] || { echo "先 make otikm（或 OTIKM=./otikm_wsl）"; exit 1; }
SIZE=${1:-33554432}

D=$(mktemp -d /tmp/xferlocal.XXXXXX)
trap 'kill %1 %2 2>/dev/null; rm -rf "$D"' EXIT
SRC="$D/src.bin"
# 用可校验的伪随机内容（别用 /dev/zero：全零文件即使传坏也可能"看起来对"）
python3 -c "
import sys
n=$SIZE
b=bytearray(65536)
out=open('$SRC','wb')
seed=12345
w=0
while w<n:
    for i in range(len(b)):
        seed=(seed*1103515245+12345)&0x7fffffff
        b[i]=(seed>>16)&0xff
    k=min(len(b), n-w)
    out.write(b[:k]); w+=k
out.close()
"
SRC_MD5=$(md5sum < "$SRC" | awk '{print $1}')
echo "  源文件 $(wc -c < "$SRC") 字节  md5=$SRC_MD5"

printf 'file://%s\n' "$SRC" > "$D/a.clip"
: > "$D/b.clip"

"$BIN" --transport "listen:$D/s" --clipboard --clip-file "$D/a.clip" \
       --clip-interval 200 --duration 90 --log "$D/a.log" &
sleep 0.4
OTI_XFER_DROP_PART="${DROP_PART:-}" "$BIN" --transport "connect:$D/s" --clipboard \
       --clip-file "$D/b.clip" --clip-interval 200 --duration 90 --log "$D/b.log" &
sleep 0.5

# 等待接收完成（B 的日志里出现"接收完成"）
i=0
while [ $i -lt 120 ]; do
    grep -q "接收完成" "$D/b.log" 2>/dev/null && break
    sleep 0.5; i=$((i+1))
done

GOT=$(ls -t /tmp/otilink-files-*/$(basename "$SRC") 2>/dev/null | head -1)
# 顺带验证"双向同时传"不会互相拖死：两边剪贴板都放大文件
if [ "${BIDI:-0}" = "1" ]; then
    printf 'file://%s\n' "$SRC" > "$D/b.clip"
    echo "  （双向模式：两边剪贴板都是同一个大文件）"
fi

echo "=== A（发送端）日志 ==="; grep -E "大文件|分片|片|完成|warn" "$D/a.log" | tail -12
echo "=== B（接收端）日志 ==="; grep -E "大文件|接收|片|完成|warn" "$D/b.log" | tail -12

FAIL=0
if [ -n "$GOT" ]; then
    GOT_MD5=$(md5sum < "$GOT" | awk '{print $1}')
    GOT_SZ=$(wc -c < "$GOT")
    echo "  收到 $GOT（$GOT_SZ 字节，md5=$GOT_MD5）"
    [ "$GOT_MD5" = "$SRC_MD5" ] && echo "  [PASS] md5 一致" || { echo "  [FAIL] md5 不一致"; FAIL=1; }
    [ "$GOT_SZ" = "$SIZE" ] && echo "  [PASS] 字节数一致" || { echo "  [FAIL] 字节数 $GOT_SZ != $SIZE"; FAIL=1; }
else
    echo "  [FAIL] 没找到落盘文件"; FAIL=1
fi
# 收端应把 uri-list 写回自己的剪贴板文件（可粘贴）
grep -q "file://" "$D/b.clip" 2>/dev/null && echo "  [PASS] 收端剪贴板已设 uri-list" || { echo "  [FAIL] 收端剪贴板没设"; FAIL=1; }
# 防回环：A 不应把同一份文件反复发送
SENDS=$(grep -c "开始发送大文件" "$D/a.log" 2>/dev/null | head -1)
[ "${SENDS:-0}" -le 1 ] && echo "  [PASS] 发送端只发一次（无回环）" || { echo "  [FAIL] 发送了 $SENDS 次"; FAIL=1; }
B_RESEND=$(grep -c "开始发送大文件" "$D/b.log" 2>/dev/null | head -1)
[ "${B_RESEND:-0}" -eq 0 ] && echo "  [PASS] 收端没把收到的文件再发回去（防回环）" || { echo "  [FAIL] 收端回发了 $B_RESEND 次"; FAIL=1; }
if [ -n "${DROP_PART:-}" ]; then
    grep -q "缺 .* 片，要求从第" "$D/b.log" && echo "  [PASS] 丢片后要求断点重传" || { echo "  [FAIL] 丢片没触发重传"; FAIL=1; }
    grep -qE "按位图补发|要求从第 .* 片重发" "$D/a.log" && echo "  [PASS] 发送端按位图重发" || { echo "  [FAIL] 发送端没重发"; FAIL=1; }
fi

[ "$FAIL" -eq 0 ] && echo "★ 大文件流式传输本机回归通过" || echo "✗ 有失败项"
exit $FAIL
