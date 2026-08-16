using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using CKFlip3D.Settings.Models;

namespace CKFlip3D.Settings.Services;

/// <summary>
/// Everything the settings UI needs to know about the precision touchpad:
///
///   • <see cref="IsPresent"/> — is there one at all (the touchpad menu is
///     grayed out when there is not);
///   • a live contact feed for the activity preview, read straight from
///     WM_INPUT on a private message-only window, so it works whether or not
///     the CKFlip3D core is running;
///   • the same gesture recogniser the core uses (hook/touchpadhook.cpp), so
///     the preview fires exactly when the real thing would — that is what
///     makes the sensitivity and smoothing sliders tunable.
///
/// Report parsing follows the Windows Precision Touchpad contract: the first
/// report of a frame carries the contact count, further reports carry the
/// remaining contacts.
/// </summary>
public static class TouchpadService
{
    // ---- HID / raw input interop -------------------------------------------

    private const int RIM_TYPEHID = 2;
    private const uint RID_INPUT = 0x10000003;
    private const uint RIDI_PREPARSEDDATA = 0x20000005;
    private const uint RIDI_DEVICEINFO = 0x2000000b;
    private const uint RIDEV_INPUTSINK = 0x00000100;
    private const uint RIDEV_REMOVE = 0x00000001;
    private const int WM_INPUT = 0x00FF;

    private const ushort PageGeneric = 0x01;
    private const ushort PageDigitizer = 0x0D;
    private const ushort UsageTouchPad = 0x05;
    private const ushort UsageX = 0x30;
    private const ushort UsageY = 0x31;
    private const ushort UsageTipSwitch = 0x42;
    private const ushort UsageContactId = 0x51;
    private const ushort UsageContactCount = 0x54;

    private const int HidP_Input = 0;
    private const int HIDP_STATUS_SUCCESS = 0x00110000;

    [StructLayout(LayoutKind.Sequential)]
    private struct RAWINPUTDEVICE
    {
        public ushort usUsagePage;
        public ushort usUsage;
        public uint dwFlags;
        public IntPtr hwndTarget;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RAWINPUTDEVICELIST
    {
        public IntPtr hDevice;
        public uint dwType;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct RID_DEVICE_INFO_HID
    {
        public uint dwVendorId, dwProductId, dwVersionNumber;
        public ushort usUsagePage, usUsage;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct HIDP_CAPS
    {
        public ushort Usage, UsagePage;
        public ushort InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)]
        public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices;
        public ushort NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
    }

