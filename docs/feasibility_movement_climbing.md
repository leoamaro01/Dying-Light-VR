# Feasibility study: VR-native movement and hand climbing

Status: research only. No code was changed. Written 2026-11-25 against fork `main` at `91fc1d9`.

This covers two requests:

1. Separate VR movement from body facing and the game's animations (direction, inertia, sprint angle, head shift,
   parkour camera takeover, air control).
2. Hand-based climbing: grab ledges with your real hands, pull to mantle or launch, climb poles by hand, and keep
   wall-run, tic-tac and the foot-driven step-overs as they are.

## How to read this

Every claim carries one of these tags:

| Tag | Meaning |
|---|---|
| **[CODE]** | Read in this repo's source. A file and line is given. |
| **[BIN]** | Read from the installed game binaries (strings / RTTI / PE headers) during this study. |
| **[DATA]** | Read from the game's own data files (`DW\Data0.pak`, `DW\Data3.pak`) during this study. |
| **[INFERRED]** | My reasoning from the above. Not verified. |
| **[UNKNOWN]** | Not found. Each one has an experiment in §4. |

RVAs are gamedll (`gamedll_x64_rwdi.dll`) unless they say engine. They come from the mod's own comments, which were
read from the **Steam** build (gamedll `97046bba`).

> **Build caveat [BIN].** The installed binaries in `C:\Games\Dying Light - Platinum Edition` have the same image size
> as the Steam build but **different link timestamps**: gamedll `0x6A4A96B8` against `0x6A4A8DD7` in
> `src/common/buildrva.h:34`, and engine `0x6A4A9509` against `0x6A4A8C7D`. `buildrva` will class them as "unknown"
> and use the Steam RVAs unchanged. Every patch site still checks its bytes. The mod clearly works on this install, so
> the code is very likely identical and only re-stamped. Still, any **new** RVA found by static analysis must be checked
> against the bytes in *this* file, not only the Steam one. The banner in `dlvr_ovr2xr.log` says which build it saw.

---

## 0. TL;DR

- **Walking is controller-driven, not root motion [CODE].** The player walk (`+BA2B20`) builds a world velocity
  `w = Root3x3 * l` from a local walk vector at `PlayerDI+4616`. It hands that to the walk-speed setter `+ED2040`,
  which drives the engine's physics walk capsule (`IPhysicsWalkConfig`, engine exports [BIN]). **The mod already
  detours `+ED2040`** (`walklook`, `src/ovr2xr/boneblend.cpp:22656`). That is the single best lever for item 1. It
  can rewrite the direction of every ground step without touching the animation system.
- **Body yaw is already decoupled from the head [CODE].** The "body follows head" fold writes the engine's four yaw
  fields (`+4460/+4468/+4588/+4592`). `walklook` puts walking back onto the look. What is left of complaint 1a is
  probably cases where `walklook` passes the call through. Its own 5 s log line counts them.
- **The sprint angle and the turn "inertia" come from what the game sees on the stick [CODE + INFERRED].** The
  smallest fix uses two injection points the mod already owns. The pad thread tells the game "stick straight forward,
  this hard". The `+ED2040` hook then turns the resulting velocity to the real stick direction. The game never sees a
  direction change, so it never plays a turn-start or drops the sprint. **This is the recommended first prototype.**
- **Climbing is fully animation-sequenced [DATA].** It runs in `EdgeGrabController` (controller id 24,
  `PlayerDI+3168`, RTTI-checked by the mod [CODE]). Ladders, pipes ("poles") and ropes run in `LadderController`, in
  discrete hand-over-hand steps (`ClimbUpL` / `ClimbUpR`). The cheapest believable hand climbing keeps all of that
  and replaces only the **trigger** and the **selection**: hands choose and start the game's own climb, then the
  game's own animation and physics carry it out. A true "your hands move your body" climb is possible, but it is a
  new locomotion mode with large unknowns.
- **Data levers exist [DATA].** `default_levels.xml` / `zombie_levels.xml` show progression props:
  `AllowEdgeHanging`, `JumpFromHangEnabled`, `AutoJumpLowObstacles`, `JumpMechanicType`,
  `LogicJumpAirControllFactor` (0.22), `TurnSpeedAirMovementFactor_*`, `MoveAcceleration`,
  `MoveSprintAcceleration` and more. The mod already ships its own fixed `Data3.pak`, which overrides Data0 files
  (it does so for `inputs_pad.scr`). So a few complaints, air control first of all, may need only data.

---

## 1. Shared facts both items rest on

### 1.1 The player's controllers

- **[CODE]** PlayerDI holds a controller table at `+2976 + 8*id`. Each controller has an "active" byte at `+16`
  (`PlayerController::Start/Stop`) (`looting.cpp:508-512`, `boneblend.cpp:19780-19786`). Known ids:

  | id | offset | class | source |
  |---|---|---|---|
  | 0 | +2976 | `MoveController` | `boneblend.cpp:18279` (INV-327) |
  | 10 | +3056 | `RepairWeaponController` | `boneblend.cpp:19784` |
  | 21 | +3144 | `DialogController` | `dlgview.cpp:34` |
  | 23 | +3160 | `DefensiveController` (kicks, slides; state at +56, 1..11) | `boneblend.cpp:19782-19783` |
  | 24 | +3168 | `EdgeGrabController` (climb; phase at +504, 3 = weapon shown) | `boneblend.cpp:19777, 19785`, `Verify(..."EdgeGrabController"...)` at `:19951` |

