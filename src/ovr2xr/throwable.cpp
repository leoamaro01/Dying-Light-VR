// Throwing by hand (VR) - docs/superpowers/specs/2026-10-14-throwables-design.md, Build A (plan
// docs/superpowers/plans/2026-10-14-throwables-build-a.md, Task 3). The decision is throwable_logic.h (unit-tested,
// src/src/tests/throwable_logic_test.cpp); this file reads its inputs on the scePad poll, applies the L2 / R2 bytes it
// asks for, keeps the right hand's history, freezes the let-go snapshot and logs the THROWABLE event lines.
// The velocity rule follows Mamfro's dying-light-games-vr, dl1/src/player/throwing.rs (author cleared use, attribution
// kept). The game-side detours (Task 4, bottom of this file): set-state +DD33A0 (our release leaves at once, Mamfro's
// release-time zero), launch +EEA9F0 (the hand's velocity and position for types 25/26/28/29), aim point +DBCCC0 (the
// ThrowingStar's point on the hand's line) and the throw controller's Update +DD1640 (the cancel, on the game thread).
//
// THREADS. ThrowablePadByte runs on the scePad poll (dlvr_FilterScePad, ~250 Hz, a game thread in the update phase -
// the same poll BoneBlendTick rides, INV-138). ThrowableTick rides the same poll, self-throttled to 1 Hz. The cancel itself
// (set-state 0) is NEVER made here: the pad thread only raises a flag that the controller-Update detour consumes on
// the controller's own path (RESEARCH-THROWABLES "Build A pre-reads" step 5, INV-566; progress.md rulings R2a / R6).
// The OS thread of each detour's caller is [UNKNOWN] statically (INV-568) - every detour keeps per-thread counters.
#include "throwable.h"

#include "aimhook.h"        // AimMirrorYaw - the gun's yaw-sign convention
#include "bodygrab.h"       // BodyGrabHandActive - a hand holding a body blocks the throwable like the menu
#include "boneblend.h"      // InstallSingleGameDetour - the shared verify / suspend / commit path
#include "calibscreen.h"    // CalibScreenActive - blocks input like the mod menu
#include "camhook.h"        // CamHookGetOpenXrToGameYaw, CamHookGetRenderedViewMatrix
#include "uipin.h"          // uipin::GameMenuOpen - an in-game menu is fed as a blocking menu (game-menu input pass-through)
#include "controllers.h"
#include "headpose.h"
#include "holster.h"        // HolsterHipCentre(0), HolsterRHipRadius - the RIGHT hip zone (the R2 ready, spec 2026-11-01)
#include "holster_logic.h"  // holster::LocalOffset, holster::kExitScale
#include "overlay.h"        // OverlayVisible - the mod menu

#include <windows.h>
#include <intrin.h>         // _ReturnAddress - the aim-point detour tells the throw's callers from the melee ones

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>          // memcmp - the restore's one-time byte check

#include "../common/buildrva.h"   // the throwables family's EGL rvas on the Epic build (tier 2)
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - diag_periodic_logs (the 5 s detour status line only)

namespace dlvr {

// [DEP: throwable_hand_enable] required-for=throwing by hand (ThrowableHandEnable) - spec 2026-10-14-throwables-design,
//      plan Build A Task 3. tree=INERT at 0 with nothing in flight: ThrowablePadByte returns at its first test, no byte
//      is touched, no game memory is read, and ThrowableTick resolves nothing. Switched to 0 while a throw is readied,
//      the decision is first driven through its cancel (the forced menuBlocking input) so L2 is never dropped in state 2.
//      HEADSET: needs both VR triggers, the head + both GRIP poses, the gamedll's local player (route 1), the holster
//      hip zone values (holster_hip_center_* / holster_hip_radius - the zone, not holster_hip_enable), and Task 4's
//      Update detour to carry out a cancel. Compiled default 1 (2026-10-16, Jay: "Ship disk for throwable" - his disk 1).
bool  ThrowableHandEnable = true;
// [DEP: throwable_full_speed] required-for=the thrown speed (ThrowableFullSpeed), m/s of hand speed that gives the
//      item's own full speed (Mamfro's FULL_SPEED). tree=INERT at throwable_hand_enable=0.
float ThrowableFullSpeed = 5.0f;
// [DEP: throwable_still_speed] required-for=drop vs throw at the let-go (ThrowableStillSpeed), m/s - a hand slower than
//      this drops the item straight down. tree=INERT at throwable_hand_enable=0.
float ThrowableStillSpeed = 0.05f;
// [DEP: throwable_star_hand_velocity] required-for=throwing stars / knives / axes (ItemType 40) with the HAND's velocity
//      (ThrowableStarHandVelocity) - throwables issue 4 (Jay run 1: "Ninja stars throw regardless of arm speed"; option 1
//      "ideally", "Approved, own switch", 2026-10-15). The flight detour on gamedll +6E6D70 (CalculateFlightParams by shape)
//      is installed the first second this is 1 with throwable_hand_enable's detours live, and replaces the velocity the
//      game sets (vfunc +432 = IPhBody::SetLinearVel) with ThrownVelocity(hand, the game's own speed) - the grenades' rule.
//      tree=INERT at 0 (one bool load per call once installed; nothing installed before it is first 1) and INERT at
//      throwable_hand_enable=0 (no release token is ever made). HEADSET. SHIPS ON - Tier A (Jay, starhand_run1
//      2026-10-15: "Works perfectly"; 14 of 14 STAR FLIGHT ours, 0 refused).
bool  ThrowableStarHandVelocity = true;

namespace {

// gamedll RVAs and offsets, md5 97046bba5eb07a918acff82a394e8c8b - RESEARCH-THROWABLES "Build A pre-reads" constants
// table, INV-563 / INV-566 / INV-569 / INV-570. Every one is the GAMEDLL.
constexpr uintptr_t kPlayerGlobalRva = 0x1C16358;  // route 1: [[gd+this]+1920] = the local PlayerDI (INV-570)
constexpr size_t    kPlayerOff       = 1920;
constexpr uintptr_t kRoute2GlobalRva = 0x1C16348;  // route 2: [[[gd+this]+1344]+184]: +80 handle, +88 object (INV-570)
constexpr size_t    kRoute2A = 1344, kRoute2B = 184, kRoute2Handle = 80, kRoute2Obj = 88;
constexpr size_t    kThrowCtrlOff    = 0xC28;      // [player+3112] = the throw controller (INV-563 / INV-565)
constexpr size_t    kSelOverrideOff  = 11096;      // selected item: [player+11096], else the quickslot row (INV-569)
constexpr size_t    kSelSlotOff      = 11088;      //   slot index (dword)
constexpr size_t    kSlotCountOff    = 11056;      //   slot count (dword)
constexpr size_t    kSlotArrayOff    = 11048;      //   row base; item = [base + 24*slot]
constexpr size_t    kSlotStride      = 24;
constexpr uintptr_t kSelThrowableRva = 0xBDD9B0;   // the game's "selected THROWABLE" getter (player) -> item or 0 (INV-569)
constexpr size_t    kItemInfoOff     = 88;         // item+88 = ItemInfo+24 (IsThrowable +6F2C60 takes item+64, INV-569)
constexpr size_t    kTypeVfuncOff    = 0x1C0;      // [[item+88]+0x1C0](item+88, player) -> ItemType (+BDD9B0 :16-17)
constexpr unsigned  kTypeHook        = 47;         // Hook = the grapple: passes IsThrowable, keeps its own LT (ruling R5)
constexpr size_t    kCtrlStateOff    = 0x38;       // int: the throw state; 2 = readied (INV-563)
constexpr size_t    kCtrlReleasedOff = 0x7E;       // byte: set to 1 by +DD1A80 right before the release +DD1AD0; cleared by
                                                   // set-state 0 and by Update in states 1 / 2 (tw3_gamedll_decomp.c:432,
                                                   // :450, :1125-1132; RESEARCH table row set-state)
// Ruling R18 (INV-572): the player's VehicleController is controller ID 4 at [player+3008] (cs3_gamedll_out.txt:6) and its
// byte +16 is the controller's ACTIVE flag - set to 1 by its Start (slot 18 +D967B0), 0 by its Stop (slot 19 +D96CF0);
// the game itself tests [[player+3008]+16] at 45 sites (tb4v_gamedll_x64_rwdi_out.txt) - the Mac build names the same
// test PlayerFppVis::UpdatePlayerInVehicle / PlayerDI::EnterVehicle. While it is set, L2 is the buggy's, never ours.
constexpr size_t    kVehicleCtrlOff  = 3008;
constexpr size_t    kCtrlActiveOff   = 16;
// INV-574: the throw controller's Start-time item - handle [+0x90] (cleared by Stop +DD1560), InventoryItem* [+0x98] (Start
// +DD1360 from +BCD0C0 = the CURRENT hand item; the same pointer kind as the quickslot row). A re-ready after a switch waits
// until the controller is inactive, holds nothing, or holds the selected item - else +DD1260 restarts the OLD item.
constexpr size_t    kCtrlItemHandleOff = 0x90;
constexpr size_t    kCtrlItemOff       = 0x98;

// ---- Task 4: the game-side detours. gamedll md5 97046bba..., RESEARCH "Build A pre-reads" + INV-563..568. ----
constexpr uintptr_t kSetStateRva  = 0xDD33A0;   // set-state(ctrl, int) (INV-563)
constexpr unsigned char kSetStateBytes[14] = { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x8B, 0x41, 0x38, 0x48, 0x8B, 0xF1,
                                               0x3B, 0xD0 };   // INV-568 SAFE TO STEAL 14
constexpr uintptr_t kLaunchRva    = 0xEEA9F0;   // launch(obj, const vec3* vel, const vec3* target) - slot 123 (INV-564/567)
constexpr unsigned char kLaunchBytes[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
                                             0x83, 0xEC, 0x60 };   // INV-568 SAFE 15
constexpr uintptr_t kAimPointRva  = 0xDBCCC0;   // aim point(meleeCtrl, vec3* out, char flag) -> IModelObject* or 0
constexpr unsigned char kAimPointBytes[15] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
                                               0x89, 0x78, 0x20 };   // INV-568 SAFE 15
constexpr uintptr_t kUpdateRva    = 0xDD1640;   // the throw controller's Update(ctrl, float dt) - slot 16 (INV-563)
constexpr unsigned char kUpdateBytes[17] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9,
                                             0x0F, 0x29, 0x7C, 0x24, 0x20 };   // INV-568 SAFE 17
constexpr uintptr_t kZeroVecRva   = 0x1CFB510;  // the zero vec3 the player's throw passes to launch = the game's lob (INV-567)
// The aim point's two throw-path callers, by RETURN address (tb4dis3/tb4dis4_gamedll_out.txt): +DD244A in the release
// +DD1AD0 and +DD458F in the throw controller's per-frame +DD44A0. Its other three callers (+DBC302 / +DBEE21 / +DC3997)
// are the melee update's. ALL FIVE pass rcx = the WEAPON MELEE controller ([player+0xC10], cs3 ID 14) - never the throw
// controller (brief step 4's "(ctrl, out, flag)" read as the throw controller would match nothing).
constexpr uintptr_t kAimRetRelease = 0xDD244F;
constexpr uintptr_t kAimRetUpdate  = 0xDD4594;
constexpr size_t    kMeleeCtrlOff  = 0xC10;
constexpr size_t    kCtrlReleaseTimeOff = 0x60;  // float: the state 3-5 release time, set by set-state from the animation
constexpr size_t    kCtrlActionOff      = 0x70;  // int: the starting action (218), -1 = none (the 241 cancel / Stop)
constexpr size_t    kCtrlAutoStartOff   = 0x74;  // byte: Start re-enters the throw when set (+DD3500 sets it; ruling R7) - AND
                                                 // "a restore of the hands is owed" (INV-573): +DD3500 put the throwable in the hand
constexpr size_t    kCtrlRestoredOff    = 0x7C;  // byte: the restore has run (+DD4BF0 sets it; +DD3500 clears it) (INV-573)
constexpr size_t    kCtrlTimeOff        = 0x78;  // float: the game's 241 cancel / Stop / QuickThrow write the controller's time
constexpr size_t    kCtrlTimeVfuncOff   = 48;    //   from vfunc +48 (tw3_gamedll_decomp.c:711, :661) (INV-573)
// The game's own TryToRestoreItems (Mac name, shape-compared - INV-573): if [+0x74] && ![+0x7C], give the player back the
// current quickslot (+BCA170 / +BD6F60) and set [+0x7C]. The 241 cancel, Stop and the release all run it BEFORE they clear
// [+0x74]; Build A's cancel did not, which left the throwable in the hand (Jay, run 1 issues 2 / 3). Called, never patched:
// the bytes below are only compared once, at install, so another build refuses instead of calling the wrong code.
constexpr uintptr_t kRestoreRva = 0xDD4BF0;
constexpr unsigned char kRestoreBytes[21] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x79, 0x74, 0x00, 0x48, 0x8B,
                                              0xD9, 0x74, 0x3A, 0x80, 0x79, 0x7C, 0x00, 0x75, 0x34 };   // gamedll file bytes
