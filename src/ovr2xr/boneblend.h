#pragma once

#include <cstdint>   // uintptr_t, used by the [ELEMDUMP] declarations below

// Read-only probe on the engine's own per-bone pose blend.
//
// This is rigging step 1. Everything before it (stages 1-4, all the
// SetElement*Matrix work) is retired: `engine+0xE5DA0` recomputes every bone
// from the animated pose every frame, so a write made from anywhere outside it
// is overwritten before render. The fix is to write inside that pipeline, which
// first means proving we can identify our bones from within it.
//
// Two hooks, both plain inline detours, both read-only here:
//
//   engine+0xE55C0  outer  void(animState, stackAlloc, outBase)
//                          Runs once per skeleton per frame. Hands us
//                          `outBase` (param_3) and the bone count
//                          (*(int*)(animState+0x2d8)).
//
//   engine+0xE5DA0  inner  void(float* dest, float weight, float* destAgain,
//                              float* src, char flag, unsigned int mask)
//                          Runs once per bone per animation layer.
//                          boneIndex = (dest - outBase) / 0x30.
//
// The probe answers four things in one session: the real call rate of both
// (this hook runs for EVERY character's skeleton, not just the player's), that
// the 0x30 stride and index derivation actually hold, which skeleton is the
// player's, and whether the blend array's index space matches the mesh-element
// one we already mapped in stage 0.
//
// It writes nothing to the game. Ever.

