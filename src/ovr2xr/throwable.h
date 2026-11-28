// Throwing by hand (VR) - docs/superpowers/specs/2026-10-14-throwables-design.md, Build A. Settings live in throwable.cpp
// (each with its [DEP:] marker); the kSettings rows are in ovr2xr.cpp next to the throw_* rows. The decision is
// throwable_logic.h (unit-tested); throwable.cpp reads the inputs on the scePad poll, applies the L2 / R2 bytes the
// decision asks for, keeps the right hand's history and logs the THROWABLE event lines. The game-side detours
// (set-state / launch / aim point / controller Update) are Task 4 and live in throwable.cpp too.
#pragma once

#include "throwable_logic.h"

namespace dlvr {

extern bool  ThrowableHandEnable;   // throwable_hand_enable - master. DEFAULT ON (2026-10-16, Jay: "Ship disk for throwable").
extern float ThrowableFullSpeed;    // throwable_full_speed  - hand m/s that gives the item's own full speed
extern float ThrowableStillSpeed;   // throwable_still_speed - hand m/s below which a let-go is a drop
extern bool  ThrowableStarHandVelocity;   // throwable_star_hand_velocity - stars / knives / axes fly with the hand's
                                          // velocity (issue 4; flight detour +6E6D70). SHIPS ON (Tier A).

// Once per scePad poll, from dlvr_FilterScePad right after BowDrawPadByte and BEFORE the mod-menu block. Writes the
// L2 byte p[0x08] (255 = held by the layer, 0 = withheld) and the R2 byte p[0x09] (0 while a throwable is out), and may
// clear the D-pad left / right bits (0x80 / 0x20) for the polls a cancel holds a switch back. throwable_hand_enable=0
// with nothing in flight, or the game not resolved -> returns at its first line and the bytes are untouched.
void ThrowablePadByte(unsigned char* p);

// Called on every scePad poll, just before ThrowablePadByte, and self-throttled to once a second (camhook.cpp's 1 Hz
// install loop, where ThrowTick runs, is behind the invariants gate and outside Task 3's files): resolves the gamedll /
// engine bases once, installs the four game-side detours (Task 4: set-state, launch, aim point, the throw controller's
// Update - the pad side readies nothing until all four are live; a prologue mismatch refuses for the session), logs
// ARMED / NOT ARMED (naming the setting, the module or the detour) on every change, and the detours' 5 s status line
// (LOG_PERIODIC).
void ThrowableTick();

// The hand at the RT let-go (pad thread writes, game thread reads; a seqlock). posWorld = the right grip in game world
// space (camera + R*(grip - reported head)), velWorld = the hand's velocity in game world space (m/s, the stroke's
// direction x the fastest speed in the window; zero when no motion was measured), ageMs = ms since the let-go.
// False = no snapshot yet, a torn read, or the let-go's mapping to world was not available (named in its Release line).
bool ThrowableReleaseSnapshot(throwable::Vec3& posWorld, throwable::Vec3& velWorld, double& ageMs);

// The local player's throw controller [player+0xC28] as read on the latest armed poll, or null (not armed, no player,
// a fault). For the game-side hooks: anything else that reaches them is not ours.
void* ThrowableLocalCtrl();

// Game thread (Task 4's controller-Update detour) consumes the pending cancel: true once per requestCancel the
// decision issued. The pad thread NEVER calls set-state (Task 1 step 5 / INV-566: unlocked read-modify-write).
bool ThrowableTakeCancelRequest();

// True while the decision is in Releasing (our RT let-go happened, the game's release not yet seen).
bool ThrowableReleasePending();

// True while the latest poll wrote L2 = 255 (a readied or cancelling throwable). For a caller that would otherwise zero
// L2 after this filter (the mod-menu block): L2 dropping while the game state is 2 throws (INV-566).
bool ThrowableHoldsL2();

// True while the grappling hook (ItemType 47) is the selected item and the player is on foot, as of the latest scePad poll (the same
// item reads as the throwable decision; false while throwable_hand_enable=0 - those reads are off then). For grapple_hand's trigger
// swap (ovr2xr.cpp's pad chain). One volatile load - any thread.
bool ThrowableHookSelected();

// Ruling R24: true while the throwable decision is not Idle - Readying / Readied / Releasing or any cancel wait (as of
// the latest scePad poll; false while throwable_hand_enable=0 and nothing is in flight). The melee throw (throw_enable)
// and the VR swing (melee_swing_mode=2) do not trigger while it is true. One volatile load - any thread.
bool ThrowableBusy();

// The decision's parameters with the live settings applied (Task 4's ThrownVelocity call uses the same values).
throwable::Params ThrowableParams();

}  // namespace dlvr