// Throwables issue 4 (2026-10-15, static, our gamedll 97046bba): a ThrowingStar (type 40) never reaches launch - the release
// +DD1AD0 makes a DroppedItem and calls DropByActor (+6E2860, vtable +153F4E0 slot +1576), which stores a TARGET POINT
// ([+1388..+1396]). Its first physics step (+6E47F0, the OnPhysicsPreStep shape, the only caller) runs +6E6D70 = Mac
// DroppedItem::CalculateFlightParams by shape (12.5 default; type 40 -> player variable 78, 47 -> 74): an arc to the point
// at the item's own speed, capped, set through vfunc +432 (our +EBAD60 -> IPhBody::SetLinearVel - Mac +648
// ObjectDI::SetPhysicsLinearVel: it SETS) and spin through +448 (+EBAE30 -> IPhBody::SetAngularVel). Then the point is
// cleared, so it computes once per flight. Every other call (every physics step of every DroppedItem) returns early.
// Prologue 17: push rbp / push rdi / push r15 / lea rbp,[rsp-47h] / sub rsp,0E0h - no RIP-relative operand, no branch
// lands inside (ts3dis / ts5_gamedll_out.txt: the four in-range xrefs are fall-through). Mac names: ts1mac / ts4 / ts6.
constexpr uintptr_t kFlightRva = 0x6E6D70;
constexpr unsigned char kFlightBytes[17] = { 0x40, 0x55, 0x57, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81,
                                             0xEC, 0xE0, 0x00, 0x00, 0x00 };   // gamedll file bytes
constexpr size_t    kDropThrowerHandle = 1456;  // [obj+1456] != 0 -> [obj+1464] = the actor DropByActor was given
constexpr size_t    kDropThrower       = 1464;
constexpr size_t    kDropItemInfo      = 1128;  // [[obj+1128]+448](obj+1128) -> ItemType (+6E6D70 :95)
constexpr size_t    kDropTypeVfunc     = 448;
constexpr size_t    kDropTargetOff     = 1388;  // vec3 target point; the zero vec = none
constexpr size_t    kDropSpeedOff      = 1412;  // float; > 0 also makes +6E6D70 compute
constexpr size_t    kSetLinVelVfunc    = 432;   // +EBAD60 IPhBody::SetLinearVel
constexpr size_t    kGetLinVelVfunc    = 440;   // +EBACF0 IPhBody::GetLinearVel -> out (the zero vec when no body)
constexpr size_t    kObjThrowerHandle = 880;     // [obj+880] != 0 -> [obj+888] = the thrower (slot 74 +EEA7B0, RESEARCH)
constexpr size_t    kObjThrower       = 888;
constexpr size_t    kObjLaunched      = 0x3B0;   // byte 944: launch runs once (+EEA9FF)
constexpr size_t    kObjVelOff        = 0x100;   // vec3 the velocity slot 125 +EEBB30 ADDS into (tw4_gamedll_decomp.c:4345)
constexpr size_t    kObjControlOff    = 0x18;    // IControlObject sub-object: +EEBB30 PhysicsBind(a1+6 floats), +DBD1EC
constexpr size_t    kOwnVelVfuncOff   = 992;     // slot 124 +EEAB60: the object's own lob velocity -> out (and ADDS it)
constexpr int       kTypeThrowingStar = 40;
constexpr double    kSnapMaxMs        = 300.0;   // the let-go snapshot / our release older than this is never used (R17)
constexpr float     kStartAheadM      = 0.1f;    // the object starts this far along the throw from the hand
constexpr float     kMaxStartGapM     = 1.0f;    // ruling R20: start further than this from the game's spawn = refuse it all
constexpr float     kAimReachPerSpeed = 3.0f, kAimNearestM = 0.5f, kAimFarthestM = 40.0f;   // Mamfro's star reach
constexpr float     kKeepTargetDeg    = 12.0f;   // Mamfro's KEEP_TARGET: a locked target this close to the hand's line stays
constexpr int       kMaxInstallTries  = 10;

constexpr unsigned char kL2Off = 0x08, kR2Off = 0x09;
constexpr uint32_t kDpadRightBit = 0x20u;   // weapon swap (holster tap, INV-311)
constexpr uint32_t kDpadLeftBit  = 0x80u;   // item swap (right-hip holster tap, INV-311)
constexpr double   kItemRecheckMs = 250.0;  // re-run the throwable test this often even when the item pointer is unchanged
constexpr float    kMaxHandOffsetM = 1.5f;  // grappleaim.cpp:760 - a tracking glitch must not throw the origin metres away

using SelThrowableFn = uintptr_t(__fastcall*)(uintptr_t player);
using ItemTypeFn     = uint64_t(__fastcall*)(uintptr_t itemInfo, uintptr_t player);

// Written once by ThrowableTick BEFORE g_resolved is set to 1; read by the pad thread only after it reads 1.
uintptr_t     g_gd = 0, g_eng = 0;
volatile LONG g_resolved = 0;
LONG          g_lastArm = -1;   // ThrowableTick only: the state last logged

// Published by the pad thread, read by the game-side hooks (Task 4).
void* volatile g_localCtrl     = nullptr;
volatile LONG  g_cancelPending = 0;
volatile LONG  g_releasePend   = 0;
volatile LONG  g_holdsL2       = 0;
volatile LONG  g_hookSel       = 0;   // the grappling hook is the selected item (ThrowableHookSelected; pad thread writes)
volatile LONG  g_busy          = 0;         // ruling R24: the decision is not Idle (incl. a cancel wait)
void* volatile g_localPlayer   = nullptr;   // route 1 PlayerDI as read on the latest armed poll
volatile LONG  g_selType       = -1;        // the selected throwable's ItemType (-1 = none / not a throwable)
volatile LONG  g_padTid        = 0;         // the pad poll's OS thread, for the CANCEL line's thread compare

// ---- Task 4 detour state ----
// Our release token: set by the set-state detour when it zeroes the release time of OUR throw (game thread), read and
// consumed by the launch detour, read by the aim-point detour. 0 = none. Milliseconds on NowMs()'s clock.
volatile LONG64 g_relTokenMs   = 0;
void* volatile  g_relTokenCtrl = nullptr;
volatile LONG   g_hooksLive    = 0;   // 1 = all four detours committed; the pad side readies nothing before
bool            g_hooksRefused = false;   // ThrowableTick only: refused for the session (named in its line)
int             g_installTries = 0;
const char*     g_hooksWhy     = "not attempted yet";

using SetStateFn = void(__fastcall*)(void* ctrl, int state);
using LaunchFn   = void(__fastcall*)(void* obj, const float* vel, const float* target);
using AimPointFn = void*(__fastcall*)(void* meleeCtrl, float* out, char flag);
using UpdateFn   = void(__fastcall*)(void* ctrl, float dt);
using OwnVelFn   = float*(__fastcall*)(void* obj, float* out);
using RestoreFn  = void(__fastcall*)(void* ctrl);                                       // +DD4BF0 (INV-573)
using CtrlTimeFn = float(__fastcall*)(void* ctrl);                                      // controller vfunc +48
using SetWorldPosFn = void(__fastcall*)(void* control, const float* pos);            // IControlObject::SetWorldPosition
using LocalToWorldFn = float*(__fastcall*)(const void* control, float* ret, const float* p);   // ::PointLocalToWorld
SetStateFn     g_origSetState = nullptr;
LaunchFn       g_origLaunch   = nullptr;
AimPointFn     g_origAimPoint = nullptr;
UpdateFn       g_origUpdate   = nullptr;
RestoreFn      g_restore      = nullptr;   // set by ThrowableInstall once kRestoreBytes match, before g_hooksLive
SetWorldPosFn  g_setWorldPos  = nullptr;
LocalToWorldFn g_localToWorld = nullptr;
using FlightFn = void(__fastcall*)(void* obj, char again);                              // +6E6D70
using VecSetFn = void(__fastcall*)(void* obj, const float* v);                           // vfunc +432
using VecGetFn = float*(__fastcall*)(void* obj, float* out);                             // vfunc +440
FlightFn       g_origFlight   = nullptr;
volatile LONG  g_flightLive   = 0;      // 1 = the flight detour is committed (issue 4)
bool           g_flightRefused = false; // ThrowableTick only: a prologue mismatch / try cap - refused for the session
int            g_flightTries  = 0;

// Per-thread call counters (INV-568: the OS thread of every root is [UNKNOWN]; memory "detour probe: threads + call
// sites"). Lock-free: a thread claims a slot by compare-exchange on its id. COST per call: GetCurrentThreadId (a TEB
// read), up to kThreadSlots compares, one or two InterlockedIncrement.
constexpr int kThreadSlots = 8;
struct ThreadCount { volatile LONG tid; volatile LONG calls; volatile LONG ours; };
struct HookStats {
    const char* name;
    ThreadCount t[kThreadSlots];
    volatile LONG overflow;   // calls from a thread that found no free slot (still in calls / ours totals)
    volatile LONG calls, ours;
};
HookStats g_stSetState = { "set-state" }, g_stLaunch = { "launch" }, g_stAim = { "aim point" }, g_stUpdate = { "update" };
// The flight detour (issue 4) counts only the calls past its fast path (switch on, a release token set) - not every
// physics step of every DroppedItem.
HookStats g_stFlight = { "flight" };
volatile LONG g_nFlightOurs = 0, g_nFlightRefused = 0;
// What the detours passed through, by reason (each one a plain counter, read by the status line).
volatile LONG g_nUnexpected = 0, g_nRelApplied = 0, g_nLaunchOurs = 0, g_nLaunchOther = 0, g_nLaunchLocalNotTaken = 0,
              g_nAimOurs = 0, g_nAimKept = 0, g_nAimNotThrow = 0, g_nCancelDone = 0, g_nCancelFault = 0,
              g_nCancelSkipped = 0, g_nCancelAtZero = 0;

void CountCall(HookStats& s, bool ours)
{
    InterlockedIncrement(&s.calls);
    if (ours) { InterlockedIncrement(&s.ours); }
    const LONG tid = static_cast<LONG>(GetCurrentThreadId());
    for (int i = 0; i < kThreadSlots; ++i) {
        LONG cur = s.t[i].tid;
        if (cur == 0) {
            cur = InterlockedCompareExchange(&s.t[i].tid, tid, 0);
            if (cur == 0) { cur = tid; }
        }
        if (cur == tid) {
            InterlockedIncrement(&s.t[i].calls);
            if (ours) { InterlockedIncrement(&s.t[i].ours); }
            return;
        }
    }
    InterlockedIncrement(&s.overflow);
}

// The let-go snapshot: one writer (the pad thread), any reader; a seqlock like controllers.cpp's g_poseSeq.
volatile LONG g_snapSeq = 0;
float         g_snapPos[3] = {}, g_snapVel[3] = {};
double        g_snapMs = 0;
bool          g_snapValid = false;

// Pad thread only.
throwable::Machine     g_m;
throwable::HandHistory g_hist;
bool      g_firstSaid = false;
bool      g_inHip = false;
uint32_t  g_prevSwitchBits = 0;
int       g_prevGameState = -1;
int       g_lastWhy = -1;          // the last read result logged (0 = fine)
bool      g_disableSaid = false;
float     g_lastPushed[3] = {};
bool      g_havePushed = false;
// The selected-item test, re-run only when the item pointer changes (or every kItemRecheckMs).
uintptr_t g_evalRaw = 1;           // 1 = never evaluated (no item pointer is 1)
double    g_evalMs = -1e9;
uintptr_t g_evalThr = 0;
int       g_evalType = -1;
bool      g_evalFault = false;
bool      g_evalFaultSaid = false;
bool      g_inLevel = false;       // the local player read non-null since it last read null (ruling R15)
long      g_nInLevel = 0;
long      g_nEv[13] = {};

const char* const kEventName[13] = { "None", "READY", "READIED", "RELEASE", "CANCEL", "CANCEL FOR SWITCH", "REFUSED",
                                     "RELEASED", "RELEASE TIMEOUT", "LOST", "WITHHELD", "CANCEL STUCK",
                                     "RE-READY TIMEOUT" };
const char* const kStateName[4] = { "Idle", "Readying", "Readied", "Releasing" };

double NowMs()
{
    static const double s_perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / s_perMs;
}

// ---- the game reads. Plain data only inside __try (no C++ unwinding - C2712). ----

struct GameRead {
    uintptr_t player = 0, ctrl = 0, rawItem = 0;
    int state = -1;
    int released = 0;
    uintptr_t vehCtrl = 0;   // [player+3008] (INV-572)
    int inVehicle = 0;       // its active byte +16; 0 when the controller pointer is null
    int ctrlActive = 0;      // the throw controller's active byte +16 (INV-574)
    uintptr_t ctrlItemHandle = 0, ctrlItem = 0;   // its [+0x90] / [+0x98]
};

// 0 = read, 1 = the HUD manager global is null (route 1, INV-570), 2 = [HUD+1920] null (no local player - a load, the
// main menu), 3 = a fault while reading. A null controller is not a failure: state stays -1.
// The loaded build's rvas of the three data sites read below (common/buildrva.h: Steam unchanged, the EGL table on the
// Epic build), each resolved once. Separate functions: a function-local static cannot sit in a function using __try.
uintptr_t PlayerGlobalRva()
{
    static const uintptr_t s = ::dlvr::buildrva::Gd(kPlayerGlobalRva);
    return s;
}
uintptr_t Route2GlobalRva()
{
    static const uintptr_t s = ::dlvr::buildrva::Gd(kRoute2GlobalRva);
    return s;
}
uintptr_t ZeroVecRva()
{
    static const uintptr_t s = ::dlvr::buildrva::Gd(kZeroVecRva);
    return s;
}

