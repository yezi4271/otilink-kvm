# install-elevated.ps1 - OTiLink Windows installer, admin phase.
#
# Called by install.ps1 via UAC.  Does only the things that need admin:
#   * copy otiagent2.exe to C:\Users\Public (the path otiagent.ps1 expects)
#   * apply the quiet-cable policy (AutoPlay off / hide the cable volumes / hide tray icon)
#   * stop the vendor MacKMLink suite and disable its autostart (if present)
param(
    [string]$Dest = (Join-Path $env:USERPROFILE 'otilink'),
    [switch]$QuietOnly
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log  = 'C:\Users\Public\otilink-install.log'

function Log([string]$m) {
    $line = ('{0} {1}' -f (Get-Date).ToString('HH:mm:ss'), '[elev] ' + $m)
    Write-Host $line
    try { Add-Content -LiteralPath $log -Value $line -Encoding UTF8 } catch { }
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Log ("=== elevated start (admin={0} quiet={1}) ===" -f $isAdmin, $QuietOnly)
if (-not $isAdmin) {
    Log 'not elevated; cannot install otiagent2.exe / quiet policy'
    exit 2
}

if (-not $QuietOnly) {
    $srcExe = Join-Path $here 'files\otiagent2.exe'
    if (Test-Path -LiteralPath $srcExe) {
        $dstExe = 'C:\Users\Public\otiagent2.exe'
        Copy-Item -LiteralPath $srcExe -Destination $dstExe -Force
        Unblock-File -LiteralPath $dstExe -ErrorAction SilentlyContinue
        Log ('installed ' + $dstExe)
    } else {
        Log 'MISSING files\otiagent2.exe'
    }
}

$qc = Join-Path $Dest 'quiet-cable.ps1'
if (Test-Path -LiteralPath $qc) {
    Log 'quiet-cable.ps1 -Elevated'
    & $qc -Elevated
} else {
    Log ('MISSING ' + $qc)
}

if (-not $QuietOnly) {
    $vo = Join-Path $Dest 'vendor-off.ps1'
    if (Test-Path -LiteralPath $vo) {
        Log 'vendor-off.ps1'
        & $vo
    } else {
        Log ('MISSING ' + $vo)
    }
}

Log '=== elevated done ==='
