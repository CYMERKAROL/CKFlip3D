#define NOMINMAX
#include "TaskbarButtonLocator.h"

#include <objbase.h>
#include <oleauto.h>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// MSAA-based per-button locator.
//
// Notes about COM/apartment management:
//   We deliberately do NOT call OleInitialize / OleUninitialize here.  The
//   main thread's apartment is already set up by the rest of the app (WGC
//   uses C++/WinRT which initialises the apartment as MTA), and pairing
//   Init/Shutdown to per-session OleInit/OleUninit destabilised that
//   apartment between sessions — the second activation deadlocked on the
//   internal SendMessage inside AccessibleObjectFromWindow.
//
//   Instead we use SendMessageTimeoutW(WM_GETOBJECT) + ObjectFromLresult
//   so a slow / unresponsive Explorer cannot hang our UI thread.  All
//   subsequent IAccessible calls are made through the cached interface,
//   which marshals through whichever apartment the thread already has.
// ---------------------------------------------------------------------------

namespace {

constexpr DWORD kSendTimeoutMs = 250;   // hard cap on cross-process WM_GETOBJECT

struct FindCtx { HWND result; };

BOOL CALLBACK FindTaskListEnum(HWND child, LPARAM lp)
{
    FindCtx* ctx = reinterpret_cast<FindCtx*>(lp);
    WCHAR cls[64] = {};
    if (GetClassNameW(child, cls, static_cast<int>(_countof(cls))) > 0) {
        if (lstrcmpiW(cls, L"MSTaskListWClass") == 0
         || lstrcmpiW(cls, L"MSTaskListClass") == 0) {
            ctx->result = child;
            return FALSE;
        }
    }
    return TRUE;
}

HWND FindTaskListHwnd(HWND tray)
{
    if (!tray) return nullptr;
    FindCtx ctx{ nullptr };
    EnumChildWindows(tray, FindTaskListEnum, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// Bootstrap an IAccessible for a window without using AccessibleObjectFromWindow
// (which uses a non-timeout SendMessage internally and can deadlock when
// Explorer is slow).  Returns S_OK + AddRef'd interface on success.
HRESULT GetAccessibleForWindowSafe(HWND hwnd, IAccessible** out)
{
    if (out) *out = nullptr;
    if (!hwnd || !out) return E_INVALIDARG;

    DWORD_PTR result = 0;
    LRESULT lr = SendMessageTimeoutW(
        hwnd, WM_GETOBJECT, 0, OBJID_CLIENT,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, kSendTimeoutMs, &result);
    if (lr == 0) return E_FAIL;          // timed out / window unresponsive
    if (result == 0) return E_FAIL;       // window doesn't expose IAccessible

    return ObjectFromLresult(static_cast<LRESULT>(result),
                             IID_IAccessible, 0,
                             reinterpret_cast<void**>(out));
}

bool ReadName(IAccessible* acc, LONG childId, BSTR& outBstr)
{
    VARIANT vChild{};
    vChild.vt   = VT_I4;
    vChild.lVal = childId;
    BSTR name = nullptr;
    if (FAILED(acc->get_accName(vChild, &name))) return false;
    outBstr = name;
    return name != nullptr;
}

bool ReadLocation(IAccessible* acc, LONG childId, RECT& outRect)
{
    VARIANT vChild{};
    vChild.vt   = VT_I4;
    vChild.lVal = childId;
    long x = 0, y = 0, w = 0, h = 0;
    if (FAILED(acc->accLocation(&x, &y, &w, &h, vChild))) return false;
    if (w <= 0 || h <= 0) return false;
    outRect = RECT{ x, y, x + w, y + h };
    return true;
}

bool VariantToChildId(const VARIANT& v, LONG& outChildId)
{
    if (v.vt != VT_I4) return false;
    outChildId = v.lVal;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
bool TaskbarButtonLocator::Init()
{
    if (IsReady()) return true;
    Shutdown();

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;
    HWND list = FindTaskListHwnd(tray);
    if (!list) return false;

    // Cache the HWND first so GetButtonListRect works even if the
    // IAccessible bootstrap fails (Win11 XAML taskbars frequently return
    // nothing useful from MSAA).  Per-button matching just won't work
    // in that case — the caller will treat the button list rect as the
    // shared emerge point for all minimized windows.
    m_buttonListHwnd = list;

    IAccessible* acc = nullptr;
    if (SUCCEEDED(GetAccessibleForWindowSafe(list, &acc)) && acc)
        m_buttonListAcc = acc;

    return true;   // ready in HWND-only mode at minimum
}

// ---------------------------------------------------------------------------
void TaskbarButtonLocator::Shutdown()
{
    if (m_buttonListAcc) {
        m_buttonListAcc->Release();
        m_buttonListAcc = nullptr;
    }
    m_buttonListHwnd = nullptr;
    // m_oleInitialized stays false — we never own OLE.  Kept in the struct
    // for header ABI compat; harmless.
    m_oleInitialized = false;
}

// ---------------------------------------------------------------------------
bool TaskbarButtonLocator::GetButtonListRect(RECT& outRect) const
{
    if (!m_buttonListHwnd) return false;
    RECT r{};
    if (!GetWindowRect(m_buttonListHwnd, &r)) return false;
    if (r.right <= r.left || r.bottom <= r.top) return false;
    outRect = r;
    return true;
}

// ---------------------------------------------------------------------------
bool TaskbarButtonLocator::GetButtonRect(HWND hwnd, RECT& outRect)
{
    if (!HasIAccessible() || !hwnd) return false;

    WCHAR title[256] = {};
    GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
    if (!title[0]) return false;

    long childCount = 0;
    if (FAILED(m_buttonListAcc->get_accChildCount(&childCount)) || childCount <= 0)
        return false;

    constexpr long kMaxChildren = 256;
    if (childCount > kMaxChildren) childCount = kMaxChildren;
    VARIANT children[kMaxChildren] = {};
    long fetched = 0;
    HRESULT hr = AccessibleChildren(m_buttonListAcc, 0, childCount, children, &fetched);
    if (FAILED(hr) || fetched <= 0) return false;

    bool found = false;
    for (long i = 0; i < fetched && !found; ++i) {
        VARIANT& v = children[i];
        IAccessible* childAcc = m_buttonListAcc;
        LONG childId = CHILDID_SELF;
        bool releaseChild = false;

        if (v.vt == VT_DISPATCH && v.pdispVal) {
            IAccessible* a = nullptr;
            if (SUCCEEDED(v.pdispVal->QueryInterface(IID_IAccessible,
                                                     reinterpret_cast<void**>(&a)))
                && a) {
                childAcc = a;
                childId = CHILDID_SELF;
                releaseChild = true;
            }
        } else if (!VariantToChildId(v, childId)) {
            VariantClear(&v);
            continue;
        }

        BSTR name = nullptr;
        if (ReadName(childAcc, childId, name) && name) {
            if (wcsstr(name, title) || wcsstr(title, name)) {
                if (ReadLocation(childAcc, childId, outRect))
                    found = true;
            }
            SysFreeString(name);
        }

        if (releaseChild) childAcc->Release();
        VariantClear(&v);
    }

    for (long i = 0; i < fetched; ++i)
        VariantClear(&children[i]);

    return found;
}