int ReadGame(uintptr_t gd, GameRead* r)
{
    const uintptr_t playerRva = PlayerGlobalRva();
    __try {
        const uintptr_t hud = *reinterpret_cast<const volatile uintptr_t*>(gd + playerRva);
        if (!hud) { return 1; }
        const uintptr_t pl = *reinterpret_cast<const volatile uintptr_t*>(hud + kPlayerOff);
        if (!pl) { return 2; }
        r->player = pl;
        // INV-569: the same read +BDD9B0 makes, inline (memory only).
        uintptr_t item = *reinterpret_cast<const volatile uintptr_t*>(pl + kSelOverrideOff);
        if (!item) {
            const uint32_t slot = *reinterpret_cast<const volatile uint32_t*>(pl + kSelSlotOff);
            const uint32_t n = *reinterpret_cast<const volatile uint32_t*>(pl + kSlotCountOff);
            if (slot < n) {
                const uintptr_t row = *reinterpret_cast<const volatile uintptr_t*>(pl + kSlotArrayOff);
                item = row ? *reinterpret_cast<const volatile uintptr_t*>(row + kSlotStride * slot) : 0;
            }
        }
        r->rawItem = item;
        const uintptr_t ctrl = *reinterpret_cast<const volatile uintptr_t*>(pl + kThrowCtrlOff);
        r->ctrl = ctrl;
        if (ctrl) {
            r->state = *reinterpret_cast<const volatile int*>(ctrl + kCtrlStateOff);
            r->released = *reinterpret_cast<const volatile unsigned char*>(ctrl + kCtrlReleasedOff);
            r->ctrlActive = *reinterpret_cast<const volatile unsigned char*>(ctrl + kCtrlActiveOff);
            r->ctrlItemHandle = *reinterpret_cast<const volatile uintptr_t*>(ctrl + kCtrlItemHandleOff);
            r->ctrlItem = *reinterpret_cast<const volatile uintptr_t*>(ctrl + kCtrlItemOff);
        }
        // INV-572: the VehicleController's active byte - two loads, no call.
        const uintptr_t veh = *reinterpret_cast<const volatile uintptr_t*>(pl + kVehicleCtrlOff);
        r->vehCtrl = veh;
        r->inVehicle = veh ? *reinterpret_cast<const volatile unsigned char*>(veh + kCtrlActiveOff) : 0;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 3;
    }
}

// Route 2 (INV-570), for the FIRST line's compare only. *why: 0 read, 1 global null, 2 +1344 null, 3 +184 null, 4 fault.
uintptr_t ReadRoute2(uintptr_t gd, int* why)
{
    const uintptr_t route2Rva = Route2GlobalRva();
    __try {
        const uintptr_t g = *reinterpret_cast<const volatile uintptr_t*>(gd + route2Rva);
        if (!g) { *why = 1; return 0; }
        const uintptr_t a = *reinterpret_cast<const volatile uintptr_t*>(g + kRoute2A);
        if (!a) { *why = 2; return 0; }
        const uintptr_t lc = *reinterpret_cast<const volatile uintptr_t*>(a + kRoute2B);
        if (!lc) { *why = 3; return 0; }
        *why = 0;
        const uintptr_t h = *reinterpret_cast<const volatile uintptr_t*>(lc + kRoute2Handle);
        const uintptr_t o = *reinterpret_cast<const volatile uintptr_t*>(lc + kRoute2Obj);
        return (h && o) ? o : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = 4;
        return 0;
    }
}

// +BDD9B0's own type test, inline (its disassembly, tb3_gamedll_x64_rwdi_out.txt:920-930): type <= 47 with bit set in
// 0xFF00FE004000, or type - 117 <= 6 with bit set in 75.
bool IsThrowableType(int type)
{
    const unsigned t = static_cast<unsigned>(type);
    if (t <= 0x2F) {
        if ((0xFF00FE004000ull >> t) & 1ull) { return true; }
    }
    const unsigned u = t - 117u;
    return u <= 6u && ((75u >> u) & 1u) != 0;
}

// The selected item's ItemType through the vfunc +BDD9B0 itself calls (+0x1C0 on item+88), ONCE (ruling R14), and the
// getter's own mask applied to that one result (= what +BDD9B0 would return, without its second call). INV-571: the
// chain is a pure leaf read - ItemDescWithContext +74AB60 forwards to the desc's +70C220, a bitfield load; no write, lock
// or allocation. Made only when the item pointer changes or every kItemRecheckMs. false = a fault (refused by name).
bool EvalItem(uintptr_t player, uintptr_t raw, uintptr_t* thr, int* type)
{
    __try {
        *thr = 0;
        *type = -1;
        if (raw) {
            const uintptr_t info = raw + kItemInfoOff;
            const uintptr_t vt = *reinterpret_cast<const volatile uintptr_t*>(info);
            const ItemTypeFn fn = *reinterpret_cast<const ItemTypeFn*>(vt + kTypeVfuncOff);
            *type = static_cast<int>(static_cast<unsigned>(fn(info, player)));
            if (IsThrowableType(*type)) { *thr = raw; }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *thr = 0;
        *type = -1;
        return false;
    }
}

const char* Route2WhyName(int why)
{
    switch (why) {
    case 0: return "read";
    case 1: return "global null";
    case 2: return "+1344 null";
    case 3: return "+184 null";
    default: return "FAULT";
    }
}

const char* ReadWhyName(int why)
{
    switch (why) {
    case 0: return "read";
    case 1: return "the HUD manager global is null (route 1)";
    case 2: return "player null ([HUD+1920] = 0: no local player - a load or the main menu)";
    case 3: return "a FAULT while reading the player / selected item / throw controller";
    default: return "?";
    }
}

// The gun's OpenXR -> game mapping: a yaw about +Y (ComputeAbsoluteAim's D branch, aimhook.cpp:621-628, and grappleaim's
// hand = camera + R*(grip - reported head), grappleaim.cpp:752-764). out may alias nothing.
void YawMap(float c, float s, const float in[3], float out[3])
{
    out[0] = in[0] * c + in[2] * s;
    out[1] = in[1];
    out[2] = -in[0] * s + in[2] * c;
}

void PublishSnapshot(bool valid, const float pos[3], const float vel[3], double tMs)
{
    InterlockedIncrement(&g_snapSeq);   // odd: write in progress
    for (int k = 0; k < 3; ++k) { g_snapPos[k] = pos[k]; g_snapVel[k] = vel[k]; }
    g_snapMs = tMs;
    g_snapValid = valid;
    InterlockedIncrement(&g_snapSeq);   // even: settled
}

// The let-go: the fastest motion in the window and the stroke around it (HandHistory, tracking space), mapped to world
// with the yaw in force NOW, and the hand's world position now. Runs on the Release event only. Writes a one-line
// description into `out` for the Release line.
void TakeSnapshot(double now, const throwable::Params& p, char* out, size_t outLen)
{
    throwable::Vec3 v{};
    double atMs = 0;
    const bool fast = g_hist.Fastest(now, p.windowMs, v, atMs);
    throwable::Vec3 dir{};
    const bool stroke = fast && g_hist.Stroke(atMs, p.strokeBeforeMs, p.strokeAfterMs, dir);
    const float speed = fast ? std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) : 0.0f;
    float handTrk[3] = { 0, 0, 0 };
    if (fast) {
        if (stroke) { handTrk[0] = dir.x * speed; handTrk[1] = dir.y * speed; handTrk[2] = dir.z * speed; }
        else { handTrk[0] = v.x; handTrk[1] = v.y; handTrk[2] = v.z; }
    }

    float rel = 0.0f;
    const bool haveYaw = CamHookGetOpenXrToGameYaw(rel);
    float m[12] = {};
    unsigned camAge = 0;
    const bool haveCam = CamHookGetRenderedViewMatrix(m, &camAge);
    XrPosef head{}, grip{};
    const bool headOk = GetHeadPose(head);
    const bool gripOk = GetControllerGripPose(kRightHand, grip);
    float off[3] = { 0, 0, 0 };
    bool offOk = false;
    if (headOk && gripOk) {
        const XrVector3f hr = NeckPivotReportedPos(head);
        off[0] = grip.position.x - hr.x;
        off[1] = grip.position.y - hr.y;
        off[2] = grip.position.z - hr.z;
        offOk = off[0] * off[0] + off[1] * off[1] + off[2] * off[2] < kMaxHandOffsetM * kMaxHandOffsetM;
    }

    float pos[3] = { 0, 0, 0 }, vel[3] = { 0, 0, 0 };
    const bool valid = haveYaw && haveCam && offOk;
    if (valid) {
        const float c = std::cos(rel);
        float s = std::sin(rel);
        if (AimMirrorYaw) { s = -s; }
        float offW[3];
        YawMap(c, s, off, offW);
        pos[0] = m[3] + offW[0];
        pos[1] = m[7] + offW[1];
        pos[2] = m[11] + offW[2];
        YawMap(c, s, handTrk, vel);
    }
    PublishSnapshot(valid, pos, vel, now);

    _snprintf_s(out, outLen, _TRUNCATE,
                "hand speed %.2f m/s (fastest %s, %.0f ms before the let-go) | stroke dir %s (%.3f, %.3f, %.3f) | "
                "velocity world (%.3f, %.3f, %.3f) | start world (%.3f, %.3f, %.3f) | snapshot %s%s%s%s | yaw %.1f deg "
                "mirror %d | camera age %u ms | grip-head offset (%.3f, %.3f, %.3f)",
                speed, fast ? "found" : "NONE - no usable pair in the window (drop)", fast ? now - atMs : 0.0,
                stroke ? "found" : "NONE (fastest pair used)", dir.x, dir.y, dir.z, vel[0], vel[1], vel[2], pos[0],
                pos[1], pos[2], valid ? "VALID" : "*** NOT VALID *** -", haveYaw ? "" : " no OpenXR->game yaw yet",
                haveCam ? "" : " no rendered view matrix (none, or older than 150 ms)",
                offOk ? "" : (headOk && gripOk ? " grip-head offset over 1.5 m" : " head / right grip pose MISSING"),
                rel * 57.2957795f, AimMirrorYaw ? 1 : 0, camAge, off[0], off[1], off[2]);
}

}  // namespace

throwable::Params ThrowableParams()
{
    throwable::Params p{};
    p.fullSpeed = ThrowableFullSpeed > 0.01f ? ThrowableFullSpeed : 0.01f;
    p.stillSpeed = ThrowableStillSpeed;
    return p;
}

