#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DirectXMath.h>
#include "../render/Renderer.hpp"
#include "../render/QuadRenderer.hpp"
#include "../capture/WGCCapture.hpp"
#include "../capture/windowscanner.h"
#include "../capture/TaskbarButtonLocator.h"
#include "../scene/FlipScene.hpp"
#include "../animation/CycleAnimator.h"
#include "../animation/EntryExitAnimator.h"
#include "../animation/CloseAnimator.h"
#include "../animation/LabelAnimator.h"
#include "../animation/HoverAnimator.h"
#include "Config.h"
#include "SearchFilter.h"
#include "SearchBox.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <deque>
#include <unordered_map>
#include <cstdint>

/// D3D11 + WGC controller: replaces the DWM-thumbnail overlay with a full
/// 3D rendered Flip3D stack.
class FlipController {
public:
    FlipController() = default;
    ~FlipController() = default;

    FlipController(const FlipController&) = delete;
    FlipController& operator=(const FlipController&) = delete;

    bool Init(HINSTANCE hInstance);
    void Shutdown();

    void Activate();
    void Cycle();
    void CycleBack();
    void Dismiss();
    void Escape();
    void CycleStop();
    /// Free stack movement (config windowSnap = false).  `windows` is a
    /// FRACTIONAL number of windows, positive = forward, fed straight from
    /// pointer motion — the stack rides it and can sit between two windows.
    /// A no-op while Window snap is on, so the default path never sees it.
    void Scrub(float windows);
    /// The drag ended: ease onto whichever window is nearer.
    void ScrubEnd();

    // --- Pointer in the cascade (Controls → Mouse & keyboard) --------------
    // A third input source next to the keyboard and the touchpad, and just as
    // additive: every entry point below no-ops when the feature is off, and
    // none of them touch a path the other sources use.  Coordinates are
    // SCREEN pixels, straight from the low-level mouse hook.
    void PointerMove(int screenX, int screenY);
    void PointerSelect(int screenX, int screenY);
    void PointerClose(int screenX, int screenY);
    /// Delete: close the hovered window, or the selected one when the pointer
    /// is not over the cascade.
    void CloseSelectedWindow();

    // --- Type-to-filter (Settings → Search) --------------------------------
    void SearchAppend(wchar_t c);
    void SearchBackspace();
    /// True when there was a query to clear — the Escape path uses it to
    /// decide whether the key cancelled the search or the whole session.
    bool SearchClear();

    void RenderFrame();

    bool IsActive() const { return m_active; }

    /// Identity of the hook session this cascade belongs to (see
    /// KeyboardHook::CurrentSessionEpoch).  Exposed so an activation that
    /// found nothing to show can be given up BY IDENTITY, without disarming a
    /// session that started while it was still scanning.
    uint64_t SessionEpoch() const { return m_sessionEpoch; }

    void SetConfig(const AppConfig* cfg) { m_config = cfg; }

    /// Forget any auto-perf-tune degradation (called on config reload so a
    /// settings change gives the full pipeline another chance).
    void ResetPerfTune() { m_perfTier = 0; m_perfGoodWindows = 0; }

private:
    struct MonitorLayout {
        RECT virtualScreen{};  // SCREEN space
        RECT primary{};        // SCREEN space
        bool multiMonitor = false;
    };

    struct SecondaryTray {
        HWND hwnd{};
        RECT rectOverlay{};    // OVERLAY space, same convention as m_taskbarRect
        std::unique_ptr<WGCCapture> capture;
        bool contentResolved = false;
        float contentCenterY = 0.5f;
        bool wasVisible = false;
        winrt::com_ptr<ID3D11ShaderResourceView> frozenSRV;  // owned ref — must outlive WGC recreate
        // Hold-behind-overlay state (autohide continuity / live preview).
        bool extendedAtStart = false;  // autohide bar was slid out at Activate
        bool held = false;             // kept visible behind the overlay
        // Enforce the hold position every frame (autohide-continuity only).
        // A bar held solely for live preview skips it — its position is
        // irrelevant to the WGC stream, and fighting the shell's autohide
        // retraction costs a cross-process SetWindowPos per frame.
        bool heldPinPosition = false;
        bool liveActive = false;       // sample live WGC frames this session
        RECT holdRectScreen{};         // SCREEN-space window rect to pin
        RECT lastSeenRect{};           // last observed rect (motion detect)
    };