    // Mirrors HIDP_VALUE_CAPS under the header's #pragma pack(4). The 16-byte
    // Range/NotRange union is flattened into eight ushorts — only its first
    // member (NotRange.Usage) is of interest here.
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct HIDP_VALUE_CAPS
    {
        public ushort UsagePage;
        public byte ReportID, IsAlias;
        public ushort BitField, LinkCollection, LinkUsage, LinkUsagePage;
        public byte IsRange, IsStringRange, IsDesignatorRange, IsAbsolute;
        public byte HasNull, Reserved;
        public ushort BitSize, ReportCount;
        public ushort Reserved2A, Reserved2B, Reserved2C, Reserved2D, Reserved2E;
        public uint UnitsExp, Units;
        public int LogicalMin, LogicalMax, PhysicalMin, PhysicalMax;
        public ushort U0, U1, U2, U3, U4, U5, U6, U7;   // Range / NotRange

        public ushort NotRangeUsage => U0;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterRawInputDevices(
        [In] RAWINPUTDEVICE[] devices, uint num, uint size);

    [DllImport("user32.dll")]
    private static extern uint GetRawInputData(IntPtr hRawInput, uint command,
        IntPtr data, ref uint size, uint headerSize);

    [DllImport("user32.dll")]
    private static extern uint GetRawInputDeviceInfoW(IntPtr hDevice, uint command,
        IntPtr data, ref uint size);

    [DllImport("user32.dll")]
    private static extern uint GetRawInputDeviceList(
        [In, Out] RAWINPUTDEVICELIST[]? list, ref uint count, uint size);

    [DllImport("hid.dll")]
    private static extern int HidP_GetCaps(IntPtr preparsed, ref HIDP_CAPS caps);

    [DllImport("hid.dll")]
    private static extern int HidP_GetValueCaps(int reportType,
        [In, Out] HIDP_VALUE_CAPS[] caps, ref ushort capsLength, IntPtr preparsed);

    [DllImport("hid.dll")]
    private static extern int HidP_GetUsages(int reportType, ushort usagePage,
        ushort linkCollection, [In, Out] ushort[] usageList, ref uint usageLength,
        IntPtr preparsed, byte[] report, uint reportLength);

    [DllImport("hid.dll")]
    private static extern int HidP_GetUsageValue(int reportType, ushort usagePage,
        ushort linkCollection, ushort usage, out uint value, IntPtr preparsed,
        byte[] report, uint reportLength);

    // ---- Device presence ---------------------------------------------------

    private static bool? _presentCache;

    /// <summary>True when a Windows Precision Touchpad is attached.</summary>
    public static bool IsPresent => _presentCache ??= DetectTouchpad();

    /// <summary>Forget the cached answer (dock attached, Bluetooth pad paired…).</summary>
    public static void InvalidatePresence() => _presentCache = null;

    private static bool DetectTouchpad()
    {
        try
        {
            uint count = 0;
            uint stride = (uint)Marshal.SizeOf<RAWINPUTDEVICELIST>();
            if (GetRawInputDeviceList(null, ref count, stride) != 0 || count == 0)
                return false;

            var list = new RAWINPUTDEVICELIST[count];
            if (GetRawInputDeviceList(list, ref count, stride) == uint.MaxValue)
                return false;

            for (int i = 0; i < count; i++)
            {
                if (list[i].dwType != RIM_TYPEHID) continue;
                if (GetHidUsage(list[i].hDevice) is not var (page, usage)) continue;
                if (page == PageDigitizer && usage == UsageTouchPad)
                    return true;
            }
        }
        catch
        {
            // Interop failure: report "no touchpad" and leave the menu disabled
            // rather than break the settings window.
        }
        return false;
    }

    private static (ushort page, ushort usage)? GetHidUsage(IntPtr hDevice)
    {
        // RID_DEVICE_INFO = { cbSize, dwType, union }. The union's largest arm
        // is the keyboard one, NOT the HID one — sizing the buffer to the HID
        // arm makes the call fail outright, so ask the API for the size.
        uint size = 0;
        if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, IntPtr.Zero, ref size) != 0
            || size < 8)
            return null;

        IntPtr buf = Marshal.AllocHGlobal((int)size);
        try
        {
            Marshal.WriteInt32(buf, 0, (int)size);   // cbSize
            if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, buf, ref size) == uint.MaxValue)
                return null;
            if (Marshal.ReadInt32(buf, 4) != RIM_TYPEHID)
                return null;
            var hid = Marshal.PtrToStructure<RID_DEVICE_INFO_HID>(buf + 8);
            return (hid.usUsagePage, hid.usUsage);
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    // ---- Live contact feed --------------------------------------------------

    /// <summary>One touchpad frame: contacts in pad-relative 0..1 coordinates.</summary>
    public static event Action<IReadOnlyList<Point>>? FrameReceived;

    /// <summary>A recognised gesture, using the CURRENT settings values.</summary>
    public static event Action<string>? GestureRecognised;

    private static HwndSource? _source;
    private static SettingsModel? _model;

    /// <summary>True while the preview is actually reading the touchpad.</summary>
    public static bool IsCapturing { get; private set; }

    /// <summary>
    /// Prepare the monitor. <paramref name="model"/> supplies the live gesture
    /// settings so the preview reacts exactly like the core would. Nothing is
    /// read until <see cref="SetCapturing"/> arms it — the preview is
    /// press-to-focus, so scrolling past it never shows up as activity.
    /// </summary>
    public static void StartMonitoring(SettingsModel model)
    {
        _model = model;
        if (_source != null || !IsPresent) return;

        try
        {
            var parameters = new HwndSourceParameters("CKFlip3D.TouchpadMonitor")
            {
                ParentWindow = new IntPtr(-3),   // HWND_MESSAGE
                WindowStyle = 0,
            };
            _source = new HwndSource(parameters);
            _source.AddHook(WndProc);
        }
        catch
        {
            StopMonitoring();
        }
    }

