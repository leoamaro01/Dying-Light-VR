cmake_minimum_required(VERSION 3.21)

# C: for the vendored puff.c (calibration screen build D - the native font's inflate). Without it the .c in the source
# list below is not compiled.
project(dlvr_ovr2xr LANGUAGES C CXX ASM_MASM)

# ---------------------------------------------------------------------------
# BUILD GATE - the classes of fault this project has actually paid for, turned
# into ERRORS so they cannot be written, missed in output, or promised away.
#
# C4129 cost a headset run on 2026-09-13. Source was appended with a bash
# heredoc (which this project's own rules forbid, for exactly this reason), the
# doubled backslashes were eaten, and "%s\pak_lock_test.txt" silently became
# "%spak_lock_test.txt" - so a probe opened a path that does not exist, returned
# without logging, and the run answered nothing. THE COMPILER DIAGNOSED IT AT
# THE TIME. It was a warning, and the build output was being filtered for
# "error" and "Deploying", so the diagnosis was thrown away before it was read.
#
# The lesson is not "be careful with heredocs" - it is that a WARNING nobody
# reads is indistinguishable from no warning at all. These are errors now.
#
# The format-string set is here for the same reason: this project's failures are
# overwhelmingly SILENT WRONG LOG LINES ("a verdict string compared against
# zero", "a log line hardcoded about its own subject"), and a printf argument
# that does not match its specifier produces exactly that - a confident,
# readable, wrong number.
if (MSVC)
    # SCOPED TO C++ ONLY. Unscoped, these reach the MASM assembler, which
    # rejects each one as an invalid command-line option - and a build gate that
    # emits its own warnings is a gate people learn to ignore.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/we4129>"   # unrecognized escape <- cost a run
        "$<$<COMPILE_LANGUAGE:CXX>:/we4473>"   # too few printf arguments
        "$<$<COMPILE_LANGUAGE:CXX>:/we4474>"   # too many printf arguments
        "$<$<COMPILE_LANGUAGE:CXX>:/we4476>"   # unknown format specifier
        "$<$<COMPILE_LANGUAGE:CXX>:/we4477>"   # format does not match argument
        "$<$<COMPILE_LANGUAGE:CXX>:/we4313>"   # specifier conflicts with argument
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED DLVR_GAME_DIR)
    set(DLVR_GAME_DIR "D:/SteamLibrary/steamapps/common/Dying Light" CACHE PATH "Game install to deploy built DLLs into")
endif()

