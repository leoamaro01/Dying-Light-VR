#include "grappleaim.h"

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/buildrva.h"   // the grapple / aim family's EGL rvas on the Epic build (tier 2)
#include "../common/iathook.h"
#include "../common/log.h"
#include "aimhook.h"        // ComputeAbsoluteAim - the gun's own mapping, shared on purpose
#include "boneblend.h"      // InstallSingleGameDetour - the file's prepare/commit-with-threads-suspended path
#include "camhook.h"        // CamHookGetRenderedViewMatrix - fire_reticle_3d's camera frame
#include "controllers.h"
#include "headpose.h"
#include "xhair_hide_logic.h"   // hide_game_crosshair - park vs restore when no VR use of the crosshair is active

#pragma intrinsic(_ReturnAddress)

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - diag_periodic_logs

// ===== R9 - THE GRAPPLE AIMED BY THE RIGHT CONTROLLER (SPEC-GRAPPLE-AIM-AND-LOOK-PITCH-2026-09-27) =====
//
// The grapple has ONE aim ray, sub_180D76000 (INV-267): origin sub_180D7D630, toward
// IBaseCamera::GetOnScreenPointF(W/2, H/2) of the FPP camera, range tunable, then IGSObject::Raytrace. The HUD
// reticle is drawn at the projected hit and the throw commits from the stored hit, so both follow that ray. The
// second Raytrace (+D76D7F) reuses the same origin/end (gr6). The game then adds its own targeting on top - sticky
// 0.75 m retention, a ledge assist, a reticle that shows only a VALID stored target (INV-273).
//
// PART A (direction) - an IMPORT-TABLE swap, no code patching. +D760A5 is `call cs:[IAT]` (FF 15) to
// ?GetOnScreenPointF@IBaseCamera@@QEAA?BVvec3@@MM@Z (IAT +137F118, from engine_x64_rwdi). That import has exactly
// TWO gamedll call sites (gr9): +5BE42E (fn +5BE1C0, NOT ours) and +D760A5, whose return address is +D760AB. The
// hook calls the real function, then changes the point ONLY when _ReturnAddress() == gamedll+0xD760AB:
// point = camPos + handDir * 1000 m, handDir = ComputeAbsoluteAim(original) - the gun's mapping (aimhook.cpp), so
// grapple and guns cannot disagree. +D76000 computes dir = normalise(point - origin): exact for a camera origin,
// |origin - camPos| / 1000 rad off otherwise (~0.014 deg for the 0.24 m hand origin below).
//
// PART A2 (origin, round 2) - a function-entry detour on sub_180D7D630 (the origin helper; 16 relocatable prologue
// bytes, gr11), through boneblend.cpp's prepare/commit path. It returns the HAND's game-space position ONLY when it
// returns to +D760B7 (the aim ray) or +D7DA4E (the ledge assist +D7D900, called only from +D76000, gr11); its three
// other callers (+D7908B attached rope, +D7C717, +D7D797) get the game's value. hand = camPos + R_yaw(hand_xr -
// head_xr), where R_yaw is the rotation part A just applied (derived from controller-forward -> handDir, so the
// offset uses EXACTLY the direction's mapping, incl. aim_mirror_yaw), cached per thread by the point hook that runs
// immediately before in the same +D76000 call (+D760A5 then +D760B2).
//
// PART A3 (marker, round 2) - the game's own centre crosshair (hud.xui HudCrosshair, G_Dot) becomes the hand-ray
// marker while the grapple aims. (1) IAT hook on ?Raytrace@IGSObject@@... (IAT +137B0E0, 398 refs): pass-through,
// and when it returns to +D763CC (the grapple's first cast - from the hand with A2) record the ray's end (Raytrace
// writes the hit into its `vec3 &` end) and the result. (2) HudCrosshair's primary vtable (+14E1F68) slot 178 =
// its per-frame update (+550FC0), which moves only its CHILD parts (gr12b) - swapped for ours: call the original,
// then move the GROUP by P(hit) - P(screen centre) (P = sub_180556580, the rope reticle's own projection; the
// difference cancels the pivot/owner-screen offsets; "screen centre" = the game's own GetOnScreenPointF(W/2, H/2)
// for the same call, because IBaseCamera::GetForwardVector points BEHIND the view - engine gr13, INV-273, which
// made run 2 refuse 30,132/30,132 samples), or put it back at its layout position when no grapple sample is fresh.
// Everything else keeps the dot at centre.
// PROBE (Jay, round 2 run 2): AIMED vs TARGET - the hand ray's cast vs the target the game keeps (after retention /
// ledge assist, where the reticle is drawn) and whether it is usable; one line per throw (rope state -> 3).
// PROBES (Jay 2026-09-27, end of session, INV-273 gr22-29): REFUSAL GATES - for each refused hit, re-run the level's own
// volume-list and zone checks and record the second cast's type word, naming which one refuses; PULL TRACE - a pass-through
// detour on the rope dispatcher +D74AF0 logs where the player actually goes after each throw (with / without a vault target).
//
// THREADS: +D76000 runs from the HUD reticle update and the rope controller update; Raytrace from anywhere.
// Counters are Interlocked; window sums and the shared hit sample are under small spinlocks; the per-call
// hand/mapping context is thread_local (written and read inside one +D76000 call on one thread).
//
// [LOADBEARING: R9-GRAPPLE-AIM] with grapple_hand_aim=1 this is the ONLY thing that moves the grapple ray off the
// head - INV-267: +D76000 is the grapple's one aim ray, and gr9: +D760A5 is its only GetOnScreenPointF call.