// COST per poll (the pad thread, ~250 Hz):
//   throwable_hand_enable=0 and nothing in flight: one bool load, one volatile load (g_resolved), one state compare.
//   armed: QueryPerformanceCounter; ~10 guarded loads (player, selected item, controller state - no call); two trigger
//   reads + the head pose + both GRIP poses (each a seqlock copy, controllers.cpp); one atan2 + sin + cos
//   (holster::LocalOffset); a 32-byte HandHistory::Push when the right grip moved; Machine::Update (a few compares).
//   The ItemType vfunc (one call - a leaf bitfield read, INV-571; +BDD9B0's mask applied inline) runs ONLY when the
//   selected item pointer changes or every 250 ms. No allocation, no RTTI walk, no GetModuleHandle (bases resolved once by ThrowableTick). Events (rare) log one
//   line; the Release event also runs HandHistory::Fastest / Stroke (at most 128 x a few samples) and the world mapping.
void ThrowablePadByte(unsigned char* p)
{
    const bool enabled = ThrowableHandEnable;
    if (!g_resolved || (!enabled && g_m.state() == throwable::State::Idle)) {
        g_disableSaid = false;
        if (g_localCtrl) { g_localCtrl = nullptr; }
        if (g_localPlayer) { g_localPlayer = nullptr; }
        if (g_selType != -1) { InterlockedExchange(&g_selType, -1); }
        if (g_hookSel) { InterlockedExchange(&g_hookSel, 0); }
        if (g_holdsL2) { InterlockedExchange(&g_holdsL2, 0); }
        if (g_releasePend) { InterlockedExchange(&g_releasePend, 0); }
        if (g_busy) { InterlockedExchange(&g_busy, 0); }
        return;
    }
    const double now = NowMs();
    const throwable::Params prm = ThrowableParams();
    g_padTid = static_cast<LONG>(GetCurrentThreadId());

    // ---- the game ----
    GameRead gr{};
    const int why = ReadGame(g_gd, &gr);
    if (why != g_lastWhy) {
        if (why != 0) {
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] REFUSED - %s. No throwable is treated as selected and the game state reads -1 "
                "until it reads again (decision state %s).", ReadWhyName(why), kStateName[static_cast<int>(g_m.state())]);
        } else if (g_lastWhy > 0) {
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] player READ again (player %p, throw controller %p)",
                reinterpret_cast<void*>(gr.player), reinterpret_cast<void*>(gr.ctrl));
        }
        g_lastWhy = why;
    }
    if (why != 0) {
        gr = GameRead{};
        g_inLevel = false;   // re-arms the IN LEVEL line (ruling R15)
    } else if (!g_inLevel) {
        // Ruling R15: once per level - the local player went null -> non-null. Carries the INV-570 route compare (the
        // process FIRST POLL line usually runs in the main menu, with no player to compare).
        g_inLevel = true;
        ++g_nInLevel;
        int why2 = 0;
        const uintptr_t r2 = ReadRoute2(g_gd, &why2);
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] IN LEVEL #%ld | route 1 player %p | route 2 player %p (%s) | routes %s | throw ctrl %p "
            "state %d | selected item %p | vehicle ctrl %p active %d",
            g_nInLevel, reinterpret_cast<void*>(gr.player), reinterpret_cast<void*>(r2), Route2WhyName(why2),
            r2 ? (gr.player == r2 ? "EQUAL" : "*** DIFFER ***") : "NOT COMPARED (route 2 null)",
            reinterpret_cast<void*>(gr.ctrl), gr.ctrl ? gr.state : -1, reinterpret_cast<void*>(gr.rawItem),
            reinterpret_cast<void*>(gr.vehCtrl), gr.inVehicle);
    }
    {
        // INV-572: entering / leaving a vehicle is an edge line, so a buggy run shows when L2 went back to the game.
        static int s_lastVeh = -1;
        const int veh = why == 0 ? (gr.inVehicle ? 1 : 0) : -1;
        if (veh != s_lastVeh) {
            if (veh >= 0 && s_lastVeh >= 0) {
                // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
                LOG("throwable: [THROWABLE] VEHICLE %s (VehicleController %p active byte +16 = %d) - %s",
                    veh ? "ENTERED" : "LEFT", reinterpret_cast<void*>(gr.vehCtrl), gr.inVehicle,
                    veh ? "no throwable is treated as selected: L2 / R2 pass to the game untouched (the buggy's brake)"
                        : "a selected throwable is ours again");
            }
            s_lastVeh = veh;
        }
    }

    // ---- the selected item (rulings m2 / R14, R5, R12) ----
    if (why == 0 && (gr.rawItem != g_evalRaw || now - g_evalMs >= kItemRecheckMs)) {
        uintptr_t thr = 0;
        int type = -1;
        const bool ok = EvalItem(gr.player, gr.rawItem, &thr, &type);
        g_evalRaw = gr.rawItem;
        g_evalMs = now;
        g_evalThr = thr;
        g_evalType = type;
        if (!ok && !g_evalFault) {
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] REFUSED - IsThrowable FAULT: the ItemType vfunc +0x%zX on item+%zu (the call "
                "the getter gamedll+0x%zX makes) faulted for item %p (player %p). No throwable is treated as selected "
                "until it reads again.", kTypeVfuncOff, kItemInfoOff, static_cast<size_t>(kSelThrowableRva),
                reinterpret_cast<void*>(gr.rawItem), reinterpret_cast<void*>(gr.player));
            g_evalFaultSaid = true;
        } else if (ok && g_evalFault && g_evalFaultSaid) {
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] IsThrowable READ again (item %p type %d)", reinterpret_cast<void*>(gr.rawItem),
                type);
        }
        g_evalFault = !ok;
    }
    // Ruling R16: also the throw controller readable (state != -1). Ruling R18 / INV-572: and NOT in a vehicle - the
    // VehicleController's active byte, the game's own in-vehicle test. (How the input layouts "Common" / "buggy_base"
    // are switched is still not read - the vehicle state is the established signal, not the layout switch itself.)
    const bool thrSelected = why == 0 && gr.ctrl != 0 && !g_evalFault && g_evalRaw == gr.rawItem && g_evalThr != 0 &&
                             g_evalThr == gr.rawItem && g_evalType != static_cast<int>(kTypeHook) && !gr.inVehicle;
    // grapple_hand (climbhand.cpp P7 round 3): the same reads, the hook's side - the grappling hook is the selected item, on foot.
    const bool hookSel = why == 0 && !g_evalFault && g_evalRaw == gr.rawItem && g_evalThr != 0 && g_evalThr == gr.rawItem &&
                         g_evalType == static_cast<int>(kTypeHook) && !gr.inVehicle;
    if ((g_hookSel != 0) != hookSel) { InterlockedExchange(&g_hookSel, hookSel ? 1 : 0); }

    // ---- the hands ----
    throwable::Input in{};
    in.tMs = now;
    in.throwableSelected = thrSelected;
    float lt = 0.0f, rt = 0.0f;
    in.ltOk = GetControllerTrigger(kLeftHand, lt);
    in.lt = lt;
    in.rtOk = GetControllerTrigger(kRightHand, rt);
    in.rt = rt;
    XrPosef head{}, lgrip{}, rgrip{};
    const bool headOk = GetHeadPose(head);
    const bool lgripOk = GetControllerGripPose(kLeftHand, lgrip);
    const bool rgripOk = GetControllerGripPose(kRightHand, rgrip);
    float hipDist = -1.0f;
    float hc[3];
    // Spec 2026-11-01-manual-reload-design goal 2: R2 at the RIGHT hip readies (the left hip is the ammo pouch now). The RIGHT hip
    // zone in use (holster_hip_from_calib's belt centre, else holster_rhip_center_*), the RIGHT grip.
    HolsterHipCentre(0, hc);
    if (headOk && rgripOk) {
        // The right hip's frame: body-locked when the holster body-lock has it latched, else head-relative.
        const float hp[3] = { head.position.x, head.position.y, head.position.z };
        const float hq[4] = { head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w };
        const float gp[3] = { rgrip.position.x, rgrip.position.y, rgrip.position.z };
        float lv[3];
        HolsterZoneLocal(kHolsterRHip, kHolsterCallerThrowable, hp, hq, gp, lv);
        const float dx = lv[0] - hc[0], dy = lv[1] - hc[1], dz = lv[2] - hc[2];
        hipDist = std::sqrt(dx * dx + dy * dy + dz * dz);
        g_inHip = g_inHip ? hipDist <= HolsterRHipRadius * holster::kExitScale : hipDist < HolsterRHipRadius;
    } else {
        g_inHip = false;   // fails closed, as the holster: no pose, not in the zone
    }
    in.rtInRHip = g_inHip;

    // Switch presses: D-pad right (weapon swap) / left (item swap) - the holster taps were OR'd into p above this call,
    // so the buttons word covers them and the real pad's own D-pad. Edges per bit, so a press still held when a re-ready
    // starts is not a second switch. An ITEM swap may land on another throwable: the decision's own re-ready test
    // (item changed AND a throwable selected AND state 0, within reReadyTimeoutMs) decides, so it is passed as "may".
    uint32_t& buttons = *reinterpret_cast<uint32_t*>(p);
    const uint32_t swBits = buttons & (kDpadRightBit | kDpadLeftBit);
    const uint32_t swEdge = swBits & ~g_prevSwitchBits;
    g_prevSwitchBits = swBits;
    in.switchPress = swEdge != 0;
    in.switchToThrowable = (swEdge & kDpadLeftBit) != 0;
    // The mod menu / calibration screen: the same predicate dlvr_MenuInputPolicy returns non-zero on (ovr2xr.cpp) -
    // called directly so this poll does not count twice in its MENUBLOCK tally. throwable_hand_enable switched to 0 with
    // a throw in flight is fed as a blocking menu: the decision then cancels before it lets go of L2.
    const bool menuOpen = OverlayVisible() || CalibScreenActive();
    // Body grab (plan 2026-10-23-body-grab Task 4): either hand holding a body is fed as a blocking menu too - the left hand holds
    // the throwable and the right trigger throws it, so neither may act while a hand grabs; a readied throwable cancels the safe
    // way (INV-566) and none readies. COST: two volatile loads.
    const bool grabbing = BodyGrabHandActive(0) || BodyGrabHandActive(1);
    // Game-menu input pass-through (spec 2026-10-25): an in-game menu (pause / inventory / map / shop) is fed as a blocking menu, so
    // a readied throwable cancels the safe way before the filter hands the triggers to the menu raw. COST: two volatile loads.
    const bool gameMenu = uipin::GameMenuOpen();
    in.menuBlocking = menuOpen || !enabled || grabbing || gameMenu;
    in.gameState = gr.ctrl ? gr.state : -1;
    in.gameReleased = gr.ctrl && gr.released != 0;
    in.selectedItem = why == 0 ? gr.rawItem : 0;
    // INV-574: the game's hand change has landed. A null controller reads gameState -1, which never re-readies anyway.
    const bool settled = why == 0 && gr.ctrl != 0 &&
                         (gr.ctrlActive == 0 || gr.ctrlItemHandle == 0 || gr.ctrlItem == gr.rawItem);
    in.switchSettled = settled;

    // The right hand's history: the RAW grip (a throw is fast - never the steadied copy), TRACKING space; mapped to world
    // only at the let-go with the yaw in force then, so a stick turn mid-swing is not read as hand speed. Pushed only when
    // the pose changed (poses are located once per frame; the poll is ~3x faster).
    if (rgripOk) {
        const float rp[3] = { rgrip.position.x, rgrip.position.y, rgrip.position.z };
        if (!g_havePushed || rp[0] != g_lastPushed[0] || rp[1] != g_lastPushed[1] || rp[2] != g_lastPushed[2]) {
            g_hist.Push(now, throwable::Vec3{ rp[0], rp[1], rp[2] });
            g_lastPushed[0] = rp[0]; g_lastPushed[1] = rp[1]; g_lastPushed[2] = rp[2];
            g_havePushed = true;
        }
    }

    if (!g_firstSaid) {
        g_firstSaid = true;
        int why2 = 0;
        const uintptr_t r2 = ReadRoute2(g_gd, &why2);
        static const char* const kR2Why[5] = { "read", "global null", "+1344 null", "+184 null", "FAULT" };
        float rel = 0.0f;
        const bool haveYaw = CamHookGetOpenXrToGameYaw(rel);
        float m[12] = {};
        unsigned camAge = 0;
        const bool haveCam = CamHookGetRenderedViewMatrix(m, &camAge);
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] FIRST POLL (logged unconditionally) | gamedll %p engine %p | route 1 player %p (%s) | "
            "route 2 player %p (%s) | routes %s | throw ctrl %p state %d released %d | selected item %p type %d, "
            "getter gamedll+0x%zX -> %p, throwable selected %d (Hook 47 excluded) | LT ok %d %.2f, RT ok %d %.2f | head "
            "%d, grip L %d R %d | hip dist %.3f m (zone (%.3f, %.3f, %.3f) r %.3f, exit x%.1f) in %d | menu %d | yaw %s "
            "%.1f deg | view matrix %s (age %u ms) | params full %.2f still %.3f press %.2f release %.2f",
            reinterpret_cast<void*>(g_gd), reinterpret_cast<void*>(g_eng), reinterpret_cast<void*>(gr.player),
            ReadWhyName(why), reinterpret_cast<void*>(r2), kR2Why[why2 >= 0 && why2 <= 4 ? why2 : 4],
            (gr.player && r2) ? (gr.player == r2 ? "EQUAL" : "*** DIFFER ***") : "NOT COMPARED (one is null)",
            reinterpret_cast<void*>(gr.ctrl), in.gameState, gr.released, reinterpret_cast<void*>(gr.rawItem),
            g_evalType, static_cast<size_t>(kSelThrowableRva), reinterpret_cast<void*>(g_evalThr),
            thrSelected ? 1 : 0, in.ltOk ? 1 : 0, in.lt, in.rtOk ? 1 : 0, in.rt, headOk ? 1 : 0, lgripOk ? 1 : 0,
            rgripOk ? 1 : 0, hipDist, hc[0], hc[1], hc[2], HolsterRHipRadius,   // the RIGHT hip since 2026-11-01 (text kept: logid)
            holster::kExitScale, g_inHip ? 1 : 0, menuOpen ? 1 : 0, haveYaw ? "ok" : "NOT YET", rel * 57.2957795f,
            haveCam ? "ok" : "NOT AVAILABLE", camAge, prm.fullSpeed, prm.stillSpeed, prm.trigPress, prm.trigRelease);
    }
    if (!enabled && !g_disableSaid) {
        g_disableSaid = true;
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] throwable_hand_enable=0 while %s - fed as a blocking menu so the cancel runs before "
            "L2 is let go; the filter goes inert once the decision is Idle.", kStateName[static_cast<int>(g_m.state())]);
    }

    // ---- the decision ----
    const throwable::Step st = g_m.Update(in, prm);

    if (st.requestCancel) {
        InterlockedExchange(&g_cancelPending, 1);   // Task 4's Update detour carries it out on the game thread
    } else if (st.state == throwable::State::Idle && g_cancelPending) {
        // Ruling R17: the decision left the cancel wait (state 0 read back, Lost, or a refused start finished) - a cancel
        // nobody consumed must not be carried out later against a fresh throw. A cancel wait never leaves the decision
        // Idle (BeginCancel keeps Readying / Readied), so Idle here means the wait is over.
        InterlockedExchange(&g_cancelPending, 0);
    }
    const unsigned char l2In = p[kL2Off], r2In = p[kR2Off];
    if (st.holdL2) {
        p[kL2Off] = 255;
    } else if (st.withholdL2 || in.throwableSelected) {
        // With a throwable selected the game sees L2 ONLY through the layer's hold: a partial pull below the decision's
        // press threshold could cross the game's own 0.5 (+BDE3A0) and ready a throw the decision never saw - and our
        // withhold at the press would then drop it from state 1/2, which throws (INV-566).
        p[kL2Off] = 0;
    }
    if (st.withholdR2) {
        p[kR2Off] = 0;
    }
    if (!st.passSwitch) {
        buttons &= ~(kDpadRightBit | kDpadLeftBit);
    }
    g_localCtrl = reinterpret_cast<void*>(gr.ctrl);
    g_localPlayer = reinterpret_cast<void*>(gr.player);
    {
        const LONG selType = thrSelected ? static_cast<LONG>(g_evalType) : -1;
        if (g_selType != selType) { InterlockedExchange(&g_selType, selType); }
    }
    if ((g_holdsL2 != 0) != st.holdL2) { InterlockedExchange(&g_holdsL2, st.holdL2 ? 1 : 0); }
    const bool releasing = st.state == throwable::State::Releasing;
    if ((g_releasePend != 0) != releasing) { InterlockedExchange(&g_releasePend, releasing ? 1 : 0); }
    {
        const bool busy = g_m.busy();   // ruling R24: read by the melee throw / VR swing triggers on their threads
        if ((g_busy != 0) != busy) { InterlockedExchange(&g_busy, busy ? 1 : 0); }
    }

    // ---- the event line ----
    if (st.ev != throwable::Event::None) {
        const int e = static_cast<int>(st.ev);
        ++g_nEv[e];
        char extra[640];
        extra[0] = '\0';
        if (st.snapshotRelease) {
            TakeSnapshot(now, prm, extra, sizeof(extra));
        }
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] %s #%ld%s -> %s | item %p type %d throwable %d | LT ok %d %.2f in hip %d (dist %.3f "
            "m), RT ok %d %.2f | game state %d -> %d, released %d | menu %d enable %d vehicle %d | switch edge 0x%02X passed %d | "
            "bytes L2 %u -> %u, R2 %u -> %u | cancel requested %d | throw ctrl active %d item %p (handle %p) settled %d%s%s",
            kEventName[e], g_nEv[e], st.reReady ? " (RE-READY after a switch)" : "",
            kStateName[static_cast<int>(st.state)], reinterpret_cast<void*>(gr.rawItem), g_evalType,
            thrSelected ? 1 : 0, in.ltOk ? 1 : 0, in.lt, in.rtInRHip ? 1 : 0, hipDist, in.rtOk ? 1 : 0, in.rt,
            g_prevGameState, in.gameState, gr.released, menuOpen ? 1 : 0, enabled ? 1 : 0, gr.inVehicle,
            static_cast<unsigned>(swEdge), st.passSwitch ? 1 : 0, static_cast<unsigned>(l2In),
            static_cast<unsigned>(p[kL2Off]), static_cast<unsigned>(r2In), static_cast<unsigned>(p[kR2Off]),
            st.requestCancel ? 1 : 0, gr.ctrlActive, reinterpret_cast<void*>(gr.ctrlItem),
            reinterpret_cast<void*>(gr.ctrlItemHandle), settled ? 1 : 0, extra[0] ? " | " : "", extra);
    }
    g_prevGameState = in.gameState;
}

