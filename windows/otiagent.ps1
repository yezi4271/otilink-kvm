# otiagent.ps1 —— OTi 对拷线 Windows 侧 agent（被控端/对端）
#
# 目的：让 Windows 侧能说**我们自己的协议**（与 Linux 端 otilink/otiproto 同一套），
#       从而在 Linux(主控) ↔ Windows(被控) 之间实现键鼠共享与剪贴板共享。
#
# 组成（单文件、零安装）：
#   - 内嵌 C#（Add-Type）：CRC32、消息编解码、65536 帧打包/校验、SPTI（IOCTL_SCSI_PASS_THROUGH_DIRECT）
#   - PowerShell 外壳：剪贴板（Get/Set-Clipboard）、注入（SendInput P/Invoke）、主循环
#
# 用法：
#   # 1) 不需要管理员：协议自检（编解码/CRC/帧）
#   powershell -ExecutionPolicy Bypass -File otiagent.ps1 -Selftest
#
#   # 2) 探测哪个设备能应答厂商命令（需要管理员）
#   powershell -ExecutionPolicy Bypass -File otiagent.ps1 -Scan
#
#   # 3) 正常跑（需要管理员；被控端只收不发键鼠，剪贴板双向）
#   powershell -ExecutionPolicy Bypass -File otiagent.ps1 -Device '\\.\H:' -Clipboard
#
# 协议（与 Linux 端一致）：
#   帧   = 65536 字节 = [20B 头][65496B 载荷][20B 头副本]；全零 = 空闲帧
#   消息 = 20B 头 + 载荷（小端）：magic 'OTL1' | type u16 | flags u16 | seq u32 | len u32 | crc32 u32
#          crc32 覆盖 头[0..15] + 载荷
#   type = 1 KEY / 2 MOUSE / 3 SWITCH / 4 CLIP / 5 PING / 10 ROLE（角色协商）
#   SCSI = 写 0xD9/0x2A/0xFF + 'OT'；读 0xD8/0x00/0x03 <held:16位大端> + 'OT'

param(
    [switch]$Selftest,
    [switch]$Scan,
    [switch]$ReadOnly,
    [string]$Device = '',
    [switch]$Clipboard,
    [int]$ClipIntervalMs = 300,
    [int]$DurationSec = 0,
    [switch]$Inject,
    # 只注入**键盘**（KEY 帧）。为什么单独一个开关：Kylin→Windows 的 HID 键盘通道有
    # 线缆固件缺陷（re/NOTES.md §61），键盘改走帧管道 + 这里 SendInput；
    # 鼠标继续走 HID，所以**不能**用 -Inject（那会把鼠标也注入，与 HID 通道双份输入）。
    [switch]$InjectKeys,
    [switch]$Verbose,
    [switch]$Dump,
    [switch]$Probe,
    [switch]$Pages,
    [switch]$Sweep,
    [switch]$HidTest,
    [switch]$InjectTest,
    [int]$Watch = 0,
    [int]$HidRead = 0,
    [string]$Tcp = '',
    [int]$Port = 0,
    [switch]$CmdSweep,
    [switch]$WriteSweep,
    [int]$Rx = 0,
    [switch]$Cable,
    [int]$AbsW = 0,        # 主控端屏幕宽（0 = 用本次收到的 HELLO / 自动）
    [int]$AbsH = 0,
    # 鼠标注入模式：
    #   native   = 收相对位移，走 Windows 自己的**指针速度/加速**（原生手感，默认）
    #   absolute = 收绝对坐标，用 MOUSEEVENTF_ABSOLUTE（1:1 跟手，不受加速影响）
    [ValidateSet('native','absolute')]
    [string]$MouseMode = 'native',
    [switch]$NoTray,        # 关掉托盘图标（无桌面会话时用）
    # 角色协商：auto=按本机键鼠自动判 / master / slave=显式强制（对端必须让位；见 otikm_role_decide）
    [ValidateSet('auto','master','slave')]
    [string]$Role = 'auto',
    [string]$Raw = '',
    [int]$RawLen = 16,
    [string]$RawDir = 'in',
    [switch]$Restart,
    [switch]$ModeSweep,
    [switch]$ModeCmd,
    [int]$Frames = 3,
    [switch]$Write,
    [string]$FrameTest = '',
    [string]$FrameOut = ''
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}

# ---- 单实例守卫（L4 的剪贴板代理版）----
# 同一条线缆帧管道**只能有一个读者**。两个实例会互相抢走发送授权消息与帧，症状是
# 双向全丢、对端"未确认"、对端写命令返回 CHECK_CONDITION（真机实测：Kylin 侧每秒
# `ERR send ROLE rc=524546`，而 Windows 这边看起来"在跑、也在发"）。排查代价极高，
# 所以直接在这里挡掉第二个实例。
if ($Cable -and -not $Selftest -and -not $Scan) {
    $me = $PID
    $dupes = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -EA SilentlyContinue |
               Where-Object { $_.ProcessId -ne $me -and $_.CommandLine -and
                              ($_.CommandLine -like '*otiagent.ps1*') -and
                              ($_.CommandLine -notlike '*-Selftest*') -and
                              ($_.CommandLine -notlike '*-Scan*') })
    if ($dupes.Count -gt 0) {
        $ids = ($dupes | ForEach-Object { $_.ProcessId }) -join ','
        Write-Host ("已有一个剪贴板代理在跑（pid=" + $ids + "）→ 本实例退出：两个读者会抢同一条线缆管道，双向全丢")
        exit 3
    }
}

# ============================ 内嵌 C# ============================
$cs = @'
using System;
using System.Runtime.InteropServices;

public static class Oti
{
    public const int FRAME_SIZE = 65536;
    public const int FRAME_HEAD = 20;
    public const int FRAME_BODY = FRAME_SIZE - 2 * FRAME_HEAD;   // 65496
    public const int HDR_SIZE   = 20;
    public const int PAYLOAD_MAX = FRAME_BODY - HDR_SIZE;        // 65476
    public const int CLIP_CHUNK_MAX = 65000;
    public const uint MAGIC = 0x314C544F;                        // 'OTL1'
    public const byte OP_WRITE = 0xD9, OP_READ = 0xD8, SUB_DATA = 0x2A;
    public const byte RD_CH_DATA = 0x03;

    // ---------------- CRC32 ----------------
    static uint[] _tab;
    static void InitTab() {
        _tab = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint c = i;
            for (int k = 0; k < 8; k++) c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            _tab[i] = c;
        }
    }
    public static uint Crc32Append(uint crc, byte[] buf, int off, int len) {
        if (_tab == null) InitTab();
        crc ^= 0xFFFFFFFFu;
        for (int i = 0; i < len; i++) crc = _tab[(crc ^ buf[off + i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
    public static uint Crc32(byte[] b, int off, int len) { return Crc32Append(0, b, off, len); }

    // ---------------- 消息编解码 ----------------
    public static void Put16(byte[] b, int o, ushort v) { b[o] = (byte)v; b[o+1] = (byte)(v >> 8); }
    public static void Put32(byte[] b, int o, uint v) {
        b[o]=(byte)v; b[o+1]=(byte)(v>>8); b[o+2]=(byte)(v>>16); b[o+3]=(byte)(v>>24);
    }
    // 大文件分片头里要放 u64（文件总长），与麒麟侧 otixfer.c 的小端写法对齐
    public static void Put64(byte[] b, int o, ulong v) {
        for (int i = 0; i < 8; i++) b[o+i] = (byte)(v >> (8*i));
    }
    public static ulong Get64(byte[] b, int o) {
        ulong v = 0; for (int i = 0; i < 8; i++) v |= ((ulong)b[o+i]) << (8*i); return v;
    }
    public static ushort Get16(byte[] b, int o) { return (ushort)(b[o] | (b[o+1] << 8)); }
    public static uint Get32(byte[] b, int o) {
        return (uint)b[o] | ((uint)b[o+1] << 8) | ((uint)b[o+2] << 16) | ((uint)b[o+3] << 24);
    }

    public static byte[] Encode(ushort type, uint seq, byte[] payload, int len) {
        if (len < 0 || len > PAYLOAD_MAX) throw new ArgumentException("payload too large");
        byte[] m = new byte[HDR_SIZE + len];
        Put32(m, 0, MAGIC); Put16(m, 4, type); Put16(m, 6, 0); Put32(m, 8, seq); Put32(m, 12, (uint)len);
        if (len > 0) Array.Copy(payload, 0, m, HDR_SIZE, len);
        uint crc = Crc32(m, 0, 16);
        if (len > 0) crc = Crc32Append(crc, m, HDR_SIZE, len);
        Put32(m, 16, crc);
        return m;
    }
    // 解出 type/seq/载荷；返回 false 表示非法（magic/长度/CRC）
    public static bool Decode(byte[] buf, int len, out ushort type, out uint seq, out byte[] payload) {
        type = 0; seq = 0; payload = null;
        if (len < HDR_SIZE || Get32(buf, 0) != MAGIC) return false;
        uint plen = Get32(buf, 12);
        if (plen > PAYLOAD_MAX || HDR_SIZE + plen > (uint)len) return false;
        uint crc = Crc32(buf, 0, 16);
        if (plen > 0) crc = Crc32Append(crc, buf, HDR_SIZE, (int)plen);
        if (crc != Get32(buf, 16)) return false;
        type = Get16(buf, 4); seq = Get32(buf, 8);
        payload = new byte[plen];
        if (plen > 0) Array.Copy(buf, HDR_SIZE, payload, 0, (int)plen);
        return true;
    }

    // 载荷构造
    public static byte[] KeyPayload(ushort code, byte value) { return new byte[] { (byte)code, (byte)(code >> 8), value, 0 }; }
    public static byte[] MousePayload(short dx, short dy, short wheel, ushort buttons) {
        byte[] p = new byte[8];
        Put16(p, 0, (ushort)dx); Put16(p, 2, (ushort)dy); Put16(p, 4, (ushort)wheel); Put16(p, 6, buttons);
        return p;
    }

    // ---------------- 帧 ----------------
    public static void FramePack(byte[] body, int len, byte[] frame) {
        Array.Clear(frame, 0, FRAME_SIZE);
        frame[0] = (byte)'O'; frame[1] = (byte)'T'; frame[2] = (byte)'I'; frame[3] = (byte)'L';
        Put32(frame, 4, (uint)len);
        if (len > 0) Array.Copy(body, 0, frame, FRAME_HEAD, Math.Min(len, FRAME_BODY));
        Array.Copy(frame, 0, frame, FRAME_SIZE - FRAME_HEAD, FRAME_HEAD);
    }
    public static int FrameCheck(byte[] frame) {          // 1=有效 0=空闲 -1=非法
        bool allZero = true;
        for (int i = 0; i < FRAME_HEAD; i++) if (frame[i] != 0) { allZero = false; break; }
        if (allZero) return 0;
        for (int i = 0; i < FRAME_HEAD; i++)
            if (frame[i] != frame[FRAME_SIZE - FRAME_HEAD + i]) return -1;
        return 1;
    }
    public static int FrameBodyLen(byte[] frame) {
        uint l = Get32(frame, 4);
        return (l > FRAME_BODY) ? FRAME_BODY : (int)l;
    }

    // ---------------- SPTI ----------------
    // 规范布局：x64 下 sizeof=56，Cdb 在偏移 36；Sense 缓冲**放在结构之后**（SenseInfoOffset=56）
    [StructLayout(LayoutKind.Sequential)]
    struct SCSI_PASS_THROUGH_DIRECT {
        public ushort Length; public byte ScsiStatus; public byte PathId; public byte TargetId;
        public byte Lun; public byte CdbLength; public byte SenseInfoLength; public byte DataIn;
        public uint DataTransferLength; public uint TimeOutValue; public IntPtr DataBuffer;
        public uint SenseInfoOffset;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] Cdb;
    }
    public static int StructSize { get { return Marshal.SizeOf(typeof(SCSI_PASS_THROUGH_DIRECT)); } }
    public static int OffsetOf(string f) { return (int)Marshal.OffsetOf(typeof(SCSI_PASS_THROUGH_DIRECT), f); }
    const uint IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x4D014;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec,
                                     uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr inBuf, uint inSize,
                                       IntPtr outBuf, uint outSize, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    public static IntPtr OpenDevice(string path) { return OpenDevice(path, false); }
    public static IntPtr OpenDeviceRaw(string path, uint access) {
        return CreateFileW(path, access, 0x3u, IntPtr.Zero, 3, 0, IntPtr.Zero);
    }
    // 打开 HID 设备接口用于读输入报告（非管理员即可）
    public static IntPtr CreateFileRead(string path) {
        return CreateFileW(path, 0x80000000u, 0x3u, IntPtr.Zero, 3, 0, IntPtr.Zero);
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(IntPtr h, byte[] buf, uint n, out uint got, IntPtr ov);
    public static int ReadHid(IntPtr h, byte[] buf, uint n) {
        uint got = 0;
        if (!ReadFile(h, buf, n, out got, IntPtr.Zero)) return -Marshal.GetLastWin32Error();
        return (int)got;
    }
    // readOnly=1 时只用 GENERIC_READ（读 CD 等卷通常不需要管理员；SPTI 读方向有可能过）
    public static IntPtr OpenDevice(string path, bool readOnly) {
        uint acc = readOnly ? 0x80000000u : (0x80000000u | 0x40000000u);
        return CreateFileW(path, acc, 0x3u, IntPtr.Zero, 3, 0, IntPtr.Zero);
    }
    public static void CloseDevice(IntPtr h) { if (h != IntPtr.Zero && h != (IntPtr)(-1)) CloseHandle(h); }
    public static bool IsValid(IntPtr h) { return h != IntPtr.Zero && h != (IntPtr)(-1); }

    // 执行一条 16 字节 CDB；toDev=1 表示数据阶段为写。返回 0 成功，否则非 0（含 SCSI 状态）
    const int SENSE_LEN = 32;
    public static int Exec(IntPtr h, byte[] cdb, byte[] data, bool toDev, out byte scsiStatus, out byte[] sense) {
        scsiStatus = 0; sense = new byte[SENSE_LEN];
        int dataLen = (data == null) ? 0 : data.Length;
        int sz = Marshal.SizeOf(typeof(SCSI_PASS_THROUGH_DIRECT));   // 56
        int need = sz + SENSE_LEN + Math.Max(dataLen, 1);
        IntPtr buf = Marshal.AllocHGlobal(need);
        IntPtr sensePtr = IntPtr.Add(buf, sz);
        IntPtr dataPtr  = IntPtr.Add(buf, sz + SENSE_LEN);
        try {
            if (dataLen > 0) Marshal.Copy(data, 0, dataPtr, dataLen);
            var s = new SCSI_PASS_THROUGH_DIRECT();
            s.Length = (ushort)sz;
            s.CdbLength = 16;
            s.SenseInfoLength = SENSE_LEN;
            s.DataIn = (byte)(dataLen == 0 ? 0 : (toDev ? 0 : 1));   // 0=写(TO_DEV) 1=读(FROM_DEV)
            s.DataTransferLength = (uint)dataLen;
            s.TimeOutValue = 5;
            s.DataBuffer = dataPtr;
            s.SenseInfoOffset = (uint)sz;                            // Sense 紧跟结构之后
            s.Cdb = new byte[16];
            Array.Copy(cdb, s.Cdb, 16);
            Marshal.StructureToPtr(s, buf, false);
            uint ret = 0;
            bool ok = DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, buf, (uint)need,
                                      buf, (uint)need, out ret, IntPtr.Zero);
            if (!ok) return Marshal.GetLastWin32Error();
            var outS = (SCSI_PASS_THROUGH_DIRECT)Marshal.PtrToStructure(buf, typeof(SCSI_PASS_THROUGH_DIRECT));
            scsiStatus = outS.ScsiStatus;
            Marshal.Copy(sensePtr, sense, 0, SENSE_LEN);
            if (dataLen > 0 && !toDev) Marshal.Copy(dataPtr, data, 0, dataLen);
            return outS.ScsiStatus == 0 ? 0 : (0x100 | outS.ScsiStatus);
        } finally { Marshal.FreeHGlobal(buf); }
    }

    // 厂商命令：设备信息块（0xF0/0x00/0x00 读 64B）
    public static int InfoRead(IntPtr h, byte[] out64) {
        byte[] cdb = new byte[16];
        cdb[0] = 0xF0; cdb[14] = (byte)'O'; cdb[15] = (byte)'T';
        byte st; byte[] se;
        return Exec(h, cdb, out64, false, out st, out se);
    }
    // 厂商命令：设备模式读（0xD9/0x60，CDB[2]=2）
    public static int DevMode(IntPtr h, out byte mode) {
        byte[] cdb = new byte[16];
        cdb[0] = OP_WRITE; cdb[1] = 0x60; cdb[2] = 2; cdb[14] = (byte)'O'; cdb[15] = (byte)'T';
        byte[] buf = new byte[16];
        byte st; byte[] se;
        int rc = Exec(h, cdb, buf, false, out st, out se);
        mode = buf[0];
        return rc;
    }

    // 厂商命令：写一个数据帧（0xD9/0x2A/0xFF），data 必须是 65536 字节
    public static int WriteFrame(IntPtr h, byte[] frame) {
        byte[] cdb = new byte[16];
        cdb[0] = OP_WRITE; cdb[1] = SUB_DATA; cdb[2] = 0xFF; cdb[14] = (byte)'O'; cdb[15] = (byte)'T';
        byte st; byte[] se;
        return Exec(h, cdb, frame, true, out st, out se);
    }
    // 厂商命令：读一个数据帧（0xD8/0x00/0x03 + held 16 位大端）
    public static int ReadFrame(IntPtr h, byte[] frame, ushort held) {
        byte[] cdb = new byte[16];
        cdb[0] = OP_READ; cdb[1] = 0x00; cdb[2] = RD_CH_DATA;
        cdb[3] = (byte)(held >> 8); cdb[4] = (byte)(held & 0xFF);
        cdb[14] = (byte)'O'; cdb[15] = (byte)'T';
        byte st; byte[] se;
        return Exec(h, cdb, frame, false, out st, out se);
    }

    // 真正的帧读取：0xD9/0x28/0x64 + 65536 字节 IN 数据阶段
    // 依据：反汇编 OTiTransporter::GetData @0x57bc（CDB = D9 28 64 ... 'OT'）
    public static int ReadFrame28(IntPtr h, byte[] frame) {
        byte[] cdb = new byte[16];
        cdb[0] = OP_WRITE; cdb[1] = 0x28; cdb[2] = 0x64;
        cdb[14] = (byte)'O'; cdb[15] = (byte)'T';
        byte st; byte[] se;
        return Exec(h, cdb, frame, false, out st, out se);
    }

    // 消息管道读：0xD8/0x00/0x03，**只读 16 字节**（这不是 64KB 帧！）
    // 依据：反汇编 ProcessIdleState @0x4e30（申请 16 字节 + 按 buf[0] SWITCH 分发）
    public static int ReadMsg(IntPtr h, byte[] m16) {
        byte[] cdb = new byte[16];
        cdb[0] = OP_READ; cdb[1] = 0x00; cdb[2] = RD_CH_DATA;
        cdb[14] = (byte)'O'; cdb[15] = (byte)'T';
        byte st; byte[] se;
        return Exec(h, cdb, m16, false, out st, out se);
    }

    // 消息管道读 + 上报"本机待发送帧数"（CDB[3..4]，16 位大端）
    public static int ReadMsgP(IntPtr h, byte[] m16, ushort pending) {
        byte[] cdb = new byte[16];
        cdb[0] = OP_READ; cdb[1] = 0x00; cdb[2] = RD_CH_DATA;
        cdb[3] = (byte)(pending >> 8); cdb[4] = (byte)(pending & 0xFF);
        cdb[14] = (byte)'O'; cdb[15] = (byte)'T';
        byte st; byte[] se;
        return Exec(h, cdb, m16, false, out st, out se);
    }

    // ---------------- 输入注入（SendInput） ----------------
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public InputUnion U; }
    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }

    [DllImport("user32.dll", SetLastError = true)]
    static extern uint SendInput(uint n, INPUT[] inputs, int size);

    const uint INPUT_KEYBOARD = 1, INPUT_MOUSE = 0;
    const uint KEYEVENTF_EXTENDEDKEY = 0x0001, KEYEVENTF_KEYUP = 0x0002, KEYEVENTF_SCANCODE = 0x0008;
    const uint MOUSEEVENTF_MOVE = 0x0001, MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004;
    const uint MOUSEEVENTF_RIGHTDOWN = 0x0008, MOUSEEVENTF_RIGHTUP = 0x0010;
    const uint MOUSEEVENTF_MIDDLEDOWN = 0x0020, MOUSEEVENTF_MIDDLEUP = 0x0040;
    const uint MOUSEEVENTF_WHEEL = 0x0800;

    // ---- 扩展键：evdev 码 → PS/2 set-1 扫描码 ----
    // 主键区（evdev 1..88）与 PS/2 set-1 **完全一致**，直接用；
    // 其余（方向键/Home/End/右Ctrl/小键盘回车/Win 键…）需要 E0 前缀，
    // 即 KEYEVENTF_EXTENDEDKEY + 下表里的扫描码。
    static readonly System.Collections.Generic.Dictionary<ushort, byte> ExtScan =
        new System.Collections.Generic.Dictionary<ushort, byte> {
            { 96, 0x1C },   // KP_ENTER
            { 97, 0x1D },   // RIGHTCTRL
            { 98, 0x35 },   // KP_SLASH
            { 99, 0x37 },   // SYSRQ / PrintScreen
            { 100, 0x38 },  // RIGHTALT
            { 102, 0x47 },  // HOME
            { 103, 0x48 },  // UP
            { 104, 0x49 },  // PAGEUP
            { 105, 0x4B },  // LEFT
            { 106, 0x4D },  // RIGHT
            { 107, 0x4F },  // END
            { 108, 0x50 },  // DOWN
            { 109, 0x51 },  // PAGEDOWN
            { 110, 0x52 },  // INSERT
            { 111, 0x53 },  // DELETE
            { 125, 0x5B },  // LEFTMETA (Win)
            { 126, 0x5C },  // RIGHTMETA
            { 127, 0x5D },  // COMPOSE (Menu)
        };

    static bool EvdevToScan(ushort code, out byte scan, out bool ext) {
        ext = false;
        if (code >= 1 && code <= 88) { scan = (byte)code; return true; }
        byte v;
        if (ExtScan.TryGetValue(code, out v)) { scan = v; ext = true; return true; }
        scan = 0;
        return false;                      // 未映射：宁可不注入，也别按错键
    }

    public static bool Key(ushort code, bool down) {
        byte scan; bool ext;
        if (!EvdevToScan(code, out scan, out ext)) return false;
        uint flags = KEYEVENTF_SCANCODE | (down ? 0u : KEYEVENTF_KEYUP);
        if (ext) flags |= KEYEVENTF_EXTENDEDKEY;
        var inp = new INPUT[1];
        inp[0].type = INPUT_KEYBOARD;
        inp[0].U.ki = new KEYBDINPUT { wVk = 0, wScan = scan, dwFlags = flags };
        SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
        return true;
    }

    // ---- 绝对坐标鼠标（绕开系统指针加速，1:1 跟手）----
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int nIndex);
    const int SM_CXSCREEN = 0, SM_CYSCREEN = 1;
    const int SM_XVIRTUALSCREEN = 76, SM_YVIRTUALSCREEN = 77,
              SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79;
    const uint MOUSEEVENTF_ABSOLUTE = 0x8000, MOUSEEVENTF_VIRTUALDESK = 0x4000;

    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static int ScreenW() { return GetSystemMetrics(SM_CXSCREEN); }
    public static int ScreenH() { return GetSystemMetrics(SM_CYSCREEN); }
    // 整个虚拟桌面（所有显示器）的尺寸；绝对坐标模式用它，才能到达副屏。
    public static int VirtW() { return GetSystemMetrics(SM_CXVIRTUALSCREEN); }
    public static int VirtH() { return GetSystemMetrics(SM_CYVIRTUALSCREEN); }

    // x,y 是"对端屏幕"（主显示器）坐标系下的像素位置；MOUSEEVENTF_ABSOLUTE 用的是
    // 整个虚拟桌面并归一化到 0..65535，所以这里要按虚拟桌面换算。
    // 实测（本机 2048x1152 + 左侧副屏）：MOUSEEVENTF_ABSOLUTE 的 0..65535
    // **线性映射到主显示器** 0..SM_CXSCREEN-1（不是文档说的虚拟桌面），
    // 且与 GetSystemMetrics 处于同一 DPI 上下文。用这个公式实测误差为 0：
    //   目标 600,300 → 600,300；1500,900 → 1500,900；50,40 → 50,40
    // x,y 是"远端**虚拟桌面**"坐标（0..w-1），w/h 由 HELLO 上报。
    // 关键：必须同时带 MOUSEEVENTF_VIRTUALDESK(0x4000)，否则 ABSOLUTE 只映射**主显示器**
    // （实测：不带它时 nx 从 0→65535 光标只在主屏 0→2559 移动，左副屏完全到不了）。
    public static void MouseAbs(int x, int y, int vw, int vh) {
        if (vw < 2) vw = 2;
        if (vh < 2) vh = 2;
        long nx = (long)x * 65535 / (vw - 1);
        long ny = (long)y * 65535 / (vh - 1);
        if (nx < 0) nx = 0;
        if (nx > 65535) nx = 65535;
        if (ny < 0) ny = 0;
        if (ny > 65535) ny = 65535;
        var inp = new INPUT[1];
        inp[0].type = INPUT_MOUSE;
        inp[0].U.mi = new MOUSEINPUT { dx = (int)nx, dy = (int)ny,
            dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK };
        SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Mouse(int dx, int dy, int wheel, ushort buttons) {
        var list = new System.Collections.Generic.List<INPUT>();
        if (dx != 0 || dy != 0) {
            var i = new INPUT(); i.type = INPUT_MOUSE;
            i.U.mi = new MOUSEINPUT { dx = dx, dy = dy, dwFlags = MOUSEEVENTF_MOVE };
            list.Add(i);
        }
        if (wheel != 0) {
            var i = new INPUT(); i.type = INPUT_MOUSE;
            i.U.mi = new MOUSEINPUT { mouseData = (uint)(wheel * 120), dwFlags = MOUSEEVENTF_WHEEL };
            list.Add(i);
        }
        // 按键状态差异由调用方维护；这里按位图直接发 down/up 需要外部状态，简化：由 PS 侧处理
        if (list.Count > 0) SendInput((uint)list.Count, list.ToArray(), Marshal.SizeOf(typeof(INPUT)));
    }
    public static uint MouseButton(int which, bool down) {
        uint f = 0;
        if (which == 0) f = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        else if (which == 1) f = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        else f = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        var inp = new INPUT[1];
        inp[0].type = INPUT_MOUSE;
        inp[0].U.mi = new MOUSEINPUT { dwFlags = f };
        return SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
    }
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
    public static int LDown() { return GetAsyncKeyState(0x01) & 0x8000; }
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    public static void Wheel(int notches) {
        var inp = new INPUT[1];
        inp[0].type = INPUT_MOUSE;
        inp[0].U.mi = new MOUSEINPUT { mouseData = (uint)(notches * 120), dwFlags = MOUSEEVENTF_WHEEL };
        SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

Add-Type -TypeDefinition $cs -Language CSharp

# ============================ 设备模式命令（0xD9/0x60）在 2208 下试读/试写，看能否切人格 ============================
if ($ModeCmd) {
    function FindDev2 {
        foreach ($c in '\\.\F:', '\\.\H:', '\\.\PhysicalDrive1', '\\.\PhysicalDrive2', '\\.\PhysicalDrive3', '\\.\PhysicalDrive4') {
            $h = [Oti]::OpenDevice($c, $false)
            if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($c, $true) }
            if (-not [Oti]::IsValid($h)) { continue }
            $buf = New-Object byte[] 64
            if ([Oti]::InfoRead($h, $buf) -eq 0 -and $buf[2] -eq 0x22) { return @{ dev = $c; h = $h; pid = $buf[3] } }
            [Oti]::CloseDevice($h)
        }
        return $null
    }
    function CurPid2 {
        $d = Get-PnpDevice -PresentOnly -EA SilentlyContinue | Where-Object { $_.InstanceId -match '0EA0' }
        if ($d) { return (($d.InstanceId | Select-Object -First 1) -replace '.*PID_', 'PID_') } else { return '(无)' }
    }

    $f = FindDev2
    if ($f -eq $null) { Write-Host "找不到设备"; exit 1 }
    Write-Host ("当前: {0}  设备路径 {1}  信息块 PID=0x{2:x2}" -f (CurPid2), $f.dev, $f.pid)
    $st = [byte]0; $se = $null

    Write-Host "== 0xD9/0x60 读（16 字节）=="
    foreach ($sel in 0,1,2,3,4) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = 0x60; $cdb[2] = [byte]$sel
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $b = New-Object byte[] 16
        $rc = [Oti]::Exec($f.h, $cdb, $b, $false, [ref]$st, [ref]$se)
        $key = if ($se -ne $null -and $se.Length -gt 2) { $se[2] -band 0x0F } else { 0 }
        $asc = if ($se -ne $null -and $se.Length -gt 13) { $se[12] } else { 0 }
        Write-Host ("  CDB[2]={0} rc={1,-4} scsi={2} key=0x{3:x} ASC=0x{4:x} buf={5}" -f $sel, $rc, $st, $key, $asc, (($b[0..7] | ForEach-Object { $_.ToString('x2') }) -join ' '))
    }

    Write-Host "== 0xD9/0x60 写（1 字节模式值 0..4），每次等 5 秒看人格是否变化 =="
    foreach ($v in 0,1,2,3,4) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = 0x60; $cdb[2] = 2
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $pl = New-Object byte[] 1
        $pl[0] = [byte]$v
        $rc = [Oti]::Exec($f.h, $cdb, $pl, $true, [ref]$st, [ref]$se)
        $key = if ($se -ne $null -and $se.Length -gt 2) { $se[2] -band 0x0F } else { 0 }
        $asc = if ($se -ne $null -and $se.Length -gt 13) { $se[12] } else { 0 }
        Start-Sleep -Seconds 5
        Write-Host ("  写 mode={0} rc={1,-4} scsi={2} key=0x{3:x} ASC=0x{4:x} -> 人格 {5}" -f $v, $rc, $st, $key, $asc, (CurPid2))
    }
    [Oti]::CloseDevice($f.h)
    exit 0
}