namespace dlvr {

// [DEP: grapple_hand_aim] required-for=R9 - the grapple's aim ray follows the right controller (GrappleHandAim)
//      tree=INERT. Was DEFAULT OFF. Installs one gamedll IAT pointer (GetOnScreenPointF) on first poll with it on;
//      the hook changes nothing unless the call returns to gamedll+0xD760AB. HEADSET: needs the right-controller
//      pose (a flat run only proves the install line).
bool GrappleHandAim = true;   // DEFAULT ON 2026-10-01 (Jay: Tier B ships on)
// [DEP: grapple_aim_hand] required-for=aiming the grapple with either hand (GrappleAimHand; tester 2026-11-28: "Aiming hand: changes
//      which hand aims the reticle and where the hook will launch"). 0 = left, 1 = right (controllers.h Hand). The hand-aim ray
//      (PointHook's direction through ComputeAbsoluteAim, and its origin for A2) and the aim probes read this hand. The 3D reticle sits
//      on the game's own target, which that ray makes. grapple_aim_from_gun's barrel line is unchanged. DEFAULT 1 (right).
int  GrappleAimHand = 1;
static Hand AimHand() { return GrappleAimHand == 0 ? kLeftHand : kRightHand; }
// [DEP: grapple_hand_origin] required-for=R9 round 2 A2 - the grapple ray (and the ledge assist) start at the HAND
//      (GrappleHandOrigin). tree=INERT. Was DEFAULT OFF. Detours gamedll+0xD7D630 (byte-verified 16-byte prologue);
//      substitutes only for returns to +D760B7 / +D7DA4E, and only while grapple_hand_aim redirected this call.
//      Also carries the stored-target probe. HEADSET.
bool GrappleHandOrigin = true;   // DEFAULT ON 2026-10-01 (Jay: Tier B ships on)
// [DEP: grapple_crosshair_marker] required-for=R9 round 2 A3 - the game's centre crosshair follows the hand ray's
//      hit while the grapple aims (GrappleCrosshairMarker). tree=INERT. DEFAULT OFF. One IAT pointer (Raytrace)
//      + one vtable slot (HudCrosshair slot 178). Restores the layout position whenever no grapple sample is fresh.
//      HEADSET (the hit comes from the hand ray).
bool GrappleCrosshairMarker = false;
// (grapple_prethrow_reaim - R9 option A, the throw re-aimed at release: REMOVED 2026-10-07, audit section 2.)
// [DEP: grapple_marker_hide_on_miss] required-for=R9 - the centre-crosshair marker (A3) is moved off-screen whenever the
//      grapple's cast hits NOTHING, so a visible dot always means the ray is touching a surface (Jay run 6: the dot looked
//      "on" a roof the grapple would not take; on a miss it is drawn at the ray's END, 25 m on) (GrappleMarkerHideOnMiss).
//      tree=INERT. Was DEFAULT OFF. Needs grapple_crosshair_marker=1. HEADSET.
bool GrappleMarkerHideOnMiss = true;   // DEFAULT ON 2026-10-01 (Jay: Tier B ships on; inert while grapple_crosshair_marker=0)
// [DEP: hide_game_crosshair] required-for=the game's centre crosshair kept off the screen whenever no VR use of it is active
//      (tester 2026-11-28: "doesn't really do anything at all in VR, it's just bothersome in the middle of the screen")
//      (HideGameCrosshair). tree=INERT. DEFAULT OFF. LIVE. No hook of its own: rides the HudCrosshair slot-178 hook
//      (CrosshairHook), and on every path that used to put the group back at its layout spot (no gun bore / bow, no fresh
//      grapple marker) PARKS it off-screen instead (xhair_hide_logic.h). The VR uses keep priority: fire_reticle_3d /
//      bow_reticle_3d (their 3D reticle) and grapple_crosshair_marker (the dot follows the hand ray). The grapple's own icon
//      (HudRopeReticle) is a different element and is not touched. The hook installs at startup if this OR fire_reticle_3d OR
//      grapple_crosshair_marker is on - with fire_reticle_3d on (the default) turning this on takes effect at once. HEADSET.
bool HideGameCrosshair = false;
// (fire_reticle_from_barrel - the gun reticle as the flat HUD dot: REMOVED 2026-10-07, audit section 2; fire_reticle_3d
//  replaced it.)
// [DEP: fire_reticle_3d] required-for=the gun reticle as a 3D marker in the headset, not the flat HUD dot (Jay 2026-09-28:
//      "the reticle can't be locked to the screen bounds, that's one of the main issues" -> "3D reticle it is")
//      (FireReticle3d). tree=INERT. Was DEFAULT OFF. Same cast as the removed fire_reticle_from_barrel (muzzle -> bore, grapple arguments;
//      INV-278: NOT the bullet's own query), but instead of moving the HUD group it publishes the hit / 50 m pin in the
//      RENDERED VIEW's frame (camhook's written anchor-camera matrix, CamHookGetRenderedViewMatrix; columns 0/1/2 =
//      right/up/back as for the engine camera, fov_pass3/gr13 - NOT the engine look camera, INV-272: v1 used it and the dot
//      sat low) and PARKS the HudCrosshair group off-screen while a gun's bore is fresh. Needs camhook levelling (the VR
//      camera path). overlay.cpp's
//      RenderReticle places a world-locked quad at declared-view-centre + declared-orientation * that vector - the same
//      transform the game image gets (ovr2xr.cpp projViews), so it sits on the drawn scene whatever the INV-266 view lag.
//      Installs the HudCrosshair hook on its own. NEEDS fire_aim_from_barrel=1 (the bore), overlay_enabled=1 (the quad
//      swapchain) and the overlay panel CLOSED. HEADSET.
bool FireReticle3d = true;   // DEFAULT ON 2026-10-01 (Jay: Tier B ships on)
// [DEP: fire_reticle_bullet_cast] required-for=fire_reticle_3d's ray stops where the BULLET's does (FireReticleBulletCast).
//      tree=INERT. DEFAULT ON (only acts with fire_reticle_3d=1). Calls the bullet's own world cast +715D90 (INV-278: kind 0x37,
//      a6 0x11, COFlags 8/32 toggles, the post-cast override) from the HudCrosshair update instead of the grapple-argument
//      Raytrace. Safe BY MEASUREMENT only (INV-282: 1,581/1,581 bullet casts >= 1 ms from any HUD crosshair update, FLAT);
//      keep diag_ray_threads=1 on the first headset run. 0 = the grapple-argument cast (v2 behaviour). HEADSET.
bool FireReticleBulletCast = true;
// [DEP: bow_reticle_3d] required-for=the BOW's reticle in 3D where the arrow will land, only while drawn (Jay 2026-10-08: "BOW
//      RETICLE WHILST DRAWN STATE (READY TO FIRE - HOLDING R2 CURRENTLY) following the Arrows Ray/direction/etc & in a 3D space";
//      arc + nothing when not drawn) (BowReticle3d). tree=LOADBEARING for the bow reticle. DEFAULT ON 2026-10-08 (Tier A, Jay
//      after run 21:41 with bow_fire_latched). INV-496/497. In the
//      HudCrosshair update, when no gun bore is fresh: the bow ray boneblend's PublishBowRay publishes (LEFT arrow_pose + its
//      +Z), the bow controller (owner +3120: active +16, state +32 - 1 drawing / 3 drawn) gates it. Drawn: the ARROW's ARC is
//      marched in <= 16 segments with the same cast as the gun reticle (fire_reticle_bullet_cast), speed = the game's
//      GetArrowSpeed +D9D650 for the CURRENT draw, gravity = the last fired arrow's (aimhook GetLastArrowAccel) else the
//      measured default (0, -9.8, 0) (v2: the static +1CFB510 read 0 - v1 drew a straight line until the first arrow); the
//      dot is published like the gun's with the Bow group (overlay kXhGroups[4], crosshair_bow_2 Left/Right opened by the
//      HUD's bow spread 15(1-sqrt draw)) at the LANDING point, plus (v2, Jay "2") a small dot where the shaft's straight
//      line points (one more cast). Bow held, NOT drawn: the
//      game's HUD crosshair is parked and nothing is published (Jay). COST, only while drawn: <= 16 casts + one getter per HUD
//      update ([BOWRETICLE] logs the measured cost). NEEDS fire_reticle_3d=1 (the hook + the overlay quad), overlay_enabled=1,
//      diag_bone_dump=1 (the arrow_pose read); the reticle predicts the arrow only with bow_aim_from_bow=1 (else the arrow
//      leaves from the head). HEADSET.
bool BowReticle3d = true;   // DEFAULT ON 2026-10-08 (Tier A: Jay asked for it; v2 + bow_fire_latched "That seems to have worked")
// [DEP: grapple_aim_from_gun] required-for=with a gun out, the grapple aims down the BARREL line (one reticle for both) (Jay
//      2026-09-28: "Aiming the main reticle with your gun also aims the grapple") (GrappleAimFromGun). tree=INERT. DEFAULT ON
//      (acts only with grapple_hand_aim=1 - the point hook - and a fresh bore, i.e. fire_aim_from_barrel=1 + a gun held).
//      In PointHook (+D760A5): point = muzzle + bore * 1000 m and A2's origin = the muzzle, so +D76000 casts along the bore.
//      No bore (melee / empty hands) -> the controller aim, unchanged. HEADSET.
bool GrappleAimFromGun = true;
// [DEP: grapple_reticle_3d] required-for=the grapple's CAN / CAN'T icons in 3D at its target, not the flat HUD
//      (Grapple3dReticle). tree=INERT. Was DEFAULT OFF. Swaps HudRopeReticle's slot 178 (vt +1507B48, expected +5AAD70): calls the
//      game's update, reads the result the game drew from - visible bit +48, stored target `sub_1805AA8D0(ctrl)`, anchor gate
//      `sub_180D75DF0()` || ctrl+195, `ctrl+193` (not usable) - and publishes: `crosshair_grapple_go` at ctrl+136..144 whenever the
//      game holds a VALID stored target (stored && !+193 - what the throw commits; v3, Jay 2026-09-28: the game's own G_Valid gate
//      +D75DF0 hid it in every session after the first while all 17 throws committed USABLE targets), NOTHING otherwise - a refused
//      hit (+193) included (D-139: "either a Grapple available or Nothing"); then parks the
//      HudRopeReticle group off-screen. Drawn by overlay.cpp on its own quad. Needs overlay_enabled=1. HEADSET.
bool Grapple3dReticle = true;   // DEFAULT ON 2026-10-01 (Jay: Tier B ships on)
// [DEP: grapple_reticle_scale] required-for=grapple_reticle_3d icon size, separately from the gun reticle (Jay 2026-09-28)
//      (GrappleReticleScale). tree=INERT. 0..1, was DEFAULT 0.4. 1.0 = the flat HUD's angular size (hud.xui: 36 design px tall).
float GrappleReticleScale = 0.2f;   // DEFAULT 0.2 2026-10-01 (Tier D: Jay's live tuning)
// [DEP: diag_move_bob] required-for=[MOVEBOB] - how much the AIM (gun bore, grapple ray) and the camera move per frame in the
//      RENDERED view's frame while standing / walking / sprinting, next to the controller's own motion relative to the head
//      (Jay 2026-09-28: "both the grapple & the guns reticle bob whilst moving, along with actually swaying side to side
//      slightly, due to the arms movements") (DiagMoveBob). tree=PROBE-ONLY. Log-only, runs in CrosshairHook (HUD update):
//      reads boneblend's published bore, camhook's rendered-view matrix, the grapple ray PointHook last cast, the OpenXR
//      head/controller poses. First sample logs regardless; one line per bucket per 5 s. Needs the A3 hook (fire_reticle_3d
//      etc.), the rig (bore) and camhook (rendered view) -> HEADSET. DEFAULT OFF.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in grappleaim.h (docs/BETA_CENSUS.md)
bool DiagMoveBob = false;
#endif
// [DEP: fire_reticle_scale] required-for=fire_reticle_3d's crosshair parts' angular size (FireReticleScale). tree=INERT.
//      1.0 = the flat HUD's own size: a part W design px wide subtends what W px subtend at the look camera's FOV
//      (720 design px = the vertical FOV, INV-281's formula inverted). The spread OFFSETS are angles and are NOT scaled.
//      Was DEFAULT 0.4 (Jay 2026-09-28: at 1.0 "rather large"; "Set it to 0.4 ... a slider 0.0 - 1"). Live: the overlay slider.
float FireReticleScale = 0.15f;   // DEFAULT 0.15 2026-10-01 (Tier D: Jay's live tuning)
// [DEP: diag_ray_threads] required-for=[RAYTHREAD] - which threads run the BULLET's own world cast (+715D90's two Raytrace
//      calls, returns +716066 / +7160BA, fa10 instruction level) vs the HudCrosshair update that runs our gun-reticle cast,
//      and whether a bullet cast ever lands WHILE ours is in flight (INV-278: +715D90 toggles COFlags 8/32 on level
//      objects around its cast, so calling it from the HUD update is only safe if the two never overlap) (DiagRayThreads).
//      tree=PROBE-ONLY. Log-only; reads GetCurrentThreadId in the existing Raytrace IAT hook + CrosshairHook - NO new hook.
//      NEEDS the A3 hook installed (grapple_crosshair_marker or fire_reticle_3d) and a gun fired.
//      FLAT-safe for the thread identities (no VR data read); the overlap count only means something while the gun
//      reticle casts (a gun out + fire_reticle_3d). v2 (after flat1 showed a 7-thread WORKER POOL
//      running both, so thread identity cannot decide it): times every HUD crosshair update (CrosshairHook wrapper, QPC ring
//      of 64) and buckets each bullet cast's distance to the nearest one - FLAT-safe, since the HUD update runs flat and our
//      cast always runs inside it. DEFAULT OFF.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in grappleaim.h (docs/BETA_CENSUS.md)
bool DiagRayThreads = false;
#endif
// [DEP: diag_grapple_aim] required-for=[GRAPPLEAIM] per-second windows (redirects, refusals by reason, game->hand
//      pitch/yaw change, measured hand-to-eye offset, origin substitutions, stored target vs hand ray, marker moves)
//      (DiagGrappleAim)
//      tree=PROBE-ONLY. Log-only. The install lines and the FIRST redirect / origin / marker samples log regardless.
//      HEADSET.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in grappleaim.h (docs/BETA_CENSUS.md)
bool DiagGrappleAim = false;
#endif

namespace {

using PointFn    = float*(__fastcall*)(void* cam, float* out, float x, float y);
using PosFn      = float*(__fastcall*)(const void* cam, float* out);
using OriginFn   = float*(__fastcall*)(void* ctrl, float* out);
using RaytraceFn = unsigned char(__fastcall*)(void* self, unsigned char kind, void* coll, const float* from, float* to,
                                              unsigned short a6, void* ignore, unsigned int a8, unsigned int a9, int a10);
using XhairFn    = char(__fastcall*)(void* self);
using RelPosFn   = float*(__fastcall*)(const void* elem, float* out);
using SetPosFn   = void(__fastcall*)(void* elem, const float* pos);
using ProjectFn  = void*(__fastcall*)(float* out, const float* point, void* cam);
using OwnerFn    = uintptr_t(__fastcall*)(void* ctrl);   // rope ctrl vfunc +32, as +D76000 calls it before its cast (gr3 :220-225)
using IfaceFn    = uintptr_t(__fastcall*)(void* iface);  // vfunc 0 of the owner's +2336 interface (bullet+8, fa2 :851-854 / fa10 +713220)
using BulletCastFn = unsigned char(__fastcall*)(void* igs, const float* from, float* to, void* coll, void* ignore);   // +715D90
using SpreadFn   = float*(__fastcall*)(uintptr_t owner, float* out);                        // +BC3BF0 (rt1)
using FovFn      = float(__fastcall*)(void* cam);                                           // IBaseCamera::GetFOV (degrees)
constexpr uintptr_t kBulletCastRva = 0x715D90;   // fa10
constexpr uintptr_t kSpreadRva     = 0xBC3BF0;   // rt1: out[0] = WFC+148 + owner[24*hand+9932], out[1] = WFC+152 + ...  (deg)
constexpr int       kXhairGroupOff = 344;        // HudCrosshair: current group index (rt1 +551720 / +550FC0 :148-150)
constexpr int       kXhairDotSlot  = 44;         // HudCrosshair a1[44] = G_Dot (rt3 :93)
constexpr int       kElemVisOff    = 48;         // IUIElement +48 bit 0 = visible (rt1 +550FC0 :184-203)
constexpr int       kHitCounterOff = 10068;      // owner +10068: the HUD plays G_Hit "Hit" when it changes (rt1 :154-182)
const char* const   kFovName = "?GetFOV@IBaseCamera@@QEAAMXZ";   // gamedll IAT, gr9

constexpr uintptr_t kRetRva       = 0xD760AB;   // return address of +D760A5 GetOnScreenPointF (gr9)
constexpr uintptr_t kOriginRva    = 0xD7D630;   // sub_180D7D630 (gr11)
constexpr uintptr_t kOriginAimRet = 0xD760B7;   // +D760B2's return, in +D76000 (gr11)
constexpr uintptr_t kOriginLedgeRet = 0xD7DA4E; // +D7DA49's return, in +D7D900 (gr11)
constexpr uintptr_t kRayRet       = 0xD763CC;   // +D763C6 Raytrace's return, in +D76000 (gr12b)
constexpr uintptr_t kRay2Ret      = 0xD76D85;   // +D76D7F (the second cast) Raytrace's return, in +D76000 (gr23 (7))
constexpr uintptr_t kXhairVtRva   = 0x14E1F68;  // HudCrosshair primary vtable (gr12)
constexpr int       kXhairSlot    = 178;
constexpr uintptr_t kXhairUpdRva  = 0x550FC0;   // its slot 178 (gr12)
constexpr uintptr_t kProjectRva   = 0x556580;   // world -> HUD projection (gr12)
constexpr uintptr_t kCamGlobalRva = 0x1C16348;  // qword_181C16348; camera = [[[g+1344]+192]+80] (INV-267)
constexpr uintptr_t kRopeGlobalRva = 0x1C16358; // qword_181C16358; rope ctrl = [[g+1920]+3200] (+5AAD70 :36-41, gr3)
constexpr float     kGunCastM     = 200.0f;     // the gun reticle (fire_reticle_3d): cast length along the bore
constexpr float     kGunPinM      = 50.0f;      // ... and where the dot sits on a miss (Jay 2026-09-28: pin, never recentre)
constexpr float     kGunSelfHitM  = 0.05f;      // a hit this close to the muzzle = our own gun -> treated as a miss
// bow_reticle_3d (INV-496), all from OUR gamedll 97046bba:
constexpr uintptr_t kArrowSpeedRva   = 0xD9D650;   // WeaponBowController GetArrowSpeed(ctrl) - draw-dependent (bow2 :377-418)
// Before the session's first arrow: the arrow acceleration MEASURED on 21/21 arrows of run 20261008_bowreticle_run1 ([BOWAIM]
// +132 1.000 x (0, -9.8, 0)). The static +1CFB510 SpawnArrow falls back to read (0, 0, 0) every time there - unusable (v1 drew
// a straight line until the first arrow: 963 placements, Jay's "first shot" defect).
constexpr float     kArrowGravDefault[3] = { 0.0f, -9.8f, 0.0f };
constexpr int       kOwnerBowCtrlOff = 3120;       // owner +3120 = the bow controller (+BC3BF0 :75-80, +550FC0 :95)
constexpr int       kBowActiveOff    = 16;         // its active byte (the same test +BC3BF0 / +550FC0 make)
constexpr int       kBowStateOff     = 32;         // EBowControllerState: 1 drawing, 3 drawn/held (+D9BFE0 :1279-1485)
constexpr int       kBowDrawOff      = 108;        // draw 0..1 (springs in states 1/3)
constexpr int       kBowArcSteps     = 16;         // arc segments, each one cast
constexpr float     kBowArcMaxS      = 3.0f;       // arc time cap (s); the length cap is kGunCastM
constexpr float     kReachM       = 1000.0f;
constexpr ULONGLONG kFreshMs      = 120;
// +D7D630 prologue: mov [rsp+10h],rsi | push rdi | sub rsp,30h | mov rax,[rcx] | mov rdi,rdx (gr11)
const unsigned char kOriginBytes[] = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83,
                                       0xEC, 0x30, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xFA };
const char* const kPointName = "?GetOnScreenPointF@IBaseCamera@@QEAA?BVvec3@@MM@Z";
const char* const kPosName   = "?GetPosition@IBaseCamera@@QEBA?BVvec3@@XZ";
const char* const kFwdName   = "?GetForwardVector@IBaseCamera@@QEBA?BVvec3@@XZ";
const char* const kCamLeftName = "?GetLeftVector@IBaseCamera@@QEBA?BVvec3@@XZ";  // camera matrix column 0 (fov_pass3)
const char* const kCamUpName   = "?GetUpVector@IBaseCamera@@QEBA?BVvec3@@XZ";    // camera matrix column 1 (fov_pass3)
const char* const kRayName   = "?Raytrace@IGSObject@@QEAAEEPEAUSCollision@@AEBVvec3@@AEAV3@GPEAVIControlObject@@IIH@Z";
const char* const kGetRelName = "?GetRelativePos@IUIElement@@QEBA?AVvec3@@XZ";
const char* const kSetRelName = "?SetRelativePos@IUIElement@@QEAAXAEBVvec3@@@Z";

uintptr_t     g_gd = 0;
// The loaded build's rvas of the Steam ones the per-frame paths add to g_gd (common/buildrva.h: Steam unchanged, the EGL
// table on the Epic build). Resolved once by ResolveRvas, just before g_gd is set and before any hook installs.
uintptr_t     g_rLeftRet = 0, g_rCamGlobal = 0, g_rRopeGlobal = 0, g_rOriginAimRet = 0, g_rOriginLedgeRet = 0;
uintptr_t     g_rBulletRayRet1 = 0, g_rBulletRayRet2 = 0, g_rRayRet = 0, g_rRay2Ret = 0;
uintptr_t     g_rSpread = 0, g_rBulletCast = 0, g_rArrowSpeed = 0;
PointFn       g_realPoint = nullptr;
PosFn         g_getPos    = nullptr;
PosFn         g_getFwd    = nullptr;
PosFn         g_getLeft   = nullptr;   // fire_reticle_3d
PosFn         g_getUp     = nullptr;   // fire_reticle_3d
FovFn         g_getFov    = nullptr;   // fire_reticle_3d: part sizes (design px -> degrees)
OriginFn      g_trueOrigin = nullptr;
RaytraceFn    g_realRay   = nullptr;
XhairFn       g_trueXhair = nullptr;
RelPosFn      g_getRel    = nullptr;
SetPosFn      g_setRel    = nullptr;
ProjectFn     g_project   = nullptr;
uintptr_t     g_retAddr   = 0;
volatile LONG g_tried     = 0;
volatile LONG g_firstSaid = 0, g_firstOriginSaid = 0, g_firstMarkerSaid = 0;
volatile LONG g_originLive = 0, g_markerLive = 0;
volatile LONG g_gunAimed = 0, g_gunAimFirstSaid = 0;   // grapple_aim_from_gun
// diag_move_bob: the grapple ray PointHook last produced (game world), seqlock-free (a torn read only perturbs one sample)
volatile float     g_mbGrapOrigin[3] = {}, g_mbGrapDir[3] = {};
volatile ULONGLONG g_mbGrapMs = 0;

// Per-call context: set by the point hook at +D760A5, read by the origin detour at +D760B2 / inside +D7D900 on the
// SAME thread during the SAME +D76000 call.
struct AimCtx {
    bool      valid;      // part A redirected this call and the hand could be placed
    float     cam[3];
    float     hand[3];    // game space
    float     dir[3];     // hand ray direction, game space
    ULONGLONG tick;
    int       bucket;     // ELEVATION probe: hand-ray elevation minus the HEAD's (the real view), bucketed; -1 = no head
    float     relElev;
    float     ctlFwd[3];  // the right controller's OpenXR forward at this call (throw probe: hand rotation since the aim froze)
};
// LEDGE-ASSIST probe (Jay 2026-09-27: it latches once he turns his HEAD toward the target; head yaw turns the body).
// +D7D900 steps the hit "up the wall" along BODY-left x wall-normal (gr10 :118-150). Facing buckets = the angle between
// the body's left vector and the wall normal: 90 = facing the wall (left along the wall), 0 = side-on (the cross
// product collapses). 6 buckets of 15 deg; index 6 = the assist did not run in that +D76000 call.
constexpr int kFaceBuckets = 6;
const char* const kFaceNames[kFaceBuckets + 1] = { "0-15", "15-30", "30-45", "45-60", "60-75", "75-90", "not-run" };
thread_local bool  t_leftCaptured = false;
thread_local float t_left[3] = {};
thread_local int   t_assistBucket = kFaceBuckets;      // this +D76000 call
thread_local int   t_prevAssistBucket = kFaceBuckets;  // the previous call on this thread (paired with its stored target)
// Elevation buckets (deg, hand minus head): < -45, -45..-25, -25..-10, -10..10, 10..25, 25..45, > 45.
constexpr int kElevBuckets = 7;
int ElevBucket(float rel)
{
    static const float edges[kElevBuckets - 1] = { -45.0f, -25.0f, -10.0f, 10.0f, 25.0f, 45.0f };
    int b = 0;
    while (b < kElevBuckets - 1 && rel >= edges[b]) { ++b; }
    return b;
}
const char* const kElevNames[kElevBuckets] = { "<-45", "-45..-25", "-25..-10", "-10..10", "10..25", "25..45", ">45" };
thread_local AimCtx t_aim = {};
// Last good yaw mapping (controller forward -> hand dir), for the rare update where the controller points straight
// up/down and the horizontal parts are too short to derive it.
thread_local bool  t_haveMap = false;
thread_local float t_cos = 1.0f, t_sin = 0.0f;
// The game's own screen-centre world point for this +D76000 call (GetOnScreenPointF(W/2, H/2) BEFORE part A rewrites
// it). A3 uses it as "forward": IBaseCamera::GetForwardVector points BEHIND the view (engine gr13, INV-273), which
// made run 2 refuse every marker sample.
thread_local bool  t_haveCentre = false;
thread_local float t_centre[3] = {};
// The last grapple cast on this thread (+D763CC): where the HAND RAY actually hit, for the aimed-vs-target probe.
struct CastRec {
    bool  valid;      // a cast happened after a placed hand context
    bool  hit;        // Raytrace returned true
    float at[3];      // the ray's end = the hit when hit
    float hand[3];
    float dir[3];
    ULONGLONG tick;
    int   bucket;
    float ctlFwd[3];
    // The hit record +D76000 passes to its first cast (gr20: r8 = [rbp+100h+var_160]): the flags WORD at +0x3A, which
    // +D763F3..+D7640C tests against 0x100 / 0x1000 (either = not grappleable, the decompile's v131 & 0x1000000 /
    // 0x10000000), and the static-object id at +0x28 (+D7647D, used to look the mesh name up for the whitelist).
    unsigned short flags;
    unsigned long long objId;
    uintptr_t collObj;   // record+0x18 (gr21: [rbp+100h+var_148]) - the object the +0x690/+0x688/+0x6A0 type checks use (obj+16);
                         // the owning object is at collObj - 24
    bool  recOk;
    float dist;   // origin -> hit
    unsigned short type;   // record+0x38 word of the FIRST cast (engine sub_180028730: the raw hit's +24 word)
    // The SECOND cast of the same +D76000 call (+D76D7F, returns +D76D85; gr23 (7)): filter 8 instead of 0x20, from the
    // origin to the first hit; if it hits, its record+0x38 word == 3 makes the hit NOT usable (+D76D89), and Raytrace
    // writes ITS hit into the same end vector - so the stored target is the second cast's point (INV-273 gr23-29).
    bool  t2Ran;
    bool  t2Hit;
    unsigned short t2Type;
    float t2At[3];
};
thread_local CastRec t_cast = {};

// The grapple's latest cast (A3), shared across threads.
volatile LONG g_hitLock = 0;
float     g_hit[3] = {};
float     g_centre[3] = {};
bool      g_haveCentre = false;
bool      g_hitOk = false;
ULONGLONG g_hitTick = 0;
long long g_hitSeq = 0;
CastRec   g_lastCast = {};   // the latest grapple cast from ANY thread, for the throw-commit hook (game thread)
// (The previous cast + the re-aim flag - option A's frozen aim and its re-aim column: REMOVED 2026-10-07 with grapple_prethrow_reaim.)

// Window counters (Interlocked - several threads).
volatile LONG g_callsOurs = 0, g_callsOther = 0, g_redirected = 0;
volatile LONG g_refNoPose = 0, g_refDegenerate = 0, g_refMapping = 0, g_refFault = 0, g_refOff = 0;
volatile LONG g_origAim = 0, g_origLedge = 0, g_origPass = 0, g_origNoCtx = 0, g_origStale = 0;
volatile LONG g_rayOurs = 0, g_rayHits = 0;
volatile LONG g_xhMoves = 0, g_xhRestores = 0, g_xhStale = 0, g_xhBehind = 0, g_xhFault = 0;
volatile LONG g_winLock = 0;
ULONGLONG     g_winStart = 0;
// Accumulated under g_winLock.
double g_sumDPitch = 0.0, g_sumDYaw = 0.0, g_sumPerp = 0.0, g_sumUp = 0.0, g_sumRight = 0.0, g_sumFwd = 0.0;
float  g_maxPerp = 0.0f;
long   g_offN = 0, g_offMissing = 0;
// Stored target (ctrl+136) vs the hand ray, read at the aim-ray origin call: 1-degree histogram, 0..179.
long   g_stHist[180] = {};
long   g_stN = 0, g_stNone = 0, g_stValid = 0, g_stFault = 0;
float  g_stMax = 0.0f;
// AIMED vs TARGET (Jay, 2026-09-27: "log where it's being AIMED from the hands ray vs where it's actually appearing
// relative to usable state"): the hand ray's own cast (+D763CC) of the PREVIOUS +D76000 call on this thread vs the
// target that call left stored at ctrl+136 (after retention / ledge assist) - which is where the reticle is drawn -
// and its usable flag (+193 == 0). Distances in 5 cm bins (0..10 m, bin 200 = beyond).
long   g_avDist[201] = {};
long   g_avAng[180] = {};
long   g_avPairs = 0, g_avNone = 0, g_avUsable = 0, g_avUnusable = 0, g_avCastMiss = 0, g_avMissKeptUsable = 0;
long   g_avOnRay = 0, g_avSticky = 0, g_avBeyond = 0, g_avUsableOff = 0, g_avStale = 0;
float  g_avMaxD = 0.0f;
long   g_state[9] = {};   // ctrl+72 (the rope controller's state; the throw writes 3, gr10) - 0..7, 8 = other
long   g_throws = 0;
// ELEVATION probe (Jay: "I can't aim too high or it won't register, unless I then look upwards"): per bucket of
// hand-ray elevation minus head elevation - casts, hits, and (at the next call) usable / none stored; throws by the
// bucket of the cast just before them. Miss ray length = the grapple's range as cast.
long   g_elCasts[kElevBuckets] = {}, g_elHits[kElevBuckets] = {}, g_elUsable[kElevBuckets] = {};
long   g_elUnusable[kElevBuckets] = {}, g_elNone[kElevBuckets] = {}, g_elThrows[kElevBuckets] = {};
long   g_elNoHead = 0;
double g_missLenSum = 0.0;
float  g_missLenMax = 0.0f;
long   g_missN = 0;
volatile LONG g_throwCommits = 0, g_throwFirstSaid = 0;
// LEDGE-ASSIST window (under g_winLock).
long   g_laCalls[2] = {};                         // by pass (a7 = 0 first, 1 second)
long   g_laN[kFaceBuckets] = {}, g_laMoved[kFaceBuckets] = {}, g_laLost[kFaceBuckets] = {}, g_laGained[kFaceBuckets] = {};
double g_laMoveSum[kFaceBuckets] = {};
float  g_laMoveMax[kFaceBuckets] = {};
long   g_laNoLeft = 0, g_laFault = 0;
long   g_laYawN = 0, g_laYawOff45 = 0;           // body facing vs hand-ray heading
double g_laYawSum = 0.0;
long   g_laOut[kFaceBuckets + 1][3] = {};         // the stored target at the NEXT update: usable / unusable / none
float  g_laDistMin = 0.0f, g_laDistMax = 0.0f;    // pass-0 distMax = the sticky var AS THE ENGINE READ IT (+D76000 :471)
long   g_laDistN = 0;
volatile LONG g_laFirstSaid = 0;
// REFUSAL events (Jay run 6: "If I FIGHT the placement, I can get it to latch ... I have to FIGHT it"): the hand ray HIT
// a surface but the next update holds no usable target, or holds an older one, or the grapple icon's own gate
// (+D75DF0: target present, not frozen, stamina >= connection cost, player-state checks) says no.
volatile LONG g_rfFlagged = 0, g_rfUnflagged = 0, g_rfKeptOlder = 0, g_rfIconHidden = 0, g_rfIconChecks = 0;
volatile LONG g_rfLines = 0;
constexpr LONG kMaxRefusalLines = 400;
volatile LONG g_xhHidden = 0;
using GateFn = bool(__fastcall*)(void* ctrl);
GateFn g_iconGate = nullptr;                      // sub_180D75DF0 - the grapple icon's gate 2 (gr4)
thread_local ULONGLONG t_lastGateCheck = 0;
volatile LONG64 g_rfLastLine[4] = {};             // per-category line throttle (ms tick)
// Throw events (state -> 3), logged one line each, capped (INV-9).
uintptr_t g_prevCtrl = 0;
int       g_prevState = -1;
volatile LONG g_throwLines = 0;
constexpr LONG kMaxThrowLines = 300;

void Lock(volatile LONG& l)   { while (InterlockedCompareExchange(&l, 1, 0) != 0) { YieldProcessor(); } }
void Unlock(volatile LONG& l) { InterlockedExchange(&l, 0); }

float ElevDeg(const float v[3])
{
    const float h = sqrtf(v[0] * v[0] + v[2] * v[2]);
    return atan2f(v[1], h) * 57.2957795f;
}

// Signed horizontal angle a -> b about +Y, degrees (dot/cross, no axis-order assumption).
float YawDeltaDeg(const float a[3], const float b[3])
{
    return atan2f(a[2] * b[0] - a[0] * b[2], a[0] * b[0] + a[2] * b[2]) * 57.2957795f;
}

void RotateByQuat(const XrQuaternionf& q, float vx, float vy, float vz, float& rx, float& ry, float& rz)
{
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
    const float xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
    const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    rx = vx * (1.0f - (yy + zz)) + vy * (xy - wz)        + vz * (xz + wy);
    ry = vx * (xy + wz)          + vy * (1.0f - (xx + zz)) + vz * (yz - wx);
    rz = vx * (xz - wy)          + vy * (yz + wx)          + vz * (1.0f - (xx + yy));
}

int Percentile(const long* hist, long n, double q)
{
    if (n <= 0) { return -1; }
    const long want = static_cast<long>(q * n + 0.5);
    long acc = 0;
    for (int i = 0; i < 180; ++i) {
        acc += hist[i];
        if (acc >= want && acc > 0) { return i; }
    }
    return 179;
}

int PercentileN(const long* hist, int bins, long n, double q)
{
    if (n <= 0) { return -1; }
    const long want = static_cast<long>(q * n + 0.5);
    long acc = 0;
    for (int i = 0; i < bins; ++i) {
        acc += hist[i];
        if (acc >= want && acc > 0) { return i; }
    }
    return bins - 1;
}

// Option 3: the hand-to-eye offset that makes a near target miss, measured in OpenXR space from the live poses.
bool MeasureOffset(const XrPosef& c, const XrPosef& h, float& perp, float& up, float& right, float& fwd)
{
    float dx, dy, dz;
    RotateByQuat(c.orientation, 0.0f, 0.0f, -1.0f, dx, dy, dz);
    const float ox = c.position.x - h.position.x;
    const float oy = c.position.y - h.position.y;
    const float oz = c.position.z - h.position.z;
    fwd = ox * dx + oy * dy + oz * dz;
    const float px = ox - fwd * dx, py = oy - fwd * dy, pz = oz - fwd * dz;
    perp = sqrtf(px * px + py * py + pz * pz);
    up = py;
    float rx, ry, rz;
    RotateByQuat(h.orientation, 1.0f, 0.0f, 0.0f, rx, ry, rz);   // head's +X = right
    const float rl = sqrtf(rx * rx + rz * rz);
    right = rl > 1e-4f ? (px * rx + pz * rz) / rl : 0.0f;
    return true;
}

void LogGateAndPullWindow();   // under g_winLock; defined with the gate / pull-trace probe below

void MaybeLogWindow()
{
    if (!DiagGrappleAim) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    Lock(g_winLock);
    if (g_winStart == 0) { g_winStart = now; }
    if (now - g_winStart < 1000) {
        Unlock(g_winLock);
        return;
    }
    const LONG ours = InterlockedExchange(&g_callsOurs, 0), other = InterlockedExchange(&g_callsOther, 0);
    const LONG red = InterlockedExchange(&g_redirected, 0);
    const LONG rp = InterlockedExchange(&g_refNoPose, 0), rd = InterlockedExchange(&g_refDegenerate, 0);
    const LONG rm = InterlockedExchange(&g_refMapping, 0), rf = InterlockedExchange(&g_refFault, 0);
    const LONG ro = InterlockedExchange(&g_refOff, 0);
    const LONG oa = InterlockedExchange(&g_origAim, 0), ol = InterlockedExchange(&g_origLedge, 0);
    const LONG op = InterlockedExchange(&g_origPass, 0), onc = InterlockedExchange(&g_origNoCtx, 0);
    const LONG ost = InterlockedExchange(&g_origStale, 0);
    const LONG ry = InterlockedExchange(&g_rayOurs, 0), rh = InterlockedExchange(&g_rayHits, 0);
    const LONG xm = InterlockedExchange(&g_xhMoves, 0), xr = InterlockedExchange(&g_xhRestores, 0);
    const LONG xs = InterlockedExchange(&g_xhStale, 0), xb = InterlockedExchange(&g_xhBehind, 0);
    const LONG xf = InterlockedExchange(&g_xhFault, 0);
    // INV-6: every average below says NO VERDICT when its sample count is zero, instead of printing a zero.
    char chg[96], off[192], st[160];
    if (red > 0) {
        _snprintf_s(chg, sizeof(chg), _TRUNCATE, "mean |game->hand| pitch %.1f yaw %.1f deg",
                    g_sumDPitch / red, g_sumDYaw / red);
    } else {
        _snprintf_s(chg, sizeof(chg), _TRUNCATE, "game->hand change: NO VERDICT (0 redirected)");
    }
    if (g_offN > 0) {
        _snprintf_s(off, sizeof(off), _TRUNCATE, "(n %ld, missing %ld): perpendicular to aim mean %.3f max %.3f m, "
                    "up %+.3f, right %+.3f, along-aim %+.3f m", g_offN, g_offMissing, g_sumPerp / g_offN, g_maxPerp,
                    g_sumUp / g_offN, g_sumRight / g_offN, g_sumFwd / g_offN);
    } else {
        _snprintf_s(off, sizeof(off), _TRUNCATE, "(n 0, missing %ld): NO VERDICT", g_offMissing);
    }
    if (g_stN > 0) {
        _snprintf_s(st, sizeof(st), _TRUNCATE, "n %ld, none stored %ld, valid %ld, read-fault %ld, angle median %d "
                    "p90 %d max %.1f deg", g_stN, g_stNone, g_stValid, g_stFault, Percentile(g_stHist, g_stN, 0.5),
                    Percentile(g_stHist, g_stN, 0.9), g_stMax);
    } else {
        _snprintf_s(st, sizeof(st), _TRUNCATE, "n 0 - NO VERDICT (none stored %ld, read-fault %ld; needs "
                    "grapple_hand_origin's detour and a placed hand)", g_stNone, g_stFault);
    }
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] window: calls at +D760AB %ld (other GetOnScreenPointF callers %ld, untouched) | "
        "redirected %ld | refused: no pose %ld, degenerate %ld, mapping %ld, fault %ld, feature-off %ld | %s | "
        "HAND-TO-EYE OFFSET %s", ours, other, red, rp, rd, rm, rf, ro, chg, off);
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] window (round 2): ORIGIN hand-substituted aim %ld ledge-assist %ld | not ours "
        "(other callers) %ld | not substituted: no context %ld, stale %ld | STORED TARGET vs hand ray: %s | CAST at "
        "+D763CC %ld, hit %ld | MARKER moved %ld restored %ld stale %ld behind-camera %ld fault %ld",
        oa, ol, op, onc, ost, st, ry, rh, xm, xr, xs, xb, xf);
    // AIMED vs TARGET window (INV-6: NO VERDICT when empty).
    char av[512];
    if (g_avPairs > 0) {
        const long castHits = g_avOnRay + g_avSticky + g_avBeyond;
        char dd[160];
        if (castHits > 0) {
            _snprintf_s(dd, sizeof(dd), _TRUNCATE, "aimed-to-target distance median %.2f p90 %.2f max %.2f m (n %ld): "
                        "on the ray <5 cm %ld, within 0.75 m %ld, beyond %ld",
                        PercentileN(g_avDist, 201, castHits, 0.5) * 0.05, PercentileN(g_avDist, 201, castHits, 0.9) * 0.05,
                        g_avMaxD, castHits, g_avOnRay, g_avSticky, g_avBeyond);
        } else {
            _snprintf_s(dd, sizeof(dd), _TRUNCATE, "aimed-to-target distance: NO VERDICT (every cast missed)");
        }
        long angN = 0;
        for (int i = 0; i < 180; ++i) { angN += g_avAng[i]; }
        _snprintf_s(av, sizeof(av), _TRUNCATE, "pairs %ld: target USABLE %ld / not usable %ld | %s | USABLE target off "
                    "the aimed point %ld | cast missed %ld (of which a usable target was KEPT %ld) | angle off the hand "
                    "ray median %d p90 %d deg (n %ld)",
                    g_avPairs, g_avUsable, g_avUnusable, dd, g_avUsableOff, g_avCastMiss, g_avMissKeptUsable,
                    PercentileN(g_avAng, 180, angN, 0.5), PercentileN(g_avAng, 180, angN, 0.9), angN);
    } else {
        _snprintf_s(av, sizeof(av), _TRUNCATE, "pairs 0 - NO VERDICT");
    }
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] window AIMED vs TARGET: %s | no target stored %ld | no fresh cast %ld | rope state "
        "0:%ld 1:%ld 2:%ld 3:%ld 4:%ld 5:%ld 6:%ld 7:%ld other:%ld | throws %ld",
        av, g_avNone, g_avStale, g_state[0], g_state[1], g_state[2], g_state[3], g_state[4], g_state[5], g_state[6],
        g_state[7], g_state[8], g_throws);
    // ELEVATION window (INV-6: a bucket with no casts says so rather than printing 0 of 0 as a rate).
    char el[900];
    int ep = 0;
    el[0] = '\0';
    for (int b = 0; b < kElevBuckets && ep < static_cast<int>(sizeof(el)) - 80; ++b) {
        int n;
        if (g_elCasts[b] == 0 && g_elUsable[b] + g_elUnusable[b] + g_elNone[b] == 0) {
            n = _snprintf_s(el + ep, sizeof(el) - ep, _TRUNCATE, " %s: -", kElevNames[b]);
        } else {
            n = _snprintf_s(el + ep, sizeof(el) - ep, _TRUNCATE, " %s: cast %ld hit %ld | target usable %ld unusable %ld "
                            "none %ld | throws %ld;", kElevNames[b], g_elCasts[b], g_elHits[b], g_elUsable[b],
                            g_elUnusable[b], g_elNone[b], g_elThrows[b]);
        }
        if (n < 0) { break; }   // truncated: keep what fits
        ep += n;
    }
    char ml[96];
    if (g_missN > 0) {
        _snprintf_s(ml, sizeof(ml), _TRUNCATE, "miss ray length mean %.1f max %.1f m (n %ld)", g_missLenSum / g_missN,
                    g_missLenMax, g_missN);
    } else {
        _snprintf_s(ml, sizeof(ml), _TRUNCATE, "miss ray length: NO VERDICT (no miss)");
    }
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] window ELEVATION (hand ray minus HEAD, deg):%s | no head pose %ld | %s | throw commits "
        "(+D7A260 state 3) this window %ld | REFUSALS (updates): hit flagged-surface %ld, hit unflagged-but-no-target %ld, "
        "older target held %ld, icon gate false %ld of %ld checks | marker hidden on a miss %ld",
        el, g_elNoHead, ml, InterlockedExchange(&g_throwCommits, 0), InterlockedExchange(&g_rfFlagged, 0),
        InterlockedExchange(&g_rfUnflagged, 0), InterlockedExchange(&g_rfKeptOlder, 0),
        InterlockedExchange(&g_rfIconHidden, 0), InterlockedExchange(&g_rfIconChecks, 0), InterlockedExchange(&g_xhHidden, 0));
    for (int b = 0; b < kElevBuckets; ++b) {
        g_elCasts[b] = g_elHits[b] = g_elUsable[b] = g_elUnusable[b] = g_elNone[b] = g_elThrows[b] = 0;
    }
    // LEDGE-ASSIST window (INV-6: empty buckets print '-', empty averages say NO VERDICT).
    char la[1400];
    int lp = 0;
    la[0] = '\0';
    for (int b = 0; b <= kFaceBuckets && lp < static_cast<int>(sizeof(la)) - 120; ++b) {
        const long outs = g_laOut[b][0] + g_laOut[b][1] + g_laOut[b][2];
        const long ncalls = b < kFaceBuckets ? g_laN[b] : 0;
        int k;
        if (ncalls == 0 && outs == 0) {
            k = _snprintf_s(la + lp, sizeof(la) - lp, _TRUNCATE, " %s: -", kFaceNames[b]);
        } else if (b < kFaceBuckets && ncalls > 0) {
            k = _snprintf_s(la + lp, sizeof(la) - lp, _TRUNCATE, " %s: assists %ld moved>5cm %ld mean %.2f max %.2f m, hit "
                            "lost %ld gained %ld -> next target usable %ld unusable %ld none %ld;", kFaceNames[b], ncalls,
                            g_laMoved[b], g_laMoveSum[b] / ncalls, g_laMoveMax[b], g_laLost[b], g_laGained[b],
                            g_laOut[b][0], g_laOut[b][1], g_laOut[b][2]);
        } else {
            k = _snprintf_s(la + lp, sizeof(la) - lp, _TRUNCATE, " %s: -> next target usable %ld unusable %ld none %ld;",
                            kFaceNames[b], g_laOut[b][0], g_laOut[b][1], g_laOut[b][2]);
        }
        if (k < 0) { break; }
        lp += k;
    }
    char yw[96], dm[96];
    if (g_laYawN > 0) {
        _snprintf_s(yw, sizeof(yw), _TRUNCATE, "body-vs-hand heading mean %.1f deg, > 45 deg %ld of %ld",
                    g_laYawSum / g_laYawN, g_laYawOff45, g_laYawN);
    } else {
        _snprintf_s(yw, sizeof(yw), _TRUNCATE, "body-vs-hand heading: NO VERDICT (no assist with a hand context)");
    }
    if (g_laDistN > 0) {
        _snprintf_s(dm, sizeof(dm), _TRUNCATE, "sticky distMax as the engine read it %.3f..%.3f m (n %ld)", g_laDistMin,
                    g_laDistMax, g_laDistN);
    } else {
        _snprintf_s(dm, sizeof(dm), _TRUNCATE, "sticky distMax: NO VERDICT (the assist did not run)");
    }
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] window LEDGE ASSIST by wall-facing (body-left vs wall normal, 90 = facing the wall): "
        "calls pass0 %ld pass1 %ld | no left vector %ld | io fault %ld |%s | %s | %s", g_laCalls[0], g_laCalls[1],
        g_laNoLeft, g_laFault, la, yw, dm);
    g_laCalls[0] = g_laCalls[1] = 0;
    for (int b = 0; b < kFaceBuckets; ++b) {
        g_laN[b] = g_laMoved[b] = g_laLost[b] = g_laGained[b] = 0;
        g_laMoveSum[b] = 0.0;
        g_laMoveMax[b] = 0.0f;
    }
    for (int b = 0; b <= kFaceBuckets; ++b) { g_laOut[b][0] = g_laOut[b][1] = g_laOut[b][2] = 0; }
    g_laNoLeft = g_laFault = g_laYawN = g_laYawOff45 = 0;
    g_laYawSum = 0.0;
    g_laDistN = 0;
    g_laDistMin = g_laDistMax = 0.0f;
    g_elNoHead = 0;
    g_missLenSum = 0.0;
    g_missLenMax = 0.0f;
    g_missN = 0;
    for (int i = 0; i < 201; ++i) { g_avDist[i] = 0; }
    for (int i = 0; i < 180; ++i) { g_avAng[i] = 0; }
    for (int i = 0; i < 9; ++i) { g_state[i] = 0; }
    g_avPairs = g_avNone = g_avUsable = g_avUnusable = g_avCastMiss = g_avMissKeptUsable = 0;
    g_avOnRay = g_avSticky = g_avBeyond = g_avUsableOff = g_avStale = 0;
    g_avMaxD = 0.0f;
    g_throws = 0;
    g_winStart = now;
    g_sumDPitch = g_sumDYaw = g_sumPerp = g_sumUp = g_sumRight = g_sumFwd = 0.0;
    g_maxPerp = 0.0f;
    g_offN = g_offMissing = 0;
    for (int i = 0; i < 180; ++i) { g_stHist[i] = 0; }
    g_stN = g_stNone = g_stValid = g_stFault = 0;
    g_stMax = 0.0f;
    LogGateAndPullWindow();
    Unlock(g_winLock);
}