- **[BIN]** Other player controllers by RTTI in gamedll: `LadderController` (+`ILadderInfo`, `LadderDI`),
  `LandingController`, `ZipLineController`, `SwimController`, `RopeLocomotionController` (grapple),
  `NaturalMovementController` / `NaturalMovementHook`, `LookController`, `GrabController`, `WeaponThrowController`.
  Their table ids are **[UNKNOWN]**. The `Locomotion*Controller` family (`LocomotionMoveTurnController`,
  `LocomotionTurnInPlaceController` and others) is most likely AI locomotion, not the player's **[INFERRED]**.
- **[CODE]** `MoveController` facts the mod already uses: jump type at `mc+448` (8/9/10 = dodge left/right/back,
  `boneblend.cpp:18268-18280`); sprinting byte at `mc+72` (`IsSprinting`, `+D4A6F0`, `physcrouch.cpp:17`); update
  `+D47F20` (`physcrouch.cpp:19`); `IsBodyVectorRefToLook` `+D57B10` (`boneblend.cpp:21818`); the active
  `IMoveController` supplier is `+BA2980` (`boneblend.cpp:22660`).

### 1.2 The four yaw fields and who writes them [CODE]

From the SNAP and camyaw comment blocks (`boneblend.cpp:21318-21345`, `20915-20940`):

| field | meaning |
|---|---|
| `+4460` | smoothed look. The **camera** and the shot read it. The mod adds the head yaw here (design D2, detour `+BA1110`). |
| `+4468` | look target. The **body** follows it. `LookController` integrates the stick into it. |
| `+4588` | body **facing**, written into the root by `+BA2500`. |
| `+4592` | facing target. `+BA1FD0` moves it toward `+4468` at a **capped rate**. |

- "Body follows head" (`comfort_body_follows_head`, default ON, `ovr2xr.cpp:1666`) folds the head yaw into all four
  fields (`headfollow_logic.h:36-50`). "Torso from controllers" then sets the facing to look plus a gap read from
  the grips.
- A **facing supplier** (`+BB7BF0`) can override the facing target. While `WeaponThrowController`,
  `EdgeGrabController` or `DefensiveController` is the supplier, it returns `+4460` as the facing target
  (`boneblend.cpp:21449-21510`, vtables `+1734548`, `+172CAF8`, `+172C058`).
- While `IsBodyVectorRefToLook` is true (wall kick), the look integrator `+BA1350` clamps `+4468` into a window
  around the **body** (`boneblend.cpp:21809-21818`).
- `LookController` narrows its own look limits in "climbs / ladders / slides / landings / sprint-angle states"
  (`SetSpringWantedAngles` windows, INV-772, `boneblend.cpp:21051-21054`). The mod already undoes the **pitch** half
  of this when the limits release (`lookpitch`).
- Snap turns are **dropped** whenever the active look controller is not `LookController` (rope, cutscene, vehicle,
  grab) (`boneblend.cpp:21340-21341`).

### 1.3 The walk path [CODE]

`boneblend.cpp:22640-22832` (`walklook`) and `headfollow_logic.h:416-479` (`WalkOnLook`):

- The player walk `+BA2B20` calls `+ED2040` (the walk-speed setter) exactly once, returning to `+BA3A7D`, with the
  world walk vector `w`.
- `w == M3x3 * l`, where `M` is the root's world transform (`IControlObject` at `player+24`, `GetWorldXform`) and
  `l` is the **local** walk vector at `player+4616` (+X left, +Y up, +Z forward). `WalkOnLook` checks this to within
  1 mm/s or 0.5 % on every call and refuses otherwise.
- The hook calls the original with a **local copy** and never writes player fields. Its 5 s `[WALKLOOK]` line counts
  turned calls and every pass-through reason: not the local player, fold not folding, controller not
  `MoveController`, non-finite, root not upright, self-check failed, fault.
- **[BIN]** The engine exports an `IPhysicsWalkConfig` family (`GetPhysicsWalk`, `GetRadiusXZ/Y`,
  `EnableJumpHeightRestriction` and others). The player is a physics walk capsule.

**Conclusion [CODE + INFERRED]:** ground movement is velocity-driven. The game decides a local velocity `l`, rotates
it by the root and gives it to physics. Animations are chosen *from* that state, but they do not move the capsule
while walking. Whether `l` itself is shaped by animation data (`MoveAnimData` / `MoveAnimNode` exist [BIN]) is
**[UNKNOWN]**. It matters for the inertia complaint (§4, E2).

### 1.4 Input injection the mod already owns [CODE]

- **scePad bytes** (pad thread, `ovr2xr.cpp:12960-13060`): left stick X/Y (`p[0x04]`, `p[0x05]`), right stick X
  (`p[0x06]`) and L3 (sprint, `vr_sprint`). `vr_decouple_move` (default ON, clamped by
  `vr_decouple_max_deg` = 30) still rotates the stick by the head yaw. The comment there states the sprint conflict:
  "the game only sustains sprint while the stick reads near-forward … confirmed in play".