    static constexpr size_t     kMaxQueueSize = 3;       // max queued cycle commands
    static constexpr float      kBgAlpha      = 0.28f;   // background dimming opacity
    /// Upper bound on slots the hit test builds a draw-offset table for, on the
    /// stack.  SceneConfig::maxVisible is 10; the headroom means raising it
    /// costs nothing here, and exceeding it degrades to the pre-hover hit test
    /// (offsets skipped) rather than to anything unsafe.
    static constexpr uint32_t   kMaxHitTestSlots = 32;
    MonitorLayout BuildMonitorLayout() const;
    void UpdateCascadeSpace(float vpW, float vpH);
    void StartCaptures();
    void StopCaptures();
    void FinishDismiss();   // post-exit-morph teardown for Dismiss path
    void FinishEscape();    // post-exit-morph teardown for Escape path
    /// Re-open for an activation that arrived mid-exit (see Activate).
    void ResolveReactivation();
    /// Hand the session back to the hook as it ends, arming the modifier-
    /// release defusal when the commit came from something other than the
    /// keyboard.  Which it was is DERIVED from the hook's own flag — see the
    /// definition; carrying it as a remembered bool is what broke last time.
    void EndSessionForHook();
    void RebuildSceneAspects();
    /// Slot ↔ window-index mapping.  Identity for the cascade preset and
    /// whenever every window has a slot.  Cover Flow with MORE windows
    /// than slots maps the LEFT-side slots onto the TAIL of the window
    /// array so the visible row is a true circular carousel — the
    /// invisible pool sits between outer-right and outer-left, a forward
    /// cycle moves the centre window to the inner-left slot, drops the
    /// outermost-left window and surfaces a new one at outer-right.
    uint32_t SlotWindowIndex(uint32_t slot) const;
    /// Inverse mapping: which slot shows m_windows[windowIdx]; -1 when
    /// the window is in the invisible pool.
    int      WindowSlotIndex(size_t windowIdx) const;
    /// Explicit-parameter forms, for call sites that must evaluate the
    /// mapping against a PREVIOUS arrangement (e.g. the close transition
    /// comparing pre- and post-removal layouts).  `coverFlow=false` or
    /// `windowCount <= slotCount` yields the identity mapping, which is
    /// what the cascade preset always uses.
    static uint32_t SlotWindowIndexFor(uint32_t slot, uint32_t slotCount,
                                       size_t windowCount, bool coverFlow);
    static int      WindowSlotIndexFor(size_t windowIdx, uint32_t slotCount,
                                       size_t windowCount, bool coverFlow);
    /// Full permutation of window indices: slot-ordered visible windows
    /// first (index == slot), then the invisible pool in array order.
    /// Used to hand the entry/exit animator a window list whose index i
    /// pairs with cascade slot i (its internal pairing assumption).
    std::vector<size_t> SlotOrderIndices() const;
    /// m_windows permuted into slot order (see SlotOrderIndices).  An
    /// exact copy of m_windows for the cascade preset and whenever every
    /// window owns a slot, so those paths are unaffected.
    std::vector<WindowInfo> SlotOrderedWindows() const;
    void DeduplicateWindows();
    /// Drop windows whose owning executable is on the General-page
    /// exclusion list (config `excludedApps`, ';'-separated exe names or
    /// full paths).  Runs once per activation, right after the scan, so
    /// the excluded windows never take part in slot building, captures or
    /// any animation — the session behaves as if they didn't exist.
    void FilterExcludedWindows();
    void InjectDesktopWindow();
    void UpdateDesktopCaptureGeometry();
    void SortWindowsByProgram();
    void RemoveClosedWindows();
    /// Stop + release the frozen captures backing dying close-anim tiles.
    void ClearClosingCaptures();
    void CloakNewWindows();
    void ExecuteCycleForward();
    void ExecuteCycleForward(bool chained);
    void ExecuteCycleBackward();
    void ExecuteCycleBackward(bool chained);
    void ProcessCycleQueue();
    /// Roll a scrub step's array rotation back after the user dragged the
    /// stack below the step's own start pose.  Called at t = 0, so the
    /// rebuilt rest pose is pixel-identical to what is already on screen.
    void ScrubUndoStep(bool wasForward);
    /// Land an in-flight scrub on a whole window immediately (commit,
    /// cancel, or anything else that needs a settled stack).
    void ResolveScrub();
    // --- Pointer helpers ---------------------------------------------------
    /// Cascade slot under a SCREEN point, or -1.  Tests the settled scene
    /// geometry (scene/../input/TileHitTest), so it agrees with what is drawn
    /// frame for frame — including mid-cycle, when the tiles are moving.
    /// Returns -1 whenever the entry/exit morph owns the poses: the tiles are
    /// then somewhere between their flat and 3D transforms and only the draw
    /// pass knows where.
    /// Master gate for every pointer path (config `pointerInCascade`).  A
    /// non-null config is implied by a true result, so the callers can read
    /// the per-feature flags straight after it.
    bool PointerEnabled() const {
        return m_config != nullptr && m_config->pointerInCascade;
    }
    /// True only while the stack is STILL — nothing cycling, reflowing,
    /// coasting, spinning or morphing.
    ///
    /// Hover and clicking both need it.  A moving stack puts a different
    /// window under a motionless pointer every frame, so the highlight
    /// flickered from tile to tile as they swept past — and a click landed on
    /// whichever window happened to be crossing the cursor, not the one aimed
    /// at.  Holding the cycle key is the worst case of both: the stack streams
    /// past and nothing under the pointer means anything until it stops.
    bool PointerInteractionReady() const;
    int  HitTestScreen(int screenX, int screenY) const;
    /// The stack is about to move: send any raised tile back down on its way.
    ///
    /// Concurrent, never a gate.  The tile settles as the cascade carries it,
    /// and the transition starts on the same frame it always did — neither
    /// motion waits for the other, and cycle timing is untouched.  What keeps
    /// the fall honest is that the lift table rotates with the window arrays
    /// (HoverAnimator::Rotate), so the tile coming down is the one that was
    /// raised rather than whichever window inherited its slot.
    ///
    /// Does nothing at all when nothing is lifted, which is every frame of a
    /// session with the pointer feature switched off.
    void DropHoverLift();
    /// Commit the window in `slot`: the front slot dismisses straight away,
    /// anything further back first SPINS to the front and dismisses when it
    /// lands (see AdvanceSelectJump).
    void CommitSlot(uint32_t slot);
    /// One step of that spin, driven once per rendered frame.
    void AdvanceSelectJump();
    void CancelSelectJump();
    /// WM_CLOSE the window occupying `slot` (never the desktop tile).
    void CloseWindowAtSlot(int slot);
    /// Per-NEW-slot map of the old slot each window came from (-1 = it was
    /// not on screen and should spawn in from the back).  What CloseAnimator
    /// needs whenever the slot order is not a plain prefix of the old one.
    std::vector<int> BuildSlotSourceMap(const std::vector<HWND>& oldSlotHwnd) const;