    /// <summary>
    /// Arm or disarm the raw-input registration. Disarmed, the OS does not
    /// even deliver WM_INPUT here, so an unfocused preview cannot pick up a
    /// stray two-finger scroll.
    /// </summary>
    public static void SetCapturing(bool capturing)
    {
        if (_source == null || IsCapturing == capturing) return;
        try
        {
            var rid = new RAWINPUTDEVICE[1];
            rid[0].usUsagePage = PageDigitizer;
            rid[0].usUsage = UsageTouchPad;
            rid[0].dwFlags = capturing ? RIDEV_INPUTSINK : RIDEV_REMOVE;
            rid[0].hwndTarget = capturing ? _source.Handle : IntPtr.Zero;
            if (RegisterRawInputDevices(rid, 1, (uint)Marshal.SizeOf<RAWINPUTDEVICE>()))
                IsCapturing = capturing;
        }
        catch { /* leave the flag alone; the UI reports what stuck */ }

        if (!IsCapturing)
        {
            ResetGesture();
            _frame.Clear();
            _frameIds.Clear();
            _frameRemaining = 0;
            _frameDevice = IntPtr.Zero;
            FrameReceived?.Invoke([]);
        }
    }

    /// <summary>Stop listening and drop the message-only window.</summary>
    public static void StopMonitoring()
    {
        if (_source == null) return;
        SetCapturing(false);
        try
        {
            _source.RemoveHook(WndProc);
            _source.Dispose();
        }
        catch { /* teardown is best effort */ }
        _source = null;
        foreach (var d in _devices.Values)
            if (d.Preparsed != IntPtr.Zero) Marshal.FreeHGlobal(d.Preparsed);
        _devices.Clear();
        // The cached handles are gone; nothing may still be keyed on them.
        _frame.Clear();
        _frameIds.Clear();
        _frameRemaining = 0;
        _frameDevice = IntPtr.Zero;
        if (_rawNative != IntPtr.Zero)
        {
            Marshal.FreeHGlobal(_rawNative);
            _rawNative = IntPtr.Zero;
            _rawNativeSize = 0;
        }
        ResetGesture();
    }

