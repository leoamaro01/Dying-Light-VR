// Standalone unit test for src/ovr2xr/climbhand_logic.h - NOT part of the DLL build.
// docs/design_climbing_overhaul.md §6 P1 / §8.5: the ledge search around a hand (FindLedge, against a box world), the low-wall /
// force-hang rules and the pad-thread machine. Exit code = number of failures.
#include "climbhand_logic.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace dlvr::climbhand;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// ---------------------------------------------------------------- a box world with a ONE-SIDED caster (like the engine's: a ray
// starting inside a box does not hit that box)
struct Box { float lo[3], hi[3]; };
struct World {
    std::vector<Box> boxes;
    int casts = 0;
    bool operator()(const float from[3], const float to[3], float hit[3])
    {
        ++casts;
        float bestT = 2.0f;
        for (const Box& b : boxes) {
            bool inside = true;
            for (int i = 0; i < 3; ++i) { inside = inside && from[i] > b.lo[i] && from[i] < b.hi[i]; }
            if (inside) { continue; }
            float t0 = 0.0f, t1 = 1.0f;
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i) {
                const float d = to[i] - from[i];
                if (std::fabs(d) < 1e-9f) {
                    ok = from[i] >= b.lo[i] && from[i] <= b.hi[i];
                    continue;
                }
                float ta = (b.lo[i] - from[i]) / d, tb = (b.hi[i] - from[i]) / d;
                if (ta > tb) { const float x = ta; ta = tb; tb = x; }
                if (ta > t0) { t0 = ta; }
                if (tb < t1) { t1 = tb; }
                ok = t0 <= t1;
            }
            if (ok && t0 < bestT) { bestT = t0; }
        }
        if (bestT > 1.0f) { return false; }
        for (int i = 0; i < 3; ++i) { hit[i] = from[i] + (to[i] - from[i]) * bestT; }
        return true;
    }
};

// A wall in front of the player (+Z), face at z = 1, top at y = 2, 4 m wide, 1 m thick; the player's feet at the origin.
static World WallWorld(float topY = 2.0f)
{
    World w;
    w.boxes.push_back({ { -2.0f, 0.0f, 1.0f }, { 2.0f, topY, 2.0f } });
    return w;
}
static const float kFeet[3] = { 0.0f, 0.0f, 0.0f };

static void ledge_at_hand_on_the_lip()
{
    World w = WallWorld();
    const float hand[3] = { 0.2f, 1.95f, 0.95f };   // just in front of the lip, slightly below the top
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kLedgeOk);
    CHECK(Near(L.lip[1], 2.0f, 1e-3f));
    CHECK(Near(L.lip[2], 1.0f, 1e-3f));
    CHECK(Near(L.into[2], 1.0f, 1e-3f) && Near(L.into[0], 0.0f, 1e-3f));
    CHECK(Near(L.a[1], 2.0f, 1e-3f) && Near(L.b[1], 2.0f, 1e-3f) && Near(std::fabs(L.a[0] - L.b[0]), 2.0f * kHalfEdgeM, 1e-3f));
    CHECK(L.dist < 0.1f);
    CHECK(L.rays == w.casts);
}

static void hand_inside_the_ledge_still_finds_it()
{
    World w = WallWorld();
    const float hand[3] = { 0.0f, 1.9f, 1.10f };   // 10 cm INTO the wall, below the top
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kLedgeOk);
    CHECK(Near(L.lip[1], 2.0f, 1e-3f) && Near(L.lip[2], 1.0f, 1e-3f));
}

static void hand_above_the_top_finds_the_lip()
{
    World w = WallWorld();
    const float hand[3] = { 0.0f, 2.15f, 1.05f };   // reaching over the edge, palm above the top
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kLedgeOk);
    CHECK(Near(L.lip[1], 2.0f, 1e-3f));
}

static void hand_beside_the_look_cone()
{
    World w = WallWorld();
    const float hand[3] = { 0.6f, 1.95f, 0.85f };   // off to the side: the face ray straight out still hits, the cone helps at the edge
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kLedgeOk);
}

static void tall_wall_has_no_top()
{
    World w = WallWorld(5.0f);   // the top is 3 m above the hand: not a ledge
    const float hand[3] = { 0.0f, 1.95f, 0.95f };
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kNoTop);
}

static void open_air_has_no_face()
{
    World w;   // nothing
    const float hand[3] = { 0.0f, 1.9f, 0.6f };
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kNoFace);
}

static void too_far_is_reported_with_distance()
{
    World w = WallWorld();
    const float hand[3] = { 0.0f, 1.65f, 0.9f };   // the face is near, but the top is 35 cm above the hand
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kTooFar);
    CHECK(L.dist > 0.3f && L.dist < 0.4f);
    CHECK(FindLedge(hand, kFeet, 0.5f, w, &L) == kLedgeOk);   // a longer reach takes it
}

static void hand_straight_over_the_feet_still_searches()
{
    // round 3: no direction is needed any more (a full circle of rays) - a wall 0.99 m away is just out of the rays' range
    World w = WallWorld();
    const float hand[3] = { 0.01f, 1.9f, 0.01f };
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kNoFace);
    const float close[3] = { 0.0f, 1.9f, 0.8f };   // 20 cm from the wall, straight over feet moved forward: found
    const float feet2[3] = { 0.0f, 0.0f, 0.8f };
    CHECK(FindLedge(close, feet2, 0.3f, w, &L) == kLedgeOk);
}

static void hand_reaching_sideways_over_the_head()
{
    // run 2's failure: the hand above the head and to the SIDE (feet -> hand points along the wall, not at it)
    World w = WallWorld();
    const float feet[3] = { 0.0f, 0.0f, 0.75f };
    const float hand[3] = { 0.25f, 2.05f, 0.85f };   // feet -> hand is mostly +X; the wall is +Z
    Ledge L;
    CHECK(FindLedge(hand, feet, 0.3f, w, &L) == kLedgeOk);
    CHECK(Near(L.into[2], 1.0f, 0.05f));
}

static void hand_far_above_the_top()
{
    World w = WallWorld();
    const float hand[3] = { 0.0f, 2.35f, 1.05f };   // 35 cm above the top, over it
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.4f, w, &L) == kLedgeOk);
    CHECK(Near(L.lip[1], 2.0f, 1e-3f));
}

// Run 4 ("I can go through walls"): the hand reached THROUGH a thin wall (z 1.0 - 1.1). The back face (z 1.1) is nearer the hand,
// but faces away from the body: the front lip must win, climbing away from the player - never back toward it.
static void hand_through_a_thin_wall_takes_the_front_face()
{
    World w;
    w.boxes.push_back({ { -2.0f, 0.0f, 1.0f }, { 2.0f, 2.0f, 1.1f } });
    const float hand[3] = { 0.0f, 1.95f, 1.2f };
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kLedgeOk);
    CHECK(Near(L.lip[2], 1.0f, 1e-3f) && L.into[2] > 0.9f);
}

static void only_a_back_face_is_refused()
{
    // a wall BEHIND the hand, seen from the body: the hand at z 1.3 is past a slab whose only face in range is its far side
    World w;
    w.boxes.push_back({ { -2.0f, 0.0f, 0.2f }, { 2.0f, 2.0f, 1.0f } });   // its front (z 0.2) is out of the rays' range
    const float hand[3] = { 0.0f, 1.92f, 1.2f };   // (no face ray at exactly y 2.0: the test caster's "inside" is strict)
    Ledge L;
    const int why = FindLedge(hand, kFeet, 0.3f, w, &L);
    if (why != kBackFace) { std::printf("  only_a_back_face_is_refused: got %d\n", why); }
    CHECK(why == kBackFace);
}

static void ledge_behind_a_fence_is_blocked()
{
    World w = WallWorld();
    w.boxes.push_back({ { -2.0f, 0.0f, 0.55f }, { 2.0f, 3.0f, 0.65f } });   // a tall thin fence between the body and the ledge
    const float hand[3] = { 0.0f, 1.95f, 0.95f };
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kBlocked);
}

static void top_inside_a_wall_has_no_room()
{
    // A tall wall (top 5 m) with a box nested inside it whose top is at 1.9: the top rays start inside the wall (one-sided: no hit)
    // and land on the nested box - a "top" inside the wall. The wall right above it refuses it.
    World w = WallWorld(5.0f);
    w.boxes.push_back({ { -1.0f, 0.0f, 1.05f }, { 1.0f, 1.95f, 1.9f } });
    const float hand[3] = { 0.0f, 1.95f, 0.95f };
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kNoRoom);
}

// Run 5: a 4 cm fence (top 1.32 m). The old 6 / 15 cm top insets both fell past its back face ("no top").
static void thin_fence_has_a_top()
{
    World w;
    w.boxes.push_back({ { -2.0f, 0.0f, 1.0f }, { 2.0f, 1.32f, 1.04f } });
    const float hand[3] = { 0.0f, 1.36f, 0.98f };
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.3f, w, &L) == kLedgeOk);
    CHECK(Near(L.lip[1], 1.32f, 1e-3f) && Near(L.lip[2], 1.0f, 1e-3f) && L.into[2] > 0.9f);
}

static void character_classes()
{
    CHECK(IsCharacterClass("HumanAI"));
    CHECK(IsCharacterClass("HumanAIVis"));
    CHECK(IsCharacterClass("ZombieAIVis"));
    CHECK(IsCharacterClass("ZombieAIBlend"));
    CHECK(IsCharacterClass("DeadRagdoll"));
    CHECK(IsCharacterClass("AI"));
    CHECK(!IsCharacterClass("AIs"));
    CHECK(!IsCharacterClass(""));
    CHECK(!IsCharacterClass(nullptr));
    CHECK(!IsCharacterClass("MeshObject"));
    CHECK(!IsCharacterClass("ZombieAttractor"));     // a world object, not a character
    CHECK(!IsCharacterClass("DeadRagdollSpawner"));  // exact match only
    CHECK(!IsCharacterClass("Vis"));
}

static void hang_transition()
{
    CHECK(HangTransition(true, 0, 8) == 1);
    CHECK(HangTransition(false, 0, 8) == 8);
    CHECK(HangTransition(true, 1, 8) == 8);    // a climb-up FROM a hang (the jump button) is the game's
    CHECK(HangTransition(true, 0, 4) == 4);    // the game's own hang-jump choice is not touched
    CHECK(HangTransition(true, 0, 1) == 1);
}

static void slanted_wall_gets_its_own_normal()
{
    // A wall turned 45 deg: one box per 10 cm step along the diagonal approximates it (the caster only knows boxes).
    World w;
    for (int i = -12; i <= 12; ++i) {
        const float c = i * 0.1f;
        w.boxes.push_back({ { c - 0.05f + 1.0f, 0.0f, -c - 0.05f + 1.0f }, { c + 0.05f + 1.6f, 2.0f, -c + 0.05f + 1.6f } });
    }
    const float hand[3] = { 0.9f, 1.95f, 0.9f };   // ~14 cm in front of the stepped face (x + z = 2)
    Ledge L;
    CHECK(FindLedge(hand, kFeet, 0.4f, w, &L) == kLedgeOk);
    // into the wall points roughly along (+1, 0, +1) / sqrt 2
    CHECK(L.into[0] > 0.5f && L.into[2] > 0.5f);
}

static void rules()
{
    const float z[3] = { 0, 0, 0 }, a[3] = { 1, 2, 3 };
    CHECK(!TargetValid(z, z));
    CHECK(TargetValid(a, z));
    CHECK(AutoLowWall(true, 1.2f, 0.0f, 1.4f));
    CHECK(!AutoLowWall(true, 1.6f, 0.0f, 1.4f));
    CHECK(!AutoLowWall(false, 1.0f, 0.0f, 1.4f));
    CHECK(!AutoLowWall(true, 1.0f, 0.0f, 0.0f));   // 0 = never
    CHECK(ForceHang(true, 1.8f, 0.0f, 1.6f));
    CHECK(!ForceHang(true, 1.5f, 0.0f, 1.6f));     // low: the game climbs over
    CHECK(!ForceHang(false, 2.5f, 0.0f, 1.6f));
    CHECK(IsHangState(1) && IsHangState(11) && IsHangState(14) && IsHangState(2) && IsHangState(7));
    CHECK(!IsHangState(0) && !IsHangState(8) && !IsHangState(4) && !IsHangState(5) && !IsHangState(-1) && !IsHangState(16));
    const float p[3] = { 1, 1, 0 }, s0[3] = { 0, 0, 0 }, s1[3] = { 2, 0, 0 };
    CHECK(Near(SegDist(p, s0, s1), 1.0f, 1e-6f));
}

// ---------------------------------------------------------------- the machine (round 7: per-hand searches, hold only on a ledge)
static In Base(unsigned long long t)
{
    In in;
    in.enabled = true;
    in.handOk[0] = in.handOk[1] = true;
    in.game.onFoot = true;
    in.game.ageMs = 10;
    in.catchMs = 1500;
    in.nowMs = t;
    return in;
}

static void press_searches_until_the_grab_starts()
{
    Machine m;
    In in = Base(0);
    Out o = Update(m, in);
    CHECK(o.event == kEvNone && !o.search[0] && !o.search[1]);
    in.grip[1] = true;
    in.nowMs = 10;
    o = Update(m, in);
    CHECK(o.searchStarted[1] && o.search[1] && !o.search[0] && m.phase == kIdle);
    CHECK(!o.withhold[1]);   // no ledge yet: the press is still the game's R1
    in.nowMs = 20;
    o = Update(m, in);
    CHECK(o.search[1] && !o.searchStarted[1]);   // still searching (a catch while falling)
    in.foundMs[1] = 25;      // the game thread found a ledge at the right hand and started the grab
    in.nowMs = 30;
    o = Update(m, in);
    CHECK(o.searchEnded[1] == kSeFound && !o.search[1] && o.withhold[1] && m.phase == kIdle && !o.joined[1]);
    in.game.egActive = true;
    in.game.egState = 0;
    in.nowMs = 40;
    o = Update(m, in);
    CHECK(o.event == kEvStarted && m.phase == kHanging && o.withhold[1]);
}

static void search_ends_on_release_or_timeout()
{
    {
        Machine m;
        In in = Base(0);
        Update(m, in);
        in.grip[0] = true;
        in.nowMs = 10;
        CHECK(Update(m, in).search[0]);
        in.grip[0] = false;
        in.nowMs = 20;
        const Out o = Update(m, in);
        CHECK(o.searchEnded[0] == kSeReleased && !o.search[0] && m.phase == kIdle);
    }
    {
        Machine m;
        In in = Base(0);
        Update(m, in);
        in.grip[0] = true;
        in.nowMs = 10;
        Update(m, in);
        in.nowMs = 10 + in.catchMs + 1;
        Out o = Update(m, in);
        CHECK(o.searchEnded[0] == kSeTimedOut && !o.search[0]);
        in.nowMs += 10;   // still squeezed: not a new press, no new search
        o = Update(m, in);
        CHECK(!o.search[0] && !o.searchStarted[0] && m.phase == kIdle);
    }
}

