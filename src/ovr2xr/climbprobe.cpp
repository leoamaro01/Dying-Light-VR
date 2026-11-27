// Climbing P0 probe - see climbprobe.h. docs/design_climbing_overhaul.md §6 P0, §8.1 (2026-11-25, first written for the fork).
//
// WHAT IT READS (static analysis 2026-11-25 of the installed gamedll, whose vtables match the Steam rvas the mod already uses -
// design doc §2 / §8; nothing here is a write):
//   the controller table PlayerDI + 2976 + 8 * id, ids 0..42 (PlayerDI + 3324 is the controller group, so the table ends before it),
//     each controller's "active" byte at +16 (PlayerController::Start / Stop - boneblend.cpp itemhide::kActiveOff); every pointer is
//     named by RTTI (rtticam.h RttiClassNameOf) once per pointer, so no table id is assumed except by name;
//   EdgeGrabController (named by RTTI, wherever it sits): +0x1F8 climb type (the XP switch at +CEA740 reads it; boneblend.cpp's
//     kEgPhaseOff names the same field; first run: 0 = any climb that hangs), +0x1FC current state, +0x200 the PREVIOUS state
//     (the XP award fires when it is 8 = after a climb-up, +D0D754), +0x504 the collision flag (+D0A559 / +D121E4; first run:
//     1 = collisions normal, 0 = the climb's reduced set - design doc §8.4), +0x4D4 a byte its IPhysicsController slot 1 tests,
//     +0xC0 / +0xC4 the two ints its
//     replication slots 11 / 12 write and read (+D07A20 / +D07AA0), +0x268 the world velocity its IMoveController slot 0 hands
//     the player walk (+D0D9D0);
//   its IPhysicsController sub-object (RTTI: EdgeGrabController at complete-object offset 48), slots 0 (a float: 0.05 or by state,
//     +D12200) and 1 (a bool by state, +D16020) - CALLED, both read-only (state compares and getters);
//   the active IMoveController: gamedll +BA2980 (the walk's own selector; byte-checked, EGL row in buildrva_table.h), named by RTTI;
//   the walk body through roomscale.cpp's resolver (CODEPhysicsWalkFly, refused by vtable there): wanted velocity +0xA70, extra
//     +0xA94, move-by-position +0x7E8, lock +0x850, physics off +0x2B1, walk state +0x9C0 (FINDINGS-ROOMSCALE-STATIC-2026-10-27);
//   the control object's world position (GetWorldPosition, through the same resolver) -> the measured velocity per update.
// WHAT IT ANSWERS (design §8.1 open question): during a hang, which of {physics off, move-by-position, lock, walk state, the
//   IPhysicsController answers} differs from walking - i.e. what cancels gravity - and whether the walk body's wanted velocity
//   equals the ledge grab's +0x268 (the body moved by velocity through +ED2040).
#include "climbprobe.h"