    // --- Search ------------------------------------------------------------
    /// Re-run the filter: non-matching windows leave the stack through the
    /// close transition, matching ones that had left come back the way an
    /// overflow window arrives.  Deferred (m_searchDirty) whenever another
    /// animator owns the arrays, so a fast typist never loses an edit.
    void ApplySearchFilter();
    /// True while a typed query is the reason the stack is empty.
    ///
    /// An empty cascade normally means the session is over and RenderFrame
    /// escapes — but "no window matches what I typed" is a real answer, and
    /// showing it honestly (an empty stack under the query) is the whole
    /// point of searching.  One backspace brings the windows back, so the
    /// session must survive the emptiness rather than end on it.
    bool SearchHoldsEmptyStack() const;
    /// Merge every filtered-out window back into the stack, ordered as
    /// scrolling to the selection by hand would have left it, so the exit
    /// morph always gets the complete session.  The windows RISE back into
    /// place rather than appearing outright; true means that arrival is
    /// playing and the caller must hold its exit until it lands.
    bool RestoreSearchWindowsForExit();
    /// An exit waiting for that arrival to finish.
    enum class PendingExit { None, Dismiss, Escape };
    PendingExit                  m_pendingExit = PendingExit::None;
    /// Resolve each window's executable name once per session (search only —
    /// with the feature off no process is ever opened).
    void BuildWindowMetadata();
    void ClearSearchState();
    void UpdateSearchBox();
    void DrawSearchBox(ID3D11DeviceContext* ctx, float vpW, float vpH);

