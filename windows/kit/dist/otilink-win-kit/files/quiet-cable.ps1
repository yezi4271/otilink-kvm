# quiet-cable.ps1 - silence the OTi/Handbond USB copy cable on Windows.
#
# Goal: plugging the cable must not pop anything up, must not add "MacKMLink" /
# "VirtualLink" drives to This PC, and must not add tray icons for it.
#
# What it changes (all reversible with -Undo):
#   1) AutoPlay off  -> no "choose what to do" popup when the CD/removable volume arrives
#      HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\AutoplayHandlers\DisableAutoplay = 1
#   2) AutoRun off   -> HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer\NoDriveTypeAutoRun = 0xFF
#   3) Hide the cable's two volumes (CD "MacKMLink" + removable disk "Virtual Link") in This PC:
#      HKCU\...\Policies\Explorer\NoDrives |= bit for each cable drive letter.
#      IMPORTANT: the drive letters stay assigned.  NoDrives is a *shell view* filter only,
#      so the agent still opens \\.\H: and the transfer path is untouched.
#   4) Hide the "Safely Remove Hardware" tray icon (the hidden-icons flyout no longer lists
#      the cable): HKCU\...\Applets\SysTray\Services &= ~0x02.  -KeepSafeRemove skips this.
#   5) Remove vendor (MacKMLink / LinkEngKM / LEWD / SKLoader) tray registrations from
#      HKCU\Control Panel\NotifyIconSettings.  The key is backed up before deletion.
#
# Windows 11 24H2 makes HKCU\...\Policies read-only for normal users, so steps 2-3 need an
# elevated run.  The script applies the per-user parts first and then requests elevation
# once (UAC).  If UAC is cancelled, the per-user parts are still applied and the drives stay
# visible; re-run from an elevated PowerShell to finish.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File quiet-cable.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File quiet-cable.ps1 -Undo
#   powershell -NoProfile -ExecutionPolicy Bypass -File quiet-cable.ps1 -Status
#   ... -NoElevate        # per-user only (AutoPlay/tray), leave This PC alone
#   ... -KeepSafeRemove   # keep the "Safely Remove Hardware" tray icon
#   ... -RestartExplorer  # force an Explorer restart after applying (closes open windows)

param(
    [switch]$Undo,
    [switch]$NoElevate,
    [switch]$KeepSafeRemove,
    [switch]$RestartExplorer,
    [switch]$Elevated,
    [switch]$Status
)

$ErrorActionPreference = 'Stop'

$ScriptPath = $PSCommandPath
if (-not $ScriptPath) { $ScriptPath = $MyInvocation.MyCommand.Path }

$LogFile     = 'C:\Users\Public\quiet-cable.log'
$StateFile   = 'C:\Users\Public\otilink-quiet-state.json'
$TrayBackup  = 'C:\Users\Public\otilink-quiet-tray.reg'
$PolicyKey   = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer'
$AutoPlayKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\AutoplayHandlers'
$SysTrayKey  = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Applets\SysTray'
$NotifyKey   = 'HKCU:\Control Panel\NotifyIconSettings'

