// ---------------------------------------------------------------------------
// Reading the touchpad directly.  Raw HID reports come in on a worker thread,
// get decoded against the device's own descriptor, and turn into contacts the
// gesture recogniser can follow across a frame.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#define NOMINMAX   // windef.h's min/max macros vs std::min in the frame walker
#include "touchpadhook.h"
#include "keyboardhook.h"
#include "../core/DebugLog.h"
#include "../core/Diagnostics.h"

// hidsdi.h first: hidpi.h uses NTSTATUS but does not define it in user mode.
#include <hidusage.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>       // _wcsicmp for the gesture token vocabulary

#pragma comment(lib, "hid.lib")

namespace TouchpadHook {
namespace {

// ---------------------------------------------------------------------------
// Threading model
// ---------------------------------------------------------------------------
// Raw input is delivered as WM_INPUT to a window, on the thread that owns
// it.  We deliberately do NOT reuse the keyboard hook's thread: that one is
// TIME_CRITICAL and lives under the OS LowLevelHooksTimeout, and HID report
// parsing (a handful of HidP_* calls per frame at up to ~125 Hz) has no
// business sharing it.  A separate normal-priority worker with its own
// message-only window keeps the hotkey path exactly as it was.
//
// All gesture state below is touched only on that worker thread.  Options
// are plain atomics written from the app thread.
// ---------------------------------------------------------------------------

// HID usages (Windows Precision Touchpad report descriptor).
constexpr USHORT kPageGeneric      = 0x01;
constexpr USHORT kPageDigitizer    = 0x0D;
constexpr USHORT kUsageTouchPad    = 0x05;
constexpr USHORT kUsageX           = 0x30;
constexpr USHORT kUsageY           = 0x31;
constexpr USHORT kUsageTipSwitch   = 0x42;
constexpr USHORT kUsageConfidence  = 0x47;
constexpr USHORT kUsageContactId   = 0x51;
constexpr USHORT kUsageContactCnt  = 0x54;

constexpr UINT WM_TP_REREGISTER = WM_APP + 1;   // options changed → (un)register

constexpr int   kMaxContacts  = 10;
constexpr float kAxisBias     = 1.20f;  // dominant axis must lead by 20 %
constexpr float kTapSlop      = 0.02f;  // pad fractions a "tap" may drift
constexpr ULONGLONG kTapMaxMs = 300;    // longest press still counted as a tap
// Scrub deltas travel in lParam as ten-thousandths of a window, fixed point.
constexpr float kScrubScale   = 10000.0f;

// --- Diagonal activation ---------------------------------------------------
// The stroke has to look like a deliberate diagonal, not a slide that
// wandered: the shorter axis must be at least this fraction of the longer
// one (≈ 24°..66°, centred on 45°).  That is also what keeps it clear of
// Windows' own slide recogniser, which only classifies cardinal directions.
constexpr float kDiagonalRatio = 0.45f;

// ...and it has to be ONE stroke.  The travel accumulator runs for as long as
// the fingers are down, so without this a two-finger scroll that drifts
// sideways over several seconds would eventually add up to a "diagonal" —
// and two fingers is the default opening gesture.  A pause this long while
// the cascade is CLOSED starts the measurement over; nothing about the open
// session (cycling, commit) is touched, those keep their running totals.
constexpr float     kStrokeIdleEps = 0.0005f;  // pad fractions per report
constexpr ULONGLONG kStrokeIdleMs  = 220;

// A touch with no report at all for this long is over, whether or not the
// pad ever sent the zero-contact frame that normally ends it.
constexpr ULONGLONG kSequenceStaleMs = 500;

// --- Commit ----------------------------------------------------------------
// A commit swipe ends the session, so it has to be unmistakable: a long
// travel, strongly vertical, and never while the same touch has already been
// cycling.  Anything less and a downward drift during a sideways swipe picks
// a window the user was only scrolling past.
constexpr float kCommitDistMul  = 2.10f;  // × the swipe threshold
constexpr float kCommitAxisBias = 2.20f;  // vertical must dominate this much

HWND  g_hwndNotify   = nullptr;
UINT  g_msgActivate  = 0;
UINT  g_msgCycle     = 0;
UINT  g_msgCycleBack = 0;
UINT  g_msgDismiss   = 0;
UINT  g_msgEscape    = 0;
UINT  g_msgScrub     = 0;
UINT  g_msgScrubEnd  = 0;

HANDLE g_thread       = nullptr;
DWORD  g_threadId     = 0;
HANDLE g_readyEvent   = nullptr;
// Written by the worker thread, read by the app thread in SetOptions.
std::atomic<HWND> g_hwndWorker{nullptr};
std::atomic<bool> g_workerReady{false};

// ---- Options (app thread → worker thread) ---------------------------------
// Starts DISABLED: the worker comes up listening to nothing and only
// registers once SetOptions arrives with the config's touchpadNav, so a user
// who wants no gestures never has raw input registered at all.
std::atomic<bool> g_optEnabled{false};
std::atomic<unsigned> g_optActivateMask{GestureBit(kActivateTwoBack)};
std::atomic<unsigned> g_optCycleMask{GestureBit(kCycleTwo)};
std::atomic<unsigned> g_optCommitMask{GestureBit(kCommitOneTap)};
std::atomic<bool> g_optReverse{false};
std::atomic<int>  g_optSensitivity{50};
std::atomic<int>  g_optSmoothing{35};
std::atomic<bool> g_optCancelSwipe{true};
std::atomic<bool> g_optContinuous{false};
std::atomic<bool> g_optWindowSnap{true};
std::atomic<bool> g_registered{false};   // raw-input device registered?

// Windows' own "Tap with a single finger to single-click" setting.
//
// Needed because the tap-commit stands down when the pointer path owns the
// left click (see EndSequence) — and that reasoning only holds while Windows
// actually SYNTHESISES a click for a tap.  With taps switched off no click ever
// arrives, so standing down would silently disable tap-to-commit instead of
// deferring to something better.  Defaults to true, which is both the Windows
// default and the safe assumption: it prefers the deferral over a double
// commit.  Re-read on every config load, so toggling it in Windows Settings
// takes effect without a restart.
std::atomic<bool> g_osTapsEnabled{true};

bool ReadOsTapsEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad",
            0, KEY_READ, &key) != ERROR_SUCCESS)
        return true;   // no precision-touchpad settings at all → assume default

    DWORD value = 1, cb = sizeof(value), type = 0;
    const bool ok =
        RegQueryValueExW(key, L"TapsEnabled", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &cb) == ERROR_SUCCESS
        && type == REG_DWORD;
    RegCloseKey(key);
    return ok ? (value != 0) : true;
}