static void owned_or_unknown_hand_never_searches()
{
    Machine m;
    In in = Base(0);
    Update(m, in);
    in.owned[1] = true;      // a holster / reload / loot took the press
    in.grip[1] = true;
    in.handOk[0] = false;
    in.grip[0] = true;       // the left hand's pose is unknown
    in.nowMs = 10;
    const Out o = Update(m, in);
    CHECK(!o.search[0] && !o.search[1] && m.phase == kIdle && !o.withhold[0] && !o.withhold[1]);
}

// Run 3: loot by hand's severed-limb wait owns every press for up to 150 ms; the press must come back to the climb after it.
static void briefly_owned_press_comes_back()
{
    Machine m;
    In in = Base(0);
    Update(m, in);
    in.grip[0] = true;
    in.owned[0] = true;      // loot's wait holds it at the edge
    in.nowMs = 10;
    Out o = Update(m, in);
    CHECK(!o.search[0] && !o.lost[0] && !o.lateTake[0]);
    in.nowMs = 100;          // still owned, still inside the window
    o = Update(m, in);
    CHECK(!o.search[0]);
    in.owned[0] = false;     // loot lets go, the grip is still closed
    in.nowMs = 160;
    o = Update(m, in);
    CHECK(o.searchStarted[0] && o.search[0] && o.lateTake[0] && o.lateMs[0] == 150 && !o.lost[0]);
    in.nowMs = 170;          // no second take
    o = Update(m, in);
    CHECK(!o.lateTake[0] && o.search[0] && !o.searchStarted[0]);
}

static void owner_that_keeps_the_press_wins()
{
    {   // owned past the window: lost once, never a search even when the owner lets go later
        Machine m;
        In in = Base(0);
        Update(m, in);
        in.grip[1] = true;
        in.owned[1] = true;
        in.nowMs = 10;
        Update(m, in);
        in.nowMs = 10 + kLateTakeMs + 1;
        Out o = Update(m, in);
        CHECK(o.lost[1] && m.phase == kIdle);
        in.owned[1] = false;
        in.nowMs += 10;
        o = Update(m, in);
        CHECK(!o.lost[1] && !o.lateTake[1] && !o.search[1] && m.phase == kIdle);
    }
    {   // the grip opens while owned: lost, no search
        Machine m;
        In in = Base(0);
        Update(m, in);
        in.grip[1] = true;
        in.owned[1] = true;
        in.nowMs = 10;
        Update(m, in);
        in.grip[1] = false;
        in.owned[1] = false;
        in.nowMs = 50;
        const Out o = Update(m, in);
        CHECK(o.lost[1] && !o.lateTake[1] && !o.search[1] && m.phase == kIdle);
    }
    {   // disabled (a menu): an owned press is neither kept nor taken - it is forgotten
        Machine m;
        In in = Base(0);
        in.enabled = false;
        Update(m, in);
        in.grip[0] = true;
        in.owned[0] = true;
        in.nowMs = 10;
        const Out o = Update(m, in);
        CHECK(!o.lost[0] && !o.lateTake[0] && !m.pending[0]);
    }
}

// Round 7 (b): the second press does not replace the first - both hands search at once, either may find the ledge.
static void both_hands_search_at_once()
{
    Machine m;
    In in = Base(0);
    Update(m, in);
    in.grip[1] = true;
    in.nowMs = 10;
    Update(m, in);
    in.grip[0] = true;       // the left hand reaches out next
    in.nowMs = 20;
    Out o = Update(m, in);
    CHECK(o.search[0] && o.search[1] && o.searchStarted[0] && !o.searchStarted[1]);
    in.foundMs[1] = 25;      // the FIRST hand (right) finds it
    in.nowMs = 30;
    o = Update(m, in);
    CHECK(o.searchEnded[1] == kSeFound && o.search[0] && !o.search[1] && o.withhold[1] && !o.withhold[0]);
    in.game.egActive = true;
    in.game.egState = 0;
    in.nowMs = 40;
    o = Update(m, in);
    CHECK(o.event == kEvStarted && o.withhold[1] && !o.withhold[0] && o.search[0]);   // the left hand still looks (to join)
}

static Machine Hanging(In& in)   // the right hand found the ledge and hangs from it
{
    Machine m;
    in = Base(0);
    Update(m, in);
    in.grip[1] = true;
    in.nowMs = 10;
    Update(m, in);
    in.foundMs[1] = 12;
    in.game.egActive = true;
    in.game.egState = 1;
    in.nowMs = 20;
    Update(m, in);
    return m;
}

static void release_while_hanging_drops_once()
{
    In in;
    Machine m = Hanging(in);
    CHECK(m.phase == kHanging && m.held == 2u);
    in.grip[1] = false;
    in.nowMs = 30;
    Out o = Update(m, in);
    CHECK(o.event == kEvDropRequested && o.requestDrop && m.phase == kDropping);
    in.nowMs = 40;
    o = Update(m, in);
    CHECK(!o.requestDrop && o.event == kEvNone);
    in.game.egActive = false;
    in.game.egState = -1;
    in.nowMs = 50;
    o = Update(m, in);
    CHECK(o.event == kEvEnded && m.phase == kIdle);
}

// Round 7 (a): a press anywhere else while hanging holds nothing - releasing the ledge hand drops.
static void press_off_the_ledge_does_not_hold()
{
    In in;
    Machine m = Hanging(in);
    in.grip[0] = true;       // the left hand closes in the air
    in.nowMs = 30;
    Out o = Update(m, in);
    CHECK(o.search[0] && !o.withhold[0] && o.withhold[1] && m.held == 2u);
    in.grip[1] = false;      // the right hand lets go of the ledge
    in.nowMs = 40;
    o = Update(m, in);
    CHECK(o.requestDrop && m.phase == kDropping && !o.withhold[0]);
}

// A second hand that IS on the ledge joins, and keeps the hang when the first lets go (hand over hand).
static void second_hand_on_the_ledge_keeps_the_hang()
{
    In in;
    Machine m = Hanging(in);
    in.grip[0] = true;
    in.nowMs = 30;
    Update(m, in);
    in.foundMs[0] = 32;      // the game thread found a ledge at the left hand while the grab runs
    in.nowMs = 40;
    Out o = Update(m, in);
    CHECK(o.joined[0] && o.searchEnded[0] == kSeFound && o.withhold[0] && o.withhold[1] && m.held == 3u);
    in.grip[1] = false;
    in.nowMs = 50;
    o = Update(m, in);
    CHECK(!o.requestDrop && m.phase == kHanging && o.withhold[0] && !o.withhold[1]);
    in.grip[0] = false;
    in.nowMs = 60;
    CHECK(Update(m, in).requestDrop);
}

static void stale_find_does_not_count()
{
    In in;
    Machine m = Hanging(in);
    in.foundMs[0] = 15;      // found for an EARLIER search of the left hand
    in.grip[0] = true;
    in.nowMs = 30;           // this search starts at 30: 15 is older
    Out o = Update(m, in);
    in.nowMs = 40;
    o = Update(m, in);
    CHECK(!o.joined[0] && o.search[0] && m.held == 2u);
}

static void grab_that_never_starts_holds_nothing()
{
    Machine m;
    In in = Base(0);
    Update(m, in);
    in.grip[0] = true;
    in.nowMs = 10;
    Update(m, in);
    in.foundMs[0] = 11;      // found, but the start was refused / never became active
    in.nowMs = 20;
    Out o = Update(m, in);
    CHECK(o.withhold[0]);
    in.nowMs = 20 + kStartWaitMs + 1;
    o = Update(m, in);
    CHECK(o.event == kEvGrabLost && !o.withhold[0] && m.held == 0 && m.phase == kIdle);
}

static void no_drop_during_scripted_moves()
{
    In in;
    Machine m = Hanging(in);
    in.game.egState = 8;
    in.grip[1] = false;
    in.nowMs = 30;
    CHECK(!Update(m, in).requestDrop && m.phase == kHanging);
    in.game.egActive = false;
    in.nowMs = 50;
    CHECK(Update(m, in).event == kEvEnded && m.phase == kIdle);
}

static void drop_failure_retries_later()
{
    In in;
    Machine m = Hanging(in);
    in.grip[1] = false;
    in.nowMs = 30;
    CHECK(Update(m, in).requestDrop);
    in.nowMs = 30 + kDropWaitMs + 1;
    CHECK(Update(m, in).event == kEvDropFailed && m.phase == kHanging);
    in.nowMs += 10;
    CHECK(!Update(m, in).requestDrop);
    in.nowMs = 30 + kDropWaitMs + 1 + kDropRetryMs;
    CHECK(Update(m, in).requestDrop);
}

static void press_joins_a_climb_the_game_started_only_on_a_ledge()
{
    Machine m;
    In in = Base(0);
    in.game.egActive = true;   // a low wall the game climbs by itself
    in.game.egState = 0;
    Update(m, in);
    in.grip[0] = true;
    in.nowMs = 10;
    Out o = Update(m, in);
    CHECK(m.phase == kIdle && !o.withhold[0] && o.search[0]);   // a press alone holds nothing
    in.foundMs[0] = 12;
    in.nowMs = 20;
    o = Update(m, in);
    CHECK(m.phase == kHanging && o.withhold[0] && o.event == kEvStarted);
}

static void disabling_mid_climb_lets_go_of_nothing()
{
    In in;
    Machine m = Hanging(in);
    in.enabled = false;
    in.nowMs = 30;
    const Out o = Update(m, in);
    CHECK(o.event == kEvDisabled && !o.requestDrop && !o.withhold[1] && m.phase == kIdle);
    in.enabled = true;   // back on with the grip still closed: not a new press
    in.nowMs = 40;
    const Out o2 = Update(m, in);
    CHECK(o2.event == kEvNone && m.phase == kIdle && !o2.search[1]);
}