// =====================================================================================================================
// Task 4 - the game-side detours. Each one acts ONLY for the local player's throw controller (ThrowableLocalCtrl, route 1
// [player+0xC28]) or the object OUR release produced; everything else is passed to the original untouched and counted.
// Nothing here runs before ThrowableTick has committed all four (g_resolved stays 0 until then, so the pad side never
// publishes a local controller and every detour is a pass-through).
// =====================================================================================================================
namespace {

float Len3(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Plain-data helpers under __try (no C++ unwinding - C2712).
int ReadCtrlState(uintptr_t ctrl)
{
    __try {
        return *reinterpret_cast<const volatile int*>(ctrl + kCtrlStateOff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// Mamfro's release: the object leaves on the controller's next update (INV-563: Update releases at [+0x60]).
bool ZeroReleaseTime(uintptr_t ctrl, float* before)
{
    __try {
        float* const f = reinterpret_cast<float*>(ctrl + kCtrlReleaseTimeOff);
        *before = *f;
        *f = 0.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// INV-573: the game's TryToRestoreItems (+DD4BF0) on the controller - it gives the hands back only while [+0x74] is set
// and [+0x7C] is not, then sets [+0x7C]. MUST run before [+0x74] is cleared (it tests that byte). Inside the caller's __try.
// *restBefore / *restAfter = [+0x7C] around the call. COST: one game call, once per cancel (an event, never per frame).
void RunRestore(uintptr_t ctrl, int* restBefore, int* restAfter)
{
    *restBefore = *reinterpret_cast<const volatile unsigned char*>(ctrl + kCtrlRestoredOff);
    g_restore(reinterpret_cast<void*>(ctrl));
    *restAfter = *reinterpret_cast<const volatile unsigned char*>(ctrl + kCtrlRestoredOff);
}

// The controller's own time, as the game writes it into [+0x78] (vfunc +48). Inside the caller's __try.
float CtrlTime(uintptr_t ctrl)
{
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ctrl);
    return reinterpret_cast<CtrlTimeFn>(*reinterpret_cast<const uintptr_t*>(vt + kCtrlTimeVfuncOff))(
        reinterpret_cast<void*>(ctrl));
}

// Ruling R23: a cancel at state 0 clears the pending-start byte [+0x74] - and, INV-573, first runs the game's restore,
// because +DD3500 has already put the throwable in the hand by then. false = a fault.
bool ClearAutoStart(uintptr_t ctrl, int* before, int* restBefore, int* restAfter)
{
    if (!g_restore) { return false; }   // unreachable while g_resolved needs the install to have verified it; FAULT
    __try {
        volatile unsigned char* const b = reinterpret_cast<volatile unsigned char*>(ctrl + kCtrlAutoStartOff);
        *before = *b;
        RunRestore(ctrl, restBefore, restAfter);
        *b = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The cancel (rulings R2a / R6 / R7), in the game's own 241 order (+DD1120, INV-573): the restore (+DD4BF0), [+0x70] = -1,
// byte [+0x74] = 0, [+0x78] = the controller's time, then set-state 0 through the trampoline (so the set-state detour does
// not count it as the game's). One call on the game thread - the pad keeps L2 held until state 0 reads back (INV-566).
// The 241 cancel's first step, ClearThrowMulti (player+2850 / +2856, the ThrowingStar multi-throw), is NOT done: the game's
// +BA4C70 clears it itself once [+0x74] is clear (INV-573 addendum (2)). false = a fault.
bool DoCancel(uintptr_t ctrl, int* sBefore, int* sAfter, int* actBefore, int* autoBefore, int* restBefore, int* restAfter)
{
    if (!g_origSetState || !g_restore) { return false; }   // unreachable while g_resolved needs both; reported as FAULTED
    __try {
        *sBefore = *reinterpret_cast<const volatile int*>(ctrl + kCtrlStateOff);
        *actBefore = *reinterpret_cast<const volatile int*>(ctrl + kCtrlActionOff);
        *autoBefore = *reinterpret_cast<const volatile unsigned char*>(ctrl + kCtrlAutoStartOff);
        RunRestore(ctrl, restBefore, restAfter);
        *reinterpret_cast<volatile int*>(ctrl + kCtrlActionOff) = -1;
        *reinterpret_cast<volatile unsigned char*>(ctrl + kCtrlAutoStartOff) = 0;
        *reinterpret_cast<volatile float*>(ctrl + kCtrlTimeOff) = CtrlTime(ctrl);
        g_origSetState(reinterpret_cast<void*>(ctrl), 0);
        *sAfter = *reinterpret_cast<const volatile int*>(ctrl + kCtrlStateOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct LaunchRead {
    uintptr_t thrower = 0;
    int launched = 0;
    int zeroVel = 0;     // vel's three floats == the game's zero constant (the compare launch itself makes, +EEAA1C)
    int zeroPtr = 0;     // vel == &the constant (slot 74's own argument)
};

bool ReadLaunch(uintptr_t obj, const float* vel, LaunchRead* r)
{
    const uintptr_t zeroRva = ZeroVecRva();
    __try {
        const uintptr_t h = *reinterpret_cast<const volatile uintptr_t*>(obj + kObjThrowerHandle);
        r->thrower = h ? *reinterpret_cast<const volatile uintptr_t*>(obj + kObjThrower) : 0;
        r->launched = *reinterpret_cast<const volatile unsigned char*>(obj + kObjLaunched);
        const float* z = reinterpret_cast<const float*>(g_gd + zeroRva);
        r->zeroPtr = vel == z ? 1 : 0;
        r->zeroVel = (vel && vel[0] == z[0] && vel[1] == z[1] && vel[2] == z[2]) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The object's own lob velocity through its slot 124 (+EEAB60), as brief step 3 asks. That vfunc ALSO ADDS the lob into
// the object's velocity [+0x100] through slot 125 (+EEBB30 `a1[64] += ...`, tw4_gamedll_decomp.c:4345 - INV-564's
// "adds"), so the three floats are saved first and put back after: the original launch then adds OUR velocity to the
// untouched value, not to the lob. (The angular velocity slot 125 sets is set again by the original from ours.)
bool OwnVelocity(uintptr_t obj, float own[3], float gamePos[3], int* posOk)
{
    __try {
        float* const v = reinterpret_cast<float*>(obj + kObjVelOff);
        const float s0 = v[0], s1 = v[1], s2 = v[2];
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        const OwnVelFn fn = *reinterpret_cast<const OwnVelFn*>(vt + kOwnVelVfuncOff);
        float tmp[4] = {};
        const float* r = fn(reinterpret_cast<void*>(obj), tmp);
        own[0] = r[0]; own[1] = r[1]; own[2] = r[2];
        v[0] = s0; v[1] = s1; v[2] = s2;
        // The spawn position the game chose, for the log only: vfunc +624, the getter launch itself calls (+EEA9F0 :150).
        using PosFn = float*(__fastcall*)(void*, float*);
        const PosFn pf = *reinterpret_cast<const PosFn*>(vt + 624);
        float ptmp[4] = {};
        const float* p = pf(reinterpret_cast<void*>(obj), ptmp);
        gamePos[0] = p[0]; gamePos[1] = p[1]; gamePos[2] = p[2];
        *posOk = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The object's world position through vfunc +624 - the getter launch itself calls (+EEA9F0 :150). Ruling R20 read-back.
bool ReadObjPos(uintptr_t obj, float pos[3])
{
    __try {
        using PosFn = float*(__fastcall*)(void*, float*);
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        const PosFn pf = *reinterpret_cast<const PosFn*>(vt + 624);
        float tmp[4] = {};
        const float* p = pf(reinterpret_cast<void*>(obj), tmp);
        pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MoveObject(uintptr_t obj, const float p[3])
{
    __try {
        g_setWorldPos(reinterpret_cast<void*>(obj + kObjControlOff), p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t ReadPtr(uintptr_t at)
{
    __try {
        return *reinterpret_cast<const volatile uintptr_t*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool TargetWorld(uintptr_t model, const float local[3], float world[3])
{
    __try {
        float tmp[4] = {};
        const float* w = g_localToWorld(reinterpret_cast<const void*>(model + kObjControlOff), tmp, local);
        world[0] = w[0]; world[1] = w[1]; world[2] = w[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Our release token is fresh: the set-state detour applied our release within kSnapMaxMs.
bool TokenFresh(double now, LONG64* tokenOut)
{
    const LONG64 t = g_relTokenMs;
    if (tokenOut) { *tokenOut = t; }
    return t != 0 && now - static_cast<double>(t) <= kSnapMaxMs;
}

// ---- set-state +DD33A0 (ctrl, s). Rare: a few calls per throw / per controller switch. ----
void __fastcall SetStateHook(void* ctrl, int s)
{
    void* const local = g_localCtrl;
    const bool mine = ctrl != nullptr && ctrl == local;
    const int prev = mine ? ReadCtrlState(reinterpret_cast<uintptr_t>(ctrl)) : -1;
    g_origSetState(ctrl, s);
    const bool intoThrow = s >= 3 && s <= 5 && !(prev >= 3 && prev <= 5);
    if (!mine || !intoThrow) {
        CountCall(g_stSetState, false);
        return;
    }
    if (ThrowableReleasePending()) {
        float before = -1.0f;
        const bool ok = ZeroReleaseTime(reinterpret_cast<uintptr_t>(ctrl), &before);
        const double now = NowMs();
        if (ok) {
            g_relTokenCtrl = ctrl;
            InterlockedExchange64(&g_relTokenMs, static_cast<LONG64>(now));
        }
        CountCall(g_stSetState, ok);
        const LONG n = InterlockedIncrement(&g_nRelApplied);
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] RELEASE APPLIED #%ld | ctrl %p state %d -> %d | release time [+0x60] %.3f -> %s | "
            "thread %lu (pad thread %ld) | the object leaves on the controller's next update",
            n, ctrl, prev, s, before, ok ? "0.000" : "*** FAULT - not written, the game's own timing stays ***",
            GetCurrentThreadId(), static_cast<long>(g_padTid));
    } else {
        CountCall(g_stSetState, false);
        const LONG n = InterlockedIncrement(&g_nUnexpected);
        // Ruling R8: the game threw on its own (its input block in a cutscene, a keyboard / mouse event) - not touched.
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=yes
        LOG("throwable: [THROWABLE] UNEXPECTED RELEASE #%ld | ctrl %p state %d -> %d with NO RT let-go pending (the game "
            "threw by itself: its own input block or a keyboard / mouse event) - passed through untouched | thread %lu",
            n, ctrl, prev, s, GetCurrentThreadId());
    }
}

// ---- launch +EEA9F0 (obj, vel, target). Once per thrown object, any actor (AI grenades / ThrowByActor included). ----
void __fastcall LaunchHook(void* obj, const float* vel, const float* target)
{
    const uintptr_t o = reinterpret_cast<uintptr_t>(obj);
    const uintptr_t player = reinterpret_cast<uintptr_t>(static_cast<void*>(g_localPlayer));
    LaunchRead lr{};
    const bool readOk = obj != nullptr && ReadLaunch(o, vel, &lr);
    const double now = NowMs();
    LONG64 token = 0;
    const bool tokenOk = TokenFresh(now, &token);
    if (!readOk || player == 0 || lr.thrower != player) {
        CountCall(g_stLaunch, false);
        InterlockedIncrement(&g_nLaunchOther);
        if (tokenOk) {
            // Ruling R19: our release is fresh, so this may be OUR object refused on the thrower test (INV-567 / INV-570:
            // [obj+888] == route 1 is not measured). Never silent: both pointers, the read result.
            const LONG n = InterlockedIncrement(&g_nLaunchLocalNotTaken);
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] LAUNCH REFUSED #%ld - %s | obj %p thrower [obj+888] %p vs route 1 player %p | our "
                "release %.0f ms ago | the game's own launch runs unchanged (an AI throw in the same window also lands here)",
                n, !readOk ? "the object read FAULTED (or obj null)" : (player == 0 ? "no local player published"
                                                                                     : "the thrower is NOT the route 1 player"),
                obj, reinterpret_cast<void*>(lr.thrower), reinterpret_cast<void*>(player), now - static_cast<double>(token));
        }
        g_origLaunch(obj, vel, target);
        return;
    }
    // The local player's object. Ours only with every check below; otherwise named and passed through.
    throwable::Vec3 sp{}, sv{};
    double age = -1.0;
    const bool snapOk = ThrowableReleaseSnapshot(sp, sv, age);
    const char* why = nullptr;
    if (!tokenOk) {
        why = token ? "our release is older than 300 ms" : "no release of ours (the game threw by itself)";
    } else if (lr.launched) {
        why = "already launched (byte +0x3B0 set)";
    } else if (!lr.zeroVel) {
        why = "the velocity is not the zero constant (player byte +10901's back-throw?)";
    } else if (target) {
        why = "a target was passed";
    } else if (!snapOk) {
        why = "no valid let-go snapshot (named in its RELEASE line)";
    } else if (age < 0.0 || age > kSnapMaxMs) {
        why = "the let-go snapshot is older than 300 ms";
    }
    float own[3] = {}, gamePos[3] = {};
    int posOk = 0;
    if (!why && !OwnVelocity(o, own, gamePos, &posOk)) {
        why = "the object's own velocity (vfunc +992) FAULTED";
    }
    // The override's start point, and its distance from where the game spawned the object (R20's bound).
    const float ownSpeed = Len3(own);
    const throwable::Vec3 tv = throwable::ThrownVelocity(sv, ownSpeed, ThrowableParams());
    const float thrown[3] = { tv.x, tv.y, tv.z };
    const float tl = Len3(thrown);
    const float k = tl > 1e-6f ? kStartAheadM / tl : 0.0f;
    const float start[3] = { sp.x + thrown[0] * k, sp.y + thrown[1] * k, sp.z + thrown[2] * k };
    const float dx = start[0] - gamePos[0], dy = start[1] - gamePos[1], dz = start[2] - gamePos[2];
    const float gap = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!why && gap > kMaxStartGapM) {
        // Ruling R20: the hand is within arm's reach of the game's own spawn point; further = a wrong frame (Task 3 C6).
        // A live grenade must never appear away from the player - the WHOLE override is refused (no move, our velocity
        // dropped), the game's own lob runs.
        why = "the hand's start is more than 1.0 m from the game's spawn point (wrong world frame?) - no move, no velocity";
        InterlockedExchange64(&g_relTokenMs, 0);
    }
    if (why) {
        CountCall(g_stLaunch, false);
        const LONG n = InterlockedIncrement(&g_nLaunchLocalNotTaken);
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] LAUNCH REFUSED #%ld - %s | obj %p vel %p zero %d (ptr %d) target %p launched %d | "
            "snapshot %s age %.0f ms | game spawn %s (%.3f, %.3f, %.3f) vs hand start (%.3f, %.3f, %.3f) gap %.3f m (limit "
            "%.1f) | the game's own launch runs unchanged",
            n, why, obj, static_cast<const void*>(vel), lr.zeroVel, lr.zeroPtr, static_cast<const void*>(target),
            lr.launched, snapOk ? "valid" : "NOT VALID", age, posOk ? "read" : "UNREAD", gamePos[0], gamePos[1],
            gamePos[2], start[0], start[1], start[2], gap, kMaxStartGapM);
        g_origLaunch(obj, vel, target);
        return;
    }
    InterlockedExchange64(&g_relTokenMs, 0);   // one launch per release
    const bool moved = MoveObject(o, start);
    float back1[3] = {}, back2[3] = {};
    const bool b1 = ReadObjPos(o, back1);      // ruling R20: what SetWorldPosition left, read back
    g_origLaunch(obj, thrown, target);
    const bool b2 = ReadObjPos(o, back2);      // and after the original launch ran (+EEBC00, vfunc +1008)
    const float e1x = back1[0] - start[0], e1y = back1[1] - start[1], e1z = back1[2] - start[2];
    const float e2x = back2[0] - start[0], e2y = back2[1] - start[1], e2z = back2[2] - start[2];
    CountCall(g_stLaunch, true);
    const LONG n = InterlockedIncrement(&g_nLaunchOurs);
    const float hv[3] = { sv.x, sv.y, sv.z };
    // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
    LOG("throwable: [THROWABLE] LAUNCH #%ld (ours) | obj %p | own lob (%.3f, %.3f, %.3f) |%.2f| m/s | hand (%.3f, %.3f, "
        "%.3f) |%.2f| m/s | thrown (%.3f, %.3f, %.3f) |%.2f| m/s | game spawn (%.3f, %.3f, %.3f) -> requested start "
        "(%.3f, %.3f, %.3f), %.3f m away%s | READ BACK after the move %s (%.3f, %.3f, %.3f) off %.3f m, after the launch "
        "%s (%.3f, %.3f, %.3f) off %.3f m | snapshot age %.0f ms | thread %lu",
        n, obj, own[0], own[1], own[2], ownSpeed, hv[0], hv[1], hv[2], Len3(hv), thrown[0], thrown[1], thrown[2], tl,
        gamePos[0], gamePos[1], gamePos[2], start[0], start[1], start[2], gap,
        moved ? "" : " *** SetWorldPosition FAULTED - not moved ***", b1 ? "read" : "*** FAULT ***", back1[0], back1[1],
        back1[2], std::sqrt(e1x * e1x + e1y * e1y + e1z * e1z), b2 ? "read" : "*** FAULT ***", back2[0], back2[1],
        back2[2], std::sqrt(e2x * e2x + e2y * e2y + e2z * e2z), age, GetCurrentThreadId());
}

// ---- aim point +DBCCC0 (meleeCtrl, out, flag). Per frame from the melee update AND from the throw's +DD44A0. ----
// COST when it is not the throw's call: one _ReturnAddress compare + the counter.
void* __fastcall AimPointHook(void* mc, float* out, char flag)
{
    static const uintptr_t s_retRelease = ::dlvr::buildrva::Gd(kAimRetRelease);   // the loaded build's; resolved once
    static const uintptr_t s_retUpdate = ::dlvr::buildrva::Gd(kAimRetUpdate);
    const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    void* const res = g_origAimPoint(mc, out, flag);
    const bool throwPath = ra == g_gd + s_retRelease || ra == g_gd + s_retUpdate;
    if (!throwPath) {
        CountCall(g_stAim, false);
        InterlockedIncrement(&g_nAimNotThrow);
        return res;
    }
    const uintptr_t player = reinterpret_cast<uintptr_t>(static_cast<void*>(g_localPlayer));
    const double now = NowMs();
    LONG64 token = 0;
    throwable::Vec3 sp{}, sv{};
    double age = -1.0;
    // No fresh release of ours: not ours, no line - this is every frame of a readied star (+DD44A0); the counters carry it.
    if (!TokenFresh(now, &token)) {
        CountCall(g_stAim, false);
        return res;
    }
    static volatile LONG64 s_loggedToken = 0;   // one line per release (the per-frame caller repeats)
    // Ruling R19: our release IS fresh, so every refusal below is named once per release.
    const LONG selType = g_selType;
    const uintptr_t melee = player ? ReadPtr(player + kMeleeCtrlOff) : 0;
    const bool snapOk = ThrowableReleaseSnapshot(sp, sv, age);
    const char* why = nullptr;
    if (selType != kTypeThrowingStar) {
        why = "the selected type is not ThrowingStar 40";
    } else if (player == 0) {
        why = "no local player published";
    } else if (out == nullptr) {
        why = "the out pointer is null";
    } else if (melee != reinterpret_cast<uintptr_t>(mc)) {
        why = "rcx is NOT the route 1 player's melee controller [player+0xC10]";
    } else if (!snapOk) {
        why = "no valid let-go snapshot (named in its RELEASE line)";
    } else if (age < 0.0 || age > kSnapMaxMs) {
        why = "the let-go snapshot is older than 300 ms";
    }
    const float v[3] = { sv.x, sv.y, sv.z };
    const float speed = Len3(v);
    if (!why && speed < 1e-3f) {
        why = "the hand's velocity is zero (a drop)";
    }
    const bool sayIt = InterlockedExchange64(&s_loggedToken, token) != token;
    if (why) {
        CountCall(g_stAim, false);
        if (sayIt) {
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] AIM REFUSED - %s | type %ld | rcx %p vs [player+0xC10] %p (player %p) | snapshot %s "
                "age %.0f ms | the game's aim point stays, target %p | caller +%zX",
                why, static_cast<long>(selType), mc, reinterpret_cast<void*>(melee), reinterpret_cast<void*>(player),
                snapOk ? "valid" : "NOT VALID", age, res, static_cast<size_t>(ra - g_gd));
        }
        return res;
    }
    const float dir[3] = { v[0] / speed, v[1] / speed, v[2] / speed };
    float dist = speed * kAimReachPerSpeed;
    dist = dist < kAimNearestM ? kAimNearestM : (dist > kAimFarthestM ? kAimFarthestM : dist);
    const float start[3] = { sp.x, sp.y, sp.z };
    if (res) {
        // KEEP_TARGET: a locked target (out is then in ITS local space, +DBD1EC) within 12 deg of the hand's line stays.
        float w[3] = {};
        if (TargetWorld(reinterpret_cast<uintptr_t>(res), out, w)) {
            const float t[3] = { w[0] - start[0], w[1] - start[1], w[2] - start[2] };
            const float tl = Len3(t);
            const float c = tl > 1e-6f ? (t[0] * dir[0] + t[1] * dir[1] + t[2] * dir[2]) / tl : -1.0f;
            const float deg = std::acos(c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c)) * 57.2957795f;
            if (deg <= kKeepTargetDeg) {
                CountCall(g_stAim, true);
                InterlockedIncrement(&g_nAimKept);
                if (sayIt) {
                    // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
                    LOG("throwable: [THROWABLE] AIM KEPT TARGET %p - %.1f deg off the hand's line (<= %.0f), world "
                        "(%.3f, %.3f, %.3f) | caller +%zX", res, deg, kKeepTargetDeg, w[0], w[1], w[2],
                        static_cast<size_t>(ra - g_gd));
                }
                return res;
            }
        }
    }
    const float p[3] = { start[0] + dir[0] * dist, start[1] + dir[1] * dist, start[2] + dir[2] * dist };
    const float g0 = out[0], g1 = out[1], g2 = out[2];
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    CountCall(g_stAim, true);
    InterlockedIncrement(&g_nAimOurs);
    if (sayIt) {
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] AIM (ours) | game point (%.3f, %.3f, %.3f)%s target %p dropped -> (%.3f, %.3f, %.3f), "
            "%.2f m along the hand's line (hand %.2f m/s) | caller +%zX | thread %lu",
            g0, g1, g2, res ? " LOCAL to its" : " world,", res, p[0], p[1], p[2], dist, speed,
            static_cast<size_t>(ra - g_gd), GetCurrentThreadId());
    }
    return nullptr;
}

// ---- the flight +6E6D70 (DroppedItem obj, char again) - issue 4. Every physics step of every DroppedItem. ----
// The object's thrower, and whether this call will compute a flight (the original's own entry test, +6E6D70 :73-77).
// The type is NOT read here (a vcall): only once the thrower is ours. False = a fault.
bool ReadDrop(uintptr_t obj, char again, uintptr_t* thrower, int* computes)
{
    const uintptr_t zeroRva = ZeroVecRva();
    __try {
        const uintptr_t h = *reinterpret_cast<const volatile uintptr_t*>(obj + kDropThrowerHandle);
        *thrower = h ? *reinterpret_cast<const volatile uintptr_t*>(obj + kDropThrower) : 0;
        const float* const t = reinterpret_cast<const float*>(obj + kDropTargetOff);
        const float* const z = reinterpret_cast<const float*>(g_gd + zeroRva);
        const float sp = *reinterpret_cast<const volatile float*>(obj + kDropSpeedOff);
        *computes = (t[0] != z[0] || t[1] != z[1] || t[2] != z[2] || again || sp > 0.0f) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// -1 = a fault. The same call +6E6D70 makes (:95).
int ReadDropType(uintptr_t obj)
{
    __try {
        const uintptr_t info = obj + kDropItemInfo;
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(info);
        using TypeFn = int(__fastcall*)(uintptr_t);
        return (*reinterpret_cast<const TypeFn*>(vt + kDropTypeVfunc))(info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool DropVelGet(uintptr_t obj, float v[3])
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        float tmp[4] = {};
        const float* r = (*reinterpret_cast<const VecGetFn*>(vt + kGetLinVelVfunc))(reinterpret_cast<void*>(obj), tmp);
        v[0] = r[0]; v[1] = r[1]; v[2] = r[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DropVelSet(uintptr_t obj, const float v[3])
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        (*reinterpret_cast<const VecSetFn*>(vt + kSetLinVelVfunc))(reinterpret_cast<void*>(obj), v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// COST, the fast path (every physics step of every DroppedItem): one bool load and one volatile 64-bit load - no release
// token (the normal case) or the switch off = straight to the original. Past it (a token set, <= 300 ms after OUR RT
// let-go of a throw): one QPC read, three pointer reads + three compares; the type vcall only for the local player's
// object that will compute. Ours: two position reads, one SetWorldPosition, three velocity vcalls, one log line.
void __fastcall FlightHook(void* obj, char again)
{
    if (!ThrowableStarHandVelocity || g_relTokenMs == 0) {
        g_origFlight(obj, again);
        return;
    }
    const uintptr_t o = reinterpret_cast<uintptr_t>(obj);
    const double now = NowMs();
    LONG64 token = 0;
    if (!TokenFresh(now, &token)) {
        g_origFlight(obj, again);
        return;
    }
    const uintptr_t player = reinterpret_cast<uintptr_t>(static_cast<void*>(g_localPlayer));
    uintptr_t thrower = 0;
    int computes = 0;
    if (!obj || !ReadDrop(o, again, &thrower, &computes) || player == 0 || thrower != player || !computes) {
        // Another actor's / item's step, or ours not computing a flight this step: not ours, no line.
        g_origFlight(obj, again);
        return;
    }
    const int type = ReadDropType(o);
    throwable::Vec3 sp{}, sv{};
    double age = -1.0;
    const bool snapOk = ThrowableReleaseSnapshot(sp, sv, age);
    const char* why = nullptr;
    if (type != kTypeThrowingStar) {
        why = type < 0 ? "the item type read FAULTED" : "the item is not ThrowingStar 40 (a flare / other drop is not ours)";
    } else if (!snapOk) {
        why = "no valid let-go snapshot (named in its RELEASE line)";
    } else if (age < 0.0 || age > kSnapMaxMs) {
        why = "the let-go snapshot is older than 300 ms";
    }
    // The start: the hand, kStartAheadM along the throw's direction (the grenades' rule, LaunchHook). The direction is the
    // rule's own (ThrownVelocity with a unit speed - a still hand = straight down).
    const throwable::Params prm = ThrowableParams();
    const throwable::Vec3 d1 = throwable::ThrownVelocity(sv, 1.0f, prm);
    const float dir[3] = { d1.x, d1.y, d1.z };
    const float dl = Len3(dir);
    const float k = dl > 1e-6f ? kStartAheadM / dl : 0.0f;
    const float start[3] = { sp.x + dir[0] * k, sp.y + dir[1] * k, sp.z + dir[2] * k };
    float gamePos[3] = {};
    const bool posOk = ReadObjPos(o, gamePos);
    const float gx = start[0] - gamePos[0], gy = start[1] - gamePos[1], gz = start[2] - gamePos[2];
    const float gap = std::sqrt(gx * gx + gy * gy + gz * gz);
    if (!why && !posOk) {
        why = "the object's position read (vfunc +624) FAULTED";
    } else if (!why && gap > kMaxStartGapM) {
        why = "the hand's start is more than 1.0 m from the game's spawn point (wrong world frame?) - no move, no velocity";
    }
    // One flight per release, taken or refused (the launch detour's rule).
    InterlockedExchange64(&g_relTokenMs, 0);
    if (why) {
        CountCall(g_stFlight, false);
        const LONG n = InterlockedIncrement(&g_nFlightRefused);
        // [PROBE: THROWABLE] gate=throwable_star_hand_velocity  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] STAR FLIGHT REFUSED #%ld - %s | obj %p type %d | snapshot %s age %.0f ms | game spawn %s "
            "(%.3f, %.3f, %.3f) vs hand start (%.3f, %.3f, %.3f) gap %.3f m | the game's own flight runs unchanged | thread %lu",
            n, why, obj, type, snapOk ? "valid" : "NOT VALID", age, posOk ? "read" : "UNREAD", gamePos[0], gamePos[1],
            gamePos[2], start[0], start[1], start[2], gap, GetCurrentThreadId());
        g_origFlight(obj, again);
        return;
    }
    const bool moved = MoveObject(o, start);
    float back1[3] = {};
    const bool b1 = ReadObjPos(o, back1);
    g_origFlight(obj, again);
    float gv[3] = {};
    const bool gvOk = DropVelGet(o, gv);
    const float gameSpeed = Len3(gv);
    if (!gvOk || !std::isfinite(gameSpeed) || gameSpeed < 0.1f) {
        CountCall(g_stFlight, false);
        const LONG n = InterlockedIncrement(&g_nFlightRefused);
        // [PROBE: THROWABLE] gate=throwable_star_hand_velocity  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] STAR FLIGHT REFUSED #%ld - the game's velocity after its flight calc %s (%.3f, %.3f, %.3f) "
            "|%.3f| m/s - not overridden (the start WAS moved: %s) | obj %p | thread %lu",
            n, gvOk ? "is zero / not finite" : "read FAULTED", gv[0], gv[1], gv[2], gameSpeed,
            moved ? "yes" : "NO, SetWorldPosition FAULTED", obj, GetCurrentThreadId());
        return;
    }
    const throwable::Vec3 tv = throwable::ThrownVelocity(sv, gameSpeed, prm);
    const float thrown[3] = { tv.x, tv.y, tv.z };
    const bool setOk = DropVelSet(o, thrown);
    float back2[3] = {};
    const bool b2 = DropVelGet(o, back2);
    const float e1x = back1[0] - start[0], e1y = back1[1] - start[1], e1z = back1[2] - start[2];
    CountCall(g_stFlight, true);
    const LONG n = InterlockedIncrement(&g_nFlightOurs);
    const float hv[3] = { sv.x, sv.y, sv.z };
    // [PROBE: THROWABLE] gate=throwable_star_hand_velocity  site=HEADSET  log-only=no
    LOG("throwable: [THROWABLE] STAR FLIGHT #%ld (ours) | obj %p | game velocity (%.3f, %.3f, %.3f) |%.2f| m/s | hand (%.3f, "
        "%.3f, %.3f) |%.2f| m/s | thrown (%.3f, %.3f, %.3f) |%.2f| m/s%s | game spawn (%.3f, %.3f, %.3f) -> start (%.3f, %.3f, "
        "%.3f), %.3f m away%s, READ BACK %s off %.3f m | velocity READ BACK %s (%.3f, %.3f, %.3f) | snapshot age %.0f ms | "
        "thread %lu",
        n, obj, gv[0], gv[1], gv[2], gameSpeed, hv[0], hv[1], hv[2], Len3(hv), thrown[0], thrown[1], thrown[2], Len3(thrown),
        setOk ? "" : " *** SetLinearVel FAULTED ***", gamePos[0], gamePos[1], gamePos[2], start[0], start[1], start[2], gap,
        moved ? "" : " *** SetWorldPosition FAULTED - not moved ***", b1 ? "read" : "*** FAULT ***",
        std::sqrt(e1x * e1x + e1y * e1y + e1z * e1z), b2 ? "read" : "*** FAULT ***", back2[0], back2[1], back2[2], age,
        GetCurrentThreadId());
}

// ---- the throw controller's Update +DD1640 (ctrl, dt): once per update of an active throw controller. ----
// COST: a pointer compare; for the local controller one volatile load of the cancel flag; the counter.
void __fastcall UpdateHook(void* ctrl, float dt)
{
    void* const local = g_localCtrl;
    if (ctrl != nullptr && ctrl == local && ThrowableTakeCancelRequest()) {
        // Ruling R22: set-state 0 is read for states 1-2 only (INV-563); mid-release (3-5) the request is dropped - the
        // throw completes - and named. -2 = the state read faulted.
        const int st = ReadCtrlState(reinterpret_cast<uintptr_t>(ctrl));
        if (st == 0) {
            // Ruling R23: at 0 a start may still be pending ([+0x74] = 1 from +DD3500, turned into state 1 by a later
            // Start). Clear that byte ONLY - no set-state (nothing to leave). The pad keeps L2 held until 1/2 has been seen
            // and 0 reads back, or readyTimeoutMs passes.
            int before = -1, rb = -1, ra = -1;
            const bool ok = ClearAutoStart(reinterpret_cast<uintptr_t>(ctrl), &before, &rb, &ra);
            CountCall(g_stUpdate, ok);
            const LONG n = InterlockedIncrement(&g_nCancelAtZero);
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] CANCEL AT 0 #%ld - state 0, pending start byte [+0x74] %d -> %s (no set-state) | "
                "restore +DD4BF0 run, restored [+0x7C] %d -> %d (1 = the hands were given back or already were) | "
                "ctrl %p | thread %lu (pad thread %ld)",
                n, before, ok ? "0" : "*** FAULT - not written ***", rb, ra, ctrl, GetCurrentThreadId(),
                static_cast<long>(g_padTid));
            g_origUpdate(ctrl, dt);
            return;
        }
        if (st != 1 && st != 2) {
            CountCall(g_stUpdate, false);
            const LONG n = InterlockedIncrement(&g_nCancelSkipped);
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] CANCEL SKIPPED #%ld - the controller's state is %d%s (a cancel is applied in 1 / "
                "2; at 0 only the pending-start byte is cleared) | ctrl %p | request dropped; the pad keeps L2 as its own state decides | thread %lu",
                n, st, st == -2 ? " (READ FAULTED)" : "", ctrl, GetCurrentThreadId());
            g_origUpdate(ctrl, dt);
            return;
        }
        int sb = -1, sa = -1, ab = 0, bb = 0, rb = -1, ra = -1;
        const bool ok = DoCancel(reinterpret_cast<uintptr_t>(ctrl), &sb, &sa, &ab, &bb, &rb, &ra);
        CountCall(g_stUpdate, ok);
        const LONG n = ok ? InterlockedIncrement(&g_nCancelDone) : InterlockedIncrement(&g_nCancelFault);
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] CANCEL #%ld %s on the game thread (Update detour, before the original) | ctrl %p state "
            "before %d after %d | action [+0x70] %d -> -1 | auto-start [+0x74] %d -> 0 | restore +DD4BF0 run, restored "
            "[+0x7C] %d -> %d (1 = the hands were given back or already were) | thread %lu (pad thread %ld) | L2 "
            "stays held until the pad reads 0 back",
            n, ok ? "CARRIED OUT" : "*** FAULTED ***", ctrl, sb, sa, ab, bb, rb, ra, GetCurrentThreadId(),
            static_cast<long>(g_padTid));
    } else {
        CountCall(g_stUpdate, false);
    }
    g_origUpdate(ctrl, dt);
}

struct HookSite {
    const char* name;
    uintptr_t rva;
    const unsigned char* bytes;
    size_t n;
    void* repl;
    void** orig;
    bool live;
};

HookSite g_sites[4] = {
    // [LOADBEARING: THROWABLE set-state] gamedll+0xDD33A0 - the release-time zero; INV-568 SAFE 14
    { "throw set-state", kSetStateRva, kSetStateBytes, sizeof(kSetStateBytes), reinterpret_cast<void*>(&SetStateHook),
      reinterpret_cast<void**>(&g_origSetState), false },
    // [LOADBEARING: THROWABLE launch] gamedll+0xEEA9F0 - the hand's velocity / start; INV-568 SAFE 15
    { "throw launch", kLaunchRva, kLaunchBytes, sizeof(kLaunchBytes), reinterpret_cast<void*>(&LaunchHook),
      reinterpret_cast<void**>(&g_origLaunch), false },
    // [LOADBEARING: THROWABLE aim point] gamedll+0xDBCCC0 - the ThrowingStar's point; INV-568 SAFE 15
    { "throw aim point", kAimPointRva, kAimPointBytes, sizeof(kAimPointBytes), reinterpret_cast<void*>(&AimPointHook),
      reinterpret_cast<void**>(&g_origAimPoint), false },
    // [LOADBEARING: THROWABLE update] gamedll+0xDD1640 - the ONLY place a cancel is carried out (R6); INV-568 SAFE 17
    { "throw update", kUpdateRva, kUpdateBytes, sizeof(kUpdateBytes), reinterpret_cast<void*>(&UpdateHook),
      reinterpret_cast<void**>(&g_origUpdate), false },
};

// From ThrowableTick (the pad poll, 1 Hz, only while throwable_hand_enable=1), until all four are live or refused. Never
// per frame. Returns true when all four are live. A prologue mismatch (attempts == 0) refuses for the session; a commit
// that could not get the other threads clear is retried next second up to kMaxInstallTries.
bool ThrowableInstall()
{
    ++g_installTries;
    if (!g_setWorldPos || !g_localToWorld) {
        const HMODULE eng = reinterpret_cast<HMODULE>(g_eng);
        g_setWorldPos = reinterpret_cast<SetWorldPosFn>(
            GetProcAddress(eng, "?SetWorldPosition@IControlObject@@QEAAXAEBVvec3@@@Z"));
        g_localToWorld = reinterpret_cast<LocalToWorldFn>(
            GetProcAddress(eng, "?PointLocalToWorld@IControlObject@@QEBA?AVvec3@@AEBV2@@Z"));
        if (!g_setWorldPos || !g_localToWorld) {
            g_hooksRefused = true;
            g_hooksWhy = "an ENGINE export is missing (IControlObject::SetWorldPosition / PointLocalToWorld) - "
                         "REFUSED for the session";
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] DETOURS REFUSED - engine %p: SetWorldPosition %p, PointLocalToWorld %p (GetProcAddress "
                "by name). Nothing is patched; throwing by hand stays off for the session.",
                reinterpret_cast<void*>(g_eng), reinterpret_cast<void*>(g_setWorldPos),
                reinterpret_cast<void*>(g_localToWorld));
            return false;
        }
    }
    if (!g_restore) {
        // [LOADBEARING: THROWABLE restore] gamedll+0xDD4BF0 - the cancel's hand restore (INV-573); called, never patched
        const uintptr_t restoreRva = ::dlvr::buildrva::Gd(kRestoreRva);   // the EGL rva on the Epic build (buildrva table)
        if (memcmp(reinterpret_cast<const void*>(g_gd + restoreRva), kRestoreBytes, sizeof(kRestoreBytes)) != 0) {
            g_hooksRefused = true;
            g_hooksWhy = "the restore +DD4BF0's bytes did not match (another gamedll build) - REFUSED for the session";
            const unsigned char* const p = reinterpret_cast<const unsigned char*>(g_gd + restoreRva);
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] DETOURS REFUSED - the restore gamedll+0x%zX reads %02X %02X %02X %02X %02X %02X, "
                "expected %02X %02X %02X %02X %02X %02X (first 6 of %zu compared). Nothing is patched; throwing by hand "
                "stays off for the session.",
                static_cast<size_t>(kRestoreRva), p[0], p[1], p[2], p[3], p[4], p[5], kRestoreBytes[0], kRestoreBytes[1],
                kRestoreBytes[2], kRestoreBytes[3], kRestoreBytes[4], kRestoreBytes[5], sizeof(kRestoreBytes));
            return false;
        }
        g_restore = reinterpret_cast<RestoreFn>(g_gd + restoreRva);
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] restore gamedll+0x%zX VERIFIED (%zu bytes match) - every cancel runs it before "
            "clearing [+0x74] (INV-573)", static_cast<size_t>(kRestoreRva), sizeof(kRestoreBytes));
    }
    int live = 0;
    for (HookSite& s : g_sites) {
        if (s.live) { ++live; continue; }
        int attempts = 0;
        s.live = InstallSingleGameDetour(g_gd + s.rva, s.bytes, s.n, s.repl, s.orig, s.name, &attempts);
        if (s.live) {
            ++live;
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] detour %s gamedll+0x%zX LIVE (%zu bytes stolen, %d commit attempt(s), try %d)",
                s.name, static_cast<size_t>(s.rva), s.n, attempts, g_installTries);
        } else {
            if (attempts == 0) {
                g_hooksRefused = true;
                g_hooksWhy = "a detour's prologue did not match (named in its line) - REFUSED for the session";
            } else if (g_installTries >= kMaxInstallTries) {
                g_hooksRefused = true;
                g_hooksWhy = "a detour could not be committed in the try cap - REFUSED for the session";
            } else {
                g_hooksWhy = "a detour's commit failed - retried next second";
            }
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] detour %s gamedll+0x%zX NOT LIVE (try %d of %d, %d commit attempt(s)) - %s. The "
                "pad side readies nothing while any of the four is not live.",
                s.name, static_cast<size_t>(s.rva), g_installTries, kMaxInstallTries, attempts,
                attempts == 0 ? "InstallDetour refused (a prologue mismatch is logged just above by boneblend if that "
                                "was the cause) - REFUSED for the session"
                              : (g_hooksRefused ? "commit failed - try cap reached, REFUSED for the session"
                                                : "commit failed - retried next second"));
            if (g_hooksRefused) { return false; }
        }
    }
    if (live == 4) {
        InterlockedExchange(&g_hooksLive, 1);
        // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] DETOURS LIVE - set-state, launch, aim point, update | SetWorldPosition %p "
            "PointLocalToWorld %p | zero vec gamedll+0x%zX | aim callers +%zX / +%zX (return addresses)",
            reinterpret_cast<void*>(g_setWorldPos), reinterpret_cast<void*>(g_localToWorld),
            static_cast<size_t>(kZeroVecRva), static_cast<size_t>(kAimRetRelease), static_cast<size_t>(kAimRetUpdate));
        return true;
    }
    return false;
}

// Issue 4's flight detour - installed apart from the four: its failure never stops throwing by hand (the pad side does not
// wait for it); stars then keep the game's own flight, named here. From ThrowableTick (1 Hz) only while
// throwable_star_hand_velocity=1 and the four are live; never per frame. Once live it stays (the switch gates each call).
void ThrowableFlightInstall()
{
    ++g_flightTries;
    int attempts = 0;
    // [LOADBEARING: THROWABLE flight] gamedll+0x6E6D70 - the ThrowingStar's velocity = the hand's (issue 4); prologue 17
    const bool ok = InstallSingleGameDetour(g_gd + kFlightRva, kFlightBytes, sizeof(kFlightBytes),
                                            reinterpret_cast<void*>(&FlightHook), reinterpret_cast<void**>(&g_origFlight),
                                            "throw flight", &attempts);
    if (ok) {
        InterlockedExchange(&g_flightLive, 1);
        // [PROBE: THROWABLE] gate=throwable_star_hand_velocity  site=HEADSET  log-only=no
        LOG("throwable: [THROWABLE] detour throw flight gamedll+0x%zX LIVE (logged unconditionally; %zu bytes stolen, %d "
            "commit attempt(s), try %d) - stars / knives / axes now fly with the hand's velocity (throwable_star_hand_velocity=1)",
            static_cast<size_t>(kFlightRva), sizeof(kFlightBytes), attempts, g_flightTries);
        return;
    }
    if (attempts == 0 || g_flightTries >= kMaxInstallTries) { g_flightRefused = true; }
    // [PROBE: THROWABLE] gate=throwable_star_hand_velocity  site=HEADSET  log-only=no
    LOG("throwable: [THROWABLE] detour throw flight gamedll+0x%zX NOT LIVE (try %d of %d, %d commit attempt(s)) - %s. "
        "Throwing by hand is unaffected; stars keep the game's own flight.",
        static_cast<size_t>(kFlightRva), g_flightTries, kMaxInstallTries, attempts,
        attempts == 0 ? "InstallDetour refused (a prologue mismatch is logged just above by boneblend if that was the cause) "
                        "- REFUSED for the session"
                      : (g_flightRefused ? "commit failed - try cap reached, REFUSED for the session"
                                         : "commit failed - retried next second"));
}

// "<name> <calls> (ours <n>) [tid calls/ours ...]" into buf.
void FormatStats(const HookStats& s, char* buf, size_t n)
{
    int w = _snprintf_s(buf, n, _TRUNCATE, "%s %ld (ours %ld) [", s.name, static_cast<long>(s.calls),
                        static_cast<long>(s.ours));
    for (int i = 0; i < kThreadSlots && w >= 0 && static_cast<size_t>(w) < n; ++i) {
        if (s.t[i].tid == 0) { break; }
        const int k = _snprintf_s(buf + w, n - static_cast<size_t>(w), _TRUNCATE, "%s%ld:%ld/%ld", i ? " " : "",
                                  static_cast<long>(s.t[i].tid), static_cast<long>(s.t[i].calls),
                                  static_cast<long>(s.t[i].ours));
        if (k < 0) { return; }
        w += k;
    }
    if (w >= 0 && static_cast<size_t>(w) < n) {
        _snprintf_s(buf + w, n - static_cast<size_t>(w), _TRUNCATE, "] overflow %ld", static_cast<long>(s.overflow));
    }
}

}  // namespace

void ThrowableTick()
{
    // Called on every scePad poll; does its work once a second. COST between: one GetTickCount64 and a compare.
    static ULONGLONG s_lastMs = 0;
    const ULONGLONG nowMs = GetTickCount64();
    if (s_lastMs != 0 && nowMs - s_lastMs < 1000) { return; }
    s_lastMs = nowMs;
    LONG want = 1;
    const char* why = "";
    if (!ThrowableHandEnable) {
        want = 0;
        why = "throwable_hand_enable=0";
    } else if (!g_resolved) {
        const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");   // until resolved only - never per poll
        const HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
        if (!gd || !eng) {
            want = 0;
            why = !gd ? "gamedll_x64_rwdi.dll not mapped yet (retried each second)"
                      : "engine_x64_rwdi.dll not mapped yet (retried each second)";
        } else {
            g_gd = reinterpret_cast<uintptr_t>(gd);
            g_eng = reinterpret_cast<uintptr_t>(eng);
            // Task 4: the pad side may only ready a throw once all four detours are live - without the Update detour a
            // cancel could never be carried out (L2 would stay held). Installed here, once, never per frame.
            if (!g_hooksLive && !g_hooksRefused) { ThrowableInstall(); }
            if (g_hooksLive) {
                MemoryBarrier();
                InterlockedExchange(&g_resolved, 1);
            } else {
                want = 0;
                why = g_hooksWhy;
            }
        }
    }
    if (g_hooksLive) {
        // Issue 4: the flight detour, once the switch is first on (the hand throw never waits for it).
        if (ThrowableHandEnable && ThrowableStarHandVelocity && !g_flightLive && !g_flightRefused) {
            ThrowableFlightInstall();
        }
        // The detours' 5 s status line (step 6): per-thread counters, LOG_PERIODIC only (diag_periodic_logs=1).
        static int s_sec = 0;
        if (++s_sec >= 5) {
            s_sec = 0;
            char a[256], b[256], c[256], d[256], f[256];
            FormatStats(g_stSetState, a, sizeof(a));
            FormatStats(g_stLaunch, b, sizeof(b));
            FormatStats(g_stAim, c, sizeof(c));
            FormatStats(g_stUpdate, d, sizeof(d));
            FormatStats(g_stFlight, f, sizeof(f));
            // [PROBE: THROWABLE] gate=diag_periodic_logs  site=HEADSET  log-only=yes
            LOG_PERIODIC("throwable: [THROWABLE] detours 5 s (cumulative; thread id:calls/ours) | %s | %s | %s | %s | "
                         "%s (live %ld, star flights ours %ld, refused %ld) | "
                         "release applied %ld, UNEXPECTED %ld | launch ours %ld, REFUSED lines %ld, not the route 1 player %ld | "
                         "aim ours %ld kept %ld, melee callers %ld | cancels %ld, faulted %ld, skipped %ld, at 0 %ld | "
                         "pad thread %ld",
                         a, b, c, d, f, static_cast<long>(g_flightLive), static_cast<long>(g_nFlightOurs),
                         static_cast<long>(g_nFlightRefused), static_cast<long>(g_nRelApplied), static_cast<long>(g_nUnexpected),
                         static_cast<long>(g_nLaunchOurs), static_cast<long>(g_nLaunchLocalNotTaken),
                         static_cast<long>(g_nLaunchOther), static_cast<long>(g_nAimOurs), static_cast<long>(g_nAimKept),
                         static_cast<long>(g_nAimNotThrow), static_cast<long>(g_nCancelDone),
                         static_cast<long>(g_nCancelFault), static_cast<long>(g_nCancelSkipped),
                         static_cast<long>(g_nCancelAtZero), static_cast<long>(g_padTid));
        }
    }
    if (want != g_lastArm) {
        g_lastArm = want;
        if (want) {
            float ahc[3];
            HolsterHipCentre(0, ahc);   // the RIGHT hip zone in use (the R2 ready, spec 2026-11-01; log text kept for the logid table)
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] ARMED (logged unconditionally) | gamedll %p engine %p | player route 1 "
                "[gamedll+0x%zX]+%zu, throw ctrl +0x%zX, state +0x%zX, released +0x%zX | getter +0x%zX, ItemType vfunc "
                "+0x%zX on item+%zu, Hook %u excluded | hip zone (%.3f, %.3f, %.3f) r %.3f | full speed %.2f still "
                "speed %.3f | throwable_star_hand_velocity=%d (flight detour %s) | the cancel is carried out by the "
                "game-thread Update detour (Task 4), never from this poll",
                reinterpret_cast<void*>(g_gd), reinterpret_cast<void*>(g_eng), static_cast<size_t>(kPlayerGlobalRva),
                kPlayerOff, kThrowCtrlOff, kCtrlStateOff, kCtrlReleasedOff, static_cast<size_t>(kSelThrowableRva),
                kTypeVfuncOff, kItemInfoOff, kTypeHook, ahc[0], ahc[1], ahc[2],
                HolsterRHipRadius, ThrowableFullSpeed, ThrowableStillSpeed, ThrowableStarHandVelocity ? 1 : 0,
                g_flightLive ? "LIVE" : (g_flightRefused ? "REFUSED" : (ThrowableStarHandVelocity ? "installing next second"
                                                                                                  : "not installed - switch off")));
        } else {
            // [PROBE: THROWABLE] gate=throwable_hand_enable  site=HEADSET  log-only=no
            LOG("throwable: [THROWABLE] NOT ARMED - %s. L2 / R2 untouched, nothing read in the game.", why);
        }
    }
}