// ---------------------------------------------------------------------------
// Swipe thresholds, in fractions of the pad's width / height.
// Sensitivity 1 → a long drag per step, 100 → a flick.
// ---------------------------------------------------------------------------
float StepThreshold()
{
    const float s = static_cast<float>(std::clamp(g_optSensitivity.load(
        std::memory_order_relaxed), 1, 100));
    return 0.14f - (s - 1.0f) / 99.0f * 0.12f;    // 0.14 … 0.02
}

float SwipeThreshold()
{
    const float s = static_cast<float>(std::clamp(g_optSensitivity.load(
        std::memory_order_relaxed), 1, 100));
    return 0.15f - (s - 1.0f) / 99.0f * 0.09f;    // 0.15 … 0.06
}

// Smoothing: an exponential filter over the per-report centroid delta plus a
// dead zone under it.  A resting hand and a twitchy pad stop nudging the
// stack; the filter only delays motion, it does not lose distance, so the
// thresholds above keep their meaning.
float SmoothAlpha()
{
    const float s = static_cast<float>(std::clamp(g_optSmoothing.load(
        std::memory_order_relaxed), 0, 100)) / 100.0f;
    return 1.0f - s * 0.85f;                      // 1.0 (raw) … 0.15
}

float DeadZone()
{
    const float s = static_cast<float>(std::clamp(g_optSmoothing.load(
        std::memory_order_relaxed), 0, 100)) / 100.0f;
    return s * 0.0022f;                           // pad fractions per report
}

// ===========================================================================
// HID device cache
// ===========================================================================
// Everything below is driven by the device's OWN report descriptor, so it
// holds for any precision touchpad: pads that pack every contact into one
// report and pads that spread a frame over several, descriptors that declare
// X/Y per finger collection and descriptors that declare them as a usage
// range, with or without a Confidence (palm-rejection) flag.
struct DeviceInfo {
    HANDLE                 handle = nullptr;
    std::vector<BYTE>      preparsed;
    std::vector<USHORT>    fingers;      // link collections carrying X/Y
    LONG                   xMin = 0, xMax = 0;
    LONG                   yMin = 0, yMax = 0;
    bool                   hasConfidence = false;
    bool                   usable = false;
    unsigned               vendorId = 0, productId = 0;   // diagnostics only
};

std::vector<DeviceInfo> g_devices;       // worker thread only

// A value cap covers `usage` either directly or through its usage range.
bool CapCoversUsage(const HIDP_VALUE_CAPS& c, USAGE usage)
{
    return c.IsRange ? (usage >= c.Range.UsageMin && usage <= c.Range.UsageMax)
                     : (c.NotRange.Usage == usage);
}

bool ButtonCapCoversUsage(const HIDP_BUTTON_CAPS& c, USAGE usage)
{
    return c.IsRange ? (usage >= c.Range.UsageMin && usage <= c.Range.UsageMax)
                     : (c.NotRange.Usage == usage);
}

