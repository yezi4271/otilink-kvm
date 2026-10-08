# curlog.ps1 -- log the Windows cursor position at 50 ms intervals (ASCII only).
param([int]$Secs = 4)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CurLog {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    public static int[] Get() {
        POINT p; GetCursorPos(out p);
        return new int[] { p.X, p.Y };
    }
}
'@
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Secs) {
  $p = [CurLog]::Get()
  $ms = [int](((Get-Date) - $t0).TotalMilliseconds)
  Write-Output ("{0,6} ms  {1,6} {2,6}" -f $ms, $p[0], $p[1])
  Start-Sleep -Milliseconds 50
}
