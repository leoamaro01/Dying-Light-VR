// Hand climbing P1 - docs/design_climbing_overhaul.md §4.2 / §4.3 / §6 P1 (P0 measurements §8.4, P1 run 1 §8.5).
// climb_by_hand ON: the game no longer starts a ledge grab by itself (look + hold jump), except for low walls (climb_auto_low_m: those
// stay the game's own, like vaults). A GRIP closed near a ledge makes the game thread look for the ledge AROUND THAT HAND with a fan
// of rays; found = the game's own "set climb target" + the game's own start. The search keeps running while the grip stays closed
// (climb_catch_ms), so a hand held out while falling past a ledge catches it. A ledge at least climb_hang_min_m above the feet is hung
// from (climb_always_hang); lower ones the game climbs over. Opening both grips while hanging asks for the game's own drop. Hanging,
// shimmying (stick), climbing up and hang-jumps (jump button) stay the game's own until P2 / P3. Wall-run, tic-tac, vaults and
// step-overs are not ledge grabs and are untouched. The decisions are climbhand_logic.h (tests/climbhand_logic_test.cpp).
#pragma once

#include <cstdint>

namespace dlvr {

extern bool  ClimbByHand;       // climb_by_hand - master. DEFAULT OFF (P1 prototype).
extern float ClimbReachM;       // climb_reach_m - hand to the found ledge's lip, at most
extern int   ClimbCatchMs;      // climb_catch_ms - a closed grip keeps looking for a ledge this long after the press
extern bool  ClimbAlwaysHang;   // climb_always_hang - a high ledge grabbed by hand hangs instead of climbing straight over
extern float ClimbHangMinM;     // climb_hang_min_m - "high" = the lip at least this far above the feet
extern float ClimbAutoLowM;     // climb_auto_low_m - walls whose top is at most this far above the feet stay the game's own
extern bool  ClimbDebugDraw;    // climb_debug_draw - REMOVED 2026-11-27: a locked key only, read by nothing
extern int   ClimbDebugPhysics; // climb_debug_physics - REMOVED 2026-11-27: a locked key only, read by nothing
extern bool  ClimbHandMotion;   // climb_hand_motion - P2: while hanging, the body follows the hands (pull up, lower down)
extern float ClimbPullMaxMps;   // climb_pull_max_mps - the fastest the hands may move the body
extern float ClimbReleaseM;     // climb_release_m - a holding hand this far from its ledge point lets go
extern bool  ClimbMantleAuto;   // climb_mantle_auto - P3: pulled up far enough = the game's own climb-up (default off)
extern float ClimbMantleM;      // climb_mantle_m - ...when the ledge is this far above the feet or less
extern bool  ClimbMantleStick;  // climb_mantle_stick - the stick forward in a hang climbs up the game's way (default on)
extern bool  ClimbFastClimbUp;  // climb_fast_climb_up - that climb-up ends once the body is up and past the lip (default on)
extern bool  ClimbSkipReach;    // climb_skip_reach - the stick-up's reach when there is no ledge above is refused (default on)
extern bool  ClimbLadderByHand; // climb_ladder_by_hand - P5: ladders / pipes / poles grabbed anywhere by hand, never the game's (default on)
extern bool  ClimbJumpByLook;   // climb_jump_by_look - the jump button from a hang jumps at full force toward the look (default on)
extern float ClimbJumpSideDeg;  // climb_jump_side_deg - a look within this of the wall / pole jumps straight up (default 45)
extern bool  ClimbRoomCheck;    // climb_room_check - a climb-up only where you fit (crouched if need be; default on)
extern bool  ClimbSkipGrabAnim; // climb_skip_grab_anim - a hand grab hangs at once (no grab animation; default on)
extern float ClimbGrabMoveMps;  // climb_grab_move_mps - after a grab, the body glides this fast until the hand is on its spot (default 2)
extern float ClimbSlideMps;     // climb_slide_mps - P5: a light grip on a ladder / pole slides down this fast (0 = never; default 2.5)
extern bool  ClimbZiplineByHand; // climb_zipline_by_hand - P6: a grip next to a zipline rides it (default on)
extern float ClimbSlideGrip;    // climb_slide_grip - a grip under this squeeze (past 0.5) is "light": it slides (default 0.85)
extern bool  ClimbAutoLow;      // climb_auto_low - low walls still climb the game's way, look + jump (default off)
extern float ClimbLaunchUpAssist; // climb_launch_up_assist - 0..1: a mostly-upward launch turned toward straight up
extern float ClimbLaunchMinMps; // climb_launch_min_mps - P3: letting go while the hands move the body this fast = a launch (0 = never)
extern float ClimbLaunchScale;  // climb_launch_scale - the launch speed = the pull speed x this (capped at a jump)

// P2 (design §4.4 / §8.1 (b)): walklook's +ED2040 detour asks this on the player walk's call. True = send `out` (a world velocity,
// m/s) instead of the game's `in`: the local player, the ledge grab in a hold state, at least one hand anchored, a fresh pad publish.
// Game thread. COST off / not hanging: two bool tests and one read.
// moveCtl = MoveController is the player's active controller (the carry after a launch / in any air time only runs under it).
bool ClimbHandWalkOverride(uintptr_t player, const float in[3], float out[3], bool moveCtl);

// P7 (design §8.21): the grappling hook as a rope. Asked on the same walk call BEFORE ClimbHandWalkOverride: true = send `out` (the
// rope's velocity, world m/s) - from the game's rope reaching its pull state until you let go. Game thread. COST off / no rope: a few
// reads per call (the rope controller's pointer, vtable and state).
bool GrappleRopeWalk(uintptr_t player, const float in[3], float out[3], bool moveCtl);
extern bool  GrappleRopeSwing;    // grapple_rope_swing - the grappling hook is a rope you swing on (default on; needs climb_by_hand)
extern bool  GrappleTriggerPull;  // grapple_trigger_pull - holding the left trigger pulls you along it (default on)
extern float GrapplePullMps;      // grapple_pull_mps - the pull's closing speed, m/s (default 15)
extern bool  GrappleHoldToSwing;  // grapple_hold_to_swing - the left trigger held keeps the rope, the right trigger pulls (default on)
// True while the rope holds the player with grapple_hold_to_swing and the trigger pull on: the right trigger is the pull, so the game's
// R2 is withheld (ovr2xr.cpp's pad chain). Any thread. COST: one volatile load.
bool GrappleRopeHoldsR2();
extern int   GrappleThrowHand;    // grapple_hand - the trigger that throws / holds the hook: 0 left (default), 1 right
extern int   GrapplePullHand;     // grapple_pull_hand - the trigger that pulls: 0 left, 1 right (default)
// grapple_hand RIGHT: while the hook is the selected item or a rope holds, the game's L2 / R2 bytes trade places (the right trigger
// throws, the left attacks). The pad chain calls it right after the trigger bytes are made, before the trigger rules. Pad thread.
void GrapplePadSwap(unsigned char* p);
extern bool  GrappleUnlockSkill;  // grapple_unlock_skill - the grapple skill needs no rank / point (the bootstrap acts on it at launch)

extern bool  ClimbPinHands;     // climb_pin_hands - P4: a hand holding a ledge is drawn on its ledge point
extern bool  ClimbLedgeLast;    // climb_ledge_last - two ledges held: the climb logic uses the last grabbed (off = their average)
extern bool  ClimbStickShimmy;  // climb_stick_shimmy - the stick still shimmies while hanging (the game's own; default off)
extern bool  ClimbBodyFollowsView; // climb_body_follows_view - the hanging body turns with the view
extern bool  ClimbAirMomentum;     // climb_air_momentum - a launch keeps its own momentum in the air (physics, no stick)
extern float ClimbAirDrag;         // climb_air_drag - how fast that momentum fades, per second
extern float ClimbAirControl;      // climb_air_control - LOCKED (round 16), read by nothing; see climb_air_control_pct
extern float ClimbAirControlPct;   // climb_air_control_pct - the stick's push in the air, % of the gap per update (default 10)
extern bool  ClimbAirMomentumAll;  // climb_air_momentum_all - the same for every jump and fall, not only launches
extern bool  ClimbViewSmooth;      // climb_view_smooth - while climbing, the view follows the eye bone smoothly (no snaps)

// P4 (design §4.8): the shift that puts hand `hand` (0 = LEFT, 1 = RIGHT) on the ledge point it holds = that point - the hand, game
// world, as the pad thread last measured it. False = not holding / climb_pin_hands off / stale (> 100 ms) / implausible (farther than
// climb_release_m). For the arm IK (boneblend.cpp SolveArmIk). Any thread.
bool ClimbHandPinShift(int hand, float out[3]);
// P4 round 5 (climb_view_smooth): what boneblend's camera-base call adds to the view (and the hand anchor) while climbing - the eye
// bone's offset from the body's origin, low-passed (climbhand_logic.h ViewSmoothStep). eyeW / originW world; false = nothing.
bool ClimbHandViewCorrection(const float eyeW[3], const float originW[3], float corrW[3]);
// P4 round 8 (tester 2026-11-27: "The camera shifting when turning my head while hanging is still there, as strong as ever. Let's
// add some logging on what could be causing this"): every part of the player camera's base on one call, from boneblend's
// EyePointApply. ClimbHandCamTrace logs while the ledge grab runs in hand mode: a CAM SNAP line whenever the camera moves more than
// kCamSnapM against the body's origin in one call (with what each part did), and a CAM line every kCamEveryMs. Game thread.
struct ClimbCamSample {
    float originW[3];     // the body's origin (world)
    float bodyFwd[3];     // the body's forward axis (world, its transform's column 2)
    float headYawDeg;     // the headset's yaw in the play space (NAN = unread)
    float baseL[3];       // the game's camera base (body-local, vis+4112) before this call added anything
    float camW[3];        // the camera base after it (world)
    float eyeW[3];        // the eye bone (world; NAN = unread)
    float d1[3], d2[3], vs[3], htvW[3];   // face offset, headset arc, climb_view_smooth, hand-twist view hold (world)
};
void ClimbHandCamTrace(const ClimbCamSample& s);

// P2 round 4: true while climb_by_hand is live and the ledge grab runs - body-follows-head (boneblend.cpp follow::Before) then pauses,
// as it does when another controller owns the look: turning the hanging body with the head swings the head bone the camera sits on
// (the "snap" when the head turns). Game thread. COST: two bool tests + one read.
bool ClimbHandFreezesBody();
// P4 round 10: true while climb_by_hand is live and the ledge grab runs (boneblend's hand-twist view hold stays off then). Game thread.
bool ClimbHandHanging();
// P4 round 11 (climb_view_rigid): the share (0..1) of the rigid standing camera base this camera call, eased in while the ledge grab
// runs in hand mode and out after (climbhand_logic.h RigidRampStep). Game thread (boneblend's EyePointApply).
// Round 4: animGapM = the vertical gap between the standing eye point and the game's animated head (m, NaN = unknown): after the
// grab ends the rigid view stays until the gap is small (the clip blended out), at most 1.5 s (climbhand_logic.h ViewHoldTail).
// Round 6: the tail ends once per grab end, and also when the game's head has settled.
float ClimbViewRigidK(float animGapM);
// Round 4: the body holds (root, upper body) while the ledge grab runs in hand mode AND for 0.5 s after it ends, while the hang /
// climb clip blends out (climbhand_logic.h BodyHoldTail). Round 6: not after a climb-up. Any thread.
bool ClimbHandBodyHeld();
// Round 6: the rigid view's remembered standing eye height (body-local y, m) from a new accepted standing value - a dip under it is
// refused until it has held for 10 s (climbhand_logic.h StandRefStep). Game thread (boneblend's EyePointApply).
float ClimbHandStandRef(float y);
// P4 round 15: while the game's climb-up runs (the ledge grab in state 8 / 10, hand mode), how far the body's origin (originY, world)
// is above the ledge top - the rigid view and the root hold come down by it (climbhand_logic.h OverLedgeM). Game thread (boneblend's
// camera call); it also publishes the value for ClimbHandOverLedgeLast. 0 when not climbing up.
float ClimbHandOverLedgeM(float originY);
// The last value ClimbHandOverLedgeM published (any thread; 0 when its last call is older than 100 ms).
float ClimbHandOverLedgeLast();
extern bool ClimbViewRigid;        // climb_view_rigid - while climbing, the climb animations never move the view
extern bool ClimbHoldBody;         // climb_hold_body - while climbing, the upper body keeps its upright pose (boneblend AhThrStep)

// P5 / P6: true = the HUD prompt slot for this trigger is updated with no trigger (hidden): it is a ladder / pipe / rope (LadderDI)
// and hand mode refuses the game's own ladder start (climbhand_logic.h HideLadderPrompt), or a zipline's prompt trigger
// (FakeZipLineTrigger / ZipLine) while ziplines are grabbed by hand (HideZiplinePrompt). Any thread (HUD slot updates hop workers).
// COST: a few bool tests; per trigger vtable one RTTI name read, then a cached compare.
bool ClimbHandHidesPrompt(void* trigger);

// Game thread, once per player update (boneblend.cpp HookTail): installs the detours the first time climb_by_hand is on, and writes
// the 5 s [CLIMBHAND] line. Off and not installed: one bool test.
void ClimbHandTick();

// Pad thread, once per scePad poll, AFTER every other grip owner (holsters, two-hand, reload, lids, loot, push-grab) and BEFORE the
// grip block. inactive = an in-game menu / our overlay / calibration (no new grabs; a running hang is left to the game). ownedL /
// ownerL / ownerR = the name of the feature that took that press first (nullptr = none - the press is free for climbing; the name is
// logged so a press climbing never saw can be traced). withholdL1 / withholdR1 come back true while that grip is holding a climb.
void ClimbHandPoll(bool inactive, const char* ownerL, const char* ownerR, bool& withholdL1, bool& withholdR1);

}  // namespace dlvr
