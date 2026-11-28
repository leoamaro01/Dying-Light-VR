// Loot by hand - docs/superpowers/specs/2026-10-23-loot-by-hand-design.md, plan docs/superpowers/plans/2026-10-23-loot-by-hand.md
// Task 3. The decision is looting_logic.h (unit-tested, src/src/tests/looting_logic_test.cpp).
//
// HOW (RESEARCH-NATIVE-LOOTING-2026-10-23 §5, INV-614 / INV-615, OUR gamedll 97046bba):
//   - The game gathers usable triggers AROUND THE PLAYER and the HUD picks one by look in GetNearbyTriggers (+B9BBB0), at most every
//     0.1 s and only while no trigger runs; the pick lands in PlayerDI+5272/+5280 through sub_180BED850. The X press reads that pick
//     and the TriggerController copies it at Start, then advances the held search while the action reads held.
//   - NearbyHook calls the original, publishes the bodies it gathered - standalone BodyContainers and, since loot_hand_run2, dead AIs
//     carrying a search container (INV-616: a fresh corpse is the AI) - every call while loot_by_hand=1, and - only
//     while a hand is settling / searching - stores the body nearest that hand with the game's own setter. Otherwise the game's
//     pick passes through untouched.
//   - LootPoll (scePad poll) feeds looting::HandMachine per hand; LootPadByte ORs Square while a hand searches.
//
// THREADS. LootPoll / LootPadByte run on the scePad poll (a game thread in the update phase, INV-138). NearbyHook runs on the HUD
// trigger widgets' update (HudTrigger +5B9BA0 / Hud3dTrigger +50B410) - presumed the game thread, NOT traced. The two share only
// atomics and two seqlocked arrays (no lock either side could wait on).
#include "looting.h"

#include "aimhook.h"        // AimMirrorYaw - the gun's yaw-sign convention (the throwables' mapping)
#include "bodygrab.h"       // body grab: the machine's DRAG / END events start and stop it (plan 2026-10-23-body-grab Task 3)
#include "bodygrab_logic.h" // YawQuat / QuatMul - the hand's world rotation for the severed-limb rotation hold
#include "boltwheel.h"      // InputLookupSetTriggerHold / InputLookupTriggerHoldLive - the search hold (trace 20261122_1620)
#include "boneblend.h"      // InstallSingleGameDetour - the shared verify / suspend / commit path
#include "calibscreen.h"    // CalibScreenActive - a blocked input for body grab
#include "camhook.h"        // CamHookGetOpenXrToGameYaw, CamHookGetRenderedViewMatrix
#include "controllers.h"
#include "headpose.h"       // GetHeadPose, NeckPivotReportedPos
#include "climbhand.h"      // ClimbHandHidesPrompt - the ladder's use prompt hidden in hand mode (climb_ladder_by_hand)
#include "camogesture.h"    // CamoGesturePadPoll - the camouflage wipe, beside body grab's hand publish
#include "looting_logic.h"
#include "lidhand.h"        // LidHandOnNearby - hand-open lids publish / null pick inside NearbyHook (trace 20261118_1711)
#include "lidhand_logic.h"  // lidhand::PromptKeepsGameText - the carriable prompt keeps "Take" (trace 20261126_1815)
#include "overlay.h"        // OverlayVisible - a blocked input for body grab
#include "promptquad.h"     // PromptQuadOnSlot - the in-world prompt reads the X slot here (trace 20261123_1356)

#include <windows.h>
#include <intrin.h>   // _ReturnAddress - the lid prompt's call-site test

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - diag_periodic_logs (the 5 s totals line only)

// The Epic (EGL) build: every fixed gamedll rva below is on tools/egl_allowlist.tsv (family 8, translated exactly by
// tools/egl_rva_table.py); resolved ONCE in TryInstall. The detour address is handed over as the Steam rva - InstallSingleGameDetour
// translates it itself (buildrva::Abs), so it must not be translated twice. The DEV tree has no buildrva.h: Steam rvas only.
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_LOOT_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_LOOT_GD_RVA(rva) (rva)
#endif

namespace dlvr {

// [DEP: loot_by_hand] required-for=looting a body by grip and hold (LootByHand) - spec 2026-10-23-loot-by-hand-design.
//      tree=INERT at 0: LootPoll / LootPadByte return at their first test, nothing is installed, no game memory is read.
//      HEADSET: needs both GRIP poses + squeeze, the head pose, the rendered view matrix and the OpenXR->game yaw (camhook),
//      and the GetNearbyTriggers detour (+B9BBB0, Steam gamedll 97046bba; the Epic build's rvas from buildrva_table.h family 8;
//      any other gamedll REFUSES by its bytes). Compiled default 1 since 2026-10-24 (3) (Jay: "They should be switched on" ->
//      "Yes." - for the tester package; verified loot_hand_run3b). Was 0 / held until the native interactions ship together.
bool  LootByHand = true;
// [DEP: loot_reach_m] required-for=how close the hand must be to a body's world box (LootReachM). tree=INERT at loot_by_hand=0.
float LootReachM = 0.25f;
// [DEP: loot_still_speed] required-for=the "hold still" rule (LootStillSpeed), m/s in tracking space over a ~100 ms window
//      (looting::SpeedWindow, D-269). tree=INERT at loot_by_hand=0. Compiled 0.75 = Jay's disk, the value loot_hand_run3b was
//      verified on (Tier D, 2026-10-24 (3); was 0.30).
float LootStillSpeed = 0.75f;
// [DEP: loot_settle_s] required-for=the still time before the search starts (LootSettleS). tree=INERT at loot_by_hand=0.
float LootSettleS = 0.30f;
// [DEP: diag_loot_why] required-for=[LOOTWHY] - why a body the player stands over gets no loot / camouflage prompt in VR (Jay
//      2026-10-27: "I can be directly over the body in VR and not be able to loot it, it wont show the prompt in the UI"). Inside the
//      GetNearbyTriggers detour (HUD thread), twice a second while a body is within the gather's reach: the game's own gather
//      (+EF2FC0) re-run with flags 3 / 1 / 0 (as the game / no look cone / no cone and no ray), each early gate of GetNearbyTriggers
//      read, and per body the values every filter compares (STATE 2026-10-27, ida/lt4b + lt9 + lt9b-d + lt10). tree=PROBE-ONLY
//      (log-only: calls the game's own read-only queries; writes nothing). DEV only, Steam rvas. HEADSET. DEFAULT OFF.
bool  DiagLootWhy = false;

namespace {

// ---- OUR gamedll 97046bba constants - RESEARCH-NATIVE-LOOTING §5 (lb1_prologue_out.txt, lt3 / lt4b / lt6). ----
constexpr uintptr_t kNearbyRva = 0xB9BBB0;   // PlayerDI::GetNearbyTriggers(player, out) - INV-615
constexpr unsigned char kNearbyBytes[15] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x53, 0x10, 0x49, 0x89, 0x4B, 0x08, 0x55, 0x56,
                                             0x41, 0x54 };   // mov r11,rsp / mov [r11+10h],rdx / mov [r11+8],rcx / push rbp
                                                             // / push rsi / push r12 - no RIP-relative operand (§5)
constexpr uintptr_t kSetterRva = 0xBED850;   // sub_180BED850(player, trigger, char) - writes +5272/+5280 (called, not patched)
constexpr uintptr_t kBodyVtRva = 0x164B528;  // BodyContainer's primary vftable (RTTI COL-checked, lt3)
// The PLAIN search containers (2026-11-22 (71), Jay: duffel bags, payphones ... "still under the default X interaction"; INV-764,
// ida/ctn1): DeclareMeshContainer -> class Container, BodyContainer's base - the same held action 1 in state 1 (+824), its own hold
// time (+504 x the player's multiplier, 0 = instant), one item into the inventory, then state 5. Gathered like a standalone body,
// flagged plain: never a body-grab drag, searchable only at state 1 exactly. Trace 20261122_1620.
constexpr uintptr_t kContainerVtRva = 0x165E268;   // Container's primary vftable (RTTI .?AVContainer@@, ctn1; Epic through buildrva)
// PlayerDI slot 324 (+BA0F20) passes the lookup the target player + 2288 (ida/ctn2, INV-765) - the search hold's target.
constexpr uintptr_t kLookupTargetOff = 2288;
// THE GRIP GLYPH (2026-11-22 (74), Jay: "We need this to be specific"; trace 20261122_1740, INV-766 + its RUN GG1 addendum): OUR
// HudTriggerSlot::Update +5B66D0 (slot, trigger), only caller HudTrigger::Update +5B8FE0. slot+256 = slot type (0 = the X slot, action
// 20), slot+120 = the active button preset, its T_Key text element at slot + 40*preset + 48, set with the element's vfunc +1472 (a
// ttl wide string {ptr, len, cap}) - ida/ctn9_out.txt :200-240 / :651-655. U+E075 = Jay's grip in boxed light 15 (Data3
// data/panel/boxed_light_15.fm + menu_common_PC.rpack boxed_semibold_30). Prologue: 15 bytes, whole instructions, no RIP-relative, no
// branch into the range (capstone, set-aside gamedll 97046bba).
constexpr uintptr_t kSlotUpdateRva = 0x5B66D0;
constexpr unsigned char kSlotUpdateBytes[15] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54,
                                                 0x41, 0x55 };   // mov rax,rsp / mov [rax+8],rcx / push rbp,rbx,rsi,rdi,r12,r13
constexpr size_t    kSlotTypeOff = 256;
constexpr size_t    kSlotPresetOff = 120;
constexpr size_t    kSlotPresetStride = 40;
constexpr size_t    kSlotKeyTextOff = 48;
constexpr size_t    kSetTextWOff = 1472;      // the text element's vfunc: set from a wide string
constexpr wchar_t   kGripGlyph = 0xE075;
// THE LID PROMPT (2026-11-23, trace 20261123_1200; Jay: "Bring the prompt back, but we'd change the text & use the new icon for it",
// key "Existing translated." -> TriggerGrab, scope "Lids + purses"): the pick getter +B9BB40 (player) has 16 call sites; HudTrigger's
// +5B9BA0 calls GetNearbyTriggers (our NearbyHook) and IMMEDIATELY the getter (return +5B9CFB), then keeps the result as its OWN
// copy (ida/ss24_out.txt :469-541). The detour returns the lid NearbyHook just nulled to that one site only; the player's pick stays
// null (X / holds unchanged). Prologue: 14 bytes = 3 whole instructions, no RIP operand, 0 branches into +B9BB41..+B9BB4D
// (ida/ss23_out.txt :3-19). The return site is verified at install: an E8 at +5B9CF6 whose target is +B9BB40.
constexpr uintptr_t kPickGetRva = 0xB9BB40;
constexpr unsigned char kPickGetBytes[14] = { 0x44, 0x8B, 0x89, 0xB8, 0x14, 0x00, 0x00, 0x48, 0x8B, 0xD1, 0x41, 0x8D, 0x41,
                                              0xC9 };   // mov r9d,[rcx+14B8h] / mov rdx,rcx / lea eax,[r9-37h]
constexpr uintptr_t kHudPickRetRva = 0x5B9CFB;   // HudTrigger +5B9BA0's return from the getter (call at -5)
constexpr size_t    kSlotActionTextOff = 152;    // the slot's action text element (ctn9 :689-697)
constexpr size_t    kSetTextKeyOff = 1448;       // the text element's vfunc: set from a narrow "&key&" string (localised)
// THE CAMOUFLAGE PROMPT HIDDEN (2026-11-22 (76), Jay: "We can just hide the camo prompt entirely"; trace 20261122_1800, INV-766
// camouflage addendum): the infected module's R1-slot handler +30BE80 (kInfectedSlot1Rva) is the ONLY offer of action 53; when the
// skill and its conditions fail it returns sub_1802B9820(a1, a2) instead - the human module's R1 answer. The detour returns that
// answer whenever the game would have said 53, so nobody in VR is shown "RB Use Camouflage" (the right grip it names is withheld on
// those corpses by GripCamoPoll; the VR camouflage is the wipe gesture, which never asks +30BE80). Prologue: 14 bytes, whole
// instructions, no RIP-relative, no branch into the range (capstone, set-aside gamedll 97046bba).
constexpr unsigned char kCamoOfferBytes[14] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41,
                                                0x40 };   // mov [rsp+10h],rbx / push rsi / sub rsp,20h / mov rax,[rcx+40h]
constexpr uintptr_t kNoCamoAnswerRva = 0x2B9820;   // sub_1802B9820(module, ctx) - +30BE80's own fallback
constexpr int       kCamoAction = 53;
constexpr size_t    kPickHandle = 5272;      // PlayerDI+5272 != 0 -> +5280 = the stored pick (INV-615)
constexpr size_t    kPickObj = 5280;
constexpr size_t    kCtlObjOff = 24;         // candidate + 24 = IControlObject (the gather's own box read, lt4b :1286)
// A FRESH corpse is gathered as the dead AI, not as a BodyContainer (INV-616, lf2 / lf4 / lf6): AI and HumanAI share
// GetTriggerActionInternal +18E280 at vftable slot 179, and the AI's logic module answers X with the container held at AI+6000
// (present when AI+5992 != 0). +6000 is read only on a candidate proven AI by its slot 179, and trusted only when the object it
// points at carries the BodyContainer vftable.
constexpr uintptr_t kAiTrigActRva = 0x18E280;
constexpr size_t    kTrigActSlot = 179;
constexpr size_t    kAiBodyHandle = 5992;
constexpr size_t    kAiBodyObj = 6000;
// Searched once (Jay: "a body can only be searched for loot a singular time"; RESEARCH-BODY-GRAB "SKELETON RUN"): the container's
// trigger state = the dword at BodyContainer +824 (OURS gamedll: BodyContainer vftable slot 195 +A57BF0 returns it; slot 180
// +A5A320 compares it with 5). Mac (comparison): PersistentTriggerObjectDI::GetState (+808) - GetBodyContainerTriggerAction offers
// the search only while the state is 1, Container::IsLooted = state 5 (or 1 and empty). A press on a body whose state is not 1 is
// not a search: with body_grab it grabs, without it it does not arm. Unreadable state = as before.
constexpr size_t    kContStateOff = 824;
constexpr uint32_t  kStateSearchable = 1;
// Right grip camouflage block: the infected logic modules' slot-1 (R1) handler +30BE80 (module vftable slot 283, shared by Zombie /
// Infected / Armored / HiveBrood / Prime / Spitter / Thug / Volatile - lc1; the human module's +2B9820 has no camouflage) returns
// action 53 when, among player-side tests, module+408 > 1.5 and AI+5713 != 0 (lf6). The module sits at AI+3408; the AI uses it only
// when its float [19] (+76) reads 0, else a default module (+18E280, lf4).
constexpr size_t    kAiModule = 3408;
constexpr size_t    kModuleDefaultFlag = 76;
constexpr size_t    kModuleSlot1Fn = 283;
constexpr uintptr_t kInfectedSlot1Rva = 0x30BE80;
constexpr size_t    kModuleCamoTime = 408;
constexpr size_t    kAiCamoFlag = 5713;
constexpr ULONGLONG kCamoFreshMs = 500;   // the HUD re-picks every <= 0.1 s while no trigger runs; older = no longer the pick
constexpr const char* kExtentsExport = "?GetExtentsInWorld@IControlObject@@QEBAAEBVextents@@XZ";   // engine +13E8B0
constexpr float     kMaxHandOffsetM = 1.5f;  // throwable.cpp:175 / grappleaim.cpp:760 - a tracking glitch is not a hand
constexpr int       kMaxBoxes = 16;
constexpr float     kGripThreshold = 0.5f;   // ovr2xr.cpp's vr_grip threshold
// body_grab: the longest a press with no body in reach waits for a due severed-limb reading (the search runs <= every 100 ms per
// hand from squeeze 0.2; relict_single_run1 saw a NO-ARM 5 ms before the first query). The grip is withheld while it waits.
constexpr double    kLateArmS = 0.15;