// ---------------------------------------------------------------- P2: the body follows the hands
static void pull_anchors_and_follows()
{
    Pull p;
    const bool ok[2] = { true, true };
    float hand[2][3] = { { 0.0f, 2.0f, 1.0f }, { 0.4f, 2.0f, 1.0f } };
    const float lip[2][3] = { { 0.0f, 2.0f, 1.0f }, { 0.4f, 2.0f, 1.0f } };   // both hands found the ledge right where they are
    const bool haveLip[2] = { true, true };
    // the right hand holds, the game is in its grab (state 0): no anchor yet
    PullOut o = PullStep(p, 2u, false, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(!o.drive && !p.has[1]);
    // the hang begins: the anchor latches on the ledge point, nothing to move
    o = PullStep(p, 2u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(o.drive && o.latched[1] && !p.has[0] && Near(o.err[1], 0.0f, 1e-6f));
    // the hand pulls down 20 cm: the body must rise 20 cm
    hand[1][1] = 1.8f;
    o = PullStep(p, 2u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(o.drive && Near(o.err[1], 0.2f, 1e-5f) && Near(o.err[0], 0.0f, 1e-6f) && Near(o.dist[1], 0.2f, 1e-5f));
    // the left hand joins (on its ledge point, where it is), the error is the mean of both
    o = PullStep(p, 3u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(o.latched[0] && Near(o.err[1], 0.1f, 1e-5f));
    // the game leaves the hold states (its own climb-up): anchors go, and re-latch after - on the SAME ledge points
    o = PullStep(p, 3u, false, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(!o.drive && !p.has[0] && !p.has[1]);
    o = PullStep(p, 3u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(o.latched[0] && o.latched[1] && Near(o.err[1], 0.1f, 1e-5f));
    // a hand that lets go stops counting; none left = no drive
    o = PullStep(p, 0u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(!o.drive);
}

// Round 2: the hand snaps onto its ledge point (the body moves to put it there), not where the controller was.
static void pull_snaps_to_the_ledge_point()
{
    Pull p;
    const bool ok[2] = { true, true };
    const float hand[2][3] = { { 0.0f, 0.0f, 0.0f }, { 0.3f, 1.80f, 0.85f } };   // the hand grabbed 20 cm below, 15 cm short
    const float lip[2][3] = { { 0.0f, 0.0f, 0.0f }, { 0.3f, 2.00f, 1.00f } };
    const bool haveLip[2] = { false, true };
    const PullOut o = PullStep(p, 2u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(o.latched[1] && Near(p.anchor[1][1], 2.0f, 1e-6f) && Near(p.anchor[1][2], 1.0f, 1e-6f));
    CHECK(o.drive && Near(o.err[1], 0.2f, 1e-5f) && Near(o.err[2], 0.15f, 1e-5f));   // the body rises and moves in to put it there
}

// Round 2: a hand pulled away from its ledge point lets go - it is never re-anchored.
static void pull_release_and_lost_pose()
{
    Pull p;
    bool ok[2] = { true, true };
    float hand[2][3] = { { 0.0f, 2.0f, 1.0f }, { 0.4f, 2.0f, 1.0f } };
    const float lip[2][3] = { { 0.0f, 2.0f, 1.0f }, { 0.4f, 2.0f, 1.0f } };
    const bool haveLip[2] = { true, true };
    PullStep(p, 3u, true, ok, hand, lip, haveLip, 0.45f, 0);
    hand[0][2] = 0.5f;   // the left hand pulled back 50 cm off its ledge point
    PullOut o = PullStep(p, 3u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(o.released[0] && !p.has[0] && !o.released[1] && o.drive && Near(o.err[2], 0.0f, 1e-6f));   // only the right drives
    // the caller clears its hold; if it did not, the next poll would re-latch on the ledge point - never on the hand
    o = PullStep(p, 3u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(Near(p.anchor[0][2], 1.0f, 1e-6f));
    ok[1] = false;       // the right hand's pose is lost: its anchor stays, nothing from it this poll
    o = PullStep(p, 2u, true, ok, hand, lip, haveLip, 0.45f, 0);
    CHECK(!o.drive && p.has[1]);
}

// Round 3 (run 7): the grab animation leaves the hand 0.5 m from its point when the hold begins - no release until it settles.
static void pull_no_release_before_settling()
{
    Pull p;
    const bool ok[2] = { true, true };
    float hand[2][3] = { { 0.0f, 0.0f, 0.0f }, { 0.3f, 1.50f, 1.00f } };
    const float lip[2][3] = { { 0.0f, 0.0f, 0.0f }, { 0.3f, 2.02f, 1.00f } };   // 0.52 m above the hand
    const bool haveLip[2] = { false, true };
    PullOut o = PullStep(p, 2u, true, ok, hand, lip, haveLip, 0.45f, 1000);
    CHECK(o.latched[1] && !o.released[1] && o.drive && !p.settled[1]);
    hand[1][1] = 1.95f;   // the body brought it within 7 cm: settled
    o = PullStep(p, 2u, true, ok, hand, lip, haveLip, 0.45f, 1200);
    CHECK(!o.released[1] && p.settled[1]);
    hand[1][1] = 1.50f;   // now pulled 0.52 m away again: released
    o = PullStep(p, 2u, true, ok, hand, lip, haveLip, 0.45f, 1300);
    CHECK(o.released[1]);
    // never settles (blocked): after kSettleMaxMs the release distance applies anyway
    Pull q;
    hand[1][1] = 1.50f;
    PullStep(q, 2u, true, ok, hand, lip, haveLip, 0.45f, 0);
    o = PullStep(q, 2u, true, ok, hand, lip, haveLip, 0.45f, kSettleMaxMs / 2);
    CHECK(!o.released[1]);
    o = PullStep(q, 2u, true, ok, hand, lip, haveLip, 0.45f, kSettleMaxMs + 1);
    CHECK(o.released[1]);
}

static void look_states_refused()
{
    CHECK(RefuseLookState(true, 1, 2) && RefuseLookState(true, 1, 3));
    CHECK(!RefuseLookState(false, 1, 2));
    CHECK(!RefuseLookState(true, 1, 13) && !RefuseLookState(true, 1, 14));   // the same call site's shimmy-up / blocked stay
    CHECK(!RefuseLookState(true, 3, 2));
}

static void pull_velocity()
{
    float v[3];
    const float e[3] = { 0.0f, 0.08f, 0.0f };
    PullVelocity(e, 0.08f, 4.0f, v);
    CHECK(Near(v[1], 1.0f, 1e-5f));
    const float big[3] = { 0.0f, 1.0f, 0.0f };
    PullVelocity(big, 0.08f, 4.0f, v);
    CHECK(Near(v[1], 4.0f, 1e-4f));   // capped
    const float bad[3] = { NAN, 0.0f, 0.0f };
    PullVelocity(bad, 0.08f, 4.0f, v);
    CHECK(v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f);
    CHECK(IsHoldState(1) && IsHoldState(14) && !IsHoldState(11) && !IsHoldState(0) && !IsHoldState(8));
}

// ---------------------------------------------------------------- P4 round 2 / 3: the game's moves from a hang, the stale ledge
static void stale_ledge_detected()
{
    const float hold[3] = { 0.0f, 3.5f, 1.0f }, into[3] = { 0.0f, 0.0f, 1.0f };
    const float hookSame[3] = { 0.3f, 3.45f, 1.0f }, hookLower[3] = { 0.0f, 2.3f, 1.0f }, hookAlong[3] = { 0.7f, 3.5f, 1.0f };
    const float dirSame[3] = { 0.1f, 0.0f, 1.0f }, dirSide[3] = { 1.0f, 0.0f, 0.0f }, dirNone[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(!HookMismatch(true, hold, into, true, hookSame, dirSame));
    CHECK(HookMismatch(true, hold, into, true, hookLower, dirSame));   // run 12: a catch on a higher ledge, the game on the lower one
    CHECK(HookMismatch(true, hold, into, true, hookAlong, dirSame));   // hand over hand along the ledge, 0.7 m
    // run 13: both hands moved onto an ORTHOGONAL ledge at a corner - the points are close, only the direction differs
    CHECK(HookMismatch(true, hold, into, true, hookSame, dirSide));
    CHECK(!HookMismatch(true, hold, into, true, hookSame, dirNone));   // no direction known: the distance test only
    CHECK(!HookMismatch(false, hold, into, true, hookLower, dirSame)); // no hand anchored: unknown, not stale
    // round 14 (run 18 #9): after a re-target the player's ledge is the held one, but the grab's own copy is still the bottom ledge
    CHECK(HeldStale(true, hold, into, true, hookSame, dirSame, true, hookLower));
    CHECK(HeldStale(true, hold, into, true, hookLower, dirSame, true, hookSame));    // the player's ledge older: stale as before
    CHECK(!HeldStale(true, hold, into, true, hookSame, dirSame, true, hookSame));    // both the held one
    CHECK(!HeldStale(true, hold, into, true, hookSame, dirSame, false, hookLower));  // the copy unread: the player's ledge decides
    CHECK(!HeldStale(false, hold, into, true, hookLower, dirSame, true, hookLower)); // no hand anchored: unknown, not stale
}

static void game_moves_from_a_hang()
{
    // args: handMode, stickShimmy, stickClimb, stickForward, ours, held, retargetFresh, from, to
    // (round 8: "held" = the ledge the hands hold is known - every move is re-targeted onto it first, stale or not; rounds 2-7 passed
    // HookMismatch's "stale" here and run 15 #17 reached up from a ledge the stale test called the same)
    // climb-up: stick forward + setting on, no held ledge known = the game's own
    CHECK(GameMoveFromHang(true, false, true, true, false, false, false, 1, 8) == kGmPass);
    CHECK(GameMoveFromHang(true, false, true, false, false, false, false, 1, 8) == kGmRefuse);  // no stick (round 3)
    CHECK(GameMoveFromHang(true, false, true, true, false, true, false, 1, 8) == kGmRetarget);  // held: re-target, it asks again
    CHECK(GameMoveFromHang(true, false, true, true, true, true, false, 1, 8) == kGmPass);      // ours always
    // run 13: the stick up on the top ledge reached the game as a shimmy UP (13) from the stale lower ledge - re-target, whatever
    // climb_stick_shimmy says (the game then asks for the climb-up from the right ledge)
    CHECK(GameMoveFromHang(true, true, true, true, false, true, false, 1, 13) == kGmRetarget);
    CHECK(GameMoveFromHang(true, false, true, true, false, true, false, 1, 13) == kGmRetarget);
    // a re-target that did not hold: the game's own move (logged by the caller), still subject to the stick rules
    CHECK(GameMoveFromHang(true, true, true, true, false, true, true, 1, 13) == kGmPass);
    CHECK(GameMoveFromHang(true, false, true, true, false, true, true, 1, 8) == kGmPass);
    CHECK(GameMoveFromHang(true, false, true, false, false, true, true, 1, 8) == kGmRefuse);
    // run 14: the reach-up (9; 5 with the option bit) - the game's own, re-targeted first on a stale ledge
    CHECK(GameMoveFromHang(true, true, true, false, false, false, false, 1, 9) == kGmPass);
    CHECK(GameMoveFromHang(true, false, false, false, false, true, false, 1, 9) == kGmRetarget);
    CHECK(GameMoveFromHang(true, false, false, false, false, true, true, 1, 5) == kGmPass);
    // the hang jumps (4, 6, 7) stay the game's
    CHECK(GameMoveFromHang(true, true, true, true, false, true, false, 1, 4) == kGmPass);
    // shimmy: only with climb_stick_shimmy
    CHECK(GameMoveFromHang(true, false, true, false, false, false, false, 1, 12) == kGmRefuse);
    CHECK(GameMoveFromHang(true, true, true, false, false, false, false, 1, 12) == kGmPass);
    // not from a hold state, other moves, hand mode off: the game's
    CHECK(GameMoveFromHang(true, false, false, false, false, false, false, 0, 8) == kGmPass);
    CHECK(GameMoveFromHang(true, false, false, false, false, true, false, 1, 16) == kGmPass);
    CHECK(GameMoveFromHang(false, false, false, false, false, true, false, 1, 12) == kGmPass);
    // round 19 / 20 (climb_skip_reach): the reach-up with nothing above is refused - after the re-target, not before it - whichever
    // call asks (the stick-up's search, or the hang's input handler the next update)
    CHECK(GameMoveFromHang(true, true, true, true, false, true, false, 1, 9, true) == kGmRetarget);
    CHECK(GameMoveFromHang(true, true, true, true, false, true, true, 1, 9, true) == kGmRefuse);
    CHECK(GameMoveFromHang(true, true, true, true, false, false, false, 1, 5, true) == kGmRefuse);
    CHECK(GameMoveFromHang(true, true, true, true, false, true, true, 1, 9, false) == kGmPass);   // setting off
    CHECK(GameMoveFromHang(true, true, true, true, false, true, true, 1, 4, true) == kGmPass);    // a hang jump stays the game's
}

// P5 round 1: the game's moves from a pole / ladder hang are all refused; the game's own ladder start is refused only from its
// three sites. (Round 1's centre-line reach, PoleInReach / PoleLedge, was replaced by the surface search in round 2.)
static void pole_hang_moves_and_ladder_start()
{
    // the game's moves from a pole hang: all refused, no re-target
    CHECK(GameMoveFromHang(true, true, true, true, false, true, false, 1, 8, false, true) == kGmRefuse);
    CHECK(GameMoveFromHang(true, true, true, true, false, true, false, 1, 12, false, true) == kGmRefuse);
    CHECK(GameMoveFromHang(true, true, true, true, false, true, false, 1, 9, false, true) == kGmRefuse);
    CHECK(GameMoveFromHang(true, true, true, true, true, true, false, 1, 8, false, true) == kGmPass);    // ours
    CHECK(GameMoveFromHang(true, true, true, true, false, true, false, 1, 16, false, true) == kGmPass);  // the drop
    // the game's ladder start
    CHECK(RefuseLadderStart(true, true, true, 7, true));
    CHECK(!RefuseLadderStart(true, true, true, 7, false));   // another caller (a script)
    CHECK(!RefuseLadderStart(true, false, true, 7, true));   // climb_ladder_by_hand off
    CHECK(!RefuseLadderStart(true, true, false, 7, true));   // another player
    CHECK(!RefuseLadderStart(true, true, true, 21, true));   // another group
    CHECK(!RefuseLadderStart(false, true, true, 7, true));   // hand mode off
}

// The ladder's use prompt is hidden only while hand mode refuses the game's own ladder start.
static void ladder_prompt_hidden_in_hand_mode()
{
    CHECK(HideLadderPrompt(true, true, true, true));
    CHECK(!HideLadderPrompt(true, true, true, false));   // any other object keeps its prompt
    CHECK(!HideLadderPrompt(true, true, false, true));   // the ladder gate is not live: the use key still climbs, keep it
    CHECK(!HideLadderPrompt(true, false, true, true));   // climb_ladder_by_hand off
    CHECK(!HideLadderPrompt(false, true, true, true));   // climb_by_hand off
    CHECK(HideZiplinePrompt(true, true, true, true));
    CHECK(!HideZiplinePrompt(true, true, true, false));
    CHECK(!HideZiplinePrompt(true, true, false, true));  // ziplines by hand not live this session: the game's way stays, keep it
    CHECK(!HideZiplinePrompt(true, false, true, true));  // climb_zipline_by_hand off
    CHECK(!HideZiplinePrompt(false, true, true, true));
    CHECK(RefuseZiplineJumpStart(true, true, true, true, 27, true));
    CHECK(!RefuseZiplineJumpStart(true, true, true, true, 27, false));   // another caller (ours goes through the trampoline anyway)
    CHECK(!RefuseZiplineJumpStart(true, true, true, false, 27, true));   // another player
    CHECK(!RefuseZiplineJumpStart(true, true, false, true, 27, true));   // ziplines by hand not live: the jump is the only way
    CHECK(!RefuseZiplineJumpStart(true, false, true, true, 27, true));   // setting off
    CHECK(!RefuseZiplineJumpStart(true, true, true, true, 7, true));     // another group
}

// P6 ziplines: the hand's distance to the grabbable part of a line (the last 4.5 m cut, as the game does) and the start rule.
static void zipline_grab_near_the_line()
{
    ZipLine z;
    z.a[0] = 0.0f; z.a[1] = 10.0f; z.a[2] = 0.0f;
    z.b[0] = 20.0f; z.b[1] = 0.0f; z.b[2] = 0.0f;
    const float l = std::sqrt(20.0f * 20.0f + 10.0f * 10.0f);
    z.d[0] = 20.0f / l; z.d[1] = -10.0f / l; z.d[2] = 0.0f;
    float c[3];
    const float onLine[3] = { 4.0f, 8.0f + 0.2f, 0.0f };   // 0.2 m above the line at x = 4
    const float d1 = ZipDistance(onLine, z, c);
    CHECK(d1 > 0.17f && d1 < 0.19f && std::fabs(c[0] - 3.92f) < 0.02f && std::fabs(c[1] - 8.04f) < 0.02f);   // the foot, uphill
    const float nearEnd[3] = { 19.0f, 0.5f, 0.0f };        // by the far end: inside the cut, so the nearest is 4.5 m back
    CHECK(ZipDistance(nearEnd, z, c) > 3.0f);
    const float beforeStart[3] = { -1.0f, 10.0f, 0.0f };   // past the start: clamped to A
    CHECK(std::fabs(ZipDistance(beforeStart, z, c) - 1.0f) < 1e-4f && c[0] == 0.0f);
    ZipLine shortLine;
    shortLine.b[0] = 3.0f; shortLine.d[0] = 1.0f;           // 3 m long: nothing left after the cut
    CHECK(ZipDistance(onLine, shortLine, c) < 0.0f);

    CHECK(ZipGrabOk(true, true, false, false, 0.2f, 0.35f));
    CHECK(!ZipGrabOk(true, true, false, false, 0.4f, 0.35f));   // too far
    CHECK(!ZipGrabOk(true, true, true, false, 0.2f, 0.35f));    // a ledge grab runs
    CHECK(!ZipGrabOk(true, true, false, true, 0.2f, 0.35f));    // already riding
    CHECK(!ZipGrabOk(true, false, false, false, 0.2f, 0.35f));  // setting off
    CHECK(!ZipGrabOk(true, true, false, false, -1.0f, 0.35f));  // no line
}

// climb_skip_grab_anim / climb_grab_move_mps: a hand grab due to hang skips state 0; a new anchor glides at the set speed until it
// settles, then the pull's own cap.
static void grab_skips_the_animation_and_glides()
{
    CHECK(SkipGrabAnim(true, true, true, true, true, 0));
    CHECK(!SkipGrabAnim(false, true, true, true, true, 0));   // setting off
    CHECK(!SkipGrabAnim(true, true, false, true, true, 0));   // not our grab (the game's own catch)
    CHECK(!SkipGrabAnim(true, true, true, false, true, 0));   // not due to hang (a low wall: the game climbs over)
    CHECK(!SkipGrabAnim(true, true, true, true, true, 1));    // already hanging
    CHECK(!SkipGrabAnim(true, true, true, true, false, 0));   // the grab is not running

    Pull p;
    const unsigned held = 1u;
    const bool ok[2] = { true, false };
    const float lip[2][3] = { { 0.0f, 2.0f, 0.0f }, {} };
    const bool haveLip[2] = { true, false };
    float hand[2][3] = { { 0.0f, 1.75f, 0.0f }, {} };   // 0.25 m under its ledge point
    PullOut o = PullStep(p, held, true, ok, hand, lip, haveLip, 0.4f, 1000);
    CHECK(o.latched[0] && o.drive);
    CHECK(GrabGliding(p));
    CHECK(GrabGlideCap(GrabGliding(p), 1.5f, 4.0f) == 1.5f);
    CHECK(GrabGlideCap(true, 0.1f, 4.0f) == kGrabGlideMinMps);
    CHECK(GrabGlideCap(true, 0.0f, 4.0f) == 4.0f);          // no glide speed: the pull's cap
    CHECK(GrabGlideCap(false, 1.5f, 4.0f) == 4.0f);
    Hold h;
    h.drive = true;
    for (int i = 0; i < 3; ++i) { h.err[i] = o.err[i]; }
    Motion m;
    const float feet[3] = { 0.0f, 0.0f, 0.0f };
    MotionOut mo = MotionStep(m, h, feet, 1000, 0.0f, GrabGlideCap(true, 1.5f, 4.0f));
    CHECK(mo.send && std::fabs(mo.v[1] - 1.5f) < 1e-4f);   // 0.25 / 0.08 = 3.1 m/s wanted, glided at 1.5
    hand[0][1] = 1.92f;                                     // the body brought the hand within kSettleM
    o = PullStep(p, held, true, ok, hand, lip, haveLip, 0.4f, 1200);
    CHECK(!GrabGliding(p));
}

// Round 4: room to climb up - a ceiling over the top decides stand / crouch / none; debris under kRoomFromM does not count.
static void climb_up_room_stand_crouch_none()
{
    const float lip[3] = { 0.0f, 1.0f, 0.0f }, into[3] = { 0.0f, 0.0f, 1.0f };   // top at y 1, the spot at z 0.5
    // a world of horizontal slabs over the spot's area: [bottom, top] heights; a vertical ray hits the first face it meets
    struct World {
        float lo, hi;   // one slab (lo = hi = -100: none)
        bool operator()(const float a[3], const float b[3], float h[3]) const
        {
            if (lo < -50.0f) { return false; }
            const bool up = b[1] > a[1];
            const float face = up ? lo : hi;   // one-sided: up rays see the underside, down rays the top
            const bool startInside = a[1] > lo && a[1] < hi;
            if (startInside) { return false; }
            const float y0 = up ? a[1] : b[1], y1 = up ? b[1] : a[1];
            if (face < y0 || face > y1) { return false; }
            h[0] = a[0]; h[1] = face; h[2] = a[2];
            return true;
        }
    };
    float hy = 0.0f;
    CHECK(ClimbUpRoom(lip, into, World{ -100.0f, -100.0f }, &hy) == kRoomStand);
    CHECK(ClimbUpRoom(lip, into, World{ 3.5f, 3.8f }, &hy) == kRoomStand);     // ceiling 2.5 m over the top
    CHECK(ClimbUpRoom(lip, into, World{ 2.4f, 2.7f }, &hy) == kRoomCrouch);    // 1.4 m: crouch only
    CHECK(std::fabs(hy - 1.4f) < 1e-4f);
    CHECK(ClimbUpRoom(lip, into, World{ 1.9f, 2.2f }, &hy) == kRoomNone);      // 0.9 m: no room
    CHECK(ClimbUpRoom(lip, into, World{ 1.2f, 1.6f }, &hy) == kRoomNone);      // a slab from 0.2 to 0.6 m (the up ray starts inside it)
    CHECK(ClimbUpRoom(lip, into, World{ 1.0f, 1.3f }, &hy) == kRoomStand);     // debris 0.3 m tall on the top: ignored
    const float none[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(ClimbUpRoom(lip, none, World{ 1.9f, 2.2f }, &hy) == kRoomStand);     // no direction: not tested
    const float feet[3] = { 0.0f, 1.0f, 0.0f };
    CHECK(StandRoomAt(feet, World{ 2.4f, 2.7f }) == false);
    CHECK(StandRoomAt(feet, World{ 3.5f, 3.8f }) == true);

    // the holds' tails after the grab ends
    CHECK(BodyHoldTail(true, 0, 5000));
    CHECK(BodyHoldTail(false, 5000, 5400));
    CHECK(!BodyHoldTail(false, 5000, 5000 + kBodyTailMs));
    CHECK(!BodyHoldTail(false, 0, 5000));
    CHECK(!BodyHoldTail(false, 5000, 5100, true));       // round 6: no body tail after a climb-up
    CHECK(BodyHoldTail(true, 5000, 5100, true));         // (still held while hanging)
    {
        ViewTail t;
        CHECK(ViewHoldTail(t, false, 5000, 5300, 0.40f));       // the head still 40 cm low: the rigid view stays
        CHECK(!ViewHoldTail(t, false, 5000, 5310, 0.05f));      // back near standing: eases out
        CHECK(!ViewHoldTail(t, false, 5000, 5400, 0.40f));      // round 6: ended once - the gap growing again does not restart it
        CHECK(ViewHoldTail(t, true, 0, 6000, 0.40f));           // a new grab
        CHECK(ViewHoldTail(t, false, 7000, 7100, 0.40f));       // its own tail
    }
    {
        ViewTail t;
        CHECK(!ViewHoldTail(t, false, 5000, 5000 + kViewTailMaxMs, 0.40f));   // timed out
    }
    {
        ViewTail t;
        CHECK(ViewHoldTail(t, false, 5000, 5300, NAN));         // unknown: held to the time-out
    }
    {   // round 6: settled - run 17:45 climb-up #2: the head rose then stayed ~25 cm short of the remembered point
        ViewTail t;
        const float gaps[] = { 0.35f, 0.30f, 0.27f, 0.25f, 0.25f, 0.25f, 0.25f };
        bool on = true;
        unsigned long long ms = 5000;
        int endAt = -1;
        for (int i = 0; i < 7; ++i) {
            ms = 5000 + 100ull * static_cast<unsigned long long>(i + 1);
            on = ViewHoldTail(t, false, 5000, ms, gaps[i]);
            if (!on && endAt < 0) { endAt = i; }
        }
        CHECK(endAt >= 0 && 5000 + 100ull * static_cast<unsigned long long>(endAt + 1) < 5000 + kViewTailMaxMs);
        CHECK(5000 + 100ull * static_cast<unsigned long long>(endAt + 1) - 5000 >= kViewSettleMinMs);
    }
    {   // round 6: the standing eye height refuses dips
        StandRef s;
        bool refused = false;
        CHECK(StandRefStep(s, 1.688f, 1000, &refused) == 1.688f && !refused);
        CHECK(StandRefStep(s, 1.31f, 2000, &refused) == 1.688f && refused);           // a landing dip
        CHECK(StandRefStep(s, 1.67f, 2100, &refused) == 1.67f && !refused);           // small changes follow
        unsigned long long t = 3000;
        float y = 0.0f;
        for (; t <= 3000 + kStandLowMs; t += 100) { y = StandRefStep(s, 1.50f, t, &refused); }
        CHECK(y == 1.50f);                                                            // a lower value held 10 s is taken
        StandRef g;
        StandRefStep(g, 1.688f, 0);
        StandRefStep(g, 1.31f, 1000);
        CHECK(StandRefStep(g, 1.31f, 1000 + kStandLowMs + 1) == 1.688f);              // two dips far apart are not "held"
    }

    // the game's zipline try-start is skipped by hand
    CHECK(SkipZipTry(true, true, true, true));
    CHECK(!SkipZipTry(true, false, true, true));
    CHECK(!SkipZipTry(true, true, false, true));
    CHECK(!SkipZipTry(true, true, true, false));
}

// P6 round 3: hands on the zipline - anchors ride with the cable, the other hand joins near it, letting go ends the ride.
static void zipline_hands_on_the_cable()
{
    ZipLine line;
    line.b[0] = 20.0f; line.d[0] = 1.0f;   // a level cable along +x at y 0
    ZipHold z;
    const float grab[3] = { 2.0f, 0.0f, 0.0f };
    ZipHoldStart(z, 1, grab, line);
    CHECK(z.on && z.has[1] && !z.has[0]);
    bool grip[2] = { false, true }, edge[2] = { false, false };
    const bool ok[2] = { true, true };
    float hand[2][3] = { { 0.0f, -0.5f, 0.0f }, { 2.0f, -0.1f, 0.0f } };
    const float none[3] = {}, adv[3] = { 0.2f, 0.0f, 0.0f };
    ZipHoldOut o = ZipHoldStep(z, true, grip, edge, ok, hand, none, 0.3f, 0.45f);
    CHECK(o.drive && std::fabs(o.err[1] - 0.1f) < 1e-5f && !o.end);   // the body moves the hand up onto the cable
    o = ZipHoldStep(z, true, grip, edge, ok, hand, adv, 0.3f, 0.45f);  // the ride carried 0.2 m: the anchor went with it
    CHECK(std::fabs(z.anchor[1][0] - 2.2f) < 1e-5f && std::fabs(o.err[0] - 0.2f) < 1e-5f && !o.end);
    // the left grip closes 0.2 m under the cable: it joins at the cable's nearest point
    grip[0] = true; edge[0] = true;
    hand[0][0] = 1.0f; hand[0][1] = -0.2f;
    hand[1][0] = 2.2f;
    o = ZipHoldStep(z, true, grip, edge, ok, hand, none, 0.3f, 0.45f);
    CHECK(o.joined[0] && z.has[0] && std::fabs(z.anchor[0][0] - 1.0f) < 1e-5f && z.anchor[0][1] == 0.0f);
    // a grip closing too far from the cable does not join (fresh hold, left hand far)
    ZipHold z2;
    ZipHoldStart(z2, 1, grab, line);
    float far[2][3] = { { 1.0f, -0.6f, 0.0f }, { 2.0f, 0.0f, 0.0f } };
    o = ZipHoldStep(z2, true, grip, edge, ok, far, none, 0.3f, 0.45f);
    CHECK(!o.joined[0] && !z2.has[0]);
    // both open: the ride ends, a let-go (a launch may follow)
    edge[0] = false;
    grip[0] = grip[1] = false;
    o = ZipHoldStep(z, true, grip, edge, ok, hand, none, 0.3f, 0.45f);
    CHECK(o.end && o.endOpened && o.released[0] && o.released[1] && !z.on);
    // a slip: the hand pulled 0.6 m off its anchor with the grip closed
    ZipHoldStart(z, 0, grab, line);
    grip[0] = true;
    float slip[2][3] = { { 2.0f, -0.6f, 0.0f }, {} };
    o = ZipHoldStep(z, true, grip, edge, ok, slip, none, 0.3f, 0.45f);
    CHECK(o.end && !o.endOpened && o.slipped[0]);
    // the game ends the ride (the line's end): the hold ends with no request
    ZipHoldStart(z, 0, grab, line);
    o = ZipHoldStep(z, false, grip, edge, ok, hand, none, 0.3f, 0.45f);
    CHECK(o.gameEnded && !o.end && !z.on);
}

// P6 X1: a launch caught by a new grab pays once, inside the window, only when the feet travelled kLaunchXpMinM.
static void launch_catch_pays_the_jump_xp()
{
    LaunchXp x;
    const float f0[3] = { 0.0f, 0.0f, 0.0f }, up[3] = { 0.3f, 1.2f, 0.4f }, near[3] = { 0.2f, 0.5f, 0.0f };
    float d = 0.0f;
    CHECK(!LaunchCatchPays(x, 1000, up, &d) && d < 0.0f);   // no launch armed
    ArmLaunchXp(x, 1000, f0, true);
    CHECK(LaunchCatchPays(x, 1800, up, &d) && d > 1.2f && d < 1.4f);
    CHECK(!LaunchCatchPays(x, 1900, up, &d) && d < 0.0f);   // one launch pays once
    ArmLaunchXp(x, 1000, f0, true);
    CHECK(!LaunchCatchPays(x, 1500, near, &d) && d > 0.5f && d < 0.6f);   // back onto (nearly) the same hold
    ArmLaunchXp(x, 1000, f0, true);
    CHECK(!LaunchCatchPays(x, 1000 + kLaunchXpWindowMs + 1, up, &d) && d < 0.0f);   // too long after the launch
    ArmLaunchXp(x, 1000, f0, false);
    CHECK(!LaunchCatchPays(x, 1200, up, &d));   // no feet at the launch: nothing to measure from
    CHECK(kXpTypeJump180 == 5);
}

// P5 slide: a light grip on a pole slides down until the bottom (extents) or the feet stall.
static void pole_slide_light_grip()
{
    const float light[2] = { 0.6f, 0.7f }, mixed[2] = { 0.6f, 1.0f }, full[2] = { 1.0f, 1.0f };
    CHECK(SlideGrip(2.5f, true, true, 3u, light, 0.85f));
    CHECK(!SlideGrip(2.5f, true, true, 3u, mixed, 0.85f));   // one hand squeezes fully: it holds
    CHECK(SlideGrip(2.5f, true, true, 1u, mixed, 0.85f));    // ...unless that hand is not holding
    CHECK(!SlideGrip(2.5f, true, true, 3u, full, 0.85f));
    CHECK(!SlideGrip(2.5f, false, true, 3u, light, 0.85f));  // a ledge: no slide
    CHECK(!SlideGrip(0.0f, true, true, 3u, light, 0.85f));   // off
    CHECK(!SlideGrip(2.5f, true, false, 3u, light, 0.85f));  // not a hold state
    CHECK(!SlideGrip(2.5f, true, true, 0u, light, 0.85f));   // no hand holds

    Slide s;
    float d = 0.0f;
    CHECK(SlideStep(s, true, 1000, 2.5f, 4.0f, true, 5.0f, true, 0.0f, &d) == kSlideStart && d == 0.0f);
    CHECK(SlideStep(s, true, 1020, 2.5f, 4.0f, true, 4.95f, true, 0.0f, &d) == kSlideMove && std::fabs(d - 0.05f) < 1e-4f);
    CHECK(SlideStep(s, true, 1200, 2.5f, 4.0f, true, 4.5f, true, 0.0f, &d) == kSlideMove && std::fabs(d - 0.125f) < 1e-4f);   // dt capped
    CHECK(SlideStep(s, true, 1220, 9.0f, 4.0f, true, 4.4f, true, 0.0f, &d) == kSlideMove && std::fabs(d - 0.064f) < 1e-4f);   // speed cap 3.2
    CHECK(SlideStep(s, true, 1240, 2.5f, 4.0f, true, 0.02f, true, 0.0f, &d) == kSlideBottom && !s.on);   // at the bottom
    CHECK(SlideStep(s, false, 1260, 2.5f, 4.0f, true, 0.02f, true, 0.0f, &d) == kSlideNone);

    // near the bottom: never below it
    s = Slide();
    SlideStep(s, true, 2000, 2.5f, 4.0f, true, 0.08f, true, 0.0f, &d);
    CHECK(SlideStep(s, true, 2040, 2.5f, 4.0f, true, 0.08f, true, 0.0f, &d) == kSlideMove && std::fabs(d - 0.08f) < 1e-4f);
    // the grip tightens: stop
    CHECK(SlideStep(s, false, 2060, 2.5f, 4.0f, true, 0.07f, true, 0.0f, &d) == kSlideStop && d == 0.0f);

    // no bottom known: the feet stalling ends it
    s = Slide();
    SlideStep(s, true, 3000, 2.5f, 4.0f, true, 2.0f, false, 0.0f, &d);
    CHECK(SlideStep(s, true, 3020, 2.5f, 4.0f, true, 1.9f, false, 0.0f, &d) == kSlideMove);
    CHECK(SlideStep(s, true, 3200, 2.5f, 4.0f, true, 1.895f, false, 0.0f, &d) == kSlideMove);
    CHECK(SlideStep(s, true, 3280, 2.5f, 4.0f, true, 1.89f, false, 0.0f, &d) == kSlideBottom);
}

// The jump button from a hang: up when looking at the wall / pole, a push-off along the look otherwise (yaw only).
static void jump_button_aims_by_look()
{
    CHECK(JumpButtonAsk(true, true, 1, 9, true, false));
    CHECK(JumpButtonAsk(true, true, 1, 4, true, false));
    CHECK(!JumpButtonAsk(true, true, 1, 9, true, true));    // the stick pushed up: the stick's reach, not a jump
    CHECK(!JumpButtonAsk(true, true, 1, 9, false, false));  // the stick-up search's own call
    CHECK(!JumpButtonAsk(true, true, 8, 9, true, false));   // not from a hold
    CHECK(!JumpButtonAsk(true, false, 1, 9, true, false));  // setting off
    CHECK(!JumpButtonAsk(false, true, 1, 9, true, false));  // hand mode off
    CHECK(!JumpButtonAsk(true, true, 1, 8, true, false));   // the climb-up is no jump

    const float into[3] = { 0.0f, 0.0f, -1.0f };
    float v[3], yaw = 0.0f;
    const float atWall[3] = { 0.3f, 0.9f, -1.0f };            // looking up at the wall, a little to the side
    CHECK(!LookJumpVelocity(atWall, true, into, 45.0f, v, &yaw) && yaw > 16.0f && yaw < 17.5f);
    CHECK(v[0] == 0.0f && v[2] == 0.0f && std::fabs(v[1] - kLaunchMaxUpMps) < 1e-4f);
    const float back[3] = { 0.0f, 0.8f, 1.0f };               // looking up but away from the wall: a push-off backward
    CHECK(LookJumpVelocity(back, true, into, 45.0f, v, &yaw) && yaw > 179.0f);
    CHECK(std::fabs(v[2] - kLaunchMaxSideMps) < 1e-4f && std::fabs(v[0]) < 1e-4f && std::fabs(v[1] - kLaunchMaxUpMps) < 1e-4f);
    const float side[3] = { 1.0f, 0.0f, 0.0f };               // along the wall: sideways
    CHECK(LookJumpVelocity(side, true, into, 45.0f, v, &yaw) && std::fabs(yaw - 90.0f) < 0.1f);
    CHECK(std::fabs(v[0] - kLaunchMaxSideMps) < 1e-4f);
    CHECK(!LookJumpVelocity(side, false, into, 45.0f, v, &yaw) && yaw < 0.0f);   // no look: up
    const float up[3] = { 0.0f, 1.0f, 0.0f };                 // straight up: no horizontal look -> up
    CHECK(!LookJumpVelocity(up, true, into, 45.0f, v, &yaw) && yaw < 0.0f);

    {   // round 21: on a pole the direction is the feet -> where the hands hold now
        const float feetJ[3] = { 0.0f, 0.0f, 0.0f }, holdJ[3] = { 0.0f, 1.5f, 0.4f }, wallJ[3] = { 1.0f, 0.0f, 0.0f };
        float in3[3];
        CHECK(JumpInto(true, feetJ, true, holdJ, true, wallJ, true, in3) == 2 && std::fabs(in3[2] - 1.0f) < 1e-4f);
        CHECK(JumpInto(false, feetJ, true, holdJ, true, wallJ, true, in3) == 1 && in3[0] == 1.0f);   // a ledge: the wall's
        const float overJ[3] = { 0.01f, 1.5f, 0.02f };                                               // the hold right over the feet
        CHECK(JumpInto(true, feetJ, true, overJ, true, wallJ, true, in3) == 1 && in3[0] == 1.0f);
        CHECK(JumpInto(true, feetJ, true, holdJ, false, wallJ, false, in3) == 0);
        // looking away from the pole (behind the hands): a push-off, whatever the grab's direction was
        const float away[3] = { 0.0f, 0.0f, -1.0f };
        JumpInto(true, feetJ, true, holdJ, true, wallJ, true, in3);
        CHECK(LookJumpVelocity(away, true, in3, 45.0f, v, &yaw) && yaw > 179.0f);
    }

    float f[3];
    const float ident[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    HeadForwardGame(ident, 0.0f, false, f);
    CHECK(std::fabs(f[0]) < 1e-5f && std::fabs(f[1]) < 1e-5f && std::fabs(f[2] + 1.0f) < 1e-5f);
    // a head turned 90 deg left about +Y (OpenXR): forward -X; the game yaw 0 keeps it
    const float s2 = std::sqrt(0.5f);
    const float left[4] = { 0.0f, s2, 0.0f, s2 };
    HeadForwardGame(left, 0.0f, false, f);
    CHECK(std::fabs(f[0] + 1.0f) < 1e-5f && std::fabs(f[2]) < 1e-5f);
    // the same rotation as the hands' offsets: (x c + z s, y, -x s + z c) with rel = 90 deg maps forward (0,0,-1) to (-1,0,0)
    HeadForwardGame(ident, 1.5707963f, false, f);
    CHECK(std::fabs(f[0] + 1.0f) < 1e-5f && std::fabs(f[2]) < 1e-5f);
    HeadForwardGame(ident, 1.5707963f, true, f);
    CHECK(std::fabs(f[0] - 1.0f) < 1e-5f && std::fabs(f[2]) < 1e-5f);
}

// P5 round 2: the grab point is on the climbable's own surface, wherever the hand is next to it - here a 1 m wide box (a ladder
// that is no thin pole) with its face at x = 1.0; other geometry (not the climbable) never counts.
static void pole_grab_on_its_surface()
{
    bool lastTarget = false;
    // the climbable: the box x 1.0..1.6, y 0..5, z -0.5..0.5; a wall (not it): z >= 0.8
    auto cast = [&](const float a[3], const float b[3], float hit[3]) {
        lastTarget = false;
        float tBest = 2.0f;
        bool target = false;
        const float dx = b[0] - a[0], dz = b[2] - a[2], dy = b[1] - a[1];
        if (dx != 0.0f) {   // the box's near face x = 1.0, entered moving +x
            const float t = (1.0f - a[0]) / dx;
            const float y = a[1] + dy * t, z = a[2] + dz * t;
            if (dx > 0.0f && t >= 0.0f && t <= 1.0f && y >= 0.0f && y <= 5.0f && z >= -0.5f && z <= 0.5f && t < tBest) {
                tBest = t; target = true;
            }
        }
        if (dz != 0.0f) {   // the wall z = 0.8, entered moving +z
            const float t = (0.8f - a[2]) / dz;
            if (dz > 0.0f && t >= 0.0f && t <= 1.0f && t < tBest) { tBest = t; target = false; }
        }
        if (tBest > 1.0f) { return false; }
        hit[0] = a[0] + dx * tBest; hit[1] = a[1] + dy * tBest; hit[2] = a[2] + dz * tBest;
        lastTarget = target;
        return true;
    };
    auto isTarget = [&]() { return lastTarget; };
    float hit[3] = {}, d = 0.0f;
    int rays = 0;
    const float nearSide[3] = { 0.8f, 2.0f, 0.3f };   // 0.2 m from its face, off its centre line
    CHECK(PoleSurfaceFind(nearSide, 0.30f, cast, isTarget, hit, &d, &rays));
    CHECK(std::fabs(hit[0] - 1.0f) < 1e-4f && std::fabs(hit[2] - 0.3f) < 1e-4f && d > 0.199f && d < 0.201f && rays == kSurfDirs);
    const float inside[3] = { 1.1f, 2.0f, 0.0f };     // pushed 10 cm into it: the face behind the hand
    CHECK(PoleSurfaceFind(inside, 0.30f, cast, isTarget, hit, &d, &rays) && std::fabs(hit[0] - 1.0f) < 1e-4f);
    const float far[3] = { 0.5f, 2.0f, 0.0f };        // 0.5 m away
    CHECK(!PoleSurfaceFind(far, 0.30f, cast, isTarget, hit, &d, &rays) && d < 0.0f);
    const float byWall[3] = { -1.0f, 2.0f, 0.65f };   // only the wall in reach: not the climbable
    CHECK(!PoleSurfaceFind(byWall, 0.30f, cast, isTarget, hit, &d, &rays));
    // the ledge made there: from the feet to the point
    const float feet[3] = { 0.0f, 0.0f, 0.3f }, fwd[3] = { 1.0f, 0.0f, 0.0f };
    Ledge L;
    const float pt[3] = { 1.0f, 2.0f, 0.3f };
    CHECK(PoleLedgeAt(pt, feet, fwd, &L) && L.into[0] > 0.999f && L.lip[1] == 2.0f && L.lip[2] == 0.3f);
    const float under[3] = { 1.0f, 0.0f, 0.3f };
    CHECK(PoleLedgeAt(pt, under, fwd, &L) && L.into[0] > 0.999f);   // feet under the point: its forward vector
    CHECK(std::fabs(L.a[2] - L.b[2]) > 0.49f && L.a[0] == L.b[0]);   // the edge runs across, 0.5 m
    int best = -1;
    CHECK(PoleSurfaceFind(nearSide, 0.30f, cast, isTarget, hit, &d, &rays, &best) && best == 0);   // the +x ray
}

// Round 19: a game shimmy carries the body; when the hang resumes, the shift is reported once (the held ledge points follow it).
static void game_move_shift_follows_the_shimmy()
{
    MoveShift m;
    float sh[3];
    const float a[3] = { 10.0f, 5.0f, 20.0f }, mid[3] = { 10.3f, 5.1f, 20.0f }, b[3] = { 10.6f, 5.0f, 20.05f };
    CHECK(!MoveShiftStep(m, true, 1, a, true, sh));         // hanging
    CHECK(!MoveShiftStep(m, true, 12, mid, true, sh));      // the game's shimmy carries the body
    CHECK(MoveShiftStep(m, true, 1, b, true, sh));          // back in the hang: the shift from where the shimmy began
    CHECK(sh[0] > 0.599f && sh[0] < 0.601f && sh[1] == 0.0f && sh[2] > 0.049f && sh[2] < 0.051f);
    CHECK(!MoveShiftStep(m, true, 1, b, true, sh));         // once
    // 14 (a blocked shimmy) is a hold state: nothing away
    CHECK(!MoveShiftStep(m, true, 14, b, true, sh));
    CHECK(!MoveShiftStep(m, true, 1, b, true, sh));
    // the grab itself (0) and a climb-up (8) never report a shift
    MoveShift g;
    CHECK(!MoveShiftStep(g, true, 0, a, true, sh));
    CHECK(!MoveShiftStep(g, true, 1, b, true, sh));         // the hang begins after the grab: no shift
    CHECK(!MoveShiftStep(g, true, 8, mid, true, sh));
    CHECK(!MoveShiftStep(g, false, -1, mid, true, sh));
    CHECK(!g.haveHold && !g.away);                          // the grab ended: reset
}

static void ledge_held_is_the_last_grabbed()
{
    Pull p;
    p.has[0] = p.has[1] = true;
    const bool lip[2] = { true, true };
    CHECK(LedgeHand(p, lip, 1) == 1);    // two ledges held: the last one grabbed
    CHECK(LedgeHand(p, lip, 0) == 0);
    p.has[1] = false;                    // the last one let go: the other
    CHECK(LedgeHand(p, lip, 1) == 0);
    p.has[0] = false;
    CHECK(LedgeHand(p, lip, 1) == -1);
    p.has[0] = true;
    CHECK(LedgeHand(p, lip, -1) == 0);   // none recorded yet: any anchored
    const bool noLip[2] = { false, true };
    p.has[1] = true;
    CHECK(LedgeHand(p, noLip, 0) == 1);  // a hand with no ledge point does not count
    // LedgeHeld: last grabbed vs the average (climb_ledge_last)
    Pull q;
    q.has[0] = q.has[1] = true;
    q.anchor[0][1] = 2.0f; q.anchor[1][1] = 3.0f;
    q.anchor[0][2] = 1.0f; q.anchor[1][2] = 1.0f;
    const float into[2][3] = { { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f } };
    float top = 0.0f, mid[3], dir[3];
    CHECK(LedgeHeld(q, lip, into, 1, true, &top, mid, dir) && Near(top, 3.0f, 1e-6f) && Near(dir[0], 1.0f, 1e-6f));
    CHECK(LedgeHeld(q, lip, into, 1, false, &top, mid, dir) && Near(top, 2.5f, 1e-6f) && Near(dir[0], 0.5f, 1e-6f) &&
          Near(dir[2], 0.5f, 1e-6f));
    Pull none;
    CHECK(!LedgeHeld(none, lip, into, 1, true, &top, mid, dir));
}


// ---------------------------------------------------------------- P4: hands drawn on the ledge
static void pin_shift_draws_the_hand_on_its_point()
{
    Pull p;
    p.has[1] = true;
    p.anchor[1][0] = 0.3f; p.anchor[1][1] = 2.0f; p.anchor[1][2] = 1.0f;
    const float hand[3] = { 0.3f, 1.9f, 0.95f };
    float s[3];
    CHECK(PinShift(true, p, 1, true, 0.11f, 0.45f, hand, s) && Near(s[1], 0.1f, 1e-5f) && Near(s[2], 0.05f, 1e-5f));
    CHECK(!PinShift(false, p, 1, true, 0.11f, 0.45f, hand, s));   // climb_pin_hands off
    CHECK(!PinShift(true, p, 0, true, 0.11f, 0.45f, hand, s));    // that hand holds nothing
    CHECK(!PinShift(true, p, 1, false, 0.11f, 0.45f, hand, s));   // no pose this poll
    CHECK(!PinShift(true, p, 1, true, 0.50f, 0.45f, hand, s));    // about to release: not drawn stretched to it
}

// ---------------------------------------------------------------- P3: mantle and launch
static Hold HoldAt(float topY, unsigned long long driveMs)
{
    Hold h;
    h.drive = true;
    h.driveMs = driveMs;
    h.haveTop = true;
    h.topY = topY;
    h.mid[0] = 0.0f; h.mid[1] = topY; h.mid[2] = 1.0f;   // the ledge 1 m ahead (+z), the wall goes in along +z
    h.into[0] = 0.0f; h.into[1] = 0.0f; h.into[2] = 1.0f;
    return h;
}

static void hands_mode_follows_and_records()
{
    Motion m;
    Hold h = HoldAt(1.7f, 0);
    h.err[1] = 0.08f;   // the hands pulled down 8 cm: rise at 1 m/s
    const float feet[3] = { 0.0f, 0.0f, 0.5f };
    MotionOut o = MotionStep(m, h, feet, 100, 1.2f, 4.0f);
    CHECK(o.send && Near(o.v[1], 1.0f, 1e-4f) && m.ringN == 1 && !o.climbUp);
    h.drive = false;   // no anchored hand: the game's own vector
    o = MotionStep(m, h, feet, 110, 1.2f, 4.0f);
    CHECK(!o.send);
}

// Round 2: the height rule only ASKS for the game's own climb-up; the hands keep moving the body until the game takes it.
static void auto_climb_up_asks_the_game()
{
    Motion m;
    Hold h = HoldAt(1.7f, 0);
    const float feet[3] = { 0.0f, 0.6f, 0.5f };   // lip 1.1 m above the feet, the hands still (no error)
    MotionOut o = MotionStep(m, h, feet, 1000, 1.2f, 4.0f);
    CHECK(!o.climbUp && o.send);   // round 4: at the height, but not held there yet
    o = MotionStep(m, h, feet, 1000 + kMantleDwellMs - 1, 1.2f, 4.0f);
    CHECK(!o.climbUp);
    o = MotionStep(m, h, feet, 1000 + kMantleDwellMs, 1.2f, 4.0f);
    CHECK(o.climbUp);                                     // held still at the top: the game's climb-up
    Motion z;
    o = MotionStep(z, h, feet, 1000, 0.0f, 4.0f);
    o = MotionStep(z, h, feet, 2000, 0.0f, 4.0f); // climb_mantle_m 0: never
    CHECK(!o.climbUp);
}

// Round 4: a jump flick crosses the climb-up height while still fast - no climb-up, so the release launches.
static void fast_pull_through_the_height_does_not_climb_up()
{
    Motion m;
    Hold h = HoldAt(1.7f, 0);
    h.err[1] = 0.32f;   // pulling at 4 m/s
    const float feet[3] = { 0.0f, 0.6f, 0.5f };
    for (unsigned long long t = 1000; t <= 1000 + 3 * kMantleDwellMs; t += 11) {
        const MotionOut o = MotionStep(m, h, feet, t, 1.2f, 4.0f, 1.5f);
        CHECK(!o.climbUp);
    }
    float v[3];
    CHECK(LaunchPick(m, 1000 + 3 * kMantleDwellMs, 1.5f, 1.25f, v));   // letting go now is a launch
    h.err[1] = 0.04f;   // slowed to 0.5 m/s at the top: the dwell starts now
    MotionOut o = MotionStep(m, h, feet, 2000, 1.2f, 4.0f, 1.5f);
    CHECK(!o.climbUp);
    o = MotionStep(m, h, feet, 2000 + kMantleDwellMs, 1.2f, 4.0f, 1.5f);
    CHECK(o.climbUp);
}

static void stick_climb_up_refused_only_when_off()
{
    CHECK(!RefuseClimbUp(true, true, true, false, 1, 8));    // climb_mantle_stick on (default), stick forward: the game's climb-up
    CHECK(RefuseClimbUp(true, true, false, false, 1, 8));    // run 11: on, but no stick - the game climbing up by itself is refused
    CHECK(RefuseClimbUp(true, false, true, false, 1, 8));    // off: the hang stays even with the stick
    CHECK(!RefuseClimbUp(true, false, false, true, 1, 8));   // ours (the height rule) always passes
    CHECK(!RefuseClimbUp(true, false, false, false, 0, 8));  // the grab's own end (0 -> 8, a low ledge) is not a climb from a hang
    CHECK(!RefuseClimbUp(false, false, false, false, 1, 8)); // hand mode off
    CHECK(!RefuseClimbUp(true, false, false, false, 1, 16));
}

// Round 13: a stick held forward from the run-up into the grab is not a push to climb up; a fresh push after the grab is.
static void stick_held_from_the_run_up_does_not_climb()
{
    CHECK(!StickClimbPush(1000, 990, 200, 500));    // pushed at 200, the grab at 500, still held: the run-up's push
    CHECK(StickClimbPush(1000, 990, 700, 500));     // let back and pushed again at 700: counts
    CHECK(StickClimbPush(1000, 990, 500, 500));     // pushed on the grab's own poll: counts
    CHECK(!StickClimbPush(1000, 800, 700, 500));    // the last forward reading is older than kStickFreshMs
    CHECK(!StickClimbPush(1000, 0, 0, 500));        // never pushed
    CHECK(StickClimbPush(1000, 990, 200, 0));       // the grab's start unknown: the push counts (never blocks)
    CHECK(!StickClimbPush(980, 990, 700, 500));     // a reading newer than now (thread skew) is not trusted
}

// Round 15: the climb-up's lift over the ledge top lowers the view / root by exactly that, never below the top, never past the cap.
static void climb_up_lift_over_the_top()
{
    CHECK(OverLedgeM(true, 146.83f + 0.5f, 146.83f) > 0.499f && OverLedgeM(true, 146.83f + 0.5f, 146.83f) < 0.501f);
    CHECK(OverLedgeM(true, 145.2f, 146.83f) == 0.0f);          // still below the top (hanging / pulling up): nothing
    CHECK(OverLedgeM(false, 147.5f, 146.83f) == 0.0f);         // not climbing up: nothing
    CHECK(OverLedgeM(true, 150.0f, 146.83f) == kOverLedgeMaxM); // capped
    CHECK(OverLedgeM(true, 147.0f, NAN) == 0.0f);              // the ledge unread
}

// Round 16: the lift is taken off until the body is down (a tail after the climb-up state), only for a climb-up from a hang.
static void climb_up_lift_tail_after_the_state()
{
    OverLedgeState s;
    int ev = 0;
    const float top = 100.0f;
    CHECK(OverLedgeStep(s, true, 1, 98.4f, top, 0, &ev) == 0.0f);            // hanging
    CHECK(OverLedgeStep(s, true, 8, 99.0f, top, 10, &ev) == 0.0f && ev == kOlStarted);
    const float up = OverLedgeStep(s, true, 8, 100.5f, top, 500, &ev);       // lifted 0.5 over the top
    CHECK(up > 0.499f && up < 0.501f);
    const float after = OverLedgeStep(s, false, -1, 100.4f, top, 900, &ev);  // the grab ended, the body still 0.4 up
    CHECK(after > 0.399f && after < 0.401f && ev == kOlClimbEnded);
    CHECK(OverLedgeStep(s, false, -1, 100.45f, top, 950, &ev) < 0.401f);     // never grows again in the tail
    CHECK(OverLedgeStep(s, false, -1, 100.0f, top, 1100, &ev) == 0.0f && ev == kOlTailEnded);   // down: done
    // the game's own climb-over straight from a grab (0 > 8): not a climb-up from a hang, nothing lowered
    OverLedgeState g;
    OverLedgeStep(g, true, 0, 99.0f, top, 0, &ev);
    CHECK(OverLedgeStep(g, true, 8, 101.0f, top, 10, &ev) == 0.0f && ev == kOlNone);
    // a tail that keeps coming down slowly ends at kOverLedgeTailMs, and fades out from there (no jump)
    OverLedgeState t;
    OverLedgeStep(t, true, 1, 98.0f, top, 0, &ev);
    OverLedgeStep(t, true, 8, 100.6f, top, 10, &ev);
    OverLedgeStep(t, false, -1, 100.6f, top, 20, &ev);
    unsigned long long ms = 20;
    float y = 100.6f;
    while (ms < 20 + kOverLedgeTailMs) { ms += 100; y -= 0.02f; OverLedgeStep(t, false, -1, y, top, ms, &ev); }
    CHECK(t.fading && !t.settled);
    const float f0 = t.fadeFromM;
    const float half = OverLedgeStep(t, false, -1, y, top, t.fadeStartMs + kOverLedgeFadeMs / 2, &ev);
    CHECK(half > f0 * 0.45f && half < f0 * 0.55f && ev == kOlNone);
    CHECK(OverLedgeStep(t, false, -1, y, top, t.fadeStartMs + kOverLedgeFadeMs, &ev) == 0.0f && ev == kOlTailEnded);
}

// Round 17 (run 21: "sometimes too low"): a body that stands on a floor higher than the ledge point stops coming down - the tail
// eases out from kOverLedgeSettleMs after it stopped, not at the 1.5 s time-out with a jump.
static void climb_up_lift_eases_out_when_the_body_settles()
{
    OverLedgeState s;
    int ev = 0;
    const float top = 100.0f;
    OverLedgeStep(s, true, 1, 98.4f, top, 0, &ev);
    OverLedgeStep(s, true, 8, 100.5f, top, 10, &ev);
    OverLedgeStep(s, false, -1, 100.30f, top, 100, &ev);                     // ended 30 cm up, coming down
    OverLedgeStep(s, false, -1, 100.10f, top, 150, &ev);
    float m = OverLedgeStep(s, false, -1, 100.09f, top, 200, &ev);           // stops 9 cm over the ledge point (a higher floor)
    CHECK(m > 0.089f && m < 0.101f && !s.fading);
    OverLedgeStep(s, false, -1, 100.09f, top, 250, &ev);
    CHECK(!s.fading);                                                         // still inside kOverLedgeSettleMs of the last descent
    m = OverLedgeStep(s, false, -1, 100.09f, top, 150 + kOverLedgeSettleMs, &ev);
    CHECK(s.fading && s.settled && m > 0.089f);                               // settled: the fade starts from what was left
    m = OverLedgeStep(s, false, -1, 100.09f, top, 150 + kOverLedgeSettleMs + kOverLedgeFadeMs, &ev);
    CHECK(m == 0.0f && ev == kOlTailEnded);
    CHECK(150 + kOverLedgeSettleMs + kOverLedgeFadeMs < 100 + kOverLedgeTailMs);   // well before the time-out
}

// Round 18: the climb-up from a hang ends once the feet stand on the top past the lip (run 21 #11's samples: up and 0.5 m in ~1.1 s
// into the game's 2.24 s).
static void fast_climb_up_ends_on_the_top_past_the_lip()
{
    const float lip[3] = { 320.03f, 141.78f, 339.77f };
    const float into[3] = { 0.37f, 0.0f, -0.93f };
    const float hanging[3] = { 320.015f, 140.05f, 340.096f };   // below, outside
    const float rising[3] = { 320.066f, 141.25f, 339.52f };     // past the lip, still below the top
    const float upNotIn[3] = { 320.03f, 141.90f, 339.70f };     // over the top, only ~6 cm in
    const float onTop[3] = { 320.10f, 141.85f, 339.33f };       // over the top, ~44 cm in
    CHECK(!FastClimbUpDone(true, true, 8, hanging, lip, into));
    CHECK(!FastClimbUpDone(true, true, 8, rising, lip, into));
    CHECK(!FastClimbUpDone(true, true, 8, upNotIn, lip, into));
    CHECK(FastClimbUpDone(true, true, 8, onTop, lip, into));
    CHECK(!FastClimbUpDone(false, true, 8, onTop, lip, into));   // climb_fast_climb_up off
    CHECK(!FastClimbUpDone(true, false, 8, onTop, lip, into));   // a climb-over straight from a grab
    CHECK(!FastClimbUpDone(true, true, 10, onTop, lip, into));   // already ending
    const float none[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(!FastClimbUpDone(true, true, 8, onTop, lip, none));    // no inward direction
}

// Round 16: climb_air_control_pct is AirControlPush's k - 10 % closes a tenth of the gap along the stick per update.
static void air_control_percent_closes_that_share_of_the_gap()
{
    const float h[2] = { 0.0f, 2.0f };           // 2 m/s along +z already
    const float w[3] = { 0.0f, 0.0f, 6.0f };     // the stick wants 6 m/s along +z: gap 4
    float out[3] = {};
    AirControlPush(h, w, 1.0f, out, 0.10f);
    CHECK(out[2] > 2.399f && out[2] < 2.401f);   // + 10 % of 4
    AirControlPush(h, w, 1.0f, out, 0.0f);
    CHECK(out[2] == 2.0f);                       // 0 % = momentum only
}

static void low_walls_switch()
{
    CHECK(AutoLowWall(true, 0.9f, 0.0f, 1.0f, true));
    CHECK(!AutoLowWall(true, 0.9f, 0.0f, 1.0f, false));   // climb_auto_low off (the default): the game starts nothing itself
}

static void up_assist_halves_the_angle()
{
    const float kRad = 3.14159265f / 180.0f;
    float v[3] = { 5.0f * std::sin(30.0f * kRad), 5.0f * std::cos(30.0f * kRad), 0.0f };   // 30 deg off straight up
    UpAssist(v, 0.5f);
    CHECK(Near(std::atan2(v[0], v[1]) / kRad, 15.0f, 1e-3f) && Near(std::sqrt(v[0] * v[0] + v[1] * v[1]), 5.0f, 1e-4f));
    float w[3] = { 1.0f, 1.7320508f, 0.0f };   // 30 deg, assist 1 = straight up
    UpAssist(w, 1.0f);
    CHECK(Near(w[0], 0.0f, 1e-5f) && Near(w[1], 2.0f, 1e-5f));
    float z[3] = { 1.0f, 1.7320508f, 0.0f };   // assist 0 = unchanged
    UpAssist(z, 0.0f);
    CHECK(Near(z[0], 1.0f, 1e-6f));
    float p[3] = { 3.0f, 1.0f, 0.0f };          // a push-off (72 deg from up): never bent
    UpAssist(p, 1.0f);
    CHECK(Near(p[0], 3.0f, 1e-6f) && Near(p[1], 1.0f, 1e-6f));
    float d[3] = { 1.0f, -1.0f, 0.0f };         // downward: untouched
    UpAssist(d, 1.0f);
    CHECK(Near(d[0], 1.0f, 1e-6f));
}

static void mantle_not_due_while_hanging_low_or_off()
{
    CHECK(!MantleDue(true, true, 1.7f, 0.0f, 1.2f));   // just hanging: the lip 1.7 m up
    CHECK(MantleDue(true, true, 1.7f, 0.5f, 1.2f));
    CHECK(!MantleDue(false, true, 1.7f, 0.5f, 1.2f));  // no hand anchored
    CHECK(!MantleDue(true, false, 1.7f, 0.5f, 1.2f));  // no ledge point
    CHECK(!MantleDue(true, true, 1.7f, 0.5f, 0.0f));   // climb_mantle_m 0 = off
}

static void launch_takes_the_fastest_recent_pull()
{
    Motion m;
    Hold h = HoldAt(1.7f, 0);
    const float feet[3] = { 0.0f, 0.0f, 0.5f };
    h.err[1] = 0.32f;   // 4 m/s up (the cap)
    MotionStep(m, h, feet, 1000, 1.2f, 4.0f);
    h.err[1] = 0.04f;   // the hands slow down just before they open
    MotionStep(m, h, feet, 1050, 1.2f, 4.0f);
    float v[3];
    CHECK(LaunchPick(m, 1100, 1.5f, 1.25f, v) && Near(v[1], kLaunchMaxUpMps, 1e-4f));   // 4 x 1.25 = 5, capped
    CHECK(!LaunchPick(m, 1000 + kLaunchWindowMs + 60, 1.5f, 1.25f, v));                 // only the slow one is recent
    CHECK(!LaunchPick(m, 1100, 0.0f, 1.25f, v));                                         // climb_launch_min_mps 0 = off
    // balance: never above the game's own jump - the rise at the cap is the default JumpMaxHeight
    CHECK(Near(kLaunchMaxUpMps * kLaunchMaxUpMps / (2.0f * kGravity), kGameJumpMaxHeightM, 2e-3f));
    CHECK(LaunchPick(m, 1100, 1.5f, 2.0f, v) && v[1] <= kLaunchMaxUpMps);                // the strongest scale stays capped
    Motion d;   // a fast pull downward is not a launch
    h.err[1] = -0.32f;
    MotionStep(d, h, feet, 1000, 1.2f, 4.0f);
    CHECK(!LaunchPick(d, 1010, 1.5f, 1.0f, v));
    Motion s;   // a push away from the wall (along -z) is: a push-off, its side speed capped
    h.err[1] = 0.0f;
    h.err[2] = -0.40f;
    MotionStep(s, h, feet, 1000, 1.2f, 8.0f);
    CHECK(LaunchPick(s, 1010, 1.5f, 1.0f, v) && Near(v[2], -kLaunchMaxSideMps, 1e-4f));
}

// ---------------------------------------------------------------- P4 round 5 / 6: the launch's carry (the game's own fall after the drop)
// A walk body that moves at what it is handed (horizontal) on a ballistic vertical, stopped by an optional wall at x = wallX.
struct CarrySim {
    float feet[3] = { 10.0f, 10.0f, 0.0f };
    float vy = 0.0f;
    float wallX = 1e9f;
    unsigned long long t = 1000;
    int Step(Carry& c, const float stick[3], bool momentum, float drag, float out[3], float dt = 0.011f)
    {
        const int r = CarryStep(c, feet, dt, t, stick, momentum, drag, out);
        if (r != kCeNone || !c.on) { return r; }
        feet[0] += out[0] * dt;
        if (feet[0] > wallX) { feet[0] = wallX; }   // the wall stops x
        feet[2] += out[2] * dt;
        feet[1] += vy * dt;
        vy -= kGravity * dt;
        t += static_cast<unsigned long long>(dt * 1000.0f + 0.5f);
        return r;
    }
};

static void carry_keeps_the_launch_speed()
{
    const float v0[3] = { 0.0f, 2.0f, -3.0f };
    const float noStick[3] = { 0.0f, 0.0f, 0.0f };
    float out[3];
    Carry c;
    CHECK(CarryStep(c, CarrySim().feet, 0.011f, 900, noStick, true, 0.1f, out) == kCeNone && out[2] == 0.0f);   // not started
    // run 10: the walk's own (zero) wanted vector overwrote a one-off velocity - the carry hands the momentum every call, both modes
    for (int mode = 0; mode < 2; ++mode) {
        Carry k;
        CarrySim s;
        s.vy = v0[1];
        StartCarry(k, v0, s.t);
        for (int i = 0; i < 40; ++i) {   // the rise and past the apex
            CHECK(s.Step(k, noStick, mode == 1, 0.0f, out) == kCeNone);
            CHECK(Near(out[0], 0.0f, 1e-3f) && Near(out[2], -3.0f, 1e-3f) && out[1] == 0.0f);   // the vertical stays the game's
        }
        CHECK(k.on);
    }
    // air drag: climb_air_drag 0.1 per second - ~4 % over 0.44 s, never more
    Carry d;
    CarrySim s;
    s.vy = v0[1];
    StartCarry(d, v0, s.t);
    for (int i = 0; i < 40; ++i) { s.Step(d, noStick, true, 0.1f, out); }
    CHECK(out[2] > -3.0f && out[2] < -2.85f);
}

static void carry_momentum_ignores_the_stick_in_the_air()
{
    const float v0[3] = { 3.0f, 1.0f, 0.0f };
    const float back[3] = { -3.0f, 0.0f, 0.0f };
    Carry c;
    CarrySim s;
    s.vy = v0[1];
    StartCarry(c, v0, s.t);
    float out[3];
    for (int i = 0; i < 20; ++i) { s.Step(c, back, true, 0.0f, out); }
    CHECK(Near(out[0], 3.0f, 1e-3f));   // climb_air_momentum: the stick has no say until it lands
}

// round 7 (revised) / round 8: climb_air_control ADDS the game's own per-update stick step to the momentum (tester: "add that 0.22
// cubed stick force to the momentum, not replace it"): k x the gap to the stick's speed along the stick, k = the air factor cubed -
// run 15 showed the walk hands the vector to the body as is, so the whole gap (round 7) let the stick take over at once
static void air_control_adds_the_sticks_push_to_the_momentum()
{
    const float k = kGameAirK;
    CHECK(Near(k, 0.010648f, 1e-6f));
    const float h[2] = { 3.0f, 0.0f };
    const float stick[3] = { 0.0f, -1.0f, 4.0f };
    float out[3] = { 0.0f, 7.0f, 0.0f };
    AirControlPush(h, stick, 0.0f, out);
    CHECK(Near(out[0], 3.0f, 1e-6f) && Near(out[2], 0.0f, 1e-6f) && out[1] == 7.0f);   // momentum only; the vertical untouched
    AirControlPush(h, stick, 1.0f, out);
    CHECK(Near(out[0], 3.0f, 1e-6f) && Near(out[2], 4.0f * k, 1e-6f));                  // momentum kept, the game's step added
    AirControlPush(h, stick, 0.5f, out);
    CHECK(Near(out[0], 3.0f, 1e-6f) && Near(out[2], 2.0f * k, 1e-6f));
    AirControlPush(h, stick, 3.0f, out);                                                 // clamped: never more than the game
    CHECK(Near(out[2], 4.0f * k, 1e-6f));
    AirControlPush(h, stick, 1.0f, out, 0.5f);                                           // the walk's own factor when read
    CHECK(Near(out[2], 2.0f, 1e-6f));
    const float none[3] = { 0.0f, -1.0f, 0.0f };                                         // stick let go: momentum, no game drag
    AirControlPush(h, none, 1.0f, out);
    CHECK(Near(out[0], 3.0f, 1e-6f) && Near(out[2], 0.0f, 1e-6f));
    const float fwd[3] = { 4.0f, 0.0f, 0.0f };                                           // along the momentum: k of the gap
    AirControlPush(h, fwd, 1.0f, out);
    CHECK(Near(out[0], 3.0f + k, 1e-6f));
    const float h2[2] = { 6.0f, 0.0f };                                                  // already faster: no push, never braked
    AirControlPush(h2, fwd, 1.0f, out);
    CHECK(Near(out[0], 6.0f, 1e-6f));
    const float back[3] = { -4.0f, 0.0f, 0.0f };                                         // against it: brakes as the game does
    AirControlPush(h, back, 1.0f, out);
    CHECK(Near(out[0], 3.0f - 7.0f * k, 1e-6f));
    // held for a second at ~72 updates a second (run 15's walk rate), the stick bends the path gradually: the speed along it grows
    // toward the stick's, never past it
    float hh[2] = { 3.0f, 0.0f };
    for (int i = 0; i < 72; ++i) { AirControlPush(hh, stick, 1.0f, out); hh[0] = out[0]; hh[1] = out[2]; }
    CHECK(Near(hh[0], 3.0f, 1e-5f) && hh[1] > 2.0f && hh[1] < 2.3f);   // 4 x (1 - (1 - k)^72) = 2.15 m/s
    // in a carry: the stick has no say until armMs, then its share
    const float v0[3] = { 3.0f, 1.0f, 0.0f };
    Carry c;
    CarrySim s;
    s.vy = v0[1];
    StartCarry(c, v0, s.t);
    s.Step(c, stick, true, 0.0f, out);
    CHECK(Near(out[0], 3.0f, 1e-4f) && Near(out[2], 0.0f, 1e-4f));
    for (int i = 0; i < 12; ++i) { s.Step(c, stick, true, 0.0f, out, 0.011f); }   // armed now (but CarrySim passes no air control)
    float o2[3];
    CarryStep(c, s.feet, 0.011f, s.t, stick, true, 0.0f, o2, 0.5f);
    CHECK(Near(o2[2], 2.0f * k, 1e-4f) && o2[0] > 2.9f);   // pushed sideways by the game's step, the momentum still there
}

static void carry_steers_with_the_stick_but_never_stacks()
{
    Carry c;
    const float v0[3] = { 3.0f, 1.0f, 0.0f };
    StartCarry(c, v0, 1000);
    const float f[3] = { 10.0f, 10.0f, 0.0f };
    float out[3];
    const float back[3] = { -3.0f, 0.0f, 0.0f };   // the stick pulls back as hard as the launch: it cancels
    CarryStep(c, f, 0.011f, 1000, back, false, 0.0f, out);
    CHECK(Near(out[0], 0.0f, 1e-5f) && Near(out[2], 0.0f, 1e-5f));
    const float same[3] = { 3.0f, 0.0f, 0.0f };    // the stick pushes the same way: never faster than the larger of the two
    CarryStep(c, f, 0.011f, 1011, same, false, 0.0f, out);
    CHECK(Near(out[0], 3.0f, 1e-4f));
    const float side[3] = { 0.0f, 0.0f, 2.0f };    // sideways: steers, the speed capped at the launch's 3 m/s
    CarryStep(c, f, 0.011f, 1022, side, false, 0.0f, out);
    CHECK(Near(std::sqrt(out[0] * out[0] + out[2] * out[2]), 3.0f, 1e-4f) && out[2] > 0.0f);
}

// round 6 (tester: "I don't think you are taking collisions into account"): a wall stops the part of the momentum into it and keeps
// the slide along it - in both modes, nothing is pushed into the wall again
static void carry_stops_at_a_wall_and_keeps_the_slide()
{
    const float v0[3] = { 3.0f, 1.0f, 2.0f };
    const float noStick[3] = {};
    for (int mode = 0; mode < 2; ++mode) {
        Carry c;
        CarrySim s;
        s.vy = v0[1];
        s.wallX = 10.5f;   // reached after ~0.17 s
        StartCarry(c, v0, s.t);
        float out[3];
        for (int i = 0; i < 40; ++i) { s.Step(c, noStick, mode == 1, 0.0f, out); }
        CHECK(Near(out[0], 0.0f, 1e-3f));    // into the wall: gone
        CHECK(Near(out[2], 2.0f, 1e-3f));    // along it: kept
    }
}

static void carry_ends_when_it_lands_or_times_out()
{
    const float v0[3] = { 2.0f, 0.5f, 0.0f };
    const float noStick[3] = {};
    for (int mode = 0; mode < 2; ++mode) {
        Carry c;
        CarrySim s;
        s.vy = v0[1];
        StartCarry(c, v0, s.t);
        float out[3];
        int r = kCeNone;
        for (int i = 0; i < 30 && r == kCeNone; ++i) { r = s.Step(c, noStick, mode == 1, 0.0f, out); }   // 0.33 s: falling
        CHECK(r == kCeNone && c.on);
        // the ground: the feet stop falling (and, with nothing pushing, stand dead still)
        s.vy = 0.0f;
        const float stopX = s.feet[0];
        for (int i = 0; i < kCarryLandCalls + 2 && r == kCeNone; ++i) {
            r = s.Step(c, noStick, mode == 1, 0.0f, out);
            s.feet[0] = stopX;   // stopped dead: the feet do not move at all
            s.vy = 0.0f;
        }
        CHECK(r == kCeLanded && !c.on);
        CHECK(CarryStep(c, s.feet, 0.011f, s.t + 11, noStick, mode == 1, 0.0f, out) == kCeNone && out[0] == 0.0f);   // the stick's
    }
    // a lone call with no physics step does not end it (kCarryLandCalls in a row)
    Carry c;
    CarrySim s;
    s.vy = v0[1];
    StartCarry(c, v0, s.t);
    float out[3];
    for (int i = 0; i < 30; ++i) { s.Step(c, noStick, true, 0.0f, out); }
    s.t += 11;
    CHECK(CarryStep(c, s.feet, 0.011f, s.t, noStick, true, 0.0f, out) == kCeNone && c.on);
    // the safety net only
    Carry tc;
    StartCarry(tc, v0, 5000);
    CHECK(CarryStep(tc, s.feet, 0.011f, 5000 + kCarryMaxMs + 1, noStick, true, 0.0f, out) == kCeTimeout && !tc.on);
}

// ---------------------------------------------------------------- P4 round 8: the walk's own airborne flag ends the carry
static void carry_ends_on_the_walks_own_flag()
{
    const float v0[3] = { 2.0f, 3.0f, 0.0f };
    const float noStick[3] = {};
    float out[3];
    // an air carry (the walk already holds the body: no arming wait) ends the call the walk says it is down - mid-rise included
    {
        Carry c;
        CarrySim s;
        s.vy = v0[1];
        StartCarry(c, v0, s.t, 0);
        int r = kCeNone;
        for (int i = 0; i < 5; ++i) { r = CarryStep(c, s.feet, 0.011f, s.t + 11 * i, noStick, true, 0.0f, out, 1.0f, kGameAirK, 1); }
        CHECK(r == kCeNone && c.on && c.sawAir);
        CHECK(CarryStep(c, s.feet, 0.011f, s.t + 60, noStick, true, 0.0f, out, 1.0f, kGameAirK, 0) == kCeLanded && !c.on);
    }
    // a launch the walk still holds as grounded for its first moments (run 15: flags 0x0001 for ~110 ms) is not ended by that
    {
        Carry c;
        CarrySim s;
        s.vy = v0[1];
        StartCarry(c, v0, s.t);
        int r = kCeNone;
        for (int i = 0; i < 10; ++i) { r = CarryStep(c, s.feet, 0.011f, s.t + 11 * i, noStick, true, 0.0f, out, 1.0f, kGameAirK, 0); }
        CHECK(r == kCeNone && c.on && !c.sawAir);
        r = CarryStep(c, s.feet, 0.011f, s.t + 120, noStick, true, 0.0f, out, 1.0f, kGameAirK, 1);
        CHECK(r == kCeNone && c.sawAir);
        CHECK(CarryStep(c, s.feet, 0.011f, s.t + 131, noStick, true, 0.0f, out, 1.0f, kGameAirK, 0) == kCeLanded);
    }
    // the flag set: the ballistic test no longer ends it (a body held up by something while the walk still says "in the air")
    {
        Carry c;
        CarrySim s;
        s.vy = 0.5f;
        const float v1[3] = { 2.0f, 0.5f, 0.0f };
        StartCarry(c, v1, s.t, 0);
        int r = kCeNone;
        for (int i = 0; i < 30; ++i) { r = CarryStep(c, s.feet, 0.011f, s.t + 11 * i, noStick, true, 0.0f, out, 1.0f, kGameAirK, 1); }
        CHECK(r == kCeNone && c.on);   // the feet never moved: the ballistic test would have called it landed
    }
}

// round 9 (run 16): a measured speed spike (the body pushed out of geometry) never makes the momentum faster than it was handed
static void carry_momentum_never_grows_from_a_spike()
{
    const float v0[3] = { 4.0f, 0.0f, 0.0f };
    const float noStick[3] = {};
    Carry c;
    StartCarry(c, v0, 1000, 0);
    float out[3] = {};
    float f[3] = { 0.0f, 10.0f, 0.0f };
    unsigned long long t = 1000;
    for (int i = 0; i < 20; ++i) { CarryStep(c, f, 0.011f, t, noStick, true, 0.0f, out, 0.0f, kGameAirK, 1); f[0] += out[0] * 0.011f; t += 11; }
    CHECK(Near(out[0], 4.0f, 1e-4f));
    f[0] += 4.0f * 0.011f;   // one step the body moved at 8 m/s (a depenetration push): never faster
    for (int i = 0; i < 10; ++i) { CarryStep(c, f, 0.011f, t, noStick, true, 0.0f, out, 0.0f, kGameAirK, 1); f[0] += out[0] * 0.011f; t += 11; }
    CHECK(Near(out[0], 4.0f, 1e-3f));
    for (int i = 0; i < 20; ++i) { CarryStep(c, f, 0.011f, t, noStick, true, 0.0f, out, 0.0f, kGameAirK, 1); f[0] += 0.5f * out[0] * 0.011f; t += 11; }
    CHECK(out[0] < 2.5f && out[0] > 0.0f);   // something held it back to half: the momentum follows
}

// round 10 (run 17): walk calls and physics steps out of step (a call with two steps, a call with none) never wear the momentum down
static void carry_momentum_survives_uneven_calls()
{
    const float v0[3] = { 6.0f, 0.0f, 0.0f };
    const float noStick[3] = {};
    Carry c;
    StartCarry(c, v0, 1000, 0);
    float out[3] = {};
    float f[3] = { 0.0f, 10.0f, 0.0f };
    unsigned long long t = 1000;
    float pending = 0.0f;   // physics time not yet stepped
    for (int i = 0; i < 70; ++i) {
        const float dt = (i % 3 == 0) ? 0.028f : 0.014f;   // uneven calls
        CarryStep(c, f, dt, t, noStick, true, 0.0f, out, 0.0f, kGameAirK, 1);
        pending += dt;
        while (pending >= 0.016f) { f[0] += out[0] * 0.016f; pending -= 0.016f; }   // 16 ms physics steps
        t += static_cast<unsigned long long>(dt * 1000.0f + 0.5f);
    }
    CHECK(Near(out[0], 6.0f, 1e-3f));
}

// round 11: climb_view_rigid eases in over kRigidRampS while climbing and out after; a hitch never jumps it
static void rigid_view_eases_in_and_out()
{
    float k = 0.0f;
    k = RigidRampStep(k, true, 0.0f);
    CHECK(k == 0.0f);
    for (int i = 0; i < 10; ++i) { k = RigidRampStep(k, true, 0.0125f); }   // 125 ms: half way
    CHECK(Near(k, 0.5f, 1e-4f));
    k = RigidRampStep(k, true, 1.0f);   // a 1 s hitch counts as 0.1 s
    CHECK(Near(k, 0.9f, 1e-4f));
    for (int i = 0; i < 10; ++i) { k = RigidRampStep(k, true, 0.016f); }
    CHECK(k == 1.0f);
    for (int i = 0; i < 40; ++i) { k = RigidRampStep(k, false, 0.016f); }
    CHECK(k == 0.0f);
}

// round 9 (run 16): a slip (a grip still closed when the hang ends) never launches
static void a_slip_is_a_plain_drop()
{
    CHECK(DropMayLaunch(false, false));
    CHECK(!DropMayLaunch(true, true));    // both slipped (run 16's launches)
    CHECK(!DropMayLaunch(false, true));   // one let go earlier, the other slipped
    CHECK(!DropMayLaunch(true, false));
    // round 10: torn off in a jump flick (a controller fast in the play space) launches; the yank (controllers still) does not
    CHECK(DropMayLaunch(true, true, 2.4f, 1.5f));
    CHECK(!DropMayLaunch(true, true, 0.3f, 1.5f));
    CHECK(!DropMayLaunch(true, true, 9.0f, 0.0f));   // climb_launch_min_mps 0 = never launch
    // HandSpeed: the peak over the window, then forgotten
    HandSpeed s;
    const float a[3] = { 0.0f, 1.0f, 0.0f }, b[3] = { 0.0f, 0.97f, 0.0f }, c[3] = { 0.0f, 0.969f, 0.0f };
    HandSpeedStep(s, true, a, 0.0, 0.15);
    HandSpeedStep(s, true, b, 0.01, 0.15);   // 3 m/s down
    CHECK(Near(HandSpeedRecent(s, 0.01, 0.15), 3.0f, 1e-3f));
    HandSpeedStep(s, true, c, 0.02, 0.15);   // 0.1 m/s: the peak stays
    CHECK(Near(HandSpeedRecent(s, 0.05, 0.15), 3.0f, 1e-3f));
    CHECK(HandSpeedRecent(s, 0.3, 0.15) == 0.0f);
}

// FeetVel: the body's velocity between walk calls (an air carry starts with it)
static void feet_velocity_between_walk_calls()
{
    FeetVel f;
    const float a[3] = { 1.0f, 2.0f, 3.0f };
    FeetVelStep(f, a, 0.0f);
    CHECK(f.have && !f.haveV);
    const float b[3] = { 1.03f, 2.04f, 3.0f };
    FeetVelStep(f, b, 0.01f);
    CHECK(f.haveV && Near(f.v[0], 3.0f, 1e-3f) && Near(f.v[1], 4.0f, 1e-3f) && Near(f.v[2], 0.0f, 1e-4f));
    FeetVelStep(f, b, 0.02f);                       // no physics step between the calls: no new sample, the last velocity kept
    CHECK(f.haveV && Near(f.v[0], 3.0f, 1e-3f) && Near(f.t, 0.01f, 1e-6f));
    const float c[3] = { 1.06f, 2.04f, 3.0f };
    FeetVelStep(f, c, 0.022f);                      // 12 ms after the last sample: measured
    CHECK(Near(f.v[0], 2.5f, 1e-3f));
    FeetVelStep(f, a, 1.0f);                        // a long gap: the next pair measures
    CHECK(!f.haveV && f.have);
    const float bad[3] = { NAN, 0.0f, 0.0f };
    FeetVelStep(f, bad, 1.01f);
    CHECK(!f.have && !f.haveV);
}

// ---------------------------------------------------------------- P4 round 5: the view follows the eye bone smoothly while climbing
static void view_smooth_glides_and_lets_go()
{
    ViewSmooth s;
    float corr[3];
    const float a[3] = { 0.2f, 1.6f, 0.0f }, b[3] = { -0.2f, 1.6f, 0.0f };
    ViewSmoothStep(s, false, a, 0.011f, corr);   // not climbing: nothing, not even started
    CHECK(!s.on && corr[0] == 0.0f);
    ViewSmoothStep(s, true, a, 0.011f, corr);    // starts AT the eye bone: no jump
    CHECK(s.on && corr[0] == 0.0f && corr[1] == 0.0f);
    ViewSmoothStep(s, true, b, 0.011f, corr);    // the head swung 40 cm in one update: the view moves only part of the way
    const float moved = a[0] - (b[0] + corr[0]);   // the view's x is b + corr; how far it went from a
    CHECK(corr[0] > 0.3f && corr[0] < 0.4f && moved > 0.0f && moved < 0.1f);
    for (int i = 0; i < 100; ++i) { ViewSmoothStep(s, true, b, 0.011f, corr); }   // 1.1 s: caught up
    CHECK(std::fabs(corr[0]) < 0.001f);
    // the cap: never more than kViewSmoothMaxM from the eye bone
    const float far[3] = { 2.0f, 1.6f, 0.0f };
    ViewSmoothStep(s, true, far, 0.0f, corr);
    CHECK(Near(std::sqrt(corr[0] * corr[0] + corr[1] * corr[1] + corr[2] * corr[2]), kViewSmoothMaxM, 1e-4f));
    // after climbing it eases back, then lets go (no jump at the end)
    int n = 0;
    while (s.on && n < 1000) { ViewSmoothStep(s, false, far, 0.011f, corr); ++n; }
    CHECK(!s.on && n > 5 && corr[0] == 0.0f);
    const float bad[3] = { NAN, 0.0f, 0.0f };
    ViewSmoothStep(s, true, a, 0.011f, corr);
    ViewSmoothStep(s, true, bad, 0.011f, corr);
    CHECK(!s.on && corr[0] == 0.0f);
}

int main()
{
    stale_ledge_detected();
    game_moves_from_a_hang();
    view_smooth_glides_and_lets_go();
    ledge_held_is_the_last_grabbed();
    game_move_shift_follows_the_shimmy();
    pole_hang_moves_and_ladder_start();
    pole_grab_on_its_surface();
    launch_catch_pays_the_jump_xp();
    ladder_prompt_hidden_in_hand_mode();
    zipline_grab_near_the_line();
    zipline_hands_on_the_cable();
    grab_skips_the_animation_and_glides();
    climb_up_room_stand_crouch_none();
    jump_button_aims_by_look();
    pole_slide_light_grip();
    pin_shift_draws_the_hand_on_its_point();
    hands_mode_follows_and_records();
    auto_climb_up_asks_the_game();
    fast_pull_through_the_height_does_not_climb_up();
    stick_climb_up_refused_only_when_off();
    stick_held_from_the_run_up_does_not_climb();
    climb_up_lift_over_the_top();
    climb_up_lift_tail_after_the_state();
    climb_up_lift_eases_out_when_the_body_settles();
    fast_climb_up_ends_on_the_top_past_the_lip();
    air_control_percent_closes_that_share_of_the_gap();
    low_walls_switch();
    up_assist_halves_the_angle();
    mantle_not_due_while_hanging_low_or_off();
    launch_takes_the_fastest_recent_pull();
    carry_keeps_the_launch_speed();
    carry_momentum_ignores_the_stick_in_the_air();
    air_control_adds_the_sticks_push_to_the_momentum();
    carry_steers_with_the_stick_but_never_stacks();
    carry_stops_at_a_wall_and_keeps_the_slide();
    carry_ends_on_the_walks_own_flag();
    feet_velocity_between_walk_calls();
    carry_momentum_never_grows_from_a_spike();
    carry_momentum_survives_uneven_calls();
    a_slip_is_a_plain_drop();
    rigid_view_eases_in_and_out();
    carry_ends_when_it_lands_or_times_out();
    pull_anchors_and_follows();
    pull_snaps_to_the_ledge_point();
    pull_release_and_lost_pose();
    pull_no_release_before_settling();
    look_states_refused();
    pull_velocity();
    ledge_at_hand_on_the_lip();
    hand_inside_the_ledge_still_finds_it();
    hand_above_the_top_finds_the_lip();
    hand_beside_the_look_cone();
    tall_wall_has_no_top();
    open_air_has_no_face();
    too_far_is_reported_with_distance();
    hand_straight_over_the_feet_still_searches();
    hand_reaching_sideways_over_the_head();
    hand_far_above_the_top();
    hang_transition();
    slanted_wall_gets_its_own_normal();
    rules();
    thin_fence_has_a_top();
    character_classes();
    hand_through_a_thin_wall_takes_the_front_face();
    only_a_back_face_is_refused();
    ledge_behind_a_fence_is_blocked();
    top_inside_a_wall_has_no_room();
    press_searches_until_the_grab_starts();
    search_ends_on_release_or_timeout();
    owned_or_unknown_hand_never_searches();
    briefly_owned_press_comes_back();
    owner_that_keeps_the_press_wins();
    both_hands_search_at_once();
    release_while_hanging_drops_once();
    press_off_the_ledge_does_not_hold();
    second_hand_on_the_ledge_keeps_the_hang();
    stale_find_does_not_count();
    grab_that_never_starts_holds_nothing();
    no_drop_during_scripted_moves();
    drop_failure_retries_later();
    press_joins_a_climb_the_game_started_only_on_a_ledge();
    disabling_mid_climb_lets_go_of_nothing();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
