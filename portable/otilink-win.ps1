<#
otilink-win.ps1 —— 对拷线 Windows 侧「免安装绿色包」入口（由 otilink.cmd 拉起）

目标：Windows 侧也不需要"安装"——不写注册表、不加开机自启、不需要管理员、
      不用 csc 现场编译（包里的 otiagent2.exe 是编好的）。

角色：
  master  键鼠插在本机 → 起键鼠代理 otiagent2.exe --edge <边> + 剪贴板代理
  slave   键鼠插在对端 → **只起剪贴板代理**（键鼠是零软件的：线缆对接收端就是真实 USB 键鼠）
  auto    自动判定（本机有键鼠 → master；但对拷线的 HID 在两端都枚举成真实键鼠，
          所以 Windows 侧 auto 有歧义：默认 master，并把覆盖方法打出来）

用法（一般通过 otilink.cmd 调用）：
  otilink-win.ps1 -Role master -Edge right
  otilink-win.ps1 -Role slave
  otilink-win.ps1 -Scan            # 只探测线缆盘符（不启动任何东西）
  otilink-win.ps1 -Stop            # 停掉本包拉起的代理（不动别的进程）
#>
[CmdletBinding()]
param(
    [ValidateSet('auto','master','slave')][string]$Role = 'auto',
    [ValidateSet('left','right','top','bottom')][string]$Edge = 'right',
    [string]$Device = '',
    [string]$LogDir = '',
    [switch]$NoClip,
    [switch]$Scan,
    [switch]$Stop
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $LogDir) { $LogDir = $here }
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Force -Path $LogDir | Out-Null }

# 包布局：dist/otilink-portable-<ver>/{otilink-win.ps1, windows/*}
# 源码布局：re/portable/otilink-win.ps1 + re/windows/*
function Resolve-Path2([string]$p) {
    if (Test-Path $p) { return (Resolve-Path $p).Path }
    return ''
}
$exe = Resolve-Path2 (Join-Path $here 'windows\otiagent2.exe')
if (-not $exe) { $exe = Resolve-Path2 (Join-Path $here '..\windows\otiagent2.exe') }
$clip = Resolve-Path2 (Join-Path $here 'windows\otiagent.ps1')
if (-not $clip) { $clip = Resolve-Path2 (Join-Path $here '..\windows\otiagent.ps1') }
$kmLog   = Join-Path $LogDir 'km.log'
$clipLog = Join-Path $LogDir 'clip.log'

function Say([string]$m) { Write-Host $m }

# 启动长驻代理统一走这里：把命令行写成一个临时 .cmd，再 Start-Process 那个 .cmd。
# 为什么不直接 Start-Process 传命令行：里面的引号 + 重定向会被 PowerShell 的参数转义搞坏
# （实测：键鼠代理一声不响没起来，日志文件都没被创建）。.cmd 落在日志目录，
# 出问题时能一眼看到"到底用什么命令行起的"。
function Start-Agent([string]$CmdLine, [string]$ScriptName) {
    $f = Join-Path $LogDir $ScriptName
    Set-Content -Path $f -Value ('@echo off' + [Environment]::NewLine + $CmdLine) -Encoding ASCII
    Start-Process -FilePath $f -WindowStyle Hidden -PassThru | Out-Null
}