// ---------------------------------------------------------------- part A (+ the context A2 needs)
float* __fastcall PointHook(void* cam, float* out, float x, float y)
{
    float* r = g_realPoint(cam, out, x, y);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_retAddr) {
        InterlockedIncrement(&g_callsOther);
        return r;
    }
    t_aim.valid = false;   // this +D76000 call starts clean; set below only if everything lines up
    t_aim.bucket = -1;
    t_prevAssistBucket = t_assistBucket;   // the previous call's assist result pairs with the target it left stored
    t_assistBucket = kFaceBuckets;         // "did not run" until +D7D900 reports otherwise in this call
    InterlockedIncrement(&g_callsOurs);
    t_centre[0] = out[0]; t_centre[1] = out[1]; t_centre[2] = out[2];   // the game's own screen centre, pre-rewrite
    t_haveCentre = true;
    if (!GrappleHandAim) {
        InterlockedIncrement(&g_refOff);
        MaybeLogWindow();
        return r;
    }
    float cp[3];
    __try {
        g_getPos(cam, cp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_refFault);
        return r;
    }
    float orig[3] = { out[0] - cp[0], out[1] - cp[1], out[2] - cp[2] };
    const float ol = sqrtf(orig[0] * orig[0] + orig[1] * orig[1] + orig[2] * orig[2]);
    if (!(ol > 1e-3f) || !std::isfinite(ol)) {
        InterlockedIncrement(&g_refDegenerate);
        MaybeLogWindow();
        return r;
    }
    orig[0] /= ol; orig[1] /= ol; orig[2] /= ol;
    XrPosef ctl;
    // Steadied (comfort_hand_steady, C1): the grapple's hand aim, the same "aim with a still hand" job as a shot.
    // grapple_aim_hand: the chosen hand (right by default) - its pose here (the A2 origin) and its direction below.
    if (!GetControllerPoseSteady(AimHand(), ctl)) {
        InterlockedIncrement(&g_refNoPose);
        MaybeLogWindow();
        return r;
    }
    float aim[3];
    if (!ComputeAbsoluteAim(orig[0], orig[1], orig[2], aim, AimHand())) {
        InterlockedIncrement(&g_refMapping);
        MaybeLogWindow();
        return r;
    }
    const float before[3] = { out[0], out[1], out[2] };
    out[0] = cp[0] + aim[0] * kReachM;
    out[1] = cp[1] + aim[1] * kReachM;
    out[2] = cp[2] + aim[2] * kReachM;
    InterlockedIncrement(&g_redirected);

    // A2's context: the yaw part A just applied = the rotation taking the controller's forward (OpenXR) to aim
    // (game) about +Y (ComputeAbsoluteAim: out.x = cx*cos + cz*sin, out.z = -cx*sin + cz*cos, out.y = cy).
    float cx, cy, cz;
    RotateByQuat(ctl.orientation, 0.0f, 0.0f, -1.0f, cx, cy, cz);
    t_aim.ctlFwd[0] = cx; t_aim.ctlFwd[1] = cy; t_aim.ctlFwd[2] = cz;
    const float ch = sqrtf(cx * cx + cz * cz), ah = sqrtf(aim[0] * aim[0] + aim[2] * aim[2]);
    if (ch > 0.1f && ah > 0.1f) {
        t_cos = (cx * aim[0] + cz * aim[2]) / (ch * ah);
        t_sin = (cz * aim[0] - cx * aim[2]) / (ch * ah);
        t_haveMap = true;
    }
    XrPosef head;
    const bool haveHead = GetHeadPose(head);
    if (haveHead) {
        // Elevation is frame-independent (both spaces Y-up), so the head's OpenXR forward compares directly with aim.
        float hx, hy, hz;
        RotateByQuat(head.orientation, 0.0f, 0.0f, -1.0f, hx, hy, hz);
        const float hf[3] = { hx, hy, hz };
        t_aim.relElev = ElevDeg(aim) - ElevDeg(hf);
        t_aim.bucket = ElevBucket(t_aim.relElev);
    }
    if (t_haveMap && haveHead) {
        // The hand = the engine CAMERA position + R*(ctl - head): subtract the head position the engine was given
        // (SPEC-HEAD-NECK-PIVOT §3; raw while head_neck_pivot is off = today), or the neck-pivot arc returns reversed.
        // [LOADBEARING: HEAD-NECK-PIVOT] grapple aim origin.
        const XrVector3f headRep = NeckPivotReportedPos(head);
        const float dx = ctl.position.x - headRep.x;
        const float dy = ctl.position.y - headRep.y;
        const float dz = ctl.position.z - headRep.z;
        if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) {   // a tracking glitch must not throw the origin metres away
            t_aim.cam[0] = cp[0]; t_aim.cam[1] = cp[1]; t_aim.cam[2] = cp[2];
            t_aim.hand[0] = cp[0] + dx * t_cos + dz * t_sin;
            t_aim.hand[1] = cp[1] + dy;
            t_aim.hand[2] = cp[2] - dx * t_sin + dz * t_cos;
            t_aim.dir[0] = aim[0]; t_aim.dir[1] = aim[1]; t_aim.dir[2] = aim[2];
            t_aim.tick = GetTickCount64();
            t_aim.valid = true;
        }
    }
    // grapple_aim_from_gun: with a gun out (boneblend's bore fresh), the grapple ray IS the barrel line - origin the muzzle
    // (A2 hands +D7D630's aim call t_aim.hand), point = muzzle + bore * reach, so +D76000's normalise(point - origin) = bore.
    // Melee / empty hands: no fresh bore -> the controller aim above, unchanged (Jay 2026-09-28).
    if (GrappleAimFromGun) {
        float bdir[3] = {}, bpos[3] = {};
        unsigned bage = 0;
        const char* bwhy = "?";
        if (GetPublishedBarrelBore(bdir, &bage, &bwhy, bpos)) {
            out[0] = bpos[0] + bdir[0] * kReachM;
            out[1] = bpos[1] + bdir[1] * kReachM;
            out[2] = bpos[2] + bdir[2] * kReachM;
            for (int k = 0; k < 3; ++k) { t_aim.hand[k] = bpos[k]; t_aim.dir[k] = bdir[k]; t_aim.cam[k] = cp[k]; }
            t_aim.tick = GetTickCount64();
            t_aim.valid = true;
            InterlockedIncrement(&g_gunAimed);
            if (InterlockedCompareExchange(&g_gunAimFirstSaid, 1, 0) == 0) {
                // [PROBE: GRAPPLEAIM] gate=grapple_aim_from_gun  site=HEADSET  log-only=yes
                LOG("grappleaim: [GRAPPLEAIM] FIRST GUN-AIMED GRAPPLE RAY (logged unconditionally) | muzzle (%.3f, %.3f, %.3f) bore "
                    "(%.4f, %.4f, %.4f) age %u ms | controller aim was (%.4f, %.4f, %.4f) | origin detour %s",
                    bpos[0], bpos[1], bpos[2], bdir[0], bdir[1], bdir[2], bage, aim[0], aim[1], aim[2],
                    GrappleHandOrigin ? "on (the ray starts at the muzzle)" : "*** OFF - the ray starts at the game origin ***");
            }
        }
    }

    if (DiagMoveBob && t_aim.valid) {
        for (int k = 0; k < 3; ++k) { g_mbGrapOrigin[k] = t_aim.hand[k]; g_mbGrapDir[k] = t_aim.dir[k]; }
        g_mbGrapMs = GetTickCount64();
    }

    const float dPitch = ElevDeg(aim) - ElevDeg(orig);
    const float dYaw = YawDeltaDeg(orig, aim);
    float perp = 0.0f, up = 0.0f, right = 0.0f, fwd = 0.0f;
    const bool haveOff = haveHead && MeasureOffset(ctl, head, perp, up, right, fwd);

    if (InterlockedCompareExchange(&g_firstSaid, 1, 0) == 0) {
        // [PROBE: GRAPPLEAIM] gate=grapple_hand_aim  site=HEADSET  log-only=yes
        LOG("grappleaim: [GRAPPLEAIM] FIRST REDIRECT (logged regardless of diag_grapple_aim) | camera pos "
            "(%.2f, %.2f, %.2f) | game point (%.2f, %.2f, %.2f) = dir (%.3f, %.3f, %.3f) elev %.1f | HAND dir "
            "(%.3f, %.3f, %.3f) elev %.1f | change pitch %+.1f yaw %+.1f deg | written point (%.1f, %.1f, %.1f) | "
            "hand-to-eye offset %s perp %.3f up %+.3f right %+.3f along-aim %+.3f m | A2 hand origin %s (%.2f, %.2f, "
            "%.2f), yaw map cos %.3f sin %.3f",
            cp[0], cp[1], cp[2], before[0], before[1], before[2], orig[0], orig[1], orig[2], ElevDeg(orig),
            aim[0], aim[1], aim[2], ElevDeg(aim), dPitch, dYaw, out[0], out[1], out[2],
            haveOff ? "" : "(NO HEAD POSE - zeros)", perp, up, right, fwd,
            t_aim.valid ? "placed" : "NOT placed", t_aim.hand[0], t_aim.hand[1], t_aim.hand[2], t_cos, t_sin);
    }
    if (DiagGrappleAim) {
        Lock(g_winLock);
        g_sumDPitch += fabsf(dPitch);
        g_sumDYaw += fabsf(dYaw);
        if (haveOff) {
            ++g_offN;
            g_sumPerp += perp;
            g_sumUp += up;
            g_sumRight += right;
            g_sumFwd += fwd;
            if (perp > g_maxPerp) { g_maxPerp = perp; }
        } else {
            ++g_offMissing;
        }
        Unlock(g_winLock);
    }
    MaybeLogWindow();
    return r;
}

// ---------------------------------------------------------------- part A2 (+ the stored-target probe)
struct StoredTarget {
    float hit[3];
    float nrm[3];
    unsigned char invalid;
};

bool ReadStored(const void* ctrl, StoredTarget& s)
{
    __try {
        const unsigned char* p = static_cast<const unsigned char*>(ctrl);
        const float* f = reinterpret_cast<const float*>(p + 136);
        s.hit[0] = f[0]; s.hit[1] = f[1]; s.hit[2] = f[2];
        s.nrm[0] = f[3]; s.nrm[1] = f[4]; s.nrm[2] = f[5];
        s.invalid = p[193];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ProbeStored(const void* ctrl)
{
    if (!DiagGrappleAim || !t_aim.valid) {
        return;
    }
    StoredTarget s;
    const bool ok = ReadStored(ctrl, s);
    Lock(g_winLock);
    if (!ok) {
        ++g_stFault;
    } else if ((s.hit[0] == 0.0f && s.hit[1] == 0.0f && s.hit[2] == 0.0f) ||
               (s.nrm[0] == 0.0f && s.nrm[1] == 0.0f && s.nrm[2] == 0.0f)) {
        ++g_stNone;   // sub_1805AA8D0's own test: no stored target
    } else {
        const float vx = s.hit[0] - t_aim.hand[0], vy = s.hit[1] - t_aim.hand[1], vz = s.hit[2] - t_aim.hand[2];
        const float vl = sqrtf(vx * vx + vy * vy + vz * vz);
        if (vl > 1e-3f) {
            float c = (vx * t_aim.dir[0] + vy * t_aim.dir[1] + vz * t_aim.dir[2]) / vl;
            c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
            const float a = acosf(c) * 57.2957795f;
            int bin = static_cast<int>(a);
            bin = bin < 0 ? 0 : (bin > 179 ? 179 : bin);
            ++g_stHist[bin];
            ++g_stN;
            if (a > g_stMax) { g_stMax = a; }
            if (!s.invalid) { ++g_stValid; }
        }
    }
    Unlock(g_winLock);
}

// ---------------------------------------------------------------- the LEDGE ASSIST (+D7D900), probe only
// gr15: +D7D900(ctrl rcx, vec3* hit rdx (in/out), u8* hitOk r8 (in/out), SCollision* coll r9 (normal = its first
// vec3, used in the cross product :148-150), float step, float distMax, char pass); prologue 15 relocatable bytes.
// Its one body-left read is `call [IAT ?GetLeftVector@IControlObject@@QEBA?AVvec3@@XZ]` at +D7DA3C, return +D7DA42 -
// captured by an IAT hook filtered on that return address, so the probe records the EXACT vector the assist used.
using LeftFn   = float*(__fastcall*)(const void* obj, float* out);
using AssistFn = unsigned char(__fastcall*)(void* ctrl, float* hit, unsigned char* hitOk, float* coll, float step,
                                            float distMax, char pass);
LeftFn   g_realLeft = nullptr;
AssistFn g_trueAssist = nullptr;
constexpr uintptr_t kAssistRva = 0xD7D900;
constexpr uintptr_t kLeftRet   = 0xD7DA42;
const char* const kLeftName = "?GetLeftVector@IControlObject@@QEBA?AVvec3@@XZ";
// mov rax,rsp | push rbp | push rbx | push rsi | push rdi | push r12 | push r13 | push r14 | push r15 (gr15)
const unsigned char kAssistBytes[] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41,
                                       0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };

float* __fastcall LeftHook(const void* obj, float* out)
{
    float* r = g_realLeft(obj, out);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_gd + g_rLeftRet) {
        __try {
            t_left[0] = out[0]; t_left[1] = out[1]; t_left[2] = out[2];
            t_leftCaptured = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            t_leftCaptured = false;
        }
    }
    return r;
}

