# clipfiles-diag.ps1 -- CF_HDROP 读取诊断（把 ClipFiles 的每一步都摊开，不吞异常）
#
# 为什么需要它：agent 里 [ClipFiles]::Get() 失败时被 catch 吞掉，只表现为
# "HDROP 命中：0 个文件"，看不出是 OpenClipboard / GetClipboardData / DragQueryFileW 哪一步挂的。
# 用法（Windows，非管理员即可）：
#   powershell -ExecutionPolicy Bypass -File clipfiles-diag.ps1
#   powershell -ExecutionPolicy Bypass -File clipfiles-diag.ps1 -SetPath C:\Windows\win.ini
param([string]$SetPath = '')

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class CFD {
    const uint CF_HDROP = 15;
    [DllImport("user32.dll", SetLastError=true)] static extern bool OpenClipboard(IntPtr h);
    [DllImport("user32.dll", SetLastError=true)] static extern bool CloseClipboard();
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr GetClipboardData(uint fmt);
    [DllImport("user32.dll", SetLastError=true)] static extern bool IsClipboardFormatAvailable(uint fmt);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern uint DragQueryFileW(IntPtr h, uint i, StringBuilder sb, uint cch);

    public static string Diag() {
        StringBuilder r = new StringBuilder();
        r.Append("IsClipboardFormatAvailable(CF_HDROP)=").Append(IsClipboardFormatAvailable(CF_HDROP)).Append("\n");
        bool ok = OpenClipboard(IntPtr.Zero);
        r.Append("OpenClipboard=").Append(ok).Append(" err=").Append(Marshal.GetLastWin32Error()).Append("\n");
        if (!ok) return r.ToString();
        try {
            IntPtr h = GetClipboardData(CF_HDROP);
            r.Append("GetClipboardData=0x").Append(h.ToInt64().ToString("x")).Append(" err=").Append(Marshal.GetLastWin32Error()).Append("\n");
            if (h == IntPtr.Zero) return r.ToString();
            uint n = DragQueryFileW(h, 0xFFFFFFFFu, null, 0);
            r.Append("DragQueryFileW(count)=").Append(n).Append(" err=").Append(Marshal.GetLastWin32Error()).Append("\n");
            for (uint i = 0; i < n; i++) {
                StringBuilder sb = new StringBuilder(1024);
                uint len = DragQueryFileW(h, i, sb, 1024);
                r.Append("  [").Append(i).Append("] len=").Append(len).Append(" '").Append(sb.ToString()).Append("'\n");
            }
        } catch (Exception ex) {
            r.Append("EXCEPTION: ").Append(ex.GetType().Name).Append(": ").Append(ex.Message).Append("\n");
        } finally { CloseClipboard(); }
        return r.ToString();
    }
}
'@

if ($SetPath -ne '') {
    Set-Clipboard -Path $SetPath
    Start-Sleep -Milliseconds 300
    Write-Host "已把 '$SetPath' 放进剪贴板（本次进程仍存活，避免 owner 退出导致数据消失）"
}
Write-Host "--- CFD::Diag() ---"
Write-Host ([CFD]::Diag())
Write-Host "--- 对照：PowerShell 自带 cmdlet ---"
Write-Host ("Get-Clipboard -Format FileDropList: " + ((Get-Clipboard -Format FileDropList -ErrorAction SilentlyContinue) -join ';'))
