#include "overlay.h"

#include <Xinput.h>
#include <wincodec.h>   // menu art: PNG decode (WIC), on its own worker thread - see MenuArtRefresh
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/log.h"
#include "aimhook.h"
#include "armfit.h"       // ArmFitStart / ArmFitStatus - the VR Comfort tab's "Arm calibration" node
#include "calibscreen.h"  // CalibScreenOpen / CalibScreenActive - the calibration screen (build A)
#include "boneblend.h"    // SnapTurnStatus / ComfortTurnMode - the VR Comfort tab's status line
#include "controllers.h"
#include "grappleaim.h"   // FireReticle3d / GetGunDot - the gun reticle's point
#include "headpose.h"     // GetHeadPose - the fire_reticle_3d probe's cross-check; the holster sphere's facing
#include "holster.h"      // HolsterZoneWorld / HolsterZoneParams / HolsterPlacing - the holster sphere (place-by-hand Task 4)
#include "ring_logic.h"   // the holster rings' look (plan 2026-11-20-holster-pivot-posture-rings Task 5)
#include "rtticam.h"
#include "camhook.h"
#include "watchwrite.h"
#include "settings.h"
#include "uipanel.h"      // UiHudPanel / uipanel::HudRecentre - the "Re-centre the HUD panel" button
#include "wristquad.h"    // UiWrist* + ApplyPreset / MatchesPreset / OnThisLaunch - the HUD tab's Wrist HUD section (stage 3a)
#include "bindcapture_logic.h"   // the Controls tab: controller press -> binds::Slot, plain input names
// The bootstrap's own aa_method.txt parser, header-only - so the dropdown reads
// the file exactly the way PAKGEN and dlssflat do and can never disagree with them.
#include "../bootstrap/aamethod.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::MarkItemEdited - a slider moved by the right stick while active saves on release
#include "backends/imgui_impl_dx11.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - diag_periodic_logs

namespace dlvr {

// OUTSIDE the anonymous namespace ON PURPOSE. 2026-09-14: this became a
// settings-file key (`show_reticle`), and the registry in ovr2xr.cpp takes the
// address of the variable itself - so it needs external linkage. Everything
// inside the anonymous namespace below still sees it by ordinary lookup.
bool g_showReticle = false;   // DEFAULT OFF 2026-10-04 = Jay's disk (a debug aim marker; the 3D reticle does not need it)

// Holster place-by-hand (plan 2026-10-30, Task 4; ledger ruling R1): the zone whose sphere is drawn (0 shoulder, 1 right hip,
// 2 left hip, 3 head), -1 = none. DEFINED here by Task 4, SET by the Holsters page (Task 5). Render thread only.
// OverlayRenderHolsterSphere reads it first: -1 costs one int test per submit.
int g_holsterShowZone = -1;

namespace {

// Panel resolution. Fixed rather than derived from the headset: this is a text
// panel a metre and a half away, and its legibility depends on the angle it
// subtends, not on the display's pixel count.
constexpr int kPanelW = 1300;
constexpr int kPanelH = 800;
// The MENU's height (Jay 2026-11-12, the BioShock-style layout: "1000 is fine"): button row, tabs, list, description box,
// divider, Text size, close-hold strip. kPanelH stays 800 for everything else that uses it (the reticle canvas, the holster
// sphere, the calibration quads, the title quad's canvas). The panel swapchain is created kPanelW x kMenuH; the reticle and the
// open-hold bar keep drawing into its top rows through their own sub-rects, and io.DisplaySize goes back to kPanelW x kPanelH
// after each menu frame (OverlayRender), so their frames are what they were.
constexpr int kMenuH = 1000;
void DrawDlssTab();
void DrawComfortTab();
void DrawHudTab();        // BETA: every ui_ setting + the Wrist HUD (Jay 2026-11-13: its own tab, after VR Comfort)
void DrawParkourTab();    // hand climbing's settings (the "Physical Parkour" group), in sections (tester 2026-11-27)
void DrawControlsTab();
bool ControlsHoldPad(bool padIdle);
bool ControlsBusy();      // a Controls-tab capture (or its release drain) owns the controller - B / L3+R3 must not act
bool ControlsBackOut();   // B on the Holsters page goes back to the bindings; false = nothing to back out of
void HolsterPageAfterTabs(double uiStart);
void DrawButtonRow();     // the menu's top row: Re-centre HUD / Calibrate my arms / Basic-Advanced (after DrawComfortTab)

// THE TAB BAR, IN ONE PLACE. Order is the on-screen order; DLSS sits next to
// Camera by request. kTabCount is DERIVED from this table, so adding a tab is
// this one edit - the shoulder-button paging below picks it up automatically.
//
// The previous form hardcoded `kTabCount = 4` here and a separate sel0..sel3
// ladder at the tab bar: two copies of "how many tabs there are", to be edited
// in step. This file's own history is full of counters that covered one of two
// call sites, and a paging wrap that disagrees with the bar would strand a tab
// the gamepad could never reach.
struct TabDef { const char* name; void (*draw)(); };
// BETA TREE (2026-10-18, Jay: "The following tabs themselves, need to be hidden - SETTINGS, CAMERA, RIGGING, DIAGNOSTIC, AIM
// DEBUGGING, CONTROLLER BINDINGS"). Only the tabs are hidden: every setting on them keeps its file value and still applies.
// Their draw functions were removed from this tree 2026-11-14 (removal batch 2; DEV's table, src/, still has all eight).
const TabDef kTabs[] = {
    { "VR Comfort",          DrawComfortTab },   // first: the one a player opens (SPEC-VR-COMFORT-2026-09-30 §1)
    // Jay 2026-11-13: "All the HUD items, including everything that sits in 'UI Master Control' in VR Comfort, will be moved to
    // it's own new HUD/UI Tab" + "After VR Comfort". Name art: dlvr\menu\tab_name_hud.png (Jay's, 228 x 40).
    { "HUD",                 DrawHudTab },
    // Tester 2026-11-27: "add a new section to the L3 + R3 in-game mod menu, called Physical Parkour" - the hand-climbing settings
    // (docs/design_climbing_overhaul.md). No name art yet: the tab draws its text (dlvr\menu\tab_name_physical_parkour.png if added).
    { "Physical Parkour",    DrawParkourTab },
    { "Anti-Aliasing",       DrawDlssTab },   // renamed from "DLSS", Jay 2026-09-26 (PROPOSAL-FSR §8)
    { "Controls",            DrawControlsTab },   // user rebinding, plan 2026-10-28-controls-rebind Task 6
};
constexpr int kTabCount = static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0]));

struct Overlay {
    bool initialised = false;
    bool failed = false;
    bool visible = false;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    XrSession session = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
    // grapple_reticle_3d: its own quad (it can be up at the same time as the gun reticle, at another depth)
    XrSwapchain swapchain2 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images2;
    std::vector<ID3D11RenderTargetView*> rtvs2;
    // The calibration screen's quad (build A, OverlayRenderCalibQuad) - created on first use.
    XrSwapchain swapchain3 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images3;
    std::vector<ID3D11RenderTargetView*> rtvs3;
    bool calibTried = false;
    // The calibration screen's FLOOR quad (build B, OverlayRenderCalibFloor) - created on first use.
    XrSwapchain swapchain4 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images4;
    std::vector<ID3D11RenderTargetView*> rtvs4;
    bool floorTried = false;
    // The floor's IMAGE path (Build D task 5, OverlayRenderCalibFloorImage): its own 2048 swapchain, filled by CopyResource.
    XrSwapchain swapchain5 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images5;
    std::vector<ID3D11RenderTargetView*> rtvs5;   // stays empty - CopyResource needs no RTV
    bool floorImgTried = false;
    // The void's walls (one swapchain, four quads) and ceiling (Jay 2026-10-16, OverlayRenderCalibVoidImage): the floor
    // image's pattern, a 2048 swapchain each, filled by CopyResource.
    XrSwapchain swapchain6 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images6;
    std::vector<ID3D11RenderTargetView*> rtvs6;   // stays empty
    bool wallImgTried = false;
    XrSwapchain swapchain7 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images7;
    std::vector<ID3D11RenderTargetView*> rtvs7;   // stays empty
    bool ceilingImgTried = false;
    // The holster sphere (place-by-hand Task 4, OverlayRenderHolsterSphere) - created on first use, kPanelW x kPanelH, the
    // centred square used. sphereList = its own ImDrawList (drawn WITHOUT an ImGui frame - see OverlayRenderHolsterSphere).
    XrSwapchain swapchain8 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images8;
    std::vector<ID3D11RenderTargetView*> rtvs8;
    bool sphereTried = false;
    ImDrawList* sphereList = nullptr;
    // The visible holster rings (plan 2026-11-20-holster-pivot-posture-rings Task 5, OverlayRenderHolsterRings): up to 3, each its own
    // swapchain (kPanelW x kPanelH, the centred square used, as the sphere's), created on first use; drawn through an ImGui frame
    // (never while the panel is open - the rings stand down then).
    XrSwapchain swapchainRing[3] = { XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::vector<XrSwapchainImageD3D11KHR> imagesRing[3];
    std::vector<ID3D11RenderTargetView*> rtvsRing[3];
    bool ringTried[3] = { false, false, false };
    // The menu TITLE's quad (OverlayRenderTitle, Jay 2026-11-11: above the panel, top left, outside it) - created on first use,
    // kPanelW x kPanelH like the sphere's, only the title's top-left rectangle shown. Own ImDrawList, no ImGui frame (as the sphere).
    XrSwapchain swapchain9 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images9;
    std::vector<ID3D11RenderTargetView*> rtvs9;
    bool titleTried = false;
    ImDrawList* titleList = nullptr;

    // Edge detection for the toggle, so holding the binding does not strobe.
    bool togglePrev = false;

    // Rebinding: when set, the next key or button pressed becomes the binding.
    int capturing = 0;   // 0 none, 1 keyboard, 2 gamepad

    char status[256] = {};
    double statusUntil = 0.0;

    double lastFrame = 0.0;

    bool padSeen = false;
    bool justOpened = false;

    int tab = 0;              // 0 settings, 1 camera
    bool tabRequest = false;  // a shoulder button asked for a change this frame
    bool shoulderPrev = false;
    bool passLook = false;    // left trigger held: the stick reaches the game

    // VR CONTROLLERS DRIVE THE MENU (Jay 2026-11-11: "L3+R3 HOLD FOR 2/3 SECONDS TO OPEN, THEN CAN CONTROL WITH JOYSTICKS &
    // BUTTONS"; then "2 seconds", and the layout "Sticks + A/B": left stick up/down = rows, right stick left/right = value
    // (hold = fast), A = select, B = back / close, grips = tabs, L3+R3 hold = close). Touch never appears as an XInput pad,
    // so before this the menu opened only from the keyboard (overlay_key) or an Xbox pad (overlay_pad).
    double comboStart = 0.0;      // when L3 and R3 were first both held; 0 = not both held
    float  holdProgress = 0.0f;   // 0..1 toward the toggle - drawn as the hold bar
    bool   comboLatched = false;  // fired: both clicks must be released before another hold counts
    bool   vrPadSeen = false;     // a Touch / Index stick reported this panel frame
    int    vrStep = 0;            // right stick: a step for the focused control THIS frame (+1 right, -1 left, 0 none)
    float  vrStepMul = 1.0f;      // 1, or kVrFastMul once the stick has been held kVrFastAfter
    bool   vrStepHeld = false;    // the right stick is pushed sideways (a save-on-release waits for this to clear)
    int    vrStepDir = 0;
    double vrStepStart = 0.0, vrStepNext = 0.0;
    bool   vrUpDown[2] = {};      // left stick Y as d-pad up / down, with hysteresis
    bool   vrLeftRight[2] = {};   // left stick X as d-pad left / right, with hysteresis (Jay 2026-11-12: the button row)
    bool   vrGripPrev[2] = {};    // grips page the tabs, edge-detected
    bool   vrGripOn[2] = {};
    bool   bPrev = false;         // B (Touch or pad), for its edge
    bool   bEdge = false;
    bool   busyPrev = false;      // LAST panel frame: a dropdown open / a slider active / a capture - B belonged to it
    bool   closeRequest = false;  // B with nothing to back out of: OverlayPoll closes the panel at its next call
    const char* helpText = nullptr;   // the focused row's help, shown in the strip under the list
    bool   helpRestart = false;
    // SLIDER ADJUST MODE (Jay, headset 2026-11-11, twice: "Should be an A press, then I'd be able to adjust them with the
    // joystick" -> "That hasn't worked either"). OURS, not ImGui's activation: imgui.cpp:13863 turns an A held >= 0.6 s
    // (NAV_ACTIVATE_INPUT_WITH_GAMEPAD_DELAY, :1338) on a slider into a TEXT-INPUT box, which this panel cannot type into and
    // which writes its own text back on exit - so a Touch A on a slider never reaches ImGui; FeedInput toggles this instead.
    ImGuiID sliderFocused = 0;    // the slider the nav cursor was on in the LAST panel frame (ArrowStepFocusedSlider), 0 = none
    bool    adjusting = false;    // A pressed on that slider: the right stick moves it until A or B
    ImGuiID adjustId = 0;
    bool    aPrev = false;
    bool    aSwallow = false;     // the A press that entered / left adjust mode is not sent to ImGui until A is released
    bool    bSwallow = false;     // likewise the B that left it (and it does not close the menu)
};

Overlay g;

constexpr double kMenuHoldSec = 2.0;    // Jay: "2 seconds"
constexpr double kVrRepeatDelay = 0.40; // right stick: first step at once, repeats after this...
constexpr double kVrRepeatEvery = 0.08; // ...every this...
constexpr double kVrFastAfter = 1.5;    // ...and after this long held, each step is kVrFastMul steps ("hold = fast")
constexpr float  kVrFastMul = 5.0f;

// Aim reticle: a small world-locked marker floating along the right
// controller's own aim ray (the same direction aimhook.cpp's ForwardFromPose
// computes), so the redirected shot direction is visible before pulling the
// trigger - not tied to a shot being fired, unlike the aim hook itself, which
// only runs per-shot. Only shown while the settings panel is closed (both
// reuse the one swapchain - see RenderReticle()). Controlled from the Aim
// debugging tab, not persisted to the settings file - same treatment as
// AimHookOverride right next to it.
// g_showReticle is defined at the top of namespace dlvr, outside the anonymous
// namespace, because it is now a settings-file key - see the comment there.
float g_reticleDistance = 3.0f;   // metres ahead, along the controller's forward
                                  // NOT persisted - still a session choice.

// fire_reticle_3d (grappleaim.cpp's DEP marker): this frame's declared eye poses, and the probe's window.
bool    g_declValid = false;
XrPosef g_declL{}, g_declR{};
struct GunDotWin {
    ULONGLONG start = 0;
    unsigned placed = 0, noDot = 0, noDecl = 0, noCross = 0, hits = 0;
    unsigned shaftOut = 0;   // bow_reticle_3d: shaft dot beyond the 30 deg canvas cap - not drawn
    float dMin = 1e9f, dMax = 0.0f;       // muzzle -> dot (m)
    float vMin = 1e9f, vMax = 0.0f;       // declared centre -> dot (m)
    double crossSum = 0.0;                // |angle| between our placement and the yaw-mapping one, seen from the centre
    float crossMax = 0.0f;
    unsigned crossN = 0;
    double headSum = 0.0;                 // |declared centre - GetHeadPose position| (m)
    float headMax = 0.0f;
    unsigned headN = 0;
    float eyeRotMax = 0.0f;               // angle between the two declared eye orientations (deg)
};
GunDotWin g_gw;
bool      g_gwFirstSaid = false;

// XInput, loaded from System32 by hand.
//
// LoadLibrary("xinput1_3.dll") would find OUR bootstrap sitting in the game
// folder, which is the DLL that proxies XInput for the engine. Going through it
// would be a loop through our own hook for no reason; the real one is the one
// we want.
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
PFN_XInputGetState g_xinput = nullptr;

void LoadXInput()
{
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;

    char sys[MAX_PATH];
    if (!GetSystemDirectoryA(sys, sizeof(sys))) {
        return;
    }
    const char* names[] = { "XInput1_4.dll", "XInput9_1_0.dll", "XInput1_3.dll" };
    for (const char* n : names) {
        char full[MAX_PATH * 2];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", sys, n);
        HMODULE m = LoadLibraryA(full);
        if (!m) {
            continue;
        }
        g_xinput = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(m, "XInputGetState"));
        if (g_xinput) {
            LOG("overlay: using %s for input", full);
            return;
        }
    }
    LOG("overlay: no XInput available; keyboard binding only");
}

bool ReadPad(XINPUT_STATE& st)
{
    LoadXInput();
    if (!g_xinput) {
        return false;
    }
    for (DWORD i = 0; i < 4; ++i) {
        ZeroMemory(&st, sizeof(st));
        if (g_xinput(i, &st) == ERROR_SUCCESS) {
            return true;
        }
    }
    return false;
}

// The Touch / Index controllers as the menu reads them (controllers.cpp accessors - cached values from the last xrSyncActions,
// no OpenXR call here). ok = either stick's action is live. lg / rg = the grips, with hysteresis (on 0.70, off 0.30).
// idle = nothing pressed or pushed - the Controls tab's capture drain waits for it.
struct VrMenuPad {
    bool ok = false;
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f, lt = 0.0f, rt = 0.0f;
    bool a = false, b = false, x = false, y = false, l3 = false, r3 = false, lg = false, rg = false, idle = true;
};

void ReadVrMenuPad(VrMenuPad& p)
{
    const bool okL = GetControllerStick(kLeftHand, p.lx, p.ly);
    const bool okR = GetControllerStick(kRightHand, p.rx, p.ry);
    p.ok = okL || okR;
    if (!p.ok) { return; }
    float sq[2] = {};
    GetControllerSqueeze(kLeftHand, sq[0]);
    GetControllerSqueeze(kRightHand, sq[1]);
    for (int h = 0; h < 2; ++h) {
        if (!g.vrGripOn[h] && sq[h] > 0.70f) { g.vrGripOn[h] = true; } else if (g.vrGripOn[h] && sq[h] < 0.30f) { g.vrGripOn[h] = false; }
    }
    p.lg = g.vrGripOn[0];
    p.rg = g.vrGripOn[1];
    GetControllerTrigger(kLeftHand, p.lt);
    GetControllerTrigger(kRightHand, p.rt);
    GetControllerStickClick(kLeftHand, p.l3);
    GetControllerStickClick(kRightHand, p.r3);
    GetControllerButton(kButtonA, p.a);
    GetControllerButton(kButtonB, p.b);
    GetControllerButton(kButtonX, p.x);
    GetControllerButton(kButtonY, p.y);
    const float m = (std::max)((std::max)(fabsf(p.lx), fabsf(p.ly)), (std::max)(fabsf(p.rx), fabsf(p.ry)));
    p.idle = !p.a && !p.b && !p.x && !p.y && !p.l3 && !p.r3 && m < 0.30f && sq[0] < 0.30f && sq[1] < 0.30f && p.lt < 0.15f &&
             p.rt < 0.15f;
}

double Now()
{
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

void SetStatus(const char* text)
{
    strncpy_s(g.status, sizeof(g.status), text, _TRUNCATE);
    g.statusUntil = Now() + 4.0;
}

// --------------------------------------------------------------- menu art ---
// Jay 2026-11-11: "Custom assets, I'll need rough sizings so I can make them myself", then "I'll be making the names of each
// menu their own image aswell". Every piece is a PNG in dlvr\menu\ (kMenuArt; the sizes Jay was given). A missing piece keeps
// the code-drawn look, so the art can arrive one file at a time. Checked each time the menu OPENS: a file is decoded again only
// when its modified time changed, so an edited PNG shows on the next open without a restart.
// Decoded by WIC on a SHORT-LIVED WORKER THREAD with its own COM apartment: CoInitializeEx on the game's render thread could
// fix that thread's apartment before the game picks one. Textures are created here, on the render thread (g.device).
// COST: nothing per frame. Per open: one GetFileAttributesEx per file (~26); a new or changed file is decoded and uploaded once
// (a 1300x800 PNG is a few ms - a one-frame hitch on that open only).
enum MenuArtId {
    kArtPanelBg, kArtTitle, kArtTab, kArtTabSel, kArtRowHi, kArtSectionOpen, kArtSectionClosed,
    kArtSliderTrack, kArtSliderFill, kArtSliderKnob, kArtCheckOff, kArtCheckOn, kArtDropdownArrow,
    kArtHoldBg, kArtHoldFill,
    // 2026-11-12, the BioShock-style layout (Jay: "We'll be doing all of them. Along with some of the things I deferred last
    // night"): buttons, the section banner and the dropdown box come back as art, plus the open list, scrollbar and divider.
    kArtButton, kArtButtonSel, kArtSectionBar, kArtDropdown, kArtDropdownList, kArtScrollTrack, kArtScrollThumb, kArtDivider,
    kArtFixedCount   // the tab NAME images follow, one per kTabs entry: tab_name_<slug>.png (TabSlug)
};
const char* const kMenuArtFile[] = {
    "panel_bg.png",          // 1300 x 1000 (kMenuH), the whole panel
    "title.png",             // Jay's 320 x 120 - its OWN quad above the panel's top-left corner (OverlayRenderTitle), any size
    "tab.png",               // 260 x 56, 9-slice border 16
    "tab_selected.png",      // 260 x 56, 9-slice border 16
    "row_highlight.png",     // 1240 x 56, 9-slice border 16 - the row the controller is on
    "section_open.png",      // 32 x 32
    "section_closed.png",    // 32 x 32
    "slider_track.png",      // 660 x 24, 9-slice border 12
    "slider_fill.png",       // 660 x 24, 9-slice border 12 - cut at the value
    "slider_knob.png",       // 40 x 48
    "checkbox_off.png",      // 48 x 48
    "checkbox_on.png",       // 48 x 48
    "dropdown_arrow.png",    // 32 x 32
    // Jay's names and size (2026-11-11): hold_bar.png, not hold_bar_bg.png; 440 x 40 - drawn at the size it is made.
    "hold_bar.png",          // 440 x 40
    "hold_bar_fill.png",     // 440 x 40 - cut left to right by the hold
    // The control-hint icons (btn_a / btn_b / grips / sticks) are GONE with the hint strip (Jay: "I don't think we need to show
    // the controls either").
    "button.png",            // 400 x 64, 9-slice border 16 - every menu button (top row, Controls cells, Place / Default ...)
    "button_selected.png",   // 400 x 64, 9-slice border 16 - the button the controller is on
    "section_bar.png",       // 1240 x 48, 9-slice border 48 - behind every section / heading
    "dropdown.png",          // 720 x 48, 9-slice border 16 - behind a closed dropdown
    "dropdown_list.png",     // any size, 9-slice border 16 - behind an open dropdown's list
    "scrollbar_track.png",   // 16 x 400, 9-slice border 8
    "scrollbar_thumb.png",   // 16 x 120, 9-slice border 8
    "divider.png",           // 1240 x 24 - drawn at the size it is made, centred
};
static_assert(sizeof(kMenuArtFile) / sizeof(kMenuArtFile[0]) == kArtFixedCount, "one file name per MenuArtId");
constexpr int kArtMax = kArtFixedCount + 16;
struct MenuArt { char file[64] = {}; int w = 0, h = 0; ID3D11ShaderResourceView* srv = nullptr; FILETIME mt{}; };
MenuArt g_menuArt[kArtMax];
int  g_menuArtN = 0;
bool g_menuArtSaid = false;
// The menu's font (Jay 2026-11-12: "You supply a TTF"): dlvr\menu\font.ttf, loaded once at OverlayInit - a changed or newly added
// font file needs a game restart (unlike the PNGs). nullptr = ImGui's built-in font. kMenuFontBase = the built-in font's 13 px,
// the base the menu was laid out at (x style.FontScaleMain 1.6 = 20.8 px on the panel).
ImFont* g_menuFont = nullptr;
// The DESCRIPTION box's font (Jay 2026-11-12: the descriptions are "hard to read in the dying light font" -> "Windows' Segoe UI"):
// %WINDIR%\Fonts\segoeui.ttf, loaded once at OverlayInit, normal case (no remap). nullptr = the box keeps the menu font.
ImFont* g_descFont = nullptr;
constexpr float kMenuFontBase = 13.0f;

// "Anti-Aliasing" -> "anti_aliasing", "VR Comfort" -> "vr_comfort": lower-case letters and digits, any run of others = one '_'.
void TabSlug(const char* name, char* out, size_t cch)
{
    size_t n = 0;
    bool sep = false;
    for (const char* s = name; *s && n + 1 < cch; ++s) {
        const char c = *s;
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum) { sep = n > 0; continue; }
        if (sep && n + 2 < cch) { out[n++] = '_'; }
        sep = false;
        out[n++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    out[n] = '\0';
}

struct ArtDecodeJob { int index = 0; wchar_t path[MAX_PATH * 2] = {}; std::vector<unsigned char> px; UINT w = 0, h = 0; HRESULT hr = E_FAIL; };

DWORD WINAPI ArtDecodeThread(LPVOID param)
{
    auto* jobs = static_cast<std::vector<ArtDecodeJob>*>(param);
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    const HRESULT fhr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    for (ArtDecodeJob& j : *jobs) {
        if (FAILED(fhr) || !fac) { j.hr = FAILED(fhr) ? fhr : E_FAIL; continue; }
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        HRESULT r = fac->CreateDecoderFromFilename(j.path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
        if (SUCCEEDED(r)) { r = dec->GetFrame(0, &frame); }
        if (SUCCEEDED(r)) { r = fac->CreateFormatConverter(&conv); }
        // Straight (not premultiplied) RGBA: the ImGui DX11 backend blends SrcAlpha / InvSrcAlpha.
        if (SUCCEEDED(r)) {
            r = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(r)) { r = conv->GetSize(&j.w, &j.h); }
        if (SUCCEEDED(r) && (j.w == 0 || j.h == 0 || j.w > 4096 || j.h > 4096)) { r = E_INVALIDARG; }
        if (SUCCEEDED(r)) {
            j.px.resize(static_cast<size_t>(j.w) * j.h * 4);
            r = conv->CopyPixels(nullptr, j.w * 4, static_cast<UINT>(j.px.size()), j.px.data());
        }
        j.hr = r;
        if (conv) { conv->Release(); }
        if (frame) { frame->Release(); }
        if (dec) { dec->Release(); }
    }
    if (fac) { fac->Release(); }
    if (SUCCEEDED(ci)) { CoUninitialize(); }
    return 0;
}

void MenuArtRefresh()
{
    if (!g.device) { return; }
    if (g_menuArtN == 0) {
        for (int i = 0; i < kArtFixedCount; ++i) {
            strncpy_s(g_menuArt[i].file, sizeof(g_menuArt[i].file), kMenuArtFile[i], _TRUNCATE);
        }
        g_menuArtN = kArtFixedCount;
        for (int t = 0; t < kTabCount && g_menuArtN < kArtMax; ++t) {
            char slug[48] = {};
            TabSlug(kTabs[t].name, slug, sizeof(slug));
            _snprintf_s(g_menuArt[g_menuArtN].file, sizeof(g_menuArt[g_menuArtN].file), _TRUNCATE, "tab_name_%s.png", slug);
            ++g_menuArtN;
        }
    }
    char dir[MAX_PATH] = {};
    if (!DlvrFilePath("menu", dir, sizeof(dir))) {
        if (!g_menuArtSaid) {
            g_menuArtSaid = true;
            // [PROBE: MENUART] gate=none (each menu open; first time unconditionally)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUART] could not resolve the dlvr\\menu folder - every menu piece is drawn by code");
        }
        return;
    }
    std::vector<ArtDecodeJob> jobs;
    jobs.reserve(static_cast<size_t>(g_menuArtN));
    int removed = 0;
    for (int i = 0; i < g_menuArtN; ++i) {
        MenuArt& a = g_menuArt[i];
        char p[MAX_PATH * 2] = {};
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", dir, a.file);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) {
            if (a.srv) { a.srv->Release(); a.srv = nullptr; a.mt = FILETIME{}; ++removed; }
            continue;
        }
        if (a.srv && CompareFileTime(&fa.ftLastWriteTime, &a.mt) == 0) { continue; }
        jobs.emplace_back();
        ArtDecodeJob& j = jobs.back();
        j.index = i;
        MultiByteToWideChar(CP_ACP, 0, p, -1, j.path, static_cast<int>(sizeof(j.path) / sizeof(j.path[0])));
        a.mt = fa.ftLastWriteTime;
    }
    int changed = 0, failed = 0;
    if (!jobs.empty()) {
        HANDLE th = CreateThread(nullptr, 0, ArtDecodeThread, &jobs, 0, nullptr);
        if (th) {
            WaitForSingleObject(th, INFINITE);   // the thread reads `jobs`, which lives on this stack
            CloseHandle(th);
        } else {
            for (ArtDecodeJob& j : jobs) { j.hr = HRESULT_FROM_WIN32(GetLastError()); }
        }
        for (ArtDecodeJob& j : jobs) {
            MenuArt& a = g_menuArt[j.index];
            ID3D11ShaderResourceView* srv = nullptr;
            if (SUCCEEDED(j.hr)) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = j.w; td.Height = j.h; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA sd{ j.px.data(), j.w * 4, 0 };
                ID3D11Texture2D* tex = nullptr;
                const HRESULT thr = g.device->CreateTexture2D(&td, &sd, &tex);
                if (SUCCEEDED(thr) && tex) {
                    g.device->CreateShaderResourceView(tex, nullptr, &srv);
                    tex->Release();
                }
                if (!srv) { j.hr = FAILED(thr) ? thr : E_FAIL; }
            }
            if (!srv) {
                ++failed;
                a.mt = FILETIME{};   // retried at the next open
                // [PROBE: MENUART] gate=none (a file that would not load - names it)  site=HEADSET  log-only=yes
                LOG("overlay: [MENUART] %s\\%s did NOT load (hr 0x%08lX) - that piece is drawn by code", dir, a.file,
                    static_cast<unsigned long>(j.hr));
                continue;
            }
            if (a.srv) { a.srv->Release(); }
            a.srv = srv;
            a.w = static_cast<int>(j.w);
            a.h = static_cast<int>(j.h);
            ++changed;
        }
    }
    if (g_menuArtSaid && changed == 0 && removed == 0 && failed == 0) { return; }
    g_menuArtSaid = true;
    int loaded = 0;
    char missing[1200] = {};
    size_t mlen = 0;
    for (int i = 0; i < g_menuArtN; ++i) {
        if (g_menuArt[i].srv) { ++loaded; continue; }
        const int w = _snprintf_s(missing + mlen, sizeof(missing) - mlen, _TRUNCATE, "%s%s", mlen ? ", " : "", g_menuArt[i].file);
        if (w > 0) { mlen += static_cast<size_t>(w); }
    }
    // Logged the first time unconditionally (all missing is the expected state until Jay's art lands - it must SAY so), then
    // only when something changed. The missing list names every file the menu looked for.
    // [PROBE: MENUART] gate=none (each menu open; first time unconditionally)  site=HEADSET  log-only=yes
    LOG("overlay: [MENUART] menu art in %s: %d of %d loaded (%d new or changed, %d removed, %d failed)%s%s", dir, loaded,
        g_menuArtN, changed, removed, failed, loaded < g_menuArtN ? " - drawn by code: " : "", missing);
}

const MenuArt* ArtOf(int id)
{
    return (id >= 0 && id < g_menuArtN && g_menuArt[id].srv) ? &g_menuArt[id] : nullptr;
}

ImTextureRef ArtTex(const MenuArt* a)
{
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(a->srv)));
}

void ArtDraw(ImDrawList* dl, const MenuArt* a, ImVec2 p0, ImVec2 p1, ImU32 col = IM_COL32_WHITE)
{
    dl->AddImage(ArtTex(a), p0, p1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), col);
}

// 9-slice: the corners keep their drawn size (border px, the same in the file and on the panel), the edges and middle stretch.
void ArtNine(ImDrawList* dl, const MenuArt* a, ImVec2 p0, ImVec2 p1, float border, ImU32 col = IM_COL32_WHITE)
{
    const float bw = (std::min)(border, a->w * 0.5f), bh = (std::min)(border, a->h * 0.5f);
    const float dx = (std::min)(bw, (p1.x - p0.x) * 0.5f), dy = (std::min)(bh, (p1.y - p0.y) * 0.5f);
    const float xs[4] = { p0.x, p0.x + dx, p1.x - dx, p1.x };
    const float ys[4] = { p0.y, p0.y + dy, p1.y - dy, p1.y };
    const float us[4] = { 0.0f, bw / a->w, 1.0f - bw / a->w, 1.0f };
    const float vs[4] = { 0.0f, bh / a->h, 1.0f - bh / a->h, 1.0f };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (xs[c + 1] <= xs[c] || ys[r + 1] <= ys[r]) { continue; }
            dl->AddImage(ArtTex(a), ImVec2(xs[c], ys[r]), ImVec2(xs[c + 1], ys[r + 1]), ImVec2(us[c], vs[r]),
                         ImVec2(us[c + 1], vs[r + 1]), col);
        }
    }
}

// Centred on c at its own size, scaled down (aspect kept) when larger than maxW x maxH.
void ArtFit(ImDrawList* dl, const MenuArt* a, ImVec2 c, float maxW, float maxH, ImU32 col = IM_COL32_WHITE)
{
    float w = static_cast<float>(a->w), h = static_cast<float>(a->h);
    const float s = (std::min)(1.0f, (std::min)(maxW / w, maxH / h));
    w *= s;
    h *= s;
    ArtDraw(dl, a, ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), col);
}

const ImU32 kAccent = IM_COL32(255, 150, 40, 255);   // the code-drawn highlight / fill (no art)

// The hold bar: hold_bar + hold_bar_fill cut by g.holdProgress (or code-drawn). Text left of the bar, or above it.
// Drawn at hold_bar.png's own size (Jay's is 440 x 40); kHoldBarW x 40 when there is no art.
constexpr float kHoldBarW = 440.0f;
void DrawHoldBar(ImDrawList* dl, ImVec2 barTopLeft, const char* text, bool textLeft)
{
    const MenuArt* bgArt = ArtOf(kArtHoldBg);
    const float w = bgArt ? static_cast<float>(bgArt->w) : kHoldBarW, h = bgArt ? static_cast<float>(bgArt->h) : 40.0f;
    const ImVec2 a = barTopLeft, b(a.x + w, a.y + h);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 tp = textLeft ? ImVec2(a.x - ts.x - 16.0f, a.y + (h - ts.y) * 0.5f) : ImVec2(a.x + (w - ts.x) * 0.5f, a.y - ts.y - 8.0f);
    dl->AddText(tp, IM_COL32_WHITE, text);
    const float t = g.holdProgress < 0.0f ? 0.0f : (g.holdProgress > 1.0f ? 1.0f : g.holdProgress);
    const float x = a.x + w * t;
    if (const MenuArt* bg = ArtOf(kArtHoldBg)) {
        ArtDraw(dl, bg, a, b);
    } else {
        dl->AddRectFilled(a, b, IM_COL32(20, 22, 28, 230), 8.0f);
        dl->AddRect(a, b, IM_COL32(200, 200, 200, 200), 8.0f, 0, 2.0f);
    }
    if (const MenuArt* fi = ArtOf(kArtHoldFill)) {
        dl->PushClipRect(a, ImVec2(x, b.y), true);
        ArtDraw(dl, fi, a, b);
        dl->PopClipRect();
    } else if (x > a.x + 1.0f) {
        dl->AddRectFilled(ImVec2(a.x + 3.0f, a.y + 3.0f), ImVec2(x - 3.0f > a.x + 3.0f ? x - 3.0f : a.x + 3.0f, b.y - 3.0f), kAccent,
                          6.0f);
    }
}

