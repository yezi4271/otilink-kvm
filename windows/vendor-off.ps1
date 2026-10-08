# vendor-off.ps1 - stop OTi GO! Suite / MacKMLink / LinkEngine KM modules and disable their autostart.
# ASCII only (PS 5.1 reads ANSI when no BOM). Run elevated.
$ErrorActionPreference = 'Continue'
$log = 'C:\Users\Public\vendor-off.log'
function L($m) {
  $t = (Get-Date).ToString('HH:mm:ss')
  $line = "$t $m"
  Write-Host $line
  Add-Content -Path $log -Value $line -EA SilentlyContinue
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
L "=== vendor-off start (elevated=$isAdmin) ==="

# ---- pattern of vendor binaries -------------------------------------------
$rxPath = 'MacKMLink|LinkEngKM|LEWD|OTi\\|GO! ?Suite|GOSuite|LinkEngine'

# ---- 1) kill vendor processes (watchdog LEWD first) -----------------------
function KillVendor([string]$why) {
  $n = 0
  $order = @('LEWD', 'LinkEngKM', 'MacKMLink')
  foreach ($nm in $order) {
    Get-Process -Name $nm -EA SilentlyContinue | ForEach-Object {
      try { Stop-Process -Id $_.Id -Force -EA Stop; L "  kill $($_.ProcessName) pid=$($_.Id)"; $n++ }
      catch { L "  kill FAIL $($_.ProcessName) pid=$($_.Id): $($_.Exception.Message)" }
    }
  }
  Get-CimInstance Win32_Process -EA SilentlyContinue |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath -match $rxPath -and $_.Name -notmatch 'otiagent2' } |
    ForEach-Object {
      try { Stop-Process -Id $_.ProcessId -Force -EA Stop; L "  kill(path) $($_.Name) pid=$($_.ProcessId)"; $n++ } catch { }
    }
  L "$why -> killed $n"
  return $n
}

KillVendor 'pass1'

# ---- 2) disable autostart -------------------------------------------------
$runKeys = @(
  'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run',
  'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run',
  'HKLM:\Software\Wow6432Node\Microsoft\Windows\CurrentVersion\Run',
  'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
)
foreach ($k in $runKeys) {
  if (-not (Test-Path $k)) { continue }
  $p = Get-ItemProperty -Path $k -EA SilentlyContinue
  if (-not $p) { continue }
  $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object {
    if ("$($_.Value)" -match $rxPath) {
      L "  del Run $k :: $($_.Name) = $($_.Value)"
      Remove-ItemProperty -Path $k -Name $_.Name -Force -EA SilentlyContinue
    }
  }
}

# startup folder shortcuts
$startups = @(
  (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup'),
  (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Startup')
)
foreach ($d in $startups) {
  Get-ChildItem -Path $d -EA SilentlyContinue | Where-Object { $_.Name -match 'OTi|MacKM|Link|GO' } | ForEach-Object {
    L "  del startup $($_.FullName)"
    Remove-Item -Path $_.FullName -Force -EA SilentlyContinue
  }
}

# scheduled tasks
Get-ScheduledTask -EA SilentlyContinue | Where-Object { $_.TaskName -match 'OTi|MacKM|LinkEngine|GO!|GOSuite' } | ForEach-Object {
  L "  disable task $($_.TaskName)"
  Disable-ScheduledTask -TaskName $_.TaskName -EA SilentlyContinue | Out-Null
}

# services
Get-CimInstance Win32_Service -EA SilentlyContinue | Where-Object { $_.PathName -match $rxPath } | ForEach-Object {
  L "  stop+disable service $($_.Name)"
  Stop-Service -Name $_.Name -Force -EA SilentlyContinue
  Set-Service -Name $_.Name -StartupType Disabled -EA SilentlyContinue
}

# ---- 3) respawn guard (20s) ----------------------------------------------
for ($i = 0; $i -lt 10; $i++) {
  Start-Sleep -Seconds 2
  $alive = @(Get-Process -Name LEWD, LinkEngKM, MacKMLink -EA SilentlyContinue)
  if ($alive.Count -gt 0) { L "guard: respawn detected ($($alive.Count))"; KillVendor 'guard' | Out-Null }
}
$left = @(Get-Process -Name LEWD, LinkEngKM, MacKMLink -EA SilentlyContinue)
L "=== vendor-off done, remaining=$($left.Count) ==="
foreach ($p in $left) { L "  still alive: $($p.ProcessName) pid=$($p.Id)" }
