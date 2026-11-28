// Physical crouch - plan 2026-10-27-roomscale-physical-crouch Task 9 (amended after Task 1's checkpoint). See physcrouch.h.
//
// WHAT IT USES (FINDINGS-ROOMSCALE-STATIC-2026-10-27 S4 + addenda 1-2, OUR gamedll 97046bba, Steam rvas):
//   the head: NeckPivotReportedPos(GetHeadPose()).y - the neck point in appSpace (LOCAL), so looking down at the floor is not a
//     crouch (Jay's ruling 1: no STAGE space; crouch_logic's Learner learns the standing level). A height jump > kJumpM between
//     two polls < kJumpS apart is a recentre, not a body: the Learner re-seeds (logged).
//   the input: our Data3 inputs_pad.scr binds _ACTION_DUCK (the game's HOLD crouch, action 15) to EJoy__BUTTON_16 = 0x0F0F, a code
//     the real pad never produces (engine +2FB300). It is sent through the InputManager [GameDI + 0xC8] (GameDI = qword
//     gamedll+0x1C16348), vtable +0x4A0 = slot 148 gamedll+0x110FD80 (manager, pad, code, pressed) - the game's own pad entry,
//     the one the engine's delivery (+2FB780) calls. NEVER slot 146 (+110FA70, the keyboard entry: it switches the device kind).
//     The pad index is the manager's own +292 (sub_1811115D0 stores the last pad there, a1[73]; +280 = 1 for a pad, hs26) - a
//     press is refused by name unless the last device was a pad. The class name is checked once per manager pointer
//     (RTTI, undecorated "InputManager") and the slot is compared with +110FD80 before every call.
//   the thread: this runs inside the scePad poll = the engine's pad-delivery thread (engine +2FB820), before that frame's own pad
//     events reach slot 148 (S4 addendum 2) - no second thread touches the manager.
//   the game: the applied duck factor vis+1332 (INV-621), published by EyePointApply each player camera update; sprinting =
//     *(u8*)(*(PlayerDI + 0xBA0) + 72) (MoveController::IsSprinting's byte, gamedll +D4A6F0 shape), PlayerDI =
//     *(*(*(GameDI + 1344) + 184) + 88).
// A HOLD: Crouch presses 0x0F0F and keeps it down, Stand releases it - the game crouches while action 15 reads 1.0 (MoveController
// update +D47F20). The press is held exactly while crouch_logic's Arb::fromHead is set.
// NOT ESTABLISHED: the Epic / GOG struct offsets +280 / +292 / +1332 / +0xBA0 / +72 (read from the Steam build only); whether a
//   0x0F0F press disturbs the pad's other bindings (it sets device kind 1 = pad, which every pad press sets anyway). The FLAT
//   test (diag_physcrouch_cycle) answers the press -> crouch edge end to end.
#include "physcrouch.h"

#include "crouch_logic.h"
#include "headpose.h"
#include "rtticam.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_WINDOW - the [PHYSCROUCH] 5 s line (2026-11-08 log trim)

