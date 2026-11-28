// Redirects the ranged-weapon aim vector to a motion controller.
//
// See CLAUDE.md's "Decided: ComputeShootDir is the redirection target"
// section for the full reasoning. In short: this lets the game call its own
// ComputeShootDir exactly as it always has (gamedll+0xDA0B84's call site),
// then - right after that call returns - overwrites the result (a
// normalized unit vector at [rbp-0x48] in the caller) with a
// controller-derived direction instead. Deliberately NOT the original
// target this hook first tried (WeaponFireControllerRight's own write site,
// gamedll+0xDA4296) - that one is proven too hot to hook persistently
// (30,000-60,000 hits/second, hung the game). ComputeShootDir's caller only
// fires in a bounded per-shot burst, confirmed silent during aim-only
// testing with no shot fired.
#pragma once

#include <cstdint>

#include "controllers.h"   // Hand (ComputeAbsoluteAim's hand)

namespace dlvr {

// Off by default. The hook always logs the game's own original vector
// (throttled) regardless of this, so the coordinate relationship between it
// and a controller pose can be read off directly before trusting an
// override to be correct.
extern bool AimHookOverride;

// Absolute, live controller aim. Replaces the relative redirect, which
// inherited the grapple hook's accumulating pitch corruption in the game's
// own aim reference. Pitch comes entirely from the controller.
extern bool AimAbsolute;
// Flip ONLY if aiming right sends the shot left (mirrored horizontal plane).
extern bool AimMirrorYaw;

// Light smoothing on the HEAD orientation the absolute mapping is derived
// from. It does NOT smooth the controller, and therefore does not add any
// lag to where the shot goes - the shot direction stays live off the hand.
// AimHeadSmoothingMs is an exponential time constant in milliseconds; 0
// disables the filtering even with the toggle on.
extern bool  AimHeadSmoothing;
extern float AimHeadSmoothingMs;

// fire_move_penalty_off - UpdateAccuracy (+BC3750) always takes its aiming branch: the movement spread term is 0
// (INV-274 (6)). Two gamedll bytes, installed once from AimHookInstall. DEFAULT ON.
extern bool FireMovePenaltyOff;
// diag_fire_spread - [FIRESPREAD] per-shot readback of the spread inputs at DR2/DR3. Log-only. DEFAULT OFF.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagFireSpread = false;
inline constexpr bool DiagAimWfcrScan = false;
#else
extern bool DiagFireSpread;
extern bool DiagAimWfcrScan;   // diag_aim_wfcr_scan - starts the INV-155 heap scan (log field only). DEFAULT OFF.
#endif
// fire_barrel_origin - the firearm hit ray starts at the barrel (bullet+220) instead of the head point (INV-277).
// Detour on Bullet::Init, acting only for the dispatcher's call (+DA16DF). Installed once from AimHookInstall. DEFAULT ON.
extern bool FireBarrelOrigin;
// fire_aim_from_barrel - the shot's BASE direction is the held gun's bore (-X of its "barrel" element, published by
// boneblend's HookTail) instead of the controller aim pose; the cone still applies on top (INV-277 addendum). DEFAULT ON.
extern bool FireAimFromBarrel;
// fire_barrel_helper - the bore / shot start use the item's own BarrelHelperName element when it has one ("bolt_pose" +Z
// on crossbows, INV-494) instead of "barrel" -X, which on a crossbow points sideways. DEFAULT ON (Tier A, 2026-10-08).
extern bool FireBarrelHelper;
// bow_aim_from_bow - a bow arrow starts at the nock ("arrow_pose") and flies along the bow (its +Z), not from the head
// along the head's look (INV-495). Branch of the Bullet::Init detour for the bow's call (+D9D5D3). DEFAULT ON (Tier A).
extern bool BowAimFromBow;
// bow_fire_latched - the arrow leaves along the bow's last DRAWN direction (the reticle's line), not the release pose. LIVE. ON.
extern bool BowFireLatched;
// bow_reticle_3d (INV-496): the last fired arrow's acceleration (bullet +132 x +136..+144), captured at the bow branch of the
// Bullet::Init detour. False until an arrow has been fired this session.
bool GetLastArrowAccel(float out[3]);

// Advances the smoothed head orientation. Must be called from something that
// runs CONTINUOUSLY - ComputeAbsoluteAim only runs on a shot, and samples
// seconds apart cannot be filtered. dlvr_FilterScePad (~250Hz, and polled in
// both VR and flatscreen) is the call site.
void AimHeadSmoothUpdate();

// The right controller's pointing direction in GAME space, given the game's own
// direction `orig` (used for the head-heading pairing, and as the magnitude gate).
// Shared by the gun redirect and the grapple (grappleaim.cpp, R9) so the two can
// never disagree on the mapping. False = no pose / degenerate heading: leave the
// game's value alone.
// hand (grapple_aim_hand, 2026-11-28): the controller whose direction is mapped; the shot paths keep the right hand.
bool ComputeAbsoluteAim(float origX, float origY, float origZ, float out[3], Hand hand = kRightHand);

bool AimHookInstall();
void AimHookUninstall();
bool AimHookActive();

}  // namespace dlvr
