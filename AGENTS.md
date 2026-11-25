# AGENTS.md

Guidance for coding agents (and humans) working in this repository.

## What this is

**Dying Light VR**: a native 6DoF OpenXR VR mod for Dying Light (Enhanced Edition, Steam / Epic). It is C++17 for Windows
x64 only, built with MSVC. It does **not** use a helper program. Instead it ships DLLs that the game loads itself:

| Artifact | Source | Role |
|---|---|---|
| `xinput1_3.dll` (the **bootstrap**) | `src/bootstrap/` | `engine_x64_rwdi.dll` imports it statically, so it loads before the game's entry point. It forwards real XInput to System32, rewrites `GetCommandLineA` to inject `/video=<our .scr>` (turning the engine's hidden Oculus path on), and installs engine-side hooks and gates. It also hosts the flatscreen DLSS/FSR/NIS path (`dlssflat.cpp`, `fsrinterop.cpp`), the pak generator (`pakgen.cpp`) and shader binaries, because the layer DLL is not loaded in flatscreen. |
| `LibOVRRT64_1.dll` (the **layer**) | `src/ovr2xr/` | Implements the **LibOVR 1.10 C API on top of OpenXR** (`ovr2xr.cpp`). It also holds every gameplay/VR feature: camera, arms, melee, reload, holsters, UI panels, overlay menu and so on. Exports are listed in `src/probe/LibOVRRT64_1.def`. |
| `DyingLightVR_Launcher.exe` | `src/launcher/` (+ reuses `src/setup/`) | Installs, updates, removes and rolls back the mod, switches between VR and flat, edits settings and collects logs. It uses Dear ImGui (vendored) with a static CRT. Updates come from GitHub releases (`launcher_update.h` `kRepoUrl`, which points at **upstream** `bymodify-ux/Dying-Light-VR`). |
| `DyingLightVR_Setup.exe` | `src/setup/` | The older installer, superseded by the launcher. It is only configured when `DLVR_SETUP_PAYLOAD_RC` is given. Its `setup_*.h` headers are still used by the launcher. |

## Repository layout

```
CMakeLists.txt          all targets, the warnings-as-errors "build gate", build options
vcpkg.json              openxr-loader, imgui (dx11 + win32)
third_party/OVR/        Oculus PC SDK headers go in Include/ (NOT committed, see its README)
src/bootstrap/          xinput1_3 proxy: dllmain.cpp (gates/hooks), dlssflat.cpp (20k lines), fsrinterop, pakgen, shaders
src/ovr2xr/             the LibOVR->OpenXR layer and all VR features (~150 files)
src/common/             header-only shared infrastructure (log, IAT hook, pattern scan, build/RVA tables, binds)
src/launcher/, src/setup/   launcher / installer; logic in *_*.h, Win32/UI glue in *.cpp
src/tests/              standalone unit tests, one exe per *_test.cpp (+ data/)
src/third_party/        vendored, unchanged: imgui_setup (1.92.8, launcher/setup only), puff (zlib inflate)
src/GENERATED_FILES.md  list of machine-generated tables (do not hand-edit them)
```

The biggest files are `ovr2xr/boneblend.cpp` (~46k lines), `bootstrap/dlssflat.cpp` (~20k), `ovr2xr/camhook.cpp` (~14k)
and `ovr2xr/ovr2xr.cpp` (~14k). Grep for a symbol before you read one of these. Don't read them top to bottom.

## Building

