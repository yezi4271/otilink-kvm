# cursorwatch.ps1 —— 光标观察器
#
# 用途：验证"线缆把 HID 注入到本机"是否真的生效。
#   在 Windows（被控端）跑本脚本，在 Linux（主控端）跑 otiprobe mousecal，
#   两边一对照即可判定 HID 包的 12 字节格式。
#
# 只用 System.Windows.Forms 读光标位置，不安装任何东西、不改系统。
#
# 用法:  powershell -ExecutionPolicy Bypass -File cursorwatch.ps1 -Seconds 45
param(
    [int]$Seconds = 45,
    [int]$IntervalMs = 80,
    [switch]$Quiet          # 只在变化时输出（缺省行为）；-Quiet:$false 则每次都输出
)

Add-Type -AssemblyName System.Windows.Forms

$t0 = Get-Date
$prev = $null
$changes = 0

Write-Output "== 光标观察 $Seconds 秒（每 ${IntervalMs}ms 采样，仅在变化时输出）=="
$p = [System.Windows.Forms.Cursor]::Position
Write-Output ("[{0,6:N2}s] 起点 {1},{2}" -f 0, $p.X, $p.Y)
$prev = "$($p.X),$($p.Y)"

while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $p = [System.Windows.Forms.Cursor]::Position
    $cur = "$($p.X),$($p.Y)"
    if ($cur -ne $prev) {
        $el = ((Get-Date) - $t0).TotalSeconds
        $dx = ''
        if ($prev) {
            $a = $prev -split ','; $b = $cur -split ','
            $dx = "  Δ=({0},{1})" -f ([int]$b[0] - [int]$a[0]), ([int]$b[1] - [int]$a[1])
        }
        Write-Output ("[{0,6:N2}s] {1}{2}" -f $el, $cur, $dx)
        $prev = $cur
        $changes++
    } elseif (-not $Quiet) {
        # 无变化不打印
    }
    Start-Sleep -Milliseconds $IntervalMs
}

Write-Output "== 观察结束：共 $changes 次位置变化 =="
if ($changes -eq 0) {
    Write-Output "   （光标全程未动 —— 若此刻主控端正在发 HID 包，说明该格式无效）"
}
