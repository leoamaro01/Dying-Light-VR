// Hand climbing P1 - the pure decisions (docs/design_climbing_overhaul.md §4.2 / §4.3 / §6 P1, P0 results §8.4, P1 run 1 §8.5).
// No OpenXR, no Windows, no logging, so src/tests/climbhand_logic_test.cpp compiles it on its own. climbhand.cpp owns the reads, the
// game calls, the raycasts and the log lines; ovr2xr.cpp's pad thread feeds Update every poll.
//
// P1 round 2 (2026-11-26, after the first headset run): the ledge is no longer the game's LOOK-based target. While a grip is held,
// the game thread searches for a ledge AROUND THE HAND with a fan of rays (FindLedge - a cone of horizontal rays toward the wall,
// then rays down onto the top; every ray starts outside the hand, so a hand inside a ledge still finds it), and hands that ledge to
// the game's own "set climb target". A grip held while falling past a ledge catches it (the search runs for climb_catch_ms after the
// press). Ledges low enough to step onto are the game's own again (AutoLowWall); a hand grab below climb_hang_min_m climbs over,
// one above it hangs (ForceHang) unless climb_always_hang is off.
#pragma once

#include <cmath>

namespace dlvr {
namespace climbhand {

// Ledge-grab states that are a hang the hands may let go of (design doc §8.4, measured): 1 hang, 2 / 3 / 7 hang sub-states,
// 11 / 12 shimmy sideways, 13 shimmy up, 14 a blocked shimmy. NOT 0 (the scripted grab), 4 / 5 / 6 (jumps), 8 (the climb-up).
inline bool IsHangState(int s)
{
    return s == 1 || s == 2 || s == 3 || s == 7 || (s >= 11 && s <= 14);
}

// Distance from p to the segment a-b (a == b is a point).
inline float SegDist(const float p[3], const float a[3], const float b[3])
{
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = 0.0f;
    if (len2 > 1e-12f) {
        t = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    const float d[3] = { ap[0] - t * ab[0], ap[1] - t * ab[1], ap[2] - t * ab[2] };
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// The game's climb-target record is allocated once and ZEROED when its detector finds nothing (+BDBEE9 passes three zero vectors),
// so "a target exists" = its edge is not all zeros.
inline bool TargetValid(const float a[3], const float b[3])
{
    const float s = std::fabs(a[0]) + std::fabs(a[1]) + std::fabs(a[2]) + std::fabs(b[0]) + std::fabs(b[1]) + std::fabs(b[2]);
    return s > 1e-4f;
}

// Low walls stay the game's own (look + jump, as vaults): the game's own target is valid and its top is at most autoMaxM above the
// feet. autoMaxM <= 0 = never. P3 round 2 (tester 2026-11-27: "let's disable the low wall auto mantle completely, add a boolean
// setting"): enabled = climb_auto_low (default OFF) - off, the game starts no ledge grab of its own at all.
inline bool AutoLowWall(bool targetValid, float topY, float feetY, float autoMaxM, bool enabled = true)
{
    return enabled && targetValid && autoMaxM > 0.0f && topY - feetY <= autoMaxM;
}

// A hand grab whose ledge is at least hangMinM above the feet hangs (the game's climb type forced to 0) when alwaysHang is on.
// Lower ledges are left to the game's own choice (it climbs over) - hanging there would put the body in the floor.
inline bool ForceHang(bool alwaysHang, float lipY, float feetY, float hangMinM)
{
    return alwaysHang && lipY - feetY >= hangMinM;
}

// The state the ledge grab is allowed to enter when its grab (state 0) ends. Round 3 (run 2: a forced type 0 still went 0 -> 8):
// +D0A240 picks 8 (climb up) unless the type is 0 AND its own +0x204 / +0x205 flags say hang, so the type alone cannot make a hang.
// A hand grab that should hang turns that one transition's 8 into 1. Everything else is the game's.
inline int HangTransition(bool forceHang, int fromState, int requested)
{
    return (forceHang && fromState == 0 && requested == 8) ? 1 : requested;
}

// Run 5 ("I can grab zombies and climb on top of them"): a ray hit whose owning object (SCollision+0x18 - 24, named by RTTI) is a
// character is not a ledge. The live and dead AI classes in the gamedll: HumanAI (+ its HumanAIVis / ZombieAIVis visuals and blends)
// and DeadRagdoll. Exact, prefix or suffix matches only - a world object never carries these names.
inline bool IsCharacterClass(const char* n)
{
    if (!n || !n[0]) { return false; }
    auto starts = [n](const char* p) { for (int i = 0; p[i]; ++i) { if (n[i] != p[i]) { return false; } } return true; };
    auto equals = [n](const char* p) { int i = 0; for (; p[i]; ++i) { if (n[i] != p[i]) { return false; } } return n[i] == 0; };
    int len = 0;
    while (n[len]) { ++len; }
    const bool endsAiVis = len >= 5 && n[len - 5] == 'A' && n[len - 4] == 'I' && n[len - 3] == 'V' && n[len - 2] == 'i' && n[len - 1] == 's';
    return starts("HumanAI") || starts("ZombieAI") || equals("DeadRagdoll") || equals("AI") || endsAiVis;
}

// P2 round 3 (tester 2026-11-26: "the camera moved to the side almost every time I hanged after pulling myself up ... the code that
// moves your camera for the side jumps should be disabled"). +D0A240 (the hang's update) measures the look against the wall and,
// past a threshold either side, sets state 2 or 3 - the hang's look-left / look-right, whose update turns the camera (+D45B90 from
// +D0AE50) ready for a side jump. With hands climbing, the look is the headset's alone: those two transitions out of the plain hang
// are refused (the ledge grab stays in 1). Side jumps from a hang become ours (P3 launch).
inline bool RefuseLookState(bool handMode, int from, int to)
{
    return handMode && from == 1 && (to == 2 || to == 3);
}

// P3 round 2 (tester 2026-11-27): the climb-up from a hang is the game's own (state 8; run 10 logged `0>1>8` from the stick pushed
// forward). climb_mantle_stick (default on) keeps the stick doing it; off = a hold state -> 8 the game asks for is refused (the hang
// stays). Ours (climb_mantle_auto's height rule asks for 8 through the same set-state) always passes.
// Round 3 (run 11, 2026-11-27: "sometimes the game will do an auto mantle, often after a jump up while hanging, even though both
// auto mantle settings are disabled"): the game ALSO climbs up by itself when the body is high enough over its ledge - an upward
// launch flown in the hold state lifts it there (run 11 climbs #1 and #10: `0>1>8` ~0.8 s after the launch, no stick). So the
// game's own hold -> 8 now passes only while the stick IS pushed forward (stickForward, the pad thread's reading).
inline bool RefuseClimbUp(bool handMode, bool stickAllowed, bool stickForward, bool ours, int from, int to)
{
    const bool hold = from == 1 || from == 2 || from == 3 || from == 7 || from == 14;   // IsHoldState (defined with P2 below)
    return handMode && !ours && to == 8 && hold && !(stickAllowed && stickForward);
}
constexpr float              kStickForward    = 0.5f;   // the left stick's y at or past this = pushed forward
constexpr unsigned long long kStickFreshMs    = 150;    // ...and that reading counts this long (the pad and game threads differ)
// P4 round 13 (tester 2026-11-27: "I'm still getting automatic mantles from time to time, even though I have both settings
// disabled"; the log: a grab at a running jump, GRAB END 0 -> 1, and 16 ms later the game's 1 -> 8 passed - the stick was still
// forward from the run-up). The stick's forward counts only for a push that STARTED after this ledge grab did (pushMs = when the
// stick last went from not forward to forward, grabMs = the grab's start; 0 = unknown, which never blocks). A stick held from
// before the grab must be let back and pushed again.
inline bool StickClimbPush(unsigned long long nowMs, unsigned long long fwdMs, unsigned long long pushMs, unsigned long long grabMs)
{
    if (fwdMs == 0 || nowMs < fwdMs || nowMs - fwdMs > kStickFreshMs) { return false; }
    return pushMs != 0 && pushMs >= grabMs;
}

// P4 round 2 (tester 2026-11-27, after run 12): "after several physical jumps through sequential vertical ledges, at the top I pressed
// the stick up to mantle: it moved me back to a lower ledge and shimmied up from it to the next one ... only after several tries it
// mantled", and "if I'm on the top ledge, jump physically and push the stick forward mid-air, it mantles mid-air". The ledge grab's
// own ledge (its hook, PlayerDI+0x14C8) is still the one it FIRST grabbed: hand-over-hand moves and launch catches move the body and
// join new ledges, but never re-target the game. So its stick climb-up / shimmy run from the stale ledge.
// HookMismatch: the ledge the hands hold (LedgeHeld's point and inward direction) is not the game's - more than kStaleUpM above /
// below it, more than kStaleSideM along, or facing more than ~30 deg away (cos < kStaleCos). False when either is unknown; the
// direction test is skipped when either direction is degenerate.
// P4 round 3 (run 13, tester 2026-11-27): round 2's 0.4 m / 1 m and no direction test missed a hand-over-hand move onto an
// ORTHOGONAL ledge at a corner ("I moved both hands to the new one, pressed stick up, it mantled from the first ledge") - the two
// ledge points were close, only the direction differed. Tighter now: a re-target costs nothing visible (below), a missed one climbs
// from the wrong ledge.
constexpr float kStaleUpM   = 0.25f;
constexpr float kStaleSideM = 0.5f;
constexpr float kStaleCos   = 0.866f;   // cos 30 deg
inline bool HookMismatch(bool haveHold, const float holdMid[3], const float holdInto[3], bool haveHook, const float hookMid[3],
                         const float hookDir[3])
{
    if (!haveHold || !haveHook) { return false; }
    const float dy = std::fabs(holdMid[1] - hookMid[1]);
    const float dx = holdMid[0] - hookMid[0], dz = holdMid[2] - hookMid[2];
    if (dy > kStaleUpM || std::sqrt(dx * dx + dz * dz) > kStaleSideM) { return true; }
    const float la = std::sqrt(holdInto[0] * holdInto[0] + holdInto[2] * holdInto[2]);
    const float lb = std::sqrt(hookDir[0] * hookDir[0] + hookDir[2] * hookDir[2]);
    if (!(la > 1e-3f) || !(lb > 1e-3f)) { return false; }
    return (holdInto[0] * hookDir[0] + holdInto[2] * hookDir[2]) / (la * lb) < kStaleCos;
}

// P4 round 14 (run 18 climb #9: three stick-ups on the top of four stacked ledges each shimmied up from one ledge higher): the ledge
// grab keeps its OWN copy of the ledge point (eg+0x4C, +D16690) beside the player's (+0x14C8), and its stick-up starts from the copy.
// A re-target writes the player's, so that one alone reads "the same" afterwards. Stale = either is not the held ledge (the copy is
// compared with the player's direction; an unread copy is not a mismatch).
inline bool HeldStale(bool haveHold, const float holdMid[3], const float holdInto[3], bool haveHook, const float hookMid[3],
                      const float hookDir[3], bool haveCopy, const float copyMid[3])
{
    return HookMismatch(haveHold, holdMid, holdInto, haveHook, hookMid, hookDir) ||
           HookMismatch(haveHold, holdMid, holdInto, haveHook && haveCopy, copyMid, hookDir);
}

// What hand mode does with a move the GAME asks for from a hold state (its set-state, StateHook). stickShimmy = climb_stick_shimmy
// (the game's own shimmy states 11-15 from the stick; tester: "a setting to remove the original game functionality and only leave
// joystick forward to mantle"), stickClimb = climb_mantle_stick, stickForward = the stick is pushed forward now, ours = our own
// set-state, held = the ledge the hands hold is known (round 8; rounds 2-7 passed stale = HookMismatch here), retargetFresh = we
// re-targeted the game within kRetargetHoldMs (this is the game's ask again, from the re-targeted ledge). (Rounds 2-4 also refused / handed over moves asked mid-flight; since P4 round 5 a launch drops out of
// the hang at once, so no hold state is ever mid-air.)
//   - on a stale ledge: kGmRetarget = refuse this one and re-target the game onto the ledge the hands hold, in place (its set-target
//     + its hook, no drop, no new start). The game asks again on its next update, now from the right ledge: at the top that is the
//     climb-up (8), below another ledge the shimmy up (13). Run 13 (P4 round 3): round 2 refused a stale SHIMMY outright and only
//     turned a stale climb-up (8) into a drop + restart - but with climb_stick_shimmy on, the stick up on the top ledge reached the
//     game as a shimmy up (13: the game, still on the lower ledge, saw a ledge above it), so "stick up did nothing at all".
//     If a re-target did not hold (retargetFresh), the move passes as the game asked: the game's own behaviour, logged.
//   - climb-up (8): needs the stick forward with climb_mantle_stick on (round 3).
//   - shimmy (11-15): needs climb_stick_shimmy.
// P4 round 4 (run 14): the game's stick-up in a hang (+D16480..+D16639) searches for a ledge ABOVE when it does not climb over:
// found = 13 (jump up to it), none = 9 (5 with a game option bit) - a short reach up that drops back ("rises up a bit but not
// mantle"). The jump button from a hang also asks 9 / 4 (+D19D4B). 5 / 9 on a stale ledge are re-targeted like the others (the
// game then searches above from the ledge the hands hold); otherwise they are the game's own.
// P4 round 8 (run 15, tester 2026-11-27: "Mantling failing on a ledge it should work is still not working sometimes. What would be
// the disadvantage of just updating the current ledge whenever it's needed, instead of keeping a value that is bound to grow
// stale"): run 15 #17 went 1 > 9 > 1 > 16 - the game reached up (no climb-up from ITS ledge) while HookMismatch called it the same
// ledge, so nothing was re-targeted. Now every move the game asks from a hold is first re-targeted onto the ledge the hands hold
// whenever that is known (held), stale or not - the game asks again from it on its next update and that ask (retargetFresh) goes
// by the rules below. The cost: each move starts one update (~14 ms) later, and the game's own ledge record is replaced by ours
// even when it was already right (the same ledge: no visible change). HookMismatch stays for the log.
enum GameMoveVerdict : int { kGmPass = 0, kGmRefuse = 1, kGmRetarget = 2 };
constexpr unsigned long long kRetargetHoldMs = 400;
// Round 19 (tester 2026-11-27: "if you could remove the animation when it doesn't find a place to shimmy and it can't mantle, that'd
// also be great, it doesn't make any sense in VR"): skipReach = climb_skip_reach - the reach-up (5 / 9) is refused after the
// re-target (from the held ledge the game found nothing either), and the hang just stays. Round 20 (tester: "even though it doesn't
// do any animation, it still leaves me unable to do any actions for a second"; run 22: the reach still ran, 1 -> 9 -> 1 in 0.8 s,
// with 0 refused): round 19 refused only the stick-up search's own call (+D16639). The stick up then reaches the hang's input
// handler too (+D19CB5: state 1, its input flag) on the next update, and that handler asks 9 / 4 from +D19D50 / +D19D41. So every
// 5 / 9 from a hold is refused, whichever call asks. Those two calls are the only ones in the ledge grab that ask them. 4 (the
// option-bit variant at +D19D41) stays the game's.
inline int GameMoveFromHang(bool handMode, bool stickShimmy, bool stickClimb, bool stickForward, bool ours, bool held,
                            bool retargetFresh, int from, int to, bool skipReach = false, bool pole = false)
{
    const bool hold = from == 1 || from == 2 || from == 3 || from == 7 || from == 14;   // IsHoldState (defined with P2 below)
    const bool reachUp = to == 5 || to == 9;
    const bool move = to == 8 || (to >= 11 && to <= 15) || reachUp;
    if (!handMode || ours || !hold || !move) { return kGmPass; }
    // P5 round 1: a hang on a ladder / pole (PoleLedge) has no real ledge - the game's climb-up, shimmies and reach would act on the
    // made-up one. All refused, no re-target: climbing it is hand over hand.
    if (pole) { return kGmRefuse; }
    if (held && !retargetFresh) { return kGmRetarget; }
    if (reachUp) { return skipReach ? kGmRefuse : kGmPass; }
    if (to == 8) { return RefuseClimbUp(handMode, stickClimb, stickForward, ours, from, to) ? kGmRefuse : kGmPass; }
    return stickShimmy ? kGmPass : kGmRefuse;
}

// P4 round 5 - the VIEW while climbing (climb_view_smooth; tester 2026-11-27, after round 4's face-the-ledge turn: "what if I'm
// grabbing a ledge behind me? A good solution could be to have the body direction follow the view as before, but instead of the
// camera position being snapped to the eye bones, have it follow the eye bones smoothly so it doesn't snap"). Round 3 found the snap:
// with the body following the view, the hang's look pose turns / leans the head bone the camera rides, and turning the body swings it
// about the body's centre (§8.15). Rounds 2-4 tried to cancel that by moving or turning the body; this smooths the CAMERA instead.
// raw = the eye bone's offset from the body's origin, in world axes (eye - origin): body TRANSLATION (the hands' pull, a fall) moves
// both and passes 1:1 with no lag; what the animation / a turn does to the head relative to the body is low-passed with
// kViewSmoothTauS, and never more than kViewSmoothMaxM from the eye bone. The headset's own motion is not in raw (the caller adds the
// arc term after) and is never smoothed.
//   - starts when active (the ledge grab runs, hand mode) at the raw offset (no jump);
//   - after active ends it keeps easing back to the eye bone and switches off within kViewSmoothDoneM (no jump at the end either).
// corr = smoothed - raw: what the caller adds to the camera (and the hand anchor, so the drawn hands stay on the controllers).
// P4 round 11 - the RIGID view while climbing (climb_view_rigid; tester 2026-11-27: "can't we remove the pose changes that affect the
// view altogether? They don't add anything mechanically and are only there to improve immersion in a flatscreen game"). While the
// ledge grab runs in hand mode the camera's base is the standing one (the game's base + face offset the last time the player stood
// still, body-local): the body's own motion carries the view, the headset's arc is added as always, and what the climb animations do
// to the head never reaches it. RigidRampStep eases it in and out over kRigidRampS (no jump at a climb's start or end): k = the share
// of the rigid base, 0..1.
// P4 round 15 (tester 2026-11-27: "mantles seem to always leave me in the air a good half a meter above the floor over the ledge" /
// "As soon as the animation ends it drops me"; run 19: every climb-up ends with the body's origin on the lip's height, 1-5 cm over the
// floor, so the lift is inside the climb-up). The game lifts the body's origin over the top to clear the edge and sets it down at the
// end; the flatscreen view rides the animated head, which stays low, but the rigid view and the root hold ride the origin. While the
// game's climb-up runs (climbingUp), both are lowered by how far the origin is above the ledge top (lipY), never more than maxM -
// the view never stands higher than standing on the ledge. 0 otherwise.
constexpr float kOverLedgeMaxM = 1.0f;
inline float OverLedgeM(bool climbingUp, float originY, float lipY, float maxM = kOverLedgeMaxM)
{
    if (!climbingUp || !(originY == originY) || !(lipY == lipY)) { return 0.0f; }
    const float d = originY - lipY;
    return d <= 0.0f ? 0.0f : (d > maxM ? maxM : d);
}

// Round 16 (run 20: "still mantling too high (but just a bit now, I'd say 10cm or less)"). (1) The lift outlasts the climb-up state:
// CLIMB-UP #1 ended with the origin 57 cm over the top, and it comes down after. So after the climb-up the lowering goes on (a TAIL)
// until the origin is down on the top (0) or kOverLedgeTailMs, never growing again (a jump right after is not lowered). (2) Only a
// climb-up FROM A HANG counts (the previous state a hold state), and the top is the ledge point when it began: run 20's #2 was the
// game's own climb-over straight from a grab (0 > 8) and read a stale ledge point, 1 m off.
// Round 17 (run 21, tester 2026-11-27: "now the mantle height is sometimes correct, sometimes too low"; CLIMB-UP #3 / #4 / #5 all
// timed out with the origin still 4-16 cm over the ledge point: it stood on a floor that much higher than the lip, so that was no
// lift, and holding it then dropping it at 1.5 s showed as too low, then a snap). The tail now ends when the origin SETTLES: it has
// not come down more than kOverLedgeSettleM over kOverLedgeSettleMs = the body is on whatever it stands on. The lowering left then
// (and at the time-out) fades to 0 over kOverLedgeFadeMs instead of switching off - never a jump.
constexpr unsigned long long kOverLedgeTailMs = 1500;
constexpr unsigned long long kOverLedgeSettleMs = 150;
constexpr unsigned long long kOverLedgeFadeMs = 250;
constexpr float kOverLedgeSettleM = 0.01f;
struct OverLedgeState {
    int   prevState = -1;
    bool  climbing = false, tail = false, fading = false;
    float lipY = 0.0f, lastM = 0.0f;
    unsigned long long tailEndMs = 0;
    float settleY = 0.0f;                 // the origin when it last came down more than kOverLedgeSettleM
    unsigned long long settleMs = 0;
    float fadeFromM = 0.0f;
    unsigned long long fadeStartMs = 0;
    bool  settled = false;                // the tail ended because the body settled (false: it timed out) - for the log
};
enum OverLedgeEvent : int { kOlNone = 0, kOlStarted, kOlClimbEnded, kOlTailEnded };
inline float OverLedgeStep(OverLedgeState& s, bool active, int state, float originY, float topY, unsigned long long nowMs, int* event)
{
    *event = kOlNone;
    const bool climbingNow = active && (state == 8 || state == 10);
    const int p = s.prevState;
    const bool fromHold = p == 1 || p == 2 || p == 3 || p == 7 || p == 14;
    if (climbingNow && !s.climbing && fromHold) {
        s.climbing = true; s.tail = false; s.fading = false; s.lipY = topY; s.lastM = 0.0f; s.fadeFromM = 0.0f;
        *event = kOlStarted;
    } else if (s.climbing && !climbingNow) {
        s.climbing = false; s.tail = true; s.fading = false; s.settled = false; s.tailEndMs = nowMs + kOverLedgeTailMs;
        s.settleY = originY; s.settleMs = nowMs;
        *event = kOlClimbEnded;
    }
    float m = 0.0f;
    if (s.climbing) {
        m = OverLedgeM(true, originY, s.lipY);
    } else if (s.tail && !s.fading) {
        const float now = OverLedgeM(true, originY, s.lipY);
        m = now < s.lastM ? now : s.lastM;
        if (originY == originY && originY < s.settleY - kOverLedgeSettleM) { s.settleY = originY; s.settleMs = nowMs; }
        const bool settled = nowMs >= s.settleMs + kOverLedgeSettleMs;
        if (m <= 0.0f) {
            s.tail = false; m = 0.0f; s.settled = true; s.fadeFromM = 0.0f; *event = kOlTailEnded;
        } else if (settled || nowMs >= s.tailEndMs) {
            s.fading = true; s.settled = settled; s.fadeFromM = m; s.fadeStartMs = nowMs;
        }
    }
    if (s.tail && s.fading) {
        const unsigned long long t = nowMs - s.fadeStartMs;
        if (t >= kOverLedgeFadeMs) {
            s.tail = false; s.fading = false; m = 0.0f; *event = kOlTailEnded;
        } else {
            m = s.fadeFromM * (1.0f - static_cast<float>(t) / static_cast<float>(kOverLedgeFadeMs));
        }
    }
    s.lastM = m;
    s.prevState = active ? state : -1;
    return m;
}

// Round 18 (tester 2026-11-27: "when I move the joystick up ... it doesn't let me move for 1 or 2 seconds, even if a mantle is
// successful"; run 21: the game's climb-up from a hang, state 8, always lasts 2.24 s - its timer, +D0A240 8 -> 10 - and the origin is
// up at the top's height about 1.1 s in; the rest lifts it over the edge and sets it down). [BIN] State 10 is the end: the ledge
// grab's update (+D09E6C) calls the player's leave (+BB2390(player, 1, 0)) on the first update in it, as it does for the drop (16).
// So the climb-up can end early with the game's own set-state 10, once the feet (the origin) are at least kFastUpOverM over the top
// and at least kFastUpInM past the lip along the ledge's inward direction - standing on the top, clear of the edge. Only a climb-up
// from a hang (fromHang), only in state 8.
constexpr float kFastUpOverM = 0.05f;
constexpr float kFastUpInM = 0.35f;
inline bool FastClimbUpDone(bool on, bool fromHang, int state, const float origin[3], const float lip[3], const float into[3])
{
    if (!on || !fromHang || state != 8) { return false; }
    for (int i = 0; i < 3; ++i) { if (!(origin[i] == origin[i]) || !(lip[i] == lip[i])) { return false; } }
    const float il = std::sqrt(into[0] * into[0] + into[2] * into[2]);
    if (!(il > 1e-3f)) { return false; }
    const float in = ((origin[0] - lip[0]) * into[0] + (origin[2] - lip[2]) * into[2]) / il;
    return origin[1] >= lip[1] + kFastUpOverM && in >= kFastUpInM;
}

// Round 4 (tester 2026-11-28): "as soon as we release it (when I end a grab) the body (not my view) shortly moves to under the ledge
// and quickly comes back" / after a climb-up "my body (this time including my view) moves quickly downwards and then recovers". The
// holds (root, upper body, rigid view) let go the moment the ledge grab ends, but the game's hang / climb clips are still BLENDING OUT
// then: the hang clip's root sits toward the ledge (the clip is authored for the game's own hang spot, under the ledge, and the
// hand-driven body is not there), and the climb-up's end pose is a crouch that stands up (CAM AFTER, 2026-11-28 15:41: the game's head
// 107 cm over the origin at the end, 150 cm 750 ms later). So each hold keeps a TAIL after the grab ends:
//   - the body (root + upper body): kBodyTailMs (the blend-out; the hold cannot see the clip's root while it holds it);
//   - the rigid view: until the game's animated head is back within kViewTailGapM (vertical) of the standing eye point, at most
//     kViewTailMaxMs - then its usual ease-out.
// Round 6 (tester 2026-11-28: "freezing the body after the mantle animation was the wrong fix, now I often clip through my body or my
// head is above it, and it fixes after a second or so"). Run 17:45, climb-ups #1-#4: every tail that timed out had the game's head
// ~25-27 cm off the "standing eye point" while that head was at 156-158 cm over the feet - the remembered standing point was ~131 cm
// (the hang's own standing base was 168.8 cm). The rigid view held the camera there, ~37 cm low inside the body, until the 1.5 s
// time-out; and a tail that ended early (climb-up #2: 187 ms) could start again as the head rose away from that low point. So:
//   - the tail ends ONCE per grab end (ViewTail.doneFor) and also when the game's head has settled (its height still for
//     kViewSettleMs: the game's stand-up finished, short of the remembered point or not);
//   - the remembered standing point refuses dips (StandRefStep);
//   - after a climb-up the body is not held on (the game's stand-up is the body's own; the hold kept an upright torso over a crouched
//     pelvis): the body tail is only for a drop / launch from a hang (round 4's swing under the ledge).
constexpr unsigned long long kBodyTailMs    = 500;
constexpr unsigned long long kViewTailMaxMs = 1500;
constexpr float              kViewTailGapM  = 0.08f;
constexpr unsigned long long kViewSettleMs  = 200;     // the head's height still this long = settled
constexpr unsigned long long kViewSettleMinMs = 300;   // ... but never before this long after the grab
constexpr float              kViewSettleStepM = 0.01f; // "still" = moved under this per kViewSettleSampleMs
constexpr unsigned long long kViewSettleSampleMs = 100;
inline bool BodyHoldTail(bool hanging, unsigned long long endMs, unsigned long long nowMs, bool endedByClimbUp = false)
{
    if (hanging) { return true; }
    return !endedByClimbUp && endMs != 0 && nowMs >= endMs && nowMs - endMs < kBodyTailMs;
}
struct ViewTail {
    unsigned long long doneFor = 0;    // the grab end (endMs) whose tail has ended - it does not start again
    unsigned long long refMs = 0;      // the settle test's last sample
    float              refGap = 0.0f;
    unsigned long long stillMs = 0;    // how long the gap has been still (summed samples)
};
// animGapM = |the standing eye point's height - the game's animated head's| (body-local, m); NaN = unknown (holds to the time-out).
inline bool ViewHoldTail(ViewTail& t, bool hanging, unsigned long long endMs, unsigned long long nowMs, float animGapM)
{
    if (hanging) { t.doneFor = 0; t.refMs = 0; t.stillMs = 0; return true; }
    if (endMs == 0 || nowMs < endMs || t.doneFor == endMs) { return false; }
    bool end = nowMs - endMs >= kViewTailMaxMs || animGapM <= kViewTailGapM;
    if (!end && animGapM == animGapM) {
        if (t.refMs == 0 || t.refMs < endMs) {
            t.refMs = nowMs; t.refGap = animGapM; t.stillMs = 0;
        } else if (nowMs - t.refMs >= kViewSettleSampleMs) {
            const float step = animGapM - t.refGap;
            t.stillMs = (step <= kViewSettleStepM && step >= -kViewSettleStepM) ? t.stillMs + (nowMs - t.refMs) : 0;
            t.refMs = nowMs; t.refGap = animGapM;
            if (t.stillMs >= kViewSettleMs && nowMs - endMs >= kViewSettleMinMs) { end = true; }
        }
    }
    if (end) { t.doneFor = endMs; return false; }
    return true;
}
// The remembered standing eye height (body-local y, m), refreshed on each accepted standing camera call (boneblend). A value more than
// kStandDipM under the remembered one is taken only once it has held for kStandLowMs of accepted calls (a dip - a landing, a stumble -
// does not become "standing"; a real change still does). Returns the height to remember.
constexpr float              kStandDipM  = 0.05f;
constexpr unsigned long long kStandLowMs = 10000;
struct StandRef {
    bool               have = false;
    float              y = 0.0f;
    unsigned long long lowSinceMs = 0;
    unsigned long long lastLowMs = 0;   // a gap over kStandGapMs between low calls starts the count again
};
constexpr unsigned long long kStandGapMs = 500;
inline float StandRefStep(StandRef& s, float y, unsigned long long nowMs, bool* refused = nullptr)
{
    if (refused) { *refused = false; }
    if (!s.have || y >= s.y - kStandDipM) {
        s.have = true;
        s.y = y;
        s.lowSinceMs = 0;
        return s.y;
    }
    if (s.lowSinceMs == 0 || nowMs - s.lastLowMs > kStandGapMs) { s.lowSinceMs = nowMs; }
    s.lastLowMs = nowMs;
    if (nowMs - s.lowSinceMs >= kStandLowMs) {
        s.y = y;
        s.lowSinceMs = 0;
        return s.y;
    }
    if (refused) { *refused = true; }
    return s.y;
}

constexpr float kRigidRampS = 0.25f;
inline float RigidRampStep(float k, bool active, float dt)
{
    if (!(dt > 0.0f)) { return k; }
    if (dt > 0.1f) { dt = 0.1f; }   // a hitch eases on, never jumps
    k += (active ? dt : -dt) / kRigidRampS;
    return k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
}

constexpr float kViewSmoothTauS  = 0.12f;
constexpr float kViewSmoothMaxM  = 0.35f;
constexpr float kViewSmoothDoneM = 0.003f;
struct ViewSmooth {
    bool  on = false;
    float off[3] = {};
};
inline void ViewSmoothStep(ViewSmooth& s, bool active, const float raw[3], float dt, float corr[3])
{
    corr[0] = corr[1] = corr[2] = 0.0f;
    if (!std::isfinite(raw[0]) || !std::isfinite(raw[1]) || !std::isfinite(raw[2])) { s.on = false; return; }
    if (!s.on) {
        if (!active) { return; }
        s.on = true;
        for (int i = 0; i < 3; ++i) { s.off[i] = raw[i]; }
        return;
    }
    if (!(dt >= 0.0f)) { dt = 0.0f; }
    if (dt > 0.1f) { dt = 0.1f; }   // a hitch is one step, not a jump
    const float a = 1.0f - std::exp(-dt / kViewSmoothTauS);
    for (int i = 0; i < 3; ++i) { s.off[i] += (raw[i] - s.off[i]) * a; }
    float d[3] = { s.off[0] - raw[0], s.off[1] - raw[1], s.off[2] - raw[2] };
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l > kViewSmoothMaxM) {
        for (int i = 0; i < 3; ++i) { d[i] *= kViewSmoothMaxM / l; s.off[i] = raw[i] + d[i]; }
    }
    if (!active && l <= kViewSmoothDoneM) { s.on = false; return; }
    for (int i = 0; i < 3; ++i) { corr[i] = d[i]; }
}

// ===== FindLedge - the ledge around a hand, from rays =====
// Cast(from, to, hit) = the engine's raycast: true + hit filled when something is between from and to. A ray that STARTS inside
// geometry does not hit it (the engine's casts are one-sided), so every ray here starts on the player's side of the hand or above it.
// Run 4 (2026-11-26 19:58, "I can go through walls pretty easily"): 6 of 50 grabs climbed TOWARD the player - a face ray aimed back
// at the body starts 0.45 m beyond the hand, i.e. on the FAR side of the wall the hand reached through, and hit that wall's back
// face. kBackFace / kBlocked / kNoRoom are the three checks added for it (see FindLedge).
enum LedgeWhy : int { kLedgeOk = 0, kNoDir = 1, kNoFace = 2, kNoTop = 3, kTooFar = 4, kNotFlat = 5, kBackFace = 6, kBlocked = 7,
                      kNoRoom = 8, kWhyCount = 9 };

struct Ledge {
    float lip[3] = {};      // the edge point nearest the hand (on the face, at the top's height)
    float a[3] = {}, b[3] = {};   // the edge the game is handed: lip -+ half-width along the wall
    float into[3] = {};     // horizontal unit vector into the wall (the climb direction)
    float dist = -1.0f;     // hand to lip (m)
    int   rays = 0;         // casts made
};

// ===== P5 round 1: ladders, pipes and poles grabbed by hand (design §4.9 revised) =====
// [BIN] The game's ladder (LadderController, group 7) starts from the player's use target (player+0x14A0, valid while +0x1498 is
// set) when it is a LadderDI: walking into it (+BDD040, its group-7 switches at +BDD277 / +BDD734) or the use key (+BDCAE0, at
// +BDCF0F). LadderController reads the climbable through OwnerLadderInfo (vtable +172E178), which wraps the LadderDI: +0x10 its
// IControlObject (LadderDI+0x18)'s GetWorldPosition, +0x18 GetForwardVector, +0x20 GetLeftVector, +0x28 the kind (LadderDI+0x36C:
// 0 ONE_SIDE_LADDER, 1 PIPE, 2 ROPE, 3 UNDERWATER_ROPE), +0x30 the turn limits (+0x380 / +0x384), +0x38 the height over its world
// extents' bottom (GetExtentsInWorld: min at +0, max at +0xC). The attach (+D34340) stands the player 0.42 m from the position
// against the forward vector, so the position is the climbable's own line.
// Hand mode: the game's ladder never starts (RefuseLadderStart). A grip next to the climbable's own collider grabs it ANYWHERE on its
// surface (PoleSurfaceFind / PoleLedgeAt, round 2 - round 1 measured to its centre line and a fixed half-width, which fit no real
// ladder: tester "I think there aren't any thin poles"), as a hang of the ledge grab on a ledge made there: the same hand-driven
// body as ledges (P2), no ladder animation, no rungs. The line and extents (ReadPole) are logged and give the facing fallback.
struct Pole {
    float axis[3] = {};       // the climbable's position (x / z)
    float fwd[3] = {};        // its forward vector (the side the game stands the player on is -fwd)
    float yLo = 0.0f, yHi = 0.0f;   // its world extents' bottom / top
    int   kind = 0;           // 0 ladder, 1 pipe, 2 rope, 3 underwater rope
};

constexpr float kFaceBackM  = 0.45f;   // face rays start this far behind the hand (toward the player)
constexpr float kFacePastM  = 0.35f;   // ... and end this far past it
constexpr float kTopAboveM  = 0.50f;   // top rays start this far above the face hit
constexpr float kTopBelowM  = 0.10f;   // ... and end this far below it
constexpr float kHalfEdgeM  = 0.25f;   // half the edge handed to the game

// P5 round 2 (tester 2026-11-27: "a lot of objects the game codes as ladders/poles/pipes/whatever are not thin poles ... I want to
// be able to grab anywhere on the surface of the ladder/pole/whatever collider"; run 23: search rays DO hit the climbable - their
// owner is the LadderDI). The grab point is where a short ray around the hand meets the climbable's own collider: kSurfDirs
// directions (8 around, up, down), each from kSurfBackM behind the hand (so a hand pushed into it still finds its surface) to
// reachM past it; the hit on a climbable (isTarget after each cast) nearest the hand, within reachM of it, wins.
constexpr float kSurfBackM = 0.30f;
constexpr int   kSurfDirs  = 10;
template <class CastFn, class IsTargetFn>
inline bool PoleSurfaceFind(const float hand[3], float reachM, CastFn&& cast, IsTargetFn&& isTarget, float hitOut[3], float* distOut,
                            int* raysOut, int* bestRayOut = nullptr)   // bestRayOut: which cast (0-based) gave the hit (its owner)
{
    static const float k[kSurfDirs][3] = {
        { 1, 0, 0 }, { 0.7071f, 0, 0.7071f }, { 0, 0, 1 }, { -0.7071f, 0, 0.7071f },
        { -1, 0, 0 }, { -0.7071f, 0, -0.7071f }, { 0, 0, -1 }, { 0.7071f, 0, -0.7071f },
        { 0, 1, 0 }, { 0, -1, 0 } };
    bool found = false;
    float best = 1e9f;
    int rays = 0;
    for (int i = 0; i < kSurfDirs; ++i) {
        const float* d = k[i];
        const float from[3] = { hand[0] - d[0] * kSurfBackM, hand[1] - d[1] * kSurfBackM, hand[2] - d[2] * kSurfBackM };
        const float to[3] = { hand[0] + d[0] * reachM, hand[1] + d[1] * reachM, hand[2] + d[2] * reachM };
        float hit[3] = {};
        ++rays;
        if (!cast(from, to, hit) || !isTarget()) { continue; }
        const float dx = hit[0] - hand[0], dy = hit[1] - hand[1], dz = hit[2] - hand[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist <= reachM && dist < best) {
            best = dist;
            hitOut[0] = hit[0]; hitOut[1] = hit[1]; hitOut[2] = hit[2];
            if (bestRayOut) { *bestRayOut = i; }
            found = true;
        }
    }
    if (distOut) { *distOut = found ? best : -1.0f; }
    if (raysOut) { *raysOut = rays; }
    return found;
}
// The ledge handed to the ledge grab for a surface hold: lip = the surface point, into = from the feet to it, horizontal (the feet
// under it: fallbackFwd). False = no direction at all.
inline bool PoleLedgeAt(const float hit[3], const float feet[3], const float fallbackFwd[3], Ledge* out)
{
    float ix = hit[0] - feet[0], iz = hit[2] - feet[2];
    float il = std::sqrt(ix * ix + iz * iz);
    if (!(il > 0.05f)) {
        ix = fallbackFwd[0]; iz = fallbackFwd[2];
        il = std::sqrt(ix * ix + iz * iz);
        if (!(il > 1e-3f)) { return false; }
    }
    ix /= il; iz /= il;
    for (int i = 0; i < 3; ++i) { out->lip[i] = hit[i]; }
    out->into[0] = ix; out->into[1] = 0.0f; out->into[2] = iz;
    const float sx = -iz, sz = ix;
    out->a[0] = hit[0] - sx * kHalfEdgeM; out->a[1] = hit[1]; out->a[2] = hit[2] - sz * kHalfEdgeM;
    out->b[0] = hit[0] + sx * kHalfEdgeM; out->b[1] = hit[1]; out->b[2] = hit[2] + sz * kHalfEdgeM;
    out->rays = 0;
    return true;
}

// The game's own ladder start (its group-7 switch from the use target's walk-in / use key) is refused in hand mode when
// climb_ladder_by_hand is on; fromLadderSite = the switch's return address is one of those three calls.
inline bool RefuseLadderStart(bool handMode, bool ladderByHand, bool localPlayer, int group, bool fromLadderSite)
{
    return handMode && ladderByHand && localPlayer && group == 7 && fromLadderSite;
}

// P5 (tester 2026-11-27: "remove the ladder use floating UI when the ladder hand climbing setting is enabled"): the game's HUD
// interaction prompt (HudTriggerSlot::Update, its X "use" slot - and the mod's in-world copy of it, promptquad) for a ladder / pipe /
// rope (trigger class LadderDI) offers the game's own ladder climb, which hand mode refuses (RefuseLadderStart). While that refusal is
// live the slot is updated with NO trigger - the game's own "nothing to use" path - so neither prompt shows.
inline bool HideLadderPrompt(bool handMode, bool ladderByHand, bool ladderGateLive, bool triggerIsLadder)
{
    return handMode && ladderByHand && ladderGateLive && triggerIsLadder;
}
// P6 (tester 2026-11-27: "If the setting is on, are hud prompts hidden for ziplines? they should"): the zipline's prompt is the HUD's
// for the game's FakeZipLineTrigger (player+0x2CA8: +BD3C60 places it in front of the player each update a line is a candidate, so
// the HUD has a trigger to show "jump" for) - or a ZipLine itself. Hidden while ziplines are grabbed by hand (ServeZip is live).
inline bool HideZiplinePrompt(bool handMode, bool zipByHand, bool zipLive, bool triggerIsZipline)
{
    return handMode && zipByHand && zipLive && triggerIsZipline;
}
// P6 (tester 2026-11-28: "pressing A is still mounting me on the zipline"): the game's own zipline start - the jump near a line, its
// group-27 switch at +BD43BF inside +BD3C60 - is refused for the local player while ziplines are grabbed by hand (ServeZip's own
// switch goes through the trampoline, never this gate). The jump itself stays a jump.
inline bool RefuseZiplineJumpStart(bool handMode, bool zipByHand, bool zipLive, bool localPlayer, int group, bool fromJumpSite)
{
    return handMode && zipByHand && zipLive && localPlayer && group == 27 && fromJumpSite;
}
// Round 4 (2026-11-28): the game's whole zipline try-start (+BD3C60) is skipped by hand - a refused button mount still jumped the
// player toward the controller's stale attach point (~140 m up on the first line of a session). Same conditions, minus the site.
inline bool SkipZipTry(bool handMode, bool zipByHand, bool zipLive, bool localPlayer)
{
    return handMode && zipByHand && zipLive && localPlayer;
}

// P6 ZIPLINES BY HAND (design §4.10 "a grip above the head on a zipline starts ZipLineController"; static RE 2026-11-27, §8.18). The
// game keeps every zipline in one list (gamedll +1BA8D08, count +1BA8D10; each entity: A = start +0xA8, B = end +0xB4, D = unit A->B
// +0xC0, its model +0xD0, its name +0xD8). Its own start (+BD3C60) takes the line nearest the body, cuts the last kZipEndCutM off the
// far end, and needs jump held, the body in the air and the body's yaw within 30 deg of A->B. Ours: a grip whose ledge search found
// nothing, with the hand within reachM of a line's grabbable part, starts that line the game's own way (the controller's line record,
// its name, group 27) - no look, yaw or air test. The ride after that is the game's.
constexpr float kZipEndCutM = 4.5f;   // the game's own (+B19850, 0x1819274D4): the last 4.5 m of a line are never grabbed
struct ZipLine {
    float a[3] = {}, b[3] = {}, d[3] = {};
};
// The distance (m) from p to the line's grabbable segment [A, B - kZipEndCutM D]; closest = that point. -1 when the segment is empty
// (a line shorter than the cut).
inline float ZipDistance(const float p[3], const ZipLine& z, float closest[3])
{
    const float e[3] = { z.b[0] - kZipEndCutM * z.d[0], z.b[1] - kZipEndCutM * z.d[1], z.b[2] - kZipEndCutM * z.d[2] };
    const float s[3] = { e[0] - z.a[0], e[1] - z.a[1], e[2] - z.a[2] };
    const float ll = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
    if (!(ll > 1e-6f) || (s[0] * z.d[0] + s[1] * z.d[1] + s[2] * z.d[2]) <= 0.0f) { return -1.0f; }
    float t = ((p[0] - z.a[0]) * s[0] + (p[1] - z.a[1]) * s[1] + (p[2] - z.a[2]) * s[2]) / ll;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    for (int i = 0; i < 3; ++i) { closest[i] = z.a[i] + t * s[i]; }
    const float q[3] = { p[0] - closest[0], p[1] - closest[1], p[2] - closest[2] };
    return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
}
// A grip that found no ledge starts the zipline: hand mode with the setting, not already climbing / riding, the nearest grabbable
// line within reachM of the hand.
inline bool ZipGrabOk(bool handMode, bool zipByHand, bool climbing, bool riding, float dist, float reachM)
{
    return handMode && zipByHand && !climbing && !riding && dist >= 0.0f && dist <= reachM;
}

// P6 round 3 - HANDS ON THE ZIPLINE (tester 2026-11-28: "your hand snaps to the line and moving your hand moves your body, same way it
// is for ledges, you should also be able to launch yourself off in some direction, not just drop off"; "drop off is good, and the game
// sliding you down is good as well"). The game's ride keeps sliding; each holding hand's anchor is a point on the cable that rides
// with it (advance = how far the ride carried the body since the last poll, from the game thread), and P2's pull (anchor - hand)
// moves the body on top of the ride's own velocity. A second grip closing within reachM of the cable joins at the cable's nearest
// point. Opening every grip ends the ride: a launch when the hands were pulling fast (LaunchPick, as off a ledge), a plain drop
// otherwise. A hand pulled more than releaseM from its anchor lets go (a slip). The game ending the ride (the line's end, its own
// jump) ends the hold.
struct ZipHold {
    bool  on = false;
    bool  has[2] = {};
    float anchor[2][3] = {};
    ZipLine line;               // the whole cable (A -> B, no cut) for joins
};
struct ZipHoldOut {
    bool  drive = false;
    float err[3] = {};
    bool  joined[2] = {};
    bool  released[2] = {};     // this hand let go (opened or slipped)
    bool  slipped[2] = {};      // ...by distance, the grip still closed
    bool  end = false;          // no hand holds any more: end the ride
    bool  endOpened = false;    // ...because the grips opened (a launch may follow), not a slip
    bool  gameEnded = false;    // the game ended the ride itself
};
inline void ZipHoldStart(ZipHold& z, int hand, const float point[3], const ZipLine& line)
{
    z = ZipHold();
    z.on = true;
    z.line = line;
    if (hand < 0 || hand > 1) { return; }
    z.has[hand] = true;
    for (int i = 0; i < 3; ++i) { z.anchor[hand][i] = point[i]; }
}
// The nearest point of the whole cable [A, B] to p, and its distance (-1 = a degenerate line).
inline float ZipCableDistance(const float p[3], const ZipLine& z, float closest[3])
{
    const float s[3] = { z.b[0] - z.a[0], z.b[1] - z.a[1], z.b[2] - z.a[2] };
    const float ll = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
    if (!(ll > 1e-6f)) { return -1.0f; }
    float t = ((p[0] - z.a[0]) * s[0] + (p[1] - z.a[1]) * s[1] + (p[2] - z.a[2]) * s[2]) / ll;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    for (int i = 0; i < 3; ++i) { closest[i] = z.a[i] + t * s[i]; }
    const float q[3] = { p[0] - closest[0], p[1] - closest[1], p[2] - closest[2] };
    return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
}
inline ZipHoldOut ZipHoldStep(ZipHold& z, bool riding, const bool grip[2], const bool gripEdge[2], const bool handOk[2],
                              const float hand[2][3], const float advance[3], float reachM, float releaseM)
{
    ZipHoldOut o;
    if (!z.on) { return o; }
    if (!riding) {
        o.gameEnded = true;
        z = ZipHold();
        return o;
    }
    float sum[3] = {};
    int n = 0;
    for (int h = 0; h < 2; ++h) {
        if (z.has[h]) {
            for (int i = 0; i < 3; ++i) { z.anchor[h][i] += advance[i]; }
            if (!grip[h]) {
                z.has[h] = false;
                o.released[h] = true;
                continue;
            }
        } else if (grip[h] && gripEdge[h] && handOk[h]) {
            float c[3];
            const float d = ZipCableDistance(hand[h], z.line, c);
            if (d >= 0.0f && d <= reachM) {
                z.has[h] = true;
                o.joined[h] = true;
                for (int i = 0; i < 3; ++i) { z.anchor[h][i] = c[i]; }
            }
        }
        if (!z.has[h] || !handOk[h]) { continue; }
        const float e[3] = { z.anchor[h][0] - hand[h][0], z.anchor[h][1] - hand[h][1], z.anchor[h][2] - hand[h][2] };
        const float dist = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (releaseM > 0.0f && dist > releaseM) {
            z.has[h] = false;
            o.released[h] = true;
            o.slipped[h] = true;
            continue;
        }
        sum[0] += e[0]; sum[1] += e[1]; sum[2] += e[2];
        ++n;
    }
    if (n > 0) {
        o.drive = true;
        for (int i = 0; i < 3; ++i) { o.err[i] = sum[i] / static_cast<float>(n); }
    }
    if (!z.has[0] && !z.has[1]) {
        o.end = true;
        o.endOpened = !o.slipped[0] && !o.slipped[1];
        z = ZipHold();
    }
    return o;
}

// P6 climbing XP, option X1 (design §8.3, user decision 2026-11-25: the game's own numbers only). Every climb-up from a hand hang
// already pays: the game's set-state (+D0CD20) awards +CEA740(xpModule, EdgeGrab+0x1F8) whenever the state it leaves is 8, and both
// the stick's climb-up and round 18's cut-short (8 -> 10) go through it. What the game never sees is a LAUNCH that ends in a new
// grab (a dyno): the launch drops the ledge grab, the catch is a fresh start. That pays climb type 5 (RunnerXPClimbJump180, 10 / 20)
// through the same +CEA740, so the game's skills, night / exhaustion factors and its own anti-farm (1.3 m / 5 s) all apply.
// Ours on top: the catch counts only within kLaunchXpWindowMs of the launch and with the feet at least kLaunchXpMinM from where they
// left (a hop back onto the same hold pays nothing). One launch pays at most once.
constexpr int                kXpTypeJump180    = 5;
constexpr unsigned long long kLaunchXpWindowMs = 2500;
constexpr float              kLaunchXpMinM     = 1.0f;
struct LaunchXp {
    bool               armed = false;
    unsigned long long t0 = 0;
    float              feet0[3] = {};
};
inline void ArmLaunchXp(LaunchXp& x, unsigned long long now, const float feet[3], bool haveFeet)
{
    x.armed = haveFeet;
    x.t0 = now;
    for (int i = 0; i < 3; ++i) { x.feet0[i] = haveFeet ? feet[i] : 0.0f; }
}
// A new grab started (feet = the feet now). True = pay the launch catch. Always disarms; distOut = the feet's travel (m), -1 when no
// launch was armed or the window has passed.
inline bool LaunchCatchPays(LaunchXp& x, unsigned long long now, const float feet[3], float* distOut)
{
    *distOut = -1.0f;
    const bool armed = x.armed;
    x.armed = false;
    if (!armed || now < x.t0 || now - x.t0 > kLaunchXpWindowMs) { return false; }
    const float dx = feet[0] - x.feet0[0], dy = feet[1] - x.feet0[1], dz = feet[2] - x.feet0[2];
    *distOut = std::sqrt(dx * dx + dy * dy + dz * dz);
    return *distOut >= kLaunchXpMinM;
}
// Heights around the hand (a hand up to 40 cm above the top included). Round 3 (2026-11-26, run 2: "no wall face" for hands held
// above the head): the face rays go out in a FULL CIRCLE around the hand, every 30 deg, not a cone along feet -> hand - over the head
// that direction is noise. The face nearest the hand wins, so the extra directions only add hits that the cone missed.
constexpr float kFaceDy[6]  = { 0.15f, 0.05f, -0.05f, -0.15f, -0.27f, -0.40f };
constexpr int   kFaceDirs   = 12;
// A face counts only when it faces the body: the hit lies at least this far past the body's axis along the ray (run 4: a face hit
// from beyond it is the back of a wall the hand reached through).
constexpr float kBodyAheadM = 0.05f;
// Line of sight body -> face: from the body's axis, at the face's height clamped to these heights above the feet (chest-ish range).
constexpr float kSightLoM   = 0.30f;
constexpr float kSightHiM   = 1.70f;
constexpr float kSightGapM  = 0.03f;   // the sight ray stops this short of the face
// Room above the top: a horizontal ray this high above it, from in front of the face to past the lip, must be clear (a "top" found
// inside a wall's shell has the wall itself right above it).
constexpr float kRoomUpM    = 0.25f;
constexpr float kRoomFrontM = 0.35f;
constexpr float kRoomInM    = 0.15f;

// ROOM TO CLIMB UP (tester 2026-11-28: "sometimes you are able to mantle onto spaces where you don't fit ... I grabbed onto a closet
// inside a house ... then I mantled and went to the ceiling onto the upper floor" / "if I mantle onto a tight spot that requires me to
// be crouched, it'll send me to the normal standing height ... it should leave me crouched" / "don't start the raycast too close to
// the ledge"). Before a climb-up from a hang, the spot the body ends on - kRoomSpotInM past the lip, inward - is tested at standing
// height, then crouched: three vertical columns (the spot, and kRoomSideM either side along the edge), each an UP ray from
// kRoomFromM over the top to the height and a DOWN ray back (the engine's casts are one-sided: the up ray finds a ceiling's
// underside, the down ray the top of a slab or floor the up ray started inside). Below kRoomFromM nothing counts - debris lying on
// the top is stepped over. Stand fits = the game's climb-up; only crouched = the climb-up, then the crouch is held while the spot
// over the feet has no standing room (StandRoomAt); neither = no climb-up.
constexpr float kRoomSpotInM = 0.50f;
constexpr float kRoomSideM   = 0.20f;
constexpr float kRoomFromM   = 0.50f;
constexpr float kStandRoomM  = 1.80f;
constexpr float kCrouchRoomM = 1.15f;
enum ClimbRoom : int { kRoomStand = 0, kRoomCrouch = 1, kRoomNone = 2 };
// One column at (x, base y, z): clear from base + kRoomFromM to base + heightM. hitY = the lowest hit's height over base (or keeps).
template <class CastFn>
inline bool RoomColumn(float x, float baseY, float z, float heightM, CastFn&& cast, float* hitY)
{
    const float lo[3] = { x, baseY + kRoomFromM, z }, hi[3] = { x, baseY + heightM, z };
    float h[3] = {};
    bool clear = true;
    if (cast(lo, hi, h)) { clear = false; if (h[1] - baseY < *hitY) { *hitY = h[1] - baseY; } }
    if (cast(hi, lo, h)) { clear = false; if (h[1] - baseY < *hitY) { *hitY = h[1] - baseY; } }
    return clear;
}
// lip = the held ledge point (top height), into = into the wall (horizontal). hitY = the lowest thing found over the top (m; 99 = none).
template <class CastFn>
inline int ClimbUpRoom(const float lip[3], const float into[3], CastFn&& cast, float* hitY)
{
    *hitY = 99.0f;
    float nx = into[0], nz = into[2];
    const float n = std::sqrt(nx * nx + nz * nz);
    if (!(n > 1e-4f)) { return kRoomStand; }   // no direction: cannot test - the game decides as before
    nx /= n; nz /= n;
    const float sx = -nz, sz = nx;   // along the edge
    const float cx = lip[0] + nx * kRoomSpotInM, cz = lip[2] + nz * kRoomSpotInM;
    const float hs[2] = { kStandRoomM, kCrouchRoomM };
    for (int k = 0; k < 2; ++k) {
        bool ok = true;
        for (int c = -1; c <= 1 && ok; ++c) {
            ok = RoomColumn(cx + sx * kRoomSideM * c, lip[1], cz + sz * kRoomSideM * c, hs[k], cast, hitY);
        }
        if (ok) { return k == 0 ? kRoomStand : kRoomCrouch; }
    }
    return kRoomNone;
}
// The standing room over the feet (one column) - the held crouch after a crouched climb-up lets go once it is true.
template <class CastFn>
inline bool StandRoomAt(const float feet[3], CastFn&& cast)
{
    float hy = 99.0f;
    return RoomColumn(feet[0], feet[1], feet[2], kStandRoomM, cast, &hy);
}

template <class CastFn>
inline int FindLedge(const float hand[3], const float feet[3], float reachM, CastFn&& cast, Ledge* out)
{
    Ledge L;
    float fx = hand[0] - feet[0], fz = hand[2] - feet[2];
    const float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 0.05f) {   // the hand straight over the feet: the circle needs no direction, start it anywhere
        fx = 0.0f;
        fz = 1.0f;
    } else {
        fx /= fl;
        fz /= fl;
    }
    // 1. the face: a circle of horizontal rays through the hand, at several heights; the face nearest the hand wins
    float best = 1e9f, F[3] = {}, d[2] = {};
    bool face = false, backFace = false;
    for (float dy : kFaceDy) {
        for (int k = 0; k < kFaceDirs; ++k) {
            const float r = static_cast<float>(k) * (6.28318531f / kFaceDirs), c = std::cos(r), s = std::sin(r);
            const float dx = fx * c - fz * s, dz = fx * s + fz * c;
            const float from[3] = { hand[0] - dx * kFaceBackM, hand[1] + dy, hand[2] - dz * kFaceBackM };
            const float to[3] = { hand[0] + dx * kFacePastM, hand[1] + dy, hand[2] + dz * kFacePastM };
            float h[3];
            ++L.rays;
            if (!cast(from, to, h)) { continue; }
            // facing the body: the hit is past the body's axis along the ray (else it is the back of a wall - run 4)
            if ((h[0] - feet[0]) * dx + (h[2] - feet[2]) * dz < kBodyAheadM) {
                backFace = true;
                continue;
            }
            const float ex = h[0] - hand[0], ey = h[1] - hand[1], ez = h[2] - hand[2];
            const float dist = std::sqrt(ex * ex + ey * ey + ez * ez);
            if (dist < best) {
                best = dist;
                F[0] = h[0]; F[1] = h[1]; F[2] = h[2];
                d[0] = dx; d[1] = dz;
                face = true;
            }
        }
    }
    if (!face) { *out = L; return backFace ? kBackFace : kNoFace; }
    {   // nothing between the body and the face (a hand poked through a fence / window / thin wall reaches a ledge beyond it)
        float sy = F[1];
        if (sy < feet[1] + kSightLoM) { sy = feet[1] + kSightLoM; }
        if (sy > feet[1] + kSightHiM) { sy = feet[1] + kSightHiM; }
        const float from[3] = { feet[0], sy, feet[2] };
        const float to[3] = { F[0] - d[0] * kSightGapM, F[1], F[2] - d[1] * kSightGapM };
        float h[3];
        ++L.rays;
        if (cast(from, to, h)) { *out = L; return kBlocked; }
    }
    // 2. the wall's direction from two rays beside the face hit (a slanted wall is not the ray's direction)
    float nx = -d[0], nz = -d[1];   // outward (toward the player); refined below
    {
        const float px = -d[1], pz = d[0];   // horizontal perpendicular of the ray
        float h1[3], h2[3];
        const float from1[3] = { F[0] - d[0] * kFaceBackM + px * 0.08f, F[1], F[2] - d[1] * kFaceBackM + pz * 0.08f };
        const float to1[3]   = { F[0] + d[0] * 0.20f + px * 0.08f, F[1], F[2] + d[1] * 0.20f + pz * 0.08f };
        const float from2[3] = { F[0] - d[0] * kFaceBackM - px * 0.08f, F[1], F[2] - d[1] * kFaceBackM - pz * 0.08f };
        const float to2[3]   = { F[0] + d[0] * 0.20f - px * 0.08f, F[1], F[2] + d[1] * 0.20f - pz * 0.08f };
        L.rays += 2;
        if (cast(from1, to1, h1) && cast(from2, to2, h2)) {
            const float tx = h1[0] - h2[0], tz = h1[2] - h2[2];
            const float tl = std::sqrt(tx * tx + tz * tz);
            if (tl > 0.02f) {
                float cx = tz / tl, cz = -tx / tl;           // up x tangent (horizontal)
                if (cx * -d[0] + cz * -d[1] < 0.0f) { cx = -cx; cz = -cz; }   // face the player
                if (cx * -d[0] + cz * -d[1] > 0.3f) { nx = cx; nz = cz; }      // ignore a nonsense reading (> ~73 deg off)
            }
        }
    }
    // 3. the top: rays down just inside the wall, from above the face hit
    bool top = false;
    float topY = 0.0f;
    // Run 5 (2026-11-26 21:01, "thin fences (< 10 cm) are not detected at all"): the first inset was 6 cm, so on a fence thinner than
    // that both top rays fell past its back face (95 "no top" in that run). Now from 2 cm, nearest first.
    static constexpr float kInsets[4] = { 0.02f, 0.05f, 0.10f, 0.15f };
    for (float inset : kInsets) {
        const float from[3] = { F[0] - nx * inset, F[1] + kTopAboveM, F[2] - nz * inset };
        const float to[3]   = { F[0] - nx * inset, F[1] - kTopBelowM, F[2] - nz * inset };
        float h[3];
        ++L.rays;
        if (cast(from, to, h)) {
            topY = h[1];
            top = true;
            break;
        }
    }
    if (!top) { *out = L; return kNoTop; }
    if (topY < F[1] - kTopBelowM + 1e-3f) { *out = L; return kNotFlat; }   // hit at the ray's very end: a slope, not a top
    {   // room above the top (a top ray that starts inside a thick wall finds a "top" inside it; the wall is then right above)
        const float ry = topY + kRoomUpM;
        const float from[3] = { F[0] + nx * kRoomFrontM, ry, F[2] + nz * kRoomFrontM };
        const float to[3] = { F[0] - nx * kRoomInM, ry, F[2] - nz * kRoomInM };
        float h[3];
        ++L.rays;
        if (cast(from, to, h)) { *out = L; return kNoRoom; }
    }
    L.lip[0] = F[0];
    L.lip[1] = topY;
    L.lip[2] = F[2];
    const float lx = L.lip[0] - hand[0], ly = L.lip[1] - hand[1], lz = L.lip[2] - hand[2];
    L.dist = std::sqrt(lx * lx + ly * ly + lz * lz);
    const float tx = -nz, tz = nx;   // along the wall
    L.a[0] = L.lip[0] - tx * kHalfEdgeM; L.a[1] = topY; L.a[2] = L.lip[2] - tz * kHalfEdgeM;
    L.b[0] = L.lip[0] + tx * kHalfEdgeM; L.b[1] = topY; L.b[2] = L.lip[2] + tz * kHalfEdgeM;
    L.into[0] = -nx; L.into[1] = 0.0f; L.into[2] = -nz;
    *out = L;
    return L.dist <= reachM ? kLedgeOk : kTooFar;
}

// ===== P2: the body follows the hands while hanging (design §4.4, §8.1 option (b), §8.4) =====
// The ledge grab's hold states are gravity-free and move the body at exactly the velocity the player walk hands the physics (§8.4:
// measured = wanted). In them the game hands zero; P2 hands a velocity of ours instead (walklook's +ED2040 detour). Model: each hand
// on the ledge has a world ANCHOR, latched where the hand was when the hold began (not where the grab first touched - the game's own
// grab animation, state 0, moves the body first). The body moves so the hands go back to their anchors: pull the hands down = the
// body rises; reach up = it lowers. Moving sideways is hand over hand (a hand only holds where a ledge is - round 7).
// The hold states P2 drives: 1 hang and its brief sub-states 2 / 3 / 7, and 14 (a blocked shimmy). NOT 11 / 12 / 13 (the game's own
// shimmies move the body themselves), 0 (the grab), 4 / 5 / 6 (jumps), 8 (the climb-up).
inline bool IsHoldState(int s)
{
    return s == 1 || s == 2 || s == 3 || s == 7 || s == 14;
}

constexpr float kPullTauS = 0.08f;   // PullVelocity's time constant: ~80 ms to close most of the gap (stable at 1-2 frames of lag)
// P2 round 2 (tester 2026-11-26): "does the hand snap to the ledge? ... otherwise hands would end up grabbing in empty space" and
// "if the hand moves away from its anchor while grabbing, that hand should stop grabbing, not move the anchor". So:
//   - the anchor is the LEDGE POINT the hand's search found (the lip nearest the hand, Ledge::lip), not where the hand was. The body
//     then moves until the hand is on the ledge - up to climb_reach_m at the hold's start;
//   - a hand farther than releaseM from its anchor RELEASES (it lets go of the ledge; the grip must reopen and close to grab again).
//     No re-anchoring.
// P2 round 3 (run 7: "sometimes when I grabbed it almost immediately let go"): the game's grab animation (state 0) moves the body
// before the hold begins, so a hand 0.18 m from its ledge point at the grab was 0.51-0.57 m from it when the hold began - released
// on the first poll. A hand may only release once it has SETTLED on its point (within kSettleM), or kSettleMaxMs after latching if
// the body never brings it there (blocked).
constexpr float              kSettleM     = 0.12f;
constexpr unsigned long long kSettleMaxMs = 1000;

struct Pull {
    float anchor[2][3] = {};
    bool  has[2] = {};
    bool  settled[2] = {};
    unsigned long long latchMs[2] = {};
};

struct PullOut {
    bool  drive = false;    // at least one anchored hand: the body follows err
    float err[3] = {};      // mean of (anchor - hand) over the anchored hands, m: where the body must go for the hands to be back
    bool  latched[2] = {};  // this hand's anchor was set this poll
    bool  released[2] = {}; // this hand is too far from its anchor: it lets go (the caller clears its hold)
    float dist[2] = { -1.0f, -1.0f };   // hand to anchor (m), -1 = not anchored / no pose
};

// One pad poll. held = the machine's "on a ledge" bits; holdState = the ledge grab is in a hold state (fresh publish). A hand's
// anchor goes when it stops holding or the game leaves the hold states (its own shimmy / climb-up / jump moved the body: re-latch
// after - on the same ledge point). hand = game-world positions, valid where handOk; lip = the ledge point each holding hand's
// search found (game world), valid where haveLip - without it the hand's own position is the anchor (never expected: a hand only
// holds once its search found a ledge). releaseM <= 0 = never release.
inline PullOut PullStep(Pull& p, unsigned held, bool holdState, const bool handOk[2], const float hand[2][3],
                        const float lip[2][3], const bool haveLip[2], float releaseM, unsigned long long nowMs)
{
    PullOut o;
    float sum[3] = {};
    int n = 0;
    for (int h = 0; h < 2; ++h) {
        if (!(held & (1u << h)) || !holdState) {
            p.has[h] = false;
            continue;
        }
        if (!handOk[h]) { continue; }   // a lost pose keeps the anchor but adds nothing this poll
        if (!p.has[h]) {
            const float* src = haveLip[h] ? lip[h] : hand[h];
            for (int i = 0; i < 3; ++i) { p.anchor[h][i] = src[i]; }
            p.has[h] = true;
            p.settled[h] = false;
            p.latchMs[h] = nowMs;
            o.latched[h] = true;
        }
        const float e[3] = { p.anchor[h][0] - hand[h][0], p.anchor[h][1] - hand[h][1], p.anchor[h][2] - hand[h][2] };
        o.dist[h] = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (!p.settled[h] && (o.dist[h] <= kSettleM || nowMs - p.latchMs[h] > kSettleMaxMs)) { p.settled[h] = true; }
        if (p.settled[h] && releaseM > 0.0f && o.dist[h] > releaseM) {
            p.has[h] = false;
            o.released[h] = true;
            continue;
        }
        sum[0] += e[0]; sum[1] += e[1]; sum[2] += e[2];
        ++n;
    }
    if (n > 0) {
        o.drive = true;
        for (int i = 0; i < 3; ++i) { o.err[i] = sum[i] / static_cast<float>(n); }
    }
    return o;
}

// The velocity that closes err: err / tauS, at most maxMps (a proportional follow - the hands' pose lags the body by a frame or two,
// so tau stays well above a frame). Non-finite err = zero.
inline void PullVelocity(const float err[3], float tauS, float maxMps, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!std::isfinite(err[0]) || !std::isfinite(err[1]) || !std::isfinite(err[2]) || tauS <= 0.0f) { return; }
    float v[3] = { err[0] / tauS, err[1] / tauS, err[2] / tauS };
    const float s = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (maxMps > 0.0f && s > maxMps) {
        const float k = maxMps / s;
        v[0] *= k; v[1] *= k; v[2] *= k;
    }
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
}

// NO GRAB ANIMATION (tester 2026-11-28: "the 'go to grab mode' animation often makes me clip through walls or moves me in a weird
// way. Could you get rid of the animation completely and just move me towards the grab position over at a reasonable speed", then
// "make the speed configurable"). The grab (state 0) is the game's root-motion swing onto the ledge; it ends on a timer with
// set-state 8 or 1 (+D0A240 at +D0A4B9, nothing else done there). A hand grab that is due to HANG (the start armed the 0 -> 1 hang;
// still within its window) is set to 1 on the first update it is seen in 0. The hold's own pull (PullStep / MotionStep) then moves the
// body until the hand is on its ledge point - with the walk's collisions, so no wall is crossed - capped at GrabGlideCap.
inline bool SkipGrabAnim(bool enabled, bool handMode, bool ours, bool due, bool active, int state)
{
    return enabled && handMode && ours && due && active && state == 0;
}
// The glide: while any anchored hand has not yet SETTLED on its ledge point (a new grab), the pull is capped at grabMps (at least
// kGrabGlideMinMps); after that, climb_pull_max_mps. grabMps <= 0 / NaN = no glide cap.
constexpr float kGrabGlideMinMps = 0.5f;
inline bool GrabGliding(const Pull& p)
{
    for (int h = 0; h < 2; ++h) { if (p.has[h] && !p.settled[h]) { return true; } }
    return false;
}
inline float GrabGlideCap(bool gliding, float grabMps, float pullMaxMps)
{
    if (!gliding || !(grabMps > 0.0f)) { return pullMaxMps; }
    return grabMps < kGrabGlideMinMps ? kGrabGlideMinMps : grabMps;
}

// P4 round 2 (tester 2026-11-27: "if you grab 2 ledges at once, the ledge-follow and mantle logic should just use the last one the
// player grabbed"): which hand's ledge is "the ledge held" for the climb-up height, the stale-ledge test and the climb on the held
// ledge - the last hand that grabbed (lastGrab, 0 = left, 1 = right, -1 = none yet) while it is anchored on a ledge point, else the
// other anchored one, else -1.
inline int LedgeHand(const Pull& p, const bool haveLip[2], int lastGrab)
{
    const auto on = [&](int h) { return h >= 0 && h <= 1 && p.has[h] && haveLip[h]; };
    if (on(lastGrab)) { return lastGrab; }
    if (on(1 - (lastGrab == 1 ? 1 : 0))) { return 1 - (lastGrab == 1 ? 1 : 0); }
    for (int h = 0; h < 2; ++h) { if (on(h)) { return h; } }
    return -1;
}

// "The ledge held" as one point: useLast (climb_ledge_last, tester 2026-11-27: "make it a setting, whether to use the last one or an
// average") = LedgeHand's ledge; otherwise the mean of every anchored hand's ledge point and inward direction. into = each hand's
// ledge inward direction. topY = the point's height; intoOut is horizontal. False = no anchored hand with a ledge point.
inline bool LedgeHeld(const Pull& p, const bool haveLip[2], const float into[2][3], int lastGrab, bool useLast, float* topY,
                      float mid[3], float intoOut[3])
{
    int n = 0;
    float m[3] = {}, d[3] = {};
    for (int h = 0; h < 2; ++h) {
        if (useLast && h != LedgeHand(p, haveLip, lastGrab)) { continue; }
        if (!p.has[h] || !haveLip[h]) { continue; }
        for (int i = 0; i < 3; ++i) { m[i] += p.anchor[h][i]; d[i] += into[h][i]; }
        ++n;
    }
    if (n == 0) { return false; }
    for (int i = 0; i < 3; ++i) { mid[i] = m[i] / static_cast<float>(n); intoOut[i] = d[i] / static_cast<float>(n); }
    intoOut[1] = 0.0f;
    *topY = mid[1];
    return true;
}

// P4 (design §4.8, climb_pin_hands): the shift that DRAWS a holding hand on its ledge point = anchor - hand, only for an anchored hand
// with a pose, and never farther than releaseM (a larger gap is about to release - drawing the arm stretched to it would be wrong).
inline bool PinShift(bool enabled, const Pull& p, int h, bool handOk, float dist, float releaseM, const float hand[3], float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!enabled || h < 0 || h > 1 || !p.has[h] || !handOk || dist < 0.0f || dist > releaseM) { return false; }
    for (int i = 0; i < 3; ++i) { out[i] = p.anchor[h][i] - hand[i]; }
    return true;
}

