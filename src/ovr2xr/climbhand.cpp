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

#include "aimhook.h"          // AimMirrorYaw (the jump's look: the same yaw mapping as the hands)
#include "boneblend.h"        // InstallSingleGameDetour
#include "camhook.h"          // CamHookGetOpenXrToGameYaw
#include "headpose.h"         // GetHeadPose
#include "physcrouch.h"       // PhysCrouchForceUntil (climb_room_check's held crouch)
#include "climbhand_logic.h"
#include "grapplerope_logic.h" // P7: the grappling hook as a rope
#include "throwable.h"        // ThrowableHookSelected (grapple_hand's trigger swap)
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
// [DEP: climb_fast_climb_up] required-for=the game's climb-up from a hang ending once the body is up and past the lip (round 18;
//      tester 2026-11-27: "when I move the joystick up ... it doesn't let me move for 1 or 2 seconds, even if a mantle is
//      successful"; climbhand_logic.h FastClimbUpDone). Off = the game's whole state 8 (2.24 s) plays out. DEFAULT ON.
bool  ClimbFastClimbUp = true;
// [DEP: climb_skip_reach] required-for=the stick-up in a hang doing nothing when there is no ledge above and no climb-up (round 19;
//      tester 2026-11-27: "remove the animation when it doesn't find a place to shimmy and it can't mantle ... it doesn't make any
//      sense in VR"). On = the game's reach-up from a hang (5 / 9: the stick-up search's +D16639, and the hang's input handler's
//      +D19D50 the next update - round 20) is refused after the re-target (climbhand_logic.h GameMoveFromHang). DEFAULT ON.
bool  ClimbSkipReach = true;
// [DEP: climb_ladder_by_hand] required-for=P5 ladders / pipes / poles climbed by hand (design §4.9 revised; tester 2026-11-26:
//      "a ladder, pipe or pole is a solid object: grab it anywhere, climb by pulling ... no animation from the game's ladder").
//      On = the game's own ladder (LadderController, group 7) never starts from walking into one or the use key (GroupHook), and a
//      grip near one grabs it anywhere as a hang (climbhand_logic.h PoleInReach / PoleLedge). Off = ladders stay the game's own.
//      DEFAULT ON.
bool  ClimbLadderByHand = true;
// [DEP: climb_jump_by_look] required-for=the jump button from a hang, ledge or pole, jumping at the game's full jump toward the look
//      (P5 jumps, design §4.9; tester 2026-11-27: "pressing the jump button should make you jump at full force ... towards wherever
//      you are looking. This is both for ledges and poles"). On = the game's own jump / reach ask from the hang's input handler
//      (+D19D50 / +D19D41, the stick not pushed up) is refused and our launch runs instead (climbhand_logic.h JumpButtonAsk /
//      LookJumpVelocity). Off = the game's own hang jump. DEFAULT ON.
bool  ClimbJumpByLook = true;
// [DEP: climb_jump_side_deg] required-for=which look counts as "at the wall" for that jump (ClimbJumpSideDeg; the planned
//      climb_pole_jump_side_deg, now for ledges too): the look's yaw under this many degrees from into-the-wall = straight up, at or
//      past it = a push-off along the look. DEFAULT 45.
float ClimbJumpSideDeg = 45.0f;
// [DEP: climb_skip_grab_anim] required-for=a hand grab that hangs skips the game's grab animation (state 0): the Hook calls set-state
//      1 on its first update in it - what +D0A240 does itself once the grab's time is up (+D0A4B9), only sooner. Tester 2026-11-28:
//      "the 'go to grab mode' animation often makes me clip through walls or moves me in a weird way". The body then glides by the
//      hands' pull (climb_grab_move_mps). Only grabs that hang (climb_always_hang); low walls keep the game's climb-over. DEFAULT ON.
bool ClimbSkipGrabAnim = true;
// [DEP: climb_room_check] required-for=a climb-up only where the body fits (climbhand_logic.h ClimbUpRoom; tester 2026-11-28: a
//      closet's top climbed "to the ceiling onto the upper floor", and a low spot left the head "above the ceiling"): standing room
//      = the game's climb-up; only crouched room = the climb-up with the game's crouch held (physcrouch PhysCrouchForceUntil) until
//      there is standing room over the feet; neither = refused. Both climb-ups from a hang (the stick, the pull). DEFAULT ON.
bool ClimbRoomCheck = true;
// [DEP: climb_grab_move_mps] required-for=the glide after a grab: the hands' pull is capped at this while a newly anchored hand has
//      not yet SETTLED on its ledge point (climbhand_logic.h GrabGlideCap); climb_pull_max_mps after. DEFAULT 2.0.
float ClimbGrabMoveMps = 2.0f;
// [DEP: climb_slide_mps] required-for=P5's slide down a ladder / pole (design §4.9 "Slide": "grips half-closed = slide down"):
//      every holding hand's grip held lightly (under climb_slide_grip) slides the hang down at this speed until the climbable's bottom
//      or the ground (climbhand_logic.h SlideGrip / SlideStep), then the plain drop. Never above 0.8 x climb_pull_max_mps (the body
//      must keep up with the anchors). 0 = no slide. DEFAULT 2.5.
float ClimbSlideMps = 2.5f;
// [DEP: climb_slide_grip] required-for=which grip counts as "light" for that slide (ClimbSlideGrip): squeezed past the 0.5 hold
//      threshold but under this = slide, at or above it = hold still. DEFAULT 0.85.
float ClimbSlideGrip = 0.85f;
// [DEP: climb_zipline_by_hand] required-for=P6 ziplines grabbed by hand (design §4.10; ServeZip): a grip that finds no ledge with the
//      hand within climb_reach_m of a zipline's grabbable part starts the game's own zipline ride on that line, with no look, facing
//      or jump needed. Off = ziplines only the game's way (look at one and jump). The game's own jump start works either way.
//      DEFAULT ON.
bool  ClimbZiplineByHand = true;
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
//      LOCKED round 16 (2026-11-27): replaced by climb_air_control_pct; read by nothing.
float ClimbAirControl = 1.0f;
// [DEP: climb_air_control_pct] required-for=the stick's say in the air while climb_air_momentum keeps the momentum (round 16; tester
//      2026-11-27: "I feel like I don't have any air control (and it's set to 1), maybe you went too hard on the 1%. Let's make it so
//      that value is configurable instead of the 0-1 we have, make the default 10%"). AirControlPush's k: each walk update the
//      walk is handed the momentum + this % of the gap between the stick's speed and the speed along the stick (the game's own
//      0.22^3 = 1.06 %). 0 = momentum only. INERT while climb_air_momentum=0. DEFAULT 10.
float ClimbAirControlPct = 10.0f;
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
//      Round 13 (tester: "I think the problem is the hips though" / "we do need to hold the root as well"): the pump's root hold
//      (boneblend RootPaStep) also holds bip01 at the standing reference for the grab. Needs rig_throwable_body_hold on (its
//      machinery and ANIMHOLD's proven map). DEFAULT ON.
bool  ClimbHoldBody = true;
// [DEP: grapple_rope_swing] required-for=the grappling hook as a rope you swing on (GrappleRopeSwing; P7, design §8.21; tester
//      2026-11-28: "It should act like an anchor point, like a rope, so you can swing on it (it respects your momentum and speed
//      when you use it)"). On = from the moment the game's rope (RopeLocomotionController, player+3200) reaches its pull state (3)
//      until you let go (the jump button, landing after air time, reaching the hook), the walk call is sent the rope's velocity
//      (grapplerope_logic.h RopeStep) instead of the game's pull: gravity, your momentum, the rope's length. Needs climb_by_hand
//      (its walk hook and velocity calls). Off = the game's own pull. DEFAULT ON. HEADSET.
bool  GrappleRopeSwing = true;
// [DEP: grapple_trigger_pull] required-for=pulling yourself along the rope by holding the trigger (GrappleTriggerPull; tester
//      2026-11-28: "only while holding the trigger with which you launched it ... If you stop holding the trigger, it stops pulling"
//      / "add a setting to remove pulling with the trigger completely, so it only works as a swing rope"). On = while the LEFT
//      trigger (the grapple's own button, L2) is held past half, the rope pulls you toward the hook (an acceleration along the rope,
//      the swing kept) up to grapple_pull_mps. Off = a swing rope only. DEFAULT ON.
bool  GrappleTriggerPull = true;
// [DEP: grapple_pull_mps] required-for=the trigger pull's strength (GrapplePullMps): the closing speed it pulls up to (m/s); it gets
//      there in about grapplerope_logic.h kPullRampS. DEFAULT 15.
float GrapplePullMps = 15.0f;
// [DEP: grapple_hold_to_swing] required-for=the left trigger as the rope and the right trigger as its pull (GrappleHoldToSwing; tester
//      2026-11-28: "make the pull be with right trigger. So pressing and holding left trigger is what launches the hook and makes you
//      swing (and you don't stop swinging until you release the left trigger, remove the game's default automatic timeout dropoff with
//      this setting on), but the actual pulling is with right trigger instead"). On = the rope lets go only when the left trigger is
//      released (no jump / landing / arrival end), the game's rope pull timer (RopeLocomotionController +0x64 against +0x60) is held
//      off while it holds, and the right trigger pulls (the game's R2 withheld meanwhile: no attack per pull). Off = round 1: the left
//      trigger pulls, jump / landing / the hook let go. DEFAULT ON.
bool  GrappleHoldToSwing = true;
// [DEP: grapple_hand / grapple_pull_hand] required-for=choosing the triggers (GrappleThrowHand / GrapplePullHand; tester 2026-11-28:
//      "Grappling hand: changes which trigger shoots the hook / Pull hand: changes which trigger causes the hook to pull"). 0 = left,
//      1 = right (controllers.h Hand). Grappling hand RIGHT = while the hook is the selected item or a rope holds, the game's L2 and R2
//      trade places in the pad chain (GrapplePadSwap; the right trigger throws, the left attacks) and the right trigger holds the rope.
//      The pull hand's trigger pulls; when it is not the grappling hand's it is withheld from the game while a rope holds
//      (grapplerope_logic.h WithholdR2). The same hand for both in hold mode: held part way it swings, squeezed fully it pulls.
//      DEFAULT left / right (round 2's layout).
int   GrappleThrowHand = 0;
int   GrapplePullHand = 1;
// [DEP: grapple_unlock_skill] required-for=the grappling hook skill from the start (GrappleUnlockSkill; tester 2026-11-28: "add a setting
//      (off by default ...) to have the grappling gun skill from the game start"). The layer only holds the knob: the BOOTSTRAP reads
//      grapple_unlock_skill=1 from ovr2xr.txt as text at launch (skillpatch.cpp), writes DW\dlvr_common_skills.xml - the game's own
//      common_skills.xml with StatusHook's rank requirement removed and its cost 0 - and points the engine's opens of
//      common_skills.xml at it. restart=true. DEFAULT OFF.
bool  GrappleUnlockSkill = false;

// P4 round 15 / 16: the climb-up's lift state (defined with ClimbHandOverLedgeM below; the Hook reads .climbing - a climb-up from a
// hang - for round 18's cut-short). Game thread.
extern climbhand::OverLedgeState g_ol;

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
// P4 round 14: +D16690, the ledge grab's own "hang point from the player's ledge" (copies player+0x14C8 / +0x14BC into eg+0x4C / +0x58,
// recomputes the hang position eg+0x1BC and the hand points eg+0x2B0..+0x348). Its only caller, +D14A08, runs it right after a set-state
// when player+0x14C8 no longer equals eg+0x4C. push rbx | sub rsp,70h | mov rax,[rcx] | mov rbx,rcx | movaps [rsp+60h],xmm6 (2026-11-27)
constexpr uintptr_t kHangPointRva = 0xD16690;
const unsigned char kHangPointBytes[17] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9,
                                            0x0F, 0x29, 0x74, 0x24, 0x60 };
constexpr uintptr_t kEgLedgeMid = 0x4C;   // the ledge grab's own copy of the ledge point (+D16690 writes it)
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
// P5 round 1 [BIN, 2026-11-27]: the game's ladder start (climbhand_logic.h's P5 note). Its three group-7 switches, each "call +B9F220"
// - the use key (+BDCAE0's ladder branch) and the walk-in (+BDD040, two), both on the player's use target (player+0x14A0, valid
// while +0x1498 is set) - are refused by return address (GroupHook). Round 2: the climbable a grip grabs is the LadderDI that owns
// a search ray's hit (its collider); its IControlObject is at +0x18 (as OwnerLadderInfo reads it), its kind at +0x36C.
constexpr uintptr_t kLadderSwitchRva[3] = { 0xBDCF0F, 0xBDD277, 0xBDD734 };
constexpr uintptr_t kLadderCtl = 0x18, kLadderKind = 0x36C;
constexpr const char* kFwdExport = "?GetForwardVector@IControlObject@@QEBA?AVvec3@@XZ";
constexpr const char* kExtWorldExport = "?GetExtentsInWorld@IControlObject@@QEBAAEBVextents@@XZ";

using TryStartFn  = void(__fastcall*)(uintptr_t player);
using TypeFn      = int(__fastcall*)(void* eg, void* a2, float* dir, float* height, void* a5);
using SetGroupFn  = void(__fastcall*)(uintptr_t player, int group, bool flag, void* zero);
using SetStateFn  = void(__fastcall*)(uintptr_t edgeGrab, int state);
using SetHookFn   = void(__fastcall*)(uintptr_t edgeGrab, uintptr_t hook);
using HangPointFn = void(__fastcall*)(uintptr_t edgeGrab);
using SetTargetFn = void(__fastcall*)(uintptr_t player, const float* a, const float* b, const float* dir, unsigned short tag);
using RayFn       = unsigned char(__fastcall*)(void* self, unsigned char kind, void* coll, const float* from, float* to,
                                               unsigned short flags, void* ignore, unsigned a8, unsigned a9, int a10);
using PosFn       = float*(__fastcall*)(const void* control, float* out);

using ExtFn = const float*(__fastcall*)(const void* control);
PosFn         g_fwdFn = nullptr;     // P5: IControlObject::GetForwardVector (same shape as GetWorldPosition)
ExtFn         g_extFn = nullptr;     // P5: IControlObject::GetExtentsInWorld (min at +0, max at +0xC)
SetGroupFn    g_trueGroup = nullptr; // P5: the group setter's trampoline (GroupHook)
uintptr_t     g_ladderRet[3] = {};   // P5: the return addresses of the game's three ladder starts (0 = that call did not check out)
volatile LONG g_groupLive = 0;       // P5: the ladder gate: 1 live, -1 refused
volatile LONG64 g_wLadderRefused = 0;
ULONGLONG     g_ladderSaidMs = 0;
// P6 climbing XP (X1, design §8.2 / §8.3): the game's climb award +CEA740(xpModule = *(PlayerDI+0x13E0), climb type). Climb-ups from
// a hand hang already pay through the game's set-state; only a launch caught by a new grab is paid here (climbhand_logic.h's P6
// note). mov [rsp+8],rbx | push rdi | sub rsp,20h | mov rax,[rcx] | mov rbx,rcx | movsxd rdi,edx - 19 bytes (read 2026-11-27)
constexpr uintptr_t kClimbXpRva = 0xCEA740;
const unsigned char kClimbXpBytes[19] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B,
                                          0xD9, 0x48, 0x63, 0xFA };
constexpr uintptr_t kXpModuleOff = 0x13E0;
using ClimbXpFn = void(__fastcall*)(uintptr_t xpModule, int climbType);
ClimbXpFn     g_climbXp = nullptr;   // null = the bytes did not match: launch catches pay nothing
climbhand::LaunchXp g_launchXp;      // game thread: armed by a launch, spent by the next grab start
volatile LONG64 g_wXpAsked = 0, g_wXpFaults = 0;
// climb_jump_by_look (P5 jumps): the hang input handler's two jump asks (+D19BA0, action 0xB in state 1): "call +D0CD20" at +D19D41
// (state 4) and +D19D50 (state 9); their return addresses name them in StateHook (0 = that call did not check out).
constexpr uintptr_t kJumpCallRva[2] = { 0xD19D41, 0xD19D50 };
uintptr_t     g_jumpRet[2] = {};
ULONGLONG     g_reqJumpMs = 0;          // game thread: StateHook refused the game's jump; the next Hook jumps by the look
int           g_reqJumpFrom = -1;       // ...from this hold state (log)
// pad thread -> game thread (under g_lock): the headset's forward in the game world, and when it was read
float         g_lookFwd[3] = {};
ULONGLONG     g_lookMs = 0;
volatile LONG64 g_wJumpsUp = 0, g_wJumpsOff = 0;
// P6 ziplines by hand (climbhand_logic.h ZipLine note). The list's address comes from the game's own "cmp dword ptr [count], 0" at
// +B19866 (83 3D disp32 00 - the nearest-line search), so a build whose code moved never reads a stale data address.
constexpr uintptr_t kZipCountCmpRva = 0xB19866;
// The game mounts a zipline from two places, both "call +B9F220" with group 27 (2026-11-28: refusing only the first left
// the A press mounting - its 5 s count stayed 0 while the tester rode):
//   [0] +BD43BF in +BD3C60, the jump start (airborne + jump held, the line's look/yaw gates);
//   [1] +B983C1 in the player's action dispatch (+B97xxx), the use/jump press while the zipline hint (player+0x2CA8,
//       +0x380 set by +BD3C60's path B) is valid - it copies the hint's line record into the controller and mounts.
constexpr uintptr_t kZipJumpSwitchRva[2] = {0xBD43BF, 0xB983C1};
const char* const   kZipJumpSwitchName[2] = {"the jump near a line", "the action press on the line's hint"};
uintptr_t     g_zipJumpRet[2] = {0, 0};             // their return addresses (0 = that call did not check out: that start stays)
// Round 4 (2026-11-28, tester: "the first one I went to grab ... launched me into the air straight up like 50-100m"): refusing [1]'s
// group switch is NOT enough - right after it, +B983C6..+B9843D jumps the player (+D4D330) by (controller+0x48 - the player's
// position), the attach point the controller only computes when the group switch goes through. Refused, it is stale - (0, 0, 0) on
// the session's first line, so the jump aimed ~140 m (log 16:04:01: the refusal of [1], then the launch). So the game's whole
// try-start, +BD3C60 (called once per player update from +B96AC8; it re-arms the hint [1] reads, and holds [0]), is skipped while
// ziplines are grabbed by hand: its first write (hint+0x380 = 0) is done here, nothing else. The two group refusals stay as a backstop.
constexpr uintptr_t kZipTryRva = 0xBD3C60;
// push rbp (REX); push rsi; push rdi; lea rbp,[rsp-0B0h]; sub rsp,1B0h - whole, position-independent instructions (19 bytes)
const unsigned char kZipTryBytes[19] = { 0x40, 0x55, 0x56, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x50, 0xFF, 0xFF, 0xFF,
                                         0x48, 0x81, 0xEC, 0xB0, 0x01, 0x00, 0x00 };
constexpr uintptr_t kZipHintSlot  = 0x2CA8;   // player -> FakeZipLineTrigger (the hint)
constexpr uintptr_t kZipHintValid = 0x380;    // its "valid" byte (+BD3C7D clears it first thing; +B982D6 tests it)
using ZipTryFn = void(__fastcall*)(uintptr_t player);
ZipTryFn      g_trueZipTry = nullptr;
volatile LONG g_zipTryLive = 0;               // 1 = the try-start detour committed
volatile LONG64 g_wZipTrySkipped = 0;
volatile LONG64 g_wZipJumpRefused = 0;
volatile LONG64 g_wGrabAnimSkipped = 0, g_wGlide = 0;
volatile bool g_grabEndClimbUp = false;   // round 6: that grab ended from a climb-up (state 8 / 10): no body tail
volatile ULONGLONG g_grabEndMs = 0;   // round 4: when the ledge grab last ended (Hook; the holds' tails read it on other threads)   // climb_skip_grab_anim's skips; walk calls capped at climb_grab_move_mps
ULONGLONG     g_zipJumpSaidMs = 0;
constexpr uintptr_t kZipNameAssignRva = 0x87FC0;   // the game's string assign (ctrl+0xB8 <- the line's name, as +BD43AD does)
// mov [rsp+8],rbx | push rdi | sub rsp,20h | mov rdi,rdx | mov rbx,rcx | cmp rcx,rdx - 19 bytes (read 2026-11-27)
const unsigned char kZipNameAssignBytes[19] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48,
                                                0x8B, 0xD9, 0x48, 0x3B, 0xCA };