- **Game actions** through the InputManager's own pad entry (slot 148, `+110FD80`), using a synthetic button code
  `0x0F0F` (`EJoy__BUTTON_16`). The pad never sends it; the mod's fixed `Data3.pak` `inputs_pad.scr` binds it
  (`physcrouch.cpp:6-12`). This is how physical crouch holds `_ACTION_DUCK`. The same route can press **any**
  action the mod binds to a spare code.
- The **Controls tab** writes `DW\dlvr_binds.scr` every launch from `dlvr\binds.txt`
  (`bootstrap/pakgen.cpp:1284-1365`, generated table `common/binds_table.gen.h`). Relevant actions: `_ACTION_JUMP`
  (with `Hysteresis(0.3, 0.3)`) and `_ACTION_CLIMB_UP` / `_ACTION_CLIMB_DOWN` on left-stick Y (`binds_table.gen.h:440-443`).

### 1.5 Data the game reads for movement [DATA]

`data/skills/default_levels.xml` (Data0) sets the player's progression props. Movement-relevant defaults:

| prop | default | likely role [INFERRED unless obvious] |
|---|---|---|
| `MoveForwardMaxSpeed` / `MoveBackwardMaxSpeed` / `MoveStrafeMaxSpeed` | 3.5 / 2.5 / 2.5 | walk speeds per direction |
| `MoveSprintSpeed` | 6.0 | sprint speed |
| `MoveAcceleration` / `MoveDeceleration` | 40 / 40 | m/s² walk ramp (0→3.5 m/s in ~0.09 s) |
| `MoveSprintAcceleration` | 10 | m/s² sprint ramp (0→6 m/s in ~0.6 s) |
| `LogicJumpAirControllFactor` | 0.22 | **air control** strength |
| `MinTimeBetweenJumps` | 0.4 | |
| `WallRunExtraGravity` / `WallStickExtraGravity` | 9.5 / 15 | wall-run tuning |
| `JumpFromHangEnabled` | 1 | jump off a hang |
| `HookClimbingEnabled` | 1 | |
| `FovSprintModif` | 4.5 | sprint FOV kick |

`data/skills/zombie_levels.xml` (the Be-the-Zombie player) shows props the human player leaves at their code
defaults: `AllowEdgeHanging` = 0, `JumpFromHangEnabled` = 0, `AutoJumpLowObstacles` = 1,
`JumpMechanicType` = "Hold", `TurnSpeedAirMovementFactor_MinTurn` / `_MaxTurn` / `Max` = 90 / 200 / 0.25, and
`MaxJumpHoldTimeSeconds`. The skill tree (`common_skills.xml`) raises `NaturalMovementDevelopmentLevel` at
`runningprof1` and `runningprof2`. `WallRun` (tic-tac) sets `WallRunFrontHeight` and `WallJumpBackEnabled`.

**[BIN]** gamedll strings with no data default found: `JumpInertiaFactor`, `DuckInertiaFactor`,
`HandInertiaFactor`, `IsMoveTurning`, `IsMoveTurnSudden`, `IsCheckingMoveTurn`, `BlockTurnOnMoveAttack`,
`SprintForward_StartR90`, `EnableClimbing`, `BlockJumpAndSprint`, `EdgeGrabMode`,
`EyeTrackingNatMovAdjustingDir`, `EyeTrackingNatMovAutoClimb`. The last two show the game already lets a **Tobii eye
tracker** steer natural-movement direction and auto-climb. So a non-camera input already feeds the parkour code
path somewhere [INFERRED].

**Can the mod change these?** The mod ships its own fixed `Data3.pak` (co-op requires the same Data3 on every
machine, U-56, `pakgen.cpp:63-80`). Data3 already replaces Data0's `data/scripts/inputs/inputs_pad.scr` [DATA]. So a
`data/skills/default_levels.xml` in Data3 would most likely win too **[INFERRED]**. The catch: XML has no `if`, so
the override would also apply in **flat** mode unless the launcher swaps it. The `dlvr_pp.mth` script-import trick
only works for `.scr` files.

### 1.6 The camera position comes from an animated bone [CODE]

`[EYEPOINT]` (`boneblend.cpp:33036-33045`): `+C75380` (PlayerVis slot 371) sets the camera base `vis+4112` from
`GetElementWorldPos(eye bone)`, and the mod adds `cam_eye_forward` and the arc offset on top. The eye bone is
animated, so **every body animation (run cycle, turn start, landing, climb) moves the view base**. That is the
"head and body shift a lot" complaint. The mod already detours this exact function.

---

## 2. Item 1: separate movement from body direction and animations

### 2.1 What controls each complaint today

