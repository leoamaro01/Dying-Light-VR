#include "aimhook.h"

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/buildrva.h"   // the aim family's EGL rvas on the Epic build (tier 2)
#include "../common/iathook.h"
#include "../common/log.h"
#include "boneblend.h"    // PatchGameBytes - fire_move_penalty_off's 2-byte patch, threads suspended for the write
#include "camhook.h"      // CamHookGetOpenXrToGameYaw - D5's head-free aim mapping
#include "controllers.h"
#include "headpose.h"
#include "rtticam.h"
#include "twohand.h"      // TwoHandNoteShot - the two-handed grip's per-shot pulse

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

bool AimHookOverride = false;
// Absolute, live, controller-driven aim - the proper fix for the grapple
// pitch accumulator. See ComputeAbsoluteAim for the full reasoning. On by
// default: the relative path is known to inherit that corruption.
bool AimAbsolute = true;
// Only flip this if aiming RIGHT sends the shot LEFT. It exists because a
// single vector pair cannot detect a mirrored horizontal plane between
// OpenXR and game space - the one honest unknown in the mapping.
bool AimMirrorYaw = false;
// Light smoothing on the head orientation the absolute mapping is derived
// from. Two reasons, and the second is structural rather than cosmetic:
//
//  1. The derived mapping is jitter for jitter as noisy as the raw head pose,
//     and that noise lands directly on where the shot goes.
//  2. The mapping is only correct if `head` and `original` describe the SAME
//     instant, and they do not - `original` was computed by the game from the
//     head pose it was handed earlier, while GetHeadPose returns the freshly
//     located current one. Our sample LEADS the game's. A small lag on our
//     side moves it back toward the game's, which is why smoothing damps the
//     head-turn offset rather than making it worse.
//
// This never touches the controller, so the shot direction itself stays live
// off the hand - no aiming lag is introduced by this.
bool  AimHeadSmoothing = true;
float AimHeadSmoothingMs = 85.0f;   // DEFAULT 85 2026-10-01 (Tier D: shipped tuning = Jay's live tuning)
// [DEP: fire_move_penalty_off] required-for=firearms fire with the weapon's AIMED accuracy regardless of ADS (Jay
//      2026-09-28, INV-274 (6)) - the per-hand movement/jump/sprint spread term PlayerDI::UpdateAccuracy (+BC3750)
//      springs toward is forced to 0, exactly as the game does while aiming (FireMovePenaltyOff). tree=INERT.
//      Was DEFAULT OFF. Two gamedll bytes at +BC3A02 (`jz +1C` 74 1C -> 90 90, the skip after `call +DA6B40` IsAiming):
//      UpdateAccuracy always takes its own aiming branch. Context bytes verified, written with threads suspended
//      (boneblend PatchGameBytes). Installed once, from AimHookInstall - so it NEEDS vr_aim_redirect=1 (or
//      aim_hook_auto_observe=1), the only auto-install path (ovr2xr.cpp dlvr_FilterScePad, polled flat and VR). The cone
//      half of the feature is DATA (tools/pak_firespread.py -> Data3.pak), not this setting. FLAT.
bool FireMovePenaltyOff = true;   // DEFAULT ON 2026-10-01 (Jay: Tier B ships on, docs/SHIP_DEFAULTS.md)
// [DEP: diag_fire_spread] required-for=[FIRESPREAD] per-shot readback of the spread inputs at the aimhook DR2/DR3 sites:
//      the WeaponFireController's accumulated cone WFC+148/+152 (degrees), aim flag +156, aim factor +164, zoom +324,
//      and the owner's per-hand movement term P+9932/+9936 (current) / +9940/+9944 (target) (DiagFireSpread).
//      tree=PROBE-ONLY. Log-only; reads the WFC from the breakpoint CONTEXT (r15 at +DA0B89, rdi at +DA263F - fa7), no
//      game call, no scan. The first sample logs regardless. NEEDS the aim hook armed (vr_aim_redirect=1). DEFAULT OFF.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in aimhook.h (docs/BETA_CENSUS.md)
bool DiagFireSpread = false;
#endif
// [DEP: fire_barrel_origin] required-for=the gun's hit ray starts at the BARREL, not the head (Jay 2026-09-28, INV-277)
//      (FireBarrelOrigin). tree=INERT. Was DEFAULT OFF. Detour on Bullet::Init +7128C0 (14 verified prologue bytes, the same
//      set boneblend's diag_antiwall_probe census uses - only one can install; both byte-verify, the second fails loudly).
//      Acts ONLY for the firearm dispatcher's call (return address +DA16DF): before Init runs, bullet+60..+68 (the start,
//      = owner vfunc +1840) AND bullet+220..+228 (the tracer start) := the world position of the held gun's own
//      "barrel" element (boneblend HeldItemElementWorld: entity+0x1020 item, GetElementID + the world accessor), when
//      finite and within 2 m of the start. NOT the game's +220: +BCEC80 falls back to body + 1.2 m forward in VR
//      (INV-277 correction, run barrelorigin_run1). No element = shot left alone, reason counted. Every other caller
//      (bow, AI, ...) untouched. Installed once from AimHookInstall -> NEEDS vr_aim_redirect=1; the element read NEEDS
//      the captured player entity (diag_bone_dump). HEADSET (the gun follows the rigged hand only in VR).
bool FireBarrelOrigin = true;   // DEFAULT ON 2026-10-01 (Jay: Tier A ships on)
// [DEP: fire_aim_from_barrel] required-for=the shot DIRECTION follows the gun as drawn (Jay 2026-09-28, INV-277 addendum:
//      the controller-derived base ran a constant ~8.5-9 deg off the barrel's bore, AIM vs GRIP forward 60 deg - the rig
//      poses the gun from GRIP + baked grip, the shot came from AIM) (FireAimFromBarrel). tree=INERT. Was DEFAULT OFF.
//      At DR2/DR3 the base direction := the bore (-X of the held gun's "barrel" element) that boneblend's HookTail
//      publishes each player update (PublishBarrelBore, seqlock; the VEH never calls the engine). Stale (> 150 ms),
//      missing or contended -> falls back to ComputeAbsoluteAim, counted. The cone (+DA1D30) still applies after.
//      NEEDS vr_aim_redirect=1 (the hook), the rig's player tail running (rig_write_at_player_tail / the rig armed) and the
//      captured entity (diag_bone_dump). HEADSET. ⚠ INV-48: element worlds can lag 60-100 ms in fast motion.
bool FireAimFromBarrel = true;   // DEFAULT ON 2026-10-01 (Jay: Tier A ships on)
// [DEP: fire_barrel_helper] required-for=a CROSSBOW's bolt goes down the crossbow (Jay 2026-10-08: "Crossbows aren't
//      shooting straight at all") (FireBarrelHelper). tree=LOADBEARING for crossbows. DEFAULT ON 2026-10-08 (Tier A, Jay
//      after run 20:55: "That's worked perfectly"; crossbow bore vs controller 1.62-1.69 deg on 6/6 shots). INV-494: the
//      crossbow's "barrel" -X runs ~90 deg sideways (4/4 logged shots 91.2-91.8 deg); the game's own barrel point for a
//      crossbow is BarrelHelperName "bolt_pose", whose +Z is the line of fire. ON: boneblend's PublishBarrelBore and the
//      firearm branch of BulletInitHook take HeldItemBarrelHelper (bolt_pose / arrow_pose +Z, else barrel -X, chosen once
//      per item) instead of "barrel". Guns have no *_pose element (census of 5,757 meshes) -> unchanged. NEEDS
//      fire_aim_from_barrel=1 for the direction, fire_barrel_origin=1 for the start. HEADSET.
bool FireBarrelHelper = true;   // DEFAULT ON 2026-10-08 (Tier A: Jay "That's worked perfectly")
// [DEP: bow_aim_from_bow] required-for=a BOW's arrow leaves the nock along the bow instead of from the head along the
//      head's look (Jay 2026-10-08: "Bows aren't shooting straight from where the arrow is fletched") (BowAimFromBow).
//      tree=LOADBEARING for bows. DEFAULT ON 2026-10-08 (Tier A, Jay after run 20:55: "That's worked perfectly"; 13/13
//      arrows moved, 0 refused). INV-495: SpawnArrow +D9CF40 starts the hit ray at owner +1840
//      (head) along +D9D750 (head look + lock-on assist); only the tracer starts at the bow. ON: BulletInitHook, for the
//      bow's call (return +D9D5D3, byte-verified at install), sets bullet+60 and +220 := the LEFT item's arrow_pose world
//      position and bullet+72 := |velocity| x arrow_pose +Z (draw strength kept; the lock-on assist is bypassed). One element
//      read per arrow, event-driven. Installs the Bullet::Init detour itself (InstallBarrelOriginOnce) - so it NEEDS the aim
//      hook installed (vr_aim_redirect=1) and the captured entity (diag_bone_dump). HEADSET.
bool BowAimFromBow = true;   // DEFAULT ON 2026-10-08 (Tier A: Jay "That's worked perfectly")
// [DEP: bow_fire_latched] required-for=the arrow leaves along the line the bow had while DRAWN (what bow_reticle_3d showed),
//      not the pose the release moves it to (Jay 2026-10-08: "It's going off to the left of the reticle at range"; candidate,
//      NOT measured: SpawnArrow runs in state 4 only once +189 is set - an animation event during the release, bow2
//      :1433-1456) (BowFireLatched). tree=LOADBEARING for bows. DEFAULT ON 2026-10-08 (Tier A; MEASURED INV-497: the release
//      pose is ~2.0 deg left / 1.4 deg up of the drawn line on 12/12 arrows; Jay: "That seems to have worked"). LIVE (read
//      per arrow, so the overlay checkbox acts at once). ON: in BowArrowFromBow the direction := boneblend's GetLastDrawnBowRay (the last published
//      arrow_pose +Z while the bow controller was in state 1 / 3) when it is <= 400 ms old; the start stays the spawn-time
//      nock. [BOWRELEASE] logs every arrow either way. NEEDS bow_aim_from_bow=1 (the branch) and the bow ray published
//      (boneblend PublishBowRay, diag_bone_dump=1; the state via grappleaim ReadBowCtrlState - needs grappleaim installed,
//      i.e. fire_reticle_3d / grapple keys). HEADSET.
bool BowFireLatched = true;   // DEFAULT ON 2026-10-08 (Tier A: Jay "That seems to have worked"; the release probe read 12/12
                              // arrows released 1.84-2.18 deg LEFT / 1.36-1.52 deg UP of the drawn line - INV-497)
// [DEP: diag_aim_wfcr_scan] required-for=the "WeaponFireControllerRight right now:" field of the per-shot aimhook LOG line
//      ONLY (DiagAimWfcrScan). tree=PROBE-ONLY. DEFAULT OFF (Jay 2026-10-03, performance: "if it's off, it's off").
//      Starts WfcrScanThread, which walks every committed private RW heap region every 3 s (INV-155: ~96% of one core,
//      the whole session). Its result g_wfcrObject is read in ONE place, ReadWfcrSnapshot at the shot log - the aim
//      itself never reads it. Off: the shot line says the scan is off instead of "not found". NEEDS vr_aim_redirect=1.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in aimhook.h (docs/BETA_CENSUS.md)
bool DiagAimWfcrScan = false;
#endif

