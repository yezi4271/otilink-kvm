# install.ps1 - OTiLink Windows installer (Win10 / Win11), user phase.
#
# Run by double-clicking install.cmd as a NORMAL user.  One UAC prompt follows for the
# machine-wide bits (copy otiagent2.exe to C:\Users\Public, apply quiet-cable policy).
#
# The cable itself is driverless on Win10/11 (USB mass storage + HID use inbox drivers).
# This kit installs OUR agent + the "quiet cable" settings; it does NOT install the vendor
# MacKMLink / SKLoader software (that would fight the agent for the cable link).
param(
    [switch]$QuietOnly,
    [switch]$NoStart,
    [switch]$NoElevated
)
$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$files = Join-Path $here 'files'
$dest  = Join-Path $env:USERPROFILE 'otilink'
$log   = 'C:\Users\Public\otilink-install.log'

function Log([string]$m) {
    $line = ('{0} {1}' -f (Get-Date).ToString('HH:mm:ss'), $m)
    Write-Host $line
    try { Add-Content -LiteralPath $log -Value $line -Encoding UTF8 } catch { }
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Log ("=== install start (admin={0} quiet={1}) ===" -f $isAdmin, $QuietOnly)
if ($isAdmin) { Log 'NOTE: running elevated. Prefer double-click install.cmd as a normal user; otherwise the agent also runs elevated.' }

# Mark-of-the-Web: files copied from a zip/USB must not be blocked by SmartScreen.
try { Get-ChildItem -LiteralPath $files -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue } catch { }

New-Item -ItemType Directory -Force -Path $dest | Out-Null
foreach ($f in @('otiagent.ps1','clip-watchdog.ps1','clip-watchdog.vbs','restart-clip.vbs','quiet-cable.ps1','vendor-off.ps1','uninstall-windows.ps1')) {
    $s = Join-Path $files $f
    if (Test-Path -LiteralPath $s) {
        Copy-Item -LiteralPath $s -Destination $dest -Force
        Log ("copied " + $f)
    } else {
        Log ("MISSING " + $f)
    }
}

if (-not $QuietOnly) {
    $iw = Join-Path $files 'install-windows.ps1'
    if (Test-Path -LiteralPath $iw) {
        Log 'install-windows.ps1 -NoStart'
        & $iw -NoStart
    } else {
        Log 'MISSING install-windows.ps1'
    }
}

if (-not $NoElevated) {
    $elev = Join-Path $here 'install-elevated.ps1'
    $argv = @('-NoProfile','-ExecutionPolicy','Bypass','-File', ('"{0}"' -f $elev), '-Dest', ('"{0}"' -f $dest))
    if ($QuietOnly) { $argv += '-QuietOnly' }
    Log 'requesting elevation (UAC): otiagent2.exe + quiet policy ...'
    try {
        $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argv -Verb RunAs -Wait -PassThru
        Log ("elevated exit=" + $p.ExitCode)
    } catch {
        Log ("elevation failed/cancelled: " + $_.Exception.Message)
    }
}

if (-not $QuietOnly -and -not $NoStart) {
    $rc = Join-Path $dest 'restart-clip.vbs'
    $wc = Join-Path $dest 'clip-watchdog.vbs'
    if (Test-Path -LiteralPath $rc) {
        & cscript.exe //B //NoLogo $rc
        Log 'clipboard agent started'
    }
    Start-Sleep -Seconds 2
    if (Test-Path -LiteralPath $wc) {
        & cscript.exe //B //NoLogo $wc
        Log 'watchdog started'
    }
}

Log '=== install done ==='
Write-Host ''
Write-Host 'Log:       C:\Users\Public\otilink-install.log'
Write-Host 'Agent log: C:\Users\Public\clip.log'
