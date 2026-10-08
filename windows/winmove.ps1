# winmove.ps1 -- synthesize relative mouse motion on Windows (ASCII only).
param([int]$Dx = 25, [int]$Dy = 0, [int]$Count = 10, [int]$GapMs = 60)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WMv {
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    public static void Move(int dx, int dy) { mouse_event(0x0001, dx, dy, 0, IntPtr.Zero); }
}
'@
for ($i = 0; $i -lt $Count; $i++) {
  [WMv]::Move($Dx, $Dy)
  Start-Sleep -Milliseconds $GapMs
}
Write-Output ("sent " + $Count + " x (dx=" + $Dx + ",dy=" + $Dy + ")")