function Log([string]$m) {
    $line = ('{0} {1}' -f (Get-Date).ToString('HH:mm:ss'), $m)
    Write-Host $line
    try { Add-Content -LiteralPath $LogFile -Value $line -Encoding UTF8 } catch { }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-RegValue([string]$path, [string]$name) {
    $p = Get-ItemProperty -LiteralPath $path -Name $name -ErrorAction SilentlyContinue
    if ($null -eq $p) { return $null }
    return $p.$name
}

# Cable volumes, by device identity (not by label: the 1 MB FAT volume has no label).
function Get-CableVolumes {
    $out = New-Object System.Collections.ArrayList
    Get-CimInstance Win32_CDROMDrive -ErrorAction SilentlyContinue | Where-Object {
        $_.PNPDeviceID -match 'VEN_&PROD_ANDROID\+MAC' -or $_.VolumeName -match '^MacKMLink$'
    } | ForEach-Object {
        if ($_.Drive -match '^([A-Za-z]):') {
            [void]$out.Add([pscustomobject]@{ Letter = $Matches[1].ToUpper(); Kind = 'CD'; Label = [string]$_.VolumeName })
        }
    }
    Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue | Where-Object {
        $_.Model -match 'Virtual Link' -or $_.PNPDeviceID -match 'PROD_VIRTUAL_LINK'
    } | ForEach-Object {
        $disk = $_
        foreach ($part in @(Get-CimAssociatedInstance -InputObject $disk -Association Win32_DiskDriveToDiskPartition -ErrorAction SilentlyContinue)) {
            foreach ($ld in @(Get-CimAssociatedInstance -InputObject $part -Association Win32_LogicalDiskToPartition -ErrorAction SilentlyContinue)) {
                if ($ld.DeviceID -match '^([A-Za-z]):') {
                    [void]$out.Add([pscustomobject]@{ Letter = $Matches[1].ToUpper(); Kind = 'Disk'; Label = [string]$ld.VolumeName })
                }
            }
        }
    }
    return @($out | Sort-Object Letter -Unique)
}

function Get-Mask([object[]]$vols) {
    $m = 0
    foreach ($v in $vols) {
        $bit = [int][char]$v.Letter - [int][char]'A'
        if ($bit -ge 0 -and $bit -le 25) { $m = $m -bor (1 -shl $bit) }
    }
    return $m
}

function Save-State {
    if (Test-Path -LiteralPath $StateFile) { return }
    $st = [ordered]@{
        NoDrives           = Get-RegValue $PolicyKey 'NoDrives'
        NoDriveTypeAutoRun = Get-RegValue $PolicyKey 'NoDriveTypeAutoRun'
        DisableAutoplay    = Get-RegValue $AutoPlayKey 'DisableAutoplay'
        SysTrayServices    = Get-RegValue $SysTrayKey 'Services'
        AppliedAt          = (Get-Date).ToString('s')
    }
    ($st | ConvertTo-Json) | Set-Content -LiteralPath $StateFile -Encoding UTF8
    Log "state saved -> $StateFile"
}

function Apply-UserParts {
    if (-not (Test-Path -LiteralPath $AutoPlayKey)) { New-Item -Path $AutoPlayKey -Force | Out-Null }
    Set-ItemProperty -LiteralPath $AutoPlayKey -Name 'DisableAutoplay' -Value 1 -Type DWord
    Log 'autoplay: DisableAutoplay=1'

    if (-not $KeepSafeRemove) {
        if (-not (Test-Path -LiteralPath $SysTrayKey)) { New-Item -Path $SysTrayKey -Force | Out-Null }
        $svc = Get-RegValue $SysTrayKey 'Services'
        if ($null -eq $svc) { $svc = 31 }
        $new = ([int]$svc) -band (-bnot 2)
        Set-ItemProperty -LiteralPath $SysTrayKey -Name 'Services' -Value $new -Type DWord
        Log ("tray: Services {0} -> {1} (Safely Remove icon off)" -f $svc, $new)
    } else {
        Log 'tray: Safely Remove icon left as is (-KeepSafeRemove)'
    }

    if (Test-Path -LiteralPath $NotifyKey) {
        $hits = @(Get-ChildItem -LiteralPath $NotifyKey -ErrorAction SilentlyContinue | Where-Object {
            $p = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
            ($null -ne $p) -and ($p.ExecutablePath -match '\\OTi\\|MacKMLink|LinkEngKM|LEWD|SKLoader')
        })
        if ($hits.Count -gt 0) {
            if (-not (Test-Path -LiteralPath $TrayBackup)) {
                & reg.exe export 'HKCU\Control Panel\NotifyIconSettings' $TrayBackup /y | Out-Null
                Log "tray: backup -> $TrayBackup"
            }
            foreach ($h in $hits) {
                Log ("tray: remove vendor icon setting " + $h.PSChildName)
                Remove-Item -LiteralPath $h.PSPath -Recurse -Force -ErrorAction SilentlyContinue
            }
        } else {
            Log 'tray: no vendor icon settings found'
        }
    }
}

function Apply-Policy {
    $vols = @(Get-CableVolumes)
    if ($vols.Count -eq 0) {
        Log 'policy: cable not present, NoDrives not changed'
        return $false
    }
    $mask = Get-Mask $vols
    $cur = Get-RegValue $PolicyKey 'NoDrives'
    if ($null -eq $cur) { $cur = 0 }
    $new = ([int]$cur) -bor $mask
    if (-not (Test-Path -LiteralPath $PolicyKey)) { New-Item -Path $PolicyKey -Force | Out-Null }
    Set-ItemProperty -LiteralPath $PolicyKey -Name 'NoDrives' -Value $new -Type DWord
    Set-ItemProperty -LiteralPath $PolicyKey -Name 'NoDriveTypeAutoRun' -Value 0xFF -Type DWord
    Log ("policy: cable letters {0} -> NoDrives 0x{1:x} (was 0x{2:x}); NoDriveTypeAutoRun=0xFF" -f (($vols | ForEach-Object { $_.Letter }) -join ','), $new, [int]$cur)
    return $true
}

function Update-Shell {
    if (-not ('QuietCableShell' -as [type])) {
        Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class QuietCableShell {
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, IntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out IntPtr lpdwResult);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern void SHChangeNotify(int eventId, uint flags, string item1, IntPtr item2);
}
"@
    }
    $r = [IntPtr]::Zero
    [void][QuietCableShell]::SendMessageTimeout([IntPtr]0xFFFF, 0x1A, [IntPtr]::Zero, 'Policy', 2, 3000, [ref]$r)
    foreach ($v in @(Get-CableVolumes)) {
        [QuietCableShell]::SHChangeNotify(0x80, 0x0005, ($v.Letter + ':\'), [IntPtr]::Zero)
    }
    if (Test-Path -LiteralPath "$env:SystemRoot\System32\systray.exe") {
        & "$env:SystemRoot\System32\systray.exe" 2>$null
    }
    Log 'shell: policy broadcast + drive-removed notifications sent'
}

function Restart-Explorer {
    Log 'explorer: restarting'
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Start-Process explorer.exe
}

function Undo-All {
    if (-not (Test-Path -LiteralPath $StateFile)) {
        Log 'undo: no state file; nothing recorded to restore'
        return
    }
    $st = Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json

    if ($null -eq $st.DisableAutoplay) {
        Remove-ItemProperty -LiteralPath $AutoPlayKey -Name 'DisableAutoplay' -ErrorAction SilentlyContinue
    } else {
        Set-ItemProperty -LiteralPath $AutoPlayKey -Name 'DisableAutoplay' -Value ([int]$st.DisableAutoplay) -Type DWord
    }
    if ($null -eq $st.SysTrayServices) {
        Remove-ItemProperty -LiteralPath $SysTrayKey -Name 'Services' -ErrorAction SilentlyContinue
    } else {
        Set-ItemProperty -LiteralPath $SysTrayKey -Name 'Services' -Value ([int]$st.SysTrayServices) -Type DWord
    }
    Log 'undo: autoplay / tray restored'

    $policyDone = $false
    if (Test-Admin) {
        if ($null -eq $st.NoDrives) {
            Remove-ItemProperty -LiteralPath $PolicyKey -Name 'NoDrives' -ErrorAction SilentlyContinue
        } else {
            Set-ItemProperty -LiteralPath $PolicyKey -Name 'NoDrives' -Value ([int]$st.NoDrives) -Type DWord
        }
        if ($null -eq $st.NoDriveTypeAutoRun) {
            Remove-ItemProperty -LiteralPath $PolicyKey -Name 'NoDriveTypeAutoRun' -ErrorAction SilentlyContinue
        } else {
            Set-ItemProperty -LiteralPath $PolicyKey -Name 'NoDriveTypeAutoRun' -Value ([int]$st.NoDriveTypeAutoRun) -Type DWord
        }
        Log 'undo: policy NoDrives / NoDriveTypeAutoRun restored'
        Update-Shell
        $policyDone = $true
    } else {
        Log 'undo: policy restore needs admin (state file kept)'
        $script:NeedAdmin = $true
    }

    if (Test-Path -LiteralPath $TrayBackup) {
        & reg.exe import $TrayBackup 2>$null | Out-Null
        Log 'undo: tray settings imported from backup'
    }
    if ($policyDone) {
        Remove-Item -LiteralPath $StateFile -Force -ErrorAction SilentlyContinue
        Log 'undo: done (re-login if the tray/This PC does not refresh)'
    } else {
        Log 'undo: per-user parts restored; run again elevated to finish the drive policy'
    }
}

function Show-Status {
    $admin = Test-Admin
    $vols = @(Get-CableVolumes)
    $nd = Get-RegValue $PolicyKey 'NoDrives'
    $ndar = Get-RegValue $PolicyKey 'NoDriveTypeAutoRun'
    $ap = Get-RegValue $AutoPlayKey 'DisableAutoplay'
    $svc = Get-RegValue $SysTrayKey 'Services'
    $hits = 0
    if (Test-Path -LiteralPath $NotifyKey) {
        $hits = @(Get-ChildItem -LiteralPath $NotifyKey -ErrorAction SilentlyContinue | Where-Object {
            $p = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
            ($null -ne $p) -and ($p.ExecutablePath -match '\\OTi\\|MacKMLink|LinkEngKM|LEWD|SKLoader')
        }).Count
    }
    Write-Output ("admin                 : {0}" -f $admin)
    Write-Output ("cable volumes         : {0}" -f (($vols | ForEach-Object { "$($_.Letter):($($_.Kind))" }) -join ' '))
    Write-Output ("NoDrives              : {0}" -f $(if ($null -eq $nd) { '(absent)' } else { ('0x{0:x}' -f [int]$nd) }))
    Write-Output ("NoDriveTypeAutoRun    : {0}" -f $(if ($null -eq $ndar) { '(absent)' } else { ('0x{0:x}' -f [int]$ndar) }))
    Write-Output ("DisableAutoplay       : {0}" -f $(if ($null -eq $ap) { '(absent)' } else { $ap }))
    Write-Output ("SysTray Services      : {0}" -f $(if ($null -eq $svc) { '(absent)' } else { $svc }))
    Write-Output ("vendor tray settings  : {0}" -f $hits)
}

# ---------------------------------------------------------------------------
function Invoke-Elevated([string[]]$extra) {
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $ScriptPath), '-Elevated') + $extra
    Log 'requesting elevation (UAC) ...'
    try {
        $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argv -Verb RunAs -Wait -PassThru
        Log ("elevated child exit={0}" -f $p.ExitCode)
        return ($p.ExitCode -eq 0)
    } catch {
        Log ("elevation failed/cancelled: {0}" -f $_.Exception.Message)
        return $false
    }
}

