// hide_game_crosshair (2026-11-28, tester: the game's centre crosshair "doesn't really do anything at all in VR, it's just
// bothersome in the middle of the screen"). Pure decision for grappleaim.cpp's HudCrosshair update hook (CrosshairHook): what to
// do with the game's own crosshair group when NONE of the VR uses of it is active this update - i.e. on every path that used
// to put it back at its layout spot (screen centre). The VR uses keep priority and are decided before this is asked:
//   - fire_reticle_3d / bow_reticle_3d already PARK it while their 3D reticle is up (gun held / bow held);
//   - grapple_crosshair_marker MOVES it onto the hand ray's hit while the grapple aims (it follows the hand - kept);
//   - the grapple's own "can grapple" icon is a different HUD element (HudRopeReticle) and is never touched here.
// No OpenXR, no Windows API, no logging: tests/xhair_hide_logic_test.cpp.
#pragma once

namespace dlvr {
namespace xhairhide {

enum class IdleAction {
    None,      // already where it should be: nothing to write
    Park,      // move the group far off-screen (the same parking the 3D reticles use; no visibility-state change)
    Restore,   // put the group back at its captured layout spot (screen centre)
};

// hide  = the hide_game_crosshair setting, read live each update.
// moved = the hook's own record that the group is currently NOT at its layout spot (parked or moved onto a grapple hit).
// Parking is written every update while hidden (the game may rewrite the group's position; the 3D reticles do the same).
inline IdleAction DecideIdle(bool hide, bool moved)
{
    if (hide) {
        return IdleAction::Park;
    }
    return moved ? IdleAction::Restore : IdleAction::None;
}

// Whether the HudCrosshair hook has anything to do at all this update. False = return the game's result untouched - except
// that a group left parked/moved by a setting turned off live must still be put back once (DecideIdle(false, true)).
inline bool HookActive(bool grappleMarker, bool fireReticle3d, bool hide)
{
    return grappleMarker || fireReticle3d || hide;
}

}  // namespace xhairhide
}  // namespace dlvr