| # | Complaint | What controls it | Evidence | Confidence |
|---|---|---|---|---|
| 1a | Movement follows body facing, not the look | Walking is in the root's frame (INV-652). With body-follows-head ON, the fold plus `walklook` put walking on the look. `walklook` passes through when the fold is stale (>100 ms), the controller isn't `MoveController`, the root isn't upright, or its self-check fails. With body-follows-head OFF, the body turns only with the stick and only `vr_decouple_move` (±30°) corrects it. | [CODE] `headfollow_logic.h:416-479`, `boneblend.cpp:22735-22760`, `ovr2xr.cpp:12997-13033` | High that the levers are right. Which pass-through hits the user is **[UNKNOWN]** (E1). |
| 1b | Direction changes lag (an invisible turn animation) | Candidates: (i) `MoveController` move-turn states (`IsMoveTurning`, `IsMoveTurnSudden`, `SprintForward_StartR90`) holding or redirecting `l`; (ii) the acceleration ramps (sprint 10 m/s² is slow); (iii) the facing chase `+BA1FD0` at a capped rate, although the fold writes the facing directly, so (iii) should be mostly gone with body-follows-head ON. | [BIN] strings, [DATA] accel values, [CODE] `boneblend.cpp:21330` | **[INFERRED]**. Which one dominates is **[UNKNOWN]** (E2). |
| 1c | Sprint only near straight ahead | `MoveController` drops sprint when the stick leaves a forward cone. Located near its update `+D47F20` / `IsSprinting` byte `mc+72`. `LookController` also has "sprint-angle states". No data prop for the cone found. | [CODE] comment `ovr2xr.cpp:13004-13013`, `physcrouch.cpp:17-19`, `boneblend.cpp:21051` | Behaviour confirmed in play. Exact code and constant **[UNKNOWN]**. The skill that widens it was not found in data (it may be code-checked by name, or the `NaturalMovementDevelopmentLevel` counter) **[UNKNOWN]**. |
| 1d | Head and body shift on direction changes | The camera base is the animated eye bone (`+C75380`). Plus the sprint FOV kick (`FovSprintModif`). | [CODE] `boneblend.cpp:33036-33045`, [DATA] | High. |
| 1e | Parkour turns the view (shimmy faces the wall) | `EdgeGrabController` becomes the facing supplier (returns `+4460`). `LookController` narrows its windows in climbs. Wall kick: `IsBodyVectorRefToLook` clamps the look to the body. D2 adds the head yaw *on top* of `+4460`, so when the game moves the base `+4460` toward the wall, the world swings under you even with your head still. | [CODE] `boneblend.cpp:21449-21510, 21051, 21809-21818` | High for the mechanism. Which exact field moves during a shimmy is **[UNKNOWN]** (E3). |
| 1f | Air control is weak | `LogicJumpAirControllFactor` 0.22 (data). `TurnSpeedAirMovementFactor_*` (data, code default for the human player). `JumpInertiaFactor` (code). Whether airborne motion still goes through `+ED2040` is unknown. | [DATA], [BIN] | Data levers: high. The code path in the air: **[UNKNOWN]** (E4). |

### 2.2 Approaches, smallest change first

**A0. Settings and data only. Effort S, confidence medium.**

- Confirm `comfort_body_follows_head=1` and read the `[WALKLOOK]` 5 s lines (E1). If "fold not folding" or "self-check
  failed" counts are high during normal walking, 1a is a bug in an existing feature, not a design gap.
- Ship a Data3 `default_levels.xml` with a higher `LogicJumpAirControllFactor` (try 0.5-0.8),
  `TurnSpeedAirMovementFactorMax`, and a higher `MoveSprintAcceleration` (a shorter sprint ramp means less "lag").
  The cost: it changes flat play too unless the launcher swaps the file per mode, and it changes co-op balance for
  everyone on that Data3.
- *Breaks:* nothing in code. Air-control values that are too high may break the game's assumptions on long jumps or
  landing on cars and zombies (it may become *easier*, which is the goal).

**A1. "Canonical forward" stick and a turn at the walk call. Effort S-M, confidence medium. Recommended first.**

This uses only the two injection points the mod already owns:

1. **Pad thread** (`ovr2xr.cpp`, where `p[0x04]` / `p[0x05]` are written): while on the ground in `MoveController`,
   send the game `stick' = (0, |stick|)`, meaning "straight forward, this hard". Keep the real stick angle θ (relative
   to the look) in a shared variable.
2. **`walklook::Decide`** (`+ED2040`): rotate the world vector by θ about +Y after `WalkOnLook` has put it on the look.
   The magnitude stays the game's.

The game then always sees a forward push. Sprint never drops at an angle (1c). No turn-start or move-turn state ever
fires (1b, if E2 confirms that is the cause). The run animation never changes direction, so the eye bone shifts less
(1d, partly). Direction changes become instant in world space, while starting and stopping keep the game's own
acceleration.

Gates, all needed. They follow the existing `walklook` refusal style:
- only while the active `IMoveController` is `MoveController` and `mc+448` (jump type) is 0, meaning on the ground
  and not dodging;
- pass the **real** stick through while jump is pressed or held, so dodges (jump type 8/9/10), wall-run and tic-tac
  see a real direction;
- pass the real stick through while `DefensiveController` or `EdgeGrabController` is active, and in menus (already
  gated by `gameMenu`).

*What it changes in the game:* backward and strafe moves get forward or sprint speed (3.5 to 6 m/s instead of 2.5).
That is arguably right for VR, and a clamp from the real angle could scale it back to the game's own per-direction
max. Peers in **co-op** see your third-person body run "forward" while you travel sideways, because the TPP animation
follows the game's state. Sprint stamina use and noise follow the game's sprint logic unchanged.

*Breaks:* wall-run entry if it is decided from the stick on the ground rather than in the air **[UNKNOWN]**.
Sliding (physical crouch) needs sprint, and stays fine. The look-back skill (`LookBack`, if bound to stick-back) needs
the real stick.