bool ThrowableReleaseSnapshot(throwable::Vec3& posWorld, throwable::Vec3& velWorld, double& ageMs)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_snapSeq;
        if (s0 & 1) { continue; }
        MemoryBarrier();
        const float px = g_snapPos[0], py = g_snapPos[1], pz = g_snapPos[2];
        const float vx = g_snapVel[0], vy = g_snapVel[1], vz = g_snapVel[2];
        const double t = g_snapMs;
        const bool valid = g_snapValid;
        MemoryBarrier();
        if (g_snapSeq != s0) { continue; }
        if (s0 == 0 || !valid) { return false; }
        posWorld = throwable::Vec3{ px, py, pz };
        velWorld = throwable::Vec3{ vx, vy, vz };
        ageMs = NowMs() - t;
        return true;
    }
    return false;
}

void* ThrowableLocalCtrl()
{
    return g_localCtrl;
}

bool ThrowableTakeCancelRequest()
{
    return g_cancelPending != 0 && InterlockedExchange(&g_cancelPending, 0) != 0;
}

bool ThrowableReleasePending()
{
    return g_releasePend != 0;
}

bool ThrowableHoldsL2()
{
    return g_holdsL2 != 0;
}

bool ThrowableHookSelected()
{
    return g_hookSel != 0;
}

bool ThrowableBusy()
{
    return g_busy != 0;
}

}  // namespace dlvr
