// Standalone unit test for src/ovr2xr/grapplerope_logic.h - NOT part of the DLL build.
// docs/design_climbing_overhaul.md §8.21: the grappling hook as a rope - a pendulum that keeps its energy and length, momentum
// kept at the catch, the trigger's pull as an acceleration along the rope (the swing kept), the reel, the end rules.
// Exit code = number of failures.
#include "grapplerope_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::grapplerope;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// The walk moves the body at exactly what it is sent (run 15's measurement in the air): feet += v dt.
struct Sim {
    Rope  r;
    float feet[3];
    StepOut last;
    void Step(float dt, float pull = 0.0f, bool grounded = false)
    {
        last = RopeStep(r, feet, dt, grounded, pull);
        for (int i = 0; i < 3; ++i) { feet[i] += last.v[i] * dt; }
    }
};

static const float kAnchor[3] = { 0.0f, 20.0f, 0.0f };

// Feet placed so the hands are at (x, y, z).
static Sim At(float x, float y, float z, const float v0[3])
{
    Sim s;
    s.feet[0] = x; s.feet[1] = y - kHandUpM; s.feet[2] = z;
    StartRope(s.r, kAnchor, s.feet, v0, 1000);
    return s;
}

static void pendulum_keeps_length_and_energy()
{
    // let go from level with the hook, 6 m out: at the bottom the speed is sqrt(2 g L) = 10.85 m/s
    const float v0[3] = {};
    Sim s = At(6.0f, 20.0f, 0.0f, v0);
    CHECK(Near(s.r.len, 6.0f, 1e-4f));
    float maxSpeed = 0.0f, maxStretch = 0.0f, minX = 1e9f;
    for (int i = 0; i < 90 * 3; ++i) {   // 3 s at 90 Hz: past the bottom and up the far side
        s.Step(1.0f / 90.0f);
        const float sp = Len3(s.last.v);
        if (sp > maxSpeed) { maxSpeed = sp; }
        if (s.last.dist - s.r.len > maxStretch) { maxStretch = s.last.dist - s.r.len; }
        if (s.feet[0] < minX) { minX = s.feet[0]; }
    }
    CHECK(maxSpeed > 0.85f * std::sqrt(2.0f * kGravity * 6.0f));
    CHECK(maxSpeed < 1.10f * std::sqrt(2.0f * kGravity * 6.0f));
    CHECK(maxStretch < 0.15f);           // the rope holds its length
    CHECK(minX < -3.0f);                 // it swung through to the far side
    CHECK(s.r.len > 5.5f);               // a taut swing is not reeled in
    CHECK(s.r.sawAir);
}

static void momentum_at_the_catch_is_kept()
{
    // caught right under the hook running sideways at 6 m/s: the swing rises about v^2 / 2g = 1.83 m
    const float v0[3] = { 6.0f, 0.0f, 0.0f };
    Sim s = At(0.0f, 12.0f, 0.0f, v0);
    float top = -1e9f;
    for (int i = 0; i < 90 * 2; ++i) {
        s.Step(1.0f / 90.0f);
        if (s.feet[1] + kHandUpM > top) { top = s.feet[1] + kHandUpM; }
    }
    CHECK(top - 12.0f > 1.4f);
    CHECK(top - 12.0f < 2.2f);
}

static void trigger_pull_closes_in_and_stops_at_its_speed()
{
    const float v0[3] = {};
    Sim s = At(0.0f, 10.0f, 0.0f, v0);   // hanging 10 m under the hook
    float maxClosing = 0.0f;
    for (int i = 0; i < 45; ++i) {   // 0.5 s
        s.Step(1.0f / 90.0f, 12.0f);
        if (s.last.closing > maxClosing) { maxClosing = s.last.closing; }
        CHECK(s.last.pulled || s.last.closing >= 11.9f);
    }
    CHECK(maxClosing <= 12.0f + 0.2f);     // never faster than the pull's speed
    CHECK(maxClosing > 6.0f);              // and it got going
    CHECK(s.r.len < 10.0f - 2.0f);         // the reel took the rope in behind it
    CHECK(s.last.dist < 8.0f);
}

static void pull_keeps_the_swing()
{
    // swinging sideways under the hook at 5 m/s; half a second of pull: the sideways speed is not taken away (the rope shortens, so
    // it grows - angular momentum)
    const float v0[3] = { 5.0f, 0.0f, 0.0f };
    Sim s = At(0.0f, 10.0f, 0.0f, v0);
    for (int i = 0; i < 20; ++i) { s.Step(1.0f / 90.0f, 12.0f); }
    CHECK(s.last.v[0] > 4.0f);
}