    void HideDesktopIcons();
    void RestoreDesktopIcons();
    /// Capture backing the wallpaper backdrop: the desktop tile's capture
    /// when the tile is enabled, or the dedicated m_wallpaperCapture when
    /// the desktop tile is disabled (config showDesktopTile = false).
    WGCCapture* WallpaperCaptureSource();
    /// Wallpaper backdrop SRV honouring the Live background toggle
    /// (config liveBackground).  Live (default): sample the running WGC
    /// capture every frame — animated wallpapers keep playing, cycle
    /// animation included.  Off: a dedicated GPU copy of the first
    /// delivered frame serves the whole session (a live desktop tile
    /// updates the SHARED cached texture in place via GetCurrentFrame, so
    /// only an owned copy is genuinely static).  May return nullptr while
    /// the capture has no frame yet.
    ID3D11ShaderResourceView* BackdropSRV();
    /// Selected-window label (front slot): rebuild the GDI-rendered
    /// title+icon texture when the selection/config changed, and draw it
    /// as a screen-space pill anchored above the front tile.
    void UpdateSelectedLabel();
    bool BuildSelectedLabelTexture(HWND hwnd, const std::wstring& title,
                                   bool showTitle, bool showIcon,
                                   bool showBox);
    void DrawSelectedLabel(ID3D11DeviceContext* ctx, float vpW, float vpH,
                           DirectX::XMMATRIX monRemap);
    void ResetSelectedLabel();
    void StartTaskbarCapture();
    void HideRealTaskbar();
    void ShowRealTaskbar();
    /// Re-assert position + below-overlay Z order of taskbars held visible
    /// behind the overlay (autohide continuity / taskbar live preview).
    void PinHeldTaskbars();

