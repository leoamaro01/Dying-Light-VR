// Climbing P0 probe - docs/design_climbing_overhaul.md §6 P0 (the questions it answers: §8.1 "what cancels gravity while hanging",
// how the ledge-grab / ladder controllers hand the body its velocity, which climb types and states a real climb walks through).
// LOG-ONLY: it reads the player's controller table, the ledge-grab controller's fields and the walk body, and calls only read-only
// game functions (the active IMoveController selector and the ledge grab's two IPhysicsController answers). It writes nothing.
// The decisions (window, caps, state sequence, measured velocity) are climbprobe_logic.h (tests/climbprobe_logic_test.cpp).
#pragma once

namespace dlvr {

extern bool DiagClimbProbe;   // diag_climb_probe - the [CLIMBPROBE] lines. DEFAULT OFF.

// Game thread, once per player update (boneblend.cpp HookTail, right after RoomscaleProbeTick, so the world position is this
// update's). Off: one bool test.
void ClimbProbeTick();

}  // namespace dlvr
