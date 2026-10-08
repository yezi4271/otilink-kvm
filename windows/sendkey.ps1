# sendkey.ps1 -- synthesize one keystroke (down+up) with SendInput, for validating hook instrumentation.
param([int]$Vk = 0x78)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SK {
    [StructLayout(LayoutKind.Sequential)] public struct INPUT {
        public uint type; public ushort wVk; public ushort wScan; public uint dwFlags;
        public uint time; public IntPtr dwExtraInfo;
    }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int size);
    public static void Key(ushort vk, bool up) {
        INPUT[] i = new INPUT[1];
        i[0].type = 1; i[0].wVk = vk; i[0].dwFlags = up ? 2u : 0u;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
[SK]::Key([uint16]$Vk, $false)
Start-Sleep -Milliseconds 60
[SK]::Key([uint16]$Vk, $true)
Write-Output ("sent vk=0x{0:X2}" -f $Vk)
