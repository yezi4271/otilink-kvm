// otiagent2.cs -- Windows-side capture agent for the OTi USB2.0 transfer cable.
//
// Design (see re/NOTES.md 41):
//   The cable itself enumerates as a REAL USB mouse + keyboard on the other PC
//   (MI_01 / MI_02). So this agent only has to CAPTURE local input and push
//   12-byte vendor HID packets over the SCSI HID channel:
//
//       CDB(16) = D9 | 0x33 mouse / 0x34 keyboard | 12 payload bytes | 'O' | 'T'
//       mouse  payload: [buttons][dx int8][dy int8][wheel int8][8 unused]
//       kbd    payload: [modifiers][reserved][k1..k6][4 unused]
//
//   No injection software is needed on the receiving side at all.
//
// Modes:
//   LOCAL  - Windows keeps its own input. Moving the cursor into the configured
//            hand-over edge switches to REMOTE.
//   REMOTE - all mouse/keyboard events are swallowed and forwarded to the peer.
//            The return hotkey (default Ctrl+Alt+Right) switches back to LOCAL.
//
// ASCII only. Build:
//   csc.exe /target:exe /out:otiagent2.exe otiagent2.cs

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

static class OtiAgent2
{
    // ---------------------------------------------------------------- SPTI
    [StructLayout(LayoutKind.Sequential)]
    struct SCSI_PASS_THROUGH_DIRECT
    {
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
    const int SENSE_LEN = 32;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec,
                                     uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr inBuf, uint inSize,
                                       IntPtr outBuf, uint outSize, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    static IntPtr dev = IntPtr.Zero;
    static string devPath = @"\\.\H:";
    static int sptiSchema = 0;

    static bool OpenDevice()
    {
        dev = CreateFileW(devPath, 0x80000000u | 0x40000000u, 3u, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (dev == (IntPtr)(-1)) { dev = IntPtr.Zero; return false; }
        return true;
    }

    // ---- 设备自动发现（拔插 / 盘符变化后自愈）--------------------------------
    // 写死 \\.\H: 时，只要线缆重新枚举后盘符变了（replug/重启很常见），
    // 代理就会永久失效。这里枚举盘符，谁能应答厂商设备信息块（0xF0/0x00）就用谁。
    // 只碰盘符，不碰 \\.\PhysicalDriveN（整块物理盘，权限/副作用都不该碰）。
    static bool ProbeInfo(string path)
    {
        IntPtr h = CreateFileW(path, 0x80000000u, 3u, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == (IntPtr)(-1) || h == IntPtr.Zero)
            h = CreateFileW(path, 0x80000000u | 0x40000000u, 3u, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == (IntPtr)(-1) || h == IntPtr.Zero) return false;
        int sz = Marshal.SizeOf(typeof(SCSI_PASS_THROUGH_DIRECT));
        IntPtr buf = Marshal.AllocHGlobal(sz + SENSE_LEN);
        IntPtr data = Marshal.AllocHGlobal(64);
        try
        {
            var s = new SCSI_PASS_THROUGH_DIRECT();
            s.Length = (ushort)sz; s.CdbLength = 16; s.SenseInfoLength = SENSE_LEN;
            s.DataIn = 1; s.DataTransferLength = 64; s.TimeOutValue = 3;
            s.DataBuffer = data; s.SenseInfoOffset = (uint)sz;
            s.Cdb = new byte[16];
            s.Cdb[0] = 0xF0; s.Cdb[14] = (byte)'O'; s.Cdb[15] = (byte)'T';
            uint ret = 0;
            bool ok = DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, buf, (uint)(sz + SENSE_LEN),
                                      buf, (uint)(sz + SENSE_LEN), out ret, IntPtr.Zero);
            if (!ok) return false;
            var o = (SCSI_PASS_THROUGH_DIRECT)Marshal.PtrToStructure(buf, typeof(SCSI_PASS_THROUGH_DIRECT));
            return o.ScsiStatus == 0;
        }
        catch { return false; }
        finally { Marshal.FreeHGlobal(buf); Marshal.FreeHGlobal(data); CloseHandle(h); }
    }

    static string FindCableDevice()
    {
        var list = new System.Collections.Generic.List<string>();
        try
        {
            foreach (var d in System.IO.DriveInfo.GetDrives())
            {
                if (d.Name.Length < 2 || d.Name[1] != ':') continue;
                string p = @"\." + d.Name.Substring(0, 2);
                // CD-ROM / 可移动优先（线缆就是这两种：D: 厂商 ISO、H: 可移动卷）
                if (d.DriveType == System.IO.DriveType.CDRom || d.DriveType == System.IO.DriveType.Removable)
                    list.Insert(0, p);
                else
                    list.Add(p);
            }
        }
        catch { }
        foreach (var p in list)
            if (ProbeInfo(p)) { Console.WriteLine("auto-detected cable device: " + p); return p; }
        return null;
    }

    static int sendOk = 0, sendErr = 0;

