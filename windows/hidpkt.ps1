# hidpkt.ps1 -- send raw vendor HID packets (SCSI CDB D9 <type> + 12 payload bytes + 'OT')
# over the transfer cable, via SPTI. Used to calibrate the HID packet -> HID report mapping.
# ASCII only (Windows PowerShell 5.1 reads .ps1 as ANSI).
param(
  [Parameter(Mandatory=$true)][string]$Payload,   # 12 bytes as hex, e.g. "0002030405060708090a0b0c"
  [int]$Type = 1,                                  # 1 -> CDB[1]=0x33, 2 -> 0x34, 3 -> 0x36
  [int]$Count = 1,
  [int]$GapMs = 120,
  [string]$Device = '\\.\H:'
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class H_Spti {
    [StructLayout(LayoutKind.Sequential)]
    struct SCSI_PASS_THROUGH_DIRECT {
        public ushort Length;
        public byte ScsiStatus;
        public byte PathId;
        public byte TargetId;
        public byte Lun;
        public byte CdbLength;
        public byte SenseInfoLength;
        public byte DataIn;
        public uint DataTransferLength;
        public uint TimeOutValue;
        public IntPtr DataBuffer;
        public uint SenseInfoOffset;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public byte[] Cdb;
    }
    const uint IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x4D014;
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec,
                                     uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr inBuf, uint inSize,
                                       IntPtr outBuf, uint outSize, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    public static string Send(string dev, byte[] cdb) {
        IntPtr h = CreateFileW(dev, 0x80000000u | 0x40000000u, 3u, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == (IntPtr)(-1)) return "CreateFile failed err=" + Marshal.GetLastWin32Error();
        int sz = Marshal.SizeOf(typeof(SCSI_PASS_THROUGH_DIRECT));
        int senseLen = 32;
        IntPtr buf = Marshal.AllocHGlobal(sz + senseLen);
        try {
            var s = new SCSI_PASS_THROUGH_DIRECT();
            s.Length = (ushort)sz;
            s.CdbLength = 16;
            s.SenseInfoLength = (byte)senseLen;
            s.DataIn = 0;
            s.DataTransferLength = 0;
            s.TimeOutValue = 5;
            s.DataBuffer = IntPtr.Zero;
            s.SenseInfoOffset = (uint)sz;
            s.Cdb = new byte[16];
            Array.Copy(cdb, s.Cdb, 16);
            Marshal.StructureToPtr(s, buf, false);
            uint ret = 0;
            bool ok = DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, buf, (uint)(sz + senseLen),
                                      buf, (uint)(sz + senseLen), out ret, IntPtr.Zero);
            if (!ok) return "DeviceIoControl failed err=" + Marshal.GetLastWin32Error();
            var o = (SCSI_PASS_THROUGH_DIRECT)Marshal.PtrToStructure(buf, typeof(SCSI_PASS_THROUGH_DIRECT));
            byte[] sense = new byte[senseLen];
            Marshal.Copy(IntPtr.Add(buf, sz), sense, 0, senseLen);
            string extra = "";
            if (o.ScsiStatus != 0) {
                extra = "  sense:";
                for (int i = 0; i < 16; i++) extra += " " + sense[i].ToString("x2");
            }
            return "scsiStatus=0x" + o.ScsiStatus.ToString("x2") + extra;
        } finally { Marshal.FreeHGlobal(buf); CloseHandle(h); }
    }
}
'@

$bytes = New-Object byte[] 12
$hex = $Payload -replace '[^0-9a-fA-F]', ''
for ($i = 0; $i -lt 12 -and ($i * 2 + 1) -lt $hex.Length; $i++) {
  $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
}
$sub = @{1 = 0x33; 2 = 0x34; 3 = 0x36}[$Type]
if (-not $sub) { Write-Output "bad -Type (1/2/3)"; exit 1 }

for ($n = 0; $n -lt $Count; $n++) {
  $cdb = New-Object byte[] 16
  $cdb[0] = 0xD9
  $cdb[1] = [byte]$sub
  [Array]::Copy($bytes, 0, $cdb, 2, 12)
  $cdb[14] = [byte][char]'O'
  $cdb[15] = [byte][char]'T'
  $cdbHex = ($cdb | ForEach-Object { $_.ToString('x2') }) -join ' '
  $res = [H_Spti]::Send($Device, $cdb)
  Write-Output ("[$($n+1)/$Count] type=$Type CDB= $cdbHex  -> $res")
  if ($n + 1 -lt $Count) { Start-Sleep -Milliseconds $GapMs }
}