constexpr uintptr_t kZipCtlSlot = 0xCA8;            // PlayerDI+0xCA8 = ZipLineController (RTTI-checked), active +0x10
constexpr uintptr_t kZipLineA = 0xA8, kZipLineModel = 0xD0, kZipLineName = 0xD8;   // in the zipline entity (A, B +0xC, D +0x18)
constexpr uintptr_t kZipCtlLine = 0x88, kZipCtlEnt = 0xB0, kZipCtlName = 0xB8;      // in the controller (+BD4384..+BD43AD)
constexpr int       kGroupZipLine = 27;
constexpr unsigned  kZipMaxLines = 4096;            // a sanity cap on the count read
constexpr const char* kRenderableExport = "?IsRenderable@IModelObject@@QEBA_NXZ";
using NameAssignFn = void(__fastcall*)(uintptr_t dst, uintptr_t src);
using RenderableFn = bool(__fastcall*)(uintptr_t model);
uintptr_t     g_zipList = 0;                        // &array (the count is at +8); 0 = ziplines by hand off this session
NameAssignFn  g_zipName = nullptr;
RenderableFn  g_zipRenderable = nullptr;
uintptr_t     g_zipCtlPtr = 0;                      // the controller last named (RTTI once per pointer)
bool          g_zipCtlOk = false;
volatile LONG64 g_wZipStarts = 0, g_wZipRefused = 0;
// P6 round 3 - hands on the zipline (climbhand_logic.h ZipHold note; static RE 2026-11-28, §8.18). The ride (ZipLineController
// +DD7220) wants: its speed (+0x7C) along the cable + a P-correction (k ~ 10/s) toward its hang point under the cable. In the walk
// call (ClimbHandWalkOverride) we keep the along-the-cable part and replace the rest with the hands' pull. The exit is the game's
// own: +B9F4F0(player, 27) (leave the group: the controller's Deactivate) and [player+0xBA0]+0x30 = 1 (as its end / jump-off
// do); then our velocity through the launch's post path and carry.
constexpr uintptr_t kLeaveGroupRva = 0xB9F4F0;
// push rsi | sub rsp,20h | cmp byte ptr [rcx+50h],0 | mov rsi,rcx - 13 bytes (then a jz rel32; read 2026-11-28)
const unsigned char kLeaveGroupBytes[13] = { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x79, 0x50, 0x00, 0x48, 0x8B, 0xF1 };
constexpr uintptr_t kWalkCtlSlot = 0xBA0, kWalkExitFlag = 0x30;
constexpr unsigned long long kZipStartGraceMs = 300;   // the game's "riding" publish may trail the start by an update
using LeaveGroupFn = void(__fastcall*)(uintptr_t player, int group);
LeaveGroupFn  g_leaveGroup = nullptr;               // null = no hands on the cable (rides stay the game's after the grab)
// P7 grapple_rope_swing (static RE 2026-11-28, design §8.21): the grappling hook is RopeLocomotionController (RTTI; vtable gamedll
// +17310D8, its slot 16 the per-update dispatcher +D74AF0, slot 19 the Stop +D75340) at PlayerDI+3200 (grappleaim.cpp's rope ctrl
// = [[g+1920]+3200]). Its state (+0x48) is set only by +D7A260: 1 -> 2 (the throw) -> 3 (+D78797: the hook caught - +D78C80 runs
// the pull each update) -> 4 / 5 / the walk controller's jump (+D79E6A, +D4D330 on PlayerDI+0xBA0 - the pull's end). The hook point
// is +0x88 (the stored target the dispatcher tests). Its controller group is 24 (+D759E4: +B9F220(player, 24, 1, 0)); we let go of
// it the zipline's way, +B9F4F0(player, 24). The leave-group pointer is checked on its own here (g_leaveGroup is the zipline's).
constexpr uintptr_t kRopeCtlSlot = 3200, kRopeCtlVtRva = 0x17310D8;
constexpr uintptr_t kRopeStateOff = 0x48, kRopeHookOff = 0x88;
// Round 2 (grapple_hold_to_swing): the pull's time out. +D78C80 (state 3) ends the pull once its time limit +0x64 is no longer above
// the time in the state +0x60 (+D78E8C: comiss [+0x64], [+0x60] / ja = keep pulling); the dispatcher adds dt to +0x60 every update,
// +D7A260 zeroes it and seeds +0x64 (+D7C0B0), +D78DE9 / +D79839 raise +0x64 to a data value. Run 2026-11-28 17:01:11: the rope's
// state left 3 at ~3 s into a swing and the walk's landing flag then ended our rope. Held off by writing kRopeNoTimeout to +0x64 on
// every walk call while the rope holds.
constexpr uintptr_t kRopeTimeOff = 0x60, kRopeLimitOff = 0x64;
constexpr float     kRopeNoTimeout = 1.0e6f;
constexpr int       kRopeStatePull = 3, kGroupRope = 24;
// Round 4 (design §8.24; tester 2026-11-28: "the game does drop me out of the grappling hook mode after a few seconds"): with the time
// limit held off, the pull still ended 1-7 s in (run 17:29, ropes #2-#8: near the hook, on touching the ground). +D78C80's end block
// is entered at +D79AD1 (je rel32 -> +D79E9A = no end this update) when +0x248 (set at the time limit, +D78EC2) OR r12b is set; r12b
// is the arrival (+D794CA: the body within a data distance of the pull target +0x6C, -> +D79840) and the contact test (+D79843..
// +D79A9C: a contact in the list +0x180 / count +0x188 facing against the pull). The block then sets state 4 / 5 or jumps off the
// walk (+D79E6A). While a hold-mode rope holds (grapplerope::KeepGameRopeOn) that je is rewritten to jmp +D79E9A (E9 C4 03 00 00 90),
// restored when it lets go. Both 6-byte forms sit inside one aligned qword (+D79AD0..+D79AD7), swapped by one compare-exchange, so a
// thread running the pull never sees a torn instruction. The code is shared: in co-op another player's rope on this machine cannot
// end itself either while ours holds (seconds; theirs ends on their next let go of ours).
constexpr uintptr_t kRopeEndJccRva = 0xD79AD1;
const unsigned char kRopeEndJcc[6]  = { 0x0F, 0x84, 0xC3, 0x03, 0x00, 0x00 };   // je +D79E9A (read 2026-11-28)
const unsigned char kRopeEndSkip[6] = { 0xE9, 0xC4, 0x03, 0x00, 0x00, 0x90 };   // jmp +D79E9A | nop
// Round 5 (tester 2026-11-28: "When shooting the grappling hook, the game moves my view to look at the hook point, remove that when our
// new functionality is enabled"): the active look owner is picked by +BA0F70, which walks the player's controllers in priority order
// and keeps the last whose vfunc +0xE0 (GetILookController) returns non-null. The rope's (vtable +17310D8 slot 28 = +D7E160) returns
// its ILookController (this+24, the one that SETS the look toward the hook, boneblend's INV-272 note) in states 4 / 5 or while a look
// target is set (+0xC4 / +0xC8..). With grapple_rope_swing, the local player's rope answers null, so LookController keeps the look.
// The slot is swapped once at install (its old value checked), the hook passes every other call through.
constexpr uintptr_t kRopeLookSlot = 28, kRopeLookFnRva = 0xD7E160;
using RopeLookFn = uintptr_t(__fastcall*)(uintptr_t self);
RopeLookFn    g_trueRopeLook = nullptr;
volatile LONG64 g_wRopeLookRefused = 0;
uintptr_t     g_ropeEndAt = 0;                      // the je's address; 0 = not available (bytes differ, or a failed write)
bool          g_ropeEndKept = false;                // the skip is written
LeaveGroupFn  g_ropeLeave = nullptr;                // null = letting go leaves the game's rope controller to end itself
// game thread (the walk call)
grapplerope::Rope g_rope;
int           g_ropePrevState = -1;                 // the game's rope state on the last walk call (-1 unread)
float         g_ropePrevFeet[3] = {};               // the feet on the last walk call, and when (the measured velocity)
bool          g_ropeHavePrev = false;
LARGE_INTEGER g_ropeQpc = {};
float         g_ropeMeasV[3] = {};                  // the feet's velocity, last good sample
bool          g_ropeHaveMeas = false;
float         g_ropeSent[3] = {};                   // what the last rope call sent
bool          g_ropePrevJump = false;               // the jump button on the last call (its press = an edge)
bool          g_ropeVertSet = false;                // the rope set the walk's vertical speed (cleared when it lets go)
long          g_ropeSeq = 0;                        // ropes caught this session
ULONGLONG     g_ropeTraceMs = 0;                    // the last trace line's time
int           g_ropeTraceLines = 0;                 // trace lines for the current rope
constexpr long kRopeTraced = 6;                     // the first ropes are traced every kRopeTraceEveryMs
constexpr ULONGLONG kRopeTraceEveryMs = 100;
constexpr int  kRopeTraceMax = 80;                  // ...at most this many lines per rope
volatile LONG64 g_wRopeCalls = 0, g_wRopePulling = 0, g_wRopeWalls = 0, g_wRopeStarts = 0, g_wRopeEnds = 0;
volatile LONG64 g_wRopeVertFail = 0;
// round 2
bool          g_ropeFree = false;                   // let go, flying free on the rope's last velocity until MoveController has the walk
ULONGLONG     g_ropeFreeT0 = 0;
bool          g_ropeHeld = false;                   // hold mode: the grappling trigger holds the rope (grapple_hand)
bool          g_ropeTimeSaid = false;               // the pull timer's first hold-off this rope (logged once per rope)
volatile LONG g_ropeHoldsR2 = 0;                    // the pull trigger lands on R2: withheld (GrappleRopeHoldsR2)
volatile LONG g_ropeOnPub = 0;                      // round 3: a rope holds (GrapplePadSwap, pad thread)
constexpr float kHoldOff = 0.3f;                    // the grappling trigger's hold (the game threw at L2 past half): released under this
volatile LONG64 g_wRopeTimeHeld = 0;
// game -> pad (under g_lock): a hand started a ride (seq bumps), and the ride's advance along the cable so far (cumulative)
unsigned      g_zipStartSeq = 0;
int           g_zipStartHand = -1;
float         g_zipStartPt[3] = {};
climbhand::ZipLine g_zipStartLine;
float         g_zipAdvSum[3] = {};
// pad -> game (under g_lock): the hold's pull
bool          g_zipHoldOn = false, g_zipDrive = false;
float         g_zipErr[3] = {};
ULONGLONG     g_zipPubMs = 0;
volatile LONG g_reqZipEnd = 0;                       // pad -> game: 1 the grips opened (a launch may follow), 2 a hand slipped
// game thread
float         g_zipAlong[3] = {};                    // the ride's last along-the-cable velocity (the exit keeps it)
float         g_zipD[3] = {};                        // the ridden line's direction
LARGE_INTEGER g_zipQpc = {};
// pad thread
climbhand::ZipHold g_zh;
unsigned      g_zipSeqSeen = 0;
float         g_zipAdvSeen[3] = {};
ULONGLONG     g_zipHoldMs = 0;
volatile LONG64 g_wZipDriven = 0, g_wZipEnds = 0, g_wZipLaunches = 0;
climbhand::Slide g_slide;               // P5 slide (pad thread)
volatile LONG64 g_wSlidePolls = 0;      // pad polls that moved the anchors down
const char* PoleKindName(int kind)
{
    return kind == 0 ? "ladder" : kind == 1 ? "pipe" : kind == 2 ? "rope" : kind == 3 ? "underwater rope" : "climbable";
}
// P5 (game thread): the pole / ladder the running hang holds (a hold made by PoleLedge), and the candidates a grip can grab
bool          g_poleOn = false;
climbhand::Pole g_pole;
uintptr_t     g_poleObj = 0;
ULONGLONG     g_poleStartMs = 0;
uintptr_t     g_poleSaidObj = 0;     // the last climbable whose geometry was logged
TryStartFn    g_true = nullptr;
TypeFn        g_trueType = nullptr;
uintptr_t     g_stateRet = 0;
volatile LONG64 g_wReachRefused = 0;   // round 19 / 20: reach-ups (5 / 9) from a hold refused (climb_skip_reach)
climbhand::MoveShift g_mvShift;     // round 19 (game thread): the body through a game move from the hang
unsigned      g_mvSeq = 0;
float         g_mvLast[3] = {};
unsigned      g_mvSeqSeen = 0;      // pad thread: the last shift applied to the held ledge points
uintptr_t     g_lookRet = 0;        // +D0AD1B: the return of +D0A240's set-state call at +D0AD16 (the hang's 2 / 3 look states)
volatile LONG64 g_wLookRefused = 0;
volatile LONG g_stateLive = 0;      // the hang transition detour: 1 live, -1 refused
uintptr_t     g_hangEg = 0;         // the ledge grab whose grab (state 0) should end in a hang (game thread)
ULONGLONG     g_hangUntil = 0;
SetGroupFn    g_setGroup = nullptr;
SetStateFn    g_setState = nullptr;
SetTargetFn   g_setTarget = nullptr;
HangPointFn   g_hangPoint = nullptr;   // P4 round 14: +D16690 (null = bytes did not match: a re-target leaves the grab's own copy stale)
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
volatile ULONGLONG g_stickPushMs = 0;   // pad thread -> StateHook: when the stick last went from not forward to forward (round 13)
volatile ULONGLONG g_grabStartMs = 0;   // pad thread -> StateHook: when the running ledge grab started (round 13)
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
bool          g_postStill = false;      // round 18: the post path sets the body still after a cut-short climb-up (not a launch)
ULONGLONG     g_fastUpStartMs = 0;      // round 18: when the Hook first saw this climb-up's state 8 (log)
int           g_fastUpLines = 0;
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
bool RdU(uintptr_t a, unsigned* o) { __try { *o = *reinterpret_cast<const unsigned*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
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
struct CharCache { uintptr_t obj[8]; bool isChar[8]; bool isLadder[8]; int n; };
CharCache g_charCache = {};
char      g_seenCls[32][48] = {};
int       g_seenN = 0;

bool OwnerIsCharacter(uintptr_t collObj, bool* isLadderOut)
{
    *isLadderOut = false;
    if (!collObj) { return false; }   // static world geometry carries no object
    const uintptr_t owner = collObj - 24;
    for (int i = 0; i < g_charCache.n; ++i) {
        if (g_charCache.obj[i] == collObj) {
            *isLadderOut = g_charCache.isLadder[i];
            return g_charCache.isChar[i];
        }
    }
    char cls[48] = {};
    const bool named = RttiClassNameOf(owner, cls, sizeof(cls));
    const bool isChar = named && climbhand::IsCharacterClass(cls);
    const bool isLadder = named && std::strcmp(cls, "LadderDI") == 0;   // P5: a search ray on a ladder / pipe / pole
    *isLadderOut = isLadder;
    if (g_charCache.n < 8) {
        g_charCache.obj[g_charCache.n] = collObj;
        g_charCache.isChar[g_charCache.n] = isChar;
        g_charCache.isLadder[g_charCache.n] = isLadder;
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
    uintptr_t lastOwner = 0;   // P5: the last hit's owner (0 = static geometry), and whether it is a LadderDI
    bool      lastLadder = false;
    bool operator()(const float from[3], const float to[3], float hit[3])
    {
        InterlockedIncrement64(&g_wCasts);
        lastOwner = 0;
        lastLadder = false;
        __try {
            alignas(16) unsigned char coll[128] = {};
            *reinterpret_cast<int*>(coll + 32) = -1;   // as swing.cpp's replica cast initialises it
            alignas(16) float f[4] = { from[0], from[1], from[2], 0.0f };
            alignas(16) float t[4] = { to[0], to[1], to[2], 0.0f };
            bool r = g_ray(reinterpret_cast<void*>(player + 40), kRayMask, coll, f, t, 0,
                           reinterpret_cast<void*>(player + 24), 8u, 0u, 0) != 0;
            if (r) {
                hit[0] = t[0]; hit[1] = t[1]; hit[2] = t[2];
                const uintptr_t collObj = *reinterpret_cast<const uintptr_t*>(coll + 0x18);
                lastOwner = collObj ? collObj - 24 : 0;
                if (OwnerIsCharacter(collObj, &lastLadder)) {
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

// P5: a climbable's line - its position, forward vector, world extents' bottom / top and kind (climbhand_logic.h's P5 note). False
// when anything is unreadable or implausible.
bool ReadPole(uintptr_t ladder, climbhand::Pole* p)
{
    if (!ladder || !g_pos || !g_fwdFn || !g_extFn) { return false; }
    __try {
        const void* ctl = reinterpret_cast<const void*>(ladder + kLadderCtl);
        alignas(16) float pos[4] = {}, fwd[4] = {};
        g_pos(ctl, pos);
        g_fwdFn(ctl, fwd);
        const float* ext = g_extFn(ctl);
        if (!ext) { return false; }
        int kind = -1;
        if (!RdI(ladder + kLadderKind, &kind)) { return false; }
        for (int i = 0; i < 3; ++i) { p->axis[i] = pos[i]; p->fwd[i] = fwd[i]; }
        p->yLo = ext[1];
        p->yHi = ext[4];
        p->kind = kind;
        const bool ok = std::isfinite(pos[0]) && std::isfinite(pos[2]) && std::isfinite(p->yLo) && std::isfinite(p->yHi) &&
                        p->yHi > p->yLo && p->yHi - p->yLo < 60.0f && kind >= 0 && kind <= 3;
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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

// P4 round 5's launch start, shared by the hands' launch and climb_jump_by_look's jump: the ledge grab's own drop at once, the
// velocity then handed to the walk body (the post path's SetLinearVel for the rise, the carry for the horizontal speed until it
// lands). True = the drop was called. Game thread (Hook).
// The velocity hand-over after a controller has let go of the body (the ledge grab's drop, or the zipline's exit): the post path's
// SetLinearVel calls and the carry. armXp = a launch (a grab that catches it pays Jump180). Game thread.
void HandOffVelocity(const float lv[3], const float feet[3], bool haveFeet, ULONGLONG now, bool armXp)
{
    for (int i = 0; i < 3; ++i) { g_postVel[i] = lv[i]; }
    g_postOn = true;
    g_postLeft = 3;
    g_postUntil = now + 700;
    g_postStill = false;
    if (armXp) { climbhand::ArmLaunchXp(g_launchXp, now, feet, haveFeet); }
    climbhand::StartCarry(g_carry, lv, now);
    g_carryAir = false;
    g_carryFeet0Ms = 0;
    g_carrySaid = false;
    g_traceOn = g_traceLaunches < kTraceLaunches;
    if (g_traceOn) { ++g_traceLaunches; }
}

bool StartLaunch(uintptr_t eg, const float lv[3], const float feet[3], bool haveFeet, ULONGLONG now)
{
    const bool done = CallSetState(eg, kStateDrop);
    InterlockedIncrement64(done ? &g_wDrops : &g_wRefused);
    if (!done) { return false; }
    InterlockedIncrement64(&g_wLaunches);
    HandOffVelocity(lv, feet, haveFeet, now, true);
    return true;
}

// P6 round 3: the hands let go of the zipline (req 1: the grips opened; 2: a hand slipped off). The game's own exit (leave group 27,
// the walk's exit flag), then the ride's along-the-cable momentum - plus, when the grips opened while the hands pulled fast, a launch
// picked as off a ledge (LaunchPick). Game thread (Hook).
void ZipEnd(uintptr_t player, LONG req, const float feet[3], bool haveFeet, ULONGLONG now)
{
    uintptr_t ctl = 0;
    unsigned char riding = 0;
    const bool on = RdQ(player + kZipCtlSlot, &ctl) && ctl && ctl == g_zipCtlPtr && g_zipCtlOk && RdB(ctl + 0x10, &riding) && riding;
    float pick[3] = {};
    const bool launch = on && req == 1 && ClimbHandMotion &&
                        climbhand::LaunchPick(g_motion, now, ClimbLaunchMinMps, ClimbLaunchScale, pick, ClimbLaunchUpAssist);
    g_motion.ringN = 0;
    bool left = false;
    const char* why = "the ride had already ended";
    if (on && g_leaveGroup) {
        __try {
            g_leaveGroup(player, kGroupZipLine);
            uintptr_t walk = 0;
            if (RdQ(player + kWalkCtlSlot, &walk) && walk) { *reinterpret_cast<unsigned char*>(walk + kWalkExitFlag) = 1; }
            left = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            why = "the leave-group call faulted";
        }
    }
    unsigned char after = 1;
    if (left) { RdB(ctl + 0x10, &after); }
    float lv[3] = {};
    for (int i = 0; i < 3; ++i) { lv[i] = g_zipAlong[i] + (launch ? pick[i] : 0.0f); }
    if (left) {
        HandOffVelocity(lv, feet, haveFeet, now, launch);
        InterlockedIncrement64(&g_wZipEnds);
        if (launch) { InterlockedIncrement64(&g_wZipLaunches); InterlockedIncrement64(&g_wLaunches); }
    }
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] ZIPLINE LET GO (%s) - %s | the controller %s | velocity (%.2f, %.2f, %.2f) m/s = the ride's along the cable "
            "(%.2f, %.2f, %.2f)%s", req == 1 ? "the grips opened" : "a hand SLIPPED off the cable",
            left ? (launch ? "a LAUNCH off the cable" : "a drop") : why,
            !left ? "untouched" : (after ? "is STILL ACTIVE after leaving the group" : "is off"), lv[0], lv[1], lv[2], g_zipAlong[0],
            g_zipAlong[1], g_zipAlong[2], launch ? " + the hands' launch" : (req == 1 ? " (the hands were not pulling fast enough to "
            "launch: climb_launch_min_mps)" : ""));
    }
}

// P6 round 3: the walk call while the zipline rides (ClimbHandWalkOverride): the ride's along-the-cable part of its wanted velocity
// is kept (and counted as the cable's advance for the hands' anchors); while the hands hold, their pull replaces the rest (the
// ride's pull toward its hang point would fight them). False = the game's own vector. Game thread.
bool ZipWalk(uintptr_t player, const float in[3], float out[3])
{
    if (!g_zipList || !g_zipCtlOk || !g_leaveGroup || player != LocalPlayer()) { return false; }
    uintptr_t ctl = 0;
    unsigned char riding = 0;
    if (!RdQ(player + kZipCtlSlot, &ctl) || ctl != g_zipCtlPtr || !RdB(ctl + 0x10, &riding) || !riding) { return false; }
    const float a = in[0] * g_zipD[0] + in[1] * g_zipD[1] + in[2] * g_zipD[2];
    float along[3];
    for (int i = 0; i < 3; ++i) { along[i] = a * g_zipD[i]; g_zipAlong[i] = along[i]; }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
    const double dt = g_zipQpc.QuadPart ? static_cast<double>(q.QuadPart - g_zipQpc.QuadPart) / g_qpf.QuadPart : 0.0;
    g_zipQpc = q;
    bool holdOn = false;
    climbhand::Hold h;
    ULONGLONG at = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (dt > 0.0 && dt < 0.1) { for (int i = 0; i < 3; ++i) { g_zipAdvSum[i] += along[i] * static_cast<float>(dt); } }
    holdOn = g_zipHoldOn;
    h.drive = g_zipDrive;
    for (int i = 0; i < 3; ++i) { h.err[i] = g_zipErr[i]; }
    at = g_zipPubMs;
    ReleaseSRWLockExclusive(&g_lock);
    const ULONGLONG now = GetTickCount64();
    if (!holdOn || now - at > 100 || !h.drive) { return false; }
    float feet[3];
    if (!Feet(player, feet)) { return false; }
    const climbhand::MotionOut mo = climbhand::MotionStep(g_motion, h, feet, now, 0.0f, ClimbPullMaxMps,
                                                          ClimbLaunchMinMps > 0.0f ? ClimbLaunchMinMps : 1.5f);
    for (int i = 0; i < 3; ++i) { out[i] = along[i] + (mo.send ? mo.v[i] : 0.0f); }
    InterlockedIncrement64(&g_wZipDriven);
    return true;
}

// P6 X1: a grab just started - when it catches a launch (LaunchCatchPays), ask the game's climb award for climb type 5 (Jump180).
// what names the grab for the log. Game thread, right after a successful start.
void PayLaunchCatch(uintptr_t player, const float feet[3], ULONGLONG now, const char* what)
{
    const ULONGLONG since = now - g_launchXp.t0;
    float dist = -1.0f;
    const bool pays = climbhand::LaunchCatchPays(g_launchXp, now, feet, &dist);
    if (dist < 0.0f) { return; }   // no launch to catch
    uintptr_t xp = 0;
    bool asked = false;
    if (pays && g_climbXp && RdQ(player + kXpModuleOff, &xp) && xp) {
        __try {
            g_climbXp(xp, climbhand::kXpTypeJump180);
            asked = true;
            InterlockedIncrement64(&g_wXpAsked);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement64(&g_wXpFaults);
        }
    }
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] LAUNCH CAUGHT by a %s %llu ms after the launch, the feet %.2f m from where they left (XP needs %.2f) | climb "
            "XP: %s", what, since, static_cast<double>(dist), static_cast<double>(climbhand::kLaunchXpMinM),
            !pays ? "none (too close to where the launch left: a hop back onto the same hold)" :
            asked ? "asked the game's award for climb type 5 (Jump180, 10 / 20 XP; its own 1.3 m / 5 s anti-farm may still refuse)" :
            !g_climbXp ? "*** none - the award's bytes did not match at install ***" :
            !xp ? "*** none - PlayerDI+0x13E0 (the XP module) is null ***" : "*** none - the award call FAULTED ***");
    }
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
    if (!DoStart(player, eg, hook, why, false)) { return false; }
    // P4 round 14 (tester 2026-11-27, four stacked ledges: "when I physically jump up to the last one and move the joystick up, it
    // moves me to the bottom ledge and shimmies up to the second ... and when I press up THEN it mantles"; run 18 climb #9: three
    // 1 -> 13, each re-targeted onto the top ledge and each shimmying up from ONE ledge higher than the last). DoStart rewrites the
    // player's ledge (+0x14C8 / +0x14BC) and the target record, but the ledge grab keeps its own copy of the ledge point (eg+0x4C)
    // and the hang position and hand points made from it - set when the hang began, and by each finished shimmy. Its stick-up starts
    // from that copy. The game's own refresh when the player's ledge changes (+D14A08) is +D16690 on the grab: run it here too.
    if (g_hangPoint) {
        __try {
            g_hangPoint(eg);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *why = "the hang-point refresh (+D16690) faulted - the game's ledge was rewritten, its own copy may be stale";
            return false;
        }
    }
    return true;
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

// P5 round 2: a ladder / pipe / pole's own surface within reach of this hand (PoleSurfaceFind: short rays around the hand, a hit
// owned by a LadderDI) - grabbed there, as a hang on a ledge made at that point (PoleLedgeAt). climbing = a pole hang runs: the
// hand joins it. False = no climbable surface in reach (nothing done). Game thread.
bool ServePole(int hand, const float pos[3], uintptr_t player, uintptr_t eg, uintptr_t hook, bool climbing, const float feet[3],
               ULONGLONG now)
{
    Caster pc{ player };
    uintptr_t owners[climbhand::kSurfDirs] = {};
    int ray = 0;
    auto cast = [&](const float a[3], const float b[3], float hit[3]) {
        const bool r = pc(a, b, hit);
        if (ray < climbhand::kSurfDirs) { owners[ray] = r && pc.lastLadder ? pc.lastOwner : 0; }
        ++ray;
        return r;
    };
    auto isTarget = [&]() { return pc.lastLadder; };
    float hit[3] = {}, d = -1.0f;
    int rays = 0, best = -1;
    const bool found = climbhand::PoleSurfaceFind(pos, ClimbReachM, cast, isTarget, hit, &d, &rays, &best);
    if (pc.fault) { InterlockedIncrement64(&g_wFaults); }
    if (!found || best < 0 || best >= climbhand::kSurfDirs || !owners[best]) { return false; }
    const uintptr_t obj = owners[best];
    climbhand::Pole P;
    const bool haveP = ReadPole(obj, &P);
    const float noFwd[3] = {};
    climbhand::Ledge L;
    if (!climbhand::PoleLedgeAt(hit, feet, haveP ? P.fwd : noFwd, &L)) { return false; }
    L.dist = d;
    const char* const what = !haveP ? "climbable" : P.kind == 0 ? "ladder" : P.kind == 1 ? "pipe" : "rope";
    if (obj != g_poleSaidObj && Line()) {
        g_poleSaidObj = obj;
        // [PROBE: CLIMBHAND] gate=climb_ladder_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] CLIMBABLE %p: %s (kind %d) | position (%.2f, %.2f, %.2f) | forward (%.2f, %.2f, %.2f) | world extents y %.2f "
            "..%.2f | first held at (%.2f, %.2f, %.2f), %.2f m from the hand | the feet at (%.2f, %.2f, %.2f)%s",
            reinterpret_cast<void*>(obj), what, haveP ? P.kind : -1, P.axis[0], P.axis[1], P.axis[2], P.fwd[0], P.fwd[1], P.fwd[2],
            P.yLo, P.yHi, hit[0], hit[1], hit[2], d, feet[0], feet[1], feet[2],
            haveP ? "" : " - its geometry is UNREADABLE (facing from the feet only)");
    }
    if (climbing) {
        PublishFound(hand, now, L.lip, L.into);
        if (haveP) { g_pole = P; }
        g_poleObj = obj;
        if (g_lastWhy[hand] != 101 && HandLine()) {
            // [PROBE: CLIMBHAND] gate=climb_ladder_by_hand  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] JOIN - %s hand is on the %s (at feet+%.2f m, %.2f m from the hand): it holds too",
                hand == 0 ? "left" : "right", what, L.lip[1] - feet[1], d);
        }
        g_lastWhy[hand] = 101;
        return true;
    }
    const char* whyNot = "ok";
    bool done = false;
    if (!CallSetTarget(player, L)) {
        whyNot = "the set-target call faulted";
    } else {
        g_forceHangUntil = g_typeLive == 1 ? now + 1000 : 0;   // a climbable is always hung from
        g_hangEg = g_stateLive == 1 ? eg : 0;
        g_hangUntil = now + 3000;
        done = DoStart(player, eg, hook, &whyNot);
        if (!done) { g_forceHangUntil = 0; g_hangEg = 0; }
    }
    InterlockedIncrement64(done ? &g_wStarts : &g_wRefused);
    if (done) {
        PublishFound(hand, now, L.lip, L.into);
        g_poleOn = true;
        g_pole = haveP ? P : climbhand::Pole();
        g_poleObj = obj;
        g_poleStartMs = now;
        PayLaunchCatch(player, feet, now, "pole / ladder grab");
    }
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=climb_ladder_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] POLE GRAB %s - %s hand on the %s's surface at feet+%.2f m (%.2f m from the hand, %d rays) | facing (%.2f, %.2f) "
            "| a hang of the ledge grab on a ledge made there%s", done ? "started" : "REFUSED", hand == 0 ? "left" : "right", what,
            L.lip[1] - feet[1], d, rays, L.into[0], L.into[2],
            g_stateLive == 1 ? "" : " - the hang detour is NOT LIVE (the game may climb over instead)");
        if (!done) { LOG("[CLIMBHAND]   refused because: %s", whyNot); }
    }
    g_lastWhy[hand] = -1;
    return true;
}

