# setcur.ps1 -- place the Windows cursor at a known point (ASCII only).
# DPI aware so coordinates match the agent's physical-pixel space.
param([int]$X = 1200, [int]$Y = 500)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SC {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static void Go(int x, int y) { SetProcessDPIAware(); SetCursorPos(x, y); }
}
'@
[SC]::Go($X, $Y)
Write-Output ("cursor set to " + $X + "," + $Y)