// ===== P3: mantle and launch (design §4.5 / §4.6, §8.14; round 2 2026-11-27) =====
//   - MANTLE = the GAME'S OWN climb-up (round 2, tester: "the new mantle implementation is not great, let's stick to the normal game
//     animation"). The stick forward in a hang already climbs up the game's way (climb_mantle_stick, default on). Optionally
//     (climb_mantle_m > 0, default OFF) pulling the body up until the highest anchored ledge point is at most that far above the feet
//     asks the game for the same climb-up (set-state 8) - MotionStep's climbUp. Round 1's own lift over the lip is gone.
//   - LAUNCH: the last hand lets go while the hands were moving the body fast (the largest velocity sent in the last kLaunchWindowMs
//     is at least climb_launch_min_mps, and not mostly downward). The body keeps that velocity x climb_launch_scale (the boost), a
//     mostly-upward launch is turned toward straight up by climb_launch_up_assist (UpAssist), and the caps apply.
//     P4 round 5 (tester 2026-11-27, after run 14: "everything can be fixed cleanly by just dropping out of the hanging state entirely
//     as soon as the player is no longer hanging ... If there are issues with maintaining movement speed when changing states, that
//     is the issue we need to address"): the launch is the game's own DROP at once - the body is the game's walking / falling body
//     from the first moment, with its own collisions, ledge grabs (a hand that closes on a ledge mid-air starts a grab as from the
//     ground: RoomscaleOnFoot is a controller-group test, falling included) and stick moves. Rounds 2-4 flew our own ballistic arc
//     INSIDE the hang state instead (no walk physics: "going through some colliders, never stepping over"; the stick mid-air reached
//     the hang's own reach-up, state 9: "rises up a bit but not mantle").
//     The speed (run 10: IPhBody::SetLinearVel once after the drop kept the vertical but ~none of the horizontal - 2.98 m/s sent,
//     0.36 kept): the walk body's horizontal velocity is set every update from the walk's WANTED vector (the +ED2040 call, the stick's
//     move), so a one-off velocity is overwritten on the next update. The CARRY: from the drop until it lands, every player walk call
//     hands the walk the body's horizontal momentum (CarryStep: climb_air_momentum, collisions, air drag); the vertical is
//     SetLinearVel's, and gravity is the game's.
//   - Anything else that lets go is the plain drop, as before.
constexpr float              kGravity         = 9.81f;
constexpr float              kUpAssistMaxDeg  = 45.0f;   // launches within this of straight up are "up" jumps (the assist applies)
constexpr float              kUpAssistFadeDeg = 60.0f;   // ...fading to none here, so a push-off is never bent
// Balance (tester 2026-11-27: "make sure that the launch speed can't be higher than the default game one"): the caps are the game's
// own defaults from data/skills/default_levels.xml - JumpMaxHeight 1.01 m (property 670) and WallJumpBackSpeed 3.0 m/s (property
// 492, the push-off from a wall). Up: sqrt(2 g 1.01) = 4.45 m/s, and the game's gravity takes it from there, so the rise never
// passes 1.01 m whatever climb_launch_scale is. Skills that raise the game's own values are not followed (never above the default).
constexpr float              kGameJumpMaxHeightM = 1.01f;
constexpr float              kLaunchMaxUpMps  = 4.4516f;   // sqrt(2 x 9.81 x 1.01)
constexpr float              kLaunchMaxSideMps = 3.0f;     // WallJumpBackSpeed
constexpr unsigned long long kLaunchWindowMs  = 150;
constexpr int                kSentRing        = 16;