*Test:* the stick angle at which sprint drops, and the time a 90° direction change takes, compared with and without
it (E2's numbers).

**A2. Stabilise the camera base. Effort S-M, confidence medium-high.**

In the existing `+C75380` detour: while `MoveController` is active and on the ground, replace the animated eye
bone's offset from the root with a filtered, capsule-fixed standing eye height. The HMD's own height and offset stay
as they are. This removes run bob, turn-start sway and landing dips from the view, while the body still animates.
The mod's `cam_eye_*` options already add offsets at exactly this point. *Breaks:* physical crouch reads the duck
factor at `vis+1332` from the same update (`physcrouch.cpp:15`), so keep that. Climbs should keep the animated base
(it is the only cue for where the climb goes), or blend it out slowly. Needs a run in the headset to judge comfort.

**A3. Hold the view through parkour takeovers. Effort M, confidence medium.**

In `camyaw::Hook`: while a facing supplier other than "none" is active, or the look window is narrowed
(EdgeGrab, Ladder, Defensive, landings), cancel the engine's own change to `+4460` from the addend each update. The
camera's world yaw then stays where the head puts it, and the body and arms still turn with the game. The mod already
has the pieces: the `yawstep` probe reads the owners (`boneblend.cpp:21449-21560`), the hand-twist view hold does a
similar cancellation (`headfollow_logic.h` HAND-TWIST VIEW HOLD), and the wall-kick fix gates on
`IsBodyVectorRefToLook`. *Breaks:* the shot direction also reads `+4460` (INV-264), so aiming while hanging would
follow the view, not the body (arguably correct). Exiting the hold must hand back without a jump; the snap and fold
code shows how (move every field by the same amount). Snap turns are currently dropped while the look controller is
not `LookController`. Whether EdgeGrab keeps `LookController` as the look controller is **[UNKNOWN]** (E3). If it
does, snap turning while hanging already works and only the base swing needs cancelling.

**A4. Air control at the walk call. Effort M, confidence low until E4.**

If airborne frames still call `+ED2040` from `+BA3A7D`, blend the in-air velocity's horizontal part toward the
stick's direction at a rate we choose. That would be more direct than the data factor. If the air path does not go
through `+ED2040`, A0's data values are the only cheap lever. *Breaks:* jump distance balance; the game's
"land on a car or zombie to break a fall" checks are unaffected by direction, but faster mid-air steering makes them
easier to hit.

**A5. Replace `MoveController` locomotion with our own controller. Effort XL, confidence low. Not recommended.**

Own every ground state (walk, sprint, crouch, slide), write the capsule velocity directly and drive animations from
it. This is the only route to "no animation decides anything", but it reimplements stamina, noise, XP, slide,
tackle, co-op replication and AI awareness, all of which key on `MoveController` state.

### 2.3 Risks for item 1

- **Co-op** [INFERRED]: A1 and A3 change only the local view and the local velocity. Peers see the game's state,
  such as a forward run while you strafe. A5 would desync.
- **Epic build:** A1 to A3 add **no new RVAs** (they reuse `+ED2040`, `+BA1110`, `+C75380` and the pad thread), so
  no EGL table work. A4 too, if E4 shows the call. Any new site needs the generated `buildrva_table.h` row from the
  private tool (AGENTS.md).
- **Interaction with the existing yaw stack**: fold, torso gap, wall-kick gate, snap and lookpitch. Every change to
  the four yaw fields must move all four by the same amount, or springs pull back. That is the main way A3 can go
  wrong. Spins like WK1 (`boneblend.cpp:21809`) show the failure mode.
- **Comfort**: A2 and A3 make the view steadier than the body. Testers may find the body visibly "sliding" under them
  in a mirror or shadow.

### 2.4 Effort and confidence (item 1)

| Approach | Effort | Confidence it works | Complaints it targets |
|---|---|---|---|
| A0 data / settings | S (hours plus a Data3 build) | medium | 1a (diagnosis), 1f, partly 1b |
| A1 canonical forward | S-M (1-3 days incl. gates and logic tests) | medium | 1b, 1c, partly 1d |
| A2 camera base | S-M | medium-high | 1d |
| A3 view hold in parkour | M (3-5 days, headset-heavy) | medium | 1e |
| A4 air control at walk call | M | low until E4 | 1f |
| A5 own locomotion | XL | low | all |

---

## 3. Item 2: hand-based climbing

### 3.1 What controls climbing today

- **Ledge climbs** run in `EdgeGrabController` (id 24, `PlayerDI+3168`, RTTI-verified [CODE]). Its facing vtable is
  `+172CAF8`, slot 6 = `+D0E390` [CODE]. The animations are fixed sequences [DATA]:
  - `m_fpp_nm_climb.scr`: `RoofJump_EdgeGrab`, `RoofJump_FromBottom`, `RoofJump_FromBottomNoHang` (a short
    vault-up), each with a `CLIMBING_LENGTH` event.
  - `m_fpp_nm_hang.scr`: `HangFromEdge` (+`_Begin`, `_Left/_Right`, `_BackLeft/_BackRight`, `_Jump`,
    `_Jump_Left/_Right`), `ShimmyLeft/Right` (+`Fail`), `ShimmyUp` (+`_Left`), `HangClimbFail`, `DamageFall`.
  - The strings `ShimmySearch*`, `SpaceToHang`, `FromBottomNoHang`, `EdgeHang`, `EdgeHangJump` and
    `ReachClimbingTarget` [BIN] suggest the controller searches for a target and moves the body along a computed
    path timed to the animation **[INFERRED]**. Whether that motion is root motion from the clip or a
    procedural blend toward `ReachClimbingTarget` is **[UNKNOWN]**.
- **Ladders, pipes ("poles") and ropes** run in `LadderController` / `LadderDI` / `ILadderInfo` [BIN].
  `m_fpp_ladder.scr` [DATA] has `{Ladder|Pipe|Rope}_ClimbUpL/R`, `ClimbDownL/R`, `EnterUp`, `EnterDownL/R`,
  `LeaveUp/Down`, `Jump`, `Slide`, `LookLeft/Right`, and for pipes and ropes `RotateLeft/Right`. These are discrete,
  alternating-hand steps, which suits hand-over-hand input.
- **Wall-run and tic-tac** are jump-family clips in `m_fpp_nm_jump.scr` (`WallRunFront/Left/Right`,
  `WallJumpFront/Back/Left/Right…`) [DATA]. They are probably `MoveController` jump types, separate from EdgeGrab
  **[INFERRED]**.
- **Step-overs**: `JumpOverLow`, `JumpOnLowStand` and `JumpOverEnemy` are jump clips [DATA]. Knee-height steps are
  most likely the physics walk's step height, with no action at all **[INFERRED]**. `AutoJumpLowObstacles` exists as
  a prop (BtZ = 1) [DATA].
- **The trigger** is `_ACTION_JUMP` held while looking at a ledge [user report]. Input bindings: `_ACTION_JUMP` and
  `_ACTION_CLIMB_UP/DOWN` [DATA]. Game data can switch hanging off (`AllowEdgeHanging` 0 in BtZ) and jumping off a
  hang (`JumpFromHangEnabled`), and quests use `EdgeGrabMode("FromTop")` as a script condition [DATA].
- **What the mod already does around climbs** [CODE]: `item_hide_by_state` hides the held weapon while EdgeGrab is
  active; `item_rel_hold=3` pins the grip through the climb; `lookpitch` restores pitch after a climb's narrowed
  limits; `yawstep` names EdgeGrab as a facing supplier; `[EYEPOINT]` has a climb tail.
- **Tools the mod has for a hand-driven version** [CODE]:
  - World raycasts: `IGSObject::Raytrace` (engine export, already IAT-hooked by the grapple, `grappleaim.cpp:49`).
  - Hand poses in game world: the rig's `SolveArmIk` targets.
  - Grip state machines with ownership arbitration in the pad thread: holster, two-hand, loot, body grab and push
    grab (`pushgrab.h`).
  - Action injection: §1.4.
  - Haptics: `HandCollisionBuzz*`.
  - Hand collision exists for enemies only, not world geometry (`handcoll.h`).
  - The grapple has a **ledge assist** (`+D7D900`) that the mod already lets start at the hand
    (`grappleaim.cpp:42-78`). That shows the game's ledge search can take a non-camera origin.

### 3.2 Approaches, smallest change first

**C1. Hands trigger and steer, the game executes. Effort M, confidence medium. Recommended.**

Keep `EdgeGrabController`, `LadderController` and every animation. Replace only how they are started and steered.

1. **Turn off look + hold-jump grabbing, keep jumps and vaults.** Three options, best first:
   - *(a)* Detour `EdgeGrabController`'s start / can-start path (a vtable slot, found by RTTI like the mod's other
     slots) and refuse unless "a hand asked". This is precise: wall-run, tic-tac and vaults are not EdgeGrab, so they
     are untouched **[INFERRED, verify with E5]**. The RVA is **[UNKNOWN]**; it needs one static-analysis session.
   - *(b)* No reverse engineering: in the pad thread, stop forwarding *held* jump after take-off unless a hand grip
     requests a grab. A tap still jumps and vaults. Risk: wall-run and tic-tac use held jump too, which breaks
     "keep exactly as they are".
   - *(c)* Data: `AllowEdgeHanging=0`. Too blunt (it removes hanging altogether), but a useful one-line experiment
     to see what else it switches off.
