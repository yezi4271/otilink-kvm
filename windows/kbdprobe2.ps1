# kbdprobe2.ps1 -- independent WH_KEYBOARD_LL probe WITH vk logging.
# Tells apart: "hook chain blind" vs "our agent swallows" vs "device input bypasses hooks".
param([int]$Seconds = 6)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class KP2 {
    public delegate IntPtr Proc(int nCode, IntPtr w, IntPtr l);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SetWindowsHookEx(int id, Proc p, IntPtr h, uint t);
    [DllImport("user32.dll")] public static extern IntPtr CallNextHookEx(IntPtr h, int n, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PeekMessage(out MSG m, IntPtr h, uint a, uint b, uint r);
    [DllImport("user32.dll")] public static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] public static extern IntPtr DispatchMessage(ref MSG m);
    [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint msg; public IntPtr w, l; public uint t; public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct KBDLLHOOKSTRUCT { public uint vkCode, scanCode, flags, time; public IntPtr dwExtraInfo; }
    public static int Seen = 0;
    public static StringBuilder Log = new StringBuilder();
    static Proc p;
    static IntPtr Hook(int n, IntPtr w, IntPtr l) {
        if (n >= 0) {
            var k = (KBDLLHOOKSTRUCT)Marshal.PtrToStructure(l, typeof(KBDLLHOOKSTRUCT));
            Seen++;
            Log.Append("vk=0x" + k.vkCode.ToString("X2") + "(w=" + w.ToString() + ",inj=" + ((k.flags & 0x10) != 0 ? 1 : 0) + ") ");
        }
        return CallNextHookEx(IntPtr.Zero, n, w, l);
    }
    public static string Run(int ms) {
        p = Hook;
        IntPtr h = SetWindowsHookEx(13, p, IntPtr.Zero, 0);
        string res = "hook=" + (h != IntPtr.Zero ? "ok" : "FAIL") + "  ";
        var sw = System.Diagnostics.Stopwatch.StartNew();
        MSG m;
        while (sw.ElapsedMilliseconds < ms) {
            while (PeekMessage(out m, IntPtr.Zero, 0, 0, 1)) { TranslateMessage(ref m); DispatchMessage(ref m); }
            System.Threading.Thread.Sleep(4);
        }
        return res + "keys_seen=" + Seen + "  " + Log.ToString();
    }
}
'@
Write-Output ([KP2]::Run($Seconds * 1000))
