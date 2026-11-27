# Plan (parked): separate VR movement from body direction and animations

Status: **parked** on 2026-11-25 so work can focus on hand climbing ([design_climbing_overhaul.md](design_climbing_overhaul.md)).
Nothing here is built. The evidence behind each step is in [feasibility_movement_climbing.md](feasibility_movement_climbing.md)
(§1-§2 and §4). This file is the "pick it up again" checklist.

## What is wrong today (user report)

1. Movement follows the body's facing, not where you look.
2. Changing direction lags (an invisible turn animation).
3. Sprint only works almost straight ahead.
4. The head and body shift a lot when the movement direction changes.
5. Parkour turns the view (for example the shimmy faces you to the wall).
6. Air control is weak.

## Key facts (from the feasibility study)

- Ground walking is velocity-driven: the player walk `+BA2B20` calls the walk-speed setter `+ED2040` with
  `w = Root3x3 * l` (`l` = `PlayerDI+4616`). The mod already detours `+ED2040` (`walklook`, `boneblend.cpp`).
- The pad thread owns the left-stick bytes and L3 (sprint) (`ovr2xr.cpp`, `vr_move` / `vr_decouple_move`).
- The camera base is the animated eye bone, set in `+C75380` (`[EYEPOINT]`, already detoured).
- Parkour view takeover comes from the facing supplier (`+BB7BF0`) and LookController's narrowed windows (INV-772).
- Air control: `LogicJumpAirControllFactor` 0.22 and `TurnSpeedAirMovementFactor_*` in `data/skills/*.xml`.

## Steps, in order

### Step 0: diagnose with no build
- Body follows head ON; walk and sprint while turning the head. Read the `[WALKLOOK] 5 s` lines in `dlvr_ovr2xr.log`.
  High "fold not folding" / "self-check failed" / "not MoveController" counts mean complaint 1 is a bug in an existing
  feature, not a design gap.

### Step 1: "canonical forward" stick (A1), the first prototype
- **Pad thread**: on the ground in MoveController, send the game a straight-forward stick with the real magnitude; keep
  the real stick angle θ (relative to the look) in a shared variable.
- **`walklook::Decide`**: after `WalkOnLook`, rotate the world vector by θ about +Y.
- **Gates** (pass the real stick through): jump pressed or held (dodges, wall-run, tic-tac), `mc+448 != 0`
  (airborne/dodging), `DefensiveController` or `EdgeGrabController` active, menus.
- Decision logic in a new pure header (for example `ovr2xr/stickfwd_logic.h`) with `tests/stickfwd_logic_test.cpp`.
- New setting, default OFF, so it can be A/B tested from the overlay.
- Add a log-only probe at the same spot (stick, `l`, `|w|`, `+4588`, `+4468`, `mc+448`, sprint byte `mc+72`) for 3 s after
  each direction change. This settles the cause of the turn lag (2) and the sprint cut-off angle (3).
- **Headset test**: sprint diagonally and around corners with the setting on and off; flick the stick between directions
  while walking and sprinting; check dodges, wall-run, tic-tac and slides still work.
- **Known trade-offs**: strafe and backward moves get forward/sprint speed (can be scaled back by angle); co-op peers see
  a forward run while you move sideways.
- Targets complaints 2 and 3, and partly 4.

### Step 2: steady camera base (A2)
- In the existing `+C75380` detour: on the ground in MoveController, replace the eye bone's animated offset from the root
  with a filtered standing eye height. Keep the animated base during climbs. Keep physical crouch's duck factor.
- Targets complaint 4.

### Step 3: hold the view through parkour (A3)
- In `camyaw::Hook`: while a facing supplier is active or the look window is narrowed, cancel the engine's own change to
  `+4460` from the addend; hand back on exit by moving all four yaw fields together (as snap does).
- Climbing hollow mode (climbing design §4.7) covers EdgeGrab and Ladder, so this step may shrink to wall kicks,
  landings and slides.
- Targets complaint 5.

### Step 4: air control
- First data: a `default_levels.xml` in the mod's fixed Data3 with a higher `LogicJumpAirControllFactor` (try 0.5-0.8) and
  `TurnSpeedAirMovementFactorMax`. Mind that it changes flat play and co-op balance too.
- Code only if a probe shows airborne frames still call `+ED2040` (then blend the in-air velocity toward the stick).
- Targets complaint 6.

## Not recommended
- Replacing MoveController with our own locomotion (A5): reimplements stamina, noise, XP, slides, tackles and co-op state.