# ============================ 模式扫描：试 USBRestart 的不同 CDB[4]，看哪种人格回来 ============================
if ($ModeSweep) {
    function FindCableDev {
        foreach ($c in '\\.\F:', '\\.\H:', '\\.\PhysicalDrive1', '\\.\PhysicalDrive2', '\\.\PhysicalDrive3', '\\.\PhysicalDrive4') {
            $h = [Oti]::OpenDevice($c, $false)
            if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($c, $true) }
            if (-not [Oti]::IsValid($h)) { continue }
            $buf = New-Object byte[] 64
            $rc = [Oti]::InfoRead($h, $buf)
            if ($rc -eq 0 -and $buf[2] -eq 0x22 -and ($buf[3] -eq 0x13 -or $buf[3] -eq 0x08)) {
                return @{ dev = $c; h = $h; pid = $buf[3] }
            }
            [Oti]::CloseDevice($h)
        }
        return $null
    }
    function CurPid {
        $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '0EA0' }
        if ($d) { return ($d.InstanceId | Select-Object -First 1) }
        return '(无)'
    }

    Write-Host "== 当前人格 =="
    Write-Host ("  {0}" -f (CurPid))
    Write-Host "== 试 USBRestart 的 CDB[4] = 0x00..0x0F（每次等 6 秒重新枚举）=="
    foreach ($v in 0x00..0x0F) {
        $f = FindCableDev
        if ($f -eq $null) { Write-Host "  找不到设备，停止"; break }
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xF0; $cdb[1] = 0x05; $cdb[2] = 0x02; $cdb[4] = [byte]$v
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $st = [byte]0; $se = $null
        $rc = [Oti]::Exec($f.h, $cdb, $null, $true, [ref]$st, [ref]$se)
        [Oti]::CloseDevice($f.h)
        Start-Sleep -Seconds 6
        $pid2 = CurPid
        Write-Host ("  CDB[4]=0x{0:x2}  rc={1,-4} -> {2}" -f $v, $rc, $pid2)
    }
    exit 0
}

# ============================ 切换设备模式（厂商 USBRestart：0xF0/0x05/0x02，CDB[4]=0x0A） ============================
if ($Restart) {
    $dev = if ($Device) { $Device } else { '\\.\F:' }
    Write-Host "== 发 USBRestart 到 $dev（设备会重新枚举，可能切换 USB 人格）=="
    $h = [Oti]::OpenDevice($dev, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($dev, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "打不开 $dev"; exit 1 }
    $cdb = New-Object byte[] 16
    $cdb[0] = 0xF0; $cdb[1] = 0x05; $cdb[2] = 0x02; $cdb[4] = 0x0A
    $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
    $st = [byte]0; $se = $null
    $rc = [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)
    Write-Host ("  已发 rc={0} scsi={1}" -f $rc, $st)
    [Oti]::CloseDevice($h)
    Start-Sleep -Seconds 6
    Write-Host "== 重新枚举后的状态 =="
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '0EA0' } | Select-Object Status,Class,FriendlyName,InstanceId | Format-Table -AutoSize | Out-String -Width 120
    Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -eq 5 -or $_.DeviceID -eq 'H:' -or $_.DeviceID -eq 'F:' } | Select-Object DeviceID,DriveType,VolumeName | Format-Table -AutoSize | Out-String -Width 60
    exit 0
}