// ------------------------------------------------------------- menu rows ---
// Jay 2026-11-11: "Readable name only" - each setting is ONE line: its name in the left column (0-560 px), its control in the
// right (580 px on, 660 px wide), 48 px tall so checkbox_on/off (48x48), dropdown (660x48) and the knob (40x48) fit exactly. The
// row the controller is on gets row_highlight.png (or a code-drawn orange box), and its help shows in the strip under the list.
// Each row splits the window draw list in two: channel 0 = backgrounds (highlight, slider track / fill / knob, dropdown box),
// channel 1 = ImGui's widgets, so ImGui's own value text stays on top of the art. No other splitter is open around a row (no
// row is drawn inside a table).
// 2026-11-12, the BioShock-style layout (Jay): name 0-500, control from 520 to 1240 (720 wide: the dropdown box); a slider's bar
// is 520-1080 (kTrackW) and its value is right-aligned in its own column ending at 1240, as in Jay's reference.
constexpr float kRowFrameH = 48.0f;
constexpr float kNameW = 500.0f;
// The row name's inset from the list's left edge (Jay 2026-11-12: "Key names are being slightly cut by the highlight, they need
// to be moved a few pixels to the right"). The wrap still ends at kNameW.
constexpr float kNameInset = 10.0f;
constexpr float kCtrlX = 520.0f;
constexpr float kCtrlW = 720.0f;
constexpr float kTrackW = 560.0f;
constexpr float kTrackH = 24.0f;

struct MenuRow { ImDrawList* dl = nullptr; float y0 = 0.0f, x0 = 0.0f, x1 = 0.0f; bool focused = false; };

void RowOpen(MenuRow& r, const char* name, bool restart)
{
    r.dl = ImGui::GetWindowDrawList();
    r.dl->ChannelsSplit(2);
    r.dl->ChannelsSetCurrent(1);
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    r.y0 = cur.y;
    r.x0 = ImGui::GetWindowPos().x;
    r.x1 = cur.x + ImGui::GetContentRegionAvail().x;
    r.focused = false;
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x, (kRowFrameH - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));   // the row highlight replaces it
    ImGui::AlignTextToFramePadding();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kNameInset);
    ImGui::PushTextWrapPos(kNameW);
    ImGui::TextWrapped("%s", (name && name[0]) ? name : "?");
    ImGui::PopTextWrapPos();
    if (restart) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.4f, 1.0f), "(restart)");
    }
    ImGui::SameLine(kCtrlX);
    ImGui::SetNextItemWidth(kCtrlW);
}

void RowClose(MenuRow& r, const char* help, bool restart)
{
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    const float bottom = (std::max)(ImGui::GetItemRectMax().y, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y);
    if (r.focused) {
        r.dl->ChannelsSetCurrent(0);
        const ImVec2 a(r.x0 + 2.0f, r.y0 - 4.0f), b(r.x1, bottom + 4.0f);
        if (const MenuArt* hi = ArtOf(kArtRowHi)) {
            ArtNine(r.dl, hi, a, b, 16.0f);
        } else {
            r.dl->AddRectFilled(a, b, IM_COL32(255, 150, 40, 56), 6.0f);
            r.dl->AddRect(a, b, kAccent, 6.0f, 0, 3.0f);
        }
        // In slider adjust mode the focused row is the slider being adjusted (the cursor cannot leave it): an orange frame says so.
        if (g.adjusting) { r.dl->AddRect(a, b, kAccent, 6.0f, 0, 4.0f); }
        g.helpText = help;
        g.helpRestart = restart;
    }
    r.dl->ChannelsMerge();
}

// The right stick's step for the item just submitted: only the focused, not-active control takes it.
int FocusedStep()
{
    return (g.vrStep != 0 && ImGui::IsItemFocused() && !ImGui::IsItemActive()) ? g.vrStep : 0;
}

void PushClear(ImGuiCol a, ImGuiCol b, ImGuiCol c)
{
    const ImVec4 none(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(a, none);
    ImGui::PushStyleColor(b, none);
    ImGui::PushStyleColor(c, none);
}

// Right stick on a checkbox: right = on, left = off.
bool RowCheckbox(const char* name, const char* id, bool* v, const char* help = nullptr, bool restart = false)
{
    MenuRow r;
    RowOpen(r, name, restart);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const MenuArt* off = ArtOf(kArtCheckOff);
    const MenuArt* on = ArtOf(kArtCheckOn);
    const bool art = off && on;
    // ImGui's own box behind the checkbox is ALWAYS cleared (Jay 2026-11-11: "turn off the background that IMGUI has for the
    // checkboxes (It's showing a blue square behind the checkboxes ...)"). It was ImGuiCol_CheckboxSelectedBg - 1.92.8 paints a
    // TICKED box with it (imgui_widgets.cpp:1291), blue by default - which the FrameBg clear below never covered. ImGui's tick
    // mark is cleared only when the art is drawn instead.
    PushClear(ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive);
    ImGui::PushStyleColor(ImGuiCol_CheckboxSelectedBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (art) { ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); }
    bool changed = ImGui::Checkbox(id, v);
    ImGui::PopStyleColor(art ? 5 : 4);
    r.focused = ImGui::IsItemFocused();
    const int s = FocusedStep();
    if (s > 0 && !*v) { *v = true; changed = true; }
    if (s < 0 && *v) { *v = false; changed = true; }
    if (art) {
        const float h = ImGui::GetFrameHeight();
        ArtDraw(r.dl, *v ? on : off, p, ImVec2(p.x + h, p.y + h));
    }
    RowClose(r, help, restart);
    return changed;
}

// Slider art under ImGui's slider (channel 0): track, fill to the knob, knob. t = the value's place in the range, 0..1.
void SliderArt(MenuRow& r, ImVec2 p, float t)
{
    const MenuArt* tr = ArtOf(kArtSliderTrack);
    if (!tr) { return; }
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float cy = p.y + kRowFrameH * 0.5f;
    const ImVec2 a(p.x, cy - kTrackH * 0.5f), b(p.x + kTrackW, cy + kTrackH * 0.5f);
    const MenuArt* kn = ArtOf(kArtSliderKnob);
    const float kw = kn ? static_cast<float>(kn->w) : 0.0f, kh = kn ? static_cast<float>(kn->h) : 0.0f;
    const float kx = p.x + kw * 0.5f + t * (kTrackW - kw);
    r.dl->ChannelsSetCurrent(0);
    ArtNine(r.dl, tr, a, b, 12.0f);
    if (const MenuArt* fi = ArtOf(kArtSliderFill)) {
        r.dl->PushClipRect(a, ImVec2(kx, b.y), true);
        ArtNine(r.dl, fi, a, b, 12.0f);
        r.dl->PopClipRect();
    }
    if (kn) { ArtDraw(r.dl, kn, ImVec2(kx - kw * 0.5f, cy - kh * 0.5f), ImVec2(kx + kw * 0.5f, cy + kh * 0.5f)); }
    r.dl->ChannelsSetCurrent(1);
}

// With track art, ImGui's own frame and grab are cleared (5 colours); returns whether to pop them.
bool PushSliderClear()
{
    if (!ArtOf(kArtSliderTrack)) { return false; }
    PushClear(ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    return true;
}

// The right stick's step is NOT applied here: callers keep their ArrowStepFocusedSlider call right after (it reads the slider
// as the last item), so each slider's own step size and save rule stay as they were.
// The slider's value, right-aligned in the value column (kCtrlX + kTrackW .. kCtrlX + kCtrlW), on the widgets channel. ImGui's
// own value text inside the bar is drawn transparent (the slider still parses / rounds with the same format).
void SliderValueText(MenuRow& r, ImVec2 p, const char* text)
{
    const ImVec2 ts = ImGui::CalcTextSize(text);
    r.dl->AddText(ImVec2(p.x + kCtrlW - ts.x, p.y + (kRowFrameH - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), text);
}

bool RowSliderF(const char* name, const char* id, float* v, float lo, float hi, const char* fmt, const char* help = nullptr,
                bool restart = false)
{
    MenuRow r;
    RowOpen(r, name, restart);
    ImGui::SetNextItemWidth(kTrackW);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clr = PushSliderClear();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool changed = ImGui::SliderFloat(id, v, lo, hi, fmt);
    ImGui::PopStyleColor(clr ? 6 : 1);
    r.focused = ImGui::IsItemFocused();
    SliderArt(r, p, hi > lo ? (*v - lo) / (hi - lo) : 0.0f);
    char vb[64];
    _snprintf_s(vb, sizeof(vb), _TRUNCATE, fmt ? fmt : "%.2f", static_cast<double>(*v));
    SliderValueText(r, p, vb);
    RowClose(r, help, restart);
    return changed;
}

bool RowSliderI(const char* name, const char* id, int* v, int lo, int hi, const char* help = nullptr, bool restart = false,
                const char* fmt = "%d")
{
    MenuRow r;
    RowOpen(r, name, restart);
    ImGui::SetNextItemWidth(kTrackW);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clr = PushSliderClear();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool changed = ImGui::SliderInt(id, v, lo, hi, fmt);
    ImGui::PopStyleColor(clr ? 6 : 1);
    r.focused = ImGui::IsItemFocused();
    SliderArt(r, p, hi > lo ? static_cast<float>(*v - lo) / static_cast<float>(hi - lo) : 0.0f);
    char vb[64];
    _snprintf_s(vb, sizeof(vb), _TRUNCATE, fmt, *v);
    SliderValueText(r, p, vb);
    RowClose(r, help, restart);
    return changed;
}

// A dropdown. Right stick on it (closed): the previous / next entry.
// NO BOX (Jay 2026-11-11: "I don't think dropdown.png would be needed either. Same thing as before, it'll be highlighted anyway"):
// ImGui's frame and its arrow square are cleared - the closed dropdown is its value text + the arrow (dropdown_arrow.png, or
// ImGui's own arrow glyph), and the row highlight marks it when the controller is on it.
bool RowCombo(const char* name, const char* id, int* cur, const char* const* items, int n, const char* help = nullptr,
              bool restart = false)
{
    MenuRow r;
    RowOpen(r, name, restart);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const MenuArt* arrow = ArtOf(kArtDropdownArrow);
    // THE BOX IS BACK (Jay 2026-11-12, "Dropdown box" brought back for the BioShock-style layout): dropdown.png behind the
    // closed dropdown (channel 0), or a code-drawn box. ImGui's own frame and arrow square stay cleared.
    {
        r.dl->ChannelsSetCurrent(0);
        const ImVec2 a(p.x, p.y), b(p.x + kCtrlW, p.y + kRowFrameH);
        if (const MenuArt* box = ArtOf(kArtDropdown)) {
            ArtNine(r.dl, box, a, b, 16.0f);
        } else {
            r.dl->AddRectFilled(a, b, IM_COL32(24, 26, 32, 230), 6.0f);
            r.dl->AddRect(a, b, IM_COL32(150, 150, 150, 200), 6.0f, 0, 2.0f);
        }
        r.dl->ChannelsSetCurrent(1);
    }
    PushClear(ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive);
    PushClear(ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive);
    // The OPEN list (a popup window, its own draw list): its background and border, and ImGui's item fills, cleared - the list
    // gets dropdown_list.png (or a code-drawn box) and the item the controller is on gets the row highlight.
    PushClear(ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const int idx = (*cur >= 0 && *cur < n) ? *cur : -1;
    bool changed = false;
    const bool open = ImGui::BeginCombo(id, idx >= 0 ? items[idx] : "", arrow ? ImGuiComboFlags_NoArrowButton : 0);
    if (open) {
        // The popup's own draw list, three channels: 0 = the list's background, 1 = the focused item's highlight, 2 = the items.
        // The popup is a separate window, so no other splitter is open on this list.
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        pdl->ChannelsSplit(3);
        pdl->ChannelsSetCurrent(2);
        for (int i = 0; i < n; ++i) {
            const bool sel = (i == idx);
            if (sel) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.45f, 1.0f)); }   // the current value
            if (ImGui::Selectable(items[i], sel)) {
                *cur = i;
                changed = true;
            }
            if (sel) { ImGui::PopStyleColor(); }
            if (ImGui::IsItemFocused()) {
                pdl->ChannelsSetCurrent(1);
                const ImVec2 ia = ImGui::GetItemRectMin(), ib = ImGui::GetItemRectMax();
                if (const MenuArt* hi = ArtOf(kArtRowHi)) {
                    ArtNine(pdl, hi, ImVec2(ia.x - 4.0f, ia.y - 2.0f), ImVec2(ib.x + 4.0f, ib.y + 2.0f), 16.0f);
                } else {
                    pdl->AddRectFilled(ia, ib, IM_COL32(255, 150, 40, 56), 4.0f);
                    pdl->AddRect(ia, ib, kAccent, 4.0f, 0, 2.0f);
                }
                pdl->ChannelsSetCurrent(2);
            }
            if (sel) { ImGui::SetItemDefaultFocus(); }
        }
        pdl->ChannelsSetCurrent(0);
        const ImVec2 wa = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        const ImVec2 wb(wa.x + ws.x, wa.y + ws.y);
        pdl->PushClipRect(wa, wb, false);   // the window's own clip excludes its padding; the box covers the whole popup
        if (const MenuArt* box = ArtOf(kArtDropdownList)) {
            ArtNine(pdl, box, wa, wb, 16.0f);
        } else {
            pdl->AddRectFilled(wa, wb, IM_COL32(18, 20, 26, 245), 6.0f);
            pdl->AddRect(wa, wb, IM_COL32(150, 150, 150, 220), 6.0f, 0, 2.0f);
        }
        pdl->PopClipRect();
        pdl->ChannelsMerge();
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(11);
    r.focused = open || ImGui::IsItemFocused();
    if (!open && n > 0) {
        const int s = FocusedStep();
        if (s != 0) {
            int nx = (idx < 0 ? 0 : idx) + (s > 0 ? 1 : -1);
            nx = nx < 0 ? 0 : (nx >= n ? n - 1 : nx);
            if (nx != *cur) {
                *cur = nx;
                changed = true;
            }
        }
    }
    const float h = kRowFrameH;
    if (arrow) {
        const float cy = p.y + h * 0.5f, ax = p.x + kCtrlW - 16.0f - arrow->w * 0.5f;
        ArtFit(r.dl, arrow, ImVec2(ax, cy), 32.0f, 32.0f);
    }
    RowClose(r, help, restart);
    return changed;
}

// A dropdown section (was ImGui::TreeNode / CollapsingHeader): section_open / _closed.png + its name, closed by default.
// NO BAR (Jay 2026-11-11: "I don't think we need a sectionbar, that can just be highlighted"): nothing behind it, and while the
// controller is on it, the same highlight as a setting row (row_highlight.png, or the code-drawn orange box), under the arrow and
// name (channel 0 of a two-channel split, as RowClose).
// treePush = TreeNode behaviour (the caller TreePops when this returns true); false = CollapsingHeader (no pop).
// Right stick on it: right opens, left closes. `id` must be stable (a count in the label must not be in it).
bool MenuSection(const char* label, const char* id, bool treePush)
{
    ImGui::PushID(id);
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID("open");
    bool open = store->GetBool(key, false);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 48.0f;
    const float x0 = ImGui::GetWindowPos().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    // ImGui's own hover / nav fill and nav frame are cleared - the row highlight below replaces them.
    PushClear(ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    bool clicked = ImGui::Selectable("##section", false, 0, ImVec2(w, h));
    ImGui::PopStyleColor(4);
    const bool focused = ImGui::IsItemFocused();
    const int s = FocusedStep();
    if ((s > 0 && !open) || (s < 0 && open)) { clicked = true; }
    if (clicked) {
        open = !open;
        store->SetBool(key, open);
    }
    // THE BANNER IS BACK (Jay 2026-11-12, "Section banner" brought back for the BioShock-style layout): section_bar.png behind
    // every section, under the highlight - or a code-drawn bar.
    {
        dl->ChannelsSetCurrent(0);
        const ImVec2 a(p.x, p.y), b(p.x + w, p.y + h);
        if (const MenuArt* bar = ArtOf(kArtSectionBar)) {
            ArtNine(dl, bar, a, b, 48.0f);
        } else {
            dl->AddRectFilled(a, b, IM_COL32(60, 52, 34, 200), 4.0f);
            dl->AddRect(a, b, IM_COL32(170, 145, 90, 220), 4.0f, 0, 2.0f);
        }
        dl->ChannelsSetCurrent(1);
    }
    if (focused) {
        dl->ChannelsSetCurrent(0);
        const ImVec2 a(x0 + 2.0f, p.y - 4.0f), b(p.x + w, p.y + h + 4.0f);
        if (const MenuArt* hi = ArtOf(kArtRowHi)) {
            ArtNine(dl, hi, a, b, 16.0f);
        } else {
            dl->AddRectFilled(a, b, IM_COL32(255, 150, 40, 56), 6.0f);
            dl->AddRect(a, b, kAccent, 6.0f, 0, 3.0f);
        }
        dl->ChannelsSetCurrent(1);
    }
    const float cy = p.y + h * 0.5f;
    if (const MenuArt* ar = ArtOf(open ? kArtSectionOpen : kArtSectionClosed)) {
        ArtFit(dl, ar, ImVec2(p.x + 28.0f, cy), 32.0f, 32.0f);
    } else {
        const ImU32 c = ImGui::GetColorU32(ImGuiCol_Text);
        if (open) {
            dl->AddTriangleFilled(ImVec2(p.x + 18.0f, cy - 6.0f), ImVec2(p.x + 38.0f, cy - 6.0f), ImVec2(p.x + 28.0f, cy + 8.0f), c);
        } else {
            dl->AddTriangleFilled(ImVec2(p.x + 21.0f, cy - 10.0f), ImVec2(p.x + 21.0f, cy + 10.0f), ImVec2(p.x + 35.0f, cy), c);
        }
    }
    dl->AddText(ImVec2(p.x + 56.0f, cy - ImGui::GetFontSize() * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
    dl->ChannelsMerge();
    ImGui::PopID();
    if (open && treePush) { ImGui::TreePush(id); }
    return open;
}

// A fixed heading (was ImGui::SeparatorText): the section banner with the text, no arrow, not focusable. 2026-11-12, the
// BioShock-style layout - the AA tab's groups and the setting-group headings look like the sections. Not inside a table.
void MenuHeading(const char* text)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 48.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y), b(p.x + w, p.y + h);
    if (const MenuArt* bar = ArtOf(kArtSectionBar)) {
        ArtNine(dl, bar, a, b, 48.0f);
    } else {
        dl->AddRectFilled(a, b, IM_COL32(60, 52, 34, 200), 4.0f);
        dl->AddRect(a, b, IM_COL32(170, 145, 90, 220), 4.0f, 0, 2.0f);
    }
    dl->AddText(ImVec2(p.x + 28.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), text);
    ImGui::Dummy(ImVec2(w, h));
}

// A divider (was ImGui::Separator): divider.png at the size it is made, centred - or a code-drawn line with a centre mark.
void MenuDivider()
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const MenuArt* d = ArtOf(kArtDivider);
    const float h = d ? static_cast<float>(d->h) : 24.0f;
    if (d) {
        const float dw = (std::min)(static_cast<float>(d->w), w);
        ArtDraw(dl, d, ImVec2(p.x + (w - dw) * 0.5f, p.y), ImVec2(p.x + (w + dw) * 0.5f, p.y + h));
    } else {
        const float cy = p.y + h * 0.5f, cx = p.x + w * 0.5f;
        const ImU32 c = IM_COL32(170, 145, 90, 200);
        dl->AddLine(ImVec2(p.x, cy), ImVec2(cx - 24.0f, cy), c, 2.0f);
        dl->AddLine(ImVec2(cx + 24.0f, cy), ImVec2(p.x + w, cy), c, 2.0f);
        dl->AddQuadFilled(ImVec2(cx, cy - 7.0f), ImVec2(cx + 9.0f, cy), ImVec2(cx, cy + 7.0f), ImVec2(cx - 9.0f, cy), c);
    }
    ImGui::Dummy(ImVec2(w, h));
}

// A menu button (was ImGui::Button): button.png / button_selected.png (the one the controller is on) behind ImGui's button,
// whose own fills are cleared - or a code-drawn box with an orange frame when focused. Drawn BEFORE the button, focus taken
// from the nav id (ImGui::GetFocusID() == this button's id), so it needs no draw-list split and works inside a table cell
// (a table holds its own splitter). Size as ImGui::Button: 0 = fit the label, -FLT_MIN = fill. `help` shows in the description
// box while the button is focused. A disabled button (BeginDisabled) is drawn at the disabled alpha.
bool MenuButton(const char* label, ImVec2 size = ImVec2(0.0f, 0.0f), const char* help = nullptr)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 ls = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 sz = ImGui::CalcItemSize(size, ls.x + st.FramePadding.x * 2.0f, ls.y + st.FramePadding.y * 2.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 b(p.x + sz.x, p.y + sz.y);
    const bool focused = ImGui::GetFocusID() == ImGui::GetID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 tint = ImGui::GetColorU32(IM_COL32_WHITE);   // carries BeginDisabled's alpha
    const MenuArt* sel = ArtOf(kArtButtonSel);
    const MenuArt* art = (focused && sel) ? sel : ArtOf(kArtButton);
    if (art) {
        ArtNine(dl, art, p, b, 16.0f, tint);
    } else {
        dl->AddRectFilled(p, b, ImGui::GetColorU32(IM_COL32(28, 30, 36, 235)), 6.0f);
        dl->AddRect(p, b, ImGui::GetColorU32(IM_COL32(150, 150, 150, 200)), 6.0f, 0, 2.0f);
    }
    if (focused && !sel) { dl->AddRect(p, b, kAccent, 6.0f, 0, 3.0f); }
    PushClear(ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool pressed = ImGui::Button(label, sz);
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemFocused() && help) { g.helpText = help; }
    return pressed;
}

// ------------------------------------------------------------------ input ---

// ImGui navigation, fed by hand.
//
// The Win32 backend is deliberately not used: it wants the game's window and a
// WndProc hook, and we have neither a reason nor the right to take those. We
// need a handful of keys, and feeding them directly is both smaller and less
// intrusive than owning the window's message loop.
void FeedInput(ImGuiIO& io)
{
    const double now = Now();
    io.DeltaTime = (g.lastFrame > 0.0)
                       ? static_cast<float>(now - g.lastFrame)
                       : (1.0f / 90.0f);
    if (io.DeltaTime <= 0.0f) {
        io.DeltaTime = 1.0f / 90.0f;
    }
    g.lastFrame = now;
    io.DisplaySize = ImVec2(static_cast<float>(kPanelW), static_cast<float>(kMenuH));   // back to kPanelH after the frame (OverlayRender)

    struct KeyMap { int vk; ImGuiKey key; };
    static const KeyMap keys[] = {
        { VK_LEFT, ImGuiKey_LeftArrow },   { VK_RIGHT, ImGuiKey_RightArrow },
        { VK_UP, ImGuiKey_UpArrow },       { VK_DOWN, ImGuiKey_DownArrow },
        { VK_RETURN, ImGuiKey_Enter },     { VK_ESCAPE, ImGuiKey_Escape },
        { VK_SPACE, ImGuiKey_Space },      { VK_TAB, ImGuiKey_Tab },
        { VK_BACK, ImGuiKey_Backspace },
    };
    for (const KeyMap& k : keys) {
        io.AddKeyEvent(k.key, (GetAsyncKeyState(k.vk) & 0x8000) != 0);
    }

    XINPUT_STATE st;
    const bool xpad = ReadPad(st);
    if (!xpad) { ZeroMemory(&st, sizeof(st)); }
    VrMenuPad vr;
    ReadVrMenuPad(vr);
    g.padSeen = xpad;
    g.vrPadSeen = vr.ok;
    g.vrStep = 0;
    g.bEdge = false;
    if (!xpad && !vr.ok) {
        // ImGui clears every gamepad key at the top of NewFrame unless the
        // backend says a pad is connected, so this flag is not decoration - it
        // is the difference between the input being used and being discarded.
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        g.vrStepDir = 0;
        g.vrStepHeld = false;
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const bool bDown = vr.b || (st.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
    const bool gripOn[2] = { vr.lg, vr.rg };
    // The Controls tab owns the controller while it captures a press, and until every pad input is released after it - the
    // press that was captured must not also move / activate the menu (an A captured would re-open the slot it just set).
    // Idle = the Xbox pad AND the Touch controllers (the Controls tab captures from Touch).
    const bool xIdle = st.Gamepad.wButtons == 0 && st.Gamepad.bLeftTrigger < 30 && st.Gamepad.bRightTrigger < 30;
    if (ControlsHoldPad(xIdle && vr.idle)) {
        // The edges restart from what is held now, so letting go after a capture is not read as a press.
        g.bPrev = bDown;
        g.vrGripPrev[0] = gripOn[0];
        g.vrGripPrev[1] = gripOn[1];
        g.vrStepDir = 0;
        g.vrStepHeld = false;
        return;
    }
    g.bEdge = bDown && !g.bPrev;
    g.bPrev = bDown;
    // SLIDER ADJUST MODE (Overlay::adjusting): Touch A on the slider the cursor is on enters it, A or B leaves it; that A / B is
    // NOT sent to ImGui (so it never activates the slider or opens its text box) and the B does not close the menu. An Xbox
    // pad's A / B still go to ImGui exactly as before.
    {
        const bool aEdge = vr.a && !g.aPrev;
        g.aPrev = vr.a;
        if (!vr.a) { g.aSwallow = false; }
        if (!vr.b) { g.bSwallow = false; }
        const bool vrBEdge = vr.b && g.bEdge;
        if (g.adjusting) {
            if (aEdge || vrBEdge) {
                g.adjusting = false;
                g.adjustId = 0;
                if (aEdge) { g.aSwallow = true; }
                if (vrBEdge) {
                    g.bSwallow = true;
                    g.bEdge = false;
                }
                // [PROBE: MENUVR] gate=none (each slider adjust mode on / off - event-driven)  site=HEADSET  log-only=yes
                LOG("overlay: [MENUVR] slider adjust mode OFF (%s)", aEdge ? "A" : "B");
            }
        } else if (aEdge && g.sliderFocused != 0) {
            g.adjusting = true;
            g.adjustId = g.sliderFocused;
            g.aSwallow = true;
            // [PROBE: MENUVR] gate=none (each slider adjust mode on / off - event-driven)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUVR] slider adjust mode ON (A on the focused slider) - the right stick moves it");
        }
    }
    const WORD b = st.Gamepad.wButtons;
    // Touch: the left stick's up / down is fed as the d-pad - ImGui 1.92 moves between items on the d-pad only (imgui.cpp
    // NavUpdateCreateMoveRequest reads GamepadDpad*; the left stick only scrolls). Hysteresis so a stick resting near the
    // threshold does not chatter; a held stick repeats at ImGui's key-repeat rate, like a held d-pad.
    auto hyst = [](bool& on, float v) {
        if (!on && v > 0.55f) { on = true; } else if (on && v < 0.35f) { on = false; }
    };
    hyst(g.vrUpDown[0], vr.ly);
    hyst(g.vrUpDown[1], -vr.ly);
    // Left / right too (Jay 2026-11-12: the top buttons "can't use the joystick to switch between them" -> "Left stick
    // left/right"): fed as d-pad left / right, so the cursor moves between the button row's buttons and between columns where a
    // page has them. Only while X is the stronger axis, so a push up or down that leans sideways does not also change column.
    hyst(g.vrLeftRight[0], -vr.lx);
    hyst(g.vrLeftRight[1], vr.lx);
    const bool lrAxis = fabsf(vr.lx) > fabsf(vr.ly);
    struct PadMap { WORD mask; ImGuiKey key; bool vrDown; };
    // While adjusting a slider the left stick does not move the cursor off it; the adjust-mode A / B are swallowed (above).
    const PadMap pads[] = {
        { XINPUT_GAMEPAD_DPAD_UP, ImGuiKey_GamepadDpadUp, g.vrUpDown[0] && !g.adjusting },
        { XINPUT_GAMEPAD_DPAD_DOWN, ImGuiKey_GamepadDpadDown, g.vrUpDown[1] && !g.adjusting },
        { XINPUT_GAMEPAD_DPAD_LEFT, ImGuiKey_GamepadDpadLeft, g.vrLeftRight[0] && lrAxis && !g.adjusting },
        { XINPUT_GAMEPAD_DPAD_RIGHT, ImGuiKey_GamepadDpadRight, g.vrLeftRight[1] && lrAxis && !g.adjusting },
        { XINPUT_GAMEPAD_A, ImGuiKey_GamepadFaceDown, vr.a && !g.aSwallow },
        { XINPUT_GAMEPAD_B, ImGuiKey_GamepadFaceRight, vr.b && !g.bSwallow },
        { XINPUT_GAMEPAD_X, ImGuiKey_GamepadFaceLeft, false },
        { XINPUT_GAMEPAD_Y, ImGuiKey_GamepadFaceUp, false },
    };
    for (const PadMap& p : pads) {
        io.AddKeyEvent(p.key, (b & p.mask) != 0 || p.vrDown);
    }

    // Touch right stick left / right: a step for the focused control (FocusedStep / ArrowStepFocusedSlider). The first step
    // lands at once, then repeats after kVrRepeatDelay every kVrRepeatEvery; held past kVrFastAfter each step counts kVrFastMul.
    {
        const int dir = vr.rx > 0.6f ? 1 : (vr.rx < -0.6f ? -1 : 0);
        if (dir == 0) {
            g.vrStepDir = 0;
            g.vrStepMul = 1.0f;
        } else if (dir != g.vrStepDir) {
            g.vrStepDir = dir;
            g.vrStepStart = now;
            g.vrStepNext = now + kVrRepeatDelay;
            g.vrStepMul = 1.0f;
            g.vrStep = dir;
        } else if (now >= g.vrStepNext) {
            g.vrStepMul = (now - g.vrStepStart) >= kVrFastAfter ? kVrFastMul : 1.0f;
            g.vrStepNext = now + kVrRepeatEvery;
            g.vrStep = dir;
        }
        g.vrStepHeld = dir != 0;
        static bool saidStep = false;
        if (g.vrStep != 0 && !saidStep) {
            saidStep = true;
            // [PROBE: MENUVR] gate=none (the first right-stick step the menu sees - proves the stick reaches it)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUVR] right stick step FIRST SEEN (x %.2f) - a slider takes it once A is pressed on it; logged once",
                static_cast<double>(vr.rx));
        }
    }

    // Left stick drives the same navigation as the d-pad, with a dead zone.
    const float lx = st.Gamepad.sThumbLX / 32767.0f;
    const float ly = st.Gamepad.sThumbLY / 32767.0f;
    const float dz = 0.45f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lx < -dz, lx < -dz ? -lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > dz, lx > dz ? lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > dz, ly > dz ? ly : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < -dz, ly < -dz ? -ly : 0.0f);

    // Shoulder buttons page between tabs. Edge-detected, or one press would
    // sweep through every tab in a few frames.
    // The Touch grips do the same (left = previous, right = next), each edge-detected.
    const bool lb = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    const bool rb = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    int page = 0;
    if ((lb || rb) && !g.shoulderPrev) { page = rb ? 1 : -1; }
    g.shoulderPrev = (lb || rb);
    if (gripOn[0] && !g.vrGripPrev[0]) { page = -1; }
    if (gripOn[1] && !g.vrGripPrev[1]) { page = 1; }
    g.vrGripPrev[0] = gripOn[0];
    g.vrGripPrev[1] = gripOn[1];
    if (page != 0) {
        g.tab += page;
        if (g.tab < 0) {
            g.tab = kTabCount - 1;
        } else if (g.tab >= kTabCount) {
            g.tab = 0;
        }
        g.tabRequest = true;
    }

    // Left trigger passes the right stick through to the game, so gamepad look
    // can be tested with the panel open - which the convention table needs, and
    // could not have while the overlay suppressed every input.
    g.passLook = st.Gamepad.bLeftTrigger > 60;
}

// True while every button in `mask` is held, and mask is not empty.
bool PadComboHeld(int mask)
{
    if (mask == 0) {
        return false;
    }
    XINPUT_STATE st;
    if (!ReadPad(st)) {
        return false;
    }
    return (st.Gamepad.wButtons & static_cast<WORD>(mask)) == static_cast<WORD>(mask);
}

// ------------------------------------------------------------------- UI -----

void HelpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    // BeginTooltip returns false when the tooltip is not going to be drawn, and
    // EndTooltip must not be called then - doing so pops a popup that was never
    // pushed and ImGui asserts on the mismatch. It used to be void, which is why
    // this was written without the check.
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
        if (ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(460.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

// Rebinding capture. Consumes the first key or button seen and stores it.
void ServiceCapture()
{
    if (g.capturing == 1) {
        for (int vk = 0x08; vk < 0xFF; ++vk) {
            // Skip mouse buttons; binding the overlay to a click would make it
            // impossible to use the menu it opens.
            if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) {
                continue;
            }
            if ((GetAsyncKeyState(vk) & 0x8000) == 0) {
                continue;
            }
            if (vk == VK_ESCAPE) {
                g.capturing = 0;
                SetStatus("rebind cancelled");
                return;
            }
            OverlayKey() = vk;
            g.capturing = 0;
            SetStatus("keyboard binding set");
            return;
        }
    } else if (g.capturing == 2) {
        XINPUT_STATE st;
        if (ReadPad(st) && st.Gamepad.wButtons != 0) {
            OverlayPad() = st.Gamepad.wButtons;
            g.capturing = 0;
            SetStatus("gamepad binding set");
        }
    }
}

// The tab strip (was ImGui's tab bar): tab.png / tab_selected.png behind each, the tab's NAME image tab_name_<slug>.png (228 x 40
// at most) or its text on top. Each tab is a focusable button - A on it selects; the grips page from anywhere (FeedInput).
// 2026-11-12, the BioShock-style layout ("Tab boxes"): each tab is as wide as its name (the name image's drawn width, or the
// text's) + 24 px each side, at least 120 - compact boxes in one strip, as in Jay's reference.
void DrawTabStrip()
{
    const float tabH = 56.0f, gap = 6.0f, pad = 24.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float x = origin.x;
    // 2026-11-27 (the fifth tab, Physical Parkour): four 228-px name images + padding already took ~1120 px, so five overflowed the
    // panel. When the natural widths do not fit the row, every tab - box, name image and text - is scaled down by the same factor.
    const auto nameWidth = [](int i) {
        float w = ImGui::CalcTextSize(kTabs[i].name).x;
        if (const MenuArt* nm = ArtOf(kArtFixedCount + i)) {
            const float s = (std::min)(1.0f, (std::min)(228.0f / nm->w, 40.0f / nm->h));   // ArtFit's scale below
            w = nm->w * s;
        }
        return w;
    };
    float natural = 0.0f;
    for (int i = 0; i < kTabCount; ++i) { natural += (std::max)(120.0f, nameWidth(i) + 2.0f * pad); }
    const float room = ImGui::GetContentRegionAvail().x - gap * static_cast<float>(kTabCount - 1);
    const float fit = natural > room && room > 0.0f ? room / natural : 1.0f;
    for (int i = 0; i < kTabCount; ++i) {
        ImGui::PushID(i);
        const float nameW = nameWidth(i);
        const float tabW = (std::max)(120.0f, nameW + 2.0f * pad) * fit;
        const ImVec2 p(x, origin.y);
        const ImVec2 q(p.x + tabW, p.y + tabH);
        x += tabW + gap;
        const bool sel = (g.tab == i);
        // A missing tab.png / tab_selected.png draws NOTHING behind the name (Jay 2026-11-11: "Make a missing tab file draw
        // nothing") - the selected tab still reads by its name: white, the others grey.
        if (const MenuArt* bg = ArtOf(sel ? kArtTabSel : kArtTab)) {
            ArtNine(dl, bg, p, q, 16.0f);
        }
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##tab", ImVec2(tabW, tabH))) { g.tab = i; }
        const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
        const ImU32 col = sel ? IM_COL32_WHITE : IM_COL32(190, 190, 190, 255);
        if (const MenuArt* nm = ArtOf(kArtFixedCount + i)) {
            ArtFit(dl, nm, c, 228.0f * fit, 40.0f * fit, col);
        } else {
            const float fs = ImGui::GetFontSize() * fit;
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, kTabs[i].name);
            dl->AddText(ImGui::GetFont(), fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, kTabs[i].name);
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + tabH + 8.0f));
}

// The bottom 56 px. NO CONTROL HINTS (Jay 2026-11-11: "I don't think we need to show the controls either (no btn_a,b etc)") - the
// strip is empty except while L3 + R3 are held with the menu open: then the close-hold bar, right-aligned.
void DrawHintStrip(ImVec2 p0, float w)
{
    if (g.holdProgress <= 0.0f) { return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const MenuArt* bgArt = ArtOf(kArtHoldBg);
    const float bw = bgArt ? static_cast<float>(bgArt->w) : kHoldBarW, bh = bgArt ? static_cast<float>(bgArt->h) : 40.0f;
    const float cy = p0.y + 56.0f * 0.5f;
    DrawHoldBar(dl, ImVec2(p0.x + w - bw - 8.0f, cy - bh * 0.5f), "Keep holding L3 + R3 to close", true);
}

template <typename T>
bool ArrowStepFocusedSlider(T* v, T lo, T hi, T step);   // defined below; DrawUi's Text size row uses it

void DrawUi()
{
    if (g.justOpened) {
        MenuArtRefresh();     // a changed PNG shows on the next open, no restart
        g.adjusting = false;  // a slider adjust never outlives a close
        g.adjustId = 0;
    }
    g.helpText = nullptr;
    g.helpRestart = false;
    g.sliderFocused = 0;   // FeedInput (before this frame) read last frame's; this frame's sliders set it again
    // B with nothing to back out of closes the menu. "Nothing" is judged on the LAST panel frame (busyPrev): by now ImGui's
    // NewFrame has already used this B to close an open dropdown or let go of a slider, so this frame alone cannot tell.
    if (g.bEdge && !g.busyPrev && !ControlsBackOut()) { g.closeRequest = true; }

    const float W = static_cast<float>(kPanelW), H = static_cast<float>(kMenuH);
    // THE MENU FONT AND TEXT SIZE (Jay 2026-11-12: "You supply a TTF"; Text size "Yes, add it"): dlvr\menu\font.ttf when it loaded
    // (OverlayInit), else ImGui's built-in font; overlay_text_size is a percent of the size the menu always had - 13 px base x
    // style.FontScaleMain 1.6 = 20.8 px at 100. Pushed for the whole menu window (its popups and children inherit it); the
    // reticle, the hold bar and the calibration screen keep the built-in font at their old size.
    {
        int pct = OverlayTextSize();
        pct = pct < 60 ? 60 : (pct > 150 ? 150 : pct);
        ImGui::PushFont(g_menuFont, kMenuFontBase * static_cast<float>(pct) / 100.0f);
    }
    const MenuArt* bgArt = ArtOf(kArtPanelBg);
    if (bgArt) { ArtDraw(ImGui::GetBackgroundDrawList(), bgArt, ImVec2(0.0f, 0.0f), ImVec2(W, H)); }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    if (g.justOpened) {
        ImGui::SetNextWindowFocus();
    }
    if (bgArt) { ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); }
    ImGui::Begin("dlvr", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (bgArt) { ImGui::PopStyleColor(); }

    if (g.justOpened) {
        // Make the nav cursor visible straight away rather than on the first
        // press, so it is obvious the panel has focus.
        ImGui::SetNavCursorVisible(true);
        g.justOpened = false;
    }

    // THE LAYOUT, top to bottom (Jay 2026-11-12, the BioShock-style menu; panel 1300 x kMenuH):
    //   y  16- 80  the button row (DrawButtonRow: Re-centre HUD / Calibrate my arms / Basic-Advanced, 400 x 64 each)
    //   y  96-152  the tab strip
    //   y 160-684  the list (scrolls; styled scrollbar at its right edge)
    //   y 692-856  the description box (kHelpH)
    //   y 856-888  the divider
    //   y 888-944  Text size (overlay_text_size)
    //   y 944-1000 the close-hold bar's strip
    // The old header line (the controllers' positions and the right trigger, a debug readout) is gone - the button row takes its
    // place. title.png stays on its own quad above the panel (Jay: "Keep it above"); with no title.png there is no title.
    ImGui::SetCursorPosY(16.0f);
    DrawButtonRow();
    ImGui::SetCursorPosY(96.0f);
    DrawTabStrip();
    // TABLE-DRIVEN as of 2026-09-13. The previous form hardcoded sel0..sel3
    // beside a separate `kTabCount` that the shoulder-button paging wrapped on -
    // two copies of "how many tabs there are", to be edited in step. This file's
    // own history is full of counters that covered one of two call sites, so
    // kTabCount is now DERIVED from the table and the paging cannot disagree
    // with the bar.
    // Holster place-by-hand (plan 2026-10-30 Task 5): stamped before the tabs so HolsterPageAfterTabs can tell whether the
    // Controls tab drew this panel frame. COST: one QueryPerformanceCounter per panel frame (the panel is open).
    const double uiStart = Now();
    if (g.tab < 0 || g.tab >= kTabCount) { g.tab = 0; }
    // THE LIST: one child per tab (its own scroll), NavFlattened so the d-pad / left stick crosses from the tab strip into it.
    const ImGuiStyle& style = ImGui::GetStyle();
    // THE DESCRIPTION BOX (Jay 2026-11-11: "fix the size of the description box, as text is being cut off"): it was 52 px (two
    // lines) above a 56 px hint strip. The hints are gone, so the box now runs to the bottom of the panel, kHelpH tall: ~7 lines
    // at this font, sized from the longest help a BETA row can show (rig_elbow_body_tracking, 489 chars ~ 113 per line = 5-6
    // lines) + the adjust / restart line. The close-hold bar shows in its bottom 56 px while L3 + R3 are held (text hidden then).
    constexpr float kHelpH = 164.0f;
    constexpr float kHelpY = 692.0f, kDividerY = 856.0f, kTextSizeY = 888.0f;
    const float hintH = 56.0f;
    const float helpH = kHelpH;
    const float listH = kHelpY - 8.0f - ImGui::GetCursorPosY();
    char cid[16];
    _snprintf_s(cid, sizeof(cid), _TRUNCATE, "list%d", g.tab);
    // THE SCROLLBAR (2026-11-12): ImGui's own is kept for its behaviour but drawn invisible, 16 px wide; scrollbar_track.png /
    // scrollbar_thumb.png (or code-drawn) are drawn over its place after the child, from the child's scroll position.
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 16.0f);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    PushClear(ImGuiCol_ScrollbarGrab, ImGuiCol_ScrollbarGrabHovered, ImGuiCol_ScrollbarGrabActive);
    float scrollY = 0.0f, scrollMax = 0.0f;
    ImVec2 listPos(0.0f, 0.0f), listSize(0.0f, 0.0f);
    if (ImGui::BeginChild(cid, ImVec2(0.0f, listH), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground)) {
        kTabs[g.tab].draw();
        scrollY = ImGui::GetScrollY();
        scrollMax = ImGui::GetScrollMaxY();
        listPos = ImGui::GetWindowPos();
        listSize = ImGui::GetWindowSize();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    if (scrollMax > 0.0f && listSize.y > 0.0f) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 ta(listPos.x + listSize.x - 16.0f, listPos.y), tb(listPos.x + listSize.x, listPos.y + listSize.y);
        const float trackH = tb.y - ta.y;
        const float thumbH = (std::max)(40.0f, trackH * listSize.y / (listSize.y + scrollMax));
        const float thumbY = ta.y + (trackH - thumbH) * (scrollY / scrollMax);
        const ImVec2 ha(ta.x, thumbY), hb(tb.x, thumbY + thumbH);
        if (const MenuArt* tr = ArtOf(kArtScrollTrack)) {
            ArtNine(dl, tr, ta, tb, 8.0f);
        } else {
            dl->AddRectFilled(ta, tb, IM_COL32(20, 22, 28, 200), 6.0f);
        }
        if (const MenuArt* th = ArtOf(kArtScrollThumb)) {
            ArtNine(dl, th, ha, hb, 8.0f);
        } else {
            dl->AddRectFilled(ImVec2(ha.x + 3.0f, ha.y + 2.0f), ImVec2(hb.x - 3.0f, hb.y - 2.0f), IM_COL32(170, 145, 90, 230), 5.0f);
        }
    }
    g.tabRequest = false;
    HolsterPageAfterTabs(uiStart);   // another tab is up: the holster sphere is hidden (spec goal 10). COST: one compare.

    // THE BOTTOM STRIP (2026-11-12): the divider, then Text size - drawn BEFORE the description box (which is above them) so a
    // focused Text size row's help reaches the box this frame. ImGui's d-pad navigation is by position, not by order.
    ImGui::SetCursorPosY(kDividerY);
    MenuDivider();
    ImGui::SetCursorPosY(kTextSizeY);
    {
        int& ts = OverlayTextSize();
        RowSliderI("Text size", "##overlay_text_size", &ts, 60, 150, "The mod menu's text size. 100% is the original size.", false,
                   "%d%%");
        ArrowStepFocusedSlider(&ts, 60, 150, 5);
        SaveSettingsIfChanged();
    }

    // HELP STRIP: the focused row's help (RowClose), then any status message. Empty while the close-hold bar is up (it is drawn
    // over this box's bottom 56 px).
    ImGui::SetCursorPosY(kHelpY);
    // The whole box in Segoe UI (g_descFont; nullptr = keep the menu font), at the menu's current size (0.0f = keep, so Text size
    // applies). COST: one push / pop per menu frame.
    ImGui::PushFont(g_descFont, 0.0f);
    if (ImGui::BeginChild("help", ImVec2(0.0f, helpH), 0,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse) &&
        g.holdProgress <= 0.0f) {
        if (g.status[0] && Now() < g.statusUntil) {
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", g.status);
        }
        if (g.adjusting) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Adjusting - right stick changes the value. A or B when done.");
        }
        if (g.helpRestart) {
            ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.4f, 1.0f), "Takes effect after a restart.");
            if (g.helpText && g.helpText[0]) { ImGui::SameLine(); }
        }
        if (g.helpText && g.helpText[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.80f, 0.80f, 1.0f));
            ImGui::TextWrapped("%s", g.helpText);
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::PopFont();

    // HINTS (bottom 56 px) - inside the window's bottom padding, so drawn with the clip opened to the whole panel.
    ImGui::GetWindowDrawList()->PushClipRectFullScreen();
    DrawHintStrip(ImVec2(ImGui::GetWindowPos().x + style.WindowPadding.x, H - hintH),
                  W - 2.0f * style.WindowPadding.x);
    ImGui::GetWindowDrawList()->PopClipRect();

    // What owns B on the NEXT frame: an open dropdown, a slider being edited, a Controls capture or its release drain, the
    // overlay-binding capture.
    // The cursor left the slider being adjusted (another tab, a page change, a scroll-out): adjust mode ends with it.
    if (g.adjusting && g.sliderFocused != g.adjustId) {
        g.adjusting = false;
        g.adjustId = 0;
        // [PROBE: MENUVR] gate=none (each slider adjust mode on / off - event-driven)  site=HEADSET  log-only=yes
        LOG("overlay: [MENUVR] slider adjust mode OFF (the cursor left the slider)");
    }
    g.busyPrev = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) || ImGui::IsAnyItemActive() || ControlsBusy() ||
                 g.capturing != 0 || g.adjusting;
    ImGui::End();
    ImGui::PopFont();   // the menu font / text size pushed at the top
}