static void slack_is_reeled_in_never_below_the_minimum()
{
    // flying straight at the hook: slack all the way, the length follows the distance down to kMinLenM
    const float v0[3] = { -10.0f, 0.0f, 0.0f };
    Sim s = At(5.0f, 20.0f, 0.0f, v0);
    for (int i = 0; i < 40; ++i) {
        s.Step(1.0f / 90.0f);
        CHECK(s.r.len >= kMinLenM - 1e-5f);
        CHECK(s.r.len <= s.last.dist + 1e-3f || s.r.len == kMinLenM);
    }
    CHECK(s.r.len < 2.0f);
}

static void taut_rope_takes_the_outward_speed()
{
    const float v0[3] = { 8.0f, 0.0f, 0.0f };   // at full length, moving straight away
    Sim s = At(6.0f, 20.0f, 0.0f, v0);
    s.Step(1.0f / 90.0f);
    CHECK(s.last.closing > -0.5f);   // the outward 8 m/s is gone (only the stretch's small correction is left)
}

static void grounded_keeps_no_fall()
{
    const float v0[3] = { 0.0f, -3.0f, 0.0f };
    Sim s = At(3.0f, 15.0f, 0.0f, v0);
    s.Step(1.0f / 90.0f, 0.0f, true);
    CHECK(s.last.v[1] >= 0.0f);
    CHECK(!s.r.sawAir);
}

static void end_rules()
{
    Rope r;
    const float feet[3] = { 0.0f, 10.0f, 0.0f }, v0[3] = {};
    StartRope(r, kAnchor, feet, v0, 1000);
    CHECK(RopeEndCheck(r, false, false, 8.0f, 1500) == kReNone);
    CHECK(RopeEndCheck(r, true, false, 8.0f, 1500) == kReJump);
    CHECK(RopeEndCheck(r, false, true, 8.0f, 1500) == kReNone);   // caught standing: a leash, not a landing
    r.sawAir = true;
    CHECK(RopeEndCheck(r, false, true, 8.0f, 1500) == kReLanded);
    CHECK(RopeEndCheck(r, false, false, kArriveM - 0.1f, 1500) == kReArrived);
    CHECK(RopeEndCheck(r, false, false, 8.0f, 1000 + kMaxMs + 1) == kReTimeout);
    Rope off;
    CHECK(RopeEndCheck(off, true, true, 0.0f, 1500) == kReNone);
    // grapple_hold_to_swing: only releasing the left trigger lets go - not the jump, a landing, the hook or the time
    CHECK(RopeEndCheck(r, true, true, 8.0f, 1000 + kMaxMs + 1, true, true) == kReNone);
    // round 4: reaching the hook still lets go in hold mode (the game's own arrival is skipped with its contact / time ends)
    CHECK(RopeEndCheck(r, false, false, kArriveM - 0.1f, 1500, true, true) == kReArrived);
    CHECK(RopeEndCheck(r, false, false, 8.0f, 1500, true, false) == kReReleased);
    CHECK(RopeEndCheck(off, false, false, 8.0f, 1500, true, false) == kReNone);
    // round 4: the game's rope leaving its pull ends ours in either mode (the release still names itself first)
    CHECK(RopeEndCheck(r, false, false, 8.0f, 1500, true, true, false) == kReGameEnded);
    CHECK(RopeEndCheck(r, false, false, 8.0f, 1500, false, true, false) == kReGameEnded);
    CHECK(RopeEndCheck(r, false, false, 8.0f, 1500, true, false, false) == kReReleased);
    CHECK(RopeEndCheck(off, false, false, 8.0f, 1500, true, true, false) == kReNone);
}

static void rope_look()
{
    CHECK(!RopeTakesLook(true, true));    // ours, feature on: the view stays the headset's
    CHECK(RopeTakesLook(false, true));    // feature off: the game's view turn
    CHECK(RopeTakesLook(true, false));    // another player's rope: the game's
}

static void game_rope_kept_and_let_go_velocity()
{
    CHECK(KeepGameRopeOn(true, true, 3));
    CHECK(!KeepGameRopeOn(false, true, 3));   // not hold mode: the game's own rules
    CHECK(!KeepGameRopeOn(true, false, 3));   // no rope of ours
    CHECK(!KeepGameRopeOn(true, true, 0));    // the game's rope is not in its pull
    const float rope[3] = { 0.0f, 40.0f, 0.0f }, meas[3] = { 1.0f, -12.0f, 2.0f };
    float out[3];
    LetGoVelocity(kReGameEnded, rope, meas, true, out);   // run 17:29: the runaway +40 is not handed on, the feet's fall is
    CHECK(out[0] == 1.0f && out[1] == -12.0f && out[2] == 2.0f);
    LetGoVelocity(kReGameEnded, rope, meas, false, out);
    CHECK(out[1] == 40.0f);
    LetGoVelocity(kReReleased, rope, meas, true, out);    // a normal let go keeps the swing's own velocity
    CHECK(out[1] == 40.0f);
}