// P5 / P6 look-aimed JUMP BUTTON from any hang, ledge or pole (design §4.9 jumps; tester 2026-11-27: "pressing the jump button should
// make you jump at full force (so the flatscreen game's default) from wherever you are hanging towards wherever you are looking. This
// is both for ledges and poles. If you are looking towards the wall/pole, it should jump you upwards ... else it should jump you
// wherever you are looking"). The game's hang input handler (+D19BA0, action 0xB) asks 9 (+D19D50) or 4 (+D19D41) for the jump
// button - and for the stick pushed up (P4 round 20), so an ask with the stick freshly forward stays the stick's.
// The jump: only the look's YAW decides (never the pitch), measured against the hang's into-the-wall direction (the ledge grab's
// target direction - the wall's, or feet -> the pole's surface): under sideDeg = straight up at the game's full jump
// (kLaunchMaxUpMps, JumpMaxHeight 1.01 m); at or past it = the same full jump up plus WallJumpBackSpeed (kLaunchMaxSideMps)
// horizontally along the look. No look (or no wall direction) = straight up.
inline bool JumpButtonAsk(bool handMode, bool jumpByLook, int from, int to, bool fromJumpCall, bool stickFwd)
{
    const bool hold = from == 1 || from == 2 || from == 3 || from == 7 || from == 14;   // IsHoldState (defined below)
    return handMode && jumpByLook && hold && (to == 4 || to == 9) && fromJumpCall && !stickFwd;
}
// The angle (deg, 0..180) between the look's horizontal direction and into-the-wall; -1 when either has no horizontal length.
inline float LookYawFromWallDeg(const float look[3], const float into[3])
{
    const float ll = std::sqrt(look[0] * look[0] + look[2] * look[2]), il = std::sqrt(into[0] * into[0] + into[2] * into[2]);
    if (!(ll > 1e-3f) || !(il > 1e-3f)) { return -1.0f; }
    float c = (look[0] * into[0] + look[2] * into[2]) / (ll * il);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return std::acos(c) * 57.29578f;
}
// v = the jump's velocity (world, m/s). Returns true for a push-off along the look, false for straight up. yawOut = the measured
// angle (-1 = unknown: straight up).
inline bool LookJumpVelocity(const float look[3], bool haveLook, const float into[3], float sideDeg, float v[3], float* yawOut)
{
    const float yaw = haveLook ? LookYawFromWallDeg(look, into) : -1.0f;
    *yawOut = yaw;
    v[0] = 0.0f; v[1] = kLaunchMaxUpMps; v[2] = 0.0f;
    if (yaw < 0.0f || yaw < sideDeg) { return false; }
    const float ll = std::sqrt(look[0] * look[0] + look[2] * look[2]);
    v[0] = look[0] / ll * kLaunchMaxSideMps;
    v[2] = look[2] / ll * kLaunchMaxSideMps;
    return true;
}
// Round 21 (tester 2026-11-28: "When pressing A to jump on from a pole or ladder, the first jump in the game, regardless of where I'm
// looking, is always upwards"): the "toward the climbable" direction the look is measured against. On a pole / ladder it is measured
// NOW, from the feet to where the hands hold (the anchored points' mean), not the ledge made at the grab (feet -> the surface then; by
// the jump the body has moved round and up the pole by hand, and the grab's own fallback is the climbable's fixed forward vector when
// the feet were under the hand). Ledges keep the wall's direction. Returns the source: 2 = the hold, 1 = the wall / grab direction,
// 0 = none (straight up).
constexpr float kJumpIntoMinM = 0.05f;
inline int JumpInto(bool pole, const float feet[3], bool haveFeet, const float holdMid[3], bool haveHold, const float wallDir[3],
                    bool haveWall, float out[3])
{
    if (pole && haveFeet && haveHold) {
        const float x = holdMid[0] - feet[0], z = holdMid[2] - feet[2];
        const float l = std::sqrt(x * x + z * z);
        if (l > kJumpIntoMinM) {
            out[0] = x / l; out[1] = 0.0f; out[2] = z / l;
            return 2;
        }
    }
    if (haveWall) {
        for (int i = 0; i < 3; ++i) { out[i] = wallDir[i]; }
        return 1;
    }
    out[0] = out[1] = out[2] = 0.0f;
    return 0;
}