// P6: a grip that found no ledge, near a zipline: start the game's zipline on that line (climbhand_logic.h ZipLine note). True = the
// hand was within reach of a line (started or refused - logged). Game thread, inside the start detour.
bool ServeZip(int hand, const float pos[3], uintptr_t player, const float feet[3])
{
    if (!g_zipList || !g_zipName) { return false; }
    unsigned count = 0;
    uintptr_t arr = 0;
    if (!RdQ(g_zipList, &arr) || !arr || !RdU(g_zipList + 8, &count) || count == 0 || count > kZipMaxLines) { return false; }
    uintptr_t best = 0;
    float bestD = -1.0f, bestC[3] = {};
    climbhand::ZipLine bestZ;
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t e = 0, model = 0;
        climbhand::ZipLine z;
        if (!RdQ(arr + 8ull * i, &e) || !e || !RdF3(e + kZipLineA, z.a) || !RdF3(e + kZipLineA + 0xC, z.b) ||
            !RdF3(e + kZipLineA + 0x18, z.d) || !RdQ(e + kZipLineModel, &model) || !model) {
            continue;
        }
        if (g_zipRenderable) {   // the game's own filter (+B19ADF): a hidden line is never offered
            bool vis = false;
            __try { vis = g_zipRenderable(model); } __except (EXCEPTION_EXECUTE_HANDLER) { vis = false; }
            if (!vis) { continue; }
        }
        float c[3];
        const float d = climbhand::ZipDistance(pos, z, c);
        if (d >= 0.0f && (bestD < 0.0f || d < bestD)) { bestD = d; best = e; bestZ = z; for (int k = 0; k < 3; ++k) { bestC[k] = c[k]; } }
    }
    uintptr_t ctl = 0;
    RdQ(player + kZipCtlSlot, &ctl);
    if (ctl != g_zipCtlPtr) {
        g_zipCtlPtr = ctl;
        char cls[64] = {};
        g_zipCtlOk = ctl && RttiClassNameOf(ctl, cls, sizeof(cls)) && std::strcmp(cls, "ZipLineController") == 0;
        if (Line()) {
            // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand  site=FLAT  log-only=no
            LOG("[CLIMBHAND] PlayerDI+0x%llX = %p class '%s' -> %s", static_cast<unsigned long long>(kZipCtlSlot),
                reinterpret_cast<void*>(ctl), cls, g_zipCtlOk ? "the zipline controller, used" : "*** NOT ZipLineController - "
                "ziplines by hand stay off for this player ***");
        }
    }
    unsigned char riding = 0;
    if (g_zipCtlOk) { RdB(ctl + 0x10, &riding); }
    if (!climbhand::ZipGrabOk(ClimbByHand, ClimbZiplineByHand, false, riding != 0, bestD, ClimbReachM)) { return false; }
    if (!g_zipCtlOk) { return false; }
    const char* why = "ok";
    bool done = false;
    __try {   // the game's own start (+BD4374..+BD43BF): the line record, the entity, its name, then group 27
        float* rec = reinterpret_cast<float*>(ctl + kZipCtlLine);
        for (int k = 0; k < 3; ++k) { rec[k] = bestZ.a[k]; rec[3 + k] = bestZ.b[k]; rec[6 + k] = bestZ.d[k]; }
        *reinterpret_cast<uintptr_t*>(ctl + kZipCtlEnt) = best;
        g_zipName(ctl + kZipCtlName, best + kZipLineName);
        g_setGroup(player, kGroupZipLine, true, nullptr);
        done = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a write or the game call faulted";
    }
    unsigned char nowRiding = 0;
    if (done) { RdB(ctl + 0x10, &nowRiding); }
    if (done && nowRiding && g_leaveGroup) {   // round 3: this hand holds the cable at the grab point (the pad thread's ZipHold)
        for (int k = 0; k < 3; ++k) { g_zipD[k] = bestZ.d[k]; g_zipAlong[k] = 0.0f; }
        g_zipQpc.QuadPart = 0;
        g_motion.ringN = 0;
        AcquireSRWLockExclusive(&g_lock);
        ++g_zipStartSeq;
        g_zipStartHand = hand;
        for (int k = 0; k < 3; ++k) { g_zipStartPt[k] = bestC[k]; }
        g_zipStartLine = bestZ;
        ReleaseSRWLockExclusive(&g_lock);
    }
    InterlockedIncrement64(done ? &g_wZipStarts : &g_wZipRefused);
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] ZIPLINE %s - %s hand %.2f m from line %p (grab point (%.2f, %.2f, %.2f), the feet at (%.2f, %.2f, %.2f)) | line "
            "(%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f), %d line(s) in the world | the controller %s after the group switch%s%s",
            done ? "STARTED" : "REFUSED", hand == 0 ? "left" : "right", static_cast<double>(bestD), reinterpret_cast<void*>(best),
            bestC[0], bestC[1], bestC[2], feet[0], feet[1], feet[2], bestZ.a[0], bestZ.a[1], bestZ.a[2], bestZ.b[0], bestZ.b[1],
            bestZ.b[2], static_cast<int>(count), nowRiding ? "is ACTIVE" : "is NOT active (the game did not take it)",
            done ? "" : " - ", done ? "" : why);
    }
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
    int why = climbhand::FindLedge(pos, feet, ClimbReachM, cast, &L);
    if (cast.fault) { InterlockedIncrement64(&g_wFaults); }
    InterlockedIncrement64(&g_wWhy[why]);
    // P5 round 2: on a ladder / pole (or before any climb), a real ledge comes first - the top of a ladder is climbed onto from it
    // (tester: "There is no way to mantle on top of ladders ... getting on top is critical"). But a "ledge" whose top is the
    // climbable's own collider (a rung, the ladder's top) is no ledge: one ray down just past the lip names it.
    const bool poleMode = ClimbLadderByHand && (!climbing || g_poleOn);
    bool ladderTop = false;
    if (why == climbhand::kLedgeOk && poleMode) {
        const float from[3] = { L.lip[0] + L.into[0] * 0.05f, L.lip[1] + 0.20f, L.lip[2] + L.into[2] * 0.05f };
        const float to[3] = { from[0], L.lip[1] - 0.15f, from[2] };
        float h[3] = {};
        ladderTop = cast(from, to, h) && cast.lastLadder;
    }
    if (why == climbhand::kLedgeOk && climbing && !ladderTop) {
        if (g_poleOn) {   // from the ladder / pole onto a real ledge: the hang goes over to it (climb-up, shimmies, the stick)
            const char* rwhy = "ok";
            const bool moved = RetargetInPlace(player, eg, L.lip, L.into, &rwhy);
            if (moved) { g_poleOn = false; }
            if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_ladder_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] LEDGE FROM THE %s - %s hand on a ledge (lip at feet+%.2f m, %.2f m from the hand): the hang %s",
                    g_pole.kind == 0 ? "LADDER" : "POLE", hand == 0 ? "left" : "right", L.lip[1] - feet[1], L.dist,
                    moved ? "is the ledge's now (the climb-up / stick work from it)" : "STAYS on the climbable - the re-target failed:");
                if (!moved) { LOG("[CLIMBHAND]   %s", rwhy); }
            }
        }
        PublishFound(hand, now, L.lip, L.into);
        if (g_lastWhy[hand] != 100 && HandLine()) {
            // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] JOIN - %s hand is on a ledge (lip at feet+%.2f m, %.2f m from the hand) while the grab runs: it holds too",
                hand == 0 ? "left" : "right", L.lip[1] - feet[1], L.dist);
        }
        g_lastWhy[hand] = 100;
        return;
    }
    if (poleMode && ServePole(hand, pos, player, eg, hook, climbing, feet, now)) { return; }
    // P6: no ledge and no climbable here - a zipline in reach of this hand rides it (the game's own ride from there)
    if (!climbing && why != climbhand::kLedgeOk && ClimbZiplineByHand && ServeZip(hand, pos, player, feet)) {
        g_lastWhy[hand] = -1;
        return;
    }
    if (climbing && g_poleOn) { return; }   // on a pole: nothing else this hand can hold
    if (why == climbhand::kLedgeOk && ladderTop) {   // a rung / the ladder's top - and its surface was not within reach either
        if (g_lastWhy[hand] != 103 && HandLine()) {
            // [PROBE: CLIMBHAND] gate=climb_ladder_by_hand  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] search (%s hand at feet+(%.2f, %.2f, %.2f)): the only ledge found is a ladder / pole's own top or rung "
                "(not a ledge), and none of its surface is within climb_reach_m %.2f", hand == 0 ? "left" : "right",
                pos[0] - feet[0], pos[1] - feet[1], pos[2] - feet[2], ClimbReachM);
        }
        g_lastWhy[hand] = 103;
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
    if (done) { PayLaunchCatch(player, feet, now, "ledge grab"); }
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
    bool  haveEg = false;  // round 14: the ledge grab's own copy of the ledge point (eg+0x4C) was read
    climbhand::Hold h;
    float hookMid[3] = {}, hookDir[3] = {}, egMid[3] = {};
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
    s.haveEg = g_egOk && g_egPtr && RdF3(g_egPtr + kEgLedgeMid, s.egMid);
    // round 14: stale when EITHER the player's ledge or the grab's own copy of it is not the held one (the copy is what its
    // stick-up starts from; the player's ledge is what we write, so it alone always reads "the same" after a re-target)
    s.stale = climbhand::HeldStale(s.held, s.h.mid, s.h.into, s.haveHook, s.hookMid, s.hookDir, s.haveEg, s.egMid);
    return s;
}

