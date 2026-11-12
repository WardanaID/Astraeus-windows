#include "drawer.h"
#include "deskpill.h"
#include "board.h"
#include "entry.h"

#include "faces.h"
#include "glyphs.h"
#include "notifs.h"
#include "starfall.h"

#include <dwmapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace {

constexpr UINT kGreetDelayMs = 1500, kGreetPollMs = 100; // the desktop settles before the greeting drops; then a cheap look at the cursor while it's up
constexpr int kGreetTries = 20; // the gpu or the shell not ready yet: a few more seconds of tries
constexpr LONG kGreetSlack = 2; // pixels the cursor may drift (a touchpad's jitter) before it counts as moving
constexpr UINT kTrimMs = 3000; // a resting pill this long after its last whole frame: hand back what startup and the drop left behind
constexpr UINT kFollowMs = 120; // every screen mode, open: how often to look which screen the cursor is on
constexpr int kDevTries = 30; // windows lets go of an old composition target a moment later: ~3 seconds of retries, 100 ms apart
constexpr GUID kDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}}; // GUID_CONSOLE_DISPLAY_STATE, without pulling in initguid

bool PillStays(const Model& m) { // the pill holds still by itself
    if (m.pillAlways) return true; // always shown, with the clock
    if (m.activityBubble && m.Activity() != Model::kActNone) return true; // a running timer lives beside it
    return m.IslandAlways() && m.pillMusic && m.hasTrack && m.track.playing; // always means: while something is playing
}
constexpr int kCityMax = 60; // the weather city field
constexpr float kFling = 800; // dips/s, fast flick decides open/close
constexpr float kOpenAt = 0.4f; // slow drag past 40% height opens
constexpr float kTap = 4; // moved less than this = click, not drag
constexpr int kShadeSecs = 30; // glass: how often what's behind it is looked at again, as halobar does
constexpr int kDeskRecheckSecs = 2; // only with an over-apps theme or body in use: a walk of the top windows
constexpr int kInkSecs = 2; // clear: its ink has more riding on it, and a thin band is a cheap read
constexpr float kShadeBand = 8, kShadeGap = 4; // the band read just past the open panel, dips
constexpr float kShadeStep = 0.04f; // smaller changes keep the tint as it is
constexpr float kShadeRate = 5; // the tint eases over in about a second
constexpr double kSkyWrap = 100000; // seconds before the sky's clock wraps: float keeps ~10 ms precision that far
constexpr float kOrbFps = 20; // starfall's drifting orb: frames per second while it's the only thing moving

double Seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

}

Drawer::Drawer(const Config& c, Model& m) : cfg(c), model(m) {
    page = prevPage = model.OpenPage(); // where it left off, or the page picked in settings
    zoomNow = model.ZoomPct();
    W = cfg.panW + 2 * cfg.fillet; // room for curved shoulders on both sides
    H = cfg.panH;
    accentModeNow = model.AccentMode();
    accentHexNow = model.AccentHex();
    shellSeed = mediaSeed = accentModeNow ? theme::FromCol(Color::Rgb((uint32_t)accentHexNow)) : Seed{}; // wallpaper theme fills in later
    lookNow = model.Look();
    bgNow = model.CustomBg();
    edgeNow = model.Edge();
    screensNow = model.screens;
    screenNow = model.screen;
    layNow = model.layout;
    pinNow = model.pinned != 0;
    floatNow = model.floating != 0;
    shellFrom = shellTo = shellNow = mediaFrom = mediaTo = mediaNow = Make(shellSeed);
}

Drawer::~Drawer() {
    UnregisterHotKey(hwnd, kBoardKey);
    boards.clear();
    Reserve(false); // the screen's room goes back to the windows
    UnwatchDesk();
    StopShelf();
    volKeys.Stop(); // the volume keys go back to windows at once
    if (acNote) UnregisterPowerSettingNotification(acNote);
    if (batNote) UnregisterPowerSettingNotification(batNote);
    FreeStrips();
    FreeDev();
    if (displayNote) UnregisterPowerSettingNotification(displayNote);
    if (hwnd) {
        WTSUnRegisterSessionNotification(hwnd);
        DestroyWindow(hwnd);
    }
}

bool Drawer::Create(HINSTANCE instance) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AstraeusDrawer";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, // no focus steal, no gdi surface (dcomp only)
                           wc.lpszClassName, L"Astraeus", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this); // this -> WM_NCCREATE
    if (!hwnd) return false;
    access.Attach(hwnd, L"Astraeus panel");
    BOOL off = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off)); // no dwm fade, we animate ourselves
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION); // locked: the pill's bars rest, nobody sees them
    displayNote = RegisterPowerSettingNotification(hwnd, &kDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE); // and the same while the screen is off
    StartShelf(); // files and text dropped on the island land on the shelf
    if (!RegisterHotKey(hwnd, kBoardKey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D')) // a dashboard's screen back to the desktop and again; win+alt+d is windows' own date flyout
        Log(L"ctrl+alt+d is taken by another app: the dashboard's own button still switches it");
    RegisterHotKey(hwnd, kBoardFocusKey, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'D'); // the keyboard into a dashboard; taken: it stays mouse only
    WatchPower(); // plugged in, unplugged, running low: windows says so, nothing polls
    SyncHud();
    if (!screensNow) mon = sys::FindScreen(screenNow); // the picked screen, primary if none or unplugged
    Place();
    BuildStrips();
    SyncWall();
    shellNow = shellTo; // start on final colors, no fade at boot
    mediaNow = mediaTo;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    desk = std::make_unique<DeskPill>(model);
    if (desk->Create()) {
        desk->onOpen = [this] { Open(); };
        desk->onEdit = [this] { DeskEdit(); };
        desk->onDone = [this] {
            SyncHide();
            if (deskOn != model.WatchesDesk()) WatchDesk();
            else OnDeskTimer();
        };
        desk->onLook = [this] { SyncThemeNow(); };
        desk->onStudioDone = [this] { // back to the panel it was opened from
            SyncHide();
            if (deskOn) OnDeskTimer();
            int back = model.studioKind == Model::kStudioTiles ? Model::kPgTools : model.studioKind == Model::kStudioWidgets ? Model::kPgWidgets : page; // the rail's was opened over any page
            if (page != back) prevPage = page, page = back, pageT = 1;
            pageSet = true;
            Open();
            if (model.studioToSettings) model.studioToSettings = false, model.OpenSettings(), Redraw();
        };
    } else {
        desk.reset();
    }
    Log(L"started");
    return true;
}

void Drawer::ShowIsland() {
    if (zoomActive || opened || peeking || hidden) return;
    if (desk && model.deskApps && !(deskView && !model.LongPill()) && !hidden && !opened && !peeking) { // over apps the long pill takes this call: the small one never drops. the desktop keeps its own form
        longHeld = true;
        SyncDesk();
        SetTimer(hwnd, kLongTimer, model.IslandLingerMs((UINT)cfg.closeMs), nullptr);
        return;
    }
    if (WantLongPill() && !desk->Editing()) { // desktop is handled by the long pill: never drop the compact pill behind it
        SyncDesk();
        return;
    }
    if (model.Glassy()) SampleBehind();
    SampleInk();
    if (hidden || opened || peeking || zoomActive || !EnsureDev()) return;
    KillTimer(hwnd, kReleaseTimer); // keep gpu alive
    KillTimer(hwnd, kCloseTimer);
    KillTimer(hwnd, kPeekExpandTimer);
    bool wasRetracting = peekRetracting;
    if (!wasRetracting) {
        peekLook = peekToastNow = peekFromPill = false;
        peekRetractH = kNotifOneH;
        peekStack.clear();
        peekIdx = 0;
        peekBmp.Reset();
    } else {
        peekLook = peekToastNow = true;
        peekFromPill = false;
    }
    peekHot = peekCloseHot = peekExpanded = peekPrevHot = peekNextHot = peekClearHot = false;
    closingToPeek = false;
    menuCloseAlpha = 0.0f;
    menuCloseScale = 1.0f;
    menuPeekSpring = anim::Spring{};
    islandVizHot = false;
    islandCtlBtn = -1;
    islandCtlT = 0.0f;
    zoomActive = zoomFromPill = zoomToPill = zoomDrawPill = false;
    zoomAlpha = zoomInvScale = 1.0f;
    zoomBodyXform = D2D1::Matrix3x2F::Identity();
    peekHoverRow = peekCloseRow = -1;
    peekShownRows = 0;
    island = islandLook = true;
    model.meter.Refresh(); // bind the level meter to the current output for the mini visualizer
    float keepH = wasRetracting ? (std::max)(kIslandH, spring.value) : kIslandH;
    Resize(keepH + overshoot, true); // room for the bounce or retracting card, trimmed once it settles
    spring.target = kIslandH;
    spring.stiff = model.reduceMotion ? 360.0f : (wasRetracting ? 245.0f : 300.0f);
    spring.damp = model.reduceMotion ? 1.0f : (wasRetracting ? 0.82f : 0.66f); // liquid settle when returning from notification
    widthSpring.target = IslandSpan();
    widthSpring.stiff = model.reduceMotion ? 360.0f : (wasRetracting ? 235.0f : 280.0f);
    widthSpring.damp = model.reduceMotion ? 1.0f : (wasRetracting ? 0.82f : 0.74f);
    if (widthSpring.value < 1.0f) widthSpring.value = IslandSpan();
    Redraw();
}

D2D1_RECT_F Drawer::IslandVizZone(float h) const {
    float hh = (std::max)(h, kIslandH);
    if (Side()) {
        float inner = kIslandSide - 2 * cfg.fillet - 20, mid = winW / 2;
        float dir = edgeNow == 1 ? -1.0f : 1.0f;
        float end = mid + dir * inner / 2;
        float a = end - dir * (islandVizHot ? 74.0f : 32.0f), b = end + dir * 10.0f;
        return {0.0f, (std::min)(a, b), hh, (std::max)(a, b)};
    }
    float inner = kIslandW - 2 * cfg.fillet - 20;
    float x1 = winW / 2 + inner / 2;
    return islandVizHot ? Rect(x1 - 74.0f, 0.0f, 84.0f, hh) : Rect(x1 - 32.0f, 0.0f, 42.0f, hh);
}

D2D1_RECT_F Drawer::IslandCtlRect(int idx, float h) const {
    float hh = (std::max)(h, kIslandH);
    if (idx < 0 || idx > 2) return D2D1_RECT_F{};
    if (Side()) {
        float inner = kIslandSide - 2 * cfg.fillet - 20, mid = winW / 2;
        float dir = edgeNow == 1 ? -1.0f : 1.0f;
        float end = mid + dir * inner / 2;
        float cy = end - dir * (57.0f - idx * 22.0f);
        return {1.0f, cy - 11.0f, hh - 1.0f, cy + 11.0f};
    }
    float inner = kIslandW - 2 * cfg.fillet - 20;
    float x1 = winW / 2 + inner / 2;
    return Rect(x1 - 68.0f + idx * 22.0f, 2.0f, 22.0f, hh - 6.0f);
}

int Drawer::IslandCtlAt(D2D1_POINT_2F p, float h) const {
    if (!IslandMusicActive() || !islandVizHot || islandCtlT < 0.25f) return -1;
    for (int i = 0; i < 3; ++i)
        if (Inside(IslandCtlRect(i, h), p)) return i;
    return -1;
}

void Drawer::SampleBehind() {
    RECT r = Footprint(); // the panel's full open footprint
    glassShade = TintFor(sys::Brightness(r)); // dark and colorful stay clear, pages that fight the ink get a deeper tint
    if (spring.value < 0.5f) glassShadeNow = glassShade; // nothing showing yet: it opens on the right tint, no fade
    shadeTick = 0;
}

void Drawer::SampleInk() {
    if (!Clear() || hidden || fullApp || overFull || stage || Unseen()) return; // staying up over a game: no screen reads under it
    constexpr float kToBlack = 0.52f, kToWhite = 0.42f; // apart, so a mid gray page doesn't flip it back and forth
    RECT f = Footprint();
    LONG half = (LONG)(IslandSpan() * scale / 2), deep = (LONG)(kIslandH * scale), band = (LONG)(kShadeBand * scale + 0.5f), gap = (LONG)(kShadeGap * scale + 0.5f);
    LONG cx = (f.left + f.right) / 2, cy = (f.top + f.bottom) / 2;
    RECT r = edgeNow == 0 ? RECT{cx - half, f.top + deep + gap, cx + half, f.top + deep + gap + band} // along the pill, on its open side
           : edgeNow == 1 ? RECT{f.left + deep + gap, cy - half, f.left + deep + gap + band, cy + half}
                          : RECT{f.right - deep - gap - band, cy - half, f.right - deep - gap, cy + half};
    float lum = sys::Brightness(r), to = clearInkGoal > 0.5f ? (lum < kToWhite ? 0.0f : 1.0f) : (lum > kToBlack ? 1.0f : 0.0f);
    if (spring.value < 0.5f) clearInk = to; // nothing showing yet: it drops in the right ink
    if (to != clearInkGoal) clearInkGoal = to, Redraw();
}

void Drawer::SampleShade() { // a page can scroll from dark to white without anything moving; a screen read is costly, so only every half minute
    if (!model.Glassy() || hidden || fullApp || overFull || stage || Unseen() || (!opened && !island)) return; // never while nothing of it shows
    RECT f = Footprint();
    LONG band = (LONG)(kShadeBand * scale + 0.5f), gap = (LONG)(kShadeGap * scale + 0.5f);
    RECT r = f; // open: a thin band just past the panel, in its middle half, never under it
    if (opened && edgeNow == 0) r = {f.left + (f.right - f.left) / 4, f.bottom + gap, f.right - (f.right - f.left) / 4, f.bottom + gap + band};
    else if (opened && edgeNow == 1) r = {f.right + gap, f.top + (f.bottom - f.top) / 4, f.right + gap + band, f.bottom - (f.bottom - f.top) / 4};
    else if (opened) r = {f.left - gap - band, f.top + (f.bottom - f.top) / 4, f.left - gap, f.bottom - (f.bottom - f.top) / 4};
    float to = TintFor(sys::Brightness(r)); // the pill alone covers little of the footprint: that's read whole
    if (std::fabs(to - glassShade) > kShadeStep) glassShade = to, Redraw(); // the tint eases over in the frames that follow
}

float Drawer::DepthOf(D2D1_POINT_2F p) const {
    return edgeNow == 0 ? p.y : edgeNow == 1 ? p.x : winH - p.x; // right edge: measured from the window's right side
}

D2D1_POINT_2F Drawer::EdgeAnchor() const {
    return edgeNow == 0 ? D2D1_POINT_2F{winW / 2, 0}
         : edgeNow == 1 ? D2D1_POINT_2F{0, winW / 2}
                        : D2D1_POINT_2F{winH, winW / 2};
}

D2D1_RECT_F Drawer::Visible() const { // the window's part of the surface, in dips: the top left corner, as big as the window
    if (pillRgn) { // the island: only the pill's slice of the full-width window, and its bubble
        float a = (peekRetracting || peekLook || spring.value > kIslandH + 1) ? (std::max)(IslandSpan(), widthSpring.value) : IslandSpan();
        float o = (winW - a) / 2, b = bubbleNow ? kBubbleGap + kBubble + 2 : 0;
        return Side() ? D2D1_RECT_F{0, o, winH, o + a + b} : D2D1_RECT_F{o, 0, o + a + b, winH};
    }
    return Side() ? D2D1_RECT_F{0, 0, winH, winW} : D2D1_RECT_F{0, 0, winW, winH};
}

D2D1_MATRIX_3X2_F Drawer::EdgeXform() const {
    float off = -(Span() - winW) / 2; // the window may be shorter than the panel (strip, island), the shape stays centered in it
    if (edgeNow == 1) return D2D1::Matrix3x2F(0, 1, 1, 0, 0, off); // left: depth runs along x
    if (edgeNow == 2) return D2D1::Matrix3x2F(0, 1, -1, 0, winH, off); // right: depth runs leftwards from the window's right side
    return D2D1::Matrix3x2F::Translation(off, 0);
}

void Drawer::ReleaseLater() {
    if (!opened && !peeking && spring.value <= 0 && cfg.gpuIdleSec > 0)
        SetTimer(hwnd, kReleaseTimer, (UINT)cfg.gpuIdleSec * 1000, nullptr); // timer handler re-checks state
}

void Drawer::Resize(float hDip, bool topmost) {
    wantH = hDip;
    bool idle = hDip <= kStrip + 0.01f;
    if (idle != eco) sys::SetEco(eco = idle); // throttle only while fully closed
    // the right edge grows leftwards: each change of depth moves the window's left side, and for a frame the last picture shows shifted with it.
    // so while anything shows it keeps its full depth, nothing drawn ever moves, and a region trims what takes the mouse to the depth in use
    winH = edgeNow == 2 && !idle ? (std::max)(hDip, Depth() + overshoot) : hDip;
    float along = idle ? (std::min)(Span(), cfg.hoverW) : Span(); // closed: small hot zone; the island keeps the full width, a region trims it to the pill
    winW = along;
    int a = (int)(along * scale + 0.5f);
    int d = (std::max)(2, (int)(winH * scale + 0.5f)); // min 2px so strip stays hoverable
    int live = (std::clamp)((int)(hDip * scale + 0.5f), 2, d); // the depth really in use, measured from the docked edge
    int x, y, w, h;
    if (edgeNow == 0) { // top: along = width, depth = height
        w = a, h = d;
        x = monitor.left + (monitor.right - monitor.left - w) / 2;
        y = monitor.top;
    } else { // left or right: along = height, depth = width, centered vertically
        w = d, h = a;
        x = edgeNow == 1 ? monitor.left : monitor.right - w;
        y = work.top + (work.bottom - work.top - h) / 2; // centered where no taskbar is, windows' or halobar's
    }
    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : nullptr, x, y, w, h,
                 SWP_NOACTIVATE | (topmost ? 0 : SWP_NOZORDER)); // only re-raise z-order when opening
    // a narrower window for the pill would move what's on screen when it grows into the panel: the pill would jump aside for a frame.
    // so the window keeps its width and a region makes everything beside the pill click-through; opening just lifts the region
    bool pill = islandLook && !opened && !idle;
    bool peekNarrow = peekLook && !opened && !idle && !(closingToPeek && menuCloseAlpha > 0.02f);
    int inner = edgeNow == 2 ? w - live : 0; // side edges: the live part's inner side; the left edge's window is only that deep anyway
    RECT want = {inner, 0, edgeNow == 0 ? w : inner + live, h}; // the right edge's trim, when nothing narrower is wanted
    if (pill) {
        float wCur = (peekRetracting || peekLook || spring.value > kIslandH + 1) ? (std::max)(IslandSpan(), widthSpring.value) : IslandSpan();
        int pa = (int)(wCur * scale + 0.5f);
        bubbleNow = Bubble();
        int extra = bubbleNow ? (int)((kBubbleGap + kBubble + 2) * scale + 0.5f) : 0; // the bubble beside it is live too
        want = edgeNow == 0 ? RECT{(w - pa) / 2, 0, (w + pa) / 2 + extra, h} : RECT{inner, (h - pa) / 2, inner + live, (h + pa) / 2 + extra};
    } else if (peekNarrow) {
        float wCur = (std::max)(PeekSpan(), widthSpring.value);
        int pa = (int)(wCur * scale + 0.5f);
        bubbleNow = false;
        want = edgeNow == 0 ? RECT{(w - pa) / 2, 0, (w + pa) / 2, h} : RECT{inner, (h - pa) / 2, inner + live, (h + pa) / 2};
    }
    if (pill || peekNarrow || live < d) {
        if (!rgnOn || !EqualRect(&want, &rgnRect)) { // only when it changed: resizes come often
            HRGN rg = CreateRectRgnIndirect(&want);
            if (rg && SetWindowRgn(hwnd, rg, FALSE)) rgnOn = true, rgnRect = want; // the system owns rg now
            else if (rg) DeleteObject(rg);
        }
    } else if (rgnOn) {
        SetWindowRgn(hwnd, nullptr, FALSE);
        rgnOn = false;
    }
    pillRgn = pill && rgnOn;
}

