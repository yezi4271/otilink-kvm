# volumeclip.ps1 —— 用对拷线的 1MB 共享卷做剪贴板同步（不依赖私有 SCSI 命令）
#
# 动机：真机实测发现数据管道写（0xD9/0x2A）在本端被拒（CHECK CONDITION），
#       但设备会暴露一个 1MB 的 FAT 卷（2213 人格下是 H:"VirtualLink"，
#       2208 人格下是 F:"Transfer line"）。若这个卷**两端共享**，
#       剪贴板就可以用最朴素的文件读写来同步 —— 不需要管理员、不需要私有命令。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File volumeclip.ps1 -Test               # 只做本地自检
#   powershell -ExecutionPolicy Bypass -File volumeclip.ps1 -Volume F:\         # 正常跑（Ctrl+C 退出）
#   powershell -ExecutionPolicy Bypass -File volumeclip.ps1 -Volume H:\ -IntervalMs 300
#
# 约定（两端对称）：
#   <卷>\otilink_clip_win.txt   ← 本机（Windows）剪贴板内容
#   <卷>\otilink_clip_lin.txt   ← 对端（Linux）剪贴板内容
#   各自只写自己的文件、只读对方的文件 → 不会互相覆盖

param(
    [string]$Volume = 'F:\',
    [int]$IntervalMs = 500,
    [switch]$Test,
    [switch]$Verbose
)

try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}
$ErrorActionPreference = 'Stop'

$outFile = Join-Path $Volume 'otilink_clip_win.txt'   # 本机写
$inFile  = Join-Path $Volume 'otilink_clip_lin.txt'   # 对端写

function Hash-Text([string]$t) {
    if ($null -eq $t) { return '' }
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $b = [Text.Encoding]::UTF8.GetBytes($t)
    return [BitConverter]::ToString($md5.ComputeHash($b))
}

if ($Test) {
    Write-Host "== 共享卷剪贴板：本地自检 =="
    if (-not (Test-Path $Volume)) { Write-Host "  [FAIL] 卷 $Volume 不存在"; exit 1 }
    Write-Host "  [OK]   卷可访问: $Volume"
    $items = Get-ChildItem $Volume -Force -ErrorAction SilentlyContinue |
             Select-Object -First 10 | ForEach-Object { $_.Name }
    Write-Host ("  卷内文件: {0}" -f ($items -join ', '))

    $probe = "otilink-volumeclip-probe-" + (Get-Random)
    Set-Content -Path $outFile -Value $probe -Encoding UTF8 -NoNewline
    $back = Get-Content -Path $outFile -Raw -ErrorAction SilentlyContinue
    if ($back -eq $probe) { Write-Host "  [OK]   写入并读回一致（$($probe.Length) 字节）" }
    else { Write-Host "  [FAIL] 写入/读回不一致"; exit 1 }

    if (Test-Path $inFile) {
        $peer = Get-Content -Path $inFile -Raw -ErrorAction SilentlyContinue
        Write-Host ("  [INFO] 对端文件已存在（{0} 字节）——说明对端至少在卷里写过东西" -f $peer.Length)
    } else {
        Write-Host "  [INFO] 对端文件 $inFile 还不存在（对端尚未写过）"
    }
    Remove-Item $outFile -ErrorAction SilentlyContinue
    Write-Host "  自检结束（已清理探针文件）"
    exit 0
}

Write-Host "共享卷剪贴板：卷=$Volume  本机写=$outFile  对端读=$inFile  间隔=${IntervalMs}ms"
$lastOut = ''
$lastIn  = ''
$sent = 0; $applied = 0

while ($true) {
    # 1) 本地剪贴板 → 写自己的文件
    try {
        $clip = Get-Clipboard -Raw -ErrorAction Stop
        if ($null -ne $clip -and $clip.Length -gt 0 -and $clip.Length -le 200000) {
            $h = Hash-Text $clip
            if ($h -ne $lastOut) {
                $lastOut = $h
                Set-Content -Path $outFile -Value $clip -Encoding UTF8 -NoNewline
                $sent++
                if ($Verbose) { Write-Host "  → 已写本机剪贴板 $($clip.Length) 字符" }
            }
        }
    } catch { }

    # 2) 对端文件 → 本机剪贴板（仅在本机剪贴板与对端内容不同时写入，避免抖动）
    try {
        if (Test-Path $inFile) {
            $peer = Get-Content -Path $inFile -Raw -ErrorAction SilentlyContinue
            if ($null -ne $peer -and $peer.Length -gt 0 -and $peer.Length -le 200000) {
                $hp = Hash-Text $peer
                if ($hp -ne $lastIn) {
                    $lastIn = $hp
                    $cur = Get-Clipboard -Raw -ErrorAction SilentlyContinue
                    if ($cur -ne $peer) {
                        Set-Clipboard -Value $peer
                        $applied++
                        Write-Host "  ← 已应用对端剪贴板 $($peer.Length) 字符"
                    }
                }
            }
        }
    } catch { }

    Start-Sleep -Milliseconds $IntervalMs
}