static void free_flight_after_letting_go()
{
    Rope r;
    r.on = false;
    r.v[0] = 8.0f; r.v[1] = 2.0f; r.v[2] = -1.0f;
    FreeStep(r, 0.1f, false);
    CHECK(Near(r.v[0], 8.0f, 1e-6f) && Near(r.v[2], -1.0f, 1e-6f));   // the sideways momentum is kept whole
    CHECK(Near(r.v[1], 2.0f - kGravity * 0.1f, 1e-5f));
    FreeStep(r, 1.0f, false);                                          // a hitch integrates as kMaxDtS
    CHECK(Near(r.v[1], 2.0f - kGravity * 0.2f, 1e-5f));
    r.v[1] = -4.0f;
    FreeStep(r, 0.01f, true);                                          // on the ground: no fall kept
    CHECK(r.v[1] == 0.0f);
}

static void wall_needs_a_run_of_calls()
{
    Rope r;
    r.on = true;
    r.v[0] = 6.0f; r.v[2] = 0.0f;
    const float sent[2] = { 6.0f, 0.0f }, blocked[2] = { 0.5f, 0.0f }, free_[2] = { 5.8f, 0.0f };
    CHECK(!WallCheck(r, sent, blocked));
    CHECK(!WallCheck(r, sent, free_));   // a free call breaks the run
    CHECK(!WallCheck(r, sent, blocked));
    CHECK(!WallCheck(r, sent, blocked));
    CHECK(WallCheck(r, sent, blocked));
    CHECK(Near(r.v[0], 0.5f, 1e-6f));
    const float slow[2] = { 0.8f, 0.0f };
    CHECK(!WallCheck(r, slow, blocked));   // under 1 m/s sent is never a wall
}

static void pull_speed_from_the_trigger()
{
    CHECK(PullMps(true, 0.9f, 15.0f) == 15.0f);
    CHECK(PullMps(true, 0.4f, 15.0f) == 0.0f);
    CHECK(PullMps(false, 1.0f, 15.0f) == 0.0f);   // grapple_trigger_pull off: a swing rope only
    CHECK(PullMps(true, 1.0f, 0.0f) == 0.0f);
    CHECK(PullMps(true, 0.9f, 15.0f, kPullSameHandThreshold) == 0.0f);   // the held grapple trigger part way: a swing
    CHECK(PullMps(true, 1.0f, 15.0f, kPullSameHandThreshold) == 15.0f);
}

static void hands()
{
    // grappling hand: the game's triggers trade places only for a right grappling hand, and only with the hook out / a rope on
    CHECK(!TriggersSwapped(kHandLeft, true, true));
    CHECK(!TriggersSwapped(kHandRight, false, false));
    CHECK(TriggersSwapped(kHandRight, true, false));
    CHECK(TriggersSwapped(kHandRight, false, true));
    // pull hand: the other trigger is withheld (it always lands on R2) while a rope holds with pulling on; the same hand never
    CHECK(WithholdR2(kHandLeft, kHandRight, true, true));
    CHECK(WithholdR2(kHandRight, kHandLeft, true, true));
    CHECK(!WithholdR2(kHandLeft, kHandLeft, true, true));
    CHECK(!WithholdR2(kHandLeft, kHandRight, false, true));
    CHECK(!WithholdR2(kHandLeft, kHandRight, true, false));
    CHECK(PullThreshold(kHandLeft, kHandLeft, true) == kPullSameHandThreshold);
    CHECK(PullThreshold(kHandLeft, kHandLeft, false) == 0.5f);
    CHECK(PullThreshold(kHandLeft, kHandRight, true) == 0.5f);
}

int main()
{
    pendulum_keeps_length_and_energy();
    momentum_at_the_catch_is_kept();
    trigger_pull_closes_in_and_stops_at_its_speed();
    pull_keeps_the_swing();
    slack_is_reeled_in_never_below_the_minimum();
    taut_rope_takes_the_outward_speed();
    grounded_keeps_no_fall();
    end_rules();
    game_rope_kept_and_let_go_velocity();
    rope_look();
    free_flight_after_letting_go();
    wall_needs_a_run_of_calls();
    pull_speed_from_the_trigger();
    hands();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
