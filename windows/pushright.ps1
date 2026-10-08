# pushright.ps1 -- reproduce the real hand-over gesture (ASCII only: PowerShell 5.1
# reads .ps1 as ANSI, so non-ASCII comments break the parser).
#
# Pushing the cursor past the RIGHT edge of the virtual desktop CLAMPS it; afterwards WM_MOUSEMOVE keeps
# arriving with dx == 0. A test that only checks "did x decrease" never exercises
# that case -- which is exactly the case the user hits. So: walk to past the right edge of
# the VIRTUAL desktop (clamped), then keep pushing outward.
#
# Must be DPI aware so its coordinates match the agent's.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PU {
    [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static void Init() { SetProcessDPIAware(); }
    public static int[] Get() { P p; GetCursorPos(out p); return new int[] { p.x, p.y }; }
    public static int RightEdge() { return GetSystemMetrics(76) + GetSystemMetrics(78) - 1; }
    public static int Width()    { return GetSystemMetrics(78); }
}
'@

[PU]::Init()
$start = [PU]::Get()
Write-Output ("start: " + $start[0] + "," + $start[1])
$rx = [PU]::RightEdge()
Write-Output ("virtual right=" + $rx)

$y = 500
[void][PU]::SetCursorPos(400, $y)
Start-Sleep -Milliseconds 120
for ($i = 0; $i -le 30; $i++) {
    $x = [int](400 + ($rx + 3 - 400) * $i / 30)
    [void][PU]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 35
}
$now = [PU]::Get()
Write-Output ("walked to: " + $now[0] + "," + $now[1])

for ($i = 0; $i -lt 45; $i++) {
    [PU]::mouse_event(0x0001, 14, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 25
}
$now2 = [PU]::Get()
Write-Output ("clamped push done, cursor: " + $now2[0] + "," + $now2[1])
[void][PU]::SetCursorPos($start[0], $start[1])
Write-Output "restored"