// The headset's forward (-Z of its orientation q = x, y, z, w, OpenXR tracking space) in the game world: the same yaw the hands get
// (looting.cpp HandWorld: off' = (x c + z s, y, -x s + z c), s negated with AimMirrorYaw), rel = CamHookGetOpenXrToGameYaw.
inline void HeadForwardGame(const float q[4], float rel, bool mirror, float out[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = -2.0f * (x * z + w * y), fy = -2.0f * (y * z - w * x), fz = -(1.0f - 2.0f * (x * x + y * y));
    const float c = std::cos(rel);
    float s = std::sin(rel);
    if (mirror) { s = -s; }
    out[0] = fx * c + fz * s;
    out[1] = fy;
    out[2] = -fx * s + fz * c;
}

// What the pad thread last published about the anchored hands (climbhand.cpp, every poll).
struct Hold {
    bool               drive = false;   // at least one anchored hand (PullOut::drive)
    float              err[3] = {};     // PullOut::err
    unsigned long long driveMs = 0;     // when drive was last published true
    bool               haveTop = false; // at least one anchored hand with a ledge point
    float              topY = 0.0f;     // the highest anchored ledge point's height
    float              mid[3] = {};     // the mean anchored ledge point
    float              into[3] = {};    // the mean inward (into the wall) direction of the anchored hands' ledges, horizontal
    bool               gliding = false; // a newly anchored hand has not settled yet (GrabGliding): the pull is capped at the glide speed
};

struct Motion {
    float              ring[kSentRing][3] = {};   // the velocities sent, newest at ringHead - 1
    unsigned long long ringMs[kSentRing] = {};
    int                ringN = 0, ringHead = 0;
    unsigned long long upSinceMs = 0;    // since when the climb-up height is reached AND the body moves slowly (0 = not)
};

// P3 round 4 (tester 2026-11-27: "are you sure that with pull up to climb up, moving the controllers down fast to do a jump won't
// cause a climb up?" - it could: a big jump flick crosses climb_mantle_m mid-pull, the climb-up took the hang, and the release no
// longer launched). The climb-up now waits until the body has been at that height AND slower than slowMps (the launch speed) for
// kMantleDwellMs: a flick is still fast when the hands open (a launch); a pull-up held at the top climbs up.
constexpr unsigned long long kMantleDwellMs = 200;

struct MotionOut {
    bool  send = false;      // hand v to the walk instead of the game's vector
    float v[3] = {};
    bool  climbUp = false;   // climb_mantle_m: pulled up far enough - ask the game for its own climb-up (set-state 8)
};

inline bool MantleDue(bool drive, bool haveTop, float topY, float feetY, float mantleM)
{
    return drive && haveTop && mantleM > 0.0f && std::isfinite(topY) && std::isfinite(feetY) && topY - feetY <= mantleM;
}

// The CARRY (P4 round 5 / 6, above): after a launch's drop, the walk body's horizontal speed, on each player walk call (the game's
// own walking / falling, the ledge grab no longer running). h = the launch's horizontal momentum the carry holds.
//   - COLLISIONS (round 6, tester: "I don't think you are taking collisions into account"): from kCarryArmMs on (the walk body has
//     taken over by then), h follows what the body really did - the horizontal velocity measured from the feet between two calls
//     (momentum: h = that; stick mode: h loses what the body lost against what it was sent). A wall that stopped the body stops that
//     part of h, a slide along it keeps only the slide; nothing is pushed into a wall again.
//     Calls where the feet did not move at all (no physics step between them) are no momentum sample.
//   - momentum = climb_air_momentum on (round 6: "a setting to completely remove the game overriding the speed mid air. Let physics
//     do its thing, though you'll probably need to code some sort of air drag"): out horizontal = h plus the stick's push
//     (AirControlPush, round 7); h loses airDrag x h per second (linear drag, exp(-airDrag dt) per call). Off = round 5's mix: h + the stick's own
//     (gameW), never faster than the larger of the two (the stick steers but never stacks on the launch).
//   - It ends when the body LANDS (kCeLanded). P4 round 8: by the walk's own airborne flag (air, FeetVel's note below): once it has
//     said "in the air" (sawAir), the first call it says "not" ends the carry. A launch the walk still holds as grounded for its
//     first moments (run 15: flags 0x0001 for ~110 ms after a drop near the ground) waits for the flag; when the flag cannot be read
//     (air < 0) or has not been set yet, round 6's ballistic test stands in (after armMs the ballistic vertical speed v0 - g t says
//     falling by more than 0.5 m/s while the measured one is above -kCarryStillMps and kCarryLandMps above it, kCarryLandCalls calls
//     in a row). Round 5's 2 s limit is gone (round 6, tester: "nor do I like the 2 seconds limit"); kCarryMaxMs is only a safety net
//     for a landing never seen.
// The vertical is always the game's (gameW's y): gravity, the rise handed over at the drop.
// Run 15 [walk probe, CLIMBPROBE lines]: in the air the walk moves the body at exactly the horizontal vector it is handed (wanted
// +0xA70 = measured, call after call) - the engine's air factor did not scale it - so what CarryStep hands IS the body's horizontal
// velocity in the air.
constexpr unsigned long long kCarryMaxMs     = 10000;
constexpr unsigned long long kCarryArmMs     = 120;
constexpr float              kCarryLandMps   = 1.5f;
constexpr float              kCarryStillMps  = 0.3f;
constexpr int                kCarryLandCalls = 3;
enum CarryEnd : int { kCeNone = 0, kCeLanded, kCeTimeout, kCeCount };
struct Carry {
    bool               on = false;
    unsigned long long t0 = 0;
    unsigned long long armMs = kCarryArmMs;   // a launch: kCarryArmMs; air time the game started: 0 (the walk already holds the body)
    float              v[3] = {};        // the launch velocity (world, m/s)
    float              h[2] = {};        // the horizontal momentum held now (x, z)
    float              prev[3] = {};     // the feet on the last call
    bool               havePrev = false;
    int                still = 0;        // calls in a row that looked like a landing
    float              last[2] = {};     // the horizontal handed to the walk on the last call (x, z)
    bool               haveLast = false;
    bool               sawAir = false;   // the walk's flag said "in the air" during this carry
    // P4 round 10: the recent calls (oldest first) for the windowed measurement - the time since the start (s), the feet (x, z) and
    // the horizontal handed on that call (x, z)
    static constexpr int kWin = 32;
    float              wt[kWin] = {};
    float              wp[kWin][2] = {};
    float              wh[kWin][2] = {};
    int                wn = 0;
    float              tacc = 0.0f;
};
// P4 round 10 (run 17, tester 2026-11-27: "now stick forward actually slows down air momentum considerably"): walk calls and physics
// steps do not line up (a call 28 ms after the last can hold one 16 ms step), so the speed measured call to call swings above and
// below the real one; round 9's cap kept every low reading and clipped every high one - the momentum ratcheted down (run 17: 5.95 ->
// 2.75 m/s in 1 s where air drag 0.2 leaves 4.9). Momentum mode now measures over up to the last kMeasMaxS of calls (at least
// kMeasMinS) and
// compares that with what was handed over the same time: in free flight they agree and h is left alone (only drag and the stick
// change it); when the body fell short (a wall, a ledge) - slower than kMeasKeep of what it was handed, or turned more than
// kMeasCos away - h becomes the measured velocity, never faster than the last call handed.
constexpr float kMeasMinS = 0.06f;
constexpr float kMeasMaxS = 0.25f;
constexpr float kMeasKeep = 0.85f;
constexpr float kMeasCos  = 0.985f;   // ~10 deg
inline void StartCarry(Carry& c, const float v[3], unsigned long long nowMs, unsigned long long armMs = kCarryArmMs)
{
    c.on = true;
    c.t0 = nowMs;
    c.armMs = armMs;
    c.v[0] = v[0]; c.v[1] = v[1]; c.v[2] = v[2];
    c.h[0] = v[0]; c.h[1] = v[2];
    c.havePrev = false;
    c.still = 0;
    c.haveLast = false;
    c.sawAir = false;
    c.wn = 0;
    c.tacc = 0.0f;
}
// The windowed measurement (above): false while fewer than kMeasMinS of calls are held. m = the feet's horizontal velocity over the
// window, a = the time-weighted mean of what was handed over it (a handed vector moves the body until the next call).
inline bool CarryWindow(const Carry& c, const float feet[3], float tNow, float m[2], float a[2])
{
    // the OLDEST sample within kMeasMaxS: one physics step more or less in the window is then at most 16 / 250 = 6 % (a 60 ms
    // window could be 27 % off and read as a wall)
    int j = -1;
    for (int i = 0; i < c.wn; ++i) {
        if (tNow - c.wt[i] <= kMeasMaxS) { j = i; break; }
    }
    if (j < 0 || tNow - c.wt[j] < kMeasMinS) { return false; }
    const float span = tNow - c.wt[j];
    m[0] = (feet[0] - c.wp[j][0]) / span;
    m[1] = (feet[2] - c.wp[j][1]) / span;
    a[0] = a[1] = 0.0f;
    for (int i = j; i < c.wn; ++i) {
        const float t1 = i + 1 < c.wn ? c.wt[i + 1] : tNow;
        a[0] += c.wh[i][0] * (t1 - c.wt[i]);
        a[1] += c.wh[i][1] * (t1 - c.wt[i]);
    }
    a[0] /= span; a[1] /= span;
    return true;
}
// AIR CONTROL (P4 round 7, climb_air_control; tester 2026-11-27: "the stick should have a say, it's not realistic but it's been
// established that games do need this ... 0 is the stick has no influence, and 1 is the game's default influence ... at most as much
// as the original game does"). How the game does it [BIN, engine e9fbfc2c]: the gamedll reads LogicJumpAirControllFactor (property
// 0xCA, 0.22 in default_levels.xml; +D105C1 / +D4DB87 / +DD65F9) and hands it to the walk body (+BE08F0 -> walk interface vfunc
// +0x388 = engine +3D3660, clamped 0..1, stored at interface +0x3AC = object +0xAAC; the constructor's default is 0.2). The walk's
// update (engine +3D87D6) multiplies the WANTED vector - the one +ED2040 hands it, the stick's move - by that factor CUBED while the
// body is off the ground (0.22^3 = 0.011): the stick's pull in the air is the engine's own, applied to whatever wanted vector it gets.
// Read as the game's air movement per step: v += k (w - v), k = the factor cubed. That is two parts: k w, the stick's PUSH, and
// -k v, a pull back to rest (the game's own air drag). Round 7's first cut handed the walk h + airControl x (w - h): at 1 that
// was the game's vector outright, i.e. the vanilla air movement, momentum and climb_air_drag gone (tester 2026-11-27: "At 1 it
// should add that 0.22 cubed stick force to the momentum, not replace it").
// So the push is ADDED to the momentum: out = h + airControl x max(0, |w| - h.u) u, u = the stick's direction. Handed to the
// walk that is the momentum (kept, ours to drag) plus airControl x the game's own push along the stick: the k |w| the game gives,
// tapering as the speed along the stick nears the stick's own speed - exactly the game's along-stick term k (|w| - v.u), so the
// stick never makes you faster than it would on its own, and pulling against the momentum brakes as hard as in the game. Across
// the stick the momentum is kept (the game's -k v there is the drag we replace). 1 = the game's own push ("at most as much as the
// original game"), 0 = momentum only. Not during the first armMs of a carry (a launch: the walk body may still treat itself
// as grounded there and take the stick at full strength - run 10's lost launch speed).
// P4 round 8 (run 15): the push above was the WHOLE gap |w| - h.u, on the assumption that the engine then scaled the handed vector by
// k. It does not (the walk moves the body at the handed vector itself, CarryStep's note), so at 1 the stick took the speed over at
// once - the tester: "The stick is still overriding air momentum". The push is now k x that gap per walk update, k = the game's own
// per-update air factor (the walk's LogicJumpAirControllFactor cubed, read live; kGameAirK when unread): the game's own along-stick
// step k (|w| - v.u), added to the momentum.
// P4 round 9 (run 16, tester: "It's extremely powerful at the default 1 value, like more powerful than when walking even"): the
// live read was 1.0 in every traced jump (walk +0xAAC, vfunc +0x390), not the 0.22 the gamedll hands +BE08F0 - so k was 1 and the
// stick took the speed over in one update. k is the game's data value again (kGameAirK, LogicJumpAirControllFactor 0.22 cubed);
// the live value is only logged.
constexpr float kGameAirK = 0.22f * 0.22f * 0.22f;   // LogicJumpAirControllFactor 0.22 (default_levels.xml), cubed (+3D87D6)
inline void AirControlPush(const float h[2], const float w[3], float airControl, float out[3], float k = kGameAirK)
{
    const float s = airControl < 0.0f ? 0.0f : (airControl > 1.0f ? 1.0f : airControl);
    out[0] = h[0];
    out[2] = h[1];
    const float wl = std::sqrt(w[0] * w[0] + w[2] * w[2]);
    if (s <= 0.0f || !(wl > 1e-6f) || !(k > 0.0f)) { return; }
    if (k > 1.0f) { k = 1.0f; }
    const float ux = w[0] / wl, uz = w[2] / wl;
    const float along = h[0] * ux + h[1] * uz;
    const float push = wl - along;
    if (!(push > 0.0f)) { return; }
    out[0] += s * k * push * ux;
    out[2] += s * k * push * uz;
}

// air = the walk's own airborne flag this call: 1 in the air, 0 not, -1 unread. airK = the game's per-update air factor
// (AirControlPush).
inline int CarryStep(Carry& c, const float feet[3], float dt, unsigned long long nowMs, const float gameW[3], bool momentum,
                     float airDrag, float out[3], float airControl = 0.0f, float airK = kGameAirK, int air = -1)
{
    out[0] = gameW[0]; out[1] = gameW[1]; out[2] = gameW[2];
    if (!c.on) { return kCeNone; }
    if (nowMs - c.t0 > kCarryMaxMs) { c.on = false; return kCeTimeout; }
    const bool feetOk = std::isfinite(feet[0]) && std::isfinite(feet[1]) && std::isfinite(feet[2]);
    const bool armed = nowMs - c.t0 >= c.armMs;
    if (air == 1) { c.sawAir = true; }
    if (air == 0 && c.sawAir) { c.on = false; return kCeLanded; }   // the walk says it is down again
    const bool flagLanding = air < 0 || !c.sawAir;                  // no flag (yet): the ballistic test below
    if (c.havePrev && feetOk && dt > 1e-4f && dt <= 0.25f) {   // dt = the REAL time since the last call (a long hitch: no sample)
        const float dx = feet[0] - c.prev[0], dy = feet[1] - c.prev[1], dz = feet[2] - c.prev[2];
        const bool moved = std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f || std::fabs(dz) > 1e-5f;
        // the landing test takes a call where the feet did not move as "not falling" (a body that landed and stopped dead must end
        // the carry too); kCarryLandCalls in a row keeps a lone call with no physics step from ending it
        const float vy = dy / dt;
        const float expVy = c.v[1] - kGravity * static_cast<float>(nowMs - c.t0) / 1000.0f;
        const bool landing = flagLanding && armed && expVy < -0.5f && vy > -kCarryStillMps && vy > expVy + kCarryLandMps;
        c.still = landing ? c.still + 1 : 0;
        if (c.still >= kCarryLandCalls) { c.on = false; return kCeLanded; }
        if (moved) {
            if (armed) {   // what the body really did: collisions taken
                const float mx = dx / dt, mz = dz / dt;
                if (momentum) {
                    // P4 round 9 (run 16, tester: "moving the stick forward increase speed noticeably even when we are already
                    // pretty fast"): the measured speed spikes when the body is pushed out of geometry (4.56 -> 8.22 m/s in one call
                    // with the stick at 6), and h = measured kept every spike. Nothing in the air speeds the body up but what it was
                    // handed: the measurement may slow h or turn it (a wall), never make it faster than the last call handed.
                    // Round 10: measured over a window, and only when the body fell short of what it was handed (above).
                    float m[2], a[2];
                    if (CarryWindow(c, feet, c.tacc + dt, m, a)) {
                        const float ml = std::sqrt(m[0] * m[0] + m[1] * m[1]), al = std::sqrt(a[0] * a[0] + a[1] * a[1]);
                        const bool shorter = ml < kMeasKeep * al;
                        const bool turned = ml > 1e-3f && al > 1e-3f && (m[0] * a[0] + m[1] * a[1]) / (ml * al) < kMeasCos;
                        if (shorter || turned) {
                            const float ll = c.haveLast ? std::sqrt(c.last[0] * c.last[0] + c.last[1] * c.last[1]) : ml;
                            const float sc = ml > ll && ml > 1e-6f ? ll / ml : 1.0f;
                            c.h[0] = m[0] * sc; c.h[1] = m[1] * sc;
                        }
                    }
                } else if (c.haveLast) {
                    // the launch's part loses what the body lost against what it was sent (a wall's share, the stick's own part
                    // cancels out); never re-grown, never turned round
                    const float h0[2] = { c.h[0], c.h[1] };
                    const float l0 = std::sqrt(h0[0] * h0[0] + h0[1] * h0[1]);
                    c.h[0] += mx - c.last[0];
                    c.h[1] += mz - c.last[1];
                    const float l1 = std::sqrt(c.h[0] * c.h[0] + c.h[1] * c.h[1]);
                    if (c.h[0] * h0[0] + c.h[1] * h0[1] <= 0.0f) {
                        c.h[0] = c.h[1] = 0.0f;
                    } else if (l1 > l0 && l1 > 1e-6f) {
                        c.h[0] *= l0 / l1; c.h[1] *= l0 / l1;
                    }
                }
            }
        }
    }
    if (feetOk) {
        for (int i = 0; i < 3; ++i) { c.prev[i] = feet[i]; }
        c.havePrev = true;
    }
    if (momentum) {
        const float dd = dt > 0.25f ? 0.25f : dt;
        const float keep = airDrag > 0.0f && dd > 0.0f ? std::exp(-airDrag * dd) : 1.0f;
        c.h[0] *= keep; c.h[1] *= keep;
        AirControlPush(c.h, gameW, armed ? airControl : 0.0f, out, airK);
    } else {
        float hx = c.h[0] + gameW[0], hz = c.h[1] + gameW[2];
        const float launch = std::sqrt(c.h[0] * c.h[0] + c.h[1] * c.h[1]);
        const float stick = std::sqrt(gameW[0] * gameW[0] + gameW[2] * gameW[2]);
        const float lim = launch > stick ? launch : stick;
        const float s = std::sqrt(hx * hx + hz * hz);
        if (s > lim && s > 1e-6f) { hx *= lim / s; hz *= lim / s; }
        out[0] = hx;
        out[2] = hz;
    }
    c.last[0] = out[0]; c.last[1] = out[2];
    c.haveLast = true;
    if (feetOk && armed) {   // the window's sample (a long hitch starts it over; a launch's drop before arming is not one)
        if (!(dt >= 0.0f && dt <= 0.25f)) { c.wn = 0; } else { c.tacc += dt; }
        if (c.wn == Carry::kWin) {
            for (int i = 1; i < Carry::kWin; ++i) {
                c.wt[i - 1] = c.wt[i];
                c.wp[i - 1][0] = c.wp[i][0]; c.wp[i - 1][1] = c.wp[i][1];
                c.wh[i - 1][0] = c.wh[i][0]; c.wh[i - 1][1] = c.wh[i][1];
            }
            --c.wn;
        }
        c.wt[c.wn] = c.tacc;
        c.wp[c.wn][0] = feet[0]; c.wp[c.wn][1] = feet[2];
        c.wh[c.wn][0] = out[0]; c.wh[c.wn][1] = out[2];
        ++c.wn;
    }
    return kCeNone;
}

// ALL AIR TIME (P4 round 6, climb_air_momentum_all; tester 2026-11-27: "It would be ideal to have this mode work generically on all
// air time, not necessarily just when jumping from a hang"): the game's own jumps and falls start the same carry.
// P4 round 8 (run 15, tester 2026-11-27: "your detection seems to be way off ... if the game needs to know if the character is mid
// air to know whether to apply the 0.22 cubed stick influence, then it MUST know the player is mid air, why can't we use that
// detection?"): round 6 read free fall off the feet (it fired on the ground - "it would move me forward super fast on the ground" -
// and missed real air time). The walk body's own flag is used now [BIN, engine e9fbfc2c]: its update (+3D6970) rebuilds the flags
// word at walk +0x9C0 each step and sets bit 1 only when it is neither on the ground nor just above it (bit 3); the walk interface
// returns that bit from vfunc +0x468 (engine +3D37E0), which the gamedll asks ~100 times (its own "in the air" test, e.g. +D233A3
// before JumpMaxHeight). The carry starts when it is set and ends when it clears (CarryStep's air input).
// FeetVel: the body's velocity off the feet between two walk calls (samples with no physics step between them, or closer than
// kFeetVelMinDtS, are no new sample) - the carry starts with it.
constexpr float kFeetVelMinDtS = 0.004f;
struct FeetVel {
    float p[3] = {};
    float t = 0.0f;
    bool  have = false;
    float v[3] = {};
    bool  haveV = false;
};
inline void FeetVelReset(FeetVel& f) { f.have = false; f.haveV = false; }
inline void FeetVelStep(FeetVel& f, const float feet[3], float tS)
{
    if (!std::isfinite(feet[0]) || !std::isfinite(feet[1]) || !std::isfinite(feet[2]) || !std::isfinite(tS)) { FeetVelReset(f); return; }
    if (f.have) {
        const float dt = tS - f.t;
        const bool moved = std::fabs(feet[0] - f.p[0]) > 1e-5f || std::fabs(feet[1] - f.p[1]) > 1e-5f || std::fabs(feet[2] - f.p[2]) > 1e-5f;
        if (dt > 0.25f || dt < 0.0f) { f.haveV = false; }                 // a long gap: the next pair measures
        else if (!moved || dt < kFeetVelMinDtS) { return; }               // no new sample
        else { for (int k = 0; k < 3; ++k) { f.v[k] = (feet[k] - f.p[k]) / dt; } f.haveV = true; }
    }
    for (int k = 0; k < 3; ++k) { f.p[k] = feet[k]; }
    f.t = tS;
    f.have = true;
}

// P4 round 9 (run 16, tester 2026-11-27: "maybe the yank happened at the same time that my second hand slipped (using the slip
// setting) and that launched me away without me actually making any movement?"): every launch in run 16 had both grips still
// closed - the hands were let go by climb_release_m (0.46-0.51 m from their ledge points), and LaunchPick took the body's speed from
// the yank toward the other hand as a pull. A launch is "let go while pulling": the grips must be OPEN when the hang ends. A grip
// still closed = a hand slipped = the plain drop.
// P4 round 10 (run 17, tester: "the upwards jump failed very often"): a jump flick pulls the hands down faster than the body follows,
// so they tear past climb_release_m with the grips still squeezed - a slip by the rule above (run 17: most up jumps were "a hand
// SLIPPED"). What tells them apart is the CONTROLLERS themselves: in a flick they move fast in the play space; in round 9's case (the
// body yanked toward the other hand) they hardly move - only the body does. So a slip launches too when a controller moved at least
// minMps (climb_launch_min_mps) in the play space within kLaunchWindowMs before it (HandSpeed).
inline bool DropMayLaunch(bool gripLeftClosed, bool gripRightClosed, float handMps = 0.0f, float minMps = 0.0f)
{
    return (!gripLeftClosed && !gripRightClosed) || (minMps > 0.0f && handMps >= minMps);
}
// A controller's speed in the play space (tracking space, so the body's own motion is not in it), its peak over the last window.
struct HandSpeed {
    float  p[3] = {};
    double t = 0.0;
    bool   have = false;
    float  peakMps = 0.0f;
    double peakT = -1e9;
};
inline void HandSpeedStep(HandSpeed& s, bool ok, const float p[3], double tS, double windowS)
{
    if (!ok || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) { s.have = false; return; }
    if (s.have && tS - s.t > 0.002 && tS - s.t <= 0.1) {
        const float dx = p[0] - s.p[0], dy = p[1] - s.p[1], dz = p[2] - s.p[2];
        const float v = std::sqrt(dx * dx + dy * dy + dz * dz) / static_cast<float>(tS - s.t);
        if (v >= s.peakMps || tS - s.peakT > windowS) { s.peakMps = v; s.peakT = tS; }
    } else if (s.have && tS - s.t <= 0.002) {
        return;   // too close to the last sample: keep it as the base
    }
    for (int i = 0; i < 3; ++i) { s.p[i] = p[i]; }
    s.t = tS;
    s.have = true;
}
inline float HandSpeedRecent(const HandSpeed& s, double tS, double windowS) { return tS - s.peakT <= windowS ? s.peakMps : 0.0f; }

// climb_launch_up_assist (tester 2026-11-27: "the game's up jumps are always straight up ... nudge the jump direction toward vertical:
// 30 degrees off centre becomes 15 at 50 %"): a launch within kUpAssistMaxDeg of straight up has its angle from vertical scaled by
// (1 - assist), keeping its speed; between kUpAssistMaxDeg and kUpAssistFadeDeg the assist fades to none, so a push-off is never bent
// and nothing jumps at the boundary. assist 0 = none, 1 = straight up.
inline void UpAssist(float v[3], float assist)
{
    if (!(assist > 0.0f) || !(v[1] > 0.0f)) { return; }
    if (assist > 1.0f) { assist = 1.0f; }
    const float side = std::sqrt(v[0] * v[0] + v[2] * v[2]);
    if (side < 1e-6f) { return; }
    const float speed = std::sqrt(side * side + v[1] * v[1]);
    const float kDeg = 57.29578f;
    const float ang = std::atan2(side, v[1]) * kDeg;   // degrees from straight up
    if (ang >= kUpAssistFadeDeg) { return; }
    float a = assist;
    if (ang > kUpAssistMaxDeg) { a *= (kUpAssistFadeDeg - ang) / (kUpAssistFadeDeg - kUpAssistMaxDeg); }
    const float nang = ang * (1.0f - a) / kDeg;
    const float nside = speed * std::sin(nang);
    v[0] *= nside / side;
    v[2] *= nside / side;
    v[1] = speed * std::cos(nang);
}

// The launch velocity when the last hand lets go: the largest velocity sent within kLaunchWindowMs (the hands slow down just before
// they open, so the newest sample is too low), x scale (the boost), turned toward straight up (UpAssist), then vertical capped at
// kLaunchMaxUpMps and horizontal at kLaunchMaxSideMps - the caps always last, so neither the boost nor the assist passes the game's
// own jump. False = no launch (too slow, mostly downward, nothing recent, or minMps <= 0).
inline bool LaunchPick(const Motion& m, unsigned long long nowMs, float minMps, float scale, float out[3], float upAssist = 0.0f)
{
    out[0] = out[1] = out[2] = 0.0f;
    if (minMps <= 0.0f) { return false; }
    float best = 0.0f;
    float bv[3] = {};
    for (int k = 0; k < m.ringN; ++k) {
        const int i = (m.ringHead - 1 - k + kSentRing) % kSentRing;
        if (nowMs - m.ringMs[i] > kLaunchWindowMs) { continue; }
        const float s = std::sqrt(m.ring[i][0] * m.ring[i][0] + m.ring[i][1] * m.ring[i][1] + m.ring[i][2] * m.ring[i][2]);
        if (s > best) { best = s; bv[0] = m.ring[i][0]; bv[1] = m.ring[i][1]; bv[2] = m.ring[i][2]; }
    }
    if (best < minMps || bv[1] < -0.5f * best) { return false; }
    for (int i = 0; i < 3; ++i) { out[i] = bv[i] * scale; }
    UpAssist(out, upAssist);
    if (out[1] > kLaunchMaxUpMps) {   // scaled whole, so the cap keeps the assisted direction
        const float k = kLaunchMaxUpMps / out[1];
        for (int i = 0; i < 3; ++i) { out[i] *= k; }
    }
    const float side = std::sqrt(out[0] * out[0] + out[2] * out[2]);
    if (side > kLaunchMaxSideMps) {
        const float k = kLaunchMaxSideMps / side;
        out[0] *= k; out[2] *= k;
    }
    return true;
}

// One player walk call in a hold state (game thread). feet = the body's feet now, h = the pad thread's latest publish (h.drive
// already false when stale). mantleM <= 0 = no auto climb-up.
inline MotionOut MotionStep(Motion& m, const Hold& h, const float feet[3], unsigned long long nowMs, float mantleM, float pullMaxMps,
                            float slowMps = 1.5f)
{
    MotionOut o;
    if (!h.drive) { m.upSinceMs = 0; return o; }
    PullVelocity(h.err, kPullTauS, pullMaxMps, o.v);
    const float speed = std::sqrt(o.v[0] * o.v[0] + o.v[1] * o.v[1] + o.v[2] * o.v[2]);
    if (MantleDue(h.drive, h.haveTop, h.topY, feet[1], mantleM) && speed < slowMps) {
        if (m.upSinceMs == 0) { m.upSinceMs = nowMs; }
        if (nowMs - m.upSinceMs >= kMantleDwellMs) { o.climbUp = true; }   // the caller asks the game, once
    } else {
        m.upSinceMs = 0;
    }
    o.send = true;
    m.ring[m.ringHead][0] = o.v[0]; m.ring[m.ringHead][1] = o.v[1]; m.ring[m.ringHead][2] = o.v[2];
    m.ringMs[m.ringHead] = nowMs;
    m.ringHead = (m.ringHead + 1) % kSentRing;
    if (m.ringN < kSentRing) { ++m.ringN; }
    return o;
}

// ===== The pad-thread machine =====
constexpr unsigned long long kGameFreshMs  = 250;   // a game publish older than this means the game thread stopped
constexpr unsigned long long kDropWaitMs   = 600;   // a drop request still hanging by then failed
constexpr unsigned long long kDropRetryMs  = 1500;  // after a failed drop, wait before asking again
// A press another feature holds at its edge, then lets go of while the grip stays closed, within this long, is still the climb's
// press (run 3, 2026-11-26: with body_grab on, loot by hand withholds EVERY press with no body near for up to 150 ms - its
// severed-limb wait, looting.cpp kLateArmS - so the press edge always read "owned" and ~95% of presses never searched).
constexpr unsigned long long kLateTakeMs   = 300;

// What the game thread last published (climbhand.cpp, every player update).
struct GameView {
    bool               egActive = false;   // the ledge grab is running
    int                egState = -1;       // its current state (+0x1FC)
    bool               onFoot = false;     // the controller group allows a new climb (roomscale's on-foot rule)
    unsigned long long ageMs = ~0ull;      // now - the publish time
    unsigned           moveSeq = 0;        // round 19: bumped each time a game move from the hang ended back in it (MoveShiftStep)
    float              moveShift[3] = {};  // ...and how far that move carried the body (game world, m)
    bool               pole = false;       // P5 slide: the hang is on a ladder / pole (g_poleOn)
    bool               haveFeet = false;   // ...the feet's height (world y) this update
    float              feetY = 0.0f;
    bool               havePoleLo = false; // ...the climbable's world bottom is known (its extents)
    float              poleLoY = 0.0f;
    int                poleKind = -1;      // 0 ladder, 1 pipe, 2 rope, 3 underwater rope (-1 unknown)
    bool               zipRiding = false;  // P6 round 3: the zipline controller is active (ZipHold ends when it is not)
};

// P5 SLIDE (design §4.9 "Slide": "grips half-closed = slide down at the game's slide speed. Let go = a fall"). On a ladder / pole
// hang, every holding hand's grip held but LIGHTLY (squeeze at or above the 0.5 hold threshold, under slideGripSq) slides the body
// down: each poll the held anchors move down by speed x dt, and P2's hand-driven body follows them (the speed is capped under the
// pull's own cap so the body keeps up and no hand drifts past climb_release_m). A full squeeze stops it at once; opening the grips is
// the plain let-go. The slide stops at the BOTTOM: the feet within kSlideBottomM of the climbable's world bottom (its extents), or
// the feet no lower than kSlideStallM over kSlideStallMs while sliding (the ground under an unreadable climbable, or something
// solid). At the bottom the hang ends with the plain drop - the player stands at the foot of the ladder.
constexpr float              kSlideBottomM  = 0.05f;
constexpr float              kSlideStallM   = 0.02f;
constexpr unsigned long long kSlideStallMs  = 250;
constexpr float              kSlideMaxDtS   = 0.05f;   // a long gap between polls never jumps the anchors
constexpr float              kSlideCapShare = 0.8f;    // the slide never above this share of climb_pull_max_mps
struct Slide {
    bool               on = false;
    unsigned long long t0 = 0, lastMs = 0, stallMs = 0;
    float              stallY = 0.0f;
    float              y0 = 0.0f;     // the feet when it began (log)
    bool               haveY0 = false;
    float              slidM = 0.0f;  // the anchors' travel this slide (log)
};
// Every holding hand grips lightly (true), on a pole hang in a hold state. held = the machine's hand bits.
inline bool SlideGrip(float slideMps, bool pole, bool holdState, unsigned held, const float sq[2], float slideGripSq)
{
    if (!(slideMps > 0.0f) || !pole || !holdState || held == 0) { return false; }
    for (int h = 0; h < 2; ++h) {
        if ((held & (1u << h)) && !(sq[h] < slideGripSq)) { return false; }
    }
    return true;
}
enum SlideResult : int { kSlideNone = 0, kSlideStart, kSlideMove, kSlideStop, kSlideBottom };
// One poll. gripSlide = SlideGrip. downOut = how far the anchors move down this poll (m, >= 0). Bottom = the hang must end (plain
// drop); Stop = the grip tightened / the hang ended.
inline int SlideStep(Slide& s, bool gripSlide, unsigned long long now, float slideMps, float pullMaxMps, bool haveFeet, float feetY,
                     bool haveLo, float loY, float* downOut)
{
    *downOut = 0.0f;
    if (!gripSlide) {
        const bool was = s.on;
        s.on = false;
        return was ? kSlideStop : kSlideNone;
    }
    int r = kSlideMove;
    if (!s.on) {
        s = Slide();
        s.on = true;
        s.t0 = s.lastMs = s.stallMs = now;
        s.stallY = haveFeet ? feetY : 0.0f;
        s.y0 = s.stallY;
        s.haveY0 = haveFeet;
        return kSlideStart;   // the first poll only times; the motion starts next poll
    }
    float dt = static_cast<float>(now - s.lastMs) * 0.001f;
    s.lastMs = now;
    if (dt > kSlideMaxDtS) { dt = kSlideMaxDtS; }
    if (dt < 0.0f) { dt = 0.0f; }
    if (haveFeet) {
        if (haveLo && feetY - loY <= kSlideBottomM) { s.on = false; return kSlideBottom; }
        if (feetY < s.stallY - kSlideStallM) {
            s.stallY = feetY;
            s.stallMs = now;
        } else if (now - s.stallMs >= kSlideStallMs) {
            s.on = false;
            return kSlideBottom;
        }
    }
    float v = slideMps;
    const float cap = pullMaxMps * kSlideCapShare;
    if (cap > 0.0f && v > cap) { v = cap; }
    float d = v * dt;
    if (haveFeet && haveLo && d > feetY - loY) { d = feetY - loY; }   // never below the bottom
    if (d < 0.0f) { d = 0.0f; }
    *downOut = d;
    s.slidM += d;
    return r;
}

// Round 19 (tester 2026-11-27: "regarding shimmies, they have been broken forever. If I'm grabbed onto a ledge, and I shimmy say
// right, it does the animation, but then moves me back to where I was originally hanging. Same for up and sideways"). Each holding
// hand's anchor is the ledge point its search found (PullStep); the game's own move from the hang (shimmy 11-13 / 15, reach-up 5 / 9)
// carries the body - and the hands, which ride the body - but the anchors re-latch on the SAME old points when the hang resumes, so
// P2 pulls the body straight back. MoveShiftStep follows the body through such a move: the feet at the last hold update before it,
// and, when the grab is back in a hold state, the shift (feet now - then). The pad thread moves each held ledge point by it before
// the anchors re-latch. Jumps / the climb-up / the drop end the hang, so they never come back to a hold with a shift.
struct MoveShift {
    bool  haveHold = false;   // lastFeet is from a hold update of this grab
    bool  away = false;       // the grab left the hold states for one of the game's moves
    int   moveState = -1;     // that move (for the log)
    float lastFeet[3] = {};
    float feet0[3] = {};
};
inline bool IsGameHangMove(int s)
{
    return (s >= 11 && s <= 13) || s == 15 || s == 5 || s == 9;
}
// True = a move just ended back in the hang; shift = how far it carried the body.
inline bool MoveShiftStep(MoveShift& m, bool active, int state, const float feet[3], bool haveFeet, float shift[3])
{
    shift[0] = shift[1] = shift[2] = 0.0f;
    if (!active) { m = MoveShift(); return false; }
    const bool hold = state == 1 || state == 2 || state == 3 || state == 7 || state == 14;   // IsHoldState (below)
    if (hold) {
        bool done = false;
        if (m.away && haveFeet) {
            for (int i = 0; i < 3; ++i) { shift[i] = feet[i] - m.feet0[i]; }
            done = true;
        }
        m.away = false;
        if (haveFeet) {
            for (int i = 0; i < 3; ++i) { m.lastFeet[i] = feet[i]; }
            m.haveHold = true;
        }
        return done;
    }
    if (!m.away && m.haveHold && IsGameHangMove(state)) {
        m.away = true;
        m.moveState = state;
        for (int i = 0; i < 3; ++i) { m.feet0[i] = m.lastFeet[i]; }
    } else if (!m.away && !IsGameHangMove(state)) {
        m.haveHold = false;   // a jump / climb-up / the grab itself: no hold to come back to with a shift
    }
    return false;
}

// Round 7 (2026-11-26, tester after run 5): (a) "grab a ledge with one hand, press grab with the other ANYWHERE, release the first:
// the character keeps hanging with no hand on the ledge" - any press while hanging joined as a hold; (b) "pressing grab with the
// other hand, the first hand's grab stops existing" - one search at a time, the newer press took it over. Now EACH hand searches on
// its own (both at once), and a hand holds the climb ONLY once the game thread found a ledge at that hand (foundMs) - the grab's
// own start, or a join while hanging. A press anywhere else holds nothing.
constexpr unsigned long long kStartWaitMs = 500;   // a found hand whose grab never became active by then holds nothing

struct In {
    bool     enabled = false;     // climb_by_hand and not in a menu / overlay, and the detour is live
    bool     grip[2] = {};        // grip squeezed past the threshold, 0 = left, 1 = right
    bool     owned[2] = {};       // another feature took this hand's press first (holster, two-hand, reload, lid, loot, push-grab)
    bool     handOk[2] = {};      // the hand's world position is known this poll
    GameView game;
    unsigned long long catchMs = 1500;   // climb_catch_ms: how long after a press a held grip keeps looking for a ledge
    unsigned long long nowMs = 0;
    unsigned long long foundMs[2] = {};  // the game thread's last "a ledge within reach of this hand" (started or joined); 0 = never
};

enum Phase : int { kIdle = 0, kHanging = 2, kDropping = 3 };   // (1 was the single search - searches are per hand now)

enum Event : int {
    kEvNone = 0,
    kEvGrabLost,        // a hand found a ledge but the grab never became active (kStartWaitMs)
    kEvStarted,         // the ledge grab became active with a hand on the ledge
    kEvUnused3,
    kEvDropRequested,   // no hand on the ledge while hanging: the game thread is asked for the game's own drop
    kEvDropFailed,      // still hanging after kDropWaitMs
    kEvEnded,           // the ledge grab ended (climbed up, dropped, jumped off, interrupted)
    kEvDisabled,        // the switch went off mid-climb: hands off, the game keeps its climb
    kEvCount
};

// Per-hand search outcomes (for the log)
enum SearchEnd : int { kSeNone = 0, kSeReleased = 1, kSeTimedOut = 2, kSeFound = 3 };

struct Machine {
    Phase              phase = kIdle;
    unsigned           held = 0;          // bit h = hand h is on the ledge, holding the climb (found and still squeezed)
    unsigned           searching = 0;     // bit h = hand h's grip is closed and the game thread looks for a ledge at it
    unsigned long long searchT[2] = {};   // when each hand's search began
    bool               prevGrip[2] = {};
    bool               pending[2] = {};   // this hand's press was owned at its edge and may still come back (kLateTakeMs)
    unsigned long long pendMs[2] = {};    // that press's edge time
    unsigned long long t = 0;             // when the current phase began (kIdle with a hand held: when it was found)
    unsigned long long retryAfter = 0;    // no drop request before this (after a failed one)
};

struct Out {
    int  event = kEvNone;
    bool search[2] = {};      // the game thread casts at this hand this update
    bool searchStarted[2] = {};
    int  searchEnded[2] = {}; // SearchEnd: why this hand's search stopped this poll
    bool joined[2] = {};      // found while the climb already ran: this hand now holds it too
    bool requestDrop = false; // ask the game thread for the drop (once, on kEvDropRequested)
    bool withhold[2] = {};    // do not send this grip to the game as L1 / R1
    // Press ownership outcomes (for the log): lateTake = an owned press came back to the climb this poll, after lateMs; lost = the
    // owner kept it (the grip opened while owned, or kLateTakeMs passed) - not a climb.
    bool lateTake[2] = {};
    bool lost[2] = {};
    unsigned long long lateMs[2] = {};
};

inline unsigned Bit(int h) { return 1u << h; }

inline void ToIdle(Machine& m, unsigned long long now)
{
    m.phase = kIdle;
    m.held = 0;
    m.t = now;
}

// One pad poll.
inline Out Update(Machine& m, const In& in)
{
    Out o;
    bool edge[2];
    for (int h = 0; h < 2; ++h) {
        edge[h] = in.grip[h] && !m.prevGrip[h];
        m.prevGrip[h] = in.grip[h];
    }
    if (!in.enabled) {   // (also drops any pending press: a press made in a menu is never a climb)
        if (m.phase != kIdle) { o.event = kEvDisabled; }
        const bool pg[2] = { m.prevGrip[0], m.prevGrip[1] };
        m = Machine{};
        m.prevGrip[0] = pg[0];
        m.prevGrip[1] = pg[1];
        m.t = in.nowMs;
        return o;
    }
    // An owned press waits up to kLateTakeMs for its owner to let go; then it is a fresh edge for everything below. A new edge
    // that is owned replaces any older pending one.
    for (int h = 0; h < 2; ++h) {
        if (edge[h] && in.owned[h]) {
            m.pending[h] = true;
            m.pendMs[h] = in.nowMs;
            edge[h] = false;
        } else if (m.pending[h]) {
            const unsigned long long waited = in.nowMs - m.pendMs[h];
            if (!in.grip[h]) {
                m.pending[h] = false;
                o.lost[h] = true;
            } else if (!in.owned[h] && waited <= kLateTakeMs) {
                m.pending[h] = false;
                edge[h] = true;
                o.lateTake[h] = true;
                o.lateMs[h] = waited;
            } else if (waited > kLateTakeMs) {
                m.pending[h] = false;
                o.lost[h] = true;
            }
        }
    }
    const bool fresh = in.game.ageMs <= kGameFreshMs;
    // 1. a hand that opens lets go (of the ledge, or of its search)
    for (int h = 0; h < 2; ++h) {
        if (!in.grip[h]) {
            m.held &= ~Bit(h);
            if (m.searching & Bit(h)) {
                m.searching &= ~Bit(h);
                o.searchEnded[h] = kSeReleased;
            }
        }
    }
    // 2. each searching hand: found a ledge -> it holds; out of catch time -> it stops looking (the grip stays the game's)
    for (int h = 0; h < 2; ++h) {
        if (!(m.searching & Bit(h))) { continue; }
        if (in.foundMs[h] != 0 && in.foundMs[h] >= m.searchT[h]) {
            m.searching &= ~Bit(h);
            o.searchEnded[h] = kSeFound;
            if (m.phase == kIdle && m.held == 0) { m.t = in.nowMs; }   // the grab's start: kStartWaitMs counts from here
            if (m.phase != kIdle) { o.joined[h] = true; }
            m.held |= Bit(h);
        } else if (in.nowMs - m.searchT[h] > in.catchMs) {
            m.searching &= ~Bit(h);
            o.searchEnded[h] = kSeTimedOut;
        }
    }
    // 3. a free press starts that hand's own search - both hands may search at once, neither replaces the other
    for (int h = 0; h < 2; ++h) {
        if (edge[h] && in.handOk[h] && !(m.held & Bit(h)) && !(m.searching & Bit(h))) {
            m.searching |= Bit(h);
            m.searchT[h] = in.nowMs;
            o.searchStarted[h] = true;
        }
    }
    switch (m.phase) {
    case kIdle:
        if (m.held != 0) {
            if (fresh && in.game.egActive) {
                m.phase = kHanging;
                m.t = in.nowMs;
                o.event = kEvStarted;
            } else if (in.nowMs - m.t > kStartWaitMs) {
                m.held = 0;
                o.event = kEvGrabLost;
            }
        }
        break;
    case kHanging:
        if (fresh && !in.game.egActive) {
            ToIdle(m, in.nowMs);
            o.event = kEvEnded;
        } else if (fresh && m.held == 0 && IsHangState(in.game.egState) && in.nowMs >= m.retryAfter) {
            m.phase = kDropping;
            m.t = in.nowMs;
            o.event = kEvDropRequested;
            o.requestDrop = true;
        }
        break;
    case kDropping:
        if (fresh && !in.game.egActive) {
            ToIdle(m, in.nowMs);
            o.event = kEvEnded;
        } else if (m.held != 0) {   // grabbed again before the drop landed: keep hanging
            m.phase = kHanging;
            m.t = in.nowMs;
        } else if (in.nowMs - m.t > kDropWaitMs) {
            m.phase = kHanging;
            m.t = in.nowMs;
            m.retryAfter = in.nowMs + kDropRetryMs;
            o.event = kEvDropFailed;
        }
        break;
    }
    for (int h = 0; h < 2; ++h) { o.search[h] = (m.searching & Bit(h)) != 0; }
    // A searching grip is NOT withheld: with no ledge it is the game's L1 / R1 as always. Only a held climb takes the grip.
    for (int h = 0; h < 2; ++h) { o.withhold[h] = in.grip[h] && (m.held & Bit(h)) != 0; }
    return o;
}

}  // namespace climbhand
}  // namespace dlvr
