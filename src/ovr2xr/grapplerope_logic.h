// The grappling hook as a ROPE (P7 of docs/design_climbing_overhaul.md §8.21; tester 2026-11-28: "It should no longer pull you
// automatically, but rather only while holding the trigger ... It should act like an anchor point, like a rope, so you can swing on
// it (it respects your momentum and speed when you use it) and using the trigger to pull affects the swing, doesn't override it").
// Pure decisions, no game reads: climbhand.cpp feeds them the feet, the walk's own "in the air" flag and the trigger, and sends the
// velocity they return to the walk (the same walk call the climbing hands drive). Tests: tests/grapplerope_logic_test.cpp.
//
// The model: the hook is a fixed anchor; the rope runs from it to the hands (the feet + kHandUpM). The body keeps its own velocity
// (gravity integrated here while the walk says it is in the air). Each update:
//   - the trigger (when pulling is on) pushes the body toward the anchor - an ACCELERATION along the rope, never a set velocity: the
//     speed across the rope (the swing) is untouched, and the pull stops adding once the body closes in at pullMps;
//   - a rope shorter than its length is slack: the length is taken in to the distance (a reel - the next swing is on the shorter
//     rope; never below kMinLenM);
//   - a taut rope removes the body's speed AWAY from the anchor (an inelastic rope) and pulls a body found past the length back
//     at kStretchK per second of the excess (drift from the walk's own steps).
// Nothing here starts or ends the rope by itself except RopeEndCheck's verdict, which the glue acts on.
#pragma once

#include <cmath>