namespace {

CRITICAL_SECTION g_cs;
bool g_csReady = false;
PVOID g_veh = nullptr;
uintptr_t g_addr = 0;    // DR2 - generic ComputeShootDir caller (revolver, AR, ...)
uintptr_t g_addr2 = 0;   // DR3 - ShotgunShootDir's own internal ComputeShootDir call
bool g_active = false;
char g_status[256] = "off";

// Dr7 local+global enable bits and RW/LEN field, per slot - slot 2 (existing
// hook) and slot 3 (added 2026-09-02 for the shotgun's separate call site,
// confirmed via dumpbin /disasm to need a different base register too - see
// ProcessShootDirHit's own comment).
constexpr DWORD64 kDr7Mask2 = 0xF000030;    // L2,G2 + RW2/LEN2
constexpr DWORD64 kDr7Mask3 = 0xF00000C0;   // L3,G3 + RW3/LEN3

void EnsureCs()
{
    if (!g_csReady) {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

// A short, capped diagnostic test (WatchExecStart, caps at 400 hits) made
// this address look bounded - a per-shot burst, silent otherwise. Wrong, or
// at least dangerously incomplete: armed persistently with no cap of its own,
// a single test produced over 500,000 identical hits across ~2 minutes and
// hung the game. The capped measurement never revealed a runaway because it
// stopped counting long before one could show up - "hit the cap" is not the
// same claim as "that is roughly the whole burst," and this was mistakenly
// treated as the latter. A persistent hook must never trust that distinction
// again: it needs its own hard cap, exactly like the diagnostic tools have,
// regardless of how safe a short test made any given address look.
volatile LONG g_hitTotal = 0;
constexpr LONG kMaxHits = 2000;
volatile LONG g_expired = 0;

// U-26 (the 4th instance, 2026-10-28, a tester's EGL dump): a DR2 trap delivered with Dr6 = 0 while DR2 was still armed and
// enabled on that thread - the Dr6 gate below passed it on and nobody claimed it. These are the last addresses this hook
// armed, NEVER zeroed by AimHookUninstall, so a trap landing after a disarm (or the cap's auto-disarm) is still recognised.
// Written at install before arming; read by Handler on any thread. The claim count is logged outside the handler.
volatile uintptr_t g_orphanAddr = 0, g_orphanAddr2 = 0;
volatile LONG g_orphanClaims = 0;
volatile LONG64 g_orphanFirstAddr = 0;
volatile LONG g_orphanFirstTid = 0;
volatile LONG64 g_orphanFirstDr7 = 0;
LONG g_orphanSaid = -1;   // -1 = nothing said yet: the first SayAimOrphans prints the count, 0 included

// Throttles the log among the hits that DO happen, same reasoning as before -
// separate from, and much smaller than, kMaxHits above.
volatile LONG g_logCounter = 0;
constexpr LONG kLogEvery = 50;
volatile LONG g_boreUsed = 0, g_boreFallback = 0;   // fire_aim_from_barrel, per DR hit (ProcessShootDirHit)

// Diagnostic only: log every hit, unthrottled, for the first N hits of each
// arm (on top of the kLogEvery throttle below, which still applies after
// this window). Added to check for a suspected feedback loop - override on,
// "original" kept reading back identical to "redirected" on every shot even
// with the head held still and the controller swept through very different
// directions, which shouldn't happen if ComputeShootDir is independently
// sampling a head/mouse/gamepad-driven look direction each call. kLogEvery's
// first sample only ever lands at hit 50 - if the override write from hit 1
// is somehow feeding back into whatever ComputeShootDir reads on hit 2 (and
// converging within a few iterations), that transient mismatch would be
// invisible at 50-hit resolution. This makes hits 1..10 of every burst
// visible regardless.
constexpr LONG kLogFirstN = 10;

// Freezes the direction actually applied for one whole shot's burst of
// hits, captured fresh at the first hit of the burst rather than re-read on
// every hit. Confirmed necessary in practice, not just theoretical: a burst
// can run long enough in real time that a naive per-hit fresh read drifts
// as the hand keeps moving, and since every hit rewrites the same memory,
// the LAST write wins - a shot that started out pointed correctly could end
// up firing wherever a late read happened to point by the time the burst
// tailed off. Comparing GetAimTriggerGeneration() against the generation
// this burst already adopted (rather than a timer) tells "new trigger pull"
// apart from "same burst, keep going" regardless of how long the burst
// runs.
long g_shotGeneration = 0;
float g_shotDir[3] = { 0.0f, 0.0f, -1.0f };
bool g_shotDirValid = false;
// Real-time timestamp (GetTickCount64) of the last direction resample -
// added 2026-09-02 alongside g_shotGeneration to catch a case the
// trigger-crossing generation alone cannot: an automatic weapon held down
// fires many real, separate rounds without the trigger ever releasing, so
// GetAimTriggerGeneration() never changes across the whole hold - confirmed
// in practice to freeze every round of an AR's burst to wherever the
// controller pointed at the moment the trigger was first pulled, ignoring
// all controller movement afterward. A resample is due if either the
// generation changed (a genuinely new trigger pull) OR enough real time has
// passed since the last resample - see kResampleIntervalMs below for why
// that threshold is safe for both cases at once.
ULONGLONG g_shotLastUpdateMs = 0;
// Long enough to not re-sample mid-burst for a single shot's own tightly
// clustered internal calls (a shotgun's pellet loop, or similar - these
// land within low single-digit milliseconds of each other in practice, per
// this hook's own safety-cap testing history), short enough to catch each
// new round of a fully-automatic weapon (even a high rate of fire is tens
// of milliseconds between rounds). Not independently tuned against a real
// automatic weapon's actual rate of fire yet - revisit if a weapon's rounds
// turn out closer together than this.
constexpr ULONGLONG kResampleIntervalMs = 40;
// Frozen alongside g_shotDir at the same newBurst moment, purely for
// logging - added 2026-09-02 so a specific shot's redirected result can be
// hand-verified against the exact head/controller poses that produced it,
// rather than trusting the rotation math by inspection alone.
XrQuaternionf g_shotHeadQuat{ 0.0f, 0.0f, 0.0f, 1.0f };
XrQuaternionf g_shotControllerQuat{ 0.0f, 0.0f, 0.0f, 1.0f };

// Conjugate of a unit quaternion is its inverse - negate the vector part,
// keep w.
XrQuaternionf QuatConjugate(const XrQuaternionf& q)
{
    return XrQuaternionf{ -q.x, -q.y, -q.z, q.w };
}

// Hamilton product, a*b.
XrQuaternionf QuatMultiply(const XrQuaternionf& a, const XrQuaternionf& b)
{
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}


// ---------------------------------------------------------------------------
// Smoothed head orientation (see AimHeadSmoothing's own comment above for why
// this exists and why it costs no aiming latency).
//
// Updated from dlvr_FilterScePad, read from the VEH handler on whatever thread
// a shot happens to land on - so the four floats need to be published as a
// unit. A tiny spinlock rather than a CRITICAL_SECTION: the critical section
// is four float writes, never I/O and never an allocation, matching the
// lock-free style already used for camhook's own shared ease state.
namespace {

volatile LONG g_headSmoothLock = 0;
XrQuaternionf g_headSmoothed = {0.0f, 0.0f, 0.0f, 1.0f};
bool          g_headSmoothValid = false;
LONGLONG      g_headSmoothLastQpc = 0;

void HeadSmoothLock()
{
    while (InterlockedCompareExchange(&g_headSmoothLock, 1, 0) != 0) {
        YieldProcessor();
    }
}

void HeadSmoothUnlock()
{
    InterlockedExchange(&g_headSmoothLock, 0);
}

}  // namespace

}  // namespace

// Both of the following are declared in aimhook.h and called from other
// translation units (dlvr_FilterScePad drives the update), so they must sit
// OUTSIDE this file-local anonymous namespace or they get internal linkage.
// The state they touch is still file-local - an unnamed namespace injects its
// names into the enclosing scope, so it stays reachable from here.

void AimHeadSmoothUpdate()
{
    if (!AimHeadSmoothing) {
        // Drop the filter state so re-enabling mid-session starts clean at
        // wherever the head actually is, instead of easing in from a pose
        // that may be minutes stale.
        HeadSmoothLock();
        g_headSmoothValid = false;
        HeadSmoothUnlock();
        return;
    }

    XrPosef headPose;
    if (!GetHeadPose(headPose)) {
        return;
    }

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);

    HeadSmoothLock();
    XrQuaternionf q = headPose.orientation;
    if (!g_headSmoothValid || AimHeadSmoothingMs <= 0.0f) {
        // First sample, or filtering switched off by the slider itself -
        // track the raw pose exactly.
        g_headSmoothed = q;
        g_headSmoothValid = true;
    } else {
        const double dt =
            double(now.QuadPart - g_headSmoothLastQpc) / double(freq.QuadPart);
        // A stall (level load, alt-tab) must not be treated as a huge step:
        // clamp so the filter re-converges rather than snapping oddly.
        const double dtc = (dt < 0.0) ? 0.0 : (dt > 0.25 ? 0.25 : dt);
        const double tau = double(AimHeadSmoothingMs) / 1000.0;
        const float alpha = float(1.0 - exp(-dtc / tau));

        // Quaternion double cover: q and -q are the same rotation, but a
        // straight componentwise blend between them takes the long way round.
        // Flip onto the near side first - the same correction camhook's own
        // MakeQuatContinuous applies for the same reason.
        const float dot = g_headSmoothed.x * q.x + g_headSmoothed.y * q.y +
                          g_headSmoothed.z * q.z + g_headSmoothed.w * q.w;
        if (dot < 0.0f) {
            q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w;
        }

        // Normalised lerp. Slerp is unnecessary here: consecutive samples at
        // ~250Hz are a fraction of a degree apart, where nlerp and slerp are
        // indistinguishable, and nlerp cannot divide by a near-zero sine.
        XrQuaternionf s;
        s.x = g_headSmoothed.x + (q.x - g_headSmoothed.x) * alpha;
        s.y = g_headSmoothed.y + (q.y - g_headSmoothed.y) * alpha;
        s.z = g_headSmoothed.z + (q.z - g_headSmoothed.z) * alpha;
        s.w = g_headSmoothed.w + (q.w - g_headSmoothed.w) * alpha;
        const float len = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z + s.w * s.w);
        if (len > 1e-6f) {
            const float inv = 1.0f / len;
            s.x *= inv; s.y *= inv; s.z *= inv; s.w *= inv;
            g_headSmoothed = s;
        }
    }
    g_headSmoothLastQpc = now.QuadPart;
    HeadSmoothUnlock();
}

// The head orientation the absolute mapping should use: the smoothed one when
// it is enabled and has a sample, otherwise the raw pose. Returns false only
// if there is no head pose at all.
bool GetAimHeadOrientation(XrQuaternionf& out)
{
    if (AimHeadSmoothing) {
        HeadSmoothLock();
        const bool have = g_headSmoothValid;
        if (have) {
            out = g_headSmoothed;
        }
        HeadSmoothUnlock();
        if (have) {
            return true;
        }
    }

    XrPosef headPose;
    if (!GetHeadPose(headPose)) {
        return false;
    }
    out = headPose.orientation;
    return true;
}