// The Epic (EGL) build: the GameDI global and slot 148 are on tools/egl_allowlist.tsv (families 10-11). The DEV tree has no
// buildrva.h: Steam rvas only (any other build refuses by name at the slot compare).
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_PC_GD(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_PC_GD(rva) (rva)
#endif

namespace dlvr {

// [DEP: physical_crouch_enable] required-for=physical crouch (PhysicalCrouchEnable) - a real crouch holds the game's hold crouch
//      through EJoy__BUTTON_16 (plan Task 9; spec §2.3, Jay: "The character should crouch in-game when his head drops low enough").
//      tree=LOADBEARING while 1 (the scePad poll sends 0x0F0F to the InputManager; EyePointApply publishes the duck factor; while
//      the game is crouched ovr_GetTrackingState reports the head up to kGameDropM higher - PhysCrouchReportLiftY, option A, so
//      the engine does not crush the spine; each edge also re-places the HUD panel at the head when ui_hud_follow is on -
//      PhysCrouchEdges). Needs our Data3 inputs_pad.scr's _ACTION_DUCK / EJoy__BUTTON_16 line - without
//      it the press binds nothing and the game never crouches. HEADSET (diag_physcrouch_cycle tests the input FLAT). DEFAULT ON
//      2026-10-27 - Tier A (Jay verified: run 7 "Much better"; ticked "Physical crouch" at session end).
bool  PhysicalCrouchEnable = true;
// [DEP: physical_crouch_on_drop_m] required-for=the head drop below the learned standing level that crouches (crouch_logic
//      HystStep's ON point; Jay: "A bit higher, so it doesn't flicker"). tree=INERT at physical_crouch_enable=0. Tier D.
float PhysicalCrouchOnDropM = 0.35f;
// [DEP: physical_crouch_off_drop_m] required-for=the head drop above which a head crouch stands back up (HystStep's OFF point; must
//      be smaller than the ON drop - ValidDrops - or physical crouch refuses by name). tree=INERT at physical_crouch_enable=0. Tier D.
float PhysicalCrouchOffDropM = 0.25f;
// [DEP: physical_crouch_slide] required-for=a physical crouch during a sprint sends the crouch, so the game slides (crouch::Decide's
//      allowSlide; Jay 2026-11-15: "PLAYER CROUCH TRIGGERS SLIDE - TOGGLE OPTION IN VR COMFORT", "IT WAS ALREADY CAPABLE OF
//      TRIGGERING SLIDE BEFOREHAND, WE MADE IT SO IT DIDN'T"). Off = no crouch until the sprint ends (spec "No slide"). Live: read
//      every poll. tree=INERT at physical_crouch_enable=0. HEADSET. DEFAULT OFF - Tier C, Jay: "Off". Trace 20261115_1605.
bool  PhysicalCrouchSlide = false;
// [DEP: diag_physcrouch_cycle] required-for=[PHYSCROUCH] the FLAT test's synthetic head (no headset): standing 1.70 m for 8 s,
//      crouched 1.20 m for 4 s, repeating, in place of the head's height. tree=PROBE-ONLY (feeds the real decision and the real
//      press - physical_crouch_enable must be 1 too). FLAT. DEFAULT OFF - never on in the headset.
bool  DiagPhyscrouchCycle = false;

namespace {

constexpr uintptr_t kGameDiRva   = 0x1C16348;   // gamedll qword_181C16348 (GameDI)
constexpr uintptr_t kJoyBtnRva   = 0x110FD80;   // gamedll InputManager slot 148 (OnInputJoyButton shape, rs17)
constexpr uintptr_t kMgrOff      = 0xC8;        // [GameDI + 0xC8] = the InputManager (S4 addendum 2)
constexpr uintptr_t kSlot148     = 0x4A0;
constexpr uintptr_t kKindOff     = 280;         // last device kind: 0 keyboard, 1 pad (sub_1811115D0 a1[70])
constexpr uintptr_t kPadOff      = 292;         // last pad index, -1 after a keyboard event (a1[73])
constexpr int       kCode        = 0x0F0F;      // EJoy__BUTTON_16 (data/inputenums.def)
constexpr float     kJumpM       = 0.10f;       // a head that moves this far between two close polls was recentred
constexpr double    kJumpS       = 0.10;
constexpr double    kGameStaleS  = 0.5;         // the duck reading is too old to act on after this
constexpr float     kCycleStandM = 1.70f, kCycleCrouchM = 1.20f;
constexpr double    kCycleStandS = 8.0, kCycleCrouchS = 4.0;

using JoyBtnFn = void(__fastcall*)(void* mgr, int pad, int code, unsigned char pressed);

// Pad thread only.
crouch::Learner g_learn;
crouch::Hyst    g_hyst;
crouch::Arb     g_arb;
bool       g_armed = false, g_refusedDrops = false, g_held = false, g_haveH = false, g_failSaid = false;
int        g_heldPad = -1;
LARGE_INTEGER g_qpf{}, g_lastQpc{};
float      g_lastH = 0.0f;
double     g_lastHs = 0.0;
uintptr_t  g_gd = 0, g_mgrChecked = 0;
bool       g_mgrOk = false;
double     g_cycleT0 = -1.0;
ULONGLONG  g_winMs = 0;
long       g_lines = 0;
constexpr long kMaxLines = 1200;   // INV-9: edges + 5 s lines; the cap guards a forgotten switch
// window (5 s line)
long       w_polls = 0, w_noHead = 0, w_noGame = 0, w_menu = 0, w_sprintHeld = 0, w_reseed = 0, w_crouch = 0, w_stand = 0,
           w_refused = 0;
float      w_hMin = 1e9f, w_hMax = -1e9f;
const char* g_lastRefusal = "none";

// Game thread -> pad thread (and the probe); pad thread -> the head report (the learned standing neck height).
SRWLOCK    g_lock = SRWLOCK_INIT;
float      g_duck = -1.0f;
ULONGLONG  g_duckMs = 0;
float      g_standPub = 0.0f;
bool       g_standPubOk = false;
// The head report's lift (option A): the engine's job workers -> the pad thread's 5 s line.
volatile LONG g_liftCalls = 0, g_liftMaxMm = 0;
// Pad thread -> the HUD panel (the OpenXR submit thread): crouch / stand edges sent (PhysCrouchEdges).
volatile LONG g_edges = 0;
// Climbing round 4 (tester 2026-11-28: a climb-up into a spot with only crouched room "will send me to the normal standing height, and
// my head will go above the ceiling, it should leave me crouched"): climbhand asks for the crouch to be HELD until this tick (it
// renews it while the spot over the feet has no standing room). Any thread -> pad thread. Held whether or not physical crouch is on.
volatile ULONGLONG g_forceUntilMs = 0;
bool g_forcedHeld = false;   // the press held is the force's alone (physical crouch off, or the head standing) - for the log
// The game's own crouch drop of the spine / eye at duck 1 (INV-621 duck_run1 HSPINE1 -0.786..-0.836 m; flat runs 5b-5d eye bone
// 0.813-0.818 m). The report is lifted by at most this x the duck factor.
constexpr float kGameDropM = 0.80f;

double Now()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<double>(q.QuadPart) / static_cast<double>(g_qpf.QuadPart);
}

bool RdQ(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdI(uintptr_t a, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdB(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const unsigned char*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallJoy(uintptr_t fn, uintptr_t mgr, int pad, unsigned char pressed)
{
    __try {
        reinterpret_cast<JoyBtnFn>(fn)(reinterpret_cast<void*>(mgr), pad, kCode, pressed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// sprinting: *(u8*)(*(PlayerDI + 0xBA0) + 72). -1 unread.
int ReadSprint()
{
    uintptr_t gdi = 0, a = 0, b = 0, player = 0, move = 0;
    unsigned char s = 0;
    if (RdQ(g_gd + DLVR_PC_GD(kGameDiRva), &gdi) && gdi && RdQ(gdi + 1344, &a) && a && RdQ(a + 184, &b) && b &&
        RdQ(b + 88, &player) && player && RdQ(player + 0xBA0, &move) && move && RdB(move + 72, &s)) {
        return s ? 1 : 0;
    }
    return -1;
}

// The press / release. Returns false with g_lastRefusal set (named on the edge line).
bool Send(bool pressed)
{
    uintptr_t gdi = 0, mgr = 0, vt = 0, fn = 0;
    if (!RdQ(g_gd + DLVR_PC_GD(kGameDiRva), &gdi) || !gdi || !RdQ(gdi + kMgrOff, &mgr) || !mgr) {
        g_lastRefusal = "no InputManager at [GameDI + 0xC8]";
        return false;
    }
    if (mgr != g_mgrChecked) {   // once per manager pointer: the class name (an edge-only read, never per poll)
        g_mgrChecked = mgr;
        char name[128] = {};
        // RttiClassNameOf returns the UNDECORATED name ("InputManager", run 5a 2026-10-27 - the decorated compare refused it).
        g_mgrOk = RttiClassNameOf(mgr, name, sizeof(name)) && std::strcmp(name, "InputManager") == 0;
        // [PROBE: PHYSCROUCH] gate=physical_crouch_enable  site=FLAT  log-only=no
        LOG("[PHYSCROUCH] InputManager %p class %s - %s", reinterpret_cast<void*>(mgr), name[0] ? name : "(unreadable)",
            g_mgrOk ? "OK, the crouch is sent through its pad entry" : "NOT InputManager - REFUSED, no crouch is sent");
    }
    if (!g_mgrOk) { g_lastRefusal = "the [GameDI + 0xC8] object is not an InputManager"; return false; }
    if (!RdQ(mgr, &vt) || !RdQ(vt + kSlot148, &fn) || fn != g_gd + DLVR_PC_GD(kJoyBtnRva)) {
        // the expected target is gamedll +0x110FD80 on Steam / its Epic translation - kept out of the string (BETA ships it readable)
        g_lastRefusal = "the input manager's pad-button entry is not this build's (Steam or Epic)";
        return false;
    }
    int pad = g_heldPad;
    if (pressed) {
        int kind = -1;
        pad = -1;
        if (!RdI(mgr + kKindOff, &kind) || !RdI(mgr + kPadOff, &pad)) {
            g_lastRefusal = "InputManager +280 / +292 unreadable";
            return false;
        }
        if (kind != 1 || pad < 0) {
            if (!DiagPhyscrouchCycle) {
                g_lastRefusal = "the last input device is not a pad (InputManager +280 / +292)";
                return false;
            }
            // The FLAT test is played on the keyboard: send as pad 0 (the press switches the device kind to the pad until the
            // next key - prompts change, nothing else). Named on the edge line.
            // [PROBE: PHYSCROUCH] gate=diag_physcrouch_cycle  site=FLAT  log-only=no
            LOG("[PHYSCROUCH] FLAT TEST: the last input device is kind %d pad %d (not a pad) - sent as pad 0", kind, pad);
            pad = 0;
        }
    }
    if (!CallJoy(fn, mgr, pad < 0 ? 0 : pad, pressed ? 1 : 0)) { g_lastRefusal = "slot 148 faulted"; return false; }
    g_heldPad = pressed ? pad : -1;
    return true;
}

void FlushWindow(ULONGLONG nowMs, float h)
{
    float duck;
    AcquireSRWLockShared(&g_lock);
    duck = g_duck;
    ReleaseSRWLockShared(&g_lock);
    if (g_lines < kMaxLines) {
        ++g_lines;
        // [PROBE: PHYSCROUCH] gate=physical_crouch_enable  site=HEADSET  log-only=yes
        LOG_WINDOW(g_lines == 1, "[PHYSCROUCH] 5 s | polls %ld | head h %.3f (min %.3f max %.3f) learned standing %.3f, crouch below %.3f, stand above "
            "%.3f | head wants %d, from head %d, pressed %d | game duck %.3f | crouches %ld stands %ld | held back: no head %ld, "
            "no game reading %ld, menu %ld, sprinting %ld | re-seeds %ld | refused %ld (last: %s) | head report lifted on %ld "
            "calls, max %.3f m%s",
            w_polls, h, w_hMin < 1e8f ? w_hMin : 0.0f, w_hMax > -1e8f ? w_hMax : 0.0f, g_learn.standM,
            g_learn.standM - PhysicalCrouchOnDropM, g_learn.standM - PhysicalCrouchOffDropM, g_hyst.on ? 1 : 0,
            g_arb.fromHead ? 1 : 0, g_held ? 1 : 0, duck, w_crouch, w_stand, w_noHead, w_noGame, w_menu, w_sprintHeld, w_reseed,
            w_refused, g_lastRefusal, static_cast<long>(InterlockedExchange(&g_liftCalls, 0)),
            0.001 * static_cast<double>(InterlockedExchange(&g_liftMaxMm, 0)), g_lines == kMaxLines ? " | LAST PHYSCROUCH line" : "");
    }
    w_polls = w_noHead = w_noGame = w_menu = w_sprintHeld = w_reseed = w_crouch = w_stand = w_refused = 0;
    w_hMin = 1e9f; w_hMax = -1e9f;
    g_winMs = nowMs;
}

}  // namespace

// COST per poll while on: one head pose read + NeckPivotReportedPos, a few dozen flops, one SRW shared read, ~6 guarded reads for
// the sprint byte; at an edge only: ~5 guarded reads, one game call (slot 148), and once per manager pointer one RTTI name read.
// Off: one bool test (and a release if a press is still held). scePad poll thread.
void PhysCrouchForceUntil(unsigned long long untilMs)
{
    g_forceUntilMs = untilMs;
}

void PhysCrouchPoll(bool gameMenu)
{
    const bool forced = GetTickCount64() < g_forceUntilMs;
    if (!PhysicalCrouchEnable) {
        if (forced && !gameMenu) {   // climbing round 4: the crouch held for a crouched climb-up, physical crouch off
            if (!g_gd) { g_gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll")); }
            if (!g_held && g_gd && Send(true)) {
                g_held = true;
                g_forcedHeld = true;
                InterlockedIncrement(&g_edges);
                // [PROBE: PHYSCROUCH] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[PHYSCROUCH] CROUCH HELD for a climb-up into a low spot (climbhand: no standing room there) - 0x0F0F pressed; "
                    "physical crouch itself is off");
            }
            return;
        }
        if (g_held && Send(false)) {   // switched off live with a crouch held (or the climb's force ended): let it go
            g_held = false;
            if (g_forcedHeld) {
                // [PROBE: PHYSCROUCH] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[PHYSCROUCH] climb-up crouch RELEASED - standing room over the feet again (or the hold timed out)");
            }
            g_forcedHeld = false;
        }
        g_arb.fromHead = false;
        return;
    }
    if (!g_armed) {
        g_armed = true;
        QueryPerformanceFrequency(&g_qpf);
        g_gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
        g_winMs = GetTickCount64();
        // [PROBE: PHYSCROUCH] gate=physical_crouch_enable  site=FLAT  log-only=no
        LOG("[PHYSCROUCH] ARMED (logged unconditionally) - source %s | crouch at a drop of %.2f m, stand at %.2f m%s | input: "
            "EJoy__BUTTON_16 (0x%04X, our Data3 _ACTION_DUCK hold) through InputManager [GameDI+0xC8] slot 148 (gamedll+0x%llX "
            "Steam) | gamedll %p",
            DiagPhyscrouchCycle ? "SYNTHETIC (diag_physcrouch_cycle: 1.70 m 8 s / 1.20 m 4 s)" : "the head's neck point, appSpace y",
            static_cast<double>(PhysicalCrouchOnDropM), static_cast<double>(PhysicalCrouchOffDropM),
            crouch::ValidDrops(PhysicalCrouchOnDropM, PhysicalCrouchOffDropM) ? "" : " - INVALID (the stand drop must be smaller)",
            kCode, static_cast<unsigned long long>(kJoyBtnRva), reinterpret_cast<void*>(g_gd));
    }
    if (!crouch::ValidDrops(PhysicalCrouchOnDropM, PhysicalCrouchOffDropM)) {
        if (!g_refusedDrops) {
            g_refusedDrops = true;
            // [PROBE: PHYSCROUCH] gate=physical_crouch_enable  site=FLAT  log-only=no
            LOG("[PHYSCROUCH] REFUSED - physical_crouch_on_drop_m %.2f must be larger than physical_crouch_off_drop_m %.2f (> 0); "
                "no crouch is sent", static_cast<double>(PhysicalCrouchOnDropM), static_cast<double>(PhysicalCrouchOffDropM));
        }
        return;
    }
    if (!g_gd) { g_gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll")); if (!g_gd) { return; } }
    g_refusedDrops = false;
    ++w_polls;
    const double t = Now();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const float dt = g_lastQpc.QuadPart ? static_cast<float>(static_cast<double>(q.QuadPart - g_lastQpc.QuadPart) /
                                                             static_cast<double>(g_qpf.QuadPart)) : 0.0f;
    g_lastQpc = q;
    const ULONGLONG nowMs = GetTickCount64();

    // The height.
    float h = NAN;
    if (DiagPhyscrouchCycle) {
        if (g_cycleT0 < 0.0) { g_cycleT0 = t; }
        const double ph = std::fmod(t - g_cycleT0, kCycleStandS + kCycleCrouchS);
        h = ph < kCycleStandS ? kCycleStandM : kCycleCrouchM;
    } else {
        XrPosef head{};
        // B13 (trace 20261115_1545): not before the runtime has located the head once - before that the pose is the session's
        // XrIdentity() start value (h = 0.000), and a level learned from it held a seated tester crouched all session.
        if (HeadPoseLocatedOnce() && GetHeadPose(head)) {
            h = NeckPivotReportedPos(head).y;
            static bool s_saidLocated = false;
            if (!s_saidLocated && std::isfinite(h)) {
                s_saidLocated = true;
                // [PROBE: PHYSCROUCH] gate=physical_crouch_enable  site=HEADSET  log-only=yes
                LOG("[PHYSCROUCH] HEAD LOCATED - the first located head h %.3f seeds the standing level (polls before it held back "
                    "as 'no head': %ld this window)", h, w_noHead);
            }
        }
    }
    if (!std::isfinite(h)) {
        ++w_noHead;
        if (nowMs - g_winMs >= 5000) { FlushWindow(nowMs, 0.0f); }
        return;
    }
    if (h < w_hMin) { w_hMin = h; }
    if (h > w_hMax) { w_hMax = h; }
    if (!DiagPhyscrouchCycle && g_haveH && t - g_lastHs < kJumpS && std::fabs(h - g_lastH) > kJumpM) {
        ++w_reseed;
        const float was = g_learn.standM;
        g_learn = crouch::Learner{};
        g_hyst = crouch::Hyst{};
        AcquireSRWLockExclusive(&g_lock);
        g_standPubOk = false;
        ReleaseSRWLockExclusive(&g_lock);
        // [PROBE: PHYSCROUCH] gate=physical_crouch_enable  site=HEADSET  log-only=yes
        LOG("[PHYSCROUCH] RE-SEED - the head moved %.3f m in %.0f ms (a recentre, not a body): standing level %.3f forgotten, "
            "learned again from %.3f", h - g_lastH, 1000.0 * (t - g_lastHs), was, h);
    }
    g_haveH = true;
    g_lastH = h;
    g_lastHs = t;
    crouch::Learn(g_learn, h, g_hyst.on, dt, PhysicalCrouchOffDropM);
    const bool wants = crouch::HystStep(g_hyst, h, g_learn.standM, PhysicalCrouchOnDropM, PhysicalCrouchOffDropM);
    if (!DiagPhyscrouchCycle) {   // the synthetic head is not in the head report's space - never publish it
        AcquireSRWLockExclusive(&g_lock);
        g_standPub = g_learn.standM;
        g_standPubOk = g_learn.has;
        ReleaseSRWLockExclusive(&g_lock);
    }

    // The game.
    float duck;
    ULONGLONG duckMs;
    AcquireSRWLockShared(&g_lock);
    duck = g_duck; duckMs = g_duckMs;
    ReleaseSRWLockShared(&g_lock);
    const bool fresh = duck >= 0.0f && duckMs != 0 && static_cast<double>(nowMs - duckMs) / 1000.0 <= kGameStaleS;
    if (!fresh) {
        ++w_noGame;
    } else if (gameMenu) {
        ++w_menu;
    } else {
        const int sprint = ReadSprint();
        const bool sprinting = sprint == 1;
        const bool allowSlide = PhysicalCrouchSlide;   // physical_crouch_slide, read once per poll
        if (wants && sprinting && !allowSlide && !g_arb.fromHead) { ++w_sprintHeld; }
        const crouch::Act act = crouch::Decide(g_arb, wants, duck > 0.0f, sprinting, allowSlide);
        const bool want = g_arb.fromHead || forced;   // climbing round 4: or held for a crouched climb-up (PhysCrouchForceUntil)
        if (want != g_held) {
            const bool ok = Send(want);
            if (ok) {
                g_held = want;
                if (want) { ++w_crouch; } else { ++w_stand; }
                InterlockedIncrement(&g_edges);
            } else {
                ++w_refused;
                if (want) { g_arb.fromHead = false; }   // not sent: the next poll may try again
            }
            const bool say = ok || !g_failSaid;   // a refusal retries every poll - named once per run of refusals
            g_failSaid = !ok;
            if (say && g_lines < kMaxLines) {
                ++g_lines;
                // [PROBE: PHYSCROUCH] gate=physical_crouch_enable  site=FLAT  log-only=no
                LOG("[PHYSCROUCH] %s %s | h %.3f stand %.3f (drop %.3f) | game duck %.3f, sprint %d | pad %d | %s",
                    want ? "CROUCH" : "STAND",
                    want && forced && !g_arb.fromHead ? "(HELD for a climb-up into a low spot - climbhand: no standing room there)"
                    : act == crouch::Act::None ? "(head came up before the game crouched)"
                        : (want && sprinting ? "(SLIDE - sent while sprinting, physical_crouch_slide on)" : ""),
                    h, g_learn.standM, g_learn.standM - h, duck, sprint, want ? g_heldPad : (ok ? -1 : g_heldPad),
                    ok ? (want ? "0x0F0F pressed (held)" : "0x0F0F released") : g_lastRefusal);
            }
        }
    }
    if (nowMs - g_winMs >= 5000) { FlushWindow(nowMs, h); }
}

// Option A (Jay 2026-10-27, "Go ahead with A", after run 6): while the game is crouched, the real head drop below the learned standing
// neck height is taken out of the head height the ENGINE is given - up to the game's own drop (duck x kGameDropM). gamedll +C08100
// puts HSPINE1 at the reported head offset by world writes (INV-306), so a real crouch on top of the game's crushed the spine to ~10 %
// (run 6: spine1/2/3 9-26 mm vs 113-167 standing; the B crouch alone keeps every length). With the cut the game sees a level head and
// its own crouch - the regular crouch's body AND view. COST per call (the engine's job workers, under the frame lock): one SRW shared
// read, a few flops, at most two interlocked ops while lifting. Off / standing / no learned level: one bool test or one read.
float PhysCrouchReportLiftY(float headY)
{
    if (!PhysicalCrouchEnable) { return 0.0f; }
    float stand, duck;
    bool ok;
    ULONGLONG duckMs;
    AcquireSRWLockShared(&g_lock);
    stand = g_standPub; ok = g_standPubOk; duck = g_duck; duckMs = g_duckMs;
    ReleaseSRWLockShared(&g_lock);
    if (!ok || !(duck > 0.0f) || duckMs == 0 || GetTickCount64() - duckMs > 500 || !std::isfinite(headY)) { return 0.0f; }
    const float drop = stand - headY;
    if (!(drop > 0.0f)) { return 0.0f; }
    const float cap = duck * kGameDropM;
    const float lift = drop < cap ? drop : cap;
    InterlockedIncrement(&g_liftCalls);
    const LONG mm = static_cast<LONG>(lift * 1000.0f);
    LONG prev = g_liftMaxMm;
    while (mm > prev) {
        const LONG seen = InterlockedCompareExchange(&g_liftMaxMm, mm, prev);
        if (seen == prev) { break; }
        prev = seen;
    }
    return lift;
}

long PhysCrouchEdges()
{
    return InterlockedCompareExchange(&g_edges, 0, 0);
}

void PhysCrouchNoteGame(float duck)
{
    AcquireSRWLockExclusive(&g_lock);
    g_duck = duck;
    g_duckMs = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace dlvr
