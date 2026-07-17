using System.Runtime.InteropServices;

namespace CKFlip3D.Settings.Interop;

public sealed record MonitorEntry(
    int Index,
    string DeviceName,
    int Left, int Top, int Right, int Bottom,
    bool IsPrimary)
{
    public int Width  => Right - Left;
    public int Height => Bottom - Top;
    public bool IsPortrait => Height > Width;

    public string AspectLabel
    {
        get
        {
            int longSide = Math.Max(Width, Height), shortSide = Math.Min(Width, Height);
            double ratio = (double)longSide / shortSide;

            // Snap to the common marketing names (3440x1440 is 43:18 exactly,
            // but everyone calls it 21:9).
            (double r, string name)[] known =
            {
                (5.0 / 4.0, "5:4"), (4.0 / 3.0, "4:3"), (3.0 / 2.0, "3:2"),
                (16.0 / 10.0, "16:10"), (16.0 / 9.0, "16:9"),
                (21.0 / 9.0, "21:9"), (32.0 / 9.0, "32:9"),
            };
            foreach (var (r, name) in known)
            {
                // ultrawides vary (2.33–2.40), give 21:9 a wider net
                double tol = name == "21:9" ? 0.08 : 0.02;
                if (Math.Abs(ratio - r) <= tol)
                    return IsPortrait ? Swap(name) : name;
            }

            static int Gcd(int a, int b) => b == 0 ? a : Gcd(b, a % b);
            int g = Gcd(Width, Height);
            int aw = Width / g, ah = Height / g;
            if (aw > 50 || ah > 50) return $"~{(double)Width / Height:0.00}:1";
            return $"{aw}:{ah}";

            static string Swap(string s)
            {
                var parts = s.Split(':');
                return $"{parts[1]}:{parts[0]}";
            }
        }
    }
}

/// <summary>Win32 monitor + taskbar enumeration used by Diagnostics and the monitor selector.</summary>
public static class MonitorInterop
{
    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MONITORINFOEX
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string szDevice;
    }

    private delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, ref RECT rect, IntPtr data);

    [DllImport("user32.dll")]
    private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc proc, IntPtr data);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetMonitorInfoW(IntPtr hMonitor, ref MONITORINFOEX info);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? className, string? windowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowExW(IntPtr parent, IntPtr childAfter, string? className, string? windowName);

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);

    private const uint MONITORINFOF_PRIMARY = 1;

    public static List<MonitorEntry> EnumerateMonitors()
    {
        var list = new List<MonitorEntry>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMon, IntPtr _, ref RECT _, IntPtr _) =>
        {
            var info = new MONITORINFOEX { cbSize = Marshal.SizeOf<MONITORINFOEX>() };
            if (GetMonitorInfoW(hMon, ref info))
            {
                list.Add(new MonitorEntry(
                    list.Count,
                    info.szDevice,
                    info.rcMonitor.Left, info.rcMonitor.Top,
                    info.rcMonitor.Right, info.rcMonitor.Bottom,
                    (info.dwFlags & MONITORINFOF_PRIMARY) != 0));
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static (int x, int y, int w, int h) GetVirtualScreenRect()
    {
        const int SM_XVIRTUALSCREEN = 76, SM_YVIRTUALSCREEN = 77,
                  SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79;
        return (GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    }

    /// <summary>Counts the primary ("Shell_TrayWnd") and secondary ("Shell_SecondaryTrayWnd") taskbars.</summary>
    public static (bool primaryFound, int secondaryCount) DetectTaskbars()
    {
        bool primary = FindWindowW("Shell_TrayWnd", null) != IntPtr.Zero;
        int secondary = 0;
        IntPtr h = IntPtr.Zero;
        while ((h = FindWindowExW(IntPtr.Zero, h, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero)
            secondary++;
        return (primary, secondary);
    }
}