// Keyboard Left / Right step a FOCUSED slider directly (Jay 2026-10-13: "adjust the values/sliders with the
// keyboards arrow keys, not just the controllers DPAD"). ImGui's own tweak only runs once the slider is ACTIVATED
// (A on the pad, which is what Jay did before the D-pad would move it; Enter on the keyboard opens the slider's
// text field instead, and this panel feeds ImGui no characters, so that field cannot be typed into). Here a slider
// that merely has the nav cursor on it takes the arrows: an int slider steps 1, a float slider 1/100 of its range,
// a held key repeats at ImGui's key-repeat rate. An ACTIVE slider is left to ImGui, so A + D-pad and Space + arrows
// still behave exactly as before. Keyboard only - the D-pad is unchanged.
// The value is written straight to the setting; SaveSettingsIfChanged's compare persists it like any slider edit.
// COST: per drawn slider row, IsItemFocused + IsItemActive; the key checks run only on the one focused row.
// 2026-11-11 (Jay: "RIGHT STICK left/right change value (hold = fast)"): the Touch right stick steps it too - g.vrStep from
// FeedInput, one step per repeat, kVrFastMul steps once held kVrFastAfter. Returns whether the value moved.
// THE STICK NEEDS A FIRST (Jay, headset, 2026-11-11: "can't move the sliders with the joystick though. Should be an A press, then
// I'd be able to adjust them with the joystick"): the right stick moves a slider only while it is ACTIVE - A on it activates it
// (ImGui's gamepad activation, which keeps it active until A again or B; ImGui's own tweak there reads only the d-pad, which
// Touch never sends, so before this nothing moved it). The keyboard arrows keep their old rule (focused, NOT active). A value
// moved while active is marked edited, so a slider that saves on release (IsItemDeactivatedAfterEdit) still saves.
// 2026-11-11 (second headset try, "That hasn't worked either"): the right stick now follows OUR adjust mode (Overlay::adjusting,
// entered by Touch A in FeedInput), not ImGui's activation - see the Overlay struct for why. Every slider registers itself here
// as the focused one (g.sliderFocused), which is how FeedInput knows A landed on a slider.
template <typename T>
bool ArrowStepFocusedSlider(T* v, T lo, T hi, T step)
{
    const bool focused = ImGui::IsItemFocused();
    const bool active = ImGui::IsItemActive();
    const ImGuiID id = ImGui::GetItemID();
    if (focused) { g.sliderFocused = id; }
    int dir = 0;
    if (focused && !active) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) { --dir; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) { ++dir; }
    }
    T mul = static_cast<T>(1);
    if (g.vrStep != 0) {
        if (g.adjusting && g.adjustId == id) {
            dir += g.vrStep;
            mul = static_cast<T>(g.vrStepMul);
        } else if (focused) {
            static bool saidNotActive = false;
            if (!saidNotActive) {
                saidNotActive = true;
                // [PROBE: MENUVR] gate=none (first right-stick step on a slider not yet activated - names the reason)  site=HEADSET  log-only=yes
                LOG("overlay: [MENUVR] right stick on a slider NOT in adjust mode - ignored until A is pressed on it (logged once)");
            }
        }
    }
    if (dir == 0) {
        return false;
    }
    T n = *v + (dir > 0 ? step : -step) * mul;
    if (n < lo) { n = lo; }
    if (n > hi) { n = hi; }
    const bool moved = n != *v;
    *v = n;
    if (moved && active) { ImGui::MarkItemEdited(id); }   // an ImGui-active slider (Xbox pad) still saves on release
    if (moved && g.vrStep != 0 && g.adjusting && g.adjustId == id) {
        static bool saidMoved = false;
        if (!saidMoved) {
            saidMoved = true;
            // [PROBE: MENUVR] gate=none (first slider moved by the right stick in adjust mode)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUVR] right stick FIRST moved a slider in adjust mode (step x%.0f) - logged once",
                static_cast<double>(g.vrStepMul));
        }
    }
    return moved;
}

// One setting's row - checkbox/combo/slider, the raw key (so the overlay's
// label and the file's key are both visible - "Pose refresh" and
// "relocate_on_query" looked unrelated before this), an optional "(restart)"
// note, and its help marker. Factored out so both DrawSettingsTab() and
// DrawControllerBindingsTab() can render a setting identically while
// filtering which ones each tab shows by group.
void DrawOneSetting(const Setting& st, int index)
{
    if (st.locked) { return; }   // LOCKED loadbearing switch (settings.h): never drawn, never editable
    ImGui::PushID(index);

    // ===== LABELS WERE BEING CUT OFF ON THE RIGHT. 2026-09-14. =====
    //
    // Reported from the headset: "some of the names are cutoff on the righthand
    // side", to the point of not being able to tell which setting was which.
    //
    // THE ROW ORDER WAS THE BUG. An ImGui widget draws its label AFTER the
    // control, and this row then appended the KEY after that - so the row ran
    // [control][long label][key][?], and the KEY, which is the thing that
    // actually identifies a setting and the thing you type into rigset.sh, was
    // last in line and first to be pushed off the 1300 px panel. The longest
    // labels simply hid their own keys.
    //
    // So: the control takes a HIDDEN label (##key - still unique per row, which
    // is what ImGui needs the label for), the KEY goes first where it is short
    // and cannot be clipped, and the human label moves to its own WRAPPED line
    // underneath, where its length stops mattering at any panel width.
    //
    // ===== 2026-11-11: READABLE NAME ONLY (Jay, BETA). =====
    // The key is no longer drawn: one line per setting, the label in the name column (wrapped there, so a long label still
    // cannot push the control off the panel) and the control in its own column (RowOpen). The help shows in the strip under
    // the list while the row is focused. A label BETA lean stripped (DLVR_BETA_TEXT keeps only the "VR Comfort" group's text)
    // falls back to the key, so a row is never blank.
    char hidden[160];
    _snprintf_s(hidden, sizeof(hidden), _TRUNCATE, "##%s", st.key);
    const char* name = (st.label && st.label[0]) ? st.label : st.key;
    // Jay 2026-11-14 (HARD; trace docs/traces/20261114_1200_wrist_holds_hud_panel.md): 'HUD on a fixed panel' is held on while the
    // Wrist HUD is ticked or running - its row greyed, a note under it (ovr2xr.cpp SaveSettingsIfChanged keeps the value on).
    // COST: one pointer compare per drawn row.
    const bool held = st.value == &UiHudPanel && ::dlvr::wristquad::HudPanelNeeded();
    if (held) { ImGui::BeginDisabled(); }

    if (st.type == SetType::Bool) {
        RowCheckbox(name, hidden, static_cast<bool*>(st.value), st.help, st.restart);
    } else if (st.type == SetType::Int && st.items) {
        int* v = static_cast<int*>(st.value);
        int cur = (*v < 0) ? 0 : (*v >= st.itemCount ? st.itemCount - 1 : *v);
        if (RowCombo(name, hidden, &cur, st.items, st.itemCount, st.help, st.restart)) {
            *v = cur;
        }
    } else if (st.type == SetType::Int) {
        int* v = static_cast<int*>(st.value);
        RowSliderI(name, hidden, v, static_cast<int>(st.lo), static_cast<int>(st.hi), st.help, st.restart);
        ArrowStepFocusedSlider(v, static_cast<int>(st.lo), static_cast<int>(st.hi), 1);
    } else {
        float* v = static_cast<float*>(st.value);
        RowSliderF(name, hidden, v, st.lo, st.hi, "%.2f", st.help, st.restart);
        ArrowStepFocusedSlider(v, st.lo, st.hi, (st.hi - st.lo) * 0.01f);
    }
    if (held) {
        ImGui::EndDisabled();
        ImGui::PushFont(g_descFont, 0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Needed by the Wrist HUD - stays on while the Wrist HUD is on.");
        ImGui::PopFont();
    }
    ImGui::PopID();
}

// Settings whose group is "Controller Bindings" are rendered in their own
// dedicated tab (DrawControllerBindingsTab) instead of here, so they are not
// shown twice. Same arrangement for the "Aim" group and DrawAimDebugTab - the
// aim toggles belong next to the hook's own install/status controls, not
// buried among the general settings.
constexpr const char* kControllerBindingsGroup = "Controller Bindings";
constexpr const char* kAimGroup = "Aim";
// The VR Comfort tab's settings (comfort_*), SPEC-VR-COMFORT-2026-09-30. Claimed by GROUP, like the two above.
constexpr const char* kComfortGroup = "VR Comfort";
// The Physical Parkour tab's settings (climb_*, diag_climb_probe), 2026-11-27. Claimed by GROUP; ovr2xr.cpp BetaTextKept keeps
// this group's labels and help in the BETA build.
constexpr const char* kParkourGroup = "Physical Parkour";

// ---------------------------------------------------------------------------
// WHICH TAB A SETTING BELONGS TO. 2026-09-13.
//
// THE PROBLEM THIS SOLVES, measured rather than eyeballed: 118 of the 163
// settings shared the single group "Camera hook" - 65 `rig_`, 26 `diag_`, 16
// `cam_`, 5 `lod_`, 3 `vr_look_back*`, 2 `rtti_`, 1 `stream_`. One heading was
// carrying 72% of the menu.
//
// CLASSIFIED BY KEY PREFIX, NOT BY REWRITING THE TABLE. Re-grouping meant
// editing 118 multi-line entries in `kSettings[]`, which is a lot of chances to
// corrupt a table that nothing would validate afterwards. The key is already
// the identity of a setting, it is already shown in the UI beside every label,
// and it is already what `ovr2xr.txt` stores - so the tab is derived from it
// and the settings table is untouched.
//
// >>> THE EXCEPTIONS ARE THE WHOLE POINT, AND THEY ARE LISTED, NOT INFERRED.
// >>> `diag_bone_blend_probe` and `diag_bone_dump` ARE NOT DIAGNOSTICS. <<<
// They are the rig's MASTER ENABLES wearing a `diag_` prefix - verified in
// code, not taken from a document: camhook.cpp's capture thread refuses to run
// without DiagBoneDump (or a stage flag), which is how the model pointer is
// obtained at all, and boneblend.cpp gates the whole blend path on
// DiagBoneBlendProbe. Turning either off silently reverts the arm to animation
// with NO error. A prefix rule would file them under Diagnostic, next to
// switches that are genuinely safe to toggle, which is precisely the mistake
// this project has already paid for by naming them `diag_`.
enum class Tab { Settings, Camera, Dlss, Rigging, Diagnostic, Aim, Bindings, Comfort, Hud, Parkour };

bool KeyHasPrefix(const char* key, const char* prefix)
{
    return strncmp(key, prefix, strlen(prefix)) == 0;
}

Tab TabForSetting(const Setting& st)
{
    // THE HUD TAB (Jay 2026-11-13: "All the HUD items, including everything that sits in 'UI Master Control' in VR Comfort, will be
    // moved to it's own new HUD/UI Tab"): every ui_ key, BEFORE the group test - they all sit in the "VR Comfort" group (which is what
    // keeps their text in BETA lean), and that group would otherwise claim them.
    if (KeyHasPrefix(st.key, "ui_")) { return Tab::Hud; }
    // the in-world prompt's switch keeps its key (on disk; the bootstrap reads it by name) - filed with ui_prompt_* (trace 20261123_1730)
    if (strcmp(st.key, "inworld_prompts") == 0) { return Tab::Hud; }
    // Groups that already own a dedicated tab keep it.
    if (strcmp(st.group, kControllerBindingsGroup) == 0) { return Tab::Bindings; }
    if (strcmp(st.group, kAimGroup) == 0)                { return Tab::Aim; }
    if (strcmp(st.group, kComfortGroup) == 0)            { return Tab::Comfort; }
    if (strcmp(st.group, kParkourGroup) == 0)            { return Tab::Parkour; }   // before the diag_ prefix rule (diag_climb_probe)

    // THE EXPLICIT EXCEPTIONS, before any prefix rule can claim them.
    static const char* const kRigMasterEnables[] = {
        "diag_bone_blend_probe",   // gates the blend path (boneblend.cpp)
        "diag_bone_dump",          // gates the model-pointer capture (camhook.cpp)
        // ROOT-3, added 2026-10-01 (Jay: loadbearing, treat it as such). Its 2 s report pass is the only
        // path to ResolveAccessors() - off = no arm, silently. It was filed under Diagnostic by the prefix rule.
        "diag_bone_blend_verbose",
        // NOT an enable - the master set's FORCED-OFF member (D-11: on, it made a chimeric arm chain, [AIM R]
        // 53.9 deg vs 1.5). Filed to Rigging 2026-10-01 (Jay, option A) so it is drawn ONLY in the locked master
        // list: as an unlocked Diagnostic checkbox it could re-create the fault and make "Enable rigging" read OFF.
        "diag_trs_skip_test",
    };
    for (const char* k : kRigMasterEnables) {
        if (strcmp(st.key, k) == 0) { return Tab::Rigging; }
    }

    // A `dlss_` key that is a LIVE layer setting, not a marker: the DLSS tab builds its own UI and never calls
    // DrawSettingsForTab(Tab::Dlss), so the prefix rule below would file it where nothing draws it (INV-456 - Jay:
    // "framedelta isnt there"). It stays under the Settings tab's "Camera hook" heading with the other AFW tests.

    // The carried arrow's slide-back is a rig fix (memory fixes-on-rigging-tab; spec 2026-10-22-hand-carried-model-design 5).
    if (strcmp(st.key, "bow_nock_carry_back_m") == 0 || strcmp(st.key, "bow_nock_carry_tilt_deg") == 0
        || strcmp(st.key, "bow_nock_carry_turn_deg") == 0 || strcmp(st.key, "bow_nock_carry_side_m") == 0) {
        return Tab::Rigging;
    }
    // The blade-length hit line's margin is a rig fix (spec 2026-10-26-blade-length-hit-line §4.4; memory fixes-on-rigging-tab).
    if (strcmp(st.key, "melee_hit_margin_m") == 0) { return Tab::Rigging; }
    if (strcmp(st.key, "melee_min_reach_m") == 0) { return Tab::Rigging; }   // revision 2, spec §7.5
    if (strcmp(st.key, "melee_hit_thickness_m") == 0) { return Tab::Rigging; }   // the hit capsule, spec §9.2
    if (strcmp(st.key, "melee_hit_back_m") == 0) { return Tab::Rigging; }
    if (strcmp(st.key, "melee_hit_factor_restore") == 0) { return Tab::Rigging; }   // spec §9.3
    if (KeyHasPrefix(st.key, "rig_"))  { return Tab::Rigging; }
    if (KeyHasPrefix(st.key, "diag_")) { return Tab::Diagnostic; }
    if (KeyHasPrefix(st.key, "rtti_")) { return Tab::Diagnostic; }
    if (KeyHasPrefix(st.key, "dlss_")) { return Tab::Dlss; }
    if (KeyHasPrefix(st.key, "cam_"))  { return Tab::Camera; }
    if (KeyHasPrefix(st.key, "vr_look_back")) { return Tab::Camera; }

    // `lod_*` and `stream_priority_distance` are rendering, not camera, and no
    // tab was asked for. They stay in Settings rather than being filed
    // somewhere plausible-looking - an unasked-for guess in a menu is how a
    // setting becomes unfindable.
    return Tab::Settings;
}

// ---------------------------------------------------------------------------
// SETTLED SETTINGS - folded into ONE collapsed node per tab. 2026-10-01.
//
// Jay: the menu had grown to where finding the toggle under a live A/B was
// tedious. Every setting nobody has had to touch for a while is drawn inside a
// collapsed "Settled - not changed recently" node in its own tab; the ones being
// worked on stay on top. FOLDING CHANGES WHERE A SETTING IS DRAWN - NEVER ITS
// VALUE, ITS DEFAULT, OR WHAT IT DOES.
//
// THE LIST NAMES THE SETTLED KEYS, NOT THE LIVE ONES, ON PURPOSE: a setting added
// later is absent from it and therefore lands in the live section by default. A
// new A/B toggle must never be born hidden.
//
// CHOSEN MECHANICALLY, NOT BY EYE. Each key's last-touched time = the later of its
// first appearance and its last value change, over every launch's `config:` dump
// (187 sessions) and every ovr2xr.txt snapshot (219) in backups/, 2026-08-31 to
// 2026-10-01. Settled = last touched before 2026-09-27 (Jay, 2026-10-01). Agreed
// exceptions: rig_left_elbow_follow_hand and rig_elbow_pole_max_up stay LIVE (the
// open right-elbow item). show_reticle is drawn by
// hand on the Aim tab and is deliberately absent. LIMIT: the overlay does not log
// edits, so a toggle flipped and flipped back inside one session left no trace.
//
// A name here that is not a registered setting matches nothing - so the first draw
// logs every such name and the matched count (SettledFlags below).
const char* const kSettledKeys[] = {
    // ---- Settings tab (52, counted 2026-11-15 after removal batch 10; the old "77" was already stale at 70) ----
    "render_scale", "max_dim", "fov_source", "symmetric_fov", "fov_match_aspect", "fov_center_correct",
    "vertical_extend", "auto_pitch", "pitch_offset_deg", "eye_offset_space", "auto_eye_assign", "swap_eyes",
    "invert_eye_offset", "use_render_pose", "relocate_on_query", /* "mirror": VR Comfort, live, 2026-11-14 */ "debug_single_eye", "horizon_lock",
    "block_pad_pitch", "block_mouse_pitch", "vr_trigger_fire",
    "anim_suppress_balance", "anim_suppress_antiwall",
    "anim_suppress_smoothpos",
    /* the 18 item_pose_* / item_basis_* / item_local_test_* keys: retired with the item-pose fix, removal batch 10 */
    "melee_pose_wave", "melee_pose_offset_x", "melee_pose_offset_y",
    "melee_pose_offset_z", "melee_pose_drive", "melee_pose_hand", "melee_pose_max_age_ms", "melee_pose_max_jump",
    "melee_force_elem", "melee_census_ms",
    "locread_window_ms", "melee_hook_inline", "melee_force_part", "bone_dump_attempts",
    "item_place_from_controller", "item_place_basis",
    "item_place_orphans", "item_place_rel", "lod_override", "lod_objects_range", "lod_mesh_cull_size",
    "lod_mesh_cull_dist", "lod_fx_range", "stream_priority_distance", "overlay_enabled", "overlay_key",
    "overlay_pad", "overlay_distance", "overlay_width",
    // ---- Camera tab (20; cam_transpose / cam_order retired 2026-11-14 with camscan) ----
    "cam_up_axis", "cam_hook_op", "cam_keep_head", "vr_look_back",
    "vr_look_back_ease_ms", "vr_look_back_angle_deg", "cam_yaw_smoothing_ms", "cam_head_pitch_axis",
    "cam_head_pitch_flip", "cam_head_roll_axis", "cam_head_roll_flip", "cam_auto", "cam_derive",
    "cam_write_combined", "cam_combined_pre", "cam_write_view", "cam_matrix_transposed", "cam_delta_deg",
    "cam_delta_axis", "cam_delta_world",
    // ---- Rigging tab (107; was 113 before the six hand-angle dials moved to VR Comfort 2026-11-16) ----
    "rig_wrist", "rig_yaw_camera_blend", "rig_shoulder_from_eye", "rig_yaw_camera_smooth", "rig_forearm_twist",
    // 2026-11-16: the six hand-angle dials moved to VR Comfort > Hands & Arms (trace 20261116_1750_grip_offset_sliders) - not listed here.
    "rig_wrist_flex_deg", "rig_wrist_ext_deg", "rig_wrist_ulnar_deg", "rig_wrist_radial_deg", "rig_left_arm",
    "rig_left_mirror_deviation", "rig_left_wrist_swap_axes", "rig_wrist_swap_axes",
    "rig_hand_len", "rig_twist_to_twistbone", "rig_write_at_player_tail", "rig_block_arm_bones",
    "rig_block_keep_visible", "rig_grip_use_baked", "rig_grip_compose_rest", "rig_hand_grip_pose",
    "rig_regrip_button", "rig_suppress_stretch", "rig_shoulder_freeze_dir", "rig_stable_shoulder",
    "rig_anchor_from_spine", "rig_hh_rot_r_pitch_deg", "rig_hh_rot_r_yaw_deg",
    "rig_hh_rot_r_roll_deg", "rig_hh_rot_l_pitch_deg", "rig_hh_rot_l_yaw_deg", "rig_hh_rot_l_roll_deg",
    "rig_hh_off_r_x", "rig_hh_off_r_y", "rig_hh_off_r_z", "rig_hh_off_l_x", "rig_hh_off_l_y", "rig_hh_off_l_z",
    "rig_weapon_pos_slew_max", "rig_drive_handholders", "rig_wristback_from_asset", "rig_grip_per_item",
    "rig_anchor_max_m", "rig_reach_soft",
    "rig_final_gate_hooks", "rig_write_at_final_gate", "rig_left_flag_handshake", "rig_restore_left_len",
    "rig_restore_e48_len", "rig_reach_scale",
    "rig_clavicle_protract_deg", "rig_shoulder_forward", "rig_upper_arm_len", "rig_forearm_len",
    "rig_clavicle_share", "rig_arm_ik", "rig_arm_inject_yaw", "rig_arm_axis_remap", "rig_arm_element",
    "rig_write_seconds", "rig_arm_follow_controller", "rig_write_at_owner", "rig_write_test",
    "diag_bone_blend_probe", "rig_e48_flag_handshake", "rig_world_watch_local", "rig_world_watch_elem",
    "rig_write_at_owner_exit", "rig_solve_at_tail", "rig_world_refresh",
    "rig_distal_bit1_poke", "rig_distal_flag_handshake", "rig_subtree_rest", "rig_subtree_fingers",
    "rig_subtree_grip", "rig_subtree_grip_delay_ms", "rig_subtree_fingers_held_release", "rig_pin_clavicle",
    "rig_parent_guard", "rig_parent_guard_basis", "rig_parent_guard_trans_m", "rig_parent_guard_max_hold",
    "rig_subtree_grip_canon", "rig_yaw_slew_max", "rig_yaw_teleport_deg", "rig_clavburst_basis",
    "rig_clavburst_offset_m", "rig_anim_gate", "rig_exec_client2", "rig_rest_from_asset",
    "rig_shoulder_rest_from_asset", "rig_hh_flag_handshake", "diag_bone_dump", "rig_stage2_use_world",
    "rig_notify_animbind", "rig_notify_weight", "rig_stage2_angle_deg",
    "diag_bone_blend_verbose",   // ROOT-3, moved from the Diagnostic tab 2026-10-01 (a rig master enable)
    "diag_trs_skip_test",        // the master set's forced-off member, moved from Diagnostic 2026-10-01 (option A)
    // ---- Diagnostic tab (79) ----
    "diag_eye_pose_compare", "diag_player_movemod", "diag_kill_physics_debug", "diag_feed_sim_camera",
    "diag_eye_rotation_compare", "diag_watch_line_add", "diag_position_mapping", "diag_yaw_noise",
    "diag_yaw_strafe", "diag_strafe", "diag_segment_len", "diag_body_yaw", "diag_anchor_ab",
    /* diag_item_xform / _holder / _parity / _dirty + diag_basis_delta: retired with the item-pose fix, removal batch 10 */
    "diag_fastik_globals",
    "diag_melee_pose_probe", "diag_melee_record_probe", "diag_melee_elem_probe", "diag_melee_bone_census",
    "diag_lasthitloc_readwatch", "diag_damage_watch_rva", "diag_damage_watch",
    "diag_dirty_flags", "diag_deform_index", "diag_deform_probe", "diag_shoulder_chain", "diag_item_scale",
    "diag_arm_compose", "diag_buffer_cmp", "diag_render_sync_watch", "diag_resolver_store", "diag_pole_watch",
    "diag_stale_clav", "diag_drawn_census", "diag_root_stability", "diag_pose_miss", "diag_snap_dump",
    "diag_snapdump_rad", "diag_snapdump_max", "diag_snapdump_per_side", "diag_rig_counters", "diag_regime_move_mps",
    "diag_hier_sync", "diag_frozen_local", "diag_rest_read", "diag_item_place", "diag_right_thumb_capture",
    "diag_hh_compose", "diag_desc_parent", "diag_thread_group", "diag_fresh_anchor", "diag_rel_step",
    "diag_root_track", "diag_orphan_rel", "diag_item_elem_dump", "diag_fastik_weight", "diag_flat_ref",
    "diag_rel_churn", "diag_elem_dump", "diag_holder_id", "diag_item_model", "diag_anim_map", "diag_anim_layers",
    "diag_spine_chain", "diag_world_writer", "diag_antiwall_probe",
    "diag_camhook_verbose", "diag_rig_stage4", "diag_rig_stage3", "diag_rig_stage2", "diag_rig_stage1",
    "diag_visibility_probe", "rtti_only_real", "rtti_sort_activity",
    // ---- Aim debugging tab (6) ----
    "aim_hook_auto_observe", "vr_aim_redirect", "aim_absolute", "aim_mirror_yaw", "aim_head_smoothing",
    "aim_head_smoothing_ms",
    // ---- Controller Bindings tab (10) ----
    "vr_sprint", "vr_decouple_move", "vr_decouple_max_deg", "vr_move", "vr_turn", "vr_face_buttons",
    "vr_stick_click_right", "vr_left_trigger", "vr_grip", "vr_menu_button",
};
constexpr int kSettledKeyCount = static_cast<int>(sizeof(kSettledKeys) / sizeof(kSettledKeys[0]));

// One flag per registry index, computed ONCE at the first draw. The menu redraws
// every frame while it is open, and 416 settings x 307 names of strcmp per frame is
// not a cost worth paying for a fixed answer.
// [PROBE: MENU] gate=none (the first draw of the overlay)  site=HEADSET  log-only=yes
//   traced: SettledFlags <- DrawLiveThenSettled <- the tab draws <- DrawUi (overlay.cpp) <- OverlayRender
//   <- ovr_SubmitFrame (ovr2xr.cpp, `OverlayRender(s->viewSpace, s->appSpace, ...)`) - it needs a live OpenXR
//   session, so it never runs flat. It logs the first time the menu is OPENED, not at launch.
const bool* SettledFlags()
{
    static std::vector<char> flags;   // char, not bool: vector<bool> has no data()
    static bool built = false;
    if (!built) {
        built = true;
        int count = 0;
        const Setting* all = AllSettings(count);
        flags.assign(static_cast<size_t>(count), 0);
        int matched = 0, stale = 0;
        for (int k = 0; k < kSettledKeyCount; ++k) {
            bool found = false;
            for (int i = 0; i < count; ++i) {
                if (strcmp(all[i].key, kSettledKeys[k]) == 0) {
                    if (!flags[static_cast<size_t>(i)]) { ++matched; }
                    flags[static_cast<size_t>(i)] = 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                ++stale;
                LOG("overlay: [MENU] settled list names '%s' - NOT a registered setting, so it "
                    "matches nothing (a stale or misspelt entry in kSettledKeys)", kSettledKeys[k]);
            }
        }
        // Logged unconditionally, once: the negative path (a stale list) must SAY so.
        LOG("overlay: [MENU] settled list: %d name(s), %d matched a registered setting, %d stale | "
            "%d of %d settings fold into each tab's 'Settled - not changed recently' node",
            kSettledKeyCount, matched, stale, matched, count);
    }
    return reinterpret_cast<const bool*>(flags.data());
}

// Draws the settings `include` selects: the live ones first (under their group
// strings as sub-headings when `headings`), then every settled one inside ONE
// collapsed node. Returns how many settings the selection HOLDS - live and
// settled - so a caller's "nothing here" test is not fooled by a closed node.
template <typename Pred>
int DrawLiveThenSettled(const char* nodeId, bool headings, Pred include)
{
    int count = 0;
    const Setting* all = AllSettings(count);
    const bool* settled = SettledFlags();
    int live = 0, folded = 0;
    const char* group = nullptr;
    for (int i = 0; i < count; ++i) {
        const Setting& st = all[i];
        if (st.locked || !include(st)) { continue; }   // locked: not drawn, not counted
        if (settled[i]) { ++folded; continue; }
        if (headings && (!group || strcmp(group, st.group) != 0)) {
            group = st.group;
            MenuHeading(group);   // was ImGui::SeparatorText (2026-11-12, the BioShock-style layout)
        }
        DrawOneSetting(st, i);
        ++live;
    }
    if (folded > 0) {
        char label[96], sid[48];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "Settled - not changed recently (%d)", folded);
        _snprintf_s(sid, sizeof(sid), _TRUNCATE, "settled_%s", nodeId);
        ImGui::Spacing();
        if (MenuSection(label, sid, true)) {   // closed by default
            group = nullptr;
            for (int i = 0; i < count; ++i) {
                const Setting& st = all[i];
                if (!include(st) || !settled[i]) { continue; }
                if (headings && (!group || strcmp(group, st.group) != 0)) {
                    group = st.group;
                    MenuHeading(group);   // was ImGui::SeparatorText (2026-11-12)
                }
                DrawOneSetting(st, i);
            }
            ImGui::TreePop();
        }
    }
    return live + folded;
}

// The VR Comfort tab (SPEC-VR-COMFORT-2026-09-30). Everything in it is live; the one thing a setting cannot show is
// whether snap turning is actually running, so that is stated here rather than left to the log.
// ------------------------------------------------------------ Controls tab ---
// User rebinding (plan docs/superpowers/plans/2026-10-28-controls-rebind.md Task 6; spec 2026-10-28-controls-rebind-design).
// Edits dlvr\binds.txt only. The bootstrap reads it at the NEXT launch and writes DW\dlvr_binds.scr, which our Data3
// inputs_pad.scr uses for the game's base pad binds (pakgen.cpp WriteBindsScr) - so every change says "restart to apply".
// Jay's rules: two slots per action; select a slot, press the input, it saves; Backspace = unbound; reset per action and for
// the whole tab; the mod's contextual uses of a button (grip, holsters, two-hand, loot, bow ...) keep priority - the layer
// withholds the pad bit for those, so a base bind there does not fire while they own it.
struct ControlsTab {
    bool loaded = false;
    binds::Binds cur{};
    std::string atLaunch;        // Serialize() of binds.txt as this launch read it (what dlvr_binds.scr was written from)
    int rejectedAtLoad = 0;
    int capAction = -1;          // the slot capturing a press, or -1
    int capSlot = 0;
    bindcapture::State capState;
    bool backPrev = false;
    bool drain = false;          // capture ended: hold the menu's pad input until everything is released
    double lastDraw = 0.0;
    bool confirmReset = false;
    // Holster place-by-hand (plan 2026-10-30 Task 5): 0 = the bindings (the tab as it always was), 1 = the Holsters page.
    int page = 0;
    int placeZone = -1;     // the row whose Place was pressed last (names the partner in the Overlaps line)
    int placeResult = -1;   // the last HolsterPlaceTick result (HolsterPlaceResult), -1 = none to show
};
ControlsTab g_ct;

// Holster place-by-hand (plan 2026-10-30 Task 5, ruling R2): HolsterPlaceTick runs from OverlayPoll (the page is not drawn
// while placing), so its result is kept here for the Holsters page to show when the panel is drawn again.
void ControlsNotePlaceResult(int result)
{
    if (result >= 0) { g_ct.placeResult = result; }
}

// The panel is closing or being kept shut (the overlay switched off, the calibration screen up, its own button): a placement
// still running ends as a cancel (the old spot is kept) and the holster sphere is hidden (spec goal 10).
// COST: one volatile read + one int store - also on the OverlayPoll paths that run every frame while the overlay is switched
// off or the calibration screen is up.
// WRIST HUD 3b - place by hand (trace docs/traces/20261113_1108_wrist_3b_place.md): the HUD tab's last result line, and which piece.
int g_wristPlaceResult = -1;   // wristquad::PlaceResult, -1 = none to show
int g_wristPlacePiece = -1;

// A holster OR a wrist piece is being placed: the panel hides, g.visible stays true (every input block holds), the menu press cancels.
bool AnyPlacing() { return HolsterPlacing() || ::dlvr::wristquad::Placing(); }

void HolsterPanelClosing()
{
    if (HolsterPlacing()) { ControlsNotePlaceResult(HolsterPlaceTick(true)); }
    if (::dlvr::wristquad::Placing()) { g_wristPlaceResult = ::dlvr::wristquad::PlaceTick(true); }   // 3b: ends as a cancel
    g_holsterShowZone = -1;
}

// After the tab bar (DrawUi): the Controls tab stamps g_ct.lastDraw with Now() on every draw - the same pattern that drops a
// capture left running on another tab. Not drawn this panel frame = another tab is up = the sphere is hidden (spec goal 10).
// COST: one int test + one double compare per panel frame.
void HolsterPageAfterTabs(double uiStart)
{
    if (g_holsterShowZone >= 0 && g_ct.lastDraw < uiStart) { g_holsterShowZone = -1; }
}

bool ControlsHoldPad(bool padIdle)
{
    if (g_ct.capAction >= 0) { return true; }
    if (g_ct.drain && padIdle) { g_ct.drain = false; }
    return g_ct.drain;
}

bool ControlsBusy()
{
    return g_ct.capAction >= 0 || g_ct.drain;
}

// B on the Holsters page = its "<- Back" button (only while the Controls tab is the one up).
bool ControlsBackOut()
{
    if (g.tab < 0 || g.tab >= kTabCount || kTabs[g.tab].draw != DrawControlsTab || g_ct.page != 1) { return false; }
    g_ct.page = 0;
    g_ct.placeResult = -1;
    g_holsterShowZone = -1;   // leaving the page hides the sphere (spec goal 10)
    return true;
}

bindcapture::Ctl ControlsReadCtl()
{
    bindcapture::Ctl c;
    GetControllerSqueeze(kLeftHand, c.lgrip);
    GetControllerSqueeze(kRightHand, c.rgrip);
    GetControllerTrigger(kLeftHand, c.lt);
    GetControllerTrigger(kRightHand, c.rt);
    GetControllerStick(kLeftHand, c.lx, c.ly);
    GetControllerStick(kRightHand, c.rx, c.ry);
    GetControllerStickClick(kLeftHand, c.l3);
    GetControllerStickClick(kRightHand, c.r3);
    GetControllerButton(kButtonA, c.a);
    GetControllerButton(kButtonB, c.b);
    GetControllerButton(kButtonX, c.x);
    GetControllerButton(kButtonY, c.y);
    GetControllerMenuButton(c.menu);
    return c;
}

void ControlsLoad()
{
    g_ct.loaded = true;
    g_ct.cur = binds::Defaults();
    char p[MAX_PATH] = {};
    std::string text;
    if (DlvrFilePath("binds.txt", p, sizeof(p))) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") == 0 && f) {
            char rb[4096];
            size_t n = 0;
            while ((n = fread(rb, 1, sizeof(rb), f)) > 0) { text.append(rb, n); }
            fclose(f);
        }
    }
    std::string report;
    g_ct.rejectedAtLoad = binds::Parse(text.c_str(), &g_ct.cur, &report);
    g_ct.atLaunch = binds::Serialize(g_ct.cur);
}