// Round 4 climb_room_check: may this climb-up from a hang run (climbhand_logic.h ClimbUpRoom)? On the held ledge (else the game's own
// ledge point). Room to crouch only arms the held crouch (g_crouchClimb; the Hook keeps it while there is no standing room over the
// feet). who = what asked, for the log. Game thread.
bool       g_crouchClimb = false;
ULONGLONG  g_crouchClimbMs = 0;
volatile LONG64 g_wRoomRefused = 0, g_wRoomCrouch = 0, g_wRoomOk = 0;
bool ClimbUpRoomOk(uintptr_t player, ULONGLONG now, const char* who)
{
    if (!ClimbRoomCheck || !player) { return true; }
    const HeldVsGame s = ReadHeldVsGame(now);
    float lip[3], into[3];
    if (s.held) {
        for (int i = 0; i < 3; ++i) { lip[i] = s.h.mid[i]; into[i] = s.h.into[i]; }
    } else if (s.haveHook) {
        for (int i = 0; i < 3; ++i) { lip[i] = s.hookMid[i]; into[i] = s.hookDir[i]; }
    } else {
        return true;   // no ledge to test on: the game decides
    }
    // the game asks for the stick's climb-up every update while the stick is held: one test per ledge point per 300 ms
    static float s_cLip[3] = { NAN, NAN, NAN };
    static ULONGLONG s_cMs = 0;
    static int s_cRoom = climbhand::kRoomStand;
    static float s_cHit = 99.0f;
    int room;
    float hitY = 99.0f;
    const float dl = std::fabs(lip[0] - s_cLip[0]) + std::fabs(lip[1] - s_cLip[1]) + std::fabs(lip[2] - s_cLip[2]);
    if (now - s_cMs <= 300 && dl < 0.05f) {
        room = s_cRoom;
        hitY = s_cHit;
    } else {
        Caster cast{ player };
        room = climbhand::ClimbUpRoom(lip, into, cast, &hitY);
        if (cast.fault) { InterlockedIncrement64(&g_wFaults); return true; }
        for (int i = 0; i < 3; ++i) { s_cLip[i] = lip[i]; }
        s_cMs = now; s_cRoom = room; s_cHit = hitY;
    }
    if (room == climbhand::kRoomCrouch) {
        g_crouchClimb = true;
        g_crouchClimbMs = now;
        PhysCrouchForceUntil(now + 1000);
        InterlockedIncrement64(&g_wRoomCrouch);
    }
    if (room != climbhand::kRoomNone) { InterlockedIncrement64(&g_wRoomOk); }
    else { InterlockedIncrement64(&g_wRoomRefused); }
    static ULONGLONG s_saidMs = 0;
    static int s_saidRoom = -1;
    if ((room != climbhand::kRoomStand || s_saidRoom != room) && (room != s_saidRoom || now - s_saidMs > 3000) && Line()) {
        s_saidMs = now;
        s_saidRoom = room;
        // [PROBE: CLIMBHAND] gate=climb_room_check  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] CLIMB-UP ROOM (%s) - the spot %.2f m past the lip (%.2f, %.2f, %.2f): %s | the lowest thing over the top %s%.2f m "
            "(stand needs %.2f, crouch %.2f, from %.2f up - lower debris ignored)", who, static_cast<double>(climbhand::kRoomSpotInM),
            lip[0], lip[1], lip[2],
            room == climbhand::kRoomStand ? "room to STAND - the climb-up runs"
            : room == climbhand::kRoomCrouch ? "room to CROUCH only - the climb-up runs and the crouch is held until there is room to stand"
                                             : "NO ROOM (not even crouched) - the climb-up is REFUSED, the hang stays",
            hitY >= 99.0f ? "none, " : "at ", static_cast<double>(hitY >= 99.0f ? 0.0f : hitY),
            static_cast<double>(climbhand::kStandRoomM), static_cast<double>(climbhand::kCrouchRoomM),
            static_cast<double>(climbhand::kRoomFromM));
    }
    return room != climbhand::kRoomNone;
}

// Every Hook: the crouch held after a crouched climb-up - renewed while the climb runs, then while the spot over the feet has no
// standing room (2 rays); let go once there is, after 30 s, or when the player is no longer on foot (a ladder, a vehicle ...).
void CrouchClimbTick(uintptr_t player, bool climbing, bool onFoot, const float feet[3], bool haveFeet, ULONGLONG now)
{
    if (!g_crouchClimb) { return; }
    bool keep = climbing;
    const char* why = "";
    if (!climbing) {
        if (!onFoot) { why = "the player is no longer on foot"; }
        else if (now - g_crouchClimbMs > 30000) { why = "30 s passed"; }
        else if (haveFeet) {
            Caster cast{ player };
            const bool room = climbhand::StandRoomAt(feet, cast);
            keep = !room && !cast.fault;
            why = room ? "there is room to stand over the feet" : "a cast faulted";
        } else {
            keep = true;   // no feet this update: keep it one more
        }
    }
    if (keep) { PhysCrouchForceUntil(now + 300); return; }
    g_crouchClimb = false;
    PhysCrouchForceUntil(0);
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=climb_room_check  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] climb-up CROUCH let go after %llu ms - %s", static_cast<unsigned long long>(now - g_crouchClimbMs), why);
    }
}

// Re-target the running grab onto the held ledge (RetargetInPlace) and say so. Game thread.
void RetargetOnHeld(uintptr_t player, uintptr_t eg, const HeldVsGame& s, ULONGLONG now, const char* when)
{
    const char* why = "ok";
    const bool done = player && RetargetInPlace(player, eg, s.h.mid, s.h.into, &why);
    g_retargetMs = now;   // a failed one also waits kRetargetHoldMs (the game's move then passes as it asked)
    InterlockedIncrement64(done ? &g_wRetargets : &g_wRetargetFails);
    float egAfter[3] = {};
    const bool egAfterOk = eg && RdF3(eg + kEgLedgeMid, egAfter);
    if (Line()) {
        char egs[200];
        if (!g_hangPoint) {
            std::snprintf(egs, sizeof(egs), " | the grab's own copy NOT refreshed (+D16690 bytes did not match at install) - it stays "
                          "(%.2f, %.2f, %.2f)", s.egMid[0], s.egMid[1], s.egMid[2]);
        } else if (s.haveEg && egAfterOk) {
            std::snprintf(egs, sizeof(egs), " | the grab's own copy (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) (+D16690)", s.egMid[0],
                          s.egMid[1], s.egMid[2], egAfter[0], egAfter[1], egAfter[2]);
        } else {
            std::snprintf(egs, sizeof(egs), " | the grab's own copy unreadable");
        }
        // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] RE-TARGET %s (%s) - the game's ledge (%.2f, %.2f, %.2f) dir (%.2f, %.2f) -> the held one (%.2f, %.2f, %.2f) "
            "dir (%.2f, %.2f)%s%s%s", done ? "done" : "REFUSED", when, s.hookMid[0], s.hookMid[1], s.hookMid[2], s.hookDir[0],
            s.hookDir[2], s.h.mid[0], s.h.mid[1], s.h.mid[2], s.h.into[0], s.h.into[2], done ? "" : " - ", done ? "" : why, egs);
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
    if (const LONG zreq = InterlockedExchange(&g_reqZipEnd, 0)) { ZipEnd(player, zreq, feet, haveFeet, now); }   // P6 round 3
    bool active = act != 0;
    {   // round 4: when the ledge grab ended (the holds' tails: climbhand_logic.h BodyHoldTail / ViewHoldTail)
        static bool s_prevAct = false;
        static int s_prevState = -1;
        if (readOk) {
            if (s_prevAct && !active) {
                g_grabEndClimbUp = s_prevState == 8 || s_prevState == 10;   // round 6: a climb-up's end - no body tail (set first:
                g_grabEndMs = now;                                          // other threads read the time, then the flag)
            }
            s_prevAct = active;
            s_prevState = active ? state : -1;
        }
    }
    // climb_skip_grab_anim: the hand grab due to hang goes 0 -> 1 on its first update in the grab - no grab animation
    if (readOk && g_hangEg != 0 && eg == g_hangEg && state == 0) {
        const bool due = now <= g_hangUntil && ClimbAlwaysHang;   // as StateHook's HangTransition would decide at the grab's end
        if (climbhand::SkipGrabAnim(ClimbSkipGrabAnim, ClimbByHand, true, due, active, state)) {
            g_hangEg = 0;   // one grab, one decision (StateHook's end-of-grab hang is not needed now)
            const bool done = CallSetState(eg, 1);
            int now1 = -1;
            RdI(eg + kEgStateOff, &now1);
            if (done && now1 == 1) { state = 1; InterlockedIncrement64(&g_wGrabAnimSkipped); InterlockedIncrement64(&g_wHung); }
            if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_skip_grab_anim  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] GRAB ANIMATION SKIPPED - set-state 1 %s (state 0 -> %d): the hang starts now; the body glides onto the "
                    "hand's ledge point at up to %.2f m/s (climb_grab_move_mps)", done ? "called" : "FAULTED", now1,
                    static_cast<double>(climbhand::GrabGlideCap(true, ClimbGrabMoveMps, ClimbPullMaxMps)));
            }
        }
    }
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
    CrouchClimbTick(player, act != 0, onFoot, feet, haveFeet, now);   // round 4: the crouch held after a crouched climb-up

    if (!act) { g_climbUpAsked = false; g_wantClimbUp = false; }
    // P5: the pole hang is over once the grab is no longer active (after the start's own update or two)
    if (g_poleOn && !act && now - g_poleStartMs > 1000) { g_poleOn = false; }
    if (g_poleOn) { g_wantClimbUp = false; }   // climb_mantle_auto: no climb-up from a made-up ledge
    if (g_wantClimbUp) {   // climb_mantle_auto: pulled up far enough - the game's own climb-up, as the stick asks for it (1 -> 8)
        g_wantClimbUp = false;
        bool can = act != 0 && climbhand::IsHoldState(state);
        bool noRoom = false;
        if (can) {   // P4 round 3: from the ledge the hands hold, not the one the grab started on
            const HeldVsGame s = ReadHeldVsGame(now);
            if (s.held) { RetargetOnHeld(player, eg, s, now, "before climb_mantle_auto's climb-up"); }   // round 8: on demand
            if (!ClimbUpRoomOk(player, now, "the pull height, climb_mantle_auto")) { can = false; noRoom = true; }   // round 4
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
                "(state %d -> %d)", g_climbUpGap, ClimbMantleM,
                done ? "called" : (can ? "FAULTED" : (noRoom ? "not called: no room on top (climb_room_check)" : "not called: not a plain hold")),
                state, now8);
        }
    }
    if (g_reqJumpMs != 0) {   // climb_jump_by_look: StateHook refused the game's hang jump - ours, toward the look
        const ULONGLONG askMs = g_reqJumpMs;
        g_reqJumpMs = 0;
        const bool can = act != 0 && climbhand::IsHoldState(state) && now - askMs <= 200;
        float look[3] = {}, into[3] = {};
        ULONGLONG lookMs = 0;
        AcquireSRWLockShared(&g_lock);
        for (int i = 0; i < 3; ++i) { look[i] = g_lookFwd[i]; }
        lookMs = g_lookMs;
        ReleaseSRWLockShared(&g_lock);
        const bool haveLook = lookMs != 0 && now >= lookMs && now - lookMs <= 200;
        float wall[3] = {};
        const bool haveWall = RdF3(player + kHookDir, wall);
        // round 21: on a pole / ladder, toward = the feet -> where the hands hold now (climbhand_logic.h JumpInto)
        const HeldVsGame hv = ReadHeldVsGame(now);
        const int intoSrc = climbhand::JumpInto(g_poleOn, feet, haveFeet, hv.h.mid, hv.held, wall, haveWall, into);
        const bool haveInto = intoSrc != 0;
        float lv[3] = {}, yaw = -1.0f;
        const bool off = climbhand::LookJumpVelocity(look, haveLook && haveInto, into, ClimbJumpSideDeg, lv, &yaw);
        const bool done = can && StartLaunch(eg, lv, feet, haveFeet, now);
        if (done) { InterlockedIncrement64(off ? &g_wJumpsOff : &g_wJumpsUp); }
        if (Line()) {
            // [PROBE: CLIMBHAND] gate=climb_jump_by_look  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] JUMP BUTTON from a %s hang (state %d) - %s: %s | velocity (%.2f, %.2f, %.2f) m/s (the game's full jump: "
                "%.2f up, %.2f sideways) | the look (%.2f, %.2f) %s %.0f deg from into the %s (climb_jump_side_deg %.0f) - into (%.2f, "
                "%.2f) from %s",
                g_poleOn ? (g_pole.kind == 0 ? "ladder" : "pole") : "ledge", g_reqJumpFrom,
                done ? "JUMPED" : (can ? "the drop FAULTED" : "not jumped: no longer a plain hold"),
                off ? "a push-off along the look" : "straight up", lv[0], lv[1], lv[2], climbhand::kLaunchMaxUpMps,
                climbhand::kLaunchMaxSideMps, look[0], look[2], !haveLook ? "(UNKNOWN: no fresh headset pose, so up)" :
                !haveInto ? "(the wall direction is UNREADABLE, so up)" : yaw < 0.0f ? "(level: no left / right to it, so up)" : "is",
                static_cast<double>(yaw), g_poleOn ? "climbable" : "wall", static_cast<double>(ClimbJumpSideDeg), into[0], into[2],
                intoSrc == 2 ? "the feet to where the hands hold now" : intoSrc == 1 ? (g_poleOn ? "the grab's ledge direction (the hold "
                "unknown or right over the feet)" : "the wall's direction") : "nothing");
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
            const bool done = StartLaunch(eg, lv, feet, haveFeet, now);
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
                    (req == 3 ? " - a hand tore off in a flick, but no launch speed was picked" :
                     (req == 4 ? " - the SLIDE reached the bottom of the ladder / pole (or the ground)" : "")),
                    static_cast<double>(g_dropHandMps), static_cast<double>(ClimbLaunchMinMps));
            }
        }
    }
    // Round 18 (climb_fast_climb_up, climbhand_logic.h FastClimbUpDone): the game's climb-up from a hang ends once the body stands on
    // the top past the lip - the game's own set-state 10, whose first update leaves the climb (+BB2390). Its exit hands the walk the
    // climb-up's velocity (still rising there), so the post path below then sets the body still (no hop).
    if (act && state == 8) {
        if (g_fastUpStartMs == 0) { g_fastUpStartMs = now; }
        float mid[3] = { NAN, NAN, NAN }, dir[3] = {};
        if (ClimbFastClimbUp && g_ol.climbing && haveFeet && RdF3(player + kHookMid, mid) && RdF3(player + kHookDir, dir) &&
            climbhand::FastClimbUpDone(true, true, state, feet, mid, dir)) {
            const bool done = CallSetState(eg, 10);
            int after = -1;
            RdI(eg + kEgStateOff, &after);
            if (done) {
                for (int i = 0; i < 3; ++i) { g_postVel[i] = 0.0f; }
                g_postOn = true;
                g_postLeft = 3;
                g_postUntil = now + 700;
                g_postStill = true;
            }
            if (g_fastUpLines < 40 && Line()) {
                ++g_fastUpLines;
                const float il = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
                const float in = il > 1e-3f ? ((feet[0] - mid[0]) * dir[0] + (feet[2] - mid[2]) * dir[2]) / il : NAN;
                // [PROBE: CLIMBHAND] gate=climb_fast_climb_up  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] CLIMB-UP CUT SHORT %llu ms into the game's climb-up (its own runs 2240 ms): the feet %.1f cm over the "
                    "top and %.1f cm past the lip (needs %.0f / %.0f) - set-state 10 %s (state now %d); the body is then set still",
                    static_cast<unsigned long long>(now - g_fastUpStartMs), 100.0f * (feet[1] - mid[1]), 100.0f * in,
                    100.0f * climbhand::kFastUpOverM, 100.0f * climbhand::kFastUpInM, done ? "called" : "FAULTED", after);
            }
        }
    } else {
        g_fastUpStartMs = 0;
    }
    // After a launch's drop: once the ledge grab has handed back to the walk body, give it the launch velocity (its rise - the walk's
    // own update then overwrites the horizontal with its wanted vector, which the carry fills). A few updates in a row, as the
    // controller switch may reset the body once.
    if (g_postOn) {
        if (now > g_postUntil) {
            if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_launch_min_mps  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] %s: %d of 3 call(s) made before the time ran out",
                    g_postStill ? "setting the body still after the cut-short climb-up" : "launch velocity after the drop",
                    3 - g_postLeft);
            }
            g_postOn = false;
            g_postStill = false;
        } else if (!act && g_postLeft > 0) {
            const char* why = "";
            const bool ok = ApplyBodyVel(player, g_postVel, &why);
            // P4 round 8 (run 15, tester: "Jumping up from hanging is now impossible"): the body's velocity alone does not keep the
            // rise - once the walk holds the body as in the air it runs its own vertical (run 15: 3.2 -> -0.2 m/s in one update the
            // moment its flags went 0x0001 -> 0x0002; every launch from a high hang lost it at once). The vertical goes where the
            // game's own velocity hand-over puts it: the walk's vertical speed (+0x260, WalkVertical), cleared when the carry ends.
            const uintptr_t wi = ok && g_carry.on && !g_carryAir ? WalkIface(player, &why) : 0;   // (only under the launch's carry,
            if (wi && WalkVertical(wi, true, g_postVel[1])) { g_vertSet = true; }                    // whose end clears it)
            if (ok && g_postStill) {   // round 18: the climb-up's rise handed over by its exit is cleared too
                const char* whyW = "";
                const uintptr_t ws = WalkIface(player, &whyW);
                if (ws) { WalkVertical(ws, false, 0.0f); }
            }
            if (ok) {
                --g_postLeft;
            } else if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_launch_min_mps  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] %s NOT applied: %s",
                    g_postStill ? "setting the body still after the cut-short climb-up" : "launch velocity after the drop", why);
            }
        } else if (g_postLeft == 0) {
            g_postOn = false;   // (the carry's line measures what the body did)
            g_postStill = false;
        }
    }

    climbhand::GameView v;
    unsigned char act3 = 0;
    v.egActive = RdB(eg + kActiveOff, &act3) && act3 != 0;
    RdI(eg + kEgStateOff, &v.egState);
    v.onFoot = onFoot;
    {   // round 19: a game move from the hang (shimmy / reach) carried the body - the pad thread moves the held ledge points with it
        float sh[3];
        const int mvState = g_mvShift.moveState;
        if (climbhand::MoveShiftStep(g_mvShift, v.egActive, v.egState, feet, haveFeet, sh)) {
            ++g_mvSeq;
            for (int i = 0; i < 3; ++i) { g_mvLast[i] = sh[i]; }
            if (HandLine()) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] GAME MOVE ended back in the hang (state %d -> %d): it carried the body (%.2f, %.2f, %.2f) m - the "
                    "held ledge points move with it (the hands re-anchor there, not on the old points)", mvState, v.egState, sh[0],
                    sh[1], sh[2]);
            }
        }
        v.moveSeq = g_mvSeq;
        for (int i = 0; i < 3; ++i) { v.moveShift[i] = g_mvLast[i]; }
    }
    v.pole = g_poleOn;   // P5 slide: the pad thread slides only on a ladder / pole, never below its bottom
    v.haveFeet = haveFeet;
    v.feetY = feet[1];
    v.havePoleLo = g_poleOn && g_pole.yHi > g_pole.yLo;
    v.poleLoY = g_pole.yLo;
    v.poleKind = g_poleOn && g_pole.yHi > g_pole.yLo ? g_pole.kind : -1;
    {   // P6 round 3: the zipline ride (the controller ServeZip named)
        uintptr_t zc = 0;
        unsigned char zr = 0;
        v.zipRiding = g_zipCtlOk && RdQ(player + kZipCtlSlot, &zc) && zc == g_zipCtlPtr && RdB(zc + 0x10, &zr) && zr != 0;
    }
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
    // climb_jump_by_look: the jump button in a hang (the input handler's 4 / 9, the stick not pushed up) is refused here and the
    // next Hook jumps toward the look (ours: the game's full jump, up or a push-off - climbhand_logic.h JumpButtonAsk).
    if (eg == g_egPtr && (state == 4 || state == 9)) {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const bool jumpCall = ret != 0 && (ret == g_jumpRet[0] || ret == g_jumpRet[1]);
        int from = -1;
        RdI(eg + kEgStateOff, &from);
        const ULONGLONG now = GetTickCount64(), fwdMs = g_stickFwdMs;
        const bool stickFwd = fwdMs != 0 && now >= fwdMs && now - fwdMs <= climbhand::kStickFreshMs;
        if (climbhand::JumpButtonAsk(ClimbByHand, ClimbJumpByLook, from, state, jumpCall, stickFwd)) {
            g_reqJumpMs = now;
            g_reqJumpFrom = from;
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
        const ULONGLONG fwdMs = g_stickFwdMs, pushMs = g_stickPushMs, grabMs = g_grabStartMs;
        // round 13: a push held over from before the grab (the run-up) does not count
        const bool stickHeldOver = fwdMs != 0 && now >= fwdMs && now - fwdMs <= climbhand::kStickFreshMs && pushMs < grabMs;
        const bool stickFwd = climbhand::StickClimbPush(now, fwdMs, pushMs, grabMs);
        const HeldVsGame s = ReadHeldVsGame(now);
        const bool fresh = g_retargetMs != 0 && now - g_retargetMs <= climbhand::kRetargetHoldMs;
        const int verdict = climbhand::GameMoveFromHang(ClimbByHand, ClimbStickShimmy, ClimbMantleStick, stickFwd, g_ourClimbUp,
                                                        s.held, fresh, from, state, ClimbSkipReach, g_poleOn);
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
            const bool reach = state == 5 || state == 9;
            if (reach) { InterlockedIncrement64(&g_wReachRefused); }
            const char* why = g_poleOn ? "the hang is on a ladder / pole (no real ledge: climb it hand over hand)"
                            : state == 8 ? (!ClimbMantleStick ? "climb_mantle_stick is off"
                                            : stickHeldOver ? "the stick is forward, but held from before this grab (let it back "
                                                              "and push it again to climb up)"
                                                            : "the stick is not pushed forward (the game climbing up by itself)")
                            : reach ? "the reach-up: no ledge above to move to and nothing to climb onto (climb_skip_reach: the hang "
                                      "stays)"
                                    : "climb_stick_shimmy is off (move along the ledge by hand)";
            const int whyId = verdict * 8 + (state == 8 ? 1 : 0) + (g_poleOn ? 4 : 0);
            if ((from != g_gmLastFrom || state != g_gmLastTo || whyId != g_gmLastWhy || now - g_gmLastMs > 3000) && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] the game's own %s from a hang (state %d -> %d) REFUSED - %s", what, from, state, why);
                g_gmLastFrom = from; g_gmLastTo = state; g_gmLastWhy = whyId; g_gmLastMs = now;
            }
            return;
        }
        // round 4 climb_room_check: the stick's climb-up from a hang only where the body fits (ours were checked before asking)
        if (state == 8 && !g_ourClimbUp && ClimbByHand && climbhand::IsHoldState(from) && !ClimbUpRoomOk(g_curPlayer, now, "the stick")) {
            InterlockedIncrement64(&g_wGameMoveRefused);
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

// P5 round 1: the player's group setter (+B9F220). The game's own ladder start - group 7 from one of its three ladder calls - is
// refused in hand mode (RefuseLadderStart): no LadderController, no ladder animation, no rungs; the ladder is grabbed by hand.
// Every other call passes straight through.
void __fastcall GroupHook(uintptr_t player, int group, bool flag, void* zero)
{
    if (group == 7 && ClimbByHand && ClimbLadderByHand) {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const bool site = ret != 0 && (ret == g_ladderRet[0] || ret == g_ladderRet[1] || ret == g_ladderRet[2]);
        if (climbhand::RefuseLadderStart(true, true, site && player == LocalPlayer(), group, site)) {
            InterlockedIncrement64(&g_wLadderRefused);
            const ULONGLONG now = GetTickCount64();
            if (now - g_ladderSaidMs > 5000 && Line()) {
                g_ladderSaidMs = now;
                // [PROBE: CLIMBHAND] gate=climb_ladder_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] the game's own ladder start (group 7, from gamedll+%llX: %s) REFUSED - climb_ladder_by_hand: grab it "
                    "by hand (one line per 5 s; the 5 s line counts them)", static_cast<unsigned long long>(ret - g_gd),
                    ret == g_ladderRet[0] ? "the use key" : "walking into it");
            }
            return;
        }
    }
    if (group == kGroupZipLine && ClimbByHand && ClimbZiplineByHand) {   // P6: the game's jump start onto a zipline
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const bool site = ret != 0 && (ret == g_zipJumpRet[0] || ret == g_zipJumpRet[1]);
        if (climbhand::RefuseZiplineJumpStart(true, true, g_zipList != 0, site && player == LocalPlayer(), group, site)) {
            InterlockedIncrement64(&g_wZipJumpRefused);
            const ULONGLONG now = GetTickCount64();
            if (now - g_zipJumpSaidMs > 5000 && Line()) {
                g_zipJumpSaidMs = now;
                // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] the game's own zipline start (%s, group 27 from gamedll+%llX) REFUSED - "
                    "climb_zipline_by_hand: grab the cable by hand (one line per 5 s; the 5 s line counts them)",
                    ret == g_zipJumpRet[0] ? kZipJumpSwitchName[0] : kZipJumpSwitchName[1],
                    static_cast<unsigned long long>(ret - g_gd));
            }
            return;
        }
    }
    g_trueGroup(player, group, flag, zero);
}