bool CopyAssistIo(const float* hit, const unsigned char* ok, const float* coll, float h[3], unsigned char& o, float n[3])
{
    __try {
        h[0] = hit[0]; h[1] = hit[1]; h[2] = hit[2];
        o = *ok;
        n[0] = coll[0]; n[1] = coll[1]; n[2] = coll[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

unsigned char __fastcall AssistHook(void* ctrl, float* hit, unsigned char* hitOk, float* coll, float step, float distMax,
                                    char pass)
{
    float hIn[3] = {}, nIn[3] = {};
    unsigned char okIn = 0;
    const bool haveIn = CopyAssistIo(hit, hitOk, coll, hIn, okIn, nIn);
    t_leftCaptured = false;
    const unsigned char r = g_trueAssist(ctrl, hit, hitOk, coll, step, distMax, pass);
    if (!DiagGrappleAim) {
        return r;
    }
    float hOut[3] = {}, nOut[3] = {};
    unsigned char okOut = 0;
    const bool haveOut = CopyAssistIo(hit, hitOk, coll, hOut, okOut, nOut);
    const bool haveLeft = t_leftCaptured;
    float faceDeg = -1.0f, yawOff = -1.0f, moved = -1.0f;
    int fb = -1;
    if (haveIn && haveLeft) {
        const float ll = sqrtf(t_left[0] * t_left[0] + t_left[1] * t_left[1] + t_left[2] * t_left[2]);
        const float nl = sqrtf(nIn[0] * nIn[0] + nIn[1] * nIn[1] + nIn[2] * nIn[2]);
        if (ll > 1e-4f && nl > 1e-4f) {
            float c = fabsf(t_left[0] * nIn[0] + t_left[1] * nIn[1] + t_left[2] * nIn[2]) / (ll * nl);
            c = c > 1.0f ? 1.0f : c;
            faceDeg = acosf(c) * 57.2957795f;   // 90 = left along the wall = facing it; 0 = side-on
            fb = static_cast<int>(faceDeg / 15.0f);
            fb = fb < 0 ? 0 : (fb >= kFaceBuckets ? kFaceBuckets - 1 : fb);
        }
        // Body facing vs the hand ray's heading: with left perpendicular to the heading the body faces along it.
        const float hx = t_aim.dir[0], hz = t_aim.dir[2];
        const float hl = sqrtf(hx * hx + hz * hz), lh = sqrtf(t_left[0] * t_left[0] + t_left[2] * t_left[2]);
        if (t_aim.valid && hl > 1e-3f && lh > 1e-3f) {
            float c = fabsf(hx * t_left[0] + hz * t_left[2]) / (hl * lh);
            c = c > 1.0f ? 1.0f : c;
            yawOff = 90.0f - acosf(c) * 57.2957795f;   // 0 = body faces the hand's heading (or directly away)
        }
    }
    if (haveIn && haveOut) {
        const float dx = hOut[0] - hIn[0], dy = hOut[1] - hIn[1], dz = hOut[2] - hIn[2];
        moved = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    if (pass == 0 && fb >= 0) {
        t_assistBucket = fb;   // the first pass's facing classifies this +D76000 call
    }
    Lock(g_winLock);
    ++g_laCalls[pass ? 1 : 0];
    if (pass == 0) {
        if (g_laDistN == 0 || distMax < g_laDistMin) { g_laDistMin = distMax; }
        if (g_laDistN == 0 || distMax > g_laDistMax) { g_laDistMax = distMax; }
        ++g_laDistN;
    }
    if (!haveIn || !haveOut) {
        ++g_laFault;
    } else if (fb < 0) {
        ++g_laNoLeft;
    } else if (pass == 0) {
        ++g_laN[fb];
        if (moved > 0.05f) { ++g_laMoved[fb]; }
        g_laMoveSum[fb] += moved;
        if (moved > g_laMoveMax[fb]) { g_laMoveMax[fb] = moved; }
        if (okIn && !okOut) { ++g_laLost[fb]; }
        if (!okIn && okOut) { ++g_laGained[fb]; }
    }
    if (yawOff >= 0.0f && pass == 0) {
        ++g_laYawN;
        g_laYawSum += yawOff;
        if (yawOff > 45.0f) { ++g_laYawOff45; }
    }
    Unlock(g_winLock);
    if (InterlockedCompareExchange(&g_laFirstSaid, 1, 0) == 0) {
        // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
        LOG("grappleaim: [GRAPPLEAIM] FIRST LEDGE ASSIST (logged regardless of window timing) pass %d step %.3f distMax %.3f "
            "(the sticky var as the ENGINE read it) | hit in (%.2f, %.2f, %.2f) ok %d -> out (%.2f, %.2f, %.2f) ok %d, moved "
            "%.3f m, returned %d | wall normal (%.3f, %.3f, %.3f) | body left %s (%.3f, %.3f, %.3f) | facing %.1f deg (90 = "
            "facing the wall) | body vs hand heading %.1f deg",
            static_cast<int>(pass), step, distMax, hIn[0], hIn[1], hIn[2], okIn, hOut[0], hOut[1], hOut[2], okOut, moved,
            static_cast<int>(r), nIn[0], nIn[1], nIn[2], haveLeft ? "captured" : "NOT CAPTURED", t_left[0], t_left[1],
            t_left[2], faceDeg, yawOff);
    }
    return r;
}

// ---------------------------------------------------------------- the THROW, at its commit (+D7A260(ctrl, 3))
// Run 3: ctrl+72 read 0 in all 9,570 +D76000 calls - +D76000 does not run with the state at 3, so the state-change
// detection above never fires. The commit itself is sub_180D7A260(ctrl, state) (gr14: prologue 15 relocatable bytes;
// the ONLY caller passing 3 is +D7879C in the throw path +D782E0). A pass-through detour (probe only, installed with
// diag_grapple_aim) logs, at each state -> 3, the latest hand-ray cast from any thread vs the stored target the game
// is committing to.
// The RTTI class name of an object, read DIRECTLY from its vtable's complete-object locator (x64: COL+12 = type
// descriptor RVA, COL+20 = the COL's own RVA -> image base; name at TD+16). No module walk, no cache needed - a handful
// of guarded reads, used only on throttled log lines.
void RttiClassOf(uintptr_t obj, char* out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s", obj ? "?" : "none");
    if (!obj) { return; }
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        const uintptr_t col = *reinterpret_cast<const uintptr_t*>(vt - 8);
        if (*reinterpret_cast<const int*>(col) != 1) { return; }
        const uintptr_t img = col - static_cast<uintptr_t>(*reinterpret_cast<const unsigned int*>(col + 20));
        const char* nm = reinterpret_cast<const char*>(img + *reinterpret_cast<const unsigned int*>(col + 12) + 16);
        _snprintf_s(out, cap, _TRUNCATE, "%s", nm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(out, cap, _TRUNCATE, "unreadable");
    }
}

bool ReadState(const void* ctrl, int& state)
{
    __try {
        state = *reinterpret_cast<const int*>(static_cast<const unsigned char*>(ctrl) + 72);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using CommitFn = void(__fastcall*)(void* ctrl, int state);
CommitFn g_trueCommit = nullptr;
constexpr uintptr_t kCommitRva = 0xD7A260;
// mov [rsp+8],rbx | push rdi | sub rsp,40h | mov edi,edx | mov rbx,rcx (gr14)
const unsigned char kCommitBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                       0xEC, 0x40, 0x8B, 0xFA, 0x48, 0x8B, 0xD9 };

void ThrowEvent(void* ctrl)
{
    CastRec c;
    Lock(g_hitLock);
    c = g_lastCast;
    Unlock(g_hitLock);
    StoredTarget s;
    const bool haveStored = ReadStored(ctrl, s);
    const ULONGLONG now = GetTickCount64();
    // Run 4: 17 of 18 commits had no cast within 250 ms - the aim stops updating before the commit (candidate: the
    // reticle skips +D76000 while ctrl+195 is set, +5AAD70 :52). So the last cast IS where the aim froze: compare it
    // however old, and print its age, +195, and how far the hand has turned since (OpenXR forward, no mapping needed).
    const bool castFresh = c.valid;
    int freeze = -1;
    __try {
        freeze = static_cast<const unsigned char*>(ctrl)[195];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        freeze = -1;
    }
    float handTurn = -1.0f;
    XrPosef nowPose;
    if (c.valid && GetControllerPose(AimHand(), nowPose)) {
        float fx, fy, fz;
        RotateByQuat(nowPose.orientation, 0.0f, 0.0f, -1.0f, fx, fy, fz);
        float cs = fx * c.ctlFwd[0] + fy * c.ctlFwd[1] + fz * c.ctlFwd[2];
        cs = cs > 1.0f ? 1.0f : (cs < -1.0f ? -1.0f : cs);
        handTurn = acosf(cs) * 57.2957795f;
    }
    const bool none = !haveStored ||
                      (s.hit[0] == 0.0f && s.hit[1] == 0.0f && s.hit[2] == 0.0f) ||
                      (s.nrm[0] == 0.0f && s.nrm[1] == 0.0f && s.nrm[2] == 0.0f);
    float d = -1.0f, ang = -1.0f;
    if (castFresh && !none) {
        if (c.hit) {
            const float dx = s.hit[0] - c.at[0], dy = s.hit[1] - c.at[1], dz = s.hit[2] - c.at[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        const float vx = s.hit[0] - c.hand[0], vy = s.hit[1] - c.hand[1], vz = s.hit[2] - c.hand[2];
        const float vl = sqrtf(vx * vx + vy * vy + vz * vz);
        if (vl > 1e-3f) {
            float cs = (vx * c.dir[0] + vy * c.dir[1] + vz * c.dir[2]) / vl;
            cs = cs > 1.0f ? 1.0f : (cs < -1.0f ? -1.0f : cs);
            ang = acosf(cs) * 57.2957795f;
        }
    }
    InterlockedIncrement(&g_throwCommits);
    Lock(g_winLock);
    if (castFresh && c.bucket >= 0) { ++g_elThrows[c.bucket]; }
    Unlock(g_winLock);
    const LONG tn = InterlockedIncrement(&g_throwLines);
    if (tn > kMaxThrowLines) {
        return;
    }
    char aimed[256], dist[128];
    if (castFresh) {
        _snprintf_s(aimed, sizeof(aimed), _TRUNCATE, "last hand ray cast (= where the aim froze) %s at (%.2f, %.2f, %.2f), "
                    "%llu ms before the commit, elevation vs head %s | hand turned %s%.1f deg since | ctrl+195 %d",
                    c.hit ? "HIT" : "MISSED (ray end)", c.at[0], c.at[1], c.at[2],
                    static_cast<unsigned long long>(now - c.tick), c.bucket >= 0 ? kElevNames[c.bucket] : "no head",
                    handTurn < 0.0f ? "n/a " : "", handTurn < 0.0f ? 0.0f : handTurn, freeze);
    } else {
        _snprintf_s(aimed, sizeof(aimed), _TRUNCATE, "NO VERDICT - no grapple cast recorded yet | ctrl+195 %d", freeze);
    }
    if (d >= 0.0f) {
        _snprintf_s(dist, sizeof(dist), _TRUNCATE, "aimed-to-target %.2f m, %.1f deg off the hand ray%s", d,
                    ang < 0.0f ? 0.0f : ang, d > 0.75f ? "  *** BEYOND THE 0.75 m STICKY RADIUS ***" : "");
    } else if (ang >= 0.0f) {
        _snprintf_s(dist, sizeof(dist), _TRUNCATE, "cast missed; target %.1f deg off the hand ray (KEPT from earlier)",
                    ang);
    } else {
        _snprintf_s(dist, sizeof(dist), _TRUNCATE, "distance: NO VERDICT");
    }
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] THROW COMMIT #%ld (+D7A260 state -> 3) | AIMED: %s | TARGET committed: %s "
        "(%.2f, %.2f, %.2f) %s | %s%s",
        tn, aimed, none ? "NONE" : "at", s.hit[0], s.hit[1], s.hit[2],
        none ? "" : (s.invalid == 0 ? "USABLE" : "NOT USABLE"), dist,
        tn == kMaxThrowLines ? "  (line cap reached - further throws are counted in the window only)" : "");
}

// ---------------------------------------------------------------- the VAULT redirect (+D7CC20), probe only
// INV-273 (gr16-18): while attached, +D782E0 calls +D7CC20 every update until the climbable-top detector finds a ledge
// within 3.1 m above the player, then writes the PULL target rope+408..+416 (point + 0.35 v1 + 0.75 v2) and the pull goes
// there instead of the hook. Detour (gr21: prologue mov [rsp+20h],rbx | push rbp | lea rbp,[rsp-57h] | sub rsp,0A0h =
// 18 relocatable bytes) - pass-through; logs the first update +408 becomes non-zero after a throw commit: the vault
// point vs the committed hook point, and the time since the commit.
using VaultFn = void(__fastcall*)(void* ctrl);
VaultFn g_trueVault = nullptr;
constexpr uintptr_t kVaultRva = 0xD7CC20;
const unsigned char kVaultBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x48, 0x8D, 0x6C,
                                      0x24, 0xA9, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 };
volatile LONG g_vaultLines = 0, g_vaults = 0, g_vaultCalls = 0;
ULONGLONG g_lastCommitTick = 0;
float     g_lastHook[3] = {};
bool      g_lastHookOk = false;
long      g_throwSeq = 0, g_vaultedSeq = -1;

bool ReadVec(const void* ctrl, int off, float v[3])
{
    __try {
        const float* f = reinterpret_cast<const float*>(static_cast<const unsigned char*>(ctrl) + off);
        v[0] = f[0]; v[1] = f[1]; v[2] = f[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// F8 (docs/AUDIT-COOP-PER-PLAYER-2026-11-13.md, trace 20261113_1810): in co-op this machine also runs the OTHER player's rope
// controller; its throws and vaults must not be recorded as ours. A PlayerController's vfunc +32 = its PlayerDI (the getter
// LookController uses; INV-594 for the controllers) == the game's local player when it is ours. Unreadable -> true = as before.
// COST per rope state change / vault: one guarded vcall + 4 guarded reads.
volatile LONG g_ropeRemote = 0, g_ropeSaidRemote = 0;
bool RopeCtlIsLocal(void* ctrl)
{
    int why = -1;
    const uintptr_t local = RigGamePlayerObject(&why);
    if (why != 0 || !local || !ctrl) { return true; }
    uintptr_t player = 0;
    __try {
        using Fn = uintptr_t(__fastcall*)(void*);
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ctrl);
        player = (*reinterpret_cast<Fn const*>(vt + 32))(ctrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    if (!player) { return true; }
    if (player == local) { return true; }
    InterlockedIncrement(&g_ropeRemote);
    if (InterlockedExchange(&g_ropeSaidRemote, 1) == 0) {
        // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
        LOG("grappleaim: [GRAPPLEAIM] REMOTE player's rope controller %p (player %p, local %p) - its throws / vaults are not "
            "recorded (co-op: local player only; logged once)", ctrl, reinterpret_cast<void*>(player), reinterpret_cast<void*>(local));
    }
    return false;
}

void __fastcall VaultHook(void* ctrl)
{
    float before[3] = {};
    const bool rb = ReadVec(ctrl, 408, before);
    g_trueVault(ctrl);
    InterlockedIncrement(&g_vaultCalls);
    if (!RopeCtlIsLocal(ctrl)) {
        return;
    }
    float after[3] = {};
    if (!rb || !ReadVec(ctrl, 408, after)) {
        return;
    }
    const bool had = before[0] != 0.0f || before[1] != 0.0f || before[2] != 0.0f;
    const bool now = after[0] != 0.0f || after[1] != 0.0f || after[2] != 0.0f;
    if (had || !now) {
        return;
    }
    InterlockedIncrement(&g_vaults);
    float anchor[3] = {};
    const bool ra = ReadVec(ctrl, 108, anchor);
    const ULONGLONG t = GetTickCount64();
    const long seq = g_throwSeq;
    const bool first = seq != g_vaultedSeq;
    g_vaultedSeq = seq;
    const LONG ln = InterlockedIncrement(&g_vaultLines);
    if (ln > 300) {
        return;
    }
    char hk[160];
    if (g_lastHookOk) {
        const float dx = after[0] - g_lastHook[0], dy = after[1] - g_lastHook[1], dz = after[2] - g_lastHook[2];
        _snprintf_s(hk, sizeof(hk), _TRUNCATE, "hook (committed) (%.2f, %.2f, %.2f) -> vault point %.2f m away, %+.2f m "
                    "higher, %.2f m sideways", g_lastHook[0], g_lastHook[1], g_lastHook[2], sqrtf(dx * dx + dy * dy + dz * dz),
                    dy, sqrtf(dx * dx + dz * dz));
    } else {
        _snprintf_s(hk, sizeof(hk), _TRUNCATE, "no committed hook recorded - NO VERDICT on the distance");
    }
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] VAULT #%ld (throw #%ld%s) %llu ms after the commit | the pull is REDIRECTED to (%.2f, %.2f, "
        "%.2f) | %s | anchor rope+108 %s(%.2f, %.2f, %.2f)%s",
        ln, seq, first ? "" : ", AGAIN for the same throw", static_cast<unsigned long long>(g_lastCommitTick ? t - g_lastCommitTick : 0),
        after[0], after[1], after[2], hk, ra ? "" : "UNREADABLE ", anchor[0], anchor[1], anchor[2],
        ln == 300 ? "  (line cap reached)" : "");
}

void __fastcall CommitHook(void* ctrl, int state)
{
    if (state == 3 && RopeCtlIsLocal(ctrl)) {
        int cur = -1;
        if (ReadState(ctrl, cur) && cur != 3) {   // +D7A260 itself returns early when the state is unchanged
            ThrowEvent(ctrl);
            float h[3] = {};
            g_lastHookOk = ReadVec(ctrl, 136, h);
            g_lastHook[0] = h[0]; g_lastHook[1] = h[1]; g_lastHook[2] = h[2];
            g_lastCommitTick = GetTickCount64();
            ++g_throwSeq;
        }
    }
    g_trueCommit(ctrl, state);
}

// ---------------------------------------------------------------- REFUSAL GATES, re-run by the probe (INV-273 gr22-29)
// Jay 2026-09-27: "If I aim at a surface ... that surface should be what hooks if it's within range." For STATIC geometry
// +D76000 refuses a hit by exactly: the flags (+0x3A & 0x1100, already logged), (a) the level-volume list, (b) the level
// zone query, (c) the second cast's type word == 3, (d) on a level of type 1 the DisableRopeClimbOnWasteland mesh
// property. (c) is recorded passively (RaytraceHook, +D76D85). (a)/(b) are called here, the way +D76000 calls them
// (gr22 (1), instruction level): level = ctrl->vfunc(+0x40) (+D76977); for each entry of level+0x10A0 (count +0x10A8):
// entry+0x110 -> slot 0 (obj, &point, xmm2 = [gamedll+0x1927B24], -1.0 in gr30) -> TRUE = refused (+D769B5); then
// level+0xAD0 -> slot 2 (obj, 2, &point, 0) -> 1 or 2 = refused (+D769DF..+D769E4). Read-only queries, on the thread
// that runs +D76000, at most every 50 ms per thread. (d): the level type is logged ([[game+1344]+4692], +D76773); the
// mesh property itself is not re-queried.
using CtrlLevelFn = void*(__fastcall*)(void* ctrl);
using VolFn  = unsigned char(__fastcall*)(void* obj, const float* p, float arg);
using ZoneFn = int(__fastcall*)(void* obj, int mode, const float* p, int zero);
constexpr uintptr_t kVolArgRva = 0x1927B24;   // flt_181927B24, the xmm2 argument of each volume check (gr22/gr30)
float g_volArg = -1.0f;
bool  g_volArgOk = false;

struct GateResult {
    bool      levelOk;
    int       listN;        // entries in the level-volume list
    int       listHit;      // first entry that claimed the point, -1 = none
    uintptr_t listEntry;    // that entry
    int       zone;         // the zone query's answer; -99 = no zone object
    uintptr_t zoneObj;
    int       levelType;    // [[game+1344]+4692]; 1 = the wasteland path; -1 = unreadable
    bool      fault;
};

void GateCheck(void* ctrl, const float at[3], GateResult& g)
{
    g.levelOk = false;
    g.listN = 0;
    g.listHit = -1;
    g.listEntry = 0;
    g.zone = -99;
    g.zoneObj = 0;
    g.levelType = -1;
    g.fault = false;
    alignas(16) float p[4] = { at[0], at[1], at[2], 0.0f };
    __try {
        const uintptr_t gm = *reinterpret_cast<const uintptr_t*>(g_gd + g_rCamGlobal);
        const uintptr_t lv = gm ? *reinterpret_cast<const uintptr_t*>(gm + 1344) : 0;
        if (lv) { g.levelType = *reinterpret_cast<const int*>(lv + 4692); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g.levelType = -1;
    }
    __try {
        CtrlLevelFn getLevel = (*reinterpret_cast<CtrlLevelFn* const*>(ctrl))[8];
        const uintptr_t level = reinterpret_cast<uintptr_t>(getLevel(ctrl));
        if (!level) { return; }
        g.levelOk = true;
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(level + 0x10A0);
        const unsigned int n = *reinterpret_cast<const unsigned int*>(level + 0x10A8);
        g.listN = static_cast<int>(n);
        for (unsigned int i = 0; arr && i < n && i < 256; ++i) {
            const uintptr_t e = *reinterpret_cast<const uintptr_t*>(arr + 8ull * i);
            void* obj = reinterpret_cast<void*>(e + 0x110);
            VolFn fn = **reinterpret_cast<VolFn* const*>(obj);
            if (fn(obj, p, g_volArg)) {
                g.listHit = static_cast<int>(i);
                g.listEntry = e;
                break;
            }
        }
        const uintptr_t z = *reinterpret_cast<const uintptr_t*>(level + 0xAD0);
        g.zoneObj = z;
        if (z) {
            ZoneFn zf = (*reinterpret_cast<ZoneFn* const*>(z))[2];
            g.zone = zf(reinterpret_cast<void*>(z), 2, p, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g.fault = true;
    }
}

// Window tallies (under g_winLock), [0] = hit with no usable target (flagged or not), [1] = an older target held.
long g_gtChecked[2] = {}, g_gtVol[2] = {}, g_gtZone[2] = {}, g_gtType3[2] = {}, g_gtWaste[2] = {}, g_gtNone[2] = {};
long g_gtFault[2] = {}, g_gtNoLevel[2] = {}, g_gtMoved2[2] = {}, g_gtSkipped = 0;
char g_gtVolClass[96] = "-", g_gtZoneClass[96] = "-";
volatile LONG g_gtFirstSaid = 0;
thread_local ULONGLONG t_lastGateRun = 0;

// ---------------------------------------------------------------- PULL TRACE (INV-273 gr26-29)
// What moves a human toward the hook when the vault search finds nothing is [UNKNOWN] statically: the Connected update
// (+D78C80) reaches its end block for a hook on static geometry and hands off to sub_180D4D330(..., 24), not read. So
// log where the player actually GOES: a detour on the rope controller's per-update dispatcher +D74AF0 (slot 16; gr30:
// prologue mov [rsp+8],rbx | mov [rsp+10h],rsi | push rdi | sub rsp,70h = 15 relocatable bytes; referenced only by the
// vtable and .pdata) - pass-through; for the controller the grapple aims with, every 100 ms from the first state change
// away from 0 until 1.5 s after it returns to 0 (or 8 s): state, player position (owner vfunc +624, the position +D76000
// itself reads), velocity (owner vfunc +440, used as the velocity by +D7A320 case 3), distance to the hook (ctrl+136 at
// Connected) and whether a vault target (ctrl+408) is set. If the dispatcher stops being called, the reticle's own
// +D76000 calls keep sampling and the summary names it. One SUMMARY line per throw.
using DispatchFn = void*(__fastcall*)(void* ctrl, float dt);
DispatchFn g_trueDispatch = nullptr;
constexpr uintptr_t kDispatchRva = 0xD74AF0;
const unsigned char kDispatchBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                         0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x70 };
volatile uintptr_t g_aimCtrl = 0;   // the controller +D76000 aims with (set at the aim-ray origin call)
volatile LONG g_dpOurs = 0, g_dpOther = 0, g_dpNoAim = 0;
volatile LONG g_trLock = 0;
constexpr LONG kMaxPullSampleTraces = 30, kMaxPullSummaries = 200;
volatile LONG g_trSummaries = 0;
struct PullTrace {
    bool      on;
    long      seq;
    ULONGLONG t0, lastSample, lastDispatch, zeroSince;
    int       lastState;
    char      states[200];
    int       statesLen;
    bool      haveStart;
    float     start[3], last[3];
    bool      haveLast;
    bool      hookOk;
    float     hook[3];
    bool      anchorOk;
    float     anchor[3];
    float     minD;
    ULONGLONG minT;
    bool      vaultSeen;
    float     vault[3];
    ULONGLONG vaultT;
    int       vaultChanges;
    int       samples, samplesDisp, samplesRet;
    float     maxSpeed;
};
PullTrace g_tr = {};
long      g_trSeq = 0;

bool OwnerKinematics(void* ctrl, float p[3], float v[3], bool& haveV)
{
    haveV = false;
    __try {
        using OwnerFn = void*(__fastcall*)(void* ctrl);
        using VecFn = float*(__fastcall*)(void* obj, float* out);
        OwnerFn getOwner = (*reinterpret_cast<OwnerFn* const*>(ctrl))[4];
        void* owner = getOwner(ctrl);
        if (!owner) { return false; }
        alignas(16) float b[8] = {};
        VecFn pos = (*reinterpret_cast<VecFn* const*>(owner))[624 / 8];
        const float* rp = pos(owner, b);
        if (!rp) { rp = b; }
        p[0] = rp[0]; p[1] = rp[1]; p[2] = rp[2];
        alignas(16) float c[8] = {};
        VecFn vel = (*reinterpret_cast<VecFn* const*>(owner))[440 / 8];
        const float* rv = vel(owner, c);
        if (!rv) { rv = c; }
        v[0] = rv[0]; v[1] = rv[1]; v[2] = rv[2];
        haveV = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AppendState(int st, ULONGLONG ms)
{
    if (g_tr.statesLen >= static_cast<int>(sizeof(g_tr.states)) - 16) { return; }
    const int k = _snprintf_s(g_tr.states + g_tr.statesLen, sizeof(g_tr.states) - g_tr.statesLen, _TRUNCATE, "%s%d@%llu",
                              g_tr.statesLen ? " " : "", st, static_cast<unsigned long long>(ms));
    if (k > 0) { g_tr.statesLen += k; }
}

void FinishTrace(const char* why)   // under g_trLock
{
    g_tr.on = false;
    const LONG sn = InterlockedIncrement(&g_trSummaries);
    if (sn > kMaxPullSummaries) { return; }
    char hk[200], ev[200], vt[200];
    if (g_tr.hookOk) {
        _snprintf_s(hk, sizeof(hk), _TRUNCATE, "hook (ctrl+136 at Connected) (%.2f, %.2f, %.2f)", g_tr.hook[0], g_tr.hook[1],
                    g_tr.hook[2]);
    } else {
        _snprintf_s(hk, sizeof(hk), _TRUNCATE, "hook: NONE captured (never reached state 3 with a stored target)");
    }
    if (g_tr.hookOk && g_tr.haveLast) {
        const float dx = g_tr.last[0] - g_tr.hook[0], dy = g_tr.last[1] - g_tr.hook[1], dz = g_tr.last[2] - g_tr.hook[2];
        _snprintf_s(ev, sizeof(ev), _TRUNCATE, "END vs hook %.2f m (height %+.2f, horizontal %.2f) | closest %.2f m at +%llu ms",
                    sqrtf(dx * dx + dy * dy + dz * dz), dy, sqrtf(dx * dx + dz * dz), g_tr.minD,
                    static_cast<unsigned long long>(g_tr.minT));
    } else {
        _snprintf_s(ev, sizeof(ev), _TRUNCATE, "END vs hook: NO VERDICT (%s)", g_tr.hookOk ? "no position sample" : "no hook");
    }
    if (g_tr.vaultSeen) {
        const float dx = g_tr.vault[0] - g_tr.hook[0], dy = g_tr.vault[1] - g_tr.hook[1], dz = g_tr.vault[2] - g_tr.hook[2];
        _snprintf_s(vt, sizeof(vt), _TRUNCATE, "vault target SET first at +%llu ms (%.2f, %.2f, %.2f)%s%.2f m from the hook, "
                    "changed %d time(s)", static_cast<unsigned long long>(g_tr.vaultT), g_tr.vault[0], g_tr.vault[1],
                    g_tr.vault[2], g_tr.hookOk ? ", " : " (no hook) ", g_tr.hookOk ? sqrtf(dx * dx + dy * dy + dz * dz) : 0.0f,
                    g_tr.vaultChanges);
    } else {
        _snprintf_s(vt, sizeof(vt), _TRUNCATE, "vault target NEVER set (in %d samples)", g_tr.samples);
    }
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] PULL #%ld SUMMARY (%s) | states (state@ms) %s | %s | start (%.2f, %.2f, %.2f) -> end "
        "(%.2f, %.2f, %.2f) | %s | %s | max speed %.1f m/s | samples %d (dispatcher %d, reticle %d)%s",
        g_tr.seq, why, g_tr.statesLen ? g_tr.states : "-", hk, g_tr.start[0], g_tr.start[1], g_tr.start[2], g_tr.last[0],
        g_tr.last[1], g_tr.last[2], ev, vt, g_tr.maxSpeed, g_tr.samples, g_tr.samplesDisp, g_tr.samplesRet,
        sn == kMaxPullSummaries ? "  (summary cap reached)" : "");
}

// Called on the game thread from the dispatcher detour (fromDispatch) and from the reticle's +D76000 origin call.
void TraceTick(void* ctrl, bool fromDispatch)
{
    const ULONGLONG now = GetTickCount64();
    int state = -1;
    const bool haveState = fromDispatch && ReadState(ctrl, state);
    bool sample = false;
    Lock(g_trLock);
    if (fromDispatch) {
        g_tr.lastDispatch = now;
        if (haveState) {
            if (!g_tr.on && state != 0) {
                const long seq = ++g_trSeq;
                g_tr = {};
                g_tr.on = true;
                g_tr.seq = seq;
                g_tr.t0 = now;
                g_tr.lastDispatch = now;
                g_tr.lastState = state;
                g_tr.minD = 1e9f;
                AppendState(state, 0);
                sample = true;
            } else if (g_tr.on) {
                if (state != g_tr.lastState) {
                    AppendState(state, now - g_tr.t0);
                    g_tr.lastState = state;
                    sample = true;
                }
                g_tr.zeroSince = state == 0 ? (g_tr.zeroSince ? g_tr.zeroSince : now) : 0;
                if (!g_tr.hookOk && state == 3) {
                    float h[3], n[3];
                    if (ReadVec(ctrl, 136, h) && ReadVec(ctrl, 148, n) && (h[0] != 0.0f || h[1] != 0.0f || h[2] != 0.0f)) {
                        g_tr.hookOk = true;
                        g_tr.hook[0] = h[0]; g_tr.hook[1] = h[1]; g_tr.hook[2] = h[2];
                    }
                    g_tr.anchorOk = ReadVec(ctrl, 108, g_tr.anchor);
                }
                float vp[3];
                if (ReadVec(ctrl, 408, vp) && (vp[0] != 0.0f || vp[1] != 0.0f || vp[2] != 0.0f)) {
                    if (!g_tr.vaultSeen) {
                        g_tr.vaultSeen = true;
                        g_tr.vaultT = now - g_tr.t0;
                        g_tr.vault[0] = vp[0]; g_tr.vault[1] = vp[1]; g_tr.vault[2] = vp[2];
                    } else if (fabsf(vp[0] - g_tr.vault[0]) + fabsf(vp[1] - g_tr.vault[1]) + fabsf(vp[2] - g_tr.vault[2]) > 0.05f) {
                        ++g_tr.vaultChanges;
                        g_tr.vault[0] = vp[0]; g_tr.vault[1] = vp[1]; g_tr.vault[2] = vp[2];
                    }
                }
            }
        }
    }
    if (!g_tr.on) {
        Unlock(g_trLock);
        return;
    }
    // Only the dispatcher samples while it runs; the reticle takes over when it has been silent for 300 ms.
    if (!fromDispatch && now - g_tr.lastDispatch < 300) {
        Unlock(g_trLock);
        return;
    }
    if (now - g_tr.lastSample >= 100) { sample = true; }
    if (sample) { g_tr.lastSample = now; }
    Unlock(g_trLock);
    float p[3] = {}, v[3] = {};
    bool haveV = false;
    const bool haveP = sample && OwnerKinematics(ctrl, p, v, haveV);
    Lock(g_trLock);
    if (!g_tr.on) {
        Unlock(g_trLock);
        return;
    }
    if (haveP) {
        if (!g_tr.haveStart) {
            g_tr.haveStart = true;
            g_tr.start[0] = p[0]; g_tr.start[1] = p[1]; g_tr.start[2] = p[2];
        }
        g_tr.haveLast = true;
        g_tr.last[0] = p[0]; g_tr.last[1] = p[1]; g_tr.last[2] = p[2];
        ++g_tr.samples;
        ++(fromDispatch ? g_tr.samplesDisp : g_tr.samplesRet);
        const float sp = haveV ? sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) : 0.0f;
        if (sp > g_tr.maxSpeed) { g_tr.maxSpeed = sp; }
        float d = -1.0f, dh = 0.0f, dxz = 0.0f;
        if (g_tr.hookOk) {
            const float dx = p[0] - g_tr.hook[0], dy = p[1] - g_tr.hook[1], dz = p[2] - g_tr.hook[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            dh = dy;
            dxz = sqrtf(dx * dx + dz * dz);
            if (d < g_tr.minD) { g_tr.minD = d; g_tr.minT = now - g_tr.t0; }
        }
        if (g_tr.seq <= kMaxPullSampleTraces) {
            char hd[96];
            if (d >= 0.0f) {
                _snprintf_s(hd, sizeof(hd), _TRUNCATE, "to hook %.2f m (height %+.2f, horizontal %.2f)", d, dh, dxz);
            } else {
                _snprintf_s(hd, sizeof(hd), _TRUNCATE, "to hook: none captured yet");
            }
            // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
            LOG("grappleaim: [GRAPPLEAIM] PULL #%ld +%llu ms (%s) state %d | player (%.2f, %.2f, %.2f) | %s | velocity %s(%.1f, "
                "%.1f, %.1f) %.1f m/s | vault target %s", g_tr.seq, static_cast<unsigned long long>(now - g_tr.t0),
                fromDispatch ? "dispatcher" : "reticle - dispatcher silent", fromDispatch ? g_tr.lastState : -1, p[0], p[1],
                p[2], hd, haveV ? "" : "UNREAD ", v[0], v[1], v[2], sp, g_tr.vaultSeen ? "set" : "none");
        }
    } else if (sample) {
        if (g_tr.seq <= kMaxPullSampleTraces) {
            // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
            LOG("grappleaim: [GRAPPLEAIM] PULL #%ld +%llu ms - position NOT READ (owner null or the vfunc faulted)", g_tr.seq,
                static_cast<unsigned long long>(now - g_tr.t0));
        }
    }
    if (g_tr.zeroSince && now - g_tr.zeroSince >= 1500) {
        FinishTrace("1.5 s after the rope returned to state 0");
    } else if (now - g_tr.t0 >= 8000) {
        FinishTrace("8 s cap");
    } else if (!fromDispatch && now - g_tr.lastDispatch >= 1500 && g_tr.lastState == 0) {
        FinishTrace("dispatcher silent 1.5 s after state 0");
    }
    Unlock(g_trLock);
}

void LogGateAndPullWindow()   // under g_winLock (MaybeLogWindow)
{
    static const char* const kGk[2] = { "no usable target", "older target held" };
    char gt[2][420];
    for (int k = 0; k < 2; ++k) {
        if (g_gtChecked[k] > 0) {
            _snprintf_s(gt[k], sizeof(gt[k]), _TRUNCATE, "%s: re-checked %ld -> volume list %ld, zone query %ld, 2nd-cast "
                        "type 3 %ld, wasteland level %ld, NONE of these %ld | no level %ld, fault %ld | 2nd cast moved the hit "
                        "> 5 cm %ld", kGk[k], g_gtChecked[k], g_gtVol[k], g_gtZone[k], g_gtType3[k], g_gtWaste[k], g_gtNone[k],
                        g_gtNoLevel[k], g_gtFault[k], g_gtMoved2[k]);
        } else {
            _snprintf_s(gt[k], sizeof(gt[k]), _TRUNCATE, "%s: re-checked 0 - NO VERDICT", kGk[k]);
        }
    }
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] window REFUSAL GATES (re-run by the probe, max 1 per 50 ms per thread; not re-run %ld) | "
        "%s | %s | last volume class %s, last zone class %s | PULL dispatcher calls: grapple ctrl %ld, other ctrls %ld, "
        "before any aim %ld", g_gtSkipped, gt[0], gt[1], g_gtVolClass, g_gtZoneClass, InterlockedExchange(&g_dpOurs, 0),
        InterlockedExchange(&g_dpOther, 0), InterlockedExchange(&g_dpNoAim, 0));
    for (int k = 0; k < 2; ++k) {
        g_gtChecked[k] = g_gtVol[k] = g_gtZone[k] = g_gtType3[k] = g_gtWaste[k] = g_gtNone[k] = 0;
        g_gtFault[k] = g_gtNoLevel[k] = g_gtMoved2[k] = 0;
    }
    g_gtSkipped = 0;
}

void* __fastcall DispatchHook(void* ctrl, float dt)
{
    void* r = g_trueDispatch(ctrl, dt);
    const uintptr_t aim = g_aimCtrl;
    if (!aim) {
        InterlockedIncrement(&g_dpNoAim);
    } else if (reinterpret_cast<uintptr_t>(ctrl) != aim) {
        InterlockedIncrement(&g_dpOther);
    } else {
        InterlockedIncrement(&g_dpOurs);
        TraceTick(ctrl, true);
    }
    return r;
}

// AIMED vs TARGET, at the start of a +D76000 call (the aim-ray origin call): the target the PREVIOUS call on this
// thread left at ctrl+136 against the hand ray's cast in that same previous call.
void ProbeAimVsTarget(void* ctrl)
{
    if (!DiagGrappleAim) {
        return;
    }
    g_aimCtrl = reinterpret_cast<uintptr_t>(ctrl);   // the pull trace follows this controller
    if (g_trueDispatch) {
        TraceTick(ctrl, false);
    }
    int state = -1;
    const bool haveState = ReadState(ctrl, state);
    StoredTarget s;
    const bool haveStored = ReadStored(ctrl, s);
    const CastRec c = t_cast;
    const ULONGLONG now = GetTickCount64();
    const bool castFresh = c.valid && now - c.tick <= kFreshMs;
    const bool none = !haveStored ||
                      (s.hit[0] == 0.0f && s.hit[1] == 0.0f && s.hit[2] == 0.0f) ||
                      (s.nrm[0] == 0.0f && s.nrm[1] == 0.0f && s.nrm[2] == 0.0f);
    float d = -1.0f, ang = -1.0f;
    if (castFresh && !none) {
        if (c.hit) {
            const float dx = s.hit[0] - c.at[0], dy = s.hit[1] - c.at[1], dz = s.hit[2] - c.at[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        const float vx = s.hit[0] - c.hand[0], vy = s.hit[1] - c.hand[1], vz = s.hit[2] - c.hand[2];
        const float vl = sqrtf(vx * vx + vy * vy + vz * vz);
        if (vl > 1e-3f) {
            float cs = (vx * c.dir[0] + vy * c.dir[1] + vz * c.dir[2]) / vl;
            cs = cs > 1.0f ? 1.0f : (cs < -1.0f ? -1.0f : cs);
            ang = acosf(cs) * 57.2957795f;
        }
    }
    // REFUSAL GATES: re-run the level checks for a refused hit (no usable target, or an older one held), outside the
    // window lock (they call into the game), at most every 50 ms on this thread.
    const int gk = (castFresh && c.hit) ? ((none || s.invalid) ? 0 : (d > 0.25f ? 1 : -1)) : -1;
    GateResult gr = {};
    bool gateRan = false;
    if (gk >= 0 && now - t_lastGateRun >= 50) {
        t_lastGateRun = now;
        GateCheck(ctrl, c.at, gr);
        gateRan = true;
    }
    const bool type3 = c.t2Ran && c.t2Hit && c.t2Type == 3;
    float moved2 = -1.0f;
    if (c.t2Ran && c.t2Hit) {
        const float mx = c.t2At[0] - c.at[0], my = c.t2At[1] - c.at[1], mz = c.t2At[2] - c.at[2];
        moved2 = sqrtf(mx * mx + my * my + mz * mz);
    }
    char gateTxt[320] = "gates: not re-run (50 ms throttle)";
    if (gateRan) {
        char vc[96] = "-", zc[96] = "-";
        if (gr.listHit >= 0) { RttiClassOf(gr.listEntry, vc, sizeof(vc)); }
        if (gr.zoneObj) { RttiClassOf(gr.zoneObj, zc, sizeof(zc)); }
        const bool waste = gr.levelType == 1;
        const bool anyGate = gr.listHit >= 0 || gr.zone == 1 || gr.zone == 2 || type3 || waste;
        char claim[128];
        if (gr.listHit >= 0) {
            _snprintf_s(claim, sizeof(claim), _TRUNCATE, "entry #%d (%s)", gr.listHit, vc);
        } else {
            _snprintf_s(claim, sizeof(claim), _TRUNCATE, "none");
        }
        _snprintf_s(gateTxt, sizeof(gateTxt), _TRUNCATE, "GATES: %s%svolume list %d entries, claimed by %s | zone query %s%d "
                    "(1/2 = refuse; object %s) | 2nd cast %s, type %u (3 = refuse), %.2f m from the first hit | level type %d%s",
                    gr.fault ? "FAULT during the re-check - " : "", !gr.levelOk ? "NO LEVEL - " : "", gr.listN, claim,
                    gr.zone == -99 ? "no object " : "", gr.zone == -99 ? 0 : gr.zone, zc,
                    !c.t2Ran ? "not recorded" : (c.t2Hit ? "hit" : "missed"), static_cast<unsigned>(c.t2Type),
                    moved2, gr.levelType, waste ? " (the wasteland path: mesh property not re-queried)" : "");
        if (!anyGate && !gr.fault && gr.levelOk) {
            const size_t len = strlen(gateTxt);
            _snprintf_s(gateTxt + len, sizeof(gateTxt) - len, _TRUNCATE, " | NONE of the re-checked gates claims this hit");
        }
        if (InterlockedCompareExchange(&g_gtFirstSaid, 1, 0) == 0) {
            // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
            LOG("grappleaim: [GRAPPLEAIM] FIRST GATE RE-CHECK (logged regardless of throttles) %s hit at (%.2f, %.2f, %.2f) | %s "
                "| volume arg %.3f %s | 2nd cast point %s(%.2f, %.2f, %.2f), %.2f m from the first",
                gk == 0 ? "no-usable-target" : "older-target-held", c.at[0], c.at[1], c.at[2], gateTxt, g_volArg,
                g_volArgOk ? "(read)" : "(DEFAULT - not read)", c.t2Ran ? "" : "NOT RECORDED ", c.t2At[0], c.t2At[1],
                c.t2At[2], moved2);
        }
    }
    Lock(g_winLock);
    if (gk >= 0 && !gateRan) { ++g_gtSkipped; }
    if (gateRan) {
        ++g_gtChecked[gk];
        if (gr.fault) { ++g_gtFault[gk]; }
        if (!gr.levelOk) { ++g_gtNoLevel[gk]; }
        if (gr.listHit >= 0) {
            ++g_gtVol[gk];
            RttiClassOf(gr.listEntry, g_gtVolClass, sizeof(g_gtVolClass));
        }
        if (gr.zone == 1 || gr.zone == 2) {
            ++g_gtZone[gk];
            RttiClassOf(gr.zoneObj, g_gtZoneClass, sizeof(g_gtZoneClass));
        }
        if (type3) { ++g_gtType3[gk]; }
        if (gr.levelType == 1) { ++g_gtWaste[gk]; }
        if (moved2 > 0.05f) { ++g_gtMoved2[gk]; }
        if (!gr.fault && gr.levelOk && gr.listHit < 0 && gr.zone != 1 && gr.zone != 2 && !type3 && gr.levelType != 1) {
            ++g_gtNone[gk];
        }
    }
    if (haveState) { ++g_state[(state >= 0 && state <= 7) ? state : 8]; }
    if (castFresh && c.bucket >= 0) {
        if (none) { ++g_elNone[c.bucket]; }
        else if (s.invalid == 0) { ++g_elUsable[c.bucket]; }
        else { ++g_elUnusable[c.bucket]; }
    }
    if (castFresh) {   // LEDGE: the previous call's assist facing bucket vs the target it left
        const int fb = (t_prevAssistBucket >= 0 && t_prevAssistBucket <= kFaceBuckets) ? t_prevAssistBucket : kFaceBuckets;
        ++g_laOut[fb][none ? 2 : (s.invalid == 0 ? 0 : 1)];
    }
    if (!castFresh) {
        ++g_avStale;
    } else if (none) {
        ++g_avNone;
    } else {
        ++g_avPairs;
        const bool usable = s.invalid == 0;
        if (usable) { ++g_avUsable; } else { ++g_avUnusable; }
        if (!c.hit) {
            ++g_avCastMiss;
            if (usable) { ++g_avMissKeptUsable; }
        } else {
            int bin = static_cast<int>(d / 0.05f);
            bin = bin < 0 ? 0 : (bin > 200 ? 200 : bin);
            ++g_avDist[bin];
            if (d > g_avMaxD) { g_avMaxD = d; }
            if (d < 0.05f) { ++g_avOnRay; } else if (d <= 0.75f) { ++g_avSticky; } else { ++g_avBeyond; }
            if (usable && d >= 0.05f) { ++g_avUsableOff; }
        }
        if (ang >= 0.0f) {
            int b = static_cast<int>(ang);
            b = b > 179 ? 179 : b;
            ++g_avAng[b];
        }
    }
    // REFUSAL events. 0 = hit a FLAGGED (not-grappleable) surface and no usable target; 1 = hit an unflagged surface and
    // still no usable target; 2 = a usable target is held but it is an OLDER one, > 0.25 m from this hit; 3 = a usable
    // target on this hit, but the icon's gate +D75DF0 says no (checked at most every 100 ms per thread).
    int cat = -1;
    int gate = -1;
    if (castFresh && c.hit && (none || s.invalid)) {
        cat = (c.recOk && (c.flags & 0x1100)) ? 0 : 1;
    } else if (castFresh && c.hit && !none && !s.invalid && d > 0.25f) {
        cat = 2;
    } else if (castFresh && !none && !s.invalid && g_iconGate && now - t_lastGateCheck >= 100) {
        t_lastGateCheck = now;
        InterlockedIncrement(&g_rfIconChecks);
        __try {
            gate = g_iconGate(ctrl) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            gate = -2;
        }
        if (gate == 0) { cat = 3; }
    }
    if (cat >= 0) {
        InterlockedIncrement(cat == 0 ? &g_rfFlagged : cat == 1 ? &g_rfUnflagged : cat == 2 ? &g_rfKeptOlder : &g_rfIconHidden);
        const LONG64 last = g_rfLastLine[cat];
        if (static_cast<LONG64>(now) - last >= 250 &&
            InterlockedCompareExchange64(&g_rfLastLine[cat], static_cast<LONG64>(now), last) == last) {
            const LONG ln = InterlockedIncrement(&g_rfLines);
            if (ln <= kMaxRefusalLines) {
                static const char* const kCat[4] = {
                    "HIT A FLAGGED SURFACE (not grappleable) - no usable target",
                    "HIT AN UNFLAGGED SURFACE - still no usable target",
                    "HIT, but the game holds an OLDER target",
                    "USABLE target on the ray, but the grapple ICON's gate (+D75DF0) says NO" };
                char held[192];
                if (none) {
                    _snprintf_s(held, sizeof(held), _TRUNCATE, "held: NONE");
                } else {
                    // Where the held target sits relative to the CURRENT hand ray: its distance off the ray (the sticky
                    // retention compares ~this against distMax 0.1) and its depth along the ray vs this hit.
                    const float vx = s.hit[0] - c.hand[0], vy = s.hit[1] - c.hand[1], vz = s.hit[2] - c.hand[2];
                    const float along = vx * c.dir[0] + vy * c.dir[1] + vz * c.dir[2];
                    const float px = vx - along * c.dir[0], py = vy - along * c.dir[1], pz = vz - along * c.dir[2];
                    _snprintf_s(held, sizeof(held), _TRUNCATE, "held: %s at %.2f m from this hit - %.2f m OFF the ray, %.1f m "
                                "along it (this hit %.1f m)", s.invalid ? "NOT USABLE" : "USABLE", d < 0.0f ? -1.0f : d,
                                sqrtf(px * px + py * py + pz * pz), along, c.dist);
                }
                char cls[96];
                RttiClassOf(c.collObj ? c.collObj - 24 : 0, cls, sizeof(cls));
                // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
                LOG("grappleaim: [GRAPPLEAIM] REFUSAL #%ld %s | hit at (%.2f, %.2f, %.2f), %.2f m from the hand, elevation vs "
                    "head %s | hit record %s flags 0x%04X (0x100/0x1000 = not grappleable) type %u object %s, hit object "
                    "class %s | %s%s | %s | line throttle 250 ms per kind%s",
                    ln, kCat[cat], c.at[0], c.at[1], c.at[2], c.dist, c.bucket >= 0 ? kElevNames[c.bucket] : "no head",
                    c.recOk ? "read," : "UNREADABLE,", c.flags, static_cast<unsigned>(c.type),
                    c.objId ? "static-mesh" : "none/dynamic", cls, held, gate == 0 ? " | icon gate FALSE" : "", gateTxt,
                    ln == kMaxRefusalLines ? "  (line cap reached - counted in the window only)" : "");
            }
        }
    }
    const bool thrown = haveState && state == 3 &&
                        !(g_prevCtrl == reinterpret_cast<uintptr_t>(ctrl) && g_prevState == 3);
    if (haveState) {
        g_prevCtrl = reinterpret_cast<uintptr_t>(ctrl);
        g_prevState = state;
    }
    if (thrown) { ++g_throws; }
    Unlock(g_winLock);

    const LONG tn = thrown ? InterlockedIncrement(&g_throwLines) : 0;
    if (thrown && tn <= kMaxThrowLines) {
        // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=HEADSET  log-only=yes
        LOG("grappleaim: [GRAPPLEAIM] THROW #%ld (state -> 3) | AIMED: hand ray cast %s at (%.2f, %.2f, %.2f), cast age "
            "%llu ms%s | TARGET (stored, where it pulls): %s (%.2f, %.2f, %.2f) %s | aimed-to-target distance %s%.2f m, "
            "angle off the hand ray %.1f deg%s%s",
            tn, c.hit ? "HIT" : "MISSED (ray end)", c.at[0], c.at[1], c.at[2],
            static_cast<unsigned long long>(c.valid ? now - c.tick : 0), castFresh ? "" : " (STALE - no verdict)",
            none ? "NONE" : "at", s.hit[0], s.hit[1], s.hit[2],
            none ? "" : (s.invalid == 0 ? "USABLE" : "NOT USABLE"),
            d < 0.0f ? "n/a " : "", d < 0.0f ? 0.0f : d, ang < 0.0f ? 0.0f : ang,
            (d >= 0.0f && d > 0.75f) ? "  *** BEYOND THE 0.75 m STICKY RADIUS ***" : "",
            tn == kMaxThrowLines ? "  (line cap reached - further throws are counted in the window only)" : "");
    }
}

// [LOADBEARING: R9-GRAPPLE-ORIGIN] with grapple_hand_origin=1 this is the ONLY thing that starts the grapple ray (and
//      the ledge assist's re-casts) at the hand - gr11: +D7D630 is +D76000's origin, called at +D760B2, and +D7D900's
//      own origin call at +D7DA49; INV-273.
float* __fastcall OriginHook(void* ctrl, float* out)
{
    float* r = g_trueOrigin(ctrl, out);
    const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool aimCall = ra == g_gd + g_rOriginAimRet;
    const bool ledgeCall = ra == g_gd + g_rOriginLedgeRet;
    if (!aimCall && !ledgeCall) {
        InterlockedIncrement(&g_origPass);
        return r;
    }
    if (aimCall) {
        ProbeStored(ctrl);
        ProbeAimVsTarget(ctrl);
    }
    if (!GrappleHandOrigin || !t_aim.valid) {
        InterlockedIncrement(&g_origNoCtx);
        return r;
    }
    if (GetTickCount64() - t_aim.tick > kFreshMs) {
        InterlockedIncrement(&g_origStale);
        return r;
    }
    const float game[3] = { out[0], out[1], out[2] };
    out[0] = t_aim.hand[0];
    out[1] = t_aim.hand[1];
    out[2] = t_aim.hand[2];
    InterlockedIncrement(aimCall ? &g_origAim : &g_origLedge);
    if (InterlockedCompareExchange(&g_firstOriginSaid, 1, 0) == 0) {
        const float ddx = out[0] - game[0], ddy = out[1] - game[1], ddz = out[2] - game[2];
        // [PROBE: GRAPPLEAIM] gate=grapple_hand_origin  site=HEADSET  log-only=yes
        LOG("grappleaim: [GRAPPLEAIM] FIRST HAND ORIGIN (logged regardless of diag_grapple_aim) via the %s call | "
            "game origin (%.3f, %.3f, %.3f) -> hand (%.3f, %.3f, %.3f), moved %.3f m | camera (%.3f, %.3f, %.3f)",
            aimCall ? "aim-ray" : "ledge-assist", game[0], game[1], game[2], out[0], out[1], out[2],
            sqrtf(ddx * ddx + ddy * ddy + ddz * ddz), t_aim.cam[0], t_aim.cam[1], t_aim.cam[2]);
    }
    return r;
}

// ---------------------------------------------------------------- diag_ray_threads
constexpr uintptr_t kBulletRayRet1 = 0x716066;   // +716060 call [Raytrace] (6 B) in +715D90 (fa10)
constexpr uintptr_t kBulletRayRet2 = 0x7160BA;   // +7160B4 call [Raytrace] (6 B) in +715D90 (fa10)
struct ThreadTally { DWORD tid; LONG n; };
constexpr int kTallyMax = 8;
ThreadTally   g_rtBullet[kTallyMax] = {};        // guarded by g_rtLock
ThreadTally   g_rtHud[kTallyMax] = {};
LONG          g_rtBulletOther = 0, g_rtHudOther = 0;   // threads past the table
volatile LONG g_rtLock = 0;
volatile LONG g_inGunCast = 0;                   // set around GunCast's own Raytrace call (HUD update thread)
thread_local bool t_ourBulletCast = false;       // our +715D90 call reaches the same IAT returns: not a bullet, not tallied
volatile LONG g_rtOurs = 0;                      // ... counted here instead
volatile LONG g_rtOverlap = 0;                   // bullet casts that ran WHILE g_inGunCast was set
volatile LONG g_rtFirstSaid = 0;
ULONGLONG     g_rtSumMs = 0;
// Run flat1 (log backups/20260928_raythreads_flat1): the HUD update AND the bullet casts both rotate over the SAME 7 worker threads -
// "same thread set" says nothing about concurrency, and our own cast never runs flat. So v2 times it: every HUD crosshair
// update's [start, end] (QPC) goes into a ring; each bullet cast finds its distance to the nearest one (0 = INSIDE an update).
constexpr int kHudRing = 64;
volatile LONG64 g_hudStart[kHudRing] = {}, g_hudEnd[kHudRing] = {};   // end 0 = in flight
volatile LONG   g_hudIdx = 0;
volatile LONG   g_hudInFlight = 0;               // HUD updates currently executing (any thread)
volatile LONG   g_rtDuringHud = 0;               // bullet casts while g_hudInFlight > 0
// distance buckets: 0 inside, <50 us, <200 us, <1 ms, <5 ms, >= 5 ms (or no HUD update in the ring)
volatile LONG   g_rtBucket[6] = {};
volatile LONG64 g_hudDurSum = 0, g_hudDurMax = 0;
volatile LONG   g_hudDurN = 0;
double          g_qpcUs = 0.0;                   // microseconds per QPC tick

LONG64 QpcNow()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

void RtBulletTiming()
{
    if (g_hudInFlight > 0) { InterlockedIncrement(&g_rtDuringHud); }
    const LONG64 t = QpcNow();
    LONG64 best = -1;
    for (int i = 0; i < kHudRing; ++i) {
        const LONG64 s = g_hudStart[i], e = g_hudEnd[i];
        if (s == 0) { continue; }
        LONG64 dist;
        if (t >= s && (e == 0 || t <= e)) {
            dist = 0;
        } else if (t < s) {
            dist = s - t;
        } else {
            dist = t - e;
        }
        if (best < 0 || dist < best) { best = dist; }
    }
    const double us = best < 0 ? 1e12 : best * g_qpcUs;
    const int b = best == 0 ? 0 : us < 50.0 ? 1 : us < 200.0 ? 2 : us < 1000.0 ? 3 : us < 5000.0 ? 4 : 5;
    InterlockedIncrement(&g_rtBucket[b]);
}

void RtTally(ThreadTally* t, LONG* other, DWORD tid)
{
    while (InterlockedCompareExchange(&g_rtLock, 1, 0) != 0) { YieldProcessor(); }
    int i = 0;
    for (; i < kTallyMax; ++i) {
        if (t[i].tid == tid) { ++t[i].n; break; }
        if (t[i].tid == 0) { t[i].tid = tid; t[i].n = 1; break; }
    }
    if (i == kTallyMax) { ++*other; }
    InterlockedExchange(&g_rtLock, 0);
}

// Called from CrosshairHook (the HUD update) once per update: tallies that thread and, every 5 s, reports both tables.
void RayThreadsTick()
{
    const DWORD self = GetCurrentThreadId();
    RtTally(g_rtHud, &g_rtHudOther, self);
    const ULONGLONG now = GetTickCount64();
    if (g_rtSumMs == 0) { g_rtSumMs = now; }
    if (now - g_rtSumMs < 5000) { return; }
    g_rtSumMs = now;
    char bl[256] = {}, hl[256] = {};
    LONG bOther, hOther, bulletTotal = 0, sameAsHud = 0;
    while (InterlockedCompareExchange(&g_rtLock, 1, 0) != 0) { YieldProcessor(); }
    int bp = 0, hp = 0;
    for (int i = 0; i < kTallyMax; ++i) {
        if (g_rtBullet[i].tid) {
            bulletTotal += g_rtBullet[i].n;
            bool onHud = false;
            for (int k = 0; k < kTallyMax; ++k) { if (g_rtHud[k].tid == g_rtBullet[i].tid) { onHud = true; } }
            if (onHud) { sameAsHud += g_rtBullet[i].n; }
            bp += _snprintf_s(bl + bp, sizeof(bl) - bp, _TRUNCATE, "%s%lu x%ld", bp ? ", " : "",
                              static_cast<unsigned long>(g_rtBullet[i].tid), g_rtBullet[i].n);
        }
        if (g_rtHud[i].tid) {
            hp += _snprintf_s(hl + hp, sizeof(hl) - hp, _TRUNCATE, "%s%lu x%ld", hp ? ", " : "",
                              static_cast<unsigned long>(g_rtHud[i].tid), g_rtHud[i].n);
        }
        if (bp < 0) { bp = static_cast<int>(strlen(bl)); }
        if (hp < 0) { hp = static_cast<int>(strlen(hl)); }
    }
    bOther = g_rtBulletOther; hOther = g_rtHudOther;
    memset(g_rtBullet, 0, sizeof(g_rtBullet));
    memset(g_rtHud, 0, sizeof(g_rtHud));
    g_rtBulletOther = g_rtHudOther = 0;
    InterlockedExchange(&g_rtLock, 0);
    const LONG ov = InterlockedExchange(&g_rtOverlap, 0);
    const LONG during = InterlockedExchange(&g_rtDuringHud, 0);
    LONG bk[6];
    for (int i = 0; i < 6; ++i) { bk[i] = InterlockedExchange(&g_rtBucket[i], 0); }
    const LONG64 dSum = InterlockedExchange64(&g_hudDurSum, 0), dMax = InterlockedExchange64(&g_hudDurMax, 0);
    const LONG dN = InterlockedExchange(&g_hudDurN, 0);
    if (bulletTotal + bOther == 0) {
        return;   // no shot this window
    }
    // [PROBE: RAYTHREAD] gate=diag_ray_threads  site=FLAT  log-only=yes
    LOG("grappleaim: [RAYTHREAD] 5 s | bullet casts (+715D90) by thread: %s%s (%ld more on other threads) | HUD update "
        "(our gun-reticle cast) by thread: %s (%ld more) | bullet casts on a HUD-update thread %ld of %ld (NOT a safety "
        "measure - a worker pool) | bullet casts WHILE our cast was in flight: %ld | WHILE ANY HUD update was in flight: %ld | "
        "distance to the nearest HUD update: inside %ld, <50us %ld, <200us %ld, <1ms %ld, <5ms %ld, >=5ms/none %ld | HUD "
        "update duration mean %.1f max %.1f us (n %ld) | our own +715D90 casts (not tallied above) %ld",
        bl, bp ? "" : "(none)", bOther, hl, hOther, sameAsHud, bulletTotal + bOther, ov, during, bk[0], bk[1], bk[2], bk[3],
        bk[4], bk[5], dN ? (dSum * g_qpcUs) / dN : 0.0, dMax * g_qpcUs, dN, InterlockedExchange(&g_rtOurs, 0));
}

// ---------------------------------------------------------------- part A3
unsigned char __fastcall RaytraceHook(void* self, unsigned char kind, void* coll, const float* from, float* to,
                                      unsigned short a6, void* ignore, unsigned int a8, unsigned int a9, int a10)
{
    const unsigned char r = g_realRay(self, kind, coll, from, to, a6, ignore, a8, a9, a10);
    const uintptr_t rret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (t_ourBulletCast && (rret == g_gd + g_rBulletRayRet1 || rret == g_gd + g_rBulletRayRet2)) {
        InterlockedIncrement(&g_rtOurs);   // fire_reticle_bullet_cast's own call through +715D90
        return r;
    }
    if (DiagRayThreads && (rret == g_gd + g_rBulletRayRet1 || rret == g_gd + g_rBulletRayRet2)) {
        // COST: two compares per Raytrace when on; a spinlocked table insert per BULLET cast only.
        const DWORD tid = GetCurrentThreadId();
        RtTally(g_rtBullet, &g_rtBulletOther, tid);
        if (g_inGunCast) { InterlockedIncrement(&g_rtOverlap); }
        RtBulletTiming();   // COST: a 64-entry scan per BULLET cast, only with diag_ray_threads
        if (InterlockedCompareExchange(&g_rtFirstSaid, 1, 0) == 0) {
            // [PROBE: RAYTHREAD] gate=diag_ray_threads  site=FLAT  log-only=yes
            LOG("grappleaim: [RAYTHREAD] FIRST BULLET CAST (logged unconditionally) | thread %lu | return +%llX | kind 0x%02X "
                "a6 0x%X a8 0x%X a9 0x%X | our cast in flight: %s",
                static_cast<unsigned long>(tid), static_cast<unsigned long long>(rret - g_gd), kind, a6, a8, a9,
                g_inGunCast ? "YES" : "no");
        }
        return r;
    }
    if (rret == g_gd + g_rRay2Ret) {
        // The second cast of this thread's +D76000 call: record it next to the first (gate probe, INV-273 gr23-29).
        t_cast.t2Ran = true;
        t_cast.t2Hit = r != 0;
        __try {
            t_cast.t2At[0] = to[0]; t_cast.t2At[1] = to[1]; t_cast.t2At[2] = to[2];
            t_cast.t2Type = r ? *reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(coll) + 0x38) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            t_cast.t2Ran = false;
        }
        return r;
    }
    if (rret != g_gd + g_rRayRet) {
        return r;
    }
    InterlockedIncrement(&g_rayOurs);
    if (r) { InterlockedIncrement(&g_rayHits); }
    float h[3];
    __try {
        h[0] = to[0]; h[1] = to[1]; h[2] = to[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return r;
    }
    Lock(g_hitLock);
    g_hit[0] = h[0]; g_hit[1] = h[1]; g_hit[2] = h[2];
    g_centre[0] = t_centre[0]; g_centre[1] = t_centre[1]; g_centre[2] = t_centre[2];
    g_haveCentre = t_haveCentre;
    g_hitOk = r != 0;
    g_hitTick = GetTickCount64();
    ++g_hitSeq;
    Unlock(g_hitLock);
    // For the aimed-vs-target probe: this thread's cast, read back at the start of its next +D76000 call.
    t_cast.valid = t_aim.valid;
    t_cast.hit = r != 0;
    t_cast.at[0] = h[0]; t_cast.at[1] = h[1]; t_cast.at[2] = h[2];
    for (int k = 0; k < 3; ++k) { t_cast.hand[k] = t_aim.hand[k]; t_cast.dir[k] = t_aim.dir[k]; }
    t_cast.tick = GetTickCount64();
    t_cast.bucket = t_aim.valid ? t_aim.bucket : -1;
    for (int k = 0; k < 3; ++k) { t_cast.ctlFwd[k] = t_aim.ctlFwd[k]; }
    t_cast.recOk = false;
    t_cast.flags = 0;
    t_cast.objId = 0;
    t_cast.collObj = 0;
    t_cast.dist = -1.0f;
    t_cast.type = 0;
    t_cast.t2Ran = t_cast.t2Hit = false;
    t_cast.t2Type = 0;
    __try {
        const unsigned char* rec = static_cast<const unsigned char*>(coll);
        t_cast.flags = *reinterpret_cast<const unsigned short*>(rec + 0x3A);
        t_cast.type = *reinterpret_cast<const unsigned short*>(rec + 0x38);
        t_cast.objId = *reinterpret_cast<const unsigned long long*>(rec + 0x28);
        t_cast.collObj = *reinterpret_cast<const uintptr_t*>(rec + 0x18);
        const float dx = h[0] - from[0], dy = h[1] - from[1], dz = h[2] - from[2];
        t_cast.dist = sqrtf(dx * dx + dy * dy + dz * dz);
        t_cast.recOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_cast.recOk = false;
    }
    Lock(g_hitLock);
    g_lastCast = t_cast;   // for the throw-commit hook, which runs on the game thread
    Unlock(g_hitLock);
    if (DiagGrappleAim && t_aim.valid) {
        float len = -1.0f;
        __try {
            const float lx = to[0] - from[0], ly = to[1] - from[1], lz = to[2] - from[2];
            len = sqrtf(lx * lx + ly * ly + lz * lz);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            len = -1.0f;
        }
        Lock(g_winLock);
        if (t_cast.bucket < 0) {
            ++g_elNoHead;
        } else {
            ++g_elCasts[t_cast.bucket];
            if (r) { ++g_elHits[t_cast.bucket]; }
        }
        if (!r && len > 0.0f) {   // a miss leaves the end where it was cast: its length is the range used
            ++g_missN;
            g_missLenSum += len;
            if (len > g_missLenMax) { g_missLenMax = len; }
        }
        Unlock(g_winLock);
    }
    return r;
}

void*  g_xhElem = nullptr;
float  g_xhLayout[4] = {};
bool   g_xhMoved = false;

void* CrosshairCamera()
{
    __try {
        const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + g_rCamGlobal);
        if (!g) { return nullptr; }
        const uintptr_t a = *reinterpret_cast<const uintptr_t*>(g + 1344);
        if (!a) { return nullptr; }
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(a + 192);
        if (!b) { return nullptr; }
        return *reinterpret_cast<void* const*>(b + 80);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// 0 = moved, 1 = behind the camera, 2 = fault. `centre` = the game's own GetOnScreenPointF(W/2, H/2) for the same
// +D76000 call - NOT GetForwardVector, which points behind the view (engine gr13, INV-273; run 2: 30,132/30,132
// refused as "behind").
int PlaceMarker(void* self, const float hit[3], const float centre[3], float newPos[4], float ph[4], float pc[4])
{
    __try {
        void* cam = CrosshairCamera();
        if (!cam) { return 2; }
        float cp[4] = {};
        g_getPos(cam, cp);
        const float vx = hit[0] - cp[0], vy = hit[1] - cp[1], vz = hit[2] - cp[2];
        const float cx = centre[0] - cp[0], cy = centre[1] - cp[1], cz = centre[2] - cp[2];
        const float vl = sqrtf(vx * vx + vy * vy + vz * vz), cl = sqrtf(cx * cx + cy * cy + cz * cz);
        if (!(vl > 1e-3f) || !(cl > 1e-6f) || (vx * cx + vy * cy + vz * cz) < 0.05f * vl * cl) {
            return 1;
        }
        g_project(ph, hit, cam);
        g_project(pc, centre, cam);
        if (!std::isfinite(ph[0]) || !std::isfinite(ph[1]) || !std::isfinite(pc[0]) || !std::isfinite(pc[1])) {
            return 2;
        }
        newPos[0] = g_xhLayout[0] + (ph[0] - pc[0]);
        newPos[1] = g_xhLayout[1] + (ph[1] - pc[1]);
        newPos[2] = g_xhLayout[2];
        newPos[3] = 0.0f;
        g_setRel(self, newPos);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

bool RestoreMarker(void* self)
{
    __try {
        g_setRel(self, g_xhLayout);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CaptureLayout(void* self)
{
    __try {
        float p[4] = {};
        g_getRel(self, p);
        g_xhLayout[0] = p[0]; g_xhLayout[1] = p[1]; g_xhLayout[2] = p[2]; g_xhLayout[3] = 0.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- the gun reticle's cast (fire_reticle_3d)
// Counters for the [FIRERETICLE] summary; CrosshairHook runs on the HUD update thread only, but the summary reads them
// with Interlocked anyway.
volatile LONG g_grHit = 0, g_grHitObj = 0, g_grMiss = 0, g_grSelf = 0, g_grNoBore = 0, g_grNoCast = 0, g_grBehind = 0,
              g_grFault = 0, g_grFirstSaid = 0;
float         g_grDistMin = 1e9f, g_grDistMax = 0.0f;
const char*   g_grNoBoreWhy = "-";
ULONGLONG     g_grSumMs = 0;

// The grapple's own cast (+D763C6), re-issued from the muzzle. `end` in = the far point, out = the hit when it returns
// true (Raytrace writes the hit into its `vec3 &`, INV-273). *collObj = SCollision+0x18 (the grapple probe's field).
// False + nothing written when the rope controller / its owner is not there.
bool GunCast(const float from[3], float end[3], bool* hit, uintptr_t* collObj)
{
    __try {
        const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + g_rRopeGlobal);
        if (!g) { return false; }
        const uintptr_t pl = *reinterpret_cast<const uintptr_t*>(g + 1920);
        if (!pl) { return false; }
        void* ctrl = *reinterpret_cast<void* const*>(pl + 3200);
        if (!ctrl) { return false; }
        const OwnerFn owner = *reinterpret_cast<const OwnerFn*>(*reinterpret_cast<const uintptr_t*>(ctrl) + 32);
        const uintptr_t o = owner(ctrl);
        if (!o) { return false; }
        alignas(16) unsigned char coll[128] = {};
        InterlockedExchange(&g_inGunCast, 1);   // diag_ray_threads: a bullet cast seen now = an overlap
        const unsigned char r = g_realRay(reinterpret_cast<void*>(o + 40), 0x37, coll, from, end, 0,
                                          reinterpret_cast<void*>(o + 24), 0x20, 0x100, 0);
        InterlockedExchange(&g_inGunCast, 0);
        *hit = r != 0;
        *collObj = r ? *reinterpret_cast<const uintptr_t*>(coll + 0x18) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_inGunCast, 0);
        return false;
    }
}

// fire_reticle_3d: what the game's crosshair shows this update, read from the HudCrosshair element itself (so the 3D reticle
// shows the game's choice, not ours) + the spread the game draws it with. HUD update thread only.
struct XhairState {
    int   group = 0;          // 0 none, 1 Pistol, 2 Rifle, 3 Shotgun, 4 Bow, 5 Crossbow (INV-281)
    bool  dot = true;         // G_Dot visible
    float sH = 0.0f, sV = 0.0f;   // the SHOT's spread (deg): WFC+148 + P[+9932+24h] / WFC+152 + P[+9936+24h] - the inputs
                                  // +DA1D30 gets for a single projectile (INV-274 (1), aimhook FireSpreadSample). Jay 2026-09-28:
                                  // "Go ahead with the real shot spread". Left/Right parts open by sH, Up/Down by sV.
    float hud0 = 0.0f, hud1 = 0.0f;   // the HUD's own +BC3BF0 pair (v1 used it) - logged against the shot's, not drawn
    bool  shotOk = false;
    float fov = 0.0f;         // the look camera's FOV (deg) - converts HUD design px to degrees
    bool  ok = false;
};
// [FIRERETICLE] window: shot spread vs the HUD's (HUD update thread only)
double g_spSumH = 0.0, g_spSumHud = 0.0, g_spMaxDiff = 0.0;
float  g_spMaxH = 0.0f;
LONG   g_spN = 0, g_spNoShot = 0;
int       g_lastHitCounter = 0;
bool      g_haveHitCounter = false;
ULONGLONG g_hitMs = 0;
volatile LONG g_xsFirstSaid = 0, g_xsFault = 0;

XhairState ReadXhairState(void* self)
{
    XhairState x;
    __try {
        x.group = *reinterpret_cast<const int*>(static_cast<const unsigned char*>(self) + kXhairGroupOff);
        const uintptr_t dot = *reinterpret_cast<const uintptr_t*>(static_cast<const unsigned char*>(self) + 8 * kXhairDotSlot);
        x.dot = dot ? ((*reinterpret_cast<const unsigned char*>(dot + kElemVisOff) & 1) != 0) : false;
        // owner = [[[+1C16348]+1344]+184]+88, gated on +80 - exactly +550FC0 :58-59
        const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + g_rCamGlobal);
        const uintptr_t a = g ? *reinterpret_cast<const uintptr_t*>(g + 1344) : 0;
        const uintptr_t b = a ? *reinterpret_cast<const uintptr_t*>(a + 184) : 0;
        const uintptr_t owner = (b && *reinterpret_cast<const uintptr_t*>(b + 80)) ? *reinterpret_cast<const uintptr_t*>(b + 88) : 0;
        if (owner) {
            float s[2] = {};
            reinterpret_cast<SpreadFn>(g_gd + g_rSpread)(owner, s);
            x.hud0 = std::isfinite(s[0]) ? s[0] : 0.0f;
            x.hud1 = std::isfinite(s[1]) ? s[1] : 0.0f;
            // The shot's own inputs: the right-hand WFC (owner+3096, active byte +16 - the same one +BC3BF0 reads first),
            // its hand index +344, and the owner's per-hand row +9932 + 24*hand.
            const uintptr_t wfc = *reinterpret_cast<const uintptr_t*>(owner + 3096);
            if (wfc && *reinterpret_cast<const unsigned char*>(wfc + 16)) {
                const int hand = *reinterpret_cast<const int*>(wfc + 344);
                if (hand >= 0 && hand <= 1) {
                    const float h = *reinterpret_cast<const float*>(wfc + 148) +
                                    *reinterpret_cast<const float*>(owner + 9932 + 24 * static_cast<uintptr_t>(hand));
                    const float v = *reinterpret_cast<const float*>(wfc + 152) +
                                    *reinterpret_cast<const float*>(owner + 9936 + 24 * static_cast<uintptr_t>(hand));
                    x.sH = std::isfinite(h) ? h : 0.0f;
                    x.sV = std::isfinite(v) ? v : 0.0f;
                    x.shotOk = true;
                }
            }
            const int hc = *reinterpret_cast<const int*>(owner + kHitCounterOff);
            if (g_haveHitCounter && hc != g_lastHitCounter) { g_hitMs = GetTickCount64(); }
            g_lastHitCounter = hc;
            g_haveHitCounter = true;
        }
        void* cam = CrosshairCamera();
        x.fov = (cam && g_getFov) ? g_getFov(cam) : 0.0f;
        x.ok = owner != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_xsFault);
        x.ok = false;
    }
    return x;
}

// fire_reticle_3d: the dot's point in the GAME CAMERA's frame, handed to the render thread (overlay.cpp) by seqlock.
volatile LONG g_g3Seq = 0;
float         g_g3Local[3] = {};     // (end - camPos) . (col0, col1, col2) = right / up / back (col2 = GetForwardVector)
float         g_g3GameOff[3] = {};   // end - camPos, game world - for the yaw-mapping cross-check only
float         g_g3Dist = 0.0f;       // muzzle -> dot
bool          g_g3Hit = false;
ULONGLONG     g_g3Ms = 0;
int           g_g3Group = 0;         // the game's crosshair group (INV-281)
bool          g_g3DotVis = true;
float         g_g3S0 = 0.0f, g_g3S1 = 0.0f, g_g3Fov = 0.0f;
ULONGLONG     g_g3HitMs = 0;         // last change of the hit counter
float         g_g3Shaft[3] = {};     // bow_reticle_3d: the shaft's straight-line point, same frame as g_g3Local
bool          g_g3ShaftOk = false;

volatile LONG g_g3NoView = 0;        // [FIRERETICLE] window: no fresh rendered-view matrix (dot not published)
double        g_g3LookSum = 0.0;     // ... angle between the engine LOOK camera's and the RENDERED view's back axis (deg)
float         g_g3LookMax = 0.0f;
LONG          g_g3LookN = 0;

// Publishes the dot and parks the HudCrosshair group off-screen. 0 = published + parked, 2 = no rendered view.
// The frame is the RENDERED view's camera matrix (camhook, after its rotation write) - NOT CrosshairCamera(), the
// engine's LOOK camera (CameraFPPDI, INV-272), whose pitch drifts from the head's: run reticle3d_run1 placed by it and
// Jay saw the dot LOWER than the shots.
// bow = true (bow_reticle_3d): the group is the Bow's (4) and the parts open by the HUD's own pair, which for an active bow is
// 15(1-sqrt draw) on both axes (+BC3BF0 :75-80) - the right-hand WFC's shot spread does not exist for the bow.
// shaftEnd (bow only, may be null): the arrow shaft's straight-line point, published beside the dot in the same frame.
int GunReticle3d(void* self, const float end[3], bool hit, float dist, const float muz[3], const float dir[3],
                 uintptr_t item, unsigned age, const char* kind, uintptr_t obj, bool bow = false,
                 const float* shaftEnd = nullptr)
{
    float m[12] = {};
    unsigned viewAge = 0;
    if (!CamHookGetRenderedViewMatrix(m, &viewAge)) {
        InterlockedIncrement(&g_g3NoView);
        return 2;
    }
    const float cp[3] = { m[3], m[7], m[11] };
    const float c0[3] = { m[0], m[4], m[8] }, c1[3] = { m[1], m[5], m[9] }, c2[3] = { m[2], m[6], m[10] };
    // Measure what the fix removes: the engine look camera's back axis vs the rendered one's (log-only).
    float look = -1.0f;
    __try {
        void* cam = CrosshairCamera();
        if (cam && g_getFwd) {
            float lf[4] = {};
            g_getFwd(cam, lf);
            float dd = lf[0] * c2[0] + lf[1] * c2[1] + lf[2] * c2[2];
            dd = dd > 1.0f ? 1.0f : (dd < -1.0f ? -1.0f : dd);
            look = acosf(dd) * 57.2957795f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        look = -1.0f;
    }
    if (look >= 0.0f) {
        g_g3LookSum += look;
        if (look > g_g3LookMax) { g_g3LookMax = look; }
        ++g_g3LookN;
    }
    const float d[3] = { end[0] - cp[0], end[1] - cp[1], end[2] - cp[2] };
    const float loc[3] = { d[0] * c0[0] + d[1] * c0[1] + d[2] * c0[2],
                           d[0] * c1[0] + d[1] * c1[1] + d[2] * c1[2],
                           d[0] * c2[0] + d[1] * c2[1] + d[2] * c2[2] };
    if (!std::isfinite(loc[0]) || !std::isfinite(loc[1]) || !std::isfinite(loc[2])) {
        InterlockedIncrement(&g_grFault);
        return 2;
    }
    float shLoc[3] = {};
    bool shOk = false;
    if (shaftEnd) {
        const float e[3] = { shaftEnd[0] - cp[0], shaftEnd[1] - cp[1], shaftEnd[2] - cp[2] };
        shLoc[0] = e[0] * c0[0] + e[1] * c0[1] + e[2] * c0[2];
        shLoc[1] = e[0] * c1[0] + e[1] * c1[1] + e[2] * c1[2];
        shLoc[2] = e[0] * c2[0] + e[1] * c2[1] + e[2] * c2[2];
        shOk = std::isfinite(shLoc[0]) && std::isfinite(shLoc[1]) && std::isfinite(shLoc[2]);
    }
    const XhairState xs = ReadXhairState(self);
    if (InterlockedCompareExchange(&g_xsFirstSaid, 1, 0) == 0) {
        // [PROBE: FIRERETICLE] gate=fire_reticle_3d  site=HEADSET  log-only=yes
        LOG("grappleaim: [FIRERETICLE] FIRST CROSSHAIR STATE (logged unconditionally) | %s | group %d (1 Pistol 2 Rifle 3 Shotgun 4 Bow "
            "5 Crossbow, INV-281) | G_Dot visible %d | SHOT spread H %.3f V %.3f deg%s | HUD's +BC3BF0 pair %.3f / %.3f deg | look "
            "camera FOV %.2f deg | hit counter %d",
            xs.ok ? "read" : "*** owner not reachable - spread/hit marker off ***", xs.group, xs.dot ? 1 : 0, xs.sH, xs.sV,
            xs.shotOk ? "" : " (*** right-hand WFC not active - drawn closed ***)", xs.hud0, xs.hud1, xs.fov, g_lastHitCounter);
    }
    if (xs.shotOk) {
        g_spSumH += xs.sH;
        g_spSumHud += xs.hud0;
        if (xs.sH > g_spMaxH) { g_spMaxH = xs.sH; }
        const double dd = fmax(fabs(static_cast<double>(xs.sH) - xs.hud0), fabs(static_cast<double>(xs.sV) - xs.hud1));
        if (dd > g_spMaxDiff) { g_spMaxDiff = dd; }
        ++g_spN;
    } else {
        ++g_spNoShot;
    }
    InterlockedIncrement(&g_g3Seq);   // odd = writing
    MemoryBarrier();
    for (int k = 0; k < 3; ++k) { g_g3Local[k] = loc[k]; g_g3GameOff[k] = d[k]; }
    g_g3Dist = dist;
    g_g3Hit = hit;
    g_g3Group = bow ? 4 : xs.group;
    g_g3DotVis = xs.dot;
    g_g3S0 = bow ? xs.hud1 : xs.sH;   // published as (H, V); overlay: Left/Right <- H, Up/Down <- V
    g_g3S1 = bow ? xs.hud0 : xs.sV;
    g_g3Fov = xs.fov;
    for (int k = 0; k < 3; ++k) { g_g3Shaft[k] = shLoc[k]; }
    g_g3ShaftOk = shOk;
    g_g3HitMs = g_hitMs;
    g_g3Ms = GetTickCount64();
    MemoryBarrier();
    InterlockedIncrement(&g_g3Seq);   // even = stable
    // The game's own crosshair (dot + per-weapon parts) leaves the screen while the 3D dot is up; CrosshairHook puts it
    // back at its layout spot once the bore goes stale (g_xhMoved).
    __try {
        float off[4] = { g_xhLayout[0] - 100000.0f, g_xhLayout[1] - 100000.0f, g_xhLayout[2], 0.0f };
        g_setRel(self, off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_grFault);
    }
    if (InterlockedCompareExchange(&g_grFirstSaid, 1, 0) == 0) {
        // Basis check, logged once: the three columns should be unit and mutually orthogonal, and right x up = back
        // (+1) for the right/up/back reading derived from GetOnScreenPointF (gr13). -1 = mirrored: the dot would be
        // placed mirrored left/right - read the [FIRERETICLE3D] cross-check before trusting a placement.
        const float cx = c0[1] * c1[2] - c0[2] * c1[1], cy = c0[2] * c1[0] - c0[0] * c1[2], cz = c0[0] * c1[1] - c0[1] * c1[0];
        const float det = cx * c2[0] + cy * c2[1] + cz * c2[2];
        // [PROBE: FIRERETICLE] gate=fire_reticle_3d  site=HEADSET  log-only=yes
        LOG("grappleaim: [FIRERETICLE] FIRST 3D PUBLISH (logged unconditionally) | item %p bore age %u ms | muzzle (%.3f, "
            "%.3f, %.3f) bore (%.4f, %.4f, %.4f) | %s at %.2f m, collision object %p | RENDERED view (camhook) age %u ms, "
            "pos (%.3f, %.3f, %.3f) | engine LOOK camera back axis vs rendered %.2f deg | "
            "|col0| %.4f |col1| %.4f |col2| %.4f, col0.col1 %+.4f col0.col2 %+.4f col1.col2 %+.4f, (col0 x col1).col2 %+.4f "
            "(expect +1) | camera-frame dot (right %+.3f, up %+.3f, back %+.3f) | HUD crosshair parked",
            reinterpret_cast<void*>(item), age, muz[0], muz[1], muz[2], dir[0], dir[1], dir[2], kind, dist,
            reinterpret_cast<void*>(obj), viewAge, cp[0], cp[1], cp[2], look,
            sqrtf(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]), sqrtf(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]),
            sqrtf(c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2]),
            c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2], c0[0] * c2[0] + c0[1] * c2[1] + c0[2] * c2[2],
            c1[0] * c2[0] + c1[1] * c2[1] + c1[2] * c2[2], det, loc[0], loc[1], loc[2]);
    }
    return 0;
}

// fire_reticle_bullet_cast: the BULLET's own segment cast +715D90 (INV-278), with the bullet's own ignore object
// (vfunc 0 of the owner's +2336 interface, +24 - fa2 :851-854, fa10 +713220 :226-231). Same contract as GunCast.
volatile LONG g_bcFirstSaid = 0, g_bcFault = 0;
bool BulletCast(const float from[3], float end[3], bool* hit, uintptr_t* collObj)
{
    __try {
        const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + g_rRopeGlobal);
        if (!g) { return false; }
        const uintptr_t pl = *reinterpret_cast<const uintptr_t*>(g + 1920);
        if (!pl) { return false; }
        void* ctrl = *reinterpret_cast<void* const*>(pl + 3200);
        if (!ctrl) { return false; }
        const OwnerFn owner = *reinterpret_cast<const OwnerFn*>(*reinterpret_cast<const uintptr_t*>(ctrl) + 32);
        const uintptr_t o = owner(ctrl);
        if (!o) { return false; }
        void* iface = reinterpret_cast<void*>(o + 2336);
        const IfaceFn f0 = *reinterpret_cast<const IfaceFn*>(*reinterpret_cast<const uintptr_t*>(iface));
        const uintptr_t who = f0(iface);
        void* ignore = who ? reinterpret_cast<void*>(who + 24) : reinterpret_cast<void*>(o + 24);
        alignas(16) unsigned char coll[128] = {};
        const float start[3] = { from[0], from[1], from[2] };
        const BulletCastFn cast = reinterpret_cast<BulletCastFn>(g_gd + g_rBulletCast);
        t_ourBulletCast = true;
        InterlockedExchange(&g_inGunCast, 1);
        const unsigned char r = cast(reinterpret_cast<void*>(o + 40), start, end, coll, ignore);
        InterlockedExchange(&g_inGunCast, 0);
        t_ourBulletCast = false;
        *hit = r != 0;
        *collObj = r ? *reinterpret_cast<const uintptr_t*>(coll + 0x18) : 0;
        if (InterlockedCompareExchange(&g_bcFirstSaid, 1, 0) == 0) {
            // [PROBE: FIRERETICLE] gate=fire_reticle_bullet_cast  site=HEADSET  log-only=yes
            LOG("grappleaim: [FIRERETICLE] FIRST BULLET-QUERY CAST (logged unconditionally) | +715D90 igs %p ignore %p (bullet-style "
                "%s; grapple-style would be %p) | %s, collision object %p",
                reinterpret_cast<void*>(o + 40), ignore, who ? "from the +2336 interface" : "*** interface returned 0 - fell back ***",
                reinterpret_cast<void*>(o + 24), r ? "HIT" : "miss", reinterpret_cast<void*>(*collObj));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_inGunCast, 0);
        t_ourBulletCast = false;
        InterlockedIncrement(&g_bcFault);
        return false;
    }
}

// ===== CROWD GUARD (2026-11-15; Jay: "HOVERING CROSSHAIR OVER LONG DISTANCE AI CAUSES DEATH (COUNTS TOWARDS XP)") =====
// +715D90 (BulletCast, every HUD update) runs the crowd damage pass +205080 on the level's Crowd::Manager: each crowd member
// within 0.24 m of the segment gets slot +200 = OnTakeDamage(dir, point, 1.0 / 0.35, attacker = the cast's ignore object = the
// player, headshot) - INV-278 (iii). Only Crowd::SimpleVis (+2083B0) and Crowd::VertexVis (+209CD0) override that slot. Both
// slots are swapped for hooks that SKIP the call inside OUR cast (t_ourBulletCast, this thread) and pass every other call through
// unchanged (real bullets / arrows, every other crowd damage source). +205080 sets the hit point BEFORE the call, so the reticle
// still stops on the zombie. If either swap refuses, g_crowdGuardLive stays 0 and the cast sites use GunCast (no crowd pass).
// Trace docs/traces/20261115_1030_crowd_reticle_kill_guard.md.
// [LOADBEARING: crowd reticle guard] (fire_reticle_bullet_cast) without it the 3D reticle kills every crowd zombie it points at.
// COST: per crowd hit inside our cast, one thread_local test + one InterlockedIncrement; outside it, one thread_local test.
using CrowdDmgFn = void(__fastcall*)(void* vis, const float* dir, const float* point, float dmg, void* attacker, bool head);
constexpr uintptr_t kCrowdSimpleVtRva = 0x1429140;    // .?AVSimpleVis@Crowd@@ (aid3)
constexpr uintptr_t kCrowdSimpleDmgRva = 0x2083B0;    // its slot +200 (aid6)
constexpr uintptr_t kCrowdVertexVtRva = 0x1429030;    // .?AVVertexVis@Crowd@@ (aid3)
constexpr uintptr_t kCrowdVertexDmgRva = 0x209CD0;    // its slot +200 (aid6)
constexpr int       kCrowdDmgSlot = 25;               // byte +200
CrowdDmgFn    g_trueCrowdSimple = nullptr, g_trueCrowdVertex = nullptr;
volatile LONG g_crowdGuardLive = 0, g_crowdSkips = 0, g_crowdSkipSaid = 0;

void CrowdSkipNote(void* vis, const char* cls, float dmg, void* attacker, bool head)
{
    const LONG n = InterlockedIncrement(&g_crowdSkips);
    if (InterlockedCompareExchange(&g_crowdSkipSaid, 1, 0) == 0) {
        // [PROBE: FIRERETICLE] gate=fire_reticle_bullet_cast  site=HEADSET  log-only=yes
        LOG("grappleaim: [FIRERETICLE] CROWD GUARD first skip (logged unconditionally) - the reticle's own cast reached %s %p "
            "OnTakeDamage (damage %.2f, attacker %p, headshot %d): NOT applied | skips so far %ld | thread %lu",
            cls, vis, static_cast<double>(dmg), attacker, head ? 1 : 0, n, GetCurrentThreadId());
    }
}

void __fastcall CrowdSimpleHook(void* vis, const float* dir, const float* point, float dmg, void* attacker, bool head)
{
    if (t_ourBulletCast) {
        CrowdSkipNote(vis, "Crowd::SimpleVis", dmg, attacker, head);
        return;
    }
    g_trueCrowdSimple(vis, dir, point, dmg, attacker, head);
}

void __fastcall CrowdVertexHook(void* vis, const float* dir, const float* point, float dmg, void* attacker, bool head)
{
    if (t_ourBulletCast) {
        CrowdSkipNote(vis, "Crowd::VertexVis", dmg, attacker, head);
        return;
    }
    g_trueCrowdVertex(vis, dir, point, dmg, attacker, head);
}

// Expect-check, then swap one slot. Returns nullptr on success, else the reason (named in the install line).
const char* SwapCrowdSlot(HMODULE gd, uintptr_t vtRva, uintptr_t dmgRva, void* hook, CrowdDmgFn* orig)
{
    void** slot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(vtRva) + 8 * kCrowdDmgSlot);
    const uintptr_t expect = reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(dmgRva);
    if (reinterpret_cast<uintptr_t>(*slot) != expect) {
        return "slot +200 does not hold the expected OnTakeDamage (different game build?)";
    }
    *orig = reinterpret_cast<CrowdDmgFn>(*slot);   // valid before the swap goes live
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return "VirtualProtect on the vtable slot failed";
    }
    InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return nullptr;
}

void InstallCrowdGuard(HMODULE gd)
{
    const char* whyS = SwapCrowdSlot(gd, kCrowdSimpleVtRva, kCrowdSimpleDmgRva, reinterpret_cast<void*>(&CrowdSimpleHook),
                                     &g_trueCrowdSimple);
    const char* whyV = whyS ? "not tried (SimpleVis refused)"
                            : SwapCrowdSlot(gd, kCrowdVertexVtRva, kCrowdVertexDmgRva, reinterpret_cast<void*>(&CrowdVertexHook),
                                            &g_trueCrowdVertex);
    InterlockedExchange(&g_crowdGuardLive, (!whyS && !whyV) ? 1 : 0);
    // [PROBE: FIRERETICLE] gate=fire_reticle_bullet_cast  site=FLAT  log-only=yes
    LOG("grappleaim: [FIRERETICLE] CROWD GUARD (logged unconditionally): %s | SimpleVis vt +%llX slot +200 %s | VertexVis vt +%llX "
        "slot +200 %s | fire_reticle_bullet_cast=%d",
        g_crowdGuardLive ? "LIVE - the reticle's bullet cast no longer damages crowd zombies"
                         : "*** NOT LIVE - BulletCast OFF, the reticle uses GunCast (no crowd pass) ***",
        static_cast<unsigned long long>(kCrowdSimpleVtRva), whyS ? whyS : "swapped",
        static_cast<unsigned long long>(kCrowdVertexVtRva), whyV ? whyV : "swapped", FireReticleBulletCast ? 1 : 0);
}

// fire_reticle_3d only (the flat-HUD placement, fire_reticle_from_barrel, was removed 2026-10-07). 0 = the dot is on the
// bore, 1 = no fresh bore (not holding a gun: caller falls through), 2 = bore fresh but the dot could not be placed.
int GunReticle(void* self)
{
    float dir[3] = {}, pos[3] = {};
    unsigned age = 0;
    const char* why = "?";
    uintptr_t item = 0;
    if (!GetPublishedBarrelBore(dir, &age, &why, pos, &item)) {
        InterlockedIncrement(&g_grNoBore);
        g_grNoBoreWhy = why;
        return 1;
    }
    float end[3] = { pos[0] + dir[0] * kGunCastM, pos[1] + dir[1] * kGunCastM, pos[2] + dir[2] * kGunCastM };
    bool hit = false;
    uintptr_t obj = 0;
    const bool cast = (FireReticleBulletCast && g_crowdGuardLive) ? BulletCast(pos, end, &hit, &obj) : GunCast(pos, end, &hit, &obj);
    float dist = -1.0f;
    const char* kind = "pin (miss)";
    if (!cast) {
        InterlockedIncrement(&g_grNoCast);
        kind = "pin (no cast - rope controller absent)";
        hit = false;
    } else if (hit) {
        const float dx = end[0] - pos[0], dy = end[1] - pos[1], dz = end[2] - pos[2];
        dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!(dist >= kGunSelfHitM)) {
            InterlockedIncrement(&g_grSelf);
            kind = "pin (hit < 5 cm = own gun)";
            hit = false;
        } else {
            InterlockedIncrement(&g_grHit);
            if (obj) { InterlockedIncrement(&g_grHitObj); }
            if (dist < g_grDistMin) { g_grDistMin = dist; }
            if (dist > g_grDistMax) { g_grDistMax = dist; }
            kind = "hit";
        }
    } else {
        InterlockedIncrement(&g_grMiss);
    }
    if (!hit) {
        end[0] = pos[0] + dir[0] * kGunPinM;
        end[1] = pos[1] + dir[1] * kGunPinM;
        end[2] = pos[2] + dir[2] * kGunPinM;
    }
    return GunReticle3d(self, end, hit, hit ? dist : kGunPinM, pos, dir, item, age, kind, obj);
}

void GunReticleSummary()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_grSumMs < 5000) { return; }
    g_grSumMs = now;
    const LONG h = InterlockedExchange(&g_grHit, 0), ho = InterlockedExchange(&g_grHitObj, 0);
    const LONG m = InterlockedExchange(&g_grMiss, 0), s = InterlockedExchange(&g_grSelf, 0);
    const LONG nb = InterlockedExchange(&g_grNoBore, 0), nc = InterlockedExchange(&g_grNoCast, 0);
    const LONG b = InterlockedExchange(&g_grBehind, 0), f = InterlockedExchange(&g_grFault, 0);
    const float dmin = g_grDistMin, dmax = g_grDistMax;
    g_grDistMin = 1e9f;
    g_grDistMax = 0.0f;
    const LONG nv = InterlockedExchange(&g_g3NoView, 0);
    const double lookMean = g_g3LookN ? g_g3LookSum / g_g3LookN : -1.0;
    const float lookMax = g_g3LookMax;
    const LONG lookN = g_g3LookN;
    g_g3LookSum = 0.0;
    g_g3LookMax = 0.0f;
    g_g3LookN = 0;
    const LONG spN = g_spN, spNo = g_spNoShot;
    const double spMeanH = spN ? g_spSumH / spN : -1.0, spMeanHud = spN ? g_spSumHud / spN : -1.0, spDiff = g_spMaxDiff;
    const float spMaxH = g_spMaxH;
    g_spSumH = g_spSumHud = g_spMaxDiff = 0.0;
    g_spMaxH = 0.0f;
    g_spN = g_spNoShot = 0;
    if (h + m + s + nc + b + f + nv == 0) {
        return;   // no gun held this window: nothing to say (the no-bore count alone is every non-gun frame)
    }
    // [PROBE: FIRERETICLE] gate=fire_reticle_3d  site=HEADSET  log-only=yes
    LOG_PERIODIC("grappleaim: [FIRERETICLE] 5 s | on the bore: hit %ld (on a collision object %ld, %.2f-%.2f m), pinned 50 m: miss %ld, "
        "own-gun hit %ld, no cast %ld | NOT placed: behind %ld, fault %ld, no rendered view %ld | no fresh bore %ld (last: %s) "
        "| 3D: engine LOOK camera vs rendered view mean %.2f max %.2f deg (n %ld; -1 = HUD mode) | spread drawn = SHOT H mean %.3f "
        "max %.3f deg vs HUD's pair mean %.3f, max |shot-HUD| %.3f deg (n %ld, right WFC inactive %ld)",
        h, ho, h ? dmin : 0.0f, h ? dmax : 0.0f, m, s, nc, b, f, nv, nb, g_grNoBoreWhy, lookMean, lookMax, lookN, spMeanH, spMaxH,
        spMeanHud, spDiff, spN, spNo);
}

// ---------------------------------------------------------------- bow_reticle_3d (INV-496)
// HUD update thread only (the counters below are not shared with any other thread).
using ArrowSpeedFn = float(__fastcall*)(void*);
LONG      g_bwPlaced = 0, g_bwHit = 0, g_bwMiss = 0, g_bwNotDrawn = 0, g_bwNoCtrl = 0, g_bwNoCast = 0, g_bwFault = 0;
LONG      g_bwStraight = 0, g_bwGravArrow = 0, g_bwGravStatic = 0, g_bwCasts = 0, g_bwBadSpeed = 0, g_bwNotPlaced = 0;
LONG      g_bwState[9] = {};               // states -1..6 seen while the bow ray was fresh (index state + 1; 8 = other)
double    g_bwUsSum = 0.0;
float     g_bwUsMax = 0.0f, g_bwSpdMin = 1e9f, g_bwSpdMax = 0.0f, g_bwDistMin = 1e9f, g_bwDistMax = 0.0f;
float     g_bwAccLast[3] = {};
double    g_bwDropSum = 0.0;               // landing point vs the shaft line, seen from the nock (deg)
float     g_bwDropMax = 0.0f;
LONG      g_bwDropN = 0;
bool      g_bwFirstSaid = false;
ULONGLONG g_bwSumMs = 0;

// The owner (PlayerDI) the HudCrosshair update itself reads: [[[+1C16348]+1344]+184]+88, gated on +80 (+550FC0 :58-59).
uintptr_t XhairOwner()
{
    const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + g_rCamGlobal);
    const uintptr_t a = g ? *reinterpret_cast<const uintptr_t*>(g + 1344) : 0;
    const uintptr_t b = a ? *reinterpret_cast<const uintptr_t*>(a + 184) : 0;
    return (b && *reinterpret_cast<const uintptr_t*>(b + 80)) ? *reinterpret_cast<const uintptr_t*>(b + 88) : 0;
}

bool SaneAccel(const float a[3])
{
    if (!std::isfinite(a[0]) || !std::isfinite(a[1]) || !std::isfinite(a[2])) { return false; }
    const float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l >= 0.5f && l <= 50.0f;
}

// 0 = drawn: the dot is on the arrow's arc, 3 = bow held but NOT drawn: HUD crosshair parked, nothing published (Jay),
// 1 = no bow (no fresh bow ray, or no active bow controller): caller falls through unchanged, 2 = drawn but not placed.
int BowReticle(void* self)
{
    float dir[3] = {}, pos[3] = {};
    unsigned age = 0;
    uintptr_t item = 0;
    const char* why = "?";
    if (!GetPublishedBowRay(dir, pos, &age, &item, &why)) { return 1; }
    uintptr_t ctrl = 0;
    bool active = false;
    int state = -99;
    float draw = -1.0f;
    __try {
        const uintptr_t owner = XhairOwner();
        ctrl = owner ? *reinterpret_cast<const uintptr_t*>(owner + kOwnerBowCtrlOff) : 0;
        if (ctrl) {
            active = *reinterpret_cast<const unsigned char*>(ctrl + kBowActiveOff) != 0;
            state = *reinterpret_cast<const int*>(ctrl + kBowStateOff);
            draw = *reinterpret_cast<const float*>(ctrl + kBowDrawOff);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_bwFault;
        return 1;
    }
    if (!ctrl || !active) {
        ++g_bwNoCtrl;
        return 1;
    }
    ++g_bwState[(state >= -1 && state <= 6) ? state + 1 : 8];
    if (state != 1 && state != 3) {
        // Held, not drawn: nothing in 3D, and the game's head-locked bow crosshair leaves the screen (Jay: "Nothing").
        ++g_bwNotDrawn;
        __try {
            float off[4] = { g_xhLayout[0] - 100000.0f, g_xhLayout[1] - 100000.0f, g_xhLayout[2], 0.0f };
            g_setRel(self, off);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ++g_bwFault;
        }
        return 3;
    }
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    float speed = 0.0f;
    __try {
        speed = reinterpret_cast<ArrowSpeedFn>(g_gd + g_rArrowSpeed)(reinterpret_cast<void*>(ctrl));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        speed = 0.0f;
        ++g_bwFault;
    }
    if (!std::isfinite(speed) || speed < 0.5f || speed > 1000.0f) {
        ++g_bwBadSpeed;
        speed = 0.0f;
    }
    float acc[3] = {};
    const char* gsrc = "?";
    if (GetLastArrowAccel(acc) && SaneAccel(acc)) {
        gsrc = "the last arrow's";
        ++g_bwGravArrow;
    } else {
        acc[0] = kArrowGravDefault[0]; acc[1] = kArrowGravDefault[1]; acc[2] = kArrowGravDefault[2];
        gsrc = "the measured default (no arrow yet)";
        ++g_bwGravStatic;
    }
    const bool arc = speed > 0.0f && SaneAccel(acc);
    if (!arc) { ++g_bwStraight; }
    for (int k = 0; k < 3; ++k) { g_bwAccLast[k] = acc[k]; }
    // March: straight = one cast of kGunCastM; arc = up to kBowArcSteps segments of the arrow's own step (INV-278: next =
    // p + (v + a dt / 2) dt, v += a dt), capped at kBowArcMaxS seconds or kGunCastM of flight.
    const int steps = arc ? kBowArcSteps : 1;
    const float T = arc ? fminf(kGunCastM / speed, kBowArcMaxS) : 0.0f;
    const float dt = arc ? T / static_cast<float>(steps) : 0.0f;
    float p[3] = { pos[0], pos[1], pos[2] };
    float v[3] = { dir[0] * speed, dir[1] * speed, dir[2] * speed };
    float end[3] = {}, pin[3] = {};
    bool hit = false, pinSet = false, castFail = false;
    uintptr_t obj = 0;
    float arcLen = 0.0f, dist = -1.0f;
    int casts = 0;
    for (int i = 0; i < steps; ++i) {
        float nx[3];
        if (arc) {
            for (int k = 0; k < 3; ++k) {
                nx[k] = p[k] + (v[k] + 0.5f * acc[k] * dt) * dt;
                v[k] += acc[k] * dt;
            }
        } else {
            for (int k = 0; k < 3; ++k) { nx[k] = p[k] + dir[k] * kGunCastM; }
        }
        float seg[3] = { nx[0], nx[1], nx[2] };   // the cast writes the hit point into its end
        bool h = false;
        uintptr_t o = 0;
        ++casts;
        const bool c = (FireReticleBulletCast && g_crowdGuardLive) ? BulletCast(p, seg, &h, &o) : GunCast(p, seg, &h, &o);
        if (!c) {
            castFail = true;
            break;
        }
        const float sx = nx[0] - p[0], sy = nx[1] - p[1], sz = nx[2] - p[2];
        const float segLen = sqrtf(sx * sx + sy * sy + sz * sz);
        if (h) {
            const float hx = seg[0] - p[0], hy = seg[1] - p[1], hz = seg[2] - p[2];
            const float d = sqrtf(hx * hx + hy * hy + hz * hz);
            if (!(i == 0 && d < kGunSelfHitM)) {   // a hit at the nock = the bow itself: keep marching
                hit = true;
                obj = o;
                end[0] = seg[0]; end[1] = seg[1]; end[2] = seg[2];
                dist = arcLen + d;
                break;
            }
        }
        if (!pinSet && segLen > 1e-6f && arcLen + segLen >= kGunPinM) {
            const float f = (kGunPinM - arcLen) / segLen;
            pin[0] = p[0] + sx * f; pin[1] = p[1] + sy * f; pin[2] = p[2] + sz * f;
            pinSet = true;
        }
        arcLen += segLen;
        p[0] = nx[0]; p[1] = nx[1]; p[2] = nx[2];
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    const float us = fq.QuadPart ? static_cast<float>(static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / fq.QuadPart) : 0.0f;
    g_bwUsSum += us;
    if (us > g_bwUsMax) { g_bwUsMax = us; }
    g_bwCasts += casts;
    if (speed > 0.0f) {
        if (speed < g_bwSpdMin) { g_bwSpdMin = speed; }
        if (speed > g_bwSpdMax) { g_bwSpdMax = speed; }
    }
    const char* kind = "arc hit";
    if (castFail) {
        ++g_bwNoCast;
        ++g_bwNotPlaced;
        return 2;   // no cast available (rope controller absent) - the caller restores the HUD crosshair
    }
    if (hit) {
        ++g_bwHit;
        if (dist < g_bwDistMin) { g_bwDistMin = dist; }
        if (dist > g_bwDistMax) { g_bwDistMax = dist; }
        if (!arc) { kind = "straight hit"; }
    } else {
        ++g_bwMiss;
        kind = arc ? "arc pin (miss)" : "straight pin (miss)";
        if (arc) {
            if (pinSet) {
                end[0] = pin[0]; end[1] = pin[1]; end[2] = pin[2];
                dist = kGunPinM;
            } else {
                end[0] = p[0]; end[1] = p[1]; end[2] = p[2];
                dist = arcLen;
            }
        } else {
            for (int k = 0; k < 3; ++k) { end[k] = pos[k] + dir[k] * kGunPinM; }
            dist = kGunPinM;
        }
    }
    // The SHAFT's straight line (Jay, option 2): one more cast along arrow_pose +Z - its hit, else the 50 m pin. Drawn as a small
    // dot; the brackets stay on the landing point. Logged: the angle between the two, seen from the nock (= the drop).
    float shaft[3] = { pos[0] + dir[0] * kGunCastM, pos[1] + dir[1] * kGunCastM, pos[2] + dir[2] * kGunCastM };
    {
        bool sh = false;
        uintptr_t so = 0;
        ++casts;
        const bool sc = (FireReticleBulletCast && g_crowdGuardLive) ? BulletCast(pos, shaft, &sh, &so) : GunCast(pos, shaft, &sh, &so);
        const float hx = shaft[0] - pos[0], hy = shaft[1] - pos[1], hz = shaft[2] - pos[2];
        if (!sc || !sh || sqrtf(hx * hx + hy * hy + hz * hz) < kGunSelfHitM) {
            for (int k = 0; k < 3; ++k) { shaft[k] = pos[k] + dir[k] * kGunPinM; }
        }
    }
    {
        const float lx = end[0] - pos[0], ly = end[1] - pos[1], lz = end[2] - pos[2];
        const float ll = sqrtf(lx * lx + ly * ly + lz * lz);
        if (ll > 1e-4f) {
            float c = (lx * dir[0] + ly * dir[1] + lz * dir[2]) / ll;
            c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
            const float a = acosf(c) * 57.2957795f;
            g_bwDropSum += a;
            ++g_bwDropN;
            if (a > g_bwDropMax) { g_bwDropMax = a; }
        }
    }
    const int r = GunReticle3d(self, end, hit, dist, pos, dir, item, age, kind, obj, true, shaft);
    if (r == 0) {
        ++g_bwPlaced;
    } else {
        ++g_bwNotPlaced;
    }
    if (!g_bwFirstSaid && r == 0) {
        g_bwFirstSaid = true;
        // [PROBE: BOWRETICLE] gate=bow_reticle_3d  site=HEADSET  log-only=yes
        LOG("grappleaim: [BOWRETICLE] FIRST PLACEMENT (logged unconditionally) | bow item %p, controller %p state %d draw %.3f | "
            "nock (%.3f %.3f %.3f) dir (%.4f %.4f %.4f) | speed %.2f (GetArrowSpeed) | gravity %s (%.3f %.3f %.3f) | %s: %d "
            "cast(s), dt %.3f s, %s at %.2f m along the path | cost %.0f us",
            reinterpret_cast<void*>(item), reinterpret_cast<void*>(ctrl), state, draw, pos[0], pos[1], pos[2], dir[0], dir[1],
            dir[2], speed, gsrc, acc[0], acc[1], acc[2], arc ? "ARC" : "STRAIGHT", casts, dt, kind, dist, us);
    }
    return r == 0 ? 0 : 2;
}

void BowReticleSummary()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_bwSumMs < 5000) { return; }
    g_bwSumMs = now;
    const LONG placed = g_bwPlaced, act = g_bwPlaced + g_bwNotPlaced + g_bwNotDrawn + g_bwFault;
    if (act == 0) {
        g_bwNoCtrl = 0;
        return;   // no bow held this window: nothing to say
    }
    const LONG drawnN = g_bwPlaced + g_bwNotPlaced;
    // [PROBE: BOWRETICLE] gate=bow_reticle_3d  site=HEADSET  log-only=yes
    LOG("grappleaim: [BOWRETICLE] 5 s | DRAWN updates %ld: placed %ld (hit %ld, %.2f-%.2f m; pinned %ld), not placed %ld (no cast "
        "%ld) | NOT DRAWN (parked) %ld | no active bow controller %ld, fault %ld | states seen -1:%ld 0:%ld 1:%ld 2:%ld 3:%ld 4:%ld "
        "5:%ld 6:%ld other:%ld | path: arc %ld, straight %ld (speed unreadable %ld) | gravity from the last arrow %ld, measured "
        "default %ld, last (%.3f %.3f %.3f) | landing vs shaft line mean %.2f max %.2f deg | speed %.2f-%.2f | casts/update %.1f "
        "(incl. the shaft's) | cost mean %.0f max %.0f us",
        drawnN, placed, g_bwHit, g_bwHit ? g_bwDistMin : 0.0f, g_bwHit ? g_bwDistMax : 0.0f, g_bwMiss, g_bwNotPlaced, g_bwNoCast,
        g_bwNotDrawn, g_bwNoCtrl, g_bwFault, g_bwState[0], g_bwState[1], g_bwState[2], g_bwState[3], g_bwState[4],
        g_bwState[5], g_bwState[6], g_bwState[7], g_bwState[8], drawnN - g_bwStraight, g_bwStraight, g_bwBadSpeed,
        g_bwGravArrow, g_bwGravStatic, g_bwAccLast[0], g_bwAccLast[1], g_bwAccLast[2],
        g_bwDropN ? g_bwDropSum / g_bwDropN : 0.0, static_cast<double>(g_bwDropMax), drawnN ? g_bwSpdMin : 0.0f,
        drawnN ? g_bwSpdMax : 0.0f, drawnN ? static_cast<double>(g_bwCasts) / drawnN : 0.0,
        drawnN ? g_bwUsSum / drawnN : 0.0, static_cast<double>(g_bwUsMax));
    g_bwPlaced = g_bwHit = g_bwMiss = g_bwNotDrawn = g_bwNoCtrl = g_bwNoCast = g_bwFault = 0;
    g_bwStraight = g_bwGravArrow = g_bwGravStatic = g_bwCasts = g_bwBadSpeed = g_bwNotPlaced = 0;
    for (LONG& s : g_bwState) { s = 0; }
    g_bwUsSum = 0.0;
    g_bwUsMax = 0.0f;
    g_bwDropSum = 0.0;
    g_bwDropMax = 0.0f;
    g_bwDropN = 0;
    g_bwSpdMin = 1e9f; g_bwSpdMax = 0.0f; g_bwDistMin = 1e9f; g_bwDistMax = 0.0f;
}

// ---------------------------------------------------------------- diag_move_bob
// Per frame (HUD update), everything in the RENDERED view's frame (camhook's written matrix: columns right/up/back), and the
// controller in the HEAD's frame (OpenXR) as the baseline for "what the player's hand actually did". Buckets: speed of the
// rendered view over the ground (EMA) still < 0.5 m/s <= walk < 4.5 <= sprint (thresholds are a CHOICE) x gun out / not.
struct MbAcc {
    LONG   n = 0;
    double gunAng = 0, gunH = 0, gunV = 0, gunPos = 0, grapAng = 0, grapPos = 0, ctlAng = 0, ctlPos = 0, camVert = 0;
    float  gunAngMax = 0, grapAngMax = 0, ctlAngMax = 0, camVertMax = 0;
    LONG   nGun = 0, nGrap = 0;
};
MbAcc     g_mb[6];
bool      g_mbHave = false;
float     g_mbPrevGun[3] = {}, g_mbPrevMuz[3] = {}, g_mbPrevGrapD[3] = {}, g_mbPrevGrapO[3] = {};
bool      g_mbPrevGunOk = false, g_mbPrevGrapOk = false;
float     g_mbPrevCtlD[3] = {}, g_mbPrevCtlP[3] = {};
float     g_mbPrevViewPos[3] = {}, g_mbPrevHmdY = 0.0f;
float     g_mbSpeed = 0.0f;
LARGE_INTEGER g_mbPrevT = {};
ULONGLONG g_mbSumMs = 0;
bool      g_mbFirstSaid = false;

void ToView(const float m[12], const float v[3], float o[3])
{
    o[0] = v[0] * m[0] + v[1] * m[4] + v[2] * m[8];
    o[1] = v[0] * m[1] + v[1] * m[5] + v[2] * m[9];
    o[2] = v[0] * m[2] + v[1] * m[6] + v[2] * m[10];
}

float AngDeg(const float a[3], const float b[3])
{
    const float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la < 1e-6f || lb < 1e-6f) { return 0.0f; }
    float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return acosf(c) * 57.2957795f;
}