2. **Grab with a hand.** A grip close (on an empty hand, arbitrated like loot and push grab) within reach of a ledge
   (two raycasts from the hand: down onto the top, forward onto the face) starts the game's grab. Make the game's
   ledge search use **our** point instead of the camera's: the grapple R9 shows the pattern (an IAT swap on the
   camera ray, filtered by return address). Which ray or which inputs the EdgeGrab search uses is **[UNKNOWN]**. That
   is the main reverse-engineering task. Fallback: hold the look on the ledge for one update while pressing the grab
   (fragile).
3. **While hanging** (EdgeGrab active, hang phase): grip held = stay. Both grips released = drop (inject the drop or
   crouch action; which action drops in DL1 is **[UNKNOWN]**). Hands moving sideways = inject left-stick X (shimmy).
   **A quick downward pull** = inject the climb-up input (jump while hanging climbs in DL1 **[INFERRED]**), which
   gives a mantle if there is floor (the game already chooses `HangFromEdge` → climb vs `HangClimbFail`). **A pull
   with the hand reaching above a higher ledge** = `ShimmyUp` / `HangFromEdge_Jump` via up + jump. **A push away from
   the wall** = jump back (`HangFromEdge_BackLeft/Right`).
4. **Look and turn while hanging:** A3 above, plus snap turn (E3 settles whether snap already applies).
5. **Hands on the ledge:** while a grip holds, pin that hand's IK target to the grab point instead of the controller,
   so the hands "stay" on the ledge while the body animates. The rig's `HandCollisionStop` shows how a target is moved
   inside the solve.

