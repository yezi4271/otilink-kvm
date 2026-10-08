# Synthesize a left click (and a right click) on Windows for the self test.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CK {
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    public static void Left()  { mouse_event(0x0002,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(80); mouse_event(0x0004,0,0,0,IntPtr.Zero); }
    public static void Right() { mouse_event(0x0008,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(80); mouse_event(0x0010,0,0,0,IntPtr.Zero); }
}
'@
Write-Output "clicking left ..."; [CK]::Left(); Start-Sleep -Milliseconds 600
Write-Output "clicking right ..."; [CK]::Right()
Write-Output "done"
