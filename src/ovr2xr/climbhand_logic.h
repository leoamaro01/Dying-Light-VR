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
inline int GameMoveFromHang(bool handMode, bool stickShimmy, bool stickClimb, bool stickForward, bool ours, bool held,
                            bool retargetFresh, int from, int to)
{
    const bool hold = from == 1 || from == 2 || from == 3 || from == 7 || from == 14;   // IsHoldState (defined with P2 below)
    const bool reachUp = to == 5 || to == 9;
    const bool move = to == 8 || (to >= 11 && to <= 15) || reachUp;
    if (!handMode || ours || !hold || !move) { return kGmPass; }
    if (held && !retargetFresh) { return kGmRetarget; }
    if (reachUp) { return kGmPass; }
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

constexpr float kFaceBackM  = 0.45f;   // face rays start this far behind the hand (toward the player)
constexpr float kFacePastM  = 0.35f;   // ... and end this far past it
constexpr float kTopAboveM  = 0.50f;   // top rays start this far above the face hit
constexpr float kTopBelowM  = 0.10f;   // ... and end this far below it
constexpr float kHalfEdgeM  = 0.25f;   // half the edge handed to the game
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

// What the pad thread last published about the anchored hands (climbhand.cpp, every poll).
struct Hold {
    bool               drive = false;   // at least one anchored hand (PullOut::drive)
    float              err[3] = {};     // PullOut::err
    unsigned long long driveMs = 0;     // when drive was last published true
    bool               haveTop = false; // at least one anchored hand with a ledge point
    float              topY = 0.0f;     // the highest anchored ledge point's height
    float              mid[3] = {};     // the mean anchored ledge point
    float              into[3] = {};    // the mean inward (into the wall) direction of the anchored hands' ledges, horizontal
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
};

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