    Renderer                     m_renderer;
    QuadRenderer                 m_quad;
    FlipScene                    m_scene;
    CycleAnimator                m_cycleAnim;
    EntryExitAnimator            m_entryExitAnimator;
    // Close transition: animates the stack reflow when a window is closed
    // while the cascade is up (see RemoveClosedWindows).  The dying tiles
    // are drawn from m_closingCaptures — the closed windows' capture
    // objects, moved out of m_captures so their frozen last frames stay
    // alive for the fade-out (parallel to CloseAnimator::GetDyingSlots).
    CloseAnimator                m_closeAnim;
    std::vector<std::unique_ptr<WGCCapture>> m_closingCaptures;
    // Frozen last frame of each dying tile, parallel to the animator's dying
    // list.  Owned refs, because a tile can outlive its capture: a window
    // hidden by the search filter keeps streaming in m_searchHidden (it may
    // come straight back), so its fade-out is drawn from a snapshot rather
    // than from a capture this list took ownership of.
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> m_closingSRVs;
    bool                         m_exitPending     = false;
    bool                         m_exitFromEscape  = false;
    // The hotkey was pressed again while this session was morphing out.  The
    // hook counts that as a new session the instant it happens, so the
    // controller has to actually open one — see Activate / ResolveReactivation.
    bool                         m_reactivatePending = false;
    // Mid-entry reverse defers actual ReverseInPlace by kReverseDelayMs so
    // the entry morph keeps playing for ~3 frames after key release before
    // it folds back.  Cures the "press-and-instantly-let-go" flicker where
    // an entry that has barely started would snap to the equivalent late-
    // exit pose on the next tick.  Cleared by RenderFrame once the delay
    // elapses (→ ReverseInPlace) or the entry finishes naturally during
    // the delay (→ standard BeginExit path).
    bool                         m_reverseDelayPending     = false;
    bool                         m_reverseDelayFromEscape  = false;
    LARGE_INTEGER                m_reverseDelayStartQPC{};
    static constexpr double      kReverseDelayMs = 135.0;

    std::vector<WindowInfo>      m_windows;
    std::unordered_map<HWND, uint32_t> m_originalZOrder;  // raw OS Z-rank snapshot, captured at Activate
    std::vector<std::unique_ptr<WGCCapture>>  m_captures;
    std::deque<bool>             m_cycleQueue;  // true=forward, false=backward

    // --- Free stack movement (config windowSnap = false) -------------------
    // A scrub is an ordinary cycle transition whose parameter comes from the
    // pointer instead of the clock.  m_scrubT is the position inside the
    // step currently in flight (0 = its start pose, 1 = its destination);
    // crossing either end commits that step and starts the next one, so a
    // long drag walks the whole stack.  m_scrubPending tells the shared
    // ExecuteCycle* path to open the transition in scrub mode.
    bool                         m_scrubActive  = false;
    bool                         m_scrubForward = true;
    float                        m_scrubT       = 0.0f;
    bool                         m_scrubPending = false;
    // Throwing the stack: the drag's velocity (windows per second, positive =
    // forward) is measured while dragging and, on release, carries the stack
    // on under friction instead of stopping dead under the hand.
    float                        m_scrubVelocity   = 0.0f;
    float                        m_scrubPendingDist = 0.0f;  // travel since the last sample
    LARGE_INTEGER                m_scrubSampleQPC{};
    bool                         m_flinging        = false;
    LARGE_INTEGER                m_flingLastQPC{};
    static constexpr float       kFlingMinVel   = 1.6f;   // windows/s worth throwing
    static constexpr float       kFlingStopVel  = 0.7f;   // windows/s to settle at
    static constexpr float       kFlingMaxVel   = 16.0f;  // cap a jerk
    static constexpr float       kFlingTauSec   = 0.42f;  // friction time constant
    // Above this the stack is moving too fast for the selected-window label
    // to be readable, so it fades — the free-drag counterpart of a held key.
    static constexpr float       kLabelHoldVel  = 2.5f;   // windows/s
    // A step this far in, still travelling this fast, FINISHES instead of
    // rewinding when the throw runs out (see ScrubSettleTarget).
    static constexpr float       kSettleCarryT   = 0.15f;
    static constexpr float       kSettleCarryVel = 0.55f;  // windows/s
    /// The scrub velocity in the current step's own frame, and where that
    /// step should come to rest.
    float ScrubStepVelocity() const;
    float ScrubSettleTarget() const;
    /// Advance an in-flight scrub by a fractional number of windows.  Shared
    /// by the pointer path and the throw, so both walk the stack identically.
    void ScrubAdvance(float windows);
    void ScrubTickFling();