# 停进程必须排除自己：这条查询自己的命令行里就含关键字（历史上把查询者自己杀掉过）
function Stop-Agents {
    $me = $PID
    $procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessId -ne $me -and $_.CommandLine -and (
            $_.CommandLine -match 'otiagent2\.exe' -or
            $_.CommandLine -match 'otiagent\.ps1')
    }
    if (-not $procs) { Say '没有正在跑的代理（本包相关的）'; return }
    foreach ($p in $procs) {
        Say ("  停止 PID {0}: {1}" -f $p.ProcessId, ($p.Name))
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

if ($Stop) { Stop-Agents; exit 0 }

# ---------------------------------------------------------------- 找线缆
# 复用 otiagent.ps1 -Scan 的发现逻辑（它按 0xF0/0x00 设备信息块判定，谁能应答就用谁），
# 只解析出"哪个 \\.\X: 能作数据通道"。
function Find-Cable {
    if ($Device) { return $Device }
    if (-not $clip) { return '' }
    # 用 @(...) 拿"按行分割好的数组"：PowerShell 对外部命令输出天然按行给对象，
    # 千万不要 Out-String 拼成一整段再自己切 —— 换行一旦是 CR/LF 混用就会切错，
    # 把"打不开的盘符"和后面那行的 rc=0 配成一对（实测踩到，键鼠代理因此起不来）。
    $lines = @()
    try {
        $lines = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$clip" -Scan 2>&1)
    } catch { return '' }
    # 只认 agent 自己给出的"建议"行：建议: -Device '\\.\H:'
    # 为什么不用"rc=0 的那一行"：真实机器上 agent 的 Write-Host 输出**可能被合并成一行**
    # （实测 [2] 元素里同时含"打不开的 \\.\F:"和"\\.\H: 信息块 rc=0"），
    # 那样取第一个盘符就会取到打不开的那个 —— 键鼠代理直接起不来。
    foreach ($ln in $lines) {
        $m = [regex]::Match([string]$ln, "-Device\s+'?(\\\\\.\\[A-Za-z]:)'?")
        if ($m.Success) { return $m.Groups[1].Value }
    }
    # 退路：全文只有一个盘符候选时才敢用（多于一个就交回给代理自己探测，别瞎猜）
    $cands = @()
    foreach ($ln in $lines) {
        foreach ($mm in [regex]::Matches([string]$ln, '\\\\\.\\[A-Za-z]:')) {
            if ($cands -notcontains $mm.Value) { $cands += $mm.Value }
        }
    }
    if ($cands.Count -eq 1) { return $cands[0] }
    return ''
}

$dev = Find-Cable
if ($Scan) {
    Say "== 线缆设备探测 =="
    if ($dev) { Say ("  可作数据通道: {0}" -f $dev) } else { Say "  没找到能应答厂商命令的设备（线缆没插？驱动没装？）" }
    exit 0
}
if (-not $dev) {
    # 探测失败：不要瞎猜盘符。键鼠代理会退回它的内建缺省（\\.\H:），
    # 剪贴板代理在 -Device 为空时**自己会探测**（Find-CableDevice）——比我们猜准。
    Say '  [warn] 没探测到线缆设备：键鼠代理将用内建缺省(\\.\H:)，剪贴板代理自行探测'
    Say '         若仍失败：otilink.cmd --scan 看候选，或 otilink.cmd master -Device \\.\X:'
}

Say "== otilink Windows 绿色包 =="
Say ("  包位置 : {0}" -f $here)
$devTxt = $dev
if (-not $devTxt) { $devTxt = '(自动探测)' }
Say ("  设备   : {0}" -f $devTxt)
$clipTxt = '开'
if ($NoClip) { $clipTxt = '关' }
Say ("  角色   : {0}  边: {1}  剪贴板: {2}" -f $Role, $Edge, $clipTxt)
Say ("  日志   : {0} / {1}" -f $kmLog, $clipLog)

if ($Role -eq 'auto') {
    $Role = 'master'
    Say '  [info] auto → 按主控端启动（本机的键鼠可用）。若键鼠其实插在对端，请用: otilink.cmd slave'
}

$started = $false

# ---------------------------------------------------------------- 键鼠（主控端）
if ($Role -eq 'master') {
    if (-not $exe) {
        Say '  [FAIL] 找不到 windows\otiagent2.exe（包不完整？）'
        exit 1
    }
    # 已经有一个在跑就不重复起（多实例 = 多套低级钩子，行为不可预测）
    $n = (Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
           Where-Object { $_.CommandLine -match 'otiagent2\.exe' }).Count
    if ($n -ge 1) {
        Say ("  [info] 已经有 {0} 个 otiagent2 在跑，不重复启动（要换参数先 --stop）" -f $n)
    } else {
        Say ("  启动键鼠代理: otiagent2.exe --device {0} --edge {1}" -f $dev, $Edge)
        # 必须显式重定向日志：Start-Process 默认不给子进程接标准输出，日志就丢了
        # （与安装版的 restart-km.vbs 用 cmd /c ... > km.log 2>&1 是同一手法）
        $devArg = ''
        if ($dev) { $devArg = ' --device ' + $dev }
        $kline = '"' + $exe + '"' + $devArg + ' --edge ' + $Edge + ' > "' + $kmLog + '" 2>&1'
        Start-Agent $kline 'otilink-run-km.cmd'
        $started = $true
    }
}

# ---------------------------------------------------------------- 剪贴板（两端都要）
if (-not $NoClip) {
    if (-not $clip) {
        Say '  [FAIL] 找不到 windows\otiagent.ps1（包不完整？）'
    } else {
        $n = (Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
               Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match '-File\s+"?[^"]*otiagent\.ps1' }).Count
        if ($n -ge 1) {
            Say ("  [info] 已经有 {0} 个剪贴板代理在跑，不重复启动" -f $n)
        } else {
            Say ("  启动剪贴板代理: -Cable -Clipboard -Device {0}" -f $dev)
            $devArg2 = ''
            if ($dev) { $devArg2 = ' -Device ' + $dev }   # 空则让代理自己探测（它更会挑）
            $cline = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' +
                      $clip + '" -Cable -Clipboard' + $devArg2 + ' > "' + $clipLog + '" 2>&1'
            Start-Agent $cline 'otilink-run-clip.cmd'
            $started = $true
        }
    }
}

if ($started) { Start-Sleep -Seconds 2 }
Say ''
Say '== 就绪 =='
Say '  用法：把指针推到屏幕边缘即交给对端；Ctrl+Alt+-> 强制收回本机（键鼠代理的热键）'
Say '  停止：otilink.cmd --stop（或任务管理器）'
Say '  日志：km.log / clip.log（就在本目录；出问题先看它们）'