// Round 4: the game's zipline try-start (+BD3C60), skipped while ziplines are grabbed by hand (see kZipTryRva). Game thread.
void __fastcall ZipTryHook(uintptr_t player)
{
    if (climbhand::SkipZipTry(ClimbByHand, ClimbZiplineByHand, g_zipList != 0, player != 0 && player == LocalPlayer())) {
        uintptr_t hint = 0;
        if (RdQ(player + kZipHintSlot, &hint) && hint) {
            __try { *reinterpret_cast<unsigned char*>(hint + kZipHintValid) = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        InterlockedIncrement64(&g_wZipTrySkipped);
        return;
    }
    g_trueZipTry(player);
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
                 ast = InterlockedExchange64(&g_wAirStarts, 0), snp = InterlockedExchange64(&g_wCamSnaps, 0),
                 rch = InterlockedExchange64(&g_wReachRefused, 0), lad = InterlockedExchange64(&g_wLadderRefused, 0),
                 xpa = InterlockedExchange64(&g_wXpAsked, 0), xpf = InterlockedExchange64(&g_wXpFaults, 0),
                 jup = InterlockedExchange64(&g_wJumpsUp, 0), joff = InterlockedExchange64(&g_wJumpsOff, 0),
                 sld = InterlockedExchange64(&g_wSlidePolls, 0), zst = InterlockedExchange64(&g_wZipStarts, 0),
                 zrf = InterlockedExchange64(&g_wZipRefused, 0), zdr = InterlockedExchange64(&g_wZipDriven, 0),
                 zen = InterlockedExchange64(&g_wZipEnds, 0), zla = InterlockedExchange64(&g_wZipLaunches, 0),
                 zjr = InterlockedExchange64(&g_wZipJumpRefused, 0), gas = InterlockedExchange64(&g_wGrabAnimSkipped, 0),
                 gli = InterlockedExchange64(&g_wGlide, 0), zts = InterlockedExchange64(&g_wZipTrySkipped, 0),
                 rok = InterlockedExchange64(&g_wRoomOk, 0), rcr = InterlockedExchange64(&g_wRoomCrouch, 0),
                 rrf = InterlockedExchange64(&g_wRoomRefused, 0),
                 rpc = InterlockedExchange64(&g_wRopeCalls, 0), rpp = InterlockedExchange64(&g_wRopePulling, 0),
                 rpw = InterlockedExchange64(&g_wRopeWalls, 0), rps = InterlockedExchange64(&g_wRopeStarts, 0),
                 rpe = InterlockedExchange64(&g_wRopeEnds, 0), rpv = InterlockedExchange64(&g_wRopeVertFail, 0),
                 rpt = InterlockedExchange64(&g_wRopeTimeHeld, 0), rpl = InterlockedExchange64(&g_wRopeLookRefused, 0);
    const float vsMax = g_wVsMaxCm, snpMax = g_wCamSnapMaxCm;   // (written on the game thread, as this runs)
    g_wVsMaxCm = 0.0f;
    g_wCamSnapMaxCm = 0.0f;
    const float maxSent = g_wMaxSent, maxMeas = g_wMaxMeas;   // (game-thread floats, read on the game thread: the window runs there)
    g_wMaxSent = g_wMaxMeas = 0.0f;
    LONG64 w[climbhand::kWhyCount];
    for (int i = 0; i < climbhand::kWhyCount; ++i) { w[i] = InterlockedExchange64(&g_wWhy[i], 0); }
    if (st == 0 && dr == 0 && su == 0 && own == 0 && late == 0 && ref == 0 && flt == 0 && low == 0 && ovc == 0 && car == 0 &&
        vsc == 0 && ast == 0 && lad == 0 && xpa == 0 && xpf == 0 && jup == 0 && joff == 0 && sld == 0 && zst == 0 && zrf == 0 && zdr == 0 && zen == 0 && zjr == 0 &&
        gas == 0 && gli == 0 && rok == 0 && rrf == 0 && rpc == 0 && rps == 0 && rpe == 0) {
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
        "rules, or re-targeted; of them reaches with nothing above %lld, climb_skip_reach), re-targeted onto the held ledge %lld "
        "(refused %lld, did not hold %lld) | view smoothed on %lld "
        "camera call(s), largest offset from the eye bone %.1f cm (climb_view_smooth) | camera snaps against the body %lld "
        "(largest %.1f cm; CAM SNAP lines) | P5: the game's own ladder starts refused %lld, a pole / ladder held %d | P6: launch "
        "catches the game's climb award was asked to pay %lld (faulted %lld) | jump button from a hang (climb_jump_by_look, "
        "counted in launches too): straight up %lld, push-offs along the look %lld | pad polls sliding down a ladder / pole %lld "
        "(climb_slide_mps) | ziplines started by hand %lld (refused %lld), walk calls the hands drove on the cable %lld, let go %lld "
        "(launches %lld), the game's own mounts refused (jump or button) %lld | grab animations skipped %lld (climb_skip_grab_anim), "
        "walk calls gliding onto the grab point %lld (climb_grab_move_mps) | the game's zipline try-start skipped %lld update(s) | "
        "climb-up room tests (climb_room_check, one per ledge per 300 ms): fit %lld (of them crouched only %lld), refused %lld | "
        "P7 grappling rope (grapple_rope_swing): caught %lld, let go %lld, walk calls on the rope %lld (the trigger pulling %lld), "
        "sideways speed taken from a wall %lld, vertical speed calls refused %lld, the game's pull time limit held off %lld "
        "(grapple_hold_to_swing), the hook's view turn refused %lld",
        upd, skip, low, pass, su, casts, w[0], w[2], w[3], w[4], w[5], w[1], w[climbhand::kBackFace], w[climbhand::kBlocked],
        w[climbhand::kNoRoom], chr, st, hung, dr, ref, own, late, flt,
        static_cast<int>(g_m.phase), ClimbHandMotion ? 1 : 0, ovc, drv, maxSent, maxMeas, blk, one, two, rel, lref, man, stk, lau,
        ast, car, land, plain, gmr, rch, rtg, rtgf, rtgn, vsc, vsMax, snp, snpMax, lad, g_poleOn ? 1 : 0, xpa, xpf, jup, joff, sld, zst, zrf, zdr, zen, zla, zjr,
        gas, gli, zts, rok, rcr, rrf, rps, rpe, rpc, rpp, rpw, rpv, rpt, rpl);
}

// P5's ladder prompt: the LadderDI vtable once a trigger named it (constant for the class), and per HUD thread the last vtables that
// were NOT a LadderDI (so the RTTI name is read once per class and thread, not per slot update).
// P6: the same for the zipline's prompt triggers (FakeZipLineTrigger, ZipLine). Kinds: 0 other, 1 ladder, 2 zipline.
enum : int { kTrigOther = 0, kTrigLadder = 1, kTrigZip = 2 };
volatile uintptr_t g_ladderDiVt = 0, g_zipTrigVt[2] = {};
volatile LONG      g_ladderPromptSaid = 0, g_zipPromptSaid = 0;
thread_local uintptr_t t_notLadderVt[16] = {};
thread_local int       t_notLadderN = 0;

int TriggerKind(uintptr_t trig)
{
    uintptr_t vt = 0;
    if (!RdQ(trig, &vt) || !vt) { return kTrigOther; }
    if (vt == g_ladderDiVt) { return kTrigLadder; }
    if (vt == g_zipTrigVt[0] || vt == g_zipTrigVt[1]) { return kTrigZip; }
    for (int i = 0; i < 16; ++i) { if (t_notLadderVt[i] == vt) { return kTrigOther; } }
    char cls[48] = {};
    const bool named = RttiClassNameOf(trig, cls, sizeof(cls));
    if (named && std::strcmp(cls, "LadderDI") == 0) { g_ladderDiVt = vt; return kTrigLadder; }
    if (named && std::strcmp(cls, "FakeZipLineTrigger") == 0) { g_zipTrigVt[0] = vt; return kTrigZip; }
    if (named && std::strcmp(cls, "ZipLine") == 0) { g_zipTrigVt[1] = vt; return kTrigZip; }
    t_notLadderVt[t_notLadderN] = vt;
    t_notLadderN = (t_notLadderN + 1) % 16;
    return kTrigOther;
}

}  // namespace

bool ClimbHandHidesPrompt(void* trigger)
{
    if (!trigger || !ClimbByHand) { return false; }   // (cheap tests first)
    const bool ladderOn = ClimbLadderByHand && g_groupLive == 1, zipOn = ClimbZiplineByHand && g_zipList != 0;
    if (!ladderOn && !zipOn) { return false; }
    const int kind = TriggerKind(reinterpret_cast<uintptr_t>(trigger));
    const bool hideLadder = climbhand::HideLadderPrompt(ClimbByHand, ClimbLadderByHand, g_groupLive == 1, kind == kTrigLadder);
    const bool hideZip = climbhand::HideZiplinePrompt(ClimbByHand, ClimbZiplineByHand, g_zipList != 0, kind == kTrigZip);
    if (hideLadder && InterlockedExchange(&g_ladderPromptSaid, 1) == 0 && Line()) {
        // [PROBE: CLIMBHAND] gate=climb_ladder_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] LADDER PROMPT hidden - the HUD's use prompt for ladder / pipe / rope %p (LadderDI) is updated with no "
            "trigger while climb_ladder_by_hand refuses the game's own ladder climb (logged once)", trigger);
    }
    if (hideZip && InterlockedExchange(&g_zipPromptSaid, 1) == 0 && Line()) {
        uintptr_t vt = 0;
        RdQ(reinterpret_cast<uintptr_t>(trigger), &vt);
        // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] ZIPLINE PROMPT hidden - the HUD's prompt for zipline trigger %p (%s) is updated with no trigger while "
            "climb_zipline_by_hand grabs ziplines by hand (logged once)", trigger,
            vt && vt == g_zipTrigVt[0] ? "FakeZipLineTrigger" : "ZipLine");
    }
    return hideLadder || hideZip;
}

// P7: the walk call is where the rope catches (GrappleRopeWalk). If the game's rope sits in its pull and no walk call has caught it
// for kRopeMissMs, the pull is moving the player some other way - said once per throw, so a run where the rope never takes over is
// not silent. Game thread (ClimbHandTick).
static uintptr_t RopeCtl(uintptr_t player);   // (with GrappleRopeWalk below)
static uintptr_t __fastcall RopeLookHook(uintptr_t self);   // round 5 (with RopeCtl below)
static int       g_ropeTickState = -1;
static ULONGLONG g_ropeTickPullMs = 0;
static bool      g_ropeTickSaid = false;
constexpr ULONGLONG kRopeMissMs = 300;
static void RopeTick(ULONGLONG now)
{
    if (!GrappleRopeSwing || !g_gd) { return; }
    const uintptr_t player = LocalPlayer();
    const uintptr_t ctl = player ? RopeCtl(player) : 0;
    int state = -1;
    if (ctl) { RdI(ctl + kRopeStateOff, &state); }
    if (state == kRopeStatePull && g_ropeTickState != kRopeStatePull) { g_ropeTickPullMs = now; g_ropeTickSaid = false; }
    g_ropeTickState = state;
    if (state != kRopeStatePull || g_rope.on || g_ropeTickSaid || now - g_ropeTickPullMs < kRopeMissMs) { return; }
    g_ropeTickSaid = true;
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=grapple_rope_swing  site=HEADSET  log-only=yes
        LOG("[CLIMBHAND] ROPE NOT CAUGHT - the game's rope has been in its pull (state 3) for %llu ms and no walk call took it (the "
            "last walk call saw the rope's state %d) - the game's own pull moves the player this time",
            static_cast<unsigned long long>(now - g_ropeTickPullMs), g_ropePrevState);
    }
}