    bool                         m_active         = false;
    /// Which hook session this cascade belongs to, taken at Activate.
    ///
    /// The teardown runs long and pumps no messages, so a hotkey pressed part
    /// way through it opens the NEXT session before this one has finished
    /// leaving.  Handing the identity back (KeyboardHook::EndSessionIfEpoch)
    /// instead of blindly clearing the flag is what stops the teardown from
    /// disarming a session that has already begun.
    uint64_t                     m_sessionEpoch   = 0;
    bool                         m_activatedOnDesktop = false; // FG was Progman/WorkerW at Activate
    HWND                         m_desktopHwnd    = nullptr;  // Progman/WorkerW
    // Desktop tile disabled for THIS session (config showDesktopTile
    // latched at Activate so a mid-session config reload can't desync the
    // window/capture arrays).  The wallpaper backdrop then comes from the
    // dedicated capture below instead of a desktop tile's capture.  One
    // long-lived object, restarted per session (Stop() keeps the cached
    // frame as a warm start) — no per-session texture accumulation, and
    // WGC's pool-recreate path absorbs resolution changes.
    bool                         m_desktopTileDisabled = false;
    std::unique_ptr<WGCCapture>  m_wallpaperCapture;
    // Selected-window label (front slot): GDI-rendered premultiplied BGRA
    // pill texture (title + program icon), rebuilt only when the selected
    // HWND / title / part-selection changes.
    winrt::com_ptr<ID3D11Texture2D>          m_labelTexture;
    winrt::com_ptr<ID3D11ShaderResourceView> m_labelSRV;
    HWND                         m_labelHwnd = nullptr;
    std::wstring                 m_labelTitle;
    bool                         m_labelShowTitle = true;
    bool                         m_labelShowIcon  = true;
    bool                         m_labelShowBox   = true;
    int                          m_labelTheme     = 0;   // appTheme the texture was built for
    int                          m_labelTexW = 0;
    int                          m_labelTexH = 0;
    // Smooths the label anchor between the projected front-slot bounds of
    // successive frames — differently sized windows swapping through the
    // front slot glide instead of teleporting (animation/LabelAnimator).
    LabelAnimator                m_labelAnim;

    // --- Pointer in the cascade --------------------------------------------
    // The hover lift is a DRAW offset (see HoverAnimator) — it owns no slot
    // state, so it composes with whichever animator happens to be running.
    HoverAnimator                m_hover;
    int                          m_hoverSlot     = -1;   // -1 = pointer elsewhere
    // When the stack last became still (0 = it is not).  The lift waits this
    // long before rising, so the stack's own transitions and the lift can never
    // end up queueing behind each other — see the hover block in RenderFrame.
    LARGE_INTEGER                m_hoverStillQPC{};
    static constexpr double      kHoverRiseHoldMs = 120.0;
    POINT                        m_pointerScreen{};
    bool                         m_pointerValid  = false;
    // Click-to-select spin.  The target is remembered by HWND, not by index:
    // the stack can be re-ordered underneath it (a window closes, the search
    // filter changes) and the spin still knows exactly what it is chasing —
    // or that the window is gone and the whole thing must be abandoned.
    HWND                         m_jumpTargetHwnd = nullptr;
    bool                         m_jumpCommit     = false;
    float                        m_jumpStepMs     = 0.0f;
    // The spin as a whole gets a fixed budget, so nine windows away is not
    // nine times the wait; the per-step bounds keep one step from being a
    // strobe or a crawl.
    static constexpr float       kJumpBudgetMs  = 380.0f;
    static constexpr float       kJumpStepMinMs = 55.0f;
    static constexpr float       kJumpStepMaxMs = 170.0f;