void ControlsSave()
{
    char p[MAX_PATH] = {}, tmp[MAX_PATH] = {};
    if (!DlvrFilePath("binds.txt", p, sizeof(p))) { SetStatus("could not resolve dlvr\\ path - controls NOT saved"); return; }
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", p);
    const std::string body = "# Dying Light VR - your controls (the Controls tab writes this file). Only changed actions are listed;\n"
                             "# applied the next time the game starts. Delete the file to go back to the default controls.\n" +
                             binds::Serialize(g_ct.cur);
    FILE* f = nullptr;
    bool ok = fopen_s(&f, tmp, "wb") == 0 && f;
    if (ok) {
        ok = fwrite(body.data(), 1, body.size(), f) == body.size();
        fclose(f);
    }
    ok = ok && MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) { DeleteFileA(tmp); }
    SetStatus(ok ? "controls saved - restart the game to apply" : "could not write dlvr\\binds.txt - controls NOT saved");
    const std::string changed = binds::Serialize(g_ct.cur);
    // [PROBE: CONTROLS] gate=none (on each Controls tab change)  site=HEADSET  log-only=yes
    LOG("overlay: [CONTROLS] binds.txt %s - %d action(s) differ from default; differs from this launch's: %s", ok ? "saved" : "NOT SAVED",
        static_cast<int>(std::count(changed.begin(), changed.end(), '\n')), changed != g_ct.atLaunch ? "yes (restart to apply)" : "no");
}

// The Holsters page (plan 2026-10-30-holster-place-by-hand Task 5; spec goals 2.1, 10, 12): place each holster by hand, put it
// back to its default, size it. Every text the player reads is this page's own literal (BETA lean strips settings-table text).
// Zones: 0 right shoulder, 1 right hip, 2 left hip, 3 head - holster.h's place-by-hand numbering.
// g_holsterShowZone = the row last used (Place, Default, its Radius slider) - only that sphere shows.
// COST: only while this page is drawn (panel open, Controls tab, page 1): per row one HolsterZoneParams + one HolsterRadiusMax
// (up to three HolsterHipCentre calls a row, each one shared lock + ArmFitRestHand), two HolsterSameHandOverlap and one
// SaveSettingsIfChanged compare. Nothing at all while the page is not drawn.
void DrawHolstersPage()
{
    if (MenuButton("<- Back")) {
        g_ct.page = 0;
        g_ct.placeResult = -1;
        g_holsterShowZone = -1;   // leaving the page hides the sphere (spec goal 10)
        return;
    }
    ImGui::Text("Holsters");
    // The page's description in Segoe UI (Jay 2026-11-12: "Text under holsters (description) needs to be changed to segoe aswell").
    ImGui::PushFont(g_descFont, 0.0f);
    ImGui::TextWrapped("Press Place, hold the hand that uses the holster where you want it, then pull that hand's trigger. "
                       "Menu button cancels.");
    ImGui::PopFont();

    // The last placement's result. OverlayPoll ran it (ruling R2); this frame is the first the panel is drawn after it.
    // Same-hand zones a placement can collide with (the left hand has three since 2026-11-01).
    static const char* const kPartner[5] = { "right hip", "right shoulder", "left head or left chest", "left hip or left chest",
                                             "left hip or left head" };
    const ImVec4 ok(0.5f, 1.0f, 0.5f, 1.0f), warn(1.0f, 0.75f, 0.3f, 1.0f), red(1.0f, 0.4f, 0.35f, 1.0f);
    // The placement result in Segoe UI too (Jay 2026-11-12: "Yes" - the result + overlap messages, as the description).
    ImGui::PushFont(g_descFont, 0.0f);
    switch (g_ct.placeResult) {
        case kPlaceStored:     ImGui::TextColored(ok, "Placed."); break;
        case kPlaceCancelled:  ImGui::TextColored(warn, "Cancelled - the old spot is kept."); break;
        case kPlaceUntracked:  ImGui::TextColored(warn, "Controller not tracked - try again."); break;
        case kPlaceNotRunning:
            ImGui::TextColored(warn, "Holsters are not running right now - check that the holsters and grips are on, then try again.");
            break;
        case kPlaceOutOfRange: ImGui::TextColored(warn, "Too far from the headset for this holster - try a closer spot."); break;
        case kPlaceOverlaps:
            ImGui::TextColored(warn, "Too close to the %s holster - move further away or make a radius smaller.",
                               (g_ct.placeZone >= 0 && g_ct.placeZone < 5) ? kPartner[g_ct.placeZone] : "other");
            break;
        default: break;
    }
    ImGui::PopFont();
    MenuDivider();

    // Placement zones 0..4 (holster_place_logic.h); the left chest = heal since spec 2026-11-01 goal 2, the left hip = the ammo pouch.
    static const char* const kRow[5] = { "Right shoulder (weapon)", "Right hip (item swap, R2 = throwable)",
                                         "Left hip (ammo pouch)", "Left head (torch)", "Left chest (heal)" };
    float* const kRadius[5] = { &HolsterRadius, &HolsterRHipRadius, &HolsterHipRadius, &HolsterHeadRadius, &HolsterLChestRadius };
    constexpr float kRadiusMin = 0.03f;   // the radius rows' own minimum (spec 9a)
    for (int z = 0; z < 5; ++z) {
        ImGui::PushID(z);
        float c[3] = {};
        float r = 0.0f;
        bool on = false;
        HolsterZoneParams(z, c, &r, &on);
        // 2026-11-12, the BioShock-style layout: the zone's name as a heading, its Place / Default as menu buttons, its radius as
        // a menu row (bar + value column) - same actions, same slider range and step as before.
        char head[96];
        _snprintf_s(head, sizeof(head), _TRUNCATE, "%s%s", kRow[z], on ? "" : "  (off)");
        MenuHeading(head);
        if (MenuButton("Place", ImVec2(200.0f, 48.0f))) {
            g_ct.placeZone = z;
            g_ct.placeResult = -1;
            g_holsterShowZone = z;
            HolsterPlaceBegin(z);   // the panel quad hides from the next submit (OverlayRender); OverlayPoll ticks it
        }
        ImGui::SameLine(0.0f, 20.0f);
        if (MenuButton("Default", ImVec2(200.0f, 48.0f))) {
            HolsterPlaceDefault(z);
            g_ct.placeResult = -1;
            g_holsterShowZone = z;
        }
        // Spec goal 12: the cap is the largest radius that does not overlap the same-hand partner, recomputed every frame. A
        // value already above it is left alone (never clamped silently) - the overlap warning below says so.
        float hi = HolsterRadiusMax(z);
        if (hi < kRadiusMin) { hi = kRadiusMin; }
        float* const rv = kRadius[z];
        const float before = *rv;
        RowSliderF("Radius", "##radius", rv, kRadiusMin, hi, "%.2f m");
        ArrowStepFocusedSlider(rv, kRadiusMin, hi, (hi - kRadiusMin) / 100.0f);
        if (ImGui::IsItemActive() || *rv != before) { g_holsterShowZone = z; }
        ImGui::PopID();
        ImGui::Spacing();
    }

    ImGui::PushFont(g_descFont, 0.0f);   // the overlap warnings in Segoe UI (Jay 2026-11-12: "Yes")
    if (HolsterSameHandOverlap(0)) {
        ImGui::TextColored(red, "Right shoulder and right hip overlap - both are off until one is moved.");
    }
    if (HolsterSameHandOverlap(1)) {
        ImGui::TextColored(red, "Two of the left hip, left chest and left head overlap - every left zone is off until one is moved.");
    }
    ImGui::PopFont();
    // A stored spot, a Default or a radius drag persists here: a placement ends in OverlayPoll BEFORE this frame's DrawUi, so
    // the first panel frame after it saves it (Task 4's carried minor).
    SaveSettingsIfChanged();
}

void DrawControlsTab()
{
    if (!g_ct.loaded) { ControlsLoad(); }
    const double now = Now();
    // A capture left running while the panel was closed or another tab was up is dropped, never resumed silently.
    if (g_ct.capAction >= 0 && now - g_ct.lastDraw > 0.5) {
        g_ct.capAction = -1;
        g_ct.drain = true;
    }
    g_ct.lastDraw = now;
    const bindcapture::Ctl ctl = ControlsReadCtl();
    const bool backDown = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
    const bool backEdge = backDown && !g_ct.backPrev;
    g_ct.backPrev = backDown;
    if (g_ct.capAction >= 0) {
        binds::Slot s = g_ct.cur.s[g_ct.capAction][g_ct.capSlot];
        const bindcapture::Result r = bindcapture::Poll(&g_ct.capState, g_ct.capAction, ctl, backEdge, &s);
        if (r != bindcapture::Result::Waiting) {
            if (r == bindcapture::Result::Captured) {
                binds::Slot (&row)[2] = g_ct.cur.s[g_ct.capAction];
                row[g_ct.capSlot] = s;
                if (row[0].bound && binds::Equal(row[0], row[1])) { row[g_ct.capSlot == 0 ? 1 : 0].bound = false; }  // stored once
                ControlsSave();
            } else {
                SetStatus("rebind cancelled");
            }
            g_ct.capAction = -1;
            g_ct.drain = true;
        }
    }

    // Holster place-by-hand (plan 2026-10-30 Task 5, Jay: "B, its own page with a back button"). The bindings below are
    // unchanged; the button is off while a slot captures a press.
    if (g_ct.page == 1) {
        DrawHolstersPage();
        return;
    }
    ImGui::BeginDisabled(g_ct.capAction >= 0);
    // 400 x 64 = the top button row's size (Jay 2026-11-14: "NEED LARGER BUTTON SIZING FOR THE "HOLSTER" BUTTON"; trace 20261114_1215).
    if (MenuButton("Holsters ->", ImVec2(400.0f, 64.0f))) { g_ct.page = 1; }
    ImGui::EndDisabled();

    // The Controls tab's description + its two notes in Segoe UI (Jay 2026-11-12, screenshot VirtualDesktop.Android-20261112-125104:
    // "This was the text I was referring to" - the paragraph under the Holsters -> button).
    ImGui::PushFont(g_descFont, 0.0f);
    ImGui::TextWrapped("Select a slot, then press the controller input you want for that action. Backspace (keyboard) leaves the "
                       "slot unbound; the Menu button cancels. Two actions on one input both fire. The mod's own uses of a button "
                       "(grips, holsters, two-handed hold, looting, bow) keep priority over these. Stick directions are new and not "
                       "yet confirmed for every action.");
    const bool pending = binds::Serialize(g_ct.cur) != g_ct.atLaunch;
    if (pending) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Restart the game to apply your changes.");
    }
    if (g_ct.rejectedAtLoad) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%d line(s) in dlvr\\binds.txt were not understood and were ignored.",
                           g_ct.rejectedAtLoad);
    }
    ImGui::PopFont();
    if (!g_ct.confirmReset) {
        if (MenuButton("Reset all controls to default")) { g_ct.confirmReset = true; }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Reset every action to its default?");
        ImGui::SameLine();
        if (MenuButton("Yes, reset all")) {
            g_ct.cur = binds::Defaults();
            g_ct.confirmReset = false;
            ControlsSave();
        }
        ImGui::SameLine();
        if (MenuButton("No")) { g_ct.confirmReset = false; }
    }
    MenuDivider();

    static const char* const kCategories[] = { "On foot", "Movement & look", "HUD & selection wheels", "Menus", "Quick-time events",
                                               "Buggy", "Grapple aiming", "Lockpicking", "Be the Zombie" };
    const binds::Binds def = binds::Defaults();
    int listed = 0;
    for (const char* cat : kCategories) {
        if (!MenuSection(cat, cat, false)) {
            for (int a = 0; a < binds::gen::kActionCount; ++a) {
                if (strcmp(binds::gen::kActions[a].category, cat) == 0) { ++listed; }
            }
            continue;
        }
        // No row stripes or column lines (2026-11-12, the BioShock-style layout): the slots are menu buttons (MenuButton).
        if (!ImGui::BeginTable(cat, 4, ImGuiTableFlags_None)) { continue; }
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Slot 1", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Slot 2", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 140.0f);   // was 90: "Default" now fills a framed button
        for (int a = 0; a < binds::gen::kActionCount; ++a) {
            if (strcmp(binds::gen::kActions[a].category, cat) != 0) { continue; }
            ++listed;
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(binds::gen::kActions[a].label);
            for (int s = 0; s < 2; ++s) {
                ImGui::TableSetColumnIndex(1 + s);
                char name[64];
                if (g_ct.capAction == a && g_ct.capSlot == s) {
                    _snprintf_s(name, sizeof(name), _TRUNCATE, "Press an input...");
                } else {
                    bindcapture::SlotName(g_ct.cur.s[a][s], name, sizeof(name));
                }
                ImGui::PushID(s);
                if (MenuButton(name, ImVec2(-FLT_MIN, 0.0f)) && g_ct.capAction < 0) {
                    g_ct.capAction = a;
                    g_ct.capSlot = s;
                    bindcapture::Begin(&g_ct.capState, ctl);
                }
                ImGui::PopID();
            }
            ImGui::TableSetColumnIndex(3);
            const bool isDefault = binds::Equal(g_ct.cur.s[a][0], def.s[a][0]) && binds::Equal(g_ct.cur.s[a][1], def.s[a][1]);
            ImGui::BeginDisabled(isDefault || g_ct.capAction >= 0);
            if (MenuButton("Default", ImVec2(-FLT_MIN, 0.0f))) {
                g_ct.cur.s[a][0] = def.s[a][0];
                g_ct.cur.s[a][1] = def.s[a][1];
                ControlsSave();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    int expected = 0;
    for (int a = 0; a < binds::gen::kActionCount; ++a) {
        if (!binds::Hidden(a)) { ++expected; }
    }
    if (listed != expected) {   // a category this list does not draw (binds_template.py CATEGORIES refuses that at build time)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%d of %d actions are not shown here (a build problem).", expected - listed,
                           expected);
    }
}

void DrawComfortTab()
{
    // The menu-panel settings (every key starting ui_panel_) are drawn in their own node below, not in the tab's flat list
    // (Jay 2026-10-11: "I'd also like all of those UI features under their own dropdown menu (Like how we have the settled
    // features) Label it 'UI Master Control'").
    // 2026-10-11, the HUD panel (SPEC-UI-HUD-PANEL): its keys start ui_hud_ and go in the same node (Jay: "it all goes into
    // the same UI submenu") - so the node takes every key starting ui_ (ui_panel_ and ui_hud_ are the only ones).
    // 2026-10-18, Jay: "Comfort_hand_steady & its subsequent sliders are to be put under their own submenu like UI Master
    // Control is" - the node takes comfort_hand_steady, its _strength / _response sliders and comfort_melee_steady.
    const auto isSteady = [](const Setting& st) {
        return KeyHasPrefix(st.key, "comfort_hand_steady") || strcmp(st.key, "comfort_melee_steady") == 0;
    };
    // 2026-11-15, Jay: "I'd like you to put the rigging options in their own dropdown in VR Comfort" -> "Hands & Arms" (trace
    // docs/traces/20261115_1655_comfort_hands_arms_dropdown.md): these rows are drawn in that closed node, not the flat list.
    const auto isRig = [](const Setting& st) {
        // 2026-11-16, Jay: "put the sliders that the users wanted into the rig settings in VR comfort" -> + the six hand-angle dials
        // and the six controller grip offsets (trace docs/traces/20261116_1750_grip_offset_sliders.md); drawn in table order.
        static const char* const kRigKeys[] = { "buggy_hand_steer", "rig_cutscene_standdown", "finger_track",
                                                "rig_two_hand_tune_rz_deg", "comfort_body_follows_head", "calib_posture",
                                                "rig_grip_pitch_deg", "rig_grip_yaw_deg", "rig_grip_roll_deg",
                                                "rig_left_grip_pitch_deg", "rig_left_grip_yaw_deg", "rig_left_grip_roll_deg",
                                                "rig_hand_offset_r_fwd_cm", "rig_hand_offset_r_up_cm", "rig_hand_offset_r_right_cm",
                                                "rig_hand_offset_l_fwd_cm", "rig_hand_offset_l_up_cm", "rig_hand_offset_l_right_cm" };
        for (const char* k : kRigKeys) {
            if (strcmp(st.key, k) == 0) { return true; }
        }
        return false;
    };
    char nodeId[16];
    _snprintf_s(nodeId, sizeof(nodeId), _TRUNCATE, "tab%d", static_cast<int>(Tab::Comfort));
    // 2026-11-13: the ui_ keys are the HUD tab's now (TabForSetting files them as Tab::Hud) - the "UI Master Control" node is gone.
    const int drawn = DrawLiveThenSettled(nodeId, true, [&](const Setting& st) {
        // Jay 2026-11-14: "SNAP ANGLE HIDDEN UNLESS SNAP TURNING IS ENABLED" (trace docs/traces/20261114_1217_snap_angle_hidden.md):
        // the row is not drawn on smooth; its value is kept.
        if (ComfortTurnMode != 1 && strcmp(st.key, "comfort_snap_angle") == 0) { return false; }
        return TabForSetting(st) == Tab::Comfort && !isSteady(st) && !isRig(st);
    });
    if (drawn == 0) {
        ImGui::TextDisabled("Nothing here yet.");
    }
    ImGui::Spacing();
    if (MenuSection("Hands & Arms", "hands_arms", true)) {   // closed by default, like Hand Steadiness
        // 2026-11-16, Jay: "I also want those toggles to only show if advanced is on" (trace
        // docs/traces/20261116_1830_hands_arms_advanced_only.md): the hand angle + hand position rows are drawn only on Advanced.
        // Hidden rows keep their values, which still act.
        const auto isHandTune = [](const Setting& st) {
            return KeyHasPrefix(st.key, "rig_grip_") || KeyHasPrefix(st.key, "rig_left_grip_") ||
                   KeyHasPrefix(st.key, "rig_hand_offset_");
        };
        const bool adv = OverlayAdvanced();
        int count = 0;
        const Setting* all = AllSettings(count);
        for (int i = 0; i < count; ++i) {
            const Setting& st = all[i];
            if (st.locked || TabForSetting(st) != Tab::Comfort || !isRig(st)) { continue; }
            if (!adv && isHandTune(st)) { continue; }
            DrawOneSetting(st, i);
        }
        ImGui::TreePop();
    }
    ImGui::Spacing();
    if (MenuSection("Hand Steadiness", "hand_steady", true)) {   // closed by default, like UI Master Control
        int count = 0;
        const Setting* all = AllSettings(count);
        for (int i = 0; i < count; ++i) {
            const Setting& st = all[i];
            if (st.locked || TabForSetting(st) != Tab::Comfort || !isSteady(st)) { continue; }
            DrawOneSetting(st, i);
        }
        ImGui::TreePop();
    }
    // "UI Master Control" MOVED to the HUD tab (Jay 2026-11-13); "Re-centre HUD" is in the button row (Jay 2026-11-12).
    // The "Arm calibration" node is GONE (Jay 2026-11-12: "Remove arm calibration dropdown in VR comfort, as that's now been
    // superseded by the button up the top"): Calibrate my arms / Cancel calibration are the button row's (DrawButtonRow). Its
    // "Research calibration (7 timed poses)" button went with it - Jay: "Drop it from BETA" (ArmFitStart stays in armfit.cpp).
    SaveSettingsIfChanged();

    if (ComfortTurnMode == 1) {
        const char* why = nullptr;
        const int st = SnapTurnStatus(&why);
        ImGui::Spacing();
        if (st == 1) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Snap turning is active.");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "Snap turning is not active yet - the right stick turns smoothly until it is.");
            ImGui::TextWrapped("%s", why ? why : "no reason given");
        }
    }

    // declare_rendered_orientation's status (INV-326): the toggle alone cannot show whether frames are being declared.
    {
        const char* why = nullptr;
        const int st = CamHookRendDeclStatus(&why);
        if (st != 0) {
            ImGui::Spacing();
            if (st == 1) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Steady view on head turns is active.");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                   "Steady view on head turns is ON but not active - frames keep the normal view.");
                ImGui::TextWrapped("%s", why ? why : "no reason given");
            }
        }
    }
}

// THE WRIST HUD SECTION (stage 3a; trace docs/traces/20261113_1040_wrist_stage3a_settings.md; spec 2026-11-12-wrist-hud-design
// §6 / §9). BASIC (spec Q3 / Q5): the switch, the layout (preset), each piece's hand. ADVANCED: + overall size, the fade angles and
// every piece's position / tilt / turn / spin / size. Every value is a ui_wrist_* setting (wristquad.cpp) - drawn here, not by the
// generic list, because Basic / Advanced split them and the preset needs its confirm. "Place by hand" = stage 3b.
// PRESETS: choosing A or C copies its 40 values at once when nothing would be lost (the values hold a preset now); otherwise the menu
// asks first (spec §6: "choosing a preset overwrites custom - the menu asks first"). Any change that leaves the chosen preset's
// values turns the layout to Custom. COST: only while this tab is drawn - up to 45 rows and one 40-value compare a frame.
void DrawWristHud()
{
    static const char* const kPieceName[5] = { "Minimap", "Health", "Quest list", "Weapon", "Quick item" };
    static const char* const kHandItems[3] = { "Left", "Right", "Off" };
    static const char* const kPresetItems[3] = { "A", "B (split)", "Custom" };   // no credit in a preset name; B, not C - no skipped letter (Jay 11-13)
    static int s_ask = -1;   // a preset waiting for "Replace" / "Keep mine", -1 = none
    const bool adv = OverlayAdvanced();

    RowCheckbox("Wrist HUD", "##ui_wrist_hud", &UiWristHud,
                "Health, the minimap, the quest list and the two weapon boxes leave the view and sit on your hands, where they move "
                "with the controllers; turn a hand away and its pieces fade out. Needs Alternate Frame Warp on. Takes effect at the "
                "next launch.", true);
    if (UiWristHud != ::dlvr::wristquad::OnThisLaunch()) {
        ImGui::PushFont(g_descFont, 0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Restart the game for the Wrist HUD to turn %s.", UiWristHud ? "on" : "off");
        ImGui::PopFont();
    }

    int cur = (UiWristPreset < 0 || UiWristPreset > 2) ? 2 : UiWristPreset;
    if (RowCombo("Layout", "##ui_wrist_preset", &cur, kPresetItems, 3,
                 "A: minimap on the left hand; health, quest list and quick item on the left forearm; weapon on the right "
                 "forearm. B: health and minimap on the left wrist, both weapon boxes on the right wrist, no quest list. Custom: "
                 "your own placement.")) {
        if (cur == 2) {
            UiWristPreset = 2;
            s_ask = -1;
        } else if (cur != UiWristPreset || !::dlvr::wristquad::MatchesPreset(cur)) {
            const bool nothingLost = (UiWristPreset != 2 && ::dlvr::wristquad::MatchesPreset(UiWristPreset))
                                     || ::dlvr::wristquad::MatchesPreset(cur);
            if (nothingLost) {
                ::dlvr::wristquad::ApplyPreset(cur);
                UiWristPreset = cur;
                s_ask = -1;
            } else {
                s_ask = cur;
            }
        }
    }
    if (s_ask == 0 || s_ask == 1) {
        ImGui::PushFont(g_descFont, 0.0f);
        ImGui::TextWrapped("Replace your own placement with layout %s? Your positions, turns and sizes are lost.", kPresetItems[s_ask]);
        ImGui::PopFont();
        if (MenuButton("Replace###wristreplace", ImVec2(200.0f, 48.0f))) {
            ::dlvr::wristquad::ApplyPreset(s_ask);
            UiWristPreset = s_ask;
            s_ask = -1;
        }
        ImGui::SameLine(0.0f, 20.0f);
        if (MenuButton("Keep mine###wristkeep", ImVec2(200.0f, 48.0f))) { s_ask = -1; }
    }

    for (int p = 0; p < 5; ++p) {
        ImGui::PushID(p);
        char lbl[48];
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "%s: hand", kPieceName[p]);
        int h = (UiWristHand[p] < 0 || UiWristHand[p] > 2) ? 2 : UiWristHand[p];
        if (RowCombo(lbl, "##hand", &h, kHandItems, 3,
                     "Which hand this piece sits on. Off = not shown at all (not on the hands and not in front of your eyes).")) {
            UiWristHand[p] = h;
        }
        // STAGE 3b - Place by hand + Reset (spec Q5: "Hand + place button"; trace docs/traces/20261113_1108_wrist_3b_place.md).
        const bool canPlace = ::dlvr::wristquad::OnThisLaunch() && (UiWristHand[p] == 0 || UiWristHand[p] == 1);
        ImGui::BeginDisabled(!canPlace);
        if (MenuButton("Place###wristplace", ImVec2(200.0f, 48.0f),
                       canPlace ? "The menu hides and this piece appears in front of you. Point a controller at it and hold the grip to "
                                  "carry it; let go to leave it there. Pull the trigger to put it on the nearest hand. The menu button "
                                  "cancels."
                                : "Needs the Wrist HUD running (on at launch) and a hand chosen for this piece.")) {
            g_wristPlacePiece = p;
            g_wristPlaceResult = ::dlvr::wristquad::PlaceBegin(p);   // -1 = started: the panel hides from the next submit
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 20.0f);
        if (MenuButton("Reset###wristreset", ImVec2(200.0f, 48.0f),
                       "Puts this piece back where the chosen layout has it (layout A's place while the layout is Custom).")) {
            ::dlvr::wristquad::ResetPiece(p);
            g_wristPlacePiece = p;
            g_wristPlaceResult = -1;
        }
        ImGui::PopID();
    }
    // The last placement's result (OverlayPoll ended it; this is the first panel frame after it).
    if (g_wristPlaceResult >= 0 && g_wristPlacePiece >= 0 && g_wristPlacePiece < 5) {
        const ImVec4 ok(0.5f, 1.0f, 0.5f, 1.0f), warn(1.0f, 0.75f, 0.3f, 1.0f);
        const char* const nm = kPieceName[g_wristPlacePiece];
        ImGui::PushFont(g_descFont, 0.0f);
        switch (g_wristPlaceResult) {
            case ::dlvr::wristquad::kWpPlaced:    ImGui::TextColored(ok, "%s placed.", nm); break;
            case ::dlvr::wristquad::kWpCancelled: ImGui::TextColored(warn, "%s: cancelled - its old place is kept.", nm); break;
            case ::dlvr::wristquad::kWpUntracked: ImGui::TextColored(warn, "%s: a controller was not tracked - try again.", nm); break;
            case ::dlvr::wristquad::kWpTooFar:
                ImGui::TextColored(warn, "%s: too far from the hand (more than 30 cm) - hold it closer.", nm);
                break;
            case ::dlvr::wristquad::kWpHandOff:   ImGui::TextColored(warn, "%s: choose a hand for it first.", nm); break;
            case ::dlvr::wristquad::kWpNotOn:     ImGui::TextColored(warn, "The Wrist HUD is not running - turn it on and restart."); break;
            default: break;
        }
        ImGui::PopFont();
    }

    if (adv) {
        RowSliderF("Wrist HUD size", "##ui_wrist_size", &UiWristSize, 0.3f, 3.0f, "%.2f",
                   "The size of every piece on the hands. 1.0 = the minimap about 9 cm wide.");
        ArrowStepFocusedSlider(&UiWristSize, 0.3f, 3.0f, 0.01f);
        RowSliderF("Fade starts", "##ui_wrist_fade_start_deg", &UiWristFadeStartDeg, 0.0f, 90.0f, "%.0f deg",
                   "A piece is fully shown while it faces you within this angle; past it, it starts to fade.");
        ArrowStepFocusedSlider(&UiWristFadeStartDeg, 0.0f, 90.0f, 1.0f);
        RowSliderF("Gone at", "##ui_wrist_fade_end_deg", &UiWristFadeEndDeg, 1.0f, 120.0f, "%.0f deg",
                   "A piece turned this far away from you is not shown at all.");
        ArrowStepFocusedSlider(&UiWristFadeEndDeg, 1.0f, 120.0f, 1.0f);
        for (int p = 0; p < 5; ++p) {
            ImGui::PushID(100 + p);
            MenuHeading(kPieceName[p]);
            RowSliderF("Across the hand", "##x", &UiWristX[p], -0.3f, 0.3f, "%.3f m", "Moves the piece across the hand, metres.");
            ArrowStepFocusedSlider(&UiWristX[p], -0.3f, 0.3f, 0.005f);
            RowSliderF("Up / down", "##y", &UiWristY[p], -0.3f, 0.3f, "%.3f m", "Moves the piece up or down from the controller, metres.");
            ArrowStepFocusedSlider(&UiWristY[p], -0.3f, 0.3f, 0.005f);
            RowSliderF("Along the arm", "##z", &UiWristZ[p], -0.3f, 0.3f, "%.3f m",
                       "Moves the piece toward the fingers (negative) or up the forearm (positive), metres.");
            ArrowStepFocusedSlider(&UiWristZ[p], -0.3f, 0.3f, 0.005f);
            RowSliderF("Tilt", "##pitch", &UiWristPitch[p], -180.0f, 180.0f, "%.0f deg", "Tilts the piece forward or back.");
            ArrowStepFocusedSlider(&UiWristPitch[p], -180.0f, 180.0f, 1.0f);
            RowSliderF("Turn", "##yaw", &UiWristYaw[p], -180.0f, 180.0f, "%.0f deg",
                       "Turns the piece about the controller's up axis - which way it faces.");
            ArrowStepFocusedSlider(&UiWristYaw[p], -180.0f, 180.0f, 1.0f);
            RowSliderF("Spin", "##roll", &UiWristRoll[p], -180.0f, 180.0f, "%.0f deg",
                       "Spins the picture in its own plane - use it if a piece reads upside down or sideways.");
            ArrowStepFocusedSlider(&UiWristRoll[p], -180.0f, 180.0f, 1.0f);
            RowSliderF("Size", "##scale", &UiWristScale[p], 0.3f, 3.0f, "%.2f", "This piece's size, times the Wrist HUD size.");
            ArrowStepFocusedSlider(&UiWristScale[p], 0.3f, 3.0f, 0.01f);
            ImGui::PopID();
        }
    } else {
        ImGui::PushFont(g_descFont, 0.0f);
        ImGui::TextDisabled("Advanced shows the size, the fade and each piece's position, turn and spin.");
        ImGui::PopFont();
    }

    // A change that leaves the chosen preset's values = Custom (a pending ask keeps the old choice until answered).
    if (UiWristPreset != 2 && !::dlvr::wristquad::MatchesPreset(UiWristPreset)) { UiWristPreset = 2; }
}