void ClimbHandTick()
{
    if (g_live == 1 && ClimbByHand) { Window(GetTickCount64()); }
    if (g_installed && ClimbByHand) { RopeTick(GetTickCount64()); }
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
    for (int k = 0; k < 2; ++k) {   // climb_jump_by_look: the hang's jump-button asks must be "call +D0CD20" (E8 rel32)
        const uintptr_t site = g_gd + DLVR_CH_GD(kJumpCallRva[k]);
        unsigned char op = 0;
        int rel = 0;
        const bool callOk = RdB(site, &op) && op == 0xE8 && RdI(site + 1, &rel) &&
                            site + 5 + static_cast<intptr_t>(rel) == g_gd + DLVR_CH_GD(kSetStateRva);
        g_jumpRet[k] = callOk ? site + 5 : 0;
        // [PROBE: CLIMBHAND] gate=climb_by_hand (install)  site=FLAT  log-only=no
        LOG("[CLIMBHAND] the hang's jump-button call (state %d) gamedll+%llX %s", k == 0 ? 4 : 9,
            static_cast<unsigned long long>(kJumpCallRva[k]),
            callOk ? "is 'call set-state' - climb_jump_by_look replaces it with a jump toward the look"
                   : "*** is NOT 'call set-state' - that jump stays the game's own ***");
    }
    const bool groupOk = BytesMatch(setGroup, kSetGroupBytes, sizeof(kSetGroupBytes));
    const bool stateOk = BytesMatch(setState, kSetStateBytes, sizeof(kSetStateBytes));
    const bool targetOk = BytesMatch(setTarget, kSetTargetBytes, sizeof(kSetTargetBytes));
    {   // P4 round 14: the grab's own hang-point refresh, called after a re-target. Optional: unmatched = re-targets leave it stale.
        const uintptr_t hangPoint = g_gd + DLVR_CH_GD(kHangPointRva);
        const bool hpOk = BytesMatch(hangPoint, kHangPointBytes, sizeof(kHangPointBytes));
        g_hangPoint = hpOk ? reinterpret_cast<HangPointFn>(hangPoint) : nullptr;
        // [PROBE: CLIMBHAND] gate=climb_by_hand (install)  site=FLAT  log-only=no
        LOG("[CLIMBHAND] the ledge grab's hang-point refresh gamedll+%llX %s", static_cast<unsigned long long>(kHangPointRva),
            hpOk ? "bytes OK - a re-target also moves the grab's own copy of the ledge (its stick-up starts from it)"
                 : "*** bytes do NOT match - a re-target leaves the grab's own copy of the ledge stale (its stick-up may start from an "
                   "older ledge) ***");
    }
    {   // P6: the game's climb award, called for a launch caught by a new grab. Optional: unmatched = such catches pay no XP.
        const uintptr_t climbXp = g_gd + DLVR_CH_GD(kClimbXpRva);
        const bool xpOk = BytesMatch(climbXp, kClimbXpBytes, sizeof(kClimbXpBytes));
        g_climbXp = xpOk ? reinterpret_cast<ClimbXpFn>(climbXp) : nullptr;
        // [PROBE: CLIMBHAND] gate=climb_by_hand (install)  site=FLAT  log-only=no
        LOG("[CLIMBHAND] the game's climb XP award gamedll+%llX %s", static_cast<unsigned long long>(kClimbXpRva),
            xpOk ? "bytes OK - a launch caught by a new grab pays climb type 5 (Jump180); climb-ups pay through the game's own "
                   "set-state as always"
                 : "*** bytes do NOT match - a launch caught by a new grab pays no XP (climb-ups still pay: that is the game's own) ***");
    }
    {   // P6 ziplines by hand: the list's address from the game's own compare, the name assign by its bytes, the renderable filter
        const uintptr_t cmpAt = g_gd + DLVR_CH_GD(kZipCountCmpRva), nameAt = g_gd + DLVR_CH_GD(kZipNameAssignRva);
        unsigned char op0 = 0, op1 = 0, imm = 0xFF;
        int disp = 0;
        const bool cmpOk = RdB(cmpAt, &op0) && RdB(cmpAt + 1, &op1) && RdI(cmpAt + 2, &disp) && RdB(cmpAt + 6, &imm) && op0 == 0x83 &&
                           op1 == 0x3D && imm == 0;
        const bool nameOk = BytesMatch(nameAt, kZipNameAssignBytes, sizeof(kZipNameAssignBytes));
        g_zipRenderable = reinterpret_cast<RenderableFn>(GetProcAddress(en, kRenderableExport));
        g_zipList = cmpOk && nameOk ? cmpAt + 7 + static_cast<intptr_t>(disp) - 8 : 0;   // the count's address - 8 = the array
        g_zipName = g_zipList ? reinterpret_cast<NameAssignFn>(nameAt) : nullptr;
        // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand (install)  site=FLAT  log-only=no
        LOG("[CLIMBHAND] ziplines by hand: the line list %s (gamedll+%llX's compare %s), the name assign gamedll+%llX %s, "
            "IModelObject::IsRenderable %s -> %s", g_zipList ? "found" : "NOT found", static_cast<unsigned long long>(kZipCountCmpRva),
            cmpOk ? "checks out" : "*** does not match ***", static_cast<unsigned long long>(kZipNameAssignRva),
            nameOk ? "bytes OK" : "*** bytes do NOT match ***", g_zipRenderable ? "found" : "MISSING (hidden lines are not filtered)",
            g_zipList ? "a grip near a zipline rides it" : "*** OFF this session - ziplines only the game's way ***");
        if (g_zipList) {
            LOG("[CLIMBHAND]   the zipline list at gamedll+%llX", static_cast<unsigned long long>(g_zipList - g_gd));
        }
        for (int k = 0; k < 2; ++k) {   // each mount site must be "call +B9F220" for its return address to name it in GroupHook
            const uintptr_t site = g_gd + DLVR_CH_GD(kZipJumpSwitchRva[k]);
            unsigned char op = 0;
            int rel = 0;
            const bool callOk = g_zipList && RdB(site, &op) && op == 0xE8 && RdI(site + 1, &rel) &&
                                site + 5 + static_cast<intptr_t>(rel) == g_gd + DLVR_CH_GD(kSetGroupRva);
            g_zipJumpRet[k] = callOk ? site + 5 : 0;
            // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand (install)  site=FLAT  log-only=no
            LOG("[CLIMBHAND] the game's zipline start #%d (%s) gamedll+%llX %s", k + 1, kZipJumpSwitchName[k],
                static_cast<unsigned long long>(kZipJumpSwitchRva[k]),
                callOk ? "is 'call the group setter' - refused while ziplines are grabbed by hand (needs the group gate below)"
                       : "*** is NOT 'call the group setter' (or no ziplines by hand) - this way still mounts the zipline ***");
        }
        {   // round 4: the game's whole zipline try-start, skipped by hand (no mount, no stale jump after a refused mount)
            int tryAttempts = 0;
            const bool tryOk = g_zipList && InstallSingleGameDetour(g_gd + kZipTryRva, kZipTryBytes, sizeof(kZipTryBytes),
                                                                    reinterpret_cast<void*>(&ZipTryHook),
                                                                    reinterpret_cast<void**>(&g_trueZipTry), "climbhand-ziptry",
                                                                    &tryAttempts);
            InterlockedExchange(&g_zipTryLive, tryOk ? 1 : -1);
            // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand (install)  site=FLAT  log-only=no
            LOG("[CLIMBHAND] the game's zipline try-start gamedll+%llX %s (%d attempt(s))", static_cast<unsigned long long>(kZipTryRva),
                tryOk ? "detoured - skipped while ziplines are grabbed by hand: no mount by jump or button, no prompt hint"
                      : (g_zipList ? "*** NOT detoured (bytes differ?) - only the group refusals stand: a refused button mount may "
                                     "still jump the player toward a stale point ***"
                                   : "not detoured (no ziplines by hand this session)"),
                tryAttempts);
        }
        const uintptr_t leaveAt = g_gd + DLVR_CH_GD(kLeaveGroupRva);
        const bool leaveOk = g_zipList && BytesMatch(leaveAt, kLeaveGroupBytes, sizeof(kLeaveGroupBytes));
        g_leaveGroup = leaveOk ? reinterpret_cast<LeaveGroupFn>(leaveAt) : nullptr;
        // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand (install)  site=FLAT  log-only=no
        LOG("[CLIMBHAND] hands on the zipline: the leave-group call gamedll+%llX %s", static_cast<unsigned long long>(kLeaveGroupRva),
            leaveOk ? "bytes OK - the hands hold the cable during the ride, letting go drops or launches off it"
                    : (g_zipList ? "*** bytes do NOT match - a hand grab starts the ride, but the ride is the game's alone ***"
                                 : "not checked (no ziplines by hand this session)"));
    }
    {   // P7 grapple_rope_swing: the rope's exit (leave group 24), checked on its own (the zipline's pointer needs the line list)
        const uintptr_t leaveAt = g_gd + DLVR_CH_GD(kLeaveGroupRva);
        const bool leaveOk = BytesMatch(leaveAt, kLeaveGroupBytes, sizeof(kLeaveGroupBytes));
        g_ropeLeave = leaveOk ? reinterpret_cast<LeaveGroupFn>(leaveAt) : nullptr;
        // round 4: the pull's end-block branch (kRopeEndJccRva); both forms must fit one aligned qword (offset 0..2 in it)
        const uintptr_t endAt = g_gd + DLVR_CH_GD(kRopeEndJccRva);
        const bool endBytes = BytesMatch(endAt, kRopeEndJcc, sizeof(kRopeEndJcc));
        const bool endFits = (endAt & 7) <= 2;
        g_ropeEndAt = endBytes && endFits ? endAt : 0;
        // round 5: the rope's look-interface getter, swapped once (its old value must be +D7E160)
        const char* lookWhy = nullptr;
        {
            void** slot = reinterpret_cast<void**>(g_gd + DLVR_CH_GD(kRopeCtlVtRva) + 8 * kRopeLookSlot);
            const uintptr_t expect = g_gd + DLVR_CH_GD(kRopeLookFnRva);
            uintptr_t cur = 0;
            DWORD old = 0;
            if (RdQ(reinterpret_cast<uintptr_t>(slot), &cur) && cur == reinterpret_cast<uintptr_t>(&RopeLookHook) && g_trueRopeLook) {
                // (already swapped by an earlier install)
            } else if (cur != expect) {
                lookWhy = "*** slot 28 does not hold the expected getter - the hook's view turn stays the game's ***";
            } else if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                lookWhy = "*** VirtualProtect on the vtable slot failed - the hook's view turn stays the game's ***";
            } else {
                g_trueRopeLook = reinterpret_cast<RopeLookFn>(cur);   // valid before the swap goes live
                InterlockedExchangePointer(slot, reinterpret_cast<void*>(&RopeLookHook));
                VirtualProtect(slot, sizeof(void*), old, &old);
            }
        }
        // [PROBE: CLIMBHAND] gate=grapple_rope_swing (install)  site=FLAT  log-only=no
        LOG("[CLIMBHAND] grappling hook's look: vtable gamedll+%llX slot %llu %s", static_cast<unsigned long long>(kRopeCtlVtRva),
            static_cast<unsigned long long>(kRopeLookSlot),
            lookWhy ? lookWhy : "swapped - with grapple_rope_swing the hook no longer turns the view toward its point");
        // [PROBE: CLIMBHAND] gate=grapple_rope_swing (install)  site=FLAT  log-only=no
        LOG("[CLIMBHAND] grappling hook as a rope: the rope controller is checked by its vtable gamedll+%llX at PlayerDI+%llu; letting "
            "go %s | the pull's own end (reaching the hook, a contact, the time limit) at gamedll+%llX: %s",
            static_cast<unsigned long long>(kRopeCtlVtRva), static_cast<unsigned long long>(kRopeCtlSlot),
            leaveOk ? "leaves its group 24 (the game's own exit call, bytes OK)"
                    : "*** cannot leave its group (the leave-group bytes do NOT match) - the game's rope ends itself ***",
            static_cast<unsigned long long>(kRopeEndJccRva),
            g_ropeEndAt ? "bytes OK - skipped while a held rope holds (grapple_hold_to_swing)"
                        : (!endBytes ? "*** bytes do NOT match - the game's rope may still end itself under a held rope ***"
                                     : "*** not in one aligned qword - left alone, the game's rope may end itself under a held rope ***"));
    }
    g_ray =reinterpret_cast<RayFn>(GetProcAddress(en, kRayExport));
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
    // P5 round 1: the ladder gate - the group setter detoured (its first 14 bytes are whole instructions, kSetGroupBytes), the
    // three ladder calls checked to be "call +B9F220". The exports for the climbable's line are needed for the grab, not the gate.
    g_fwdFn = reinterpret_cast<PosFn>(GetProcAddress(en, kFwdExport));
    g_extFn = reinterpret_cast<ExtFn>(GetProcAddress(en, kExtWorldExport));
    int sitesOk = 0;
    for (int i = 0; i < 3; ++i) {
        const uintptr_t site = g_gd + DLVR_CH_GD(kLadderSwitchRva[i]);
        unsigned char op = 0;
        int rel = 0;
        const bool callOk = RdB(site, &op) && op == 0xE8 && RdI(site + 1, &rel) &&
                            site + 5 + static_cast<intptr_t>(rel) == setGroup;
        g_ladderRet[i] = callOk ? site + 5 : 0;
        if (callOk) { ++sitesOk; }
    }
    bool groupHookOk = false;
    int groupAttempts = 0;
    if (ok && sitesOk > 0 && g_fwdFn && g_extFn) {   // no gate without the grab that replaces the game's ladder
        groupHookOk = InstallSingleGameDetour(g_gd + kSetGroupRva, kSetGroupBytes, sizeof(kSetGroupBytes),
                                              reinterpret_cast<void*>(&GroupHook), reinterpret_cast<void**>(&g_trueGroup),
                                              "climbhand-ladder", &groupAttempts);
        if (groupHookOk) { g_setGroup = g_trueGroup; }   // our own group switches skip the gate
    }
    InterlockedExchange(&g_groupLive, groupHookOk ? 1 : -1);
    // [PROBE: CLIMBHAND] gate=climb_ladder_by_hand (install)  site=FLAT  log-only=no
    LOG("[CLIMBHAND] P5 ladders by hand: the game's ladder start calls %d of 3 checked (use key %s, walk-in %s / %s) | gate (group "
        "setter detour gamedll+%llX) %s (%d attempt(s)) | GetForwardVector %s, GetExtentsInWorld %s%s", sitesOk,
        g_ladderRet[0] ? "ok" : "NOT", g_ladderRet[1] ? "ok" : "NOT", g_ladderRet[2] ? "ok" : "NOT",
        static_cast<unsigned long long>(kSetGroupRva), groupHookOk ? "committed" : "*** NOT LIVE ***", groupAttempts,
        g_fwdFn ? "resolved" : "MISSING", g_extFn ? "resolved" : "MISSING",
        groupHookOk ? "" : " - the game's own ladder still starts (walking into a ladder climbs it the game's way)");
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

bool ClimbHandBodyHeld()
{
    if (!ClimbByHand || g_live != 1 || !g_egOk) { return false; }
    return climbhand::BodyHoldTail(ClimbHandHanging(), g_grabEndMs, GetTickCount64(), g_grabEndClimbUp);
}

float ClimbHandStandRef(float y)
{
    static climbhand::StandRef s_ref;
    static bool s_inDip = false;
    bool refused = false;
    const float before = s_ref.y;
    const bool had = s_ref.have;
    const float out = climbhand::StandRefStep(s_ref, y, GetTickCount64(), &refused);
    if (refused) {
        if (!s_inDip && Line()) {
            // [PROBE: CLIMBHAND] gate=climb_view_rigid  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] STANDING EYE HEIGHT: a lower value %.1f cm refused (remembered %.1f cm; taken only if it holds %llu s) - "
                "the climbing view keeps the standing height", 100.0f * y, 100.0f * s_ref.y,
                static_cast<unsigned long long>(climbhand::kStandLowMs / 1000));
        }
        s_inDip = true;
    } else {
        if (s_inDip && had && out < before - climbhand::kStandDipM && Line()) {
            // [PROBE: CLIMBHAND] gate=climb_view_rigid  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] STANDING EYE HEIGHT lowered %.1f -> %.1f cm (the lower value held %llu s)", 100.0f * before, 100.0f * out,
                static_cast<unsigned long long>(climbhand::kStandLowMs / 1000));
        }
        s_inDip = false;
    }
    return out;
}

float ClimbViewRigidK(float animGapM)
{
    static float s_k = 0.0f;
    static LARGE_INTEGER s_q = {};
    static volatile LONG s_said = 0;
    static bool s_tail = false;
    static float s_tailMaxGap = 0.0f;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
    const float dt = s_q.QuadPart ? static_cast<float>(static_cast<double>(q.QuadPart - s_q.QuadPart) / g_qpf.QuadPart) : 0.0f;
    s_q = q;
    const bool hanging = ClimbHandHanging();
    const ULONGLONG now = GetTickCount64(), endMs = g_grabEndMs;
    static climbhand::ViewTail s_vt;   // round 6: ends once per grab end, or when the game's head has settled
    const bool active = ClimbViewRigid && g_live == 1 && climbhand::ViewHoldTail(s_vt, hanging, endMs, now, animGapM);
    // round 4: the tail after the grab (the game's clip still blending out under the view) - one line when it ends
    const bool tailNow = active && !hanging;
    if (tailNow) {
        if (!s_tail) { s_tailMaxGap = 0.0f; }
        if (animGapM == animGapM && animGapM > s_tailMaxGap) { s_tailMaxGap = animGapM; }
    }
    if (s_tail && !tailNow && !hanging && Line()) {
        // [PROBE: CLIMBHAND] gate=climb_view_rigid  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] RIGID VIEW TAIL ended %llu ms after the %s: %s (the game's head was up to %.1f cm off the standing eye point "
            "in the tail; it is %.1f cm off now) - the view eases back to the game's head over %.0f ms",
            static_cast<unsigned long long>(endMs && now >= endMs ? now - endMs : 0), g_grabEndClimbUp ? "climb-up" : "grab",
            now - endMs >= climbhand::kViewTailMaxMs ? "TIMED OUT (the animation never came back within 8 cm)"
            : animGapM <= climbhand::kViewTailGapM ? "the game's head is back near the standing eye point"
                                                   : "the game's head has SETTLED (its height still for 200 ms)",
            static_cast<double>(s_tailMaxGap * 100.0f), static_cast<double>(animGapM * 100.0f), climbhand::kRigidRampS * 1000.0f);
    }
    s_tail = tailNow;
    s_k = climbhand::RigidRampStep(s_k, active, dt);
    if (active && InterlockedCompareExchange(&s_said, 1, 0) == 0 && Line()) {
        // [PROBE: CLIMBHAND] gate=climb_view_rigid  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] RIGID VIEW first used (logged once) - while climbing the camera keeps the standing eye point on the body "
            "(eased in / out over %.0f ms); the climb animations no longer move the view", climbhand::kRigidRampS * 1000.0f);
    }
    return s_k;
}