    // --- Search (type-to-filter) -------------------------------------------
    SearchFilter                 m_search;
    SearchBox                    m_searchBox;
    bool                         m_searchDirty   = false;  // an edit is waiting for a settled stack
    bool                         m_searchNoMatch = false;  // query typed but not applied
    /// Windows the filter has taken out of the stack.  Their captures keep
    /// running: the next keystroke may well bring them straight back, and a
    /// stop/restart round trip per character would cost far more than the
    /// frames they quietly deliver in the meantime.
    struct HiddenWindow {
        WindowInfo                  win;
        std::unique_ptr<WGCCapture> capture;
        size_t                      order = 0;   // session scan order
    };
    std::vector<HiddenWindow>    m_searchHidden;
    /// Per-window facts that survive the array rotations — keyed by HWND
    /// precisely so nothing has to be kept parallel to m_windows.
    struct WindowMeta {
        size_t       order = 0;      // position in the session's scan order
        std::wstring exeLower;       // owning executable's file name
    };
    std::unordered_map<HWND, WindowMeta> m_windowMeta;

    HWND                         m_iconListView   = nullptr;  // Desktop SysListView32
    bool                         m_iconsWereVisible = false;  // Restore icons on dismiss
    std::unique_ptr<WGCCapture>  m_taskbarCapture;            // Live WGC for Shell_TrayWnd
    HWND                         m_taskbarHwnd = nullptr;     // Shell_TrayWnd
    RECT                         m_taskbarRect{};              // OVERLAY-space taskbar rect
    std::vector<SecondaryTray>   m_secondaryTrays;
    bool                         m_taskbarContentResolved = false; // v8.5: content-band UV crop resolved
    float                        m_taskbarContentCenterY  = 0.5f;  // v8.5: UV.y centre of taskbar content band
    TaskbarButtonLocator         m_taskbarLocator;             // UIA per-button rect lookup
    bool                         m_taskbarWasVisible = false;  // Was taskbar visible before hide
    bool                         m_taskbarDrawOnTop = false;   // Autohide taskbar overlays windows in DWM
    // Hold-behind-overlay session state (see HideRealTaskbar):
    //   - autohide bar that was slid out at activation is kept visible
    //     (disabled + pinned below the overlay) so it is still extended the
    //     instant the overlay hides — the shell then retracts it with its
    //     own animation instead of leaving a sudden gap.
    //   - taskbar live preview holds the bar the same way so the WGC stream
    //     keeps delivering real frames for the whole session.
    bool                         m_taskbarAutoHide = false;        // ABS_AUTOHIDE at Activate
    bool                         m_taskbarExtendedAtStart = false; // autohide bar slid out at Activate
    bool                         m_taskbarHeld = false;            // bar kept visible behind overlay
    bool                         m_taskbarHeldPinPosition = false; // enforce hold position per frame (continuity holds only)
    bool                         m_taskbarLiveActive = false;      // live taskbar sampling this session
    RECT                         m_taskbarHoldRectScreen{};        // SCREEN-space pin rect
    RECT                         m_taskbarLastSeenRect{};          // last observed rect (motion detect)
    uint32_t                     m_heldPinCounter = 0;             // periodic Z re-assert divider
    // Frozen refs own the SRV (AddRef via com_ptr): WGCCapture recreates its
    // cached SRV on any size change, so a raw pointer here could dangle
    // mid-animation and issue a draw call on a freed GPU view.
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> m_frozenStartSRVs; // SRVs captured BEFORE array rotate
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> m_frozenTargetSRVs; // SRVs captured AFTER array rotate
    winrt::com_ptr<ID3D11ShaderResourceView> m_frozenDesktopSRV; // Frozen wallpaper SRV
    winrt::com_ptr<ID3D11ShaderResourceView> m_frozenTaskbarSRV; // Frozen taskbar SRV
    // Live background OFF: owned per-session snapshot of the wallpaper
    // (see BackdropSRV) — a plain SRV ref would not be static because
    // GetCurrentFrame copies new frames into the same cached texture.
    winrt::com_ptr<ID3D11Texture2D>          m_staticBackdropTexture;
    winrt::com_ptr<ID3D11ShaderResourceView> m_staticBackdropSRV;
    bool                         m_sessionFrozen = false;      // True while animation is active
    winrt::com_ptr<ID3D11Texture2D> m_exitSelectedStableTexture; // Owned copy for selected minimized exit tile
    winrt::com_ptr<ID3D11ShaderResourceView> m_exitSelectedStableSRV;
    HWND                         m_exitSelectedStableHwnd = nullptr;
    std::unordered_map<HWND, std::unique_ptr<WGCCapture>> m_captureCache; // Warm cache
    HINSTANCE                    m_hInstance      = nullptr;
    const AppConfig*             m_config         = nullptr;
    MonitorLayout                m_monLayout{};
    float                        m_cascadeW = 1920.0f;
    float                        m_cascadeH = 1080.0f;
    float                        m_cascadeAspect = 16.0f / 9.0f;
    float                        m_overlayOriginX = 0.0f;
    float                        m_overlayOriginY = 0.0f;
    RECT                         m_desktopBackdropRect{};     // SCREEN-space Progman/WorkerW rect
    DirectX::XMFLOAT4            m_desktopTileUV{0.0f, 0.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT4X4          m_monRemapNDC{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    // Performance monitoring
    static constexpr size_t      kPerfSampleCount = 60;
    double                       m_frameTimes[kPerfSampleCount] = {};
    size_t                       m_frameTimeIdx   = 0;
    size_t                       m_frameTimeCount = 0;
    LARGE_INTEGER                m_perfFreq{};
    LARGE_INTEGER                m_lastFrameTime{};

    // --- Auto performance tune ---------------------------------------------
    // Runtime quality ladder driven by measured frame times (config
    // autoPerfTune + perfProfile -1).  Tier 0 = full quality,
    // 1 = motion blur off, 2 = + antialiasing off, 3 = + live preview off
    // (next activation captures static snapshots instead of WGC sessions).
    // Two-way with hysteresis: a sample window >1.35× over budget steps a
    // tier down; kPerfRecoveryWindows consecutive windows <0.85× under
    // budget step back up.  The budget is floored at 60 Hz equivalent —
    // running below a 144/165 Hz native refresh is not a reason to strip
    // quality as long as the cascade holds ~60 fps.  ResetPerfTune()
    // clears the ladder on config reload.
    static constexpr double      kMinTuneBudgetMs = 1000.0 / 60.0;
    static constexpr int         kPerfRecoveryWindows = 3;
    int                          m_perfTier = 0;
    int                          m_perfGoodWindows = 0;   // consecutive under-budget windows
    double                       m_refreshBudgetMs = 1000.0 / 60.0;
    /// Quality actually used this frame: combines the user toggles, the
    /// manual perfProfile override (0=low,1=medium,2=high) and the
    /// auto-tune tier (profile -1).
    bool EffectiveMotionBlur() const;
    bool EffectiveAntialiasing() const;
    bool EffectiveLivePreview() const;
    /// Glass floor reflections actually drawn this frame (config
    /// `reflections` plus the perf ladder — they are the first optional
    /// effect dropped, sharing tier 1 with motion blur).
    bool EffectiveReflections() const;
    /// Per-animation gates: master `animations` switch AND the animation's
    /// own selection flag (Appearance → Animations dropdown).  No config
    /// loaded (nullptr) defaults to animated, matching the AppConfig
    /// defaults.
    bool AnimEntryExitEnabled() const;
    bool AnimCycleEnabled() const;
    bool AnimCloseEnabled() const;
    bool AnimLabelEnabled() const;
    bool AnimHoverEnabled() const;
    /// Activation warm-up budget in ms (config startDelayMs; auto perf
    /// tune substitutes a value derived from refresh rate + perf tier).
    uint32_t EffectiveStartDelayMs() const;
};
