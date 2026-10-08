#!/bin/bash
# clipreg.sh —— 剪贴板**真机**回归（文本 / 大文本 / 图片 / 文件，双向）
#
# 与 cliptest.sh 的分工：
#   cliptest.sh  本机模拟（文件当剪贴板、AF_UNIX 传输、两个 otikm 对跑）—— 验**协议与重组**
#   clipreg.sh   真机（Windows 剪贴板 ↔ 麒麟 X11 剪贴板，走对拷线）—— 验**端到端真实行为**
#
# 为什么单独一个脚本：hwtest.sh 覆盖键鼠（约 90 秒，要动真实光标），
# 剪贴板是**另一条链路**（协议帧管道 + otiagent.ps1 -Clipboard），排查时只需要这一条，
# 跑得快、不动光标、可反复跑（约 50 秒）。
#
# 依赖：WSL 侧能跑 powershell.exe；SSH 到麒麟（密码由环境变量 KY_PASS 提供，见 re/AGENTS.md §1）；
#       麒麟侧 xclip；Windows 侧剪贴板代理在跑（不在会自动 deploy.sh --restart）。
#
# 用法： ./clipreg.sh          正常跑
#        ./clipreg.sh check    只查环境
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
WIN=$(cd "$HERE/../windows" && pwd)
KY=${KY:-}
KY_PASS=${KY_PASS:-}
AGENT_LOG=/mnt/c/Users/Public/clip.log      # = Windows 侧 C:\Users\Public\clip.log
TESTIMG=$HERE/testimg.png
PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }

# ---- SSH：自带 askpass，不依赖 /tmp 里的历史遗留文件 ----
# 注意：**不要**把 stderr 并进 stdout。麒麟的登录 banner（"Kylin V10 SP1"）走 stderr，
# 一并进来会污染所有捕获（文本多 14 字节、二进制前面多一行 → 判定全错，实测踩过）。
ASKPASS=$(mktemp); chmod 700 "$ASKPASS"; printf '#!/bin/sh\necho %s\n' "$KY_PASS" > "$ASKPASS"
kssh() { SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force setsid -w timeout "${KT:-60}" \
         ssh -o ConnectTimeout=8 -o NumberOfPasswordPrompts=1 -o StrictHostKeyChecking=no "$KY" "$@"; }
trap 'rm -f "$ASKPASS"' EXIT

pwsh_file() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$1")" "${@:2}" 2>&1 | tr -d '\r'; }
pwsh_cmd()  { powershell.exe -NoProfile -Command "$1" 2>&1 | tr -d '\r'; }
kclip_text(){ kssh "DISPLAY=:0 xclip -selection clipboard -o 2>/dev/null"; }
# 麒麟侧"放内容进剪贴板"：必须脱离 ssh 通道，否则持有选区的 xclip 会让 ssh 挂到超时
kset_file() { kssh "cd ~/otilink && ./setclip.sh '$1' ${2:-}" >/dev/null; }
newlines_log() { wc -l < "$AGENT_LOG" 2>/dev/null | tr -d ' '; }
agent_log_since() { tail -n +"$(( ${1:-0} + 1 ))" "$AGENT_LOG" 2>/dev/null | tr -d '\r'; }
png_dim() { python3 - "$1" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
print('none' if len(d) < 24 or d[:4] != b'\x89PNG' else '%dx%d' % struct.unpack('>II', d[16:24]))
EOF
}

echo "=== 0/9 环境 ==="
# 注意 -ne $PID：这条查询自身的命令行里就含 '-File*otiagent.ps1*'，不排除自己会数到自己
q_count() { pwsh_cmd "(Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { \$_.ProcessId -ne \$PID -and \$_.CommandLine -like '*-File*otiagent.ps1*' } | Measure-Object).Count" | tail -1; }
RUNNING=$(q_count)
if [ "${RUNNING:-0}" -ge 1 ]; then
    ok "Windows 剪贴板代理运行中（$RUNNING 个）"
else
    echo "  ... 代理没在跑，用 deploy.sh --restart 拉起"
    bash "$WIN/deploy.sh" --restart >/dev/null 2>&1; sleep 4
    RUNNING=$(q_count)
    [ "${RUNNING:-0}" -ge 1 ] && ok "已拉起剪贴板代理" || bad "剪贴板代理起不来（看 C:\\Users\\Public\\clip.log）"
fi
[ -f "$AGENT_LOG" ] && ok "代理日志可读" || bad "读不到代理日志 $AGENT_LOG"
kssh 'pgrep -x otikm >/dev/null && echo yes' | grep -q yes && ok "麒麟 otikm 运行中" || bad "麒麟 otikm 未运行"
kssh 'DISPLAY=:0 xclip -selection clipboard -o >/dev/null 2>&1; echo yes' | grep -q yes && ok "麒麟 xclip 可用" || bad "麒麟 xclip 不可用"
[ -f "$TESTIMG" ] && ok "测试图片在（$(basename "$TESTIMG")）" || bad "缺 $TESTIMG"