DeviceInfo* BuildDeviceInfo(HANDLE hDevice)
{
    DeviceInfo info;
    info.handle = hDevice;

    {
        RID_DEVICE_INFO rid{};
        rid.cbSize = sizeof(rid);
        UINT cb = sizeof(rid);
        if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &rid, &cb)
            != static_cast<UINT>(-1) && rid.dwType == RIM_TYPEHID) {
            info.vendorId  = rid.hid.dwVendorId;
            info.productId = rid.hid.dwProductId;
        }
    }

    UINT size = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &size) != 0
        || size == 0) {
        g_devices.push_back(std::move(info));   // cache the failure too
        return &g_devices.back();
    }
    info.preparsed.resize(size);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA,
                               info.preparsed.data(), &size) == static_cast<UINT>(-1)) {
        g_devices.push_back(std::move(info));
        return &g_devices.back();
    }

    auto* pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(info.preparsed.data());
    HIDP_CAPS caps{};
    if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS
        || caps.NumberInputValueCaps == 0) {
        g_devices.push_back(std::move(info));
        return &g_devices.back();
    }

    std::vector<HIDP_VALUE_CAPS> valueCaps(caps.NumberInputValueCaps);
    USHORT capCount = caps.NumberInputValueCaps;
    if (HidP_GetValueCaps(HidP_Input, valueCaps.data(), &capCount, pp)
        != HIDP_STATUS_SUCCESS) {
        g_devices.push_back(std::move(info));
        return &g_devices.back();
    }

    // A finger is a link collection that reports BOTH X and Y.  Collect the
    // candidates first, then take the coordinate ranges from a collection we
    // actually accepted — a pad that also exposes a mouse collection would
    // otherwise hand us that one's range.
    struct Axis { USHORT lc; LONG lo, hi; };
    std::vector<Axis> xs, ys;
    for (USHORT i = 0; i < capCount; ++i) {
        const HIDP_VALUE_CAPS& c = valueCaps[i];
        if (c.UsagePage != kPageGeneric || c.LogicalMax <= c.LogicalMin)
            continue;
        if (CapCoversUsage(c, kUsageX))
            xs.push_back({ c.LinkCollection, c.LogicalMin, c.LogicalMax });
        else if (CapCoversUsage(c, kUsageY))
            ys.push_back({ c.LinkCollection, c.LogicalMin, c.LogicalMax });
    }
    for (const Axis& x : xs) {
        auto y = std::find_if(ys.begin(), ys.end(),
                              [&](const Axis& a) { return a.lc == x.lc; });
        if (y == ys.end())
            continue;
        if (info.fingers.empty()) {
            info.xMin = x.lo; info.xMax = x.hi;
            info.yMin = y->lo; info.yMax = y->hi;
        }
        info.fingers.push_back(x.lc);
    }
    // Report order follows the descriptor's collection order.
    std::sort(info.fingers.begin(), info.fingers.end());
    info.fingers.erase(std::unique(info.fingers.begin(), info.fingers.end()),
                       info.fingers.end());

    // Palm rejection: only pads that DECLARE Confidence get it enforced.
    if (caps.NumberInputButtonCaps > 0) {
        std::vector<HIDP_BUTTON_CAPS> buttonCaps(caps.NumberInputButtonCaps);
        USHORT btnCount = caps.NumberInputButtonCaps;
        if (HidP_GetButtonCaps(HidP_Input, buttonCaps.data(), &btnCount, pp)
            == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < btnCount; ++i) {
                if (buttonCaps[i].UsagePage == kPageDigitizer
                    && ButtonCapCoversUsage(buttonCaps[i], kUsageConfidence)) {
                    info.hasConfidence = true;
                    break;
                }
            }
        }
    }

    info.usable = !info.fingers.empty()
               && info.xMax > info.xMin
               && info.yMax > info.yMin;

    if (info.usable) {
        wchar_t buf[192];
        swprintf_s(buf, L"CKFlip touchpad: VID %04X PID %04X — %zu contact "
                        L"collection(s), X %ld..%ld Y %ld..%ld%s\n",
                   info.vendorId, info.productId, info.fingers.size(),
                   info.xMin, info.xMax, info.yMin, info.yMax,
                   info.hasConfidence ? L", confidence" : L"");
        CKLog::Log(buf);
    }

    g_devices.push_back(std::move(info));
    return &g_devices.back();
}

DeviceInfo* GetDeviceInfo(HANDLE hDevice)
{
    for (auto& d : g_devices)
        if (d.handle == hDevice)
            return &d;
    if (g_devices.size() > 8)
        g_devices.clear();        // pathological churn guard
    return BuildDeviceInfo(hDevice);
}

// ===========================================================================
// Gesture state machine
// ===========================================================================
struct Contact { uint32_t id; float x, y; };   // x/y already normalised 0..1

struct GestureState {
    bool      inSequence = false;
    int       maxFingers = 0;
    ULONGLONG startMs    = 0;
    ULONGLONG lastMoveMs = 0;    // last report that carried real motion
    ULONGLONG lastFrameMs = 0;   // last frame of any kind — staleness guard
    float     prevX = 0.0f, prevY = 0.0f;
    float     smoothDx = 0.0f, smoothDy = 0.0f;   // filtered per-report delta
    float     speed = 0.0f;                       // filtered |motion| per report
    float     totalDx = 0.0f, totalDy = 0.0f;
    float     stepAccum = 0.0f;
    bool      fired   = false;   // activation / cancel / commit delivered
    bool      moved   = false;   // left the tap slop
    bool      cycled  = false;   // this touch has already moved the stack
    bool      scrubbed = false;  // posted at least one scrub delta
    uint32_t  prevIds[kMaxContacts] = {};
    int       prevIdCount = 0;
};
GestureState g_g;

void ResetGesture() { g_g = GestureState{}; }

/// A gesture has just fired.  What happens to the fingers still on the pad?
///
/// Ordinarily: nothing more.  The touch is closed for business (`fired`) until
/// it lifts, and that is what keeps the tail of an opening diagonal — the
/// centimetre the hand travels before it registers that the cascade is up —
/// from immediately stepping the stack it just opened.
///
/// With CONTINUOUS gestures the same touch stays live and only the STROKE is
/// retired: the travel totals, the step accumulator and the filter state go
/// back to zero, so the next gesture is measured from here rather than from
/// where the finger landed.  That is what lets one motion open the cascade and
/// then walk through it without lifting — and it is also why the option warns
/// about misinput, because a stroke that wanders can now say two things.
void RetireStroke()
{
    if (!g_optContinuous.load(std::memory_order_relaxed)) {
        g_g.fired = true;
        return;
    }
    g_g.totalDx = g_g.totalDy = 0.0f;
    g_g.stepAccum = 0.0f;
    g_g.smoothDx = g_g.smoothDy = 0.0f;
    g_g.speed = 0.0f;
    g_g.lastMoveMs = GetTickCount64();
}

int ActivateFingerCount(int gesture)
{
    return (gesture == kActivateFourBack || gesture == kActivateFourFwd) ? 4 : 2;
}

// Sign the stroke's X must carry for this gesture.  Y is always positive
// (downward) for the opening stroke — both diagonals run top → bottom.
// "\" starts top-LEFT and travels right (+X); "/" starts top-RIGHT (−X).
int ActivateXSign(int gesture)
{
    return (gesture == kActivateTwoBack || gesture == kActivateFourBack) ? +1 : -1;
}