float DistMm(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;
}

void MoveBobTick()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    const double dt = g_mbPrevT.QuadPart ? static_cast<double>(t.QuadPart - g_mbPrevT.QuadPart) / static_cast<double>(f.QuadPart) : 0.0;
    g_mbPrevT = t;
    float m[12];
    unsigned va = 0;
    XrPosef head, ctl;
    if (!CamHookGetRenderedViewMatrix(m, &va) || !GetHeadPose(head) || !GetControllerPose(kRightHand, ctl)) {
        g_mbHave = false;
        return;
    }
    const float viewPos[3] = { m[3], m[7], m[11] };
    // controller in the head frame
    const XrQuaternionf hi = { -head.orientation.x, -head.orientation.y, -head.orientation.z, head.orientation.w };
    float cf[3], cfh[3], cp[3], cph[3];
    RotateByQuat(ctl.orientation, 0.0f, 0.0f, -1.0f, cf[0], cf[1], cf[2]);
    RotateByQuat(hi, cf[0], cf[1], cf[2], cfh[0], cfh[1], cfh[2]);
    cp[0] = ctl.position.x - head.position.x; cp[1] = ctl.position.y - head.position.y; cp[2] = ctl.position.z - head.position.z;
    RotateByQuat(hi, cp[0], cp[1], cp[2], cph[0], cph[1], cph[2]);
    // gun
    float bdir[3], bpos[3], gunV[3] = {}, muzV[3] = {};
    unsigned bage = 0;
    const char* bwhy = nullptr;
    const bool gun = FireAimFromBarrel && GetPublishedBarrelBore(bdir, &bage, &bwhy, bpos);
    if (gun) {
        ToView(m, bdir, gunV);
        const float rel[3] = { bpos[0] - viewPos[0], bpos[1] - viewPos[1], bpos[2] - viewPos[2] };
        ToView(m, rel, muzV);
    }
    // grapple ray (as PointHook last cast it)
    float grapD[3] = {}, grapO[3] = {};
    const bool grap = g_mbGrapMs && GetTickCount64() - g_mbGrapMs <= 150;
    if (grap) {
        const float d[3] = { g_mbGrapDir[0], g_mbGrapDir[1], g_mbGrapDir[2] };
        const float o[3] = { g_mbGrapOrigin[0] - viewPos[0], g_mbGrapOrigin[1] - viewPos[1], g_mbGrapOrigin[2] - viewPos[2] };
        ToView(m, d, grapD);
        ToView(m, o, grapO);
    }
    if (g_mbHave && dt > 0.004 && dt < 0.05) {
        const float dx = viewPos[0] - g_mbPrevViewPos[0], dz = viewPos[2] - g_mbPrevViewPos[2];
        const float sp = sqrtf(dx * dx + dz * dz) / static_cast<float>(dt);
        g_mbSpeed = g_mbSpeed * 0.9f + sp * 0.1f;
        const int sb = g_mbSpeed < 0.5f ? 0 : (g_mbSpeed < 4.5f ? 1 : 2);
        MbAcc& a = g_mb[sb * 2 + (gun ? 1 : 0)];
        ++a.n;
        const float ca = AngDeg(cfh, g_mbPrevCtlD);
        a.ctlAng += ca; a.ctlPos += DistMm(cph, g_mbPrevCtlP);
        if (ca > a.ctlAngMax) { a.ctlAngMax = ca; }
        const float cv = fabsf((viewPos[1] - g_mbPrevViewPos[1]) - (head.position.y - g_mbPrevHmdY)) * 1000.0f;
        a.camVert += cv;
        if (cv > a.camVertMax) { a.camVertMax = cv; }
        if (gun && g_mbPrevGunOk) {
            const float ga = AngDeg(gunV, g_mbPrevGun);
            a.gunAng += ga; a.gunPos += DistMm(muzV, g_mbPrevMuz);
            a.gunH += fabsf(gunV[0] - g_mbPrevGun[0]) * 57.2957795f;   // sideways (view right) - small-angle
            a.gunV += fabsf(gunV[1] - g_mbPrevGun[1]) * 57.2957795f;   // vertical (view up)
            if (ga > a.gunAngMax) { a.gunAngMax = ga; }
            ++a.nGun;
        }
        if (grap && g_mbPrevGrapOk) {
            const float ra = AngDeg(grapD, g_mbPrevGrapD);
            a.grapAng += ra; a.grapPos += DistMm(grapO, g_mbPrevGrapO);
            if (ra > a.grapAngMax) { a.grapAngMax = ra; }
            ++a.nGrap;
        }
    }
    for (int k = 0; k < 3; ++k) {
        g_mbPrevGun[k] = gunV[k]; g_mbPrevMuz[k] = muzV[k]; g_mbPrevGrapD[k] = grapD[k]; g_mbPrevGrapO[k] = grapO[k];
        g_mbPrevCtlD[k] = cfh[k]; g_mbPrevCtlP[k] = cph[k]; g_mbPrevViewPos[k] = viewPos[k];
    }
    g_mbPrevHmdY = head.position.y;
    g_mbPrevGunOk = gun;
    g_mbPrevGrapOk = grap;
    g_mbHave = true;
    if (!g_mbFirstSaid) {
        g_mbFirstSaid = true;
        // [PROBE: MOVEBOB] gate=diag_move_bob  site=HEADSET  log-only=yes
        LOG("grappleaim: [MOVEBOB] FIRST SAMPLE (logged unconditionally) | rendered view age %u ms | gun bore %s | grapple ray %s | "
            "controller-in-head fwd (%.3f, %.3f, %.3f) pos (%.3f, %.3f, %.3f) m. Per 5 s: per-frame MEAN steps by bucket.",
            va, gun ? "fresh" : "none", grap ? "fresh" : "none", cfh[0], cfh[1], cfh[2], cph[0], cph[1], cph[2]);
    }
    const ULONGLONG now = GetTickCount64();
    if (g_mbSumMs == 0) { g_mbSumMs = now; }
    if (now - g_mbSumMs < 5000) { return; }
    g_mbSumMs = now;
    static const char* const kNames[6] = { "still/no-gun", "still/gun", "walk/no-gun", "walk/gun", "sprint/no-gun", "sprint/gun" };
    for (int i = 0; i < 6; ++i) {
        const MbAcc& a = g_mb[i];
        if (a.n < 20) { continue; }
        // [PROBE: MOVEBOB] gate=diag_move_bob  site=HEADSET  log-only=yes
        LOG("grappleaim: [MOVEBOB] 5 s %-13s n %4ld | CONTROLLER vs head %.3f deg (max %.2f) %.2f mm | GUN bore in view %.3f deg "
            "(sideways %.3f, vertical %.3f; max %.2f) muzzle %.2f mm (n %ld) | GRAPPLE ray %.3f deg (max %.2f) origin %.2f mm (n %ld) | "
            "CAMERA vertical beyond head %.2f mm (max %.2f) | speed EMA now %.2f m/s",
            kNames[i], a.n, a.ctlAng / a.n, a.ctlAngMax, a.ctlPos / a.n,
            a.nGun ? a.gunAng / a.nGun : -1.0, a.nGun ? a.gunH / a.nGun : -1.0, a.nGun ? a.gunV / a.nGun : -1.0, a.gunAngMax,
            a.nGun ? a.gunPos / a.nGun : -1.0, a.nGun,
            a.nGrap ? a.grapAng / a.nGrap : -1.0, a.grapAngMax, a.nGrap ? a.grapPos / a.nGrap : -1.0, a.nGrap,
            a.camVert / a.n, a.camVertMax, g_mbSpeed);
    }
    for (MbAcc& a : g_mb) { a = MbAcc{}; }
}

