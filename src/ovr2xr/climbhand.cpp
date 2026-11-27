// Hand climbing P1 - see climbhand.h. docs/design_climbing_overhaul.md §6 P1, §8.5 (2026-11-25, round 2 2026-11-26).
//
// WHAT IT USES (static analysis of the installed gamedll = the Steam layout, design doc §8 / §8.4 / §8.5; none of these gamedll rvas
// is on the Epic allowlist, so on Epic every byte check fails and the feature refuses by name):
//   +BD4450  the player's "try to start a ledge grab", once per player update (+B96A8A). Reads the climb target PlayerDI+0x1600,
//            AllowEdgeHanging, the jump input and a 30-degree look test, then: midpoint -> PlayerDI+0x14C8, direction -> +0x14BC, EdgeGrab
//            vtable slot 35 (+D07300: mov [rcx+38h], rdx) takes the target, +B9F220(player, 21, 1, 0) switches the controller group to
//            21 = the ledge grab (+BD4C29..+BD4C7C). DETOURED: with climb_by_hand on the original runs only while a ledge grab is
//            running (chained grabs after a hang jump) or for a LOW wall (climb_auto_low_m); otherwise the game starts nothing itself.
//   +B9D460  the game's "set climb target"(player, a, b, dir, ushort tag): one 0x30-byte record, allocated once and kept at
//            PlayerDI+0x1600 (a +0x0, b +0xC, dir +0x18, tag +0x28, 1.2f at +0x2C). The game's own detector (+BDB020) refills it every
//            update from the LOOK and ZEROES it (+BDBEE9) when nothing is found - P1 run 1 measured the wrong ledge because of this.
//            CALLED with the ledge FindLedge found at the hand, tag 8 (the ledge grab's own re-target call, +D0A729), just before the
//            start, inside the same detour - so the detector's next refill cannot get between them.
//   +D0AF40  the ledge grab's climb-type chooser (0 = hang; 1-8 = the direct climbs), called from its start at +D0E74C.
//            DETOURED, filtered on the return address +D0E751: for a hand grab whose lip is >= climb_hang_min_m above the feet, with
//            climb_always_hang on, a non-zero type becomes 0 (hang). Every other call (+BD4D85, +BD57A4) passes through untouched.
//   +B9F220  the controller-group setter (INV-329), +D0CD20 the ledge grab's set-state (16 = the drop, P0 §8.4) - byte-checked, CALLED.
//   engine IGSObject::Raytrace (player+40, mask 0x37 = the ledge grab's own, ignore = player+24, 8, 0, 0 - its call at +D0E046) and
//   IControlObject::GetWorldPosition (the feet). PlayerDI+0xC60 = EdgeGrabController (RTTI-checked), active +16, state +0x1FC;
//   PlayerDI+3324 = the controller group. The local player = *(*(*(GameDI + 1344) + 184) + 88).
// THREADS: the pad thread decides (climbhand_logic Update) and publishes which hand searches and where it is; the game thread casts,
// starts and drops (Raytrace and the start must run in the player update).
#include "climbhand.h"

#include "boneblend.h"        // InstallSingleGameDetour
#include "climbhand_logic.h"
#include "controllers.h"      // GetControllerSqueeze
#include "looting.h"          // LootHandWorldPos
#include "roomscale.h"        // RoomscaleOnFoot
#include "rtticam.h"          // RttiClassNameOf

#include <windows.h>
#include <intrin.h>           // _ReturnAddress

#include <cmath>
#include <cstdint>
#include <cstdio>             // std::snprintf (the re-target line's reason)
#include <cstring>

#include "../common/log.h"

#pragma intrinsic(_ReturnAddress)

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_CH_GD(rva) ::dlvr::buildrva::Gd(rva)
#define DLVR_CH_EN(rva) ::dlvr::buildrva::Eng(rva)
#else
#define DLVR_CH_GD(rva) (rva)
#define DLVR_CH_EN(rva) (rva)
#endif

namespace dlvr {

// [DEP: climb_by_hand] required-for=hand climbing P1 (ClimbByHand): the game's own ledge-grab start is skipped (low walls excepted) and
//      a grip on a ledge found around the hand starts it instead; both grips open while hanging = the game's drop. tree=PROTOTYPE. Off =
//      both detours, if installed, pass everything through. DEFAULT OFF until a headset run says otherwise.
bool  ClimbByHand = false;
// [DEP: climb_reach_m] required-for=how far the found lip may be from the hand (ClimbReachM). tree=INERT while climb_by_hand=0. 0.30.
float ClimbReachM = 0.30f;
// [DEP: climb_catch_ms] required-for=how long a closed grip keeps searching (ClimbCatchMs) - the catch while falling. INERT while off.
int   ClimbCatchMs = 1500;
// [DEP: climb_always_hang] required-for=a hand grab on a high ledge hanging instead of climbing over (ClimbAlwaysHang; user 2026-11-26:
//      "grabbing the edge when there's a floor above makes the character go over immediately... this could be a setting"). DEFAULT ON.
bool  ClimbAlwaysHang = true;
// [DEP: climb_hang_min_m] required-for=which ledges climb_always_hang applies to (ClimbHangMinM): lip >= this above the feet. Lower ones
//      the game climbs over (hanging there would put the body in the floor). DEFAULT 1.7 (2026-11-26 run 6, tester: "very often
//      climbs over instead of hanging" - 7 of 16 climb-overs had their lip at 1.59-1.86 m; was 1.9).
float ClimbHangMinM = 1.7f;
// [DEP: climb_auto_low_m] required-for=low walls staying the game's own look + jump climb (ClimbAutoLowM; user 2026-11-26: walls
//      "between knees and chest" should not need hands; after run 2: "1.5 is pretty high" -> 1 m). 0 = every ledge needs a hand.
//      DEFAULT 1.0.
float ClimbAutoLowM = 1.0f;
// REMOVED 2026-11-27 (tester: "remove the engine collision view and the raycast debug line draws"): both variables below are kept
// only for their LOCKED keys in the settings table (ovr2xr.cpp) - nothing reads them; the drawing and the engine call are gone.
// climb_debug_draw drew the search's rays in the headset (2026-11-26); climb_debug_physics asked the engine for its physics debug view
// (ILevel::SetPhysicsDebugRenderMode - the released engine drew nothing, design §8.8).
bool  ClimbDebugDraw = false;
int   ClimbDebugPhysics = 0;
// [DEP: climb_hand_motion] required-for=P2 (design §4.4): while hanging (the ledge grab's hold states), the body follows the hands -
//      walklook's +ED2040 detour sends our velocity instead of the game's zero (ClimbHandWalkOverride). Off = the hang stays still, as
//      in P1. INERT while climb_by_hand=0. DEFAULT ON.
bool  ClimbHandMotion = true;
// [DEP: climb_pull_max_mps] required-for=the speed cap of the hand-driven body (ClimbPullMaxMps). DEFAULT 4.0 m/s.
float ClimbPullMaxMps = 4.0f;
// [DEP: climb_release_m] required-for=a holding hand letting go when it is this far from its ledge point (ClimbReleaseM; tester
//      2026-11-26: "if the hand moves away from its anchor while grabbing, that hand should stop grabbing"). Never used below
//      climb_reach_m + 0.1 (the hold's start snaps the hand up to the reach onto the ledge). DEFAULT 0.45 m.
float ClimbReleaseM = 0.45f;
// [DEP: climb_stick_shimmy] required-for=the game's own shimmy from a hang (states 11-15, the stick sideways / up) in hand mode
//      (ClimbStickShimmy; tester 2026-11-27: "a setting to remove the original game functionality and only leave joystick forward to
//      mantle"). Off = refused (GameMoveFromHang); move along a ledge hand over hand. DEFAULT ON (tester 2026-11-27; was off).
bool  ClimbStickShimmy = true;
// [DEP: climb_body_follows_view] required-for=the hanging body turning with the view (ClimbBodyFollowsView; tester 2026-11-27: "if I
//      shimmy with my hands around a corner my body stays turned in the direction of the original hang ... I'd want it to follow the
//      player's view, but not move the camera"). On = body-follows-head is NOT paused while hanging. Off = round 5's pause (§8.13).
//      Needs comfort_body_follows_head for the body to turn at all. The view's steadiness is climb_view_smooth's (P4 round 5);
//      round 2's HeadOrbitComp (moving the body against the head's swing) and round 4's face-the-ledge turn are gone.
//      Round 3 set it off (run 13: "the camera snapping when turning is back") - DEFAULT ON again since round 5 (tester 2026-11-27:
//      "have the body direction follow the view as before, but ... have [the camera] follow the eye bones smoothly").
bool  ClimbBodyFollowsView = true;
// [DEP: climb_view_smooth] required-for=a steady view while climbing (ClimbViewSmooth; tester 2026-11-27, see ViewSmoothStep). On =
//      while the ledge grab runs in hand mode, the camera follows the eye bone's offset from the body smoothly (boneblend
//      EyePointApply -> ClimbHandViewCorrection), so the hang's look pose, a body turn or the game's climb animations glide instead
//      of snapping; the body's own movement and the headset's are never smoothed. Off = the camera is on the eye bone. DEFAULT ON.
bool  ClimbViewSmooth = true;
// [DEP: climb_ledge_last] required-for=which ledge the climb-up height, the stale-ledge test and the climb on the held ledge use when
//      both hands hold different ledges (ClimbLedgeLast; tester 2026-11-27: "the last one the player grabbed", then "make it a
//      setting, whether to use the last one or an average"). On = the last grabbed; off = the average. DEFAULT OFF (tester
//      2026-11-27; was on).
bool  ClimbLedgeLast = false;
// [DEP: climb_pin_hands] required-for=P4 (design §4.8; ClimbPinHands): a hand holding a ledge is DRAWN on the ledge point it holds -
//      the arm IK's wrist target moves by (that point - the hand) (boneblend.cpp SolveArmIk, ClimbHandPinShift). Off = the drawn hands
//      follow the controllers while hanging, as in P2. INERT while climb_hand_motion=0 (no anchors). DEFAULT ON.
bool  ClimbPinHands = true;
// [DEP: climb_mantle_auto] required-for=P3 round 2's automatic climb-up (ClimbMantleAuto; tester 2026-11-27: "the automatic mantle
//      from hang height should be at a customizable height and disabled by default"): pulled up until the highest held ledge point is
//      at most climb_mantle_m above the feet = the GAME'S OWN climb-up (set-state 8, its animation). INERT while climb_hand_motion=0.
//      DEFAULT OFF.
bool  ClimbMantleAuto = false;
// [DEP: climb_mantle_m] required-for=climb_mantle_auto's height (ClimbMantleM). Round 1 lifted the body over the lip itself - replaced
//      by the game's animation (tester: "not great ... stick to the normal game animation"). DEFAULT 1.2 m (a hang starts at
//      climb_hang_min_m 1.7 or more, so about a half-metre pull).
float ClimbMantleM = 1.2f;
// [DEP: climb_mantle_stick] required-for=the stick pushed forward in a hang climbing up the game's way (ClimbMantleStick; tester
//      2026-11-27: "only triggers from pushing the left stick forward (as it currently works)"). Off = the game's own 1 -> 8 is refused
//      in hand mode (climbhand_logic.h RefuseClimbUp). DEFAULT ON.
bool  ClimbMantleStick = true;
// [DEP: climb_auto_low] required-for=low walls (climb_auto_low_m) still climbing the game's own way, look + jump (ClimbAutoLow; tester
//      2026-11-27: "disable the low wall auto mantle completely, add a boolean setting ... disabled by default"). Off = the game starts
//      no ledge grab of its own at all. DEFAULT OFF.
bool  ClimbAutoLow = false;
// [DEP: climb_launch_up_assist] required-for=a mostly-upward launch turned toward straight up (ClimbLaunchUpAssist; tester 2026-11-27:
//      "30 degrees off centre ... make it 15", 0 = no help, 1 = straight up). climbhand_logic.h UpAssist. DEFAULT 0.5.
float ClimbLaunchUpAssist = 0.5f;
// [DEP: climb_launch_min_mps] required-for=P3's launch (design §4.6; ClimbLaunchMinMps): the last hand letting go while the hands moved
//      the body at least this fast keeps that speed in a jump (a fling up to a higher ledge, or a push-off). 0 = never (a plain drop).
//      INERT while climb_hand_motion=0. DEFAULT 1.5 m/s.
float ClimbLaunchMinMps = 1.5f;
// [DEP: climb_launch_scale] required-for=the launch's speed (ClimbLaunchScale): the pull's speed x this, never above the game's own
//      jump (tester 2026-11-27, balance): the default JumpMaxHeight 1.01 m up, WallJumpBackSpeed 3 m/s sideways (climbhand_logic.h
//      kLaunchMaxUpMps / kLaunchMaxSideMps). DEFAULT 1.25.
float ClimbLaunchScale = 1.25f;
// [DEP: climb_air_momentum] required-for=a launch keeping its momentum in the air (ClimbAirMomentum; tester 2026-11-27: "a setting to
//      completely remove the game overriding the speed mid air. Let physics do its thing ... On the ground I do agree the stick should
//      take over"). On = from a launch's drop until it lands, the walk is handed the body's own measured horizontal velocity (what
//      physics let it do: collisions kept) minus climb_air_drag, plus climb_air_control's push. Off = the launch's speed plus the
//      stick's (P4 round 5), still stopped by walls. climbhand_logic.h CarryStep. DEFAULT ON.
bool  ClimbAirMomentum = true;
// [DEP: climb_air_drag] required-for=how fast the air momentum fades (ClimbAirDrag, per second, linear: the speed x exp(-drag t)).
//      0 = none. INERT while climb_air_momentum=0. DEFAULT 0.1 (10 % in the first second).
float ClimbAirDrag = 0.1f;
// [DEP: climb_air_control] required-for=the stick's say in the air while climb_air_momentum keeps the momentum (ClimbAirControl;
//      tester 2026-11-27: "0 is the stick has no influence, and 1 is the game's default influence"). climbhand_logic.h
//      AirControlPush: the walk is handed momentum + ClimbAirControl x the game's own per-update air step toward the stick (its
//      air factor cubed, read from the walk, x the gap to the stick's speed along the stick), so 1 = the game's own stick force
//      ADDED to the momentum (tester: "not replace it"), 0 = momentum only. (Round 7's first cut added the whole gap, assuming the
//      engine scaled it down - run 15 showed the walk hands it to the body as is: the stick took over.) INERT while
//      climb_air_momentum=0. DEFAULT 1 (the game's own steering).
float ClimbAirControl = 1.0f;
// [DEP: climb_air_momentum_all] required-for=momentum in ALL air time (ClimbAirMomentumAll; tester 2026-11-27: "It would be ideal to
//      have this mode work generically on all air time, not necessarily just when jumping from a hang"). On (with climb_air_momentum)
//      = the game's own jumps and falls start the same carry while the walk's own airborne flag is set (P4 round 8; round 6 read
//      free fall off the feet - climbhand_logic.h FeetVel's note), under MoveController only. Off = launches from a hang only.
//      DEFAULT ON.
bool  ClimbAirMomentumAll = true;
// [DEP: climb_view_rigid] required-for=a view the climb animations never move (ClimbViewRigid; tester 2026-11-27: "can't we remove
//      the pose changes that affect the view altogether?"). On = while the ledge grab runs in hand mode, boneblend's EyePointApply
//      puts the camera base on the standing eye point (remembered body-local the last time the player stood still) instead of the
//      animated head, eased in / out over climbhand_logic.h kRigidRampS; climb_view_smooth and the face offset are not used then.
//      DEFAULT ON.
bool  ClimbViewRigid = true;
// [DEP: climb_hold_body] required-for=an upright upper body while climbing (ClimbHoldBody; tester 2026-11-27: "the body tends to lean
//      forward a ton, sometimes actually bending at the torso"). On = while the ledge grab runs in hand mode, boneblend's throwable
//      body hold (AhThrStep: spine, neck, head stop taking the animation's rotation) arms with the grab and lets go when it ends.
//      Needs rig_throwable_body_hold on (its machinery and ANIMHOLD's proven map). DEFAULT ON.
bool  ClimbHoldBody = true;

namespace {

constexpr uintptr_t kTryStartRva  = 0xBD4450;
constexpr uintptr_t kSetGroupRva  = 0xB9F220;
constexpr uintptr_t kSetStateRva  = 0xD0CD20;
constexpr uintptr_t kSetTargetRva = 0xB9D460;
constexpr uintptr_t kTypeRva      = 0xD0AF40;
constexpr uintptr_t kTypeRetRva   = 0xD0E751;   // the ledge grab's own call at +D0E74C
constexpr uintptr_t kLookCallRva  = 0xD0AD16;   // +D0A240's set-state call for 2 / 3 (the hang's look states) and 0xD / 0xE
constexpr uintptr_t kStateRetRva  = 0xD0A4BE;   // +D0A240's set-state call at +D0A4B9: the end of the grab (0 -> 1 hang / 4 / 8 climb)
constexpr uintptr_t kGameDiRva    = 0x1C16348;  // GameDI (EGL row exists; the rest have none -> Epic refuses at the bytes)
// +BD4450: push rbp | push rbx | lea rbp,[rsp-98h] | sub rsp,198h - 18 bytes, whole instructions, no RIP-relative (read 2026-11-25)
const unsigned char kTryStartBytes[18] = { 0x40, 0x55, 0x53, 0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFF, 0xFF, 0xFF,
                                           0x48, 0x81, 0xEC, 0x98, 0x01, 0x00, 0x00 };
// +D0AF40: mov rax,rsp | push rbp | push r13 | push r14 | lea rbp,[rax-38h] | sub rsp,120h - 19 bytes, no RIP-relative (2026-11-26)
const unsigned char kTypeBytes[19] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x68, 0xC8,
                                       0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00 };
// +B9F220: push rbx | push r13 | push r14 | sub rsp,30h | cmp byte ptr [rcx+50h],0 - checked before the first call
const unsigned char kSetGroupBytes[14] = { 0x40, 0x53, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x80, 0x79, 0x50, 0x00 };
// +D0CD20: push rbp | push rsi | push r14 | lea rbp,[rsp-47h] | sub rsp,0D0h - checked before the first call
const unsigned char kSetStateBytes[17] = { 0x40, 0x55, 0x56, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9,
                                           0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00 };
// +B9D460: mov [rsp+8],rbx | mov [rsp+10h],rsi | mov [rsp+18h],rdi | push r14 | sub rsp,20h | mov r10,[rcx+1600h] (2026-11-26)
const unsigned char kSetTargetBytes[25] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C,
                                            0x24, 0x18, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x91, 0x00 };
