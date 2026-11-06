# clip-watchdog.ps1 —— Windows 侧**剪贴板代理看门狗**（不需要管理员）
#
# 为什么需要（第 58 轮，2026-10-19）：
#   剪贴板代理会在**运行中静默死掉**（2026-10-08、2026-10-16 两次现场：clip.log 停更、进程消失），
#   而登录自启（启动文件夹的 otilink-agent.vbs）只管登录那一刻，中途死掉没有任何东西把它拉起来
#   → 剪贴板双向全废（麒麟日志反复刷「重发 3 次仍无确认（对端在吗？），放弃」）。见 re/NOTES.md §73。
#
# 判活**两条腿**（为什么不能只查进程）：
#   ① 主判据 = clip.log 的 mtime。代理活着时每秒写一行 ROLE，日志一直在动；**纯文件系统操作**，
#      不碰 WMI —— 实测 Get-CimInstance 在别的查询压着时会阻塞好几分钟，只靠它会把「代理已死」
#      迟迟发现不了（本轮实测：杀进程后 4 分钟才被察觉，回归里出现 184s 才恢复）。
#   ② 日志不新鲜时才问 WMI（带 -OperationTimeoutSec）。WMI 超时/失败按「未知」处理，
#      **绝不据此判死**（避免把好代理当成死的、反复重启）。
#   连续 -Grace 次判定为「死」→ 调 restart-clip.vbs 拉起（脱离式，日志照旧写 clip.log）。
#
# 单实例：具名互斥体，重复启动会自己退出。
# 停止：任务管理器结束命令行含 clip-watchdog.ps1 的 powershell.exe；
#       或删掉启动文件夹的 otilink-watchdog.vbs 后注销。
param(
    [int]$IntervalSec = 10,
    [int]$Grace = 2,
    [int]$StaleSec = 12,
    [string]$Log = 'C:\Users\Public\watchdog.log',
    [string]$ClipLog = 'C:\Users\Public\clip.log',
    [string]$OtilinkDir = (Join-Path $env:USERPROFILE 'otilink')
)
$ErrorActionPreference = 'Continue'

function Write-Log([string]$m) {
    $line = '[' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '] ' + $m
    try { Add-Content -LiteralPath $Log -Value $line -Encoding UTF8 -ErrorAction Stop } catch { }
}

# 返回 $true=活着 / $false=死了 / $null=判不了（WMI 失败、日志缺失）
function Test-ProxyAlive {
    # ① 日志新鲜度（快、不碰 WMI）
    try {
        if (Test-Path -LiteralPath $ClipLog) {
            $age = ((Get-Date) - (Get-Item -LiteralPath $ClipLog -ErrorAction Stop).LastWriteTime).TotalSeconds
            if ($age -le $StaleSec) { return $true }
        }
    } catch { }
    # ② 日志不新鲜 → 问 WMI（限时；失败按未知）
    try {
        $n = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -OperationTimeoutSec 8 -ErrorAction Stop |
               Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -and $_.CommandLine -match 'otiagent\.ps1' }).Count
        if ($n -ge 1) { return $true }
        return $false
    } catch { return $null }
}

# 日志轮转：>2MB 改名，避免常驻把盘写满
try {
    if ((Test-Path -LiteralPath $Log) -and ((Get-Item -LiteralPath $Log).Length -gt 2MB)) {
        Move-Item -LiteralPath $Log -Destination ($Log + '.old') -Force
    }
} catch { }

# --- 单实例 ---
$mutex = New-Object System.Threading.Mutex($false, 'Local\OTiLinkClipWatchdog')
$owned = $false
try { $owned = $mutex.WaitOne(0) } catch { $owned = $false }
if (-not $owned) { Write-Log '另一个看门狗实例已经在跑，本次退出'; exit 0 }

Write-Log ("看门狗启动 pid=$PID interval=${IntervalSec}s grace=${Grace} stale=${StaleSec}s dir=$OtilinkDir")
$miss = 0
while ($true) {
    $alive = Test-ProxyAlive
    if ($null -eq $alive) {
        Write-Log '判活失败（WMI 超时/日志缺失），本轮跳过'
    } elseif ($alive) {
        if ($miss -gt 0) { Write-Log '代理已恢复' }
        $miss = 0
    } else {
        $miss++
        Write-Log ("代理缺失（连续 $miss/$Grace 次）")
        if ($miss -ge $Grace) {
            $vbs = Join-Path $OtilinkDir 'restart-clip.vbs'
            if (Test-Path -LiteralPath $vbs) {
                Write-Log '拉起剪贴板代理（restart-clip.vbs）'
                try { & cscript.exe //B //NoLogo $vbs | Out-Null }
                catch { Write-Log ('restart-clip.vbs 调用异常：' + $_.Exception.Message) }
                # 冷启动 + 首行日志可能要几秒：**轮询**最多 25s，别用单次检查下结论
                # （单次 6s 检查会把「刚起来还没写日志」误报成失败，也会让回归里的
                #  紧随其后的实例数断言看到 0 —— 实测踩过）。
                $n2 = $null
                for ($k = 0; $k -lt 25; $k++) {
                    Start-Sleep -Seconds 1
                    $n2 = Test-ProxyAlive
                    if ($n2 -eq $true) { break }
                }
                if ($n2 -eq $true) { Write-Log ('拉起成功（第 ' + ($k + 1) + ' 秒确认代理已在写日志/进程在位）') }
                else { Write-Log ('拉起失败（25s 内未确认，判活=' + $n2 + '，看 C:\Users\Public\clip.err）') }
            } else {
                Write-Log ("找不到 $vbs —— 先跑 deploy.sh --watchdog 部署，本轮无法拉起")
            }
            $miss = 0   # 失败也等下一轮再试，避免日志刷屏
        }
    }
    Start-Sleep -Seconds $IntervalSec
}