D2D1_POINT_2F Drawer::ToDip(LPARAM lp) const {
    return {GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale};
}

void Drawer::TrackLeave() {
    if (tracking) return;
    TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
    tracking = TrackMouseEvent(&t) != FALSE;
}

bool Drawer::MouseIn() const {
    POINT p;
    RECT r;
    GetCursorPos(&p);
    GetWindowRect(hwnd, &r);
    if (!PtInRect(&r, p)) return false; // real cursor check, leave msgs can lie
    if (!rgnOn) return true;
    HRGN rg = CreateRectRgn(0, 0, 0, 0); // the island or the right edge's trim: inside the live part, not just the window
    bool in = rg && GetWindowRgn(hwnd, rg) != ERROR && PtInRegion(rg, p.x - r.left, p.y - r.top);
    if (rg) DeleteObject(rg);
    return in;
}

void Drawer::Open() {
    if (hidden || (overFull && !model.pillFullOpen)) return; // over a fullscreen app, the panel only if that's allowed
    if (!opened && !pageSet && (!model.rememberPage || !model.HasPage(page)) && page != model.OpenPage()) prevPage = page, page = model.OpenPage(), pageT = 1; // the page picked in settings, or one still shown, without a slide
    pageSet = false;
    if (model.Glassy() && spring.value < 1) SampleBehind(); // read the background before the panel covers it
    bool fromPill = islandLook || peekLook;
    float pillW0 = peekLook ? ActivePeekSpan() : IslandSpan();
    float pillH0 = peekLook ? (std::max)(kIslandH, spring.value) : kIslandH;
    bool wasRestingPill = islandLook && !peekLook && !opened;
    bool wasClosingZoom = zoomActive && !opened;
    float longLen = 0, longThick = 0;
    bool fromLongSameEdge = !fromPill && !opened && desk && desk->MorphShape(edgeNow, longLen, longThick);
    if (fromLongSameEdge) {
        fromPill = true;
        pillW0 = (std::min)(Span(), longLen + 2 * cfg.fillet);
        pillH0 = (std::max)(kIslandH, longThick);
    }
    if (desk && (desk->Up() || desk->Visible()) && model.openAnim == Model::kAnimZoom && !opened) desk->HideNow();
    grow = grow || fromPill; // expanding out of the island or notification pop-up: width morphs as it drops
    island = islandLook = peekLook = peekToastNow = peekHot = peekCloseHot = peekExpanded = peekPrevHot = peekNextHot = peekClearHot = false;
    peekFromPill = peekRetracting = false;
    peekRetractH = kNotifOneH;
    closingToPeek = false;
    menuCloseAlpha = 0.0f;
    menuCloseScale = 1.0f;
    menuPeekSpring = anim::Spring{};
    islandVizHot = false;
    islandCtlBtn = -1;
    islandCtlT = 0.0f;
    peekHoverRow = peekCloseRow = -1;
    peekShownRows = 0;
    peekStack.clear();
    peekIdx = 0;
    if (!EnsureDev()) return; // gpu may have been freed while idle
    KillTimer(hwnd, kReleaseTimer); // keep gpu alive
    KillTimer(hwnd, kCloseTimer);
    KillTimer(hwnd, kPeekTimer);
    KillTimer(hwnd, kPeekExpandTimer);
    peeking = false; // peek upgrades to full open
    for (sys::Toast t; model.notifyWatch.Pop(t);) {} // the rest of a notification queue would only show stale once it closes
    bool alreadyOpen = opened;
    if (!opened) {
        opened = true;
        SyncDesk(); // the long pill makes way
        SyncWall(); // wallpaper may have changed while closed
        if (Clear()) SyncWallTone();
        model.OnOpen();
        model.SetLive(true, page == Model::kPgSystem, page == Model::kPgNetwork); // fast polling only while visible, the process list only on the system page, latency only on the network page
    }
    else if (!tourSeen && model.tourNext && tourStep < 0) TourStart(); // the first time the panel opens this launch, while its switch is on

    float targetDepth = Depth();
    Resize(targetDepth + overshoot, true); // grow first so bounce/zoom isn't clipped
    if (model.openAnim == Model::kAnimZoom && !alreadyOpen) {
        zoomActive = true;
        if (!wasClosingZoom) {
            zoomFromPill = fromPill;
            zoomDrawPill = wasRestingPill;
            zoomPillW = pillW0;
            zoomPillH = pillH0;
            float dragStart = !fromPill && spring.value > 1.0f ? anim::Clamp01(spring.value / targetDepth) : 0.0f;
            zoomSpring.value = dragStart;
            zoomSpring.vel = 0;
        }
        zoomToPill = false;
        static constexpr float kOpenStiff[Model::kAnimSpeeds] = {420.0f, 290.0f, 185.0f};
        zoomSpring.stiff = kOpenStiff[(std::clamp)(model.animSpeed, 0, Model::kAnimSpeeds - 1)];
        zoomSpring.damp = model.reduceMotion ? 1.0f : 0.60f; // lively spring overshoot (~9.5%)
        zoomSpring.target = 1.0f;
        spring.value = spring.target = targetDepth;
        spring.vel = 0;
    } else if (!alreadyOpen) {
        zoomActive = zoomFromPill = zoomToPill = zoomDrawPill = false;
        zoomSpring = anim::Spring{};
        zoomAlpha = zoomInvScale = 1.0f;
        zoomBodyXform = D2D1::Matrix3x2F::Identity();
        spring.target = targetDepth;
        spring.stiff = 320.0f;
        spring.damp = model.reduceMotion ? 1.0f : 0.62f; // underdamped, drips open with a bounce; calm: it just arrives
    }
    if (screensNow == 1 && !strips.empty()) SetTimer(hwnd, kFollowTimer, kFollowMs, nullptr); // every screen: follow the cursor to another one
    Redraw();
}

void Drawer::Greet() {
    greetTries = 0;
    SetTimer(hwnd, kGreetTimer, kGreetDelayMs, nullptr);
}

