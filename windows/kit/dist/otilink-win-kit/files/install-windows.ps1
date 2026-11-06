# install-windows.ps1 —— Windows 被控端安装（不需要管理员）
#
# 做三件事：
#   1) 把 agent 复制到 %USERPROFILE%\otilink\（不依赖 WSL/源码目录）
#   2) 在"启动"文件夹放一个隐藏窗口的启动器 → 登录自动运行
#   3) 立即启动一次
#
# 用法：powershell -ExecutionPolicy Bypass -File install-windows.ps1
param(
    [string]$Device = '\\.\H:',             # 对拷线被控端的盘符/卷
    [ValidateSet('native','absolute')]
    [string]$MouseMode = 'native',
    [switch]$NoStart                        # 只安装不立即启动
)

$ErrorActionPreference = 'Stop'
$src  = Join-Path $PSScriptRoot 'otiagent.ps1'
if (-not (Test-Path $src)) { Write-Host "找不到 $src"; exit 1 }

$dest = Join-Path $env:USERPROFILE 'otilink'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
try { Copy-Item $src (Join-Path $dest 'otiagent.ps1') -Force -ErrorAction Stop } catch { Write-Host "otiagent.ps1 被占用，跳过覆盖（其余步骤继续）" }
Write-Host "已安装到 $dest\otiagent.ps1"

# 第 58 轮：剪贴板看门狗（代理运行中静默死掉时自动拉起，见 re/NOTES.md §73）
foreach ($f in @('clip-watchdog.ps1','clip-watchdog.vbs')) {
    $s = Join-Path $PSScriptRoot $f
    if (Test-Path $s) { try { Copy-Item $s (Join-Path $dest $f) -Force -ErrorAction Stop } catch { Write-Host "$f 被占用，跳过覆盖" } }
}

$startup = [Environment]::GetFolderPath('Startup')
$vbs = Join-Path $startup 'otilink-agent.vbs'
# 只起"角色代理"（剪贴板 + ROLE 协商）；键鼠代理 otiagent2 由它按协商结果起停（L17）。
# 不再传 -Inject：那是旧键鼠路径，会与线缆 HID 通道打架（AGENTS §4.2）。
$cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$dest\otiagent.ps1`" -Cable -Clipboard -NoTray -MouseMode $MouseMode -Device $Device"
@"
' OTiLink 角色代理：登录时隐藏窗口启动（不需要管理员）
' 卸载：运行 uninstall-windows.ps1，或删除本文件后重新登录
Set sh = CreateObject("WScript.Shell")
sh.Run "$cmd", 0, False
"@ | Set-Content -Path $vbs -Encoding ASCII
Write-Host "已创建登录自启: $vbs"

# 看门狗单独一个启动项（名字不同，互不影响）
$wdSrc = Join-Path $PSScriptRoot 'clip-watchdog.vbs'
if (Test-Path $wdSrc) {
    $wdStartup = Join-Path $startup 'otilink-watchdog.vbs'
    Copy-Item $wdSrc $wdStartup -Force
    Write-Host "已创建看门狗登录自启: $wdStartup"
}

if (-not $NoStart) {
    Write-Host "立即启动…"
    Start-Process powershell.exe -ArgumentList @(
        '-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden',
        '-File', (Join-Path $dest 'otiagent.ps1'),
        '-Cable','-Clipboard','-NoTray','-MouseMode',$MouseMode,'-Device',$Device
    ) -WindowStyle Hidden
    $wdVbs = Join-Path $dest 'clip-watchdog.vbs'
    if (Test-Path $wdVbs) {
        Start-Process cscript.exe -ArgumentList @('//B','//NoLogo',$wdVbs) -WindowStyle Hidden
        Write-Host "已启动剪贴板看门狗"
    }
}

Write-Host @"

完成。被控端会在每次登录时自动运行（-NoTray：不显示托盘图标；停止：结束命令行含
otiagent.ps1 的 powershell.exe，或运行 uninstall-windows.ps1）。
手动启动一次（可见日志）：
  powershell -ExecutionPolicy Bypass -File "$dest\otiagent.ps1" -Cable -Clipboard -MouseMode $MouseMode -Device '$Device'
"@