// EdgeGrab vtable slot 35 (+D07300): mov [rcx+38h], rdx | ret - checked at every start, through the live vtable
const unsigned char kSetHookBytes[5] = { 0x48, 0x89, 0x51, 0x38, 0xC3 };
constexpr const char* kRayExport = "?Raytrace@IGSObject@@QEAAEEPEAUSCollision@@AEBVvec3@@AEAV3@GPEAVIControlObject@@IIH@Z";
constexpr const char* kPosExport = "?GetWorldPosition@IControlObject@@QEBA?AVvec3@@XZ";
// P3's launch: the ledge grab's own exit (+D11D90, its call at +D1204E) hands the walk body a velocity as
// IControlObject::PhysicsBind(player+0x18) -> bind vfunc +0xB0 (the walk interface) -> its vfunc +0x2C0 (the IPhBody) -> SetLinearVel.
// The walk interface is checked as roomscale checks it: interface - 0x700 must carry CODEPhysicsWalkFly's vtable (engine +86DC38).
constexpr const char* kBindExport = "?PhysicsBind@IControlObject@@QEAAPEAVIPhysicsBind@@XZ";
constexpr const char* kSetVelExport = "?SetLinearVel@IPhBody@@QEAAXAEBVvec3@@@Z";
constexpr uintptr_t kWalkFullVtRva = 0x86DC38;
constexpr uintptr_t kBindWalkVfn = 0xB0, kWalkBodyVfn = 0x2C0, kWalkIfaceToFull = 0x700;
constexpr uintptr_t kEdgeGrabSlot = 0xC60, kHookPtr = 0x1600, kGroupOff = 3324, kHookMid = 0x14C8, kHookDir = 0x14BC;
constexpr uintptr_t kActiveOff = 16, kEgStateOff = 0x1FC, kSetHookVfn = 0x118;
constexpr int       kGroupEdgeGrab = 21, kStateDrop = 16;
constexpr unsigned short kTargetTag = 8;    // the ledge grab's own tag when it re-targets (+D0A729)
constexpr unsigned char  kRayMask = 0x37;   // the ledge grab's own ledge-search mask (+D0E035)
constexpr long      kMaxLines = 1500;       // INV-9: event lines this session (per-hand routine lines: HandLine)

using TryStartFn  = void(__fastcall*)(uintptr_t player);
using TypeFn      = int(__fastcall*)(void* eg, void* a2, float* dir, float* height, void* a5);
using SetGroupFn  = void(__fastcall*)(uintptr_t player, int group, bool flag, void* zero);
using SetStateFn  = void(__fastcall*)(uintptr_t edgeGrab, int state);
using SetHookFn   = void(__fastcall*)(uintptr_t edgeGrab, uintptr_t hook);
using SetTargetFn = void(__fastcall*)(uintptr_t player, const float* a, const float* b, const float* dir, unsigned short tag);
using RayFn       = unsigned char(__fastcall*)(void* self, unsigned char kind, void* coll, const float* from, float* to,
                                               unsigned short flags, void* ignore, unsigned a8, unsigned a9, int a10);
using PosFn       = float*(__fastcall*)(const void* control, float* out);

TryStartFn    g_true = nullptr;
TypeFn        g_trueType = nullptr;
uintptr_t     g_stateRet = 0;
uintptr_t     g_lookRet = 0;        // +D0AD1B: the return of +D0A240's set-state call at +D0AD16 (the hang's 2 / 3 look states)
volatile LONG64 g_wLookRefused = 0;
volatile LONG g_stateLive = 0;      // the hang transition detour: 1 live, -1 refused
uintptr_t     g_hangEg = 0;         // the ledge grab whose grab (state 0) should end in a hang (game thread)
ULONGLONG     g_hangUntil = 0;
SetGroupFn    g_setGroup = nullptr;
SetStateFn    g_setState = nullptr;
SetTargetFn   g_setTarget = nullptr;
RayFn         g_ray = nullptr;
PosFn         g_pos = nullptr;
uintptr_t     g_gd = 0, g_typeRet = 0;
volatile LONG g_installed = 0;   // 1 once an install was attempted
volatile LONG g_live = 0;        // 1 live, -1 refused
volatile LONG g_typeLive = 0;    // the hang detour: 1 live, -1 refused (the rest works without it - climb_always_hang does nothing)
volatile LONG g_lines = 0;

// game thread -> pad thread
SRWLOCK               g_lock = SRWLOCK_INIT;
climbhand::GameView   g_pub;
ULONGLONG             g_pubMs = 0;
// pad thread -> game thread: which hands search now and where each is (game world) - round 7: both hands at once
bool          g_searchOn[2] = {};
float         g_searchPos[2][3] = {};
ULONGLONG     g_searchMs = 0;    // when the pad thread last published (a stale request is not served)
volatile LONG g_reqDrop = 0;   // 1 grips opened, 2 a slip (plain drop), 3 a slip during a jump flick (launches) - round 9 / 10
volatile float g_dropHandMps = 0.0f;   // the fastest controller (play space) in kLaunchWindowMs before that drop request (log)
// game thread -> pad thread: per hand, when a ledge was last found within reach of it (a start, or a join while hanging), and the
// ledge point found (the lip nearest the hand - P2 round 2 anchors the hand there)
ULONGLONG     g_foundMs[2] = {};
float         g_foundLip[2][3] = {};
float         g_foundInto[2][3] = {};   // P3: that ledge's inward (into the wall) direction, for the mantle
// pad thread: the ledge point of each holding hand (latched when its search ends FOUND)
float         g_holdLip[2][3] = {};
float         g_holdInto[2][3] = {};
bool          g_haveHoldLip[2] = {};
int           g_lastGrabHand = -1;      // the hand whose search last ended FOUND (the ledge held = its ledge, LedgeHand)
bool          g_relWithhold[2] = {};    // a hand released for distance keeps its grip withheld until the grip opens
volatile LONG g_saidFreeze = 0;
// pad thread -> game thread (P2 / P3): the anchored hands - where the body must go for them to be back on their anchors (PullStep),
// and their ledge points (the mantle's height and direction)
climbhand::Hold g_hold;
ULONGLONG     g_holdPubMs = 0;
// pad thread -> the arm IK (P4 climb_pin_hands): per hand, (its ledge point - the hand), game world; valid where g_pinOn (under g_lock)
bool          g_pinOn[2] = {};
float         g_pinShift[2][3] = {};
float         g_pinMaxM = 0.45f;        // the release distance in use: a larger shift is never drawn
// P4 look probe (game thread): while a hand-held hang runs, the look as the game holds it - logged once per hang when it ends
struct HangLook {
    bool  on = false;
    long  n = 0, notLook = 0;
    float win0[4] = {};                  // the first look window seen (maxYaw, maxPitch, minYaw, minPitch)
    float winLo[4] = {}, winHi[4] = {};  // each window value's range over the hang
    bool  haveWin = false;
    float cam0 = 0.0f, look0 = 0.0f, prevCam = 0.0f, prevLook = 0.0f, camRel = 0.0f, lookRel = 0.0f;
    float camLo = 0.0f, camHi = 0.0f, lookLo = 0.0f, lookHi = 0.0f, pitchLo = 0.0f, pitchHi = 0.0f;
};
HangLook      g_hl;
climbhand::Pull g_pull;   // pad thread
// P3 / P4 round 5 (game thread): the hands' motion (the velocities sent, for the launch pick) and climb_mantle_auto's request for
// the game's own climb-up; the launch's CARRY after its drop (ClimbHandWalkOverride hands the walk the launch's horizontal speed)
climbhand::Motion g_motion;
climbhand::Carry  g_carry;
bool          g_carryAir = false;       // this carry began in the game's own air time (climb_air_momentum_all), not a launch
climbhand::FeetVel g_fv;                // P4 round 8: the feet's velocity between walk calls (an air carry starts with it)
bool          g_vertSet = false;        // a launch set the walk's vertical speed (WalkVertical) - cleared when its carry ends
int           g_traceLaunches = 0;      // launches traced call by call (the first kTraceLaunches)
int           g_traceAirs = 0;          // air carries traced call by call (the first kTraceAirs)
bool          g_traceOn = false;        // this carry is traced
volatile LONG g_traceLines = 0;
constexpr int  kTraceLaunches = 6, kTraceAirs = 4;
constexpr long kMaxTraceLines = 700;
constexpr unsigned long long kTraceLaunchMs = 700, kTraceAirMs = 1200;
LARGE_INTEGER g_airBaseQpc = {};
volatile LONG g_airLines = 0;
volatile LONG64 g_wAirStarts = 0;
float         g_carryFeet0[3] = {};     // the feet at the carry's first walk call, for the measured line
ULONGLONG     g_carryFeet0Ms = 0;
bool          g_carrySaid = false;      // the measured line, once per launch
bool          g_climbUpAsked = false;   // climb_mantle_auto asked once this hold
bool          g_wantClimbUp = false;    // ...for the Hook to call set-state 8
float         g_climbUpGap = 0.0f;      // the ledge's height above the feet when it asked (log)
bool          g_ourClimbUp = false;     // our own set-state 8 is in flight (RefuseClimbUp lets it through)
volatile ULONGLONG g_stickFwdMs = 0;    // pad thread -> StateHook: the last poll that read the left stick pushed forward
uintptr_t     g_curPlayer = 0;          // the local player of the last Hook (StateHook reads its ledge, +0x14C8)
// P4 round 3: the game re-targeted onto the ledge the hands hold (RetargetInPlace) - when, for GameMoveFromHang's retargetFresh.
// Game thread.
ULONGLONG     g_retargetMs = 0;
int           g_gmLastFrom = -1, g_gmLastTo = -1, g_gmLastWhy = -1;   // the last game-move refusal logged (one line per change)
ULONGLONG     g_gmLastMs = 0;
// P4 round 5 view smoothing (game thread: boneblend's camera-base call -> ClimbHandViewCorrection)
climbhand::ViewSmooth g_vs;
LARGE_INTEGER g_vsQpc = {};
volatile LONG g_saidVs = 0;
float         g_wVsMaxCm = 0.0f;        // the largest correction applied this window (cm)
volatile LONG64 g_wCamSnaps = 0;        // P4 round 8: camera snaps against the body seen this window (ClimbHandCamTrace)
float         g_wCamSnapMaxCm = 0.0f;   // ...the largest (cm; game thread)
volatile LONG64 g_wGameMoveRefused = 0, g_wRetargets = 0, g_wRetargetFails = 0, g_wRetargetNotHeld = 0, g_wVsCalls = 0;
volatile LONG64 g_wCarryOn = 0;         // walk calls the carry handed the launch's speed
bool          g_postOn = false;
float         g_postVel[3] = {};
int           g_postLeft = 0;           // SetLinearVel calls still to make once the ledge grab has ended
ULONGLONG     g_postUntil = 0;
LARGE_INTEGER g_motionQpc = {};
volatile LONG64 g_wClimbUps = 0, g_wStickRefused = 0, g_wLanded = 0, g_wLaunches = 0, g_wPlainDrops = 0;
using BindFn  = void*(__fastcall*)(void* control);
using SetVelFn = void(__fastcall*)(void* body, const float* v);
BindFn        g_bindFn = nullptr;
SetVelFn      g_setVel = nullptr;
uintptr_t     g_en = 0;
// P2 5 s window (game thread): driven walk calls, the largest speed sent / measured, calls where the body moved < 30 % of what was
// sent while sending >= 0.5 m/s (something solid in the way), override calls that passed through (not a hold state / stale)
volatile LONG64 g_wDriven = 0, g_wBlocked = 0, g_wOverrideCalls = 0;
volatile LONG64 g_wOneHand = 0, g_wTwoHand = 0, g_wReleased = 0;   // pad polls driving from one / both anchored hands; releases
float     g_wMaxSent = 0.0f, g_wMaxMeas = 0.0f;
float     g_lastSent[3] = {}, g_lastFeet[3] = {};
LARGE_INTEGER g_lastDriveQpc = {}, g_qpf = {};
// game-thread state
uintptr_t g_egPtr = 0;
bool      g_egOk = false;
int       g_lastWhy[2] = { -1, -1 };   // the last search outcome logged for each hand's current search
ULONGLONG g_forceHangUntil = 0;  // the next type choice of the ledge grab is forced to hang until then (game thread)
// pad-thread state
climbhand::Machine g_m;
// 5 s window
volatile LONG64 g_wUpd = 0, g_wSkipped = 0, g_wPassActive = 0, g_wPassLow = 0, g_wStarts = 0, g_wDrops = 0, g_wRefused = 0,
                g_wCasts = 0, g_wFaults = 0, g_wHung = 0, g_wSearchUpd = 0, g_wOwned = 0, g_wLate = 0, g_wCharHits = 0;
volatile LONG64 g_wWhy[climbhand::kWhyCount] = {};
ULONGLONG       g_winMs = 0;