void Drawer::OnGreetTimer() {
    if (!greeting) { // first: drop the home page down
        if (opened || stage || fullApp) return; // the user got there first, or a game has the screen
        if (hidden || !EnsureDev()) {
            if (++greetTries < kGreetTries) SetTimer(hwnd, kGreetTimer, kGreetDelayMs / 3, nullptr);
            return;
        }
        if (model.HasPage(Model::kPgHome) && page != Model::kPgHome) SetPage(Model::kPgHome);
        pageSet = true; // home, even with another opening page picked in settings
        Open();
        if (!opened) return;
        greeting = true;
        GetCursorPos(&greetAt);
        SetTimer(hwnd, kGreetTimer, kGreetPollMs, nullptr);
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    bool moved = std::abs(pt.x - greetAt.x) > kGreetSlack || std::abs(pt.y - greetAt.y) > kGreetSlack;
    if (!opened) greeting = false; // closed some other way
    else if (moved) greeting = false, Pinned() ? void() : Close(); // the mouse woke up: back up it goes, unless it's pinned open
    else SetTimer(hwnd, kGreetTimer, kGreetPollMs, nullptr);
}

void Drawer::KeyOpen() {
    if (opened && !Pinned()) {
        Close();
        return;
    }
    if (opened) { // pinned: the shortcut only takes the keyboard, the panel stays
        keys = true;
        keysFrom = GetForegroundWindow();
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        return;
    }
    if (hidden) return;
    if (screensNow == 1) { // every screen: open on the one with the mouse
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (m != mon && !dragging && spring.value <= 0.5f) {
            mon = m;
            FreeDev();
            Place();
            BuildStrips();
            SyncHide();
        }
    }
    Open();
    keys = true;
    keysFrom = GetForegroundWindow();
    SetForegroundWindow(hwnd); // the shortcut lets us take the keyboard: arrows, numbers and esc work at once
    SetFocus(hwnd);
}

void Drawer::Close() {
    SyncRailKeys(false);
    if (keys) { // the keyboard goes back where it was
        keys = false;
        if (GetForegroundWindow() == hwnd && keysFrom && IsWindow(keysFrom)) SetForegroundWindow(keysFrom);
    }
    ui.Unfocus();
    KillTimer(hwnd, kFollowTimer);
    if (cfg.gpuIdleSec > 0) SetTimer(hwnd, kReleaseTimer, (UINT)cfg.gpuIdleSec * 1000 + 1000, nullptr); // freed, or trimmed if the pill stays
    KillTimer(hwnd, kCloseTimer);
    bool wasOpened = opened;
    bool wasOpenedOrZoom = opened || zoomActive;
    bool keepPeek = wasOpened && peeking && peekToastNow && !followTo && !hidden && !stage;
    if (!keepPeek) {
        KillTimer(hwnd, kPeekTimer);
        KillTimer(hwnd, kPeekExpandTimer);
        peeking = peekHot = peekCloseHot = peekExpanded = peekPrevHot = peekNextHot = peekClearHot = false;
        peekFromPill = peekRetracting = false;
        peekRetractH = kNotifOneH;
        closingToPeek = false;
        menuCloseAlpha = 0.0f;
        menuCloseScale = 1.0f;
        menuPeekSpring = anim::Spring{};
        peekHoverRow = peekCloseRow = -1;
    } else {
        KillTimer(hwnd, kPeekExpandTimer);
        peekHot = peekHot && MouseIn();
        if (!peekHot) {
            peekCloseHot = peekPrevHot = peekNextHot = peekClearHot = false;
            peekHoverRow = peekCloseRow = -1;
            SetTimer(hwnd, kPeekTimer, 4500, nullptr);
        }
    }
    islandVizHot = false;
    islandCtlBtn = -1;
    islandCtlT = 0.0f;
    opened = false;
    island = false; // the island retracts with the same spring, its shape stays until it's gone
    model.hexFocus = model.cityFocus = model.calFocus = model.faceSearch = false;
    model.calErr = false;
    ++model.pageVisit;
    model.nameFor = -1;
    model.field = Model::kFieldNone;
    model.CloseFace(); // what was typed in a widget is kept
    if (model.shelfField == Model::kShelfNote) model.CommitShelfNote(); // a note half typed is kept, like a city
    model.shelfField = Model::kShelfNone; // the search text stays for next time, the keyboard goes back
    ShelfClosed();
    Entry::Get().Drop(); // whatever was typed goes with the panel
    StagePrefs::Get().capture = 0; // a fullscreen key half picked is dropped with the panel
    model.CancelPass();
    SyncTyping(); // hand the keyboard back right away
    if (model.rememberPage && page != model.lastPage) model.SetPref(model.lastPage, L"LastPage", page); // the next launch opens here too
    model.CloseBt(); // menus die with the panel
    model.CloseWifi();
    model.CloseSettings();
    model.CloseCal();
    model.SetLive(false, false);
    if (keepPeek) {
        zoomActive = zoomFromPill = zoomToPill = zoomDrawPill = false;
        zoomAlpha = zoomInvScale = 1.0f;
        zoomBodyXform = D2D1::Matrix3x2F::Identity();
        peeking = peekToastNow = peekLook = true;
        closingToPeek = true;
        menuCloseAlpha = 1.0f;
        menuCloseScale = 1.0f;
        widthSpring.value = Span();
        widthSpring.vel = 0.0f;
        UpdatePeekLayout();
        spring.stiff = model.reduceMotion ? 360.0f : 270.0f;
        spring.damp = model.reduceMotion ? 1.0f : 0.72f;
        widthSpring.stiff = model.reduceMotion ? 360.0f : 260.0f;
        widthSpring.damp = model.reduceMotion ? 1.0f : 0.72f;
        menuPeekSpring = anim::Spring{};
    } else if (model.openAnim == Model::kAnimZoom && wasOpenedOrZoom && spring.value > 1.0f && !dragging) {
        bool longTarget = WantLongPill() || (desk && desk->Up());
        bool toPill = Stays() && !longTarget && !followTo && !stage && !hidden;
        bool fromOpenZoom = wasOpened && (!zoomActive || zoomSpring.value >= 0.90f);
        bool wasClosingZoom = zoomActive && !wasOpened && zoomSpring.target <= 0.0f;
        if (!wasClosingZoom) {
            if (!zoomActive) {
                zoomSpring.value = 1.0f;
                zoomSpring.vel = 0;
            }
            if (!model.reduceMotion && fromOpenZoom) zoomSpring.vel = 0.68f; // tactile release pop before spring-closing
        }
        zoomActive = true;
        zoomToPill = toPill;
        if (toPill) {
            zoomFromPill = true;
            zoomDrawPill = true;
            zoomPillW = IslandSpan();
            zoomPillH = kIslandH;
        } else {
            zoomFromPill = false;
            zoomDrawPill = false;
        }
        static constexpr float kCloseStiff[Model::kAnimSpeeds] = {390.0f, 270.0f, 175.0f};
        zoomSpring.stiff = kCloseStiff[(std::clamp)(model.animSpeed, 0, Model::kAnimSpeeds - 1)];
        zoomSpring.damp = model.reduceMotion ? 1.0f : 0.64f; // bouncy spring settle on close too
        zoomSpring.target = 0.0f;
        spring.target = Depth();
        Resize(Depth() + overshoot, false);
    } else {
        zoomActive = zoomFromPill = zoomToPill = zoomDrawPill = false;
        zoomAlpha = zoomInvScale = 1.0f;
        zoomBodyXform = D2D1::Matrix3x2F::Identity();
        spring.target = 0;
        spring.stiff = 320.0f;
        spring.damp = 0.92f; // near critical, retract without bounce
    }
    Redraw();
}

void Drawer::ShowSettings() {
    Open();
    model.OpenSettings();
    Redraw();
}

void Drawer::Toggle() {
    if (opened && Pinned()) return; // pinned: it stays where it is
    if (opened) Close();
    else Open();
}

bool Drawer::Stays() const { return PillStays(model) || (model.pillDesk && deskView); }

void Drawer::SyncStay() {
    if (opened || zoomActive) return; // never spawn or toggle the compact pill while Zoom Fade is opening or closing
    if (WantLongPill() || (desk && desk->Up())) { // the long pill owns the desktop: retract any compact pill so both never stack
        if (island && !opened && !peeking) Close();
        stayWasAlways = false;
        return;
    }
    bool always = Stays();
    if (always && !island && !peeking && !dragging && !hidden && !stage && spring.value <= 0.5f) ShowIsland(); // the pill comes back by itself
    else if (!always && stayWasAlways && island && !MouseIn()) Close(); // always was just turned off, or an app came under it
    stayWasAlways = always;
}

void Drawer::Answer() {
    if (hidden) return; // a game has this screen
    if (MouseIn()) { // still at the edge: what a hover there brings
        islandNext = AppBelow();
        if (!model.keyOnly && !(islandNext && model.appKeyOnly)) { // shortcut only, everywhere or over an app: a hover brings nothing, as below
            if (islandNext) ShowIsland();
            else Open();
            return;
        }
    }
    stayWasAlways = false; // the mouse moved on: the pill comes back here if something holds it up, without closing anything
    SyncStay();
}

void Drawer::OnSecond() {
    if (model.StepTrackTransition()) {
        SyncStay();
        if (desk) desk->Refresh();
        Redraw();
    }
    if (model.facesDue) {
        model.facesDue = false; // set again only if the page draws again
        Redraw();
    }
    SyncBoard();
    if (Pinned() && !overFull && !opened && !hidden && !stage && !dragging && !peeking && !PlaceStale() && spring.value <= 0.5f) Open(); // pinned: back open after fullscreen, a re-dock or the player
    if (fullApp && !FullOn(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY))) fullApp = false; // the shell's "it's over" can go missing: never stay hidden on its word alone
    SyncHide(); // a fullscreen app on the panel's screen, or gone from it
    if (++shadeTick >= kShadeSecs) shadeTick = 0, SampleShade();
    if (Clear() && island && !opened && ++inkTick >= kInkSecs) inkTick = 0, SampleInk();
    model.IdleSpectrum();
    if (model.PollShared()) Redraw(); // halobar changed the theme we wear: a showing pill or panel repaints, SyncLook crossfades
    if (model.haloReach != reachUsed && !opened && !dragging) Place(); // halobar's dock grew, shrank, came or went: the sidebar refits, never under the mouse
    SyncHud();
    RetryHud();
    if (deskOn != model.WatchesDesk()) WatchDesk(); // the switch in settings was flipped
    if (deskOn && (model.appsTheme != 0) != lookAppsOn) lookAppsOn = model.appsTheme != 0, OnDeskTimer(); // the over-apps theme: on now, not at the next window event
    if (deskOn && (model.appsTheme || model.AnyLong()) && !Unseen() && ++deskRecheck >= kDeskRecheckSecs) deskRecheck = 0, OnDeskTimer();
    SyncThemeNow();
    SyncDesk();
    if (desk && !Unseen()) desk->Second(); // locked or dark: no screen reads and no refreshes for nobody
    bool smallUp = island && model.pillAlways, longUp = desk && desk->Up();
    auto onPill = [&](int kind) { return (smallUp && (model.pillLeft == kind || model.pillRight == kind)) || (longUp && std::find(model.deskItems.begin(), model.deskItems.end(), kind) != model.deskItems.end()); };
    model.SetPillMeters(!Unseen() && onPill(Model::kWGpu), !Unseen() && (onPill(Model::kWCpu) || onPill(Model::kWRam))); // only while one shows: the gpu counter closes the moment none does
    if (!opened) {
        SyncStay();
        if (island && Bubble() != bubbleNow) Resize(wantH, false); // the bubble came or went: the live region follows
        if (island && !Unseen()) { // a still pill redraws only when what it shows changed: the minute, a widget, the bubble's time
            std::wstring sig = PillSig();
            if (sig != pillSig) {
                pillSig = std::move(sig);
                Redraw();
            }
        }
        if (island && model.pillAlways && (model.pillLeft == Model::kWWeather || model.pillRight == Model::kWWeather)) model.Prefetch(); // a no-op while the forecast is fresh
        return;
    }
    model.ReadAudio(); // volume only matters when visible
    Redraw();
}

void Drawer::SetPage(int p) {
    if (p == page || !model.HasPage(p)) return; // a page taken off the rail can't be gone to
    model.CloseBt();
    model.CloseWifi();
    model.CloseSettings();
    model.CloseCal();
    prevPage = page;
    page = p;
    pageT = 0;
    model.SetLive(opened, page == Model::kPgSystem, page == Model::kPgNetwork);
    Redraw();
}

void Drawer::Peek(const std::wstring& title, const std::wstring& detail, int icon) {
    if (overFull) return; // over a fullscreen app, only the pill
    if (!opened && peeking && peekToastNow && model.PillsWanted()) {
        sys::Toast toast;
        toast.app = L"Astraeus";
        toast.aumid = L"astraeus.notice";
        toast.title = title;
        toast.body = detail;
        toast.iconGlyph = icon ? icon : glyph::Timer;
        toast.silent = true;
        PeekToast(std::move(toast));
        return;
    }
    peekLevel = -1; // a notice, not the hud's bar
    peekToastNow = peekHot = peekCloseHot = peekExpanded = peekPrevHot = peekNextHot = peekClearHot = false;
    peekHoverRow = peekCloseRow = -1;
    peekStack.clear();
    peekIdx = 0;
    zoomActive = zoomToPill = zoomFromPill = zoomDrawPill = false;
    zoomAlpha = zoomInvScale = 1.0f;
    zoomBodyXform = D2D1::Matrix3x2F::Identity();
    KillTimer(hwnd, kPeekExpandTimer);
    peekBmp.Reset();
    peekIcon = icon;
    peekTitle = title;
    peekDetail = detail;
    if (opened) {
        if (!Pinned()) SetPage(Model::kPgTools); // already open, jump to notice page; pinned, it would yank the page from under you all day
        return;
    }
    if (hidden || !EnsureDev()) return;
    if (model.Glassy() && spring.value < 1) SampleBehind();
    KillTimer(hwnd, kReleaseTimer);
    KillTimer(hwnd, kCloseTimer);
    if (!peeking && !peekLook) {
        widthSpring.value = islandLook ? IslandSpan() : kIslandW;
        widthSpring.vel = 0;
    }
    island = islandLook = grow = false; // a notice replaces the island
    peeking = peekLook = true;
    peekReach = Side() ? kPeekSide : kPeekH; // on a side edge the banner needs room for its text
    widthSpring.target = PeekSpan();
    widthSpring.damp = model.reduceMotion ? 1.0f : 0.68f;
    SyncDesk();
    Resize(peekReach + overshoot, true);
    spring.target = peekReach; // drop only a short banner
    spring.damp = model.reduceMotion ? 1.0f : 0.58f; // extra bouncy to catch the eye
    SetTimer(hwnd, kPeekTimer, model.notifyWatch.More() ? 3500 : 5000, nullptr);
    Redraw();
}

D2D1_RECT_F Drawer::MenuPeekBox() const {
    if (!opened) return D2D1_RECT_F{};
    float h = (std::max)(0.0f, peekReach * menuPeekSpring.value);
    if (Side()) {
        float ox = edgeNow == 1 ? 0.0f : winH - CW();
        return Rect(ox, cfg.fillet, CW(), h);
    }
    float topY = Floating() ? kFloatGap : 0.0f;
    return Rect(cfg.fillet, topY, CW(), h);
}

float Drawer::PeekTopOffset() const {
    if (opened) return Side() ? cfg.fillet : (Floating() ? kFloatGap : 0.0f);
    return 0.0f;
}

float Drawer::PeekInnerWidth() const {
    if (opened) return CW() - 28.0f;
    return ActivePeekSpan() - 2 * cfg.fillet - 28.0f;
}

float Drawer::PeekLeftOffset() const {
    if (opened) return (Side() ? (edgeNow == 1 ? 0.0f : winH - CW()) : cfg.fillet) + 14.0f;
    return winW / 2 - PeekInnerWidth() / 2;
}

D2D1_RECT_F Drawer::PeekCloseBox(float h) const {
    (void)h;
    if (!opened && Side()) return D2D1_RECT_F{};
    float x0 = PeekLeftOffset(), inner = PeekInnerWidth(), y0 = PeekTopOffset();
    return Rect(x0 + inner - 22, y0 + 8, 22, 22);
}

D2D1_RECT_F Drawer::PeekPrevBox() const {
    if ((!opened && Side()) || peekExpanded || peekStack.size() <= 1) return D2D1_RECT_F{};
    float x1 = PeekLeftOffset() + PeekInnerWidth(), y0 = PeekTopOffset();
    return Rect(x1 - 94, y0 + 7, 18, 20);
}

D2D1_RECT_F Drawer::PeekNextBox() const {
    if ((!opened && Side()) || peekExpanded || peekStack.size() <= 1) return D2D1_RECT_F{};
    float x1 = PeekLeftOffset() + PeekInnerWidth(), y0 = PeekTopOffset();
    return Rect(x1 - 46, y0 + 7, 18, 20);
}

D2D1_RECT_F Drawer::PeekClearBox() const {
    if ((!opened && Side()) || !peekExpanded || peekStack.size() <= 1) return D2D1_RECT_F{};
    float x1 = PeekLeftOffset() + PeekInnerWidth(), y0 = PeekTopOffset();
    return Rect(x1 - 82, y0 + peekPrimaryBot + 4, 82, 20);
}

int Drawer::PeekShownStackRows() const {
    if (peekStack.size() <= 1) return 0;
    return peekShownRows > 0 ? peekShownRows : (std::min)((int)peekStack.size() - 1, kMaxStackRows);
}

int Drawer::PeekRowRealIdx(int rowSlot) const {
    int slot = 0;
    for (int i = 0; i < (int)peekStack.size(); ++i) {
        if (i == peekIdx) continue;
        if (slot == rowSlot) return i;
        ++slot;
    }
    return -1;
}

D2D1_RECT_F Drawer::PeekStackRowBox(int rowSlot) const {
    if ((!opened && Side()) || !peekExpanded || rowSlot < 0 || rowSlot >= PeekShownStackRows()) return D2D1_RECT_F{};
    float x0 = PeekLeftOffset(), inner = PeekInnerWidth(), y0 = PeekTopOffset();
    float ry = y0 + peekPrimaryBot + 28.0f + rowSlot * 48.0f;
    return Rect(x0, ry, inner, 44.0f);
}

D2D1_RECT_F Drawer::PeekStackRowCloseBox(int rowSlot) const {
    D2D1_RECT_F r = PeekStackRowBox(rowSlot);
    if (Width(r) <= 0) return D2D1_RECT_F{};
    return Rect(r.right - 26.0f, r.top + 11.0f, 20.0f, 20.0f);
}

bool Drawer::IsPeekTruncated() const {
    if (!peekToastNow || (!opened && Side()) || !canvas) return false;
    float innerCompact = opened ? (CW() - 28.0f) : ((std::min)(kNotifW, W > 0 ? W : kNotifW) - 2 * cfg.fillet - 28.0f);
    float tw = innerCompact - 50.0f;
    if (canvas->Measure(peekTitle, Weight::Semi, 13.0f) > tw - 26.0f) return true;
    if (peekDetail.empty()) return false;
    if (peekDetail.find(L'\n') != std::wstring::npos || peekDetail.find(L'\r') != std::wstring::npos) return true;
    if (canvas->Measure(peekDetail, Weight::Regular, 11.5f) > tw - 6.0f) return true;
    int lines = 1;
    canvas->MeasureWrap(peekDetail, Weight::Regular, 11.5f, tw - 4.0f, &lines);
    return lines > 1;
}

void Drawer::SyncPeekFields() {
    if (peekStack.empty()) return;
    peekIdx = (std::clamp)(peekIdx, 0, (int)peekStack.size() - 1);
    auto& item = peekStack[(size_t)peekIdx];
    if (!item.bmp && !item.toast.iconPx.empty() && item.toast.iconW > 0 && item.toast.iconH > 0 && ctx) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ctx->CreateBitmap({item.toast.iconW, item.toast.iconH}, item.toast.iconPx.data(), item.toast.iconW * 4, bp, &item.bmp);
    }
    peekToast = item.toast;
    peekBmp = item.bmp;
    peekTitle = !peekToast.title.empty() ? peekToast.title : (!peekToast.app.empty() ? peekToast.app : L"Notification");
    peekDetail = peekToast.body;
}

