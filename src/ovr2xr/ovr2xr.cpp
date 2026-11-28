// dlvr ovr2xr - a LibOVR 1.x runtime implemented on OpenXR.
//
// This is the real translation layer: Dying Light's renderer talks the Oculus
// CAPI, and we answer it with OpenXR. It ships as LibOVRRT64_1.dll, which the
// game's stock Oculus shim loads (see DESIGN.md for the signature-check bypass
// the bootstrap applies first).
//
// Two structural mismatches drive the design:
//
//  1. LibOVR creates its session (ovr_Create) BEFORE the app has a D3D device;
//     the device only appears at ovr_CreateTextureSwapChainDX. OpenXR needs the
//     graphics binding at xrCreateSession. So ovr_Create only does
//     xrCreateInstance + xrGetSystem, and xrCreateSession is deferred until the
//     first swapchain is created.
//
//  2. ovr_SubmitFrame is a single call, while OpenXR wants
//     xrWaitFrame -> xrBeginFrame -> xrEndFrame. We drive wait+begin from
//     whichever of ovr_GetTrackingState / ovr_GetPredictedDisplayTime the game
//     calls first each frame, and end in ovr_SubmitFrame.
//
// Coordinate systems line up: both LibOVR and OpenXR are right-handed, Y up,
// -Z forward, metres. Poses convert field-for-field with no basis change.
//
// ABI note: built against Oculus SDK 1.10 headers. The game requests
// RequestedMinorVersion=3, and 1.10 still has ovrEyeRenderDesc::HmdToEyeOffset
// (ovrVector3f). SDK 1.17+ renamed that to HmdToEyePose (ovrPosef), which is a
// silent ABI break - do not "upgrade" these headers without re-checking.

#define OVR_DLL_BUILD
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "xrloader.h"

#include <OVR_CAPI.h>
#include <OVR_CAPI_D3D.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "camhook.h"
#include "aimhook.h"
#include "grappleaim.h"
#include "holster.h"
#include "twohand.h"
#include "armfit.h"      // RigArmFit - rig_arm_fit (SPEC-ARM-FIT stage 2)
#include "calibscreen.h" // the calibration screen (build A): calib_posture, its input block, its quad
#include "swing.h"
#include "meleethrow.h"
#include "throwable.h"
#include "looting.h"    // loot by hand (spec 2026-10-23-loot-by-hand-design): LootPoll / LootPadByte, the loot_* settings
#include "lidhand.h"    // hand-open lids (spec 2026-11-18-hand-open-lids-design): LidHandPoll, the lid_* settings
#include "promptquad.h" // the in-world prompt (trace 20261123_1356): the inworld_prompts setting
#include "bodygrab.h"   // body grab (plan 2026-10-23-body-grab): BodyGrabHandActive - the trigger bytes; the body_grab_* settings
#include "camogesture.h"   // the camouflage gesture (plan 2026-11-07-camouflage-gesture): the camo_* settings
#include "breakout.h"   // physical breakout (plan 2026-10-24-physical-breakout): BreakoutPoll; the breakout_* settings
#include "reload.h"     // manual reload (plan 2026-11-01-manual-reload-pistol): the manual_reload setting
#include "reload_logic.h"   // rifle plan Task 5: TwoHandPressRefused (the reload / two-handed press arbitration; pure, no tables)
#include "handcoll.h"   // live-enemy hand collision (plan 2026-10-30-live-enemy-hand-collision): the hand_collision* settings
#include "pushgrab.h"   // open-hand push and grab (plan 2026-10-31-open-hand-push-and-grab): the enemy_push_grab / push_* settings
#include "coopnet.h"    // co-op arm-pose channel (plan 2026-11-13-coop-arm-pose-channel): the coop_pose setting
#include "roomscale.h"  // roomscale (plan 2026-10-27-roomscale-physical-crouch Task 7a): the diag_roomscale_probe setting
#include "climbprobe.h" // climbing P0 (docs/design_climbing_overhaul.md §6): the diag_climb_probe setting
#include "climbhand.h"  // hand climbing P1 (same doc): climb_by_hand / climb_reach_m + ClimbHandPoll in the scePad filter
#include "physcrouch.h" // physical crouch (same plan, Task 9): its settings + PhysCrouchPoll in the scePad filter
#include "bownock.h"
#include "boneblend.h"
#include "controllers.h"
#include "bodytrack.h"          // the body-tracked elbow (rig_elbow_body_tracking), XR_FB_body_tracking
#include "headpose.h"
#include "headfollow_logic.h"   // ReportedHeadPosForFold - SPEC-HEAD-POSITION-FOLD-2026-10-19
#include "recentre_logic.h"     // recentre absorb - D-253
#include "centre_logic.h"       // centre on the head at session start / headset back on - D-284
#include "roomscale_logic.h"    // the roomscale-off drift bleed (BleedStep, StickMoving) - plan 2026-10-27 Task 6
#include "neckpivot_logic.h"    // head_neck_pivot arc - yaw-free under the decouple (2026-10-20)
#include "rtticam.h"
#include "overlay.h"
#include "wristprobe.h"
#include "wristquad.h"   // wrist HUD stage 2: the pieces on quads on the hands (dlvr\wrist_hud.txt)
#include "uipin.h"
#include "uipanel.h"
#include "deskmirror.h"   // the desktop window's picture: view 1's (the right eye's) image, one eye (trace 20261114_1030_desktop_mirror_single_eye)
#include "loadscreen.h"
#include "watchwrite.h"   // WatchExecSayOrphans - the U-26 orphan-trap count on the 600-frame line
#include "settings.h"
#include "snap_logic.h"
#include "menutap_logic.h"  // the left menu button's tap / hold machine + the co-op teleport latch (INV-745)
#include "menuchord_logic.h"  // SteamVR: X + Y together = the menu button (trace 20261118_1047_steamvr_xy_menu_chord)
#include "fovtable_logic.h"   // first-launch FOV rows (no hmd_cache.txt yet)
#include "cantview_logic.h"   // canted views -> parallel (LocateViews; trace 20261118_1150_canted_views)
#include "fovcrop_logic.h"    // SteamVR: the projection view cut to the panel window (trace 20261118_1345_steamvr_panel_crop)
#include "eyefov_logic.h"     // one frustum for both eyes (INV-597)
#include "videoforce_logic.h" // vr_force_vsync_off's bring-up stub
#include "buggywheel.h"       // buggy_hand_steer (BuggyHandSteer)

#include "../common/buildrva.h"   // VSYNCOFF's store on the Epic build
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC + DiagPeriodicLogs - diag_periodic_logs

// Logs only the first N calls at a site. Entry points that run every frame
// would otherwise bury the interesting startup sequence, but the startup
// sequence is exactly where a crash lands.
#define LOG_FIRST(n, ...)                         do {                                              static int _dlvr_hits = 0;                    if (_dlvr_hits < (n)) {                           ++_dlvr_hits;                                 LOG(__VA_ARGS__);                         }                                         } while (0)

using namespace dlvr;

// Exported by xinput1_3.dll (the bootstrap) for the milestone D eye handover.
// See the call site in ovr_GetRenderDesc.
typedef void (*PFN_DlvrNoteEye)(int eye);
typedef void (*PFN_DlvrNoteTex)(void* tex);

// Jitter route test - see ovr_GetRenderDesc. Marker file rather than a setting
// because it belongs with the other DLSS markers and is a one-run experiment,
// not a mode anyone should leave on.
bool g_jitterTest = false;
constexpr float kJitterTestPixels = 10.0f;

namespace {

// ---------------------------------------------------------------- helpers ---

bool XrOk(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r)) {
        return true;
    }
    LOG("%s failed: XrResult %d", what, static_cast<int>(r));
    return false;
}

ovrPosef ToOvr(const XrPosef& p)
{
    ovrPosef o;
    o.Orientation.x = p.orientation.x;
    o.Orientation.y = p.orientation.y;
    o.Orientation.z = p.orientation.z;
    o.Orientation.w = p.orientation.w;
    o.Position.x = p.position.x;
    o.Position.y = p.position.y;
    o.Position.z = p.position.z;
    return o;
}

XrPosef ToXr(const ovrPosef& p)
{
    XrPosef o;
    o.orientation.x = p.Orientation.x;
    o.orientation.y = p.Orientation.y;
    o.orientation.z = p.Orientation.z;
    o.orientation.w = p.Orientation.w;
    o.position.x = p.Position.x;
    o.position.y = p.Position.y;
    o.position.z = p.Position.z;
    return o;
}

ovrPosef IdentityPose()
{
    ovrPosef p{};
    p.Orientation.w = 1.0f;
    return p;
}

XrPosef XrIdentity()
{
    XrPosef p{};
    p.orientation.w = 1.0f;
    return p;
}

// OpenXR gives signed angles in radians; LibOVR wants positive tangents.
// angleLeft and angleDown are negative, hence the negations.
ovrFovPort ToOvrFov(const XrFovf& f)
{
    ovrFovPort o;
    o.UpTan = tanf(f.angleUp);
    o.DownTan = -tanf(f.angleDown);
    o.LeftTan = -tanf(f.angleLeft);
    o.RightTan = tanf(f.angleRight);
    return o;
}

XrFovf ToXrFov(const ovrFovPort& f)
{
    XrFovf o;
    o.angleUp = atanf(f.UpTan);
    o.angleDown = -atanf(f.DownTan);
    o.angleLeft = -atanf(f.LeftTan);
    o.angleRight = atanf(f.RightTan);
    return o;
}

// The runtime's clock, for everything the game is told a time in.
PFN_xrConvertWin32PerformanceCounterToTimeKHR g_qpcToXrTime = nullptr;
XrInstance g_timeInstance = XR_NULL_HANDLE;

double NowSeconds()
{
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

double XrTimeToSeconds(XrTime t)
{
    return static_cast<double>(t) / 1e9;
}

// "Now", expressed on the same clock as predictedDisplayTime.
//
// This is the one the game must be given. LibOVR lets an application compute
// ovr_GetPredictedDisplayTime() - ovr_GetTimeInSeconds() and treat the result as
// how far ahead it is rendering. We were answering the first from XrTime, whose
// epoch is the runtime's business, and the second from QueryPerformanceCounter,
// whose epoch is the boot. Subtracting one from the other is meaningless, and a
// meaningless prediction interval does not show up as a constant offset - it
// shows up as motion that is wrong by an amount that changes with frame timing.
double NowOnRuntimeClock()
{
    if (g_qpcToXrTime && g_timeInstance != XR_NULL_HANDLE) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        XrTime t = 0;
        if (XR_SUCCEEDED(g_qpcToXrTime(g_timeInstance, &c, &t))) {
            return XrTimeToSeconds(t);
        }
    }
    return NowSeconds();
}

DXGI_FORMAT ToDxgi(ovrTextureFormat f)
{
    switch (f) {
        case OVR_FORMAT_B5G6R5_UNORM:        return DXGI_FORMAT_B5G6R5_UNORM;
        case OVR_FORMAT_B5G5R5A1_UNORM:      return DXGI_FORMAT_B5G5R5A1_UNORM;
        case OVR_FORMAT_B4G4R4A4_UNORM:      return DXGI_FORMAT_B4G4R4A4_UNORM;
        case OVR_FORMAT_R8G8B8A8_UNORM:      return DXGI_FORMAT_R8G8B8A8_UNORM;
        case OVR_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case OVR_FORMAT_B8G8R8A8_UNORM:      return DXGI_FORMAT_B8G8R8A8_UNORM;
        case OVR_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case OVR_FORMAT_B8G8R8X8_UNORM:      return DXGI_FORMAT_B8G8R8X8_UNORM;
        case OVR_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        case OVR_FORMAT_R16G16B16A16_FLOAT:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case OVR_FORMAT_R11G11B10_FLOAT:     return DXGI_FORMAT_R11G11B10_FLOAT;
        case OVR_FORMAT_D16_UNORM:           return DXGI_FORMAT_D16_UNORM;
        case OVR_FORMAT_D24_UNORM_S8_UINT:   return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case OVR_FORMAT_D32_FLOAT:           return DXGI_FORMAT_D32_FLOAT;
        default:                             return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }
}

// ------------------------------------------------------------------ state ---

// The frame state machine is reachable from more than one thread, and until now
// nothing serialised it.
//
// ovr_GetTrackingState, ovr_GetPredictedDisplayTime and ovr_SubmitFrame all
// drive it, and a 2015 engine is under no obligation to call them from the same
// thread. Two threads could each observe frameActive == false and each run
// xrWaitFrame + xrBeginFrame, which OpenXR explicitly forbids - xrBeginFrame
// and xrEndFrame require external synchronisation - and s->views, s->frameState
// and s->headPose were written by one thread and read by another with no
// barrier at all.
//
// That is a timing-dependent crash that does not care what resolution you
// render at, which is exactly the behaviour observed: intermittent, and
// unaffected by halving the render size.
//
// A CRITICAL_SECTION is recursive on the same thread, so the nested
// ovr_SubmitFrame -> BeginFrameIfNeeded path is safe. Holding it across
// xrWaitFrame is safe too: we only reach the wait when no frame is in flight,
// so it can block on the compositor but never on one of our own threads.
struct FrameLock {
    CRITICAL_SECTION cs{};
    FrameLock() { InitializeCriticalSection(&cs); }
    ~FrameLock() { DeleteCriticalSection(&cs); }
};
FrameLock g_frameLock;

struct Guard {
    Guard() { EnterCriticalSection(&g_frameLock.cs); }
    ~Guard() { LeaveCriticalSection(&g_frameLock.cs); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// Drops the frame lock for the duration of a scope, then takes it back.
//
// For one call only: xrWaitFrame. It blocks until the runtime wants the next
// frame started, which at 72 Hz is most of a 13.89 ms period, and it was being
// called with the lock held. Every engine thread that wanted a pose was parked
// behind it, so the game got roughly two milliseconds per frame in which it was
// allowed to run at all - and a frame needing more than that could not overlap
// its work with our wait, it could only start after it. That turns an ordinary
// heavy frame into a multi-period stall.
//
// A critical section is recursive, so this only releases if the count is one.
// BeginFrameIfNeeded no longer takes the lock itself for exactly that reason:
// its callers hold it, and a second acquisition here would make the release a
// no-op while looking correct.
struct Unguard {
    Unguard() { LeaveCriticalSection(&g_frameLock.cs); }
    ~Unguard() { EnterCriticalSection(&g_frameLock.cs); }
    Unguard(const Unguard&) = delete;
    Unguard& operator=(const Unguard&) = delete;
};

// Records the distinct threads each call site is reached from, one log line per
// new thread. An earlier version logged every CHANGE of thread, which on a job
// system meant two file writes per frame from inside the frame lock - 300 KB of
// log in two minutes, and a stall where the diagnostic was supposed to be.
//
// Answer, measured: ovr_GetTrackingState and ovr_GetPredictedDisplayTime come
// from sixteen different threads. ovr_SubmitFrame comes from exactly one.
struct ThreadSet {
    static const int kMax = 16;
    DWORD ids[kMax]{};
    int count = 0;
    bool overflowed = false;
};

// Callers hold the frame lock, so the set needs no synchronisation of its own.
void NoteThread(const char* site, ThreadSet& set)
{
    const DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < set.count; ++i) {
        if (set.ids[i] == tid) {
            return;
        }
    }
    if (set.count < ThreadSet::kMax) {
        set.ids[set.count++] = tid;
        LOG("thread: %s called from thread %lu (%d distinct so far)",
            site, tid, set.count);
    } else if (!set.overflowed) {
        set.overflowed = true;
        LOG("thread: %s reached from more than %d threads - this is a job-system "
            "worker pool, not a fixed thread", site, ThreadSet::kMax);
    }
}

XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

// Rotates a vector by the INVERSE of a unit quaternion, i.e. takes a vector
// from tracking space into the space that quaternion describes.
XrVector3f UnrotateBy(const XrQuaternionf& q, const XrVector3f& v)
{
    // Conjugate: same rotation, opposite direction.
    const float x = -q.x, y = -q.y, z = -q.z, w = q.w;
    const float tx = 2.0f * (y * v.z - z * v.y);
    const float ty = 2.0f * (z * v.x - x * v.z);
    const float tz = 2.0f * (x * v.y - y * v.x);
    XrVector3f out;
    out.x = v.x + w * tx + (y * tz - z * ty);
    out.y = v.y + w * ty + (z * tx - x * tz);
    out.z = v.z + w * tz + (x * ty - y * tx);
    return out;
}

struct SwapChain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    ovrTextureSwapChainDesc desc{};
    uint32_t index = 0;
    bool acquired = false;
    // afw_rebuild (AFW step 4b): the commit left the image acquired so ovr_SubmitFrame can rebuild the held eye in it;
    // cleared where that submit releases it. A use of the chain while this is set is counted ([AFWRB] early use).
    bool deferPending = false;
    // submit_drawn_image (U-52 / INV-429): while sdMode, up to TWO images are held, in acquire order - q[0] is the oldest,
    // the one the frame being replayed drew into (the renderer records one hand-out ahead of its replay) and the one the
    // next submit releases; `index` / `acquired` mirror q[0] / nq > 0 for every other reader. A hand-out acquires a new
    // image only when a commit happened since the newest acquire. sdMode is switched only at a submit (both directions).
    uint32_t  q[4] = {};
    int       nq = 0;
    bool      commitSinceAcquire = false;
    bool      sdMode = false;
    long long relSubmit = -1;   // the submit that last released on this chain (one release per submit; both eyes share it)
    // v3 (2026-10-05, after build 848769b4's block 2: XrResult -37 on every other frame): OpenXR - "Once a swapchain image has
    // been successfully waited on ... the app must release before waiting on the next acquired swapchain image"; VDXR refuses a
    // wait unless the OLDEST acquired image is unwaited, and an acquire when all images are acquired. So the second image is
    // ACQUIRED at the hand-out but WAITED only after the drawn one is released. qWaited[i] mirrors q[i]. sdFault = a runtime
    // call failed in this mode: the next submit leaves the mode (release everything) and it is not re-entered until re-ticked.
    bool      qWaited[4] = {};
    bool      sdFault = false;
};

struct MirrorTexture {
    ID3D11Texture2D* tex = nullptr;
    ovrMirrorTextureDesc desc{};
};

struct Session {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace appSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    // The runtime's floor (2026-11-16, trace 20261116_1545_standing_posture): a STAGE space, read only - appFloorY = the appSpace
    // origin's height above it, re-located every 30th LocateViews; floorOk false until it locates (or when the runtime has none).
    XrSpace stageSpace = XR_NULL_HANDLE;
    float appFloorY = 0.0f;
    bool floorOk = false;
    unsigned floorTick = 0;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;

    bool running = false;
    bool frameActive = false;
    bool everBegan = false;
    // Set once a worker has refreshed the pose in the current frame, so the
    // pose cannot move underneath the frame being built from it.
    bool locatedThisFrame = false;
    // Which of the game's two eye slots is the left eye. Latched once, because
    // it is a property of the engine and must not change as the player turns.
    // -1 = not yet decided, 0 = view i takes slot i, 1 = swapped.
    int  eyeAssign = -1;
    bool shouldQuit = false;
    bool shouldRecenter = false;
    bool posesValid = false;

    // Recentre absorb (D-253, recentre_logic.h): appSpace = the reference space of type appSpaceType turned by appBaseYawDeg
    // about +Y and moved to appBasePos. A runtime recentre queues `absorb`; BeginFrameIfNeeded applies it before the frame's
    // first locate - headPose then still holds the previous frame's head in the OLD appSpace (the fallback's "before").
    XrReferenceSpaceType appSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    float appBaseYawDeg = 0.0f;
    XrVector3f appBasePos{ 0.0f, 0.0f, 0.0f };
    ::dlvr::recentre::Pending absorb;
    int absorbCount = 0;
    ::dlvr::recentre::HeadRing headRing;   // v3: each frame's head (yaw + position in appSpace), for the look-back
    // Centre (D-284, centre_logic.h): PollEvents feeds the session states; BeginFrameIfNeeded moves appSpace sideways onto the head
    // once a centre is due. The two counters describe the wait of the centre now pending, for its DONE / GAVE UP line.
    ::dlvr::centre::Tracker centre;
    int centreUntracked = 0;
    int centreAbsorbWait = 0;
    // Drift bleed while walking (roomscale_bleed_mps, roomscale_logic.h BleedStep): its 5 s window, for the BLEED line.
    bool      bleedSaid = false;
    long long bleedWinStartNs = 0;
    int       bleedFrames = 0, bleedSkipCentre = 0, bleedSkipAbsorb = 0, bleedSkipNoHead = 0, bleedSkipMenu = 0, bleedFail = 0;
    double    bleedTotalM = 0.0;
    float     bleedOffsetM = -1.0f;   // the head's x/z distance from the origin at the last located walking frame (-1 = none yet)

    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    XrViewConfigurationView viewConfigs[2]{};

    // Identity, not zero. These are now held across a locate the runtime says
    // is invalid, so their starting value is what the game would be handed if
    // the very first locate came back invalid - and a zero-initialised
    // XrPosef has the quaternion (0,0,0,0), which is not a rotation at all.
    XrView views[2]{ { XR_TYPE_VIEW, nullptr, XrIdentity(), {} },
                     { XR_TYPE_VIEW, nullptr, XrIdentity(), {} } };
    XrPosef headPose = XrIdentity();

    // Frame-phase diagnostics.
    //
    // The frame is begun at the end of ovr_SubmitFrame, and the pose for it is
    // sampled at the engine's first tracking query afterwards. That ordering is
    // only true if the engine actually asks between our begin and its next
    // submit. If it asks BEFORE submitting - pipelining its camera update ahead
    // of the render - the query lands while the previous frame is still active
    // and is answered with the previous frame's pose, so the frame is drawn one
    // sample stale. Systematic staleness would be invisible; staleness that
    // comes and goes with load is a glitch. These count it instead of guessing.
    int      poseQueriesThisFrame = 0;
    unsigned staleFrames = 0;      // submitted without a fresh sample
    unsigned totalFrames = 0;
    XrTime   lastPredicted = 0;
    unsigned longGaps = 0;         // display time jumped more than one period
    double   beganAt = 0.0;        // NowSeconds() at xrBeginFrame
    double   renderMaxMs = 0.0;    // longest begin->end seen since the last report
    unsigned overBudget = 0;       // frames that took longer than a display period

    // Where the frame actually goes.
    //
    // The game's own work measures 1.4-6 ms against a 13.89 ms period, and we
    // still miss display periods - so the time is going somewhere else, and the
    // only blocking call in the path is xrWaitFrame. It is called at the tail of
    // ovr_SubmitFrame, which is not where LibOVR's contract expects an app to be
    // parked: ovr_SubmitFrame is supposed to return promptly and let the engine
    // pace itself. If both are pacing, they beat against each other.
    double   waitSumMs = 0.0;
    double   waitMaxMs = 0.0;
    unsigned waitCount = 0;
    double   lateSumMs = 0.0;      // submit time minus the intended display time
    double   lateMaxMs = 0.0;
    unsigned lateFrames = 0;       // submitted after the moment we declared

    // How long the engine spends parked on our frame lock, waiting to be told
    // where its head is. This is the cost we impose on the game directly, and
    // until it was measured it was invisible: it does not appear in the frame
    // time we attribute to the game, because that clock only starts once we let
    // it go.
    double   blockedSumMs = 0.0;
    double   blockedMaxMs = 0.0;
    unsigned blockedCount = 0;

    // Whose milliseconds are they?
    //
    // What was being reported as "game work" ran from our frame begin to a point
    // inside ovr_SubmitFrame, so it also contained everything WE do on the submit
    // path - overlay poll, the RTTI camera refresh, layer building - and none of
    // xrEndFrame. A 50 ms spike in that number could have been the engine or us,
    // and attributing it to the engine was a guess. These three split it.
    double   submitEnteredAt = 0.0;
    double   gameMaxMs = 0.0;      // frame begin -> the game calls submit
    double   ourMaxMs = 0.0;       // our submit-path work before xrEndFrame
    double   endMaxMs = 0.0;       // inside xrEndFrame
    // >>> AVERAGES BESIDE THE MAXIMA, ADDED 2026-09-21 TO SPLIT THE 8.98 ms GAP. <<<
    // [THREADS] measured an 11.56 ms frame as 2.39 ms of CPU submission and
    // **8.98 ms of gap** between the composite and the next frame's first
    // render-target bind (INV-151), and that gap is UNATTRIBUTED. These four
    // terms already exist as peaks - and INV-8 is explicit that a peak is not a
    // cost: the worst-frame line has read `engine 83.70 ms` on an 11.56 ms
    // frame, which is a loading hitch, not a budget. The SUM/COUNT pair is what
    // makes them comparable against the frame period.
    // Cost: three double adds per submitted frame. `waitSumMs`/`waitCount`
    // already existed, so xrWaitFrame was the only term already averaged.
    double   gameSumMs = 0.0;
    double   ourSumMs = 0.0;
    double   endSumMs = 0.0;
    unsigned splitCount = 0;       // frames contributing to all three sums
    unsigned endStalls = 0;        // xrEndFrame calls longer than one period

    // Does the pose the game draws with follow the pose we gave it?
    //
    // The reported symptom is that missing framerate makes poses jump rather
    // than merely stutter. A frame that is simply late still looks stable,
    // because the compositor reprojects it from the pose it was drawn with to
    // where the head is now - and it can only do that if the pose we declare is
    // the pose that was actually drawn. So the test is not how big any single
    // pose is, it is whether the game's frame-to-frame rotation matches ours.
    // Ours is smooth by construction: one locate per frame from the runtime. If
    // the game's steps are larger, something between our answer and its camera
    // is adding motion of its own.
    XrQuaternionf prevOurs{ 0, 0, 0, 1 };
    XrQuaternionf prevGame{ 0, 0, 0, 1 };
    bool     haveePrev = false;
    double   ourStepMaxDeg = 0.0;
    double   gameStepMaxDeg = 0.0;
    unsigned divergedFrames = 0;   // game step much bigger than ours

    // Which of our poses is the game's RenderPose actually equal to?
    //
    // The step sizes say the game's pose is ours delayed by a frame, and that
    // only matters because of what we do with it: we DECLARE RenderPose to the
    // compositor as the pose the image was drawn from. If the engine draws with
    // the fresh pose and reports the stale one, the compositor warps the image
    // by a whole frame of head motion - up to fourteen degrees at the step sizes
    // measured here - and does it only when frames are missed, because that is
    // when reprojection has any work to do.
    //
    // So compare the game's pose against both of ours and count which it
    // matches. This does not need a threshold or a guess; one of them is
    // essentially exact and the other is not.
    unsigned matchesCurrent = 0;
    unsigned matchesPrevious = 0;
    double   declaredErrMaxDeg = 0.0;   // how wrong the declared pose could be

    ID3D11Device* device = nullptr;
    LUID adapterLuid{};

    ovrTrackingOrigin trackingOrigin = ovrTrackingOrigin_EyeLevel;
    float ipd = 0.064f;


    std::vector<SwapChain*> chains;
    std::vector<MirrorTexture*> mirrors;
};

Session* g_session = nullptr;
bool g_initialized = false;
ovrErrorInfo g_lastError{};

// Render-target sizing. Defaults to the runtime's recommendation untouched.
// A previous build clamped this to 1600 on the theory that 3072x3264 was too
// much for Chrome Engine 6; the crash happened identically with and without the
// clamp, and Revive drives this game at high resolution, so the theory was
// wrong. Kept configurable for performance tuning, not correctness.
float g_renderScale = 1.0f;
int   g_maxDim = 0;   // 0 = no cap; pass the runtime's recommendation through
// The desktop window shows the right eye (deskmirror.cpp kMirrorView; the left eye until 2026-11-16). SHIPS ON since 2026-11-14 - Tier A, Jay: "Done. Ship it on" after RUN DM2
// (DM1 "That's worked"; Discord share "no issues"). Trace docs/traces/20261114_1115_mirror_ship_on.md.
bool  g_mirror = true;
// [DEP: vr_force_vsync_off] required-for=loading the game's VSync OFF in VR: patches the engine's own VSync=2 store (engine+0x2E63F2, inside the Oculus bring-up, INV-379) to 0 from ovr_Initialize, once at launch. Replaces the DXGI Present hook that crashed (INV-378, D-179). Jay 2026-10-03: "We NEED that to be off" / "it DOES AFFECT OTHER MACHINES"  tree=INERT. DEFAULT ON (Jay's requirement). Applies at launch only; the store only runs when the headset bring-up succeeds. WIDENED 2026-10-21 (Jay: "force the framerate cap to be off", "No new key"): the store is now a jmp to a stub that zeroes VSync, MaxFPS, HBAO+, DOF, PCSS and MotionBlur in the parsed video.scr values, and ovr_Create OK patches the engine frame limiter (engine+0x239E00) to ret - spec docs/superpowers/specs/2026-10-21-vr-video-force-design.md.
bool  g_vrForceVsyncOff = true;
// Stale default, corrected 2026-09-03 (see CLAUDE.md's "public test build round 2
// misalignment" section): this was left at `true` from an early, since-disproven
// theory that the game's eye halves always come out reversed. Once auto_eye_assign
// (below) was added and confirmed correctly latching the true left/right eye from
// the game's own reported head-space X, swap_eyes on top of it was measured as
// unneeded and the dev's own live, confirmed-working ovr2xr.txt has run with this
// at 0 ever since - but nobody updated this hardcoded fallback to match, so any
// FRESH install with no settings file yet (every public tester's first launch)
// silently got the double-flip: auto_eye_assign correctly picks the real left eye,
// then this unconditionally swaps it back to the wrong one. That is a textbook
// cause of stereo misalignment/double vision, and it is not headset-specific - it
// hits every fresh install, confirmed via a public tester's log showing this
// exact 1/1/1 stale-default combination on a from-scratch F:\ install.
bool  g_swapEyes = false;
// F2 LENS MASK: per GAME eye (the texture / viewport index the game submitted), refreshed at every submit: the image's declared
// tangents (positive: left, right, up, down), the runtime FOV of the view it is shown in (angles, radians, positive) and the pitch
// the declared pose carries. Written and read on the executor thread (submit and the bootstrap's D3D hooks, INV-152).
// game[4] / gameOk (INV-735, trace 20261113_2115): the frustum the GAME rendered this eye with (ovrLayerEyeFov, before any
// fov_center_correct recentre - that moves only the declared frustum, not the pixels) - the UI plane's place in the image.
struct LensMaskEye { bool ok; float img[4]; float rt[4]; float pitchDeg; float game[4]; bool gameOk; };
LensMaskEye g_lmEye[2] = {};
// F2 LENS MASK - WHICH EYE IS RENDERING NOW (fix 2026-10-16 after lensmask_run1: the bootstrap's live afwalt-stamp read put each image
// under the OTHER eye's mask - INV-377 had already found those reads out of step). Identity from the SUBMIT RECORD instead (INV-384 /
// INV-386): each submit's held eye comes from that image's own capture-paired record (CamHookAfwSubmitSkipEye); AFW skips eye N & 1 on
// frame N, so the eye the last submit HELD is the one rendering now. Answered only while the last two submits alternated. Self-check: the
// eye answered during a frame is compared at that frame's submit with its image's own fresh eye ([LENSMASKID], every 5 s while on).
int g_lmLastSkip = -2, g_lmPrevSkip = -2;
volatile LONG g_lmAnswer = -1;   // the last eye handed out since the previous submit, -1 = none
long long g_lmAgree = 0, g_lmDisagree = 0, g_lmNoAnswer = 0, g_lmNoRecord = 0;

// Diagnostic: submit only one view, so we can tell which PHYSICAL eye a given
// OpenXR view index lands in, independently of what the game drew. Swapping the
// sampled texture halves did not change the reported reversal, which means the
// fault is not where I assumed - this separates the two possibilities:
//   1 = submit only view 0 (expect: LEFT eye shows an image, right eye black)
//   2 = submit only view 1 (expect: RIGHT eye shows an image, left eye black)
// If the wrong physical eye lights up, the view-index mapping is inverted.
// If the right one does, the mapping is fine and the game's own stereo is
// reversed - which points at the HmdToEyeOffset sign in ovr_GetRenderDesc.
int   g_debugSingleEye = 0;

// LibOVR changed the meaning of this field between SDK generations, and Dying
// Light is 2015-era code:
//   SDK 0.x  ovrEyeRenderDesc::HmdToEyeViewOffset - a translation to APPLY to
//            the view matrix, i.e. the negation of the eye position.
//   SDK 1.x  ovrEyeRenderDesc::HmdToEyeOffset     - the eye position itself
//            (left eye = negative X).
// We report 1.x semantics. If the engine consumes it with 0.x semantics it
// renders each eye from the other eye's position, which reverses the stereo -
// and no amount of swapping which texture half we sample can fix that, because
// both halves are already wrong. Flipping the sign is the actual fix.
bool  g_invertEyeOffset = false;  // the game wants plain 1.x semantics

// The game reports, per eye, the pose it actually rendered from (ovrLayerEyeFov
// ::RenderPose). This comment's own reasoning (believe RenderPose, since it is
// the only authoritative statement of what the game drew) turned out to be built
// on a premise that does not hold on the build this project actually ships
// against: RenderPose is permanently zero there (the game never calls
// ovr_GetTrackingState/ovr_GetPredictedDisplayTime at all - see CLAUDE.md's
// translation-layer section), so "believing" it means declaring every eye's pose
// as the origin, not a harmless no-op. Confirmed-correct value, and what the
// dev's own live ovr2xr.txt has actually run with since the real stereo-alignment
// fix landed 2026-09-01, is 0 (use our own xrLocateViews measurement instead).
// This hardcoded fallback was never updated to match - stale, corrected
// 2026-09-03, same regression class as swap_eyes just above.
bool  g_useRenderPose = false;

// Measured: the game consumes HmdToEyeOffset directly as an eye POSITION, and
// echoes it back in RenderPose. With invert_eye_offset=1 we told it eye 0 sat at
// x=+0.032 and it dutifully rendered eye 0 from the right - the reversal.
//
// Rather than depend on getting that sign right, assign images to eyes from what
// the game actually rendered: whichever eye index has the smaller (more
// negative) X becomes OpenXR view 0, the left eye. That is correct whatever
// convention the engine uses, and self-corrects if a future patch changes it.
bool  g_autoEyeAssign = true;

// Investigation, 2026-09-04: purely observational, logs the angular
// difference between the two eyes' final poses declared to OpenXR every ~2s
// - see the comment at its call site (the eye-submission loop) for the
// reasoning. Off by default.
#if defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md) - no header extern, so constexpr here
constexpr bool g_diagEyePoseCompare = false;
#else
bool  g_diagEyePoseCompare = false;
#endif

// [DEP: diag_fake_view_cant_deg] required-for=testing the canted-view conversion (cantview_logic.h, LocateViews) on a headset
//      whose views are parallel (g_fakeViewCantDeg; trace docs/traces/20261118_1150_canted_views.md). +N turns the two located
//      views outward N deg each (left eye +Y) with their FOV re-expressed - a fake canted runtime - and the conversion then runs
//      on it; -N fakes the same and BYPASSES the conversion (the pre-fix declaration: the double image). tree=PROBE-ONLY (a
//      debug fake; 0 = nothing touched). HEADSET (the image in the lenses). VDXR / SteamVR honour a declared FOV; Meta Link
//      does not (D-165) - the fake is not valid there. DEFAULT 0 - never shipped on. OUTSIDE every DLVR_BETA_LEAN block.
float g_fakeViewCantDeg = 0.0f;

// The game renders a SYMMETRIC frustum, because ovr_GetHmdDesc has to answer
// before any view exists and can only offer a symmetric fallback. Real headset
// FOVs are asymmetric - on Quest 3 the optical centre sits below and outward of
// the centre of a symmetric image - so declaring the game's symmetric frustum
// verbatim leaves the picture sitting high.
//
// The runtime tells us its true asymmetry in XrFovf, so we keep the game's
// angular EXTENT (that is what it actually drew) but re-centre it on the
// runtime's optical centre. Purely a submit-side correction; the game is
// untouched.
bool  g_fovCenterCorrect = false;  // declaring != rendered warps the image

// How much of the runtime's real FOV asymmetry we pass on to the game.
//   0 = none, hand over the true asymmetric frustum
//   1 = symmetrise VERTICALLY only (default)
//   2 = symmetrise both axes
//
// Chrome Engine 6 is from 2015 and may well assume a symmetric frustum when it
// builds its projection; feeding it asymmetric values then looks subtly wrong
// rather than failing outright. Symmetrising costs pixels - we take the LARGER
// half-angle so no field of view is lost, and render some that sits outside the
// lens - but it keeps the game's maths on ground it was written for, and
// fov_center_correct puts the image back in the right place at submit time.
// COMPILED 0 since 2026-11-24 (Jay: ships 0 in 1.07 - "That was the reason I wanted you to make sure it would work regardless of
// runtime, headset, etc."; trace docs/traces/20261124_2030_symfov0_default.md): every off-centre path reads each eye's matrix (STATE
// (170)-(194)). The text above is the original rationale for 2. DEV stays 2 (it has none of the off-centre work).
int   g_symmetricFov = 0;

// [LOADBEARING: FOV-META-OWN-FOV] Meta's PC OpenXR runtime (Quest Link / Air Link) shows each eye's image over ITS OWN FOV,
//      whatever fov the projection view declares (Venture PR #79 on "Oculus" 1.208.0), so the symmetric superset lands squeezed
//      and off-centre = the cross-eye + jitter a tester reported at symmetric_fov=2, both gone at 0 (Jay 2026-11-16: "A"; trace
//      docs/traces/20261116_1835_meta_runtime_symmetric_fov.md). Set once in ovr_Create from the runtime's name. Since 2026-11-24 (Jay:
//      "Go ahead with 1"; trace 20261124_1550_meta_panel_crop.md) it no longer forces symmetric_fov to 0: ovr_SubmitFrame cuts each view
//      to that runtime's own FOV window (SteamVrPanelCrop, ownFov), so the game renders what it renders on VDXR and AFW can run.
//      Removing the cut there puts the cross-eye back.
bool  g_runtimeStretchesFov = false;
// The runtime's name starts "SteamVR/OpenXR" (menuchord::RuntimeIsSteamVr) - set once in ovr_Create, before the first pad poll.
// SteamVR takes the left Touch menu button (Jay 2026-11-18: "SteamVR eats the input"), so there the X + Y chord is the menu
// button (dlvr_FilterScePad's face-button block). VDXR / WiVRn / Meta's runtime: false, X / Y unchanged.
bool  g_runtimeIsSteamVr = false;

int SymmetricFovMode()
{
    return g_symmetricFov;   // the setting on every runtime (Meta's too since 2026-11-24 - see g_runtimeStretchesFov)
}

// Where the FOV we hand the game comes from.
//   0 = the symmetric fallback only (tan(0.8) both axes)
//   1 = the real per-eye FOV measured from the runtime
// Handing over the true mirrored/asymmetric frustum made everything warp, which
// is consistent with the engine assuming a symmetric projection - so this is
// meant to be used together with symmetric_fov=2, which turns the real FOV into
// the smallest SYMMETRIC frustum that still contains it.
int   g_fovSource = 1;

// Reconcile the frustum's aspect with the eye texture's, by growing the FRUSTUM
// to fit the texture rather than growing the texture to fit the frustum.
//
// The texture and the frustum have to agree on aspect ratio, or an engine that
// derives one axis of its projection from the other through the viewport aspect
// - an ordinary shortcut, and just what a symmetric-only engine tends to do -
// renders a frustum we never asked for.
//
// An earlier version fixed that by sizing the texture from the frustum at the
// runtime's pixel density. That is defensible in isolation and wrong in
// practice: it silently inflated a requested 2688x2880 to 3336x3464, so the
// resolution the user set in their runtime was not the resolution the game
// rendered. The size you ask for is the size you get; the frustum is the part
// with slack in it.
//
// Growing the frustum keeps it a superset of the lens (we only ever widen), and
// costs a few percent of field rather than 49% of the pixels.
bool  g_fovMatchAspect = true;

// When a worker asks for the head pose, refresh it with xrLocateViews.
//
//   0 = never; the pose is whatever was located at the frame boundary
//   1 = once per frame, on the FIRST query of that frame (default)
//   2 = on every query
//
// 2 was the first attempt and it is wrong in a way that matters here: with
// sixteen workers querying repeatedly within one frame, every call returned a
// different pose, so the engine could build its view matrix from one sample and
// something else from another. A pose must be sampled once per frame and then
// held.
//
// 1 was the next attempt, and this comment used to claim it was the fix - it
// is not. 1 still locates TWICE per frame (once at the frame boundary, again at
// the first worker query), two different wall-clock moments for the same
// predicted display time; the runtime answers those differently, so which pose
// the engine used came down to a per-frame coin toss of a fraction of a degree -
// exactly what jitter is made of. The actual fix, confirmed and running live on
// the dev's own working install since 2026-09-01 (see CLAUDE.md's ovr2xr.txt
// comment for `relocate_on_query`), is 0: sample once at the frame boundary and
// never refresh on query, trading a little latency for a fixed sample point.
// This hardcoded fallback was left at the disproven value 1 - stale, corrected
// 2026-09-03, same regression class as swap_eyes/use_render_pose above.
int   g_relocateOnQuery = 0;

// Which space ovrEyeRenderDesc::HmdToEyeOffset is expressed in.
//
//   0 = the tracking-space head->eye delta as it stands at the moment of the
//       call - what we used to send, and what was wrong.
//   1 = the constant HEAD-LOCAL offset, (+/-ipd/2, 0, 0). This is what LibOVR
//       actually specifies, and it is what this engine wants.
//
// The deciding observation was the main menu, which is locked to the view and
// so should look identical at every head angle - and instead the eye separation
// drifted progressively as the player turned. That rules out the offset being
// consumed unrotated, because an unrotated offset produces no yaw dependence at
// all. The engine rotates it, as a LibOVR client is supposed to.
//
// So we were handing it a vector that was ALREADY rotated into tracking space,
// and it rotated it again. The eye ends up at head + R*R*e instead of head +
// R*e: the baseline keeps its length but swings to twice the head angle, so the
// component that actually separates the eyes falls off as cos(yaw) - the
// separation appears to shrink the further you turn, which is exactly what was
// reported. At 90 degrees it would collapse entirely.
//
// The constant head-local offset has no orientation baked into it, so there is
// nothing to rotate twice.
int   g_eyeOffsetSpace = 1;

// Extend the vertical half-angles (symmetrically, using the larger of the two
// the runtime reports) so the render actually covers the lens below the forward
// axis. This is the whole cause of the "centre sits high" symptom: the fallback
// tan(0.8)=1.030 is smaller than the 1.428 the Quest 3 needs downward, so the
// bottom of the field was never drawn. Costs pixels above, changes nothing
// horizontally, and keeps the frustum symmetric.
bool  g_verticalExtend = false;  // never touch the FOV the game renders with

// Vertical placement, done WITHOUT touching the frustum.
//
// The FOV we hand the game must equal the FOV we declare at submit, or the
// runtime resamples with the wrong mapping and everything warps. So the picture
// is moved by rotating the pose we declare instead: same frustum shape, same
// pixels, just placed at a different angle. No stretching of any kind.
//
// auto_pitch derives the angle from the runtime's own vertical FOV: the
// angular bisector (angleUp + angleDown) / 2, which is -5.5 degrees on a
// Quest 3 (+44.0 up, -55.0 down).
// pitch_offset_deg overrides it; negative moves the picture DOWN.
//
// This is NOT a workaround for a frustum that was too small - that theory was
// wrong. It survived widening the frustum to a superset that fully covers the
// lens, and it is still needed, by the same 5.5 degrees, which rules out
// coverage as the cause.
//
// What it actually corrects is the runtime's declared view axis. VDXR reports
// the Quest 3 frustum as +44 up / -55 down about the view pose's forward, which
// puts the optical centre of the panel at the bisector, 5.5 degrees BELOW that
// forward. Content the game draws down its camera axis therefore lands 5.5
// degrees above where the wearer is actually looking. Rotating the declared
// pose by the bisector puts it back on the optical axis.
//
// The same figure was arrived at independently on another Quest 3 mod, so treat
// it as a property of this headset and runtime, not of this code.
// SHIPPED OFF (2026-11-17, Jay: "Verified for now. As well as the auto_pitch=0"; trace 20261117_1545): rotating ONLY the declared pose
// rolled the horizon with head yaw (INV-664; testers' 1.02 wobble). Reference mods submit the located pose unrotated.
bool  g_autoPitch = false;
float g_pitchOffsetDeg = 0.0f;   // 0 with auto_pitch=1 means "use the computed value"

// Rotates a pose about its own X axis. Positive tilts the declared view up,
// which moves the visible picture down.
XrPosef PitchPose(const XrPosef& p, float degrees)
{
    if (degrees == 0.0f) {
        return p;
    }
    const float half = degrees * 3.14159265358979f / 360.0f;
    const float sx = sinf(half), cx = cosf(half);
    const XrQuaternionf& q = p.orientation;

    XrPosef out = p;
    out.orientation.w = q.w * cx - q.x * sx;
    out.orientation.x = q.w * sx + q.x * cx;
    out.orientation.y = q.y * cx + q.z * sx;
    out.orientation.z = q.z * cx - q.y * sx;
    return out;
}

dlvr::eyefov::Tans ToTans(const ovrFovPort& f)
{
    return dlvr::eyefov::Tans{ f.LeftTan, f.RightTan, f.UpTan, f.DownTan };
}

ovrFovPort FromTans(const dlvr::eyefov::Tans& t)
{
    ovrFovPort f;
    f.LeftTan = t.l;
    f.RightTan = t.r;
    f.UpTan = t.u;
    f.DownTan = t.d;
    return f;
}
bool  g_configLoaded = false;

// Builds "<game dir>\dlvr\<leaf>". Everything we read or write lives there.
bool LayerFilePath(const char* leaf, char* out, size_t cch)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LayerFilePath), &self);
    DWORD n = GetModuleFileNameA(self, out, static_cast<DWORD>(cch));
    if (n == 0 || n >= cch) {
        return false;
    }
    char* slash = strrchr(out, '\\');
    if (!slash) {
        return false;
    }
    slash[1] = '\0';
    strcat_s(out, cch, "dlvr\\");
    // Nothing else in the mod ever creates this folder - not the bootstrap, not
    // any other reader/writer in here, all of which just fopen a leaf inside it
    // and silently degrade (fall back to defaults, skip a cache write) if that
    // fails. On a genuinely fresh install that has never had this folder before
    // (confirmed on a real public tester's log, 2026-09-03 - "could not write
    // ...\ovr2xr.txt.tmp"), that means the very first settings write can never
    // succeed, so a corrected set of defaults could never even get persisted to
    // fix itself on a later launch. CreateDirectoryA is idempotent - harmless
    // and cheap to call on every resolve, not just the first.
    CreateDirectoryA(out, nullptr);
    strcat_s(out, cch, leaf);
    return true;
}

// ------------------------------------------------------- HMD capability cache
//
// ovr_GetHmdDesc has to answer before any XrSession exists, so on a first run we
// can only offer a symmetric fallback FOV - and the game renders a frustum that
// does not match the lenses, which is why the picture needs re-centring and why
// pixels are spent outside the visible field.
//
// Once a session is up we know the real per-eye FOV and IPD, so persist them and
// use them from the next launch onward. The game then renders the correct
// asymmetric frustum natively.

struct HmdCache {
    bool valid = false;
    ovrFovPort fov[2]{};
    float ipd = 0.064f;
};
HmdCache g_hmdCache;
bool g_hmdCacheSaved = false;

// The runtime's system name, kept from ovr_Create's xrGetSystemProperties. With no hmd_cache.txt it picks the
// first-launch FOV row (fovtable_logic.h); empty = not read, which takes the wide row.
char g_xrSystemName[XR_MAX_SYSTEM_NAME_SIZE] = {};

void LoadHmdCache()
{
    char path[MAX_PATH * 2];
    if (!LayerFilePath("hmd_cache.txt", path, sizeof(path))) {
        return;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        LOG("hmd cache: none yet (%s) - first run uses a symmetric fallback FOV", path);
        return;
    }
    int got = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            continue;
        }
        int eye = 0;
        float l = 0.0f, r = 0.0f, u = 0.0f, d = 0.0f, v = 0.0f;
        if (sscanf_s(line, "fov%d=%f,%f,%f,%f", &eye, &l, &r, &u, &d) == 5 &&
            eye >= 0 && eye <= 1) {
            g_hmdCache.fov[eye].LeftTan = l;
            g_hmdCache.fov[eye].RightTan = r;
            g_hmdCache.fov[eye].UpTan = u;
            g_hmdCache.fov[eye].DownTan = d;
            ++got;
        } else if (sscanf_s(line, "ipd=%f", &v) == 1 && v > 0.03f && v < 0.10f) {
            g_hmdCache.ipd = v;
        }
    }
    fclose(f);
    g_hmdCache.valid = (got == 2);
    if (g_hmdCache.valid) {
        LOG("hmd cache: loaded real FOV  L=(%.3f %.3f %.3f %.3f) R=(%.3f %.3f %.3f %.3f) ipd=%.4f",
            g_hmdCache.fov[0].LeftTan, g_hmdCache.fov[0].RightTan,
            g_hmdCache.fov[0].UpTan, g_hmdCache.fov[0].DownTan,
            g_hmdCache.fov[1].LeftTan, g_hmdCache.fov[1].RightTan,
            g_hmdCache.fov[1].UpTan, g_hmdCache.fov[1].DownTan, g_hmdCache.ipd);
    } else {
        LOG("hmd cache: file present but incomplete - ignoring");
    }
}

void SaveHmdCache(Session* s)
{
    if (g_hmdCacheSaved || !s || !s->posesValid) {
        return;
    }
    g_hmdCacheSaved = true;

    char path[MAX_PATH * 2];
    if (!LayerFilePath("hmd_cache.txt", path, sizeof(path))) {
        return;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        LOG("hmd cache: could not write %s", path);
        return;
    }
    const ovrFovPort l = ToOvrFov(s->views[0].fov);
    const ovrFovPort r = ToOvrFov(s->views[1].fov);
    fprintf(f, "# Written by dlvr ovr2xr from the live OpenXR runtime.\n");
    fprintf(f, "# Per-eye FOV as LibOVR tangents: left,right,up,down.\n");
    fprintf(f, "#\n");
    fprintf(f, "# Rewritten every run, as soon as the runtime reports real view\n");
    fprintf(f, "# poses. There is nothing to delete: change headset or runtime and\n");
    fprintf(f, "# the next launch measures it again and overwrites this. It exists\n");
    fprintf(f, "# only because ovr_GetHmdDesc has to answer before any view has\n");
    fprintf(f, "# been located, so the FIRST launch on a new headset uses the\n");
    fprintf(f, "# previous numbers and the second onwards are correct.\n");
    fprintf(f, "fov0=%.6f,%.6f,%.6f,%.6f\n", l.LeftTan, l.RightTan, l.UpTan, l.DownTan);
    fprintf(f, "fov1=%.6f,%.6f,%.6f,%.6f\n", r.LeftTan, r.RightTan, r.UpTan, r.DownTan);
    fprintf(f, "ipd=%.6f\n", s->ipd);
    fclose(f);
    LOG("hmd cache: measured and rewritten (%s) - fov0 %.3f/%.3f/%.3f/%.3f ipd %.4f",
        path, l.LeftTan, l.RightTan, l.UpTan, l.DownTan, s->ipd);
}

// Overlay bindings and appearance. Kept with the rest of the settings so the
// overlay can rebind itself and have the change survive a restart.
int   g_overlayKey = 0x2D;      // VK_INSERT
int   g_overlayPad = 0x00C0;    // XINPUT L3 | R3
float g_overlayDistance = 1.4f;
float g_overlayWidth = 1.0f;
bool  g_overlayEnabled = true;
// [DEP: overlay_text_size] required-for=the mod menu's text size, percent of the built-in size (g_overlayTextSize; overlay.cpp
//      DrawUi PushFont). Jay 2026-11-12, the BioShock-style menu: "Yes, add it" - a slider in the menu's bottom strip.
//      tree=INERT (menu text only; nothing in the rig reads it). DEFAULT 100 = the size the menu had before (1.6x).
int   g_overlayTextSize = 100;
// [DEP: overlay_advanced] required-for=the mod menu's Basic / Advanced button (g_overlayAdvanced; overlay.cpp DrawButtonRow).
//      Jay 2026-11-12: "Basic / Advanced" in the button row. No row is marked advanced yet, so Basic hides nothing until Jay
//      names them. tree=INERT. DEFAULT 0 (Basic).
bool  g_overlayAdvanced = false;
bool  g_horizonLock = true;   // DEFAULT ON 2026-10-01 (Jay: Tier B ships on, docs/SHIP_DEFAULTS.md)
// Mirrors CamHookOp, which is an enum class. Defaults to Level, not Observe:
// Observe is a diagnostic that writes nothing, and defaulting to it meant the
// horizon lock could be switched on, the hook installed and the cameras picked
// correctly - and still nothing happened, with no error anywhere to say why.
int   g_camHookOp = 1;
bool  g_camHookOpDirty = false;
int   g_camUpAxis = 0;
bool  g_blockPadPitch = true;   // DEFAULT ON 2026-10-01 (Tier B)
bool  g_vrTriggerFire = true;
// [DEP: melee_r2_suppress] required-for=blocking the game's R2 attack while a melee weapon is held (g_meleeR2Suppress) -
//      D-62's last step, Jay 2026-09-30. tree=INERT. Was DEFAULT OFF. Off: MeleeR2Filter is never called and the R2 byte is
//      exactly what vr_trigger_fire left. Needs the [ITEM] capture (RightHandItemKind), i.e. the rig's master enables;
//      without it the hand reads NOT KNOWN and R2 is PASSED (fails open, logged by name).
bool  g_meleeR2Suppress = true;   // DEFAULT ON 2026-10-01 (Tier B)
// See dlvr_FilterSdlEvent's own comment - zeroes the engine's own mouse-look
// pitch input at the SDL event level, same reasoning as block_pad_pitch but
// for the mouse instead of the gamepad's right stick. Defaults off, unlike
// block_pad_pitch/vr_trigger_fire - the SDL_MOUSEMOTION struct layout this
// relies on is inferred from public SDL2 headers, not independently
// confirmed against this build the way ScePadData's layout was, so this
// stays opt-in until tested.
bool  g_blockMousePitch = true;   // DEFAULT ON 2026-10-01 (Tier B - Jay's tested state)
// See dlvr_FilterScePad's own comment - real feature now, confirmed
// 2026-09-02 (left thumbstick click is genuinely Dying Light's sprint
// binding). Superseded the earlier always-on "diag_force_l3" diagnostic.
// vr_* inputs below: DEFAULT ON 2026-10-01 (Jay: Tier B ships on - "Our values, our current state, is the verification").
bool  g_vrSprint = true;
bool  g_vrMove = true;
// Camera/body decoupling: counter-rotate injected movement by head yaw so
// looking around never curves where the character travels. See the block in
// dlvr_FilterScePad for the full derivation.
bool  g_vrDecoupleMove = true;
// Max counter-rotation in degrees. A compromise, not a preference: the game
// drops sprint when the stick stops reading near-forward. See CLAUDE.md.
float g_vrDecoupleMaxDeg = 30.0f;
// Auto-install the aim hook in OBSERVE-ONLY mode (AimHookOverride stays off),
// so the game's own unmodified ComputeShootDir output is logged without
// needing the overlay mid-test. For the grapple-hook aim investigation.
bool  g_aimHookAutoObserve = false;
// The real thing: install the aim hook with AimHookOverride ON, self-renewing
// the same way the observe-only path does. A settings-file switch rather than
// an overlay-only one because every in-headset test is run with the overlay
// closed, and the overlay is the only other way to reach AimHookOverride.
bool  g_vrAimRedirect = true;
bool  g_vrTurn = true;
// 1:1 physical parity with a real gamepad's face buttons - see
// dlvr_FilterScePad for the Cross/Circle/Square/Triangle bit mapping. Same
// "confirmed by genre convention, not yet independently verified in-game"
// status as vr_sprint/vr_move/vr_turn were before their own first test -
// defaulted off until confirmed; compiled ON now.
bool  g_vrFaceButtons = true;
// The rest of the physical inputs left unbound as of 2026-09-03 (R3, left
// trigger, both grips, left menu button) - see dlvr_FilterScePad for the bit
// mapping. Same "genre-convention bet, confirm live" lifecycle as every
// other input feature above - (compiled ON now) defaulted off until each is individually
// confirmed against the game's real default control scheme.
bool  g_vrStickClickRight = true;    // R3
bool  g_vrLeftTrigger = true;        // left trigger -> L2
bool  g_vrGrip = true;               // both grips -> L1/R1
bool  g_vrMenuButton = true;         // left menu button -> SELECT (touchpad), 0.5 s hold -> START (OPTIONS)
// [DEP: vr_left_trigger_deadzone] required-for=the left trigger's rest noise not reaching L2 (g_vrLeftTriggerDeadzone):
//      pulls at or below it synthesise 0, above it rescale to 0..255 so a full pull still reads 255. The 2026-10-08
//      Index tester's left trigger read 0.00-0.05 for the WHOLE session (2,873 vr_left_trigger lines, L2 1-11 / 255;
//      backups/20261014_index_tester_oldbuild_log) - what the game does with that is [UNKNOWN]. Every profile, Touch
//      included. 0 = the old behaviour bit for bit. tree=LOADBEARING while vr_left_trigger=1 (it is the L2 byte).
//      DEFAULT 0.08 (2026-10-14, above the 0.05 measured; was Tier C until Jay called it; Tier A since 2026-10-17).
float g_vrLeftTriggerDeadzone = 0.08f;

// The head orientation as last handed to the game, correction included. The
// camera scanner subtracts it from the engine's composed camera to recover the
// game's own orientation, so it has to be what the game actually received, not
// what the runtime reported.
// Set while one thread is inside xrWaitFrame with the lock released.
volatile LONG g_inWait = 0;

XrQuaternionf g_lastReportedHead{ 0.0f, 0.0f, 0.0f, 1.0f };
// The head POSITION of that same report (neck pivot applied when on). Written at the SAME point as
// g_lastReportedHead; [AFWCAM] stores it in the camera capture (PLAN-AFW-WARP-DATA step 2).
XrVector3f g_lastReportedHeadPos{ 0.0f, 0.0f, 0.0f };
// SPEC-HEAD-YAW-DECOUPLE C1: the yaw-only rotation stripped from that same report (identity when
// the decouple is not active). Written at the SAME point as g_lastReportedHead, so camhook's C2
// pairs this frame's yaw with this frame's pitch/roll.
XrQuaternionf g_lastStrippedHeadYaw{ 0.0f, 0.0f, 0.0f, 1.0f };
// The decouple, latched once at the first ovr_GetTrackingState (headpose.h says why):
// -1 = not yet decided, 0 = off / refused, 1 = active.
volatile LONG g_headYawDecoupleState = -1;
bool          g_headYawDecoupleFlipLatched = false;

// ===== HEAD NECK PIVOT (docs/SPEC-HEAD-NECK-PIVOT-2026-10-01.md, INV-320/321) =====
// The engine places HSPINE1 - parent of the clavicles AND the eyecamera - at the head POSITION we report (C08100,
// INV-306), and that position is the EYE MIDPOINT, which swings on an arc around the neck whenever the head ROTATES.
// So a pure head turn slides the shoulders (INV-320). With this on, the reported position has the rotation arc removed:
//     p_report = p_eye - (R_head - I) * n
// n = eye midpoint - neck pivot in the HEAD frame (OpenXR view axes), measured for Jay by [HEADTURN R] PIVOT (INV-321).
// At the reference pose (LOCAL forward, level) p_report == p_eye, so neutral height/position are unchanged.
// [DEP: head_neck_pivot] required-for=SPEC-HEAD-NECK-PIVOT (the shoulder slide on head turns, INV-320)
//      tree=INERT. Was DEFAULT OFF. Read LIVE on every call (overlay flips it mid-session for the A/B). When ON it is
//      LOADBEARING for the BODY and the VIEW position: ovr_GetTrackingState reports p_report, and every consumer that
//      adds R*(ctl - head) to an ENGINE anchor must use NeckPivotReportedPos() (boneblend SolveArmIk +
//      ProbePositionMapping, grappleaim MaybeLogWindow). Raw position while poses are invalid or |n| > 300 mm (refused,
//      logged). HEADSET only (tracked head).
//      DEFAULT ON since 2026-10-02 (Jay, INV-348: "the values I set are pretty good, so I'd say those are the shipped ones").
bool  HeadNeckPivot = true;
// [DEP: head_neck_pivot_x_mm] required-for=n.x of head_neck_pivot (INV-321: +8.2 mm, Jay's headset, 23 turn windows)
//      tree=INERT. Only read while head_neck_pivot is on.
float HeadNeckPivotXmm = 8.0f;
// [DEP: head_neck_pivot_y_mm] required-for=n.y of head_neck_pivot (INV-321 addendum: +111.6 mm, 12 nod windows)
//      tree=INERT. Only read while head_neck_pivot is on.
float HeadNeckPivotYmm = 112.0f;
// [DEP: head_neck_pivot_z_mm] required-for=n.z of head_neck_pivot (INV-321: -83.5 turns / -96.2 nods; turns used)
//      tree=INERT. Only read while head_neck_pivot is on.
float HeadNeckPivotZmm = -84.0f;

// [DEP: roomscale_bleed_mps] required-for=the roomscale-off drift bleed (RoomscaleBleedMps): while the left stick moves the character
//      (|stick| > 0.2), the view is moved back over the character by up to this many metres a second when real steps have taken the
//      head off it (spec 2026-10-26-roomscale-physical-crouch-design §2.2, Jay: "That's correct"; plan Task 6). Position only, x/z
//      only; standing still and leaning are never touched. 0 = off. tree=LOADBEARING while > 0 (it recreates appSpace, as the centre
//      does). HEADSET only (a tracked head + the left stick). Was Tier C - ask Jay at ship time; Tier A since 2026-10-28 (Jay verified, run B1). Compiled 0.3.
float RoomscaleBleedMps = 0.3f;

// [NECKPIVOT] accounting - ovr_GetTrackingState runs under the frame lock, so plain counters.
struct NeckPivotStats {
    bool       armedSaid;
    int        lastOn;          // -1 = not yet seen
    long long  calls, applied, offCalls, invalid, refused;
    double     sumMm;
    float      maxMm;
    ULONGLONG  winStartMs;
};
NeckPivotStats g_np = { false, -1, 0, 0, 0, 0, 0, 0.0, 0.0f, 0 };

// The ONE formula, shared by the report and every consumer. Returns false (out = raw) when off / refused.
bool NeckPivotCompute(const XrPosef& raw, XrVector3f& out, bool* refused)
{
    out = raw.position;
    if (refused) { *refused = false; }
    if (!HeadNeckPivot) { return false; }
    const float nx = HeadNeckPivotXmm * 0.001f, ny = HeadNeckPivotYmm * 0.001f, nz = HeadNeckPivotZmm * 0.001f;
    if (nx * nx + ny * ny + nz * nz > 0.3f * 0.3f) {
        if (refused) { *refused = true; }
        return false;
    }
    // (R - B) n. B = identity with the decouple off (INV-322: the engine's eye lever re-adds the whole arc from the head it is
    // given). B = the head's own yaw with the decouple ON (2026-10-20): the engine is given a YAW-FREE head, so its lever never
    // re-adds the yaw arc - removing it put the reported head 2 |n_xz| sin(yaw/2) off (~17 cm facing the playspace's back; Jay
    // after a 180 + recentre: "essentially my forearms only"). Same yaw extraction as the decouple's strip (CamYawOnly).
    // COST: one yaw extraction + two 3x3 builds per call.
    const float q[4] = { raw.orientation.x, raw.orientation.y, raw.orientation.z, raw.orientation.w };
    float base[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (HeadYawDecoupleActive()) {
        CamYawOnly(q, 0, base);   // OpenXR is Y-up: yaw about +Y
    }
    const float n[3] = { nx, ny, nz };
    float d[3];
    ::dlvr::neckpivot::ArcOffset(q, base, n, d);
    out.x = raw.position.x - d[0];
    out.y = raw.position.y - d[1];
    out.z = raw.position.z - d[2];
    return true;
}

// INV-740 (trace docs/traces/20261116_1101_roomscale_neck_point_dead_zone.md): ROOMSCALE's input. NeckPivotCompute keeps the eyes'
// YAW arc while the decouple is on (base = head yaw, above), so every head turn moved roomscale's offset and walked the character
// (~0.0844 m x the turn rate, applied in one physics step; > 0.1 m/s starts the game's walk bob). This removes the WHOLE arc - same gates
// as NeckPivotCompute (head_neck_pivot on, |n| <= 300 mm; otherwise raw). Roomscale only; every other neck-pivot consumer is unchanged.
// COST: one quaternion -> matrix and nine multiplies, once per frame (ApplyRoomscale).
static bool NeckPivotNeckPoint(const XrPosef& raw, XrVector3f& out)
{
    out = raw.position;
    if (!HeadNeckPivot) { return false; }
    const float nx = HeadNeckPivotXmm * 0.001f, ny = HeadNeckPivotYmm * 0.001f, nz = HeadNeckPivotZmm * 0.001f;
    if (nx * nx + ny * ny + nz * nz > 0.3f * 0.3f) { return false; }
    const float q[4] = { raw.orientation.x, raw.orientation.y, raw.orientation.z, raw.orientation.w };
    const float eye[3] = { raw.position.x, raw.position.y, raw.position.z };
    const float n[3] = { nx, ny, nz };
    float p[3];
    ::dlvr::neckpivot::FullArcPoint(q, eye, n, p);
    out.x = p[0];
    out.y = p[1];
    out.z = p[2];
    return true;
}

// Called from ovr_GetTrackingState, under the frame lock, before the first report. Decides once and
// says so - including the refusal, by name (CLAUDE.md: a feature that silently does nothing is the
// worst outcome here).
void LatchHeadYawDecouple()
{
    if (g_headYawDecoupleState >= 0) {
        return;
    }
    g_headYawDecoupleFlipLatched = HeadYawDecoupleFlip;
    if (!HeadYawDecouple) {
        InterlockedExchange(&g_headYawDecoupleState, 0);
        LOG("[HEADYAW] decouple OFF (head_yaw_decouple=0) - the engine gets the real head yaw, as before");
        return;
    }
    if (g_useRenderPose) {
        InterlockedExchange(&g_headYawDecoupleState, 0);
        LOG("[HEADYAW] *** decouple REFUSED - use_render_pose=1 declares the ENGINE's render pose to "
            "OpenXR, which would be the yaw-free one, so the compositor would rotate the image by the "
            "missing yaw. Set use_render_pose=0 to use the decouple. Running with the real head. ***");
        return;
    }
    InterlockedExchange(&g_headYawDecoupleState, 1);
    LOG("[HEADYAW] decouple ACTIVE (latched at startup; overlay changes need a restart) flip=%d - the "
        "engine gets a yaw-free head, camhook re-adds the real yaw to the view (C1/C2)",
        g_headYawDecoupleFlipLatched ? 1 : 0);
}

// Whether the engine has asked us for this frame's head yet.
//
// The camera hook re-applies the headset's pitch and roll from the head we last
// reported. If the engine builds its camera before it asks for the pose, that
// head belongs to the previous frame - so the image is drawn with one head
// orientation while the pose we declare to the compositor carries another. The
// compositor then reprojects between two poses that disagree, which is a
// rotational step in an otherwise smooth sequence.
//
// Whether it happens is a matter of job ordering, so it comes and goes, and it
// gets worse when frame timing is irregular. Exactly the reported symptom - so
// it is counted rather than assumed.
volatile LONG g_headSampled = 0;

const char* const kOffOn[]       = { "off", "on" };
const char* const kWristPreset[] = { "A", "B (split)", "Custom" };   // ui_wrist_preset (wristquad.cpp kPresets)
const char* const kWristHand[]   = { "Left", "Right", "Off" };                  // ui_wrist_<piece>_hand (controllers.h Hand 0 / 1)
const char* const kLeftRight[]   = { "Left", "Right" };                         // grapple_aim_hand / grapple_hand / grapple_pull_hand (Hand 0 / 1)
const char* const kWristAnchor[] = { "Controller", "Forearm", "Hand" };         // ui_wrist_<piece>_anchor (wristquad.cpp UiWristAnchor)
const char* const kSingleEye[]   = { "normal stereo", "left view only", "right view only" };
const char* const kSymmetric[]   = { "pass real asymmetry", "vertical only", "both axes" };
const char* const kFovSource[]   = { "tan(0.8) fallback", "runtime measured" };
// "never" was a lie: 0 does not skip locating, it locates at the frame boundary
// instead of at the first query. Both are exactly one locate per frame, which is
// why switching between them changes nothing you can see.
const char* const kRelocate[]    = { "at frame boundary", "at first query",
                                     "every query (broken)" };
const char* const kOffsetSpace[] = { "tracking space", "head local" };
// Order must match MeleeForcePart's switch in boneblend.cpp's ElementTableDump.
const char* const kMeleePart[] = {
    "off", "l_hand", "r_hand", "head", "spine2", "l_forearm", "r_forearm"
};
const char* const kUpAxis[]      = { "Y up", "Z up" };
const char* const kAxisXYZ[]     = { "X", "Y", "Z" };
const char* const kHookOps[]     = { "observe", "level", "delta" };
// Order must match MeleePoseDrive's use in dlvr_MeleePoseObserve.
const char* const kMeleePoseDrive[] = {
    "off (step 5a offset path)", "position only",
    "position + cWorld", "position + hand bone"
};
const char* const kMeleeHand[]   = { "right", "left" };
// comfort_turn_mode / comfort_snap_angle (the VR Comfort tab). The angle list IS snapturn::AngleDeg's table.
const char* const kTurnMode[]    = { "smooth", "snap" };
const char* const kSnapAngle[]   = { "15 deg", "30 deg", "45 deg", "60 deg", "90 deg" };
const char* const kCalibPosture[] = { "Standing", "Seated" };   // calib_posture (calibscreen.cpp, build A)
static_assert(sizeof(kSnapAngle) / sizeof(kSnapAngle[0]) == dlvr::snapturn::kAngleCount,
              "kSnapAngle must list exactly snapturn::AngleDeg's angles");
// Order must match MeleeSwingMode's use in swing.cpp.
const char* const kMeleeSwingMode[] = { "off (nothing installed)", "detect only (never hits)", "hit (full B1)" };

// BETA LEAN (spec 2026-10-18-beta-leak-hardening 6.4): a setting's label and help are compiled in only when a BETA tab
// draws them - the VR Comfort tab draws settings by group (overlay.cpp TabForSetting: kComfortGroup = "VR Comfort");
// the Anti-Aliasing tab builds its own UI and draws no setting's help. Every other entry keeps its KEY (file parsing,
// the config: dump) and compiles with empty text: the condition is a constant expression, so the unchosen literal is
// never referenced and never emitted.
#if defined(DLVR_BETA_LEAN)
constexpr bool BetaGroupIs(const char* grp, const char* k)
{
    while (*grp && *grp == *k) { ++grp; ++k; }
    return *grp == '\0' && *k == '\0';
}
// + "Physical Parkour" (2026-11-27): its own BETA tab (overlay.cpp DrawParkourTab) draws the climb_ rows' labels and help.
constexpr bool BetaTextKept(const char* grp)
{
    return BetaGroupIs(grp, "VR Comfort") || BetaGroupIs(grp, "Physical Parkour");
}
#define DLVR_BETA_TEXT(grp, text) (BetaTextKept(grp) ? (text) : "")
// LOCKED and FOLLOWS entries are never drawn on any tab (settings.h `locked`), so BETA drops their text whatever the
// group - declare_rendered_orientation's help named SPEC-/INV- references from the VR Comfort group.
#define DLVR_BETA_DROP(text) ("")
#else
#define DLVR_BETA_TEXT(grp, text) (text)
#define DLVR_BETA_DROP(text) (text)
#endif

#define S_BOOL(k, v, lbl, grp, help) \
    { k, SetType::Bool, &v, 0, 1, DLVR_BETA_TEXT(grp, lbl), DLVR_BETA_TEXT(grp, help), kOffOn, 2, grp, false, false }
// A LOADBEARING switch Jay has locked on: never read from the file, never drawn (settings.h `locked`).
// A bool read once at startup (the bootstrap reads it as text before the game loads its data - grapple_unlock_skill).
#define S_BOOL_R(k, v, lbl, grp, help) \
    { k, SetType::Bool, &v, 0, 1, DLVR_BETA_TEXT(grp, lbl), DLVR_BETA_TEXT(grp, help), kOffOn, 2, grp, true, false }
#define S_BOOL_LOCKED(k, v, lbl, grp, help) \
    { k, SetType::Bool, &v, 0, 1, DLVR_BETA_DROP(lbl), DLVR_BETA_DROP(help), kOffOn, 2, grp, false, true }
// The same lock for a slider (2026-10-08, D-184, Jay: "Lock them aswell" - the AFW tuning values).
#define S_FLOAT_LOCKED(k, v, lo, hi, lbl, grp, help) \
    { k, SetType::Float, &v, lo, hi, DLVR_BETA_DROP(lbl), DLVR_BETA_DROP(help), nullptr, 0, grp, false, true }
#define S_INT_LOCKED(k, v, lo, hi, lbl, grp, help) \
    { k, SetType::Int, &v, lo, hi, DLVR_BETA_DROP(lbl), DLVR_BETA_DROP(help), nullptr, 0, grp, false, true }
// A bool that FOLLOWS a master bool (settings.h `follows`): hidden, file value never read, = the master after load and
// after every overlay edit.
#define S_BOOL_FOLLOWS(k, v, master, lbl, grp, help) \
    { k, SetType::Bool, &v, 0, 1, DLVR_BETA_DROP(lbl), DLVR_BETA_DROP(help), kOffOn, 2, grp, false, true, &master }
#define S_ENUM(k, v, lbl, grp, help, items)                                            \
    { k, SetType::Int, &v, 0,                                                          \
      static_cast<float>((sizeof(items) / sizeof((items)[0])) - 1),                    \
      DLVR_BETA_TEXT(grp, lbl), DLVR_BETA_TEXT(grp, help), items,                      \
      static_cast<int>(sizeof(items) / sizeof((items)[0])), grp, false }
#define S_FLOAT(k, v, lo, hi, lbl, grp, help) \
    { k, SetType::Float, &v, lo, hi, DLVR_BETA_TEXT(grp, lbl), DLVR_BETA_TEXT(grp, help), nullptr, 0, grp, false }
#define S_FLOAT_R(k, v, lo, hi, lbl, grp, help) \
    { k, SetType::Float, &v, lo, hi, DLVR_BETA_TEXT(grp, lbl), DLVR_BETA_TEXT(grp, help), nullptr, 0, grp, true }
#define S_INT_R(k, v, lo, hi, lbl, grp, help) \
    { k, SetType::Int, &v, lo, hi, DLVR_BETA_TEXT(grp, lbl), DLVR_BETA_TEXT(grp, help), nullptr, 0, grp, true }
#define S_INT(k, v, lo, hi, lbl, grp, help) \
    { k, SetType::Int, &v, lo, hi, DLVR_BETA_TEXT(grp, lbl), DLVR_BETA_TEXT(grp, help), nullptr, 0, grp, false }

const Setting kSettings[] = {
    S_FLOAT_R("render_scale", g_renderScale, 0.25f, 2.0f, "Render scale", "Resolution",
            "1.0 is exactly the runtime's recommended rect. This is a frame rate knob, "
            "not a stability one - the crashes were the frame loop, not the resolution."),
    S_INT_R("max_dim", g_maxDim, 0, 8192, "Max dimension", "Resolution",
          "Hard cap on the larger side, aspect preserved. 0 = no cap."),

    S_ENUM("fov_source", g_fovSource, "FOV source", "Field of view",
           "Where the frustum handed to the game comes from. The fallback is narrower "
           "than a Quest 3 lens, which is what left black bars at the outer edges.",
           kFovSource),
    S_ENUM("symmetric_fov", g_symmetricFov, "Symmetrise", "Field of view",
           "The engine handles a symmetric frustum only. Taking the LARGER half-angle on "
           "each axis gives the smallest symmetric frustum that still contains the lens, "
           "so nothing is missing at any edge.", kSymmetric),
    S_BOOL("fov_match_aspect", g_fovMatchAspect, "Match texture aspect", "Field of view",
           "Grow the frustum until its aspect matches the eye texture's. They have to "
           "agree, or an engine that derives one projection axis from the other through "
           "the viewport aspect renders a frustum nobody asked for."),
    S_BOOL("fov_center_correct", g_fovCenterCorrect, "Re-centre FOV", "Field of view",
           "Off. Declaring a frustum different from the one rendered warps the image."),
    S_BOOL("vertical_extend", g_verticalExtend, "Extend vertical", "Field of view",
           "Off. Superseded by symmetrising, which covers the lens properly."),

    S_BOOL("auto_pitch", g_autoPitch, "Auto pitch", "Placement",
           "Rotate the declared pose by the vertical FOV's angular bisector, -5.5 deg on "
           "a Quest 3. Corrects the runtime's declared view axis: the panel's optical "
           "centre sits below the view pose's forward."),
    S_FLOAT("pitch_offset_deg", g_pitchOffsetDeg, -20.0f, 20.0f, "Pitch offset",
            "Placement", "Manual override for auto pitch. Negative moves the picture down."),

    S_ENUM("eye_offset_space", g_eyeOffsetSpace, "Eye offset space", "Stereo",
           "Head local is correct here. The engine rotates HmdToEyeOffset itself, so "
           "handing it an already-rotated vector rotated it twice and the eye separation "
           "fell off as cos(yaw).", kOffsetSpace),
    S_BOOL("auto_eye_assign", g_autoEyeAssign, "Auto eye assign", "Stereo",
           "Work out which rendered image is the left eye from the poses, in head space, "
           "and latch it. Latching matters: it must not change as you turn."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_eye_pose_compare", g_diagEyePoseCompare, "Diagnostic: compare final eye poses",
           "Stereo",
           "Purely observational - logs the angular difference between the "
           "two eyes' final poses declared to OpenXR every ~2s. Should be "
           "~0 degrees (real stereo differs eyes by translation only) - "
           "investigating a VR-only ground-plane rendering artifact, see "
           "CLAUDE.md."),
#endif
    S_BOOL("swap_eyes", g_swapEyes, "Swap eyes", "Stereo",
           "Manual override on top of auto assign."),
    S_BOOL("invert_eye_offset", g_invertEyeOffset, "Invert eye offset", "Stereo",
           "Negate HmdToEyeOffset, for the SDK 0.x meaning. This engine wants 1.x."),
    S_FLOAT("diag_fake_view_cant_deg", g_fakeViewCantDeg, -25.0f, 25.0f,
            "Diagnostics: fake canted displays (deg per eye)", "Stereo",
            "Test only. Pretends the headset's two displays are turned outward by this many degrees each, like a Pimax. "
            "Positive: the canted-display fix handles it and the picture should look normal. Negative: the same fake "
            "with the fix bypassed, which shows the double image a canted headset had before the fix. Read at launch. "
            "Not valid over Meta Link. DEFAULT 0."),
    // LOCKED OFF 2026-10-07 (Jay: "Yes" - lock declare_rendered_orientation's prerequisites; the declaration refuses by name at 1).
    S_BOOL_LOCKED("use_render_pose", g_useRenderPose, "Use game render pose", "Stereo",
           "Describe each eye image with the pose the GAME says it rendered from. "
           "Declared must equal rendered or the compositor resamples wrongly."),

    S_ENUM("relocate_on_query", g_relocateOnQuery, "Pose refresh", "Timing",
           "Once per frame is right. Every query gave each of sixteen job-system worker "
           "threads a different pose within one frame.", kRelocate),
    S_BOOL("vr_force_vsync_off", g_vrForceVsyncOff, "Force VR video settings off", "Timing",
           "On (recommended). In VR, loads VSync, the frame cap, NVIDIA HBAO+ / DOF / PCSS and motion blur OFF at every "
           "launch, whatever video.scr or the menu says, and disables the game's frame limiter. "
           "Takes effect at the next launch. Flat play is untouched. Logs [VSYNCOFF]."),
    // `mirror` moved to the VR Comfort group 2026-11-14 (Jay: "Add a toggle for the mirror in VR Comfort") - see after
    // comfort_cutscene_pitch below. src/ keeps it here under "Timing".
    S_ENUM("debug_single_eye", g_debugSingleEye, "Single eye", "Debug",
           "Submit only one eye, to isolate an eye-assignment problem.", kSingleEye),

    S_BOOL("horizon_lock", g_horizonLock, "Horizon lock", "Camera",
           "Remove the game camera's pitch and roll, keep its yaw, so gamepad look "
           "and cutscenes stop tipping the world. Needs a camera picked on the "
           "Camera tab first."),
    S_ENUM("cam_up_axis", g_camUpAxis, "Up axis", "Camera",
           "Which axis the engine calls up. Y is OpenXR's; engines of this "
           "vintage are often Z-up, and then what we call pitch is their roll.",
           kUpAxis),

    S_BOOL("block_pad_pitch", g_blockPadPitch, "Block gamepad pitch", "Camera",
           "Zero the right stick's vertical axis before the game sees it, so you "
           "pitch with your head instead. Independent of horizon lock: this stops "
           "pitch being introduced, the lock cancels pitch already there."),
    S_BOOL("block_mouse_pitch", g_blockMousePitch, "Block mouse pitch", "Camera",
           "Zero the mouse's vertical motion before the game sees it (an SDL_PollEvent "
           "hook, since this engine reads mouse-look through SDL2, not DirectInput/"
           "RawInput). Horizon lock only re-levels the RENDERED camera - the game's own "
           "underlying mouse-driven pitch tracking keeps accumulating invisibly "
           "regardless, and that hidden value still feeds the aim-redirect hook's "
           "`original` direction, showing up as an unexplained offset when it drifts "
           "away from level (confirmed 2026-09-02). This stops the drift at the source."),

    S_BOOL("vr_sprint", g_vrSprint, "Left stick click sprints", "Controller Bindings",
           "Drive Sony's standard L3 (left stick click) bit in the scePad buttons mask "
           "from the LEFT-hand VR controller's own thumbstick click - confirmed "
           "2026-09-02 that L3 is genuinely Dying Light's sprint binding."),
    S_BOOL("aim_hook_auto_observe", g_aimHookAutoObserve,
           "Aim hook: auto-install (observe only)", "Aim",
           "Installs the aim hook at startup with AimHookOverride forced OFF, "
           "so the game's own UNMODIFIED ComputeShootDir output is logged "
           "including its elevation angle. For the grapple-hook bug: fire "
           "level, grapple, fire level again and compare. See CLAUDE.md."),
    S_BOOL("vr_aim_redirect", g_vrAimRedirect,
           "Aim: redirect shots to the right controller", "Aim",
           "Installs the aim hook with the override ON and keeps it armed, so "
           "where you point the right controller is where the shot goes. "
           "Settings-file driven because the overlay is closed during any real "
           "headset test. Takes precedence over aim_hook_auto_observe."),
    S_BOOL("aim_absolute", AimAbsolute,
           "Aim: absolute (live controller), not relative", "Aim",
           "ON: the shot direction comes straight from the right controller's "
           "own live pointing direction, mapped into game space by comparing "
           "the head's heading against the game's own. Pitch therefore comes "
           "entirely from your hand, which is what makes it immune to the "
           "grapple hook's pitch accumulator. OFF falls back to the older "
           "relative path (controller-relative-to-head applied on top of the "
           "game's direction), which inherits that corruption. See CLAUDE.md."),
    S_BOOL("aim_mirror_yaw", AimMirrorYaw,
           "Aim: mirror the horizontal plane", "Aim",
           "Only flip this if aiming RIGHT sends the shot LEFT. A single "
           "head/direction vector pair cannot detect a mirrored horizontal "
           "plane between OpenXR and game space, so this is the one axis the "
           "absolute mapping has to be told about rather than deriving."),
    S_BOOL("fire_move_penalty_off", FireMovePenaltyOff,
           "Firearms: no movement accuracy penalty (as when aiming)", "Aim",
           "Guns normally spread wider while you move, jump or sprint unless you aim down sights - which VR never does. "
           "ON: the game's per-hand movement penalty (PlayerDI::UpdateAccuracy) is held at 0, exactly as while aiming. "
           "Two gamedll bytes (+BC3A02), written once when the aim hook installs. The weapons' AIMED cone itself comes "
           "from Data3.pak (tools/pak_firespread.py), not this setting. Read at startup. DEFAULT OFF."),
    S_BOOL("fire_barrel_origin", FireBarrelOrigin,
           "Firearms: shots start at the gun's barrel, not your head", "Aim",
           "The game starts every bullet's hit ray at a point on your HEAD and only takes its direction from the controller, "
           "so shots fly parallel to the barrel but offset by the hand-to-head distance. ON: the ray starts at the barrel "
           "point the game already computes for the held gun (Bullet::Init, firearm shots only). Read at startup. DEFAULT OFF."),
    S_BOOL("fire_aim_from_barrel", FireAimFromBarrel,
           "Firearms: shots go down the gun's barrel, not the controller's aim ray", "Aim",
           "The shot direction normally comes from the controller's AIM pose, but the gun you see is posed from its GRIP pose "
           "- measured ~9 degrees apart on the AR. ON: the shot's base direction is the barrel's own bore, read from the held "
           "gun each frame; the weapon's spread still applies. Needs the rig running. Read at startup. DEFAULT OFF."),
    S_BOOL("fire_barrel_helper", FireBarrelHelper,
           "Crossbows: bolts go down the crossbow (use the weapon's own barrel point)", "Aim",
           "The barrel-aim fix reads a 'barrel' part on the held gun, but on a crossbow that part points sideways, so bolts "
           "fly about 90 degrees off. ON: when the weapon names its own barrel point (crossbows: the bolt, bows: the arrow), "
           "the shot starts there and goes along it. Guns are unchanged. Needs fire_aim_from_barrel=1. Read at startup. "
           "DEFAULT ON."),
    S_BOOL("bow_aim_from_bow", BowAimFromBow,
           "Bows: arrows leave the bow and fly where the bow points, not where you look", "Aim",
           "The game starts every arrow at your HEAD and sends it where your head looks; only the arrow you see starts at the "
           "bow and then bends onto that line. ON: the arrow starts at the nocked arrow and flies along the bow, at the "
           "speed your draw gave it. The game's lock-on aim assist no longer steers arrows. Read at startup. DEFAULT ON."),
    S_BOOL("fire_reticle_3d", FireReticle3d,
           "Firearms: a 3D reticle where the gun's barrel points (replaces the HUD dot)", "Aim",
           "While a gun is held, a small dot is drawn in the headset itself at the point where a ray from the muzzle along "
           "the barrel meets the world (50 m down the barrel if nothing is hit). It is not part of the game's HUD, so it is "
           "not held to the screen, and it has real depth. The game's own crosshair is hidden while it shows. Needs "
           "fire_aim_from_barrel=1 and overlay_enabled=1; hidden while the overlay menu is open. Read at startup. DEFAULT OFF."),
    S_BOOL("fire_reticle_bullet_cast", FireReticleBulletCast,
           "Firearms: the 3D reticle stops where the BULLET would (the bullet's own hit check)", "Aim",
           "With fire_reticle_3d on, the reticle's ray uses the game's own bullet hit-check instead of the grapple's, so it stops "
           "on exactly what a bullet would hit. 0 = the older grapple-style check. Read at startup. DEFAULT ON."),
    S_BOOL("bow_fire_latched", BowFireLatched,
           "Bows: arrows leave along the line you held at full draw (A/B, live)", "Aim",
           "Test toggle, takes effect at once. ON: each arrow flies along the direction the bow had on the last frame you were "
           "holding the draw - the line the bow reticle showed - instead of the pose the bow moves to as it releases. OFF: the "
           "release pose (about 2 degrees left and 1.4 up of the drawn line, measured). Every arrow logs how far the two differ. "
           "DEFAULT ON."),
    S_BOOL("bow_reticle_3d", BowReticle3d,
           "Bows: a 3D reticle where the arrow will land, only while drawn", "Aim",
           "While you draw or hold the bow (R2), the game's bow crosshair is drawn in the headset where the arrow will land - "
           "following the arrow's real drop for the current draw - and its brackets close as the draw completes, like the flat "
           "game's. A small dot shows where the arrow's shaft points in a straight line; the gap between the dot and the "
           "brackets is the drop. While the bow is held but not drawn, no crosshair shows. Needs fire_reticle_3d=1 and "
           "overlay_enabled=1; "
           "matches the arrow when bow_aim_from_bow=1 and bow_fire_latched=1. Read at startup. DEFAULT ON."),
    S_FLOAT("fire_reticle_scale", FireReticleScale, 0.0f, 1.0f,
            "Firearms: 3D crosshair size", "Aim",
            "Size of the 3D crosshair's parts, live. 1.0 = the same angular size the game's flat crosshair has (large in VR); "
            "0 hides the parts, dot and hit marker. How far the parts open on fire is the real shot spread and is not scaled. "
            "DEFAULT 0.4."),
    S_BOOL("grapple_aim_from_gun", GrappleAimFromGun,
           "Grapple: with a gun out, aim the grapple down the gun's barrel", "Aim",
           "With a firearm in hand the grapple's ray comes from the muzzle along the barrel, so the gun's reticle is also where the "
           "grapple goes. With melee or empty hands it keeps aiming along the controller. Needs grapple_hand_aim=1 and "
           "fire_aim_from_barrel=1. Read at startup. DEFAULT ON."),
    S_BOOL("grapple_reticle_3d", Grapple3dReticle,
           "Grapple: show its 'can grapple' icon in 3D at the target", "Aim",
           "Replaces the flat HUD grapple icon with the game's own grapple icon drawn in the headset at the grapple's target: the "
           "'go' icon where you can grapple, nothing otherwise. "
           "Needs overlay_enabled=1. Read at startup. DEFAULT OFF."),
    S_FLOAT("grapple_reticle_scale", GrappleReticleScale, 0.0f, 1.0f,
            "Grapple: 3D icon size", "Aim",
            "Size of the 3D grapple icon, live, separate from the gun reticle. 1.0 = the flat HUD's angular size; 0 hides it. "
            "DEFAULT 0.4."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_move_bob", DiagMoveBob,
           "Diagnostics: [MOVEBOB] how much the aim and camera bob per frame while moving", "Aim",
           "Log-only. Per frame, in the view you see: the gun barrel's and the grapple ray's direction/position changes, the "
           "controller's own motion relative to the head, and the camera's vertical motion beyond the head - split by "
           "standing / walking / sprinting and gun / no gun. HEADSET. DEFAULT OFF."),
    S_BOOL("diag_ray_threads", DiagRayThreads,
           "Diagnostics: [RAYTHREAD] which threads run bullet hit-checks vs the HUD update", "Aim",
           "Log-only. Records the thread of every bullet world cast and of the HUD crosshair update, and counts bullet casts "
           "that happen while the gun reticle's own cast is running - whether the reticle can safely use the bullet's own "
           "hit-check. FLAT-safe. DEFAULT OFF."),
    S_BOOL("diag_fire_spread", DiagFireSpread,
           "Diagnostics: [FIRESPREAD] per-shot spread readback", "Aim",
           "One line per shot at the aim hook: the weapon's current cone (degrees), the movement term and its target, "
           "the aim flag/factor and zoom. Proves the Data3.pak cone and the movement patch are live. The first shot logs "
           "regardless. FLAT. Log-only. DEFAULT OFF."),
    S_BOOL("diag_aim_wfcr_scan", DiagAimWfcrScan,
           "Diagnostics: gun-controller heap scan (costly)", "Aim",
           "Starts a background scan of the game's whole memory every 3 seconds to find the gun's fire controller, "
           "only so each shot's log line can show the game's own shot direction. Costs most of one CPU core for the "
           "whole session. The aim does NOT use it. Needs vr_aim_redirect. Read at startup. Log-only. DEFAULT OFF."),
#endif
    S_BOOL("grapple_hand_aim", GrappleHandAim,
           "Grapple: aim with the right controller (R9)", "Aim",
           "The grapple's one aim ray (gamedll+0xD76000, INV-267) points where the right controller points "
           "instead of screen centre, so the valid/invalid reticle and the throw follow the hand. Uses the gun "
           "aim's own mapping (aim_mirror_yaw applies to both). One import-table pointer, changed only for the "
           "call that returns to gamedll+0xD760AB. The ray still STARTS at the eye: near targets land a hand-to-eye "
           "offset off (diag_grapple_aim measures it). Read at startup. DEFAULT OFF."),
    S_BOOL("grapple_hand_origin", GrappleHandOrigin,
           "Grapple: the ray starts at the right hand (R9 A2)", "Aim",
           "The grapple's ray - and the game's ledge assist - start at your hand instead of your eye, removing the "
           "near-target miss (~0.24 m sideways, measured run 1). A detour on gamedll+0xD7D630 substitutes ONLY for "
           "the aim ray and the ledge assist (return addresses +D760B7 / +D7DA4E); the attached rope's physics keeps "
           "the game's origin. Needs grapple_hand_aim=1. Read at startup. DEFAULT OFF."),
    S_BOOL("grapple_crosshair_marker", GrappleCrosshairMarker,
           "Grapple: the centre crosshair marks where the hand ray hits (R9 A3)", "Aim",
           "While the grapple aims, the game's own centre crosshair (the small dot) is moved onto the point the "
           "grapple's ray hits, using the game's own world-to-HUD projection; it returns to the centre whenever the "
           "grapple is not aiming. One import pointer (Raytrace) + one vtable slot (HudCrosshair update). "
           "Read at startup. DEFAULT OFF."),
    S_BOOL("grapple_marker_hide_on_miss", GrappleMarkerHideOnMiss,
           "Grapple: hide the hand-ray dot when the ray hits nothing", "Aim",
           "With grapple_crosshair_marker on, the dot is drawn where the grapple's ray hits - but on a MISS the ray's end is 25 m "
           "further on, so the dot could look like it sits on a surface the ray actually passed. ON: a miss parks the dot "
           "off-screen, so a visible dot always means the ray is touching something. Read at startup. DEFAULT OFF."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_grapple_aim", DiagGrappleAim,
           "Diagnostics: [GRAPPLEAIM] grapple hand-aim windows", "Aim",
           "Per second while the grapple aims: redirects, refusals by reason, mean game->hand pitch/yaw change, "
           "and the measured hand-to-eye offset perpendicular to the aim (the near-target miss). The install "
           "line and the first redirect log regardless. HEADSET. Log-only. DEFAULT OFF."),
#endif
    // COMPILED OUT OF THE PUBLIC BUILD ON PURPOSE. g_showReticle is forced
    // false there (overlay.cpp) because the reticle belongs to the aim-override
    // feature that build ships without; if the key existed here, an ovr2xr.txt
    // line would switch it straight back on.
    S_BOOL("show_reticle", g_showReticle,
           "Aim: show the reticle", "Aim",
           "A small marker along the right controller's own aim ray, drawn ONLY "
           "while this panel is CLOSED - the two share one overlay swapchain. "
           "Persisted since 2026-09-14; it used to reset to ON every launch, "
           "which meant the overlay's render path ran every frame even with the "
           "menu shut and there was no way to configure it off for a run. Turn "
           "it off to take the overlay out of the frame entirely. The toggle "
           "and its distance slider are on the Aim debugging tab."),
    S_BOOL("aim_head_smoothing", AimHeadSmoothing,
           "Aim: smooth the head used for the mapping", "Aim",
           "Low-pass filters the HEAD orientation the absolute mapping is "
           "derived from. It does NOT smooth the controller, so no lag is "
           "added to where the shot goes. Two effects: less jitter on the "
           "derived mapping, and a damping of the head-turn offset - our head "
           "sample leads the one the game used to build its own direction, "
           "and a small lag moves ours back toward it."),
    S_FLOAT("aim_head_smoothing_ms", AimHeadSmoothingMs, 0.0f, 250.0f,
            "Aim: head smoothing strength (ms)", "Aim",
            "Exponential time constant. Higher is smoother but lags further "
            "behind a fast head turn. 0 disables the filtering even with the "
            "toggle on. Default 60 is deliberately light - raise it only if "
            "the aim still looks jittery, lower it if turning feels laggy."),
    S_BOOL("vr_decouple_move", g_vrDecoupleMove,
           "Decouple movement from head look", "Controller Bindings",
           "Counter-rotates the injected left stick by the head's yaw, so "
           "pushing forward travels along your BODY's forward regardless of "
           "where you are looking. Fixes head-turn curving your movement "
           "while running. Needs vr_move on. See CLAUDE.md."),
    S_FLOAT("vr_decouple_max_deg", g_vrDecoupleMaxDeg, 0.0f, 90.0f,
            "Decouple: max correction (deg)", "Controller Bindings",
            "KNOWN COMPROMISE, not a preference. The game only sustains "
            "sprint while the stick reads near-forward, and the correction "
            "necessarily makes it non-forward - so a full correction drops "
            "your sprint whenever you turn your head. Clamping keeps sprint "
            "alive at the cost of leaving some curve at large head angles. "
            "90 = full correction (best straightness, breaks sprint), 0 = "
            "off. The real fix is engine-side; see CLAUDE.md."),
    S_BOOL("vr_move", g_vrMove, "Left stick movement", "Controller Bindings",
           "Drive the scePad left-stick X/Y bytes the engine reads for movement from "
           "the LEFT-hand VR controller's own thumbstick, the same injection technique "
           "as vr_trigger_fire below. Overwrites the real left stick outright rather "
           "than blending - unlike the trigger's max()-with-real-pad approach, a "
           "bipolar movement axis has no sensible way to combine two sources."),
    S_BOOL("vr_turn", g_vrTurn, "Right stick turning", "Controller Bindings",
           "Drive the scePad right-stick X byte (turning) from the RIGHT-hand VR "
           "controller's own thumbstick X axis. Smooth or snap is chosen on the VR Comfort "
           "tab (comfort_turn_mode); this is the master for both. Right-stick Y (pitch) is "
           "deliberately left untouched here - see block_pad_pitch for that axis."),
    S_ENUM("comfort_turn_mode", ComfortTurnMode, "Turning", "VR Comfort",
           "SNAP turns you a fixed angle per flick of the right stick - the comfortable option, and the default. "
           "SMOOTH turns continuously, at the game's own stick speed. Snap needs the rig running "
           "(diag_bone_blend_probe=1); until its hook is in, the stick stays smooth and the log says why.",
           kTurnMode),
    S_ENUM("comfort_snap_angle", ComfortSnapAngle, "Snap angle", "VR Comfort",
           "How far one flick turns you. 30 degrees is the standard; 45 and 90 are common elsewhere, 15 is fine "
           "grained. Stored in the file as the list position: 0=15, 1=30, 2=45, 3=60, 4=90.",
           kSnapAngle),
    // BETA (2026-10-19, Jay: "Package that with the toggle available for the beta build"): the DEV help text's last sentence
    // named a diag setting - removed here (leak hardening 6.4); src/ keeps it.
    // plan 2026-11-10-buggy-wheel-and-key Task 2 (Jay 2026-11-10 (14): "A toggle in the VR Comfort tab"; shipped ON - his Tier C pick).
    S_BOOL("buggy_hand_steer", dlvr::BuggyHandSteer, "Hands on the buggy", "VR Comfort",
           "ON: in the buggy, grip the steering wheel with either hand to steer - the stick still steers whenever no hand is on "
           "the wheel - and turn the key with your right hand to start the engine. OFF: the buggy drives as in the base game. "
           "Takes effect at the next launch."),
    S_BOOL("comfort_body_follows_head", ComfortBodyFollowsHead, "Body follows head", "VR Comfort",
           "ON: turning your head or your whole body turns your character with it, instantly, and pushing the stick "
           "forward walks you where you are looking. OFF: your head is only a view - the character turns with the "
           "stick alone. Aim and the grapple stay on your hands either way. Changes take effect at once."),
    // LOCKED ON 2026-11-09 (Jay, RUN SY1: "That's fixed it" -> "probably best off having it shipped as is. Verified, lock & hide the
    // setting") - never read from the file, never drawn; ships at the compiled true. See CameraShakeYawOff's [DEP:] in boneblend.cpp.
    S_BOOL_LOCKED("comfort_camera_shake_yaw_off", dlvr::CameraShakeYawOff, "Steady view when firing", "VR Comfort",
           "ON: the game's own camera shake (each shot, melee hits, damage, vehicles) no longer turns your view. "
           "OFF: the game's shake reaches the view as before."),
    S_BOOL("roomscale_enable", dlvr::RoomscaleEnable, "Roomscale", "VR Comfort",
           "Your real steps move your character. Off: only the stick moves you. On foot only - ladders, ziplines, vehicles, "
           "cutscenes and menus leave your character where it is."),
    // LOCKED 2026-10-28 (Jay: "Roomscale & Physical Crouch are to be Enable/Disable boxes only. Bake the other values") - the
    // four tuning values below are never read from the file, never drawn; each ships at its compiled default (= Jay's disk:
    // 0.25 / 0.3 / 0.35 / 0.25). roomscale_bleed_mps 0.3 verified by Jay 2026-10-28 run B1 ("walking felt fine"). src/ keeps them.
    S_FLOAT_LOCKED("roomscale_lean_max_m", dlvr::RoomscaleLeanMaxM, 0.05f, 0.5f, "Roomscale: lean past a wall (m)", "VR Comfort",
            "With roomscale on: how far your view may lean past your character when it cannot follow you (a wall, a ladder). "
            "Past this your view stays put. Live."),
    S_FLOAT_LOCKED("roomscale_bleed_mps", RoomscaleBleedMps, 0.0f, 1.0f, "Drift correction while walking (m/s)", "VR Comfort",
            "While you walk with the stick, slowly moves your view back over your character if your real steps have taken you "
            "off it. Standing still and leaning are never touched. 0 = off. Live."),
    S_BOOL("physical_crouch_enable", dlvr::PhysicalCrouchEnable, "Physical crouch", "VR Comfort",
           "Crouch for real and your character crouches. A crouch with the button still works as before. Needs the mod's Data3 "
           "(it adds the crouch input this uses). Takes effect at the next launch."),
    S_FLOAT_LOCKED("physical_crouch_on_drop_m", dlvr::PhysicalCrouchOnDropM, 0.15f, 0.8f, "Physical crouch: crouch at a drop of (m)",
            "VR Comfort",
            "How far below your standing height your head must go before your character crouches. Must be larger than the stand "
            "drop below. Live."),
    S_FLOAT_LOCKED("physical_crouch_off_drop_m", dlvr::PhysicalCrouchOffDropM, 0.10f, 0.7f, "Physical crouch: stand at a drop of (m)",
            "VR Comfort",
            "Your character stands back up once your head is back within this distance of your standing height. Smaller than the "
            "crouch drop, so it does not flicker. Live."),
    S_BOOL("physical_crouch_slide", dlvr::PhysicalCrouchSlide, "Physical crouch: slide while sprinting", "VR Comfort",
           "With physical crouch on: crouching for real while you sprint makes your character slide. Off: a real crouch waits "
           "until the sprint ends. Live."),
    S_BOOL("comfort_cutscene_pitch", CutscenePitch, "Flatscreen Cutscene Animations", "VR Comfort",
           "Keeps the camera pitch true to the original cutscenes - WARNING MAY CAUSE MOTION SICKNESS"),
    S_BOOL("rig_cutscene_standdown", RigCutsceneStanddown, "Cutscene arms from the game", "VR Comfort",
           "In cutscenes and conversations, your arms, hands and fingers play the scene's own animation instead of following your "
           "controllers. Your view still follows your head. Off: you keep hand control in scenes. Live."),
    // Desktop mirror (deskmirror.cpp; Jay 2026-11-14: "Add a toggle for the mirror in VR Comfort"; trace 20261114_1110). Live: read
    // every submit. Was group "Timing" (hidden tab in BETA, its text stripped).
    S_BOOL("mirror", g_mirror, "Desktop mirror", "VR Comfort",
           "Show the right eye's picture - what the headset is given - in the desktop window, cropped to the "
           "window's shape, refreshed at most 60 times a second. Wrist HUD and other headset add-ons are not "
           "in it. Off: the window only shows a picture behind the mod menu while the menu is open."),
    S_ENUM("calib_posture", dlvr::CalibPosture, "I play (calibration)", "VR Comfort",
           "Standing or seated - asked first by the calibration screen ('Calibrate my arms' at the top of the menu), which saves "
           "your answer here. The seated poses are not built yet, so the screen asks you to stand either way.",
           kCalibPosture),
    // LOCKED ON 2026-10-18 (Jay: "Calibration_screen_auto is to be hidden & locked") - never read from the file, never drawn.
    S_BOOL_LOCKED("calib_screen_auto", dlvr::CalibScreenAuto, "Calibration screen at start-up", "VR Comfort",
           "Opens the arm calibration screen by itself at the main menu when you have not calibrated yet (no saved "
           "calibration). It can always be skipped with B, and opened any time from 'Calibrate my arms'."),
    // LOCKED AT 0 2026-10-18 (Jay: "Hide comfort_snap_repeat_ms"; hiding is a lock, and 0 was his pick) - one snap per flick.
    S_INT_LOCKED("comfort_snap_repeat_ms", ComfortSnapRepeatMs, 0, 1000, "Snap repeat while held (ms)", "VR Comfort",
          "0 = one snap per flick: let the stick back to centre to snap again. Above 0, holding the stick keeps "
          "snapping every this many milliseconds (250-400 is usual)."),
    // Group "Arm fit" (2026-10-13, Jay: "those should be in the rigging tab itself, not VR comfort ... these are fixes"; then "Move
    // rig_arm_fit to the rigging tab aswell. Only Arm Calibration should stay in VR comfort"): a rig_ key outside the VR Comfort
    // group is filed to the Rigging tab by TabForSetting (overlay.cpp). The calibration itself is a node DrawComfortTab draws.
    S_BOOL("rig_arm_fit", RigArmFit, "Fit the arms to my length", "Arm fit",
           "Makes the in-game arms your own length, from the arm calibration ('Calibrate my arms' at the top of the menu - run "
           "it once, standing). The "
           "upper arm and forearm are lengthened or shortened by the same percentage so your hand reaches where your "
           "controller is; hands and weapons keep their size. Without a calibration nothing changes. Works live."),
    S_FLOAT("rig_arm_fit_straight_cm", RigArmFitStraightCm, 0.0f, 5.0f, "Straight arm at full reach (cm)", "Arm fit",
            "When your arm is fully stretched, the in-game arm goes fully straight too. Over the last few centimetres of "
            "reach it straightens smoothly instead of keeping a slight bend at the elbow. Bigger = it straightens earlier; "
            "your hand can sit up to this far past the controller at full stretch. Needs 'Fit the arms to my length' and a "
            "calibration (VR Comfort tab). 0 = off. Works live."),
    S_FLOAT("rig_arm_fit_follow_cm", RigArmFitFollowCm, 0.0f, 20.0f, "Shoulder follows a full reach (cm)", "Arm fit",
            "When you reach further than your arm's calibrated length - your shoulder blade coming forward - the in-game "
            "shoulder slides toward your hand by up to this many centimetres, so the arm does not stop short. Needs 'Fit "
            "the arms to my length' and a calibration (VR Comfort tab). 0 = off. Works live."),
    S_FLOAT("rig_arm_fit_shoulder_cm", RigArmFitShoulderCm, 0.0f, 12.0f, "Shoulder rolls with the reach (cm)", "Arm fit",
            "As you reach out or up, the in-game collarbone rolls forward / up the way a real shoulder does, so the in-game "
            "arm straightens when YOUR arm straightens instead of a little before it. It starts at half reach and is "
            "limited to this many centimetres of shoulder movement. Reaches out to the side and hanging arms are not moved. "
            "Not while a bow is held, unless 'Arm at its true length' is on. Needs 'Fit the arms to my length', a calibration "
            "(VR Comfort tab), and the clavicle hold. 0 = off. Works live."),
    S_BOOL("rig_arm_fit_anatomical", RigArmFitAnatomical, "Arm at its true length (side-pose calibration)", "Arm fit",
           "Uses your arm's true length - measured from your shoulder with the arm straight out to the side during the arm "
           "calibration - instead of your full forward reach, so the in-game arm is straight out to the side when yours is. "
           "The shoulder roll then makes up the extra reach forward and up (with this on it also works with the bow and the "
           "two-handed grip). Needs a calibration made with this build (recalibrate once) and 'Shoulder rolls with the reach' "
           "above 0. Works live."),
    S_FLOAT("rig_arm_fit_shoulder_max_deg", RigArmFitShoulderMaxDeg, 10.0f, 60.0f, "Shoulder roll limit (deg)", "Arm fit",
            "How far the collarbone may roll, in degrees. 33 is the usual limit; with 'Arm at its true length' on, the shoulder "
            "has more to make up and needs about 45. Works live."),
    S_BOOL("comfort_hand_steady", ComfortHandSteady, "Hand steadiness", "VR Comfort",
           "Smooths out the tiny shakes of your hands when you hold still - steadier aim with guns, the bow and the "
           "grapple - while fast, deliberate movements stay instant. Melee weapons use the lighter Melee steadiness "
           "below instead, so swings keep their full speed."),
    S_INT("comfort_hand_steady_strength", ComfortHandSteadyStrength, 0, 100, "Steadiness strength", "VR Comfort",
          "How much a still hand is smoothed. Higher = steadier aim, but slow careful movements lag a little more. "
          "50 is the starting point."),
    S_INT("comfort_hand_steady_response", ComfortHandSteadyResponse, 0, 100, "Steadiness response", "VR Comfort",
          "How quickly the smoothing gets out of the way when you move. Higher = less lag on fast moves but more of "
          "the shake gets through during slow ones. 50 is the starting point."),
    S_INT("comfort_melee_steady", ComfortMeleeSteady, 0, 100, "Melee steadiness", "VR Comfort",
          "A very light version of hand steadiness for melee weapons: it only calms the tiniest movements of a still "
          "hand, and gets out of the way completely during a swing. 0 = off. 15 is the starting point."),
    // D-184 (Jay, 2026-10-04 / 2026-10-08): the ONE AFW switch. afw_alt_eye + afw_rebuild follow it (S_BOOL_FOLLOWS);
    // every AFW fix under them is LOCKED on (S_*_LOCKED). Default enabled (Jay: "Default to enabled").
    S_BOOL("afw_enable", AfwEnable, "Alternate Frame Warp", "VR Comfort",
           "Renders one eye per frame, taking turns, and rebuilds the other eye from its last image so both eyes stay "
           "in step. Higher frame rate in the headset; moving hands, weapons and fast objects may show slight "
           "edge artefacts. Untick to render both eyes every frame. VR only - flatscreen is never affected."),
    // Menu pin, stage 1 (SPEC-UI-MENU-PIN-2026-10-11; Jay 2026-10-11: "Switch off default is fine, toggle is fine").
    // Menu panel, stage 1 (SPEC-UI-MENU-PANEL-2026-10-11; Jay 2026-10-11: "Do the spec." + his four answers).
    S_BOOL("ui_panel_menus", UiPanelMenus, "Menus on a fixed panel", "VR Comfort",
           "While a menu is open (main menu, pause, inventory, map, shop) it is shown on a flat panel that stays where it "
           "was when the menu opened, instead of following your head. Needs Alternate Frame Warp on at launch. VR only. "
           "Live."),
    S_FLOAT("ui_panel_tabs_up_m", UiPanelTabsUpM, -0.5f, 0.8f, "Menu panel: inventory height (m)", "VR Comfort",
            "How much higher than the pause menu the inventory, map, quests, skills and shop sit on the panel. Live."),
    // LOCKED ON 2026-10-11 (Jay, after the v4 run: "The switch itself is good. Ship on & Hide") - never read from the file,
    // never drawn. Tier A (INV-537 add. 9). Still needs ui_panel_menus, which arms the code that names the root camera.
    S_BOOL_LOCKED("ui_panel_main_menu_turn", UiPanelMainMenuTurn, "Main menu: turn with the head", "VR Comfort",
           "At the main menu the scene turns with your head side to side instead of the picture riding your head. "
           "Needs 'Menus on a fixed panel'. Does nothing in game."),
    S_FLOAT("ui_panel_tabs_x_m", UiPanelTabsXM, -1.0f, 1.0f, "Menu panel: inventory left / right (m)", "VR Comfort",
            "Moves the inventory, map, quests, skills and shop sideways on the panel; negative = to the left. The pause "
            "menu does not move. Live."),
    S_FLOAT("ui_panel_distance_m", UiPanelDistanceM, 0.8f, 4.0f, "Menu panel distance (m)", "VR Comfort",
            "How far in front of you the menu panel is placed when a menu opens. Takes effect at the next opening."),
    S_FLOAT("ui_panel_height_m", UiPanelHeightM, -1.0f, 0.5f, "Menu panel height (m)", "VR Comfort",
            "The panel's centre against your head height when a menu opens; negative = below eye level. Next opening."),
    S_FLOAT("ui_panel_scale", UiPanelScale, 0.5f, 1.5f, "Menu panel size", "VR Comfort",
            "1.0 = the menu covers the same angle it does without the panel. Live."),
    S_BOOL("ui_panel_all_menus", UiPanelAllMenus, "Menu panel: all in-game menus", "VR Comfort",
           "On: the pause menu, inventory, map, quests, skills and shop all go on the panel. Off: only the pause menu. "
           "Live."),
    // HUD panel, stage 1 (SPEC-UI-HUD-PANEL-2026-10-11; Jay 2026-10-11: "Go for it"). Tier A / D - docs/TIERS.md.
    S_BOOL("ui_hud_panel", UiHudPanel, "HUD on a fixed panel", "VR Comfort",
           "In game, the whole HUD (health, minimap, weapon, quest text, markers and prompts) is shown on a flat panel "
           "that stays where it was placed in your room instead of riding your head. It is placed in front of you when "
           "the HUD first appears after a load. Markers and prompts sit on the panel, so they no longer point at the "
           "world. Needs Alternate Frame Warp on at launch. VR only. Live."),
    S_FLOAT("ui_hud_distance_m", UiHudDistanceM, 0.5f, 4.0f, "HUD panel distance (m)", "VR Comfort",
            "How far in front of you the HUD panel is placed. Moving it places the panel again at once."),
    S_FLOAT("ui_wheel_distance_m", UiWheelDistanceM, 0.5f, 4.0f, "Selection wheel distance (m)", "VR Comfort",
            "How far in front of you the weapon / throwables / arrow wheel is placed while it is open. The rest of the HUD hides meanwhile."),
    S_FLOAT("ui_hud_height_m", UiHudHeightM, -1.0f, 1.0f, "HUD panel height (m)", "VR Comfort",
            "The HUD panel's centre against your head height; negative = below eye level. Places the panel again at once."),
    S_FLOAT("ui_hud_x_m", UiHudXM, -1.5f, 1.5f, "HUD panel left / right (m)", "VR Comfort",
            "Moves the HUD panel sideways from straight ahead; negative = to the left. Places the panel again at once."),
    S_FLOAT("ui_hud_scale", UiHudScale, 0.3f, 1.5f, "HUD panel size", "VR Comfort",
            "1.0 = the HUD covers the same angle it does without the panel. Live."),
    S_BOOL("ui_hud_hide", UiHudHide, "Hide the HUD", "VR Comfort",
           "With 'HUD on a fixed panel' on: shows no HUD at all (for recording). Does nothing while the panel is off. Live."),
    // HUD panel lazy follow (docs/superpowers/specs/2026-10-20-hud-panel-lazy-follow-design.md). Tier A / D - docs/TIERS.md.
    S_BOOL("ui_hud_follow", UiHudFollow, "HUD panel follows you", "VR Comfort",
           "With 'HUD on a fixed panel' on: when you have turned away from the panel (or walked away from it) for a "
           "moment, it glides back in front of you. Small head turns never move it. Off = the panel stays where it was "
           "placed. Live."),
    S_FLOAT("ui_hud_follow_deg", UiHudFollowDeg, 15.0f, 120.0f, "HUD follow: turn angle (deg)", "VR Comfort",
            "How far you must turn away from the HUD panel before it follows. Live."),
    S_INT("ui_hud_follow_delay_ms", UiHudFollowDelayMs, 0, 3000, "HUD follow: wait (ms)", "VR Comfort",
          "How long you must stay turned away before the HUD panel follows. Live."),
    S_INT("ui_hud_follow_ms", UiHudFollowMs, 50, 2000, "HUD follow: glide time (ms)", "VR Comfort",
          "How long the HUD panel takes to glide back in front of you. Live."),
    S_FLOAT("ui_hud_follow_drift_m", UiHudFollowDriftM, 0.2f, 3.0f, "HUD follow: walk distance (m)", "VR Comfort",
            "How far you can walk from where the HUD panel was placed before it follows. Live."),
    // WRIST HUD STAGE 3a (trace docs/traces/20261113_1040_wrist_stage3a_settings.md; spec 2026-11-12-wrist-hud-design §9). Group
    // "VR Comfort" so BETA lean keeps the text; overlay.cpp files every ui_ key on the HUD tab, and DrawWristHud draws the ui_wrist_
    // rows itself (Basic / Advanced). ui_wrist_hud Tier C (ships 0); the rest Tier D, defaults = preset A (wristquad.cpp).
    S_BOOL("ui_wrist_hud", UiWristHud, "Wrist HUD", "VR Comfort",
           "Health, the minimap, the quest list and the two weapon boxes leave the view and sit on your hands, where they move with "
           "the controllers; turn a hand away and its pieces fade out. Needs Alternate Frame Warp on. Takes effect at the next "
           "launch."),
    S_ENUM("ui_wrist_preset", UiWristPreset, "Wrist HUD layout", "VR Comfort",
           "A: minimap on the left hand; health, quest list and quick item on the left forearm; weapon on the right forearm. "
           "B: health and minimap on the left wrist, both weapon boxes on the right wrist, no quest list. Custom: your own.",
           kWristPreset),
    S_FLOAT("ui_wrist_size", UiWristSize, 0.3f, 3.0f, "Wrist HUD size", "VR Comfort",
            "The size of every piece on the hands. 1.0 = the minimap about 9 cm wide. Live."),
    S_FLOAT("ui_wrist_fade_start_deg", UiWristFadeStartDeg, 0.0f, 90.0f, "Wrist HUD: fade starts (deg)", "VR Comfort",
            "A piece is fully shown while it faces you within this angle; past it, it starts to fade. Live."),
    S_FLOAT("ui_wrist_fade_end_deg", UiWristFadeEndDeg, 1.0f, 120.0f, "Wrist HUD: gone at (deg)", "VR Comfort",
            "A piece turned this far away from you is not shown at all. Live."),
#define S_WRIST_PIECE(nm, i, lbl)                                                                                                   \
    S_ENUM("ui_wrist_" nm "_hand", UiWristHand[i], lbl ": hand", "VR Comfort",                                                     \
           "Which hand this piece sits on. Off = not shown at all (not on the hands and not in front of your eyes).", kWristHand),   \
    S_FLOAT("ui_wrist_" nm "_x", UiWristX[i], -0.3f, 0.3f, lbl ": across the hand (m)", "VR Comfort",                               \
            "Moves the piece across the hand, metres. Live."),                                                                       \
    S_FLOAT("ui_wrist_" nm "_y", UiWristY[i], -0.3f, 0.3f, lbl ": up / down (m)", "VR Comfort",                                     \
            "Moves the piece up or down from the controller, metres. Live."),                                                        \
    S_FLOAT("ui_wrist_" nm "_z", UiWristZ[i], -0.3f, 0.3f, lbl ": along the arm (m)", "VR Comfort",                                 \
            "Moves the piece toward the fingers (negative) or up the forearm (positive), metres. Live."),                            \
    S_FLOAT("ui_wrist_" nm "_pitch", UiWristPitch[i], -180.0f, 180.0f, lbl ": tilt (deg)", "VR Comfort",                             \
            "Tilts the piece forward or back. Live."),                                                                               \
    S_FLOAT("ui_wrist_" nm "_yaw", UiWristYaw[i], -180.0f, 180.0f, lbl ": turn (deg)", "VR Comfort",                                 \
            "Turns the piece about the controller's up axis - which way it faces. Live."),                                           \
    S_FLOAT("ui_wrist_" nm "_roll", UiWristRoll[i], -180.0f, 180.0f, lbl ": spin (deg)", "VR Comfort",                               \
            "Spins the picture in its own plane - use it if a piece reads upside down or sideways. Live."),                          \
    S_FLOAT("ui_wrist_" nm "_scale", UiWristScale[i], 0.3f, 3.0f, lbl ": size", "VR Comfort",                                         \
            "This piece's size, times the Wrist HUD size. Live."),                                                                   \
    S_ENUM("ui_wrist_" nm "_anchor", UiWristAnchor[i], lbl ": follows", "VR Comfort",                                               \
           "What the piece moves with: the controller, or your forearm / hand as drawn in game. Placing it by hand picks the "       \
           "nearest.", kWristAnchor)
    S_WRIST_PIECE("minimap", 0, "Minimap"),
    S_WRIST_PIECE("health", 1, "Health"),
    S_WRIST_PIECE("quests", 2, "Quest list"),
    S_WRIST_PIECE("weapon", 3, "Weapon"),
    S_WRIST_PIECE("quick", 4, "Quick item"),
#undef S_WRIST_PIECE
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_steady", DiagSteady, "Diagnostics: hand steadiness windows", "VR Comfort diagnostics",
           "Once a second per hand: how much the gun-hand's pointing direction moves frame to frame while you hold "
           "still, raw vs steadied, and the worst lag. HEADSET. Log-only. DEFAULT OFF."),
#endif
    S_BOOL("vr_face_buttons", g_vrFaceButtons, "Face buttons (A/B/X/Y)", "Controller Bindings",
           "Drive Sony's Cross/Circle/Square/Triangle bits in the scePad buttons mask "
           "from the Touch controllers' own A/B (right hand) and X/Y (left hand) "
           "buttons - 1:1 physical parity with a real gamepad's face buttons, same "
           "genre-convention bet already confirmed correct for L3 (vr_sprint) and R2 "
           "(vr_trigger_fire). Not yet independently confirmed in-game."),
    S_BOOL("vr_stick_click_right", g_vrStickClickRight, "Right stick click (R3)",
           "Controller Bindings",
           "Drive Sony's R3 bit in the scePad buttons mask from the RIGHT-hand VR "
           "controller's own thumbstick click. With the mod's Data3.pak, R3 is the KICK in gameplay "
           "(and the instant escape when a zombie grabs you); it no longer aims or readies a weapon throw. "
           "Survivor Sense is on X (hold 0.5 s)."),
    S_BOOL("vr_left_trigger", g_vrLeftTrigger, "Left trigger (L2)", "Controller Bindings",
           "Drive Sony's L2 analog byte in the scePad buffer from the LEFT-hand VR "
           "controller's own trigger pull, the same offset-and-scale technique already "
           "confirmed correct for R2 (vr_trigger_fire) - just the other hand. "
           "max()'d with whatever a real pad's own L2 already reports, so a real "
           "controller's L2 still works too. Not yet independently confirmed against "
           "this game's real left-trigger binding."),
    S_FLOAT("vr_left_trigger_deadzone", g_vrLeftTriggerDeadzone, 0.0f, 0.3f, "Left trigger deadzone",
            "Controller Bindings",
            "How far the left trigger must be pulled before the game sees any of it. Some controllers (an Index tester's) "
            "never rest at exactly 0. Above the deadzone the pull is rescaled, so a full pull is still a full pull. "
            "0 = off."),
    S_BOOL("index_grip_force", dlvr::IndexGripForce, "Valve Index: grip by squeezing", "Controller Bindings",
           "Valve Index controllers only. On: the grips (L1/R1, holsters, two-handed grab) react to SQUEEZING the "
           "controller's grip. Off: they react to fingers closing on the grip, which can trigger with a relaxed hand. "
           "No effect on other controllers."),
    S_BOOL("finger_track", dlvr::FingerTrack, "Finger tracking (empty hands)", "VR Comfort",
           "With nothing in a hand, its fingers copy yours on the controller: rest your finger on the trigger to curl the index, "
           "lift it to point; touch the stick, a face button or the thumb rest to put the thumb down; squeeze the grip to close "
           "the other fingers. Holding anything keeps its own grip. Live."),
    S_FLOAT("finger_track_ease_ms", dlvr::FingerTrackEaseMs, 0.0f, 500.0f, "Finger tracking: ease (ms)", "Controller Bindings",
            "How long a finger takes to go fully open <-> closed. 0 = instant. Live."),
    S_FLOAT("finger_track_fist_at", dlvr::FingerTrackFistAt, 0.05f, 1.0f, "Finger tracking: grip for a full fist", "Controller Bindings",
            "How far the grip must be squeezed for the middle, ring and little fingers to close fully. Live."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_ctl_input", dlvr::DiagCtlInput, "Diagnostics: controller inputs", "Controller Bindings",
           "Every 2 s per hand: the controller profile, whether the hand poses are tracked, and the lowest / highest "
           "trigger, grip and trackpad readings. HEADSET. Log-only. DEFAULT OFF."),
#endif
    S_BOOL("vr_grip", g_vrGrip, "Grip buttons (L1/R1)", "Controller Bindings",
           "Drive Sony's L1/R1 bits in the scePad buttons mask from both Touch "
           "controllers' analog squeeze, thresholded at 0.5 into a discrete press. "
           "With the mod's Data3.pak the right grip (R1) is buggy reset at an upgrade station "
           "- NOT jump; jump and parkour grab are on A. Camouflage over an Infected body is "
           "held back from the right grip and the Grapple skill is unbound. Left grip (L1) "
           "is unchanged."),
    S_BOOL("holster_enable", HolsterEnable, "Right-shoulder holster", "Controller Bindings",
           "Reach the right hand over the right shoulder and squeeze right grip: switches weapon (D-pad right tap) "
           "the moment you squeeze, and that press does not reach the game as R1. A light pulse marks the spot. Grip anywhere else "
           "is R1 exactly as before. Needs vr_grip=1. DEFAULT OFF."),
    S_FLOAT("holster_center_x", HolsterCenterX, -0.5f, 0.5f, "Holster centre X (right)", "Controller Bindings",
            "Zone centre, metres from the headset, +right. The frame turns with head yaw only."),
    S_FLOAT("holster_center_y", HolsterCenterY, -0.5f, 0.5f, "Holster centre Y (up)", "Controller Bindings",
            "Zone centre, metres from the headset, +up."),
    S_FLOAT("holster_center_z", HolsterCenterZ, -0.5f, 0.5f, "Holster centre Z (behind)", "Controller Bindings",
            "Zone centre, metres from the headset, +behind."),
    S_BOOL("bow_manual_draw", dlvr::BowManualDraw, "Bow: draw the string by hand", "Controller Bindings",
           "With a bow out, the right trigger only draws if you press it with your right hand AT the nocked arrow on the string. "
           "Hold it and pull your hand back: the string follows, and how far you pull sets how fast the arrow flies. Release to "
           "shoot; release with almost no pull to cancel and keep the arrow. Your fingers take the bow grip while you hold the "
           "string. On by default. Off = the trigger draws from anywhere, as before."),
    S_FLOAT("bow_draw_zone_m", dlvr::BowDrawZoneM, 0.03f, 0.20f, "Bow: string grab distance", "Controller Bindings",
            "How close (metres) your right hand must be to the nocked arrow for the trigger to grab the string."),
    S_FLOAT("bow_draw_full_m", dlvr::BowDrawFullM, 0.20f, 0.70f, "Bow: pull for full power", "Controller Bindings",
            "How far back (metres) you must pull the string for a full-power shot."),
    S_FLOAT("bow_draw_max_m", dlvr::BowDrawMaxM, 0.20f, 0.80f, "Bow: longest string pull", "Controller Bindings",
            "The furthest (metres) the string will follow your hand."),
    S_FLOAT("bow_draw_line_m", dlvr::BowDrawLineM, 0.05f, 0.50f, "Bow: how far off the string your hand may stray",
            "Controller Bindings",
            "While you hold the string, your hand only pulls it while it stays within this distance (metres) of the string's "
            "line. Further out the string stays where it was until your hand comes back."),
    S_FLOAT("bow_draw_cancel_m", dlvr::BowDrawCancelM, 0.0f, 0.20f, "Bow: cancel under this pull", "Controller Bindings",
            "Releasing the trigger with less than this much pull (metres) cancels the shot and keeps the arrow. 0 = never cancel."),
    // LOCKED ON (Jay 2026-10-17: "Tier A, ship" -> "Yes, hide & lock it too"): not drawn, the file value never read.
    S_BOOL_LOCKED("bow_manual_nock", dlvr::BowManualNock, "Bow: nock each arrow by hand", "Controller Bindings",
           "With a bow out, the bow does not reload itself. Reach over your right shoulder and press the right trigger to take an "
           "arrow; keep holding it, bring the arrow to the bow pointing the way it will fly, and push it back until it reaches "
           "the string. Let go, then grab the string to draw as usual. Let go early and the arrow goes back in the quiver. Needs "
           "'Bow: draw the string by hand'. On by default. Off = the bow reloads itself."),
    // The nock's five catch values are LOCKED (Jay 2026-10-17: "Tier A, ship. Hide the adjusted values aswell, they are not to be
    // touched"): not drawn, the file value never read - the compiled values are the verified ones (run 7: "Values are good.").
    S_FLOAT_LOCKED("bow_nock_catch_m", dlvr::BowNockCatchM, 0.05f, 0.40f, "Bow: arrow catch distance", "Controller Bindings",
            "How close (metres) the back of the arrow must come to the bow's line for the bow to catch it."),
    S_FLOAT_LOCKED("bow_nock_hold_m", dlvr::BowNockHoldM, 0.05f, 0.50f, "Bow: how far the arrow may stray once caught",
            "Controller Bindings",
            "Once caught, your hand may wander this far (metres) off the bow's line before the arrow is back in your hand."),
    S_FLOAT_LOCKED("bow_nock_seat_m", dlvr::BowNockSeatM, 0.02f, 0.20f, "Bow: arrow seat distance", "Controller Bindings",
            "The arrow seats on the string when its back comes within this distance (metres) of the string."),
    S_FLOAT_LOCKED("bow_nock_catch_deg", dlvr::BowNockCatchDeg, 15.0f, 90.0f, "Bow: how far the arrow may point off the bow's line",
            "Controller Bindings",
            "The arrow is only caught when it points within this many degrees of the way the bow shoots. A backwards arrow "
            "never catches."),
    S_FLOAT_LOCKED("bow_nock_catch_len_m", dlvr::BowNockCatchLenM, 0.10f, 0.75f, "Bow: how far in front of the string the arrow latches",
            "Controller Bindings",
            "The arrow only latches onto the bow's firing line within this distance (metres) in front of the string."),
    S_FLOAT_LOCKED("bow_nock_carry_back_m", dlvr::BowNockCarryBackM, 0.00f, 0.15f,
            "Rigging: bow - how far back the carried arrow sits in the hand", "Camera hook",
            "Metres the arrow you carry from the quiver sits further back along its own length than the game's reload pose. "
            "Higher = deeper in the hand. Works live."),
    S_FLOAT_LOCKED("bow_nock_carry_tilt_deg", dlvr::BowNockCarryTiltDeg, -60.0f, 60.0f,
            "Rigging: bow - carried arrow angle, tilt", "Camera hook",
            "Tilts the arrow you carry from the quiver about the nock (degrees), toward the palm or the back of the hand. 0 = the "
            "game's reload pose. Works live."),
    S_FLOAT_LOCKED("bow_nock_carry_turn_deg", dlvr::BowNockCarryTurnDeg, -60.0f, 60.0f,
            "Rigging: bow - carried arrow angle, turn", "Camera hook",
            "Turns the arrow you carry from the quiver about the nock (degrees), toward the thumb or the little finger. 0 = the "
            "game's reload pose. Works live."),
    S_FLOAT_LOCKED("bow_nock_carry_side_m", dlvr::BowNockCarrySideM, -0.05f, 0.05f,
            "Rigging: bow - carried arrow sideways in the hand", "Camera hook",
            "Slides the arrow you carry from the quiver sideways (metres), horizontally across its shaft - at right angles to the "
            "line of your knuckles. 0 = the game's reload pose. Works live."),
    S_FLOAT("holster_radius", HolsterRadius, 0.03f, 0.5f, "Holster radius", "Controller Bindings",
            "Enter radius in metres; the hand leaves the zone at 1.2x this."),
    S_FLOAT("holster_haptic", HolsterHaptic, 0.0f, 1.0f, "Holster pulse strength", "Controller Bindings",
            "Vibration strength for the zone and switch pulses. 0 = no pulses (the holster still works)."),
    S_INT("holster_tap_ms", HolsterTapMs, 20, 450, "Holster switch tap length (ms)", "Controller Bindings",
          "How long D-pad right is held when the shoulder holster fires. Under half a second is a weapon "
          "switch; holding longer opens the weapon wheel, so this stops at 450."),
    S_BOOL("holster_rhip_enable", HolsterRHipEnable, "Right-hip holster (item swap)", "Controller Bindings",
           "Squeeze right grip with the right hand at your right hip: taps D-pad left (swap item) instead of R1, "
           "the moment you squeeze. Right grip anywhere else is R1 exactly as before. Needs vr_grip=1."),
    S_FLOAT("holster_rhip_center_x", HolsterRHipCenterX, -0.8f, 0.8f, "Right hip holster centre X (right)",
            "Controller Bindings", "Zone centre, metres from the headset, +right (right hip is positive)."),
    S_FLOAT("holster_rhip_center_y", HolsterRHipCenterY, -1.2f, 0.5f, "Right hip holster centre Y (up)",
            "Controller Bindings", "Zone centre, metres from the headset, +up."),
    S_FLOAT("holster_rhip_center_z", HolsterRHipCenterZ, -0.8f, 0.8f, "Right hip holster centre Z (behind)",
            "Controller Bindings", "Zone centre, metres from the headset, +behind."),
    S_FLOAT("holster_rhip_radius", HolsterRHipRadius, 0.03f, 0.5f, "Right hip holster radius",
            "Controller Bindings", "Enter radius in metres; the hand leaves the zone at 1.2x this."),
    S_INT("holster_rhip_tap_ms", HolsterRHipTapMs, 20, 450, "Right hip holster tap length (ms)",
          "Controller Bindings", "How long D-pad left is held when the right hip holster fires."),
    S_INT("holster_wheel_hold_ms", HolsterWheelHoldMs, 300, 2000, "Holster wheel hold (ms)", "Controller Bindings",
          "Hold a holster this long to open the game's wheel; point with the other hand and let go to choose. A shorter "
          "press is the quick switch, at the release."),
    S_FLOAT("holster_wheel_centre_m", HolsterWheelCentreM, 0.01f, 0.15f, "Holster wheel centre radius",
            "Controller Bindings", "Metres on the wheel as you see it: a hand over the wheel's middle cancels and keeps the current item."),
    S_BOOL("holster_hip_enable", HolsterHipEnable, "Left-hip ammo pouch (manual reload)", "Controller Bindings",
           "With manual reload on: squeeze left grip with the left hand at your left hip to take a magazine or round for the gun "
           "in your right hand. With manual reload off the left hip does nothing. Heal is on the left chest. Needs vr_grip=1."),
    S_FLOAT("holster_hip_center_x", HolsterHipCenterX, -0.8f, 0.8f, "Hip holster centre X (right)",
            "Controller Bindings", "Zone centre, metres from the headset, +right (left hip is negative)."),
    S_FLOAT("holster_hip_center_y", HolsterHipCenterY, -1.2f, 0.5f, "Hip holster centre Y (up)",
            "Controller Bindings", "Zone centre, metres from the headset, +up."),
    S_FLOAT("holster_hip_center_z", HolsterHipCenterZ, -0.8f, 0.8f, "Hip holster centre Z (behind)",
            "Controller Bindings", "Zone centre, metres from the headset, +behind."),
    S_FLOAT("holster_hip_radius", HolsterHipRadius, 0.03f, 0.5f, "Hip holster radius", "Controller Bindings",
            "Enter radius in metres; the hand leaves the zone at 1.2x this."),
    S_INT("holster_hip_tap_ms", HolsterHipTapMs, 20, 450, "Heal tap length (ms)", "Controller Bindings",
          "How long D-pad down (heal) is held when the left chest holster fires."),
    S_BOOL("holster_lchest_enable", HolsterLChestEnable, "Left-chest holster (heal)", "Controller Bindings",
           "Squeeze left grip with the left hand on your left chest: taps D-pad down (heal) instead of L1, the moment you "
           "squeeze. Left grip anywhere else is L1 exactly as before. Needs vr_grip=1."),
    S_FLOAT("holster_lchest_center_x", HolsterLChestCenterX, -0.8f, 0.8f, "Chest holster centre X (right)",
            "Controller Bindings", "Zone centre, metres, +right (the left chest is negative)."),
    S_FLOAT("holster_lchest_center_y", HolsterLChestCenterY, -1.2f, 0.5f, "Chest holster centre Y (up)",
            "Controller Bindings", "Zone centre, metres, +up."),
    S_FLOAT("holster_lchest_center_z", HolsterLChestCenterZ, -0.8f, 0.8f, "Chest holster centre Z (behind)",
            "Controller Bindings", "Zone centre, metres, +behind (the front is negative)."),
    S_FLOAT("holster_lchest_radius", HolsterLChestRadius, 0.03f, 0.5f, "Chest holster radius", "Controller Bindings",
            "Enter radius in metres; the hand leaves the zone at 1.2x this."),
    S_BOOL("holster_hip_from_calib", HolsterHipFromCalib, "Hip holsters: place at your belt from the arm calibration",
           "Controller Bindings",
           "Both hip holsters (right = item swap and readying a throwable with R2, left = the ammo pouch) sit at your belt, measured from where "
           "your hands hung in the arm calibration's 'arms straight down' pose, instead of the fixed centres below. Needs a "
           "calibration done with the calibration screen on or after 15 Oct 2026, STANDING ('Calibrate my arms' at the top of "
           "the menu); without one the fixed centres apply. The zone size is unchanged. On by default."),
    S_FLOAT("holster_hip_belt_up_cm", HolsterHipBeltUpCm, 10.0f, 15.0f, "Hip holsters: height above your resting hand (cm)",
            "Controller Bindings",
            "How far above your hanging hand the belt zone sits. Only with the calibration placement on."),
    S_FLOAT("holster_hip_belt_fwd_cm", HolsterHipBeltFwdCm, 0.0f, 4.0f, "Hip holsters: forward of your resting hand (cm)",
            "Controller Bindings",
            "How far in front of your hanging hand the belt zone sits (at most 4 cm). Only with the calibration placement on."),
    S_BOOL("holster_head_enable", HolsterHeadEnable, "Left-head holster (torch)", "Controller Bindings",
           "Squeeze left grip with the left hand against the left side of your head: taps D-pad up (torch on/off) "
           "instead of L1, the moment you squeeze. Left grip anywhere else is L1 exactly as before. Needs "
           "vr_grip=1. DEFAULT OFF."),
    S_FLOAT("holster_head_center_x", HolsterHeadCenterX, -0.5f, 0.5f, "Head holster centre X (right)",
            "Controller Bindings", "Zone centre, metres from the headset, +right (left side is negative)."),
    S_FLOAT("holster_head_center_y", HolsterHeadCenterY, -0.5f, 0.5f, "Head holster centre Y (up)",
            "Controller Bindings", "Zone centre, metres from the headset, +up."),
    S_FLOAT("holster_head_center_z", HolsterHeadCenterZ, -0.5f, 0.5f, "Head holster centre Z (behind)",
            "Controller Bindings", "Zone centre, metres from the headset, +behind."),
    S_FLOAT("holster_head_radius", HolsterHeadRadius, 0.03f, 0.3f, "Head holster radius", "Controller Bindings",
            "Enter radius in metres; the hand leaves the zone at 1.2x this."),
    S_INT("holster_head_tap_ms", HolsterHeadTapMs, 20, 90, "Head holster tap length (ms)", "Controller Bindings",
          "How long D-pad up is held when the head holster fires. Stops at 90: holding D-pad up for 0.1 s is the "
          "game's fast-equip-item action."),
    S_BOOL("melee_r2_suppress", g_meleeR2Suppress, "Block R2 attack with melee", "Controller Bindings",
           "With a MELEE weapon (class WeaponVis) in the right hand, the game's R2 reads 0 - neither the VR trigger "
           "nor a real pad starts the game's own attack; the VR swing is the attack. Firearms, the bow and empty "
           "hands keep R2. The mod still reads the VR trigger. Default controller layout only. DEFAULT OFF."),
    S_BOOL("throw_enable", ThrowEnable, "VR melee throw", "Controller Bindings",
           "spec 2026-09-30-melee-throw-design. Hand above the shoulder line + pull the trigger readies the game's own "
           "melee throw; let go of the trigger mid-throw to throw, a let-go without throw speed cancels. Needs "
           "melee_swing_mode 1 or 2 and melee_r2_suppress on. HEADSET only. DEFAULT OFF."),
    S_FLOAT("throw_height_y", ThrowHeightY, -0.5f, 0.3f, "Throw ready height (m)", "Controller Bindings",
            "The hand must be at or above this, metres from the headset (+up), to ready a throw. A guess to start."),
    S_FLOAT("throw_trig_press", ThrowTrigPress, 0.2f, 0.95f, "Throw trigger press", "Controller Bindings",
            "The trigger counts as pulled at or above this."),
    S_FLOAT("throw_trig_release", ThrowTrigRelease, 0.05f, 0.8f, "Throw trigger release", "Controller Bindings",
            "The trigger counts as let go at or below this."),
    S_FLOAT("throw_release_speed", ThrowReleaseSpeed, 0.5f, 8.0f, "Throw release speed (m/s)", "Controller Bindings",
            "Forward hand speed at the let-go that throws. Slower cancels. A guess to start."),
    S_FLOAT("throw_swing_holdoff_ms", ThrowSwingHoldoffMs, 0.0f, 1000.0f, "Throw swing hold-off (ms)",
            "Controller Bindings", "The VR swing stays off this long after a throw, so the follow-through does not hit."),
    S_BOOL("throwable_hand_enable", dlvr::ThrowableHandEnable, "Throw grenades by hand", "Controller Bindings",
           "spec 2026-10-14-throwables-design (Build A). With a throwable selected: LT at the LEFT HIP readies it (the "
           "mod holds L2 for you), squeeze RT, swing and let go of RT to throw along your hand. LT at the hip again, a "
           "weapon or item switch, or opening a menu cancels. LT away from the hip does nothing while a throwable is "
           "selected. Uses the left hip holster's zone. HEADSET only. DEFAULT ON."),
    S_FLOAT("throwable_full_speed", dlvr::ThrowableFullSpeed, 0.5f, 10.0f, "Throwable full speed (m/s)",
            "Controller Bindings", "Hand speed that throws an item at its own full speed; faster throws go up to 1.25x."),
    S_FLOAT("throwable_still_speed", dlvr::ThrowableStillSpeed, 0.0f, 0.5f, "Throwable drop speed (m/s)",
            "Controller Bindings", "A let-go with the hand slower than this drops the item at your feet."),
    S_BOOL("throwable_star_hand_velocity", dlvr::ThrowableStarHandVelocity,
           "Throw stars, knives and axes with your hand's speed", "Controller Bindings",
           "With \"Throw grenades by hand\" on. Throwing stars, throwing knives and throwing axes fly the way you "
           "throw them, like grenades: in your hand's direction, harder and further the faster your arm moves; letting go "
           "with a still hand drops the item. Off = the game's own throw at its own speed. HEADSET only. On by default."),
    S_BOOL("loot_by_hand", dlvr::LootByHand, "Loot by hand (grip and hold on a body)", "Controller Bindings",
           "spec 2026-10-23-loot-by-hand-design. Put a hand on a dead body's torso, squeeze the grip and hold the hand still: "
           "the game's own search runs on THAT body and the loot goes into your inventory, exactly like holding X. Opening the "
           "grip early cancels. While a hand loots, that grip does not tackle / camouflage. X looting is unchanged. HEADSET "
           "only. DEFAULT ON."),
    S_FLOAT("loot_reach_m", dlvr::LootReachM, 0.05f, 0.6f, "Loot reach (m)", "Controller Bindings",
            "How close the hand must be to the body for the grip to loot."),
    S_FLOAT("loot_still_speed", dlvr::LootStillSpeed, 0.05f, 2.0f, "Loot: hand still below (m/s)", "Controller Bindings",
            "The hand counts as still below this speed. Moving faster before the search starts cancels that grip."),
    S_FLOAT("loot_settle_s", dlvr::LootSettleS, 0.0f, 1.5f, "Loot: settle time (s)", "Controller Bindings",
            "How long the hand must stay still on the body before the search starts."),
    S_BOOL("lid_by_hand", dlvr::LidByHand, "Open chests and trunks by hand", "Controller Bindings",
           "spec 2026-11-18-hand-open-lids-design. Needs \"Loot by hand\" on. Wooden chests, car trunks and lidded baskets: squeeze "
           "a grip with your hand on the lid and swing it open - your view and position are never moved (no walk-up, no kneel). "
           "Past the open point the game opens it and the lid finishes swinging; grab it again to close it (visual only). Grip an "
           "item inside to hold it; let go over your shoulder to put it in your inventory, anywhere else to put it back. Gas "
           "canisters, crates and bricks: hold a grip to carry one, let go to throw it. X does nothing on these containers and "
           "objects while this is on. HEADSET only."),
    S_FLOAT("lid_open_frac", dlvr::LidOpenFrac, 0.1f, 0.9f, "Lid: open point (fraction)", "Controller Bindings",
            "How far the lid must be swung, as a share of its full opening, before the game opens the container."),
    // group "VR Comfort" = BETA keeps the text; the HUD tab draws these three (overlay.cpp TabForSetting / DrawHudTab, trace 20261123_1730)
    S_BOOL("inworld_prompts", dlvr::InworldPrompts, "Prompts in the world", "VR Comfort",
           "trace 20261123_1356. The game's interaction prompt (Open, Take, Search, Use, Talk ...) is also shown as a small panel "
           "in the world, just above the object, facing you, in the game's own words and language. Doors, switches, containers, "
           "items and bodies show the grip icon; people, traders, lockpicking and breaking doors show X. Needs \"Loot by hand\" "
           "installed and the two prompt files in DW\\Data. HEADSET only."),
    S_FLOAT("ui_prompt_brightness", dlvr::InworldPromptBrightness, 0.05f, 4.0f, "Prompts in the world: brightness", "VR Comfort",
            "trace 20261123_1700. How bright the in-world prompt is (the game's prompt multiplied by this). Takes effect at once."),
    S_FLOAT("ui_prompt_size", dlvr::InworldPromptSize, 0.25f, 4.0f, "Prompts in the world: size", "VR Comfort",
            "trace 20261123_1700. How big the in-world prompt is: 1.0 = a one-line prompt about 15 cm wide. The same size on every "
            "headset. Takes effect at once."),
    S_FLOAT("ui_prompt_plate", dlvr::InworldPromptPlate, 0.0f, 1.0f, "Prompts in the world: backing", "VR Comfort",
            "trace 20261123_1830. How dark the backing behind the in-world prompt is: 1.0 = as the game darkens behind its own prompt, "
            "0 = none. Takes effect at once."),
    S_BOOL("body_grab", dlvr::BodyGrab, "Grab bodies by hand", "Controller Bindings",
           "Needs \"Loot by hand\" on. Squeeze a grip with your hand on a dead body away from its torso - an arm, a leg, the "
           "head - and that part follows your hand while the rest drags behind. On the torso, a still hand loots as before; "
           "move it and you drag instead. Let go and the body keeps its swing: drag fast and let go to fling it. Both hands can "
           "hold a body. While a hand holds a body its weapon is hidden and cannot attack. HEADSET only. DEFAULT ON."),
    S_FLOAT("body_grab_reach_m", dlvr::BodyGrabReachM, 0.05f, 0.6f, "Grab reach (m)", "Controller Bindings",
            "How close the hand must be to a body, away from its torso, for the grip to grab it."),
    S_FLOAT("body_grab_hz", dlvr::BodyGrabHz, 0.5f, 15.0f, "Grab: pull stiffness (Hz)", "Controller Bindings",
            "How quickly the grabbed part follows your hand. Higher = snappier, lower = looser."),
    S_FLOAT("body_grab_damping", dlvr::BodyGrabDamping, 0.1f, 2.0f, "Grab: damping", "Controller Bindings",
            "How much the grabbed part's bounce is calmed. 1 = no overshoot; lower = springier."),
    S_FLOAT("body_grab_max_accel", dlvr::BodyGrabMaxAccel, 2.0f, 200.0f, "Grab: strength (m/s^2)", "Controller Bindings",
            "The most your hand can pull on each kilogram of the body - how heavy a body feels. Lower = heavier."),
    S_FLOAT("body_grab_limb_max_accel", dlvr::BodyGrabLimbMaxAccel, 2.0f, 200.0f, "Grab: severed limb strength (m/s^2)",
            "Controller Bindings", "The same as Grab: strength, for a cut-off limb only - how heavy a severed limb feels. Lower = "
            "heavier."),
    S_FLOAT("body_grab_limb_turn_hz", dlvr::BodyGrabLimbTurnHz, 0.5f, 15.0f, "Grab: severed limb turn stiffness (Hz)",
            "Controller Bindings", "How quickly a held severed limb turns with your wrist. Lower = looser and steadier."),
    S_FLOAT("body_grab_limb_turn_damping", dlvr::BodyGrabLimbTurnDamping, 0.1f, 3.0f, "Grab: severed limb turn damping",
            "Controller Bindings", "How much a held severed limb's turning is calmed. 1 = no overshoot; higher = slower, steadier."),
    S_FLOAT("body_grab_hitbox_margin_m", dlvr::BodyGrabHitboxMarginM, 0.0f, 0.3f, "Grab: hitbox margin (m)", "Controller Bindings",
            "How far outside a body part's hitbox (or a severed limb's) your hand can be and still grab it."),
    // Hand climbing P1 (docs/design_climbing_overhaul.md §6, 2026-11-25): a prototype, OFF by default. Takes effect at once (the
    // start detour installs on the first player update that sees it on; off again = the detour passes everything through).
    // 2026-11-27 (tester: "add a new section to the L3 + R3 in-game mod menu, called Physical Parkour"): every climb_ row is in the
    // "Physical Parkour" group - overlay.cpp's Physical Parkour tab draws them in sections (DrawParkourTab), and BetaTextKept keeps
    // that group's text in the BETA build. Labels no longer repeat "Climb by hand:" (the tab and its section headings say it).
    S_BOOL("climb_by_hand", dlvr::ClimbByHand, "Climb ledges by hand (prototype)", "Physical Parkour",
           "PROTOTYPE. ON: the game no longer grabs ledges by itself when you look at one and hold jump. Instead, close a grip with "
           "your hand on a ledge to grab it - it also catches a ledge if you keep the grip closed while falling past it - and open "
           "both grips while hanging to let go. While hanging, the stick still shimmies and climbs up, and the jump button still "
           "jumps, as before. Wall runs, tic-tacs, vaults and step-overs are not affected. OFF: climbing is the game's own."),
    // 2026-11-28 (tester: the centre crosshair "doesn't really do anything at all in VR, it's just bothersome in the middle of the
    // screen"): filed here at the tester's request, drawn beside the master switch (overlay.cpp DrawParkourTab kTop). grappleaim.cpp.
    S_BOOL("hide_game_crosshair", dlvr::HideGameCrosshair, "Hide the game's centre crosshair", "Physical Parkour",
           "ON: the game's own crosshair (the dot in the middle of your view, and the weapon brackets around it) is kept off the "
           "screen. The VR aim markers stay: the 3D gun and bow reticles, the grapple's 3D icon (or its flat icon), and - if "
           "grapple_crosshair_marker is on - the dot that follows where your hand aims the grapple. Takes effect at once while "
           "fire_reticle_3d or grapple_crosshair_marker is on (fire_reticle_3d is on by default); otherwise after a restart."),
    S_FLOAT("climb_reach_m", dlvr::ClimbReachM, 0.10f, 0.60f, "Grab reach (m)", "Physical Parkour",
            "How close your hand must be to a ledge's edge for a grip to grab it."),
    S_INT("climb_catch_ms", dlvr::ClimbCatchMs, 0, 5000, "Catch time (ms)", "Physical Parkour",
          "After you close a grip, how long it keeps looking for a ledge - long enough to close your hand before you reach the ledge, "
          "or while you fall past one."),
    S_BOOL("climb_always_hang", dlvr::ClimbAlwaysHang, "Hang from high ledges", "Physical Parkour",
           "ON: grabbing a ledge above head height hangs from it, even when there is room to climb onto it - press jump (or later, pull) "
           "to climb up. OFF: the game decides, and climbs straight over whenever it can."),
    S_FLOAT("climb_hang_min_m", dlvr::ClimbHangMinM, 1.2f, 3.0f, "Hang from (m above your feet)", "Physical Parkour",
            "How high a ledge must be for 'hang from high ledges' to hang from it. Lower ledges are climbed over."),
    S_FLOAT("climb_auto_low_m", dlvr::ClimbAutoLowM, 0.0f, 2.0f, "Low walls up to (m)", "Physical Parkour",
            "With 'game climbs low walls' on: walls whose top is at most this high above your feet still climb the game's own way "
            "(look at them and jump), no hands needed. 0 = every ledge needs a hand."),
    S_BOOL("climb_hand_motion", dlvr::ClimbHandMotion, "Move with your hands while hanging", "Physical Parkour",
           "ON: while hanging from a ledge, your body follows your hands - pull down to rise, reach up to lower yourself. Move along "
           "a ledge hand over hand: grab further along with the free hand, then let go with the other. OFF: the hang holds still."),
    S_FLOAT("climb_pull_max_mps", dlvr::ClimbPullMaxMps, 1.0f, 8.0f, "Fastest hand pull (m/s)", "Physical Parkour",
            "The fastest your hands can move your body while hanging."),
    S_FLOAT("climb_release_m", dlvr::ClimbReleaseM, 0.2f, 1.0f, "Hand lets go beyond (m)", "Physical Parkour",
            "While hanging, each hand is held on the ledge point it grabbed. A hand pulled farther than this from it lets go of the "
            "ledge (close the grip again to re-grab). Never less than the reach + 0.1 m."),
    S_BOOL("climb_ledge_last", dlvr::ClimbLedgeLast, "Two ledges held: use the last one grabbed", "Physical Parkour",
           "When your hands hold two different ledges, climbing up uses the ledge you grabbed last. OFF: the average of the two."),
    S_BOOL("climb_stick_shimmy", dlvr::ClimbStickShimmy, "Stick shimmies while hanging", "Physical Parkour",
           "While hanging, the left stick sideways or up shimmies along the ledge (the game's own). OFF: move along ledges with your "
           "hands only; the stick only climbs up (with 'Stick forward climbs up')."),
    S_BOOL("climb_body_follows_view", dlvr::ClimbBodyFollowsView, "Body turns with your view while hanging", "Physical Parkour",
           "While hanging, your body keeps turning to face where you look (around corners too). Needs 'body follows head' on. OFF: the "
           "body stays facing the wall it grabbed."),
    S_BOOL("climb_view_rigid", dlvr::ClimbViewRigid, "Steady view while climbing", "Physical Parkour",
           "While climbing, the view stays at your standing eye height on the body and ignores what the hanging and climbing "
           "animations do to the character's head - only your body's movement and your own head move it. OFF: the view follows the "
           "character's head ('Smooth view while climbing' then softens it)."),
    S_BOOL("climb_hold_body", dlvr::ClimbHoldBody, "Upright body while climbing", "Physical Parkour",
           "While climbing, your character's spine, neck and head keep the upright pose they had when you grabbed, and the hips and "
           "the body's root stay in their standing pose under the view, instead of the hanging and climbing animations bending and "
           "lowering the body over the ledge. Your arms still follow your hands. Needs "
           "'keep your body still when you pull out a grenade or other throwable' on (it uses the same hold)."),
    S_BOOL("climb_view_smooth", dlvr::ClimbViewSmooth, "Smooth view while climbing", "Physical Parkour",
           "While climbing, the view glides after your character's head instead of snapping with the hanging and climbing animations. "
           "Your own head movement is never smoothed. Not used while 'Steady view while climbing' is on. OFF: the view sits exactly "
           "on the character's eyes."),
    S_BOOL("climb_pin_hands", dlvr::ClimbPinHands, "Hands stay on the ledge", "Physical Parkour",
           "While hanging, each hand that holds a ledge is drawn on the ledge where it grabbed, not where your controller is. OFF: the "
           "hands follow your controllers."),
    S_BOOL("climb_mantle_stick", dlvr::ClimbMantleStick, "Stick forward climbs up", "Physical Parkour",
           "While hanging, push the left stick forward to climb up onto the ledge (the game's own climb-up). The game never climbs up "
           "by itself while you hang (for example after a jump up). OFF: the stick does not climb up either."),
    S_BOOL("climb_fast_climb_up", dlvr::ClimbFastClimbUp, "Quick climb-up", "Physical Parkour",
           "A climb-up from a hang ends as soon as you are up on the top and past the edge, and you can move again. OFF: the game's "
           "whole climb-up (about 2.2 s) plays out first."),
    S_BOOL("climb_skip_reach", dlvr::ClimbSkipReach, "No reach when nothing is above", "Physical Parkour",
           "While hanging, pushing the stick up where there is no ledge above to move to and nothing to climb onto does nothing. OFF: "
           "the game plays its short reach up and drops back into the hang."),
    S_BOOL("climb_ladder_by_hand", dlvr::ClimbLadderByHand, "Ladders and poles by hand", "Physical Parkour",
           "Ladders, pipes and poles are solid things you grab anywhere and climb by pulling, like ledges - walking into one or "
           "pressing use no longer starts the game's ladder climb. OFF: ladders, pipes and poles are the game's own."),
    S_BOOL("climb_room_check", dlvr::ClimbRoomCheck, "Climb up only where you fit", "Physical Parkour",
           "Before climbing up from a hang, checks there is room for you on top. Room to stand: the normal climb-up. Room only to "
           "crouch: you climb up and stay crouched until there is room to stand. No room: you keep hanging. Small debris lying on "
           "the top does not count. OFF: the climb-up always runs, wherever it puts you."),
    S_BOOL("climb_skip_grab_anim", dlvr::ClimbSkipGrabAnim, "No grab animation", "Physical Parkour",
           "When a hand grabs a ledge, ladder or pole, the game's grab animation (which swings your body onto the ledge, sometimes "
           "through the wall) is skipped: you hang at once, and your body glides until your hand is on the spot it grabbed, at the "
           "speed below. OFF: the game's grab animation plays first."),
    S_FLOAT("climb_grab_move_mps", dlvr::ClimbGrabMoveMps, 0.5f, 4.0f, "Grab glide speed (m/s)", "Physical Parkour",
            "How fast your body moves after a grab until the hand that grabbed is on its spot. Lower = gentler, higher = snappier."),
    S_BOOL("climb_zipline_by_hand", dlvr::ClimbZiplineByHand, "Grab ziplines by hand", "Physical Parkour",
           "Close a hand on a zipline's cable and you ride it, wherever you are looking - no jump needed. The game's own mount (a "
           "jump at the line, or the button when its prompt would show) is off while this is on. OFF: only the game's way."),
    S_BOOL("grapple_rope_swing", dlvr::GrappleRopeSwing, "Grappling hook is a rope", "Physical Parkour",
           "Once the grappling hook catches, it no longer yanks you to it: it is a rope you hang and swing on, keeping your speed and "
           "momentum. Let go with the jump button (or by landing, or reaching the hook). Needs Hand climbing on. OFF: the game's own "
           "pull."),
    S_BOOL("grapple_trigger_pull", dlvr::GrappleTriggerPull, "Hold the trigger to pull", "Physical Parkour",
           "While on the rope, holding the pull hand's trigger pulls you toward the hook; let go of it and the pull stops. The pull adds to your swing, it does not replace it. OFF: the rope only swings."),
    S_BOOL("grapple_hold_to_swing", dlvr::GrappleHoldToSwing, "Hold the trigger to stay on the rope", "Physical Parkour",
           "Press and hold the grappling hand's trigger to throw the hook and swing; you stay on the rope until you let go of it (no "
           "drop-off after a few seconds, landing or jumping does not let go). The pull hand's trigger pulls you in (it does not "
           "attack while on the rope). OFF: the jump button, landing or reaching the hook lets go."),
    S_ENUM("grapple_aim_hand", dlvr::GrappleAimHand, "Aiming hand", "Physical Parkour",
           "The hand that aims the grappling hook: its reticle and where the hook flies. (With a gun out, the gun still aims it, as "
           "set by grapple_aim_from_gun.)", kLeftRight),
    S_ENUM("grapple_hand", dlvr::GrappleThrowHand, "Grappling hand", "Physical Parkour",
           "The trigger that throws the hook (and holds the rope, with \"Left trigger holds, right trigger pulls\" on). Right: while "
           "the hook is selected, the right trigger throws and the left trigger attacks.", kLeftRight),
    S_ENUM("grapple_pull_hand", dlvr::GrapplePullHand, "Pull hand", "Physical Parkour",
           "The trigger that pulls you along the rope (when pulling is on). If it is the grappling hand too: holding it part way "
           "swings, squeezing it all the way pulls.", kLeftRight),
    S_FLOAT("grapple_pull_mps", dlvr::GrapplePullMps, 3.0f, 30.0f, "Rope pull speed (m/s)", "Physical Parkour",
            "How fast holding the trigger pulls you toward the hook at most. It gets there in about half a second."),
    S_BOOL_R("grapple_unlock_skill", dlvr::GrappleUnlockSkill, "Grappling hook from the start", "Physical Parkour",
             "The grappling hook skill (Survivor tree) needs no Survivor rank and no skill point: take it in the skill menu "
             "whenever you like, from the start of the game. Takes effect at the next game start. OFF: the game's own rank 12."),
    S_FLOAT("climb_slide_mps", dlvr::ClimbSlideMps, 0.0f, 3.2f, "Slide down ladders and poles (m/s)", "Physical Parkour",
            "On a ladder or pole, hold on with a light grip (squeezed, but not all the way) and you slide down this fast; squeeze "
            "fully to stop. At the bottom you let go and stand. 0 = no sliding."),
    S_FLOAT("climb_slide_grip", dlvr::ClimbSlideGrip, 0.55f, 1.0f, "Slide below this grip", "Physical Parkour",
            "How hard you squeeze to hold still on a ladder or pole: a grip below this (but still closed) slides you down. Higher = "
            "you need a firmer squeeze to stay put."),
    S_BOOL("climb_jump_by_look", dlvr::ClimbJumpByLook, "Jump button jumps where you look", "Physical Parkour",
           "While hanging from a ledge, ladder or pole, the jump button jumps at the game's full jump: straight up when you look at "
           "the wall or pole, otherwise off it in the direction you look (only left / right / behind counts, not up / down). OFF: "
           "the game's own hang jump."),
    S_FLOAT("climb_jump_side_deg", dlvr::ClimbJumpSideDeg, 10.0f, 90.0f, "Jump up within (degrees)", "Physical Parkour",
            "For the jump button while hanging: looking within this many degrees of the wall or pole jumps straight up; further "
            "round than that pushes off toward your look."),
    S_BOOL("climb_mantle_auto", dlvr::ClimbMantleAuto, "Pull up to climb up", "Physical Parkour",
           "While hanging, pull yourself up until the ledge is low enough (the height below) and hold still there a moment: the game's "
           "own climb-up runs. A fast pull you let go of is a jump, not a climb-up."),
    S_FLOAT("climb_mantle_m", dlvr::ClimbMantleM, 0.0f, 1.6f, "Pull-up climb height (m)", "Physical Parkour",
            "With 'pull up to climb up' on: the climb-up starts once the ledge you hold is this far above your feet or less."),
    S_BOOL("climb_auto_low", dlvr::ClimbAutoLow, "Game climbs low walls", "Physical Parkour",
           "ON: walls up to the low-wall height still climb the game's way (look at them and jump). OFF: every ledge needs your hands."),
    S_FLOAT("climb_launch_min_mps", dlvr::ClimbLaunchMinMps, 0.0f, 4.0f, "Launch when letting go faster than (m/s)",
            "Physical Parkour",
            "Let go of the ledge while your hands are still pulling you fast and you keep flying: up to grab a higher ledge, or away "
            "from the wall if you push off it. Slower than this is a plain drop. 0 = never launch."),
    S_FLOAT("climb_launch_scale", dlvr::ClimbLaunchScale, 0.5f, 2.0f, "Launch boost", "Physical Parkour",
            "Multiplies your pull's speed into the launch, so a smaller motion jumps as far. Never more than the game's own jump: at "
            "most its default jump height (1.01 m) up and its wall push-off speed (3 m/s) sideways."),
    S_FLOAT("climb_launch_up_assist", dlvr::ClimbLaunchUpAssist, 0.0f, 1.0f, "Upward launch assist",
            "Physical Parkour",
            "Turns a mostly-upward launch toward straight up, as the game's own up jumps are. 0 = none (you jump the way you pulled), "
            "0.5 = half the angle (30 degrees off becomes 15), 1 = always straight up. Push-offs are never bent."),
    S_BOOL("climb_air_momentum", dlvr::ClimbAirMomentum, "Keep momentum in the air", "Physical Parkour",
           "After a launch you keep the speed you jumped with until you land: walls and ledges still stop you, and only air drag "
           "slows you down; the stick steers as much as 'Air control' allows. OFF: after a launch the stick steers in the air as "
           "well, never adding speed on top of the launch."),
    // round 16 (2026-11-27): replaced by climb_air_control_pct (a % instead of a share of the game's 1.06 %); locked, read by nothing
    S_FLOAT_LOCKED("climb_air_control", dlvr::ClimbAirControl, 0.0f, 1.0f, "Air control (old)", "Physical Parkour",
                   "Replaced by 'Air control (%)'."),
    S_FLOAT("climb_air_control_pct", dlvr::ClimbAirControlPct, 0.0f, 100.0f, "Air control (%)", "Physical Parkour",
            "How much the stick can steer you in the air while 'Keep momentum in the air' is on, added to your momentum. Each "
            "update the stick closes this % of the gap between how fast it would move you and how fast you already go its way. "
            "0 = none (pure momentum). The game's own air steering is about 1 %. The stick never pushes you faster than it would "
            "move you on its own."),
    S_BOOL("climb_air_momentum_all", dlvr::ClimbAirMomentumAll, "Keep momentum on every jump and fall", "Physical Parkour",
           "'Keep momentum in the air' for all air time - the game's own jumps and falls too, not only launches from a ledge. "
           "OFF: only launches; the game's own jumps steer with the stick as usual."),
    S_FLOAT("climb_air_drag", dlvr::ClimbAirDrag, 0.0f, 2.0f, "Air drag", "Physical Parkour",
            "How quickly a launch loses its sideways speed in the air, per second. 0 = none, 0.1 = about 10% in the first second. "
            "Only with 'Keep momentum in the air' on."),
    // REMOVED 2026-11-27 (tester: "remove the engine collision view and the raycast debug line draws ... delete the code that triggers
    // from them"): both features' code is gone. The keys stay LOCKED (AGENTS.md: a key is never dropped from the table) - never read
    // from the file, never drawn, pinned off; their variables (climbhand.cpp) are read by nothing.
    S_BOOL_LOCKED("climb_debug_draw", dlvr::ClimbDebugDraw, "Show the ledge search (removed)", "Physical Parkour",
                  "Removed: drew the ledge search's rays in the headset."),
    S_INT_LOCKED("climb_debug_physics", dlvr::ClimbDebugPhysics, 0, 255, "Engine collision view (removed)", "Physical Parkour",
                 "Removed: the released engine's physics debug view drew nothing."),
    S_BOOL("camo_gesture", dlvr::CamoGesture, "Camouflage by wiping a corpse", "Controller Bindings",
           "Hold an infected corpse, or a limb from one, and wipe it across your face to camouflage yourself - no skill needed. Once "
           "per corpse. Needs Grab bodies by hand. HEADSET only. DEFAULT ON."),
    S_FLOAT("camo_wipe_fwd_m", dlvr::CamoWipeFwdM, 0.05f, 0.6f, "Camouflage wipe: reach in front of face (m)", "Controller Bindings",
            "How far in front of your face the held body counts as at your face."),
    S_FLOAT("camo_wipe_radius_m", dlvr::CamoWipeRadiusM, 0.05f, 0.4f, "Camouflage wipe: face size (m)", "Controller Bindings",
            "How far left / right and up / down of your face's centre still counts as at your face."),
    S_FLOAT("camo_wipe_sweep_m", dlvr::CamoWipeSweepM, 0.03f, 0.4f, "Camouflage wipe: sideways distance (m)", "Controller Bindings",
            "How far sideways across your face the hand must travel for a wipe."),
    S_FLOAT("camo_wipe_max_s", dlvr::CamoWipeMaxS, 0.2f, 3.0f, "Camouflage wipe: time (s)", "Controller Bindings",
            "The wipe must cover that distance within this long of reaching your face."),
    S_BOOL("hand_collision", dlvr::HandCollision, "Hands stop on enemies", "Controller Bindings",
           "An empty hand (fists too) stops on a live zombie or hostile's body instead of passing through it, with one short buzz "
           "when it touches. A hand holding anything passes through as before. HEADSET only. DEFAULT OFF."),
    S_FLOAT("hand_collision_buzz_amp", dlvr::HandCollisionBuzzAmp, 0.0f, 1.0f, "Hand stop: buzz strength", "Controller Bindings",
            "How strong the buzz is when an empty hand touches an enemy. 0 = no buzz."),
    S_FLOAT("hand_collision_buzz_ms", dlvr::HandCollisionBuzzMs, 1.0f, 200.0f, "Hand stop: buzz length (ms)", "Controller Bindings",
            "How long the touch buzz lasts."),
    S_BOOL("enemy_grab_stiff_hold", dlvr::EnemyGrabStiffHold, "Grab: stiff, weightless hold", "Controller Bindings",
           "A zombie you grab keeps its shape and weighs nothing while you hold it, instead of hanging limp; it gets back up when you "
           "let go. Needs \"Shove enemies by hand\". Test feature. HEADSET only. DEFAULT OFF."),
    S_FLOAT("enemy_grab_stiff_force", dlvr::EnemyGrabStiffForce, 0.5f, 200.0f, "Grab: hold stiffness", "Controller Bindings",
            "How hard a held zombie's joints resist bending. Higher = stiffer."),
    S_FLOAT("enemy_grab_weight", dlvr::EnemyGrabWeight, 0.0f, 1.0f, "Grab: held zombie weight", "Controller Bindings",
            "How much of its weight a held zombie keeps. 0 = floats, 1 = its full weight."),
    S_BOOL("enemy_push_grab", dlvr::EnemyPushGrab, "Shove enemies by hand", "Controller Bindings",
           "An empty hand shoves a live zombie or hostile it touches, through the game's own hit: a shove staggers it back or aside "
           "the way your hand moves, a really hard shove knocks it down; squeeze the grip to grab it (limp) and let go to throw it. "
           "Needs \"Hands stop on enemies\", body grab and loot by hand. Single player only for the grab. HEADSET only. DEFAULT OFF."),
    S_FLOAT("push_min_mps", dlvr::PushMinMps, 0.2f, 6.0f, "Shove: minimum hand speed (m/s)", "Controller Bindings",
            "A hand moving slower than this into an enemy only touches it."),
    S_FLOAT("player_ai_helper_radius_m", dlvr::PlayerAiHelperRadiusM, 0.1f, 0.6f, "Enemies' keep-off radius (m)", "Controller Bindings",
            "The invisible ring the game puts around you to keep enemies off - smaller lets zombies come closer. The game's is 0.6. "
            "Applies to the ring the game builds next (load a save to apply at once). Needs \"Shove enemies by hand\"."),
    S_FLOAT("push_hard_mps", dlvr::PushHardMps, 0.5f, 10.0f, "Shove: hard shove speed (m/s)", "Controller Bindings",
            "Below this a shove only makes the enemy flinch; at or above it the enemy staggers back."),
    S_FLOAT("push_knockdown_mps", dlvr::PushKnockdownMps, 0.5f, 10.0f, "Shove: knock-down speed (m/s)", "Controller Bindings",
            "A shove at least this fast also knocks the enemy down."),
    S_FLOAT("push_rearm_s", dlvr::PushRearmS, 0.05f, 3.0f, "Shove: re-arm time (s)", "Controller Bindings",
            "How long before the same touch can shove again. Taking the hand off the enemy re-arms at once."),
    S_FLOAT("grab_break_free_s", dlvr::GrabBreakFreeS, 0.3f, 10.0f, "Grab: break-free time (s)", "Controller Bindings",
            "Squeeze the grip on an enemy you touch: the game knocks it down and it hangs limp from your hand. After this long it "
            "breaks free by itself."),
    S_FLOAT("grab_drop_mps", dlvr::GrabDropMps, 0.0f, 5.0f, "Grab: throw speed (m/s)", "Controller Bindings",
            "Let go with the hand moving at least this fast and the enemy is thrown that way; slower and it is just dropped."),
    S_FLOAT("push_grab_intent_squeeze", dlvr::PushGrabIntentSqueeze, 0.05f, 0.49f, "Shove: grip that means a grab", "Controller Bindings",
            "A hand squeezed at least this much when it would shove is reaching to grab - it does not shove."),
    S_FLOAT("push_zombie_melee_range_m", dlvr::PushZombieMeleeRangeM, 0.5f, 2.5f, "Zombies come this close (m)", "Controller Bindings",
            "Ordinary zombies walk in to this distance before they attack you, so your hands can reach them. Other enemies and "
            "zombie-vs-NPC fights are unchanged. Needs \"Shove enemies by hand\"."),
    S_FLOAT("push_grab_reach_m", dlvr::PushGrabReachM, 0.0f, 0.15f, "Shove / grab reach (m)", "Controller Bindings",
            "A hand this close to an enemy's body already counts as touching it for a shove or a grab. 0 = the hand must reach "
            "the body. Needs \"Shove enemies by hand\"."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out 2026-11-07 (Jay: tester package, "Off-by-default probes"); constexpr false in boneblend.h
    S_BOOL("diag_anim_lod", dlvr::DiagAnimLod,
           "Diagnostics: how often far zombies are animated + animation CPU", "Controller Bindings",
           "INV-682. Watches the engine's animation scheduler and every animation job (pass-through, changes "
           "nothing): per distance band, how many models were re-posed on each update against how many waited, "
           "the interval the engine asked for, and the CPU time the posing took (total, per update, per band, "
           "zombies apart from the rest). Live: the first update with it on installs the two watchers; off again "
           "stops the counting (the watchers stay, passing straight through). Uses body_grab's "
           "radius query to tell zombies apart. Log-only. Logs ANIMLOD lines in dlvr_ovr2xr.log every 5 s. "
           "DEFAULT OFF."),
#endif
    S_BOOL("breakout_physical", dlvr::BreakoutPhysical, "Shove off a grabbing zombie", "Controller Bindings",
           "When a zombie grabs you, one hard shove with both hands breaks free - no skill needed. A Volatile pin takes a struggle: "
           "each hard two-handed shove counts as one press. A and the kick button still work as before. HEADSET only. DEFAULT ON."),
    S_FLOAT("breakout_shove_speed_mps", dlvr::BreakoutShoveSpeedMps, 0.5f, 5.0f, "Shove speed (m/s)", "Controller Bindings",
            "How fast each hand must move for a shove to count. Lower = easier; higher = only a hard push counts."),
    S_BOOL("coop_pose", dlvr::CoopPose, "Show my arms to co-op players", "Controller Bindings",
           "Sends your head, hand and finger movement to other co-op players running this mod, so they see your arms move (and you "
           "see theirs once the receiving side is built). Both players need the same mod build. In progress - not yet tested with two "
           "players. HEADSET only. DEFAULT OFF."),
    S_BOOL("manual_reload", dlvr::ManualReload, "Reload the pistol by hand", "Controller Bindings",
           "Reload the pistol with your left hand: pull the magazine out, take a fresh one from your left hip, push it in, and rack "
           "the slide if it was fired empty. Also makes the left hip the ammo pouch. Other guns reload with X as before. In "
           "progress - not yet tested in the headset. HEADSET only. DEFAULT OFF."),
    S_FLOAT("reload_snap_m", dlvr::ReloadSnapM, 0.01f, 0.20f, "Magazine snap distance (m)", "Controller Bindings",
            "A fresh magazine brought this close to the grip snaps in. Higher = easier. Needs \"Reload the pistol by hand\"."),
    S_FLOAT("reload_align_deg", dlvr::ReloadAlignDeg, 5.0f, 90.0f, "Magazine snap angle (deg)", "Controller Bindings",
            "...and pointed within this many degrees of the grip. Higher = easier."),
    S_FLOAT("reload_pull_out_m", dlvr::ReloadPullOutM, 0.02f, 0.20f, "Magazine pull-out distance (m)", "Controller Bindings",
            "How far you pull the magazine down before it leaves the gun. Let go sooner and it goes back in."),
    S_FLOAT("reload_rack_pull_m", dlvr::ReloadRackPullM, 0.01f, 0.10f, "Slide rack distance (m)", "Controller Bindings",
            "How far your hand pulls the slide back to rack it after the gun was fired empty."),
    S_FLOAT("reload_grab_m", dlvr::ReloadGrabM, 0.02f, 0.20f, "Magazine / slide grab reach (m)", "Controller Bindings",
            "Your left hand this close to the magazine or the slide grabs it when you squeeze."),
    S_FLOAT("reload_flick_dps", dlvr::ReloadFlickDps, 100.0f, 2000.0f, "Double-barrel flick to close (deg/s)", "Controller Bindings",
            "How fast the muzzle must snap upward to flick the open double-barrel shut. Lower = easier."),
    // diag_duck_probe: OUTSIDE every DLVR_BETA_LEAN block on purpose (2026-10-25: a diag inside a lean block is compiled out of
    // BETA and the file's 1 is never read).
    // holster_rings (plan 2026-11-20-holster-pivot-posture-rings Task 5, Jay: "B, with a switch defaulting on"): OUTSIDE every
    // DLVR_BETA_LEAN block - it must exist in BETA (the [DEP] marker is at its declaration in holster.cpp).
    S_BOOL("holster_rings", dlvr::HolsterRings, "Show holsters", "VR Comfort",
           "A faint ring shows each chest and hip holster as your hand comes near it, so you can see where to grab. Off: no rings."),
    S_BOOL("diag_hip_capture", dlvr::DiagHipCapture,
           "Diagnostics: [HIPCAP] standing recording for the hip holsters (log-only)", "Aim",
           "Log-only. One line per frame to dlvr_hipcap.log: your head, both controllers' grip poses with grip and trigger, the "
           "play-space shifts and the holsters' body facing - the recording the hip holsters' new anchor is tested on. Writes "
           "nothing the game reads. HEADSET. DEFAULT OFF."),
    S_BOOL("diag_roomscale_probe", dlvr::DiagRoomscaleProbe,
           "Diagnostics: [ROOMSCALE] walk body + head-to-world map (log-only)", "Aim",
           "Log-only. Twice a second: the player's walk body (wanted velocity, its guards), the controller group, the control "
           "object's world position, the head we report and the camera's world position with the measured head-to-world yaw - "
           "what roomscale will use. Writes nothing. HEADSET for the map. DEFAULT OFF."),
    // Climbing P0 (docs/design_climbing_overhaul.md §6, 2026-11-25): NOT inside a DLVR_BETA_LEAN block on purpose - the hand-climbing
    // work needs it in the fork's normal (beta) build. Off by default, capped (climbprobe_logic.h).
    S_BOOL("diag_climb_probe", dlvr::DiagClimbProbe,
           "Log the game's own climbs (diagnostic)", "Physical Parkour",
           "Log-only. While a ledge grab, ladder / pipe / rope or zipline is active (and 0.6 s after): which controllers are active, the "
           "ledge grab's type, state, collision flag and velocity, its physics answers, the walk body's velocity and physics flags, the "
           "player's position and measured speed; one START and one END summary line per climb, and a line whenever the set of active "
           "controllers changes. Writes nothing. Capped at 8000 lines a session. DEFAULT OFF."),
    S_BOOL("diag_loot_why", dlvr::DiagLootWhy,
           "Diagnostics: [LOOTWHY] why a body gets no loot prompt (log-only)", "Aim",
           "Log-only. Twice a second while a body is near: the game's own gather run three ways (as the game / without the look "
           "cone / without the cone and the line-of-sight ray), every gate that blocks the loot prompt, and per body the distance, "
           "angles and tests the game compares. Writes nothing. HEADSET. DEFAULT OFF."),
    S_BOOL("diag_physcrouch_cycle", dlvr::DiagPhyscrouchCycle,
           "Diagnostics: physical crouch FLAT test (synthetic head)", "Aim",
           "FLAT TEST ONLY, never in the headset. With physical_crouch_enable on, replaces the head's height with a synthetic "
           "one - standing 1.70 m for 8 s, crouched 1.20 m for 4 s, repeating - so the crouch input can be tested without a "
           "headset. The [PHYSCROUCH] lines name it. DEFAULT OFF."),
    S_ENUM("melee_swing_mode", MeleeSwingMode, "VR swing attack", "Controller Bindings",
           "SPEC-VR-SWING-ATTACK-2026-09-29. 0 = off, nothing installed, melee exactly as before. 1 = DETECT: logs "
           "[SWING] FIRE/END for every right-hand swing with a melee weapon, never deals damage. 2 = HIT: a swing "
           "calls the game's own melee hit with the sweep taken from the controller. R2 attacks are never touched. "
           "HEADSET only, and needs the rig running (all three master enables).",
           kMeleeSwingMode),
    S_FLOAT("melee_swing_fire_speed", MeleeSwingFireSpeed, 0.5f, 10.0f, "Swing fire speed (m/s)",
            "Controller Bindings", "Hand speed that starts a swing. CPVR's shipped 2.5 to start."),
    S_FLOAT("melee_swing_arm_speed", MeleeSwingArmSpeed, 0.1f, 5.0f, "Swing arm speed (m/s)",
            "Controller Bindings", "The hand must slow below this before a new swing can start."),
    S_FLOAT("melee_swing_end_speed", MeleeSwingEndSpeed, 0.1f, 5.0f, "Swing end speed (m/s)",
            "Controller Bindings", "A swing ends when the hand slows below this."),
    S_FLOAT("melee_swing_window_ms", MeleeSwingWindowMs, 10.0f, 200.0f, "Swing speed window (ms)",
            "Controller Bindings", "Speed is measured over at least this long, never frame to frame."),
    S_FLOAT("melee_swing_max_ms", MeleeSwingMaxMs, 100.0f, 3000.0f, "Swing max length (ms)",
            "Controller Bindings", "A swing ends after this long whatever the hand does."),
    S_FLOAT("melee_swing_min_travel_m", MeleeSwingMinTravelM, 0.05f, 1.5f, "Swing min travel (m)",
            "Controller Bindings", "A fast hand only counts as a swing once it has travelled this far. Suppresses "
            "the arm's recoil after a slash, which otherwise fired a second swing (Stage 1 run 1: 11 FIRE for 5 "
            "slashes)."),
    S_BOOL("melee_swing_refuse_toward_body", MeleeSwingRefuseTowardBody, "Never swing toward body",
           "Controller Bindings", "A stroke moving back toward your body never counts as a swing - it is the arm "
           "returning or winding up. Off = only strokes within the return window after a swing are refused."),
    S_FLOAT("melee_swing_return_ms", MeleeSwingReturnMs, 0.0f, 3000.0f, "Return-stroke window (ms)",
            "Controller Bindings", "Only used with 'Never swing toward body' OFF: after a swing ends, a stroke "
            "moving back toward your body within this long does not fire. 0 turns this off."),
    S_FLOAT("melee_swing_return_fast_mps", MeleeSwingReturnFastMps, 0.0f, 15.0f, "Backswing speed (m/s)",
            "Controller Bindings", "Inside the return window, a stroke at least this fast is a backswing and counts; "
            "slower is your arm recoiling and does not. 0 turns this off. 4.5 by default."),
    S_FLOAT("melee_swing_reversal_deg", MeleeSwingReversalDeg, 30.0f, 180.0f, "Swing reversal angle (deg)",
            "Controller Bindings", "A swing ends when your hand changes direction by more than this within a moment. "
            "Curved swings keep going; a sharp backswing starts a new swing. 90 by default."),
    S_FLOAT("melee_swing_reversal_ms", MeleeSwingReversalMs, 0.0f, 300.0f, "Swing reversal look-back (ms)",
            "Controller Bindings", "How far back the direction change is measured. Longer catches smooth backswings as a "
            "new swing; too long splits wide curved swings. 100 by default."),
    S_FLOAT("melee_swing_return_deg", MeleeSwingReturnDeg, 10.0f, 90.0f, "Toward-body cone (deg)",
            "Controller Bindings", "How close to straight back toward your body a stroke must point to be refused. "
            "Wider catches more returns but risks real strikes aimed back past your own body."),
    S_FLOAT("melee_blade_pitch_deg", MeleeBladePitchDeg, -90.0f, 90.0f, "Blade tilt (deg)",
            "Controller Bindings", "Tilts the blade about the controller's side axis. 0 = straight out of the "
            "top of the fist (OpenXR grip -Z). Adjust live until swings land where the weapon looks."),
    S_FLOAT("melee_hit_margin_m", MeleeHitMarginM, 0.0f, 0.30f,
            "Rigging: melee hit reach past the blade tip (metres)", "Camera hook",
            "How far past your weapon's tip a swing still hits. The hit follows the weapon you see, from your hand to its "
            "tip, plus this much. 0.30 by default."),
    S_FLOAT("melee_min_reach_m", MeleeMinReachM, 0.0f, 1.20f,
            "Rigging: melee minimum reach from the hand (metres)", "Camera hook",
            "The shortest distance from your hand a swing still hits, along the weapon, so small weapons do not fall "
            "short. 0 turns it off. 0.65 by default."),
    S_FLOAT("melee_hit_thickness_m", MeleeHitThicknessM, 0.0f, 0.40f,
            "Rigging: melee hit thickness (metres)", "Camera hook",
            "How close beside the path of your blade a zombie still gets hit - the swing counts as a thick band, not a "
            "thin line. Also reaches this much past the tip. 0 turns it off. 0.20 by default."),
    S_FLOAT("melee_hit_back_m", MeleeHitBackM, 0.0f, 0.60f,
            "Rigging: melee hit reach behind the hand (metres)", "Camera hook",
            "How far back from your hand toward your elbow a swing still hits, so a zombie right up against you is "
            "not missed. 0 turns it off. 0.30 by default."),
    S_BOOL("melee_hit_factor_restore", MeleeHitFactorRestore, "Rigging: game hit scaling",
           "Camera hook", "Gives each hit the same strength scaling the game's own melee would have given it at that "
           "distance from your hand. Off by default."),
    S_BOOL("anim_suppress_melee_idle", AnimSuppressMeleeIdle, "No melee idle animations", "Controller Bindings",
           "INV-319. With a melee weapon out, the game switches its ready pose to an exhausted, alert ('Vigilent') or "
           "weapon-trick animation, which can pull your arm off the controller. This keeps the plain standing pose. "
           "Needs the VR swing attack on (mode 1 or 2). Melee only. Off by default."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_melee_swing_probe", DiagMeleeSwingProbe, "Swing per-frame detail", "Controller Bindings",
           "Per-frame [SWING] lines while a swing is active, budget 600. The FIRE/END/STATS lines do not need it."),
#endif
    S_BOOL("vr_menu_button", g_vrMenuButton, "Left menu button (Select / Start)", "Controller Bindings",
           "Left Touch controller's menu button (the only controller-side menu input Touch "
           "exposes - the right controller's system button is runtime-reserved). Press = SELECT "
           "(Sony touchpad, the inventory), sent on release. Hold 0.5 second = START (Sony OPTIONS)."),

    S_BOOL("vr_trigger_fire", g_vrTriggerFire, "VR trigger fires", "Camera",
           "Drive the R2 analog value the engine reads from Sony's pad SDK "
           "(scePadReadState) from the right-hand VR controller's own trigger, "
           "so a ranged weapon fires from the motion controller instead of "
           "needing a real Xbox controller or mouse. Works even with no "
           "physical pad connected at all - this hooks the engine's real "
           "input path directly, it doesn't need XInput to see anything."),

    S_ENUM("cam_hook_op", g_camHookOp, "Hook does", "Camera hook",
           "Observe writes nothing. Delta adds a fixed rotation and assumes no "
           "conventions - always the first thing to try. Level is the goal.",
           kHookOps),
    S_BOOL("cam_keep_head", CamHookKeepHead, "Keep headset tilt", "Camera hook",
           "Drop only the game's pitch and roll; put the headset's back on top. "
           "Without it the camera is levelled absolutely and looking up does "
           "nothing."),
    S_BOOL("vr_look_back", CamHookLookBack, "Look back (synthetic)", "Camera hook",
           "The game's own Y/Triangle action never actually turns the camera, so "
           "this is a from-scratch override: eases a 180-degree yaw in while Y is "
           "held, sprinting, and not in a vehicle, and back out the instant any of "
           "those stop being true. Never touches the game's own facing/movement "
           "state - purely visual. Off by default, same as every other input "
           "feature here before its first confirmed test."),
    S_FLOAT("vr_look_back_ease_ms", CamHookLookBackEaseMs, 50.0f, 1000.0f,
            "Look back ease time (ms)", "Camera hook",
            "How long the turn takes to fully engage, and separately to fully "
            "release - same rate both directions. Lower is snappier, higher is "
            "smoother."),
    S_FLOAT("vr_look_back_angle_deg", CamHookLookBackAngleDeg, 60.0f, 180.0f,
            "Look back angle (deg)", "Camera hook",
            "How far to turn at full engagement. A full 180 showed real "
            "culling/streaming holes on the far side - defaulting to something "
            "closer to a natural over-the-shoulder glance instead."),
    S_BOOL("diag_player_movemod", DiagPlayerMovementModule,
           "Diagnostic: find player's sprint state via weapon anchor", "Camera hook",
           "Bounded scan (background thread, gives up if never found) for the "
           "live WeaponFireControllerRight instance - a confirmed player-"
           "specific object - then checks its own nearby fields for a pointer "
           "to the owning entity, and logs m_MoveStyle/m_CurrentMoveStyle from "
           "its MovementModule once found. Several earlier, more expensive "
           "approaches under this same setting were tried and superseded - see "
           "CLAUDE.md. Only meant to be left on for this specific "
           "investigation, not normal play."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_kill_physics_debug", DiagKillPhysicsDebugRender,
           "Diagnostic: disable physics/navmesh debug render", "Camera hook",
           "One-shot attempt to call ILevel::SetPhysicsDebugRenderMode(0), on "
           "the theory a VR-only ground-plane line/dot artifact is the game's "
           "own debug-render subsystem leaking into the VR eye viewports. "
           "Gives up after one successful call or one failed bounded scan - "
           "see CLAUDE.md. Only meant for this specific investigation."),
    S_BOOL("diag_feed_sim_camera", DiagFeedSimCamera,
           "Diagnostic: feed real head pose to SetSimulationCamera", "Camera hook",
           "Experimental: calls ILevel::SetSimulationCamera every frame with "
           "the mod's own live head pose, on the theory the engine's "
           "streaming/LOD reference camera is separate from the render "
           "camera and has never been told where VR's headset actually is - "
           "unconfirmed, see CLAUDE.md."),
    S_BOOL("diag_eye_rotation_compare", DiagEyeRotationCompare,
           "Diagnostic: compare final eye rotation matrices", "Camera hook",
           "Purely observational - logs the angular difference between the "
           "two real stereo eyes' final rendered rotation every ~2s. Should "
           "be ~0 degrees (real stereo differs eyes by translation only, "
           "never rotation) - testing a stereo-rotation-mismatch theory for "
           "the VR-only ground-plane artifact. See CLAUDE.md."),
    S_BOOL("diag_eye_axis", DiagEyeAxis,
           "Diagnostics: [EYEAXIS] does the stereo baseline follow the view?", "Camera hook",
           ">>> INV-260, 2026-09-27: the engine places each eye at source.pos + e * source.right "
           "(sub_1802E5530) AFTER camhook has rotated the eye copies, so the stereo baseline "
           "follows the SOURCE camera, which camhook may or may not rotate. Once per frame this "
           "reads both eye cameras and compares the baseline (right eye minus left eye) with the "
           "left eye's right axis, against HEAD ROLL: a baseline that follows the view rolls with "
           "the head; one built on an unrotated source stays level. Per 1 s window and "
           "cumulatively: slopes of view roll, baseline roll and (baseline - view) roll against "
           "head roll, the horizontal angle between them, |baseline|. Decides whether the head-yaw "
           "decouple needs C5. HEADSET-ONLY. Log-only, no behaviour change. DEFAULT OFF."),
    S_BOOL("diag_watch_line_add", DiagWatchLineAdd,
           "Diagnostic: watch CLineListEx's add-line function", "Camera hook",
           "Live, bounded (10s, auto-stops) execution-breakpoint watch on "
           "the game's own debug-line-primitive factory function - logs "
           "every caller (RTTI-resolved) that reaches it while armed. Arm, "
           "then look at the ground-plane artifact within the 10s window. "
           "See CLAUDE.md."),
    S_BOOL("diag_position_mapping", DiagPositionMapping,
           "Rigging PHASE A: measure the OpenXR -> game position mapping", "Camera hook",
           "READ-ONLY, and the prerequisite for two-bone IK. IK needs a target in "
           "the space the bones live in, and nothing so far touches positions - "
           "the axis remap that works for rotations was derived for the bone's "
           "own frame and says nothing about how a world offset maps. So this "
           "assumes nothing: it expresses ONE physical quantity - where the right "
           "hand sits relative to the head - in both spaces, each in its own "
           "local frame, and logs them side by side. Hold your real hand where "
           "the virtual hand appears and the two vectors describe the same "
           "offset, which gives the axis permutation, the signs AND the scale "
           "from a single measurement. Scale matters on its own: nothing yet "
           "establishes that a game unit is a metre. See CLAUDE.md."),
#endif
    S_FLOAT("cam_yaw_smoothing_ms", CamHookYawSmoothingMs, 0.0f, 200.0f,
            "Rigging: smoothing on the OpenXR->game yaw (ms)", "Camera hook",
            "The yaw that positions the IK target. Its jitter was traced to the "
            "camera being levelled more often than the head pose refreshes - the "
            "frame-phase counter reads '100998 camera levelings used a head from "
            "the previous frame, of 100998', i.e. 100%. So the difference paired "
            "a moving camera against a frozen head, and jittered by the "
            "inter-sample motion: near zero at rest, 3-8.5 deg during turns. The "
            "structural fix is to publish only when the head actually changes; "
            "this filter takes the residual. Body yaw varies slowly so a little "
            "costs almost no lag. 0 disables it, which is how to see the raw "
            "signal again."),
    S_BOOL("rig_wrist", RigWrist,
           "Rigging: wrist - the hand takes the controller's rotation", "Camera hook",
           "Until this, the hand only inherited whatever the forearm was doing, "
           "which is why rolling the controller did nothing to the blade. The "
           "controller maps into game space by the same Ry(bodyYaw) the position "
           "already uses, composed with a GRIP constant - the fixed angle at "
           "which the controller sits in the hand, plus the bone's own axis "
           "convention. Captured once from the pose the game is already holding "
           "rather than guessed; hold B to recapture if the first one caught an "
           "awkward pose."),
    S_FLOAT("rig_yaw_camera_blend", RigYawCameraBlend, 0.0f, 1.0f,
            "Rigging: arm yaw from camera, not root", "Camera hook",
            "0 = the skeleton ROOT's yaw (every build before 2026-09-14), 1 = the "
            "camera's. MEASURED 2026-09-14: the root LAGS the camera on a stick "
            "turn - 143 turn windows, not one under the 5 deg threshold, median "
            "peak 64.5 deg, max 115.8. That rotates the controller offset most of "
            "the way around the body and swings BOTH arms the same way, which is "
            "the reported turning sway. It is fully present empty-handed, so it is "
            "NOT the held-item fault. Left at 0 by default because the root was "
            "chosen over the camera for a real reason - camhook's differenced yaw "
            "is noisier - so this trades a measured lag for an unmeasured noise "
            "and wants testing at 1.0 before it becomes the default. NOTE the "
            "[SWAY] probe goes tautological above 0: it reports the very "
            "difference this closes, so set this to 0 to measure the root again."),
    S_FLOAT("rig_shoulder_from_eye", RigShoulderFromEye, 0.0f, 1.0f,
            "Rigging: hang the shoulder off the eye, not the body", "Camera hook",
            "0 = the shoulder is rebuilt from the CLAVICLE, which is on the body "
            "(every build before 2026-09-14). 1 = it is hung off the EYE using a "
            "resting offset latched while standing still. MEASURED: the target is "
            "anchored to the eye while the shoulder is anchored to the body, so "
            "locomotion's body lean swings the eye-to-shoulder vector by 0.089 m "
            "moving forward and 0.165 m sideways, against 0.0008 m standing still "
            "- and all of it moves the arm's ORIGIN under a target that did not "
            "move, with the controller still. Left at 0 because S tracking the "
            "real body is not purely wrong: the clavicle's genuine contribution to "
            "reach is modelled deliberately, and pinning S to the eye discards it. "
            "Try 1.0 first to see the whole effect, then back off if reach suffers. "
            "It latches only while STANDING STILL and announces if it never does - "
            "an unlatched side is INERT, not broken."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_yaw_noise", DiagYawNoise,
           "Diag: how noisy is the camera yaw, actually?", "Camera hook",
           "Read-only, 1 Hz, right side only. Reports the per-tick step of the "
           "RAW camera yaw split by whether anything is moving - the STILL floor "
           "IS the noise, because standing still with no stick the camera yaw "
           "should not change at all. Reports MEAN as well as peak: a filter's "
           "time constant is set by the typical amplitude, and sizing it off one "
           "outlier is how smooth=0.02 (~550 ms) got chosen against a correction "
           "that is sustained for whole seconds. The root was chosen over the "
           "camera because the camera was 'noisy' - that was REASONING, never a "
           "measurement, and it has shaped three decisions since. Reads the "
           "SOURCE before any blend, so it stays valid during a filter sweep "
           "where [SWAY] and [YAWSTRAFE] both go tautological."),
    S_BOOL("diag_yaw_strafe", DiagYawStrafe,
           "Diag: does strafing move bodyYaw with no stick input?", "Camera hook",
           "Read-only, 1 Hz, PEAK-HELD, right side only (bodyYaw is one number "
           "per frame). Separates CAMERA turn rate from ROOT travel, so a window "
           "can be classified STRAFING with the camera STILL - the exact case "
           "[SWAY] throws away, because that probe marks turn windows by camera "
           "rate and a strafe with no stick reads as STILL. Tests whether a "
           "strafing character reorients its root while the camera does not, "
           "moving Ry(bodyYaw) and swinging the arm. Passing value: in sideways "
           "windows with the camera still, peak |bodyYaw-camYaw| within 5 deg AND "
           "peak per-tick bodyYaw step under 2 deg kills the hypothesis. NOTE it "
           "warns and goes TAUTOLOGICAL if rig_yaw_camera_blend is above 0 - set "
           "that to 0 to measure what the root actually does."),
    S_BOOL("diag_strafe", DiagStrafe,
           "Diag: does strafing lean the body under the arm?", "Camera hook",
           "Read-only, 1 Hz, PEAK-HELD, both arms. The target is anchored to the "
           "EYE while the shoulder is rebuilt from the CLAVICLE, which is on the "
           "BODY - so a lateral body lean moves the arm's origin under a target "
           "that did not move, and the arm swings with the controller perfectly "
           "still. Reports the eye->shoulder VECTOR decomposed in the body frame, "
           "because |eye-elem48| is a LENGTH and a lean rotates that vector "
           "without changing it - which is why this was never caught. Includes a "
           "sideways/forward travel signal from the skeleton root so strafe "
           "windows are measured, not assumed, and says STANDING STILL when a "
           "window holds no movement. Passing value: lateral spread within 0.005 m "
           "through real sideways travel kills the lean mechanism."),
    S_BOOL("diag_segment_len", DiagSegmentLen,
           "Diag: is the bicep-to-elbow segment changing length?", "Camera hook",
           "Read-only, 1 Hz, PEAK-HELD per second, both arms. Reports the min and "
           "max of element 49's and 53's local translations - which ARE the "
           "bicep-to-elbow and forearm lengths, since a bone's local translation "
           "is its offset from its parent. Element 48's equivalent is clamped by "
           "rig_restore_e48_len; 49 and 53 are NOT clamped and are never written "
           "by us, so this reads the ANIMATION. Passing value: a spread within "
           "0.0005 across a second containing a turn means the bone is not "
           "stretching and the swing is elsewhere. Millimetres or more IS the "
           "stretch. Needs the three master enables for g_getLocal to exist, and "
           "says so by name if it cannot read."),
#endif
    S_FLOAT("rig_yaw_camera_smooth", RigYawCameraSmooth, 0.02f, 1.0f,
            "Rigging: yaw correction smoothing", "Camera hook",
            "Only does anything when rig_yaw_camera_blend is above 0. 1.0 = NO "
            "smoothing, which reproduces the 2026-09-14 15:34 test exactly - less "
            "sway, but the IK jitter came back. Lower = smoother. The point: the "
            "root is SMOOTH BUT LATE and the camera is PROMPT BUT NOISY, so a "
            "plain blend buys the promptness and the noise together. This "
            "low-passes the CORRECTION instead - a stick turn is a sustained "
            "few-hundred-ms excursion and survives the filter, while per-frame "
            "camera noise does not. ~0.25 at a ~11 ms frame is a time constant "
            "near 40 ms. If jitter survives even at 0.05, the noise is SLOW "
            "wobble in the same band as the turn and no filter can separate them - "
            "the fix would then be at source in camhook."),
    S_FLOAT("rig_forearm_twist", RigForearmTwistShare, 0.0f, 1.0f,
            "Rigging: share of wrist roll given to the forearm", "Camera hook",
            "Pronation is distributed along a real forearm, so a rig that rotates "
            "only the hand pinches at the wrist - the candy-wrapper artefact. "
            "This hands a share of the roll to the forearm instead. The rig does "
            "carry dedicated *_foretwist bones for exactly this, but they sit "
            "outside the parent chain, so this achieves the same with bones "
            "already driven. NOT a free knob in either direction: all of it on "
            "the forearm rolls the elbow with the hand, since to us the forearm "
            "is one rigid bone and the roll is uniform along it rather than zero "
            "at the elbow; too little leaves the wrist doing something it "
            "anatomically cannot."),
    S_FLOAT("rig_wrist_flex_deg", RigWristFlexDeg, 0.0f, 120.0f,
            "Rigging: wrist flexion limit (palm inward)", "Camera hook",
            "Human range is 75-90 degrees. The four wrist limits are separate "
            "because the real envelope is strongly asymmetric - radial deviation "
            "is about a quarter of flexion - so one symmetric number either lets "
            "the hand fold sideways four times too far or throttles flexion to a "
            "quarter of its range."),
    S_FLOAT("rig_wrist_ext_deg", RigWristExtDeg, 0.0f, 120.0f,
            "Rigging: wrist extension limit (hand back)", "Camera hook",
            "Human range is 60-90 degrees. If flexion and extension look "
            "inverted in the headset, swap this value with rig_wrist_flex_deg - "
            "that covers the sign without a rebuild."),
    S_FLOAT("rig_wrist_ulnar_deg", RigWristUlnarDeg, 0.0f, 90.0f,
            "Rigging: ulnar deviation limit (toward the pinky)", "Camera hook",
            "Human range is 30-45 degrees. Swap with the radial value if the two "
            "come out inverted."),
    S_FLOAT("rig_wrist_radial_deg", RigWristRadialDeg, 0.0f, 90.0f,
            "Rigging: radial deviation limit (toward the thumb)", "Camera hook",
            "Human range is only 15-21 degrees - by far the tightest of the four, "
            "and the one a single symmetric cone gets most wrong."),
    // 2026-11-16, Jay: "put the sliders that the users wanted into the rig settings in VR comfort" (testers: "Weapon/wrist angle also
    // seems off") -> the six hand-angle dials move to VR Comfort > Hands & Arms (group "VR Comfort" keeps their text in BETA lean;
    // overlay.cpp isRig). Ranges, defaults and every reader unchanged (trace docs/traces/20261116_1750_grip_offset_sliders.md). The
    // pitch / yaw turn about the hand bone's own Y / Z after the authored rest (boneblend.cpp CorrectHandLocal) - their on-screen
    // direction is not named in the labels because it has not been seen; the roll is the twist about the bone axis.
    S_FLOAT("rig_grip_pitch_deg", RigGripPitchDeg, -180.0f, 180.0f,
            "Right hand angle: bend A (degrees)", "VR Comfort",
            "Turns your right hand (and what it holds) on the controller, one way. If the hand or weapon sits at the wrong angle "
            "to your real hand, try small amounts here, bend B and twist. 0 = as the mod draws it."),
    S_FLOAT("rig_grip_yaw_deg", RigGripYawDeg, -180.0f, 180.0f,
            "Right hand angle: bend B (degrees)", "VR Comfort",
            "Turns your right hand (and what it holds) on the controller, across the other way from bend A. 0 = as the mod "
            "draws it."),
    S_FLOAT("rig_left_grip_pitch_deg", RigLeftGripPitchDeg, -180.0f, 180.0f,
            "Left hand angle: bend A (degrees)", "VR Comfort",
            "Turns your left hand on the controller, one way. Set separately from the right hand. 0 = as the mod draws it."),
    S_FLOAT("rig_left_grip_yaw_deg", RigLeftGripYawDeg, -180.0f, 180.0f,
            "Left hand angle: bend B (degrees)", "VR Comfort",
            "Turns your left hand on the controller, across the other way from bend A. 0 = as the mod draws it."),
    S_FLOAT("rig_grip_roll_deg", RigGripRollDeg, -180.0f, 180.0f,
            "Right hand angle: twist (degrees)", "VR Comfort",
            "Twists your right hand about your forearm's line. 0 = as the mod draws it."),
    // The controller grip offset (trace docs/traces/20261116_1750_grip_offset_sliders.md; testers: "a grip offset slider would be very
    // helpful"; Jay: "Move the controller"): moves where the game takes each controller to be, in the controller's own frame - the
    // drawn hand, grabs, holsters, the wheel, the bow, the calibration and the wrist HUD all follow. Declared in controllers.cpp.
    S_FLOAT("rig_hand_offset_r_fwd_cm", RigHandOffsetRFwdCm, -10.0f, 10.0f,
            "Right hand position: forward / back (cm)", "VR Comfort",
            "Moves your right hand forward (+) or back (-) along the controller, if the drawn hand does not sit where your real "
            "hand is. Grabs, holsters and the wrist HUD move with it. Recalibrate your arms after a big change. 0 = no change."),
    S_FLOAT("rig_hand_offset_r_up_cm", RigHandOffsetRUpCm, -10.0f, 10.0f,
            "Right hand position: up / down (cm)", "VR Comfort",
            "Moves your right hand up (+) or down (-) relative to the controller. 0 = no change."),
    S_FLOAT("rig_hand_offset_r_right_cm", RigHandOffsetRRightCm, -10.0f, 10.0f,
            "Right hand position: right / left (cm)", "VR Comfort",
            "Moves your right hand to the right (+) or left (-) relative to the controller. 0 = no change."),
    S_FLOAT("rig_hand_offset_l_fwd_cm", RigHandOffsetLFwdCm, -10.0f, 10.0f,
            "Left hand position: forward / back (cm)", "VR Comfort",
            "Moves your left hand forward (+) or back (-) along the controller, if the drawn hand does not sit where your real "
            "hand is. Grabs, holsters and the wrist HUD move with it. Recalibrate your arms after a big change. 0 = no change."),
    S_FLOAT("rig_hand_offset_l_up_cm", RigHandOffsetLUpCm, -10.0f, 10.0f,
            "Left hand position: up / down (cm)", "VR Comfort",
            "Moves your left hand up (+) or down (-) relative to the controller. 0 = no change."),
    S_FLOAT("rig_hand_offset_l_right_cm", RigHandOffsetLRightCm, -10.0f, 10.0f,
            "Left hand position: right / left (cm)", "VR Comfort",
            "Moves your left hand to the right (+) or left (-) relative to the controller. 0 = no change."),
    S_BOOL("rig_left_arm", RigLeftArm,
           "Rigging: drive the LEFT arm too", "Camera hook",
           "Same solver, mirrored element indices (clavicle 9, upper 12, fore 13, "
           "hand 17, foretwist 14). Off by default so the working right arm cannot "
           "regress behind it. The left has its own grip constant, captured "
           "independently, because that is a property of how that hand holds its "
           "own controller."),
    S_FLOAT("rig_left_grip_roll_deg", RigLeftGripRollDeg, -180.0f, 180.0f,
            "Left hand angle: twist (degrees)", "VR Comfort",
            "Twists your left hand about your forearm's line. 0 = as the mod draws it."),
    S_BOOL("rig_left_mirror_deviation", RigLeftMirrorDeviation,
           "Rigging: swap ulnar/radial limits for the LEFT wrist", "Camera hook",
           "Ulnar is toward the pinky and radial toward the thumb, so on the left "
           "hand they sit on the opposite side in bone space and the sign pairing "
           "flips. Without this the left wrist gets radial's tight 18 degrees "
           "where it needs ulnar's 38 - the exact clipping just fixed on the "
           "right. On by default because the anatomy says so, off to test it."),
    S_BOOL("rig_left_wrist_swap_axes", RigLeftWristSwapAxes,
           "Rigging: LEFT wrist axis assignment", "Camera hook",
           "The right's assignment was determined empirically and is not "
           "guaranteed to carry over. Separate so the left can be settled the same "
           "way: if the left hand bends sideways where it should bend at the palm, "
           "flip this."),
    S_BOOL("rig_wrist_swap_axes", RigWristSwapAxes,
           "Rigging: swap which wrist axis is flexion vs deviation", "Camera hook",
           "Which of the hand's two off-axis directions is flexion and which is "
           "deviation depends on the rig's own axis convention, which is not "
           "documented and is a coin flip. If the hand bends sideways when it "
           "should bend at the palm, this is the fix."),
    S_FLOAT("rig_hand_len", RigHandLen, 0.0f, 0.30f,
            "Rigging: wrist-to-grip distance, metres", "Camera hook",
            "A controller is held in the palm, not at the wrist. The IK solves "
            "for the WRIST, so the target steps back from the controller by this "
            "much along the hand's own axis - otherwise the wrist lands where the "
            "grip should be and the hand can only make up the difference by "
            "bending, which is the kink at the cuff. Taken from the rig itself: "
            "r_handholder, the weapon socket, sits 0.0970 from r_hand. 0 restores "
            "the old behaviour."),
    S_FLOAT("rig_twist_to_twistbone", RigTwistToTwistBone, 0.0f, 1.0f,
            "Rigging: route pronation to the rig's own twist bone", "Camera hook",
            "To us the forearm is one rigid bone, so rolling it turns the elbow "
            "end as much as the wrist end - which is why the elbow twisted. A real "
            "forearm has zero pronation at the elbow and full at the wrist, and "
            "the rig carries r_foretwist at the far end for exactly that, with the "
            "mesh weighted between the two so it twists gradually. 1 is the "
            "anatomical routing; 0 is the old whole-bone roll."),
    S_FLOAT("rig_twist_chain", RigTwistChain, 0.0f, 1.0f,
            "Rigging: spread the forearm twist along the forearm (0 = all at the elbow)", "Camera hook",
            "The candy-wrapper at the elbow (INV-355). When you turn your palm, the whole twist is put on the "
            "forearm bone, so the arm's skin wrings in the few centimetres round the elbow. The arm model has "
            "its own twist bones along the forearm that the skin is attached to: one at the wrist, one in the "
            "middle. This slider moves the twist off the elbow and onto them - 1 = none at the elbow, the full "
            "turn at the wrist, part of it mid-forearm (next slider). The hand, the weapon and where it points "
            "do not change. Works live. Needs 'rig_subtree_rest' and 'rig_pin_foretwist' on and "
            "'rig_twist_to_twistbone' at 0, or it does nothing and the log's TWISTCHAIN line says which. "
            "1 by default (verified in the headset 2026-10-02)."),
    S_FLOAT("rig_twist_chain_mid", RigTwistChainMid, 0.0f, 1.0f,
            "Rigging: forearm twist - how much of the turn the middle of the forearm shows", "Camera hook",
            "Only with the slider above. 0.5 = the middle of the forearm turns half as far as the wrist, an "
            "even wring from elbow to wrist. Lower keeps the turn nearer the wrist; higher brings it up "
            "toward the elbow. Works live. 0.5 by default."),
    S_FLOAT("rig_twist_chain_max_deg", RigTwistChainMaxDeg, 10.0f, 180.0f,
            "Rigging: forearm twist - the most the forearm may turn (degrees)", "Camera hook",
            "Only with the forearm-twist slider above. A real forearm turns about 80 to 90 degrees each way; the "
            "hand tracking can ask for more. Up to three quarters of this value the forearm turns exactly as "
            "asked, then it eases off and never passes the value - whatever is left over is turned at the elbow "
            "instead, as the upper arm would. Lower = a stiffer forearm and more of the turn at the elbow. The "
            "hand still follows the controller exactly. Works live. 180 = no limit. 71.5 by default "
            "(set in the headset 2026-10-02)."),
    S_BOOL("rig_write_at_player_tail", RigWriteAtPlayerTail,
           "Rigging: write the arm at the player-update tail", "Camera hook",
           "The LATEST write point in the frame - gamedll+0xBFC170, the player "
           "entity's whole per-frame update tail, which also contains the "
           "per-hand IK request block nested inside it. We already write after "
           "FUN_1800dd7b0 and at FUN_1800d7380's entry; both are earlier, both "
           "show 92-95% survival, and the drawn arm is still wrong - so "
           "something between our last write and the render still runs. Four "
           "attempts at disabling that something have now failed (the anti-wall "
           "deformation, the elbow-pole cliff, blocking the arm bones, and a "
           "stale-model theory). The alternative disables nothing and writes "
           "at the outermost impose point, which is "
           "exactly this one. So have the last word instead of trying to "
           "win the argument. Once per frame, so it is a cold hook next to the "
           "blend's 19,000 calls a second. Watch TAIL writes in the periodic "
           "report and UNEXPLAINED in [DRAWN R]."),
    S_BOOL("rig_block_arm_bones", RigBlockArmBones,
           "Rigging: take the arm bones off the animation system", "Camera hook",
           "THE BOW FIX, and a different kind of fix from the geometric ones. "
           "Measured with a bow held: an animation pose sitting 89-94 degrees off "
           "ours, rewriting our own locals by up to 24 degrees - while our elbow "
           "pole never crosses zero and our plane normal never inverts. So the "
           "arm we solve is fine and something else is drawing over it. This "
           "calls the Block(i) primitive (engine+0x15C920) on r_upperarm, "
           "r_forearm and r_hand, which sets bit 0x4000000 on each bone's flags "
           "and takes it off the animation system - the same mechanism stage 4 "
           "proved lets a written pose both hold and render. Blocks ONCE per "
           "captured model, and every bone index is verified against the "
           "bone-to-element table before a single flag is written, because the "
           "three numbering systems in this rig have been conflated before. "
           "Turning it off unblocks, so the arm is handed back without a "
           "relaunch. Deliberately NOT the HackPABlender mask API, which applies "
           "the whole mask - true UNBLOCKS - and turned the character into a "
           "noodle by releasing 83 bones the game had legitimately blocked."),
    S_BOOL("rig_block_keep_visible", RigBlockKeepVisible,
           "Rigging: keep the arms visible when blocking them", "Camera hook",
           "Block(i) does TWO things: it sets the animation-block bit 0x4000000 "
           "on the bone, and it also flips bit 0x2 on that bone's mesh element - "
           "which is a RENDER bit. The first time rig_block_arm_bones was enabled "
           "it worked mechanically (3 of 3 bones on both sides, indices verified) "
           "and made both arms invisible. The earlier matrix-buffer probe had "
           "already measured that bit as normally clear on these elements "
           "(0x1200 / 0x2300 / 0x2200 for 48 / 49 / 53). This clears it back "
           "immediately after each Block call, leaving the animation block in "
           "place. Its own setting on purpose: if the arms come back AND the "
           "animation resumes competing, that says bit 0x2 was load-bearing for "
           "the block itself, which is worth learning rather than assuming. The "
           "[ARMBLOCK] line prints the before and after flags."),
    S_BOOL("rig_grip_use_baked", RigGripUseBaked,
           "Rigging: use the BAKED grip constant instead of the per-session capture",
           "Camera hook",
           "THIS IS WHAT MAKES THE RESTING POSE AUTOMATIC. Baked 2026-09-10 into "
           "kGripBaked[2][9] in boneblend.cpp: start the game, load a save, correct "
           "resting hands, no calibration step, no B press, identical for every user "
           "and every controller. It replaces a snapshot that was taken ONCE at spawn "
           "from whatever wrist angle happened to be held - which is the bug it fixes. "
           "`grip` contains NO per-user quantity (it is a pure rotation; hand size and "
           "arm length live in rig_upper_arm_len / rig_forearm_len / rig_hand_len), "
           "which is why a shipped constant is correct here and calibration is not. "
           "ACCURACY, HONESTLY: the two harvests disagree by 19.72 deg (right) and "
           "11.46 deg (left), and within one run spawn-vs-B-hold disagreed by 9.11 / "
           "1.85. `grip` is only pose-invariant to the extent our IK forearm tracks the "
           "real forearm, and the elbow-pole choice varies with arm pose - so this is a "
           "good estimator, not an exact value, and a careful straight-wrist capture "
           "would tighten it. It is still strictly better than the snapshot it replaces, "
           "which re-rolled that dice every session with no way to notice it was wrong. "
           "Turn OFF to fall back to the per-session capture - the escape hatch if a "
           "runtime reports a grip pose that does not match the OpenXR convention the "
           "constant was derived under. The capture still RUNS either way, because "
           "[GRIPMTX]/[GRIPORTHO]/[BONEREST]/[TWISTREST] latch there; with this on, its "
           "result is simply overwritten by the constant, so those stay available for "
           "refining the bake."),
    S_BOOL("rig_grip_compose_rest", RigGripComposeRest,
           "Rigging: compose the grip capture with the hand's AUTHORED rest local",
           "Camera hook",
           "THE GRIP CAPTURE CONTAINED AN UNCHECKED ASSUMPTION AND IT WAS WRONG BY 77 DEGREES. "
           "Capture forces handWorld := forearmWorld, which is the same as forcing element 53's "
           "LOCAL to IDENTITY - i.e. 'the hand is exactly in line with the forearm'. Measured "
           "2026-09-10 via [BONEREST], both sides: the authored rest local is 77.0 deg from "
           "identity, decomposing as a 13.74 deg axis tilt plus a roll of +75.12 (right) / "
           "-75.12 (left) about the bone axis. So rig_grip_roll_deg=90 has been a hand-tuned "
           "scalar standing in for a 75.12 deg authored roll AND for a 13.74 deg tilt that no "
           "roll about the bone axis can express at all - which is exactly why the wrist has "
           "always been 'mostly correct' and never right. "
           "With world_child = world_parent . local_child, the neutral pose wants "
           "handWorld = forearmWorld . L_rest, so grip_true = grip_captured . L_rest, "
           "right-multiplied. Both are proper rotations ([GRIPORTHO] det +1.0000, unit columns, "
           "and the latch refuses anything else), so no orthonormalising is needed. "
           "This removes the SYSTEMATIC error only - the 3-12 deg capture reproducibility floor "
           "remains until kGripBaked is populated from a [GRIPMTX] line. "
           "Default ON: it is a derived correction, not an experiment, and it no-ops safely if "
           "the rest basis never latched (the log says so). Turn it off only to A/B against the "
           "old wrong behaviour. EXPECT rig_grip_roll_deg to want 0 once this is on, since "
           "L_rest now carries the real roll - expected, NOT confirmed: it is applied by "
           "CorrectHandLocal in a different frame, downstream of the swing/twist split."),
    S_BOOL("rig_two_hand_grip", RigTwoHandGrip, "Two-handed grip (rifles, shotguns, two-handed melee)", "Controller Bindings",
           "Hold the LEFT grip with your left hand on a rifle's or shotgun's barrel to hold it with both hands: the "
           "gun then points along the line between your hands, and your shots follow it. Let go of the grip to go back "
           "to one hand. While you grip, the left grip is not L1. Two-handed melee weapons: see the melee settings. "
           "Not the bow, the crossbow or one-handed weapons. Needs vr_grip=1. Headset only. On by default."),
    S_FLOAT("rig_two_hand_gun_radius", RigTwoHandGunRadius, 0.02f, 0.30f, "Two-handed grip: grab distance",
            "Controller Bindings",
            "How close (metres) your left hand must be to the line the gun points along to grab it. Kept small so "
            "reaching past the gun does not grab it."),
    S_FLOAT("rig_two_hand_gun_min", RigTwoHandGunMin, 0.0f, 0.5f, "Two-handed grip: nearest grab point",
            "Controller Bindings", "How far in front of your right hand (metres) the grab area starts."),
    S_FLOAT("rig_two_hand_gun_max", RigTwoHandGunMax, 0.1f, 1.2f, "Two-handed grip: farthest grab point",
            "Controller Bindings", "How far in front of your right hand (metres) the grab area ends."),
    S_FLOAT("rig_two_hand_slide_s", RigTwoHandSlideS, 0.0f, 1.0f, "Two-handed grip: settle time (seconds)",
            "Controller Bindings",
            "How long the gun takes to move into and out of the two-handed hold. 0 = instant."),
    S_BOOL("rig_two_hand_pin", RigTwoHandPin, "Two-handed grip: lock the left hand onto the gun", "Controller Bindings",
           "While you two-hand a rifle or shotgun, your left hand is drawn on the gun's own handguard grip and stays "
           "there instead of following your controller. Off = the left hand stays where your controller is."),
    S_FLOAT("rig_two_hand_pin_radius", RigTwoHandPinRadius, 0.03f, 0.30f,
            "Two-handed grip: grab distance from the handguard", "Controller Bindings",
            "How close (metres) your left hand must be to the gun's handguard grip to grab it."),
    S_FLOAT("rig_two_hand_haptic", RigTwoHandHaptic, 0.0f, 1.0f, "Two-handed grip: vibration strength",
            "Controller Bindings",
            "Strength of the buzz when your hand nears the grab spot, when you grab, and on each shot while two-handing. "
            "0 = no vibration."),
    S_BOOL("rig_two_hand_palm", RigTwoHandPalm, "Two-handed grip: hold the handguard in the palm", "Controller Bindings",
           "While your left hand is locked onto the gun, the middle of your palm sits on the handguard. Off = the base of "
           "the fingers sits there instead (the hand sits further back and the fingers go into the gun)."),
    S_FLOAT("rig_two_hand_wrist_overflow_deg", RigTwoHandWristOverflowDeg, 0.0f, 90.0f,
            "Two-handed grip: extra wrist bend allowed (degrees)", "Controller Bindings",
            "While you two-hand a gun, how far past its normal limit the right wrist may bend so the gun follows both "
            "hands. Never less than the one-handed setting; at most 90."),
    S_BOOL("rig_two_hand_forend", RigTwoHandForend, "Two-handed grip: hold shotguns at the middle of the forend",
           "Controller Bindings",
           "Shotguns: your left hand grips the middle of the pump / forend instead of the game's own grip point. Off = the "
           "game's grip point."),
    S_BOOL("rig_two_hand_rifle_pose", RigTwoHandRiflePose, "Two-handed grip: standard left-hand angle on rifles",
           "Controller Bindings",
           "Rifles: when you grab the handguard, your left hand settles into the set angle that wraps the handguard (palm "
           "up, fingers clear of the barrel). Off = the hand keeps the angle your controller had when you grabbed."),
    S_FLOAT("rig_two_hand_slide_len", RigTwoHandSlideLen, 0.0f, 0.40f,
            "Two-handed grip: rifle handguard grab length (metres)", "Controller Bindings",
            "Rifles: how long the grab area along the handguard is. Your left hand locks on wherever you grab inside it. "
            "0 = one fixed grab point."),
    S_FLOAT("rig_two_hand_zone_x", RigTwoHandZoneX, 0.03f, 0.30f,
            "Two-handed grip: rifle handguard grab reach sideways (metres)", "Controller Bindings",
            "Rifles: how far to the side of the handguard your left hand can be and still grab it."),
    S_BOOL("rig_two_hand_pin_reach", RigTwoHandPinReach, "Two-handed grip: left hand reaches the grip spot fully",
           "Controller Bindings",
           "While your left hand is locked onto the gun, the arm-reach shortening is not applied to it, so the hand lands "
           "on the spot it locked to. Off = the hand stops a few centimetres short."),
    S_BOOL("rig_two_hand_zone_drawn", RigTwoHandZoneDrawn, "Two-handed grip: grab where you see your hand",
           "Controller Bindings",
           "The grab area is checked against your in-game left hand, so grabbing happens where you see the hand on the gun. "
           "Off = checked against the controller's own position."),
    S_FLOAT("rig_two_hand_rifle_palm_x_cm", RigTwoHandRiflePalmXCm, -6.0f, 6.0f,
            "Two-handed grip: rifle palm sideways (cm)", "Controller Bindings",
            "Rifles: moves where your left palm sits on the handguard, sideways. Nudge it if the fingers clip the gun."),
    S_FLOAT("rig_two_hand_rifle_palm_y_cm", RigTwoHandRiflePalmYCm, -6.0f, 6.0f,
            "Two-handed grip: rifle palm up / down (cm)", "Controller Bindings",
            "Rifles: moves where your left palm sits on the handguard, up (+) or down (-)."),
    S_BOOL("rig_two_hand_melee", RigTwoHandMelee, "Two-handed grip: melee weapons", "Controller Bindings",
           "Two-handed melee weapons (axes, sledgehammers, bats and the like): hold left grip with your left hand on the handle "
           "below your right hand and the weapon slides down to a low two-handed grip, like a baseball bat. Let go and it slides "
           "back. Your right hand still aims. Needs the two-handed grip on."),
    S_FLOAT("rig_two_hand_melee_radius", RigTwoHandMeleeRadius, 0.03f, 0.30f,
            "Two-handed grip: melee grab distance (metres)", "Controller Bindings",
            "How close your left hand must be to the handle, below your right hand, to grab it."),
    S_FLOAT("rig_two_hand_melee_butt_cm", RigTwoHandMeleeButtCm, -40.0f, 0.0f,
            "Two-handed grip: melee grab area bottom (cm)", "Controller Bindings",
            "How far down the handle (below the game's swing grip point) the melee grab area reaches. More negative = further "
            "toward the end of the handle."),
    S_FLOAT("rig_two_hand_melee_stack_cm", RigTwoHandMeleeStackCm, 4.0f, 20.0f,
            "Two-handed grip: melee left hand gap (cm)", "Controller Bindings",
            "How far below your right hand your left hand sits on the handle."),
    S_FLOAT("rig_two_hand_melee_curl_deg", RigTwoHandMeleeCurlDeg, 0.0f, 60.0f,
            "Two-handed grip: melee finger curl (degrees)", "Controller Bindings",
            "How tightly your left fingers wrap the handle. 30 wraps it; 45 is close to a fist."),
    S_BOOL("rig_two_hand_melee_pose", RigTwoHandMeleePose, "Two-handed grip: standard left-hand angle on melee",
           "Controller Bindings",
           "Melee: when you grab the handle, your left hand takes the set grip angle. Off = the hand stays on the handle but "
           "turns with your controller (used to capture a better angle)."),
    S_FLOAT("rig_two_hand_melee_roll_deg", RigTwoHandMeleeRollDeg, -90.0f, 90.0f,
            "Two-handed grip: melee left hand around the handle (degrees)", "Controller Bindings",
            "Melee: turns your left hand around the handle while keeping the palm on it. Raise or lower it until the hand sits "
            "where you would hold it; positive and negative turn it opposite ways round the handle."),
    S_FLOAT("rig_two_hand_melee_wrist_cm", RigTwoHandMeleeWristCm, 0.0f, 8.0f,
            "Two-handed grip: melee handle across the fingers (cm)", "Controller Bindings",
            "Melee: how far the handle sits from the heel of your left palm toward your fingers. Higher = the handle sits further "
            "across the base of the fingers."),
    S_FLOAT("rig_two_hand_tune_x_cm", RigTwoHandTuneXCm, -10.0f, 10.0f,
            "Two-handed grip tuning: move X (cm)", "Controller Bindings",
            "Tuning: moves a two-handed melee weapon in your right hand along the hand's X axis while you hold it two-handed."),
    S_FLOAT("rig_two_hand_tune_y_cm", RigTwoHandTuneYCm, -10.0f, 10.0f,
            "Two-handed grip tuning: move Y (cm)", "Controller Bindings",
            "Tuning: moves a two-handed melee weapon in your right hand along the hand's Y axis while you hold it two-handed."),
    S_FLOAT("rig_two_hand_tune_z_cm", RigTwoHandTuneZCm, -10.0f, 10.0f,
            "Two-handed grip tuning: move Z (cm)", "Controller Bindings",
            "Tuning: moves a two-handed melee weapon in your right hand along the hand's Z axis while you hold it two-handed."),
    S_FLOAT("rig_two_hand_tune_rx_deg", RigTwoHandTuneRxDeg, -90.0f, 90.0f,
            "Two-handed grip tuning: turn about X (degrees)", "Controller Bindings",
            "Tuning: turns a two-handed melee weapon in your right hand about the hand's X axis while you hold it two-handed."),
    S_FLOAT("rig_two_hand_tune_ry_deg", RigTwoHandTuneRyDeg, -90.0f, 90.0f,
            "Two-handed grip tuning: turn about Y (degrees)", "Controller Bindings",
            "Tuning: turns a two-handed melee weapon in your right hand about the hand's Y axis while you hold it two-handed."),
    S_FLOAT("rig_two_hand_tune_rz_deg", RigTwoHandTuneRzDeg, -90.0f, 90.0f,
            "Two-handed grip tuning: turn about Z (degrees)", "Controller Bindings",
            "Tuning: turns a two-handed melee weapon in your right hand about the hand's Z axis while you hold it two-handed."),
    // 2026-11-08: "VR Comfort" so the BETA menu draws it (BETA draws settings by group only on that tab) - a feel choice per player.
    // LOCKED AT 0.75 2026-11-09 (Jay: "Also lock & hide the setting for the left hand follow on two handing firearms") - never read
    // from the file, never drawn; ships at the compiled 0.75 (= Jay's disk, RUN TH3).
    S_FLOAT_LOCKED("rig_two_hand_aim_share", RigTwoHandAimShare, 0.0f, 1.0f, "Two-handed guns: left hand's share of the aim",
            "VR Comfort",
            "While you hold a rifle or shotgun with both hands, your right wrist aims it as it does one-handed, and the line "
            "between your hands pulls the barrel toward your left hand by this share. 0 = your right wrist alone aims (the left "
            "hand only holds on). 1 = only the line between your hands aims. In between, both hands steer. Changes apply at once."),
    // LOCKED OFF 2026-11-08 (Jay: "Yes, hide it too ... unfinished & hidden, we'll do further research into it another time"; hiding
    // is a lock, 2026-10-18) - never drawn, never read from the file. RUN BT1 / BT2: VD forwarded no arm joint positions. To resume:
    // S_BOOL again (and its group decides where it is drawn).
    S_BOOL_LOCKED("rig_elbow_body_tracking", RigElbowBodyTracking, "Elbows follow body tracking (if your headset supports it)",
           "VR Comfort",
           "When your headset's body tracking reaches the PC, your in-game elbows point where your real elbows are. Your hands "
           "stay on the controllers. On a Quest with Virtual Desktop, switch on \"Forward tracking data\" in Virtual Desktop's "
           "streaming settings and use its VDXR runtime. Without body tracking, or while it loses your arm, the elbows use the "
           "normal placement. The elbow is still kept out of your view. Works live, but the game must be started with the "
           "headset's body tracking available."),
    S_BOOL("diag_twohand_record", DiagTwoHandRecord, "Diagnostics: record two-handed gun holds", "Controller Bindings",
           "Writes every frame of each two-handed gun hold to dlvr\\twohand_rec.bin for offline replay. Off by default."),
    S_BOOL("rig_hand_grip_pose", RigHandGripPose,
           "Rigging: drive the hand from OpenXR's GRIP pose, not its AIM pose",
           "Camera hook",
           "OpenXR reports TWO standard poses per controller and they are not "
           "interchangeable. `aim` is a POINTING RAY, deliberately angled off the "
           "hand and tuned per controller model so pointing feels natural - "
           "correct for shooting, which still uses it. `grip` is ANATOMY: origin "
           "at the palm centroid, orientation tied to how the hand wraps the "
           "grip. The rig had been using `aim` for both jobs. "
           "WHY IT MATTERS BEYOND TIDINESS: the fixed controller-to-hand-bone "
           "rotation (`grip` in boneblend.cpp) is only a UNIVERSAL constant if "
           "the controller frame is itself anatomy-defined. Fed by the aim pose "
           "it silently absorbs each controller's pointing-ray tilt, which "
           "differs between Touch, Index and Vive BY DESIGN - so no single baked "
           "value can be right for every user and the grip keeps needing per-user "
           "calibration. Fed by the grip pose it is one constant for everyone, "
           "which is what lets the spawn-time snapshot be deleted outright. "
           "It also makes an existing assumption true: the wrist step-back "
           "already assumes a PALM-CENTRED controller origin (stepping back by "
           "rig_hand_len to put the wrist behind the grip), which is the grip "
           "pose's definition and is not where the aim pose sits. "
           "NOT the grip BUTTON - that is input/squeeze/value, mapped to L1/R1, "
           "and is completely untouched by this. Falls back to the aim pose if "
           "the runtime never locates a grip pose, so turning this on cannot "
           "cost the hands. Check the 'controllers: grip pose' log line for "
           "whether it actually came up, and [GRIPCAL]'s 'pose source' field "
           "before baking anything."),
    S_BOOL("rig_regrip_button", RigRegripButton,
           "Rigging: hold B for 1.2 s to redefine the neutral grip on both hands",
           "Camera hook",
           "THE NEUTRAL GRIP WAS A ONE-SHOT SNAPSHOT WITH NO WAY TO REDEFINE IT. "
           "It is latched on the first tick that resolves - measured at 12 ms "
           "after the rig arms - as controller-inverse times forearm, i.e. "
           "'the hand is exactly in line with the forearm right now'. So whatever "
           "orientation your controllers happened to be in at that instant became "
           "the permanent definition of a straight wrist. The wrist's own log line "
           "has promised 'Hold B to re-neutralise at any time' since it was "
           "written, and nothing in the codebase ever cleared the latch - the "
           "message was simply false. This implements it. Symptoms it addresses: "
           "the resting hand sits rotated against the real controller, and the "
           "wrist swing explores its anatomical cone off-centre - over one bow "
           "session the RIGHT axisA pinned exactly on a clamp limit in 33 of 110 "
           "samples, bimodal at 22x -18.0 and 11x +38.0, while the LEFT never "
           "pinned once and stayed inside 6.8-14.6. A clamp saturating at BOTH "
           "ends is a wrong centre, not a limit that needs raising. Hold, not tap, "
           "because B is also passed through to gameplay. See CLAUDE.md. "
           "OFF BY DEFAULT since 2026-10-12: the baked grip constant makes it a "
           "no-op for the grip, and each hold cost one solve with the wrist on the "
           "controller (REVIEW-RIG-2026-10-12 N3)."),
    S_BOOL("rig_suppress_stretch", RigSuppressStretch,
           "Rigging: undo the engine IK's bone stretch - impose each driven bone's "
           "authored length at the player tail", "Camera hook",
           "THE ARM THAT GETS LONGER. Measured, not inferred: the +0x1938 buffer is "
           "PARENT-LOCAL and its translation is the bone's own offset from its "
           "parent - first-sample magnitudes read 0.0853 / 0.1718 / 0.2865 / 0.2649 "
           "for elements 45/48/49/53, exactly the four authored rest lengths. Over "
           "one bow session with the hand held 0.75 m out (the rig's whole arm is "
           "0.546 m), element 48 grew 0.1718 to 0.6944 while elements 49 and 53 - "
           "DRIVEN by the identical rotation-only write - and the NOT-driven "
           "clavicle 45 all held their rest length to four decimals. Three driven "
           "bones, one moves: it is not our write, it is the engine's own hand IK "
           "scaling the first bone of the chain when the target exceeds its reach. "
           "It is FastIkHandPosAndRot's stretch flag, which keeps the arm from "
           "visually snapping; we want the opposite. "
           "This undoes it at our own write, which runs at the player-update tail "
           "and already has the last word, keeping the direction the engine chose "
           "and imposing the authored length. Rest length is latched from live "
           "samples and tracks the MINIMUM seen, because a stretch only ever "
           "lengthens - so it converges to rest even if the first sample is caught "
           "mid-stretch. COST, stated plainly: the hand will now fall SHORT of the "
           "real controller at full extension rather than deforming to reach it - "
           "about 0.2 m at a 0.75 m reach. See CLAUDE.md."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_body_yaw", DiagBodyYaw,
           "Rigging: log bodyYaw and the rotated controller offset, melee vs bow",
           "Camera hook",
           "THE TWO SYMPTOMS LEFT ON THE BOW after the grip fix straightened the "
           "wrist in melee: the right arm starts on the LEFT side, and the "
           "controls are inverted. Both are what a wrong bodyYaw produces, because "
           "bodyYaw is what rotates the head-relative controller offset into game "
           "space before the IK target is built. It is derived as ctrlYaw + K, and "
           "K is calibrated ONCE over 240 samples then latched - so if the bow "
           "changes the control object's yaw relationship, the entire bow window "
           "runs on a stale constant. Logs bodyYaw, whether the right hand holds "
           "an item, and the offset before and after rotation, once a second. A "
           "bodyYaw that shifts when the bow is equipped is the answer; one that "
           "holds steady rules it out. Read only."),
#endif
    S_BOOL("rig_shoulder_freeze_dir", RigShoulderFreezeDir,
           "Rigging: freeze the whole latched shoulder offset (the version "
           "confirmed working) instead of keeping the live direction",
           "Camera hook",
           "ON is the ORIGINAL rig_stable_shoulder behaviour - freeze both the "
           "length and the direction of element 48's latched local offset. That is "
           "the build the user confirmed in the headset: 'right-arm control with "
           "the bow works, positioning is good', with clav-to-upper peaking at "
           "0.2250 for the whole run. OFF keeps the live direction and clamps only "
           "the length, which was written because 'the arm starts off to the left' "
           "was read as the frozen direction fighting the game's shoulder re-pose. "
           "That premise is dead: the stretch trigger is the RIGHT HAND BEING "
           "EMPTY - the bone leaves rest in the same second the right hand empties "
           "and the bow is equipped, after a full minute at rest holding melee - "
           "so it is neither the shoulder nor reach. Needs rig_stable_shoulder."),
    S_BOOL("rig_stable_shoulder", RigStableShoulder,
           "Rigging: derive the IK shoulder from the clavicle instead of element "
           "48's live position - breaks the bow runaway", "Camera hook",
           "THE BOW RUNAWAY FIX. SolveArmIk read its shoulder S straight out of "
           "element 48's live world matrix, and our own write is what moves element "
           "48 - so S fed back into the rotation that displaced it. Proven by a "
           "single-variable A/B on one binary: rig OFF, element 48's local "
           "translation holds its rest 0.1718 through the whole bow window "
           "(0.1509-0.2084, max deviation 0.037); rig ON, it RAMPS 0.17, 0.59, "
           "0.81, 1.05, 1.55, 1.60 - in exact lockstep with |clav-upper| to four "
           "decimals. Our written 3x3 is orthonormal by construction, so the input "
           "is bounded and monotonic growth can only be positional feedback. This "
           "takes S from the clavicle - measured clean in BOTH halves, "
           "|hspine1-clav| exactly 0.0853 and basis columns exactly 1.0000 - "
           "composed with element 48's authored rest offset, latched once while it "
           "is still sane. Retires the 'the game displaces the shoulder' root cause "
           "and the six theories downstream of it. See CLAUDE.md."),
    S_BOOL("anim_suppress_balance", AnimSuppressBalance,
           "Anim: STUB UpdateBalanceAnimation (gamedll+0xC84340) - the idle-sway "
           "candidate", "Camera hook",
           "A true stub: a throttled log and `return 0`. UpdateBalanceAnimation is the balance / "
           "weight-shift animation, which is the strongest candidate we have ever "
           "had for Jay's IDLE SWAY - and it is a CAUSE, not a filter, unlike D-77, "
           "D-78 and D-83. "
           "THE ADDRESS IS VERIFIED AGAINST OUR OWN BINARY: 0xC84340 resolves to an "
           "EXACT function start with a clean 19-byte prologue in our gamedll "
           "(md5 97046bba5eb07a918acff82a394e8c8b). "
           ">>> PRE-REGISTERED: idle sway reduced or gone = this was it. Unchanged = "
           "the balance animation is not the sway and the lead dies. Body, legs or "
           "camera misbehaving = turn it straight back off. <<< "
           "Byte-verified before patching and REFUSES on mismatch, naming what a "
           "mismatch means. HEADSET for the verdict, but it installs in FLAT too, "
           "so a prologue mismatch is discoverable without spending a headset run."),
    S_BOOL("anim_suppress_antiwall", AnimSuppressAntiWall,
           "Anim: STUB UpdateAntiWallDeformation (gamedll+0xC24160). ⚠ EXPERIMENTAL",
           "Camera hook",
           "⚠ This function does REAL WORK (a second writer); STUBBING it is an "
           "experiment. Our own archive separately says the "
           "game zeroes ANTI_WALL_MODE before gameplay, so this may be inert. "
           "Independently switchable so a regression names its culprit."),
    S_BOOL("anim_suppress_smoothpos", AnimSuppressSmoothPos,
           "Anim: STUB BodyVis::UpdateSmoothPosPlayer (gamedll+0x10B2F80). ⚠ EXPERIMENTAL",
           "Camera hook",
           "⚠ This function DOES REAL WORK. >>> IT IS THE BEST CANDIDATE "
           "OF THE FOUR FOR 'DISPLACEMENT UPON MOVING': the name says it smooths the "
           "player's position, and a position smoother is exactly what makes a "
           "world-space-anchored arm lag the body while moving. <<<"),
    S_BOOL("rig_anchor_from_spine", RigAnchorFromSpine,
           "Rig: anchor the arms on hspine1 x element 8's AUTHORED local, not on "
           "element 8's LIVE world", "Camera hook",
           ">>> ANCHOR THE ARM CHAIN ON hspine1, resolved BY NAME - "
           "`GetElementID(model, \"hspine1\")`. <<< "
           "Otherwise WE anchor on element 8, `eyecamera`, which is a CHILD of hspine1(7) and "
           "carries its OWN animation on top of the spine. Both clavicles also hang "
           "off hspine1, so that extra term is absent from the shoulders and does NOT "
           "cancel in the shoulder->target relationship. ENGINE_ARCHITECTURE.md:3674 "
           "called it on 2026-09-07 - \"anything that pitches the spine pitches the "
           "view and the held weapon with it, and nothing resets it\" - and marked it "
           "\"Not yet acted on\". It is Jay's IDLE SWAY, reported 2026-09-22 once the "
           "weapon began riding the hand. "
           ">>> IT IS NOT A ONE-CONSTANT SWAP. <<< The target is "
           "anchor + R*(controller - head), so the anchor must be where the HEAD is. "
           "Substituting hspine1 directly would displace BOTH hands by ~0.3 m. This "
           "computes hspine1.world x element 8's AUTHORED local - the true eye "
           "position with the live animation dropped. The authored read is INV-148's "
           "accessor, proven on 71/35/48/12/53/17/50/14 over 53 passes, drift 0 "
           "(INV-161). LATCHED ONCE per process (INV-4: never re-read a value off a "
           "bone you drive) and FAILS CLOSED - no latch, no swap, and [SPINEANCHOR] "
           "says so BY NAME rather than silently anchoring somewhere else. "
           "⚠ IT DOES NOT ADDRESS THE CACHE LAG. hspine1 comes through the same "
           "accessor and carries INV-48's 60-100 ms like everything else. What it "
           "removes is the eyecamera's OWN animation - a DIFFERENT defect in the same "
           "bone. Residual 'displacement upon moving' is EXPECTED to survive this, and "
           "its survival is not evidence against the change. "
           ">>> PRE-REGISTERED: [SPINEANCHOR] FIRST USE prints the two anchors and "
           "their separation. A separation of ~0 means the eyecamera is not animated "
           "in that pose and nothing visible will change until it is. HEADSET - the "
           "rig does not solve flat. DEFAULT OFF; 0 reproduces today exactly."),
    S_BOOL("diag_anchor_ab", DiagAnchorAb,
           "Diagnostics: is the ENGINE'S OWN CAMERA a fresher anchor than element 8?",
           "Camera hook",
           ">>> READ-ONLY. WRITES NOTHING. CHANGES NO BEHAVIOUR. <<< The arm's IK "
           "target is eye + R*(controller - head) with eye = element 8's WORLD, and "
           "element 8 has TWO cited defects neither of which was ever fixed: (1) it is "
           "`eyecamera`, PARENTED TO hspine1 - ENGINE_ARCHITECTURE.md:3674, 2026-09-07, "
           "\"anything that pitches the spine pitches the view and the held weapon with "
           "it, and nothing resets it\", marked \"Not yet acted on\" - which is Jay's "
           "IDLE SWAY; and (2) its world is a derived cache lagging 60-100 ms (INV-48), "
           "with [IK JITTER] measuring catch-up steps at 145-308 m/s - which is Jay's "
           "DISPLACEMENT UPON MOVING. D-77, D-78 and D-83 all FILTERED these; none "
           "touched a cause. "
           "This logs the per-solve STEP DISTRIBUTION of element 8 beside the engine's "
           "own camera position, published from LevelCamera through a seqlock. "
           ">>> PRE-REGISTERED: camera SMOOTH where the eye JUMPS = the cache lag is "
           "the cause and re-anchoring is the fix. BOTH jumping alike = "
           "element 8 is not the source and the line dies. <<< "
           "⚠ IT COMPARES STEPS, NOT POSITIONS - the two sit at different points and a "
           "constant separation is expected and meaningless. "
           "⚠ THE CAMERA IS ONE FRAME STALE, NOT FRESH: LevelCamera runs in the RENDER "
           "phase, the rig solves in the UPDATE phase (camhook.cpp:7659) - ~8 ms at 120 "
           "fps against the eye's 60-100 ms. Roughly 10x better, NOT zero. "
           "⚠ IT DOES NOT ADDRESS IDLE SWAY, which is defect (1) and may afflict the "
           "camera too. "
           ">>> HEADSET, STRUCTURALLY: CamHookInstall's three call sites are ALL VR, so "
           "with no XR session LevelCamera never runs, nothing is published, and the "
           "probe prints NO VERDICT rather than comparing against zeros. <<<"),
    S_FLOAT("rig_hh_rot_r_pitch_deg", RigHhRotRPitchDeg, -180.0f, 180.0f,
          "Rig: RIGHT handholder, per-hand PITCH correction baked into its authored "
          "LOCAL (deg). 0 = authored", "Camera hook",
          ">>> THE ONLY ORIENTATION CHANNEL THAT SURVIVES WHEN item_pose_fix=0. <<< "
          "Measured in the headset 2026-09-22: with the position write off, the engine "
          "composes 71.world = 53.world x 71.local, the POSITION came out right and "
          "THE SHAKE WAS GONE on every weapon - but item_pose_basis writes the CACHED "
          "WORLD and dl_WorldResolver recomposes the whole matrix from the local and "
          "discards it (INV-176). Clearing bit 1 to save the basis would also stop the "
          "position recompose, because the recompose rewrites basis AND translation "
          "together - so through the world channel the two are MUTUALLY EXCLUSIVE. "
          "Writing the correction into the LOCAL has no such conflict. "
          "POST-MULTIPLIED in the handholder's own frame, the same convention "
          "item_basis_yaw_deg uses. PER HAND because the two hands' rest rotations are "
          "MIRRORED (INV-150) while item_basis_yaw_deg is ONE global applied to both "
          "with no sign flip - the standing candidate for the BOW being UPSIDE DOWN in "
          "the left hand with its arrow axis correct. "
          "DIAL IT BY EYE: overlay.cpp:541 binds a live slider to every registered "
          "float, so all twelve of these are adjustable in one run - which D-82 "
          "established is the only way to settle a roll about the grip axis. "
          "Needs rig_drive_handholders=1; inert without it."),
    S_FLOAT("rig_hh_rot_r_yaw_deg", RigHhRotRYawDeg, -180.0f, 180.0f,
          "Rig: RIGHT handholder, per-hand YAW correction in its authored LOCAL (deg)",
          "Camera hook", "See rig_hh_rot_r_pitch_deg. Applied as authored_R x "
          "Rx(pitch)*Ry(yaw)*Rz(roll). Needs rig_drive_handholders=1."),
    S_FLOAT("rig_hh_rot_r_roll_deg", RigHhRotRRollDeg, -180.0f, 180.0f,
          "Rig: RIGHT handholder, per-hand ROLL correction in its authored LOCAL (deg)",
          "Camera hook", "See rig_hh_rot_r_pitch_deg. ROLL ABOUT THE GRIP AXIS is the "
          "one a closed fist hides and a barrel or a bow limb shouts - try this first "
          "for 'the weapon is rotated but the hand looks right'."),
    S_FLOAT("rig_hh_rot_l_pitch_deg", RigHhRotLPitchDeg, -180.0f, 180.0f,
          "Rig: LEFT handholder, per-hand PITCH correction in its authored LOCAL (deg)",
          "Camera hook", "See rig_hh_rot_r_pitch_deg. THE LEFT HAND IS THE BOW'S HAND."),
    S_FLOAT("rig_hh_rot_l_yaw_deg", RigHhRotLYawDeg, -180.0f, 180.0f,
          "Rig: LEFT handholder, per-hand YAW correction in its authored LOCAL (deg)",
          "Camera hook", "See rig_hh_rot_r_pitch_deg."),
    S_FLOAT("rig_hh_rot_l_roll_deg", RigHhRotLRollDeg, -180.0f, 180.0f,
          "Rig: LEFT handholder, per-hand ROLL correction in its authored LOCAL (deg)",
          "Camera hook", ">>> START HERE FOR 'THE BOW IS UPSIDE DOWN'. <<< Jay, "
          "2026-09-22: the arrows face outwards correctly but the bow is inverted - a "
          "180 deg roll about the grip axis, which is exactly what one unmirrored "
          "global constant gives on a mirrored hand. Candidate set {180, -180, 90, -90}."),
    S_FLOAT("rig_hh_off_r_x", RigHhOffRX, -0.5f, 0.5f,
          "Rig: RIGHT handholder, offset added to its authored LOCAL translation, X (m)",
          "Camera hook",
          "In the HANDHOLDER's own frame, so it moves with the hand rather than the "
          "world. The natural home for a grip offset - D-79's 'where the item-dependence "
          "actually lives'. ⚠ IT IS PER HAND, NOT PER WEAPON, and must not be reported "
          "as the latter: the per-weapon position error is Holder authoring inside each "
          "item's own mesh and one constant cannot fix it. Needs rig_drive_handholders=1."),
    S_FLOAT("rig_hh_off_r_y", RigHhOffRY, -0.5f, 0.5f,
          "Rig: RIGHT handholder local translation offset, Y (m)", "Camera hook",
          "See rig_hh_off_r_x."),
    S_FLOAT("rig_hh_off_r_z", RigHhOffRZ, -0.5f, 0.5f,
          "Rig: RIGHT handholder local translation offset, Z (m)", "Camera hook",
          "See rig_hh_off_r_x."),
    S_FLOAT("rig_hh_off_l_x", RigHhOffLX, -0.5f, 0.5f,
          "Rig: LEFT handholder local translation offset, X (m)", "Camera hook",
          "See rig_hh_off_r_x."),
    S_FLOAT("rig_hh_off_l_y", RigHhOffLY, -0.5f, 0.5f,
          "Rig: LEFT handholder local translation offset, Y (m)", "Camera hook",
          "See rig_hh_off_r_x."),
    S_FLOAT("rig_hh_off_l_z", RigHhOffLZ, -0.5f, 0.5f,
          "Rig: LEFT handholder local translation offset, Z (m)", "Camera hook",
          "See rig_hh_off_r_x."),
    S_FLOAT("rig_weapon_pos_slew_max", RigWeaponPosSlewMax, 0.0f, 100.0f,
            "Item: rate-limit the WEAPON POSITION ONLY, metres per second", "Camera hook",
            "INV-163, AND THE SCOPE IS THE WHOLE POINT. It filters `achievedWrist` and NOTHING "
            "ELSE - S itself is untouched, so the IK, the arm, the elbow pole and the body are "
            "BIT-IDENTICAL with this on or off. That is exactly the property D-78 did not have: "
            "that one rate-limited the ANCHOR feeding the IK TARGET, broke a common-mode "
            "cancellation with the rest of the body, and made the rig WORSE in the headset. "
            "WHY IT IS DEFENSIBLE HERE. Jay, 2026-09-21: the weapon shakes and THE HAND DOES NOT. "
            "The drawn hand is composed by the ENGINE through the bone chain from our LOCALS and "
            "never reads the cached world; achievedWrist does, through S (element 48's live "
            "world). So the true hand is INDEPENDENTLY KNOWN TO BE STEADY - high-frequency content "
            "in our estimate of it is ERROR BY DEFINITION, and clipping it can only move the "
            "weapon TOWARD the hand. There is no common mode to break, because the consumer is one "
            "position and nothing else. "
            ">>> ⚠ THE LIMIT IS NOT MEASURED, AND THIS SHIPS AT 0 BECAUSE OF THAT. <<< Nobody has "
            "measured achievedWrist's PER-TICK step. INV-162's 0.174 m is a peak between 1 s log "
            "lines; INV-140's 0.159 m is the ANCHOR's step, not this one. The [WEAPONPOS] line "
            "reports the WORST RAW PER-TICK STEP, which is the number this should be sized "
            "against, plus clipped/passed so 'clipping everything' and 'clipping nothing' cannot "
            "look alike. DIAL IT ON THE OVERLAY'S LIVE SLIDER while watching the weapon - that is "
            "how the 90 deg quarter turn was found in ONE run instead of three. "
            "FAILURE MODE TO WATCH FOR: the weapon LAGGING the hand rather than shaking in it "
            "means the limit is below real motion - raise it. Needs item_pose_achieved=1."),
    S_BOOL("rig_drive_handholders", RigDriveHandholders,
           "Rig: DRIVE elements 71/35 from the asset instead of the animation", "Camera hook",
           "D-80's FIX, AND IT IS WHAT UNBLOCKS rig_anim_gate. Elements 71/35 (the handholders "
           "the weapon hangs from) were neither GATED nor DRIVEN: INV-38 forbids gating a bone we "
           "do not drive, because the animation was the only thing clamping element 71 and it ran "
           "0.09698 -> 3.60786 the one time that was tried. But leaving them out is the other "
           "horn: D-80 measured the gate arming PERFECTLY (6 of 6, 0 re-asserted) while item "
           "placement went 49 mm -> 605 mm WITHIN TWO SECONDS, same run, same route, because "
           "their local stayed the animation's while their parent 53/17 became ours. "
           "THIS WRITES BOTH ROTATION AND TRANSLATION, which nothing else in this rig does - "
           "everywhere else we write rotations and let the animation supply translations, and "
           "that is exactly the dependency the gate removes. The values come from INV-148's "
           "accessor, measured by [RESTREAD] over 53 passes with drift 0 (INV-161): element 71 "
           "rotation IDENTITY, t (+0.10000 0 -0.02500), |t| 0.10308; element 35 rotation "
           "IDENTITY, t (+0.09625 0 -0.02377), |t| 0.09914. It writes what was READ, not an "
           "assumed identity - a different model may not be. "
           "LATCHED ONCE PER SIDE IN SolveArmIk, never read in ApplyDrivenBones, which runs "
           "~19,461 times a second. Fails CLOSED: no latch, no drive, and [HANDHOLDER] says so. "
           ">>> IT ALSO CHANGES WHAT rig_anim_gate GATES. <<< With this ON and both latches in, "
           "the gated set becomes 48/49/53/12/13/17 PLUS 71/35. With it off the set is the "
           "original six and the [ANIMGATE] ARMED line says which it used. INV-38 is not "
           "repealed - its rule is 'do not gate an UNDRIVEN bone', and these are now driven."),
    S_BOOL("rig_wristback_from_asset", RigWristBackFromAsset,
           "Rig: step the wrist target back along the AUTHORED vector, not a scalar",
           "Camera hook",
           "INV-161, AND IT IS A DIRECTION ERROR, NOT JUST A LENGTH ONE. wristBack is built as "
           "hw[0]*RigHandLen, hw[3]*RigHandLen, hw[6]*RigHandLen - purely along the hand's column "
           "0 - with ONE global RigHandLen 0.0970 for both arms. The authored handholder offsets "
           "read off the mesh asset are (+0.10000 0 -0.02500) right and (+0.09625 0 -0.02377) "
           "left: about 14 deg OFF the bone axis, and 0.10308 / 0.09914 long. So the existing "
           "form is wrong in direction as well as length, and wrong differently on each side. "
           "This rotates the authored offset by the hand basis instead - the same operation the "
           "engine performs to place the handholder (world = parentWorld * local). It reduces "
           "EXACTLY to the old behaviour when the offset is (RigHandLen, 0, 0), which is the "
           "sanity property worth knowing. "
           "⚠ WHICH VALUE IS CORRECT IS NOT SETTLED. INV-21 measured element 71's LIVE local at "
           "0.0970 while the ASSET says 0.10308 - 6.1 mm apart, a discrepancy INV-161 records and "
           "deliberately does not reconcile. The ENGINE places the item from the LIVE local, so "
           "an asset-matched step-back is only strictly correct once rig_drive_handholders owns "
           "that bone. Treat this as paired with that setting, not independent of it."),
    S_BOOL("rig_grip_per_item", RigGripPerItem,
           "Rig: choose the grip by the HELD ITEM, not just by the hand", "Camera hook",
           "W3. `grip` maps the controller to the hand bone, and it has been TWO CONSTANTS for the "
           "whole game - boneblend.cpp:4850, one per hand, with nothing depending on what is held. "
           "MEASURED 2026-09-21 with rig_forearm_twist=1.0, which puts all roll into the extracted "
           "twist: RIGHT hand twist median by class - EMPTY -23.1, WeaponVis el=17 -30.8, WeaponVis "
           "el=6 -29.8, FireWeaponVis el=15 -34.1, and WeaponVis el=7 at -74.9. Three classes agree "
           "within ~4 deg and one sits 45 deg off all of them; one per-hand constant cannot do "
           "that. Corroborated in the headset: a frying pan held palm-DOWN draws with its face "
           "broadside, about a quarter turn out, which is exactly the grip-vs-bone axis difference "
           "the OpenXR spec makes (grip -Z runs ACROSS the palm, the hand bone's axis runs "
           "DISTALLY). The reference mod calibrates wrist pose PER WEAPON and PER HAND. "
           ">>> THE KEY IS CLASS **AND** ELEMENT COUNT <<< - WeaponVis appears with 6, 7 and 17 "
           "elements and they are different items; keying on the name alone merges the 45 deg "
           "outlier with two that agree. "
           "TABLE: dlvr/grips.txt, one entry per line, 'R|L <class> <elems> m0..m8'. THE LAYER "
           "NEVER WRITES IT - ovr2xr.txt is already rewritten from memory and a second "
           "self-writing file is a second way to lose a hand-edited value. A capture LOGS a "
           "paste-ready [GRIPCAP] line instead. "
           "WORKFLOW: hold the item, pose your hand as you want it, hold B to re-neutralise, then "
           "paste the [GRIPCAP] line into grips.txt for the next launch. "
           "NO-OP UNTIL A ROW EXISTS: with no file or no matching entry the per-hand constant is "
           "used, which is the pre-W3 behaviour exactly, and [GRIPTAB] SAYS which path it took on "
           "every item change. The adopted grip is replaced by the constant again when the hand "
           "empties, so a grip can never outlive its item. "
           "⚠ NOT AN ACCURACY CLAIM: a per-item constant harvested the same way inherits the same "
           "~19.72/11.46 deg harvest disagreement the grip family already has. This makes the grip "
           "ADDRESSABLE per item, not accurate."),
    S_FLOAT("rig_anchor_max_m", RigAnchorMaxM, 0.10f, 2.0f,
            "Rigging: plausibility ceiling on eye-to-shoulder", "Camera hook",
            "Metres. Past this, |eye - shoulder| cannot be a real anatomical "
            "distance on a 0.546 m arm and the anchor is treated as bad. Generous "
            "at 0.40 on purpose - the job is to reject the absurd, not to police "
            "the merely unusual. Measured at a rock-steady 0.302 m with a melee "
            "weapon held. It was 0.60 first, which let a 0.496 m sample through "
            "and poisoned the reference on the bow's ramp upward. Raise it if a "
            "legitimate pose is being caught; the [SOLVE] line reports the live "
            "value either way."),
    S_FLOAT("rig_elbow_pole_max_up", RigElbowPoleMaxUp, -1.0f, 1.0f,
            "Rigging: ceiling on how high the elbow may be poled", "Camera hook",
            "THE RIGHT ARM GOING UPSIDE DOWN ON THE BOW. The elbow pole is a "
            "blend of world-down and the hand axis - world-down contributes "
            "(1 - rig_elbow_follow_hand) to the up axis and the hand term "
            "contributes the rest, so once the hand term's up-component exceeds "
            "about 0.43 at the default weight, the blend crosses zero and the "
            "elbow SNAPS from below the shoulder to above it. The plane normal "
            "then flips sign, and since both arm bones take their roll from that "
            "plane, the whole arm rolls 180 degrees. It changes none of the other "
            "numbers - twist, axisA and axisB are all computed afterwards - which "
            "is why nothing in the log read 180. A real elbow does not go above "
            "the shoulder with the hand at chest height, so 0.0 (level at most) "
            "removes the cliff by making the blend unable to cross zero. Raise "
            "toward 1 to restore the old unclamped behaviour. Read the [ELBOW] "
            "line: pole clamped counting up means it is working, plane-normal "
            "FLIPS should be at or near zero."),
    S_FLOAT("rig_reach_soft", RigReachSoft, 0.5f, 1.0f,
            "Rigging: where reach starts easing off", "Camera hook",
            "The rig's arm is 0.5514 m - shorter than a real one - so a hard clamp "
            "means the in-game arm stops dead while yours keeps travelling, which "
            "reads as hitting a wall. Below this fraction of maximum, motion is "
            "exactly 1:1; above it the last stretch compresses smoothly and "
            "asymptotes, so full extension is approached rather than struck. 1 "
            "restores the old hard clamp."),
    S_FLOAT("rig_reach_margin_mm", RigReachMarginMm, 0.1f, 5.0f,
            "Rigging: how straight the arm gets at full reach (lower = straighter)", "Camera hook",
            "The in-game arm is never allowed to go completely straight: it stops this many millimetres short "
            "of its full length. That small gap leaves a visible bend at the elbow when your own arm is fully "
            "out - at 5 the elbow sits 37 mm off the straight line, at 0.5 about 12 mm, at 0.1 about 5 mm "
            "(INV-360). Lower = a straighter arm at full reach. Both arms. The hand's position does not move "
            "by more than this many millimetres. Works live. 5 by default."),
    S_BOOL("rig_forearm_aim_achieved", RigForearmAimAchieved,
           "Rigging: keep the hand on the weapon when the arm cannot reach the controller", "Camera hook",
           "When the in-game arm is shorter than the reach you ask of it - at full stretch, or everywhere with the "
           "reach scale below 1 - the forearm was still pointed at where your controller is instead of where the "
           "in-game hand can get to, so the drawn hand sat one to three centimetres from the weapon it holds "
           "(INV-362). On: the forearm points at the reachable spot, and the hand and weapon stay together. No "
           "effect while the arm reaches the controller. Works live. On by default (verified in the headset "
           "2026-10-02). Leave it on while the reach scale is below 1."),
    S_BOOL("diag_fastik_globals", DiagFastIkGlobals,
           "Rigging: read the engine hand-IK's blendWeight/maxDelta globals",
           "Camera hook",
           "FastIkHandPosAndRot (gamedll+0x10B7D80) is what stretches element 48. "
           "The decompile already on disk (re_analysis/fastik_callers_decompiled.txt) "
           "shows all six of its call sites take blendWeight and maxDelta from two "
           "engine GLOBALS rather than computing them: gamedll+0x1927050 and "
           "gamedll+0x1927B24. So the engine's hand IK can be turned down with a "
           "4-byte write and no hook. This reads and reports them. Read only."),
    S_BOOL("rig_final_gate_hooks", RigFinalGateHooks,
           "Rigging: detour UpdateBodyModyfication + visUpdate2 and report which "
           "runs last / which stretches", "Camera hook",
           "READ-ONLY MEASUREMENT by default. Our player-tail write undid the "
           "stretch 7,363 times, worst +1.38 m, and element 48 still read 1.55 m "
           "afterwards - so something rewrites it AFTER the tail. It is not "
           "FastIkHandPosAndRot: that hook matched the player on 262 of 262 calls, "
           "bounded maxDelta on all of them, and changed nothing. The reference "
           "mod names its last two impose points '[hand] final-gate impose: "
           "UpdateBodyModyfication=%s visUpdate2=%s', both later than the tail. "
           "This hooks both and logs, per gate, the call count, how often element "
           "48 GREW across it, the worst growth, and a sequence number - the "
           "highest sequence runs last, and 'grew' names the stretcher. NOTE the "
           "intervention map's 0xC1FB30 is INSIDE FUN_180c1f9b0; the detour target "
           "is its start, 0xC1F9B0 (per re_analysis/arm_ik_targets_decompiled.txt)."),
    S_BOOL("rig_write_at_final_gate", RigWriteAtFinalGate,
           "Rigging: also write the solved bones at the final gates (later than "
           "the tail)", "Camera hook",
           "Applies the driven bones after the original at BOTH final gates, which "
           "is strictly later than rig_write_at_player_tail. Needs "
           "rig_final_gate_hooks. Leave OFF for the first run - measure which gate "
           "runs last before writing at either, or the result cannot be attributed."),
    S_INT("melee_pose_wave", MeleePoseWave, 0, 1,
          "R5 step 5a-bis: 1 = alternate the pose offset's SIGN every call",
          "Camera hook",
          "0 is the default and is what INV-132 proved - a CONSTANT offset, which moves "
          "the sweep and is therefore blind to WHEN each delta arrives. 1 alternates the "
          "sign on every call, so consecutive record starts must differ by TWICE the "
          "amplitude. >>> WHY THAT IS THE RIGHT SIGNAL: the hand moves ~2 cm between "
          "frames (INV-128), so at amplitude 0.25 the injected step is 0.5 m against "
          "0.02 m of real motion - twenty-five to one, separable by subtraction rather "
          "than by fitting. <<< WHAT IT TESTS THAT A CONSTANT CANNOT: that each call's "
          "delta reaches the record for THAT call. A one-frame-stale pose, an averaged "
          "pose, and a pose written after the caller has already read it are all "
          "invisible under a constant offset and obvious under this - and all three are "
          "failure modes step 5b would otherwise meet for the first time in a headset. "
          "Needs melee_pose_offset_* non-zero for an amplitude, and "
          "diag_melee_pose_probe=1. FLAT: it compares OUR injected signal against the "
          "record, which is a structural correspondence, not a world-matrix claim."),
    S_FLOAT("melee_pose_offset_x", MeleePoseOffsetX, -5.0f, 5.0f,
            "R5 step 5a: world X offset added to the melee sweep's pose (0 = disarmed)",
            "Camera hook",
            ">>> THIS WRITES INTO THE GAME'S MEMORY. 0.0 IS THE DEFAULT AND THE SAFE "
            "STATE, and all three axes must be 0 for the write to be skipped entirely. <<< "
            "It adds a constant WORLD offset to the INCOMING matrix of gamedll+0xDC1C90 "
            "before the original runs. INV-130's arithmetic shows the incoming translation "
            "appears additively in the composed result, so the delta lands in the output "
            "exactly and the original still does all its own work. That is why this is an "
            "offset and not a replacement pose: no replacement signature to get wrong "
            "(INV-131's xmm3 finding), and NO write through g_meleeSelf, whose staleness is "
            "INV-131's other finding - a2 is the CALLER'S OWN STACK, passed as an argument. "
            "WHAT IT PROVES: that the convention is right in the WRITE direction and not "
            "just the read, that the game accepts a moved sweep, and that targets and damage "
            "follow it. All structural, so FLAT (INV-11). A good first value is 0.5. >>> IF "
            "AN OFFSET SWEEP FINDS NOTHING AT ALL, that is the broad phase being centred on "
            "the animated hand rather than derived from the quads - read ComputeSearchPos "
            "before going further, do not just try a bigger number. <<< Needs "
            "diag_melee_pose_probe=1 to have a hook to write through."),
    S_FLOAT("melee_pose_offset_y", MeleePoseOffsetY, -5.0f, 5.0f,
            "R5 step 5a: world Y offset added to the melee sweep's pose (0 = disarmed)",
            "Camera hook",
            "See melee_pose_offset_x. 0.0 is the safe state. Y is the vertical axis in the "
            "world positions measured so far (~54 while X and Z are in the hundreds)."),
    S_FLOAT("melee_pose_offset_z", MeleePoseOffsetZ, -5.0f, 5.0f,
            "R5 step 5a: world Z offset added to the melee sweep's pose (0 = disarmed)",
            "Camera hook",
            "See melee_pose_offset_x. 0.0 is the safe state."),
    S_ENUM("melee_pose_drive", MeleePoseDrive,
           "R5 step 5b: drive the melee sweep from the CONTROLLER",
           "Camera hook",
           ">>> THIS WRITES A CONTROLLER-DERIVED POSE INTO THE GAME'S HIT GEOMETRY, "
           "AND IT IS HEADSET-ONLY TWICE OVER (INV-133): controller and head poses "
           "do not exist flat, and every number in the transform is a world-matrix "
           "claim, which INV-11 says flat cannot decide. <<< 0 is the default and "
           "restores step 5a's offset path exactly - INV-132 and INV-134 stay "
           "reproducible against this binary. The transform is the RIG'S, already "
           "working for weeks: world = anchor + Ry(bodyYaw)*(grip - head), anchor = "
           "element 8's translation, and the GRIP pose rather than the aim pose "
           "because aim is tilted per controller model and is not transferable. "
           ">>> SO IT RIDES THE RIG AND INHERITS EVERY WAY THE RIG CAN BE SILENTLY "
           "DEAD: all three master enables (INV-1), [SHOULDER] latched in "
           "0.1690-0.1750 (INV-3), and a player DEATH destroys the model capture "
           "permanently until a re-capture (INV-81). <<< THE THREE MODES: 1 writes "
           "the POSITION only and leaves the basis to the animation, which is the "
           "smallest step from the write INV-132/INV-134 proved. 2 adds "
           "Ry(bodyYaw)*quat(grip.orientation). 3 adds the HAND BONE's basis "
           "instead - the composed output is a2.R * helper.t + a2.t and helper.t is "
           "the \"AttackHelper\"/\"HandHelper\" offset, which is expressed in the "
           "hand bone's frame, so 3 is arguably the frame it belongs in. WHICH OF 2 "
           "AND 3 IS RIGHT IS [UNKNOWN] - switch between them here in the headset "
           "and find out; that is why this is a combo and not a bool. Any non-zero "
           "value IGNORES melee_pose_offset_* and melee_pose_wave entirely, and "
           "says so once in the log. Needs diag_melee_pose_probe=1 for a hook to "
           "write through, and the rig actually solving.",
           kMeleePoseDrive),
    S_ENUM("melee_pose_hand", MeleePoseHand,
           "R5 step 5b: which controller drives the sweep", "Camera hook",
           "Only the matching side of SolveArmIk publishes a grip pose, so this "
           "also decides which arm pays the (negligible) publish cost. Default "
           "right. INV-128 established that only record A - the single-weapon "
           "record - is ever reachable, and Jay reports akimbo melee is not "
           "available in EE; that says nothing about which physical hand holds the "
           "weapon, which is why this is a setting and not a constant. If it is "
           "wrong the log says 'THE RIG HAS PUBLISHED NO GRIP POSE' rather than "
           "going quiet.",
           kMeleeHand),
    S_FLOAT("melee_pose_max_age_ms", MeleePoseMaxAgeMs, 0.0f, 1000.0f,
            "R5 step 5b: refuse a published grip pose older than this", "Camera hook",
            "The rig solves ~580 times a second when it is healthy - measured, "
            "209,762 solves over ~6 minutes - so at the default 100 ms this cannot "
            "trip on latency. It trips when the rig has STOPPED, which is INV-1's "
            "master enables or INV-81's death-destroys-the-capture, and both of "
            "those are silent everywhere else. The refusal is counted as stale= on "
            "[POSE STATS] and named the first time it happens. 0 disables the check "
            "and is NOT recommended: a stale pose is a plausible wrong pose, which "
            "is the one failure mode that cannot be told apart from a different bug."),
    S_FLOAT("melee_pose_max_jump", MeleePoseMaxJump, 0.0f, 50.0f,
            "R5 step 5b: refuse a controller pose this far from the animation's",
            "Camera hook",
            "|controller-derived position - the incoming animation position|, in "
            "metres. TWO JOBS. First, a wrong transform - a bad bodyYaw, a "
            "displaced anchor - throws the sweep somewhere melee can never connect, "
            "and INV-132 proved that at 3.0 m melee misses at point blank; without "
            "this limit that outcome is indistinguishable from 'the feature does "
            "nothing'. Second, it is a free guard against a caller that is not the "
            "player: INV-131 measured zero calls outside the player's own damage "
            "window, but a foreign caller's incoming matrix would not be near our "
            "controller and gets refused rather than teleported. The refusal NAMES "
            "the value, so a run that refuses everything still reports the "
            "divergence it refused on. 0 disables it. >>> The gap is MEASURED on "
            "every call regardless, refused or not, and its min/mean/max and 0.25 m "
            "buckets are on [POSE STATS] - that distribution is the U-17 number "
            "this run exists to get. <<<"),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_melee_pose_probe", DiagMeleePoseProbe,
           "R5 step 4: observe gamedll+0xDC1C90, the melee sweep's pose source",
           "Camera hook",
           ">>> READ-ONLY. An inline detour that observes four arguments and returns - "
           "it writes nothing and substitutes nothing. <<< This is the function every "
           "non-seed vertex of the melee sweep comes from (INV-129); it reads the named "
           "animation helpers, NOT the live element, which is why moving a bone cannot "
           "steer the sweep. It is the point Jay chose to substitute, so this observes "
           "it first. >>> FLAT-VALID: it asks about a matrix's FORMAT and a call rate, "
           "not about world values (INV-11). <<< What it settles: that the hook lands "
           "and the game is unchanged; that the translation really is at indices 3/7/11 "
           "(INV-130) - the swing positions sit near (-628, 54, -179) so only the "
           "translation triple can read in the hundreds, which makes it an unambiguous "
           "test; the TIME argument and how many sub-steps run per window, which "
           "INV-129 inferred rather than measured and which sets how deep a controller "
           "pose ring has to be. Independent of diag_melee_elem_probe - different "
           "address, own trampoline - and it needs diag_melee_record_probe=1 for the "
           "per-window call counts and the [QUAD] output side."),
    S_BOOL("diag_melee_record_probe", DiagMeleeRecordProbe,
           "R5 step 3: sample the melee HIT RECORD read-only, from its own thread",
           "Camera hook",
           ">>> READ-ONLY, AND IT CONFLICTS WITH NOTHING. <<< No WatchExec slot, no "
           "debug register, no detour, not on any per-frame path - it samples "
           "this+0x6F0 / +0x728 / +0x760 (INV-127) off the pointer the melee input "
           "thunk already latches, at 2 ms from its own thread. It installs that "
           "thunk itself, so it does NOT need diag_damage_watch=1. "
           ">>> FLAT-VALID (INV-11): every question it asks is structural, temporal "
           "or a constant read. DO NOT SPEND A HEADSET RUN ON IT. <<< What it "
           "settles: whether the phase latch really runs 1->2->3 and how long the "
           "damage window lasts; whether frame n's sweep START equals frame n-1's "
           "END (the sliding sweep); whether +0x30/+0x34 are TIMES or LENGTHS - the "
           "open contradiction in INV-127; and whether the vec3 at gamedll+0x1CFB510 "
           "is really (0,0,0), which static could not answer because those bytes are "
           "not in the file. It needs a level and a melee weapon in hand: until the "
           "first swing it logs NO INSTANCE YET and names why."),
    S_BOOL("diag_melee_elem_probe", DiagMeleeElemProbe,
           "Diagnostic: watch the SLastHit builder and report the melee ELEMENT",
           "Camera hook",
           "MOVES THE SINGLE WatchExec SLOT FROM SetLastHitBone TO THE SLastHit BUILDER "
           "(gamedll+0xC4D920), so it REPLACES [BONEWATCH] - there will be no "
           "*** BONE HIT *** lines while this is on, by design. What it buys: the "
           "element index at [R8+0x8], captured at the builder, which is UPSTREAM OF ALL "
           "NINE melee attack paths (INV-65) including the two that hardcode \"spine3\". "
           "Nothing has ever logged the element, so this is what produces the "
           "element<->bone mapping. READ-ONLY on its own - the write needs "
           "melee_force_elem."),
#endif
    S_FLOAT("melee_force_elem", MeleeForceElem, -1.0f, 1024.0f,
            "R5: force every melee hit's element to this index (-1 = observe only)",
            "Camera hook",
            ">>> -1 IS THE DEFAULT AND THE SAFE STATE. A value >= 0 ARMS A WRITE INTO THE "
            "GAME'S OWN MEMORY at a breakpoint - the first thing in this project that "
            "mutates game state rather than observing it. <<< It replaces the element at "
            "[R8+0x8] on entry to the SLastHit builder, so every consumer inside that "
            "function reads OUR value: the visual effect placement (confirmed, INV-63/U-24) "
            "and, on pseudocode-level evidence, the AI-side element that the damage path "
            "reads. THE POINT OF THE EXPERIMENT is exactly to find out whether damage and "
            "dismemberment follow, or whether only the blood moves - U-24's open half. "
            "REQUIRES diag_melee_elem_probe=1. Run it read-only FIRST and check the "
            "element values look sane before arming this."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_melee_bone_census", DiagMeleeBoneCensus,
           "R5 step 2: census the call sites of GetMeshElemFromBoneID (writes nothing)",
           "Camera hook",
           "TAKES THE SINGLE WatchExec CLIENT 0 SLOT, SO IT REPLACES [BONEWATCH] - there "
           "will be no *** BONE HIT *** lines while this is on, by design, and it refuses "
           "outright if diag_melee_elem_probe=1 holds the same slot. IT WRITES NOTHING. "
           "It traps IModelObject::GetMeshElemFromBoneID (an ENGINE export, resolved by "
           "name; INV-104 read it at engine+0x143DC0) and counts hits BY RETURN ADDRESS. "
           "Two questions: (1) what does trapping this site COST - the per-call rate is "
           "[UNKNOWN] and gamedll alone reaches it from 372 call sites, so if it is a "
           "per-frame path the whole approach dies here cheaply; (2) WHICH return "
           "addresses are the six melee ones (INV-65), because the write's call-site gate "
           "has to be built from measurement and a decompile names caller FUNCTIONS, not "
           "return addresses. RUNS IN FLATSCREEN - it needs no weaponanchor and no ILevel, "
           "unlike everything else in the damagewatch family."),
#endif
    S_FLOAT("melee_census_ms", MeleeCensusMs, 0.0f, 600000.0f,
            "R5 step 2: how long to hold the census armed, ms (0 = the whole session)",
            "Camera hook",
            "A WINDOW RATHER THAN A HIT CAP, DELIBERATELY. WatchExecSetMaxHits disarms "
            "SILENTLY mid-run - that is what killed the 2026-09-16 10:52 run - and here it "
            "would also destroy the measurement: frame pacing after a silent self-disarm is "
            "the pacing of an UNARMED game, which reads as 'this site costs nothing'. The "
            "cap is explicitly lifted to 0 for the duration and the window bounds the "
            "exposure in TIME, with the open and close both logged. 15000 is the default. "
            "Progress prints every 2 s so a window that becomes unusable is attributable to "
            "the moment it did."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_lasthitloc_readwatch", DiagLastHitLocReadWatch,
           "R5: name everything that touches m_LastHitLocation (read-only)",
           "Camera hook",
           "IT WRITES NOTHING. Arms a DR0 DATA breakpoint in READ-OR-WRITE mode at "
           "stateVec+0x688, taking the state vector from the melee trap's own RCX "
           "(INV-103), so it REQUIRES diag_damage_watch=1 and the [BONEWATCH] trap, and "
           "it does nothing until you land a melee hit. WHAT IT DECIDES: INV-106 found no "
           "static consumer of that field in gamedll - setter and no getter, while its "
           "siblings have both - but could not rule out a function reading ONLY +0x688 "
           "(74 displacement-matched candidates it cannot separate from ItemDescFirearm) "
           "or an engine-side accessor. If nothing reads it, INV-105's location mismatch "
           "is bookkeeping and R5's write half is the element poke that already works. "
           ">>> THE POSITIVE CONTROL IS BUILT IN: SetLastHitLocation writes this exact "
           "address on every hit, so it MUST appear in the list; if it does not, the run "
           "is VOID and the probe says so rather than reporting an empty list as 'nothing "
           "reads it'. <<< DR0 is the bone probe's exec client 1 with rig_exec_client2=1 - "
           "that client is a ONE-SHOT model capture that releases the register, so a "
           "refusal is retried rather than latched."),
#endif
    S_FLOAT("locread_window_ms", LocReadWindowMs, 0.0f, 600000.0f,
            "R5: how long to hold the m_LastHitLocation read-watch, ms (0 = until the cap)",
            "Camera hook",
            "30000 is the default. The data watch self-disarms at 400 hits regardless and "
            "REPORTS that it did - unlike the exec watch, whose silent cap killed the "
            "2026-09-16 10:52 run. Progress prints every 5 s with the reader list so far, "
            "and every report carries the number of melee hits recorded on the watched "
            "body: an empty reader list means nothing only if that number is climbing, "
            "because no further hits means no further WRITES either."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_FLOAT("diag_damage_watch_rva", DamageWatchRvaOverride, 0.0f, 16777216.0f,
            "Diagnostic: which gamedll RVA diag_damage_watch traps (0 = default)",
            "Camera hook",
            "THE WATCH TARGET IS SETTABLE BECAUSE THERE IS MORE THAN ONE CANDIDATE and a "
            "rebuild per candidate wastes a session. 0 uses the default, 0x2A2830 = "
            "HealthModule::AddDamageToBody. "
            "MEASURED 2026-09-10 with every instrument check passing - slot verified "
            "EXCLUSIVE (only our address armed, 117 threads) and the staged gamedll's md5 "
            "equal to the deployed one, so the RVA was valid for the running build - "
            "0x2A2830 recorded ZERO hits while the user landed melee hits and kills. That "
            "is a GENUINE NEGATIVE, not an instrument failure: the base "
            "HealthModule::AddDamageToBody does not run for melee damage on zombies. "
            "LEADING HYPOTHESIS: zombies use BodyPartsHealthModule (per-limb health and "
            "dismemberment - exactly what this game does to them). Slot 263 appears in "
            "THREE vtables: 0x1447AF0 and 0x1455158 both hold sub_1802A2830 (watched, never "
            "fired), while 0x14451E0 holds sub_180279AF0. So try **2595568 decimal = "
            "0x279AF0** next - a function dismissed earlier for LOOKING like a pass-through "
            "wrapper rather than for any measured reason. "
            "Enter it in DECIMAL (settings are floats): 0x279AF0 = 2595568, 0x2A2830 = "
            "2762288. A float carries these exactly. The [damagewatch] ARMED line prints "
            "the RVA it actually used, so a typo is visible rather than silently watching "
            "the wrong address. Needs diag_damage_watch=1, and the bone-probe master "
            "enables OFF - see that setting's note."),
#endif
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_damage_watch", DiagDamageWatch,
           "Diagnostic: capture HealthModule::AddDamageToBody's 12 arguments live",
           "Camera hook",
           "READ-ONLY, and it is the ONE thing static analysis could not settle for the "
           "melee-damage work. Four IDA passes established the damage path (see "
           "ghidra-project\\ida\\DAMAGE_SYSTEM_FINDINGS_2026-09-10.md) and stalled on the "
           "twelve arguments of HealthModule::AddDamageToBody at gamedll RVA 0x2A2830 - a "
           "function identified by its OWN log strings, '[HealthModule::AddDamageToBody] "
           "current health:' and 'remaining HealthBody:'. Hex-Rays contradicts ITSELF about "
           "those arguments across the function's two overrides (a5 is float in one, "
           "unsigned int in the other), so at least one inferred signature is wrong and "
           "neither is trustworthy; and the call-site sweep that would have resolved them "
           "FAILED, because +0x838 is vtable slot 263 of ANY class - 65 sites matched and "
           "the first is a one-argument bool method on something unrelated. "
           "So this traps the function ENTRY and reads the arguments out of the real "
           "CONTEXT: RCX, RDX, R8/XMM2, R9/XMM3 and eight stack qwords from [rsp+0x28], "
           "each in BOTH integer and float form, because which view is meaningful is "
           "precisely the unknown. Armed on the ENTRY deliberately - only there does [RSP] "
           "hold the return address and the stack-argument layout apply, and "
           "ExecHit::stackArgsValid says whether to trust those slots at all. "
           "IT TAKES THE SINGLE PROCESS-WIDE WatchExec SLOT, so diag_world_writer, "
           "diag_visibility_probe, diag_player_movemod, diag_watch_line_add and "
           "diag_kill_physics_debug must ALL be off - a second client silently overwrites "
           "the first and both read the same results table, which has already produced one "
           "fully retracted finding in this project. "
           "Collects for 20 s, then disarms and reports once per process. HIT THINGS while "
           "it is armed - light hits, a heavy hit, a kill - because the slot that tracks "
           "hit STRENGTH across hits is the damage magnitude, while a slot constant on "
           "every hit is a type or flag. 'ret' names the caller, which no static xref can "
           "give because this dispatches through a vtable. Zero hits is itself a finding: "
           "it would mean this is not the melee damage path."),
#endif
    S_BOOL("rig_left_flag_handshake", RigLeftFlagHandshake,
           "Rigging: element 12's flag handshake - the LEFT mirror of "
           "rig_e48_flag_handshake", "Camera hook",
           "MEASURED BEFORE IT WAS WRITTEN, and the measurement is the whole "
           "justification. [FLAGRATE L], sampled at EVERY WRITE (611,576 samples): "
           "element 12's bit0 LOCAL-stale is SET 18,584 times, 3.04%, while element 48's "
           "reads 0.00% because its own handshake holds it there. Every one of those is "
           "the engine regenerating OUR local from the world - the direction that "
           "destroys the pose - and clearing bit0 while setting bit1 is exactly what "
           "this write does. "
           "AT 3.04% A ONCE-A-SECOND PROBE WOULD HAVE READ ZERO and concluded the "
           "handshake was pointless here; the sampling was deliberately moved to the "
           "per-write path first. "
           "DO NOT READ 0x0F AS HEALTHY: CLAUDE.md calls 0x0F 'the working equilibrium "
           "seen on the right after its fix', and the LEFT sits in 0x0F and is BROKEN. "
           "The regime NAME is not the health indicator - bit0's RATE is. Right: 0x0E "
           "with bit0 0.00% = healthy. Left: 0x0F with bit0 3.04% = broken. "
           "SYMPTOMS IT TARGETS: the left arm swaying while turning the camera and "
           "raising to the animation pose while sprinting - one root cause, the left's "
           "pose not holding, measured as [OVERRIDE L] peaks of 51 deg in 16 of 30 "
           "windows against the right's 17 in 3. "
           "IT DOES NOT ADDRESS THE RIGHT ARM'S OWN OVERRIDE: with a weapon held outside "
           "the safe zone the right is overridden too ([OVERRIDE R] peak 11.40 deg) but "
           "with bit0 at 0.00%, so that is a DIFFERENT mechanism and this cannot fix it. "
           "Do not read a left-side improvement as having solved that. "
           "Its own setting so a left-side regression can be disarmed without touching "
           "the right arm's five-month fix."),
    S_BOOL("rig_restore_left_len", RigRestoreLeftLen,
           "Rigging: clamp the LEFT upper arm's bone offset to its authored length",
           "Camera hook",
           "THE MIRROR OF rig_restore_e48_len, FOR ELEMENT 12. Added 2026-09-10 after "
           "the left arm was measured for the first time. Numbers, same 59 samples: "
           "elem12 (LEFT) local |t| ranged 0.1081-0.2419, a SPREAD OF 0.1338 m, against "
           "a latched rest of 0.1717 - while elem48 (RIGHT) held 0.1718 to 0.1718, "
           "spread 0.0000. A bone offset is authored data and cannot change. The right "
           "is flat ONLY because rig_restore_e48_len rewrites it every frame, and that "
           "clamp reads g_haveUpperRest[0] - hardcoded. So this is the SAME FAULT the "
           "right arm had for five months, still live on the left, and invisible until "
           "[CHAIN L] was added because the probe was right-only too. "
           "User-visible as the left arm extending from the shoulder/clavicle to the "
           "elbow while shooting. "
           "[CHAIN L] also read the l_clavicle basis DOTS at +0.00000 every sample, so "
           "the parent is not sheared - it is the offset itself being rewritten, not a "
           "basis inflating the child. "
           "LENGTH ONLY, DIRECTION UNTOUCHED, same as the right: a bone's length cannot "
           "change by rotation, and freezing the whole vector pinned the shoulder to its "
           "melee pose when that was tried. "
           "NO FLAG HANDSHAKE on this side yet, deliberately - on the right the "
           "handshake was decisive only in the 0x0D regime rig_write_at_final_gate "
           "creates, and is a documented no-op in 0x0E. Element 12's regime has never "
           "been measured. Restore first, measure, add a handshake only if the restore "
           "alone does not hold. "
           "Its own setting rather than sharing rig_restore_e48_len so a left-side "
           "regression can be switched off WITHOUT disarming the right arm's fix. Watch "
           "[CHAIN L]'s DRIFT and the left restore count in [E48T]."),
    S_BOOL("rig_restore_upper_dir", RigRestoreUpperDir,
           "Rigging: restore the upper arms' whole bone offset (direction too), not only its length",
           "Camera hook",
           "INV-330. The game moves the DIRECTION of the upper arm's offset from the clavicle - the hand "
           "correction steadily on the left with a pistol or shotgun, and something else for under a second "
           "at a pistol / shotgun draw on both arms (11-13 degrees, 33-39 mm at the shoulder). The length "
           "restores put back the length only, while the arm solve assumes the authored direction "
           "(rig_shoulder_freeze_dir), so the drawn arm sits off the solved arm by that amount: the left-arm "
           "flicker with a firearm and the jump when swapping to or from the pistol. On = both upper arms get "
           "the whole latched offset back at every restore site. Acts only where rig_restore_e48_len / "
           "rig_restore_left_len act. Watch the UPPERDIR log line: its counts say how often the live direction "
           "was off and by how much. If the shoulders or the bow arm look wrong, turn this off first."),
    S_INT("rig_elbow_pole_mode", RigElbowPoleMode, 0, 2,
          "Rigging: how the elbows are placed (0 = follow the hand, 1 = body-based, 2 = follow your real arm)", "Camera hook",
          "The elbow inversion, both arms (INV-333, INV-339). 0 = as before: each elbow swivels to line the "
          "forearm up with where the hand points, which carries the elbow toward the chest when a hand comes "
          "in to the body, and differently on each arm. 1 = each elbow is placed from where the HAND IS "
          "relative to the shoulder, by one rule mirrored for both arms: down when you reach forward, back "
          "when the arm hangs, down and slightly out when the hand comes to the chest. Turning the wrist no "
          "longer moves the elbow. The hand, the weapon and where it shoots are not touched. The wrist may "
          "show more bend, most on the right hand. In mode 1 the three older elbow settings and the 'right "
          "elbow steers by the corrected hand direction' switch do nothing. Takes effect at once. 1 by default "
          "(verified in the headset 2026-10-02). 2 = each elbow follows your real arm: it starts from mode 1 and "
          "moves only as far as needed to keep your wrist, then your forearm's roll, inside a human range, using "
          "your arm calibration (needs a calibration from 2026-10-13 or later; without one it behaves as 1)."),
    S_FLOAT("rig_elbow_axis_fwd", RigElbowAxisFwd, -1.0f, 1.5f,
            "Rigging: body-based elbow - lean of the reference axis FORWARD", "Camera hook",
            "Only with the body-based elbow (mode 1). The elbow is placed across a reference axis that points "
            "out to the arm's own side; this leans that axis forward. More = a hanging arm's elbow sits further "
            "OUT from the body. 0.30 is the starting value from the replay of logged arm poses (INV-339)."),
    S_FLOAT("rig_elbow_axis_up", RigElbowAxisUp, -1.0f, 1.5f,
            "Rigging: body-based elbow - lean of the reference axis UP", "Camera hook",
            "Only with the body-based elbow (mode 1). Leans the reference axis up. More = the elbow sits further "
            "OUT when you reach forward or bring the hand to the chest, and starts to come FORWARD at the chest "
            "above about 0.2. 0.17 is the starting value from the replay (INV-339)."),
    S_FLOAT("rig_elbow_axis_fwd_side", RigElbowAxisFwdSide, -1.0f, 1.5f,
            "Rigging: body-based elbow - lean of the reference axis with the arm OUT TO THE SIDE", "Camera hook",
            "Only with the body-based elbow (mode 1). With an arm held out to its own side the forward lean above "
            "puts the elbow ABOVE the arm and turns the whole arm over (INV-356). This is the lean used instead "
            "once the arm points outward, blended in gradually as it swings from in front to the side. Negative "
            "= lean back: the elbow goes below and behind the arm. -0.60 is the value from the replay of logged "
            "arm poses. Set it equal to the forward lean (0.30) to switch this off. Arms in front, at the centre "
            "and at the eye are not affected. Works live. -0.60 by default (verified in the headset 2026-10-02)."),
    S_FLOAT("rig_wrist_overflow_deg", RigWristOverflowDeg, 0.0f, 60.0f,
            "Rigging: let the wrist over-bend to keep the hand on the controller (degrees)", "Camera hook",
            "When the wrist is asked to bend past its limit, the hand - and a gun in it - is turned away from the "
            "controller. This lets the wrist bend this many degrees further instead, so the hand and the gun keep "
            "the controller's angle. Both hands, every elbow mode. Works live. 60 by default."),
    S_FLOAT("rig_grip_right_fix", RigGripRightFix, 0.0f, 1.0f,
            "Rigging: correct the right hand's angle", "Camera hook",
            "Arm stage 1 (INV-336 / INV-343). The right hand's built-in angle against the controller is about 28 "
            "degrees off the left's mirror image, and that pulls the right elbow toward the chest and bends the "
            "wrist. 1 = fully corrected, 0 = as before. The hand, the wrist and the elbow all change. A held weapon "
            "turns with the hand unless the next slider says otherwise. Works live. 0 by default."),
    S_FLOAT("rig_grip_right_fix_weapon", RigGripRightFixWeapon, 0.0f, 1.0f,
            "Rigging: right-hand weapon follows the corrected hand", "Camera hook",
            "Only with the slider above. 1 = the weapon turns with the corrected hand, so it stays seated in the palm "
            "but points up to 28 degrees further inward than before. 0 = the weapon keeps pointing where it does today "
            "and the hand turns underneath it. In between mixes the two. Works live. 1 by default."),
    S_FLOAT("rig_elbow_hand_weight", RigElbowHandWeight, 0.0f, 0.30f,
            "Rigging: body-based elbow - wrist relief", "Camera hook",
            "Only with the body-based elbow (mode 1). How much the elbow still swivels toward lining the forearm "
            "up with the hand, the same for both arms. 0 = the wrist never moves the elbow. Raise it only if the "
            "wrist looks over-bent or the hand stops following the controller; it brings the elbow back toward "
            "the chest in proportion. Capped at 0.30."),
    S_FLOAT("rig_elbow_short_hold_cm", RigElbowShortHoldCm, 0.0f, 40.0f,
            "Rigging: body-based elbow - hold it when the hand is at the shoulder (cm)", "Camera hook",
            "Only with the body-based elbow (mode 1). When your hand comes this close to the in-game shoulder (a bow draw at "
            "your neck, a hand to the shoulder), the arm's direction stops meaning anything and the elbow used to flip, even "
            "pointing up. With this set, the upper arm keeps the direction it had as your hand came in and only the forearm "
            "folds toward your hand - fully below half this distance. 0 = off. Works live."),
    S_BOOL("rig_bow_elbow_from_hand", RigBowElbowFromHand,
           "Rigging: bow - drawing elbow follows your real forearm", "Camera hook",
           "While you draw the bow, the drawing arm's elbow goes where your own forearm puts it, read from how you hold the "
           "controller (a draw is held with a straight wrist). It uses a measurement the arm calibration takes during its "
           "straight-arm poses, so calibrate once with this build. Off = the fixed elbow direction. Works live."),
    S_FLOAT("rig_elbow_rate_dps", RigElbowRateDps, 0.0f, 3600.0f,
            "Rigging: body-based elbow - turn-speed limit, degrees per second", "Camera hook",
            "Only with the body-based elbow (mode 1). The fastest the elbow's direction may swing round the arm. "
            "Lower = smoother but the elbow can lag a fast move; 0 = no limit. 720 is a starting value, not a "
            "measured one."),
    S_FLOAT("rig_elbow_forearm_rot_deg", RigElbowForearmRotDeg, 30.0f, 180.0f,
            "Rigging: real-arm elbow - how far your forearm rolls before the elbow swings (degrees)", "Camera hook",
            "Only with the real-arm elbow (mode 2). When you turn your palm further than this from neutral, the elbow "
            "swings out or in to take the rest, as a real shoulder does - but never at the cost of bending your wrist "
            "past its range. 80 by default."),
    S_FLOAT("rig_elbow_out_deg", RigElbowOutDeg, 0.0f, 45.0f,
            "Rigging: real-arm elbow - elbow out to the side (degrees)", "Camera hook",
            "Only with the real-arm elbow (mode 2). Lifts the elbow outward from the body rule while your arm moves in front of "
            "you; it fades out as a gun comes up to your eye. Your wrist's range still comes first. 0 = as before."),
    S_FLOAT("rig_elbow_face_min_cm", RigElbowFaceMinCm, 0.0f, 40.0f,
            "Rigging: real-arm elbow - keep the elbow this far from your view (cm)", "Camera hook",
            "Only with the real-arm elbow (mode 2). The elbow is never placed nearer your eyes than this - it stops the elbow "
            "swinging into view when you reach over your shoulder (weapon wheel, arrows). If no elbow position is that far, the "
            "farthest one is used. 0 = no limit. Works live."),
    S_FLOAT("rig_elbow_swing_max_deg", RigElbowSwingMaxDeg, 0.0f, 180.0f,
            "Rigging: real-arm elbow - how far the elbow may swing round from its normal place (degrees)", "Camera hook",
            "Only with the real-arm elbow (mode 2). Stops the elbow flipping to the other side of your arm to keep the wrist "
            "straight. Lower = fewer flips but the wrist bends further in awkward poses; 0 = no limit (as before). Works live."),
    S_BOOL("rig_elbow_right_axis_mirror", RigElbowRightAxisMirror,
           "Rigging: right elbow steers by the corrected hand direction",
           "Camera hook",
           "The elbow inversion (INV-333, INV-336). The elbow swivels to line the forearm up with the direction "
           "the hand points. For the right hand that direction comes from a fixed calibration that measures "
           "about 20-30 degrees outward of the real right forearm, so the right elbow is steered toward the "
           "chest. On = the right elbow steers by the left hand's calibration mirrored instead, which matched "
           "the measured right arm to a few degrees. It changes ONLY where the right elbow goes: the hand, the "
           "wrist position, the held weapon and where it shoots are not touched, and the left arm is not "
           "touched. The right wrist may show more bend, because the forearm no longer chases the hand's old "
           "direction. Takes effect at once. Off by default until verified in the headset."),
    S_BOOL("rig_restore_e48_len", RigRestoreE48Len,
           "Rigging: restore element 48's authored bone length at the player-"
           "update tail", "Camera hook",
           "THE FIX for the bow arm. Measured run 20:43:39: we hand element 48 over "
           "at exactly its authored 0.1718 and the engine's player update hands back "
           "a mean 0.6575 with the bow (peak 1.4526), including 22 consecutive "
           "frames with both controllers on the floor. Restores the LENGTH only and "
           "leaves the engine's direction alone, because a bone's length cannot "
           "change by rotation. No sanity band - the previous tail undo capped at "
           "0.60 m and skipped every excursion worth catching. Watch [E48T]: |C| "
           "should read the authored length on every frame."),
    S_FLOAT("rig_reach_scale", RigReachScale, 0.5f, 1.5f,
            "Rigging: reach scale, about the shoulder", "Camera hook",
            "Scales how far the hand reaches for a given real-arm extension, "
            "pivoting about the shoulder so only distance changes and not the "
            "hand's direction. Set it to the rig's arm length over your own to "
            "line up full extension exactly - roughly 0.55 / your shoulder-to-"
            "wrist in metres. The cost is that it compresses near-field motion "
            "too, which the soft limit does not; 1 leaves the mapping 1:1. 0.9 by "
            "default: the in-game arm is shorter than most players', and at 1 it locks "
            "out well before a real arm does. Raise it toward 1 if your hands sit too "
            "close to your body; lower it if the arm still straightens before yours."),
    S_FLOAT("rig_left_elbow_follow_hand", RigLeftElbowFollowHand, 0.0f, 1.0f,
            "Rigging: LEFT elbow-pole weight - kept under 0.5 BY PROOF", "Camera hook",
            "THE LEFT ARM'S 180 DEGREE ROLL. Measured 2026-09-10: at the shared 0.70 the "
            "LEFT elbow pole clamped 236-280 frames per window and still produced 3 "
            "plane-normal FLIPS, while the RIGHT clamped 0 and flipped 0 on the same run. "
            "User-visible as the bicep and forearm looking disjointed when rotating the "
            "left arm. It is invisible in twist/axisA/axisB - all three are computed "
            "after the pole. "
            "THE ARITHMETIC: pole_y = (1-w)*(-1) + w*(-hp_y/hpl). The cliff is pole_y "
            "crossing zero, which needs w*(-hp_y/hpl) > (1-w); since |hp_y/hpl| <= 1 the "
            "left term cannot exceed w, so a crossing requires w > 1-w, i.e. w > 0.5. "
            "AT w <= 0.5 THE CLIFF IS MATHEMATICALLY IMPOSSIBLE. 0.45 rather than 0.50 "
            "because at exactly 0.5 the crossing is marginal, and a value that only just "
            "works is not a guard. "
            "AND rig_elbow_pole_max_up DOES NOT SAVE IT: clamping pole[1] to 0 stops the "
            "vertical crossing and substitutes a SECOND degeneracy - a pole pinned flat "
            "in the horizontal plane can swing through parallel-to-dir, and "
            "axis = dir x pole flips there just the same. That is why the guard fires "
            "constantly on the left and the arm still rolls, and why the archive called "
            "it 'a cheap anatomical guard, not as a fix'. Holding w under 0.5 means the "
            "clamp never engages, so neither degeneracy is reachable. "
            "PER-SIDE because the right measures clean at 0.70 and there is no reason to "
            "move a working arm while fixing a broken one. "
            "COST: the left elbow follows the hand less, so its placement is slightly "
            "less natural than the right's - the trade for removing a 180 degree roll. "
            "Watch [ELBOW L]: pole clamped should fall to 0 and FLIPS to 0."),
    S_FLOAT("rig_elbow_follow_hand", RigElbowFollowHand, 0.0f, 1.0f,
            "Rigging: elbow swivels to line the forearm up with the hand", "Camera hook",
            "With shoulder and wrist fixed the elbow lies on a circle about the "
            "line between them, and moving round that circle changes the forearm's "
            "direction without moving the wrist at all. Pinning it to world-down "
            "spent that freedom on a constant and left the wrist as the only joint "
            "able to absorb the difference - which is why the radial stop "
            "saturated whenever the hand moved. The optimum is closed form: elbow "
            "opposite the hand axis's perpendicular component. 0 restores the old "
            "fixed pole, which is the A/B against the [WRIST] log angles."),
    S_FLOAT("rig_clavicle_protract_deg", RigClavicleProtractDeg, -30.0f, 30.0f,
            "Rigging: clavicle protraction - moves the shoulder forward", "Camera hook",
            "THIS is the lever for a shoulder sitting too far back. Bone "
            "translations are never written, so rig_shoulder_forward only moves "
            "where the arm AIMS, not where the shoulder is; rotating the clavicle "
            "genuinely relocates the joint, which is what protraction means. "
            "Human range is roughly 15-20 degrees. Positive is forward by "
            "construction, not by guess. Distinct from rig_clavicle_share, which "
            "swings the clavicle toward the hand - a reach rather than a posture."),
    S_FLOAT("rig_shoulder_forward", RigShoulderForward, -0.20f, 0.20f,
            "Rigging: shoulder nudge along the body's forward, metres", "Camera hook",
            "Moves where the arm is rooted. Signed on purpose - if the sign comes "
            "out inverted in the headset, a negative value is the fix rather than "
            "a rebuild. Affects the elbow directly, since the solve triangle is "
            "measured from here."),
    S_FLOAT("rig_upper_arm_len", RigUpperArmLen, 0.10f, 0.60f,
            "Rigging: upper arm length, metres", "Camera hook",
            "Measured off the rig - r_forearm's own local offset from "
            "r_upperarm is 0.2865, and a child bone's offset IS its parent's "
            "length. Exposed because a wrong proportion is visible long before "
            "it makes the IK fail to reach, and because the elbow's placement "
            "depends on it directly."),
    S_FLOAT("rig_forearm_len", RigForearmLen, 0.10f, 0.60f,
            "Rigging: forearm length, metres", "Camera hook",
            "Measured off the rig - r_hand's local offset from r_forearm is "
            "0.2649. Together with the upper arm this sets total reach, so "
            "raising either makes the arm straighten less often at full "
            "extension."),
    S_FLOAT("rig_clavicle_share", RigClavicleShare, 0.0f, 1.0f,
            "Rigging: how far the shoulder follows a reach", "Camera hook",
            "A real clavicle rotates toward whatever the hand is reaching for, "
            "and that is most of what separates a human reach from a doll's arm "
            "on a fixed socket. It also extends reach, so the IK hits its limit "
            "and straightens less often. A FRACTION only - the clavicle is a "
            "strut, not an aiming bone, and rotating it fully would put the "
            "shoulder somewhere no shoulder goes. 0 pins it as before."),
    S_BOOL("rig_arm_ik", RigArmIk,
           "Rigging PHASE B: two-bone IK - the hand reaches your hand", "Camera hook",
           "Drives r_upperarm (48) and r_forearm (49) together so the game hand "
           "reaches where your real hand actually is, using the segment lengths "
           "measured off the rig (0.2865 and 0.2649). No calibration and NO "
           "re-centring: the target uses the engine's own play-space conversion, "
           "read out of FUN_1802e52a0 - it negates X and Z of the pose we hand it "
           "and keeps Y, a 180 degree yaw and nothing else. Solved once per tick "
           "and published as finished matrices; the write hooks only copy, which "
           "is what keeps a 19,461/sec hook affordable. Supersedes "
           "rig_arm_follow_controller. Elbow poles downward. Reach is clamped at "
           "0.551 m - past that a real arm straightens rather than detaching, and "
           "the status line reports when it is clamping."),
    S_BOOL("rig_arm_inject_yaw", RigArmInjectYaw,
           "Rigging: inject the mapping yaw into the arm (jitter visibility test)", "Camera hook",
           "The OpenXR->game yaw jumps up to 8.5 degrees per tick during fast "
           "turns and is near zero when still. At roughly full arm extension "
           "that is about 8 cm of transient hand displacement. Rather than argue "
           "about whether that matters, this injects the same quantity into the "
           "arm as rotation so it can be looked at. The AXIS IS ARBITRARY on "
           "purpose - 8.5 degrees is 8.5 degrees whichever axis carries it, and "
           "picking the right one would mean solving the frame question this step "
           "exists to defer. Not the real world-stabilisation, which belongs in "
           "the IK solve where the target is a position. Needs rig_write_test and "
           "rig_arm_follow_controller."),
    S_BOOL("rig_arm_axis_remap", RigArmAxisRemap,
           "Rigging: apply the measured axis remap", "Camera hook",
           "OpenXR play space and the bone's own frame do not share a basis. "
           "Measured in-headset: controller pitch produced arm yaw, controller "
           "yaw produced arm pitch, and roll came out inverted - i.e. "
           "(x,y,z) -> (-y,-x,-z) on the delta quaternion's vector part. That "
           "matrix has determinant +1, so it is a proper rotation and a genuine "
           "change of basis rather than a fudge. Turn OFF to get the raw delta "
           "back, which is how the mapping was measured in the first place - "
           "worth keeping switchable so the measurement can be repeated rather "
           "than trusted forever."),
    S_FLOAT("rig_arm_element", RigArmElement, 0.0f, 92.0f,
            "Rigging: which element the controller drives", "Camera hook",
            "48 = r_upperarm, which swings the WHOLE arm because forearm and hand "
            "inherit through the hierarchy - this is what moves the weapon to "
            "where your hand actually is, and it is far easier to see. 49 = "
            "r_forearm, confirmed working but it can only twist (rolling the "
            "wrist rolls the blade). One at a time on purpose: applying the same "
            "rotation to both composes it twice. Changing this re-captures the "
            "baseline automatically. Other useful elements from the stage-0 map: "
            "45 r_clavicle, 53 r_hand, 71 r_handholder."),
    S_FLOAT("rig_write_seconds", RigWriteSeconds, 0.0f, 600.0f,
            "Rigging: how long the arm write stays live (seconds)", "Camera hook",
            "Counted from the FIRST write, not from launch, so time spent leaving "
            "the safe zone and drawing a weapon does not eat the window. 60 suits "
            "a yes/no probe; judging how the arm actually feels to move needs "
            "longer.\n\n"
            "SET 0 FOR NO TIMEOUT - it then stays live until rig_write_test is "
            "toggled off and on, which still works exactly as before. Use this for "
            "any run where the arm is being JUDGED rather than smoke-tested: an "
            "auto-disarm part way through reads exactly like the rig failing.\n\n"
            "With a timeout set it will not re-arm within a session once spent - "
            "toggle rig_write_test off and on, or relaunch."),
    S_BOOL("rig_arm_follow_controller", RigArmFollowController,
           "Rigging: right forearm FOLLOWS the right controller", "Camera hook",
           "Turns the write test into the actual feature. Instead of the +/-90 "
           "degree test oscillation, the right forearm takes the rotation of the "
           "right controller RELATIVE to wherever that controller was when the "
           "arm was first driven - so holding still leaves the arm exactly where "
           "the game put it, and moving rotates it from there. Relative rather "
           "than absolute because OpenXR play space and the bone's own frame are "
           "not calibrated against each other yet; an absolute pose would snap "
           "the arm somewhere arbitrary. Known working with no weapon and with "
           "melee (one and two handed); ranged weapons override the arm further "
           "downstream and are not covered yet. Needs rig_write_test on. See "
           "CLAUDE.md."),
    S_BOOL("rig_write_at_owner", RigWriteAtOwner,
           "Rigging: also write at the world-matrix build (phase 2)", "Camera hook",
           "Second write point for the write test, at FUN_1800d7380's entry - the "
           "last moment in a frame that a write to the element matrix buffer can "
           "still reach the world-matrix build. Exists because writing after "
           "FUN_1800dd7b0 moves the forearm with NO weapon held and does nothing "
           "with one, while the writes themselves still land (224,154 in 60s). So "
           "something recomputes the arm between the two - an IK pass reaching for "
           "the held item is the likely candidate. If the forearm moves with a "
           "weapon from here but not from dd7b0, the overwriter is bounded between "
           "them. Needs rig_write_test on. See CLAUDE.md."),
    S_BOOL("rig_write_test", RigWriteTest,
           "Rigging: WRITE TEST - bend the right forearm", "Camera hook",
           "WRITES TO THE PLAYER SKELETON. One bone only (r_forearm, element 49), "
           "rotation only - the translation column is left exactly as the engine "
           "wrote it. Oscillates +/-60 degrees over 2 seconds so the result is "
           "unmistakable; a static offset was tried before and proved unreadable. "
           "Written immediately after engine+0xDD7B0 returns, which is the one "
           "window between the function that writes the element matrix buffer and "
           "the one that reads it to build world matrices - that ORDERING is what "
           "the test is actually checking, since stage 2a already proved a write "
           "to this same storage holds at 99.8% and still never rendered. The "
           "rotation is always composed from a baseline captured once, never from "
           "the live value, so it cannot feed back on itself the way the look-back "
           "camera did. Auto-disarms 60s after the FIRST write. Needs "
           "diag_bone_blend_probe on. See CLAUDE.md."),
    S_BOOL("diag_bone_blend_probe", DiagBoneBlendProbe,
           "Diagnostic: bone-blend probe (rigging step 1)", "Camera hook",
           "READ-ONLY, writes nothing to the game. Inline-hooks the engine's own "
           "per-bone pose blend (engine+0xE5DA0) and the outer function that "
           "drives it (engine+0xE55C0), then reports: the real call rate of "
           "both, whether the 0x30 stride and boneIndex derivation hold, how "
           "many distinct skeletons are being blended, and a per-bone local "
           "offset table for each so its index space can be aligned against the "
           "stage-0 element map. This is the pipeline stages 1-4 were fighting - "
           "the blend recomputes every bone from the animated pose each frame, "
           "so writes made anywhere else are overwritten before render. Does NOT "
           "use the exec-watch slot, so it can run alongside diag_bone_dump "
           "(which is worth having on, to correlate the player's model). Hooks "
           "stay resident once installed; turning this off makes them "
           "pass-throughs. See CLAUDE.md."),
    S_BOOL("diag_dirty_flags", DiagDirtyFlags,
          "Rigging: read the two per-slot dirty bits, and destruction-test element 48",
          "Camera hook",
          "Walks desc -> container -> flags[slot] and prints bit 0 (LOCAL stale, rebuilt "
          "FROM world) and bit 1 (WORLD stale, rebuilt FROM local) for element 48 with "
          "element 12 as the control. Also compares *(container+0x20) against "
          "g_playerMatrices - if they differ we have never been writing the local cache "
          "the resolver returns. Structure from re_analysis/local_pos_helpers_decompiled"
          ".txt, and the destruction test proves the mechanism: read raw, call the "
          "accessor, read raw again."),
    S_BOOL("rig_e48_flag_handshake", RigE48FlagHandshake,
          "Rigging: THE FIX - perform the engine's own post-write flag handshake on "
          "element 48", "Camera hook",
          "flags[slot] = (flags[slot] & 0xfe) | 0xe, copied verbatim from "
          "re_analysis/dirty_writers_decompiled.txt: clear bit 0 so the local we just "
          "wrote is NOT rebuilt from the stale world matrix, and set bit 1 so world is "
          "rebuilt from our local. This rig has never written that byte, which is why "
          "850,000 landed restores all got regenerated away. DEFAULT OFF - confirm the "
          "bits with diag_dirty_flags first, then switch this on for the A/B."),
    S_BOOL("rig_world_watch_local", RigWorldWatchLocal,
          "Rigging: diag_world_writer watches the LOCAL matrix instead of the "
          "WORLD one", "Camera hook",
          "Point the hardware data breakpoint at the buffer WE write rather than "
          "the one that gets drawn. Run 21:49:45: with the restore at all three "
          "write sites, PRE and A both read 0.1713 every sample while B at the "
          "tail reads 0.114-0.681 and |clav-upper| runs 0.134-0.758 - so the engine "
          "rewrites element 48's LOCAL translation between our sites and the tail, "
          "and six sessions of moving OUR write around have not found that moment. "
          "This asks the CPU instead: the RIP of the storing instruction, minus "
          "module base, names the function. Use with rig_world_watch_elem=48."),
    S_INT("rig_world_watch_elem", RigWorldWatchElem, 0, 159,
          "Rigging: which element diag_world_writer watches", "Camera hook",
          "The element whose WORLD matrix translation gets the data breakpoint. "
          "53 (r_hand) is the wrist and the default, because the wrist is the "
          "quantity measured wrong. 49 (r_forearm) is the ELBOW, which measures "
          "correct at 0.03 m - arming there found nothing, which was guaranteed "
          "rather than informative. 48 is the shoulder end of the upper arm."),
    S_BOOL("rig_write_at_owner_exit", RigWriteAtOwnerExit,
           "Rigging: re-apply the arm after d7380 returns", "Camera hook",
           "Writes the solved bones a second time, after the engine's own "
           "world-matrix build has run. Only worth enabling if the d7380 EXIT "
           "checkpoint shows our value present at entry and gone at exit - which "
           "would mean the entry write is being discarded inside that function. "
           "Off by default so the exit checkpoint measures the system "
           "unperturbed."),
    S_BOOL("rig_solve_at_tail", RigSolveAtTail,
           "Rigging: solve the arm once per frame at the player-update TAIL", "Camera hook",
           ">>> INV-246's fix, SECOND SITE, because the first is wrong: [CLAVLAG] measured the "
           "clavicle at d7380's EXIT ~43-47 deg away (view-relative, melee sprint) from the one the "
           "solve sees 2 ms earlier - something after d7380 still poses it, so the exit site was "
           "removed. The TAIL (gamedll+0xBFC170, the player entity's whole per-frame update, the "
           "latest player-side site; rig_write_at_player_tail already writes there) is later than "
           "d7380 and FUN_1800dd7b0. WITH THIS ON, the arm is solved there once per frame - after "
           "ItemPlaceProbe, before ItemCtlPlace, so the weapon is placed from the fresh solve - and "
           "written at once. [CLAVLAG ... tail] measures the clavicle at this site first. The pad-poll "
           "solve stands down only while tail solves are live (< 250 ms). DEFAULT OFF."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_deform_index", DiagDeformIndex,
           "Rigging: read the 12-bit DEFORM index per element - does a deform "
           "matrix fold into the final pose?", "Camera hook",
           "Its root is *(model+8), which is our own modelInner. "
           "Walks modelInner -> +0x398 -> +0x20 -> +0x38 to an element descriptor "
           "array of stride 0x30, reads the dword at +0x28, and takes bits 12-23 as "
           "a 12-bit deform index where 0xfff means none. THE QUESTION IT SETTLES: "
           "after rig_restore_e48_len every local in the right arm is authored and "
           "ours, and the drawn wrist is still 0.29-0.45 m out. Reads BOTH arms so "
           "the clean left arm is the control - right-only folding is the answer, "
           "both folding explains nothing, zero folding means look elsewhere. Pure "
           "read. If the printed raw dword looks like a sane float, the ARRAY is "
           "wrong and the index is meaningless."),
    S_BOOL("diag_deform_probe", DiagDeformProbe,
           "Rigging: is the +0x1938 buffer a skinning matrix? Watches X = W^-1*M "
           "for constancy", "Camera hook",
           "THE DEFORM FOLD. Two "
           "anomalies already recorded about the +0x1938 buffer fit a skinning "
           "matrix M = W * Binv exactly: its translations match neither the local "
           "nor the world offsets and NOT by a constant factor (r_upperarm 0.035 "
           "vs 0.1718, r_handholder 0.418 vs 0.0970), and its basis vectors are "
           "unit length. If that is what it is, then overwriting only the 3x3 and "
           "leaving the translation column - which WriteAbsolute3x3 does "
           "deliberately - gives an implied world translation of t_M + R*t_bind, "
           "where t_M was computed for the OLD rotation. A ROTATION-ONLY WRITE "
           "THEN NECESSARILY MOVES THE BONE. That accounts for the local "
           "translation changing though we write none, melee being mild and the "
           "bow severe, and the 4.3x excursions that REMAIN now the feedback loop "
           "is cut. B must be constant if it is a bind pose, so this logs "
           "X = W^-1*M once a second for elements 48/49/53 (driven) and 45 (a "
           "NOT-driven control) and reports the worst drift since the first "
           "sample. Near-zero drift confirms it, and the fix is to write the full "
           "3x4 as W_desired * B. Drift kills the hypothesis cleanly. Pure read. "
           "See CLAUDE.md."),
    S_BOOL("diag_shoulder_chain", DiagShoulderChain,
           "Diagnostics: localise the shoulder displacement", "Camera hook",
           "Logs the four bone separations along hspine1 -> r_clavicle -> "
           "r_upperarm, plus the eye, once a second on the right arm. Read it as "
           "a check against the rig's own rest lengths (0.0853 and 0.1718): "
           "those two can never change by rotation, so if either moves, a bone "
           "TRANSLATION is being written and that is the bug. If instead "
           "|hspine1-eye| moves, the EYE is being displaced rather than the "
           "shoulder - which the earlier per-tick measurement could not rule "
           "out, since a one-time jump that then holds reads as perfectly "
           "smooth. Pure read; costs one extra accessor call a second."),
    S_BOOL("diag_item_scale", DiagItemScale,
           "Diag: is the held item's scale collapsing?", "Camera hook",
           "Read-only, 1 Hz, both hands, item-gated. Watches the three basis "
           "column magnitudes of the held item's \"Holder\" element - found BY "
           "NAME, as the engine finds it. Read statically out of our own binary "
           "first (re_analysis/correct_item_position_decompiled.txt, "
           "gamedll+0xC62AC0): CorrectItemPosition extracts the scale FROM THE "
           "ITEM'S OWN Holder matrix - the previous frame's output - normalises "
           "the hand bone's basis, applies that scale, and writes it back. It is "
           "SELF-REFERENTIAL with no other source of truth, so one corruption is "
           "PERMANENT, and a zero scale renders the item as nothing at all. That "
           "is a disappearance with NO positional error, which is what is needed: "
           "INV-27 showed the anchor's position is correct and the weapon still "
           "vanishes. Our own writes cannot cause it - the hand basis is "
           "normalised before the item's scale is applied. NOTE INV-28: this read "
           "is not passive and may itself trigger a rebuild on the item's model, "
           "which is why it is 1 Hz."),
#endif
    S_BOOL("rig_world_refresh", RigWorldRefresh,
           "Rigging: INV-48 - force the cached world to rebuild, and measure it",
           "Camera hook",
           "WRITES ENGINE MEMORY: a byte OR setting BIT1 (*f |= 0x02) on the HAND "
           "BONE's flag (53 right / 17 left), inside CorrectItemPosition's hook, "
           "immediately before re-reading that bone's world. INV-48 measured "
           "GetElementWorldMatrix lagging ~60-100 ms in motion (~0.001 m at rest, "
           "~0.3 m walking) and established it is REAL, not sampling skew. The "
           "resolver returns cachedWorld while bit1 is CLEAR and only recomposes "
           "when it is SET, so this tests whether the cache IS the lag. NOTE THE "
           "DIRECTION: bit1 SET forces a rebuild; clearing it (`&= ~2`) is "
           "the OPPOSITE operation, used after writing a world to assert it is "
           "authoritative. [REFRESH] reports err BEFORE and AFTER in the SAME "
           "sample, so the control needs no second run. SAMPLED 1-in-9, because a "
           "world read is not passive (INV-28: a 1 Hz version was visible in the "
           "headset as a pulse). Default 0."),
    S_BOOL("rig_distal_bit1_poke", RigDistalBit1Poke,
           "Rigging: U-2 - poke the item anchor's bit1 only", "Camera hook",
           "Needs rig_distal_flag_handshake=1. Sets BIT1 ONLY (*f |= 0x02) on the "
           "item anchor (71 from 53, 35 from 17), leaving bit0 alone - unlike run "
           "C's (f & 0xfe) | 0x0e, which ALSO cleared bit0 and made the weapon "
           "DISAPPEAR. That run changed two things at once and U-2 has been open "
           "on why ever since; this separates them. Instrumented: reads the "
           "anchor's world, pokes, reads again - the second read is what triggers "
           "the engine's rebuild, since the resolver early-outs on (*flag & 2)==0 "
           "- then compares |anchor-hand| against the authored 0.0970. Lands on "
           "0.0970 and bit1-only is sound; lands elsewhere and the +0x530 notify "
           "is the route after all. Reads are throttled to 1 Hz per side; the "
           "poke itself is a byte OR, the same shape as the element-48 handshake "
           "already running on this path at ~19,461 calls/sec. DEFAULT OFF: it "
           "writes engine memory on a hot path and the last attempt vanished the "
           "weapon. EXPECT the weapon to be offset - that is the handshake, not "
           "this."),
    S_BOOL("rig_distal_flag_handshake", RigDistalFlagHandshake,
           "Rig: handshake the DISTAL arm bones (49/53, 13/17)", "Camera hook",
           "THE PREDICTED FIX FOR THE HELD-ITEM ARM FAULT. DEFAULT OFF - predicted, "
           "never run in a headset, so off keeps the next run a clean A/B. Chain, "
           "every link read from OUR OWN binary: CorrectItemPosition reads the "
           "character's R_HandHolder/L_HandHolder WORLD matrix every frame and ONLY "
           "when an item is held; that read runs the world resolver "
           "(engine RVA 0x4862f0), which returns the CACHED world when bit1 is "
           "clear and, when it does rebuild, CLEARS bit1 on every ancestor it "
           "resolves; so with an item the chain 71<-53<-49<-48<-45 is resolved from "
           "the ANIMATION's local and its bits cleared BEFORE we write; we then "
           "write 48/49/53 and only 48 (and 12) re-set bit1, so 49/53 keep serving "
           "a cached world built from the pre-write pose. [ARMCOMP] v3 measured "
           "exactly that: bit1 CLEAR on 100% of samples, world != parent x local, "
           "ONLY with an item, ZERO failures empty-handed, race-guarded, across "
           "eight item alternations - and the mechanism retro-predicts the bow "
           "inversion (bow in the LEFT hand broke the LEFT arm 0.38 and left the "
           "right clean at 0.025) which it was not fitted to. Writes the engine's "
           "own (flags & 0xfe) | 0x0e idiom, the same constant element 48 uses, "
           "with the same caveat that bits 2 and 3 are copied verbatim and their "
           "meaning is unknown. The handshake COUNT is reported beside the regime "
           "in [FLAGARM], so 'off' and 'on but never fired' cannot look alike."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_arm_compose", DiagArmCompose,
           "Diagnostics: the arm compose, as a ROTATION", "Camera hook",
           "THE GAP EVERY OTHER ARM PROBE IS SILENT ON. Six mechanisms are "
           "eliminated and the drawn arm is still not ours with the elbow 0.207 m "
           "off - and EVERY quantity certifying the arm healthy is a LENGTH or a "
           "DOT, never a rotation. |clav-upper| holds 0.1719 to four decimals while "
           "element 48's world ROTATION could be arbitrarily wrong. [SPINE]'s "
           "compose check covers elements 0-7 only. [ORTHO] does test 45->48 and "
           "prints UNDECIDABLE whenever cu and ul agree - which rig_restore_e48_len "
           "now guarantees by pinning them, so THE FIX THAT SOLVED THE DISPLACEMENT "
           "DISABLED THE INSTRUMENT. This composes world_parent x local_child and "
           "compares it to world_child as an ANGLE, for 45/48/49/53 and 9/12/13/17, "
           "with hspine1 and both clavicles as upstream CONTROLS that must stay "
           "clean. PASSING VALUE: angle <= 0.05 deg everywhere -> the engine is "
           "self-consistent and the gap is on our side; large on the arm while the "
           "controls stay clean -> the arm is not composed as parent x local and "
           "that is the mechanism; large EVERYWHERE -> the probe is wrong, not the "
           "engine, and it says so. The engine-consistency half is pure engine "
           "state and RUNS IN FLATSCREEN. Pure read."),
    S_BOOL("diag_buffer_cmp", DiagBufferCmp,
           "Diagnostics: buffer 0 vs buffer 1 (per-thread)", "Camera hook",
           "THE LAST UNTESTED LINK. Our 3x3 writes survive 96% (and HIGHER where the "
           "arm is more broken), bone lengths are pinned, bases orthogonal, compose "
           "exact, the layer stack constant - and the drawn arm still is not ours. "
           "ENGINE_ARCHITECTURE.md has warned since 2026-09-05 that FUN_18009A520 "
           "picks between this+0x10 and this+0x20 by a PER-THREAD index, and that "
           "WHICH BUFFER A WRITE LANDS IN MAY DEPEND ON WHICH THREAD PERFORMS IT. "
           "Every rig path uses buffer 0 (inner+0x398); buffer 1 (inner+0x3A8) has "
           "never been read. [ARRAYS] did NOT test this - it compared the three "
           "arrays WITHIN one container. NOTE: comparing the two buffers against "
           "EACH OTHER proves nothing, because double buffering means they hold "
           "different frames and a difference is EXPECTED; this probe therefore "
           "compares each buffer against WHAT WE WROTE. Two halves: the STRUCTURAL "
           "one (does buffer 1 exist and is it distinct) is engine state and RUNS IN "
           "FLATSCREEN, and can kill the hypothesis for the price of a flat launch; "
           "the WRITE one needs the rig and therefore VR, and REFUSES BY NAME in "
           "flat rather than printing zeros. Pure read."),
#endif
    S_BOOL("diag_render_sync_watch", DiagRenderSyncWatch,
           "Diagnostics: [RSYNC] does the render-sync snapshot capture the clobber?", "Camera hook",
           "Stage 0 of docs/SPEC-RENDER-SYNC-HOOK-2026-09-26.md. READ-ONLY detour on "
           "engine+0x487190, the only writer found for altMatrices - the array the render "
           "thread (ctx 2, INV-239) draws (INV-240). At each sync of the PLAYER's container it "
           "checks whether the local about to be published is OURS, with CLOBREG's own 0.0005 "
           "test and regime buckets, so the rate compares cell for cell with INV-235. "
           "HEADSET-ONLY: needs the rig solving. With this OFF no detour is installed at all."),
    S_BOOL("diag_resolver_store", DiagResolverStore,
           "Diagnostics: [RSTORE] which store dl_WorldResolver hands back", "Camera hook",
           ">>> FLAT-SAFE AT BOTH BUILT SITES. Read the verdict PER SITE (PROBE_DEPS "
           "RULE 0c): S1 is the ITEM hook, which INV-178 already ran in FLAT; S2 is "
           "the rig tick, the same site and model source as [BUFCMP] and [RESTREAD], "
           "both of which produced flat results. The classification is POINTER "
           "IDENTITY, which INV-11 admits in flat - NO numeric world-matrix claim is "
           "made and none is needed. S3, the submit path, is NOT BUILT: it is "
           "headset-only and bundling it with flat-reachable sites is exactly what "
           "D-106 paid for. <<< WHY IT EXISTS: D-106 concluded container+0x18 is "
           "unread by anything we run on, from [TGROUP] reading g_ThreadGroupIndex==0 "
           "on eight threads - but INV-213 establishes the branch that RETURNS "
           "container+0x18 is gated on the CThreadContextManager's ctx id, a "
           "DIFFERENT TLS slot (INV-211: different object, type, TlsAlloc and "
           "module). That verdict is void and this half of U-19 has no measurement. "
           "HOW IT AVOIDS REPEATING THE MISTAKE: it does not read a thread-local for "
           "its verdict at all. INV-214 - GetElementWorldMatrix TAIL-CALLS the "
           "resolver, so the pointer g_getWorld already returns IS the store the "
           "resolver chose; classify it against container+0x18/+0x20/+0x30 "
           "(+slot*0x30) on BOTH buffers and nothing is modelled. Immune to INV-5 "
           "and INV-212. The ctx id is printed BESIDE it as corroboration only. "
           "PRE-REGISTERED (spec s6): PREV* 0 with refused 0 closes U-19's prev-frame "
           "half FOR THE LOGGED TIDS ONLY; any PREV* > 0 means the ctx==2 branch "
           "fires; ctx==2 with zero PREV* is a CONTRADICTION and neither half may be "
           "reported; ctx all-zero at S1 contradicts INV-178's >=2 and means suspect "
           "THIS probe first; STATIC>0 is INV-214's reject path being read as data "
           "and is a SEPARATE defect. COST: no RttiVtableOf, no WatchExec slot, no "
           "debug register, no hook, no patch, no write. pTCM resolved ONCE by NAME, "
           "never a hardcoded RVA. Windowed 1 Hz per site, 3 elements (8/48/53). "
           "Spec: docs/PROBE-SPEC-RESOLVER-STORE-2026-09-24.md. Pure read."),
    S_BOOL("rig_subtree_rest", RigSubtreeRest,
           "Rigging: pin the arm's UNDRIVEN children to their authored local", "Camera hook",
           ">>> A-1 of docs/PROPOSAL-FIX-SHAKE-AND-SNAP-2026-09-24.md. THE SPRINT SHAKE'S "
           "MEASURED CAUSE, not a guess. [RELCHURN] in the flat run of 2026-09-24 12:55 "
           "ranked every element by travel in EYECAMERA-LOCAL space, with its own validity "
           "check PASSED (basis column scale 1.0000..1.0000 UNIT, 0 degenerate reads): "
           "r_uparmtwist 717.6 deg/s step 37.88 deg, r_foretwistt 660.6 deg/s step 33.73 "
           "deg, l_finger23 5.182 m/s jump 0.5623 m. THE RIG DRIVES FOUR ELEMENTS PER ARM "
           "({48,49,50,53}/{12,13,14,17}); THE ARM SUBTREE HAS ~24. foretwist1 and "
           "foretwistt are CHILDREN of the twist we DO write and are FOREARM-MESH SKINNING "
           "bones, so the forearm's deformation is driven by bones we never touch - which "
           "is Jay's 'I can visibly see the forearms moving' and why the rig's own numbers "
           "read clean (they only measure the four we write, INV-37). <<< WHAT IT DOES: "
           "latches each of {51,52,64,72}/{15,16,28,36} authored LOCAL once from the mesh "
           "asset (AuthoredLocalForElement, INV-148/INV-150 - NOT a live read, which is "
           "INV-4's trap) and writes those twelve constants every frame, so the child rides "
           "its parent rigidly. It is done through "
           "the LOCAL array instead of world recomposition: no world read, no inversion, no "
           "multiply per frame, and IDEMPOTENT - which is why no re-entrancy guard is needed "
           "(composing from a live parent world needs one; INV-4's SPIN was "
           "that bug). NOT IN THE SET, DELIBERATELY: the handholders 71/35 (the item path "
           "owns them - D-85/INV-171/175, a second writer is how D-90 put the weapon off "
           "screen), the 17 fingers per hand (that is A-2), and ANY flag poke (bit 1 would "
           "be needed for the engine to rebuild the world from our local, and D-90's "
           "|= 0x0e is what sent the weapon off screen). PASSING VALUE, FLAT: [RELCHURN] "
           "ROT travel for uparmtwist/foretwistt COLLAPSES from 660-717 deg/s toward their "
           "parent's - not to zero, because the parent is still animated in flat. If "
           "nothing changes, the local write is not reaching the world and the bit-1 "
           "handshake is the reason. Refuses per element BY NAME on a parent mismatch or a "
           "failed asset read. DEFAULT OFF."),
    S_BOOL("rig_subtree_fingers", RigSubtreeFingers,
           "Rigging: pin the FINGERS to their authored pose too (A-2)", "Camera hook",
           ">>> A-2. NEEDS rig_subtree_rest=1 - this gate only adds the 32 finger pins (16 "
           "per hand: 54..63,65..70 and 18..27,29..34), EVERY JOINT IN EVERY CHAIN, because "
           "pinning only the proximal joint leaves its children adding rotation on top, "
           "which is the defect itself. <<< WHY THE FINGERS AND NOT SOMETHING ELSE: after "
           "A-1 shipped, a PAIRED same-window [RELCHURN] comparison showed foretwistt and "
           "hand1 were ALREADY rigid with respect to their parents before any pin (travel "
           "diff 0.00 deg/s over 4 windows), and only uparmtwist had independent motion "
           "(117.22 deg/s / 17.62 deg apart from r_upperarm, now 0.18 / 0.00). The motion "
           "that is still independent and still large is the FINGERS: r_finger13 836 deg/s "
           "with 179 deg SINGLE-FRAME steps, l_finger23 5.18 m/s with 0.38 m jumps, in "
           "eyecamera-local space, undriven. THE TRADE, STATED: pinned to the AUTHORED local "
           "the fingers become a STATIC hand shape that rides the hand bone - the flailing "
           "stops and no grip animation survives. Driving grip from the trigger/grip axis is "
           "a later, separate feature; this is the half that stops the defect. Reverting is "
           "one setting. PASSING VALUE: paired same-window child-vs-parent |travel diff| and "
           "|maxStep diff| -> ~0 for every finger, and the finger rows leave the top of the "
           "[RELCHURN] boards. NOT a cross-run peak comparison - INV-8. DEFAULT OFF."),
    S_BOOL("rig_subtree_grip", RigSubtreeGrip,
           "Rigging: re-latch the finger pins from the LIVE grip on equip", "Camera hook",
           ">>> THE FIX FOR INV-216's MEASURED DEFECT. Jay, in the headset 2026-09-24: 'Do the "
           "fingers stop moving? Yes. However it also cancels out the grip pose for the "
           "weapons.' rig_subtree_fingers pins each finger to its AUTHORED local, and the "
           "authored shape is an OPEN HAND - correct for an empty hand, wrong for a held "
           "weapon. <<< WHAT THIS DOES: watches each hand's item pointer; on a change it "
           "RELEASES that hand's 16 finger pins so the animation can close the hand around "
           "the new item, waits rig_subtree_grip_delay_ms for the equip animation to settle, "
           "then latches the LIVE local ONCE and holds it. An empty hand restores the "
           "authored copy, which is kept separately and never overwritten. WHY THE DELAY: "
           "the grip closes OVER the equip animation, so the first frame after the change is "
           "a mid-transition half-closed hand and pinning it would look worse than the "
           "flail. WHY THE CAPTURE RUNS IN ApplyDrivenBones AND NOT THE TICK: the live local "
           "must be read BEFORE our own write for that element lands this frame - INV-4, 'a "
           "rest pose read from a bone the rig drives must be LATCHED BEFORE THE FIRST "
           "WRITE'; reading it from the tick would read back our own previous output, which "
           "is the shape of the continuous-SPIN bug. Needs rig_subtree_rest=1 AND "
           "rig_subtree_fingers=1. Logs [GRIP R/L] on every release and capture, and says "
           "'ZERO CAPTURED' and leaves the hand UNPINNED rather than pinning garbage. "
           "DEFAULT OFF."),
    S_INT("rig_subtree_grip_delay_ms", RigSubtreeGripDelayMs, 0, 3000,
          "Rigging: ms to wait after an equip before latching the grip", "Camera hook",
          "How long the fingers stay on the animation after an item change before the grip "
          "pose is captured. Too short latches a half-closed hand mid-equip; too long leaves "
          "the animation's finger flail visible for that window. 400 ms is a starting value "
          "and has NOT been tuned against a measurement - the equip animation's length is "
          "[UNKNOWN] in our build. Only does anything when rig_subtree_grip=1."),
    S_BOOL("rig_subtree_fingers_held_release", RigSubtreeFingersHeldRelease,
           "Rigging: do NOT drive the fingers of a hand that is holding something", "Camera hook",
           ">>> THE RULE, at Jay's instruction 2026-09-24: whichever hand the weapon is in, we do "
           "not drive THAT hand's fingers while it is in the hand. The 16 finger pins for that "
           "hand are released on pick-up and stay released until the hand is empty, at which point "
           "they return to the authored pin. The twists, hand1 and the other hand are untouched, "
           "so the empty-hand win from INV-216 is kept and the weapon is gripped by the animation "
           "that owns it. <<< WHY THIS REPLACES THE CAPTURE ROUTE RATHER THAN TUNING IT: INV-226. "
           "Across 29 captures the canonical grip stored ZERO real poses, because the accessor "
           "returns the CACHED world (INV-215) and a finger's cache was last rebuilt from OUR OWN "
           "authored local (INV-223) - so every store we can read hands our own pose back and the "
           "animation's grip is unreachable from anywhere we read. TWO capture designs failed for "
           "that one reason; a third would fail the same way. THE COST, STATED: while a weapon is "
           "held, that hand's fingers move with the animation again - including the flail INV-216 "
           "removed. That is the trade Jay chose over a broken grip, and authoring our own curl "
           "per joint is the eventual answer, not another capture. DEFAULT ON."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_pole_watch", DiagPoleWatch,
           "Diagnostics: [POLEWATCH] does the elbow-pole cancellation act?", "Camera hook",
           ">>> U-40, and it tests INV-233, which is a STATIC read with zero runs. The elbow pole is "
           "pole = (1-w)*world_down + w*(-handPerp), both unit - AND IT IS NEVER RENORMALISED after "
           "that blend. axis = dir x pole is then DIVIDED BY al = |pole|*sin(theta), so |pole| sets "
           "how hard a wobble in handAxis is amplified into the elbow plane: ~1/|pole|. The worst "
           "case is |1-2w|, which is 0.40 at the right's rig_elbow_follow_hand=0.70 but only 0.10 at "
           "the left's rig_left_elbow_follow_hand=0.45 - a 4x difference, and at w=0.50 exactly the "
           "pole VANISHES and the axis is undefined. 0.45 was chosen because any w <= 0.5 makes the "
           "OLD elbow-above-shoulder cliff mathematically impossible, and it does - but the closer w "
           "is to 0.5 the more completely the two halves cancel, so the fix for one degeneracy sits "
           "next to another. <<< AND NOTHING IN THE CODE CAN SEE IT: g_poleClamped counts only the "
           "pole[1] clamp (boneblend.cpp:9744) and g_axisFlips only an INVERSION PAST 90 DEG "
           "(:9780), while INV-232 measured SIX output jumps of 0.309-0.827 with flips constant and "
           "clamps 0. THIS REPORTS, per side per second: |pole| min/max, al min, the counts under "
           "0.50/0.25/0.12, and unconditional counters for the two branches that have never had one "
           "- D1 (hand term dropped, hpl <= 1e-3) and D3 (blended pole THROWN AWAY for world +Z, "
           "al < 1e-3), plus how often D3 twice ABANDONED the solve. It also feeds |pole| and al "
           "into [SNAPDUMP]'s ring so a dump shows them ON THE JUMP FRAME - which is the only "
           "reading that answers U-40, because a small minimum somewhere in the run is not the same "
           "claim as a small |pole| at the snap. PRE-REGISTERED BOTH WAYS: min |pole| falling toward "
           "0.10 on the LEFT while the RIGHT holds near 0.40, or D1/D3 firing at all, means the "
           "mechanism ACTS; min |pole| staying near 1 on both sides with D1/D3 never firing means "
           "THIS STAGE IS INNOCENT and INV-233 is a static curiosity. HEADSET-ONLY (INV-11) - it "
           "needs the solve to reach the pole stage, so a flat run prints NO SAMPLES and nothing "
           "else. Log-only, no behaviour change, costs one VecLen the cross product already needed. "
           "DEFAULT OFF."),
#endif
    S_BOOL("diag_stale_clav", DiagStaleClav,
           "Diagnostics: [STALECLAV] does the stale clavicle carry the arm?", "Camera hook",
           ">>> INV-246. The solve runs on the scePad poll at ~686/s, ASYNCHRONOUS to the ~80/s game "
           "update. It samples the clavicle's world and builds the upper, forearm and hand LOCALS "
           "relative to that sample; ApplyDrivenBones later writes them verbatim inside the animation "
           "pass. The clavicle is not driven (rig_clavicle_share=0), so d7380 re-poses it under our "
           "locals and the drawn arm is our solution carried by the clavicle's motion since the "
           "sample. <<< THIS MEASURES THE SIZE OF THAT. Per side, it keeps the last DISTINCT clavicle "
           "sample (consecutive solves inside one update read the same value and are counted, not "
           "measured) and for each new distinct sample reports, IN THE EYE BONE's FRAME (v2 - v1 "
           "worked in world space and counted the whole body moving, INV-246 result block), the "
           "view-relative clavicle's rotation and origin step, and the displacement it carries to "
           "the solve's own target: Crel = E^-1 o C, trel = E_old^-1(T_old), "
           "|Crel_new(Crel_old^-1(trel)) - trel|, with full 3x3 inverses so baked scale cannot "
           "falsify it. Filed by still/moving (diag_rig_counters) x empty/melee(WeaponVis)/other "
           "(right hand's held class). Gaps over 200 ms are skipped and counted. PRE-REGISTERED: "
           "moving+melee median carried displacement in the CENTIMETRES and >= 3x moving+empty -> "
           "the mechanism has the size to be the shake; under ~5 mm in moving+melee -> it is too "
           "small and INV-246 is not the shake. HEADSET-ONLY - a flat run prints NO SAMPLES. "
           "Log-only, no behaviour change, no accessor call. DEFAULT OFF."),
    S_BOOL("diag_drawn_census", DiagDrawnCensus,
           "Diagnostics: [BODYVIEW] what shakes in the drawn body? + [TICKGAP]", "Camera hook",
           ">>> INV-243/244/245: what is drawn is altMatrices (container+0x18), filled by "
           "dl_BatchWorldBuild after each completed update and read by the skinning job at ctx 2. "
           "THIS READS THAT ARRAY RAW - no accessor, no resolver pull, so it cannot perturb what it "
           "measures. Once per PUBLISHED frame (detected as a change in the drawn eye, element 8; "
           "identical re-reads are counted, not measured; a frame whose eye changes during the read "
           "is discarded as TORN), every element of the player's container is expressed relative to "
           "the drawn eye and three things are accumulated per element: STEP (mm moved since the "
           "last frame), JERK (mm, second difference - smooth motion has small jerk, a spasm has "
           "large jerk) and ROTATION (deg). Filed by still/moving (diag_rig_counters) x "
           "empty/melee(WeaponVis)/other. At 1 Hz, per bucket, the TOP 10 elements by mean jerk, by "
           "NAME, plus a fixed line for eye, clavicles, upper arms, forearms, hands and handholders. "
           "<<< ALSO [TICKGAP]: the rig tick (the scePad poll thread that also runs the SOLVE and "
           "every 1 Hz report) - its max duration and max gap per second. If the report tick takes "
           "tens of ms, the solve stalls once a second: the 1 Hz pulse candidate. HEADSET for the "
           "arms. Log-only, no behaviour change. DEFAULT OFF."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_view_pos", DiagViewPos,
           "Diagnostics: [VIEWPOS] view timing and camera position vs the head", "Camera hook",
           ">>> Design D run 2: 'something still feels wrong whilst turning my head'. Once per frame: the real head's "
           "yaw and pitch against the rendered view's (yaw arrives through the engine camera, pitch through camhook), "
           "as a lag in frames/ms from cross-correlating per-frame changes; and the camera's world position against the "
           "tracked head position mapped through the rig's own tracking-to-game yaw (gain, residual, heading error, "
           "largest single-frame jump the head did not cause). Only frames with both sticks centred. HEADSET-ONLY. "
           "Log-only. DEFAULT OFF."),
#endif
    // LOCKED ON 2026-10-07 (Jay: "that's an absolute neccessity to be on ... force enabled and hide the setting toggle for it,
    // that is now considered loadbearing"). Not drawn; the file's value is ignored. The VR Comfort tab still shows its status.
    S_BOOL_LOCKED("declare_rendered_orientation", DeclareRenderedOrientation,
           "Master enable: steady view on head turns (declare rendered orientation)", "VR Comfort",
           ">>> Removes the 'swim' on left/right head turns (SPEC-RENDERED-ORIENTATION, INV-326). LIVE. NEEDS "
           "head_yaw_decouple=1 (restart to change) and use_render_pose=0 - refuses by name otherwise. Tells the "
           "headset, for every frame, the head orientation the game actually rendered it with (it trails your head "
           "by about 2 frames, ~5 degrees on a fast turn), so the headset corrects the difference instead of showing "
           "a late image as current. Frames it cannot pair keep today's pose and are counted by reason once per 5 s. "
           "Best at a headset refresh the game can hold (90 Hz measured; 120 Hz at ~105 fps judders). HEADSET-ONLY. "
           "DEFAULT ON."),
    // LOCKED ON 2026-10-14 (Jay: "That is now a TIER A - So move it & make sure it doesn't get touched by anyone or
    // anything"; verified "That's done it. That's the fix.", INV-562). Not drawn; the file's value is ignored and rewritten
    // to 1. The key keeps its diag_ name because keys are never renamed (D-156) - it is NOT a diagnostic. The worker also
    // re-applies the engine bytes if anything else reverts them (camhook.cpp jitvr::Thread).
    S_BOOL_LOCKED("diag_jitter_vr_exec", DiagJitterVrExec,
           "VR jitter execute fix (textures / decals flashing)", "Camera hook",
           "THE FIX for textures / decals / shadows flashing in VR (INV-562, "
           "verified 2026-10-14). The engine's jitter command switches the "
           "jitter OFF on the context that runs it whenever the headset is "
           "live, while the copies handed to the parallel render jobs keep it "
           "ON - so part of each frame drew without jitter. Removes that VR "
           "test (3 engine bytes). LOCKED ON - not in the menu, file value "
           "ignored. Logs [JITVR]."),
    S_BOOL("diag_render_pose", DiagRenderPose,
           "Diagnostics: [RENDPOSE] which head yaw is inside each submitted frame", "Camera hook",
           ">>> SPEC-RENDERED-ORIENTATION Stage 1 (the turn swim, INV-266). NEEDS head_yaw_decouple=1 and "
           "use_render_pose=0 - refuses by name otherwise. Records the head yaw design D adds into the camera look "
           "each player update, pairs every submitted frame with it by value, and logs once per second: pairs found, "
           "the fitted camera-yaw mapping and its residual, and how far the rendered yaw trails the pose we declare "
           "while turning (what the compositor would correct). HEADSET-ONLY. Log-only - nothing is declared. "
           "DEFAULT OFF."),
#if !defined(DLVR_BETA_LEAN) || defined(DLVR_YAW_PROBE)   // BETA lean: compiled out; back in the yaw probe variant (trace 20261119_2145)
    S_BOOL("diag_cam_yaw", DiagCamYaw,
           "Diagnostics: [CAMYAW] design D's head yaw in the engine camera", "Camera hook",
           ">>> SPEC-HEAD-YAW-DECOUPLE-D. With head_yaw_decouple active, a detour on the player's "
           "camera-look chase (gamedll+0xBA1110) adds the real head yaw to the camera look (+4460) "
           "every player update. This logs the first write unconditionally, then per second: writes, "
           "non-local-player skips, mean/max head yaw added, and how often the camera would NOT be "
           "built from +4460 (eye-bone branch pointers set, flag +4135&4, vehicle state). "
           "HEADSET-ONLY (the detour needs the decouple latched in a VR session). Log-only. DEFAULT OFF."),
#endif
    S_BOOL("head_yaw_look_enable_fix", HeadYawLookEnableFix,
           "Head yaw decouple: keep the body look steady across weapon swaps / climbs / grapples / throws", "Camera hook",
           ">>> INV-329. Only matters with head_yaw_decouple on. Every time the player changes state (weapon swap, "
           "climb, grapple, melee throw) the game re-enables its look controller, which copies the camera's look "
           "direction onto the body's. With the decouple on the camera's look includes your head's yaw, so the view "
           "and body jumped sideways by however far your head was turned, and stayed there. ON: the head yaw is taken "
           "out of the camera look for the duration of that copy and put back after, so nothing jumps. OFF: the old "
           "behaviour, for comparison. LIVE. HEADSET-ONLY. DEFAULT ON."),
    S_INT("look_pitch_anchor", LookPitchAnchor, 0, 2,
           "Look pitch: re-anchor the game's look to the head (INV-272)", "Camera hook",
           ">>> The grapple pitch drift. The head reaches the game's look pitch (+4472) only as per-update deltas, "
           "and the rope's own look controller sets it absolutely during a grapple, so after the hand-back the "
           "game's aim/camera pitch keeps the rope's pitch forever (INV-272). 1 = when control returns to the "
           "normal look controller, set the look pitch to the head's pitch (engine's own reading) once. 2 = also "
           "every update while the normal look controller is active (removes stick pitch and recoil from the look). "
           "0 = off. Rides the same gamedll+0xBA1110 detour as design D (installed when this or the decouple is on). "
           "Read at startup. DEFAULT 0."),
    S_BOOL("diag_look_pitch", DiagLookPitch,
           "Diagnostics: [LOOKPITCH] look-pitch anchor windows", "Camera hook",
           "Per second: game look pitch minus head pitch (min/mean/max) while the normal look controller is active, "
           "hand-backs, anchors, offset jumps > 3 deg, helper-inactive updates. Every hand-back logs the offset the "
           "game would have kept and the offset after. The first sample logs regardless. HEADSET. Log-only. "
           "DEFAULT OFF."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_head_turn", DiagHeadTurn,
           "Diagnostics: [HEADTURN] does a pure head turn turn the body?", "Camera hook",
           ">>> Jay 2026-09-26: turning the head swings the upper arms/shoulders and they STAY "
           "displaced while the head is held turned. This measures which bones follow HMD yaw. "
           "Only ticks with BOTH sticks centred (|x|,|y| <= 0.20) and the root still (< 0.10 m/s) "
           "count; stick turns and locomotion are rejected and COUNTED BY NAME. Per gated tick the "
           "yaw change of the HMD, the root (0), hspine1 (7), the eye bone (8), this side's "
           "clavicle (45/9), the rig's bodyYaw and camhook's camera-minus-head yaw is integrated; "
           "per 1 s window and cumulatively the SLOPE of each against HMD yaw is logged, with the "
           "shoulder's horizontal swing about the root and the controller's range (hands-still "
           "check). Slopes are not computed when the head swept < 5 deg. HEADSET-ONLY. Log-only, "
           "no behaviour change. DEFAULT OFF."),
#endif
    S_BOOL("diag_root_stability", DiagRootStability,
           "Diagnostics: [ROOTSTAB] is C-3's stable base real?", "Camera hook",
           ">>> C-3a of docs/SPEC-C3-SHOULDER-FROM-GEOMETRY-2026-09-24.md, and it exists to stop a "
           "solve rewrite being built on one frame. C-3's entire premise is that the TRANSLATIONS "
           "upstream of the clavicle are stable while its BASIS is not, and the only evidence today "
           "is ONE FRAME of ONE [SNAPDUMP] (INV-225: clav translation moved 0.3 mm while its basis "
           "rotated ~44 deg, and the upper arm's translation swung 13 cm - a 0.0853 m lever times "
           "sin 44 deg plus the chain). <<< THIS MEASURES IT ACROSS A WHOLE RUN: the per-tick "
           "translation STEP of hspine1(7), the clavicle(45/9) and the upper arm(48/12), reported "
           "SEPARATELY per side, with the worst step and the counts over 10 mm and 50 mm. "
           "PRE-REGISTERED: hspine1 and CLAVICLE stay in the millimetres while UPPER shows "
           "centimetre spikes -> C-3's base exists and should be whichever is quieter. THE CLAVICLE "
           "ALSO SPIKES -> C-3's premise is FALSE and the spec is VOID, learned from a log line "
           "instead of a rewrite. COST: nothing new is read. hspine1's world is already fetched "
           "every frame by rig_anchor_from_spine (which is ON); clav and upper come from the solve's "
           "own five reads. Three 3-float copies and three compares per solve, no accessor call, no "
           "behaviour change - it runs BEFORE rig_parent_guard so it measures what the ENGINE "
           "handed us, not what the guard substituted. Says NO SAMPLES - NOT MEASURED rather than a "
           "fake zero when a bone was never read (hspine1 needs rig_anchor_from_spine=1). "
           "DEFAULT OFF."),
    S_INT("rig_pin_clavicle", RigPinClavicle, 0, 3,
          "Rigging: pin the CLAVICLE so the arm's root rides hspine1 (C-1)", "Camera hook",
          ">>> C-1 of docs/PROPOSAL-PART-C-ARM-ROOT-2026-09-24.md. 0=off, 1=RIGHT only, 2=LEFT "
          "only, 3=both. THE MEASURED TARGET: INV-225 - on a [SNAPDUMP] trigger frame the target "
          "held to 0.3 mm and bodyYaw to 0.00 deg while the engine handed our solve a CLAVICLE "
          "rotated ~44 deg and an upper arm rotated ~41 deg, and our output jumped ~46 deg. That is "
          "INV-35's ~6%-of-frames replacement arriving through the solve's INPUTS, and it is BOTH "
          "the snap while turning AND the melee sprint shake. Filtering it failed four times "
          "(D-77, D-78, D-83, and rig_parent_guard - whose forced accepts sat at exactly 1/3 of its "
          "rejections, because the corruption outlives any hold short enough not to lag a turn). "
          "This stops READING the animated clavicle instead: its local is held at the AUTHORED "
          "value so it rides hspine1 rigidly - the same bone rig_anchor_from_spine already derives "
          "the anchor from, so the arm's root and the anchor end up under one parent. <<< PASS "
          "VALUE, MECHANICAL AND WITHIN-RUN: [PGUARD]'s basis rejections COLLAPSE on the pinned "
          "side while the unpinned side stays high - a same-run per-side comparison, not a "
          "cross-run peak (INV-8). Start at 1: the untouched left arm is the CONTROL, and the "
          "bow-era clavicle fault survived five months precisely because the left had no symmetric "
          "instrument (ARCHIVE:4529). ⚠ THE CLAVICLE IS A REAL SKINNING BONE (bone=14 right, "
          "bone=10 left, against bone=255 for the twists and fingers), so the SHOULDER'S SHAPE will "
          "change, not just its orientation - expected, not a fault, and only Jay can price it. "
          "⚠ AND THIS JOINT HAS HISTORY: |l_clavicle -> l_upperarm| once read 0.4162 / 0.7879 / "
          "29.5591 and took five months to solve. Its regression alarm is already running - "
          "[PGUARD]'s offset counter measures exactly that distance and reads 0.0000-0.0001 m "
          "today, so a revival shows up in the same run. The mitigations are also ON "
          "(rig_restore_e48_len, rig_restore_left_len, rig_stable_shoulder). REFUSES BY NAME if "
          "rig_clavicle_share or rig_clavicle_protract_deg is non-zero: the rig drives the clavicle "
          "then (boneblend.cpp:8927) and this pin runs LAST, so it would silently erase that "
          "contribution. DEFAULT 0."),
    S_BOOL("rig_parent_guard", RigParentGuard,
           "Rigging: reject a one-frame corrupted clav/upper read in the solve", "Camera hook",
           ">>> INV-225's FIX, AND THE DEFECT IT IS AIMED AT IS MEASURED TO THE FRAME. [SNAPDUMP], "
           "headset 2026-09-24 17:26: on the trigger frame T moved 0.3 mm, anchor 0.3 mm, bodyYaw "
           "0.00 deg and axisFlips did not step - while the engine handed the solve a CLAVICLE "
           "rotated ~44 deg, an UPPER ARM rotated ~41 deg and its translation moved ~13 cm, and "
           "our output basis jumped ~46 deg. The target was innocent and so was the solver: its "
           "PARENT INPUT was replaced for one frame. That is INV-35's ~6% replacement arriving "
           "through the solve's own inputs, and it is BOTH the snap while turning AND the melee "
           "sprint shake - one defect, two symptoms, with the item gate (INV-222) following from "
           "the animation's amplitude (INV-218, 67x). <<< WHAT IT DOES: compares clav and upper "
           "against the last ACCEPTED pair; if either exceeds rig_parent_guard_basis or "
           "rig_parent_guard_trans_m, BOTH are substituted from that pair - both, because using "
           "last frame's shoulder with this frame's upper arm is a corruption of its own. Within "
           "threshold the values pass through UNTOUCHED: this is an outlier rejector, not a filter "
           "or a rate limit, because the corruption is measured as intermittent and one-frame. "
           ">>> AND THE FAILURE MODE OF MY OWN YAW SLEW LIMITER IS DESIGNED OUT: that one held its "
           "clipped output as state while bodyYaw was recomputed upstream every frame, the two "
           "diverged, and it ended up clipping 61% of ticks. This one is BOUNDED - after "
           "rig_parent_guard_max_hold consecutive rejections it FORCE-ACCEPTS whatever the engine "
           "says and re-baselines, counting those separately - so a real fast turn costs at most N "
           "frames of hold and never a runaway. At ~350 ticks/s a 180 deg/s turn moves the basis "
           "~0.009 per tick against a 0.25 limit, i.e. 28x inside budget. <<< It runs AFTER "
           "[SNAPDUMP]'s snapshot so the dump keeps showing RAW engine values and this guard can "
           "never hide what it filtered. Reports [PGUARD R/L] with reads, rejections split by "
           "which term tripped, forced accepts, and the WORST deltas seen against the limits - so "
           "'never rejected' can be told apart from 'threshold too high'. DEFAULT OFF."),
    S_FLOAT("rig_parent_guard_basis", RigParentGuardBasis, 0.02f, 1.0f,
            "Rigging: [PGUARD] basis-term jump that counts as corruption", "Camera hook",
            "Largest single basis-term change on clav or upper between consecutive solves. The "
            "measured corruption moved terms by 0.28-0.62 (a ~44 deg rotation); ordinary motion at "
            "~350 ticks/s moves them ~0.009 for a fast 180 deg/s turn, so 0.25 sits 28x above "
            "normal and well below the fault. Lower it only if [PGUARD]'s 'worst delta seen' shows "
            "the fault slipping under the limit. Only meaningful when rig_parent_guard=1."),
    S_FLOAT("rig_parent_guard_trans_m", RigParentGuardTransM, 0.001f, 0.5f,
            "Rigging: [PGUARD] change in the clav->upper BONE LENGTH that counts as corruption",
            "Camera hook",
            ">>> CORRECTED 2026-09-24 AFTER A MEASURED FAILURE, AND THE FAILURE IS WORTH KEEPING. "
            "v1 compared the ABSOLUTE WORLD TRANSLATION of clav and upper, which is dominated by "
            "the player simply moving: it rejected 10,935 of 27,279 frames (40%) and its 3,649 "
            "forced accepts consumed the shared hold budget, DISABLING the basis test - the guard "
            "was off exactly when it mattered, and the snap still fired 3 times. That is INV-208's "
            "lesson (relative vs absolute) which I had quoted all day and then failed to apply. "
            "<<< NOW it tests the clavicle -> upper-arm OFFSET LENGTH, which is a BONE LENGTH "
            "(~0.0853 m in the element table) and is therefore CONSTANT under all locomotion: body "
            "motion cancels exactly and it cannot fire on movement. A one-frame change in a bone "
            "length means the chain was replaced, which is the fault. 0.01 m is generous for a "
            "quantity that should never change at all. Only meaningful when rig_parent_guard=1."),
    S_INT("rig_parent_guard_max_hold", RigParentGuardMaxHold, 1, 30,
          "Rigging: [PGUARD] consecutive rejections before it gives up and accepts", "Camera hook",
          "THE BOUND THAT KEEPS THIS FROM BECOMING THE YAW SLEW LIMITER. After this many "
          "consecutive rejections the guard force-accepts the engine's value and re-baselines, so "
          "it can never hold a stale parent indefinitely against a source that keeps moving. 3 is "
          "chosen because INV-35 measures the replacement as ~6% of frames, i.e. isolated rather "
          "than sustained. If [PGUARD]'s 'forced accepts' climbs as fast as its rejections, the "
          "corruption lasts longer than 3 frames and THAT is the finding - do not just raise this "
          "number. Only meaningful when rig_parent_guard=1."),
    S_BOOL("rig_subtree_grip_canon", RigSubtreeGripCanon,
           "Rigging: reuse the last REAL grip when a capture returns authored", "Camera hook",
           ">>> BUILT ON A MEASUREMENT, NOT A GUESS. Of 12 grip captures on 2026-09-24 17:04, "
           "exactly ONE returned a real pose (max basis term 0.64266) and eleven returned the "
           "authored open hand to 0.00000 - and Jay, independently: 'I saw the grip work ONCE.' "
           "The cause is INV-215/INV-223: GetElementWorldMatrix hands back the CACHED world, and "
           "a finger's cache was last rebuilt from OUR OWN authored local, so composing it back "
           "returns authored. The animation's finger pose only reaches the world occasionally. "
           "<<< WHAT THIS DOES: when a capture DOES see a real grip it is kept per hand; when a "
           "later capture returns only authored, that stored grip is substituted instead. One "
           "success becomes every equip, and the pose used was MEASURED FROM THE GAME rather than "
           "invented. Every substitution is LOGGED - a pose that did not come from the current "
           "item must never be silent - so if one weapon grips wrong you can see exactly why, and "
           "the answer is a per-item canon rather than a longer delay. Does nothing unless "
           "rig_subtree_grip=1 AND a real grip has been captured at least once this session; the "
           "1 Hz [GRIP] line says so explicitly when nothing is stored yet. NOT a dirty-bit poke: "
           "forcing the engine to rebuild the local from the world is D-90's territory, where "
           "|= 0x0e on elements 71/35 put the weapon off screen. DEFAULT ON."),
    S_FLOAT("rig_yaw_slew_max", RigYawSlewMax, 0.0f, 2000.0f,
            "Rigging: per-tick rate limit on bodyYaw, degrees per second", "Camera hook",
            ">>> INV-217's FIX, AND THE MEASUREMENT IT IS AIMED AT. [YAWNOISE], headset "
            "2026-09-24, 80 windows with TURNING data: the camera yaw we build the IK target "
            "from is a STAIRCASE - HELD on 75.3-76.7% of ticks and STEPPING on the rest, "
            "worst single-tick step median 3.882 deg, p90 6.835, clean max 11.651 - and it "
            "arrived RAW because rig_yaw_camera_smooth=1 makes the correction filter a "
            "pass-through ([SWAY] read bodyYaw == camera yaw to +0.0 deg). Since T = anchor + "
            "Ry(bodyYaw)*(controller - head), each step rotates the whole ~0.5 m lever: 34 mm "
            "at the median worst, 101 mm at 11.651 deg, in ONE tick, ONLY while turning - the "
            "shape and the trigger of 'snaps hands back every few moments whilst turning'. "
            "<<< 270 deg/s: at the "
            "300-370 ticks/s measured in that run the budget is ~0.73-0.90 deg/tick, so a "
            "fast 180 deg/s real turn costs ~0.5 deg/tick and passes UNTOUCHED while the "
            "4-12 deg stair steps get clipped. >>> THIS IS NOT D-78. That entry rejected "
            "limiting the ANCHOR POSITION, and its reason was that the anchor's jump is "
            "COMMON-MODE - the whole cached skeleton advances at once - so limiting it turned "
            "shared motion into relative error and produced 'arms lagging behind and snapping "
            "back'. This limits an ANGLE applied to a lever whose anchor end is untouched; "
            "there is no shared term to break, and INV-208's 51 mm body-relative anchor step "
            "is not affected by this code at all. <<< A slew limit rather than more smoothing "
            "because an EMA lags in proportion to turn rate and leaves the arms trailing "
            "during a sustained turn, while a limiter passes anything within budget "
            "untouched. Past rig_yaw_teleport_deg the step is taken WHOLE and counted, so a "
            "level load or a cutscene cut does not drag the arms around for a second (D-78's "
            "other lesson). Reports [YAWSLEW R/L] with ticks, clipped %, teleports, the worst "
            "RAW step and the worst amount CUT - and says NEVER CLIPPED explicitly rather "
            "than letting a zero read as a success. 0 = OFF, DEFAULT OFF."),
    S_INT("rig_yaw_stick_gate", RigYawStickGate, 0, 3,
          "Rigging: keep the body's walk wobble out of the arms", "Camera hook",
          "0 = off (the arms' facing is built exactly as before). 1 = while you walk or sprint without "
          "turning, the arms take their facing from the camera alone, which stays still, instead of from "
          "the character's hips, which wobble with every step (measured 0.25-3.17 degrees per update, "
          "INV-287); while the right stick turns you, it switches back to the previous behaviour, which "
          "brings the wobble back mid-turn. 2 = the camera alone ALL the time, also while turning, so "
          "aiming holds steady through a turn; while turning it just follows the camera more quickly "
          "(rig_yaw_turn_smooth) - rejected in the headset, it jitters (D-145, D-146). 3 = as 1 when not "
          "turning; while turning, the arms follow the camera's own unfiltered value, re-timed to the arm "
          "rig's update so it moves smoothly (INV-292). The switch is eased over rig_yaw_gate_ramp_ms. Needs "
          "rig_yaw_camera_blend=1 and says so in the log if it is not. Takes effect at launch."),
    S_FLOAT("rig_yaw_turn_smooth", RigYawTurnSmooth, 0.02f, 1.0f,
            "Rigging: how closely the arms follow the camera while you turn", "Camera hook",
            "Only used when rig_yaw_stick_gate=2. 1 = the arms follow the camera exactly while the right "
            "stick turns; lower = smoother but the arms trail the view slightly. 0.5 is the default: about "
            "2 degrees of trail in a normal turn, and a rare single-frame jump in the camera arrives halved. "
            "Takes effect at launch."),
    S_FLOAT("rig_yaw_gate_ramp_ms", RigYawGateRampMs, 20.0f, 1000.0f,
            "Rigging: how quickly the turn switch eases in and out, milliseconds", "Camera hook",
            "Only used when rig_yaw_stick_gate=1. How long the arms take to go from the walking behaviour "
            "to the turning behaviour and back when you move or release the right stick. 150 is the "
            "default. Too short can show a small step when you start or stop turning; too long lets a "
            "little wobble in at the start and end of a turn."),
    S_FLOAT("rig_yaw_teleport_deg", RigYawTeleportDeg, 10.0f, 180.0f,
            "Rigging: yaw step above which the slew limiter gives up and snaps", "Camera hook",
            "Degrees, single tick. Above this the step is taken WHOLE and counted as a "
            "teleport instead of being slewed - a level load, a respawn or a cutscene cut is "
            "not a turn, and slewing one would drag the arms across the screen for a second. "
            "The 115.353 deg single-tick step seen at 15:43:06 on 2026-09-24 was exactly such "
            "an event (it coincides with the [POSEMISS] dropout, and INV-217 excludes it from "
            "the staircase numbers for that reason). Only does anything when "
            "rig_yaw_slew_max is above 0."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_pose_miss", DiagPoseMiss,
           "Diagnostics: [POSEMISS] controller tracking dropouts", "Camera hook",
           ">>> B-1 of the same proposal, and it settles the SNAP-WHILE-TURNING question in "
           "one headset run. THE MECHANISM, TRACED IN OUR OWN SOURCE: controllers.cpp:692 "
           "sets g_valid=false when xrLocateSpace returns without both valid bits; "
           "GetControllerPose then returns false; SolveArmIk does 'return false' at "
           "boneblend.cpp:6909; and g_drivenCount is published ONLY on a successful solve "
           "(boneblend.cpp:10593) - so the rig keeps writing LAST frame's rotations while "
           "the body and heading keep turning, then jumps to the controller in one frame on "
           "re-acquisition. The jump is proportional to how far you turned during the "
           "dropout, which is why it shows up TURNING and why it is aperiodic rather than "
           "time-gated. The runtime here is inside-out (virtualdesktop-openxr): turning "
           "sweeps the hands out of the headset cameras' FOV. <<< The counters accumulate "
           "UNCONDITIONALLY (two interlocked increments per frame per hand) so the number "
           "exists in every log; this setting gates only the LOG line. It reports frames, "
           "misses, miss %, BOUTS and the longest run separately - 600 one-frame misses and "
           "one 600-frame miss are different defects and a total cannot separate them "
           "(INV-5's family). FLAT prints 'NO FRAMES LOCATED - NO VERDICT' rather than a "
           "fake zero: xrLocateSpace never runs there. HEADSET for a verdict. DEFAULT OFF."),
    S_BOOL("diag_snap_dump", DiagSnapDump,
           "Diagnostics: [SNAPDUMP] catch the turning snap, whatever causes it", "Camera hook",
           ">>> Dumps 30 frames when the output rotation jumps > 0.7 rad, at most 3 "
           "times a session. It exists because FIVE "
           "single-hypothesis probes have now failed on this symptom. Jay: 'IT IS NOT A CONSTANT "
           "SNAP ... IT HAPPENS MOMENTARILY AND VERY QUICKLY. TURNING IN EITHER DIRECTION CAUSES "
           "IT.' DEAD by measurement: the anchor guard, the controller dropout (2 bouts in 3 "
           "minutes), a mid-run shoulder re-latch (exactly 2 latches) - all D-110 - and the yaw "
           "staircase, which survived smoothing at 0.15 AND clipping at 270 deg/s and turned out "
           "to be the slight JITTER instead (INV-224). The plane-normal flip DOES fire but at 2 "
           "per 73 s and LEFT ONLY, which does not match 'every few moments' on both hands. <<< "
           "THIS ONE TRIGGERS ON THE OUTPUT - the upper-arm basis we actually write - so it fires "
           "for ANY cause on EITHER arm and hands back the inputs that produced it. The dump is "
           "30 frames in TIME ORDER with T, anchor, bodyYaw and the RUNNING axisFlips/poleClamped "
           "counters, and it prints how to read itself: if T or anchor jumped on the trigger "
           "frame the snap is in the TARGET and the solver is faithful; if they did NOT while the "
           "basis did, it is INSIDE the solve (pole, axis, reach) and the target is innocent; if "
           "axisFlips stepped, it was the 180 deg roll. Those three are mutually exclusive and "
           "ONE dump decides between them. COST: 28 float stores and 18 compares per SOLVE "
           "(~350/s/side, not ApplyDrivenBones' ~12,000/s), no trig on the per-frame path - the "
           "radian threshold is converted to a basis-term delta once - no allocation, no accessor "
           "call, no WatchExec slot. The angle is only computed on a trigger. FAILS CLOSED: the 1 "
           "Hz [SNAPDUMP] line reports the WORST jump seen even when nothing triggered, so 'no "
           "dumps' can never be read as 'no snaps' when the threshold was simply too high, and it "
           "says NO FRAMES RECORDED rather than staying silent when the solve never ran (which is "
           "what FLAT does - this needs a HEADSET). DEFAULT OFF."),
#endif
    S_FLOAT("diag_snapdump_rad", DiagSnapDumpRad, 0.05f, 3.0f,
            "Diagnostics: [SNAPDUMP] trigger, radians of basis jump in one solve", "Camera hook",
            "Default 0.7 rad (40 deg). Compared as sin(this) against "
            "the largest single basis-term change on the upper arm between consecutive solves, so "
            "the per-frame test needs no trig. Lower it if the 1 Hz line's 'worst basis jump seen' "
            "sits below the threshold while Jay is still reporting snaps - that is the case where "
            "silence means the threshold is wrong, not that nothing happened. Only meaningful "
            "when diag_snap_dump=1."),
    S_INT("diag_snapdump_max", DiagSnapDumpMax, 1, 20,
          "Diagnostics: [SNAPDUMP] how many dumps per session", "Camera hook",
          "Each dump is 30 log lines plus a header, so this is a log-volume cap, not a "
          "correctness knob. Default 3. Raise it if the snap is rare enough that 3 "
          "dumps all land in the same few seconds and you need spread across a session. Only "
          "meaningful when diag_snap_dump=1."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_snapdump_per_side", DiagSnapDumpPerSide,
           "Diagnostics: [SNAPDUMP] budget is PER SIDE rather than shared", "Camera hook",
           "U-40(ii). THE SHARED BUDGET IS WHAT STARVED THE RIGHT ARM. INV-234 LIMIT 3: in the "
           "2026-09-24 21:29 run all THREE dumps went to the LEFT - in fact to one 43 ms burst "
           "- while the RIGHT crossed the threshold for the first time in four runs (worst "
           "basis jump 0.8681 against a 0.6442 threshold) and NO DUMP CAUGHT IT. The right "
           "arm's snap is the one mechanism al cannot explain (its al never fell below "
           "0.567355, a band where the left produces jumps of <=0.171), so at least one other "
           "mechanism exists and it has never had a single frame of evidence. With this at 1, "
           "diag_snapdump_max applies to EACH side, so the left's frequent events cannot "
           "consume the right's budget - at 3 that is up to 6 dumps in a session, which is a "
           "log-volume consequence and not a correctness one. DEFAULT OFF, and off reproduces "
           "the historical shared cap EXACTLY, so an earlier run stays quotable. Only "
           "meaningful when diag_snap_dump=1."),
#endif
    S_BOOL("diag_rig_counters", DiagRigCounters,
           "Diagnostics: [SNAPRATE] [CLAVBURST] [REACHCLAMP] [DEGEN] - the open measurements",
           "Camera hook",
           ">>> THE ONE MASTER FOR EVERY RATE THIS PROJECT HAS NEVER HAD. <<< Four sessions of "
           "snap work have produced mechanisms and ZERO rates - INV-234's own LIMIT 2 says "
           "'nothing counts threshold crossings', so how much of what Jay feels any mechanism "
           "accounts for is UNKNOWN. This turns on four log-only counters at once, all "
           "bucketed STILL vs MOVING off a self-contained smoothed eye speed, all printing "
           "HeldItemsBrief() on their report line - so ONE run separates melee-sprint from "
           "empty-standing inside the log. [SNAPRATE] U-40(i): counts every upper-arm basis "
           "jump past diag_snapdump_rad, per side, NOT subject to the dump budget, with its "
           "OWN previous-basis latch - and prints a [SNAPRATE xcheck] line comparing its worst "
           "jump against [SNAPDUMP]'s independently-latched one, which is a real cross-check "
           "and fails loudly. [CLAVBURST] U-39: burst LENGTH of clavicle/upper-arm corruption, "
           "measured against a LATCHED reference that never substitutes and never re-baselines "
           "- which is precisely what [PGUARD] cannot do, because it re-baselines onto the "
           "corrupted pair and so cannot see past frame 3 of its own bursts (INV-231, which "
           "could only prove '>3 frames'). It runs while rig_parent_guard=0, where that "
           "setting sits on disk. [REACHCLAMP] U-41 D-RIG-5: the reach clamp per side and as "
           "an EDGE, which g_ikReachClamped is neither - one shared LONG holding a LEVEL, so "
           "whichever arm solved second destroyed the other's state every frame, which is what "
           "has blocked the sprint-side reach measurement. [DEGEN] U-41 D-RIG-3/4/6: has "
           "BuildBoneLocal's rank-1 fallback ever fired, has ClampWristSwing ever returned "
           "identity or run near pi (the ZERO-EVIDENCE candidate for the right arm's "
           "unexplained snap), is Mat3ToQuat's input ever non-orthonormal. EVERY COUNT ON THAT "
           "LINE IS EXPECTED TO BE ZERO and nothing in the previous build could answer it. "
           ">>> NONE OF THIS IS A FIX. No bone is written, no value substituted, no branch "
           "changed. If the arm behaves differently with this on, that is a bug in the probe "
           "block. <<< COST per solve (~350/s/side, not ApplyDrivenBones' ~12,000/s): one QPC "
           "and one sqrtf for the regime, 9 subtracts/9 compares for [SNAPRATE], 18 compares "
           "and two lengths for [CLAVBURST], two compares for [REACHCLAMP], ~12 flops at the "
           "single [DEGEN] ortho site and nothing at all on the other Degen paths, which sit "
           "inside branches believed never taken. No allocation, no accessor call, no RTTI "
           "walk, no WatchExec slot. HEADSET-ONLY: every counter needs the solve, and the "
           "solve needs controller poses (INV-11). In FLAT every line prints its own NO "
           "SAMPLES / NO SOLVES refusal by name rather than going quiet. DEFAULT OFF."),
    S_FLOAT("diag_regime_move_mps", DiagRegimeMoveMps, 0.2f, 8.0f,
            "Diagnostics: still/moving threshold for the counter buckets, m/s", "Camera hook",
            "Smoothed eye speed above this files a sample under MOVING. 1.5 m/s sits well "
            "above a standing head and well below the 6-7 m/s moving mean measured during a "
            "sprint. THE SPEED IS SMOOTHED, NOT PER-TICK, AND THAT IS NOT A STYLE CHOICE: "
            "element 8's world is a derived cache that refreshes on only some ticks (INV-48), "
            "so a per-tick speed reads ~0 mid-sprint and files sprint samples under STILL - a "
            "defect already measured and fixed once in [ANCHORAB] v3 (boneblend.cpp:680-706), "
            "and this copies that reasoning rather than repeating the mistake. The [REGIME] "
            "line prints the live EMA and the peak beside the threshold, so if the split looks "
            "wrong it can be retuned from the log without a rebuild. Only meaningful when "
            "diag_rig_counters=1."),
    S_FLOAT("rig_clavburst_basis", RigClavBurstBasis, 0.02f, 1.0f,
            "Diagnostics: [CLAVBURST] reject limit, largest basis-term change", "Camera hook",
            "U-39. Largest single basis-term change on the clavicle or upper arm against the "
            "LATCHED last-good pair. DELIBERATELY A SEPARATE KEY FROM rig_parent_guard_basis "
            "even though it defaults to the same 0.25: U-39 has to be measurable while "
            "rig_parent_guard=0, which is where that setting sits on disk, and sharing a key "
            "would tie an instrument's threshold to a disabled fix's threshold. IF THE "
            "[CLAVBURST] LINE REPORTS A NON-ZERO 'runaway' COUNT, THIS VALUE IS TOO TIGHT and "
            "the burst lengths on that line are not burst lengths - the latched reference is "
            "going stale against ordinary motion. Raise it and re-run; that is a setting, not "
            "a rebuild. Only meaningful when diag_rig_counters=1."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_hier_sync", DiagHierSync,
           "Diagnostics: [HIERSYNC] - where does LOCAL -> WORLD actually happen?", "Camera hook",
           ">>> FLAT-SAFE. RUN THIS IN FLATSCREEN; IT NEEDS NO HEADSET AND NO SOLVE. <<< INV-236 "
           "measured that a clobbered LOCAL reaches the DRAWN world matrix at full magnitude, so "
           "the consumer reads at or inside d7380's window. A static read then closed off the two "
           "obvious write points: d7380 copies the matrix array OUT at :165 and BACK at :725, so "
           "anything written to the array in between is DISCARDED by the copy-back - 'write "
           "inside' is futile; and d7380's ONLY caller (FUN_1800e6890) does memcpy -> d7380 -> "
           "TlsGetValue/TlsSetValue -> return, composing nothing. The Mac 2022 symbol dump names "
           "the real mechanism and it is a FORK-JOIN - "
           "CHierarchyElementSynchronizer::CThreadGroupData::StartMatrixSynchronizationJobs / "
           "HelpMatrixSynchronizationJobs / FinishMatrixSynchronization / MatrixSyncTask - and the "
           "CLASS is in OUR OWN engine_x64_rwdi.dll as the RTTI descriptor "
           "`.?AVCHierarchyElementSynchronizer@@` at file offset 0xa25d74, immediately after "
           "`.?AVIThreadGroupUser@@`. ⚠ THE METHOD NAMES ARE MAC-ONLY AND THEIR ADDRESSES DO NOT "
           "TRANSFER; only the shape does. This probe resolves that class's vtable in OUR process "
           "and dumps its slots with module+RVA, which is what turns 'writing at d7380 EXIT is a "
           "RACE against worker jobs' from an INFERENCE into a measurement. It is the difference "
           "between two completely different fixes. >>> COST, AND THIS RULE HAS ALREADY COST TWO "
           "RUNS: RttiVtableOf is an UNCACHED WALK OF BOTH DLLs (aimhook.cpp:334; camhook.cpp:2020 "
           "records losing the 17:04 run to reintroducing exactly this). It is resolved ONCE into "
           "a static, retried at most once per second, and CAPPED at 60 attempts - so the worst "
           "case is one walk per second for a minute, never one per frame. It also does NOT latch "
           "dead the way [TGROUP] v1 did: every failed attempt PRINTS which class failed and why, "
           "because RTTI only appears once an instance has been constructed. <<< Takes no "
           "WatchExec slot, installs no hook, writes nothing. DEFAULT OFF."),
#endif
    S_FLOAT("rig_clavburst_offset_m", RigClavBurstOffsetM, 0.001f, 0.5f,
            "Diagnostics: [CLAVBURST] reject limit, clavicle->upper OFFSET change in m",
            "Camera hook",
            "U-39, the second half of the reject test. THE OFFSET, NOT A WORLD POSITION, and "
            "that distinction already cost one measurement: [PGUARD] v1 compared absolute "
            "world positions and rejected 10,935 of 27,279 frames (40%) because the shoulder "
            "travels through the world whenever the player does. The clavicle->upper-arm "
            "offset is a BONE LENGTH (~0.0853 m in the element table), constant under all "
            "locomotion, so body motion cancels exactly and a one-frame change in it means the "
            "chain itself was replaced. INV-208's lesson. Same default as "
            "rig_parent_guard_trans_m and separate from it for the same reason as the basis "
            "key above. Only meaningful when diag_rig_counters=1."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_frozen_local", DiagFrozenLocal,
           "Diagnostics: what freezes when rig_anim_gate is on", "Camera hook",
           "THE ONE READ BETWEEN HERE AND FINISHING THE TAKEOVER. INV-36: the gate "
           "stops FUN_1800D7380 writing our elements COMPLETELY - divergence to exactly "
           "0.00000. D-32: and the arm is destroyed, forearms smeared, while hands, "
           "fingers and the weapon attachment all look CORRECT. CANDIDATE: "
           "WriteAbsolute3x3 skips indices 3/7/11 so the rig has NEVER written "
           "translations - correct while d7380 supplied them from the animated TRS "
           "channel every frame, and gated nobody does. BUT ONE MEASURED FACT ARGUES "
           "AGAINST THE SIMPLE VERSION, which is why this READS rather than fixes: if "
           "the segment translations were badly wrong the hand could not land on "
           "target, and it did - [DRAWN] mean 0.002 worst 0.020 straight through the "
           "deformation. So it reads BOTH halves of the local matrix: |t|, the segment "
           "LENGTH, and the three BASIS COLUMN LENGTHS, the per-axis SCALE. d7380 bakes "
           "a non-uniform scale into the 3x3 on every write and we write a PURE "
           "rotation, so gated, nothing re-applies it - and a per-axis scale on a bone "
           "is exactly what smears a mesh while leaving its endpoint in place. NOTHING "
           "IN THIS PROJECT HAS EVER MEASURED ELEMENT 48/49/53'S OWN BASIS COLUMNS: "
           "[CHAIN] measured the CLAVICLE's and read 1.0000, and the architecture file "
           "has flagged element 48's own basis as never measured since 2026-09-10. "
           "Every line carries gate=ON/OFF so a sample labels its own regime (INV-7), "
           "and rig_anim_gate is a live overlay toggle, so ONE run gives both states "
           "under identical conditions - within-run, not across two walks of different "
           "scenery (INV-8). Pure read, one guarded accessor call per element per "
           "second. Elements 48/49/53/71 and 12/13/17/35."),
#endif
    S_BOOL("rig_anim_gate", RigAnimGate,
           "Rig: TAKE THE ARM CHAIN OFF THE ANIMATION (writes engine memory)", "Camera hook",
           "THE TAKEOVER. Zeroes the u16 at map+entry*0x10 for 48/49/53/71 and "
           "12/13/17/35. That u16 is a per-element ANIMATION LAYER MASK (INV-32, read "
           "from the inner loop at blend_callers.txt:252/279 walking its bits against "
           "the global active mask at E+0x1900) and BOTH consumers test it != 0, so "
           "zero removes the element from every layer. NOT Block(i), whose bit also "
           "clears the mesh render bit and therefore cannot drive a visible bone "
           "(D-26). THE REASON CHANGED AFTER MEASUREMENT: this was designed believing "
           "FUN_1800DD7B0 dragged our writes; INV-35 measured dd7b0 ENTRY == dd7b0 EXIT "
           "on all 450 [DRAG] lines, zero mismatches, so it does NOT. The writer is "
           "FUN_1800D7380 and the shape is INTERMITTENT TOTAL REPLACEMENT - 94% ours at "
           "d7380 EXIT, so ~6% of samples carry the whole divergence with the bone "
           "fully at the animation's pose (peak 1.56, 115.8 deg, item-gated). Gating "
           "removes these elements from that decompression loop. ELEMENT SET SETTLED BY "
           "MEASUREMENT: [DRAG] emitted for 48/49/53 and 12/13/17; the clavicles "
           "emitted nothing, so they are not written here and gating them would freeze "
           "a bone nobody drives. Plus 71/35, the handholders the weapon hangs from "
           "(INV-34). ALL-OR-NOTHING: refuses to arm unless EVERY element resolves, "
           "because a partial gate is D-11's chimeric chain, which produced [AIM R] "
           "53.9 deg against 1.5. REFUSES until the rest latch is in on BOTH sides - "
           "gating freezes the TRANSLATIONS (we write rotations only), so arming "
           "mid-swing bakes that length in for the session; that is the race that "
           "voided the 2026-09-10 13:36:40 run. Re-asserts at 1 Hz and says how many "
           "the engine rewrote. Restores the EXACT original mask, never 1 - the two "
           "sides carry DIFFERENT masks (right 5 = layers {0,2}, left 11 = layers "
           "{0,1,3}). If the map pointer moves the originals are NOT written back, "
           "because the old allocation may be freed. DEFAULT 0. This is the one setting "
           "in the rig family that writes engine memory."),
    S_BOOL("melee_hook_inline", MeleeHookInline,
           "Melee poke via inline hook, not a debug register", "Camera hook",
           "WHICH MECHANISM CARRIES THE ELEMENT POKE, and it changes nothing about "
           "WHAT is written - the same body runs either way (WatchExecPokeApplyAt), "
           "deliberately, so this is one variable. 0 = the DR1 execution breakpoint "
           "at gamedll+0xC4D920, as it has always been. 1 = an inline hook at the "
           "same address, no debug register involved. >>> IT EXISTS TO SETTLE U-27. "
           "INV-120: six of six IN-PLAY crashes on engine+0x8CD6F8 had the trap "
           "armed, THREE of them with the write OFF, and no session with "
           "diag_melee_elem_probe=0 has ever produced that face - but there is NO "
           "MECHANISM, so the way to decide it is to remove the debug register and "
           "see whether the face goes away. If it does, the trap is the trigger and "
           "the write half becomes shippable; if it does not, U-27 is wrong. <<< In "
           "inline mode WatchExec's `fires` stays 0 and [MELEEHOOK] calls= is the "
           "denominator instead."),
    S_ENUM("melee_force_part", MeleeForcePart, "Force melee hits to a body part",
           "Camera hook",
           "THE PER-MODEL VERSION OF melee_force_elem, AND IT SUPERSEDES IT. A constant "
           "index cannot mean a body part: INV-72 measured the SAME index as `r_hand` on "
           "one zombie model and a jacket sleeve with NO BONE on another, which is why "
           "forcing a constant affected only some zombies. This names the PART, resolves "
           "its element BY NAME on whatever model is actually being hit "
           "(GetElementID, on the [ELEMDUMP] thread - never inside the exception "
           "handler), and the trap writes that model's own index. REQUIRES "
           "diag_elem_dump=1, which is what discovers the models. A hit on a model with "
           "no mapping yet writes NOTHING and counts a MISS - deliberately, because a "
           "default would be indistinguishable from a redirect. READ THE MISS COUNT "
           "BEFORE BELIEVING ANY RESULT. When this is anything but `off` it takes "
           "precedence over melee_force_elem.", kMeleePart),
    S_INT("bone_dump_attempts", BoneDumpAttempts, 1, 600,
          "Bone probe: how many times it hunts for a held item before giving up "
          "(default 20, ~5.6 s each)", "Rig",
          ">>> THE WINDOW IS MEASURED FROM PROCESS START, NOT FROM GAMEPLAY, AND "
          "THAT IS WHAT MAKES IT A TRAP. <<< The probe arms an exec watch on "
          "CorrectItemPosition, which only runs for a HELD ITEM. At the default 20 "
          "it gives up ~114 s after launch. Measured 2026-09-22: a run that reached "
          "gameplay in ~54 s captured on attempt 10 and the rig latched both sides "
          "at 0.17177; a run that did not logged 'gave up after 20 attempts' at "
          "21:25:39, 114 s after its 21:23:45 launch, and EVERYTHING downstream was "
          "dead - no model, no [SHOULDER] latch, no rig - while every arming message "
          "still printed. If your load takes 90 s you have 24 s of margin. "
          ">>> RAISING IT IS FREE ON A RUN THAT CAPTURES <<< - the loop exits on "
          "success, so the extra attempts are only ever spent on a run that was "
          "failing anyway. It is NOT free on one that does not: each attempt calls "
          "WatchExecStart, which arms a hardware breakpoint across ~170 threads for "
          "1500 ms. 60 gives a ~5.5 minute window."),
    S_BOOL("rig_exec_client2", RigExecClient2,
           "Bone probe uses exec CLIENT 1 (DR0) instead of client 0 (DR1)", "Rig",
           ">>> THIS IS THE ONE THING BLOCKING VR-DIRECTED MELEE. <<< The rig and the "
           "melee trap both need an execution breakpoint, and before the two-client "
           "refactor there was exactly ONE - so arming both meant the trap logged "
           "'REFUSING TO ARM - the bone probe holds the single process-wide WatchExec "
           "slot' every 5 s and NEITHER produced usable output (measured 2026-09-16). "
           "With this on, the bone probe moves to DR0 and the trap keeps DR1, so both "
           "run. "
           ">>> DR0 IS SHARED WITH THE DATA WATCH. <<< Client 1 and WatchWrites cannot "
           "both hold it; whichever is second REFUSES BY NAME and arms nothing - look "
           "for 'watchexec2: REFUSING' or 'watchwrites: REFUSING'. The data watch is "
           "off in the melee configuration (Dr0=0 in two crash dumps), so this is "
           "free there. "
           "OFF BY DEFAULT so the pre-refactor behaviour is still reachable for a "
           "regression run - turn it on only for the coexistence test."),
    S_BOOL("rig_rest_from_asset", RigRestFromAsset,
           "Rig: latch the hand rest local from the MESH ASSET, not the live bone", "Camera hook",
           "THE INV-150 FIX, AND IT CHANGES WHAT THE DRAWN HAND RESTS AT. g_handRestLocal used to "
           "be latched from g_getLocal - the LIVE element matrix - behind a shape-only guard, so "
           "'authored' was a claim the code did not support (INV-146): it captured whatever pose "
           "the bone happened to be in on the first resolving tick. That is 77.0 deg in flat, where "
           "the bone sits at rest, and it was 107.4 / 92.6 in the headset on 2026-09-19. Three "
           "sessions went looking for what had changed; nothing had. With this ON the value comes "
           "from the AUTHORED store on the shared mesh asset via IMeshEntity vtable +0x58 "
           "(INV-148) - pose-independent, identical every launch, and measured at 77.0 = 13.73 + "
           "-/+75.12 over 22 passes with ZERO DRIFT by [RESTREAD] (INV-150). "
           "IT IS READ ONCE, AT LATCH TIME, so it is a LAUNCH-time A/B and not a live toggle; set "
           "it to 0 to reproduce the old behaviour exactly. "
           "FAILS CLOSED AND SAYS SO: the asset read keeps INV-149's index self-check, and if it "
           "cannot be made the code falls back to the live accessor and NAMES the fallback on its "
           "own line - a fallback that reads like a success is how this survived four sessions. "
           "[BONEREST] now prints SOURCE = ... on every latch. "
           "DOES NOT DEPEND ON diag_rest_read - the fix must not need a diagnostic to be on."),
    S_BOOL("rig_rest_refuse_live", RigRestRefuseLive,
           "Rigging: never take the hand's rest pose from the live bone", "Camera hook",
           "Fixes the twisted left wrist that could stick after a load until you restarted the game. "
           "The hand's natural rest angle is normally read from the character's mesh file. When that "
           "read failed (it happened while a character was being unloaded), the mod used to fall back "
           "to the bone's current pose, which came out wrong every time it was seen (5 of 5 logs) and "
           "stayed wrong for the whole session. With this on, the mod refuses that fallback: the "
           "wrist follows the forearm for a moment and the hand is set up properly as soon as the mesh "
           "read works, normally on the next character after the load. Needs rig_rest_from_asset on. "
           "On by default - leave it on."),
    S_BOOL("rig_shoulder_rest_from_asset", RigShoulderRestFromAsset,
           "Rig: latch [SHOULDER]'s rest offset from the MESH ASSET, not the live bone",
           "Camera hook",
           "THE INV-159 FIX - THE SAME SOURCE SWAP AS rig_rest_from_asset, ONE BONE FURTHER UP. "
           "[SHOULDER] latched g_upperRestLocal from VecLen of element 48/12's LIVE local "
           "translation, guarded by a 6 mm band (0.1690..0.1750) plus a 16-sample stability "
           "window. [RESTREAD] measured both halves of that on 2026-09-21 in ONE flat run, 53 "
           "passes, drift events 0: the AUTHORED |t| is 0.17177 on BOTH sides, one unique value "
           "per element, while the LIVE read the latch samples spans 0.12013..0.23352 on element "
           "48 and 0.06692..0.19387 on element 12. A 6 mm guard band across a 113-127 mm live "
           "range is why the right shoulder latched out of known-good in six of seven runs "
           "(0.1739, 0.1745, 0.1783, 0.2140, 0.1304) - every one of those is a correct reading of "
           "a bone the animation rewrites every frame (INV-4, FUN_1800D7380), taken at a "
           "different moment. There is no tick on which the live local is guaranteed to be the "
           "authored local, so this cannot be fixed by sampling harder; tightening kRestMax only "
           "moves the ceiling (D-29). "
           "WITH THIS ON the value comes from the AUTHORED store on the shared mesh asset via "
           "IMeshEntity vtable +0x58 (INV-148), pose-independent and identical every launch, and "
           "it latches on the FIRST resolving tick with no window to wait for. "
           "IT IS READ ONCE PER SIDE PER PROCESS, so it is a LAUNCH-time A/B and not a live "
           "toggle; set it to 0 to reproduce the band + stability-window behaviour exactly. "
           "FAILS CLOSED AND SAYS SO: AuthoredLocalForElement keeps INV-149's index self-check on "
           "this fix path, returns false rather than a zeroed matrix, and if the read cannot be "
           "made the code falls back to the live path and NAMES the fallback on its own line. An "
           "authored |t| that lands OUTSIDE the band is neither accepted nor swallowed - it is "
           "reported in full as a finding about the BAND and the live path is left to run. "
           "[SHOULDER] latched now prints SOURCE = on every path. "
           "DOES NOT DEPEND ON diag_rest_read - the fix must not need a diagnostic to be on. "
           "Needs rig_stable_shoulder, which is what owns this latch."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_rest_read", DiagRestRead,
           "Diagnostics: read the AUTHORED rest local off the shared mesh ASSET", "Camera hook",
           "U-33 HALF B, AND IT IS ONE NUMBER: is the player's authored rest local for "
           "elements 53/17 the reproduced 77.0 symmetric pair (13.74 tilt + -/+75.12 roll) "
           "or the 2026-09-19 107.4 / 92.6? "
           ">>> THIS IS NOT [BONEREST]. <<< [BONEREST] latches a LIVE accessor read behind "
           "a shape-only guard, so 'authored' in its log line is a claim its code does not "
           "support (INV-146), and it is headset-only because it sits inside SolveArmIk "
           "(INV-145). This reads a DIFFERENT STORE: the 12 floats the engine keeps on the "
           "shared mesh ASSET, reached by model+8 -> +0x398 (CMesh) -> +0x18 "
           "(CMeshElement[], stride 0x78) -> +0x18 (IMeshEntity*) -> vtable +0x58, which "
           "copies them into a caller buffer and writes nothing (INV-147/148/149). "
           "SELF-VALIDATING TWICE: it prints the element count from two independent fields "
           "against INV-144's 92, and a PASS/FAIL per element on INV-149's "
           "CMeshElement[i]+0x20 == descArray + 0x30*i - a FAIL there means the matrix may "
           "belong to a different element and the line is not usable. "
           "IT MEASURES ITS OWN PREMISE: the field's immutability was INFERRED, never "
           "enumerated, so it re-reads every 5 s and reports any change as a DRIFT event "
           "with both values. A drift voids every reading in the run. "
           "FLATSCREEN IS ENOUGH: nothing in the chain touches SolveArmIk, controller poses "
           "or the IK solver, and it runs from the same BoneBlendTick site as "
           "[ELEMDUMP], which produced a full player table in flat on 2026-09-20. "
           "DEPENDS ON diag_bone_dump=1 (which is what CAPTURES the player model) and on "
           "the three master enables (INV-1); it refuses BY NAME if the model is absent."),
#endif
    S_BOOL("item_place_from_controller", ItemPlaceFromController,
           "Rig: place the weapon from the CONTROLLER at the tail, taking the last word after CorrectItemPosition",
           "Camera hook",
           "OPTION A OF docs/PROPOSAL-ITEM-PLACEMENT-2026-09-22.md, AND IT ANSWERS "
           "INV-47 BY NOT ARGUING WITH IT. INV-47 measured the item's \"Holder\" at "
           "0.0000 m from the body handholder on all three classes and concluded "
           "\"the weapon is downstream of the arm and has NO independent fix\" - "
           "true OF THE EXISTING DERIVATION, where the weapon rides element 71, 71 "
           "rides the hand, and the hand is wrong (the error is UPSTREAM of 53). "
           ">>> THIS DOES NOT USE THAT CHAIN. It writes the item's own \"Holder\" "
           "element from the CONTROLLER pose - a SUBSTITUTION of the placement "
           "source, not a correction of it, which is exactly the distinction D-35 "
           "could not make when it rejected an intercept \"derived from the same "
           "wrong chain\". It creates the independent path INV-47 correctly says "
           "does not otherwise exist. <<< WHERE: the "
           "player-update tail (gamedll+0xBFC170) AFTER g_trueTail, therefore after "
           "UpdateBodyModyfication and after CorrectItemPosition has placed the item "
           "this frame - established by reading correct_item_position_callers_"
           "decompiled.txt. We get the LAST WORD. "
           "IT REPRODUCES THE ENGINE'S SCALE STEP (FUN_1801d0070 column lengths, "
           "then FUN_18131e740 normalise-and-rescale) so the item renders at its "
           "AUTHORED scale, not ours. IT USES THE API, never a raw array write, "
           "because INV-178 makes ITEM models DOUBLE-BUFFERED and a hardcoded buffer "
           "writes the wrong store - the defect behind the retracted INV-177. The "
           "per-hand rig_hh_rot_* and rig_hh_off_* dials are applied here too, so a "
           "value dialled for the handholder keeps its meaning. Finiteness gate over "
           "all twelve floats before any write. Reports [ITEMCTL] every second "
           "including every refusal reason by name. HEADSET - it needs a controller "
           "grip pose and refuses with nosnap in flat."),
    S_INT("item_place_basis", ItemPlaceBasis, 0, 2,
          "Rig: which basis item_place_from_controller writes",
          "Camera hook",
          ">>> 1 IS THE ONE THAT NEEDS NO DIALLING. <<< Each mode selects a BASIS "
          "AND ITS MATCHING TRANSLATION, and mismatching them is what forces a "
          "human to dial out a residual offset: 1 uses achieved[] = achievedWrist "
          "+ drawnBasis * g_handHolderLocal, i.e. where the handholder BELONGS on "
          "the hand bone we actually drew, using the offset latched from the MESH "
          "ASSET (INV-161) - that pair reproduces the game's own authored grip on "
          "our hand, which is \"what it would be in flatscreen\", automatically. "
          "0 and 2 use pos[] = where the CONTROLLER asked the grip to be, and that "
          "differs from achieved[] by the reach shortfall (median 31/41 mm, max "
          "442/383 mm, [SOLVE PEAK] 2026-09-21). "
          "0 = grip.hand[] - the COMMANDED hand basis, controller orientation times "
          "the grip calibration. 1 = grip.drawn[] - where the hand BONE actually "
          "ended up, i.e. it follows the arm. 2 = grip.cWorld[] - the raw controller "
          "orientation with no calibration. >>> THE TRADE IS ALREADY MEASURED, in "
          "MeleeGripSnapshot's own comment: writing hand[] gave \"42,088 basis "
          "writes, zero refusals, FLAILING GONE, and every weapon STATICALLY "
          "ROTATED\" on 2026-09-21. A STATIC rotation is what the per-hand "
          "rig_hh_rot_* dials already correct; flailing is not. <<< So 0 is the "
          "default and the intended answer, 1 exists because it is the honest "
          "control - if 1 looks identical to today then the weapon was following "
          "the bone all along and this whole option is inert, which is worth being "
          "able to show. 2 is the fallback if the grip calibration is the thing "
          "introducing the static rotation."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_right_thumb_capture", DiagRightThumbCapture,
           "Diagnostics: log the right thumb's pose while holding a two-handed melee weapon", "Camera hook",
           "LOG-ONLY (INV-516): once a second while the right hand holds an axe / sledgehammer / pickaxe, logs the right thumb and "
           "index bones as the game draws them, so the left hand's thumb can copy it. Off by default."),
    S_BOOL("diag_bow_string_probe", DiagBowStringProbe,
           "Diagnostics: bow string write test (4 blocks of 3 arrows)", "Camera hook",
           "WRITE TEST, NOT LOG-ONLY (INV-541): with a bow in the left hand, fire exactly 12 arrows. The left controller buzzes N "
           "times when block N starts. Block 1 only logs the game's own draw. Block 2 holds the string and the nocked arrow at a "
           "fixed short pull. Block 3 keeps the bow's limbs straight. Block 4 forces a weak draw power. One long buzz = finished. "
           "Off by default; leave it off outside that run."),
    S_BOOL("diag_item_place", DiagItemPlace,
           "Diagnostics: does the weapon's own Holder element track element 71 within the frame",
           "Camera hook",
           "A.3 OF docs/PROPOSAL-ITEM-PLACEMENT-2026-09-22.md, AND IT DECIDES WHETHER "
           "OPTION A IS BUILDABLE. CorrectItemPosition's formula, read in full: "
           "item.Holder.world.translation = element71.world.translation VERBATIM "
           "(FUN_18131e740 touches indices 0,1,2,4,5,6,8,9,10 only - 3/7/11 are "
           "never modified), and the basis is normalise(element71.basis) x the "
           "item's own scale. So at the player-update TAIL - after "
           "UpdateBodyModyfication and therefore after CorrectItemPosition has run "
           "- the two translations should be EQUAL. >>> ANY DIFFERENCE IS THE "
           "WEAPON HAVING BEEN PLACED FROM A DIFFERENT element71 THAN IS NOW IN "
           "MEMORY, AND ITS MAGNITUDE IS THE ORDERING ERROR IN METRES. <<< "
           "~0 means ordering is NOT the defect and Option A's premise is wrong - "
           "which is far cheaper to learn from a probe than from a fix. Large "
           "means Option A is aimed at a real, measured quantity. "
           "IT ALSO SETTLES TWO OF THE PROPOSAL'S THREE UNKNOWNS FREE: whether "
           "GetElementID(item,\"Holder\") resolves and to what id, and on which "
           "item CLASSES - the RTTI name is logged, which is U-17's firearm "
           "question. IT WRITES NOTHING. It calls the world accessor on the item, "
           "which IS dl_WorldResolver and therefore a write (INV-169) - deliberate "
           "and bounded, because CorrectItemPosition calls it on that exact item "
           "and element every frame (its lines 73-74), so this adds no new CLASS "
           "of side effect. The API is used rather than a raw walk because INV-178 "
           "establishes ITEM models are DOUBLE-BUFFERED and a hardcoded buffer "
           "reads the wrong store - the defect behind the retracted INV-177. "
           "COST: the tail hook is ONCE PER FRAME, not the blend's 19,461/s; "
           "RttiClassNameOf is an uncached vtable walk so it runs only when the "
           "item POINTER changes, and GetElementID is cached on the same key. "
           "FLATSCREEN IS ENOUGH - CorrectItemPosition ran 14,179 times in flat on "
           "2026-09-22. Needs rig_write_at_player_tail's hook site (installed "
           "unconditionally) and diag_bone_dump=1 plus the three master enables "
           "for the player model; it prints NO SAMPLES with a reason otherwise."),
#endif
    S_BOOL("rig_hh_flag_handshake", RigHhFlagHandshake,
           "Rig: after writing the handholder's LOCAL, apply the engine's own stale-marking idiom so it takes effect",
           "Camera hook",
           "THE STEP P4 OMITTED, AND IT IS READ OUT OF OUR OWN BINARY RATHER THAN "
           "INVENTED. HHDRIVE writes element 71/35's LOCAL - verified landing, "
           "600,000+ writes, 0 failures, correct array (g_playerMatrices == "
           "*(container+0x20), confirmed three runs) and correct index (slot=71 "
           "for element 71) - and then does NOTHING to mark the derived caches "
           "stale. The engine's own post-local-write idiom is byte-identical to "
           "what this applies: dirty_writers_decompiled.txt:173 has "
           "'*(byte *)(slot + *(container+0x40)) = ... & 0xfe | 0xe', and "
           "ENGINE_ARCHITECTURE.md:4271 says the same of ?SetMeshElementsMatrices@. "
           ">>> WITHOUT BIT 1 NOTHING RECOMPOSES THE WORLD - FUN_1804862f0 returns "
           "the CACHED *(container+0x30) when (flags & 2) == 0, never reading our "
           "local. WITHOUT BIT 3 THE ctx==2 SNAPSHOT NEVER REFRESHES - FUN_180486d20 "
           "copies +0x30 into +0x18 only under (flags & 8), and the resolver hands "
           "+0x18 straight back to ctx==2 threads with the flags never read. So a "
           "local write with no handshake reaches NEITHER consumer, which is D-85's "
           "missing mechanism: 1.6 M writes a side, 0 failures, weapon never moved. <<< "
           "When the bits were set on 2026-09-22 the weapon DID visibly move - the "
           "channel works; that run's payload was wrong because item_pose_basis=1 "
           "was also on. IT REFUSES BY NAME while item_pose_basis=1: that path "
           "writes the CACHED WORLD and bit 1 makes the resolver recompose from the "
           "local and discard it, so the two are mutually exclusive (INV-176) and "
           "running both is what made D-90 unreadable. COST: none - g_armFlagPtr[] "
           "is resolved at 1 Hz in DirtyFlagProbe, ungated, precisely so this never "
           "walks pointers at ~19,461 calls a second. DEFAULT OFF so it is an "
           "explicit A/B. Reports [HHSHAKE] every second including when refused or "
           "when no pointer resolved."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_hh_compose", DiagHhCompose,
           "Diagnostics: is element 71/35's world composed from OUR local, and does the bit-1 handshake fix it",
           "Camera hook",
           "THE A/B THAT CARRIES ITS OWN CANDIDATE FIX. Two runs established a "
           "contradiction: [HHCLOB] 16:07 proved element 71's LOCAL is ours and "
           "constant (809,752 intact, and the 1.3%% mismatch is a documented "
           "read/write race with 0.00 deg magnitude on BOTH sides), while "
           "[RELWATCH] 15:58 measured its WORLD rotating 4.42x its parent's "
           "(peak 7.70x) with the LEFT at exactly 1.00x. Both cannot hold if the "
           "engine composes world_71 = world_53 x local_71 - so it does not, and "
           "INV-176 names the mechanism: dl_WorldResolver recomposes from the "
           "local ONLY when bit 1 is set and otherwise returns the CACHED world. "
           ">>> EVERY READ IS RAW AND THAT IS THE WHOLE DESIGN: "
           "GetElementWorldMatrix IS dl_WorldResolver (INV-176/INV-169), so "
           "reading the world through it would CAUSE the compose this probe "
           "detects and report agreement every time - the exact defect that "
           "produced the retracted INV-177. It walks ResolveSlotFlagOn to "
           "worldBase/localBase/flagPtr and loads at slot*0x30. No accessor is "
           "called. <<< IT IS READ-ONLY. It briefly also WROTE a "
           "(flags & 0xfe) | 0x0e handshake to 71/35 as a live A/B; that write "
           "is DELETED - D-90 - because it never read back (bit1 set 0/480 "
           "against 376,320 writes) and it still threw the weapon off screen. Do "
           "not reinstate it without first establishing why the byte we write is "
           "not the byte we read. SELF-CONSISTENT: "
           "world_parent, world_self and local_self are read at ONE instant, so "
           "the ratio cannot be an artefact of comparing two probes' clocks. "
           "FLATSCREEN IS ENOUGH - the drive runs in flat ([HHDRIVE] written "
           "200000, 2026-09-22) and nothing in the chain needs a controller pose "
           "or the VR camera path. DEPENDS ON rig_drive_handholders=1, "
           "diag_bone_dump=1 and the three master enables (INV-1); it refuses BY "
           "NAME when the model is absent."),
#endif
    S_BOOL("item_place_orphans", ItemPlaceOrphans,
           "Item: carry the weapon's PARENTLESS elements with the holder write",
           "Camera hook",
           "PHASE 2 OF docs/PROPOSAL-PARENTLESS-ELEMENTS-2026-09-23.md. FIXES "
           "THE VOLKATRONIX'S DETACHED SIGHT AND, EXPECTED, AESIR'S BLADE. "
           "THE DEFECT (INV-190): our holder write moves `holder` AND ONLY WHAT "
           "IS PARENTED UNDER IT, so an element with parent == -1 is left exactly "
           "where the engine put it. INV-192 proved OUR WRITE causes the sight's "
           "detachment - with item_place_from_controller=0 the sight is ATTACHED. "
           "THE FIX: each parentless element's pose relative to HOLDER is captured "
           "at the same instant the hand->holder rel is, then replayed onto our "
           "holder: basis = normalise(w.basis) * orphR rescaled by THE ELEMENT'S "
           "OWN column lengths, pos = w.pos + normalise(w.basis) * orphT. "
           ">>> THE PREMISE IS MEASURED, NOT ARGUED - INV-193, PHASE 1: that "
           "relationship is EXACTLY constant at the capture instant, 0.0000 m / "
           "0.00 deg across every window and every element, and the CONTROL fired "
           "- the ordinary melee weapon that already works reports ZERO "
           "parentless elements, exactly as INV-190 predicts. <<< "
           "SCALE IS CAPTURED AND REAPPLIED EXPLICITLY: the rel is computed from "
           "NORMALISED bases, because only a normalised basis has transpose == "
           "inverse, so the element's own column lengths would otherwise be "
           "silently dropped - INV-13's family of defect. "
           "ALL TWELVE FLOATS ARE CHECKED BEFORE EACH WRITE, the same rule the "
           "holder write follows: a NaN here kills the weapon for the session. "
           "COST: the parentless list is walked ONCE PER ITEM, on swap, never per "
           "frame; then one accessor read and one compose per element per frame, "
           "typically 1-2, plus that many SetElementWorldMatrix calls. "
           "COUNTERS: [ITEMCTL] prints `PARENTLESS carried= refused=` per hand "
           "plus captured=/noWorld=. `carried=0` on a weapon WITH parentless "
           "elements is a failed chain; `carried=0` on one WITHOUT them is normal "
           "and expected. "
           "DEFAULT OFF so the A/B is one setting inside one run. REQUIRES "
           "item_place_rel=1 (it extends that write) and therefore also "
           "item_pose_fix=0 and item_pose_basis=0. "
           ">>> IT MAY STILL FAIL, AND THE FALSIFIER IS PRE-REGISTERED: a working "
           "weapon regressing means the capture instant was not coherent after "
           "all; the writes landing while the draw ignores them would mean "
           "parentless element worlds do not reach the draw, which is D-86's "
           "outcome for the control object and would close the last "
           "representation layer. <<<"),
    S_BOOL("item_place_holder0", ItemPlaceHolder0,
           "Item: place weapons whose grip element is holder_0 (base pistols, Trick Pick)",
           "Camera hook",
           "INV-492. Seven weapon meshes name their grip element holder_0 instead of holder: wn_pistol_a / "
           "wn_pistol_b (+ their _b variants) - the mesh of 23 Beretta-animation pistol items, every PistolA/B "
           "variant, the Rais gun, M9, Taser - and wn_pick_f (LiveOps Trick Pick). The engine's CorrectItemPosition "
           "and every one of our placement lookups ask for \"Holder\" by name, so NOBODY places these weapons: they "
           "ride the animated handholder and sit off in the palm (INV-363: 'hand empty' on 450 of 450 samples). "
           "ON: when an item has no \"Holder\", the placement and the rel capture use \"holder_0\". The per-frame "
           "hand-to-holder rel is read from the BODY's handholder (71/35) - exactly what the engine copies onto holder "
           "for every other weapon - never from holder_0, which the engine never resets (reading it would read our "
           "own previous write, D-93). A weapon WITH holder never takes this path. COST: one name lookup per item "
           "change and one element-name read per update, for holder-less items only. "
           "LOG: [ITEMCTL] 'placing via holder_0 = element N' once per item; with this OFF, a holder_0 weapon says "
           "'NOT PLACED because item_place_holder0=0'. REQUIRES item_place_rel=1, and item_place_orphans=1 for the "
           "pistols' body mesh (a root-level element). Does NOT help the guitar class (no grip element at all, "
           "INV-195). DEFAULT ON (Tier A, 2026-10-08): verified on wn_pistol_b_hq with an off-check - "
           "off put the pistol back off the hand. Takes effect on the next weapon draw."),
    S_BOOL("item_place_virtual_holder", ItemPlaceVirtualHolder,
           "Item: place weapons with NO grip element (guitar, candy cane, ...)",
           "Camera hook",
           "INV-492 / INV-493. Some weapon meshes have no grip element at all - neither holder nor holder_0: the "
           "electric guitar, the candy-cane crowbar, short_knife_f (GodFather knife), polearm_b_dm_b, machinegun_a. "
           "Nobody places them, so they ride the ANIMATED handholder instead of your hand. INV-493 read where they "
           "ride: the item's root sits on the body's handholder for its slot (71 right / 35 left) at identity, and "
           "every element hangs off that root rigidly. ON: that handholder is used as the item's holder - the grip "
           "pose is computed exactly as for any weapon (rel from the handholder, as item_place_holder0 does), and "
           "every root-level element is written from it, each latched once per item before our first write "
           "(item_place_orphans' method). REFUSES, named in the log: a throwable-named mesh (grenade, throw, "
           "shuriken, molotov, bomb - throwables have a separate plan), a grip side that differs from the slot, "
           "item_place_orphans=0, item_place_rel=0. An item WITH holder or holder_0 never takes this path. "
           "COST: one guarded mesh-name read per update for grip-less items only; up to 8 element writes. "
           "LOG: [ITEMCTL] 'PLACING via the VIRTUAL HOLDER' or 'virtual holder REFUSED: <why>' once per item; "
           "[VHOLDER] 'Root-level elements carried: N of M' (M > N = the rest are NOT placed, cap 8). "
           "DEFAULT ON (Tier A, 2026-10-08): verified on the electric guitar. Known: slight misplacement during "
           "jumps / climbs (deferred). Takes effect on the next weapon draw."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_desc_parent", DiagDescParent,
           "Diagnostics: each element's REAL transform parent (desc->parent)",
           "Camera hook",
           "ANSWERS WHAT POSITIONS A HOLDER-LESS ITEM LIKE THE GUITAR. "
           "INV-197: GetElementParent() returns -1 when desc->parent is NULL "
           "**OR** when the parent's slotAndDeform SIGN BIT IS CLEAR - it is a "
           "REPORTING FILTER, not a structural fact. Every element this session "
           "called 'parentless' was read through it. And INV-196: "
           "AttachChildToMeshElement hands the child container->descArray[a3] - a "
           "pointer to the PLAYER'S element record - as a transform parent, so an "
           "item element can carry a CROSS-MODEL parent the accessor hides. "
           "THE GUITAR (INV-195) has no `holder`, so neither the engine nor we "
           "place it, yet it is CORRECT IN FLAT and floats in VR while we never "
           "touch it. The attachment is the only link between a holder-less item "
           "and the hand, and this probe reads it directly. "
           "PER ELEMENT IT PRINTS: desc->parent, the parent's container, whether "
           "that container is THIS ITEM / >>> INSIDE THE PLAYER <<< / a third "
           "model / NULL, the parent's raw slotAndDeform with its sign bit "
           "spelled out, and what GetElementParent() reports - so the filter and "
           "the truth sit on the same line. "
           "THE WALK IS READ, NOT GUESSED (re_analysis/gu_elemdesc_layout.txt), "
           "with the layout confirmed TWICE - IDA's struct definition AND the "
           "instruction encodings of dl_LocalResolver: model+8 -> "
           "dl_PickThreadBuffer(inner+904) -> *buf+0x20 = container -> +0x38 = "
           "descArray -> +i*0x30 = desc -> +0x00 = parent; parent+0x20 = its "
           "container, parent+0x28 = slotAndDeform. "
           ">>> dl_PickThreadBuffer IS THREAD-DEPENDENT and is CALLED every time, "
           "never cached. It is an UNEXPORTED internal reached by engine RVA "
           "0x9A520, so the call is SEH-GUARDED and a failed walk REFUSES BY NAME "
           "- a null must never read as 'this element has no parent'. <<< "
           "READ-ONLY, writes nothing, runs ONCE PER ITEM alongside the element "
           "dump, so it costs one walk per weapon and nothing per frame. "
           "WORKS IN FLAT - pointer identity and structure are flat-safe (INV-11). "
           "DEPENDS ON diag_item_elem_dump=1 (it rides that dump), diag_bone_dump=1 "
           "and the three master enables (INV-1), and an item being HELD."),
#endif
    S_BOOL("diag_compose_chain", DiagComposeChain,
           "Diagnostics: is the engine's world for a bone really parentWorld * ownLocal?",
           "Camera hook",
           ">>> TESTS THE ASSUMPTION UNDER EVERY ARM CONCLUSION WE HOLD, AND IT "
           "HAS NEVER BEEN CHECKED. <<< The rig writes 3x3 ROTATIONS into bone "
           "LOCALS and NEVER a translation (INV-202; D-32 said it first - "
           "'WriteAbsolute3x3 skips indices 3/7/11'). The hand's POSITION is "
           "therefore whatever the engine's forward kinematics produces, so every "
           "statement of the form 'our rotations survived, the arm should be "
           "right' silently assumes the chain composes the way we think. "
           "WHY NOW: U-37. The drawn hand is 0.18-0.42 m from the rig's own target "
           "while every solver input is healthy - anchor flat at 0.302, the guard "
           "never firing (subs pinned at 140), reach shortfall only 26-76 mm "
           "against a 0.546 m cap. If compose(parent, local) != world for the "
           "ENGINE'S OWN DATA then our model of the arm is wrong and that is where "
           "the error is; if it holds exactly, the chain is sound and the error is "
           "in the TARGET we aimed at. "
           ">>> IT NEVER CALLS THE WORLD ACCESSOR, AND THAT IS THE WHOLE DESIGN. "
           "GetElementWorldMatrix IS dl_WorldResolver: with bit 1 set it REBUILDS "
           "the world FROM THE LOCAL and clears the bit (INV-167/169/176). A probe "
           "reading the world through the accessor would compare a freshly-composed "
           "value against its own composition and AGREE BY CONSTRUCTION - a "
           "guaranteed pass measuring nothing, the same defect class as D-93 "
           "(comparing ourselves against our own write) and D-96 (two sides sharing "
           "a common factor). Every read here is RAW via ResolveSlotFlagOn. <<< "
           "THE FLAG COLUMN IS NOT DECORATION: bit1 CLEAR means the cached world is "
           "the engine's considered value and a mismatch is REAL; bit1 SET means "
           "the engine itself calls that world stale and a mismatch says NOTHING. "
           "Clean and stale are counted SEPARATELY and never merged. The ALT array "
           "is printed too, because INV-169 measured alt and world over 100 mm "
           "apart on 48% of samples and never established which one is drawn. "
           "The parent comes from desc->parent, NOT GetElementParent, which hides "
           "a parent whose slotAndDeform sign bit is clear (INV-197/198). "
           "COVERS clavicle/upperarm/forearm/foretwist/hand on both arms "
           "(45,48,49,50,53 and 9,12,13,14,17). Every failure REFUSES BY NAME and "
           "is counted as refused, never as a pass. "
           "COST: a few pointer reads and one 3x4 multiply per element, once per "
           "second. No accessor, no resolver, no RttiVtableOf, no WatchExec slot. "
           "WRITES NOTHING. "
           ">>> FLAT-CAPABLE AND FLAT IS THE POINT: the rig solves nothing in flat, "
           "so every number is the ENGINE'S OWN and the test is of the engine's "
           "chain rather than of ours. DEPENDS ON diag_bone_dump=1 and the three "
           "master enables (INV-1) for the captured player model. <<<"),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_thread_group", DiagThreadGroup,
           "Diagnostics: which THREAD GROUP is each of our sites on?",
           "Camera hook",
           ">>> U-19's LIVE HALF, AND THE ONLY HALF NOT ALREADY ANSWERED. <<< "
           "dl_WorldResolver returns container+0x18 - the PREVIOUS FRAME's world "
           "matrices, named 2026-09-24 by "
           "CHierarchyElementContainer::EnableWorldMatrixPrevFrame(bool) and "
           "CHierarchyElement::GetWorldMatrixPrevFrame() in the 190k symbol dump "
           "- to ANY caller whose g_ThreadGroupIndex is 2, with NO dirty-flag "
           "check and NO null check (world_rebuild_decompiled.txt:64-66). The "
           "selector is the THREAD, not the call site, so 'which consumer reads "
           "+0x18' is unanswerable from the code: all 434 resolver call sites "
           "across 245 functions read it on such a thread. "
           ">>> WHAT IS ALREADY ANSWERED: INV-169 ([ITEMPARITY], flat "
           "2026-09-21) measured the accessor returning +0x30 on 100% of calls "
           "from SEVEN distinct tids and +0x18 NEVER - but those are the threads "
           "calling the ITEM hook, and a probe on our own hook threads cannot "
           "see the DRAW thread by construction. INV-169 filed exactly that as "
           "[UNKNOWN]. This probe adds the RENDER site. <<< "
           "It also reinterprets INV-169 for free: the two arrays diverging "
           ">100 mm on 47.8% of 78,142 samples is EXPECTED once +0x18 is named - "
           "a bone that moved is ~100 mm from where it was a frame ago. "
           "READS ONLY: one TlsGetValue and one deref, module base and TLS slot "
           "resolved ONCE into statics, deduped per (tid, site). It does NOT "
           "call the world accessor - INV-169 measured that a READ IS A WRITE on "
           "that path (bit1 SET 100% -> 50.0% from adding the call alone), so "
           "this probe perturbs nothing. "
           ">>> HEADSET, NOT FLAT, AND THE DISTINCTION IS PER SITE. <<< The READ "
           "is flat-safe by INV-11 (thread identity and an integer, no "
           "world-matrix numbers) and the BoneBlendTick and HookCorrectItem "
           "sites both fire in flat - 8 and 7 tids, all group 0, measured "
           "2026-09-24. But ovr_SubmitFrame is the site the question is ABOUT "
           "and it produced ZERO lines in flat, because flat never enters the "
           "Oculus submit path; that log carried no render-side subsystem at "
           "all, dlss included. A probe that ARMS in flat and ANSWERS only in "
           "the headset is a HEADSET probe. See PROBE_DEPS.md RULE 0c. "
           "IF A SITE REPORTS 2: the drawn arm is one frame behind everything we "
           "write - invisible still, growing with speed, the shape Jay reports "
           "while sprinting, and it would explain why the whole 'who writes the "
           "bone' family (INV-45/D-34) changed nothing. IF NO SITE REPORTS 2: "
           "the line is dead and U-19 closes."),
#endif
    S_BOOL("diag_fresh_anchor", DiagFreshAnchor,
           "Diagnostics: is a world COMPOSED from locals fresher than the cached one?",
           "Camera hook",
           ">>> D-78's LAST SURVIVING FRESH-ANCHOR CANDIDATE, IN ITS OWN WORDS: "
           "'the LOCAL path is genuinely different - GetElementLocalMatrix "
           "tail-jumps to RVA 0x4861D0, not 0x4862F0, so locals do NOT go through "
           "the world resolver, which makes composing element 8's world from the "
           "local chain the one remaining fresh-source candidate.' <<< "
           "The cached world is a DERIVED cache lagging 60-100 ms (INV-48); "
           "SetMeshElementsMatrices writes the LOCAL and marks the derived caches "
           "stale, so the local is the written-fresh side. THAT IS A READING OF "
           "ENGINE_ARCHITECTURE, NOT A MEASUREMENT - this probe is the measurement. "
           ">>> IT COMPOSES BOTH ELEMENT 8 AND ELEMENT 48 AND REPORTS THE STEP OF "
           "THE COMPOSED SEPARATION BESIDE THE CACHED ONE. That pairing is the "
           "whole point: INV-208 measured the arm shake as element 8's step "
           "RELATIVE to element 48 (21 mm walking / 51 mm sprinting), and it is "
           "small BECAUSE both bones are stale and largely cancel. Freshening the "
           "anchor ALONE would turn that differential into the FULL lag - D-78's "
           "mechanism verbatim, 'converted shared motion into relative error'. A "
           "probe that only measured element 8 could not see that and would invite "
           "the same mistake a fourth time. <<< "
           "COMPOSED << CACHED in the sprint phase = composing from locals removes "
           "the jitter and is THE FIX. COMPOSED ~ CACHED = the jitter is in the "
           "locals too, D-78's last candidate DIES, and the anchor line closes "
           "entirely - the most valuable outcome, because it stops work. "
           "CONTROL, AND THE RUN IS VOID WITHOUT IT: standing still, composed and "
           "cached must AGREE and both steps must be ~0. Disagreement while still "
           "means the COMPOSITION is wrong rather than fresh. "
           "The arithmetic is INV-204/205's, already validated on this model at "
           "EXACT ZERO on four of five bones per arm (parentWorld * ownLocal == "
           "ownWorld), and every read is RAW - the world accessor is never called, "
           "because it IS dl_WorldResolver and would rebuild the world from the "
           "local, agreeing by construction (D-93). bit1-SET cached worlds are "
           "counted and NEVER merged (INV-6). "
           "COST: the chain is resolved ONCE PER MODEL into statics - walking "
           "desc->parent per tick is a pointer walk on a ~700 Hz path "
           "(aimhook.cpp:334). Per tick it is two short chains of 3x4 multiplies "
           "and four raw reads. WRITES NOTHING. "
           ">>> HEADSET: it rides SolveArmIk, so in flat the rig never solves and "
           "it reports NO SAMPLES - NO VERDICT. Needs diag_bone_dump=1 and the "
           "three master enables (INV-1). <<<"),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_rel_step", DiagRelStep,
           "Diagnostics: the anchor, the drawn hand and the weapon, stepped RELATIVE TO THE BODY",
           "Camera hook",
           ">>> THIS IS D-78's NAMED MISSING INSTRUMENT, QUOTED: '[IK JITTER] "
           "reports the anchor's ABSOLUTE step. The quantity that can reach the "
           "player is the anchor's step RELATIVE TO THE BODY, bounded by that "
           "stdev at ~27-45 mm rather than 159 mm, and nothing measures it. "
           "Measure that before building anything else against the anchor.' <<< "
           "WHY IT MATTERS: INV-140's second correction showed the anchor's 0.159 m "
           "median step is largely COMMON-MODE with the shoulder - |eye - element "
           "48| holds to a stdev of only 0.027-0.045 m - so the ABSOLUTE step is "
           "NOT evidence the player sees a 0.16 m arm error. D-78 is what acting "
           "on one costs: rate-limiting the anchor alone hit its pre-registered "
           "target on paper and produced 'arms lagging behind slightly and "
           "snapping back whilst moving'. "
           ">>> AND IT MEASURES ALL THREE IN ONE WINDOW, WHICH NOTHING HAS DONE. "
           "The weapon's shake was attributed to the anchor on a MAGNITUDE "
           "COINCIDENCE ACROSS THREE RUNS - 0.159 (INV-140), 0.174 (INV-162), "
           "0.10-0.29 (INV-203) - and INV-203 states its own limit: 'what this "
           "probe cannot do is say which end moved'. INV-163, the only entry "
           "asserting the arm and the weapon are SEPARATE, is already flagged IN "
           "DOUBT on Jay's own 2026-09-23 contradiction. <<< "
           "ALL THREE TERMS ARE IN ELEMENT 48's OWN FRAME, transpose(R48)*(p-t48), "
           "NOT world axes: a body-FIXED point still rotates in world axes when "
           "the player TURNS, which would report the turn itself as a step in the "
           "exact regime the question is about. INV-140 escaped this only because "
           "it used a SCALAR distance, which is rotation-invariant. Element 48 is "
           "the reference because INV-140 and D-78 used it, so the number is "
           "directly comparable to the 27-45 mm bound it tests. The basis is "
           "checked orthonormal and the sample REFUSED AND COUNTED if it is not "
           "(INV-11: world basis scale ~0.4 in flat against unit in VR). "
           "The drawn hand is read RAW from worldMatrices via ResolveSlotFlagOn, "
           "NEVER through the world accessor - that accessor IS dl_WorldResolver "
           "and with bit 1 set REBUILDS the world from the local, so reading "
           "through it would agree by construction (D-93's defect class). bit1-SET "
           "samples are EXCLUDED AND COUNTED, never merged (INV-6). "
           "COST: one subtract, 9 multiply-adds and a VecLen per term, plus ONE "
           "48-byte raw read. ResolveSlotFlagOn runs ONCE PER MODEL into statics, "
           "never per tick - it is a pointer walk and this path runs ~700 times a "
           "second (aimhook.cpp:334, and ignoring that cost two aborted headset "
           "runs on 2026-09-15). No accessor, no RttiVtableOf, no WatchExec slot. "
           "WRITES NOTHING - it changes no rig output and no item placement. "
           ">>> HEADSET ONLY. Terms 1 and 3 hang off the IK target, so IN FLAT the "
           "rig never solves, there are no controller poses (PROBE_DEPS) and the "
           "probe prints 'NO SAMPLES - NO VERDICT' naming the path rather than "
           "printing zeros. That is [ABSWORLD]'s exact failure and it cost a run. "
           "DEPENDS ON diag_bone_dump=1 and the three master enables (INV-1), and "
           "[SHOULDER] latched must be present (INV-2). <<<"),
#endif
    S_BOOL("diag_root_track", DiagRootTrack,
           "Diagnostics: does the ITEM'S ROOT CONTROL OBJECT actually track the hand?",
           "Camera hook",
           ">>> MEASURES INV-198's CANDIDATE, WHICH IS NOT ABOUT THE GUITAR. <<< "
           "INV-198 read the tree off live data: element 71 (r_handholder) -> the "
           "item's ROOT CONTROL OBJECT -> `holder` -> every other element. A weapon "
           "WITH a holder has that holder rewritten onto the handholder by "
           "CorrectItemPosition EVERY FRAME. The guitar (INV-195) has no holder, so "
           "every element hangs straight off the root and nothing corrects it. "
           "IN FLAT THE GUITAR IS CORRECT, so the root tracks the hand there; IN VR "
           "IT IS NOT, while `holder` is. That is consistent with THE ROOT NOT "
           "TRACKING THE HAND IN VR AT ALL, with the per-frame holder rewrite "
           "MASKING it on every weapon that has one - in which case every item fix "
           "of 2026-09-23 has been compensating downstream of a root in the wrong "
           "place. THE ROOT'S TRANSFORM HAS NEVER BEEN MEASURED. This measures it. "
           "PER LINE IT PRINTS: the root's world position, |root - handholder| and "
           "the angle between them (THE HEADLINE), |root - hand bone|, and "
           "|holder - handholder| as THE CONTROL - INV-47 measured that control at "
           "0.0000 m, so ROOT FAR + HOLDER ~0 IS THE MASK and ROOT ~0 KILLS THE "
           "CANDIDATE. "
           "THE WALK IS INV-198's, already proved on live data with ZERO refusals: "
           "DescContainerOf(item) -> +0x38 descArray -> element 0's desc -> +0x00 "
           "parent = THE ROOT'S desc. Element 0 is right for BOTH cases and that is "
           "MEASURED, not assumed: on a weapon element 0 is `holder` whose parent is "
           "the root, and on the guitar all five elements share that same parent. "
           "THE ROOT'S WORLD IS READ TWO WAYS: (A) dl_WorldResolver, engine "
           "RVA 0x4862F0, which IS what GetElementWorldMatrix calls "
           "(re_analysis/gu_elemdesc_layout.txt PART B), SEH-guarded; and (B) the "
           "raw worldMatrices(+0x30) and altMatrices(+0x18) slots plus the dirty "
           "bit. The line says WHICH RAW ARRAY (A) AGREED WITH, which closes "
           "INV-178's double-buffer question for the root at no extra cost and "
           "leaves numbers on the page if the resolver ever refuses. "
           ">>> PRE-REGISTERED VOIDING CONDITION, A REAL RISK: (A) IS NOT "
           "SIDE-EFFECT-FREE. INV-169/176 record that the world accessor clears "
           "bit 1 and recomposes from the local, and the item ROOT is a container "
           "we have NEVER touched. IF ANY WEAPON BEHAVES DIFFERENTLY WITH THIS ON "
           "THAN WITH IT OFF, THE PROBE IS THE CAUSE AND THE RUN IS VOID - ask and "
           "record the answer either way. <<< "
           "COST: per hand per frame, one resolver call + two SafeGetWorldPtr + ~8 "
           "pointer reads, the same order as [ITEMREL]'s existing two accessor "
           "calls on the same path. NO RttiVtableOf, NO WatchExec slot. Logging is "
           "throttled to 1 line/sec/hand and the FIRST sample per item pointer is "
           "logged UNCONDITIONALLY. Every failure REFUSES BY NAME - a null must "
           "never read as 'the root is at the hand'. "
           "WRITES NOTHING. WORKS IN FLAT - and flat is the control leg, because "
           "the guitar is correct there. DEPENDS ON diag_bone_dump=1 and the three "
           "master enables (INV-1), and an item being HELD."),
    S_BOOL("diag_orphan_rel", DiagOrphanRel,
           "Diagnostics: PHASE 1 - are the item's PARENTLESS elements coherent when we capture?",
           "Camera hook",
           "PHASE 1 OF docs/PROPOSAL-PARENTLESS-ELEMENTS-2026-09-23.md, AND IT "
           "IS BUILT TO KILL THAT PROPOSAL AS READILY AS CLEAR IT. READ-ONLY, "
           "WRITES NOTHING. "
           "THE DEFECT IT SERVES: an item element with NO PARENT has nothing for "
           "dl_WorldResolver to recompute it from, so our holder write never "
           "reaches it - which is the Volkatronix's detached sight (INV-192 "
           "proved OUR WRITE causes it), Aesir's floating blade (INV-190) and "
           "the guitar. The proposed fix captures each parentless element's pose "
           "RELATIVE TO HOLDER at the instant after the engine's own placement, "
           "then replays it onto our holder. "
           ">>> IT ALL RESTS ON ONE UNVERIFIED PREMISE: are those elements "
           "COHERENT at that instant? With our writer OFF the sight is attached, "
           "so the engine keeps them coherent at SOME point in the frame - but "
           "nothing establishes that point is BEFORE our capture. If it is after, "
           "a fix built on this captures a STALE pose and writes it forward, "
           "which looks like a PARTIAL fix and is the worst outcome to debug. <<< "
           "THE STATIC PRE-CHECK DID NOT SETTLE IT: vtbl+0xCB8 resolved to "
           "sub_180C81190, which touches the held-item slots but only reads four "
           "element positions and writes ONE, smoothed - not a general item "
           "update. A wrong idea died cheaply; the premise survived. "
           "THE VERDICT IS PRE-REGISTERED IN THE LINE ITSELF: relH CONSTANT while "
           "held prints '*** STABLE - the capture instant IS coherent, the "
           "phase-2 arithmetic is SOUND ***'; relH DRIFTING prints '>>> DRIFTS - "
           "we would be capturing a STALE pose and writing it forward. THE "
           "PHASE-2 DESIGN IS WRONG AND MUST NOT BE BUILT. <<<'. Thresholds 5 mm "
           "and 2 deg, measured against THIS HOLD'S FIRST SAMPLE and never reset "
           "per window, because the question is drift across the whole hold. "
           "It also logs the ids of BOTH 'Holder' and 'HolderL' per hand: "
           "sub_180C81190 uses holderl for the LEFT while CorrectItemPosition "
           "uses holder for BOTH, and INV-185 measured the handholder frame as "
           "identity for the left-hand bow - so our use of holder matches the "
           "placement. Logging both makes that checkable rather than argued. "
           "COST: the parentless element list is walked ONCE PER ITEM and cached; "
           "per frame one accessor read and one inverse+compose per parentless "
           "element, typically 1-2. GetElementNameCStr runs once per element at "
           "slot claim, never per frame. "
           "HEADSET - the premise is about OUR capture instant while the rig is "
           "solving. DEPENDS ON diag_bone_dump=1 and the three master enables "
           "(INV-1), and on item_pose_fix=0 AND item_pose_basis=0, which keep "
           "gamedll+0xC62AC0 with itempos where the capture runs."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_item_elem_dump", DiagItemElemDump,
           "Diagnostics: the HELD weapon's FULL element table, once per weapon",
           "Camera hook",
           "DECIDES WHETHER A WEAPON'S SIGHT IS PART OF ITS MESH OR A SEPARATE "
           "ATTACHED OBJECT, AND IS BUILT TO KILL THE HYPOTHESIS AS READILY AS "
           "CONFIRM IT. Jay, 2026-09-23: the 'Volkatronix', an AR WITH A SIGHT, "
           "placed correctly while 'the entire Sight array was detached from the "
           "weapon itself, locked to the shoulder (Flatscreen positioning)'. The "
           "weapon moved; its attachment did not. INV-181 names a mechanism that "
           "would do exactly that: we place with IDIOM A, "
           "SetElementWorldMatrix(item, Holder, world), which moves ONE ELEMENT'S "
           "world, while the engine's carried-object path uses IDIOM B, "
           "SetWorldXform(itemROOT, ...), which moves the ROOT so CHILDREN "
           "FOLLOW - and DECISIONS:1551 establishes the weapon is itself a child "
           "control object attached via AttachChildToMeshElement, so a sight may "
           "be a further child left behind by idiom A. "
           ">>> THE READ THAT DECIDES IT: sight names PRESENT in the weapon's own "
           "element list means idiom A moved them, the sight is part of the mesh, "
           "and THE HYPOTHESIS IS DEAD - the detachment has another cause. Sight "
           "names ABSENT means a separate attached object and idiom B is the "
           "shape of the fix. <<< Deciding this from the NAME 'Sight array' is "
           "what CLAUDE.md forbids outright; the element table is the evidence. "
           "[ITEM] already prints elements= and the FIRST FIVE names, which can "
           "only hint - this prints the WHOLE table, index -> name -> BONE. "
           "ONCE PER DISTINCT ITEM POINTER: ElementTableDump dedupes internally, "
           "and its cap was raised 8 -> 32 for this, because the player takes a "
           "slot and every weapon swap or re-equip is a new model pointer - at 8, "
           "a four-weapon session would have silently skipped the LATER weapons, "
           "which are the ones being investigated. It still SAYS SO when it fills. "
           "IT WRITES NOTHING. RttiClassNameOf runs inside the dump, AFTER the "
           "dedupe check, so the uncached RTTI walk never reaches a per-frame "
           "path. "
           "WORKS IN FLAT for the element table itself (structural - INV-11 "
           "permits identity, class and existence), but >>> THE SYMPTOM IT "
           "EXPLAINS ONLY APPEARS IN THE HEADSET <<< because in flat our writer "
           "never runs (measured: [ITEMCTL] placed=0, and [ABSWORLD] refuses with "
           "'the rig has never solved this side'), so nothing detaches and flat "
           "would show a clean picture of a broken thing. "
           "DEPENDS ON diag_bone_dump=1 and the three master enables (INV-1) for "
           "the capture, and on an item being HELD - it rides "
           "CorrectItemPosition."),
#endif
    // LOCKED 2026-10-08 (Jay: "Item_rel_hold is to be locked as it's now a Tier A fix") at the compiled 3 - never read from
    // the file, never drawn. Mode 3 = climbs + kicks / slides + the bow (INV-490, INV-491, D-201).
    S_INT_LOCKED("item_rel_hold", ItemRelHold, 0, 3,
          "Rig: hold the weapon in the palm through climbs, kicks and slides (3)", "Camera hook",
          "3 = while CLIMBING, KICKING or SLIDING: those animations swing the weapon away from your hand and back; with 3 "
          "the weapon keeps the grip it had standing still until the move is over and the game's grip is back at your "
          "hand, so it never flies off. Each weapon's grip is remembered, so the first kick after drawing is held too; "
          "the bow's grip is only taken once it has settled. Needs item_hide_by_state (it reads the climb / kick state). "
          "0 = off, 1 = melee weapons only, 2 = every weapon except the bow. While you "
          "move, the game's walk and sprint animation shifts the weapon's grip point "
          "around your hand - up to 2.5 cm and 6 degrees per frame in a melee sprint. "
          "This keeps the grip you had while standing still for as long as you move, "
          "then eases back to the live grip over a quarter of a second when you stop. "
          "Standing still is unchanged. Modes 1 and 2 never hold the bow (it settles into its "
          "carry pose over a few seconds). Needs item_place_rel=1. Takes effect at launch."),
    S_BOOL("item_place_rel", ItemPlaceRel,
           "Item: place the weapon with THE ENGINE'S OWN per-weapon grip transform",
           "Camera hook",
           "FIXES THE ROTATION, AND THE BUG IS ONE LINE. ItemCtlPlace writes the "
           "item's basis as `src = g.drawn` - THE HAND BONE'S BASIS, DIRECTLY, "
           "with no per-weapon term. The engine writes handBone * rel, where rel "
           "is the weapon's authored grip transform. INV-185/186 measured that "
           "transform IDENTICAL IN FLAT AND IN VR to four decimals, and it is "
           "neither identity nor the same between weapons: melee 0.0970 m / "
           "90.06 deg, the two-handed Scythe 0.1086 m / 90.06, the AR 0.0939 m / "
           "85.55, the bow carried 0.1158 m / 179.24. So the weapon comes out "
           "rotated by exactly the term we failed to apply - and Jay reported "
           "precisely that, per weapon, WITHOUT seeing the numbers: melee and the "
           "AR 90 deg to the right, the bow upside down needing 180. "
           ">>> WHY THE OLD CODE BELIEVED IT WAS RIGHT: MeleeGripSnapshot's own "
           "comment says the handholder's authored local rotation is IDENTITY "
           "(INV-161) so the weapon should follow the drawn hand bone. The "
           "AUTHORED value is identity and INV-161 stands - but the LIVE "
           "handholder is 90.06 deg from the live hand bone. INV-4 is exactly "
           "this trap: authored data is not constant for the bones the rig cares "
           "about. A rest-pose value was used where a live one was needed. <<< "
           "AND THE GUITAR IS THE CONTROL FROM THE OTHER SIDE: it is two-handed, "
           "our writer refuses it, the ENGINE places it, and it is the ONE weapon "
           "Jay reports correctly oriented. The weapon we do not touch is the one "
           "that looks right. "
           "HOW: every frame, inside itempos::HookCorrectItem AFTER the original "
           "has run, rel = inverse(handBoneWorld) * itemHolderWorld is captured "
           "per (hand, item); ItemCtlPlace then writes basis = drawn * rel.R and "
           "position = wrist + drawn * rel.T. NOTHING IS HARDCODED - no table, no "
           "dial - so a weapon nobody has ever measured works too, and "
           "item_basis_yaw_deg becomes dead. "
           "RE-READ EVERY FRAME, NOT LATCHED ONCE, DELIBERATELY: "
           "CorrectItemPosition overwrites the item Holder unconditionally each "
           "frame so the value read is always the engine's fresh one, never our "
           "own write; latching once would freeze the BOW at whatever state it "
           "was equipped in, and INV-185 measured the bow moving 131 -> 179 deg "
           "over ~3 s with a further real state change on draw. "
           "NO SILENT FALLBACK: with no rel yet, ItemCtlPlace SKIPS its write and "
           "says so once by name, so the engine's placement stands for that frame "
           "- right orientation, not following the hand, the guitar behaviour - "
           "which is visibly different from 'the fix is working'. "
           "DEFAULT OFF so the A/B is one setting inside one run. "
           "DEPENDS ON item_place_from_controller=1 (it modifies THAT writer), a "
           "SOLVING rig, diag_bone_dump=1 and the three master enables (INV-1). "
           ">>> AND ON item_pose_fix=0 AND item_pose_basis=0: those steal "
           "gamedll+0xC62AC0 from itempos, which is where the capture runs - a "
           "climbing norel= with placed=0 is that collision, and the log says so. "
           "<<< IT DOES NOT FIX THE SNAPPING WHILE MOVING: that is INV-165/168's "
           "every-second-call cadence and INV-48's 60-100 ms accessor lag, a "
           "SEPARATE defect, and this changes neither."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_fastik_weight", DiagFastIkWeight,
           "Diagnostics: the ENGINE'S OWN arm-IK blend weight, per hand, player only",
           "Camera hook",
           "TESTS INV-186's CANDIDATE MECHANISM, AND IS BUILT TO KILL IT AS "
           "READILY AS CONFIRM IT. INV-186 measured the hand bone 0.233 m from "
           "the rig's controller-derived target ONLY while a firearm was held - "
           "flat-stable, starting 2 s after the AR was equipped, recovering the "
           "moment the hand went empty - while melee and a two-handed Scythe on "
           "the SAME hand in the SAME run read 1.6-2.8 mm. [AIM] read delta "
           "+0.000 throughout, so our solver hit its own target while the "
           "engine's bone was a quarter-metre away. STATICALLY "
           "(re_analysis/ar_fastik_weight.txt) the player's visual update runs "
           "the ENGINE'S OWN arm IK on both hands every frame: FUN_180c1f9b0 -> "
           "sub_180C72500 -> FastIkHandPosAndRot(entity, hand, goal, WEIGHT), "
           "where the weight comes from FUN_180d92a70 - a TIME ENVELOPE ramping "
           "0 to 1 over a fade - and FastIk resolves R_HandHolder/L_HandHolder BY "
           "NAME and solves the arm to put the HANDHOLDER on its goal, dragging "
           "element 53 with it. A weight ramping to 1 and then holding produces "
           "exactly the 2-second onset and flat plateau that was measured. "
           ">>> THAT PASS READ CODE, NOT VALUES. THIS READS THE VALUES. <<< "
           "THE DISCRIMINATOR, STATED BEFORE THE RUN: the blend weight per hand, "
           "keyed on the held class from the [ITEM]/[FLATREF] lines by timestamp. "
           "~0 with melee and ~1 with a firearm CONFIRMS the engine re-poses the "
           "arm for firearms and our write is overridden. The SAME value for "
           "both, or ~0 for both, KILLS the candidate outright - and the line "
           "says so in those words. The goal position is logged too; its distance "
           "from our own target is the other half and should read ~0.23 m if this "
           "is right. "
           "IT WRITES NOTHING and changes NO behaviour: it reads the parameters "
           "already passed to a hook that exists, before anything can modify "
           "them. NOT logged deliberately: the weapon CATEGORY at *(obj+0x38) - "
           "`self` is the entity, not sub_180C72500's local object, and that "
           "offset chain has NOT been read; inventing it would break the "
           "decide-it-by-READING-it rule. "
           "gamedll+0x10B7D80 can hold only one detour (a second would collide, "
           "the trap item_pose_basis sprang on [FLATREF]); this installs a pass-through "
           "that writes nothing, so arming this cannot change what the rig does. "
           "HEADSET: the comparison needs a SOLVING rig and a real controller "
           "pose. DEPENDS ON diag_bone_dump=1 and the three master enables "
           "(INV-1) for the player capture - without them every call counts as "
           "no-capture and the line SAYS SO rather than reading as a zero."),
#endif
    S_BOOL("diag_flat_ref", DiagFlatRef,
           "Diagnostics: THE REFERENCE LEG - the weapon's pose in the hand's own frame",
           "Camera hook",
           "THE INSTRUMENT THIS PROJECT HAS NEVER HAD, AND D-96 IS WHY IT IS "
           "NEEDED. Every previous 'the weapon matches' number compares us "
           "against ourselves: [ITEMDEV]'s two sides BOTH hang off the drawn "
           "hand bone - ours is that bone's basis H, the engine's is the "
           "handholder H*L - so H CANCELS EXACTLY and a wrong ARM is invisible "
           "to it, for every weapon class. It cannot tell you a weapon is "
           "correctly oriented in the hand, and nothing else here can either. "
           "THIS MEASURES rel = inverse(bodyElementWorld) * itemHolderWorld - "
           "the weapon's pose expressed in a BODY element's own frame, in TWO "
           "frames: the HAND BONE (53/17, what our rig drives) and the "
           "HANDHOLDER (71/35, what the engine attaches the item to). That "
           "quantity is INVARIANT to where the arm actually is. Flat and VR put "
           "the hand in completely different places, but if the weapon is SEATED "
           "correctly the number is THE SAME in both. So the FLAT capture is the "
           "REFERENCE - what it is meant to be - and a DIFFERENT number in the "
           "HEADSET is the seating error, DERIVED rather than dialled, per "
           "weapon class. It also decomposes the problem properly: rel wrong = "
           "the weapon is not seated on the hand; rel right but it still looks "
           "wrong = the HAND is in the wrong place, which is INV-47's arm "
           "problem and which this probe deliberately cannot see. "
           ">>> CAPTURE THE FLAT LEG FIRST, AND STANDING STILL. <<< U-19 is open "
           "and INV-48 measures GetElementWorldMatrix lagging 60-100 ms IN "
           "MOTION, so a moving capture is not a reference. "
           "IT WRITES NOTHING - SafeGetWorldPtr and GetElementID, no patch, no "
           "hook of its own (it rides the UNCONDITIONAL itempos pass-through, "
           "reading AFTER the original returns so it sees what the engine "
           "actually placed), and NO WatchExec slot, so it cannot collide with "
           "the one global client. COST: three accessor reads and ~40 flops per "
           "hand per call against [RELCHURN]'s 93 reads per frame; "
           "GetElementID and RttiClassNameOf run ONCE PER SLOT, never per frame. "
           "INV-11 IS HANDLED: every basis is column-normalised, the column "
           "scales are printed, and the reference basis's off-diagonal error is "
           "measured - above 0.01 the line states in words that the POSITION "
           "numbers are suspect, because transpose is only the inverse of an "
           "ORTHONORMAL basis and flat's 0.4-scale defect looks exactly like "
           "that. "
           "FLATSCREEN IS ENOUGH and the CHAIN is why, not the setting: nothing "
           "in it touches a controller pose, SolveArmIk, the head pose, the "
           "camera hook or an XR session. "
           "DEPENDS ON diag_bone_dump=1 and the three master enables (INV-1) for "
           "the captured player model, AND an item actually HELD - it rides "
           "CorrectItemPosition, which only runs with something in hand. It "
           "REFUSES BY NAME, once, naming the exact pointer or setting that "
           "failed, and prints its first sample UNCONDITIONALLY."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_rel_churn", DiagRelChurn,
           "Diagnostics: which element moves relative to the EYE, ranked, by name",
           "Camera hook",
           "ANSWERS 'WHAT IS SWAYING' WITHOUT A HYPOTHESIS, AND NAMES IT. Every "
           "sway round so far has begun by naming a bone and spending a run "
           "testing it - element 8's jitter, the eyecamera's own animation, "
           "rig_anchor_from_spine. This needs no candidate. It transforms EVERY "
           "element into eyecamera-local space each frame, accumulates "
           "per-element positional and rotational travel, and once a second "
           "prints the four largest of each with the ENGINE'S OWN NAME for the "
           "element, plus an immediate [RELJUMP] line naming anything that "
           "moves >2.5 cm or rotates >15 deg relative to the eye in one frame. "
           "Anything moving relative to the eye is by definition something the "
           "player sees move. "
           "Every number printed is produced by OUR binary. "
           "IT WRITES NOTHING: GetElementWorldMatrix and nothing else, no hook, "
           "no patch, and NO WatchExec slot, so it cannot collide with the one "
           "global client. COST: 93 accessor calls and 93 small matrix "
           "multiplies per frame; GetElementNameCStr runs ONCE PER MODEL and the "
           "names are cached, so the expensive call is out of the hot path. "
           "FLATSCREEN IS ENOUGH - nothing in the chain touches a controller "
           "pose, SolveArmIk or the VR camera path, and it rides the same tick "
           "site and the same model source as [ELEMDUMP], which produced a full "
           "player table in flat on 2026-09-20. "
           "DEPENDS ON diag_bone_dump=1 (which CAPTURES the player model), the "
           "three master enables (INV-1), and GAMEPLAY - the model is captured "
           "at CorrectItemPosition, which only runs while an item is HELD. It "
           "refuses BY NAME, naming the missing link, in every one of those cases."),
    S_BOOL("diag_elem_dump", DiagElemDump,
           "Diagnostics: dump a model's element table (index, name, BONE)", "Camera hook",
           "ANSWERS 'WHAT DOES melee_force_elem=N ACTUALLY DO' - and nothing short of "
           "it can. Walks GetElementsNumber and prints, per index, the engine's own "
           "GetElementNameCStr AND GetBoneIDFromMeshElem. The BONE column is the point: "
           "INV-64 establishes melee damage lands on a bone DERIVED from the element, "
           "so a name alone is not an answer. Supersedes the .chr route in "
           "ghidra-project/chr_tables/, whose index is unvalidated and which contains "
           "no bone mapping at all. SELF-VALIDATING ON THE PLAYER: it prints a PASS/FAIL "
           "against INV-34's r_handholder=71 / l_handholder=35, and a FAIL there means "
           "the whole dump is unusable. One dump per distinct model, capped at 8. "
           "FLATSCREEN is enough for the PLAYER model (element identity is structural, "
           "INV-11); a ZOMBIE model needs the headset, because the entity capture rides "
           "weaponanchor -> LevelCamera -> HorizonLock -> the VR camera path."),
    S_BOOL("diag_holder_id", DiagHolderId,
           "Diagnostics: which element IS \"R_HandHolder\"", "Camera hook",
           "ASKS THE ENGINE THE ONE THING THE RIG HAS ONLY EVER ASSUMED. "
           "CorrectItemPosition fetches the hand bone BY NAME - GetElementID(model, "
           "\"R_HandHolder\") at correct_item_position_decompiled.txt:53 - and INV-33 "
           "has established it does that on OUR model. But \"71 = r_handholder\" comes "
           "from our stage-0 element table, NOT from the engine's own lookup. PlayerDI "
           "has 92 elements including the FPP arms at 86-91, and [ANIMMAP] found 75 "
           "bound entries against 86 matrix slots - eleven slots animated by nothing. "
           "If a second similarly-named socket resolves FIRST, the weapon hangs off a "
           "bone nobody drives, and every measurement of element 71 since 2026-09-10 "
           "was aimed at a bone the engine never consults. DOES BOTH HALVES: calls the "
           "engine's own lookup with the EXACT strings from the decompile, AND walks "
           "every element name printing each one containing \"holder\" with its parent "
           "- because GetElementID returns ONE id and cannot say whether a second "
           "element shares the name, and 'the first match' is the failure mode under "
           "test. A case mismatch that makes the lookup return -1 shows up as a real "
           "name in the enumeration instead of as silence. Prints the PARENT of each "
           "hit, because the answer that matters is whether it hangs off the hand we "
           "drive (53 / 17), not the number itself. One-shot per model. Pure read - "
           "all four are const accessors. REFUSES BY NAME if any export is null, "
           "because a null accessor would report 'no match' for every element and read "
           "exactly like a finding. WORKS IN FLATSCREEN (element identity is "
           "structural, INV-11) but needs an item held once for the model capture."),
    S_BOOL("diag_item_model", DiagItemModel,
           "Diagnostics: U-12 - is the item's hand bone on OUR skeleton", "Camera hook",
           "THE QUESTION THAT CAN INVALIDATE THE WHOLE ARM-SIDE APPROACH. "
           "CorrectItemPosition (gamedll+0xC62AC0) is not handed a model - it resolves "
           "the hand bone BY NAME through a virtual on an object EMBEDDED at "
           "entity+0x40, read from our own binary in "
           "re_analysis/correct_item_position_decompiled.txt:46. The object is "
           "EMBEDDED (this = entity+0x40, and *(entity+0x40) is its VTABLE - camhook "
           "corrected exactly this misreading on 2026-09-07 when a live run returned a "
           "MODULE address there against a heap entity), and slot 1 (+8) returns the "
           "IModelObject* the weapon is placed from. WHETHER THAT IS THE MODEL THE RIG "
           "WRITES HAS NEVER BEEN CHECKED. It matters because INV-27 measured the "
           "anchor's world as ALREADY CORRECT relative to our hand (0.0970, authored) "
           "while the weapon still tracks the animation, and INV-31 removed the "
           "re-binding explanation - a correct anchor with a wrong weapon is what a "
           "SECOND SKELETON looks like. THIS IS THE ONLY PROBE IN THIS FAMILY THAT "
           "CALLS ANYTHING: one virtual, at 1 Hz, and it REFUSES TO CALL unless the "
           "resolved slot lands in EXECUTABLE memory (VirtualQuery, PAGE_EXECUTE*) - a "
           "vtable slot read from the wrong object is a jump to arbitrary memory and "
           "SEH is not a licence to make it. SEH wraps the call anyway and a fault is "
           "logged by name. Emits three model pointers, their inners and their RTTI "
           "class names; READ THE INNER, not the model pointer - PlayerDI has 25 "
           "base-class sub-object views that share an inner. WORKS IN FLATSCREEN "
           "(pointer identity is structural, INV-11) but needs an item held at least "
           "once, because CorrectItemPosition only runs for a held item."),
    S_BOOL("diag_anim_map", DiagAnimMap,
           "Diagnostics: the animation BINDING MAP", "Camera hook",
           "THE ENGINE'S OWN PER-ELEMENT 'TAKE THIS BONE OFF THE ANIMATION' SWITCH, "
           "read rather than named. E+0x1908 is a 0x10-stride map: +0x00 a u16 gate, "
           "+0x04 a destination index, +0x08 flags. Read from our own binary in "
           "re_analysis/blend_callers.txt, TWO consumers test the SAME u16 - "
           "FUN_1800D7380 line 679 (decompress the TRS key into matrices[dst]) and "
           "FUN_1800DD7B0 line 810 (read matrices[dst], quaternion-blend it toward a "
           "reference and write it back, additionally gated on flags & 0x700). So one "
           "u16 takes an element off BOTH stages. This is NOT Block(i), whose bit also "
           "clears the mesh element's RENDER bit and therefore cannot drive a visible "
           "bone (D-26); this gate renders, which is why diag_trs_skip_test produced a "
           "VISIBLE wrong arm (D-11). TWO OPEN QUESTIONS IT ANSWERS: (1) which elements "
           "have entries at all - TrsSkipTick has only ever scanned for one, and D-11's "
           "chimeric-chain verdict was measured with exactly ONE element skipped, which "
           "is its stated regime; (2) what +0x04 indexes - the 2026-09-08 dump says bone "
           "57 -> element 48 with matIdx 57, while the [TRS] probe and D7380's own use "
           "say it is the ELEMENT index, and printing i against dst settles it. Dumps "
           "every entry once per map allocation, then watches 45/48/49/50/53/71 and "
           "9/12/13/17/35 at 1 Hz with session-sticky 'ever seen' flags so an entry that "
           "only exists while a weapon is held is not missed between samples. EMITS "
           "VALUES, NOT A VERDICT. Pure read - no SafeWrite in the function. WORKS IN "
           "FLATSCREEN: existence, indices and flags are structural, the only kind INV-11 "
           "lets flat answer, and it makes no numeric claim about a world matrix."),
    S_BOOL("diag_anim_layers", DiagAnimLayers,
           "Diagnostics: the animation LAYER stack", "Camera hook",
           "TESTS THE LEADING HYPOTHESIS FOR THE HELD-ITEM ARM FAULT. The drawn-arm "
           "error is a held item TIMES locomotion interaction (0.002 item alone, "
           "0.004 locomotion alone, 0.37 together) and it follows the hand holding "
           "the item. Geometry is eliminated (bases orthogonal, compose exact, bone "
           "lengths pinned) and so is write survival - 95.7% with empty hands where "
           "the arm is PERFECT against 97.8% with the bow where it is 0.38 m out, "
           "i.e. the wrong direction to be the cause. Read from blend_callers.txt: "
           "FUN_1800D7380 is a LAYERED blender, E holds EIGHT layer slots of 0x320 "
           "bytes (0x1900/0x320 = 8 exactly) and E+0x1900 is their ACTIVE MASK; each "
           "layer carries its own matrix array (+0x2d0), count (+0x2d8) and weight "
           "(+0x318), and the blend gate is bit AND weight>0 AND count!=0. This logs "
           "the mask, all eight weights and the active set, mirroring that gate "
           "exactly, and logs IMMEDIATELY on any change of the active set because "
           "the TRANSITION is the signal. PASSING VALUE: a bit appears, or a weight "
           "goes 0 -> non-zero, when an item is drawn while running -> mechanism "
           "found. Identical mask and weights across both -> the hypothesis is dead "
           "and the log says so. Pure read. WORKS IN FLATSCREEN."),
    S_BOOL("diag_spine_chain", DiagSpineChain,
           "Diagnostics: the parent chain ABOVE hspine1", "Camera hook",
           "THE NEVER-MEASURED HALF. Our LOCAL write is provably correct at every "
           "step and the composed WORLD is still 0.4-0.5 m off, matching the drawn "
           "error to 2 cm on BOTH arms - so the error enters through world_parent. "
           "The clavicle is excluded and hspine1 holds; nothing has ever looked "
           "above it. Walks the parent chain out of the DESCRIPTORS (so the "
           "zero-length joints 1 and 3 are included without assuming they are "
           "there) and logs, per element: local |t| with its SESSION SPREAD, the "
           "world separation, the compose predicted from the parent's basis with a "
           "transposed control, and the world basis COLUMN LENGTHS AND DOTS. "
           "READ THE DOTS - unit column lengths do not imply orthogonality, and "
           "measuring only the lengths is what hid the clavicle for five months. "
           "The verdict is on the SPREAD, not on a latched rest: the animation "
           "rewrites these translations every frame, so a latch catches whatever "
           "pose it happened to land on (that is how [SHOULDER] latched 0.1936 "
           "against an authored 0.1718), whereas a bone offset cannot change at "
           "all, so any spread is the fault. >>> IT MUST BE RUN WHILE MOVING: the "
           "error is 0.002 m standing still, 0.045 m walking and 0.34 m running, "
           "so a standstill run returns a clean verdict having tested nothing. A "
           "locomotion proxy is built in and the probe refuses to imply a result "
           "from a stationary window. <<< Pure read. Needs diag_bone_blend_probe "
           "and diag_bone_dump, the rig's master enables."),
#endif
    S_BOOL("diag_trs_skip_test", DiagTrsSkipTest,
           "Diagnostics: take one bone off the animation", "Camera hook",
           "THE MANUAL-BOW-DRAW EXPERIMENT. FUN_1800D7380 decompresses the "
           "animation's TRS keys over the whole local-matrix array every frame and "
           "copies it back, which is what our arm writes have been racing. Its loop "
           "skips any entry whose gate short is zero, and a skipped element keeps "
           "whatever was already there - our write. This zeroes that gate for the "
           "element named by rig_arm_element (NOT rig_world_watch_elem), per-element, "
           "so the LEFT arm can "
           "keep the bow animation while the RIGHT goes silent. It REFUSES to write "
           "unless the key it read validates as a real TRS key (unit quaternion, "
           "finite, sane translation), and restores the original gate when switched "
           "off."),
    S_BOOL("diag_view_body", DiagViewBody,
           "Diagnostics: where the body moves in your view", "Camera hook",
           "Log-only. The drawn pelvis, spine base, eye bone, clavicles, shoulders, elbows, "
           "hands and weapon sockets, measured in the frame you actually SEE (the rendered "
           "view camhook writes), sampled inside that camera write so the camera and the "
           "body come from the same frame. Per 5 s and per speed/item: up-down and sideways "
           "motion per frame, each also minus the spine base's (does it move with the body "
           "or on its own?), and rotation. Frames where the view turned are excluded. "
           "Headset only. Off by default."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_throw_arm", DiagThrowArm,
           "Diagnostics: drawn right arm vs the rig's solve", "Camera hook",
           "Log-only. Rides the body-in-view probe above (needs it ON). Per frame: is the right "
           "arm you SEE still the arm the rig solved? The angle between the drawn upper arm and "
           "forearm and the solved ones, how far the drawn shoulder sits from the shoulder the "
           "solve assumed, the drawn bone lengths, and how close the solve is to full reach - "
           "split the same way, including the melee throw's ready and release. Headset only. "
           "Off by default."),
    S_BOOL("diag_arm_capture", DiagArmCapture,
           "Diagnostics: record the arms for offline analysis", "Camera hook",
           "Log-only. Ten times a second per arm it writes, to its own file dlvr_armcap.log beside the main "
           "log, everything the arm placement was given and everything it produced: head and controller "
           "poses, shoulder, hand target, reach, the elbow, the rotations written to the upper arm, forearm "
           "and hand, the forearm twist, and the game's own matrices for the collarbone, upper arm, forearm, "
           "forearm twist bone, hand and weapon holder - plus every bone of the body once a second, and the "
           "bone table once. It is for replaying arm fixes on real poses without the headset. Changes nothing "
           "you see. The file is started fresh each launch, so copy it before relaunching. Headset only. Off "
           "by default."),
#endif
#if !defined(DLVR_BETA_LEAN) || defined(DLVR_SWING_PROBE)   // the swing probe variant (trace 20261119_1730) registers it too
    S_BOOL("diag_swing_shadow", DiagSwingShadow,
           "Diagnostics: melee swing shadow test (the weapon you see vs the line the game tests)", "Camera hook",
           "Log-only. On every VR melee swing (melee_swing_mode 2) it also tests the weapon as it is drawn - "
           "from its grip to its blade end - with the game's own target search, beside the game's real hit, "
           "and logs per swing what each would have hit, how far apart the two are, and how long your arm is "
           "at that moment. Per-frame rows go to their own file dlvr_swingshadow.log. Changes no hit. Headset "
           "only. Off by default."),
#endif
    S_BOOL("diag_elbow_side", DiagElbowSide,
           "Diagnostics: which way each elbow points", "Camera hook",
           "Log-only. For each arm, every time the rig places it: how far the elbow sits to the "
           "outside or the inside of the straight line from shoulder to wrist, and how far above "
           "or below it, measured against your body's facing. Beside it, the two things the rig "
           "blends to choose the elbow's direction (straight down, and opposite to where the hand "
           "points) and whether the ceiling on the elbow's height stepped in. Split by how close "
           "the hand is to the shoulder. Says so in the log if an arm is not being placed. "
           "Headset only. Off by default."),
    S_BOOL("diag_gun_hand", DiagGunHand,
           "Diagnostics: is the weapon where the drawn hand is", "Camera hook",
           "Log-only. Rides the body-in-view probe above (needs it ON). With a weapon in the "
           "right hand and the view still: how far, and in which direction (right / up / back "
           "in your view, and in the hand's own frame), the weapon's grip point sits from where "
           "the game's own hand-to-weapon offset would put it on the hand you actually SEE, "
           "plus how far that drawn hand sits from the wrist the rig solved. Melee should read "
           "about zero. Headset only. Off by default."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_rel_dev", DiagRelDev,
           "Diagnostics: does the weapon's grip offset leave the hand (climbs, heals)", "Camera hook",
           "Log-only, changes nothing. Every update, for each held item: how far the game's own "
           "hand-to-weapon offset (the one the weapon is placed with) has moved away from where it "
           "sits when you are standing still with that weapon, in degrees and millimetres. Prints "
           "one line per second and one line per excursion (when it starts, how long, how far). "
           "Measures with or without the per-weapon grip setting. Headset only. Off by default."),
#endif
    // LOCKED 2026-10-08 (Jay: "Yes, lock item_hide_by_state too") at the compiled true - never read from the file, never
    // drawn. Tier A (INV-490), and item_rel_hold=3 (locked) reads the climb / kick / slide state from its hook.
    S_BOOL_LOCKED("item_hide_by_state", ItemHideByState,
           "Item: hide the held weapon while climbing or healing, like the flat game", "Camera hook",
           "While you climb a ledge or heal, the flat game moves your weapon out of view (and hides it for most of a "
           "climb). In VR your hand stays with the controller, so that move made the weapon fly off your hand. With this "
           "on, the game's own hiding is extended: the weapon is hidden for the whole climb, and while it is put away for "
           "a heal and drawn again after. Ordinary weapon swaps are not affected. It comes back on its own. Swimming, "
           "ziplines and the grapple are left to the game. It also supplies the climb / kick / slide state the weapon "
           "hold (item_rel_hold) reads. On by default."),
    S_BOOL("diag_stale_write", DiagStaleWrite,
           "Diagnostics: arm writes while the player is being rebuilt", "Camera hook",
           "Log-only, changes nothing. When you sleep in a bed (or die) the game removes your "
           "character and builds a new one a moment later. This counts how many of the mod's "
           "arm writes happen while the old character is gone or has been replaced, and logs "
           "the moment it first sees one. Used to track down the sleeping crash. Needs the arm "
           "rig to be running (it is from spawn). Off by default."),
    S_BOOL("rig_player_live_guard", RigPlayerLiveGuard,
           "Rigging: stop writing when the game rebuilds your character", "Camera hook",
           "When you sleep in a bed or die, the game removes your character and builds a new "
           "one. With this on, the arm rig checks the game's own record of your character "
           "before every write: if the character it was attached to is gone, it writes nothing, "
           "lets go at once and re-attaches to the new one, instead of carrying on writing into "
           "memory the game has already freed (the sleeping crash) and taking half a minute to "
           "notice. If the game's record cannot be read it does nothing and the rig behaves as "
           "before. On by default - leave it on."),
    S_BOOL("diag_body_writers", DiagBodyWriters,
           "Diagnostics: what the game's own arm movers do", "Camera hook",
           "Log-only. Measures, every update, how much each of the game's after-animation "
           "arm movers changes your arm: the movement sway on the shoulders and hands, the "
           "45-degree upper-arm pitch, the look/aim turn of the collarbones, and the engine's "
           "pose crossfade. It also logs the order they run in against our own arm write, how "
           "far the drawn shoulder sits from its rest length, and how much the arm's body yaw "
           "changes per update. Adds three pass-through hooks (they always run the game's own "
           "code) only while on. Takes effect at launch. Headset only. Off by default."),
    S_BOOL("diag_bones_anim", DiagBonesAnim,
           "Diagnostics: the game's weapon-hold pose on your arms", "Camera hook",
           "Log-only. Measures, every update, how much the game's own weapon-hold animation "
           "layer changes your arms, hands, forearm twists and fingers, how strongly it is "
           "blended in for each hand, and whether it runs before or after our own arm write. "
           "Adds one pass-through hook (it always runs the game's own code) only while on. "
           "Takes effect at launch. Headset only. Off by default."),
    S_BOOL("anim_restore_owned_bones", AnimRestoreOwnedBones,
           "Anim: keep your arms when holding a gun", "Camera hook",
           "A/B test (INV-293). While you hold a gun, the game re-poses both arms, the "
           "forearm twists and the fingers into its own weapon-hold pose every update, on "
           "top of your tracked arms. This puts back the arm bones and pinned fingers we "
           "control right after the game does that, so your arms follow your controllers. "
           "The gun hand's fingers keep the game's grip. Does nothing with melee or empty "
           "hands. Takes effect at launch. Off by default. If the arms, hands or weapon "
           "misbehave, turn this off first."),
    S_BOOL("anim_skip_owned_bones", AnimSkipOwnedBones,
           "Anim: don't pose your arms into the gun hold", "Camera hook",
           "Fix test (INV-293). While you hold a gun, the game poses both arms, the forearm "
           "twists and the fingers into its own weapon-hold pose every update. This stops the "
           "game computing that pose for the arm bones and pinned fingers we control, so your "
           "arms stay on your controllers. The gun hand's fingers still take the game's grip. "
           "Does nothing with melee or empty hands. Takes effect at launch. Off by default. If "
           "the arms, hands or weapon misbehave, turn this off first."),
    S_BOOL("anim_decouple_arms", AnimDecoupleArms,
           "Anim: free both arms from the gun hold", "Camera hook",
           "Full decouple. While you hold a gun the game poses both arms into its two-handed "
           "hold - collarbones, arms, forearm twists, hands and the empty hand's fingers. "
           "This stops the game computing that pose for any arm bone, so both arms follow "
           "your controllers the way they do with melee. The gun hand's fingers still take "
           "the game's grip, and the gun's own placement is untouched. The free hand does not "
           "grip the gun. Takes effect at launch. Off by default. If the arms, hands or weapon "
           "misbehave, turn this off first."),
    S_BOOL("rig_pin_foretwist", RigPinForetwist,
           "Rig: hold the forearm twist bone still", "Camera hook",
           "Fix test (INV-294). The forearm's twist bone is not driven by the rig, so each "
           "gun's hold animation rolls it differently and the lower forearm skin twists the "
           "wrong way. This holds it at its rest pose, like the other twist bones. Needs the "
           "subtree pins on. Refuses if rig_twist_to_twistbone is on. Takes effect at launch. "
           "Off by default. If the forearms look wrong, turn this off first."),
    S_BOOL("anim_skip_hand_correction", AnimSkipHandCorrection,
           "Anim: don't move your arms onto the gun", "Camera hook",
           "Fix test (INV-295). While you hold a gun the game shifts each upper arm so the hand "
           "lands on the gun's own grip point, ramping in over about a second. This stops that "
           "for the player, so the arms stay on your controllers. Takes effect at launch. Off "
           "by default. If the arms or weapon misbehave, turn this off first."),
    S_BOOL("anim_skip_spine_pitch", AnimSkipSpinePitch,
           "Anim: don't tilt the upper spine with your head", "Camera hook",
           "Fix test (INV-310). The game tilts the upper spine by your look pitch after it has "
           "already placed it at your real head, so the camera swings further than your head "
           "when you nod. This stops that tilt for the player. The shoulders stop tilting with "
           "your head too. Takes effect at launch. Off by default. If the view, arms or weapon "
           "misbehave, turn this off first."),
    S_BOOL("rig_clavicle_hold", RigClavicleHold,
           "Rig: keep your shoulders put when holding a gun", "Camera hook",
           "Fix test (INV-296). Each gun's hold animation swings the collarbones to its own "
           "angle, which moves your shoulders and makes the arms look longer or shorter per "
           "gun. This keeps each collarbone at the pose it had just before you drew the gun "
           "(the melee / empty-handed pose) for as long as a gun is held. Refuses if another "
           "collarbone setting is on. Takes effect at launch. On by default. If the shoulders "
           "or arms misbehave, turn this off first."),
    S_BOOL("rig_clavicle_hold_bow", RigClavicleHoldBow,
           "Rig: keep your left shoulder put when holding a bow", "Camera hook",
           "Fix test (INV-544). The bow's draw animation swings the left collarbone, which moves your shoulder and makes the "
           "arm look longer between shoulder and elbow while you draw. With this on the setting above also holds the LEFT "
           "collarbone while a bow is in your left hand. The right arm is not touched. Needs the setting above. On by default."),
    S_BOOL("rig_throwable_body_hold", RigThrowableBodyHold,
           "Rig: keep your body still when you pull out a grenade or other throwable", "Camera hook",
           "Fix test. A throwable's own animation twists your spine, neck and head (and so your shoulders and view) when you pull "
           "it out, and shifts your view slightly. With this on, those bones stop following the animation while a throwable is out and go back to normal when "
           "you put it away. Your hand keeps the grenade's grip. On by default."),
    S_BOOL("rig_throwable_hold_pelvis", RigThrowableHoldPelvis,
           "Rig: also keep your hips still when you pull out a throwable", "Camera hook",
           "Fix test, with the setting above on. The throwable's animation also turns your hips a little (about 3 degrees), "
           "which moves your view slightly forward. With this on, the hips stop following the animation too while a throwable "
           "is out. Trade-off: your hips do not sway if you walk with a throwable out (your legs still move). A change applies "
           "from the next time you pull one out. On by default."),
    S_BOOL("rig_throwable_hold_lclavicle", RigThrowableHoldLClavicle,
           "Rig: also keep your left shoulder in place when you pull out a throwable", "Camera hook",
           "Fix test, with the first throwable setting above on. The throwable's animation moves your left shoulder a few "
           "centimetres when you pull one out, and it stays moved until you switch back to a weapon. With this on, the left "
           "shoulder keeps its position while a throwable is out. A change applies from the next time you pull one out. On by "
           "default."),
    S_BOOL("diag_clavicle_bow_ab", DiagClavicleBowAb,
           "Diagnostics: bow left-shoulder test (alternates the fix every 3 shots)", "Camera hook",
           "For one test run: with the setting above on and a bow out, the left-shoulder fix switches OFF for 3 shots, then ON "
           "for 3, and repeats. The left controller buzzes once for OFF and twice for ON. Needs the manual bow draw on. Off by "
           "default; leave it off outside that run."),
    S_BOOL("rig_clavicle_hold_throw", RigClavicleHoldThrow,
           "Rig: keep your right shoulder put during a melee throw", "Camera hook",
           "Fix test (INV-317). The melee throw's animation swings the right collarbone, which "
           "moves your shoulder and bends the elbow oddly. With this on, the setting above also "
           "holds the RIGHT collarbone at its melee pose while a throw is readied and for 1.5 s "
           "after you throw. Needs the setting above and the VR melee throw on. The left arm is "
           "not touched. Off by default."),
    S_BOOL("rig_clavicle_hold_live_t", RigClavicleHoldLiveT,
           "Rig: guns sit in the hand (collarbone position)", "Camera hook",
           "Fix test (INV-331). With a gun out, the collarbone hold above keeps the collarbone's "
           "angle but the game still moves its position a little per gun, so the arm you see sits "
           "1.5-2 cm off where the rig aimed it and the gun ends up beside the hand. With this on "
           "the rig aims from the collarbone where it really is. Needs the setting above. On by "
           "default."),
    S_BOOL("anim_suppress_aim_paths", AnimSuppressAimPaths,
           "Anim: stop the game re-aiming your collarbones", "Camera hook",
           "A/B test A (INV-286). The game turns your spine and collarbones toward where you "
           "aim, every update, after the animation. This switches off only the aim paths that "
           "include a collarbone, for the player only; the look path that turns the spine and "
           "eye camera keeps running. Takes effect at launch. On by default. If the body or "
           "camera misbehaves, turn this off first."),
    S_BOOL("anim_suppress_look_path", AnimSuppressLookPath,
           "Anim: stop the body leaning forward when you look down", "Camera hook",
           "Fix test (INV-345). The game's own VR look setting bends your spine forward by the "
           "whole of your downward look, so your chest and shoulders lean over when you look "
           "down. This switches off the game's look path (spine + eye camera) for the player "
           "only. Works live. On by default. If the camera or body misbehaves, turn this off."),
    S_FLOAT("cam_eye_forward", CamEyeForward, 0.0f, 1.5f,
            "Camera: move the view to the face's eyes", "Camera hook",
            "Fix test (SPEC-EYE-FORWARD). The game's camera sits about 6.5 cm behind the face's eyes, in the middle "
            "of the head, so looking down you can see inside the body. 1 = the view at the face's own eyes; 0 = the "
            "game's camera. Your hands move with the view so they stay on the controllers. Works live once it is "
            "above 0 at launch. 1.15 by default."),
    S_BOOL("cam_eye_arc", CamEyeArc,
           "Camera: eyes move forward when you look down", "Camera hook",
           "Fix test (SPEC-EYE-FORWARD). With the neck pivot on, the body no longer swings with your head, and this "
           "puts that swing back on the VIEW only: looking down carries your eyes forward and down over your chest, "
           "as a real head does. Does nothing while the neck pivot is off. Works live if this or the slider above "
           "is on at launch. On by default."),
    S_FLOAT("cam_eye_up_mm", CamEyeUpMm, 0.0f, 150.0f,
            "Camera: raise the view (mm)", "Camera hook",
            "Fix test (INV-347). Lifts the view straight up by this many millimetres, all the time, so that when "
            "you stand and look down the chest and shoulders are far enough below you not to be cut open. Your "
            "hands move with the view. The cost: you are this much taller everywhere. Works live once any of the "
            "eye settings is on at launch. 0 by default (the 5 cm near clip keeps the body solid without it)."),
    S_FLOAT("cam_eye_arc_drop", CamEyeArcDrop, 0.0f, 1.0f,
            "Camera: how far the view sinks when you look down", "Camera hook",
            "Fix test (INV-347). Only with 'eyes move forward when you look down' on. 1 = the view sinks as far as "
            "a real head does (about 15 cm at a steep look-down); lower values keep it higher, so the body stays "
            "clear of the view, without making you taller when you look level. Your hands stay on the "
            "controllers. Works live. 0.5 by default."),
    S_BOOL("anim_suppress_sway", AnimSuppressSway,
           "Anim: stop the game's movement sway on the arms", "Camera hook",
           "A/B test C, the control (INV-286). Skips the game's movement-driven arm sway "
           "(shoulder shift and hand turn from your speed and turning). The probe showed its "
           "shoulder shift never reaches the drawn arm, so no visible change is expected. "
           "Takes effect at launch. Off by default."),
    S_BOOL("anim_hold_upper", AnimHoldUpper,
           "Hold the upper body off the animation", "Camera hook",
           "INV-283. Clears the ROTATION write-enable (flags bit 9) of the animation's own "
           "binding entry for hspine, spine, spine1-3, hspine1, eyecamera, neck, neck1 and "
           "head, so those bones HOLD the rotation they had when this armed while legs, "
           "pelvis, arms and all translations keep animating. Not the gate (no pose "
           "shift), not dst, not Block. Arms only after the player's own animation pass "
           "is seen reading the map and no other character's is; restores exactly what it "
           "cleared when switched off. Arm it standing still. Needs diag_bone_blend_probe "
           "and diag_bone_dump. Works in flat. Off by default."),
    S_BOOL("anim_hold_clavicles", AnimHoldClavicles,
           "Hold the collarbones off the animation", "Camera hook",
           "Stops the walk and sprint animation raising and lowering your shoulders, which "
           "is what makes the arms bob up and down while you move (the collarbones' "
           "rotation stops following the animation; everything else keeps animating). "
           "It locks in the collarbone pose you have while standing still, so it waits "
           "until you stand still for half a second before it switches on, and says so in "
           "the log. Shoulder shrugs in climbing and attacks are held too while it is on. "
           "Needs diag_bone_blend_probe, diag_bone_dump and the headset (it reads your "
           "speed from the arm rig). Takes effect at launch. Off by default."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_world_writer", DiagWorldWriter,
           "Diagnostics: find what re-poses the arm", "Camera hook",
           "Arms a hardware data breakpoint on the player r_forearm's WORLD "
           "matrix in bounded 4-second windows and logs every distinct writing "
           "instruction next to the weapon actually held. The engine's own "
           "builder writes that address every frame regardless, so read it as a "
           "DIFFERENTIAL: the address that appears only in the FireWeaponVis and "
           "CrossbowVis windows is the one re-posing the arm. Needs "
           "diag_bone_blend_verbose for the weapon markers to be logged too. Off "
           "by default - arming suspends every thread, so it is not free."),
#endif
    // [LOADBEARING: ROOT-3] - corrected 2026-10-01 (Jay: "it's loadbearing and should be marked & treated as such").
    // The old text said "Leave this off for any timing measurement", which would have turned the ARM off.
    S_BOOL("diag_bone_blend_verbose", DiagBoneBlendVerbose,
           "Rigging MASTER ENABLE (3 of 3) - NOT a logging switch, never turn it off", "Camera hook",
           "LOADBEARING (ROOT-3), despite the diag_ prefix and the name. Its 2 s report pass is the ONLY "
           "path to ResolveAccessors(), so with it OFF the engine accessors stay NULL and the arm stops "
           "solving for the whole session - SILENTLY: every arming line, [SHOULDER] latched included, still "
           "prints. It is one of the rig master enables, owned by the Rigging tab's 'Enable rigging' switch. "
           "To quieten the rig's log, use diag_periodic_logs instead - that gates only the log lines."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_periodic_logs", DiagPeriodicLogs,
           "Diagnostics: the periodic report lines (rig 2 s report, 1 Hz per-side lines, timers)", "Periodic logs",
           "OFF = the timer-driven report lines are not written: the rig's 2 s report pass, "
           "[TWISTBONE]/[WRIST]/[CLAVPOSE]/[SHOULDERVIEW]/[SOLVEEXIT] and the other 1 Hz lines, "
           "the scePad heartbeat, camhook's reject / [table] / [look back], [SWING STATS], "
           "[RENDDECL]/[NECKPIVOT]/[YAWGATE] windows. With no probe on they were ~110 lines/s, "
           "and INV-249 measured ~0.16 ms per line. Only the log statements change - the rig, "
           "its report pass and ResolveAccessors run exactly as before. The rig liveness line "
           "('boneblend: hooks -'), the frame-pacing report, every install / ARMED / FIRST / "
           "latch line and every error stay. Turn ON for a run that reads any of those lines "
           "(docs/PROBE_DEPS.md). Live."),
    S_BOOL("diag_antiwall_probe", AntiWallProbe,
           "Diagnostics: anti-wall deformation and projectile callers", "Camera hook",
           "READ ONLY - writes nothing to the game. Reads the live anti-wall "
           "deformation weight off the player entity, the weapon category the "
           "deformation is gated on, ANTI_WALL_MODE itself, and which callers "
           "construct projectiles. This is the cheapest disproof available for "
           "the theory that the anti-wall deformation is what overrides our arm "
           "on firearms: if the weight reads zero with a firearm held, the theory "
           "is dead in one run. It also names the caller behind every projectile, "
           "which settles whether one hook at Bullet::Init would cover the bow - "
           "the weapon neither ComputeShootDir watch has ever seen. Independent "
           "of diag_bone_blend_verbose, but it does need diag_bone_blend_probe "
           "and diag_bone_dump on, because those are what capture the player "
           "entity it reads from."),
    S_BOOL("diag_camhook_verbose", DiagCamhookVerbose,
           "Diagnostics: camhook's concluded-investigation logging", "Camera hook",
           "Re-enables the look-back baseline, continuity, yaw-investigation and "
           "camera reject lines. These were unconditional and produced 18,000 of "
           "24,425 log lines in one run, for investigations that are finished. "
           "LogTo takes a lock across file I/O per line, so on a per-camera-per-"
           "frame path that is a real cost and a moving variable in any timing "
           "measurement. Turn it on if a camera problem needs the reject reasons."),
#endif
    S_BOOL("diag_bone_dump", DiagBoneDump,
           "Diagnostic: dump the player's skeleton (rigging stage 0)", "Camera hook",
           "READ-ONLY. With a weapon equipped, captures the player's model at "
           "CorrectItemPosition and logs every skeleton element's index, name, "
           "PARENT and local-space offset, plus the EBones-id -> element map. "
           "This is the bone map the arm-rigging work depends on. Uses the "
           "single exec-watch slot, so turn every other diag_* exec watch OFF "
           "(diag_visibility_probe, diag_player_movemod, diag_watch_line_add, "
           "diag_kill_physics_debug) or they corrupt each other. One-shot: "
           "disarms itself once it has a usable dump. See CLAUDE.md."),
    S_BOOL("diag_rig_stage4", DiagRigStage4,
           "Diagnostic: rigging stage 4 - block + write (engine idiom)", "Camera hook",
           "WRITES A DELIBERATELY WRONG POSE. The engine's own sequence, end to "
           "end: clear IControlObject flag 4, HackPABlender_BlockAnimation the "
           "right-arm bones (57/58/62), write the pose, notify via AnimBind "
           "vtable +0x38/+0x160. First stage where the write can both hold AND "
           "render. Needs a weapon drawn OUTSIDE the safe zone at least once - "
           "the model pointer comes from CorrectItemPosition, which only runs "
           "for a held item. See CLAUDE.md."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_rig_stage3", DiagRigStage3,
           "Diagnostic: rigging stage 3 - bone-array numbering", "Camera hook",
           "READ-ONLY. Dumps the bone array at model+0x388 (stride 0x78) and the "
           "bone-index -> mesh-element lookup at model+0x408, cross-referenced "
           "against the stage-0 element names. Resolves the THIRD numbering: "
           "HackPABlender's block mask is indexed by raw bone-array index "
           "(0-57), not mesh-element index (0-92), and that mapping is the hard "
           "prerequisite for driving the skeleton. Implies the stage-0 capture, "
           "so it uses the single exec-watch slot. See CLAUDE.md."),
#endif
    S_BOOL("rig_stage2_use_world", RigStage2UseWorld,
           "Stage 2: write WORLD matrices instead of local", "Camera hook",
           "Narrow test. Evidence says SetElementLocalMatrix writes a "
           "representation the skinning path does not read - the data "
           "persisted, the animbind notify landed, and the arm still animated "
           "normally - while still dirtying the cache enough to cause per-eye "
           "rivalry on the hand and weapon. CorrectItemPosition drives this "
           "same skeleton through SetElementWorldMatrix every frame, so the "
           "world path is engine-proven. If the arm visibly bends with this "
           "on, the skeleton IS drivable and only timing remains."),
    S_BOOL("rig_notify_animbind", RigNotifyAnimBind,
           "Notify the blend system after a bone write", "Camera hook",
           "Calls IModelObject::GetAnimBind() then the virtual at "
           "IAnimBind+0x248 after every raw bone-matrix write - the idiom every "
           "engine caller of SetMeshElementsMatrices follows. Leaving this off "
           "is poking bone data behind the animation system's back, which is "
           "the leading explanation for the body/weapon desync seen in rigging "
           "stages 1-2. Turn OFF only to reproduce that deliberately."),
    S_FLOAT("rig_notify_weight", RigNotifyWeight, 0.0f, 1.0f,
            "Blend notify weight", "Camera hook",
            "The float passed to IAnimBind+0x248. The engine passes a global "
            "whose value was not resolved, so 1.0 is a starting assumption - "
            "if the notify works but the pose blends oddly, try lower."),
    S_BOOL("diag_rig_stage2", DiagRigStage2,
           "Diagnostic: rigging stage 2 - static pose write", "Camera hook",
           "WRITES A DELIBERATELY WRONG POSE. Pins r_forearm to a fixed "
           "rotation from the engine's own render thread, to find out whether "
           "a bone write survives the animation tick. The arm SHOULD look "
           "wrong - that is the signal. If the forearm freezes in a bad pose "
           "the write holds; if it animates normally the tick overwrites us "
           "and we need the per-bone block mask. Log reports held vs "
           "clobbered. Implies the stage-0 capture. See CLAUDE.md."),
    S_FLOAT("rig_stage2_angle_deg", RigStage2AngleDeg, 0.0f, 180.0f,
            "Stage 2 test rotation", "Camera hook",
            "How far to rotate r_forearm about its own local Z for the stage-2 "
            "write test. 60 is large enough to be unmistakable without looking "
            "like a crash."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_rig_stage1", DiagRigStage1,
           "Diagnostic: rigging stage 1 - null round-trip", "Camera hook",
           "READ-MOSTLY. Reads each right-arm bone's local matrix, writes it "
           "back UNCHANGED, reads again and compares. Answers whether the data "
           "is live/animating, whether our mtx34 layout and calling convention "
           "round-trip correctly, and whether a write survives to the next "
           "pass. Writes identical bytes so it cannot corrupt the pose. Implies "
           "the stage-0 capture, so it uses the single exec-watch slot - turn "
           "other diag_* exec watches OFF. Logs at 2 Hz for 60s. See CLAUDE.md."),
#endif
    S_BOOL("lod_override", LodOverride,
           "Push LOD / visibility distances out", "Camera hook",
           "Calls the engine's own exported ILevel setters about once a "
           "second, so these sliders take effect live and survive level "
           "loads. The engine's 2015 LOD tuning is tolerable glanced at on a "
           "monitor and not when you are standing in it. See CLAUDE.md."),
    S_FLOAT("lod_objects_range", LodObjectsRange, 500.0f, 20000.0f,
            "Objects visibility range", "Camera hook",
            "ILevel::SetObjectsVisibilityRange. Reads 1700 by default (read "
            "live from the engine). This is the knob for NPCs and objects not "
            "rendering at all until approached - raise it until the distant "
            "NPC appears."),
    S_FLOAT("lod_mesh_cull_size", LodMeshCullSize, -1.0f, 1.0f,
            "Mesh cull size", "Camera hook",
            "ILevel::SetMeshCullSize. Screen-relative size below which a mesh "
            "is culled, so LOWER keeps more small distant clutter. -1 leaves "
            "it alone; no getter exists, so its stock value is unknown - "
            "move it in small steps."),
    S_FLOAT("lod_mesh_cull_dist", LodMeshCullDist, -1.0f, 20000.0f,
            "Mesh cull distance threshold", "Camera hook",
            "ILevel::SetMeshCullDistTreshold (the engine's own spelling). "
            "-1 leaves it alone."),
    S_FLOAT("lod_fx_range", LodFxRange, -1.0f, 20000.0f,
            "FX visibility range", "Camera hook",
            "ILevel::SetFXVisibilityRange - particles and effects. -1 leaves "
            "it alone."),
    S_BOOL("diag_no_rast_occlusion", DiagNoRastOcclusion,
           "Diagnostic: occlusion culling OFF", "Camera hook",
           "A/B for far buildings vanishing in the CENTRE of the view. Sets "
           "the scene view's own ForceViewNoRastOcclusion flag with the "
           "engine's exported setter, so the CPU occlusion pass no longer "
           "hides anything. Takes effect live (within 0.25 s) - toggle it while "
           "looking at a building that has vanished. Draws more, so fps drops "
           "while on. Diagnostic only, ships OFF. Logs [RASTOCC]."),
    S_BOOL("diag_clip_far", DiagClipFar,
           "Diagnostic: log the scene camera's clip far", "Camera hook",
           "Read-only. Once a second logs the scene camera's clip near/far "
           "(the engine's own GetClipFar), the cull copy's, and how far a flat "
           "far plane reaches dead centre vs the edge of the view. For far "
           "buildings vanishing in the CENTRE of the view (INV-368). Logs "
           "[CLIPFAR]. Ships OFF."),
    S_FLOAT("clip_far_scale", ClipFarScale, 1.0f, 20.0f,
            "Clip far multiplier (A/B)", "Camera hook",
            "1.0 = untouched. Above 1, multiplies the scene camera's clip far "
            "with the engine's own IBaseCamera::SetClipFar, live. Drag it up "
            "while looking at a far building that has vanished: if it comes "
            "back, the far clip was hiding it. Draws more far geometry, so "
            "fps can drop. Back to 1.0 restores the game's value."),
    S_FLOAT("vr_near_clip_cm", VrNearClipCm, 2.0f, 12.0f,
            "Camera: near clip in VR (cm)", "Camera hook",
            "How close to your eyes the game still draws things. The game uses 9.76 cm in VR (5 cm on a flat screen), "
            "so your own chest and shoulders get cut away when you look down - the reason the view had to be moved "
            "forward and up. Lower it (5 = the flat game's value) and the body stays solid closer to your eyes. "
            "9.76 = the game's value, nothing changed. 5 by default. Works live."),
    // D-184 (2026-10-08): every AFW fix below is LOCKED on (hidden, file value ignored); afw_alt_eye and afw_rebuild
    // FOLLOW afw_enable (VR Comfort). The diagnostics and the tint stay ordinary settings.
    S_BOOL_LOCKED("afw_held_eye_pose", AfwHeldEyePose,
           "AFW test: correct the held eye for head turns", "Camera hook",
           "Only acts while 'AFW test: render one eye per frame' is on (and "
           "declare_rendered_orientation). Tells the headset the held eye's "
           "image is one frame old, so it rotates it to your current head. "
           "Fixes head-turn ghosting between the eyes; does NOT fix stick "
           "turns, walking or moving hands (that needs the warp). Live. "
           "Logs [AFWPOSE]."),
    S_BOOL_FOLLOWS("afw_rebuild", AfwRebuild, AfwEnable,
           "AFW test: rebuild the held eye (world only)", "Camera hook",
           "EXPERIMENT. Only acts while 'AFW test: render one eye per frame' is on. Meant to work with "
           "every DLSS / FSR mode and quality. Each frame the eye that was not rendered is redrawn from its own last "
           "image and depth, moved to where the camera is now, so the world should stop doubling on "
           "stick turns and walking. Your hands, weapon and the HUD are NOT handled yet and will smear "
           "in that eye. Falls back to the old behaviour on any frame it cannot do. Live. Logs [AFWRB]."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_afw_rebuild_check", DiagAfwRebuildCheck,
           "Diagnostics: [AFWRB] CHECK is the rebuilt world right", "Camera hook",
           "Only acts with the rebuild on. A few times a second it predicts the eye that WAS rendered "
           "from older images, the same way the rebuild makes the other eye, and logs how far the "
           "prediction is from the real picture, next to how far the unmoved old picture is. Costs "
           "some frame time while on. Log-only. Live. Logs [AFWRB] CHECK in dlvr_bootstrap.log."),
#endif
    S_BOOL("afw_rebuild_tint", AfwRebuildTint,
           "AFW test: tint the rebuilt eye by rule (debug)", "Camera hook",
           "Only acts with the rebuild above. Colours each pixel of the rebuilt eye by where it came "
           "from: no tint = that eye's own last image, blue = taken from the other eye, green = taken "
           "from the other eye because something there had moved, orange = an edge that was re-tried, "
           "red = fallback, cyan = near miss, magenta = filled in. Live."),
    S_BOOL_LOCKED("afw_rb_far_rule", AfwRbFarRule,
           "AFW rebuild: far rule (sky)", "Camera hook",
           "Only acts with the rebuild above. Anything farther than 250 m counts as equally far when "
           "the rebuild decides which eye's picture to use, so the sky should stop flashing blue (with "
           "the tint on) while turning. Live."),
    S_BOOL_LOCKED("afw_rb_stale", AfwRbStale,
           "AFW rebuild: moving objects (stale test)", "Camera hook",
           "Only acts with the rebuild above. Where the freshly rendered eye can see past something "
           "the held eye still shows, that thing has moved, and the fresh eye's picture is used there "
           "instead - zombies and other moving things should stop trailing. Your hands and weapon are "
           "treated like any other object for now. Shows green with the tint on. Live."),
    S_BOOL_LOCKED("afw_rb_edge_rescue", AfwRbEdgeRescue,
           "AFW rebuild: re-try edges", "Camera hook",
           "Only acts with the rebuild above. Where the rebuild lands on the wrong side of an object's "
           "edge it tries again from the neighbouring point. Shows orange with the tint on. Live."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_afw_rebuild_rules", DiagAfwRebuildRules,
           "Diagnostics: [AFWRB] RULES share of the eye per rule", "Camera hook",
           "Only acts with the rebuild on. A few times a second it counts how much of the rebuilt eye "
           "each rule drew (own image, other eye, moved object, re-tried edge, fill), split by whether "
           "the camera was still or moving. "
           "Log-only. Live. Logs [AFWRB] RULES in dlvr_bootstrap.log every 5 s."),
    S_BOOL("diag_afw_fg_motion", DiagAfwFgMotion,
           "Diagnostics: [AFWMOT] do the hands carry motion vectors", "Camera hook",
           "AFW hands / weapon step 2b. Only acts with the rebuild, the hands / weapon mask and the "
           "rule counter above all on. Counts whether the game's own motion vectors move with your "
           "hands and weapon, and whether following them back one frame lands on the hand in the "
           "other eye's image. Changes nothing you can see. "
           "Log-only. Live. Logs [AFWMOT] in dlvr_bootstrap.log every 5 s."),
#endif
    // diag_afw_heldmap: OUTSIDE the DLVR_BETA_LEAN block since 2026-10-29 (it gates the [AFWVEH] buggy probe - depth-wanted bit 6;
    // compiled out, the file's 1 was never read: 0 hits for the key in BETA 402ad729).
    S_BOOL("diag_afw_heldmap", DiagAfwHeldMap,
           "Diagnostics: [AFWHM] redraw the hands as the other eye saw them", "Camera hook",
           "AFW hands / weapon step 2b-1. Only acts with the rebuild, the hands / weapon mask and the "
           "rule counter above all on. Draws your body once "
           "more, off screen, in the pose and from the viewpoint of the other eye's older image, and "
           "counts how exactly that matches what that image showed. Changes nothing you can see. "
           "Log-only. Live. Logs [AFWHM] in dlvr_bootstrap.log every 5 s."),
    S_BOOL_LOCKED("afw_rb_fg_move", AfwRbFgMove,
           "AFW rebuild: hands follow the held pose (step 2b)", "Camera hook",
           "Verified 2026-10-06. Needs the rebuild and the hands / weapon mask on. In the rebuilt eye your hands and body are moved to where they are NOW, using the "
           "game's own record of the pose that eye last drew, instead of staying where they were. "
           "Should stop the hands doubling and trailing in one eye. The weapon is moved only with the next switch. Live."),
    S_BOOL_LOCKED("afw_rb_fg_move_weapon", AfwRbFgMoveWeapon,
           "AFW rebuild: weapon follows the held pose too", "Camera hook",
           "Verified 2026-10-06. Needs the rebuild, the hands / weapon mask and 'hands follow the held pose' on. In the rebuilt "
           "eye the weapon in your hand is moved to where it is NOW, the same way the hands are, instead of staying where "
           "it was. Should stop the weapon trailing / doubling in one eye when you swing it or turn. Live."),
    S_BOOL_LOCKED("afw_rb_fg_edge_ring", AfwRbFgEdgeRing,
           "AFW rebuild: no old outline beside moving hands", "Camera hook",
           "Verified 2026-10-06. Needs the rebuild, the hands / weapon mask and 'hands follow the held pose' on. Where your hands "
           "have moved (stick turns, fast hands), the thin edge around where the hand WAS is taken from the other eye or "
           "filled, instead of being left behind as a faint outline. No effect while the hands are still. Live."),
    S_BOOL_LOCKED("afw_rb_fill_near", AfwRbFillNear,
           "AFW rebuild: fill gaps from the background next to them", "Camera hook",
           "Verified 2026-10-06. Needs the rebuild on. Where neither eye can see what is behind something that moved (beside your "
           "hands on stick turns), the gap is filled from the background right next to it, on the side away from the hand, "
           "instead of from further away - should stop streaks / smears there. Live."),
    S_BOOL_LOCKED("afw_rb_seed_indexed", AfwRbSeedIndexed,
           "AFW rebuild: faster seed mesh (A1a)", "Camera hook",
           "Verified 2026-10-16. Needs the rebuild on. Draws the rebuild's two seed meshes from an index buffer so each "
           "grid point is shaded about once instead of six times. The picture is IDENTICAL; the rebuild costs ~0.2 ms less GPU "
           "per frame. Builds a 44 MB buffer once."),
    S_BOOL_LOCKED("afw_rb_fg_mask_engine", AfwRbFgMaskEngine,
           "AFW rebuild: see-through weapon parts (sight glass)", "Camera hook",
           "Verified 2026-10-08. Needs the rebuild, the hands / weapon mask and 'hands follow the held pose' on. Treats "
           "see-through parts of your weapon (a gun sight's glass) as see-through when the rebuilt eye is made, so the "
           "barrel seen through the sight should no longer break up or vanish while you aim and turn. Logs [AFWFG] MASK "
           "ENGINE and [AFWHM] GLASS lines. Live."),
    S_FLOAT_LOCKED("afw_mv_object_scale", AfwMvObjectScale, 1.0f, 2.5f,
           "AFW test: NPC motion scale for DLSS / FSR", "Camera hook",
           "Verified 2026-10-06 at 2.0. With AFW on, each eye's DLSS / FSR history is two game frames old, but the game's own "
           "motion for moving things (NPCs, zombies) covers one. This multiplies that motion. 1.0 = unchanged; "
           "2.0 = spans both frames. Should stop NPC faces / clothes swimming with AFW on. No effect with AFW off. Live."),
    S_BOOL_LOCKED("afw_rb_edge_hold", AfwRbEdgeHold,
           "AFW test: steady outlines (edge hold)", "Camera hook",
           "Verified 2026-10-06. Needs the rebuild on. At outlines the rebuilt eye sometimes borrows the other eye's picture "
           "for a pixel and sometimes not, changing every frame - that is the edge shimmer. This keeps the rebuilt eye's "
           "own pixel wherever its own image already has the nearer surface right next to it. Live."),
    S_INT_LOCKED("afw_rb_edge_hold_radius", AfwRbEdgeHoldRadius, 1, 3,
           "AFW test: edge hold search distance", "Camera hook",
           "How far the edge hold looks for the nearer surface in the rebuilt eye's own image: 1 = right next to it, "
           "2 or 3 = further (2 = the tuned default). Higher catches more of the shimmer but may keep the old picture where "
           "something really moved. Only acts with 'steady outlines (edge hold)' on. Live."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_afw_trail_src", DiagAfwTrailSrc,
           "Diagnostics: [AFWTRAIL] where the hand trail comes from", "Camera hook",
           "Only acts with the rebuild, the hands / weapon mask and the rule counter on (and the motion "
           "counter off). Counts whether the pixels drawn around moving hands from the other eye or by "
           "the fill come from right at the hand's edge. Changes nothing you can see. "
           "Log-only. Live. Logs [AFWTRAIL] in dlvr_bootstrap.log every 5 s."),
#endif
    S_BOOL("diag_hiccup", DiagHiccup,
           "Diagnostics: [HICCUP] head-turn hiccup log", "Camera hook",
           "Logs every frame where the headset might show a hiccup or a brief double image: the "
           "runtime holding a frame, a gap between game frames, a skipped display slot, or a jump in "
           "the declared head angle - with how fast the head was turning. Changes nothing you can "
           "see. Log-only. Live."),
    S_BOOL("submit_drawn_image", SubmitDrawnImage,
           "VR fix: show each frame's own image", "Camera hook",
           "Hands the headset the image each frame actually drew, instead of the image from two "
           "frames earlier (the game prepares its next frame before it finishes the current one). "
           "Removes head-turn hiccups and Alternate Frame Warp doubling. Affects all VR. On by "
           "default. Live."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_afw_capture", DiagAfwCapture,
           "Diagnostics: [AFWCAP] capture 4 rebuilt frames", "Camera hook",
           "Only acts with the rebuild on. Tick it, wait 5 seconds, and it saves 4 frames in a row "
           "(both eyes' images, the rebuilt eye, its tint and the hand masks) as pictures in the game "
           "folder's dlvr_capture. Each saved frame causes a short hitch. Untick and tick again for "
           "another set. Changes nothing you see otherwise. Live."),
    S_BOOL("diag_afw_fg_probe", DiagAfwFgProbe,
           "Diagnostics: [AFWFG] which draws are the arms (hide test)", "Camera hook",
           "AFW hands / weapon step 0. NOT log-only: while on, your hands, torso, legs and watch each "
           "disappear for 2 seconds in turn, so the log can tell which draw calls belong to them. Also "
           "counts whether the draw hooks hold and whether the game uses one projection for everything. "
           "Needs the rig running. Costs frame time while on. Live. Logs [AFWFG] in dlvr_bootstrap.log "
           "every 30 s and in the layer log."),
#endif
    S_BOOL_LOCKED("afw_rb_fg_mask", AfwRbFgMask,
           "AFW rebuild: hands / weapon mask", "Camera hook",
           "Marks which pixels are your own body and held weapon, by re-drawing those meshes into a "
           "mask as the game draws them. REQUIRED by the hand and weapon moves (they read it) - leave "
           "it on. With the rebuild tint on, your hands, body and weapon show yellow in the rebuilt eye. "
           "Needs the rig running. Live. Ships ON (2026-10-07)."),
    // (afw_rb_fg_rules: REMOVED 2026-10-08, Jay "Remove it" - failed in the headset, D-185.)
    S_BOOL_FOLLOWS("afw_alt_eye", AfwAltEye, AfwEnable,
           "AFW test: render one eye per frame", "Camera hook",
           "Test lever for alternate-eye rendering (AFW). While on, each frame "
           "renders the 3D scene for only ONE eye, alternating; the other eye "
           "keeps its previous image (no warp yet), so that eye will lag and "
           "swim on head turns - expected. DLSS runs without history while on. "
           "Live: tick to compare frame rate, untick to restore both eyes. "
           "VR only: in flatscreen it holds off and no eye is skipped. "
           "Logs [AFWALT]. Ships OFF."),
    S_FLOAT("stream_priority_distance", StreamPriorityDistanceM, 50.0f, 3000.0f,
            "Texture streaming priority distance (m)", "Camera hook",
            "r_device.scr ships 100 metres: past that, EVERYTHING drops to "
            "lowest streaming priority. /notexturestreaming was the only "
            "change that ever made the distant NPC render, which points here. "
            "Applied via SRendererAPI slot 143 case 6 and verified by reading "
            "rd3d11+0x784b4 back. Raise until distant assets stream in."),
    S_INT("stream_budget_mb", StreamBudgetMB, -1, 16384,
          "Texture streaming budget (MB)", "Camera hook",
          "The engine caps its texture-streaming budget at 2000 MB whatever the GPU has (INV-580). "
          "-1 = AUTO (default): sized from free VRAM every 5 s (DXGI, any GPU), never below the engine's own value. "
          "0 = leave the engine's own value. Any other value = that many MB. Live; kept across a graphics-settings change."),
    S_INT("stream_budget_reserve_mb", StreamBudgetReserveMB, -1, 16384,
          "AUTO streaming budget: VRAM to leave free (MB)", "Camera hook",
          "Only with the budget at -1 (AUTO). Video memory left free beyond what the game already uses, "
          "for buffers created later (DLSS, AFW), the Virtual Desktop encoder and other programs. "
          "-1 = AUTO (default): 20% of the card's video memory. Any other value = that many MB."),
    S_INT("texture_aniso", TextureAniso, 0, 16,
          "Texture sharpness: anisotropic level (restart)", "Camera hook",
          "Keeps floors and other surfaces at a shallow angle sharp further away. 16 = default. 2-16 = that level, on the game's "
          "world textures including normal maps. 0 = the game's own filtering (at most 4x, none on normal maps). Takes effect after "
          "a restart: the game creates its samplers at load."),
    // Map fixes, ported from DEV 2026-11-07 (rows as DEV's; DEV's road_drop_mm + road_project are dropped trials and stay out).
    S_BOOL("road_drape", RoadDrape,
           "Road trial: roads follow the ground (load a save to apply)", "Camera hook",
           "TRIAL. Rebuilds the farm's roads in the Wasteland on the ground's own shape, so they follow it instead of sitting above it. "
           "Applies when a level loads: change it, then load a save. Walking is not changed."),
    S_BOOL("edge_strip", EdgeStrip,
           "Road trial: asphalt edging slopes into the ground (restart)", "Camera hook",
           "TRIAL. The thin edging strip along the Wasteland's asphalt slopes down into the ground instead of hovering where the road stands above it. "
           "The asphalt itself does not move. Takes effect after a restart. Walking is not changed."),
    S_BOOL("terrain_berm", TerrainBerm,
           "Road trial: ground rises to meet the asphalt (restart)", "Camera hook",
           "TRIAL. At the Wasteland farm, the ground beside and under the asphalt is raised to just below it, so the road stops floating. Only what you see changes: "
           "beside the road your feet can sit slightly under the ground. Takes effect after a restart."),
    S_BOOL("decal_seat", DecalSeat,
           "Decal trial: decals sit on their surface (load a save to apply)", "Camera hook",
           "TRIAL. Graffiti, trash, blood and other decals that float a few millimetres in front of the wall or floor they are on are moved onto it. "
           "Only decals with a clear surface behind them move; the rest stay as the game ships them. Applies when a map loads: change it, then load a save."),
    S_INT("decal_seat_gap_um", DecalSeatGapUm, 0, 5000,
          "Decal trial: gap left in front of the surface (micrometres)", "Camera hook",
          "How far in front of its surface a seated decal ends. 500 = 0.5 mm, what the game's own signs use. Smaller can flicker at distance. Applies at map load."),
    S_INT("decal_seat_max_mm", DecalSeatMaxMm, 0, 250,
          "Decal trial: largest move (mm)", "Camera hook",
          "Decals further than this from their surface are left as the game ships them. Applies at map load."),
    S_INT("shadow_cascade_stagger", ShadowCascadeStagger, 0, 4,
          "Sun shadow cascade stagger", "Camera hook",
          "0 = stock: all 4 sun shadow cascades re-rendered every frame (~2 ms at 4096). 2 = the far cascades take "
          "turns, one per frame (cascades below the first-staggered index still every frame). Each skipped cascade keeps "
          "its own texture and matrix, so static shadows do not change; far moving shadows lag a frame. Refuses with "
          "NVIDIA PCSS on. Installs on first use; set it before loading a level."),
    S_INT("shadow_stagger_first", ShadowStaggerFirst, 1, 3,
          "Shadow stagger: first staggered cascade", "Camera hook",
          "Cascades below this index render every frame. 1 (default) = only cascade 0 every frame; 2 = cascades 0 and 1."),
    S_BOOL("lens_mask", LensMask,
           "Lens mask (skip pixels the headset cannot show)", "Camera hook",
           "VR only. Each eye's pixels outside what the headset can display (its own field of view plus the margin "
           "below) are marked in the depth / stencil buffer before the scene, so the engine's depth- and stencil-tested "
           "passes skip them. Any headset, any runtime, any AA mode. Live."),
    S_FLOAT("lens_mask_margin_deg", LensMaskMarginDeg, 0.0f, 15.0f,
            "Lens mask: safety margin (deg)", "Camera hook",
            "Degrees kept unmasked beyond the headset's field of view on every side, for the runtime's reprojection to "
            "your newest head position. Too small shows black or red (with the tint) at the edges on fast turns."),
    S_BOOL("lens_mask_tint", LensMaskTint,
           "Lens mask: paint the masked area red", "Camera hook",
           "Visual check. With the lens mask on, the masked pixels are painted bright red: you should never see red. "
           "Red at an edge on fast turns = raise the margin. Red on the inner (nose) side = the eyes are mixed up."),
    S_BOOL("dlss_lens_crop", DlssLensCrop,
           "Lens crop (DLSS / FSR skip pixels the headset cannot show)", "Camera hook",
           "VR only, live, every AA mode. The top and bottom of each eye that the headset never displays are not "
           "reconstructed by DLSS or FSR; a cheap fill keeps the image whole there. Does not need the lens mask."),
    S_FLOAT("dlss_lens_crop_margin_deg", DlssLensCropMarginDeg, 0.0f, 15.0f,
            "Lens crop: safety margin (deg)", "Camera hook",
            "Degrees kept above and below the headset's field of view, for the runtime's turn to your newest head "
            "position. Too small shows a soft or red (with the tint) band at the top or bottom on fast nods."),
    S_BOOL("dlss_lens_crop_tint", DlssLensCropTint,
           "Lens crop: paint the cropped bands red", "Camera hook",
           "Visual check. With the lens crop on, the cropped rows are painted bright red: you should never see red. "
           "Red at the top or bottom on fast nods = raise the margin."),
    // FOVEATED DLSS / FSR (plan docs/superpowers/plans/2026-11-11-foveated-dlss.md Task 2): Cheeky's controls + defaults
    // (CheekyFoveatedDLSS USAGE.md) and our horizontal offset. Live; the bootstrap reads them through dlvr_FovGeom.
    S_BOOL("fov_dlss", FovDlss,
           "Foveated DLSS / FSR (CheekyFoveatedDLSS)", "Camera hook",
           "VR only, live, DLSS and FSR 3. Full-quality reconstruction straight ahead of each eye, a cheaper pass "
           "outside it, blended at the edge. Based on CheekyFoveatedDLSS by ClarkCheekyKent. The lens crop is "
           "ignored while this is on."),
    S_FLOAT("fov_width", FovWidth, 0.05f, 1.0f,
            "Fovea width", "Camera hook",
            "Sets the normalized size of the DLSS-processed region (fraction of the eye's width)."),
    S_FLOAT("fov_height", FovHeight, 0.05f, 1.0f,
            "Fovea height", "Camera hook",
            "Sets the normalized size of the DLSS-processed region (fraction of the eye's height)."),
    S_FLOAT("fov_height_offset", FovHeightOffset, -1.0f, 1.0f,
            "Height offset", "Camera hook",
            "Moves the fovea up (negative) or down (positive), both eyes alike. Zero = straight ahead."),
    S_FLOAT("fov_x_offset", FovXOffset, -1.0f, 1.0f,
            "Horizontal offset", "Camera hook",
            "Ours, not Cheeky's. Moves the fovea toward the temple (positive) or the nose (negative), both eyes "
            "mirrored. Zero = straight ahead."),
    S_FLOAT("fov_roundness", FovRoundness, 0.0f, 1.0f,
            "Roundness", "Camera hook",
            "Blends the region shape from rectangular (0) to elliptical (1). This does not affect performance."),
    S_FLOAT("fov_transition", FovTransition, 0.0f, 0.5f,
            "Transition width", "Camera hook",
            "Feathers the edge of the region."),
    S_FLOAT("fov_supersample", FovSupersample, 1.0f, 2.0f,
            "Center supersampling", "Camera hook",
            "Scales the center DLSS output by 1-2x per dimension, then area-downsamples to its original size. "
            "Costs GPU time and memory."),
    S_BOOL("fov_periphery", FovPeriphery,
           "Peripheral DLSS", "Camera hook",
           "Enables the auxiliary DLSS pass for the area outside the fovea. Off = a plain stretch outside the "
           "fovea: a debug comparison only."),
    S_INT("fov_periphery_preset", FovPeripheryPreset, 5, 13,
          "Peripheral preset (5 = E, 11 = K, 12 = L, 13 = M)", "Camera hook",
          "Selects E, K, L, or M for the peripheral DLSS pass. E = Fastest (the default). Other numbers are sent "
          "as E."),
    S_FLOAT("fov_periphery_scale", FovPeripheryScale, 0.2f, 1.0f,
            "Periphery scale", "Camera hook",
            "Sets peripheral processing to 20-100 percent of the DLSS input width and height; 0.75 is the default."),
    S_BOOL("fov_border", FovBorder,
           "Show 5 px red alignment border", "Camera hook",
           "Displays the processed region while calibrating the fovea."),
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out (docs/BETA_CENSUS.md)
    S_BOOL("diag_stream_budget", DiagStreamBudget,
           "Diagnostic: log texture streaming used vs budget", "Camera hook",
           "LOG-ONLY. Every 30 s: the engine's streaming budget, how much of it is used, how often that "
           "changed (stream-in / evict churn) and DXGI's free video memory. Logs [STREAMBUDGET]."),
    S_BOOL("diag_visibility_probe", DiagVisibilityProbe,
           "Diagnostic: read the engine's object visibility range", "Camera hook",
           "READ-ONLY. Calls the engine's own exported "
           "ILevel::GetObjectsVisibilityRange() and logs the value. Run once "
           "in VR and once flatscreen on the same save: differing values mean "
           "a real VR-conditional draw-distance override exists; identical "
           "values mean the divergence is in how the value is applied (a "
           "screen-relative cull size against our wider frustum). Mutates "
           "nothing engine-side. See CLAUDE.md."),
#endif
    S_ENUM("cam_head_pitch_axis", CamHookHeadPitchAxis, "Head pitch axis",
           "Camera hook", "Which of the camera's local axes the engine pitches "
           "about.", kAxisXYZ),
    S_BOOL("cam_head_pitch_flip", CamHookHeadPitchFlip, "Flip head pitch",
           "Camera hook", "Look up and the view goes down: flip this."),
    S_ENUM("cam_head_roll_axis", CamHookHeadRollAxis, "Head roll axis",
           "Camera hook", "Which local axis is roll.", kAxisXYZ),
    S_BOOL("cam_head_roll_flip", CamHookHeadRollFlip, "Flip head roll",
           "Camera hook", "Tilt your head and the world tilts the wrong way."),
    // LOCKED ON 2026-10-07 (Jay: "Yes" - lock declare_rendered_orientation's prerequisites; the declaration refuses by name at 0).
    S_BOOL_LOCKED("head_yaw_decouple", HeadYawDecouple, "Head yaw decouple (head is a view)",
           "Camera hook",
           ">>> R10, docs/SPEC-HEAD-YAW-DECOUPLE-2026-09-26.md. Turning your head no longer turns "
           "the character: the engine is handed a yaw-free head, so the body, movement and the "
           "game's own aim turn only with the stick, and the real head yaw is put back on the "
           "rendered view. READ ONCE AT STARTUP - changing it here does nothing until a restart "
           "(the arm solve calibrates a constant against it). Refused, and logged, while "
           "use_render_pose is on. DEFAULT OFF."),
    S_BOOL("head_yaw_decouple_flip", HeadYawDecoupleFlip, "Flip decoupled head yaw",
           "Camera hook",
           "Only with head yaw decouple on: turn your head left and the view turns RIGHT (or the "
           "world swims twice as fast) - flip this. Read once at startup."),
    S_BOOL("head_neck_pivot", HeadNeckPivot, "Neck pivot (head turns don't slide the body)",
           "Camera hook",
           ">>> docs/SPEC-HEAD-NECK-PIVOT-2026-10-01.md. The engine puts your torso and shoulders at the "
           "head position it is given, and the headset reports your EYES, which swing on an arc round "
           "your neck when you turn or nod - so the shoulders slide. On: the position given to the "
           "engine has that arc removed (eye - (R - I) n), so the body moves only when your neck does. "
           "Looking straight ahead from the recentre it is unchanged. The VIEW's position changes too. "
           "LIVE - flip it here mid-session to compare. On by default."),
    S_FLOAT("head_neck_pivot_x_mm", HeadNeckPivotXmm, -50.0f, 50.0f,
            "Neck pivot: eye offset right (mm)", "Camera hook",
            "n.x - how far your eye midpoint sits RIGHT of your neck pivot, in the headset's frame. "
            "Default +8 = measured for one user's headset (INV-321)."),
    S_FLOAT("head_neck_pivot_y_mm", HeadNeckPivotYmm, 0.0f, 200.0f,
            "Neck pivot: eye offset up (mm)", "Camera hook",
            "n.y - how far your eye midpoint sits ABOVE your neck pivot. Default +112 = measured from "
            "nods for one user's headset (INV-321)."),
    S_FLOAT("head_neck_pivot_z_mm", HeadNeckPivotZmm, -200.0f, 0.0f,
            "Neck pivot: eye offset forward (mm, negative)", "Camera hook",
            "n.z - the eye midpoint's offset along the headset's z axis, NEGATIVE = in FRONT of the neck "
            "(OpenXR -z is forward). Default -84 = measured from turns (nods read -96, INV-321)."),
    S_BOOL("cam_auto", CamHookAuto, "Automatic camera selection", "Camera hook",
           "Level every CRendererCamera the engine is actually driving, "
           "identified by its vtable and by its matrix changing. Off means "
           "picking rows out of the scan by hand, which cannot survive a level "
           "load - only the two eye cameras keep their addresses."),
    S_BOOL("cam_derive", CamHookDerive, "Re-derive view and combined",
           "Camera hook",
           "Rebuild view = inverse(camera) and combined = proj * view the way "
           "CCamera::SetView does, instead of nudging them. Off falls back to "
           "the old delta multiply, which leaves the two disagreeing."),
    S_BOOL("cam_write_combined", CamHookWriteCombined, "Write combined matrix",
           "Camera hook",
           "The renderer reads the combined matrix, not the camera one - writing "
           "only the camera moved the culling and left the picture alone."),
    S_BOOL("cam_combined_pre", CamHookCombinedPre, "Combined pre-multiply",
           "Camera hook", "Flip if the view tilts the wrong way or skews."),
    S_BOOL("cam_write_view", CamHookWriteView, "Write view matrix", "Camera hook",
           "Synthesises the view matrix from an assumption about its form. The "
           "only place a whole matrix is built rather than edited, so the only "
           "place that can move the camera somewhere else entirely."),
    S_BOOL("cam_matrix_transposed", CamHookTransposed, "Matrix transposed",
           "Camera hook", "Read the stored 3x3 as column-major."),
    S_FLOAT("cam_delta_deg", CamHookDeltaDeg, -80.0f, 80.0f, "Delta degrees",
            "Camera hook", "How far the delta test rotates."),
    S_ENUM("cam_delta_axis", CamHookAxis, "Delta axis", "Camera hook",
           "Which axis the delta test rotates about.", kAxisXYZ),
    S_BOOL("cam_delta_world", CamHookPreMultiply, "Delta about world axis",
           "Camera hook", "Pre-multiply rather than post-multiply."),
    S_BOOL("rtti_only_real", RttiOnlyRealCameras, "Only real cameras", "Camera hook",
           "Hide objects that carry the vtable but were never set up - most of "
           "them - so the list is short enough to read."),
    S_BOOL("rtti_sort_activity", RttiSortByActivity, "Sort by activity",
           "Camera hook",
           "On finds the live camera; off holds the rows still so you can click "
           "one."),

    S_BOOL("overlay_enabled", g_overlayEnabled, "Overlay enabled", "Overlay",
           "Uncheck to disable the toggle entirely."),
    S_INT("overlay_key", g_overlayKey, 0, 255, "Keyboard binding", "Overlay",
          "Virtual-key code. Rebind with the Capture button."),
    S_INT("overlay_pad", g_overlayPad, 0, 65535, "Gamepad binding", "Overlay",
          "XInput button mask, all of which must be held. Default is L3+R3."),
    S_FLOAT("overlay_distance", g_overlayDistance, 0.4f, 4.0f, "Distance (m)", "Overlay",
            "How far in front of your head the panel sits."),
    S_FLOAT("overlay_width", g_overlayWidth, 0.3f, 3.0f, "Width (m)", "Overlay",
            "Panel width in metres; height follows the panel's aspect."),
    S_INT("overlay_text_size", g_overlayTextSize, 60, 150, "Text size", "Overlay",
          "The mod menu's text size, in percent. 100 is the original size."),
    S_BOOL("overlay_advanced", g_overlayAdvanced, "Advanced menu", "Overlay",
           "Show the advanced rows in the mod menu."),
};

#undef S_BOOL
#undef S_ENUM
#undef S_FLOAT
#undef S_INT

const int kSettingCount = static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]));
float ClampTo(const Setting& st, float v)
{
    if (v < st.lo) {
        return st.lo;
    }
    if (v > st.hi) {
        return st.hi;
    }
    return v;
}

void FormatSetting(const Setting& st, char* out, size_t cch)
{
    if (st.type == SetType::Float) {
        _snprintf_s(out, cch, _TRUNCATE, "%s=%g", st.key, *static_cast<float*>(st.value));
    } else if (st.type == SetType::Int) {
        _snprintf_s(out, cch, _TRUNCATE, "%s=%d", st.key, *static_cast<int*>(st.value));
    } else {
        _snprintf_s(out, cch, _TRUNCATE, "%s=%d", st.key,
                    *static_cast<bool*>(st.value) ? 1 : 0);
    }
}

// The values the code was built with, captured before the file is read.
//
// A setting that is missing from the file, or present with a value outside its
// range, falls back to one of these rather than being clamped. Clamping turns
// "3" into "2" and looks deliberate; a value nobody chose should go back to the
// one that was.
float g_defaults[kSettingCount] = {};
bool g_seen[kSettingCount] = {};
bool g_lockedOverrode = false;   // a LOCKED setting's file value differed - the file is rewritten to the locked value
bool g_defaultsCaptured = false;
signed char g_followerFile[kSettingCount] = {};   // a FOLLOWER's value as the file had it (0 / 1) - valid where g_seen[i]

// Every FOLLOWER (settings.h `follows`) takes its master's value. Two bool copies per follower - called at load and
// from SaveSettingsIfChanged (after each overlay tab draws), never on a per-frame render path.
void SyncFollowers()
{
    for (int i = 0; i < kSettingCount; ++i) {
        const Setting& st = kSettings[i];
        if (st.follows) {
            *static_cast<bool*>(st.value) = *st.follows;
        }
    }
    // Data3 check (Jay 2026-10-18): AFW only on this build's Data3.pak - afw_enable stays as the user set it, its followers are held
    // off (camhook.cpp AfwData3Allowed says why, once). Only asked while the master is on.
    if (AfwEnable && !dlvr::AfwData3Allowed()) {
        AfwAltEye = false;
        AfwRebuild = false;
    }
    // [AFWNET] (trace docs/traces/20261121_1057_afw_tonemap_net.md): while the safety net holds them off (the bootstrap has not seen
    // the tonemap draw - no rebuild possible), a settings write / menu tab draw must not turn them back on. camhook gives them back.
    if (dlvr::AfwNetHeld()) {
        AfwAltEye = false;
        AfwRebuild = false;
    }
}

float SettingAsFloat(const Setting& st)
{
    if (st.type == SetType::Float) {
        return *static_cast<float*>(st.value);
    }
    if (st.type == SetType::Int) {
        return static_cast<float>(*static_cast<int*>(st.value));
    }
    return *static_cast<bool*>(st.value) ? 1.0f : 0.0f;
}

void SettingFromFloat(const Setting& st, float v)
{
    if (st.type == SetType::Float) {
        *static_cast<float*>(st.value) = v;
    } else if (st.type == SetType::Int) {
        *static_cast<int*>(st.value) = static_cast<int>(v);
    } else {
        *static_cast<bool*>(st.value) = (v != 0.0f);
    }
}

void LoadLayerConfig()
{
    if (g_configLoaded) {
        return;
    }
    g_configLoaded = true;

    LoadHmdCache();

    for (int i = 0; i < kSettingCount; ++i) {
        g_defaults[i] = SettingAsFloat(kSettings[i]);
    }
    g_defaultsCaptured = true;

    char path[MAX_PATH * 2];
    if (!LayerFilePath("ovr2xr.txt", path, sizeof(path))) {
        return;
    }

    bool existed = false;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f) {
        existed = true;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') {
                continue;
            }
            ApplySettingLine(line);
        }
        fclose(f);
    } else {
        LOG("config: %s not found - writing one with the built-in defaults", path);
    }


    // ONE-SHOT ENABLE (2026-10-05, Jay: "Turn the fix on, repackage it"): an install that ran build 848769b4 saved
    // submit_drawn_image=0 into its ovr2xr.txt, and a saved value beats the compiled default. If the package's marker
    // dlvr\enable_submit_drawn_image.txt is present, the fix is switched ON, the file is rewritten so it stays on, and the
    // marker is DELETED - so a later untick by the user is respected. Absent marker: nothing happens (logged once).
    bool forceWrite = false;
    {
        char marker[MAX_PATH * 2] = {};
        if (!LayerFilePath("enable_submit_drawn_image.txt", marker, sizeof(marker))) {
            LOG("[SUBDRAWN] one-shot marker: could NOT resolve the dlvr folder - not checked");
        } else if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) {
            const bool was = SubmitDrawnImage;
            SubmitDrawnImage = true;
            forceWrite = true;
            const BOOL gone = DeleteFileA(marker);
            // [PROBE: SUBDRAWN] gate=dlvr\enable_submit_drawn_image.txt (one-shot)  site=FLAT-SAFE  log-only=no
            LOG("[SUBDRAWN] one-shot marker %s found: submit_drawn_image %d -> 1, written to the settings file; marker %s",
                marker, was ? 1 : 0, gone ? "DELETED" : "could NOT be deleted - it will apply again next launch");
        } else {
            LOG("[SUBDRAWN] one-shot marker %s absent - submit_drawn_image stays as the settings file says (%d)", marker,
                SubmitDrawnImage ? 1 : 0);
        }
    }

    // STREAMING BUDGET -> AUTO (2026-11-13, Jay: "we're putting it in for end users now", "Reserve needs to be auto", "Yes, migrate
    // 2048 to AUTO"; docs/traces/20261113_1603_streambudget_auto_reserve.md). Every install that ran an earlier build saved the old
    // compiled defaults (stream_budget_mb=2048, stream_budget_reserve_mb=3072) into its file, and a saved value beats the compiled
    // one. Exactly those values move to -1 (AUTO) and the file is rewritten; any other number is the user's own and stays. A key
    // absent from the file already holds the compiled -1, so the compare alone decides. Logged only when it fires (once per install:
    // the rewritten file holds -1). A user who later types exactly 2048 / 3072 is moved again - any other number is kept.
    if (dlvr::StreamBudgetMB == 2048) {
        dlvr::StreamBudgetMB = -1;
        forceWrite = true;
        LOG("config: stream_budget_mb was 2048 (the old default) - moved to -1 (AUTO) and written to the settings file");
    }
    if (dlvr::StreamBudgetReserveMB == 3072) {
        dlvr::StreamBudgetReserveMB = -1;
        forceWrite = true;
        LOG("config: stream_budget_reserve_mb was 3072 (the old default) - moved to -1 (AUTO, 20%% of the card) and written to "
            "the settings file");
    }

    // WRIST HUD HOLDS THE HUD PANEL ON (Jay 2026-11-14, HARD: "WRIST HUD HARD REQUIREMENT - HUD ON A FIXED PANEL ON - NEEDS TO TURN
    // ON AUTOMATICALLY OTHERWISE CAN GLITCH FOR USERS"; trace docs/traces/20261114_1200_wrist_holds_hud_panel.md). The wrist HUD's HUD
    // redirect rides the HUD panel's PanelInfo and every wrist run had ui_hud_panel=1. ui_wrist_hud=1 in the file -> ui_hud_panel on,
    // the file rewritten, BEFORE the config dump below. Reads UiWristHud itself, not wristquad::OnThisLaunch - that latch's first
    // caller stays wristprobe::Start (the same value). One line every launch, whichever way it goes.
    {
        const bool hudFile = dlvr::UiHudPanel;
        if (dlvr::UiWristHud && !hudFile) {
            dlvr::UiHudPanel = true;
            forceWrite = true;
        }
        LOG("config: ui_wrist_hud=%d, ui_hud_panel in the file %d -> %s", dlvr::UiWristHud ? 1 : 0, hudFile ? 1 : 0,
            !dlvr::UiWristHud ? "the HUD panel as the file says"
            : hudFile ? "held on for the Wrist HUD (already on)"
                      : "TURNED ON for the Wrist HUD and written to the settings file");
    }

    // FOLLOWERS (settings.h): every one takes its master's value now that the whole file is read. Each says so once,
    // whatever the file held, so the config dump's value for it is never a surprise; a file value that differs is
    // rewritten to match (the same route as a locked one).
    SyncFollowers();
    for (int i = 0; i < kSettingCount; ++i) {
        const Setting& st = kSettings[i];
        if (!st.follows) {
            continue;
        }
        const int now = *static_cast<bool*>(st.value) ? 1 : 0;
        const bool differs = g_seen[i] && g_followerFile[i] != now;
        if (differs) {
            g_lockedOverrode = true;
        }
        LOG("config: %s FOLLOWS its master = %d (hidden; the file's value %s)", st.key, now,
            !g_seen[i] ? "was absent" : differs ? "differed and is rewritten" : "already matched");
    }

    int missing = 0;
    for (int i = 0; i < kSettingCount; ++i) {
        if (!g_seen[i]) {
            ++missing;
            LOG("config: %s absent, using the default", kSettings[i].key);
        }
    }
    // Put the file back into a complete state: every key present, every value
    // one this build understands. Nothing is lost - the writer edits values in
    // place and leaves comments alone.
    if (!existed || missing > 0 || forceWrite || g_lockedOverrode) {
        char why[256] = {};
        if (!WriteSettingsFile(why, sizeof(why))) {
            LOG("config: could not write %s (%s)", path, why);
        } else {
            LOG("config: wrote %s (%d value%s filled in)", path, missing,
                missing == 1 ? "" : "s");
        }
    }

    for (int i = 0; i < kSettingCount; ++i) {
        char buf[256];
        FormatSetting(kSettings[i], buf, sizeof(buf));
        LOG("config: %s", buf);
    }
    // S1 shadow stagger: read ONCE here at launch; at 0 nothing of it runs for the rest of the session unless the menu sets it >= 2
    // (SaveSettingsIfChanged). Jay 2026-10-16: "Truly inert."
    if (dlvr::ShadowCascadeStagger >= 2) {
        dlvr::ShadowStaggerStart();
    }
    CamHookOp = static_cast<HookOp>((g_camHookOp >= 0 && g_camHookOp <= 2)
                                        ? g_camHookOp
                                        : 0);
}

void SetError(ovrResult r, const char* msg)
{
    g_lastError.Result = r;
    strncpy_s(g_lastError.ErrorString, sizeof(g_lastError.ErrorString), msg, _TRUNCATE);
    LOG("error %d: %s", static_cast<int>(r), msg);
}

Session* S(ovrSession s)
{
    return reinterpret_cast<Session*>(s);
}

ovrSizei EyeRenderSize(Session* s, ovrEyeType eye, float pixelsPerDisplayPixel);
ovrSizei EyeSizeFromRecommended(uint32_t recW, uint32_t recH, float pixelsPerDisplayPixel);

// The runtime's true per-eye frustum: live if views have been located, else the
// value measured on a previous run. False only on a first-ever launch, before
// hmd_cache.txt exists.
bool RealEyeFov(Session* s, int idx, ovrFovPort& out)
{
    if (s && s->posesValid) {
        out = ToOvrFov(s->views[idx].fov);
        return true;
    }
    if (g_hmdCache.valid) {
        out = g_hmdCache.fov[idx];
        return true;
    }
    return false;
}

// The FOV we hand the game - the single source of truth for it.
//
// TWO answers come out of it since 2026-11-25 (trace docs/traces/20261125_1040_live_eye_fov.md):
//   ovr_GetHmdDesc (startup)          = `mode` 2, the centred superset: the engine's one game camera (culling, sun-shadow fit, light lists).
//   ovr_GetRenderDesc (every frame)   = `mode` symmetric_fov, what each eye is DRAWN with (EyeFovForFrame below).
// What ovr_SubmitFrame declares is the second one: declared must equal rendered or the compositor resamples with the wrong mapping and
// the whole image warps.
//
// The engine is assumed to handle only a SYMMETRIC frustum. A Quest 3 lens is
// not symmetric (54 deg out / 40 deg in, 44 up / 55 down), and the fix is not
// to hand the engine an asymmetry it will mangle: it is to hand it the smallest
// symmetric frustum that CONTAINS the lens - max half-angle on each axis. That
// is a strict superset, so nothing is missing at any edge and no black bars are
// possible; we just render some pixels beyond the lens and the compositor drops
// them. Both eyes come out with the same frustum, which also suits an engine
// that assumes one projection for the pair - but only on a headset whose eyes are
// mirror images: OwnEyeFov symmetrises EACH eye from its own FOV, and GameEyeFov
// below is what makes the two identical everywhere (INV-597).
ovrFovPort OwnEyeFov(Session* s, int idx, const char** srcOut, int mode)
{
    LoadLayerConfig();

    ovrFovPort f;
    f.UpTan = f.DownTan = f.LeftTan = f.RightTan = tanf(0.8f);
    const char* src = "symmetric fallback";

    ovrFovPort real{};
    const bool haveReal = RealEyeFov(s, idx, real);

    if (g_fovSource >= 1 && haveReal) {
        f = real;
        src = (s && s->posesValid) ? "live runtime" : "cached from a previous session";
    } else if (g_fovSource >= 1) {
        // [LOADBEARING: FOV-FIRST-LAUNCH] no live views and no hmd_cache.txt (a first launch): the game is handed this
        // frustum until the live FOV is known (EyeFovForFrame, 2026-11-25; the whole session before that), and its centred superset
        // is the cull camera's for the WHOLE session (ovr_GetHmdDesc, read once). fovtable_logic.h: largest measured FOV for the
        // runtime's name, or the wide row for a name not in the table.
        dlvr::fovtable::Tans t{};
        const char* row = dlvr::fovtable::Lookup(g_xrSystemName, idx, t);
        f.LeftTan = t.l; f.RightTan = t.r; f.UpTan = t.u; f.DownTan = t.d;
        static char s_src[2][XR_MAX_SYSTEM_NAME_SIZE + 64];
        _snprintf_s(s_src[idx & 1], sizeof(s_src[0]), _TRUNCATE,
                    row ? "first-launch table row \"%s\"" : "first-launch WIDE row - \"%s\" is not in the table",
                    row ? row : (g_xrSystemName[0] ? g_xrSystemName : "(system name not read)"));
        src = s_src[idx & 1];
    } else if (g_verticalExtend && haveReal) {
        const float need = (real.UpTan > real.DownTan) ? real.UpTan : real.DownTan;
        if (need > f.UpTan) {
            f.UpTan = need;
            f.DownTan = need;
            src = "fallback + vertical extend";
        }
    }

    dlvr::eyefov::Tans t = dlvr::eyefov::Symmetrise(ToTans(f), mode);

    // Grow (never shrink) to the eye texture's aspect, so the tangent-derived
    // and aspect-derived readings of this frustum are the same frustum.
    if (g_fovMatchAspect && s) {
        const ovrSizei sz = EyeRenderSize(s, (idx == 1) ? ovrEye_Right : ovrEye_Left, 1.0f);
        if (sz.w > 0 && sz.h > 0) {
            t = dlvr::eyefov::GrowToAspect(t, static_cast<float>(sz.w) / static_cast<float>(sz.h));
        }
    }
    f = FromTans(t);

    if (srcOut) {
        *srcOut = src;
    }
    return f;
}

ovrFovPort GameEyeFov(Session* s, int idx, const char** srcOut, int mode)
{
    const char* src = "";
    ovrFovPort f = OwnEyeFov(s, idx, &src, mode);
    // [LOADBEARING: FOV-SHARED-FRUSTUM] symmetric_fov=2: both eyes are handed the frustum that contains both. The AFW rebuild
    //      draws the held eye with the FRESH eye's projection (dlssflat.cpp AfwRebuildRun, INV-597), so two different
    //      frustums put every rebuilt frame off in zoom - 1.9 % on the Steam Frame (eye0 1.7260 / eye1 1.6938). Mirror-image
    //      headsets compute identical frustums here and are unchanged. Modes 0 / 1 hand over deliberately mirrored frustums
    //      and are left alone.
    if (mode >= 2) {
        const ovrFovPort o = OwnEyeFov(s, 1 - (idx & 1), nullptr, mode);
        const dlvr::eyefov::Tans own{ f.LeftTan, f.RightTan, f.UpTan, f.DownTan };
        const dlvr::eyefov::Tans oth{ o.LeftTan, o.RightTan, o.UpTan, o.DownTan };
        const dlvr::eyefov::Tans sh = dlvr::eyefov::Shared(own, oth);
        if (dlvr::eyefov::Differ(sh, own)) {
            f.LeftTan = sh.l; f.RightTan = sh.r; f.UpTan = sh.u; f.DownTan = sh.d;
            static char s_shared[2][XR_MAX_SYSTEM_NAME_SIZE + 160];
            _snprintf_s(s_shared[idx & 1], sizeof(s_shared[0]), _TRUNCATE,
                        "%s - WIDENED to the other eye's frustum (own L/R %.4f U/D %.4f), one frustum for both eyes",
                        src, own.l, own.u);
            src = s_shared[idx & 1];
        }
    }
    if (srcOut) {
        *srcOut = src;
    }
    return f;
}

// THE LIVE EYE FOV (trace docs/traces/20261125_1040_live_eye_fov.md; Jay 2026-11-25). The engine rebuilds each eye's frustum every frame from the
// Fov ovr_GetRenderDesc RETURNS (engine +2E5530 -> +2E68F0, its only writer), so the eyes can follow the headset's FOV of THIS launch; the
// startup answer alone left a headset whose driver changes the FOV at every start drawn short (a tester 2026-11-24). The state
// machine, and why the switch is never seen, is eyefov_logic.h LiveFov. Read and written under the frame lock (Guard) only.
dlvr::eyefov::LiveFov g_liveFov;

// What eye `idx` is drawn with and declared with now. `asked` = the engine's own number, used only before there is a session.
// COST after the switch: one bool test + a 16-byte copy (~470 calls / s); while it waits for a hidden moment the same.
ovrFovPort EyeFovForFrame(Session* s, int idx, const ovrFovPort& asked)
{
    namespace ef = dlvr::eyefov;
    if (!ef::Ready(g_liveFov)) {
        if (!s) {
            return asked;
        }
        const ef::Tans st[2] = { ToTans(GameEyeFov(s, 0, nullptr, SymmetricFovMode())),
                                 ToTans(GameEyeFov(s, 1, nullptr, SymmetricFovMode())) };
        ef::SetStart(g_liveFov, st);
        // [PROBE: LIVEFOV] gate=none (once per launch, logged unconditionally)  site=HEADSET  log-only=no
        LOG("[LIVEFOV] START (logged unconditionally): the eyes are drawn with eye0 L %.4f R %.4f U %.4f D %.4f eye1 L %.4f R %.4f U %.4f "
            "D %.4f (%s; symmetric_fov=%d) until the runtime reports this launch's own FOV", st[0].l, st[0].r, st[0].u, st[0].d, st[1].l,
            st[1].r, st[1].u, st[1].d, s->posesValid ? "the live FOV already" : "saved by the previous launch, or the first-launch row",
            SymmetricFovMode());
    }
    if (!g_liveFov.latched && s && s->posesValid && (!g_liveFov.saidWait || g_liveFov.lastHidden)) {
        const ef::Tans live[2] = { ToTans(GameEyeFov(s, 0, nullptr, SymmetricFovMode())),
                                   ToTans(GameEyeFov(s, 1, nullptr, SymmetricFovMode())) };
        const ef::LatchResult r = ef::TryLatch(g_liveFov, true, live);
        const char* what = nullptr;
        switch (r) {
            case ef::LatchResult::kSame:
                what = "SAME as the startup one: nothing changes";
                break;
            case ef::LatchResult::kSwitched:
                what = "SWITCHED to the live one - no game image was on the display; it stays off it for the next 4 submits";
                break;
            case ef::LatchResult::kWaiting:
                what = "WAITING: it differs and the game is on the display - the startup one stays until the next loading screen";
                break;
            case ef::LatchResult::kRefused:
                what = "*** REFUSED: not a plausible FOV - the startup one stays until a plausible one is reported ***";
                break;
            default:
                break;
        }
        if (what) {
            // [PROBE: LIVEFOV] gate=none (one line per outcome, at most 4 a launch)  site=HEADSET  log-only=no
            // [LOADBEARING: FOV-LIVE-EYE] this switch is what makes the drawn frustum the headset's own on a launch whose FOV is not the saved
            //      one; without it the SteamVR panel crop is refused there (STEAMVR-PANEL-CROP) and the image is short at every edge.
            LOG("[LIVEFOV] %s | live eye0 L %.4f R %.4f U %.4f D %.4f eye1 L %.4f R %.4f U %.4f D %.4f | startup eye0 L %.4f R %.4f U %.4f D %.4f",
                what, live[0].l, live[0].r, live[0].u, live[0].d, live[1].l, live[1].r, live[1].u, live[1].d, g_liveFov.start[0].l,
                g_liveFov.start[0].r, g_liveFov.start[0].u, g_liveFov.start[0].d);
        }
    }
    return FromTans(ef::Used(g_liveFov, idx));
}

// The single source of truth for per-eye render size.
//
// This must be used everywhere we report a size, because the game cross-checks
// them: ovr_GetFovTextureSize, ovrEyeRenderDesc::DistortedViewport and
// ovrHmdDesc::Resolution all have to agree. Clamping only one of them hands the
// engine contradictory numbers - it sized something from the 3072x3264
// DistortedViewport and something else from the 1504x1600 texture size, and
// fell over in between.
ovrSizei EyeRenderSize(Session* s, ovrEyeType eye, float pixelsPerDisplayPixel)
{
    LoadLayerConfig();

    ovrSizei size{ 1280, 1440 };
    if (!s) {
        return size;
    }
    const int idx = (eye == ovrEye_Right) ? 1 : 0;
    return EyeSizeFromRecommended(s->viewConfigs[idx].recommendedImageRectWidth,
                                  s->viewConfigs[idx].recommendedImageRectHeight, pixelsPerDisplayPixel);
}

// The arithmetic of EyeRenderSize, from a runtime recommendation alone - shared with DlvrQueryEyeSizeEarly (the bootstrap's
// same-launch size, early_eye_size), which has no Session yet, so the two cannot size an eye differently.
ovrSizei EyeSizeFromRecommended(uint32_t recW, uint32_t recH, float pixelsPerDisplayPixel)
{
    LoadLayerConfig();

    ovrSizei size{ 1280, 1440 };
    const float ppd = (pixelsPerDisplayPixel > 0.0f) ? pixelsPerDisplayPixel : 1.0f;

    float w = recW * ppd * g_renderScale;
    float h = recH * ppd * g_renderScale;


    // Preserve aspect while capping the larger dimension.
    const float largest = (w > h) ? w : h;
    if (g_maxDim > 0 && largest > static_cast<float>(g_maxDim)) {
        const float k = static_cast<float>(g_maxDim) / largest;
        w *= k;
        h *= k;
    }
    size.w = static_cast<int>(w) & ~3;  // keep it a tidy multiple of 4
    size.h = static_cast<int>(h) & ~3;
    return size;
}

// --------------------------------------------------------- session plumbing --

// Queries the view configuration. Available as soon as we have a systemId, so
// ovr_GetHmdDesc and ovr_GetFovTextureSize work before the session exists.
bool QueryViewConfigs(Session* s)
{
    uint32_t count = 0;
    XrResult r = xrEnumerateViewConfigurationViews_(
        s->instance, s->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (!XrOk(r, "xrEnumerateViewConfigurationViews(count)") || count < 2) {
        return false;
    }
    XrViewConfigurationView v[2]{};
    v[0].type = v[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    count = 2;
    r = xrEnumerateViewConfigurationViews_(
        s->instance, s->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, v);
    if (!XrOk(r, "xrEnumerateViewConfigurationViews")) {
        return false;
    }
    s->viewConfigs[0] = v[0];
    s->viewConfigs[1] = v[1];
    LOG("view config: %ux%u recommended, %ux%u max",
        v[0].recommendedImageRectWidth, v[0].recommendedImageRectHeight,
        v[0].maxImageRectWidth, v[0].maxImageRectHeight);
    return true;
}

// Creates the XrSession. Deferred until we are handed a D3D11 device, because
// OpenXR requires the graphics binding up front and LibOVR does not give us one
// until the first swapchain.
bool EnsureSession(Session* s, ID3D11Device* device)
{
    if (s->session != XR_NULL_HANDLE) {
        return true;
    }
    if (!device) {
        return false;
    }
    s->device = device;
    device->AddRef();

    // The runtime dictates which adapter we must use; honour it.
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (xrGetD3D11GraphicsRequirementsKHR_) {
        XrResult rr = xrGetD3D11GraphicsRequirementsKHR_(s->instance, s->systemId, &req);
        if (XR_SUCCEEDED(rr)) {
            s->adapterLuid = req.adapterLuid;
        }
    }

    XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = device;

    XrSessionCreateInfo ci{ XR_TYPE_SESSION_CREATE_INFO };
    ci.next = &binding;
    ci.systemId = s->systemId;

    XrResult r = xrCreateSession_(s->instance, &ci, &s->session);
    if (!XrOk(r, "xrCreateSession")) {
        return false;
    }
    LOG("xrCreateSession OK (device=%p)", device);

    // STAGE gives a floor-relative origin; LOCAL is eye-level. Prefer whichever
    // matches the tracking origin the game asked for, falling back to LOCAL.
    XrReferenceSpaceCreateInfo sp{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    sp.poseInReferenceSpace = XrIdentity();
    sp.referenceSpaceType = (s->trackingOrigin == ovrTrackingOrigin_FloorLevel)
                                ? XR_REFERENCE_SPACE_TYPE_STAGE
                                : XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xrCreateReferenceSpace_(s->session, &sp, &s->appSpace))) {
        sp.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!XrOk(xrCreateReferenceSpace_(s->session, &sp, &s->appSpace),
                  "xrCreateReferenceSpace(LOCAL)")) {
            return false;
        }
    }
    s->appSpaceType = sp.referenceSpaceType;
    // [PROBE: RECENTRE] gate=comfort_body_follows_head  site=HEADSET  log-only=no
    LOG("[RECENTRE] appSpace reference type %d (2 LOCAL, 3 STAGE) - a runtime recentre of this type is absorbed in yaw while "
        "body-follows-head is on (D-253)", static_cast<int>(s->appSpaceType));

    sp.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrOk(xrCreateReferenceSpace_(s->session, &sp, &s->viewSpace),
         "xrCreateReferenceSpace(VIEW)");

    // The floor for the holsters' standing / seated posture (trace 20261116_1545_standing_posture): a STAGE space beside appSpace,
    // only ever LOCATED (appSpace is not touched). VDXR: STAGE = the floor (VirtualDesktop-OpenXR space.cpp:626-635).
    {
        XrReferenceSpaceCreateInfo st{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        st.poseInReferenceSpace = XrIdentity();
        st.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        const XrResult sr = xrCreateReferenceSpace_(s->session, &st, &s->stageSpace);
        if (XR_FAILED(sr)) { s->stageSpace = XR_NULL_HANDLE; }
        // [PROBE: POSTURE] gate=none  site=HEADSET  log-only=no
        LOG("[POSTURE] floor space (STAGE) %s (XrResult %d) - %s", XR_SUCCEEDED(sr) ? "CREATED" : "REFUSED", static_cast<int>(sr),
            XR_SUCCEEDED(sr) ? "the holsters measure standing / seated from it"
                             : "no floor: the holsters treat the player as SEATED (the f365b5c0 facing)");
    }

    // Not fatal if this fails - the game still gets a head pose either way,
    // it just means aim redirection has nothing to read from yet.
    ControllersInit(s->instance, s->session, s->appSpace);
    // The body-tracked elbow: the system's support + the entry points (the tracker is created on the first frame with the setting on).
    BodyTrackInit(s->instance, s->systemId, s->session);

    return true;
}

void PollEvents(Session* s)
{
    if (!s || s->instance == XR_NULL_HANDLE) {
        return;
    }
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent_ && xrPollEvent_(s->instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                s->state = e->state;
                LOG("session state -> %d", static_cast<int>(e->state));
                {
                    const ::dlvr::centre::Reason before = s->centre.pending;
                    const long long armedBefore = s->centre.armedAt;
                    ::dlvr::centre::OnState(s->centre, static_cast<int>(e->state), static_cast<long long>(e->time));
                    if (s->centre.pending != ::dlvr::centre::Reason::None &&
                        (s->centre.pending != before || s->centre.armedAt != armedBefore)) {
                        s->centreUntracked = 0;
                        s->centreAbsorbWait = 0;
                        // [PROBE: CENTRE] gate=none  site=HEADSET  log-only=no
                        LOG("[CENTRE] ARMED %s at state %d (logged unconditionally) - appSpace moves sideways onto the head once it "
                            "is tracked %.1f s after this (gives up after %.1f s)", ::dlvr::centre::ReasonName(s->centre.pending),
                            static_cast<int>(e->state), static_cast<double>(::dlvr::centre::kSettleNs) / 1.0e9,
                            static_cast<double>(::dlvr::centre::kGiveUpNs) / 1.0e9);
                    }
                }
                if (e->state == XR_SESSION_STATE_READY && !s->running) {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XrOk(xrBeginSession_(s->session, &bi), "xrBeginSession")) {
                        s->running = true;
                    }
                } else if (e->state == XR_SESSION_STATE_STOPPING && s->running) {
                    XrOk(xrEndSession_(s->session), "xrEndSession");
                    s->running = false;
                } else if (e->state == XR_SESSION_STATE_EXITING ||
                           e->state == XR_SESSION_STATE_LOSS_PENDING) {
                    s->shouldQuit = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                s->shouldQuit = true;
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                s->shouldRecenter = true;
                // The runtime moved LOCAL (INV-300): the HUD panel's latch, a pose in appSpace, is re-taken (Jay 2026-10-17).
                auto* e = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
                ::dlvr::uipanel::HudRecentreRuntime(e->changeTime, static_cast<int>(e->referenceSpaceType));
                // D-253: with body-follows-head on, absorb the recentre's YAW (applied in BeginFrameIfNeeded); off = the snap.
                const bool typeMatches = e->referenceSpaceType == s->appSpaceType;
                if (ComfortBodyFollowsHead && HeadYawDecoupleActive()) {
                    ::dlvr::recentre::Queue(s->absorb, typeMatches, static_cast<long long>(e->changeTime),
                                            e->poseValid != XR_FALSE,
                                            ::dlvr::recentre::YawDegAboutY(&e->poseInPreviousSpace.orientation.x));
                    if (!typeMatches && s->absorbCount < 200) {
                        ++s->absorbCount;
                        // [PROBE: RECENTRE] gate=comfort_body_follows_head  site=HEADSET  log-only=no
                        LOG("[RECENTRE] event for reference type %d ignored - appSpace is built on type %d",
                            static_cast<int>(e->referenceSpaceType), static_cast<int>(s->appSpaceType));
                    }
                } else if (typeMatches && s->absorbCount < 200) {
                    ++s->absorbCount;
                    // [PROBE: RECENTRE] gate=comfort_body_follows_head  site=HEADSET  log-only=no
                    LOG("[RECENTRE] SNAP as designed - body-follows-head %s, decouple %s (type %d)",
                        ComfortBodyFollowsHead ? "on" : "OFF", HeadYawDecoupleActive() ? "active" : "INACTIVE",
                        static_cast<int>(e->referenceSpaceType));
                }
                break;
            }
            default:
                break;
        }
        ev = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// xrLocateViews on its own - no wait, no begin, no blocking.
//
// This is split out so a job-system worker asking for the head pose can refresh
// it at the moment it asks, instead of being handed whatever was located at the
// last submit. That staleness is the difference between a pose sampled when the
// engine wants it and one sampled up to a frame earlier, and a frame of
// ROTATIONAL error is far more visible than a frame of translational error -
// which is exactly the reported shape of the problem: jittery when turning the
// head, smooth when walking with the head still.
//
// Only xrWaitFrame and xrBeginFrame have to stay on the render thread. Locating
// views does not block and does not touch the frame boundary, so it is safe to
// do here.
// B13 (trace 20261115_1545): 1 from the first locate with BOTH valid bits, never cleared. Before it, s->headPose is still its
// XrIdentity() start value (h = 0.000), which physical crouch once learned its standing level from (20261108_knife_logs).
volatile LONG g_headLocatedOnce = 0;
void LocateViews(Session* s)
{
    if (!s || s->session == XR_NULL_HANDLE || !s->running ||
        !s->frameState.predictedDisplayTime) {
        return;
    }
    // Same predicted display time and space as the views below, so a
    // controller pose used to redirect aim this frame is predicted for the
    // same instant as the frame actually being built from it.
    ControllersLocate(s->frameState.predictedDisplayTime, s->appSpace);
    // [LOADBEARING: rig_elbow_body_tracking] the body is located here, same time + space as the controllers (bodytrack.cpp; once
    // per display time, setting off = one bool test).
    BodyTrackLocate(s->frameState.predictedDisplayTime, s->appSpace);

    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = s->frameState.predictedDisplayTime;
    li.space = s->appSpace;

    XrViewState vs{ XR_TYPE_VIEW_STATE };
    uint32_t count = 0;
    XrView views[2]{};
    views[0].type = views[1].type = XR_TYPE_VIEW;
    if (XR_SUCCEEDED(xrLocateViews_(s->session, &li, &vs, 2, &count, views)) && count >= 2) {
        // [LOADBEARING: CANTED-VIEWS] the game renders BOTH eyes at ONE orientation (SDK 1.10 HmdToEyeOffset is a translation,
        //      ovr2xr.cpp:24-27). On a runtime whose two views are turned against each other (canted displays - a Pimax), the
        //      views are replaced HERE by their bisector and each FOV re-expressed in it (cantview_logic.h ToParallel), before
        //      anything copies them: the head (:headPose below), the declared poses (submit, rendered * inv(view0) * view_e)
        //      and the game's frustum (RealEyeFov / hmd_cache) then all describe the image the game drew. Without it eye 1 is
        //      declared turned by the whole cant - the doubled image a Crystal Light player reported 2026-11-18. Views under
        //      1 deg apart are not touched (the path every logged headset runs). Trace docs/traces/20261118_1150_canted_views.md.
        //      COST: one quaternion product + atan2 per locate; converting adds 8 vector rotations + 4 atan; fake off = one test.
        {
            using namespace ::dlvr::cantview;
            const bool oriOkCv = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
            static const float s_fake = g_fakeViewCantDeg;   // latched at the first locate: the game's frustum is fixed per launch
            const float fakeDeg = fminf(fabsf(s_fake), kMaxFakeDeg);
            const bool bypass = s_fake < 0.0f;
            Quat cq[2];
            Fov cf[2];
            for (int e = 0; e < 2; ++e) {
                cq[e] = { views[e].pose.orientation.x, views[e].pose.orientation.y, views[e].pose.orientation.z,
                          views[e].pose.orientation.w };
                cf[e] = { views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown };
            }
            const Fov rawF[2] = { cf[0], cf[1] };
            const float rawAngle = AngleDeg(cq[0], cq[1]);
            bool clampedCv = false;
            if (fakeDeg > 0.0f) {
                FakeCant(0, fakeDeg, cq[0], cf[0], &clampedCv);
                FakeCant(1, fakeDeg, cq[1], cf[1], &clampedCv);
            }
            const float usedAngle = AngleDeg(cq[0], cq[1]);
            const Quat cvRel0 = Mul(Conj(Bisector(cq[0], cq[1])), cq[0]);   // view 0 against the bisector, for the log
            static Quat s_rel[2] = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
            static bool s_active = false;
            bool convertedNow = false;
            if (bypass) {
                // the A/B leg only (diag_fake_view_cant_deg < 0): the views go on exactly as faked - the pre-fix declaration
            } else if (oriOkCv) {
                const Result cr = ToParallel(cq, cf);
                clampedCv = clampedCv || cr.clamped;
                convertedNow = cr.converted;
                s_active = cr.converted;
                if (cr.converted) { s_rel[0] = cr.rel[0]; s_rel[1] = cr.rel[1]; }
            } else if (s_active) {
                // orientation not valid this locate: it is HELD below and the FOV is not - re-express it with the last turn
                cf[0] = Reexpress(cf[0], s_rel[0], &clampedCv);
                cf[1] = Reexpress(cf[1], s_rel[1], &clampedCv);
            }
            for (int e = 0; e < 2; ++e) {
                views[e].pose.orientation = XrQuaternionf{ cq[e].x, cq[e].y, cq[e].z, cq[e].w };
                views[e].fov = XrFovf{ cf[e].left, cf[e].right, cf[e].up, cf[e].down };
            }
            if (oriOkCv) {
                static bool s_said = false;
                static int s_lastState = -1;
                static int s_lines = 0;
                const int state = bypass ? 2 : (convertedNow ? 1 : 0);
                if ((!s_said || state != s_lastState) && s_lines < 20) {
                    ++s_lines;
                    const float k = kDegPerRad;
                    const float yaw0 = 2.0f * atan2f(cvRel0.y, fabsf(cvRel0.w)) * k * (cvRel0.w < 0.0f ? -1.0f : 1.0f);
                    const float off0 = 2.0f * asinf(fminf(1.0f, sqrtf(cvRel0.x * cvRel0.x + cvRel0.z * cvRel0.z))) * k;
                    // [PROBE: CANTVIEW] gate=none (first located views unconditionally, then each change of decision, 20 lines max)  site=HEADSET  log-only=yes
                    LOG("[CANTVIEW] %s: the runtime's two views are %.3f deg apart (after the fake: %.3f; fake %.1f deg per eye%s; "
                        "view 0 turned %+.3f deg about head-up and %.3f deg off that axis, + = left) | runtime FOV deg view0 L%.2f "
                        "R%.2f U%.2f D%.2f view1 L%.2f R%.2f U%.2f D%.2f | -> %s | FOV used view0 L%.2f R%.2f U%.2f D%.2f view1 "
                        "L%.2f R%.2f U%.2f D%.2f%s",
                        s_said ? "decision changed" : "FIRST located views (logged unconditionally)", rawAngle, usedAngle,
                        fakeDeg, bypass ? ", conversion BYPASSED" : "", yaw0, off0,
                        rawF[0].left * k, rawF[0].right * k, rawF[0].up * k, rawF[0].down * k,
                        rawF[1].left * k, rawF[1].right * k, rawF[1].up * k, rawF[1].down * k,
                        state == 1 ? "CONVERTED to parallel: both eyes at their bisector, FOV re-expressed in it"
                                   : (state == 2 ? "NOT CONVERTED: diag_fake_view_cant_deg is negative (the fix bypassed)"
                                                 : "UNCHANGED: under 1.0 deg apart, a parallel-view headset"),
                        cf[0].left * k, cf[0].right * k, cf[0].up * k, cf[0].down * k,
                        cf[1].left * k, cf[1].right * k, cf[1].up * k, cf[1].down * k,
                        clampedCv ? " | CLAMPED at 80 deg: part of the display is not covered (black at that edge)" : "");
                    s_said = true;
                    s_lastState = state;
                }
            }
        }
        // Take only what the runtime says is meaningful.
        //
        // When a valid bit is clear the matching part of views[].pose is
        // UNDEFINED by the spec, and runtimes fill it with whatever is
        // convenient - identity is common. This used to copy the views in
        // regardless, so a single frame with the bit clear handed the game a
        // head snapped to the origin and then recovered. That is one frame of
        // hard discontinuity in an otherwise smooth sequence: not judder, not
        // low frame rate, a glitch - and it shows up under load, because a
        // stressed runtime is exactly when a locate comes back invalid.
        //
        // The bits are independent, so they are honoured independently, and
        // whatever is not valid this frame keeps the value it had last frame.
        // A pose one frame old is a far smaller error than a pose that is not a
        // pose at all.
        const bool oriOk = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        const bool posOk = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;

        for (int e = 0; e < 2; ++e) {
            // The FOV is a property of the view configuration, not of tracking,
            // and stays usable either way.
            s->views[e].fov = views[e].fov;
            if (oriOk) {
                s->views[e].pose.orientation = views[e].pose.orientation;
            }
            if (posOk) {
                s->views[e].pose.position = views[e].pose.position;
            }
        }
        s->posesValid = oriOk && posOk;
        if (oriOk && posOk && g_headLocatedOnce == 0) { InterlockedExchange(&g_headLocatedOnce, 1); }

        if (!oriOk || !posOk) {
            static unsigned holds = 0;
            ++holds;
            LOG_FIRST(6, "pose: locate came back %s%s - holding the previous "
                         "pose (%u so far)",
                      oriOk ? "" : "orientation-invalid ",
                      posOk ? "" : "position-invalid", holds);
            if ((holds % 100) == 0) {
                LOG("pose: %u locates held so far (flags=%#x)", holds,
                    static_cast<unsigned>(vs.viewStateFlags));
            }
        }

        // Head pose = midpoint between the eyes. Built from s->views, not from
        // the local copy, so a held component stays held here too.
        s->headPose.orientation = s->views[0].pose.orientation;
        s->headPose.position.x = (s->views[0].pose.position.x + s->views[1].pose.position.x) * 0.5f;
        s->headPose.position.y = (s->views[0].pose.position.y + s->views[1].pose.position.y) * 0.5f;
        s->headPose.position.z = (s->views[0].pose.position.z + s->views[1].pose.position.z) * 0.5f;
        // D-253 v3: one sample per frame for the recentre look-back (a re-locate of the same display time replaces it).
        // COST: one atan2 + a 20-byte copy per locate.
        if (oriOk && posOk) {
            ::dlvr::recentre::HeadPush(s->headRing, static_cast<long long>(s->frameState.predictedDisplayTime),
                                       ::dlvr::recentre::YawDegAboutY(&s->headPose.orientation.x), s->headPose.position.x,
                                       s->headPose.position.y, s->headPose.position.z);
        }
        // [PROBE: HIPCAP] gate=diag_hip_capture  site=HEADSET  log-only=yes - the standing recording for the hip-anchor redesign
        // (holster.cpp HolsterHipCapture, trace 20261116_1400_hipcap_probe). Off = one bool load.
        if (::dlvr::DiagHipCapture) {
            const float hcHp[3] = { s->headPose.position.x, s->headPose.position.y, s->headPose.position.z };
            const float hcHq[4] = { s->headPose.orientation.x, s->headPose.orientation.y, s->headPose.orientation.z,
                                    s->headPose.orientation.w };
            const float hcBase[3] = { s->appBasePos.x, s->appBasePos.y, s->appBasePos.z };
            ::dlvr::HolsterHipCapture(static_cast<long long>(s->frameState.predictedDisplayTime), oriOk && posOk, hcHp, hcHq, hcBase,
                                      s->appBaseYawDeg);
        }
        // The floor (trace 20261116_1545_standing_posture): every 30th locate, where the appSpace origin sits above the STAGE floor.
        // COST: one xrLocateSpace per 30 locates; nothing without a stage space.
        if (s->stageSpace != XR_NULL_HANDLE && xrLocateSpace_ && (s->floorTick++ % 30u) == 0u) {
            XrSpaceLocation fl{ XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(xrLocateSpace_(s->appSpace, s->stageSpace, s->frameState.predictedDisplayTime, &fl)) &&
                (fl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
                s->appFloorY = fl.pose.position.y;
                s->floorOk = true;
            } else {
                s->floorOk = false;
            }
        }

        float dx = s->views[1].pose.position.x - s->views[0].pose.position.x;
        float dy = s->views[1].pose.position.y - s->views[0].pose.position.y;
        float dz = s->views[1].pose.position.z - s->views[0].pose.position.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 0.03f && d < 0.10f) {
            s->ipd = d;
        }

        // Now that the real numbers are known, persist them for next launch.
        SaveHmdCache(s);
    }
}

// [LOADBEARING: RECENTRE-ABSORB] D-253 / docs/superpowers/specs/2026-10-20-recentre-absorb-yaw-design.md: with body-follows-head
// on, a runtime recentre would turn the view AND the body by its angle in one update (the head yaw h is read in appSpace). This
// recreates appSpace turned so the head keeps its yaw; the position reset (the runtime's new origin) is kept. Every consumer is
// handed s->appSpace per frame. Runs before the frame's first locate.
// Exact pose (poseValid): absorbed at the first frame past changeTime. No pose (VDXR, INV-300): the move can land a frame BEFORE
// the event (run 2: VDXR reads the flag only in xrWaitFrame) or with it (run 2's lean holds), so the absorb looks for the
// impossible one-frame head step in s->headRing from changeTime - kLookbackNs to now (this frame's head located here and pushed),
// and keeps looking on later frames up to changeTime + kWatchNs (Jay 2026-10-20: base it on the HMD direction at the event, like the
// HUD's recentre). COST: event-driven - one xrLocateSpace per frame while an event waits (<= 250 ms), then one create + destroy.
static void ApplyAbsorb(Session* s)
{
    static int absorbed = 0;
    ::dlvr::recentre::Pending& w = s->absorb;
    const XrTime t = s->frameState.predictedDisplayTime;
    ::dlvr::recentre::Step step{ 0.0f, 0.0f, 0 };
    if (!w.poseValid) {
        if (::dlvr::recentre::WatchExpired(w, static_cast<long long>(t))) {
            if (s->absorbCount < 200) {
                ++s->absorbCount;
                // [PROBE: RECENTRE] gate=comfort_body_follows_head  site=HEADSET  log-only=no
                LOG("[RECENTRE] NO STEP from 250 ms before the event to 250 ms after (%d frames watched, %d head samples held, "
                    "yaw step > %.0f deg or position step > %.2f m never seen) - nothing absorbed", w.framesWatched,
                    s->headRing.n, ::dlvr::recentre::kJumpYawDeg, ::dlvr::recentre::kJumpPosM);
            }
            w = ::dlvr::recentre::Pending{};
            return;
        }
        ++w.framesWatched;
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (s->everBegan && s->viewSpace != XR_NULL_HANDLE && xrLocateSpace_ &&
            XR_SUCCEEDED(xrLocateSpace_(s->viewSpace, s->appSpace, t, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0 &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
            ::dlvr::recentre::HeadPush(s->headRing, static_cast<long long>(t),
                                       ::dlvr::recentre::YawDegAboutY(&loc.pose.orientation.x), loc.pose.position.x,
                                       loc.pose.position.y, loc.pose.position.z);
        }
        if (!::dlvr::recentre::FindRecentreStep(s->headRing, w.changeTime - ::dlvr::recentre::kLookbackNs, step)) {
            return;   // not landed yet - look again next frame
        }
    }
    const ::dlvr::recentre::Pending p = w;
    w = ::dlvr::recentre::Pending{};
    const float posStep = step.posStepM;
    const float jump = p.poseValid ? -p.poseYawDeg : step.yawStepDeg;
    const float newBase = ::dlvr::recentre::NewBaseYaw(s->appBaseYawDeg, p.poseValid, p.poseYawDeg, step.yawStepDeg, 0.0f);
    XrReferenceSpaceCreateInfo sp{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    sp.referenceSpaceType = s->appSpaceType;
    float q[4];
    ::dlvr::recentre::QuatFromYawDeg(newBase, q);
    sp.poseInReferenceSpace.orientation = XrQuaternionf{ q[0], q[1], q[2], q[3] };
    sp.poseInReferenceSpace.position = XrVector3f{ 0.0f, 0.0f, 0.0f };
    XrSpace fresh = XR_NULL_HANDLE;
    const XrResult cr = xrCreateReferenceSpace_(s->session, &sp, &fresh);
    const bool ok = XR_SUCCEEDED(cr);
    if (ok) {
        if (s->appSpace) {
            xrDestroySpace_(s->appSpace);
        }
        s->appSpace = fresh;
        s->appBaseYawDeg = newBase;
        s->appBasePos = XrVector3f{ 0.0f, 0.0f, 0.0f };
        ::dlvr::recentre::HeadClear(s->headRing);   // its samples are in the old appSpace
    }
    ++absorbed;
    if (s->absorbCount < 200) {
        ++s->absorbCount;
        // [PROBE: RECENTRE] gate=comfort_body_follows_head  site=HEADSET  log-only=no
        LOG("[RECENTRE] ABSORBED #%d: type %d, changeTime %lld, recentre yaw %+.2f deg from %s (head step at %+.1f ms vs the "
            "event's changeTime - negative = before it; position step %.3f m; found on frame %d of the watch), base yaw now %+.2f "
            "deg | appSpace recreated %s (result %d)",
            absorbed, static_cast<int>(s->appSpaceType), p.changeTime, jump,
            p.poseValid ? "poseInPreviousSpace" : "the head step",
            p.poseValid ? 0.0 : static_cast<double>(step.t - p.changeTime) / 1.0e6, posStep, p.framesWatched,
            ok ? newBase : s->appBaseYawDeg, ok ? "OK" : "FAILED - kept the old one", static_cast<int>(cr));
    }
}

// [LOADBEARING: CENTRE] D-284 / spec 2026-10-26-roomscale-physical-crouch-design §2.1: moves appSpace's origin to `newBasePos` (the
// reference space, same type, same base yaw - position only). Every consumer is handed s->appSpace per frame (as ApplyAbsorb).
// Called once per centre (and by the roomscale plan's later tasks). COST: one xrCreateReferenceSpace + one xrDestroySpace - on
// VDXR a `new` + a set insert / erase under one mutex, no OVR call (FINDINGS-ROOMSCALE-STATIC S1); SteamVR's cost not read.
// clearHeadRing false (the bleed): the ring keeps its samples - a bleed step is at most 1.0 m/s x one frame, far under the
// absorb's 0.10 m position-step test, so the look-back still reads a real recentre and never mistakes a bleed step for one.
static bool ShiftAppSpaceTo(Session* s, const XrVector3f& newBasePos, XrResult* outResult, bool clearHeadRing = true)
{
    XrReferenceSpaceCreateInfo sp{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    sp.referenceSpaceType = s->appSpaceType;
    float q[4];
    ::dlvr::recentre::QuatFromYawDeg(s->appBaseYawDeg, q);
    sp.poseInReferenceSpace.orientation = XrQuaternionf{ q[0], q[1], q[2], q[3] };
    sp.poseInReferenceSpace.position = newBasePos;
    XrSpace fresh = XR_NULL_HANDLE;
    const XrResult cr = xrCreateReferenceSpace_(s->session, &sp, &fresh);
    if (outResult) { *outResult = cr; }
    if (XR_FAILED(cr)) {
        return false;
    }
    if (s->appSpace) {
        xrDestroySpace_(s->appSpace);
    }
    s->appSpace = fresh;
    s->appBasePos = newBasePos;
    if (clearHeadRing) {
        ::dlvr::recentre::HeadClear(s->headRing);   // its samples are in the old appSpace
    }
    return true;
}

// Centre (D-284): runs in BeginFrameIfNeeded after the absorb, before the frame's first locate, only while a centre is pending.
// The head's x/z is taken AFTER the neck pivot, so the x/z we REPORT read 0 once it is done. Yaw is untouched. Height: only at the
// SESSION START and only when the STAGE floor says STANDING (centre::HeightDue; trace 20261117_1627_centre_height_standing, Jay
// 2026-11-17 option B - players loaded in "extremely tall") the origin moves to the head's raw height, as ovr_RecenterTrackingOrigin
// does; otherwise height is kept.
// COST: one xrLocateSpace per frame while a centre is pending (at most kGiveUpNs = 5 s per session-state change), then
// ShiftAppSpaceTo's create + destroy once, plus one xrLocateSpace(VIEW, STAGE) at a session-start centre. Nothing while none is pending.
static void ApplyCentre(Session* s)
{
    ::dlvr::centre::Tracker& c = s->centre;
    const XrTime t = s->frameState.predictedDisplayTime;
    const long long now = static_cast<long long>(t);
    if (::dlvr::centre::GiveUp(c, now)) {
        // [PROBE: CENTRE] gate=none  site=HEADSET  log-only=no
        LOG("[CENTRE] GAVE UP %s - no centre in %.1f s (absorb waiting %d frames, head untracked %d frames) - nothing moved",
            ::dlvr::centre::ReasonName(c.pending), static_cast<double>(::dlvr::centre::kGiveUpNs) / 1.0e9, s->centreAbsorbWait,
            s->centreUntracked);
        ::dlvr::centre::Done(c);
        return;
    }
    if (now < c.armedAt + ::dlvr::centre::kSettleNs) {
        return;   // settling - nothing located yet
    }
    if (s->absorb.has) {
        ++s->centreAbsorbWait;
        return;
    }
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    const bool tracked = s->viewSpace != XR_NULL_HANDLE && xrLocateSpace_ &&
                         XR_SUCCEEDED(xrLocateSpace_(s->viewSpace, s->appSpace, t, &loc)) &&
                         (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                         (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
    if (!::dlvr::centre::Due(c, now, false, tracked)) {
        ++s->centreUntracked;
        return;
    }
    XrVector3f rep = loc.pose.position;
    NeckPivotCompute(loc.pose, rep, nullptr);   // off / refused -> rep = the raw head position
    const float base[3] = { s->appBasePos.x, s->appBasePos.y, s->appBasePos.z };
    float out[3];
    ::dlvr::centre::ComposeBasePos(s->appBaseYawDeg, base, rep.x, rep.z, out);
    // Height (trace 20261117_1627): the eye above the STAGE floor, located once, only at the session start.
    const bool atStart = c.pending == ::dlvr::centre::Reason::SessionStart;
    XrSpaceLocation fl{ XR_TYPE_SPACE_LOCATION };
    const bool floorOk = atStart && s->stageSpace != XR_NULL_HANDLE &&
                         XR_SUCCEEDED(xrLocateSpace_(s->viewSpace, s->stageSpace, t, &fl)) &&
                         (fl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    const float eyeFloor = floorOk ? fl.pose.position.y : -1.0f;
    const bool height = ::dlvr::centre::HeightDue(c.pending, floorOk, eyeFloor);
    if (height) {
        out[1] = base[1] + loc.pose.position.y;   // the raw head height, as ovr_RecenterTrackingOrigin composes it
    }
    XrResult cr = XR_SUCCESS;
    const bool ok = ShiftAppSpaceTo(s, XrVector3f{ out[0], out[1], out[2] }, &cr);
    if (ok) {
        ::dlvr::HolsterBodyReset();        // holster body-lock: its facing + latches were in the old appSpace
        ::dlvr::uipanel::HudRecentre();    // the HUD panel is fixed in appSpace - latch it in front of the head again
        if (height) {
            s->floorTick = 0;              // appFloorY changed with the origin - re-locate it at the next locate
        }
    }
    const char* heightWhy = height    ? "HEIGHT SET to the head (standing)"
                            : !atStart ? "height kept (headset back on)"
                            : !floorOk ? "height kept (no floor located)"
                                       : "height kept (seated: eye not above 1.38 m)";
    // [PROBE: CENTRE] gate=none  site=HEADSET  log-only=no
    LOG("[CENTRE] DONE %s: head was (%+.3f, %+.3f) m from the origin (%.3f m, after the neck pivot), head y %+.3f m, eye %.3f m above "
        "the floor (-1 = not located) - %s, base pos now (%+.3f, %+.3f, %+.3f), base yaw %+.2f deg unchanged, waited %.0f ms (absorb "
        "%d frames, untracked %d frames) | appSpace recreated %s (result %d)",
        ::dlvr::centre::ReasonName(c.pending), rep.x, rep.z, std::sqrt(rep.x * rep.x + rep.z * rep.z),
        static_cast<double>(loc.pose.position.y), static_cast<double>(eyeFloor), heightWhy,
        ok ? out[0] : s->appBasePos.x, ok ? out[1] : s->appBasePos.y, ok ? out[2] : s->appBasePos.z, s->appBaseYawDeg,
        static_cast<double>(now - c.armedAt) / 1.0e6, s->centreAbsorbWait, s->centreUntracked,
        ok ? "OK" : "FAILED - kept the old one", static_cast<int>(cr));
    ::dlvr::centre::Done(c);
}

// [LOADBEARING: BLEED] roomscale-off drift bleed (spec 2026-10-26-roomscale-physical-crouch-design §2.2, plan Task 6): while the left
// stick moves the character, appSpace is moved toward the head by at most roomscale_bleed_mps x the frame - position only, x/z only,
// base yaw unchanged - so real steps stop leaving the view off the body. Standing still and leaning are never touched. Runs after
// the centre, in BeginFrameIfNeeded, so no layer is built on a destroyed handle. Not while a centre or an absorb is pending, nor in
// a menu (ours, the calibration screen, the game's main / in-game menus: the stick moves a cursor there). The HUD panel moves with
// the body (Jay 2026-10-27: "Move with the body" - its latch stays in appSpace); the holsters' body frame needs nothing (its
// anchor is the neck, re-taken every poll); the head ring is kept (see ShiftAppSpaceTo).
// COST per frame: off / stick still = two compares + one stick read; walking = one xrLocateSpace + NeckPivotCompute, and while
// the head is off the body one create + destroy (ShiftAppSpaceTo, cheap on VDXR - S1).
static void ApplyBleed(Session* s)
{
    const long long now = static_cast<long long>(s->frameState.predictedDisplayTime);
    if (!s->bleedSaid) {
        s->bleedSaid = true;
        s->bleedWinStartNs = now;
        // [PROBE: BLEED] gate=roomscale_bleed_mps  site=HEADSET  log-only=no
        LOG("[BLEED] ARMED (logged unconditionally at the first frame) - roomscale_bleed_mps %.2f m/s (0 = off), stick dead zone "
            "%.2f, smallest shift 1 mm | runs only while the left stick moves the character; never standing still",
            static_cast<double>(RoomscaleBleedMps), static_cast<double>(::dlvr::roomscale::kStickDead));
    }
    float sx = 0.0f, sy = 0.0f;
    // roomscale_enable on: roomscale owns the offset (ApplyRoomscale) - the bleed stands aside (spec §2.2: the bleed is roomscale OFF)
    const bool walking = !::dlvr::RoomscaleEnable && RoomscaleBleedMps > 0.0f &&
                         ::dlvr::GetControllerStick(::dlvr::kLeftHand, sx, sy) &&
                         ::dlvr::roomscale::StickMoving(sx, sy);
    if (walking) {
        unsigned menuAge = 0;
        const int ms = ::dlvr::uipin::PublishedMenuState(&menuAge);
        const bool menu = ::dlvr::OverlayVisible() || ::dlvr::CalibScreenActive() || ((ms == 1 || ms == 2) && menuAge <= 250);
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (s->centre.pending != ::dlvr::centre::Reason::None) {
            ++s->bleedSkipCentre;
        } else if (s->absorb.has) {
            ++s->bleedSkipAbsorb;
        } else if (menu) {
            ++s->bleedSkipMenu;
        } else if (!(s->viewSpace != XR_NULL_HANDLE && xrLocateSpace_ &&
                     XR_SUCCEEDED(xrLocateSpace_(s->viewSpace, s->appSpace, s->frameState.predictedDisplayTime, &loc)) &&
                     (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                     (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0)) {
            ++s->bleedSkipNoHead;
        } else {
            XrVector3f rep = loc.pose.position;
            NeckPivotCompute(loc.pose, rep, nullptr);   // the x/z the engine is told (as the centre)
            s->bleedOffsetM = std::sqrt(rep.x * rep.x + rep.z * rep.z);
            const double dtS = static_cast<double>(s->frameState.predictedDisplayPeriod) / 1.0e9;
            float shX = 0.0f, shZ = 0.0f;
            ::dlvr::roomscale::BleedStep(rep.x, rep.z, RoomscaleBleedMps, static_cast<float>(dtS), shX, shZ);
            const float shM = std::sqrt(shX * shX + shZ * shZ);
            if (shM >= 0.001f) {
                const float base[3] = { s->appBasePos.x, s->appBasePos.y, s->appBasePos.z };
                float out[3];
                ::dlvr::centre::ComposeBasePos(s->appBaseYawDeg, base, shX, shZ, out);
                XrResult cr = XR_SUCCESS;
                if (ShiftAppSpaceTo(s, XrVector3f{ out[0], out[1], out[2] }, &cr, false)) {
                    ++s->bleedFrames;
                    s->bleedTotalM += shM;
                } else {
                    ++s->bleedFail;
                }
            }
        }
    }
    if (now - s->bleedWinStartNs >= 5000000000LL) {
        if (s->bleedFrames || s->bleedSkipCentre || s->bleedSkipAbsorb || s->bleedSkipMenu || s->bleedSkipNoHead || s->bleedFail) {
            // [PROBE: BLEED] gate=roomscale_bleed_mps  site=HEADSET  log-only=no
            LOG("[BLEED] 5 s | frames bled %d, total %.3f m, offset now %.3f m (-1 = not located while walking) | skipped: centre %d "
                "absorb %d menu %d no-head %d | recreate FAILED %d | base pos (%+.3f, %+.3f), base yaw %+.2f deg",
                s->bleedFrames, s->bleedTotalM, static_cast<double>(s->bleedOffsetM), s->bleedSkipCentre, s->bleedSkipAbsorb,
                s->bleedSkipMenu, s->bleedSkipNoHead, s->bleedFail, static_cast<double>(s->appBasePos.x),
                static_cast<double>(s->appBasePos.z), static_cast<double>(s->appBaseYawDeg));
        }
        s->bleedWinStartNs = now;
        s->bleedFrames = s->bleedSkipCentre = s->bleedSkipAbsorb = s->bleedSkipMenu = s->bleedSkipNoHead = s->bleedFail = 0;
        s->bleedTotalM = 0.0;
        s->bleedOffsetM = -1.0f;
    }
}

// [LOADBEARING: ROOMSCALE] roomscale (plan 2026-10-27 Task 7b; spec §2.2): the render-thread half. Each frame: the head's x/z offset
// from the appSpace origin (after the neck pivot, as the centre / bleed), the world share of body movement ROOMSCALE caused since
// the last frame (roomscale.cpp's pre-step detour, never stick walking) turned into the app frame by the inverse of HandWorld's
// verified map (looting.cpp:208-229; run 4a), roomscale_logic.h Update -> appSpace shifted by its shift (position only, base yaw
// kept), and its command handed to the detour as a world velocity (Ry(yaw) * cmd / kCatchS, capped). Stand-down (Jay's ruling 5 +
// the bleed's menus + a zombie grab): no command, the offset is a lean clamped to roomscale_lean_max_m, the rest absorbed. Not at all
// while a centre or an absorb is pending (they recreate appSpace themselves). Off: one bool test (and one command reset).
// COST per frame while on: one xrLocateSpace + NeckPivotCompute, one SRW-locked take, a few dozen flops, and while the view moves one
// create + destroy (ShiftAppSpaceTo; cheap on VDXR - FINDINGS S1).
static void ApplyRoomscale(Session* s)
{
    constexpr float kCatchS = 0.10f;   // the body is asked to close the remaining offset in this long
    constexpr float kMaxMps = 3.0f;    // never faster than a brisk real walk (a tracking glitch is not a step)
    struct Win {
        bool said = false, cmdLive = false;
        long long startNs = 0;
        int frames = 0, cmdFrames = 0, shifts = 0, offCentre = 0, offAbsorb = 0, offMenu = 0, offGroup = 0, offGrab = 0,
            offHead = 0, offYaw = 0, offLive = 0, lastGroup = -99;
        double moved = 0.0, shifted = 0.0, absorbed = 0.0, cmdSum = 0.0;
        float leanMax = 0.0f;
    };
    static Win w;
    const long long now = static_cast<long long>(s->frameState.predictedDisplayTime);
    if (!::dlvr::RoomscaleEnable) {
        if (w.cmdLive) { ::dlvr::RoomscaleSetCommand(0.0f, 0.0f); w.cmdLive = false; }
        return;
    }
    if (!w.said) {
        w.said = true;
        w.startNs = now;
        // [PROBE: ROOMSCALE] gate=roomscale_enable  site=HEADSET  log-only=no
        LOG("[ROOMSCALE] RENDER ARMED (logged unconditionally) - lean max %.2f m, catch %.2f s, speed cap %.1f m/s | moves the "
            "character on foot only (controller groups 0-6, 25); stands down in ladders / ziplines / vehicles / cutscenes / dialogue / "
            "grabs / menus", static_cast<double>(::dlvr::RoomscaleLeanMaxM), static_cast<double>(kCatchS),
            static_cast<double>(kMaxMps));
    }
    ++w.frames;
    float vx = 0.0f, vz = 0.0f;
    float rel = 0.0f;
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (s->centre.pending != ::dlvr::centre::Reason::None) {
        ++w.offCentre;
    } else if (s->absorb.has) {
        ++w.offAbsorb;
    } else if (!::dlvr::RoomscaleLive()) {
        ++w.offLive;
    } else if (!::dlvr::CamHookGetOpenXrToGameYaw(rel)) {
        ++w.offYaw;
    } else if (!(s->viewSpace != XR_NULL_HANDLE && xrLocateSpace_ &&
                 XR_SUCCEEDED(xrLocateSpace_(s->viewSpace, s->appSpace, s->frameState.predictedDisplayTime, &loc)) &&
                 (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                 (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0)) {
        ++w.offHead;
    } else {
        const float c = std::cos(rel);
        float sn = std::sin(rel);
        if (::dlvr::AimMirrorYaw) { sn = -sn; }   // the same switch HandWorld honours
        float wx = 0.0f, wz = 0.0f;
        ::dlvr::RoomscaleTakeMoved(&wx, &wz);
        // app = inverse of world = (c x + s z, -s x + c z)
        const float mx = c * wx - sn * wz, mz = sn * wx + c * wz;
        XrVector3f rep = loc.pose.position;
        NeckPivotNeckPoint(loc.pose, rep);   // INV-740: the whole arc removed - a head turn is not a step
        unsigned menuAge = 0;
        const int ms = ::dlvr::uipin::PublishedMenuState(&menuAge);
        const bool menu = ::dlvr::OverlayVisible() || ::dlvr::CalibScreenActive() || ((ms == 1 || ms == 2) && menuAge <= 250);
        const int group = ::dlvr::RoomscaleGroup();
        w.lastGroup = group;
        const bool grabbed = ::dlvr::BreakoutPlayerGrabbed();
        const bool standDown = menu || grabbed || !::dlvr::RoomscaleOnFoot(group);
        if (menu) { ++w.offMenu; } else if (grabbed) { ++w.offGrab; } else if (standDown) { ++w.offGroup; }
        const ::dlvr::roomscale::Out o =
            ::dlvr::roomscale::Update(::dlvr::roomscale::In{ rep.x, rep.z, mx, mz, standDown, ::dlvr::RoomscaleLeanMaxM });
        w.moved += std::sqrt(mx * mx + mz * mz);
        w.absorbed += o.absorbedM;
        if (o.leanM > w.leanMax) { w.leanMax = o.leanM; }
        const float shM = std::sqrt(o.shiftX * o.shiftX + o.shiftZ * o.shiftZ);
        if (shM >= 0.001f) {
            const float base[3] = { s->appBasePos.x, s->appBasePos.y, s->appBasePos.z };
            float out[3];
            ::dlvr::centre::ComposeBasePos(s->appBaseYawDeg, base, o.shiftX, o.shiftZ, out);
            XrResult cr = XR_SUCCESS;
            if (ShiftAppSpaceTo(s, XrVector3f{ out[0], out[1], out[2] }, &cr, false)) {
                ++w.shifts;
                w.shifted += shM;
            }
        }
        // INV-740: only the part of the remainder past kCmdDeadZoneM (the residual arc of a head that differs from the shipped n)
        float cmdX = o.cmdX, cmdZ = o.cmdZ;
        ::dlvr::roomscale::DeadZoneCommand(cmdX, cmdZ, ::dlvr::roomscale::kCmdDeadZoneM);
        // the command: app -> world (HandWorld's map), as a velocity, capped
        vx = (c * cmdX + sn * cmdZ) / kCatchS;
        vz = (-sn * cmdX + c * cmdZ) / kCatchS;
        const float v = std::sqrt(vx * vx + vz * vz);
        if (v > kMaxMps) { vx *= kMaxMps / v; vz *= kMaxMps / v; }
        if (v > 0.0f) { ++w.cmdFrames; w.cmdSum += (v > kMaxMps ? kMaxMps : v); }
    }
    ::dlvr::RoomscaleSetCommand(vx, vz);
    w.cmdLive = vx != 0.0f || vz != 0.0f;
    if (now - w.startNs >= 5000000000LL) {
        long steps = 0, commanded = 0, guarded = 0, changed = 0;
        ::dlvr::RoomscaleTakeStepStats(&steps, &commanded, &guarded, &changed);
        // [PROBE: ROOMSCALE] gate=roomscale_enable  site=HEADSET  log-only=no
        LOG_PERIODIC("[ROOMSCALE] 5 s | frames %d, commanding %d (mean %.2f m/s) | roomscale moved the body %.3f m, view shifted %.3f m in %d "
            "recreates, absorbed %.3f m, lean max %.3f m | stand-down frames: menu %d, grab %d, not on foot %d (group %d) | skipped: "
            "centre %d absorb %d no-hook %d no-yaw %d no-head %d | physics steps %ld (commanded %ld, guarded %ld, game changed its "
            "wanted %ld)",
            w.frames, w.cmdFrames, w.cmdFrames ? w.cmdSum / w.cmdFrames : 0.0, w.moved, w.shifted, w.shifts, w.absorbed,
            static_cast<double>(w.leanMax), w.offMenu, w.offGrab, w.offGroup, w.lastGroup, w.offCentre, w.offAbsorb, w.offLive,
            w.offYaw, w.offHead, steps, commanded, guarded, changed);
        const bool said = w.said, live = w.cmdLive;
        w = Win{};
        w.said = said;
        w.cmdLive = live;
        w.startNs = now;
    }
}

// xrWaitFrame + xrBeginFrame + xrLocateViews. The frame boundary; render thread
// only (see BootstrapFrame for the one exception, the very first frame).
// The caller must already hold the frame lock - see Unguard for why this no
// longer takes it itself.
bool BeginFrameIfNeeded(Session* s)
{
    if (!s || s->session == XR_NULL_HANDLE) {
        return false;
    }
    PollEvents(s);
    if (!s->running || s->frameActive) {
        return s->frameActive;
    }
    // The lock is dropped across the wait below, so a second thread can arrive
    // here in the meantime. Only the first frame is exposed to that - after it,
    // BootstrapFrame stops calling in - but "only the first frame" is exactly
    // when two threads racing to begin a frame would be hardest to diagnose.
    if (InterlockedCompareExchange(&g_inWait, 1, 0) != 0) {
        return false;
    }

    XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
    s->frameState = XrFrameState{ XR_TYPE_FRAME_STATE };
    const double waitStart = NowSeconds();
    XrResult wr;
    {
        // Wait unlocked. The engine keeps running - and keeps being answered
        // with the pose located for the frame that is still current, which is
        // what it would have been given anyway.
        Unguard letTheGameRun;
        wr = xrWaitFrame_(s->session, &wi, &s->frameState);
    }
    InterlockedExchange(&g_inWait, 0);
    const double waitMs = (NowSeconds() - waitStart) * 1000.0;
    s->waitSumMs += waitMs;
    ++s->waitCount;
    if (waitMs > s->waitMaxMs) {
        s->waitMaxMs = waitMs;
    }
    if (!XrOk(wr, "xrWaitFrame")) {
        return false;
    }
    // D-253: a queued runtime recentre is absorbed before anything locates a pose for this frame. Events are polled AGAIN here
    // (one xrPollEvent): a recentre the runtime applied during the unlocked wait would otherwise be seen only next frame, after
    // this frame's locate already read the moved head - the fallback would then measure a jump of ~0 and the snap would show.
    // A session that stopped in that poll begins no frame.
    PollEvents(s);
    if (!s->running) {
        return false;
    }
    if (s->absorb.has && ::dlvr::recentre::Due(s->absorb, static_cast<long long>(s->frameState.predictedDisplayTime))) {
        ApplyAbsorb(s);
    }
    // D-284: centre on the head at session start / headset back on - after the absorb, so the two never recreate appSpace
    // against each other (the centre waits while an absorb is queued). None pending: one compare.
    if (s->centre.pending != ::dlvr::centre::Reason::None) {
        ApplyCentre(s);
    }
    // Drift bleed while walking (roomscale_bleed_mps): after the centre, before the frame's first locate.
    ApplyBleed(s);
    // Roomscale (roomscale_enable): after the bleed (which stands aside while it is on). Off: one bool test.
    ApplyRoomscale(s);

    XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
    XrResult r = xrBeginFrame_(s->session, &bi);
    if (XR_FAILED(r) && r != XR_FRAME_DISCARDED) {
        XrOk(r, "xrBeginFrame");
        return false;
    }
    s->frameActive = true;
    s->everBegan = true;

    // A frame-boundary call, like the wait/begin above it - once per frame,
    // same thread. Locating the resulting controller spaces happens
    // separately, wherever the head pose gets located, since that part is
    // safe from any thread and does not need to happen right here.
    ControllersSync(s->session);

    // Deliberately does NOT locate views.
    //
    // It used to, and then the first worker query located them AGAIN - twice per
    // frame, at two different wall-clock moments, for the same predicted display
    // time. The runtime answers those with slightly different predictions
    // because the second sees fresher tracking data, so which pose the engine
    // ended up building its camera from depended on where its job system
    // happened to be. That is a per-frame coin toss of a fraction of a degree,
    // and a fraction of a degree of ROTATION is exactly the kind of error that
    // reads as jitter while translation stays smooth.
    //
    // Now the pose is sampled once, at the first moment the engine asks for it,
    // and every later query in that frame gets that same value. Which thread
    // asks first does not matter; that it is asked once does.
    // How far the display time moved. One interval is normal; two or more means
    // the runtime gave the previous frame away and this one is landing a whole
    // display period later than the last, which is where a smooth sequence
    // acquires a step.
    // The period comes from the runtime. The first version of this inferred it
    // as the smallest gap ever seen, which is the wrong statistic in exactly the
    // way that matters: one abnormally short gap at startup latched the estimate
    // near zero and then every ordinary frame counted as a long gap. The runtime
    // already reports the answer.
    const XrTime period = s->frameState.predictedDisplayPeriod;
    if (s->lastPredicted && period > 0 &&
        s->frameState.predictedDisplayTime > s->lastPredicted) {
        const XrTime d = s->frameState.predictedDisplayTime - s->lastPredicted;
        if (d > period + period / 2) {
            ++s->longGaps;
        }
    }
    s->lastPredicted = s->frameState.predictedDisplayTime;
    s->beganAt = NowSeconds();

    s->poseQueriesThisFrame = 0;
    InterlockedExchange(&g_headSampled, 0);
    s->locatedThisFrame = false;
    if (g_relocateOnQuery == 0) {
        LocateViews(s);
    }
    // >>> THE FRAME BOUNDARY LIVES HERE, AND ONLY HERE. <<< 2026-09-14.
    // Re-arms the camera hook's once-per-frame OpenXR->game yaw publish.
    //
    // IT WAS FIRST PUT IN LocateOncePerFrame AND NEVER RAN. That function
    // early-returns on `g_relocateOnQuery == 0`, which is the CONFIGURED MODE
    // (verified on disk AND in the layer's own config: dump), so the re-arm
    // never executed, the publish latched after its first call, and the yaw
    // FROZE - the arms stopped following the camera entirely.
    //
    // BeginFrameIfNeeded runs every frame in every mode, which
    // LocateOncePerFrame does not. Note the same early-return means
    // g_headSampled is never set to 1 in this mode either - so
    // HeadSampledThisFrame() is false 100% of the time BY CONSTRUCTION, which
    // is worth knowing before trusting the "100998 of 100998 stale" figure the
    // old head-change gate was designed around.
    CamHookNoteFrameBoundary();
    CamHookEyeAxisTick();
    CamHookViewPosTick();
    return true;
}

// One pose sample per frame, taken at the first LibOVR call of that frame.
//
// This is the whole of the policy: sample once, as early as the engine asks,
// and hand every later caller the same value. Locating twice - or once per
// query, as an earlier version did - hands different subsystems of the same
// frame slightly different heads.
void LocateOncePerFrame(Session* s)
{
    if (!s) {
        return;
    }
    if (g_relocateOnQuery == 0) {
        return;   // pose comes from the frame boundary only
    }
    ++s->poseQueriesThisFrame;
    if (g_relocateOnQuery == 2 || !s->locatedThisFrame) {
        LocateViews(s);
        s->locatedThisFrame = true;
    }
    // The camera hook levels using the head we last REPORTED, so it matters
    // whether the engine has asked for this frame's head before it builds this
    // frame's camera. See HeadSampledThisFrame.
    InterlockedExchange(&g_headSampled, 1);
    // NOT the yaw re-arm site. This function early-returns above when
    // g_relocateOnQuery == 0, which is the configured mode, so anything placed
    // here does not run. The re-arm is in BeginFrameIfNeeded - see its note.
}

// Job-system workers must NOT drive the frame loop.
//
// Measured: ovr_GetTrackingState and ovr_GetPredictedDisplayTime arrive from
// sixteen different threads, because Chrome Engine 6 runs its update on a job
// system and whichever worker picks up the camera task calls us. Only
// ovr_SubmitFrame stays on one thread - the render thread.
//
// So the frame is paced from ovr_SubmitFrame, which ends the current frame and
// immediately waits for and begins the next one. Workers only ever read the
// poses located there. Two things break otherwise:
//
//   - OpenXR requires xrBeginFrame/xrEndFrame to be externally synchronised.
//     Driving them from an arbitrary worker satisfies that only by luck; doing
//     them both on the render thread makes it structural.
//   - xrWaitFrame blocks for most of a frame interval. Calling it from a worker
//     while holding the frame lock parks every other worker behind it, which
//     turns a diagnostic lock into a game-wide stall.
//
// This only has to prime the very first frame, before any submit has happened.
void BootstrapFrame(Session* s)
{
    if (s && !s->everBegan) {
        BeginFrameIfNeeded(s);
    }
}

SwapChain* SC(ovrTextureSwapChain c)
{
    return reinterpret_cast<SwapChain*>(c);
}

// (CopyToMirror - a CopyResource of the whole eye texture into the mirror - REMOVED 2026-11-14: it refused on every submit, the eye
// images being TYPELESS and the mirror sRGB; the desktop mirror is deskmirror::Draw now, trace 20261114_1030_desktop_mirror_single_eye.)

// afw_rebuild: commits whose release was deferred to ovr_SubmitFrame, and uses of a chain while one was pending.
volatile LONG g_afwRbDeferred = 0, g_afwRbEarlyUse = 0;

// ([SWAPORD] - diag_swap_order's event ring and 5 s report, U-52 / INV-427: REMOVED 2026-10-07, audit section 4.)

// LibOVR's "current index" is implicit; OpenXR's is handed out by acquire. We
// acquire lazily on first use each frame and release on commit.
bool AcquireIfNeeded(SwapChain* c)
{
    if (!c || c->handle == XR_NULL_HANDLE || c->acquired) {
        return c && c->acquired;
    }
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XrOk(xrAcquireSwapchainImage_(c->handle, &ai, &c->index), "xrAcquireSwapchainImage")) {
        return false;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (!XrOk(xrWaitSwapchainImage_(c->handle, &wi), "xrWaitSwapchainImage")) {
        return false;
    }
    c->acquired = true;
    return true;
}

// ===== submit_drawn_image (U-52 / INV-429, EXPERIMENT, default 0) =====
// INV-429: the renderer RECORDS a frame (and asks for the image index) one hand-out ahead of REPLAYING it, so the frame being
// replayed at a commit / submit drew into the image of the hand-out BEFORE the newest; the old code released the newest -
// an image whose content was the composite of two frames earlier. Here the chain holds up to two images and each submit
// releases the oldest (OpenXR releases the oldest acquired image by definition), i.e. the one that frame drew.
// Locks: g_sdQLock guards q / nq / index / acquired AND every xr call on a chain in sdMode (OpenXR requires the swapchain to
// be externally synchronised - hand-outs run on worker threads, the release on the render thread); g_sdHandOutLock
// serialises hand-outs among themselves so two cannot both decide to acquire. Neither is held across xrEndFrame.
// COST: off -> one bool test per hand-out / commit / submit. On: one SRW lock per call; per frame one acquire + wait (as
// before, moved) and one release (as before, moved). The wait is timed (max per 5 s in the log).
SRWLOCK       g_sdQLock = SRWLOCK_INIT;
SRWLOCK       g_sdHandOutLock = SRWLOCK_INIT;
volatile LONG g_sdAcq = 0, g_sdAcqFail = 0, g_sdRepeat = 0, g_sdCap = 0, g_sdRelHeld2 = 0, g_sdRelHeld1 = 0, g_sdRelNone = 0;
volatile LONG g_sdSwitchOn = 0, g_sdSwitchOff = 0;
bool          g_sdFaultLatched = false;   // a runtime error left the mode; set until submit_drawn_image is unticked
volatile LONG g_sdWaitMaxUs = 0;

// The hand-out (ovr_GetTextureSwapChainCurrentIndex) of a chain in sdMode: a NEW image when nothing is held or a commit
// happened since the newest acquire (a new recorded frame), else the newest again. Returns the index to hand out.
uint32_t SdHandOut(SwapChain* c, bool* acquiredNew)
{
    *acquiredNew = false;
    AcquireSRWLockExclusive(&g_sdHandOutLock);
    AcquireSRWLockExclusive(&g_sdQLock);
    const int cap = c->images.size() > 2 ? 2 : 1;
    const bool want = c->nq == 0 || (c->commitSinceAcquire && c->nq < cap);
    if (!want) {
        if (c->commitSinceAcquire) { InterlockedIncrement(&g_sdCap); }   // a new frame, but two are already held
        else { InterlockedIncrement(&g_sdRepeat); }                      // the same recorded frame asked again
    }
    if (want && !c->sdFault) {
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XrOk(xrAcquireSwapchainImage_(c->handle, &ai, &idx), "xrAcquireSwapchainImage (submit_drawn_image)")) {
            const int at = c->nq++;
            c->q[at] = idx;
            c->qWaited[at] = false;
            c->commitSinceAcquire = false;
            c->index = c->q[0];
            c->acquired = true;
            *acquiredNew = true;
            InterlockedIncrement(&g_sdAcq);
            // Only the OLDEST acquired image may be waited, and only while no waited image is held: here that is the case
            // exactly when this is the only one held. Otherwise the submit waits it after releasing the drawn one.
            if (at == 0) {
                XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                if (XrOk(xrWaitSwapchainImage_(c->handle, &wi), "xrWaitSwapchainImage (submit_drawn_image, hand-out)")) {
                    c->qWaited[0] = true;
                } else {
                    c->sdFault = true;
                    InterlockedIncrement(&g_sdAcqFail);
                }
            }
        } else {
            c->sdFault = true;
            InterlockedIncrement(&g_sdAcqFail);
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        const LONG us = fq.QuadPart ? static_cast<LONG>((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart) : 0;
        if (us > g_sdWaitMaxUs) { g_sdWaitMaxUs = us; }
    }
    const uint32_t out = c->nq > 0 ? c->q[c->nq - 1] : c->index;
    ReleaseSRWLockExclusive(&g_sdQLock);
    ReleaseSRWLockExclusive(&g_sdHandOutLock);
    return out;
}

// Before releasing the oldest held image: wait it if it was never waited. VDXR refuses a release unless the front image was waited
// (swapchain.cpp: front != lastWaitedIndex -> CALL_ORDER_INVALID, nothing popped). Legal here: an unwaited front means no waited image
// is held (waited images always precede unwaited ones in q). Caller holds g_sdQLock.
void SdWaitFrontIfNeeded(SwapChain* c)
{
    if (c->nq <= 0 || c->qWaited[0]) { return; }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XrOk(xrWaitSwapchainImage_(c->handle, &wi), "xrWaitSwapchainImage (submit_drawn_image, before release)")) {
        c->qWaited[0] = true;
    } else {
        c->sdFault = true;
    }
}

// The texture this frame's eye composite drew into (the bootstrap's composite detector -> dlvr_SwapNoteExt 'D', on the
// render thread during the replay), cleared at each submit. nullptr = no composite seen since the last submit.
void* volatile g_sdDrawnTex = nullptr;
volatile LONG  g_sdPathDrawn = 0, g_sdPathFallback = 0, g_sdPathKeep = 0, g_sdDrawnNotHeld = 0, g_sdDropped = 0;

// The submit's release in sdMode (82cce37b released the oldest whenever ONE was held - with the recording one hand-out ahead
// that is the image just handed out, so it never left the old pairing: backups/20261005_submitdrawn_run1, 1-held 100 %).
// Now: if this frame's composite was seen and its image is held, release up to and including it (normally exactly one);
// otherwise release the oldest only while TWO are held (the record-ahead steady state, INV-429) and keep a single one.
// Returns how many were held before.
int SdReleaseDrawn(SwapChain* c)
{
    AcquireSRWLockExclusive(&g_sdQLock);
    const int before = c->nq;
    int drawnAt = -1;
    void* const dt = g_sdDrawnTex;
    g_sdDrawnTex = nullptr;
    if (dt) {
        for (int i = 0; i < c->nq; ++i) {
            if (c->q[i] < c->images.size() && c->images[c->q[i]].texture == dt) { drawnAt = i; break; }
        }
        if (drawnAt < 0) { InterlockedIncrement(&g_sdDrawnNotHeld); }
    }
    // v3 (model2.py "v4", checked against VDXR's own rules before the build): composite seen and its image held -> release up
    // to and including it; seen but NOT held (the switch-on transient: the game drew an image the old path already released)
    // -> release nothing; NO composite seen this frame -> release everything held = the old path for this one frame (never an
    // unwaited image left in front, never a stuck pair). 848769b4's "oldest of two" fallback is gone.
    const int n = drawnAt >= 0 ? drawnAt + 1 : (dt ? 0 : c->nq);
    InterlockedIncrement(drawnAt >= 0 ? &g_sdPathDrawn : (dt ? &g_sdPathKeep : &g_sdPathFallback));
    if (drawnAt > 0) { InterlockedAdd(&g_sdDropped, drawnAt); }   // older held images released unshown (should stay 0)
    for (int i = 0; i < n; ++i) {
        SdWaitFrontIfNeeded(c);   // a release is refused unless the front image was waited (VDXR swapchain.cpp)
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        ::dlvr::loadscreen::CaptureBeforeRelease(c->handle, c->q[0] < c->images.size() ? c->images[c->q[0]].texture : nullptr);
        if (!XrOk(xrReleaseSwapchainImage_(c->handle, &ri), "xrReleaseSwapchainImage (submit_drawn_image)")) { c->sdFault = true; }
        for (int k = 1; k < c->nq; ++k) { c->q[k - 1] = c->q[k]; c->qWaited[k - 1] = c->qWaited[k]; }
        --c->nq;
    }
    // The image the NEXT replay draws into is now the oldest held: wait it here (legal now - no waited image is held).
    if (c->nq > 0 && !c->qWaited[0] && !c->sdFault) {
        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = XR_INFINITE_DURATION;
        if (XrOk(xrWaitSwapchainImage_(c->handle, &wi), "xrWaitSwapchainImage (submit_drawn_image, after release)")) {
            c->qWaited[0] = true;
        } else {
            c->sdFault = true;
        }
    }
    if (c->nq > 0) { c->index = c->q[0]; }
    c->acquired = c->nq > 0;
    ReleaseSRWLockExclusive(&g_sdQLock);
    return before;
}

// The desktop mirror's source in sdMode (deskmirror.h): the image SdReleaseDrawn is about to hand the compositor, chosen the same way
// and WITHOUT consuming anything (g_sdDrawnTex stays for SdReleaseDrawn): the composite's image when it is held; nullptr when a
// composite was seen but its image is not held (nothing is released this submit); the newest held when no composite was seen (the
// release-everything fallback shows the newest). Every held image is still acquired, so reading it is legal. Render thread.
ID3D11Texture2D* SdShownTexture(SwapChain* c)
{
    ID3D11Texture2D* out = nullptr;
    AcquireSRWLockExclusive(&g_sdQLock);
    void* const dt = g_sdDrawnTex;
    if (dt) {
        for (int i = 0; i < c->nq; ++i) {
            if (c->q[i] < c->images.size() && c->images[c->q[i]].texture == dt) { out = c->images[c->q[i]].texture; break; }
        }
    } else if (c->nq > 0 && c->q[c->nq - 1] < c->images.size()) {
        out = c->images[c->q[c->nq - 1]].texture;
    }
    ReleaseSRWLockExclusive(&g_sdQLock);
    return out;
}

// Release EVERY held image of a chain in sdMode (leaving sdMode). Returns how many were held before.
int SdRelease(SwapChain* c, bool all)
{
    AcquireSRWLockExclusive(&g_sdQLock);
    const int before = c->nq;
    const int n = all ? c->nq : (c->nq > 0 ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        SdWaitFrontIfNeeded(c);   // 7a59e640 released an UNWAITED second image here at the switch-off: refused (-37), so VDXR kept it
                                  // and every later old-path release was one image off (Jay, block 3: "ghosted/doubled on everything")
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        ::dlvr::loadscreen::CaptureBeforeRelease(c->handle, c->q[0] < c->images.size() ? c->images[c->q[0]].texture : nullptr);
        XrOk(xrReleaseSwapchainImage_(c->handle, &ri), "xrReleaseSwapchainImage (submit_drawn_image)");
        for (int k = 1; k < c->nq; ++k) { c->q[k - 1] = c->q[k]; c->qWaited[k - 1] = c->qWaited[k]; }
        --c->nq;
    }
    if (c->nq > 0) { c->index = c->q[0]; }
    c->acquired = c->nq > 0;
    ReleaseSRWLockExclusive(&g_sdQLock);
    return before;
}

// [SUBDRAWN] 5 s tallies (cumulative; the wait max is per window). Behind diag_periodic_logs since 2026-10-07 (they rode
// diag_swap_order, removed then); the switch lines themselves are unconditional.
void SdReport()
{
    if (!::dlvr::DiagPeriodicLogs) { return; }
    static ULONGLONG s_next = 0;
    static bool s_said = false;
    const ULONGLONG now = GetTickCount64();
    if (s_said && now < s_next) { return; }
    s_next = now + 5000;
    const LONG w = g_sdWaitMaxUs;
    g_sdWaitMaxUs = 0;
    // [PROBE: SUBDRAWN] gate=submit_drawn_image + diag_periodic_logs (tallies)  site=HEADSET  log-only=yes
    LOG("[SUBDRAWN] %s: submit_drawn_image %d | new images acquired %ld (failed %ld), acquire+wait max %.2f ms this window | "
        "hand-outs given no new image: same frame asking again %ld, two already held %ld | submits with 2 held "
        "(recording one ahead) %ld, 1 held %ld, none held %ld | RELEASE PATH (v3): the composite's own image %ld, no composite "
        "seen -> released all = old path %ld, composite's image not held -> kept %ld | composite seen but its image not held "
        "%ld | older images released unshown %ld | switched on %ld, off (incl. runtime-error exits) %ld | acquire/wait "
        "failures = 'failed' above, must be 0 | diag_periodic_logs=1",
        s_said ? "5 s" : "FIRST (logged unconditionally)", SubmitDrawnImage ? 1 : 0, g_sdAcq, g_sdAcqFail,
        static_cast<double>(w) / 1000.0, g_sdRepeat, g_sdCap, g_sdRelHeld2, g_sdRelHeld1, g_sdRelNone, g_sdPathDrawn,
        g_sdPathFallback, g_sdPathKeep, g_sdDrawnNotHeld, g_sdDropped, g_sdSwitchOn, g_sdSwitchOff);
    s_said = true;
}

// ===== [HICCUP] (diag_hiccup, 2026-10-05) - head-turn hiccup / momentary double image (Jay; INV-326's "slight hitch/ghost
// ... part of the rendered view would double very momentarily", seen again at 90 Hz AFW off with the declaration on AND off
// and 0 late frames). The 600-frame windows and 5 s tallies average such single frames away, and the per-event
// "xrEndFrame blocked" line stops after 20 per session (LOG_FIRST) - every run but one today hit that cap.
// Per submitted frame, on the render thread, after xrEndFrame and the next xrWaitFrame: E = xrEndFrame held > 1.5 periods;
// G = the gap since the previous submit > 1.5 periods; S = the runtime's predicted display time stepped >= 1.5 periods
// (a display slot with no new frame of ours); J = the declared-vs-located orientation of view 0 jumped > 0.5 deg since the
// previous frame (the INV-326 fallback signature). Every event is COUNTED; event lines print at most 10 per second (the
// rest counted as unprinted); a 5 s summary splits each kind by head turning (yaw rate > 30 deg/s) vs not.
// COST: off -> one bool test per submit. On: two quaternion reads, an atan2, an acos and a few compares per frame.
namespace hiccup {
double    g_prevSubmit = 0.0, g_prevCorr = -1.0;
XrTime    g_prevDisp = 0;
float     g_prevYaw = 0.0f;
bool      g_havePrev = false, g_said = false;
LONG      g_frames = 0, g_turn = 0, g_ev[4] = {}, g_evTurn[4] = {}, g_unprinted = 0;
double    g_max[4] = {};
ULONGLONG g_sec = 0, g_next = 0;
int       g_secN = 0;
const char* const kKind[4] = { "E runtime held the submit", "G gap between game submits",
                               "S display slot without a new frame", "J declared-pose jump" };
}  // namespace hiccup

float HcYawDeg(const XrQuaternionf& q)
{
    // Heading of the view's forward axis (-Z) in the tracking space's horizontal plane.
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return atan2f(fx, -fz) * 57.29577951f;
}

double HcAngleDeg(const XrQuaternionf& a, const XrQuaternionf& b)
{
    double d = static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y +
               static_cast<double>(a.z) * b.z + static_cast<double>(a.w) * b.w;
    d = d < 0.0 ? -d : d;
    if (d > 1.0) { d = 1.0; }
    return 2.0 * acos(d) * 180.0 / 3.14159265358979;
}

void HiccupFrame(Session* s, double endMs, bool haveDecl, const XrQuaternionf& decl)
{
    if (!DiagHiccup || !s) { return; }
    using namespace hiccup;
    const double periodMs = s->frameState.predictedDisplayPeriod / 1e6;
    const ULONGLONG nowMs = GetTickCount64();
    if (!g_said) {
        g_said = true;
        g_next = nowMs + 5000;
        // [PROBE: HICCUP] gate=diag_hiccup  site=HEADSET  log-only=yes
        LOG("[HICCUP] FIRST (logged unconditionally): per-frame hiccup log armed - display period %.2f ms; events E (xrEndFrame "
            "> 1.5 periods), G (submit gap > 1.5 periods), S (display time step >= 1.5 periods), J (declared-pose jump > 0.5 "
            "deg); turning = head yaw rate > 30 deg/s; event lines capped at 10 per second, every event counted. "
            "diag_hiccup=1", periodMs);
    }
    if (periodMs <= 0.0) { return; }
    const double now = s->submitEnteredAt;
    const float yaw = HcYawDeg(s->headPose.orientation);
    const XrTime disp = s->frameState.predictedDisplayTime;
    const double corr = haveDecl ? HcAngleDeg(decl, s->views[0].pose.orientation) : -1.0;
    if (g_havePrev && now > g_prevSubmit) {
        const double gapMs = (now - g_prevSubmit) * 1000.0;
        float dy = yaw - g_prevYaw;
        while (dy > 180.0f) { dy -= 360.0f; }
        while (dy < -180.0f) { dy += 360.0f; }
        const double rate = fabs(static_cast<double>(dy)) / (gapMs / 1000.0);
        const double steps = (g_prevDisp > 0 && disp > g_prevDisp)
                                 ? static_cast<double>(disp - g_prevDisp) / 1e6 / periodMs : 0.0;
        const double jump = (corr >= 0.0 && g_prevCorr >= 0.0) ? fabs(corr - g_prevCorr) : 0.0;
        const bool turning = rate > 30.0;
        ++g_frames;
        if (turning) { ++g_turn; }
        const double val[4] = { endMs / periodMs, gapMs / periodMs, steps, jump };
        const bool hit[4] = { val[0] > 1.5, val[1] > 1.5, val[2] >= 1.5, val[3] > 0.5 };
        bool any = false;
        for (int k = 0; k < 4; ++k) {
            if (!hit[k]) { continue; }
            any = true;
            ++g_ev[k];
            if (turning) { ++g_evTurn[k]; }
            if (val[k] > g_max[k]) { g_max[k] = val[k]; }
        }
        if (any) {
            const ULONGLONG sec = nowMs / 1000;
            if (sec != g_sec) { g_sec = sec; g_secN = 0; }
            if (g_secN < 10) {
                ++g_secN;
                LOG("[HICCUP] %s%s%s%s| xrEndFrame %.2f ms (%.2f periods), submit gap %.2f ms (%.2f periods), display "
                    "time step %.2f periods, head yaw rate %.0f deg/s (%s), declared-vs-located %.2f deg (jump %.2f) | "
                    "period %.2f ms | diag_hiccup=1",
                    hit[0] ? "E " : "", hit[1] ? "G " : "", hit[2] ? "S " : "", hit[3] ? "J " : "", endMs, val[0], gapMs,
                    val[1], steps, rate, turning ? "TURNING" : "still", corr, jump, periodMs);
            } else {
                ++g_unprinted;
            }
        }
    }
    g_prevSubmit = now;
    g_prevYaw = yaw;
    g_prevDisp = disp;
    g_prevCorr = corr;
    g_havePrev = true;
    if (nowMs >= g_next) {
        g_next = nowMs + 5000;
        LOG("[HICCUP] 5 s: %ld frames (%ld while turning) | %s %ld (turning %ld, max %.2f periods) | %s %ld (turning %ld, max "
            "%.2f periods) | %s %ld (turning %ld, max step %.2f periods) | %s %ld (turning %ld, max %.2f deg) | event lines "
            "not printed (10/s cap) %ld | diag_hiccup=1",
            g_frames, g_turn, kKind[0], g_ev[0], g_evTurn[0], g_max[0], kKind[1], g_ev[1], g_evTurn[1], g_max[1],
            kKind[2], g_ev[2], g_evTurn[2], g_max[2], kKind[3], g_ev[3], g_evTurn[3], g_max[3], g_unprinted);
        g_frames = g_turn = g_unprinted = 0;
        for (int k = 0; k < 4; ++k) { g_ev[k] = g_evTurn[k] = 0; g_max[k] = 0.0; }
    }
}

}  // namespace

// ============================================================================
//  Lifecycle
// ============================================================================

namespace vsyncoff { void PatchEngineVsyncStore(); void PatchFrameLimiter(); void LogRecordOnce(); }   // defined with the rest of vr_force_vsync_off, below
namespace vrresblock { void PatchResolutionSetters(); }   // the video-options Apply cannot change the size in VR - defined after vsyncoff

OVR_PUBLIC_FUNCTION(ovrResult) ovr_Initialize(const ovrInitParams* params)
{
    LOG("--- ovr_Initialize (flags=%#x requestedMinor=%u) ---",
        params ? params->Flags : 0u, params ? params->RequestedMinorVersion : 0u);

    // vr_force_vsync_off (INV-379): the engine stores VSync = 2 at engine+0x2E63F2 right after THIS call returns
    // successfully, so the patch must land here - and the setting must be read first. LoadLayerConfig() is once-guarded;
    // loading it here is EARLIER than the FOV query / pad poll, and is logged so the ordering change is attributable.
    if (!g_configLoaded) {
        LoadLayerConfig();
        LOG("config: loaded from ovr_Initialize (vr_force_vsync_off needs it before the engine's VSync store).");
    }
    vsyncoff::PatchEngineVsyncStore();
    // Wrist HUD stage 0 (spec 2026-11-12-wrist-hud-design §10, INV-724): marker-gated (dlvr\wrist_probe.txt), log-only gamedll
    // import swaps + a HUD_DI / HUD-component read; starts at most once, its own thread waits for the gamedll.
    ::dlvr::wristprobe::Start();

    if (g_initialized) {
        return ovrSuccess;
    }

    char why[512] = {};
    if (!LoadOpenXrRuntime(why, sizeof(why))) {
        SetError(ovrError_Initialize, why);
        return ovrError_Initialize;
    }
    if (!ResolveGlobalFunctions()) {
        SetError(ovrError_Initialize, "could not resolve core OpenXR entry points");
        return ovrError_Initialize;
    }
    g_initialized = true;
    LOG("OpenXR runtime ready: %s", g_runtimePath);
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_Shutdown()
{
    LOG("ovr_Shutdown");
    g_initialized = false;
}

OVR_PUBLIC_FUNCTION(void) ovr_GetLastErrorInfo(ovrErrorInfo* errorInfo)
{
    if (errorInfo) {
        *errorInfo = g_lastError;
    }
}

OVR_PUBLIC_FUNCTION(const char*) ovr_GetVersionString()
{
    return "1.10.0 (dlvr ovr2xr)";
}

OVR_PUBLIC_FUNCTION(int) ovr_TraceMessage(int level, const char* message)
{
    LOG("[game trace %d] %s", level, message ? message : "");
    return 0;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_Create(ovrSession* pSession, ovrGraphicsLuid* pLuid)
{
    LOG("--- ovr_Create ---");
    if (!pSession) {
        return ovrError_InvalidParameter;
    }
    *pSession = nullptr;
    if (!g_initialized) {
        SetError(ovrError_NotInitialized, "ovr_Create before ovr_Initialize");
        return ovrError_NotInitialized;
    }

    auto* s = new Session();

    // XR_KHR_win32_convert_performance_counter_time lets us state a QPC moment
    // in the runtime's own time domain. Without it the two clocks we hand the
    // game are unrelated: ovr_GetPredictedDisplayTime is XrTime, which has an
    // unspecified epoch, while ovr_GetTimeInSeconds was QPC seconds since boot.
    // LibOVR requires those to be the same clock, because the game is entitled
    // to subtract one from the other and use the result as a prediction
    // interval.
    // The body-tracked elbow (rig_elbow_body_tracking): XR_FB_body_tracking is requested only when the runtime lists it, whatever
    // the setting (so the menu switch works live); the time extension stays LAST so the retry below still drops only it.
    const char* exts[3] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, nullptr, nullptr };
    uint32_t extCount = 1;
    BodyTrackAddExtension(exts, &extCount, 2);
    exts[extCount++] = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    ci.enabledExtensionCount = extCount;
    ci.enabledExtensionNames = exts;
    strcpy_s(ci.applicationInfo.applicationName, "Dying Light");
    strcpy_s(ci.applicationInfo.engineName, "Chrome Engine 6 (dlvr)");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    // The time extension is a convenience, not a requirement: if the runtime
    // does not have it, create the instance without it and fall back to a
    // measured offset between the two clocks.
    XrResult ir = xrCreateInstance_(&ci, &s->instance);
    if (XR_FAILED(ir)) {
        LOG("xrCreateInstance with %s failed (%d) - retrying without it",
            XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
            static_cast<int>(ir));
        ci.enabledExtensionCount = extCount - 1;
        ir = xrCreateInstance_(&ci, &s->instance);
    }
    if (!XrOk(ir, "xrCreateInstance")) {
        SetError(ovrError_Initialize, "xrCreateInstance failed");
        delete s;
        return ovrError_Initialize;
    }
    if (!ResolveInstanceFunctions(s->instance)) {
        SetError(ovrError_Initialize, "could not resolve instance entry points");
        delete s;
        return ovrError_Initialize;
    }

    // Which OpenXR runtime is this actually talking to, on every launch -
    // the single most useful line for triaging a report from a headset/
    // runtime combination never tested here (SteamVR vs. Virtual Desktop's
    // VDXR vs. Oculus/Meta's own runtime all answer this differently).
    {
        XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(xrGetInstanceProperties_(s->instance, &ip))) {
            LOG("runtime: %s %u.%u.%u", ip.runtimeName,
                XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                XR_VERSION_PATCH(ip.runtimeVersion));
            // [LOADBEARING: FOV-META-OWN-FOV] the runtime's name decides whether each view is cut to its own window; logged either way.
            g_runtimeStretchesFov = dlvr::eyefov::RuntimeStretchesToOwnFov(ip.runtimeName);
            g_runtimeIsSteamVr = dlvr::menuchord::RuntimeIsSteamVr(ip.runtimeName);
            LOG("symmetric_fov: file value %d -> using %d (runtime \"%s\" %s)", g_symmetricFov, SymmetricFovMode(),
                ip.runtimeName,
                g_runtimeStretchesFov
                    ? "= Meta's runtime, which shows the image over its own FOV - each eye is submitted as that runtime's own window"
                    : "shows the declared FOV");
        } else {
            LOG("runtime: xrGetInstanceProperties failed");
            g_runtimeStretchesFov = false;
            g_runtimeIsSteamVr = false;
            LOG("symmetric_fov: file value %d used as it is - the runtime's name could not be read", g_symmetricFov);
        }
    }

    // Optional, so resolved by hand rather than through the required-function
    // list, which fails the whole session if any name is missing.
    if (XR_FAILED(xrGetInstanceProcAddr_(
            s->instance, "xrConvertWin32PerformanceCounterToTimeKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&g_qpcToXrTime)))) {
        g_qpcToXrTime = nullptr;
    }
    g_timeInstance = s->instance;
    if (g_qpcToXrTime) {
        // Say plainly how far apart the two clocks were. This is the number that
        // decides whether the timestamps handed to the game were ever coherent.
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        XrTime t = 0;
        if (XR_SUCCEEDED(g_qpcToXrTime(s->instance, &c, &t))) {
            LOG("clock: QPC %.6f s, runtime %.6f s, offset %.3f ms - "
                "ovr_GetTimeInSeconds now reports the runtime's clock",
                NowSeconds(), XrTimeToSeconds(t),
                (NowSeconds() - XrTimeToSeconds(t)) * 1000.0);
        }
    } else {
        LOG("clock: %s unavailable - predicted display time and "
            "ovr_GetTimeInSeconds may not share an epoch",
            XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    }

    XrSystemGetInfo gi{ XR_TYPE_SYSTEM_GET_INFO };
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrOk(xrGetSystem_(s->instance, &gi, &s->systemId), "xrGetSystem")) {
        SetError(ovrError_NoHmd, "no HMD available to the OpenXR runtime");
        delete s;
        return ovrError_NoHmd;
    }
    {
        // The actual headset, from the runtime's own mouth - "Bigscreen
        // Beyond via SteamVR" and "Meta Quest 2" both show up here even
        // though ovr_* callers only ever see the LibOVR 1.x shim.
        XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
        if (XR_SUCCEEDED(xrGetSystemProperties_(s->instance, s->systemId, &sp))) {
            strcpy_s(g_xrSystemName, sp.systemName);
            LOG("system (openxr): \"%s\" (vendor 0x%04X), max swapchain %ux%u, "
                "%u layer(s), orientation tracking=%d position tracking=%d", sp.systemName,
                sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth,
                sp.graphicsProperties.maxSwapchainImageHeight,
                sp.graphicsProperties.maxLayerCount, sp.trackingProperties.orientationTracking,
                sp.trackingProperties.positionTracking);
        } else {
            LOG("system (openxr): xrGetSystemProperties failed");
        }
    }
    if (!QueryViewConfigs(s)) {
        SetError(ovrError_Initialize, "could not query view configuration");
        delete s;
        return ovrError_Initialize;
    }

    // The adapter LUID matters: the game picks its D3D adapter from it.
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (xrGetD3D11GraphicsRequirementsKHR_ &&
        XR_SUCCEEDED(xrGetD3D11GraphicsRequirementsKHR_(s->instance, s->systemId, &req))) {
        s->adapterLuid = req.adapterLuid;
        LOG("runtime adapter LUID %08lx:%08lx",
            static_cast<unsigned long>(req.adapterLuid.HighPart),
            static_cast<unsigned long>(req.adapterLuid.LowPart));
    }
    if (pLuid) {
        memset(pLuid, 0, sizeof(*pLuid));
        memcpy(pLuid, &s->adapterLuid, sizeof(LUID) <= sizeof(*pLuid) ? sizeof(LUID)
                                                                      : sizeof(*pLuid));
    }

    g_session = s;
    *pSession = reinterpret_cast<ovrSession>(s);
    vsyncoff::PatchFrameLimiter();   // VR confirmed here (flat fails above with no HMD); once per process
    vrresblock::PatchResolutionSetters();   // same: VR only, once per process, before any menu exists
    LOG("ovr_Create OK (session=%p)", s);
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_Destroy(ovrSession session)
{
    Session* s = S(session);
    LOG("ovr_Destroy(%p)", s);
    if (!s) {
        return;
    }
    ::dlvr::deskmirror::Shutdown();   // its views hold the eye images and the mirror: dropped before both are released below
    for (auto* c : s->chains) {
        if (c->handle) {
            xrDestroySwapchain_(c->handle);
        }
        delete c;
    }
    for (auto* m : s->mirrors) {
        if (m->tex) {
            m->tex->Release();
        }
        delete m;
    }
    if (s->appSpace) {
        xrDestroySpace_(s->appSpace);
    }
    ::dlvr::uipanel::Shutdown();
    ::dlvr::wristquad::Shutdown();   // wrist HUD stage 2: its swapchain belongs to the session
    ::dlvr::loadscreen::Shutdown();
    OverlayShutdown();
    if (s->viewSpace) {
        xrDestroySpace_(s->viewSpace);
    }
    if (s->stageSpace) {
        xrDestroySpace_(s->stageSpace);   // the holsters' floor (trace 20261116_1545_standing_posture)
        s->stageSpace = XR_NULL_HANDLE;
        s->floorOk = false;
    }
    BodyTrackShutdown();   // the body tracker belongs to the session
    if (s->session) {
        xrDestroySession_(s->session);
    }
    if (s->instance) {
        xrDestroyInstance_(s->instance);
    }
    if (s->device) {
        s->device->Release();
    }
    if (g_session == s) {
        g_session = nullptr;
    }
    delete s;
}

// ============================================================================
//  Descriptors
// ============================================================================

OVR_PUBLIC_FUNCTION(ovrHmdDesc) ovr_GetHmdDesc(ovrSession session)
{
    LOG_FIRST(3, "-> ovr_GetHmdDesc(%p)", session);
    ovrHmdDesc d{};
    Session* s = S(session);

    d.Type = s ? ovrHmd_CV1 : ovrHmd_None;
    strcpy_s(d.ProductName, "dlvr OpenXR HMD");
    strcpy_s(d.Manufacturer, "dlvr");
    d.VendorId = 0x2833;
    d.ProductId = 0x0031;
    strcpy_s(d.SerialNumber, "DLVR-0001");
    d.FirmwareMajor = 1;
    d.FirmwareMinor = 0;

    d.AvailableHmdCaps = 0;
    d.DefaultHmdCaps = 0;
    d.AvailableTrackingCaps = ovrTrackingCap_Orientation | ovrTrackingCap_Position;
    d.DefaultTrackingCaps = d.AvailableTrackingCaps;

    for (int i = 0; i < 2; ++i) {
        const char* src = "";
        // Mode 2 whatever symmetric_fov is (trace 20261125_1040): the engine's one centred game camera - culling, sun-shadow fit, light
        // lists - is built from this answer alone and must reach each eye's outer edge. What the eyes are DRAWN with is ovr_GetRenderDesc's.
        const ovrFovPort f = GameEyeFov(s, i, &src, 2);
        d.DefaultEyeFov[i] = f;
        d.MaxEyeFov[i] = f;

        ovrFovPort real{};
        if (RealEyeFov(s, i, real)) {
            LOG_FIRST(4, "   HmdDesc eye%d FOV (%s): runtime L=%.4f R=%.4f U=%.4f D=%.4f "
                         "-> given L=%.4f R=%.4f U=%.4f D=%.4f (the cull camera's superset; the eyes are drawn by [LIVEFOV]; symmetric_fov=%d)",
                      i, src, real.LeftTan, real.RightTan, real.UpTan, real.DownTan,
                      f.LeftTan, f.RightTan, f.UpTan, f.DownTan, SymmetricFovMode());
            // How much the symmetric superset costs, so the waste is a measured
            // number in the log rather than something to argue about.
            const float wasted = ((f.LeftTan + f.RightTan) * (f.UpTan + f.DownTan)) /
                                 ((real.LeftTan + real.RightTan) * (real.UpTan + real.DownTan));
            LOG_FIRST(2, "   eye%d symmetric superset covers the lens at %.0f%% of "
                         "the pixels an asymmetric render would need", i, wasted * 100.0f);
        } else {
            LOG_FIRST(4, "   HmdDesc eye%d FOV (%s): given L=%.4f R=%.4f U=%.4f D=%.4f "
                         "- no runtime FOV yet, hmd_cache.txt is written this run",
                      i, src, f.LeftTan, f.RightTan, f.UpTan, f.DownTan);
        }
    }

    // "Resolution of the full HMD screen (both eyes)". Derived from the same
    // clamped per-eye size so it cannot disagree with the other two.
    const ovrSizei eyeSize = EyeRenderSize(s, ovrEye_Left, 1.0f);
    d.Resolution.w = eyeSize.w * 2;
    d.Resolution.h = eyeSize.h;
    d.DisplayRefreshRate = 90.0f;
    LOG_FIRST(2, "   HmdDesc: resolution %dx%d, fov tans L%.3f R%.3f U%.3f D%.3f",
              d.Resolution.w, d.Resolution.h,
              d.DefaultEyeFov[0].LeftTan, d.DefaultEyeFov[0].RightTan,
              d.DefaultEyeFov[0].UpTan, d.DefaultEyeFov[0].DownTan);
    return d;
}

OVR_PUBLIC_FUNCTION(ovrSizei)
ovr_GetFovTextureSize(ovrSession session, ovrEyeType eye, ovrFovPort fov,
                      float pixelsPerDisplayPixel)
{
    (void)fov;
    Session* s = S(session);
    const ovrSizei size = EyeRenderSize(s, eye, pixelsPerDisplayPixel);
    LOG("ovr_GetFovTextureSize(eye=%d, ppd=%.2f) -> %dx%d  (runtime recommended %ux%u)",
        static_cast<int>(eye), pixelsPerDisplayPixel, size.w, size.h,
        s ? s->viewConfigs[0].recommendedImageRectWidth : 0u,
        s ? s->viewConfigs[0].recommendedImageRectHeight : 0u);

    // >>> TELL THE BOOTSTRAP THE REAL PER-EYE SIZE, THIS LAUNCH. <<<
    //
    // The DLSS upscale ratio is baked into render_loop.ppfx as explicit pixel
    // sizes, and the bootstrap computes them at DLL_PROCESS_ATTACH - before
    // anything here exists. A CACHED resolution cannot be correct: Virtual
    // Desktop's slider, SteamVR supersampling, a different headset or a changed
    // render_scale all move this number, and the stale launch is precisely the
    // one after the user changes it. Worse than imprecise - if the resulting
    // ratio leaves the detector's band, upscaling silently does not engage.
    //
    // THIS is the authoritative number: it is what the engine sizes its render
    // targets from, after pixelsPerDisplayPixel, render_scale and the max-dim
    // cap have all been applied. Sent once, on the first call, through the same
    // GetProcAddress channel as DlvrNoteEyeRender and DlvrSetDlssEnabled.
    //
    // Whether it lands in time is decided by the LOG, not by this comment:
    // [PAKGEN] prints what it wrote and [DLSSFLAT] prints what the engine used.
    {
        static bool s_told = false;
        if (!s_told && size.w > 0 && size.h > 0) {
            s_told = true;
            if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                typedef int (*RegenFn)(int, int);
                if (RegenFn fn = reinterpret_cast<RegenFn>(
                        reinterpret_cast<void*>(GetProcAddress(b, "DlvrRegenPakForOutput")))) {
                    const int rc = fn(size.w, size.h);
                    LOG("pak regen for %dx%d per eye: %s", size.w, size.h,
                        rc ? "accepted" : "declined (no marker, an override, or out of range)");
                } else {
                    LOG("pak regen: the bootstrap has no DlvrRegenPakForOutput - old xinput1_3.dll");
                }
            }
        }
    }
    return size;
}

// >>> early_eye_size (2026-10-17): THIS launch's per-eye size, for an open that comes BEFORE ovr_Create - the FALLBACK. <<<
//
// Called by the bootstrap (pakgen.cpp early_eye_size, default ON) at the engine's first open of DW\dlvr_pp.mth ONLY when the
// layer has not yet handed over its size. Measured (INV-592, runs 1-4): the open comes ~0.9 s AFTER ovr_Create, the size has
// been handed over, and this is not called - and when it WAS called (run 1) VDXR refused it (-10, LIMIT_REACHED: the session's
// instance is live). It is only for an open before ovr_Create, never seen. A SHORT-LIVED instance: create, get the HMD system,
// enumerate the stereo views, destroy - so ovr_Create later creates the session's own instance exactly as before. VDXR allows
// one instance at a time and resets itself on a successful destroy (its dispatcher calls ResetInstance(); public source, main).
// Sized by EyeSizeFromRecommended at ppd 1.00 - the value the engine passes to ovr_GetFovTextureSize (measured, the 14:13
// run 2026-10-17) - so ovr_GetFovTextureSize's own pak-regen line later checks this number against the engine's real call.
// Returns 1 + the size, or 0 + a reason. Makes no global change except loading the runtime DLL and the config, both of which
// ovr_Initialize does anyway (both once-guarded).
extern "C" __declspec(dllexport) int DlvrQueryEyeSizeEarly(int* outW, int* outH, char* why, int whyBytes)
{
    if (!outW || !outH || !why || whyBytes <= 0) {
        return 0;
    }
    *outW = *outH = 0;
    why[0] = '\0';
    const size_t wb = static_cast<size_t>(whyBytes);
    // [PROBE: EARLYSIZE] gate=early_eye_size (default ON; only called when no size was handed over - INV-592)  site=HEADSET  log-only=no
    LOG("[EARLYSIZE] query from the bootstrap (thread %lu): this launch's per-eye size, before ovr_Initialize",
        GetCurrentThreadId());
    if (!LoadOpenXrRuntime(why, wb)) {
        LOG("[EARLYSIZE] *** no runtime *** %s", why);
        return 0;
    }
    if (!ResolveGlobalFunctions()) {
        strncpy_s(why, wb, "could not resolve xrCreateInstance from the runtime", _TRUNCATE);
        LOG("[EARLYSIZE] *** %s ***", why);
        return 0;
    }

    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    strcpy_s(ci.applicationInfo.applicationName, "Dying Light");
    strcpy_s(ci.applicationInfo.engineName, "Chrome Engine 6 (dlvr)");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrInstance inst = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance_(&ci, &inst);
    if (XR_FAILED(r)) {
        LOG("[EARLYSIZE] xrCreateInstance with %s failed (%d) - retrying with no extension", XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
            static_cast<int>(r));
        ci.enabledExtensionCount = 0;
        r = xrCreateInstance_(&ci, &inst);
    }
    if (XR_FAILED(r)) {
        _snprintf_s(why, wb, _TRUNCATE, "xrCreateInstance failed (XrResult %d)", static_cast<int>(r));
        LOG("[EARLYSIZE] *** %s ***", why);
        return 0;
    }

    // Only the three calls this needs, into locals: ResolveInstanceFunctions would fail on this instance (no D3D11 session
    // functions are wanted) and would repoint the session's globals at a handle about to be destroyed.
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumViews = nullptr;
    PFN_xrDestroyInstance destroy = nullptr;
    xrGetInstanceProcAddr_(inst, "xrGetSystem", reinterpret_cast<PFN_xrVoidFunction*>(&getSystem));
    xrGetInstanceProcAddr_(inst, "xrEnumerateViewConfigurationViews", reinterpret_cast<PFN_xrVoidFunction*>(&enumViews));
    xrGetInstanceProcAddr_(inst, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&destroy));
    if (!destroy) {
        // Cannot release it, so it must not exist past here either way - say so loudly; ovr_Create will then hit the limit.
        strncpy_s(why, wb, "*** the runtime gave no xrDestroyInstance - the early instance is LEAKED ***", _TRUNCATE);
        LOG("[EARLYSIZE] %s", why);
        return 0;
    }

    int ok = 0;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrSystemGetInfo gi{ XR_TYPE_SYSTEM_GET_INFO };
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!getSystem || !enumViews) {
        strncpy_s(why, wb, "the runtime gave no xrGetSystem / xrEnumerateViewConfigurationViews", _TRUNCATE);
    } else if (XR_FAILED(r = getSystem(inst, &gi, &sys))) {
        _snprintf_s(why, wb, _TRUNCATE, "xrGetSystem failed (XrResult %d - is the headset connected / streaming?)",
                    static_cast<int>(r));
    } else {
        XrViewConfigurationView v[2]{};
        v[0].type = v[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        uint32_t count = 2;
        r = enumViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, v);
        if (XR_FAILED(r) || count < 2) {
            _snprintf_s(why, wb, _TRUNCATE, "xrEnumerateViewConfigurationViews failed (XrResult %d, %u views)",
                        static_cast<int>(r), count);
        } else {
            const ovrSizei sz = EyeSizeFromRecommended(v[0].recommendedImageRectWidth, v[0].recommendedImageRectHeight, 1.0f);
            *outW = sz.w;
            *outH = sz.h;
            ok = 1;
            LOG("[EARLYSIZE] view config: %ux%u recommended -> %dx%d per eye (ppd 1.00, render_scale %.3f, max dim %d)",
                v[0].recommendedImageRectWidth, v[0].recommendedImageRectHeight, sz.w, sz.h, g_renderScale, g_maxDim);
        }
    }
    const XrResult dr = destroy(inst);
    LOG("[EARLYSIZE] early instance destroyed: XrResult %d%s", static_cast<int>(dr),
        XR_FAILED(dr) ? " *** FAILED - ovr_Create's own instance may now be refused ***" : "");
    if (!ok) {
        LOG("[EARLYSIZE] *** NO SIZE *** %s", why);
    }
    return ok;
}

OVR_PUBLIC_FUNCTION(ovrEyeRenderDesc)
ovr_GetRenderDesc(ovrSession session, ovrEyeType eye, ovrFovPort fov)
{
    LOG_FIRST(4, "-> ovr_GetRenderDesc(eye=%d)", static_cast<int>(eye));

    // MILESTONE D / JITTER RECON, 2026-09-12. READ-ONLY, costs two increments.
    //
    // DLSS jitter is a sub-pixel translation of the projection. We do NOT own
    // the projection matrix - we supply FOV TANGENTS and the ENGINE builds the
    // matrix - so jitter through this route is possible only if the engine
    // re-reads the FOV EVERY FRAME. That has never been established, and
    // LOG_FIRST(4) above makes the existing log structurally incapable of
    // answering it: past four calls it is equally silent at 4 and at 400,000.
    //
    // AND NOTE WHAT THE BODY ALREADY DOES: `d.Fov = fov` ECHOES the caller's own
    // FOV back. So the engine arrives holding a FOV and is asking us to describe
    // it. A per-frame call therefore would NOT by itself prove the returned Fov
    // is what builds the projection - that is a separate question. Recorded here
    // so the two do not get conflated later.
    // MILESTONE D RECON v2: hand the eye to the BOOTSTRAP, which owns the D3D
    // hooks. Recon v1 proved the eye cannot be recovered from D3D resources -
    // both eyes share ONE (HDR source -> render target) pair - so the layer is
    // the only thing that knows it, and this is the channel milestone D would
    // use. Resolved once by GetProcAddress: an older bootstrap, or a flat run
    // where this layer never loads at all, simply produces no events.
    {
        static PFN_DlvrNoteEye s_noteEye = nullptr;
        static bool s_resolved = false;
        if (!s_resolved) {
            s_resolved = true;
            if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                s_noteEye = reinterpret_cast<PFN_DlvrNoteEye>(
                    GetProcAddress(b, "DlvrNoteEyeRender"));
            }
            LOG("[JITRECON] bootstrap eye handover %s",
                s_noteEye ? "resolved (DlvrNoteEyeRender)"
                          : "NOT resolved - no L/R events will reach the recon");

            // Marker, read once, beside the other DLSS markers.
            char dir[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, dir, sizeof(dir));
            if (char* slash = strrchr(dir, '\\')) { *slash = 0; }
            char marker[MAX_PATH] = {};
            _snprintf_s(marker, sizeof(marker), _TRUNCATE,
                        "%s\\dlvr\\dlss_jitter_test.txt", dir);
            g_jitterTest = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
            LOG("[JITTEST] %s (marker %s)",
                g_jitterTest ? "ENABLED - the returned FOV will be shaken deliberately"
                             : "off - create dlvr\\dlss_jitter_test.txt to test whether the "
                               "engine uses the FOV we return",
                marker);
        }
        if (s_noteEye) { s_noteEye(eye == ovrEye_Right ? 1 : 0); }
    }

    {
        static volatile LONG s_calls = 0;
        static volatile LONG s_lastReport = 0;
        const LONG n    = InterlockedIncrement(&s_calls);
        const LONG now  = static_cast<LONG>(GetTickCount() / 1000);
        const LONG last = s_lastReport;
        if (now - last >= 10 && InterlockedCompareExchange(&s_lastReport, now, last) == last) {
            LOG_PERIODIC("[JITRECON] ovr_GetRenderDesc called %ld time(s) total. %s", n,
                n > 200 ? "PER-FRAME - the engine re-reads the FOV, so a sub-pixel frustum shift "
                          "through ovrFovPort is a CANDIDATE jitter route (still to prove the "
                          "RETURNED Fov is what builds the projection)."
                        : "NOT PER-FRAME - the FOV is read at setup only, so jitter CANNOT come "
                          "through this path; it has to come from the engine-side projection "
                          "(CCamera::FixProjectionMatrix / IBaseCamera::GetProjectionMatrix).");
        }
    }

    ovrEyeRenderDesc d{};
    Session* s = S(session);
    Guard lock;   // reads s->views / s->posesValid
    int idx = (eye == ovrEye_Right) ? 1 : 0;

    d.Eye = eye;
    // [LOADBEARING: FOV-LIVE-EYE] the engine builds this eye's frustum from the Fov returned HERE (+2E5530); ovr_SubmitFrame declares the same.
    const ovrFovPort used = EyeFovForFrame(s, idx, fov);
    d.Fov = used;

    // ---- JITTER ROUTE TEST (dlss_jitter_test.txt) -------------------------
    //
    // THE QUESTION: does the engine build its projection from the Fov we RETURN
    // here? Recon proved this function is called per frame (~19,400 calls in
    // 90 s), which is necessary but NOT sufficient - the body above echoes the
    // CALLER's own fov straight back, so the engine arrives already holding
    // one, and a per-frame call says nothing about whether our return value is
    // used. Decidable, so decide it rather than assume it.
    //
    // THE TEST: apply a DELIBERATELY EXAGGERATED shift - kJitterTestPixels, not
    // the sub-pixel amount DLSS actually needs. A sub-pixel offset would be
    // invisible and the result ambiguous; ten pixels of alternating shake is
    // unmissable. **The magnitude is wrong ON PURPOSE** - this measures
    // reachability, not quality, exactly as the NGX smoke test did.
    //
    // A frustum shift is the same thing as a sub-pixel translation of the
    // projection: move LeftTan and RightTan TOGETHER by the same delta and the
    // frustum shears sideways without changing its width. delta in tangent
    // units = (total tan span) * pixels / width.
    if (g_jitterTest) {
        static volatile LONG s_phase = 0;
        const LONG ph = InterlockedIncrement(&s_phase);
        // Alternate sign per frame so it SHAKES rather than sitting offset -
        // a static shift could be mistaken for a calibration error.
        const float dir = (ph & 2) ? 1.0f : -1.0f;
        const ovrSizei sz = EyeRenderSize(s, eye, 1.0f);
        const float tanSpan = used.LeftTan + used.RightTan;
        if (sz.w > 0 && tanSpan > 0.0f) {
            const float delta = tanSpan * (kJitterTestPixels * dir) / static_cast<float>(sz.w);
            d.Fov.LeftTan  = used.LeftTan  + delta;
            d.Fov.RightTan = used.RightTan - delta;
        }
        static volatile LONG s_said = 0;
        if (InterlockedCompareExchange(&s_said, 1, 0) == 0) {
            LOG("[JITTEST] ARMED - shifting the RETURNED ovrFovPort by +/-%.0f pixels every "
                "frame. If the view visibly SHAKES, the engine builds its projection from our "
                "returned Fov and DLSS jitter can be injected here. If nothing moves, the "
                "return value is ignored and jitter must come from the engine-side projection "
                "(CCamera::FixProjectionMatrix / IBaseCamera::GetProjectionMatrix).",
                kJitterTestPixels);
        }
    }

    const ovrSizei eyeSize = EyeRenderSize(s, eye, 1.0f);
    d.DistortedViewport.Pos.x = 0;
    d.DistortedViewport.Pos.y = 0;
    d.DistortedViewport.Size = eyeSize;

    float tanH = used.LeftTan + used.RightTan;
    float tanV = used.UpTan + used.DownTan;
    d.PixelsPerTanAngleAtCenter.x = tanH > 0.0f ? d.DistortedViewport.Size.w / tanH : 1000.0f;
    d.PixelsPerTanAngleAtCenter.y = tanV > 0.0f ? d.DistortedViewport.Size.h / tanV : 1000.0f;

    // ovrEyeRenderDesc carries a translation only (SDK <= 1.16 ABI). If we have
    // located views, use the real inter-eye offset; otherwise assume a nominal
    // IPD split about the head centre.
    float half = (s ? s->ipd : (g_hmdCache.valid ? g_hmdCache.ipd : 0.064f)) * 0.5f;
    d.HmdToEyeOffset.x = (eye == ovrEye_Left) ? -half : half;
    d.HmdToEyeOffset.y = 0.0f;
    d.HmdToEyeOffset.z = 0.0f;
    if (s && s->posesValid) {
        const XrVector3f delta{ s->views[idx].pose.position.x - s->headPose.position.x,
                                s->views[idx].pose.position.y - s->headPose.position.y,
                                s->views[idx].pose.position.z - s->headPose.position.z };
        // Head-local is the constant (+/-ipd/2, 0, 0) LibOVR documents; the
        // tracking-space delta is only valid for the orientation it was read at.
        const XrVector3f use = (g_eyeOffsetSpace == 1)
                                   ? UnrotateBy(s->headPose.orientation, delta)
                                   : delta;
        d.HmdToEyeOffset.x = use.x;
        d.HmdToEyeOffset.y = use.y;
        d.HmdToEyeOffset.z = use.z;
    }
    // How often the game asks decides whether a tracking-space offset can ever
    // be right: read once and it is frozen at the startup orientation, read
    // every frame and it tracks. The count settles that without guessing.
    static unsigned calls = 0;
    if ((++calls % 600) == 1) {
        LOG_PERIODIC("ovr_GetRenderDesc call #%u (eye=%d, eye_offset_space=%d)",
            calls, static_cast<int>(eye), g_eyeOffsetSpace);
    }
    if (g_invertEyeOffset) {
        d.HmdToEyeOffset.x = -d.HmdToEyeOffset.x;
        d.HmdToEyeOffset.y = -d.HmdToEyeOffset.y;
        d.HmdToEyeOffset.z = -d.HmdToEyeOffset.z;
    }
    LOG_FIRST(4, "   RenderDesc eye=%d HmdToEyeOffset=(%.4f, %.4f, %.4f) invert=%d",
              static_cast<int>(eye), d.HmdToEyeOffset.x, d.HmdToEyeOffset.y,
              d.HmdToEyeOffset.z, g_invertEyeOffset ? 1 : 0);
    return d;
}

// ============================================================================
//  Tracking and timing
// ============================================================================

OVR_PUBLIC_FUNCTION(double) ovr_GetTimeInSeconds()
{
    return NowOnRuntimeClock();
}

OVR_PUBLIC_FUNCTION(double) ovr_GetPredictedDisplayTime(ovrSession session, long long frameIndex)
{
    static ThreadSet seen;
    Session* s = S(session);
    if (s) {
        Guard lock;
        NoteThread("ovr_GetPredictedDisplayTime", seen);
        BootstrapFrame(s);
        LocateOncePerFrame(s);
        // The engine passes its own frame counter here. If it is a real
        // increasing index it is the engine's frame identity, which would let us
        // key the pose to ITS frame rather than to our submit boundary - worth
        // knowing, so log the first few.
        LOG_FIRST(8, "   ovr_GetPredictedDisplayTime(frameIndex=%lld)",
                  static_cast<long long>(frameIndex));
        if (s->frameState.predictedDisplayTime) {
            return XrTimeToSeconds(s->frameState.predictedDisplayTime);
        }
    }
    return NowOnRuntimeClock();
}

OVR_PUBLIC_FUNCTION(ovrTrackingState)
ovr_GetTrackingState(ovrSession session, double absTime, ovrBool latencyMarker)
{
    (void)absTime;
    (void)latencyMarker;

    LOG_FIRST(3, "-> ovr_GetTrackingState");
    ovrTrackingState ts{};
    ts.HeadPose.ThePose = IdentityPose();
    ts.HandPoses[0].ThePose = IdentityPose();
    ts.HandPoses[1].ThePose = IdentityPose();
    ts.CalibratedOrigin = IdentityPose();

    Session* s = S(session);
    if (!s) {
        return ts;
    }
    static ThreadSet seen;
    const double lockWait = NowSeconds();
    Guard lock;
    {
        const double ms = (NowSeconds() - lockWait) * 1000.0;
        s->blockedSumMs += ms;
        ++s->blockedCount;
        if (ms > s->blockedMaxMs) {
            s->blockedMaxMs = ms;
        }
    }
    NoteThread("ovr_GetTrackingState", seen);
    BootstrapFrame(s);
    LocateOncePerFrame(s);

    // Horizon lock, applied to the pose we report rather than to the engine.
    //
    // The engine composes its own camera with this pose, so pre-multiplying a
    // rotation here lands inside that composition - which is how the game's
    // pitch and roll can be cancelled without writing a single byte into engine
    // memory. Nothing to race with the renderer, nothing to corrupt.
    XrPosef head = s->headPose;
    // HEAD NECK PIVOT (SPEC-HEAD-NECK-PIVOT-2026-10-01): POSITION only, from the RAW orientation - before the decouple
    // strip, horizon lock and the scanner probe, which change only the orientation we report.
    // [LOADBEARING: HEAD-NECK-PIVOT] with head_neck_pivot on, this is the position C08100 puts HSPINE1 at (INV-306) -
    // the body AND the eyecamera's parent. Off: head.position is untouched (today).
    {
        const ULONGLONG npNow = GetTickCount64();
        NeckPivotStats& np = g_np;
        if (!np.armedSaid) {
            np.armedSaid = true;
            np.winStartMs = npNow;
            // [PROBE: NECKPIVOT] gate=head_neck_pivot  site=HEADSET  log-only=yes
            LOG("[NECKPIVOT] ARMED (logged unconditionally at the first ovr_GetTrackingState) - head_neck_pivot %d, n "
                "(%+.1f %+.1f %+.1f) mm head frame | reported position = eye - (R_head - I) n while on; raw while off, "
                "poses invalid or |n| > 300 mm. Live switch. 5 s windows follow.",
                HeadNeckPivot ? 1 : 0, HeadNeckPivotXmm, HeadNeckPivotYmm, HeadNeckPivotZmm);
        }
        const int onNow = HeadNeckPivot ? 1 : 0;
        if (onNow != np.lastOn) {
            if (np.lastOn >= 0) {
                // [PROBE: NECKPIVOT] gate=head_neck_pivot  site=HEADSET  log-only=yes
                LOG("[NECKPIVOT] SWITCHED %s - n (%+.1f %+.1f %+.1f) mm", onNow ? "ON" : "OFF",
                    HeadNeckPivotXmm, HeadNeckPivotYmm, HeadNeckPivotZmm);
            }
            np.lastOn = onNow;
        }
        ++np.calls;
        if (!onNow) {
            ++np.offCalls;
        } else if (!s->posesValid) {
            ++np.invalid;
        } else {
            XrVector3f rep;
            bool refused = false;
            if (NeckPivotCompute(s->headPose, rep, &refused)) {
                const float ddx = rep.x - head.position.x, ddy = rep.y - head.position.y, ddz = rep.z - head.position.z;
                const float mm = 1000.0f * sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);
                head.position = rep;
                ++np.applied;
                np.sumMm += mm;
                if (mm > np.maxMm) { np.maxMm = mm; }
            } else if (refused) {
                ++np.refused;
            }
        }
        if (npNow - np.winStartMs >= 5000) {
            // [PROBE: NECKPIVOT] gate=head_neck_pivot  site=HEADSET  log-only=yes
            LOG_PERIODIC("[NECKPIVOT] window: applied %lld of %lld calls | off %lld, poses-invalid %lld, refused (|n| > 300 mm) %lld | "
                "|reported - eye| mean %.1f max %.1f mm | head_neck_pivot %d",
                np.applied, np.calls, np.offCalls, np.invalid, np.refused,
                np.applied ? static_cast<float>(np.sumMm / static_cast<double>(np.applied)) : 0.0f, np.maxMm, onNow);
            np.calls = np.applied = np.offCalls = np.invalid = np.refused = 0;
            np.sumMm = 0.0; np.maxMm = 0.0f;
            np.winStartMs = npNow;
        }
    }
    // HEAD-POSITION FOLD (SPEC-HEAD-POSITION-FOLD-2026-10-19): gamedll +C08100 puts HSPINE1 (the body's spine, and the eyecamera
    // = the camera position) at the head position we report rotated by the BODY frame (INV-306). Body-follows-head turns that frame
    // by `folded` (INV-262 addendum: "THE LINK, CLOSED") while tracking space does not, so the offset lands `folded` off-axis and the
    // spine bends - Jay: "More so leaning". Turning the report by +folded about +Y cancels the engine's extra turn. Position only,
    // x/z only, after the neck pivot; the rig and hands use the real head pose (GetHeadPose / NeckPivotReportedPos), unchanged.
    // folded 0 (switch off from launch, flat, nothing folded) -> skipped -> today's report bit for bit. COST: one interlocked read
    // per call (job workers, under the frame lock), one sincos when folded.
    // [LOADBEARING: head-position fold] with comfort_body_follows_head=1 and the decouple active, the torso stands upright only
    //      because of this (S-61).
    if (HeadYawDecoupleActive()) {
        const float folded = HeadFollowFoldedDeg();
        if (folded != 0.0f) {
            headfollow::ReportedHeadPosForFold(folded, head.position.x, head.position.z);
        }
    }
    // PHYSICAL CROUCH, option A (Jay 2026-10-27 "Go ahead with A", after run 6): +C08100 puts HSPINE1 at this reported offset
    // (INV-306), so a real crouch on top of the game's crushed the spine to ~10 %. While the game is crouched, the real drop below
    // the learned standing neck height is taken back out, up to the game's own drop - the engine sees a level head and its own
    // crouch, the regular crouch's body and view. y only, after the neck pivot (the same neck point physcrouch learns from).
    // [LOADBEARING: physical_crouch_enable] the head height the engine is given while crouched
    head.position.y += ::dlvr::PhysCrouchReportLiftY(head.position.y);
    // C1 (SPEC-HEAD-YAW-DECOUPLE): strip the head's yaw from what the ENGINE is told, so its look -
    // and with it the character's facing, movement and the game's shot direction - turns only by
    // the stick (INV-257/258: the HMD yaw delta and the stick are separate addends). camhook's C2
    // puts this exact yaw back on the rendered view. BEFORE horizon lock and the scanner probe:
    // horizon lock levels against the head we report, and stripping after the probe would erase
    // the probe's yaw wobble and break camera identification. Position is untouched.
    LatchHeadYawDecouple();
    XrQuaternionf stripped{ 0.0f, 0.0f, 0.0f, 1.0f };
    if (HeadYawDecoupleActive()) {
        const float hq[4] = { head.orientation.x, head.orientation.y,
                              head.orientation.z, head.orientation.w };
        float yq[4];
        CamYawOnly(hq, 0, yq);                     // OpenXR is Y-up: yaw about +Y
        stripped = XrQuaternionf{ yq[0], yq[1], yq[2], yq[3] };
        const XrQuaternionf inv{ -yq[0], -yq[1], -yq[2], yq[3] };
        head.orientation = QuatMul(inv, head.orientation);
    }

    g_lastReportedHead = head.orientation;
    g_lastReportedHeadPos = head.position;
    g_lastStrippedHeadYaw = stripped;

    ts.HeadPose.ThePose = ToOvr(head);
    {
        // FIX 1 (trace docs/traces/20261117_1316_rscam_fix1.md; BETA port 20261117_1600): record the position the engine's slot 7 may
        // store (as (-x, y, -z)), with the share the appSpace shift had taken - both only change inside BeginFrameIfNeeded under this lock.
        const float rep[3] = { ts.HeadPose.ThePose.Position.x, ts.HeadPose.ThePose.Position.y, ts.HeadPose.ThePose.Position.z };
        ::dlvr::RoomscaleNoteReported(rep);
    }
    ts.HeadPose.TimeInSeconds = XrTimeToSeconds(s->frameState.predictedDisplayTime);
    ts.StatusFlags = s->posesValid
                         ? (ovrStatus_OrientationTracked | ovrStatus_PositionTracked)
                         : 0;
    // Motion controllers are not wired up yet; report them as untracked rather
    // than feeding the game identity poses it might treat as real.
    ts.HandStatusFlags[0] = 0;
    ts.HandStatusFlags[1] = 0;
    return ts;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetSessionStatus(ovrSession session, ovrSessionStatus* status)
{
    LOG_FIRST(3, "-> ovr_GetSessionStatus");
    if (!status) {
        return ovrError_InvalidParameter;
    }
    memset(status, 0, sizeof(*status));

    Session* s = S(session);
    if (!s) {
        return ovrError_InvalidSession;
    }
    Guard lock;   // PollEvents drains the same event queue as the frame loop
    PollEvents(s);

    status->HmdPresent = ovrTrue;
    status->HmdMounted = ovrTrue;
    status->IsVisible = (s->state == XR_SESSION_STATE_VISIBLE ||
                         s->state == XR_SESSION_STATE_FOCUSED)
                            ? ovrTrue
                            : ovrFalse;
    status->ShouldQuit = s->shouldQuit ? ovrTrue : ovrFalse;
    status->ShouldRecenter = s->shouldRecenter ? ovrTrue : ovrFalse;
    status->DisplayLost = ovrFalse;
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SetTrackingOriginType(ovrSession session, ovrTrackingOrigin origin)
{
    Session* s = S(session);
    if (!s) {
        return ovrError_InvalidSession;
    }
    s->trackingOrigin = origin;
    LOG("ovr_SetTrackingOriginType(%d)", static_cast<int>(origin));
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrTrackingOrigin) ovr_GetTrackingOriginType(ovrSession session)
{
    Session* s = S(session);
    return s ? s->trackingOrigin : ovrTrackingOrigin_EyeLevel;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_RecenterTrackingOrigin(ovrSession session)
{
    Session* s = S(session);
    if (!s || s->session == XR_NULL_HANDLE) {
        return ovrError_InvalidSession;
    }
    // Recreate the reference space at the current head pose - the OpenXR
    // equivalent of LibOVR's recenter.
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (s->viewSpace && xrLocateSpace_ &&
        XR_SUCCEEDED(xrLocateSpace_(s->viewSpace, s->appSpace,
                                    s->frameState.predictedDisplayTime, &loc))) {
        XrReferenceSpaceCreateInfo sp{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        sp.referenceSpaceType = (s->trackingOrigin == ovrTrackingOrigin_FloorLevel)
                                    ? XR_REFERENCE_SPACE_TYPE_STAGE
                                    : XR_REFERENCE_SPACE_TYPE_LOCAL;
        // Keep yaw and position, drop pitch/roll so the horizon stays level. The head was located IN appSpace, which is the
        // reference space turned by appBaseYawDeg and moved to appBasePos (a recentre absorb, D-253, or an earlier recentre
        // here), so the new pose is composed onto that base - handing the in-appSpace pose straight to the reference space
        // would land turned by the base (Jay 2026-10-20: "Yes fix it").
        const float headYaw = ::dlvr::recentre::YawDegAboutY(&loc.pose.orientation.x);
        const float newBase = ::dlvr::recentre::ComposeBaseYaw(s->appBaseYawDeg, headYaw);
        float px = loc.pose.position.x, pz = loc.pose.position.z;
        ::dlvr::headfollow::ReportedHeadPosForFold(s->appBaseYawDeg, px, pz);   // rotate about +Y by the base
        XrPosef p = XrIdentity();
        p.position = XrVector3f{ s->appBasePos.x + px, s->appBasePos.y + loc.pose.position.y, s->appBasePos.z + pz };
        float q[4];
        ::dlvr::recentre::QuatFromYawDeg(newBase, q);
        p.orientation = XrQuaternionf{ q[0], q[1], q[2], q[3] };
        sp.poseInReferenceSpace = p;

        XrSpace fresh = XR_NULL_HANDLE;
        if (XR_SUCCEEDED(xrCreateReferenceSpace_(s->session, &sp, &fresh))) {
            if (s->appSpace) {
                xrDestroySpace_(s->appSpace);
            }
            s->appSpace = fresh;
            s->appSpaceType = sp.referenceSpaceType;
            s->appBaseYawDeg = newBase;
            s->appBasePos = p.position;
            ::dlvr::recentre::HeadClear(s->headRing);   // D-253 v3: its samples are in the old appSpace
            ::dlvr::HolsterBodyReset();                 // holster body-lock: appSpace turned - facing + latches start again
            LOG("recentered (base yaw %+.2f deg)", newBase);
        }
    }
    s->shouldRecenter = false;
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_ClearShouldRecenterFlag(ovrSession session)
{
    Session* s = S(session);
    if (s) {
        s->shouldRecenter = false;
    }
}

// ============================================================================
//  FORCE DESKTOP VSYNC OFF IN VR (vr_force_vsync_off) - 2026-10-03. Jay: "VSync just keeps turning itself on.
//  We NEED that to be off, we know it interferes with other users."
// ============================================================================
// 2026-10-03 rewrite (INV-379, static read of OUR engine/rd3d11). The DXGI vtable Present hook that was here killed the
// game on the first desktop present (INV-378, D-179) and is GONE. What the read found instead:
//  - WHY VSync "turns itself back on": the engine's Oculus bring-up (engine +0x2E6010), called by
//    CVideoSettings::LoadSettings with the freshly parsed video.scr values BEFORE they go live, executes
//        engine+0x2E63F2   c7 47 18 02 00 00 00   mov dword [rdi+0x18], 2      ; parsed VSync = 2
//    on every successful VR launch, whatever the file or the menu said. The engine's own save then writes VSync(2) back.
//  - This layer IS the OVR runtime the engine loads inside that same function, and the engine calls our ovr_Initialize
//    before it reaches +0x2E63F2. So ovr_Initialize patches the immediate 2 -> 0, once: VSync loads OFF in VR, and the
//    engine saves VSync(0). The 7 bytes are verified first; anything else is REFUSED and named in the log.
//  - (On this build the engine's VR-branch desktop Present already asks SyncInterval 0, rd3d11 +0x310B6. Jay: the value
//    still matters on other machines, so it is forced off at its source.)
// 2026-10-21 widening (Jay: "the other settings that are required to be off, like PCSS & also force the framerate cap to be
// off"; spec docs/superpowers/specs/2026-10-21-vr-video-force-design.md; static ida/fc1 + fc2). The 7-byte store becomes a
// jmp to a stub (videoforce_logic.h) that records the parsed VSync / MaxFPS / HBAO+ / DOF / PCSS / MotionBlur and zeroes all
// six before LoadSettings copies them live; flat never reaches the store (our ovr_Create fails there). At ovr_Create OK the
// engine's frame limiter (engine +0x239E00, the only reader of the cap that waits) gets a ret, so no cap source works in VR.
// COST: once per process in ovr_Initialize and ovr_Create; one byte load per submitted frame (LogRecordOnce).
namespace vsyncoff {

constexpr uintptr_t     kStoreRva = 0x2E63F2;
constexpr uintptr_t     kReturnRva = 0x2E63F9;     // the instruction after the store (mov [rdi+1Dh], bpl)
constexpr uintptr_t     kLimiterRva = 0x239E00;    // frame limiter, called once per frame from engine +0x230DD1
constexpr uintptr_t     kCapRva = 0xA3F3D8;        // the cap the limiter reads (float, fps; < 10 = uncapped)
const unsigned char     kExpect[7] = { 0xC7, 0x47, 0x18, 0x02, 0x00, 0x00, 0x00 };   // mov dword [rdi+0x18], 2
const unsigned char     kLimiterExpect[6] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x50 };   // push rbx / sub rsp, 50h
volatile LONG           g_done = 0;
volatile LONG           g_limiterDone = 0;
::dlvr::videoforce::Record* volatile g_rec = nullptr;   // set once the stub is installed; the stub fills it

// One page within rel32 reach of the engine (the bootstrap's dlssflat AllocNear pattern), code + record.
void* AllocNearExec(uintptr_t base)
{
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (uintptr_t d = 0x01000000; d < 0x60000000; d += 0x00100000) {
        const uintptr_t cand[2] = { (base + 0x01000000 + d) & ~(gran - 1), base > d ? ((base - d) & ~(gran - 1)) : 0 };
        for (uintptr_t c : cand) {
            if (!c) { continue; }
            void* p = VirtualAlloc(reinterpret_cast<void*>(c), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) { return p; }
        }
    }
    return nullptr;
}

void PatchEngineVsyncStore()
{
    if (InterlockedCompareExchange(&g_done, 1, 0) != 0) { return; }
    if (!g_vrForceVsyncOff) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off (says so when it is off)  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] vr_force_vsync_off=0 - the engine's VSync=2 store at engine+0x%llX and its frame limiter are LEFT ALONE; "
            "VSync loads as 2 in VR and MaxFPS, HBAO+, DOF, PCSS and MotionBlur load as video.scr says",
            static_cast<unsigned long long>(kStoreRva));
        return;
    }
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleA("engine_x64_rwdi.dll"));
    if (!base) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** REFUSED: engine_x64_rwdi.dll is not loaded - nothing is forced this session ***");
        return;
    }
    unsigned char* p = base + ::dlvr::buildrva::Eng(kStoreRva);   // the EGL rva on the Epic build (buildrva table)
    unsigned char got[7] = {};
    __try {
        memcpy(got, p, sizeof(got));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("[VSYNCOFF] *** REFUSED: engine+0x%llX is not readable - nothing is forced this session ***",
            static_cast<unsigned long long>(kStoreRva));
        return;
    }
    if (memcmp(got, kExpect, sizeof(kExpect)) != 0) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** REFUSED: engine+0x%llX holds %02X %02X %02X %02X %02X %02X %02X, expected C7 47 18 02 00 00 00 "
            "(mov dword [rdi+0x18], 2) - a different engine build? nothing is forced this session ***",
            static_cast<unsigned long long>(kStoreRva), got[0], got[1], got[2], got[3], got[4], got[5], got[6]);
        return;
    }
    auto* page = static_cast<unsigned char*>(AllocNearExec(reinterpret_cast<uintptr_t>(base)));
    if (!page) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** REFUSED: no free page within rel32 reach of the engine for the stub - nothing is forced this "
            "session ***");
        return;
    }
    auto* rec = reinterpret_cast<::dlvr::videoforce::Record*>(page + 0x100);
    const size_t n = ::dlvr::videoforce::BuildStub(page, ::dlvr::videoforce::kStubMax, reinterpret_cast<uintptr_t>(page),
                                                   reinterpret_cast<uintptr_t>(rec),
                                                   reinterpret_cast<uintptr_t>(p + sizeof(kExpect)));   // = kReturnRva; the byte check above proves this boundary
    unsigned char jump[7] = {};
    if (n == 0 || !::dlvr::videoforce::BuildSiteJump(jump, reinterpret_cast<uintptr_t>(p), reinterpret_cast<uintptr_t>(page))) {
        VirtualFree(page, 0, MEM_RELEASE);
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** REFUSED: the stub page %p is out of rel32 reach of engine+0x%llX - nothing is forced this "
            "session ***", static_cast<void*>(page), static_cast<unsigned long long>(kStoreRva));
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), page, n);
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(page, 0, MEM_RELEASE);
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** REFUSED: VirtualProtect on engine+0x%llX failed (%lu) - nothing is forced this session ***",
            static_cast<unsigned long long>(kStoreRva), GetLastError());
        return;
    }
    memcpy(p, jump, sizeof(jump));   // the bring-up runs on this thread after ovr_Initialize returns - nothing executes it now
    VirtualProtect(p, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(jump));
    g_rec = rec;
    // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=FLAT+HEADSET  log-only=no
    LOG("[VSYNCOFF] stub installed at engine+0x%llX (stub %p, %zu bytes): the Oculus bring-up now forces VSync, MaxFPS, HBAO+, "
        "DOF, PCSS and MotionBlur to 0 in the parsed video.scr values (VR only - flat never reaches the store)",
        static_cast<unsigned long long>(kStoreRva), static_cast<void*>(page), n);
}

// Called once from ovr_Create OK (VR confirmed: flat fails ovr_Create with no HMD).
void PatchFrameLimiter()
{
    if (InterlockedCompareExchange(&g_limiterDone, 1, 0) != 0) { return; }
    if (!g_vrForceVsyncOff) { return; }   // PatchEngineVsyncStore already logged that everything is left alone
    if (!g_rec) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] frame limiter engine+0x%llX LEFT ALONE: the video.scr stub was refused (line above), so nothing is "
            "forced this session", static_cast<unsigned long long>(kLimiterRva));
        return;
    }
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleA("engine_x64_rwdi.dll"));
    unsigned char* p = base + ::dlvr::buildrva::Eng(kLimiterRva);
    unsigned char got[6] = {};
    float cap = 0.0f;
    __try {
        memcpy(got, p, sizeof(got));
        memcpy(&cap, base + ::dlvr::buildrva::Eng(kCapRva), sizeof(cap));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** REFUSED: frame limiter engine+0x%llX is not readable - the frame cap is NOT disabled ***",
            static_cast<unsigned long long>(kLimiterRva));
        return;
    }
    if (memcmp(got, kLimiterExpect, sizeof(kLimiterExpect)) != 0) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** REFUSED: frame limiter engine+0x%llX holds %02X %02X %02X %02X %02X %02X, expected 40 53 48 83 EC "
            "50 - the frame cap is NOT disabled ***", static_cast<unsigned long long>(kLimiterRva), got[0], got[1], got[2],
            got[3], got[4], got[5]);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** REFUSED: VirtualProtect on the frame limiter engine+0x%llX failed (%lu) - the frame cap is NOT "
            "disabled ***", static_cast<unsigned long long>(kLimiterRva), GetLastError());
        return;
    }
    p[0] = 0xC3;   // ret: the limiter's own uncapped path (cap < 10 returns at once); one byte, an instruction boundary
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
    LOG("[VSYNCOFF] frame limiter engine+0x%llX patched (ret): no frame cap in VR from any source - menu, console or "
        "video.scr (cap value at patch time %.1f)", static_cast<unsigned long long>(kLimiterRva), static_cast<double>(cap));
}

// Called from ovr_SubmitFrame. COST: one byte load per frame after the first.
void LogRecordOnce()
{
    static volatile LONG s_logged = 0;
    if (s_logged) { return; }
    s_logged = 1;
    ::dlvr::videoforce::Record* r = g_rec;
    if (!r) { return; }   // the install logged its refusal (or the switch is off)
    if (!r->ran) {
        // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
        LOG("[VSYNCOFF] *** the bring-up stub never ran - the video.scr values were NOT forced this session (frame limiter: "
            "see its own line) ***");
        return;
    }
    // [PROBE: VSYNCOFF] gate=vr_force_vsync_off  site=HEADSET  log-only=no
    LOG("[VSYNCOFF] video.scr as loaded: VSync %u MaxFPS %u NvidiaEffects(%u,%u,%u) MotionBlur %u -> all forced to 0",
        r->vsync, r->maxFps, static_cast<unsigned>(r->hbao), static_cast<unsigned>(r->dof), static_cast<unsigned>(r->pcss),
        static_cast<unsigned>(r->motionBlur));
}

}  // namespace vsyncoff

// ============================================================================
//  VR RESOLUTION BLOCK - a video-options Apply cannot change the screen size in VR (2026-11-24, Jay: "Yes, build it").
//  Trace docs/traces/20261124_1840_vr_resolution_block.md; STATE (196). Static, our engine e9fbfc2c / gamedll 97046bba.
// ============================================================================
// WHY: in VR the live size is the headset's (the Oculus bring-up +2E6010 replaces video.scr's Resolution at load). The video
// menu's current-mode lookup (ITools +0x770 -> engine +261220) finds that size in no monitor mode -> -1; its list (modes of
// width >= 1024, gamedll +12A2760) then selects item 0 by default (+1174A30) = 1024x768 on a typical monitor, and every Apply
// sees "selected != current" and calls ITools::SetResolutionMode (gamedll +12A49D0). ApplyChanges (+23EF50) then re-creates the
// renderer at 1024x768 and the menu is rebuilt for it, drawn into the eye image = the huge menu. A restart fixes it because the
// VR save keeps the file's resolution (helper +8) and the bring-up runs again.
// WHAT: two 2-byte jumps, once at ovr_Create OK (VR only - flat fails ovr_Create): ITools::SetResolution (+2609E0) skips its
// two stores to its ret; ITools::SetResolutionMode (+260A00) skips its two stores AND its notify tail call (+2FE6F0, which
// writes the 2D screen globals) to its epilogue. Each is byte-checked first with no rip-relative byte in the check (the Epic
// build's identical code passes the same check through buildrva); anything else REFUSES by name and leaves it alone.
// Callers (all read): the menu Apply +12A49D0, RevertResolutionChange +11FC5A0, SetRes1024 / SetRes800 +11FC710 / +11FC790,
// the safe-mode defaults +1112DD0; none reads the return. The engine never calls them itself (uir33).
// COST: once per process; nothing per frame.
namespace vrresblock {

constexpr uintptr_t     kSetResRva = 0x2609E0;    // ITools slot 211 (+0x698) SetResolution(w, h)
constexpr uintptr_t     kSetModeRva = 0x260A00;   // ITools slot 212 (+0x6A0) SetResolutionMode(mode, bpp)
// SetResolution: +07 mov rcx,[rax+0C0h] / +0E mov [rcx+18h],edx / +11 mov [rcx+1Ch],r8d / +15 ret
const unsigned char     kSetResExpect[15] = { 0x48, 0x8B, 0x88, 0xC0, 0x00, 0x00, 0x00, 0x89, 0x51, 0x18, 0x44, 0x89, 0x41, 0x1C, 0xC3 };
constexpr size_t        kSetResCheckOff = 0x07;
constexpr size_t        kSetResPatchOff = 0x0E;
const unsigned char     kSetResPatch[2] = { 0xEB, 0x05 };   // jmp +5: from +10 to the ret at +15
// SetResolutionMode: +30 test al,al / +32 jz +3A / +34 mov [rbx+18h],ecx / +37 mov [rbx+1Ch],edx ... +57 add rsp,40h / pop rbx / ret
const unsigned char     kSetModeExpect[10] = { 0x84, 0xC0, 0x74, 0x06, 0x89, 0x4B, 0x18, 0x89, 0x53, 0x1C };
constexpr size_t        kSetModeCheckOff = 0x30;
const unsigned char     kSetModeEpilogue[6] = { 0x48, 0x83, 0xC4, 0x40, 0x5B, 0xC3 };
constexpr size_t        kSetModeEpiOff = 0x57;
constexpr size_t        kSetModePatchOff = 0x30;
const unsigned char     kSetModePatch[2] = { 0xEB, 0x25 };  // jmp +25h: from +32 to the epilogue at +57
volatile LONG           g_done = 0;

// One site: verify the check window(s), then write the 2-byte jump. Returns a short verdict for the summary line.
const char* PatchSite(unsigned char* fn, const char* what, uintptr_t rva, size_t checkOff, const unsigned char* expect,
                      size_t nExpect, size_t epiOff, const unsigned char* epi, size_t nEpi, size_t patchOff,
                      const unsigned char* patch)
{
    unsigned char got[16] = {};
    unsigned char gotEpi[8] = {};
    __try {
        memcpy(got, fn + checkOff, nExpect);
        if (epi) { memcpy(gotEpi, fn + epiOff, nEpi); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // [PROBE: RESBLOCK] gate=none (always, VR) site=HEADSET log-only=no
        LOG("[RESBLOCK] *** REFUSED %s: engine+0x%llX is not readable - Apply CAN still change the size this session ***", what,
            static_cast<unsigned long long>(rva));
        return "REFUSED (unreadable)";
    }
    if (memcmp(got, expect, nExpect) != 0 || (epi && memcmp(gotEpi, epi, nEpi) != 0)) {
        char hex[3 * 24 + 1] = {};
        size_t at = 0;
        for (size_t i = 0; i < nExpect && at + 3 < sizeof(hex); ++i) {
            at += static_cast<size_t>(_snprintf_s(hex + at, sizeof(hex) - at, _TRUNCATE, "%02X ", got[i]));
        }
        // [PROBE: RESBLOCK] gate=none (always, VR) site=HEADSET log-only=no
        LOG("[RESBLOCK] *** REFUSED %s: engine+0x%llX +0x%zX holds %s- not the expected bytes (a different engine build?); "
            "Apply CAN still change the size this session ***", what, static_cast<unsigned long long>(rva), checkOff, hex);
        return "REFUSED (bytes)";
    }
    DWORD old = 0;
    if (!VirtualProtect(fn + patchOff, 2, PAGE_EXECUTE_READWRITE, &old)) {
        // [PROBE: RESBLOCK] gate=none (always, VR) site=HEADSET log-only=no
        LOG("[RESBLOCK] *** REFUSED %s: VirtualProtect on engine+0x%llX failed (%lu) - Apply CAN still change the size ***", what,
            static_cast<unsigned long long>(rva + patchOff), GetLastError());
        return "REFUSED (protect)";
    }
    memcpy(fn + patchOff, patch, 2);   // ovr_Create OK: no menu exists yet, nothing executes these two functions now
    VirtualProtect(fn + patchOff, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn + patchOff, 2);
    return "patched";
}

void PatchResolutionSetters()
{
    if (InterlockedCompareExchange(&g_done, 1, 0) != 0) { return; }
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleA("engine_x64_rwdi.dll"));
    if (!base) {
        // [PROBE: RESBLOCK] gate=none (always, VR) site=HEADSET log-only=no
        LOG("[RESBLOCK] *** REFUSED: engine_x64_rwdi.dll is not loaded - Apply CAN still change the size this session ***");
        return;
    }
    const uintptr_t resRva = ::dlvr::buildrva::Eng(kSetResRva);     // the Epic rva on EGL (buildrva_table.h), else Steam
    const uintptr_t modeRva = ::dlvr::buildrva::Eng(kSetModeRva);
    const char* a = PatchSite(base + resRva, "SetResolution", resRva, kSetResCheckOff, kSetResExpect, sizeof(kSetResExpect),
                              0, nullptr, 0, kSetResPatchOff, kSetResPatch);
    const char* b = PatchSite(base + modeRva, "SetResolutionMode", modeRva, kSetModeCheckOff, kSetModeExpect,
                              sizeof(kSetModeExpect), kSetModeEpiOff, kSetModeEpilogue, sizeof(kSetModeEpilogue),
                              kSetModePatchOff, kSetModePatch);
    // [PROBE: RESBLOCK] gate=none (always, VR) site=HEADSET log-only=no
    LOG("[RESBLOCK] engine build %s: ITools::SetResolution engine+0x%llX %s, ITools::SetResolutionMode engine+0x%llX %s - "
        "in VR a video-options Apply keeps the headset size (shadows, draw distance and the rest still apply)",
        ::dlvr::buildrva::BuildName(::dlvr::buildrva::ModuleBuild(::dlvr::buildrva::kEngine)),
        static_cast<unsigned long long>(resRva), a, static_cast<unsigned long long>(modeRva), b);
}

}  // namespace vrresblock

// ===== [EYEALPHA] - what ALPHA does the eye image carry when the compositor gets it? (2026-10-18, Jay: "Sure") =====
// The game's projection layer is submitted with XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT (the layer build
// below), so a runtime may blend any texel whose alpha < 1. Two SteamVR testers see AFW-only warping / glitches that Jay's
// VDXR does not, and the AFW rebuild + HUD layer are the only AFW-only writers into the image; the rebuild passes the
// source alpha through (bootstrap PSCompose -> shade). This samples the FINAL image (after the rebuild and the HUD layer,
// before release) and reports alpha per eye ROLE. Log-only; the image is only read.
// Marker dlvr\diag_eye_alpha.txt, read once. COST: off - one bool test per submit. On - every 16th submit: 64 one-texel
// CopySubresourceRegion into a 64x1 staging ring of 3, read back DO_NOT_WAIT (INV-14) - a not-ready slot is counted and
// skipped, never read as data (INV-6).
namespace eyealpha {

constexpr int kPerEye = 32;   // an 8 x 4 grid of texel centres per eye
constexpr int kRing = 3;
const char* const kRole[4] = { "AFW off / unpaired", "AFW fresh eye", "AFW REBUILT held eye", "AFW held eye NOT rebuilt" };
struct Acc { long long n = 0, below = 0, zero = 0; int mn = 256, mx = -1; };

bool             g_read = false, g_on = false, g_failed = false, g_first = true;
ID3D11Texture2D* g_stg[kRing] = {};
bool             g_pending[kRing] = {};
int              g_role[kRing][2] = {};
int              g_slot = 0;
long long        g_submits = 0, g_copies = 0, g_notReady = 0;
Acc              g_acc[4];
ULONGLONG        g_next = 0;

void Fail(const char* why, long hr)
{
    g_failed = true;
    // [PROBE: EYEALPHA] gate=dlvr\diag_eye_alpha.txt  site=HEADSET  log-only=yes
    LOG("[EYEALPHA] *** REFUSED for the session: %s (hr 0x%08lX) - no alpha is reported ***", why, static_cast<unsigned long>(hr));
}

void Tick(ID3D11Texture2D* tex, const ovrRecti* vp, int afwSkip, bool afwRebuilt, bool afwOn)
{
    if (!g_read) {
        g_read = true;
        char p[MAX_PATH] = {};
        g_on = LayerFilePath("diag_eye_alpha.txt", p, sizeof(p)) && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
        // [PROBE: EYEALPHA] gate=dlvr\diag_eye_alpha.txt  site=HEADSET  log-only=yes
        LOG("[EYEALPHA] %s (marker %s)", g_on ? "ON - sampling the final eye image's alpha every 16th submit"
                                             : "off - create dlvr\\diag_eye_alpha.txt to sample the eye image's alpha", p);
    }
    if (!g_on || g_failed || !tex || !vp) { return; }
    if ((++g_submits & 15) != 0) { return; }

    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    if (!dev) { Fail("the eye texture returned no device", 0); return; }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) { dev->Release(); Fail("no immediate context", 0); return; }
    if (!g_stg[0]) {
        D3D11_TEXTURE2D_DESC sd = {};
        tex->GetDesc(&sd);
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 2 * kPerEye; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = sd.Format;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < kRing; ++i) {
            const HRESULT hr = dev->CreateTexture2D(&d, nullptr, &g_stg[i]);
            if (FAILED(hr)) { ctx->Release(); dev->Release(); Fail("CreateTexture2D (staging)", hr); return; }
        }
        if (sd.SampleDesc.Count != 1 || (sd.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
                                         sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
            ctx->Release(); dev->Release();
            Fail("the eye image is not single-sampled RGBA8 - the alpha byte position is not known", static_cast<long>(sd.Format));
            return;
        }
    }
    // Read back the oldest slot first (written kRing - 1 samplings ago), then reuse it for this submit.
    const int s = g_slot;
    if (g_pending[s]) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        const HRESULT hr = ctx->Map(g_stg[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            ++g_notReady;   // left pending: this slot is not overwritten this time
            g_slot = (g_slot + 1) % kRing;
            ctx->Release(); dev->Release();
            return;
        }
        if (FAILED(hr)) { ctx->Release(); dev->Release(); Fail("Map (staging)", hr); return; }
        const unsigned char* px = static_cast<const unsigned char*>(m.pData);
        for (int e = 0; e < 2; ++e) {
            Acc& a = g_acc[g_role[s][e]];
            for (int k = 0; k < kPerEye; ++k) {
                const int al = px[4 * (e * kPerEye + k) + 3];
                ++a.n;
                if (al < 255) { ++a.below; }
                if (al == 0) { ++a.zero; }
                if (al < a.mn) { a.mn = al; }
                if (al > a.mx) { a.mx = al; }
            }
        }
        ctx->Unmap(g_stg[s], 0);
        g_pending[s] = false;
    }
    for (int e = 0; e < 2; ++e) {
        g_role[s][e] = !afwOn || afwSkip < 0 || afwSkip > 1 ? 0 : (e != afwSkip ? 1 : (afwRebuilt ? 2 : 3));
        for (int iy = 0; iy < 4; ++iy) {
            for (int ix = 0; ix < 8; ++ix) {
                const UINT x = static_cast<UINT>(vp[e].Pos.x + (2 * ix + 1) * vp[e].Size.w / 16);
                const UINT y = static_cast<UINT>(vp[e].Pos.y + (2 * iy + 1) * vp[e].Size.h / 8);
                const D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
                ctx->CopySubresourceRegion(g_stg[s], 0, static_cast<UINT>(e * kPerEye + iy * 8 + ix), 0, 0, tex, 0, &box);
            }
        }
    }
    g_pending[s] = true;
    ++g_copies;
    g_slot = (g_slot + 1) % kRing;
    ctx->Release();
    dev->Release();

    const ULONGLONG now = GetTickCount64();
    const bool any = g_acc[0].n + g_acc[1].n + g_acc[2].n + g_acc[3].n > 0;
    if (any && (g_first || now >= g_next)) {
        g_next = now + 5000;
        char line[1024] = {};
        int off = 0;
        for (int r = 0; r < 4; ++r) {
            const Acc& a = g_acc[r];
            if (!a.n) { continue; }
            off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " | %s: %lld texels, alpha min %d max %d, below 255 %lld, zero %lld",
                               kRole[r], a.n, a.mn, a.mx, a.below, a.zero);
        }
        // [PROBE: EYEALPHA] gate=dlvr\diag_eye_alpha.txt  site=HEADSET  log-only=yes
        LOG("[EYEALPHA] %s: %lld samplings, readback not ready %lld%s", g_first ? "FIRST (logged unconditionally)" : "5 s",
            g_copies, g_notReady, line);
        g_first = false;
        for (Acc& a : g_acc) { a = Acc(); }
        g_copies = g_notReady = 0;
    }
}

}  // namespace eyealpha

// ============================================================================
//  Swapchains (D3D11)
// ============================================================================

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_CreateTextureSwapChainDX(ovrSession session, IUnknown* d3dPtr,
                             const ovrTextureSwapChainDesc* desc,
                             ovrTextureSwapChain* out_TextureSwapChain)
{
    LOG("-> ovr_CreateTextureSwapChainDX(d3d=%p, %dx%d, fmt=%d, array=%d, mips=%d, samples=%d)",
        d3dPtr, desc ? desc->Width : 0, desc ? desc->Height : 0,
        desc ? static_cast<int>(desc->Format) : -1, desc ? desc->ArraySize : 0,
        desc ? desc->MipLevels : 0, desc ? desc->SampleCount : 0);
    if (!out_TextureSwapChain || !desc || !d3dPtr) {
        return ovrError_InvalidParameter;
    }
    *out_TextureSwapChain = nullptr;

    Session* s = S(session);
    if (!s) {
        return ovrError_InvalidSession;
    }

    ID3D11Device* device = nullptr;
    if (FAILED(d3dPtr->QueryInterface(__uuidof(ID3D11Device),
                                      reinterpret_cast<void**>(&device))) || !device) {
        SetError(ovrError_InvalidParameter, "ovr_CreateTextureSwapChainDX: not an ID3D11Device");
        return ovrError_InvalidParameter;
    }

    // This is where the D3D device finally reaches us, so this is where the
    // OpenXR session can be created.
    const bool first = (s->session == XR_NULL_HANDLE);
    if (!EnsureSession(s, device)) {
        device->Release();
        SetError(ovrError_Initialize, "xrCreateSession failed");
        return ovrError_Initialize;
    }
    device->Release();
    if (first) {
        LOG("session created lazily from first swapchain (%dx%d)", desc->Width, desc->Height);
        // Same trigger as the session itself: this is the first moment both a
        // D3D device and an XrSession exist.
        OverlayInit(s->device, s->session);
    ::dlvr::uipanel::Init(s->device, s->session);   // ui_panel_menus: its swapchain is made the first time a menu shows
    ::dlvr::wristquad::Init(s->device, s->session); // wrist HUD stage 2 (dlvr\wrist_hud.txt): its swapchain is made at the first piece
    ::dlvr::loadscreen::Init(s->device, s->session);   // the loading-screen void: its swapchain is made at the first loading screen
        // (vr_force_vsync_off no longer hooks DXGI here - INV-378. It patches the engine in ovr_Initialize, INV-379.)
    }

    auto* c = new SwapChain();
    c->desc = *desc;

    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = ToDxgi(desc->Format);
    ci.sampleCount = desc->SampleCount > 0 ? desc->SampleCount : 1;
    ci.width = desc->Width;
    ci.height = desc->Height;
    ci.faceCount = 1;
    ci.arraySize = desc->ArraySize > 0 ? desc->ArraySize : 1;
    ci.mipCount = desc->MipLevels > 0 ? desc->MipLevels : 1;

    if (!XrOk(xrCreateSwapchain_(s->session, &ci, &c->handle), "xrCreateSwapchain")) {
        delete c;
        SetError(ovrError_RuntimeException, "xrCreateSwapchain failed");
        return ovrError_RuntimeException;
    }

    uint32_t count = 0;
    xrEnumerateSwapchainImages_(c->handle, 0, &count, nullptr);
    c->images.resize(count);
    for (auto& img : c->images) {
        img = XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
    }
    if (!XrOk(xrEnumerateSwapchainImages_(
                  c->handle, count, &count,
                  reinterpret_cast<XrSwapchainImageBaseHeader*>(c->images.data())),
              "xrEnumerateSwapchainImages")) {
        xrDestroySwapchain_(c->handle);
        delete c;
        return ovrError_RuntimeException;
    }

    s->chains.push_back(c);
    *out_TextureSwapChain = reinterpret_cast<ovrTextureSwapChain>(c);
    LOG("swapchain %p: %dx%d fmt=%d, %u images", c, desc->Width, desc->Height,
        static_cast<int>(desc->Format), count);
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetTextureSwapChainLength(ovrSession session, ovrTextureSwapChain chain, int* out_Length)
{
    (void)session;
    LOG_FIRST(3, "-> ovr_GetTextureSwapChainLength");
    SwapChain* c = SC(chain);
    if (!c || !out_Length) {
        return ovrError_InvalidParameter;
    }
    *out_Length = static_cast<int>(c->images.size());
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetTextureSwapChainCurrentIndex(ovrSession session, ovrTextureSwapChain chain, int* out_Index)
{
    (void)session;
    SwapChain* c = SC(chain);
    if (!c || !out_Index) {
        return ovrError_InvalidParameter;
    }
    if (c->sdMode) {   // submit_drawn_image: hand out the newest held image (a new one per recorded frame)
        bool acquiredNew = false;
        const uint32_t idx = SdHandOut(c, &acquiredNew);
        *out_Index = static_cast<int>(idx);
        return ovrSuccess;
    }
    const bool wasAcquired = c->acquired;
    if (c->deferPending) { InterlockedIncrement(&g_afwRbEarlyUse); }   // asked for the next image before our submit released this one
    AcquireIfNeeded(c);
    *out_Index = static_cast<int>(c->index);
    LOG_FIRST(5, "-> ovr_GetTextureSwapChainCurrentIndex -> %d (acquired now=%d, was=%d)",
              *out_Index, c->acquired ? 1 : 0, wasAcquired ? 1 : 0);
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetTextureSwapChainDesc(ovrSession session, ovrTextureSwapChain chain,
                            ovrTextureSwapChainDesc* out_Desc)
{
    (void)session;
    SwapChain* c = SC(chain);
    if (!c || !out_Desc) {
        return ovrError_InvalidParameter;
    }
    *out_Desc = c->desc;
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetTextureSwapChainBufferDX(ovrSession session, ovrTextureSwapChain chain, int index,
                                IID iid, void** out_Buffer)
{
    (void)session;
    LOG_FIRST(4, "-> ovr_GetTextureSwapChainBufferDX(index=%d)", index);
    SwapChain* c = SC(chain);
    if (!c || !out_Buffer) {
        return ovrError_InvalidParameter;
    }
    if (index < 0) {
        if (c->sdMode) {   // submit_drawn_image: "the current buffer" = the hand-out
            bool acquiredNew = false;
            index = static_cast<int>(SdHandOut(c, &acquiredNew));
        } else {
            AcquireIfNeeded(c);
            index = static_cast<int>(c->index);
        }
    }
    if (index >= static_cast<int>(c->images.size())) {
        return ovrError_InvalidParameter;
    }
    ID3D11Texture2D* tex = c->images[index].texture;
    if (!tex) {
        return ovrError_RuntimeException;
    }

    // MILESTONE D RECON v3 - register the eye-swapchain textures with the
    // BOOTSTRAP, which owns the D3D hooks.
    //
    // This is the exact texture the engine is about to render its composite
    // into, and it is OURS - the layer allocated it. Run 1 proved the eye
    // cannot be recovered from the engine's own buffers (both eyes share one
    // pair) and run 2 proved ovr_GetRenderDesc bursts at frame setup rather
    // than per eye. The swapchain is what is left, and a bind to it plus the
    // viewport's x origin is definitive and re-anchors every frame.
    //
    // EVERY chain is registered, not just the eye one - the bootstrap narrows
    // it by width against the measured per-eye render size, which is a smaller
    // assumption than guessing here which chain is which.
    {
        static PFN_DlvrNoteTex s_noteTex = nullptr;
        static bool s_resolved = false;
        if (!s_resolved) {
            s_resolved = true;
            if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                s_noteTex = reinterpret_cast<PFN_DlvrNoteTex>(
                    GetProcAddress(b, "DlvrNoteEyeSwapchainTexture"));
            }
            LOG("[JITRECON] eye-swapchain registration %s",
                s_noteTex ? "resolved (DlvrNoteEyeSwapchainTexture)"
                          : "NOT resolved - the v3 eye anchor cannot work");
        }
        if (s_noteTex) { s_noteTex(tex); }
    }

    return SUCCEEDED(tex->QueryInterface(iid, out_Buffer)) ? ovrSuccess
                                                           : ovrError_InvalidParameter;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_CommitTextureSwapChain(ovrSession session, ovrTextureSwapChain chain)
{
    (void)session;
    SwapChain* c = SC(chain);
    if (!c) {
        return ovrError_InvalidParameter;
    }
    LOG_FIRST(5, "-> ovr_CommitTextureSwapChain (acquired=%d index=%u)",
              c->acquired ? 1 : 0, c->index);
    if (c->sdMode) {   // submit_drawn_image: nothing is released here - the submit releases the image the frame drew
        AcquireSRWLockExclusive(&g_sdQLock);
        c->commitSinceAcquire = true;
        ReleaseSRWLockExclusive(&g_sdQLock);
        return ovrSuccess;
    }
    if (!c->acquired) {
        return ovrSuccess;  // nothing was rendered into it this frame
    }
    // afw_rebuild (AFW step 4b, an EXPERIMENT; compiled ON, follows afw_enable): the held eye is rebuilt INTO this image at
    // ovr_SubmitFrame, so it must still be acquired there. ovr_SubmitFrame already releases any image it finds
    // acquired before xrEndFrame (the eye loop), so leaving it acquired here only moves the release later in the
    // same frame. Off, or AFW off: released here, as always. The rebuild cannot work without this deferral.
    if (AfwRebuild && AfwAltEye) {
        c->deferPending = true;
        InterlockedIncrement(&g_afwRbDeferred);
        return ovrSuccess;
    }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    ::dlvr::loadscreen::CaptureBeforeRelease(c->handle, c->index < c->images.size() ? c->images[c->index].texture : nullptr);
    XrOk(xrReleaseSwapchainImage_(c->handle, &ri), "xrReleaseSwapchainImage");
    c->acquired = false;
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void)
ovr_DestroyTextureSwapChain(ovrSession session, ovrTextureSwapChain chain)
{
    Session* s = S(session);
    SwapChain* c = SC(chain);
    if (!c) {
        return;
    }
    if (c->handle) {
        xrDestroySwapchain_(c->handle);
    }
    if (s) {
        for (size_t i = 0; i < s->chains.size(); ++i) {
            if (s->chains[i] == c) {
                s->chains.erase(s->chains.begin() + i);
                break;
            }
        }
    }
    delete c;
}

// ============================================================================
//  Mirror texture - a plain D3D texture; the game blits it to its window.
// ============================================================================

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_CreateMirrorTextureDX(ovrSession session, IUnknown* d3dPtr,
                          const ovrMirrorTextureDesc* desc, ovrMirrorTexture* out_MirrorTexture)
{
    LOG("-> ovr_CreateMirrorTextureDX(%dx%d)", desc ? desc->Width : 0, desc ? desc->Height : 0);
    if (!out_MirrorTexture || !desc || !d3dPtr) {
        return ovrError_InvalidParameter;
    }
    *out_MirrorTexture = nullptr;
    Session* s = S(session);
    if (!s) {
        return ovrError_InvalidSession;
    }

    ID3D11Device* device = nullptr;
    if (FAILED(d3dPtr->QueryInterface(__uuidof(ID3D11Device),
                                      reinterpret_cast<void**>(&device))) || !device) {
        return ovrError_InvalidParameter;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc->Width;
    td.Height = desc->Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = ToDxgi(desc->Format);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    auto* m = new MirrorTexture();
    m->desc = *desc;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m->tex);
    device->Release();
    if (FAILED(hr)) {
        delete m;
        SetError(ovrError_RuntimeException, "mirror CreateTexture2D failed");
        return ovrError_RuntimeException;
    }

    s->mirrors.push_back(m);
    *out_MirrorTexture = reinterpret_cast<ovrMirrorTexture>(m);
    LOG("mirror texture %p: %dx%d", m, desc->Width, desc->Height);
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetMirrorTextureBufferDX(ovrSession session, ovrMirrorTexture mirror, IID iid,
                             void** out_Buffer)
{
    (void)session;
    auto* m = reinterpret_cast<MirrorTexture*>(mirror);
    if (!m || !m->tex || !out_Buffer) {
        return ovrError_InvalidParameter;
    }
    return SUCCEEDED(m->tex->QueryInterface(iid, out_Buffer)) ? ovrSuccess
                                                              : ovrError_InvalidParameter;
}

OVR_PUBLIC_FUNCTION(void) ovr_DestroyMirrorTexture(ovrSession session, ovrMirrorTexture mirror)
{
    Session* s = S(session);
    auto* m = reinterpret_cast<MirrorTexture*>(mirror);
    if (!m) {
        return;
    }
    if (m->tex) {
        m->tex->Release();
    }
    if (s) {
        for (size_t i = 0; i < s->mirrors.size(); ++i) {
            if (s->mirrors[i] == m) {
                s->mirrors.erase(s->mirrors.begin() + i);
                break;
            }
        }
    }
    delete m;
}

// SteamVR PANEL CROP (fovcrop_logic.h; trace docs/traces/20261118_1345_steamvr_panel_crop.md; Jay 2026-11-18 "Go ahead", "SteamVR
// only"). On the SteamVR runtime the projection view e is cut to the runtime's own panel window inside the image the game drew:
// imageRect = that window's pixels, fov = the exact tangents of its pixel edges. SteamVR passes a layer's frustum to the headset
// driver, and the Pimax driver ("aapvr") shows it with the panel's frustum instead - a Crystal Light saw the whole image doubled
// at symmetric_fov=2, correct with the SteamVR dashboard open. Called AFTER the wrist HUD, the lens mask and lsVp took the
// uncropped values (they describe the whole image). COST: a bitwise compare of the cached inputs per eye per submit; on a change
// 8 tan + 4 atan.
// META'S RUNTIME (ownFov; Jay 2026-11-24 "Go ahead with 1"; trace docs/traces/20261124_1550_meta_panel_crop.md): it spreads the submitted
// rectangle over its OWN FOV, so it gets the same cut. Two differences: a pitched pose does not stop it (the window is cut in the image's
// own frame and declared at the image's pose - what that runtime was given before), and a panel past the image is zoomed to fit
// (CropScaled) instead of left whole.
namespace {
struct PanelCropCache {
    bool                    have = false;
    XrFovf                  img{}, panel{};
    XrRect2Di               vp{};
    int                     gate = -1;   // -1 computed by Crop, else the gate reason below
    ::dlvr::fovcrop::Result res{};
};
PanelCropCache g_pcCache[2];
int            g_pcLastLogged[2] = { -99, -99 };
int            g_pcLines = 0;
constexpr int  kPcGatePitched = 10, kPcGateCentre = 11, kPcGateNoGameFov = 12;

const char* PanelCropGateName(int gate)
{
    switch (gate) {
        case kPcGatePitched:   return "NOT CUT: the declared pose is pitched (auto_pitch / pitch_offset_deg) - the panel window is not in the image's frame";
        case kPcGateCentre:    return "NOT CUT: fov_center_correct=1 declares a re-centred frustum";
        case kPcGateNoGameFov: return "NOT CUT: no game frustum this submit (the runtime's own FOV is declared already)";
        default:               return ::dlvr::fovcrop::WhyName(gate);
    }
}

void SteamVrPanelCrop(int e, XrCompositionLayerProjectionView& pv, const XrFovf& panel, float pitchDeg, bool haveGameFov, bool ownFov)
{
    if (e < 0 || e > 1) { return; }
    PanelCropCache& c = g_pcCache[e];
    const int gate = (!ownFov && fabsf(pitchDeg) >= 0.01f) ? kPcGatePitched
                   : (g_fovCenterCorrect ? kPcGateCentre : (!haveGameFov ? kPcGateNoGameFov : -1));
    const XrRect2Di& vp = pv.subImage.imageRect;
    const bool same = c.have && c.gate == gate && memcmp(&c.img, &pv.fov, sizeof(XrFovf)) == 0 &&
                      memcmp(&c.panel, &panel, sizeof(XrFovf)) == 0 && memcmp(&c.vp, &vp, sizeof(XrRect2Di)) == 0;
    if (!same) {
        c.have = true;
        c.img = pv.fov;
        c.panel = panel;
        c.vp = vp;
        c.gate = gate;
        if (gate < 0) {
            const ::dlvr::fovcrop::Fov cImg{ pv.fov.angleLeft, pv.fov.angleRight, pv.fov.angleUp, pv.fov.angleDown };
            const ::dlvr::fovcrop::Fov cPan{ panel.angleLeft, panel.angleRight, panel.angleUp, panel.angleDown };
            const ::dlvr::fovcrop::Rect cVp{ vp.offset.x, vp.offset.y, vp.extent.width, vp.extent.height };
            c.res = ownFov ? ::dlvr::fovcrop::CropScaled(cImg, cPan, cVp) : ::dlvr::fovcrop::Crop(cImg, cPan, cVp);
        }
        const int decision = gate >= 0 ? gate : c.res.why;
        if (decision != g_pcLastLogged[e] && g_pcLines < 20) {
            ++g_pcLines;
            const float k = 57.2957795f;
            const bool cut = gate < 0 && (c.res.why == ::dlvr::fovcrop::kCropped || c.res.why == ::dlvr::fovcrop::kScaled);
            // [PROBE: FOVCROP] gate=none (SteamVR or Meta runtime; first decision per eye unconditionally, then each change, 20 lines max)  site=HEADSET  log-only=no
            LOG("[FOVCROP] eye%d %s: %s | image deg L%.2f R%.2f U%.2f D%.2f rect (%d,%d %dx%d) | panel deg L%.2f R%.2f U%.2f D%.2f | "
                "margin px L%.1f R%.1f T%.1f B%.1f | -> declared deg L%.2f R%.2f U%.2f D%.2f rect (%d,%d %dx%d)",
                e, g_pcLastLogged[e] == -99 ? "FIRST (logged unconditionally)" : "decision changed", PanelCropGateName(decision),
                pv.fov.angleLeft * k, pv.fov.angleRight * k, pv.fov.angleUp * k, pv.fov.angleDown * k, vp.offset.x, vp.offset.y,
                vp.extent.width, vp.extent.height, panel.angleLeft * k, panel.angleRight * k, panel.angleUp * k, panel.angleDown * k,
                gate < 0 ? c.res.marginPx[0] : 0.0, gate < 0 ? c.res.marginPx[1] : 0.0, gate < 0 ? c.res.marginPx[2] : 0.0,
                gate < 0 ? c.res.marginPx[3] : 0.0,
                (cut ? c.res.fov.left : pv.fov.angleLeft) * k, (cut ? c.res.fov.right : pv.fov.angleRight) * k,
                (cut ? c.res.fov.up : pv.fov.angleUp) * k, (cut ? c.res.fov.down : pv.fov.angleDown) * k,
                cut ? c.res.rect.x : vp.offset.x, cut ? c.res.rect.y : vp.offset.y, cut ? c.res.rect.w : vp.extent.width,
                cut ? c.res.rect.h : vp.extent.height);
            g_pcLastLogged[e] = decision;
        }
    }
    if (c.gate < 0 && (c.res.why == ::dlvr::fovcrop::kCropped || c.res.why == ::dlvr::fovcrop::kScaled)) {
        pv.fov = XrFovf{ c.res.fov.left, c.res.fov.right, c.res.fov.up, c.res.fov.down };
        pv.subImage.imageRect.offset.x = c.res.rect.x;
        pv.subImage.imageRect.offset.y = c.res.rect.y;
        pv.subImage.imageRect.extent.width = c.res.rect.w;
        pv.subImage.imageRect.extent.height = c.res.rect.h;
    }
}
}  // namespace

// ============================================================================
//  Frame submission
// ============================================================================

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_SubmitFrame(ovrSession session, long long frameIndex,
                const ovrViewScaleDesc* viewScaleDesc,
                ovrLayerHeader const* const* layerPtrList, unsigned int layerCount)
{
    (void)frameIndex;
    vsyncoff::LogRecordOnce();   // vr_force_vsync_off: what video.scr loaded with, once; one byte load per frame after
    // [TGROUP] site 3 of 3, AND THE ONE THE QUESTION IS ABOUT - the RENDER side.
    // INV-169 covered only the threads that call the ITEM hook (seven tids, none
    // of them group 2). A probe on our own hook threads cannot see the draw
    // thread by construction, which INV-169 said in those words. ovr_SubmitFrame
    // "comes from exactly one" thread (this file, :279) and it is the render one.
    // Pure reads, deduped per (tid, site), so this costs one TLS fetch on the
    // first frame and a 32-entry scan thereafter.
    dlvr::ThreadGroupProbe("ovr_SubmitFrame");
    // [RSTORE] S3-submit - THE SITE THE QUESTION IS ABOUT, with the RIGHT selector.
    // [TGROUP] above reads g_ThreadGroupIndex (TLS slot 22). INV-213/INV-215: the
    // altMatrices parity branch is gated on the CThreadContextManager's ctx id, TLS
    // slot 62 - a different object, type, TlsAlloc and module (INV-211), measured live
    // as 22->0 and 62->1 on the same threads in one run. So this site has been probed
    // with the wrong selector since it was built. This reads the RIGHT one, passively.
    dlvr::ResolverCtxProbe("ovr_SubmitFrame");
    if (viewScaleDesc) {
        LOG_FIRST(2, "   SubmitFrame viewScale: L=(%.4f,%.4f,%.4f) R=(%.4f,%.4f,%.4f) scale=%.3f",
                  viewScaleDesc->HmdToEyeOffset[0].x, viewScaleDesc->HmdToEyeOffset[0].y,
                  viewScaleDesc->HmdToEyeOffset[0].z, viewScaleDesc->HmdToEyeOffset[1].x,
                  viewScaleDesc->HmdToEyeOffset[1].y, viewScaleDesc->HmdToEyeOffset[1].z,
                  viewScaleDesc->HmdSpaceToWorldScaleInMeters);
    }

    LOG_FIRST(8, "-> ovr_SubmitFrame(frameIndex=%lld, layers=%u)",
              static_cast<long long>(frameIndex), layerCount);
    Session* s = S(session);
    if (!s || s->session == XR_NULL_HANDLE) {
        return ovrError_InvalidSession;
    }
    static ThreadSet seen;
    Guard lock;
    NoteThread("ovr_SubmitFrame", seen);
    s->submitEnteredAt = NowSeconds();
    ::dlvr::loadscreen::Tick();   // the engine's background-render flag for this submit (loadscreen.h)
    // [GPUFRAME] (bootstrap, dlvr\gpu_frame.txt, INV-583): ONE GPU-timing frame boundary per submitted frame. This runs on the
    // executor thread (the one thread issuing D3D calls, INV-152), so the bootstrap may End() queries on the immediate context
    // here. Resolved once; a bootstrap without the export simply never times. Off in the bootstrap: one bool test there.
    {
        typedef void (*PFN_GpuFrameBoundary)();
        static PFN_GpuFrameBoundary s_gfb = nullptr;
        static bool s_gfbResolved = false;
        if (!s_gfbResolved) {
            s_gfbResolved = true;
            if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                s_gfb = reinterpret_cast<PFN_GpuFrameBoundary>(GetProcAddress(b, "DlvrGpuFrameBoundary"));
            }
            LOG("gpuframe: frame-boundary export %s", s_gfb ? "resolved (DlvrGpuFrameBoundary)"
                                                           : "NOT resolved - the bootstrap cannot time GPU frames");
        }
        if (s_gfb) { s_gfb(); }
    }
    if (s->beganAt > 0.0) {
        const double ms = (s->submitEnteredAt - s->beganAt) * 1000.0;
        if (ms > s->gameMaxMs) {
            s->gameMaxMs = ms;   // purely the engine: we are not running here
        }
        s->gameSumMs += ms;
        ++s->splitCount;         // counted HERE, where the frame's split begins
    }
    if (!s->frameActive) {
        // The game submitted without us having begun a frame (can happen on the
        // very first frame); start one so xrEndFrame is legal.
        if (!BeginFrameIfNeeded(s)) {
            return ovrSuccess;
        }
    }

    XrCompositionLayerProjectionView projViews[2]{};
    XrFovf imgFov0{};   // eye 0's frustum of the WHOLE image, before the SteamVR panel crop: the menu panel / loading quads draw the whole pass
    XrFovf imgFov1{};   // eye 1's, taken at the same point - the menu / HUD panel size and centre eye 1's quad from it (trace 20261124_1345)
    XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad overlayQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad grappleQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };   // grapple_reticle_3d
    XrCompositionLayerQuad sphereQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };    // the holster sphere (place-by-hand Task 4)
    XrCompositionLayerQuad ringQuad[3] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD },
                                           { XR_TYPE_COMPOSITION_LAYER_QUAD } };   // the holster rings (plan 2026-11-20 Task 5)
    XrCompositionLayerQuad titleQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };     // the mod menu's title above its panel (BETA)
    XrCompositionLayerQuad calibQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };     // the calibration screen (build A)
    XrCompositionLayerQuad calibFloor{ XR_TYPE_COMPOSITION_LAYER_QUAD };    // its floor under the void (build B)
    XrCompositionLayerQuad calibBox[5] = {};                                // the void's 4 walls + ceiling (Jay 2026-10-16)
    XrCompositionLayerQuad menuQuad[2] = {};                                // ui_panel_menus (uipanel.cpp): one per eye
    XrQuaternionf menuRendered0{ 0.0f, 0.0f, 0.0f, 1.0f };                  // eye 0's orientation BEFORE the declared pitch
    XrCompositionLayerQuad loadQuad[2] = {};                                // the loading-screen void (loadscreen.cpp): one per eye
    XrRect2Di lsVp[2] = {};                                                 // ...each eye's rectangle in its image this submit
    // 7: + the calibration screen's panel (build A) and floor (build B); 12: + the void's 4 walls and ceiling (2026-10-16).
    // The runtime reports 16 layers (VDXR, "system (openxr): ... 16 layer(s)").
    // 16 (2026-11-12, wrist HUD stage 2: + up to 5 wrist quads): XR_MIN_COMPOSITION_LAYERS_SUPPORTED (openxr.h) - every conformant
    // runtime takes 16 (trace docs/traces/20261112_1810_wrist_stage2_quads.md).
    const int kLayerCap = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    XrCompositionLayerQuad wristQuad[::dlvr::wristquad::kMaxQuads] = {};   // 5 pieces + the 3b placement box + 2 beams                               // the wrist HUD pieces (wristquad.cpp)
    const XrCompositionLayerBaseHeader* layers[kLayerCap] = {};
    uint32_t submitted = 0;

    // Set when the game image was blitted into the flat window's mirror this
    // frame. The overlay is only drawn into the mirror on those frames: the
    // mirror is never cleared, so drawing a translucent panel onto it every
    // frame while the backdrop underneath is only refreshed occasionally would
    // composite the panel onto itself and darken it.
    bool mirrorRefreshed = false;

    // Both stereo layer types are accepted, but they must each be read with
    // their OWN layout. They do NOT share a prefix past Viewport:
    //
    //   ovrLayerEyeFov     Header, ColorTexture[2], Viewport[2], Fov[2],        RenderPose[2]
    //   ovrLayerEyeMatrix  Header, ColorTexture[2], Viewport[2], RenderPose[2], Matrix[2]
    //
    // An earlier version read both through one "common prefix" struct that
    // omitted Fov, so for an EyeFov layer it read the FOV tangents as if they
    // were RenderPose. The poses came out as (1.0296, 1.0296, 1.0296) - that is
    // tan(0.8), our own fallback FOV - and xrEndFrame rejected the layer with
    // XR_ERROR_POSE_INVALID (-39), i.e. a black screen. A static_assert on size
    // happily passed while the layout was nonsense; only reading real values
    // exposed it.
    const ovrTextureSwapChain* colorTex = nullptr;
    const ovrRecti* viewport = nullptr;
    const ovrPosef* renderPose = nullptr;
    const ovrFovPort* layerFov = nullptr;  // EyeFov layers only
    ovrFovPort usedFov[2] = {};            // what the eyes were drawn with (EyeFovForFrame), copied under the lock
    bool fovNoted = false;                 // dlvr::eyefov::NoteSubmit ran for this submit

    for (unsigned int i = 0; i < layerCount && submitted == 0; ++i) {  // first usable layer wins
        const ovrLayerHeader* h = layerPtrList ? layerPtrList[i] : nullptr;
        LOG_FIRST(6, "   layer[%u] type=%d flags=%#x", i,
                  h ? static_cast<int>(h->Type) : -1, h ? h->Flags : 0u);
        if (!h) {
            continue;
        }
        if (h->Type == ovrLayerType_EyeFov) {
            auto* l = reinterpret_cast<const ovrLayerEyeFov*>(h);
            colorTex = l->ColorTexture;
            viewport = l->Viewport;
            renderPose = l->RenderPose;
            layerFov = l->Fov;
            // [LOADBEARING: FOV-LIVE-EYE] the layer's own Fov is rd3d11's one copy of the STARTUP answer (the cull camera's superset) -
            //      the frustum this image was drawn with is the one ovr_GetRenderDesc returned. Declared must equal rendered.
            if (dlvr::eyefov::Ready(g_liveFov)) {
                usedFov[0] = FromTans(dlvr::eyefov::Used(g_liveFov, 0));
                usedFov[1] = FromTans(dlvr::eyefov::Used(g_liveFov, 1));
                layerFov = usedFov;
            }
        } else if (h->Type == ovrLayerType_EyeMatrix) {
            auto* l = reinterpret_cast<const ovrLayerEyeMatrix*>(h);
            colorTex = l->ColorTexture;
            viewport = l->Viewport;
            renderPose = l->RenderPose;
        } else {
            continue;
        }

        // A single shared eye texture is legal and is what this game uses: it
        // allocates one 6144x3264 swapchain, sets ColorTexture[0], leaves
        // ColorTexture[1] NULL, and selects each eye through Viewport[].
        // Requiring both entries rejected the layer outright and left us
        // submitting nothing.
        bool usable = true;
        LOG_FIRST(4,
                  "   game RenderPose: [0]=(%.4f,%.4f,%.4f) [1]=(%.4f,%.4f,%.4f)",
                  renderPose[0].Position.x, renderPose[0].Position.y, renderPose[0].Position.z,
                  renderPose[1].Position.x, renderPose[1].Position.y, renderPose[1].Position.z);
        LOG_FIRST(4,
                  "   our xrLocateViews: [0]=(%.4f,%.4f,%.4f) [1]=(%.4f,%.4f,%.4f) head=(%.4f,%.4f,%.4f)",
                  s->views[0].pose.position.x, s->views[0].pose.position.y,
                  s->views[0].pose.position.z, s->views[1].pose.position.x,
                  s->views[1].pose.position.y, s->views[1].pose.position.z,
                  s->headPose.position.x, s->headPose.position.y, s->headPose.position.z);
        LOG_FIRST(3, "   viewports: L=(%d,%d %dx%d) R=(%d,%d %dx%d)  swap_eyes=%d",
                  viewport[0].Pos.x, viewport[0].Pos.y, viewport[0].Size.w, viewport[0].Size.h,
                  viewport[1].Pos.x, viewport[1].Pos.y, viewport[1].Size.w, viewport[1].Size.h,
                  g_swapEyes ? 1 : 0);

        // Decide which of the game's eye slots feeds each OpenXR view, using the
        // poses it says it rendered from rather than an assumed convention.
        //
        // This used to compare the two RenderPose X coordinates directly, which
        // is a TRACKING-SPACE test for a HEAD-SPACE question. The separation
        // between the eyes along tracking X is ipd * cos(yaw): it shrinks to
        // nothing as the player turns and changes sign past ninety degrees. Near
        // that angle the comparison is decided by tracking jitter, so the
        // assignment flip-flops frame to frame - which reads in the headset
        // exactly as reported: a hard cutoff at a particular yaw where the
        // stereo collapses or the eyes trade places, rather than anything smooth.
        //
        // Rotating the head-to-eye vector into head space by the inverse of the
        // head orientation makes the test independent of where the player is
        // looking. The result is then latched: which slot the engine fills first
        // is a property of the engine and cannot change while the game runs, so
        // re-deciding it every frame only ever creates opportunities to be wrong.
        int srcForView[2] = { 0, 1 };
        if (g_autoEyeAssign) {
            if (s->eyeAssign < 0) {
                const XrVector3f d0{ renderPose[0].Position.x - s->headPose.position.x,
                                     renderPose[0].Position.y - s->headPose.position.y,
                                     renderPose[0].Position.z - s->headPose.position.z };
                const XrVector3f d1{ renderPose[1].Position.x - s->headPose.position.x,
                                     renderPose[1].Position.y - s->headPose.position.y,
                                     renderPose[1].Position.z - s->headPose.position.z };
                const float x0 = UnrotateBy(s->headPose.orientation, d0).x;
                const float x1 = UnrotateBy(s->headPose.orientation, d1).x;
                // Commit only on a pose that actually looks like a pair of
                // eyes. The first version required nothing but a centimetre of
                // separation, and duly latched on frame 1 - where the game's
                // RenderPose is still its uninitialised value, a head-to-eye
                // vector of (0.028, -1.056, -0.059). That is a metre below the
                // head, and it passed. It happened to latch the right answer,
                // which is the worst kind of pass: a coin flip that looked like
                // a measurement.
                //
                // A real pair of eyes sits half an IPD either side of the head
                // centre, level with it, and on opposite sides.
                const XrVector3f l0 = UnrotateBy(s->headPose.orientation, d0);
                const XrVector3f l1 = UnrotateBy(s->headPose.orientation, d1);
                const bool sane =
                    fabsf(l0.y) < 0.02f && fabsf(l1.y) < 0.02f &&
                    fabsf(l0.z) < 0.05f && fabsf(l1.z) < 0.05f &&
                    fabsf(l0.x) > 0.02f && fabsf(l0.x) < 0.05f &&
                    fabsf(l1.x) > 0.02f && fabsf(l1.x) < 0.05f &&
                    (l0.x * l1.x) < 0.0f;
                if (sane && fabsf(x1 - x0) > 0.01f) {
                    s->eyeAssign = (x0 > x1) ? 1 : 0;
                    LOG("eye assignment latched: slot %d is the left eye "
                        "(head-space x: [0]=%.4f [1]=%.4f)",
                        s->eyeAssign, x0, x1);
                }
            }
            if (s->eyeAssign == 1) {
                srcForView[0] = 1;
                srcForView[1] = 0;
            }
        }
        if (layerFov) {
            LOG_FIRST(3, "   game Fov[0]=(L%.3f R%.3f U%.3f D%.3f) Fov[1]=(L%.3f R%.3f U%.3f D%.3f) (as drawn)",
                      layerFov[0].LeftTan, layerFov[0].RightTan, layerFov[0].UpTan,
                      layerFov[0].DownTan, layerFov[1].LeftTan, layerFov[1].RightTan,
                      layerFov[1].UpTan, layerFov[1].DownTan);
            LOG_FIRST(3, "   runtime Fov[0]=(L%.3f R%.3f U%.3f D%.3f) Fov[1]=(L%.3f R%.3f U%.3f D%.3f)",
                      -tanf(s->views[0].fov.angleLeft), tanf(s->views[0].fov.angleRight),
                      tanf(s->views[0].fov.angleUp), -tanf(s->views[0].fov.angleDown),
                      -tanf(s->views[1].fov.angleLeft), tanf(s->views[1].fov.angleRight),
                      tanf(s->views[1].fov.angleUp), -tanf(s->views[1].fov.angleDown));
        }
        LOG_FIRST(3, "   eye assignment: view0<-game[%d] view1<-game[%d] (latched=%d)",
                  srcForView[0], srcForView[1], s->eyeAssign);

        // [RENDPOSE] / [RENDDECL] (SPEC-RENDERED-ORIENTATION): pair this frame with the orientation it was RENDERED with,
        // BEFORE the views are declared, so Stage 2 (declare_rendered_orientation) can replace the located orientation.
        // echo = the game's RenderPose orientation - the head it was handed for this image (identifies the capture).
        float renderedQ[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        bool declareRendered = false;
        if (CamHookRendPoseWanted()) {
            const XrQuaternionf& o0 = s->views[0].pose.orientation;
            const XrQuaternionf& o1 = s->views[1].pose.orientation;
            const float e0[4] = { o0.x, o0.y, o0.z, o0.w };
            const float e1[4] = { o1.x, o1.y, o1.z, o1.w };
            float echo[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            if (renderPose) {
                echo[0] = renderPose[0].Orientation.x; echo[1] = renderPose[0].Orientation.y;
                echo[2] = renderPose[0].Orientation.z; echo[3] = renderPose[0].Orientation.w;
            }
            {   // the AFW gate's eye-position check (afwgate_logic.h) needs this submit's eye separation before the pairing
                const XrVector3f& q0 = s->views[0].pose.position;
                const XrVector3f& q1 = s->views[1].pose.position;
                CamHookAfwNoteEyeSep(sqrtf((q1.x - q0.x) * (q1.x - q0.x) + (q1.y - q0.y) * (q1.y - q0.y) +
                                           (q1.z - q0.z) * (q1.z - q0.z)));
            }
            declareRendered = CamHookRendPoseSubmit(s->posesValid, e0, e1, echo, renderPose != nullptr, g_useRenderPose,
                                                    renderedQ, static_cast<long long>(frameIndex));
        }

        // AFW HELD-EYE POSE (afw_held_eye_pose, 2026-10-03, INV-375). Under afw_alt_eye one eye's Scene replay is skipped per
        // frame and that eye shows its PREVIOUS image (the RIFT target is no longer cleared). Declaring it with THIS frame's
        // rendered orientation tells the compositor it is current, so on a head turn it lags one frame behind the other eye.
        // The held eye is declared with the orientation IT was last rendered with - this frame's renderedQ saved on the
        // previous submit - so the compositor rotates it to the current head. Which eye is held comes from this submit's OWN
        // capture-paired RENDTAG record (CamHookAfwSubmitSkipEye), never from frame order (INV-325: submits lag 2-3 frames).
        // Orientation only: translation, stick turns and moving objects are NOT corrected (that is the warp).
        // Anything unpaired, or a held eye whose previous render was not the previous submit -> this frame's orientation, as
        // before, counted by reason.
        // (Stick-turn correction afw_held_eye_stick_yaw: rejected D-182, removed 2026-10-07.)
        static float     s_afwQ[2][4] = {};
        static long long s_afwQSub[2] = { -10, -10 };
        static long long s_afwSub = 0;
        static long long s_apSubs = 0, s_apAfw = 0, s_apHeld = 0, s_apNoPrev = 0, s_apUnknown = 0, s_apOff = 0;
        static double    s_apSumDeg = 0.0;
        static float     s_apMaxDeg = 0.0f;
        static ULONGLONG s_apNext = 0;
        ++s_afwSub;
        const int afwSkip = declareRendered ? CamHookAfwSubmitSkipEye() : -2;
        // F2 LENS MASK identity (see g_lmLastSkip): check what the mask was told during this image's render, then roll the history.
        {
            const int fresh = (afwSkip == 0 || afwSkip == 1) ? 1 - afwSkip : -1;
            const LONG ans = InterlockedExchange(&g_lmAnswer, -1);
            if (LensMask) {
                if (ans < 0) { ++g_lmNoAnswer; } else if (fresh < 0) { ++g_lmNoRecord; }
                else if (ans == fresh) { ++g_lmAgree; } else { ++g_lmDisagree; }
                static ULONGLONG s_lmNext = 0;
                static bool s_lmFirst = true;
                const ULONGLONG nowMs = GetTickCount64();
                if (s_lmFirst || nowMs >= s_lmNext) {
                    s_lmNext = nowMs + 5000;
                    // [PROBE: LENSMASKID] gate=lens_mask (FIRST line unconditional while on)  site=HEADSET  log-only=yes
                    LOG("[LENSMASKID] %s | eye the mask drew vs this image's own fresh eye (its capture-paired record): agree %lld, "
                        "DISAGREE %lld, no eye handed out %lld, no record at submit %lld | this submit held %d (declare_rendered "
                        "orientation %s). DISAGREE > 0 = the identity is wrong - the mask covers the other eye's region.",
                        s_lmFirst ? "FIRST (logged unconditionally)" : "5 s", g_lmAgree, g_lmDisagree, g_lmNoAnswer, g_lmNoRecord,
                        afwSkip, declareRendered ? "on" : "OFF - no identity, no mask");
                    s_lmFirst = false;
                    g_lmAgree = g_lmDisagree = g_lmNoAnswer = g_lmNoRecord = 0;
                }
            }
            g_lmPrevSkip = g_lmLastSkip;
            g_lmLastSkip = afwSkip;
        }

        // AFW REBUILD (afw_rebuild, AFW step 4b, 2026-10-04). Before the eye loop releases the image: when this image's
        // gate passes (camhook: GPU camera == captured camera, exactly one matrix, the previous image was the other eye)
        // the bootstrap redraws the HELD eye's half of the shared eye image at this image's camera (DlvrAfwRebuild). On
        // success the held eye is declared with THIS image's orientation (afwRebuilt, below) instead of its own older
        // one. Any refusal - counted by reason, one [AFWRB] line per 5 s - leaves today's held eye untouched.
        // Slot 0 is the LEFT eye in the game's matrices (INV-386), so a held slot 1 is the right-hand camera.
        // COST: off -> one bool test. On: see AfwRebuildRun (bootstrap); here one export call and a distance.
        bool afwRebuilt = false;
        {
            typedef int (*PFN_Rebuild)(void*, const int*, const int*, int, float, int);
            static PFN_Rebuild s_fn = nullptr;
            static bool s_looked = false, s_first = true;
            static long long s_try = 0, s_ok = 0, s_gate[8] = {}, s_boot[8] = {}, s_noImg = 0, s_noFn = 0;
            static float s_sepSum = 0.0f, s_resMax = 0.0f;
            static ULONGLONG s_next = 0;
            if (AfwRebuild && AfwAltEye) {
                ++s_try;
                float res = 0.0f;
                const int gate = declareRendered ? CamHookAfwRebuildGate(&res) : 1;
                SwapChain* rc = colorTex ? SC(colorTex[0]) : nullptr;
                if (!s_looked) {
                    if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                        s_looked = true;
                        s_fn = reinterpret_cast<PFN_Rebuild>(GetProcAddress(b, "DlvrAfwRebuild"));
                    }
                }
                int boot = -1;
                float sep = 0.0f;
                if (gate != 0 || afwSkip < 0 || afwSkip > 1) {
                    ++s_gate[(gate > 0 && gate < 8) ? gate : 1];
                } else if (!s_fn) {
                    ++s_noFn;
                } else if (!rc || !rc->acquired || rc->index >= rc->images.size() || !rc->images[rc->index].texture ||
                           (colorTex[1] && colorTex[1] != colorTex[0]) || !viewport || !s->posesValid) {
                    ++s_noImg;
                } else {
                    const int held = afwSkip, fresh = 1 - afwSkip;
                    const int hv[4] = { viewport[held].Pos.x, viewport[held].Pos.y, viewport[held].Size.w, viewport[held].Size.h };
                    const int fv[4] = { viewport[fresh].Pos.x, viewport[fresh].Pos.y, viewport[fresh].Size.w, viewport[fresh].Size.h };
                    const XrVector3f& p0 = s->views[0].pose.position;
                    const XrVector3f& p1 = s->views[1].pose.position;
                    sep = sqrtf((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y) + (p1.z - p0.z) * (p1.z - p0.z));
                    boot = s_fn(rc->images[rc->index].texture, hv, fv, held == 1 ? 1 : 0, sep,
                                (AfwRebuildTint ? 1 : 0) | (DiagAfwRebuildCheck ? 2 : 0) | (AfwRbFarRule ? 4 : 0) |
                                (AfwRbStale ? 8 : 0) | (AfwRbEdgeRescue ? 16 : 0) | (DiagAfwRebuildRules ? 32 : 0) |
                                /* bit 6 retired 2026-10-08 (afw_rb_fg_rules) */ (DiagAfwFgMotion ? 128 : 0) | (DiagAfwHeldMap ? 256 : 0) |
                                (AfwRbFgMove ? 512 : 0) | (DiagAfwTrailSrc ? 1024 : 0) | (DiagAfwCapture ? 2048 : 0) |
                                // bits 12 / 13 / 14 retired 2026-10-07 (nearmiss_world / fill_world D-192, bicubic D-194)
                                (AfwRbEdgeHold ? 32768 : 0) |
                                ((AfwRbEdgeHoldRadius < 1 ? 1 : AfwRbEdgeHoldRadius > 3 ? 3 : AfwRbEdgeHoldRadius) << 16) |
                                ((AfwRbFgMove && AfwRbFgMoveWeapon) ? 262144 : 0) |
                                (AfwRbFgEdgeRing ? (1 << 19) : 0) | (AfwRbFillNear ? (1 << 20) : 0) |
                                (AfwRbSeedIndexed ? (1 << 21) : 0));
                    if (boot == 0) {
                        afwRebuilt = true;
                        ++s_ok;
                        s_sepSum += sep;
                        if (res > s_resMax) { s_resMax = res; }
                    } else {
                        ++s_boot[(boot > 0 && boot < 8) ? boot : 7];
                    }
                }
                const ULONGLONG nowMs = GetTickCount64();
                if (s_next == 0) { s_next = nowMs + 5000; }
                if (s_first || nowMs >= s_next) {
                    if (!s_first) { s_next = nowMs + 5000; }
                    // [PROBE: AFWRB] gate=afw_rebuild + afw_alt_eye (+ declare_rendered_orientation for the pairing, dlss_cbsnap.txt for the matrices, the bootstrap's VR tonemap path for the depth)  site=HEADSET  log-only=no
                    LOG_WINDOW(s_first, "[AFWRB] %s: submits %lld | REBUILT %lld (eye separation mean %.2f mm, gate residual max %.3f deg) | "
                        "refused by the gate: not paired %lld, no single matrix %lld, torn %lld, GPU camera != captured "
                        "%lld, previous image not the other eye %lld, STALE camera %lld, eye position %lld | refused by "
                        "the bootstrap: not ready %lld, depth "
                        "copies < 2 or no matrix %lld, depth copies not adjacent %lld, D3D object failed %lld, viewport "
                        "%lld, matrix %lld, other %lld | no acquired eye image %lld, DlvrAfwRebuild missing %lld | commits "
                        "deferred %ld, chain used before our release %ld | this submit: gate %d (%s), bootstrap %d, held "
                        "slot %d, eye assign %d | afw_rebuild=1 tint=%d",
                        s_first ? "FIRST (logged unconditionally)" : "5 s", s_try, s_ok,
                        s_ok ? 1000.0f * s_sepSum / static_cast<float>(s_ok) : 0.0f, s_resMax, s_gate[1], s_gate[2],
                        s_gate[3], s_gate[4], s_gate[5], s_gate[6], s_gate[7], s_boot[1], s_boot[2], s_boot[3], s_boot[4],
                        s_boot[5], s_boot[6],
                        s_boot[7], s_noImg, s_noFn, g_afwRbDeferred, g_afwRbEarlyUse, gate, CamHookAfwRebuildGateName(gate),
                        boot, afwSkip, s->eyeAssign, AfwRebuildTint ? 1 : 0);
                    if (!s_first) {
                        s_try = s_ok = s_noImg = s_noFn = 0;
                        memset(s_gate, 0, sizeof(s_gate));
                        memset(s_boot, 0, sizeof(s_boot));
                        s_sepSum = 0.0f; s_resMax = 0.0f;
                    }
                    s_first = false;
                }
            }
        }

        // THE IN-WORLD PROMPT (trace docs/traces/20261123_1535_inworld_prompt_routed.md): the bootstrap copies the routed prompt widget into
        // our quad's live texture - once per submit, whatever owns the HUD pass (the HUD panel included). inworld_prompts off at launch:
        // one bool test. COST on: one export call; a 768 x 192 clear + scaled copy only while the prompt is routed.
        // [LOADBEARING: inworld_prompts] the per-submit prompt copy
        if (::dlvr::PromptQuadOnThisLaunch()) {
            typedef int (*PFN_PromptCopy)(float, float);   // DlvrPromptCopy(fade, plate) - ui_prompt_brightness / ui_prompt_plate, live
                                                           // (traces 20261123_1700, 20261123_1830)
            static PFN_PromptCopy s_pc = nullptr;
            static bool s_pcLooked = false;
            if (!s_pcLooked) {
                if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                    s_pcLooked = true;
                    s_pc = reinterpret_cast<PFN_PromptCopy>(GetProcAddress(b, "DlvrPromptCopy"));
                    // [PROBE: PROMPTQ] gate=inworld_prompts  site=FLAT  log-only=yes
                    LOG("promptquad: [PROMPTQ] the bootstrap's DlvrPromptCopy %s (logged unconditionally)", s_pc ? "found - called each submit"
                        : "*** NOT FOUND - the prompt is never copied (an older bootstrap?) ***");
                }
            }
            if (s_pc) { s_pc(::dlvr::InworldPromptBrightness, ::dlvr::InworldPromptPlate); }
        }

        // AFW HUD LAYER v4 (2026-10-07; the bootstrap's dlvr\hud_layer.txt + DlvrHudLayerSubmit, INV-482 / INV-483). Under
        // afw_alt_eye the bootstrap keeps the bracketed HUD pass (sprite50.ppfx) OUT of each eye's RIFT target, so the rebuild
        // above never re-projects a HUD; each eye's HUD is laid on its viewport of the shared eye image here - after the
        // rebuild, before the eye loop releases the image. Every call is also the bootstrap's handshake: without it (AFW off,
        // or the export missing) the HUD passes are not redirected and draw into RIFT as stock. Marker absent -> the
        // bootstrap answers 2 and does nothing.
        // COST: off -> one bool test; on -> one export call per submit (+ up to two full-eye blends in the bootstrap).
        {
            typedef int (*PFN_Hud)(void*, const int*, const int*);
            static PFN_Hud s_hud = nullptr;
            static bool s_hudLooked = false, s_hudFirst = true;
            static long long s_hudCalls = 0, s_hudRc[8] = {}, s_hudNoImg = 0;
            static ULONGLONG s_hudNext = 0;
            // ui_panel_menus: while the menu panel owns the pass (a menu shows) it is NOT laid on the eyes - uipanel's own
            // call is the handshake then. One submit of lag at each change (Active() is last submit's answer).
            if (AfwAltEye && !::dlvr::uipanel::Active()) {
                if (!s_hudLooked) {
                    if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
                        s_hudLooked = true;
                        s_hud = reinterpret_cast<PFN_Hud>(GetProcAddress(b, "DlvrHudLayerSubmit"));
                    }
                }
                SwapChain* hc = colorTex ? SC(colorTex[0]) : nullptr;
                if (s_hud && hc && hc->acquired && hc->index < hc->images.size() && hc->images[hc->index].texture &&
                    !(colorTex[1] && colorTex[1] != colorTex[0]) && viewport) {
                    const int va[4] = { viewport[0].Pos.x, viewport[0].Pos.y, viewport[0].Size.w, viewport[0].Size.h };
                    const int vb[4] = { viewport[1].Pos.x, viewport[1].Pos.y, viewport[1].Size.w, viewport[1].Size.h };
                    const int rc = s_hud(hc->images[hc->index].texture, va, vb);
                    ++s_hudCalls;
                    ++s_hudRc[(rc >= 0 && rc < 7) ? rc : 7];
                } else if (s_hud) {
                    ++s_hudNoImg;
                }
                const ULONGLONG nowMs = GetTickCount64();
                if (s_hudLooked && (s_hudFirst || nowMs >= s_hudNext)) {
                    s_hudNext = nowMs + 5000;
                    // [PROBE: AFWHUD] gate=afw_alt_eye + the bootstrap's dlvr\hud_layer.txt  site=HEADSET  log-only=no
                    LOG("[AFWHUD] %s: DlvrHudLayerSubmit %s | calls %lld -> laid on %lld, nothing to lay %lld, HUD layer off / "
                        "COUNT %lld, refused (viewport / image) %lld, D3D refused %lld, bootstrap off %lld, other %lld | no "
                        "single acquired eye image %lld", s_hudFirst ? "FIRST (logged unconditionally)" : "5 s",
                        s_hud ? "found" : "NOT exported (older bootstrap) - the HUD stays in RIFT", s_hudCalls,
                        s_hudRc[0], s_hudRc[1], s_hudRc[2], s_hudRc[3], s_hudRc[5], s_hudRc[6], s_hudRc[4] + s_hudRc[7],
                        s_hudNoImg);
                    s_hudFirst = false;
                    s_hudCalls = s_hudNoImg = 0;
                    memset(s_hudRc, 0, sizeof(s_hudRc));
                }
            }
        }

        // WRIST HUD STAGE A (trace docs/traces/20261113_1300_wrist_ingame_stageA.md): the pieces on the drawn forearm / hand, drawn INTO
        // the acquired eye image - after the rebuild and the HUD lay-on (so the rebuild never carries them), before the release, with or
        // without AFW. The same image expression as the HUD lay-on above. Off (ui_wrist_hud not on this launch): wristquad's g_on test.
        {
            SwapChain* wc = colorTex ? SC(colorTex[0]) : nullptr;
            if (wc && wc->acquired && wc->index < wc->images.size() && wc->images[wc->index].texture &&
                !(colorTex[1] && colorTex[1] != colorTex[0]) && viewport) {
                const int wa[4] = { viewport[0].Pos.x, viewport[0].Pos.y, viewport[0].Size.w, viewport[0].Size.h };
                const int wb[4] = { viewport[1].Pos.x, viewport[1].Pos.y, viewport[1].Size.w, viewport[1].Size.h };
                float wsep = 0.0f;
                if (s->posesValid) {
                    const XrVector3f& q0 = s->views[0].pose.position;
                    const XrVector3f& q1 = s->views[1].pose.position;
                    wsep = sqrtf((q1.x - q0.x) * (q1.x - q0.x) + (q1.y - q0.y) * (q1.y - q0.y) + (q1.z - q0.z) * (q1.z - q0.z));
                }
                // The AFW pick (fresh = newest latch, held = previous / moved) only when the held eye is KNOWN (afwSkip 0 / 1). A both-
                // rendered (-1) or unknown (-2) frame was drawn as "eye 0 held" - when eye 1 was held, both eyes took the other eye's
                // camera + rows (a one-frame sideways jump; Jay 2026-11-14). Those frames take each eye's own latest latch (the AFW-off
                // pick). Trace docs/traces/20261114_1300_wrist_unknown_skip.md.
                const bool wAfw = AfwAltEye && (afwSkip == 0 || afwSkip == 1);
                // The held eye's previous render is the newest-but-one latch only if the PREVIOUS submit rendered this held eye (it held
                // the other one). Trace docs/traces/20261114_1315_wrist_heldprev_proof.md.
                const bool wPrevHeld = wAfw && g_lmPrevSkip == 1 - afwSkip;
                ::dlvr::wristquad::DrawInGame(wc->images[wc->index].texture, wa, wb, wAfw, afwRebuilt, afwSkip, wPrevHeld, wsep);
            }
        }

        // [EYEALPHA] (dlvr\diag_eye_alpha.txt, log-only): the final shared eye image - after the rebuild and the HUD layer,
        // before the release below - is what the compositor blends with SOURCE_ALPHA. Off: one bool test.
        {
            SwapChain* ac = colorTex ? SC(colorTex[0]) : nullptr;
            if (ac && ac->acquired && ac->index < ac->images.size() && !(colorTex[1] && colorTex[1] != colorTex[0])) {
                eyealpha::Tick(ac->images[ac->index].texture, viewport, afwSkip, afwRebuilt, AfwAltEye);
            }
        }

        static long long s_sdSubmit = 0;   // submit_drawn_image: one release per chain per submit
        ++s_sdSubmit;
        for (int e = 0; e < 2; ++e) {
            int src = srcForView[e];
            if (g_swapEyes) {
                src = 1 - src;  // manual override on top of the automatic choice
            }

            ovrTextureSwapChain handle = colorTex[src];
            if (!handle) {
                handle = colorTex[0];
            }
            SwapChain* c = SC(handle);
            if (!c || c->handle == XR_NULL_HANDLE) {
                usable = false;
                break;
            }
            // The loading-screen void copies only this view's chain, before the release below.
            ::dlvr::loadscreen::NoteEyeChain(e, c->handle, static_cast<int64_t>(ToDxgi(c->desc.Format)));
            // DESKTOP MIRROR, ONE EYE (deskmirror.h; Jay 2026-11-14 "The desktop window should show what the headset sees"; trace
            // docs/traces/20261114_1030_desktop_mirror_single_eye.md). The engine copies the mirror texture into its window after
            // every submit (INV-379), so this fills it with view 1's (the right eye's) image - the texture and rectangle projViews[1] is given -
            // cropped to the window's shape. mirror=1: live, at most 60 per second. The overlay still forces a backdrop every 8th
            // submit while the mod menu is open, so the panel (OverlayRenderToMirror, after the layers) has the game behind it.
            // Drawn BEFORE this chain's release below: an image may only be read while it is acquired. The old CopyResource here
            // never ran: the eye images are TYPELESS (27) and the mirror sRGB (29), so it refused on every submit.
            constexpr unsigned kBackdropEveryNFrames = 8;
            static unsigned backdropTick = 0;
            if (e == 0 && (g_mirror || OverlayVisible()) && !s->mirrors.empty() && s->mirrors[0]->tex) {
                const bool backdropDue = OverlayVisible() && (backdropTick % kBackdropEveryNFrames) == 0;
                if (OverlayVisible() && !g_mirror) {
                    ++backdropTick;
                }
                const int why = ::dlvr::deskmirror::Due(g_mirror, backdropDue);
                if (why != 0) {
                    // The view the window shows (deskmirror::kMirrorView = 1, the RIGHT eye; Jay 2026-11-16; trace
                    // 20261116_1925_desktop_mirror_right_eye): drawn HERE, in view 0's pass, because a shared chain is released
                    // in this pass below - but from that view's own source, picked exactly as the loop picks src.
                    int msrc = srcForView[::dlvr::deskmirror::kMirrorView];
                    if (g_swapEyes) {
                        msrc = 1 - msrc;
                    }
                    SwapChain* mc = SC(colorTex[msrc] ? colorTex[msrc] : colorTex[0]);
                    if (!mc || mc->handle == XR_NULL_HANDLE) {
                        mc = c;
                        msrc = src;
                    }
                    ID3D11Texture2D* shown = mc->sdMode ? SdShownTexture(mc)
                                                        : ((mc->acquired && mc->index < mc->images.size()) ? mc->images[mc->index].texture
                                                                                                           : nullptr);
                    const int vr[4] = { viewport[msrc].Pos.x, viewport[msrc].Pos.y, viewport[msrc].Size.w, viewport[msrc].Size.h };
                    if (::dlvr::deskmirror::Draw(s->device, s->mirrors[0]->tex, shown, ToDxgi(mc->desc.Format), vr, msrc, why)) {
                        mirrorRefreshed = true;
                    }
                }
            }
            if (e == 0) {   // once per submit
                SdReport();
            }
            // [LOADBEARING: SUBMIT-DRAWN-IMAGE] with submit_drawn_image=1 this release decides WHICH image the compositor
            //      shows: the oldest held = the one this replayed frame drew (INV-429). Releasing the newest instead shows the
            //      composite of two frames earlier under this frame's pose. Once per chain per submit (both eyes share it).
            if (c->sdMode && c->relSubmit != s_sdSubmit) {
                c->relSubmit = s_sdSubmit;
                if (!SubmitDrawnImage || c->sdFault) {   // switched OFF, or a runtime call failed: release everything held
                    const bool fault = c->sdFault;
                    SdRelease(c, true);
                    AcquireSRWLockExclusive(&g_sdQLock);
                    c->sdMode = false;
                    c->sdFault = false;
                    c->commitSinceAcquire = false;
                    ReleaseSRWLockExclusive(&g_sdQLock);
                    InterlockedIncrement(&g_sdSwitchOff);
                    if (fault) { g_sdFaultLatched = true; }
                    LOG("[SUBDRAWN] submit_drawn_image %s at submit %lld - every held image released, the chain is back on "
                        "the old path (release at commit / submit)%s", fault ? "*** LEFT ON A RUNTIME ERROR ***" : "switched OFF",
                        s_sdSubmit, fault ? ". The XrResult is logged just above. It stays on the old path until "
                        "submit_drawn_image is unticked and ticked again." : "");
                } else {
                    const int held = SdReleaseDrawn(c);
                    InterlockedIncrement(held >= 2 ? &g_sdRelHeld2 : (held == 1 ? &g_sdRelHeld1 : &g_sdRelNone));
                }
            } else if (!c->sdMode) {
                // Release before xrEndFrame. With a shared texture both eyes map to
                // the same swapchain, so the second pass finds it already released.
                if (c->acquired) {
                    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    ::dlvr::loadscreen::CaptureBeforeRelease(c->handle,
                                                             c->index < c->images.size() ? c->images[c->index].texture : nullptr);
                    xrReleaseSwapchainImage_(c->handle, &ri);
                    c->acquired = false;
                }
                if (!SubmitDrawnImage) { g_sdFaultLatched = false; }   // unticked: a later tick may try again
                if (SubmitDrawnImage && !g_sdFaultLatched) {   // switched ON (or first submit with it on): nothing held; start
                    AcquireSRWLockExclusive(&g_sdQLock);
                    c->sdMode = true;
                    c->nq = 0;
                    c->commitSinceAcquire = false;
                    c->relSubmit = s_sdSubmit;
                    g_sdDrawnTex = nullptr;   // a composite seen before the switch belongs to the old path
                    ReleaseSRWLockExclusive(&g_sdQLock);
                    InterlockedIncrement(&g_sdSwitchOn);
                    // [PROBE: SUBDRAWN] gate=submit_drawn_image (switch lines unconditional; 5 s tallies with diag_swap_order)  site=HEADSET  log-only=no
                    LOG("[SUBDRAWN] submit_drawn_image switched ON at submit %lld for chain %p (%zu images) - from the next "
                        "hand-out each submit releases the image its frame drew (the oldest held). The first frame after "
                        "the switch still shows the old pairing once.", s_sdSubmit, static_cast<void*>(c), c->images.size());
                }
            }
            c->deferPending = false;

            projViews[e] = XrCompositionLayerProjectionView{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            XrPosef declared = g_useRenderPose
                                   ? ToXr(renderPose[src])
                                   : (s->posesValid ? s->views[e].pose
                                                    : ToXr(renderPose[src]));
            // [LOADBEARING: DECLARE-RENDERED-ORIENTATION] with declare_rendered_orientation=1 and a paired frame, this is
            //      the ONLY thing that tells the compositor which head yaw is inside the image (INV-323: the rendered yaw
            //      trails the located pose ~1-3 deg on fast turns; declaring the located pose says it is current).
            //      Orientation only, before PitchPose; the eye keeps its own rotation offset from view 0; position as before.
            if (declareRendered && !g_useRenderPose && s->posesValid) {
                const float* q = renderedQ;
                if (afwSkip >= 0 && src == afwSkip && !afwRebuilt) {   // a REBUILT held eye is at this image's orientation
                    if (!AfwHeldEyePose) {
                        ++s_apOff;
                    } else if (s_afwQSub[src] == s_afwSub - 1) {
                        q = s_afwQ[src];
                        ++s_apHeld;
                        const float d = fabsf(q[0] * renderedQ[0] + q[1] * renderedQ[1] + q[2] * renderedQ[2] + q[3] * renderedQ[3]);
                        const float deg = 2.0f * acosf(d > 1.0f ? 1.0f : d) * (180.0f / 3.14159265358979f);
                        s_apSumDeg += deg;
                        if (deg > s_apMaxDeg) { s_apMaxDeg = deg; }
                    } else {
                        ++s_apNoPrev;
                    }
                }
                const XrQuaternionf r{ q[0], q[1], q[2], q[3] };
                const XrQuaternionf& v0 = s->views[0].pose.orientation;
                const XrQuaternionf inv0{ -v0.x, -v0.y, -v0.z, v0.w };
                declared.orientation = QuatMul(r, QuatMul(inv0, s->views[e].pose.orientation));
            }

            float pitch = g_pitchOffsetDeg;
            if (g_autoPitch && pitch == 0.0f) {
                // Angular bisector of the runtime's vertical FOV - the
                // direction the panel is actually centred on.
                //
                // This must be the mean of the ANGLES, not atan of the mean of
                // the tangents. Tangent-space averaging gives the centre for a
                // SHEAR (an off-axis frustum offset on the image plane); we are
                // applying a ROTATION, so the correct value is the bisector.
                // The two diverge sharply at wide angles - on Quest 3, with
                // angleUp +44.0 and angleDown -55.0, tangent-space says -13.0
                // degrees while the true bisector is -5.5, and -5.5 is the one
                // that looks right in the headset.
                const XrFovf& rt = s->views[e].fov;
                pitch = (rt.angleUp + rt.angleDown) * 0.5f * 180.0f / 3.14159265358979f;
            }
            if (e == 0) { menuRendered0 = declared.orientation; }   // ui_panel_menus: the frame the image was rendered in
            declared = PitchPose(declared, pitch);
            LOG_FIRST(3, "   eye%d declared pose pitched by %.2f deg (frustum untouched)",
                      e, pitch);
            projViews[e].pose = declared;
            // Declare the FOV the game actually rendered with - the value it
            // echoes back in ovrLayerEyeFov::Fov, which is the one GameEyeFov
            // handed it via ovr_GetHmdDesc. This is the load-bearing invariant
            // of the whole layer: declared MUST equal rendered. Every previous
            // attempt to "improve" the declared FOV (substituting the runtime's
            // asymmetric values, re-centring it, extending it vertically) was a
            // lie about the pixels, and the compositor duly resampled them into
            // a warped image.
            //
            // Coverage is not this call's problem. It is solved upstream, by
            // handing the game a symmetric frustum that already contains the
            // lens - so declaring that frustum verbatim leaves nothing missing
            // at any edge, and no black bars.
            XrFovf useFov = layerFov ? ToXrFov(layerFov[src]) : s->views[e].fov;
            if (g_fovCenterCorrect && layerFov) {
                // Work in tangent space: keep the half-extents the game drew,
                // move their centre onto the runtime's optical centre.
                const XrFovf& rt = s->views[e].fov;
                const float cx = (tanf(rt.angleRight) + tanf(rt.angleLeft)) * 0.5f;
                const float cy = (tanf(rt.angleUp) + tanf(rt.angleDown)) * 0.5f;
                const float hx = (tanf(useFov.angleRight) - tanf(useFov.angleLeft)) * 0.5f;
                const float hy = (tanf(useFov.angleUp) - tanf(useFov.angleDown)) * 0.5f;
                useFov.angleRight = atanf(cx + hx);
                useFov.angleLeft = atanf(cx - hx);
                useFov.angleUp = atanf(cy + hy);
                useFov.angleDown = atanf(cy - hy);
                LOG_FIRST(3, "   fov recentre eye%d: runtime centre (%.4f, %.4f), "
                             "half-extent (%.4f, %.4f)", e, cx, cy, hx, hy);
            }
            projViews[e].fov = useFov;
            // WRIST HUD fix round 2 (trace docs/traces/20261113_1330_wrist_ingame_A2.md): the declared pose + FOV the compositor will show
            // eye e with - the wrist placement maps a dropped piece through it. Off: wristquad's g_on test.
            ::dlvr::wristquad::NoteDeclared(e, projViews[e].pose, projViews[e].fov);
            // F2 LENS MASK (dlvr_LensMask): what the runtime can display of the image the GAME's eye `src` drew - this view's
            // runtime FOV, in the frame of the pose and FOV declared for that image (declared = eye * Rx(pitch), PitchPose).
            if (src == 0 || src == 1) {
                LensMaskEye& lm = g_lmEye[src];
                lm.img[0] = -tanf(useFov.angleLeft); lm.img[1] = tanf(useFov.angleRight);
                lm.img[2] = tanf(useFov.angleUp);    lm.img[3] = -tanf(useFov.angleDown);
                const XrFovf& rf = s->views[e].fov;
                lm.rt[0] = -rf.angleLeft; lm.rt[1] = rf.angleRight; lm.rt[2] = rf.angleUp; lm.rt[3] = -rf.angleDown;
                lm.pitchDeg = pitch;
                lm.ok = lm.img[0] > 0.0f && lm.img[1] > 0.0f && lm.img[2] > 0.0f && lm.img[3] > 0.0f &&
                        lm.rt[0] > 0.0f && lm.rt[1] > 0.0f && lm.rt[2] > 0.0f && lm.rt[3] > 0.0f;
                if (layerFov) {   // INV-735: the game's own frustum for this image (uipanel::GameEyeTan / dlvr_GameEyeTan)
                    lm.game[0] = layerFov[src].LeftTan; lm.game[1] = layerFov[src].RightTan;
                    lm.game[2] = layerFov[src].UpTan;   lm.game[3] = layerFov[src].DownTan;
                    lm.gameOk = lm.game[0] > 0.0f && lm.game[1] > 0.0f && lm.game[2] > 0.0f && lm.game[3] > 0.0f;
                }
            }
            projViews[e].subImage.swapchain = c->handle;
            projViews[e].subImage.imageArrayIndex = 0;
            // projViews[e] is the OpenXR view (0=left, 1=right) and keeps that
            // eye's pose and fov; only the texture region we sample follows the
            // swap, since that is what the game packed the image into.
            projViews[e].subImage.imageRect.offset.x = viewport[src].Pos.x;
            projViews[e].subImage.imageRect.offset.y = viewport[src].Pos.y;
            projViews[e].subImage.imageRect.extent.width = viewport[src].Size.w;
            projViews[e].subImage.imageRect.extent.height = viewport[src].Size.h;
            lsVp[e] = projViews[e].subImage.imageRect;   // the loading-screen void cuts this eye's HUD box from here
            if (e == 0) { imgFov0 = projViews[0].fov; }
            if (e == 1) { imgFov1 = projViews[1].fov; }
            // [LOADBEARING: STEAMVR-PANEL-CROP] on SteamVR the view handed to xrEndFrame is cut to the runtime's panel window (pixels +
            //      their exact frustum). Removing it puts back the Pimax ("aapvr") doubled image: that driver shows a layer with the
            //      panel's frustum, not the layer's (Crystal Light, 2026-11-18; correct with the SteamVR dashboard open).
            // [LOADBEARING: FOV-META-OWN-FOV] and on Meta's runtime, which shows the rectangle over its own FOV (2026-11-24).
            if (g_runtimeIsSteamVr || g_runtimeStretchesFov) {
                SteamVrPanelCrop(e, projViews[e], s->views[e].fov, pitch, layerFov != nullptr, g_runtimeStretchesFov);
            }
        }

        // AFW held-eye pose: remember the orientation each RENDERED eye of this image was drawn with (the held eye keeps
        // its older one), then the 5 s [AFWPOSE] window.
        if (usable && declareRendered && !g_useRenderPose && s->posesValid) {
            for (int g = 0; g < 2; ++g) {
                if (afwSkip >= 0 && g == afwSkip) { continue; }
                memcpy(s_afwQ[g], renderedQ, sizeof(renderedQ));
                s_afwQSub[g] = s_afwSub;
            }
        } else {
            s_afwQSub[0] = s_afwQSub[1] = -10;   // a gap: the next held eye falls back rather than use a stale pose
        }
        ++s_apSubs;
        if (afwSkip >= 0) { ++s_apAfw; } else if (afwSkip == -2 && AfwAltEye) { ++s_apUnknown; }
        {
            const ULONGLONG nowMs = GetTickCount64();
            if (s_apNext == 0) { s_apNext = nowMs + 5000; }
            if (nowMs >= s_apNext) {
                s_apNext = nowMs + 5000;
                if (AfwAltEye || s_apAfw > 0) {
                    // [PROBE: AFWPOSE] gate=afw_alt_eye (+ afw_held_eye_pose, declare_rendered_orientation; pairing needs diag_bone_blend_probe for the RENDTAG recorder detour)  site=HEADSET  log-only=no
                    LOG_PERIODIC("[AFWPOSE] 5 s: submits %lld | carried a skipped eye %lld | HELD eye declared with its own last "
                        "orientation %lld (correction mean %.3f deg, max %.3f deg) | held but previous render not the "
                        "previous submit -> current pose %lld | afw_held_eye_pose OFF -> current pose %lld | AFW on but skip "
                        "unknown (unpaired) %lld | afw_held_eye_pose=%d",
                        s_apSubs, s_apAfw, s_apHeld, s_apHeld ? s_apSumDeg / s_apHeld : 0.0, s_apMaxDeg, s_apNoPrev, s_apOff,
                        s_apUnknown, AfwHeldEyePose ? 1 : 0);
                }
                s_apSubs = s_apAfw = s_apHeld = s_apNoPrev = s_apUnknown = s_apOff = 0;
                s_apSumDeg = 0.0;
                s_apMaxDeg = 0.0f;
            }
        }

        // Investigation, 2026-09-04: a VR-only, ground-plane-only, distance-
        // dependent line/dot rendering artifact. camhook.cpp's own tracked
        // camera objects turned out to be a PRE-eye-offset representation
        // (identical position for both "eyes" - a red herring, not a bug),
        // so the comparison has to happen here instead, on the actual final
        // poses declared to OpenXR (projViews[e].pose) - the real thing that
        // determines what gets rendered per eye. Real stereo displays should
        // differ the two eyes by translation (IPD) only, never rotation; a
        // tiny residual angular mismatch here would be nearly invisible for
        // near-field convergence but could show up as ghosting/double-
        // geometry on a large, flat, distant surface like the ground - worth
        // checking directly since g_autoPitch computes its correction
        // independently per eye from that eye's own FOV.
        if (g_diagEyePoseCompare) {
            const XrQuaternionf& o0 = projViews[0].pose.orientation;
            const XrQuaternionf& o1 = projViews[1].pose.orientation;
            const float dotQ = o0.x * o1.x + o0.y * o1.y + o0.z * o1.z + o0.w * o1.w;
            const float c = fabsf(dotQ) > 1.0f ? 1.0f : fabsf(dotQ);
            const float angleDeg = 2.0f * acosf(c) * (180.0f / 3.14159265358979f);
            static LONG64 lastMs = 0;
            const LONG64 now = static_cast<LONG64>(GetTickCount64());
            if (now - lastMs > 2000) {
                lastMs = now;
                const float dx = projViews[0].pose.position.x - projViews[1].pose.position.x;
                const float dy = projViews[0].pose.position.y - projViews[1].pose.position.y;
                const float dz = projViews[0].pose.position.z - projViews[1].pose.position.z;
                const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                LOG("eyepose diag: angleBetween=%.4f deg  interEyeDist=%.4f  "
                    "o0=(%.5f,%.5f,%.5f,%.5f) o1=(%.5f,%.5f,%.5f,%.5f)",
                    angleDeg, dist, o0.x, o0.y, o0.z, o0.w, o1.x, o1.y, o1.z, o1.w);
            }
        }

        if (!usable) {
            LOG_FIRST(4, "   layer[%u] unusable: a ColorTexture was null/unknown", i);
            continue;
        }

        // Blank the other eye by collapsing its rect to nothing; the layer must
        // still carry both views for a stereo view configuration.
        if (g_debugSingleEye == 1 || g_debugSingleEye == 2) {
            const int blank = (g_debugSingleEye == 1) ? 1 : 0;
            projViews[blank].subImage.imageRect.extent.width = 1;
            projViews[blank].subImage.imageRect.extent.height = 1;
            LOG_FIRST(2, "   debug_single_eye=%d: view %d blanked",
                      g_debugSingleEye, blank);
        }

        proj.space = s->appSpace;
        proj.viewCount = 2;
        proj.views = projViews;
        proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        // Calibration screen, build B: the VOID - while the screen is up at the main menu the game image is not submitted
        // (ruling B4; VDXR's xrEndFrame accepts a quad-only frame - no check requires a projection layer). The eye images are
        // still acquired / released as always; proj itself is still built (the menu panel and the reticles read
        // proj.viewCount). The flag is the PREVIOUS submit's (CalibScreenFrameBegin runs below, every submit) - one frame of
        // lag at on / off. COST: one bool read.
        // [LOADBEARING: LOADSCREEN-VOID] while the engine draws its background path (loading screens) the game image is NOT
        //      submitted: declared at the head's own pose it sits stuck to the face at ~32 fps (Jay 2026-10-20, motion
        //      sickness reports). Removing this test puts the face-locked picture and the stale menu border back.
        // [LOADBEARING: FOV-LIVE-EYE] the flicker guard: after the FOV switch the game image stays off the display for kHoldSubmits more
        //      submits (frames recorded before it), and the switch itself asks whether the last submit showed one (eyefov_logic.h).
        const bool wouldShow = !::dlvr::CalibScreenVoid() && !::dlvr::loadscreen::SkipProjection(::dlvr::CalibScreenVoid());
        const bool mayShow = fovNoted ? (wouldShow && !g_liveFov.lastHidden) : dlvr::eyefov::NoteSubmit(g_liveFov, wouldShow);
        fovNoted = true;
        if (mayShow) {
            layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
        }
    }
    if (!fovNoted) {
        dlvr::eyefov::NoteSubmit(g_liveFov, false);   // no game image this submit
    }

    // Calibration screen, build B: the once-per-launch auto-open, the void flag (read by the NEXT submit's projection
    // decision above), the floor space. Every submit, whether or not a game image was built this frame.
    // COST closed + the auto-open decided: one bool test + one volatile read.
    ::dlvr::CalibScreenFrameBegin(s->session, s->appSpace);

    // The overlay goes last so it composites over the game. Polling here keeps
    // it on the render thread with everything else that touches D3D, and means
    // it cannot be toggled halfway through building a frame.
    OverlayPoll();
    // Menu pin (ui_pin_menus, SPEC-UI-MENU-PIN-2026-10-11): once per submit - finds the game's menu level and tells the
    // engine-side detour which UI camera to rewrite. Off and never switched on: one bool test.
    ::dlvr::uipin::Tick();
    // Cutscene head turn (INV-620 (c)): once per submit - names the cutscene's render root while a cutscene owns the look. Off: one load.
    ::dlvr::CamHookCutsceneTick();
    // Game-menu input pass-through: the game's menu state, published for the scePad filter (uipin.h). COST: one level walk.
    ::dlvr::uipin::PublishMenuState();

    // Horizon lock is a setting, so it has to come up on its own. Installing
    // the hook needs the engine mapped, which it is by the time we are building
    // a frame, and it is cheap enough to attempt once here rather than invent a
    // separate init point. Retried while the pattern search keeps failing, so a
    // hook that could not resolve during the first frames still lands later.
    if (dlvr::HorizonLock() && CamHookAuto && !CamHookInstalled()) {
        static unsigned tries = 0;
        if (tries < 600 && (tries % 60) == 0) {
            CamHookInstall();
        }
        ++tries;
    }

    dlvr::PullCamHookOp();   // the overlay may have changed cam_hook_op this frame
    RttiUpdateCameras();
    // fire_reticle_3d anchors the gun dot to the poses the game image is declared with THIS frame (proj.viewCount is
    // set only when a usable projection layer was built above).
    OverlaySetDeclaredViews(proj.viewCount == 2, projViews[0].pose, projViews[1].pose);
    // MENU PANEL (ui_panel_menus, SPEC-UI-MENU-PANEL-2026-10-11): while a menu shows, the bracketed sprite pass goes on a
    // room-fixed quad. Directly after the game image and UNDER the settings panel and the reticles. Off = one bool test.
    const XrPosef menuEyes[2] = { projViews[0].pose, projViews[1].pose };
    // The desktop window: the menu panel is laid over it only on a submit that redrew it (deskmirror.h DrawPanel, trace 20261114_1125).
    ::dlvr::deskmirror::SetFresh(mirrorRefreshed && !s->mirrors.empty() ? s->mirrors[0]->tex : nullptr);
    const int menuQuads = ::dlvr::uipanel::Render(s->appSpace, proj.viewCount == 2, menuEyes, menuRendered0, imgFov0, imgFov1,
                                                  menuQuad, s->frameState.predictedDisplayTime);
    for (int mq = 0; mq < menuQuads && submitted < 5; ++mq) {
        layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad[mq]);
    }
    // LOADING-SCREEN VOID (loadscreen.h): the picture on its room-fixed quad pair, under the settings panel. Not loading = one
    // bool test.
    const int loadQuads = ::dlvr::loadscreen::Render(s->appSpace, proj.viewCount == 2, s->posesValid, menuEyes, imgFov0,
                                                     lsVp, ::dlvr::CalibScreenVoid(), loadQuad);
    for (int lq = 0; lq < loadQuads && submitted < static_cast<uint32_t>(kLayerCap - 1); ++lq) {
        layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&loadQuad[lq]);
    }
    // HOLSTER SPHERE (place-by-hand Task 4, ruling R11): the Holsters page's selected zone, BEFORE the panel quad so the panel
    // always draws on top (the radius slider is never covered). Safe before the panel's ImGui frame: the sphere draws into its
    // own ImDrawList with no NewFrame (overlay.cpp RenderCalibQuadInto ownList), so the panel's frame / draw data and the
    // mirror re-render are untouched. Budget: kLayerCap - 1, not the `< 5` of the menu / grapple gates (Task 1 G2: worst
    // case 7 of 12 with it) - the -1 leaves the slot the panel's UNCONDITIONAL write just below needs (the loading quads and
    // the calibration box stop at kLayerCap - 1 for the same reason). COST: no zone selected = one int test inside the call.
    if (submitted < static_cast<uint32_t>(kLayerCap - 1)) {
        if (OverlayRenderHolsterSphere(s->appSpace, &sphereQuad)) {
            layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&sphereQuad);
        }
    } else if (::dlvr::g_holsterShowZone >= 0) {
        static bool saidSphereCap = false;
        if (!saidSphereCap) {
            saidSphereCap = true;
            LOG("overlay: holster sphere NOT submitted - the layer list is full (%u of %d layers); logged once", submitted,
                kLayerCap);
        }
    }
    // HOLSTER RINGS (plan 2026-11-20-holster-pivot-posture-rings Task 5; trace 20261120_1300_holster_rings): right after the sphere,
    // before the panel / title (they stand down while the panel is open anyway), same budget rule (kLayerCap - 1 keeps the panel's
    // unconditional slot), none while the loading quads are up. COST off / no hand near a holster: one bool test / a few distances.
    if (loadQuads == 0 && ::dlvr::HolsterRings) {
        const int room = static_cast<int>(kLayerCap - 1) - static_cast<int>(submitted);
        if (room > 0) {
            const int nr = OverlayRenderHolsterRings(s->appSpace, ringQuad, room < 3 ? room : 3);
            for (int rq = 0; rq < nr; ++rq) { layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ringQuad[rq]); }
        } else {
            static bool saidRingCap = false;
            if (!saidRingCap) {
                saidRingCap = true;
                // [PROBE: HOLSTERRING] gate=holster_rings  site=HEADSET  log-only=no
                LOG("overlay: [HOLSTERRING] NOT submitted - the layer list is full (%u of %d layers); logged once", submitted, kLayerCap);
            }
        }
    }
    // MENU TITLE (Jay 2026-11-11: above the panel, top left, outside it): its own quad beside the panel's, same budget rule as the
    // sphere (kLayerCap - 1 keeps the panel's slot). No ImGui frame (own draw list), so before OverlayRender is safe. Worst case
    // with it: one more than the sphere's 7 of 12. COST menu closed: two bool tests inside the call.
    if (submitted < static_cast<uint32_t>(kLayerCap - 1)) {
        if (OverlayRenderTitle(s->viewSpace, s->appSpace, s->frameState.predictedDisplayTime, &titleQuad)) {
            layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&titleQuad);
        }
    } else if (::dlvr::OverlayVisible()) {
        static bool saidTitleCap = false;
        if (!saidTitleCap) {
            saidTitleCap = true;
            LOG("overlay: menu title NOT submitted - the layer list is full (%u of %d layers); logged once", submitted,
                kLayerCap);
        }
    }
    if (OverlayRender(s->viewSpace, s->appSpace, s->frameState.predictedDisplayTime,
                      &overlayQuad)) {
        layers[submitted++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlayQuad);
        if (mirrorRefreshed && !s->mirrors.empty() && s->mirrors[0]->tex) {
            OverlayRenderToMirror(s->mirrors[0]->tex);
        }
    }
    // The 3D grapple icon (trace 20261121_2225, Jay: the reticle "keeps disappearing", "definitely ... with the gun out"): the same cap
    // as the other extra quads (kLayerCap - 1 keeps the calibration panel's slot). It was a hard `< 5` - the game image + the HUD
    // panel (2) + the gun reticle + any holster ring (since 11-20) filled it and dropped the icon without a word.
    if (submitted < static_cast<uint32_t>(kLayerCap - 1)) {
        if (OverlayRenderGrapple(s->appSpace, &grappleQuad)) {
            layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&grappleQuad);
        }
    } else if (::dlvr::Grapple3dReticle) {
        static bool saidGrappleCap = false;
        if (!saidGrappleCap) {
            saidGrappleCap = true;
            // [PROBE: GRAPPLE3D] gate=grapple_reticle_3d  site=HEADSET  log-only=yes
            LOG("overlay: [GRAPPLE3D] icon NOT submitted - the layer list is full (%u of %d layers); logged once", submitted, kLayerCap);
        }
    }
    // WRIST HUD STAGE 2 (wristquad.h, marker dlvr\wrist_hud.txt): the five HUD pieces on quads in the grip spaces, AFTER the `< 5`
    // gates above (they cannot be starved) and before the calibration layers; kLayerCap - 1 keeps the calibration panel's slot, as
    // the sphere / title / loading quads do. Marker absent = one bool test inside the call.
    {
        const int room = static_cast<int>(kLayerCap) - 1 - static_cast<int>(submitted);
        const int nw = ::dlvr::wristquad::Render(s->appSpace, proj.viewCount == 2, menuEyes, room, wristQuad);
        for (int w = 0; w < nw && submitted < static_cast<uint32_t>(kLayerCap - 1); ++w) {
            layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&wristQuad[w]);
        }
    }
    // Calibration screen: the floor under the void (build B), then the panel on top of everything (build A). Closed = one
    // bool test + one volatile read.
    if (submitted < kLayerCap && ::dlvr::CalibScreenVoid() && ::dlvr::CalibScreenRenderFloor(&calibFloor)) {
        layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&calibFloor);
    }
    // The void's walls + ceiling, after the floor and before the panel; one slot is always left for the panel. Seen from
    // inside the box, no two of its faces cover the same pixel, so submit order (no depth) cannot misdraw them.
    if (::dlvr::CalibScreenVoid()) {
        const int room = kLayerCap - 1 - static_cast<int>(submitted);
        const int nb = ::dlvr::CalibScreenRenderVoidBox(calibBox, room < 5 ? room : 5);
        for (int b = 0; b < nb; ++b) {
            layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&calibBox[b]);
        }
    }
    if (submitted < kLayerCap && ::dlvr::CalibScreenActive() &&
        ::dlvr::CalibScreenRender(s->viewSpace, s->appSpace, s->frameState.predictedDisplayTime, &calibQuad)) {
        layers[submitted++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&calibQuad);
    }

    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = s->frameState.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = submitted;
    ei.layers = submitted ? layers : nullptr;

    // Submit rate, measured rather than assumed. Judder that appears when you
    // turn your head and vanishes when you hold still is the signature of a low
    // frame rate, not of a pose bug - reprojection has nothing to correct while
    // the head is still. Printing the number stops that being a matter of
    // opinion, and makes render_scale a decision instead of a guess.
    ++s->totalFrames;
    if (s->beganAt > 0.0) {
        const double ms = (NowSeconds() - s->beganAt) * 1000.0;
        if (ms > s->renderMaxMs) {
            s->renderMaxMs = ms;
        }
        if (s->frameState.predictedDisplayPeriod > 0 &&
            ms > s->frameState.predictedDisplayPeriod / 1e6) {
            ++s->overBudget;
        }
    }
    // How late is this frame against the moment we told the compositor it was
    // for? Positive means xrEndFrame is happening after the display time in the
    // layer, so the compositor has to reproject further than it was asked to.
    if (s->frameState.predictedDisplayTime) {
        const double lateMs =
            (NowOnRuntimeClock() -
             XrTimeToSeconds(s->frameState.predictedDisplayTime)) * 1000.0;
        s->lateSumMs += lateMs;
        if (lateMs > s->lateMaxMs) {
            s->lateMaxMs = lateMs;
        }
        if (lateMs > 0.0) {
            ++s->lateFrames;
        }
    }
    if (s->poseQueriesThisFrame == 0) {
        // Rendered and submitted without ever asking us for a pose after the
        // frame began: whatever it drew, it drew from the previous sample.
        ++s->staleFrames;
        LOG_FIRST(6, "frame phase: submit with no tracking query since the frame "
                     "began - this frame was drawn from the previous pose (%u of %u)",
                  s->staleFrames, s->totalFrames);
    }

    static unsigned frames = 0;
    static double lastMark = 0.0;
    if ((frames++ % 600) == 0) {
        ::dlvr::WatchExecSayOrphans();   // U-26: a claim is never left unlogged (prints only when the count moved)
        const double now = NowSeconds();
        const double fps = (lastMark > 0.0) ? 600.0 / (now - lastMark) : 0.0;
        lastMark = now;
        LOG("frame %u: %.1f fps  state=%d running=%d shouldRender=%d "
            "layersSubmitted=%u posesValid=%d",
            frames, fps, static_cast<int>(s->state), s->running ? 1 : 0,
            s->frameState.shouldRender ? 1 : 0, submitted, s->posesValid ? 1 : 0);
        LOG("frame phase: %u/%u stale pose, %u long gaps, %d queries this frame, "
            "period %.2f ms, worst render %.2f ms, %u over budget",
            s->staleFrames, s->totalFrames, s->longGaps, s->poseQueriesThisFrame,
            s->frameState.predictedDisplayPeriod / 1e6, s->renderMaxMs,
            s->overBudget);
        LOG("frame phase: %llu camera levelings used a head from the previous "
            "frame, of %llu",
            static_cast<unsigned long long>(CamHookStaleHeadCount()),
            static_cast<unsigned long long>(CamHookLevelCount()));
        LOG("frame budget: wait avg %.2f ms max %.2f ms | game work max %.2f ms | "
            "late avg %+.2f ms max %+.2f ms, %u of %u frames late",
            s->waitCount ? s->waitSumMs / s->waitCount : 0.0, s->waitMaxMs,
            s->renderMaxMs, s->waitCount ? s->lateSumMs / s->waitCount : 0.0,
            s->lateMaxMs, s->lateFrames, s->waitCount);
        LOG("engine stalled on our lock: avg %.2f ms max %.2f ms over %u pose "
            "queries", s->blockedCount ? s->blockedSumMs / s->blockedCount : 0.0,
            s->blockedMaxMs, s->blockedCount);
        // Printed before anything is reset. The first version of this line put
        // the reset above it and duly reported xrWaitFrame as 0.00 ms every
        // time - a measurement of a variable that had just been zeroed.
        LOG("worst frame, split: engine %.2f ms | our submit path %.2f ms | "
            "xrEndFrame %.2f ms | xrWaitFrame %.2f ms",
            s->gameMaxMs, s->ourMaxMs, s->endMaxMs, s->waitMaxMs);
        // >>> THE AVERAGE SPLIT. THIS IS THE LINE THAT ATTRIBUTES THE FRAME. <<<
        // The four terms are consecutive and disjoint by construction:
        //   engine       xrBeginFrame -> the game calls ovr_SubmitFrame
        //   our submit   ovr_SubmitFrame entry -> xrEndFrame call
        //   xrEndFrame   inside xrEndFrame
        //   xrWaitFrame  inside xrWaitFrame
        // so their SUM should land near the display period. **If it does not,
        // the residual is time in none of them and the split is INCOMPLETE -
        // say so rather than reading the four numbers as a breakdown** (D-28's
        // lesson in a different instrument: a table that does not sum to its
        // total is not a breakdown).
        if (s->splitCount > 0) {
            const double g = s->gameSumMs / s->splitCount;
            const double o = s->ourSumMs / s->splitCount;
            const double e = s->endSumMs / s->splitCount;
            const double w = s->waitCount ? s->waitSumMs / s->waitCount : 0.0;
            const double periodMs = s->frameState.predictedDisplayPeriod / 1e6;
            // >>> THE RESIDUAL IS AGAINST THE **ACHIEVED** FRAME, NOT THE DISPLAY PERIOD. <<<
            // Corrected 2026-09-21, same day this line was added, because the first
            // version differenced against predictedDisplayPeriod and printed a
            // misleading `residual +3.4 ms` on a run whose real residual was
            // **+0.03 ms**: the headset was at 120 Hz (8.33 ms) while the game
            // delivered 11.76 ms, so the two were never meant to match. INV-8 is
            // explicit that the quotable frame time is `600 / delta-t`, which is
            // the `fps` computed at the head of this block - so use THAT as the
            // denominator and report the display period alongside as context.
            // A split that cannot be checked against the right total is D-28's
            // failure in a different instrument.
            const double achievedMs = (fps > 0.0) ? 1000.0 / fps : 0.0;
            LOG("AVERAGE frame split over %u frames: engine %.2f ms | our submit path %.2f ms | "
                "xrEndFrame %.2f ms | xrWaitFrame %.2f ms  ==  %.2f ms total vs %.2f ms ACHIEVED "
                "frame (%.1f fps) -> residual %+.2f ms. [display period %.2f ms = %.0f Hz, which "
                "is the TARGET, not the denominator.] **If the residual is large the split is "
                "INCOMPLETE and the four terms are NOT a breakdown.**",
                s->splitCount, g, o, e, w, g + o + e + w, achievedMs, fps,
                achievedMs > 0.0 ? (g + o + e + w) - achievedMs : 0.0,
                periodMs, periodMs > 0.0 ? 1000.0 / periodMs : 0.0);
        } else {
            LOG("AVERAGE frame split: *** NO FRAMES COUNTED - NO VERDICT *** splitCount is 0, so "
                "xrBeginFrame never preceded a submit. This is an instrument state, not a frame "
                "that costs nothing.");
        }
        LOG("xrEndFrame: %u of %u frames blocked longer than a display period",
            s->endStalls, s->waitCount);
        LOG("pose steps: ours max %.3f deg, game max %.3f deg, %u frames where "
            "the game moved further than we asked",
            s->ourStepMaxDeg, s->gameStepMaxDeg, s->divergedFrames);
        LOG("render pose age: matches this frame %u, matches previous frame %u, "
            "worst gap between declared and current %.3f deg",
            s->matchesCurrent, s->matchesPrevious, s->declaredErrMaxDeg);

        s->renderMaxMs = 0.0;
        s->waitSumMs = s->waitMaxMs = 0.0;
        s->waitCount = 0;
        s->gameMaxMs = s->ourMaxMs = s->endMaxMs = 0.0;
        // Reset the sums with the maxima, and only AFTER the print above - the
        // comment at the head of this block records what happened the last time
        // a reset sat above its own report line.
        s->gameSumMs = s->ourSumMs = s->endSumMs = 0.0;
        s->splitCount = 0;
        s->endStalls = 0;
        s->ourStepMaxDeg = s->gameStepMaxDeg = 0.0;
        s->divergedFrames = 0;
        s->matchesCurrent = s->matchesPrevious = 0;
        s->declaredErrMaxDeg = 0.0;
        s->lateSumMs = s->lateMaxMs = 0.0;
        s->lateFrames = 0;
        s->blockedSumMs = s->blockedMaxMs = 0.0;
        s->blockedCount = 0;
    }

    // Periodic pose audit. The open question is what the game does with the
    // HmdToEyeOffset we hand it, and rather than guess we reconstruct its own
    // RenderPose both ways and report which one it matches:
    //
    //   "unrotated"  the game does headPos + offset, treating our offset as
    //                already being in tracking space. Then eye_offset_space=0
    //                is right, but only if it re-reads the offset every frame -
    //                check the ovr_GetRenderDesc call count.
    //   "rotated"    the game does headPos + R(head) * offset, the proper
    //                LibOVR contract. Then eye_offset_space=1 is right and the
    //                current tracking-space offset is being rotated twice,
    //                which grows with yaw exactly as reported.
    //
    // Also prints the orientation the game claims it rendered with next to the
    // one we located, since we DECLARE the game's - if those diverge, the
    // compositor is placing the image at an angle it was not drawn at, which is
    // what head-rotation jitter looks like.
    // Only audit when the head is actually turned. The first attempt sampled
    // every 300 frames regardless, and every sample landed within three degrees
    // of straight ahead - where the rotated and unrotated reconstructions agree
    // to within tracking noise, so the verdict was measuring nothing. The whole
    // question is what happens at yaw, so only look at yaw.
    //
    // |w| = cos(angle/2), so |w| < 0.985 is a head turned more than 20 degrees.
    static int audits = 0;
    const bool turned = fabsf(s->headPose.orientation.w) < 0.985f;
    if (submitted && turned && audits < 12) {
        ++audits;
        const XrVector3f d0{ renderPose[0].Position.x - s->headPose.position.x,
                             renderPose[0].Position.y - s->headPose.position.y,
                             renderPose[0].Position.z - s->headPose.position.z };
        const XrVector3f local0 = UnrotateBy(s->headPose.orientation, d0);
        const XrVector3f ours{ s->views[0].pose.position.x - s->headPose.position.x,
                               s->views[0].pose.position.y - s->headPose.position.y,
                               s->views[0].pose.position.z - s->headPose.position.z };
        const XrVector3f oursLocal = UnrotateBy(s->headPose.orientation, ours);
        // If the game left our tracking-space offset unrotated, d0 == ours. If
        // it rotated it, the head-local version is the one that matches.
        const float errUnrot = fabsf(d0.x - ours.x) + fabsf(d0.y - ours.y) +
                               fabsf(d0.z - ours.z);
        const float errRot = fabsf(local0.x - ours.x) + fabsf(local0.y - ours.y) +
                             fabsf(local0.z - ours.z);
        LOG("eye offset: game head->eye [0] tracking=(%.4f,%.4f,%.4f) "
            "head-local=(%.4f,%.4f,%.4f); ours tracking=(%.4f,%.4f,%.4f) "
            "head-local=(%.4f,%.4f,%.4f)",
            d0.x, d0.y, d0.z, local0.x, local0.y, local0.z,
            ours.x, ours.y, ours.z, oursLocal.x, oursLocal.y, oursLocal.z);
        LOG("eye offset: head turned %.1f deg, unrotated err=%.4f rotated "
            "err=%.4f -> game consumes HmdToEyeOffset %s (want eye_offset_space=%d)",
            2.0f * acosf(fabsf(s->headPose.orientation.w)) * 180.0f / 3.14159265f,
            errUnrot, errRot, (errUnrot <= errRot) ? "UNROTATED" : "ROTATED",
            (errUnrot <= errRot) ? 0 : 1);
        LOG("pose audit: game quat [0]=(%.4f,%.4f,%.4f,%.4f) "
            "ours=(%.4f,%.4f,%.4f,%.4f)",
            renderPose[0].Orientation.x, renderPose[0].Orientation.y,
            renderPose[0].Orientation.z, renderPose[0].Orientation.w,
            s->views[0].pose.orientation.x, s->views[0].pose.orientation.y,
            s->views[0].pose.orientation.z, s->views[0].pose.orientation.w);
    }

    // Compare the two step sizes for this frame. renderPose is what the game
    // says it drew with; s->headPose is what we handed it.
    if (renderPose) {
        auto stepDeg = [](const XrQuaternionf& a, const XrQuaternionf& b) {
            // Angle of the relative rotation, via the quaternion dot product.
            double d = static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y +
                       static_cast<double>(a.z) * b.z + static_cast<double>(a.w) * b.w;
            d = d < 0.0 ? -d : d;
            if (d > 1.0) {
                d = 1.0;
            }
            return 2.0 * acos(d) * 180.0 / 3.14159265358979;
        };
        const XrQuaternionf game{ renderPose[0].Orientation.x, renderPose[0].Orientation.y,
                                  renderPose[0].Orientation.z, renderPose[0].Orientation.w };
        if (s->haveePrev) {
            // Against this frame's pose and against last frame's. The game's
            // pose carries an eye offset, not a different rotation, so the
            // orientations should match one of them almost exactly.
            const double vsNow = stepDeg(game, s->headPose.orientation);
            const double vsPrev = stepDeg(game, s->prevOurs);
            if (vsNow <= vsPrev) {
                ++s->matchesCurrent;
            } else {
                ++s->matchesPrevious;
                // If the game drew with the current pose but reported the
                // previous one, this is the angle the compositor would warp by
                // in error.
                if (vsNow > s->declaredErrMaxDeg) {
                    s->declaredErrMaxDeg = vsNow;
                }
            }

            const double ourStep = stepDeg(s->prevOurs, s->headPose.orientation);
            const double gameStep = stepDeg(s->prevGame, game);
            if (ourStep > s->ourStepMaxDeg) {
                s->ourStepMaxDeg = ourStep;
            }
            if (gameStep > s->gameStepMaxDeg) {
                s->gameStepMaxDeg = gameStep;
            }
            // A step the game took that we did not ask for. The threshold is
            // deliberately generous: this is looking for jumps, not for the
            // fraction of a degree that rounding and prediction always cost.
            if (gameStep > ourStep * 2.0 + 0.5) {
                ++s->divergedFrames;
                LOG_FIRST(20, "pose diverged: we moved %.3f deg this frame, the "
                              "game's render pose moved %.3f deg",
                          ourStep, gameStep);
            }
        }
        s->prevOurs = s->headPose.orientation;
        s->prevGame = game;
        s->haveePrev = true;
    }

    if (s->submitEnteredAt > 0.0) {
        const double ms = (NowSeconds() - s->submitEnteredAt) * 1000.0;
        if (ms > s->ourMaxMs) {
            s->ourMaxMs = ms;
        }
        s->ourSumMs += ms;
    }
    const double endStart = NowSeconds();
    XrResult r = xrEndFrame_(s->session, &ei);
    double hcEndMs = 0.0;   // [HICCUP]
    {
        const double ms = (NowSeconds() - endStart) * 1000.0;
        hcEndMs = ms;
        if (ms > s->endMaxMs) {
            s->endMaxMs = ms;
        }
        s->endSumMs += ms;
        const double periodMs = s->frameState.predictedDisplayPeriod / 1e6;
        if (periodMs > 0.0 && ms > periodMs) {
            ++s->endStalls;
            // Logged individually with a timestamp: one 101 ms outlier and a
            // steady drip of 17 ms ones are different problems, and a
            // per-600-frame maximum cannot tell them apart.
            LOG_FIRST(20, "xrEndFrame blocked %.2f ms (period %.2f ms) - the "
                          "runtime is holding the submit, not the game",
                      ms, periodMs);
        }
    }
    s->frameActive = false;
    if (XR_FAILED(r)) {
        XrOk(r, "xrEndFrame");  // keep the game running; a dropped frame is not fatal
    }

    // Wait for and begin the next frame here, while we are still on the render
    // thread and still hold the lock. This is the frame pacing point, and it is
    // the reason no worker ever has to call xrWaitFrame. See BootstrapFrame.
    BeginFrameIfNeeded(s);
    // [HICCUP]: after the next xrWaitFrame, so the predicted display time step is this frame's. View 0's declared pose
    // only when a projection layer went out (submitted > 0); otherwise the jump test is skipped for this frame.
    HiccupFrame(s, hcEndMs, submitted > 0, projViews[0].pose.orientation);
    return ovrSuccess;
}

// ---------------------------------------------------------------- settings ---
//
// Defined out here so the overlay can reach them; the values themselves stay
// where they are used.
namespace dlvr {

// meleethrow.h: melee_r2_suppress's live value for the throw's refusal (g_meleeR2Suppress is file-local here).
bool MeleeR2SuppressOn() { return g_meleeR2Suppress; }

const Setting* AllSettings(int& count)
{
    count = kSettingCount;
    return kSettings;
}

// The one path builder, published. LayerFilePath itself lives in this file's
// anonymous namespace and is used throughout it; this is that same function
// under a name other translation units can reach. Deliberately a FORWARD, not a
// copy - the folder-creation it performs is load-bearing on a fresh install
// (nothing else in the mod creates dlvr\) and is exactly the kind of detail a
// second, hand-rolled implementation would omit.
bool DlvrFilePath(const char* leaf, char* out, size_t cch)
{
    return LayerFilePath(leaf, out, cch);
}

// Writes the file when a value has actually moved.
//
// Called after every widget in the overlay, so the file always matches what is
// on screen and there is nothing to remember to press. It compares first: a
// slider being dragged fires every frame, and rewriting the config sixty times
// a second to store the same number would be a poor way to spend a frame.
void SaveSettingsIfChanged()
{
    SyncFollowers();   // a master edited in the tab just drawn reaches its followers before the compare below
    // Jay 2026-11-14 (HARD; trace docs/traces/20261114_1200_wrist_holds_hud_panel.md): the Wrist HUD ticked, or running this launch,
    // holds 'HUD on a fixed panel' on - a Wrist HUD tick in the tab just drawn turns it on here, before the compare saves it. Fires
    // once per event (the row is greyed while needed). COST: two bool reads per call.
    if (::dlvr::wristquad::HudPanelNeeded() && !UiHudPanel) {
        UiHudPanel = true;
        LOG("config: ui_hud_panel TURNED ON - the Wrist HUD needs it (ui_wrist_hud=%d, running this launch %d)", UiWristHud ? 1 : 0,
            ::dlvr::wristquad::OnThisLaunch() ? 1 : 0);
    }
    static float last[kSettingCount] = {};
    static bool primed = false;
    bool changed = false;
    for (int i = 0; i < kSettingCount; ++i) {
        const float v = SettingAsFloat(kSettings[i]);
        if (!primed || v != last[i]) {
            changed = true;
            // S1: the only menu route into the shadow stagger - reached only when a value has just changed, never per frame.
            if (primed && kSettings[i].value == &dlvr::ShadowCascadeStagger && dlvr::ShadowCascadeStagger >= 2) {
                dlvr::ShadowStaggerStart();
            }
        }
        last[i] = v;
    }
    if (!primed) {
        primed = true;
        return;
    }
    if (!changed) {
        return;
    }
    char why[256] = {};
    if (!WriteSettingsFile(why, sizeof(why))) {
        LOG("config: save failed (%s)", why);
    }
}

bool GetHeadPose(XrPosef& outPose)
{
    Guard lk;
    if (!g_session) {
        return false;
    }
    outPose = g_session->headPose;
    return true;
}
bool GetAppBase(float pos[3], float* yawDeg)
{
    Guard lk;
    if (!g_session) {
        return false;
    }
    pos[0] = g_session->appBasePos.x;
    pos[1] = g_session->appBasePos.y;
    pos[2] = g_session->appBasePos.z;
    if (yawDeg) { *yawDeg = g_session->appBaseYawDeg; }
    return true;
}
bool GetEyeAboveFloor(float* metres)
{
    Guard lk;
    if (!g_session || !g_session->floorOk) {
        return false;
    }
    if (metres) { *metres = g_session->headPose.position.y + g_session->appFloorY; }
    return true;
}
bool HeadPoseLocatedOnce() { return InterlockedCompareExchange(&g_headLocatedOnce, 0, 0) != 0; }

int&   OverlayKey()      { return g_overlayKey; }
int&   OverlayPad()      { return g_overlayPad; }
float& OverlayDistance() { return g_overlayDistance; }
float& OverlayWidth()    { return g_overlayWidth; }
bool&  OverlayEnabled()  { return g_overlayEnabled; }
int&   OverlayTextSize() { return g_overlayTextSize; }
bool&  OverlayAdvanced() { return g_overlayAdvanced; }
bool&  HorizonLock()     { return g_horizonLock; }
int    CamUpAxis()       { return g_camUpAxis; }   // cam_up_axis: the file value (LOADBEARING - camhook's camera code reads it)

// Was PullCamConvention until 2026-11-14 (removal batch 2): its camscan half went with camscan; this half stays.
void PullCamHookOp()
{
    // CamHookOp is an enum class and the settings table stores ints, so the two
    // are mirrored rather than shared. Whichever moved last wins.
    const int live = static_cast<int>(CamHookOp);
    if (live != g_camHookOp) {
        if (g_camHookOp >= 0 && g_camHookOp <= 2 && g_camHookOpDirty) {
            CamHookOp = static_cast<HookOp>(g_camHookOp);
            g_camHookOpDirty = false;
        } else {
            g_camHookOp = live;
        }
    }
}
const XrQuaternionf& LastReportedHead() { return g_lastReportedHead; }
const XrVector3f& LastReportedHeadPos() { return g_lastReportedHeadPos; }
const XrQuaternionf& LastStrippedHeadYaw() { return g_lastStrippedHeadYaw; }
bool HeadYawDecoupleActive() { return g_headYawDecoupleState == 1; }
XrVector3f NeckPivotReportedPos(const XrPosef& rawHead)
{
    XrVector3f p;
    NeckPivotCompute(rawHead, p, nullptr);
    return p;
}
const char* XrSystemNameNow() { return g_xrSystemName; }   // headpose.h (plan 2026-11-20-holster-pivot-posture-rings Task 4)
XrVector3f NeckOffsetM()
{
    return XrVector3f{ HeadNeckPivotXmm * 0.001f, HeadNeckPivotYmm * 0.001f, HeadNeckPivotZmm * 0.001f };
}
bool HeadYawDecoupleFlipLatched() { return g_headYawDecoupleFlipLatched; }
bool HeadSampledThisFrame() { return g_headSampled != 0; }

bool ApplySettingLine(const char* line)
{
    for (int i = 0; i < kSettingCount; ++i) {
        const Setting& st = kSettings[i];
        const size_t n = strlen(st.key);
        if (_strnicmp(line, st.key, n) != 0 || line[n] != '=') {
            continue;
        }
        const char* v = line + n + 1;
        while (*v == ' ' || *v == '\t') {
            ++v;
        }
        g_seen[i] = true;

        // FOLLOWS A MASTER (settings.h): the file's value is never applied - SyncFollowers sets it from the master after
        // the whole file is read (the master's line may come later). Kept here only to say so if it differed.
        if (st.follows) {
            g_followerFile[i] = (atoi(v) != 0) ? 1 : 0;
            return true;
        }
        // LOCKED (settings.h): the compiled value stands. A differing file value is SAID, and the file is rewritten to match.
        if (st.locked) {
            const float want = SettingAsFloat(st);
            const float file = (st.type == SetType::Float) ? static_cast<float>(atof(v)) : static_cast<float>(atoi(v));
            if (file != want) {
                g_lockedOverrode = true;
                LOG("config: %s=%s IGNORED - this setting is LOCKED to %g (loadbearing; hidden from the menu)", st.key, v,
                    static_cast<double>(want));
            }
            return true;
        }

        float parsed = (st.type == SetType::Float) ? static_cast<float>(atof(v))
                                                   : static_cast<float>(atoi(v));
        // Out of range means the file holds something this build does not
        // understand - an option that was removed, or a hand-edited typo. Take
        // the default rather than the nearest legal value.
        if (!(parsed >= st.lo && parsed <= st.hi)) {
            if (g_defaultsCaptured) {
                LOG("config: %s=%s is not a value this build knows - using the "
                    "default", st.key, v);
                parsed = g_defaults[i];
            } else {
                parsed = ClampTo(st, parsed);
            }
        }
        SettingFromFloat(st, parsed);
        return true;
    }
    return false;
}

// Rewrites values in place, leaving every comment and blank line alone.
//
// The file is not a serialisation format, it is documentation with values in
// it: each block records why that setting is what it is, and several of those
// notes are the only surviving record of a measurement that cost a launch to
// get. A writer that regenerated the file would delete the reasoning behind its
// own defaults, so it edits instead.
bool WriteSettingsFile(char* whyNot, size_t whyNotBytes)
{
    char path[MAX_PATH * 2];
    if (!LayerFilePath("ovr2xr.txt", path, sizeof(path))) {
        strncpy_s(whyNot, whyNotBytes, "could not resolve the dlvr folder", _TRUNCATE);
        return false;
    }

    std::vector<std::string> lines;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
                line[--n] = '\0';
            }
            lines.push_back(line);
        }
        fclose(f);
    }

    std::vector<bool> written(static_cast<size_t>(kSettingCount), false);
    for (auto& l : lines) {
        if (l.empty() || l[0] == '#') {
            continue;
        }
        for (int i = 0; i < kSettingCount; ++i) {
            const Setting& st = kSettings[i];
            const size_t n = strlen(st.key);
            if (l.size() <= n || _strnicmp(l.c_str(), st.key, n) != 0 || l[n] != '=') {
                continue;
            }
            char buf[256];
            FormatSetting(st, buf, sizeof(buf));
            l = buf;
            written[static_cast<size_t>(i)] = true;
            break;
        }
    }

    // Anything the file never had - a setting added since it was last written.
    for (int i = 0; i < kSettingCount; ++i) {
        if (written[static_cast<size_t>(i)]) {
            continue;
        }
        char buf[256];
        FormatSetting(kSettings[i], buf, sizeof(buf));
        if (!lines.empty() && !lines.back().empty()) {
            lines.push_back("");
        }
        lines.push_back("# added by the overlay");
        lines.push_back(buf);
    }

    // Write a temporary file and swap it in, so a crash part-way through cannot
    // leave a truncated config - which would be the one failure the user has no
    // way to repair from inside the overlay.
    char tmp[MAX_PATH * 2];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);
    if (fopen_s(&f, tmp, "w") != 0 || !f) {
        _snprintf_s(whyNot, whyNotBytes, _TRUNCATE, "cannot write %s", tmp);
        return false;
    }
    for (const auto& l : lines) {
        fprintf(f, "%s\n", l.c_str());
    }
    fclose(f);

    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        _snprintf_s(whyNot, whyNotBytes, _TRUNCATE, "cannot replace %s (error %lu)", path,
                    GetLastError());
        DeleteFileA(tmp);
        return false;
    }
    LOG("settings written to %s", path);
    return true;
}

}  // namespace dlvr

// Lets the bootstrap suppress gamepad input to the game while the menu is open.
//
// The bootstrap is the game's xinput1_3.dll, so it is already in the path every
// pad read takes. Without this, navigating the menu also drives the player -
// and L3+R3 is crouch and melee, so opening the overlay would swing a weapon.
// AFW (2026-10-03): for the bootstrap's DLSS upscale path - which eye THIS frame renders under afw_alt_eye (0/1), or -1
// when both eyes render. See CamHookAfwRenderedEye.
extern "C" __declspec(dllexport) int dlvr_AfwRenderedEye()
{
    return dlvr::CamHookAfwRenderedEye();
}

// afw_mv_object_scale (INV-453): the factor the bootstrap's MV pass puts on the engine object vector while AFW is on.
extern "C" __declspec(dllexport) float dlvr_AfwMvObjectScale()
{
    return dlvr::AfwMvObjectScale;
}

// afw_rebuild's gate: the bootstrap hands over each GBuffer-pass view-projection (64 bytes) on the render thread.
extern "C" __declspec(dllexport) void dlvr_AfwPushVp(const float* vp16)
{
    dlvr::CamHookAfwPushVp(vp16);
}

// The bootstrap's replay-side events: k = 'D' (the eye composite bound swapchain texture `tex`, v = viewport width) or 'T'
// (a tonemap draw). Only submit_drawn_image reads them now: the image this frame's composite drew is what the submit
// releases (SdReleaseDrawn). COST: one pointer store per composite bind. Always returns -1 (no image index) - the
// [SWAPORD] ring and its lookup went with diag_swap_order on 2026-10-07, and -1 is what it returned whenever that was off.
extern "C" __declspec(dllexport) int dlvr_SwapNoteExt(int k, void* tex, long v, int eye)
{
    (void)v; (void)eye;
    if (k == 'D' && SubmitDrawnImage && tex) { g_sdDrawnTex = tex; }
    return -1;
}

// F2 LENS MASK: asked by the bootstrap at each scene-depth clear / stencil clear / tonemap draw for the GAME eye it is drawing.
// Returns 0 = no mask (lens_mask off, eye out of range, or no submit has described that eye yet); else bit 0 = mask, bit 1 = tint.
// out[12]: 0-3 the image's tangents L R U D, 4-7 the visible region's tangents L R U D (the runtime's angles + the margin, capped at
// 85 deg), 8 cos(pitch), 9 sin(pitch), 10 the margin in degrees, 11 the pitch in degrees.
// INV-735 (trace 20261113_2115): the frustum the GAME rendered eye `eye` with, for every UI-plane crop (uipanel_logic.h UiCrop) -
// never a constant, any headset. Not gated by any setting. 0 = not known yet (no submit has described that eye); 1 = out[4] = left,
// right, up, down tangents (positive). Asked by the bootstrap's wrist HUD router once per HUD pass (resolved once there).
extern "C" __declspec(dllexport) int dlvr_GameEyeTan(int eye, float* out)
{
    if (eye < 0 || eye > 1 || !out || !g_lmEye[eye].gameOk) { return 0; }
    for (int i = 0; i < 4; ++i) { out[i] = g_lmEye[eye].game[i]; }
    return 1;
}
namespace dlvr {
namespace uipanel {
bool GameEyeTan(int eye, EyeTan* out)
{
    float t[4];
    if (!out || !dlvr_GameEyeTan(eye, t)) { return false; }
    *out = EyeTan{ t[0], t[1], t[2], t[3] };
    return true;
}
}  // namespace uipanel
}  // namespace dlvr

extern "C" __declspec(dllexport) int dlvr_LensMask(int eye, float* out)
{
    if (!dlvr::LensMask || eye < 0 || eye > 1 || !out || !g_lmEye[eye].ok) { return 0; }
    const LensMaskEye& lm = g_lmEye[eye];
    const float m = (dlvr::LensMaskMarginDeg < 0.0f ? 0.0f : dlvr::LensMaskMarginDeg) * (3.14159265358979f / 180.0f);
    const float cap = 85.0f * (3.14159265358979f / 180.0f);
    for (int i = 0; i < 4; ++i) {
        out[i] = lm.img[i];
        const float a = lm.rt[i] + m;
        out[4 + i] = tanf(a > cap ? cap : a);
    }
    const float p = lm.pitchDeg * (3.14159265358979f / 180.0f);
    out[8] = cosf(p); out[9] = sinf(p);
    out[10] = dlvr::LensMaskMarginDeg; out[11] = lm.pitchDeg;
    return 1 | (dlvr::LensMaskTint ? 2 : 0);
}

// DLSS / FSR LENS CROP (docs/superpowers/specs/2026-11-11-dlss-lens-crop-design.md §4.1): the same per-eye geometry as dlvr_LensMask,
// gated on dlss_lens_crop instead of lens_mask (the lens mask is NOT needed), with the crop's own margin. Asked by the bootstrap at
// each VR DLSS / FSR evaluation (the function pointer is resolved once there). Returns 0 = no crop (dlss_lens_crop off, eye out of
// range, or no submit has described that eye yet); else bit 0 = crop, bit 1 = tint. out[12]: dlvr_LensMask's layout.
// g_lmEye is written on the submit thread and read here on the render thread without a lock: each float is one aligned store, and the
// geometry is constant while the runtime FOV and the declared pitch are (the crop re-snaps and resets history if it ever changes).
extern "C" __declspec(dllexport) int dlvr_LensGeom(int eye, float* out)
{
    if (!dlvr::DlssLensCrop || eye < 0 || eye > 1 || !out || !g_lmEye[eye].ok) { return 0; }
    const LensMaskEye& lm = g_lmEye[eye];
    const float m = (dlvr::DlssLensCropMarginDeg < 0.0f ? 0.0f : dlvr::DlssLensCropMarginDeg) * (3.14159265358979f / 180.0f);
    const float cap = 85.0f * (3.14159265358979f / 180.0f);
    for (int i = 0; i < 4; ++i) {
        out[i] = lm.img[i];
        const float a = lm.rt[i] + m;
        out[4 + i] = tanf(a > cap ? cap : a);
    }
    const float p = lm.pitchDeg * (3.14159265358979f / 180.0f);
    out[8] = cosf(p); out[9] = sinf(p);
    out[10] = dlvr::DlssLensCropMarginDeg; out[11] = lm.pitchDeg;
    return 1 | (dlvr::DlssLensCropTint ? 2 : 0);
}

// FIXED FOVEATED DLSS / FSR (docs/superpowers/specs/2026-11-11-foveated-dlss-design.md §4.2, plan Task 2): the same per-eye geometry
// as dlvr_LensGeom with margin 0 (the fovea is placed straight ahead, the margin is a crop concept), gated on fov_dlss - independent
// of dlss_lens_crop. Asked by the bootstrap at each VR DLSS / FSR evaluation (the pointer resolved once there; this body is a copy of
// a few floats - nothing resolved, nothing logged per call). Returns 0 = no foveation (fov_dlss off, eye out of range, no submit has
// described that eye yet), else 1. g12: dlvr_LensMask's layout. p12: 0 width, 1 height, 2 heightOffset, 3 roundness, 4 transition,
// 5 supersample, 6 peripheryScale, 7 (float) periphery preset id, 8 periphery (0 / 1), 9 border (0 / 1), 10 xOffset, 11 0.
// A preset id other than 5 / 11 / 12 / 13 is sent as 5 (E) and logged once per distinct value. Same threading note as dlvr_LensGeom.
extern "C" __declspec(dllexport) int dlvr_FovGeom(int eye, float* g12, float* p12)
{
    if (!dlvr::FovDlss || eye < 0 || eye > 1 || !g12 || !p12 || !g_lmEye[eye].ok) { return 0; }
    const LensMaskEye& lm = g_lmEye[eye];
    const float cap = 85.0f * (3.14159265358979f / 180.0f);
    for (int i = 0; i < 4; ++i) {
        g12[i] = lm.img[i];
        const float a = lm.rt[i];
        g12[4 + i] = tanf(a > cap ? cap : a);
    }
    const float p = lm.pitchDeg * (3.14159265358979f / 180.0f);
    g12[8] = cosf(p); g12[9] = sinf(p);
    g12[10] = 0.0f; g12[11] = lm.pitchDeg;
    int preset = dlvr::FovPeripheryPreset;
    if (preset != 5 && preset != 11 && preset != 12 && preset != 13) {
        static int s_lastBad = 0;
        if (preset != s_lastBad) {
            s_lastBad = preset;
            LOG("[FOVGEOM] fov_periphery_preset %d is not 5 (E) / 11 (K) / 12 (L) / 13 (M) - sent as 5 (E)", preset);
        }
        preset = 5;
    }
    p12[0] = dlvr::FovWidth; p12[1] = dlvr::FovHeight; p12[2] = dlvr::FovHeightOffset; p12[3] = dlvr::FovRoundness;
    p12[4] = dlvr::FovTransition; p12[5] = dlvr::FovSupersample; p12[6] = dlvr::FovPeripheryScale;
    p12[7] = static_cast<float>(preset); p12[8] = dlvr::FovPeriphery ? 1.0f : 0.0f; p12[9] = dlvr::FovBorder ? 1.0f : 0.0f;
    p12[10] = dlvr::FovXOffset; p12[11] = 0.0f;
    return 1;
}

// F2 LENS MASK: the GAME eye rendering now, from the submit record (g_lmLastSkip), or -1 = unknown (mask off, AFW not alternating,
// no record). Asked by the bootstrap at every mask draw; the answer is kept for the [LENSMASKID] check at the next submit.
extern "C" __declspec(dllexport) int dlvr_LensMaskEyeNow()
{
    if (!dlvr::LensMask) { return -1; }
    const int a = g_lmLastSkip, p = g_lmPrevSkip;
    const int eye = ((a == 0 || a == 1) && p == 1 - a) ? a : -1;
    InterlockedExchange(&g_lmAnswer, eye);
    return eye;
}

// Asked by the bootstrap at each VR tonemap draw - bit 0 = copy the depth for the eye rendered (afw_rebuild).
extern "C" __declspec(dllexport) int dlvr_AfwDepthWanted()
{
    // bit 0 = copy the depth (the rebuild wants it), bit 1 = never set since 2026-10-07 (it ran diag_afw_depth's 5 s proofs),
    // bit 2 = also copy the motion buffer ([AFWMOT], diag_afw_fg_motion - read only by the rebuild's rule counter),
    // bit 3 = draw the held-pose map at the mask's body draws ([AFWHM], diag_afw_heldmap),
    // bit 4 = also the held weapon in that map (afw_rb_fg_move_weapon, INV-464 - only with bit 3),
    // bit 5 = the mask is drawn the ENGINE's way (its PS, its depth write) AND alpha-tested draws are left out of the
    //         held-pose map (afw_rb_fg_mask_engine, 2026-10-08, INV-486)
    // bit 6 = [AFWVEH], the vehicle rigid-draw probe (diag_afw_heldmap itself - bit 3 cannot tell it from afw_rb_fg_move;
    //         2026-10-29, the buggy's world doubling, log-only)
    if (!dlvr::AfwAltEye) { return 0; }
    const bool hm = (dlvr::DiagAfwHeldMap || dlvr::AfwRbFgMove) && dlvr::AfwRebuild;
    return (dlvr::AfwRebuild ? 1 : 0) |
           ((dlvr::DiagAfwFgMotion && dlvr::AfwRebuild) ? 4 : 0) | (hm ? 8 : 0) |
           ((hm && dlvr::AfwRbFgMove && dlvr::AfwRbFgMoveWeapon) ? 16 : 0) |
           ((dlvr::AfwRbFgMask && dlvr::AfwRbFgMaskEngine) ? 32 : 0) |
           ((hm && dlvr::DiagAfwHeldMap) ? 64 : 0);
}

// [AFWFG] (diag_afw_fg_probe): asked by the bootstrap at each GBuffer open - the hide test's phase (boneblend.cpp).
extern "C" __declspec(dllexport) int dlvr_AfwFgProbePhase()
{
    return dlvr::AfwFgProbePhase();
}

// afw_rb_fg_mask: the bootstrap's once-per-frame copy of the foreground draw ranges (boneblend.cpp).
extern "C" __declspec(dllexport) int dlvr_AfwFgMaskRanges(unsigned* out, int maxTriples, long* gen)
{
    return dlvr::AfwFgMaskRanges(out, maxTriples, gen);
}

// WRIST HUD stage A (trace docs/traces/20261113_1300_wrist_ingame_stageA.md): the arm bone table (boneblend.cpp wristarm) for the
// bootstrap's palette row copy at the arm draws. tab = triples, slots = 8 per range, bind96 = 8 x 3x4.
extern "C" __declspec(dllexport) int dlvr_WristArmTable(unsigned* tab, signed char* slots, int maxRanges, float* bind96,
                                                       unsigned* bindOk, long* gen)
{
    return dlvr::WristArmTable(tab, slots, maxRanges, bind96, bindOk, gen);
}

// AFW item 5: the NPC draw ranges the layer has learned (bodygrab.cpp), for the bootstrap's NPC table.
extern "C" __declspec(dllexport) int dlvr_AfwNpcRanges(unsigned* out, int maxTriples, long* gen)
{
    return dlvr::NpcRanges(out, maxTriples, gen);
}

// The weapon -> shader catalogue (boneblend.cpp, INV-471): owners + names beside the ranges, and the bootstrap's notes.
extern "C" __declspec(dllexport) int dlvr_AfwFgMaskOwners(unsigned char* owner, int maxRanges, char* mesh3x128,
                                                          char* cls3x48, int* elems3, long* gen)
{
    return dlvr::AfwFgMaskOwners(owner, maxRanges, mesh3x128, cls3x48, elems3, gen);
}
extern "C" __declspec(dllexport) void dlvr_AfwNoteItemShader(int hand, const char* mesh, const char* cls, int elems,
                                                             int range, unsigned count, unsigned long long vs,
                                                             const char* vsClass, const char* vsWhy, const char* status)
{
    dlvr::AfwNoteItemShader(hand, mesh, cls, elems, range, count, vs, vsClass, vsWhy, status);
}

// GAME RUMBLE -> TOUCH HAPTICS (spec docs/superpowers/specs/2026-10-25-game-rumble-to-touch-haptics-design.md; Jay: all game rumble,
// "Both -> both hands", "No key, always on"). The bootstrap's scePadSetVibration hook hands over the VR pad's two motor bytes (the
// engine's Sony pad device +2FCD10 sends {large, small} x 255 on every motor change); both hands vibrate at the stronger one until
// the game sends 0 (controllers.cpp SetGameRumble). Called on the game's thread, only when the game changes a motor (or retries an
// apply). COST: one compare + one interlocked store; a LOG only on a change of values, capped.
extern "C" __declspec(dllexport) void dlvr_PadVibration(int largeMotor, int smallMotor)
{
    const int l = largeMotor < 0 ? 0 : (largeMotor > 255 ? 255 : largeMotor);
    const int s = smallMotor < 0 ? 0 : (smallMotor > 255 ? 255 : smallMotor);
    const int m = l > s ? l : s;
    dlvr::SetGameRumble(static_cast<float>(m) / 255.0f);
    static volatile LONG s_calls = 0, s_lines = 0, s_last = -1;
    const LONG n = InterlockedIncrement(&s_calls);
    const LONG packed = (l << 8) | s;
    if (n == 1) {
        InterlockedExchange(&s_last, packed);
        // [PROBE: PADRUMBLE] gate=none (always on)  site=HEADSET  log-only=no
        LOG("padrumble: [PADRUMBLE] first game rumble (logged unconditionally) - large %d small %d -> both hands at %.2f until the game "
            "sends 0", l, s, static_cast<float>(m) / 255.0f);
        return;
    }
    if (InterlockedExchange(&s_last, packed) == packed) { return; }   // the engine re-applying the same values
    const LONG k = InterlockedIncrement(&s_lines);
    if (k <= 300) {
        // [PROBE: PADRUMBLE] gate=none (always on)  site=HEADSET  log-only=no
        LOG("padrumble: [PADRUMBLE] #%ld large %d small %d -> both hands %.2f%s", n, l, s, static_cast<float>(m) / 255.0f,
            m == 0 ? " (stop)" : "");
    } else if (k == 301) {
        // [PROBE: PADRUMBLE] gate=none (always on)  site=HEADSET  log-only=no
        LOG("padrumble: [PADRUMBLE] change lines CAPPED at 300 - the rumble keeps reaching the controllers, unlogged");
    }
}

extern "C" __declspec(dllexport) int dlvr_OverlayActive()
{
    return dlvr::OverlayVisible() ? 1 : 0;
}

// MOD-MENU INPUT BLOCK - the policy half (Jay 2026-10-13: "Inputs still pass through to the game while it's opened,
// regardless if it's keyboard or controller"). The only block that existed was dlvr_FilterGamepad below, on the
// XInput proxy path - which the game never uses for play ("gamepad filter:" has never printed in dlvr_bootstrap.log).
// The game's real reads, from OUR engine_x64_rwdi.dll's import table and every call site (static, 2026-10-13):
//   Xbox pad  - SDL2!SDL_GameControllerGetButton (1 site, +2BD498, a 15-button loop) / GetAxis (6 sites, same fn)
//   scePad    - libScePad!scePadReadState (2 sites) -> dlvr_FilterScePad, blocked at its end via path 0
//   keyboard  - USER32 GetKeyboardState (2: +177C59 the key-event handler = IEngineImpl vtable slot 200, +235FA9 the
//               per-frame poll into device+0x1810), GetAsyncKeyState (21), GetKeyState (5), and the window
//               messages, pumped by the exe / engine (PeekMessageA) and SDL2 (PeekMessageW / GetMessageW).
//               WHICH of these carries gameplay keys is [UNKNOWN] - so every one is blocked, and the first-block
//               line below names the paths that were actually live.
// The bootstrap owns the hooks (the same mechanics/policy split as scePad); this decides, per call:
//   0 = pass, 1 = block, 3 = block but the RIGHT STICK passes (LT held - OverlayPassLook, as dlvr_FilterGamepad).
// gamedll's 5 GetKeyState sites read only VK 0x14 / 0x90 / 0x1D (Caps / Num Lock, a toggle) - not hooked.
// COST per call: one bool load (OverlayVisible); while the menu is open, one interlocked increment.
namespace {
const char* const kMenuPathName[] = {
    "scePad buffer", "SDL pad buttons", "SDL pad axes", "exe PeekMessageA", "engine PeekMessageA",
    "SDL2 PeekMessageW", "SDL2 GetMessageW", "engine GetKeyboardState", "engine GetAsyncKeyState",
    "engine GetKeyState",
    // Run 2 (2026-10-13): every path above blocked and input still reached the main menu - the engine's CInput reads
    // keyboard + mouse through DirectInput 8, loaded at runtime (engine +27E146), in no import table. Bootstrap ids 10-13.
    "dinput keyboard state", "dinput keyboard events", "dinput controller state", "dinput controller events",
    // Run 3: still through. Every keyboard event the engine queues passes ONE function, engine +2A360 (3 call sites:
    // the window-message key handler and the DirectInput keyboard loop). Bootstrap id 14.
    "engine key queue",
    // Run 5: keyboard fixed; the pad reaches the engine as SDL controller / joystick EVENTS through the engine's own
    // SDL_PollEvent (g_SdlManager -> CJoystickSDL). Bootstrap id 15.
    "SDL pad events",
    // Run 6: still through - the engine reads no pad EVENT type; every pad backend writes a per-device map read back
    // through CNewInputManager::GetJoyAxisState / GetJoyButtonState. Bootstrap ids 16 / 17.
    "joy axis getter", "joy button getter",
};
constexpr int kMenuPaths = static_cast<int>(sizeof(kMenuPathName) / sizeof(kMenuPathName[0]));
volatile LONG g_menuPathSeen[kMenuPaths] = {};
volatile LONG g_menuPathBlockedEver[kMenuPaths] = {};
volatile LONG g_menuPathBlocked[kMenuPaths] = {};   // this menu opening only; tallied and reset on close
volatile LONG g_menuWasOpen = 0;
}

extern "C" __declspec(dllexport) int dlvr_MenuInputPolicy(int path)
{
    if (path < 0 || path >= kMenuPaths) {
        return 0;
    }
    const bool open = dlvr::OverlayVisible() || dlvr::CalibScreenActive();   // the calibration screen blocks like the menu
    if (g_menuPathSeen[path] == 0 && InterlockedExchange(&g_menuPathSeen[path], 1) == 0) {
        // [PROBE: MENUBLOCK] gate=none (always on)  site=FLAT  log-only=no
        LOG("menublock: [MENUBLOCK] path '%s' FIRST CALL (logged unconditionally) - the game reads input through it; "
            "mod menu %s", kMenuPathName[path], open ? "OPEN" : "closed");
    }
    if (!open) {
        if (g_menuWasOpen != 0 && InterlockedExchange(&g_menuWasOpen, 0) != 0) {
            char tally[1024];
            int len = 0;
            tally[0] = '\0';
            for (int i = 0; i < kMenuPaths; ++i) {
                const LONG n = InterlockedExchange(&g_menuPathBlocked[i], 0);
                const int w = _snprintf_s(tally + len, sizeof(tally) - len, _TRUNCATE, "%s%s %ld",
                                          i ? ", " : "", kMenuPathName[i], n);
                if (w > 0) {
                    len += w;
                }
            }
            // [PROBE: MENUBLOCK] gate=none (always on)  site=HEADSET  log-only=no
            LOG("menublock: [MENUBLOCK] menu CLOSED - reads/messages blocked while it was open: %s", tally);
        }
        return 0;
    }
    if (g_menuWasOpen == 0) {
        InterlockedExchange(&g_menuWasOpen, 1);
    }
    InterlockedIncrement(&g_menuPathBlocked[path]);
    if (g_menuPathBlockedEver[path] == 0 && InterlockedExchange(&g_menuPathBlockedEver[path], 1) == 0) {
        // [PROBE: MENUBLOCK] gate=none (always on)  site=HEADSET  log-only=no
        LOG("menublock: [MENUBLOCK] path '%s' FIRST BLOCK - the game's read through it is neutral while the mod menu "
            "is open", kMenuPathName[path]);
    }
    return dlvr::OverlayPassLook() ? 3 : 1;
}

// Gamepad policy, decided here rather than in the bootstrap.
//
// The bootstrap is the game's xinput1_3.dll and so sits in the path of every
// pad read, but it has no settings of its own and no reason to grow any. It
// hands the state here instead, where the settings already live.
//
// Layout only - the bootstrap deliberately does not include <Xinput.h>, because
// declaring XInputGetState with its real signature collides with the void*
// export it has to provide.
extern "C" __declspec(dllexport) void dlvr_FilterGamepad(void* state)
{
    struct Pad {
        unsigned long dwPacketNumber;
        unsigned short wButtons;
        unsigned char bLeftTrigger;
        unsigned char bRightTrigger;
        short sThumbLX, sThumbLY, sThumbRX, sThumbRY;
    };
    auto* p = static_cast<Pad*>(state);
    if (!p) {
        return;
    }
    if (dlvr::OverlayVisible() || dlvr::CalibScreenActive()) {   // + the calibration screen: one more volatile read
        // Hold LT and the right stick still reaches the game, so gamepad look
        // can be exercised with the panel open. The Camera tab's convention
        // table is unusable otherwise: it asks which reading responds to a
        // stick the overlay was suppressing.
        const bool passLook = dlvr::OverlayPassLook();
        const short rx = p->sThumbRX;
        const short ry = p->sThumbRY;

        // A connected pad with nothing pressed, not a disconnected one: yanking
        // the controller out from under the engine mid-frame is a good way to
        // find out how it handles hot-unplug.
        const unsigned long packet = p->dwPacketNumber;
        memset(p, 0, sizeof(*p));
        p->dwPacketNumber = packet + 1;
        if (passLook) {
            p->sThumbRX = rx;
            // Deliberately not subject to block_pad_pitch: holding the trigger
            // is an explicit request to see pitch move.
            p->sThumbRY = ry;
        }
        return;
    }
    if (g_blockPadPitch) {
        p->sThumbRY = 0;
    }
}

// Snap turn, the pad side - see ComfortTurnMode's [DEP:] marker (boneblend.cpp) and SPEC-VR-COMFORT-2026-09-30 §5.
// Returns true when snap owns the right stick this poll: the X byte the GAME reads is centred (so the engine's own
// smooth turn never runs) and a flick queues one snap for camyaw::Hook. Returns false - and the caller's smooth path
// runs exactly as before - in smooth mode AND whenever the camyaw detour is not live, so a missing hook can never leave
// the player with no turning at all. Each status change is logged once, by name.
// COST per poll: one volatile load, a few compares; the edge detector is snap_logic.h's pure Edge.
//
// Lockpicking, the snap side (2026-10-24; a tester: "the right one didn't move so I could place the pin, but not rotate the
// barrel"; Jay, headset: "Confirmed, Snap turn is bugged for lockpicking"). The lock's barrel reads ONE action,
// _ACTION_LOCKPICKING_SCREWDRIVER_LEFT (id 0xC9, stored at gamedll +73029; read only by +784DE0, from 0.3 up), which
// inputs_pad.scr:230 binds to the right stick X - the byte this filter centres. So while the minigame runs, snap stands
// aside and the caller writes the real stick X, exactly as in smooth mode. "Runs" = BOTH of these, read on every poll:
//   (a) the LockpickController (player controller ID 27 at player+3192, INV-327) active byte +16: its Start (slot 18
//       +D3F170) sets it to 1 and its Stop (slot 19 +D3F5E0) to 0, both unconditionally (ida/lp3_out.txt);
//   (b) the InputManager's Player-target block count, IM+308 (= +304 + 4 * target 1), above 0: the minigame's Start
//       +781900 blocks target 1 (+11101F0, ++[IM+304+4t]) and its Finish +781ED0 unblocks it (+1110280) and then stops
//       the controller (ida/lp4_out.txt). Target 1 = Player is the Mac EInputTarget::ToString (comparison only).
// (a) alone is not enough: the controller is active through its walk-in stages (slot 16 +D3F790, ctrl+72) BEFORE the
// minigame exists and blocks Player - there the engine's own turn would act on the stick. While (b) holds, the turn
// actions (Player target) cannot. RE-ARM: either one dropping - the minigame's Finish, or a Stop without one - returns
// snap on the very next poll, and the edge detector is re-primed on the way out, so a stick still held from the barrel
// is "a flick we never saw start" (snap_logic.h Edge) and fires nothing; the next flick from centre snaps as before.
// Any null or fault reads as "not lockpicking" = snap exactly as before this change.
// player = [[[gd+0x1C16348]+1344]+184]+88 gated on +80 - the object the minigame's Finish reads +3192 from (INV-570);
// the InputManager = [gd+0x1C16348]+200, as the game's GetInputValue +782E60 reads it.
// COST per poll, snap mode only: up to eight guarded loads, no call (GetModuleHandleA only until the gamedll is mapped).
#if __has_include("../common/buildrva.h")
#define DLVR_SNAP_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)   // the BETA tree: the Epic build's rva
#else
#define DLVR_SNAP_GD_RVA(rva) (rva)                       // the DEV tree: Steam rvas only, as the rest of this tree
#endif
static bool SnapLockpickRunning(int* ctrlActive, int* playerBlocks)
{
    *ctrlActive = -1;
    *playerBlocks = -1;
    // Plain statics, resolved once with the module (a dynamically initialised local static needs unwinding, which
    // __try refuses - C2712). The rva is stored BEFORE the module, so a poll that sees s_gd set also sees s_gRva.
    static uintptr_t s_gRva = 0;
    static volatile uintptr_t s_gd = 0;
    if (!s_gd) {
        const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
        if (!gd) {
            return false;
        }
        s_gRva = DLVR_SNAP_GD_RVA(0x1C16348);
        MemoryBarrier();
        s_gd = gd;
    }
    __try {
        const uintptr_t g = *reinterpret_cast<const volatile uintptr_t*>(s_gd + s_gRva);
        if (!g) { return false; }
        const uintptr_t a = *reinterpret_cast<const volatile uintptr_t*>(g + 1344);
        if (!a) { return false; }
        const uintptr_t lc = *reinterpret_cast<const volatile uintptr_t*>(a + 184);
        if (!lc || !*reinterpret_cast<const volatile uintptr_t*>(lc + 80)) { return false; }
        const uintptr_t pl = *reinterpret_cast<const volatile uintptr_t*>(lc + 88);
        if (!pl) { return false; }
        const uintptr_t ctrl = *reinterpret_cast<const volatile uintptr_t*>(pl + 3192);
        if (!ctrl) { return false; }
        *ctrlActive = *reinterpret_cast<const volatile unsigned char*>(ctrl + 16);
        const uintptr_t im = *reinterpret_cast<const volatile uintptr_t*>(g + 200);
        if (!im) { return false; }
        *playerBlocks = *reinterpret_cast<const volatile int*>(im + 308);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *ctrlActive == 1 && *playerBlocks > 0;
}

// CO-OP WAITING-PLACE TELEPORT OFFER (INV-745; trace docs/traces/20261117_2045_coop_teleport_menu_latch.md). The game's hold
// progress for "appear in waiting place" is a float at PlayerDI+10340 (+BCF600 = PlayerDI+440's slot 1 writes a1+9900): -1.0 when
// no teleport is on offer, [0, 1] while it is. Read ONLY when the menu-button machine asks (menutap::NeedsOffer: the 500 ms hold
// decision and the latch) - never on an idle poll. COST when read: buildrva's cached build test, RigGamePlayerObject's four guarded
// loads, one guarded float load. *why: 0 read, 1..5 no local player (1 + RigGamePlayerObject's why), 6 the float load faulted,
// 10 the gamedll build is not STEAM / EGL (BETA only - the offset was checked on those two), 13 the gamedll is not loaded.
static bool CoopTpLoadFloat(uintptr_t addr, float* out)
{
    __try {
        *out = *reinterpret_cast<const volatile float*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static float CoopTpProgress(int* why, uintptr_t* player)
{
    *player = 0;
#if __has_include("../common/buildrva.h")
    const ::dlvr::buildrva::Build b = ::dlvr::buildrva::ModuleBuild(::dlvr::buildrva::kGamedll);
    if (b != ::dlvr::buildrva::kSteam && b != ::dlvr::buildrva::kEgl) {
        *why = b == ::dlvr::buildrva::kNotLoaded ? 13 : 10;
        return -1.0f;
    }
#endif
    int pw = 0;
    const uintptr_t pl = dlvr::RigGamePlayerObject(&pw);
    if (!pl) {
        *why = 1 + pw;
        return -1.0f;
    }
    *player = pl;
    float v = -1.0f;
    if (!CoopTpLoadFloat(pl + 10340, &v)) {
        *why = 6;
        return -1.0f;
    }
    *why = 0;
    return v;
}

static bool SnapTurnFilter(unsigned char* p, float rx)
{
    static dlvr::snapturn::Edge s_edge;
    static int s_lastMode = -1;
    static int s_lastStatus = -2;
    const int mode = dlvr::ComfortTurnMode;
    if (mode != 1) {
        if (mode != s_lastMode) {
            s_lastMode = mode;
            s_lastStatus = -2;
            // [PROBE: SNAP] gate=comfort_turn_mode  site=HEADSET  log-only=no
            LOG("snapturn: [SNAP] comfort_turn_mode=%d - SMOOTH turning, the right stick goes to the game unchanged",
                mode);
        }
        s_edge.Reset();
        return false;
    }
    const char* why = nullptr;
    const int st = dlvr::SnapTurnStatus(&why);
    if (mode != s_lastMode || st != s_lastStatus) {
        s_lastMode = mode;
        s_lastStatus = st;
        // [PROBE: SNAP] gate=comfort_turn_mode  site=HEADSET  log-only=no
        LOG("snapturn: [SNAP] comfort_turn_mode=1 (snap %.0f deg, repeat %d ms): %s%s",
            dlvr::snapturn::AngleDeg(dlvr::ComfortSnapAngle), dlvr::ComfortSnapRepeatMs,
            st == 1 ? "LIVE - the right stick now SNAPS and the game sees it centred" : "NOT LIVE - ",
            st == 1 ? "" : why);
        if (st != 1) {
            LOG("snapturn: [SNAP] ... until it is live the right stick stays SMOOTH, exactly as comfort_turn_mode=0");
        }
    }
    if (st != 1) {
        s_edge.Reset();
        return false;
    }
    // [LOADBEARING: snap lockpick pass-through] `if (lockpick) { s_edge.Reset(); return false; }` - without it the
    // lock's barrel never turns in snap mode (this function centres the only byte +784DE0 reads, ovr2xr.cpp p[0x06]).
    static int s_lastLockpick = -1;   // -1 = not evaluated yet: the first evaluation logs unconditionally
    int lpActive = -1, lpBlocks = -1;
    const bool lockpick = SnapLockpickRunning(&lpActive, &lpBlocks);
    if ((lockpick ? 1 : 0) != s_lastLockpick) {
        const bool first = s_lastLockpick < 0;
        s_lastLockpick = lockpick ? 1 : 0;
        // [PROBE: SNAP] gate=comfort_turn_mode  site=HEADSET  log-only=no
        LOG("snapturn: [SNAP] lockpick pass-through %s - LockpickController active byte %d, Player-target blocks %d "
            "(-1 = not read): %s",
            first ? "ARMED (first poll, logged unconditionally)" : (lockpick ? "ON" : "OFF"), lpActive, lpBlocks,
            lockpick ? "the right stick X goes to the game (the lock's barrel), no snaps"
                     : "snap owns the right stick");
    }
    if (lockpick) {
        s_edge.Reset();   // re-primed on the way out: a stick still held from the barrel fires no snap
        return false;
    }
    const int dir = s_edge.Update(rx, GetTickCount64(), static_cast<unsigned long long>(
                                          dlvr::ComfortSnapRepeatMs > 0 ? dlvr::ComfortSnapRepeatMs : 0));
    if (dir != 0) {
        dlvr::SnapTurnQueue(dir);
    }
    p[0x06] = 128;   // centred - the same byte stickToByte(0.0f) produces
    return true;
}

// melee_r2_suppress - see g_meleeR2Suppress's [DEP:] marker. Called from dlvr_FilterScePad AFTER vr_trigger_fire, so
// p[0x09] already holds max(real pad, VR trigger). Zeroes only the byte the GAME reads; GetControllerTrigger is not
// touched, so the mod can still use the VR trigger. Bindings, shipped DW/Data0.pak data/scripts/inputs/inputs_pad.scr
// (md5 191519ea238446f7256ebff6c682d3ae) :45-53: ATTACK, CHARGE_ATTACK, ATTACK_TRIGGER_R, FIRE_WEAPON,
// FIRE_RHAND_WEAPON, THROW_CARRYING_ITEM and CANCEL_MULTI_THROW all ride EJoy__AXIS_RIGHTTRIGGER in the DEFAULT layout.
// The "triggers" presets 2/3 swap it with RB (:316, :322) and southpaw with LT (:303) - there this byte is NOT the
// attack. Jay's layout: Default / Default (checked in-game 2026-09-30). WEAPON_THROW is R3 by default (:85), untouched.
// COST per poll: one volatile load and a compare. The class compare runs in CheckHeldItems once per item change.
static void MeleeR2Filter(unsigned char* p)
{
    static bool s_armedLogged = false;
    static bool s_down = false;
    static long s_presses = 0, s_suppressed = 0, s_passed = 0, s_notKnown = 0;
    static long long s_pollsZeroed = 0;

    const char* cls = nullptr;
    const int kind = dlvr::RightHandItemKind(&cls);
    if (!s_armedLogged) {
        s_armedLogged = true;
        // [PROBE: R2SUP] gate=melee_r2_suppress  site=HEADSET  log-only=no
        LOG("r2suppress: [R2SUP] ARMED (logged unconditionally) - the game's R2 byte reads 0 while the right hand "
            "holds class WeaponVis; the VR trigger is still read by the mod. Right hand now: %s (kind %d)",
            cls, kind);
    }
    const unsigned char in = p[0x09];
    if (kind == 1) {
        p[0x09] = 0;
        if (in) { ++s_pollsZeroed; }
    }
    // Press edges, for the log only. Hysteresis (on at 51 = the bindings' 0.2, off below 26) so an analog trigger
    // resting near the line cannot chatter a line per poll.
    if (!s_down && in >= 51) {
        s_down = true;
        ++s_presses;
        const char* verdict;
        if (kind == 1) {
            ++s_suppressed;
            verdict = "SUPPRESSED";
        } else if (kind < 0) {
            ++s_notKnown;
            verdict = "PASSED - hand NOT KNOWN (the item capture has no entity: rig not armed, not in a level, or dead)";
        } else {
            ++s_passed;
            verdict = "PASSED - not the melee class";
        }
        // Cross-check (U-c: slot 16 runs with melee, not with a firearm or the bow). Log-only.
        const unsigned long long last = dlvr::SwingPrimaryLastCallTick();
        const unsigned long long now  = GetTickCount64();
        char slot16[64];
        if (last == 0) {
            _snprintf_s(slot16, sizeof(slot16), _TRUNCATE, "never (or melee_swing_mode 0)");
        } else {
            _snprintf_s(slot16, sizeof(slot16), _TRUNCATE, "%llu ms ago", now > last ? now - last : 0ull);
        }
        // [PROBE: R2SUP] gate=melee_r2_suppress  site=HEADSET  log-only=no
        LOG("r2suppress: [R2SUP] press #%ld (R2 byte %u): right hand %s (kind %d) -> %s | slot 16 last called %s | "
            "session: suppressed %ld, passed %ld, not known %ld, polls zeroed %lld",
            s_presses, static_cast<unsigned>(in), cls, kind, verdict, slot16, s_suppressed, s_passed, s_notKnown,
            s_pollsZeroed);
    } else if (s_down && in < 26) {
        s_down = false;
    }
}

// Real gamepad policy: the engine's actual gameplay input comes through
// Sony's cross-platform pad SDK (scePadReadState), not XInput.
// XInputGetState is confirmed (2026-09-02, see CLAUDE.md) to never be called
// during real gameplay - a genuine physical controller button press produced
// zero activity through dlvr_FilterGamepad above, so block_pad_pitch is
// suspect too, not just this feature; XInputGetState was never a usable
// place to inject anything. This is the same policy split as
// dlvr_FilterGamepad - the bootstrap owns the IAT hook mechanics, this owns
// what to write - just pointed at the buffer that's actually live.
//
// ScePadData layout, confirmed against this build's own parsing code (see
// CLAUDE.md's scePad section): buttons (u32) at +0x00, left stick X/Y at
// +0x04/+0x05, right stick X/Y at +0x06/+0x07, L2 analog at +0x08, R2 analog
// at +0x09. R2 is DualShock/DualSense's equivalent of Xbox's RT, which is
// confirmed to fire a ranged weapon in this game's default bindings.
// GAME-MENU INPUT PASS-THROUGH (spec docs/superpowers/specs/2026-10-25-game-menu-input-passthrough-design.md; Jay: "We need to
// detect the menu state and allow the inputs regardless of throwables etc." -> "All 3."). While uipin::GameMenuOpen() (an in-game
// menu - pause / inventory / map / shop - published once per submit) the scePad filter hands the game the controllers as they
// are: (A) the trigger / button byte rules (melee R2, bow R2, throwable L2 / R2 / D-pad, loot Square, body grab) are undone after
// they run - except an L2 the throwable holds for its safe cancel (INV-566) - and the throwable is fed a blocking menu
// (throwable.cpp); (B) the holster D-pad taps and the holster / two-hand / loot / camouflage L1 / R1 withholds are not applied;
// (C) the decoupled move's head-yaw rotation and the snap-turn centring are skipped. Our own mod menu still blocks, last. No key:
// a stale or unreadable signal = not a menu = the rules as before. COST per poll: two volatile loads; in a menu, a 10-byte compare.
namespace {
struct MenuPassCounts {
    long polls, decouple, snap, holsterTap, supL1, supR1, l2, r2, dpad, buttons, sticks, l2Held;
};
MenuPassCounts g_mp = {};   // pad thread only
bool g_mpWas = false, g_mpFirst = true;
ULONGLONG g_mpOnMs = 0;

// Once per poll: the edge lines. Returns GameMenuOpen().
bool MenuPassBegin()
{
    const bool on = dlvr::uipin::GameMenuOpen();
    if (g_mpFirst) {
        g_mpFirst = false;
        unsigned age = 0;
        const int st = dlvr::uipin::PublishedMenuState(&age);
        // [PROBE: MENUPASS] gate=none (always on)  site=FLAT  log-only=no
        LOG("menupass: [MENUPASS] first poll (logged unconditionally) - published menu state %d (2 = in-game menu, -2 = not "
            "published yet), %u ms old -> %s", st, age, on ? "PASS-THROUGH" : "rules as before");
    }
    if (on != g_mpWas) {
        unsigned age = 0;
        const int st = dlvr::uipin::PublishedMenuState(&age);
        const ULONGLONG now = GetTickCount64();
        if (on) {
            g_mp = MenuPassCounts{};
            g_mpOnMs = now;
            // [PROBE: MENUPASS] gate=none (always on)  site=HEADSET  log-only=no
            LOG("menupass: [MENUPASS] ON - in-game menu (state %d, %u ms old): triggers, D-pad, grips and sticks go to the game as "
                "the controllers give them; the throwable is fed a blocking menu", st, age);
        } else {
            // [PROBE: MENUPASS] gate=none (always on)  site=HEADSET  log-only=no
            LOG("menupass: [MENUPASS] OFF after %llu ms (state now %d, %u ms old) - polls %ld | a rule WOULD have changed: L2 %ld, R2 "
                "%ld, D-pad %ld, other buttons %ld, sticks %ld | L2 kept for the throwable's cancel %ld | holster taps not sent %ld, "
                "L1 withholds not applied %ld, R1 %ld | decoupled-move turns skipped %ld, snap centring skipped %ld",
                static_cast<unsigned long long>(now - g_mpOnMs), st, age, g_mp.polls, g_mp.l2, g_mp.r2, g_mp.dpad, g_mp.buttons,
                g_mp.sticks, g_mp.l2Held, g_mp.holsterTap, g_mp.supL1, g_mp.supR1, g_mp.decouple, g_mp.snap);
        }
        g_mpWas = on;
    }
    if (on) { ++g_mp.polls; }
    return on;
}

// In a menu the right stick X is not run through the snap filter; counted when snap mode would have centred it. Returns true.
bool MenuPassSnap(float rx)
{
    if (dlvr::ComfortTurnMode == 1 && fabsf(rx) > 0.1f) { ++g_mp.snap; }
    return true;
}

// After the byte rules: in a menu, undo what they changed (counted per field). An L2 the throwable holds stays (INV-566).
void MenuPassRestore(unsigned char* p, const unsigned char* before)
{
    constexpr uint32_t kDpad = 0x10u | 0x20u | 0x40u | 0x80u;
    uint32_t bNow = 0, bWas = 0;
    for (int i = 0; i < 4; ++i) {
        bNow |= static_cast<uint32_t>(p[i]) << (8 * i);
        bWas |= static_cast<uint32_t>(before[i]) << (8 * i);
    }
    if ((bNow ^ bWas) & kDpad) { ++g_mp.dpad; }
    if ((bNow ^ bWas) & ~kDpad) { ++g_mp.buttons; }
    if (p[4] != before[4] || p[5] != before[5] || p[6] != before[6] || p[7] != before[7]) { ++g_mp.sticks; }
    if (p[9] != before[9]) { ++g_mp.r2; }
    const bool holdL2 = dlvr::ThrowableHoldsL2();
    if (p[8] != before[8]) {
        if (holdL2) { ++g_mp.l2Held; } else { ++g_mp.l2; }
    }
    for (int i = 0; i < 10; ++i) {
        if (i == 8 && holdL2) { continue; }
        p[i] = before[i];
    }
}
}  // namespace

extern "C" __declspec(dllexport) void dlvr_FilterScePad(void* pData)
{
    // >>> THIS IS WHAT MAKES THE LAYER'S PROBES WORK IN FLATSCREEN. 2026-09-13. <<<
    //
    // THE LAYER IS LOADED IN FLAT - PreloadOvrRuntime() LoadLibraryA's it from the
    // bootstrap unconditionally, not gated on VR, and the bootstrap's scePad hook
    // resolves this very export through GetModuleHandleA and calls it. So the tick
    // below genuinely runs. **What did NOT run was LoadLayerConfig()**, whose only
    // two callers are GameEyeFov() and EyeRenderSize() - both taking a Session*,
    // i.e. both VR CAPI entry points. No session in flat, so the config was never
    // read, so EVERY setting sat at its compiled default and every diag_ gate was
    // false.
    //
    // **THAT is why a flat run logs nothing from the layer** - not because the
    // module is absent, which is what this project had concluded from "[DLSSFLAT]
    // present, zero [DLSSPROBE]". The module was there the whole time with its
    // settings unread. The comment below this block has claimed flatscreen support
    // since it was written, and it was true about the CALL and useless in practice.
    //
    // LoadLayerConfig() is once-guarded on g_configLoaded, so this is idempotent
    // and the VR path's own call becomes a no-op return.
    //
    // KNOWN, DELIBERATE VR BEHAVIOUR CHANGE: in VR the config now loads at the
    // first pad poll instead of at the first FOV query, which is EARLIER. It is
    // logged when it happens so the ordering change is attributable rather than
    // silent - if a VR regression ever appears, that line dates it.
    if (!g_configLoaded) {
        LoadLayerConfig();
        LOG("config: loaded from the scePad poll, not from a VR entry point. In "
            "FLATSCREEN this is the only thing that loads it at all; in VR it "
            "simply means the pad polled before the first FOV query.");
    }

    auto* p = static_cast<unsigned char*>(pData);
    if (!p) {
        return;
    }

    // Driven from here rather than from LevelCamera() on purpose. The probe's
    // whole job is comparing a VR run against a FLATSCREEN run, and
    // LevelCamera only runs once the camera hook installs - which only ever
    // happens in VR, so hanging it there made the flatscreen half of the
    // experiment impossible to collect (confirmed the hard way: a flatscreen
    // run logged zero visibilityprobe lines and zero camhook lines).
    // scePadReadState is polled continuously in BOTH modes - the heartbeat at
    // the end of this function fires in headset-less runs - so this is the
    // right cadence to ride. Self-throttled and one-shot internally; costs a
    // bool check per poll once it has answered.
    dlvr::ProbeVisibilityRangeOnce();
    dlvr::ApplyStreamingBudgetOnce();
    dlvr::StartLodOverrideOnce();
    dlvr::StartRoadDrapeOnce();    // map fixes, ported from DEV 2026-11-07: one-shot, each starts one worker thread
    dlvr::StartEdgeStripOnce();
    dlvr::StartDecalSeatOnce();
    dlvr::StartDecalPlsOnce();
    dlvr::StartRastOccOnce();
    dlvr::StartJitVrOnce();
    dlvr::StartClipFarOnce();
    dlvr::StartNearClipOnce();   // vr_near_clip_cm (INV-549) - verifies, then idles at the game's value unless set
    dlvr::StartAfwAltEyeOnce();
    dlvr::StartDamageWatchOnce();
    // ANIMATION SUPPRESSION. One-shot internally. Rides this poll for the same
    // reason the others do - it is a game thread in the UPDATE phase and it
    // fires in flatscreen too, so a prologue mismatch is discoverable without
    // spending a headset run on it.
    dlvr::AnimSuppressInstall();
    // [BODYWRITERS] pass-through detours (diag_body_writers). One-shot once gamedll is mapped; same poll, same
    // reason: a game thread in the UPDATE phase, and a prologue mismatch shows in flat too.
    dlvr::BodyWritersInstall();
    // [BONESANIM] pass-through detour on gamedll BFF080 (diag_bones_anim). Same poll, same one-shot pattern.
    dlvr::BonesAnimInstall();
    // [EYEPOINT] pass-through detour on gamedll C75380 (cam_eye_forward / cam_eye_arc). Same poll, same one-shot pattern.
    dlvr::EyePointInstall();
    dlvr::StartBoneDumpOnce();
    dlvr::RigStage4Tick();
    // Rigging step 1: the read-only bone-blend probe. Rides this poll rather
    // than the camera hook so it also runs in flatscreen, and because the
    // camera hook is the render phase.
    dlvr::BoneBlendTick();
    dlvr::ApplyStreamPriorityDistance();
    // Head smoothing for the absolute aim mapping. Has to ride this poll,
    // not the aim hook itself: ComputeAbsoluteAim only runs on a shot, and
    // samples seconds apart cannot be low-pass filtered. Cheap - one head
    // pose read and a lerp, and it early-outs on the bool when disabled.
    dlvr::AimHeadSmoothUpdate();

    // R9 grapple hand-aim: one import-table pointer, installed once (latched flag - a
    // single load and compare per poll after that). See grappleaim.cpp.
    dlvr::GrappleAimInstall();


    // Observe-only aim hook for the grapple investigation, and SELF-RENEWING -
    // which is load-bearing, not a nicety. The hook carries a 2000-hit safety
    // cap and a single shot generates 400+ hits, so about five shots exhaust
    // it. Worse, the cap's cleanup AND its "auto-disarmed" log line are both
    // emitted from AimHookActive(), which is normally polled BY THE OVERLAY.
    // With the overlay closed - as it is during any headset test - the hook
    // expires completely silently: a real session logged five shots and then
    // nothing whatsoever for the rest of the test, with no disarm message and
    // no sign anything had stopped. Polling AimHookActive() here drives that
    // cleanup and lets us re-arm, so the hook survives a whole before/after
    // comparison. AimHookOverride is forced off: we want the game's own
    // UNMODIFIED value, since the question is whether grappling corrupts it.
    //
    // vr_aim_redirect drives the same machinery with the override ON - the
    // real feature rather than the diagnostic - and takes precedence, since
    // observing the game's own value is pointless once we are replacing it.
    if (g_aimHookAutoObserve || g_vrAimRedirect) {
        static ULONGLONG lastArmMs = 0;
        static int armCount = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (!dlvr::AimHookActive() && (nowMs - lastArmMs) > 1000) {
            lastArmMs = nowMs;
            AimHookOverride = g_vrAimRedirect;
            if (dlvr::AimHookInstall()) {
                if (++armCount == 1) {
                    if (g_vrAimRedirect) {
                        LOG("aimhook: auto-installed with the override LIVE - shots "
                            "follow the right controller (%s aim). Re-arms "
                            "automatically when the safety cap trips, so it will "
                            "not go quiet mid-session.",
                            AimAbsolute ? "absolute/live" : "relative-to-head");
                    } else {
                        LOG("aimhook: auto-installed OBSERVE-ONLY for the grapple "
                            "investigation - logging the game's own unmodified shoot "
                            "direction. Re-arms automatically when the safety cap trips, "
                            "so it will not go quiet mid-test.");
                    }
                } else if ((armCount % 10) == 0) {
                    LOG("aimhook: re-armed after the safety cap (%d times so far)",
                        armCount);
                }
            }
        }
    }

    // Snapshot of the REAL pad state, before any of our own writes below -
    // for the movement-confirmation heartbeat at the end of this function.
    // See that heartbeat's own comment for why this exists.
    const uint32_t rawButtons = *reinterpret_cast<uint32_t*>(p);
    const unsigned char rawLX = p[0x04], rawLY = p[0x05], rawRX = p[0x06], rawRY = p[0x07];
    const unsigned char rawL2 = p[0x08], rawR2 = p[0x09];

    // Sprint, 2026-09-02: confirmed live in-game that left thumbstick click
    // is genuinely Dying Light's sprint binding (Sony's standard L3 bit,
    // 0x2) - superseded the earlier "always force it on" diagnostic
    // (diag_force_l3) now that the assumption itself is verified, not just
    // the mechanism. Independent of vr_trigger_fire's own gate below - a
    // separate feature that should work regardless of whether the others
    // are enabled.
    if (g_vrSprint) {
        bool pressed = false;
        if (dlvr::GetControllerStickClick(dlvr::kLeftHand, pressed) && pressed) {
            auto* buttons = reinterpret_cast<uint32_t*>(p);
            *buttons |= 0x2u;
        }
    }

    // Movement/turning, 2026-09-02 - independent of vr_trigger_fire's own
    // gate below, same reasoning as vr_sprint above: these are separate
    // features that should each work regardless of whether the others are
    // enabled. Overwrites outright rather than blending with a real pad's
    // stick (unlike the trigger's max()-with-real-input approach) - there is
    // no sensible way to combine two different bipolar axis sources, unlike
    // a unipolar trigger where "whichever pulled harder" is well-defined.
    // -1..1 to a centered byte (128 = neutral, matching Sony's public
    // ScePadAnalogStick convention already confirmed for the trigger bytes).
    // Game-menu input pass-through (MenuPassBegin above): read once per poll, used by the stick, grip and trigger rules below.
    const bool gameMenu = MenuPassBegin();
    auto stickToByte = [](float v) -> unsigned char {
        const float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<unsigned char>(128 + static_cast<int>(c * 127.0f));
    };
    if (g_vrMove) {
        float lx = 0.0f, ly = 0.0f;
        if (dlvr::GetControllerStick(dlvr::kLeftHand, lx, ly)) {
            // ---- CAMERA / BODY DECOUPLING (2026-09-05) ----------------------
            //
            // The problem: turning your head while running curves where the
            // character actually goes. Root cause, confirmed from the logs
            // this session: the game DOES call ovr_GetTrackingState (8
            // distinct threads) and its RenderPose echo carries real values
            // (+-0.0320 = the IPD half-offset, -1.108 = head height) - so it
            // reads the head pose from us, applies it to its own camera, and
            // derives movement direction from that camera. Note this
            // contradicts the older note in CLAUDE.md claiming the game never
            // calls those functions and RenderPose is permanently zero; that
            // was true of an earlier build state and is not true now.
            //
            // Simply withholding head yaw does not work - the game must
            // render with head orientation or the view stops tracking. But we
            // do not have to fight that at all, because we own the stick
            // values. The game computes
            //     worldDir = rotate(stick, cameraYaw),  cameraYaw ~ bodyYaw + headYaw
            // so injecting a counter-rotated stick
            //     stick'   = rotate(stick, -headYaw)
            // makes the head term cancel exactly:
            //     worldDir = rotate(stick, bodyYaw)
            // i.e. push forward and you travel along the BODY's forward no
            // matter where you are looking. Pure math in our own filter - no
            // hooks, no engine mutation, nothing that can crash.
            //
            // headYaw here is the head's yaw in play space, which IS the
            // offset from body-forward: the body only ever turns via the
            // vr_turn injection below, and that turn is applied by the game
            // to its own camera, so it is common to both sides and cancels.
            // In an in-game menu the left stick pans / moves the cursor: no head-yaw rotation (game-menu pass-through, C).
            if (gameMenu && g_vrDecoupleMove && (fabsf(lx) > 0.1f || fabsf(ly) > 0.1f)) { ++g_mp.decouple; }
            if (g_vrDecoupleMove && !gameMenu) {
                const XrQuaternionf& h = dlvr::LastReportedHead();
                // Yaw about Y from the quaternion, standard extraction.
                const float headYaw = atan2f(2.0f * (h.w * h.y + h.x * h.z),
                                             1.0f - 2.0f * (h.y * h.y + h.x * h.x));
                // CLAMPED - and this is a KNOWN COMPROMISE, not a tuning knob.
                // See CLAUDE.md, "the sprint conflict". Dying Light only
                // sustains sprint while the stick reads near-forward. A full
                // counter-rotation makes the stick point sideways exactly when
                // you turn your head, so the game decides you have stopped
                // running straight and drops the sprint - confirmed in play.
                // Limiting the correction keeps the stick forward ENOUGH to
                // hold sprint while still cancelling most of the curve for
                // ordinary glancing. The cost is that large head turns are
                // only partially corrected, so some curve returns at the
                // extremes. The real fix is engine-side - make the movement
                // code read a body yaw we control rather than the render
                // camera's - and is not something the stick can fake: ANY
                // correction routed through the stick necessarily makes the
                // stick non-forward, which is precisely what breaks sprint.
                const float maxRad = g_vrDecoupleMaxDeg * 0.01745329252f;
                float correction = -headYaw;
                if (correction > maxRad)  { correction = maxRad; }
                if (correction < -maxRad) { correction = -maxRad; }

                const float c = cosf(correction), s = sinf(correction);
                const float rx2 = lx * c - ly * s;
                const float ry2 = lx * s + ly * c;
                lx = rx2;
                ly = ry2;
                static int decLog = 0;
                if ((fabsf(lx) > 0.1f || fabsf(ly) > 0.1f) && (++decLog % 90) == 0) {
                    LOG_PERIODIC("vr_decouple: headYaw=%.1f deg, applied %.1f deg (clamped at "
                        "+-%.0f to preserve sprint) -> stick x=%.2f y=%.2f",
                        headYaw * 57.2957795f, correction * 57.2957795f,
                        g_vrDecoupleMaxDeg, lx, ly);
                }
            }
            // Torso from controllers turns the body toward the hands; walking along the LOOK is now done at the game's walk call
            // (boneblend.cpp walklook, 2026-11-18 - docs/superpowers/specs/2026-11-18-walk-follows-look-design.md), not by turning
            // this stick, so sprint's near-forward check sees the stick as pushed.
            // Y confirmed inverted 2026-09-02 (pushing the stick forward
            // moved the character backward) - OpenXR's +y-is-forward does
            // not match this buffer's own convention on this axis. Negated
            // here now that it's a confirmed fact, not a guess.
            p[0x04] = stickToByte(lx);
            p[0x05] = stickToByte(-ly);
            static int moveLogCounter = 0;
            if ((fabsf(lx) > 0.1f || fabsf(ly) > 0.1f) && (++moveLogCounter % 60) == 0) {
                LOG_PERIODIC("vr_move (scepad): x=%.2f y=%.2f -> byte %u,%u", lx, ly, p[0x04], p[0x05]);
            }
        }
    }
    // The vr_* (scepad) trace lines in this function are LOG_PERIODIC since 2026-10-07 (audit section 3): bring-up
    // confirmations from 2026-09-02/03 that printed on every move / press. diag_periodic_logs=1 brings them back.
    // vr_menu_button stays a plain LOG - it prints only on a state change, and a tap is a few polls long.
    dlvr::HeadFollowStatusTick();   // comfort_body_follows_head by name, once per status change (rig off included)
    float wheelX = 0.0f, wheelY = 0.0f;
    const bool wheelOwnsStick = !gameMenu && dlvr::HolsterWheelStick(wheelX, wheelY);   // last poll's wheel (no turn while choosing)
    if (g_vrTurn && !wheelOwnsStick) {
        float rx = 0.0f, ry = 0.0f;
        if (dlvr::GetControllerStick(dlvr::kRightHand, rx, ry) && (gameMenu ? MenuPassSnap(rx) : !SnapTurnFilter(p, rx))) {
            p[0x06] = stickToByte(rx);
            // Right stick Y (pitch) deliberately left untouched - out of
            // scope for this feature, see block_pad_pitch for that axis.
            static int turnLogCounter = 0;
            if (fabsf(rx) > 0.1f && (++turnLogCounter % 60) == 0) {
                LOG_PERIODIC("vr_turn (scepad): x=%.2f -> byte %u", rx, p[0x06]);
            }
        }
    }
    // Right stick Y (Controls tab, plan 2026-10-28-controls-rebind Task 5; Jay: "Yes, go ahead" on G3): forwarded so a user can
    // bind actions to right stick up / down. Before this it never reached the game (123,388 archived heartbeats: RY 128 raw AND
    // injected). ONLY with this build's Data3 (AfwData3Allowed = the bootstrap's verdict OURS): its template leaves Look up / down
    // UNBOUND, while an older / stock inputs_pad.scr binds _ACTION_ROTATE_UP/DOWN to AXIS_RY and would tilt the aim. Not in its own
    // block's way: separate from the turn block above (snap owns THAT block), and skipped while a wheel owns the stick (contextual
    // wins - it writes p[0x07] itself below). Lockpicking does not read RY (SCREWDRIVER_UP/DOWN "read by nothing", STATE 10-24).
    // Not in a game menu: there RY was 128 too, and a menu's own stick use is not a base bind - forwarding it would change menus
    // for every user, binds or not.
    // COST per poll: one cached static compare (AfwData3Allowed after its first final verdict) + the stick read.
    if (g_vrTurn && !wheelOwnsStick && !gameMenu && dlvr::AfwData3Allowed()) {
        float rx = 0.0f, ry = 0.0f;
        if (dlvr::GetControllerStick(dlvr::kRightHand, rx, ry)) {
            p[0x07] = stickToByte(-ry);   // up = stickToByte(-y), the convention the wheel (below) and vr_move (p[0x05]) use
        }
    }

    // Face buttons, 2026-09-03 - independent of the other gates above, same
    // reasoning as vr_sprint/vr_move/vr_turn: a separate feature that should
    // work regardless of whether the others are enabled. Sony's public
    // ScePadButton bits (Cross/Circle/Square/Triangle = A/B/X/Y), same
    // family as the already-confirmed L3 (0x2) and TOUCH_PAD (0x100000)
    // bits - OR'd in, never cleared, so a real pad's own press still works
    // too (matches vr_trigger_fire's non-destructive approach, unlike the
    // stick axes which have no such option).
    // SteamVR X + Y CHORD (Jay 2026-11-18: "60 ms, combo works in menus, keep Options tap" -> "Do it"; trace
    // docs/traces/20261118_1047_steamvr_xy_menu_chord.md; menuchord_logic.h, unit-tested): on SteamVR (which takes the left menu
    // button) with the left hand not on Index, the first of X / Y is held back 60 ms; both down inside it = the menu button's `down`
    // for the tap / hold machine below (tap = SELECT, 0.5 s = START), and neither bit reaches the game until both are up. A single
    // press passes 60 ms late; a tap shorter than that is re-sent as a 100 ms pulse. Everywhere else X / Y pass exactly as before.
    // Also inside in-game menus (Jay's ruling) - this runs before the mpBefore snapshot, so a withheld bit stays withheld there.
    // COST per poll: two cached-bool reads + a few compares.
    bool xyChordDown = false;
    if (g_vrFaceButtons) {
        auto* buttons = reinterpret_cast<uint32_t*>(p);
        bool pressed = false;
        static int faceLogCounter = 0;
        bool anyPressed = false;
        if (dlvr::GetControllerButton(dlvr::kButtonA, pressed) && pressed) {
            *buttons |= 0x4000u;  // Cross
            anyPressed = true;
        }
        if (dlvr::GetControllerButton(dlvr::kButtonB, pressed) && pressed) {
            *buttons |= 0x2000u;  // Circle
            anyPressed = true;
        }
        bool rawX = false, rawY = false;
        if (!dlvr::GetControllerButton(dlvr::kButtonX, rawX)) { rawX = false; }
        if (!dlvr::GetControllerButton(dlvr::kButtonY, rawY)) { rawY = false; }
        const bool chordOn = g_runtimeIsSteamVr && !dlvr::ControllerLeftIsIndexProfile();
        static dlvr::menuchord::Machine s_chord;
        static bool s_chordFirst = true;
        if (s_chordFirst) {
            s_chordFirst = false;
            // [PROBE: XYCHORD] gate=vr_face_buttons  site=HEADSET  log-only=no
            LOG("vr_face_buttons (scepad): [XYCHORD] first poll (logged unconditionally) - runtime SteamVR %d, left hand Index %d -> "
                "X + Y chord %s (window %llu ms, tap pulse %llu ms)", g_runtimeIsSteamVr ? 1 : 0,
                dlvr::ControllerLeftIsIndexProfile() ? 1 : 0, chordOn ? "ON = the menu button" : "OFF - X / Y pass as before",
                dlvr::menuchord::kWindowMs, dlvr::menuchord::kTapPulseMs);
        }
        static bool s_chordOnSaid = false;
        if (chordOn != s_chordOnSaid) {
            s_chordOnSaid = chordOn;
            // [PROBE: XYCHORD] gate=vr_face_buttons  site=HEADSET  log-only=no
            LOG("vr_face_buttons (scepad): [XYCHORD] chord now %s (runtime SteamVR %d, left hand Index %d)", chordOn ? "ON" : "OFF",
                g_runtimeIsSteamVr ? 1 : 0, dlvr::ControllerLeftIsIndexProfile() ? 1 : 0);
        }
        const dlvr::menuchord::State chordBefore = s_chord.st;
        const dlvr::menuchord::Out co = dlvr::menuchord::Step(s_chord, chordOn, rawX, rawY, GetTickCount64());
        xyChordDown = co.chord;
        if (s_chord.st != chordBefore) {
            // Logged on each state change only (a chord lasts a few hundred ms; a tap a few polls).
            // [PROBE: XYCHORD] gate=vr_face_buttons  site=HEADSET  log-only=no
            LOG("vr_face_buttons (scepad): [XYCHORD] %s -> %s (X %d Y %d)", dlvr::menuchord::StateName(chordBefore),
                dlvr::menuchord::StateName(s_chord.st), rawX ? 1 : 0, rawY ? 1 : 0);
        }
        if (co.sendX) {
            *buttons |= 0x8000u;  // Square
            anyPressed = true;
        }
        if (co.sendY) {
            *buttons |= 0x1000u;  // Triangle
            anyPressed = true;
        }
        if (anyPressed && (++faceLogCounter % 15) == 0) {
            LOG_PERIODIC("vr_face_buttons (scepad): buttons=0x%08X", *buttons);
        }
    }

    // R3, 2026-09-03 - same OR'd-in, non-destructive treatment as the face
    // buttons above (Sony's public R3 bit, 0x4 - the same family as the
    // already-confirmed L3/0x2). Independent of every other gate here.
    if (g_vrStickClickRight) {
        bool pressed = false;
        if (dlvr::GetControllerStickClick(dlvr::kRightHand, pressed) && pressed) {
            auto* buttons = reinterpret_cast<uint32_t*>(p);
            *buttons |= 0x4u;
            static int r3LogCounter = 0;
            if ((++r3LogCounter % 15) == 0) {
                LOG_PERIODIC("vr_stick_click_right (scepad): buttons=0x%08X", *buttons);
            }
        }
    }

    // Left menu button, 2026-09-03; press/hold split 2026-10-03 (Jay: "Single press should be SELECT"; then,
    // a double press being a Virtual Desktop shortcut: "Make it a 1 second hold for START instead"; the 1 s hold
    // was also a VD shortcut: "Make the hold .5 seconds"). The only
    // controller-side menu input Touch exposes at all lives on the left controller (see controllers.h). Same
    // OR'd-in treatment as every button here.
    //   SELECT = TOUCH_PAD 0x100000 - shipped Data0.pak inputs_pad.scr:35 binds EJoy__BUTTON_TOUCHPAD beside
    //            EJoy__BUTTON_BACK to _ACTION_INVENTORY. The engine bit-tests 0x100000 in its own scePad read
    //            (ARCHIVE layout table). Headset-verified 2026-10-03 on build 2bbb04c1 (Jay: "That works").
    //   START  = OPTIONS 0x8 - the previous single-press mapping.
    // A press cannot be told from the start of a hold until it is released or reaches kMenuHoldMs, so SELECT
    // is sent on a release before kMenuHoldMs (a 100 ms pulse, the length INV-299 measured working), and START
    // from kMenuHoldMs for as long as the button stays held. SELECT itself can therefore never be HELD -
    // inputs_pad.scr:91's 0.5 s TOUCHPAD hold action (_ACTION_APPEAR_IN_WAITING_PLACE) was unreachable here - until:
    // CO-OP TELEPORT LATCH (2026-11-17, Jay: "Yes, build it that way"; INV-745; trace 20261117_2045_coop_teleport_menu_latch): when the
    // 0.5 s hold lands while the game OFFERS the waiting-place teleport (CoopTpProgress in [0, 1]) and no in-game menu is up, the hold
    // LATCHES a held TOUCH_PAD instead of START - the game needs >= 1.0 s of held input, past VD's ~1 s menu-hold shortcut (D-175), so
    // the player lets go and the latch holds it until the teleport fires, the offer ends, an in-game menu opens, the button is pressed
    // again, or 3 s pass (menutap_logic.h, unit-tested). Not on offer = SELECT / START exactly as before. The offer is read only at
    // the hold decision and during the latch.
    if (g_vrMenuButton) {
        constexpr uint32_t kSelectBit = 0x100000u;  // TOUCH_PAD
        constexpr uint32_t kStartBit = 0x8u;        // OPTIONS
        static dlvr::menutap::Machine s_tap;
        bool pressed = false;
        // + the SteamVR X + Y chord (face-button block above; false off SteamVR / with vr_face_buttons=0). OR'd, so PSVR2's right
        // Options tap (Sony's remap of this button) keeps working beside it (Jay 2026-11-18: "keep Options tap").
        const bool down = (dlvr::GetControllerMenuButton(pressed) && pressed) || xyChordDown;
        const ULONGLONG now = GetTickCount64();
        const dlvr::menutap::State before = s_tap.st;
        bool offer = false;
        float prog = -1.0f;
        int tpWhy = -1;
        uintptr_t tpPlayer = 0;
        if (dlvr::menutap::NeedsOffer(s_tap, down, now)) {
            prog = CoopTpProgress(&tpWhy, &tpPlayer);
            offer = tpWhy == 0 && dlvr::menutap::OfferFromProgress(prog);
            static bool s_tpFirst = true;
            if (s_tpFirst) {
                s_tpFirst = false;
                // [PROBE: COOPTP] gate=vr_menu_button  site=HEADSET  log-only=no
                LOG("vr_menu_button (scepad): [COOPTP] first teleport-offer read (logged unconditionally) - read %d (0 read, 1-5 no local "
                    "player, 6 the float faulted, 10 gamedll build not STEAM / EGL, 13 gamedll not loaded), player %p, progress %.3f -> %s",
                    tpWhy, reinterpret_cast<void*>(tpPlayer), prog, offer ? "ON OFFER" : "not on offer");
            }
            static bool s_tpBuildSaid = false;
            if (tpWhy == 10 && !s_tpBuildSaid) {
                s_tpBuildSaid = true;
                // [PROBE: COOPTP] gate=vr_menu_button  site=HEADSET  log-only=no
                LOG("vr_menu_button (scepad): [COOPTP] REFUSED - this gamedll build is not STEAM or EGL (the PlayerDI+10340 offset was "
                    "checked on those two only); the co-op teleport latch never engages, the 0.5 s hold is START as before");
            }
        }
        const dlvr::menutap::Out o = dlvr::menutap::Step(s_tap, down, now, offer, prog, gameMenu);
        auto* buttons = reinterpret_cast<uint32_t*>(p);
        if (o.touchpad) {
            *buttons |= kSelectBit;
        } else if (o.start) {
            *buttons |= kStartBit;
        }
        // Logged on each state change only - unthrottled, since a tap lasts a few polls and a %-counter
        // throttle could miss it entirely.
        if (s_tap.st != before) {
            if (s_tap.st == dlvr::menutap::State::TeleportLatch || o.ended != dlvr::menutap::EndWhy::None) {
                // [PROBE: COOPTP] gate=vr_menu_button  site=HEADSET  log-only=no
                LOG("vr_menu_button (scepad): [COOPTP] %s -> %s, progress %.3f (read %d), ended: %s, buttons=0x%08X",
                    dlvr::menutap::StateName(before), dlvr::menutap::StateName(s_tap.st), prog, tpWhy,
                    dlvr::menutap::EndName(o.ended), *buttons);
            } else {
                LOG("vr_menu_button (scepad): %s -> %s, buttons=0x%08X", dlvr::menutap::StateName(before),
                    dlvr::menutap::StateName(s_tap.st), *buttons);
            }
        }
    }

    // Grip buttons, 2026-09-03 - Sony's L1/R1 bits (0x400/0x800). Touch has
    // no physical bumpers, only analog squeeze, so a discrete press is
    // synthesised by thresholding at 0.5 - matches how a real DualSense's
    // own digital L1/R1 would read (never partially pressed), rather than
    // trying to feed an analog value into what this game reads as a plain
    // digital bit.
    // Right-shoulder holster (SPEC-SHOULDER-HOLSTER-2026-09-29): true only while a grip press that STARTED in
    // the shoulder zone is held - that press must not reach the game as R1 (jump). holster_enable=0 -> false.
    // While the holster's weapon-switch tap is held, D-pad right (Sony 0x20) is OR'd in - non-destructive,
    // like every button here, so a real pad's own D-pad still works.
    // Right hip (Jay 2026-10-04): a second right-grip zone; its tap is D-pad left (0x80, item swap).
    bool holsterDpadRight = false;
    bool holsterDpadLeft = false;
    // Hip yield (spec 2026-10-26 §2.4): a hip press waiting for the severed-limb reading withholds R1 / L1 (the return) but does NOT
    // own the grip, so LootPoll below sees the press too - it is handed the OWNS flags, not the withhold.
    bool holsterOwnsR = false;
    const bool holsterSuppressR1 = dlvr::HolsterPoll(g_vrGrip, holsterDpadRight, holsterDpadLeft, holsterOwnsR, gameMenu);
    // Selection wheels (plan 2026-10-27 Task 3): while a wheel is open (or confirming) it owns the RIGHT stick - full deflection
    // toward the slot to steer the game's cursor to; up = stickToByte(-y), the convention vr_move's p[0x05] uses. After the turn
    // block, so it overwrites this poll's turn byte; the turn block also skips while a wheel owns the stick. COST: two volatile reads.
    {
        float wx = 0.0f, wy = 0.0f;
        if (!gameMenu && dlvr::HolsterWheelStick(wx, wy)) {
            p[0x06] = stickToByte(wx);
            p[0x07] = stickToByte(-wy);
        }
        // The arrow wheel keeps X (Square 0x8000, the vr_face_buttons bit) down 150 ms past the player's release. OR'd, as always.
        if (!gameMenu && dlvr::HolsterWheelHoldsX()) {
            *reinterpret_cast<uint32_t*>(p) |= 0x8000u;
        }
    }
    // In an in-game menu no holster tap is sent (game-menu pass-through, B) - counted.
    if (gameMenu && (holsterDpadRight || holsterDpadLeft)) { ++g_mp.holsterTap; }
    if (holsterDpadRight && !gameMenu) {
        *reinterpret_cast<uint32_t*>(p) |= 0x20u;
    }
    if (holsterDpadLeft && !gameMenu) {
        *reinterpret_cast<uint32_t*>(p) |= 0x80u;
    }
    // Left hip + left temple holsters (Jay 2026-09-30): true only while a LEFT grip press that started in one of
    // those zones is held - that press must not reach the game as L1. The tap fires on the grip: D-pad down
    // (0x40, heal - Jay 2026-10-04, was D-pad left) from the hip, D-pad up (0x10, torch) from the head. Both
    // zones off -> false.
    bool holsterDpadDown = false;
    bool holsterDpadUp = false;
    bool holsterOwnsL = false;
    const bool holsterSuppressL1 = dlvr::HolsterLeftPoll(g_vrGrip, holsterDpadDown, holsterDpadUp, holsterOwnsL);
    if (gameMenu && (holsterDpadDown || holsterDpadUp)) { ++g_mp.holsterTap; }
    if (holsterDpadDown && !gameMenu) {
        *reinterpret_cast<uint32_t*>(p) |= 0x40u;
    }
    if (holsterDpadUp && !gameMenu) {
        *reinterpret_cast<uint32_t*>(p) |= 0x10u;
    }
    // Physical crouch (plan 2026-10-27 Task 9): after the holsters, on this thread on purpose - it is the engine's pad-delivery
    // thread, so the 0x0F0F press reaches the InputManager before this frame's own pad events. Sends nothing to the pad bytes.
    // No new crouch in an in-game menu, our overlay or the calibration screen. physical_crouch_enable=0 -> one bool test.
    dlvr::PhysCrouchPoll(gameMenu || dlvr::OverlayVisible() || dlvr::CalibScreenActive());
    // Manual reload (reload.cpp, plan 2026-11-01-manual-reload-pistol Task 5): publishes the left grip for the step machine; a press
    // that starts at the pistol's mag / slide, or mid-reload, is the reload's - not L1. AFTER HolsterLeftPoll (the pouch press is the
    // holster's own L1 withhold). manual_reload=0 -> false at one bool test.
    // Rifle plan Task 5 (Task 0 U-d): moved BEFORE TwoHandPoll so the press the reload owns (decided once, at its edge, in
    // ReloadPadPoll) is never also a two-handed grab - one press at a rifle's mag could start both. After a reload the two-handed
    // grip returns only on a NEW press at the front grip (no automatic re-grip).
    bool reloadSuppressL1 = false;
    dlvr::ReloadPadPoll(g_vrGrip, reloadSuppressL1);
    // Two-handed grip (SPEC-TWO-HANDED-GRIP-2026-10-09): true while the left hand holds a two-handed firearm - that
    // left grip press is the grab, not L1. AFTER the left holsters, so a press a holster took is never also a grab.
    // rig_two_hand_grip=0 -> false.
    // [LOADBEARING: manual reload two-hand arbitration] (manual_reload, rig_two_hand_grip) a press the reload owns is refused as a
    //      two-handed grab (the machine's holsterBusy refusal at the press edge, twohand_logic.h Machine::Update) - without it one
    //      press at a rifle's mag starts the reload AND the two-handed hold (Task 0 U-d). COST: two bool ors per poll.
    // Fix round 1: reloadSuppressL1 also goes in alone, only so the PRESS line names the reload as the owner (not a holster).
    const bool twoHandSuppressL1 =
        dlvr::TwoHandPoll(g_vrGrip, dlvr::reload::TwoHandPressRefused(holsterSuppressL1, reloadSuppressL1), reloadSuppressL1);
    // Loot by hand (spec 2026-10-23-loot-by-hand-design): AFTER the holsters and the two-handed hold, which own the grip first. A
    // grip held on a body (settling / searching) is the loot, not L1 / R1. loot_by_hand=0 -> both false at one bool test.
    // [LOADBEARING: manual reload press arbitration - loot / push-grab] (manual_reload) rifle plan Task 5 fix round 1: a LEFT press the
    //      manual reload owns (reloadSuppressL1 - at the mag / handle or mid-reload, decided at its edge by ReloadPadPoll) is also
    //      "owned" for LootPoll and PushGrabPoll, so one press never starts a reload AND a loot search / an enemy grab. Before rifles
    //      the two-handed hold absorbed a mag-zone press; it no longer does (Task 5). COST: one bool or per poll per call.
    // Hand-open lids (spec 2026-11-18-hand-open-lids-design, trace 20261118_1711): a grip held on a lid / an item, or squeezed next
    // to one, is the grab, not L1 (the kick) / R1. lid_by_hand=0 -> both false at one bool test. BEFORE loot by hand (trace
    // 20261119_1240, Jay: "This is what I was concerned about having it tied to that system"): RUN LH6 lost 12 / 12 lid presses to
    // loot's limb-reading wait (its withheld grip read as "owned"). The lid decides first; a press it claims is "owned elsewhere"
    // for loot (no wait, NO-ARM), so loot can never delay or refuse a lid press. Holsters / two-hand / reload still own first.
    bool lidSuppressL1 = false, lidSuppressR1 = false;
    dlvr::LidHandPoll(g_vrGrip, holsterOwnsL || twoHandSuppressL1 || reloadSuppressL1, holsterOwnsR, lidSuppressL1, lidSuppressR1);
    bool lootSuppressL1 = false, lootSuppressR1 = false;
    dlvr::LootPoll(g_vrGrip, holsterOwnsL || twoHandSuppressL1 || reloadSuppressL1 || lidSuppressL1, holsterOwnsR || lidSuppressR1,
                   lootSuppressL1, lootSuppressR1);
    // Physical breakout (plan 2026-10-24-physical-breakout Task 3): the shove detector only - it withholds and injects nothing;
    // the game call happens on the game thread (HookTail -> BreakoutTick). breakout_physical=0 -> one bool test.
    dlvr::BreakoutPoll();
    // Right grip camouflage block (Jay 10-23 (3), option 1; always on): a right-grip press made while the game's pick is an infected
    // corpse is not sent as R1 (camouflage); buggy reset / traders keep R1.
    bool camoSuppressR1 = false;
    dlvr::GripCamoPoll(g_vrGrip, camoSuppressR1);
    // [LOADBEARING: enemy_push_grab] the open-hand grab (plan 2026-10-31-open-hand-push-and-grab Task 4): AFTER the holsters, the
    // two-handed hold and loot, which own a press first. A grip that starts a grab on a live enemy is the grab, not L1 (left, the
    // kick) / R1 (right, the combat grapple), until it opens. enemy_push_grab=0 -> both false at one bool test.
    bool pgSuppressL1 = false, pgSuppressR1 = false;
    // (+ reloadSuppressL1: the manual reload's press is never an enemy grab - the arbitration marker at LootPoll above. One bool or.)
    dlvr::PushGrabPoll(g_vrGrip, holsterOwnsL || twoHandSuppressL1 || lootSuppressL1 || reloadSuppressL1 || lidSuppressL1,
                       holsterOwnsR || lootSuppressR1 || camoSuppressR1 || lidSuppressR1,
                       pgSuppressL1, pgSuppressR1);
    // (Manual reload's ReloadPadPoll runs ABOVE, before TwoHandPoll - rifle plan Task 5.)
    // Hand climbing P1 (climbhand.h, docs/design_climbing_overhaul.md §6): LAST of the grip owners - a press any feature above took is
    // never a ledge grab; a press on the detected ledge (or any press while hanging) is the climb's, not L1 / R1.
    // climb_by_hand=0 and no climb held -> both false at one bool test.
    bool climbSuppressL1 = false, climbSuppressR1 = false;
    // The owner is NAMED (run 2: "presses another feature owned" 2-5 per 5 s and no way to tell which feature took them).
    const char* const climbOwnerL = holsterOwnsL ? "a holster" : twoHandSuppressL1 ? "the two-handed grip"
                                  : reloadSuppressL1 ? "the manual reload" : lidSuppressL1 ? "a lid / chest"
                                  : lootSuppressL1 ? "loot by hand" : pgSuppressL1 ? "the enemy grab" : nullptr;
    const char* const climbOwnerR = holsterOwnsR ? "a holster" : lidSuppressR1 ? "a lid / chest" : lootSuppressR1 ? "loot by hand"
                                  : camoSuppressR1 ? "the camouflage block" : pgSuppressR1 ? "the enemy grab" : nullptr;
    dlvr::ClimbHandPoll(gameMenu || dlvr::OverlayVisible() || dlvr::CalibScreenActive(), climbOwnerL, climbOwnerR,
                        climbSuppressL1, climbSuppressR1);
    // In an in-game menu the grips are plain L1 / R1 (game-menu pass-through, B) - a withhold that would have applied is counted.
    if (gameMenu && (holsterSuppressL1 || twoHandSuppressL1 || lootSuppressL1 || pgSuppressL1 || reloadSuppressL1 || lidSuppressL1 ||
                     climbSuppressL1)) {
        ++g_mp.supL1;
    }
    if (gameMenu && (holsterSuppressR1 || lootSuppressR1 || camoSuppressR1 || pgSuppressR1 || lidSuppressR1 || climbSuppressR1)) {
        ++g_mp.supR1;
    }
    if (g_vrGrip) {
        constexpr float kGripThreshold = 0.5f;
        auto* buttons = reinterpret_cast<uint32_t*>(p);
        float squeezeL = 0.0f, squeezeR = 0.0f;
        bool anyGripPressed = false;
        // (+ ReloadWithholdsL1: Jay after RUN XB1 - no L1 from the left grip while a manual crossbow is not loaded. One call.)
        if (dlvr::GetControllerSqueeze(dlvr::kLeftHand, squeezeL) && squeezeL >= kGripThreshold &&
            (gameMenu || (!holsterSuppressL1 && !twoHandSuppressL1 && !lootSuppressL1 && !pgSuppressL1 && !reloadSuppressL1 &&
                          !lidSuppressL1 && !climbSuppressL1 && !dlvr::ReloadWithholdsL1()))) {
            *buttons |= 0x400u;  // L1
            anyGripPressed = true;
        }
        if (dlvr::GetControllerSqueeze(dlvr::kRightHand, squeezeR) &&
            squeezeR >= kGripThreshold &&
            (gameMenu || (!holsterSuppressR1 && !lootSuppressR1 && !camoSuppressR1 && !pgSuppressR1 && !lidSuppressR1 &&
                          !climbSuppressR1))) {
            *buttons |= 0x800u;  // R1
            anyGripPressed = true;
        }
        static int gripLogCounter = 0;
        if (anyGripPressed && (++gripLogCounter % 15) == 0) {
            LOG_PERIODIC("vr_grip (scepad): squeezeL=%.2f squeezeR=%.2f buttons=0x%08X", squeezeL,
                squeezeR, *buttons);
        }
    }

    // Left trigger, 2026-09-03 - Sony's L2 analog byte (+0x08), the same
    // offset-and-scale-then-max() technique already confirmed correct for
    // R2/vr_trigger_fire below, just the other hand and the other trigger.
    if (g_vrLeftTrigger) {
        float trigL = 0.0f;
        if (dlvr::GetControllerTrigger(dlvr::kLeftHand, trigL)) {
            // vr_left_trigger_deadzone: at or below it = 0, above it rescaled so 1.0 still reads 255.
            const float dz = g_vrLeftTriggerDeadzone;
            if (dz > 0.0f) {
                trigL = trigL <= dz ? 0.0f : (trigL - dz) / (1.0f - dz);
            }
            const unsigned char synth = static_cast<unsigned char>(
                trigL <= 0.0f ? 0 : (trigL >= 1.0f ? 255 : trigL * 255.0f));
            unsigned char& l2 = p[0x08];
            const unsigned char before = l2;
            if (synth > l2) {
                l2 = synth;
            }
            static int leftTrigLogCounter = 0;
            if (synth > 0 && (++leftTrigLogCounter % 60) == 0) {
                LOG_PERIODIC("vr_left_trigger (scepad): pull=%.2f synth=%u l2 %u -> %u", trigL, synth,
                    before, l2);
            }
        }
    }

    if (g_vrTriggerFire) {
        float trig = 0.0f;
        if (dlvr::GetControllerTrigger(dlvr::kRightHand, trig)) {
            const unsigned char synth = static_cast<unsigned char>(
                trig <= 0.0f ? 0 : (trig >= 1.0f ? 255 : trig * 255.0f));
            unsigned char& r2 = p[0x09];
            const unsigned char before = r2;
            if (synth > r2) {
                r2 = synth;
            }
            // Diagnostic: confirms the write lands in the buffer
            // scePadReadState's caller actually reads. Throttled - this runs
            // every real poll while the trigger is pulled, and unlike
            // XInput's ~14 Hz, this path is polled every frame.
            static int logCounter = 0;
            if (synth > 0 && (++logCounter % 60) == 0) {
                LOG_PERIODIC("vr_trigger_fire (scepad): pull=%.2f synth=%u r2 %u -> %u", trig, synth,
                    before, r2);
            }
        }
    }
    // AFTER vr_trigger_fire on purpose: the byte is final here, whichever source set it. Before the heartbeat, so its
    // "injected buffer now" R2 shows what the game actually got.
    // Game-menu pass-through (A): the controller bytes as they stand here, before any trigger / button rule - restored below while
    // an in-game menu is up (MenuPassRestore). The rules still run, so their own state keeps up.
    unsigned char mpBefore[10];
    for (int i = 0; i < 10; ++i) { mpBefore[i] = p[i]; }
    // grapple_hand (climbhand.cpp P7 round 3): a right grappling hand trades the game's L2 / R2 while the hook is selected or a rope
    // holds - FIRST of the trigger rules, so every rule below sees the bytes as the game will (L2 = throw, R2 = attack). COST: one bool
    // test + two volatile loads.
    dlvr::GrapplePadSwap(p);
    if (g_meleeR2Suppress) {
        MeleeR2Filter(p);
    }
    // bow_manual_draw (SPEC-MANUAL-BOW-DRAW-2026-10-12): AFTER the melee rule, the byte is final here. With a bow held the game
    // reads R2 = 0 unless the string was grabbed (boneblend BowDrawPadByte). COST per poll: off = one bool test; on = a volatile
    // load and a compare.
    p[0x09] = dlvr::BowDrawPadByte(p[0x09]);
    // throwable_hand_enable (spec 2026-10-14-throwables-design, Build A; throwable.cpp): with a throwable selected the layer
    // owns L2 (holds 255 while readied, 0 otherwise) and zeroes R2 while one is out. AFTER the bow and melee rules, so its
    // bytes are final - and it MUST stay BEFORE the mod-menu block below: that block zeroes L2 last, and L2 dropping while
    // the game's throw state is 2 THROWS (INV-566). That is why the decision cancels on a blocking menu. COST per poll:
    // off = one GetTickCount64 + compare (the 1 Hz tick) and one bool test; on = see ThrowablePadByte's comment.
    dlvr::ThrowableTick();
    dlvr::ThrowablePadByte(p);
    // Loot by hand: Square (X) held while a hand searches a body - BEFORE the mod-menu block, which zeroes the pad last. COST: off =
    // one bool test; on = two state reads.
    dlvr::LootPadByte(p);
    // Body grab (plan 2026-10-23-body-grab Task 4): a hand holding a body sends no trigger - R2 for the right, L2 for the left. AFTER
    // every rule above (the byte is final), BEFORE the mod-menu block. L2 stays while the throwable decision holds it (INV-566: L2
    // dropping in throw state 2 THROWS) - throwable.cpp cancels that hold itself when a hand grabs. COST: two volatile loads.
    if (dlvr::BodyGrabHandActive(1)) {
        p[0x09] = 0;
    }
    // Manual reload (reload.cpp Task 5): the held pistol does not fire while a reload step is live (spec goal 8). Same place in the
    // chain as the body grab's R2 zero - AFTER every R2 rule, BEFORE the mod-menu block. COST: one bool test + two volatile reads.
    if (dlvr::ReloadWithholdsR2()) {
        p[0x09] = 0;
    }
    // [LOADBEARING: bolt wheel R2 withhold] (manual_reload) the crossbow does not fire while its bolt wheel is held / open (plan
    //      2026-11-04-crossbow-bolt-wheel, Jay: R2 "Hold off"; holster.cpp HolsterBoltWheelHoldsR2). Same place as the two above - AFTER
    //      every R2 rule, BEFORE the mod-menu block. COST: one volatile read.
    if (dlvr::HolsterBoltWheelHoldsR2()) {
        p[0x09] = 0;
    }
    // Carry (trace 20261121_1450, Jay WH1: "guns were still able to shoot and they shouldn't be able to" -> "Right hand only"): while
    // lidhand's RIGHT hand (side 0) carries an item - a lid container item or a world item, take to release - the game reads R2 = 0.
    // Same place as the three above. lid_by_hand=0 -> LidHandCarrying false. COST: one bool test + one volatile load.
    if (dlvr::LidHandCarrying(0)) {
        p[0x09] = 0;
    }
    // grapple_pull_hand (climbhand.cpp P7, design §8.21 / §8.23): while the grappling rope holds you and the pull trigger is not the
    // grappling one, that trigger lands on R2 and the game reads R2 = 0 (no attack / shot with each pull). Same place as the four
    // above. COST: one volatile load.
    if (dlvr::GrappleRopeHoldsR2()) {
        p[0x09] = 0;
    }
    if (dlvr::BodyGrabHandActive(0) && !dlvr::ThrowableHoldsL2()) {
        p[0x08] = 0;
    }

    // Game-menu pass-through (A): undo the trigger / button rules above while an in-game menu is up - BEFORE the mod-menu block,
    // which still neutralises everything when our own menu is open.
    if (gameMenu) {
        MenuPassRestore(p, mpBefore);
    }

    // MOD-MENU HOLD (Jay 2026-11-11: L3 + R3 held 2 s opens / closes the menu - overlay.cpp OverlayPoll): once the Touch L3 and
    // R3 are both down, NEITHER reaches the game until that click itself is let go - so the hold does not set off Survivor Sense
    // (R3 hold) or the L3 action, and letting go of one click first does not hand the other to the game. A click pressed ALONE
    // still passes the moment it is pressed: the game sees the first instant of whichever was pressed first (not delayed - the
    // L3 / R3 actions keep their timing). After every injection above, before the mod-menu block. COST: two cached-bool reads.
    {
        static bool s_holdL3 = false, s_holdR3 = false;
        bool l3 = false, r3 = false;
        dlvr::GetControllerStickClick(dlvr::kLeftHand, l3);
        dlvr::GetControllerStickClick(dlvr::kRightHand, r3);
        if (l3 && r3) { s_holdL3 = s_holdR3 = true; }
        if (!l3) { s_holdL3 = false; }
        if (!r3) { s_holdR3 = false; }
        auto* buttons = reinterpret_cast<uint32_t*>(p);
        if (s_holdL3) { *buttons &= ~0x2u; }
        if (s_holdR3) { *buttons &= ~0x4u; }
    }

    // Mod-menu input block, scePad path (dlvr_MenuInputPolicy above): LAST, after every injection, so neither a real
    // DualSense nor the Touch bytes written above reach the game while the menu is open. Neutral = no buttons, sticks
    // centred (128, the byte stickToByte(0.0f) gives), triggers 0. LT held (policy 3) passes the real right stick, as
    // dlvr_FilterGamepad does. Before the heartbeat, so its "injected buffer now" is what the game got.
    if (const int menuPolicy = dlvr_MenuInputPolicy(0)) {
        const unsigned char rsx = p[0x06], rsy = p[0x07];
        *reinterpret_cast<uint32_t*>(p) = 0;
        p[0x04] = p[0x05] = p[0x06] = p[0x07] = 128;
        p[0x08] = p[0x09] = 0;
        // Throwing by hand (ruling R13): while the throwable decision holds L2 (readied, or its cancel still waiting for
        // the game's state 0 to read back) L2 stays 255 under the menu. L2 = 0 while the throw state is 2 THROWS, at the
        // player's feet (INV-566); the decision cancels on this same menu and lets go of L2 only after state 0 reads back.
        // Also covers unticking throwable_hand_enable from the overlay mid-throw. Every other input stays blocked.
        if (dlvr::ThrowableHoldsL2()) {
            p[0x08] = 255;
        }
        if (menuPolicy == 3) {
            p[0x06] = rsx;
            p[0x07] = rsy;
        }
    }
    // Movement-confirmation heartbeat, 2026-09-03 - built for the public
    // test build (see CLAUDE.md's public-build section), where we cannot
    // read the tester's screen to confirm the character actually responds.
    // This cannot prove the character moved (that needs either eyes on the
    // screen or reading the game's own player-position memory, neither of
    // which this does) - what it CAN prove, unconditionally and from the log
    // alone, is every step up to that point: that this hook is even being
    // called at all on the tester's setup, what OpenXR is actually reporting
    // for each action (isActive - did the runtime resolve our suggested
    // binding to a real input on THIS controller at all), and that our
    // computed bytes really did land in the exact buffer scePadReadState's
    // caller reads (already confirmed to be the live path on this dev
    // machine - the open question for a new headset is whether the same is
    // true there). Fires on a fixed low cadence regardless of whether
    // anything is being pressed - unlike every other log line in this
    // function, a still, idle controller is exactly the case this needs to
    // be visible for, not throttled away.
    {
        static int heartbeat = 0;
        if ((++heartbeat % 180) == 0) {
            bool poseL = dlvr::ControllerPoseValid(dlvr::kLeftHand);
            bool poseR = dlvr::ControllerPoseValid(dlvr::kRightHand);
            float lx = 0, ly = 0, rx = 0, ry = 0;
            const bool stickLActive = dlvr::GetControllerStick(dlvr::kLeftHand, lx, ly);
            const bool stickRActive = dlvr::GetControllerStick(dlvr::kRightHand, rx, ry);
            float trigVal = 0;
            const bool trigActive = dlvr::GetControllerTrigger(dlvr::kRightHand, trigVal);
            bool btn[4] = {};
            bool btnActive[4] = {};
            const dlvr::FaceButton btnIds[4] = { dlvr::kButtonA, dlvr::kButtonB, dlvr::kButtonX,
                                                 dlvr::kButtonY };
            for (int i = 0; i < 4; ++i) {
                btnActive[i] = dlvr::GetControllerButton(btnIds[i], btn[i]);
            }
            LOG_PERIODIC("heartbeat (scepad): call #%d | real pad: buttons=0x%08X L=(%u,%u) "
                "R=(%u,%u) L2=%u R2=%u | our controllers: poseL=%d poseR=%d "
                "stickL(active=%d x=%.2f y=%.2f) stickR(active=%d x=%.2f y=%.2f) "
                "trigger(active=%d v=%.2f) A(active=%d,%d) B(active=%d,%d) "
                "X(active=%d,%d) Y(active=%d,%d) | injected buffer now: buttons=0x%08X "
                "L=(%u,%u) R=(%u,%u) L2=%u R2=%u",
                heartbeat, rawButtons, rawLX, rawLY, rawRX, rawRY, rawL2, rawR2, poseL, poseR,
                stickLActive, lx, ly, stickRActive, rx, ry, trigActive, trigVal, btnActive[0],
                btn[0], btnActive[1], btn[1], btnActive[2], btn[2], btnActive[3], btn[3],
                *reinterpret_cast<uint32_t*>(p), p[0x04], p[0x05], p[0x06], p[0x07], p[0x08],
                p[0x09]);
        }
    }
}

// Mouse-look policy, same bootstrap-owns-mechanics/layer-owns-policy split as
// dlvr_FilterGamepad/dlvr_FilterScePad above - the bootstrap's IAT hook on
// engine_x64_rwdi.dll's import of SDL2.dll!SDL_PollEvent calls this after a
// real event comes back, and this decides what (if anything) to change.
//
// Why this exists: horizon lock (camhook.cpp) only re-levels the RENDERED
// camera every frame - it does not touch whatever internal state the engine
// itself uses to accumulate mouse-driven look. That state keeps drifting
// invisibly regardless of horizon lock, and it turned out to still feed the
// aim-redirect hook's `original` direction (see aimhook.cpp) - moving the
// mouse vertically shifted that hidden baseline away from level, showing up
// as an unexplained redirect offset that only went away once the mouse
// happened to be back at whatever position corresponds to "level" for that
// hidden state (confirmed in practice 2026-09-02). Zeroing the mouse's
// vertical motion at the source, the same way block_pad_pitch already does
// for the gamepad's right stick, stops the hidden state from ever drifting
// in the first place.
//
// SDL_Event is a tagged union; every variant starts with a 4-byte `type`
// field, so reading just that much from the raw pointer to identify
// SDL_MOUSEMOTION (0x400) is safe regardless of which variant the union
// actually holds. The SDL_MouseMotionEvent layout below matches SDL2's own
// public header (Uint32 type/timestamp/windowID/which, Uint8 state +3 bytes
// padding, Sint32 x/y/xrel/yrel) - inferred from the public ABI, not
// independently confirmed against this build's own SDL2.dll the way
// ScePadData's layout was, hence block_mouse_pitch defaulting off until
// tested.
extern "C" __declspec(dllexport) void dlvr_FilterSdlEvent(void* event)
{
    if (!g_blockMousePitch || !event) {
        return;
    }
    struct SdlMouseMotionEvent {
        uint32_t type;
        uint32_t timestamp;
        uint32_t windowID;
        uint32_t which;
        uint8_t  state;
        uint8_t  padding1, padding2, padding3;
        int32_t  x, y, xrel, yrel;
    };
    constexpr uint32_t kSdlMouseMotion = 0x400;
    const uint32_t type = *static_cast<const uint32_t*>(event);
    if (type != kSdlMouseMotion) {
        return;
    }
    auto* m = static_cast<SdlMouseMotionEvent*>(event);
    if (m->yrel != 0) {
        static int logCounter = 0;
        if ((++logCounter % 60) == 0) {
            LOG("block_mouse_pitch: zeroed yrel=%d (xrel=%d passed through)", m->yrel, m->xrel);
        }
        m->yrel = 0;
    }
}

// ============================================================================
//  Property store - the game reads a few of these; defaults are fine.
// ============================================================================

OVR_PUBLIC_FUNCTION(ovrBool) ovr_GetBool(ovrSession, const char*, ovrBool defaultVal)
{
    return defaultVal;
}
OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetBool(ovrSession, const char*, ovrBool) { return ovrTrue; }

OVR_PUBLIC_FUNCTION(int) ovr_GetInt(ovrSession, const char*, int defaultVal)
{
    return defaultVal;
}
OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetInt(ovrSession, const char*, int) { return ovrTrue; }

OVR_PUBLIC_FUNCTION(float) ovr_GetFloat(ovrSession session, const char* name, float defaultVal)
{
    // "IPD" is the well-known LibOVR key; report the runtime's measured value.
    if (name && _stricmp(name, "IPD") == 0) {
        Session* s = S(session);
        return s ? s->ipd : defaultVal;
    }
    return defaultVal;
}
OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetFloat(ovrSession, const char*, float) { return ovrTrue; }

OVR_PUBLIC_FUNCTION(unsigned int)
ovr_GetFloatArray(ovrSession, const char*, float*, unsigned int)
{
    return 0;
}
OVR_PUBLIC_FUNCTION(ovrBool)
ovr_SetFloatArray(ovrSession, const char*, const float[], unsigned int)
{
    return ovrTrue;
}

OVR_PUBLIC_FUNCTION(const char*)
ovr_GetString(ovrSession, const char*, const char* defaultVal)
{
    return defaultVal;
}
OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetString(ovrSession, const char*, const char*)
{
    return ovrTrue;
}

// ============================================================================
//  Input - not wired to OpenXR actions yet. Report "no controllers" honestly
//  rather than inventing poses; the game stays playable on pad.
// ============================================================================

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetInputState(ovrSession, ovrControllerType, ovrInputState* inputState)
{
    if (inputState) {
        memset(inputState, 0, sizeof(*inputState));
    }
    return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(unsigned int) ovr_GetConnectedControllerTypes(ovrSession)
{
    return 0;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_SetControllerVibration(ovrSession, ovrControllerType, float, float)
{
    return ovrSuccess;
}

// ============================================================================
//  Trackers - OpenXR has no base-station concept; report none.
// ============================================================================

OVR_PUBLIC_FUNCTION(unsigned int) ovr_GetTrackerCount(ovrSession) { return 0; }

OVR_PUBLIC_FUNCTION(ovrTrackerDesc) ovr_GetTrackerDesc(ovrSession, unsigned int)
{
    ovrTrackerDesc d{};
    return d;
}

OVR_PUBLIC_FUNCTION(ovrTrackerPose) ovr_GetTrackerPose(ovrSession, unsigned int)
{
    ovrTrackerPose p{};
    p.Pose = IdentityPose();
    p.LeveledPose = IdentityPose();
    return p;
}

// ============================================================================
//  OpenGL variants - the renderer is D3D11; refuse cleanly.
// ============================================================================

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_CreateTextureSwapChainGL(ovrSession, const ovrTextureSwapChainDesc*, ovrTextureSwapChain*)
{
    return ovrError_Unsupported;
}
OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetTextureSwapChainBufferGL(ovrSession, ovrTextureSwapChain, int, unsigned int*)
{
    return ovrError_Unsupported;
}
OVR_PUBLIC_FUNCTION(ovrResult)
ovr_CreateMirrorTextureGL(ovrSession, const ovrMirrorTextureDesc*, ovrMirrorTexture*)
{
    return ovrError_Unsupported;
}
OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetMirrorTextureBufferGL(ovrSession, ovrMirrorTexture, unsigned int*)
{
    return ovrError_Unsupported;
}

// ============================================================================
//  Audio device queries - let the game fall back to the default device.
// ============================================================================

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetAudioDeviceOutWaveId(unsigned int*)
{
    return ovrError_AudioDeviceNotFound;
}
OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetAudioDeviceInWaveId(unsigned int*)
{
    return ovrError_AudioDeviceNotFound;
}
OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetAudioDeviceOutGuidStr(WCHAR*)
{
    return ovrError_AudioDeviceNotFound;
}
OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetAudioDeviceOutGuid(GUID*)
{
    return ovrError_AudioDeviceNotFound;
}
OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetAudioDeviceInGuidStr(WCHAR*)
{
    return ovrError_AudioDeviceNotFound;
}
OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetAudioDeviceInGuid(GUID*)
{
    return ovrError_AudioDeviceNotFound;
}

// ============================================================================
//  Private / undocumented entry points.
//
//  None of these are declared in OVR_CAPI.h. The game's CAPI shim resolves its
//  whole function table by name, so they have to exist as exports even though
//  nothing here is likely to call them.
//
//  ovr_Lookup and ovr_ReportClientInfo DO exist in the official Meta runtime,
//  so we provide them. ovr_InitializeRenderingShimVersion and
//  ovr_SetQueueAheadFraction do NOT exist even there - they are SDK 0.x
//  leftovers - which is exactly why the shim tolerates them being absent. We
//  export them anyway; a resolved no-op is safer than a null pointer.
// ============================================================================

extern "C" {

// Looks up an internal interface by name. We have none to offer.
__declspec(dllexport) ovrResult ovr_Lookup(const char* name, void** data)
{
    LOG("ovr_Lookup(\"%s\") -> unsupported", name ? name : "");
    if (data) {
        *data = nullptr;
    }
    return ovrError_Unsupported;
}

// Telemetry. Declared with no parameters on purpose: the real arity is unknown,
// and on x64 a callee that ignores its arguments is safe whatever is passed.
__declspec(dllexport) void ovr_ReportClientInfo(void)
{
}

__declspec(dllexport) ovrResult ovr_InitializeRenderingShimVersion(void)
{
    return ovrSuccess;
}

__declspec(dllexport) ovrResult ovr_SetQueueAheadFraction(void)
{
    return ovrSuccess;
}

}  // extern "C"