namespace dlvr {

// Settings (registered in ovr2xr.cpp, default off).
// MASTER ENABLE for the rig, despite the name - without it nothing installs and
// the arm reverts to animation. Not a logging flag.
extern bool DiagBoneBlendProbe;
// The rig's own logging, separate so the arm can run silently for timing runs.
extern bool DiagBoneBlendVerbose;
// Arms a DR0 data breakpoint on the player's r_forearm WORLD matrix in bounded
// windows, logging every distinct writer alongside the weapon held. Off by
// default - it suspends threads to arm, so it is not free.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagWorldWriter = false;
#else
extern bool DiagWorldWriter;
#endif
// Read-only probe for the anti-wall deformation - the candidate for what
// overrides the arm on firearms. Reads the live deformation weight, the weapon
// category it is gated on, and which callers construct projectiles. Writes
// nothing to the game.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool AntiWallProbe = false;
#else
extern bool AntiWallProbe;
#endif
// Write the solved bones at the player-update TAIL (gamedll+0xBFC170), strictly
// later in the frame than anything else we do, instead of disabling things earlier.
// Four attempts at disabling all failed.
extern bool  RigWriteAtPlayerTail;
// Take the arm bones off the animation system via Block(i). DISPROVEN as a fix:
// it works mechanically, but Block(i) also sets bit 0x2 on the bone's mesh
// element and that bit is load-bearing for the block itself - set, the block
// holds and the arm is invisible; cleared, the arm draws and the animation
// competes again. Kept for the record and for further probing, not as the answer.
extern bool  RigBlockArmBones;
// Clears the render bit Block(i) sets, which otherwise makes both arms vanish.
extern bool  RigBlockKeepVisible;
// Re-derive the IK anchor from the shoulder when the eye bone (element 8, an
// animated bone) goes anatomically implausible. Measured |eye - shoulder| of up
// to 1.11 m on a rig whose whole arm is 0.546 m, against a rock-steady 0.302 m
// with a melee weapon held.
extern bool  DiagFastIkGlobals;
extern bool  RigFinalGateHooks;
extern bool  RigWriteAtFinalGate;
extern bool  RigSuppressStretch;
extern bool  RigRegripButton;
extern bool  RigHandGripPose;
extern bool  RigGripComposeRest;
extern bool  RigGripUseBaked;
extern bool  RigStableShoulder;
extern bool  RigShoulderFreezeDir;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagBodyYaw = false;
#else
extern bool  DiagBodyYaw;
#endif
// Localise the shoulder displacement in the parent chain by measuring the bone
// SEPARATIONS, whose rest lengths the rig already gives us, rather than the
// endpoints. Pure read, right arm only, once a second.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagDeformProbe = false;
inline constexpr bool DiagShoulderChain = false;
#else
extern bool DiagDeformProbe;
extern bool DiagShoulderChain;
#endif
// The parent chain ABOVE hspine1 - the never-measured half. Read-only, 1 Hz,
// with a per-poll locomotion proxy because the fault only appears while moving.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagSpineChain = false;
#else
extern bool DiagSpineChain;
#endif
// The 8-slot animation layer stack and its active mask at E+0x1900. Read-only,
// 1 Hz plus an immediate log on any change of the active set.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagAnimLayers = false;
inline constexpr bool DiagAnimMap = false;
inline constexpr bool DiagItemModel = false;
inline constexpr bool DiagHolderId = false;
inline constexpr bool DiagElemDump = false;
inline constexpr bool DiagRelChurn = false;
#else
extern bool DiagAnimLayers;
extern bool DiagAnimMap;
extern bool DiagItemModel;
extern bool DiagHolderId;
extern bool DiagElemDump;
extern bool DiagRelChurn;
#endif
extern bool DiagFlatRef;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagFastIkWeight = false;
inline constexpr bool DiagItemElemDump = false;
inline constexpr bool DiagHhCompose = false;
#else
extern bool DiagFastIkWeight;
extern bool DiagItemElemDump;
extern bool DiagHhCompose;
#endif
extern bool RigHhFlagHandshake;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagItemPlace = false;
inline constexpr bool DiagRightThumbCapture = false;
inline constexpr bool DiagBowStringProbe = false;
#else
extern bool DiagItemPlace;
extern bool DiagRightThumbCapture;   // diag_right_thumb_capture - log the right thumb's runtime pose while holding melee (INV-516)
extern bool DiagBowStringProbe;      // diag_bow_string_probe - the bow string / nock / limb / power WRITE TEST (INV-541)
#endif
// The bow string probe: once per player update from HookTail, after PublishBowRay. Off = one bool test.
void BowStringProbe(void* ent);
// bow_manual_draw (SPEC-MANUAL-BOW-DRAW-2026-10-12): the grab gate, the hand-driven string + nock, the draw power, the cancel.
extern bool  BowManualDraw;    // bow_manual_draw   - master. DEFAULT ON (Tier A, Jay 2026-10-12: "I want it on please").
extern float BowDrawZoneM;     // bow_draw_zone_m   - grab radius round the nock (m)
extern float BowDrawFullM;     // bow_draw_full_m   - the pull that gives power 1.0 (m)
extern float BowDrawMaxM;      // bow_draw_max_m    - how far the string may travel (m)
extern float BowDrawCancelM;   // bow_draw_cancel_m - R2 released under this pull = the game's cancel, arrow kept (m)
void BowDrawTick(void* ent);                           // HookTail, BEFORE PublishBowRay. Off = one bool test.
unsigned char BowDrawPadByte(unsigned char in);        // pad thread: the R2 byte the GAME is to read
int  BowItemElemId(uintptr_t item, const char* name);  // bow_manual_nock: element id by name, -1 = none (game thread)
bool BowItemElemWorld(uintptr_t item, int id, float out12[12]);   // that element's world 3x4 (game thread)
bool BowDrawFingerT(float* t);                         // the right hand's bow-pose blend 0..1; false = authored pins
extern float BowDrawLineM;     // bow_draw_line_m   - how far off the string's line the real hand may be and still pull (m)
// The RIGHT SolveArmIk while the bow's string is grabbed (v3: the hand is LOCKED TO THE NOCK in the game's own draw pose).
// Rot: cWorld is slerped to the one whose DRAWN hand (cWorld x grip x post) is the nock's world rotation x the game's hand-in-
// nock rotation. Pos: handPos = the hand bone's position for that pose with the nock at the pulled point; the pull follows the
// real hand ALONG the bow's line only while it is within bow_draw_line_m of that line, else it HOLDS (Jay: option A).
// False = not grabbed / no fresh bow frame: the solve is left alone. One seqlock read each.
// The RIGHT arm's elbow while the string is grabbed: the bow's own elbow direction in the world (a constant in the bow's frame,
// from the game's hip draw) and the grab's blend. False = not grabbed / no fresh bow frame. Note: per-draw statistics for the
// log (deg = how far the body rule's pole was from it; applied false = the direction lay along the arm and was skipped).
bool BowDrawElbowHint(float hintWorld[3], float* t);
void BowDrawElbowNote(float deg, bool applied);
bool BowDrawPinRot(const float grip9[9], const float post9[9], float cWorld[9]);
bool BowDrawPinPos(const float gripCmd[3], float handPos[3], float* t);
extern bool  ItemPlaceFromController;
extern int   ItemPlaceBasis;
extern bool  ItemPlaceRel;
extern bool  DiagOrphanRel;
extern bool  ItemPlaceOrphans;
extern bool  ItemPlaceHolder0;   // item_place_holder0 - INV-492 (holder_0 meshes)
extern bool  ItemPlaceVirtualHolder;   // item_place_virtual_holder - INV-492/493 (meshes with no grip element)
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagDescParent = false;
#else
extern bool  DiagDescParent;
#endif
// [ROOTTRK]. Read-only. The ITEM ROOT CONTROL OBJECT's world pose measured
// against the handholder and the hand bone, every frame, flat and VR. INV-198's
// candidate: the root may not track the hand in VR at all, with
// CorrectItemPosition's per-frame `holder` rewrite masking it on every weapon
// that has a holder - which would make the guitar the only one showing it.
extern bool  DiagRootTrack;
// [COMPOSE]. Read-only and FULLY PASSIVE - never calls the world accessor,
// because that accessor IS dl_WorldResolver and would rebuild the world from the
// local, making the test agree by construction. Checks parentWorld * ownLocal ==
// ownWorld on the arm chain, from raw arrays, with the dirty flag printed beside
// each result. FLAT-CAPABLE, and flat is the point: the rig writes nothing there.
extern bool  DiagComposeChain;
// [RELSTEP]. Read-only and FULLY PASSIVE. D-78's NAMED MISSING INSTRUMENT - the
// anchor's step RELATIVE TO THE BODY, which nothing has ever measured, plus the
// drawn hand and our grip stepped against the same reference in the same window.
// All three in ELEMENT 48's OWN FRAME, because a body-fixed point still rotates
// in world axes when the player TURNS and that would report the turn as a step.
// HEADSET ONLY: terms 1 and 3 hang off the IK target, so in flat the rig never
// solves and the probe says NO SAMPLES - NO VERDICT rather than printing zeros.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagRelStep = false;
#else
extern bool  DiagRelStep;
#endif
// [FRESH]. Read-only, FULLY PASSIVE. D-78's LAST surviving fresh-anchor
// candidate: locals do not go through dl_WorldResolver, so a world composed from
// the local chain should lead the 60-100 ms cached one. Composes BOTH element 8
// and element 48 and reports the step of the COMPOSED separation beside the
// CACHED one - because freshening the anchor alone would convert INV-208's
// ~51 mm differential into the full lag, which is D-78's mechanism verbatim.
extern bool  DiagFreshAnchor;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagThreadGroup = false;
#else
extern bool  DiagThreadGroup;
#endif
// [TGROUP] - which THREAD GROUP is this thread in? Answers U-19's live half.
// Pure reads: one TLS fetch and one deref, logged ONCE per (thread, site).
void ThreadGroupProbe(const char* where);
// [RSTORE] S3-submit. The PASSIVE ctx-id read: one TLS fetch, two SEH-guarded 4-byte
// reads, EVERY call counted into a 1 Hz histogram plus a line per value CHANGE (v2 -
// v1 deduped per (tid, site) and read the thread once, in the menu, INV-212). It does
// NOT call the world accessor - INV-169/INV-237
// (a read composes, stores and clears bit 1) and INV-28 (the read was visible in the
// headset) make the specced pointer-identity S3 unsafe on the render thread.
// HEADSET: flat never enters the Oculus submit path. Gate: diag_resolver_store.
void ResolverCtxProbe(const char* where);
// [RESTREAD]. U-33 half B: the AUTHORED rest local, read off the shared mesh
// ASSET through IMeshEntity vtable +0x58 - a DIFFERENT STORE from the live
// element matrix every other rest latch in this project reads. INV-147/148/149.
// FLAT-capable: nothing in its chain touches SolveArmIk or controller poses.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagRestRead = false;
#else
extern bool DiagRestRead;
#endif
// THE INV-150 FIX. Latch g_handRestLocal from the AUTHORED store on the mesh
// asset (INV-148) rather than from the live element matrix. Default ON. Read
// once, at latch time, so it is a LAUNCH-time A/B, not a live toggle.
extern bool RigRestFromAsset;
// U-47 containment (2026-10-10). With RigRestFromAsset, a failed AUTHORED read defers that side's grip capture
// instead of latching the live bone (identity in 5 of 5 logs). Default ON.
extern bool RigRestRefuseLive;
// THE INV-159 FIX. Latch [SHOULDER]'s rest offset for element 48/12 from the
// AUTHORED store on the mesh asset (INV-148) rather than from the live element
// matrix behind the 6 mm band + stability window. Default ON. Read once per side
// per process, so it is a LAUNCH-time A/B, not a live toggle.
extern bool RigShoulderRestFromAsset;
// W3: select `grip` by the HELD ITEM (class + element count) from dlvr/grips.txt
// instead of one constant per hand. Default OFF, and a strict no-op with no table.
extern bool RigGripPerItem;
// D-80 / INV-161. Drive elements 71/35 from their AUTHORED local (rotation AND
// translation) instead of leaving them to the animation; this is what lets them
// be gated without repeating INV-38. Default OFF.
extern bool RigDriveHandholders;
// INV-163. Rate-limit the WEAPON POSITION ONLY (achievedWrist), metres per second.
// S, the IK, the arm and the body are untouched. 0 = off, and it ships off.
extern float RigWeaponPosSlewMax;
extern bool  DiagAnchorAb;
extern bool  RigAnchorFromSpine;
extern bool  AnimSuppressBalance;
extern bool  AnimSuppressAntiWall;
extern bool  AnimSuppressSmoothPos;
void AnimSuppressInstall();
// Published from LevelCamera (camhook), consumed by AnchorAbTick. Per-object
// seqlock: `obj` is the camera object's address and is REQUIRED. The single-slot
// version was the defect that voided the 2026-09-23 21:57 run - TEN cameras are
// driven per frame and one slot held whichever was validated last, so the
// consumer differenced ACROSS cameras and reported gaps up to 1607 m.
void PublishCameraWorldPos(uintptr_t obj, const float* p);
void AnchorAbTick(const float* eye12);
extern float RigHhRotRPitchDeg;
extern float RigHhRotRYawDeg;
extern float RigHhRotRRollDeg;
extern float RigHhRotLPitchDeg;
extern float RigHhRotLYawDeg;
extern float RigHhRotLRollDeg;
extern float RigHhOffRX;
extern float RigHhOffRY;
extern float RigHhOffRZ;
extern float RigHhOffLX;
extern float RigHhOffLY;
extern float RigHhOffLZ;
// INV-161. Build wristBack from the authored handholder translation VECTOR rather
// than RigHandLen along the hand's column 0. DEFAULT ON.
extern bool RigWristBackFromAsset;
extern int  MeleeForcePart;

// Where a model keeps its ELEMENT COUNT - the TYPE key for D-44's fix. Read out
// of OUR OWN engine 2026-09-18:
//
//   IModelObject::GetElementsNumber(this):            engine+0x141F60
//       v1 = *(u64*)(this + 8);
//       return *(u16*)(*(u64*)dl_PickThreadBuffer(v1 + 904) + 40);
//   dl_PickThreadBuffer(a1):                          engine+0x9A520
//       return (*tls < 2) ? a1 + 16 : a1 + 32;        // slot selection only
//
// 904 + 16 = 920 = 0x398 is slot 0, and D-1 measured both slots holding the
// same container pointer (54 samples, one distinct value, structural).
// ⚠ THEY LIVE HERE BECAUSE TWO PLACES NEED THEM AND MUST NOT DRIFT APART: the
// VEH handler keys on them, and ElementPartMapUpdate validates them against the
// real export before registering anything.
constexpr uintptr_t kModelInnerOff     = 0x08;
constexpr uintptr_t kModelContainerOff = 0x398;
constexpr uintptr_t kModelCountOff     = 0x28;

// melee_hook_inline - WHICH MECHANISM CARRIES THE ELEMENT POKE.
//   0 = the DR1 execution breakpoint, as it has always been
//   1 = an inline hook at gamedll+0xC4D920, no debug register involved
// It exists to settle U-27 with ONE variable: INV-120 measured six of six
// in-play face-2 crashes trap-armed, three with the write off, and no face-2
// crash ever with the trap at 0 - but there is no mechanism, so the way to
// decide it is to remove the debug register and see whether the face goes.
extern int MeleeHookInline;
extern float MeleePoseOffsetX;
extern float MeleePoseOffsetY;
extern float MeleePoseOffsetZ;
// 0 = constant offset (what INV-132 proved). 1 = sign alternates every call,
// which is the only way to test that EACH call's delta reaches the record for
// THAT call - staleness and averaging are invisible under a constant.
extern int   MeleePoseWave;

// ---- R5 step 5b: DRIVE THE SWEEP FROM THE CONTROLLER ----
// 0 = off, and step 5a's offset path is then byte-for-byte what it was.
// 1 = write the controller-derived POSITION only (mtx[3/7/11]); the basis stays
//     the attack animation's, so the weapon's reach still points where the
//     animation points.
// 2 = position + cWorld, i.e. Ry(bodyYaw) * quat(grip.orientation). STATE.md's
//     5b spec.
// 3 = position + the HAND BONE's world basis (cWorld * the grip calibration).
//     The composed output is a2.R * helper.t + a2.t and helper.t is the
//     "AttackHelper"/"HandHelper" offset, which is expressed in the hand bone's
//     frame - so 3 is the frame that offset actually belongs to. WHICH OF 2 AND
//     3 IS RIGHT IS [UNKNOWN] and is what the first headset run decides.
// >>> DRIVE != 0 IGNORES MeleePoseOffset*/MeleePoseWave ENTIRELY. <<< They are a
// different experiment (5a / 5a-bis) and mixing two instruments into one probe
// is INV-128's last bullet.
extern int   MeleePoseDrive;
// Which controller drives the sweep. 0 = right, 1 = left. INV-128 established
// that only record A - the single-weapon record - is reachable, which says
// nothing about which physical hand holds the weapon.
extern int   MeleePoseHand;
// A published grip pose older than this is REFUSED, not used. The rig ticks far
// faster than the frame (~580 solves/s measured), so anything approaching this
// means the rig has stopped - INV-1's three master enables, or INV-81's
// death-destroys-the-capture. Fail closed and name it.
extern float MeleePoseMaxAgeMs;
// If |controller-derived position - the incoming animation position| exceeds
// this, the write is REFUSED and the value is named. Two jobs: it stops a wrong
// transform from throwing the sweep somewhere melee can never connect (which is
// indistinguishable from "the feature does nothing"), and it is a free guard
// against a caller that is not the player, whose incoming matrix would not be
// near our controller. 0 disables it.
extern float MeleePoseMaxJump;

// Install the inline hook. Retried by the caller until it takes; logs which
// check failed if the prologue does not match the bytes decoded on 2026-09-18.
bool MeleeElemInlineHookInstall();
bool MeleeElemInlineHookLive();
// R5 step 4: the melee POSE observation hook at gamedll+0xDC1C90 (INV-129/130).
// READ-ONLY - observes four arguments and returns. Independent of the element
// hook above: different address, its own trampoline, no shared state.
bool MeleePoseHookInstall();
// The right grip pose the rig published for the melee hook (MeleeGripRead, seqlock), for swing.cpp.
// pos = anchor + Ry(bodyYaw)*(ctl - head); cWorld = Ry(bodyYaw)*quat(grip), row-major, grip axes as COLUMNS.
// false = NO POSE (nothing published, or every read raced a write) - never a zero pose. INV-6.
bool SwingGripRead(float pos[3], float cWorld[9], long long* qpcOut);
// The same for EITHER side (0 = RIGHT, 1 = LEFT) - buggywheel.cpp's grab tests. false = NO POSE.
bool RigGripWorldRead(int side, float pos[3], float cWorld[9], long long* qpcOut);
// The wrist HUD's arm anchor (trace docs/traces/20261113_1145_wrist_arm_anchor.md): side 0 right / 1 left - the arm solve's final
// elbow, forearm world rotation (row-major, basis in the COLUMNS), achieved wrist and drawn hand basis, game world. False = none.
bool RigArmAnchorRead(int side, float elbow[3], float fore[9], float wrist[3], float hand[9], long long* qpcOut);
// The DRAWN hand's grip socket (side 0 right / 1 left), game world, mtx34 rows: the drawn hand basis + the achieved wrist + drawn x the
// handholder's authored local (trace 20261120_2000) - where ItemCtlPlace puts a weapon. False = no pose. lidhand's carried item.
bool RigDrawnHolderRead(int side, float out12[12]);
// The RIGHT hand's key-pinch point (world): the published grip point + the commanded hand basis x kKeyPinchFromGrip (buggy_poses.inc,
// the game's frame-600 thumb / index tip midpoint) - buggywheel.cpp's key grab test (RUN BK3). false = no pose or no hand basis yet.
bool RigGripPinchRead(float pinch[3]);
// diag_swing_shadow: the right arm's shoulder and wrist lengths (asked |T - S|, drawn, reach dMax). False until published.
bool SwingArmRead(float shoulder[3], float* dAsked, float* dAchieved, float* dMax);
// The blade-length hit line: a weapon model's mesh name (itemrel::RhMeshName, guarded). False when unreadable.
bool ItemMeshName(uintptr_t model, char* out, size_t n);
// [LOADBEARING: item_place_rel] THE HELD ITEM's IDENTITY (a tester, 2026-11-08: the axe held sideways after a heal). A hand item model's
// InventoryItem: WeaponVis+0xB0 while the +0xA8 handle is set - the pair OUR CorrectItemPosition tests (gamedll+0xC62AC0,
// re_analysis/correct_item_position_decompiled.txt:56-57); Mac 2022 names it (WeaponVis::GetItemDesc / SetInventoryItem, ida/pru1_mac).
// 0 = none / unreadable. The game REUSES the model ADDRESS on a swap (PlayerVis::ChangeInventory destroys the old WeaponVis and creates
// the new one in one call - ida/pru2_mac), so every per-item cache keys on (model pointer, this). A durability stage swap keeps it
// (UpdateMeshAndSkin, same object). COST: two guarded pointer reads.
uintptr_t HeldItemInventoryItem(uintptr_t model);
bool MeleePoseHookLive();
unsigned long MeleePoseHookCalls();
// R5 step 5a: the pose OFFSET. A constant world delta added to the incoming
// matrix, which INV-130's arithmetic shows lands in the composed output
// unchanged. All three default to 0.0 = disarmed; the write is skipped while
// they are. Not a write through g_meleeSelf - a2 is the caller's own stack.
unsigned long MeleePoseHookWrites();
unsigned long MeleePoseHookWriteFails();
// R5 step 5b: how many calls the CONTROLLER steered, and how many were refused
// for any reason. Both unbudgeted. driven + refused does not have to equal
// calls - with melee_pose_drive=0 neither moves at all.
unsigned long MeleePoseHookDriven();
unsigned long MeleePoseHookRefused();
// How many times the hook has run. The DR path's equivalent is WatchExec's
// `fires`, which stays at 0 in inline mode - so this is the denominator for a
// run, and INV-69's rule applies to it just the same.
unsigned long MeleeElemInlineHookCalls();

// >>> MODEL DISCOVERY FOR THE INLINE PATH. RUN 6 DID NOT HAVE THIS AND WROTE
// NOTHING ALL SESSION - `PART MAP hits=0 typeHits=0 misses=257, 0 model(s)
// mapped`. <<< The hook parks each contact record in a ring; the damagewatch
// worker calls this to hand them to ElementTableDumpFromHitRecord, which is the
// SAME entry point the DR path uses. Returns how many were drained.
int MeleeElemDrainSeen();
// Records the hook had to drop because the worker was behind. Counted rather
// than overwritten: a silent overwrite would make a missing model look like a
// model that never appeared.
unsigned long MeleeElemSeenDropped();

// [ELEMDUMP] - dump a model's element table (index -> name -> BONE), asking the
// engine. Gated on diag_elem_dump; one dump per distinct model, capped at 8.
//
// ...FromHitRecord takes whatever the melee trap captured at [a3+0] and resolves
// a model from it, trying the pointer itself and then the INV-55/INV-33 chain
// (AI+0x28 -> *(entity+0x40) -> vtable slot 1). A candidate is dumped ONLY if
// the engine answers sanely about it - INV-67 established that field is not
// reliably a model, so a failed resolve is logged as a failed CHAIN and never as
// "this model has no elements".
void ElementTableDump(void* model, const char* whoFor);
void ElementTableDumpFromHitRecord(uintptr_t candidate, const char* who);
extern bool RigAnimGate;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagFrozenLocal = false;
#else
extern bool DiagFrozenLocal;
#endif
// Buffer 0 (inner+0x398) vs buffer 1 (inner+0x3A8). Structural half is free in
// flatscreen; the write half needs the rig, therefore VR, and refuses by name.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagBufferCmp = false;
#else
extern bool DiagBufferCmp;
#endif
// [RSTORE] - which store dl_WorldResolver hands back, decided by POINTER IDENTITY
// (INV-214) rather than by reading a thread-local (INV-212). Replaces [TGROUP],
// which read the wrong TLS slot - INV-213. Flat-valid at sites S1 and S2; the
// submit-path site S3 is NOT built. Spec: docs/PROBE-SPEC-RESOLVER-STORE-2026-09-24.md
extern bool DiagResolverStore;
// [RSYNC] - Stage 0 of docs/SPEC-RENDER-SYNC-HOOK-2026-09-26.md. A READ-ONLY detour on the
// render-sync job (engine+0x487190, INV-240): does the snapshot that fills altMatrices capture
// the clobber? HEADSET - needs the rig solving. No detour is installed while this is off.
extern bool DiagRenderSyncWatch;
// [SUBTREE] - A-1 of docs/PROPOSAL-FIX-SHAKE-AND-SNAP-2026-09-24.md. Pins the arm's
// UNDRIVEN children ({51,52,64,72} / {15,16,28,36}) to their AUTHORED local so they
// ride the parent rigidly. Measured cause: [RELCHURN] 2026-09-24 ranked uparmtwist at
// 717 deg/s and foretwistt at 660 deg/s in eye-local space, undriven. DEFAULT ON.
extern bool RigSubtreeRest;
// [SUBTREE] A-2 - the 32 finger pins (16 per hand, every joint in every chain), on
// their own gate. The fingers are where the measured independent motion actually is:
// r_finger13 836 deg/s with 179 deg single-frame steps, l_finger23 5.18 m/s with
// 0.38 m jumps, all in eyecamera-local space. DEFAULT ON.
extern bool RigSubtreeFingers;
// The grip re-latch: capture each hand's finger pose from the LIVE local after an item
// is equipped, instead of holding the authored OPEN hand. Fixes INV-216's measured
// defect - Jay: "it also cancels out the grip pose for the weapons". DEFAULT OFF.
extern bool RigSubtreeGrip;
extern int  RigSubtreeGripDelayMs;
// Reuse the last REAL grip a hand produced when a capture returns only the authored pose.
// Built on a measurement: of 12 captures on 2026-09-24, ONE saw a real grip (basis
// 0.64266) and eleven saw authored (0.00000), because the accessor returns the CACHED
// world and a finger's cache was last rebuilt from our own authored local (INV-215/223).
extern bool RigSubtreeGripCanon;
// THE RULE: whichever hand holds something, that hand's FINGERS ARE NOT DRIVEN - released
// on pick-up, restored to the authored pin when the hand is empty. Default ON. The capture
// route it replaces is INV-226: 29 captures, 0 real poses stored.
extern bool RigSubtreeFingersHeldRelease;
// C-1 of docs/PROPOSAL-PART-C-ARM-ROOT-2026-09-24.md: pin the CLAVICLE's local so the arm's
// root rides hspine1 rigidly, closing the doorway INV-225 measured (a ~44 deg one-frame
// clavicle replacement reaching our solve). 0=off, 1=RIGHT only, 2=LEFT only, 3=both. Per
// side because the untouched arm is the control. REFUSES if rig_clavicle_share or
// rig_clavicle_protract_deg is non-zero - the rig drives the clavicle then, and two writers
// on one element with mine running last would silently erase the rig's contribution.
extern int  RigPinClavicle;
// [PGUARD] - INV-225. Rejects a one-frame corrupted clav/upper read inside the solve. The
// measured corruption is a ~44 deg clavicle / ~41 deg upper-arm / 13 cm jump while the
// target holds to 0.3 mm, which is INV-35's replacement arriving through the solve's INPUTS
// and is BOTH the snap and the melee sprint shake. BOUNDED: after MaxHold consecutive
// rejections it force-accepts and re-baselines, so it cannot run away the way the yaw slew
// limiter did. Default OFF.
// [ROOTSTAB] - C-3a. Measures C-3's premise: are the TRANSLATIONS upstream of the clavicle
// stable while its BASIS is not? Reports the per-tick translation step of hspine1, the
// clavicle and the upper arm SEPARATELY. Log-only, and it rides reads the solve already
// makes, so it costs no accessor call. DEFAULT OFF.
extern bool  DiagRootStability;
// [POLEWATCH] - U-40. |pole| and al in the elbow-pole stage, plus counters on the two
// discontinuities that have never had one. Log-only, rides quantities already computed.
// HEADSET-ONLY: it needs the solve to reach the pole stage, which needs controller poses.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagPoleWatch = false;
#else
extern bool  DiagPoleWatch;
#endif
// [STALECLAV] - INV-246. How far does the clavicle move between the distinct samples the
// asynchronous solve takes, and how far would that carry the hand? Log-only, rides quantities
// the solve already holds. HEADSET-ONLY (needs controller poses to reach the solve's clavUsed).
extern bool  DiagStaleClav;
// [BODYVIEW] + [TICKGAP] - the whole DRAWN body (altMatrices, read raw) relative to the drawn eye,
// per published frame: step, jerk and rotation per element; and the rig tick's timing.
// Log-only, no accessor call. HEADSET for the arm question.
extern bool  DiagDrawnCensus;
// [HEADTURN] - does a PURE head turn (sticks centred, no locomotion) yaw the root, hspine1, the
// eye bone or the clavicles? Slope of each against HMD yaw, per 1 s window. Log-only. HEADSET.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagHeadTurn = false;
#else
extern bool  DiagHeadTurn;
#endif
#if defined(DLVR_BETA_LEAN) && !defined(DLVR_YAW_PROBE)   // compiled out of BETA - back in the yaw probe variant (trace 20261119_2145)
inline constexpr bool DiagCamYaw = false;
#else
extern bool  DiagCamYaw;     // [CAMYAW] - design D's D2 detour (camyaw::Hook)
#endif
extern bool  HeadYawLookEnableFix;   // head_yaw_look_enable_fix - INV-329, camyaw::lookenable::Hook (LIVE)
extern bool  CameraShakeYawOff;      // comfort_camera_shake_yaw_off - shakeyaw::Hook zeroes +347DB0's angles (LIVE, test)
extern int   LookPitchAnchor; // look_pitch_anchor 0/1/2 - INV-272's fix, rides camyaw::Hook
// VR Comfort tab, SPEC-VR-COMFORT-2026-09-30 §5 (C0). Snap turning also rides camyaw::Hook.
extern int   ComfortTurnMode;      // comfort_turn_mode 0 = smooth, 1 = snap
extern int   ComfortSnapAngle;     // comfort_snap_angle - index into snapturn::AngleDeg (snap_logic.h)
extern bool  ComfortBodyFollowsHead;   // comfort_body_follows_head - SPEC-BODY-FOLLOWS-HEAD-2026-10-18 (LIVE)
extern int   ComfortSnapRepeatMs;  // comfort_snap_repeat_ms - 0 = one snap per flick
// One byte-verified gamedll detour through this file's prepare/commit machinery (threads suspended during the
// write). Returns true only when committed. For detours owned elsewhere (grappleaim.cpp A2).
bool InstallSingleGameDetour(uintptr_t target, const unsigned char* expect, size_t stolen, void* replacement,
                             void** trueOut, const char* name, int* attemptsOut);
// A small byte patch (n <= 16) under the same verify / suspend / commit rule. False = mismatch or not written.
bool PatchGameBytes(uintptr_t target, const unsigned char* expect, const unsigned char* repl, size_t n, int* attemptsOut);
// The held item's element world matrix (mtx34, row-major, translation at [3],[7],[11]) BY NAME. hand 0 = right. For
// aimhook's fire_barrel_origin (INV-277). `name` must be a string literal (cached by pointer). False + *why on refusal.
bool HeldItemElementWorld(int hand, const char* name, float out12[12], uintptr_t* itemOut, int* idOut, const char** why);
// The held item's BARREL helper as the game names it (INV-494/495): "bolt_pose" (crossbows), "arrow_pose" (bows), else
// "barrel"; chosen once per item. boreAxisOut: 2 = projectile forward is the element's +Z, -1 = its -X. nameOut = the
// chosen name ("none" when the item has none). False + *why on refusal.
bool HeldItemBarrelHelper(int hand, float out12[12], uintptr_t* itemOut, int* idOut, const char** nameOut,
                          int* boreAxisOut, const char** why);
// fire_aim_from_barrel: HookTail publishes the held gun's bore (-X of "barrel") once per player update; aimhook reads it
// from its VEH without calling the engine. False when unset, stale (> 150 ms) or contended; *why says which.
// posOut (optional) = the "barrel" element's world position (= the bullet start, INV-277), for the gun reticle cast.
void PublishBarrelBore();
// bow_reticle_3d (INV-496): the bow's arrow line (LEFT arrow_pose position + its +Z), own slot. Get: false when stale
// (> 150 ms - no bow held) with the last refusal in *whyOut.
void PublishBowRay();
bool GetPublishedBowRay(float dirOut[3], float posOut[3], unsigned* ageMsOut, uintptr_t* itemOut, const char** whyOut);
// [BOWRELEASE] / bow_fire_latched: the published bow direction at least agoMs old (ring of ~0.8 s; state = the bow
// controller's, -99 unreadable), and the last one published while the bow was DRAWN (state 1 / 3).
bool GetBowRayAgo(unsigned agoMs, float dirOut[3], int* stateOut, unsigned* actualAgoMsOut);
bool GetLastDrawnBowRay(float dirOut[3], unsigned* ageMsOut);
bool GetPublishedBarrelBore(float dirOut[3], unsigned* ageMsOut, const char** whyOut, float posOut[3] = nullptr,
                            uintptr_t* itemOut = nullptr);
extern bool  DiagLookPitch;   // [LOOKPITCH]
extern bool  RigParentGuard;
extern float RigParentGuardBasis;
extern float RigParentGuardTransM;
extern int   RigParentGuardMaxHold;
// [YAWSLEW] - INV-217's fix. Per-tick rate limit on bodyYaw in deg/s, 0 = OFF. Limits
// an ANGLE applied to a lever whose anchor is untouched, so D-78 (which rejected
// limiting the anchor POSITION, a common-mode term) does not apply.
extern float RigYawSlewMax;
extern float RigYawTeleportDeg;
// rig_yaw_stick_gate (INV-287): 0 = today's bodyYaw; 1 = the root's fast band only while the right stick turns.
extern int   RigYawStickGate;
extern float RigYawGateRampMs;
// rig_yaw_stick_gate=2: the camera EMA's constant while the right stick turns.
extern float RigYawTurnSmooth;
// [POSEMISS] - B-1. Logs the controller-dropout counters. DEFAULT OFF.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagPoseMiss = false;
#else
extern bool DiagPoseMiss;
#endif
// [SNAPDUMP] - the snap instrument. Triggers on the OUTPUT basis
// jumping more than diag_snapdump_rad in one solve and dumps the preceding 30 frames of
// target, anchor, yaw and the flip counters - so it catches the snap whatever causes it,
// on either arm, instead of instrumenting one hypothesis. DEFAULT OFF.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagSnapDump = false;
#else
extern bool  DiagSnapDump;
#endif
extern float DiagSnapDumpRad;
extern int   DiagSnapDumpMax;
// U-40(ii). Make the dump budget PER SIDE. Off = the historical shared cap, unchanged.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagSnapDumpPerSide = false;
#else
extern bool  DiagSnapDumpPerSide;
#endif
// THE OPEN MEASUREMENTS - U-40(i), U-39, U-41. One master for [SNAPRATE], [CLAVBURST],
// [REACHCLAMP] and [DEGEN], all log-only, all bucketed still/moving with the held-item
// string on every report line so one run separates melee-sprint from empty-standing.
// HEADSET-ONLY, every one: they need the solve, which needs controller poses (INV-11).
extern bool  DiagRigCounters;
extern float DiagRegimeMoveMps;
extern float RigClavBurstBasis;
extern float RigClavBurstOffsetM;
// [HIERSYNC] - resolves CHierarchyElementSynchronizer's vtable and dumps its slots, to find the
// local->world compose entry point. ⚠ FLAT-SAFE and gated SEPARATELY from the counters above,
// which are headset-only: an RTTI walk and vtable reads are class existence and pointer identity
// (INV-11's flat-safe column) and must not be made to wait on a headset run.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagHierSync = false;
#else
extern bool  DiagHierSync;
#endif
// world_child vs world_parent x local_child, DOWN THE ARM, compared as an ANGLE.
// Engine-consistency half runs in flatscreen.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagArmCompose = false;
#else
extern bool DiagArmCompose;
#endif
// Extend the element-48 flag handshake to the distal arm (49/53, 13/17). The
// predicted fix for the held-item compose failure. DEFAULT ON (was OFF - never run).
extern bool RigDistalFlagHandshake;
// U-2: bit1-only poke on the item anchor (71 <- 53, 35 <- 17), instrumented with
// a world read either side of the engine's rebuild. Default off - it writes
// engine memory on a hot path and the last attempt made the weapon vanish.
extern bool RigDistalBit1Poke;
extern bool RigWorldRefresh;
// [ITEMSCALE]. Read-only: the held item's "Holder" basis column magnitudes. The
// engine re-derives that scale from the item's own matrix every frame, so it is
// self-referential and one corruption is permanent.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagItemScale = false;
#else
extern bool DiagItemScale;
#endif
extern bool DiagTrsSkipTest;
// INV-283: hold the upper body's ROTATION channel off the animation (EntityInfo flags bit 9).
extern bool AnimHoldUpper;
// anim_hold_clavicles: the same bit-9 hold on the two clavicles only (the vertical arm bob). Arms while standing still.
extern bool AnimHoldClavicles;
// rig_throwable_body_hold: the same bit-9 hold on the ten upper-body elements, ONLY while a throwable is out (INV-575 / INV-576).
extern bool RigThrowableBodyHold;
// rig_throwable_hold_pelvis: also hold the PELVIS rotation in that hold (11th slot; 2.7 deg in the throw clip, INV-576 addendum).
extern bool RigThrowableHoldPelvis;
// rig_throwable_hold_lclavicle: also hold the LEFT clavicle's POSITION in that hold (12th slot; the clip moves it 3.7-6.2 cm).
extern bool RigThrowableHoldLClavicle;
// diag_view_body: the drawn body in the RENDERED view's frame, sampled inside camhook's anchor-camera write.
extern bool DiagViewBody;
// diag_throw_arm ([THROWARM], INV-316): the drawn right arm vs what SolveArmIk placed, in [VIEWBODY]'s buckets.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagThrowArm = false;
#else
extern bool DiagThrowArm;
#endif
// diag_elbow_side ([ELBOWSIDE], INV-332): which side of the shoulder-wrist line SolveArmIk puts each elbow on.
extern bool DiagElbowSide;
// diag_arm_capture ([ARMCAP]): the arm solve's full inputs / outputs and the engine's element matrices, to dlvr_armcap.log.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagArmCapture = false;
#else
extern bool DiagArmCapture;
#endif
// diag_gun_hand ([GUNHAND]): the weapon Holder vs the DRAWN right hand, as a vector, in [VIEWBODY]'s sampler.
extern bool DiagGunHand;
// diag_rel_dev ([RELDEV]): the hand->Holder rel item_place_rel carries, against a standing reference (climbs / heals).
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagRelDev = false;
#else
extern bool DiagRelDev;
#endif
// item_hide_by_state ([ITEMHIDE], INV-490): hide the held item while the climb / heal controller is active, like flat.
extern bool ItemHideByState;
// diag_stale_write ([STALEWRITE], U-49 / INV-364): our arm writes and flag handshakes classed by whether the game's
// player object still exists. Log-only.
extern bool DiagStaleWrite;
// rig_player_live_guard ([PLAYERLIVE], U-49 / INV-364..366): refuse every arm write and drop the capture when the
// game's player object is no longer the one the model was captured from.
extern bool RigPlayerLiveGuard;
// The game's local player object (INV-570's chain, the guard's own read - SwPlayerObjNow), or 0. *why: 0 = the chain
// read (0 then means no player object), else it did not read. For camhook's capture (INV-524 fix A). Four guarded loads.
uintptr_t RigGamePlayerObject(int* why);
// Called by camhook right after it publishes the rendered view (render thread). cam = its 12 written floats.
void ViewBodySample(const float* cam);
// diag_body_writers ([BODYWRITERS], INV-285 / U-44): what each post-animation writer adds to the player's arm per
// update, and their order. Installs three PASS-THROUGH gamedll detours only when the setting is on.
extern bool DiagBodyWriters;
// INV-286 A/B: A = the AIM_AT controllers stop re-aiming the player's clavicles; C = C04A30's sway is not called (control).
extern bool AnimSuppressAimPaths;
// INV-345 option 1: the LOOK controller (SPINE_0..3 + EYE_CAMERA) stops bending the player's spine. Live.
extern bool AnimSuppressLookPath;
// Arm stage 1 (SPEC-ARM-HIERARCHY): the RIGHT grip constant rotated toward the mirror of the LEFT's (0..1), and how much
// of that the held weapon follows (1 = turns with the hand, 0 = keeps today's direction). Both live.
// INV-352: the wrist clamp lets the hand keep the controller's angle this many degrees past each limit. Live.
extern float RigWristOverflowDeg;
extern float RigGripRightFix;
extern float RigGripRightFixWeapon;
// Calibration screen, build B: the grip (row-major 3x3, columns = axes) the solve uses with an EMPTY hand - the baked
// constant, the right one rotated by rig_grip_right_fix as the solve does. False = rig_grip_use_baked off / no constant.
// Read-only, any thread.
bool CalibGripEmptyHand(int side, float out[9]);
// The hand's DRAWN turn on top of cW . grip (row-major 3x3) - the solve's wristBack post (trace 20261116_1126): the authored rest
// (latched, or the baked one in the main menu) x the hand-angle dials. side 0 = right, 1 = left. Read-only, any thread.
void HandPostForSide(int side, float post[9]);
// SPEC-EYE-FORWARD: the view at the face's eyes (0..1.5 of the eye-bone -> face-eyes vector) and the eye's arc about the
// neck on the camera only. Live once installed.
extern float CamEyeForward;
extern bool  CamEyeArc;
// The view lifted straight up (mm), and how much of the arc's DROP on a look-down is kept (0..1). Both live once installed.
extern float CamEyeUpMm;
extern float CamEyeArcDrop;
// Poll thread, beside BonesAnimInstall. One-shot once gamedll is mapped; logs NOT ARMED when both settings are 0 at launch.
void EyePointInstall();
// INV-619 (dlgview.cpp): the WORLD offset the camera base received this update (d1 + d2, exactly as added to vis+4112) and the
// player object it was added for (the controller-table owner, INV-619 addendum 1 (c)). False when not armed or older than 250 ms.
bool EyePointViewOffsetWorld(float out[3], uintptr_t* owner);
// INV-620 (cinlook.cpp): design D's addend now in +4460, its object, and whether camyaw::Hook's own chase is running. Game thread only.
void CamYawAddendNow(float* added, uintptr_t* obj, bool* inChase);
// item_rel_hold: 0 off | 1 melee | 2 all but the bow - the standing grip rel is used while the player moves.
extern int  ItemRelHold;
extern bool AnimSuppressSway;
// Poll thread, beside AnimSuppressInstall. One-shot once gamedll is mapped; logs NOT ARMED when the setting is 0.
void BodyWritersInstall();
// Tick thread (BoneBlendTick): latch the drawn column once; log a handed-over 5 s window.
void BodyWritersLatchTick();
void BodyWritersReport();
// diag_bones_anim ([BONESANIM]): does PlayerFppVis slot 342 (gamedll BFF080) re-pose the arm locals with a firearm,
// and after our write? Installs ONE pass-through detour only when the setting is on.
extern bool DiagBonesAnim;
// anim_restore_owned_bones (INV-293 A/B): after slot 342 re-poses the arm with a firearm, put back the elements WE own.
extern bool AnimRestoreOwnedBones;
// anim_skip_owned_bones (INV-293 fix candidate): slot 342 does not compute the gun pose for elements WE own.
extern bool AnimSkipOwnedBones;
// anim_decouple_arms: slot 342 does not compute the gun pose for ANY arm element (the held hand's fingers excepted).
extern bool AnimDecoupleArms;
// rig_pin_foretwist (INV-294): pin the forearm twist bones 50/14 to their authored local with the [SUBTREE] pins.
extern bool RigPinForetwist;
// anim_skip_hand_correction (INV-295): PlayerFppVis slot 407 (CorrectHandPosition) is not called for the player.
extern bool AnimSkipHandCorrection;
// anim_skip_spine_pitch (INV-310 A/B): PlayerFppVis slot 343's C06C60 (HSPINE1 pitched by the look) is not called for the player.
extern bool AnimSkipSpinePitch;
// rig_clavicle_hold (INV-296): hold the clavicle at its last non-gun pose while a gun is held (driven + handshake).
extern bool RigClavicleHold;
// rig_clavicle_hold_throw: rig_clavicle_hold also holds the RIGHT clavicle while the melee throw is READY / just released.
extern bool RigClavicleHoldThrow;
// rig_clavicle_hold_live_t (INV-331): the held clavicle's solve uses the LIVE local translation, not the fixed pose's.
extern bool RigClavicleHoldLiveT;
extern bool RigClavicleHoldBow;   // rig_clavicle_hold_bow - the LEFT collarbone is also held while a bow is in the left hand (INV-544)
extern bool DiagClavicleBowAb;    // diag_clavicle_bow_ab  - the proving run: the bow hold alternates OFF / ON every 3 looses
// Poll thread, beside BodyWritersInstall. One-shot once gamedll is mapped; logs NOT ARMED when the setting is 0.
void BonesAnimInstall();
// The player's PlayerFppVis as latched by the BFF080 detour (the first `this` seen), or 0 when not latched / not installed.
// [VIEWPOS] STAB reads the head stabiliser (INV-306) through it; diag_view_pos=1 installs the detour for that alone.
uintptr_t BonesAnimPlayerVis();
// Tick thread (BoneBlendTick): the 5 s window.
void BonesAnimReport();
// Plausibility ceiling on |eye - element 48|, metres ([SOLVE]'s IMPLAUSIBLE marker).
extern float RigAnchorMaxM;
// Ceiling on the elbow pole's world-up component. The right arm going upside
// down on the bow: past a threshold the pole blend crosses zero, taking the
// elbow above the shoulder and flipping the plane normal both arm bones' roll
// is derived from. 0.0 = level with the shoulder at most.
extern float RigElbowPoleMaxUp;
// Re-apply the solved bones after d7380 returns, for when the entry write is
// being consumed and then discarded inside it.
extern bool RigWriteAtOwnerExit;
// INV-246's fix: solve the arm once per frame at the player-update TAIL (after the whole player update).
extern bool RigSolveAtTail;
// rig_cutscene_standdown: the rig hands the arms, hands and fingers to the scene's animation in cutscenes + dialogues (default off).
extern bool RigCutsceneStanddown;
// Which element's world matrix DiagWorldWriter watches. 53 = r_hand, whose
// origin is the wrist; 49 = r_forearm, whose origin is the elbow.
extern int RigWorldWatchElem;

// WRITES to the player skeleton. One bone (r_forearm, element 49), rotation
// only, oscillating +/-60 deg, and it auto-disarms 60 seconds after its FIRST
// write - the countdown deliberately starts then rather than at launch, so time
// spent leaving the safe zone and drawing a weapon does not eat the window.
// Needs diag_bone_blend_probe on as well.
extern bool RigWriteTest;

// Second write point: at FUN_1800d7380's ENTRY, the last moment in the frame a
// write can still reach the world-matrix build. Writing after FUN_1800dd7b0
// moves the forearm with no weapon held and does nothing with one, so something
// recomputes the arm in between - most likely an IK pass reaching for the held
// item. On by default while that is being narrowed down.
extern bool RigWriteAtOwner;

// Drive the right forearm from the right controller instead of the test
// oscillation. Relative to the controller pose captured when the arm is first
// driven, so holding still leaves the arm where the game put it.
extern bool RigArmFollowController;

// How long the write stays live after its FIRST write. 60s suits a yes/no probe;
// judging how the arm actually FEELS needs longer.
extern float RigWriteSeconds;

// Which skeleton element the controller drives. 48 = r_upperarm (swings the
// whole arm), 49 = r_forearm (twist only). One at a time - driving both with the
// same rotation composes it twice.
extern float RigArmElement;

// Apply the measured OpenXR -> bone-space axis remap, (x,y,z) -> (-y,-x,-z).
// Off reverts to the raw delta, which is how the mapping was measured in the
// first place - keep it switchable so the measurement can be repeated.
extern bool RigArmAxisRemap;

// Inject the mapping's yaw CHANGE into the arm as rotation, to see whether the
// measured 8.5 deg/tick jitter is actually perceptible. The axis is arbitrary -
// this is a visibility test, not the final world-stabilisation.
extern bool RigArmInjectYaw;

// PHASE B: two-bone IK. Drives r_upperarm (48) and r_forearm (49) so the game
// hand reaches where your real hand is. Target uses the engine's OWN play-space
// conversion (negate X and Z, keep Y - read out of FUN_1802e52a0), so no
// calibration and no re-centring. Supersedes rig_arm_follow_controller when on.
extern bool RigArmIk;

// Wrist: the hand takes the controller's own rotation, via a grip constant
// // captured once. Forearm twist share splits pronation between forearm and
// wrist rather than pinching it all at the wrist. Clavicle share lets the
// shoulder contribute to a reach, which is most of what makes it look human.
extern bool  RigWrist;
extern float RigForearmTwistShare;
// 0.0 = arm body yaw from the skeleton root (pre-2026-09-14 behaviour), 1.0 =
// from the camera. The root lags the camera by tens of degrees on a stick turn
// (INV-20). Shortest-arc blend; see the use site in boneblend.cpp.
extern float RigYawCameraBlend;
// Smoothing on the camera-minus-root CORRECTION. 1.0 = none. See the use site.
extern float RigYawCameraSmooth;
// [SEGLEN]. Read-only: peak-held per-second min/max of elements 49 and 53's
// local translations - the bicep-to-elbow and forearm lengths. Neither is
// clamped and neither is written by us.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagSegmentLen = false;
#else
extern bool  DiagSegmentLen;
#endif
// [STRAFE]. Read-only: eye->shoulder VECTOR in the body frame, peak-held, with a
// root-derived sideways/forward travel signal so strafe windows are measured
// rather than assumed.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagStrafe = false;
#else
extern bool  DiagStrafe;
#endif
// 0 = shoulder from the clavicle (pre-2026-09-14), 1 = shoulder hung off the eye
// via a latched resting offset so a body lean cannot move the arm's origin.
extern float RigShoulderFromEye;
// [YAWSTRAFE]. Read-only: separates camera turn rate from root travel so a
// strafe-without-turn window can be measured - the case [SWAY] discards.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagYawStrafe = false;
#else
extern bool  DiagYawStrafe;
#endif
// [YAWNOISE]. Read-only: raw camera-yaw per-tick step, split still vs moving.
// Independent of the blend, so it can run during a filter sweep.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagYawNoise = false;
#else
extern bool  DiagYawNoise;
#endif
// How far the wrist may bend before it is capped. A real wrist manages roughly
// 70 degrees of flexion and rather less deviation; past that a rig stops looking
// like an arm.
// The wrist's real, asymmetric envelope in degrees. A single symmetric limit is
// wrong by roughly 4x on the radial side.
extern float RigWristFlexDeg;
extern float RigWristExtDeg;
extern float RigWristUlnarDeg;
extern float RigWristRadialDeg;
extern bool  RigWristSwapAxes;
// The left arm, mirrored. Off by default so the right cannot regress behind it.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagDeformIndex = false;
#else
extern bool  DiagDeformIndex;
#endif
extern bool  RigRestoreE48Len;
extern bool  RigRestoreLeftLen;
// rig_restore_upper_dir (INV-330): the upper-arm restores write the whole latched vector, not its length only.
extern bool  RigRestoreUpperDir;
// rig_elbow_right_axis_mirror (INV-336): the RIGHT elbow pole's hand term chases the mirrored LEFT grip axis. Pole only.
extern bool  RigElbowRightAxisMirror;
// rig_elbow_pole_mode and its four dials (SPEC-ELBOW-POLE §4c, INV-339): 1 = pole = dir x A, a tilted body axis.
extern int   RigElbowPoleMode;
// rig_elbow_forearm_rot_deg (SPEC-ELBOW-REAL-ARM): mode 2's forearm roll range from the measured neutral, degrees each way.
extern float RigElbowForearmRotDeg;
extern float RigElbowOutDeg;   // rig_elbow_out_deg - mode 2's outward lift (2026-10-29 smooth / outward spec)
extern float RigElbowFaceMinCm;     // rig_elbow_face_min_cm - mode 2's elbow zone: minimum elbow-to-camera distance (2026-11-08)
extern float RigElbowSwingMaxDeg;   // rig_elbow_swing_max_deg - mode 2's elbow zone: swing cap from the lifted target (2026-11-08)
extern float RigElbowAxisFwd;
extern float RigElbowAxisUp;
// rig_elbow_axis_fwd_side (INV-356): the FORWARD part of A once the arm points out to its own side. Equal to
// rig_elbow_axis_fwd = no side rule.
extern float RigElbowAxisFwdSide;
extern float RigElbowHandWeight;
extern float RigElbowShortHoldCm;   // the short-arm elbow hold (mode 1), cm; 0 = off
extern bool  RigBowElbowFromHand;   // the bow's drawing elbow from the real hand + the calibrated forearm-in-hand (INV-558)
extern float RigElbowRateDps;
extern bool  RigLeftFlagHandshake;
extern bool  RigWorldWatchLocal;
extern bool  DiagDirtyFlags;
extern bool  RigE48FlagHandshake;
extern bool  RigLeftArm;
extern float RigLeftGripRollDeg;
extern bool  RigLeftMirrorDeviation;
extern bool  RigLeftWristSwapAxes;
// Roll correction between the forearm's and the hand's bone axis conventions.
extern float RigGripRollDeg;
// The other two axes of the hand correction - one angle cannot express a
// correction needing the other axes. Default 0, exact no-ops until tuned.
extern float RigGripPitchDeg;
extern float RigGripYawDeg;
extern float RigLeftGripPitchDeg;
extern float RigLeftGripYawDeg;
// Wrist-to-grip distance. The IK targets the WRIST, so the target steps back
// from the controller by this much along the hand axis - otherwise the wrist
// lands where the grip should be and the hand has to bend to compensate.
extern float RigHandLen;
// Signed nudge of the shoulder along the body's forward.
extern float RigShoulderForward;
// Fixed forward rotation of the clavicle - the real way to move a shoulder
// forward, since bone translations are never written.
extern float RigClavicleProtractDeg;
// How far the elbow swivels to line the forearm up with the hand. 0 is the old
// fixed world-down pole.
extern float RigElbowFollowHand;
extern float RigLeftElbowFollowHand;
// Reach mapping - the rig's arm is 0.5514 m, shorter than a real one.
extern float RigReachScale;
extern float RigReachSoft;
// rig_reach_margin_mm (INV-360): how far short of the bone sum the reach cap sits. Smaller = a straighter arm at full reach.
extern float RigReachMarginMm;
// rig_forearm_aim_achieved (INV-362): the forearm aims at the wrist the arm can reach, not at the unreached target.
extern bool  RigForearmAimAchieved;
// How much pronation is routed to r_foretwist rather than rolling the whole
// forearm bone, which turns the elbow as much as the wrist.
extern float RigTwistToTwistBone;
// rig_twist_chain / rig_twist_chain_mid (INV-355): the fraction of the forearm twist taken off the elbow and written
// along the forearm's twist bones through the [SUBTREE] pins, and the mid-forearm bone's share of it.
extern float RigTwistChain;
extern float RigTwistChainMid;
// rig_twist_chain_max_deg (INV-358): the most twist the forearm may show; the excess stays at the elbow. 180 = no cap.
extern float RigTwistChainMaxDeg;
// Measured off the rig itself (a child bone's local offset IS its parent's
// length), tunable because a wrong proportion is visible long before it causes
// the IK to fail to reach.
extern float RigUpperArmLen;
extern float RigForearmLen;
extern float RigClavicleShare;

// PHASE A of the IK work: measure the OpenXR -> game POSITION mapping instead of
// assuming the rotation remap carries over to it. Read-only. Logs where the
// right hand sits relative to the head, expressed in both spaces, so the axis
// permutation, the signs and the scale can all be read off one measurement.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagPositionMapping = false;
#else
extern bool DiagPositionMapping;
#endif

// Drive from the scePad poll - the update phase, and polled in flatscreen too.
// Installing from the camera hook would make this VR-only, which has already
// cost this project two whole test runs.
void BoneBlendTick();

// [AFWFG] (diag_afw_fg_probe, AFW item 2 stage 0): the hide test's phase for the bootstrap's draw census -
// -1 off, -2 settling / not ready, 0 everything shown, k = hide group k. Exported as dlvr_AfwFgProbePhase (ovr2xr.cpp).
int AfwFgProbePhase();
// afw_rb_fg_mask (AFW item 2 stage 1): the foreground draw ranges of the player model and the held items, for the
// bootstrap's mask - triples (index count, start index, base vertex). Exported as dlvr_AfwFgMaskRanges (ovr2xr.cpp).
int AfwFgMaskRanges(unsigned* out, int maxTriples, long* gen);
// WRIST HUD stage A / B: the player's (and the held items') draw ranges that skin one of 10 bones, each with the palette slot of the 10
// (L forearm / foretwist1 / foretwist / hand, R the same, 8 / 9 = the right / left held item's bone_body else bone_root; -1 = not in that
// submesh's list), and the 10 binds (3x4 rows, bind96 = 120 floats). Returns the range count.
// Exported as dlvr_WristArmTable (ovr2xr.cpp); wristquad reads the binds directly.
int WristArmTable(unsigned* tab, signed char* slots, int maxRanges, float* bind96, unsigned* bindOk, long* gen);
// AFW item 5: MaskCollect on any model; elems = its element count, -1 = not a model this walk can read.
int AfwNpcCollect(void* model, unsigned (*out)[3], int cap, int* elems);
// [ANIMLOD] (diag_anim_lod, INV-682): the engine's per-model animation rate by distance + the animation CPU. Game thread, from
// bodygrab.cpp: installs the two pass-through engine detours once, publishes the NPC census (CModelObject pointers, the main CLevel
// read through ILevel+8, 0 = not read) and writes the 5 s report. npcWhy names why the census is empty when it is.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA 2026-11-07 (tester package): constant false, the gated code is dropped
inline constexpr bool DiagAnimLod = false;
#else
extern bool DiagAnimLod;
#endif
void AnimLodTick(const uintptr_t* npcModels, int nNpc, uintptr_t cLevel, int hits, int nAi, const char* npcWhy);
// The weapon -> shader catalogue (INV-471): per-range owner + per-owner mesh / class / elements, same lock + generation as
// the ranges (dlvr_AfwFgMaskOwners); and one (weapon, shader) note from the bootstrap (dlvr_AfwNoteItemShader).
int AfwFgMaskOwners(unsigned char* owner, int maxRanges, char* mesh3x128, char* cls3x48, int* elems3, long* gen);
void AfwNoteItemShader(int hand, const char* mesh, const char* cls, int elems, int range, unsigned count,
                       unsigned long long vs, const char* vsClass, const char* vsWhy, const char* status);

bool BoneBlendInstalled();

// The RIGHT hand's held item, from the [ITEM] capture (entity+0x1020), for melee_r2_suppress. -1 = NOT KNOWN (no
// entity on CheckHeldItems' last call), 0 = EMPTY, 1 = exactly "WeaponVis" (melee), 2 = any other class. The class
// compare runs once per item change; this is one load. *clsOut (optional) is for logging only.
int RightHandItemKind(const char** clsOut);
// WRIST HUD stage B fix 2: hand h's held model (0 right / 1 left; 0 = empty hand) straight from the [ITEM] capture, and a copy of its
// RTTI class into cls (n bytes). Any thread. COST: one pointer load + a <= 48-byte copy.
uintptr_t HeldItemNow(int h, char* cls, size_t n);

// Snap turn (comfort_turn_mode=1). Queue one snap from the pad thread: dir +1 right, -1 left. camyaw::Hook applies
// it on the next LOCAL player update, or drops it (stale / not the normal look controller) and says why.
void SnapTurnQueue(int dir);
// 1 = the camyaw detour is committed and snaps will be applied; 0 = not installed yet; -1 = the install failed or
// was refused for this session. *why (optional) names the reason for 0 / -1.
int SnapTurnStatus(const char** why);
// Hand climbing P4's look probe (climbhand.cpp): LookController's look window and the player's camera / look yaw and look pitch this
// update - out = { maxYaw, maxPitch, minYaw, minPitch, camYaw, lookYaw, lookPitch } (the window NaN unless *isLook). Game thread.
bool HangLookSample(uintptr_t player, float out[7], bool* isLook);
// comfort_body_follows_head's status by name, once per change - ovr2xr.cpp calls it every pad poll (pad thread).
void HeadFollowStatusTick();

}  // namespace dlvr