void Drawer::UpdatePeekLayout() {
    if (!peeking || !peekToastNow) return;
    peekShownRows = 0;
    float availDepth = opened ? (CH() - 16.0f) : Depth();
    if (!opened && Side()) {
        peekReach = kPeekSide;
    } else if (!peekExpanded) {
        float stackExtra = peekStack.size() > 1 ? 14.0f : 0.0f;
        peekReach = (peekDetail.empty() ? kNotifShortH : kNotifOneH) + stackExtra;
    } else {
        float innerExp = opened ? (CW() - 28.0f) : (PeekSpan() - 2 * cfg.fillet - 28.0f);
        float tw = innerExp - 50.0f;
        float titleH = canvas ? (std::max)(18.0f, canvas->MeasureWrap(peekTitle, Weight::Semi, 13.5f, tw - 26.0f) + 2.0f) : 20.0f;
        int availOthers = (std::max)(0, (int)peekStack.size() - 1);
        float maxBodyH = availOthers > 0 ? (std::max)(60.0f, availDepth - 165.0f) : (std::max)(90.0f, availDepth - 80.0f);
        float bodyH = (!peekDetail.empty() && canvas)
            ? (std::min)(maxBodyH, (std::max)(18.0f, canvas->MeasureWrap(peekDetail, Weight::Regular, 12.0f, tw - 4.0f) + 4.0f))
            : 0.0f;
        peekPrimaryBot = (std::max)(62.0f, 26.0f + titleH + (bodyH > 0 ? 4.0f + bodyH : 0.0f) + 10.0f);
        int maxFitRows = (std::min)(availOthers, kMaxStackRows);
        while (maxFitRows > 0) {
            float needed = peekPrimaryBot + 28.0f + maxFitRows * 48.0f + (availOthers > maxFitRows ? 16.0f : 0.0f) + 14.0f;
            if (needed <= availDepth) break;
            --maxFitRows;
        }
        peekShownRows = maxFitRows;
        float totalH = peekPrimaryBot;
        if (peekShownRows > 0) {
            totalH += 28.0f + peekShownRows * 48.0f;
            if (availOthers > peekShownRows) totalH += 16.0f;
        }
        peekReach = (std::min)(availDepth, totalH + 14.0f);
    }
    if (opened) {
        widthSpring.target = PeekSpan();
        widthSpring.value = widthSpring.target;
        widthSpring.vel = 0;
        Redraw();
        return;
    }
    widthSpring.target = PeekSpan();
    widthSpring.stiff = model.reduceMotion ? 360.0f : (peekExpanded ? 245.0f : 260.0f);
    widthSpring.damp = model.reduceMotion ? 1.0f : (peekExpanded ? 0.76f : 0.78f);
    spring.target = peekReach;
    spring.stiff = model.reduceMotion ? 360.0f : (peekExpanded ? 250.0f : 275.0f);
    spring.damp = model.reduceMotion ? 1.0f : (peekExpanded ? 0.76f : 0.74f);
    Resize((std::max)(wantH, peekReach + overshoot), true);
    Redraw();
}

void Drawer::PeekToast(sys::Toast toast) {
    bool isInternal = toast.launch == L"astraeus:tools" || toast.aumid == L"astraeus.notice";
    if (overFull || hidden || (model.quiet && !isInternal) || !EnsureDev()) return;
    if (!opened) {
        zoomActive = zoomFromPill = zoomToPill = zoomDrawPill = false;
        zoomAlpha = zoomInvScale = 1.0f;
        zoomBodyXform = D2D1::Matrix3x2F::Identity();
        if (model.Glassy() && spring.value < 1) SampleBehind();
    }
    ULONGLONG nowTick = GetTickCount64();
    if (!toast.silent && !model.quiet && nowTick - lastToastSoundAt > 450) {
        lastToastSoundAt = nowTick;
        PlaySoundW(L"Notification.Default", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT | SND_SENTRY);
    }
    peekLevel = -1;
    peekIcon = glyph::Bell;
    bool wasPeeking = peeking && peekToastNow;

    PeekItem item;
    item.toast = std::move(toast);
    if (!item.toast.iconPx.empty() && item.toast.iconW > 0 && item.toast.iconH > 0 && ctx) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ctx->CreateBitmap({item.toast.iconW, item.toast.iconH}, item.toast.iconPx.data(), item.toast.iconW * 4, bp, &item.bmp);
    }

    if (!wasPeeking) peekStack.clear();
    if (!peekStack.empty() && peekStack.front().toast.app == item.toast.app &&
        peekStack.front().toast.title == item.toast.title && peekStack.front().toast.body == item.toast.body) {
        peekStack.front() = std::move(item);
    } else {
        peekStack.insert(peekStack.begin(), std::move(item));
        if (peekStack.size() > kMaxPeekStack) peekStack.resize(kMaxPeekStack);
    }
    peekIdx = 0;

    if (opened) {
        peekToastNow = true;
        peeking = true;
        peekLook = false;
        closingToPeek = false;
        menuCloseAlpha = 0.0f;
        if (!wasPeeking) {
            menuPeekSpring.value = 0.0f;
            menuPeekSpring.vel = 0.0f;
        } else if (!model.reduceMotion) {
            menuPeekSpring.vel += 1.6f;
        }
        menuPeekSpring.stiff = 340.0f;
        menuPeekSpring.damp = model.reduceMotion ? 1.0f : 0.70f;
        menuPeekSpring.target = 1.0f;
        widthSpring.value = widthSpring.target = PeekSpan();
        widthSpring.vel = 0.0f;
        SyncPeekFields();
        UpdatePeekLayout();
    } else {
        bool fromPill = (!peeking && !peekLook && islandLook && spring.value > 16.0f) || peekRetracting;
        if (!peeking && !peekLook) {
            widthSpring.value = islandLook ? IslandSpan() : kIslandW;
            widthSpring.vel = 0;
        } else if (wasPeeking && !model.reduceMotion) {
            spring.vel += 60.0f; // subtle tactile Dynamic Island bounce when another notification joins the stack
            widthSpring.vel += 45.0f;
        }

        peekFromPill = fromPill;
        peekRetracting = false;
        peekToastNow = true;
        KillTimer(hwnd, kReleaseTimer);
        KillTimer(hwnd, kCloseTimer);
        island = islandLook = grow = false;
        peeking = peekLook = true;
        SyncPeekFields();
        SyncDesk();
        UpdatePeekLayout();
        if (fromPill) peekRetractH = peekReach;
    }
    if (!peekHot) {
        SetTimer(hwnd, kPeekTimer, 5500, nullptr);
    } else if (!peekExpanded && (IsPeekTruncated() || peekStack.size() > 1)) {
        peekDwellAt = Seconds();
        SetTimer(hwnd, kPeekExpandTimer, kPeekDwellMs, nullptr);
    }
}

void Drawer::StepPeekStack(int delta) {
    if (peekStack.size() <= 1) return;
    int n = (int)peekStack.size();
    peekIdx = (peekIdx + delta % n + n) % n;
    SyncPeekFields();
    if (!model.reduceMotion) spring.vel += 35.0f;
    UpdatePeekLayout();
}

void Drawer::DismissPeekItem(int idx) {
    if (idx < 0 || idx >= (int)peekStack.size()) {
        DismissPeek();
        return;
    }
    peekStack.erase(peekStack.begin() + idx);
    if (peekStack.empty()) {
        DismissPeek();
        return;
    }
    if (peekIdx >= (int)peekStack.size()) peekIdx = (int)peekStack.size() - 1;
    else if (idx < peekIdx) --peekIdx;
    peekHoverRow = peekCloseRow = -1;
    SyncPeekFields();
    UpdatePeekLayout();
}

void Drawer::DismissPeek() {
    KillTimer(hwnd, kPeekTimer);
    KillTimer(hwnd, kPeekExpandTimer);
    peeking = peekHot = peekCloseHot = peekExpanded = peekPrevHot = peekNextHot = peekClearHot = false;
    islandVizHot = false;
    islandCtlBtn = -1;
    islandCtlT = 0.0f;
    peekHoverRow = peekCloseRow = -1;
    peekStack.clear();
    peekIdx = 0;
    sys::Toast next;
    if (!model.quiet && !hidden && model.notifyWatch.Pop(next)) {
        PeekToast(std::move(next)); // seamlessly morph to the next waiting notification in the Dynamic Island
        return;
    }
    if (opened) {
        menuPeekSpring.stiff = 320.0f;
        menuPeekSpring.damp = model.reduceMotion ? 1.0f : 0.92f;
        menuPeekSpring.target = 0.0f;
        widthSpring.target = Span();
        Redraw();
        return;
    }
    bool appsLong = desk && model.deskApps && !(deskView && !model.LongPill());
    bool longOwns = WantLongPill() || appsLong;
    if (Stays() && !longOwns && !(desk && desk->Up())) {
        if (peekToastNow && spring.value > kIslandH + 2.0f) {
            peekRetracting = true;
            peekFromPill = false;
            peekRetractH = (std::max)(peekReach, spring.value);
        }
        ShowIsland(); // smoothly shrink and crossfade from the Dynamic Island notification back to the resting pill
        return;
    }
    peekFromPill = false;
    peekRetracting = false;
    if (appsLong && Stays() && !hidden) {
        longHeld = true;
        SetTimer(hwnd, kLongTimer, model.IslandLingerMs((UINT)cfg.closeMs), nullptr);
    }
    widthSpring.target = IslandSpan();
    widthSpring.damp = model.reduceMotion ? 1.0f : 0.80f;
    Close();
}

void Drawer::StartDrag(float y) {
    if (!EnsureDev()) return;
    dragging = true;
    pressTo = spring.target; // remember if it was closed, for tap-to-open
    dragOff = (std::max)(spring.value, 0.0f) - y; // grab point offset so panel doesn't jump
    dragY0 = dragY = y;
    dragT = Seconds();
    dragVel = 0;
    spring.vel = 0; // finger takes over from spring
    KillTimer(hwnd, kCloseTimer);
    KillTimer(hwnd, kHoverTimer); // the hover's pending open would fight the drag; armed stays, no re-arm until the mouse leaves
    Resize(Depth() + overshoot, true); // full size so drag isn't clipped
    Redraw();
}

void Drawer::MoveDrag(float y) {
    double now = Seconds();
    double dt = now - dragT;
    if (dt > 0) dragVel = dragVel * 0.3f + (float)((y - dragY) / dt) * 0.7f; // smoothed velocity, favors latest sample
    dragY = y;
    dragT = now;
    spring.value = Rubber(dragOff + y); // panel follows cursor directly
    Redraw();
}

void Drawer::EndDrag() {
    if (!dragging) return;
    dragging = false;
    if (std::fabs(dragY - dragY0) < kTap) { // barely moved, treat as click
        if (pressTo <= 0) Open(); // click on strip opens
        Redraw();
        return;
    }
    if (Seconds() - dragT > 0.12) dragVel = 0; // paused before release, no fling
    bool open = dragVel > kFling || (dragVel > -kFling && spring.value > Depth() * kOpenAt); // fling wins, else by position
    spring.vel = dragVel; // hand off momentum to spring
    if (open) Open();
    else Close();
}

namespace {
void PerfWrite(const wchar_t* line) { // perf.log beside the exe
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L'\\')) + L"\\perf.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fputws(line, f);
        fclose(f);
    }
}
}

void Drawer::Log(const wchar_t* what) const {
    if (!cfg.perfLog) return;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT w = {};
    GetWindowRect(hwnd, &w);
    wchar_t line[512];
    _snwprintf_s(line, _TRUNCATE, L"[screen] %s: on %s (%ld,%ld %ldx%ld) scale %.2f, mode %s '%s', window %ld,%ld %ldx%ld, %s%s%s, %zu strips\n", what,
               mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
               scale, screensNow ? L"every" : L"one", screenNow.c_str(), w.left, w.top, w.right - w.left, w.bottom - w.top,
               IsWindowVisible(hwnd) ? L"visible" : L"HIDDEN", fullApp ? L", fullscreen notice" : L"", hidden ? L", hidden for fullscreen" : L"",
               strips.size());
    OutputDebugStringW(line);
    PerfWrite(line);
}

void Drawer::Govern(bool fast, double gap) {
    bool steady = fast && lastFast && gap > 0 && gap < 3.5 * fastSync / hz; // unbroken animation only; one long stall (a disk hiccup, a drag) isn't the machine's pace
    lastFast = fast;
    if (!steady || hz < 50) return;
    ++framesSeen;
    if (gap > 1.5 * fastSync / hz) ++framesLate;
    if (framesSeen < 90) return; // judge a window of frames, not one
    double now = Seconds();
    if (fastSync == 1 && framesLate * 2 > framesSeen) { // most frames late: a steady lower rate looks smoother than a stuttering high one
        fastSync = hz > 100 ? (UINT)std::lround(hz / 60) : 2;
        syncRetry = now + 30;
        Log(L"animation stepped down, frames were landing late");
    } else if (fastSync > 1 && now > syncRetry) { // a while on: maybe it was only busy, try full rate again
        fastSync = 1;
    }
    framesSeen = framesLate = 0;
}

void Drawer::Perf(double drawSec) {
    double now = Seconds();
    ++perfN;
    perfDraw += drawSec;
    if (perfT0 == 0) perfT0 = now;
    if (now - perfT0 < 5) return;
    wchar_t line[256];
    swprintf_s(line, L"%.0f frames/s, %.2f ms avg draw, %.1f%% of a core | why: spring %d drag %d ui %d theme %d ambient %d input %d\n",
               perfN / (now - perfT0), perfDraw * 1000 / perfN, perfDraw * 100 / (now - perfT0),
               perfWhy[0], perfWhy[1], perfWhy[2], perfWhy[3], perfWhy[4], perfWhy[5]);
    for (int& n : perfWhy) n = 0;
    OutputDebugStringW(line);
    PerfWrite(line);
    perfT0 = now;
    perfN = 0;
    perfDraw = 0;
}

float Drawer::Rubber(float raw) const {
    if (raw <= 0) return 0;
    float full = Depth();
    if (raw <= full) return raw;
    float over = raw - full;
    return full + overshoot * 0.8f * (1 - std::exp(-over / (overshoot * 2))); // rubber band past full height, caps at 80% of overshoot
}

