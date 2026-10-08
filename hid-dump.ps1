# hid-dump.ps1 —— 用 Windows HID API 读指定 VID 的 HID 能力（无需管理员权限）
#
# 用法（在 WSL 里）：
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File hid-dump.ps1
# 可通过环境变量覆盖 VID：$env:HID_VID = '0EA0'
#
# 它回答三个关键问题：
#   1) 每个 HID 顶层的 UsagePage/Usage（0x01=通用桌面即键盘鼠标，0xFF00+=厂商自定义）
#   2) OutputReportByteLength > 0 ？—— 有输出报文 = 存在"发"的通道
#   3) FeatureReportByteLength > 0 ？—— 有特性报文 = 另一种可能的私有通道

$ErrorActionPreference = 'Stop'
$vid = if ($env:HID_VID) { $env:HID_VID } else { '0EA0' }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class HidNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct HIDD_ATTRIBUTES {
        public int Size; public ushort VendorID; public ushort ProductID; public ushort VersionNumber;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct HIDP_CAPS
    {
        public ushort Usage; public ushort UsagePage;
        public ushort InputReportByteLength; public ushort OutputReportByteLength; public ushort FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps; public ushort NumberInputValueCaps; public ushort NumberInputDataIndices;
        public ushort NumberOutputButtonCaps; public ushort NumberOutputValueCaps; public ushort NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps; public ushort NumberFeatureValueCaps; public ushort NumberFeatureDataIndices;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct HIDP_VALUE_CAPS
    {
        public ushort UsagePage; public byte ReportID; public byte IsAlias; public ushort BitField;
        public ushort LinkCollection; public ushort LinkUsage; public ushort LinkUsagePage;
        public byte IsRange; public byte IsStringRange; public byte IsDesignatorRange;
        public byte IsAbsolute; public byte HasNull; public byte Reserved;
        public ushort BitSize; public ushort ReportCount;
        public ushort R2a, R2b, R2c, R2d, R2e;
        public uint UnitsExp; public uint Units;
        public int LogicalMin; public int LogicalMax; public int PhysicalMin; public int PhysicalMax;
        // union：Range 视图（NotRange 时 Usage/Reserved1 等落在同一偏移）
        public ushort UsageMin; public ushort UsageMax; public ushort StringMin; public ushort StringMax;
        public ushort DesignatorMin; public ushort DesignatorMax; public ushort DataIndexMin; public ushort DataIndexMax;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);

    [DllImport("hid.dll", SetLastError = true)]
    public static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr pp);

    [DllImport("hid.dll")]
    public static extern bool HidD_FreePreparsedData(IntPtr pp);

    [DllImport("hid.dll")]
    public static extern int HidP_GetCaps(IntPtr pp, ref HIDP_CAPS caps);

    [DllImport("hid.dll")]
    public static extern int HidP_GetValueCaps(int type, [Out] HIDP_VALUE_CAPS[] caps, ref int len, IntPtr pp);

    [DllImport("hid.dll")]
    public static extern bool HidD_GetAttributes(IntPtr h, ref HIDD_ATTRIBUTES a);
}
'@

$HidGuid = '{4d1e55b2-f16f-11cf-88cb-001111000030}'
$INVALID = [IntPtr](-1)

function Dump-Hid([string]$InstanceId) {
    # HID 设备接口路径可由 PnP 实例 ID 直接构造（这是常用技巧）：
    #   HID\VID_xxxx&PID_xxxx&MI_yy\8&zzz&0&0000
    # -> \\?\hid#vid_xxxx&pid_xxxx&mi_yy#8&zzz&0&0000#{4d1e55b2-...}
    $link = '\\?\' + ($InstanceId.ToLower() -replace '\\', '#') + '#' + $HidGuid
    Write-Host "=== $InstanceId"
    Write-Host "    path: $link"

    # access=0 (仅查询), share=3, OPEN_EXISTING
    $h = [HidNative]::CreateFileW($link, 0, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq $INVALID) {
        Write-Host ("    CreateFile 失败 err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        return
    }

    $attr = New-Object HidNative+HIDD_ATTRIBUTES
    $attr.Size = [Runtime.InteropServices.Marshal]::SizeOf($attr)
    if ([HidNative]::HidD_GetAttributes($h, [ref]$attr)) {
        Write-Host ("    VID:PID = {0:x4}:{1:x4}  ver={2:x4}" -f $attr.VendorID, $attr.ProductID, $attr.VersionNumber)
    }

    $pp = [IntPtr]::Zero
    if (-not [HidNative]::HidD_GetPreparsedData($h, [ref]$pp)) {
        Write-Host "    HidD_GetPreparsedData 失败"
        [void][HidNative]::CloseHandle($h)
        return
    }

    $caps = New-Object HidNative+HIDP_CAPS
    [void][HidNative]::HidP_GetCaps($pp, [ref]$caps)
    Write-Host ("    顶层 UsagePage=0x{0:x4} Usage=0x{1:x4}" -f $caps.UsagePage, $caps.Usage)
    Write-Host ("    报文字节长度: Input={0}  Output={1}  Feature={2}" -f `
        $caps.InputReportByteLength, $caps.OutputReportByteLength, $caps.FeatureReportByteLength)
    Write-Host ("    ValueCaps: In={0} Out={1} Feat={2} | ButtonCaps: In={3} Out={4} Feat={5}" -f `
        $caps.NumberInputValueCaps, $caps.NumberOutputValueCaps, $caps.NumberFeatureValueCaps, `
        $caps.NumberInputButtonCaps, $caps.NumberOutputButtonCaps, $caps.NumberFeatureButtonCaps)

    $kinds = @(
        @{ N = 'Input';   T = 0; C = $caps.NumberInputValueCaps },
        @{ N = 'Output';  T = 1; C = $caps.NumberOutputValueCaps },
        @{ N = 'Feature'; T = 2; C = $caps.NumberFeatureValueCaps }
    )
    foreach ($k in $kinds) {
        if ($k.C -le 0) { continue }
        $arr = New-Object 'HidNative+HIDP_VALUE_CAPS[]' $k.C
        $len = $k.C
        [void][HidNative]::HidP_GetValueCaps($k.T, $arr, [ref]$len, $pp)
        for ($i = 0; $i -lt $len; $i++) {
            $c = $arr[$i]
            $usage = if ($c.IsRange) { '0x{0:x4}-0x{1:x4}' -f $c.UsageMin, $c.UsageMax } else { '0x{0:x4}' -f $c.UsageMin }
            Write-Host ("      [{0}] UsagePage=0x{1:x4} ReportID={2} BitSize={3} Count={4} Logical=[{5},{6}] Usage={7}" -f `
                $k.N, $c.UsagePage, $c.ReportID, $c.BitSize, $c.ReportCount, $c.LogicalMin, $c.LogicalMax, $usage)
        }
    }

    [void][HidNative]::HidD_FreePreparsedData($pp)
    [void][HidNative]::CloseHandle($h)
}

$targets = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'HID\*' -and $_.InstanceId -match $vid }
if (-not $targets) { Write-Host "没找到 VID=$vid 的 HID 顶层集合"; exit 1 }
$targets | ForEach-Object { Dump-Hid $_.InstanceId }