// THE HUD TAB (BETA; Jay 2026-11-13: every HUD / UI setting on its own tab, after VR Comfort). The menu panel and HUD panel rows that
// were VR Comfort's "UI Master Control" node, drawn as that node drew them (DrawOneSetting, same order as the settings table), then
// the Wrist HUD section. Every ui_ key is Tab::Hud (TabForSetting); the ui_wrist_ ones are drawn by DrawWristHud.
void DrawHudTab()
{
    int count = 0;
    const Setting* all = AllSettings(count);
    // DROPDOWNS (Jay 2026-11-14: "make the main options in the HUD/UI tab as dropdowns, the menu is rather lengthy"; trace
    // docs/traces/20261114_1210_hud_tab_dropdowns.md): each of the three sections is a MenuSection, closed by default (as Hand
    // Steadiness); a section with no rows is not drawn. SaveSettingsIfChanged runs every frame either way.
    // NOT OFFERED (trace docs/traces/20261124_2015_hud_panel_menu_hidden.md; Jay 2026-11-24: "we don't really want the "HUD on face" as
    // an option anymore" -> "Hide only"): these rows are never drawn here, but they are NOT locked - the file value is still read at
    // load (U-68: ui_hud_panel=0 by hand is the Linux SteamVR workaround). tools/settings_schema.py reads this list (launcher: hidden).
    static const char* const kMenuHiddenKeys[] = { "ui_hud_panel" };
    const auto menuHidden = [](const Setting& st) {
        for (const char* k : kMenuHiddenKeys) { if (strcmp(st.key, k) == 0) { return true; } }
        return false;
    };
    const auto rows = [&](const char* heading, const char* sid, auto pick) {
        const auto take = [&](const Setting& st) {
            return !st.locked && !menuHidden(st) && TabForSetting(st) == Tab::Hud && !KeyHasPrefix(st.key, "ui_wrist_") && pick(st);
        };
        bool any = false;
        for (int i = 0; i < count && !any; ++i) { any = take(all[i]); }
        if (!any || !MenuSection(heading, sid, true)) { return; }
        for (int i = 0; i < count; ++i) {
            if (take(all[i])) { DrawOneSetting(all[i], i); }
        }
        ImGui::TreePop();
    };
    rows("Menu panel", "hudtab_menu", [](const Setting& st) { return KeyHasPrefix(st.key, "ui_panel_"); });
    // the in-world prompt's three (trace 20261123_1730) get their own section; HUD panel excludes them so none is drawn twice
    const auto isPrompt = [](const Setting& st) { return KeyHasPrefix(st.key, "ui_prompt_") || strcmp(st.key, "inworld_prompts") == 0; };
    rows("HUD panel", "hudtab_hud", [&](const Setting& st) { return !KeyHasPrefix(st.key, "ui_panel_") && !isPrompt(st); });
    rows("Prompts in the world", "hudtab_prompts", [&](const Setting& st) { return isPrompt(st); });
    if (MenuSection("Wrist HUD", "hudtab_wrist", true)) {
        DrawWristHud();
        ImGui::TreePop();
    }
    SaveSettingsIfChanged();
}

// THE PHYSICAL PARKOUR TAB (tester 2026-11-27: "add a new section to the L3 + R3 in-game mod menu, called Physical Parkour, and put
// all the settings we've added so far in neat sections"). Every "Physical Parkour" setting (TabForSetting -> Tab::Parkour): the
// master switch on top, then one closed section per part of the climb (docs/design_climbing_overhaul.md). A key of the group that no
// section names lands in "Other", so a setting added later is never unreachable. climb_debug_draw / climb_debug_physics were removed
// 2026-11-27 (locked keys: never drawn).
void DrawParkourTab()
{
    struct Section { const char* heading; const char* id; const char* const* keys; int n; };
    static const char* const kGrab[] = { "climb_reach_m", "climb_catch_ms", "climb_always_hang", "climb_hang_min_m",
                                         "climb_skip_grab_anim", "climb_grab_move_mps", "climb_room_check",
                                         "climb_ladder_by_hand", "climb_slide_mps", "climb_slide_grip", "climb_zipline_by_hand" };
    static const char* const kHang[] = { "climb_hand_motion", "climb_pull_max_mps", "climb_release_m", "climb_pin_hands",
                                         "climb_body_follows_view", "climb_view_rigid", "climb_hold_body", "climb_view_smooth", "climb_stick_shimmy" };
    static const char* const kUp[] = { "climb_mantle_stick", "climb_fast_climb_up", "climb_skip_reach", "climb_mantle_auto",
                                       "climb_mantle_m", "climb_ledge_last" };
    static const char* const kLaunch[] = { "climb_jump_by_look", "climb_jump_side_deg", "climb_launch_min_mps", "climb_launch_scale",
                                           "climb_launch_up_assist",
                                           "climb_air_momentum", "climb_air_momentum_all", "climb_air_control_pct", "climb_air_drag" };
    static const char* const kLow[] = { "climb_auto_low", "climb_auto_low_m" };
    static const char* const kRope[] = { "grapple_rope_swing", "grapple_aim_hand", "grapple_hand", "grapple_pull_hand",
                                         "grapple_trigger_pull", "grapple_hold_to_swing", "grapple_pull_mps", "grapple_unlock_skill" };
    static const char* const kDebug[] = { "diag_climb_probe" };
    // hide_game_crosshair (2026-11-28): not a climb setting, but filed on this tab at the tester's request - drawn under the master
    // switch, outside the climb sections.
    static const char* const kTop[] = { "climb_by_hand", "hide_game_crosshair" };
    constexpr int kTopN = static_cast<int>(sizeof(kTop) / sizeof(kTop[0]));
    static const Section kSections[] = {
        // (counts from the arrays: the hang list said 6 while holding 7, so climb_stick_shimmy was never drawn in its section)
        { "Grabbing ledges", "pk_grab", kGrab, static_cast<int>(sizeof(kGrab) / sizeof(kGrab[0])) },
        { "Moving while hanging", "pk_hang", kHang, static_cast<int>(sizeof(kHang) / sizeof(kHang[0])) },
        { "Climbing up", "pk_up", kUp, static_cast<int>(sizeof(kUp) / sizeof(kUp[0])) },
        { "Launching off", "pk_launch", kLaunch, static_cast<int>(sizeof(kLaunch) / sizeof(kLaunch[0])) },
        { "Low walls", "pk_low", kLow, static_cast<int>(sizeof(kLow) / sizeof(kLow[0])) },
        { "Grappling hook", "pk_rope", kRope, static_cast<int>(sizeof(kRope) / sizeof(kRope[0])) },
        { "Debug", "pk_debug", kDebug, static_cast<int>(sizeof(kDebug) / sizeof(kDebug[0])) },
    };
    const auto in = [](const char* key, const char* const* keys, int n) {
        for (int i = 0; i < n; ++i) { if (strcmp(key, keys[i]) == 0) { return true; } }
        return false;
    };
    int count = 0;
    const Setting* all = AllSettings(count);
    const auto mine = [](const Setting& st) { return !st.locked && TabForSetting(st) == Tab::Parkour; };
    for (int i = 0; i < count; ++i) {   // the master switch, outside any section
        if (mine(all[i]) && in(all[i].key, kTop, kTopN)) { DrawOneSetting(all[i], i); }
    }
    const auto section = [&](const char* heading, const char* id, auto pick) {
        bool any = false;
        for (int i = 0; i < count && !any; ++i) { any = mine(all[i]) && pick(all[i]); }
        ImGui::Spacing();
        if (!any || !MenuSection(heading, id, true)) { return; }
        for (int i = 0; i < count; ++i) {
            if (mine(all[i]) && pick(all[i])) { DrawOneSetting(all[i], i); }
        }
        ImGui::TreePop();
    };
    for (const Section& s : kSections) {
        section(s.heading, s.id, [&](const Setting& st) { return in(st.key, s.keys, s.n); });
    }
    section("Other", "pk_other", [&](const Setting& st) {
        if (in(st.key, kTop, kTopN)) { return false; }
        for (const Section& s : kSections) { if (in(st.key, s.keys, s.n)) { return false; } }
        return true;
    });
    SaveSettingsIfChanged();
}

// THE BUTTON ROW (Jay 2026-11-12, the BioShock-style menu: "Re-centre HUD, Calibrate my arms, Basic / Advanced"): three 400 x 64
// buttons, 20 px apart, at the top of the panel on every tab. The first two MOVED here from the VR Comfort tab and do exactly
// what they did there. Labels that change keep one ImGui id (###), so the controller stays on the button when its text changes.
void DrawButtonRow()
{
    const ImVec2 sz(400.0f, 64.0f);
    // Re-centre HUD: ui_hud_panel's action (was in VR Comfort > UI Master Control). Off while the HUD is not on its fixed panel.
    ImGui::BeginDisabled(!UiHudPanel);
    if (MenuButton("Re-centre HUD###recentre", sz,
                   UiHudPanel ? "Puts the HUD panel back in front of you."
                              : "Puts the HUD panel back in front of you. The HUD panel is off in the settings file (ui_hud_panel=0).")) {
        ::dlvr::uipanel::HudRecentre();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 20.0f);
    // Calibrate my arms (was in VR Comfort > Arm calibration): opens the calibration screen, which replaces the menu. While the
    // research calibration runs, the same button cancels it (as that node's "Cancel calibration").
    {
        char l1[200] = {}, l2[220] = {};
        const bool running = ::dlvr::ArmFitStatus(l1, sizeof(l1), l2, sizeof(l2));
        if (MenuButton(running ? "Cancel calibration###armcal" : "Calibrate my arms###armcal", sz,
                       running ? "Stops the calibration that is running."
                               : "Measures your arm length and shoulder position. Opens the calibration screen: stand up "
                                 "straight with both controllers in hand and follow the poses.")) {
            if (running) {
                ::dlvr::ArmFitCancel();
            } else {
                HolsterPanelClosing();             // place-by-hand Task 5: no placement / sphere outlives the panel
                g.visible = false;                 // the screen replaces the menu
                LOG("overlay: hidden (the calibration screen opens)");
                ::dlvr::CalibScreenOpen();
            }
        }
    }
    ImGui::SameLine(0.0f, 20.0f);
    // Basic / Advanced (overlay_advanced). Advanced-only rows: the Wrist HUD section's advanced rows (DrawWristHud) and VR Comfort >
    // Hands & Arms' hand angle + hand position rows (2026-11-16, trace 20261116_1830_hands_arms_advanced_only).
    {
        bool& adv = OverlayAdvanced();
        if (MenuButton(adv ? "Advanced -###advanced" : "Basic +###advanced", sz,
                       adv ? "Showing every setting. Press for the basic menu." : "Showing the basic menu. Press to show every setting.")) {
            adv = !adv;
            SaveSettingsIfChanged();
        }
    }
}

// ---------------------------------------------------------------------------
// DLSS.
//
// TWO DIFFERENT MECHANISMS ON ONE TAB, and the difference is visible in the UI
// rather than buried:
//
//   SHARPENING is LIVE (DlvrSetNisSharpness). The old live "Enable DLSS"
//   checkbox (DlvrSetDlssEnabled, F10's state) was REMOVED 2026-09-26 - Jay:
//   activation is the Anti-Aliasing dropdown only; F10 stays as a hidden A/B.
//   Its resolver went with it; the bootstrap export itself is unchanged.
//
//   EVERYTHING ELSE IS A MARKER FILE in dlvr\, read by the bootstrap ONCE at
//   startup. Changing one here writes the file; it takes effect next launch.
//   Said plainly on the tab rather than implied.
//
// WHY IT CANNOT ALL BE LIVE: DLSS lives in the bootstrap (xinput1_3.dll) and
// this overlay lives in the layer (LibOVRRT64_1.dll). The bootstrap reads its
// markers during its own startup, long before the layer exists.
using DlvrSetNisSharpnessFn = void (*)(float);
using DlvrGetNisSharpnessFn = float (*)();
using DlvrGetNisDispatchesFn = int (*)();
// requested, active, divisor, sceneW, sceneH - returns 0 if no pak was built.
using DlvrGetActiveQualityFn = int (*)(int*, int*, int*, int*, int*);

// The NIS half of the same channel, resolved the same way and independently -
// a bootstrap with the DLSS enable but not the sharpness exports yields a
// disabled slider rather than a live-looking one that does nothing.
bool ResolveNisChannel(DlvrSetNisSharpnessFn& setFn, DlvrGetNisSharpnessFn& getFn,
                       DlvrGetNisDispatchesFn& disFn)
{
    static bool tried = false;
    static DlvrSetNisSharpnessFn  s = nullptr;
    static DlvrGetNisSharpnessFn  g2 = nullptr;
    static DlvrGetNisDispatchesFn d = nullptr;
    if (!tried) {
        tried = true;
        if (HMODULE h = GetModuleHandleA("xinput1_3.dll")) {
            s  = reinterpret_cast<DlvrSetNisSharpnessFn>(GetProcAddress(h, "DlvrSetNisSharpness"));
            g2 = reinterpret_cast<DlvrGetNisSharpnessFn>(GetProcAddress(h, "DlvrGetNisSharpness"));
            d  = reinterpret_cast<DlvrGetNisDispatchesFn>(GetProcAddress(h, "DlvrGetNisDispatches"));
        }
    }
    setFn = s;
    getFn = g2;
    disFn = d;
    return s != nullptr && g2 != nullptr;
}

// A marker file is present/absent, so the control is a checkbox over the file
// itself. Written into dlvr\ beside the settings file.
//
// Uses the layer's OWN path helper rather than rebuilding the path here: it
// resolves relative to this DLL (not the exe), and it creates dlvr\ if it does
// not exist - which matters, because on a genuinely fresh install nothing else
// creates that folder and every writer in the mod silently degrades when it is
// missing. A second, hand-rolled path builder would have missed both.
bool MarkerPath(const char* name, char* out, size_t cap)
{
    return DlvrFilePath(name, out, cap);
}

void MarkerSet(const char* name, bool on, const char* contents)
{
    char p[MAX_PATH] = {};
    if (!MarkerPath(name, p, sizeof(p))) { SetStatus("could not resolve dlvr\\ path"); return; }
    if (on) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "w") == 0 && f) {
            fputs(contents ? contents : "created by the overlay\n", f);
            fclose(f);
            SetStatus("marker written - takes effect next launch");
        } else {
            SetStatus("could not write the marker file");
        }
    } else {
        if (DeleteFileA(p) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            SetStatus("marker removed - takes effect next launch");
        } else {
            SetStatus("could not remove the marker file");
        }
    }
}

// requested method, fsrMode, provider buffer, its size, fsr4Fallback - returns 0
// if the bootstrap is not enabled. See dlssflat.cpp DlvrGetAaState.
using DlvrGetAaStateFn = int (*)(int*, int*, char*, int, int*);