bool Line() { return InterlockedIncrement(&g_lines) <= kMaxLines; }
// Run 13 (2026-11-27): the routine per-hand lines (search start / end, join, anchor, let go, grip hand-over) used up the whole event
// budget by 14:13 of a 10-minute run, and every refusal / climb line after that was lost. They now have their own budget.
volatile LONG g_handLines = 0;
constexpr long kMaxHandLines = 400;
bool HandLine() { return InterlockedIncrement(&g_handLines) <= kMaxHandLines; }


bool RdQ(uintptr_t a, uintptr_t* o) { __try { *o = *reinterpret_cast<const uintptr_t*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool RdI(uintptr_t a, int* o) { __try { *o = *reinterpret_cast<const int*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool RdB(uintptr_t a, unsigned char* o) { __try { *o = *reinterpret_cast<const unsigned char*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool RdF3(uintptr_t a, float o[3])
{
    __try { std::memcpy(o, reinterpret_cast<const void*>(a), 12); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WrF3(uintptr_t a, const float v[3])
{
    __try { std::memcpy(reinterpret_cast<void*>(a), v, 12); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BytesMatch(uintptr_t a, const unsigned char* b, size_t n)
{
    __try { return std::memcmp(reinterpret_cast<const void*>(a), b, n) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t LocalPlayer()
{
    uintptr_t gdi = 0, a = 0, b = 0, p = 0;
    if (RdQ(g_gd + DLVR_CH_GD(kGameDiRva), &gdi) && gdi && RdQ(gdi + 1344, &a) && a && RdQ(a + 184, &b) && b && RdQ(b + 88, &p)) {
        return p;
    }
    return 0;
}

bool Feet(uintptr_t player, float out[3])
{
    __try {
        alignas(16) float v[4] = {};
        g_pos(reinterpret_cast<const void*>(player + 0x18), v);
        out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The hit's owner (SCollision+0x18 - 24, as grappleaim.cpp names it) is a character? Per-search cache (a search casts ~75 rays, most
// on the same one or two objects), reset by ServeSearch. Every class seen is logged once a session (the names this filter keys on
// were read from the gamedll's RTTI, not yet seen on a hit - the log shows what a zombie hit really reports).
struct CharCache { uintptr_t obj[8]; bool isChar[8]; int n; };
CharCache g_charCache = {};
char      g_seenCls[32][48] = {};
int       g_seenN = 0;

bool OwnerIsCharacter(uintptr_t collObj)
{
    if (!collObj) { return false; }   // static world geometry carries no object
    for (int i = 0; i < g_charCache.n; ++i) {
        if (g_charCache.obj[i] == collObj) { return g_charCache.isChar[i]; }
    }
    const uintptr_t owner = collObj - 24;
    char cls[48] = {};
    const bool named = RttiClassNameOf(owner, cls, sizeof(cls));
    const bool isChar = named && climbhand::IsCharacterClass(cls);
    if (g_charCache.n < 8) {
        g_charCache.obj[g_charCache.n] = collObj;
        g_charCache.isChar[g_charCache.n] = isChar;
        ++g_charCache.n;
    }
    bool seen = false;
    for (int i = 0; i < g_seenN && !seen; ++i) { seen = std::strcmp(g_seenCls[i], named ? cls : "?") == 0; }
    if (!seen && g_seenN < 32) {
        strcpy_s(g_seenCls[g_seenN++], named ? cls : "?");
        if (Line()) {
            // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] first ray hit on class '%s' (owner %p, from SCollision+0x18 - 24) -> %s", named ? cls : "? (no RTTI)",
                reinterpret_cast<void*>(owner), isChar ? "a CHARACTER: such hits are skipped" : "world: used");
        }
    }
    return isChar;
}

// One engine ray, as the ledge grab casts (+D0E046). hit = the point (the engine writes it into `to`). A hit on a character is
// reported as a miss (run 5: zombies were grabbed and climbed).
struct Caster {
    uintptr_t player;
    bool fault = false;
    bool operator()(const float from[3], const float to[3], float hit[3])
    {
        InterlockedIncrement64(&g_wCasts);
        __try {
            alignas(16) unsigned char coll[128] = {};
            *reinterpret_cast<int*>(coll + 32) = -1;   // as swing.cpp's replica cast initialises it
            alignas(16) float f[4] = { from[0], from[1], from[2], 0.0f };
            alignas(16) float t[4] = { to[0], to[1], to[2], 0.0f };
            bool r = g_ray(reinterpret_cast<void*>(player + 40), kRayMask, coll, f, t, 0,
                           reinterpret_cast<void*>(player + 24), 8u, 0u, 0) != 0;
            if (r) {
                hit[0] = t[0]; hit[1] = t[1]; hit[2] = t[2];
                if (OwnerIsCharacter(*reinterpret_cast<const uintptr_t*>(coll + 0x18))) {
                    InterlockedIncrement64(&g_wCharHits);
                    r = false;
                }
            }
            return r;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fault = true;
            return false;
        }
    }
};

// The game's own start, as +BD4C29..+BD4C7C does it, on the target already in the record. False + why on a refusal.
// start = false (P4 round 3's re-target in place): everything but the group switch - the running ledge grab is handed the new target
// record and the player's ledge (+0x14C8 mid / +0x14BC dir) is rewritten, so its own climb-up / shimmies run from it.
bool DoStart(uintptr_t player, uintptr_t eg, uintptr_t hook, const char** why, bool start = true)
{
    float a[3], b[3], dir[3];
    if (!RdF3(hook, a) || !RdF3(hook + 0xC, b) || !RdF3(hook + 0x18, dir)) { *why = "the target record is unreadable"; return false; }
    uintptr_t vt = 0, setHook = 0;
    if (!RdQ(eg, &vt) || !RdQ(vt + kSetHookVfn, &setHook) || !BytesMatch(setHook, kSetHookBytes, sizeof(kSetHookBytes))) {
        *why = "EdgeGrab vtable slot 35 is not 'mov [rcx+38h], rdx; ret' - REFUSED";
        return false;
    }
    const float mid[3] = { (a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f };
    __try {
        if (!WrF3(player + kHookMid, mid) || !WrF3(player + kHookDir, dir)) { *why = "a player write faulted"; return false; }
        reinterpret_cast<SetHookFn>(setHook)(eg, hook);
        if (start) { g_setGroup(player, kGroupEdgeGrab, true, nullptr); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "the game call faulted";
        return false;
    }
    return true;
}

bool CallSetTarget(uintptr_t player, const climbhand::Ledge& L)
{
    __try {
        alignas(16) float a[4] = { L.a[0], L.a[1], L.a[2], 0.0f }, b[4] = { L.b[0], L.b[1], L.b[2], 0.0f },
                          d[4] = { L.into[0], L.into[1], L.into[2], 0.0f };
        g_setTarget(player, a, b, d, kTargetTag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetState(uintptr_t eg, int state)
{
    __try { g_setState(eg, state); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// P4 round 3: re-target the RUNNING ledge grab onto the ledge the hands hold (mid = LedgeHeld's point, into = its inward direction),
// as a hand grab targets it (lip -+ kHalfEdgeM along the wall): the game's set-target, then DoStart without the group switch. No drop,
// no new start - the hang goes on. Game thread (StateHook / Hook). False + why on a refusal.
bool RetargetInPlace(uintptr_t player, uintptr_t eg, const float mid[3], const float into[3], const char** why)
{
    float ix = into[0], iz = into[2];
    const float il = std::sqrt(ix * ix + iz * iz);
    if (!(il > 1e-3f)) { *why = "the held ledge has no inward direction"; return false; }
    ix /= il; iz /= il;
    climbhand::Ledge L;
    for (int i = 0; i < 3; ++i) { L.lip[i] = mid[i]; }
    L.into[0] = ix; L.into[1] = 0.0f; L.into[2] = iz;
    const float sx = -iz, sz = ix;   // along the ledge
    L.a[0] = L.lip[0] - sx * climbhand::kHalfEdgeM; L.a[1] = L.lip[1]; L.a[2] = L.lip[2] - sz * climbhand::kHalfEdgeM;
    L.b[0] = L.lip[0] + sx * climbhand::kHalfEdgeM; L.b[1] = L.lip[1]; L.b[2] = L.lip[2] + sz * climbhand::kHalfEdgeM;
    if (!CallSetTarget(player, L)) { *why = "the set-target call faulted"; return false; }
    uintptr_t hook = 0;
    if (!RdQ(player + kHookPtr, &hook) || !hook) { *why = "no target record"; return false; }
    return DoStart(player, eg, hook, why, false);
}

const char* WhyName(int w)
{
    static const char* const k[] = { "found", "no direction (hand over the feet)", "no wall face near the hand",
                                     "no top (the wall goes on up)", "the lip is out of reach", "the top is not flat",
                                     "only the back of a wall near the hand (the hand reached through it)",
                                     "something between the body and the wall face (the hand reached through it)",
                                     "no room above the top (the wall goes on above it)" };
    static_assert(sizeof(k) / sizeof(k[0]) == climbhand::kWhyCount, "one name per LedgeWhy");
    return w >= 0 && w < climbhand::kWhyCount ? k[w] : "?";
}

void PublishFound(int hand, ULONGLONG now, const float lip[3], const float into[3])
{
    AcquireSRWLockExclusive(&g_lock);
    g_foundMs[hand] = now;
    for (int i = 0; i < 3; ++i) {
        g_foundLip[hand][i] = lip[i];
        g_foundInto[hand][i] = into[i];
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// The player's walk interface (PhysicsBind(player+0x18) -> bind vfunc +0xB0), checked as roomscale checks it: interface - 0x700
// must carry CODEPhysicsWalkFly's vtable (engine +86DC38). Null + why otherwise. Game thread.
uintptr_t WalkIface(uintptr_t player, const char** why)
{
    if (!g_bindFn) { *why = "the engine exports are missing"; return 0; }
    __try {
        void* bind = g_bindFn(reinterpret_cast<void*>(player + 0x18));
        if (!bind) { *why = "PhysicsBind returned null"; return 0; }
        uintptr_t bvt = 0, getter = 0;
        if (!RdQ(reinterpret_cast<uintptr_t>(bind), &bvt) || !RdQ(bvt + kBindWalkVfn, &getter) || !getter) {
            *why = "the bind's walk getter is unreadable";
            return 0;
        }
        void* iface = reinterpret_cast<void* (__fastcall*)(void*)>(getter)(bind);
        if (!iface) { *why = "no walk body yet"; return 0; }
        uintptr_t fvt = 0;
        if (!RdQ(reinterpret_cast<uintptr_t>(iface) - kWalkIfaceToFull, &fvt) || fvt - g_en != DLVR_CH_EN(kWalkFullVtRva)) {
            *why = "the walk object is not CODEPhysicsWalkFly - REFUSED";
            return 0;
        }
        return reinterpret_cast<uintptr_t>(iface);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a call faulted";
        return 0;
    }
}

// P4 round 8: three walk interface vfuncs, used only when their code is byte-for-byte what was read [BIN, engine e9fbfc2c] - the
// check reads the function the live vtable points at, so a different build refuses (logged once) instead of calling something else:
//   +0x468 (engine +3D37E0) "in the air": movzx eax, byte [rcx+2C0h] / shr al,1 / and al,1 / ret - bit 1 of walk +0x9C0
//          (climbhand_logic.h FeetVel's note);
//   +0x390 (engine +3D3680) the air factor: movss xmm0, [rcx+3ACh] / ret - LogicJumpAirControllFactor as the walk holds it;
//   +0x260 (engine +3D3760) the VERTICAL speed: movss [rcx+0ACh], xmm1 / ret - walk +0x7AC, which the walk's air step adds to its
//          own fall (+3D8666); the gamedll's own velocity hand-over (+D68FB4..+D6901F) sets the horizontal through +0x2D0 and the
//          vertical through this one;
//   +0x268 (engine +3D3770) clears it: mov dword [rcx+0ACh], 0 / ret - the gamedll clears it the same way (+D648F0).
constexpr uintptr_t kWalkAirVfn = 0x468, kWalkAirKVfn = 0x390, kWalkVertSetVfn = 0x260, kWalkVertClrVfn = 0x268;
const unsigned char kWalkAirBytes[12] = { 0x0F, 0xB6, 0x81, 0xC0, 0x02, 0x00, 0x00, 0xD0, 0xE8, 0x24, 0x01, 0xC3 };
const unsigned char kWalkAirKBytes[9] = { 0xF3, 0x0F, 0x10, 0x81, 0xAC, 0x03, 0x00, 0x00, 0xC3 };
const unsigned char kWalkVertSetBytes[9] = { 0xF3, 0x0F, 0x11, 0x89, 0xAC, 0x00, 0x00, 0x00, 0xC3 };
const unsigned char kWalkVertClrBytes[11] = { 0xC7, 0x81, 0xAC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC3 };
volatile LONG g_saidWalkVfn = 0;
// The function at vtable slot `slot` of the walk interface, if its code matches `bytes`; 0 otherwise (logged once per run).
uintptr_t WalkVfn(uintptr_t iface, uintptr_t slot, const unsigned char* bytes, size_t n, const char* name)
{
    uintptr_t vt = 0, fn = 0;
    if (!RdQ(iface, &vt) || !RdQ(vt + slot, &fn) || !fn) { return 0; }
    for (size_t i = 0; i < n; ++i) {
        unsigned char b = 0;
        if (!RdB(fn + i, &b) || b != bytes[i]) {
            if (InterlockedCompareExchange(&g_saidWalkVfn, 1, 0) == 0 && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_air_momentum  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] walk interface vfunc +0x%llX (%s) at engine+0x%llX does not match the bytes read for this build - "
                    "REFUSED (logged once): %s", static_cast<unsigned long long>(slot), name,
                    static_cast<unsigned long long>(fn - g_en), slot == kWalkAirVfn ? "no air carries for the game's own air time; "
                    "launches fall back to the ballistic landing test" : "that part is skipped");
            }
            return 0;
        }
    }
    return fn;
}
// The walk's own "in the air" (1 / 0), -1 when it cannot be read. Game thread.
int WalkAirborne(uintptr_t iface)
{
    const uintptr_t fn = iface ? WalkVfn(iface, kWalkAirVfn, kWalkAirBytes, sizeof(kWalkAirBytes), "in the air") : 0;
    if (!fn) { return -1; }
    __try {
        return reinterpret_cast<bool (__fastcall*)(uintptr_t)>(fn)(iface) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// The game's per-update air factor (the walk's air factor cubed, as +3D87D6 applies it); kGameAirK when it cannot be read.
float WalkAirK(uintptr_t iface)
{
    const uintptr_t fn = iface ? WalkVfn(iface, kWalkAirKVfn, kWalkAirKBytes, sizeof(kWalkAirKBytes), "air factor") : 0;
    if (!fn) { return climbhand::kGameAirK; }
    float f = -1.0f;
    __try {
        f = reinterpret_cast<float (__fastcall*)(uintptr_t)>(fn)(iface);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return climbhand::kGameAirK; }
    if (!(f >= 0.0f && f <= 1.0f)) { return climbhand::kGameAirK; }
    return f * f * f;
}
// The walk's vertical speed (+0x260; clear = +0x268). False when refused or faulted.
bool WalkVertical(uintptr_t iface, bool set, float vy)
{
    const uintptr_t fn = !iface ? 0 : (set ? WalkVfn(iface, kWalkVertSetVfn, kWalkVertSetBytes, sizeof(kWalkVertSetBytes), "vertical speed")
                                           : WalkVfn(iface, kWalkVertClrVfn, kWalkVertClrBytes, sizeof(kWalkVertClrBytes), "clear vertical"));
    if (!fn) { return false; }
    __try {
        if (set) { reinterpret_cast<void (__fastcall*)(uintptr_t, float)>(fn)(iface, vy); }
        else     { reinterpret_cast<void (__fastcall*)(uintptr_t)>(fn)(iface); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
// The walk's flags word (walk +0x9C0, its +0x3D8 getter's field; read for the trace lines only), 0xFFFF unread.
unsigned short WalkFlags(uintptr_t iface)
{
    unsigned short w = 0xFFFF;
    unsigned char lo = 0, hi = 0;
    if (iface && RdB(iface + 0x2C0, &lo) && RdB(iface + 0x2C1, &hi)) { w = static_cast<unsigned short>(lo | (hi << 8)); }
    return w;
}

// P3's launch: hand the walk body a velocity, the way the ledge grab's own exit does (+D1204E). False + why when the chain is not
// there (the walk body not rebuilt yet, or not CODEPhysicsWalkFly) or a call faults. Game thread.
bool ApplyBodyVel(uintptr_t player, const float v[3], const char** why)
{
    if (!g_bindFn || !g_setVel) { *why = "the engine exports are missing"; return false; }
    const uintptr_t ifaceU = WalkIface(player, why);
    if (!ifaceU) { return false; }
    __try {
        void* iface = reinterpret_cast<void*>(ifaceU);
        uintptr_t ivt = 0, bodyFn = 0;
        if (!RdQ(reinterpret_cast<uintptr_t>(iface), &ivt) || !RdQ(ivt + kWalkBodyVfn, &bodyFn) || !bodyFn) {
            *why = "the walk body getter is unreadable";
            return false;
        }
        void* body = reinterpret_cast<void* (__fastcall*)(void*)>(bodyFn)(iface);
        if (!body) { *why = "the walk interface has no body"; return false; }
        alignas(16) float vv[4] = { v[0], v[1], v[2], 0.0f };
        g_setVel(body, vv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a call faulted";
        return false;
    }
    *why = "ok";
    return true;
}

// One hand's search. climbing = the ledge grab already runs: a ledge in reach only JOINS this hand to the hold (no new start -
// round 7: a hand holds only where a ledge is). Otherwise a ledge in reach starts the grab. Game thread, inside the start detour;
// ServeSearches calls it for each searching hand.
void ServeSearch(int hand, const float pos[3], uintptr_t player, uintptr_t eg, uintptr_t hook, bool climbing, bool onFoot,
                 const float feet[3])
{
    const ULONGLONG now = GetTickCount64();
    if (!climbing && !onFoot) { return; }   // a new grab only from on foot (roomscale's rule); a join any time the grab runs
    InterlockedIncrement64(&g_wSearchUpd);
    Caster cast{ player };
    climbhand::Ledge L;
    const int why = climbhand::FindLedge(pos, feet, ClimbReachM, cast, &L);
    if (cast.fault) { InterlockedIncrement64(&g_wFaults); }
    InterlockedIncrement64(&g_wWhy[why]);
    if (why == climbhand::kLedgeOk && climbing) {
        PublishFound(hand, now, L.lip, L.into);
        if (g_lastWhy[hand] != 100 && HandLine()) {
            // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] JOIN - %s hand is on a ledge (lip at feet+%.2f m, %.2f m from the hand) while the grab runs: it holds too",
                hand == 0 ? "left" : "right", L.lip[1] - feet[1], L.dist);
        }
        g_lastWhy[hand] = 100;
        return;
    }
    if (why != climbhand::kLedgeOk) {
        if (why != g_lastWhy[hand] && HandLine()) {
            // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] search (%s hand at feet+(%.2f, %.2f, %.2f)): %s | %d rays", hand == 0 ? "left" : "right",
                pos[0] - feet[0], pos[1] - feet[1], pos[2] - feet[2], WhyName(why), L.rays);
            if (why == climbhand::kTooFar) {
                LOG("[CLIMBHAND]   lip at feet+%.2f m, %.2f m from the hand (climb_reach_m %.2f)", L.lip[1] - feet[1], L.dist,
                    ClimbReachM);
            }
        }
        g_lastWhy[hand] = why;
        return;
    }
    // Compare with the game's own LOOK target before it is overwritten (the direction convention check - log only).
    float ga[3] = {}, gb[3] = {}, gd[3] = {};
    const bool gameValid = RdF3(hook, ga) && RdF3(hook + 0xC, gb) && RdF3(hook + 0x18, gd) && climbhand::TargetValid(ga, gb);
    const char* whyNot = "ok";
    bool done = false;
    const bool hang = climbhand::ForceHang(ClimbAlwaysHang, L.lip[1], feet[1], ClimbHangMinM);
    if (!CallSetTarget(player, L)) {
        whyNot = "the set-target call faulted";
    } else {
        // Both hang hooks armed for THIS start: the type chooser (0) and, the one that decides, the grab's end transition (8 -> 1).
        g_forceHangUntil = hang && g_typeLive == 1 ? now + 1000 : 0;
        g_hangEg = hang && g_stateLive == 1 ? eg : 0;
        g_hangUntil = now + 3000;
        done = DoStart(player, eg, hook, &whyNot);
        if (!done) { g_forceHangUntil = 0; g_hangEg = 0; }
    }
    InterlockedIncrement64(done ? &g_wStarts : &g_wRefused);
    if (done) { PublishFound(hand, now, L.lip, L.into); }   // this hand holds the grab it started, at this ledge point
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] GRAB %s - %s hand | lip at feet+%.2f m, %.2f m from the hand, %d rays | into wall (%.2f, %.2f) | %s | game's "
            "look target %s", done ? "started" : "REFUSED", hand == 0 ? "left" : "right", L.lip[1] - feet[1], L.dist, L.rays,
            L.into[0], L.into[2],
            hang ? (g_stateLive == 1 ? "will HANG" : "should hang but the hang detour is NOT LIVE")
                 : (ClimbAlwaysHang ? "below climb_hang_min_m: the game decides (climbs over)" : "the game decides"),
            gameValid ? "valid" : "none");
        if (gameValid) {
            LOG("[CLIMBHAND]   game's target: mid (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f) | ours: mid (%.2f, %.2f, %.2f) dir (%.2f, "
                "%.2f, %.2f)", (ga[0] + gb[0]) * 0.5f, (ga[1] + gb[1]) * 0.5f, (ga[2] + gb[2]) * 0.5f, gd[0], gd[1], gd[2],
                L.lip[0], L.lip[1], L.lip[2], L.into[0], L.into[1], L.into[2]);
        }
        if (!done) { LOG("[CLIMBHAND]   refused because: %s", whyNot); }
    }
    g_lastWhy[hand] = -1;
}

// Every searching hand, this update (both at once). climbing as for ServeSearch; re-read after each hand, so a grab one hand
// starts lets the other hand join it in the same update.
void ServeSearches(uintptr_t player, uintptr_t eg, uintptr_t hook, bool onFoot, const float feet[3], bool haveFeet)
{
    bool on[2];
    float pos[2][3];
    ULONGLONG at;
    AcquireSRWLockShared(&g_lock);
    for (int h = 0; h < 2; ++h) {
        on[h] = g_searchOn[h];
        for (int i = 0; i < 3; ++i) { pos[h][i] = g_searchPos[h][i]; }
    }
    at = g_searchMs;
    ReleaseSRWLockShared(&g_lock);
    for (int h = 0; h < 2; ++h) { if (!on[h]) { g_lastWhy[h] = -1; } }
    if ((!on[0] && !on[1]) || !haveFeet || !hook || GetTickCount64() - at > 100) { return; }   // nothing / the pad thread stopped
    g_charCache.n = 0;   // objects move and die between searches: name them again
    for (int h = 0; h < 2; ++h) {
        if (!on[h]) { continue; }
        unsigned char act = 0;
        const bool climbing = RdB(eg + kActiveOff, &act) && act != 0;
        ServeSearch(h, pos[h], player, eg, hook, climbing, onFoot, feet);
    }
}

float WrapDeg(float d)
{
    while (d > 180.0f) { d -= 360.0f; }
    while (d < -180.0f) { d += 360.0f; }
    return d;
}

// P4 look probe (log-only): while the ledge grab holds by hand (hold states), how the game holds the look - LookController's window
// and how far the camera / look yaw and the look pitch actually travelled. One line when the hold ends, so a run answers "what
// limits turning (stick / snap) and looking behind while hanging" (design §4.7). Game thread. COST: one boneblend read per update.
void HangLookTick(uintptr_t player, bool holding)
{
    if (!holding) {
        if (g_hl.on && g_hl.n > 0 && Line()) {
            // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=yes
            LOG("[CLIMBHAND] HANG LOOK over %ld update(s)%s | look window %s max (yaw %.1f, pitch %.1f) min (yaw %.1f, pitch %.1f); "
                "over the hang max yaw %.1f..%.1f, min yaw %.1f..%.1f, max pitch %.1f..%.1f, min pitch %.1f..%.1f | camera yaw "
                "travelled %+.1f..%+.1f deg, look yaw %+.1f..%+.1f deg (from the hang's start), look pitch %.1f..%.1f",
                g_hl.n, g_hl.notLook ? " (some with another look owner)" : "", g_hl.haveWin ? "first" : "NOT READ",
                g_hl.win0[0], g_hl.win0[1], g_hl.win0[2], g_hl.win0[3], g_hl.winLo[0], g_hl.winHi[0], g_hl.winLo[2], g_hl.winHi[2],
                g_hl.winLo[1], g_hl.winHi[1], g_hl.winLo[3], g_hl.winHi[3], g_hl.camLo, g_hl.camHi, g_hl.lookLo, g_hl.lookHi,
                g_hl.pitchLo, g_hl.pitchHi);
        }
        g_hl = HangLook{};
        return;
    }
    float s[7];
    bool isLook = false;
    if (!HangLookSample(player, s, &isLook)) { return; }
    if (!g_hl.on) {
        g_hl.on = true;
        g_hl.cam0 = g_hl.prevCam = s[4];
        g_hl.look0 = g_hl.prevLook = s[5];
        g_hl.pitchLo = g_hl.pitchHi = s[6];
    }
    ++g_hl.n;
    g_hl.camRel += WrapDeg(s[4] - g_hl.prevCam);
    g_hl.lookRel += WrapDeg(s[5] - g_hl.prevLook);
    g_hl.prevCam = s[4];
    g_hl.prevLook = s[5];
    if (g_hl.camRel < g_hl.camLo) { g_hl.camLo = g_hl.camRel; }
    if (g_hl.camRel > g_hl.camHi) { g_hl.camHi = g_hl.camRel; }
    if (g_hl.lookRel < g_hl.lookLo) { g_hl.lookLo = g_hl.lookRel; }
    if (g_hl.lookRel > g_hl.lookHi) { g_hl.lookHi = g_hl.lookRel; }
    if (s[6] < g_hl.pitchLo) { g_hl.pitchLo = s[6]; }
    if (s[6] > g_hl.pitchHi) { g_hl.pitchHi = s[6]; }
    if (!isLook) { ++g_hl.notLook; return; }
    if (!std::isfinite(s[0]) || !std::isfinite(s[1]) || !std::isfinite(s[2]) || !std::isfinite(s[3])) { return; }
    for (int i = 0; i < 4; ++i) {
        if (!g_hl.haveWin) { g_hl.win0[i] = g_hl.winLo[i] = g_hl.winHi[i] = s[i]; continue; }
        if (s[i] < g_hl.winLo[i]) { g_hl.winLo[i] = s[i]; }
        if (s[i] > g_hl.winHi[i]) { g_hl.winHi[i] = s[i]; }
    }
    g_hl.haveWin = true;
}

// P4 round 3: the ledge the hands hold (published by the pad thread) against the game's (its hook, +0x14C8 mid / +0x14BC dir).
struct HeldVsGame {
    bool  stale = false;
    bool  held = false;    // the ledge the hands hold is known (published within 250 ms) - round 8 re-targets on it
    bool  haveHook = false;
    climbhand::Hold h;
    float hookMid[3] = {}, hookDir[3] = {};
};
HeldVsGame ReadHeldVsGame(ULONGLONG now)
{
    HeldVsGame s;
    ULONGLONG at;
    AcquireSRWLockShared(&g_lock);
    s.h = g_hold;
    at = g_holdPubMs;
    ReleaseSRWLockShared(&g_lock);
    s.haveHook = g_curPlayer && RdF3(g_curPlayer + kHookMid, s.hookMid) && RdF3(g_curPlayer + kHookDir, s.hookDir);
    s.held = s.h.haveTop && now - at <= 250;
    s.stale = climbhand::HookMismatch(s.held, s.h.mid, s.h.into, s.haveHook, s.hookMid, s.hookDir);
    return s;
}

// Re-target the running grab onto the held ledge (RetargetInPlace) and say so. Game thread.
void RetargetOnHeld(uintptr_t player, uintptr_t eg, const HeldVsGame& s, ULONGLONG now, const char* when)
{
    const char* why = "ok";
    const bool done = player && RetargetInPlace(player, eg, s.h.mid, s.h.into, &why);
    g_retargetMs = now;   // a failed one also waits kRetargetHoldMs (the game's move then passes as it asked)
    InterlockedIncrement64(done ? &g_wRetargets : &g_wRetargetFails);
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] RE-TARGET %s (%s) - the game's ledge (%.2f, %.2f, %.2f) dir (%.2f, %.2f) -> the held one (%.2f, %.2f, %.2f) "
            "dir (%.2f, %.2f)%s%s", done ? "done" : "REFUSED", when, s.hookMid[0], s.hookMid[1], s.hookMid[2], s.hookDir[0],
            s.hookDir[2], s.h.mid[0], s.h.mid[1], s.h.mid[2], s.h.into[0], s.h.into[2], done ? "" : " - ", done ? "" : why);
    }
}

void __fastcall Hook(uintptr_t player)
{
    if (!ClimbByHand) { g_true(player); return; }
    if (player == 0 || player != LocalPlayer()) { g_true(player); return; }
    InterlockedIncrement64(&g_wUpd);
    g_curPlayer = player;
    uintptr_t eg = 0, hook = 0;
    RdQ(player + kEdgeGrabSlot, &eg);
    if (eg != g_egPtr) {   // name it once per pointer
        g_egPtr = eg;
        char cls[64] = {};
        g_egOk = eg && RttiClassNameOf(eg, cls, sizeof(cls)) && std::strcmp(cls, "EdgeGrabController") == 0;
        if (Line()) {
            // [PROBE: CLIMBHAND] gate=climb_by_hand  site=FLAT  log-only=no
            LOG("[CLIMBHAND] PlayerDI+0x%llX = %p class '%s' -> %s", static_cast<unsigned long long>(kEdgeGrabSlot),
                reinterpret_cast<void*>(eg), cls, g_egOk ? "the ledge grab, used" : "*** NOT EdgeGrabController - hand climbing "
                "passes everything to the game for this player ***");
        }
    }
    if (!g_egOk) { g_true(player); return; }
    unsigned char act = 0;
    int state = -1, group = -99;
    const bool readOk = RdB(eg + kActiveOff, &act) && RdI(eg + kEgStateOff, &state) && RdQ(player + kHookPtr, &hook);
    RdI(player + kGroupOff, &group);
    if (!readOk) { InterlockedIncrement64(&g_wFaults); }
    float feet[3] = {};
    const bool haveFeet = Feet(player, feet);
    const bool onFoot = RoomscaleOnFoot(group);
    const ULONGLONG now = GetTickCount64();
    bool active = act != 0;
    HangLookTick(player, active && climbhand::IsHoldState(state));   // P4 look probe (log-only)

    if (active) {
        InterlockedIncrement64(&g_wPassActive);
        g_true(player);   // a climb is running: the game's chained grabs (hang jumps) keep working
    } else {
        // Low walls stay the game's own (look + jump), on the game's own target.
        float ga[3] = {}, gb[3] = {};
        const bool gameValid = hook && RdF3(hook, ga) && RdF3(hook + 0xC, gb) && climbhand::TargetValid(ga, gb);
        const float top = ga[1] > gb[1] ? ga[1] : gb[1];
        if (haveFeet && climbhand::AutoLowWall(gameValid, top, feet[1], ClimbAutoLowM, ClimbAutoLow)) {
            InterlockedIncrement64(&g_wPassLow);
            g_true(player);
        } else {
            InterlockedIncrement64(&g_wSkipped);
        }
    }
    // Round 7: the hands search whether or not a climb runs - a start when none does, a join (the hand holds too) when one does.
    ServeSearches(player, eg, hook, onFoot, feet, haveFeet);

    if (!act) { g_climbUpAsked = false; g_wantClimbUp = false; }
    if (g_wantClimbUp) {   // climb_mantle_auto: pulled up far enough - the game's own climb-up, as the stick asks for it (1 -> 8)
        g_wantClimbUp = false;
        const bool can = act != 0 && climbhand::IsHoldState(state);
        if (can) {   // P4 round 3: from the ledge the hands hold, not the one the grab started on
            const HeldVsGame s = ReadHeldVsGame(now);
            if (s.held) { RetargetOnHeld(player, eg, s, now, "before climb_mantle_auto's climb-up"); }   // round 8: on demand
        }
        g_ourClimbUp = true;
        const bool done = can && CallSetState(eg, 8);
        g_ourClimbUp = false;
        int now8 = -1;
        RdI(eg + kEgStateOff, &now8);
        if (done) { InterlockedIncrement64(&g_wClimbUps); }
        if (Line()) {
            // [PROBE: CLIMBHAND] gate=climb_mantle_auto  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] CLIMB-UP asked (climb_mantle_auto: the ledge %.2f m above the feet, climb_mantle_m %.2f) - set-state 8 %s "
                "(state %d -> %d)", g_climbUpGap, ClimbMantleM, done ? "called" : (can ? "FAULTED" : "not called: not a plain hold"),
                state, now8);
        }
    }
    if (const LONG req = InterlockedExchange(&g_reqDrop, 0)) {   // no hand on the ledge any more (the pad thread)
        float lv[3] = {};
        // P4 round 5: a LAUNCH is the game's own drop at once, the launch velocity then carried by the walk body (the post path's
        // SetLinearVel for the rise, ClimbHandWalkOverride's carry for the horizontal speed every update until it lands).
        // P4 round 9 / 10: only when the grips were opened (req 1) or a hand tore off in a jump flick (req 3) - a slip (req 2) is a
        // plain drop.
        if ((req == 1 || req == 3) && act != 0 && climbhand::IsHoldState(state) && ClimbHandMotion &&
            climbhand::LaunchPick(g_motion, now, ClimbLaunchMinMps, ClimbLaunchScale, lv, ClimbLaunchUpAssist)) {
            const bool done = CallSetState(eg, kStateDrop);
            InterlockedIncrement64(done ? &g_wDrops : &g_wRefused);
            if (done) {
                InterlockedIncrement64(&g_wLaunches);
                for (int i = 0; i < 3; ++i) { g_postVel[i] = lv[i]; }
                g_postOn = true;
                g_postLeft = 3;
                g_postUntil = now + 700;
                climbhand::StartCarry(g_carry, lv, now);
                g_carryAir = false;
                g_carryFeet0Ms = 0;
                g_carrySaid = false;
                g_traceOn = g_traceLaunches < kTraceLaunches;
                if (g_traceOn) { ++g_traceLaunches; }
            }
            g_motion.ringN = 0;
            if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_launch_min_mps  site=HEADSET  log-only=no
                char how[112];
                if (req == 3) {
                    std::snprintf(how, sizeof(how), "the hands tore off in a flick (grips closed, a controller at %.2f m/s)",
                                  static_cast<double>(g_dropHandMps));
                } else {
                    std::snprintf(how, sizeof(how), "let go while pulling");
                }
                LOG("[CLIMBHAND] LAUNCH %s - %s: velocity (%.2f, %.2f, %.2f) m/s (x climb_launch_scale %.2f, up "
                    "assist %.2f, capped at the game's jump: %.2f up / %.2f sideways) | the ledge grab dropped at once: the game's own "
                    "fall, the walk carries the horizontal speed until it lands - %s", done ? "started" : "REFUSED (the drop faulted)",
                    how,
                    lv[0], lv[1], lv[2], ClimbLaunchScale, ClimbLaunchUpAssist, climbhand::kLaunchMaxUpMps, climbhand::kLaunchMaxSideMps,
                    ClimbAirMomentum ? "climb_air_momentum: the body's own momentum, air drag only, no stick" :
                                       "the launch's speed plus the stick's");
            }
        } else {
            const bool can = act != 0 && climbhand::IsHangState(state);
            const bool done = can && CallSetState(eg, kStateDrop);
            InterlockedIncrement64(done ? &g_wDrops : &g_wRefused);
            if (done) { InterlockedIncrement64(&g_wPlainDrops); }
            if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] DROP %s (ledge grab state %d%s)%s | fastest controller %.2f m/s (a flick from climb_launch_min_mps "
                    "%.2f)", done ? "asked (set-state 16)" : "REFUSED", state,
                    can ? (done ? "" : ", the call faulted") : ", not a hang state",
                    req == 2 ? " - a hand SLIPPED (grip still closed) with the controllers still: no launch" :
                    (req == 3 ? " - a hand tore off in a flick, but no launch speed was picked" : ""),
                    static_cast<double>(g_dropHandMps), static_cast<double>(ClimbLaunchMinMps));
            }
        }
    }
    // After a launch's drop: once the ledge grab has handed back to the walk body, give it the launch velocity (its rise - the walk's
    // own update then overwrites the horizontal with its wanted vector, which the carry fills). A few updates in a row, as the
    // controller switch may reset the body once.
    if (g_postOn) {
        if (now > g_postUntil) {
            if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_launch_min_mps  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] launch velocity after the drop: %d of 3 call(s) made before the time ran out", 3 - g_postLeft);
            }
            g_postOn = false;
        } else if (!act && g_postLeft > 0) {
            const char* why = "";
            const bool ok = ApplyBodyVel(player, g_postVel, &why);
            // P4 round 8 (run 15, tester: "Jumping up from hanging is now impossible"): the body's velocity alone does not keep the
            // rise - once the walk holds the body as in the air it runs its own vertical (run 15: 3.2 -> -0.2 m/s in one update the
            // moment its flags went 0x0001 -> 0x0002; every launch from a high hang lost it at once). The vertical goes where the
            // game's own velocity hand-over puts it: the walk's vertical speed (+0x260, WalkVertical), cleared when the carry ends.
            const uintptr_t wi = ok && g_carry.on && !g_carryAir ? WalkIface(player, &why) : 0;   // (only under the launch's carry,
            if (wi && WalkVertical(wi, true, g_postVel[1])) { g_vertSet = true; }                    // whose end clears it)
            if (ok) {
                --g_postLeft;
            } else if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_launch_min_mps  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] launch velocity after the drop NOT applied: %s", why);
            }
        } else if (g_postLeft == 0) {
            g_postOn = false;   // (the carry's line measures what the body did)
        }
    }

    climbhand::GameView v;
    unsigned char act3 = 0;
    v.egActive = RdB(eg + kActiveOff, &act3) && act3 != 0;
    RdI(eg + kEgStateOff, &v.egState);
    v.onFoot = onFoot;
    AcquireSRWLockExclusive(&g_lock);
    g_pub = v;
    g_pubMs = now;
    ReleaseSRWLockExclusive(&g_lock);
}

// The climb-type chooser: a hand grab on a high ledge hangs (type 0) instead of the direct climb the game chose.
int __fastcall TypeHook(void* eg, void* a2, float* dir, float* height, void* a5)
{
    const int t = g_trueType(eg, a2, dir, height, a5);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_typeRet || g_forceHangUntil == 0) { return t; }
    const bool due = GetTickCount64() <= g_forceHangUntil;
    g_forceHangUntil = 0;   // one start, one choice
    if (!due || !ClimbByHand || !ClimbAlwaysHang || t == 0) { return t; }
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=climb_always_hang  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] climb type %d (straight over) -> 0 for this hand grab (the hang itself is decided at the grab's end)", t);
    }
    return 0;
}

// The grab's end transition (+D0A240's set-state at +D0A4B9): for the hand grab that should hang, 0 -> 8 (climb up) becomes 0 -> 1
// (hang). Every other caller and every other ledge grab pass straight through. Logs the game's own hang flags (+0x204 / +0x205) so a
// hang the game itself would not have chosen is visible.
using TrueStateFn = void(__fastcall*)(uintptr_t eg, int state);
TrueStateFn g_trueState = nullptr;
void __fastcall StateHook(uintptr_t eg, int state)
{
    // P2 round 3: the hang's look-left / look-right (1 -> 2 / 3, +D0A240's call at +D0AD16) turns the camera - refused by hand.
    if ((state == 2 || state == 3) && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_lookRet) {
        int from = -1;
        RdI(eg + kEgStateOff, &from);
        if (climbhand::RefuseLookState(ClimbByHand, from, state)) {
            if (InterlockedIncrement64(&g_wLookRefused) == 1 && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] the hang's look-%s (state %d, it turns the camera for a side jump) REFUSED - the hang stays in 1 "
                    "(counted in the 5 s line from here)", state == 2 ? "one-way" : "other-way", state);
            }
            return;
        }
    }
    // P3 round 2 / 3, P4 round 2 / 3 / 4: the game's own moves from a hang (climb-up 8, shimmies 11-15, reach-up 5 / 9) -
    // GameMoveFromHang decides: on a STALE ledge (the game still on an older ledge than the one the hands hold) this move is refused
    // and the game is re-targeted onto the held ledge in place, so it asks again from there next update; the climb-up only with the
    // stick forward; the shimmies only with climb_stick_shimmy. Ours always passes.
    if (eg == g_egPtr && (state == 8 || (state >= 11 && state <= 15) || state == 5 || state == 9)) {
        int from = -1;
        RdI(eg + kEgStateOff, &from);
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG fwdMs = g_stickFwdMs;
        const bool stickFwd = fwdMs != 0 && now - fwdMs <= climbhand::kStickFreshMs;
        const HeldVsGame s = ReadHeldVsGame(now);
        const bool fresh = g_retargetMs != 0 && now - g_retargetMs <= climbhand::kRetargetHoldMs;
        const int verdict = climbhand::GameMoveFromHang(ClimbByHand, ClimbStickShimmy, ClimbMantleStick, stickFwd, g_ourClimbUp,
                                                        s.held, fresh, from, state);
        const char* const what = state == 8 ? "climb-up" : (state == 5 || state == 9 ? "reach-up (no ledge above)" : "shimmy");
        if (verdict == climbhand::kGmPass && s.stale && fresh && ClimbByHand && !g_ourClimbUp) {
            // the re-target did not hold: the game still reads the older ledge - its move passes as it asked (round 2's behaviour)
            InterlockedIncrement64(&g_wRetargetNotHeld);
            if (now - g_gmLastMs > 3000 && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] the game's own %s (state %d -> %d) PASSED on a stale ledge - the re-target %llu ms ago did not hold: "
                    "the game's ledge (%.2f, %.2f, %.2f), the held one (%.2f, %.2f, %.2f)", what, from, state,
                    static_cast<unsigned long long>(now - g_retargetMs), s.hookMid[0], s.hookMid[1], s.hookMid[2], s.h.mid[0],
                    s.h.mid[1], s.h.mid[2]);
                g_gmLastMs = now;
            }
        }
        if (verdict == climbhand::kGmRetarget) {
            InterlockedIncrement64(&g_wGameMoveRefused);
            char when[160];
            std::snprintf(when, sizeof(when), "the game asked for its %s, state %d -> %d; its ledge %s", what, from, state,
                          !s.haveHook ? "unread" : (s.stale ? "an older one" : "the same within the stale test"));
            RetargetOnHeld(g_curPlayer, eg, s, now, when);
            return;
        }
        if (verdict != climbhand::kGmPass) {
            InterlockedIncrement64(&g_wGameMoveRefused);
            if (state == 8) { InterlockedIncrement64(&g_wStickRefused); }
            const char* why = state == 8 ? (!ClimbMantleStick ? "climb_mantle_stick is off" : "the stick is not pushed forward (the game "
                                                                                             "climbing up by itself)")
                                         : "climb_stick_shimmy is off (move along the ledge by hand)";
            const int whyId = verdict * 8 + (state == 8 ? 1 : 0);
            if ((from != g_gmLastFrom || state != g_gmLastTo || whyId != g_gmLastWhy || now - g_gmLastMs > 3000) && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] the game's own %s from a hang (state %d -> %d) REFUSED - %s", what, from, state, why);
                g_gmLastFrom = from; g_gmLastTo = state; g_gmLastWhy = whyId; g_gmLastMs = now;
            }
            return;
        }
    }
    if (g_hangEg != 0 && eg == g_hangEg && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_stateRet) {
        int from = -1;
        unsigned char f204 = 0xFF, f205 = 0xFF;
        RdI(eg + kEgStateOff, &from);
        RdB(eg + 0x204, &f204);
        RdB(eg + 0x205, &f205);
        const bool due = GetTickCount64() <= g_hangUntil && ClimbByHand && ClimbAlwaysHang;
        const int to = climbhand::HangTransition(due, from, state);
        g_hangEg = 0;   // one grab, one decision
        if (to != state) { InterlockedIncrement64(&g_wHung); }
        if (Line()) {
            // [PROBE: CLIMBHAND] gate=climb_always_hang  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] GRAB END %d -> %d%s (the game asked for %d; its hang flags +0x204 %u +0x205 %u)", from, to,
                to != state ? " = HANG (climb_always_hang)" : "", state, static_cast<unsigned>(f204), static_cast<unsigned>(f205));
        }
        state = to;
    }
    g_trueState(eg, state);
}

void Window(ULONGLONG now)
{
    if (g_winMs == 0) { g_winMs = now; return; }
    if (now - g_winMs < 5000) { return; }
    g_winMs = now;
    const LONG64 upd = InterlockedExchange64(&g_wUpd, 0), skip = InterlockedExchange64(&g_wSkipped, 0),
                 pass = InterlockedExchange64(&g_wPassActive, 0), low = InterlockedExchange64(&g_wPassLow, 0),
                 st = InterlockedExchange64(&g_wStarts, 0), dr = InterlockedExchange64(&g_wDrops, 0),
                 ref = InterlockedExchange64(&g_wRefused, 0), casts = InterlockedExchange64(&g_wCasts, 0),
                 flt = InterlockedExchange64(&g_wFaults, 0), hung = InterlockedExchange64(&g_wHung, 0),
                 su = InterlockedExchange64(&g_wSearchUpd, 0), own = InterlockedExchange64(&g_wOwned, 0),
                 late = InterlockedExchange64(&g_wLate, 0), chr = InterlockedExchange64(&g_wCharHits, 0),
                 drv = InterlockedExchange64(&g_wDriven, 0), blk = InterlockedExchange64(&g_wBlocked, 0),
                 ovc = InterlockedExchange64(&g_wOverrideCalls, 0), one = InterlockedExchange64(&g_wOneHand, 0),
                 two = InterlockedExchange64(&g_wTwoHand, 0), rel = InterlockedExchange64(&g_wReleased, 0),
                 lref = InterlockedExchange64(&g_wLookRefused, 0), man = InterlockedExchange64(&g_wClimbUps, 0),
                 stk = InterlockedExchange64(&g_wStickRefused, 0), land = InterlockedExchange64(&g_wLanded, 0),
                 lau = InterlockedExchange64(&g_wLaunches, 0), plain = InterlockedExchange64(&g_wPlainDrops, 0),
                 gmr = InterlockedExchange64(&g_wGameMoveRefused, 0), rtg = InterlockedExchange64(&g_wRetargets, 0),
                 rtgf = InterlockedExchange64(&g_wRetargetFails, 0), rtgn = InterlockedExchange64(&g_wRetargetNotHeld, 0),
                 vsc = InterlockedExchange64(&g_wVsCalls, 0), car = InterlockedExchange64(&g_wCarryOn, 0),
                 ast = InterlockedExchange64(&g_wAirStarts, 0), snp = InterlockedExchange64(&g_wCamSnaps, 0);
    const float vsMax = g_wVsMaxCm, snpMax = g_wCamSnapMaxCm;   // (written on the game thread, as this runs)
    g_wVsMaxCm = 0.0f;
    g_wCamSnapMaxCm = 0.0f;
    const float maxSent = g_wMaxSent, maxMeas = g_wMaxMeas;   // (game-thread floats, read on the game thread: the window runs there)
    g_wMaxSent = g_wMaxMeas = 0.0f;
    LONG64 w[climbhand::kWhyCount];
    for (int i = 0; i < climbhand::kWhyCount; ++i) { w[i] = InterlockedExchange64(&g_wWhy[i], 0); }
    if (st == 0 && dr == 0 && su == 0 && own == 0 && late == 0 && ref == 0 && flt == 0 && low == 0 && ovc == 0 && car == 0 &&
        vsc == 0 && ast == 0) {
        return;   // quiet
    }
    // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
    LOG("[CLIMBHAND] 5 s | updates %lld: game's start skipped %lld, passed for a low wall %lld, passed (climb running) %lld | "
        "searching %lld update(s), %lld rays: found %lld, no face %lld, no top %lld, too far %lld, not flat %lld, no direction %lld, "
        "back face only %lld, blocked from the body %lld, no room above %lld; ray hits on characters skipped %lld | "
        "grabs %lld (forced to hang %lld), drops %lld, refused %lld | presses another feature kept %lld, released to the climb %lld | "
        "faults %lld | phase %d | P2 hand motion %d: walk calls while climbing %lld, driven by the hands %lld (largest speed sent %.2f, "
        "measured %.2f m/s), body held back by something solid %lld | pad polls driven by one hand %lld, by both (averaged) %lld | "
        "hands released for distance %lld | the hang's camera-turning look states refused %lld | P3: climb-ups asked by the pull "
        "height %lld, the game's own climb-ups refused (no stick / climb_mantle_stick off) %lld, launches %lld, air time of the "
        "game's own carried %lld (climb_air_momentum_all) | walk calls carrying momentum %lld, carries ended by landing %lld | plain "
        "drops %lld | P4: the game's own hang moves refused %lld (stick "
        "rules, or re-targeted), re-targeted onto the held ledge %lld (refused %lld, did not hold %lld) | view smoothed on %lld "
        "camera call(s), largest offset from the eye bone %.1f cm (climb_view_smooth) | camera snaps against the body %lld "
        "(largest %.1f cm; CAM SNAP lines)",
        upd, skip, low, pass, su, casts, w[0], w[2], w[3], w[4], w[5], w[1], w[climbhand::kBackFace], w[climbhand::kBlocked],
        w[climbhand::kNoRoom], chr, st, hung, dr, ref, own, late, flt,
        static_cast<int>(g_m.phase), ClimbHandMotion ? 1 : 0, ovc, drv, maxSent, maxMeas, blk, one, two, rel, lref, man, stk, lau,
        ast, car, land, plain, gmr, rtg, rtgf, rtgn, vsc, vsMax, snp, snpMax);
}

}  // namespace

void ClimbHandTick()
{
    if (g_live == 1 && ClimbByHand) { Window(GetTickCount64()); }
    if (g_installed || !ClimbByHand) { return; }
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !en) { return; }
    InterlockedExchange(&g_installed, 1);   // latch before the attempt
    g_gd = reinterpret_cast<uintptr_t>(gd);
    const uintptr_t setGroup = g_gd + DLVR_CH_GD(kSetGroupRva), setState = g_gd + DLVR_CH_GD(kSetStateRva),
                    setTarget = g_gd + DLVR_CH_GD(kSetTargetRva);
    g_typeRet = g_gd + DLVR_CH_GD(kTypeRetRva);
    g_stateRet = g_gd + DLVR_CH_GD(kStateRetRva);
    {   // P2 round 3: +D0AD16 must be "call +D0CD20" (E8 rel32) for its return to name the hang's look-state call
        const uintptr_t site = g_gd + DLVR_CH_GD(kLookCallRva);
        unsigned char op = 0;
        int rel = 0;
        const bool callOk = RdB(site, &op) && op == 0xE8 && RdI(site + 1, &rel) &&
                            site + 5 + static_cast<intptr_t>(rel) == g_gd + DLVR_CH_GD(kSetStateRva);
        g_lookRet = callOk ? site + 5 : 0;
        // [PROBE: CLIMBHAND] gate=climb_by_hand (install)  site=FLAT  log-only=no
        LOG("[CLIMBHAND] the hang's look-state call gamedll+%llX %s", static_cast<unsigned long long>(kLookCallRva),
            callOk ? "is 'call set-state' - its look-left / look-right (camera turn) is refused by hand"
                   : "*** is NOT 'call set-state' - the hang's look states stay the game's (the camera may turn) ***");
    }
    const bool groupOk = BytesMatch(setGroup, kSetGroupBytes, sizeof(kSetGroupBytes));
    const bool stateOk = BytesMatch(setState, kSetStateBytes, sizeof(kSetStateBytes));
    const bool targetOk = BytesMatch(setTarget, kSetTargetBytes, sizeof(kSetTargetBytes));
    g_ray = reinterpret_cast<RayFn>(GetProcAddress(en, kRayExport));
    g_pos = reinterpret_cast<PosFn>(GetProcAddress(en, kPosExport));
    g_en = reinterpret_cast<uintptr_t>(en);   // P3's launch velocity after the drop (ApplyBodyVel); missing = the launch keeps no
    g_bindFn = reinterpret_cast<BindFn>(GetProcAddress(en, kBindExport));        // sideways speed, logged per launch
    g_setVel = reinterpret_cast<SetVelFn>(GetProcAddress(en, kSetVelExport));
    // [PROBE: CLIMBHAND] gate=climb_by_hand (install)  site=FLAT  log-only=no
    LOG("[CLIMBHAND] P3 climb-up: the stick %s, by pull height %s (%.2f m) - both the game's own | low walls the game's own %s | "
        "launch from %.2f m/s x %.2f boost, up assist %.2f (caps %.2f up / %.2f sideways = the game's default jump) | PhysicsBind %s, "
        "IPhBody::SetLinearVel %s", ClimbMantleStick ? "on" : "OFF", ClimbMantleAuto ? "ON" : "off", ClimbMantleM,
        ClimbAutoLow ? "ON" : "off", ClimbLaunchMinMps, ClimbLaunchScale, ClimbLaunchUpAssist, climbhand::kLaunchMaxUpMps,
        climbhand::kLaunchMaxSideMps, g_bindFn ? "resolved" : "MISSING", g_setVel ? "resolved" : "MISSING");
    bool ok = groupOk && stateOk && targetOk && g_ray && g_pos;
    int attempts = 0, typeAttempts = 0, stateAttempts = 0;
    if (ok) {
        g_setGroup = reinterpret_cast<SetGroupFn>(setGroup);
        g_setState = reinterpret_cast<SetStateFn>(setState);
        g_setTarget = reinterpret_cast<SetTargetFn>(setTarget);
        // InstallSingleGameDetour translates the address for the loaded build itself (buildrva::Abs) - hand it the Steam one.
        ok = InstallSingleGameDetour(g_gd + kTryStartRva, kTryStartBytes, sizeof(kTryStartBytes), reinterpret_cast<void*>(&Hook),
                                     reinterpret_cast<void**>(&g_true), "climbhand", &attempts);
    }
    bool typeOk = false;
    if (ok) {
        typeOk = InstallSingleGameDetour(g_gd + kTypeRva, kTypeBytes, sizeof(kTypeBytes), reinterpret_cast<void*>(&TypeHook),
                                         reinterpret_cast<void**>(&g_trueType), "climbhand-type", &typeAttempts);
    }
    // The hang decision (round 3): the set-state detour. Our own drop call (g_setState) goes through it too and passes straight on.
    bool stateHookOk = false;
    if (ok) {
        stateHookOk = InstallSingleGameDetour(g_gd + kSetStateRva, kSetStateBytes, sizeof(kSetStateBytes),
                                              reinterpret_cast<void*>(&StateHook), reinterpret_cast<void**>(&g_trueState),
                                              "climbhand-hang", &stateAttempts);
    }
    InterlockedExchange(&g_stateLive, stateHookOk ? 1 : -1);
    InterlockedExchange(&g_typeLive, typeOk ? 1 : -1);
    InterlockedExchange(&g_live, ok ? 1 : -1);
    // [PROBE: CLIMBHAND] gate=climb_by_hand (install)  site=FLAT  log-only=no
    LOG("[CLIMBHAND] %s - group setter %s, set-state %s, set-target %s, Raytrace %s, GetWorldPosition %s | start detour gamedll+%llX %s "
        "(%d attempt(s)) | type detour gamedll+%llX %s (%d) | hang detour gamedll+%llX %s (%d) | reach %.2f m, "
        "catch %d ms, always hang %d from %.2f m, low walls the game's own %s up to %.2f m%s",
        ok ? "LIVE" : "*** NOT LIVE ***", groupOk ? "bytes OK" : "BYTES DIFFER", stateOk ? "bytes OK" : "BYTES DIFFER",
        targetOk ? "bytes OK" : "BYTES DIFFER", g_ray ? "resolved" : "MISSING", g_pos ? "resolved" : "MISSING",
        static_cast<unsigned long long>(kTryStartRva), ok ? "committed" : "not committed", attempts,
        static_cast<unsigned long long>(kTypeRva), typeOk ? "committed" : "NOT LIVE", typeAttempts,
        static_cast<unsigned long long>(kSetStateRva), stateHookOk ? "committed" : "*** NOT LIVE - climb_always_hang does nothing ***",
        stateAttempts, ClimbReachM, ClimbCatchMs, ClimbAlwaysHang ? 1 : 0, ClimbHangMinM, ClimbAutoLow ? "ON" : "off", ClimbAutoLowM,
        ok ? " - grips find the ledge at the hand; the game starts no ledge grab of its own (but low walls when that is on)"
           : " - climbing stays the game's own this session (look + hold jump)");
}

