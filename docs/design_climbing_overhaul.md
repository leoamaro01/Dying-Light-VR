# Design study: full replacement of climbing with VR hand climbing

Status: design and research only. No mod code was changed. Written 2026-11-25 on fork `main` at `91fc1d9`, after
[feasibility_movement_climbing.md](feasibility_movement_climbing.md). That study ranked a full replacement ("C3") last.
This document works out what a full replacement looks like if we choose it anyway, as the user asked, with every
expectation met.

Tags, as in the feasibility study: **[CODE]** this repo, **[BIN]** read from the installed game binaries during this
study, **[DATA]** the game's data paks, **[INFERRED]** my reasoning, **[UNKNOWN]** not found yet (each has an RE task
or a headset experiment).

The static analysis here used two throwaway offline tools (an RTTI/vtable walker and an import-table mapper, kept in
the session scratchpad, not the repo) plus `dumpbin /DISASM`. All addresses are gamedll RVAs. **[BIN]** The installed
gamedll's vtables match the RVAs the mod already uses for the Steam build (`EdgeGrabController` facing vtable
`+172CAF8`, `MoveController` IMoveController vtable `+172EFA0`), so the installed code layout is the Steam layout even
though its PE timestamp differs.

---

## 1. The user's expectations, and whether each is reachable

| # | Expectation | Reachable? | Where in this design |
|---|---|---|---|
| E1 | The look + hold-jump grab no longer happens | Yes | §4.2 (the start gate) |
| E2 | Knee-height step-over, low-wall vaults and other foot moves stay | Yes: they are not ledge-grab code [BIN/DATA] | §2.3 |
| E3 | Grab ledges with real hands | Yes, with our own ledge test (raycasts) | §4.3 |
| E4 | Moving the hands moves the body (VR climbing) | Yes in principle: the engine already lets a controller drive the body kinematically. The exact write path is an RE task | §4.4 |
| E5 | A quick downward pull on a held ledge mantles if there is floor, or launches upward otherwise | Yes | §4.5, §4.6 |
| E6 | Wall-run and tic-tac stay exactly as they are | Yes: they are not ledge-grab modes [BIN] | §2.3 |
| E7 | While hanging: look around freely, snap or smooth turn with the stick, no view takeover | Yes: the ledge-grab controller does not own the look [BIN] | §4.7 |
| E8 | Poles (pipes), ladders and ropes climbable by hand | Yes: grabbable anywhere as solid objects, LadderController switched off, no animation or view control, look-directed jumps (spec 2026-11-26) | §4.9 |
| E9 | Other hand parkour moves (catch a ledge in a fall, shimmy by hand, push off, ziplines…) | Most fall out of E3-E5 for free | §4.10 |

---

## 2. What the game actually has

### 2.1 Controllers are swappable components [BIN]

Every player state is a `PlayerController` subclass. Each one implements a subset of six interfaces, and the player
asks "who owns this interface right now" each update. Walking a full RTTI scan of the installed gamedll gave 46
controller classes. The ones that matter here:

| Controller | Interfaces (this-offset) |
|---|---|
| `MoveController` | IMoveController@24, IPhysicsController@32, IBodyController@40 |
| `EdgeGrabController` (: `NaturalMovementController`) | IMoveController@24, IBodyController@32, IInventoryItemController@40, IPhysicsController@48 |
| `LadderController` | IMoveController@24, IBodyController@32, **ILookController@40**, IInventoryItemController@48, IVisibilityController@56 |
| `ZipLineController` | IMoveController@24, IInventoryItemController@32, IBodyController@40, IPhysicsController@48 |
| `RopeLocomotionController` (grapple) | ILookController@24, IBodyController@32, IMoveController@40, IInventoryItemController@48, IPhysicsController@56 |
| `LandingController` | IInventoryItemController@24, IMoveController@32, ILookController@40 |
| `LookController` | ILookController@24 |
| `DefensiveController` (kicks, slides) | ILookController@24, IMoveController@32, IBodyController@40, IPhysicsController@48 |

These correspond to the selectors the mod already calls [CODE]: the active `IMoveController` (`+BA2980`), the active
`ILookController` (`+BA0F70`) and the facing/body supplier (`+BB7BF0`). Controllers sit in a per-player table at
`PlayerDI+2976 + 8*id`, with an active byte at `+16` [CODE, `looting.cpp:508`]. `EdgeGrabController` is id 24 at
`PlayerDI+3168` [CODE].

**This is what makes a "full replacement" practical.** We do not need to invent a new controller class and register
it, which would mean a new table slot, serialisation and save support. We can **hollow out `EdgeGrabController`**:
keep the object, its table slot, its interfaces and its "active" state, and replace its *behaviour* while the VR climb
is on. The rest of the game (item hiding, the look and body selectors, landing, co-op, tutorials) keeps seeing "the
player is climbing" through the same object it always saw.

### 2.2 How the ledge-grab controller works today [BIN, partly INFERRED]

Vtables, from the RTTI walk:

| Table | RVA | Notable slots |
|---|---|---|
| primary (PlayerController) | `+172CBE0`, 50 slots | 11 `+D07A20` / 12 `+D07AA0` = **replication write/read** (2 ints at `+0xC0`, `+0xC4`; buffer cap `0xE4C`); 16 `+D09C10` = update (by analogy: MoveController's slot 16 `+D47F20` is its update [CODE `physcrouch.cpp:19`]); 34-49 = NaturalMovementController virtuals |
| IMoveController | `+172CB80`, 11 slots | 0 `+D0D9D0`, 5 `+D0DAB0`, 6 `+D0DBB0`, 7 `+D0DC80`, 9 `+D074C0` |
| IBodyController | `+172CAF8`, 12 slots | 6 `+D0E390` = facing target (the mod already names it [CODE `boneblend.cpp:21467`]) |
| IInventoryItemController | `+172CAB8`, 7 slots | |
| IPhysicsController | `+172CB60`, 3 slots | 0 `+D12200` returns a float: 0.05 in some states, from a state switch otherwise. 1 `+D16020` returns a bool by state (`+0x1CC`): physics on or off per phase |

What the controller's code calls (import-table scan of `+D07000…+D1A700`):

- `IControlObject::PhysicsBind` ×25, then IPhysicsBind virtuals `+0xA0`, `+0xB0`, `+0xD0`, `+0x100`, `+0x128`.
  **This is how it moves the player body.**
- `IPhysicsConfig::EnableStaticCollisions(2, 0x1D30C0)` at `+D0A559` switches some static collisions off during a
  climb, and `(0, -1)` at `+D121DE` restores them. A byte at `+0x504` remembers that they are off.
- `IPhBody::SetLinearVel` at `+D1204E`: sets a velocity on exit (jump from a hang, **[INFERRED]**).
- `IGSObject::Raytrace` ×3, all with collision mask `0x37` (`+D0E046`, `+D0E1DC`, `+D1883C`): the ledge search.
- `IGSObject::ReplSendNow` after a state change (`+D0D6B2`).

Debug strings next to its mode table name the mechanics: `hook pos and hook forward`, `_dir_to_hook_`,
`_look_point_`, `_wanted_point_`, `Sim_Pos` / `Vis_Pos`, `avg_sim_vel`, `anim start to end`, `Phx_On`,
`R Hand` / `L Hand`. **[INFERRED]** A climb finds a **hook** (a ledge point plus its outward direction) from a
**look point**. It then moves the simulated position toward a **wanted point** along a path timed by the animation
("anim start to end"), with physics partly off. It also places both hands on the ledge.

Ledge-grab **modes** [BIN, the string table beside `Shimmy_*`]: `FromBottomNoHang`, `FromBottom`, `EdgeGrab`,
`FromTop`, `SlopeRL`, `SlopeLR`, `HighSlopeRL`, `HighSlopeLR`, `JumpIn180`. The current mode lives at `+0x1CC`
**[INFERRED from the state switches in slots 0 and 1 of IPhysicsController]**.

Maps also ship binary `*_hooks.scr` files (edge segments as float pairs) [DATA], but only about 11 maps have them, so
most ledges are found at run time by the raycasts above **[INFERRED]**.

### 2.3 What the replacement must leave alone

- **Wall-run and tic-tac** are not ledge-grab modes (§2.2). They are jump clips in `m_fpp_nm_jump.scr`
  (`WallRunFront/Left/Right`, `WallJumpFront/Back/…`) [DATA], under MoveController's jump types [CODE `mc+448`].
  **The replacement does not touch them.**
- **Step-overs and vaults**: `JumpOverLow`, `JumpOnLowStand` and `JumpOverEnemy` are jump clips too [DATA]. Knee-height
  steps belong to the physics walk capsule **[INFERRED]**. Both stay.
- **The one grey area: `FromBottomNoHang`**, the game's quick climb onto a chest-high ledge without hanging, *is* a
  ledge-grab mode. The user asked to keep "the vault of low walls", so it gets a setting (§4.2): kept as the game does
  it (default), or replaced by a two-hand push-up.

---

## 3. Options for "replacing the controller", and the choice

| | Approach | Pros | Cons | Verdict |
|---|---|---|---|---|
| R1 | **Hollow `EdgeGrabController`**: detour its start, update and stop paths and its interface slots; while VR climbing owns it, our code runs instead of the game's | Everything that keys on "EdgeGrab active" still works: weapon hiding [CODE], selectors, landing hand-off, tutorials, quest conditions (`EdgeGrabMode`), co-op serialisation slots. No new controller registration | Needs RE of its start, stop and body-write paths. The hollowed state must keep the game's invariants (collision flag `+0x504`, `ReplSendNow`) | **Chosen** |
| R2 | A brand-new controller (a fake vtable in our DLL, inserted into the table) | Total freedom | The table slot, save and replication streams, the selectors' priority lists and the vtable layout of six interfaces would all have to be faked. Any engine path that RTTI-checks the class breaks | Rejected |
| R3 | No controller: write the player's position from outside while MoveController stays active | Least RE | MoveController applies gravity and walk physics and would fight every write. Landing, fall damage and the item logic would never know you are climbing | Rejected |

The same choice applies to pipes and ladders: **hollow `LadderController`**. It is the only one that also owns the
look (`ILookController@40`), and that ownership is exactly what takes over the view on a pipe today.

---

## 4. The design

### 4.1 Modules (following the `_logic.h` pattern)

| File | Role |
|---|---|
| `ovr2xr/climb_logic.h` | **Pure**: the per-hand grip state machine, the anchor math (body from held anchors and hand offsets), pull and launch detection, the mantle decision, reach and stretch limits, release velocity. Unit-tested in `tests/climb_logic_test.cpp`. |
| `ovr2xr/climbsense_logic.h` | **Pure**: decides from raycast results whether a hand point is on a grabbable edge (top surface, face, lip), a pipe, or nothing; the mantle floor and clearance test. Tested with fake hit lists. |
| `ovr2xr/climbsense.cpp` | Calls `IGSObject::Raytrace` (already used through the IAT by the grapple [CODE `grappleaim.cpp:49`]) with the ledge search's own mask `0x37`, at most 4-6 rays per hand per update and only near a grip. |
| `ovr2xr/climbctl.cpp` | The hollowed controllers: detours on EdgeGrab's and Ladder's start gate, update (slot 16), stop, and the interface slots listed below; the body write; the hand-off on exit. Byte-checked and refusing by name, like every other site. |
| Pad thread (`ovr2xr.cpp`) | Grip ownership: a grip near a valid surface belongs to climbing (withholding L1/R1 from the game, as push-grab does [CODE `pushgrab.h`]). Holster, loot, body grab and two-hand keep their priority when they claim first. |
| Rig (`boneblend.cpp`) | While a hand holds an anchor, that hand's IK target is the anchor, not the controller, so the hand stays on the ledge. The same mechanism `HandCollisionStop` uses to move a target inside the solve [CODE]. |

Settings (table entries): `climb_by_hand` (master, default OFF until it has been played), `climb_keep_quick_climb`
(the game's `FromBottomNoHang`, default ON), `climb_pull_mps`, `climb_launch_scale`, `climb_reach_m`,
`climb_haptics`.

### 4.2 Turning off the old trigger (E1, E2)

- Detour the path that **starts** `EdgeGrabController` (**[UNKNOWN]** RVA, RE task T1: the caller that sets its active
  byte, or its own can-start virtual among slots 34-49). With `climb_by_hand` on:
  - a start the game asks for is **refused** unless it is (a) one our code asked for, or (b) `FromBottomNoHang` while
    `climb_keep_quick_climb` is on;
  - every refusal is counted and logged by mode, so a quest that forces a climb shows up by name.
- Jump, vault, wall-run and tic-tac are not affected (§2.3). Holding jump against a wall simply jumps, as it does where
  there is no ledge.

### 4.3 Grabbing (E3)

A hand **grabs** when its grip closes (squeeze ≥ threshold, with hysteresis) and `climbsense` says the hand point is
on a grabbable edge within `climb_reach_m` (about 8-10 cm):

- **Edge test**: one ray down from just above the hand finds a top surface (normal within 35° of up); one ray toward
  the wall at hand height finds the face; their crease is the lip. The anchor is the hand's closest point on the lip.
  This reproduces in miniature what the game's own search does (§2.2), but from the hand instead of the look point.
- **Pipes and ladders**: if the hand is within reach of a `LadderDI` object, the anchor is the closest point on its
  axis from `ILadderInfo` (**[UNKNOWN]** accessor, RE task T6).
- **No grab** in the air above a jump's apex unless the anchor is reachable. Grabs while falling are allowed (a "ledge
  catch", §4.10).
- On the first anchored hand, our code **starts EdgeGrab in hollow mode** (mode `EdgeGrab`, so `EdgeGrabMode`
  conditions and the weapon hiding still see a climb), sets the replicated ints (`+0xC0` / `+0xC4`, RE task T4) to a
  hang state, and switches the body over to our motion (§4.4).
- Haptic tick on grab. A grab with a weapon in that hand: holster it first (the holster code already exists), or
  refuse with a short buzz (setting).

### 4.4 Moving with the hands (E4)

The classic VR climbing model, in `climb_logic.h`:

- Each holding hand *i* has a world anchor `A_i` and a current real hand offset `h_i` from the body origin (head-relative
  in play space, mapped to game world through the existing body-yaw mapping).
- The body origin wants to be `P* = mean_i(A_i − h_i)` (one hand: that hand; two hands: the average, so a hand pulled
  in moves you toward it).
- Clamp `|P* − P_current|` per update (no teleports) and keep each `|A_i − shoulder_i|` within arm length plus a
  margin. If over the margin for more than 0.3 s, that hand **slips** (releases with a buzz).

**Writing the body: answered in §8.1 (2026-11-25, round 2).** The ledge grab moves the body by *velocity* through the
same player walk and walk-speed setter (`+ED2040`) the mod already hooks, so the preferred path is now "hand our
velocity to the walk", not the three options below. They are kept as fallbacks. The original analysis:

**Writing the body (the main RE task, T2).** Three ways to apply `P*`, in order of preference:

1. **The ledge controller's own body write**: the IPhysicsBind virtuals at `+0xA0/+0xB0/+0xD0/+0x100` (§2.2) the game
   already uses to move `Sim_Pos` during a climb. Calling the same function with our target means the engine keeps
   its own interpolation, network smoothing and `Vis_Pos` consistent. *To find*: the call in update `+D09C10` (or its
   callees `+D0A240`, `+D0E4F0`, `+D10EB0`) that hands the wanted point to the physics bind.
2. **`IControlObject::SetWorldPosition`** (an engine export [BIN]) each update while the hollowed controller says
   "physics off" through IPhysicsController slot 1. Simple, but it may skip the physics bind's own smoothing.
3. **A velocity** on the walk body (`IPhBody::SetLinearVel`, the import the controller already uses) equal to
   `(P* − P)/dt`, with gravity cancelled by the "physics off" state. Collisions then stay real.

**Collisions while hanging.** The game turns *some* static collisions off for its scripted climb (mask `0x1D30C0`).
For hand climbing we want the wall to stay solid, so: leave collisions **on** while hanging and moving (approach 3,
or 1/2 with a short capsule sweep from `climbsense` before each move). Use the game's own "collisions off" call only
during the mantle lift (§4.5), exactly as the game does.

### 4.5 Pull to mantle (E5, floor case)

- **Gesture** (`climb_logic.h`): with at least one hand holding, the held hands' *real* positions move down relative to
  the head faster than `climb_pull_mps` (default 1.5 m/s) for ≥ 80 ms. The anchor model already lifts the body during
  the pull; the gesture decides what happens at the end.
- **Floor test** (`climbsense`): rays down onto the top surface 0.4-0.6 m beyond the lip, plus a column of rays for
  standing clearance (capsule height, from the walk config `GetRadiusY` [BIN export]). Crouch clearance as a fallback
  (then end in crouch through the physical crouch's action path [CODE `physcrouch.cpp`]).
- **Floor and clearance → mantle**: switch static collisions off (the game's own call and mask), lift the body over the
  lip in a short curve (0.35-0.5 s, head path kept smooth for comfort), restore collisions, **stop EdgeGrab** through
  its normal stop path (so `ReplSendNow` and the landing hand-off run), and leave MoveController standing on top.
- **No floor → launch** (§4.6).

### 4.6 Pull to launch (E5, no-floor case)

- Same gesture, but no floor beyond the lip (or the hand pulled while the head is already near the lip): on release,
  **stop EdgeGrab** and hand MoveController an upward and outward velocity = the hands' pull velocity reversed ×
  `climb_launch_scale`, capped at the game's jump height (`JumpMaxHeight` 1.01 m [DATA]) unless a higher target
  ledge is within reach. Velocity through the game's own exit call (`SetLinearVel` at `+D1204E`, RE task T3), the
  one the game uses for `HangFromEdge_Jump`.
- Grabbing again in flight re-enters §4.3 (a hand-over-hand ascent up a building face, which is the point).
- **Letting go with no pull** = a drop: stop EdgeGrab, zero velocity, MoveController falls, LandingController and fall
  damage behave as after any fall.

### 4.7 Looking and turning while hanging (E7)

- **Look**: `EdgeGrabController` does **not** implement `ILookController` [BIN], so `LookController` keeps the look and
  the mod's head-yaw path stays as is. The takeover today comes through **IBodyController** (slot 6 `+D0E390`, the
  facing supplier [CODE]) and LookController's narrowed windows [CODE, INV-772]. In hollow mode:
  - IBodyController slot 6 returns the **look** (the facing the mod already writes), so the body is not swung to the wall.
  - LookController's narrowed climb windows: release them while hollow mode is on (the `lookpitch` code already does the
    pitch half on release [CODE]).
- **Snap / smooth turn**: snap is applied only while LookController owns the look [CODE `boneblend.cpp:21340`], which
  stays true here, so the stick works unchanged. The turn rotates you about your **head**. Because the body position is
  computed from the anchors and the hand offsets (§4.4), the body correctly swings around the held hands. No extra code.
- **Pipes / ladders**: `LadderController` *does* own the look, so its hollow mode must return LookController's look
  values from its `ILookController@40` (or decline ownership), RE task T6.

### 4.8 Hands, body and what others see

- **Your own view**: hands pinned to the anchors (§4.1 rig). No climb animation is needed in first person, since the
  arms come from the controllers.
- **The body's lower half and TPP mesh**: hold the game's `HangFromEdge_*_Idle` pose if the animation graph can be
  asked for it (RE task T5, optional). Otherwise the last pose stays.
- **Co-op peers [INFERRED]**: they see your replicated position (moving smoothly) and whatever state the two replicated
  ints name (§2.2: EdgeGrab serialises only `+0xC0` and `+0xC4`). In a hang state they should play a hang animation on
  your avatar. **First version: single-player only**, refused by name in co-op as push-grab does [CODE `pushgrab.h`],
  until a two-player test has been done.

### 4.9 Pipes, ladders and poles (E8) - revised 2026-11-26 to the tester's spec

**The tester's spec (2026-11-26), which replaces the earlier "hollow LadderController" plan and §8.9's "drive the ladder's
state machine" suggestion:**
- Manual climbing. A ladder, pipe or pole is **a solid object**: grab it anywhere, climb by pulling, at any height (no rungs).
- **No animation** from the game's ladder, and **nothing turns or moves the view** but the headset.
- **Jumps**: two kinds, with the jump button, **chosen only by whether the player looks toward the pole or away from it**: toward
  = up, away = a push-off in the look's direction, which covers sideways and backward (simplified 2026-11-26).
- **The stick turns the view** (snap / smooth), as on ledges (§4.7). It does **not** orbit the player around the pole.

**Design.**
- **LadderController never runs** while `climb_by_hand` is on. Its own start (walking into a ladder + the look) is refused at its
  gate, the way +BD4450 refuses the ledge grab's (RE task T7). That removes its animations, its rung snapping, its look ownership
  (`ILookController@40`) and its orbit-around-the-pole stick, all at once.
- **The ladder / pole is just grabbable geometry.** A grip whose rays hit an object of a climbable class (the hit's owner by RTTI,
  as the character filter does: §8.10) holds there, **anywhere on it**, with no top or lip needed. Other surfaces keep the ledge
  rules (§4.3). Which classes these are is read from the run-6 log ("first ray hit on class ...") and the probe's LADDER ATTACH
  line (the +0x188 climbable's class).
- **The body is the same hand-driven body as everywhere else** (§4.4, P2): anchors where the hands closed, `P* = mean(A_i − h_i)`,
  gravity-free while any hand holds. Nothing about it is ladder-specific, so ledges, ladders, pipes and poles share one carrier.
  Which carrier is the main open question (U3 below).
- **Jumps** (jump button while holding a climbable). The tester's rules (2026-11-26, second pass): the look's yaw is measured
  against the direction from the body to the pole (0° = looking at the pole, 180° = looking straight away from it):
  - yaw **under `climb_pole_jump_side_deg`** (planned default 45°), i.e. looking toward the pole → **up**, along the axis;
  - yaw **at or beyond it** → a **push-off** in the horizontal direction the look points, away from the pole. Looking straight
    away from the pole makes it a backward jump.

  **Only the yaw decides, never the pitch.** Looking up while facing away from the pole is a push-off, not an up jump. This is the
  tester's simplification (2026-11-26): there is no separate backward class and no pitch test.

  The angle becomes a setting in the "Controller Bindings" group when the jump is built (P5). It is not added before, so the
  overlay never shows a knob that does nothing.

  **The game has only one ladder / pipe / rope jump.** Its data (`m_fpp_ladder.scr`) has a single `*_Jump` clip, the same frames
  (`M_FPP_Unarmed_Pole_Climbing.anm2` 264-274) for `Ladder_Jump`, `Pipe_Jump` and `Rope_Jump`. LadderController reads no jump input
  of its own: its actions are 0x50 / 0x51 (down / up), 6 / 7 (turn around the pole) and 0x112 (slide). So "sideways" and "backward"
  are not two mechanics in the game. They are the same push-off, in whatever direction the player faces around the pole after
  turning. The jump is therefore a launch velocity of ours (§4.6's exit path) in the direction the look rules choose. Its speed
  should match the game's push-off (U4). It is not the ladder's own states, because those need LadderController running and bring
  its animation and view with them.
- **Slide** (optional, later): grips half-closed = slide down at the game's slide speed. Let go = a fall.
- **Top**: pull up past the top = the mantle path (§4.5). Weapon hiding: whatever the ledge path does while holding.

**Open before building it** (in order):
- **U1** Do the ledge rays (mask 0x37) hit ladders and poles at all, and what class owns the hit? Run 6's "first ray hit on class"
  lines answer this with no new code: grab at a ladder and a pole. If they are not hit, a second mask or a nearby-climbable query is
  needed.
- **U2** LadderController's start gate (T7): who calls the group-7 switch (`+B9F4F0(player, 7)`, seen in `+D35C20`) and the entry
  test, so it can be refused by name like the ledge grab's.
- **U3** The carrier: a gravity-free body we move with hand velocity. Ledges already have one (EdgeGrab's hang, §8.4: no gravity,
  measured velocity = wanted). For a pole there is no lip, so either EdgeGrab's hang is entered with an edge built at the hand (and
  it must not drop itself for want of a real ledge), or P2 finds a way to switch MoveController's gravity off. This belongs to P2
  and decides P5.
- **U4** The game's ladder push-off speed (one jump, §4.9 jumps) and a matching upward speed, from its data or from a probe run with
  the stick-driven ladder.

### 4.10 Other hand moves that fall out of this (E9)

| Move | How |
|---|---|
| Catching a ledge in a fall or jump | §4.3 with no extra code (the grab test runs whenever a grip closes near an edge) |
| Shimmy | §4.4: reach sideways, grab, let go of the other hand. No `Shimmy*` animation needed |
| Climb up to a higher ledge | §4.6 launch, then grab |
| Push off backwards (wall jump from a hang) | Push the hands *away* from the wall fast while releasing: launch outward (§4.6 with the reversed push direction) |
| Turn 180 and jump (`JumpIn180`) | Snap-turn while hanging (§4.7), then push off |
| Quick climb onto a chest-high wall | Kept as the game does it (`climb_keep_quick_climb`), or two palms on top + a push down = mantle (§4.5) |
| Ziplines | Later: a grip above the head on a zipline starts `ZipLineController` (game behaviour after that) |
| Helping an NPC onto a ledge (`HelpSafeNPC_Ledge`) | Leave to the game (quest-scripted) |
| Grapple | Already hand-aimed [CODE R9]. Unchanged |

---

## 5. RE tasks (all static first; each ends in a byte-checked site with a `[DEP]` tag)

| # | Find | Starting points [BIN] |
|---|---|---|
| T1 | What starts `EdgeGrabController`, and its can-start/try-start | Callers that set the active byte (`+16`) for table id 24; `NaturalMovementController` virtuals (EdgeGrab primary slots 34-49 `+D072F0…+D0E460`); the ledge search around the Raytrace calls `+D0E046` / `+D0E1DC` / `+D1883C` |
| T2 | ~~The body write~~ **Answered (§8.1):** velocity at `EdgeGrab+0x268` → IMoveController slot 0 → player walk → `+ED2040`. Left open: what cancels gravity while hanging (IPhysicsController slot 1 `+D16020`, [INFERRED]) | — |
| T3 | The exit velocity call and its stop path | `SetLinearVel` at `+D1204E` (function containing it); the collision restore at `+D121DE` (`+0x504` = 1) |
| T4 | The state/mode fields: `+0x1CC` (state), the replicated `+0xC0` / `+0xC4`, `+0x4D4`, `+0x504` | Replication write/read slots 11/12 `+D07A20` / `+D07AA0`; IPhysicsController slots `+D12200` / `+D16020` |
| T5 | (optional) requesting a hang idle pose from the animation graph | `HangFromEdge_*_Idle` string references |
| T6 | ~~`LadderController`: start, update, look ownership, `ILadderInfo` axis~~ **Mapped (§8.9).** No longer needed for motion (§4.9 revised) | — |
| T7 | LadderController's **start gate**, to refuse it in hand mode (§4.9 U2) | the group-7 switch `+B9F4F0(player, 7)` (called from `+D35C20`); callers of LadderController's attach (main slot 18 `+D34340`) |

Epic (EGL) note: every new RVA needs an EGL row from the private generator, or the feature refuses by name on Epic
(AGENTS.md). The vtable lookups can use RTTI at run time (`rtti_vtable.h` [CODE]) so they need no table.

---

## 6. Phases, each with a headset test

| Phase | Builds | Headset test | Effort |
|---|---|---|---|
| P0 probes (**built and run 2026-11-25**: `diag_climb_probe`, results in §8.4; pipes/ladders still to run) | Log-only: EdgeGrab and Ladder state/mode per update, `+0x1CC` / `+0xC0` / `+0xC4` transitions, who calls start (T1), Sim_Pos per update during a game climb | Climb 5 ledges, shimmy, drop, climb a pipe, with the game's own controls. Send the log | 2-3 days RE + 1 day |
| P1 gate + static hang (**built 2026-11-25**, `climb_by_hand`, `ovr2xr/climbhand.cpp`: the game's start `+BD4450` is skipped unless a climb runs; a grip within `climb_reach_m` of the detected target (`PlayerDI+0x1600`) does the game's own start - target into the player, EdgeGrab slot 35, group 21 via `+B9F220`; both grips open in a hang state = `+D0CD20(eg, 16)`. Hanging is the game's own state 1, so no body write yet. Not yet run) | §4.2 gate; grip on a ledge → hollow EdgeGrab that holds the body still at the anchor; release = drop | Grab ledges at chest, head and arm's-reach height; drop; check the weapon hides and fall damage works | 1-2 weeks |
| P2 hand motion | §4.4 anchor model and body write (T2), slip, collision kept on | Hang and pull yourself up and sideways along a ledge; hang on a wall corner; try to push through the wall | 1-2 weeks |
| P3 mantle and launch (**round 1 built 2026-11-27**, §8.14: own lift over the lip, launch flight in the hold state, capped at the game's default jump) | §4.5, §4.6 | Mantle onto roofs of different heights; launch to a higher ledge; launch with no ledge (fall); low-ceiling mantle (crouch) | ~1 week |
| P4 look, turn, hands | §4.7, §4.8 | Hang, look 180° behind, snap and smooth turn, check the view never moves on its own; the hands stay on the ledge | 3-5 days |
| P5 pipes and ladders (after P2: it rides P2's hand-driven body) | §4.9 revised (U1-U4, T7) | Grab a ladder / pole at any height and climb hand over hand; turn with the stick (view only); jump up / back / sideways by looking; pull off the top | 1-2 weeks |
| P6 extras | Ledge catch tuning, ziplines, the co-op test, XP (§8.2-§8.3: the award function and its numbers are known) | Two-player co-op session; climb at night and exhausted to check the multipliers | open |

Rough total: **6-10 weeks** of focused work, most of it headset iteration. Confidence: **medium for P1 to P5**.
P2 was medium-low until round 2 showed the body moves by velocity through a call the mod already hooks (§8.1); what
cancels gravity while hanging is the remaining unknown there.

---

## 7. Risks

- **The body write (T2)** is the crux. If neither the controller's own physics-bind write nor `SetWorldPosition`
  holds the body steadily, P2 needs a velocity drive, which is jumpier near walls.
- **Engine invariants in hollow mode**: the static-collision flag (`+0x504`) must be restored on *every* exit path
  (drop, death, cutscene, load, co-op disconnect). A missed restore leaves the player walking through some static
  geometry. Mirror the game's own restore and log every exit by name.
- **Quests and tutorials** that force or wait for a climb (`EdgeGrabMode("FromTop")` in three daily bounties [DATA],
  any tutorial step that waits for a climb **[INFERRED]**): hollow mode keeps the controller and its mode visible, but a forced *game*
  climb that our gate refuses would stall a quest. The gate logs every refusal by mode; a setting can let forced
  climbs through.
- **XP**: solved in principle (§8.2): the game's award function, its numbers and multipliers are known and callable.
- **Co-op**: replication of a hollow climb is unknown; single-player only until tested.
- **Comfort**: the body following the hands is standard in VR climbing games, but launches and mantles move the view
  with no matching head motion. Keep them short and smooth, with a vignette option.
- **Flat mode**: the whole feature only runs in VR (the layer is not loaded in flat), so flat play is unchanged.

---

## 8. Round 2 findings (2026-11-25): the body write and climbing XP

Item 1 is parked in [plan_movement_decoupling.md](plan_movement_decoupling.md).

### 8.1 How the ledge grab moves the body (T2, answered) [BIN]

The player walk `+BA2B20` is not MoveController-specific. It asks the **active IMoveController** (`+BA2980`) for its
movement through that interface's slots, then applies it:

| IMoveController slot | What the walk does with it (from `+BA2D18…+BA2EE7`) |
|---|---|
| 0 (`+0x00`) | a **vec3**, scaled by a factor, kept as the move vector |
| 5 (`+0x28`) | a vec3, stored beside it |
| 6 (`+0x30`) | a float, compared and divided (a speed or rate) |
| 1-4 (`+0x08…+0x20`) | four floats |
| 7 (`+0x38`) | an int |

It then turns the vector into world space (`VectorLocalToWorld`) and calls **the walk-speed setter `+ED2040`** from
`+BA3A78`, the very call `walklook` already detours. `walklook`'s "controller not MoveController" pass-through counter
exists because other move controllers come through here too.

`EdgeGrabController`'s IMoveController slot 0 (`+D0D9D0`) returns **zero** when idle, and otherwise
`VectorWorldToLocal(player, *(vec3*)(EdgeGrab + 0x268))`: a **world velocity the controller stores at `+0x268`** each
update. Its slots 6 and 7 return values chosen by the climb state (`EdgeGrab+0x1FC`) and phase (`EdgeGrab+0x1F8`).
So a game climb is: the controller computes a velocity from `Sim_Pos` toward the wanted point, and the ordinary player
walk applies it to the physics capsule.

**What this means for the design:**
- The hollowed controller needs **no new body-write site**. Two equivalent options:
  - (a) write our velocity `(P* − P)/dt` into `EdgeGrab+0x268` each update, so the game's own slot 0 returns it; or
  - (b) in the existing `walklook::Hook`, when the active move controller is the hollowed EdgeGrab, replace `w` with our
    world velocity.

  (b) reuses an already-installed, byte-checked detour. (a) keeps the game's own slots 5-7 consistent.
- Collisions stay physics-resolved (the capsule moves by velocity), as §4.4 wanted. The static-collision switch-off
  is only needed for the mantle lift.
- **Still unknown:** what cancels gravity while hanging. Most likely IPhysicsController slot 1 (`+D16020`, a bool by
  climb state) tells the physics walk "no gravity / kinematic" **[INFERRED]**. The hollow mode would answer it the way
  the game's hang state does. P0's probe settles it.

**No-build check the user can run now:** in a normal climb, the `[WALKLOOK] 5 s` lines in `dlvr_ovr2xr.log` should show
the "controller not MoveController" count rising during climbs. That confirms the walk call runs while climbing.

### 8.2 How the game awards climbing XP [BIN + DATA]

**The award path:**

1. When a ledge climb finishes (`EdgeGrab+0x200 == 8`), the ledge-grab code (`+D0D777`) calls
   **`+CEA740(xpModule, climbType)`**. `xpModule` is `*(PlayerDI + 0x13E0)` and `climbType` is `EdgeGrab+0x1F8`.
2. `+CEA740` checks two player gates (vfuncs `+0x878` and `+0x9C8`; `+0x9C8` picks the local vs network progression,
   so probably "is a co-op client" **[INFERRED]**) and the **anti-farm check `+CEB710`**. That check refuses if this
   climb ends within `RunnerXPMinDistBetweenClimb` (1.3 m) of the last rewarded climb, with a time test against
   `RunnerXPMinTimeBetweenClimb` (5 s) **[the time half INFERRED]**.
3. It maps `climbType` (0-8) to an XP property, reads it (`+C58E40`), truncates it to an int and calls
   **`+CEB630(xpModule, 0x40, amount, true)`**. Then `+CEB6C0` records this climb's position and time for the next
   anti-farm check.
4. `+CEB630` forwards to the player's **`+BB6B90` AddExperience(category 0x40 = Runner [INFERRED], amount, …)**. That
   function reads the multipliers by property id: `RunnerXPFactor` (395), `RunnerXPNightFactor` (404),
   `RunnerXPFactorOnCriticalFitness` (396), and the Fighter, Survivor and Driver equivalents.
5. Shimmy-up XP is awarded separately: `+D0D895` calls `+CEA6A0(xpModule)`, which pays `RunnerXPClimbShimmyUp` (property
   382, read straight from the property array at `+0xBF0`) through the same `+CEB630`.

The property ids come from the game's id→name switch at `+C59290` (1040 cases).

Note: `EdgeGrab+0x1F8` is the field `item_hide_by_state` reads as `kEgPhaseOff` ("phase 3 = the game shows the weapon",
`boneblend.cpp:19786`). The XP switch uses it as the **climb type**, where 3 = FromTop. Both readings may hold (a FromTop
climb keeps the weapon), but the mod's comment may be naming it too narrowly **[INFERRED]**. Worth checking in P0.

**Climb type → XP property** (the switch table at `+CEA8C8`). **Correction from the P0 run (§8.4):** the type numbers are
right but the mode names in the second column are not. Type 0 is every climb that hangs, so hang-then-climb-up pays
`RunnerXPClimbFromBottom` (15 / 30). Type 2 (`RunnerXPClimbEdgeGrab`, 25 / 50) and type 3 (`RunnerXPClimbFromTop`, 5 / 10) are
direct climbs without a hang.

| `climbType` | Mode name (by the mode-string order; **[INFERRED]** for 4-8) | Property | Base XP |
|---|---|---|---|
| 0 | FromBottomNoHang (quick climb-up) | `RunnerXPClimbFromBottom` (385) | 15 |
| 1 | FromBottom | `RunnerXPClimbFromBottom` (385) | 15 |
| 2 | EdgeGrab (hang, then climb) | `RunnerXPClimbEdgeGrab` (383) | 25 |
| 3 | FromTop | `RunnerXPClimbFromTop` (386) | 5 |
| 4, 5, 6 | SlopeRL, JumpIn180, HighSlopeRL | `RunnerXPClimbJump180` (384) | 10 |
| 7, 8 | SlopeLR, HighSlopeLR | `RunnerXPClimbFromBottom` (385) | 15 |
| (separate) | shimmy up | `RunnerXPClimbShimmyUp` (382) | 5 |

**How much, in practice** [DATA, `default_levels.xml` and `common_skills.xml`]:
- Base values as above. The two "Freerunning" skill nodes **add** to them (`runningprof1`: EdgeGrab +10, FromBottom +7.5,
  Jump180 +5, FromTop +2.5, ShimmyUp +2.5; `runningprof2`: EdgeGrab +15 and the same others). A fully skilled ledge climb
  is therefore **50 XP**, 30 from below, 20 for a 180 jump, 10 from the top, 10 for a shimmy-up.
- Multipliers: `RunnerXPFactor` 1.0, **night ×2** (`RunnerXPNightFactor`), **×0.5 when exhausted**
  (`RunnerXPFactorOnCriticalFitness`).
- Anti-farm: no XP for a climb that ends within 1.3 m and 5 s of the last rewarded one.
- Other Runner XP for scale: wall jump / landing / slide / jump over zombie 20, `JumpOn` 10, grapple use 5,
  chase XP 1-4 per second (`RunnerXPPursuitLvl1-4PerSec`), and `RunnerXPRunningPerSec` 0 (exists, disabled).
- **A distance-based precedent already exists:** `RunnerXPZipLinePerMeter` (property 388) pays zipline XP per metre. Its
  value is not in the data files, so it is a code default **[UNKNOWN value]**.
- Runner levels for scale: 550 XP to level 2, 3 000 to level 5, 8 750 to level 10, 23 750 to level 24 (cumulative
  thresholds per level, `ExpToNextLevelRunner`).

### 8.3 Climbing XP options for the hollow controller

All of them pay through the game's own `+CEB630(xpModule, 0x40, amount, true)`. Night ×2, the exhaustion ×0.5 and
co-op/progression routing then stay the game's.

| Option | What it pays | Pros | Cons |
|---|---|---|---|
| X1 **Game-equivalent** | Call `+CEA740(xpModule, type)` ourselves at the end of each hand climb: mantle = type 2 (EdgeGrab, 25-50), mantle from a quick climb-up = type 0 (15-30), a launch that ends in a new grab = type 5 (Jump180, 10-20), dropping onto a ledge from above = type 3 | Exactly the game's numbers, skills, anti-farm and bookkeeping. One call | A long free climb up a wall pays only once, at the top |
| X2 **Per metre climbed** | Accumulate the *upward* distance the body moved while holding (and pipe/ladder axis distance); pay `k` XP per metre in batches (for example each whole metre, or at release) | Rewards real climbing effort; mirrors the zipline's per-metre XP | Needs a value for `k`, and our own anti-farm (for example only metres above the highest point reached in the last 10 s, so up-down farming pays nothing) |
| X3 **Per second climbing** | Pay `k` XP per second while holding with at least one hand, like chase XP | Simplest | Rewards hanging still. Easy to farm |
| X4 **Move bonuses** | On top of X1 or X2: a launch-and-catch (dyno) pays like Jump180, a catch from a fall pays like EdgeGrab, a 180° push-off and catch pays Jump180 | Rewards the VR-specific moves | Each needs its detection, and its own anti-farm |

**DECISION (user, 2026-11-25): X1 (game-equivalent) only, for now.** X2-X4 stay on file as possible later additions.
X1 needs no new settings: the hollow controller calls `+CEA740(xpModule, type)` with the type table in §8.3's X1 row.

Earlier recommendation, kept for reference: X1 + X2 + X4. X1 keeps parity with the game (the mantle pays what a climb always
paid). X2 pays a small amount per metre of real climbing above the last highest point; about 2 XP/m makes a 10 m wall
roughly a ledge climb's worth. X4 adds a bonus for dynos and fall catches. X3 is the least useful.

If X2-X4 are added later, their numbers become settings (`climb_xp_per_m`, `climb_xp_dyno`, …) in the settings table,
so they can be tuned in the headset.

New RE items, all small: the gates `+0x878` / `+0x9C8` (confirm which is the co-op client test), the time half of
`+CEB710`, and the `RunnerXPZipLinePerMeter` default.

### 8.4 P0 results: the first headset run (2026-11-25, `diag_climb_probe`) [MEASURED]

Source: `dlvr_ovr2xr.log` 20:45-20:49. 21 ledge-grab windows and 1 zipline, 7 027 per-update samples (cap never reached).
The user did hangs, shimmies, drops, short climbs, from-the-top grabs, a turn-and-jump, a zipline, and walking/sprinting.
Pipes and ladders were **not** tested.

**Field meanings, corrected by the data:**

| Field | Was assumed | Measured meaning |
|---|---|---|
| `EdgeGrab+0x1FC` | climb state | **current state** (confirmed). See the state table below; −1 when the controller is idle |
| `EdgeGrab+0x200` | "end state" | **the previous state**. It records every transition (0→1, 1→11, 11→1 …). The XP award condition "`+0x200 == 8`" (§8.2) therefore means "the state before this one was 8 (a climb-up)", so XP is paid when a climb-up completes |
| `EdgeGrab+0x1F8` | climb mode, in mode-string order | **the climb's type**, constant through one climb and set again when a new climb chains on. Observed: **0 = any climb that hangs**; 1, 2, 3, 4 = direct climbs that never hang (3 the most common short climb-up). The mode-string order guessed in §8.2 is **wrong**; the switch's type→XP property table itself still holds |
| `EdgeGrab+0x504` | "static collisions off" flag | **1 = collisions normal, 0 = the game's reduced collision set.** It is 0 for the whole climb, hanging included, and 1 once MoveController takes over |
| `+0xC0` / `+0xC4` (replicated) | climb state for peers | −1/−1 in every sample (single-player). Meaning still unknown |

**Ledge-grab states seen** (`+0x1FC`), with how the body moved:

| State | What the player was doing [from position and timing] | Walk body | Physics answers (slot 0 / slot 1) | Movement |
|---|---|---|---|---|
| 0 | the grab itself (~0.2-0.3 s) | **gone** (the physics bind has no walk physics) | 0.1 / 0 | scripted |
| 1 | hanging still | present, walk state `0x0002` | **0.2 / 1** | wanted = 0, **measured = 0: no fall** |
| 11, 12 | shimmy sideways (±0.65 m per step) | present, `0x0002` | 0.2 / 1 | **wanted = ledge-grab velocity = measured** (e.g. (−0.02, 0, −0.55) m/s) |
| 13 | shimmy up (+1.1-1.3 m) | present, `0x0002` | 0.2 / 1 | by velocity |
| 14 | a shimmy attempt that did not move (blocked) [INFERRED] | present | 0.2 / 1 | none |
| 2, 3, 7 | brief hang sub-states (2 led to 6; 3/7 led to a climb-up) | present | 0.2 / 1 | — |
| 4, 5, 6 | jumping off / up from a hang (4 → MoveController; 5 → a new grab higher up) | present | **0.1 / 1** | by velocity |
| 8 | the climb-up (mantle) animation | **gone** | 0.1 / 0 | scripted (position spikes up to 27 m/s = placed, not physics) |
| −1 after 8 | done (previous state 10); after a drop the previous state is 16 | back | −1 / 0 | MoveController |

Walk state `0x0001` = on the ground and `0x0002` = in the air or flying (MoveController falling after a drop also shows `0x0002`).
The zipline (`ZipLineController`, group 27) also moves the body through the walk body's wanted velocity (|wanted y| up to
3.4 m/s). Controller groups seen: 21 = ledge grab, 27 = zipline, 0/1 = on foot.

**What this answers:**

1. **Gravity (the §8.1 open question): answered.** In every hang and shimmy state the ledge grab answers IPhysicsController
   slot 1 = **1**, and the walk body then moves at exactly its wanted velocity with no fall. Zero wanted velocity means the
   body stays perfectly still. MoveController in the air answers 0 and falls. So the ledge grab's own hang state *is* the
   gravity-free, velocity-driven, collision-resolved container hand climbing needs. **[MEASURED; that slot 1 is the switch
   itself is still INFERRED from the correlation.]**
2. **The body write: confirmed.** During movement states the walk body's wanted velocity equals the ledge grab's velocity
   and the measured velocity, to the printed precision. In state 1 the game hands the walk zero (its IMoveController slot 0
   returns zero outside the movement states), so **writing `+0x268` alone would not move a hanging player**. The hollow
   controller must replace the vector at the walk call: `walklook`'s existing `+ED2040` detour, when the active move
   controller is EdgeGrab and hollow mode is on (§8.1 option (b)), or a detour on IMoveController slot 0.
3. **The mantle and the launch can be the game's own.** Climb-up (state 8) and hang-jumps (states 4/5) are the game's
   transitions out of state 1. If the hollow controller keeps the ledge grab genuinely in state 1 and only moves the body,
   a pull gesture can **ask the game for its own climb-up or jump** (the same input a hanging player presses). The game
   then checks for floor, plays the mantle, and **pays its own XP** (the award fires after state 8). That meets the
   "game-equivalent XP" decision with no XP code at all.
4. **Collisions while hanging are reduced** (`+0x504` = 0 throughout). Moving a hanging body with the hands may pass through
   whatever the reduced set leaves out (mask `0x1D30C0`, group 2 at `+D0A559`). P2 must test pushing into walls; if needed,
   the hollow mode restores full collisions (the game's own `EnableStaticCollisions(0, -1)` call) while hand-moving.

**What is still open:**
- **T1, the entry**: how the grab (state 0) starts, and whether our code can start it at a ledge the hand chose. Unchanged.
- **The input that makes a hanging player climb up or jump**, for point 3: confirm in a headset run (P1).
- Pipes and ladders (`LadderController`): **not measured**. A second run with pipes is needed before P5.
- Whether slot 0 (0.2 / 0.1 / −1) matters. It differs between hanging and jumping, which suggests a step height or capsule
  setting.

### 8.5 P1 run 1 (2026-11-25 21:18) and round 2 [MEASURED + BIN]

User report: very finicky; the left hand rarely worked; never worked mid-jump; the grab was "extremely precise" (asked for a sphere
or cone cast that also works with the hand inside the ledge); a ledge with floor above was climbed over at once (asked for a hang
setting); low walls between knees and chest should not need hands. The old trigger was off; vaults and step-overs still worked.

The log (13 grabs, 12 right-hand, every start accepted by the game) showed why:
- **The target was the wrong ledge.** `PlayerDI+0x1600` held a target on **every** update (291/291), because it is the game's
  **look-based** detector's record (+BDB020): allocated once (+B9D460, 0x30 bytes) and refilled or zeroed (+BDBEE9) every update.
  Grips were measured against whatever ledge the head pointed at (misses reported 0.6, 1.3, 6.3, 16 m away).
- **Climb-over:** 12 of 13 grabs got a direct-climb type (1, 3, 4, 8). The type chooser `+D0AF40` returns 0 (hang) only when its
  "space on top" flag is false, so a ledge with floor above never hangs.

Round 2 (built 2026-11-26, not yet run):
- **Own ledge search from the hand** (`climbhand_logic.h` `FindLedge`, tested against a box world): a cone of 25 horizontal rays
  (5 heights from +10 to −30 cm around the hand × 5 yaws of ±30°) starting 45 cm on the player's side of the hand, so a hand
  **inside** a ledge still finds its face. Then 2 side rays for the wall's real direction and up to 2 rays down from 50 cm above
  the face onto the top. The lip must be within `climb_reach_m` of the hand. About 29 engine raycasts per update while searching,
  using the ledge grab's own mask `0x37`.
- **The found ledge is handed to the game** with its own set-target `+B9D460(player, a, b, into, 8)`, then the game's own start, in
  the same player update.
- **Catching while falling:** a closed grip keeps searching for `climb_catch_ms` (1.5 s). A searching grip is still sent to the
  game as L1/R1, and is only withheld once a climb holds it.
- **Hang setting:** `climb_always_hang` (default on) detours `+D0AF40`, only for the call at `+D0E74C` that follows a hand grab
  whose lip is at least `climb_hang_min_m` (1.9 m) above the feet. Any non-zero type becomes 0.
- **Low walls:** `climb_auto_low_m` (1.5 m). The game's own start runs when its own target's top is at most that far above the
  feet, so those walls climb the game's way (look + jump).

### 8.6 P1 run 2 (2026-11-26 12:14) and round 3

User report: still very hard to pick ledges, the right hand never worked, never stayed hanging. Asked for debug drawing and for
`climb_auto_low_m` = 1 m.

The log showed:
- **Most presses never reached climbing.** Only 8 searches ran in the whole session, while "presses another feature owned"
  counted 2-5 every 5 s. Another grip feature took the press first, and which one was not logged.
- **Face misses with the hand over the head** (feet+2.07 to 2.21 m). The cone pointed along feet → hand, which is noise when the
  hand is above the head.
- **The forced hang did not hang.** The type chooser returned 0, but the grab still went 0 → 8. Static analysis of `+D0A240`: at
  the end of the grab (`+D0A442…+D0A4B9`) the next state is 8 unless type == 0 **and** the ledge grab's own flags `+0x204` /
  `+0x205` allow a hang (then 1, or 4 if `+BD8790` says so). The set-state call there is `+D0A4B9`, return address `+D0A4BE`.

Round 3 (built 2026-11-26):
- **Full circle of face rays:** 12 directions × 6 heights (+15 to −40 cm). A hand up to 40 cm above the top still finds it.
- **The hang decided where the game decides it:** a detour on set-state `+D0CD20`, filtered on return address `+D0A4BE`, for the
  ledge grab a hand grab armed. It turns that one 8 into 1 and logs the game's own `+0x204` / `+0x205` flags. The type detour stays.
- **Owner names:** each grip press another feature takes is logged with that feature's name.
- **Debug view** `climb_debug_draw`: every ray of the current search, the hand, the found ledge, an out-of-reach ledge and the
  game's own look target, drawn as two per-eye quads covering each eye's declared FOV, with lines projected through that eye.
  The world → headset mapping is the grapple icon's. It shows for 3 s after the last search.
- **Engine collision view** `climb_debug_physics` (experiment): `ILevel::SetPhysicsDebugRenderMode(level, flags)`. The engine reads
  bits 1, 2, 8, 0x10, 0x20 and 0x40. Whether the retail build still draws it is unknown.
- `climb_auto_low_m` default 1.0 m.

### 8.7 P1 run 3 (2026-11-26 12:33) and round 4

- **Most presses never reached the climb.** About 90 grip presses were logged "taken by loot by hand"; only 8 searched. Cause:
  with `body_grab` on, loot by hand puts **every** press with no body in reach into its `Waiting` state (up to 150 ms,
  `looting.cpp` `kLateArmS`, waiting for the severed-limb reading) and withholds the grip meanwhile. The climb looked only at the
  press's first frame, so it lost the press for good. The rare presses that did search were ones where the limb reading was
  already in.
- **The hang works.** The one grab that got through went `GRAB END 0 -> 1 = HANG` (the game asked for 8, hang flags 0/0), held
  for 8 s, and dropped cleanly on both grips open (set-state 16).
- **Round 4:** an owned press stays pending for up to 300 ms (`climbhand_logic.h` `kLateTakeMs`). If its owner lets go while the
  grip is still closed, it becomes a fresh press for the climb. If the owner keeps it, or the grip opens first, it is lost as
  before. The log names both outcomes, and the 5 s line counts "kept" and "released to the climb" separately.
- `climb_debug_physics` set to 255 in the tester's settings for this run.

### 8.8 P1 run 4 (2026-11-26 19:58) - P1 passed; walls; the first ladder data

**Verdict (tester): P1 is a success.** Grips reach the climb on nearly every press (loot's wait handed back 1-4 presses per 5 s), grabs
with either hand, hangs from high ledges (`GRAB END 0 -> 1`), shimmies, and drops on both grips open. `climb_debug_physics` (255)
drew nothing: the retail engine's physics debug view is dead. The setting stays as a no-op until it is removed or locked.

**Going through walls.** 50 grabs; 6 had a climb direction opposite to the game's own target, and the logged hand position puts the
"into wall" vector pointing BACK at the body (19:59:23: hand at feet+(0.69, .., 0.17), into (-0.95, -0.31); 20:02:19: hand at
feet+(-0.60, ..), into (0.93, -0.36)). Cause: the round-3 full circle of face rays starts each ray 0.45 m behind the hand along its
own direction, so a ray aimed at the body starts on the far side of whatever the hand reached through and hits its back face, which
is nearer the hand than the front face. Round 5 adds three checks to `FindLedge` (`climbhand_logic.h`), each with its own log reason
and 5 s counter:
- **back face** (`kBackFace`): a face hit counts only when it lies past the body's axis along the ray (>= 5 cm), so the face faces
  the body.
- **blocked** (`kBlocked`): one ray from the body's axis (at the face's height, clamped to feet + 0.3..1.7 m) to 3 cm short of the
  face must be clear. This catches a hand poked through a fence, window bars or a thin wall.
- **no room** (`kNoRoom`): a horizontal ray 25 cm above the top, from 35 cm in front of the face to 15 cm past the lip, must be
  clear. This catches a "top" found inside a thick wall (the top ray started inside it).

Tests: thin wall reached through (front lip wins), back face only, ledge behind a fence, top inside a wall.

**Ladder / pipe (LadderController), first measurement** (climbs #20, #21; one object, axis at x 531.03, z 212.87):
- Controllers: LookController(2) + **LadderController(7)** + EquipmentController(22). MoveController is off; LadderController is
  the move controller. Group **7**.
- Motion is kinematic along the axis: x/z snap onto it at the start (~0.4 m over ~0.6 s), then only y changes. Up at 1.5-2.4 m/s,
  down at about 1.7 m/s, exactly 0 with no input (it holds still for seconds). #21 climbed 6.4 m in 4.4 s and left at the top with a
  forward step (walk wanted 1.76 m/s along the exit). #20 left at the bottom.
- The walk-physics getter returns null here, as it does for every ledge grab (ROOMSCALE logs it per climb), so the walk columns
  read NaN. Hand-driven ladder motion (P5) must go through LadderController's own fields, not the walk body. Those fields are not
  mapped yet (T5).
- **Not captured:** the pole. The probe's 8000-line session cap was reached at 20:03:20 (climb #9 alone took 900 samples), and the
  pole climb came after it. Next probe run: lower the per-climb cap or probe ladders only.

### 8.10 P1 run 5 (2026-11-26 21:01) - thin fences, zombies

- **Thin fences (< ~10 cm) were never found.** The top rays drop 6 cm and 15 cm inside the face, so on anything thinner than 6 cm
  both fell past the back face: "no top". That run had 95 "no top" results out of 333 searches. Round 6 drops the top rays at 2, 5,
  10 and 15 cm, nearest first. Test: a 4 cm fence.
- **Zombies could be grabbed and climbed.** The ledge grab's ray mask (0x37) includes characters. Round 6 names each hit's owner
  (`SCollision+0x18 - 24`, as `grappleaim.cpp` does) by RTTI and treats a character as a miss. `IsCharacterClass` matches HumanAI*,
  ZombieAI*, *AIVis, DeadRagdoll and AI, the live and dead AI classes in the gamedll's RTTI. These names were not yet seen on a real
  hit, so every hit class is logged once per session ("first ray hit on class ...") to confirm what a zombie reports. The 5 s line
  counts skipped character hits, and the debug view draws them pink.

### 8.11 Round 7 (2026-11-26): each hand holds only where it is on a ledge

Tester, after run 5:
- (a) Grab a ledge with one hand, close the other grip anywhere in the air, let go with the first: the character kept hanging
  with no hand on the ledge.
- (b) Closing the second grip made the first hand's search stop.

Cause: the pad-thread machine had one search slot, and a newer press took it over. While hanging, **any** press joined as a hold.

Round 7 (`climbhand_logic.h` `Update`, `climbhand.cpp` `ServeSearches`):
- **Both hands search at once**, each with its own catch time. Neither replaces the other.
- **A hand holds the climb only once the game thread found a ledge within reach of that hand** (`In.foundMs`). That is either the
  grab it started, or a **join**: the same ledge search runs at a searching hand while the grab is running, and a hit joins that
  hand to the hold. A press anywhere else holds nothing, so letting go with the last hand on a ledge drops, as it should.
- A found hand whose grab never becomes active within 500 ms (`kStartWaitMs`) holds nothing ("GRAB LOST").
- A press during the game's own low-wall climb also holds only on a ledge.
- The log names each hand: SEARCH, search ended (released / catch time over / FOUND), JOIN, let go. The phase lines show
  "on a ledge 0x.." and "searching 0x..".

Tests: both hands search at once; a press off the ledge holds nothing and the release drops; a second hand on the ledge keeps the
hang (hand over hand); a find from an earlier search does not count; a grab that never starts holds nothing; the game's own climb
is joined only on a ledge.

Not solved here: once holding, a hand stays held until it opens, even if the body moves away from it (the game's own shimmy or
climb-up). Pinning the hand to its grab point is P4.

### 8.12 Run 6 (2026-11-26 21:41): round 5-7 confirmed, hang height, first ladder attach

Tester: "everything looks great"; the character often climbed over instead of hanging.
- **Zombie filter confirmed:** a zombie hit reports `HumanAI`, and 719 character ray hits were skipped. Other classes seen on
  hits: `DestroyableObject`, `DoorDI`, `ComplexContainer` (all world, used). Plain static geometry has no object, so it logs no
  class.
- **Round 7 confirmed:** 6 joins, each at a lip 1.5-1.8 m with the hand 3-22 cm away. Drops fired only when no hand was on the
  ledge.
- **Hang height:** 26 hand grabs. All 10 with the lip ≥ 1.95 m hung. Of the 16 climb-overs, 7 had the lip at 1.59-1.86 m.
  `climb_hang_min_m` default **1.9 → 1.7** (also set in the tester's file).
- **Pole (probe; the tester confirmed it was a POLE, not a ladder):** the climbable is class **`OwnerLadderInfo`** (`+0x188`,
  interface at +0), the same class as ladders. A pole is a LadderDI of type `PIPE`, and runs LadderController with "rungs".
  This pole had 12 rungs.
  - `+0xE0` = the axis top (y 143.53).
  - `+0xEC` = the axis bottom (y 138.93 / 139.53: it differs between the two attaches).
  - The player's x / z snap exactly onto the axis point (245.468, 268.582).
  - Over 4.0-4.6 m, the rung spacing is about 0.33-0.38 m.
  - `+0xD4` read 0 throughout, so it is not the rung's world position.
  - State sequences: `6>9>0>3>0>8>11` and `6>9>0>8>11`. So 6 = enter (bottom), 9 = one rung up (the rung index goes **down**),
    3 = turn, 8 = one rung down, 11 = leave at the bottom.
  - State durations: 5 → 1.33 s, 6 → 0.11 s, 7-9 → 0.33 s, 10 → 1.57 s, 11 → 0.41 s, 12 → 1.0 s, 14 → 0.33 s.
- **Not yet seen:** a real ladder, and whether the hand rays hit a pole or ladder at all (U1). No hand grab was tried on the pole.

### 8.13 P2 round 1 (2026-11-26): the hanging body follows the hands

Built on §8.1 option (b) and §8.4: in the ledge grab's hold states the body moves at exactly the velocity the player walk hands
the physics, with no gravity, and the game hands zero there.
- **Anchors** (`climbhand_logic.h` `PullStep`, pad thread):
  - Each hand on the ledge gets a world anchor, latched where the hand is **when the hold begins**. That is state 1 / 2 / 3 / 7 /
    14, not the grab's first touch, because the grab animation (state 0) moves the body first.
  - Anchors are dropped when the hand opens or the game leaves the hold states (its own shimmy 11 / 12 / 13, climb-up 8, jumps
    4-6), and re-latch after.
  - A hand more than 0.8 m from its anchor in one poll **slips** and re-anchors where it is.
- **Velocity**: the published error is the mean of (anchor − hand). On the game thread, `ClimbHandWalkOverride` turns it into
  error / 80 ms, capped at `climb_pull_max_mps` (4 m/s). walklook's `+ED2040` detour sends that instead of the game's vector on
  the player walk's call, only while the ledge grab is active in a hold state and the publish is fresh (≤ 100 ms).
  - walklook now also installs when `climb_by_hand` is on, not only with `comfort_body_follows_head`.
- **Effect**: pulling the hands down raises the body; reaching up lowers it. Moving along a ledge is hand over hand: a new hand
  holds only where a ledge is (round 7), then the old hand lets go.
- **Settings**: `climb_hand_motion` (default on), `climb_pull_max_mps` (1-8, default 4).
- **Log**:
  - "ANCHORED" / "SLIPPED" per hand.
  - The 5 s line adds the walk calls while climbing, how many the hands drove, the largest speed sent vs measured, and "body held
    back by something solid" (sent ≥ 0.5 m/s, moved < 30 % of it).
- **Open (the headset run decides)**:
  - (1) Does the ledge grab tolerate the body leaving its hang point, or does it snap back, shimmy or drop?
  - (2) Hanging uses the reduced collision set (`+0x504` = 0, §8.4). Does a pull through a wall pass? The "held back" count and the
    tester's report tell.
  - (3) Pulling above the lip is not limited yet. P3's mantle owns that; until then the body may end up level with or above the
    lip while still "hanging".

**P2 round 2 (same day, before any headset run; tester's corrections):**
- **The hand snaps to the ledge.** A hand's anchor is the ledge point its own search found (`Ledge::lip`, published with
  `foundMs`), not where the controller was. With the search's 30 cm reach, the controller would otherwise usually hold empty air.
  At the hold's start the body moves (up to `climb_reach_m`) so the hand lands on that point. Re-latching after a game shimmy /
  climb-up uses the same point.
- **A hand pulled away lets go, with no re-anchoring.** Farther than `climb_release_m` (default 0.45 m, never under the reach +
  0.1 m) from its point, the hand releases: its hold bit clears (no hand left = the drop). Its still-closed grip stays withheld
  until it opens, so it is never a kick or grapple. To grab again, open and close the grip. This replaces round 1's 0.8 m slip.
- The **visible** hand model still follows the controller. Pinning it to the ledge point is P4.

**P2 run 7 (2026-11-26 22:05) and round 3.** P2 moves the body as designed: the measured speed matched the sent speed (up to 4.0 /
4.0 m/s), and "held back by something solid" counted 0-58 per 5 s. The tester reported three problems:
- **"Sometimes it let go almost immediately."** The game's grab animation (state 0) moves the body before the hold begins. A hand
  0.18 m from its point at the grab was **0.51-0.57 m** from it when the hold began, and was released on the first poll. (The log
  line also said "ITSELF" by mistake: it was printed after the release cleared the flag.)
  - Fix: a hand may release only once it has **settled** on its point (within 12 cm), or 1 s after latching if the body never gets
    it there (`kSettleM`, `kSettleMaxMs`).
- **"Only the last hand to grab moves the body."** The code already averaged both anchored hands. The body follows the mean of
  (anchor − hand), which is exactly the tester's proposal: average anchor minus average hand. Two causes are likely:
  - With both hands holding, a pull with **one** hand moves the body **half as far**. That reads as the other hand winning.
  - The averaging leaves each hand off its own anchor by half the difference between the hands. A long one-handed pull reaches
    0.45 m on the other hand and **releases** it. Run 7 had one such release (left, 0.52 m) while both held.
  - The 5 s line now counts the polls driven by one hand and by both, plus the releases.
- **"The camera moved to the side almost every time I hanged after pulling myself up."** While hanging, the hang's update
  (`+D0A240`) compares the look to the wall. Past a threshold either side it sets **state 2 or 3**: its look-left / look-right
  ahead of a side jump. Their update turns the camera through `+D45B90` (from `+D0AE50`). Run 7's sequences show them:
  `0>1>3>1`, `0>1>2>1`.
  - Fix: with `climb_by_hand` on, the set-state detour refuses 1 → 2 / 3 from that call site (`+D0AD16`, return `+D0AD1B`,
    checked at install to be `call set-state`), so the hang stays in 1.
  - The same call site's 0xD / 0xE (shimmy up / blocked) pass. The game's side jumps from a hang therefore need state 2 / 3 and
    are gone in hand mode; P3's launch replaces them.
  - Not handled: a camera call in state 1 itself (`+D0AC6B`, a timed one with a different constant set). If the camera still
    moves, that is the next suspect.

**P2 run 8 (2026-11-26 22:35) and round 4.** The look-state refusal worked: the hang stayed in 1 (`0>1>16`, `0>1>8` only), with
up to 67 refusals per 5 s. The pull drove from both hands most of the time (for example 1436 / 1436 polls), and only 2 hands were
released for distance.

Tester: "even though the camera doesn't swing when looking sideways, the body does some animation that adjusts the player's position
a bit." Cause (inferred from the code; no state change was logged): the pull measured each hand as **camera** + R·(grip − head)
(looting `HandWorld`). The hang's look pose moves the game camera when the player looks sideways (the head leans), so a still hand
appeared to move, and the pull moved the body after it.
- Fix: the pull measures each hand from the **body** instead: feet + R·grip + a per-hand key (`BodyHandKey` / `BodyHand`).
- The key is taken whenever the hand has no anchor, so at the latch both measures agree.
- After that, only the body's own motion (what the pull causes) and the real hand's motion move the measured hand.
- The ledge search still uses the camera-based hand (where the hand is drawn).
- Open: a snap turn while hanging rotates R about the play-space origin, not the head, so it would swing the measured hands. P4
  owns turning while hanging.

**Round 4 REVERTED (run 9, 2026-11-26 23:09; tester: "shift is still there ... it's more like snaps").** The tester's reading: the
eyes snap back when the head turns. The run-9 log shows `[HEADFOLLOW] BODY-FACING ... over a fold of +41.3 deg` during a hang.
- **Cause:** body-follows-head (`comfort_body_follows_head`, boneblend `follow::Before`, neck hold) kept turning the hanging body
  with the head, because LookController still owns the look while the ledge grab runs. The neck hold holds the body inside 40° and
  then pulls it round. The game camera sits on the head bone, so turning the body swings the eyes.
- **Fix (round 5):** `ClimbHandFreezesBody()` is true while `climb_by_hand` is live and the ledge grab runs. `follow::Before` then
  takes the paused path, as when another controller owns the look: nothing is folded, D2 carries the head yaw on the camera only,
  and the outstanding yaw folds in when the climb ends.
- **Consequence, accepted by the tester (option 3):**
  - While hanging, the body and the game's look stay where the grab left them, facing the wall.
  - The **game's own** jumps from a hang (jump button) therefore go along the body, not where the player looks.
  - In hand mode those jumps are replaced by P3's launch, which takes its direction from the headset's look (§4.6 / §4.9). The game
    jumps' direction is not fixed separately.
  - Rejected alternatives: catching the fold up just before a jump (which needs the jump held back a frame), and turning the body
    around the head instead of its centre.

### 8.14 P3 round 1 (2026-11-27): mantle and launch [BIN + DATA]

Run 9 confirmed round 5: the snap is gone, and the game's side jumps from a hang still work. So the jump button stays the game's for
now, and P3 adds the two hand moves.

**What the game offers (static analysis):**
- `+D19BA0` is the ledge grab's input handler. In a hang, jump (action 0xB) sets state 4, or 9 when a game option bit is set; states 2 / 3
  plus jump set 6 / 7 (the side jumps). Actions 0xE / 0xF set 16 (the drop).
- `+D11D90` is the exit that rebuilds the walk body after states 0 / 8. Its `SetLinearVel` at `+D1204E` pushes the walk body along the
  player's facing at property 0x271 = **`MoveSprintSpeed`** (the id → name switch `+C59290`). That is the climb-up carrying into a run,
  not a jump. The call chain `PhysicsBind(player+0x18)` → bind vfunc +0xB0 (walk interface) → its vfunc +0x2C0 (IPhBody) →
  `IPhBody::SetLinearVel` is what P3 uses after its own drop.
- **Balance caps (tester: "the launch speed can't be higher than the default game one"):** `data/skills/default_levels.xml` sets
  `JumpMaxHeight` 1.01 m (property 670), `WallJumpBackSpeed` 3.0 m/s (492) and `WallJumpBackHeight` 0.8 m (491). The launch is capped
  at √(2g·1.01) = 4.45 m/s up and 3.0 m/s sideways (`kLaunchMaxUpMps` / `kLaunchMaxSideMps`). Skills that raise the game's values are
  not followed.

**Design (`climbhand_logic.h` `MotionStep`, game thread on the walk call):** both moves ride P2's carrier, the hold state
(gravity-free, moving at exactly the velocity sent). No animation, no camera.
- **Mantle**: once the highest held ledge point is at most `climb_mantle_m` (1.2 m) above the feet:
  - The body rises until the feet are 6 cm over the top, then moves 40 cm in along the ledge's inward direction, at 2.5 m/s.
  - Then the game's own drop (set-state 16) runs, and MoveController stands on the top.
  - Not there within 1.5 s (a ceiling, a railing) = dropped where it is ("MANTLE BLOCKED").
  - The anchors pause during the lift, and the grips stay withheld until they open.
- **Launch**: the last hand lets go while the largest velocity sent in the last 150 ms is at least `climb_launch_min_mps` (1.5 m/s),
  and the velocity is not mostly downward.
  - The body keeps that velocity × `climb_launch_scale` (1.25), capped as above.
  - It flies a ballistic arc of ours, still in the hold state. A hand that closes on a ledge in flight **joins** (round 7), and the
    hands take over ("LAUNCH CAUGHT"): hand-over-hand up a face.
  - At the apex comes the drop. The horizontal part then goes to the walk body with `SetLinearVel` (three updates in a row once the
    ledge grab has ended). The log measures whether it stuck.
- Anything slower is the plain drop, as before. The pad machine's drop wait pauses while a flight or mantle of ours runs.
- **Settings**: `climb_mantle_m` (0 = off), `climb_launch_min_mps` (0 = off), `climb_launch_scale` (0.5-2).
- **Log**:
  - The MANTLE / LAUNCH start lines and their end lines (done / blocked / apex / caught / interrupted).
  - "launch velocity after the drop ... the feet then moved".
  - The 5 s line counts mantles (blocked), launches (caught), plain drops.

**Not in this round:**
- The low-ceiling crouch mantle.
- Climbing XP for a mantle of ours (the game pays only after its state 8; §8.2 has the award call, P6).
- A look-aimed jump button: the game's own still works.
- Open for the headset run:
  - Does the drop from a body standing over the top land cleanly?
  - Does `SetLinearVel` stick after the drop?

**P3 run 10 (2026-11-27 10:22) and round 2.**

The tester's verdicts:
- **Our mantle:** "not great, stick to the normal game animation".
- **Up launches:** "work great".
- **Side / back launches:** they "stop you mid air soon after you jump off".
- **Requests:**
  - A boost (×1.25, no change to the caps).
  - An assist that turns up jumps toward vertical.
  - The low-wall auto climb off by default, behind a switch.

What the log showed:
- **The game's climb-up from a hang is a direct `1 → 8`.** Run 10 logged `0>1>8` 4 times, from the stick pushed forward. Three of our
  lifts were cut short by it ("interrupted ... state 8").
- **The walk body keeps almost none of the velocity handed to it after the drop:**

  | Sent sideways (m/s) | Feet then moved (m/s) |
  |---|---|
  | 2.98 | 0.36 |
  | 2.33 | 0.14 |
  | 1.32 / 1.84 | 0.18 / 0.26 |
  | 2.66 | 2.14 (the only one in 9 that kept it) |

  The game's own hang jumps (4 / 5 / 6) fly inside the ledge grab (§8.4), so ours now does too, for longer.

Round 2:
- **Climb-up = the game's own (state 8).** Round 1's lift is gone.
  - `climb_mantle_stick` (on): the stick climbs up, as the game does. Off = `RefuseClimbUp` refuses the game's own hold → 8 in hand
    mode.
  - `climb_mantle_auto` (**off**) with `climb_mantle_m` (1.2): pulling up that far calls set-state 8 once per hold. That is the same
    transition as the stick, and it plays the game's animation and pays its XP.
- **The flight continues past the apex** until one of these:
  - it falls at 5 m/s (`kFlightEndFallMps`, about 1.3 m below the apex);
  - it **lands**: the body is held back for 3 walk calls in a row while falling (`kLandCalls`). It is dropped there;
  - 1.5 s pass.

  Then comes the drop. Sideways speed is kept for the whole arc. `SetLinearVel` is still tried after the drop, best effort.
- **Boost**: `climb_launch_scale` is labelled "launch boost" (default 1.25). The caps come last: a launch that would pass the up cap is
  scaled **whole**, so the assisted direction is kept.
- **Up assist** `climb_launch_up_assist` (0.5), `UpAssist`:
  - A launch within 45° of straight up has its angle from vertical scaled by (1 − assist), speed kept (30° → 15° at 0.5).
  - The assist fades to none between 45° and 60°, so push-offs are never bent.
- **Low walls**: `climb_auto_low` (**off**). Off, the game starts no ledge grab of its own at all.
- A climb that ends with grips still squeezed (the climb-up, a drop) keeps them withheld until they open.
- **Log**:
  - "CLIMB-UP asked".
  - "LAUNCH falling fast now / LANDED / CAUGHT".
  - The 5 s line counts climb-ups by pull height, refused stick climb-ups, launches (caught, landed), plain drops.

**P3 run 11 (2026-11-27 11:04) and round 3.** Tester: "mostly all great", but the game sometimes climbed up by itself, often after
a jump up while hanging, with both auto options off.
- Climbs #1 and #10 went `0>1>8` with no new grab and no stick input. About 0.8 s after an upward launch, the walk body was gone (state 8),
  and the flight logged "interrupted ... state 8".
- So the game climbs up from a hang **by itself** once the body is high enough over its ledge, and the upward launch lifts it there.
  Round 2's refusal let every game 1 → 8 through while `climb_mantle_stick` was on.
- Round 3: the game's own hold → 8 passes only while the **left stick is pushed forward**. The pad thread records the last poll with
  stick y ≥ 0.5; the reading counts for 150 ms (`RefuseClimbUp` + `kStickForward` / `kStickFreshMs`). Ours (the height rule) still
  passes. Each refusal is logged with its reason.
- Caveat: the left stick is assumed to be the move stick (the game's default binding).

**Round 4 (same day, before a run):** with `climb_mantle_auto` on, a big jump flick could cross `climb_mantle_m` mid-pull. The
climb-up then took the hang, and the release no longer launched. The climb-up now waits until the body has been at the height **and**
slower than the launch speed (`climb_launch_min_mps`, 1.5 m/s if that is 0) for 200 ms (`kMantleDwellMs`, `Motion::upSinceMs`). A
flick is still fast when the hands open, so it launches; a pull-up held at the top climbs up.

### 8.15 P4 round 1 (2026-11-27): hands on the ledge, and a look probe [BIN + CODE]

**What the hang does to the look (static):**
- In the plain hang (state 1), `+D0A240` calls `+D45B90` at `+D0AC6B` with constant ranges and a direction from the ledge grab
  (vfunc +0x120).
- `+D45B90(lookWindow = *(player+0xBB0), a, b, dir, flag)` stores a look window around that direction: ranges at +0x30…+0x5C,
  seeded from the player's `+0x116C` / `+0x1170`. It returns early when its arguments have not changed.
- LookController reads its limits back through ILookController slots +64 (max) / +72 (min), each (yaw, pitch). Those are the getters
  `+BA1350` clamps the look with.
- Run 11 logged `[LOOKPITCH] LIMIT RELEASE ... last window pitch max +0.0 min +0.0`. So while hanging, the game's look pitch is held at 0.
  The yaw window is not logged anywhere yet.
- The headset's head yaw rides on the camera only while hanging (body-follows-head paused, §8.13). Whether the window limits what
  the player sees, the stick's smooth turn, or nothing, needs a run.

**Built:**
- **Hands stay on the ledge** (`climb_pin_hands`, default on, Physical Parkour > Moving while hanging):
  - Each anchored hand is drawn on its ledge point. The arm IK's wrist target `T` moves by (ledge point − hand), as the pad thread
    measures it (`PinShift`, published per poll). `boneblend.cpp` SolveArmIk does this after the collision stops and before the
    reach clamp.
  - Not drawn: farther than the release distance, or a publish older than 100 ms.
- **Look probe (log-only), one `HANG LOOK` line per hold:**
  - LookController's look window: the first one seen, and each limit's range over the hold.
  - How far the camera yaw (+4460) and look yaw (+4468) travelled from the hold's start, and the look pitch (+4472) range.
  - `boneblend.cpp` HangLookSample, `climbhand.cpp` HangLookTick.

**Headset test for round 1:** hang and look all around, including behind you; turn with the right stick (smooth, and snap if you
use it); look up and down. Say what stopped or moved by itself. The HANG LOOK lines say which limit did it. Round 2 then releases
what limits the view, likely by refusing the `+D0AC6B` call or widening the window.

**P4 round 2 (2026-11-27, after run 12).** Three reports from the tester:
- **(1) Corners:** "if I shimmy with my hands around a corner my body stays turned in the direction of the original hang ... I'd want
  it to follow the player's view, but not move the camera."
- **(2) Climb-up from the wrong ledge:** after several physical jumps up stacked ledges, the stick climb-up "moved me back to a lower
  ledge, shimmied up from it to the next one, then back to the top ... only after several tries it mantled".
- **(2b) Mid-air mantle:** stick forward during a physical jump mantled mid-air.

Then: two ledges held should use the last one grabbed, made a setting.

- **Cause of (2) and (2b):** the ledge grab's own ledge (its hook, `PlayerDI+0x14C8`) is still the one it first grabbed. Hand-over-hand
  moves and launch catches join new ledges but never re-target the game, so its climb-up and shimmies run from the stale ledge.
  During a launch flight the hang is still state 1, so a stick climb-up starts mid-air.
- **The game's own moves from a hang** (`GameMoveFromHang`, StateHook):
  - **Climb-up (8):** needs the stick forward (round 3) and is never allowed in flight. On a stale ledge (`HookMismatch`: the held
    ledge more than 0.4 m above / below or 1 m along the game's) it becomes **the game's own climb on the held ledge**: the drop,
    then the game's set-target and start on that ledge with no forced hang, so it climbs over where there is room (Hook,
    `g_reclimb`).
  - **Shimmies (11-15):** only with `climb_stick_shimmy` (new, **default off**), and never in flight or on a stale ledge.
- **The ledge held** (the climb-up height, the stale test, the climb on it): `climb_ledge_last` (on) = the last hand to grab;
  off = the mean of both (`LedgeHand` / `LedgeHeld`).
- **(1) Body turns with the view** (`climb_body_follows_view`, default on):
  - Body-follows-head is no longer paused while hanging (`ClimbHandFreezesBody` false).
  - Each walk call, the body's turn since the last call (`PlayerDI+0x11EC`, degrees) swings the head bone by R(d)·o − o, where
    o = camera − feet. The walk override adds the opposite velocity (`HeadOrbitComp`, capped 6 m/s), so the camera holds still.
  - The yaw convention is from `+D11FB1`: the facing × 0.01745 → (cos, sin) → world (x, z).
  - Off = round 5's pause.
- **Log:**
  - A refusal line per change, with the held vs the game's ledge when stale.
  - "CLIMB ON THE HELD LEDGE started".
  - The 5 s line: hang moves refused, climbs on the held ledge, degrees the hanging body turned, and the compensated walk calls
    (largest m/s).
- **Headset test:**
  - Shimmy round a corner by hand: the body should turn, the view stay put.
  - Stacked ledges by launches, then the stick at the top: one climb-up, at the top.
  - Stick forward mid-launch: nothing.

**Defaults changed (tester, 2026-11-27):** `climb_stick_shimmy` **on**, `climb_ledge_last` **off** (the average).

**The game's climb type, by height (static, `+D0AF40`'s tail, read 2026-11-27) [BIN + MEASURED].** The ledge height is measured
from the player's **bone 0** (`GetMeshElemFromBoneID(0)` → `GetElementWorldPos`), not the feet. The decision runs in this order:
- The ledge grab's "space on top" flag false = **0**, a hang.
- The player above the ledge while not falling fast (velocity dir · up < −0.3 and y < −10 are the exceptions) = **2** (from the top).
- Ledge **≤ 0.5 m above bone 0** = **3**, the short climb-up.
- The slope parameter ≥ 0.215 = **5**, or **7** if the ledge is more than 0.2 m above another body point (vfunc +0x730).
  Slope ≤ −0.215 = **6** / **8** in the same way.
- Otherwise: ledge **> 1.214 m above bone 0** = **1**, else **4**.

Run 12 pairs a ledge 1.41 m above the feet with type 3 and one at 1.44 m with type 4, which puts bone 0 at about 0.92 m above the feet
(the hips). Standing, that gives:
- the short climb (3) up to about **1.42 m**;
- type 4 from there to about **2.13 m**;
- type 1 above.

Lower walls (vaults, step-ups, `JumpOnLowStand` / `JumpOverLow`) are MoveController jump clips, not the ledge grab (§2.3).

**P4 round 3 (2026-11-27, after run 13).** Tester reports:
- (1) "on the top one when using stick up nothing happened at all" after jumping up several ledges.
- (2) Hands moved from one ledge onto an orthogonal one at a corner: the stick climb-up "mantled from the first ledge".
- (3) Wrong-ledge shimmies are rarer but still happen.
- (4) "The camera snapping when turning is back, same as before if not worse."

Run 13's event lines ran out at 14:13 (800-line budget, spent on per-hand join / anchor / let-go lines), so the rest of the run
is read from the per-climb summaries and the 5 s counters:
- **Cause of (1):** with `climb_stick_shimmy` on, the stick up reaches the game as a **shimmy up (13)**: the game, still on the
  lower ledge, sees a ledge above it. Round 2 refused a stale shimmy outright and only turned a stale climb-up (8) into a drop and
  restart. So nothing happened. Climbs #30, #33, #38 and #49 show refused hang moves with no climb-up refusal and no climb on the
  held ledge (always 0).
- **Cause of (2):** `HookMismatch` compared points only (0.4 m up, 1 m along). At a corner the two ledge points are close, and only
  the direction differs.
- **Cause of (4):** the run's `HANG LOOK` line shows the game's look yaw travelling with the camera (−104..+43°) once the fold was
  no longer paused. The hang's look pose turns and leans the head bone the camera rides (§8.13, round 4). `HeadOrbitComp` answers
  only the body facing's turn about the feet. Its sign was re-checked against the binary: `+D11FB1` → `+134ABB0` returns
  (sin, cos) in lanes (0, 1) and writes x = lane 1, z = lane 0, so forward = (cos, sin), as assumed. The pause, which freezes the
  look, was snap-free in runs 9–12.

Built:
- **Re-target in place** (`RetargetInPlace`): the game's set-target on the held ledge (lip ± `kHalfEdgeM`, like a hand grab), then
  `DoStart` without the group switch (the new `start` flag). No drop, no new start; the hang goes on. The drop-and-restart
  (`g_reclimb`) is gone.
- `GameMoveFromHang` now returns **`kGmRetarget`** for any stale move (8 or 11–15), whatever `climb_stick_shimmy` says: this move
  is refused, the game is re-targeted, and it asks again on its next update from the right ledge (the climb-up at the top, the
  shimmy up below another ledge). It is never used in flight.
  - If the game is stale again within `kRetargetHoldMs` (400 ms), the re-target did not hold: the move passes as the game asked,
    and is logged.
- `climb_mantle_auto`'s own climb-up re-targets first when stale.
- **`HookMismatch`** is tighter: 0.25 m up / down, 0.5 m along, or the direction more than ~30° off (`kStaleCos`). It now reads
  the game's ledge direction (`+0x14BC`).
- **`climb_body_follows_view` default OFF** ("experimental" in the menu). The tester's `ovr2xr.txt` had it written as 1 by the
  overlay; it was set to 0 there.
- **Log:** a `RE-TARGET done / REFUSED` line, with the game's ledge and the held one, and a "PASSED on a stale ledge" line when a
  re-target did not hold. The 5 s line counts re-targets, refusals and ones that did not hold; climb-up refusals in flight no
  longer count as stick refusals. Per-hand routine lines have their own budget (`HandLine`, 400), and the event budget is 1500.

**Open:** a body that turns with the view **and** a still camera needs the facing turned without the look. That means writing only
`+4588` / `+4592` while the hang keeps `+4460` / `+4468` paused, then `HeadOrbitComp` for the facing's swing, if the ledge grab's
body follows `+4588` at all. Whether the re-target alone turns the hanging body to the new ledge's direction at a corner is a
headset question.

**Headset test for round 3:**
- Launch up several stacked ledges, then the stick up at the top: one climb-up, from the top.
- Move both hands onto an orthogonal ledge at a corner, then the stick up: the climb-up goes over the new ledge.
- Turn your head and smooth-turn while hanging: the view stays steady.
- Look for `RE-TARGET` lines, and say whether the body turns at the corner when you press the stick.

**P4 round 4 (2026-11-27, after run 14).** Run 14 confirmed the corner re-target: climb #6 re-targeted from dir (0.01, −1.00) to
(0.98, −0.09), then climbed up (`0>1>8`). Tester reports:
- (1) "Using stick up at the top of a ledge makes the player rise up a bit but not mantle."
- (2) After a physical jump, stick forward "didn't interact with the colliders like it should, going through some and never stepping
  over or mantling". The tester: no mid-air mantle without a ledge in front, "but that doesn't mean we need to disable the action
  altogether".
- (3) "The body not looking where I'm grabbing issue is back" (round 3 turned `climb_body_follows_view` off).

Findings:
- **(1) and (2) are one case.** Climb #19 is a launch (14:43:22.9), then `1>9>1>9…` ten times. What state 9 is (static):
  - The game's stick-up in a hang (`+D16480..+D16639`) searches for a ledge **above** (`+D18A20`, twice) when it does not climb
    over. Found = **13**, a jump up to it, with the target stored at `+0x3C4..`. None = **9**, or **5** with the game option bit
    (`*(… +0x1F60)+8` bit 0): a short reach up that drops back.
  - The jump button from a hang also asks for 9 or 4 (`+D19D4B`, same bit).
  - Our flight flies inside the hang state, so the stick mid-air reached the game as that reach-up. Round 3 neither refused nor
    handled 5 / 9.
  - In flight the walk body's own physics and the game's own ledge grab don't run (it is our kinematic carrier), so nothing steps
    over.
- Hang flags `+0x204 / +0x205` read the same (0 / 1) for the hangs that climbed and for #19, so they are not what decides.

Built:
- **Air handover** (`kGmAirHandover`): in flight, any game move from a hang (8, 11–15, 5, 9) is refused, and the Hook ends our flight
  in the game's own fall: its drop, plus the flight's velocity to the walk body (the post path).
  - For `kAirStepMs` (1.2 s) the game's own ledge grab is let through for a ledge at most `kAirStepMaxM` (1.5 m) above the feet
    (`AirStepPass`), so it steps or climbs over the ledge in front by itself, with its own type choice and collisions. One per
    handover.
  - With no ledge in front it is a normal fall with air control.
  - The hang jumps (4 / 6 / 7) are still the game's, in flight too.
- **5 / 9 on a stale ledge** are re-targeted like the others; otherwise they stay the game's.
- **`climb_body_faces_ledge`** (new, default on, Physical Parkour > Moving while hanging): while hanging, and with
  `climb_body_follows_view` off, the walk override turns the **facing only** (`+4588` and its target `+4592`).
  - It turns toward the held ledge's inward direction at up to 180°/s (`FaceLedgeStep`; yaw = atan2(z, x), the `+D11FB1`
    convention).
  - The body moves by `HeadOrbitComp` of **our** turn: the change of the value we wrote, so the engine's capped chase of the facing
    toward the look is overwritten, not answered.
  - The look (`+4460` / `+4468`) is never written, so the hang's look pose, round 3's suspect for the snap, stays still.
  - Facing writes are what turned the hanging body last round, so the body should follow them.
- **Log:** `AIR HANDOVER done / NOT done` with the state asked and the velocity; `AIR STEP` when the game's own grab takes the ledge.
  The 5 s line counts handovers, the steps after them, and the degrees and calls of the turn toward the ledge.

**Headset test for round 4:**
- Launch up from a ledge, then push the stick forward over it. Expected: you step or climb over it. With nothing in front you just
  fall, with no reach-up hop.
- Shimmy round a corner by hand. Expected: the body turns to the new wall, and the view does not jump.
- Hang and turn your head. Expected: the view is steady.

**P4 round 5 (2026-11-27): round 4 replaced before any run.** The tester called round 4's air handover and face-the-ledge turn
"extremely specific and patchy":
- "Everything can be fixed cleanly by just dropping out of the hanging state entirely as soon as the player is no longer hanging ...
  If there are issues with maintaining movement speed when changing states, that is the issue we need to address."
- "What if I'm grabbing a ledge behind me? ... have the body direction follow the view as before, but instead of the camera
  position being snapped to the eye bones, have it follow the eye bones smoothly."

Built:
- **The launch is the game's own drop at once.**
  - The in-hang flight (`StartFlight`, `kMotFlight`, the caught / fell / landed ends) is gone, along with round 4's air handover
    and step window.
  - From the first moment the body is the game's walking / falling body: its collisions, its stick moves, and its own ledge grabs.
  - A hand that closes on a ledge mid-air starts a grab as from the ground. `RoomscaleOnFoot` is a controller-group test, and
    falling is included.
  - No hold state is ever mid-air, so `GameMoveFromHang` lost its flight case.
- **The speed problem itself.** Run 10 showed the walk body sets its horizontal velocity every update from the walk's **wanted**
  vector (the `+ED2040` call, the stick's move), so a one-off `SetLinearVel` keeps only the vertical.
  - The **carry** (`CarryStep`): from the drop until it lands or `kCarryMaxMs` (2 s), every player walk call gets the launch's
    horizontal velocity plus the stick's own.
  - The stick steers but never stacks on the launch: the speed is capped at the larger of the two.
  - The rise is `SetLinearVel`'s, as before (the post path), and gravity is the game's.
  - Landing: after 120 ms, the ballistic vertical says "falling" while the measured one stopped.
  - The walk detour now runs its look-turn first and hands the result to the override, so the carry adds onto the game's
    look-turned stick.
- **The view: `climb_view_smooth`** (new, default on; Physical Parkour > Moving while hanging), `ViewSmoothStep`.
  - While the ledge grab runs in hand mode, boneblend's camera-base call (`EyePointApply`) adds a correction from
    `ClimbHandViewCorrection`. The view follows the eye bone's offset from the body's origin (`IControlObject::GetWorldXform`)
    through a low-pass: τ 120 ms, never more than 35 cm off.
  - The body's own translation passes 1:1. The headset's arc term (d2) is never smoothed.
  - The correction goes on the view **and** the hand anchor, so the drawn hands stay on the controllers.
  - It starts at the eye bone and eases back after the climb, so there is no jump at either end.
- **`climb_body_follows_view` default on again.** Round 2's `HeadOrbitComp` and round 4's `climb_body_faces_ledge` (never run, so
  its key was dropped rather than locked) are gone.
- **Log:**
  - `LAUNCH started`.
  - `LAUNCH CARRY`, once per launch: the launch's horizontal speed against the feet's over the first ~300 ms. If they match, the
    carry works.
  - `LAUNCH CARRY ended` (landed / time out / a grab started).
  - `VIEW SMOOTH first used`.
  - The 5 s line counts carried walk calls, landings, the smoothed camera calls and the largest offset (cm).

**P4 round 6 (2026-11-27, before any run of round 5): momentum in the air.** Tester requests:
- "I don't think you are taking collisions into account, nor do I like the 2 seconds limit."
- "A setting to completely remove the game overriding the speed mid air. Let physics do its thing ... some sort of air drag ... On
  the ground I do agree the stick should take over."
- "Ideal to have this mode work generically on all air time."

Built (`CarryStep`, `AirStep`):
- **Collisions:** from 120 ms after the start, the carry follows what the body really did: the horizontal velocity measured from the
  feet between walk calls, with calls where the feet did not move skipped.
  - `climb_air_momentum` (new, default on): the walk is handed exactly that, × exp(−`climb_air_drag` · dt) (new, default 0.1 per
    second). The stick has no say in the air. A wall stops the part of the momentum into it, and a slide keeps only the slide.
  - Off: round 5's launch + stick, but the launch part loses what the body lost against what it was sent, so it is never pushed
    into a wall again and never re-grows.
- **No time limit:** the carry ends when the body lands, which is `kCarryLandCalls` (3) calls in a row of "the ballistic vertical
  says falling, the feet don't". A body that stops dead counts too. 10 s is only a safety net.
- **All air time** (`climb_air_momentum_all`, new, default on): free fall is read off the feet. Over two sample triples in a row, the
  vertical speed must drop at 0.5–1.5 g. A slope (steady vertical speed) and a stair (one spike per step, out of range) never
  match. The carry then starts with the body's own measured velocity.
  - It runs only under MoveController: the walk detour now passes `MoveCtlActive`, so ladders, ropes, ziplines and vehicles end it.
- **Log:** the first 10 `AIR CARRY started` lines; `AIR` / `LAUNCH CARRY ended` with the momentum left; the 5 s line counts air
  carries.

**P4 round 7 (2026-11-27): air control.** The tester: "the stick should have a say ... 0 is the stick has no influence, and 1 is the
game's default influence ... at most as much as the original game does". On the detector: "fires about 30 ms after takeoff even on
the way up - this is perfect", so the free-fall detector stays (no ground raycasts).

The engine exports no sphere / capsule sweep, only rays (`IGSObject::Raytrace`). It does export `IPhBody::GetNumContactJoints`,
`GetContactJoint` and `IPhJointContact::GetContactPos`, and `IPhysicsWalkConfig::GetRadiusXZ`. None of them is used yet.

How the game's air control works [BIN, engine e9fbfc2c, gamedll 97046bba]:
- The gamedll reads `LogicJumpAirControllFactor` (property 0xCA, 0.22) at `+D105C1` / `+D4DB87` / `+DD65F9`. It passes the value to
  `+BE08F0`, which also writes `player+0x2BD4` (the factor), `+0x2BD0` (1.2) and `+0x2BD8`.
- `+BE08F0` calls walk interface vfunc `+0x388` = **engine `+3D3660`**. That clamps the factor to 0..1 and stores it at interface
  `+0x3AC` = object `+0xAAC`. The constructor's default (`+3D3DDC`) is 0.2.
- The walk interface vtable is engine `+86D618` (145 slots; its RTTI locator `+8EFE10` gives offset 0x700, the same type as the main
  vtable `+86DC38`).
- **engine `+3D87D6`:** while off the ground (flags `+0x9C0` bit 3, `+0x824`, a vfunc `+0x3D0` test), the walk's **wanted** vector is
  scaled by the factor **cubed** (0.22³ ≈ 0.011). The stick's say in the air is the engine's own, applied to whatever wanted vector
  `+ED2040` hands it.
- Also read: `+3D919C`, where min(factor, k) scales the body velocity `+0x768..` and `+0xA50..` (not followed), `+3DD8EB` and
  `+3E25BE`.

Built:
- **`climb_air_control`** (new, 0..1, **default 1**): `AirControlPush` hands the walk momentum + s · max(0, |w| − h·u) · u, where
  w is the game's stick vector and u its direction.
  - Read the game's air step as v += k(w − v), k = factor³. That is the stick's push k·w plus a pull back to rest −k·v (the game's
    own air drag). The push is **added** to the momentum; the pull is replaced by `climb_air_drag`.
  - The push tapers as the speed along the stick nears |w|: exactly the game's along-stick term k(|w| − v·u). So the stick never
    makes you faster than it would on its own, and pulling against the momentum brakes as hard as in the game. Across the stick
    the momentum is kept.
  - At 1 it is the game's own push; at 0 it is momentum only. It applies only after `kCarryArmMs`, because just after a launch's
    drop the walk body may still act as grounded.
- First cut (replaced the same day): a blend h + s(w − h). At 1 that was the game's vector outright, i.e. vanilla air movement with
  momentum and drag gone. The tester: "At 1 it should add that 0.22 cubed stick force to the momentum, not replace it".
- Not settled from the binary: `+3D87D6` scales a displacement (wanted + `+0xA94..`) by k and later divides by a time; whether the
  body's velocity then blends toward it (as assumed above) or the push is added to a velocity physics keeps was not traced. The
  `LAUNCH CARRY` line shows whether the momentum is kept.

**P4 round 8 (2026-11-27): run 15.** The tester: jumping up from a hang impossible; the stick still overriding the air momentum
(with air control and air drag off); the camera shift when turning the head while hanging "as strong as ever"; mantling still
failing sometimes ("What would be the disadvantage of just updating the current ledge whenever it's needed"); air momentum firing on
the ground ("move me forward super fast") and rarely in the air - "if the game needs to know if the character is mid air ... it MUST
know the player is mid air, why can't we use that detection?"

What run 15 showed (the CLIMBPROBE lines log the walk's wanted vector, its flags word and the measured velocity every update):
- **In the air the walk moves the body at exactly the horizontal vector it is handed** (wanted = measured, call after call, flags
  0x0002). The engine's air factor did not scale it. So round 7's push (the whole gap to the stick's speed) let the stick take over.
- **The vertical:** a launch from a high hang (the walk already 0x0002 while hanging) lost its rise in one update; the one launch that
  rose started with the walk at 0x0001 and rose until the flags went 0x0002, then 3.2 -> -0.2 m/s in one update. The walk runs its
  own vertical in the air; `SetLinearVel` on the body is not it.
- **AIR CARRY** started from the free-fall test on the ground and grew to 9.5 m/s (the whole-gap push on a carry never ended).
- Run 15 #17 went `1 > 9 > 1 > 16`: the game reached up (no climb-up from its ledge) while the stale test called its ledge the same
  as the held one, so nothing was re-targeted.

Binary findings [BIN, engine e9fbfc2c, gamedll 97046bba]:
- **The walk's airborne flag:** its update (`+3D6970`) rebuilds the flags word at walk `+0x9C0` every step: bit 0 grounded, bit 3
  just above the ground, **bit 1 = neither** (set at `+3D6DA8` only when not grounded and bit 3 clear). Walk interface vfunc `+0x468`
  (engine `+3D37E0`) returns bit 1; the gamedll calls it ~100 times (bind `+0xB0` then `+0x468`), e.g. `+D233A3` before reading
  JumpMaxHeight. `+0x3D8` (`+3D37F0`) returns the whole word.
- **The walk's vertical speed:** vfunc `+0x260` (engine `+3D3760`) stores a float at walk `+0x7AC`; `+0x268` (`+3D3770`) zeroes it.
  The air step reads it at `+3D8666` with the fall. The gamedll's own velocity hand-over (`+D68FB4..+D6901F`) sets the wanted vector
  through `+0x2D0` and then `+0x260` with the vector's y; `+D648F0` clears it.
- The air factor: vfunc `+0x390` (`+3D3680`) returns walk `+0xAAC`.
- Not used: walk `+0x954` / `+0x960` (set by vfunc `+0x3C0`, `+3E0450`) are a contact point and normal, not a velocity.

Built:
- **Air time = the walk's own flag** (`WalkAirborne`, vfunc `+0x468` after a byte check of the function it points at). An air carry
  starts when it is set (with the feet's measured velocity, `FeetVel`; no arming wait) and every carry ends the first call it clears
  after having been set (`CarryStep`'s `air`). A launch the walk still holds as grounded waits for the flag; the ballistic landing
  test stands in only before the flag was ever seen, or when it cannot be read. The free-fall detector (`AirStep`) is gone.
- **Air control:** the push is now k x the gap to the stick's speed along the stick per walk update, k = the walk's air factor cubed
  (`WalkAirK`, vfunc `+0x390`; 0.22³ = 0.0106 when unread) - the game's own along-stick step, added to the momentum. Held for a
  second at run 15's ~72 updates a second it bends a 0 m/s sideways speed to 2.15 of a 4 m/s stick.
- **The launch's rise:** with the velocity post, the walk's vertical speed is set to the launch's (`WalkVertical`, vfunc `+0x260`),
  as the game's own hand-over does; the carry's end clears it (`+0x268`). Not proven: whether the air step adds it to a fall that
  restarts at take-off - the trace below shows it.
- **Ledge on demand:** every move the game asks from a hold (climb-up, shimmies, reach-up) is first re-targeted onto the ledge the
  hands hold whenever that is known, stale or not; the game's next ask (within `kRetargetHoldMs`) goes by the usual rules. Cost: one
  update (~14 ms) per move; a re-target onto the same ledge changes nothing visible. `HookMismatch` stays for the log.
- **Logs:** `CARRY TRACE` (the first 6 launches for 700 ms, the first 4 air carries for 1.2 s, every walk call: walk flags, feet vy
  against the ballistic, horizontal handed / measured / stick, the air factor); `CAM SNAP` (the camera moved more than 1.5 cm against
  the body's origin in one camera call, with what each part did: the game's base, the eye bone, d1, d2, view smoothing, the hand-twist
  hold, body and headset yaw) and a `CAM` line every 250 ms while the ledge grab runs; the 5 s line counts the snaps.

**P4 round 9 (2026-11-27): run 16.** The jump up from a hang works (the walk's vertical speed). The tester: sideways air control
"extremely powerful at the default 1 value, like more powerful than when walking even"; "moving the stick forward increase speed
noticeably even when we are already pretty fast".
- **The air factor:** the walk's live factor (`+0xAAC`) read **1.0** in every traced jump, so k = 1 and the stick took the speed over
  in one update. The push uses the game's data value again (`kGameAirK` = 0.22³); the live value is only in the trace.
- **Speed growing:** the measured horizontal speed spikes when the body is pushed out of geometry (4.56 -> 8.22 m/s in one call with
  the stick at 6, control 0), and momentum mode kept each spike. The measurement may now slow or turn the momentum, never make it
  faster than the last call handed (`CarryStep`).
- **The camera snap, found:** `CAM SNAP` lines show the face offset d1 jumping 19-24 cm in one call (24 snaps) while everything else
  moved 1-2 cm. In the hang poses the face bones sit 16-20 cm from the eye bone (65.8 mm standing); past 0.20 m `EyePointApply`
  zeroes d1, so the view flipped between ~20 cm and 0 as the head turned. While a climb runs (and its 3 s tail) with climb_by_hand on,
  d1 is now the remembered standing offset through the eye bone's orientation (the crouch hold's path; `CLIMB HOLD FIRST` line).
- **Launch on a slip:** every launch in run 16 had both grips still closed (`DROP REQUESTED ... grips L1 R1`) - the hands were let go
  by climb_release_m (0.46-0.51 m from their ledge points), and `LaunchPick` read the yank toward the other hand as a pull. A launch
  now needs both grips open when the hang ends (`DropMayLaunch`); a slip is the plain drop (its `DROP` line says so).
- (Round 10 below revisits the slip rule and the momentum measurement.)
- The traces restart when climb_air_control or climb_air_drag change (run 16's four traced air carries were all at control 0).
- Also seen: the hang turning the body at ~250 deg/s with the headset still (body yaw -3.9 deg a call) and the game's camera base
  moving 1.5-2 cm a call in the body's frame during it; not changed.

**P4 round 10 (2026-11-27): run 17.** The tester: camera snaps still there; sideways swings and air speed-ups gone, "but now stick
forward actually slows down air momentum considerably"; the upward jump "failed very often".
- **Momentum wearing down:** run 17's traces: 5.95 -> 2.75 m/s in 1 s where air drag 0.2 leaves 4.9. Walk calls and physics steps
  do not line up, so the speed measured call to call swings both ways, and round 9's cap kept the low readings and clipped the high
  ones. Momentum mode now measures over up to 250 ms of calls (at least 60 ms; one physics step more or less is then <= 6 %) against
  the time-weighted mean of what was handed over the same time: equal = h untouched (free flight: only drag and the stick); slower
  than 85 % of it or turned more than ~10 deg = h becomes the measured velocity (never faster than the last call handed).
- **Up jumps failing:** most were round 9's "a hand SLIPPED" - a jump flick tears the hands past climb_release_m before the grips
  open. A slip now launches when a controller moved at climb_launch_min_mps or faster in the PLAY SPACE within kLaunchWindowMs
  (`HandSpeed`, the OpenXR controller pose: the body's motion is not in it); round 9's yank (body moved, controllers still) stays a
  plain drop. The `DROP` and `LAUNCH` lines give the fastest controller speed.
- **Camera snaps left:** with d1 held, the `CAM SNAP` lines show the hand-twist view hold (comfort_body_follows_head's correction)
  flipping between 0 and ~25 cm in one call while hanging. It is now off while the ledge grab runs in hand mode (`ClimbHandHanging`).

**P4 round 11 (2026-11-27): the rigid view.** The tester, on the 1.5-2 cm per call the game's camera base moves during climb poses:
"can't we remove the pose changes that affect the view altogether? They don't add anything mechanically and are only there to
improve immersion in a flatscreen game".
- **`climb_view_rigid`** (new, default on): while the ledge grab runs in hand mode, `EyePointApply` moves the camera base from the
  animated head to the **standing** one - the game's base plus the face offset, body-local, remembered on every call that refreshes
  the standing face offset (standing still, not crouched, no climb in the last 3 s, no swing). The body's own motion and the
  headset's arc still move the view; the climb animations do not.
- It eases in and out over 250 ms (`RigidRampStep`), so a climb's start and end do not jump. The face offset d1 and
  climb_view_smooth's correction fade out by the same share; the hand anchor gets the same world shift (the drawn hands stay on the
  controllers). With no standing base remembered yet it stays off (counted).
- Not covered: vaults and other moves that are not ledge grabs.
- **P4 round 12, `climb_hold_body`** (new, default on). The tester, with the rigid view: "the body tends to lean forward a ton,
  sometimes actually bending at the torso" (the screenshot: the yellow shoulder filling the lower view while hanging). The hang and
  climb clips bend the spine, neck and head forward. The throwable body hold (`AhThrStep`, rig_throwable_body_hold: those ten
  elements stop taking ROTATION from the animation, INV-283 bit 9, and translation on the four the clip moves; the pelvis with
  rig_throwable_hold_pelvis) now also arms while the ledge grab runs in hand mode. The episode starts with the grab, so the held pose
  is the upright one the grab began from; it lets go when the grab ends (`CLIMB BODY HOLD` line, then the usual ARMED / RELEASED).
  Needs rig_throwable_body_hold and ANIMHOLD's proven player-only map (run 17: `THROWABLE HOLD READY`).
- Also fixed: the Parkour menu's hang section said 6 keys while listing 7, so "climb_stick_shimmy" was never drawn there; the
  section sizes now come from the arrays.

**Headset test for round 5:**
- Launch sideways and backwards off a wall. Expected: you keep flying the whole arc. Check the `LAUNCH CARRY` line.
- Launch up past a ledge and push the stick forward. Expected: the game steps you over it, or you fall normally if there is nothing
  there.
- Catch a ledge mid-launch with a hand.
- Hang, turn your head and shimmy around a corner. Expected: the body follows your view, and the view glides instead of snapping.

### 8.9 LadderController map (T5, static analysis 2026-11-26) [BIN]

**Shape.** Bases: PlayerController, IMoveController@24, IBodyController@32, ILookController@40, IInventoryItemController@48,
IVisibilityController@56. **No IPhysicsController** (EdgeGrab and RopeLocomotionController have one). The ladder takes no physics
answers; it places the body itself, which is why the walk body reads null on a ladder. Code lives at `+D33C90`..`+D3CBB0`.
RopeLocomotionController (`+D73B60`..`+D7EDC0`) is a separate class with IPhysicsController and was not mapped.

| vtable | slot | rva | role |
|---|---|---|---|
| main | 16 | `+D35C20` | **per-update**: timed-state transitions, stick → state 0/1/2, then `+D3BF20`, `+D3B6D0`, `+D3B990`, `+D3BBA0` |
| main | 18 | `+D34340` | attach (5.7 KB): writes the attach-time vectors below once |
| main | 19 | `+D36170` | detach: state = -1 |
| move | 0 | `+D38730` | the move vector: constant unless state 13 (then a scaled vector by time) |
| move | 9 / 10 | `+D33CB0` / `+D33C90` | state == 13 / state is 6 or 12 |
| — | — | `+D35F60` | **set-state (this, s)**: the only writer of `+0x134` |
| — | — | `+D38E30` | fills the state durations `+0x40`.. (animation lengths) |

**Climbable kinds** (`data/presets/ladderdi.pre` in Data0.pak, field `m_LadderType`): `ONE_SIDE_LADDER`, `PIPE`, `ROPE`,
`UNDERWATER_ROPE`. These are very likely the climbable's kind 0..3: 2 and 3 slide on their own (state 13), which fits the two
ropes. So ladders, pipes **and ropes** all run LadderController. A pipe may carry `m_BlockedRung` (0.7 / 1.1 / 2), a rung it
cannot pass. The animations (`m_fpp_ladder.scr`) are per kind (`Ladder_*`, `Pipe_*`, `Rope_*`): enter up / down, leave up / down,
climb up / down (left and right hand), slide, look left / right, rotate (pipe and rope), and one jump.

**Fields** (complete object):

| off | meaning |
|---|---|
| `+0x134` / `+0x138` / `+0x13C` | state / previous state / the state's start (game time) |
| `+0x40 + 4·s` | duration of timed state s; states 5..14 move on when it runs out (jump table at `+D35F30`) |
| `+0xCC` / `+0xD0` | rung count / current rung (int); `+0xD4..+0xDC` that rung's world position (`+D38570(this, &out, rung+1)`) |
| `+0x10C` | angle around the axis (deg), turned by input actions 7 − 6 while `+0x130` / `+0x131` allow; clamped by the climbable |
| `+0x104 / +0x108 → +0x114 / +0x118` | two values smoothed (SmoothCD `+29AB00`) toward their targets each update |
| `+0x11C`, `+0x11D` | flags (`+0x11D` = turning this update) |
| `+0x168`, `+0x170` | pointers needed for the stick-driven climb (both non-null) |
| `+0x188` | **the climbable** (interface): vfunc `+8` valid, `+0x10` a point (its y = the top?), `+0x18` a vector, `+0x28` **kind 0..3**, `+0x30` angle limits |

**States** (from the set-state callers; names are inferred):
- **-1** off.
- **0 / 1 / 2** on the ladder: idle, and the two climbing directions. These are kept only for climbables of kind 0 or 1. The stick,
  projected on the player's facing (`player+0x11EC`), picks 2 or 1 past a threshold and 0 below another.
- **3 / 4** turning around the axis, one way or the other (`+D3BBA0`).
- **5, 6, 8, 9, 10, 11, 12, 13** timed: enter, exit at the top or bottom, a jump-off, a turn-around. **13** is the slide: held input
  0x112, forced on for kinds 2 / 3. It ends at the bottom rung with 11. Exact names wait for the probe.

**For P5 (hand-driven ladders and poles) - SUPERSEDED 2026-11-26:** the tester wants no ladder animation or rungs at all (§4.9,
revised). LadderController will not run in hand mode, so the route below is dropped. Its map stays useful for refusing its start
(T7) and for reading the jump speeds. The original suggestion: height is **rung-based and animation-timed**. There is no velocity to write as there was
for the ledge grab. The cheapest faithful route is to drive the ladder's own state machine: a hand pull of about one rung spacing
asks for state 2 (or 1) by feeding the stick or calling set-state, and the game plays its own rung animation and motion.
Sliding = 13, turning = 3 / 4, letting go = the game's jump-off or exit states. Writing positions directly would fight the per-rung
animation. Open before P5: which kinds are ladder / pipe / pole, the rung spacing, and whether set-state alone (without the stick)
moves a rung. The probe now logs all of it (below).

**Probe additions** (`climbprobe.cpp`, `diag_climb_probe`):
- On a ladder, a `LADDER` sample line replaces the walk / ledge columns, which are all NaN there. It carries state / prev, rung /
  count and rung position, player y − rung y, angle and smoothing, and flags.
- A `LADDER ATTACH` line, once per climb, with the climbable's RTTI class, rung count, every attach-time vector and the duration
  table.
- The END summary carries the ladder's state sequence.
- A ledge grab alone now gets 120 samples per climb (`kLedgeSamplesPerClimb`), not 900, so a session no longer runs out before its
  ladders and poles.

---

## 9. How this fits with item 1

Item 1 is **parked** in [plan_movement_decoupling.md](plan_movement_decoupling.md). Its "canonical forward" stick (A1)
is independent of all of this: it only touches MoveController's ground walk. The camera-base change
(A2) and the parkour view hold (A3) reduce the same discomfort the climbing launches and mantles will cause, so they
are worth having before P3.
