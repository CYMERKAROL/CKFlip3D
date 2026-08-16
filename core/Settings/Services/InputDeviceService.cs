using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace CKFlip3D.Settings.Services;

/// <summary>One attached input device, as the raw-input stack sees it.</summary>
public sealed record InputDeviceEntry(
    string Kind,          // "Mouse" / "Keyboard" / "Precision touchpad" / "HID"
    string Name,          // friendly name where Windows offers one
    string Detail);       // buttons / layout / usage, whatever fits the kind

/// <summary>
/// Enumerates pointing devices and keyboards for the Diagnostics page.
///
/// Raw input is the right source here: it is the same list the core's
/// keyboard hook and touchpad worker see, so a device missing from this page
/// is a device CKFlip3D genuinely cannot use — which is exactly the question
/// someone reading diagnostics is trying to answer.
/// </summary>
public static class InputDeviceService
{
    private const int RIM_TYPEMOUSE = 0;
    private const int RIM_TYPEKEYBOARD = 1;
    private const int RIM_TYPEHID = 2;
    private const uint RIDI_DEVICEINFO = 0x2000000b;
    private const uint RIDI_DEVICENAME = 0x20000007;

    [StructLayout(LayoutKind.Sequential)]
    private struct RAWINPUTDEVICELIST { public IntPtr hDevice; public uint dwType; }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct RID_DEVICE_INFO_MOUSE
    {
        public uint dwId, dwNumberOfButtons, dwSampleRate;
        public int fHasHorizontalWheel;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct RID_DEVICE_INFO_KEYBOARD
    {
        public uint dwType, dwSubType, dwKeyboardMode;
        public uint dwNumberOfFunctionKeys, dwNumberOfIndicators, dwNumberOfKeysTotal;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct RID_DEVICE_INFO_HID
    {
        public uint dwVendorId, dwProductId, dwVersionNumber;
        public ushort usUsagePage, usUsage;
    }

    [DllImport("user32.dll")]
    private static extern uint GetRawInputDeviceList(
        [In, Out] RAWINPUTDEVICELIST[]? list, ref uint count, uint size);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern uint GetRawInputDeviceInfoW(IntPtr hDevice, uint command,
        IntPtr data, ref uint size);

    public static List<InputDeviceEntry> Enumerate()
    {
        var result = new List<InputDeviceEntry>();
        try
        {
            uint count = 0;
            uint stride = (uint)Marshal.SizeOf<RAWINPUTDEVICELIST>();
            if (GetRawInputDeviceList(null, ref count, stride) != 0 || count == 0)
                return result;

            var list = new RAWINPUTDEVICELIST[count];
            if (GetRawInputDeviceList(list, ref count, stride) == uint.MaxValue)
                return result;

            for (int i = 0; i < count; i++)
                if (Describe(list[i]) is { } entry)
                    result.Add(entry);
        }
        catch
        {
            // Interop failure: an empty list is honest — nothing was read.
        }

        return result
            .OrderBy(e => e.Kind switch
            {
                "Precision touchpad" => 0,
                "Mouse" => 1,
                "Keyboard" => 2,
                _ => 3,
            })
            .ThenBy(e => e.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    private static InputDeviceEntry? Describe(RAWINPUTDEVICELIST dev)
    {
        uint size = 0;
        if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICEINFO, IntPtr.Zero, ref size) != 0
            || size < 8)
            return null;

        IntPtr buf = Marshal.AllocHGlobal((int)size);
        try
        {
            Marshal.WriteInt32(buf, 0, (int)size);   // cbSize
            if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICEINFO, buf, ref size) == uint.MaxValue)
                return null;

            int type = Marshal.ReadInt32(buf, 4);
            IntPtr arm = buf + 8;
            string path = DevicePath(dev.hDevice);
            string name = FriendlyName(path);

            switch (type)
            {
                case RIM_TYPEMOUSE:
                {
                    var m = Marshal.PtrToStructure<RID_DEVICE_INFO_MOUSE>(arm);
                    return new InputDeviceEntry("Mouse", name,
                        $"{m.dwNumberOfButtons} buttons, "
                        + $"{(m.fHasHorizontalWheel != 0 ? "h-wheel" : "no h-wheel")}"
                        + (m.dwSampleRate > 0 ? $", {m.dwSampleRate} Hz" : ""));
                }
                case RIM_TYPEKEYBOARD:
                {
                    var k = Marshal.PtrToStructure<RID_DEVICE_INFO_KEYBOARD>(arm);
                    return new InputDeviceEntry("Keyboard", name,
                        $"type {k.dwType}/{k.dwSubType}, {k.dwNumberOfKeysTotal} keys, "
                        + $"{k.dwNumberOfFunctionKeys} F-keys, {k.dwNumberOfIndicators} LEDs");
                }
                case RIM_TYPEHID:
                {
                    var h = Marshal.PtrToStructure<RID_DEVICE_INFO_HID>(arm);
                    bool touchpad = h.usUsagePage == 0x0D && h.usUsage == 0x05;
                    // Only touchpads are interesting here; the rest of the HID
                    // list is game controllers and vendor plumbing.
                    if (!touchpad) return null;
                    return new InputDeviceEntry("Precision touchpad", name,
                        $"VID {h.dwVendorId:X4} PID {h.dwProductId:X4}, "
                        + $"rev {h.dwVersionNumber}, usage {h.usUsagePage:X2}:{h.usUsage:X2}");
                }
                default:
                    return null;
            }
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    private static string DevicePath(IntPtr hDevice)
    {
        uint chars = 0;
        if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, IntPtr.Zero, ref chars) != 0
            || chars == 0)
            return string.Empty;
        IntPtr buf = Marshal.AllocHGlobal((int)chars * 2);
        try
        {
            if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, buf, ref chars) == uint.MaxValue)
                return string.Empty;
            return Marshal.PtrToStringUni(buf) ?? string.Empty;
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    /// <summary>
    /// Turn a raw-input device path into the name Device Manager shows. The
    /// path embeds the device instance ID, which is the key under
    /// HKLM\SYSTEM\CurrentControlSet\Enum where the friendly name lives.
    /// </summary>
    private static string FriendlyName(string devicePath)
    {
        if (string.IsNullOrEmpty(devicePath)) return "(unnamed)";
        try
        {
            // \\?\HID#VID_04F3&PID_307A#7&...#{guid}  →  HID\VID_04F3&PID_307A\7&...
            string trimmed = devicePath.TrimStart('\\', '?');
            int guid = trimmed.IndexOf('{');
            if (guid > 0) trimmed = trimmed[..guid].TrimEnd('#');
            string instance = trimmed.Replace('#', '\\');

            using var key = Registry.LocalMachine.OpenSubKey(
                $@"SYSTEM\CurrentControlSet\Enum\{instance}");
            string? name = key?.GetValue("FriendlyName") as string
                        ?? key?.GetValue("DeviceDesc") as string;
            if (name == null) return ShortPath(devicePath);
            // DeviceDesc is "@file.inf,%key%;Real Name" — keep the real name.
            int semi = name.LastIndexOf(';');
            return semi >= 0 ? name[(semi + 1)..] : name;
        }
        catch
        {
            return ShortPath(devicePath);
        }
    }

    private static string ShortPath(string devicePath)
    {
        int hash = devicePath.IndexOf('#');
        int second = hash >= 0 ? devicePath.IndexOf('#', hash + 1) : -1;
        return second > 0 ? devicePath[(hash + 1)..second] : devicePath;
    }

    /// <summary>Multi-line summary for the copied diagnostics text.</summary>
    public static string ToText(IReadOnlyList<InputDeviceEntry> devices)
    {
        if (devices.Count == 0) return "  (none enumerated)";
        var sb = new StringBuilder();
        foreach (var d in devices)
            sb.AppendLine($"  {d.Kind,-20}{d.Name}  —  {d.Detail}");
        return sb.ToString().TrimEnd();
    }
}
