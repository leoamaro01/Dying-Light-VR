// Standalone unit test for src/ovr2xr/xhair_hide_logic.h - NOT part of the DLL build.
// hide_game_crosshair (2026-11-28): what the HudCrosshair hook does with the game's crosshair when no VR use of it is active.
// Exit code = number of failures.
#include "xhair_hide_logic.h"

#include <cstdio>

using namespace dlvr::xhairhide;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

int main()
{
    // Hidden: parked on every idle update, whether or not it is already off its spot (the game may move it back).
    CHECK(DecideIdle(true, false) == IdleAction::Park);
    CHECK(DecideIdle(true, true) == IdleAction::Park);
    // Not hidden: the pre-setting behaviour - put back once if it was moved, else leave alone.
    CHECK(DecideIdle(false, true) == IdleAction::Restore);
    CHECK(DecideIdle(false, false) == IdleAction::None);
    // Turning the setting off live: the first idle update after it restores the parked group, the next one does nothing.
    {
        bool moved = true;   // parked while hide was on
        IdleAction a = DecideIdle(false, moved);
        CHECK(a == IdleAction::Restore);
        moved = false;
        a = DecideIdle(false, moved);
        CHECK(a == IdleAction::None);
    }

    // The hook runs if any of its three users is on, and only then.
    CHECK(!HookActive(false, false, false));
    CHECK(HookActive(true, false, false));
    CHECK(HookActive(false, true, false));
    CHECK(HookActive(false, false, true));
    CHECK(HookActive(true, true, true));

    std::printf("xhair_hide_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