/// Is this stroke the diagonal `gesture` asks for?  `sign` is +1 for the
/// opening direction and −1 for the reversed (cancelling) one.
bool IsDiagonalStroke(int gesture, int sign, float dx, float dy, float distance)
{
    const float ax = std::fabs(dx), ay = std::fabs(dy);
    const float longer = std::max(ax, ay);
    if (longer <= 0.0f)
        return false;
    // Deliberately diagonal — see kDiagonalRatio.
    if (std::min(ax, ay) < longer * kDiagonalRatio)
        return false;
    if (std::sqrt(dx * dx + dy * dy) < distance)
        return false;
    return dx * static_cast<float>(ActivateXSign(gesture) * sign) > 0.0f
        && dy * static_cast<float>(sign) > 0.0f;
}

/// The first gesture in `mask` this stroke draws, or kActivateOff.
///
/// The finger count is part of the match, not a prior filter: two-finger and
/// four-finger diagonals can both be bound, and only one of them can be what
/// the hand on the pad is doing right now.
int MatchActivateStroke(unsigned mask, int sign, int count,
                        float dx, float dy, float distance)
{
    for (int g = kActivateTwoBack; g <= kActivateFourFwd; ++g) {
        if (!(mask & GestureBit(g)))
            continue;
        if (count != ActivateFingerCount(g))
            continue;
        if (IsDiagonalStroke(g, sign, dx, dy, distance))
            return g;
    }
    return kActivateOff;
}

/// Does the cycle set claim a swipe by this many fingers?
bool CycleClaims(unsigned mask, int count)
{
    return (count == 2 && (mask & GestureBit(kCycleTwo)))
        || (count == 4 && (mask & GestureBit(kCycleFour)));
}

void PostCommit()
{
    // Foreign, for the same reason a mouse click is: the keyboard hook never
    // saw this commit, so it cannot arm the Win/Alt release defusal itself —
    // and a cascade opened with Win+Tab and committed with a tap would leave
    // the Start menu to open on the modifier release.
    KeyboardHook::EndSessionForeign();
    PostMessageW(g_hwndNotify, g_msgDismiss, 0, 0);
}

void EndSequence(ULONGLONG now)
{
    if (!g_g.inSequence) {
        ResetGesture();
        return;
    }

    if (g_g.scrubbed)
        PostMessageW(g_hwndNotify, g_msgScrubEnd, 0, 0);

    // Tap commit: a short, still press of a bound finger count.  Both taps can
    // be bound at once, so the touch itself decides which one this was.
    const unsigned commitMask = g_optCommitMask.load(std::memory_order_relaxed);
    const int tapFingers =
        (g_g.maxFingers == 1 && (commitMask & GestureBit(kCommitOneTap))) ? 1
      : (g_g.maxFingers == 2 && (commitMask & GestureBit(kCommitTwoTap))) ? 2 : 0;

    // ...unless the pointer path already owns this tap.
    //
    // A ONE-finger tap is also a left click: Windows synthesises one, the mouse
    // hook turns it into a tile pick.  Committing here as well posts a SECOND
    // commit, from a different thread, for one physical tap — and the app acts
    // on whichever of the two it drains first.  That is a coin flip between
    // "the tile you tapped" and "the window in front".
    //
    // Standing down leaves the better of the two answers, because the click
    // knows where the cursor is and this thread does not.  It also hands tap
    // DETECTION to Windows, whose recogniser is far stricter than the
    // slop/timeout pair here — so a hand resting on the pad stops committing
    // sessions by accident.
    //
    // Three conditions, each load-bearing:
    //   - one finger      — two-finger taps are a RIGHT click, which no commit
    //                       path listens to, so they never collide;
    //   - taps enabled    — with "tap to click" off in Windows Settings no
    //                       click is coming, so there is nothing to defer to
    //                       and standing down would just lose the gesture;
    //   - pointer owns it — the select binding has to actually be a plain left
    //                       click for the collision to exist at all.
    const bool tapIsPointerClick =
        (tapFingers == 1)
        && g_osTapsEnabled.load(std::memory_order_relaxed)
        && KeyboardHook::PointerOwnsLeftClick();

    if (!g_g.fired
        && tapFingers != 0
        && !tapIsPointerClick
        && !g_g.moved
        && (now - g_g.startMs) <= kTapMaxMs
        && KeyboardHook::IsSessionActive())
        PostCommit();

    ResetGesture();
}