find_package(OpenXR CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

set(SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(OVR_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OVR/Include")

# ---------------------------------------------------------------- xinput1_3 ---
# The bootstrap proxy: loaded first (engine_x64_rwdi.dll statically imports it),
# forwards real XInput, and installs the engine-side hooks/gates.
add_library(xinput1_3 SHARED
    ${SRC_ROOT}/bootstrap/dllmain.cpp
    ${SRC_ROOT}/bootstrap/dlssflat.cpp
    ${SRC_ROOT}/bootstrap/shaderbin.cpp
    ${SRC_ROOT}/bootstrap/pakgen.cpp
    # FSR 3.1 / 4 through a D3D11<->D3D12 interop (docs/PROPOSAL-FSR-INTEROP-2026-09-26.md).
    # d3d12.dll and the FFX loader are LoadLibrary'd, never linked, so nothing
    # is added to target_link_libraries for it.
    ${SRC_ROOT}/bootstrap/fsrinterop.cpp
    # (ovr2xr/watchwrite.cpp is no longer compiled into the bootstrap - its only user, the flat-mode jitter watch, was
    # removed 2026-11-15, removal batch 6. The layer's copy is the only one.)
    # A tester's start-up hang watch (2026-10-21). Empty unless DLVR_HANG_WATCH is ON (below).
    ${SRC_ROOT}/bootstrap/hangwatch.cpp
    ${SRC_ROOT}/bootstrap/xinput1_3.def
)
# A tester's start-up hang (BETA-FINDINGS-2026-10-19 section 14): a SEPARATE build folder configured with
# -DDLVR_HANG_WATCH=ON gets the watchdog; the normal BETA build (OFF, the default) compiles hangwatch.cpp to two empty functions.
option(DLVR_HANG_WATCH "Diagnostic build only: the start-up hang watch in the bootstrap" OFF)
if(DLVR_HANG_WATCH)
    target_compile_definitions(xinput1_3 PRIVATE DLVR_HANG_WATCH=1)
endif()
# The [CTXSITE] fault test (trace docs/traces/20261121_1036_ctxsite_detours.md, Jay 2026-11-21: "marker in the probe build only"):
# src_beta/build_probe configured -DDLVR_CTXSITE_FAULT_TEST=ON compiles ctxsite::FaultTest in - with dlvr\ctxsite_fault_test.txt the
# bootstrap writes the context's vtable slots 33 / 8 / 44 back to the originals once at device creation (a tester's lost-hook state).
# The normal BETA build (OFF, the default) does not contain it.
option(DLVR_CTXSITE_FAULT_TEST "Diagnostic build only: the [CTXSITE] lost-vtable-hook fault test in the bootstrap" OFF)
if(DLVR_CTXSITE_FAULT_TEST)
    target_compile_definitions(xinput1_3 PRIVATE DLVR_CTXSITE_FAULT_TEST=1)
endif()
# The NVIDIA DLSS SDK, for dlssflat's milestone C evaluation.
set(DLVR_NGX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../DLSS_SDK" CACHE PATH "NVIDIA DLSS SDK root")
# The AMD FidelityFX SDK 2.3.0 (FSR 3.1.5 + FSR 4.1.1), headers only at build
# time; the signed DLLs are deployed beside xinput1_3.dll below.
set(DLVR_FFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../FSR_SDK/v2.3.0/Kits/FidelityFX" CACHE PATH "AMD FidelityFX SDK root")
target_include_directories(xinput1_3 PRIVATE ${SRC_ROOT}/common "${DLVR_NGX_SDK}/include"
                           "${DLVR_NGX_SDK}/NVIDIAImageScaling/NIS"
                           "${DLVR_FFX_SDK}/api/include" "${DLVR_FFX_SDK}/upscalers/include")
target_compile_definitions(xinput1_3 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
# shell32 for SHGetKnownFolderPath, ole32 for CoTaskMemFree - both used by
# EnsureVideoScrHasOculusEnabled() to resolve the (possibly OneDrive-
# redirected) Documents folder. dxgi for the public build's GPU-adapter
# launch logging (LogSystemInfo) - harmless to link unconditionally.
# d3d11 for dlssflat: the flat DLSS prototype lives here because
# LibOVRRT64_1.dll is NOT loaded in flatscreen, so the layer cannot host it.
# nvsdk_ngx_d.lib is the DYNAMIC-CRT variant, which is what MSVC's default /MD
# matches. advapi32 and user32 are NOT optional: the static lib calls
# RegOpenKeyExW/RegCloseKey/RegQueryValueExW and GetWindowThreadProcessId and
# does not pull them in itself - milestone A's build paid for that discovery.
target_link_libraries(xinput1_3 PRIVATE shell32 ole32 dxgi d3d11 d3dcompiler advapi32 user32
    "${DLVR_NGX_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
set_target_properties(xinput1_3 PROPERTIES
    RUNTIME_OUTPUT_NAME "xinput1_3"
    PREFIX ""
)

# ------------------------------------------------------------- LibOVRRT64_1 ---
# The real payload: implements the LibOVR 1.10 CAPI on top of OpenXR.
add_library(LibOVRRT64_1 SHARED
    ${SRC_ROOT}/ovr2xr/ovr2xr.cpp
    ${SRC_ROOT}/ovr2xr/xrloader.cpp
    ${SRC_ROOT}/ovr2xr/camhook.cpp
    ${SRC_ROOT}/ovr2xr/camhook_thunk.asm
    # The melee element hook's thunk. Same reason the camera one exists: a
    # compiled function cannot be grafted into someone else's register
    # allocation safely. See melee_thunk.asm for why it tail-jumps.
    ${SRC_ROOT}/ovr2xr/melee_thunk.asm
    # R5 step 4: the melee POSE observation hook's thunk (gamedll+0xDC1C90).
    # Tail-jumps for exactly the reason melee_thunk.asm documents - the stolen
    # prologue spills to the CALLER's shadow space through RSP.
    ${SRC_ROOT}/ovr2xr/melee_pose_thunk.asm
    # (item_pose_thunk.asm - the ITEM POSE fix's thunk - removed with that fix, removal batch 10, 2026-11-15.)
    ${SRC_ROOT}/ovr2xr/aimhook.cpp
    ${SRC_ROOT}/ovr2xr/coopnet.cpp
    ${SRC_ROOT}/ovr2xr/cooppose.cpp
    ${SRC_ROOT}/ovr2xr/grappleaim.cpp
    ${SRC_ROOT}/ovr2xr/controllers.cpp
    # The body-tracked elbow (rig_elbow_body_tracking, 2026-11-08): XR_FB_body_tracking when the runtime offers it.
    ${SRC_ROOT}/ovr2xr/bodytrack.cpp
    ${SRC_ROOT}/ovr2xr/holster.cpp
    ${SRC_ROOT}/ovr2xr/swing.cpp
    ${SRC_ROOT}/ovr2xr/meleethrow.cpp
    # Throwing by hand, Build A (docs/superpowers/plans/2026-10-14-throwables-build-a.md): the pad filter, the hand
    # history and the event probe. The decision is throwable_logic.h (tests/throwable_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/throwable.cpp
    # Loot by hand (docs/superpowers/plans/2026-10-23-loot-by-hand.md): the GetNearbyTriggers detour, the pad bytes and the
    # event probe. The decision is looting_logic.h (tests/looting_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/looting.cpp
    # Hand-open lids (docs/superpowers/plans/2026-11-18-hand-open-lids-phase2.md): lid bone from the hand, the game's own open /
    # take, items on the hand. The decision is lidhand_logic.h (tests/lidhand_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/lidhand.cpp
    # The in-world prompt (trace docs/traces/20261123_1356_inworld_prompt_quad.md): one quad carrying the game's own prompt image.
    ${SRC_ROOT}/ovr2xr/promptquad.cpp
    # Grab bodies by hand (docs/superpowers/plans/2026-10-23-body-grab.md): the ragdoll pull, driven from the player-update tail.
    # The decisions are bodygrab_logic.h + looting_logic.h's DRAG state (tests/bodygrab_logic_test.cpp, looting_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/bodygrab.cpp
    # The camouflage gesture (plan 2026-11-07-camouflage-gesture): wipe a held infected corpse / limb across the face -> the game's
    # camouflage. The rules are camo_logic.h (tests/camo_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/camogesture.cpp
    # Roomscale (docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 7): 7a = the log-only walk-body / map probe,
    # driven from the player-update tail. The decisions are roomscale_logic.h (tests/roomscale_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/roomscale.cpp
    ${SRC_ROOT}/ovr2xr/physcrouch.cpp
    ${SRC_ROOT}/ovr2xr/breakout.cpp
    ${SRC_ROOT}/ovr2xr/handcoll.cpp
    # Solid guns (docs/superpowers/plans/2026-11-02-solid-guns-reload-sound-haptics.md Task 4): the held firearm is solid to the
    # left hand. The decisions are gunshape_logic.h (tests/gunshape_logic_test.cpp); the shapes gun_shapes_table.inc (tools/gun_shapes.py).
    ${SRC_ROOT}/ovr2xr/gunshape.cpp
    # Solid guns Task 5: the game's own reload sounds at the player's steps (engine IGSObject::PlaySound2D, research step 1).
    ${SRC_ROOT}/ovr2xr/gunsound.cpp
    ${SRC_ROOT}/ovr2xr/pushgrab.cpp
    # Map fixes, ported from DEV 2026-11-07 (load-time only, data from dlvr\*.bin shipped in the package): road drape (INV-692),
    # edging strip + terrain berm (INV-694), decal seat (INV-696) + DecalPLS (INV-697). DEV's roaddrop.cpp (dropped trial) stays out.
    ${SRC_ROOT}/ovr2xr/roaddrape.cpp
    ${SRC_ROOT}/ovr2xr/edgestrip.cpp
    ${SRC_ROOT}/ovr2xr/decalseat.cpp
    ${SRC_ROOT}/ovr2xr/decalpls.cpp
    # Manual reload (docs/superpowers/plans/2026-11-01-manual-reload-pistol.md Task 4): the pistol's parts on the player-update tail.
    # The decisions are reload_logic.h (tests/reload_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/reload.cpp
    # Pump shotgun manual reload Task 3: the detour that drops the game's per-shot rack sound (pumpsound_logic.h = the skip rule).
    ${SRC_ROOT}/ovr2xr/pumpsound.cpp
    # Crossbow bolt wheel (plan 2026-11-04-crossbow-bolt-wheel Task 2): the input lookup detour + the bolt wheel's Update slot
    # (boltwheel_logic.h = the rules, tests/boltwheel_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/boltwheel.cpp
    # Inventory keep-built (INV-526 addendum 2026-11-14; trace docs/traces/20261114_1415_inventory_keep_built.md): a fix, no key.
    ${SRC_ROOT}/ovr2xr/invkeep.cpp
    # buggy hand steer + key (plan 2026-11-10-buggy-wheel-and-key; buggywheel_logic.h = the rules, tests/buggywheel_logic_test.cpp)
    ${SRC_ROOT}/ovr2xr/buggywheel.cpp
    # Lockpick position P1 (spec 2026-10-25-lockpick-position-p1-design): the lockpick look asks for the head's pitch.
    ${SRC_ROOT}/ovr2xr/lockpitch.cpp
    # The lower body turns with the stick when looking down (INV-749, trace 20261119_1530): PlayerFppVis slot 270 swap.
    ${SRC_ROOT}/ovr2xr/pelvisturn.cpp
    # Dialogue view offset (spec 2026-10-25-dialogue-view-offset-design, INV-619): DialogController's look position gets the
    # cam_eye_* offset the camera base gets on foot.
    ${SRC_ROOT}/ovr2xr/dlgview.cpp
    # Cutscene camera (spec 2026-10-25-cutscene-camera-design, INV-620): (a) the cutscene's wanted look stops feeding design D's head
    # yaw back into the body; (b) no side bars in VR cutscenes.
    ${SRC_ROOT}/ovr2xr/cinlook.cpp
    ${SRC_ROOT}/ovr2xr/cinbars.cpp
    # The bow's manual nock (docs/superpowers/plans/2026-10-17-bow-manual-nock.md): the +D9BF60 count detour and the tick
    # BowDrawTick calls. The decision is nock_logic.h (tests/nock_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/bownock.cpp
    # A game model carried on a player element (docs/superpowers/plans/2026-10-22-hand-carried-model.md): the bow's carried arrow.
    # The decision is handcarry_logic.h (tests/handcarry_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/handcarry.cpp
    ${SRC_ROOT}/ovr2xr/twohand.cpp
    ${SRC_ROOT}/ovr2xr/armfit.cpp
    # The calibration screen, build A (docs/superpowers/plans/2026-10-14-calibration-screen-build-a.md): the glue and the
    # panel layout. The logic is calibscreen_logic.h (tests/calibscreen_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/calibscreen.cpp
    ${SRC_ROOT}/ovr2xr/calibdraw.cpp
    # Build D (docs/superpowers/plans/2026-10-14-calibration-screen-build-d.md task 4): the one-shot loader thread for
    # Jay's PNGs (WIC) and the game's own font, and the render-thread textures. The logic is calibassets_logic.h +
    # calibnative_logic.h (tests/calibassets_logic_test.cpp, tests/calibnative_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/calibassets.cpp
    # Build D task 6: the game's own menu sounds through the engine export gui::IObject::PlaySound2D (route A,
    # docs/RESEARCH-CALIB-AUDIO-2026-10-14.md), behind byte checks of engine e9fbfc2c.
    ${SRC_ROOT}/ovr2xr/calibsound.cpp
    # puff: zlib contrib/puff v1.3.1, decode-only deflate, vendored UNCHANGED, zlib licence (notice in puff.h).
    ${SRC_ROOT}/third_party/puff/puff.c
    ${SRC_ROOT}/ovr2xr/rtticam.cpp
    ${SRC_ROOT}/ovr2xr/watchwrite.cpp
    ${SRC_ROOT}/ovr2xr/boneblend.cpp
    ${SRC_ROOT}/ovr2xr/overlay.cpp
    # Desktop mirror, one eye (2026-11-14, trace docs/traces/20261114_1030_desktop_mirror_single_eye.md).
    ${SRC_ROOT}/ovr2xr/deskmirror.cpp
    ${SRC_ROOT}/ovr2xr/wristprobe.cpp
    # Wrist HUD stage 2 (docs/superpowers/specs/2026-11-12-wrist-hud-design.md): marker dlvr\wrist_hud.txt - the five HUD pieces
    # the bootstrap's router publishes, on quads in the grip spaces (preset A, fixed facing fade).
    ${SRC_ROOT}/ovr2xr/wristquad.cpp
    # Menu pin, stage 1 (SPEC-UI-MENU-PIN-2026-10-11): ui_pin_menus - the main menu and the pause menu are drawn from
    # the scene's camera, through one detour on engine +2E6BF0. The maths is uipin_logic.h (tests/uipin_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/uipin.cpp
    # Menu panel, stage 1 (SPEC-UI-MENU-PANEL-2026-10-11): ui_panel_menus - while a menu shows, the bracketed sprite pass
    # goes on a room-fixed OpenXR quad. Maths: uipanel_logic.h (tests/uipanel_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/uipanel.cpp
    # Loading-screen void (docs/superpowers/specs/2026-10-20-loading-screen-void-design.md): no setting - while the engine
    # draws its background path, a black void + the picture on a room-fixed quad pair. Decisions: loadscreen_logic.h
    # (tests/loadscreen_logic_test.cpp).
    ${SRC_ROOT}/ovr2xr/loadscreen.cpp
    ${SRC_ROOT}/probe/LibOVRRT64_1.def
)
target_include_directories(LibOVRRT64_1 PRIVATE
    ${SRC_ROOT}/common
    ${SRC_ROOT}/ovr2xr
    ${OVR_INCLUDE}
)
target_compile_definitions(LibOVRRT64_1 PRIVATE
    OVR_DLL_BUILD
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
# The swing probe (trace docs/traces/20261119_1730_swing_probe.md, Jay 2026-11-19: "Setup the probe then. We need everything from
# this run"): a SEPARATE build folder (src_beta/build_probe, tools/deploy_variant.py probe) configured with -DDLVR_SWING_PROBE=ON
# compiles diag_swing_shadow ([SWINGSHADOW] / [SWINGMISS] / [SWINGAI]) back into the layer; the normal BETA build (OFF) is unchanged.
option(DLVR_SWING_PROBE "Diagnostic build only: the melee swing shadow / miss / AI-list probe in the layer" OFF)
if(DLVR_SWING_PROBE)
    target_compile_definitions(LibOVRRT64_1 PRIVATE DLVR_SWING_PROBE=1)
endif()
# The yaw-step probe (trace docs/traces/20261119_2145_yawstep_probe.md, RUN WK1 - the wall-kick spin; Jay: "Beta probe."): the same
# build_probe folder, configured -DDLVR_SWING_PROBE=OFF -DDLVR_YAW_PROBE=ON, compiles diag_cam_yaw ([YAWSTEP] / [CAMYAW]) back in.
option(DLVR_YAW_PROBE "Diagnostic build only: the [YAWSTEP] / [CAMYAW] yaw probe (diag_cam_yaw) in the layer" OFF)
if(DLVR_YAW_PROBE)
    target_compile_definitions(LibOVRRT64_1 PRIVATE DLVR_YAW_PROBE=1)
endif()
# BETA LEAN (spec docs/superpowers/specs/2026-10-18-beta-leak-hardening-design.md section 6.1): compiles the periodic logs,
# help text and arm capture out. BOTH DLLs; only this tree's CMakeLists has it. (Log lines are plain text since 2026-11-14.)
if(DLVR_BETA_BUILD)
    target_compile_definitions(xinput1_3 PRIVATE DLVR_BETA_LEAN=1)
    target_compile_definitions(LibOVRRT64_1 PRIVATE DLVR_BETA_LEAN=1)
    # Symbols for reading a tester's crash dump (spec 6.6): /Zi + /DEBUG writes build/Release/<target>.pdb - archived by
    # make_setup_pkg.py --tree beta OUTSIDE the package, never shipped. /OPT:REF + /OPT:ICF are stated because /DEBUG
    # would otherwise turn Release's defaults off. /PDBALTPATH embeds only the .pdb's NAME: the default embeds its full
    # path (the build folder's full path) in the DLL.
    foreach(tgt xinput1_3 LibOVRRT64_1)
        target_compile_options(${tgt} PRIVATE "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Zi>")
        target_link_options(${tgt} PRIVATE $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF>
                            $<$<CONFIG:Release>:/OPT:ICF> $<$<CONFIG:Release>:/PDBALTPATH:${tgt}.pdb>)
    endforeach()
endif()
target_link_libraries(LibOVRRT64_1 PRIVATE
    OpenXR::openxr_loader
    imgui::imgui
    d3d11
    dxgi
    # calibassets.cpp: WIC (CLSID_WICImagingFactory, the PNG decode) and COM (CoInitializeEx / CoCreateInstance).
    windowscodecs
    ole32
    # calibassets.cpp: Media Foundation (the floor video, dlvr\calib\floor_grid_1.mp4): MFStartup / MFCreateAttributes /
    # MFCreateMediaType (mfplat), MFCreateSourceReaderFromURL (mfreadwrite), the MF GUIDs (mfuuid).
    mfplat
    mfreadwrite
    mfuuid
)
set_target_properties(LibOVRRT64_1 PROPERTIES
    RUNTIME_OUTPUT_NAME "LibOVRRT64_1"
    PREFIX ""
)

# ------------------------------------------------------- DyingLightVR_Setup ---
# The setup program (docs/superpowers/specs/2026-10-17-installer-design.md, plan docs/superpowers/plans/2026-10-17-
# installer.md). Every decision is in src/setup/setup_*.h (unit tests src/tests/setup_*_test.cpp); setup_main.cpp only
# finds, asks and executes. NOT deployed (no POST_BUILD) - tools/make_setup_pkg.py packages it. Static CRT: it must run
# before the VC++ runtime is installed, because it is what installs it. Build it alone with
#   cmake --build build --config Release --target DyingLightVR_Setup
# REVISION 2 (spec section 8): every file the mod needs is compiled INTO the exe (Jay: "baked into the installer
# itself"), from a resource script tools/make_setup_pkg.py generates - so the target exists only when that script is
# given (-DDLVR_SETUP_PAYLOAD_RC=...), is EXCLUDE_FROM_ALL (the mod's normal build never relinks the ~86 MB exe), and
# the packager runs its build. The window is Dear ImGui 1.92.8 vendored in third_party/imgui_setup (static CRT - the
# layer's vcpkg imgui is /MD).
if(DEFINED DLVR_SETUP_PAYLOAD_RC AND EXISTS "${DLVR_SETUP_PAYLOAD_RC}")
    set(IMGUI_SETUP ${SRC_ROOT}/third_party/imgui_setup)
    add_executable(DyingLightVR_Setup WIN32 EXCLUDE_FROM_ALL
        ${SRC_ROOT}/setup/setup_main.cpp
        ${SRC_ROOT}/setup/setup_sys.cpp
        ${SRC_ROOT}/setup/setup_payload.cpp
        ${SRC_ROOT}/setup/setup_ui.cpp
        ${SRC_ROOT}/setup/setup.manifest
        ${SRC_ROOT}/third_party/puff/puff.c
        ${IMGUI_SETUP}/imgui.cpp
        ${IMGUI_SETUP}/imgui_draw.cpp
        ${IMGUI_SETUP}/imgui_tables.cpp
        ${IMGUI_SETUP}/imgui_widgets.cpp
        ${IMGUI_SETUP}/backends/imgui_impl_dx11.cpp
        ${IMGUI_SETUP}/backends/imgui_impl_win32.cpp
        ${DLVR_SETUP_PAYLOAD_RC}
    )
    target_include_directories(DyingLightVR_Setup PRIVATE ${SRC_ROOT}/setup ${SRC_ROOT}/third_party/puff ${IMGUI_SETUP}
                               ${IMGUI_SETUP}/backends)
    target_compile_definitions(DyingLightVR_Setup PRIVATE UNICODE _UNICODE)
    target_link_libraries(DyingLightVR_Setup PRIVATE d3d11 d3dcompiler dxgi windowscodecs bcrypt shlwapi comctl32 advapi32
                          shell32 ole32 version)
    set_target_properties(DyingLightVR_Setup PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    message(STATUS "DyingLightVR_Setup not configured: DLVR_SETUP_PAYLOAD_RC is not set or missing ('${DLVR_SETUP_PAYLOAD_RC}') - "
                   "tools/make_setup_pkg.py sets it and builds the setup")
endif()

# ---------------------------------------------------- DyingLightVR_Launcher ---
# The launcher (docs/superpowers/specs/2026-11-15-launcher-design.md, plan docs/superpowers/plans/2026-11-15-launcher.md),
# which supersedes the setup program. Decisions in src/launcher/launcher_*.h (tests src/tests/launcher_*_test.cpp, the
# setup's headers reused by include). NOT deployed (no POST_BUILD) and EXCLUDE_FROM_ALL, so the mod's normal build never
# builds it. Static CRT, like the setup: it runs before the VC++ runtime is installed. Build it alone with
#   cmake --build build --config Release --target DyingLightVR_Launcher
set(IMGUI_LAUNCHER ${SRC_ROOT}/third_party/imgui_setup)
add_executable(DyingLightVR_Launcher WIN32 EXCLUDE_FROM_ALL
    ${SRC_ROOT}/launcher/launcher_main.cpp
    ${SRC_ROOT}/launcher/launcher_ui.cpp
    ${SRC_ROOT}/launcher/launcher_sys.cpp
    ${SRC_ROOT}/launcher/launcher_app.cpp
    ${SRC_ROOT}/launcher/launcher_net.cpp
    ${SRC_ROOT}/setup/setup_sys.cpp
    ${SRC_ROOT}/setup/setup_payload.cpp
    ${SRC_ROOT}/third_party/puff/puff.c
    ${SRC_ROOT}/launcher/launcher.manifest
    ${IMGUI_LAUNCHER}/imgui.cpp
    ${IMGUI_LAUNCHER}/imgui_draw.cpp
    ${IMGUI_LAUNCHER}/imgui_tables.cpp
    ${IMGUI_LAUNCHER}/imgui_widgets.cpp
    ${IMGUI_LAUNCHER}/backends/imgui_impl_dx11.cpp
    ${IMGUI_LAUNCHER}/backends/imgui_impl_win32.cpp
)
target_include_directories(DyingLightVR_Launcher PRIVATE ${SRC_ROOT}/launcher ${SRC_ROOT}/setup ${SRC_ROOT}/bootstrap
                           ${SRC_ROOT}/third_party/puff ${IMGUI_LAUNCHER} ${IMGUI_LAUNCHER}/backends)
target_compile_definitions(DyingLightVR_Launcher PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(DyingLightVR_Launcher PRIVATE d3d11 dxgi windowscodecs shcore shell32 ole32 winhttp bcrypt advapi32
                      version shlwapi comctl32)
set_target_properties(DyingLightVR_Launcher PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
# Jay's art, built INTO the exe (Jay 2026-11-15: "In the EXE"). It lives OUTSIDE src/ (src_beta/launcher_art/), so the public
# repo never carries it; a checkout without the folder builds a launcher with the code-drawn look (logged at start).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/launcher_art/launcher_art.rc")
    target_sources(DyingLightVR_Launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/launcher_art/launcher_art.rc")
    message(STATUS "DyingLightVR_Launcher: launcher_art/launcher_art.rc found - the art is embedded")
else()
    message(STATUS "DyingLightVR_Launcher: NO launcher_art/launcher_art.rc - the launcher builds with the code-drawn look")
endif()
# /W4 /WX on our own launcher sources only (plan Global Constraints) - the vendored ImGui stays at the default level.
set_source_files_properties(${SRC_ROOT}/launcher/launcher_main.cpp ${SRC_ROOT}/launcher/launcher_ui.cpp
                            ${SRC_ROOT}/launcher/launcher_sys.cpp ${SRC_ROOT}/launcher/launcher_app.cpp
                            ${SRC_ROOT}/launcher/launcher_net.cpp TARGET_DIRECTORY DyingLightVR_Launcher PROPERTIES COMPILE_OPTIONS "/W4;/WX")

# ------------------------------------------------------------------- deploy ---
# Public builds are deliberately NOT auto-deployed into DLVR_GAME_DIR - that
# path is the dev's own live test install, and silently dropping a
# menu-less, feature-stripped build into it would strand the next dev
# testing session with no way to open the overlay. Public build output stays
# in build/Release/ for a deliberate, manual zip-and-send step instead (see
# CLAUDE.md's public-build section).
# THE BETA TREE (src_beta/, cloned from src/ 2026-10-18 - Jay: "We need to make a clone of the source for this"). Its
# builds are NEVER auto-deployed: DLVR_GAME_DIR is Jay's DEV install. tools/deploy_variant.py installs BETA or DEV on
# demand. Only this copy of CMakeLists has the option; src/ deploys as before.
option(DLVR_BETA_BUILD "The BETA tree: build into build/Release only, never deploy" ON)
# Fork: deploying is its own switch, so the lean BETA build can be copied into a local game folder without also turning
# the DEV flavour on. Default keeps upstream's behaviour (deploy only when not BETA). Set it with DLVR_GAME_DIR, e.g. in
# CMakeUserPresets.json (not committed).
if(DLVR_BETA_BUILD)
    set(_dlvr_deploy_default OFF)
else()
    set(_dlvr_deploy_default ON)
endif()
option(DLVR_DEPLOY "POST_BUILD: copy the two mod DLLs and the FFX DLLs into DLVR_GAME_DIR" ${_dlvr_deploy_default})
if(DLVR_DEPLOY)
    if(NOT EXISTS "${DLVR_GAME_DIR}/engine_x64_rwdi.dll")
        message(FATAL_ERROR "DLVR_DEPLOY is ON but DLVR_GAME_DIR ('${DLVR_GAME_DIR}') has no engine_x64_rwdi.dll - "
                            "point it at the Dying Light install folder")
    endif()
    message(STATUS "DLVR: POST_BUILD deploy is ON -> ${DLVR_GAME_DIR}")
else()
    message(STATUS "DLVR: POST_BUILD deploy is OFF - output stays in build/<config>")
endif()
if(DLVR_DEPLOY)
    foreach(tgt xinput1_3 LibOVRRT64_1)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${tgt}>" "${DLVR_GAME_DIR}/$<TARGET_FILE_NAME:${tgt}>"
            COMMENT "Deploying ${tgt} to ${DLVR_GAME_DIR}"
        )
    endforeach()
    # The FFX loader + upscaler go beside xinput1_3.dll - fsrinterop.cpp loads
    # them by FULL PATH from its own directory. The licence travels with them:
    # redistribution is permitted in binary form with the notice reproduced
    # (docs/license.md lines 1-35 / 885-894, REF-FSR-SDK). copy_if_different, so
    # an unchanged 28 MB DLL is not rewritten on every build.
    add_custom_command(TARGET xinput1_3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLVR_FFX_SDK}/signedbin/amd_fidelityfx_loader_dx12.dll"
            "${DLVR_FFX_SDK}/signedbin/amd_fidelityfx_upscaler_dx12.dll"
            "${DLVR_GAME_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLVR_FFX_SDK}/docs/license.md" "${DLVR_GAME_DIR}/amd_fidelityfx_license.md"
        COMMENT "Deploying the AMD FidelityFX DLLs + licence to ${DLVR_GAME_DIR}"
    )
endif()
