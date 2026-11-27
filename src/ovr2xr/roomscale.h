// Roomscale - docs/superpowers/specs/2026-10-26-roomscale-physical-crouch-design.md §2.2, plan
// docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 7 (amended: 7a measures, 7b drives).
// 7a: a LOG-ONLY probe of the walk body, its guards, the controller group and the app -> world map (run 4a passed).
// 7b: roomscale_enable - real steps move the character. The game-thread side lives here (the walk body's pre-step detour, the
// measured share of each step that roomscale caused); ovr2xr.cpp's render thread owns the view (roomscale_logic.h Update and the
// appSpace shift) and hands the command over.
#pragma once

#include <cstdint>   // uintptr_t - RoomscaleResolveForProbe

namespace dlvr {

extern bool  DiagRoomscaleProbe;   // diag_roomscale_probe - Task 7a's log-only probe. DEFAULT OFF.
extern bool  RoomscaleEnable;      // roomscale_enable - real steps move the character. DEFAULT ON (was OFF, Tier C).
extern float RoomscaleLeanMaxM;    // roomscale_lean_max_m - how far the view may lean past a blocked body

// Game thread, once per player update (boneblend.cpp HookTail, after BodyGrabTick): the 7a probe, and for 7b the player's walk
// body + controller group (published for the detour / render thread) and the pre-step detour's one-time install. Both off: two
// bool tests.
void RoomscaleProbeTick();

// Game thread. The climbing P0 probe (climbprobe.cpp, docs/design_climbing_overhaul.md §6 P0) borrows this file's resolver instead of a
// second copy: the PlayerDI, the walk body's full object (0 + why when it is not CODEPhysicsWalkFly's - refused by vtable, as here),
// the controller group and the control object's world position. Read-only. False only when the engine exports are missing or no
// player exists yet (*why says which); *full may be 0 with a true return.
bool RoomscaleResolveForProbe(uintptr_t* player, uintptr_t* full, int* group, float worldPos[3], const char** why);

// Render thread (ovr2xr.cpp BeginFrameIfNeeded).
bool RoomscaleLive();                          // the pre-step detour is installed and the player's walk body is known
int  RoomscaleGroup();                         // the player's controller group (PlayerDI+3324), -99 = not read
bool RoomscaleOnFoot(int group);               // Jay's ruling 5: 0-6 and 25 move the character, everything else stands down
void RoomscaleTakeMoved(float* wx, float* wz); // world x/z the body moved BECAUSE of roomscale since the last call (consumed)
void RoomscaleSetCommand(float vx, float vz);  // world x/z velocity (m/s) roomscale adds to the body; 0, 0 = none
// Physics-side totals since the last call (consumed), for the [ROOMSCALE] window line.
void RoomscaleTakeStepStats(long* steps, long* commanded, long* guarded, long* gameChanged);
// Fix 1 (trace docs/traces/20261117_1316_rscam_fix1.md; BETA port 20261117_1600): ovr_GetTrackingState records each head position it
// reports, with the share total the appSpace shift had taken at that moment (called under the frame lock, right after ToOvr).
void RoomscaleNoteReported(const float pos[3]);

}  // namespace dlvr