void ProcessFrame(const Contact* contacts, int count)
{
    const ULONGLONG now = GetTickCount64();

    if (count <= 0) {
        EndSequence(now);
        return;
    }

    // The sequence normally ends on a zero-contact report.  If that report
    // never arrives — the pad was unplugged mid-touch, the frame was dropped,
    // a session change ate it — the old touch would otherwise stay "open" and
    // carry its start time and finger count into the next one, silently
    // disabling tap-to-commit until the next clean lift.  A gap this long
    // means the fingers are long gone.
    bool recovered = false;
    if (g_g.inSequence && now - g_g.lastFrameMs > kSequenceStaleMs) {
        ResetGesture();
        recovered = true;
    }

    if (!g_g.inSequence) {
        ResetGesture();
        g_g.inSequence = true;
        g_g.startMs    = now;
        g_g.lastMoveMs = now;
        // A touch we only joined half-way through is not a tap: we never saw
        // it land, so neither its age nor its stillness proves anything, and
        // a finger that had been resting for a second must not commit a
        // window just because the pad went quiet and spoke up again.
        g_g.moved = recovered;
    }
    g_g.lastFrameMs = now;

    float sx = 0.0f, sy = 0.0f;
    for (int i = 0; i < count; ++i) { sx += contacts[i].x; sy += contacts[i].y; }
    const float cx = sx / static_cast<float>(count);
    const float cy = sy / static_cast<float>(count);

    // A finger landing or lifting jumps the centroid — rebase instead of
    // counting that jump as motion.
    bool sameSet = (count == g_g.prevIdCount);
    for (int i = 0; sameSet && i < count; ++i)
        sameSet = (contacts[i].id == g_g.prevIds[i]);

    if (g_g.maxFingers < count)
        g_g.maxFingers = count;
    for (int i = 0; i < count && i < kMaxContacts; ++i)
        g_g.prevIds[i] = contacts[i].id;
    g_g.prevIdCount = std::min(count, kMaxContacts);

    if (!sameSet) {
        g_g.prevX = cx;
        g_g.prevY = cy;
        g_g.smoothDx = g_g.smoothDy = 0.0f;
        return;
    }

    float dx = cx - g_g.prevX;
    float dy = cy - g_g.prevY;
    g_g.prevX = cx;
    g_g.prevY = cy;

    // ---- Smoothing: dead zone, then an exponential filter ------------------
    const float dead = DeadZone();
    if (std::fabs(dx) < dead) dx = 0.0f;
    if (std::fabs(dy) < dead) dy = 0.0f;
    const float a = SmoothAlpha();
    g_g.smoothDx += a * (dx - g_g.smoothDx);
    g_g.smoothDy += a * (dy - g_g.smoothDy);
    dx = g_g.smoothDx;
    dy = g_g.smoothDy;
    g_g.speed += 0.35f * (std::sqrt(dx * dx + dy * dy) - g_g.speed);

    g_g.totalDx += dx;
    g_g.totalDy += dy;
    if (std::fabs(g_g.totalDx) > kTapSlop || std::fabs(g_g.totalDy) > kTapSlop)
        g_g.moved = true;

    // One stroke, not a session's worth of drift — see kStrokeIdleMs.  Only
    // while the cascade is closed: an open session's cycle and commit swipes
    // read the same totals and must keep counting through a pause.
    if (std::fabs(dx) > kStrokeIdleEps || std::fabs(dy) > kStrokeIdleEps) {
        g_g.lastMoveMs = now;
    } else if (!KeyboardHook::IsSessionActive()
               && now - g_g.lastMoveMs > kStrokeIdleMs) {
        g_g.totalDx = g_g.totalDy = 0.0f;
        g_g.lastMoveMs = now;
    }

    if (g_g.fired)
        return;

    const unsigned activateMask = g_optActivateMask.load(std::memory_order_relaxed);
    const unsigned commitMask   = g_optCommitMask.load(std::memory_order_relaxed);
    const unsigned cycleMask    = g_optCycleMask.load(std::memory_order_relaxed);
    const bool reverse      = g_optReverse.load(std::memory_order_relaxed);
    const bool snap         = g_optWindowSnap.load(std::memory_order_relaxed);
    const bool horizontal   = std::fabs(g_g.totalDx) > std::fabs(g_g.totalDy) * kAxisBias;
    const float swipe       = SwipeThreshold();

    if (KeyboardHook::IsSessionActive()) {
        // Commit by swiping down.  Deliberately hard to trigger: a long,
        // strongly vertical travel, and never once this same touch has
        // started moving the stack — otherwise a sideways swipe that drifts
        // down picks a window the user was only passing.
        if ((commitMask & GestureBit(kCommitTwoDown))
            && !g_g.cycled
            && count == 2
            && g_g.totalDy > std::fabs(g_g.totalDx) * kCommitAxisBias
            && g_g.totalDy >= swipe * kCommitDistMul) {
            PostCommit();
            RetireStroke();
            return;
        }

        if (CycleClaims(cycleMask, count) && horizontal) {
            // Swiping left carries the row left, so the next window arrives
            // from the right — the same direction sense as the wheel.
            const float thr = StepThreshold();
            if (snap) {
                g_g.stepAccum += dx;
                while (g_g.stepAccum <= -thr) {
                    PostMessageW(g_hwndNotify,
                                 reverse ? g_msgCycleBack : g_msgCycle, 0, 0);
                    g_g.stepAccum += thr;
                    g_g.cycled = true;
                }
                while (g_g.stepAccum >= thr) {
                    PostMessageW(g_hwndNotify,
                                 reverse ? g_msgCycle : g_msgCycleBack, 0, 0);
                    g_g.stepAccum -= thr;
                    g_g.cycled = true;
                }
            } else if (dx != 0.0f) {
                // Window snap off: hand the controller a fractional amount
                // and let the stack ride the fingers.
                const float windows = (reverse ? dx : -dx) / thr;
                const LONG fixed = static_cast<LONG>(windows * kScrubScale);
                if (fixed != 0) {
                    PostMessageW(g_hwndNotify, g_msgScrub, 0,
                                 static_cast<LPARAM>(fixed));
                    g_g.scrubbed = true;
                    g_g.cycled   = true;
                }
            }
        } else if (g_optCancelSwipe.load(std::memory_order_relaxed)
                   && MatchActivateStroke(activateMask, -1, count,
                                          g_g.totalDx, g_g.totalDy, swipe)
                      != kActivateOff) {
            // ANY bound opening stroke drawn backwards = Escape.  Foreign, like
            // the tap commit: a cascade opened by holding Win and cancelled
            // with a gesture still owes that modifier's release a defusal, and
            // the keyboard hook never saw the gesture.
            KeyboardHook::EndSessionForeign();
            PostMessageW(g_hwndNotify, g_msgEscape, 0, 0);
            RetireStroke();
        }
    } else {
        // Fast strokes fire at a shorter distance — the gesture should feel
        // as immediate as the hotkey does.
        const float eff = swipe * std::clamp(1.0f - g_g.speed * 12.0f, 0.50f, 1.0f);
        if (MatchActivateStroke(activateMask, +1, count,
                                g_g.totalDx, g_g.totalDy, eff) != kActivateOff) {
            // Same contract as the keyboard hook's activation branch, filters
            // included: "Ignore in fullscreen applications" and the
            // ignored-apps list gate the gesture exactly as they gate the
            // hotkey.
            if (KeyboardHook::ShouldIgnoreActivation()) {
                g_g.fired = true;   // don't retry mid-stroke, continuous or not
                return;
            }
            KeyboardHook::SetSessionActive(true);
            PostMessageW(g_hwndNotify, g_msgActivate, 0, 0);
            RetireStroke();
        }
    }
}