namespace {

// General quaternion-vector rotate, v' = q*v*q^-1 expanded to a 3x3 matrix
// multiply - the same math this project's own ForwardFromPose (overlay.cpp,
// controllers.cpp) specialises for v=(0,0,-1), here kept general since it is
// applied to an arbitrary vector (the game's own `original` shoot direction -
// see where this is called from for why).
void RotateVectorByQuat(const XrQuaternionf& q, float vx, float vy, float vz,
                         float& rx, float& ry, float& rz)
{
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
    const float xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
    const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    rx = vx * (1.0f - (yy + zz)) + vy * (xy - wz)        + vz * (xz + wy);
    ry = vx * (xy + wz)          + vy * (1.0f - (xx + zz)) + vz * (yz - wx);
    rz = vx * (xz - wy)          + vy * (yz + wx)          + vz * (1.0f - (xx + yy));
}

// Ground-truth check, added 2026-09-02: does redirecting ComputeShootDir's
// output actually change what the real shot uses, or does the real raycast
// read a genuinely separate value? WeaponFireControllerRight's own fields
// (+0xCC/+0xD0/+0xD4 direction, +0xC8 length) were independently confirmed,
// via live testing much earlier in this project, to be the real per-shot
// data feeding the actual IGSObject::Raytrace call - ComputeShootDir was
// chosen as the redirect target afterward for being safer/simpler to hook,
// never because the two were confirmed to be the same underlying value.
// This reads WeaponFireControllerRight's fields right alongside our own
// redirect and logs both, so that question has a real answer instead of an
// assumption.
//
// The heap scan that FINDS the object must never run inside the exception
// handler - a multi-megabyte VirtualQuery/read walk mid-fault is exactly
// the kind of thing this project's own history warns against. It runs on
// its own dedicated background thread instead (see WfcrScanThread below) -
// NOT polled every frame by the overlay, which is where this first went
// wrong (see that function's own comment). The handler only ever does a
// cheap, SEH-guarded read of the cached address - the same discipline as
// every other memory read taken from inside this hook.
uintptr_t g_wfcrObject = 0;
// RttiVtableOf() itself is not cached (only a private wrapper inside
// rtticam.cpp is, for its own internal use) - calling it directly, every
// scan, re-walks RTTI across both engine_x64_rwdi.dll and
// gamedll_x64_rwdi.dll from scratch every time. Resolved once here instead.
uintptr_t g_wfcrVtable = 0;

void RescanWeaponFireControllerRight()
{
    if (!g_wfcrVtable) {
        g_wfcrVtable = RttiVtableOf("WeaponFireControllerRight");
        if (!g_wfcrVtable) {
            return;
        }
    }
    const uintptr_t vt = g_wfcrVtable;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uintptr_t found = 0;
    for (uintptr_t a = lo; a < hi && !found;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) {
            break;
        }
        const uintptr_t bbase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool ok = mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                         prot == PAGE_EXECUTE_READWRITE) &&
                        mbi.Type == MEM_PRIVATE;   // objects live on the heap
        if (ok && size >= sizeof(uintptr_t)) {
            __try {
                const auto* q = reinterpret_cast<const uintptr_t*>(bbase);
                const size_t count = size / sizeof(uintptr_t);
                for (size_t i = 0; i < count; ++i) {
                    if (q[i] != vt) {
                        continue;
                    }
                    // A raw vtable-pointer match is very often a stale/freed
                    // heap slot the allocator has not reused yet, not a live
                    // object - confirmed repeatedly, earlier in this project,
                    // for other RTTI-found classes (turned out to be
                    // ntdll/driver churn on a reused slot, never a real
                    // gamedll instance). Taking the first match unqualified
                    // reproduced exactly that failure here: a constant,
                    // clearly-bogus value (len in the tens of billions)
                    // logged identically across 20 minutes and 5 weapons.
                    // Filter by basic plausibility of the fields this
                    // diagnostic actually reads before accepting a
                    // candidate, and keep scanning past one that fails it.
                    const uintptr_t candidate = bbase + i * sizeof(uintptr_t);
                    const auto* dir = reinterpret_cast<const float*>(candidate + 0xCC);
                    const float len = *reinterpret_cast<const float*>(candidate + 0xC8);
                    // A trivially all-zero object passes a finite/magnitude
                    // check just as easily as a real one - and turned out to
                    // be exactly what "first plausible match" was actually
                    // finding (2026-09-02: six real shots, constant exact
                    // zero every time, while this field is documented
                    // elsewhere in this project to be populated continuously
                    // while a ranged weapon is held/aimed). Require a
                    // meaningfully non-zero length too, so an unused/zeroed
                    // dummy instance no longer satisfies "plausible" and the
                    // scan keeps walking for one that's actually been
                    // written to.
                    const bool plausible =
                        std::isfinite(dir[0]) && std::isfinite(dir[1]) &&
                        std::isfinite(dir[2]) && std::isfinite(len) &&
                        fabsf(dir[0]) < 100.0f && fabsf(dir[1]) < 100.0f &&
                        fabsf(dir[2]) < 100.0f && fabsf(len) < 100.0f &&
                        fabsf(len) > 0.01f;
                    if (plausible) {
                        found = candidate;
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Freed under us mid-scan - nothing found here stands.
            }
        }
        a = bbase + size;
    }
    if (found) {
        g_wfcrObject = found;
    }
}

// Runs on its own dedicated thread, well away from rendering. Confirmed the
// hard way 2026-09-02: a full virtual-address-space scan is genuinely
// expensive (worse once RttiVtableOf() wasn't cached either - see its own
// comment above), and running it synchronously on the render thread, polled
// every frame via AimHookActive(), froze the game outright - each scan took
// longer than the 2s gap between them, so the render thread was permanently
// stuck doing back-to-back scans and never got to present a frame again.
// Every other RTTI scan in this codebase is deliberately manual (a button
// or dropdown click) for exactly this reason; this one has to be periodic,
// to survive a weapon switch recreating the instance, so it gets its own
// thread instead of breaking that convention a second way.
volatile LONG g_wfcrScanThreadRunning = 0;

DWORD WINAPI WfcrScanThread(LPVOID)
{
    while (g_active && !g_expired) {
        RescanWeaponFireControllerRight();
        Sleep(3000);
    }
    InterlockedExchange(&g_wfcrScanThreadRunning, 0);
    return 0;
}

struct WfcrSnapshot {
    float dir[3];
    float len;
    bool ok;
};

// Called from inside the exception handler - cheap, SEH-guarded, no
// scanning. g_wfcrObject may be 0 (never found yet) or stale (freed since
// the last periodic rescan); either way this fails safely rather than
// faulting the game.
WfcrSnapshot ReadWfcrSnapshot()
{
    WfcrSnapshot s{};
    const uintptr_t obj = g_wfcrObject;
    if (!obj) {
        return s;
    }
    __try {
        s.dir[0] = *reinterpret_cast<float*>(obj + 0xCC);
        s.dir[1] = *reinterpret_cast<float*>(obj + 0xD0);
        s.dir[2] = *reinterpret_cast<float*>(obj + 0xD4);
        s.len = *reinterpret_cast<float*>(obj + 0xC8);
        s.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s.ok = false;
    }
    return s;
}

// Shared by both DR2 (generic ComputeShootDir caller, gamedll+0xDA0B89 -
// revolver, AR, ...) and DR3 (ShotgunShootDir's own internal ComputeShootDir
// call, gamedll+0xDA263F). Both call sites write ComputeShootDir's normalized
// output into a 3-float buffer immediately after the call returns; only
// WHERE that buffer lives differs (RBP-relative vs RSP-relative - confirmed
// via dumpbin /disasm 2026-09-02 that ShotgunShootDir's own prologue never
// sets up an RBP frame at all, so it needed its own base register, not just
// a different offset). `vec` is already resolved to the right address by the
// caller; everything from here down - validity check, burst-freeze, the
// override itself, logging - is identical between the two targets.
// ---- ABSOLUTE, LIVE controller aim (2026-09-05) -----------------------------
//
// Replaces the relative redirect for good. WHY, in one line: the relative
// approach applies `delta = controller * inverse(head)` to the game's own
// `original`, so it faithfully inherits ANY error already in that reference -
// and the grapple hook was proven to leave a permanent, ACCUMULATING pitch
// offset there (measured: -2.4 deg clean, +3.1 after one grapple, -31.0 after
// three floor-grapples, six consecutive shots each, never recovering). No
// amount of work on the delta can fix a corrupted reference.
//
// This computes the shot direction ABSOLUTELY from the right controller, and
// LIVE on every hit rather than frozen at burst start, so the gun points
// where the controller is physically pointing at the moment of the shot.
//
// The only thing needed from the game is the OpenXR -> game-world mapping.
// Both spaces are gravity-aligned and Y-up, so that mapping is a pure YAW
// rotation - the difference between where the play space faces and where the
// character faces. It is derived live as "the rotation about Y taking the
// head's heading to original's heading", using dot/cross on the two
// horizontal vectors rather than atan2 with hardcoded axis indices, so no
// assumption is made about the game's X/Z ordering. Pitch is then taken
// ENTIRELY from the controller, which is exactly why the pitch accumulator
// becomes irrelevant.
//
// Y needs no flip here, and the measured data settles that: raw origY read
// -31 deg (asin) at the moment the gun was visibly pointing at the floor, so
// the game's buffer is negative-Y-down, same as OpenXR. The old
// `g_shotDir[1] = -g_shotDir[1]` existed to correct the COMPOSITION of an
// OpenXR quaternion with a game-space vector - a step this path never does.
//
// The one genuine unknown, isolated deliberately rather than guessed: if the
// game's horizontal plane is MIRRORED relative to OpenXR, a single vector
// pair cannot detect it and left/right would come out inverted. AimMirrorYaw
// exists solely to flip that, and is the only thing to touch if aiming right
// sends the shot left.
//
// Outside the file-local namespace since 2026-09-27: the grapple (R9,
// grappleaim.cpp) uses the SAME mapping, declared in aimhook.h. Body unchanged.
}  // namespace

bool ComputeAbsoluteAim(float origX, float origY, float origZ, float out[3], Hand hand)
{
    // D5 (SPEC-HEAD-YAW-DECOUPLE-D, R10/R6): with the decouple active the head takes NO part in aim.
    // The XR->game yaw comes from the rig's own published mapping - camhook's `rel`, the angle the
    // arms use - instead of pairing the head's heading with `original`'s. It is the SAME angle as
    // the pairing below computes: A = theta(original) - theta(head) with theta = atan2(x, z), and
    // rel = atan2(camFwd) - atan2(headFwd) with the same rotated (0,0,-1); `original` IS the camera
    // look (INV-264). Under D camhook pairs `rel` with the head that went INTO the camera (D3), so
    // the head cancels inside it. `original` still gates (the magnitude check), it no longer steers.
    if (HeadYawDecoupleActive()) {
        XrPosef controllerPose;
        float rel = 0.0f;
        const float ohLen = sqrtf(origX * origX + origZ * origZ);
        if (!GetControllerPoseSteady(hand, controllerPose) || ohLen < 0.10f ||
            !CamHookGetOpenXrToGameYaw(rel)) {
            return false;
        }
        float cx, cy, cz;
        RotateVectorByQuat(controllerPose.orientation, 0.0f, 0.0f, -1.0f, cx, cy, cz);
        const float cosA = cosf(rel);
        float sinA = sinf(rel);
        if (AimMirrorYaw) {
            sinA = -sinA;
        }
        out[0] = cx * cosA + cz * sinA;
        out[1] = cy;
        out[2] = -cx * sinA + cz * cosA;
        const float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        if (len < 1e-4f) {
            return false;
        }
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
        return true;
    }

    XrPosef controllerPose;
    XrQuaternionf headOrientation;
    // Steadied (comfort_hand_steady, C1): this is a shot direction, the thing hand steadiness is for.
    if (!GetAimHeadOrientation(headOrientation) ||
        !GetControllerPoseSteady(hand, controllerPose)) {
        return false;
    }

    // OpenXR forward is -Z. Head and controller headings in play space.
    float hx, hy, hz, cx, cy, cz;
    RotateVectorByQuat(headOrientation, 0.0f, 0.0f, -1.0f, hx, hy, hz);
    RotateVectorByQuat(controllerPose.orientation, 0.0f, 0.0f, -1.0f, cx, cy, cz);

    // Horizontal (XZ) components. If either heading is near-vertical its
    // horizontal part is meaningless, so bail rather than derive a garbage
    // mapping from it - looking straight up or down must not corrupt aim.
    const float hhLen = sqrtf(hx * hx + hz * hz);
    const float ohLen = sqrtf(origX * origX + origZ * origZ);
    if (hhLen < 0.10f || ohLen < 0.10f) {
        return false;
    }
    const float hhx = hx / hhLen, hhz = hz / hhLen;
    const float ohx = origX / ohLen, ohz = origZ / ohLen;

    // The yaw rotation taking the head's heading to original's heading.
    // cos from the dot product, sin from the Y component of the cross
    // product - no axis-order assumption, and applying this to (hhx,hhz)
    // reproduces (ohx,ohz) exactly by construction.
    const float cosA = hhx * ohx + hhz * ohz;
    float sinA = hhz * ohx - hhx * ohz;
    if (AimMirrorYaw) {
        sinA = -sinA;
    }

    // Apply that yaw to the controller's FULL direction: horizontal part
    // rotated into game space, vertical part passed straight through.
    out[0] = cx * cosA + cz * sinA;
    out[1] = cy;
    out[2] = -cx * sinA + cz * cosA;

    const float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len < 1e-4f) {
        return false;
    }
    out[0] /= len;
    out[1] /= len;
    out[2] /= len;
    return true;
}