*What it keeps:* XP (`RunnerXPClimbEdgeGrab` and others), stamina, co-op replication, quests (`EdgeGrabMode`) and
fail states, because the game runs every climb. *What it does not give:* continuous "the body moves exactly with
my hand". Each move is still a game animation, now started by a hand gesture.

**C1-L. Ladders, pipes and ropes hand-over-hand. Effort S-M, confidence medium-high.**

Once `LadderController` is active, map alternating hand pulls (one hand grips above the other, then pulls down) to
`_ACTION_CLIMB_UP` / `_ACTION_CLIMB_DOWN`, which are left-stick Y **[DATA]**. The stick byte is injected already.
A loosened grip while held = slide (`*_Slide` clips). Entering a pipe or ladder still uses the game's own entry: it
is started by touching it or pressing use **[UNKNOWN which]**. A grip on the pipe could inject that. This is
input-only, needs no new RVAs except finding `LadderController`'s slot and active flag (RTTI, like EdgeGrab), and is
the best first hand-climb prototype.

**C2. Hand-driven hang offset. Effort L, confidence low-medium.**

Inside the game's hang state (gravity off, the controller holding the body at the ledge), add a bounded offset
from the hands: body = anchor − (hand − grabPoint), clamped to arm reach. Moved by `IControlObject::SetWorldPosition`
(engine export) or by the controller's own anchor fields **[UNKNOWN]**. Letting go beyond a limit hands back to the
game's fall. This gives the VR feel *while hanging*, and C1 still does the transitions. Risk: the controller may
re-pin the body every update (fighting) or replicate the anchor to peers.

**C3. Own climbing locomotion. Effort XL, confidence low.**

A new state: freeze `MoveController` gravity and physics walk; raycasts confirm a grabbable edge, wall or pole; the
capsule moves by hand deltas; mantle = a capsule-fit test on top plus a scripted lerp; launch = a set velocity on
release. It feels exactly like VR games. Unknowns: how to suspend gravity safely, how to stop the game's own
controllers from taking over (EdgeGrab, Landing, fall damage), how to replicate in co-op, and losing parkour XP and
quests unless faked. It reimplements the ledge search the game already has.

### 3.3 Other hand-based parkour moves worth covering

From the animation and controller lists [DATA/BIN], ordered by how naturally they map to hands:

1. Ledge catch from a fall or jump (`RoofJump_EdgeGrab`): reach up and grip while airborne.
2. Short climb-ups (`FromBottomNoHang`): both palms on top of a chest-high wall + push down = vault up.
3. Shimmy left/right/up (`Shimmy*`), hang-jump sideways (`HangFromEdge_Jump_Left/Right`), push off backwards
   (`HangFromEdge_BackLeft/Right`).
4. Ladders, pipes and ropes: hand-over-hand, slide by loosening the grip, rotate around a pipe
   (`Pipe_RotateLeft/Right`) by twisting the body.
5. Ziplines (`ZipLineController`): raise a hand to the line and grip; let go to drop.
6. Grapple (already hand-aimed by R9).
7. Helping an NPC on a ledge (`HelpSafeNPC_Ledge`): a reach-down grab is a natural fit, but quest-specific.
8. Balance beams (`*_Ballance_*`): arms only, visual. Probably leave as is.

Leave as they are, as asked: wall-run and tic-tac (jump clips, a different controller), step-over and
`JumpOverLow` / `JumpOnLowStand` (consider `AutoJumpLowObstacles=1` so they no longer need jump at all **[DATA]**),
slides and drop kicks (`DefensiveController`).

### 3.4 Risks for item 2

- **Disabling the old trigger** may break tutorials and quest prompts ("hold jump to climb") and bounties gated on
  `EdgeGrabMode("FromTop")`. Keep a setting to restore the game's trigger.
- **Wall-run and tic-tac** break if the jump-hold path is touched (C1 option b). Option a avoids it.
- **Co-op**: C1 and C1-L are replicated by the game; C2 may jitter for peers; C3 likely desyncs.
- **The weapon in hand**: climbs already hide or pin it (`item_hide_by_state`, `item_rel_hold`). Hand climbing with
  a weapon in the gripping hand needs a rule (holster on grab, or one-handed grab only with the free hand).
- **Grip conflicts**: left grip = L1, right grip = R1 today (`pushgrab.h`), and those are game actions. Any new
  grip use must go through the same "owned" arbitration and withhold the L1/R1 forward.
- **Comfort**: game animations under a hand-started climb still move the camera (climb-up arcs). A2's camera base
  and A3's view hold matter more here.
- **Static analysis on this install** must check the bytes in the installed (re-stamped) binary, not only Steam's.

### 3.5 Effort and confidence (item 2)

| Approach | Effort | Confidence |
|---|---|---|
| C1-L ladders/pipes/ropes hand-over-hand | S-M | medium-high |
| C1 hand-triggered edge grab + hang gestures | M (plus one RE task: the EdgeGrab start and search inputs) | medium |
| C2 hand-driven hang offset | L | low-medium |
| C3 own climbing locomotion | XL | low |