char CrosshairHookBody(void* self);

// diag_ray_threads v2: times the WHOLE HUD update (the game's slot-178 body + everything of ours, incl. the gun-reticle cast).
char __fastcall CrosshairHook(void* self)
{
    if (!DiagRayThreads) {
        return CrosshairHookBody(self);
    }
    if (g_qpcUs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcUs = 1e6 / static_cast<double>(f.QuadPart);
    }
    const LONG slot = (InterlockedIncrement(&g_hudIdx) - 1) & (kHudRing - 1);
    const LONG64 t0 = QpcNow();
    InterlockedExchange64(&g_hudEnd[slot], 0);
    InterlockedExchange64(&g_hudStart[slot], t0);
    InterlockedIncrement(&g_hudInFlight);
    const char r = CrosshairHookBody(self);
    InterlockedDecrement(&g_hudInFlight);
    const LONG64 t1 = QpcNow();
    InterlockedExchange64(&g_hudEnd[slot], t1);
    InterlockedAdd64(&g_hudDurSum, t1 - t0);
    if (t1 - t0 > g_hudDurMax) { InterlockedExchange64(&g_hudDurMax, t1 - t0); }
    InterlockedIncrement(&g_hudDurN);
    return r;
}

// hide_game_crosshair (2026-11-28): every path of CrosshairHookBody that has no VR use for the game's crosshair this update ends
// here. Setting off = the behaviour before it (put the group back at its layout spot once if it was moved); on = parked far
// off-screen, the same parking the 3D reticles use (no visibility-state change - HudCrosshair owns that). Re-parked each update
// while hidden, as the 3D reticles do. Returns true only when it put the group back (the grapple-marker path counts restores).
volatile LONG g_xhHideSaid = 0;
bool IdleCrosshair(void* self)
{
    switch (xhairhide::DecideIdle(HideGameCrosshair, g_xhMoved)) {
    case xhairhide::IdleAction::Park: {
        float off[4] = { g_xhLayout[0] - 100000.0f, g_xhLayout[1] - 100000.0f, g_xhLayout[2], 0.0f };
        __try {
            g_setRel(self, off);
            g_xhMoved = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_xhFault);
            return false;
        }
        if (InterlockedCompareExchange(&g_xhHideSaid, 1, 0) == 0) {
            LOG("grappleaim: hide_game_crosshair: FIRST PARK (logged once) - the game's centre crosshair %p leaves the screen while "
                "no VR use of it is active (layout spot (%.1f, %.1f) kept for when the setting is turned off) | fire_reticle_3d=%d "
                "bow_reticle_3d=%d grapple_crosshair_marker=%d keep their own placement",
                self, g_xhLayout[0], g_xhLayout[1], FireReticle3d ? 1 : 0, BowReticle3d ? 1 : 0, GrappleCrosshairMarker ? 1 : 0);
        }
        return false;
    }
    case xhairhide::IdleAction::Restore: {
        const bool ok = RestoreMarker(self);
        g_xhMoved = false;
        return ok;
    }
    case xhairhide::IdleAction::None:
        break;
    }
    return false;
}