bool ClimbHandPinShift(int hand, float out[3])
{
    if (!ClimbPinHands || !ClimbByHand || hand < 0 || hand > 1) { return false; }
    bool on;
    ULONGLONG at;
    float maxM;
    AcquireSRWLockShared(&g_lock);
    on = g_pinOn[hand];
    for (int i = 0; i < 3; ++i) { out[i] = g_pinShift[hand][i]; }
    at = g_holdPubMs;
    maxM = g_pinMaxM;
    ReleaseSRWLockShared(&g_lock);
    if (!on || GetTickCount64() - at > 100) { return false; }   // not holding / the pad thread stopped publishing
    const float d = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    return std::isfinite(d) && d <= maxM;
}

bool ClimbHandHanging()
{
    if (!ClimbByHand || g_live != 1 || !g_egOk) { return false; }
    unsigned char act = 0;
    return RdB(g_egPtr + kActiveOff, &act) && act != 0;
}

float ClimbViewRigidK()
{
    static float s_k = 0.0f;
    static LARGE_INTEGER s_q = {};
    static volatile LONG s_said = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
    const float dt = s_q.QuadPart ? static_cast<float>(static_cast<double>(q.QuadPart - s_q.QuadPart) / g_qpf.QuadPart) : 0.0f;
    s_q = q;
    const bool active = ClimbViewRigid && ClimbHandHanging();
    s_k = climbhand::RigidRampStep(s_k, active, dt);
    if (active && InterlockedCompareExchange(&s_said, 1, 0) == 0 && Line()) {
        // [PROBE: CLIMBHAND] gate=climb_view_rigid  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] RIGID VIEW first used (logged once) - while climbing the camera keeps the standing eye point on the body "
            "(eased in / out over %.0f ms); the climb animations no longer move the view", climbhand::kRigidRampS * 1000.0f);
    }
    return s_k;
}

