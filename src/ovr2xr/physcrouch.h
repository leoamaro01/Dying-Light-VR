// Physical crouch - plan docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 9 (amended), spec §2.3.
// A real crouch (the head's height in appSpace drops past a learned standing level) holds the game's own HOLD crouch
// (_ACTION_DUCK, action 15) through an input nothing else uses: EJoy__BUTTON_16 (0x0F0F), bound in our Data3 inputs_pad.scr and
// sent through the game's InputManager pad entry (slot 148) from inside the scePad poll. The view is the game's, exactly as a B
// crouch (Jay 2026-10-27 after headset run 5: "2" - the eye lift was removed); while crouched, the real drop is taken out of the
// head height the engine is given so the body is the regular crouch's (option A, after run 6). Decisions: crouch_logic.h.
#pragma once

namespace dlvr {

extern bool  PhysicalCrouchEnable;     // physical_crouch_enable
extern float PhysicalCrouchOnDropM;    // physical_crouch_on_drop_m
extern float PhysicalCrouchOffDropM;   // physical_crouch_off_drop_m
extern bool  PhysicalCrouchSlide;      // physical_crouch_slide - a physical crouch during a sprint is sent (the game slides)
extern bool  DiagPhyscrouchCycle;      // diag_physcrouch_cycle - the FLAT test's synthetic head

// scePad poll (the engine's pad-delivery thread). gameMenu = the in-game menu pass-through is on (no new crouch edge then).
void PhysCrouchPoll(bool gameMenu);

// Game thread, EyePointApply, every player camera update: the applied duck factor (vis+1332), or a negative value when unread.
void PhysCrouchNoteGame(float duck);

// ovr_GetTrackingState (the engine's job workers): metres to ADD to the reported head y - while the game is crouched, the real head
// drop below the learned standing neck height, up to the game's own drop (option A, so the spine is not crushed). 0 otherwise.
float PhysCrouchReportLiftY(float headY);

// Any thread: how many crouch / stand edges the head has sent so far (each 0x0F0F press or release that went through). The HUD
// panel's lazy follow re-places the panel at the head when it changes (uipanel.cpp; Jay 2026-10-27, option A).
long PhysCrouchEdges();

// Climbing round 4: hold the game's crouch until this GetTickCount64 tick (climbhand renews it while a crouched climb-up's spot has
// no standing room). Works with physical crouch on or off. Any thread.
void PhysCrouchForceUntil(unsigned long long untilMs);

}  // namespace dlvr