// THE ANTI-ALIASING TAB (renamed from "DLSS", Jay 2026-09-26, PROPOSAL-FSR §8).
// ONE dropdown decides what runs - NO AA (the default) / DLSS / FSR 3.1 / FSR 4 -
// and every control below it is shown only for the method that uses it. The old
// "Enable DLSS" checkbox is GONE: activation is the dropdown. F10 remains as a
// hidden runtime A/B (Jay: "Keep F10 as a hidden A/B").
void DrawDlssTab()
{
    const ImVec4 kAmber(0.9f, 0.75f, 0.4f, 1.0f);
    // In Segoe UI, as the Controls tab's paragraph (Jay 2026-11-14: "Anti-Aliasing description text (UNDER THE TAB) should also be
    // in segoe"; trace docs/traces/20261114_1220_aa_desc_segoe.md). nullptr g_descFont = the menu font.
    ImGui::PushFont(g_descFont, 0.0f);
    ImGui::TextWrapped("Anti-aliasing for the game's own image - in the headset, two eyes and two "
                       "histories. Choose the method below; the options under it follow the method.");
    ImGui::TextColored(kAmber, "Changes on this tab take effect after a restart - except sharpening, "
                               "which is live.");
    ImGui::PopFont();
    MenuDivider();   // was ImGui::Separator; this tab's SeparatorText headings are MenuHeading (2026-11-12, BioShock-style layout)

    // texture_aniso, ABOVE the method so it stays reachable under NO AA: it applies on every AA path. The bootstrap read the file
    // at launch; the first draw of this tab comes before any edit (this is the only edit point), so it latches what is running.
    MenuHeading("Textures");
    {
        static const int running = TextureAniso;
        const char* kAnisoNames[] = { "Game's own (at most 4x)", "2x", "4x", "8x", "16x (default)" };
        const int kAnisoValues[] = { 0, 2, 4, 8, 16 };
        int anisoIdx = -1;
        for (int i = 0; i < 5; ++i) { if (TextureAniso == kAnisoValues[i]) { anisoIdx = i; } }
        if (RowCombo("Texture sharpness", "##aniso", &anisoIdx, kAnisoNames, 5,
                     "Keeps floors and other surfaces at a shallow angle sharp further away. Higher is sharper. "
                     "Lower it if you need the frame rate back. Game's own - the game's original filtering.",
                     true) &&
            anisoIdx >= 0 && anisoIdx < 5) {
            TextureAniso = kAnisoValues[anisoIdx];
            SaveSettingsIfChanged();
        }
        if (TextureAniso != running) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "running now: %d  -  restart to apply the change", running);
        }
    }

    MenuHeading("Method");
    // THE FILE IS THE AUTHORITY, read once and re-set on every write - the lesson
    // the quality combo below already learned (a local default that displayed a
    // wrong answer). Parsed by the bootstrap's own header, so the three readers
    // (PAKGEN, dlssflat, this) cannot disagree about what a line means.
    static int method = -1;
    if (method < 0) {
        method = ::dlvr::kAaNone;
        char p[MAX_PATH] = {};
        if (MarkerPath("aa_method.txt", p, sizeof(p))) {
            bool present = false;
            char raw[16] = {};
            method = ::dlvr::ReadAaMethod(p, &present, raw, sizeof(raw));
            if (present && method == ::dlvr::kAaNone && _stricmp(raw, "none") != 0) {
                SetStatus("aa_method.txt holds a word that is not none/dlss/fsr3/fsr4/taa - treated as NO AA");
            }
        }
    }
    // TAA (2026-11-12, Jay: "a 'stock standard' anti aliasing solution, no upscale" -> "Yes, go with TAA"): index 4 == kAaTaa.
    const char* kMethods[] = { "NO AA", "DLSS", "FSR 3.1", "FSR 4", "TAA" };
    // 2026-11-11 (readable names only): the marker file's name is no longer drawn beside the dropdown.
    if (RowCombo("Anti-aliasing", "##aamethod", &method, kMethods, 5,
                 "NO AA - the game at native resolution, no anti-aliasing (the default). DLSS - NVIDIA RTX cards. "
                 "FSR 3.1 - any modern GPU. FSR 4 - RX 7000 / 9000 only; on any other GPU it runs FSR 3.1 and the "
                 "line below says so. TAA - any GPU, native resolution, no upscaling: lighter than DLSS / FSR, softer "
                 "image. F10 still toggles DLSS / FSR mid-session for a quick comparison.",
                 true) &&
        method >= 0 && method < 5) {
        const char* kWords[] = { "none\n", "dlss\n", "fsr3\n", "fsr4\n", "taa\n" };
        MarkerSet("aa_method.txt", true, kWords[method]);
        // TAA'S AUTO NIS (2026-11-12, Jay: ".12 auto switch then please" + "Make sure that it switches back off upon switching to
        // FSR/DLSS unless the USER has made their own value for it prior"; NIS 0.10-0.15 helped TAA in RUN TA2). The auto value is
        // tagged by a second line "auto" (the bootstrap reads only the first number, dlssflat.cpp fscanf_s "%f"); the slider's own
        // save writes the number alone, so anything the user set is untagged and never touched here.
        //   -> TAA:            no dlss_sharpness.txt -> write "0.120\nauto\n". A file already there (the user's value) is kept.
        //   -> any other method: a file tagged "auto" is removed (NIS off); an untagged file (the user's) is kept.
        char sp[MAX_PATH] = {};
        if (MarkerPath("dlss_sharpness.txt", sp, sizeof(sp))) {
            const bool present = GetFileAttributesA(sp) != INVALID_FILE_ATTRIBUTES;
            bool isAuto = false;
            if (present) {
                FILE* f = nullptr;
                if (fopen_s(&f, sp, "rb") == 0 && f) {
                    char buf[64] = {};
                    fread(buf, 1, sizeof(buf) - 1, f);
                    fclose(f);
                    isAuto = strstr(buf, "auto") != nullptr;
                }
            }
            if (method == ::dlvr::kAaTaa) {
                if (!present) {
                    MarkerSet("dlss_sharpness.txt", true, "0.120\nauto\n");
                    SetStatus("TAA: NIS sharpening set to 0.12 (restart to apply)");
                }
            } else if (present && isAuto) {
                MarkerSet("dlss_sharpness.txt", false, nullptr);
                SetStatus("TAA's automatic NIS sharpening switched off (restart to apply)");
            }
        }
    }

    // WHAT IS RUNNING, beside what was asked for - the rule the quality combo
    // learned: a menu that shows only the request lies whenever the launch ran
    // something else (a restart pending, a fallback, a hand-edited marker).
    {
        static DlvrGetAaStateFn aaGet = nullptr;
        static bool tried = false;
        if (!tried) {
            tried = true;
            if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                aaGet = reinterpret_cast<DlvrGetAaStateFn>(
                    reinterpret_cast<void*>(GetProcAddress(b, "DlvrGetAaState")));
            }
        }
        int runM = 0, runQ = -1, fb = 0;
        char prov[32] = {};
        if (aaGet && aaGet(&runM, &runQ, prov, sizeof(prov), &fb)) {
            const char* kQ[] = { "NATIVE", "QUALITY", "BALANCED", "PERFORMANCE" };
            char now[160] = {};
            if (runM < 0) {
                _snprintf_s(now, sizeof(now), _TRUNCATE,
                            "a hand-only marker (aa_native.txt) overrides the method");
            } else if (runM == ::dlvr::kAaFsr3 || runM == ::dlvr::kAaFsr4) {
                _snprintf_s(now, sizeof(now), _TRUNCATE, "FSR %s, %s%s", prov[0] ? prov : "(not started yet)",
                            (runQ >= 0 && runQ <= 3) ? kQ[runQ] : "?",
                            fb ? " - FSR 4 is NOT AVAILABLE on this GPU, running 3.1" : "");
            } else {
                _snprintf_s(now, sizeof(now), _TRUNCATE, "%s", ::dlvr::AaMethodName(runM));
            }
            const bool match = (runM == method);
            ImGui::TextColored(match && !fb ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(1.0f, 0.55f, 0.4f, 1.0f),
                               "running now: %s%s", now, match ? "" : "  -  restart to apply the change");
        } else {
            ImGui::TextDisabled("running now: unknown - the bootstrap is not enabled, or is older than this "
                                "layer (no DlvrGetAaState export)");
        }
    }

    if (method == ::dlvr::kAaNone) {
        ImGui::TextDisabled("NO AA: nothing else on this tab applies.");
        return;
    }
    const bool isDlss = (method == ::dlvr::kAaDlss);
    // TAA (2026-11-12): native resolution only, never foveated or cropped (dlssflat lenscrop / fovdlss refuse kPathTaa) - so the
    // Quality, Lens crop and Foveated sections are not drawn for it; Sharpening (NIS runs after the TAA resolve too) is.
    const bool isTaa = (method == ::dlvr::kAaTaa);

    // THE "Image" SECTION IS HIDDEN (2026-10-18, Jay: "The check boxes under 'Image' all need to be hidden & locked ... WITH
    // THEIR CURRENT VALUES"; lock depth "Menu-hide only"). Its five marker toggles are no longer drawn; the bootstrap still
    // reads the files, which the setup writes: dlss_mvfix.txt, dlss_cbsnap.txt, jitter_halton.txt (1.0) present;
    // dlss_mv_jittered.txt, dlss_depth_normal.txt absent. A hand-made marker still takes effect.

    if (!isTaa) {   // the block's own braces now belong to this if (TAA: section not drawn)
    MenuHeading("Quality");
        // >>> THIS IS REAL NOW. <<< The bootstrap's PAKGEN reads
        // dlvr\dlss_quality.txt at DLL_PROCESS_ATTACH - before the engine's
        // entry point, so before any pak is opened - and rewrites
        // render_loop.ppfx's scene sizes for the selected mode, computed from
        // the OUTPUT resolution rather than from anything hardcoded.
        //
        // The old version of this control was a DISABLED combo backed by a
        // `static int quality = 0`, so it always displayed "DLAA (native)"
        // regardless of what the game was actually running. That is worse than
        // absent: the user reported it showing DLAA during a live Quality run.
        // A control that confidently displays a wrong answer is the same
        // failure class as a verdict string compared against zero.
        //
        // SO IT READS THE MARKER, NOT A LOCAL DEFAULT - and the file is the
        // authority, exactly like the preset combo below.
        //
        // LINE 2 IS AN OUTPUT-RESOLUTION OVERRIDE AND MUST SURVIVE A MODE
        // CHANGE. It exists because nothing at DLL_PROCESS_ATTACH knows whether
        // a launch will reach a headset - dlss_vr.txt is just a file on disk and
        // is present in flatscreen too, which has already cost one run. Dropping
        // it here would silently re-point a flat test at the headset size.
        // >>> PER METHOD (Jay, 2026-09-26): "Quality modes available must be for
        // their respective upscaler (Everything except Ultra Perf for FSR)". <<<
        // One file, dlss_quality.txt, one mode number (0..4) - PAKGEN and dlssflat
        // read it the same way under either method. The old "Off - leave the pak
        // exactly as it is" row is gone: the METHOD now decides the pak.
        static int  qMode = -2;               // -2 unread, -1 no/invalid file, 0..4 the mode
        static char qOverride[64] = {};       // line 2, verbatim, or empty
        static char qBuf[128] = {};

        const char* kQualityDlss[] = {
            "DLAA (1.00x, native - no upscale)",
            "Quality (1.50x)",
            "Balanced (1.72x)",
            "Performance (2.00x)",
            "Ultra Performance (3.00x)",
        };
        const char* kQualityFsr[] = {
            "NATIVE (1.00x - anti-aliasing only)",
            "QUALITY (1.50x)",
            "BALANCED (1.72x)",
            "PERFORMANCE (2.00x)",
        };
        const char* const* kQuality = isDlss ? kQualityDlss : kQualityFsr;
        const int kQualityCount = isDlss ? 5 : 4;   // index == mode number

        if (qMode == -2) {
            qMode = -1;
            char p[MAX_PATH] = {};
            if (MarkerPath("dlss_quality.txt", p, sizeof(p))) {
                FILE* f = nullptr;
                if (fopen_s(&f, p, "r") == 0 && f) {
                    int v = -1;
                    if (fscanf_s(f, "%d", &v) == 1) {
                        if (v >= 0 && v <= 4) {
                            qMode = v;
                        } else {
                            // ANNOUNCED, NOT CLAMPED - the same contract PAKGEN
                            // itself honours. Showing "Off" for an out-of-range
                            // value would hide a hand-edit that is doing nothing.
                            SetStatus("dlss_quality.txt holds a mode outside 0-4");
                        }
                    }
                    char line[64] = {};
                    if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f)) {
                        int ow = 0, oh = 0;
                        if (sscanf_s(line, "%d %d", &ow, &oh) == 2) {
                            _snprintf_s(qOverride, sizeof(qOverride), _TRUNCATE, "%d %d", ow, oh);
                        }
                    }
                    fclose(f);
                }
            }
        }

        // Ultra Performance under FSR is REFUSED - by PAKGEN, which runs PERFORMANCE
        // for it - so the combo shows PERFORMANCE and says why, rather than a blank.
        int qIdx = qMode;
        if (!isDlss && qMode == 4) {
            qIdx = 3;
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f),
                               "dlss_quality.txt asks for Ultra Performance, which is not offered for FSR - "
                               "PERFORMANCE runs instead.");
        }
        const char* qHelp = isDlss
                   ? "The scene is rendered smaller and DLSS reconstructs it to full output "
                     "resolution; the HUD is drawn AFTER that, at full resolution, so it stays "
                     "sharp. The sizes are computed from YOUR output resolution, not baked in - "
                     "the mod's own detector then measures the ratio back out of the engine's "
                     "buffers and picks the matching DLSS mode, so the two sides share no "
                     "constant.\n\n"
                     "DLAA renders at full size and anti-aliases only."
                     // BETA (2026-10-18, leak hardening 6.4): the DEV text's next paragraph told the player to check
                     // [PAKGEN] / [DLSSFLAT] lines in dlvr_bootstrap.log - BETA logs carry ids, not tags, and the line
                     // named internals. Removed here only; src/ keeps it.
                   : "NATIVE renders at full size and FSR anti-aliases it, on the HDR image.\n\n"
                     "QUALITY / BALANCED / PERFORMANCE render the scene smaller and FSR rebuilds it "
                     "to full resolution; the HUD is drawn after that, at full resolution, so it "
                     "stays sharp.\n\n"
                     "Ultra Performance is not offered for FSR: at 3x its upscaling looks poor.";
                     // BETA (2026-10-18, leak hardening 6.4): the DEV text's log-verification paragraph removed, as in
                     // the DLSS branch above.
        // 2026-11-11 (readable names only): the marker file's name is no longer drawn beside the dropdown.
        if (RowCombo("Quality", "##aaquality", &qIdx, kQuality, kQualityCount, qHelp, true) && qIdx >= 0 &&
            qIdx < kQualityCount) {
            qMode = qIdx;
            if (qOverride[0]) {
                _snprintf_s(qBuf, sizeof(qBuf), _TRUNCATE, "%d\n%s\n", qIdx, qOverride);
            } else {
                _snprintf_s(qBuf, sizeof(qBuf), _TRUNCATE, "%d\n", qIdx);
            }
            MarkerSet("dlss_quality.txt", true, qBuf);
        }
        if (qMode < 0) {
            ImGui::TextDisabled(isDlss ? "not set - the pak keeps whatever was last written; pick one"
                                       : "not set - FSR runs NATIVE; pick one");
        }

        // The override is SHOWN rather than hidden, because it silently changes
        // what every mode computes and a user who set it once will not remember.
        if (qOverride[0]) {
            ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.4f, 1.0f),
                               "output resolution overridden to %s (line 2 of the marker)",
                               qOverride);
            ImGui::SameLine();
            HelpMarker("A developer control for flatscreen testing. Without it the headset "
                       "resolution is used, which is correct in VR and wrong for a flat run.");
        }

        // >>> WHAT IS ACTUALLY RUNNING, NOT JUST WHAT WAS ASKED FOR. <<<
        //
        // The combo above shows the REQUEST, which is a marker file. The
        // bootstrap may legitimately be running something else: ask for Quality
        // with no cached per-eye resolution and the launch falls back to
        // Performance at divisor 2 - exact, deliberate, and not what the marker
        // says. A menu that read only the marker would confidently display
        // "Quality" while the game rendered Performance, which is the same
        // defect this control already had once (a `static int quality = 0` that
        // displayed DLAA forever).
        //
        // So the OUTCOME is read back from the bootstrap every frame and shown
        // beneath the request, and it is only called a match when it is one.
        {
            DlvrGetActiveQualityFn aGet = nullptr;
            if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                aGet = reinterpret_cast<DlvrGetActiveQualityFn>(
                    reinterpret_cast<void*>(GetProcAddress(b, "DlvrGetActiveQuality")));
            }
            int req = -1, act = -1, div = 0, aw = 0, ah = 0;
            if (aGet && aGet(&req, &act, &div, &aw, &ah)) {
                const char* kShortDlss[] = { "DLAA", "Quality", "Balanced", "Performance",
                                             "Ultra Performance" };
                const char* kShortFsr[]  = { "NATIVE", "QUALITY", "BALANCED", "PERFORMANCE",
                                             "PERFORMANCE" };   // 4 is refused under FSR and runs 3
                const char* const* kShort = isDlss ? kShortDlss : kShortFsr;
                const char* actName = (act >= 0 && act <= 4) ? kShort[act] : "?";
                char how[96] = {};
                if (div > 0) {
                    _snprintf_s(how, sizeof(how), _TRUNCATE, "divisor %d (any resolution)", div);
                } else {
                    _snprintf_s(how, sizeof(how), _TRUNCATE, "scene %dx%d", aw, ah);
                }
                if (act == req) {
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                                       "running now: %s, %s", actName, how);
                } else {
                    const char* reqName = (req >= 0 && req <= 4) ? kShort[req] : "?";
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f),
                                       "running now: %s, %s  -  NOT the %s you asked for",
                                       actName, how, reqName);
                    ImGui::SameLine();
                    HelpMarker("The pak is built at startup and the engine holds it open for the "
                               "rest of the session, so the mode cannot change mid-run.\n\n"
                               "This happens when the per-eye resolution is not known yet - the "
                               "first VR launch, or the first launch after you change your "
                               "headset resolution. Rather than guess a size and lose upscaling "
                               "entirely, it falls back to an exact integer divisor.\n\n"
                               "The real size is recorded during this run. RESTART and you get "
                               "the mode you asked for.");
                }
            } else {
                ImGui::TextDisabled("running now: unknown - the bootstrap did not build a pak "
                                    "this launch (no marker, or it refused; the log says which)");
            }
        }
    }

    // LENS CROP (2026-11-11, docs/superpowers/plans/2026-11-11-dlss-lens-crop.md Task 2): live, so the headset run can compare crop
    // OFF / ON on the same view. Tier C (compiled off) until Jay verifies it. The margin is not drawn (Tier D, Jay's disk value).
    if (!isTaa) {   // TAA: section not drawn
    MenuHeading("Lens crop");
        if (RowCheckbox("Lens crop", "##lenscrop", &DlssLensCrop,
                        "Live. DLSS / FSR skip the rows at the top and bottom of each eye that the headset never shows; a "
                        "cheap fill keeps the image whole there.")) {
            SaveSettingsIfChanged();
        }
        // "Lens crop: paint the cropped bands red" (dlss_lens_crop_tint) is NOT drawn in BETA (Jay 2026-11-12: "Can also be hidden,
        // it's diagnostic and not needed for end users"). Compiled default false (camhook.cpp); still read from ovr2xr.txt.
    }

    // FOVEATED DLSS / FSR (2026-11-11, docs/superpowers/plans/2026-11-11-foveated-dlss.md Task 2). CheekyFoveatedDLSS's own controls,
    // names and defaults (USAGE.md; the author asked "If possible keep original UI as well"), credited with the repo link, plus our
    // Horizontal offset (Jay: "Yes, add the horizontal slider"). Live: a slider writes its global at once (the bootstrap debounces
    // recreates 300 ms); the file is saved on RELEASE, as the NIS slider does, not on every drag frame. Tier C (compiled off).
    // DROPDOWN (Jay 2026-11-14: "FOVEATED DLSS NEEDS TO BE A DROPDOWN"; trace docs/traces/20261114_1215_fov_dropdown_holster_button.md):
    // a MenuSection, closed by default; closed = rows not drawn, values untouched.
    if (!isTaa && MenuSection("Foveated DLSS / FSR", "aa_foveated", true)) {   // TAA: section not drawn
        // In Segoe UI (Jay 2026-11-12: "Cheekys name & link text, I'd like to be Segoe aswell for readability"); nullptr = menu font.
        ImGui::PushFont(g_descFont, 0.0f);
        ImGui::TextWrapped("Based on CheekyFoveatedDLSS by ClarkCheekyKent - github.com/ClarkCheekyKent/CheekyFoveatedDLSS");
        ImGui::PopFont();
        if (RowCheckbox("Enable foveated DLSS-SR", "##fovdlss", &FovDlss,
                        "Enables the main foveated Super Resolution path. VR only, live, DLSS and FSR 3: full quality "
                        "straight ahead of each eye, a cheaper pass outside it. The lens crop is ignored while this is on.")) {
            SaveSettingsIfChanged();
        }
        // Keyboard Left / Right step a FOCUSED slider by 1/100 of its range (ArrowStepFocusedSlider - Jay 2026-10-13, and RUN FV1:
        // "Couldn't adjust the sliders with the keyboard though"); called right after the slider so it reads that item's focus.
        // Saved on release of a drag, and on an arrow step (a focused, not-active slider whose value changed this frame).
        auto slider = [](const char* label, float* v, float lo, float hi, const char* fmt, const char* help) {
            const float before = *v;
            char id[64];
            _snprintf_s(id, sizeof(id), _TRUNCATE, "##%s", label);
            RowSliderF(label, id, v, lo, hi, fmt, help);
            const bool released = ImGui::IsItemDeactivatedAfterEdit();
            ArrowStepFocusedSlider(v, lo, hi, (hi - lo) * 0.01f);
            if (released || (*v != before && !ImGui::IsItemActive())) { SaveSettingsIfChanged(); }
        };
        slider("Fovea width", &FovWidth, 0.05f, 1.0f, "%.2f", "Sets the normalized size of the DLSS-processed region.");
        slider("Fovea height", &FovHeight, 0.05f, 1.0f, "%.2f", "Sets the normalized size of the DLSS-processed region.");
        slider("Height offset", &FovHeightOffset, -1.0f, 1.0f, "%.2f",
               "Moves the fovea up (negative) or down (positive). Zero = straight ahead.");
        slider("Horizontal offset", &FovXOffset, -1.0f, 1.0f, "%.2f",
               "Ours, not Cheeky's. + = toward the temple, both eyes mirrored; 0 = straight ahead.");
        slider("Roundness", &FovRoundness, 0.0f, 1.0f, "%.2f",
               "Blends the region shape from rectangular (0) to elliptical (1). This does not affect performance.");
        slider("Transition width", &FovTransition, 0.0f, 0.5f, "%.3f", "Feathers the edge of the region.");
        // FSR-AWARE CONTROLS (Jay 2026-11-11: "Yes" to "Peripheral FSR + hide the preset on FSR", then "Yes, do the same for Center
        // supersampling"): on FSR (3.1 and 4) the periphery pass is FSR's own (a second FSR context per eye - fsrinterop.cpp
        // DispatchPeriphery), never DLSS; FSR has no presets; and FSR's centre has no supersampled output (fsrinterop.cpp
        // DispatchCentre writes the output rectangle), so the preset combo and Center supersampling are hidden there - their values
        // are kept for DLSS. The method is the one RUNNING (the bootstrap's DlvrGetAaState, as "running now" above), else the
        // dropdown's choice. "###" keeps one ImGui id whatever the label says.
        bool fovFsr = (method == ::dlvr::kAaFsr3 || method == ::dlvr::kAaFsr4);
        {
            static DlvrGetAaStateFn s_aaGet = nullptr;
            static bool s_tried = false;
            if (!s_tried) {
                s_tried = true;
                if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                    s_aaGet = reinterpret_cast<DlvrGetAaStateFn>(reinterpret_cast<void*>(GetProcAddress(b, "DlvrGetAaState")));
                }
            }
            int runM = 0, runQ = -1, fb = 0;
            char prov[32] = {};
            if (s_aaGet && s_aaGet(&runM, &runQ, prov, sizeof(prov), &fb) && runM >= 0) {
                fovFsr = (runM == ::dlvr::kAaFsr3 || runM == ::dlvr::kAaFsr4);
            }
        }
        if (!fovFsr) {
            slider("Center supersampling", &FovSupersample, 1.0f, 2.0f, "%.2fx",
                   "Scales the center DLSS output by 1-2x per dimension, then area-downsamples to its original size.");
        }
        if (RowCheckbox(fovFsr ? "Peripheral FSR" : "Peripheral DLSS", "##fovperiph", &FovPeriphery,
                        fovFsr ? "Enables the auxiliary FSR pass for the area outside the fovea (FSR anti-aliasing at the "
                                 "periphery scale - no DLSS involved). Off = a plain stretch outside the fovea: a debug "
                                 "comparison only."
                               : "Enables the auxiliary DLSS pass for the area outside the fovea. Off = a plain stretch outside "
                                 "the fovea: a debug comparison only.")) {
            SaveSettingsIfChanged();
        }
        if (!fovFsr) {
            static const char* const kFovPresets[] = { "E (Fastest)", "K", "L", "M" };
            static const int kFovPresetIds[] = { 5, 11, 12, 13 };
            int presetIdx = 0;
            for (int i = 0; i < 4; ++i) { if (kFovPresetIds[i] == FovPeripheryPreset) { presetIdx = i; } }
            if (RowCombo("Peripheral preset", "##fovpreset", &presetIdx, kFovPresets, 4,
                         "Selects E, K, L, or M for the peripheral DLSS pass.")) {
                FovPeripheryPreset = kFovPresetIds[presetIdx];
                SaveSettingsIfChanged();
            }
        }
        slider("Periphery scale", &FovPeripheryScale, 0.2f, 1.0f, "%.2f",
               "Sets peripheral processing to 20-100 percent of the DLSS input width and height; 0.75 is the default.");
        if (RowCheckbox("Show 5 px red alignment border", "##fovborder", &FovBorder,
                        "Displays the processed region while calibrating the fovea.")) {
            SaveSettingsIfChanged();
        }
        ImGui::TreePop();
    }

    MenuHeading("Sharpening");
    {
        // >>> A SHARPENER CHOICE, WITH ONE ENTRY TODAY. <<< Jay, 2026-09-26: "NIS
        // Scaling available on both FSR AND DLSS modes (FIDELITY CAS WILL BE ADDED
        // LATER, USERS WILL BE ABLE TO CHOOSE BETWEEN WHICH THEY PREFER WITH A
        // DROPDOWN)". The row exists now so CAS is an added entry, not a re-layout.
        // NIS already runs after BOTH resolves (the DLSS/FSR NATIVE substitution and
        // the upscale path), so nothing in the bootstrap changes for it.
        static int sharpener = 0;
        const char* kSharpeners[] = { "NIS (NVIDIA Image Scaling)" };
        RowCombo("Sharpener", "##sharpener", &sharpener, kSharpeners, 1,
                 "The sharpening pass that runs after DLSS or FSR. FidelityFX CAS will be added here as a second choice.");
        DlvrSetNisSharpnessFn  nSet = nullptr;
        DlvrGetNisSharpnessFn  nGet = nullptr;
        DlvrGetNisDispatchesFn nDis = nullptr;
        if (ResolveNisChannel(nSet, nGet, nDis)) {
            const float state = nGet();
            if (state < 0.0f) {
                ImGui::TextDisabled("Sharpening is not running in this session.");
            } else {
                // LIVE, AND THE VALUE IS READ BACK FROM THE BOOTSTRAP each frame
                // rather than kept here - same reason as the Enable checkbox. A
                // local copy would drift from what the pass is actually using
                // and the slider would start lying after the first marker edit.
                float v = state;
                static const char* const kNisHelp =
                    "NVIDIA Image Scaling in sharpen-only mode. Live - the image changes as you move it; it is saved when "
                    "you let go. 0 turns the pass off. It runs before the game's HUD, so the HUD is not sharpened.";
                if (RowSliderF("NIS sharpening", "##nis", &v, 0.0f, 1.0f, "%.2f", kNisHelp)) {
                    nSet(v);   // immediate - the pass rebuilds its config every dispatch
                }
                // PERSIST ONLY ON RELEASE. Writing the marker on every slider
                // frame would rewrite the file sixty times a second while
                // dragging; this is the same reasoning as SaveSettingsIfChanged's
                // compare, arrived at for the same reason.
                auto persist = [](float val) {
                    char buf[32];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.3f\n", val);
                    if (val > 0.0f) { MarkerSet("dlss_sharpness.txt", true, buf); }
                    else            { MarkerSet("dlss_sharpness.txt", false, nullptr); }
                    SetStatus("sharpness saved");
                };
                const bool released = ImGui::IsItemDeactivatedAfterEdit();
                // KEYBOARD Left / Right (Jay 2026-10-19: "Fix it for the keyboard" - a tester's
                // "stays at 0"). This slider was the one slider without ArrowStepFocusedSlider,
                // so the arrows did nothing on it; the D-pad (A, then D-pad) already worked.
                // Same step as every settings row: 1/100 of the range. Rounded to the 0.01 grid
                // so stepping down lands on an exact 0 - a float residue above 0 would write
                // "0.000" as ON and keep the pass running. Saved once the arrow is let go, not
                // per key repeat - the same reason as the release rule above.
                static bool s_arrowUnsaved = false;
                const float beforeArrow = v;
                ArrowStepFocusedSlider(&v, 0.0f, 1.0f, 0.01f);
                if (v != beforeArrow) {
                    v = floorf(v * 100.0f + 0.5f) / 100.0f;
                    nSet(v);
                    s_arrowUnsaved = true;
                }
                if (released) {
                    persist(v);
                    s_arrowUnsaved = false;
                } else if (s_arrowUnsaved && !ImGui::IsKeyDown(ImGuiKey_LeftArrow)
                                          && !ImGui::IsKeyDown(ImGuiKey_RightArrow) && !g.vrStepHeld) {
                    // + the Touch right stick (2026-11-11): saved once the stick is let go, as for the arrows.
                    persist(v);
                    s_arrowUnsaved = false;
                }
                // 2026-11-11 (readable names only): the marker file's name is no longer drawn; the help is in the strip
                // under the list (kNisHelp). Not DLSS's own InSharpness, which the SDK marks deprecated (programming guide
                // section 3.11 replaces it with exactly this).

                // A dispatch count of zero while sharpness is up means the pass
                // is not executing - which looks identical to "too subtle to
                // see" if nobody says so.
                if (nDis) {
                    const int n = nDis();
                    if (v > 0.0f && n == 0) {
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                                           "NIS has never dispatched - the pass is not running. "
                                           "dlvr_bootstrap.log says why.");
                    } else if (n > 0) {
                        ImGui::TextDisabled("NIS dispatches: %d", n);
                    }
                }
            }
        } else {
            ImGui::TextDisabled("Sharpening control unavailable.");
            ImGui::SameLine();
            HelpMarker("xinput1_3.dll did not export DlvrSetNisSharpness/DlvrGetNisSharpness - "
                       "usually an older bootstrap beside a newer layer. Rebuild and redeploy "
                       "both, and verify by md5 against build\\Release.");
        }
    }

    // Jay, 2026-09-26: "PRESET MUST ONLY SHOW IF DLSS IS ACTIVE".
    if (!isDlss) { return; }
    MenuHeading("Model");
    {
        // THIS ONE IS REAL - the bootstrap already reads dlss_preset.txt as a
        // decimal preset id, 0 meaning "use K".
        // >>> ONLY THE PRESETS THAT STILL EXIST. <<<
        //
        // v1 of this list offered A, B, C, D, E, F, J, K, L, M - and more than
        // half of those are gone. Read from the SDK's own enum comments in
        // nvsdk_ngx_defs.h and the programming guide section 3.12, NOT from
        // memory:
        //
        //   A-D   REMOVED as of NGX SDK 310.6.0
        //   E, F  "Deprecated"
        //   G     "Do not use, reverts to default behavior"   (H, I reserved)
        //   J=10  "Similar to preset K. Preset J might exhibit slightly less
        //          ghosting at the cost of extra flickering. Preset K is
        //          generally recommended over preset J"
        //   K=11  "Default preset for DLAA/Balanced/Quality modes that is
        //          transformer based. Best image quality preset at a higher
        //          performance cost"
        //   L=12  "Default for Ultra Perf mode"
        //   M=13  "Default for Perf mode"
        //   N, O  "Do not use, reverts to default behavior"
        //
        // Offering a removed preset is worse than offering none: the runtime
        // silently reverts to default, which is indistinguishable from "this
        // preset looks the same" - and the tooltip below already warns against
        // exactly that misreading, so the list must not manufacture it.
        //
        // >>> THE PRESET IS PER-MODE, AND THE MODE IS NOW SELECTABLE ABOVE. <<<
        // K is the default for DLAA/Balanced/Quality, M for Performance, L for
        // Ultra Performance. This comment was written when DLAA was all that
        // ran, and read "L/M are off-label rather than wrong" - which stopped
        // being true the moment upscaling shipped.
        //
        // MEASURED, 2026-09-13: a Quality (1.50x) run was judged with M still
        // set, because M had been correct at 2.00x. Setting K for the same scene
        // and ratio, the user reported it "looks much better compared to M".
        // A PRESET VALIDATED FOR ONE MODE IS NOT VALIDATED FOR THE MODE THAT
        // REPLACES IT - the same regime rule as MVJittered.
        //
        // The bootstrap applies whatever is chosen to ALL FIVE hints, so it
        // cannot fall back per-mode on its own; picking the wrong one is silent.
        static int  presetIdx = -1;
        static char presetBuf[32] = {};
        const char* kPresets[] = {
            "Default (K)",
            "K - transformer, best quality",
            "J - less ghosting, more flicker",
            "L - default for Ultra Performance",
            "M - default for Performance",
        };
        const int kPresetIds[] = { 0, 11, 10, 12, 13 };
        const int kPresetCount = static_cast<int>(sizeof(kPresetIds) / sizeof(kPresetIds[0]));
        if (presetIdx < 0) {
            presetIdx = 0;
            char p[MAX_PATH] = {};
            if (MarkerPath("dlss_preset.txt", p, sizeof(p))) {
                FILE* f = nullptr;
                if (fopen_s(&f, p, "r") == 0 && f) {
                    int v = 0;
                    if (fscanf_s(f, "%d", &v) == 1) {
                        bool known = false;
                        for (int i = 0; i < kPresetCount; ++i) {
                            if (kPresetIds[i] == v) { presetIdx = i; known = true; }
                        }
                        // A hand-edited file naming a removed preset must SAY so
                        // rather than silently showing "Default (K)" while the
                        // bootstrap passes the dead id through to NGX.
                        if (!known && v != 0) {
                            SetStatus("dlss_preset.txt names a preset that no longer exists");
                        }
                    }
                    fclose(f);
                }
            }
        }
        // 2026-11-11 (readable names only): the marker file's name is no longer drawn beside the dropdown.
        if (RowCombo("DLSS model", "##dlssmodel", &presetIdx, kPresets, kPresetCount,
                     "K is the transformer-based default and the SDK calls it the best image quality preset. A different "
                     "preset is not guaranteed to look different on every scene. Match it to the mode above: K for DLAA / Quality / "
                     "Balanced, M for Performance, L for Ultra Performance.",
                     true) &&
            presetIdx >= 0 && presetIdx < kPresetCount) {
            _snprintf_s(presetBuf, sizeof(presetBuf), _TRUNCATE, "%d\n", kPresetIds[presetIdx]);
            if (presetIdx == 0) {
                MarkerSet("dlss_preset.txt", false, nullptr);
            } else {
                MarkerSet("dlss_preset.txt", true, presetBuf);
            }
        }
        // Measured 2026-09-13: Quality with M set was visibly worse than the same scene with K.
    }
}

// -------------------------------------------------------------- rendering ---

bool g_quadRtvSrgb = true;   // CreateSwapchainInto's RTV choice (true = _SRGB, the pre-2026-11-14 behaviour); OverlayQuadRtvIsSrgb()

bool CreateSwapchainInto(XrSwapchain& sc, std::vector<XrSwapchainImageD3D11KHR>& images,
                         std::vector<ID3D11RenderTargetView*>& rtvs, const char* what, int w = kPanelW, int h = kPanelH,
                         bool makeRtvs = true)
{
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    if (!makeRtvs) { ci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT; }   // the floor image path CopyResource()s into it
    ci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(w);
    ci.height = static_cast<uint32_t>(h);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;

    XrResult r = xrCreateSwapchain_(g.session, &ci, &sc);
    if (XR_FAILED(r)) {
        LOG("overlay: xrCreateSwapchain failed (%d) for the %s", static_cast<int>(r), what);
        return false;
    }

    uint32_t n = 0;
    xrEnumerateSwapchainImages_(sc, 0, &n, nullptr);
    images.resize(n);
    for (auto& i : images) {
        i = XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
    }
    if (XR_FAILED(xrEnumerateSwapchainImages_(
            sc, n, &n,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
        return false;
    }

    rtvs.resize(n, nullptr);
    // ImGui's colours are already sRGB-encoded display values; an _SRGB RTV would encode them a second time (the menu too bright,
    // Jay 2026-11-14). On TYPELESS images (Jay's runtime: format 27) the RTV is plain UNORM - stored as given, decoded once by the
    // compositor (the swapchain stays _SRGB). Any other image format keeps the old _SRGB RTV. Trace 20261114_1230_imgui_quad_rtv_gamma.
    int imageFormat = -1;
    bool rtvSrgb = true;
    for (uint32_t i = 0; i < n && makeRtvs; ++i) {
        D3D11_TEXTURE2D_DESC td{};
        images[i].texture->GetDesc(&td);
        if (i == 0) { imageFormat = static_cast<int>(td.Format); }
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = (td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (i == 0) { rtvSrgb = (rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); }
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.device->CreateRenderTargetView(images[i].texture, &rd, &rtvs[i]))) {
            LOG("overlay: CreateRenderTargetView failed for image %u of the %s", i, what);
            return false;
        }
    }
    if (makeRtvs) { g_quadRtvSrgb = rtvSrgb; }
    LOG("overlay: %ux%u quad swapchain, %u images (%s), image format %d, RTV %s", ci.width, ci.height, n, what, imageFormat,
        makeRtvs ? (rtvSrgb ? "UNORM_SRGB (image not TYPELESS - ImGui colours encoded twice)" : "UNORM") : "none");
    return true;
}

bool CreateSwapchain()
{
    // kPanelW x kMenuH (2026-11-12): the menu is 1000 tall; the reticle / hold bar use sub-rects of its top kPanelH rows.
    if (!CreateSwapchainInto(g.swapchain, g.images, g.rtvs, "panel / gun reticle", kPanelW, kMenuH)) {
        return false;
    }
    if (Grapple3dReticle && !CreateSwapchainInto(g.swapchain2, g.images2, g.rtvs2, "grapple reticle")) {
        // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=FLAT  log-only=yes
        LOG("overlay: [GRAPPLE3D] *** the grapple reticle's swapchain could not be created - no 3D grapple icon this session ***");
        g.swapchain2 = XR_NULL_HANDLE;
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------------ public --

bool OverlayInit(ID3D11Device* device, XrSession session)
{
    if (g.initialised) {
        return true;
    }
    if (g.failed || !device || session == XR_NULL_HANDLE) {
        return false;
    }

    g.device = device;
    g.device->AddRef();
    g.device->GetImmediateContext(&g.context);
    g.session = session;

    if (!CreateSwapchain()) {
        g.failed = true;
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // the settings file is ours, not ImGui's
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = false;
    io.DisplaySize = ImVec2(static_cast<float>(kPanelW), static_cast<float>(kPanelH));

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // Everything is scaled up: this is read through a lens at a metre and a
    // half, not on a monitor at arm's length.
    style.ScaleAllSizes(1.6f);
    style.FontScaleMain = 1.6f;   // io.FontGlobalScale moved here in 1.92
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 0.94f);

    // THE MENU FONT (Jay 2026-11-12: "You supply a TTF"): dlvr\menu\font.ttf, once, here. The built-in font stays Fonts[0] (the
    // default for every other ImGui frame); the menu pushes this one (DrawUi). Checked for existence first: AddFontFromFileTTF
    // on a missing file is an ImGui user error. COST: once per session.
    {
        char fp[MAX_PATH] = {};
        io.Fonts->AddFontDefault();
        if (!DlvrFilePath("menu\\font.ttf", fp, sizeof(fp))) {
            // [PROBE: MENUART] gate=none (once per session - the menu font)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUART] font: could not resolve dlvr\\menu\\font.ttf - the menu uses the built-in font");
        } else if (GetFileAttributesA(fp) == INVALID_FILE_ATTRIBUTES) {
            // [PROBE: MENUART] gate=none (once per session - the menu font)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUART] font: %s not found - the menu uses the built-in font", fp);
        } else {
            g_menuFont = io.Fonts->AddFontFromFileTTF(fp, kMenuFontBase);
            // ALL CAPS (Jay 2026-11-12: "The font has to be used in all caps to work properly"; option 1, font-level): every
            // lowercase a-z draws as its capital in the MENU font only. ImGui applies the remap before it loads a glyph or its
            // advance (imgui_draw.cpp ImFontAtlas_FontHookRemapCodepoint), so widths match what is drawn. Fonts[0] unaffected.
            if (g_menuFont) {
                for (ImWchar c = 'a'; c <= 'z'; ++c) {
                    g_menuFont->AddRemapChar(c, static_cast<ImWchar>(c - 'a' + 'A'));
                }
            }
            // [PROBE: MENUART] gate=none (once per session - the menu font)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUART] font: %s %s", fp,
                g_menuFont ? "loaded - the menu uses it, a-z remapped to A-Z" : "did NOT load - the menu uses the built-in font");
        }
        // THE DESCRIPTION FONT (Jay 2026-11-12: "Windows' Segoe UI"): the Windows folder's own Fonts\segoeui.ttf, not shipped.
        char wd[MAX_PATH] = {}, dp[MAX_PATH] = {};
        const UINT wn = GetWindowsDirectoryA(wd, MAX_PATH);
        if (wn == 0 || wn >= MAX_PATH) {
            // [PROBE: MENUART] gate=none (once per session - the description font)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUART] description font: GetWindowsDirectoryA failed - the descriptions use the menu font");
        } else {
            _snprintf_s(dp, sizeof(dp), _TRUNCATE, "%s\\Fonts\\segoeui.ttf", wd);
            if (GetFileAttributesA(dp) == INVALID_FILE_ATTRIBUTES) {
                // [PROBE: MENUART] gate=none (once per session - the description font)  site=HEADSET  log-only=yes
                LOG("overlay: [MENUART] description font: %s not found - the descriptions use the menu font", dp);
            } else {
                g_descFont = io.Fonts->AddFontFromFileTTF(dp, kMenuFontBase);
                // [PROBE: MENUART] gate=none (once per session - the description font)  site=HEADSET  log-only=yes
                LOG("overlay: [MENUART] description font: %s %s", dp,
                    g_descFont ? "loaded - the description box uses it" : "did NOT load - the descriptions use the menu font");
            }
        }
    }

    if (!ImGui_ImplDX11_Init(g.device, g.context)) {
        LOG("overlay: ImGui_ImplDX11_Init failed");
        ImGui::DestroyContext();
        g.failed = true;
        return false;
    }

    g.initialised = true;
    LOG("overlay: ready");
    return true;
}

void OverlayPoll()
{
    // BETA TREE: says once which menu this binary carries, so a log names the tree that ran.
    static bool loggedBeta = false;
    if (!loggedBeta) {
        LOG("overlay: BETA build - the menu holds %d tab(s): VR Comfort, Anti-Aliasing", kTabCount);
        loggedBeta = true;
    }
    if (!OverlayEnabled()) {
        HolsterPanelClosing();   // place-by-hand Task 5: a placement / the sphere never outlives the panel. COST: see its definition
        g.visible = false;
        g.holdProgress = 0.0f;
        g.comboStart = 0.0;
        return;
    }
    // The calibration screen is on top of everything and owns the controllers while it is up - the menu cannot open over it.
    if (::dlvr::CalibScreenActive()) {
        HolsterPanelClosing();   // as above
        g.visible = false;
        g.holdProgress = 0.0f;
        g.comboStart = 0.0;
        return;
    }
    const bool key = (GetAsyncKeyState(OverlayKey()) & 0x8000) != 0;
    const bool pad = PadComboHeld(OverlayPad());
    const bool now = key || pad;

    // TOUCH / INDEX: L3 + R3 held together for kMenuHoldSec (2 s) opens or closes the menu (Jay 2026-11-11). Fires once per hold;
    // both clicks must be let go before the next hold counts. Not while a Controls-tab capture owns the controller (it may be
    // capturing L3 / R3) or the overlay binding is being captured. The game never sees L3 / R3 while both are held (ovr2xr.cpp,
    // the scePad filter). COST: two cached-bool reads + one QPC per submit.
    bool vrToggle = false;
    {
        bool l3 = false, r3 = false;
        GetControllerStickClick(kLeftHand, l3);
        GetControllerStickClick(kRightHand, r3);
        if (l3 && r3 && !g.comboLatched && g.capturing == 0 && !ControlsBusy()) {
            const double t = Now();
            if (g.comboStart <= 0.0) { g.comboStart = t; }
            const double held = t - g.comboStart;
            g.holdProgress = static_cast<float>(held / kMenuHoldSec);
            if (held >= kMenuHoldSec) {
                vrToggle = true;
                g.comboLatched = true;
                g.holdProgress = 0.0f;
                g.comboStart = 0.0;
                // [PROBE: MENUVR] gate=none (each L3+R3 toggle and each B close - event-driven)  site=HEADSET  log-only=yes
                LOG("overlay: [MENUVR] L3+R3 held %.2f s - the menu %s", held, g.visible ? "closes" : "opens");
            }
        } else {
            g.comboStart = 0.0;
            g.holdProgress = 0.0f;
        }
        if (!l3 && !r3) { g.comboLatched = false; }
    }
    // B with nothing to back out of (DrawUi decided it on the last panel frame).
    if (g.closeRequest) {
        g.closeRequest = false;
        if (g.visible && !AnyPlacing()) {
            g.visible = false;
            g_holsterShowZone = -1;
            // [PROBE: MENUVR] gate=none (each L3+R3 toggle and each B close - event-driven)  site=HEADSET  log-only=yes
            LOG("overlay: [MENUVR] B pressed with nothing open - the menu closes");
            LOG("overlay: hidden");
        }
    }

    // While rebinding, the binding itself must not toggle the overlay shut
    // under the user's fingers.
    const bool edge = (now && !g.togglePrev && g.capturing == 0) || vrToggle;
    // Holster place-by-hand (plan 2026-10-30 Task 5, ruling R2, Task 1 G3): while a placement waits for the trigger, THIS is
    // its per-frame tick, and the menu press edge cancels it (ONE press) instead of toggling the panel - g.visible stays true
    // throughout, so every OverlayVisible() input block holds. When it ends, the panel is drawn again on this same submit
    // (OverlayRender runs after this), whose Holsters page shows the result and saves the stored spot.
    // COST idle: one volatile read per submit. Placing: HolsterPlaceTick (one trigger read; the pull, once - see holster.cpp).
    if (HolsterPlacing()) {
        ControlsNotePlaceResult(HolsterPlaceTick(edge));
    } else if (::dlvr::wristquad::Placing()) {   // WRIST HUD 3b: the same contract as the holster placement above
        const int r = ::dlvr::wristquad::PlaceTick(edge);
        if (r >= 0) { g_wristPlaceResult = r; }
    } else if (edge) {
        g.visible = !g.visible;
        g.justOpened = g.visible;
        if (!g.visible) { g_holsterShowZone = -1; }   // the panel closed: the sphere goes with it (spec goal 10)
        LOG("overlay: %s", g.visible ? "shown" : "hidden");
    }
    g.togglePrev = now;
}

bool OverlayVisible()
{
    return g.visible;
}

ID3D11Device* OverlayDevice()
{
    return g.device;
}

bool OverlayQuadRtvIsSrgb()
{
    return g_quadRtvSrgb;
}

bool OverlayPassLook()
{
    return g.visible && g.passLook;
}

// Same rotate-local-(0,0,-1)-by-orientation math as aimhook.cpp's
// ForwardFromPose - deliberately duplicated rather than shared across
// modules for four lines of stable, pure math; see that file if this one
// ever needs to change; it should change with it.
void ReticleForwardFromPose(const XrQuaternionf& q, float& fx, float& fy, float& fz)
{
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, yy = q.y * y2;
    const float xz = q.x * z2, yz = q.y * z2;
    const float wx = q.w * x2, wy = q.w * y2;
    fx = -(xz + wy);
    fy = -(yz - wx);
    fz = -(1.0f - (xx + yy));
}

void ReticleRotate(const XrQuaternionf& q, const float v[3], float o[3])
{
    // v' = v + 2w(u x v) + 2 u x (u x v), u = (x, y, z)
    const float ux = q.x, uy = q.y, uz = q.z, w = q.w;
    const float tx = 2.0f * (uy * v[2] - uz * v[1]);
    const float ty = 2.0f * (uz * v[0] - ux * v[2]);
    const float tz = 2.0f * (ux * v[1] - uy * v[0]);
    o[0] = v[0] + w * tx + (uy * tz - uz * ty);
    o[1] = v[1] + w * ty + (uz * tx - ux * tz);
    o[2] = v[2] + w * tz + (ux * ty - uy * tx);
}

float ReticleQuatAngleDeg(const XrQuaternionf& a, const XrQuaternionf& b)
{
    float d = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    d = d > 1.0f ? 1.0f : d;
    return 2.0f * acosf(d) * 57.2957795f;
}

// fire_reticle_3d: where the gun dot goes this frame. The game camera's frame (right/up/back) is the frame the game image
// is drawn in, and that image is shown at the DECLARED eye poses - so the dot = declared centre + declared orientation *
// the camera-frame vector: the same transform the scene gets. The yaw-mapping placement (aimhook's XR<->game yaw, real
// head position) is computed alongside ONLY to be logged against it.
// ---- fire_reticle_3d art: dlvr\reticle_art.bin (tools/make_reticle_art.py, from the player's own hud_dw atlas, INV-280)
struct ReticleImg { char name[32]; int w = 0, h = 0; ID3D11ShaderResourceView* srv = nullptr; };
ReticleImg g_art[16];
int        g_artN = 0;
bool       g_artTried = false;
const char* g_artWhy = "not loaded";

void LoadReticleArt()
{
    if (g_artTried) { return; }
    g_artTried = true;
    char path[MAX_PATH] = {};
    if (!DlvrFilePath("reticle_art.bin", path, sizeof(path))) { g_artWhy = "dlvr path unresolved"; }
    FILE* f = nullptr;
    if (path[0] && fopen_s(&f, path, "rb") == 0 && f) {
        char magic[4] = {};
        unsigned ver = 0, count = 0;
        if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "DLRA", 4) == 0 && fread(&ver, 4, 1, f) == 1 && ver == 1 &&
            fread(&count, 4, 1, f) == 1) {
            for (unsigned i = 0; i < count && g_artN < 16; ++i) {
                ReticleImg& im = g_art[g_artN];
                unsigned short wh[2] = {};
                if (fread(im.name, 1, 32, f) != 32 || fread(wh, 2, 2, f) != 2) { break; }
                im.name[31] = 0;
                im.w = wh[0];
                im.h = wh[1];
                std::vector<unsigned char> px(static_cast<size_t>(im.w) * im.h * 4);
                if (fread(px.data(), 1, px.size(), f) != px.size()) { break; }
                D3D11_TEXTURE2D_DESC td{};
                td.Width = im.w; td.Height = im.h; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA sd{ px.data(), static_cast<UINT>(im.w * 4), 0 };
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(g.device->CreateTexture2D(&td, &sd, &tex)) && tex) {
                    g.device->CreateShaderResourceView(tex, nullptr, &im.srv);
                    tex->Release();
                }
                if (im.srv) { ++g_artN; }
            }
            g_artWhy = g_artN ? "ok" : "file read but no texture created";
        } else {
            g_artWhy = "bad header (not DLRA v1)";
        }
        fclose(f);
    } else if (path[0]) {
        g_artWhy = "dlvr\\reticle_art.bin missing - run tools/make_reticle_art.py";
    }
    // [PROBE: FIRERETICLE3D] gate=fire_reticle_3d  site=FLAT  log-only=yes
    LOG("overlay: [FIRERETICLE3D] crosshair art: %d image(s) from %s (%s)%s", g_artN, path[0] ? path : "?", g_artWhy,
        g_artN ? "" : " - *** the 3D reticle falls back to the red dot ***");
}