    static void SendCdb(byte[] cdb)
    {
        if (dev == IntPtr.Zero)
        {
            if (!OpenDevice())
            {
                // 拔插后盘符可能变了：重新发现一次（"拔线重插要复原"的关键）
                string alt = FindCableDevice();
                if (alt != null) devPath = alt;
                if (!OpenDevice()) { sendErr++; return; }
            }
        }
        int sz = Marshal.SizeOf(typeof(SCSI_PASS_THROUGH_DIRECT));
        IntPtr buf = Marshal.AllocHGlobal(sz + SENSE_LEN);
        try
        {
            var s = new SCSI_PASS_THROUGH_DIRECT();
            s.Length = (ushort)sz;
            s.CdbLength = 16;
            s.SenseInfoLength = SENSE_LEN;
            s.DataIn = 0;
            s.DataTransferLength = 0;
            s.TimeOutValue = 5;
            s.DataBuffer = IntPtr.Zero;
            s.SenseInfoOffset = (uint)sz;
            s.Cdb = new byte[16];
            Array.Copy(cdb, s.Cdb, 16);
            Marshal.StructureToPtr(s, buf, false);
            uint ret = 0;
            bool ok = DeviceIoControl(dev, IOCTL_SCSI_PASS_THROUGH_DIRECT, buf, (uint)(sz + SENSE_LEN),
                                      buf, (uint)(sz + SENSE_LEN), out ret, IntPtr.Zero);
            if (!ok)
            {
                sendErr++;
                CloseHandle(dev); dev = IntPtr.Zero;   // reopen on next event
                return;
            }
            var o = (SCSI_PASS_THROUGH_DIRECT)Marshal.PtrToStructure(buf, typeof(SCSI_PASS_THROUGH_DIRECT));
            if (o.ScsiStatus != 0) sendErr++; else sendOk++;
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    static byte[] pkt = new byte[12];

    // ---- 发送队列 ----------------------------------------------------------
    // 低级钩子有 LowLevelHooksTimeout（默认 300ms）：在钩子里直接做 SPTI 会超时，
    // Windows 会**静默摘除钩子**（表现为"用一会儿就再也不转发"）。
    // 因此钩子只做入队（微秒级），真正的 SCSI 发送交给工作线程。
    const int RING = 512;
    static readonly byte[] ring = new byte[RING * 13];   // 1 字节子命令 + 12 字节负载
    static int rHead = 0, rTail = 0;
    static readonly object rLock = new object();
    static long dropped = 0;

    static void Enqueue(int type, byte[] p)
    {
        lock (rLock)
        {
            int n = (rHead + 1) % RING;
            if (n == rTail) { dropped++; return; }
            int off = rHead * 13;
            ring[off] = (byte)(type == 1 ? 0x33 : type == 2 ? 0x34 : 0x36);
            Array.Copy(p, 0, ring, off + 1, 12);
            rHead = n;
        }
    }

    static void Worker()
    {
        byte[] cdb = new byte[16];
        for (;;)
        {
            /* 钩子健康看门狗：**正在转发（REMOTE）**时每 30 秒重装一次低级钩子。
               代价可忽略，但能自愈"钩子被系统静默摘除 → 回程令牌收不到"这类故障。
               只在 REMOTE 做：LOCAL 时钩子即使掉了也不影响回程（本来就在本机）。 */
            if (remote && unchecked(Environment.TickCount - lastHookReinstall) > 30000)
            {
                lastHookReinstall = Environment.TickCount;
                /* **不能**在本线程装钩子：低级钩子只在安装线程的消息泵里派发，
                   本线程不泵消息 → 装完即哑（键盘转发/F24 令牌全废）。
                   投一条消息给主线程，让它去装。 */
                if (rawWnd != IntPtr.Zero)
                    PostMessageW(rawWnd, WM_APP_REINSTALL, IntPtr.Zero, IntPtr.Zero);
                else
                    ReinstallHooks("watchdog (no hwnd!)");
            }
            int avail;
            lock (rLock) { avail = (rHead - rTail + RING) % RING; }
            if (avail == 0) { Thread.Sleep(1); continue; }
            for (int i = 0; i < avail; i++)
            {
                int off;
                lock (rLock)
                {
                    if (rHead == rTail) break;
                    off = rTail * 13;
                    rTail = (rTail + 1) % RING;
                }
                cdb[0] = 0xD9;
                cdb[1] = ring[off];
                Array.Copy(ring, off + 1, cdb, 2, 12);
                cdb[14] = (byte)'O';
                cdb[15] = (byte)'T';
                SendCdb(cdb);
            }
        }
    }

    static void SendHid(int type, byte[] p) { Enqueue(type, p); }

    // ------------------------------------------------------- packet builders
    static int lastButtons = 0;
    static byte[] kbdMods = new byte[1];
    static byte[] kbdKeys = new byte[6];

    static void SendMouse(int buttons, int dx, int dy, int wheel)
    {
        // split into +-127 chunks (descriptor: 1 signed byte per axis)
        do
        {
            int cx = Clamp(dx), cy = Clamp(dy), cw = Clamp(wheel);
            pkt[0] = (byte)(buttons & 0x1f);
            pkt[1] = (byte)(sbyte)cx;
            pkt[2] = (byte)(sbyte)cy;
            pkt[3] = (byte)(sbyte)cw;
            for (int i = 4; i < 12; i++) pkt[i] = 0;
            SendHid(1, pkt);
            dx -= cx; dy -= cy; wheel -= cw;
        } while (dx != 0 || dy != 0 || wheel != 0);
    }

    static int Clamp(int v) { return v > 127 ? 127 : (v < -127 ? -127 : v); }

    static void SendKeyboard()
    {
        pkt[0] = kbdMods[0];
        pkt[1] = 0;
        for (int i = 0; i < 6; i++) pkt[2 + i] = kbdKeys[i];
        for (int i = 8; i < 12; i++) pkt[i] = 0;
        SendHid(2, pkt);
    }

    static bool KeyInReport(byte usage)
    {
        for (int i = 0; i < 6; i++) if (kbdKeys[i] == usage) return true;
        return false;
    }

    static void ApplyKey(byte usage, byte modBit, bool down)
    {
        if (modBit != 0)
        {
            if (down) kbdMods[0] |= modBit; else kbdMods[0] = (byte)(kbdMods[0] & ~modBit);
            SendKeyboard();
            return;
        }
        if (down)
        {
            if (KeyInReport(usage)) return;
            for (int i = 0; i < 6; i++) if (kbdKeys[i] == 0) { kbdKeys[i] = usage; SendKeyboard(); return; }
            return;                                  // rollover full
        }
        else
        {
            bool ch = false;
            for (int i = 0; i < 6; i++) if (kbdKeys[i] == usage) { kbdKeys[i] = 0; ch = true; }
            if (ch) SendKeyboard();
        }
    }

    static void ReleaseAll()
    {
        kbdMods[0] = 0;
        for (int i = 0; i < 6; i++) kbdKeys[i] = 0;
        SendKeyboard();
        SendMouse(0, 0, 0, 0);
        lastButtons = 0;
    }

    // ------------------------------------------------------------- VK tables
    // Windows VK -> USB HID usage id (keyboard page 0x07)
    static int VkToUsage(int vk)
    {
        if (vk >= 0x41 && vk <= 0x5A) return vk - 0x41 + 0x04;              // A-Z
        if (vk >= 0x31 && vk <= 0x39) return vk - 0x31 + 0x1E;              // 1-9
        if (vk >= 0x61 && vk <= 0x69) return vk - 0x61 + 0x59;              // numpad 1-9
        if (vk >= 0x70 && vk <= 0x7B) return vk - 0x70 + 0x3A;              // F1-F12
        switch (vk)
        {
            case 0x30: return 0x27;   // 0
            case 0x0D: return 0x28;   // Enter
            case 0x1B: return 0x29;   // Esc
            case 0x08: return 0x2A;   // Backspace
            case 0x09: return 0x2B;   // Tab
            case 0x20: return 0x2C;   // Space
            case 0xBD: return 0x2D;   // OEM_MINUS  -_
            case 0xBB: return 0x2E;   // OEM_PLUS   =+
            case 0xDB: return 0x2F;   // OEM_4      [{
            case 0xDD: return 0x30;   // OEM_6      ]}
            case 0xDC: return 0x31;   // OEM_5      \|
            case 0xBA: return 0x33;   // OEM_1      ;:
            case 0xDE: return 0x34;   // OEM_7      '"
            case 0xC0: return 0x35;   // OEM_3      `~
            case 0xBC: return 0x36;   // OEM_COMMA  ,<
            case 0xBE: return 0x37;   // OEM_PERIOD .>
            case 0xBF: return 0x38;   // OEM_2      /?
            case 0x14: return 0x39;   // CapsLock
            case 0x2C: return 0x46;   // PrintScreen
            case 0x91: return 0x47;   // ScrollLock
            case 0x13: return 0x48;   // Pause
            case 0x2D: return 0x49;   // Insert
            case 0x24: return 0x4A;   // Home
            case 0x21: return 0x4B;   // PageUp
            case 0x2E: return 0x4C;   // Delete
            case 0x23: return 0x4D;   // End
            case 0x22: return 0x4E;   // PageDown
            case 0x27: return 0x4F;   // Right
            case 0x25: return 0x50;   // Left
            case 0x28: return 0x51;   // Down
            case 0x26: return 0x52;   // Up
            case 0x90: return 0x53;   // NumLock
            case 0x6F: return 0x54;   // Divide
            case 0x6A: return 0x55;   // Multiply
            case 0x6D: return 0x56;   // Subtract
            case 0x6B: return 0x57;   // Add
            case 0x60: return 0x62;   // Numpad 0
            case 0x6E: return 0x63;   // Decimal
            case 0xE2: return 0x64;   // OEM_102
            case 0x5D: return 0x65;   // Apps
            default: return 0;
        }
    }

    static byte VkToModBit(int vk)
    {
        switch (vk)
        {
            case 0xA2: return 0x01;   // LControl
            case 0xA0: return 0x02;   // LShift
            case 0xA4: return 0x04;   // LAlt (Menu)
            case 0x5B: return 0x08;   // LWin
            case 0xA3: return 0x10;   // RControl
            case 0xA1: return 0x20;   // RShift
            case 0xA5: return 0x40;   // RAlt
            case 0x5C: return 0x80;   // RWin
            default: return 0;
        }
    }

    // ---------------- 原始输入：识别"这个鼠标事件是不是线缆那头来的" ----------------
    // 为什么必须识别：Linux 驱动 Windows 光标时，光标会被推到屏幕边缘。
    // 如果"裸推边缘"就交接，会被误判成用户想切回来 —— 两端互相抢控制权。
    // 以前的办法是要求按 Ctrl，但用户左 Ctrl 有硬件问题；改判"事件来源"更干净：
    // 来自线缆自带 HID 鼠标的事件一律不触发交接。
    const int WM_INPUT = 0x00FF;
    /* 自定义消息：请求**主线程**重装低级钩子。
       低级钩子只在"安装它的那个线程"的消息泵里派发。看门狗原先直接在 Worker
       线程里 Unhook+SetWindowsHookEx，钩子就挂到了一个**不泵消息**的线程上 ——
       整套钩子从此哑掉（键盘转发失灵、F24 回程令牌收不到 = 用户说的"回不来"），
       而鼠标移动走 Raw Input 所以看起来"还好好的"。必须回主线程装。 */
    const int WM_APP_REINSTALL = 0x8001;
    static IntPtr rawWnd = IntPtr.Zero;
    const uint RIDEV_INPUTSINK = 0x00000100;
    const uint RID_INPUT = 0x10000003;
    const uint RIDI_DEVICENAME = 0x20000007;
    const uint RAWINPUTHEADER_SIZE = 24;      // x64
    const int CABLE_MOUSE_QUIET_MS = 1500;

    [StructLayout(LayoutKind.Sequential)]
    struct RAWINPUTDEVICE { public ushort usUsagePage; public ushort usUsage; public uint dwFlags; public IntPtr hwndTarget; }
    /* RAWMOUSE（x64，紧跟在 24 字节 RAWINPUTHEADER 之后）：
       usFlags / usButtonFlags / usButtonData / ulRawButtons / lLastX / lLastY / ulExtraInformation
       关键：lLastX/lLastY 是**设备级相对位移**，不受屏幕边缘夹取影响 ——
       这正是我们要的：不需要把光标"回中"，也就绕开了"回中在低级钩子里不生效、
       光标卡在边缘只剩 ±1px 抖动"这一整类问题。 */
    [StructLayout(LayoutKind.Sequential)]
    struct RAWMOUSE {
        public ushort usFlags;
        public ushort usButtonFlags;
        public ushort usButtonData;
        public uint   ulRawButtons;
        public int    lLastX;
        public int    lLastY;
        public uint   ulExtraInformation;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct WNDCLASS
    {
        public uint style; public IntPtr lpfnWndProc; public int cbClsExtra; public int cbWndExtra;
        public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszMenuName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
    }
    delegate IntPtr WndProcD(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    /* 必须 CharSet.Unicode：CreateWindowExW 找的是 Unicode 窗口类，
       用默认的 ANSI 声明注册会"注册成功但窗口类名对不上"，CreateWindowExW 失败，
       于是原始输入注册静默无效（实测表现为 WM_INPUT 从来没到过）。 */
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern ushort RegisterClassW(ref WNDCLASS wc);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y, int w, int h,
                                         IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] d, uint num, uint size);
    [DllImport("user32.dll")] static extern uint GetRawInputData(IntPtr hRawInput, uint cmd, IntPtr data, ref uint size, uint headerSize);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetRawInputDeviceInfoW(IntPtr hDevice, uint cmd, System.Text.StringBuilder data, ref uint size);

    static int lastCableMs = 0;
    static WndProcD wndProc;

    static bool CableMouseActive()
    {
        return lastCableMs != 0 && unchecked(Environment.TickCount - lastCableMs) < CABLE_MOUSE_QUIET_MS;
    }

    static IntPtr MyWndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        if (msg == WM_APP_REINSTALL)          /* 看门狗请求：在**本线程**（消息泵）重装钩子 */
        {
            ReinstallHooks("watchdog/REMOTE-30s (main thread)");
            return IntPtr.Zero;
        }
        if (msg == WM_INPUT)
        {
            uint size = 0;
            GetRawInputData(lParam, RID_INPUT, IntPtr.Zero, ref size, RAWINPUTHEADER_SIZE);
            if (size > 0)
            {
                IntPtr buf = Marshal.AllocHGlobal((int)size);
                try
                {
                    uint got = GetRawInputData(lParam, RID_INPUT, buf, ref size, RAWINPUTHEADER_SIZE);
                    if (got == size)
                    {
                        IntPtr hDev = Marshal.ReadIntPtr(buf, 8);      // RAWINPUTHEADER.hDevice
                        var sb = new System.Text.StringBuilder(512);
                        uint sz = 512;
                        string devName = "";
                        if (GetRawInputDeviceInfoW(hDev, RIDI_DEVICENAME, sb, ref sz) > 0)
                            devName = sb.ToString();
                        bool isCable = devName.IndexOf("VID_0EA0", StringComparison.OrdinalIgnoreCase) >= 0;
                        if (verbose && (nRawLog < 12 || (((RAWMOUSE)Marshal.PtrToStructure(IntPtr.Add(buf, 24), typeof(RAWMOUSE))).usButtonFlags != 0 && nBtnLog < 30)))
                        {
                            nRawLog++;
                            if (((RAWMOUSE)Marshal.PtrToStructure(IntPtr.Add(buf, 24), typeof(RAWMOUSE))).usButtonFlags != 0) nBtnLog++;
                            var rm0 = (RAWMOUSE)Marshal.PtrToStructure(IntPtr.Add(buf, 24), typeof(RAWMOUSE));
                            Console.WriteLine("raw dev=" + (isCable ? "CABLE" : devName.Substring(Math.Max(0, devName.Length - 40))) +
                                              " remote=" + remote + " flags=0x" + rm0.usFlags.ToString("x") +
                                              " dx=" + rm0.lLastX + " dy=" + rm0.lLastY + " btn=0x" + rm0.usButtonFlags.ToString("x"));
                        }
                        if (isCable)
                        {
                            /* 对端（线缆自带 HID）在驱动本机 —— 记下来，避免被误判成"用户想交接" */
                            lastCableMs = Environment.TickCount;
                        }
                        else if (remote)
                        {
                            /* REMOTE：只搬**设备级原始位移**。钩子已经把事件吞掉了，
                               所以本机光标不动；也就不存在撞边、夹取、抖动的任何问题。 */
                            var rm = (RAWMOUSE)Marshal.PtrToStructure(IntPtr.Add(buf, 24), typeof(RAWMOUSE));
                            bool absPos = (rm.usFlags & 0x01) != 0;   // MOUSE_MOVE_ABSOLUTE
                            if (!absPos && (rm.lLastX != 0 || rm.lLastY != 0))
                            {
                                SendMouse(lastButtons, rm.lLastX, rm.lLastY, 0); nMouse++;
                            }
                            /* 按键与滚轮**不在这里处理**：实测合成点击不产生 RAWMOUSE 的
                               usButtonFlags（移动会），而且真实点击也可能被"钩子吞掉"影响。
                               钩子对按键是 100% 可靠的，所以按键/滚轮统一走低级钩子（见 MouseHook）。 */
                        }
                    }
                }
                finally { Marshal.FreeHGlobal(buf); }
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    static void InitRawInput()
    {
        wndProc = MyWndProc;
        var wc = new WNDCLASS();
        wc.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(wndProc);
        wc.lpszClassName = "otiagent2raw";
        wc.hInstance = GetModuleHandle(null);
        ushort atom = RegisterClassW(ref wc);
        if (atom == 0 && Marshal.GetLastWin32Error() != 1410)   // 1410 = 类已存在
            Console.WriteLine("RegisterClassW failed err=" + Marshal.GetLastWin32Error());
        IntPtr hwnd = CreateWindowExW(0, "otiagent2raw", "otiagent2", 0, 0, 0, 0, 0,
                                      new IntPtr(-3) /* HWND_MESSAGE */, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        if (hwnd == IntPtr.Zero)
        {
            Console.WriteLine("CreateWindowExW failed err=" + Marshal.GetLastWin32Error() + " -> 原始输入不可用");
            return;
        }
        var rid = new RAWINPUTDEVICE[1];
        rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;
        rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwnd;
        rawWnd = hwnd;                        /* 看门狗靠它把"重装钩子"投回主线程 */
        if (!RegisterRawInputDevices(rid, 1, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE))))
            Console.WriteLine("RegisterRawInputDevices failed err=" + Marshal.GetLastWin32Error());
        else
            Console.WriteLine("原始输入已注册（鼠标，RIDEV_INPUTSINK）hwnd=0x" + hwnd.ToInt64().ToString("x"));
    }

    // ---------------------------------------------------------------- hooks
    const int WH_KEYBOARD_LL = 13, WH_MOUSE_LL = 14;
    const int WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
              WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
              WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208,
              WM_MOUSEWHEEL = 0x020A, WM_MOUSEHWHEEL = 0x020E,
              WM_XBUTTONDOWN = 0x020B, WM_XBUTTONUP = 0x020C;
    const int WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105;

    [StructLayout(LayoutKind.Sequential)]
    struct POINT { public int x; public int y; }
    [StructLayout(LayoutKind.Sequential)]
    struct MSLLHOOKSTRUCT
    {
        public POINT pt; public uint mouseData; public uint flags; public uint time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct KBDLLHOOKSTRUCT
    {
        public uint vkCode; public uint scanCode; public uint flags; public uint time; public IntPtr dwExtraInfo;
    }

    delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr SetWindowsHookEx(int id, HookProc fn, IntPtr mod, uint tid);
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool UnhookWindowsHookEx(IntPtr h);
    [DllImport("user32.dll")]
    static extern IntPtr CallNextHookEx(IntPtr h, int nCode, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")]
    static extern int GetMessage(out MSG m, IntPtr h, uint a, uint b);
    /* 必须 DispatchMessage：低级钩子是系统在取消息时回调的，所以只 GetMessage 也能工作；
       但 WM_INPUT（原始输入）是**窗口消息**，不 dispatch 就永远送不到窗口过程 ——
       实测表现为"原始输入注册成功，却一个 WM_INPUT 都收不到"。 */
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref MSG m);
    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public POINT pt; }
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern int ShowCursor(bool show);
    // 必须 DPI-aware：否则 GetSystemMetrics/SetCursorPos 用"逻辑坐标"，
    // 而低级钩子报的是"物理坐标"，两者混用会产生巨大的假位移（实测 ±127 洪流）。
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte sc, uint f, IntPtr e);
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);

    /*
     * 卡键自愈：实测遇到过两次"某个按键/鼠标键卡在按下状态"（键盘右 Ctrl、鼠标中键），
     * 症状是"Windows 点什么都没反应 / 键盘像失灵"。裸发一个 key-up / HID 全零报告**清不掉**，
     * 必须补一次完整的"按下-抬起"循环，状态机才会复位。所以在每次交还控制权时做一遍。
     */
    static void HealStuckInput()
    {
        // 键盘修饰键：合成一次 down+up 循环
        int[] mods = { 0xA2, 0xA3, 0xA0, 0xA1, 0xA4, 0xA5, 0x5B, 0x5C };
        foreach (int vk in mods)
        {
            if ((GetAsyncKeyState(vk) & 0x8000) != 0)
            {
                keybd_event((byte)vk, 0, 0, IntPtr.Zero);
                Thread.Sleep(15);
                keybd_event((byte)vk, 0, 2, IntPtr.Zero);
            }
        }
        // 鼠标键：down+up 循环（up 单独发无效，实测）
        uint[][] btns = new uint[][] {
            new uint[]{0x0002,0x0004},   // 左
            new uint[]{0x0008,0x0010},   // 右
            new uint[]{0x0020,0x0040},   // 中
            new uint[]{0x0080,0x0100},   // 侧1
            new uint[]{0x0200,0x0400},   // 侧2
        };
        int[] vks = { 0x01, 0x02, 0x04, 0x05, 0x06 };
        for (int i = 0; i < btns.Length; i++)
        {
            if ((GetAsyncKeyState(vks[i]) & 0x8000) != 0)
            {
                mouse_event(btns[i][0], 0, 0, 0, IntPtr.Zero);
                Thread.Sleep(15);
                mouse_event(btns[i][1], 0, 0, 0, IntPtr.Zero);
            }
        }
    }
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

    static HookProc mouseProc, kbdProc;      // keep alive
    static volatile bool remote = false;
    /* 构建标记：csc 覆盖正在运行的 exe 会**静默失败**（RUNBOOK 记过），
       日志里带上它才能确认"现在跑的到底是哪一版"。改代码时同步改这里。 */
    const string BuildTag = "mainhook-3";
    static int edge = 0;                     // 0=left 1=right 2=top 3=bottom
    static uint hkMods = 0x0002 | 0x0001;    // MOD_CONTROL | MOD_ALT
    static int hkVk = 0x27;                  // VK_RIGHT
    /* 钩子句柄 + 重装计数：Windows 在回调超时会**静默摘除**低级钩子
       （LowLevelHooksTimeout）。摘掉之后 F24 回程令牌永远收不到 ——
       用户看到的就是"鼠标移到对端就回不来了"。定期重装是最省事的自愈手段。 */
    static IntPtr hMouse = IntPtr.Zero, hKbd = IntPtr.Zero;
    static int lastHookReinstall = 0;
    static int nHookReinstall = 0;
    static bool verbose = false;
    static bool noCenter = false;
    // 边缘交接是否需要按修饰键（默认 Ctrl）。
    // 为什么需要：Linux 正在驱动 Windows 光标时，光标可能被推到屏幕边缘；
    // 若"裸推边缘"就交接，会被误判成用户想切回来，造成两端互相抢控制权。
    // Ctrl 在纯鼠标移动时不会出现，所以"按住 Ctrl 往边缘推"是一个无歧义的意图。
    static int edgeMod = 0;          // 0 = 不需要修饰键（纯边缘检测，用户要求）
    static int lastX = 0, lastY = 0, firstMove = 1;
    static int ignoreMoves = 0;      // 回中之后忽略紧随的若干次移动（避免 ±1px 乒乓风暴）
    static int edgeSince = 0;        // 持续向边缘推的起始时刻（防误触）
    static int nMouse = 0, nKey = 0, nSkip = 0, nRawLog = 0, nHookLog = 0, nBtnLog = 0;

    static int ScreenW() { return GetSystemMetrics(0); }
    static int ScreenH() { return GetSystemMetrics(1); }
    /* 交接判据必须用**整个虚拟桌面**的边界，不能用主屏的 0/宽。
       踩过的坑：用户有左显示器时 "x <= 0" 表示"光标落在左显示器上"，
       于是随便动一下就被当成撞边（实测日志 pt=(-929,625) 误判）。 */
    static int VLeft() { return GetSystemMetrics(76); }   // SM_XVIRTUALSCREEN
    static int VTop()  { return GetSystemMetrics(77); }   // SM_YVIRTUALSCREEN
    static int VW()    { return GetSystemMetrics(78); }   // SM_CXVIRTUALSCREEN
    static int VH()    { return GetSystemMetrics(79); }   // SM_CYVIRTUALSCREEN

    static void Log(string s)
    {
        if (verbose) Console.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + "  " + s);
    }