bool ClimbHandFreezesBody()
{
    // P4 round 2 / 5: with climb_body_follows_view on, the body keeps following the head while hanging - no pause (the view's
    // steadiness is climb_view_smooth's).
    if (!ClimbByHand || g_live != 1 || !g_egOk || ClimbBodyFollowsView) { return false; }
    unsigned char act = 0;
    const bool on = RdB(g_egPtr + kActiveOff, &act) && act != 0;
    if (on && InterlockedCompareExchange(&g_saidFreeze, 1, 0) == 0 && Line()) {
        // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] FIRST ledge grab with body-follows-head paused (logged once) - the hanging body no longer turns with the head; "
            "it folds back in when the climb ends");
    }
    return on;
}

// P4 round 5 (climb_view_smooth): boneblend's camera-base call (EyePointApply, the player's camera, game thread, up to twice per
// update) asks what to add to the view: the eye bone's offset from the body, low-passed while the ledge grab runs in hand mode
// (ViewSmoothStep) and eased back after. eyeW = the eye bone (world), originW = the body's origin (world). False = nothing to add.
// P4 round 8 camera trace (climbhand.h ClimbCamSample). Game thread.
constexpr float     kCamSnapM    = 0.015f;   // a camera move against the body's origin this large in one call is a SNAP line
constexpr ULONGLONG kCamEveryMs  = 250;
constexpr long      kMaxCamLines = 500;
ClimbCamSample g_camPrev = {};
bool           g_camHave = false;
ULONGLONG      g_camPrevMs = 0, g_camEveryMs = 0;
volatile LONG  g_camLines = 0;
static float CamYawDeg(const float f[3]) { return std::atan2(f[0], f[2]) * 57.29578f; }
static float CamWrap(float d) { while (d > 180.0f) { d -= 360.0f; } while (d < -180.0f) { d += 360.0f; } return d; }
static float CamLenCm(const float a[3], const float b[3])
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return 100.0f * std::sqrt(x * x + y * y + z * z);
}
void ClimbHandCamTrace(const ClimbCamSample& s)
{
    unsigned char act = 0;
    const bool active = ClimbByHand && g_live == 1 && g_egOk && RdB(g_egPtr + kActiveOff, &act) && act != 0;
    if (!active) { g_camHave = false; return; }
    const ULONGLONG now = GetTickCount64();
    float rel[3], eyeRel[3];
    for (int i = 0; i < 3; ++i) { rel[i] = s.camW[i] - s.originW[i]; eyeRel[i] = s.eyeW[i] - s.originW[i]; }
    const float bodyYaw = CamYawDeg(s.bodyFwd);
    if (g_camHave && now - g_camPrevMs <= 200) {
        float prel[3], peye[3];
        for (int i = 0; i < 3; ++i) { prel[i] = g_camPrev.camW[i] - g_camPrev.originW[i]; peye[i] = g_camPrev.eyeW[i] - g_camPrev.originW[i]; }
        const float dCm = CamLenCm(rel, prel);
        if (dCm > kCamSnapM * 100.0f) {
            InterlockedIncrement64(&g_wCamSnaps);
            if (dCm > g_wCamSnapMaxCm) { g_wCamSnapMaxCm = dCm; }
            if (InterlockedIncrement(&g_camLines) <= kMaxCamLines) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=yes
                LOG("[CLIMBHAND] CAM SNAP %.1f cm in %llu ms (camera - body origin moved (%.1f, %.1f, %.1f) cm) | body yaw %+.1f deg "
                    "(now %.1f), headset yaw %+.1f deg | what moved, cm: game's base (body-local) %.1f, eye bone from the origin %.1f, "
                    "face offset d1 %.1f, headset arc d2 %.1f, view smooth vs %.1f, hand-twist hold %.1f | body origin moved %.1f",
                    dCm, static_cast<unsigned long long>(now - g_camPrevMs), 100.0f * (rel[0] - prel[0]), 100.0f * (rel[1] - prel[1]),
                    100.0f * (rel[2] - prel[2]), CamWrap(bodyYaw - CamYawDeg(g_camPrev.bodyFwd)), bodyYaw,
                    CamWrap(s.headYawDeg - g_camPrev.headYawDeg), CamLenCm(s.baseL, g_camPrev.baseL), CamLenCm(eyeRel, peye),
                    CamLenCm(s.d1, g_camPrev.d1), CamLenCm(s.d2, g_camPrev.d2), CamLenCm(s.vs, g_camPrev.vs),
                    CamLenCm(s.htvW, g_camPrev.htvW), CamLenCm(s.originW, g_camPrev.originW));
            }
        }
    }
    if (now - g_camEveryMs >= kCamEveryMs && InterlockedIncrement(&g_camLines) <= kMaxCamLines) {
        g_camEveryMs = now;
        // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=yes
        LOG("[CLIMBHAND] CAM body yaw %.1f, headset yaw %.1f deg | camera - body origin (%.1f, %.1f, %.1f) cm | game's base (body-local) "
            "(%.1f, %.1f, %.1f) | eye bone - origin (%.1f, %.1f, %.1f) | d1 (%.1f, %.1f, %.1f) d2 (%.1f, %.1f, %.1f) vs (%.1f, %.1f, "
            "%.1f) hand-twist (%.1f, %.1f, %.1f) cm", bodyYaw, s.headYawDeg, 100.0f * rel[0], 100.0f * rel[1], 100.0f * rel[2],
            100.0f * s.baseL[0], 100.0f * s.baseL[1], 100.0f * s.baseL[2], 100.0f * eyeRel[0], 100.0f * eyeRel[1], 100.0f * eyeRel[2],
            100.0f * s.d1[0], 100.0f * s.d1[1], 100.0f * s.d1[2], 100.0f * s.d2[0], 100.0f * s.d2[1], 100.0f * s.d2[2],
            100.0f * s.vs[0], 100.0f * s.vs[1], 100.0f * s.vs[2], 100.0f * s.htvW[0], 100.0f * s.htvW[1], 100.0f * s.htvW[2]);
    }
    g_camPrev = s;
    g_camPrevMs = now;
    g_camHave = true;
}

