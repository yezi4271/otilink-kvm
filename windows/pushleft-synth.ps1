# pushleft-synth.ps1 -- pure RELATIVE leftward push, mimics a real "push into the left edge" gesture
# while the agent is in REMOTE (the local cursor is swallowed, the peer cursor moves left).
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PLS {
    [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static void Init() { SetProcessDPIAware(); }
    public static int[] Get() { P p; GetCursorPos(out p); return new int[] { p.x, p.y }; }
}
'@
[PLS]::Init()
$b = [PLS]::Get()
Write-Output ("local cursor before: " + $b[0] + "," + $b[1])
for ($i = 0; $i -lt 120; $i++) {
    [PLS]::mouse_event(0x0001, -20, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 20
}
$a = [PLS]::Get()
Write-Output ("local cursor after:  " + $a[0] + "," + $a[1] + "   (REMOTE 时本机光标应几乎不动)")