const ReticleImg* Art(const char* name)
{
    for (int i = 0; i < g_artN; ++i) {
        if (strcmp(g_art[i].name, name) == 0) { return &g_art[i]; }
    }
    return nullptr;
}

GunDot   g_curDot;          // this frame's dot, for the drawing below
float    g_curHalfDeg = 3.0f;   // the canvas's angular half-extent this frame
// bow_reticle_3d (option 2): the shaft dot's angles from the quad's centre (deg, +x right, +y DOWN - DrawGunCrosshair's P()),
// seen from the declared eye centre. Valid only when g_curShaftOk.
float    g_curShaftAx = 0.0f, g_curShaftAy = 0.0f;
bool     g_curShaftOk = false;
constexpr float kShaftDotPx = 8.0f;   // the shaft dot's size (design px) - smaller than G_Dot's 12, a choice

// Parts per crosshair group, from hud.xui (INV-280/281): image, displayed W x H (design px), which of Up/Down/Left/Right.
struct XhGroup { const char* img; float w, h; bool up, down, left, right; };
const XhGroup kXhGroups[6] = {
    { nullptr, 0, 0, false, false, false, false },                   // 0 none
    { "crosshair_rifle", 12, 6, false, true, true, true },           // 1 Pistol (SMGs are ItemType_Pistol)
    { "crosshair_rifle", 12, 6, true, true, true, true },            // 2 Rifle
    { "crosshair_shotgun", 12, 28, false, false, true, true },       // 3 Shotgun
    { "crosshair_bow_2", 14, 14, false, false, true, true },         // 4 Bow (hud.xui G_Bow: Left/Right 14x14, INV-496)
    { "crosshair_crossbow", 14, 14, true, true, true, true },        // 5 Crossbow
};
constexpr float kXhGapPx = 3.0f;   // inner end's rest distance from the centre (design px) - a choice, not read from the game

float DegPerDesignPx(const GunDot& d)
{
    const float fov = (d.fov > 10.0f && d.fov < 170.0f) ? d.fov : 90.0f;
    return atanf(tanf(fov * 0.5f * 0.0174532925f) / 360.0f) * 57.2957795f * FireReticleScale;
}

bool GunDotPlacement(XrPosef& pose, float& size)
{
    GunDot gd;
    if (!GetGunDot(gd)) {
        ++g_gw.noDot;
        return false;
    }
    const float* loc = gd.local;
    const float* goff = gd.gameOff;
    const float dist = gd.dist;
    const bool hit = gd.hit;
    const unsigned age = gd.ageMs;
    g_curDot = gd;
    if (!g_declValid) {
        ++g_gw.noDecl;
        return false;
    }
    const XrQuaternionf& q = g_declL.orientation;
    const float c[3] = { (g_declL.position.x + g_declR.position.x) * 0.5f,
                         (g_declL.position.y + g_declR.position.y) * 0.5f,
                         (g_declL.position.z + g_declR.position.z) * 0.5f };
    float v[3];
    ReticleRotate(q, loc, v);
    const float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(vl) || vl < 0.05f) {
        ++g_gw.noDot;
        return false;
    }
    pose.position = { c[0] + v[0], c[1] + v[1], c[2] + v[2] };
    float qx[3] = {}, qy[3] = {}, qz[3] = {};   // the quad's right / up / toward-the-eyes axes (bow shaft dot)
    bool qAxes = false;
    // Face the eyes: quad +Z toward the declared centre, its up as close to the view's up as possible (look-at).
    {
        float z[3] = { -v[0] / vl, -v[1] / vl, -v[2] / vl };
        const float up0[3] = { 0.0f, 1.0f, 0.0f };
        float up[3];
        ReticleRotate(q, up0, up);
        float x[3] = { up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0] };
        float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (xl < 1e-4f) { pose.orientation = q; }
        else {
            for (int k = 0; k < 3; ++k) { x[k] /= xl; }
            const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
            for (int k = 0; k < 3; ++k) { qx[k] = x[k]; qy[k] = y[k]; qz[k] = z[k]; }
            qAxes = true;
            // rotation matrix columns x, y, z -> quaternion
            const float m00 = x[0], m11 = y[1], m22 = z[2];
            const float tr = m00 + m11 + m22;
            XrQuaternionf r;
            if (tr > 0.0f) {
                const float s = sqrtf(tr + 1.0f) * 2.0f;
                r.w = 0.25f * s; r.x = (y[2] - z[1]) / s; r.y = (z[0] - x[2]) / s; r.z = (x[1] - y[0]) / s;
            } else if (m00 > m11 && m00 > m22) {
                const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
                r.w = (y[2] - z[1]) / s; r.x = 0.25f * s; r.y = (y[0] + x[1]) / s; r.z = (z[0] + x[2]) / s;
            } else if (m11 > m22) {
                const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
                r.w = (z[0] - x[2]) / s; r.x = (y[0] + x[1]) / s; r.y = 0.25f * s; r.z = (z[1] + y[2]) / s;
            } else {
                const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
                r.w = (x[1] - y[0]) / s; r.x = (z[0] + x[2]) / s; r.y = (z[1] + y[2]) / s; r.z = 0.25f * s;
            }
            pose.orientation = r;
        }
    }
    // Canvas half-angle: big enough for the opened crosshair and the hit marker; the quad spans it at the dot's distance.
    const float k = DegPerDesignPx(gd);
    const XhGroup& grp = kXhGroups[(gd.group >= 0 && gd.group < 6) ? gd.group : 0];
    const float reach = fmaxf(fmaxf(gd.s0, gd.s1), 0.0f) + (kXhGapPx + fmaxf(grp.w, grp.h)) * k;
    float half = fmaxf(fmaxf(reach, 10.0f * k) * 1.15f, 0.5f);
    // bow_reticle_3d (option 2): the shaft dot - its direction from the eye centre, as angles in the quad's frame. The canvas
    // grows to hold it (cap 30 deg; beyond that it is not drawn and counted). It is drawn ON the quad, i.e. at the landing
    // point's depth: the direction is exact, the depth cue is not.
    g_curShaftOk = false;
    if (gd.shaftOk && qAxes) {
        float sv[3];
        ReticleRotate(q, gd.shaft, sv);
        const float fw = -(sv[0] * qz[0] + sv[1] * qz[1] + sv[2] * qz[2]);
        if (fw > 1e-3f) {
            g_curShaftAx = atan2f(sv[0] * qx[0] + sv[1] * qx[1] + sv[2] * qx[2], fw) * 57.2957795f;
            g_curShaftAy = -atan2f(sv[0] * qy[0] + sv[1] * qy[1] + sv[2] * qy[2], fw) * 57.2957795f;
            const float need = (fmaxf(fabsf(g_curShaftAx), fabsf(g_curShaftAy)) + kShaftDotPx * k) * 1.15f;
            static bool s_shaftSaid = false, s_shaftOutSaid = false;
            if (need <= 30.0f) {
                half = fmaxf(half, need);
                g_curShaftOk = true;
                if (!s_shaftSaid) {
                    s_shaftSaid = true;
                    // [PROBE: BOWSHAFT] gate=bow_reticle_3d  site=HEADSET  log-only=yes
                    LOG("overlay: [BOWSHAFT] FIRST shaft dot drawn (logged unconditionally) | %.2f deg right, %.2f deg down of the "
                        "brackets' centre (the landing point) | canvas half-angle %.2f deg",
                        g_curShaftAx, g_curShaftAy, fminf(half, 30.0f));
                }
            } else {
                ++g_gw.shaftOut;
                if (!s_shaftOutSaid) {
                    s_shaftOutSaid = true;
                    // [PROBE: BOWSHAFT] gate=bow_reticle_3d  site=HEADSET  log-only=yes
                    LOG("overlay: [BOWSHAFT] FIRST shaft dot NOT drawn: %.2f / %.2f deg from the landing point needs a canvas past "
                        "the 30 deg cap (a very short draw). Logged once.", g_curShaftAx, g_curShaftAy);
                }
            }
        }
    }
    g_curHalfDeg = fminf(half, 30.0f);
    size = 2.0f * vl * tanf(g_curHalfDeg * 0.0174532925f);

    // ---- probe: the cross-check and the window
    ++g_gw.placed;
    if (hit) { ++g_gw.hits; }
    if (dist < g_gw.dMin) { g_gw.dMin = dist; }
    if (dist > g_gw.dMax) { g_gw.dMax = dist; }
    if (vl < g_gw.vMin) { g_gw.vMin = vl; }
    if (vl > g_gw.vMax) { g_gw.vMax = vl; }
    const float eyeRot = ReticleQuatAngleDeg(g_declL.orientation, g_declR.orientation);
    if (eyeRot > g_gw.eyeRotMax) { g_gw.eyeRotMax = eyeRot; }
    XrPosef head;
    float rel = 0.0f, crossDeg = -1.0f, headOff = -1.0f, declVsHead = -1.0f;
    if (GetHeadPose(head) && CamHookGetOpenXrToGameYaw(rel)) {
        const float cs = cosf(rel);
        const float sn = AimMirrorYaw ? -sinf(rel) : sinf(rel);
        // inverse of ComputeAbsoluteAim's XR->game yaw (game.x = x cos + z sin, game.z = -x sin + z cos)
        const float b[3] = { head.position.x + goff[0] * cs - goff[2] * sn - c[0], head.position.y + goff[1] - c[1],
                             head.position.z + goff[0] * sn + goff[2] * cs - c[2] };
        const float bl = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        if (bl > 1e-3f) {
            float dd = (v[0] * b[0] + v[1] * b[1] + v[2] * b[2]) / (vl * bl);
            dd = dd > 1.0f ? 1.0f : (dd < -1.0f ? -1.0f : dd);
            crossDeg = acosf(dd) * 57.2957795f;
            g_gw.crossSum += crossDeg;
            if (crossDeg > g_gw.crossMax) { g_gw.crossMax = crossDeg; }
            ++g_gw.crossN;
        }
        const float hx = c[0] - head.position.x, hy = c[1] - head.position.y, hz = c[2] - head.position.z;
        headOff = sqrtf(hx * hx + hy * hy + hz * hz);
        g_gw.headSum += headOff;
        if (headOff > g_gw.headMax) { g_gw.headMax = headOff; }
        ++g_gw.headN;
        declVsHead = ReticleQuatAngleDeg(q, head.orientation);
    } else {
        ++g_gw.noCross;
    }
    if (!g_gwFirstSaid) {
        g_gwFirstSaid = true;
        // [PROBE: FIRERETICLE3D] gate=fire_reticle_3d  site=HEADSET  log-only=yes
        LOG("overlay: [FIRERETICLE3D] FIRST 3D DOT (logged unconditionally) | %s at %.2f m from the muzzle, age %u ms | "
            "camera-frame (right %+.3f, up %+.3f, back %+.3f) | declared centre (%.3f, %.3f, %.3f), eyes %.3f deg apart | dot "
            "(%.3f, %.3f, %.3f) = %.2f m from the centre, quad %.3f m | cross-check vs yaw mapping %.2f deg (declared vs real "
            "head orientation %.2f deg, centre vs head position %.3f m; -1 = unavailable) | crosshair group %d dot %d spread "
            "%.3f/%.3f deg, FOV %.1f -> %.4f deg per design px x scale %.2f, canvas half %.2f deg",
            hit ? "hit" : "pin", dist, age, loc[0], loc[1], loc[2], c[0], c[1], c[2], eyeRot, pose.position.x,
            pose.position.y, pose.position.z, vl, size, crossDeg, declVsHead, headOff, gd.group, gd.dotVisible ? 1 : 0,
            gd.s0, gd.s1, gd.fov, k, FireReticleScale, g_curHalfDeg);
    }
    return true;
}

void GunDotWindow()
{
    const ULONGLONG now = GetTickCount64();
    if (g_gw.start == 0) { g_gw.start = now; }
    if (now - g_gw.start < 5000) { return; }
    if (g_gw.placed || g_gw.noDecl) {
        // [PROBE: FIRERETICLE3D] gate=fire_reticle_3d  site=HEADSET  log-only=yes
        LOG_PERIODIC("overlay: [FIRERETICLE3D] 5 s | dot placed %u frames (hit %u, pinned %u) | muzzle->dot %.2f-%.2f m, centre->dot "
            "%.2f-%.2f m | NOT placed: no fresh gun dot %u, no projection layer %u | cross-check vs yaw mapping mean %.2f "
            "max %.2f deg (n %u, unavailable %u) | declared centre vs head position mean %.3f max %.3f m | eye orientations "
            "apart max %.3f deg | bow shaft dot beyond the 30 deg canvas %u",
            g_gw.placed, g_gw.hits, g_gw.placed - g_gw.hits, g_gw.placed ? g_gw.dMin : 0.0f, g_gw.dMax,
            g_gw.placed ? g_gw.vMin : 0.0f, g_gw.vMax, g_gw.noDot, g_gw.noDecl,
            g_gw.crossN ? g_gw.crossSum / g_gw.crossN : 0.0, g_gw.crossMax, g_gw.crossN, g_gw.noCross,
            g_gw.headN ? g_gw.headSum / g_gw.headN : 0.0, g_gw.headMax, g_gw.eyeRotMax, g_gw.shaftOut);
    }
    g_gw = GunDotWin{};
    g_gw.start = now;
}

// Draws the game's crosshair for g_curDot into the square kPanelH region centred on the canvas. Angles map to the canvas
// through the tangent plane: u = tan(angle) / tan(g_curHalfDeg) * (kPanelH / 2). False = no art (caller draws the red dot).
bool DrawGunCrosshair(ImDrawList* dl, ImVec2 c)
{
    LoadReticleArt();
    if (g_artN == 0) { return false; }
    const GunDot& d = g_curDot;
    const float k = DegPerDesignPx(d);
    const float half = static_cast<float>(kPanelH) * 0.5f;
    const float tA = tanf(g_curHalfDeg * 0.0174532925f);
    auto P = [&](float ax, float ay) {   // angles (deg), +x right, +y DOWN
        return ImVec2(c.x + tanf(ax * 0.0174532925f) / tA * half, c.y + tanf(ay * 0.0174532925f) / tA * half);
    };
    auto tex = [](const ReticleImg* im) { return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(im->srv))); };
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    const XhGroup& grp = kXhGroups[(d.group >= 0 && d.group < 6) ? d.group : 0];
    const ReticleImg* part = grp.img ? Art(grp.img) : nullptr;
    if (part) {
        struct Dir { bool on; float dx, dy, spread; };
        // s0 = the shot's HORIZONTAL spread -> Left/Right, s1 = VERTICAL -> Up/Down (grappleaim ReadXhairState)
        const Dir dirs[4] = { { grp.up, 0, -1, d.s1 }, { grp.down, 0, 1, d.s1 }, { grp.left, -1, 0, d.s0 }, { grp.right, 1, 0, d.s0 } };
        for (const Dir& r : dirs) {
            if (!r.on) { continue; }
            const float inner = fmaxf(r.spread, 0.0f) + kXhGapPx * k, len = grp.w * k, thick = grp.h * k;
            const float px = -r.dy, py = r.dx;   // perpendicular = the image's +y (direction rotated +90 deg, y down)
            auto C = [&](float u, float v) {
                const float along = inner + u * len, across = (v - 0.5f) * thick;
                return P(r.dx * along + px * across, r.dy * along + py * across);
            };
            dl->AddImageQuad(tex(part), C(0, 0), C(1, 0), C(1, 1), C(0, 1), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                             ImVec2(0, 1), white);
        }
    }
    auto centred = [&](const ReticleImg* im, float wpx, float hpx, ImU32 col) {
        if (!im) { return; }
        dl->AddImage(tex(im), P(-wpx * 0.5f * k, -hpx * 0.5f * k), P(wpx * 0.5f * k, hpx * 0.5f * k), ImVec2(0, 0),
                     ImVec2(1, 1), col);
    };
    // Bow with its shaft dot (option 2): the brackets alone mark the landing point - no centre dot, so the only dot is the
    // shaft's and the two cannot be confused.
    const bool bowShaft = d.group == 4 && g_curShaftOk;
    if ((d.dotVisible || !part) && !bowShaft) {
        centred(Art("crosshair_dot"), 12, 12, white);
    }
    if (bowShaft) {
        if (const ReticleImg* im = Art("crosshair_dot")) {
            const float h = kShaftDotPx * 0.5f * k;
            dl->AddImage(tex(im), P(g_curShaftAx - h, g_curShaftAy - h), P(g_curShaftAx + h, g_curShaftAy + h), ImVec2(0, 0),
                         ImVec2(1, 1), white);
        }
    }
    // Hit marker: G_Hit's I_Part opacity timeline (hud.xui): 0 -> 1 at frame 1, 1 until 9, -> 0 at 17. Frame rate ASSUMED 30/s
    // ([UNKNOWN] - the UI timeline's fps was not read): 33 ms in, hold to 300 ms, out by 567 ms.
    const float t = static_cast<float>(d.hitAgeMs);
    const float a = t < 33.0f ? t / 33.0f : t < 300.0f ? 1.0f : t < 567.0f ? 1.0f - (t - 300.0f) / 267.0f : 0.0f;
    if (a > 0.0f && d.hitAgeMs != 0xFFFFFFFFu) {
        centred(Art("crosshair_hit"), 16, 16, IM_COL32(255, 255, 255, static_cast<int>(a * 255.0f)));
    }
    return true;
}

// The aim reticle, drawn in place of the settings panel when it's closed -
// see this file's top-of-namespace comment. Both share the one swapchain, so
// only one is ever submitted per frame.
// fire_reticle_3d takes it over while a gun is out: the dot then sits where the barrel's line meets the world
// (GunDotPlacement) instead of 3 m along the controller, and is drawn whether or not show_reticle is on.
bool RenderReticle(XrSpace appSpace, XrCompositionLayerQuad* out)
{
    if (appSpace == XR_NULL_HANDLE) {
        return false;
    }
    XrPosef gunPose{};
    float gunSize = 0.0f;
    const bool gun = FireReticle3d && GunDotPlacement(gunPose, gunSize);
    if (FireReticle3d) {
        GunDotWindow();
    }
    if (!gun && !g_showReticle) {
        return false;
    }
    XrPosef pose{};
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    if (!gun) {
        if (!GetControllerPose(kRightHand, pose)) {
            return false;   // not tracked yet - nothing sensible to draw
        }
        ReticleForwardFromPose(pose.orientation, fx, fy, fz);
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(g.swapchain, &ai, &index))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage_(g.swapchain, &wi))) {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage_(g.swapchain, &ri);
        return false;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    // A plain filled dot, not a menu - no FeedInput()/DrawUi(). Centred in
    // the canvas; the whole texture maps onto a small in-world quad (see
    // `size` below), so a dot most of the canvas's width reads as a small,
    // solid marker rather than a speck.
    const ImVec2 center(static_cast<float>(kPanelW) * 0.5f, static_cast<float>(kPanelH) * 0.5f);
    // The gun dot samples a SQUARE kPanelH x kPanelH region around the centre (below), so it stays round; the old
    // controller reticle keeps its full-canvas mapping unchanged.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!(gun && DrawGunCrosshair(dl, center))) {
        const float radius = static_cast<float>(kPanelW < kPanelH ? kPanelW : kPanelH) * (gun ? 0.04f : 0.18f);
        dl->AddCircleFilled(center, radius, IM_COL32(255, 40, 40, 235));
        dl->AddCircle(center, radius, IM_COL32(20, 20, 20, 235), 0, radius * 0.12f);
    }
    ImGui::Render();

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g.context->ClearRenderTargetView(g.rtvs[index], clear);
    g.context->OMSetRenderTargets(1, &g.rtvs[index], nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g.swapchain, &ri);

    *out = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = appSpace;   // world-locked: floats along the aim ray, not the head
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = g.swapchain;
    out->subImage.imageArrayIndex = 0;
    if (gun) {
        out->subImage.imageRect.offset = { (kPanelW - kPanelH) / 2, 0 };
        out->subImage.imageRect.extent = { kPanelH, kPanelH };
        out->pose = gunPose;
        out->size = { gunSize, gunSize };
        return true;
    }
    out->subImage.imageRect.offset = { 0, 0 };
    out->subImage.imageRect.extent = { kPanelW, kPanelH };
    // The controller's own orientation, unchanged - by construction its local
    // -Z already points from the controller toward the reticle position
    // (that's the same direction the reticle was placed along), exactly the
    // relationship the head-locked panel above already relies on: identity
    // orientation in view space, positioned further along view space's own
    // -Z. No separate billboard/look-at math needed.
    out->pose.orientation = pose.orientation;
    out->pose.position.x = pose.position.x + fx * g_reticleDistance;
    out->pose.position.y = pose.position.y + fy * g_reticleDistance;
    out->pose.position.z = pose.position.z + fz * g_reticleDistance;
    // Scales gently with distance so it doesn't loom huge up close or vanish
    // far away - not physically meaningful, just legible.
    const float size = g_reticleDistance * 0.02f;
    out->size = { size, size };
    return true;
}

// The L3 + R3 hold bar while the menu is CLOSED (Jay 2026-11-11): a small head-locked strip where the panel opens, drawn into
// the top-left (bar + 40) x 100 of the panel's swapchain - hold_bar / hold_bar_fill (Jay's 440 x 40) or code-drawn, on a dark
// backing.
// COST: only while both clicks are held (at most kMenuHoldSec): one ImGui frame + one quad, like the reticle it replaces.
bool RenderHoldBar(XrSpace viewSpace, XrCompositionLayerQuad* out)
{
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(g.swapchain, &ai, &index))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage_(g.swapchain, &wi))) {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage_(g.swapchain, &ri);
        return false;
    }
    if (g_menuArtN == 0) { MenuArtRefresh(); }   // the very first hold: the bar's own art, before the menu has ever opened
    // The strip fits the bar at its own size (hold_bar.png - Jay's is 440 x 40) + 20 px each side; 100 tall (text above it).
    const MenuArt* holdArt = ArtOf(kArtHoldBg);
    const int barW = holdArt ? holdArt->w : static_cast<int>(kHoldBarW);
    const int kW = (std::min)(barW + 40, kPanelW), kH = 100;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(kW), static_cast<float>(kH)), IM_COL32(10, 12, 16, 210), 10.0f);
    DrawHoldBar(dl, ImVec2(20.0f, 48.0f), "Keep holding L3 + R3 to open the menu", false);
    ImGui::Render();

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g.context->ClearRenderTargetView(g.rtvs[index], clear);
    g.context->OMSetRenderTargets(1, &g.rtvs[index], nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g.swapchain, &ri);

    const float width = OverlayWidth() * static_cast<float>(kW) / static_cast<float>(kPanelW);
    *out = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = viewSpace;   // head-locked, where the panel will open
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = g.swapchain;
    out->subImage.imageArrayIndex = 0;
    out->subImage.imageRect.offset = { 0, 0 };
    out->subImage.imageRect.extent = { kW, kH };
    out->pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
    out->pose.position = { 0.0f, 0.0f, -OverlayDistance() };
    out->size = { width, width * static_cast<float>(kH) / static_cast<float>(kW) };
    return true;
}

namespace {
// The declared-view placement of GunDotPlacement, for any rendered-view-frame vector: position + a look-at orientation
// (quad +Z toward the declared eye centre, up as near the view's up as possible). False = no projection layer / degenerate.
bool PlaceFacingEyes(const float loc[3], XrPosef& pose, float& vl)
{
    if (!g_declValid) { return false; }
    const XrQuaternionf& q = g_declL.orientation;
    const float c[3] = { (g_declL.position.x + g_declR.position.x) * 0.5f, (g_declL.position.y + g_declR.position.y) * 0.5f,
                         (g_declL.position.z + g_declR.position.z) * 0.5f };
    float v[3];
    ReticleRotate(q, loc, v);
    vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(vl) || vl < 0.05f) { return false; }
    pose.position = { c[0] + v[0], c[1] + v[1], c[2] + v[2] };
    float z[3] = { -v[0] / vl, -v[1] / vl, -v[2] / vl };
    const float up0[3] = { 0.0f, 1.0f, 0.0f };
    float up[3];
    ReticleRotate(q, up0, up);
    float x[3] = { up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0] };
    const float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (xl < 1e-4f) { pose.orientation = q; return true; }
    for (int k = 0; k < 3; ++k) { x[k] /= xl; }
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    const float tr = x[0] + y[1] + z[2];
    XrQuaternionf r;
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        r.w = 0.25f * s; r.x = (y[2] - z[1]) / s; r.y = (z[0] - x[2]) / s; r.z = (x[1] - y[0]) / s;
    } else if (x[0] > y[1] && x[0] > z[2]) {
        const float s = sqrtf(1.0f + x[0] - y[1] - z[2]) * 2.0f;
        r.w = (y[2] - z[1]) / s; r.x = 0.25f * s; r.y = (y[0] + x[1]) / s; r.z = (z[0] + x[2]) / s;
    } else if (y[1] > z[2]) {
        const float s = sqrtf(1.0f + y[1] - x[0] - z[2]) * 2.0f;
        r.w = (z[0] - x[2]) / s; r.x = (y[0] + x[1]) / s; r.y = 0.25f * s; r.z = (z[1] + y[2]) / s;
    } else {
        const float s = sqrtf(1.0f + z[2] - x[0] - y[1]) * 2.0f;
        r.w = (x[1] - y[0]) / s; r.x = (z[0] + x[2]) / s; r.y = (z[1] + y[2]) / s; r.z = 0.25f * s;
    }
    pose.orientation = r;
    return true;
}

struct GrappleWin { ULONGLONG start = 0; unsigned can = 0, refused = 0, none = 0, noDecl = 0; bool firstSaid = false; };
GrappleWin g_gpw;
}  // namespace

// grapple_reticle_3d: the game's grapple icon ("go" = can grapple, "no" = refused) on its own quad at the grapple target.
bool OverlayRenderGrapple(XrSpace appSpace, XrCompositionLayerQuad* out)
{
    if (!Grapple3dReticle || !g.initialised || g.visible || !out || appSpace == XR_NULL_HANDLE ||
        g.swapchain2 == XR_NULL_HANDLE) {
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_gpw.start == 0) { g_gpw.start = now; }
    if (now - g_gpw.start >= 5000) {
        if (g_gpw.can + g_gpw.refused + g_gpw.noDecl) {
            // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=HEADSET  log-only=yes
            LOG_PERIODIC("overlay: [GRAPPLE3D] 5 s | icon drawn: can grapple %u, refused %u | nothing to draw %u | no projection layer %u",
                g_gpw.can, g_gpw.refused, g_gpw.none, g_gpw.noDecl);
        }
        const bool said = g_gpw.firstSaid;
        g_gpw = GrappleWin{};
        g_gpw.start = now;
        g_gpw.firstSaid = said;
    }
    GrappleDot gd;
    if (!GetGrappleDot(gd)) {
        ++g_gpw.none;
        return false;
    }
    XrPosef pose{};
    float vl = 0.0f;
    if (!PlaceFacingEyes(gd.local, pose, vl)) {
        ++g_gpw.noDecl;
        return false;
    }
    LoadReticleArt();
    const ReticleImg* img = Art(gd.state == 1 ? "crosshair_grapple_go" : "crosshair_grapple_no");
    if (!img) {
        return false;
    }
    // hud.xui: the icon image is 36 design px tall; the art is 90 x 54, so 60 px wide at that height.
    const float fov = (gd.fov > 10.0f && gd.fov < 170.0f) ? gd.fov : 90.0f;
    const float k = atanf(tanf(fov * 0.5f * 0.0174532925f) / 360.0f) * 57.2957795f * GrappleReticleScale;
    const float hw = 30.0f * k, hh = 18.0f * k;   // half extents (deg)
    if (!(hw > 0.0f)) {
        return false;   // scale 0 = hidden
    }
    const float halfDeg = fmaxf(hw, hh) * 1.1f;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(g.swapchain2, &ai, &index))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage_(g.swapchain2, &wi))) {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage_(g.swapchain2, &ri);
        return false;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    const ImVec2 cc(static_cast<float>(kPanelW) * 0.5f, static_cast<float>(kPanelH) * 0.5f);
    const float half = static_cast<float>(kPanelH) * 0.5f;
    const float tA = tanf(halfDeg * 0.0174532925f);
    const float px = tanf(hw * 0.0174532925f) / tA * half, py = tanf(hh * 0.0174532925f) / tA * half;
    ImGui::GetBackgroundDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(img->srv))),
                                             ImVec2(cc.x - px, cc.y - py), ImVec2(cc.x + px, cc.y + py));
    ImGui::Render();
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g.context->ClearRenderTargetView(g.rtvs2[index], clear);
    g.context->OMSetRenderTargets(1, &g.rtvs2[index], nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g.swapchain2, &ri);

    *out = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = appSpace;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = g.swapchain2;
    out->subImage.imageArrayIndex = 0;
    out->subImage.imageRect.offset = { (kPanelW - kPanelH) / 2, 0 };
    out->subImage.imageRect.extent = { kPanelH, kPanelH };
    out->pose = pose;
    const float size = 2.0f * vl * tanf(halfDeg * 0.0174532925f);
    out->size = { size, size };
    if (gd.state == 1) { ++g_gpw.can; } else { ++g_gpw.refused; }
    if (!g_gpw.firstSaid) {
        g_gpw.firstSaid = true;
        // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=HEADSET  log-only=yes
        LOG("overlay: [GRAPPLE3D] FIRST 3D GRAPPLE ICON (logged unconditionally) | %s at %.2f m (camera) / %.2f m (eye centre) | "
            "icon %.2f x %.2f deg (FOV %.1f, scale %.2f) | quad %.3f m",
            gd.state == 1 ? "can grapple (go)" : "refused (no)", gd.dist, vl, 2.0f * hw, 2.0f * hh, fov, GrappleReticleScale, size);
    }
    return true;
}

namespace {
// The calibration screen's quads (panel, build A; floor, build B): one ImGui frame drawn into `sc`, then a quad in `space`.
// `square` = use the centred kPanelH x kPanelH part of the image (the floor). COST per call: one ImGui frame + one quad.
// `ownList` (holster sphere, place-by-hand Task 4): draw into that caller-owned ImDrawList and render it directly, WITHOUT
// ImGui::NewFrame / Render - for a quad drawn in the SAME submit as the open settings panel. A second ImGui frame there
// would leave the panel's window un-submitted for one frame: ImGui 1.92.8 NewFrame (imgui.cpp:5752) then moves nav focus
// off it, Begin re-"appears" it (7465/7830), an active slider not kept alive is cleared, and io.DeltaTime would be counted
// twice (nav / key repeat at 2x). nullptr = the old path, unchanged. COST with ownList: one draw-list fill + one quad.
bool RenderCalibQuadInto(XrSwapchain& sc, std::vector<XrSwapchainImageD3D11KHR>& images,
                         std::vector<ID3D11RenderTargetView*>& rtvs, bool& tried, const char* what, XrSpace space,
                         const XrPosef& pose, float sizeM, bool square, OverlayDrawFn draw, const void* user,
                         XrCompositionLayerQuad* out, ImDrawList* ownList = nullptr, const char* refusedTail = nullptr)
{
    if (!g.initialised || !out || !draw || space == XR_NULL_HANDLE) { return false; }
    // The draw-list shared data (white-pixel UV, clip rect, AA flags) is filled by the first NewFrame; none yet = nothing.
    if (ownList && ImGui::GetFrameCount() <= 0) { return false; }
    if (sc == XR_NULL_HANDLE) {
        if (tried) { return false; }
        tried = true;
        if (!CreateSwapchainInto(sc, images, rtvs, what)) {
            // [PROBE: CALIBSCR] gate=none (the calibration screen)  site=HEADSET  log-only=yes
            // refusedTail (holster sphere, place-by-hand Task 4 fix round 1): the caller's own consequence text; nullptr = the
            // calibration screen's, so its two callers print exactly what they always did. One-time (the `tried` flag above).
            LOG("overlay: [CALIBSCR] *** the %s swapchain could not be created - it is NOT drawn this session (%s) ***", what,
                refusedTail ? refusedTail : "the screen's flow still runs and B still skips");
            sc = XR_NULL_HANDLE;
            return false;
        }
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(sc, &ai, &index))) { return false; }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage_(sc, &wi))) {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage_(sc, &ri);
        return false;
    }
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (ownList) {
        // imgui.h:3306 "If you want to create ImDrawList instances, pass them ImGui::GetDrawListSharedData()"; the reset +
        // texture + clip push is what GetViewportBgFgDrawList (imgui.cpp:5235-5237) does for the background list each frame.
        // Textures = nullptr: no atlas update outside a frame (imgui.h:3463 allows it; the shapes only use the white pixel).
        static ImDrawData dd;
        ownList->_ResetForNewFrame();
        ownList->PushTexture(ImGui::GetIO().Fonts->TexRef);
        ownList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(kPanelW), static_cast<float>(kPanelH)), false);
        draw(ownList, static_cast<float>(kPanelW), static_cast<float>(kPanelH), user);
        dd.Clear();
        dd.Valid = true;
        dd.DisplayPos = ImVec2(0.0f, 0.0f);
        dd.DisplaySize = ImVec2(static_cast<float>(kPanelW), static_cast<float>(kPanelH));
        dd.FramebufferScale = ImVec2(1.0f, 1.0f);
        dd.AddDrawList(ownList);
        g.context->ClearRenderTargetView(rtvs[index], clear);
        g.context->OMSetRenderTargets(1, &rtvs[index], nullptr);
        ImGui_ImplDX11_RenderDrawData(&dd);
    } else {
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        draw(ImGui::GetBackgroundDrawList(), static_cast<float>(kPanelW), static_cast<float>(kPanelH), user);
        ImGui::Render();
        g.context->ClearRenderTargetView(rtvs[index], clear);
        g.context->OMSetRenderTargets(1, &rtvs[index], nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(sc, &ri);

    *out = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = space;
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = sc;
    out->subImage.imageArrayIndex = 0;
    if (square) {
        out->subImage.imageRect.offset = { (kPanelW - kPanelH) / 2, 0 };
        out->subImage.imageRect.extent = { kPanelH, kPanelH };
        out->size = { sizeM, sizeM };
    } else {
        out->subImage.imageRect.offset = { 0, 0 };
        out->subImage.imageRect.extent = { kPanelW, kPanelH };
        out->size = { sizeM, sizeM * static_cast<float>(kPanelH) / static_cast<float>(kPanelW) };
    }
    out->pose = pose;
    return true;
}
}  // namespace

// Calibration screen (build A plan task 4). COST: nothing until the screen opens (the caller tests CalibScreenActive first);
// while open, one ImGui frame + one quad per submit - the grapple quad's own pattern above.
bool OverlayRenderCalibQuad(XrSpace viewSpace, const XrPosef& pose, float sizeM, OverlayDrawFn draw, const void* user,
                            XrCompositionLayerQuad* out)
{
    return RenderCalibQuadInto(g.swapchain3, g.images3, g.rtvs3, g.calibTried, "calibration screen", viewSpace, pose, sizeM,
                               false, draw, user, out);
}