bool ClimbHandViewCorrection(const float eyeW[3], const float originW[3], float corrW[3])
{
    corrW[0] = corrW[1] = corrW[2] = 0.0f;
    unsigned char act = 0;
    const bool active = ClimbViewSmooth && ClimbByHand && g_live == 1 && g_egOk && RdB(g_egPtr + kActiveOff, &act) && act != 0;
    if (!active && !g_vs.on) { g_vsQpc.QuadPart = 0; return false; }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
    const float dt = g_vsQpc.QuadPart ? static_cast<float>(static_cast<double>(q.QuadPart - g_vsQpc.QuadPart) / g_qpf.QuadPart) : 0.0f;
    g_vsQpc = q;
    const bool wasOn = g_vs.on;
    const float raw[3] = { eyeW[0] - originW[0], eyeW[1] - originW[1], eyeW[2] - originW[2] };
    climbhand::ViewSmoothStep(g_vs, active, raw, dt, corrW);
    if (!wasOn && g_vs.on && InterlockedCompareExchange(&g_saidVs, 1, 0) == 0 && Line()) {
        // [PROBE: CLIMBHAND] gate=climb_view_smooth  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] VIEW SMOOTH first used (logged once) - while climbing the view follows the eye bone's offset from the body "
            "with a %.0f ms time constant, never more than %.0f cm from it; the eye bone starts (%.2f, %.2f, %.2f) m from the body's "
            "origin", climbhand::kViewSmoothTauS * 1000.0f, climbhand::kViewSmoothMaxM * 100.0f, raw[0], raw[1], raw[2]);
    }
    const float l = std::sqrt(corrW[0] * corrW[0] + corrW[1] * corrW[1] + corrW[2] * corrW[2]);
    if (l <= 0.0f) { return false; }
    InterlockedIncrement64(&g_wVsCalls);
    if (l * 100.0f > g_wVsMaxCm) { g_wVsMaxCm = l * 100.0f; }
    return true;
}