namespace {

void ProcessShootDirHit(float* vec, LONG hitNum, const char* label)
{
    // In OBSERVE-ONLY mode log every hit. kLogEvery=50 is a stale tuning from
    // when the missing-EFLAGS.RF bug made a single shot produce 400+ spurious
    // hits (every 50th then gave ~8 lines per shot). With that bug fixed a
    // shot generates only a couple of real hits, so the every-50th rule needs
    // roughly 10-25 SHOTS to emit one line - which silently cost a whole
    // grapple test: five lines appeared, then nothing, while the hook was
    // working correctly the entire time and had not hit its cap. Volume here
    // is bounded by kMaxHits (2000) regardless, so this cannot flood.
    const bool shouldLog = !AimHookOverride ||
                            hitNum <= kLogFirstN ||
                            (InterlockedIncrement(&g_logCounter) % kLogEvery) == 0;
    const float origX = vec[0], origY = vec[1], origZ = vec[2];

    // A genuine ComputeShootDir result is always a near-unit vector
    // (confirmed repeatedly - see CLAUDE.md). Confirmed 2026-09-02 that this
    // exact instruction address is ALSO reached from a context where the
    // buffer is not that value at all - garbage floats (values in the tens
    // of quadrillions) and hard zeros, likely another caller of the same
    // enclosing function entirely (an NPC's own ranged-weapon logic sharing
    // the code path is the leading guess, now more likely to be seen at all
    // now that the watcher thread arms every thread, not just whichever one
    // the player's own shot happened to land on). Redirecting a hit that
    // doesn't look like real shot data would silently corrupt whatever that
    // call actually was, for no benefit - skip entirely rather than guess.
    const float origLenSq = origX * origX + origY * origY + origZ * origZ;
    const bool origLooksReal = origLenSq > 0.5f && origLenSq < 2.0f;

    if (!AimHookOverride) {
        if (shouldLog) {
            // Elevation logged explicitly for the grapple-hook investigation
            // (2026-09-05). Reported symptom: after grappling the gun "skews
            // down and shoots towards the floor", while MOVEMENT stays
            // correct. Movement uses yaw only, so a yaw-carrying corruption
            // is ruled out - whatever is wrong carries PITCH, and this is the
            // number that shows it.
            //
            // The redirect is RELATIVE (delta = controller * inverse(head),
            // applied to this original), so a persistent pitch error in this
            // reference is inherited faithfully by the redirected shot. Fire
            // level, grapple, fire level again: if elevation is pushed in the
            // second set and STAYS there, the corruption is in the aim
            // reference. The camera is already cleared as a suspect - its
            // pitch tracks the head and returns to level on its own.
            //
            // Sign caveat: this buffer is Y-flipped relative to OpenXR's Y-up
            // (established when the redirect math was fixed), so do not read
            // the absolute sign as up/down. What matters is whether it
            // CHANGES across the grapple and stays changed.
            const float len = (origLenSq > 1e-6f) ? sqrtf(origLenSq) : 1.0f;
            const float elevDeg = asinf(origY / len) * (180.0f / 3.14159265f);
            LOG("aimhook[%s]: original x=%.4f y=%.4f z=%.4f  elevation=%.1f deg",
                label, origX, origY, origZ, elevDeg);
        }
    } else if (!origLooksReal) {
        if (shouldLog) {
            LOG("aimhook[%s]: original x=%.4f y=%.4f z=%.4f (len^2=%.2f, doesn't look like "
                "real shot data - not this hook's actual target, leaving untouched)",
                label, origX, origY, origZ, origLenSq);
        }
    } else if (AimAbsolute && origLooksReal) {
        // THE path now - absolute, live, controller-driven. Deliberately
        // ahead of and bypassing the whole burst-freeze machinery below.
        //
        // The freeze existed to stop the RELATIVE direction drifting inside
        // one shot's multi-hit burst, and to force a resample per round on
        // automatic weapons. Neither problem exists here: sampling the
        // controller live gives every round the direction the controller is
        // actually pointing at that instant, which is the whole requirement -
        // "aim tied to the live movement of the right controller". Hits
        // within a single shot land within a few milliseconds of each other,
        // so they naturally agree without needing to be frozen.
        float aim[3];
        const bool haveCtl = ComputeAbsoluteAim(origX, origY, origZ, aim);
        // fire_aim_from_barrel: the gun's bore, published by boneblend's HookTail - a plain seqlock read, no engine call.
        float bore[3] = {};
        unsigned boreAge = 0;
        const char* boreWhy = "setting off";
        const bool useBore = FireAimFromBarrel && GetPublishedBarrelBore(bore, &boreAge, &boreWhy);
        if (FireAimFromBarrel) {
            const LONG k = useBore ? InterlockedIncrement(&g_boreUsed) : InterlockedIncrement(&g_boreFallback);
            float offDeg = -1.0f;
            if (useBore && haveCtl) {
                float c = bore[0] * aim[0] + bore[1] * aim[1] + bore[2] * aim[2];
                c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
                offDeg = acosf(c) * 57.29578f;
            }
            if (k == 1 || shouldLog) {
                // [PROBE: FIREAIM] gate=fire_aim_from_barrel  site=HEADSET  log-only=no
                LOG("aimhook[%s]: [FIREAIM] %s%s | bore (%.4f %.4f %.4f) age %u ms | controller-derived aim %s (%.4f %.4f %.4f) "
                    "-> bore vs controller %.2f deg | totals: bore used %ld, fell back %ld",
                    label, useBore ? "BASE = the gun's BORE" : "*** FELL BACK to the controller aim *** - ",
                    useBore ? "" : boreWhy, bore[0], bore[1], bore[2], boreAge, haveCtl ? "" : "(none)", aim[0], aim[1],
                    aim[2], offDeg, g_boreUsed, g_boreFallback);
            }
        }
        if (useBore) {
            vec[0] = bore[0];
            vec[1] = bore[1];
            vec[2] = bore[2];
        } else if (haveCtl) {
            vec[0] = aim[0];
            vec[1] = aim[1];
            vec[2] = aim[2];
            if (shouldLog) {
                const float oLen = sqrtf(origLenSq) > 1e-6f ? sqrtf(origLenSq) : 1.0f;
                const float origElev = asinf(origY / oLen) * (180.0f / 3.14159265f);
                const float aimElev = asinf(aim[1]) * (180.0f / 3.14159265f);
                LOG("aimhook[%s]: ABSOLUTE original x=%.4f y=%.4f z=%.4f (elev %.1f) "
                    "-> aim x=%.4f y=%.4f z=%.4f (elev %.1f)  [pitch is the controller's, "
                    "so original's accumulated grapple drift is discarded]",
                    label, origX, origY, origZ, origElev, aim[0], aim[1], aim[2], aimElev);
            }
        } else if (shouldLog) {
            // No pose, or a near-vertical heading that would yield a garbage
            // yaw mapping. Leave the game's own value alone rather than
            // write something derived from a degenerate basis.
            LOG("aimhook[%s]: ABSOLUTE - no usable pose/heading this hit, left untouched",
                label);
        }
    } else {
        // A resample happens either when the trigger-crossing generation
        // changes (a genuinely new trigger pull) OR enough real time has
        // passed since the last resample (see kResampleIntervalMs) - shared
        // across both DR2/DR3 targets (only one weapon fires at a time, so
        // one shared burst/direction is correct regardless of which call
        // site the current weapon routes through). The generation check
        // alone caught a fresh trigger pull but NOT a fully-automatic
        // weapon held down for many real rounds without ever releasing -
        // confirmed in practice to freeze an AR's whole burst to wherever
        // the controller pointed at the very first round, ignoring all
        // controller movement for every round after. Captures a fresh pose
        // right here, at the moment a resample is due, rather than reusing
        // whatever was true earlier - a shot's own tightly-clustered
        // internal calls (a shotgun's pellet loop) still reuse one frozen
        // value between resamples, since kResampleIntervalMs is chosen to
        // outlast those.
        const long curGen = GetAimTriggerGeneration();
        const ULONGLONG nowMs = GetTickCount64();
        const bool newBurst = curGen != g_shotGeneration ||
                               (nowMs - g_shotLastUpdateMs) >= kResampleIntervalMs;
        if (newBurst) {
            // Superseded 2026-09-02: writing an absolute world-space
            // controller-forward vector (even with the empirically-derived
            // X+Z negation) assumed the game's ComputeShootDir buffer used a
            // FIXED coordinate convention. It doesn't - real testing showed
            // the same code correctly matched the revolver in one session,
            // then came out mirrored AND rotated for the AR in another, and
            // turning the player's body ~90 degrees (with the SAME real-world
            // pointing direction held constant) made the redirected shot go
            // behind the player. That is the signature of a value that needs
            // to be expressed relative to the character's current facing,
            // not a fixed world axis - no static negation can be correct for
            // every facing at once.
            //
            // Fixed approach: compute the controller's rotation RELATIVE TO
            // THE HEAD (both in OpenXR/room space, which stays valid no
            // matter which way the in-game character is currently facing),
            // then apply that same relative rotation to the game's own
            // `original` direction - a value already expressed correctly in
            // whatever convention/frame the game itself uses, since the game
            // computed it. "Controller pointing exactly where the head is
            // looking" becomes the identity rotation, so redirected =
            // original untouched; any rotation away from head-forward is
            // carried through as a rotation on top of wherever the game
            // already considers forward. This needs no assumption about the
            // buffer's own axis convention beyond sharing OpenXR/LibOVR's
            // right-handed, Y-up basis (see this file's own top-of-file
            // comment) - rotation composition is basis-independent as long
            // as that much is shared.
            XrPosef controllerPose, headPose;
            if (GetControllerPoseSteady(kRightHand, controllerPose) && GetHeadPose(headPose)) {
                // World-space relative rotation, not head-local: original is
                // a world-space vector, so the delta applied to it has to be
                // expressed in world-space terms too. controller * head^-1
                // (this order) gives that; head^-1 * controller instead gives
                // the delta in the HEAD's OWN local frame - correct for
                // rotating something expressed in head-local coordinates, but
                // a frame mismatch when applied directly to a world vector
                // like `original`. The two only coincide when the head has no
                // pitch/roll of its own (pure yaw or dead level) - exactly
                // why an earlier facing-only test looked completely fixed
                // while a real up/down test then showed a clean pitch
                // inversion (confirmed 2026-09-02: aiming the reticle up
                // shot down and vice versa) - the natural head tilt that
                // comes with aiming up/down is exactly the case where the
                // two orders diverge.
                const XrQuaternionf delta = QuatMultiply(
                    controllerPose.orientation, QuatConjugate(headPose.orientation));
                RotateVectorByQuat(delta, origX, origY, origZ,
                                   g_shotDir[0], g_shotDir[1], g_shotDir[2]);
                // Confirmed 2026-09-02: a clean up/down inversion survived a
                // legitimate rotation-composition-order fix, which is the
                // signature of a mirrored axis (a reflection), not a
                // remaining rotation bug - no proper rotation can produce a
                // clean inversion on its own. The game's buffer is Y-flipped
                // relative to OpenXR's Y-up convention; negating Y here is
                // the correction - retested and confirmed fixed.
                g_shotDir[1] = -g_shotDir[1];
                g_shotDirValid = true;
                g_shotHeadQuat = headPose.orientation;
                g_shotControllerQuat = controllerPose.orientation;
            } else {
                g_shotDirValid = false;
            }
            g_shotGeneration = curGen;
            g_shotLastUpdateMs = nowMs;
        }
        // The first hit of every new shot logs unconditionally, on top
        // of the usual kLogFirstN/kLogEvery throttle - that throttle is
        // sized for "don't flood the log across a whole play session",
        // not "guarantee every shot gets at least one line", and a
        // single shot's burst can easily land on zero throttled hits by
        // sheer counter luck (confirmed happening in practice
        // 2026-09-02 - two deliberate test shots produced no log output
        // at all). The redirected value is frozen for the whole burst
        // anyway, so this one line already says everything a repeat hit
        // would.
        const bool logThis = shouldLog || newBurst;

        if (g_shotDirValid) {
            // g_shotDir is `original` rotated by the controller's rotation
            // relative to the head (see the newBurst block above) - already
            // normalized, since rotating a unit vector by a unit quaternion
            // preserves length exactly. No axis convention assumed here
            // beyond OpenXR/LibOVR's shared right-handed, Y-up basis.
            vec[0] = g_shotDir[0];
            vec[1] = g_shotDir[1];
            vec[2] = g_shotDir[2];
            if (logThis) {
                // head/controller quaternions logged raw, unthrottled with
                // everything else here, so a specific shot's redirected
                // result can be hand-recomputed from these exact inputs and
                // checked against the formula - added 2026-09-02 while
                // chasing the pitch-inversion bug, rather than trusting the
                // rotation math by inspection alone.
                const WfcrSnapshot wfcr = ReadWfcrSnapshot();
                if (wfcr.ok) {
                    LOG("aimhook[%s]: original x=%.4f y=%.4f z=%.4f -> redirected(shot-frozen, "
                        "gen %ld) x=%.4f y=%.4f z=%.4f | head q=(%.4f,%.4f,%.4f,%.4f) "
                        "controller q=(%.4f,%.4f,%.4f,%.4f) | WeaponFireControllerRight right "
                        "now: dir x=%.4f y=%.4f z=%.4f len=%.4f",
                        label, origX, origY, origZ, g_shotGeneration, g_shotDir[0], g_shotDir[1],
                        g_shotDir[2], g_shotHeadQuat.x, g_shotHeadQuat.y, g_shotHeadQuat.z,
                        g_shotHeadQuat.w, g_shotControllerQuat.x, g_shotControllerQuat.y,
                        g_shotControllerQuat.z, g_shotControllerQuat.w, wfcr.dir[0], wfcr.dir[1],
                        wfcr.dir[2], wfcr.len);
                } else {
                    LOG("aimhook[%s]: original x=%.4f y=%.4f z=%.4f -> redirected(shot-frozen, "
                        "gen %ld) x=%.4f y=%.4f z=%.4f | head q=(%.4f,%.4f,%.4f,%.4f) "
                        "controller q=(%.4f,%.4f,%.4f,%.4f) | WeaponFireControllerRight %s",
                        label, origX, origY, origZ, g_shotGeneration, g_shotDir[0], g_shotDir[1],
                        g_shotDir[2], g_shotHeadQuat.x, g_shotHeadQuat.y, g_shotHeadQuat.z,
                        g_shotHeadQuat.w, g_shotControllerQuat.x, g_shotControllerQuat.y,
                        g_shotControllerQuat.z, g_shotControllerQuat.w,
                        DiagAimWfcrScan ? "not found/unreadable yet"
                                        : "not scanned (diag_aim_wfcr_scan=0)");
                }
            }
        } else if (logThis) {
            // Override is on and this looks like a real shot, but no
            // controller pose was available at the start of this burst
            // (not tracked yet, hand inactive, etc.) - vec[] is left
            // untouched, same as if override were off.
            LOG("aimhook[%s]: original x=%.4f y=%.4f z=%.4f (override on, no controller pose "
                "available at burst start - not redirected)",
                label, origX, origY, origZ);
        }
    }
}

// [FIRESPREAD] readback (INV-274). `wfc` is the WeaponFireController the dispatcher is shooting with, taken from the
// breakpoint CONTEXT: r15 at +DA0B89 (+DA0160 prologue `mov r15, rcx`), rdi at +DA263F (+DA2610 `mov rdi, rcx`) - fa7.
// Owner = [wfc+8] (the WFC vtable slot 4 is `mov rax,[rcx+8]`), hand = [wfc+0x158]. The spread the shot is about to get
// is +DA1D30(WFC+148 + P[+9932+24h], WFC+152 + P[+9936+24h]) for a single projectile (INV-274 (1)), so these are the
// exact inputs. Plain reads under SEH, nothing called, nothing written.
volatile LONG g_fireSpreadSamples = 0;
volatile LONG g_fireSpreadNotShot = 0;
void FireSpreadSample(const float* vec, uintptr_t wfc, const char* site)
{
    // DR2's address is shared with an unrelated caller passing non-unit garbage (ENGINE_ARCHITECTURE "second caller
    // shares that address"); there r15 is not a WFC. Same magnitude gate as ProcessShootDirHit, counted, not silent.
    float lenSq = 0.0f;
    __try {
        lenSq = vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lenSq = 0.0f;
    }
    if (!(lenSq > 0.5f && lenSq < 2.0f)) {
        const LONG k = InterlockedIncrement(&g_fireSpreadNotShot);
        if (k == 1) {
            // [PROBE: FIRESPREAD] gate=diag_fire_spread  site=FLAT  log-only=yes
            LOG("aimhook: [FIRESPREAD] %s: not a real shot (len^2 %.3g) - no WFC read; further such hits counted only",
                site, lenSq);
        }
        return;
    }
    const LONG n = InterlockedIncrement(&g_fireSpreadSamples);
    if (!DiagFireSpread && n > 1) {
        return;
    }
    float coneH = 0, coneV = 0, aimF = 0, zoom = 0, mH = 0, mV = 0, tH = 0, tV = 0;
    int aimFlag = -1, hand = -1;
    bool ok = false;
    __try {
        coneH = *reinterpret_cast<const float*>(wfc + 148);
        coneV = *reinterpret_cast<const float*>(wfc + 152);
        aimFlag = *reinterpret_cast<const unsigned char*>(wfc + 156);
        aimF = *reinterpret_cast<const float*>(wfc + 164);
        zoom = *reinterpret_cast<const float*>(wfc + 324);
        hand = *reinterpret_cast<const int*>(wfc + 0x158);
        const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(wfc + 8);
        if (owner && hand >= 0 && hand <= 1) {
            const uintptr_t row = owner + 9932 + 24 * static_cast<uintptr_t>(hand);
            mH = *reinterpret_cast<const float*>(row + 0);
            mV = *reinterpret_cast<const float*>(row + 4);
            tH = *reinterpret_cast<const float*>(row + 8);
            tV = *reinterpret_cast<const float*>(row + 12);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    // [PROBE: FIRESPREAD] gate=diag_fire_spread  site=FLAT  log-only=yes
    LOG("aimhook: [FIRESPREAD] #%ld %s wfc=%p%s | cone H %.4f V %.4f deg (WFC+148/+152) | move term H %.4f V %.4f -> "
        "target %.3f %.3f (P+9932.., hand %d) | aim flag %d factor %.3f zoom %.3f | move-penalty patch %s%s",
        n, site, reinterpret_cast<void*>(wfc), n == 1 ? " (FIRST sample, logs regardless of diag_fire_spread)" : "",
        coneH, coneV, mH, mV, tH, tV, hand, aimFlag, aimF, zoom,
        FireMovePenaltyOff ? "requested (see the install line)" : "off",
        ok ? "" : " | *** owner/hand unreadable - move-term fields are NOT data ***");
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = ep->ContextRecord;
    // DR2 - the original hook (generic ComputeShootDir caller). DR3 - added
    // 2026-09-02 for ShotgunShootDir's own internal call site. Both are this
    // hook's own dedicated registers, separate from the diagnostic tools'
    // DR0/DR1.
    bool dr2Hit = (ctx->Dr6 & 0x4) != 0;
    bool dr3Hit = (ctx->Dr6 & 0x8) != 0;
    if (!dr2Hit && !dr3Hit) {
        // [LOADBEARING: AIMHOOK-ORPHAN] U-26: an exec trap with NO breakpoint bit (B0..B3) and no BS in Dr6, exactly on an
        //      address this hook armed, is ours - without this nobody claims it and the game dies (0x80000004 at
        //      gamedll+0xDA0029 on EGL, 2026-10-28). Claimed as that DR's hit while the hook is live; RF + resume otherwise.
        //      No lock, no allocation, no LOG here (SayAimOrphans logs).
        const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
        const bool orphan = (ctx->Dr6 & 0x400F) == 0 && rip != 0 &&
                            reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) == rip &&
                            (rip == g_orphanAddr || rip == g_orphanAddr2);
        if (!orphan) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (InterlockedCompareExchange64(&g_orphanFirstAddr, static_cast<LONG64>(rip), 0) == 0) {
            InterlockedExchange(&g_orphanFirstTid, static_cast<LONG>(GetCurrentThreadId()));
            InterlockedExchange64(&g_orphanFirstDr7, static_cast<LONG64>(ctx->Dr7));
        }
        InterlockedIncrement(&g_orphanClaims);
        if (!g_active || g_expired) {
            ctx->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        dr2Hit = rip == g_orphanAddr;
        dr3Hit = !dr2Hit;
    }
    ctx->Dr6 = 0;

    // Instruction (execute) breakpoints are fault-class: the exception fires
    // BEFORE the flagged instruction runs, with RIP still pointing at it, not
    // past it. Resuming without setting EFLAGS.RF (bit 16) leaves the debug
    // hardware free to immediately re-match the same still-current RIP
    // against DR2/DR3 and re-raise the exception again before that
    // instruction has ever actually executed - an x86 gotcha, not a
    // game/engine quirk. This is almost certainly the real explanation for
    // every "this call fires an enormous number of times per shot"
    // observation earlier in CLAUDE.md: the same single call attempt
    // re-trapping on itself, not the game genuinely re-invoking anything.
    // Setting RF here tells the CPU to suppress exactly one re-check of the
    // instruction breakpoint on resume, so the real instruction gets to
    // execute exactly once before the breakpoint can arm again for a
    // genuinely new call.
    ctx->EFlags |= 0x10000;

    // One shared budget across both targets - disarms BOTH the instant
    // either one crosses it, rather than tracking two independent caps. The
    // safety invariant that matters is "this VEH handler never processes
    // more than kMaxHits hits total before fully disarming," not "each
    // address individually stays under some limit" - see kMaxHits's own
    // comment for why any address needs a hard cap regardless of how safe a
    // short diagnostic test made it look. Disarm on THIS thread the moment
    // the cap is hit; full cleanup (every thread, removing the VEH) has to
    // happen from a normal thread, not from inside an exception handler -
    // AimHookActive() notices g_expired on its next poll (the overlay calls
    // it every frame) and finishes the job.
    const LONG hitNum = InterlockedIncrement(&g_hitTotal);
    if (hitNum > kMaxHits) {
        ctx->Dr7 &= ~(kDr7Mask2 | kDr7Mask3);
        ctx->Dr2 = 0;
        ctx->Dr3 = 0;
        InterlockedExchange(&g_expired, 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    __try {
        if (dr2Hit) {
            // Armed at gamedll+0xDA0B89, five bytes past the `call rel32` to
            // ComputeShootDir at gamedll+0xDA0B84 - right after that call
            // returns. RBP is callee-saved, so it is still exactly the
            // caller's own frame pointer here even though ComputeShootDir
            // has just run; RCX/RDX are not guaranteed to be (volatile -
            // ComputeShootDir is free to have reused them internally). The
            // vec3 output lives on the CALLER's stack at [rbp-0x48] -
            // confirmed live, always unit-length, via the read-only
            // diagnostic capture recorded in CLAUDE.md.
            auto* vec = reinterpret_cast<float*>(static_cast<uintptr_t>(ctx->Rbp) - 0x48);
            ProcessShootDirHit(vec, hitNum, "ComputeShootDir");
            FireSpreadSample(vec, static_cast<uintptr_t>(ctx->R15), "DR2 single-projectile base (+DA0B89)");
        }
        if (dr3Hit) {
            // Armed at gamedll+0xDA263F, five bytes past ShotgunShootDir's
            // OWN internal call to ComputeShootDir at gamedll+0xDA263A - a
            // separate call site from DR2's, only reached by weapons routed
            // through ShotgunShootDir (confirmed: the shotgun never hits
            // DR2's address at all). ShotgunShootDir's own prologue is a
            // plain `sub rsp, 0x70` with no RBP frame whatsoever (confirmed
            // via dumpbin /disasm 2026-09-02), so RBP here is left over from
            // some unrelated ancestor frame - reading [rbp+offset] silently
            // returned a near-constant, Y-dominant vector that was NOT real
            // shot data (the tell: the user was aiming level, not down). The
            // real output buffer is `lea rdx,[rsp+30h]` right before the
            // call; a call/ret pair is stack-neutral, so RSP right after the
            // call returns (where this breakpoint sits) equals RSP right
            // before it - [rsp+0x30] is correct, confirmed via a live
            // forward-facing unit vector matching the shape of every other
            // confirmed-good reading.
            auto* vec = reinterpret_cast<float*>(static_cast<uintptr_t>(ctx->Rsp) + 0x30);
            ProcessShootDirHit(vec, hitNum, "ShotgunShootDir");
            FireSpreadSample(vec, static_cast<uintptr_t>(ctx->Rdi), "DR3 multi-projectile base (+DA263F)");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The stack slot is not readable/writable for some unforeseen
        // reason - vanishingly unlikely given both were just confirmed live
        // by their respective diagnostic captures, but a fault here would
        // take the whole game down with it, so guard regardless.
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

// U-26: the handler's orphan-trap claims, logged here - outside the handler - when the count has moved. Called at install
// (prints 0) and from AimHookActive (every frame). COST: one interlocked read when nothing changed.
void SayAimOrphans()
{
    const LONG n = InterlockedCompareExchange(&g_orphanClaims, 0, 0);
    if (n == g_orphanSaid) { return; }
    g_orphanSaid = n;
    // [PROBE: AIMHOOK] gate=none (always on)  site=FLAT  log-only=yes
    LOG("aimhook: [AIMHOOK] ORPHAN TRAP claims: %ld so far (a DR2/DR3 trap with an empty Dr6 at an address this hook "
        "armed; claimed instead of crashing; 0 = none yet) | first: address %p, thread %lu, Dr7 0x%llx",
        static_cast<long>(n),
        reinterpret_cast<void*>(static_cast<uintptr_t>(g_orphanFirstAddr)), static_cast<unsigned long>(g_orphanFirstTid),
        static_cast<unsigned long long>(g_orphanFirstDr7));
}

// Sets or clears DR2 or DR3 on one thread, execute mode (RW=00, LEN=00
// required). `slot` is 2 or 3 - added 2026-09-02 alongside the shotgun's
// separate call site so both targets can be armed independently without
// duplicating this whole function.
bool ArmThread(DWORD tid, uintptr_t addr, bool on, int slot)
{
    if (tid == GetCurrentThreadId()) {
        return true;   // cannot Get/SetThreadContext on self reliably
    }
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                              THREAD_SUSPEND_RESUME,
                          FALSE, tid);
    if (!t) {
        return false;
    }
    SuspendThread(t);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (GetThreadContext(t, &ctx)) {
        if (slot == 2) {
            if (on) {
                ctx.Dr2 = addr;
                ctx.Dr7 &= ~kDr7Mask2;
                ctx.Dr7 |= (1ull << 4);   // L2 enable; RW2=00, LEN2=00 already clear
            } else {
                ctx.Dr2 = 0;
                ctx.Dr7 &= ~kDr7Mask2;
            }
        } else {
            if (on) {
                ctx.Dr3 = addr;
                ctx.Dr7 &= ~kDr7Mask3;
                ctx.Dr7 |= (1ull << 6);   // L3 enable; RW3=00, LEN3=00 already clear
            } else {
                ctx.Dr3 = 0;
                ctx.Dr7 &= ~kDr7Mask3;
            }
        }
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ok = SetThreadContext(t, &ctx) != 0;
    }
    ResumeThread(t);
    CloseHandle(t);
    return ok;
}

int ArmAll(uintptr_t addr, bool on, int slot)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    int n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && ArmThread(te.th32ThreadID, addr, on, slot)) {
                ++n;
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

struct SelfArmArgs {
    uintptr_t addr;
    DWORD tid;
    bool on;
    int slot;
};

DWORD WINAPI ArmSelfFromOutside(LPVOID p)
{
    const auto* a = static_cast<const SelfArmArgs*>(p);
    ArmThread(a->tid, a->addr, a->on, a->slot);
    return 0;
}

bool ArmSelf(uintptr_t addr, bool on, int slot)
{
    SelfArmArgs args{ addr, GetCurrentThreadId(), on, slot };
    HANDLE helper = CreateThread(nullptr, 0, ArmSelfFromOutside, &args, 0, nullptr);
    if (!helper) {
        return false;
    }
    const bool ok = WaitForSingleObject(helper, 2000) == WAIT_OBJECT_0;
    CloseHandle(helper);
    return ok;
}

// ArmAll() at install time only catches threads that already exist - a
// thread the game creates afterward (a worker/thread-pool respawn, e.g.
// across a weapon switch) is never armed and the hook silently misses
// whichever shots happen to route through it. Confirmed happening in
// practice 2026-09-02: the hook installed successfully but never fired once
// across several real shots in a session that switched between weapon
// types. Same fix already proven the same day for the bootstrap's own
// diagnostic tooling (see dllmain.cpp's DiagWatcherThread) - a lightweight
// thread that just keeps re-arming for as long as the hook stays installed.
// Unlike that one-shot diagnostic, this has no time limit: the hook is
// meant to keep working for the whole play session, and the existing
// kMaxHits safety cap (not this loop) is what bounds its lifetime.
volatile LONG g_watcherRunning = 0;

DWORD WINAPI WatcherThread(LPVOID)
{
    while (g_active && !g_expired) {
        ArmAll(g_addr, true, 2);
        ArmAll(g_addr2, true, 3);
        Sleep(250);
    }
    InterlockedExchange(&g_watcherRunning, 0);
    return 0;
}

// Even a 250ms watcher still has a real gap: a thread that is created and
// used again faster than that closes before the next poll ever sees it.
// Confirmed happening in practice 2026-09-02 - a freshly installed hook,
// immediately followed by real shots, produced zero hits at all, with the
// watcher thread already running the whole time. Deterministic fix: hook
// gamedll_x64_rwdi.dll's own import of CreateThread (confirmed a real,
// direct import, not resolved dynamically - see CLAUDE.md) and arm every
// new thread before it ever runs a single instruction, closing the race
// entirely rather than shrinking it.
using PFN_CreateThread = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE,
                                         LPVOID, DWORD, LPDWORD);
PFN_CreateThread g_realCreateThread = nullptr;

HANDLE WINAPI CreateThreadHook(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                               LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags,
                               LPDWORD outTid)
{
    if (!g_realCreateThread) {
        return nullptr;
    }
    // Always create suspended ourselves, regardless of what the caller
    // asked for, so there is a window to arm the breakpoint before any of
    // the new thread's own code can run - then resume it ourselves unless
    // the caller genuinely wanted it left suspended.
    const bool wantsSuspended = (flags & CREATE_SUSPENDED) != 0;
    DWORD tid = 0;
    HANDLE h = g_realCreateThread(sa, stackSize, start, param, flags | CREATE_SUSPENDED, &tid);
    if (h && g_active) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            ctx.Dr2 = g_addr;
            ctx.Dr7 &= ~kDr7Mask2;
            ctx.Dr7 |= (1ull << 4);   // L2 enable; RW2=00, LEN2=00 already clear
            ctx.Dr3 = g_addr2;
            ctx.Dr7 &= ~kDr7Mask3;
            ctx.Dr7 |= (1ull << 6);   // L3 enable; RW3=00, LEN3=00 already clear
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            SetThreadContext(h, &ctx);
        }
    }
    if (h && !wantsSuspended) {
        ResumeThread(h);
    }
    if (outTid) {
        *outTid = tid;
    }
    return h;
}

// Installed once, the first time the hook is ever installed, and left in
// place afterward - CreateThreadHook() itself checks g_active before doing
// anything, so it is a harmless no-op while the aim hook is uninstalled.
void InstallCreateThreadHookOnce()
{
    if (g_realCreateThread) {
        return;
    }
    HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gamedll) {
        return;
    }
    void* orig = ::dlvr::PatchIat(gamedll, "KERNEL32.dll", "CreateThread",
                                  reinterpret_cast<void*>(&CreateThreadHook));
    if (orig) {
        g_realCreateThread = reinterpret_cast<PFN_CreateThread>(orig);
        LOG("aimhook: CreateThread hook installed - new threads will be armed at birth");
    } else {
        LOG("aimhook: CreateThread NOT FOUND in gamedll's imports - new threads will only "
            "be caught by the periodic watcher (250ms window)");
    }
}

// fire_barrel_origin (INV-277). fa8, our gamedll: Bullet::Init +7128C0 prologue `push rbx | sub rsp,20h | movss xmm0,
// [rcx+3Ch] | xorps xmm1,xmm1` = 14 bytes, no RIP-relative operand (boneblend antiwall uses the same set). The firearm
// dispatcher's call is +DA16DA (`mov rcx, r13 | call +7128C0`), return address +DA16DF. Init copies bullet+60..+68 into
// its current/previous position (and fills +220 from it only when +220 is zero), so moving +60 BEFORE Init moves the ray.
const unsigned char kBulletInitBytes[14] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F, 0x10, 0x41, 0x3C,
                                             0x0F, 0x57, 0xC9 };
using BulletInitFn = void(__fastcall*)(void*);
BulletInitFn g_trueBulletInit = nullptr;
uintptr_t g_fireInitRet = 0;              // gamedll+0xDA16DF
volatile LONG g_barrelTried = 0;
// Counters since the last window line (shared by every thread; a lost increment costs a count, never a shot).
volatile LONG g_orgMoved = 0, g_orgRefused = 0, g_orgFirst = 0;
const char* volatile g_orgLastWhy = "none";
float g_orgSumStart = 0.0f, g_orgSumGame = 0.0f, g_orgMaxStart = 0.0f;
float g_orgSumAx[3] = {};                 // angle between the shot direction and each barrel-element axis, summed
volatile LONG64 g_orgLastLogMs = 0;

// Unsigned angle (deg) between the shot direction and the element matrix's column k (row-major mtx34: column k =
// m[k], m[4+k], m[8+k]). 0 = along +axis, 180 = along -axis. -1 = degenerate.
float AxisAngleDeg(const float* m, int k, const float* dir)
{
    const float ax = m[k], ay = m[4 + k], az = m[8 + k];
    const float l = sqrtf(ax * ax + ay * ay + az * az);
    if (!(l > 1e-6f)) { return -1.0f; }
    float c = (ax * dir[0] + ay * dir[1] + az * dir[2]) / l;
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return acosf(c) * 57.29578f;
}

// The controller's AIM forward vs its GRIP forward (OpenXR -Z of each), once - the rig places the gun from GRIP
// (rig_hand_grip_pose=1, boneblend :7905), the shot takes AIM (GetControllerPose). -1 = a pose was unavailable.
float AimVsGripDeg()
{
    XrPosef a, g;
    if (!GetControllerPose(kRightHand, a) || !GetControllerGripPose(kRightHand, g)) { return -1.0f; }
    float ax, ay, az, gx, gy, gz;
    RotateVectorByQuat(a.orientation, 0.0f, 0.0f, -1.0f, ax, ay, az);
    RotateVectorByQuat(g.orientation, 0.0f, 0.0f, -1.0f, gx, gy, gz);
    float c = ax * gx + ay * gy + az * gz;
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return acosf(c) * 57.29578f;
}

// bow_aim_from_bow (INV-495). SpawnArrow +D9CF40 calls Bullet::Init at +D9D5CE (return +D9D5D3) after writing bullet+60
// (start = owner +1840, the head point), +72 (head-look dir x arrow speed) and +220 (the tracer start = the bow's
// arrow_pose). Before Init copies them: start and tracer := the LEFT item's arrow_pose, velocity := |v| x arrow_pose +Z.
uintptr_t g_bowInitRet = 0;               // gamedll+0xD9D5D3; 0 = the call site did not verify (branch never matches)
uintptr_t g_bowZeroVec = 0;               // gamedll+0x1CFB510 (the loaded build's), set with g_bowInitRet
volatile LONG g_bowMoved = 0, g_bowRefused = 0;
// bow_reticle_3d (INV-496): the last arrow's own acceleration = bullet +132 (SpawnArrow writes 1.0) x +136..+144 (the owner's
// PhysicsBind gravity) - what the bullet step integrates (INV-278). Seqlock; read by grappleaim's bow arc.
volatile LONG g_arrowAccSeq = 0;
float         g_arrowAcc[3] = {};
bool          g_haveArrowAcc = false;

// [BOWRELEASE] (Jay 2026-10-08: arrows go LEFT of the bow reticle at range). Every arrow: the spawn-time arrow_pose +Z
// (nd) against the published bow direction 0 / 50 / 100 / 200 / 300 ms earlier and against the last DRAWN one, split on the
// RENDERED view's right / up axes (camhook, INV-279: columns right/up/back) - "R +x" = the arrow leaves x deg to the RIGHT of
// that earlier line, "U" = UP. Small-angle (difference vector x 57.3). With bow_fire_latched on (live) and the drawn line
// <= 400 ms old, nd := the drawn line. Returns whether it replaced nd.
bool BowReleaseProbe(float nd[3])
{
    float vm[12] = {};
    unsigned vAge = 0;
    const bool haveView = CamHookGetRenderedViewMatrix(vm, &vAge);
    const float rt[3] = { vm[0], vm[4], vm[8] }, up[3] = { vm[1], vm[5], vm[9] };
    const float spawn[3] = { nd[0], nd[1], nd[2] };
    auto split = [&](const float o[3], float& lat, float& vert) {
        const float d[3] = { spawn[0] - o[0], spawn[1] - o[1], spawn[2] - o[2] };
        lat = (d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2]) * 57.29578f;
        vert = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) * 57.29578f;
    };
    static const unsigned kAgo[5] = { 0, 50, 100, 200, 300 };
    char hist[320] = {};
    size_t n = 0;
    for (unsigned a : kAgo) {
        float o[3];
        int st = -99;
        unsigned act = 0;
        if (GetBowRayAgo(a, o, &st, &act)) {
            float lat = 0.0f, vert = 0.0f;
            split(o, lat, vert);
            n += static_cast<size_t>(snprintf(hist + n, sizeof(hist) - n, " | %ums(%u st %d): R %+.2f U %+.2f", a, act, st, lat,
                                              vert));
        } else {
            n += static_cast<size_t>(snprintf(hist + n, sizeof(hist) - n, " | %ums: none", a));
        }
        if (n >= sizeof(hist)) { break; }
    }
    float ld[3] = {};
    unsigned lAge = 0;
    const bool haveL = GetLastDrawnBowRay(ld, &lAge);
    float lLat = 0.0f, lVert = 0.0f;
    if (haveL) { split(ld, lLat, lVert); }
    const float ll = sqrtf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
    const bool use = BowFireLatched && haveL && lAge <= 400 && ll > 1e-6f && std::isfinite(ll);
    if (use) {
        nd[0] = ld[0] / ll; nd[1] = ld[1] / ll; nd[2] = ld[2] / ll;
    }
    // [PROBE: BOWRELEASE] gate=bow_aim_from_bow  site=HEADSET  log-only=no
    LOG("aimhook: [BOWRELEASE] arrow vs the DRAWN line (%s, %u ms old): R %+.2f U %+.2f deg | bow_fire_latched=%d -> %s | "
        "view axes %s (age %u ms)%s",
        haveL ? "the last state-1/3 publish" : "NONE", lAge, lLat, lVert, BowFireLatched ? 1 : 0,
        use ? "FIRED ALONG THE DRAWN LINE" : (BowFireLatched ? "*** drawn line missing / older than 400 ms - spawn pose kept ***"
                                                             : "spawn pose kept"),
        haveView ? "ok" : "*** UNAVAILABLE - R/U read 0 ***", vAge, hist);
    return use;
}

