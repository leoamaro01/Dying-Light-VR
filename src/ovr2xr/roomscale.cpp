// Roomscale - Task 7a (the log-only probe) and 7b (the walk body). See roomscale.h.
//
// WHAT IT READS (FINDINGS-ROOMSCALE-STATIC-2026-10-27, OUR engine e9fbfc2c / gamedll 97046bba, Steam rvas - the DEV tree has no
// buildrva table, so any other build REFUSES by name at the vtable / prologue check):
//   PlayerDI = *(*(*(GameDI + 1344) + 184) + 88), GameDI = qword gamedll+0x1C16348 (the CScriptCondition chain, rs4);
//   its control object = PlayerDI + 0x18; IControlObject::PhysicsBind (engine export) -> CPhysicsBind vfunc +0xB0 = the walk
//   interface; full object = interface - 0x700, its vtable must be CODEPhysicsWalkFly's engine +86DC38 (rs1/rs2);
//   wanted velocity full+0xA70, additional velocity +0xA94, move-by-position +0x7E8, lock +0x850, physics off +0x2B1, walk state
//   (u16) +0x9C0; the main body *(full+0x710) (iface slot 88 = [iface+0x10]) and its position body+0xBC; the controller group
//   PlayerDI+3324 (S5); the control object's world position (IControlObject::GetWorldPosition).
// 7b WRITES one thing: the player walk body's wanted velocity, inside the engine's own pre-step (+3D6F50, full vtable slot 9,
// (full, dt in xmm1)), as the game's value + roomscale's extra on x / z (Mamfro's merge rule: a value other than the one we last
// wrote is the game's new one). The engine's own collision then stops the body at walls.
// THE SHARE (plan amendment 2026-10-27, ledgered): the view is shifted only by what ROOMSCALE moved, never by stick walking. Each
// pre-step measures the step before it: body delta - the game's wanted velocity x that step's dt, projected on that step's extra
// and clamped to [0, |extra| x dt]. Mamfro's public code measures each step at the next one the same way; its roomscale crate
// (monaka_vr/crates/body) is not public, so the share rule is ours.
#include "roomscale.h"

#include "boneblend.h"   // InstallSingleGameDetour - the shared verify / suspend / commit install
#include "camhook.h"
#include "headpose.h"
#include "periodic_log.h"     // LOG_PERIODIC - the [RSCAM] 5 s window (diag_periodic_logs)
#include "roomscale_logic.h"  // ShareAlong / IntraframeTime / MatchReported - fix 1 (trace 20261117_1316_rscam_fix1)

#include <windows.h>
#include <intrin.h>           // _ReturnAddress - fix 1's caller check

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

// The Epic (EGL) build: the GameDI global and the walk vtable are on tools/egl_allowlist.tsv (family 10) and translated here; the
// pre-step detour address is handed over as the Steam rva - InstallSingleGameDetour translates it itself (buildrva::Abs), so it must
// not be translated twice. The DEV tree has no buildrva.h: Steam rvas only (any other build refuses by name at the vtable check).
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_RS_GD(rva) ::dlvr::buildrva::Gd(rva)
#define DLVR_RS_EN(rva) ::dlvr::buildrva::Eng(rva)
#else
#define DLVR_RS_GD(rva) (rva)
#define DLVR_RS_EN(rva) (rva)
#endif