if [ "${1:-run}" = "check" ]; then
    echo; echo "检查完毕：PASS=$PASS FAIL=$FAIL"; exit $((FAIL>0))
fi

LOG0=$(newlines_log)

echo "=== 1/9 小文本 Windows → 麒麟 ==="
pwsh_cmd "Set-Clipboard -Value 'OTITEST-W2L-42'" >/dev/null; sleep 4
V=$(kclip_text | head -1)
[ "$V" = "OTITEST-W2L-42" ] && ok "收到 '$V'" || bad "收到 '$V'（期望 OTITEST-W2L-42）"

echo "=== 2/9 小文本 麒麟 → Windows ==="
kssh 'printf "OTITEST-L2W-99" | DISPLAY=:0 xclip -selection clipboard' >/dev/null; sleep 4
V=$(pwsh_cmd 'Get-Clipboard -Raw' | head -1)
[ "$V" = "OTITEST-L2W-99" ] && ok "收到 '$V'" || bad "收到 '$V'（期望 OTITEST-L2W-99）"

echo "=== 3/9 大文本 70000 字节 Windows → 麒麟（跨块边界，至少 2 块）==="
pwsh_cmd "
\$s='OTIBIGW'+('0123456789abcdef'*4374)
[IO.File]::WriteAllText('C:\Users\Public\cliptest_w2l.txt',\$s,(New-Object Text.UTF8Encoding(\$false)))
Set-Clipboard -Value \$s
'set '+\$s.Length" >/dev/null; sleep 6
kclip_text > /tmp/clipreg_k.txt
EXP=$(pwsh_cmd "(Get-FileHash 'C:\Users\Public\cliptest_w2l.txt' -Algorithm MD5).Hash" | head -1 | tr 'A-Z' 'a-z')
GOT=$(md5sum < /tmp/clipreg_k.txt | awk '{print $1}')
SZ=$(wc -c < /tmp/clipreg_k.txt)
[ "$EXP" = "$GOT" ] && ok "md5 一致（实收 $SZ 字节）" || bad "md5 不一致：期望 $EXP 实收 $GOT（$SZ 字节）"

echo "=== 4/9 大文本 200012 字节 麒麟 → Windows（4 块）==="
kssh "rm -f /tmp/clipreg_l2w.txt; printf 'OTIBIGL' > /tmp/clipreg_l2w.txt
i=0; while [ \$i -lt 12500 ]; do printf '0123456789abcdef' >> /tmp/clipreg_l2w.txt; i=\$((i+1)); done
printf 'END' >> /tmp/clipreg_l2w.txt; wc -c < /tmp/clipreg_l2w.txt" | tail -1
kset_file /tmp/clipreg_l2w.txt; sleep 6
EXP=$(kssh 'md5sum < /tmp/clipreg_l2w.txt' | awk '{print $1}')
GOT=$(pwsh_cmd "\$t=Get-Clipboard -Raw; [IO.File]::WriteAllText('C:\Users\Public\cliptest_l2w_got.txt',\$t,(New-Object Text.UTF8Encoding(\$false))); (Get-FileHash 'C:\Users\Public\cliptest_l2w_got.txt' -Algorithm MD5).Hash" | head -1 | tr 'A-Z' 'a-z')
SZ=$(pwsh_cmd "(Get-Item 'C:\Users\Public\cliptest_l2w_got.txt').Length" | head -1)
[ "$EXP" = "$GOT" ] && ok "md5 一致（实收 $SZ 字节）" || bad "md5 不一致：期望 $EXP 实收 $GOT（$SZ 字节）"

echo "=== 5/9 图片 240x160 Windows → 麒麟 ==="
pwsh_cmd "
Add-Type -AssemblyName System.Drawing; Add-Type -AssemblyName System.Windows.Forms
\$img=[System.Drawing.Image]::FromFile('C:\Users\Public\otitest\testimg.png')
[System.Windows.Forms.Clipboard]::SetImage(\$img); \$img.Dispose()
'ok'" >/dev/null; sleep 6
# 图片是二进制，直接从麒麟拉回 WSL 再本地解析（麒麟上的 python3 被 kysec 拦，别在那边用）
kssh "DISPLAY=:0 xclip -selection clipboard -t image/png -o 2>/dev/null" > /tmp/clipreg_img.png
DIM=$(png_dim /tmp/clipreg_img.png)
[ "$DIM" = "240x160" ] && ok "麒麟剪贴板里的图片是 240x160（$(wc -c < /tmp/clipreg_img.png) 字节）" || bad "图片尺寸/格式不对：$DIM"