// P4 round 5: the launch's CARRY - the player's own walk / fall after the launch's drop. The walk hands the body its wanted vector
// every update (the stick's move), which overwrites any one-off velocity's horizontal part (run 10); here the launch's horizontal
// speed is added in until the body lands (CarryStep). The ledge grab is not running. Game thread. One measured line per launch: the
// feet's horizontal speed over the first ~300 ms against the launch's (the carry works when they match).
// P4 round 8: the end is the walk's own airborne flag (air), the stick's push the game's air factor (round 9: its data value; the
// walk's live one read 1.0); the first
// kTraceLaunches launches and kTraceAirs air carries are traced call by call (CARRY TRACE: the walk's flags, the feet's vertical
// against the launch's ballistic one, the horizontal handed and measured) - run 15 needed exactly that to see where the rise went.
// A launch's vertical speed (g_vertSet) is cleared when its carry ends.
static void CarryEnded()
{
    if (!g_vertSet) { return; }
    g_vertSet = false;
    const char* why = "";
    const uintptr_t wi = g_curPlayer ? WalkIface(g_curPlayer, &why) : 0;
    WalkVertical(wi, false, 0.0f);
}
static bool CarryOverride(uintptr_t player, const float in[3], float out[3], uintptr_t wi, int air)
{
    float feet[3];
    if (!Feet(player, feet)) { return false; }
    const ULONGLONG now = GetTickCount64();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
    // the REAL time since the last walk call (CarryStep measures the body's velocity over it; a long hitch is no sample there)
    float dt = g_motionQpc.QuadPart ? static_cast<float>(static_cast<double>(q.QuadPart - g_motionQpc.QuadPart) / g_qpf.QuadPart) : 0.0f;
    if (dt < 0.0f) { dt = 0.0f; }
    g_motionQpc = q;
    const ULONGLONG t0 = g_carry.t0;
    const float prevFeet[3] = { g_carry.prev[0], g_carry.prev[1], g_carry.prev[2] };
    const bool hadPrev = g_carry.havePrev;
    // round 9: the push uses the game's data value (kGameAirK); the walk's live factor read 1.0 in run 16 and is only logged
    const float liveK = WalkAirK(wi);
    const int end = climbhand::CarryStep(g_carry, feet, dt, now, in, ClimbAirMomentum, ClimbAirDrag, out, ClimbAirControl,
                                         climbhand::kGameAirK, air);
    if (g_traceOn && now - t0 <= (g_carryAir ? kTraceAirMs : kTraceLaunchMs) && InterlockedIncrement(&g_traceLines) <= kMaxTraceLines) {
        float mv[3] = { NAN, NAN, NAN };
        if (hadPrev && dt > 1e-4f) { for (int i = 0; i < 3; ++i) { mv[i] = (feet[i] - prevFeet[i]) / dt; } }
        const float ball = g_carry.v[1] - climbhand::kGravity * static_cast<float>(now - t0) / 1000.0f;
        // [PROBE: CLIMBHAND] gate=climb_air_momentum  site=HEADSET  log-only=yes
        LOG("[CLIMBHAND] CARRY TRACE %s t+%llu | walk flags 0x%04X (air %d) | feet vy %.2f (ballistic %.2f) | horizontal handed "
            "(%.2f, %.2f) measured (%.2f, %.2f) stick (%.2f, %.2f) | air push factor %.4f at climb_air_control %.2f (the walk's own "
            "factor^3 reads %.4f, unused) | %s", g_carryAir ? "AIR" : "LAUNCH",
            static_cast<unsigned long long>(now - t0), WalkFlags(wi), air, mv[1], ball, out[0], out[2], mv[0], mv[2], in[0], in[2],
            climbhand::kGameAirK, ClimbAirControl, liveK,
            end == climbhand::kCeNone ? "carrying" : (end == climbhand::kCeLanded ? "LANDED" : "timed out"));
    }
    if (g_carryFeet0Ms == 0) {
        for (int i = 0; i < 3; ++i) { g_carryFeet0[i] = feet[i]; }
        g_carryFeet0Ms = now;
    } else if (!g_carrySaid && (now - g_carryFeet0Ms >= 300 || end != climbhand::kCeNone)) {
        g_carrySaid = true;
        const float s = static_cast<float>(now - g_carryFeet0Ms) / 1000.0f;
        if (s > 0.02f && Line()) {
            const float mx = (feet[0] - g_carryFeet0[0]) / s, mz = (feet[2] - g_carryFeet0[2]) / s;
            // [PROBE: CLIMBHAND] gate=climb_launch_min_mps  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] LAUNCH CARRY - horizontal: launched (%.2f, %.2f) |%.2f| m/s, the feet moved (%.2f, %.2f) |%.2f| m/s over "
                "%.0f ms; vertical %.2f m/s at the launch, the feet %.2f m/s (horizontal near the launch's = the carry works)",
                g_carry.v[0], g_carry.v[2], std::sqrt(g_carry.v[0] * g_carry.v[0] + g_carry.v[2] * g_carry.v[2]), mx, mz,
                std::sqrt(mx * mx + mz * mz), s * 1000.0f, g_carry.v[1], (feet[1] - g_carryFeet0[1]) / s);
        }
    }
    if (end != climbhand::kCeNone) {
        if (end == climbhand::kCeLanded) { InterlockedIncrement64(&g_wLanded); }
        CarryEnded();
        if ((!g_carryAir || g_airLines <= 10) && Line()) {
            // [PROBE: CLIMBHAND] gate=climb_air_momentum  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] %s CARRY ended - %s after %llu ms | feet (%.2f, %.2f, %.2f) | horizontal momentum then %.2f m/s "
                "(at the start %.2f; less = walls / drag%s)", g_carryAir ? "AIR" : "LAUNCH",
                end == climbhand::kCeLanded ? (g_carry.sawAir ? "LANDED (the walk says it is down; the stick has the speed again)"
                                                              : "LANDED (seen from the feet; the stick has the speed again)")
                                            : "safety time out (no landing seen)",
                static_cast<unsigned long long>(now - t0), feet[0], feet[1], feet[2],
                std::sqrt(g_carry.h[0] * g_carry.h[0] + g_carry.h[1] * g_carry.h[1]),
                std::sqrt(g_carry.v[0] * g_carry.v[0] + g_carry.v[2] * g_carry.v[2]), ClimbAirMomentum ? "" : " / the stick");
        }
        return false;
    }
    InterlockedIncrement64(&g_wCarryOn);
    return true;
}

bool ClimbHandWalkOverride(uintptr_t player, const float in[3], float out[3], bool moveCtl)
{
    if (!ClimbByHand || g_live != 1 || !g_egOk) {
        if (g_carry.on) { CarryEnded(); }
        g_carry.on = false;
        climbhand::FeetVelReset(g_fv);
        return false;
    }
    unsigned char act = 0;
    int state = -1;
    if (!RdB(g_egPtr + kActiveOff, &act) || !act || !RdI(g_egPtr + kEgStateOff, &state)) {
        // not climbing: a launch's carry, or (climb_air_momentum_all) any air time of the game's own
        if (player != LocalPlayer()) { return false; }
        if (!moveCtl) {   // a ladder, rope, zipline, vehicle ...: not ours
            if (g_carry.on && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_air_momentum  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] %s CARRY ended - another controller took the player %llu ms in", g_carryAir ? "AIR" : "LAUNCH",
                    static_cast<unsigned long long>(GetTickCount64() - g_carry.t0));
            }
            if (g_carry.on) { CarryEnded(); }
            g_carry.on = false;
            climbhand::FeetVelReset(g_fv);
            return false;
        }
        // P4 round 8: the feet's velocity every call (an air carry starts with it) and the walk's own airborne flag
        float feet[3];
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
        // times from a base: small floats, full precision (rebased after a pause, or every ~15 min of play)
        if (!g_fv.have || q.QuadPart - g_airBaseQpc.QuadPart > g_qpf.QuadPart * 1000) {
            climbhand::FeetVelReset(g_fv);
            g_airBaseQpc = q;
        }
        const float tS = static_cast<float>(static_cast<double>(q.QuadPart - g_airBaseQpc.QuadPart) / g_qpf.QuadPart);
        if (Feet(player, feet)) { climbhand::FeetVelStep(g_fv, feet, tS); }
        const char* why = "";
        const uintptr_t wi = WalkIface(player, &why);
        const int air = WalkAirborne(wi);
        if (!g_carry.on) {
            if (!ClimbAirMomentum || !ClimbAirMomentumAll || air != 1) { return false; }
            float v[3] = {};
            if (g_fv.haveV) { for (int i = 0; i < 3; ++i) { v[i] = g_fv.v[i]; } }
            const ULONGLONG now = GetTickCount64();
            climbhand::StartCarry(g_carry, v, now, 0);   // the walk already holds the body: no arming wait
            g_carryAir = true;
            g_carryFeet0Ms = 0;
            g_carrySaid = true;   // (the measured line is a launch's)
            g_motionQpc = q;
            // round 9: a change of the air settings traces the next ones again (run 16's four traced carries were all at control 0)
            static float s_tracedCtl = -1.0f, s_tracedDrag = -1.0f;
            if (ClimbAirControl != s_tracedCtl || ClimbAirDrag != s_tracedDrag) {
                s_tracedCtl = ClimbAirControl; s_tracedDrag = ClimbAirDrag;
                g_traceAirs = 0;
                g_traceLaunches = 0;
            }
            g_traceOn = g_traceAirs < kTraceAirs;
            if (g_traceOn) { ++g_traceAirs; }
            InterlockedIncrement64(&g_wAirStarts);
            if (InterlockedIncrement(&g_airLines) <= 10 && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_air_momentum_all  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] AIR CARRY started (the walk says it is in the air, flags 0x%04X; the first 10 are logged, all are "
                    "counted) - velocity %s(%.2f, %.2f, %.2f) m/s: the body keeps its horizontal momentum until it lands, the stick "
                    "pushes at climb_air_control %.2f of the game's own", WalkFlags(wi), g_fv.haveV ? "" : "NOT MEASURED YET ", v[0],
                    v[1], v[2], ClimbAirControl);
            }
        }
        return CarryOverride(player, in, out, wi, air);
    }
    climbhand::FeetVelReset(g_fv);
    if (g_carry.on && Line()) {
        // [PROBE: CLIMBHAND] gate=climb_air_momentum  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] %s CARRY ended - a ledge grab started (state %d) %llu ms in", g_carryAir ? "AIR" : "LAUNCH", state,
            static_cast<unsigned long long>(GetTickCount64() - g_carry.t0));
    }
    if (g_carry.on) { CarryEnded(); }
    g_carry.on = false;   // a grab (a catch mid-air, or anything else) ends the carry
    if (!ClimbHandMotion) { return false; }
    InterlockedIncrement64(&g_wOverrideCalls);
    if (player != LocalPlayer()) { return false; }
    if (!climbhand::IsHoldState(state)) {   // the game's own shimmy / climb / jump took over
        g_motion.ringN = 0;
        g_climbUpAsked = false;   // a new hold (a shimmy's end, a new grab) may climb up again
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    climbhand::Hold h;
    ULONGLONG at;
    AcquireSRWLockShared(&g_lock);
    h = g_hold;
    at = g_holdPubMs;
    ReleaseSRWLockShared(&g_lock);
    if (now - at > 100) { h.drive = false; }   // the pad thread stopped publishing
    float feet[3];
    if (!Feet(player, feet)) { return false; }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
    g_motionQpc = q;
    const climbhand::MotionOut mo = climbhand::MotionStep(g_motion, h, feet, now, ClimbMantleAuto ? ClimbMantleM : 0.0f,
                                                          ClimbPullMaxMps, ClimbLaunchMinMps > 0.0f ? ClimbLaunchMinMps : 1.5f);
    if (mo.climbUp && !g_climbUpAsked) {   // once per hang: the Hook asks the game for its own climb-up (set-state 8)
        g_climbUpAsked = true;
        g_wantClimbUp = true;
        g_climbUpGap = h.topY - feet[1];
    }
    if (!mo.send) { g_lastDriveQpc.QuadPart = 0; return false; }   // (the next driven call starts a new measurement)
    for (int i = 0; i < 3; ++i) { out[i] = mo.v[i]; }   // in a hold state the game's own vector is zero
    // measured vs sent, from the feet between two driven calls (the 5 s line; "blocked" = the physics refused most of it)
    {
        const double dt = g_lastDriveQpc.QuadPart ? static_cast<double>(q.QuadPart - g_lastDriveQpc.QuadPart) / g_qpf.QuadPart : 0.0;
        if (dt > 0.001 && dt < 0.1) {
            const float mx = static_cast<float>((feet[0] - g_lastFeet[0]) / dt), my = static_cast<float>((feet[1] - g_lastFeet[1]) / dt),
                        mz = static_cast<float>((feet[2] - g_lastFeet[2]) / dt);
            const float meas = std::sqrt(mx * mx + my * my + mz * mz);
            const float sent = std::sqrt(g_lastSent[0] * g_lastSent[0] + g_lastSent[1] * g_lastSent[1] + g_lastSent[2] * g_lastSent[2]);
            if (meas > g_wMaxMeas) { g_wMaxMeas = meas; }
            if (sent >= 0.5f && meas < 0.3f * sent) { InterlockedIncrement64(&g_wBlocked); }
        }
        g_lastFeet[0] = feet[0]; g_lastFeet[1] = feet[1]; g_lastFeet[2] = feet[2];
        g_lastDriveQpc = q;
    }
    g_lastSent[0] = out[0]; g_lastSent[1] = out[1]; g_lastSent[2] = out[2];
    const float s = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (s > g_wMaxSent) { g_wMaxSent = s; }
    InterlockedIncrement64(&g_wDriven);
    return true;
}