// Build B: the floor under the calibration void - same mechanics, its own swapchain, a SQUARE quad in `floorSpace`.
bool OverlayRenderCalibFloor(XrSpace floorSpace, const XrPosef& pose, float sizeM, OverlayDrawFn draw, const void* user,
                             XrCompositionLayerQuad* out)
{
    return RenderCalibQuadInto(g.swapchain4, g.images4, g.rtvs4, g.floorTried, "calibration floor", floorSpace, pose, sizeM,
                               true, draw, user, out);
}

namespace {
// The holster sphere's picture, in the square's own pixels (square mode = the centred h x h part of the w x h image): a
// translucent disc filling the square + a solid ring at its edge. Premultiplied by ImGui's SRC_ALPHA blend onto the cleared
// (0,0,0,0) target, the same as every other quad here. COST: two circle fills on one draw list (no ImGui frame).
void DrawHolsterSphere(void* drawList, float w, float h, const void*)
{
    ImDrawList* dl = static_cast<ImDrawList*>(drawList);
    const ImVec2 c(w * 0.5f, h * 0.5f);
    const float ringPx = 4.0f;
    const float r = h * 0.5f - ringPx * 0.5f - 1.0f;   // ring centred 1 px inside the square's edge, so its AA fringe is not cut
    dl->AddCircleFilled(c, r, IM_COL32(235, 240, 255, 64), 0);   // alpha 64/255 = 0.25
    dl->AddCircle(c, r, IM_COL32(235, 240, 255, 230), 0, ringPx);  // alpha 230/255 = 0.9
}
}  // namespace

// Holster place-by-hand (plan 2026-10-30 Task 4): the selected zone (g_holsterShowZone, set by the Holsters page) as a square
// quad 2 x radius wide at HolsterZoneWorld's point, turned to face the head - a disc that faces the eye reads as that sphere
// from any side. Drawn while the panel is open on that zone, and while placing (the panel itself is then skipped, OverlayRender).
// Ruling R10: drawn wherever HolsterZoneWorld says, a stale body-locked zone included. False = nothing to submit.
// COST: g_holsterShowZone < 0 = one int test. On the page: one bool test (g.visible), two GetHeadPose, one HolsterZoneWorld
// (Task 3: up to two HolsterHipCentre + one shared SRW lock), one HolsterZoneParams, an atan2/asin/4 sin-cos, one swapchain
// acquire / wait / release, one draw-list fill (no ImGui frame - see RenderCalibQuadInto's ownList), one quad.
bool OverlayRenderHolsterSphere(XrSpace appSpace, XrCompositionLayerQuad* out)
{
    if (g_holsterShowZone < 0) { return false; }
    // Not with the panel shut: the page is gone (Task 5 should also reset the zone; this keeps a stale value from drawing),
    // and the calibration screen forces g.visible false (overlay.cpp OverlayPoll), so its layers never meet this one.
    if (!g.initialised || !g.visible || !out || appSpace == XR_NULL_HANDLE) { return false; }
    const int zone = g_holsterShowZone;
    float centre[3], local[3], radius = 0.0f;
    if (!HolsterZoneParams(zone, local, &radius, nullptr) || !(radius > 0.0f)) { return false; }
    if (!HolsterZoneWorld(zone, centre)) { return false; }   // no head pose yet
    XrPosef head{};
    if (!GetHeadPose(head)) { return false; }
    XrPosef pose{};
    pose.position = { centre[0], centre[1], centre[2] };
    // Billboard, yaw + pitch only (no roll): the quad's VISIBLE face is its +Z side (OpenXR quad layer), so +Z must point from
    // the quad to the head. q = Ry(yaw) * Rx(pitch) maps +Z to (cos p sin y, -sin p, cos p cos y) = d with yaw = atan2(dx, dz),
    // pitch = -asin(dy); its +Y keeps a non-negative up component (no roll). Same handedness as the grapple quad's
    // PlaceFacingEyes (z = toward the eye centre, x = up x z, y = z x x): for d = +X both give a +90 deg turn about +Y.
    float d[3] = { head.position.x - centre[0], head.position.y - centre[1], head.position.z - centre[2] };
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(len)) { return false; }
    if (len < 1e-3f) {
        pose.orientation = head.orientation;   // the eye at the centre: no direction to face - any facing is the same
    } else {
        for (int k = 0; k < 3; ++k) { d[k] /= len; }
        const float dy = d[1] < -1.0f ? -1.0f : (d[1] > 1.0f ? 1.0f : d[1]);
        const float yaw = atan2f(d[0], d[2]);
        const float pitch = -asinf(dy);
        const float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f), sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
        pose.orientation = { cy * sp, sy * cp, -sy * sp, cy * cp };   // (x, y, z, w) of Ry(yaw) * Rx(pitch)
    }
    if (!g.sphereList) { g.sphereList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData()); }
    return RenderCalibQuadInto(g.swapchain8, g.images8, g.rtvs8, g.sphereTried, "holster sphere", appSpace, pose,
                               2.0f * radius, true, DrawHolsterSphere, nullptr, out, g.sphereList,
                               "no holster sphere is shown; the Holsters page and placing by hand still work");
}

namespace {
// A holster ring, option B (plan 2026-11-20 Task 5; renders backups/20261120_holster_ring_renders): a faint disc + an outline, the
// sphere's colour, both scaled by the ring's alpha (*user = float 0..1). Drawn into the centred square (h x h) of the canvas.
void DrawHolsterRing(void* drawList, float w, float h, const void* user)
{
    const float a = user ? *static_cast<const float*>(user) : 1.0f;
    ImDrawList* dl = static_cast<ImDrawList*>(drawList);
    const ImVec2 c(w * 0.5f, h * 0.5f);
    const float line = std::max(2.0f, holster::ring::kLineFrac * h * 0.5f);
    const float r = h * 0.5f - line * 0.5f - 1.0f;
    const auto A = [a](float k) { return static_cast<int>(std::min(255.0f, std::max(0.0f, k * a * 255.0f + 0.5f))); };
    dl->AddCircleFilled(c, r, IM_COL32(235, 240, 255, A(holster::ring::kFill)), 0);
    dl->AddCircle(c, r, IM_COL32(235, 240, 255, A(holster::ring::kOutline)), 0, line);
}
}  // namespace

// The visible holster rings (plan 2026-11-20-holster-pivot-posture-rings Task 5; trace 20261120_1300_holster_rings): up to maxOut quads
// in appSpace at holster.cpp HolsterRingTargets' centres (the grab's own frame), 2 x radius wide, facing the head like the sphere.
// None while holster_rings is off, the panel is open or a placement sphere shows, or HolsterRingsAllowed() says no. COST with no hand
// near a holster: a few distance tests in HolsterRingTargets; per visible ring one ImGui frame + one quad (the grapple quad's path).
int OverlayRenderHolsterRings(XrSpace appSpace, XrCompositionLayerQuad* out, int maxOut)
{
    if (!HolsterRings || maxOut <= 0 || !out || appSpace == XR_NULL_HANDLE) { return 0; }
    if (!g.initialised || g.visible || g_holsterShowZone >= 0) { return 0; }
    if (!HolsterRingsAllowed()) { return 0; }
    float centre[3][3], radius[3], alpha[3];
    const int n = HolsterRingTargets(centre, radius, alpha, maxOut < 3 ? maxOut : 3);
    if (n <= 0) { return 0; }
    XrPosef head{};
    if (!GetHeadPose(head)) { return 0; }
    static float s_alpha[3];   // the draw callback's user data must outlive the call; render thread only
    int k = 0;
    for (int i = 0; i < n; ++i) {
        XrPosef pose{};
        pose.position = { centre[i][0], centre[i][1], centre[i][2] };
        float d[3] = { head.position.x - centre[i][0], head.position.y - centre[i][1], head.position.z - centre[i][2] };
        const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!std::isfinite(len)) { continue; }
        if (len < 1e-3f) {
            pose.orientation = head.orientation;
        } else {
            for (int q = 0; q < 3; ++q) { d[q] /= len; }   // the sphere's billboard: +Z of the quad toward the head, no roll
            const float dy = d[1] < -1.0f ? -1.0f : (d[1] > 1.0f ? 1.0f : d[1]);
            const float yaw = atan2f(d[0], d[2]);
            const float pitch = -asinf(dy);
            const float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f), sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
            pose.orientation = { cy * sp, sy * cp, -sy * sp, cy * cp };
        }
        s_alpha[i] = alpha[i];
        if (RenderCalibQuadInto(g.swapchainRing[i], g.imagesRing[i], g.rtvsRing[i], g.ringTried[i], "holster ring", appSpace, pose,
                                2.0f * radius[i], true, DrawHolsterRing, &s_alpha[i], &out[k], nullptr,
                                "that holster ring is not shown; the holsters still work")) {
            ++k;
        }
    }
    static bool s_saidFirst = false;
    if (k > 0 && !s_saidFirst) {
        s_saidFirst = true;
        // [PROBE: HOLSTERRING] gate=holster_rings  site=HEADSET  log-only=no
        LOG("overlay: [HOLSTERRING] FIRST (logged unconditionally) - %d ring(s) drawn, the nearest at (%.3f, %.3f, %.3f) r %.3f alpha %.2f | "
            "option B: fill %.0f %%, outline %.0f %%, fade over %.2f m", k, centre[0][0], centre[0][1], centre[0][2], radius[0], alpha[0],
            holster::ring::kFill * 100.0f, holster::ring::kOutline * 100.0f, holster::ring::kFadeM);
    }
    return k;
}

namespace {
// title.png at the canvas's top-left, at its own size (clamped to the canvas).
void DrawMenuTitle(void* drawList, float w, float h, const void* user)
{
    const MenuArt* t = static_cast<const MenuArt*>(user);
    ImDrawList* dl = static_cast<ImDrawList*>(drawList);
    ArtDraw(dl, t, ImVec2(0.0f, 0.0f), ImVec2((std::min)(static_cast<float>(t->w), w), (std::min)(static_cast<float>(t->h), h)));
}

// THE MENU HELD IN THE WORLD (Jay 2026-11-24: "I'd like the mod menu to be held in world" -> "Level, build it"; trace
// docs/traces/20261124_1855_menu_world_lock.md). At open the head is located once in the app space (the room); the panel is placed
// overlay_distance ahead of the head's LEVEL facing, at head height, turned about +y only to face it, and stays there while open.
// Cleared on close (OverlayRender), so every open places it again; a new app space (recenter) re-latches - the calibration panel's
// pattern (calibscreen.cpp). Not located yet -> the old head-locked pose that frame, retried every submit, said once per open.
// COST: one xrLocateSpace per submit until latched (the first frame of an open), then a few multiplies; closed = one bool test.
struct MenuAnchor {
    bool set = false;
    bool missSaid = false;
    XrSpace space = XR_NULL_HANDLE;
    XrVector3f head{};
    float fx = 0.0f, fz = -1.0f;   // the level facing (unit, horizontal)
};
MenuAnchor g_menuAnchor;

void MenuAnchorClear()
{
    g_menuAnchor.set = false;
    g_menuAnchor.missSaid = false;
    g_menuAnchor.space = XR_NULL_HANDLE;
}

// The world pose of a point at (ox, oy) on the panel's plane (panel centre = 0, 0), facing the head; false = not anchored (use
// viewSpace). Latches on the first call of an open (the title runs first in a frame, the panel second - both see one anchor).
bool MenuWorldPose(XrSpace viewSpace, XrSpace appSpace, XrTime time, float ox, float oy, XrPosef* pose)
{
    if (appSpace == XR_NULL_HANDLE || viewSpace == XR_NULL_HANDLE || !xrLocateSpace_) { return false; }
    if (!g_menuAnchor.set || g_menuAnchor.space != appSpace) {
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (!XR_SUCCEEDED(xrLocateSpace_(viewSpace, appSpace, time, &loc)) || (loc.locationFlags & need) != need) {
            if (!g_menuAnchor.missSaid) {
                g_menuAnchor.missSaid = true;
                // [PROBE: MENUANCHOR] gate=none (every mod-menu open)  site=HEADSET  log-only=yes
                LOG("overlay: [MENUANCHOR] head NOT located this frame - the menu is HEAD-LOCKED until it is (retried every submit)");
            }
            return false;
        }
        const XrQuaternionf& q = loc.pose.orientation;
        // forward = q * (0, 0, -1); keep its horizontal part. Straight up / down (no horizontal part) -> tracking forward.
        float fx = -2.0f * (q.x * q.z + q.w * q.y);
        float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        const float len = std::sqrt(fx * fx + fz * fz);
        if (len > 1e-3f) { fx /= len; fz /= len; } else { fx = 0.0f; fz = -1.0f; }
        const bool relatch = g_menuAnchor.set;
        g_menuAnchor.set = true;
        g_menuAnchor.space = appSpace;
        g_menuAnchor.head = loc.pose.position;
        g_menuAnchor.fx = fx;
        g_menuAnchor.fz = fz;
        // [PROBE: MENUANCHOR] gate=none (every mod-menu open)  site=HEADSET  log-only=yes
        LOG("overlay: [MENUANCHOR] menu %s in the room: head (%+.3f, %+.3f, %+.3f) m, level facing (%+.3f, %+.3f) = %+.1f deg off "
            "tracking forward, panel centre %.2f m ahead at head height", relatch ? "RE-PLACED (the app space changed - recenter)" :
            "PLACED", g_menuAnchor.head.x, g_menuAnchor.head.y, g_menuAnchor.head.z, fx, fz,
            static_cast<double>(std::atan2(-fx, -fz) * 57.29578f), static_cast<double>(OverlayDistance()));
    }
    // The panel faces the head: its +z (towards the viewer) = -facing, i.e. a rotation about +y by th = atan2(-fx, -fz).
    const float th = std::atan2(-g_menuAnchor.fx, -g_menuAnchor.fz);
    const float c = std::cos(th), s = std::sin(th);
    const float d = OverlayDistance();
    // panel centre + R_y(th) * (ox, oy, 0) = (ox c, oy, -ox s)
    pose->position = { g_menuAnchor.head.x + g_menuAnchor.fx * d + ox * c, g_menuAnchor.head.y + oy,
                       g_menuAnchor.head.z + g_menuAnchor.fz * d - ox * s };
    pose->orientation = { 0.0f, std::sin(th * 0.5f), 0.0f, std::cos(th * 0.5f) };
    return true;
}
}  // namespace

// THE MENU TITLE, OUTSIDE THE PANEL (Jay 2026-11-11: "For the title though, I'd like it to sit above the menu, top left side, out
// of the actual panel above it"). title.png on its own head-locked quad: left edge on the panel's left edge, bottom kTitleGapPx
// above the panel's top, at the panel's own pixel scale (OverlayWidth() / kPanelW metres per pixel), so it reads as part of it.
// Only while the panel is drawn (not while a holster placement hides it). Drawn into its own ImDrawList with no ImGui frame - the
// same path as the holster sphere, safe in the panel's submit (RenderCalibQuadInto's ownList comment). No title.png = no quad
// (the panel shows the text title instead). COST: menu closed = two bool tests; open = one tiny draw-list fill + one quad.
bool OverlayRenderTitle(XrSpace viewSpace, XrSpace appSpace, XrTime time, XrCompositionLayerQuad* out)
{
    if (!g.initialised || !g.visible || AnyPlacing() || !out || viewSpace == XR_NULL_HANDLE) { return false; }
    const MenuArt* t = ArtOf(kArtTitle);
    if (!t) { return false; }
    constexpr float kTitleGapPx = 8.0f;
    const int tw = (std::min)(t->w, kPanelW), th = (std::min)(t->h, kPanelH);
    const float mpp = OverlayWidth() / static_cast<float>(kPanelW);   // metres per panel pixel
    const float panelH = OverlayWidth() * static_cast<float>(kMenuH) / static_cast<float>(kPanelW);   // the menu quad's height
    const float ox = -OverlayWidth() * 0.5f + tw * mpp * 0.5f;              // the title's centre on the panel's plane
    const float oy = panelH * 0.5f + (kTitleGapPx + th * 0.5f) * mpp;
    XrPosef pose{};
    pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
    pose.position = { ox, oy, -OverlayDistance() };
    XrSpace space = viewSpace;                                             // head-locked until the menu anchor latches
    XrPosef world{};
    if (MenuWorldPose(viewSpace, appSpace, time, ox, oy, &world)) {        // held in the room with the panel
        space = appSpace;
        pose = world;
    }
    if (!g.titleList) { g.titleList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData()); }
    if (!RenderCalibQuadInto(g.swapchain9, g.images9, g.rtvs9, g.titleTried, "menu title", space, pose, OverlayWidth(), false,
                             DrawMenuTitle, t, out, g.titleList, "the menu title is not shown above the panel")) {
        return false;
    }
    // Only the title's rectangle of the canvas, at the panel's scale.
    out->subImage.imageRect.offset = { 0, 0 };
    out->subImage.imageRect.extent = { tw, th };
    out->size = { tw * mpp, th * mpp };
    return true;
}

// Build D task 5: the floor from Jay's 2048 x 2048 art. The swapchain is created ONCE at the source's size; every submit is
// acquire -> wait -> CopyResource -> release. COST per call while the screen is up at the main menu: one 2048 x 2048 RGBA8
// GPU-to-GPU copy (16 MB) + one quad; nothing while the screen is closed (the caller tests that first).
namespace {
// The image path shared by the floor (build D task 5) and the void's walls / ceiling (Jay 2026-10-16): `sc` is created ONCE at
// `src`'s size; one acquire -> wait -> CopyResource -> release per call, then `n` square quads from that one image, one per
// pose (the four walls share one copy). `what` names it in the log; `fallback` says what is drawn instead after a refusal.
bool RenderCalibImageInto(XrSwapchain& sc, std::vector<XrSwapchainImageD3D11KHR>& images,
                          std::vector<ID3D11RenderTargetView*>& rtvs, bool& tried, const char* what, const char* fallback,
                          XrSpace space, const XrPosef* poses, int n, float sizeM, ID3D11Texture2D* src,
                          XrCompositionLayerQuad* out)
{
    if (!g.initialised || !out || !src || !poses || n <= 0 || space == XR_NULL_HANDLE) { return false; }
    if (sc == XR_NULL_HANDLE) {
        if (tried) { return false; }
        tried = true;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        // [PROBE: CALIBART] gate=none (the calibration screen floor image)  site=HEADSET  log-only=yes
        LOG("overlay: [CALIBART] %s source: %ux%u format %d mips %u array %u", what, sd.Width, sd.Height,
            static_cast<int>(sd.Format), sd.MipLevels, sd.ArraySize);
        if (sd.MipLevels != 1 || sd.ArraySize != 1 || sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            // [PROBE: CALIBART] gate=none (the calibration screen floor image)  site=HEADSET  log-only=yes
            LOG("overlay: [CALIBART] *** %s path REFUSED: source is %ux%u format %d mips %u array %u (need "
                "R8G8B8A8_UNORM_SRGB, 1 mip, 1 slice) - %s this session ***",
                what, sd.Width, sd.Height, static_cast<int>(sd.Format), sd.MipLevels, sd.ArraySize, fallback);
            return false;
        }
        bool ok = CreateSwapchainInto(sc, images, rtvs, what, static_cast<int>(sd.Width), static_cast<int>(sd.Height), false);
        D3D11_TEXTURE2D_DESC rd{};
        if (ok && !images.empty() && images[0].texture) {
            images[0].texture->GetDesc(&rd);
            // [PROBE: CALIBART] gate=none (the calibration screen floor image)  site=HEADSET  log-only=yes
            LOG("overlay: [CALIBART] %s runtime texture: %ux%u format %d mips %u array %u, %u images", what, rd.Width,
                rd.Height, static_cast<int>(rd.Format), rd.MipLevels, rd.ArraySize, static_cast<unsigned>(images.size()));
            if (rd.Width != sd.Width || rd.Height != sd.Height || rd.MipLevels != 1 || rd.ArraySize != 1 ||
                (rd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && rd.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS)) {
                ok = false;
            }
        } else {
            ok = false;
        }
        if (!ok) {
            // [PROBE: CALIBART] gate=none (the calibration screen floor image)  site=HEADSET  log-only=yes
            LOG("overlay: [CALIBART] *** %s path REFUSED: swapchain not created or incompatible (runtime texture "
                "%ux%u format %d mips %u array %u vs source %ux%u) - %s this session ***",
                what, rd.Width, rd.Height, static_cast<int>(rd.Format), rd.MipLevels, rd.ArraySize, sd.Width, sd.Height, fallback);
            if (sc != XR_NULL_HANDLE && xrDestroySwapchain_) { xrDestroySwapchain_(sc); }
            sc = XR_NULL_HANDLE;
            images.clear();
            return false;
        }
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(sc, &ai, &index))) { return false; }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage_(sc, &wi))) {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage_(sc, &ri);
        return false;
    }
    if (index < images.size()) { g.context->CopyResource(images[index].texture, src); }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(sc, &ri);

    D3D11_TEXTURE2D_DESC cd{};
    src->GetDesc(&cd);
    for (int k = 0; k < n; ++k) {
        out[k] = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        out[k].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        out[k].space = space;
        out[k].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        out[k].subImage.swapchain = sc;
        out[k].subImage.imageArrayIndex = 0;
        out[k].subImage.imageRect.offset = { 0, 0 };
        out[k].subImage.imageRect.extent = { static_cast<int32_t>(cd.Width), static_cast<int32_t>(cd.Height) };
        out[k].size = { sizeM, sizeM };
        out[k].pose = poses[k];
    }
    return true;
}
}  // namespace

bool OverlayRenderCalibFloorImage(XrSpace floorSpace, const XrPosef& pose, float sizeM, ID3D11Texture2D* src,
                                  XrCompositionLayerQuad* out)
{
    return RenderCalibImageInto(g.swapchain5, g.images5, g.rtvs5, g.floorImgTried, "calibration floor image",
                                "the 800 px grid floor is used", floorSpace, &pose, 1, sizeM, src, out);
}

bool OverlayRenderCalibVoidImage(bool ceiling, XrSpace floorSpace, const XrPosef* poses, int n, float sizeM,
                                 ID3D11Texture2D* src, XrCompositionLayerQuad* out)
{
    if (ceiling) {
        return RenderCalibImageInto(g.swapchain7, g.images7, g.rtvs7, g.ceilingImgTried, "calibration void ceiling",
                                    "no ceiling is drawn", floorSpace, poses, n, sizeM, src, out);
    }
    return RenderCalibImageInto(g.swapchain6, g.images6, g.rtvs6, g.wallImgTried, "calibration void walls",
                                "no walls are drawn", floorSpace, poses, n, sizeM, src, out);
}

// True once the image path was tried and refused (or its swapchain failed): the caller then uses the ImGui grid floor.
bool OverlayCalibFloorImageRefused()
{
    return g.floorImgTried && g.swapchain5 == XR_NULL_HANDLE;
}

void OverlaySetDeclaredViews(bool valid, const XrPosef& left, const XrPosef& right)
{
    g_declValid = valid;
    g_declL = left;
    g_declR = right;
}

bool OverlayRender(XrSpace viewSpace, XrSpace appSpace, XrTime displayTime,
                   XrCompositionLayerQuad* out)
{
    if (!g.initialised || !out || viewSpace == XR_NULL_HANDLE) {
        return false;
    }
    if (!g.visible) {
        if (g_menuAnchor.set || g_menuAnchor.missSaid) { MenuAnchorClear(); }   // the next open places the menu again
        // The 2 s L3 + R3 hold to OPEN: its bar replaces the reticle for those 2 s (one swapchain, one quad).
        if (g.holdProgress > 0.0f) {
            return RenderHoldBar(viewSpace, out);
        }
        return RenderReticle(appSpace, out);
    }
    // Holster place-by-hand, spec goal 2.4 "the menu hides": while a placement waits for the trigger the PANEL quad is not
    // submitted (no swapchain image acquired, no ImGui frame, no mirror draw - the caller adds no layer). g.visible stays true,
    // so every OverlayVisible() input block holds and the grapple icon / fire reticle stay off (Task 1 G2/G3). The holster
    // sphere is still drawn (OverlayRenderHolsterSphere). COST: one volatile read per submit while the panel is open.
    if (AnyPlacing()) {   // + a wrist piece being placed (3b) - the same hidden panel
        return false;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(g.swapchain, &ai, &index))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage_(g.swapchain, &wi))) {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage_(g.swapchain, &ri);
        return false;
    }

    ServiceCapture();

    ImGui_ImplDX11_NewFrame();
    FeedInput(ImGui::GetIO());
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();

    // ImGui's DX11 backend saves and restores the whole pipeline state around
    // its draw, so borrowing the immediate context here does not disturb the
    // engine's. This runs on the render thread inside ovr_SubmitFrame, which
    // the thread logging established is a single thread.
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g.context->ClearRenderTargetView(g.rtvs[index], clear);
    g.context->OMSetRenderTargets(1, &g.rtvs[index], nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // The other ImGui frames (reticle, hold bar, calibration screen) keep the size they always had; the mirror pass below uses
    // the draw data's own DisplaySize, not this.
    ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(kPanelW), static_cast<float>(kPanelH));

    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g.swapchain, &ri);

    const float width = OverlayWidth();
    const float height = width * static_cast<float>(kMenuH) / static_cast<float>(kPanelW);

    *out = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    out->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->space = viewSpace;   // head-locked only until the menu anchor latches (the first frame of an open) - then held in the room
    out->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    out->subImage.swapchain = g.swapchain;
    out->subImage.imageArrayIndex = 0;
    out->subImage.imageRect.offset = { 0, 0 };
    out->subImage.imageRect.extent = { kPanelW, kMenuH };
    out->pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
    out->pose.position = { 0.0f, 0.0f, -OverlayDistance() };
    XrPosef world{};
    if (MenuWorldPose(viewSpace, appSpace, displayTime, 0.0f, 0.0f, &world)) {   // Jay 2026-11-24: "Level, build it"
        out->space = appSpace;
        out->pose = world;
    }
    out->size = { width, height };
    return true;
}

namespace {
// Desktop mirror, one eye (deskmirror.cpp; trace docs/traces/20261114_1030_desktop_mirror_single_eye.md): its own draw list, as the
// holster sphere's, created on first use and freed in OverlayShutdown before the ImGui context.
ImDrawList* g_mirrorList = nullptr;
// The eye image is opaque picture, not UI: blending it over the mirror's last picture would mix two frames wherever its alpha is
// below 1. This draw callback runs inside the backend's state backup, so the blend state comes back after the draw.
void MirrorOpaqueBlend(const ImDrawList*, const ImDrawCmd*)
{
    if (g.context) { g.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu); }
}
// The menu panel's image is premultiplied (the bootstrap's lay-on blend): over the window's picture it is ONE / INV_SRC_ALPHA, the way
// the compositor lays the panel quad over the eye (trace 20261114_1125). Created once, released in OverlayShutdown.
ID3D11BlendState* g_mirrorPremulBlend = nullptr;
void MirrorPremulBlend(const ImDrawList*, const ImDrawCmd*)
{
    if (g.context && g_mirrorPremulBlend) {
        const float f[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g.context->OMSetBlendState(g_mirrorPremulBlend, f, 0xffffffffu);
    }
}
}  // namespace

int OverlayDrawImageToTarget(ID3D11RenderTargetView* rtv, float w, float h, ID3D11ShaderResourceView* srv, const float uv[4],
                             bool premultiplied)
{
    if (premultiplied && !g_mirrorPremulBlend && g.device) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(g.device->CreateBlendState(&bd, &g_mirrorPremulBlend))) { g_mirrorPremulBlend = nullptr; }
    }
    if (premultiplied && !g_mirrorPremulBlend) { return 3; }
    if (!g.initialised || !g.context || !rtv || !srv || !uv || !(w > 0.0f) || !(h > 0.0f)) { return 1; }
    // Before the overlay's first ImGui frame (RUN DM1: no picture until the mod menu had been opened once) the backend has no shaders /
    // states yet - ImGui_ImplDX11_NewFrame creates them only when missing and starts no ImGui frame; the next real NewFrame skips it.
    // The draw list's shared data needs nothing from a frame for one AddImage with its own clip (trace 20261114_1110).
    if (ImGui::GetFrameCount() <= 0) { ImGui_ImplDX11_NewFrame(); }
    if (!g_mirrorList) { g_mirrorList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData()); }
    static ImDrawData dd;
    g_mirrorList->_ResetForNewFrame();
    const ImTextureRef tex(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(srv)));
    g_mirrorList->PushTexture(tex);
    g_mirrorList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(w, h), false);
    g_mirrorList->AddCallback(premultiplied ? MirrorPremulBlend : MirrorOpaqueBlend, nullptr);
    g_mirrorList->AddImage(tex, ImVec2(0.0f, 0.0f), ImVec2(w, h), ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]), IM_COL32_WHITE);
    dd.Clear();
    dd.Valid = true;
    dd.DisplayPos = ImVec2(0.0f, 0.0f);
    dd.DisplaySize = ImVec2(w, h);
    dd.FramebufferScale = ImVec2(1.0f, 1.0f);
    dd.AddDrawList(g_mirrorList);
    g.context->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(&dd);
    return 0;
}

void OverlayRenderToMirror(ID3D11Texture2D* dst)
{
    // Draws the same ImGui frame a second time, into the desktop mirror.
    //
    // The panel lives on its own OpenXR quad layer, which the compositor puts in
    // front of your eyes and which the flat window never sees - so without this
    // the overlay is invisible to a screenshot. ImGui's draw data stays valid
    // until the next NewFrame, so rendering it twice into two targets is free.
    if (!g.initialised || !g.visible || !dst || !g.context) {
        return;
    }
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) {
        return;
    }

    // One RTV per texture, cached: the mirror is created once and reused.
    static ID3D11Texture2D* cachedTex = nullptr;
    static ID3D11RenderTargetView* cachedRtv = nullptr;
    if (cachedTex != dst) {
        if (cachedRtv) {
            cachedRtv->Release();
            cachedRtv = nullptr;
        }
        D3D11_TEXTURE2D_DESC td{};
        dst->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = td.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.device->CreateRenderTargetView(dst, &rd, &cachedRtv))) {
            LOG("overlay: mirror RTV failed (format %d)", static_cast<int>(td.Format));
            cachedTex = nullptr;
            return;
        }
        cachedTex = dst;
        LOG("overlay: compositing into the %ux%u desktop mirror", td.Width, td.Height);
    }
    if (!cachedRtv) {
        return;
    }

    // Scale the panel up to fill the mirror before drawing it there.
    //
    // It used to render at 1:1 - 1100x760 in the corner of a 6144x3264 texture,
    // which the flat window then shrinks again. The result was unreadable, which
    // matters because the desktop view is how the tool gets screenshotted and
    // discussed. ImGui's backend takes its viewport and its projection from the
    // draw data, so scaling the vertices and telling it the mirror's size is
    // enough; nothing is restored afterwards because the list is rebuilt every
    // frame, and the headset pass has already used it by this point.
    D3D11_TEXTURE2D_DESC td{};
    dst->GetDesc(&td);
    const float mw = static_cast<float>(td.Width);
    const float mh = static_cast<float>(td.Height);
    const float sx = mw / static_cast<float>(kPanelW);
    const float sy = mh / static_cast<float>(kMenuH);   // the menu's height (this pass runs only while the menu is up)
    const float scale = (sx < sy ? sx : sy) * 0.95f;
    const float offX = (mw - kPanelW * scale) * 0.5f;
    const float offY = (mh - kMenuH * scale) * 0.5f;

    for (int n = 0; n < dd->CmdListsCount; ++n) {
        ImDrawList* list = dd->CmdLists[n];
        for (int v = 0; v < list->VtxBuffer.Size; ++v) {
            ImDrawVert& vert = list->VtxBuffer[v];
            vert.pos.x = vert.pos.x * scale + offX;
            vert.pos.y = vert.pos.y * scale + offY;
        }
        for (int c = 0; c < list->CmdBuffer.Size; ++c) {
            ImDrawCmd& cmd = list->CmdBuffer[c];
            cmd.ClipRect.x = cmd.ClipRect.x * scale + offX;
            cmd.ClipRect.y = cmd.ClipRect.y * scale + offY;
            cmd.ClipRect.z = cmd.ClipRect.z * scale + offX;
            cmd.ClipRect.w = cmd.ClipRect.w * scale + offY;
        }
    }
    dd->DisplayPos = ImVec2(0.0f, 0.0f);
    dd->DisplaySize = ImVec2(mw, mh);

    g.context->OMSetRenderTargets(1, &cachedRtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(dd);
}

void OverlayShutdown()
{
    if (!g.initialised) {
        return;
    }
    if (g.sphereList) {   // the holster sphere's draw list points at this context's shared data: free it first
        IM_DELETE(g.sphereList);
        g.sphereList = nullptr;
    }
    if (g_mirrorList) {   // the desktop mirror's draw list: same reason
        IM_DELETE(g_mirrorList);
        g_mirrorList = nullptr;
    }
    if (g_mirrorPremulBlend) { g_mirrorPremulBlend->Release(); g_mirrorPremulBlend = nullptr; }
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    for (auto* r : g.rtvs) {
        if (r) {
            r->Release();
        }
    }
    g.rtvs.clear();
    if (g.swapchain != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain);
        g.swapchain = XR_NULL_HANDLE;
    }
    for (auto* r : g.rtvs2) {
        if (r) {
            r->Release();
        }
    }
    g.rtvs2.clear();
    if (g.swapchain2 != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain2);
        g.swapchain2 = XR_NULL_HANDLE;
    }
    for (auto* r : g.rtvs3) {
        if (r) {
            r->Release();
        }
    }
    g.rtvs3.clear();
    if (g.swapchain3 != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain3);
        g.swapchain3 = XR_NULL_HANDLE;
    }
    for (auto* r : g.rtvs4) {
        if (r) {
            r->Release();
        }
    }
    g.rtvs4.clear();
    if (g.swapchain4 != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain4);
        g.swapchain4 = XR_NULL_HANDLE;
    }
    for (auto* r : g.rtvs8) {   // the holster sphere's (place-by-hand Task 4), same as rtvs3 / rtvs4
        if (r) {
            r->Release();
        }
    }
    g.rtvs8.clear();
    if (g.swapchain8 != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain8);
        g.swapchain8 = XR_NULL_HANDLE;
    }
    for (int i = 0; i < 3; ++i) {   // the holster rings (plan 2026-11-20 Task 5), same as rtvs8 / swapchain8
        for (auto* r : g.rtvsRing[i]) {
            if (r) { r->Release(); }
        }
        g.rtvsRing[i].clear();
        if (g.swapchainRing[i] != XR_NULL_HANDLE && xrDestroySwapchain_) {
            xrDestroySwapchain_(g.swapchainRing[i]);
            g.swapchainRing[i] = XR_NULL_HANDLE;
        }
    }
    for (auto* r : g.rtvs9) {   // the menu title's quad (OverlayRenderTitle), same as rtvs8
        if (r) {
            r->Release();
        }
    }
    g.rtvs9.clear();
    if (g.swapchain9 != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain9);
        g.swapchain9 = XR_NULL_HANDLE;
    }
    for (int i = 0; i < g_menuArtN; ++i) {   // the menu art (MenuArtRefresh) holds textures on g.device
        if (g_menuArt[i].srv) {
            g_menuArt[i].srv->Release();
            g_menuArt[i].srv = nullptr;
            g_menuArt[i].mt = FILETIME{};
        }
    }
    if (g.swapchain5 != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain5);
        g.swapchain5 = XR_NULL_HANDLE;
    }
    g.images5.clear();
    if (g.swapchain6 != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain6);
        g.swapchain6 = XR_NULL_HANDLE;
    }
    g.images6.clear();
    if (g.swapchain7 != XR_NULL_HANDLE && xrDestroySwapchain_) {
        xrDestroySwapchain_(g.swapchain7);
        g.swapchain7 = XR_NULL_HANDLE;
    }
    g.images7.clear();
    if (g.context) {
        g.context->Release();
        g.context = nullptr;
    }
    if (g.device) {
        g.device->Release();
        g.device = nullptr;
    }
    g.initialised = false;
}

}  // namespace dlvr
