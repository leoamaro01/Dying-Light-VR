// Standalone unit test for src/ovr2xr/climbprobe_logic.h - NOT part of the DLL build.
// docs/design_climbing_overhaul.md §6 P0 (the log-only climbing probe). Exit code = number of failures.
#include "climbprobe_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::climbprobe;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static void idle_never_logs()
{
    Window w;
    for (unsigned long long t = 0; t < 5000; t += 11) {
        const Out o = Step(w, false, t);
        CHECK(o.event == kNone && !o.sample && !o.capReached);
    }
    CHECK(w.lines == 0 && w.climbs == 0);
}

static void start_samples_tail_then_end()
{
    Window w;
    Out o = Step(w, true, 1000);
    CHECK(o.event == kStart && o.sample && !o.tail);
    CHECK(w.in && w.climbs == 1 && w.samples == 1 && w.lines == 2);
    o = Step(w, true, 1011);
    CHECK(o.event == kNone && o.sample && !o.tail);
    // controllers off: the tail still samples
    o = Step(w, false, 1022);
    CHECK(o.event == kNone && o.sample && o.tail);
    o = Step(w, false, 1011 + kTailMs);
    CHECK(o.event == kNone && o.sample && o.tail);
    // past the tail: the end event, not sampled
    o = Step(w, false, 1012 + kTailMs);
    CHECK(o.event == kEnd && !o.sample);
    CHECK(!w.in);
    o = Step(w, false, 2000 + kTailMs);
    CHECK(o.event == kNone && !o.sample);
}

static void climbing_again_inside_tail_keeps_one_window()
{
    Window w;
    Step(w, true, 0);
    Step(w, false, 100);
    const Out o = Step(w, true, 200);
    CHECK(o.event == kNone && o.sample && !o.tail);
    CHECK(w.climbs == 1);
}

static void second_climb_resets_per_climb_state()
{
    Window w;
    Step(w, true, 0);
    NoteState(w, 3);
    Step(w, false, kTailMs + 10);   // end
    CHECK(!w.in);
    const Out o = Step(w, true, kTailMs + 20);
    CHECK(o.event == kStart && w.climbs == 2 && w.samples == 1 && w.seqN == 0 && !w.seqOverflow);
}

static void per_climb_cap_says_once()
{
    Window w;
    int samples = 0, caps = 0;
    for (int i = 0; i < kMaxSamplesPerClimb + 50; ++i) {
        const Out o = Step(w, true, static_cast<unsigned long long>(i) * 11);
        samples += o.sample ? 1 : 0;
        caps += o.capReached ? 1 : 0;
    }
    CHECK(samples == kMaxSamplesPerClimb);
    CHECK(caps == 1);
    CHECK(w.lines == 1 + kMaxSamplesPerClimb + 1);   // start + samples + the cap line
}

// Run 4: a ledge grab alone gets kLedgeSamplesPerClimb; the window turning into a ladder climb gets the full cap from then on.
static void ledge_cap_then_ladder_cap()
{
    Window w;
    int samples = 0, caps = 0;
    for (int i = 0; i < kLedgeSamplesPerClimb + 30; ++i) {
        const Out o = Step(w, true, static_cast<unsigned long long>(i) * 11, kLedgeSamplesPerClimb);
        samples += o.sample ? 1 : 0;
        caps += o.capReached ? 1 : 0;
    }
    CHECK(samples == kLedgeSamplesPerClimb && caps == 1);
    const Out o = Step(w, true, 5000, kMaxSamplesPerClimb);   // the same window, now a ladder: samples again
    CHECK(o.sample && !o.capReached);
    NoteLadderState(w, 0);
    NoteLadderState(w, 0);
    NoteLadderState(w, 2);
    CHECK(w.ladSeqN == 2 && w.ladSeq[0] == 0 && w.ladSeq[1] == 2);
    Step(w, false, 9000);              // closes (past the tail)
    Step(w, true, 9100);               // a new window clears the ladder sequence
    CHECK(w.ladSeqN == 0 && !w.ladSeqOverflow);
}

static void session_cap_stops_everything()
{
    Window w;
    w.lines = kMaxLines - 1;
    Out o = Step(w, true, 0);
    CHECK(o.event == kStart && !o.sample);   // the start took the last line
    CHECK(w.lines == kMaxLines);
    o = Step(w, true, 10);
    CHECK(!o.sample && !o.capReached && o.event == kNone);
    o = Step(w, false, 10 + kTailMs + 1);
    CHECK(o.event == kNone && !w.in);        // closed, but no line left for the event
    CHECK(w.lines == kMaxLines);
}

static void state_sequence_folds_and_caps()
{
    Window w;
    Step(w, true, 0);
    NoteState(w, 1);
    NoteState(w, 1);
    NoteState(w, 4);
    NoteState(w, 1);
    CHECK(w.seqN == 3 && w.seq[0] == 1 && w.seq[1] == 4 && w.seq[2] == 1);
    for (int i = 0; i < kSeqMax + 5; ++i) { NoteState(w, 100 + i); }
    CHECK(w.seqN == kSeqMax && w.seqOverflow);
}

static void measured_velocity()
{
    const float a[3] = { 1.0f, 2.0f, 3.0f };
    const float b[3] = { 1.1f, 2.0f, 2.9f };
    float v[3] = { 7.0f, 7.0f, 7.0f };
    CHECK(MeasuredVel(a, b, 0.1, v));
    CHECK(std::fabs(v[0] - 1.0f) < 1e-4f && std::fabs(v[1]) < 1e-6f && std::fabs(v[2] + 1.0f) < 1e-4f);
    float u[3] = { 7.0f, 7.0f, 7.0f };
    CHECK(!MeasuredVel(a, b, 0.0, u));
    CHECK(!MeasuredVel(a, b, 0.5, u));       // a pause, not one update
    const float n[3] = { NAN, 0.0f, 0.0f };
    CHECK(!MeasuredVel(a, n, 0.01, u));
    CHECK(u[0] == 7.0f && u[1] == 7.0f && u[2] == 7.0f);
}

int main()
{
    idle_never_logs();
    start_samples_tail_then_end();
    climbing_again_inside_tail_keeps_one_window();
    second_climb_resets_per_climb_state();
    per_climb_cap_says_once();
    ledge_cap_then_ladder_cap();
    session_cap_stops_everything();
    state_sequence_folds_and_caps();
    measured_velocity();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
