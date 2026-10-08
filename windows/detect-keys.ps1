# detect-keys.ps1 -- poll the SESSION-wide async key state (not hooks) and print every VK that
# goes down. Used to tell "the cable's emulated keyboard delivered nothing" apart from
# "our low-level hook ignored it".
param([int]$Seconds = 8)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class KS {
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
}
'@
$seen = @{}
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    for ($vk = 1; $vk -lt 255; $vk++) {
        $s = [KS]::GetAsyncKeyState($vk)
        if (($s -band 0x8001) -ne 0) {
            if (-not $seen.ContainsKey($vk)) {
                $seen[$vk] = $true
                Write-Output ("DOWN vk=0x{0:X2} ({1})" -f $vk, $vk)
            }
        }
    }
    Start-Sleep -Milliseconds 4
}
Write-Output ("--- done, distinct keys seen: " + $seen.Count + " ---")