namespace dlvr {
namespace grapplerope {

constexpr float kGravity  = 9.81f;
constexpr float kHandUpM  = 1.3f;     // where the rope meets the body: about the hands' height above the feet
constexpr float kMinLenM  = 1.0f;     // the reel never takes the rope shorter than this
constexpr float kStretchK = 6.0f;     // 1/s: a body past the rope's length is pulled back at (excess x this)
constexpr float kMaxMps   = 40.0f;    // the body's speed on the rope is capped here (a safety net, not a feel)
constexpr float kPullRampS = 0.4f;    // the trigger's pull reaches its full closing speed in about this long
constexpr float kArriveM  = 1.2f;     // the hands this close to the hook: the rope lets go (the body is there)
constexpr unsigned long long kMaxMs = 120000;   // a safety net: no rope lasts longer
constexpr float kMaxDtS   = 0.1f;     // a longer gap between walk calls (a hitch) integrates as this much

struct Rope {
    bool               on = false;
    float              anchor[3] = {};   // the hook (world)
    float              len = 0.0f;       // the rope's length now (m)
    float              len0 = 0.0f;      // ...when it caught (for the log)
    float              v[3] = {};        // the body's velocity (world, m/s)
    unsigned long long t0 = 0;
    bool               sawAir = false;   // the walk said "in the air" at least once on this rope
    int                wallCalls = 0;    // calls in a row the body moved far less than it was sent sideways (a wall)
};

inline float Len3(const float a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

// The rope's end on the body.
inline void HandPoint(const float feet[3], float p[3])
{
    p[0] = feet[0];
    p[1] = feet[1] + kHandUpM;
    p[2] = feet[2];
}

// The hook caught: the rope's length is the distance at that moment, the body keeps the velocity it had (v0, measured).
inline void StartRope(Rope& r, const float anchor[3], const float feet[3], const float v0[3], unsigned long long nowMs)
{
    r = Rope{};
    r.on = true;
    for (int i = 0; i < 3; ++i) {
        r.anchor[i] = anchor[i];
        r.v[i] = std::isfinite(v0[i]) ? v0[i] : 0.0f;
    }
    float p[3];
    HandPoint(feet, p);
    const float d[3] = { p[0] - anchor[0], p[1] - anchor[1], p[2] - anchor[2] };
    r.len = Len3(d);
    if (r.len < kMinLenM) { r.len = kMinLenM; }
    r.len0 = r.len;
    r.t0 = nowMs;
}

struct StepOut {
    float v[3] = {};      // the velocity to send (world, m/s)
    float dist = 0.0f;    // hands to hook
    bool  taut = false;   // the rope is at its length
    float closing = 0.0f; // the body's speed toward the hook after the step (m/s; negative = moving away)
    bool  pulled = false; // the trigger added speed this step
};

// One walk call. dt = the real time since the last call (s); grounded = the walk says the body is on the ground (no gravity, no
// downward speed kept); pullMps > 0 = the trigger pulls, toward this closing speed.
inline StepOut RopeStep(Rope& r, const float feet[3], float dt, bool grounded, float pullMps)
{
    StepOut o;
    if (!(dt >= 0.0f)) { dt = 0.0f; }
    if (dt > kMaxDtS) { dt = kMaxDtS; }
    if (grounded) {
        if (r.v[1] < 0.0f) { r.v[1] = 0.0f; }
    } else {
        r.sawAir = true;
        r.v[1] -= kGravity * dt;
    }
    float p[3];
    HandPoint(feet, p);
    float n[3] = { p[0] - r.anchor[0], p[1] - r.anchor[1], p[2] - r.anchor[2] };   // anchor -> hands
    const float dist = Len3(n);
    if (dist > 1e-4f) {
        for (int i = 0; i < 3; ++i) { n[i] /= dist; }
    } else {
        n[0] = 0.0f; n[1] = -1.0f; n[2] = 0.0f;
    }
    if (pullMps > 0.0f) {
        const float away = r.v[0] * n[0] + r.v[1] * n[1] + r.v[2] * n[2];   // + = away from the hook
        const float room = away + pullMps;                                   // how far below the closing cap the body is
        if (room > 0.0f) {
            float dv = pullMps / kPullRampS * dt;
            if (dv > room) { dv = room; }
            for (int i = 0; i < 3; ++i) { r.v[i] -= n[i] * dv; }
            o.pulled = dv > 0.0f;
        }
    }
    if (dist < r.len) { r.len = dist > kMinLenM ? dist : kMinLenM; }   // slack: the reel takes it in
    o.taut = dist >= r.len - 1e-3f;
    if (o.taut) {   // at its length (not only past it): moving outward now would stretch it on this step
        const float away = r.v[0] * n[0] + r.v[1] * n[1] + r.v[2] * n[2];
        if (away > 0.0f) { for (int i = 0; i < 3; ++i) { r.v[i] -= n[i] * away; } }
    }
    if (dist > r.len) {
        const float back = (dist - r.len) * kStretchK;
        for (int i = 0; i < 3; ++i) { r.v[i] -= n[i] * back; }
    }
    const float s = Len3(r.v);
    if (s > kMaxMps) { for (int i = 0; i < 3; ++i) { r.v[i] *= kMaxMps / s; } }
    for (int i = 0; i < 3; ++i) { o.v[i] = r.v[i]; }
    o.dist = dist;
    o.closing = -(r.v[0] * n[0] + r.v[1] * n[1] + r.v[2] * n[2]);
    return o;
}

// A WALL: the walk moves the body at what it is sent unless something stops it. When the body moved far less sideways than it was
// sent (under half, at more than 1 m/s sent) on kWallCalls calls in a row, the rope's sideways velocity becomes what the body really
// did (a lone call with no physics step between two walk calls reads as zero movement - hence the run of calls). sentH / measH =
// (x, z) m/s. True = the rope's velocity was changed.
constexpr int kWallCalls = 3;
inline bool WallCheck(Rope& r, const float sentH[2], const float measH[2])
{
    const float sl = std::sqrt(sentH[0] * sentH[0] + sentH[1] * sentH[1]);
    const float ml = std::sqrt(measH[0] * measH[0] + measH[1] * measH[1]);
    if (!(sl > 1.0f) || !(ml < 0.5f * sl)) { r.wallCalls = 0; return false; }
    if (++r.wallCalls < kWallCalls) { return false; }
    r.v[0] = measH[0];
    r.v[2] = measH[1];
    r.wallCalls = 0;
    return true;
}

enum RopeEnd : int { kReNone = 0, kReJump, kReLanded, kReArrived, kReTimeout, kReReleased, kReGameEnded, kReCount };

// Whether the rope lets go this call. jumpEdge = the jump button was pressed this call; grounded as in RopeStep; dist = hands to
// hook. A landing ends it only after the rope has been in the air (a rope caught standing is a leash until you jump or pull).
// Round 2 (grapple_hold_to_swing; tester 2026-11-28: "pressing and holding left trigger is what launches the hook and makes you swing
// (and you don't stop swinging until you release the left trigger ...)"): holdMode = the left trigger IS the rope - it lets go when
// that trigger is released (holdHeld false) and for nothing else: no jump, landing, arrival or time out (a rope pulled all the way in
// hangs at kMinLenM; on the ground it is a leash). Round 4 puts the arrival back (see below).
// Round 4 (tester 2026-11-28: "the game does drop me out of the grappling hook mode after a few seconds ... when I finally release the
// trigger, I'm moved to where the grappling hook latched and dropped from there"): gameHolds = the game's rope is still in its pull
// (state 3). Run 2026-11-28 17:29: once it left (near the hook, or touching the ground), MoveController had the walk, the rope's vertical
// went unheard (the feet fell at -12 m/s while the rope asked +40) and that runaway velocity was handed on at the release - the launch
// to the hook. Our rope cannot hold the body without the game's controller, so it lets go at once (kReGameEnded), in either mode; the
// glue keeps the game's pull from ending itself while a hold-mode rope holds (KeepGameRopeOn), so this is the net, not the rule.
inline int RopeEndCheck(const Rope& r, bool jumpEdge, bool grounded, float dist, unsigned long long nowMs, bool holdMode = false,
                        bool holdHeld = true, bool gameHolds = true)
{
    if (!r.on) { return kReNone; }
    if (holdMode && !holdHeld) { return kReReleased; }
    if (!gameHolds) { return kReGameEnded; }
    // round 4 (tester 2026-11-28: "letting go at the hook point it's okay, but touching surfaces shouldn't force the drop, neither should
    // the timeout"): the game's own arrival is skipped with its contact and time-limit ends (one branch), so arrival is ours here
    if (holdMode) { return dist <= kArriveM ? kReArrived : kReNone; }
    if (jumpEdge) { return kReJump; }
    if (grounded && r.sawAir) { return kReLanded; }
    if (dist <= kArriveM) { return kReArrived; }
    if (nowMs - r.t0 > kMaxMs) { return kReTimeout; }
    return kReNone;
}

// Round 4: whether the game's pull is kept from taking its end block (gamedll +D79AD1, see climbhand.cpp kRopeEndJccRva) - the exit it
// takes on reaching the hook, on a contact against the pull, and on its time limit. Only while a hold-mode rope holds and the game's
// rope is in its pull: the trigger and reaching the hook (RopeEndCheck's own arrival) are the only ways off, as asked. Otherwise the
// game's rope ends by its own rules.
inline bool KeepGameRopeOn(bool holdMode, bool ropeOn, int gameState)
{
    return holdMode && ropeOn && gameState == 3;
}

// Round 5 (tester 2026-11-28: "When shooting the grappling hook, the game moves my view to look at the hook point, remove that when our
// new functionality is enabled"): whether the rope's own look interface is offered to the look selector (climbhand.cpp RopeLookHook).
// featureOn = grapple_rope_swing with climb_by_hand (the rope needs its walk hook); localRope = the local player's rope controller.
inline bool RopeTakesLook(bool featureOn, bool localRope)
{
    return !(featureOn && localRope);
}

// Round 4: the velocity handed on when the rope lets go. Normally the rope's own (the body followed it: run 1 TRACE, within ~10 %).
// When the game's rope ended first the body was no longer following ours, so what the feet really did is handed on instead.
inline void LetGoVelocity(int end, const float ropeV[3], const float measV[3], bool haveMeas, float out[3])
{
    const bool meas = end == kReGameEnded && haveMeas;
    for (int i = 0; i < 3; ++i) { out[i] = meas ? measV[i] : ropeV[i]; }
}

// Round 2 (tester 2026-11-28: "Make sure that the swing's momentum carries into the walk state when releasing the hook, as it
// currently doesn't"): on letting go the game's rope controller keeps the player for an update or more (run 2026-11-28 17:01:28:
// "LAUNCH CARRY ended - another controller took the player 0 ms in" - the air carry only runs under MoveController). Until the walk is
// MoveController's again (at most kFreeMaxMs), the body flies free on the rope's last velocity: gravity in the air, no rope.
constexpr unsigned long long kFreeMaxMs = 600;
inline void FreeStep(Rope& r, float dt, bool grounded)
{
    if (!(dt >= 0.0f)) { dt = 0.0f; }
    if (dt > kMaxDtS) { dt = kMaxDtS; }
    if (grounded) {
        if (r.v[1] < 0.0f) { r.v[1] = 0.0f; }
    } else {
        r.v[1] -= kGravity * dt;
    }
}

// The trigger's pull this call: pulling on (grapple_trigger_pull), the trigger past `threshold`, and a pull speed set. 0 = no pull.
inline float PullMps(bool pullOn, float trigger, float pullMps, float threshold = 0.5f)
{
    return pullOn && trigger >= threshold && pullMps > 0.0f ? pullMps : 0.0f;
}

// ROUND 3 (tester 2026-11-28: "Aiming hand ... Grappling hand: changes which trigger shoots the hook ... Pull hand: changes which
// trigger causes the hook to pull"). Hands are controllers.h's: 0 = left, 1 = right.
// The game throws the hook on L2 (_ACTION_USE_EQUIPEMENT) and attacks on R2. With the grappling hand on the RIGHT, the game's two
// trigger bytes trade places while the hook is the selected item (or a rope holds): the right trigger throws, the left attacks.
constexpr int kHandLeft = 0, kHandRight = 1;
inline bool TriggersSwapped(int grappleHand, bool hookSelected, bool ropeOn)
{
    return grappleHand == kHandRight && (hookSelected || ropeOn);
}
// While a rope holds and pulling is on, the pull trigger must not also reach the game. It is the trigger that is NOT the grappling
// hand's, and that one always lands on the game's R2 (unswapped: the right trigger on R2; swapped: the left trigger on R2). The same
// hand for both: nothing is withheld (that trigger is the grapple's own L2).
inline bool WithholdR2(int grappleHand, int pullHand, bool ropeOn, bool pullOn)
{
    return ropeOn && pullOn && grappleHand != pullHand;
}
// The pull's threshold: past half on its own trigger. The grappling trigger doubling as the pull in hold mode (it is held all the
// rope long) pulls only when squeezed all the way - held part way it swings.
constexpr float kPullSameHandThreshold = 0.95f;
inline float PullThreshold(int grappleHand, int pullHand, bool holdMode)
{
    return holdMode && grappleHand == pullHand ? kPullSameHandThreshold : 0.5f;
}

}  // namespace grapplerope
}  // namespace dlvr
