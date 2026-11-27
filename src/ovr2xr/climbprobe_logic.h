// Climbing P0 probe - the pure decisions (docs/design_climbing_overhaul.md §6 P0 and §8). No OpenXR, no Windows, no logging, so
// src/tests/climbprobe_logic_test.cpp compiles it on its own. climbprobe.cpp owns every read and every LOG line.
// What it decides: when a "climb window" opens and closes (any climbing controller active, plus a tail after it so the hand-back
// to walking is recorded), which updates inside it get a sample line (capped per climb and per session - INV-9: a forgotten switch
// must not flood the log), the climb's compressed state sequence for the end-of-climb summary, and the measured velocity from two
// world positions.
#pragma once

#include <cmath>

namespace dlvr {
namespace climbprobe {

constexpr unsigned long long kTailMs = 600;   // keep sampling this long after the climb controllers go inactive (the hand-back)
constexpr int  kMaxSamplesPerClimb = 900;     // ~10 s at 90 updates/s - a long hang still shows its start and its exit
// Run 4 (2026-11-26): ledge grabs are mapped (design doc §8.4) and used the whole session budget by 20:03 (one hang = 900 samples),
// so the pole climb after it went unrecorded. A window with only a ledge grab gets this many samples; a ladder / zipline window
// keeps kMaxSamplesPerClimb.
constexpr int  kLedgeSamplesPerClimb = 120;
constexpr long kMaxLines = 8000;              // every sample, event and summary line this session together
constexpr int  kSeqMax = 24;                  // distinct consecutive states kept per climb for the summary

enum Event : int { kNone = 0, kStart = 1, kEnd = 2 };

struct Window {
    bool               in = false;            // a climb window is open
    unsigned long long startMs = 0;           // when it opened
    unsigned long long lastClimbMs = 0;       // the last update a climbing controller was active
    int                samples = 0;           // sample lines given to this climb
    int                climbs = 0;            // windows opened this session
    long               lines = 0;             // lines used this session (samples + events + summaries)
    int                seq[kSeqMax] = {};     // the state sequence (consecutive duplicates folded)
    int                seqN = 0;
    bool               seqOverflow = false;   // more distinct states came than kSeqMax
    bool               capSaid = false;       // the per-climb cap line was given for this climb
    int                ladSeq[kSeqMax] = {};  // the LadderController's state sequence (consecutive duplicates folded)
    int                ladSeqN = 0;
    bool               ladSeqOverflow = false;
};

struct Out {
    int  event = kNone;       // kStart on the update the window opens, kEnd on the update it closes (that update is not sampled)
    bool sample = false;      // log a sample line for this update
    bool capReached = false;  // this climb just hit its per-climb cap: say so once
    bool tail = false;        // the sample is in the tail (no climbing controller active any more)
};

// One player update. climbing = any climbing controller is active this update. Lines are counted here, so the caller logs exactly
// what Out says and nothing more. An event line is only granted while the session has lines left (the summary needs one too).
// capPerClimb: this update's per-climb sample cap (kLedgeSamplesPerClimb for a ledge grab alone, else kMaxSamplesPerClimb); a
// window that turns into a ladder climb after a ledge grab gets the larger cap from then on.
inline Out Step(Window& w, bool climbing, unsigned long long nowMs, int capPerClimb = kMaxSamplesPerClimb)
{
    Out o;
    if (climbing) {
        w.lastClimbMs = nowMs;
        if (!w.in) {
            w.in = true;
            w.startMs = nowMs;
            w.samples = 0;
            w.seqN = 0;
            w.seqOverflow = false;
            w.ladSeqN = 0;
            w.ladSeqOverflow = false;
            w.capSaid = false;
            ++w.climbs;
            if (w.lines < kMaxLines) {
                o.event = kStart;
                ++w.lines;
            }
        }
    } else if (w.in) {
        if (nowMs - w.lastClimbMs > kTailMs) {
            w.in = false;
            if (w.lines < kMaxLines) {
                o.event = kEnd;
                ++w.lines;
            }
            return o;
        }
        o.tail = true;
    } else {
        return o;
    }
    if (w.samples < capPerClimb) {
        if (w.lines < kMaxLines) {
            o.sample = true;
            ++w.samples;
            ++w.lines;
        }
    } else if (!w.capSaid && w.lines < kMaxLines) {
        w.capSaid = true;
        o.capReached = true;
        ++w.lines;
    }
    return o;
}

// Records state s into the climb's sequence, folding consecutive repeats. Only meaningful inside an open window.
inline void NoteState(Window& w, int s)
{
    if (w.seqN > 0 && w.seq[w.seqN - 1] == s) { return; }
    if (w.seqN >= kSeqMax) {
        w.seqOverflow = true;
        return;
    }
    w.seq[w.seqN++] = s;
}

// The same for the LadderController's state (+0x134).
inline void NoteLadderState(Window& w, int s)
{
    if (w.ladSeqN > 0 && w.ladSeq[w.ladSeqN - 1] == s) { return; }
    if (w.ladSeqN >= kSeqMax) {
        w.ladSeqOverflow = true;
        return;
    }
    w.ladSeq[w.ladSeqN++] = s;
}

// Velocity (m/s) from two world positions dtS seconds apart. False (out untouched) when dt is not a plausible single update
// (<= 0 or > 0.25 s, e.g. a pause or a load) or anything is non-finite.
inline bool MeasuredVel(const float prev[3], const float cur[3], double dtS, float out[3])
{
    if (!(dtS > 0.0) || dtS > 0.25) { return false; }
    float v[3];
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(prev[i]) || !std::isfinite(cur[i])) { return false; }
        v[i] = static_cast<float>((cur[i] - prev[i]) / dtS);
    }
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    return true;
}

}  // namespace climbprobe
}  // namespace dlvr