// ---------------------------------------------------------------------------
// Turn one WM_INPUT payload into gesture frames.
//
// A precision touchpad splits a frame over several reports when it has more
// contacts than fit in one: the FIRST report of a frame carries the contact
// count, the rest carry the remainder.  We therefore accumulate until the
// announced count is in, then hand the whole frame to the state machine.
// ---------------------------------------------------------------------------
Contact g_frame[kMaxContacts];
int     g_frameCount     = 0;
int     g_frameRemaining = 0;
HANDLE  g_frameDevice    = nullptr;   // whose half-built frame this is

void ResetFrameAssembly()
{
    g_frameCount     = 0;
    g_frameRemaining = 0;
    g_frameDevice    = nullptr;
}

void ProcessRawInput(const RAWINPUT& ri)
{
    DeviceInfo* dev = GetDeviceInfo(ri.header.hDevice);
    if (!dev || !dev->usable)
        return;

    // A frame is assembled across several reports, so it belongs to one
    // device.  Two pads (a laptop's and a dock's) reporting at once would
    // otherwise merge their contacts into one nonsense frame; the newcomer
    // simply starts a frame of its own.
    if (g_frameDevice != ri.header.hDevice) {
        ResetFrameAssembly();
        g_frameDevice = ri.header.hDevice;
    }

    auto* pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(dev->preparsed.data());
    const ULONG reportLen = ri.data.hid.dwSizeHid;
    const float xSpan = static_cast<float>(dev->xMax - dev->xMin);
    const float ySpan = static_cast<float>(dev->yMax - dev->yMin);

    for (DWORD r = 0; r < ri.data.hid.dwCount; ++r) {
        auto* report = const_cast<PCHAR>(
            reinterpret_cast<const char*>(ri.data.hid.bRawData) + r * reportLen);

        ULONG contactCount = 0;
        const bool haveCount =
            HidP_GetUsageValue(HidP_Input, kPageDigitizer, 0, kUsageContactCnt,
                               &contactCount, pp, report, reportLen)
            == HIDP_STATUS_SUCCESS;

        if (haveCount && contactCount > 0) {
            g_frameCount     = 0;
            g_frameRemaining = static_cast<int>(
                std::min<ULONG>(contactCount, kMaxContacts));
        } else if (g_frameRemaining <= 0) {
            // Contact count 0 with no frame in progress = every finger has
            // lifted; anything else (a mouse-mode report, another report ID)
            // is not ours to interpret.
            if (haveCount) {
                g_frameCount = 0;
                ProcessFrame(nullptr, 0);
            }
            continue;
        }

        const int inThisReport = std::min<int>(
            g_frameRemaining, static_cast<int>(dev->fingers.size()));
        for (int f = 0; f < inThisReport; ++f) {
            const USHORT lc = dev->fingers[static_cast<size_t>(f)];

            USAGE usages[16] = {};
            ULONG usageLen = static_cast<ULONG>(std::size(usages));
            bool tip = false, confident = !dev->hasConfidence;
            if (HidP_GetUsages(HidP_Input, kPageDigitizer, lc, usages, &usageLen,
                               pp, report, reportLen) == HIDP_STATUS_SUCCESS) {
                for (ULONG u = 0; u < usageLen; ++u) {
                    if (usages[u] == kUsageTipSwitch)  tip = true;
                    if (usages[u] == kUsageConfidence) confident = true;
                }
            }
            if (!tip || !confident)
                continue;   // lifted, or a palm the pad already flagged

            ULONG x = 0, y = 0, id = 0;
            if (HidP_GetUsageValue(HidP_Input, kPageGeneric, lc, kUsageX,
                                   &x, pp, report, reportLen) != HIDP_STATUS_SUCCESS
                || HidP_GetUsageValue(HidP_Input, kPageGeneric, lc, kUsageY,
                                      &y, pp, report, reportLen) != HIDP_STATUS_SUCCESS)
                continue;
            HidP_GetUsageValue(HidP_Input, kPageDigitizer, lc, kUsageContactId,
                               &id, pp, report, reportLen);

            if (g_frameCount < kMaxContacts) {
                g_frame[g_frameCount].id = static_cast<uint32_t>(id);
                g_frame[g_frameCount].x  =
                    (static_cast<float>(static_cast<LONG>(x)) - static_cast<float>(dev->xMin)) / xSpan;
                g_frame[g_frameCount].y  =
                    (static_cast<float>(static_cast<LONG>(y)) - static_cast<float>(dev->yMin)) / ySpan;
                ++g_frameCount;
            }
        }

        g_frameRemaining -= inThisReport;
        if (g_frameRemaining <= 0) {
            g_frameRemaining = 0;
            ProcessFrame(g_frame, g_frameCount);
            g_frameCount = 0;
        }
    }
}

// ===========================================================================
// Worker window / thread
// ===========================================================================
constexpr const wchar_t* kWorkerClass = L"CKFlip3D_TouchpadWorker";

