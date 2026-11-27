// Loot by hand - docs/superpowers/specs/2026-10-23-loot-by-hand-design.md (plan docs/superpowers/plans/2026-10-23-loot-by-hand.md,
// Task 3). Grip and hold a hand on a dead body's torso: the game's own body search runs on THAT body (INV-614 / INV-615). The
// decision is looting_logic.h (unit-tested); looting.cpp feeds it on the scePad poll, ORs Square while searching, withholds the
// grip's L1 / R1, and re-picks the HAND's body inside the game's GetNearbyTriggers (+B9BBB0). The kSettings rows are in ovr2xr.cpp
// beside the throwable_* rows.
#pragma once

#include <cstdint>

#include "holster_logic.h"   // holster::Yield (LootYieldAt)

namespace dlvr {

extern bool  LootByHand;       // loot_by_hand - master. DEFAULT ON (was OFF, Tier C).
extern float LootReachM;       // loot_reach_m - hand to the body's world box
extern float LootStillSpeed;   // loot_still_speed - m/s, the hand counts as still below it
extern float LootSettleS;      // loot_settle_s - still this long before the search starts
extern bool  DiagLootWhy;      // diag_loot_why - the [LOOTWHY] probe (log-only, DEV): why a body gets no loot prompt

// Once per scePad poll, AFTER HolsterPoll / HolsterLeftPoll / TwoHandPoll and BEFORE the grip block. The *Owns flags = those already
// took this press of that grip. withholdL1 / withholdR1 come back true while that hand is settling or searching on a body.
// loot_by_hand=0 -> returns at its first test, both false.
void LootPoll(bool vrGrip, bool leftOwned, bool rightOwned, bool& withholdL1, bool& withholdR1);

// Hip yield (spec 2026-10-26-roomscale-physical-crouch-design §2.4): from HolsterPoll / HolsterLeftPoll (the same scePad poll, BEFORE
// LootPoll), on a hip press edge (pressEdge true - starts the wait clock) and while that press is Pending. hand 0 = LEFT, 1 = RIGHT.
// Yes = a body / severed limb under the hand takes the press; Pending = the limb reading is due; No = the holster fires (also
// loot_by_hand off / not installed / no hand pose). outDist = that body's / limb's distance (-1 = none), outLimb = it is a limb,
// outWaitS = seconds since the edge. (A lid / chest item under the hand is asked of lidhand by the hips BEFORE this - trace 20261119_1240.)
holster::Yield LootYieldAt(int hand, bool pressEdge, float* outDist, bool* outLimb, double* outWaitS);

// The hand in game world (looting.cpp HandWorld: camera + yaw-mapped grip offset from the reported head). Pad thread. hand 0 = LEFT,
// 1 = RIGHT. False when the camera / head / grip is not known this poll. Used by hand climbing (climbhand.cpp).
bool LootHandWorldPos(int hand, float out[3]);
// Right grip camouflage block (Jay 10-23 (3), option 1; no key - always on): once per scePad poll, AFTER LootPoll and BEFORE the grip
// block. withholdR1 comes back true for a right-grip press made while the game's current pick is an infected corpse the R1 slot
// would camouflage on (INV-616 follow-up); buggy reset / traders / anything else keep R1. Installs the shared GetNearbyTriggers detour.
void GripCamoPoll(bool vrGrip, bool& withholdR1);

// Once per scePad poll, right after ThrowablePadByte and BEFORE the mod-menu block (which zeroes the pad last): ORs Square
// (0x8000, the game's X = _ACTION_EXECUTE_TRIGGER) as a hand's search START tap (0.15 s) and holds the search through the input
// lookup (boltwheel.h InputLookupSetTriggerHold); without the lookup detour, Square for the whole search (trace 20261122_1620).
void LootPadByte(unsigned char* p);

// The camouflage gesture (camogesture.cpp, plan 2026-11-07-camouflage-gesture): is `ai` an AI whose logic module is the infected one
// (the test PickIsCamoTarget applies to the pick, INV-704)? *module = that module (its +408 is the game's camouflage time). Any
// thread; guarded reads only. False before loot by hand is installed (the rvas are resolved there).
bool LootInfectedModule(uintptr_t ai, uintptr_t* module);

}  // namespace dlvr