// P4 round 15 (climbhand_logic.h OverLedgeM). The ledge top = the game's ledge point (player+0x14C8: the held ledge after a re-target).
volatile float     g_overLedgeM = 0.0f;
volatile ULONGLONG g_overLedgeMs = 0;
climbhand::OverLedgeState g_ol;
float     g_olMaxM = 0.0f, g_olStartY = 0.0f, g_olEndM = 0.0f;   // this climb-up's largest lift / start / lift at the state's end
ULONGLONG g_olEndMs = 0;
LONG64    g_olClimbUps = 0;
float ClimbHandOverLedgeM(float originY)
{
    int state = -1;
    unsigned char act = 0;
    const bool active = ClimbByHand && g_live == 1 && g_egOk && g_egPtr && RdB(g_egPtr + kActiveOff, &act) && act != 0 &&
                        RdI(g_egPtr + kEgStateOff, &state);
    float mid[3] = { NAN, NAN, NAN };
    if (active && (state == 8 || state == 10) && !g_ol.climbing && g_curPlayer) { RdF3(g_curPlayer + kHookMid, mid); }
    const ULONGLONG now = GetTickCount64();
    int ev = climbhand::kOlNone;
    const float m = climbhand::OverLedgeStep(g_ol, active, active ? state : -1, originY, mid[1], now, &ev);
    if (ev == climbhand::kOlStarted) { g_olMaxM = 0.0f; g_olStartY = originY; }
    if (ev == climbhand::kOlClimbEnded || (ev == climbhand::kOlTailEnded && g_olEndMs == 0)) {
        g_olEndM = climbhand::OverLedgeM(true, originY, g_ol.lipY);
        g_olEndMs = now;
    }
    if (m > g_olMaxM) { g_olMaxM = m; }
    if (ev == climbhand::kOlTailEnded) {
        ++g_olClimbUps;
        if (g_olClimbUps <= 40 && Line()) {
            // [PROBE: CLIMBHAND] gate=climb_view_rigid,climb_hold_body  site=HEADSET  log-only=no
            char how[160];
            if (g_ol.fadeFromM <= 0.0f) {
                strcpy_s(how, "it came down to the ledge point");
            } else {
                sprintf_s(how, "%s with %.1f cm left over the ledge point, eased out over %llu ms",
                          g_ol.settled ? "it SETTLED (stood on a floor that much higher)" : "tail TIMED OUT", 100.0f * g_ol.fadeFromM,
                          static_cast<unsigned long long>(climbhand::kOverLedgeFadeMs));
            }
            LOG("[CLIMBHAND] CLIMB-UP #%lld done - from a hang, the body's origin rose to %.1f cm over the ledge top (%.2f; it started "
                "%.1f cm below it); when the climb-up state ended it was still %.1f cm over; the lowering ended %llu ms later (%s). %s",
                static_cast<long long>(g_olClimbUps), 100.0f * g_olMaxM, g_ol.lipY, 100.0f * (g_ol.lipY - g_olStartY),
                100.0f * g_olEndM, static_cast<unsigned long long>(now - g_olEndMs), how, g_olMaxM > 0.0f
                    ? "The view (and the held root, while the grab ran) were lowered by the lift throughout."
                    : "Never above it, nothing lowered.");
        }
        g_olEndMs = 0;
    }
    g_overLedgeM = m;
    g_overLedgeMs = GetTickCount64();
    return m;
}
float ClimbHandOverLedgeLast()
{
    const ULONGLONG at = g_overLedgeMs;
    return at != 0 && GetTickCount64() - at <= 100 ? g_overLedgeM : 0.0f;
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
// P4 round 15 (tester 2026-11-27: "mantles seem to always leave me in the air a good half a meter above the floor over the ledge";
// run 18's climb-ups #7 / #9 / #13 end with the body's origin on the lip's height and the walk not airborne, and the CAM lines stop
// with the grab). For kCamTailMs after a grab ends: a CAM AFTER line every kCamEveryMs - the camera's and the origin's height over
// the floor straight below (one engine ray from 0.5 m above the origin down kCamFloorRayM) and the walk's flags (bit1 = airborne).
constexpr ULONGLONG kCamTailMs   = 2500;
constexpr float     kCamFloorRayM = 4.0f;
ULONGLONG      g_camEndMs = 0;
void CamAfterLine(const ClimbCamSample& s, ULONGLONG now)
{
    const uintptr_t player = g_curPlayer;
    float floorY = 0.0f;
    bool floorOk = false;
    if (player && g_ray) {
        Caster cast{ player };
        const float from[3] = { s.originW[0], s.originW[1] + 0.5f, s.originW[2] };
        const float to[3] = { s.originW[0], s.originW[1] + 0.5f - kCamFloorRayM, s.originW[2] };
        float hit[3] = {};
        floorOk = cast(from, to, hit) && !cast.fault;
        floorY = hit[1];
    }
    const char* why = "";
    const uintptr_t wi = player ? WalkIface(player, &why) : 0;
    char fl[48];
    if (wi) { std::snprintf(fl, sizeof(fl), "walk flags 0x%04X", static_cast<unsigned>(WalkFlags(wi))); }
    else    { std::snprintf(fl, sizeof(fl), "walk unread"); }
    char fs[96];
    if (floorOk) {
        std::snprintf(fs, sizeof(fs), "camera base %.1f cm over the floor below, body origin %.1f cm", 100.0f * (s.camW[1] - floorY),
                      100.0f * (s.originW[1] - floorY));
    } else {
        std::snprintf(fs, sizeof(fs), "no floor within %.1f m below", kCamFloorRayM - 0.5f);
    }
    // [PROBE: CLIMBHAND] gate=climb_by_hand  site=HEADSET  log-only=yes
    LOG("[CLIMBHAND] CAM AFTER +%llu ms | %s | camera - body origin (%.1f, %.1f, %.1f) cm | game's base (body-local) (%.1f, %.1f, %.1f) "
        "| eye bone - origin (%.1f, %.1f, %.1f) | origin (%.2f, %.2f, %.2f) | %s", static_cast<unsigned long long>(now - g_camEndMs), fs,
        100.0f * (s.camW[0] - s.originW[0]), 100.0f * (s.camW[1] - s.originW[1]), 100.0f * (s.camW[2] - s.originW[2]),
        100.0f * s.baseL[0], 100.0f * s.baseL[1], 100.0f * s.baseL[2], 100.0f * (s.eyeW[0] - s.originW[0]),
        100.0f * (s.eyeW[1] - s.originW[1]), 100.0f * (s.eyeW[2] - s.originW[2]), s.originW[0], s.originW[1], s.originW[2], fl);
}

void ClimbHandCamTrace(const ClimbCamSample& s)
{
    unsigned char act = 0;
    const bool active = ClimbByHand && g_live == 1 && g_egOk && RdB(g_egPtr + kActiveOff, &act) && act != 0;
    if (!active) {
        const ULONGLONG t = GetTickCount64();
        if (g_camHave) { g_camEndMs = t; g_camEveryMs = 0; }   // the grab just ended: the tail starts
        g_camHave = false;
        if (g_camEndMs && t - g_camEndMs <= kCamTailMs && t - g_camEveryMs >= kCamEveryMs &&
            InterlockedIncrement(&g_camLines) <= kMaxCamLines) {
            g_camEveryMs = t;
            CamAfterLine(s, t);
        }
        return;
    }
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
    // round 16: the push's k is climb_air_control_pct (the game's own is kGameAirK, 1.06 %)
    const float liveK = WalkAirK(wi);
    const float airK = ClimbAirControlPct * 0.01f;
    const int end = climbhand::CarryStep(g_carry, feet, dt, now, in, ClimbAirMomentum, ClimbAirDrag, out, 1.0f, airK, air);
    if (g_traceOn && now - t0 <= (g_carryAir ? kTraceAirMs : kTraceLaunchMs) && InterlockedIncrement(&g_traceLines) <= kMaxTraceLines) {
        float mv[3] = { NAN, NAN, NAN };
        if (hadPrev && dt > 1e-4f) { for (int i = 0; i < 3; ++i) { mv[i] = (feet[i] - prevFeet[i]) / dt; } }
        const float ball = g_carry.v[1] - climbhand::kGravity * static_cast<float>(now - t0) / 1000.0f;
        // [PROBE: CLIMBHAND] gate=climb_air_momentum  site=HEADSET  log-only=yes
        LOG("[CLIMBHAND] CARRY TRACE %s t+%llu | walk flags 0x%04X (air %d) | feet vy %.2f (ballistic %.2f) | horizontal handed "
            "(%.2f, %.2f) measured (%.2f, %.2f) stick (%.2f, %.2f) | air push %.1f %% of the gap per update (climb_air_control_pct; "
            "the game's own %.2f %%, the walk's own factor^3 reads %.4f, unused) | %s", g_carryAir ? "AIR" : "LAUNCH",
            static_cast<unsigned long long>(now - t0), WalkFlags(wi), air, mv[1], ball, out[0], out[2], mv[0], mv[2], in[0], in[2],
            100.0f * airK, 100.0f * climbhand::kGameAirK, liveK,
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

// ---------------------------------------------------------------- P7: the grappling hook as a rope (grapple_rope_swing)
// Design §8.21. The game's rope (RopeLocomotionController) still throws the hook, catches it and draws the rope; from the update its
// state reaches the pull (3), the walk call is sent the rope's velocity instead (grapplerope_logic.h RopeStep): your momentum,
// gravity, the rope's length, and the trigger's pull. The vertical part goes through the walk's own vertical speed (+0x260, the
// channel a launch's rise uses), steered by what the feet really did (g_ropeLift): the walk's own fall is not ours to set, so the
// lift is raised while the body falls faster than the rope allows and drops to zero when it does not. Game thread.
static float g_ropeLift = 0.0f;             // the walk's vertical speed the rope asks for (>= 0; 0 = cleared)
constexpr float kRopeLiftK = 0.6f;          // per walk call: the share of (wanted - measured) vertical speed added to the lift
constexpr float kRopeLiftMax = 30.0f;

static uintptr_t RopeCtl(uintptr_t player)
{
    uintptr_t ctl = 0, vt = 0;
    if (!RdQ(player + kRopeCtlSlot, &ctl) || !ctl || !RdQ(ctl, &vt) || vt != g_gd + DLVR_CH_GD(kRopeCtlVtRva)) { return 0; }
    return ctl;
}

// Round 5: the rope's look-interface getter (kRopeLookSlot). Game thread (the look selector, every player update).
static uintptr_t __fastcall RopeLookHook(uintptr_t self)
{
    const uintptr_t r = g_trueRopeLook(self);
    const bool on = GrappleRopeSwing && ClimbByHand;
    if (!r || !on) { return r; }
    const uintptr_t lp = LocalPlayer();
    // another player's rope (co-op) keeps the game's look
    if (grapplerope::RopeTakesLook(on, lp != 0 && RopeCtl(lp) == self)) { return r; }
    if (InterlockedIncrement64(&g_wRopeLookRefused) == 1 && Line()) {
        // [PROBE: CLIMBHAND] gate=grapple_rope_swing  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] the grappling hook's own look (it turns the view toward the hook) REFUSED - grapple_rope_swing: the view stays "
            "the headset's (logged once; the 5 s line counts the refused updates)");
    }
    return 0;
}

static const char* RopeEndName(int e)
{
    static const char* const k[] = { "-", "the jump button", "landed after air time", "reached the hook", "the safety time out",
                                     "the grappling trigger released",
                                     "*** the game's rope left its pull first (the rope cannot hold the body without it) ***" };
    static_assert(sizeof(k) / sizeof(k[0]) == grapplerope::kReCount, "one name per RopeEnd");
    return e >= 0 && e < grapplerope::kReCount ? k[e] : "?";
}

// The momentum handed to the walk: the rope's vertical speed cleared, then like a launch's (the post path's SetLinearVel and the air
// carry). Round 2: called once MoveController has the walk again (the free flight's end), so the carry is not ended by the rope
// controller's last update. what = for the log.
static void RopeHandOff(uintptr_t player, const float feet[3], bool haveFeet, ULONGLONG now, const char* what)
{
    const float v[3] = { g_rope.v[0], g_rope.v[1], g_rope.v[2] };
    const ULONGLONG freeMs = now - g_ropeFreeT0;
    g_ropeFree = false;
    const char* w = "";
    const uintptr_t wi = WalkIface(player, &w);
    if (g_ropeVertSet || g_ropeLift > 0.0f) { WalkVertical(wi, false, 0.0f); }
    g_ropeVertSet = false;
    g_ropeLift = 0.0f;
    bool handed = false;
    if (ClimbByHand && haveFeet) {
        HandOffVelocity(v, feet, true, now, false);
        handed = true;
    } else {
        const char* bw = "";
        handed = ApplyBodyVel(player, v, &bw);
    }
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=grapple_rope_swing  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] ROPE #%ld MOMENTUM HANDED ON - %s, %llu ms after letting go | velocity (%.2f, %.2f, %.2f) m/s (%.2f m/s "
            "sideways) %s", g_ropeSeq, what, static_cast<unsigned long long>(freeMs), v[0], v[1], v[2],
            std::sqrt(v[0] * v[0] + v[2] * v[2]),
            handed ? (ClimbByHand && haveFeet ? "like a launch: the air carry keeps it until you land" : "set on the body once")
                   : "*** NOT handed on (the walk body was not there) ***");
    }
}

// Round 4: write the pull's end-block branch (kRopeEndJccRva) as the skip (true) or the game's je (false). One compare-exchange of the
// aligned qword holding it (offset checked at install). True = the wanted form is there now.
static bool RopeEndWrite(bool skip)
{
    const uintptr_t at = g_ropeEndAt;
    if (!at) { return false; }
    const uintptr_t q = at & ~static_cast<uintptr_t>(7);
    const size_t off = static_cast<size_t>(at - q);
    const unsigned char* const from = skip ? kRopeEndJcc : kRopeEndSkip;
    const unsigned char* const to = skip ? kRopeEndSkip : kRopeEndJcc;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(q), 8, PAGE_EXECUTE_READWRITE, &old)) { return false; }
    bool ok = false;
    __try {
        const LONG64 cur = *reinterpret_cast<volatile LONG64*>(q);
        unsigned char b[8];
        std::memcpy(b, &cur, 8);
        if (std::memcmp(b + off, to, 6) == 0) {
            ok = true;
        } else if (std::memcmp(b + off, from, 6) == 0) {
            std::memcpy(b + off, to, 6);
            LONG64 nv = 0;
            std::memcpy(&nv, b, 8);
            ok = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(q), nv, cur) == cur;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(q), 8, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(q), 8);
    return ok;
}

// Round 4: keep the game's pull from ending itself (want) or give it its end back. Writes only on a change; a failed write is logged
// and switches the skip off for the session (g_ropeEndAt = 0) after one restore attempt.
static void RopeKeepOn(bool want)
{
    if (want == g_ropeEndKept || !g_ropeEndAt) { return; }
    if (RopeEndWrite(want)) {
        g_ropeEndKept = want;
        return;
    }
    if (want) { RopeEndWrite(false); }
    g_ropeEndAt = 0;
    g_ropeEndKept = false;
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=grapple_hold_to_swing  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] *** the rope's end-block branch gamedll+%llX could not be %s - off for this session: the game's rope may end "
            "itself under a held rope (ours then lets go) ***", static_cast<unsigned long long>(kRopeEndJccRva),
            want ? "skipped" : "restored");
    }
}

// Let go: the game's rope controller leaves its group (its own exit); the body then flies free on the rope's velocity until the walk is
// MoveController's (GrappleRopeWalk), which hands the momentum on (RopeHandOff). why = what ended it (log).
static void RopeLetGo(uintptr_t player, const char* why, const float feet[3], bool haveFeet, ULONGLONG now)
{
    RopeKeepOn(false);   // round 4: the game's rope gets its own end back before it is left
    const float v[3] = { g_rope.v[0], g_rope.v[1], g_rope.v[2] };
    const ULONGLONG ms = now - g_rope.t0;
    const float len0 = g_rope.len0, len = g_rope.len;
    g_rope.on = false;
    InterlockedExchange(&g_ropeHoldsR2, 0);
    InterlockedExchange(&g_ropeOnPub, 0);
    InterlockedIncrement64(&g_wRopeEnds);
    const uintptr_t ctl = RopeCtl(player);
    int before = -1, after = -1;
    if (ctl) { RdI(ctl + kRopeStateOff, &before); }
    const char* exit = "the game's rope had already ended";
    if (ctl && before != 0) {
        if (!g_ropeLeave) {
            exit = "*** the game's rope is left running (no leave-group call this session) ***";
        } else {
            __try {
                g_ropeLeave(player, kGroupRope);
                exit = "the game's rope left its group 24";
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                exit = "*** the leave-group call FAULTED ***";
            }
            RdI(ctl + kRopeStateOff, &after);
        }
    }
    g_ropeFree = true;
    g_ropeFreeT0 = now;
    if (Line()) {
        // [PROBE: CLIMBHAND] gate=grapple_rope_swing  site=HEADSET  log-only=no
        LOG("[CLIMBHAND] ROPE #%ld LET GO - %s after %llu ms | %s (its state %d -> %d) | velocity (%.2f, %.2f, %.2f) m/s: flying free on "
            "it until MoveController has the walk (MOMENTUM HANDED ON says when) | the rope was %.2f m at the catch, %.2f m at the end",
            g_ropeSeq, why, static_cast<unsigned long long>(ms), exit, before, after, v[0], v[1], v[2], len0, len);
    }
    (void)feet; (void)haveFeet;
}

bool GrappleRopeHoldsR2() { return g_ropeHoldsR2 != 0; }

void GrapplePadSwap(unsigned char* p)
{
    if (!GrappleRopeSwing) { return; }
    if (grapplerope::TriggersSwapped(GrappleThrowHand, ThrowableHookSelected(), g_ropeOnPub != 0)) {
        const unsigned char l2 = p[0x08];
        p[0x08] = p[0x09];
        p[0x09] = l2;
    }
}