    static int CenterX() { return VLeft() + VW() / 2; }
    static int CenterY() { return VTop() + VH() / 2; }

    // 把光标钉在屏幕中心：这样它永远不会撞到屏幕边缘而"卡住不再产生位移"。
    // 回中本身会产生一个 WM_MOUSEMOVE，但它的 pt == center → 位移为 0 → 不会重复转发，
    // 所以这个自洽的循环是安全的。
    // 只在贴近屏幕边缘时回中一次：光标永远不会撞边而"卡住不再产生位移"。
    // 回中产生的那次 WM_MOUSEMOVE 的 pt 恰好等于新的 lastX/lastY → 位移为 0 → 不会重复转发，
    // 因此不会形成反馈风暴（这是经典实现，实测比"每次回中"稳定得多）。
    // 漂移超过阈值才回中（阈值太小会和系统取整误差形成 ±1px 乒乓风暴）。
    // 回中后的 2 次移动直接忽略，避免把"跳回中心"当成真实位移转发出去。
    static void RecenterIfDrifted(int x, int y)
    {
        const int LIMIT = 220;
        if (Math.Abs(x - CenterX()) > LIMIT || Math.Abs(y - CenterY()) > LIMIT)
        {
            int cx = CenterX(), cy = CenterY();
            SetCursorPos(cx, cy);
            lastX = cx; lastY = cy;
            ignoreMoves = 2;
        }
    }