using NearbyFn = void(__fastcall*)(void* player, void* out);
using SetterFn = void(__fastcall*)(void* player, void* trigger, char flag);
using ExtentsFn = const float*(__fastcall*)(void* ctlObj);

struct GameVec {   // the out vector GetNearbyTriggers fills: { T** data; u32 count; u32 cap } (§5)
    void** data;
    unsigned count;
    unsigned cap;
};

uintptr_t g_gd = 0;
// the rvas as the loaded build has them (Steam values until TryInstall resolves them, before g_gd is published)
uintptr_t g_rSetter = kSetterRva, g_rBodyVt = kBodyVtRva, g_rAiTrigAct = kAiTrigActRva, g_rInfectedSlot1 = kInfectedSlot1Rva;
uintptr_t g_rContVt = kContainerVtRva;
uintptr_t g_rSlotUpdate = kSlotUpdateRva;
using SlotUpdateFn = void(__fastcall*)(void* slot, void* trigger);
SlotUpdateFn g_origSlot = nullptr;
volatile LONG g_glyphState = 0;      // 0 not installed, 1 live, -1 refused (named once)
volatile LONG g_nGlyph = 0, g_nGlyphFault = 0;
struct WideStr { wchar_t* p; uint32_t len; uint32_t cap; };   // ttl::string_base<wchar_t> (ctn9 :958-960: 2-byte chars, len, cap)
wchar_t g_gripText[2] = { kGripGlyph, 0 };                     // static: the element copies it; never freed by anyone
using CamoOfferFn = long long(__fastcall*)(void* module, void* ctx);   // both return __int64 (lf6): the full rax is passed through
CamoOfferFn g_origCamoOffer = nullptr;
uintptr_t g_rNoCamoAnswer = kNoCamoAnswerRva;
volatile LONG g_camoOfferState = 0, g_nCamoHidden = 0;
// HUD-ONLY scope (trace 20261122_1815; RUN CH1: swapping 53 for EVERY caller dropped searched infected corpses from the game's trigger
// gather, so body grab found no body): > 0 only while the game's own HudTriggerSlot::Update runs on this thread (SlotUpdateHook), the
// one caller whose answer is the prompt. Every other caller of +30BE80 gets the game's answer.
thread_local int t_inSlotUpdate = 0;
// THE LID PROMPT (trace 20261123_1200). t_hudLid: the lid / purse THIS thread's NearbyHook just nulled - read once by PickGetHook at
// HudTrigger's site (the same C call chain). g_promptLid: what that site was last handed - a GLOBAL, because the HUD updates hop
// worker threads; SlotUpdateHook compares the slot's trigger to it (never dereferenced).
thread_local void* t_hudLid = nullptr;
void* volatile g_promptLid = nullptr;
using PickGetFn = void*(__fastcall*)(void* player);
PickGetFn g_origPickGet = nullptr;
uintptr_t g_hudPickRet = 0;          // absolute; 0 never matches a return address
volatile LONG g_pickGetState = 0;    // 0 not installed, 1 live, -1 refused
volatile LONG g_nLidPrompt = 0, g_nGrabText = 0, g_nGrabTextFault = 0;
volatile LONG g_nCarryGlyph = 0;   // carriable prompts given the grip icon (trace 20261126_1815)
struct NarrowStr { const char* p; uint32_t len; uint32_t cap; };   // ttl::string_base<char> (ctn9 :169-170 / :510-511: ptr, len, cap)
const char g_grabKey[] = "&TriggerGrab&";                         // static: +1448 copies it (the game frees its own, ctn9 :1176)
// Remembered bodies (trace 20261122_1850): a SEARCHED body offers no action, so the game's gather drops it (+EF3E70 / +A5BD30) and
// body grab could not take it. Every body NearbyHook gathers is remembered by its IGSObject id; one missing from a later gather is
// re-proven by FindObjectByID (INV-663), the body test and its box, then appended to the box list. Engine exports, the handcarry pattern.
using MemGetIdFn = int(__fastcall*)(const void* gso);           // ?GetID@IGSObject@@QEBAHXZ  +252080
using MemFindFn = void*(__fastcall*)(void* gso, int id);        // ?FindObjectByID@IGSObject@@UEBAPEAV1@H@Z  +251DF0
constexpr size_t kMemGsoOff = 40;      // obj + 40 = IGSObject (handcarry.cpp kGsoOff)
constexpr int    kMemMax = 16;
constexpr float  kMemRangeM = 2.6f;    // the gather's own far radius (INV-615)
struct MemBody { int id; uintptr_t obj; ULONGLONG seenMs; };
MemBody   g_mem[kMemMax] = {};
int       g_memN = 0;
SRWLOCK   g_memLock = SRWLOCK_INIT;
MemGetIdFn g_memGetId = nullptr;
MemFindFn  g_memFind = nullptr;
volatile LONG g_memSaidNoExport = 0, g_memSaidNoPlayerBox = 0;
volatile LONG g_nMemAppended = 0, g_nMemForgot = 0;
// The gaze re-pick (2026-10-27, run 8): the gather, the cone's FOV variable and two early gates, as the loaded build has them.
constexpr uintptr_t kGatherRva2 = 0xEF2FC0, kVarRva2 = 0xC58E40, kUiGateRva2 = 0xCAEB00, kObjGateRva2 = 0x6D5590,
                    kGameDiRva2 = 0x1C16348, kVarAltRva2 = 0x1C122C8;
uintptr_t g_rGather = kGatherRva2, g_rVar = kVarRva2, g_rUiGate = kUiGateRva2, g_rObjGate = kObjGateRva2, g_rGameDi = kGameDiRva2,
          g_rVarAlt = kVarAltRva2;
NearbyFn  g_origNearby = nullptr;
SetterFn  g_setter = nullptr;
ExtentsFn g_extents = nullptr;
volatile LONG g_installState = 0;   // 0 not tried, 1 READY, -1 REFUSED (named once)
ULONGLONG g_lastTryMs = 0;
int       g_tries = 0;

// Shared pad thread <-> HUD hook. g_lootHand: -1 none, 0 left, 1 right (the hand settling / searching).
volatile LONG g_lootHand = -1;
volatile LONG g_handSeq = 0;
float g_handPos[3] = {};
volatile LONG g_boxSeq = 0;
int   g_boxCount = 0;
float g_boxA[kMaxBoxes][6] = {};   // the candidate's own box (a standalone BodyContainer, or the dead AI)
float g_boxC[kMaxBoxes][6] = {};   // a dead AI's attached container's box
bool  g_haveA[kMaxBoxes] = {};
bool  g_haveC[kMaxBoxes] = {};
void* g_boxObj[kMaxBoxes] = {};    // what the setter receives: the BodyContainer, or the dead AI
bool  g_boxPlain[kMaxBoxes] = {};  // a plain search Container (bag, bin, payphone ...) - not a body (trace 20261122_1620)
volatile LONG64 g_lootTarget = 0;  // the local player + 2288 (NearbyHook) - the search hold's lookup target (INV-765)
volatile LONG g_nPlainSaid = 0, g_nTapSaid = 0;

// Counters (5 s totals). The last gather's counts also ride the NO-ARM line (BETA compiles LOG_PERIODIC out).
volatile LONG g_nHook = 0, g_nSub = 0, g_nPass = 0, g_nNoBody = 0, g_nFault = 0;
volatile LONG g_lastCand = 0, g_lastStandalone = 0, g_lastAi = 0, g_lastAiBody = 0;
volatile LONG g_nArm = 0, g_nSearch = 0, g_nEndOpen = 0, g_nEndMoved = 0, g_nEndGone = 0, g_nNoArm = 0;
volatile LONG g_nDrag = 0, g_nEndDrag = 0;   // body grab: DRAG, and every END of a drag
constexpr LONG kMaxNoArmLines = 40;   // NO-ARM is one line per press edge - human rate; the cap only guards a flickering squeeze

// Right grip camouflage block: set by NearbyHook (HUD thread) after each re-pick, read by GripCamoPoll (pad thread).
volatile LONG      g_camoPick = 0;                 // 1 = the game's current pick is a camouflage target
volatile LONG64    g_camoTickMs = 0;               // GetTickCount64 of that evaluation
void* volatile     g_camoObj = nullptr;            // the pick, for the log
volatile LONG      g_nCamoWithheld = 0, g_nCamoSeen = 0;
looting::PressLatch g_camoLatch;

looting::HandMachine g_machine[2];
looting::SpeedWindow g_speed[2];   // tracking-space grip samples; reset on a press edge and when the pose drops out
bool g_prevGripDown[2] = {};