// Round 2: push the game's pull time limit out (kRopeLimitOff, see kRopeNoTimeout). lim / inState = what it held before. False = faulted.
static bool RopeHoldTimer(uintptr_t ctl, float* lim, float* inState)
{
    __try {
        *lim = *reinterpret_cast<const float*>(ctl + kRopeLimitOff);
        *inState = *reinterpret_cast<const float*>(ctl + kRopeTimeOff);
        *reinterpret_cast<float*>(ctl + kRopeLimitOff) = kRopeNoTimeout;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GrappleRopeWalk(uintptr_t player, const float in[3], float out[3], bool moveCtl)
{
    if (!GrappleRopeSwing || !g_gd || player != LocalPlayer()) {
        if (g_rope.on && player == LocalPlayer()) {
            float f[3];
            const bool hf = Feet(player, f);
            RopeLetGo(player, "grapple_rope_swing switched off", f, hf, GetTickCount64());
        }
        if (player == LocalPlayer()) { g_ropeFree = false; }
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    const uintptr_t ctl = RopeCtl(player);
    int state = -1;
    if (ctl) { RdI(ctl + kRopeStateOff, &state); }
    float feet[3];
    const bool haveFeet = Feet(player, feet);
    // the feet's velocity between walk calls (a call with no physics step between it and the last reads as no movement: skipped)
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); }
    const float dt = g_ropeQpc.QuadPart ? static_cast<float>(static_cast<double>(q.QuadPart - g_ropeQpc.QuadPart) / g_qpf.QuadPart) : 0.0f;
    g_ropeQpc = q;
    bool freshMeas = false;
    if (haveFeet) {
        if (g_ropeHavePrev && dt >= 0.004f && dt <= 0.25f) {
            const float d[3] = { feet[0] - g_ropePrevFeet[0], feet[1] - g_ropePrevFeet[1], feet[2] - g_ropePrevFeet[2] };
            if (std::fabs(d[0]) > 1e-5f || std::fabs(d[1]) > 1e-5f || std::fabs(d[2]) > 1e-5f) {
                for (int i = 0; i < 3; ++i) { g_ropeMeasV[i] = d[i] / dt; }
                g_ropeHaveMeas = true;
                freshMeas = true;
            }
        } else if (dt > 0.25f) {
            g_ropeHaveMeas = false;
        }
        for (int i = 0; i < 3; ++i) { g_ropePrevFeet[i] = feet[i]; }
        g_ropeHavePrev = true;
    }
    bool jump = false;
    GetControllerButton(kButtonA, jump);
    const bool jumpEdge = jump && !g_ropePrevJump;
    g_ropePrevJump = jump;
    const int prev = g_ropePrevState;
    g_ropePrevState = state;
    // the hook caught (the game's state enters the pull): a new rope - a rope already held lets go of the old hook first
    if (state == kRopeStatePull && prev != kRopeStatePull && haveFeet) {
        float hook[3] = {};
        if (RdF3(ctl + kRopeHookOff, hook) && (hook[0] != 0.0f || hook[1] != 0.0f || hook[2] != 0.0f)) {
            if (g_rope.on) { RopeLetGo(player, "a new throw caught", feet, true, now); }
            g_ropeFree = false;                                     // (a free flight from the last rope: this rope has the body)
            if (g_carry.on) { CarryEnded(); g_carry.on = false; }   // a launch's / the air's carry: the rope has the body now
            const float zero[3] = {};
            const float* v0 = g_ropeHaveMeas ? g_ropeMeasV : zero;   // the feet's measured velocity
            grapplerope::StartRope(g_rope, hook, feet, v0, now);
            ++g_ropeSeq;
            g_ropeLift = 0.0f;
            g_ropeTraceLines = 0;
            g_ropeTraceMs = 0;
            g_ropeTimeSaid = false;
            InterlockedExchange(&g_ropeOnPub, 1);
            InterlockedIncrement64(&g_wRopeStarts);
            if (Line()) {
                char rules[260];
                const char* const gName = GrappleThrowHand == 1 ? "right" : "left";
                const char* const pName = GrapplePullHand == 1 ? "right" : "left";
                const bool same = (GrappleThrowHand == 1) == (GrapplePullHand == 1);
                const char* const pullTxt = !GrappleTriggerPull ? " (pulling OFF)" :
                    same ? (GrappleHoldToSwing ? " squeezed fully (the same trigger: held part way it swings)" : "") :
                           " (its game trigger, R2, withheld meanwhile)";
                if (GrappleHoldToSwing) {
                    _snprintf_s(rules, sizeof(rules), _TRUNCATE, "grapple_hold_to_swing: the %s trigger holds it (let go = release it), "
                                "the %s trigger pulls%s", gName, pName, pullTxt);
                } else {
                    _snprintf_s(rules, sizeof(rules), _TRUNCATE, "the %s trigger pulls%s; let go: jump button, landing after air "
                                "time, the hands within %.1f m of the hook", pName, pullTxt, grapplerope::kArriveM);
                }
                // [PROBE: CLIMBHAND] gate=grapple_rope_swing  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] ROPE #%ld CAUGHT - the hook at (%.2f, %.2f, %.2f), the rope %.2f m | the feet (%.2f, %.2f, %.2f), "
                    "velocity %s(%.2f, %.2f, %.2f) m/s kept | the game's rope %d -> %d, %s | %s, up to %.1f m/s",
                    g_ropeSeq, hook[0], hook[1], hook[2], g_rope.len, feet[0], feet[1], feet[2],
                    g_ropeHaveMeas ? "" : "NOT MEASURED - ", g_rope.v[0], g_rope.v[1], g_rope.v[2], prev, state,
                    moveCtl ? "MoveController holds the player" : "another controller holds the player", rules, GrapplePullMps);
            }
        } else if (Line()) {
            // [PROBE: CLIMBHAND] gate=grapple_rope_swing  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] ROPE: the game's rope entered its pull with NO hook point at +0x88 (unreadable or zero) - left to the game");
        }
    }
    // the vertical: steer the walk's vertical speed by what the feet did (see g_ropeLift). False = the call was refused.
    const auto vertical = [&](uintptr_t wi, float wantVy, bool grounded) {
        if (!grounded && freshMeas) {
            g_ropeLift += kRopeLiftK * (wantVy - g_ropeMeasV[1]);
            if (g_ropeLift < 0.0f) { g_ropeLift = 0.0f; }
            if (g_ropeLift > kRopeLiftMax) { g_ropeLift = kRopeLiftMax; }
        } else if (grounded) {
            g_ropeLift = wantVy > 0.0f ? wantVy : 0.0f;   // a pull lifting the body off the ground
        }
        bool ok = true;
        if (g_ropeLift > 0.0f) {
            ok = WalkVertical(wi, true, g_ropeLift);
            g_ropeVertSet = ok;
        } else if (g_ropeVertSet) {
            ok = WalkVertical(wi, false, 0.0f);
            g_ropeVertSet = false;
        }
        if (!ok) { InterlockedIncrement64(&g_wRopeVertFail); }
        return ok;
    };
    if (!g_rope.on && g_ropeFree) {   // round 2: let go - free flight until MoveController has the walk, then the carry
        if (!haveFeet) { return false; }
        const char* fw = "";
        const uintptr_t fwi = WalkIface(player, &fw);
        const bool fground = WalkAirborne(fwi) == 0;
        if (moveCtl || fground || now - g_ropeFreeT0 > grapplerope::kFreeMaxMs) {
            RopeHandOff(player, feet, true, now, moveCtl ? "MoveController has the walk again" : fground ? "the walk is on the ground" :
                        "*** the free flight timed out with another controller still holding the walk ***");
            return false;   // this call is the carry's (ClimbHandWalkOverride)
        }
        grapplerope::FreeStep(g_rope, dt, fground);
        for (int i = 0; i < 3; ++i) { out[i] = g_rope.v[i]; }
        vertical(fwi, g_rope.v[1], fground);
        return true;
    }
    if (!g_rope.on) { return false; }
    // a climb took the player (a hand caught a ledge, a ladder): the rope lets go, the climb has the body
    unsigned char act = 0;
    if (g_egOk && g_egPtr && RdB(g_egPtr + kActiveOff, &act) && act) {
        RopeLetGo(player, "a climb took the player", feet, haveFeet, now);
        g_ropeFree = false;   // (no free flight: the climb has the body)
        return false;
    }
    if (!haveFeet) { return false; }   // (one call without the feet: the game's vector this once)
    const char* why = "";
    const uintptr_t wi = WalkIface(player, &why);
    const int air = WalkAirborne(wi);
    const bool grounded = air == 0;
    // round 2 (grapple_hold_to_swing): the grappling trigger holds the rope; round 3: the pull is the pull hand's trigger
    // (grapple_hand / grapple_pull_hand, 0 left 1 right)
    const int gh = GrappleThrowHand == 1 ? 1 : 0, ph = GrapplePullHand == 1 ? 1 : 0;
    float trigs[2] = {};
    GetControllerTrigger(kLeftHand, trigs[0]);
    GetControllerTrigger(kRightHand, trigs[1]);
    const bool hold = GrappleHoldToSwing;
    const float trig = trigs[ph];
    g_ropeHeld = trigs[gh] >= kHoldOff;
    InterlockedExchange(&g_ropeHoldsR2, grapplerope::WithholdR2(gh, ph, true, GrappleTriggerPull) ? 1 : 0);
    const float pull = grapplerope::PullMps(GrappleTriggerPull, trig, GrapplePullMps, grapplerope::PullThreshold(gh, ph, hold));
    float hp[3];
    grapplerope::HandPoint(feet, hp);
    const float toHook[3] = { hp[0] - g_rope.anchor[0], hp[1] - g_rope.anchor[1], hp[2] - g_rope.anchor[2] };
    // round 4: the game's rope still in its pull (an unreadable controller is not taken as an end)
    const bool gameHolds = !ctl || state == kRopeStatePull;
    const int end = grapplerope::RopeEndCheck(g_rope, jumpEdge, grounded, grapplerope::Len3(toHook), now, hold, g_ropeHeld, gameHolds);
    if (end != grapplerope::kReNone) {
        float v[3];
        grapplerope::LetGoVelocity(end, g_rope.v, g_ropeMeasV, g_ropeHaveMeas, v);   // the game ended first: what the feet did
        for (int i = 0; i < 3; ++i) { g_rope.v[i] = v[i]; }
        RopeLetGo(player, RopeEndName(end), feet, true, now);
        return false;
    }
    // round 2: hold mode keeps the game's rope pulling (drawn) as long as ours holds - its time limit is pushed out every call;
    // round 4: and its end block (reaching the hook, a contact, the time limit) is skipped
    RopeKeepOn(grapplerope::KeepGameRopeOn(hold, g_rope.on, state));
    if (hold && ctl && state == kRopeStatePull) {
        float lim = 0.0f, inState = 0.0f;
        const bool wrote = RopeHoldTimer(ctl, &lim, &inState);
        if (wrote) { InterlockedIncrement64(&g_wRopeTimeHeld); }
        if (!g_ropeTimeSaid && Line()) {
            g_ropeTimeSaid = true;
            // [PROBE: CLIMBHAND] gate=grapple_hold_to_swing  site=HEADSET  log-only=no
            LOG("[CLIMBHAND] ROPE #%ld: the game's pull time limit %s (it was %.2f s, %.2f s into the pull), its own end (reaching the "
                "hook, a contact, the time limit) %s - the rope stays until the %s trigger is released or the hands are within %.1f m "
                "of the hook", g_ropeSeq, wrote ? "HELD OFF" : "*** could NOT be written ***", lim, inState,
                g_ropeEndKept ? "SKIPPED" : "*** NOT skipped (see the install line) ***", gh == 1 ? "right" : "left",
                grapplerope::kArriveM);
        }
    }
    // a wall: the body moved far less sideways than the last call sent
    if (freshMeas) {
        const float sentH[2] = { g_ropeSent[0], g_ropeSent[2] }, measH[2] = { g_ropeMeasV[0], g_ropeMeasV[2] };
        if (grapplerope::WallCheck(g_rope, sentH, measH)) { InterlockedIncrement64(&g_wRopeWalls); }
    }
    const grapplerope::StepOut so = grapplerope::RopeStep(g_rope, feet, dt, grounded, pull);
    for (int i = 0; i < 3; ++i) { out[i] = so.v[i]; g_ropeSent[i] = so.v[i]; }
    const bool vok = vertical(wi, so.v[1], grounded);
    InterlockedIncrement64(&g_wRopeCalls);
    if (so.pulled) { InterlockedIncrement64(&g_wRopePulling); }
    if (g_ropeSeq <= kRopeTraced && g_ropeTraceLines < kRopeTraceMax && now - g_ropeTraceMs >= kRopeTraceEveryMs) {
        g_ropeTraceMs = now;
        ++g_ropeTraceLines;
        // [PROBE: CLIMBHAND] gate=grapple_rope_swing  site=HEADSET  log-only=yes
        LOG("[CLIMBHAND] ROPE #%ld TRACE t+%llu | the game's rope state %d, %s, the walk %s | the game's vector (%.2f, %.2f, %.2f), "
            "ours (%.2f, %.2f, %.2f), the feet measured %s(%.2f, %.2f, %.2f) m/s | vertical lift %.2f m/s%s | hands to hook %.2f m, "
            "rope %.2f m%s, closing %.2f m/s | trigger %.2f%s", g_ropeSeq, static_cast<unsigned long long>(now - g_rope.t0), state,
            moveCtl ? "MoveController" : "another controller", air == 1 ? "in the air" : (air == 0 ? "ON THE GROUND" : "air flag unread"),
            in[0], in[1], in[2], so.v[0], so.v[1], so.v[2], g_ropeHaveMeas ? "" : "(stale) ", g_ropeMeasV[0], g_ropeMeasV[1],
            g_ropeMeasV[2], g_ropeLift, vok ? "" : " (*** REFUSED ***)", so.dist, g_rope.len, so.taut ? " taut" : " slack", so.closing,
            trig, so.pulled ? " PULLING" : "");
    }
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
            if (ZipWalk(player, in, out)) { return true; }   // P6 round 3: the hands on a zipline's cable
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
            if (ClimbAirControlPct != s_tracedCtl || ClimbAirDrag != s_tracedDrag) {
                s_tracedCtl = ClimbAirControlPct; s_tracedDrag = ClimbAirDrag;
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
                    "pushes %.1f %% of the gap to its speed per update (climb_air_control_pct; the game's own is %.2f %%)",
                    WalkFlags(wi), g_fv.haveV ? "" : "NOT MEASURED YET ", v[0], v[1], v[2], ClimbAirControlPct,
                    100.0f * climbhand::kGameAirK);
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
    const float pullCap = climbhand::GrabGlideCap(h.gliding, ClimbGrabMoveMps, ClimbPullMaxMps);
    if (h.gliding && h.drive) { InterlockedIncrement64(&g_wGlide); }
    const climbhand::MotionOut mo = climbhand::MotionStep(g_motion, h, feet, now, ClimbMantleAuto ? ClimbMantleM : 0.0f,
                                                          pullCap, ClimbLaunchMinMps > 0.0f ? ClimbLaunchMinMps : 1.5f);
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
    float squeeze[2] = {};   // P5 slide: how far each grip is squeezed (0 = unread)
    for (int h = 0; h < 2; ++h) {
        float sq = 0.0f;
        in.grip[h] = GetControllerSqueeze(h == 0 ? kLeftHand : kRightHand, sq) && sq >= 0.5f;   // the grip block's threshold
        squeeze[h] = in.grip[h] ? sq : 0.0f;
        in.handOk[h] = LootHandWorldPos(h, hand[h]);
        if (in.grip[h] && !g_m.prevGrip[h] && in.owned[h]) { s_edgeOwner[h] = owner[h]; }
    }
    {   // climb_jump_by_look: the headset's forward in the game world, for the jump button's aim (the hands' own yaw mapping)
        XrPosef head{};
        float rel = 0.0f;
        if (ClimbJumpByLook && GetHeadPose(head) && CamHookGetOpenXrToGameYaw(rel)) {
            const float q[4] = { head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w };
            float f[3] = {};
            climbhand::HeadForwardGame(q, rel, AimMirrorYaw, f);
            AcquireSRWLockExclusive(&g_lock);
            for (int i = 0; i < 3; ++i) { g_lookFwd[i] = f[i]; }
            g_lookMs = in.nowMs;
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
    {   // P3 round 3: the left stick pushed forward is what lets the game's own climb-up from a hang through (StateHook)
        float sx = 0.0f, sy = 0.0f;
        static bool s_fwd = false;
        const bool fwd = GetControllerStick(kLeftHand, sx, sy) && sy >= climbhand::kStickForward;
        if (fwd) {
            if (!s_fwd) { g_stickPushMs = in.nowMs; }   // round 13: the push's start
            g_stickFwdMs = in.nowMs;
        }
        s_fwd = fwd;
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
    bool zipWithhold[2] = {};   // P6 round 3: a hand holding the zipline's cable keeps its grip from the game
    {   // P6 round 3: the hands on the zipline (climbhand_logic.h ZipHold) - before Update, which moves prevGrip on
        unsigned seq = 0;
        int sh = -1;
        float spt[3] = {}, adv[3] = {};
        climbhand::ZipLine sl;
        AcquireSRWLockShared(&g_lock);
        seq = g_zipStartSeq;
        sh = g_zipStartHand;
        for (int i = 0; i < 3; ++i) { spt[i] = g_zipStartPt[i]; adv[i] = g_zipAdvSum[i]; }
        sl = g_zipStartLine;
        ReleaseSRWLockShared(&g_lock);
        if (seq != g_zipSeqSeen) {
            g_zipSeqSeen = seq;
            climbhand::ZipHoldStart(g_zh, sh, spt, sl);
            for (int i = 0; i < 3; ++i) { g_zipAdvSeen[i] = adv[i]; }
            g_zipHoldMs = in.nowMs;
            if (Line()) {
                // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] ZIPLINE HOLD - %s hand on the cable at (%.2f, %.2f, %.2f): it rides with the slide, moving the hand moves "
                    "the body; open the grips to let go (a fast pull launches)", sh == 0 ? "left" : "right", spt[0], spt[1], spt[2]);
            }
        }
        if (g_zh.on) {
            float delta[3];
            for (int i = 0; i < 3; ++i) { delta[i] = adv[i] - g_zipAdvSeen[i]; g_zipAdvSeen[i] = adv[i]; }
            const bool edge[2] = { in.grip[0] && !g_m.prevGrip[0], in.grip[1] && !g_m.prevGrip[1] };
            const bool fresh = in.game.ageMs <= climbhand::kGameFreshMs;
            const bool riding = !fresh || in.game.zipRiding || in.nowMs - g_zipHoldMs < kZipStartGraceMs;
            const float releaseZ = ClimbReleaseM > ClimbReachM + 0.1f ? ClimbReleaseM : ClimbReachM + 0.1f;
            const climbhand::ZipHoldOut zo = climbhand::ZipHoldStep(g_zh, riding, in.grip, edge, in.handOk, hand, delta, ClimbReachM,
                                                                    releaseZ);
            AcquireSRWLockExclusive(&g_lock);
            g_zipHoldOn = g_zh.on;
            g_zipDrive = zo.drive;
            for (int i = 0; i < 3; ++i) { g_zipErr[i] = zo.err[i]; }
            g_zipPubMs = in.nowMs;
            ReleaseSRWLockExclusive(&g_lock);
            if (zo.end) { InterlockedExchange(&g_reqZipEnd, zo.endOpened ? 1 : 2); }
            for (int h = 0; h < 2; ++h) {
                zipWithhold[h] = g_zh.has[h];
                if (zo.released[h] && in.grip[h]) { g_relWithhold[h] = true; }   // a slip's still-closed grip: no kick / grapple
                if ((zo.joined[h] || zo.slipped[h]) && Line()) {
                    // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand  site=HEADSET  log-only=no
                    LOG("[CLIMBHAND] ZIPLINE %s hand %s", h == 0 ? "left" : "right",
                        zo.joined[h] ? "JOINED on the cable" : "SLIPPED off the cable (pulled past climb_release_m)");
                }
            }
            if (zo.gameEnded) {   // the grips still closed at the line's end are not a kick / grapple: withheld until they open
                for (int h = 0; h < 2; ++h) { if (in.grip[h]) { g_relWithhold[h] = true; } }
            }
            if (zo.gameEnded && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_zipline_by_hand  site=HEADSET  log-only=no
                LOG("[CLIMBHAND] ZIPLINE HOLD ended by the game (the line's end, or its own jump / drop) after %llu ms",
                    static_cast<unsigned long long>(in.nowMs - g_zipHoldMs));
            }
        } else {
            AcquireSRWLockExclusive(&g_lock);
            g_zipHoldOn = false;
            g_zipDrive = false;
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
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
        // Round 19: the game's own shimmy / reach carried the body (MoveShiftStep, game thread): each held ledge point moves with it,
        // before PullStep re-latches the anchors on them - otherwise the hang pulls the body straight back to where the move began.
        if (in.game.moveSeq != g_mvSeqSeen) {
            g_mvSeqSeen = in.game.moveSeq;
            for (int h = 0; h < 2; ++h) {
                if (!g_haveHoldLip[h]) { continue; }
                for (int i = 0; i < 3; ++i) { g_holdLip[h][i] += in.game.moveShift[i]; }
            }
        }
        if (o.event == climbhand::kEvStarted) { g_grabStartMs = in.nowMs; }   // round 13: StickClimbPush's grab start
        // P3 round 2: a climb that ends with grips still squeezed (the game's climb-up onto the top, a launch's drop) keeps them
        // withheld until they open, so they are not a kick / grapple the moment the climb hands back.
        if (o.event == climbhand::kEvEnded) {
            for (int h = 0; h < 2; ++h) { if (in.grip[h]) { g_relWithhold[h] = true; } }
        }
        const bool hold = in.game.ageMs <= climbhand::kGameFreshMs && in.game.egActive && climbhand::IsHoldState(in.game.egState);
        {   // P5 slide: a light grip on a ladder / pole moves the held anchors down; the body follows them (climbhand_logic.h Slide)
            const bool gripSlide = climbhand::SlideGrip(ClimbSlideMps, in.game.pole, hold, g_m.held, squeeze, ClimbSlideGrip);
            float down = 0.0f;
            const float y0 = g_slide.y0, slid = g_slide.slidM;
            const bool haveY0 = g_slide.haveY0;
            const ULONGLONG t0 = g_slide.t0;
            const int r = climbhand::SlideStep(g_slide, gripSlide, in.nowMs, ClimbSlideMps, ClimbPullMaxMps, in.game.haveFeet,
                                               in.game.feetY, in.game.havePoleLo, in.game.poleLoY, &down);
            if (down > 0.0f) {
                for (int h = 0; h < 2; ++h) {
                    if (!(g_m.held & (1u << h))) { continue; }
                    if (g_haveHoldLip[h]) { g_holdLip[h][1] -= down; }
                    if (g_pull.has[h]) { g_pull.anchor[h][1] -= down; }
                }
                InterlockedIncrement64(&g_wSlidePolls);
            }
            if (r == climbhand::kSlideBottom) { InterlockedExchange(&g_reqDrop, 4); }   // the plain drop: stand at the foot
            if (r != climbhand::kSlideNone && r != climbhand::kSlideMove && Line()) {
                // [PROBE: CLIMBHAND] gate=climb_slide_mps  site=HEADSET  log-only=no
                if (r == climbhand::kSlideStart) {
                    LOG("[CLIMBHAND] SLIDE started on the %s - grips %.2f / %.2f (under climb_slide_grip %.2f), %.2f m/s (climb_slide_mps; "
                        "at most %.2f, 0.8 x climb_pull_max_mps) | the feet at %.2f, its bottom %s %.2f", PoleKindName(in.game.poleKind),
                        static_cast<double>(squeeze[0]), static_cast<double>(squeeze[1]), static_cast<double>(ClimbSlideGrip),
                        static_cast<double>(ClimbSlideMps), static_cast<double>(ClimbPullMaxMps * climbhand::kSlideCapShare),
                        static_cast<double>(in.game.feetY), in.game.havePoleLo ? "at" : "UNKNOWN (stops when the feet stall), read",
                        static_cast<double>(in.game.poleLoY));
                } else {
                    LOG("[CLIMBHAND] SLIDE %s after %llu ms: the hands slid %.2f m, the feet went %.2f m down",
                        r == climbhand::kSlideBottom
                            ? (in.game.havePoleLo && in.game.feetY - in.game.poleLoY <= climbhand::kSlideBottomM + 0.001f
                                   ? "reached the BOTTOM of the climbable - the hang ends (plain drop)"
                                   : "STALLED (the feet stopped going down: the ground or something solid) - the hang ends (plain drop)")
                            : "stopped (the grip tightened, or the hang ended)",
                        static_cast<unsigned long long>(in.nowMs - t0), static_cast<double>(slid),
                        haveY0 && in.game.haveFeet ? static_cast<double>(y0 - in.game.feetY) : -1.0);
                }
            }
        }
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
        hp.gliding = climbhand::GrabGliding(g_pull);   // climb_grab_move_mps: a new grab's glide onto the ledge point
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
    withholdL1 = o.withhold[0] || zipWithhold[0] || (g_relWithhold[0] && in.grip[0]);
    withholdR1 = o.withhold[1] || zipWithhold[1] || (g_relWithhold[1] && in.grip[1]);
}

}  // namespace dlvr