namespace dlvr {

// [DEP: diag_roomscale_probe] required-for=[ROOMSCALE] - Task 7a's log-only probe of the walk body, its guards, the controller group
//      and the app -> world map (plan 2026-10-27-roomscale-physical-crouch, amended). tree=INERT (log-only, writes nothing).
//      HEADSET for the map (a tracked head + the rendered camera); the walk-body / group reads are flat-valid structurally. DEFAULT OFF.
bool DiagRoomscaleProbe = false;
// [DEP: roomscale_enable] required-for=roomscale (RoomscaleEnable) - real steps move the character through the walk body's own
//      physics (plan Task 7b; spec §2.2, Jay: "Add roomscale in", "New key for both"). tree=LOADBEARING while 1 (the pre-step detour
//      writes the player's wanted velocity; ovr2xr.cpp shifts appSpace by roomscale's measured share). 0 = the detour, if installed,
//      passes every step straight through and the drift bleed (roomscale_bleed_mps) runs instead. HEADSET. DEFAULT ON 2026-10-27 -
//      Tier A (Jay verified: run 4 "View stayed on my body, stick walking felt normal"; ticked "Roomscale" at session end).
bool  RoomscaleEnable = true;
// [DEP: roomscale_lean_max_m] required-for=how far the view may lean past a body that cannot follow (a wall, a stand-down state)
//      before the rest is absorbed into the origin (RoomscaleLeanMaxM; Jay: "That's correct" to the wall rule). tree=INERT at
//      roomscale_enable=0. Tier D.
float RoomscaleLeanMaxM = 0.25f;

namespace {

constexpr uintptr_t kGameDiRva = 0x1C16348;    // gamedll qword_181C16348 (GameDI)
constexpr uintptr_t kWalkFullVtRva = 0x86DC38;  // engine CODEPhysicsWalkFly full-object vtable (RTTI, rs1)
constexpr uintptr_t kPreStepRva = 0x3D6F50;     // engine CODEPhysicsWalkFly pre-step (full vtable slot 9) - rs1 / rs2
constexpr unsigned char kPreStepBytes[18] = {   // movss [rsp+10h], xmm1 / push rbp rsi rdi r12 r14 / lea rbp, [rsp-70h]
    0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0x90 };   // no RIP-relative
constexpr const char* kPhysicsBindExport = "?PhysicsBind@IControlObject@@QEAAPEAVIPhysicsBind@@XZ";
constexpr const char* kWorldPosExport = "?GetWorldPosition@IControlObject@@QEBA?AVvec3@@XZ";

using PhysicsBindFn = void*(__fastcall*)(void* control);
using GetterFn = void*(__fastcall*)(void* self);
using WorldPosFn = float*(__fastcall*)(void* control, float* out);
using PreStepFn = void(__fastcall*)(void* full, float dt);

int        g_state = 0;   // 0 not tried, 1 ready, -1 refused (named once)
uintptr_t  g_gd = 0, g_en = 0;
PhysicsBindFn g_bind = nullptr;
WorldPosFn g_worldPos = nullptr;
ULONGLONG  g_lastMs = 0;
long       g_lines = 0;
constexpr long kMaxLines = 2400;   // INV-9: 2 a second -> 20 minutes; the cap guards a forgotten switch
bool       g_vtSaid = false;
uintptr_t  g_lastVt = 0;
bool       g_havePrev = false;
float      g_prevHead[2] = {}, g_prevCam[2] = {};

// 7b shared state. g_playerFull / g_group: written by the game thread, read by the detour and the render thread.
volatile uintptr_t g_playerFull = 0;
volatile LONG      g_group = -99;
PreStepFn          g_origPreStep = nullptr;
volatile LONG      g_hook = 0;          // 0 not tried, 1 live, -1 refused
SRWLOCK            g_lock = SRWLOCK_INIT;
float              g_cmd[2] = {};       // render -> detour: world x/z velocity
float              g_moved[2] = {};     // detour -> render: world x/z roomscale share, accumulated
long               g_steps = 0, g_commanded = 0, g_guarded = 0, g_gameChanged = 0;
// detour-only (one writer: the physics step of the player's body)
bool               g_stepPrev = false;
float              g_prevPos[2] = {}, g_prevGame[2] = {}, g_prevExtra[2] = {}, g_prevDt = 0.0f;
float              g_gameWanted[3] = {}, g_written[3] = { NAN, NAN, NAN };
bool               g_wroteLast = false;

// ===== FIX 1 - THE ROOMSCALE CAMERA IN STEP (spec docs/superpowers/specs/2026-11-17-roomscale-camera-in-step-design.md §3 / §3a;
// trace docs/traces/20261117_1316_rscam_fix1.md; Jay: "We need 100% certainty here" -> "Yes"). INV-306 addenda 5-7: the camera
// rides the body as the engine writes it (extrapolated, after this frame's physics) plus the head C08100 takes from COculusHelper
// slot 4; the appSpace shift comes from the raw body one step late on the submit thread. RsHeadGetter hands C08100 the head minus
// the residual: roomscale's share of the body move NOW less what the shift had taken when that head was reported.
constexpr uintptr_t kHelperSlot4Rva  = 0x863398;   // engine .rdata: COculusHelper vtable slot 4 (the only data ref to +2E6BB0, rc1)
constexpr uintptr_t kHeadGetterRva   = 0x2E6BB0;   // engine: the head-position getter (49 bytes; copies helper+0x218 + 12 x slot)
constexpr uintptr_t kC08100CallRva   = 0xC0818E;   // gamedll: C08100's `call [r8+20h]` (41 FF 50 20)
constexpr uintptr_t kC08100RetRva    = 0xC08192;   // gamedll: the instruction after it = the return address the check wants
constexpr unsigned char kC08100CallBytes[4] = { 0x41, 0xFF, 0x50, 0x20 };
constexpr uintptr_t kFullObjOffX = 0x720, kFullObjOffZ = 0x728;   // walk full object: body -> object offset (+3E08C0 :v14 / :v15)
constexpr float     kMaxResidualM = 0.5f;          // a larger residual is not believed (a teleport / respawn) - refused, counted
using HeadGetterFn = float*(__fastcall*)(void* helper, float* out);
using V2LFn        = void*(__fastcall*)(const void* ctrl, float* ret, const float* in);   // IControlObject::VectorWorldToLocal
constexpr const char* kV2LExport = "?VectorWorldToLocal@IControlObject@@QEBA?AVvec3@@AEBV2@@Z";
HeadGetterFn       g_origGetter = nullptr;
V2LFn              g_v2l = nullptr;
volatile LONG      g_getterHook = 0;      // 0 not tried, 1 live, -1 refused (named once)
volatile uintptr_t g_playerDI = 0;        // RoomscaleProbeTick publishes it beside g_playerFull
uintptr_t          g_c08100Ret = 0;
// shared under g_lock: every share the pre-step measured, every share RoomscaleTakeMoved handed out (world x/z), and the last
// pre-step's parameters (the step that ran after it is not measured until the next pre-step)
float              g_shareTotal[2] = {}, g_takenTotal[2] = {};
float              g_pubPos[2] = {}, g_pubGame[2] = {}, g_pubExtra[2] = {}, g_pubDt = 0.0f;
bool               g_pubValid = false;
constexpr int      kRing = 16;
float              g_ringPos[kRing][3] = {};
float              g_ringTaken[kRing][2] = {};
int                g_ringN = 0, g_ringHead = 0;
// the getter's own counters (simulation thread, read by the 5 s window)
volatile LONG64    g_rcCalls = 0, g_rcOther = 0, g_rcOff = 0, g_rcNoPlayer = 0, g_rcUnpaired = 0, g_rcRefused = 0, g_rcFault = 0,
                   g_rcApplied = 0;
double             g_rcSumMm = 0.0;
float              g_rcMaxMm = 0.0f;
volatile LONG      g_rcSaidOther = 0, g_rcSaidFirst = 0, g_rcSaidRefused = 0;

bool RdQ(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdF(uintptr_t a, float* out)
{
    __try { *out = *reinterpret_cast<const float*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdB(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const unsigned char*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdW(uintptr_t a, unsigned short* out)
{
    __try { *out = *reinterpret_cast<const unsigned short*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RdI(uintptr_t a, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WrF3(uintptr_t a, const float v[3])
{
    __try { std::memcpy(reinterpret_cast<void*>(a), v, 12); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* CallBind(void* control)
{
    __try { return g_bind(control); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* CallGetter(uintptr_t fn, void* self)
{
    __try { return reinterpret_cast<GetterFn>(fn)(self); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CallWorldPos(void* control, float out[3])
{
    __try { g_worldPos(control, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Resolve()
{
    if (g_state != 0) { return g_state == 1; }
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !en) { return false; }   // not mapped yet: tried again next update (no line - the ARMED / REFUSED line follows)
    g_gd = reinterpret_cast<uintptr_t>(gd);
    g_en = reinterpret_cast<uintptr_t>(en);
    g_bind = reinterpret_cast<PhysicsBindFn>(GetProcAddress(en, kPhysicsBindExport));
    g_worldPos = reinterpret_cast<WorldPosFn>(GetProcAddress(en, kWorldPosExport));
    g_v2l = reinterpret_cast<V2LFn>(GetProcAddress(en, kV2LExport));   // fix 1 only - null = fix 1 refuses by name, roomscale unaffected
    g_c08100Ret = g_gd + kC08100RetRva;
    if (!g_bind || !g_worldPos) {
        g_state = -1;
        // [PROBE: ROOMSCALE] gate=diag_roomscale_probe,roomscale_enable  site=FLAT  log-only=yes
        LOG("[ROOMSCALE] REFUSED - the engine does not export %s%s - roomscale and its probe stay off this launch",
            g_bind ? "" : kPhysicsBindExport, g_worldPos ? "" : kWorldPosExport);
        return false;
    }
    g_state = 1;
    // [PROBE: ROOMSCALE] gate=diag_roomscale_probe,roomscale_enable  site=FLAT  log-only=yes
    LOG("[ROOMSCALE] ARMED (logged unconditionally) - roomscale_enable %d, probe %d | PhysicsBind %p, GetWorldPosition %p | GameDI "
        "gamedll+%llX, walk full vtable expected engine+%llX (Steam), pre-step engine+%llX",
        RoomscaleEnable ? 1 : 0, DiagRoomscaleProbe ? 1 : 0, reinterpret_cast<void*>(g_bind), reinterpret_cast<void*>(g_worldPos),
        static_cast<unsigned long long>(kGameDiRva), static_cast<unsigned long long>(kWalkFullVtRva),
        static_cast<unsigned long long>(kPreStepRva));
    return true;
}

// The player's walk body, resolved from scratch (no cached entity - U-50). Returns the full object or 0 with *why.
uintptr_t ResolveWalk(uintptr_t* playerOut, int* groupOut, uintptr_t* vtOut, const char** why)
{
    *playerOut = 0;
    *vtOut = 0;
    *why = "no player";
    uintptr_t gdi = 0, a = 0, b = 0, player = 0;
    if (!(RdQ(g_gd + DLVR_RS_GD(kGameDiRva), &gdi) && gdi && RdQ(gdi + 1344, &a) && a && RdQ(a + 184, &b) && b && RdQ(b + 88, &player) &&
          player)) {
        return 0;
    }
    *playerOut = player;
    RdI(player + 3324, groupOut);
    void* bind = CallBind(reinterpret_cast<void*>(player + 0x18));
    uintptr_t bvt = 0, getter = 0;
    if (!bind) { *why = "PhysicsBind returned null"; return 0; }
    if (!RdQ(reinterpret_cast<uintptr_t>(bind), &bvt) || !RdQ(bvt + 0xB0, &getter) || !getter) {
        *why = "the bind's walk getter (vfunc +0xB0) is unreadable";
        return 0;
    }
    void* iface = CallGetter(getter, bind);
    if (!iface) { *why = "the bind has no walk physics (getter returned null)"; return 0; }
    const uintptr_t full = reinterpret_cast<uintptr_t>(iface) - 0x700;
    uintptr_t vt = 0;
    if (!RdQ(full, &vt)) { *why = "the walk object is unreadable"; return 0; }
    *vtOut = vt;
    if (vt - g_en != DLVR_RS_EN(kWalkFullVtRva)) {
        *why = "the walk object's vtable is not CODEPhysicsWalkFly's (Steam +86DC38 / its Epic translation) - REFUSED";
        return 0;
    }
    *why = "ok";
    return full;
}

// The pre-step detour: every CODEPhysicsWalkFly body steps through here (NPCs too) - anything but the player's body is one compare
// and the original. COST for the player's body: ~12 guarded reads, one 12-byte write, one SRW lock, a dozen flops per physics step.
void __fastcall PreStepDetour(void* full, float dt)
{
    const uintptr_t me = reinterpret_cast<uintptr_t>(full);
    if (me == 0 || me != g_playerFull) {
        g_origPreStep(full, dt);
        return;
    }
    uintptr_t body = 0;
    float pos[2] = { NAN, NAN };
    if (RdQ(me + 0x710, &body) && body) { RdF(body + 0xBC, &pos[0]); RdF(body + 0xC4, &pos[1]); }
    float wanted[3] = { NAN, NAN, NAN };
    for (int i = 0; i < 3; ++i) { RdF(me + 0xA70 + 4 * i, &wanted[i]); }
    unsigned char mbp = 1, lock = 1, off = 1;
    RdB(me + 0x7E8, &mbp);
    RdB(me + 0x850, &lock);
    RdB(me + 0x2B1, &off);
    const bool guarded = mbp || lock || off;

    // The share of the LAST step roomscale caused: body delta - the game's own wanted x dt, along the extra, clamped.
    float shareX = 0.0f, shareZ = 0.0f;
    if (g_stepPrev && std::isfinite(pos[0]) && std::isfinite(pos[1])) {
        const float ex = g_prevExtra[0], ez = g_prevExtra[1];
        const float elen = std::sqrt(ex * ex + ez * ez);
        if (elen > 1e-4f && g_prevDt > 0.0f) {
            const float rx = (pos[0] - g_prevPos[0]) - g_prevGame[0] * g_prevDt;
            const float rz = (pos[1] - g_prevPos[1]) - g_prevGame[1] * g_prevDt;
            float along = (rx * ex + rz * ez) / elen;
            const float maxAlong = elen * g_prevDt;
            along = along < 0.0f ? 0.0f : (along > maxAlong ? maxAlong : along);
            shareX = ex / elen * along;
            shareZ = ez / elen * along;
        }
    }

    // The game's own wanted velocity: a value other than the one we wrote last is the game's new one.
    bool changed = false;
    if (std::isfinite(wanted[0]) && std::isfinite(wanted[2])) {
        const bool ours = g_wroteLast && wanted[0] == g_written[0] && wanted[1] == g_written[1] && wanted[2] == g_written[2];
        if (!ours) {
            changed = g_wroteLast;
            g_gameWanted[0] = wanted[0]; g_gameWanted[1] = wanted[1]; g_gameWanted[2] = wanted[2];
        }
    }
    float cmd[2] = {};
    AcquireSRWLockExclusive(&g_lock);
    g_moved[0] += shareX;
    g_moved[1] += shareZ;
    g_shareTotal[0] += shareX;   // fix 1: every measured share (RsHeadGetter's share_now)
    g_shareTotal[1] += shareZ;
    if (RoomscaleEnable && !guarded) { cmd[0] = g_cmd[0]; cmd[1] = g_cmd[1]; }
    ++g_steps;
    if (guarded) { ++g_guarded; }
    if (cmd[0] != 0.0f || cmd[1] != 0.0f) { ++g_commanded; }
    if (changed) { ++g_gameChanged; }
    ReleaseSRWLockExclusive(&g_lock);

    // Write game + extra (x, z; y the game's) - or give the game its own value back when there is nothing to add.
    g_wroteLast = false;
    if (std::isfinite(g_gameWanted[0]) && (cmd[0] != 0.0f || cmd[1] != 0.0f || wanted[0] != g_gameWanted[0] ||
                                           wanted[2] != g_gameWanted[2])) {
        const float w[3] = { g_gameWanted[0] + cmd[0], g_gameWanted[1], g_gameWanted[2] + cmd[1] };
        if (WrF3(me + 0xA70, w)) {
            g_written[0] = w[0]; g_written[1] = w[1]; g_written[2] = w[2];
            g_wroteLast = cmd[0] != 0.0f || cmd[1] != 0.0f;
            if (!g_wroteLast) { g_written[0] = g_written[1] = g_written[2] = NAN; }
        }
    }
    g_stepPrev = std::isfinite(pos[0]) && std::isfinite(pos[1]);
    g_prevPos[0] = pos[0]; g_prevPos[1] = pos[1];
    g_prevGame[0] = g_gameWanted[0]; g_prevGame[1] = g_gameWanted[2];
    g_prevExtra[0] = cmd[0]; g_prevExtra[1] = cmd[1];
    g_prevDt = dt;
    // fix 1: the step about to run is measured only at the NEXT pre-step - publish its parameters so RsHeadGetter can measure it now
    AcquireSRWLockExclusive(&g_lock);
    g_pubValid = g_stepPrev;
    g_pubPos[0] = g_prevPos[0]; g_pubPos[1] = g_prevPos[1];
    g_pubGame[0] = g_prevGame[0]; g_pubGame[1] = g_prevGame[1];
    g_pubExtra[0] = g_prevExtra[0]; g_pubExtra[1] = g_prevExtra[1];
    g_pubDt = g_prevDt;
    ReleaseSRWLockExclusive(&g_lock);
    g_origPreStep(full, dt);
}

void InstallOnce()
{
    if (g_hook != 0) { return; }
    int attempts = 0;
    void* tramp = nullptr;
    const bool ok = InstallSingleGameDetour(g_en + kPreStepRva, kPreStepBytes, sizeof(kPreStepBytes),
                                            reinterpret_cast<void*>(&PreStepDetour), &tramp, "roomscale walk pre-step", &attempts);
    if (ok) {
        g_origPreStep = reinterpret_cast<PreStepFn>(tramp);
        InterlockedExchange(&g_hook, 1);
    } else {
        InterlockedExchange(&g_hook, -1);
    }
    // [PROBE: ROOMSCALE] gate=roomscale_enable  site=FLAT  log-only=no
    LOG("[ROOMSCALE] pre-step detour (engine+%llX, %zu bytes verified) %s after %d attempt(s)%s",
        static_cast<unsigned long long>(kPreStepRva), sizeof(kPreStepBytes), ok ? "LIVE" : "REFUSED", attempts,
        ok ? "" : " - its bytes differ from engine e9fbfc2c's, or the write was refused; roomscale does nothing this launch");
}

// ===== FIX 1: the head C08100 takes, corrected by the residual (see the block above g_origGetter). Simulation thread, once per player
// update (PlayerVis::OnPostUpdate +C5FD10 -> slot 342 C08100), AFTER this frame's physics + mesh synchro (INV-306 addendum 7).
// COST per call: the original (3 loads), one _ReturnAddress compare; on C08100's call while roomscale is live: one SRW lock, a
// 16-entry float compare, ~8 guarded reads, two engine calls (GetWorldPosition, VectorWorldToLocal), a few dozen flops.
bool CallV2L(void* ctrl, const float in[3], float out[3])
{
    __try { g_v2l(ctrl, out, in); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

float* __fastcall RsHeadGetter(void* helper, float* out)
{
    float* const r = g_origGetter(helper, out);
    InterlockedIncrement64(&g_rcCalls);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_c08100Ret) {
        InterlockedIncrement64(&g_rcOther);
        if (!InterlockedExchange(&g_rcSaidOther, 1)) {
            // [PROBE: RSCAM] gate=roomscale_enable  site=HEADSET  log-only=yes
            LOG("[RSCAM] *** ANOTHER CALLER of the head getter (COculusHelper slot 4) *** return address %p (gamedll+%llX if in gamedll) - "
                "it gets the ORIGINAL value, never the correction; counted per window as 'other'",
                _ReturnAddress(), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_gd));
        }
        return r;
    }
    if (!RoomscaleEnable || g_hook != 1) { InterlockedIncrement64(&g_rcOff); return r; }
    const uintptr_t player = g_playerDI, full = g_playerFull;
    if (!player || !full || !g_v2l || !g_worldPos) { InterlockedIncrement64(&g_rcNoPlayer); return r; }

    // the shared totals + the reported head this store came from (§3a A4: exact match of (-x, y, -z))
    float share[2], taken[2] = { 0.0f, 0.0f }, pPos[2], pGame[2], pExtra[2], pDt;
    bool pValid, paired;
    AcquireSRWLockShared(&g_lock);
    share[0] = g_shareTotal[0]; share[1] = g_shareTotal[1];
    pValid = g_pubValid;
    pPos[0] = g_pubPos[0]; pPos[1] = g_pubPos[1];
    pGame[0] = g_pubGame[0]; pGame[1] = g_pubGame[1];
    pExtra[0] = g_pubExtra[0]; pExtra[1] = g_pubExtra[1];
    pDt = g_pubDt;
    const int idx = ::dlvr::roomscale::MatchReported(g_ringPos, g_ringN, out[0], out[1], out[2]);
    paired = idx >= 0;
    if (paired) { taken[0] = g_ringTaken[idx][0]; taken[1] = g_ringTaken[idx][1]; }
    ReleaseSRWLockShared(&g_lock);
    if (!paired) { InterlockedIncrement64(&g_rcUnpaired); return r; }

    // the body NOW: raw ODE position + velocity, and the object position the engine wrote (+3E08C0: object = intraframe - offset)
    uintptr_t body = 0;
    float raw[2] = { NAN, NAN }, vel[2] = { NAN, NAN }, off[2] = { NAN, NAN }, obj[3] = { NAN, NAN, NAN };
    const bool readOk = RdQ(full + 0x710, &body) && body && RdF(body + 0xBC, &raw[0]) && RdF(body + 0xC4, &raw[1]) &&
                        RdF(body + 0xD8, &vel[0]) && RdF(body + 0xE0, &vel[1]) && RdF(full + kFullObjOffX, &off[0]) &&
                        RdF(full + kFullObjOffZ, &off[1]) && CallWorldPos(reinterpret_cast<void*>(player + 0x18), obj);
    if (!readOk || !std::isfinite(raw[0]) || !std::isfinite(raw[1]) || !std::isfinite(obj[0]) || !std::isfinite(obj[2])) {
        InterlockedIncrement64(&g_rcFault);
        return r;
    }
    // the last step (run after the last pre-step, not measured yet) - the pre-step's own formula
    float lastX = 0.0f, lastZ = 0.0f;
    if (pValid) {
        ::dlvr::roomscale::ShareAlong(raw[0] - pPos[0], raw[1] - pPos[1], pGame[0], pGame[1], pDt, pExtra[0], pExtra[1], lastX, lastZ);
    }
    // the intraframe part the engine extrapolated (intraframe = object + offset; vt = intraframe - raw = vel x t)
    const float vtX = (obj[0] + off[0]) - raw[0], vtZ = (obj[2] + off[1]) - raw[1];
    const float t = ::dlvr::roomscale::IntraframeTime(vtX, vtZ, vel[0], vel[1]);
    float intraX = 0.0f, intraZ = 0.0f;
    ::dlvr::roomscale::ShareAlong(vtX, vtZ, pGame[0], pGame[1], t, pExtra[0], pExtra[1], intraX, intraZ);
    const float res[3] = { share[0] + lastX + intraX - taken[0], 0.0f, share[1] + lastZ + intraZ - taken[1] };
    const float resM = std::sqrt(res[0] * res[0] + res[2] * res[2]);
    if (!(resM <= kMaxResidualM)) {
        InterlockedIncrement64(&g_rcRefused);
        if (!InterlockedExchange(&g_rcSaidRefused, 1)) {
            // [PROBE: RSCAM] gate=roomscale_enable  site=HEADSET  log-only=yes
            LOG("[RSCAM] residual %.3f m REFUSED (over %.2f m - a teleport / respawn is not a step): share %.3f %.3f last %.3f %.3f intra "
                "%.3f %.3f taken %.3f %.3f - the head is left as reported; counted per window", static_cast<double>(resM),
                static_cast<double>(kMaxResidualM), share[0], share[1], lastX, lastZ, intraX, intraZ, taken[0], taken[1]);
        }
        return r;
    }
    // world -> the body frame C08100 adds the head in (it turns it back with VectorLocalToWorld(body +24), +C081EB)
    float loc[3] = { 0.0f, 0.0f, 0.0f };
    if (!CallV2L(reinterpret_cast<void*>(player + 0x18), res, loc) || !std::isfinite(loc[0]) || !std::isfinite(loc[2])) {
        InterlockedIncrement64(&g_rcFault);
        return r;
    }
    // [LOADBEARING: RSCAM fix 1] the head C08100 puts HSPINE1 (and so the camera) at, moved back by roomscale's not-yet-shifted move
    out[0] -= loc[0];
    out[1] -= loc[1];
    out[2] -= loc[2];
    InterlockedIncrement64(&g_rcApplied);
    const float mm = 1000.0f * resM;
    g_rcSumMm += mm;
    if (mm > g_rcMaxMm) { g_rcMaxMm = mm; }
    if (!InterlockedExchange(&g_rcSaidFirst, 1)) {
        // [PROBE: RSCAM] gate=roomscale_enable  site=HEADSET  log-only=yes
        LOG("[RSCAM] FIRST CORRECTION (logged unconditionally) - residual world (%+.4f, %+.4f) m = share %.4f %.4f + last step %.4f %.4f "
            "+ intraframe %.4f %.4f (t %.1f ms) - taken %.4f %.4f | body frame (%+.4f %+.4f %+.4f) subtracted from the head C08100 adds",
            res[0], res[2], share[0], share[1], lastX, lastZ, intraX, intraZ, 1000.0f * t, taken[0], taken[1], loc[0], loc[1], loc[2]);
    }
    return r;
}

// Once, when roomscale is live: point COculusHelper's vtable slot 4 at RsHeadGetter. REFUSED by name (nothing written) unless the slot
// holds engine+2E6BB0 and gamedll+C0818E holds C08100's `call [r8+20h]` (41 FF 50 20) - another build of either refuses.
void InstallGetterOnce()
{
    if (g_getterHook != 0) { return; }
    const char* why = nullptr;
    uintptr_t slotVal = 0;
    unsigned char call[4] = {};
    void** const slot = reinterpret_cast<void**>(g_en + kHelperSlot4Rva);
    if (!g_v2l) {
        why = "the engine does not export IControlObject::VectorWorldToLocal";
    } else if (!RdQ(reinterpret_cast<uintptr_t>(slot), &slotVal) || slotVal != g_en + kHeadGetterRva) {
        why = "COculusHelper vtable slot 4 (engine+863398) does not hold engine+2E6BB0 (another engine build?)";
    } else {
        bool callOk = true;
        for (int i = 0; i < 4 && callOk; ++i) {
            unsigned char b = 0;
            callOk = RdB(g_gd + kC08100CallRva + i, &b) && b == kC08100CallBytes[i];
            call[i] = b;
        }
        if (!callOk) { why = "gamedll+C0818E is not C08100's `call [r8+20h]` (41 FF 50 20) (another gamedll build?)"; }
    }
    if (!why) {
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            why = "VirtualProtect on the vtable slot failed";
        } else {
            g_origGetter = reinterpret_cast<HeadGetterFn>(slotVal);
            InterlockedExchangePointer(slot, reinterpret_cast<void*>(&RsHeadGetter));   // one aligned 8-byte store
            DWORD tmp = 0;
            VirtualProtect(slot, sizeof(void*), old, &tmp);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            uintptr_t back = 0;
            if (!RdQ(reinterpret_cast<uintptr_t>(slot), &back) || back != reinterpret_cast<uintptr_t>(&RsHeadGetter)) {
                why = "the slot did not read back as ours after the write";
            }
        }
    }
    InterlockedExchange(&g_getterHook, why ? -1 : 1);
    // [PROBE: RSCAM] gate=roomscale_enable  site=FLAT  log-only=no
    LOG("[RSCAM] head getter (COculusHelper slot 4, engine+863398 -> was engine+2E6BB0) %s%s%s | corrects only the call returning to "
        "gamedll+C08192 (C08100); every other caller gets the original value", why ? "REFUSED - " : "LIVE", why ? why : "",
        why ? " - fix 1 is off this launch, roomscale itself unchanged" : "");
}

void RsCamWindow()
{
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    if (s_last == 0) { s_last = now; return; }
    if (now - s_last < 5000) { return; }
    s_last = now;
    const LONG64 calls = InterlockedExchange64(&g_rcCalls, 0), other = InterlockedExchange64(&g_rcOther, 0),
                 off = InterlockedExchange64(&g_rcOff, 0), noPlayer = InterlockedExchange64(&g_rcNoPlayer, 0),
                 unpaired = InterlockedExchange64(&g_rcUnpaired, 0), refused = InterlockedExchange64(&g_rcRefused, 0),
                 fault = InterlockedExchange64(&g_rcFault, 0), applied = InterlockedExchange64(&g_rcApplied, 0);
    const double mean = applied ? g_rcSumMm / static_cast<double>(applied) : 0.0;
    const float mx = g_rcMaxMm;
    g_rcSumMm = 0.0;
    g_rcMaxMm = 0.0f;
    // [PROBE: RSCAM] gate=roomscale_enable,diag_periodic_logs  site=HEADSET  log-only=yes
    LOG_PERIODIC("[RSCAM] 5 s | getter calls %lld: corrected %lld (|residual| mean %.1f max %.1f mm) | other callers %lld | roomscale off "
                 "%lld | no player %lld | unpaired %lld | refused %lld | read faults %lld", calls, applied, mean,
                 static_cast<double>(mx), other, off, noPlayer, unpaired, refused, fault);
}

void ProbeLine(uintptr_t player, int group, uintptr_t full, uintptr_t vt, const char* walkWhy)
{
    float world[3] = { NAN, NAN, NAN };
    float wanted[3] = { NAN, NAN, NAN }, extra[3] = { NAN, NAN, NAN };
    unsigned char mbp = 0xFF, lock = 0xFF, off = 0xFF;
    unsigned short wstate = 0xFFFF;
    if (player) { CallWorldPos(reinterpret_cast<void*>(player + 0x18), world); }
    if (full) {
        for (int i = 0; i < 3; ++i) { RdF(full + 0xA70 + 4 * i, &wanted[i]); RdF(full + 0xA94 + 4 * i, &extra[i]); }
        RdB(full + 0x7E8, &mbp);
        RdB(full + 0x850, &lock);
        RdB(full + 0x2B1, &off);
        RdW(full + 0x9C0, &wstate);
    }
    if (!g_vtSaid || vt != g_lastVt) {
        g_vtSaid = true;
        g_lastVt = vt;
        // [PROBE: ROOMSCALE] gate=diag_roomscale_probe,roomscale_enable  site=FLAT  log-only=yes
        LOG("[ROOMSCALE] walk body %s | PlayerDI %p, full object %p, vtable engine+%llX", walkWhy, reinterpret_cast<void*>(player),
            reinterpret_cast<void*>(full), vt ? static_cast<unsigned long long>(vt - g_en) : 0ULL);
    }
    if (!DiagRoomscaleProbe || g_lines >= kMaxLines) { return; }
    XrPosef head{};
    float hx = NAN, hz = NAN;
    if (GetHeadPose(head)) {
        const XrVector3f rep = NeckPivotReportedPos(head);
        hx = rep.x;
        hz = rep.z;
    }
    const float fold = HeadFollowFoldedDeg();
    float cam[12] = {};
    unsigned camAge = 0;
    const bool haveCam = CamHookGetRenderedViewMatrix(cam, &camAge);
    float yaw = NAN;
    const bool haveYaw = CamHookGetOpenXrToGameYaw(yaw);
    // The pair since the last line: the head's app delta, the camera's world delta, and the yaw's prediction of the world delta
    // (world = Ry(yaw) * app, x/z - the hypothesis 7a tests; the sign convention printed is the formula used, not a finding).
    float dhx = NAN, dhz = NAN, dcx = NAN, dcz = NAN, px = NAN, pz = NAN;
    if (haveCam && std::isfinite(hx) && g_havePrev) {
        dhx = hx - g_prevHead[0];
        dhz = hz - g_prevHead[1];
        dcx = cam[3] - g_prevCam[0];
        dcz = cam[11] - g_prevCam[1];
        if (haveYaw) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            px = c * dhx + s * dhz;
            pz = -s * dhx + c * dhz;
        }
    }
    if (haveCam && std::isfinite(hx)) {
        g_havePrev = true;
        g_prevHead[0] = hx; g_prevHead[1] = hz;
        g_prevCam[0] = cam[3]; g_prevCam[1] = cam[11];
    }
    ++g_lines;
    // [PROBE: ROOMSCALE] gate=diag_roomscale_probe  site=HEADSET  log-only=yes
    LOG("[ROOMSCALE] 0.5 s #%ld | group %d | control world (%.3f, %.3f, %.3f) | walk %s: wanted (%.2f, %.2f, %.2f) extra (%.2f, "
        "%.2f, %.2f) move-by-pos %u lock %u physics-off %u state 0x%04X | head app (%.3f, %.3f) fold %.1f deg | camera world (%.3f, "
        "%.3f, %.3f) age %u ms%s | yaw %.2f deg%s | delta head app (%.3f, %.3f) camera world (%.3f, %.3f) Ry(yaw) predicts (%.3f, "
        "%.3f)%s",
        g_lines, group, world[0], world[1], world[2], walkWhy, wanted[0], wanted[1], wanted[2], extra[0], extra[1], extra[2],
        static_cast<unsigned>(mbp), static_cast<unsigned>(lock), static_cast<unsigned>(off), static_cast<unsigned>(wstate), hx, hz,
        fold, cam[3], cam[7], cam[11], camAge, haveCam ? "" : " (NO CAMERA)", haveYaw ? yaw * 57.29578f : 0.0f,
        haveYaw ? "" : " (NO YAW)", dhx, dhz, dcx, dcz, px, pz, g_lines == kMaxLines ? " | LAST ROOMSCALE line" : "");
}

}  // namespace

// COST: both off = two bool tests. On = one tick read per update; every 0.25 s the walk body resolved from scratch (~10 guarded
// reads, two engine calls - PhysicsBind and the bind's walk getter, the game's own); with the probe also GetWorldPosition and three
// layer getters every 0.5 s. The detour install runs once. Game thread (HookTail).
void RoomscaleProbeTick()
{
    if (!DiagRoomscaleProbe && !RoomscaleEnable) { return; }
    if (!Resolve()) { return; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastMs < 250) { return; }
    g_lastMs = now;
    static ULONGLONG s_lastProbeMs = 0;
    const bool probeDue = DiagRoomscaleProbe && now - s_lastProbeMs >= 500;
    if (probeDue) { s_lastProbeMs = now; }
    uintptr_t player = 0, vt = 0;
    int group = -99;
    const char* why = "no player";
    const uintptr_t full = ResolveWalk(&player, &group, &vt, &why);
    g_playerFull = full;
    g_playerDI = full ? player : 0;   // fix 1: the PlayerDI whose control object (+0x18) C08100 turns the head with
    InterlockedExchange(&g_group, group);
    if (RoomscaleEnable && full && g_hook == 0) { InstallOnce(); }
    if (RoomscaleEnable && full && g_hook == 1 && g_getterHook == 0) { InstallGetterOnce(); }
    if (g_getterHook == 1) { RsCamWindow(); }
    if (probeDue || !g_vtSaid || vt != g_lastVt) { ProbeLine(player, group, full, vt, why); }
}

bool RoomscaleResolveForProbe(uintptr_t* player, uintptr_t* full, int* group, float worldPos[3], const char** why)
{
    *player = 0;
    *full = 0;
    *group = -99;
    worldPos[0] = worldPos[1] = worldPos[2] = NAN;
    if (!Resolve()) {
        *why = g_state == -1 ? "the engine exports roomscale needs are missing (see the [ROOMSCALE] REFUSED line)" : "modules not mapped";
        return false;
    }
    uintptr_t vt = 0;
    *full = ResolveWalk(player, group, &vt, why);
    if (!*player) { return false; }
    CallWorldPos(reinterpret_cast<void*>(*player + 0x18), worldPos);
    return true;
}

bool RoomscaleLive() { return g_hook == 1 && g_playerFull != 0; }
int  RoomscaleGroup() { return static_cast<int>(g_group); }
bool RoomscaleOnFoot(int group) { return (group >= 0 && group <= 6) || group == 25; }

void RoomscaleTakeMoved(float* wx, float* wz)
{
    AcquireSRWLockExclusive(&g_lock);
    *wx = g_moved[0];
    *wz = g_moved[1];
    g_takenTotal[0] += g_moved[0];   // fix 1: every share handed to the appSpace shift (RsHeadGetter's taken)
    g_takenTotal[1] += g_moved[1];
    g_moved[0] = g_moved[1] = 0.0f;
    ReleaseSRWLockExclusive(&g_lock);
}

void RoomscaleNoteReported(const float pos[3])
{
    if (g_getterHook != 1) { return; }   // fix 1 not live: one load
    AcquireSRWLockExclusive(&g_lock);
    const int last = (g_ringHead + kRing - 1) % kRing;
    if (g_ringN > 0 && g_ringPos[last][0] == pos[0] && g_ringPos[last][1] == pos[1] && g_ringPos[last][2] == pos[2]) {
        g_ringTaken[last][0] = g_takenTotal[0];   // the same pose again (job workers ask ~16x a frame): keep one entry
        g_ringTaken[last][1] = g_takenTotal[1];
    } else {
        g_ringPos[g_ringHead][0] = pos[0]; g_ringPos[g_ringHead][1] = pos[1]; g_ringPos[g_ringHead][2] = pos[2];
        g_ringTaken[g_ringHead][0] = g_takenTotal[0];
        g_ringTaken[g_ringHead][1] = g_takenTotal[1];
        g_ringHead = (g_ringHead + 1) % kRing;
        if (g_ringN < kRing) { ++g_ringN; }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void RoomscaleSetCommand(float vx, float vz)
{
    AcquireSRWLockExclusive(&g_lock);
    g_cmd[0] = vx;
    g_cmd[1] = vz;
    ReleaseSRWLockExclusive(&g_lock);
}

void RoomscaleTakeStepStats(long* steps, long* commanded, long* guarded, long* gameChanged)
{
    AcquireSRWLockExclusive(&g_lock);
    *steps = g_steps; *commanded = g_commanded; *guarded = g_guarded; *gameChanged = g_gameChanged;
    g_steps = g_commanded = g_guarded = g_gameChanged = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace dlvr