void ApplyRegistration(HWND hwnd)
{
    const bool want = g_optEnabled.load(std::memory_order_relaxed);
    if (want == g_registered.load(std::memory_order_relaxed))
        return;

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = kPageDigitizer;
    rid.usUsage     = kUsageTouchPad;
    if (want) {
        // INPUTSINK: gestures must work while another app is in the
        // foreground.  DEVNOTIFY: a touchpad that appears later (dock,
        // Bluetooth) starts working without a restart.
        rid.dwFlags    = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
        rid.hwndTarget = hwnd;
        if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            g_registered.store(true, std::memory_order_relaxed);
            CKLog::Log(L"CKFlip touchpad: raw input registered\n");
        } else {
            CKLog::Log(L"CKFlip touchpad: raw-input registration FAILED\n");
        }
    } else {
        rid.dwFlags    = RIDEV_REMOVE;
        rid.hwndTarget = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        g_registered.store(false, std::memory_order_relaxed);
        g_devices.clear();
        ResetFrameAssembly();   // the cached handles are gone with them
        ResetGesture();
        CKLog::Log(L"CKFlip touchpad: raw input released\n");
    }
}

LRESULT CALLBACK WorkerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TP_REREGISTER:
        ApplyRegistration(hwnd);
        return 0;

    case WM_INPUT_DEVICE_CHANGE:
        // Drop the cached descriptor for a device that came or went; the
        // next report rebuilds it.  A frame half-assembled from the departing
        // pad has to go with it, or its leftover count would swallow the
        // first reports of whatever arrives next.
        for (size_t i = 0; i < g_devices.size(); ++i) {
            if (g_devices[i].handle == reinterpret_cast<HANDLE>(lParam)) {
                g_devices.erase(g_devices.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
        ResetFrameAssembly();
        ResetGesture();
        return 0;

    case WM_INPUT: {
        if (!g_optEnabled.load(std::memory_order_relaxed))
            break;
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                            nullptr, &size, sizeof(RAWINPUTHEADER)) != 0
            || size == 0)
            break;
        static std::vector<BYTE> buffer;
        if (buffer.size() < size)
            buffer.resize(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                            buffer.data(), &size, sizeof(RAWINPUTHEADER))
            != size)
            break;
        const auto& ri = *reinterpret_cast<const RAWINPUT*>(buffer.data());
        if (ri.header.dwType == RIM_TYPEHID)
            ProcessRawInput(ri);
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI WorkerThreadProc(LPVOID)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WorkerProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWorkerClass;
    RegisterClassExW(&wc);   // duplicate registration is harmless

    HWND hwnd = CreateWindowExW(0, kWorkerClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    g_hwndWorker.store(hwnd, std::memory_order_release);
    g_workerReady.store(hwnd != nullptr, std::memory_order_release);
    if (g_readyEvent)
        SetEvent(g_readyEvent);
    if (!hwnd)
        return 1;

    ApplyRegistration(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Stop taking input before the window that receives it goes away.
    g_optEnabled.store(false, std::memory_order_relaxed);
    ApplyRegistration(hwnd);
    g_hwndWorker.store(nullptr, std::memory_order_release);
    DestroyWindow(hwnd);
    return 0;
}

// Enumerate attached precision touchpads (shared by IsTouchpadPresent and
// DescribeDevices — neither touches the worker thread's cache).
std::vector<HANDLE> EnumerateTouchpads()
{
    std::vector<HANDLE> found;
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0
        || count == 0)
        return found;

    std::vector<RAWINPUTDEVICELIST> list(count);
    count = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (count == static_cast<UINT>(-1))
        return found;

    for (UINT i = 0; i < count; ++i) {
        if (list[i].dwType != RIM_TYPEHID)
            continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT cb = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &cb)
            == static_cast<UINT>(-1))
            continue;
        if (info.dwType == RIM_TYPEHID
            && info.hid.usUsagePage == kPageDigitizer
            && info.hid.usUsage == kUsageTouchPad)
            found.push_back(list[i].hDevice);
    }
    return found;
}

// ---------------------------------------------------------------------------
// Token lists → gesture masks
// ---------------------------------------------------------------------------
// One walker for all three lists; the caller supplies the vocabulary.  Same
// shape as the key lists the keyboard hook parses ('!' parks an entry, blanks
// are skipped, unknown tokens are counted rather than fatal) because they are
// the same idea and a user editing config.json by hand should only have to
// learn it once.
struct GestureToken { const wchar_t* name; int gesture; };

unsigned ParseGestureList(const std::wstring& list, const GestureToken* vocab,
                          size_t vocabCount, unsigned* dropped)
{
    unsigned mask = 0;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L';', start);
        if (end == std::wstring::npos) end = list.size();

        std::wstring tok = list.substr(start, end - start);
        size_t b = tok.find_first_not_of(L" \t");
        size_t e = tok.find_last_not_of(L" \t");
        tok = (b == std::wstring::npos) ? std::wstring() : tok.substr(b, e - b + 1);

        if (!tok.empty() && tok[0] != L'!') {
            bool matched = false;
            for (size_t i = 0; i < vocabCount && !matched; ++i) {
                if (_wcsicmp(tok.c_str(), vocab[i].name) == 0) {
                    mask |= GestureBit(vocab[i].gesture);
                    matched = true;
                }
            }
            if (!matched && dropped) ++*dropped;
        }

        if (end == list.size()) break;
        start = end + 1;
    }
    return mask;
}

} // anonymous namespace

unsigned ParseActivateList(const std::wstring& list, unsigned* dropped)
{
    static const GestureToken kVocab[] = {
        { L"TwoDownRight",  kActivateTwoBack  },
        { L"TwoDownLeft",   kActivateTwoFwd   },
        { L"FourDownRight", kActivateFourBack },
        { L"FourDownLeft",  kActivateFourFwd  },
    };
    return ParseGestureList(list, kVocab, std::size(kVocab), dropped);
}