echo "=== 6/9 图片 240x160 麒麟 → Windows ==="
kssh "cat > /tmp/testimg.png" < "$TESTIMG"
sleep 0.3
kset_file /tmp/testimg.png image/png; sleep 6
DIM=$(pwsh_cmd "
Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing
\$i=[System.Windows.Forms.Clipboard]::GetImage()
if(\$i){ '' + \$i.Width + 'x' + \$i.Height } else { 'none' }" 2>/dev/null | tail -1)
[ "$DIM" = "240x160" ] && ok "Windows 剪贴板里的图片是 240x160" || bad "图片尺寸不对：$DIM"

echo "=== 7/9 文件 Windows → 麒麟 ==="
pwsh_cmd "
\$d='C:\Users\Public\otitest'
[IO.File]::WriteAllText(\$d+'\cliptest_w2l.txt','OTI-FILE-W2L-'+('Z'*500),(New-Object Text.UTF8Encoding(\$false)))
Set-Clipboard -Path (\$d+'\cliptest_w2l.txt')" >/dev/null; sleep 6
EXP=$(pwsh_cmd "(Get-FileHash 'C:\Users\Public\otitest\cliptest_w2l.txt' -Algorithm MD5).Hash" | head -1 | tr 'A-Z' 'a-z')
# 注意：落盘在**麒麟**的 /tmp/otilink-files-<otikm pid>/ 里，必须在麒麟上查
GOTLINE=$(kssh 'f=$(ls -t /tmp/otilink-files-*/cliptest_w2l.txt 2>/dev/null | head -1); if [ -n "$f" ]; then echo "$(md5sum < "$f" | cut -d" " -f1) $f"; else echo none; fi' | tail -1)
GOT=$(echo "$GOTLINE" | awk '{print $1}')
if [ "$GOT" != "none" ] && [ -n "$GOT" ]; then
    [ "$EXP" = "$GOT" ] && ok "麒麟收到文件：$(echo "$GOTLINE" | cut -d' ' -f2-)（md5 一致）" || bad "文件内容不一致：期望 $EXP 实收 $GOT"
else
    bad "麒麟没收到文件（/tmp/otilink-files-*/cliptest_w2l.txt 不存在）"
fi
URI=$(kssh 'DISPLAY=:0 xclip -selection clipboard -t text/uri-list -o 2>/dev/null' | head -1)
case "$URI" in file://*) ok "剪贴板 uri-list = $URI";; *) bad "uri-list 未设置：'$URI'";; esac

echo "=== 8/9 文件 麒麟 → Windows ==="
kssh "printf 'OTI-FILE-L2W-' > /tmp/clipreg_l2w_file.txt
i=0; while [ \$i -lt 400 ]; do printf 'Q' >> /tmp/clipreg_l2w_file.txt; i=\$((i+1)); done
printf 'file:///tmp/clipreg_l2w_file.txt\n' > /tmp/clipreg_l2w_uri.txt" >/dev/null
kset_file /tmp/clipreg_l2w_uri.txt text/uri-list; sleep 6
EXP=$(kssh 'md5sum < /tmp/clipreg_l2w_file.txt' | awk '{print $1}')
GOT=$(pwsh_cmd "
\$d = Get-ChildItem \$env:TEMP -Directory -Filter 'otilink_files_*' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (\$d) { \$f = Join-Path \$d.FullName 'clipreg_l2w_file.txt'; if (Test-Path \$f) { (Get-FileHash \$f -Algorithm MD5).Hash + ' ' + \$f } else { 'nofile' } } else { 'nodir' }" | head -1)
GOTH=$(echo "$GOT" | awk '{print tolower($1)}')
[ "$EXP" = "$GOTH" ] && ok "Windows 收到文件：$(echo "$GOT" | cut -d' ' -f2-)" || bad "Windows 没收到文件或内容不符：$GOT"

echo "=== 9/9 日志不变量 ==="
NEW=$(agent_log_since "$LOG0")
if echo "$NEW" | grep -q "解包失败"; then
    bad "出现真正的帧解码失败（厂商 XML 帧应走 [skip]）："; echo "$NEW" | grep "解包失败" | head -3
else
    ok "无真正的帧解码失败"
fi
IMG_TX=$(echo "$NEW" | grep -c "源=图片" || true)
[ "${IMG_TX:-0}" -le 3 ] && ok "图片发送 $IMG_TX 次（无回环风暴）" || bad "图片发送 $IMG_TX 次，疑似回环"
if echo "$NEW" | grep -q "有文件却读不出来"; then
    bad "文件剪贴板读不出来（ClipFiles 回归）"; echo "$NEW" | grep "有文件却读不出来" | head -2
else
    ok "文件剪贴板读取正常"
fi

echo; echo "=== 结果 ==="
echo "  PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "  ★ 剪贴板双向（文本/大文本/图片/文件）全部通过" || echo "  ✗ 有失败项，见上"
exit $((FAIL>0))