LRESULT CALLBACK Drawer::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) { // stash this ptr from CreateWindowEx
        auto* self = (Drawer*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd = hwnd; // set early, msgs arrive before Create returns
    }
    auto* self = (Drawer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Drawer::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // clicks never steal focus from user's app
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p = ToDip(lp);
        TrackLeave();
        if (dragging) {
            MoveDrag(DepthOf(p));
            return 0;
        }
        if (peeking && (!opened || (peekToastNow && Inside(MenuPeekBox(), p)))) { // a notification pop-up: hovering pauses auto-dismiss, tracks stack controls, and arms hover-dwell expansion
            TrackLeave();
            KillTimer(hwnd, kPeekTimer);
            if (opened) {
                KillTimer(hwnd, kCloseTimer);
                input.mouse = {-1e6f, -1e6f};
            }
            bool wasHot = peekHot;
            peekHot = true;
            bool toastUi = peekToastNow && (opened || !Side());
            float boxH = opened ? peekReach : (std::max)(spring.value, kIslandH);
            bool closeHov = toastUi && Inside(PeekCloseBox(boxH), p);
            bool prevHov = toastUi && !peekExpanded && peekStack.size() > 1 && Inside(PeekPrevBox(), p);
            bool nextHov = toastUi && !peekExpanded && peekStack.size() > 1 && Inside(PeekNextBox(), p);
            bool clearHov = toastUi && peekExpanded && peekStack.size() > 1 && Inside(PeekClearBox(), p);
            int hovRow = -1, clsRow = -1;
            if (toastUi && peekExpanded && peekStack.size() > 1) {
                int rows = PeekShownStackRows();
                for (int s = 0; s < rows; ++s) {
                    if (Inside(PeekStackRowCloseBox(s), p)) {
                        clsRow = s;
                        hovRow = s;
                        break;
                    }
                    if (Inside(PeekStackRowBox(s), p)) {
                        hovRow = s;
                        break;
                    }
                }
            }
            if (!wasHot && toastUi && !peekExpanded && (IsPeekTruncated() || peekStack.size() > 1)) {
                peekDwellAt = Seconds();
                SetTimer(hwnd, kPeekExpandTimer, kPeekDwellMs, nullptr);
            }
            if (!wasHot || closeHov != peekCloseHot || prevHov != peekPrevHot || nextHov != peekNextHot ||
                clearHov != peekClearHot || hovRow != peekHoverRow || clsRow != peekCloseRow) {
                peekCloseHot = closeHov;
                peekPrevHot = prevHov;
                peekNextHot = nextHov;
                peekClearHot = clearHov;
                peekHoverRow = hovRow;
                peekCloseRow = clsRow;
                Redraw();
            }
            hand = true;
            return 0;
        }
        if (opened && peeking && peekHot) {
            KillTimer(hwnd, kPeekExpandTimer);
            bool wasExp = peekExpanded;
            peekHot = peekCloseHot = peekPrevHot = peekNextHot = peekClearHot = false;
            peekHoverRow = peekCloseRow = -1;
            if (wasExp) {
                peekExpanded = false;
                UpdatePeekLayout();
            }
            if (peekToastNow) SetTimer(hwnd, kPeekTimer, 3500, nullptr);
            Redraw();
        }
        if (!opened && !islandLook && spring.value < 1) { // on the closed strip; the island handles its own hover
            if (cfg.hoverOpen && !armed) {
                armed = true; // once per hover, even when nothing is going to open
                islandNext = AppBelow(); // one check per hover, nothing polls
                if (model.keyOnly || (islandNext && model.appKeyOnly)) return 0; // only the shortcut opens it: everywhere, or over an app
                EnsureDev(); // warm up gpu during hover delay
                SetTimer(hwnd, kHoverTimer, islandNext ? (UINT)model.appHoverMs : (UINT)cfg.hoverMs, nullptr); // open only if mouse lingers
            }
            return 0;
        }
        KillTimer(hwnd, kCloseTimer); // back inside, cancel pending close
        bool ctlDirty = false;
        if (islandLook && !opened) {
            bool musicUp = IslandMusicActive();
            float hCur = (std::max)(kIslandH, spring.value);
            bool vHot = musicUp && Inside(IslandVizZone(hCur), p);
            int cBtn = (vHot && islandCtlT > 0.25f) ? IslandCtlAt(p, hCur) : -1;
            if (vHot != islandVizHot || cBtn != islandCtlBtn) {
                islandVizHot = vHot;
                islandCtlBtn = cBtn;
                ctlDirty = true;
            }
        }
        if (islandLook && (!islandHot || ctlDirty)) { // entering the island or moving across mini visualizer controls
            islandHot = true;
            Redraw();
        }
        // while a pill shows, the panel only ever opens on a click: hovering the pill never drops it
        input.mouse = p;
        D2D1_POINT_2F org = BodyOrg((std::clamp)(spring.value, 0.0f, Depth() + overshoot)); // panel space as the frame draws it, on every edge
        D2D1_POINT_2F local = {p.x - org.x, p.y - org.y};
        bool onRail = opened && Inside(RailBox(), local);
        if (ui.Captured() || ui.HitAt(local) != ui.Hovered() || onRail != railHot) Redraw(); // hover target changed, or the rail's arrow keys come or go
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        KillTimer(hwnd, kHoverTimer);
        armed = false;
        islandHot = islandVizHot = false;
        islandCtlBtn = -1;
        if (peeking) {
            KillTimer(hwnd, kPeekExpandTimer);
            peekHot = peekCloseHot = peekPrevHot = peekNextHot = peekClearHot = false;
            peekHoverRow = peekCloseRow = -1;
            if (peekExpanded) {
                peekExpanded = false;
                UpdatePeekLayout();
            }
            SetTimer(hwnd, kPeekTimer, model.notifyWatch.More() ? 1800 : 2600, nullptr);
        }
        input.mouse = {-1e6f, -1e6f}; // park offscreen to clear hover
        SyncRailKeys(false); // off the panel: the arrows go back at once, not on the next frame
        if ((opened || island) && !dragging && GetCapture() != hwnd && !(opened && Pinned())) { // pinned: it stays open
            bool pill = island && !opened;
            if (!(pill && Stays())) // always: the pill stays put
                SetTimer(hwnd, kCloseTimer, pill ? model.IslandLingerMs((UINT)cfg.closeMs) : (UINT)cfg.closeMs, nullptr); // close after grace period, the pill can linger
        }
        ReleaseLater(); // brushed the strip without opening: the hover warm-up made a device
        Redraw();
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp); // all timers are one-shot
        if (wp == kFollowTimer) { // open, every screen mode: the cursor crossed to another screen, the panel goes with it
            POINT pt;
            GetCursorPos(&pt);
            HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            if (opened && !dragging && m && m != mon) {
                followTo = m;
                Close(); // up here, then down there once it's out of sight
            } else if (opened) {
                SetTimer(hwnd, kFollowTimer, kFollowMs, nullptr);
            }
            return 0;
        }
        if (wp == kPasteTimer) { // the app in front has the keyboard back: send it ctrl+v
            HWND to = pasteTo;
            pasteTo = nullptr;
            if (to && GetForegroundWindow() == to) { // moved on meanwhile: it stays on the clipboard, nothing is typed elsewhere
                INPUT in[4] = {};
                for (INPUT& i : in) i.type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[3].ki.wVk = VK_CONTROL;
                in[1].ki.wVk = in[2].ki.wVk = 'V';
                in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(4, in, sizeof(INPUT));
            }
            return 0;
        }
        if (wp == kAdoptTimer) { // the strip on another screen was held: is the mouse still at that screen's edge
            HMONITOR m = adoptTo;
            adoptTo = nullptr;
            POINT pt;
            GetCursorPos(&pt);
            bool there = false;
            for (HWND s : strips) {
                RECT r;
                if (GetWindowRect(s, &r) && MonitorFromWindow(s, MONITOR_DEFAULTTONULL) == m && (InflateRect(&r, 2, 2), PtInRect(&r, pt))) there = true; // brushed past and gone: stays put
            }
            if (there && island && !opened && !dragging && !peeking && !stage) {
                followTo = m;
                followCall = true;
                Close(); // up on this screen, then it answers on that one
                Log(L"pill called from another screen");
            }
            return 0;
        }
        if (wp == Model::kCalWaitTimer) { // the calendar link never came
            model.StopCalWait();
            Redraw();
            return 0;
        }
        if (wp == kLongTimer) { // lingers as the small pill would, for as long as the mouse is on it or something holds it up
            if (longHeld && (Stays() || (desk && desk->MouseOn()) || MouseIn())) SetTimer(hwnd, kLongTimer, model.IslandLingerMs((UINT)cfg.closeMs), nullptr);
            else longHeld = false, SyncDesk();
            return 0;
        }
        if (wp == kDeskTimer) {
            OnDeskTimer();
            return 0;
        }
        if (wp == kGreetTimer) {
            OnGreetTimer();
            return 0;
        }
        if (wp == kFullTimer) { // the shell said fullscreen: is there really one in front?
            OnFullTimer();
            return 0;
        }
        if (wp == kDevTimer) { // try the device again: the frame picks up where it left off
            needsFrame = true;
            return 0;
        }
        if (wp == kHoverTimer) {
            armed = false;
            if (dragging || GetCapture() == hwnd) {} // a press on the strip already decides: a pill dropping mid-drag would shrink the window under the finger
            else if (!MouseIn()) ReleaseLater();
            else if (islandNext) ShowIsland(); // an app below: the pill first, a tap opens the panel
            else Open();
        } else if (wp == kCloseTimer) {
            if (!dragging && GetCapture() != hwnd && !MouseIn() && !(island && !opened && Stays()) && !(opened && Pinned()) && tourStep < 0) Close(); // the tour keeps it open until it's done
        } else if (wp == kTrimTimer) {
            if (island && !opened && !peeking && !dragging && !(devWide && RenewDev())) gfx::GiveBack(); // a resting pill, only its bars move: idle pages go back; after the panel, a fresh device too. d2d's caches stay: cleared under live brushes, the media page's glows came back flat
        } else if (wp == kReleaseTimer) {
            if (!opened && !peeking && spring.value <= 0) FreeDev(); // free gpu after idle
            else if (!opened && !peeking && !dragging && !(devWide && RenewDev())) TrimDev(); // the pill keeps drawing on a fresh device; if one can't be made, the old one just drops its caches
        } else if (wp == kPeekTimer) {
            if (peeking) DismissPeek();
        } else if (wp == kPeekExpandTimer) {
            if (peeking && peekToastNow && peekHot && !peekExpanded && (IsPeekTruncated() || peekStack.size() > 1)) {
                peekExpanded = true;
                peekPrevHot = peekNextHot = false;
                UpdatePeekLayout();
            }
        }
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F p = ToDip(lp);
        if (!opened && model.keyOnly) return 0; // only the shortcut opens it: a click on the strip, the pill or a notification does nothing
        auto handlePeekClick = [&](D2D1_POINT_2F pt) -> bool {
            if (peekToastNow && (opened || !Side())) {
                float boxH = opened ? peekReach : (std::max)(spring.value, kIslandH);
                if (Inside(PeekCloseBox(boxH), pt)) {
                    DismissPeekItem(peekIdx);
                    return true;
                }
                if (!peekExpanded && peekStack.size() > 1) {
                    if (Inside(PeekPrevBox(), pt)) {
                        StepPeekStack(-1);
                        return true;
                    }
                    if (Inside(PeekNextBox(), pt)) {
                        StepPeekStack(1);
                        return true;
                    }
                }
                if (peekExpanded && peekStack.size() > 1) {
                    if (Inside(PeekClearBox(), pt)) {
                        peekStack.clear();
                        DismissPeek();
                        return true;
                    }
                    int rows = PeekShownStackRows();
                    for (int s = 0; s < rows; ++s) {
                        int realIdx = PeekRowRealIdx(s);
                        if (realIdx < 0) continue;
                        if (Inside(PeekStackRowCloseBox(s), pt)) {
                            DismissPeekItem(realIdx);
                            return true;
                        }
                        if (Inside(PeekStackRowBox(s), pt)) {
                            peekIdx = realIdx;
                            SyncPeekFields();
                            if (!model.reduceMotion && !opened) spring.vel += 35.0f;
                            UpdatePeekLayout();
                            return true;
                        }
                    }
                }
            }
            if (peekToastNow && peekToast.launch == L"astraeus:tools") {
                int act = model.Activity();
                model.toolTab = act == Model::kActWatch ? 1 : act == Model::kActFocus ? 2 : 0;
                if (opened) {
                    SetPage(Model::kPgTools);
                    DismissPeekItem(peekIdx);
                } else {
                    if (page != Model::kPgTools && model.HasPage(Model::kPgTools)) prevPage = page, page = Model::kPgTools, pageT = 1;
                    pageSet = true;
                    Open();
                }
                return true;
            }
            if (peekToastNow && (!peekToast.aumid.empty() || !peekToast.launch.empty())) {
                sys::Notif n;
                n.aumid = peekToast.aumid;
                n.launch = peekToast.launch;
                sys::OpenNotifApp(n);
                DismissPeekItem(peekIdx);
                return true;
            }
            return false;
        };
        if (opened && peeking && peekToastNow && Inside(MenuPeekBox(), p)) {
            if (!handlePeekClick(p)) DismissPeekItem(peekIdx);
            return 0;
        }
        if ((island || peeking || zoomActive) && !opened) { // tap a pill (or reverse a zoom close): the full panel grows out of it, or a notification opens its app
            if (peeking && handlePeekClick(p)) return 0;
            if (island && !peeking && !opened && IslandMusicActive()) {
                float hCur = (std::max)(kIslandH, spring.value);
                int btn = IslandCtlAt(p, hCur);
                if (btn >= 0) {
                    if (btn == 0 && model.track.canPrev) model.PrevTrack();
                    else if (btn == 1) model.media.TogglePlay();
                    else if (btn == 2 && model.track.canNext) model.NextTrack();
                    Redraw();
                    return 0;
                }
            }
            if (island && Bubble() && Inside(BubbleBox(), p)) { // the bubble: straight to the tool that runs
                int act = model.Activity();
                model.toolTab = act == Model::kActWatch ? 1 : act == Model::kActFocus ? 2 : 0;
                if (page != Model::kPgTools && model.HasPage(Model::kPgTools)) prevPage = page, page = Model::kPgTools, pageT = 1;
                pageSet = true;
            }
            Open();
            return 0;
        }
        if (!opened && spring.value < 1 && model.appKeyOnly && AppBelow()) return 0; // over an app with shortcut only: the bare strip opens nothing
        SetCapture(hwnd); // keep getting moves outside window while held
        if (!opened && spring.value < 1) {
            StartDrag(DepthOf(p)); // pull out from the strip
            return 0;
        }
        if (input.released && !dragging) Frame(); // the last release hasn't had its frame yet: deliver it first, or its widget still holds the mouse and this press goes nowhere
        input.mouse = p;
        input.pressPos = p;
        input.pressed = true;
        input.down = true;
        Frame(); // run ui now to see what was hit
        if (ui.FreePress() && !Pinned()) StartDrag(DepthOf(p)); // press on empty area drags panel; pinned, it doesn't move
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging) {
            ReleaseCapture();
            EndDrag();
            return 0;
        }
        input.mouse = ToDip(lp);
        input.released = true;
        input.down = false;
        ReleaseCapture();
        Redraw();
        if (!MouseIn() && opened && !Pinned()) SetTimer(hwnd, kCloseTimer, (UINT)cfg.closeMs, nullptr); // released outside, leave was suppressed by capture
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging) EndDrag(); // capture stolen, finish drag cleanly
        if (input.down) { // synthesize release so widgets don't stick
            input.down = false;
            input.released = true;
            Redraw();
        }
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        float notches = GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA; // in notches
        POINT sp = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; // wheel messages carry screen coordinates
        ScreenToClient(hwnd, &sp);
        D2D1_POINT_2F p = {sp.x / scale, sp.y / scale};
        D2D1_POINT_2F org = BodyOrg((std::clamp)(spring.value, 0.0f, Depth() + overshoot));
        bool rail = opened && Inside(RailBox(), {p.x - org.x, p.y - org.y}); // the rail as the frame draws it, any edge or layout
        if (msg == WM_MOUSEHWHEEL && opened && !rail) return 0; // a sideways swipe means pages only over the rail
        if (peeking && peekToastNow && peekStack.size() > 1 && (!opened || Inside(MenuPeekBox(), p))) {
            StepPeekStack(notches < 0 ? 1 : -1);
            if (!peekHot) SetTimer(hwnd, kPeekTimer, 4200, nullptr);
            return 0;
        }
        if (rail || (island && !opened)) {
            WheelPages(msg == WM_MOUSEHWHEEL ? notches : -notches); // wheel down or swipe right: the next page
            return 0;
        }
        input.wheel += notches; // accumulates till next frame
        Redraw();
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW)); // hand set by ui over clickables
        return TRUE;
    case kBarMsg: // pinned: the shell moved bars around, or a fullscreen app came
        if (wp == ABN_POSCHANGED && barOn) Reserve(true);
        return 0;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        boards.clear(); // the dashboards are rebuilt for the new screens on the next second
        sys::ForgetScreenIds();
        FreeDev(); // swapchain size/dpi stale, rebuild lazily
        twinSplit = -1; // a fullscreen pair looks again for its other screen
        if (!screensNow) mon = sys::FindScreen(screenNow); // the picked screen may have just been plugged back in
        model.screenList.clear(); // settings re-reads the monitors next frame
        Place();
        BuildStrips(); // screens may have come or gone
        Log(msg == WM_DPICHANGED ? L"dpi changed" : L"displays changed");
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) Place(), Redraw(); // a taskbar came, went or changed size: the sidebar fits the new room
        model.ReadAccess(); // high contrast or the text size may be what changed
        if (model.HighContrast() != theme::Contrast() || model.ZoomPct() != zoomNow) Redraw();
        if (wp == SPI_SETDESKWALLPAPER) {
            if (Clear()) SyncWallTone(), Redraw();
            if (desk) desk->LookBehind();
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_THEMECHANGED: // high contrast's colors themselves
        if (theme::Contrast()) theme::SetContrast(false), Redraw(); // read afresh next frame
        return 0;
    case WM_GETOBJECT: {
        LRESULT out = 0;
        if (access.OnGetObject(wp, lp, out)) return out;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case Access::kMsg:
        access.Apply(ui, wp, lp);
        Redraw();
        return 0;
    case WM_KEYUP:
        if (wp == VK_RETURN || wp == VK_SPACE) ui.KeyHold(false), Redraw();
        return 0;
    case WM_APP_CAL:
        if (wp == 1) model.cals.Sync(hwnd, WM_APP_CAL, 0); // a link came in while the last fetch ran: fetched now
        if (model.calOpen) Redraw();
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (model.OnClipboard()) Redraw(); // connected: the calendar shows its events as they arrive
        return 0;
    case WM_APP_MEDIA:
        model.OnMedia();
        SyncStay();
        if (desk) desk->Refresh();
        Redraw();
        return 0;
    case WM_APP_FEEDS:
        if (opened && page == Model::kPgWidgets) Redraw();
        return 0;
    case WM_APP_BRIGHT:
        model.OnBright(wp);
        Redraw();
        return 0;
    case WM_APP_WEATHER:
        model.OnWx();
        Redraw();
        return 0;
    case WM_APP_WS: // the workspaces changed: the pill shows it even closed
        model.wsNow = model.ws.Snap();
        Redraw();
        return 0;
    case WM_APP_UPDATES:
        model.updateNow = model.updates.Get();
        Redraw();
        return 0;
    case WM_APP_PING:
        model.hostNow = model.pinger.Results();
        if (opened) Redraw();
        return 0;
    case WM_APP_STATS:
        model.OnStats();
        Redraw();
        return 0;
    case WM_APP_BT:
        model.OnBt();
        Redraw();
        return 0;
    case WM_APP_WIFI:
        model.OnWifi();
        Redraw();
        return 0;
    case kAdoptMsg: { // hovered the strip on another screen
        HMONITOR m = (HMONITOR)wp;
        if (model.keyOnly) return 0; // only the shortcut opens it: another screen's strip brings nothing either
        if (island && !opened && !dragging && !peeking && !stage && m != mon) { // a resting pill here: after a moment's dwell it goes up and comes down there
            if (adoptTo != m) adoptTo = m, SetTimer(hwnd, kAdoptTimer, (UINT)cfg.hoverMs, nullptr);
            return 0;
        }
        if (!opened && !dragging && !peeking && !followTo && spring.value <= 0.5f && m != mon) { // a pill already on its way over moves itself
            mon = m;
            FreeDev(); // other screen, maybe other dpi
            Place();
            BuildStrips();
            SyncHide();
            Log(L"moved to the hovered screen");
        }
        return 0;
    }
    case WM_APP_PICKED: // the color picker finished
        if (lp) {
            wchar_t hex[8];
            swprintf_s(hex, L"#%06X", (unsigned)wp);
            Peek(std::wstring(L"Copied ") + hex, L"Picked from the screen", glyph::Copy);
        }
        return 0;
    case WM_APP_NOTIFY: { // a windows notification arrived and pills are on, ours or handed over by halobar
        sys::Toast toast;
        if (model.quiet || hidden) { // do not disturb or a game: dropped, all of them, never shown stale later
            while (model.notifyWatch.Pop(toast)) {}
            return 0;
        }
        while (model.notifyWatch.Pop(toast)) PeekToast(std::move(toast));
        return 0;
    }
    case WM_APP_SHARE:
        model.OnShare();
        Redraw();
        return 0;
    case WM_APP_ACCENT: // windows' accent changed: the next frame crossfades to it
        model.winAccentHex = model.winAccent.Read();
        Redraw();
        return 0;
    case WM_APP_HUDVOL:
        if (model.hud && !opened) ShowHud(lp || wp == 0 ? glyph::VolumeOff : wp < 500 ? glyph::VolumeDown : glyph::VolumeUp, lp ? 0.0f : wp / 1000.0f, lp ? L"Muted" : L"Volume");
        return 0;
    case WM_APP_HUDBRIGHT:
        if (model.hud && !opened) ShowHud(glyph::Bright, (float)wp / 100, L"Brightness");
        return 0;
    case WM_APP_VOLKEY:
        OnVolumeKey((UINT)wp);
        return 0;
    case WM_APP_SHELF: // a preview came in, an image was kept, or the clipboard brought something
        model.shelf.OnWork();
        Redraw();
        return 0;
    case kDeskEditMsg:
        if (wp) model.studioKind = (int)wp, WidgetStudio();
        else DeskEdit((int)lp);
        return 0;
    case kShelfMsg:
        if (wp == 1) ShelfPaste();
        else ShelfOut();
        return 0;
    case WM_CHAR: // only reaches us while a field holds the keyboard
        if (model.faceSearch) { // the widget gallery's search
            wchar_t ch = (wchar_t)wp;
            if (ch == VK_ESCAPE) {
                if (model.faceQuery.empty()) model.faceSearch = false;
                else model.faceQuery.clear();
            } else if (ch == VK_BACK) {
                if (!model.faceQuery.empty()) model.faceQuery.pop_back();
            } else if (ch >= 32 && model.faceQuery.size() < Model::kFaceQueryMax) {
                model.faceQuery += ch;
            }
            Redraw();
            return 0;
        }
        if (model.faceKeys) {
            if (!faces::Char(model.facePanel, (wchar_t)wp) && wp == VK_ESCAPE) model.CloseFace();
            Redraw();
            return 0;
        }
        if (model.passFocus) {
            wchar_t ch = (wchar_t)wp;
            if (ch == 22) { // ctrl+v, first line only, appended in place
                std::wstring clip = sys::ClipText();
                for (wchar_t c : clip) {
                    if (c == L'\r' || c == L'\n') break;
                    if (c >= 32 && c < 127 && model.passEdit.size() < 63) model.passEdit += c;
                }
                SecureZeroMemory(clip.data(), clip.size() * sizeof(wchar_t));
            } else if (ch == 8) {
                if (!model.passEdit.empty()) model.passEdit.back() = 0, model.passEdit.pop_back();
            } else if (ch >= 32 && ch < 127 && model.passEdit.size() < 63) { // wpa passphrases are printable ascii, 63 at most
                model.passEdit += ch;
            }
            model.passWrong = false;
            Redraw();
            return 0;
        }
        if (Entry::Get().focus) {
            Entry::Get().Type((wchar_t)wp);
            Redraw();
            return 0;
        }
        if (model.shelfField != Model::kShelfNone) {
            ShelfKey(wp);
            Redraw();
            return 0;
        }
        if (model.field != Model::kFieldNone) { // a to-do or a server being typed
            wchar_t ch = (wchar_t)wp;
            if (ch == 22) { // ctrl+v, the first line
                for (wchar_t c : sys::ClipText()) {
                    if (c == L'\n' || c == L'\n') break;
                    if (c >= 32 && model.fieldEdit.size() < Model::kFieldMax) model.fieldEdit += c;
                }
            } else if (ch == 8) {
                if (!model.fieldEdit.empty()) model.fieldEdit.pop_back();
            } else if (ch >= 32 && ch != 127 && model.fieldEdit.size() < Model::kFieldMax) {
                model.fieldEdit += ch;
            }
            Redraw();
            return 0;
        }
        if (model.nameFor >= 0) { // a mixer group's name
            wchar_t ch = (wchar_t)wp;
            if (ch == 8) {
                if (!model.nameEdit.empty()) model.nameEdit.pop_back();
            } else if (ch >= 32 && ch != 127 && model.nameEdit.size() < Model::kNameMax) {
                model.nameEdit += ch;
            }
            Redraw();
            return 0;
        }
        if (model.cityFocus) {
            wchar_t ch = (wchar_t)wp;
            if (ch == 22) { // ctrl+v, the first line
                for (wchar_t c : sys::ClipText()) {
                    if (c == L'\r' || c == L'\n') break;
                    if (c >= 32 && model.cityEdit.size() < kCityMax) model.cityEdit += c;
                }
            } else if (ch == 8) {
                if (!model.cityEdit.empty()) model.cityEdit.pop_back();
            } else if (ch >= 32 && ch != 127 && model.cityEdit.size() < kCityMax) {
                model.cityEdit += ch;
            }
            Redraw();
            return 0;
        }
        if (model.hexFocus) {
            wchar_t ch = (wchar_t)wp;
            if (ch == 22) { // ctrl+v
                int rgb = 0;
                if (Model::ParseHex(sys::ClipText(), rgb)) {
                    wchar_t buf[8];
                    swprintf_s(buf, L"%06X", rgb);
                    model.hexEdit = buf;
                }
            } else if (ch == 8) {
                if (!model.hexEdit.empty()) model.hexEdit.pop_back();
            } else if (iswxdigit(ch) && model.hexEdit.size() < 6) {
                model.hexEdit += (wchar_t)towupper(ch);
            }
            Redraw();
            return 0;
        }
        if (model.calFocus) {
            wchar_t ch = (wchar_t)wp;
            if (ch == 22) { // ctrl+v, first line trimmed
                for (wchar_t c : sys::ClipText()) {
                    if (c == L'\r' || c == L'\n') break;
                    if (c >= 32 && model.calEdit.size() < 2048) model.calEdit += c;
                }
            } else if (ch == 8) {
                if (!model.calEdit.empty()) model.calEdit.pop_back();
            } else if (ch >= 32 && ch != 127 && model.calEdit.size() < 2048) {
                model.calEdit += ch;
            }
            model.calErr = false;
            Redraw();
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && keys && opened && !model.Typing() && !Entry::Get().focus) { // clicked away from a panel opened by the keyboard: it goes too
            keys = false;
            if (!Pinned()) Close(); // pinned: only the keyboard goes back
        }
        return 0;
    case WM_SYSKEYDOWN: // alt held: only a fullscreen key being picked in settings wants it
        if (!StagePrefs::Get().capture) break;
        [[fallthrough]];
    case WM_KEYDOWN:
        if (int which = StagePrefs::Get().capture) { // settings is waiting for a fullscreen key: the next real key is it, esc keeps the old one
            StagePrefs& sp = StagePrefs::Get();
            if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_LWIN || wp == VK_RWIN) return 0; // a modifier alone: wait for the key with it
            if (wp != VK_ESCAPE) sp.SetKey(which == 1 ? sp.keySplit : sp.keySwap, which == 1 ? L"StageSplitKey" : L"StageSwapKey", StagePrefs::Combo((UINT)wp));
            sp.capture = 0;
            Redraw();
            return 0;
        }
        if (keys && !model.Typing() && !Entry::Get().focus) { // a panel opened from the shortcut: move with the keys
            if (KeyNav(wp, lp)) {
                Redraw();
                return 0;
            }
            if (wp == VK_ESCAPE && Pinned()) { // pinned: esc hands the keyboard back, the panel stays
                keys = false;
                if (keysFrom && IsWindow(keysFrom)) SetForegroundWindow(keysFrom);
            } else if (wp == VK_ESCAPE) Close();
            else if (wp == VK_RIGHT || wp == VK_DOWN) SetPage(model.PageAt(model.SlotOf(page) + 1)); // the rail's order, not the pages' numbers
            else if (wp == VK_LEFT || wp == VK_UP) SetPage(model.PageAt(model.SlotOf(page) - 1));
            else if (wp >= '1' && wp < '1' + (WPARAM)model.pages.size()) SetPage(model.PageAt((int)(wp - '1')));
            Redraw();
            return 0;
        }
        if (model.passFocus) {
            if (wp == VK_RETURN) model.SubmitPass();
            else if (wp == VK_ESCAPE) model.CancelPass();
            Redraw();
            return 0;
        }
        if (Entry::Get().focus) {
            if (wp == VK_RETURN) Entry::Get().Submit();
            else if (wp == VK_ESCAPE) Entry::Get().Drop();
            Redraw();
            return 0;
        }
        if (model.shelfField != Model::kShelfNone) {
            if (model.shelfField == Model::kShelfNote && wp == VK_RETURN) model.CommitShelfNote();
            else if (model.shelfField == Model::kShelfNote && wp == VK_ESCAPE) model.shelfField = Model::kShelfNone, model.shelfNoteFor = 0, model.shelfEdit.clear(); // the note stays as it was
            else if (wp == VK_RETURN || wp == VK_ESCAPE) { // the search: enter keeps it, esc empties it first
                if (wp == VK_ESCAPE && !model.shelfQuery.empty()) model.shelfQuery.clear();
                else model.shelfField = Model::kShelfNone;
            }
            Redraw();
            return 0;
        }
        if (model.faceKeys || model.faceSearch) return 0; // their enter and esc arrive as characters
        if (model.field != Model::kFieldNone) {
            if (wp == VK_RETURN) model.CommitField();
            else if (wp == VK_ESCAPE) model.field = Model::kFieldNone, model.fieldEdit.clear();
            Redraw();
            return 0;
        }
        if (model.nameFor >= 0) {
            if (wp == VK_RETURN) model.CommitName();
            else if (wp == VK_ESCAPE) model.nameFor = -1;
            Redraw();
            return 0;
        }
        if (model.cityFocus) {
            if (wp == VK_RETURN) model.CommitCity();
            else if (wp == VK_ESCAPE) model.cityFocus = false;
            Redraw();
            return 0;
        }
        if (model.hexFocus) {
            if (wp == VK_RETURN) model.CommitHex();
            else if (wp == VK_ESCAPE) model.hexFocus = false;
            Redraw();
            return 0;
        }
        if (model.calFocus) {
            if (wp == VK_RETURN) model.CommitCalUrl();
            else if (wp == VK_ESCAPE) model.calFocus = false, model.calErr = false;
            Redraw();
            return 0;
        }
        break;
    case WM_HOTKEY: // the arrows, held only while the mouse rests on the rail: the next or previous page, scrolled into view
        if ((int)wp == kBoardKey) { // ctrl+alt+d: the screen under the mouse, dashboard or desktop
            BoardKey();
            return 0;
        }
        if ((int)wp == kBoardFocusKey) {
            if (model.a11yKeys) BoardFocus();
            return 0;
        }
        if ((int)wp != kRailKeyPrev && (int)wp != kRailKeyNext) break;
        if (opened) SetPage(model.PageAt(model.SlotOf(page) + ((int)wp == kRailKeyNext ? 1 : -1)));
        Redraw();
        return 0;
    case WM_RBUTTONDOWN:
        return 0;
    case WM_RBUTTONUP: {
        D2D1_POINT_2F rp = ToDip(lp);
        if (opened && peeking && peekToastNow && Inside(MenuPeekBox(), rp)) {
            DismissPeek(); // right-click on the menu-width notification pill dismisses it
            return 0;
        }
        if (opened) { // right click only means something inside the open panel
            input.mouse = rp;
            input.rclick = true;
            Redraw();
        } else if (peeking) {
            DismissPeek(); // right-click on a Dynamic Island notification dismisses it
        } else if (island && !peeking && !dragging && desk && !stage) { // the regular pill: into the long pill's editor, which asks first if neither place wears it
            PostMessageW(hwnd, kDeskEditMsg, 0, deskView ? Model::kEditFromDesk : Model::kEditFromApps);
        }
        return 0;
    }
    case WM_APP_RADIOS:
    case WM_APP_WANIP:
        Redraw();
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) locked = true;
        else if (wp == WTS_SESSION_UNLOCK) locked = false, Redraw(); // the bars pick up where the music is
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            const auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (s) OnPower(*s); // plugged in or out, battery percent; it ignores the rest
            if (s && s->PowerSetting == kDisplayState && s->DataLength >= sizeof(DWORD)) {
                screenOff = *reinterpret_cast<const DWORD*>(s->Data) == 0; // 0 off, 1 on, 2 dimmed: dimmed still shows
                if (!screenOff) Redraw();
            }
        }
        break; // the rest are windows' own business
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Drawer::PaintShape(ID2D1Geometry* shape, float h) {
    if (!shape || ClearPill()) return; // clear's pill: the ink, straight on what's behind it
    Color body = shellNow.bg;
    if (!shellNow.glass) body.a *= model.BgOpacity() / 100.0f; // the solid looks can let the desktop through a little
    if (skyPass != 1) canvas->FillGeometry(shape, body, true); // dark mode: black, blends with screen bezel. kept: a shape at rest is tessellated once
    D2D1_RECT_F sky{};
    if (shellNow.stars && SUCCEEDED(shape->GetBounds(nullptr, &sky))) { // starfall: the pill and the panel both wear the sky; a resting pill bakes it once into its layer
        bool alive = opened && !islandLook && !model.reduceMotion; // the open panel's orb drifts and its dust breathes; the pill and calm motion hold still
        float t = alive ? (float)std::fmod(lastFrame, kSkyWrap) + 0.001f : 0;
        D2D1_RECT_F r = {sky.left + cfg.fillet, sky.top, sky.right - cfg.fillet, sky.bottom};
        if (skyPass == 0) {
            starfall::Sky(*canvas, r, 0, shellNow, t, body.a, 0, shape, 1, alive);
        } else if (Width(r) >= 4 && Height(r) >= 4 && body.a >= 0.01f) { // Sky's own guard
            if (skyPass == 1) {
                starfall::SkyOver(*canvas, r, 0, shellNow, t, body.a, 0, shape, 1, starfall::kStill);
            } else { // the orb drifts between the wash and the still parts' layer, the breathing motes go on top
                starfall::SkyUnder(*canvas, r, 0, shellNow, body.a, 0, shape);
                starfall::SkyOrb(*canvas, r, 0, shellNow, t, body.a, 0, shape, alive);
                D2D1_MATRIX_3X2_F was;
                ctx->GetTransform(&was);
                ctx->SetTransform(D2D1::Matrix3x2F::Identity()); // the layer is the whole surface, pixel for pixel
                ctx->DrawImage(skyLayer.Get());
                ctx->SetTransform(was);
                starfall::SkyOver(*canvas, r, 0, shellNow, t, body.a, 0, shape, 1, starfall::kTwinkle);
            }
        }
    }
    if (shellNow.glass && glassShadeNow > 0.01f) canvas->FillGeometry(shape, Tint(glassShadeNow), true); // over a page that fights the ink
    if (shellNow.glass) { // liquid glass: glossy light from above and a bright specular rim
        canvas->FillGeoV(shape, {0, 0, W, (std::min)(h, 90.0f)}, shellNow.sheen, shellNow.sheen.Alpha(0), true);
        canvas->FillGeoV(shape, {0, h - (std::min)(h * 0.5f, 34.0f), W, h}, Color{1, 1, 1, 0}, Color{1, 1, 1, 0.09f}, true); // light caught in the lower lip, like a drop of glass
        canvas->StrokeGeo(shape, Color{1, 1, 1, 0.26f}, 1.2f, true);
    }
}

