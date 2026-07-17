using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace CKFlip3D.Settings.Interop;

/// <summary>
/// Enables a DWM blur/acrylic backdrop behind the window via the undocumented
/// SetWindowCompositionAttribute API (works on Windows 10 and 11).
/// Falls back gracefully: acrylic -> blur-behind -> plain translucent gradient.
/// </summary>
public static class AeroGlass
{
    private enum AccentState
    {
        Disabled = 0,
        EnableBlurBehind = 3,
        EnableAcrylicBlurBehind = 4,
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct AccentPolicy
    {
        public AccentState AccentState;
        public uint AccentFlags;
        public uint GradientColor;   // AABBGGRR
        public uint AnimationId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WindowCompositionAttributeData
    {
        public int Attribute;        // 19 = WCA_ACCENT_POLICY
        public IntPtr Data;
        public int SizeOfData;
    }

    [DllImport("user32.dll")]
    private static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref WindowCompositionAttributeData data);

    private const int WCA_ACCENT_POLICY = 19;

    /// <summary>Apply an acrylic-like glass backdrop. <paramref name="tint"/> is blended by DWM.</summary>
    public static bool Enable(Window window, Color tint, byte tintAlpha)
    {
        var hwnd = new WindowInteropHelper(window).Handle;
        if (hwnd == IntPtr.Zero)
            return false;

        // DWM expects AABBGGRR.
        uint gradient = ((uint)tintAlpha << 24) | ((uint)tint.B << 16) | ((uint)tint.G << 8) | tint.R;

        if (Apply(hwnd, AccentState.EnableAcrylicBlurBehind, gradient))
            return true;
        return Apply(hwnd, AccentState.EnableBlurBehind, 0);
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    [StructLayout(LayoutKind.Sequential)]
    private struct MARGINS { public int Left, Right, Top, Bottom; }

    [DllImport("dwmapi.dll")]
    private static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref MARGINS margins);

    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
    private const int DWMSBT_TRANSIENTWINDOW = 3;   // acrylic

    /// <summary>
    /// Windows 11 22H2+ real acrylic: the system backdrop blurs whatever is
    /// behind the window (other apps, desktop) — same material as Windows
    /// Terminal's "acrylic". Requires a non-layered window (no
    /// AllowsTransparency) with the frame extended into the client area.
    /// Returns false on older builds so callers fall back to
    /// SetWindowCompositionAttribute.
    /// </summary>
    public static bool TryEnableSystemBackdrop(Window window, bool darkMode)
    {
        if (Environment.OSVersion.Version.Build < 22621)
            return false;

        var hwnd = new WindowInteropHelper(window).Handle;
        if (hwnd == IntPtr.Zero) return false;

        var margins = new MARGINS { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        if (DwmExtendFrameIntoClientArea(hwnd, ref margins) != 0)
            return false;

        SetDarkMode(window, darkMode);

        int backdrop = DWMSBT_TRANSIENTWINDOW;
        return DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                     ref backdrop, sizeof(int)) == 0;
    }

    /// <summary>Hints DWM whether the backdrop should use its dark or light material.</summary>
    public static void SetDarkMode(Window window, bool dark)
    {
        var hwnd = new WindowInteropHelper(window).Handle;
        if (hwnd == IntPtr.Zero) return;
        int v = dark ? 1 : 0;
        _ = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref v, sizeof(int));
    }

    /// <summary>
    /// Ask Windows 11 DWM to round the real window surface. This also clips
    /// the accent blur rectangle, so the glass matches our rounded frame.
    /// No-op on builds that don't support DWMWA_WINDOW_CORNER_PREFERENCE.
    /// </summary>
    public static void SetRoundedCorners(Window window, bool round)
    {
        var hwnd = new WindowInteropHelper(window).Handle;
        if (hwnd == IntPtr.Zero) return;

        const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        int pref = round ? 2 /* DWMWCP_ROUND */ : 1 /* DWMWCP_DONOTROUND */;
        _ = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref pref, sizeof(int));
    }

    private static bool Apply(IntPtr hwnd, AccentState state, uint gradient)
    {
        var accent = new AccentPolicy
        {
            AccentState = state,
            AccentFlags = 2, // draw all borders
            GradientColor = gradient,
        };

        int size = Marshal.SizeOf<AccentPolicy>();
        IntPtr ptr = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(accent, ptr, false);
            var data = new WindowCompositionAttributeData
            {
                Attribute = WCA_ACCENT_POLICY,
                Data = ptr,
                SizeOfData = size,
            };
            return SetWindowCompositionAttribute(hwnd, ref data) != 0;
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }
}