void BowArrowFromBow(void* bullet)
{
    float m[12] = {};
    uintptr_t item = 0;
    int id = -1, axis = -1;
    const char* name = "none";
    const char* why = "?";
    bool have = HeldItemBarrelHelper(1, m, &item, &id, &name, &axis, &why);
    if (have && axis != 2) {
        have = false;
        why = "the LEFT item's barrel helper is not a *_pose (+Z) element";
    }
    bool ok = false;
    float s[3] = {}, g[3] = {}, od[3] = {}, nd[3] = {}, dStart = 0.0f, dGame = 0.0f, speed = 0.0f, turn = -1.0f;
    // The arrow's gravity, read for EVERY arrow (moved or not): +132 factor, +136..+144 vector, +32 (BulletGravityFactor's slot,
    // reader untraced - INV-496), and the static fallback vector SpawnArrow uses without a physics bind (gamedll +1CFB510).
    float gf = 0.0f, gv[3] = {}, g32 = 0.0f, gs[3] = {};
    bool gOk = false;
    __try {
        const auto* q = reinterpret_cast<const float*>(bullet);
        gf = q[33]; gv[0] = q[34]; gv[1] = q[35]; gv[2] = q[36]; g32 = q[8];
        const auto* st = reinterpret_cast<const float*>(g_bowZeroVec);
        gs[0] = st[0]; gs[1] = st[1]; gs[2] = st[2];
        gOk = std::isfinite(gf) && std::isfinite(gv[0]) && std::isfinite(gv[1]) && std::isfinite(gv[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gOk = false;
    }
    if (gOk) {
        InterlockedIncrement(&g_arrowAccSeq);   // odd = writing
        MemoryBarrier();
        g_arrowAcc[0] = gf * gv[0]; g_arrowAcc[1] = gf * gv[1]; g_arrowAcc[2] = gf * gv[2];
        g_haveArrowAcc = true;
        MemoryBarrier();
        InterlockedIncrement(&g_arrowAccSeq);   // even = stable
    }
    // START + DIRECTION (Jay 2026-10-30: "When something needs to be a permanent fix, you don't leave room for error").
    // arrow_pose is NOT the nock at the spawn: the shoot clip flies it out with the arrow (+2.28 m ahead .. -1.41 m behind along
    // bone_root X, INV-541), so a distance cut on it refused real shots (run B1 10-30: 7 of 13, 1.04-1.28 m from bone_root).
    // START = the bow's REST nock read from bone_root NOW - bone_root local (-0.1992, 0, 0), the mesh rest (INV-495 / INV-541).
    // bone_root moves <= 8 mm / 3 deg in the shoot clip and rides the hand (run B1, state 4: grip 0.00 deg / 0.0000 m from the
    // fixed rel). Not the drawn nock: that is 78-110 ms old at the spawn (396 releases in backups/) and lags a moving player.
    // DIRECTION = the drawn line (bow_fire_latched via BowReleaseProbe; missing on 0 of 396), else arrow_pose +Z.
    // Refused only for a non-finite start, no direction or zero speed - never sent to the head line by a distance.
    // No bone_root (or unreadable) -> the start is arrow_pose, with no distance cut.
    constexpr float kRestNockX = -0.1992f;
    float st[3] = {}, rm[12] = {};
    float dPose = -1.0f, dRootPose = -1.0f;
    const char* ref = "none";
    bool latched = false;
    if (have) {
        const int rid = BowItemElemId(item, "bone_root");   // one GetElementID per arrow (event-driven, a few a minute)
        if (rid >= 0 && BowItemElemWorld(item, rid, rm)) {
            st[0] = rm[3] + kRestNockX * rm[0]; st[1] = rm[7] + kRestNockX * rm[4]; st[2] = rm[11] + kRestNockX * rm[8];
            ref = "the bow's rest nock (bone_root now)";
            dRootPose = sqrtf((m[3] - rm[3]) * (m[3] - rm[3]) + (m[7] - rm[7]) * (m[7] - rm[7]) +
                              (m[11] - rm[11]) * (m[11] - rm[11]));
        } else {
            st[0] = m[3]; st[1] = m[7]; st[2] = m[11];
            ref = rid < 0 ? "arrow_pose (the bow has no bone_root)" : "arrow_pose (bone_root unreadable)";
        }
        dPose = sqrtf((st[0] - m[3]) * (st[0] - m[3]) + (st[1] - m[7]) * (st[1] - m[7]) + (st[2] - m[11]) * (st[2] - m[11]));
    }
    if (have) {
        __try {
            auto* p = reinterpret_cast<float*>(bullet);
            s[0] = p[15]; s[1] = p[16]; s[2] = p[17];    // +60  start (owner +1840, the head point)
            g[0] = p[55]; g[1] = p[56]; g[2] = p[57];    // +220 the game's tracer start (its barrel helper)
            speed = sqrtf(p[18] * p[18] + p[19] * p[19] + p[20] * p[20]);   // +72 velocity = dir x arrow speed
            const float fl = sqrtf(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
            dStart = sqrtf((st[0] - s[0]) * (st[0] - s[0]) + (st[1] - s[1]) * (st[1] - s[1]) + (st[2] - s[2]) * (st[2] - s[2]));
            dGame = sqrtf((st[0] - g[0]) * (st[0] - g[0]) + (st[1] - g[1]) * (st[1] - g[1]) + (st[2] - g[2]) * (st[2] - g[2]));
            if (fl > 1e-6f && std::isfinite(fl)) {
                nd[0] = m[2] / fl; nd[1] = m[6] / fl; nd[2] = m[10] / fl;
            }
            latched = BowReleaseProbe(nd);   // [BOWRELEASE] every arrow; bow_fire_latched (live) may replace nd with the drawn line
            const float nl = sqrtf(nd[0] * nd[0] + nd[1] * nd[1] + nd[2] * nd[2]);
            const char* bad = nullptr;
            if (!(std::isfinite(st[0]) && std::isfinite(st[1]) && std::isfinite(st[2]))) {
                bad = "the start is not finite";
            } else if (!(nl > 0.5f && std::isfinite(nl))) {
                bad = "no direction (no fresh drawn line and a degenerate arrow_pose +Z)";
            } else if (!(speed > 1e-4f && std::isfinite(speed))) {
                bad = "zero or non-finite arrow speed";
            }
            if (!bad) {
                od[0] = p[18] / speed; od[1] = p[19] / speed; od[2] = p[20] / speed;
                nd[0] /= nl; nd[1] /= nl; nd[2] /= nl;
                float c = od[0] * nd[0] + od[1] * nd[1] + od[2] * nd[2];
                c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
                turn = acosf(c) * 57.29578f;
                p[15] = st[0]; p[16] = st[1]; p[17] = st[2];   // the hit ray starts at the bow's nock
                p[55] = st[0]; p[56] = st[1]; p[57] = st[2];   // and so does the tracer
                p[18] = nd[0] * speed; p[19] = nd[1] * speed; p[20] = nd[2] * speed;   // along the drawn line, same speed
                ok = true;
            } else {
                why = bad;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
            why = "bullet unreadable";
        }
    }
    const LONG mv = ok ? InterlockedIncrement(&g_bowMoved) : g_bowMoved;
    const LONG rf = ok ? g_bowRefused : InterlockedIncrement(&g_bowRefused);
    // Every arrow logs (a few per minute at most, event-driven): the run needs each shot, not a sample.
    // [PROBE: BOWAIM] gate=bow_aim_from_bow  site=HEADSET  log-only=no
    if (ok) {
        LOG("aimhook: [BOWAIM] arrow FROM THE BOW: item %p '%s' el %d | start: %s at (%.3f %.3f %.3f), %.3f m from arrow_pose "
            "(the clip's flying arrow; bone_root to arrow_pose %.3f m, -1 = not read) | old start (head point) %.3f m away | the "
            "game's own tracer start %.3f m away | dir (%.4f %.4f %.4f) = %s, was (%.4f %.4f %.4f) = the head look: turned %.1f "
            "deg | speed %.2f kept | moved %ld, refused %ld | gravity %s: +132 %.3f x (%.3f %.3f %.3f), +32 %.3f, static "
            "+1CFB510 (%.3f %.3f %.3f)",
            reinterpret_cast<void*>(item), name, id, ref, st[0], st[1], st[2], dPose, dRootPose, dStart, dGame, nd[0], nd[1],
            nd[2], latched ? "the drawn line" : "arrow_pose +Z (no fresh drawn line)", od[0], od[1], od[2], turn, speed, mv, rf,
            gOk ? "read" : "UNREADABLE", gf, gv[0], gv[1], gv[2], g32, gs[0], gs[1], gs[2]);
    } else {
        LOG("aimhook: [BOWAIM] arrow NOT moved (%s) - item %p '%s' el %d. The arrow keeps the game's head start and "
            "head-look direction. | moved %ld, refused %ld | gravity %s: +132 %.3f x (%.3f %.3f %.3f), +32 %.3f | start %s, "
            "%.3f m from arrow_pose (-1 = not read), drawn line %s, speed %.2f",
            why, reinterpret_cast<void*>(item), name, id, mv, rf, gOk ? "read" : "UNREADABLE", gf, gv[0], gv[1], gv[2], g32,
            ref, dPose, latched ? "used" : "not used", speed);
    }
}

bool ReadLastArrowAccel(float out[3])
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_arrowAccSeq;
        if (s0 & 1) { continue; }
        MemoryBarrier();
        const float a0 = g_arrowAcc[0], a1 = g_arrowAcc[1], a2 = g_arrowAcc[2];
        const bool have = g_haveArrowAcc;
        MemoryBarrier();
        if (g_arrowAccSeq != s0) { continue; }
        if (!have) { return false; }
        out[0] = a0; out[1] = a1; out[2] = a2;
        return true;
    }
    return false;
}

// F4 (docs/AUDIT-COOP-PER-PLAYER-2026-11-13.md, trace 20261113_1810): a bullet's +8 = its owner's +2336 interface, written by the
// firearm dispatcher from the fire controller's vfunc +32 player (ida/fa2_ours_out.txt:850-854; INV-278) - the game's local PlayerDI
// when the shot is ours (INV-594). Unreadable chain or bullet+8 -> true = today's behaviour. COST per firearm shot: 4 guarded reads
// + one guarded qword read.
volatile LONG64 g_fbRemote = 0;
volatile LONG   g_fbSaidRemote = 0;
uintptr_t FbOwnerIface(void* bullet)
{
    __try { return *reinterpret_cast<const uintptr_t*>(reinterpret_cast<const unsigned char*>(bullet) + 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool FbShotIsLocal(void* bullet)
{
    int why = -1;
    const uintptr_t local = RigGamePlayerObject(&why);
    if (why != 0 || !local) { return true; }
    const uintptr_t iface = FbOwnerIface(bullet);
    if (!iface) { return true; }
    return iface == local + 2336;
}

void __fastcall BulletInitHook(void* bullet)
{
    if (BowAimFromBow && g_bowInitRet && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_bowInitRet) {
        BowArrowFromBow(bullet);
        g_trueBulletInit(bullet);
        return;
    }
    if (FireBarrelOrigin && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_fireInitRet && !FbShotIsLocal(bullet)) {
        // F4 (docs/AUDIT-COOP-PER-PLAYER-2026-11-13.md, trace 20261113_1810): another player's shot - the game's own start and
        // tracer, and no two-hand pulse on OUR controllers.
        InterlockedIncrement64(&g_fbRemote);
        if (InterlockedExchange(&g_fbSaidRemote, 1) == 0) {
            // [PROBE: FIREORIGIN] gate=fire_barrel_origin  site=HEADSET  log-only=yes
            LOG("aimhook: [FIREORIGIN] REMOTE shot (bullet %p, owner iface %p) - origin and tracer left to the game, no two-hand "
                "pulse (co-op: local player's shots only; logged once)", bullet, reinterpret_cast<void*>(FbOwnerIface(bullet)));
        }
        g_trueBulletInit(bullet);
        return;
    }
    if (FireBarrelOrigin && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_fireInitRet) {
        // [LOADBEARING: rig_two_hand_haptic] a player gun shot - pulse both controllers while two-handing. One interlocked
        // read when not gripping; PulseController is one interlocked store (applied on the frame thread, controllers.cpp:715).
        TwoHandNoteShot();
        // The REAL barrel: the held gun's own "barrel" element (INV-189 lists it on the ARs), NOT bullet+220, which the
        // game fills from +BCEC80's fallback (body + 1.2 m forward) in VR - INV-277 correction.
        float m[12] = {};
        uintptr_t item = 0;
        int id = -1;
        const char* why = "?";
        // fire_barrel_helper: a crossbow's start is its bolt_pose (the game's own barrel point, INV-494), not "barrel".
        const bool have = FireBarrelHelper ? HeldItemBarrelHelper(0, m, &item, &id, nullptr, nullptr, &why)
                                           : HeldItemElementWorld(0, "barrel", m, &item, &id, &why);
        bool ok = false;
        float s[3] = {}, g[3] = {}, dir[3] = {}, dStart = 0.0f, dGame = 0.0f, ang[3] = { -1.0f, -1.0f, -1.0f };
        if (have) {
            __try {
                auto* p = reinterpret_cast<float*>(bullet);
                s[0] = p[15]; s[1] = p[16]; s[2] = p[17];    // +60  start (owner +1840)
                g[0] = p[55]; g[1] = p[56]; g[2] = p[57];    // +220 the game's "barrel" (its fallback in VR)
                const float bx = m[3], by = m[7], bz = m[11];
                dStart = sqrtf((bx - s[0]) * (bx - s[0]) + (by - s[1]) * (by - s[1]) + (bz - s[2]) * (bz - s[2]));
                dGame = sqrtf((bx - g[0]) * (bx - g[0]) + (by - g[1]) * (by - g[1]) + (bz - g[2]) * (bz - g[2]));
                const float vl = sqrtf(p[18] * p[18] + p[19] * p[19] + p[20] * p[20]);   // +72 velocity
                if (std::isfinite(bx) && std::isfinite(by) && std::isfinite(bz) && std::isfinite(dStart) &&
                    dStart < 2.0f && vl > 1e-4f) {
                    dir[0] = p[18] / vl; dir[1] = p[19] / vl; dir[2] = p[20] / vl;
                    p[15] = bx; p[16] = by; p[17] = bz;        // the hit ray starts at the muzzle
                    p[55] = bx; p[56] = by; p[57] = bz;        // and so does the tracer
                    ok = true;
                } else {
                    why = "barrel element >= 2 m from the start, non-finite, or zero velocity";
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
                why = "bullet unreadable";
            }
        }
        if (ok) {
            for (int k = 0; k < 3; ++k) { ang[k] = AxisAngleDeg(m, k, dir); }
            InterlockedIncrement(&g_orgMoved);
            g_orgSumStart += dStart;
            g_orgSumGame += dGame;
            if (dStart > g_orgMaxStart) { g_orgMaxStart = dStart; }
            for (int k = 0; k < 3; ++k) { g_orgSumAx[k] += ang[k]; }
            if (InterlockedCompareExchange(&g_orgFirst, 1, 0) == 0) {
                // [PROBE: FIREORIGIN] gate=fire_barrel_origin  site=HEADSET  log-only=no
                LOG("aimhook: [FIREORIGIN] FIRST shot from the REAL barrel: item %p element 'barrel' = %d at (%.3f %.3f %.3f) | "
                    "old start (%.3f %.3f %.3f) %.3f m away | the game's own +220 (%.3f %.3f %.3f) %.3f m away | shot dir vs "
                    "barrel axes X %.1f Y %.1f Z %.1f deg (0/180 = along that axis) | controller AIM vs GRIP forward %.1f deg "
                    "(logs regardless of diag_fire_spread)",
                    reinterpret_cast<void*>(item), id, m[3], m[7], m[11], s[0], s[1], s[2], dStart, g[0], g[1], g[2], dGame,
                    ang[0], ang[1], ang[2], AimVsGripDeg());
            }
        } else {
            InterlockedIncrement(&g_orgRefused);
            g_orgLastWhy = why;
            if (InterlockedCompareExchange(&g_orgFirst, 2, 0) == 0) {
                // [PROBE: FIREORIGIN] gate=fire_barrel_origin  site=HEADSET  log-only=no
                LOG("aimhook: [FIREORIGIN] FIRST shot NOT moved (%s) - item %p, element id %d. The shot keeps the game's start.",
                    why, reinterpret_cast<void*>(item), id);
            }
        }
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        const LONG64 last = g_orgLastLogMs;
        if (DiagFireSpread && now - last >= 250 && InterlockedCompareExchange64(&g_orgLastLogMs, now, last) == last) {
            const LONG mv = InterlockedExchange(&g_orgMoved, 0), rf = InterlockedExchange(&g_orgRefused, 0);
            const float n = mv ? static_cast<float>(mv) : 1.0f;
            // [PROBE: FIREORIGIN] gate=diag_fire_spread  site=HEADSET  log-only=yes
            LOG("aimhook: [FIREORIGIN] window: moved to the real barrel %ld, refused %ld (last reason: %s) | barrel vs old start "
                "mean %.3f max %.3f m | vs the game's +220 mean %.3f m | shot dir vs barrel axes mean X %.1f Y %.1f Z %.1f deg",
                mv, rf, rf ? g_orgLastWhy : "-", g_orgSumStart / n, g_orgMaxStart, g_orgSumGame / n,
                g_orgSumAx[0] / n, g_orgSumAx[1] / n, g_orgSumAx[2] / n);
            g_orgSumStart = 0.0f; g_orgSumGame = 0.0f; g_orgMaxStart = 0.0f;
            g_orgSumAx[0] = g_orgSumAx[1] = g_orgSumAx[2] = 0.0f;
        }
    }
    g_trueBulletInit(bullet);
}

void InstallBarrelOriginOnce(HMODULE gd)
{
    if (!(FireBarrelOrigin || BowAimFromBow) || InterlockedExchange(&g_barrelTried, 1)) {
        return;
    }
    const uintptr_t g = reinterpret_cast<uintptr_t>(gd);
    // Every rva below is the loaded build's (common/buildrva.h: Steam unchanged, the EGL table on the Epic build); the
    // detour itself is handed the Steam address, which InstallDetour moves (the same choke point as every detour).
    using ::dlvr::buildrva::Gd;
    const uintptr_t init = g + Gd(0x7128C0);
    // The call must still be where fa8 put it: E8 rel32 at +DA16DA resolving to +7128C0.
    const unsigned char* c = reinterpret_cast<const unsigned char*>(g + Gd(0xDA16DA));
    int32_t rel = 0;
    memcpy(&rel, c + 1, 4);
    const bool fireSiteOk = c[0] == 0xE8 && g + Gd(0xDA16DF) + rel == init;
    g_fireInitRet = (FireBarrelOrigin && fireSiteOk) ? g + Gd(0xDA16DF) : 0;   // 0 never matches a return address
    // bow_aim_from_bow (INV-495): SpawnArrow's call, E8 rel32 at +D9D5CE resolving to +7128C0 (byte scan of 97046bba).
    const unsigned char* cb = reinterpret_cast<const unsigned char*>(g + Gd(0xD9D5CE));
    int32_t relb = 0;
    memcpy(&relb, cb + 1, 4);
    const bool bowSiteOk = cb[0] == 0xE8 && g + Gd(0xD9D5D3) + relb == init;
    g_bowZeroVec = g + Gd(0x1CFB510);
    g_bowInitRet = (BowAimFromBow && bowSiteOk) ? g + Gd(0xD9D5D3) : 0;
    // [PROBE: BOWAIM] gate=bow_aim_from_bow  site=FLAT  log-only=yes
    LOG("aimhook: [BOWAIM] bow_aim_from_bow=%d | SpawnArrow's Bullet::Init call +D9D5CE %s%s", BowAimFromBow ? 1 : 0,
        bowSiteOk ? "verified (return +D9D5D3)" : "MISMATCH (different game build?)",
        BowAimFromBow ? (bowSiteOk ? " - bow arrows will start at the nock, along the bow" : " - BOW BRANCH DISABLED") : " - off");
    // The detour serves both branches; it is installed when either armed branch's call site verified.
    const bool siteOk = g_fireInitRet != 0 || g_bowInitRet != 0;
    int attempts = 0;
    const bool ok = siteOk && InstallSingleGameDetour(g + 0x7128C0, kBulletInitBytes, sizeof(kBulletInitBytes),
                                                      reinterpret_cast<void*>(&BulletInitHook),
                                                      reinterpret_cast<void**>(&g_trueBulletInit), "fire barrel origin",
                                                      &attempts);
    // [PROBE: FIREORIGIN] gate=fire_barrel_origin  site=FLAT  log-only=yes
    LOG("aimhook: [FIREORIGIN] Bullet::Init detour (gamedll+0x7128C0): %s | fire_barrel_origin=%d, dispatcher call site "
        "+DA16DA %s (filter return +DA16DF %s) | bow branch %s | %d attempt(s)%s",
        ok ? "INSTALLED (committed)" : "*** NOT INSTALLED ***", FireBarrelOrigin ? 1 : 0,
        fireSiteOk ? "verified" : "MISMATCH (different game build?)", g_fireInitRet ? "ARMED" : "off",
        g_bowInitRet ? "ARMED (return +D9D5D3)" : "off", attempts,
        ok ? "" : (siteOk ? " - prologue mismatch (diag_antiwall_probe holds this function?) or commit failure; shots and "
                            "arrows still start at the head."
                          : " - no armed branch verified its call site; nothing to detour."));
}

// fire_move_penalty_off (INV-274 (6)). fa6, our gamedll: +BC39FB `call +DA6B40` (E8 40 31 1E 00, IsAiming) |
// +BC3A00 `test al, al` (84 C0) | +BC3A02 `jz +1C` (74 1C, skips the aiming branch) | +BC3A04 `comiss xmm6,
// [rdi+rbx*8+26CCh]` (0F 2F B4 DF CC 26 00 00) ... +BC3A18 `mov [rdi+rbx*8+26D4h], r15` = target 0. NOPing the jz makes
// UpdateAccuracy always take the aiming branch, whose only effect is the movement term's target (0) and spring rate.
// Once per process: the bytes stay patched across aim-hook re-arms; nothing restores them (off = do not patch at launch).
volatile LONG g_movePatchTried = 0;
void InstallMovePenaltyPatchOnce(HMODULE gd)
{
    if (!FireMovePenaltyOff || InterlockedExchange(&g_movePatchTried, 1)) {
        return;
    }
    const uintptr_t g = reinterpret_cast<uintptr_t>(gd);
    // The loaded build's rva (common/buildrva.h); PatchGameBytes is handed the Steam address and moves it itself.
    const unsigned char* p = reinterpret_cast<const unsigned char*>(g + ::dlvr::buildrva::Gd(0xBC39FB));
    const unsigned char ctxCall[5] = { 0xE8, 0x40, 0x31, 0x1E, 0x00 };
    const unsigned char ctxTest[2] = { 0x84, 0xC0 };
    const unsigned char ctxComiss[8] = { 0x0F, 0x2F, 0xB4, 0xDF, 0xCC, 0x26, 0x00, 0x00 };
    const bool ctxOk = memcmp(p, ctxCall, 5) == 0 && memcmp(p + 5, ctxTest, 2) == 0 &&
                       memcmp(p + 9, ctxComiss, 8) == 0;
    const unsigned char expect[2] = { 0x74, 0x1C };
    const unsigned char repl[2]   = { 0x90, 0x90 };
    const unsigned char was0 = p[7], was1 = p[8];
    int attempts = 0;
    const bool ok = ctxOk && PatchGameBytes(g + 0xBC3A02, expect, repl, 2, &attempts);
    // [PROBE: FIRESPREAD] gate=fire_move_penalty_off  site=FLAT  log-only=yes
    LOG("aimhook: [FIRESPREAD] move-penalty patch (UpdateAccuracy +BC3A02): %s | was %02X %02X, context %s, %d attempt(s)%s",
        ok ? "PATCHED (74 1C -> 90 90) - the movement spread term is held at 0 as when aiming"
           : "*** NOT PATCHED ***",
        was0, was1, ctxOk ? "verified" : "MISMATCH (different game build?)", attempts,
        ok ? "" : " - moving/jumping still widens the cone. Set fire_move_penalty_off=0.");
}

}  // namespace

bool GetLastArrowAccel(float out[3])
{
    return ReadLastArrowAccel(out);
}

bool AimHookInstall()
{
    EnsureCs();
    if (g_active) {
        return true;
    }
    HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gamedll) {
        strncpy_s(g_status, sizeof(g_status), "gamedll_x64_rwdi.dll not loaded", _TRUNCATE);
        return false;
    }
    // Right after the call to ComputeShootDir returns (call site 0xDA0B84 +
    // 5, a call rel32 is 5 bytes) - superseded the original
    // WeaponFireControllerRight write site (0xDA4296), which hung the game
    // outright. This address is NOT proven safe either - it hung the game
    // too, once armed persistently long enough (see the kMaxHits comment
    // above and CLAUDE.md). The hard cap is what actually keeps this safe
    // now, not the address choice on its own. g_addr2 is the shotgun's own
    // separate call site (ShotgunShootDir's internal ComputeShootDir call,
    // 0xDA263A + 5) - confirmed 2026-09-02 that the shotgun never reaches
    // g_addr at all, so it needs this second, independent hook on DR3 to be
    // covered by the redirect.
    // The loaded build's rvas (common/buildrva.h: Steam unchanged, the EGL table on the Epic build) - on EGL the Steam
    // values land inside other code, so the two watches would sit on the wrong instructions.
    g_addr = reinterpret_cast<uintptr_t>(gamedll) + ::dlvr::buildrva::Gd(0xDA0B89);
    g_addr2 = reinterpret_cast<uintptr_t>(gamedll) + ::dlvr::buildrva::Gd(0xDA263F);
    g_orphanAddr = g_addr;     // U-26: before arming - a trap can land while ArmAll walks
    g_orphanAddr2 = g_addr2;
    InstallCreateThreadHookOnce();
    InstallMovePenaltyPatchOnce(gamedll);
    InstallBarrelOriginOnce(gamedll);

    InterlockedExchange(&g_hitTotal, 0);
    InterlockedExchange(&g_expired, 0);
    InterlockedExchange(&g_logCounter, 0);
    g_shotDirValid = false;
    g_shotGeneration = GetAimTriggerGeneration();   // don't re-adopt an old crossing as "new"

    g_veh = AddVectoredExceptionHandler(1, Handler);
    if (!g_veh) {
        strncpy_s(g_status, sizeof(g_status), "could not install the handler", _TRUNCATE);
        return false;
    }
    const int n = ArmAll(g_addr, true, 2);
    const bool selfArmed = ArmSelf(g_addr, true, 2);
    const int n2 = ArmAll(g_addr2, true, 3);
    const bool selfArmed2 = ArmSelf(g_addr2, true, 3);
    g_active = (n > 0) || selfArmed || (n2 > 0) || selfArmed2;
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                g_active ? "installed at %p (%d threads, caller: %s) and %p (%d threads, "
                           "caller: %s)"
                         : "could not arm any thread",
                reinterpret_cast<void*>(g_addr), n, selfArmed ? "yes" : "no",
                reinterpret_cast<void*>(g_addr2), n2, selfArmed2 ? "yes" : "no");
    LOG("aimhook: %s", g_status);
    SayAimOrphans();
    if (!g_active) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
        return false;
    }
    // See WatcherThread's own comment: keeps re-arming any thread the game
    // creates after this point, for as long as the hook stays installed.
    if (!InterlockedCompareExchange(&g_watcherRunning, 1, 0)) {
        if (!CreateThread(nullptr, 0, WatcherThread, nullptr, 0, nullptr)) {
            InterlockedExchange(&g_watcherRunning, 0);
        }
    }
    // Started here, after g_active is actually true - WfcrScanThread's loop
    // condition checks g_active, and starting the thread any earlier (it
    // used to, right after resolving g_addr above) raced the thread against
    // this function's own remaining setup: CreateThread can schedule the
    // new thread before g_active is assigned, so its very first check saw
    // the old (false) value and exited immediately without ever scanning -
    // confirmed happening in practice 2026-09-02, every shot logged
    // "WeaponFireControllerRight not found/unreadable yet" for the whole
    // session despite the thread nominally running the entire time.
    // diag_aim_wfcr_scan (default OFF, INV-155): the scan only feeds a log field, so it is not started unless asked for.
    if (!DiagAimWfcrScan) {
        LOG("aimhook: WeaponFireControllerRight heap scan NOT started - diag_aim_wfcr_scan=0 (INV-155; log field only, "
            "aim unaffected)");
    } else if (!InterlockedCompareExchange(&g_wfcrScanThreadRunning, 1, 0)) {
        if (!CreateThread(nullptr, 0, WfcrScanThread, nullptr, 0, nullptr)) {
            InterlockedExchange(&g_wfcrScanThreadRunning, 0);
        }
    }
    return true;
}

void AimHookUninstall()
{
    if (g_addr) {
        ArmAll(g_addr, false, 2);
        ArmSelf(g_addr, false, 2);
    }
    if (g_addr2) {
        ArmAll(g_addr2, false, 3);
        ArmSelf(g_addr2, false, 3);
    }
    if (g_veh) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
    }
    g_active = false;
    g_addr = 0;
    g_addr2 = 0;
    strncpy_s(g_status, sizeof(g_status), "off", _TRUNCATE);
}

bool AimHookActive()
{
    // The WeaponFireControllerRight scan runs on its own thread (started in
    // AimHookInstall(), see WfcrScanThread) - nothing to poll for it here.
    // The handler disarms its own thread's register once the budget is
    // spent; clean the rest up here, on a thread that is allowed to take
    // locks and call ArmAll/ArmSelf - same pattern the diagnostic tools use.
    if (g_active && g_expired) {
        AimHookUninstall();
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "auto-disarmed after %d hits (safety cap - see CLAUDE.md, "
                    "this is not a per-shot count)",
                    static_cast<int>(kMaxHits));
        LOG("aimhook: %s", g_status);
    }
    SayAimOrphans();
    return g_active;
}

}  // namespace dlvr