Log ("=== quiet-cable start (admin={0} elevated={1} undo={2}) ===" -f (Test-Admin), $Elevated, $Undo)

if ($Status) {
    Show-Status
    exit 0
}

$script:NeedAdmin = $false

if (-not $Undo) {
    Save-State
    Apply-UserParts
    if (Test-Admin) {
        [void](Apply-Policy)
        Update-Shell
        if ($RestartExplorer) { Restart-Explorer }
    } elseif ($NoElevate) {
        Log 'policy: skipped (-NoElevate); cable drives stay visible'
        $script:NeedAdmin = $true
    } elseif (-not $Elevated) {
        $argv = @()
        if ($KeepSafeRemove) { $argv += '-KeepSafeRemove' }
        if ($RestartExplorer) { $argv += '-RestartExplorer' }
        if (-not (Invoke-Elevated $argv)) {
            Log 'drives stay visible; run from an elevated PowerShell to finish'
            $script:NeedAdmin = $true
        }
    } else {
        Log 'policy: not admin even in -Elevated run; drives stay visible'
        $script:NeedAdmin = $true
    }
} else {
    if (-not (Test-Admin) -and -not $NoElevate -and -not $Elevated) {
        if (Invoke-Elevated @('-Undo')) { exit 0 }
        Log 'undo: elevation failed; restoring per-user parts only'
    }
    Undo-All
}

if ($script:NeedAdmin) { exit 2 }
exit 0