unsigned ParseCycleList(const std::wstring& list, unsigned* dropped)
{
    // Two or four fingers only — three belongs to Windows (see ActivateGesture),
    // so there is no token for it and a config that names one is simply dropped.
    static const GestureToken kVocab[] = {
        { L"TwoSwipe",  kCycleTwo  },
        { L"FourSwipe", kCycleFour },
    };
    return ParseGestureList(list, kVocab, std::size(kVocab), dropped);
}

unsigned ParseCommitList(const std::wstring& list, unsigned* dropped)
{
    static const GestureToken kVocab[] = {
        { L"OneTap",  kCommitOneTap  },
        { L"TwoTap",  kCommitTwoTap  },
        { L"TwoDown", kCommitTwoDown },
    };
    return ParseGestureList(list, kVocab, std::size(kVocab), dropped);
}

// ===========================================================================
// Public API
// ===========================================================================
bool IsTouchpadPresent()
{
    return !EnumerateTouchpads().empty();
}

std::wstring DescribeDevices()
{
    std::vector<HANDLE> pads = EnumerateTouchpads();
    if (pads.empty())
        return L"none";

    std::wstring out;
    for (HANDLE h : pads) {
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT cb = sizeof(info);
        if (GetRawInputDeviceInfoW(h, RIDI_DEVICEINFO, &info, &cb)
            == static_cast<UINT>(-1))
            continue;
        wchar_t buf[96];
        swprintf_s(buf, L"%sVID %04X PID %04X",
                   out.empty() ? L"" : L"; ",
                   info.hid.dwVendorId, info.hid.dwProductId);
        out += buf;
    }
    return out.empty() ? L"none" : out;
}

bool Install(HWND hwndNotify,
             UINT msgActivate,
             UINT msgCycle,
             UINT msgCycleBack,
             UINT msgDismiss,
             UINT msgEscape,
             UINT msgScrub,
             UINT msgScrubEnd)
{
    if (g_thread)
        return false;

    g_hwndNotify   = hwndNotify;
    g_msgActivate  = msgActivate;
    g_msgCycle     = msgCycle;
    g_msgCycleBack = msgCycleBack;
    g_msgDismiss   = msgDismiss;
    g_msgEscape    = msgEscape;
    g_msgScrub     = msgScrub;
    g_msgScrubEnd  = msgScrubEnd;

    // Deliberately NOT reported when no pad is attached.  Touchpad navigation
    // is on by default, so every desktop machine would carry a permanent
    // notice about hardware it was never going to have — and a log that
    // reports the ordinary state of a normal machine is a log people learn to
    // ignore.  The listener costs nothing and picks a pad up on hot-plug.
    CKLog::Log(IsTouchpadPresent()
        ? L"CKFlip touchpad: precision touchpad detected\n"
        : L"CKFlip touchpad: no precision touchpad attached (hot-plug still works)\n");

    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, &g_threadId);
    if (!g_thread) {
        Diag::ReportLastError(Diag::Code::RawInputFailed, Diag::Sev::Warning,
                              L"CKFlip3D could not start its touchpad listener",
                              L"gestures will not work this session; the keyboard "
                              L"and mouse are unaffected");
        if (g_readyEvent) { CloseHandle(g_readyEvent); g_readyEvent = nullptr; }
        return false;
    }

    if (g_readyEvent) {
        WaitForSingleObject(g_readyEvent, 3000);
        CloseHandle(g_readyEvent);
        g_readyEvent = nullptr;
    }
    const bool ready = g_workerReady.load(std::memory_order_acquire);
    if (!ready)
        Diag::Report(Diag::Code::RawInputFailed, Diag::Sev::Warning,
                     L"CKFlip3D's touchpad listener did not come up",
                     L"raw input registration did not complete within three "
                     L"seconds; gestures are off for this session");
    return ready;
}

void Uninstall()
{
    bool joined = true;
    if (g_thread) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        joined = (WaitForSingleObject(g_thread, 3000) == WAIT_OBJECT_0);
        CloseHandle(g_thread);
        g_thread   = nullptr;
        g_threadId = 0;
    }

    // Join timed out: the worker may still be inside ProcessFrame, which reads
    // g_hwndNotify and calls into KeyboardHook.  Leave its state alone rather
    // than pull it out from under a live thread — the same discipline
    // KeyboardHook::Uninstall applies to its own hook-thread state.
    if (!joined)
        return;

    g_hwndNotify = nullptr;
    g_registered.store(false, std::memory_order_relaxed);
}

void SetOptions(const Options& opts)
{
    g_optActivateMask.store(opts.activateMask, std::memory_order_relaxed);
    g_optCycleMask.store(opts.cycleMask, std::memory_order_relaxed);
    g_optCommitMask.store(opts.commitMask, std::memory_order_relaxed);
    g_optReverse.store(opts.reverse, std::memory_order_relaxed);
    g_optSensitivity.store(std::clamp(opts.sensitivity, 1, 100),
                           std::memory_order_relaxed);
    g_optSmoothing.store(std::clamp(opts.smoothing, 0, 100),
                         std::memory_order_relaxed);
    g_optCancelSwipe.store(opts.cancelSwipe, std::memory_order_relaxed);
    g_optContinuous.store(opts.continuous, std::memory_order_relaxed);
    g_optWindowSnap.store(opts.windowSnap, std::memory_order_relaxed);
    g_optEnabled.store(opts.enabled, std::memory_order_relaxed);

    // Cheap, and only on a config load — never on the gesture path.
    g_osTapsEnabled.store(ReadOsTapsEnabled(), std::memory_order_relaxed);

    if (HWND worker = g_hwndWorker.load(std::memory_order_acquire))
        PostMessageW(worker, WM_TP_REREGISTER, 0, 0);
}

} // namespace TouchpadHook