#include "climbprobe_logic.h"
#include "roomscale.h"   // RoomscaleResolveForProbe - the PlayerDI / walk body / group / world position, read-only
#include "rtticam.h"     // RttiClassNameOf / RttiClassNameAndOffsetOf

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_CP_GD(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_CP_GD(rva) (rva)
#endif

namespace dlvr {

// [DEP: diag_climb_probe] required-for=[CLIMBPROBE] - climbing P0 (docs/design_climbing_overhaul.md §6): per-update samples while a
//      climbing controller (EdgeGrab / Ladder / ZipLine) is active, controller-set changes, an end-of-climb summary. tree=PROBE-ONLY.
//      Log-only, writes nothing. HEADSET (the climbs are the game's own, so FLAT works too). DEFAULT OFF.
bool DiagClimbProbe = false;

namespace {

constexpr uintptr_t kTableOff  = 2976;   // PlayerDI controller table (INV-327)
constexpr int       kTableIds  = 43;     // ids 0..42: PlayerDI + 3324 (the group) follows the table
constexpr uintptr_t kActiveOff = 16;     // PlayerController active byte
// EdgeGrabController fields (complete-object offsets) - see the file comment for where each was read
constexpr uintptr_t kEgType = 0x1F8, kEgState = 0x1FC, kEgEnd = 0x200 /* previous state */, kEgCollOff = 0x504 /* 1 = normal */,
                    kEg4D4 = 0x4D4, kEgReplA = 0xC0, kEgReplB = 0xC4, kEgVel = 0x268;
constexpr uint32_t  kEgPhysSub = 48;     // IPhysicsController@48 (RTTI base-class walk, design §2.1)
// LadderController fields (complete-object offsets; static analysis 2026-11-26, design doc §8.9):
//   +0x134 state (the only writer is set-state +D35F60, which keeps +0x138 the previous state and +0x13C the state's start time);
//   +0x40 + 4 * state the timed states' durations (filled by +D38E30; states 5..14 end when their time is up, +D35D17);
//   +0xCC rung count, +0xD0 current rung (int), +0xD4 that rung's world position (+D3B8EB / +D38570);
//   +0x10C the angle around the axis in degrees (+D3BBA0: turned by two input actions while +0x130 / +0x131 allow it, clamped by the
//   climbable's vfunc +0x30), +0x104 / +0x108 smoothed toward +0x114 / +0x118 (+D3BED8, SmoothCD);
//   +0x11C / +0x11D flags (+0x11D = "turning" this update); +0x188 the climbable object (an interface pointer: vfunc +8 valid, +0x28
//   its kind 0..3 - 2 / 3 slide on their own in state 13, +D3B754); attach-time vectors +0x30, +0x84..+0x9C, +0xE0..+0x104,
//   +0x14C..+0x160 (main slot 18 +D34340 writes them once).
constexpr uintptr_t kLdState = 0x134, kLdPrev = 0x138, kLdStateT = 0x13C, kLdDur = 0x40, kLdRungs = 0xCC, kLdRung = 0xD0,
                    kLdRungPos = 0xD4, kLdAngle = 0x10C, kLdSmooth = 0x104, kLdTarget = 0x114, kLdFlagA = 0x11C, kLdFlagB = 0x11D,
                    kLdTurnA = 0x130, kLdTurnB = 0x131, kLdObj = 0x188, kLdVecA = 0x30, kLdVecB = 0x84, kLdMat = 0xE0, kLdVecC = 0x14C;
// walk body (CODEPhysicsWalkFly full object) fields
constexpr uintptr_t kWkWanted = 0xA70, kWkExtra = 0xA94, kWkMbp = 0x7E8, kWkLock = 0x850, kWkOff = 0x2B1, kWkState = 0x9C0;
constexpr uintptr_t kSupplierRva = 0xBA2980;   // active IMoveController (walklook uses the same; EGL row exists)
// +BA2980: push rsi | sub rsp,20h | cmp dword ptr [rcx+0CFCh],-1 (the group test) - 13 bytes, read 2026-11-25
constexpr unsigned char kSupplierBytes[13] = { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xB9, 0xFC, 0x0C, 0x00, 0x00, 0xFF };

using SupplierFn = uintptr_t(__fastcall*)(uintptr_t player);
using PhysFloatFn = float(__fastcall*)(uintptr_t self);
using PhysBoolFn = bool(__fastcall*)(uintptr_t self);

enum Kind : int { kOther = 0, kEdgeGrab = 1, kLadder = 2, kZipLine = 3 };

struct Slot {
    uintptr_t ptr = 0;
    char      name[48] = {};
    int       kind = kOther;
    bool      isController = false;
};

int        g_state = 0;   // 0 not tried, 1 ready, -1 refused (named once)
SupplierFn g_supplier = nullptr;
Slot       g_slots[kTableIds];
uintptr_t  g_tablePlayer = 0;
uint64_t   g_prevMask = ~0ull;
climbprobe::Window g_w;
// per-update timing / the previous position, for the measured velocity
LARGE_INTEGER g_qpf = {}, g_prevQpc = {};
float      g_prevPos[3] = { NAN, NAN, NAN };
// per-climb summary (reset at kStart)
float      g_startPos[3] = {}, g_maxSpeed = 0.0f, g_maxWantedY = 0.0f, g_maxVelGap = 0.0f;
unsigned   g_physOffSeen = 0, g_mbpSeen = 0, g_lockSeen = 0;   // bit 0 = value 0 seen, bit 1 = nonzero seen
int        g_firstType = -1, g_lastType = -1, g_kindsSeen = 0;
bool       g_saidNoWalk = false, g_saidEgPhys = false;
bool       g_ladGeomSaid = false;   // this climb window's ladder attach line was given

bool RdQ(uintptr_t a, uintptr_t* o) { __try { *o = *reinterpret_cast<const uintptr_t*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool RdI(uintptr_t a, int* o) { __try { *o = *reinterpret_cast<const int*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool RdF(uintptr_t a, float* o) { __try { *o = *reinterpret_cast<const float*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool RdB(uintptr_t a, unsigned char* o) { __try { *o = *reinterpret_cast<const unsigned char*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool RdW(uintptr_t a, unsigned short* o) { __try { *o = *reinterpret_cast<const unsigned short*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

bool BytesMatch(uintptr_t a, const unsigned char* b, size_t n)
{
    __try { return std::memcmp(reinterpret_cast<const void*>(a), b, n) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t CallSupplier(uintptr_t player)
{
    __try { return g_supplier(player); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool CallPhys(uintptr_t sub, float* f, int* b)
{
    __try {
        const uintptr_t* vt = *reinterpret_cast<uintptr_t* const*>(sub);
        *f = reinterpret_cast<PhysFloatFn>(vt[0])(sub);
        *b = reinterpret_cast<PhysBoolFn>(vt[1])(sub) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Resolve()
{
    if (g_state != 0) { return g_state == 1; }
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gd) { return false; }   // not mapped yet: tried again next update
    const uintptr_t sup = reinterpret_cast<uintptr_t>(gd) + DLVR_CP_GD(kSupplierRva);
    const bool supOk = BytesMatch(sup, kSupplierBytes, sizeof(kSupplierBytes));
    g_supplier = supOk ? reinterpret_cast<SupplierFn>(sup) : nullptr;
    QueryPerformanceFrequency(&g_qpf);
    g_state = 1;
    // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
    LOG("[CLIMBPROBE] ARMED - active-IMoveController selector gamedll+%llX %s | table PlayerDI+%llu ids 0..%d, every slot named by "
        "RTTI | the ledge grab's fields and IPhysicsController answers are read only after RTTI names them",
        static_cast<unsigned long long>(DLVR_CP_GD(kSupplierRva)),
        supOk ? "byte-checked OK" : "*** BYTES DIFFER - not called, the 'move ctl' column says so ***",
        static_cast<unsigned long long>(kTableOff), kTableIds - 1);
    return true;
}

int KindOf(const char* n)
{
    if (std::strcmp(n, "EdgeGrabController") == 0) { return kEdgeGrab; }
    if (std::strcmp(n, "LadderController") == 0) { return kLadder; }
    if (std::strcmp(n, "ZipLineController") == 0) { return kZipLine; }
    return kOther;
}

// Re-reads the table; names any pointer that changed. Returns the active mask (bit id) of slots that are controllers.
uint64_t ReadTable(uintptr_t player)
{
    if (player != g_tablePlayer) {   // a new PlayerDI (load, death): forget every name
        for (int i = 0; i < kTableIds; ++i) { g_slots[i] = Slot{}; }
        g_tablePlayer = player;
        g_prevMask = ~0ull;
    }
    uint64_t mask = 0;
    for (int i = 0; i < kTableIds; ++i) {
        uintptr_t p = 0;
        if (!RdQ(player + kTableOff + 8 * static_cast<uintptr_t>(i), &p)) { p = 0; }
        Slot& s = g_slots[i];
        if (p != s.ptr) {
            s = Slot{};
            s.ptr = p;
            if (p && RttiClassNameOf(p, s.name, sizeof(s.name))) {
                s.isController = std::strstr(s.name, "Controller") != nullptr;
                s.kind = KindOf(s.name);
            }
        }
        unsigned char act = 0;
        if (s.ptr && s.isController && RdB(s.ptr + kActiveOff, &act) && act) { mask |= 1ull << i; }
    }
    return mask;
}

// "Name(id) Name(id) ..." for the mask, into buf.
void MaskNames(uint64_t mask, char* buf, size_t len)
{
    buf[0] = 0;
    size_t at = 0;
    for (int i = 0; i < kTableIds && at + 1 < len; ++i) {
        if (!(mask & (1ull << i))) { continue; }
        const int n = _snprintf_s(buf + at, len - at, _TRUNCATE, "%s%s(%d)", at ? " " : "", g_slots[i].name, i);
        if (n < 0) { break; }
        at += static_cast<size_t>(n);
    }
    if (at == 0) { strcpy_s(buf, len, "none"); }
}

uintptr_t FindKind(int kind, int* idOut)
{
    for (int i = 0; i < kTableIds; ++i) {
        if (g_slots[i].kind == kind && g_slots[i].ptr) {
            if (idOut) { *idOut = i; }
            return g_slots[i].ptr;
        }
    }
    return 0;
}

void Bits(unsigned* seen, unsigned v) { *seen |= v ? 2u : 1u; }

// n floats from a into out (NaN for any unreadable one), printed "a b c" into buf.
void FloatsTo(uintptr_t a, int n, char* buf, size_t len)
{
    buf[0] = 0;
    size_t at = 0;
    for (int i = 0; i < n && at + 1 < len; ++i) {
        float f = NAN;
        RdF(a + 4 * static_cast<uintptr_t>(i), &f);
        const int k = _snprintf_s(buf + at, len - at, _TRUNCATE, "%s%.3f", i ? " " : "", f);
        if (k < 0) { break; }
        at += static_cast<size_t>(k);
    }
}

// The ladder's attach-time picture, once per climb window: the climbable's class (by RTTI) and every vector main slot 18 writes.
void LogLadderAttach(int climb, uintptr_t ld, const float pos[3])
{
    uintptr_t obj = 0;
    RdQ(ld + kLdObj, &obj);
    char cls[64] = "?";
    uint32_t off = 0;
    if (!obj || !RttiClassNameAndOffsetOf(obj, cls, sizeof(cls), off)) { strcpy_s(cls, obj ? "? (no RTTI)" : "none"); }
    int rungs = -1;
    RdI(ld + kLdRungs, &rungs);
    char a[96], b[160], m[200], c[160], dur[260];
    FloatsTo(ld + kLdVecA, 3, a, sizeof(a));
    FloatsTo(ld + kLdVecB, 7, b, sizeof(b));
    FloatsTo(ld + kLdMat, 10, m, sizeof(m));
    FloatsTo(ld + kLdVecC, 6, c, sizeof(c));
    FloatsTo(ld + kLdDur, 17, dur, sizeof(dur));
    // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
    LOG("[CLIMBPROBE] LADDER ATTACH #%d | climbable %p class '%s' (interface at +%u) | rungs %d | player pos (%.3f, %.3f, %.3f) | "
        "+0x30: %s | +0x84..+0x9C: %s | +0xE0..+0x104: %s | +0x14C..+0x160: %s | state durations +0x40.. (index = state): %s",
        climb, reinterpret_cast<void*>(obj), cls, off, rungs, pos[0], pos[1], pos[2], a, b, m, c, dur);
}

}  // namespace

// COST: off = one bool test. On = 43 pointer reads + active bytes per update, RTTI only when a slot's pointer changes; inside a climb
// window also the selector, two IPhysicsController calls, ~25 guarded reads and one LOG line per update (capped, climbprobe_logic.h).
void ClimbProbeTick()
{
    if (!DiagClimbProbe) { return; }
    if (!Resolve()) { return; }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const double dtS = g_prevQpc.QuadPart ? static_cast<double>(qpc.QuadPart - g_prevQpc.QuadPart) / static_cast<double>(g_qpf.QuadPart)
                                          : 0.0;
    if (g_prevQpc.QuadPart && dtS < 0.001) { return; }   // a second tail call in the same update: one sample per update
    g_prevQpc = qpc;

    uintptr_t player = 0, full = 0;
    int group = -99;
    float pos[3];
    const char* why = "";
    if (!RoomscaleResolveForProbe(&player, &full, &group, pos, &why)) { return; }
    if (!full && !g_saidNoWalk) {
        g_saidNoWalk = true;
        // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
        LOG("[CLIMBPROBE] walk body not read (%s) - the walk columns print as n/a this session", why);
    }
    const uint64_t mask = ReadTable(player);
    const ULONGLONG now = GetTickCount64();

    if (mask != g_prevMask && g_w.lines < climbprobe::kMaxLines) {
        char was[512], is[512];
        MaskNames(g_prevMask == ~0ull ? 0 : g_prevMask, was, sizeof(was));
        MaskNames(mask, is, sizeof(is));
        ++g_w.lines;
        // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
        LOG("[CLIMBPROBE] CONTROLLERS %s -> %s | group %d | pos (%.3f, %.3f, %.3f)", g_prevMask == ~0ull ? "(first read)" : was, is,
            group, pos[0], pos[1], pos[2]);
    }
    g_prevMask = mask;

    int egId = -1, ldId = -1, zlId = -1;
    const uintptr_t eg = FindKind(kEdgeGrab, &egId), ld = FindKind(kLadder, &ldId), zl = FindKind(kZipLine, &zlId);
    const bool egOn = eg && egId >= 0 && (mask & (1ull << egId));
    const bool ldOn = ld && ldId >= 0 && (mask & (1ull << ldId));
    const bool zlOn = zl && zlId >= 0 && (mask & (1ull << zlId));
    const bool climbing = egOn || ldOn || zlOn;

    // a ledge grab alone gets the small cap (it is mapped); a ladder / zipline keeps the full one (run 4: the budget ran out first)
    const bool ledgeOnly = egOn && !ldOn && !zlOn;
    const climbprobe::Out o =
        climbprobe::Step(g_w, climbing, now, ledgeOnly ? climbprobe::kLedgeSamplesPerClimb : climbprobe::kMaxSamplesPerClimb);
    float mv[3] = { NAN, NAN, NAN };
    const bool haveMv = climbprobe::MeasuredVel(g_prevPos, pos, dtS, mv);
    g_prevPos[0] = pos[0]; g_prevPos[1] = pos[1]; g_prevPos[2] = pos[2];

    // the ledge grab's fields (read whenever a window is open or opening - the summary uses them)
    int egType = -1, egState = -1, egEnd = -1, replA = 0, replB = 0;
    unsigned char coll = 0xFF, b4d4 = 0xFF;
    float egVel[3] = { NAN, NAN, NAN }, phys0 = NAN;
    int phys1 = -1;
    if (eg && (g_w.in || o.event == climbprobe::kEnd)) {
        RdI(eg + kEgType, &egType);
        RdI(eg + kEgState, &egState);
        RdI(eg + kEgEnd, &egEnd);
        RdB(eg + kEgCollOff, &coll);
        RdB(eg + kEg4D4, &b4d4);
        RdI(eg + kEgReplA, &replA);
        RdI(eg + kEgReplB, &replB);
        for (int i = 0; i < 3; ++i) { RdF(eg + kEgVel + 4 * i, &egVel[i]); }
        char cls[48] = {};
        uint32_t off = 0;
        const bool subOk = RttiClassNameAndOffsetOf(eg + kEgPhysSub, cls, sizeof(cls), off) &&
                           std::strcmp(cls, "EdgeGrabController") == 0 && off == kEgPhysSub;
        if (subOk) {
            if (!CallPhys(eg + kEgPhysSub, &phys0, &phys1)) { phys0 = NAN; phys1 = -2; }
        } else if (!g_saidEgPhys) {
            g_saidEgPhys = true;
            // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
            LOG("[CLIMBPROBE] the ledge grab's +%u sub-object is not IPhysicsController by RTTI (class '%s', offset %u) - its two "
                "answers are NOT called (phys columns n/a)", kEgPhysSub, cls, off);
        }
    }

    // the ladder's fields (only while it is the active climb)
    int ldState = -9, ldPrev = -9, ldRungs = -1, ldRung = -1;
    float ldT0 = NAN, ldRungPos[3] = { NAN, NAN, NAN }, ldAngle = NAN, ldSmooth[2] = { NAN, NAN }, ldTarget[2] = { NAN, NAN };
    unsigned char ldFa = 0xFF, ldFb = 0xFF, ldTa = 0xFF, ldTb = 0xFF;
    if (ldOn) {
        RdI(ld + kLdState, &ldState);
        RdI(ld + kLdPrev, &ldPrev);
        RdF(ld + kLdStateT, &ldT0);
        RdI(ld + kLdRungs, &ldRungs);
        RdI(ld + kLdRung, &ldRung);
        for (int i = 0; i < 3; ++i) { RdF(ld + kLdRungPos + 4 * i, &ldRungPos[i]); }
        RdF(ld + kLdAngle, &ldAngle);
        for (int i = 0; i < 2; ++i) { RdF(ld + kLdSmooth + 4 * i, &ldSmooth[i]); RdF(ld + kLdTarget + 4 * i, &ldTarget[i]); }
        RdB(ld + kLdFlagA, &ldFa);
        RdB(ld + kLdFlagB, &ldFb);
        RdB(ld + kLdTurnA, &ldTa);
        RdB(ld + kLdTurnB, &ldTb);
    }

    // the walk body
    float wanted[3] = { NAN, NAN, NAN }, extra[3] = { NAN, NAN, NAN };
    unsigned char mbp = 0xFF, lock = 0xFF, poff = 0xFF;
    unsigned short wstate = 0xFFFF;
    if (full && (g_w.in || o.event != climbprobe::kNone)) {
        for (int i = 0; i < 3; ++i) { RdF(full + kWkWanted + 4 * i, &wanted[i]); RdF(full + kWkExtra + 4 * i, &extra[i]); }
        RdB(full + kWkMbp, &mbp);
        RdB(full + kWkLock, &lock);
        RdB(full + kWkOff, &poff);
        RdW(full + kWkState, &wstate);
    }

    if (o.event == climbprobe::kStart) {
        g_startPos[0] = pos[0]; g_startPos[1] = pos[1]; g_startPos[2] = pos[2];
        g_maxSpeed = g_maxWantedY = g_maxVelGap = 0.0f;
        g_physOffSeen = g_mbpSeen = g_lockSeen = 0;
        g_firstType = egOn ? egType : -1;
        g_kindsSeen = 0;
        g_ladGeomSaid = false;
        char is[512];
        MaskNames(mask, is, sizeof(is));
        // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
        LOG("[CLIMBPROBE] START climb #%d (%s) | controllers %s | group %d | pos (%.3f, %.3f, %.3f) | ledge grab type %d state %d",
            g_w.climbs, egOn ? "ledge grab" : ldOn ? "ladder/pipe/rope" : "zipline", is, group, pos[0], pos[1], pos[2], egType,
            egState);
    }
    if (g_w.in || o.event == climbprobe::kStart) {
        g_kindsSeen |= (egOn ? 1 : 0) | (ldOn ? 2 : 0) | (zlOn ? 4 : 0);
        if (egOn) {
            climbprobe::NoteState(g_w, egState);
            g_lastType = egType;
        }
        if (ldOn) {
            climbprobe::NoteLadderState(g_w, ldState);
            if (!g_ladGeomSaid && g_w.lines < climbprobe::kMaxLines) {   // the attach picture, once per window (one line)
                g_ladGeomSaid = true;
                ++g_w.lines;
                LogLadderAttach(g_w.climbs, ld, pos);
            }
        }
        if (haveMv) {
            const float sp = std::sqrt(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]);
            if (sp > g_maxSpeed) { g_maxSpeed = sp; }
        }
        if (std::isfinite(wanted[1]) && std::fabs(wanted[1]) > g_maxWantedY) { g_maxWantedY = std::fabs(wanted[1]); }
        if (egOn && std::isfinite(wanted[0]) && std::isfinite(egVel[0])) {
            const float gx = wanted[0] - egVel[0], gy = wanted[1] - egVel[1], gz = wanted[2] - egVel[2];
            const float gap = std::sqrt(gx * gx + gy * gy + gz * gz);
            if (gap > g_maxVelGap) { g_maxVelGap = gap; }
        }
        if (poff != 0xFF) { Bits(&g_physOffSeen, poff); }
        if (mbp != 0xFF) { Bits(&g_mbpSeen, mbp); }
        if (lock != 0xFF) { Bits(&g_lockSeen, lock); }
    }

    if (o.sample && ldOn) {
        // On a ladder the walk body and the ledge grab read nothing (run 4: all NaN / -1): the ladder's own fields instead.
        // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
        LOG("[CLIMBPROBE] #%d t+%llu LADDER | grp %d | state %d prev %d (since game time %.3f) | rung %d of %d at (%.3f, %.3f, %.3f), "
            "player y - rung y %.3f | angle %.2f deg, smoothed (%.3f, %.3f) -> (%.3f, %.3f) | flags 11C %u 11D %u turn %u/%u | pos "
            "(%.3f, %.3f, %.3f) measured v (%.2f, %.2f, %.2f) dt %.1f ms",
            g_w.climbs, static_cast<unsigned long long>(now - g_w.startMs), group, ldState, ldPrev, ldT0, ldRung, ldRungs, ldRungPos[0],
            ldRungPos[1], ldRungPos[2], pos[1] - ldRungPos[1], ldAngle, ldSmooth[0], ldSmooth[1], ldTarget[0], ldTarget[1],
            static_cast<unsigned>(ldFa), static_cast<unsigned>(ldFb), static_cast<unsigned>(ldTa), static_cast<unsigned>(ldTb), pos[0],
            pos[1], pos[2], mv[0], mv[1], mv[2], dtS * 1000.0);
    } else if (o.sample) {
        char mcName[48] = "n/a";
        if (g_supplier) {
            const uintptr_t mc = CallSupplier(player);
            if (!mc) { strcpy_s(mcName, "none"); }
            else if (!RttiClassNameOf(mc, mcName, sizeof(mcName))) { strcpy_s(mcName, "?"); }
        }
        // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
        // Labels per the first run (design doc §8.4): +0x200 is the PREVIOUS state; +0x504 is 1 = collisions normal, 0 = reduced.
        LOG("[CLIMBPROBE] #%d t+%llu%s | move ctl %s | grp %d | EG%s type %d state %d prev %d coll-normal %u b4D4 %u repl %d/%d vel (%.2f, "
            "%.2f, %.2f) phys0 %.3f phys1 %d | walk wanted (%.2f, %.2f, %.2f) extra (%.2f, %.2f, %.2f) mbp %u lock %u phys-off %u "
            "state 0x%04X | pos (%.3f, %.3f, %.3f) measured v (%.2f, %.2f, %.2f) dt %.1f ms | ladder %s zipline %s",
            g_w.climbs, static_cast<unsigned long long>(now - g_w.startMs), o.tail ? " TAIL" : "", mcName, group, egOn ? "" : "(off)",
            egType, egState, egEnd, static_cast<unsigned>(coll), static_cast<unsigned>(b4d4), replA, replB, egVel[0], egVel[1],
            egVel[2], phys0, phys1, wanted[0], wanted[1], wanted[2], extra[0], extra[1], extra[2], static_cast<unsigned>(mbp),
            static_cast<unsigned>(lock), static_cast<unsigned>(poff), static_cast<unsigned>(wstate), pos[0], pos[1], pos[2], mv[0],
            mv[1], mv[2], dtS * 1000.0, ldOn ? "ON" : "off", zlOn ? "ON" : "off");
    }
    if (o.capReached) {
        // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
        LOG("[CLIMBPROBE] climb #%d: %d samples - per-climb cap reached (%s), the rest of this climb is in its END summary only",
            g_w.climbs, g_w.samples, ledgeOnly ? "ledge grab: kLedgeSamplesPerClimb" : "kMaxSamplesPerClimb");
    }
    if (o.event == climbprobe::kEnd) {
        char seq[256] = {};
        size_t at = 0;
        for (int i = 0; i < g_w.seqN && at + 1 < sizeof(seq); ++i) {
            const int n = _snprintf_s(seq + at, sizeof(seq) - at, _TRUNCATE, "%s%d", i ? ">" : "", g_w.seq[i]);
            if (n < 0) { break; }
            at += static_cast<size_t>(n);
        }
        if (g_w.seqN == 0) { strcpy_s(seq, "none (no ledge grab)"); }
        char lseq[256] = {};
        at = 0;
        for (int i = 0; i < g_w.ladSeqN && at + 1 < sizeof(lseq); ++i) {
            const int n = _snprintf_s(lseq + at, sizeof(lseq) - at, _TRUNCATE, "%s%d", i ? ">" : "", g_w.ladSeq[i]);
            if (n < 0) { break; }
            at += static_cast<size_t>(n);
        }
        if (g_w.ladSeqN == 0) { strcpy_s(lseq, "none"); }
        // [PROBE: CLIMBPROBE] gate=diag_climb_probe  site=FLAT  log-only=yes
        LOG("[CLIMBPROBE] END climb #%d after %llu ms (%s%s%s) | ledge-grab states %s%s | ladder states %s%s | type first %d last %d | moved (%.2f, %.2f, "
            "%.2f) m | max measured speed %.2f m/s | max |wanted y| %.2f m/s | max |walk wanted - EG vel| %.3f m/s | walk phys-off seen "
            "%s, move-by-pos %s, lock %s (0 / nonzero) | %d sample(s), session lines %ld of %ld%s",
            g_w.climbs, static_cast<unsigned long long>(g_w.lastClimbMs - g_w.startMs), (g_kindsSeen & 1) ? "ledge grab " : "",
            (g_kindsSeen & 2) ? "ladder " : "", (g_kindsSeen & 4) ? "zipline" : "", seq, g_w.seqOverflow ? " (more not kept)" : "",
            lseq, g_w.ladSeqOverflow ? " (more not kept)" : "",
            g_firstType, g_lastType, pos[0] - g_startPos[0], pos[1] - g_startPos[1], pos[2] - g_startPos[2], g_maxSpeed, g_maxWantedY,
            g_maxVelGap, g_physOffSeen == 3 ? "both" : g_physOffSeen == 2 ? "nonzero" : g_physOffSeen == 1 ? "0" : "n/a",
            g_mbpSeen == 3 ? "both" : g_mbpSeen == 2 ? "nonzero" : g_mbpSeen == 1 ? "0" : "n/a",
            g_lockSeen == 3 ? "both" : g_lockSeen == 2 ? "nonzero" : g_lockSeen == 1 ? "0" : "n/a", g_w.samples, g_w.lines,
            climbprobe::kMaxLines, g_w.lines >= climbprobe::kMaxLines ? " | SESSION CAP REACHED - [CLIMBPROBE] is silent from here" : "");
    }
}

}  // namespace dlvr