void ClimbHandPoll(bool inactive, const char* ownerL, const char* ownerR, bool& withholdL1, bool& withholdR1)
{
    withholdL1 = withholdR1 = false;
    if (!ClimbByHand && g_m.phase == climbhand::kIdle) { return; }
    climbhand::In in;
    in.enabled = ClimbByHand && !inactive && g_live == 1;
    in.owned[0] = ownerL != nullptr;
    in.owned[1] = ownerR != nullptr;
    const char* owner[2] = { ownerL, ownerR };
    in.catchMs = ClimbCatchMs > 0 ? static_cast<unsigned long long>(ClimbCatchMs) : 0ull;
    in.nowMs = GetTickCount64();
    float hand[2][3] = {};
    // The owner named at the press edge, kept for the outcome line (by then the owner may have let go - its name is null).
    static const char* s_edgeOwner[2] = {};
    for (int h = 0; h < 2; ++h) {
        float sq = 0.0f;
        in.grip[h] = GetControllerSqueeze(h == 0 ? kLeftHand : kRightHand, sq) && sq >= 0.5f;   // the grip block's threshold
        in.handOk[h] = LootHandWorldPos(h, hand[h]);
        if (in.grip[h] && !g_m.prevGrip[h] && in.owned[h]) { s_edgeOwner[h] = owner[h]; }
    }
    {   // P3 round 3: the left stick pushed forward is what lets the game's own climb-up from a hang through (StateHook)
        float sx = 0.0f, sy = 0.0f;
        if (GetControllerStick(kLeftHand, sx, sy) && sy >= climbhand::kStickForward) { g_stickFwdMs = in.nowMs; }
    }
    AcquireSRWLockShared(&g_lock);
    in.game = g_pub;
    const ULONGLONG pubMs = g_pubMs;
    ReleaseSRWLockShared(&g_lock);
    in.game.ageMs = pubMs && in.nowMs >= pubMs ? in.nowMs - pubMs : ~0ull;
    float foundLip[2][3], foundInto[2][3];
    AcquireSRWLockShared(&g_lock);
    in.foundMs[0] = g_foundMs[0];
    in.foundMs[1] = g_foundMs[1];
    for (int h = 0; h < 2; ++h) {
        for (int i = 0; i < 3; ++i) {
            foundLip[h][i] = g_foundLip[h][i];
            foundInto[h][i] = g_foundInto[h][i];
        }
    }
    ReleaseSRWLockShared(&g_lock);
    const climbhand::Phase was = g_m.phase;
    const unsigned heldWas = g_m.held;
    const climbhand::Out o = climbhand::Update(g_m, in);
    // Run 3: an owned press is decided up to kLateTakeMs later (loot by hand's severed-limb wait holds every press briefly).
    for (int h = 0; h < 2; ++h) {
        const char* const who = s_edgeOwner[h] ? s_edgeOwner[h] : "another feature";
        if (o.lost[h]) {
            InterlockedIncrement64(&g_wOwned);
            if (HandLine()) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] %s grip press kept by %s (still its own after %llu ms, or the grip opened first) - not a climb",
                    h == 0 ? "left" : "right", who, climbhand::kLateTakeMs);
            }
        } else if (o.lateTake[h]) {
            InterlockedIncrement64(&g_wLate);
            if (HandLine()) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] %s grip press held by %s for %llu ms, then let go - the climb takes it", h == 0 ? "left" : "right",
                    who, o.lateMs[h]);
            }
        }
    }
    // P2: the anchored hands -> where the body must go (published for ClimbHandWalkOverride on the game thread)
    {
        for (int h = 0; h < 2; ++h) {   // the ledge point each holding hand found (round 2: the hand's anchor)
            if (o.searchEnded[h] == climbhand::kSeFound) {
                g_lastGrabHand = h;
                for (int i = 0; i < 3; ++i) {
                    g_holdLip[h][i] = foundLip[h][i];
                    g_holdInto[h][i] = foundInto[h][i];
                }
                g_haveHoldLip[h] = true;
            } else if (!(g_m.held & (1u << h))) {
                g_haveHoldLip[h] = false;
            }
        }
        // P3 round 2: a climb that ends with grips still squeezed (the game's climb-up onto the top, a launch's drop) keeps them
        // withheld until they open, so they are not a kick / grapple the moment the climb hands back.
        if (o.event == climbhand::kEvEnded) {
            for (int h = 0; h < 2; ++h) { if (in.grip[h]) { g_relWithhold[h] = true; } }
        }
        const bool hold = in.game.ageMs <= climbhand::kGameFreshMs && in.game.egActive && climbhand::IsHoldState(in.game.egState);
        // the release distance is never under the reach + 10 cm: the hold's start snaps a hand up to climb_reach_m onto the ledge
        const float releaseM = ClimbReleaseM > ClimbReachM + 0.1f ? ClimbReleaseM : ClimbReachM + 0.1f;
        const bool hadLip[2] = { g_haveHoldLip[0], g_haveHoldLip[1] };   // (for the ANCHORED line: a release below clears it)
        const climbhand::PullOut po = climbhand::PullStep(g_pull, ClimbHandMotion ? g_m.held : 0u, hold, in.handOk, hand, g_holdLip,
                                                          g_haveHoldLip, releaseM, in.nowMs);
        if (g_pull.has[0] && g_pull.has[1] && po.drive) { InterlockedIncrement64(&g_wTwoHand); }
        else if (po.drive) { InterlockedIncrement64(&g_wOneHand); }
        for (int h = 0; h < 2; ++h) {   // too far from its ledge point: that hand lets go (a re-press grabs again)
            if (po.released[h]) {
                InterlockedIncrement64(&g_wReleased);
                g_m.held &= ~(1u << h);
                g_haveHoldLip[h] = false;
                g_relWithhold[h] = true;   // ...but its still-closed grip is not a kick / grapple: withheld until it opens
            }
            if (!in.grip[h]) { g_relWithhold[h] = false; }
        }
        bool pinOn[2] = {};   // P4: the shift that draws each anchored hand on its ledge point (the arm IK)
        float pinShift[2][3] = {};
        for (int h = 0; h < 2; ++h) {
            pinOn[h] = climbhand::PinShift(ClimbPinHands, g_pull, h, in.handOk[h], po.dist[h], releaseM, hand[h], pinShift[h]);
        }
        climbhand::Hold hp;   // the anchored hands for the game thread: the pull, and their ledge points (the mantle)
        hp.drive = po.drive;
        for (int i = 0; i < 3; ++i) { hp.err[i] = po.err[i]; }
        // "The ledge held" (the climb-up height, the stale-ledge test, the climb on the held ledge): climb_ledge_last on = the LAST
        // grabbed hand's ledge, off = the mean of both (tester 2026-11-27).
        hp.haveTop = climbhand::LedgeHeld(g_pull, g_haveHoldLip, g_holdInto, g_lastGrabHand, ClimbLedgeLast, &hp.topY, hp.mid, hp.into);
        AcquireSRWLockExclusive(&g_lock);
        hp.driveMs = po.drive ? in.nowMs : g_hold.driveMs;
        g_hold = hp;
        g_holdPubMs = in.nowMs;
        for (int h = 0; h < 2; ++h) {
            g_pinOn[h] = pinOn[h];
            for (int i = 0; i < 3; ++i) { g_pinShift[h][i] = pinShift[h][i]; }
        }
        g_pinMaxM = releaseM;
        ReleaseSRWLockExclusive(&g_lock);
        for (int h = 0; h < 2; ++h) {
            if (po.latched[h] && HandLine()) {
                // [PROBE: CLIMBHAND] gate=climb_hand_motion  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] %s hand ANCHORED on %s at (%.2f, %.2f, %.2f), %.2f m from the hand - the body moves the hand onto it "
                    "(ledge grab state %d)", h == 0 ? "left" : "right", hadLip[h] ? "its ledge point" : "ITSELF (no ledge "
                    "point - unexpected)", g_pull.anchor[h][0], g_pull.anchor[h][1], g_pull.anchor[h][2], po.dist[h], in.game.egState);
            }
            if (po.released[h] && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_hand_motion  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] %s hand RELEASED - %.2f m from its ledge point (climb_release_m %.2f, used %.2f) | still on a ledge "
                    "0x%X", h == 0 ? "left" : "right", po.dist[h], ClimbReleaseM, releaseM, g_m.held);
            }
        }
    }
    // Round 7: each searching hand is published on its own (both may search at once).
    AcquireSRWLockExclusive(&g_lock);
    for (int h = 0; h < 2; ++h) {
        g_searchOn[h] = o.search[h] && in.handOk[h];
        if (g_searchOn[h]) { for (int i = 0; i < 3; ++i) { g_searchPos[h][i] = hand[h][i]; } }
    }
    g_searchMs = in.nowMs;
    ReleaseSRWLockExclusive(&g_lock);
    // P4 round 9: 1 = let go by opening the grips (a launch may follow), 2 = a hand SLIPPED (climb_release_m) with its grip still
    // closed - a plain drop (climbhand_logic.h DropMayLaunch). Round 10: a slip while a controller moved fast in the play space (a
    // jump flick) is a let-go too.
    static climbhand::HandSpeed s_hs[2];
    static LARGE_INTEGER s_hsBase = {};
    LARGE_INTEGER hq;
    QueryPerformanceCounter(&hq);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
    if (!s_hsBase.QuadPart) { s_hsBase = hq; }
    const double hsT = static_cast<double>(hq.QuadPart - s_hsBase.QuadPart) / static_cast<double>(g_qpf.QuadPart);
    const double hsWin = static_cast<double>(climbhand::kLaunchWindowMs) / 1000.0;
    float handMps = 0.0f;
    for (int h = 0; h < 2; ++h) {
        XrPosef cp = {};
        const bool cok = GetControllerPose(h == 0 ? kLeftHand : kRightHand, cp);
        const float pp[3] = { cp.position.x, cp.position.y, cp.position.z };
        climbhand::HandSpeedStep(s_hs[h], cok, pp, hsT, hsWin);
        const float v = climbhand::HandSpeedRecent(s_hs[h], hsT, hsWin);
        if (v > handMps) { handMps = v; }
    }
    if (o.requestDrop) {
        const bool opened = climbhand::DropMayLaunch(in.grip[0], in.grip[1]);
        const bool may = climbhand::DropMayLaunch(in.grip[0], in.grip[1], handMps, ClimbLaunchMinMps);
        g_dropHandMps = handMps;
        InterlockedExchange(&g_reqDrop, opened ? 1 : (may ? 3 : 2));
    }
    for (int h = 0; h < 2; ++h) {
        const bool let = (heldWas & (1u << h)) && !(g_m.held & (1u << h));
        if (!o.searchStarted[h] && o.searchEnded[h] == 0 && !let) { continue; }
        if (!HandLine()) { break; }
        const char* const hn = h == 0 ? "left" : "right";
        if (o.searchStarted[h]) {
            // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] %s hand SEARCH (grip closed: looking for a ledge at this hand) | searching 0x%X, on a ledge 0x%X | ledge "
                "grab %s", hn, g_m.searching, g_m.held, in.game.egActive ? "active" : "idle");
        }
        static const char* const kEnd[4] = { "", "grip opened, no ledge", "catch time over, no ledge (the grip stays the game's)",
                                             "FOUND a ledge - this hand holds" };
        if (o.searchEnded[h] > 0 && o.searchEnded[h] < 4) {
            LOG("[CLIMBHAND] %s hand search ended: %s%s | on a ledge 0x%X", hn, kEnd[o.searchEnded[h]],
                o.joined[h] ? " (JOINED the running grab)" : "", g_m.held);
        }
        if (let) {
            LOG("[CLIMBHAND] %s hand let go of the ledge | still on it 0x%X", hn, g_m.held);
        }
    }
    if (o.event != climbhand::kEvNone && Line()) {
        static const char* const kNames[climbhand::kEvCount] = {
            "none", "GRAB LOST (a hand found a ledge but the grab never started)", "STARTED (the ledge grab is running)", "-",
            "DROP REQUESTED (no hand on the ledge)", "DROP FAILED (still hanging - retry later)", "ENDED (the ledge grab finished)",
            "DISABLED mid-climb (the game keeps its climb)" };
        const int e = o.event >= 0 && o.event < climbhand::kEvCount ? o.event : 0;
        // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] %s | phase %d -> %d | grips L%d R%d (owned L%d R%d), on a ledge 0x%X, searching 0x%X | ledge grab %s state "
            "%d", kNames[e], static_cast<int>(was), static_cast<int>(g_m.phase), in.grip[0] ? 1 : 0, in.grip[1] ? 1 : 0,
            in.owned[0] ? 1 : 0, in.owned[1] ? 1 : 0, g_m.held, g_m.searching, in.game.egActive ? "active" : "idle",
            in.game.egState);
    }
    withholdL1 = o.withhold[0] || (g_relWithhold[0] && in.grip[0]);
    withholdR1 = o.withhold[1] || (g_relWithhold[1] && in.grip[1]);
}

}  // namespace dlvr