void Drawer::Frame() {
    if (stage) { // stage paces the loop while it's up, the panel only draws if it's still retracting
        StageFrame();
        if (!needsFrame) return;
    }
    double now = Seconds();
    double gap = lastFrame > 0 ? now - lastFrame : 0; // the real interval, for the governor
    float dt = (float)(std::min)(now - lastFrame, 0.05); // clamp so a stall doesn't jump the spring
    if (lastFrame == 0) dt = 1 / 60.0f; // first frame, assume 60hz
    lastFrame = now;

    bool hMoving = !dragging && !zoomActive && spring.Step(dt); // drag drives value directly
    bool wMoving = widthSpring.Step(dt);
    bool zMoving = !dragging && zoomActive && zoomSpring.Step(dt, 0.0012f, 0.02f); // normalized 0..1 spring needs sub-thousandth settle thresholds
    bool mpMoving = (opened || menuPeekSpring.value > 0.001f || menuPeekSpring.target > 0.0f) && menuPeekSpring.Step(dt, 0.002f, 0.02f);
    bool mcMoving = false;
    if (closingToPeek) {
        float fadeSpeed = model.reduceMotion ? 16.0f : (model.openAnim == Model::kAnimZoom ? 8.5f : 11.0f);
        menuCloseAlpha = (std::max)(0.0f, menuCloseAlpha - dt * fadeSpeed);
        if (model.openAnim == Model::kAnimZoom) {
            float targetS = model.reduceMotion ? 0.96f : 0.86f;
            menuCloseScale = (std::max)(targetS, menuCloseScale - dt * 1.4f);
        }
        mcMoving = menuCloseAlpha > 0.004f;
        if (!mcMoving) {
            closingToPeek = false;
            menuCloseAlpha = 0.0f;
            menuCloseScale = 1.0f;
            ui.Forget();
        }
    }
    if (opened && !peeking && peekToastNow && menuPeekSpring.target <= 0.0f && !mpMoving) {
        peekToastNow = peekHot = peekCloseHot = peekExpanded = peekPrevHot = peekNextHot = peekClearHot = false;
        peekHoverRow = peekCloseRow = -1;
        peekShownRows = 0;
        peekStack.clear();
        peekIdx = 0;
        peekBmp.Reset();
    }
    if (zoomActive && !zMoving && !dragging) {
        if (!opened && zoomSpring.target <= 0.0f) {
            zoomActive = false;
            zoomBodyXform = D2D1::Matrix3x2F::Identity();
            zoomInvScale = 1.0f;
            zoomAlpha = 1.0f;
            if (zoomToPill && !followTo && !stage && !hidden && Stays() && !WantLongPill() && !(desk && desk->Up())) {
                zoomToPill = zoomFromPill = zoomDrawPill = false;
                island = islandLook = true;
                peekLook = peekToastNow = false;
                spring.value = spring.target = kIslandH;
                spring.vel = 0;
                widthSpring.value = widthSpring.target = IslandSpan();
                widthSpring.vel = 0;
                Resize(kIslandH, false);
                SyncDesk();
            } else {
                zoomToPill = zoomFromPill = zoomDrawPill = false;
                island = islandLook = false;
                spring.value = spring.target = 0;
                spring.vel = 0;
                widthSpring.value = widthSpring.target = IslandSpan();
                widthSpring.vel = 0;
            }
        } else if (opened && zoomSpring.target >= 1.0f) {
            zoomActive = zoomFromPill = zoomToPill = zoomDrawPill = false;
            zoomSpring.value = 1.0f;
            zoomSpring.vel = 0;
            zoomBodyXform = D2D1::Matrix3x2F::Identity();
            zoomInvScale = 1.0f;
            zoomAlpha = 1.0f;
            spring.value = spring.target = Depth();
            spring.vel = 0;
        }
    }
    bool moving = hMoving || wMoving || zMoving || mpMoving || mcMoving;
    if (!moving && !dragging) { // spring settled
        grow = false; // expansion finished, or retracted
        if (spring.target <= 0 && winH > kStrip) {
            islandLook = peekLook = peekToastNow = peekFromPill = peekRetracting = peekHot = peekCloseHot = peekExpanded = peekPrevHot = peekNextHot = peekClearHot = hand = false; // fully retracted, back to the plain strip and arrow cursor
            peekRetractH = kNotifOneH;
            peekHoverRow = peekCloseRow = -1;
            peekShownRows = 0;
            peekStack.clear();
            peekIdx = 0;
            peekBmp.Reset();
            pageT = 1; // a page slide cut short by closing only advances while the body draws: left behind, it kept the closed strip at full frame rate
            Resize(kStrip, false); // shrink window so it can't block clicks
            ui.Forget();
            SyncDesk(); // the desktop again: the long pill comes back now, not on the next second
            if (model.pickPending) { // the color picker waited for the panel to be gone
                model.pickPending = false;
                model.StartPicker();
            }
            if (followTo) { // retracted on the old screen: move over and drop open on the new one
                mon = followTo;
                followTo = nullptr;
                FreeDev(); // other screen, maybe other dpi
                Place();
                BuildStrips();
                SyncHide();
                Log(L"followed the mouse");
                if (followCall) Answer(); // called from there: the pill or the panel, as a hover would bring
                else Open();
                followCall = false;
                return;
            }
            if (cfg.gpuIdleSec > 0) SetTimer(hwnd, kReleaseTimer, (UINT)cfg.gpuIdleSec * 1000, nullptr); // schedule gpu release
        } else if (spring.target > 0) {
            bool wasRetract = peekRetracting;
            if (peekRetracting) {
                peekRetracting = peekLook = peekToastNow = peekFromPill = false;
                peekRetractH = kNotifOneH;
                peekStack.clear();
                peekIdx = 0;
                peekBmp.Reset();
            }
            float room = spring.target + (hintFade > 0.01f ? kHintRoom : 0); // a tip outside the panel needs a little window
            if (std::fabs(wantH - room) > 1 || peekLook || wasRetract) Resize(room, false); // otherwise drop the overshoot margin once at rest
        }
    }
    if (PlaceStale() && opened) Close(); // re-dock once it has retracted
    if (model.ZoomPct() != zoomNow) { // a new size: every dip is bigger or smaller, the window and surfaces are rebuilt for it
        zoomNow = model.ZoomPct();
        FreeDev();
        Place();
        BuildStrips();
    }
    SyncHud();
    model.SyncThemeSet(); // the desktop's theme or the over-apps one; settings previews whichever it's editing
    SyncLook(); // before the device: switching to or from glass rebuilds it
    SyncAccent();
    SyncTyping();
    if (!EnsureDev()) {
        needsFrame = false; // no device, stop loop instead of spinning
        if (devTries++ < kDevTries) SetTimer(hwnd, kDevTimer, 100, nullptr); // often just the old glass target still being released: try again shortly
        return;
    }
    devTries = 0;
    bool themes = StepTheme(dt);
    themes = anim::Approach(glassShadeNow, glassShade, kShadeRate, dt, 0.004f) || themes; // the glass tint fading to what's behind it now
    themes = anim::Approach(clearInk, clearInkGoal, kShadeRate, dt, 0.004f) || themes; // clear's ink crosses over, it doesn't snap
    float ctlGoal = (islandLook && !opened && !peeking && IslandMusicActive() && islandVizHot) ? 1.0f : 0.0f;
    bool ctlMoving = anim::Approach(islandCtlT, ctlGoal, model.reduceMotion ? 40.0f : 14.0f, dt, 0.004f);
    if (themes && desk) desk->Refresh();
    SyncCover();
    if (peekToastNow && ctx) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        for (auto& it : peekStack) {
            if (!it.bmp && !it.toast.iconPx.empty() && it.toast.iconW > 0 && it.toast.iconH > 0)
                ctx->CreateBitmap({it.toast.iconW, it.toast.iconH}, it.toast.iconPx.data(), it.toast.iconW * 4, bp, &it.bmp);
        }
        if (!peekBmp && !peekStack.empty() && peekIdx >= 0 && peekIdx < (int)peekStack.size())
            peekBmp = peekStack[(size_t)peekIdx].bmp;
    }

    float h = (std::max)(0.0f, (std::min)(spring.value, Depth() + overshoot)); // clamp to window room
    double drawT0 = Seconds();
    FitSurfaces(); // before drawing: a pill alone keeps a small strip, the panel gets its full size back
    if (!ctx || !canvas) return; // resizing failed and dropped the device: the next frame rebuilds it
    canvas->NewFrame();
    bool hear = access.Listening(); // a screen reader is here: this frame says what it draws
    canvas->Listen(hear);
    ui.Listen(hear);
    ui.Keyboard(keys && model.a11yKeys);
    bool pillCached = false, skyCached = false;
    if (islandLook && !opened && !peeking && !peekLook && !peekRetracting && !Side() && !moving && !dragging && !themes && h > 0.5f && std::fabs(h - spring.target) < 0.01f &&
        !islandVizHot && islandCtlT <= 0.004f && !(model.vizWave && model.vizOn)) { // resting pill; the waveform runs behind its still parts, so it's drawn whole
        shapeXform = EdgeXform();
        pillCached = PillLayer(h, IslandSpan());
    }
    if (shellNow.stars && opened && !islandLook && !grow && !peeking && !moving && !dragging && !themes && h > 24 && std::fabs(h - spring.target) < 0.01f) { // starfall, the panel at rest
        shapeXform = EdgeXform();
        skyCached = SkyLayer(h, Span());
    } else if (skyLayer && !opened) {
        skyLayer.Reset(); // the panel went: its gpu memory goes too
        skyKey = 0;
    }
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_RECT_F vis = Visible();
    ctx->PushAxisAlignedClip(vis, D2D1_ANTIALIAS_MODE_ALIASED); // the surface is panel-sized, the window shows only this corner: work only there
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent, desktop shows through via dcomp
    bool uiBusy = false, ambient = false;
    railHot = false; // the body sets it again while the mouse stays on the rail
    ComPtr<ID2D1Geometry> outline; // this frame's shape, the glass blur is cut to it
    float glassFade = 1.0f;
    if (h > 0.5f || zoomActive) {
        float shapeWide = Span();
        float curPeekW = ActivePeekSpan();
        bool morphPill = zoomActive && (zoomFromPill || zoomToPill);
        bool pushedOuterFade = false;
        bool pushedMorphClip = false;
        float pillFadeA = 0.0f;
        float shapeTopGap = -1.0f;
        zoomBodyXform = D2D1::Matrix3x2F::Identity();
        zoomInvScale = 1.0f;
        zoomAlpha = 1.0f;

        if (zoomActive) {
            float p = zoomSpring.value;
            float e = anim::Clamp01(p);
            float over = p - e; // > 0 during spring overshoot on open
            float under = (std::min)(0.0f, p); // < 0 during spring settle bounce on close into pill
            float minZ = model.reduceMotion ? 0.96f : 0.82f;
            float zScale = (std::max)(0.5f, minZ + (1.0f - minZ) * e + (over > 0.0f ? over * 0.08f : under * 0.20f));
            D2D1_POINT_2F anc = EdgeAnchor();
            if (morphPill) {
                float hGrow = p >= 0.0f
                    ? zoomPillH + (Depth() - zoomPillH) * p
                    : (std::max)(zoomPillH * 0.82f, zoomPillH + under * 88.0f);
                h = (std::clamp)(hGrow, 8.0f, Depth() + overshoot - 4.0f);
                shapeWide = p >= 1.0f
                    ? Span()
                    : p >= 0.0f
                        ? zoomPillW + (Span() - zoomPillW) * p
                        : (std::max)(zoomPillW * 0.88f, zoomPillW + under * 240.0f);
                if (Floating()) shapeTopGap = (std::min)(kFloatGap + 4.0f, (std::max)(0.0f, p) * kFloatGap);
                zoomAlpha = anim::Clamp01((p - 0.10f) / 0.72f);
                if (zoomDrawPill) pillFadeA = anim::Clamp01(1.0f - e / 0.36f);
                zoomBodyXform = D2D1::Matrix3x2F::Scale(zScale, zScale, anc);
                zoomInvScale = 1.0f / zScale;
                zoomAnchor = anc;
                shapeXform = EdgeXform();
                glassFade = 1.0f;
            } else {
                float zx = (std::min)(1.0f, zScale);
                h = (std::clamp)(Depth() * (over > 0.0f ? 1.0f + over * 0.18f : 1.0f), 8.0f, Depth() + overshoot - 4.0f);
                shapeWide = Span();
                glassFade = e;
                zoomAlpha = 1.0f;
                zoomBodyXform = D2D1::Matrix3x2F::Scale(zx, zx, anc);
                zoomInvScale = 1.0f / zx;
                zoomAnchor = anc;
                shapeXform = D2D1::Matrix3x2F::Scale(zx, zx, {Span() / 2, 0}) * EdgeXform();
                if (e < 0.999f) {
                    pushedOuterFade = true;
                    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
                    ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), e), nullptr);
                }
            }
        } else {
            if (peekRetracting || peekLook) {
                if (peekFromPill || peekRetracting) {
                    shapeWide = curPeekW;
                } else {
                    float tH = anim::Clamp01(h / kIslandH);
                    shapeWide = IslandSpan() + (curPeekW - IslandSpan()) * tH;
                }
                if (Floating() && closingToPeek) shapeTopGap = kFloatGap * menuCloseAlpha;
            } else if (islandLook) {
                shapeWide = IslandSpan();
            }
            auto wideAt = [&](float at) {
                float from = peekToastNow ? curPeekW : IslandSpan();
                float h0 = peekToastNow ? peekReach : kIslandH;
                return from + (Span() - from) * anim::Clamp01((at - h0) / ((std::max)(h0 + 20.0f, Depth() * 0.3f) - h0));
            };
            if (!islandLook && !peekLook && grow) shapeWide = wideAt(h); // pill widens into the panel
            shapeXform = EdgeXform();
        }
        ctx->SetTransform(shapeXform); // turned to the docked edge, centered in the window
        ComPtr<ID2D1Geometry> shape = shapeTopGap >= 0.0f ? BuildShape(h, shapeWide, shapeTopGap) : Shape(h, shapeWide);
        if (pillCached) { // a resting pill: its still parts come from the layer, only the bars are drawn fresh
            ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            ctx->DrawImage(pillLayer.Get());
            ctx->SetTransform(shapeXform);
        } else {
            skyPass = skyCached ? 2 : 0;
            PaintShape(shape.Get(), h);
            skyPass = 0;
        }
        outline = shape;
        if (glass && !zoomActive && (std::fabs(spring.vel) > 1 || std::fabs(widthSpring.vel) > 1) && !dragging) { // composition applies the blur's outline a few frames late: cut it where the glass will be, both ways
            bool closing = spring.target < h - overshoot; // really going away, not the bounce settling back after a drop
            float frames = closing ? 3.0f : 1.5f; // dropping and its bounce: a shorter lead, any more and the blur runs ahead of the glass
            anim::Spring ahead = spring; // where the spring will really be, not a straight line: that overshot the bounce's peak
            ahead.Step(dt * frames);
            anim::Spring wAhead = widthSpring;
            wAhead.Step(dt * frames);
            float lead = (std::clamp)(ahead.value, 0.0f, Depth() + overshoot);
            float leadW = wAhead.value > 1.0f ? wAhead.value : PeekSpan();
            float leadWide = shapeWide;
            if (peekRetracting || peekLook) {
                if (peekFromPill || peekRetracting) {
                    leadWide = leadW;
                } else {
                    float tH = anim::Clamp01(lead / kIslandH);
                    leadWide = IslandSpan() + (leadW - IslandSpan()) * tH;
                }
            } else if (!islandLook && !peekLook && grow) {
                float from = peekToastNow ? leadW : IslandSpan();
                float h0 = peekToastNow ? peekReach : kIslandH;
                leadWide = from + (Span() - from) * anim::Clamp01((lead - h0) / ((std::max)(h0 + 20.0f, Depth() * 0.3f) - h0));
            }
            outline = lead > 0.5f ? BuildShape(lead, leadWide, shapeTopGap) : nullptr;
        }
        if (ClearPill() || (zoomActive && glassFade < 0.02f)) outline = nullptr; // nothing to frost
        ctx->SetTransform(Side() ? D2D1::Matrix3x2F::Identity() : shapeXform);
        if (!opened && (peeking || peekLook || peekRetracting)) {
            if (closingToPeek && menuCloseAlpha > 0.004f) {
                Input savedIn = input;
                input = Input{};
                if (model.openAnim == Model::kAnimZoom && menuCloseScale < 0.999f) {
                    zoomAnchor = EdgeAnchor();
                    zoomBodyXform = D2D1::Matrix3x2F::Scale(menuCloseScale, menuCloseScale, zoomAnchor);
                    zoomInvScale = 1.0f / menuCloseScale;
                }
                if (shape) {
                    ctx->SetTransform(shapeXform);
                    ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.Get(),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), menuCloseAlpha), nullptr);
                    DrawBody(Depth(), dt, now);
                    ctx->PopLayer();
                } else {
                    DrawBody(Depth(), dt, now);
                }
                zoomBodyXform = D2D1::Matrix3x2F::Identity();
                zoomInvScale = 1.0f;
                input = savedIn;
            }
            float baseH = (std::max)(peekRetractH, peekReach);
            float tMorph = (peekFromPill || peekRetracting) && baseH > kIslandH + 1.0f
                ? anim::Clamp01((h - kIslandH) / (baseH - kIslandH))
                : 1.0f;
            bool morphingWithPill = (peekFromPill || peekRetracting) && !closingToPeek && peekToastNow;
            float pillA = peekRetracting ? anim::EaseOut(anim::Clamp01((0.56f - tMorph) / 0.56f))
                        : (morphingWithPill ? anim::Clamp01(1.0f - tMorph / 0.40f) : 0.0f);
            float notifA = peekRetracting ? anim::Clamp01((tMorph - 0.24f) / 0.68f)
                         : (morphingWithPill ? anim::EaseOut(anim::Clamp01((tMorph - 0.14f) / 0.68f)) : 1.0f);

            bool clipped = morphingWithPill && shape;
            if (clipped) {
                ctx->SetTransform(shapeXform);
                ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.Get()), nullptr);
            }
            if (morphingWithPill && pillA > 0.01f) {
                float ps = (model.reduceMotion ? 0.96f : 0.88f) + (model.reduceMotion ? 0.04f : 0.12f) * pillA;
                D2D1_POINT_2F pAnc = Side() ? EdgeAnchor() : D2D1_POINT_2F{W / 2, kIslandH / 2};
                D2D1_MATRIX_3X2_F baseX = Side() ? D2D1::Matrix3x2F::Identity() : shapeXform;
                ctx->SetTransform(D2D1::Matrix3x2F::Scale(ps, ps, pAnc) * baseX);
                ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), pillA), nullptr);
                float pillDrawH = kIslandH + (h - kIslandH) * 0.25f;
                ambient = DrawPillNow(pillDrawH, dt, now, false) || ambient;
                ctx->PopLayer();
            }
            if (notifA > 0.01f || !morphingWithPill) {
                ctx->SetTransform(Side() ? D2D1::Matrix3x2F::Identity() : shapeXform);
                DrawPeek(h, notifA, morphingWithPill ? tMorph : 1.0f, !morphingWithPill);
                if (peeking && peekToastNow && peekHot && !peekExpanded && (IsPeekTruncated() || peekStack.size() > 1))
                    ambient = true; // animate the subtle hover-dwell progress bar
            }
            if (morphingWithPill && !Side() && h > 14.0f) {
                ctx->SetTransform(shapeXform);
                float hA = anim::Clamp01((h - 14.0f) / 18.0f);
                bool canExp = peeking && !peekExpanded && (IsPeekTruncated() || peekStack.size() > 1);
                float hw = 16.0f + (peekHot ? (canExp ? 26.0f : 16.0f) : 4.0f) * tMorph;
                canvas->Line({W / 2 - hw / 2, h - 3.5f}, {W / 2 + hw / 2, h - 3.5f}, shellNow.text.Alpha(0.28f * hA), 2.5f);
                if (peekHot && canExp && peekDwellAt > 0) {
                    float dwell = anim::Clamp01((float)((lastFrame - peekDwellAt) / (kPeekDwellMs / 1000.0)));
                    float pw = hw * dwell;
                    if (pw > 1.0f)
                        canvas->Line({W / 2 - pw / 2, h - 3.5f}, {W / 2 + pw / 2, h - 3.5f}, shellNow.accentHi.Alpha(0.95f * hA), 2.5f);
                }
            }
            if (clipped) ctx->PopLayer();
        }
        else if (islandLook) { // bars bounce only while it shows and music plays
            pillPass = pillCached ? 2 : 0;
            ambient = DrawPillNow(h, dt, now);
            pillPass = 0;
        }
        else if (zoomActive) {
            if (morphPill && pillFadeA > 0.01f && !Side()) {
                float ps = 1.0f + (model.reduceMotion ? 0.04f : 0.14f) * (1.0f - pillFadeA);
                ctx->SetTransform(D2D1::Matrix3x2F::Scale(ps, ps, {W / 2, kIslandH / 2}) * shapeXform);
                ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), pillFadeA), nullptr);
                ambient = DrawPillNow(kIslandH, dt, now) || ambient;
                ctx->PopLayer();
            } else if (morphPill && pillFadeA > 0.01f && Side()) {
                float ps = 1.0f + (model.reduceMotion ? 0.04f : 0.14f) * (1.0f - pillFadeA);
                ctx->SetTransform(D2D1::Matrix3x2F::Scale(ps, ps, EdgeAnchor()));
                ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), pillFadeA), nullptr);
                ambient = DrawPillNow(kIslandH, dt, now) || ambient;
                ctx->PopLayer();
            }
            if (zoomAlpha > 0.01f) {
                if (morphPill && shape) {
                    ctx->SetTransform(shapeXform);
                    ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.Get()), nullptr);
                    pushedMorphClip = true;
                }
                DrawBody(Depth(), dt, now);
                uiBusy = ui.Busy();
                ambient = ui.Ambient() || ambient;
                if (pushedMorphClip) ctx->PopLayer();
            }
        }
        else if (h > 24) { // too short for content, just shape
            DrawBody(h, dt, now);
            uiBusy = ui.Busy();
            ambient = ui.Ambient();
        }
        if (MenuPeekShown() && (!zoomActive || zoomAlpha > 0.05f) && h > 24) {
            DrawMenuPeek(h);
            if (peeking && peekToastNow && peekHot && !peekExpanded && (IsPeekTruncated() || peekStack.size() > 1))
                ambient = true;
        }
        if (pushedOuterFade) ctx->PopLayer();
    }
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->PopAxisAlignedClip();
    HRESULT hr = ctx->EndDraw();
    if (hear) access.Publish(ui, *canvas, scale, {0, 0}, opened);
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    SyncRailKeys(railHot);
    if (hr == D2DERR_RECREATE_TARGET) { // device lost, rebuild next frame
        FreeDev();
        needsFrame = true;
        return;
    }
    if (glass) {
        GlassBlur((model.GlassBlur() / 100.0f) * glassFade);
        GlassMask(outline.Get());
    }
    bool fast = moving || dragging || uiBusy || themes || pageT < 1 || ctlMoving; // anything actively animating
    if (cfg.perfLog) {
        int why = moving ? 0 : dragging ? 1 : uiBusy ? 2 : (themes || pageT < 1) ? 3 : ambient ? 4 : 5;
        ++perfWhy[why];
        Perf(Seconds() - drawT0);
    }
    Govern(fast, gap);
    bool orb = shellNow.stars && opened && !islandLook && !model.reduceMotion && h > 24; // starfall's orb drifts only while the open panel shows
    UINT orbSync = (UINT)(std::clamp)((int)std::lround(hz / kOrbFps), 1, 4); // ~20 fps; dxgi caps the interval at 4
    UINT calm = islandLook && !opened ? pillSync : orb && !ambient ? (std::max)(ambientSync, orbSync) : ambientSync; // ambient motion at ~30 fps, the orb alone ~20, the resting pill's little bars ~15
    if (glass) { // no swapchain: the frame lands with the blur outline, a timer paces the next one
        GlassPresent();
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((fast ? fastSync : calm) * 1e7 / hz); // relative, 100 ns units
        SetWaitableTimer(frameWait, &due, 0, nullptr, nullptr, FALSE);
        hr = S_OK;
    } else {
        hr = swap->Present(fast ? fastSync : (std::min)(calm, 4u), 0); // ambient-only motion runs at ~30 fps, the pill at ~15; a whole present: with dirty rects, a frame shown while the window is a sliver is never retired and the loop stalls
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { // driver reset / gpu switch
        FreeDev();
        needsFrame = true;
        return;
    }
    needsFrame = fast || ((ambient || orb) && h > 0.5f && !Unseen()); // otherwise go idle until next input; locked or dark: ambient motion waits too
    if (pillCached && islandLook && pillKey != trimKey) { // a new resting pill: tidy up once it has settled
        trimKey = pillKey;
        SetTimer(hwnd, kTrimTimer, kTrimMs, nullptr);
    }
    if (PlaceStale() && !opened && !dragging && !peeking && !zoomActive && (spring.value <= 0.5f || island)) ApplyPlace(); // a pill that always stays would otherwise keep the old edge forever
    if (model.deskEditAsk) model.deskEditAsk = false, PostMessageW(hwnd, kDeskEditMsg, 0, 0); // never inside a frame: it takes the panel away
    if (model.tourAsk) model.tourAsk = false, TourStart(); // asked for in settings: they close for it
    if (model.studioAsk) PostMessageW(hwnd, kDeskEditMsg, model.studioAsk, 0), model.studioAsk = Model::kStudioNone;
    if (model.shelfDrag) PostMessageW(hwnd, kShelfMsg, 0, 0); // the drag runs its own loop: never inside a frame
    if (model.shelfPaste) PostMessageW(hwnd, kShelfMsg, 1, 0); // nor a paste: it hands the keyboard around
}