char CrosshairHookBody(void* self)
{
    const char r = g_trueXhair(self);
    if (DiagRayThreads) {
        RayThreadsTick();   // COST: one spinlocked tally per HUD update + one line per 5 s with shots
    }
    if (DiagMoveBob) {
        MoveBobTick();      // COST: a few seqlock reads + small vector maths per HUD update; 6 lines per 5 s at most
    }
    if (!xhairhide::HookActive(GrappleCrosshairMarker, FireReticle3d, HideGameCrosshair)) {
        // Everything off (live): a group a mode left moved / parked goes back once, then nothing more is written.
        if (g_xhMoved && g_xhElem == self) {
            IdleCrosshair(self);
        }
        return r;
    }
    if (g_xhElem != self) {   // first sight of this HUD element (or a new one after a HUD reload): its layout spot
        if (!CaptureLayout(self)) {
            InterlockedIncrement(&g_xhFault);
            return r;
        }
        g_xhElem = self;
        g_xhMoved = false;
    }
    if (FireReticle3d) {
        // COST: one Raytrace (the game's own grapple cast makes the same call per HUD update) + four camera reads, and
        // only while a gun's bore is fresh; otherwise one seqlock read.
        const int g = GunReticle(self);
        GunReticleSummary();
        if (g == 0) {
            g_xhMoved = true;
            return r;
        }
        if (g == 2) {
            IdleCrosshair(self);   // hide_game_crosshair: parked; else back at its spot if it was moved
            return r;
        }
        // g == 1 (no gun bore): the bow (INV-496). COST: one seqlock read when no bow is held; while drawn <= 16 casts.
        if (BowReticle3d) {
            const int b = BowReticle(self);
            BowReticleSummary();
            if (b == 0 || b == 3) {   // drawn: the 3D dot is up / held, not drawn: hidden - the HUD crosshair is parked either way
                g_xhMoved = true;
                return r;
            }
            if (b == 2) {
                IdleCrosshair(self);
                return r;
            }
        }
    }
    if (!GrappleCrosshairMarker) {
        IdleCrosshair(self);   // no gun / bow reticle and no grapple marker: the plain centre crosshair (hidden by the setting)
        return r;
    }
    float hit[3], centre[3];
    bool haveCentre, hitOk;
    ULONGLONG tick;
    Lock(g_hitLock);
    hit[0] = g_hit[0]; hit[1] = g_hit[1]; hit[2] = g_hit[2];
    centre[0] = g_centre[0]; centre[1] = g_centre[1]; centre[2] = g_centre[2];
    haveCentre = g_haveCentre;
    hitOk = g_hitOk;
    tick = g_hitTick;
    Unlock(g_hitLock);
    const bool fresh = tick != 0 && haveCentre && GetTickCount64() - tick <= kFreshMs;
    if (fresh && !hitOk && GrappleMarkerHideOnMiss) {
        // The cast hit nothing: the ray's END is 25 m on, and drawing the dot there made it look "on" surfaces the ray
        // passed. Park the group far off-screen instead (no visibility-state change - HudCrosshair owns that).
        float off[4] = { g_xhLayout[0] - 100000.0f, g_xhLayout[1] - 100000.0f, g_xhLayout[2], 0.0f };
        __try {
            g_setRel(self, off);
            g_xhMoved = true;
            InterlockedIncrement(&g_xhHidden);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_xhFault);
        }
        return r;
    }
    if (!fresh) {
        if (!g_xhMoved && tick != 0) {
            InterlockedIncrement(&g_xhStale);
        }
        if (IdleCrosshair(self)) { InterlockedIncrement(&g_xhRestores); }
        return r;
    }
    float np[4] = {}, ph[4] = {}, pc[4] = {};
    const int res = PlaceMarker(self, hit, centre, np, ph, pc);
    if (res == 0) {
        g_xhMoved = true;
        InterlockedIncrement(&g_xhMoves);
        if (InterlockedCompareExchange(&g_firstMarkerSaid, 1, 0) == 0) {
            // [PROBE: GRAPPLEAIM] gate=grapple_crosshair_marker  site=HEADSET  log-only=yes
            LOG("grappleaim: [GRAPPLEAIM] FIRST MARKER MOVE (logged regardless of diag_grapple_aim) | element %p "
                "layout (%.1f, %.1f, %.1f) | hit (%.2f, %.2f, %.2f) projects (%.1f, %.1f, %.1f), screen centre "
                "projects (%.1f, %.1f, %.1f) | new relative pos (%.1f, %.1f)",
                self, g_xhLayout[0], g_xhLayout[1], g_xhLayout[2], hit[0], hit[1], hit[2], ph[0], ph[1], ph[2],
                pc[0], pc[1], pc[2], np[0], np[1]);
        }
    } else {
        InterlockedIncrement(res == 1 ? &g_xhBehind : &g_xhFault);
        IdleCrosshair(self);
    }
    return r;
}

// ---------------------------------------------------------------- grapple_reticle_3d
constexpr uintptr_t kRopeVtRva   = 0x1507B48;   // HudRopeReticle primary vtable (gr3)
constexpr uintptr_t kRopeUpdRva  = 0x5AAD70;    // its slot 178 (gr3)
constexpr int       kRopeElemOff = 96;          // a1[12 + i]: 0 G_Valid, 1 G_Invalid, 2 G_Default (+5AA910 names, gr2; +5AAD70 :65)
XhairFn       g_trueRope = nullptr;
volatile LONG g_ropeLive = 0;
void*         g_ropeElem = nullptr;
float         g_ropeLayout[4] = {};
bool          g_ropeParked = false;
volatile LONG g_gpSeq = 0;
float         g_gpLocal[3] = {};
float         g_gpDist = 0.0f;
int           g_gpState = 0;                    // 0 none, 1 can grapple (G_Valid), 2 refused (ctrl+193)
float         g_gpFov = 0.0f;
ULONGLONG     g_gpMs = 0;
volatile LONG g_gpFirstSaid = 0, g_gpFault = 0;
volatile LONG g_gpValid = 0, g_gpRefused = 0, g_gpNone = 0, g_gpHidden = 0, g_gpNoView = 0;
// Split of "shown without a target" (Jay 2026-09-28, movebob run: icon never showed, throws worked): no stored target vs a
// VALID stored target whose icon the game held back (G_Valid not visible: +5AAD70's anchor gate `sub_180D75DF0(ctrl)` - which
// includes the rope energy cost - failed and ctrl+195 clear); plus one line per change of the stored target's invalid flag.
volatile LONG g_gpHeld = 0, g_gpHeldFrozen = 0;
// v3 (Jay 2026-09-28 "Do it"): "can grapple" = a VALID stored target (stored && !+193) - what the throw commits (17/17 USABLE in the
// split runs) - no longer the game's G_Valid visibility, whose extra gate +D75DF0 held the icon back in 2,784 / 2,802 updates while
// every throw worked. The gate's clauses (gr4 :24-84) are read on those frames, log-only, to find which one differs between sessions.
volatile LONG g_gpGameVis = 0;                   // of our "can" frames, the game itself showed G_Valid
LONG          g_gcN = 0, g_gcExhaust = 0, g_gcCool = 0, g_gcGlobal = 0, g_gcGlobal96 = 0, g_gcZombie = 0, g_gcFault = 0;
float         g_gcStamMin = 1e9f, g_gcStamMax = -1e9f, g_gcCoolMax = 0.0f;
double        g_gcStamSum = 0.0;

// +D75DF0's clauses after the target checks, read on a held-back frame: stamina = owner[+4936]+16 unless its +32 flag (then 0)
// (compared there with fitness_connection_cost * sub_180CE1360 - NOT computed here); the timer [owner+4992]+116 > 0 (human only,
// owner vfunc +2168 false); the global [+1C16348]+3152 and its +96. The sub_180BABED0 / +7664 bit clause is not read.
void GateClauses(uintptr_t ctrl)
{
    __try {
        const OwnerFn ownerFn = *reinterpret_cast<const OwnerFn*>(*reinterpret_cast<const uintptr_t*>(ctrl) + 32);
        const uintptr_t owner = ownerFn(reinterpret_cast<void*>(ctrl));
        if (!owner) { ++g_gcFault; return; }
        const uintptr_t st = *reinterpret_cast<const uintptr_t*>(owner + 4936);
        if (st) {
            const bool ex = *reinterpret_cast<const unsigned char*>(st + 32) != 0;
            const float s = ex ? 0.0f : *reinterpret_cast<const float*>(st + 16);
            if (ex) { ++g_gcExhaust; }
            g_gcStamSum += s;
            if (s < g_gcStamMin) { g_gcStamMin = s; }
            if (s > g_gcStamMax) { g_gcStamMax = s; }
        }
        using BoolFn = unsigned char(__fastcall*)(uintptr_t);
        const BoolFn isZombie = *reinterpret_cast<const BoolFn*>(*reinterpret_cast<const uintptr_t*>(owner) + 2168);
        if (isZombie(owner)) {
            ++g_gcZombie;
        } else {
            const uintptr_t cd = *reinterpret_cast<const uintptr_t*>(owner + 4992);
            if (cd) {
                const float t = *reinterpret_cast<const float*>(cd + 116);
                if (t > 0.0f) { ++g_gcCool; if (t > g_gcCoolMax) { g_gcCoolMax = t; } }
            }
        }
        const uintptr_t gl = *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(g_gd + g_rCamGlobal) + 3152);
        if (gl) {
            ++g_gcGlobal;
            if (*reinterpret_cast<const uintptr_t*>(gl + 96)) { ++g_gcGlobal96; }
        }
        ++g_gcN;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_gcFault;
    }
}
bool          g_gpPrevRefused = false, g_gpPrevStored = false;
LONG          g_gpFlipLines = 0;
constexpr LONG kGpFlipMax = 80;
ULONGLONG     g_gpSumMs = 0;

bool GrappleDotRead(GrappleDot& out)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_gpSeq;
        if (s0 & 1) { continue; }
        MemoryBarrier();
        GrappleDot g;
        for (int k = 0; k < 3; ++k) { g.local[k] = g_gpLocal[k]; }
        g.dist = g_gpDist;
        g.state = g_gpState;
        g.fov = g_gpFov;
        const ULONGLONG t = g_gpMs;
        MemoryBarrier();
        if (g_gpSeq != s0) { continue; }
        if (!t) { return false; }
        const ULONGLONG now = GetTickCount64();
        g.ageMs = static_cast<unsigned>(now > t ? now - t : 0);
        if (g.ageMs > 150 || g.state == 0) { return false; }
        out = g;
        return true;
    }
    return false;
}

