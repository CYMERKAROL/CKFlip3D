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
#include "Config.h"
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
    void RenderFrame();

    bool IsActive() const { return m_active; }

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
    MonitorLayout BuildMonitorLayout() const;
    void UpdateCascadeSpace(float vpW, float vpH);
    void StartCaptures();
    void StopCaptures();
    void FinishDismiss();   // post-exit-morph teardown for Dismiss path
    void FinishEscape();    // post-exit-morph teardown for Escape path
    void RebuildSceneAspects();
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
    bool                         m_exitPending     = false;
    bool                         m_exitFromEscape  = false;
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

    bool                         m_active         = false;
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
    /// Per-animation gates: master `animations` switch AND the animation's
    /// own selection flag (Appearance → Animations dropdown).  No config
    /// loaded (nullptr) defaults to animated, matching the AppConfig
    /// defaults.
    bool AnimEntryExitEnabled() const;
    bool AnimCycleEnabled() const;
    bool AnimCloseEnabled() const;
    bool AnimLabelEnabled() const;
    /// Activation warm-up budget in ms (config startDelayMs; auto perf
    /// tune substitutes a value derived from refresh rate + perf tier).
    uint32_t EffectiveStartDelayMs() const;
};