double NowS()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// ---- SEH-guarded game reads (no C++ objects in these frames). ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadBox(void* obj, float out[6])
{
    __try {
        const float* e = g_extents(static_cast<char*>(obj) + kCtlObjOff);
        if (!e) { return false; }
        for (int k = 0; k < 6; ++k) { out[k] = e[k]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadF32(uintptr_t a, float* out)
{
    __try { *out = *reinterpret_cast<const float*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadU8(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSetter(void* player, void* body)
{
    __try { g_setter(player, body, 1); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The game's own NULL pick (+BED850(player, 0, 0) - the game writes it itself at four sites, ida/lt4b :247 / :794 / :967 / :989).
bool CallSetterNull(void* player)
{
    __try { g_setter(player, nullptr, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The hand in game world: camera + R*(grip - reported head) - throwable.cpp:500-528 (verified in play), either hand. `rot` (x, y,
// z, w) = the same yaw applied to the grip rotation (10-24 (2), the severed-limb rotation hold: bodygrab_logic YawQuat is tested
// against the position formula below).
bool HandWorld(Hand h, float out[3], float trk[3], float rot[4])
{
    float rel = 0.0f;
    float m[12] = {};
    unsigned camAge = 0;
    XrPosef head{}, grip{};
    if (!CamHookGetOpenXrToGameYaw(rel) || !CamHookGetRenderedViewMatrix(m, &camAge) || !GetHeadPose(head) ||
        !GetControllerGripPose(h, grip)) {
        return false;
    }
    trk[0] = grip.position.x;
    trk[1] = grip.position.y;
    trk[2] = grip.position.z;
    const XrVector3f hr = NeckPivotReportedPos(head);
    const float off[3] = { grip.position.x - hr.x, grip.position.y - hr.y, grip.position.z - hr.z };
    if (off[0] * off[0] + off[1] * off[1] + off[2] * off[2] >= kMaxHandOffsetM * kMaxHandOffsetM) { return false; }
    const float c = std::cos(rel);
    float s = std::sin(rel);
    if (AimMirrorYaw) { s = -s; }
    out[0] = m[3] + off[0] * c + off[2] * s;
    out[1] = m[7] + off[1];
    out[2] = m[11] - off[0] * s + off[2] * c;
    float yq[4] = {};
    bodygrab::YawQuat(AimMirrorYaw ? -rel : rel, yq);
    const float gq[4] = { grip.orientation.x, grip.orientation.y, grip.orientation.z, grip.orientation.w };
    bodygrab::QuatMul(yq, gq, rot);
    return true;
}

// Nearest published body to p: index or -1; *dist = the nearer of its boxes, *da / *dc = each box's distance (-1 = no such box).
// *plainOut (optional) = that one is a plain search Container (trace 20261122_1620), read inside the same settled window.
int NearestBox(const float p[3], float* dist, void** objOut, float* da, float* dc, bool* plainOut = nullptr)
{
    if (plainOut) { *plainOut = false; }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG s0 = g_boxSeq;
        if (s0 & 1) { continue; }
        int best = -1;
        float bd = 0.0f, ba = -1.0f, bc = -1.0f;
        void* bo = nullptr;
        bool bp = false;
        const int n = g_boxCount;
        for (int i = 0; i < n && i < kMaxBoxes; ++i) {
            float a = -1.0f, c = -1.0f;
            const float d = looting::NearerBoxDist(g_boxA[i], g_haveA[i], g_boxC[i], g_haveC[i], p, &a, &c);
            if (d < 0.0f) { continue; }
            if (best < 0 || d < bd) { best = i; bd = d; ba = a; bc = c; bo = g_boxObj[i]; bp = g_boxPlain[i]; }
        }
        if (g_boxSeq == s0) {
            *dist = bd;
            *da = ba;
            *dc = bc;
            if (objOut) { *objOut = bo; }
            if (plainOut) { *plainOut = best >= 0 && bp; }
            return best;
        }
    }
    return -1;
}

// Is the player's stored pick (PlayerDI+5272/+5280, INV-615) an infected corpse the R1 slot would camouflage on? The game's own
// handler (+30BE80) tests, besides the player-side CanUseCamouflage / dissolved / renderable / Hellraid: module+408 > 1.5 and
// AI+5713 != 0. Every read is guarded; the module fields are read only after the AI test and the module's own slot-283 match.
// Is `ai` an AI (vftable slot 179 == +18E280) whose logic module is the infected one (AI+3408, module+76 == 0, the module's slot 283 ==
// +30BE80)? *moduleOut = that module. Shared by PickIsCamoTarget and the camouflage gesture (camogesture.cpp, LootInfectedModule).
// COST: five guarded reads. g_gd 0 (loot by hand not installed) = false.
bool InfectedModule(uintptr_t ai, uintptr_t* moduleOut)
{
    uintptr_t vt = 0, fn = 0, module = 0, mvt = 0, mfn = 0;
    float defFlag = 1.0f;
    *moduleOut = 0;
    if (!g_gd || !ai) { return false; }
    if (!ReadPtr(ai, &vt) || !ReadPtr(vt + 8 * kTrigActSlot, &fn) || fn != g_gd + g_rAiTrigAct) { return false; }
    if (!ReadPtr(ai + kAiModule, &module) || !module || !ReadF32(module + kModuleDefaultFlag, &defFlag) || defFlag != 0.0f) {
        return false;
    }
    if (!ReadPtr(module, &mvt) || !ReadPtr(mvt + 8 * kModuleSlot1Fn, &mfn) || mfn != g_gd + g_rInfectedSlot1) { return false; }
    *moduleOut = module;
    return true;
}

bool PickIsCamoTarget(uintptr_t player, uintptr_t* pickOut)
{
    uintptr_t h = 0, pick = 0, module = 0;
    float camoTime = 0.0f;
    unsigned char camoFlag = 0;
    *pickOut = 0;
    if (!ReadPtr(player + kPickHandle, &h) || !h || !ReadPtr(player + kPickObj, &pick) || !pick) { return false; }
    *pickOut = pick;
    if (!InfectedModule(pick, &module)) { return false; }
    if (!ReadF32(module + kModuleCamoTime, &camoTime) || !ReadU8(pick + kAiCamoFlag, &camoFlag)) { return false; }
    return camoFlag != 0 && camoTime > 1.5f;
}

// The search container's trigger state for a gathered body (a standalone BodyContainer, or a dead AI's container at +6000):
// -1 = unreadable / no container. COST: up to five guarded pointer reads.
int ContainerState(void* body)
{
    if (!body || !g_gd) { return -1; }
    const uintptr_t b = reinterpret_cast<uintptr_t>(body);
    const uintptr_t bodyVt = g_gd + g_rBodyVt;
    uintptr_t vt = 0, c = 0, h = 0, cvt = 0, fn = 0;
    if (!ReadPtr(b, &vt)) { return -1; }
    if (vt == bodyVt || vt == g_gd + g_rContVt) {   // a standalone BodyContainer, or a plain Container (its +824 too - ctn1 slot 195)
        c = b;
    } else if (ReadPtr(vt + 8 * kTrigActSlot, &fn) && fn == g_gd + g_rAiTrigAct && ReadPtr(b + kAiBodyHandle, &h) && h &&
               ReadPtr(b + kAiBodyObj, &c) && c && ReadPtr(c, &cvt) && cvt == bodyVt) {
        // c = the AI's container
    } else {
        return -1;
    }
    uintptr_t raw = 0;
    if (!ReadPtr(c + kContStateOff, &raw)) { return -1; }
    return static_cast<int>(static_cast<uint32_t>(raw));
}

// ---- [LOOTWHY] (diag_loot_why): why a body near the player is or is not the HUD's prompt. OUR gamedll 97046bba / engine
// e9fbfc2c, Steam rvas (DEV tree only). Read 2026-10-27: GetNearbyTriggers +B9BBB0 (ida/lt4b :207-767), the gather +EF2FC0
// (lt4b :1076-1667), PlayerDI slots 230 / 233 / 222 / 243 (lt9 / lt9c), +EF3CD0 IsApproachAngleCorrect / +EF3E70 (lt9b), the HUD
// +5B9BA0 and the gates +CAEB00 / +6D5590 (lt9c / lt9d), ENGINE ITools slots 240 / 884 (lt10). ----
namespace why {
constexpr uintptr_t kGatherRva   = 0xEF2FC0;    // bool (ILevel*, ctx*, GameVec* out, unsigned a4, const float* dir)
constexpr uintptr_t kApproachRva = 0xEF3CD0;    // bool (trigger, const float* eye) - TriggerObjectDI::IsApproachAngleCorrect (Mac)
constexpr uintptr_t kActionRva   = 0xEF3E70;    // bool (trigger, ctx*, int a3)
constexpr uintptr_t kVarRva      = 0xC58E40;    // float (table*, int index) - player variable 779 = CameraDefaultFOV (Mac name)
constexpr uintptr_t kPickDirRva  = 0x439080;    // dword* (GameDI+4368, dword* out, char) - the pick's direction
constexpr uintptr_t kUiGateRva   = 0xCAEB00;    // bool (*(ILevel+4632))
constexpr uintptr_t kObjGateRva  = 0x6D5590;    // bool ()
constexpr uintptr_t kGameDiRva   = 0x1C16348;   // GameDI
constexpr uintptr_t kToolsRva    = 0x1C11A70;   // ITools (engine object, INV-257)
constexpr uintptr_t kVarAltRva   = 0x1C122C8;   // the gather's alternative variable owner (lt4b :257)
constexpr int       kSlotEye = 230, kSlotLook = 233, kSlotMelee = 222, kSlotItem = 243, kSlotPoint = 181;
constexpr int       kCap = 256;                 // the gather never reallocates below this (its own growth path is CRT realloc)
constexpr long      kMaxLines = 1200;           // INV-9: two a second -> 10 minutes
using GatherFn   = bool(__fastcall*)(void* level, void* ctx, void* out, unsigned a4, const float* dir);
using ApproachFn = bool(__fastcall*)(void* trig, const float* eye);
using ActionFn   = bool(__fastcall*)(void* trig, void* ctx, int a3);
using VarFn      = float(__fastcall*)(void* table, int index);
using PickDirFn  = int*(__fastcall*)(uintptr_t gdi4368, int* out, char a3);
using UiGateFn   = bool(__fastcall*)(uintptr_t a1);
using ObjGateFn  = bool(__fastcall*)();
using Vec3Fn     = float*(__fastcall*)(void* self, float* out);
using ItemFn     = uintptr_t(__fastcall*)(void* self, int slot);
using BoolFn     = unsigned char(__fastcall*)(void* self);
using PtrFn      = uintptr_t(__fastcall*)(void* self);
using LevelFn    = void*(__fastcall*)(void* igs);
#pragma pack(push, 1)
struct Ctx {                 // the gather's context (lt4b :250-270 / +5B9BA0 :599-604): 40 bytes
    void*         player;
    int           slot;      // 4 = every action slot 0..3
    float         r1, r2;    // 1.8 / 2.6
    unsigned char flags;     // &1 the ray, &2 the look cone
    unsigned char pad[3];
    float         angFar, angNear, dFar, dNear;
};
#pragma pack(pop)
static_assert(sizeof(Ctx) == 40, "the gather reads 40 bytes of context");

ULONGLONG g_lastMs = 0;
long      g_lines = 0;
bool      g_saidArmed = false;
LevelFn   g_getLevel = nullptr;
// Per thread: the HUD has two callers of GetNearbyTriggers (+5B9BA0, +50B410) on the engine's job workers - never one shared buffer.
thread_local void* t_buf[4][kCap];

template <typename F> bool Call(F f) { __try { f(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool Gather(GatherFn fn, void* level, Ctx* ctx, GameVec* v, unsigned a4, const float* dir)
{
    __try { fn(level, ctx, v, a4, dir); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallVec3(uintptr_t self, int slot, float out[3])
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(self);
        reinterpret_cast<Vec3Fn>(*reinterpret_cast<const uintptr_t*>(vt + 8 * slot))(reinterpret_cast<void*>(self), out);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int CallBoolSlot(uintptr_t self, int slot)   // -1 = fault
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(self);
        return reinterpret_cast<BoolFn>(*reinterpret_cast<const uintptr_t*>(vt + 8 * slot))(reinterpret_cast<void*>(self)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int Active(uintptr_t player, int id)   // controller ID active (player + 2976 + 8 ID, byte +16 - INV-327), -1 unread
{
    uintptr_t c = 0;
    unsigned char b = 0;
    if (!ReadPtr(player + 2976 + 8 * static_cast<uintptr_t>(id), &c) || !c || !ReadU8(c + 16, &b)) { return -1; }
    return b ? 1 : 0;
}
int ItemGate(uintptr_t player)   // GetNearbyTriggers :229-232: hand item 0 and its +88 object's vfunc +2024; -1 unread
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(player);
        const uintptr_t item = reinterpret_cast<ItemFn>(*reinterpret_cast<const uintptr_t*>(vt + 8 * kSlotItem))(
            reinterpret_cast<void*>(player), 0);
        if (!item || item == static_cast<uintptr_t>(-88)) { return 0; }
        const uintptr_t o = item + 88;
        const uintptr_t ovt = *reinterpret_cast<const uintptr_t*>(o);
        return reinterpret_cast<BoolFn>(*reinterpret_cast<const uintptr_t*>(ovt + 2024))(reinterpret_cast<void*>(o)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int GdiGate(uintptr_t gd)   // :235-240: *(GameDI+1344) vfunc +1952 -> +4616 != 0; -1 unread
{
    __try {
        const uintptr_t gdi = *reinterpret_cast<const uintptr_t*>(gd + kGameDiRva);
        const uintptr_t o = gdi ? *reinterpret_cast<const uintptr_t*>(gdi + 1344) : 0;
        if (!o) { return 0; }
        const uintptr_t r = reinterpret_cast<PtrFn>(*reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(o) +
                                                                                        1952))(reinterpret_cast<void*>(o));
        if (!r) { return 0; }
        return *reinterpret_cast<const uintptr_t*>(r + 4616) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// AABB distance (the gather's v32, lt4b :1287-1319) from p to a world box {min xyz, max xyz}
float BoxDist(const float b[6], const float p[3])
{
    float s = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float c = 0.5f * (b[k] + b[k + 3]), h = b[k + 3] - c, d = p[k] - c;
        if (std::fabs(d) > h) { const float e = d > 0.0f ? d - h : d + h; s += e * e; }
    }
    return std::sqrt(s);
}
float AngDeg(const float a[3], const float b[3])
{
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la < 1e-6f || lb < 1e-6f) { return -1.0f; }
    float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return std::acos(c) * 57.29578f;
}
// a body = a standalone BodyContainer, or a dead AI carrying one (NearbyHook's own test)
bool IsBody(uintptr_t obj)
{
    uintptr_t vt = 0, fn = 0, h = 0, c = 0, cvt = 0;
    if (!ReadPtr(obj, &vt)) { return false; }
    if (vt == g_gd + g_rBodyVt) { return true; }
    return ReadPtr(vt + 8 * kTrigActSlot, &fn) && fn == g_gd + g_rAiTrigAct && ReadPtr(obj + kAiBodyHandle, &h) && h &&
           ReadPtr(obj + kAiBodyObj, &c) && c && ReadPtr(c, &cvt) && cvt == g_gd + g_rBodyVt;
}
bool InVec(const GameVec& v, uintptr_t o)
{
    for (unsigned i = 0; i < v.count && i < static_cast<unsigned>(kCap); ++i) {
        if (reinterpret_cast<uintptr_t>(v.data[i]) == o) { return true; }
    }
    return false;
}

// COST while diag_loot_why=1: every HUD re-pick a tick compare; twice a second three runs of the game's own gather (its box tests
// and up to one raytrace per candidate each), ~30 guarded reads and ~15 game calls, one LOG line when a body is within reach.
void Evaluate(void* player)
{
    const uintptr_t pl = reinterpret_cast<uintptr_t>(player);
    const ULONGLONG now = GetTickCount64();
    if (g_lines >= kMaxLines || now - g_lastMs < 500) { return; }
    g_lastMs = now;
    if (!g_getLevel) {
        const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
        g_getLevel = en ? reinterpret_cast<LevelFn>(GetProcAddress(en, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ")) : nullptr;
    }
    // The probe's rvas are Steam values and NOT on the Epic allowlist: on any build whose loot rvas were translated (the BETA
    // tree's buildrva, TryInstall), calling them would land in other code - refuse by name, once, and never call.
    if (g_rSetter != kSetterRva) {
        if (!g_saidArmed) {
            g_saidArmed = true;
            g_lines = kMaxLines;
            // [PROBE: LOOTWHY] gate=diag_loot_why  site=HEADSET  log-only=yes
            LOG("looting: [LOOTWHY] REFUSED - this gamedll is not the Steam build the probe's addresses were read from (the loot rvas "
                "were translated); nothing is called");
        }
        return;
    }
    if (!g_saidArmed) {
        g_saidArmed = true;
        ++g_lines;
        // [PROBE: LOOTWHY] gate=diag_loot_why  site=HEADSET  log-only=yes
        LOG("looting: [LOOTWHY] ARMED (logged unconditionally) - gather +%llX x3 (flags 3 game / 1 no cone / 0 no cone no ray), "
            "approach +%llX, action +%llX, pick dir +%llX, gates +%llX / +%llX, GetLevel %p | a line twice a second while a body is "
            "within the gather's 2.6 m of the eye; Steam rvas (DEV)",
            static_cast<unsigned long long>(kGatherRva), static_cast<unsigned long long>(kApproachRva),
            static_cast<unsigned long long>(kActionRva), static_cast<unsigned long long>(kPickDirRva),
            static_cast<unsigned long long>(kUiGateRva), static_cast<unsigned long long>(kObjGateRva),
            reinterpret_cast<void*>(g_getLevel));
    }
    if (!g_getLevel) { return; }
    void* level = nullptr;
    if (!Call([&] { level = g_getLevel(reinterpret_cast<void*>(pl + 40)); }) || !level) { return; }

    float eye[3] = {}, look[3] = {};
    const bool haveEye = CallVec3(pl, kSlotEye, eye), haveLook = CallVec3(pl, kSlotLook, look);
    float yaw = NAN, pitch = NAN;
    ReadF32(pl + 4460, &yaw);
    ReadF32(pl + 4464, &pitch);
    // the cone's FOV (lt4b :256-266)
    float fov = NAN;
    {
        unsigned char b80 = 0;
        ReadU8(pl + 80, &b80);
        uintptr_t owner = 0, alt = 0;
        ReadPtr(pl + 8 * 459, &owner);
        if (b80 && CallBoolSlot(pl, 2504 / 8) == 0 && ReadPtr(g_gd + kVarAltRva, &alt)) { owner = alt; }
        uintptr_t table = 0;
        if (owner && ReadPtr(owner + 208, &table) && table) {
            Call([&] { fov = reinterpret_cast<VarFn>(g_gd + kVarRva)(reinterpret_cast<void*>(table), 779); });
        }
    }
    const float angFar = fov * 0.0076794489f, angNearRaw = fov * 0.013823008f;
    const float angNear = angNearRaw >= 1.5707964f ? 1.5707964f : angNearRaw;
    unsigned char b9896 = 0;
    ReadU8(pl + 9896, &b9896);

    GameVec v[3];
    int nBody[3] = {};
    bool gOk[3] = {};
    static const unsigned char kFlags[3] = { 3, 1, 0 };
    for (int r = 0; r < 3; ++r) {
        Ctx ctx{};
        ctx.player = player;
        ctx.slot = 4;
        ctx.r1 = 1.8f;
        ctx.r2 = 2.6f;
        ctx.flags = kFlags[r];
        ctx.angFar = angFar;
        ctx.angNear = angNear;
        ctx.dFar = 1.8f;
        ctx.dNear = 0.5f;
        v[r].data = t_buf[r];
        v[r].count = 0;
        v[r].cap = kCap;
        gOk[r] = haveLook && std::isfinite(fov) &&
                 Gather(reinterpret_cast<GatherFn>(g_gd + kGatherRva), level, &ctx, &v[r], b9896 ? 1u : 0u, look);
        if (v[r].data != t_buf[r]) {   // never expected below kCap - if it happens the game's CRT owns that block: say so, stop
            ++g_lines;
            // [PROBE: LOOTWHY] gate=diag_loot_why  site=HEADSET  log-only=yes
            LOG("looting: [LOOTWHY] STOPPED - the gather grew its list past %d entries (its own realloc); the probe stops here and "
                "leaks that block rather than free it across CRTs", kCap);
            g_lines = kMaxLines;
            return;
        }
        for (unsigned i = 0; gOk[r] && i < v[r].count; ++i) {
            if (IsBody(reinterpret_cast<uintptr_t>(v[r].data[i]))) { ++nBody[r]; }
        }
    }
    if (nBody[2] == 0 && g_lines > 1) { return; }   // no body within reach at all - nothing to explain (the first line always prints)

    // the early gates (GetNearbyTriggers :209-249) and the HUD's (+5B9BA0)
    int grp = -99, b10896 = -1;
    {
        uintptr_t raw = 0;
        if (ReadPtr(pl + 3324, &raw)) { grp = static_cast<int>(static_cast<uint32_t>(raw)); }
        if (ReadPtr(pl + 10896, &raw)) { b10896 = static_cast<int>(static_cast<uint32_t>(raw)); }
    }
    int ui = -1, objGate = -1;
    {
        uintptr_t l4632 = 0;
        if (ReadPtr(reinterpret_cast<uintptr_t>(level) + 4632, &l4632)) {
            bool r = false;
            if (Call([&] { r = reinterpret_cast<UiGateFn>(g_gd + kUiGateRva)(l4632); })) { ui = r ? 1 : 0; }
        }
        bool r = false;
        if (Call([&] { r = reinterpret_cast<ObjGateFn>(g_gd + kObjGateRva)(); })) { objGate = r ? 1 : 0; }
    }
    const int melee = CallBoolSlot(pl, kSlotMelee), item = ItemGate(pl), gdiGate = GdiGate(g_gd);
    // the pick direction and the engine's VR-branch bytes (ITools slots 240 / 884, lt10)
    int vr = -1, s0 = -1, s1 = -1, s2 = -1, s37 = -1;
    float pdir[3] = { NAN, NAN, NAN };
    {
        uintptr_t tools = 0;
        if (ReadPtr(g_gd + kToolsRva, &tools) && tools) {
            vr = CallBoolSlot(tools, 240);
            uintptr_t set = 0;
            if (Call([&] { set = reinterpret_cast<PtrFn>(*reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(tools) +
                                                                                             8 * 884))(reinterpret_cast<void*>(tools)); }) && set) {
                unsigned char b = 0;
                if (ReadU8(set + 0, &b)) { s0 = b; }
                if (ReadU8(set + 1, &b)) { s1 = b; }
                if (ReadU8(set + 2, &b)) { s2 = b; }
                if (ReadU8(set + 37, &b)) { s37 = b; }
            }
        }
        uintptr_t gdi = 0;
        if (ReadPtr(g_gd + kGameDiRva, &gdi) && gdi && s37 >= 0) {
            int out[3] = {};
            if (Call([&] { reinterpret_cast<PickDirFn>(g_gd + kPickDirRva)(gdi + 4368, out, static_cast<char>(s37)); })) {
                std::memcpy(pdir, out, sizeof(pdir));
            }
        }
    }
    uintptr_t ph = 0, pick = 0;
    const bool havePick = ReadPtr(pl + kPickHandle, &ph) && ph && ReadPtr(pl + kPickObj, &pick);

    char bodies[1400];
    size_t bn = 0;
    bodies[0] = 0;
    int shown = 0;
    for (unsigned i = 0; gOk[2] && i < v[2].count && shown < 3; ++i) {
        const uintptr_t o = reinterpret_cast<uintptr_t>(v[2].data[i]);
        if (!IsBody(o)) { continue; }
        ++shown;
        float pt[3] = { NAN, NAN, NAN }, box[6] = {};
        const bool havePt = CallVec3(o, kSlotPoint, pt);
        const bool haveBox = ReadBox(reinterpret_cast<void*>(o), box);
        const float dEye = haveBox && haveEye ? BoxDist(box, eye) : -1.0f;
        float toPt[3] = { pt[0] - eye[0], pt[1] - eye[1], pt[2] - eye[2] };
        const float angLook = havePt && haveEye && haveLook ? AngDeg(toPt, look) : -1.0f;
        float lim = -1.0f;   // the gather's cone limit at this distance (lt4b :1592-1653: far 1.8 m, near 0.5 m)
        if (dEye >= 0.0f) {
            lim = dEye >= 1.8f ? angFar : (dEye <= 0.5f ? angNear : (angNear - angFar) * ((dEye - 1.8f) / (0.5f - 1.8f)) + angFar);
            lim *= 57.29578f;
        }
        const float angPick = havePt && haveEye && std::isfinite(pdir[0]) ? AngDeg(toPt, pdir) : -1.0f;
        const float dPick = havePt && haveEye ? std::sqrt(toPt[0] * toPt[0] + toPt[1] * toPt[1] + toPt[2] * toPt[2]) : -1.0f;
        int appr = -1, act = -1;
        {
            bool r = false;
            if (haveEye && Call([&] { r = reinterpret_cast<ApproachFn>(g_gd + kApproachRva)(reinterpret_cast<void*>(o), eye); })) {
                appr = r ? 1 : 0;
            }
            Ctx ctx{};
            ctx.player = player; ctx.slot = 4; ctx.r1 = 1.8f; ctx.r2 = 2.6f; ctx.flags = 3;
            ctx.angFar = angFar; ctx.angNear = angNear; ctx.dFar = 1.8f; ctx.dNear = 0.5f;
            if (Call([&] { r = reinterpret_cast<ActionFn>(g_gd + kActionRva)(reinterpret_cast<void*>(o), &ctx, b9896 ? 1 : 0); })) {
                act = r ? 1 : 0;
            }
        }
        char t[420];
        _snprintf_s(t, sizeof(t), _TRUNCATE,
                    " || body %p state %d: in game/no-cone/no-cone-no-ray %d/%d/%d | eye->box %.2f m | eye->point %.2f m, %.1f deg "
                    "from the look (cone %.1f), %.1f deg from the pick dir (needs < 25.8) | point (%.2f %.2f %.2f) %s | approach %d "
                    "action %d | %s",
                    reinterpret_cast<void*>(o), ContainerState(reinterpret_cast<void*>(o)), InVec(v[0], o) ? 1 : 0,
                    InVec(v[1], o) ? 1 : 0, 1, dEye, dPick, angLook, lim, angPick, pt[0], pt[1], pt[2],
                    havePt ? "" : "(UNREAD)", appr, act, havePick && pick == o ? "THE PICK" : "not the pick");
        for (const char* s = t; *s && bn + 1 < sizeof(bodies); ++s) { bodies[bn++] = *s; }
        bodies[bn] = 0;
    }
    ++g_lines;
    // [PROBE: LOOTWHY] gate=diag_loot_why  site=HEADSET  log-only=yes
    LOG("looting: [LOOTWHY] #%ld | bodies gathered as the game / no cone / no cone no ray: %d / %d / %d (calls %d%d%d) | gates: "
        "group %d, +10896 %d, grab %d dialog %d cannon %d knockdown %d injection %d defensive %d fire %d interaction %d trigger %d, "
        "melee %d, hand item %d, UI %d, object %d, GameDI %d | eye (%.2f %.2f %.2f)%s, look yaw %.1f pitch %.1f deg, dir (%.2f %.2f "
        "%.2f), CameraDefaultFOV %.1f -> cone %.1f..%.1f deg | pick dir (%.2f %.2f %.2f), engine VR %d settings %d%d%d / %d | "
        "pick %p%s%s",
        g_lines, nBody[0], nBody[1], nBody[2], gOk[0] ? 1 : 0, gOk[1] ? 1 : 0, gOk[2] ? 1 : 0, grp, b10896, Active(pl, 3),
        Active(pl, 21), Active(pl, 8), Active(pl, 11), Active(pl, 19), Active(pl, 23), Active(pl, 15), Active(pl, 25),
        Active(pl, 6), melee, item, ui, objGate, gdiGate, eye[0], eye[1], eye[2], haveEye ? "" : " (UNREAD)", yaw, pitch, look[0],
        look[1], look[2], fov, angFar * 57.29578f, angNear * 57.29578f, pdir[0], pdir[1], pdir[2], vr, s0, s1, s2, s37,
        havePick ? reinterpret_cast<void*>(pick) : nullptr, bodies, g_lines >= kMaxLines ? " | LAST LOOTWHY line" : "");
}

// ---- THE GAZE RE-PICK (Jay 2026-10-27: "No new setting, always on"). Run 8 ([LOOTWHY], backups/20261027_lootwhy_run8): the game's
// LOOK CONE was the only filter that ever dropped a body near the player - in VR the game's look (+4460 / +4464, slot 233) is not
// where the head looks (standing over a body looking down it read -50..-55 deg; crouched on one -25..+15). So after the game's own
// pick, when none of GetNearbyTriggers' / the HUD's gates is set, the game's gather (+EF2FC0) runs again with the HEAD's gaze as its
// cone direction - an ARGUMENT, so no game field is written (the HUD pick and the look write run on different job workers - STATE
// 2026-10-27) - and the best-aligned candidate by the game's own rule (dot > 0.9 within 2.2 m, GetNearbyTriggers :585-646) goes in
// through the game's own setter +BED850, the same call loot by hand makes here. X, the camouflage test and loot by hand then read
// it. No view (flat, a stale camera), a gate set, or nothing aligned: the game's own pick stands, untouched.
ULONGLONG g_gzWinMs = 0;
volatile LONG g_gzRuns = 0, g_gzSet = 0, g_gzKept = 0, g_gzGated = 0, g_gzNoView = 0, g_gzNone = 0, g_gzFault = 0;
volatile LONG g_gzSaidSet = 0, g_gzSaidGate = 0, g_gzSaidFault = 0;

int GdiGateT(uintptr_t gd)   // GdiGate with the build's GameDI rva
{
    __try {
        const uintptr_t gdi = *reinterpret_cast<const uintptr_t*>(gd + g_rGameDi);
        const uintptr_t o = gdi ? *reinterpret_cast<const uintptr_t*>(gdi + 1344) : 0;
        if (!o) { return 0; }
        const uintptr_t r = reinterpret_cast<PtrFn>(*reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(o) +
                                                                                        1952))(reinterpret_cast<void*>(o));
        if (!r) { return 0; }
        return *reinterpret_cast<const uintptr_t*>(r + 4616) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Every early gate of GetNearbyTriggers (lt4b :209-249) and the HUD (+5B9BA0): true = all read clear; *why = the first that did not
// (an unreadable gate counts as set - the re-pick then stands aside).
bool GatesClear(uintptr_t pl, void* level, const char** why)
{
    uintptr_t raw = 0;
    int grp = -99, b10896 = 1;
    if (ReadPtr(pl + 3324, &raw)) { grp = static_cast<int>(static_cast<uint32_t>(raw)); }
    if (ReadPtr(pl + 10896, &raw)) { b10896 = static_cast<int>(static_cast<uint32_t>(raw)); }
    if (grp == 12 || grp == 22 || grp == 24 || grp == -99) { *why = "controller group 12 / 22 / 24 (or unread)"; return false; }
    if (b10896 > 0) { *why = "PlayerDI +10896 > 0"; return false; }
    static const int kIds[] = { 3, 6, 8, 11, 19, 21, 25 };   // grab, trigger, cannon, knockdown, injection, dialog, interaction
    static const char* const kNames[] = { "grab", "trigger", "stationary cannon", "knockdown", "injection", "dialog", "interaction" };
    for (int i = 0; i < 7; ++i) {
        if (Active(pl, kIds[i]) != 0) { *why = kNames[i]; return false; }
    }
    const int melee = CallBoolSlot(pl, kSlotMelee);
    if (melee != 0 && (melee < 0 || Active(pl, 15) != 0 || Active(pl, 23) != 0)) { *why = "melee / kick with fire or defensive"; return false; }
    if (ItemGate(pl) != 0) { *why = "the hand item"; return false; }
    if (CallBoolSlot(pl, 36) != 0 || CallBoolSlot(pl, 271) != 0) { *why = "a candidate-filtering mode (slot 36 / 271)"; return false; }
    if (GdiGateT(g_gd) != 0) { *why = "GameDI +1344 +4616"; return false; }
    uintptr_t l4632 = 0;
    bool r = true;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(level) + 4632, &l4632) ||
        !Call([&] { r = reinterpret_cast<UiGateFn>(g_gd + g_rUiGate)(l4632); }) || r) {
        *why = "the UI gate (+CAEB00)";
        return false;
    }
    r = true;
    if (!Call([&] { r = reinterpret_cast<ObjGateFn>(g_gd + g_rObjGate)(); }) || r) { *why = "the object gate (+6D5590)"; return false; }
    *why = "none";
    return true;
}

// COST per HUD re-pick (<= every 0.1 s, VR only): one view-matrix copy, ~25 guarded reads and ~10 game calls for the gates, one run
// of the game's own gather (its box tests + up to one raytrace per candidate), one point call per candidate, and the setter only
// when the pick changes. Fills *out with the gaze gather's candidates (this thread's buffer) for loot by hand.
void GazeRepick(void* player, GameVec* out)
{
    out->data = t_buf[3];
    out->count = 0;
    out->cap = kCap;
    const uintptr_t pl = reinterpret_cast<uintptr_t>(player);
    InterlockedIncrement(&g_gzRuns);
    float m[12] = {};
    unsigned age = 0;
    if (!CamHookGetRenderedViewMatrix(m, &age)) { InterlockedIncrement(&g_gzNoView); return; }   // flat, or the camera is stale
    float gaze[3] = { -m[2], -m[6], -m[10] };   // forward = -(column 2) of the rendered view (lockpitch.cpp ViewPitchDeg)
    const float gl = std::sqrt(gaze[0] * gaze[0] + gaze[1] * gaze[1] + gaze[2] * gaze[2]);
    if (!(gl > 1e-4f)) { InterlockedIncrement(&g_gzNoView); return; }
    for (float& c : gaze) { c /= gl; }
    if (!g_getLevel) {
        const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
        g_getLevel = en ? reinterpret_cast<LevelFn>(GetProcAddress(en, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ")) : nullptr;
    }
    void* level = nullptr;
    if (!g_getLevel || !Call([&] { level = g_getLevel(reinterpret_cast<void*>(pl + 40)); }) || !level) {
        InterlockedIncrement(&g_gzFault);
        return;
    }
    const char* why = "none";
    if (!GatesClear(pl, level, &why)) {
        InterlockedIncrement(&g_gzGated);
        if (!InterlockedExchange(&g_gzSaidGate, 1)) {
            // [PROBE: LOOTGAZE] gate=always (no key)  site=HEADSET  log-only=no
            LOG("looting: [LOOTGAZE] first stand-aside - a gate of the game's own trigger pick is set (%s); the game's pick stands "
                "(counted on the totals line)", why);
        }
        return;
    }
    float eye[3] = {};
    if (!CallVec3(pl, kSlotEye, eye)) { InterlockedIncrement(&g_gzFault); return; }
    float fov = NAN;   // the cone's angles, as GetNearbyTriggers builds them (lt4b :256-266)
    {
        unsigned char b80 = 0;
        ReadU8(pl + 80, &b80);
        uintptr_t owner = 0, alt = 0, table = 0;
        ReadPtr(pl + 8 * 459, &owner);
        if (b80 && CallBoolSlot(pl, 2504 / 8) == 0 && ReadPtr(g_gd + g_rVarAlt, &alt)) { owner = alt; }
        if (owner && ReadPtr(owner + 208, &table) && table) {
            Call([&] { fov = reinterpret_cast<VarFn>(g_gd + g_rVar)(reinterpret_cast<void*>(table), 779); });
        }
    }
    if (!std::isfinite(fov)) { InterlockedIncrement(&g_gzFault); return; }
    unsigned char b9896 = 0;
    ReadU8(pl + 9896, &b9896);
    // The cone x kGazeConeScale (Jay 2026-10-27 after run 9, "still a little position specific" -> "Can you just up the gaze cone a
    // little?" -> "Go with that": 1.25 -> ~29 deg far .. ~52 deg near at CameraDefaultFOV 52; the game's own 22.9..41.2).
    constexpr float kGazeConeScale = 1.25f;
    Ctx ctx{};
    ctx.player = player; ctx.slot = 4; ctx.r1 = 1.8f; ctx.r2 = 2.6f; ctx.flags = 3;
    const float angNear = fov * 0.013823008f * kGazeConeScale;
    ctx.angFar = fov * 0.0076794489f * kGazeConeScale; ctx.angNear = angNear >= 1.5707964f ? 1.5707964f : angNear;
    ctx.dFar = 1.8f; ctx.dNear = 0.5f;
    if (!Gather(reinterpret_cast<GatherFn>(g_gd + g_rGather), level, &ctx, out, b9896 ? 1u : 0u, gaze) || out->data != t_buf[3]) {
        if (out->data != t_buf[3] && !InterlockedExchange(&g_gzSaidFault, 1)) {
            // [PROBE: LOOTGAZE] gate=always (no key)  site=HEADSET  log-only=no
            LOG("looting: [LOOTGAZE] the gather grew its list past %d entries (its own realloc) - that block is left to the game's "
                "CRT; no re-pick this time", kCap);
        }
        out->data = t_buf[3];
        out->count = 0;
        InterlockedIncrement(&g_gzFault);
        return;
    }
    uintptr_t best = 0;
    float bestDot = 0.8660254f, bestD = 0.0f;   // within 30 deg of the gaze (Jay 2026-10-27; the game's own rule 0.9 = 25.8) and 2.2 m
    for (unsigned i = 0; i < out->count && i < static_cast<unsigned>(kCap); ++i) {
        const uintptr_t o = reinterpret_cast<uintptr_t>(out->data[i]);
        float p[3];
        if (!o || !CallVec3(o, kSlotPoint, p)) { continue; }
        const float v[3] = { p[0] - eye[0], p[1] - eye[1], p[2] - eye[2] };
        const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(d > 1e-4f) || d >= 2.2f) { continue; }
        const float dot = (v[0] * gaze[0] + v[1] * gaze[1] + v[2] * gaze[2]) / d;
        if (dot > bestDot) { bestDot = dot; best = o; bestD = d; }
    }
    if (!best) { InterlockedIncrement(&g_gzNone); return; }
    uintptr_t h = 0, cur = 0;
    if (ReadPtr(pl + kPickHandle, &h) && h && ReadPtr(pl + kPickObj, &cur) && cur == best) { InterlockedIncrement(&g_gzKept); return; }
    if (!CallSetter(player, reinterpret_cast<void*>(best))) { InterlockedIncrement(&g_gzFault); return; }
    InterlockedIncrement(&g_gzSet);
    if (!InterlockedExchange(&g_gzSaidSet, 1)) {
        // [PROBE: LOOTGAZE] gate=always (no key)  site=HEADSET  log-only=no
        LOG("looting: [LOOTGAZE] FIRST re-pick by the head's gaze - %p (%s) %.2f m from the eye, %.1f deg off the gaze, replaces the "
            "game's pick %p | %u candidates in the gaze cone (CameraDefaultFOV %.1f) | thread %lu",
            reinterpret_cast<void*>(best), IsBody(best) ? "a body" : "another trigger", bestD, std::acos(bestDot) * 57.29578f,
            h ? reinterpret_cast<void*>(cur) : nullptr, out->count, fov, GetCurrentThreadId());
    }
}

void GazeTotals()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_gzWinMs < 5000) { return; }
    g_gzWinMs = now;
    // [PROBE: LOOTGAZE] gate=always (no key)  site=HEADSET  log-only=yes
    LOG_PERIODIC("looting: [LOOTGAZE] 5 s | re-picks run %ld: set %ld, already the pick %ld, nothing aligned %ld, gated %ld, no view %ld, "
                 "fault %ld", InterlockedExchange(&g_gzRuns, 0), InterlockedExchange(&g_gzSet, 0),
                 InterlockedExchange(&g_gzKept, 0), InterlockedExchange(&g_gzNone, 0), InterlockedExchange(&g_gzGated, 0),
                 InterlockedExchange(&g_gzNoView, 0), InterlockedExchange(&g_gzFault, 0));
}
}  // namespace why

// ---- remembered bodies (trace 20261122_1850). SEH frames hold no C++ objects. ----
bool MemCallId(uintptr_t obj, int* out)
{
    __try { *out = g_memGetId(reinterpret_cast<const char*>(obj) + kMemGsoOff); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// true = `obj` is still the live object with `id` (FindObjectByID through the player's IGSObject returns obj + 40)
bool MemStillLive(void* player, uintptr_t obj, int id)
{
    void* found = nullptr;
    __try { found = g_memFind(static_cast<char*>(player) + kMemGsoOff, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return found == reinterpret_cast<void*>(obj + kMemGsoOff);
}
// A body's boxes as the gather loop takes them: a standalone BodyContainer its own box; a dead AI its own + its container's.
bool MemBoxes(uintptr_t obj, float a[6], bool* haveA, float c[6], bool* haveC)
{
    uintptr_t vt = 0, cont = 0;
    if (!ReadPtr(obj, &vt)) { return false; }
    *haveA = ReadBox(reinterpret_cast<void*>(obj), a);
    *haveC = false;
    if (vt != g_gd + g_rBodyVt && ReadPtr(obj + kAiBodyObj, &cont) && cont) {
        *haveC = ReadBox(reinterpret_cast<void*>(cont), c);
    }
    return *haveA || *haveC;
}
float MemBoxDist(const float b[6], const float p[3])
{
    float s = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float lo = b[k] < b[k + 3] ? b[k] : b[k + 3], hi = b[k] < b[k + 3] ? b[k + 3] : b[k];
        const float d = p[k] < lo ? lo - p[k] : (p[k] > hi ? p[k] - hi : 0.0f);
        s += d * d;
    }
    return std::sqrt(s);
}

// Remember this gather's bodies, then append every remembered body this gather lacks that is still proven, within kMemRangeM of
// the player. COST: per HUD re-pick, <= 16 GetID + <= 16 FindObjectByID (no allocation) + their guarded reads; one lock.
void MemRememberAndAppend(void* player, void** objs, const bool* plain, float (*boxA)[6], bool* haveA, float (*boxC)[6],
                          bool* haveC, bool* outPlain, int* n)
{
    if (!g_memGetId || !g_memFind) {
        if (InterlockedIncrement(&g_memSaidNoExport) == 1) {
            // [PROBE: LOOTHAND] gate=loot_by_hand  site=FLAT  log-only=no
            LOG("looting: [LOOTHAND] REMEMBERED BODIES OFF - the engine does not export IGSObject::GetID / FindObjectByID (%p / %p); "
                "a searched body leaves the gather and body grab cannot take it", reinterpret_cast<void*>(g_memGetId),
                reinterpret_cast<void*>(g_memFind));
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const int gathered = *n;
    AcquireSRWLockExclusive(&g_memLock);
    // 1. remember (or refresh) every gathered body
    for (int i = 0; i < gathered; ++i) {
        if (plain[i]) { continue; }
        const uintptr_t o = reinterpret_cast<uintptr_t>(objs[i]);
        int id = 0;
        if (!MemCallId(o, &id)) { continue; }
        int slot = -1;
        for (int j = 0; j < g_memN; ++j) { if (g_mem[j].obj == o && g_mem[j].id == id) { slot = j; break; } }
        if (slot < 0) {
            if (g_memN < kMemMax) { slot = g_memN++; }
            else {
                slot = 0;   // the oldest seen is replaced
                for (int j = 1; j < g_memN; ++j) { if (g_mem[j].seenMs < g_mem[slot].seenMs) { slot = j; } }
            }
            g_mem[slot].id = id;
            g_mem[slot].obj = o;
        }
        g_mem[slot].seenMs = now;
    }
    // 2. the player's box centre (the range reference)
    float pb[6] = {}, pc[3] = {};
    const bool havePlayer = ReadBox(player, pb);
    if (havePlayer) { for (int k = 0; k < 3; ++k) { pc[k] = 0.5f * (pb[k] + pb[k + 3]); } }
    else if (InterlockedIncrement(&g_memSaidNoPlayerBox) == 1) {
        // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=no
        LOG("looting: [LOOTHAND] REMEMBERED BODIES: the player's box (player %p + 24) did not read - no range filter this re-pick "
            "(the hand's own reach still applies) | thread %lu", player, GetCurrentThreadId());
    }
    // 3. re-prove and append the remembered bodies this gather lacks
    for (int j = 0; j < g_memN;) {
        const MemBody m = g_mem[j];
        bool inGather = false;
        for (int i = 0; i < gathered; ++i) { if (reinterpret_cast<uintptr_t>(objs[i]) == m.obj) { inGather = true; break; } }
        if (inGather) { ++j; continue; }
        const char* reason = nullptr;
        float a[6] = {}, c[6] = {};
        bool hA = false, hC = false;
        if (!MemStillLive(player, m.obj, m.id)) { reason = "its id no longer finds it (gone / recycled)"; }
        else if (!why::IsBody(m.obj)) { reason = "no longer a body"; }
        else if (!MemBoxes(m.obj, a, &hA, c, &hC)) { reason = "no box read"; }
        if (reason) {
            const LONG k = InterlockedIncrement(&g_nMemForgot);
            if (k <= 3) {
                // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=yes
                LOG("looting: [LOOTHAND] REMEMBERED BODY FORGOTTEN #%ld - %p (id %d): %s | thread %lu", k,
                    reinterpret_cast<void*>(m.obj), m.id, reason, GetCurrentThreadId());
            }
            g_mem[j] = g_mem[--g_memN];
            continue;
        }
        ++j;
        if (havePlayer) {
            const float d = (std::fmin)(hA ? MemBoxDist(a, pc) : 1e9f, hC ? MemBoxDist(c, pc) : 1e9f);
            if (d > kMemRangeM) { continue; }   // kept, not appended
        }
        if (*n >= kMaxBoxes) { continue; }
        const int i = (*n)++;
        for (int k = 0; k < 6; ++k) { boxA[i][k] = a[k]; boxC[i][k] = c[k]; }
        haveA[i] = hA;
        haveC[i] = hC;
        outPlain[i] = false;
        objs[i] = reinterpret_cast<void*>(m.obj);
        const LONG k = InterlockedIncrement(&g_nMemAppended);
        if (k == 1) {
            // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=no
            LOG("looting: [LOOTHAND] FIRST REMEMBERED BODY - %p (id %d, %s) is not in the game's gather (searched: it offers no action) "
                "and is added back to loot by hand's list (%d gathered, %d remembered) | thread %lu", reinterpret_cast<void*>(m.obj),
                m.id, hC ? "a dead AI" : "a standalone container", gathered, g_memN, GetCurrentThreadId());
        }
    }
    ReleaseSRWLockExclusive(&g_memLock);
}

// ---- the detour: GetNearbyTriggers (+B9BBB0). Every call (installed once the camouflage block or loot by hand needs it): the
// original, then the camouflage-target test on the pick it just stored (a few guarded reads). COST while loot_by_hand=1: the original + one pass over its candidates (a vptr
// compare each; for a non-BodyContainer one more vtable-slot read, and for an AI two pointer reads + one vptr read; up to two box
// reads per body) every HUD re-pick (<= every 0.1 s); a setter call only while a hand is armed. ----
void __fastcall NearbyHook(void* player, void* out)
{
    g_origNearby(player, out);
    t_hudLid = nullptr;   // the lid prompt's hand-off: only this call's null may set it (trace 20261123_1200)
    if (!player || !out) { return; }
    InterlockedIncrement(&g_nHook);
    if (DiagLootWhy) { why::Evaluate(player); }   // [LOOTWHY] - log-only; off: one bool test
    // [LOADBEARING: gaze re-pick] (no key, always on in VR) the X prompt / camouflage / loot by hand on what the head looks at -
    // the game's own gather and setter, run 8's cone fix. Before the camouflage test, which reads the pick.
    GameVec gz{};
    why::GazeRepick(player, &gz);
    why::GazeTotals();
    // [LOADBEARING: lid_by_hand null pick] (lid_by_hand) hand-open lids (spec 2026-11-18-hand-open-lids-design, trace
    //      20261118_1711): right after the gaze re-pick and BEFORE every later reader of the pick (the camouflage test below, loot by
    //      hand's body pick, the HUD prompt, the X press): a game pick on a lid container or a carried item becomes the game's own
    //      NULL pick, so the game's interaction (walk-to, look turn, player clip) never starts for them. lid_by_hand=0 -> false at one
    //      bool test inside LidHandOnNearby.
    {
        const GameVec* lv = static_cast<const GameVec*>(out);
        void* promptLid = nullptr;
        if (LidHandOnNearby(player, lv->data, lv->count, gz.data, gz.count, &promptLid)) {
            if (!CallSetterNull(player)) {
                InterlockedIncrement(&g_nFault);
            } else {
                t_hudLid = promptLid;   // an unlocked lid / purse: HudTrigger's getter call right after this is handed it
            }
        }
    }
    BodyGrabPublishPlayer(player);   // severed limbs: bodygrab's level query takes the ILevel from the player
    {
        uintptr_t pick = 0;
        const bool camo = PickIsCamoTarget(reinterpret_cast<uintptr_t>(player), &pick);
        g_camoObj = reinterpret_cast<void*>(pick);
        InterlockedExchange(&g_camoPick, camo ? 1 : 0);
        InterlockedExchange64(&g_camoTickMs, static_cast<LONG64>(GetTickCount64()));
        if (camo && InterlockedIncrement(&g_nCamoSeen) == 1) {
            // [PROBE: GRIPCAMO] gate=always (no key)  site=HEADSET  log-only=yes
            LOG("looting: [GRIPCAMO] first camouflage target seen as the game's pick %p (infected corpse: module slot 283 = "
                "+%llX, its own preconditions met) - a right-grip press made on it now withholds R1 | thread %lu",
                reinterpret_cast<void*>(pick), static_cast<unsigned long long>(kInfectedSlot1Rva), GetCurrentThreadId());
        }
    }
    if (!LootByHand) { return; }
    // the search hold's lookup target (INV-765): this HUD's player = the local player
    InterlockedExchange64(&g_lootTarget, static_cast<LONG64>(reinterpret_cast<uintptr_t>(player) + kLookupTargetOff));
    const GameVec* v = static_cast<const GameVec*>(out);
    const uintptr_t bodyVt = g_gd + g_rBodyVt;
    const uintptr_t contVt = g_gd + g_rContVt;
    const uintptr_t aiTrigAct = g_gd + g_rAiTrigAct;
    int n = 0, nStandalone = 0, nAi = 0, nAiBody = 0, nPlain = 0;
    float boxA[kMaxBoxes][6] = {}, boxC[kMaxBoxes][6] = {};
    bool haveA[kMaxBoxes] = {}, haveC[kMaxBoxes] = {}, plain[kMaxBoxes] = {};
    void* objs[kMaxBoxes];
    bool fault = false;
    // the game's gathered list, then the gaze gather's (a body near the feet is often only in the second - run 8)
    const unsigned count = v->count;
    const unsigned total = count + gz.count;
    for (unsigned i = 0; i < total && n < kMaxBoxes; ++i) {
        uintptr_t obj = 0, vt = 0;
        const bool fromGame = i < count;
        if (fromGame) {
            if (!ReadPtr(reinterpret_cast<uintptr_t>(v->data) + 8 * i, &obj) || !obj) { fault = true; continue; }
        } else {
            obj = reinterpret_cast<uintptr_t>(gz.data[i - count]);
            if (!obj) { continue; }
            bool dup = false;
            for (int j = 0; j < n; ++j) { if (reinterpret_cast<uintptr_t>(objs[j]) == obj) { dup = true; break; } }
            if (dup) { continue; }
        }
        if (!ReadPtr(obj, &vt)) { fault = true; continue; }
        if (vt == bodyVt || vt == contVt) {   // a standalone container (loot_hand_run1's body), or a plain search Container
            haveA[n] = ReadBox(reinterpret_cast<void*>(obj), boxA[n]);
            haveC[n] = false;
            if (!haveA[n]) { fault = true; continue; }
            plain[n] = vt == contVt;
            if (plain[n]) { ++nPlain; } else { ++nStandalone; }
            objs[n++] = reinterpret_cast<void*>(obj);
            continue;
        }
        uintptr_t fn = 0;
        if (!ReadPtr(vt + 8 * kTrigActSlot, &fn)) { fault = true; continue; }
        if (fn != aiTrigAct) { continue; }   // not an AI - +6000 is never read on it
        ++nAi;
        uintptr_t h = 0, c = 0, cvt = 0;
        if (!ReadPtr(obj + kAiBodyHandle, &h) || !h || !ReadPtr(obj + kAiBodyObj, &c) || !c || !ReadPtr(c, &cvt) ||
            cvt != bodyVt) {
            continue;   // an AI without a search container (a live or non-lootable one)
        }
        haveA[n] = ReadBox(reinterpret_cast<void*>(obj), boxA[n]);
        haveC[n] = ReadBox(reinterpret_cast<void*>(c), boxC[n]);
        if (!haveA[n] && !haveC[n]) { fault = true; continue; }
        plain[n] = false;
        ++nAiBody;
        objs[n++] = reinterpret_cast<void*>(obj);   // the setter gets the AI: X then runs the game's own path for that body
    }
    // [LOADBEARING: remembered bodies] (loot_by_hand) a searched body the game's gather dropped (it offers no action) is added back,
    //      re-proven by id, so body grab can take it (trace 20261122_1850).
    MemRememberAndAppend(player, objs, plain, boxA, haveA, boxC, haveC, plain, &n);
    if (fault) { InterlockedIncrement(&g_nFault); }
    InterlockedExchange(&g_lastCand, static_cast<LONG>(count));
    InterlockedExchange(&g_lastStandalone, nStandalone);
    InterlockedExchange(&g_lastAi, nAi);
    InterlockedExchange(&g_lastAiBody, nAiBody);
    InterlockedIncrement(&g_boxSeq);   // odd: writing
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 6; ++k) {
            g_boxA[i][k] = boxA[i][k];
            g_boxC[i][k] = boxC[i][k];
        }
        g_haveA[i] = haveA[i];
        g_haveC[i] = haveC[i];
        g_boxObj[i] = objs[i];
        g_boxPlain[i] = plain[i];
    }
    g_boxCount = n;
    InterlockedIncrement(&g_boxSeq);   // even: settled
    if (nPlain > 0 && InterlockedIncrement(&g_nPlainSaid) == 1) {
        // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=yes
        LOG("looting: [LOOTHAND] FIRST PLAIN CONTAINER gathered - %d plain search container(s) (bag / bin / payphone ..., Container "
            "vftable +%llX as loaded) beside %d standalone body container(s) and %d dead AI with one | thread %lu",
            nPlain, static_cast<unsigned long long>(g_rContVt), nStandalone, nAiBody, GetCurrentThreadId());
    }

    if (g_lootHand < 0) {
        InterlockedIncrement(&g_nPass);
        return;
    }
    float hp[3];
    for (int attempt = 0;; ++attempt) {
        const LONG s0 = g_handSeq;
        hp[0] = g_handPos[0]; hp[1] = g_handPos[1]; hp[2] = g_handPos[2];
        if (!(s0 & 1) && g_handSeq == s0) { break; }
        if (attempt == 3) { InterlockedIncrement(&g_nNoBody); return; }
    }
    int best = -1;
    float bd = 0.0f, bda = -1.0f, bdc = -1.0f;
    for (int i = 0; i < n; ++i) {
        float a = -1.0f, c = -1.0f;
        const float d = looting::NearerBoxDist(boxA[i], haveA[i], boxC[i], haveC[i], hp, &a, &c);
        if (d < 0.0f) { continue; }
        if (best < 0 || d < bd) { best = i; bd = d; bda = a; bdc = c; }
    }
    if (best < 0 || bd > LootReachM) {
        const LONG k = InterlockedIncrement(&g_nNoBody);
        if (k == 1) {
            // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=no
            LOG("looting: [LOOTHAND] PICK REFUSED #1 - a hand is armed but no gathered body is within %.2f m of it "
                "(%d bodies gathered: %d standalone, %d dead AI with a container; nearest %.3f m) - the game's own pick stands | "
                "thread %lu",
                LootReachM, n, nStandalone, nAiBody, best < 0 ? -1.0f : bd, GetCurrentThreadId());
        }
        return;
    }
    uintptr_t h = 0, cur = 0;
    const uintptr_t pl = reinterpret_cast<uintptr_t>(player);
    const bool curOk = ReadPtr(pl + kPickHandle, &h) && (h == 0 || ReadPtr(pl + kPickObj, &cur));
    if (curOk && h != 0 && cur == reinterpret_cast<uintptr_t>(objs[best])) { return; }   // already ours
    const bool ok = CallSetter(player, objs[best]);
    const LONG k = ok ? InterlockedIncrement(&g_nSub) : InterlockedIncrement(&g_nFault);
    if (k == 1 || !ok) {
        // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=no
        LOG("looting: [LOOTHAND] PICK %s #%ld - the hand's body %p (%s; %.3f m from the hand - own box %.3f, container box %.3f, "
            "-1 = none) replaces the game's pick %p | %d bodies gathered | thread %lu",
            ok ? "SET" : "*** SETTER FAULTED ***", k, objs[best],
            bdc >= 0.0f ? "a dead AI" : (plain[best] ? "a plain search container" : "a standalone container"), bd, bda,
            bdc, curOk && h ? reinterpret_cast<void*>(cur) : nullptr, n, GetCurrentThreadId());
    }
}

// ---- the grip glyph (trace 20261122_1740). ----
// Is `trig` one loot by hand would search: a standalone BodyContainer or a dead AI holding one (IsBody), or a plain Container - in
// container state 1 exactly. COST: one vptr read + IsBody's / ContainerState's guarded reads.
bool HandSearchTarget(uintptr_t trig)
{
    uintptr_t vt = 0;
    if (!g_gd || !ReadPtr(trig, &vt)) { return false; }
    if (vt != g_gd + g_rContVt && !why::IsBody(trig)) { return false; }
    return ContainerState(reinterpret_cast<void*>(trig)) == static_cast<int>(kStateSearchable);
}
bool CallSetTextW(uintptr_t el, WideStr* s)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(el);
        using SetTextFn = void(__fastcall*)(void* self, WideStr* text);
        reinterpret_cast<SetTextFn>(*reinterpret_cast<const uintptr_t*>(vt + kSetTextWOff))(reinterpret_cast<void*>(el), s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [LOADBEARING: grip glyph] (loot_by_hand) the X prompt of a body / plain container a hand can search shows Jay's grip (U+E075)
//      instead of the X glyph - the game's slot update runs first and unchanged; only that slot's T_Key text is replaced afterwards.
// COST: per slot update (4 per HUD update): the original + two dword reads; for the X slot with a trigger the identity reads above;
// one vfunc call only when it qualifies.
bool CallSetTextKey(uintptr_t el, NarrowStr* s)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(el);
        using SetKeyFn = void(__fastcall*)(void* self, NarrowStr* key);
        reinterpret_cast<SetKeyFn>(*reinterpret_cast<const uintptr_t*>(vt + kSetTextKeyOff))(reinterpret_cast<void*>(el), s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The X slot's T_Key -> the grip (U+E075). True = set. COST: two guarded reads + one vfunc call.
bool SetGripGlyph(uintptr_t s, uint32_t* presetOut, uintptr_t* elOut)
{
    uintptr_t raw = 0;
    if (!ReadPtr(s + kSlotPresetOff, &raw)) { return false; }
    const uint32_t preset = static_cast<uint32_t>(raw);
    if (preset > 1) { return false; }   // the slot carries two presets (the update's own loop over 2, ctn9 :678-688)
    uintptr_t el = 0;
    if (!ReadPtr(s + kSlotPresetStride * preset + kSlotKeyTextOff, &el) || !el) { return false; }
    WideStr w{ g_gripText, 1, 1 };
    *presetOut = preset;
    *elOut = el;
    return CallSetTextW(el, &w);
}

void __fastcall SlotUpdateHook(void* slot, void* trigger)
{
    // P5 (climb_ladder_by_hand): a ladder / pipe / rope offers the game's own ladder climb, which hand mode refuses - its prompt is
    // updated as if nothing were targeted; P6 (climb_zipline_by_hand) the same for the zipline's prompt (the game's own null-trigger path, which this hook already sees), so neither the HUD
    // prompt nor the in-world copy (PromptQuadOnSlot, skipped below on null) shows.
    if (ClimbHandHidesPrompt(trigger)) { trigger = nullptr; }
    ++t_inSlotUpdate;   // the camouflage hide applies only to the action queries made inside this call (CamoOfferHook)
    g_origSlot(slot, trigger);
    --t_inSlotUpdate;
    if (!slot || !trigger || g_installState != 1) { return; }
    // the in-world prompt (trace 20261123_1356): reads the X slot's visibility, the key the game just set and the object's box -
    // BEFORE the lid branch below rewrites that key to &TriggerGrab&. inworld_prompts=0 -> one bool test inside.
    PromptQuadOnSlot(slot, trigger);
    const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
    uintptr_t raw = 0;
    // [LOADBEARING: lid prompt] (lid_by_hand) the prompt HudTrigger was handed for a lid / purse (PickGetHook): grip glyph + "Grab"
    //      (TriggerGrab, the game's own translations) - the game built "X Open" first; X itself still has no target (trace
    //      20261123_1200). COST: one pointer compare per slot update; two vfunc calls only on the lid's X slot.
    if (LidByHand && trigger == g_promptLid) {
        if (!ReadPtr(s + kSlotTypeOff, &raw) || static_cast<uint32_t>(raw) != 0) { return; }   // not the X slot
        uint32_t preset = 0;
        uintptr_t el = 0;
        const bool glyph = SetGripGlyph(s, &preset, &el);
        // a CARRIABLE handed off (trace 20261126_1815): the grip icon, the game's own translated "Take" kept (the in-world panel's word)
        if (lidhand::PromptKeepsGameText(LidHandIsCarriable(trigger))) {
            const LONG c = glyph ? InterlockedIncrement(&g_nCarryGlyph) : InterlockedIncrement(&g_nGrabTextFault);
            if (c == 1) {
                // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("looting: [CARRYHAND] FIRST CARRY GLYPH %s - X slot of carriable %p: T_Key %p (preset %u) -> U+E075 %s, the game's "
                    "action text kept | thread %lu", glyph ? "SET" : "*** FAULTED ***", trigger, reinterpret_cast<void*>(el), preset,
                    glyph ? "ok" : "NOT SET", GetCurrentThreadId());
            }
            return;
        }
        uintptr_t txt = 0;
        NarrowStr k{ g_grabKey, static_cast<uint32_t>(sizeof(g_grabKey) - 1), static_cast<uint32_t>(sizeof(g_grabKey) - 1) };
        const bool text = ReadPtr(s + kSlotActionTextOff, &txt) && txt && CallSetTextKey(txt, &k);
        const LONG n = (glyph && text) ? InterlockedIncrement(&g_nGrabText) : InterlockedIncrement(&g_nGrabTextFault);
        if (n == 1) {
            // [PROBE: LOOTHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("looting: [LOOTHAND] FIRST GRAB TEXT %s - X slot of lid / purse %p: T_Key %p (preset %u) -> U+E075 %s, action text "
                "%p -> \"%s\" %s | thread %lu", (glyph && text) ? "SET" : "*** FAULTED ***", trigger, reinterpret_cast<void*>(el),
                preset, glyph ? "ok" : "NOT SET", reinterpret_cast<void*>(txt), g_grabKey, text ? "ok" : "NOT SET",
                GetCurrentThreadId());
        }
        return;
    }
    if (!LootByHand) { return; }
    if (!ReadPtr(s + kSlotTypeOff, &raw) || static_cast<uint32_t>(raw) != 0) { return; }   // not the X slot
    if (!HandSearchTarget(reinterpret_cast<uintptr_t>(trigger))) { return; }
    if (!ReadPtr(s + kSlotPresetOff, &raw)) { return; }
    const uint32_t preset = static_cast<uint32_t>(raw);
    if (preset > 1) { return; }   // the slot carries two presets (the update's own loop over 2, ctn9 :678-688)
    uintptr_t el = 0;
    if (!ReadPtr(s + kSlotPresetStride * preset + kSlotKeyTextOff, &el) || !el) { return; }
    WideStr w{ g_gripText, 1, 1 };
    const bool ok = CallSetTextW(el, &w);
    const LONG k = ok ? InterlockedIncrement(&g_nGlyph) : InterlockedIncrement(&g_nGlyphFault);
    if (k == 1) {
        // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=no
        LOG("looting: [LOOTHAND] FIRST GRIP GLYPH %s - X slot of trigger %p (a %s, searchable) preset %u, T_Key %p set to U+E075 | "
            "thread %lu", ok ? "SET" : "*** SET FAULTED - the X glyph stays ***", trigger,
            why::IsBody(reinterpret_cast<uintptr_t>(trigger)) ? "body" : "plain container", preset, reinterpret_cast<void*>(el),
            GetCurrentThreadId());
    }
}

void InstallGripGlyph()
{
    int attempts = 0;
    // the trampoline goes straight into g_origSlot (as boltwheel's g_lookupTrue): the hook can never run with it unset
    const bool ok = InstallSingleGameDetour(g_gd + kSlotUpdateRva, kSlotUpdateBytes, sizeof(kSlotUpdateBytes),
                                            reinterpret_cast<void*>(&SlotUpdateHook), reinterpret_cast<void**>(&g_origSlot),
                                            "loot grip glyph slot update", &attempts);
    InterlockedExchange(&g_glyphState, ok ? 1 : -1);
    // [PROBE: LOOTHAND] gate=loot_by_hand  site=FLAT  log-only=yes
    LOG("looting: [LOOTHAND] GRIP GLYPH %s - HudTriggerSlot::Update (Steam +%llX, as loaded +%llX) %s (%zu bytes, %d attempt(s)); "
        "the X prompt of a searchable body / plain container %s",
        ok ? "READY" : "REFUSED", static_cast<unsigned long long>(kSlotUpdateRva), static_cast<unsigned long long>(g_rSlotUpdate),
        ok ? "detoured" : "NOT detoured - its first bytes differ from the builds these were read from, or the write was refused",
        sizeof(kSlotUpdateBytes), attempts, ok ? "shows the grip (U+E075)" : "keeps the game's X glyph");
}

// ---- the lid prompt (trace 20261123_1200). ----
// [LOADBEARING: lid prompt] (lid_by_hand) the pick getter +B9BB40 hands HudTrigger (its call at +5B9CF6 only) the lid / purse that
//      NearbyHook just nulled, so the game builds and places its own prompt for it; every other caller gets the game's value (the
//      null pick - X has no target). No write to the player's pick (D-393).
// COST: every getter call (16 sites, the input handlers per update): the original + one _ReturnAddress compare; at HudTrigger's
// site (<= every 0.1 s) two pointer writes.
void* __fastcall PickGetHook(void* player)
{
    void* r = g_origPickGet(player);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_hudPickRet) { return r; }
    void* const lid = t_hudLid;
    t_hudLid = nullptr;   // one shot
    if (!r && lid) {
        r = lid;
        if (InterlockedIncrement(&g_nLidPrompt) == 1) {
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("looting: [LIDHAND] FIRST LID PROMPT - HudTrigger handed the lid / purse %p the null pick hid (the player's pick "
                "stays null: X has no target) | thread %lu", lid, GetCurrentThreadId());
        }
    }
    InterlockedExchangePointer(const_cast<void**>(&g_promptLid), (lid && r == lid) ? lid : nullptr);
    return r;
}

void InstallLidPrompt()
{
    const uintptr_t ret = g_gd + DLVR_LOOT_GD_RVA(kHudPickRetRva);
    const uintptr_t get = g_gd + DLVR_LOOT_GD_RVA(kPickGetRva);
    unsigned char op = 0;
    int32_t rel = 0;
    bool siteOk = false;
    __try {
        op = *reinterpret_cast<const unsigned char*>(ret - 5);
        rel = *reinterpret_cast<const int32_t*>(ret - 4);
        siteOk = op == 0xE8 && ret + static_cast<intptr_t>(rel) == get;
    } __except (EXCEPTION_EXECUTE_HANDLER) { siteOk = false; }
    int attempts = 0;
    bool ok = false;
    if (siteOk) {
        g_hudPickRet = ret;
        MemoryBarrier();
        // the trampoline goes straight into g_origPickGet: the hook can never run with it unset
        ok = InstallSingleGameDetour(g_gd + kPickGetRva, kPickGetBytes, sizeof(kPickGetBytes), reinterpret_cast<void*>(&PickGetHook),
                                     reinterpret_cast<void**>(&g_origPickGet), "lid prompt pick getter", &attempts);
        if (!ok) { g_hudPickRet = 0; }
    }
    InterlockedExchange(&g_pickGetState, ok ? 1 : -1);
    // [PROBE: LOOTHAND] gate=always (no key)  site=FLAT  log-only=yes
    LOG("looting: [LOOTHAND] LID PROMPT %s - HudTrigger's getter return (Steam +%llX, as loaded +%llX): byte at -5 = %02X, call target "
        "%s (+%llX expected); pick getter (Steam +%llX) %s (%zu bytes, %d attempt(s)) - an unlocked lid / purse %s",
        ok ? "READY" : "REFUSED", static_cast<unsigned long long>(kHudPickRetRva), static_cast<unsigned long long>(ret - g_gd),
        op, siteOk ? "matches" : "*** DOES NOT MATCH - not this build's call site ***", static_cast<unsigned long long>(get - g_gd),
        static_cast<unsigned long long>(kPickGetRva),
        ok ? "detoured" : (siteOk ? "NOT detoured - its first bytes differ, or the write was refused" : "not attempted"),
        sizeof(kPickGetBytes), attempts, ok ? "shows the game's prompt with the grip + Grab" : "shows no prompt (as before)");
}

// ---- the camouflage prompt hidden (trace 20261122_1800). ----
// [LOADBEARING: camo prompt hidden] (no key, always on in VR - like the grip camouflage block) the infected module's R1 offer never
//      returns action 53: the game's own no-camouflage answer instead, so no "RB Use Camouflage" prompt and no R1 camouflage; the wipe
//      gesture (camogesture.cpp) starts camouflage on its own path.
// COST: per R1-slot action query on an infected corpse: the original + one compare; the fallback call only where 53 would have been.
long long __fastcall CamoOfferHook(void* module, void* ctx)
{
    const long long r = g_origCamoOffer(module, ctx);
    if (static_cast<int>(r) != kCamoAction) { return r; }   // the callers read the action as a dword (lf6: `!(_DWORD)result`)
    if (t_inSlotUpdate <= 0) { return r; }                    // not the prompt build: the gather / the press / the AI keep 53
    const long long alt = reinterpret_cast<CamoOfferFn>(g_gd + g_rNoCamoAnswer)(module, ctx);
    if (InterlockedIncrement(&g_nCamoHidden) == 1) {
        // [PROBE: LOOTHAND] gate=always (no key)  site=HEADSET  log-only=no
        LOG("looting: [LOOTHAND] FIRST CAMOUFLAGE OFFER HIDDEN - the infected module %p offered action 53 (the skill is learned); "
            "returned %d instead (the game's own no-camouflage answer) | thread %lu", module, static_cast<int>(alt),
            GetCurrentThreadId());
    }
    return alt;
}

void InstallCamoOfferHide()
{
    int attempts = 0;
    const bool ok = InstallSingleGameDetour(g_gd + kInfectedSlot1Rva, kCamoOfferBytes, sizeof(kCamoOfferBytes),
                                            reinterpret_cast<void*>(&CamoOfferHook), reinterpret_cast<void**>(&g_origCamoOffer),
                                            "loot camouflage offer", &attempts);
    InterlockedExchange(&g_camoOfferState, ok ? 1 : -1);
    // [PROBE: LOOTHAND] gate=always (no key)  site=FLAT  log-only=yes
    LOG("looting: [LOOTHAND] CAMOUFLAGE PROMPT %s - the infected module's R1 offer (Steam +%llX, as loaded +%llX) %s (%zu bytes, %d "
        "attempt(s)); fallback +%llX as loaded",
        ok ? "HIDDEN" : "NOT HIDDEN", static_cast<unsigned long long>(kInfectedSlot1Rva), static_cast<unsigned long long>(g_rInfectedSlot1),
        ok ? (g_glyphState == 1 ? "detoured - action 53 is withheld from the 2D prompt only (inside HudTriggerSlot::Update); the "
                                  "gather and the press keep it"
                                : "detoured, but the slot-update detour is NOT live - nothing is hidden (the scope never opens)")
           : "NOT detoured - its first bytes differ, or the write was refused; the game's prompt stays",
        sizeof(kCamoOfferBytes), attempts, static_cast<unsigned long long>(g_rNoCamoAnswer));
}

void TryInstall()
{
    if (g_installState != 0) { return; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTryMs < 1000) { return; }
    g_lastTryMs = now;
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !en) {
        if (++g_tries == 1) {
            // [PROBE: LOOTHAND] gate=loot_by_hand  site=FLAT  log-only=no
            LOG("looting: [LOOTHAND] waiting - %s not mapped yet (retries once a second)", gd ? "engine_x64_rwdi.dll"
                                                                                                : "gamedll_x64_rwdi.dll");
        }
        return;
    }
    // the build's rvas first (Steam, or the Epic build's from buildrva_table.h), then the base the readers test
    g_rSetter = DLVR_LOOT_GD_RVA(kSetterRva);
    g_rBodyVt = DLVR_LOOT_GD_RVA(kBodyVtRva);
    g_rContVt = DLVR_LOOT_GD_RVA(kContainerVtRva);
    g_rSlotUpdate = DLVR_LOOT_GD_RVA(kSlotUpdateRva);
    g_rNoCamoAnswer = DLVR_LOOT_GD_RVA(kNoCamoAnswerRva);
    g_rAiTrigAct = DLVR_LOOT_GD_RVA(kAiTrigActRva);
    g_rInfectedSlot1 = DLVR_LOOT_GD_RVA(kInfectedSlot1Rva);
    g_rGather = DLVR_LOOT_GD_RVA(kGatherRva2);
    g_rVar = DLVR_LOOT_GD_RVA(kVarRva2);
    g_rUiGate = DLVR_LOOT_GD_RVA(kUiGateRva2);
    g_rObjGate = DLVR_LOOT_GD_RVA(kObjGateRva2);
    g_rGameDi = DLVR_LOOT_GD_RVA(kGameDiRva2);
    g_rVarAlt = DLVR_LOOT_GD_RVA(kVarAltRva2);
    MemoryBarrier();
    g_gd = reinterpret_cast<uintptr_t>(gd);
    g_extents = reinterpret_cast<ExtentsFn>(GetProcAddress(en, kExtentsExport));
    g_memGetId = reinterpret_cast<MemGetIdFn>(GetProcAddress(en, "?GetID@IGSObject@@QEBAHXZ"));
    g_memFind = reinterpret_cast<MemFindFn>(GetProcAddress(en, "?FindObjectByID@IGSObject@@UEBAPEAV1@H@Z"));
    g_setter = reinterpret_cast<SetterFn>(g_gd + g_rSetter);
    if (!g_extents) {
        InterlockedExchange(&g_installState, -1);
        // [PROBE: LOOTHAND] gate=loot_by_hand  site=FLAT  log-only=no
        LOG("looting: [LOOTHAND] REFUSED - the engine does not export %s; loot by hand stays off this launch", kExtentsExport);
        return;
    }
    int attempts = 0;
    void* tramp = nullptr;
    const bool ok = InstallSingleGameDetour(g_gd + kNearbyRva, kNearbyBytes, sizeof(kNearbyBytes),
                                            reinterpret_cast<void*>(&NearbyHook), &tramp, "loot GetNearbyTriggers", &attempts);
    if (!ok) {
        InterlockedExchange(&g_installState, -1);
        // [PROBE: LOOTHAND] gate=loot_by_hand  site=FLAT  log-only=no
        LOG("looting: [LOOTHAND] REFUSED - the GetNearbyTriggers detour at gamedll +%llX (Steam rva) was not installed (its first "
            "%zu bytes differ from the Steam / Epic builds these were read from, or the write was refused; %d attempt(s)). Loot "
            "by hand stays off this launch; X looting is unchanged.",
            static_cast<unsigned long long>(kNearbyRva), sizeof(kNearbyBytes), attempts);
        return;
    }
    g_origNearby = reinterpret_cast<NearbyFn>(tramp);
    InterlockedExchange(&g_installState, 1);
    // [PROBE: LOOTHAND] gate=loot_by_hand  site=FLAT  log-only=no
    LOG("looting: [LOOTHAND] READY - GetNearbyTriggers (Steam +%llX) detoured (%zu bytes verified, %d attempt(s)), setter +%llX, "
        "body vftable +%llX, plain Container vftable +%llX, AI slot 179 +%llX, camouflage slot +%llX (as loaded; Steam %s), extents "
        "%p | reach %.2f m, still < %.2f m/s, settle %.2f s | remembered bodies: GetID %p FindObjectByID %p (%s)",
        static_cast<unsigned long long>(kNearbyRva), sizeof(kNearbyBytes), attempts,
        static_cast<unsigned long long>(g_rSetter), static_cast<unsigned long long>(g_rBodyVt),
        static_cast<unsigned long long>(g_rContVt),
        static_cast<unsigned long long>(g_rAiTrigAct), static_cast<unsigned long long>(g_rInfectedSlot1),
        g_rSetter == kSetterRva ? "values" : "values translated - the Epic build",
        reinterpret_cast<void*>(g_extents), LootReachM, LootStillSpeed, LootSettleS, reinterpret_cast<void*>(g_memGetId),
        reinterpret_cast<void*>(g_memFind),
        g_memGetId && g_memFind ? "on - a searched body stays grabbable" : "OFF - an export is missing; a searched body leaves the list");
    // the camouflage prompt is hidden always (the grip block that makes it name a dead input is always on). The slot-update detour
    // installs FIRST and always: it is what scopes the hide to the prompt (t_inSlotUpdate); its grip-glyph part checks loot_by_hand.
    InstallGripGlyph();
    InstallLidPrompt();   // after the slot-update detour: its glyph / text half lives in SlotUpdateHook
    InstallCamoOfferHide();
}

void LogTotals()
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 5000) { return; }
    last = now;
    LOG_PERIODIC("looting: [LOOTHAND] 5 s totals (session) | install %ld | hook calls %ld, picks set %ld, passed through %ld, "
                 "armed with no body in reach %ld, read faults %ld | ARM %ld, NO-ARM %ld, SEARCH %ld, END grip opened %ld / moved "
                 "%ld / body gone %ld | DRAG %ld, drag END %ld | bodies gathered now %d | lid prompt %ld: handed %ld, grab text %ld "
                 "(faults %ld)",
                 static_cast<long>(g_installState), static_cast<long>(g_nHook), static_cast<long>(g_nSub),
                 static_cast<long>(g_nPass), static_cast<long>(g_nNoBody), static_cast<long>(g_nFault),
                 static_cast<long>(g_nArm), static_cast<long>(g_nNoArm), static_cast<long>(g_nSearch),
                 static_cast<long>(g_nEndOpen), static_cast<long>(g_nEndMoved), static_cast<long>(g_nEndGone),
                 static_cast<long>(g_nDrag), static_cast<long>(g_nEndDrag), g_boxCount, static_cast<long>(g_pickGetState),
                 static_cast<long>(g_nLidPrompt), static_cast<long>(g_nGrabText), static_cast<long>(g_nGrabTextFault));
}

}  // namespace

// COST per poll while loot_by_hand=1: per hand one squeeze read, the grip / head poses and the camera matrix (cached reads, the
// throwables' mapping), a distance to <= 16 boxes; one speed-window push + a backward scan of <= 128 samples (stops at the first
// >= 100 ms old, ~57 at 570 polls/s); one machine step; with a body in reach, ContainerState (<= 5 guarded reads) and with
// body_grab two lock-free hand-offs (BodyGrabPublishCandidate / BodyGrabTorsoOf - the bone reads run on the game thread). Off: one
// bool test.
void LootPoll(bool vrGrip, bool leftOwned, bool rightOwned, bool& withholdL1, bool& withholdR1)
{
    withholdL1 = false;
    withholdR1 = false;
    if (!LootByHand) {
        g_lootHand = -1;
        return;
    }
    if (g_installState == 0) { TryInstall(); }
    LogTotals();
    if (g_installState != 1 || !vrGrip) {
        g_lootHand = -1;
        return;
    }
    // body_grab rides this machine (dragEnabled). Off: every body-grab input below stays at its default and nothing is published.
    const looting::Params prmBase{ LootReachM, LootStillSpeed, LootSettleS, BodyGrab };
    const bool blocked = BodyGrab && (OverlayVisible() || CalibScreenActive());   // the throwables' menu predicate (throwable.cpp)
    const double now = NowS();
    static bool s_firstSample = false;
    LONG active = -1;
    for (int hi = 0; hi < 2; ++hi) {
        const Hand h = hi == 0 ? kLeftHand : kRightHand;
        float sq = 0.0f;
        const bool grip = GetControllerSqueeze(h, sq) && sq >= kGripThreshold;
        float pos[3] = {}, trk[3] = {}, rot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const bool havePos = HandWorld(h, pos, trk, rot);
        // D-269: speed over a ~100 ms window, never a per-poll delta. The window restarts on the press edge, so the reach onto the
        // body before the squeeze does not spend the press; until 50 ms of samples are held it reads -1 and the machine sees 0.
        if (grip && !g_prevGripDown[hi]) {
            g_speed[hi].Reset();
            // 10-24 (2) (Jay: catching a thrown part): a fresh severed-limb query at the press, and the press waits for it
            if (BodyGrab) { BodyGrabRequestRelict(hi); }
        }
        g_prevGripDown[hi] = grip;
        if (havePos) { g_speed[hi].Push(now, trk); } else { g_speed[hi].Reset(); }
        const float winSpeed = g_speed[hi].Speed();
        const float speed = winSpeed < 0.0f ? 0.0f : winSpeed;
        float dist = -1.0f, distOwn = -1.0f, distCont = -1.0f;
        void* body = nullptr;
        bool isPlain = false;   // a plain search Container (trace 20261122_1620): no body-grab reading, never a drag
        if (havePos && NearestBox(pos, &dist, &body, &distOwn, &distCont, &isPlain) < 0) { dist = -1.0f; }
        if (BodyGrab && havePos) { BodyGrabPublishHand(hi, pos, rot, now); }
        CamoGesturePadPoll(hi, trk, havePos, now);   // the camouflage wipe (camo_gesture off: one bool test)
        // Severed limbs (Jay: "Yes please"): bodygrab looks for them on the game thread while this grip is squeezed; one nearer than
        // the gathered body becomes the press's body - never searchable, never the torso, so a press on it grabs.
        if (BodyGrab) { BodyGrabPublishSqueeze(hi, sq); }
        const float outerReach = LootReachM > BodyGrabReachM ? LootReachM : BodyGrabReachM;
        // The gathered body stays the candidate while a limb is near, so its hitbox reading keeps coming (10-24 (2)).
        if (BodyGrab) {
            BodyGrabPublishCandidate(hi, (havePos && body && !isPlain && dist >= 0.0f && dist <= outerReach) ? body : nullptr);
        }
        bool isRelict = false;
        float corpseHit = -1.0f;
        float limbD = -1.0f;   // the fresh severed-limb reading for this hand (-1 = none), logged with the press
        if (BodyGrab && havePos) {
            // 10-24 (2): a limb and a corpse are compared by the same measure - each one's hitbox surface - never the limb against
            // the corpse's box (a hand inside a lying corpse's box reads 0 there, and that box still spans a part cut off it:
            // hitbox_dump_run1) nor its skeleton (it keeps a cut part's bones). A corpse with no hitbox reading (ragdoll released)
            // never beats a limb within the margin. A tie goes to the limb.
            corpseHit = (body && !isPlain) ? BodyGrabHitOf(hi, body) : -1.0f;
            float rd = -1.0f;
            void* rel = BodyGrabRelictOf(hi, &rd);
            limbD = rel ? rd : -1.0f;
            // Jay 2026-10-27 after run 10 ("Alright, go ahead"): a hand INSIDE an unsearched corpse's own box belongs to that corpse -
            // the bone rule above decides search or drag; a loose severed limb lying there does not take the press (run 10: 9 of 17
            // grabs were such limbs in a crowd). A searched body, or a hand outside the box: the limb comparison as before.
            const bool inUnsearchedCorpse = body && distOwn >= 0.0f && distOwn <= 0.02f && ContainerState(body) == static_cast<int>(kStateSearchable);
            if (rel && !inUnsearchedCorpse && rd >= 0.0f && rd <= BodyGrabHitboxMarginM && (corpseHit < 0.0f || rd <= corpseHit)) {
                body = rel;
                dist = rd;
                distOwn = rd;
                distCont = -1.0f;
                isRelict = true;
                isPlain = false;
            }
        }
        const bool inReach = havePos && body && dist >= 0.0f && dist <= outerReach;
        const looting::State before = g_machine[hi].state();
        const looting::State other = g_machine[1 - hi].state();
        // spec 6b.3: "torso" = within loot reach of the attached container's box (spine2). No container box = a standalone
        // container, which is loot only. Away from the torso the press reaches as far as body_grab_reach_m.
        // 10-24 (2) (Jay: "Do 1"): with body_grab the skeleton decides when it has a fresh reading - a spine / pelvis bone under the
        // hand is the torso, any other bone is not; no reading = the box rule. And a body already searched is never the torso
        // (Jay: "a body can only be searched for loot a singular time"): its press grabs.
        const bool boxTorso = !isRelict && (distCont < 0.0f || distCont <= LootReachM);
        const int skelTorso = (BodyGrab && inReach && !isPlain) ? BodyGrabTorsoOf(hi, body) : -1;
        const int cstate = inReach ? ContainerState(body) : -1;
        // a plain container (trace 20261122_1620) is searchable at state 1 EXACTLY - closed (2), locked (10), searched (5) or unread
        // keep the game's own X; a body as before (unread = searchable)
        const bool searchable = isPlain ? cstate == static_cast<int>(kStateSearchable)
                                        : (cstate < 0 || cstate == static_cast<int>(kStateSearchable));
        const bool onTorso = (skelTorso >= 0 ? skelTorso == 1 : boxTorso) && searchable;
        looting::Params prm = prmBase;
        if (isPlain) { prm.dragEnabled = false; }   // a press on a bag / bin / payphone never becomes a body-grab drag
        if (BodyGrab && !onTorso && !isPlain) { prm.reachM = BodyGrabReachM; }
        // a press that finds nothing while the severed-limb search is still due waits for it (looting_logic Params.lateArmS)
        if (BodyGrab) { prm.lateArmS = kLateArmS; }
        looting::Input in;
        in.gripDown = grip;
        // A drag owns its body: BodyGrabTick re-validates it every player update and names an END there; the machine keeps the
        // grip withheld until it opens, whatever happened to the body (a body vanishing mid-squeeze must not send L1 / R1).
        in.bodyDist = before == looting::State::Dragging ? 0.0f : (havePos ? dist : -1.0f);
        // without body_grab a searched body offers nothing: the press does not arm ("no body"); a plain container likewise, always
        if ((!BodyGrab || isPlain) && !searchable && before == looting::State::Idle) { in.bodyDist = -1.0f; }
        in.handSpeed = speed;
        in.nowS = now;
        in.gripOwnedElsewhere = hi == 0 ? leftOwned : rightOwned;
        in.otherHandBusy = other == looting::State::Settling || other == looting::State::Searching;
        in.onTorso = onTorso;
        in.blocked = blocked;
        in.limbPending = BodyGrab && BodyGrabRelictPending(hi);
        const looting::Output o = g_machine[hi].Step(in, prm);
        if (!s_firstSample && hi == 1) {
            s_firstSample = true;
            // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=yes
            LOG("looting: [LOOTHAND] first poll | right hand %s (%.3f, %.3f, %.3f) | grip %.2f | nearest body %.3f m (%d gathered)",
                havePos ? "at" : "NOT MAPPED (no camera / yaw / pose yet)", pos[0], pos[1], pos[2], sq, dist, g_boxCount);
        }
        if (o.event) {
            const char* ev = o.event;
            if (ev[0] == 'A') { InterlockedIncrement(&g_nArm); }
            else if (ev[0] == 'S') { InterlockedIncrement(&g_nSearch); }
            else if (ev[0] == 'D') { InterlockedIncrement(&g_nDrag); }
            else if (before == looting::State::Dragging) { InterlockedIncrement(&g_nEndDrag); }
            else if (ev[4] == 'g' && ev[5] == 'r') { InterlockedIncrement(&g_nEndOpen); }
            else if (ev[4] == 'm') { InterlockedIncrement(&g_nEndMoved); }
            else { InterlockedIncrement(&g_nEndGone); }
            // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=no
            LOG("looting: [LOOTHAND] %s | %s hand | body %p (%s) %.3f m (own box %.3f, container box %.3f; -1 = none) | hand speed "
                "%.2f m/s over ~100 ms (-1 = under 50 ms of samples since the press) | grip %.2f | torso %d (body_grab %d) - box rule %d, "
                "skeleton %d (-1 = no fresh reading), container state %d (%s) | gathered body's hitbox %.3f m (-1 = no fresh "
                "reading) | nearest severed limb %.3f m (-1 = no fresh reading) | waited %.0f ms for the limb reading (-1 = no wait)",
                ev, hi == 0 ? "left" : "right", body, isRelict ? "a severed limb" : (isPlain ? "a plain search container" : "gathered"),
                dist, distOwn, distCont, winSpeed, sq,
                onTorso ? 1 : 0, BodyGrab ? 1 : 0,
                boxTorso ? 1 : 0, skelTorso, cstate, cstate < 0 ? "unread - searchable as before" : searchable ? "searchable"
                                                                                                             : "already searched",
                corpseHit, limbD, o.waitedS < 0.0 ? -1.0 : o.waitedS * 1000.0);
            // body grab: the press became a drag -> queue the grab of the body under the hand; any END of a drag -> stop pushing.
            if (ev[0] == 'D') {
                BodyGrabBegin(hi, body, pos);
            } else if (before == looting::State::Dragging) {
                BodyGrabEnd(hi, ev);
            }
        }
        if (o.noArm) {
            const LONG k = InterlockedIncrement(&g_nNoArm);
            if (k <= kMaxNoArmLines) {
                // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=yes
                LOG("looting: [LOOTHAND] NO-ARM #%ld - %s hand press did not arm: %s | hand %s | nearest body %.3f m (own box "
                    "%.3f, container box %.3f; reach %.2f, %d gathered) | grip %.2f | holster / two-handed owns it %d, other hand "
                    "looting %d | hook calls %ld, last gather: %ld candidates, %ld standalone containers, %ld AI (%ld with a "
                    "container) | container state %d (1 = searchable, -1 = unread) | nearest severed limb %.3f m (-1 = no fresh "
                    "reading) | waited %.0f ms for the limb reading (-1 = no wait)%s",
                    k, hi == 0 ? "left" : "right",
                    (!havePos && o.noArm[0] == 'n') ? "hand NOT MAPPED (no camera / yaw / pose, or > 1.5 m from the head)" : o.noArm,
                    havePos ? "mapped" : "NOT MAPPED", dist, distOwn, distCont, LootReachM, g_boxCount, sq,
                    static_cast<int>(in.gripOwnedElsewhere), static_cast<int>(in.otherHandBusy), static_cast<long>(g_nHook),
                    static_cast<long>(g_lastCand), static_cast<long>(g_lastStandalone), static_cast<long>(g_lastAi),
                    static_cast<long>(g_lastAiBody), cstate, limbD, o.waitedS < 0.0 ? -1.0 : o.waitedS * 1000.0,
                    k == kMaxNoArmLines ? " | LAST NO-ARM line this session" : "");
            }
        }
        if (o.withholdGrip) {
            if (hi == 0) { withholdL1 = true; } else { withholdR1 = true; }
            // a drag never re-picks the game's trigger: only a settling / searching hand hands its body to NearbyHook (a press still
            // waiting for the limb reading has no body yet)
            if (active < 0 && !o.dragging && o.state != looting::State::Waiting) {
                active = hi;
                if (havePos) {
                    InterlockedIncrement(&g_handSeq);
                    g_handPos[0] = pos[0]; g_handPos[1] = pos[1]; g_handPos[2] = pos[2];
                    InterlockedIncrement(&g_handSeq);
                }
            }
        }
    }
    g_lootHand = active;
}

// Hip yield (spec 2026-10-26-roomscale-physical-crouch-design §2.4, plan Task 4): the same reach and limb rule LootPoll applies, read
// for the hip holsters BEFORE LootPoll runs this poll (same scePad thread). Yes = a gathered body within the press's outer reach
// (loot_reach_m, or body_grab_reach_m when larger and body_grab is on) - without body_grab only a searchable one, since LootPoll gives
// an already-searched body nothing - or a fresh severed-limb reading within body_grab_hitbox_margin_m. Pending = body_grab on, none
// of those, and the limb reading still due: the press poll itself (LootPoll asks for the fresh query later in this poll) or
// BodyGrabRelictPending for < kLateArmS. COST: called only on a hip press edge and while that press is Pending (<= 150 ms) - one
// HandWorld (cached reads), one NearestBox (<= 16 boxes), ContainerState (<= 5 guarded reads) with a body in reach, two lock-free reads.
// Hand climbing P1 (climbhand.cpp, docs/design_climbing_overhaul.md §6): the same hand-in-game-world HandWorld gives loot, so a
// climbing grab and a loot grab measure reach from the same point. Pad thread. hand 0 = LEFT, 1 = RIGHT.
bool LootHandWorldPos(int hand, float out[3])
{
    float trk[3], rot[4];
    return HandWorld(hand == 0 ? kLeftHand : kRightHand, out, trk, rot);
}
holster::Yield LootYieldAt(int hand, bool pressEdge, float* outDist, bool* outLimb, double* outWaitS)
{
    static double s_pressS[2] = {};
    if (outDist) { *outDist = -1.0f; }
    if (outLimb) { *outLimb = false; }
    if (outWaitS) { *outWaitS = -1.0; }
    if (hand < 0 || hand > 1) { return holster::Yield::No; }
    const double now = NowS();
    if (pressEdge) { s_pressS[hand] = now; }
    const double waited = now - s_pressS[hand];
    if (outWaitS) { *outWaitS = waited; }
    if (!LootByHand || g_installState != 1) { return holster::Yield::No; }
    float pos[3] = {}, trk[3] = {}, rot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!HandWorld(hand == 0 ? kLeftHand : kRightHand, pos, trk, rot)) { return holster::Yield::No; }
    const float reach = (BodyGrab && BodyGrabReachM > LootReachM) ? BodyGrabReachM : LootReachM;
    float dist = -1.0f, distOwn = -1.0f, distCont = -1.0f;
    void* body = nullptr;
    bool isPlain = false;
    if (NearestBox(pos, &dist, &body, &distOwn, &distCont, &isPlain) >= 0 && body && dist >= 0.0f &&
        dist <= (isPlain ? LootReachM : reach)) {
        const int cs = ContainerState(body);
        // a plain search container (trace 20261122_1620) takes the hip press only while LootPoll would search it: state 1 exactly
        if (isPlain ? cs == static_cast<int>(kStateSearchable) : (BodyGrab || cs < 0 || cs == static_cast<int>(kStateSearchable))) {
            if (outDist) { *outDist = dist; }
            return holster::Yield::Yes;
        }
    }
    if (!BodyGrab) { return holster::Yield::No; }
    float rd = -1.0f;
    void* rel = BodyGrabRelictOf(hand, &rd);
    if (rel && rd >= 0.0f && rd <= BodyGrabHitboxMarginM) {
        if (outDist) { *outDist = rd; }
        if (outLimb) { *outLimb = true; }
        return holster::Yield::Yes;
    }
    if (pressEdge || (BodyGrabRelictPending(hand) && waited < kLateArmS)) { return holster::Yield::Pending; }
    return holster::Yield::No;
}

// COST per poll: one squeeze read, two atomic reads, one tick read, one latch step. The detour install runs at most once a second until
// it settles (READY / REFUSED).
void GripCamoPoll(bool vrGrip, bool& withholdR1)
{
    withholdR1 = false;
    if (!vrGrip) { return; }
    if (g_installState == 0) { TryInstall(); }
    float sq = 0.0f;
    const bool down = GetControllerSqueeze(kRightHand, sq) && sq >= kGripThreshold;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG age = now - static_cast<ULONGLONG>(g_camoTickMs);
    const bool target = g_installState == 1 && g_camoPick != 0 && age <= kCamoFreshMs;
    static bool s_prev = false;
    const bool w = g_camoLatch.Step(down, target);
    if (w && !s_prev) {
        const LONG k = InterlockedIncrement(&g_nCamoWithheld);
        if (k <= 40) {
            // [PROBE: GRIPCAMO] gate=always (no key)  site=HEADSET  log-only=no
            LOG("looting: [GRIPCAMO] R1 WITHHELD #%ld - right-grip press made while the game's pick %p is an infected corpse "
                "(camouflage) - the press is not sent as R1 until the grip opens | pick %llu ms old | grip %.2f%s",
                k, g_camoObj, static_cast<unsigned long long>(age), sq, k == 40 ? " | LAST line of this kind" : "");
        }
    }
    s_prev = w;
    withholdR1 = w;
}

// 2026-11-22 (71) (Jay: Survivor Sense "can currently fire from a grip being held on a body"; INV-765, trace 20261122_1620): Square
// only as the search's START tap (looting::kSearchTapS); the hold after it is the input lookup reading action 20 held for the local
// player (boltwheel.cpp LookupHook), stamped here every poll. Survivor Sense (X held 0.5 s) and the other X holds never see a hold.
// With the lookup detour not live: Square for the whole search, as before. COST: one QPC read, two state reads, three interlocked
// stores per poll.
void LootPadByte(unsigned char* p)
{
    if (!LootByHand || !p) { return; }
    static bool s_prevSearch[2] = {};
    static double s_startS[2] = { -1.0, -1.0 };
    const double now = NowS();
    bool searching = false;
    double since = -1.0;
    for (int hi = 0; hi < 2; ++hi) {
        const bool s = g_machine[hi].state() == looting::State::Searching;
        if (s && !s_prevSearch[hi]) { s_startS[hi] = now; }
        s_prevSearch[hi] = s;
        if (s) {
            searching = true;
            since = now - s_startS[hi];
        }
    }
    const unsigned long long target = static_cast<unsigned long long>(g_lootTarget);
    const bool live = InputLookupTriggerHoldLive() && target != 0;
    InputLookupSetTriggerHold(searching && live, target);
    if (looting::SquareForSearch(searching, since, live)) {
        *reinterpret_cast<uint32_t*>(p) |= 0x8000u;   // Square = the game's X (_ACTION_EXECUTE_TRIGGER, inputs_pad.scr:56)
    }
    if (searching && InterlockedCompareExchange(&g_nTapSaid, 1, 0) == 0) {
        // [PROBE: LOOTHAND] gate=loot_by_hand  site=HEADSET  log-only=yes
        LOG("looting: [LOOTHAND] FIRST SEARCH X - %s (lookup hold %s, target %p)",
            live ? "a start tap only, the search held by the input lookup" : "X held for the whole search (Survivor Sense can fire)",
            live ? "LIVE" : (target == 0 ? "no target yet" : "NOT LIVE"), reinterpret_cast<void*>(target));
    }
}

bool LootInfectedModule(uintptr_t ai, uintptr_t* module)
{
    return InfectedModule(ai, module);
}

}  // namespace dlvr