# ============================ 控制命令空间扫描：找"链路/对端状态"命令 ============================
if ($CmdSweep) {
    $h = [Oti]::OpenDevice('\\.\H:', $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice('\\.\H:', $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "H: 打不开"; exit 1 }
    $st = [byte]0; $se = $null

    Write-Host "== 0xF0 控制命令扫描（CDB[1]=0x00..0x3F，读 64 字节）=="
    foreach ($b1 in 0x00..0x3F) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xF0; $cdb[1] = [byte]$b1; $cdb[2] = 0
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $buf = New-Object byte[] 64
        $rc = [Oti]::Exec($h, $cdb, $buf, $false, [ref]$st, [ref]$se)
        $nz = 0; for ($k = 0; $k -lt 64; $k++) { if ($buf[$k] -ne 0) { $nz++ } }
        if ($rc -eq 0 -and $nz -gt 0) {
            $hex = ($buf[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' '
            $asc = -join ($buf[0..15] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
            Write-Host ("  F0/{0:x2} 非零={1,-3} {2}  |{3}|" -f $b1, $nz, $hex, $asc)
        }
    }

    Write-Host "== 0xD8 读通道扫描（CDB[2]=0x00..0x0F，读 8KB）=="
    foreach ($b2 in 0x00..0x0F) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD8; $cdb[1] = 0x00; $cdb[2] = [byte]$b2; $cdb[4] = 1
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $buf = New-Object byte[] 8192
        $rc = [Oti]::Exec($h, $cdb, $buf, $false, [ref]$st, [ref]$se)
        $nz = 0; for ($k = 0; $k -lt 8192; $k++) { if ($buf[$k] -ne 0) { $nz++ } }
        $hex = ($buf[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $asc = -join ($buf[0..15] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
        Write-Host ("  D8/00/{0:x2} rc={1,-4} 非零={2,-5} {3}  |{4}|" -f $b2, $rc, $nz, $hex, $asc)
    }

    Write-Host "== 0xD9 写命令子通道扫描（CDB[1]=0x00..0x3F，无数据阶段，看哪些被接受）=="
    $accepted = @()
    foreach ($b1 in 0x00..0x3F) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = [byte]$b1
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $rc = [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)
        if ($rc -eq 0) { $accepted += ('0x{0:x2}' -f $b1) }
    }
    Write-Host ("  被接受的子通道: {0}" -f ($(if ($accepted.Count) { $accepted -join ', ' } else { '（无）' })))
    [Oti]::CloseDevice($h)
    exit 0
}

# ============================ 直读线缆注入的 HID 报告（最精确的对端注入观察器） ============================
if ($HidRead -gt 0) {
    # 从 PnP 实例 ID 构造 HID 设备接口路径（HID\VID_xxxx&PID_xxxx&MI_yy\... -> \\?\hid#...)
    $guid = '{4d1e55b2-f16f-11cf-88cb-001111000030}'
    $targets = @()
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '^HID\\VID_0EA0&PID_2213' } | ForEach-Object {
        $targets += @{ id = $_.InstanceId; kind = $_.FriendlyName }
    }
    if ($targets.Count -eq 0) { Write-Host "没找到对拷线的 HID 顶层集合"; exit 1 }
    $hs = @()
    foreach ($t in $targets) {
        $path = '\\?\' + ($t.id.ToLower() -replace '\\', '#') + '#' + $guid
        $h = [Oti]::CreateFileRead($path)
        if ([Oti]::IsValid($h)) {
            Write-Host ("  已打开 {0}  ({1})" -f $t.kind, $t.id)
            $hs += @{ h = $h; kind = $t.kind }
        } else {
            Write-Host ("  打不开 {0}: err={1}" -f $t.id, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
    }
    if ($hs.Count -eq 0) { Write-Host "没有可读的 HID 接口"; exit 1 }

    Write-Host "== 读 $HidRead 秒：任何由线缆注入的对端键鼠都会在这里以 HID 报告形式出现 =="
    $t0 = Get-Date
    $total = 0
    while (((Get-Date) - $t0).TotalSeconds -lt $HidRead) {
        foreach ($e in $hs) {
            $buf = New-Object byte[] 64
            $got = [Oti]::ReadHid($e.h, $buf, 64)
            if ($got -gt 0) {
                $hex = ($buf[0..($got-1)] | ForEach-Object { $_.ToString('x2') }) -join ' '
                $el = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
                Write-Host ("  [{0,6}s] {1,-22} {2} 字节: {3}" -f $el, $e.kind, $got, $hex)
                $total++
            }
        }
        Start-Sleep -Milliseconds 5
    }
    foreach ($e in $hs) { [Oti]::CloseDevice($e.h) }
    Write-Host "  读结束：共 $total 个 HID 报告"
    exit 0
}

# ============================ 本机观察器：等待对端注入的键鼠 ============================
if ($Watch -gt 0) {
    Write-Host "== 本机观察 $Watch 秒：任何"新出现"的按键或光标移动都打印出来 =="
    Write-Host "   （用于配合 Linux 主控端执行 hidseq：能看出哪种包格式真的生效）"
    Write-Host "   [自测：观察开始 1 秒后用 SendInput 模拟一次 A 按下，应被捕获]"
    $p = New-Object Oti+POINT
    [void][Oti]::GetCursorPos([ref]$p)
    $prev = @{}
    $t0 = Get-Date
    $found = 0
    while (((Get-Date) - $t0).TotalSeconds -lt $Watch) {
        $el = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
        if (-not $script:selfTested -and $el -ge 1.0) {
            [Oti]::Key(0x1E, $true); Start-Sleep -Milliseconds 60; [Oti]::Key(0x1E, $false)
            $script:selfTested = $true
        }
        foreach ($vk in 0x08..0xFE) {
            $down = (([Oti]::GetAsyncKeyState($vk) -band 0x8000) -ne 0)
            if ($down -and -not $prev.ContainsKey($vk)) {
                Write-Host ("  [{0,6}s] 按键按下: VK=0x{1:x2} (scan=0x{2:x2})" -f $el, $vk, $vk)
                $found++
            }
            if ($down) { $prev[$vk] = $true } elseif ($prev.ContainsKey($vk)) { $prev.Remove($vk) | Out-Null }
        }
        $q = New-Object Oti+POINT
        [void][Oti]::GetCursorPos([ref]$q)
        if ($q.X -ne $p.X -or $q.Y -ne $p.Y) {
            Write-Host ("  [{0,6}s] 光标移动: {1},{2} -> {3},{4}" -f $el, $p.X, $p.Y, $q.X, $q.Y)
            $p = $q
            $found++
        }
        Start-Sleep -Milliseconds 15
    }
    Write-Host "  观察结束：捕获到 $found 个事件"
    exit 0
}

# ============================ 注入方向/包格式实测（本机观察光标与按键） ============================
if ($InjectTest) {
    $h = [Oti]::OpenDevice('\\.\H:', $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice('\\.\H:', $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "H: 打不开"; exit 1 }

    function SendHidRaw([byte]$sub, [byte[]]$p12) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = $sub
        [Array]::Copy($p12, 0, $cdb, 2, [Math]::Min(12, $p12.Length))
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $st = [byte]0; $se = $null
        return [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)
    }

    Write-Host "== 1) 光标：发鼠标 HID 包，看本机光标是否移动 =="
    $p0 = New-Object Oti+POINT
    [void][Oti]::GetCursorPos([ref]$p0)
    Write-Host ("  起始光标: {0},{1}" -f $p0.X, $p0.Y)

    $cands = @(
        @{ n = 'type2 [01,00,dx=200,0,...]'; sub = 0x34; p = @(1,0,200,0,0,0,0,0,0,0,0,0) },
        @{ n = 'type2 [00,00,dx=200,0,...]'; sub = 0x34; p = @(0,0,200,0,0,0,0,0,0,0,0,0) },
        @{ n = 'type2 [00,dx=200,0,...]';    sub = 0x34; p = @(0,200,0,0,0,0,0,0,0,0,0,0) },
        @{ n = 'type2 [01,00,00,00,200,...]';sub = 0x34; p = @(1,0,0,0,200,0,0,0,0,0,0,0) },
        @{ n = 'type1 [01,00,dx=200...]';    sub = 0x33; p = @(1,0,200,0,0,0,0,0,0,0,0,0) }
    )
    foreach ($c in $cands) {
        $rc = SendHidRaw $c.sub ([byte[]]$c.p)
        Start-Sleep -Milliseconds 250
        $p1 = New-Object Oti+POINT
        [void][Oti]::GetCursorPos([ref]$p1)
        $moved = ($p1.X -ne $p0.X) -or ($p1.Y -ne $p0.Y)
        Write-Host ("  {0,-32} rc={1,-4} 光标 {2},{3}  移动={4}" -f $c.n, $rc, $p1.X, $p1.Y, $moved)
        $p0 = $p1
    }

    Write-Host "== 2) 按键：发键盘 HID 包，看本机是否收到按键（VK A=0x41 / S=0x53 / 空格=0x20）=="
    $kbd = @(
        @{ n = 'type1 [01,00,04(A),0,...]'; p = @(1,0,4,0,0,0,0,0,0,0,0,0) },
        @{ n = 'type1 [00,00,04,0,...]';    p = @(0,0,4,0,0,0,0,0,0,0,0,0) },
        @{ n = 'type1 [01,00,00,00,1E,...]';p = @(1,0,0,0,0x1E,0,0,0,0,0,0,0) },
        @{ n = 'type1 [00,04,00,...]';      p = @(0,4,0,0,0,0,0,0,0,0,0,0) }
    )
    foreach ($c in $kbd) {
        [void](SendHidRaw 0x33 ([byte[]]$c.p))
        Start-Sleep -Milliseconds 200
        $down = @()
        foreach ($vk in 0x41, 0x53, 0x20, 0x0D) {
            if (([Oti]::GetAsyncKeyState($vk) -band 0x8000) -ne 0) { $down += ('0x{0:x}' -f $vk) }
        }
        [void](SendHidRaw 0x33 (New-Object byte[] 12))     # 释放
        Start-Sleep -Milliseconds 150
        $txt = if ($down.Count) { $down -join ',' } else { '（无）' }
        Write-Host ("  {0,-32} rc={1} 本机按下的键: {2}" -f $c.n, 0, $txt)
    }
    [Oti]::CloseDevice($h)
    exit 0
}

# ============================ HID/控制包按厂商用法重测（数据阶段为空） ============================
if ($HidTest) {
    $dev = if ($Device) { $Device } else { '\\.\H:' }
    $h = [Oti]::OpenDevice($dev, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($dev, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "$dev 打不开"; exit 1 }
    Write-Host "  设备: $dev"
    $st = [byte]0; $se = $null

    function Health([string]$tag) {
        $b = New-Object byte[] 64
        $rc = [Oti]::InfoRead($h, $b)
        Write-Host ("  [健康检查 {0}] 信息块 rc={1} 头={2}" -f $tag, $rc, (($b[0..7] | ForEach-Object { $_.ToString('x2') }) -join ' '))
    }

    function SendHid([string]$tag, [byte]$sub, [byte[]]$payload12) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = $sub
        if ($payload12 -ne $null) { [Array]::Copy($payload12, 0, $cdb, 2, [Math]::Min(12, $payload12.Length)) }
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $st = [byte]0; $se = $null
        $rc = [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)   # 数据阶段为空（厂商用法）
        $key = if ($se -ne $null -and $se.Length -gt 2) { $se[2] -band 0x0F } else { 0 }
        $asc = if ($se -ne $null -and $se.Length -gt 13) { $se[12] } else { 0 }
        Write-Host ("  {0,-26} rc={1,-4} scsi={2} key=0x{3:x} ASC=0x{4:x}" -f $tag, $rc, $st, $key, $asc)
    }

    Health '开始'
    SendHid 'D9/33 CDB[2..13]=全零' 0x33 (New-Object byte[] 12)
    Health 'HID全零之后'
    $k = New-Object byte[] 12; $k[0] = 1; $k[2] = 0x04      # 猜测: [reportID=1][mods][scan=0x04(A)]
    SendHid 'D9/33 猜测键盘报告' 0x33 $k
    Health 'HID键盘之后'
    $m = New-Object byte[] 12; $m[1] = 0x01; $m[2] = 0x05    # 猜测鼠标: 按钮+位移
    SendHid 'D9/34 猜测鼠标报告' 0x34 $m
    Health 'HID鼠标之后'

    # 控制类：数据阶段为空
    $cdb = New-Object byte[] 16
    $cdb[0] = 0xF0; $cdb[1] = 0x05; $cdb[2] = 0x02; $cdb[4] = 0x0A
    $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
    $st = [byte]0; $se = $null
    $rc = [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)
    Write-Host ("  {0,-26} rc={1,-4} scsi={2}" -f 'F0/05/02 USB重启(无数据)', $rc, $st)
    Health 'USB重启之后'

    # 数据写：数据阶段为空 vs 64KB
    $cdb = New-Object byte[] 16
    $cdb[0] = 0xD9; $cdb[1] = 0x2A; $cdb[2] = 0xFF
    $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
    $st = [byte]0; $se = $null
    $rc = [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)
    $key = if ($se -ne $null -and $se.Length -gt 2) { $se[2] -band 0x0F } else { 0 }
    Write-Host ("  {0,-26} rc={1,-4} scsi={2} key=0x{3:x}" -f 'D9/2A/FF 无数据阶段', $rc, $st, $key)
    [Oti]::CloseDevice($h)
    exit 0
}

# ============================ 写命令扫描：找"哪种编码能被接受" ============================
# ============================ 正确的接收循环（消息 + 帧）============================
if ($Rx -gt 0) {
    $dev = if ($Device) { $Device } else { '\\.\H:' }
    $h = [Oti]::OpenDevice($dev, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($dev, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "$dev 打不开"; exit 1 }
    Write-Host "  设备: $dev"
    Write-Host "== 接收循环 $Rx 轮：0xD8/0x00/0x03 读 16B 消息；type=0x05 时用 0xD9/0x28/0x64 读 64KB 帧 =="

    for ($i = 1; $i -le $Rx; $i++) {
        $m = New-Object byte[] 16
        $rc = [Oti]::ReadMsg($h, $m)
        $cnt = ([int]$m[1] -shl 8) -bor [int]$m[2]
        $hex = ($m | ForEach-Object { $_.ToString('x2') }) -join ' '
        Write-Host ("  [{0}] 消息 rc={1,-4} type=0x{2:x2}  {3}   (be16@1={4})" -f $i, $rc, $m[0], $hex, $cnt)

        if ($rc -eq 0 -and $m[0] -eq 0x05 -and $cnt -gt 0) {
            $want = [Math]::Min($cnt, 8)
            for ($f = 1; $f -le $want; $f++) {
                $fr = New-Object byte[] 65536
                $rc2 = [Oti]::ReadFrame28($h, $fr)
                $chk = [Oti]::FrameCheck($fr)
                $nz = 0; for ($k = 0; $k -lt 65536; $k++) { if ($fr[$k] -ne 0) { $nz++ } }
                $head = ($fr[0..19] | ForEach-Object { $_.ToString('x2') }) -join ' '
                Write-Host ("      帧#{0} rc={1} 判定={2} 非零={3}" -f $f, $rc2, $(if ($chk -eq 1) { '有效' } elseif ($chk -eq 0) { '空闲' } else { '非法' }), $nz)
                Write-Host ("        头20B: {0}" -f $head)
                $asc = -join ($fr[20..(20+63)] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
                Write-Host ("        体@20 前64字节 ASCII: |{0}|" -f $asc)
                $t = [uint16]0; $sq = [uint32]0; $pl = $null
                if ([Oti]::Decode($fr, 65516, [ref]$t, [ref]$sq, [ref]$pl)) {
                    Write-Host ("        → 解出协议消息 type={0} seq={1} 载荷={2} 字节" -f $t, $sq, $pl.Length)
                }
            }
        }
        Start-Sleep -Milliseconds 200
    }
    [Oti]::CloseDevice($h)
    exit 0
}

# ============================ 通用 CDB 探针 ============================
if ($Raw) {
    $dev = if ($Device) { $Device } else { '\\.\H:' }
    $h = [Oti]::OpenDevice($dev, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($dev, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "$dev 打不开"; exit 1 }
    $st = [byte]0; $se = $null
    $list = @($Raw -split ';' | Where-Object { $_ -match '[0-9a-fA-F]' })
    foreach ($one in $list) {
    $cdb = New-Object byte[] 16
    $hx = ($one -replace '[^0-9a-fA-F]', '')
    $n = [Math]::Min(16, [int]($hx.Length / 2))
    for ($i = 0; $i -lt $n; $i++) { $cdb[$i] = [Convert]::ToByte($hx.Substring($i * 2, 2), 16) }
    if ($cdb[14] -eq 0 -and $cdb[15] -eq 0) { $cdb[14] = 0x4F; $cdb[15] = 0x54 }
    $cdbs = ($cdb | ForEach-Object { $_.ToString('x2') }) -join ' '
    if ($RawDir -eq 'none') {
        $rc = [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)
        Write-Host ("  CDB {0}  无数据阶段 rc={1} scsi={2}" -f $cdbs, $rc, $st)
    } elseif ($RawDir -eq 'out') {
        $buf = New-Object byte[] $RawLen
        $rc = [Oti]::Exec($h, $cdb, $buf, $true, [ref]$st, [ref]$se)
        Write-Host ("  CDB {0}  OUT {1}B rc={2} scsi={3}" -f $cdbs, $RawLen, $rc, $st)
    } else {
        $buf = New-Object byte[] $RawLen
        $rc = [Oti]::Exec($h, $cdb, $buf, $false, [ref]$st, [ref]$se)
        $nz = 0; for ($k = 0; $k -lt $RawLen; $k++) { if ($buf[$k] -ne 0) { $nz++ } }
        $show = [Math]::Min(31, $RawLen - 1)
        $hex = ($buf[0..$show] | ForEach-Object { $_.ToString('x2') }) -join ' '
        Write-Host ("  CDB {0}  IN {1}B rc={2} scsi={3} 非零={4}" -f $cdbs, $RawLen, $rc, $st, $nz)
        Write-Host ("    前32B: {0}" -f $hex)
    }
    Write-Host ("  sense key=0x{0:x} ASC=0x{1:x} ASCQ=0x{2:x}" -f ($se[2] -band 0xf), $se[12], $se[13])
    Start-Sleep -Milliseconds 700
    }
    [Oti]::CloseDevice($h)
    exit 0
}

if ($WriteSweep) {
    $dev = if ($Device) { $Device } else { '\\.\H:' }
    $h = [Oti]::OpenDevice($dev, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($dev, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "$dev 打不开"; exit 1 }
    Write-Host "  设备: $dev"
    $st = [byte]0; $se = $null
    $frame = New-Object byte[] 65536

    Write-Host "== A. 0xD9 CDB[1]=0x00..0x3F，OUT 64KB 全零帧 =="
    $acc = @()
    foreach ($b1 in 0x00..0x3F) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = [byte]$b1; $cdb[2] = 0xFF
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $rc = [Oti]::Exec($h, $cdb, $frame, $true, [ref]$st, [ref]$se)
        if ($rc -eq 0) { $acc += ('0x{0:x2}' -f $b1) }
        elseif ($b1 -in 0x2A, 0x33, 0x34, 0x36, 0x60) {
            Write-Host ("     D9/{0:x2}  rc={1,-6} scsi={2} key=0x{3:x} ASC=0x{4:x} ASCQ=0x{5:x}" -f $b1, $rc, $st, $se[0], $se[12], $se[13])
        }
    }
    Write-Host ("   64KB OUT 被接受: {0}" -f ($(if ($acc.Count) { $acc -join ', ' } else { '（无）' })))

    Write-Host "== B. 0xD8 CDB[1]=0x00..0x03，OUT 64KB =="
    foreach ($b1 in 0x00..0x03) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD8; $cdb[1] = [byte]$b1; $cdb[2] = 0x03
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $rc = [Oti]::Exec($h, $cdb, $frame, $true, [ref]$st, [ref]$se)
        Write-Host ("     D8/{0:x2}/03  rc={1,-6} scsi={2} key=0x{3:x} ASC=0x{4:x} ASCQ=0x{5:x}" -f $b1, $rc, $st, $se[0], $se[12], $se[13])
    }

    Write-Host "== C. 0xD9/0x2A 的 CDB[2] 扫描，OUT 64KB =="
    $acc2 = @()
    foreach ($b2 in 0x00..0x1F + 0xFF) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = 0x2A; $cdb[2] = [byte]$b2
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $rc = [Oti]::Exec($h, $cdb, $frame, $true, [ref]$st, [ref]$se)
        if ($rc -eq 0) { $acc2 += ('0x{0:x2}' -f $b2) }
    }
    Write-Host ("   CDB[2] 被接受: {0}" -f ($(if ($acc2.Count) { $acc2 -join ', ' } else { '（无）' })))

    Write-Host "== D. 0xD9/0x2A/0xFF 不同传输长度 =="
    foreach ($len in 2, 512, 4096, 65496, 65536) {
        $buf = New-Object byte[] $len
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = 0x2A; $cdb[2] = 0xFF
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $rc = [Oti]::Exec($h, $cdb, $buf, $true, [ref]$st, [ref]$se)
        Write-Host ("     {0,-6}B rc={1,-6} scsi={2} key=0x{3:x} ASC=0x{4:x}" -f $len, $rc, $st, $se[0], $se[12])
    }

    Write-Host "== E. 读回验证（0xD8/0x00/0x03 held=1）=="
    $rb = New-Object byte[] 65536
    $cdb = New-Object byte[] 16
    $cdb[0] = 0xD8; $cdb[1] = 0x00; $cdb[2] = 0x03; $cdb[3] = 0; $cdb[4] = 1
    $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
    $rc = [Oti]::Exec($h, $cdb, $rb, $false, [ref]$st, [ref]$se)
    $nz = 0; for ($k = 0; $k -lt 65536; $k++) { if ($rb[$k] -ne 0) { $nz++ } }
    $hex = ($rb[0..19] | ForEach-Object { $_.ToString('x2') }) -join ' '
    Write-Host ("     rc={0} 非零={1} 头20B: {2}" -f $rc, $nz, $hex)
    [Oti]::CloseDevice($h)
    exit 0
}

if ($Sweep) {
    $st = [byte]0; $se = $null
    Write-Host "== 打开方式 =="
    $cands = @('\\.\F:', '\\.\H:')
    foreach ($pd in 1..6) { $cands += ('\\.\PhysicalDrive' + $pd) }
    foreach ($c in $cands) {
        foreach ($mode in 0, 1, 2) {     # 0=access0  1=GENERIC_READ  2=R/W
            $acc = if ($mode -eq 0) { [uint32]0 } elseif ($mode -eq 1) { [uint32]2147483648 } else { [uint32]3221225472 }
            $h = [Oti]::OpenDeviceRaw($c, $acc)
            if ([Oti]::IsValid($h)) {
                $buf = New-Object byte[] 64
                $rc = [Oti]::InfoRead($h, $buf)
                Write-Host ("  {0,-20} access=0x{1:x} 打开OK  信息块 rc={2}" -f $c, $acc, $rc)
                [Oti]::CloseDevice($h)
            }
            elseif ($acc -ne 0) { }
        }
    }

    Write-Host "== 写命令扫描（小载荷 2 字节，打印 SCSI 状态与 sense）=="
    $dev = if ($Device) { $Device } else { '\\.\H:' }
    $h = [Oti]::OpenDevice($dev, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($dev, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "  $dev 打不开"; exit 1 }
    Write-Host "  设备: $dev"
    $cases = @(
        @{ n = 'D9/2A/FF 厂商数据写'; op = 0xD9; b1 = 0x2A; b2 = 0xFF; b3 = 0x00; b4 = 0x00 },
        @{ n = 'D9/2A/00';           op = 0xD9; b1 = 0x2A; b2 = 0x00; b3 = 0x00; b4 = 0x00 },
        @{ n = 'D9/2A/01';           op = 0xD9; b1 = 0x2A; b2 = 0x01; b3 = 0x00; b4 = 0x00 },
        @{ n = 'D9/33 HID-1';        op = 0xD9; b1 = 0x33; b2 = 0x00; b3 = 0x00; b4 = 0x00 },
        @{ n = 'D9/34 HID-2';        op = 0xD9; b1 = 0x34; b2 = 0x00; b3 = 0x00; b4 = 0x00 },
        @{ n = 'D9/36 HID-3';        op = 0xD9; b1 = 0x36; b2 = 0x00; b3 = 0x00; b4 = 0x00 },
        @{ n = 'D9/00';              op = 0xD9; b1 = 0x00; b2 = 0x00; b3 = 0x00; b4 = 0x00 },
        @{ n = 'F0/05/02 USB重启';   op = 0xF0; b1 = 0x05; b2 = 0x02; b3 = 0x00; b4 = 0x0A },
        @{ n = 'F0/30';              op = 0xF0; b1 = 0x30; b2 = 0x00; b3 = 0x00; b4 = 0x00 },
        @{ n = 'F0/31 锁';           op = 0xF0; b1 = 0x31; b2 = 0x01; b3 = 0x00; b4 = 0x00 },
        @{ n = 'DA/2A';              op = 0xDA; b1 = 0x2A; b2 = 0x00; b3 = 0x00; b4 = 0x00 },
        @{ n = 'F1/2A';              op = 0xF1; b1 = 0x2A; b2 = 0x00; b3 = 0x00; b4 = 0x00 }
    )
    foreach ($c in $cases) {
        $cdb = New-Object byte[] 16
        $cdb[0] = [byte]$c.op; $cdb[1] = [byte]$c.b1; $cdb[2] = [byte]$c.b2
        $cdb[3] = [byte]$c.b3; $cdb[4] = [byte]$c.b4
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $data = New-Object byte[] 2
        $st = [byte]0; $se = $null
        $rc = [Oti]::Exec($h, $cdb, $data, $true, [ref]$st, [ref]$se)
        $key = if ($se -ne $null -and $se.Length -gt 2) { $se[2] -band 0x0F } else { 0 }
        $asc = if ($se -ne $null -and $se.Length -gt 13) { $se[12] } else { 0 }
        $ok = if ($rc -eq 0) { '接受' } else { '拒绝' }
        Write-Host ("  {0,-20} rc={1,-4} scsi={2} key=0x{3:x} ASC=0x{4:x}  {5}" -f $c.n, $rc, $st, $key, $asc, $ok)
    }
    [Oti]::CloseDevice($h)
    exit 0
}

# ============================ 分页读（把每个 CDB[2] 页读出来看结构） ============================
if ($Pages) {
    $dev = if ($Device) { $Device } else { '\\.\F:' }
    $h = [Oti]::OpenDevice($dev, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($dev, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "打不开 $dev"; exit 1 }
    Write-Host "设备: $dev"
    $st = [byte]0; $se = $null
    foreach ($sel in 0..7) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD8; $cdb[1] = 0x00; $cdb[2] = [byte]$sel; $cdb[3] = 0; $cdb[4] = 1
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $buf = New-Object byte[] 8192
        $rc = [Oti]::Exec($h, $cdb, $buf, $false, [ref]$st, [ref]$se)
        $nz = 0; for ($k = 0; $k -lt $buf.Length; $k++) { if ($buf[$k] -ne 0) { $nz++ } }
        $head = ($buf[0..31] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $asc = -join ($buf[0..31] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
        Write-Host ("  page {0}: rc={1} scsi={2} 非零={3}" -f $sel, $rc, $st, $nz)
        Write-Host ("      hex: {0}" -f $head)
        Write-Host ("      asc: {0}" -f $asc)
        # 把非零区间的 ASCII 串抓出来（可能有版本/侧别/状态文本）
        $sb = New-Object System.Text.StringBuilder
        $strs = @()
        for ($k = 0; $k -lt $buf.Length; $k++) {
            if ($buf[$k] -ge 32 -and $buf[$k] -lt 127) { [void]$sb.Append([char]$buf[$k]) }
            else { if ($sb.Length -ge 4) { $strs += $sb.ToString() }; [void]$sb.Clear() }
        }
        if ($sb.Length -ge 4) { $strs += $sb.ToString() }
        if ($strs.Count -gt 0) { Write-Host ("      串: {0}" -f (($strs | Select-Object -First 8) -join ' | ')) }
    }
    [Oti]::CloseDevice($h)
    exit 0
}

# ============================ 状态矩阵探测：找出"设备肯回真帧"的前置条件 ============================
if ($Probe) {
    $dev = if ($Device) { $Device } else { '\\.\F:' }
    $h = [Oti]::OpenDevice($dev, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($dev, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "打不开 $dev"; exit 1 }
    Write-Host "设备: $dev"
    $st = [byte]0; $se = $null

    function Show([string]$label) {
        $fr = New-Object byte[] 65536
        $rc = [Oti]::ReadFrame($h, $fr, 1)
        $chk = [Oti]::FrameCheck($fr)
        $nz = 0; for ($k = 0; $k -lt 65536; $k++) { if ($fr[$k] -ne 0) { $nz++ } }
        $head = ($fr[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $v = if ($chk -eq 1) { '有效' } elseif ($chk -eq 0) { '空闲' } else { '非法' }
        Write-Host ("  [{0}] 读 rc={1} 判定={2} 非零={3} 头={4}" -f $label, $rc, $v, $nz, $head)
    }

    Write-Host "== 1) 基线 =="
    Show "baseline-1"; Show "baseline-2"

    Write-Host "== 2) 独占锁 0xF0/0x31 =="
    $cdb = New-Object byte[] 16
    $cdb[0] = 0xF0; $cdb[1] = 0x31; $cdb[2] = 1; $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
    $rc = [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)
    Write-Host "  lock rc=$rc scsi=$st"
    Show "after-lock"

    Write-Host "== 3) 写全零帧（厂商 SendDummyData）=="
    $z = New-Object byte[] 65536
    $rc = [Oti]::WriteFrame($h, $z)
    Write-Host "  dummy write rc=$rc"
    Show "after-dummy"

    Write-Host "== 4) 写我们的 PING 帧（合法帧：首尾头一致）=="
    $cmd = New-Object byte[] 64
    [Oti]::Put32($cmd, 0, [uint32]0x314C544F); [Oti]::Put16($cmd, 4, 5)
    [Oti]::Put32($cmd, 8, 1); [Oti]::Put32($cmd, 12, 4); [Oti]::Put32($cmd, 20, 0x11223344)
    $crc = [Oti]::Crc32($cmd, 0, 16); $crc = [Oti]::Crc32Append($crc, $cmd, 20, 4)
    [Oti]::Put32($cmd, 16, $crc)
    $fr = New-Object byte[] 65536
    [Oti]::FramePack($cmd, 24, $fr)
    $rc = [Oti]::WriteFrame($h, $fr)
    Write-Host "  ping write rc=$rc 帧自检=$([Oti]::FrameCheck($fr))"
    Show "after-ping"

    Write-Host "== 5) 远端主机状态写 0xD8/0x01 =="
    $cdb = New-Object byte[] 16
    $cdb[0] = 0xD8; $cdb[1] = 0x01; $cdb[2] = 1; $cdb[4] = 2
    $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
    $pl = New-Object byte[] 2
    $rc = [Oti]::Exec($h, $cdb, $pl, $true, [ref]$st, [ref]$se)
    Write-Host "  rstatus write rc=$rc scsi=$st"
    Show "after-rstatus"

    Write-Host "== 6) 读通道选择子 CDB[2] 变体 =="
    foreach ($sel in 0,1,2,3,4) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD8; $cdb[1] = 0x00; $cdb[2] = [byte]$sel; $cdb[4] = 1
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $b = New-Object byte[] 65536
        $rc = [Oti]::Exec($h, $cdb, $b, $false, [ref]$st, [ref]$se)
        $nz = 0; for ($k = 0; $k -lt 65536; $k++) { if ($b[$k] -ne 0) { $nz++ } }
        $head = ($b[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' '
        Write-Host ("  CDB[2]={0} rc={1} scsi={2} 非零={3} 头={4}" -f $sel, $rc, $st, $nz, $head)
    }

    Write-Host "== 7) 写失败原因（打印 sense：key/ASC/ASCQ）=="
    foreach ($d in '\\.\F:', '\\.\H:') {
        $hh = [Oti]::OpenDevice($d, $false)
        if (-not [Oti]::IsValid($hh)) { $hh = [Oti]::OpenDevice($d, $true) }
        if (-not [Oti]::IsValid($hh)) { Write-Host "  $d 打不开"; continue }
        foreach ($len in 65536, 1024, 2) {
            $cdb = New-Object byte[] 16
            $cdb[0] = 0xD9; $cdb[1] = 0x2A; $cdb[2] = 0xFF
            $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
            $data = New-Object byte[] $len
            $st = [byte]0; $se = $null
            $rc = [Oti]::Exec($hh, $cdb, $data, $true, [ref]$st, [ref]$se)
            $key = if ($se -ne $null -and $se.Length -gt 2) { $se[2] -band 0x0F } else { -1 }
            $asc = if ($se -ne $null -and $se.Length -gt 13) { $se[12] } else { -1 }
            $ascq = if ($se -ne $null -and $se.Length -gt 14) { $se[13] } else { -1 }
            $slen = if ($se -ne $null -and $se.Length -gt 7) { $se[7] } else { 0 }
            Write-Host ("  {0} 写 0xD9/0x2A 长度={1,-6} rc={2} scsi={3} sense_len={4} key=0x{5:x} ASC=0x{6:x} ASCQ=0x{7:x}" -f $d, $len, $rc, $st, $slen, $key, $asc, $ascq)
        }
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xD9; $cdb[1] = 0x60; $cdb[2] = 2; $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $b = New-Object byte[] 16
        $st = [byte]0; $se = $null
        $rc = [Oti]::Exec($hh, $cdb, $b, $false, [ref]$st, [ref]$se)
        $key = if ($se -ne $null -and $se.Length -gt 2) { $se[2] -band 0x0F } else { -1 }
        $asc = if ($se -ne $null -and $se.Length -gt 13) { $se[12] } else { -1 }
        Write-Host ("  {0} 读 0xD9/0x60 rc={1} scsi={2} key=0x{3:x} ASC=0x{4:x} buf={5}" -f $d, $rc, $st, $key, $asc, (($b[0..7] | ForEach-Object { $_.ToString('x2') }) -join ' '))
        [Oti]::CloseDevice($hh)
    }

    Write-Host "== 8) 解锁 =="
    $cdb = New-Object byte[] 16
    $cdb[0] = 0xF0; $cdb[1] = 0x31; $cdb[2] = 0; $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
    $rc = [Oti]::Exec($h, $cdb, $null, $true, [ref]$st, [ref]$se)
    Write-Host "  unlock rc=$rc"
    [Oti]::CloseDevice($h)
    exit 0
}

# ============================ 原始探测（读设备信息块 / 连续读帧 / 可选写帧） ============================
if ($Dump) {
    if (-not $Device) { Write-Host "需要 -Device"; exit 2 }
    $h = [Oti]::OpenDevice($Device, $false)
    if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($Device, $true) }
    if (-not [Oti]::IsValid($h)) { Write-Host "打不开 $Device"; exit 1 }
    Write-Host "设备: $Device"

    Write-Host "== 设备信息块（0xF0/0x00，最多 64 字节）=="
    for ($sel = 0; $sel -le 3; $sel++) {
        $cdb = New-Object byte[] 16
        $cdb[0] = 0xF0; $cdb[1] = 0x00; $cdb[2] = [byte]$sel
        $cdb[14] = [byte][char]'O'; $cdb[15] = [byte][char]'T'
        $buf = New-Object byte[] 64
        $st = [byte]0; $se = $null
        $rc = [Oti]::Exec($h, $cdb, $buf, $false, [ref]$st, [ref]$se)
        $hex = ($buf[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $asc = -join ($buf[0..15] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
        Write-Host ("  CDB[2]={0} rc={1} scsi={2}  {3}  |{4}|" -f $sel, $rc, $st, $hex, $asc)
    }

    Write-Host "== 连续读帧（0xD8/0x00/0x03 held=1）=="
    for ($i = 1; $i -le $Frames; $i++) {
        $fr = New-Object byte[] 65536
        $st = [byte]0; $se = $null
        $rc = [Oti]::ReadFrame($h, $fr, 1)
        $chk = [Oti]::FrameCheck($fr)
        $head = ($fr[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $tail = ($fr[65516..65535] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $hasOTIL = $false
        for ($k = 0; $k -lt 65532; $k++) { if ($fr[$k] -eq 0x4F -and $fr[$k+1] -eq 0x54 -and $fr[$k+2] -eq 0x49 -and $fr[$k+3] -eq 0x4C) { $hasOTIL = $true; break } }
        $nz = 0; for ($k = 0; $k -lt 65536; $k++) { if ($fr[$k] -ne 0) { $nz++ } }
        Write-Host ("  #{0} rc={1} 帧判定={2} 非零字节={3} 含'OTIL'={4}" -f $i, $rc, $chk, $nz, $hasOTIL)
        Write-Host ("      头: {0}" -f $head)
        Write-Host ("      尾: {0}" -f $tail)
    }

    if ($Write) {
        Write-Host "== 写一帧（0xD9/0x2A/0xFF，内容=我们的 PING 消息）=="
        $cmd = New-Object byte[] 64
        [Oti]::Put32($cmd, 0, [uint32]0x314C544F)
        [Oti]::Put16($cmd, 4, 5); [Oti]::Put32($cmd, 8, 1); [Oti]::Put32($cmd, 12, 4)
        [Oti]::Put32($cmd, 20, 0x11223344)
        $crc = [Oti]::Crc32($cmd, 0, 16); $crc = [Oti]::Crc32Append($crc, $cmd, 20, 4)
        [Oti]::Put32($cmd, 16, $crc)
        $n = 24
        $fr = New-Object byte[] 65536
        [Oti]::FramePack($cmd, $n, $fr)
        $st = [byte]0; $se = $null
        $rc = [Oti]::WriteFrame($h, $fr)
        Write-Host ("  写帧 rc={0} scsi={1} 帧自检={2}" -f $rc, 0, [Oti]::FrameCheck($fr))
    }
    [Oti]::CloseDevice($h)
    exit 0
}

# ============================ 跨实现线上兼容测试（不需要管理员） ============================
if ($FrameTest) {
    $fail = 0
    function Chk($cond, $msg) {
        if ($cond) { Write-Host "  [PASS] $msg" } else { Write-Host "  [FAIL] $msg"; $script:fail++ }
    }
    Write-Host "== 跨实现线上兼容测试 =="
    $all = [IO.File]::ReadAllBytes($FrameTest)
    $n = [math]::Floor($all.Length / 65536)
    Chk ($n -ge 2) "读到 $n 帧（每帧 65536 字节）"
    for ($i = 0; $i -lt $n; $i++) {
        $frame = New-Object byte[] 65536
        [Array]::Copy($all, $i * 65536, $frame, 0, 65536)
        $chk = [Oti]::FrameCheck($frame)
        $len = [Oti]::FrameBodyLen($frame)
        $body = New-Object byte[] $len
        [Array]::Copy($frame, 20, $body, 0, $len)
        $t = [uint16]0; $sq = [uint32]0; $pl = $null
        $ok = [Oti]::Decode($body, $len, [ref]$t, [ref]$sq, [ref]$pl)
        $desc = ""
        $valOk = $true
        if ($ok -and $t -eq 1 -and $pl.Length -ge 4) {
            $code = [BitConverter]::ToUInt16($pl, 0)
            $desc = "KEY code=$code value=$($pl[2]) seq=$sq"
            $valOk = ($code -eq 30 -and $pl[2] -eq 1 -and $sq -eq 7)
        } elseif ($ok -and $t -eq 2 -and $pl.Length -ge 8) {
            $dx = [BitConverter]::ToInt16($pl, 0); $dy = [BitConverter]::ToInt16($pl, 2)
            $wh = [BitConverter]::ToInt16($pl, 4); $btn = [BitConverter]::ToUInt16($pl, 6)
            $desc = "MOUSE dx=$dx dy=$dy wheel=$wh btn=$btn seq=$sq"
            $valOk = ($dx -eq -1234 -and $dy -eq 567 -and $wh -eq -1 -and $btn -eq 3 -and $sq -eq 8)
        } else { $desc = "type=$t len=$len" }
        Chk (($chk -eq 1) -and $ok -and $valOk) "帧#$($i+1) $desc"
    }

    if ($FrameOut) {
        $buf = New-Object byte[] (2 * 65536)
        $frame = New-Object byte[] 65536
        $k = [Oti]::KeyPayload(31, 0)                       # KEY code=31 抬起
        $msg = [Oti]::Encode(1, 9, $k, $k.Length)
        [Oti]::FramePack($msg, $msg.Length, $frame)
        [Array]::Copy($frame, 0, $buf, 0, 65536)
        $mp = [Oti]::MousePayload(-5, 9, 0, 0)              # MOUSE dx=-5 dy=9
        $msg = [Oti]::Encode(2, 10, $mp, $mp.Length)
        [Oti]::FramePack($msg, $msg.Length, $frame)
        [Array]::Copy($frame, 0, $buf, 65536, 65536)
        [IO.File]::WriteAllBytes($FrameOut, $buf)
        Write-Host "  已写出 2 帧到 $FrameOut（KEY code=31 seq=9 / MOUSE dx=-5 dy=9 seq=10）"
    }
    Write-Host ""
    if ($fail -eq 0) { Write-Host "全部通过"; exit 0 } else { Write-Host "有失败（$fail 项）"; exit 1 }
}

# ============================ 自检（不需要管理员） ============================
if ($Selftest) {
    $fail = 0
    function Chk($cond, $msg) {
        if ($cond) { Write-Host "  [PASS] $msg" } else { Write-Host "  [FAIL] $msg"; $script:fail++ }
    }
    Write-Host "== Windows agent 协议自检 =="

    # KEY 往返
    $p = [Oti]::KeyPayload(30, 1)
    $m = [Oti]::Encode(1, 7, $p, $p.Length)
    $t = [uint16]0; $s = [uint32]0; $pl = $null
    $ok = [Oti]::Decode($m, $m.Length, [ref]$t, [ref]$s, [ref]$pl)
    Chk ($ok -and $t -eq 1 -and $s -eq 7 -and $pl.Length -eq 4 -and $pl[0] -eq 30 -and $pl[2] -eq 1) "KEY 编解码往返（type=$t seq=$s code=$($pl[0])）"

    # CRC 拦截篡改
    $m2 = $m.Clone(); $m2[20] = $m2[20] -bxor 0xFF
    $ok2 = [Oti]::Decode($m2, $m2.Length, [ref]$t, [ref]$s, [ref]$pl)
    Chk (-not $ok2) "载荷被篡改 → 拒收"

    # 鼠标负位移
    $p = [Oti]::MousePayload(-1234, 567, -1, 3)
    $m = [Oti]::Encode(2, 1, $p, $p.Length)
    $ok3 = [Oti]::Decode($m, $m.Length, [ref]$t, [ref]$s, [ref]$pl)
    $dx = [BitConverter]::ToInt16($pl, 0)
    Chk ($ok3 -and $t -eq 2 -and $dx -eq -1234) "鼠标负位移往返（dx=$dx）"

    # 帧：打包/校验/空闲
    $frame = New-Object byte[] 65536
    $body = [Oti]::Encode(1, 1, [Oti]::KeyPayload(30,1), 4)
    [Oti]::FramePack($body, $body.Length, $frame)
    Chk ([Oti]::FrameCheck($frame) -eq 1) "普通帧判定为有效"
    Chk ([Oti]::FrameBodyLen($frame) -eq $body.Length) "帧内长度字段正确（$([Oti]::FrameBodyLen($frame))）"
    $z = New-Object byte[] 65536
    Chk ([Oti]::FrameCheck($z) -eq 0) "全零帧判定为空闲"
    $frame[65516] = $frame[65516] -bxor 0xFF
    Chk ([Oti]::FrameCheck($frame) -eq -1) "尾部头被篡改 → 非法"

    # SPTI 结构体布局（对不齐会在真机上以难懂的 err 失败）
    Chk ([Oti]::StructSize -eq 56) "SCSI_PASS_THROUGH_DIRECT 大小 = $([Oti]::StructSize)（期望 56）"
    Chk ([Oti]::OffsetOf('DataTransferLength') -eq 12) "DataTransferLength 偏移 = $([Oti]::OffsetOf('DataTransferLength'))（期望 12）"
    Chk ([Oti]::OffsetOf('DataBuffer') -eq 24) "DataBuffer 偏移 = $([Oti]::OffsetOf('DataBuffer'))（期望 24）"
    Chk ([Oti]::OffsetOf('SenseInfoOffset') -eq 32) "SenseInfoOffset 偏移 = $([Oti]::OffsetOf('SenseInfoOffset'))（期望 32）"
    Chk ([Oti]::OffsetOf('Cdb') -eq 36) "Cdb 偏移 = $([Oti]::OffsetOf('Cdb'))（期望 36）"

    Write-Host ""
    if ($fail -eq 0) { Write-Host "全部通过"; exit 0 } else { Write-Host "有失败（$fail 项）"; exit 1 }
}

# ============================ 设备探测 ============================
function New-Cdb([byte]$op, [byte]$b1, [byte]$b2) {
    $c = New-Object byte[] 16
    $c[0] = $op; $c[1] = $b1; $c[2] = $b2; $c[14] = [byte][char]'O'; $c[15] = [byte][char]'T'
    return $c
}

if ($Scan) {
    Write-Host "== 探测哪些设备能应答厂商命令（0xF0/0x00 设备信息块）=="
    $cands = @('\\.\F:', '\\.\H:', '\\.\PhysicalDrive1', '\\.\PhysicalDrive2', '\\.\PhysicalDrive3')
    $found = 0
    foreach ($c in $cands) {
        $h = [Oti]::OpenDevice($c, $ReadOnly.IsPresent)
        if (-not [Oti]::IsValid($h)) { $h = [Oti]::OpenDevice($c, -not $ReadOnly.IsPresent) }
        if (-not [Oti]::IsValid($h)) {
            Write-Host ("  {0,-22} 打不开（err={1}）" -f $c, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
            continue
        }
        $buf = New-Object byte[] 64
        $rc = [Oti]::InfoRead($h, $buf)
        $hex = ($buf[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $mark = if ($rc -eq 0) { '<== 可作数据通道' } else { '' }
        Write-Host ("  {0,-22} 信息块 rc={1} info={2} {3}" -f $c, $rc, $hex, $mark)
        if ($rc -eq 0) {
            $found++
            $mode = [byte]0
            $mrc = [Oti]::DevMode($h, [ref]$mode)
            Write-Host ("      设备模式 rc={0} mode={1}" -f $mrc, $mode)
            $fr = New-Object byte[] 65536
            $frc = [Oti]::ReadFrame($h, $fr, 1)
            if ($frc -eq 0) {
                $chk = [Oti]::FrameCheck($fr)
                $verdict = if ($chk -eq 1) { '有效数据帧' } elseif ($chk -eq 0) { '空闲帧（对端无数据）' } else { '非法帧' }
                $fh = ($fr[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' '
                Write-Host ("      数据管道读 rc=0 -> {0}  头16字节={1}" -f $verdict, $fh)
            } else {
                Write-Host ("      数据管道读 rc={0}（0x{0:x}）" -f $frc)
            }
            Write-Host ("      建议: -Device '{0}'" -f $c)
        }
        [Oti]::CloseDevice($h)
    }
    if ($found -eq 0) { Write-Host "  没找到可应答的设备；确认以管理员运行（SPTI 需要写权限）"; exit 1 }
    exit 0
}

# ---- 设备自动发现（拔插 / 盘符变化后自愈）------------------------------------
# 为什么需要：写死 '\\.\H:' 时，只要线缆重新枚举后盘符变了（replug/重启很常见），
# 代理就会一直"读取失败→重连→还是失败"，而设备其实好好的。
# 这里把候选设备**动态枚举**出来，谁应答厂商命令（0xF0/0x00 设备信息块）就用谁。
function Find-CableDevice {
    # 只枚举**盘符**（不碰 \\.\PhysicalDriveN：那是整块物理盘，权限/副作用都不该碰）：
    # CD-ROM(5) 与可移动(2) 优先 —— 线缆就是这两种（D: 厂商 ISO / H: 可移动卷）。
    $letters = New-Object System.Collections.ArrayList
    try {
        foreach ($d in [IO.DriveInfo]::GetDrives()) {
            if ($d.Name -notmatch '^[A-Za-z]:\\$') { continue }
            $pri = 1
            if ($d.DriveType -eq [IO.DriveType]::CdRom -or $d.DriveType -eq [IO.DriveType]::Removable) { $pri = 0 }
            [void]$letters.Add([pscustomobject]@{ Path = '\\.\' + $d.Name.Substring(0, 2); Pri = $pri })
        }
    } catch { }
    $cands = @($letters | Sort-Object Pri | ForEach-Object { $_.Path })
    foreach ($c in $cands) {
        $h = [Oti]::OpenDevice($c, $false)
        if (-not [Oti]::IsValid($h)) { continue }
        $buf = New-Object byte[] 64
        $rc = [Oti]::InfoRead($h, $buf)
        [Oti]::CloseDevice($h)
        if ($rc -eq 0) {
            Write-Host ("  自动发现线缆设备: {0}（信息块 {1}）" -f $c, (($buf[0..3] | ForEach-Object { $_.ToString('x2') }) -join ' '))
            return $c
        }
    }
    return ''
}

# ============================ 正常模式 ============================
$useTcp = ($Tcp -ne '')
$h = [IntPtr]::Zero
if ($useTcp) {
    $parts = $Tcp -split ':'
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect($parts[0], [int]$parts[1])
    $client.ReceiveTimeout = 100          # 必须在 GetStream() 之前设，NetworkStream 才会继承读超时
    $stream = $client.GetStream()
    try { $stream.ReadTimeout = 100 } catch {}
    Write-Host "已连接 TCP $Tcp（与 Linux 端 otikm 的 fd 传输对跑；不需要 SPTI/管理员）"
} else {
    if (-not $Device) {
        Write-Host "未指定 -Device → 自动发现线缆设备…"
        $Device = Find-CableDevice
        if (-not $Device) { Write-Host "没找到线缆设备；可用 -Scan 诊断，或 -Tcp host:port"; exit 2 }
    }
    $h = [Oti]::OpenDevice($Device)
    if (-not [Oti]::IsValid($h)) {
        Write-Host ("打不开 {0}（err={1}）—— 试试自动发现" -f $Device, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        $alt = Find-CableDevice
        if ($alt -and $alt -ne $Device) { $Device = $alt; $h = [Oti]::OpenDevice($Device) }
    }
    if (-not [Oti]::IsValid($h)) {
        Write-Host ("打不开 {0} —— SPTI 需要管理员权限，或被别的程序独占" -f $Device)
        exit 1
    }
    Write-Host "设备已打开: $Device"
    if ($useCable) { Write-Host "线缆对等循环模式（两条管道 + 授权门控发送）" }
    Write-Host ("鼠标注入模式: {0}" -f $(if ($MouseMode -eq 'absolute') { 'absolute（绝对坐标，1:1 无加速）' } else { 'native（相对位移，走 Windows 指针速度/加速）' }))
}

# ============================ 线缆对等循环（两条管道 + 授权门控）============================
# 协议依据（NOTES §33/§34）：
#   消息管道 0xD8/0x00/0x03 + IN 16B，CDB[3..4]=本机待发帧数
#   0x05 → be16(m[1..2]) 帧数 → 0xD9/0x28/0x64 读 64KB 帧
#   0x06/0x07 → 发送授权 → 必须【立刻】用 0xD9/0x2A/0xFF 写帧
#   0x01/0x00/0x08/0x10 → 复位（链路未就绪/对端未消费）
try { [void][Oti]::SetProcessDPIAware() } catch {}   # DPI 感知：坐标全部按物理像素，避免缩放错位
$useCable = ($Cable -and -not $useTcp)
$script:txq = New-Object System.Collections.Queue
$script:rxq = New-Object System.Collections.Queue
$script:cableResets = 0
$script:cableFail = 0
$script:cableTxDropped = 0
# 会话自愈用的时间戳/计数（判据见 Pump-Cable 顶部）
$script:lastFrameMs = Get-Date      # 上次**收到帧**的时刻
$script:lastDrainMs = Get-Date      # 上次发送队列**减少**的时刻
$script:lastTxqCount = 0

# 缓冲区只分配一次（教训：原来每轮循环都 New-Object byte[] 65536，
# 每秒上千次分配造成 GC 风暴，整台机器都会变卡）
$script:frameBuf = New-Object byte[] 65536
$script:msgBuf = New-Object byte[] 16

function Pump-Cable {
    if (-not $useCable) { return $false }
    # ---- 会话自愈（第 46 轮续）：消息管道"看起来正常"但数据完全不动时必须主动重开 ----
    # 真机故障（re/NOTES.md §57）：ReadMsgP 一直成功（cableFail 恒为 0），但对端写命令被拒
    # rc=524546、帧静默丢失 → 下面的 cableFail>=60 重连**永远不触发**，链路卡死只能人工拔插。
    # 判据（两条都要满足，避免"对端本来是零软件、根本没帧"时误重开）：
    #   ① 20 秒没收到任何帧；且 ② 发送队列积压超过 10 秒没有减少（有东西发不出去）。
    $nowT = Get-Date
    if ($script:txq.Count -lt $script:lastTxqCount) { $script:lastDrainMs = $nowT }
    $script:lastTxqCount = $script:txq.Count
    if ($script:txq.Count -eq 0) { $script:lastDrainMs = $nowT }
    $idleRx = ($nowT - $script:lastFrameMs).TotalSeconds
    $stuckTx = ($nowT - $script:lastDrainMs).TotalSeconds
    if ($idleRx -gt 20 -and $stuckTx -gt 10) {
        Write-Host ("会话自愈：{0:N0}s 无入帧且发送队列 {1:N0}s 未排空 → 重开线缆设备" -f $idleRx, $stuckTx)
        [Oti]::CloseDevice($h)
        Start-Sleep -Milliseconds 300
        $h = [Oti]::OpenDevice($Device)
        if (-not [Oti]::IsValid($h)) {
            $alt = Find-CableDevice
            if ($alt) { $Device = $alt; $h = [Oti]::OpenDevice($Device) }
        }
        if ([Oti]::IsValid($h)) { Write-Host "会话自愈：已重新连接 $Device" }
        else { Write-Host "会话自愈：重连失败（设备插好了吗？）" }
        $script:txq.Clear(); $script:rxq.Clear()
        $script:lastFrameMs = $nowT; $script:lastDrainMs = $nowT; $script:cableFail = 0
        return $false
    }
    $pending = [uint16][Math]::Min($script:txq.Count, 100)
    if ([Oti]::ReadMsgP($h, $script:msgBuf, $pending) -ne 0) {
        # 永久运行要能扛住拔插：连续失败到一定次数就尝试重新打开设备
        $script:cableFail++
        if ($script:cableFail -ge 60) {
            $script:cableFail = 0
            Write-Host "设备连续读取失败，尝试重连 $Device …"
            [Oti]::CloseDevice($h)
            Start-Sleep -Milliseconds 500
            $h = [Oti]::OpenDevice($Device)
            if (-not [Oti]::IsValid($h)) {
                # 拔插后盘符可能变了：重新发现一次（这是"拔线重插要复原"的关键）
                Write-Host "  原设备打不开，重新自动发现…"
                $alt = Find-CableDevice
                if ($alt) { $Device = $alt; $h = [Oti]::OpenDevice($Device) }
            }
            if ([Oti]::IsValid($h)) {
                $script:txq.Clear(); $script:rxq.Clear()
                Write-Host "已重新连接 $Device"
            } else {
                Write-Host "重连失败（设备是否已插好？）"
            }
        }
        return $false
    }
    $script:cableFail = 0
    $t0 = $script:msgBuf[0]
    if ($t0 -eq 0x05) {
        $n = ([int]$script:msgBuf[1] -shl 8) -bor [int]$script:msgBuf[2]
        for ($i = 0; $i -lt $n; $i++) {
            if ($script:rxq.Count -ge 16) { break }         # 队列满：留在设备侧下次再取
            if ([Oti]::ReadFrame28($h, $script:frameBuf) -ne 0) { break }
            $script:lastFrameMs = Get-Date                     # 入帧时刻（会话自愈判据①）
            if ([Oti]::FrameCheck($script:frameBuf) -ne 1) { continue }  # 空闲/非法帧丢弃
            $l = [Oti]::FrameBodyLen($script:frameBuf)
            if ($l -lt 0) { continue }
            $b = New-Object byte[] $l                       # 只有真收到帧才分配
            if ($l -gt 0) { [Array]::Copy($script:frameBuf, 20, $b, 0, $l) }
            $script:rxq.Enqueue($b)
        }
        return $true
    }
    if ($t0 -eq 0x06 -or $t0 -eq 0x07) {
        $did = $false
        while ($script:txq.Count -gt 0) {                   # 授权：立刻写，中间不插别的设备操作
            $b = $script:txq.Dequeue()
            [Oti]::FramePack($b, $b.Length, $script:frameBuf)
            if ([Oti]::WriteFrame($h, $script:frameBuf) -eq 0) { $did = $true }
            else { $script:cableTxDropped++ }
        }
        return $did
    }
    $script:cableResets++
    return $false
}

function Read-Exact([System.IO.Stream]$st, [int]$n, [int]$deadlineMs) {
    $buf = New-Object byte[] $n
    $off = 0
    $t0 = Get-Date
    while ($off -lt $n) {
        try {
            $r = $st.Read($buf, $off, $n - $off)
            if ($r -le 0) { return $null }
            $off += $r
        } catch {
            # PowerShell 会把 .NET 异常包成 MethodInvocationException，这里一律当"读超时"处理
            if ($off -eq 0) { return $null }                       # 没读到任何字节：只是超时
            if (((Get-Date) - $t0).TotalMilliseconds -gt $deadlineMs) { return $null }
        }
    }
    return $buf
}
function RxTcp {
    $hdr = Read-Exact $stream 4 5000
    if ($null -eq $hdr) { return $null }
    $n = [BitConverter]::ToUInt32($hdr, 0)
    if ($n -gt 65496) { return $null }
    return (Read-Exact $stream ([int]$n) 5000)
}
function TxBody([byte[]]$body) {
    if ($useTcp) {
        $hdr = [BitConverter]::GetBytes([uint32]$body.Length)
        $stream.Write($hdr, 0, 4)
        $stream.Write($body, 0, $body.Length)
        $stream.Flush()
        return 0
    }
    if ($useCable) {
        $script:txq.Enqueue($body)
        # ⚠️ 绝不在这里等：原来最多等 5 秒，一旦对端授权慢，主循环就被卡住 5 秒 ——
        #    期间**读不到任何帧**。真机实测症状：对端 ROLE 变成每 5.2 / 10.4 秒才收到一条，
        #    剪贴板大面积"未确认"。队列由主循环每轮的 Pump-Cable 推进；可靠性交给 ACK/重发。
        [void](Pump-Cable)     # 必须 [void]：PowerShell 会把未接住的返回值拼进本函数返回值
        return 0
    }
    [Oti]::FramePack($body, $body.Length, $fr)
    return [Oti]::WriteFrame($h, $fr)
}

$frame = New-Object byte[] 65536
$lastClipHash = [uint32]0
$haveClipHash = $false
# 图片专用：最近一次"我们自己写入/发出"的 CF_DIB 指纹（见 ClipImg::DibHash 的注释）
$lastDibHash = [uint32]0
$haveDibHash = $false
$seq = [uint32]0
$start = Get-Date

# ---- 剪贴板图片：直接用 Win32 API（不用 WinForms）------------------------------
# 为什么不用 [Windows.Forms.Clipboard]：代理是隐藏窗口的 PowerShell 进程，
# 那套 API 依赖 STA/桌面会话，实测调用会被吞掉异常、图片永远发不出去。
# 这里用 OpenClipboard/GetClipboardData(CF_DIB)/SetClipboardData，与线程模型无关。
# 必须显式引用 System.Drawing：PowerShell 5.1 的 Add-Type 不会自动带上它，
# 否则报 SOURCE_CODE_ERROR（找不到 Bitmap/ImageFormat 等类型）。
Add-Type -AssemblyName System.Drawing | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class ClipImg {
    const uint CF_DIB = 8;
    const uint GMEM_MOVEABLE = 0x0002;

    [DllImport("user32.dll", SetLastError=true)] static extern bool OpenClipboard(IntPtr h);
    [DllImport("user32.dll")] static extern bool CloseClipboard();
    [DllImport("user32.dll")] static extern bool EmptyClipboard();
    [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint fmt);
    [DllImport("user32.dll")] static extern IntPtr SetClipboardData(uint fmt, IntPtr h);
    [DllImport("user32.dll")] static extern bool IsClipboardFormatAvailable(uint fmt);
    [DllImport("kernel32.dll")] static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
    [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr h);
    [DllImport("kernel32.dll")] static extern UIntPtr GlobalSize(IntPtr h);

    public static bool HasImage() {
        try { return IsClipboardFormatAvailable(CF_DIB); } catch { return false; }
    }

    // 剪贴板图片的**像素级指纹**（CF_DIB 字节的 CRC）。
    // 为什么需要它：Windows 剪贴板只放 CF_DIB，收/发都要过一次 PNG ↔ DIB 转换，
    // **同一张图的 PNG 字节在两侧永远不相等** → 原来那套"按载荷字节 CRC 抑制回发"
    // 对图片必然失效：每传一张图都会原样弹回去一次（实测：麒麟发 10973 字节，
    // Windows 应用成功后立刻回发一张 8017 字节的重编码图）。
    // 换成 DIB 指纹后，只要剪贴板里的图还是我刚写进去的那张，就不再回发。
    // ClipImg 与 Oti 是**两次独立的 Add-Type**（不是同一个编译单元），
    // 所以这里不能直接调 Oti.Crc32 —— 自带一份（同多项式，只用于指纹比对）。
    static uint Crc32Local(byte[] b, int off, int len) {
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < len; i++) {
            crc ^= b[off + i];
            for (int k = 0; k < 8; k++) crc = ((crc & 1) != 0) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
        return ~crc;
    }

    public static uint DibHash() {
        if (!OpenClipboard(IntPtr.Zero)) return 0;
        try {
            IntPtr h = GetClipboardData(CF_DIB);
            if (h == IntPtr.Zero) return 0;
            UIntPtr sz = GlobalSize(h);
            if ((int)sz <= 0) return 0;
            IntPtr p = GlobalLock(h);
            if (p == IntPtr.Zero) return 0;
            byte[] dib = new byte[(int)sz];
            Marshal.Copy(p, dib, 0, (int)sz);
            GlobalUnlock(h);
            uint crc = Crc32Local(dib, 0, dib.Length);
            return crc == 0 ? 1u : crc;      // 0 保留给"没有图片"
        } catch { return 0; } finally { CloseClipboard(); }
    }

    // 剪贴板里的 CF_DIB -> PNG 字节；没有图片返回 null
    public static byte[] GetPng() {
        if (!OpenClipboard(IntPtr.Zero)) return null;
        try {
            IntPtr h = GetClipboardData(CF_DIB);
            if (h == IntPtr.Zero) return null;
            UIntPtr sz = GlobalSize(h);
            IntPtr p = GlobalLock(h);
            if (p == IntPtr.Zero) return null;
            byte[] dib = new byte[(int)sz];
            Marshal.Copy(p, dib, 0, (int)sz);
            GlobalUnlock(h);
            // BITMAPINFOHEADER.biSize + 调色板（<=8bpp 时）算出行数据前的偏移
            int hdr = BitConverter.ToInt32(dib, 0);
            int bpp = BitConverter.ToInt16(dib, 14);
            int pal = 0;
            if (bpp <= 8) pal = (1 << bpp) * 4;
            else if (hdr > 40) pal = hdr - 40;
            int off = 14 + hdr + pal;
            byte[] bmp = new byte[14 + dib.Length];
            bmp[0] = (byte)'B'; bmp[1] = (byte)'M';
            BitConverter.GetBytes(14 + dib.Length).CopyTo(bmp, 2);
            BitConverter.GetBytes(off).CopyTo(bmp, 10);
            Array.Copy(dib, 0, bmp, 14, dib.Length);
            using (MemoryStream ms = new MemoryStream(bmp))
            using (Bitmap bm = new Bitmap(ms))
            using (MemoryStream outMs = new MemoryStream()) {
                bm.Save(outMs, ImageFormat.Png);
                return outMs.ToArray();
            }
        } catch { return null; }
        finally { CloseClipboard(); }
    }

    // PNG 字节 -> 剪贴板 CF_DIB
    public static bool SetPng(byte[] png) {
        try {
            byte[] dib;
            using (MemoryStream ms = new MemoryStream(png))
            using (Bitmap bm = new Bitmap(ms))
            using (MemoryStream bms = new MemoryStream()) {
                bm.Save(bms, ImageFormat.Bmp);      // 存成 BMP 再去掉 14 字节文件头 = CF_DIB
                byte[] all = bms.ToArray();
                dib = new byte[all.Length - 14];
                Array.Copy(all, 14, dib, 0, dib.Length);
            }
            if (!OpenClipboard(IntPtr.Zero)) return false;
            try {
                EmptyClipboard();
                IntPtr h = GlobalAlloc(GMEM_MOVEABLE, (UIntPtr)dib.Length);
                if (h == IntPtr.Zero) return false;
                IntPtr p = GlobalLock(h);
                if (p == IntPtr.Zero) return false;
                Marshal.Copy(dib, 0, p, dib.Length);
                GlobalUnlock(h);
                if (SetClipboardData(CF_DIB, h) == IntPtr.Zero) return false;
                return true;                        // 所有权已交给系统，不要再 free
            } finally { CloseClipboard(); }
        } catch { return false; }
    }
}
'@ -ReferencedAssemblies System.Drawing


# ---- 剪贴板文件（CF_HDROP）：同样走 Win32 API ---------------------------------
# 剪贴板里放文件时 Windows 用的是 CF_HDROP（DROPFILES 结构 + 宽字符双零结尾路径表）。
Add-Type -AssemblyName System.Drawing | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class ClipFiles {
    const uint CF_HDROP = 15;
    const uint GMEM_MOVEABLE = 0x0002;

    [DllImport("user32.dll", SetLastError=true)] static extern bool OpenClipboard(IntPtr h);
    [DllImport("user32.dll")] static extern bool CloseClipboard();
    [DllImport("user32.dll")] static extern bool EmptyClipboard();
    [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint fmt);
    [DllImport("user32.dll")] static extern IntPtr SetClipboardData(uint fmt, IntPtr h);
    [DllImport("user32.dll")] static extern bool IsClipboardFormatAvailable(uint fmt);
    // 坑（实测根因）：DragQueryFileW 在 **shell32.dll**，不在 user32.dll！
    // 写错 DLL 会抛 EntryPointNotFoundException，而 Get() 的 catch 把它整个吞掉 →
    // 表现只是"HasFiles()=True，但 0 个文件"，于是文件退化成纯文本路径发过去
    // （对端只收到一串 C:\ 路径）—— 这正是"Windows→麒麟 文件方向发不出去"的根因。
    [DllImport("shell32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern uint DragQueryFileW(IntPtr h, uint i, StringBuilder sb, uint cch);
    [DllImport("kernel32.dll")] static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
    [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr h);

    // Get() 原来把所有异常都吞掉 → 上面那个 DLL 写错整整藏了一轮没被发现。
    // 现在把最后一次错误记下来，调用方在 HasFiles()=true 却取不到文件时打印它。
    public static string LastError = "";

    public static bool HasFiles() { try { return IsClipboardFormatAvailable(CF_HDROP); } catch { return false; } }

    public static string[] Get() {
        LastError = "";
        if (!OpenClipboard(IntPtr.Zero)) { LastError = "OpenClipboard 失败 err=" + Marshal.GetLastWin32Error(); return null; }
        try {
            IntPtr h = GetClipboardData(CF_HDROP);
            if (h == IntPtr.Zero) { LastError = "GetClipboardData(CF_HDROP) 为空 err=" + Marshal.GetLastWin32Error(); return null; }
            uint n = DragQueryFileW(h, 0xFFFFFFFFu, null, 0);
            if (n == 0) { LastError = "DragQueryFileW 报告 0 个文件 err=" + Marshal.GetLastWin32Error(); return null; }
            List<string> l = new List<string>();
            for (uint i = 0; i < n; i++) {
                StringBuilder sb = new StringBuilder(1024);
                if (DragQueryFileW(h, i, sb, 1024) > 0) l.Add(sb.ToString());
            }
            if (l.Count == 0) LastError = "DragQueryFileW 逐个取名都失败（count=" + n + "）";
            return l.ToArray();
        } catch (Exception ex) { LastError = ex.GetType().Name + ": " + ex.Message; return null; } finally { CloseClipboard(); }
    }

    public static bool Set(string[] paths) {
        try {
            MemoryStream ms = new MemoryStream();
            BinaryWriter bw = new BinaryWriter(ms);
            bw.Write((uint)20);          // DROPFILES.pFiles = 结构体大小
            bw.Write((int)0); bw.Write((int)0);   // pt
            bw.Write((int)0);            // fNC
            bw.Write((int)1);            // fWide = 宽字符
            foreach (string p in paths) { bw.Write(Encoding.Unicode.GetBytes(p)); bw.Write((ushort)0); }
            bw.Write((ushort)0);
            byte[] data = ms.ToArray();
            if (!OpenClipboard(IntPtr.Zero)) return false;
            try {
                EmptyClipboard();
                IntPtr h = GlobalAlloc(GMEM_MOVEABLE, (UIntPtr)data.Length);
                if (h == IntPtr.Zero) return false;
                IntPtr q = GlobalLock(h);
                if (q == IntPtr.Zero) return false;
                Marshal.Copy(data, 0, q, data.Length);
                GlobalUnlock(h);
                return SetClipboardData(CF_HDROP, h) != IntPtr.Zero;
            } finally { CloseClipboard(); }
        } catch { return false; }
    }
}
'@

$clipRx = @{ fid = 0; total = 0; got = 0; buf = $null; active = $false; fmt = 1 }
$rx = 0; $txClip = 0; $injected = 0; $script:vendorRx = 0; $script:lastOvs = ''
$lastClipCheck = Get-Date

# ============================ 托盘图标（状态可见性）============================
# 对标 Mouse Without Borders：状态、暂停、退出都从托盘走，用户不需要看命令行。
$script:paused = $false
$script:tray = $null
# 鼠标按键状态：协议里 buttons 是**位图**（bit0 左 / bit1 右 / bit2 中），
# 而 SendInput 需要按下/抬起**事件**，所以必须自己比对新旧状态发差量。
# （之前这里漏了，导致"左右键完全没反应"。）
$script:btnState = 0
function Apply-Buttons([int]$btns) {
    $chg = $btns -bxor $script:btnState
    if ($chg -ne 0) {
        $r = 0
        if ($chg -band 1) { $r = [Oti]::MouseButton(0, (($btns -band 1) -ne 0)) }
        if ($chg -band 2) { $r = [Oti]::MouseButton(1, (($btns -band 2) -ne 0)) }
        if ($chg -band 4) { $r = [Oti]::MouseButton(2, (($btns -band 4) -ne 0)) }
        $script:btnState = $btns
        if ($Verbose) {
            Write-Host ("  BTN 0x{0:x} 变化 0x{1:x} SendInput={2} LButtonState={3}" -f $btns, $chg, $r, [Oti]::LDown())
        }
    }
}
function Init-Tray {
    if ($NoTray) { return }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        Add-Type -AssemblyName System.Drawing
    } catch { Write-Host "托盘不可用（无桌面会话？）：$($_.Exception.Message)"; return }

    $script:tray = New-Object System.Windows.Forms.NotifyIcon
    # 用 shell32 里的键盘图标；取不到就退回系统默认
    try { $script:tray.Icon = [System.Drawing.Icon]::ExtractAssociatedIcon("$env:SystemRoot\System32\shell32.dll") } catch {}
    if (-not $script:tray.Icon) { $script:tray.Icon = [System.Drawing.SystemIcons]::Application }
    $script:tray.Text = "OTiLink 被控端：启动中…"
    $menu = New-Object System.Windows.Forms.ContextMenuStrip
    $miStatus = $menu.Items.Add("显示状态")
    $miStatus.add_Click({
        [System.Windows.Forms.MessageBox]::Show((Get-StatusText), "OTiLink 被控端") | Out-Null
    })
    $miPause = $menu.Items.Add("暂停注入")
    $miPause.add_Click({
        $script:paused = -not $script:paused
        $miPause.Text = if ($script:paused) { "恢复注入" } else { "暂停注入" }
        $script:tray.Text = Get-TrayText
        Write-Host ("注入已" + $(if ($script:paused) { "暂停" } else { "恢复" }))
    })
    $miLog = $menu.Items.Add("打开控制台日志…")
    $miLog.add_Click({ Write-Host "日志即当前控制台输出（若为隐藏启动，请手动前台运行一次）" })
    $menu.Items.Add("-") | Out-Null
    $miQuit = $menu.Items.Add("退出")
    $miQuit.add_Click({ $script:quit = $true })
    $script:tray.ContextMenuStrip = $menu
    $script:tray.Visible = $true
    $script:tray.add_DoubleClick({ [System.Windows.Forms.MessageBox]::Show((Get-StatusText), "OTiLink 被控端") | Out-Null })
    Write-Host "托盘图标已启用（双击看状态，右键菜单可暂停/退出）"
}
function Get-TrayText {
    $st = if ($script:paused) { "已暂停" } else { "运行中" }
    $dev = if ($Device) { $Device } else { "?" }
    return ("OTiLink 被控端 [$st] $dev 注入=$injected 收=$rx")
}
function Get-StatusText {
    return @"
OTiLink 被控端
────────────────────
设备        : $Device
鼠标注入模式: $MouseMode
运行状态    : $(if ($script:paused) { '已暂停（不注入）' } else { '运行中' })
收到消息    : $rx 条
注入次数    : $injected 次
剪贴板发块  : $txClip
线缆复位    : $($script:cableResets) 次
写失败丢弃  : $($script:cableTxDropped) 次
主控端屏幕  : $(if ($script:peerW) { "$($script:peerW)x$($script:peerH)" } else { '未上报' })
"@
}
$script:quit = $false

$script:peerW = 0; $script:peerH = 0
$lastHello = [DateTime]::MinValue
function Send-Hello {
    # 把本机屏幕几何告诉对端：主控端据此把指针位置换算到本机坐标系
    $pl = New-Object byte[] 4
    [BitConverter]::GetBytes([uint16][Oti]::VirtW()).CopyTo($pl, 0)
    [BitConverter]::GetBytes([uint16][Oti]::VirtH()).CopyTo($pl, 2)
    $body = [Oti]::Encode(8, 0, $pl, 4)
    # 同时声明鼠标注入模式（加速曲线在本机系统里，所以由本机决定）
    $mb = New-Object byte[] 1
    $mb[0] = if ($MouseMode -eq 'absolute') { 0 } else { 1 }
    $mbody = [Oti]::Encode(9, 0, $mb, 1)
    if ($useCable) {
        $script:txq.Enqueue($body)      # 交给 Pump-Cable 发，不阻塞
        $script:txq.Enqueue($mbody)
    } else {
        [void](TxBody $body)
        [void](TxBody $mbody)
    }
}

# ============================ 角色协商（OTI_MSG_ROLE=10）============================
# 第 1 步：只做「能收发 ROLE + 打日志」——不动自启、不起停 otiagent2（AGENTS L5）。
# 载荷 12 字节小端（与 re/otilink/otiproto.h 的 oti_role_evt 逐字段一致）：
#   u8 has_local_input, u8 want, u8 state, u8 flags, u32 boot_id, u32 input_age_ms
# boot_id：对齐麒麟侧 /proc/sys/kernel/random/boot_id 的语义——「本机启动标识低 32 位」；
#          Windows 无该文件，用系统启动时间的 Unix 秒低 32 位代替（平票小者优先时够用）。
# Raw Input 设备枚举（毫秒级、不走 WMI）—— 用于验证它能否替代把主循环卡 5 秒的 Get-PnpDevice。
if (-not ('OtiRaw' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OtiRaw {
    [StructLayout(LayoutKind.Sequential)] public struct RAWINPUTDEVICELIST { public IntPtr hDevice; public uint dwType; }
    [DllImport("user32.dll")] public static extern uint GetRawInputDeviceList(IntPtr p, ref uint n, uint size);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint GetRawInputDeviceInfoW(IntPtr h, uint cmd, StringBuilder buf, ref uint size);
    public static string[] Names() {
        uint n = 0; uint sz = (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICELIST));
        if (GetRawInputDeviceList(IntPtr.Zero, ref n, sz) != 0 || n == 0) return new string[0];
        IntPtr buf = Marshal.AllocHGlobal((int)(n * sz));
        try {
            if (GetRawInputDeviceList(buf, ref n, sz) == unchecked((uint)-1)) return new string[0];
            var res = new System.Collections.Generic.List<string>();
            for (uint i = 0; i < n; i++) {
                IntPtr p = (IntPtr)((long)buf + i * (long)sz);
                RAWINPUTDEVICELIST d = (RAWINPUTDEVICELIST)Marshal.PtrToStructure(p, typeof(RAWINPUTDEVICELIST));
                if (d.dwType != 0 && d.dwType != 1) continue;
                uint len = 0;
                GetRawInputDeviceInfoW(d.hDevice, 0x20000000, null, ref len);
                if (len == 0) continue;
                var sb = new StringBuilder((int)len + 1);
                if (GetRawInputDeviceInfoW(d.hDevice, 0x20000000, sb, ref len) > 0) res.Add(sb.ToString());
            }
            return res.ToArray();
        } finally { Marshal.FreeHGlobal(buf); }
    }
}
'@
}
$script:roleBoot = [uint32]0
try {
    $lbt = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime
    $script:roleBoot = [uint32](([DateTimeOffset]$lbt).ToUnixTimeSeconds() % 4294967296)
} catch {
    $script:roleBoot = [uint32](([Environment]::TickCount) % 4294967296)
}
$script:rolePeerSeen     = $false
$script:rolePeerHasLocal = 0
$script:rolePeerWant     = 0
$script:rolePeerState    = 0
$script:rolePeerFlags    = 0
$script:rolePeerBoot     = [uint32]0
$script:rolePeerAge      = [uint32]0
$script:rolePeerMs       = [DateTime]::MinValue
$script:roleLastTx       = [DateTime]::MinValue
$script:roleLastLog      = [DateTime]::MinValue
$script:roleLastProbe    = [DateTime]::MinValue
$script:roleLastDecide   = [DateTime]::MinValue
$script:roleStart        = Get-Date
$script:roleLoopStart    = [DateTime]::MinValue
$script:roleHasLocal     = 0
$script:roleInputLast    = [DateTime]::MinValue
$script:roleState        = 2      # 初始：没跑 otiagent2 = slave（Windows 侧 grab == 跑 otiagent2）
$script:roleStateMs      = 0
$script:roleWant         = 0
switch ($Role) { 'master' { $script:roleWant = 1 } 'slave' { $script:roleWant = 2 } }
$script:rolePeerSlaveStreak = 0
$script:roleGrabOk       = $false
$script:roleLoggedState  = -1
$script:roleLoggedGrab   = -1
# 常量与 otikm_core.h / otikm.c 缺省一致
$script:ROLE_PEER_ABSENT_MS = 5000
$script:ROLE_DWELL_MS       = 10000
$script:ROLE_MARGIN_MS      = 2000
$script:ROLE_STREAK_NEED    = 3

# GetLastInputInfo：本机最近一次输入的时刻。仅在 master（本机在驱动）时采样，
# 与 Linux handle_local() 只在 !return_on_edge 时更新 input_last_ms 对齐。
try {
    Add-Type -Namespace OtiRole -Name Native -EA SilentlyContinue -MemberDefinition @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
'@
} catch {}

# ⚠️ 不要在 PowerShell 里 `New-Object 结构体` + `[ref]$li` 调 P/Invoke：
#    PowerShell 不会把结构体的写回值带回来（实测 dwTime 恒为 0）→ idle 变成"开机时长"
#    （真机日志出现过 4004641442ms ≈ 46 天）→ 本机刚当上 master 就因"从没在用"把主控让回去，
#    两端来回翻。改成由 C# 内部完成 ref 调用、只回传一个 uint。
if (-not ('OtiIdle' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OtiIdle {
    [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
    [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);
    [DllImport("kernel32.dll")] public static extern uint GetTickCount();
    public static uint IdleMs() {
        LASTINPUTINFO li = new LASTINPUTINFO();
        li.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO));
        if (!GetLastInputInfo(ref li)) return 0xFFFFFFFFu;   // 失败：当作"从未"
        return (uint)(GetTickCount() - li.dwTime);           // 无符号回绕就是正确的毫秒差
    }
}
'@
}

function Get-IdleMs {
    try {
        $v = [uint32][OtiIdle]::IdleMs()
        if ($v -eq [uint32]4294967295) { return -1 }   # 未知/失败：不更新 roleInputLast
        return [int64]$v
    } catch { return -1 }
}

# 本机（线缆之外）的 HID 键盘/鼠标：与麒麟侧 oti_input_autoselect(0,...) 同义，
# 排除线缆自身（InstanceId 含 VID_0EA0&PID_2213；与 -HidRead 观察器的过滤一致）。
# 内置键鼠（笔记本自带键盘/触控板/EC）永远在位，**不能**算"键鼠插在这一边"，
# 否则两侧永远"都有键鼠" → "物理换边"这个信号被彻底淹掉。真机实测本机内置三个：
#   ACPI\FUJ7401（内置键盘）、HID\VID_048D&PID_C100&COL02（ITE EC 键盘）、HID\MSFT0001（触控板）
# 判据 DEVPKEY_Device_RemovalPolicy：对拷线 HID = 3（可意外拔出）、内置 = 1（不可移除）。
# ⚠️ 这个函数在**主循环**里被调用：Get-PnpDeviceProperty 是 WMI/PNP 查询（几十~几百 ms），
#    每 3 秒对每个设备查一次会**卡住线缆泵**（帧静默丢失、对端写命令被拒 rc=524546）。
#    RemovalPolicy 在设备插着的时候不会变 → 按 InstanceId 缓存，只在首次见到时查。
$script:rolePolCache = @{}
function Test-HotplugInput([string]$instanceId) {
    if ($script:rolePolCache.ContainsKey($instanceId)) { return $script:rolePolCache[$instanceId] }
    $v = $true
    try {
        $pol = (Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_RemovalPolicy' -EA Stop).Data
        $v = ([int]$pol -ne 1)        # 1 = EXPECT_NO_REMOVAL = 内置/不可移除
    } catch {
        Write-Host ("  ROLE [warn] 读不到 " + $instanceId + " 的 RemovalPolicy，按可热插拔处理")
        $v = $true
    }
    $script:rolePolCache[$instanceId] = $v
    return $v
}

# ⚠️ 设备枚举（Get-PnpDevice）是 WMI 调用，实测**单次把主循环卡住 5.2 秒** —— 期间线缆
#    一帧都收不到（LOOP 慢轮诊断：A=21ms / B=5374ms / C=39ms）。所以它必须跑在**后台作业**里，
#    主循环只读结果，探测间隔才能回到 3 秒。
#    教训：一度把间隔放宽到 90 秒来躲卡顿 —— 直接后果是"键鼠插到 Windows 后最长 90 秒才换主控"，
#    而这 90 秒里两端都以为自己是主控 → 键盘乱加/删字母、鼠标被另一侧拽回。
$script:probeJob = $null
# ⚠️ 设备枚举（Get-PnpDevice）是 WMI 调用，实测单次把主循环卡住 5.2 秒（LOOP 慢轮诊断
#    A=21ms / B=5374ms / C=39ms）。**后台作业（Start-Job）方案试过并回退了**：每几秒起一个
#    子 PowerShell 进程会把剪贴板读取拖垮（门禁出现 `<Get-Clipboard : ` 这种错误串，
#    clipreg 从 17/17 掉到 10/7）。所以现在仍是同步探测 —— 探测间隔就是"卡顿频率"，
#    取值是权衡：**20 秒**（检测键鼠换边 ≤20s；更长会拖慢换主控、甚至两端同时主控）。
#    根治方向：改用 Raw Input（GetRawInputDeviceList，无 WMI），或在后台 **runspace** 里跑
#    （而不是每几秒起子进程）。见 re/NOTES.md §58。
function Update-LocalInput {
    # 用 Win32_Keyboard / Win32_PointingDevice（简单 CIM 类）替代 Get-PnpDevice：
    # 实测同样 5 个设备，Get-PnpDevice **1267ms** vs CIM **328ms**。这个耗时就是主循环被
    # 阻塞的时间（阻塞期间一帧都收不到）—— 越短，帧丢得越少、换边检测才能更快。
    $kbd = @(); $mou = @()
    try { $kbd = @(Get-CimInstance Win32_Keyboard -EA SilentlyContinue) } catch {}
    try { $mou = @(Get-CimInstance Win32_PointingDevice -EA SilentlyContinue) } catch {}
    $kbd = @($kbd | Where-Object { $_.PNPDeviceID -and $_.PNPDeviceID -notmatch 'VID_0EA0&PID_2213' -and (Test-HotplugInput $_.PNPDeviceID) })
    $mou = @($mou | Where-Object { $_.PNPDeviceID -and $_.PNPDeviceID -notmatch 'VID_0EA0&PID_2213' -and (Test-HotplugInput $_.PNPDeviceID) })
    $nl = 0; if ($kbd.Count -gt 0 -or $mou.Count -gt 0) { $nl = 1 }
    if ($nl -ne $script:roleHasLocal) {
        $script:roleHasLocal = $nl
        $ks = if ($kbd.Count) { (($kbd | ForEach-Object { $_.PNPDeviceID }) -join ' ; ') } else { '-' }
        $ms = if ($mou.Count) { (($mou | ForEach-Object { $_.PNPDeviceID }) -join ' ; ') } else { '-' }
        Write-Host ("ROLE 本机输入: hasLocal={0} 键盘[{1}]={2} 鼠标[{3}]={4}（只算可热插拔；已排除线缆 VID_0EA0&PID_2213 与内置设备）" -f $nl, $kbd.Count, $ks, $mou.Count, $ms)
    }
    if ($script:roleState -eq 1) {
        $idle = Get-IdleMs
        if ($idle -ge 0) {
            $cand = (Get-Date).AddMilliseconds(-$idle)
            if ($cand -gt $script:roleInputLast) { $script:roleInputLast = $cand }
        }
    }
}

function Get-RoleInputAge {
    # "从未输入"给一个**真·最大**值：麒麟侧 role_input_age 在"从未"时给 600000，
    # 但当本机 idle 超过 600000ms 时它会返回更大的真实值 —— 两条比较时"从未"的
    # 600000 反而显得"更近"，会让对端把主控让给一台根本没在用的机器（实测：Kylin
    # 我=635413 时让位给 Windows 的 600000 → 双方来回翻）。用 uint 最大值兜底，
    # "从未/极久没用"在任何 age 比较里都必然输。实际 age 不截断（上限 uint32）。
    if ($script:roleInputLast -eq [DateTime]::MinValue) { return [uint32]4294967295 }
    $d = ((Get-Date) - $script:roleInputLast).TotalMilliseconds
    if ($d -lt 0) { $d = 0 }
    if ($d -gt 4294967295) { $d = 4294967295 }
    return [uint32][int64]$d
}

# 纯逻辑：逐行对齐 otikm_core.c 的 otikm_role_decide（coretest 期望见 /tmp/roletest.ps1 验证）
function Invoke-RoleDecide($in) {
    $out = [ordered]@{ want_state = $in.state; want_valid = $false; grab_ok = $false; must_yield = $false; peer_absent = $false; reason = '保持现状' }
    $peer_recent = $in.peer_seen -and (($in.now_ms - $in.peer_ms) -le $in.peer_absent_ms)
    $out.peer_absent = -not $peer_recent
    $want = $in.want
    if ($want -eq 0 -and $peer_recent) {
        if ($in.peer_want -eq 1) { $want = 2 } elseif ($in.peer_want -eq 2) { $want = 1 }
    }
    if ($want -eq 1) { $out.want_state = 1; $out.want_valid = $true; $out.reason = '本机显式 master' }
    elseif ($want -eq 2) { $out.want_state = 2; $out.want_valid = $true; $out.reason = '本机/对端显式 slave' }
    else {
        $my_has = [int]$in.has_local_input
        $pe_has = [int]($peer_recent -and $in.peer_has_input)
        if ($my_has -and -not $pe_has) { $out.want_state = 1; $out.want_valid = $true; $out.reason = '只有本机有键鼠' }
        elseif (-not $my_has -and $pe_has) { $out.want_state = 2; $out.want_valid = $true; $out.reason = '只有对端有键鼠' }
        elseif ($my_has -and $pe_has) {
            $i_master = ($in.state -eq 1); $peer_master = ($in.peer_state -eq 1)
            $dwell_ok = ($in.state -eq 0) -or (($in.now_ms - $in.state_since_ms) -ge $in.dwell_ms)
            $margin = [int64]$in.input_margin_ms
            if (-not $i_master -and -not $peer_master) {
                if (([int64]$in.input_age_ms + $margin) -lt [int64]$in.peer_input_age_ms) { $out.want_state = 1; $out.want_valid = $true; $out.reason = '双方都有键鼠：本机刚在用' }
                elseif (([int64]$in.peer_input_age_ms + $margin) -lt [int64]$in.input_age_ms) { $out.want_state = 2; $out.want_valid = $true; $out.reason = '双方都有键鼠：对端刚在用' }
            } elseif ($dwell_ok) {
                if (([int64]$in.input_age_ms + $margin) -lt [int64]$in.peer_input_age_ms) { $out.want_state = 1; $out.want_valid = $true; $out.reason = $(if ($i_master) { '保持 master（本机更近）' } else { '抢 master（本机更近）' }) }
                elseif (([int64]$in.peer_input_age_ms + $margin) -lt [int64]$in.input_age_ms) { $out.want_state = 2; $out.want_valid = $true; $out.reason = '让 master（对端更近）' }
            }
        } else { $out.want_state = 2; $out.want_valid = $true; $out.reason = '双方都没有本机键鼠（请显式 --role）' }
    }
    $my_has_input = $in.has_local_input
    $peer_has_input2 = [int]($peer_recent -and $in.peer_has_input)
    if ($peer_recent -and ($in.peer_state -eq 1) -and ($in.want -eq 0) -and $my_has_input -and $peer_has_input2) {
        $pw = ([int64]$in.peer_boot_id -lt [int64]$in.boot_id) -or (([int64]$in.peer_boot_id -eq [int64]$in.boot_id) -and ([int64]$in.peer_input_age_ms -lt [int64]$in.input_age_ms))
        if ($pw) { $out.want_state = 2; $out.want_valid = $true; $out.reason = '冲突：对端 boot_id 更小（我让位）' }
        else { $out.want_state = 1; $out.want_valid = $true; $out.reason = '冲突：本机 boot_id 更小（对端该让）' }
    }
    $out.must_yield = ($in.state -eq 1) -and $out.want_valid -and ($out.want_state -ne 1)
    $peer_says_slave = $peer_recent -and ($in.peer_state -eq 2) -and ($in.peer_slave_streak -ge $script:ROLE_STREAK_NEED)
    $out.grab_ok = ($out.want_state -eq 1) -and ($peer_says_slave -or $out.peer_absent)
    if ($out.must_yield) { $out.grab_ok = $false }
    return $out
}

# otiagent2 是"grab"的实体：跑着 = 主控。L4：任何时刻 ≤1 个，停进程排除 $PID。
function Test-RoleMasterRunning {
    # 用 Get-Process 按进程名查：CIM(Win32_Process) 查询在本机实测会误报 true，
    # 导致 master 起不来（日志只有"生效: master"却没有"拉起"，且 .cmd 从未生成）。
    return (@(Get-Process -Name 'otiagent2' -EA SilentlyContinue) | Measure-Object).Count -ge 1
}
function Start-RoleMaster {
    if (Test-RoleMasterRunning) { return }
    $exe = 'C:\Users\Public\otiagent2.exe'
    if (-not (Test-Path $exe)) { Write-Host "  ROLE [warn] 找不到 $exe，无法起 master"; return }
    $devArg = ''
    if ($Device) { $devArg = ' --device ' + $Device }
    # 临时 .cmd + Start-Process（参照 portable/otilink-win.ps1 的 Start-Agent）：绕开引号/重定向转义
    $line = '"' + $exe + '"' + $devArg + ' --edge right > "C:\Users\Public\km.log" 2>&1'
    $f = 'C:\Users\Public\otilink-role-km.cmd'
    try {
        Set-Content -Path $f -Value ('@echo off' + [Environment]::NewLine + $line) -Encoding ASCII
        Start-Process -FilePath $f -WindowStyle Hidden | Out-Null
        Write-Host ("  ROLE 拉起 master: otiagent2{0} --edge right" -f $devArg)
    } catch { Write-Host ("  ROLE [warn] 起 master 失败: " + $_.Exception.Message) }
}
function Stop-RoleMaster {
    $ps = @(Get-Process -Name 'otiagent2' -EA SilentlyContinue)
    foreach ($p in $ps) {
        try { Stop-Process -Id $p.Id -Force -EA Stop; Write-Host ("  ROLE 停止 master: pid=" + $p.Id) } catch {}
    }
}

# 一次决策 + 起停。每 1s 调一次；ROLE 到达时由主循环自然纳入下一拍。
function Update-Role {
    $nowMs = [int64]((Get-Date) - $script:roleStart).TotalMilliseconds
    $in = @{
        has_local_input   = $script:roleHasLocal
        want              = $script:roleWant
        state             = $script:roleState
        boot_id           = $script:roleBoot
        input_age_ms      = [uint32](Get-RoleInputAge)
        state_since_ms    = $script:roleStateMs
        peer_seen         = [int]$script:rolePeerSeen
        peer_ms           = [int64]($script:rolePeerMs - [DateTime]::MinValue).TotalMilliseconds
        peer_has_input    = $script:rolePeerHasLocal
        peer_want         = $script:rolePeerWant
        peer_state        = $script:rolePeerState
        peer_boot_id      = $script:rolePeerBoot
        peer_input_age_ms = $script:rolePeerAge
        peer_slave_streak = $script:rolePeerSlaveStreak
        now_ms            = $nowMs
        dwell_ms          = $script:ROLE_DWELL_MS
        input_margin_ms   = $script:ROLE_MARGIN_MS
        peer_absent_ms    = $script:ROLE_PEER_ABSENT_MS
    }
    if ($script:roleLoopStart -eq [DateTime]::MinValue) { $script:roleLoopStart = Get-Date }
    $ro = Invoke-RoleDecide $in
    # L17 启动安全窗：**还没收到过对端 ROLE** 且进主循环不足 5s 时不许抢主控
    # （实测：用脚本初始化时刻计时不成立——脚本加载 Add-Type 已过 5s，进主循环即抢，抢在对端 ROLE 到达之前）。
    # 5s 后仍满足 I4（对端一直没消息 → 单方面 master）。
    if ((-not $script:rolePeerSeen) -and (((Get-Date) - $script:roleLoopStart).TotalSeconds -lt 5)) { $ro.grab_ok = $false }
    $script:roleGrabOk = $ro.grab_ok
    if ($ro.want_valid -and ($script:roleLoggedState -ne $ro.want_state -or $script:roleLoggedGrab -ne [int]$ro.grab_ok)) {
        $script:roleLoggedState = $ro.want_state
        $script:roleLoggedGrab = [int]$ro.grab_ok
        $ws = if ($ro.want_state -eq 1) { 'master' } else { 'slave' }
        Write-Host ("ROLE 决策: {0}（{1}）对端 state={2} hasIn={3} age={4} want={5} 我={6} grab={7}" -f $ws, $ro.reason, $script:rolePeerState, $script:rolePeerHasLocal, $script:rolePeerAge, $script:rolePeerWant, (Get-RoleInputAge), $(if ($ro.grab_ok) { '可' } else { '否' }))
    }
    if ($ro.want_valid) {
        if ($ro.want_state -eq 1 -and $ro.grab_ok) {
            Start-RoleMaster
            if ($script:roleState -ne 1) {
                $script:roleState = 1; $script:roleStateMs = $nowMs
                Write-Host "ROLE 生效: master（键鼠在本机，已拉起 otiagent2 --edge right）"
            }
        } else {
            Stop-RoleMaster
            if ($script:roleState -ne 2) {
                $script:roleState = 2; $script:roleStateMs = $nowMs
                Write-Host "ROLE 生效: slave（已停 otiagent2，转零软件接收端；剪贴板代理保留）"
            }
        }
    } else {
        if ($script:roleState -ne 1) { Stop-RoleMaster }
    }
}

function Send-Role {
    $hl = 0; if ($script:roleHasLocal) { $hl = 1 }
    $pl = New-Object byte[] 12
    $pl[0] = [byte]$hl
    $pl[1] = [byte]$script:roleWant
    $pl[2] = [byte]$script:roleState
    $pl[3] = [byte]$(if ($script:roleState -eq 1) { 1 } else { 0 })   # bit0 = 我持有 grab（= otiagent2 在跑）
    [Oti]::Put32($pl, 4, [uint32]$script:roleBoot)
    [Oti]::Put32($pl, 8, [uint32](Get-RoleInputAge))
    $body = [Oti]::Encode(10, 0, $pl, 12)
    if ($useCable) { $script:txq.Enqueue($body) }   # 与 Send-Hello 一致：不阻塞主循环
    else { [void](TxBody $body) }
}

Update-LocalInput
try { $rawN = @([OtiRaw]::Names()).Count; Write-Host ("RAW 设备数=" + $rawN + "（>0 表示可用 Raw Input 替代 Get-PnpDevice）") } catch { Write-Host ("RAW 探测失败: " + $_.Exception.Message) }
Write-Host ("ROLE 初始化: boot={0:x8} hasLocal={1} want={2} state=slave（Windows 侧 grab=跑 otiagent2）" -f $script:roleBoot, $script:roleHasLocal, $script:roleWant)

# ============================ 大文件流式传输（format 4/5）============================
# 与麒麟侧 otixfer.c 一一对应：
#   format 4 = PART：clip 头(20B) 之后 u32 idx, u32 nparts, u64 total, u32 namelen,
#                    name, u32 dlen, data
#   format 5 = CTRL：u16 kind(1=DONE,2=VERDICT), u16 flags(1=OK,2=RESUME,4=ABORT),
#                    u64 total, u32 crc, u32 nparts, u32 resume_from, u32 namelen, name
#
# 为什么不能沿用"文件包"（format 3）：那是**整包驻留内存**的 —— 发端把文件全读进内存、
# 收端按 total_len 一次性分配。8MB 可以，500MB 会把本机仅剩的 ~1GB 内存打爆，
# 而且中间丢一块就得整包重来。这里改成 64000 字节一片、收端边收边落盘（内存恒定 64KB），
# 收端用位图记录缺哪片 → 回 RESUME，发端 seek 过去续传。
$script:PART_MAX = 64000
$script:BIG_THRESHOLD = 8000000

function New-TxState {
    @{ active=$false; fid=[uint32]0; paths=@(); cur=0; fs=$null; name=''; total=[uint64]0;
       nparts=0; sent=0; crc=[uint32]0; crcDone=$false; phase=0; deadline=(Get-Date);
       rounds=0; t0=(Get-Date); lastLog=(Get-Date); done=0; bytes=[uint64]0;
       todo=$null; todoI=0; doneParts=0; failStreak=0 }
}
function New-RxState {
    @{ active=$false; fid=[uint32]0; fs=$null; name=''; path=''; partPath=''; total=[uint64]0;
       nparts=0; bits=$null; got=0; t0=(Get-Date); lastLog=(Get-Date); lastPart=(Get-Date) }
}
$script:fileTx = New-TxState
$script:fileRx = New-RxState
$script:lastFileIdent = [uint32]0
$script:haveFileIdent = $false

# 文件剪贴板的"身份"指纹（路径+大小+mtime）：不读内容就能判断"还是那一份"。
# 以前每次轮询都把文件整个读一遍算 CRC —— 8MB 文件每 300ms 读一次，500MB 更不可能。
function Get-FileIdent([string[]]$paths) {
    $h = [uint32]0
    foreach ($p in $paths) {
        try {
            $fi = New-Object System.IO.FileInfo $p
            if (-not $fi.Exists) { continue }
            $sz = [uint64]$fi.Length
            $mt = [uint64][int64]($fi.LastWriteTimeUtc - [datetime]'1970-01-01Z').TotalSeconds
            $nb = [Text.Encoding]::UTF8.GetBytes($p)
            $h = [Oti]::Crc32Append($h, $nb, 0, $nb.Length)
            $b1 = [BitConverter]::GetBytes($sz); $h = [Oti]::Crc32Append($h, $b1, 0, 8)
            $b2 = [BitConverter]::GetBytes($mt); $h = [Oti]::Crc32Append($h, $b2, 0, 8)
        } catch { }
    }
    return $h
}

function Send-FileCtrl([uint32]$fid, [uint16]$kind, [uint16]$flags, [uint64]$total,
                       [uint32]$crc, [uint32]$nparts, [uint32]$resumeFrom, [string]$name,
                       [byte[]]$missMap = $null) {
    $nb = [Text.Encoding]::UTF8.GetBytes($name)
    $maplen = 0
    if ($missMap) { $maplen = $missMap.Length; $flags = $flags -bor 8 }   # 8 = HAS_MAP
    $body = 28 + $nb.Length + $maplen
    $pl = New-Object byte[] (20 + $body)
    [Oti]::Put16($pl, 0, [uint16]5); [Oti]::Put16($pl, 2, [uint16]3)
    [Oti]::Put32($pl, 4, $fid); [Oti]::Put32($pl, 8, [uint32]0)
    [Oti]::Put32($pl, 12, [uint32]$body); [Oti]::Put32($pl, 16, [uint32]$body)
    [Oti]::Put16($pl, 20, $kind); [Oti]::Put16($pl, 22, $flags)
    [Oti]::Put64($pl, 24, $total)
    [Oti]::Put32($pl, 32, $crc); [Oti]::Put32($pl, 36, $nparts)
    [Oti]::Put32($pl, 40, $resumeFrom); [Oti]::Put32($pl, 44, [uint32]$nb.Length)
    if ($nb.Length -gt 0) { [Array]::Copy($nb, 0, $pl, 48, $nb.Length) }
    if ($maplen -gt 0) { [Array]::Copy($missMap, 0, $pl, 48 + $nb.Length, $maplen) }
    $msg = [Oti]::Encode(4, $fid, $pl, $pl.Length)
    $q0 = $script:txq.Count
    $rc = TxBody $msg
    $q1 = $script:txq.Count
    if ($q1 -gt 0 -or $rc -ne 0) { Write-Host "  [warn] 裁决发出去了吗？rc=$rc 队列 $q0→$q1（kind=$kind flags=$flags）" }
    return $rc
}

function Abort-FileTx([string]$why) {
    $t = $script:fileTx
    if (-not $t.active) { return }
    try { if ($t.fs) { $t.fs.Close() } } catch { }
    Write-Host "  [warn] 大文件发送中止：$why（已发 $($t.sent)/$($t.nparts) 片，$($t.name)）"
    $script:fileTx = New-TxState
}

function Start-FileTx([string[]]$paths) {
    if ($script:fileTx.active) { return $false }
    $ok = @(); $tot = [uint64]0
    foreach ($p in $paths) {
        try { $fi = New-Object System.IO.FileInfo $p } catch { continue }
        if ($fi.Exists) { $ok += $fi.FullName; $tot += [uint64]$fi.Length }
    }
    if ($ok.Count -eq 0 -or $tot -le $script:BIG_THRESHOLD) { return $false }
    $t = New-TxState
    $t.active = $true; $t.paths = $ok; $t.total = $tot
    $t.fid = [uint32](Get-Random -Minimum 1 -Maximum 2000000000)
    $t.t0 = Get-Date; $t.lastLog = Get-Date
    $script:fileTx = $t
    Write-Host "  [xfer] 大文件流式发送：$($ok.Count) 个文件 / $tot 字节（>8MB 走分片，内存恒定 64KB）"
    return $true
}

function Open-TxFile {
    $t = $script:fileTx
    $p = $t.paths[$t.cur]
    try { $t.fs = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite) }
    catch { Write-Host "  [warn] 打不开 $p：$($_.Exception.Message)"; return $false }
    $t.total = [uint64]$t.fs.Length
    $t.name = [IO.Path]::GetFileName($p)
    $t.nparts = [int][Math]::Ceiling($t.total / [double]$script:PART_MAX)
    if ($t.nparts -le 0) { $t.nparts = 1 }
    $t.sent = 0; $t.phase = 0; $t.rounds = 0; $t.failStreak = 0
    # 整文件 CRC **一次算好**（顺序读一遍）。不要在发片时增量累加：
    # 发失败重传会把同一片重复计入 → 对端校验必然失败（实测踩过）。
    $h = [uint32]0
    $cb = New-Object byte[] 1048576
    [void]$t.fs.Seek(0, [IO.SeekOrigin]::Begin)
    while (($rd = $t.fs.Read($cb, 0, $cb.Length)) -gt 0) { $h = [Oti]::Crc32Append($h, $cb, 0, $rd) }
    [void]$t.fs.Seek(0, [IO.SeekOrigin]::Begin)
    $t.crc = $h; $t.crcDone = $true
    $t.t0 = Get-Date; $t.lastLog = Get-Date
    Write-Host "  [xfer] 发送 $($t.name)：$($t.total) 字节 / $($t.nparts) 片（crc=$('{0:x8}' -f $h)，fid=$($t.fid)）"
    return $true
}

function Send-TxPart([int]$idx) {
    $t = $script:fileTx
    $off = [uint64]$idx * [uint64]$script:PART_MAX
    $n = [int][Math]::Min([int64]$script:PART_MAX, [int64]$t.total - [int64]$off)
    if ($n -lt 0) { $n = 0 }
    $buf = $t.buf
    if ($n -gt 0) {
        [void]$t.fs.Seek([int64]$off, [IO.SeekOrigin]::Begin)
        $rd = 0
        while ($rd -lt $n) {
            $r = $t.fs.Read($buf, $rd, $n - $rd)
            if ($r -le 0) { break }
            $rd += $r
        }
        if ($rd -ne $n) { Write-Host "  [warn] 读 $($t.name) 偏移 $off 只读到 $rd/$n"; return -2 }
    }
    $nb = [Text.Encoding]::UTF8.GetBytes($t.name)
    $body = 20 + $nb.Length + 4 + $n
    $pl = $t.pay
    [Oti]::Put16($pl, 0, [uint16]4); [Oti]::Put16($pl, 2, [uint16]3)
    [Oti]::Put32($pl, 4, [uint32]$t.fid); [Oti]::Put32($pl, 8, [uint32]0)
    [Oti]::Put32($pl, 12, [uint32]$body); [Oti]::Put32($pl, 16, [uint32]$body)
    [Oti]::Put32($pl, 20, [uint32]$idx)
    [Oti]::Put32($pl, 24, [uint32]$t.nparts)
    [Oti]::Put64($pl, 28, [uint64]$t.total)
    [Oti]::Put32($pl, 36, [uint32]$nb.Length)
    if ($nb.Length -gt 0) { [Array]::Copy($nb, 0, $pl, 40, $nb.Length) }
    [Oti]::Put32($pl, 40 + $nb.Length, [uint32]$n)
    if ($n -gt 0) { [Array]::Copy($buf, 0, $pl, 44 + $nb.Length, $n) }
    $msg = [Oti]::Encode(4, $t.fid, $pl, 44 + $nb.Length + $n)
    $wrc = TxBody $msg
    if ($wrc -eq 0) {
        if ($t.todo) { $t.todoI++ } else { $t.sent++ }
        $t.doneParts++
    }
    return $wrc
}

function Step-FileTx {
    # 接收端超时清理（对端掉线/中断时别把 .part 文件永远留着）
    $r = $script:fileRx
    if ($r.active -and ((Get-Date) - $r.lastPart).TotalSeconds -gt 60) {
        Write-Host "  [warn] 大文件接收超时（60 秒无新片），丢弃 $($r.name)（$($r.got)/$($r.nparts) 片）"
        try { if ($r.fs) { $r.fs.Close() } } catch { }
        try { Remove-Item -LiteralPath $r.partPath -Force -ErrorAction SilentlyContinue } catch { }
        $script:fileRx = New-RxState
    }

    $t = $script:fileTx
    if (-not $t.active) { return }
    # 双向**同时**传大文件会把管道拖到 0.1MB/s（实测 20MB 要 249 秒）——按 fid 确定性让路：
    # 我这边 fid 大就先停，等对端传完（两边比较的是同一对数 → 不会双双停住）。
    if ($script:fileRx.active -and $t.fid -gt $script:fileRx.fid) { return }
    if ($t.phase -eq 1) {                       # 等裁决
        if (((Get-Date) - $t.deadline).TotalSeconds -gt 30) { Abort-FileTx "等待对端校验超时（30 秒）" }
        return
    }
    if ($null -eq $t.fs) {
        if (-not $t.ContainsKey('buf') -or $null -eq $t.buf) {
            $t.buf = New-Object byte[] $script:PART_MAX
            $t.pay = New-Object byte[] (44 + 256 + $script:PART_MAX)
        }
        if (-not (Open-TxFile)) { Abort-FileTx "打开源文件失败"; return }
    }
    $k = 0
    while ($k -lt 16) {
        if ($t.todo) {
            if ($t.todoI -ge $t.todo.Count) { break }
            $idx = [int]$t.todo[$t.todoI]
        } else {
            if ($t.sent -ge $t.nparts) { break }
            $idx = [int]$t.sent
        }
        $rc = Send-TxPart $idx
        if ($rc -ne 0) {
            # 不推进游标（下一轮重试同一片），但要退避：这里的调用来自主循环，
            # 立刻重试只会把主循环打满（麒麟侧就是这么把设备读拖挂的）。
            $t.failStreak++
            if ($t.failStreak -eq 1 -or ($t.failStreak % 20) -eq 0) {
                Write-Host "  [warn] 第 $idx 片发送失败 rc=$rc（连续 $($t.failStreak) 次），退避重试"
            }
            if ($t.failStreak -ge 200) { Abort-FileTx "连续 200 次发送失败（设备/链路异常）"; return }
            Start-Sleep -Milliseconds 20
            break
        }
        $t.failStreak = 0
        $k++
    }
    if (((Get-Date) - $t.lastLog).TotalSeconds -ge 1) {
        $t.lastLog = Get-Date
        $sec = ((Get-Date) - $t.t0).TotalSeconds
        $mb = ([double]$t.doneParts * $script:PART_MAX) / 1MB
        $pct = 100.0 * $t.doneParts / $t.nparts
        Write-Host ("  [xfer] 发送 {0}：{1}/{2} 片（{3:N1}%）{4:N1} MB/s" -f $t.name, $t.sent, $t.nparts, $pct, $(if ($sec -gt 0) { $mb / $sec } else { 0 }))
    }
    $allSent = if ($t.todo) { $t.todoI -ge $t.todo.Count } else { $t.sent -ge $t.nparts }
    if ($allSent -and $t.phase -eq 0) {
        $rc = Send-FileCtrl $t.fid 1 0 $t.total $t.crc $t.nparts 0 $t.name
        $t.phase = 1
        $t.deadline = (Get-Date).AddSeconds(30)
        Write-Host "  [xfer] $($t.name) 全部 $($t.nparts) 片已发出，等待对端校验（crc=$('{0:x8}' -f $t.crc)）…"
    }
}

function Handle-FileVerdict([byte[]]$pl) {
    $t = $script:fileTx
    $kind = [BitConverter]::ToUInt16($pl, 20); $flags = [BitConverter]::ToUInt16($pl, 22)
    $total = [Oti]::Get64($pl, 24); $nparts = [BitConverter]::ToUInt32($pl, 36)
    $resume = [BitConverter]::ToUInt32($pl, 40); $nl = [BitConverter]::ToUInt32($pl, 44)
    if ($kind -ne 2) { return }
    Write-Host "  [xfer] 收到裁决 kind=$kind flags=0x$('{0:x}' -f $flags) len=$($pl.Length) nparts=$nparts nl=$nl resume=$resume 需要位图=$([int](($nparts + 7) / 8)) 字节"
    if (-not $t.active) { return }
    if ($flags -band 1) {                       # OK
        $sec = ((Get-Date) - $t.t0).TotalSeconds
        $mb = [double]$t.total / 1MB
        Write-Host ("  [xfer] ✔ {0} 传送完成（{1} 字节，{2:N1} 秒，{3:N1} MB/s）" -f $t.name, $t.total, $sec, $(if ($sec -gt 0) { $mb / $sec } else { 0 }))
        try { if ($t.fs) { $t.fs.Close() } } catch { }
        $t.fs = $null
        $t.todo = $null; $t.todoI = 0
        $t.done++; $t.bytes += $t.total
        $t.cur++
        if ($t.cur -ge $t.paths.Count) {
            Write-Host "  [xfer] ✔ 本批大文件全部完成：$($t.done) 个文件 / $($t.bytes) 字节"
            $script:fileTx = New-TxState
        } else {
            $t.rounds = 0
            [void](Open-TxFile)
        }
        return
    }
    if ($flags -band 4) { Abort-FileTx "对端放弃"; return }
    if ($flags -band 2) {                       # RESUME
        if ($t.rounds -ge 3) { Abort-FileTx "重传 3 轮仍未通过校验"; return }
        $t.rounds++
        $t.todo = $null; $t.todoI = 0
        $mapOff = 48 + $nl
        $need = [int][Math]::Floor(($t.nparts + 7) / 8)
        if (($flags -band 8) -and $pl.Length -ge ($mapOff + $need)) {
            # 按位图只补缺片（500MB 时能避免"缺口靠前 → 重发几百 MB"）
            $todo = New-Object System.Collections.ArrayList
            for ($i = 0; $i -lt $t.nparts; $i++) {
                if (($pl[$mapOff + [int]($i -shr 3)] -band [byte](1 -shl ($i -band 7))) -ne 0) { [void]$todo.Add($i) }
            }
            if ($todo.Count -eq 0) {
                Write-Host "  [xfer] 对端位图为空（可能要求整份重发），从头发"
                $t.sent = 0
            } else {
                $t.todo = $todo
                Write-Host "  [xfer] 对端缺 $($todo.Count) 片，按位图补发（第 $($t.rounds) 轮）"
            }
        } else {
            if ($resume -ge $t.nparts) { $resume = $t.nparts - 1 }
            $t.sent = [int]$resume
            Write-Host "  [xfer] 对端要求从第 $resume 片重发（第 $($t.rounds) 轮）"
        }
        $t.phase = 0
    }
}

function Handle-FilePart([byte[]]$pl) {
    $idx = [BitConverter]::ToUInt32($pl, 20); $nparts = [BitConverter]::ToUInt32($pl, 24)
    $total = [Oti]::Get64($pl, 28); $nl = [BitConverter]::ToUInt32($pl, 36)
    if ($nl -gt 1024 -or ($pl.Length -lt (44 + $nl))) { return }
    $name = [Text.Encoding]::UTF8.GetString($pl, 40, $nl)
    $dl = [BitConverter]::ToUInt32($pl, 40 + $nl)
    $dataOff = 44 + $nl
    if ($pl.Length -lt ($dataOff + $dl)) { return }
    $fid = [BitConverter]::ToUInt32($pl, 4)

    $r = $script:fileRx
    if ($r.active -and $r.fid -ne $fid) {
        Write-Host "  [warn] 大文件接收被新的一笔打断（旧 fid=$($r.fid)，已丢弃）"
        try { if ($r.fs) { $r.fs.Close() } } catch { }
        try { Remove-Item -LiteralPath $r.partPath -Force -ErrorAction SilentlyContinue } catch { }
        $script:fileRx = New-RxState
        $r = $script:fileRx
    }
    if (-not $r.active) {
        if ($nparts -le 0 -or $nparts -gt 1000000) { return }
        # 文件名消毒（防 ../ 之类）
        $safe = [IO.Path]::GetFileName($name)
        if ([string]::IsNullOrWhiteSpace($safe)) { $safe = 'unnamed' }
        $dir = Join-Path $env:TEMP ('otilink_files_' + $fid.ToString('x8'))
        [IO.Directory]::CreateDirectory($dir) | Out-Null
        $r = New-RxState
        $r.active = $true; $r.fid = $fid; $r.nparts = $nparts; $r.total = $total; $r.name = $safe
        $r.path = Join-Path $dir $safe; $r.partPath = $r.path + '.part'
        $r.bits = New-Object byte[] ([int][Math]::Floor(($nparts + 7) / 8))
        try { $r.fs = [IO.File]::Open($r.partPath, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
        catch { Write-Host "  [warn] 建不了接收文件 $($r.partPath)：$($_.Exception.Message)"; $script:fileRx = New-RxState; return }
        try { $r.fs.SetLength([int64]$total) } catch { }
        $r.t0 = Get-Date; $r.lastLog = Get-Date; $r.lastPart = Get-Date
        $script:fileRx = $r
        Write-Host "  [xfer] 开始接收大文件 $safe（$total 字节 / $nparts 片，fid=$fid）"
    }
    if ($idx -ge $r.nparts) { return }
    try {
        [void]$r.fs.Seek([int64]$idx * $script:PART_MAX, [IO.SeekOrigin]::Begin)
        if ($dl -gt 0) { $r.fs.Write($pl, $dataOff, [int]$dl) }
    } catch { Write-Host "  [warn] 写 $($r.partPath) 失败：$($_.Exception.Message)"; return }
    $bi = [int]($idx -shr 3); $mask = [byte](1 -shl ($idx -band 7))
    if (($r.bits[$bi] -band $mask) -eq 0) { $r.bits[$bi] = $r.bits[$bi] -bor $mask; $r.got++ }
    $r.lastPart = Get-Date
    if (((Get-Date) - $r.lastLog).TotalSeconds -ge 2) {
        $r.lastLog = Get-Date
        $sec = ((Get-Date) - $r.t0).TotalSeconds
        $mb = ([double]$r.got * $script:PART_MAX) / 1MB
        Write-Host ("  [xfer] 接收 {0}：{1}/{2} 片（{3:N1}%）{4:N1} MB/s" -f $r.name, $r.got, $r.nparts, (100.0 * $r.got / $r.nparts), $(if ($sec -gt 0) { $mb / $sec } else { 0 }))
    }
}

function Handle-FileDone([byte[]]$pl) {
    $fid = [BitConverter]::ToUInt32($pl, 4)
    $total = [Oti]::Get64($pl, 24); $crc = [BitConverter]::ToUInt32($pl, 32)
    $nparts = [BitConverter]::ToUInt32($pl, 36); $nl = [BitConverter]::ToUInt32($pl, 44)
    $name = ''
    if ($nl -le 1024 -and $pl.Length -ge (48 + $nl)) { $name = [Text.Encoding]::UTF8.GetString($pl, 48, $nl) }
    $r = $script:fileRx
    if (-not $r.active -or $r.fid -ne $fid) {
        Write-Host "  [warn] 收到未知传输的 DONE（fid=$fid），回 ABORT"
        [void](Send-FileCtrl $fid 2 4 0 0 0 0 $name)
        return
    }
    try { $r.fs.Flush(); $r.fs.Close() } catch { }
    $r.fs = $null
    if ($r.got -lt $r.nparts) {
        $miss = 0
        for ($i = 0; $i -lt $r.nparts; $i++) { if (($r.bits[[int]($i -shr 3)] -band [byte](1 -shl ($i -band 7))) -eq 0) { $miss = $i; break } }
        # 带"缺片位图"，让发端只补缺的片（不是从缺口一路发到结尾）
        $map = New-Object byte[] ([int][Math]::Floor(($r.nparts + 7) / 8))
        for ($i = 0; $i -lt $r.nparts; $i++) {
            if (($r.bits[[int]($i -shr 3)] -band [byte](1 -shl ($i -band 7))) -eq 0) {
                $map[[int]($i -shr 3)] = $map[[int]($i -shr 3)] -bor [byte](1 -shl ($i -band 7))
            }
        }
        Write-Host "  [warn] $($r.name) 缺 $($r.nparts - $r.got) 片，回缺片位图（从第 $miss 片起）"
        [void](Send-FileCtrl $fid 2 2 $r.total 0 $r.nparts $miss $r.name $map)
        return
    }
    # 整文件校验（把落盘结果读一遍算 CRC；500MB 约 1~2 秒，只在收全后做一次）
    $ok = $false; $sz = [uint64]0; $h = [uint32]0
    try {
        $fsv = [IO.File]::OpenRead($r.partPath)
        $buf = New-Object byte[] 1048576
        while (($rd = $fsv.Read($buf, 0, $buf.Length)) -gt 0) { $h = [Oti]::Crc32Append($h, $buf, 0, $rd); $sz += [uint64]$rd }
        $fsv.Close()
        $ok = ($sz -eq $r.total -and $h -eq $crc)
    } catch { $ok = $false }
    if (-not $ok) {
        Write-Host "  [warn] $($r.name) 校验失败（长度 $sz/$($r.total)，crc=$('{0:x8}' -f $h)/$('{0:x8}' -f $crc)），要求整份重发"
        [void](Send-FileCtrl $fid 2 2 $r.total 0 $r.nparts 0 $r.name)
        return
    }
    $final = $r.path
    try { Move-Item -LiteralPath $r.partPath -Destination $final -Force } catch {
        Write-Host "  [warn] 重命名失败：$($_.Exception.Message)"
        [void](Send-FileCtrl $fid 2 4 $r.total 0 $r.nparts 0 $r.name)
        $script:fileRx = New-RxState
        return
    }
    $sec = ((Get-Date) - $r.t0).TotalSeconds
    $mb = [double]$r.total / 1MB
    Write-Host ("  [xfer] ✔ {0} 接收完成（{1} 字节，{2:N1} 秒，{3:N1} MB/s）→ {4}" -f $r.name, $r.total, $sec, $(if ($sec -gt 0) { $mb / $sec } else { 0 }), $final)
    try { [void][ClipFiles]::Set(@($final)) } catch { }
    # 防回环：记下"我刚写进剪贴板的这份文件"的身份，下一轮轮询就不会再发回去
    $script:lastFileIdent = Get-FileIdent @($final)
    $script:haveFileIdent = $true
    [void](Send-FileCtrl $fid 2 1 $r.total 0 $r.nparts $r.nparts $r.name)
    $script:fileRx = New-RxState
}


# ---- 剪贴板确认（ACK + 超时重发）--------------------------------------------
# 为什么需要：帧通道**会静默丢帧**（设备打嗝/帧被跳过，两侧日志都不报错）——
# 实测麒麟→Windows 连发 15 条剪贴板丢 6 条（40%），反向 12/12 全到。
# 没有确认就没有重传，用户看到的就是"复制粘贴不了"。
$script:clipAck = @{ active = $false; crc = [uint32]0; fmt = 1; bytes = $null; deadline = (Get-Date); tries = 0 }
$script:nAckOk = 0; $script:nAckRetry = 0; $script:nAckFail = 0

function Send-ClipAck([uint32]$crc, [uint16]$fmt) {
    $pl = New-Object byte[] 8
    [Oti]::Put32($pl, 0, $crc); [Oti]::Put16($pl, 4, $fmt); [Oti]::Put16($pl, 6, 0)
    $msg = [Oti]::Encode(6, 0, $pl, 8)
    return (TxBody $msg)
}

# 发送一份剪贴板载荷（首发/重发共用）；成功后登记"待对端确认"
function Send-ClipPayload([byte[]]$bytes, [int]$clipFmt, [uint32]$crc, [string]$src, [switch]$NoRegister) {
    $script:seq++
    $fid = $script:seq
    $off = 0
    $txChunks = 0
    $swTx = [System.Diagnostics.Stopwatch]::StartNew()
    while ($off -lt $bytes.Length) {
        $n = [Math]::Min(65000, $bytes.Length - $off)
        $flags = 0
        if ($off -eq 0) { $flags = $flags -bor 1 }
        if (($off + $n) -eq $bytes.Length) { $flags = $flags -bor 2 }
        $payload = New-Object byte[] (20 + $n)
        [Oti]::Put16($payload, 0, [uint16]$clipFmt); [Oti]::Put16($payload, 2, [uint16]$flags)
        [Oti]::Put32($payload, 4, $fid); [Oti]::Put32($payload, 8, [uint32]$off)
        [Oti]::Put32($payload, 12, [uint32]$bytes.Length); [Oti]::Put32($payload, 16, [uint32]$n)
        [Array]::Copy($bytes, $off, $payload, 20, $n)
        $msg = [Oti]::Encode(4, $fid, $payload, $payload.Length)
        $wrc = 0
        if ($useTcp) {
            try {
                $hdr2 = [BitConverter]::GetBytes([uint32]$msg.Length)
                [void]$client.Client.Send($hdr2, 0, 4, [System.Net.Sockets.SocketFlags]::None)
                [void]$client.Client.Send($msg, 0, $msg.Length, [System.Net.Sockets.SocketFlags]::None)
            } catch { $wrc = -1; Write-Host ("  [warn] TCP 写失败: " + $_.Exception.Message) }
        } else {
            $wrc = TxBody $msg
        }
        if ($wrc -ne 0) { Write-Host "  写帧失败 rc=$wrc" }
        $off += $n
        $script:txClip++
        $txChunks++
    }
    $swTx.Stop()
    $mbs = if ($swTx.ElapsedMilliseconds -gt 0) { [Math]::Round($bytes.Length / 1MB / ($swTx.ElapsedMilliseconds / 1000.0), 2) } else { 0 }
    Write-Host "  CLIP 发送 $($bytes.Length) 字节 / 本笔 $txChunks 块（累计 $script:txClip）fid=$fid fmt=$clipFmt 源=$src 耗时 $($swTx.ElapsedMilliseconds)ms（$mbs MB/s）"
    # 登记待确认：对端应用后会回 ACK（type=6）。重发时**不重新登记**（否则重试计数被清零 → 无限重发）
    if (-not $NoRegister) {
        $script:clipAck = @{ active = $true; crc = $crc; fmt = $clipFmt; bytes = $bytes;
                             deadline = (Get-Date).AddMilliseconds(1500); tries = 0 }
    }
    return 0
}

# 超时未确认 → 重发（最多 3 次）
function Step-ClipAck {
    $k = $script:clipAck
    if (-not $k.active) { return }
    if ((Get-Date) -lt $k.deadline) { return }
    if ($k.tries -ge 3) {
        Write-Host "  [warn] 剪贴板 $($k.bytes.Length) 字节重发 3 次仍无确认，放弃"
        $script:nAckFail++
        $k.active = $false; $k.bytes = $null
        return
    }
    $k.tries++
    $script:nAckRetry++
    Write-Host "  [clip] 未确认 → 第 $($k.tries) 次重发 $($k.bytes.Length) 字节（crc=$('{0:x8}' -f $k.crc)）"
    $t = $k.tries
    [void](Send-ClipPayload $k.bytes $k.fmt $k.crc '重发' -NoRegister)
    $k.tries = $t                       # Send-ClipPayload 不动它，这里显式保留
    $k.deadline = (Get-Date).AddMilliseconds(1500)
}

Write-Host "开始循环（Ctrl+C 退出）…"
Init-Tray
$lastTray = [DateTime]::MinValue
$lastPump = [DateTime]::MinValue
# ---- 慢轮诊断（长期保留）：帧大量丢失的直接原因是"主循环整段卡住"，
#      把"整轮间隔"与"取帧之前那段耗时"分开，一眼看出卡在哪一段。----
$script:lastIter0 = $null
$script:slowLogAt = Get-Date
$script:profPre = 0
while ($true) {
    $iter0 = Get-Date
    if ($script:lastIter0) {
        $gapMs = ($iter0 - $script:lastIter0).TotalMilliseconds
        if ($gapMs -gt 1200 -and (($iter0 - $script:slowLogAt).TotalSeconds -ge 2)) {
            $script:slowLogAt = $iter0
            Write-Host ("LOOP 慢轮: 距上轮 {0:N0}ms；取帧前 {1:N0}ms = A[托盘/剪贴板ACK/传输] {2:N0} + B[心跳/输入探测/角色/发ROLE] {3:N0} + C[取帧] {4:N0}" -f $gapMs, $script:profPre, $ptA, ($ptB - $ptA), ($script:profPre - $ptB))
        }
    }
    $script:lastIter0 = $iter0
    if ($script:quit) { Write-Host "从托盘退出"; break }
    if ($DurationSec -gt 0 -and ((Get-Date) - $start).TotalSeconds -ge $DurationSec) { break }
    # 托盘消息泵 + 提示刷新（WinForms 需要定期 DoEvents）
    if ($script:tray -and ((Get-Date) - $lastPump).TotalMilliseconds -ge 50) {
        $lastPump = Get-Date
        try { [System.Windows.Forms.Application]::DoEvents() } catch {}
        if (((Get-Date) - $lastTray).TotalSeconds -ge 1) {
            $lastTray = Get-Date
            try { $script:tray.Text = Get-TrayText } catch {}
        }
    }

    # 0.4) 剪贴板确认：超时没等到 ACK 就重发（帧通道会静默丢帧）
    if ($Clipboard) { Step-ClipAck }

    # 0.5) 大文件流式传输：每轮推进一小批（不长时间占住主循环，
    #      这样收到的裁决/剪贴板消息仍能在下一轮被处理）
    if ($Clipboard) { Step-FileTx }
    $ptA = [int]((Get-Date) - $iter0).TotalMilliseconds   # A = 托盘+剪贴板ACK+文件传输 之后

    # 0) 周期性上报本机屏幕几何（10 秒一次）
    if (((Get-Date) - $lastHello).TotalSeconds -ge 10) {
        $lastHello = Get-Date
        Send-Hello
    }

    # 0.6) 角色协商：本机输入探测（每 3s）/ 决策与起停 otiagent2（每 1s，I1/I4/L4）
    if (((Get-Date) - $script:roleLastProbe).TotalSeconds -ge 10) {
        # 间隔为什么是 20 秒：Get-PnpDevice 是 WMI 枚举，实测单次把主循环卡住 5.2 秒
        # （LOOP 慢轮诊断 A=21ms / B=5374ms / C=39ms）→ 期间一帧都收不到 → 帧大量丢失。
        # 键鼠换边是低频人工事件，20 秒延迟可接受；根治是挪到后台 runspace（NOTES §57）。
        $script:roleLastProbe = Get-Date
        Update-LocalInput
    }
    if (((Get-Date) - $script:roleLastDecide).TotalSeconds -ge 1) {
        $script:roleLastDecide = Get-Date
        Update-Role
    }

    # 0.7) 角色协商：每 1 秒广播一条 ROLE；每 5 秒打印一行对端 ROLE（证据行，前缀固定 "ROLE: peer"）。
    #      每 1 秒广播一条 ROLE；每 5 秒打印一行对端 ROLE（证据行，前缀固定 "ROLE: peer"）。
    if (((Get-Date) - $script:roleLastTx).TotalSeconds -ge 1) {
        $script:roleLastTx = Get-Date
        Send-Role
    }
    $ptB = [int]((Get-Date) - $iter0).TotalMilliseconds   # B = 心跳+输入探测+角色决策+发ROLE 之后
    if (((Get-Date) - $script:roleLastLog).TotalSeconds -ge 5) {
        $script:roleLastLog = Get-Date
        if ($script:rolePeerSeen) {
            $peerDelta = [int]((Get-Date) - $script:rolePeerMs).TotalMilliseconds
            Write-Host ("ROLE: peer state={0} hasLocal={1} age={2} want={3} flags={4} boot={5:x8}（{6}ms 前收到）| 本机 state={7} hasLocal={8} age={9} want={10} grab={11}" -f $script:rolePeerState, $script:rolePeerHasLocal, $script:rolePeerAge, $script:rolePeerWant, $script:rolePeerFlags, $script:rolePeerBoot, $peerDelta, $script:roleState, $script:roleHasLocal, (Get-RoleInputAge), $script:roleWant, $(if ($script:roleGrabOk) { '可' } else { '否' }))
        } else {
            Write-Host ("ROLE: peer 未收到（本机 boot={0:x8}，已每 1s 广播）" -f $script:roleBoot)
        }
    }

    # 1) 取一条消息体（TCP = 4 字节长度前缀；SPTI = 从 64KB 帧里取，跳过空闲/非法帧）
    $body = $null; $len = 0; $didWork = $false
    if ($useTcp) {
        $body = RxTcp
        if ($null -ne $body) { $len = $body.Length }
    } elseif ($useCable) {
        $didWork = Pump-Cable
        if ($script:rxq.Count -gt 0) {
            $body = $script:rxq.Dequeue()
            $len = $body.Length
            $didWork = $true
        }
    } else {
        $rc = [Oti]::ReadFrame($h, $frame, 1)
        if ($rc -eq 0) {
            $chk = [Oti]::FrameCheck($frame)
            if ($chk -eq 1) {
                $len = [Oti]::FrameBodyLen($frame)
                $body = New-Object byte[] $len
                [Array]::Copy($frame, 20, $body, 0, $len)
            } elseif ($chk -eq -1) { Write-Host "  [warn] 非法帧（首尾头不一致）" }
        } elseif ($Verbose -and $rc -ne 0) { Write-Host "  读失败 rc=$rc" }
    }
    if ($useCable -and -not $didWork -and $null -eq $body) {
        Start-Sleep -Milliseconds 2      # 空闲小睡：既省 CPU 又不影响延迟
    }
    $script:profPre = ((Get-Date) - $iter0).TotalMilliseconds   # 取帧之后的那一段（见慢轮诊断）
    if ($null -ne $body -and $len -gt 0) {
        $t = [uint16]0; $s = [uint32]0; $pl = $null
        if ([Oti]::Decode($body, $len, [ref]$t, [ref]$s, [ref]$pl)) {
            $rx++
                switch ($t) {
                    1 {  # KEY: payload = code u16, value u8
                        # 按 seq 去重：Kylin 侧为对抗帧丢失会把同一条 KEY 重发 3 次（re/NOTES.md §61）
                        if ($null -eq $script:seenSeq) { $script:seenSeq = @() }
                        $dup = $script:seenSeq -contains $s
                        if (-not $dup) {
                            $script:seenSeq += $s
                            if ($script:seenSeq.Count -gt 64) { $script:seenSeq = @($script:seenSeq | Select-Object -Last 32) }
                        }
                        if ((-not $dup) -and ($Inject -or $InjectKeys) -and -not $script:paused -and $pl.Length -ge 3) {
                            $code = [BitConverter]::ToUInt16($pl, 0)
                            if ([Oti]::Key($code, ($pl[2] -ne 0))) {
                                $injected++
                                if ($Verbose) { Write-Host "  KEY code=$code value=$($pl[2])" }
                            } elseif ($Verbose) {
                                Write-Host "  KEY code=$code 未映射（跳过，宁可不按错键）"
                            }
                        }
                    }
                    2 {  # MOUSE: dx i16, dy i16, wheel i16, buttons u16
                        if ($Inject -and -not $script:paused -and $pl.Length -ge 8) {
                            $dx = [BitConverter]::ToInt16($pl, 0); $dy = [BitConverter]::ToInt16($pl, 2)
                            $wh = [BitConverter]::ToInt16($pl, 4)
                            [Oti]::Mouse($dx, $dy, 0, 0); if ($wh -ne 0) { [Oti]::Wheel($wh) }
                            Apply-Buttons ([int][BitConverter]::ToUInt16($pl, 6))
                            $injected++
                            if ($Verbose) { Write-Host "  MOUSE dx=$dx dy=$dy wheel=$wh btn=0x$([BitConverter]::ToUInt16($pl,6).ToString('x'))" }
                        }
                    }
                    3 {  # SWITCH
                        if ($pl.Length -ge 2) { Write-Host "  SWITCH side=$($pl[0]) 指针$(if($pl[0] -eq 1){'在本机'}else{'在对端'})" }
                    }
                    4 {  # CLIP: format u16, flags u16, fid u32, offset u32, total u32, dlen u32, data
                        # 门槛必须 >= 20（= 剪贴板头本身：fmt2+flags2+fid4+off4+total4+dlen4）！
                # 原来是 -ge 24 → 1~3 个字符的短文本载荷只有 21/22/23 字节，整帧被丢掉：
                # 不应用、不 ACK，发送端重发 3 次后放弃 —— 现场表现"1/2/3 字符复制不过去，
                # >=4 个字符才行"（第 41 轮，用户报障）。回归：otilink/clipshort.sh
                if ($Clipboard -and -not $script:paused -and $pl.Length -ge 20) {
                            $fmt   = [BitConverter]::ToUInt16($pl, 0)   # 1=文本 2=PNG 3=文件包 4=大文件分片 5=传输控制
                            $flags = [BitConverter]::ToUInt16($pl, 2)
                            $fid = [BitConverter]::ToUInt32($pl, 4); $off = [BitConverter]::ToUInt32($pl, 8)
                            $total = [BitConverter]::ToUInt32($pl, 12); $dlen = [BitConverter]::ToUInt32($pl, 16)
                            if ($fmt -eq 4) {
                                # 大文件分片：直接落盘（不进重组缓冲，内存恒定 64KB）
                                Handle-FilePart $pl
                            } elseif ($fmt -eq 5) {
                                $kind = [BitConverter]::ToUInt16($pl, 20)
                                if ($kind -eq 1) { Handle-FileDone $pl } else { Handle-FileVerdict $pl }
                            } else {
                            if ($Verbose) { Write-Host "  CLIP 收块 fmt=$fmt flags=$flags fid=$fid off=$off total=$total dlen=$dlen" }
                            if ((($flags -band 1) -ne 0) -and $clipRx.active -and $clipRx.got -lt $clipRx.total) {
                                Write-Host "  [warn] CLIP 前一笔未收完（fid=$($clipRx.fid) got=$($clipRx.got)/$($clipRx.total)）被 fid=$fid 打断"
                            }
                            if ($clipRx.active -and $fid -eq $clipRx.fid -and $off -ne $clipRx.got) {
                                Write-Host "  [warn] CLIP 乱序/缺口 fid=$fid off=$off 期望=$($clipRx.got) total=$total dlen=$dlen"
                            }
                            if (($flags -band 1) -ne 0) {
                                $clipRx.active = $true; $clipRx.fid = $fid; $clipRx.total = $total
                                $clipRx.fmt = $fmt
                                $clipRx.got = 0; $clipRx.buf = New-Object byte[] $total
                            }
                            if ($clipRx.active -and $fid -eq $clipRx.fid -and ($off + $dlen) -le $clipRx.total) {
                                [Array]::Copy($pl, 20, $clipRx.buf, $off, $dlen)
                                $clipRx.got = $off + $dlen
                                if (($flags -band 2) -ne 0) {
                                    if ($clipRx.fmt -eq 2) {
                                        # PNG -> CF_DIB 写进剪贴板（Win32 API，见 ClipImg）
                                        $png = if ($clipRx.got -eq $clipRx.buf.Length) { $clipRx.buf } else { $clipRx.buf[0..($clipRx.got-1)] }
                                        $ok = [ClipImg]::SetPng($png)
                                        $lastClipHash = [Oti]::Crc32($clipRx.buf, 0, $clipRx.got)
                                        $haveClipHash = $true
                                        # 关键：同时记下"我刚写进去的 DIB 指纹"，否则下一轮轮询
                                        # 会把这张图重新编码成 PNG 再发回去（PNG 字节必然不同）。
                                        $lastDibHash = [ClipImg]::DibHash(); $haveDibHash = ($lastDibHash -ne 0)
                                        $clipRx.active = $false
                                        Write-Host "  CLIP 已应用远端**图片** $($clipRx.got) 字节 rc=$ok（并抑制回发）"
                                        [void](Send-ClipAck $lastClipHash ([uint16]2))
                                    } elseif ($clipRx.fmt -eq 3) {
                                        # 文件包：u16 个数；每项 u32 名长、名字(UTF-8)、u64 长度、内容
                                        $p = 2
                                        $cnt = [BitConverter]::ToUInt16($clipRx.buf, 0)
                                        $dir = Join-Path $env:TEMP ('otilink_files_' + [Guid]::NewGuid().ToString('N'))
                                        [IO.Directory]::CreateDirectory($dir) | Out-Null
                                        $paths = @()
                                        for ($k = 0; $k -lt $cnt; $k++) {
                                            $nl = [BitConverter]::ToUInt32($clipRx.buf, $p); $p += 4
                                            $nm = [Text.Encoding]::UTF8.GetString($clipRx.buf, $p, $nl); $p += $nl
                                            $sz = [BitConverter]::ToUInt64($clipRx.buf, $p); $p += 8
                                            $fp = Join-Path $dir $nm
                                            $ob = New-Object byte[] ([int]$sz)
                                            [Array]::Copy($clipRx.buf, $p, $ob, 0, [int]$sz); $p += [int]$sz
                                            [IO.File]::WriteAllBytes($fp, $ob)
                                            $paths += $fp
                                        }
                                        $okf = [ClipFiles]::Set($paths)
                                        $lastClipHash = [Oti]::Crc32($clipRx.buf, 0, $clipRx.got)
                                        $haveClipHash = $true
                                        $clipRx.active = $false
                                        Write-Host "  CLIP 已应用远端**文件** $cnt 个 rc=$okf -> $dir"
                                        [void](Send-ClipAck $lastClipHash ([uint16]3))
                                        $script:lastFileIdent = Get-FileIdent $paths   # 防回环：刚落的文件别再发回去
                                        $script:haveFileIdent = $true
                                    } else {
                                        $text = [Text.Encoding]::UTF8.GetString($clipRx.buf, 0, $clipRx.got)
                                        Set-Clipboard -Value $text
                                        $lastClipHash = [Oti]::Crc32([Text.Encoding]::UTF8.GetBytes($text), 0, [Text.Encoding]::UTF8.GetByteCount($text))
                                        $haveClipHash = $true
                                        $clipRx.active = $false
                                        Write-Host "  CLIP 已应用远端剪贴板 $($clipRx.got) 字节（并抑制回发）"
                                        if ($Verbose) { Write-Host ("  → 回执 ACK crc={0:x8}" -f $lastClipHash) }
                                        [void](Send-ClipAck $lastClipHash ([uint16]1))
                                    }
                                }
                            }
                            }   # end else（普通剪贴板重组路径）
                        }
                    }
                    7 {  # MOUSE_ABS: x i16, y i16, wheel i16, buttons u16（对端屏幕坐标）
                        if ($Inject -and -not $script:paused -and $pl.Length -ge 8) {
                            $ax = [BitConverter]::ToInt16($pl, 0); $ay = [BitConverter]::ToInt16($pl, 2)
                            $aw = [BitConverter]::ToInt16($pl, 4)
                            $sw = if ($AbsW -gt 0) { $AbsW } else { [Oti]::VirtW() }
                            $sh = if ($AbsH -gt 0) { $AbsH } else { [Oti]::VirtH() }
                            [Oti]::MouseAbs($ax, $ay, $sw, $sh)
                            if ($aw -ne 0) { [Oti]::Wheel($aw) }
                            Apply-Buttons ([int][BitConverter]::ToUInt16($pl, 6))
                            $injected++
                            if ($Verbose) { Write-Host "  MOUSE_ABS x=$ax y=$ay wheel=$aw（绝对注入，无加速）" }
                        }
                    }
                    8 {  # HELLO: 对端屏幕宽高
                        if ($pl.Length -ge 4) {
                            $pw = [BitConverter]::ToUInt16($pl, 0); $ph = [BitConverter]::ToUInt16($pl, 2)
                            $script:peerW = $pw; $script:peerH = $ph
                            Write-Host "  HELLO 主控端屏幕 ${pw}x${ph}"
                        }
                    }
                    6 {  # ACK：剪贴板确认（对端说我发的东西真落地了）
                        if ($pl.Length -ge 8) {
                            $acrc = [BitConverter]::ToUInt32($pl, 0)
                            if ($Verbose) { Write-Host ("  RECV ACK crc={0:x8}（待确认={1:x8} active={2}）" -f $acrc, $script:clipAck.crc, $script:clipAck.active) }
                            if ($script:clipAck.active -and $acrc -eq $script:clipAck.crc) {
                                $script:clipAck.active = $false
                                $script:clipAck.bytes = $null
                                $script:nAckOk++
                                if ($Verbose) { Write-Host ("  ACK 收到（crc={0:x8}）确认成功 {1} 次" -f $acrc, $script:nAckOk) }
                            }
                        }
                    }
                    5 { if ($Verbose) { Write-Host "  PING" } }
                    10 {  # ROLE: has_local_input u8, want u8, state u8, flags u8, boot_id u32, input_age_ms u32
                        # 第 1 步只解析+存变量（每 5 秒由主循环打印一行 ROLE:）；不据此起停 otiagent2。
                        if ($pl.Length -ge 12) {
                            $script:rolePeerSeen     = $true
                            $script:rolePeerHasLocal = [int]$pl[0]
                            $script:rolePeerWant     = [int]$pl[1]
                            $script:rolePeerState    = [int]$pl[2]
                            $script:rolePeerFlags    = [int]$pl[3]
                            $script:rolePeerBoot     = [BitConverter]::ToUInt32($pl, 4)
                            $script:rolePeerAge      = [BitConverter]::ToUInt32($pl, 8)
                            $script:rolePeerMs       = Get-Date
                            # I1：连续 3 个心跳看到对端 state=slave 才允许 grab（起 otiagent2）
                            if ($script:rolePeerState -eq 2) { $script:rolePeerSlaveStreak++ } else { $script:rolePeerSlaveStreak = 0 }
                            if ($Verbose) { Write-Host ("  ROLE 收到 state={0} hasLocal={1} want={2} flags={3} boot={4:x8} age={5}ms streak={6}" -f $script:rolePeerState, $script:rolePeerHasLocal, $script:rolePeerWant, $script:rolePeerFlags, $script:rolePeerBoot, $script:rolePeerAge, $script:rolePeerSlaveStreak) }
                        }
                    }
                }
            } elseif ($len -ge 5 -and $body[0] -eq 0x39) {
                # 厂商 XML 帧：帧体 = [0x39][u32 长度][XML]。麒麟端 otikm 在"撞边交还控制权"
                # 时会按厂商线格式发 Cmd_Notify_KM_Switch_To_Local（668 字节），厂商程序
                # （MacKMLink）也往同一条帧管道里写。这些**不是**我们的协议消息，
                # 以前一律打成 "[warn] 解包失败（CRC/格式）"，被误当成剪贴板传坏了
                # （整整误导了一轮排查）。这里单独识别、单独计数。
                $vl = [BitConverter]::ToUInt32($body, 1)
                $script:vendorRx++
                Write-Host "  [skip] 厂商 XML 帧 $vl 字节（第 $($script:vendorRx) 条，非本协议，正常忽略）"
            } else {
                # 诊断：把失败帧的长度与头 16 字节打出来。厂商程序（MacKMLink）或
                # otikm 的厂商 XML 通知帧也走同一条帧管道，它们**不是**我们的格式，
                # 会在这里被判失败 —— 必须能看到内容才能区分"噪声"与"真损坏"。
                $nh = [Math]::Min(16, $len)
                $hx = (($body[0..($nh-1)] | ForEach-Object { $_.ToString('x2') }) -join ' ')
                Write-Host ("  [warn] 解包失败（CRC/格式）len={0} head={1}" -f $len, $hx)
            }
    }

    # 2) 剪贴板：本地变化则分块发送（按 ClipIntervalMs 节流，避免每 5ms 读剪贴板）
    if ($Clipboard -and (((Get-Date) - $lastClipCheck).TotalMilliseconds -ge $ClipIntervalMs)) {
        $lastClipCheck = Get-Date
        try {
            $bytes = $null; $clipFmt = 1; $src = '无'; $curDibHash = 0; $ovs = ''; $skipSend = $false
            try {
                if ([ClipFiles]::HasFiles()) {
                    $fs = [ClipFiles]::Get()
                    $src = '文件'
                    if ($Verbose) { Write-Host "  [clip] HDROP 命中：$($fs.Count) 个文件" }
                    if ($null -eq $fs -or $fs.Count -eq 0) {
                        # 有不代表读得出：OpenClipboard 会被别的进程短暂占住（err=5 权限拒绝）。
                        # 这种**瞬时失败**绝不能退化成"发文件路径文本"——本轮什么都不发，300ms 后重试。
                        Write-Host "  [warn] 剪贴板有文件却读不出来（本轮跳过）：[ClipFiles]::LastError=$([ClipFiles]::LastError)"
                        $skipSend = $true
                    }
                    if ($fs -and $fs.Count -gt 0 -and $fs.Count -le 16) {
                        # 先算"身份"指纹（路径+大小+mtime）：**不读内容**就能判断还是不是那一份。
                        # 大文件一旦超过 8MB 就走分片流式（内存恒定 64KB），小文件仍是原来的文件包。
                        $fident = Get-FileIdent $fs
                        $ftotal = [uint64]0
                        foreach ($f in $fs) { try { $ftotal += [uint64](New-Object System.IO.FileInfo $f).Length } catch { } }
                        if ($haveFileIdent -and $fident -eq $lastFileIdent) {
                            $src = '文件(未变，跳过)'
                        } elseif ($ftotal -gt 8000000) {
                            if ($script:fileRx.active) {
                                $src = '文件(大文件，正在收对端的，稍后再发)'
                            } elseif (Start-FileTx $fs) {
                                $lastFileIdent = $fident; $haveFileIdent = $true
                                $src = '文件(大文件流式)'
                            } else {
                                $src = '文件(大文件，传输占用中)'
                            }
                        } else {
                            $ms = New-Object System.IO.MemoryStream
                            $bw = New-Object System.IO.BinaryWriter($ms)
                            $bw.Write([uint16]$fs.Count)
                            $tot = 0
                            foreach ($f in $fs) {
                                $fb = [IO.File]::ReadAllBytes($f)
                                $nb = [Text.Encoding]::UTF8.GetBytes([IO.Path]::GetFileName($f))
                                $bw.Write([uint32]$nb.Length); $bw.Write($nb)
                                $bw.Write([uint64]$fb.Length); $bw.Write($fb)
                                $tot += $fb.Length
                                if ($tot -gt 8000000) { break }
                            }
                            if ($tot -le 8000000) {
                                $bw.Flush(); $bytes = $ms.ToArray(); $clipFmt = 3
                                $lastFileIdent = $fident; $haveFileIdent = $true
                            } else { $ovs = "文件包 $($tot + 22) 字节（$($fs.Count) 个文件）" }
                            $bw.Dispose(); $ms.Dispose()
                        }
                    }
                }
            } catch { }
            try {
                if ($null -eq $bytes -and [ClipImg]::HasImage()) {
                    # 先比**像素指纹**再决定要不要编码成 PNG：如果剪贴板里的图正是
                    # 我们刚写入/刚发出去的那张，就没必要回发（PNG 字节对不上，
                    # 但图是同一张 —— 这就是图片"回声"的来源）。
                    $dibh = [ClipImg]::DibHash()
                    if ($dibh -ne 0 -and $haveDibHash -and $dibh -eq $lastDibHash) {
                        $src = '图片(未变，跳过)'
                    } else {
                        $png = [ClipImg]::GetPng()
                        if ($png -and $png.Length -gt 8) {
                            $bytes = $png; $clipFmt = 2; $src = '图片'; $curDibHash = $dibh
                        }
                    }
                }
            } catch { }
            if ($null -eq $bytes) {
                $text = Get-Clipboard -Raw -ErrorAction Stop
                if ($null -ne $text -and $text.Length -gt 0) { $bytes = [Text.Encoding]::UTF8.GetBytes($text); $src = '文本' }
            }
            if ($skipSend) { $bytes = $null }
            # 超限要**说出来**：以前是静默不发（对面永远等不到，日志里一个字都没有，
            # 排查时完全看不出是"太大"还是"没触发"）。按内容去重，避免每 300ms 刷屏。
            if ($ovs -eq '' -and $null -ne $bytes -and $bytes.Length -gt 8388608) { $ovs = "$($bytes.Length) 字节" }
            if ($ovs -ne $script:lastOvs) {
                $script:lastOvs = $ovs
                if ($ovs -ne '') {
                    Write-Host "  [warn] 剪贴板内容超过上限（$ovs > 8MB）——本次不发送；可换共享卷方案或调大上限（见 RUNBOOK §15.13.5）"
                }
            }
            if ($null -ne $bytes -and $bytes.Length -gt 0) {
                if ($bytes.Length -le 8388608) {
                    $hash = [Oti]::Crc32($bytes, 0, $bytes.Length)
                    if ((-not $haveClipHash) -or $hash -ne $lastClipHash) {
                        $lastClipHash = $hash; $haveClipHash = $true
                        if ($clipFmt -eq 2 -and $curDibHash -ne 0) { $lastDibHash = $curDibHash; $haveDibHash = $true }
                        [void](Send-ClipPayload $bytes $clipFmt $hash $src)
                    }
                }
            }
        } catch { }
    }

    Start-Sleep -Milliseconds 5
}

Write-Host "结束: 收到 $rx 条消息，注入 $injected 次，剪贴板发块 $txClip"
try { if ($script:btnState -ne 0) { Apply-Buttons 0 } } catch {}   # 退出前松开所有按键
if ($script:tray) { try { $script:tray.Visible = $false; $script:tray.Dispose() } catch {} }
if (-not $useTcp) { [Oti]::CloseDevice($h) } else { $client.Close() }