    static int cursorHidden = 0;

    static void EnterRemote(string why)
    {
        remote = true;
        ReleaseAll();
        POINT p; GetCursorPos(out p);
        lastX = p.x; lastY = p.y;
        ignoreMoves = 0;
        if (!noCenter) { ShowCursor(false); cursorHidden = 1; }
        Console.WriteLine("[{0:HH:mm:ss}] REMOTE  (" + why + ")  -- input is forwarded to the peer",
                          DateTime.Now);
    }

    static void EnterLocal(string why)
    {
        ReleaseAll();
        remote = false;
        firstMove = 1;
        if (cursorHidden == 1) { ShowCursor(true); cursorHidden = 0; }
        try { HealStuckInput(); } catch { }
        Console.WriteLine("[{0:HH:mm:ss}] LOCAL   (" + why + ")  -- Windows keeps its own input",
                          DateTime.Now);
    }

    static void ReinstallHooks(string why)
    {
        try
        {
            if (hKbd != IntPtr.Zero) UnhookWindowsHookEx(hKbd);
            if (hMouse != IntPtr.Zero) UnhookWindowsHookEx(hMouse);
            hMouse = SetWindowsHookEx(WH_MOUSE_LL, mouseProc, GetModuleHandle(null), 0);
            hKbd = SetWindowsHookEx(WH_KEYBOARD_LL, kbdProc, GetModuleHandle(null), 0);
            nHookReinstall++;
            Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss") + "] hooks reinstalled #" + nHookReinstall +
                              " (" + why + ") mouse=" + (hMouse != IntPtr.Zero) + " kbd=" + (hKbd != IntPtr.Zero) +
                              " tid=" + GetCurrentThreadId());
        }
        catch (Exception e) { Console.WriteLine("hook reinstall: " + e.Message); }
    }

    static bool HotkeyHeld()
    {
        bool ctrl = (GetAsyncKeyState(0xA2) & 0x8000) != 0 || (GetAsyncKeyState(0xA3) & 0x8000) != 0;
        bool alt = (GetAsyncKeyState(0xA4) & 0x8000) != 0 || (GetAsyncKeyState(0xA5) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(0xA0) & 0x8000) != 0 || (GetAsyncKeyState(0xA1) & 0x8000) != 0;
        bool win = (GetAsyncKeyState(0x5B) & 0x8000) != 0 || (GetAsyncKeyState(0x5C) & 0x8000) != 0;
        uint m = 0;
        if (alt) m |= 0x0001;
        if (ctrl) m |= 0x0002;
        if (shift) m |= 0x0004;
        if (win) m |= 0x0008;
        return m == hkMods;
    }

    static IntPtr MouseHook(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode < 0) return CallNextHookEx(IntPtr.Zero, nCode, wParam, lParam);
        var ms = (MSLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(MSLLHOOKSTRUCT));
        int msg = (int)wParam;

        if (!remote)
        {
            if (verbose && nHookLog < 25 && msg == WM_MOUSEMOVE)
            {
                nHookLog++;
                Console.WriteLine("hook pt=(" + ms.pt.x + "," + ms.pt.y + ") VRight=" + (VLeft() + VW() - 2) +
                                  " VLeft=" + VLeft() + " cable=" + CableMouseActive());
            }
            // hand-over edge detection
            bool atEdge =
                (edge == 0 && ms.pt.x <= VLeft() + 1) ||
                (edge == 1 && ms.pt.x >= VLeft() + VW() - 2) ||
                (edge == 2 && ms.pt.y <= VTop() + 1) ||
                (edge == 3 && ms.pt.y >= VTop() + VH() - 2);
            if (atEdge && msg == WM_MOUSEMOVE && !CableMouseActive())
            {
                // only hand over when the user keeps pushing outward
                int dx = ms.pt.x - lastX, dy = ms.pt.y - lastY;
                bool outward = (edge == 0 && dx < 0) || (edge == 1 && dx > 0) ||
                               (edge == 2 && dy < 0) || (edge == 3 && dy > 0);
                /* 交接手势：把光标推到边缘并**停在那里** >=200ms。
                   这里有个必须踩过的坑：光标一旦被屏幕边缘夹住就**不再移动**，
                   WM_MOUSEMOVE 依旧会来，但 pt 不变 → dx==0。
                   早期版本把"dx==0"当成"用户没在推"从而清零计时器，
                   于是"推到边缘一停"永远攒不满时间 —— 用户看到的就是**根本移不过去**。
                   正确语义：
                     * 在边缘出现过"向外推"(outward) → 开始计时；
                     * 之后 dx==0（被夹住、仍按着往那边推）→ **继续计时**；
                     * 向内移动(inward) → 清零。 */
                bool modOk = (edgeMod == 0) ||
                             (GetAsyncKeyState(edgeMod) & 0x8000) != 0 ||
                             (GetAsyncKeyState(0xA3) & 0x8000) != 0;
                /* 抖动容差：光标被屏幕边缘夹住时会在 +/-1px 抖动
                   （实测日志 pt=(-1941,500) dx=-1 与 pt=(-1940,500) dx=+1 交替）。
                   若把 +1px 当成"向内移动"，计时器被无限清零 ——
                   这是"推到边缘了却永远移不过去"的最后一个坑。 */
                const int JIT = 3;
                bool inward =
                    (edge == 0 && dx >  JIT) || (edge == 1 && dx < -JIT) ||
                    (edge == 2 && dy >  JIT) || (edge == 3 && dy < -JIT);
                if (verbose && (atEdge || edgeSince != 0))
                    Console.WriteLine("edge pt=(" + ms.pt.x + "," + ms.pt.y + ") dx=" + dx + " out=" + outward +
                                      " in=" + inward + " since=" + edgeSince + " cable=" + CableMouseActive());
                if (inward) edgeSince = 0;
                else if (edgeSince != 0 && modOk && firstMove == 0 &&
                         unchecked(Environment.TickCount - edgeSince) >= 200)
                {
                    edgeSince = 0;
                    EnterRemote("cursor pushed out of the hand-over edge");
                }
                /* 开始计时用"只在未开始时启动"，**不能**每次 outward 都重新赋值：
                   边缘抖动每隔一个事件就产生一次 outward，赋值会把计时器无限重启
                   （实测日志里 elapsed 永远停在 ~93ms）。 */
                else if (outward && edgeSince == 0) edgeSince = Environment.TickCount;
            }
            lastX = ms.pt.x; lastY = ms.pt.y; firstMove = 0;
            return CallNextHookEx(IntPtr.Zero, nCode, wParam, lParam);
        }

        // ---- REMOTE: swallow everything, forward relative motion ----
        switch (msg)
        {
            /* 按键与滚轮在钩子里转发（可靠）；移动交给原始输入（设备级位移，不受边缘夹取）。
               两者都置 lastButtons，发出的包里按键位始终是最新的。 */
            case WM_LBUTTONDOWN: lastButtons |= 0x01; SendMouse(lastButtons, 0, 0, 0); break;
            case WM_LBUTTONUP:   lastButtons &= ~0x01; SendMouse(lastButtons, 0, 0, 0); break;
            case WM_RBUTTONDOWN: lastButtons |= 0x02; SendMouse(lastButtons, 0, 0, 0); break;
            case WM_RBUTTONUP:   lastButtons &= ~0x02; SendMouse(lastButtons, 0, 0, 0); break;
            case WM_MBUTTONDOWN: lastButtons |= 0x04; SendMouse(lastButtons, 0, 0, 0); break;
            case WM_MBUTTONUP:   lastButtons &= ~0x04; SendMouse(lastButtons, 0, 0, 0); break;
            case WM_XBUTTONDOWN:
                lastButtons |= ((ms.mouseData >> 16) == 1) ? 0x08 : 0x10;
                SendMouse(lastButtons, 0, 0, 0); break;
            case WM_XBUTTONUP:
                lastButtons &= ~(((ms.mouseData >> 16) == 1) ? 0x08 : 0x10);
                SendMouse(lastButtons, 0, 0, 0); break;
            case WM_MOUSEWHEEL:
                SendMouse(lastButtons, 0, 0, (short)((ms.mouseData >> 16) & 0xffff) / 120);
                break;
            default:
                break;
        }
        /* REMOTE：鼠标事件**全部吞掉**（含移动）。
           位移与按键改由原始输入(WM_INPUT)提供 —— 那是设备级数据，不受屏幕边缘夹取影响，
           所以本机光标可以完全不动，也不需要"回中"。
           早期版本在这里放行移动、靠 SetCursorPos 回中来避免撞边，实测回中在低级钩子里
           不生效，光标卡在边缘只剩 ±1px 抖动，转发出去的就是"乱抖"。 */
        return (IntPtr)1;   // swallow
    }

    static IntPtr KbdHook(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode < 0) return CallNextHookEx(IntPtr.Zero, nCode, wParam, lParam);
        var k = (KBDLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(KBDLLHOOKSTRUCT));
        int msg = (int)wParam;
        bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        int vk = (int)k.vkCode;

        /* F24 一律记一笔：回程令牌是否到达，是"指针回不来/键鼠失灵"这类故障的第一线索。
           其余按键只在 --verbose 下记（否则用户打字会刷爆日志）。 */
        if (vk == 0x87)
            Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss") + "] kbd: F24 token seen (mode=" +
                              (remote ? "REMOTE" : "LOCAL") + ", down=" + down + ")");
        else if (verbose)
            Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss") + "] kbd: vk=0x" +
                              vk.ToString("X2") + " down=" + down + " mode=" + (remote ? "REMOTE" : "LOCAL"));

        // return hotkey works in both modes and is always swallowed while remote
        if (!remote)
            return CallNextHookEx(IntPtr.Zero, nCode, wParam, lParam);

        /* 对端令牌：F24（usage 0x73）表示"我这边控制状态变了，你回到被动"。
           两端都靠边缘检测切换，被驱动的一侧无法凭自身判断对面已放手，
           没有这个令牌就会一直吞着键鼠 —— 用户实测就是"鼠标回不去、键盘像失灵"。 */
        if (vk == 0x87)                       // VK_F24
        {
            if (down) EnterLocal("peer token (F24)");
            return (IntPtr)1;
        }
        if (down && vk == hkVk && HotkeyHeld())
        {
            EnterLocal("return hotkey");
            return (IntPtr)1;
        }

        int usage = VkToUsage(vk);
        byte mod = VkToModBit(vk);
        if (usage != 0 || mod != 0) { ApplyKey((byte)usage, mod, down); nKey++; }
        return (IntPtr)1;   // swallow
    }

    static void Usage()
    {
        Console.WriteLine(@"otiagent2 -- Windows capture agent for the OTi transfer cable

  --device \\.\H:      SCSI device of the cable (default \\.\H:)
  --edge left|right|top|bottom     hand-over edge (default left)
  --hotkey-back <vk>   virtual key to return to LOCAL (default 0x27 = Right)
  --verbose            log every action
  --nocenter           do not pin the cursor to the screen centre while REMOTE
  --edge-modifier ctrl|shift|alt|none   modifier required for edge hand-over (default none)
  --status             print a status line every second
  --probe              send a short mouse+keyboard probe and exit (no hooks)
  --remote             start already in REMOTE mode (for testing)
");
    }

    static int Main(string[] args)
    {
        try { SetProcessDPIAware(); } catch { }
        bool probe = false, startRemote = false, status = false;
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i];
            if (a == "--device" && i + 1 < args.Length) devPath = args[++i];
            else if (a == "--edge" && i + 1 < args.Length)
            {
                string v = args[++i];
                edge = v == "right" ? 1 : v == "top" ? 2 : v == "bottom" ? 3 : 0;
            }
            else if (a == "--hotkey-back" && i + 1 < args.Length) hkVk = Convert.ToInt32(args[++i], 16);
            else if (a == "--verbose") verbose = true;
            else if (a == "--nocenter") noCenter = true;
            else if (a == "--edge-modifier" && i + 1 < args.Length)
            {
                string v = args[++i];
                edgeMod = v == "none" ? 0 : v == "shift" ? 0xA0 : v == "alt" ? 0xA4 : 0xA2;
            }
            else if (a == "--status") status = true;
            else if (a == "--probe") probe = true;
            else if (a == "--remote") startRemote = true;
            else { Usage(); return 2; }
        }

        if (!OpenDevice())
        {
            Console.WriteLine("cannot open " + devPath + " (err " + Marshal.GetLastWin32Error() + ") - trying auto-detect");
            string alt = FindCableDevice();
            if (alt != null) { devPath = alt; }
        }
        if (!OpenDevice())
        {
            Console.WriteLine("cannot open " + devPath + " - cable plugged in? (or run: otiagent2 --probe)");
            return 1;
        }

        if (probe)
        {
            var pt = new Thread(Worker); pt.IsBackground = true; pt.Start();
            Console.WriteLine("probe: mouse +40/-40 then one 'a'");
            SendMouse(0, 40, 0, 0); Thread.Sleep(250);
            SendMouse(0, -40, 0, 0); Thread.Sleep(250);
            kbdMods[0] = 0; kbdKeys[0] = 0x04; SendKeyboard(); Thread.Sleep(150);
            kbdKeys[0] = 0; SendKeyboard();
            Thread.Sleep(400);
            Console.WriteLine("probe done. sent ok=" + sendOk + " err=" + sendErr);
            return 0;
        }

        var wt = new Thread(Worker);
        wt.IsBackground = true;
        wt.Start();

        try { InitRawInput(); } catch (Exception e) { Console.WriteLine("rawinput init: " + e.Message); }

        mouseProc = MouseHook;
        kbdProc = KbdHook;
        hMouse = SetWindowsHookEx(WH_MOUSE_LL, mouseProc, GetModuleHandle(null), 0);
        hKbd = SetWindowsHookEx(WH_KEYBOARD_LL, kbdProc, GetModuleHandle(null), 0);
        if (hMouse == IntPtr.Zero || hKbd == IntPtr.Zero)
        {
            Console.WriteLine("SetWindowsHookEx failed: " + Marshal.GetLastWin32Error());
            return 1;
        }

        Console.WriteLine("otiagent2 up. device=" + devPath +
                          "  edge=" + (new string[] { "left", "right", "top", "bottom" })[edge] +
                          "  hotkey-back=Ctrl+Alt+VK" + hkVk.ToString("X2") +
                          "  [build " + BuildTag + "]");
        if (startRemote) EnterRemote("--remote"); else EnterLocal("startup");

        if (status)
        {
            var t = new Thread(delegate ()
            {
                for (;;)
                {
                    Thread.Sleep(1000);
                    int q;
                    lock (rLock) { q = (rHead - rTail + RING) % RING; }
                    Console.WriteLine(string.Format("[status] mode={0} mouse={1} key={2} tx_ok={3} tx_err={4} queued={5} dropped={6}",
                                      remote ? "REMOTE" : "LOCAL", nMouse, nKey, sendOk, sendErr, q, dropped));
                }
            });
            t.IsBackground = true; t.Start();
        }

        MSG msg;
        while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0)
        {
            TranslateMessage(ref msg);
            DispatchMessageW(ref msg);
        }
        UnhookWindowsHookEx(hMouse);
        UnhookWindowsHookEx(hKbd);
        if (dev != IntPtr.Zero) CloseHandle(dev);
        return 0;
    }
}