Requirements are Windows, VS 2022 C++ build tools, CMake ≥ 3.21 and vcpkg. You also need these external SDKs, which are
not in the repo:
- Oculus PC SDK headers → `third_party/OVR/Include/` (must be the **1.10-compatible** API, see the ABI note below).
- NVIDIA DLSS SDK (default `../DLSS_SDK`, or `-DDLVR_NGX_SDK=`).
- AMD FidelityFX SDK 2.3.0 (default `../FSR_SDK/v2.3.0/Kits/FidelityFX`, or `-DDLVR_FFX_SDK=`).

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DDLVR_NGX_SDK=<dlss> -DDLVR_FFX_SDK=<ffx>/Kits/FidelityFX
cmake --build build --config Release                                   # the two mod DLLs -> build/Release/
cmake --build build --config Release --target DyingLightVR_Launcher    # EXCLUDE_FROM_ALL, build it explicitly
```

CMake options:
- `DLVR_BETA_BUILD` (default **ON** in this tree) builds into `build/Release` only, with **no auto-deploy**. It defines
  `DLVR_BETA_LEAN` (periodic logs, help text and arm capture are compiled out) and emits PDBs. With it OFF, POST_BUILD
  copies the DLLs and FFX DLLs into `DLVR_GAME_DIR` (default is the upstream dev's `D:/SteamLibrary/...`, so set it).
- These are diagnostic-only and default OFF: `DLVR_HANG_WATCH`, `DLVR_CTXSITE_FAULT_TEST`, `DLVR_SWING_PROBE` and
  `DLVR_YAW_PROBE`. Use a separate build folder for them.

**Build gate:** for C++ only, MSVC turns C4129 (unrecognised escape) and the printf-format warnings
(C4473/4474/4476/4477/4313) into errors. The launcher's own `.cpp` files build at `/W4 /WX`. Keep them clean. Don't
filter build output in a way that hides warnings.

## Testing

Tests are pure-logic and need no game, headset or SDK. They run from an x64 VS developer prompt. The script finds
`cl` via vswhere if it isn't on PATH:

```powershell
src\tests\run_tests.bat [path\to\dlvr_binds.default.scr] [output folder]
```

- Each `src/tests/*_test.cpp` is compiled on its own (`cl /W4 /std:c++17`), with `puff.c` linked in. Each test runs
  from `src/tests/`. The exit code is the failure count. **New tests are picked up automatically** and need no
  registration. Output goes to `%TEMP%\dlvr_tests` by default, never into the tree.
- `binds_logic_test` is SKIPPED unless you pass the golden `dlvr_binds.default.scr`.
- Tests use a local `CHECK(cond)` macro that prints `FAIL file:line` and counts failures. Copy that pattern. There is
  no test framework.
- Include paths available to tests: `src`, `src/ovr2xr`, `src/bootstrap`, `src/setup`, `src/common`, `src/tests`,
  `src/third_party/puff`. **Not** `src/launcher`, so launcher tests include their header by relative path.

The game itself is the only real integration test. Note in your summary which changes still need a run in the headset.

## Architecture essentials

**The layer (`ovr2xr.cpp`)** maps two API mismatches:
1. `ovr_Create` happens before any D3D device exists, so it only does `xrCreateInstance` and `xrGetSystem`.
   `xrCreateSession` is deferred to the first `ovr_CreateTextureSwapChainDX`.
2. `ovr_SubmitFrame` is one call, so `xrWaitFrame` and `xrBeginFrame` are driven from whichever of
   `ovr_GetTrackingState` / `ovr_GetPredictedDisplayTime` comes first each frame. `xrEndFrame` happens in submit.

Both APIs are right-handed, Y-up, -Z forward and in metres, so poses copy field-for-field.
**ABI:** the build uses the SDK 1.10 headers. SDK 1.17+ renamed `HmdToEyeOffset` to `HmdToEyePose`, which silently
breaks the ABI. Never "upgrade" the OVR headers.

**Feature module pattern.** Almost every feature is split in three:
- `ovr2xr/<feature>.cpp/.h`: hooks, detours, engine memory reads and writes, logging. These are the side effects.
- `ovr2xr/<feature>_logic.h` (also `common/*_logic.h`, `bootstrap/*_logic.h`, `launcher/launcher_*.h`,
  `setup/setup_*.h`): **pure, header-only decisions** with no OpenXR, Windows API or logging, so they compile alone.
- `tests/<feature>_logic_test.cpp`: unit tests for that header.

When you add or change behaviour, put the decision in the `_logic.h` header and test it there. Keep the `.cpp`
limited to glue. New `.cpp` files must be added to the right target's source list in `CMakeLists.txt`. Its comments
there say what each file is for, so follow that style.

**Hooking the game.** The engine and gamedll addresses are fixed RVAs read from the **Steam** binaries (engine
`e9fbfc2c`, gamedll `97046bba`).
- Every patch site checks its bytes, vtable slot or export first. If they don't match, it **refuses and logs**
  instead of writing.
- `common/buildrva.h` identifies the build from the PE timestamp and image size. On Epic (EGL) it translates only
  the allowlisted RVAs in `common/buildrva_table.h`. That table is **generated**, so don't edit it by hand. Sites not
  on the allowlist turn themselves off on EGL. GOG is not supported.
- `common/pattern.h` checks the RVA hint first and falls back to a signature scan. **A pattern that matches more
  than once counts as no match.**
- Calling-convention-sensitive detours use MASM thunks (`*_thunk.asm`), because they tail-jump to keep the stolen
  prologue's shadow-space use intact.
- A game update or a different store build means checking these sites again. Never relax a byte check just to make
  a feature turn on.

**Settings.** One registry drives three things: the `ovr2xr.txt` parser, its writer and the in-game overlay UI. The
table is the `S_BOOL` / `S_FLOAT` / `S_INT` / `S_ENUM` / `*_LOCKED` / `S_BOOL_FOLLOWS` macros in `ovr2xr/ovr2xr.cpp`
(around line 1320), and the struct is in `ovr2xr/settings.h`. Rules:
- Add a knob in one place only, the table.
- **Never remove a key from the table**, because a key that drops out leaves a dead key behind. Use `*_LOCKED` to
  pin the compiled value and hide it.
- Use `restart=true` when the game only reads the value at startup.
- The bootstrap runs before the layer, so it reads a few keys from `ovr2xr.txt` as plain text.

Files the mod reads and writes go under `<game dir>\dlvr\` (`LayerFilePath`). Some features are switched on by marker
files there.

**Logging.** `common/log.h` writes each log next to its DLL: `dlvr_bootstrap.log`, `dlvr_engine.log` and
`dlvr_ovr2xr.log`. Each log is truncated on the first write of a launch and starts with a build banner. Writes are
serialised by a lock. Log lines are the main way to debug a tester's run, so make them **accurate and
self-describing**. A wrong or hard-coded log line is treated as a bug. Use `periodic_log.h` for anything per-frame,
and gate verbose probes behind diagnostic options or `DLVR_BETA_LEAN`.

## Conventions and pitfalls

- **Byte-exact files:** `.gitattributes` is `* -text`, so git does no line-ending conversion. Keep each file's
  existing line endings and encoding.
- **Don't write or append source with shell heredocs, `echo` or `sed`.** This project lost a headset run when a
  heredoc ate `\\` in a path string. Use a real file-edit tool.
- **Generated tables** (`*.inc`, `*.gen.h`, `buildrva_table.h`, `tests/data/*`; see `src/GENERATED_FILES.md`) come
  from private Python tools that read game data. Their generators and inputs are not in this repo, and several are
  cross-checked by `static_assert`. Don't hand-edit their numbers.
- **Comments carry the "why"**, usually with a date and a reference. Many refer to `docs/...`, `tools/...`,
  `src_beta/`, `CLAUDE.md`, `INV-nnn` or trace files, which come from the **private upstream dev tree and do not
  exist in this repo**. Keep these comments when editing, and write new ones in the same style: what the code
  protects against and why.
- `// [DEP: name] required-for=...` tags mark code that other behaviour depends on. Read the tag before removing or
  changing anything it guards.
- Code is in `namespace dlvr { namespace <feature> { ... } }`, with `WIN32_LEAN_AND_MEAN` and `NOMINMAX`. Use
  C++17, no exceptions in hot paths, no new third-party dependencies, and keep vendored code unchanged.
- The runtime has to tolerate SteamVR, Meta/Oculus and other OpenXR runtimes. Feature-detect extensions (for
  example `XR_FB_body_tracking`) instead of assuming them.

## Git / fork workflow

- `origin` = the fork `leoamaro01/Dying-Light-VR` and `upstream` = `bymodify-ux/Dying-Light-VR`.
- Sync with `git fetch upstream` then `git merge upstream/main`. Work on feature branches and open PRs against the
  fork, or upstream when contributing back.
- Upstream commit messages follow the pattern `<version>: <comma-separated user-visible changes>`.
- Releases built from the fork are **not** what the launcher updates to: `kRepoUrl` (and its test in
  `tests/launcher_update_test.cpp`) still points at upstream.
- The repo is GPL-3.0. Credit ported code in `README.md` and `THIRD_PARTY_NOTICES.txt`.
