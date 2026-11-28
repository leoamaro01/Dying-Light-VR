// R9 - the grapple aimed by the RIGHT controller, not the head.
// SPEC-GRAPPLE-AIM-AND-LOOK-PITCH-2026-09-27 part A; evidence INV-267 (+ gr5/gr6 addendum), INV-272 addendum (gr9).
#pragma once

#include <cstdint>   // uintptr_t (ReadBowCtrlPtr)

namespace dlvr {

extern bool GrappleHandAim;          // grapple_hand_aim          - A: the ray's direction. DEFAULT ON.
extern bool GrappleHandOrigin;       // grapple_hand_origin       - A2: the ray starts at the hand. DEFAULT ON.
extern bool GrappleCrosshairMarker;  // grapple_crosshair_marker  - A3: the centre crosshair marks the hand ray. OFF.
extern bool GrappleMarkerHideOnMiss; // grapple_marker_hide_on_miss - the dot leaves the screen when the cast hits nothing. OFF.
extern bool FireReticle3d;           // fire_reticle_3d           - the gun reticle as a 3D quad (overlay.cpp). OFF.
extern bool HideGameCrosshair;       // hide_game_crosshair       - the game's centre crosshair parked when no VR use is active. OFF.
extern bool GrappleAimFromGun;       // grapple_aim_from_gun      - gun out: the grapple ray = the barrel line. ON.
extern bool Grapple3dReticle;        // grapple_reticle_3d        - grapple CAN/CAN'T icons in 3D at the target. OFF.
extern float GrappleReticleScale;    // grapple_reticle_scale     - its icon size, 0..1. 0.4
extern int   GrappleAimHand;         // grapple_aim_hand          - the hand whose ray aims the grapple: 0 left, 1 right (default)
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagMoveBob = false;
inline constexpr bool DiagRayThreads = false;
inline constexpr bool DiagGrappleAim = false;
#else
extern bool DiagMoveBob;             // diag_move_bob             - [MOVEBOB] aim/camera bob per frame by speed bucket. OFF.
extern bool DiagRayThreads;        // diag_ray_threads          - [RAYTHREAD] bullet-cast vs HUD-update threads. OFF.
extern bool DiagGrappleAim;       // diag_grapple_aim          - [GRAPPLEAIM] per-second windows. DEFAULT OFF.
#endif

// Installs the import-table hook once, the first time it is called with the
// feature on and the gamedll mapped. Cheap to call every poll: it returns on a
// latched flag. Refuses in the public build (same policy as aimhook).
void GrappleAimInstall();

// fire_reticle_3d: the gun dot published by the HudCrosshair update (game thread), seqlock-read on the render thread.
// local = the dot relative to the RENDERED view in its right/up/back frame (metres); gameOff = the same offset in game
// world axes (for the yaw-mapping cross-check). group/dotVisible = what the game's own crosshair shows (INV-281); s0/s1 = the
// SHOT's spread H / V (deg) - Left/Right parts open by s0, Up/Down by s1;
// fov = the look camera's FOV (deg, HUD design px -> degrees); hitAgeMs = ms since the hit counter changed (0xFFFFFFFF never).
// False when never published or older than 150 ms (no gun out).
struct GunDot {
    float    local[3] = {}, gameOff[3] = {};
    float    dist = 0.0f;
    bool     hit = false;
    unsigned ageMs = 0;
    int      group = 0;
    bool     dotVisible = true;
    float    s0 = 0.0f, s1 = 0.0f, fov = 0.0f;
    unsigned hitAgeMs = 0xFFFFFFFFu;
    // bow_reticle_3d (Jay 2026-10-08, option 2): where the arrow SHAFT's straight line points (its hit / 50 m pin), in the same
    // rendered-view frame as `local`; drawn as a small dot beside the brackets, which sit at the landing point. Bow only.
    float    shaft[3] = {};
    bool     shaftOk = false;
};
bool GetGunDot(GunDot& out);
// The bow controller's EBowControllerState (1 drawing, 3 drawn - INV-496). False = no active bow controller.
bool ReadBowCtrlState(int* state);
// The ACTIVE bow controller's address, for the bow string probe (boneblend, game thread). False = no owner / no controller / not
// active / unreadable. Four pointer reads, the same walk as ReadBowCtrlState.
bool ReadBowCtrlPtr(uintptr_t* ctrl);
// grapple_reticle_3d: the grapple target as the game's HudRopeReticle decided it this update (game thread), seqlock-read on the
// render thread. local = the target in the RENDERED view's right/up/back frame (m); state 1 = can grapple (G_Valid), 2 = the
// stored hit is refused (ctrl+193). False when nothing to show (no target / reticle hidden) or older than 150 ms.
struct GrappleDot {
    float    local[3] = {};
    float    dist = 0.0f;
    int      state = 0;
    unsigned ageMs = 0;
    float    fov = 0.0f;   // the look camera's FOV (deg) - design px -> degrees for the icon size
};
bool GetGrappleDot(GrappleDot& out);
extern bool FireReticleBulletCast;   // fire_reticle_bullet_cast - the 3D reticle's ray = the bullet's own cast. ON.
extern bool BowReticle3d;            // bow_reticle_3d            - the bow's 3D reticle on the arrow's arc, while drawn. ON.
extern float FireReticleScale;       // fire_reticle_scale        - 3D crosshair part size multiplier, 0..1. 0.4

}  // namespace dlvr
