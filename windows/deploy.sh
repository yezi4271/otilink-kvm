#!/bin/bash
# deploy.sh —— 把 Windows 侧脚本部署到代理**真正运行**的目录并做解析自检。
#
# 为什么需要它：
#  1) 代理跑的是 `%USERPROFILE%\otilink\otiagent.ps1`，**不是**仓库里这份 ——
#     改完不 cp 过去等于没改（历史上为此白折腾两轮）。
#  2) otiagent.ps1 里有中文注释，PowerShell 5.1 按 ANSI 读 .ps1，**必须带 UTF-8 BOM**；
#     用不支持 BOM 的编辑器/工具改完容易把 BOM 弄丢，症状是一堆莫名其妙的
#     "缺少 }" 解析错误（实测踩过）。
#  3) 解析自检能在**不重启代理**的前提下发现语法错误。
#
# 用法： ./deploy.sh            # 部署 otiagent.ps1 + 其它 ps1 并自检
#        ./deploy.sh --restart-km # 只重启键鼠代理（otiagent2.exe）
#        ./deploy.sh --restart  # 顺带重启剪贴板代理（打印日志到 C:\Users\Public\clip.log）
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
# 部署目录从 Windows 的 %USERPROFILE% 推导（不写死用户名）；可用 OTI_WIN_DEST 覆盖
win_home() {
    local p
    p=$(powershell.exe -NoProfile -Command 'Write-Output $env:USERPROFILE' 2>/dev/null | tr -d '\r' | tail -1)
    [ -n "$p" ] || p=$(cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r' | tail -1)
    printf '%s' "$p"
}
WINHOME=$(win_home)
if [ -z "$WINHOME" ]; then
    echo "[FAIL] 取不到 Windows 的 %USERPROFILE%（powershell.exe / cmd.exe 不在 PATH？）" >&2
    echo "       显式指定即可：OTI_WIN_DEST='C:\\Users\\<你>\\otilink' ./deploy.sh" >&2
    exit 1
fi
DEST_WIN="${OTI_WIN_DEST:-$WINHOME\\otilink}"
DEST=$(wslpath "$WINHOME")/otilink
[ -d "$DEST" ] || mkdir -p "$DEST" 2>/dev/null || true
LOG='C:\Users\Public\clip.log'

ensure_bom() {
    python3 - "$1" <<'EOF'
import sys
p = sys.argv[1]
d = open(p, 'rb').read()
if not d.startswith(b'\xef\xbb\xbf'):
    open(p, 'wb').write(b'\xef\xbb\xbf' + d)
    print("  [bom] 补上 UTF-8 BOM:", p)
EOF
}

echo "=== 1/3 确保 BOM ==="
for f in "$HERE"/otiagent.ps1 "$HERE"/*.ps1 "$HERE"/*.cs; do
    [ -f "$f" ] && ensure_bom "$f"      # .cs 也补：中文注释被 csc 按 ANSI 解码时可能吞掉代码
done

echo "=== 2/3 复制到 $DEST ==="
cp -v "$HERE"/otiagent.ps1 "$HERE"/*.ps1 "$HERE"/*.vbs "$HERE"/*.cs "$DEST"/ 2>/dev/null | sed 's/^/  /'

# .vbs 必须是 **CRLF**：Windows Script Host 遇到 LF-only 的脚本会把这行与下一行
# 合成一条语句，症状是 cscript 返回 0 但什么都没发生（实测踩过一次，
# 排查了很久才发现是行尾）。git/编辑器很容易把它改成 LF，所以每次部署都归一化。
python3 - "$DEST" <<'EOF'
import glob, os, sys
for p in glob.glob(os.path.join(sys.argv[1], '*.vbs')):
    d = open(p, 'rb').read().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
    open(p, 'wb').write(d)
    print("  [crlf]", os.path.basename(p))
EOF

echo "=== 3/3 解析自检（PowerShell Parser）==="
powershell.exe -NoProfile -Command "
\$bad = 0
Get-ChildItem '$DEST_WIN\*.ps1' | ForEach-Object {
    \$e = \$null
    [void][System.Management.Automation.Language.Parser]::ParseFile(\$_.FullName, [ref]\$null, [ref]\$e)
    if (\$e -and \$e.Count -gt 0) { \$bad++; Write-Host ('  [FAIL] ' + \$_.Name); \$e | ForEach-Object { Write-Host ('     ' + \$_.Message) } }
    else { Write-Host ('  [ok] ' + \$_.Name) }
}
if (\$bad -gt 0) { exit 1 }
" 2>&1 | tr -d '\r'
RC=${PIPESTATUS[0]}

if [ "${1:-}" = "--restart-km" ]; then
    # 重启键鼠代理（otiagent2.exe）：先杀 → **编译** → 再用 VBS 脱离式拉起。
    # 为什么要在这里编译：`csc /out:` 覆盖**正在运行**的 exe 会**静默失败**，
    # 手工流程漏一步就会一直在跑旧二进制（第 37 轮为此白查了半天）。
    # 编译失败必须显式报错 —— 这里不用 2>/dev/null 吞输出。
    echo "=== 重启键鼠代理 otiagent2（先杀 → 编译 → 启动）==="
    powershell.exe -NoProfile -Command "
Get-Process otiagent2 -EA SilentlyContinue | ForEach-Object { Stop-Process -Id \$_.Id -Force; Write-Host ('  stopped ' + \$_.Id) }
Start-Sleep -Milliseconds 500
Copy-Item -Force '$DEST_WIN\otiagent2.cs' 'C:\Users\Public\otiagent2.cs' -EA SilentlyContinue
\$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& \$csc /nologo /target:exe /out:C:\Users\Public\otiagent2.exe C:\Users\Public\otiagent2.cs 2>&1 |
  Where-Object { \$_ -match ': error' } | ForEach-Object { Write-Host ('  ' + \$_) }
Write-Host ('  csc exit=' + \$LASTEXITCODE + '  exe=' + (Get-Item C:\Users\Public\otiagent2.exe).Length + 'B')
" >/tmp/otilink_kmstop.out 2>&1
    tr -d '\r' < /tmp/otilink_kmstop.out | sed 's/^/  /'
    cscript.exe //B //NoLogo "$DEST_WIN\\restart-km.vbs" >/dev/null 2>&1
    # 轮询而不是死等 3 秒：Windows PowerShell 冷启动经常 >3s（死等会误报"没起来"）
    N=0
    for _ in $(seq 1 12); do
        sleep 1
        N=$(powershell.exe -NoProfile -Command "(Get-Process otiagent2 -EA SilentlyContinue | Measure-Object).Count" 2>/dev/null | tr -d '\r' | tail -1)
        [ "${N:-0}" -ge 1 ] && break
    done
    [ "${N:-0}" -ge 1 ] && echo "  [ok] 键鼠代理已在跑（$N 个）" || echo "  [FAIL] 键鼠代理没起来"
    echo "  当前版本：$(tr -d '\r' < /mnt/c/Users/Public/km.log 2>/dev/null | grep -o '\[build [^]]*\]' | tail -1)"
fi

if [ "${1:-}" = "--restart" ]; then
    echo "=== 重启剪贴板代理（日志 -> $LOG）==="
    # 必须排除 \$PID：这条查询命令**自己的命令行里就含** '-File*otiagent.ps1*'，
    # 不排除自己 = 把自己 Stop-Process 掉 —— 症状是脚本走到这里一声不响就结束、
    # 代理也没起来（实测踩过一次）。
    powershell.exe -NoProfile -Command "
Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" |
  Where-Object { \$_.ProcessId -ne \$PID -and \$_.CommandLine -like '*-File*otiagent.ps1*' } |
  ForEach-Object { Stop-Process -Id \$_.ProcessId -Force; Write-Host ('  stopped ' + \$_.ProcessId) }
" >/tmp/otilink_stop.out 2>&1
    tr -d '\r' < /tmp/otilink_stop.out | sed 's/^/  /'
    # 启动必须走 VBS（WScript.Shell.Run 完全脱离调用者）。用 PowerShell 的 Start-Process
    # 会因为新进程继承 WSL 侧句柄而让本脚本挂死到超时 —— 实测。
    cscript.exe //B //NoLogo "$DEST_WIN\\restart-clip.vbs" >/dev/null 2>&1
    # 状态确认放在 bash 侧：直接查进程 + 看日志比信 PowerShell 的输出可靠。
    # ⚠️ 必须轮询：Windows PowerShell 冷启动经常 >3s，死等 3 秒会误报"代理没起来"
    #    （2026-10-08 实测：代理其实已起，clip.log 已写"设备已打开"，只是检查跑早了）。
    N=0
    for _ in $(seq 1 12); do
        sleep 1
        N=$(powershell.exe -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { \$_.ProcessId -ne \$PID -and \$_.CommandLine -like '*-File*otiagent.ps1*' } | Measure-Object).Count" 2>/dev/null | tr -d '\r' | tail -1)
        [ "${N:-0}" -ge 1 ] && break
    done
    if [ "${N:-0}" -ge 1 ]; then
        echo "  [ok] 剪贴板代理已在跑（$N 个），日志：$LOG"
        tail -2 /mnt/c/Users/Public/clip.log 2>/dev/null | tr -d '\r' | sed 's/^/       /'
    else
        echo "  [FAIL] 代理没起来，看 C:\\Users\\Public\\clip.err"
        tail -5 /mnt/c/Users/Public/clip.err 2>/dev/null | sed 's/^/       /'
    fi
fi

exit $RC