void GrapplePublish(int state, const float target[3], float dist, float fov)
{
    float m[12] = {};
    unsigned va = 0;
    float loc[3] = {};
    if (state != 0) {
        if (!CamHookGetRenderedViewMatrix(m, &va)) {
            InterlockedIncrement(&g_gpNoView);
            state = 0;
        } else {
            const float d[3] = { target[0] - m[3], target[1] - m[7], target[2] - m[11] };
            loc[0] = d[0] * m[0] + d[1] * m[4] + d[2] * m[8];
            loc[1] = d[0] * m[1] + d[1] * m[5] + d[2] * m[9];
            loc[2] = d[0] * m[2] + d[1] * m[6] + d[2] * m[10];
        }
    }
    InterlockedIncrement(&g_gpSeq);
    MemoryBarrier();
    for (int k = 0; k < 3; ++k) { g_gpLocal[k] = loc[k]; }
    g_gpDist = dist;
    g_gpState = state;
    g_gpFov = fov;
    g_gpMs = GetTickCount64();
    MemoryBarrier();
    InterlockedIncrement(&g_gpSeq);
}

// HudRopeReticle slot 178: the game's update first, then read what it decided, publish, and park the flat icons.
char __fastcall RopeReticleHook(void* self)
{
    const char r = g_trueRope(self);
    if (!Grapple3dReticle) {
        return r;
    }
    if (g_ropeElem != self) {
        __try {
            float p[4] = {};
            g_getRel(self, p);
            g_ropeLayout[0] = p[0]; g_ropeLayout[1] = p[1]; g_ropeLayout[2] = p[2]; g_ropeLayout[3] = 0.0f;
            g_ropeElem = self;
            g_ropeParked = false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_gpFault);
            return r;
        }
    }
    int state = 0;
    float target[3] = {}, dist = 0.0f;
    bool shown = false, validVis = false, stored = false, refused = false, frozen = false;
    __try {
        const unsigned char* s = static_cast<const unsigned char*>(self);
        shown = (s[kElemVisOff] & 1) != 0;
        const uintptr_t gv = *reinterpret_cast<const uintptr_t*>(s + kRopeElemOff);
        validVis = gv && (*reinterpret_cast<const unsigned char*>(gv + kElemVisOff) & 1) != 0;
        const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + g_rRopeGlobal);
        const uintptr_t pl = g ? *reinterpret_cast<const uintptr_t*>(g + 1920) : 0;
        const uintptr_t ctrl = pl ? *reinterpret_cast<const uintptr_t*>(pl + 3200) : 0;
        if (ctrl) {
            const float* c = reinterpret_cast<const float*>(ctrl);
            stored = (c[34] != 0.0f || c[35] != 0.0f || c[36] != 0.0f) && (c[37] != 0.0f || c[38] != 0.0f || c[39] != 0.0f);   // +5AA8D0
            refused = *reinterpret_cast<const unsigned char*>(ctrl + 193) != 0;
            frozen = *reinterpret_cast<const unsigned char*>(ctrl + 195) != 0;
            target[0] = c[34]; target[1] = c[35]; target[2] = c[36];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_gpFault);
        return r;
    }
    if (shown && stored && !refused) {
        state = 1;                                // a valid stored target = what a throw commits (v3)
        InterlockedIncrement(&g_gpValid);
        if (validVis) {
            InterlockedIncrement(&g_gpGameVis);
        } else {
            InterlockedIncrement(&g_gpHeld);      // the game would have held its icon back here
            if (frozen) { InterlockedIncrement(&g_gpHeldFrozen); }
            uintptr_t ctrl = 0;
            __try {
                const uintptr_t g = *reinterpret_cast<const uintptr_t*>(g_gd + g_rRopeGlobal);
                const uintptr_t pl = g ? *reinterpret_cast<const uintptr_t*>(g + 1920) : 0;
                ctrl = pl ? *reinterpret_cast<const uintptr_t*>(pl + 3200) : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ctrl = 0;
            }
            if (ctrl) { GateClauses(ctrl); }
        }
    } else if (shown && stored && refused) {
        // Counted only. Jay 2026-09-28: "don't worry about the 'no' state ... It's much more effective seeing either a Grapple
        // available or Nothing" (D-139) - a refused hit draws NOTHING, same as no target.
        InterlockedIncrement(&g_gpRefused);
    } else if (!shown) {
        InterlockedIncrement(&g_gpHidden);
    } else {
        InterlockedIncrement(&g_gpNone);          // no stored target at all
    }
    // One line whenever the stored target appears/disappears or its invalid flag flips, with what the player holds.
    if ((stored && refused) != g_gpPrevRefused || stored != g_gpPrevStored) {
        if (g_gpFlipLines < kGpFlipMax) {
            ++g_gpFlipLines;
            float bd[3], bp[3];
            unsigned ba = 0;
            const char* bw = nullptr;
            const bool gunOut = GetPublishedBarrelBore(bd, &ba, &bw, bp);
            // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=HEADSET  log-only=yes
            LOG_PERIODIC("grappleaim: [GRAPPLE3D] STORED TARGET %s | invalid(+193) %d -> %d | frozen(+195) %d | reticle shown %d, G_Valid visible %d | "
                "target (%.2f, %.2f, %.2f) | holding %s%s",
                stored == g_gpPrevStored ? "FLAG CHANGE" : (stored ? "APPEARED" : "CLEARED"), g_gpPrevRefused ? 1 : 0,
                (stored && refused) ? 1 : 0, frozen ? 1 : 0, shown ? 1 : 0, validVis ? 1 : 0, target[0], target[1], target[2],
                gunOut ? "a GUN (bore fresh)" : "no gun (melee / empty)",
                g_gpFlipLines == kGpFlipMax ? "  (line cap reached - further changes not logged)" : "");
        }
        g_gpPrevRefused = stored && refused;
        g_gpPrevStored = stored;
    }
    float fov = 0.0f;
    if (state != 0) {
        float cp[4] = {};
        __try {
            void* cam = CrosshairCamera();
            if (cam) {
                g_getPos(cam, cp);
                if (g_getFov) { fov = g_getFov(cam); }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        const float dx = target[0] - cp[0], dy = target[1] - cp[1], dz = target[2] - cp[2];
        dist = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    GrapplePublish(state, target, dist, fov);
    // The flat icons leave the screen for good while the 3D icon is on (the game repositions only the CHILDREN, +5AAD70 :97).
    __try {
        float off[4] = { g_ropeLayout[0] - 100000.0f, g_ropeLayout[1] - 100000.0f, g_ropeLayout[2], 0.0f };
        g_setRel(self, off);
        g_ropeParked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_gpFault);
    }
    if (InterlockedCompareExchange(&g_gpFirstSaid, 1, 0) == 0) {
        // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=HEADSET  log-only=yes
        LOG("grappleaim: [GRAPPLE3D] FIRST ROPE-RETICLE UPDATE (logged unconditionally) | element %p layout (%.1f, %.1f) | shown %d "
            "G_Valid visible %d stored target %d refused(+193) %d -> state %d (0 none 1 can 2 refused) | target (%.2f, %.2f, %.2f) "
            "%.2f m | flat icons parked",
            self, g_ropeLayout[0], g_ropeLayout[1], shown ? 1 : 0, validVis ? 1 : 0, stored ? 1 : 0, refused ? 1 : 0, state,
            target[0], target[1], target[2], dist);
    }
    const ULONGLONG now = GetTickCount64();
    if (g_gpSumMs == 0) { g_gpSumMs = now; }
    if (now - g_gpSumMs >= 5000) {
        g_gpSumMs = now;
        const LONG v = InterlockedExchange(&g_gpValid, 0), rf = InterlockedExchange(&g_gpRefused, 0);
        const LONG no = InterlockedExchange(&g_gpNone, 0), hd = InterlockedExchange(&g_gpHidden, 0);
        const LONG nv = InterlockedExchange(&g_gpNoView, 0), ft = InterlockedExchange(&g_gpFault, 0);
        const LONG ga = InterlockedExchange(&g_gunAimed, 0);
        const LONG he = InterlockedExchange(&g_gpHeld, 0), hf = InterlockedExchange(&g_gpHeldFrozen, 0);
        const LONG gvis = InterlockedExchange(&g_gpGameVis, 0);
        if (v + rf + no) {
            // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=HEADSET  log-only=yes
            LOG_PERIODIC("grappleaim: [GRAPPLE3D] 5 s | updates: can grapple (icon drawn) %ld - of which the game ALSO showed G_Valid %ld, the "
                "game held its icon back %ld (frozen +195 %ld) | refused %ld, no stored target %ld, reticle hidden %ld | no rendered view "
                "%ld, faults %ld | grapple rays aimed down a gun barrel %ld", v, gvis, he, hf, rf, no, hd, nv, ft, ga);
            if (g_gcN || g_gcFault) {
                // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=HEADSET  log-only=yes
                LOG_PERIODIC("grappleaim: [GRAPPLE3D] 5 s | +D75DF0 clauses on the %ld held-back frames: stamina (owner+4936 +16) min %.3f mean %.3f "
                    "max %.3f, exhausted-flag %ld | timer [owner+4992]+116 > 0 on %ld (max %.3f), zombie-owner %ld | global +3152 set %ld "
                    "(+96 set %ld) | faults %ld  (stamina vs fitness_connection_cost x sub_180CE1360 and the +7664 bit clause NOT read)",
                    g_gcN, g_gcN ? g_gcStamMin : 0.0f, g_gcN ? g_gcStamSum / g_gcN : 0.0, g_gcN ? g_gcStamMax : 0.0f, g_gcExhaust,
                    g_gcCool, g_gcCoolMax, g_gcZombie, g_gcGlobal, g_gcGlobal96, g_gcFault);
            }
        }
        g_gcN = g_gcExhaust = g_gcCool = g_gcGlobal = g_gcGlobal96 = g_gcZombie = g_gcFault = 0;
        g_gcStamMin = 1e9f; g_gcStamMax = -1e9f; g_gcCoolMax = 0.0f; g_gcStamSum = 0.0;
    }
    return r;
}

// ---------------------------------------------------------------- installs (each once, each reports itself)
// The g_r* rvas above, as the loaded build has them. Once, with the gamedll mapped, before g_gd is set.
void ResolveRvas()
{
    using ::dlvr::buildrva::Gd;
    g_rLeftRet = Gd(kLeftRet);
    g_rCamGlobal = Gd(kCamGlobalRva);
    g_rRopeGlobal = Gd(kRopeGlobalRva);
    g_rOriginAimRet = Gd(kOriginAimRet);
    g_rOriginLedgeRet = Gd(kOriginLedgeRet);
    g_rBulletRayRet1 = Gd(kBulletRayRet1);
    g_rBulletRayRet2 = Gd(kBulletRayRet2);
    g_rRayRet = Gd(kRayRet);
    g_rRay2Ret = Gd(kRay2Ret);
    g_rSpread = Gd(kSpreadRva);
    g_rBulletCast = Gd(kBulletCastRva);
    g_rArrowSpeed = Gd(kArrowSpeedRva);
}

void InstallPoint(HMODULE gd, HMODULE eng)
{
    const uintptr_t retRva = ::dlvr::buildrva::Gd(kRetRva);   // the EGL rva on the Epic build (buildrva table)
    g_retAddr = reinterpret_cast<uintptr_t>(gd) + retRva;
    // The slot goes live the instant it is patched, so the pass-through target must be valid BEFORE the swap:
    // take it from the engine's export (what the loader put in the slot), then adopt whatever the slot really
    // held once PatchIat returns it (identical unless something else hooked it first).
    g_realPoint = reinterpret_cast<PointFn>(GetProcAddress(eng, kPointName));
    // Verify the call site before trusting the return address: +D760A5 must be `call qword ptr [rip+rel32]`
    // (FF 15) and its target slot must be the import we patch. A different game build refuses by name.
    const unsigned char* site = reinterpret_cast<const unsigned char*>(gd) + (retRva - 6);
    const bool siteOk = site[0] == 0xFF && site[1] == 0x15;
    void* orig = (siteOk && g_realPoint)
                     ? PatchIat(gd, "engine_x64_rwdi.dll", kPointName, reinterpret_cast<void*>(&PointHook))
                     : nullptr;
    if (orig) {
        g_realPoint = reinterpret_cast<PointFn>(orig);
        const int32_t rel = *reinterpret_cast<const int32_t*>(site + 2);
        void* const* slot = reinterpret_cast<void* const*>(site + 6 + rel);
        if (*slot != reinterpret_cast<void*>(&PointHook)) {
            // The call site reads a different slot than the one PatchIat changed: undo and refuse.
            PatchIat(gd, "engine_x64_rwdi.dll", kPointName, orig);
            // [PROBE: GRAPPLEAIM] gate=grapple_hand_aim  site=FLAT  log-only=yes
            LOG("grappleaim: [GRAPPLEAIM] *** NOT INSTALLED - the call at gamedll+0x%llX does not read the %s import "
                "slot (it reads %p). Different game build? Patch undone; the grapple stays head-aimed. ***",
                static_cast<unsigned long long>(kRetRva - 6), kPointName, static_cast<const void*>(slot));
            return;
        }
    }
    // [PROBE: GRAPPLEAIM] gate=grapple_hand_aim  site=FLAT  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] %s | call site gamedll+0x%llX bytes %02X %02X (want FF 15) | return address "
        "filter %p | original GetOnScreenPointF %p | GetPosition %p",
        orig ? "INSTALLED - the grapple ray follows the right controller"
             : "*** NOT INSTALLED - call-site bytes or the import were not found; the grapple stays head-aimed. "
               "Set grapple_hand_aim=0. ***",
        static_cast<unsigned long long>(kRetRva - 6), site[0], site[1], reinterpret_cast<void*>(g_retAddr),
        orig, reinterpret_cast<void*>(g_getPos));
}

void InstallOrigin(HMODULE gd)
{
    int attempts = 0;
    const bool ok = InstallSingleGameDetour(reinterpret_cast<uintptr_t>(gd) + kOriginRva, kOriginBytes,
                                            sizeof(kOriginBytes), reinterpret_cast<void*>(&OriginHook),
                                            reinterpret_cast<void**>(&g_trueOrigin), "grappleaim origin", &attempts);
    InterlockedExchange(&g_originLive, ok ? 1 : 0);
    // [PROBE: GRAPPLEAIM] gate=grapple_hand_origin  site=FLAT  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] A2 origin detour on gamedll+0x%llX: %s after %d commit attempt(s) | substitutes only "
        "for returns to +%llX (aim ray) / +%llX (ledge assist)%s",
        static_cast<unsigned long long>(kOriginRva), ok ? "INSTALLED (committed)" : "*** NOT INSTALLED ***", attempts,
        static_cast<unsigned long long>(kOriginAimRet), static_cast<unsigned long long>(kOriginLedgeRet),
        ok ? "" : " - prologue mismatch or commit failure (the line above names it); the ray starts at the EYE. "
                  "Set grapple_hand_origin=0.");
}

void InstallThrowProbe(HMODULE gd)
{
    int attempts = 0;
    const bool ok = InstallSingleGameDetour(reinterpret_cast<uintptr_t>(gd) + kCommitRva, kCommitBytes,
                                            sizeof(kCommitBytes), reinterpret_cast<void*>(&CommitHook),
                                            reinterpret_cast<void**>(&g_trueCommit), "grappleaim throw commit",
                                            &attempts);
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=FLAT  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] throw-commit probe detour on gamedll+0x%llX: %s after %d commit attempt(s) | "
        "pass-through; logs each state -> 3 (the throw, caller +D7879C)%s",
        static_cast<unsigned long long>(kCommitRva), ok ? "INSTALLED (committed)" : "*** NOT INSTALLED ***", attempts,
        ok ? "" : " - prologue mismatch or commit failure (the line above names it); no THROW COMMIT lines this run");
    int va = 0;
    const bool vok = InstallSingleGameDetour(reinterpret_cast<uintptr_t>(gd) + kVaultRva, kVaultBytes, sizeof(kVaultBytes),
                                             reinterpret_cast<void*>(&VaultHook), reinterpret_cast<void**>(&g_trueVault),
                                             "grappleaim vault redirect", &va);
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=FLAT  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] vault-redirect probe detour on gamedll+0x%llX: %s after %d commit attempt(s)%s",
        static_cast<unsigned long long>(kVaultRva), vok ? "INSTALLED (committed)" : "*** NOT INSTALLED ***", va,
        vok ? " | pass-through; logs when the pull is redirected from the hook to a ledge" : " - no VAULT lines this run");
    // Refusal-gate re-check: the volume check's float argument, read from the game (gr30: 0xBF800000 = -1.0).
    __try {
        g_volArg = *reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(kVolArgRva));
        g_volArgOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_volArgOk = false;
    }
    int da = 0;
    const bool dok = InstallSingleGameDetour(reinterpret_cast<uintptr_t>(gd) + kDispatchRva, kDispatchBytes,
                                             sizeof(kDispatchBytes), reinterpret_cast<void*>(&DispatchHook),
                                             reinterpret_cast<void**>(&g_trueDispatch), "grappleaim pull trace", &da);
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=FLAT  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] pull-trace probe detour on gamedll+0x%llX (rope per-update dispatcher): %s after %d commit "
        "attempt(s)%s | refusal-gate re-check: volume arg %.3f (%s, gr30 read -1.0), 2nd cast filter return +%llX",
        static_cast<unsigned long long>(kDispatchRva), dok ? "INSTALLED (committed)" : "*** NOT INSTALLED ***", da,
        dok ? " | pass-through; PULL lines per throw" : " - prologue mismatch or commit failure (the line above names it); "
                                                        "no PULL lines this run", g_volArg,
        g_volArgOk ? "read" : "NOT READABLE - default used", static_cast<unsigned long long>(kRay2Ret));
}

void InstallLedgeProbe(HMODULE gd, HMODULE eng)
{
    g_iconGate = reinterpret_cast<GateFn>(reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(0xD75DF0));   // refusal probe: icon gate 2 (gr4)
    // The left-vector capture first (pass-through for its 141 other callers), then the assist detour.
    g_realLeft = reinterpret_cast<LeftFn>(GetProcAddress(eng, kLeftName));   // valid before the slot goes live
    void* orig = g_realLeft ? PatchIat(gd, "engine_x64_rwdi.dll", kLeftName, reinterpret_cast<void*>(&LeftHook)) : nullptr;
    if (orig) {
        g_realLeft = reinterpret_cast<LeftFn>(orig);
    }
    int attempts = 0;
    const bool ok = orig && InstallSingleGameDetour(reinterpret_cast<uintptr_t>(gd) + kAssistRva, kAssistBytes,
                                                    sizeof(kAssistBytes), reinterpret_cast<void*>(&AssistHook),
                                                    reinterpret_cast<void**>(&g_trueAssist), "grappleaim ledge assist",
                                                    &attempts);
    // [PROBE: GRAPPLEAIM] gate=diag_grapple_aim  site=FLAT  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] ledge-assist probe: GetLeftVector IAT %s (filter return +%llX) | detour on gamedll+0x%llX "
        "%s after %d commit attempt(s)%s",
        orig ? "hooked" : "*** NOT FOUND ***", static_cast<unsigned long long>(kLeftRet),
        static_cast<unsigned long long>(kAssistRva), ok ? "INSTALLED (committed)" : "*** NOT INSTALLED ***", attempts,
        ok ? " | pass-through; logs body-facing vs what the assist did" : " - no LEDGE ASSIST lines this run");
}

void InstallMarker(HMODULE gd, HMODULE eng)
{
    g_getFwd = reinterpret_cast<PosFn>(GetProcAddress(eng, kFwdName));
    g_getLeft = reinterpret_cast<PosFn>(GetProcAddress(eng, kCamLeftName));
    g_getUp = reinterpret_cast<PosFn>(GetProcAddress(eng, kCamUpName));
    g_getFov = reinterpret_cast<FovFn>(GetProcAddress(eng, kFovName));
    g_getRel = reinterpret_cast<RelPosFn>(GetProcAddress(eng, kGetRelName));
    g_setRel = reinterpret_cast<SetPosFn>(GetProcAddress(eng, kSetRelName));
    g_project = reinterpret_cast<ProjectFn>(reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(kProjectRva));
    g_realRay = reinterpret_cast<RaytraceFn>(GetProcAddress(eng, kRayName));
    const char* why = nullptr;
    if (!g_getPos || !g_getRel || !g_setRel || !g_realRay) {
        why = "an engine export is missing (GetPosition / Get/SetRelativePos / Raytrace)";
    }
    // The crosshair's update slot must be exactly what gr12 read before we swap it.
    void** slot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(kXhairVtRva) + 8 * kXhairSlot);
    const uintptr_t expect = reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(kXhairUpdRva);
    if (!why && reinterpret_cast<uintptr_t>(*slot) != expect) {
        why = "HudCrosshair vtable slot 178 does not hold +550FC0 (different game build?)";
    }
    void* rayOrig = nullptr;
    if (!why) {
        rayOrig = PatchIat(gd, "engine_x64_rwdi.dll", kRayName, reinterpret_cast<void*>(&RaytraceHook));
        if (!rayOrig) {
            why = "the Raytrace import slot was not found";
        } else {
            g_realRay = reinterpret_cast<RaytraceFn>(rayOrig);
        }
    }
    if (!why) {
        g_trueXhair = reinterpret_cast<XhairFn>(*slot);   // valid before the swap goes live
        DWORD old = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            InterlockedExchangePointer(slot, reinterpret_cast<void*>(&CrosshairHook));
            VirtualProtect(slot, sizeof(void*), old, &old);
        } else {
            why = "VirtualProtect on the HudCrosshair vtable slot failed";
        }
    }
    InterlockedExchange(&g_markerLive, why ? 0 : 1);
    // [PROBE: GRAPPLEAIM] gate=grapple_crosshair_marker  site=FLAT  log-only=yes
    LOG("grappleaim: [GRAPPLEAIM] A3 crosshair marker: %s%s | modes: grapple_crosshair_marker=%d "
        "fire_reticle_3d=%d hide_game_crosshair=%d (fire_aim_from_barrel=%d%s) | camera left/up exports %s | Raytrace IAT original %p (filter "
        "return +%llX) | HudCrosshair vt +%llX slot %d (was +%llX) | projection +%llX",
        why ? "*** NOT INSTALLED - " : "INSTALLED - the centre crosshair hook is live",
        why ? why : "", GrappleCrosshairMarker ? 1 : 0, FireReticle3d ? 1 : 0, HideGameCrosshair ? 1 : 0,
        FireAimFromBarrel ? 1 : 0,
        (FireReticle3d && !FireAimFromBarrel) ?
            " - *** the gun reticle NEVER shows: no bore is published ***" : "",
        (g_getLeft && g_getUp) ? "found" : "*** MISSING - fire_reticle_3d faults every frame, the HUD dot stays ***",
        rayOrig, static_cast<unsigned long long>(kRayRet),
        static_cast<unsigned long long>(kXhairVtRva), kXhairSlot, static_cast<unsigned long long>(kXhairUpdRva),
        static_cast<unsigned long long>(kProjectRva));
}

void InstallRope(HMODULE gd, HMODULE eng)
{
    if (!g_getRel) { g_getRel = reinterpret_cast<RelPosFn>(GetProcAddress(eng, kGetRelName)); }
    if (!g_setRel) { g_setRel = reinterpret_cast<SetPosFn>(GetProcAddress(eng, kSetRelName)); }
    if (!g_getFov) { g_getFov = reinterpret_cast<FovFn>(GetProcAddress(eng, kFovName)); }
    const char* why = nullptr;
    if (!g_getRel || !g_setRel || !g_getPos) {
        why = "an engine export is missing (Get/SetRelativePos / GetPosition)";
    }
    void** slot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(kRopeVtRva) + 8 * kXhairSlot);
    const uintptr_t expect = reinterpret_cast<uintptr_t>(gd) + ::dlvr::buildrva::Gd(kRopeUpdRva);
    if (!why && reinterpret_cast<uintptr_t>(*slot) != expect) {
        why = "HudRopeReticle vtable slot 178 does not hold +5AAD70 (different game build?)";
    }
    if (!why) {
        g_trueRope = reinterpret_cast<XhairFn>(*slot);
        DWORD old = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            InterlockedExchangePointer(slot, reinterpret_cast<void*>(&RopeReticleHook));
            VirtualProtect(slot, sizeof(void*), old, &old);
        } else {
            why = "VirtualProtect on the HudRopeReticle vtable slot failed";
        }
    }
    InterlockedExchange(&g_ropeLive, why ? 0 : 1);
    // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=FLAT  log-only=yes
    LOG("grappleaim: [GRAPPLE3D] rope reticle hook: %s%s | HudRopeReticle vt +%llX slot %d (was +%llX) | grapple_aim_from_gun=%d "
        "(needs grapple_hand_aim=%d, fire_aim_from_barrel=%d)",
        why ? "*** NOT INSTALLED - " : "INSTALLED - the grapple's CAN/CAN'T icon is drawn in 3D", why ? why : "",
        static_cast<unsigned long long>(kRopeVtRva), kXhairSlot, static_cast<unsigned long long>(kRopeUpdRva),
        GrappleAimFromGun ? 1 : 0, GrappleHandAim ? 1 : 0, FireAimFromBarrel ? 1 : 0);
}

}  // namespace

bool GetGrappleDot(GrappleDot& out)
{
    return GrappleDotRead(out);
}

// The bow controller's state (owner +3120: active +16, state +32 - INV-496), for boneblend's PublishBowRay (game thread).
// False = no owner / no controller / not active / unreadable. Four pointer reads.
bool ReadBowCtrlState(int* state)
{
    if (!g_gd) { return false; }
    __try {
        const uintptr_t owner = XhairOwner();
        const uintptr_t ctrl = owner ? *reinterpret_cast<const uintptr_t*>(owner + kOwnerBowCtrlOff) : 0;
        if (!ctrl || !*reinterpret_cast<const unsigned char*>(ctrl + kBowActiveOff)) { return false; }
        *state = *reinterpret_cast<const int*>(ctrl + kBowStateOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadBowCtrlPtr(uintptr_t* ctrlOut)
{
    if (!g_gd || !ctrlOut) { return false; }
    __try {
        const uintptr_t owner = XhairOwner();
        const uintptr_t ctrl = owner ? *reinterpret_cast<const uintptr_t*>(owner + kOwnerBowCtrlOff) : 0;
        if (!ctrl || !*reinterpret_cast<const unsigned char*>(ctrl + kBowActiveOff)) { return false; }
        *ctrlOut = ctrl;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GetGunDot(GunDot& out)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_g3Seq;
        if (s0 & 1) { continue; }
        MemoryBarrier();
        GunDot g;
        for (int k = 0; k < 3; ++k) { g.local[k] = g_g3Local[k]; g.gameOff[k] = g_g3GameOff[k]; }
        g.dist = g_g3Dist;
        g.hit = g_g3Hit;
        g.group = g_g3Group;
        g.dotVisible = g_g3DotVis;
        g.s0 = g_g3S0;
        g.s1 = g_g3S1;
        g.fov = g_g3Fov;
        for (int k = 0; k < 3; ++k) { g.shaft[k] = g_g3Shaft[k]; }
        g.shaftOk = g_g3ShaftOk;
        const ULONGLONG hm = g_g3HitMs;
        const ULONGLONG t = g_g3Ms;
        MemoryBarrier();
        if (g_g3Seq != s0) { continue; }
        if (!t) { return false; }
        const ULONGLONG now = GetTickCount64();
        g.ageMs = static_cast<unsigned>(now > t ? now - t : 0);
        if (g.ageMs > 150) { return false; }   // no gun out (the HUD update stopped publishing)
        g.hitAgeMs = hm ? static_cast<unsigned>(now > hm ? now - hm : 0) : 0xFFFFFFFFu;
        out = g;
        return true;
    }
    return false;
}

void GrappleAimInstall()
{
    if (!(GrappleHandAim || GrappleHandOrigin || GrappleCrosshairMarker || FireReticle3d ||
          Grapple3dReticle || HideGameCrosshair) || g_tried) {
        return;
    }
    HMODULE gd  = GetModuleHandleA("gamedll_x64_rwdi.dll");
    HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !eng) {
        return;   // not mapped yet - try again next poll
    }
    InterlockedExchange(&g_tried, 1);   // latch before the attempt: one install, one line per part
    ResolveRvas();
    g_gd = reinterpret_cast<uintptr_t>(gd);
    g_getPos = reinterpret_cast<PosFn>(GetProcAddress(eng, kPosName));
    if (!g_getPos) {
        // [PROBE: GRAPPLEAIM] gate=grapple_hand_aim  site=FLAT  log-only=yes
        LOG("grappleaim: [GRAPPLEAIM] *** NOTHING INSTALLED - engine_x64_rwdi.dll does not export %s. The grapple "
            "stays head-aimed. Set grapple_hand_aim, grapple_hand_origin and grapple_crosshair_marker to 0. ***",
            kPosName);
        return;
    }
    if (GrappleHandAim) {
        InstallPoint(gd, eng);
    }
    if (GrappleHandOrigin) {
        if (!GrappleHandAim) {
            // [PROBE: GRAPPLEAIM] gate=grapple_hand_origin  site=FLAT  log-only=yes
            LOG("grappleaim: [GRAPPLEAIM] A2 hand origin NOT INSTALLED - it needs grapple_hand_aim=1 (the origin moves "
                "only for a ray whose direction came from the hand).");
        } else {
            InstallOrigin(gd);
        }
    }
    if (xhairhide::HookActive(GrappleCrosshairMarker, FireReticle3d, HideGameCrosshair)) {
        InstallMarker(gd, eng);   // the HudCrosshair hook (hide_game_crosshair rides it too)
    }
    if (FireReticle3d) {
        InstallCrowdGuard(gd);   // before any HUD update can choose BulletCast (g_crowdGuardLive starts 0 = GunCast)
    }
    if (Grapple3dReticle) {
        InstallRope(gd, eng);
    }
    if (DiagGrappleAim) {
        InstallThrowProbe(gd);
        InstallLedgeProbe(gd, eng);
    }
}

}  // namespace dlvr