---

## 4. Unknowns and the experiments that settle them

Experiments marked **(no build)** use the current deployed build. The others need a small log-only probe, which is a
small, separate change.

| # | Question | Experiment |
|---|---|---|
| E1 | Why does movement still follow the body for the user (1a)? | **(no build)** In the overlay, check *Body follows head* is ON. Walk, then sprint in a straight line for 20 s while turning your head left and right 45°. Then repeat with your whole body turned. Send `dlvr_ovr2xr.log` and look for `[WALKLOOK] 5 s` lines: "turned to the look" should dominate. High "fold not folding" / "self-check failed" / "not MoveController" counts name the cause. Also note whether `vr_decouple_move` is still ON. |
| E2 | What makes direction changes lag (1b), and at what stick angle does sprint drop (1c)? | Probe at `walklook::Decide` plus the pad thread, per update for 3 s after each stick direction change: stick (x, y), `l` (+4616), `\|w\|`, `+4588`, `+4468`, `mc+448`, sprint byte `mc+72`. In the headset: stand still, flick the stick forward → right → back → left, 1 s each, walking then sprinting; then sprint and slowly rotate the stick from forward to the side. This shows whether `l` turns gradually (in-controller inertia), drops to zero (turn-start state) or turns instantly (then the lag is the facing chase), and the exact sprint cut-off angle. |
| E3 | Who owns the look and facing while hanging and shimmying, and which field moves (1e)? | **Needs the yaw-probe build** (`-DDLVR_YAW_PROBE=ON`, own build folder, `diag_cam_yaw=1`): grab a ledge, hang 3 s with your head still, shimmy left 3 steps, climb up. `[YAWSTEP]` OWNER CHANGE lines give the look controller and facing supplier. JUMP lines give how far `+4468` moved. Also try a snap turn while hanging and read `[SNAP]`: applied or "not-look". |
| E4 | Does airborne motion go through `+ED2040`? | Probe: count `walklook` matched calls per update while `mc+448 != 0` or the player is off the ground. In the headset: 10 standing jumps, 10 running jumps pushing the stick sideways mid-air. If the counts are zero in the air, A4 is not possible this way. |
| E5 | Is wall-run/tic-tac separate from `EdgeGrabController`? | **(no build, read-only)** The existing `item_hide_by_state` hook logs when EdgeGrab goes active (the climb key). Do 3 wall-runs, 3 tic-tacs and 3 ledge grabs; the weapon disappears only for real climbs if they are separate. Watching the held weapon is enough. |
| E6 | Do Data3 overrides of `default_levels.xml` take effect, and what does `AllowEdgeHanging=0` switch off? | A test Data3 build carrying `default_levels.xml` with `LogicJumpAirControllFactor=0.6` and, in a second run, `AllowEdgeHanging=0` and `AutoJumpLowObstacles=1`. Check air steering; check what still climbs (ledges from below? vaults? pipes?). |
| E7 | Which inputs does the EdgeGrab ledge search use (camera ray, body forward, look)? | Static analysis: find EdgeGrabController's vtable by RTTI (COL), follow its start path, and look for `Raytrace` / `GetOnScreenPointF` imports in it (the grapple's method). Then a probe that logs the ray origin and direction at each grab. |
| E8 | How does a ladder or pipe start, and which action drops from a hang? | **(no build)** In the game with a gamepad-style binding view (Controls tab): note what starts a pipe climb (walk into it? use? jump?) and what drops from a hang (crouch? back + jump?). |

---

## 5. Recommended order of work

1. **E1, E5 and E8 now** (no build). They decide whether 1a is a bug in an existing feature, and they confirm the
   EdgeGrab-versus-wall-run separation that C1 relies on.
2. **A1: the "canonical forward" sprint and direction prototype**, together with E2's probe, behind a new setting
   that defaults OFF. This is the **smallest first prototype worth building**:
   - no new RVAs, no EGL table work: it reuses the pad thread's stick bytes and the existing `+ED2040` detour;
   - the decision (when to send "forward", the angle to restore, the gates) is a pure `_logic.h` header with unit
     tests, following the `WalkOnLook` pattern; the `.cpp` change is a few lines in two existing places;
   - it targets the two complaints that matter most in everyday play (sprint angle, turn lag) and may reduce head
     shift as a side effect;
   - the headset test is a direct A/B: toggle it in the overlay, sprint around a corner and diagonally.
3. **A2 camera base** in the existing `+C75380` detour (head and body shift).
4. **E3, then A3 view hold** through EdgeGrab and Ladder (no camera takeover). This is also step 4 of C1.
5. **C1-L ladders, pipes and ropes hand-over-hand**: the first hand-climbing feature, input-only.
6. **E7 static analysis, then C1** hand-triggered ledge grabs and hang gestures, with the old trigger switched off
   by setting.
7. **E4, then A0/A4 air control** (data first, code only if E4 allows).
8. Only after C1 has been played: decide whether **C2** (hand-driven hang) is worth it. **C3 and A5 are not
   recommended** without a much stronger reason.

### What still needs the headset

Everything in this document. No behaviour change was made or tested. Every approach above, including A1, needs a
run in the headset to judge comfort and to confirm the gates (dodges, wall-run, tic-tac, slides) still work.
