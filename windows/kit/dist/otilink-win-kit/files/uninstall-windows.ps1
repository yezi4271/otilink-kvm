# uninstall-windows.ps1 —— Windows 被控端卸载（不需要管理员）
#
# 删除登录自启与安装目录；可选 -PurgeLog 一并清理临时日志。
param([switch]$PurgeLog)

$ErrorActionPreference = 'SilentlyContinue'

$startup = [Environment]::GetFolderPath('Startup')
$vbs = Join-Path $startup 'otilink-agent.vbs'
if (Test-Path $vbs) { Remove-Item $vbs -Force; Write-Host "已删除自启: $vbs" }
$wvbs = Join-Path $startup 'otilink-watchdog.vbs'
if (Test-Path $wvbs) { Remove-Item $wvbs -Force; Write-Host "已删除看门狗自启: $wvbs" }

$dest = Join-Path $env:USERPROFILE 'otilink'
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force; Write-Host "已删除 $dest" }

# 结束正在运行的 agent
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | ForEach-Object {
    if ($_.CommandLine -and ($_.CommandLine -match 'otiagent\.ps1' -or $_.CommandLine -match 'clip-watchdog\.ps1')) {
        Stop-Process -Id $_.ProcessId -Force
        Write-Host ("已结束 agent/看门狗进程 " + $_.ProcessId)
    }
}
if ($PurgeLog) { Remove-Item "$env:TEMP\otilink*" -Force }
Write-Host "卸载完成。"