    private static IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg != WM_INPUT) return IntPtr.Zero;
        try { OnRawInput(lParam); }
        catch { /* never let a malformed report take the settings window down */ }
        return IntPtr.Zero;
    }

    // ---- Device descriptor cache -------------------------------------------

    private sealed class DeviceCaps
    {
        public IntPtr Preparsed;          // unmanaged copy, alive for the process
        public ushort[] Fingers = [];     // link collections carrying X and Y
        public int XMin, XMax, YMin, YMax;
        public bool Usable;
    }

    private static readonly Dictionary<IntPtr, DeviceCaps> _devices = new();

    private static DeviceCaps? GetCaps(IntPtr hDevice)
    {
        if (_devices.TryGetValue(hDevice, out var cached))
            return cached;

        var caps = new DeviceCaps();
        _devices[hDevice] = caps;   // cache failures too — do not retry per report

        uint size = 0;
        if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, IntPtr.Zero, ref size) != 0
            || size == 0)
            return caps;

        IntPtr pp = Marshal.AllocHGlobal((int)size);
        if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, pp, ref size) == uint.MaxValue)
        {
            Marshal.FreeHGlobal(pp);
            return caps;
        }
        caps.Preparsed = pp;

        var hidCaps = new HIDP_CAPS { Reserved = new ushort[17] };
        if (HidP_GetCaps(pp, ref hidCaps) != HIDP_STATUS_SUCCESS
            || hidCaps.NumberInputValueCaps == 0)
            return caps;

        var valueCaps = new HIDP_VALUE_CAPS[hidCaps.NumberInputValueCaps];
        ushort capCount = hidCaps.NumberInputValueCaps;
        if (HidP_GetValueCaps(HidP_Input, valueCaps, ref capCount, pp) != HIDP_STATUS_SUCCESS)
            return caps;

        // A finger is a link collection reporting BOTH X and Y. Collect the
        // candidates first and take the coordinate ranges from a collection we
        // actually accepted — a pad that also exposes a mouse collection would
        // otherwise hand us that one's range. Usage RANGES are honoured too,
        // which some vendors' descriptors use instead of single usages.
        var xs = new List<(ushort Lc, int Lo, int Hi)>();
        var ys = new List<(ushort Lc, int Lo, int Hi)>();
        for (int i = 0; i < capCount; i++)
        {
            var c = valueCaps[i];
            if (c.UsagePage != PageGeneric || c.LogicalMax <= c.LogicalMin) continue;
            if (Covers(c, UsageX)) xs.Add((c.LinkCollection, c.LogicalMin, c.LogicalMax));
            else if (Covers(c, UsageY)) ys.Add((c.LinkCollection, c.LogicalMin, c.LogicalMax));
        }

        var fingers = new List<ushort>();
        foreach (var x in xs)
        {
            int yi = ys.FindIndex(y => y.Lc == x.Lc);
            if (yi < 0) continue;
            if (fingers.Count == 0)
            {
                caps.XMin = x.Lo; caps.XMax = x.Hi;
                caps.YMin = ys[yi].Lo; caps.YMax = ys[yi].Hi;
            }
            fingers.Add(x.Lc);
        }

        caps.Fingers = fingers.Distinct().OrderBy(v => v).ToArray();
        caps.Usable = caps.Fingers.Length > 0 && caps.XMax > caps.XMin && caps.YMax > caps.YMin;
        return caps;

        static bool Covers(HIDP_VALUE_CAPS c, ushort usage) =>
            c.IsRange != 0 ? usage >= c.U0 && usage <= c.U1 : c.U0 == usage;
    }

    // ---- Report → frame ----------------------------------------------------

    private static readonly List<Point> _frame = new(10);
    private static int _frameRemaining;
    private static IntPtr _frameDevice;   // whose half-built frame this is
    private static byte[] _rawBuffer = new byte[256];
    private static IntPtr _rawNative;
    private static int _rawNativeSize;
    private static readonly ushort[] _usageScratch = new ushort[8];
    private static readonly List<uint> _frameIds = new(10);
    private static byte[] _report = [];

    // RAWINPUTHEADER: dwType, dwSize, hDevice, wParam.
    private static readonly uint HeaderSize = IntPtr.Size == 8 ? 24u : 16u;

    private static void OnRawInput(IntPtr hRawInput)
    {
        uint size = 0;
        if (GetRawInputData(hRawInput, RID_INPUT, IntPtr.Zero, ref size, HeaderSize) != 0
            || size == 0)
            return;

        // One long-lived unmanaged staging block — WM_INPUT arrives at up to
        // ~125 Hz and per-message allocation would be pure churn.
        if (_rawNativeSize < (int)size)
        {
            if (_rawNative != IntPtr.Zero) Marshal.FreeHGlobal(_rawNative);
            _rawNative = Marshal.AllocHGlobal((int)size);
            _rawNativeSize = (int)size;
        }
        if (GetRawInputData(hRawInput, RID_INPUT, _rawNative, ref size, HeaderSize) != size)
            return;
        if (_rawBuffer.Length < size) _rawBuffer = new byte[size];
        Marshal.Copy(_rawNative, _rawBuffer, 0, (int)size);

        if (BitConverter.ToInt32(_rawBuffer, 0) != RIM_TYPEHID) return;
        IntPtr hDevice = IntPtr.Size == 8
            ? new IntPtr(BitConverter.ToInt64(_rawBuffer, 8))
            : new IntPtr(BitConverter.ToInt32(_rawBuffer, 8));

        var caps = GetCaps(hDevice);
        if (caps is not { Usable: true }) return;

        // A frame is assembled across several reports and belongs to one
        // device; a second pad reporting at once starts its own rather than
        // merging its contacts into the first one's half-built frame.
        if (_frameDevice != hDevice)
        {
            _frame.Clear();
            _frameIds.Clear();
            _frameRemaining = 0;
            _frameDevice = hDevice;
        }

        // …then RAWHID { dwSizeHid, dwCount, bRawData[] }.
        int headerSize = (int)HeaderSize;
        int sizeHid = BitConverter.ToInt32(_rawBuffer, headerSize);
        int reportCount = BitConverter.ToInt32(_rawBuffer, headerSize + 4);
        int dataOffset = headerSize + 8;
        if (sizeHid <= 0 || reportCount <= 0) return;

        if (_report.Length != sizeHid) _report = new byte[sizeHid];
        byte[] report = _report;
        float xSpan = caps.XMax - caps.XMin;
        float ySpan = caps.YMax - caps.YMin;

        for (int r = 0; r < reportCount; r++)
        {
            int offset = dataOffset + r * sizeHid;
            if (offset + sizeHid > _rawBuffer.Length) break;
            Buffer.BlockCopy(_rawBuffer, offset, report, 0, sizeHid);

            bool haveCount = HidP_GetUsageValue(HidP_Input, PageDigitizer, 0,
                UsageContactCount, out uint contactCount, caps.Preparsed,
                report, (uint)sizeHid) == HIDP_STATUS_SUCCESS;

            if (haveCount && contactCount > 0)
            {
                _frame.Clear();
                _frameIds.Clear();
                _frameRemaining = (int)Math.Min(contactCount, 10);
            }
            else if (_frameRemaining <= 0)
            {
                if (haveCount)
                {
                    _frame.Clear();
                    _frameIds.Clear();
                    Publish();
                }
                continue;
            }

            int inThisReport = Math.Min(_frameRemaining, caps.Fingers.Length);
            for (int f = 0; f < inThisReport; f++)
            {
                ushort lc = caps.Fingers[f];

                uint usageLen = (uint)_usageScratch.Length;
                Array.Clear(_usageScratch);
                bool tip = false;
                if (HidP_GetUsages(HidP_Input, PageDigitizer, lc, _usageScratch,
                        ref usageLen, caps.Preparsed, report, (uint)sizeHid) == HIDP_STATUS_SUCCESS)
                {
                    for (int u = 0; u < usageLen && u < _usageScratch.Length; u++)
                        if (_usageScratch[u] == UsageTipSwitch) { tip = true; break; }
                }
                if (!tip) continue;

                if (HidP_GetUsageValue(HidP_Input, PageGeneric, lc, UsageX, out uint x,
                        caps.Preparsed, report, (uint)sizeHid) != HIDP_STATUS_SUCCESS
                    || HidP_GetUsageValue(HidP_Input, PageGeneric, lc, UsageY, out uint y,
                        caps.Preparsed, report, (uint)sizeHid) != HIDP_STATUS_SUCCESS)
                    continue;
                HidP_GetUsageValue(HidP_Input, PageDigitizer, lc, UsageContactId,
                    out uint id, caps.Preparsed, report, (uint)sizeHid);

                _frame.Add(new Point(((int)x - caps.XMin) / xSpan, ((int)y - caps.YMin) / ySpan));
                _frameIds.Add(id);
            }

            _frameRemaining -= inThisReport;
            if (_frameRemaining <= 0)
            {
                _frameRemaining = 0;
                Publish();
                _frame.Clear();
                _frameIds.Clear();
            }
        }
    }

    private static void Publish()
    {
        var snapshot = _frame.ToArray();
        Recognise(snapshot);
        FrameReceived?.Invoke(snapshot);
    }

    // ---- Gesture recogniser (mirrors hook/touchpadhook.cpp) -----------------

    private const float AxisBias = 1.20f;
    private const float TapSlop = 0.02f;
    private const int TapMaxMs = 300;
    private const double DiagonalRatio = 0.45;
    private const double CommitDistMul = 2.10;
    private const double CommitAxisBias = 2.20;
    private const double StrokeIdleEps = 0.0005;   // "still moving" threshold
    private const int StrokeIdleMs = 220;          // a pause this long restarts the stroke
    private const int SequenceStaleMs = 500;       // no report at all this long = touch over

    private static bool _inSequence;
    private static int _maxFingers;
    private static long _startTick;
    private static long _lastMoveTick;
    private static long _lastFrameTick;
    private static double _prevX, _prevY, _totalDx, _totalDy, _stepAccum;
    private static double _smoothDx, _smoothDy, _speed;
    private static bool _fired, _moved, _cycled;
    private static uint[] _prevIds = [];

    private static void ResetGesture()
    {
        _inSequence = false;
        _maxFingers = 0;
        _startTick = _lastMoveTick = _lastFrameTick = 0;
        _prevX = _prevY = _totalDx = _totalDy = _stepAccum = 0;
        _smoothDx = _smoothDy = _speed = 0;
        _fired = _moved = _cycled = false;
        _prevIds = [];
    }

    // These four mirror hook/touchpadhook.cpp exactly — the preview is only
    // worth anything if it fires where the real recogniser fires.
    private static float StepThreshold(int sensitivity) =>
        0.14f - (Math.Clamp(sensitivity, 1, 100) - 1) / 99.0f * 0.12f;

    private static float SwipeThreshold(int sensitivity) =>
        0.15f - (Math.Clamp(sensitivity, 1, 100) - 1) / 99.0f * 0.09f;

    private static double SmoothAlpha(int smoothing) =>
        1.0 - Math.Clamp(smoothing, 0, 100) / 100.0 * 0.85;

    private static double DeadZone(int smoothing) =>
        Math.Clamp(smoothing, 0, 100) / 100.0 * 0.0022;

    /// <summary>Two or four fingers — never three, that one is Windows'.</summary>
    private static int ActivateFingerCount(int gesture) => gesture is 3 or 4 ? 4 : 2;

    /// <summary>X direction the opening stroke carries: "\" travels right.</summary>
    private static int ActivateXSign(int gesture) => gesture is 1 or 3 ? +1 : -1;

    /// <summary>Does this stroke draw the configured diagonal? sign −1 = reversed.</summary>
    private static bool IsDiagonalStroke(int gesture, int sign, double dx, double dy,
                                         double distance)
    {
        double ax = Math.Abs(dx), ay = Math.Abs(dy);
        double longer = Math.Max(ax, ay);
        if (longer <= 0) return false;
        if (Math.Min(ax, ay) < longer * DiagonalRatio) return false;
        if (Math.Sqrt(dx * dx + dy * dy) < distance) return false;
        return dx * ActivateXSign(gesture) * sign > 0 && dy * sign > 0;
    }

    private static string DescribeGesture(int gesture) => gesture switch
    {
        1 => "Two fingers ↘",
        2 => "Two fingers ↙",
        3 => "Four fingers ↘",
        4 => "Four fingers ↙",
        _ => "",
    };

    private static void Recognise(IReadOnlyList<Point> contacts)
    {
        var model = _model;
        if (model == null) return;

        long now = Environment.TickCount64;

        if (contacts.Count == 0)
        {
            int tapFingers = model.TouchpadCommitGesture switch { 1 => 1, 2 => 2, _ => 0 };
            if (_inSequence && !_fired && tapFingers != 0 && _maxFingers == tapFingers
                && !_moved && now - _startTick <= TapMaxMs)
                Raise(tapFingers == 1 ? "One-finger tap — commit"
                                      : "Two-finger tap — commit");
            ResetGesture();
            return;
        }

        // Same staleness guard as the core (kSequenceStaleMs): a touch whose
        // lift report never arrived must not carry its age and finger count
        // into the next one, and a sequence we only joined half-way through
        // is never a tap.
        bool recovered = false;
        if (_inSequence && now - _lastFrameTick > SequenceStaleMs)
        {
            ResetGesture();
            recovered = true;
        }

        if (!_inSequence)
        {
            ResetGesture();
            _inSequence = true;
            _startTick = now;
            _lastMoveTick = now;
            _moved = recovered;
        }
        _lastFrameTick = now;

        double cx = contacts.Average(p => p.X);
        double cy = contacts.Average(p => p.Y);

        bool sameSet = _prevIds.Length == _frameIds.Count;
        for (int i = 0; sameSet && i < _prevIds.Length; i++)
            sameSet = _prevIds[i] == _frameIds[i];

        _maxFingers = Math.Max(_maxFingers, contacts.Count);
        _prevIds = _frameIds.ToArray();

        if (!sameSet)
        {
            _prevX = cx;
            _prevY = cy;
            return;
        }

        double dx = cx - _prevX, dy = cy - _prevY;
        _prevX = cx;
        _prevY = cy;

        // Smoothing: dead zone first, then the exponential filter.
        double dead = DeadZone(model.TouchpadSmoothing);
        if (Math.Abs(dx) < dead) dx = 0;
        if (Math.Abs(dy) < dead) dy = 0;
        double a = SmoothAlpha(model.TouchpadSmoothing);
        _smoothDx += a * (dx - _smoothDx);
        _smoothDy += a * (dy - _smoothDy);
        dx = _smoothDx;
        dy = _smoothDy;
        _speed += 0.35 * (Math.Sqrt(dx * dx + dy * dy) - _speed);

        _totalDx += dx;
        _totalDy += dy;
        if (Math.Abs(_totalDx) > TapSlop || Math.Abs(_totalDy) > TapSlop) _moved = true;

        // The stroke has to be one continuous motion (core: kStrokeIdleMs).
        // The preview has no session, so unlike the core it always applies —
        // which is exactly what the panel is for: showing when a drifting
        // two-finger scroll would and would not have opened the cascade.
        if (Math.Abs(dx) > StrokeIdleEps || Math.Abs(dy) > StrokeIdleEps)
            _lastMoveTick = now;
        else if (now - _lastMoveTick > StrokeIdleMs)
        {
            _totalDx = _totalDy = 0;
            _lastMoveTick = now;
        }

        if (_fired) return;

        int gesture = model.TouchpadActivateGesture;
        bool horizontal = Math.Abs(_totalDx) > Math.Abs(_totalDy) * AxisBias;
        double swipe = SwipeThreshold(model.TouchpadSensitivity);

        // Commit: long, strongly vertical, and never once this touch has
        // already moved the stack — mirrors the core's guard exactly.
        int commitFingers = model.TouchpadCommitGesture == 3 ? 2 : 0;
        if (commitFingers != 0 && !_cycled && contacts.Count == commitFingers
            && _totalDy > Math.Abs(_totalDx) * CommitAxisBias
            && _totalDy >= swipe * CommitDistMul)
        {
            Raise("Two fingers down — commit");
            _fired = true;
            return;
        }

        if (contacts.Count == model.TouchpadCycleFingers && horizontal)
        {
            double thr = StepThreshold(model.TouchpadSensitivity);
            if (!model.WindowSnap)
            {
                // Window snap off: the stack rides the fingers, so report the
                // travel rather than discrete steps.
                _stepAccum += dx;
                if (Math.Abs(_stepAccum) >= thr * 0.5)
                {
                    Raise($"Free drag — {(_stepAccum < 0 ? "forward" : "back")} "
                        + $"{Math.Abs(_stepAccum) / thr:0.0} window(s)");
                    _stepAccum = 0;
                    _cycled = true;
                }
            }
            else
            {
                _stepAccum += dx;
                while (_stepAccum <= -thr)
                {
                    Raise(model.TouchpadReverse ? "Swipe ← — previous window" : "Swipe ← — next window");
                    _stepAccum += thr;
                    _cycled = true;
                }
                while (_stepAccum >= thr)
                {
                    Raise(model.TouchpadReverse ? "Swipe → — next window" : "Swipe → — previous window");
                    _stepAccum -= thr;
                    _cycled = true;
                }
            }
        }
        else if (gesture != 0 && contacts.Count == ActivateFingerCount(gesture))
        {
            // Same velocity boost as the core: a quick stroke fires sooner.
            double eff = swipe * Math.Clamp(1.0 - _speed * 12.0, 0.50, 1.0);
            if (IsDiagonalStroke(gesture, +1, _totalDx, _totalDy, eff))
            {
                Raise($"{DescribeGesture(gesture)} — open CKFlip3D");
                _fired = true;
            }
            else if (model.TouchpadCancelSwipe
                     && IsDiagonalStroke(gesture, -1, _totalDx, _totalDy, swipe))
            {
                Raise("Reversed stroke — cancel");
                _fired = true;
            }
        }
    }

    private static void Raise(string text)
    {
        GestureRecognised?.Invoke(text);
    }

}
