/*
 * curstortrace.cs —— Windows 侧"光标冻结"采样器（免管理员；csc 编译，不写系统目录）
 *
 * 为什么需要它（2026-11-06 真机证据，见 /tmp/redesign-brief.md §1）：
 *   现状 Windows 剪贴板代理是 PowerShell 主循环，取帧一轮 1.3~9s；麒麟侧向它写 64KB 帧时
 *   在 SCSI 命令里阻塞 250~700ms。对拷线是**单队列**设备（/sys/block/sdb/device/queue_depth=1），
 *   HID 注入写和剪贴板帧共用一条管道 → HID 写被堵 100ms~2.5s，用户看到"鼠标突然冻住"。
 *   "卡顿"必须变成可回归的数字：本采样器用 1ms 精度读 GetCursorPos（**只读，不注入任何输入**），
 *   记录相邻两次**光标位置变化**的间隔；在合成 HID 持续驱动光标时，这个间隔 = 光标冻结时长。
 *
 * 编译（不需要管理员）：
 *   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize+ /target:exe ^
 *       /out:cursortrace.exe curstortrace.cs
 *
 * 用法：
 *   curstortrace.exe <采样毫秒> <csv 路径> [warmup_ms=800] [tail_ms=800]
 *
 *   - 只记录"位置变化"事件：csv 行 = t_ms,x,y,abs_ms（t_ms 相对本次启动；
 *     abs_ms = 1970 起绝对毫秒，来自 GetSystemTimePreciseAsFileTime，供跨机时间对表）。
 *   - warmup/tail 用于排除"采样先启动、驱动后开始"两头的**无输入空闲**（否则会被误算成冻结）。
 *   - 结束时 stdout 固定打印一行：
 *       MAXFREEZE=<ms> FREEZES_OVER_50=<n> MOVES=<n> WINDOW_MS=<warmup>..<end>
 *     并写同名 `<csv>.summary`（含 START_UTC/END_UTC，便于跨机对表）。
 *   - 若 MOVES 太少（<2），说明驱动/线缆/HID 没生效 → MAXFREEZE=-1（判定为"测量无效"，不是冻结）。
 *
 * 退出码：0 = 正常完成；1 = 参数/IO 错误。
 */
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class CursorTrace
{
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetCursorPos(out POINT p);

    [DllImport("winmm.dll", EntryPoint = "timeBeginPeriod")]
    public static extern uint TimeBeginPeriod(uint uPeriod);

    [DllImport("winmm.dll", EntryPoint = "timeEndPeriod")]
    public static extern uint TimeEndPeriod(uint uPeriod);

    [DllImport("kernel32.dll")]
    public static extern void GetSystemTimePreciseAsFileTime(out long ft);

    private const long FileTimeToUnixMs = 11644473600000L;   // 1601→1970 的毫秒差

    private static long AbsUnixMs()
    {
        long ft;
        GetSystemTimePreciseAsFileTime(out ft);
        return ft / 10000L - FileTimeToUnixMs;
    }

    private static int ArgInt(string[] args, int i, int dflt)
    {
        if (args.Length <= i) return dflt;
        int v;
        return int.TryParse(args[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out v) ? v : dflt;
    }

    public static int Main(string[] args)
    {
        int durationMs = ArgInt(args, 0, 17000);
        string csv = args.Length > 1 ? args[1] : Path.Combine(Path.GetTempPath(), "cursortrace.csv");
        int warmupMs = ArgInt(args, 2, 800);
        int tailMs = ArgInt(args, 3, 800);
        if (durationMs < 500) durationMs = 500;
        if (warmupMs < 0) warmupMs = 0;
        if (tailMs < 0) tailMs = 0;

        uint timerSet = TimeBeginPeriod(1);
        try
        {
            string dir = Path.GetDirectoryName(Path.GetFullPath(csv));
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                Directory.CreateDirectory(dir);

            // 采样：只在位置变化时记一行（1ms 轮询）；t_ms 用高精度 Stopwatch。
            var times = new List<long>(4096);   // 相对 ms
            var xs = new List<int>(4096);
            var ys = new List<int>(4096);
            var abs = new List<long>(4096);
            long startAbs = AbsUnixMs();
            string startUtc = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture);

            Stopwatch sw = Stopwatch.StartNew();
            POINT cur;
            GetCursorPos(out cur);
            int lx = cur.X, ly = cur.Y;
            var sb = new StringBuilder(1 << 20);
            sb.Append("# curstortrace: duration_ms=").Append(durationMs)
              .Append(" warmup_ms=").Append(warmupMs)
              .Append(" tail_ms=").Append(tailMs)
              .Append(" start_utc=").Append(startUtc).Append('\n');
            sb.Append("# t_ms,x,y,abs_ms\n");

            while (true)
            {
                long t = sw.ElapsedMilliseconds;
                if (t >= durationMs) break;
                if (GetCursorPos(out cur) && (cur.X != lx || cur.Y != ly))
                {
                    lx = cur.X; ly = cur.Y;
                    long a = AbsUnixMs();
                    times.Add(t); xs.Add(lx); ys.Add(ly); abs.Add(a);
                    sb.Append(t).Append(',').Append(lx).Append(',').Append(ly)
                      .Append(',').Append(a).Append('\n');
                    if (sb.Length > 1 << 20) { File.AppendAllText(csv + ".part", sb.ToString(), new UTF8Encoding(false)); sb.Length = 0; }
                }
                Thread.Sleep(1);   // timeBeginPeriod(1) 下约 1~1.5ms
            }
            sw.Stop();
            long endAbs = AbsUnixMs();

            int winStart = warmupMs;
            int winEnd = durationMs - tailMs;
            if (winEnd <= winStart) { winStart = 0; winEnd = durationMs; }
            long maxFreeze = 0;
            int over50 = 0, gaps = 0;
            for (int i = 1; i < times.Count; i++)
            {
                long g = times[i] - times[i - 1];
                if (times[i - 1] < winStart || times[i] > winEnd) continue;
                gaps++;
                if (g > maxFreeze) maxFreeze = g;
                if (g > 50) over50++;
            }
            bool valid = times.Count >= 2 && gaps > 0;
            string line = string.Format(CultureInfo.InvariantCulture,
                "MAXFREEZE={0} FREEZES_OVER_50={1} MOVES={2} WINDOW_MS={3}..{4} GAPS={5}",
                valid ? maxFreeze : -1, over50, times.Count, winStart, winEnd, gaps);

            sb.Append("# ").Append(line).Append('\n');
            sb.Append("# START_UTC=").Append(startUtc).Append('\n');
            sb.Append("# END_UTC=").Append(DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture)).Append('\n');
            File.AppendAllText(csv, sb.ToString(), new UTF8Encoding(false));
            try { if (File.Exists(csv + ".part")) { File.AppendAllText(csv, File.ReadAllText(csv + ".part"), new UTF8Encoding(false)); File.Delete(csv + ".part"); } } catch { }

            string summary = line + "\nSTART_UTC=" + startUtc
                + "\nSTART_ABS_MS=" + startAbs + "\nEND_ABS_MS=" + endAbs
                + "\nDURATION_MS=" + durationMs + "\nWARMUP_MS=" + warmupMs + "\nTAIL_MS=" + tailMs
                + "\nCSV=" + Path.GetFullPath(csv) + "\n";
            File.WriteAllText(csv + ".summary", summary, new UTF8Encoding(false));
            Console.WriteLine(line);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("cursortrace error: " + ex.Message);
            return 1;
        }
        finally
        {
            if (timerSet == 0) TimeEndPeriod(1);
        }
    }
}
