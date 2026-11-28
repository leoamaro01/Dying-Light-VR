// dlvr bootstrap - loaded by Dying Light's own import table.
//
// engine_x64_rwdi.dll statically imports xinput1_3.dll (ordinals 2/3/4), so the
// Windows loader maps this DLL before DyingLightGame.exe's entry point runs -
// which is before the engine parses its command line. That is early enough to
// inject "/video=<our .scr>" and turn the hidden Oculus path on without ever
// touching the user's Documents\DyingLight\out\settings\video.scr.
//
// Two jobs:
//   1. forward XInput to the genuine C:\Windows\System32\XInput1_3.dll
//   2. repoint GetCommandLineA at an augmented command line
//
// The command-line hook patches *data*, not code: on Win10/11 GetCommandLineA is
//     48 8B 05 <disp32>   mov rax, [rip+disp32]
//     C3                  ret
// so we decode the displacement and overwrite the char* the function returns.
// No trampoline, no inline patching, nothing for anti-cheat or the loader to
// undo when it snaps engine_x64_rwdi.dll's IAT.

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <intrin.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/buildrva.h"   // the renderer race gate hint + the load-chain fix address on the Epic build
#include "../common/iathook.h"
#include "../common/log.h"
#include "../common/pattern.h"
#include "dlssflat.h"
#include "hangwatch.h"   // the start-up hang watch (empty unless built with DLVR_HANG_WATCH)

#define LOG(...) ::dlvr::LogTo("dlvr_bootstrap.log", __VA_ARGS__)
#define ELOG(...) ::dlvr::LogTo("dlvr_engine.log", __VA_ARGS__)

// pakgen.cpp (global scope - the rd3d11 watchdog below is inside the anonymous namespace): early_eye_size's rd3d11 slot.
void InstallEarlyEyeSizeHookOn(HMODULE rd);

namespace {

HMODULE g_realXInput = nullptr;

// Set by PreloadOvrRuntime() on success, read by diag::DiagHandler's hitD
// branch - see PreloadOvrRuntime's own header comment and CLAUDE.md's
// SteamVR/Bigscreen Beyond section for the full history. A real, valid,
// already-successfully-loaded handle for the exact correct file, kept
// around specifically so the engine's own failing internal load attempt has
// something known-good to fall back on instead of needing to succeed on its
// own merits.
HMODULE g_preloadedOvrRuntime = nullptr;

// ---------------------------------------------------------------- XInput ----

using PFN_GetState        = DWORD(WINAPI*)(DWORD, void*);
using PFN_SetState        = DWORD(WINAPI*)(DWORD, void*);
using PFN_GetCapabilities = DWORD(WINAPI*)(DWORD, DWORD, void*);
using PFN_Enable          = void (WINAPI*)(BOOL);
using PFN_GetDSoundGuids  = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
using PFN_GetBattery      = DWORD(WINAPI*)(DWORD, BYTE, void*);
using PFN_GetKeystroke    = DWORD(WINAPI*)(DWORD, DWORD, void*);

PFN_GetState        g_getState        = nullptr;
PFN_SetState        g_setState        = nullptr;
PFN_GetCapabilities g_getCapabilities = nullptr;
PFN_Enable          g_enable          = nullptr;
PFN_GetDSoundGuids  g_getDSoundGuids  = nullptr;
PFN_GetBattery      g_getBattery      = nullptr;
PFN_GetKeystroke    g_getKeystroke    = nullptr;

// ERROR_DEVICE_NOT_CONNECTED - what XInput returns for an absent pad. Safe to
// hand back if the genuine DLL could not be loaded; the game just sees no pad.
constexpr DWORD kNoDevice = 1167;

// -------------------------------------------------------------- system info ---
//
// A full environment fingerprint, logged unconditionally as the very first
// thing on every launch - before any gate patching or engine detection is
// even attempted, so a launch that fails before the engine ever calls into
// LibOVRRT64_1.dll (see the SteamVR/Bigscreen Beyond investigation in
// CLAUDE.md for a real example of exactly that) still leaves behind OS,
// CPU/GPU, RAM and game-platform info to triage from. Built for the public
// test build (see CLAUDE.md's public-build section) but left on for every
// build - more context in a bug report never hurts, and none of this reads
// anything specific to the game or engine.
void LogOsVersion()
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                       KEY_READ, &key) != ERROR_SUCCESS) {
        LOG("system: OS version unavailable (registry key open failed)");
        return;
    }
    char productName[128] = {};
    char displayVersion[64] = {};
    char buildNumber[64] = {};
    DWORD ubr = 0;
    DWORD size = sizeof(productName);
    RegQueryValueExA(key, "ProductName", nullptr, nullptr,
                      reinterpret_cast<BYTE*>(productName), &size);
    size = sizeof(displayVersion);
    if (RegQueryValueExA(key, "DisplayVersion", nullptr, nullptr,
                          reinterpret_cast<BYTE*>(displayVersion), &size) != ERROR_SUCCESS) {
        // Older builds (pre-20H2-ish) never had DisplayVersion - ReleaseId
        // is the same idea under its old name.
        size = sizeof(displayVersion);
        RegQueryValueExA(key, "ReleaseId", nullptr, nullptr,
                          reinterpret_cast<BYTE*>(displayVersion), &size);
    }
    size = sizeof(buildNumber);
    RegQueryValueExA(key, "CurrentBuildNumber", nullptr, nullptr,
                      reinterpret_cast<BYTE*>(buildNumber), &size);
    size = sizeof(ubr);
    RegQueryValueExA(key, "UBR", nullptr, nullptr, reinterpret_cast<BYTE*>(&ubr), &size);
    RegCloseKey(key);
    LOG("system: OS %s %s (build %s.%lu)", productName[0] ? productName : "?",
        displayVersion[0] ? displayVersion : "?", buildNumber[0] ? buildNumber : "?", ubr);
}

void LogCpuAndRam()
{
    int regs[4] = {};
    char brand[0x40] = {};
    __cpuid(regs, static_cast<int>(0x80000000));
    if (static_cast<unsigned int>(regs[0]) >= 0x80000004u) {
        __cpuid(reinterpret_cast<int*>(brand), static_cast<int>(0x80000002));
        __cpuid(reinterpret_cast<int*>(brand + 16), static_cast<int>(0x80000003));
        __cpuid(reinterpret_cast<int*>(brand + 32), static_cast<int>(0x80000004));
    }
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    LOG("system: CPU %s (%lu logical cores), RAM %.1f GB total", brand[0] ? brand : "?",
        static_cast<unsigned long>(si.dwNumberOfProcessors),
        static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
}

// Runs on its own thread (see LogSystemInfo below) - CreateDXGIFactory1
// reliably failed when called synchronously from DllMain(DLL_PROCESS_ATTACH)
// on this exact machine/driver during the public build's own smoke test
// (logged as a clean "GPU enumeration failed", not a hang or crash - but
// silently missing exactly the info a GPU-adjacent bug report most needs).
// Consistent with DXGI/D3D being well known to misbehave when created while
// the loader lock is held; deferring to a short-lived worker thread, same
// pattern already used elsewhere in this codebase (the rd3d11 watchdog, the
// diag watcher thread), sidesteps it entirely rather than chasing the exact
// internal reason.
DWORD WINAPI LogGpuAdaptersThread(LPVOID)
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory))) ||
        !factory) {
        LOG("system: GPU enumeration failed (CreateDXGIFactory1)");
        return 0;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr,
                                nullptr);
            LOG("system: GPU[%u] %s (VRAM %.1f GB, vendor 0x%04X device 0x%04X)%s", i, name,
                static_cast<double>(desc.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0),
                desc.VendorId, desc.DeviceId,
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software/WARP]" : "");
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return 0;
}

// Best-effort, not authoritative: the path check is the more reliable half
// (a store-specific install path virtually always contains its own store
// folder name) - the module-presence check depends on load order between
// this DLL and the store SDK's own, which is not guaranteed to have run
// yet at DLL_PROCESS_ATTACH time, so it can legitimately come back "not
// loaded yet" on either store without meaning anything is wrong.
void LogGamePlatform(const char* hostPath)
{
    const bool steamPath = strstr(hostPath, "steamapps") != nullptr;
    const bool epicPath = strstr(hostPath, "Epic Games") != nullptr;
    const bool steamModule = GetModuleHandleA("steam_api64.dll") != nullptr;
    const bool epicModule = GetModuleHandleA("EOSSDK-Win64-Shipping.dll") != nullptr;
    const char* guess = (steamPath || steamModule) ? "STEAM"
                        : (epicPath || epicModule)  ? "EPIC"
                                                     : "UNKNOWN";
    LOG("system: game platform guess = %s (path: steam=%d epic=%d; module: "
        "steam_api64=%d EOSSDK=%d)", guess, steamPath, epicPath, steamModule, epicModule);
}

void LogSystemInfo(const char* hostPath)
{
    LogOsVersion();
    LogCpuAndRam();
    // See LogGpuAdaptersThread's own comment - not called inline, has to run
    // off DllMain's own thread. Fire-and-forget: the thread logs for itself
    // and exits; nothing here needs its result or has to wait for it.
    CreateThread(nullptr, 0, LogGpuAdaptersThread, nullptr, 0, nullptr);
    LogGamePlatform(hostPath);
}

void LoadRealXInput()
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    strcat_s(path, "\\XInput1_3.dll");
    g_realXInput = LoadLibraryA(path);
    if (!g_realXInput) {
        // Not every install has the DX9 redist; XInput1_4 is always present and
        // is ABI-compatible for everything the engine imports.
        n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return;
        }
        strcat_s(path, "\\XInput1_4.dll");
        g_realXInput = LoadLibraryA(path);
    }
    if (!g_realXInput) {
        return;
    }

    auto get = [](const char* name) {
        return GetProcAddress(g_realXInput, name);
    };
    g_getState        = reinterpret_cast<PFN_GetState>(get("XInputGetState"));
    g_setState        = reinterpret_cast<PFN_SetState>(get("XInputSetState"));
    g_getCapabilities = reinterpret_cast<PFN_GetCapabilities>(get("XInputGetCapabilities"));
    g_enable          = reinterpret_cast<PFN_Enable>(get("XInputEnable"));
    g_getDSoundGuids  = reinterpret_cast<PFN_GetDSoundGuids>(get("XInputGetDSoundAudioDeviceGuids"));
    g_getBattery      = reinterpret_cast<PFN_GetBattery>(get("XInputGetBatteryInformation"));
    g_getKeystroke    = reinterpret_cast<PFN_GetKeystroke>(get("XInputGetKeystroke"));
}

// ----------------------------------------------------------- command line ----

char g_cmdline[32768];

// Returns the address of the char* that GetCommandLineA hands back, or null if
// the prologue is not the expected rip-relative load.
char** FindCommandLineSlot()
{
    const wchar_t* mods[] = { L"kernelbase.dll", L"kernel32.dll" };
    for (const wchar_t* m : mods) {
        HMODULE h = GetModuleHandleW(m);
        if (!h) {
            continue;
        }
        auto fn = reinterpret_cast<const uint8_t*>(GetProcAddress(h, "GetCommandLineA"));
        if (!fn) {
            continue;
        }
        // 48 8B 05 <disp32>  mov rax, [rip+disp32]
        if (fn[0] == 0x48 && fn[1] == 0x8B && fn[2] == 0x05) {
            int32_t disp;
            memcpy(&disp, fn + 3, sizeof(disp));
            return reinterpret_cast<char**>(const_cast<uint8_t*>(fn) + 7 + disp);
        }
    }
    return nullptr;
}

// Directory this DLL was loaded from - i.e. the game folder.
bool ModuleDir(HMODULE self, char* out, size_t cch)
{
    DWORD n = GetModuleFileNameA(self, out, static_cast<DWORD>(cch));
    if (n == 0 || n >= cch) {
        return false;
    }
    char* slash = strrchr(out, '\\');
    if (!slash) {
        return false;
    }
    *slash = '\0';
    return true;
}

// Attempts to pre-load LibOVRRT64_1.dll ourselves, plainly, before the engine
// ever gets a chance to try its own WinVerifyTrust-wrapped load (see
// PatchEngineSignatureGate's own comment, and CLAUDE.md's SteamVR/Bigscreen
// Beyond section). A friend's machine reproducibly gets ERROR_ACCESS_DENIED
// (5) from the engine's own internal LoadLibraryW call there, with every
// other explanation already checked and ruled out: not a missing file, not
// Mark-of-the-Web, not Smart App Control, not a third-party AV, not NTFS
// permissions on either DLL, not even Windows Defender with real-time
// protection fully off.
//
// Idea borrowed from noticing how a sibling project's ReviveXR-based mod
// actually gets its own shim DLL into the target process: an EXTERNAL
// injector, never relying on the game's own internal loader logic to
// succeed at all (see the ghidra-project/HANDOFF.md reference material).
// We do not have (and do not need) a separate injector process to get the
// same benefit - we already run code inside this process, earlier than the
// engine's own attempt. The Windows loader recognises an already-mapped
// module by its normalized full path and short-circuits a second
// LoadLibrary call on that same path to "bump the refcount, return the
// existing handle" - genuinely skipping the underlying file-open path a
// *fresh* load goes through, which is exactly the code path wrapped in the
// engine's own WinVerifyTrust call. If our own plain load here succeeds,
// the engine's later internal attempt should just hit that fast path and
// succeed too, regardless of whatever is denying a fresh open specifically
// on that one machine. If our own plain load ALSO fails with the same
// error, that is itself a real, useful answer - it rules out anything
// specific to the engine's own wrapped call, and points to something more
// fundamental about loading this exact file on this exact machine.
//
// Deliberately never freed - staying mapped for the rest of the process's
// life is the whole point.
void PreloadOvrRuntime(HMODULE self)
{
    char dir[MAX_PATH];
    if (!ModuleDir(self, dir, sizeof(dir))) {
        LOG("preload: could not determine module directory - skipping");
        return;
    }
    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\LibOVRRT64_1.dll", dir);
    // Logged unconditionally, success or fail - GetModuleHandleA (used by the
    // "still mapped right now" diag check elsewhere in this file) matches by
    // bare module NAME, but the loader's own internal "already mapped, skip
    // a fresh open" fast path matches by exact FULL PATH STRING. "Still
    // mapped" was never actual proof the engine's LoadLibraryW call was
    // hitting that fast path - only that *some* module with this basename
    // is loaded somewhere. Logging our own resolved path lets it be
    // byte-compared against the engine's own already-confirmed path
    // (v6_verify: "C:\Program Files (x86)\Steam\steamapps\common\Dying
    // Light\LibOVRRT64_1.dll") - if they differ at all, that alone would
    // explain why an already-loaded module doesn't save the engine's later
    // call.
    LOG("preload: resolved path = \"%s\", calling thread id = %lu", path, GetCurrentThreadId());
    HMODULE h = LoadLibraryA(path);
    if (h) {
        g_preloadedOvrRuntime = h;
        LOG("preload: LibOVRRT64_1.dll loaded successfully ahead of the engine's own attempt "
            "(handle=%p) - the engine's later LoadLibraryW on the same path should now just "
            "bump the refcount instead of doing a fresh open", static_cast<void*>(h));
    } else {
        LOG("preload: LibOVRRT64_1.dll FAILED to load here too, GetLastError=%lu - if this is "
            "5 (ACCESS_DENIED), same as the engine's own attempt, the block is NOT specific to "
            "the engine's own WinVerifyTrust-wrapped call path - it is something more "
            "fundamental about this file/machine", GetLastError());
    }
}

// Reads dlvr\extra_args.txt and appends it to the command line. Keeping the
// arguments in a text file rather than compiled in means we can change what we
// inject between launches without a rebuild - which matters while we are still
// working out which switches the engine actually honours.
//
// Blank lines and lines starting with # are ignored. The token %GAMEDIR% is
// replaced with the folder this DLL lives in, so paths can be written either
// absolute or relative and swapped between runs.
void InjectExtraArgs(HMODULE self)
{
    char dir[MAX_PATH];
    if (!ModuleDir(self, dir, sizeof(dir))) {
        LOG("FAIL: could not determine module directory");
        return;
    }
    LOG("game dir: %s", dir);

    char argsPath[MAX_PATH * 2];
    _snprintf_s(argsPath, sizeof(argsPath), _TRUNCATE, "%s\\dlvr\\extra_args.txt", dir);

    FILE* f = nullptr;
    if (fopen_s(&f, argsPath, "r") != 0 || !f) {
        LOG("SKIP: %s not found - running the game stock.", argsPath);
        return;
    }

    char args[4096] = {};
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') {
            continue;
        }
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == '\n' || p[n - 1] == ' ')) {
            p[--n] = '\0';
        }
        if (!n) {
            continue;
        }
        if (args[0]) {
            strcat_s(args, " ");
        }
        strcat_s(args, p);
    }
    fclose(f);

    if (!args[0]) {
        LOG("SKIP: %s is empty", argsPath);
        return;
    }

    // Expand %GAMEDIR%.
    char expanded[4096] = {};
    for (const char* s = args; *s;) {
        const char* tok = strstr(s, "%GAMEDIR%");
        if (!tok) {
            strcat_s(expanded, s);
            break;
        }
        size_t pre = static_cast<size_t>(tok - s);
        size_t have = strlen(expanded);
        memcpy(expanded + have, s, pre);
        expanded[have + pre] = '\0';
        strcat_s(expanded, dir);
        s = tok + strlen("%GAMEDIR%");
    }
    LOG("extra args: %s", expanded);

    char** slot = FindCommandLineSlot();
    if (!slot) {
        LOG("FAIL: GetCommandLineA prologue was not the expected rip-relative load");
        return;
    }

    const char* current = GetCommandLineA();
    if (!current) {
        current = "";
    }
    LOG("cmdline before: %s", current);

    _snprintf_s(g_cmdline, sizeof(g_cmdline), _TRUNCATE, "%s %s", current, expanded);

    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(char*), PAGE_READWRITE, &old)) {
        LOG("FAIL: VirtualProtect on the command-line slot (%lu)", GetLastError());
        return;
    }
    *slot = g_cmdline;
    VirtualProtect(slot, sizeof(char*), old, &old);

    LOG("cmdline after : %s", GetCommandLineA());
    LOG("injection %s", strcmp(GetCommandLineA(), g_cmdline) == 0 ? "OK" : "FAILED");

    // The C runtime keeps its OWN copy of the pointer: ucrtbase's __acrt_initialize_command_line sets acmdln =
    // GetCommandLineA() at its start-up, and WinMain's lpCmdLine (common_wincmdln) and __argv (common_configure_argv) read
    // acmdln when the exe's start-up calls them, after this DllMain (ucrtbase 10.0.26100.4652, read 2026-10-20). The engine
    // splits lpCmdLine into the gamedll's argument list (IGame::Initialize -> CGame slot 161 -> +228CC0 -> CGame+1080),
    // so without this an injected -nologos reached GetCommandLineA and the logos still played (Jay's flat check, 10-20).
    // If ucrtbase initialises AFTER this point it copies GetCommandLineA, which already returns g_cmdline.
    HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
    using PFN_PAcmdln = char**(__cdecl*)();
    auto pAcmdln = ucrt ? reinterpret_cast<PFN_PAcmdln>(GetProcAddress(ucrt, "__p__acmdln")) : nullptr;
    char** acmdln = pAcmdln ? pAcmdln() : nullptr;
    if (!acmdln) {
        LOG("cmdline CRT: NOT updated - ucrtbase.dll %s, so WinMain / argv (the gamedll's arguments) keep the original line",
            !ucrt ? "is not loaded" : !pAcmdln ? "has no __p__acmdln export" : "returned a null pointer");
        return;
    }
    LOG("cmdline CRT before: %s", *acmdln ? *acmdln : "(null - ucrtbase not initialised yet)");
    *acmdln = g_cmdline;
    LOG("cmdline CRT after : %s", *acmdln);
}

// ------------------------------------------------- Epic: first profile name --
//
// Epic testers lost their saves to "PROFILE CHANGED" once -nologos reached the gamedll (package 20261021). Read
// 2026-10-22 from the EGL engine (md5 21477b35) and OUR gamedll 97046bba (EGL gamedll fingerprint-identical):
//   - the first profile load (EGL engine +6DC9A1) names the profile from the /profile= setting, else from a static
//     0x100-byte buffer at +AC7620 that it fills with GetComputerNameA ONLY while the buffer's first byte is 0
//     (cmp byte [+AC7620],0 / jne past the call). That name goes into the profile's name field ([obj+0x60]+0x28) and
//     out/profiles/<name>.pro. A tester's PC wrote <computer name>.pro at the menu's first frame.
//   - when Epic's display-name query completes (CLobbyProfileEpic::GetDisplayNameTask, +6DCF6B..+6DCF88) the SAME
//     field is overwritten with the Epic display name ("<display name>.pro").
//   - the gamedll's user-changed test (Steam +120FCB0 / EGL +120F350) compares the stored name with the current one;
//     the player id it also compares is constant zero on Epic (ITools +3432 -> CLobby +2659C0 -> CLobbyProfileEpic
//     slot 2 +6CD7F0 writes zeros; ITools +3440 -> +265B50 equal), so on Epic the NAME alone decides it.
// Without the intro's wait the name is stored before the query completes, the overwrite then reads as a user
// change. Seeding the buffer with the launcher's -epicusername makes the first name the Epic name, so the later
// overwrite writes the same string. The buffer has no other reference in the engine (the two above), sits in .data's
// zero-fill tail (no initialiser writes it), and is read only at that first load - so attach time is early enough.
// Epic build only (buildrva: engine stamp + image size), byte-checked, writes nothing on any refusal. 2026-10-23: the
// seed alone was NOT enough - the menu-time load replaces the name; StartEpicMenuNameHook below covers it.
// COST: once, at DLL_PROCESS_ATTACH.
// The name actually seeded (empty when nothing was written) - the menu-load hook below gives the gamedll the same one.
char g_epicSeedName[0x100] = {};
size_t g_epicSeedLen = 0;

void SeedEpicProfileName()
{
    using namespace ::dlvr::buildrva;
    const Build b = ModuleBuild(kEngine);
    if (b != kEgl) {
        LOG("epic profile name: engine build %s - not the Epic build, nothing written", BuildName(b));
        return;
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("epic profile name: engine_x64_rwdi.dll is not loaded - nothing written");
        return;
    }
    const auto base = reinterpret_cast<uintptr_t>(engine);
    const uintptr_t kTestRva = 0x6DC9A1;   // cmp byte ptr [rip+disp], r15b  (r15 = 0 there)
    const uintptr_t kLeaRva = 0x6DC9A8;    // lea rbx, [rip+disp]
    const uintptr_t kBufRva = 0xAC7620;
    const size_t kBufSize = 0x100;          // the size the engine passes to GetComputerNameA
    const auto* test = reinterpret_cast<const unsigned char*>(base + kTestRva);
    const auto* lea = reinterpret_cast<const unsigned char*>(base + kLeaRva);
    auto target = [](const unsigned char* insn) {
        int32_t disp = 0;
        memcpy(&disp, insn + 3, sizeof(disp));
        return reinterpret_cast<uintptr_t>(insn) + 7 + static_cast<intptr_t>(disp);
    };
    const bool testOk = test[0] == 0x44 && test[1] == 0x38 && test[2] == 0x3D && target(test) == base + kBufRva;
    const bool leaOk = lea[0] == 0x48 && lea[1] == 0x8D && lea[2] == 0x1D && target(lea) == base + kBufRva;
    const bool jneOk = lea[7] == 0x75 && lea[8] == 0x14;
    if (!testOk || !leaOk || !jneOk) {
        LOG("epic profile name: the computer-name test at engine +6DC9A1 does not match (test %d, lea %d, jne %d) - "
            "nothing written", testOk ? 1 : 0, leaOk ? 1 : 0, jneOk ? 1 : 0);
        return;
    }
    char* buf = reinterpret_cast<char*>(base + kBufRva);
    if (buf[0] != '\0') {
        LOG("epic profile name: the engine's profile-name buffer is already filled - nothing written");
        return;
    }

    // -epicusername="Name With Spaces" or -epicusername=Name (the Epic launcher quotes it).
    const char* cmd = GetCommandLineA();
    const char* key = "-epicusername=";
    const char* at = nullptr;
    for (const char* p = cmd ? cmd : ""; *p; ++p) {
        if (_strnicmp(p, key, strlen(key)) == 0) {
            at = p + strlen(key);
            break;
        }
    }
    if (!at) {
        LOG("epic profile name: no -epicusername on the command line (not launched by the Epic launcher?) - nothing "
            "written, the engine names the profile itself");
        return;
    }
    char name[kBufSize] = {};
    size_t n = 0;
    const bool quoted = *at == '"';
    for (const char* p = quoted ? at + 1 : at; *p; ++p) {
        if (quoted ? *p == '"' : (*p == ' ' || *p == '\t')) {
            break;
        }
        if (n + 1 >= kBufSize) {
            LOG("epic profile name: -epicusername is %u or more characters - nothing written",
                static_cast<unsigned>(kBufSize - 1));
            return;
        }
        name[n++] = *p;
    }
    if (n == 0) {
        LOG("epic profile name: -epicusername is empty - nothing written");
        return;
    }
    // It becomes out/profiles/<name>.pro: printable ASCII only, none of the characters a file name cannot hold.
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c > 0x7E || strchr("\\/:*?\"<>|", c)) {
            LOG("epic profile name: -epicusername holds character 0x%02X at position %u (not printable ASCII, or not "
                "allowed in a file name) - nothing written, the engine names the profile itself",
                static_cast<unsigned>(c), static_cast<unsigned>(i));
            return;
        }
    }
    memcpy(buf, name, n + 1);
    memcpy(g_epicSeedName, name, n + 1);
    g_epicSeedLen = n;
    LOG("epic profile name: seeded engine +AC7620 with the launcher's -epicusername (%u characters) - the first "
        "profile loaded is the Epic account's, not the computer name's", static_cast<unsigned>(n));
}

// ------------------------------------------- Epic: the menu-time profile load --
//
// The seed alone did not stop PROFILE CHANGED (BETA-FINDINGS-2026-10-19 section 16 (i)-(n), the EPICPROF probe on two Epic testers):
// when the menu activates, the EGL gamedll ALWAYS reloads the profile as "<GetUserNameA>.pro" (GameDI slot 235 +44B65B
// -> +44A780; the Steam gamedll gates the same call on game+264, the EGL one does not). That swaps the seeded Epic name
// for the Windows user name, the user-changed test sees a different name, and OnUserChanged (EGL +120F530) raises the
// warning because the intro end has just set the finished byte. Measured: field 14 -> 5 -> stored 5 -> dialog.
// The EGL gamedll calls GetUserNameA from that one site only (+44A7BB, `call [rip]` to its IAT slot +1379000; capstone
// rip scan: 1 ref; the only "GetUserNameA" string is the import name). This hooks that IAT slot - the gamedll's, no other
// module's - and answers with the seeded name, so the menu load reloads the SAME profile and the test sees no change.
// The display-name write that follows never runs the test (measured on all three probe runs).
// Refused (logged, the call left alone = today's behaviour) when: nothing was seeded; the gamedll is not the EGL build;
// the call site or its IAT slot does not match; the name holds a '.' (the engine's load +6DC970 cuts the passed file
// name at its FIRST '.', so "a.b.pro" would load "a" while the seed loaded "a.b" - still a change).
// COST: one thread that polls GetModuleHandleA every 5 ms until the gamedll maps (it maps long before the menu, which is
// seconds after the intro), patches one pointer and exits; at most 120 s. The hook runs once per menu activation.
namespace epicmenu {

using PFN_GetUserNameA = BOOL(WINAPI*)(LPSTR, LPDWORD);
PFN_GetUserNameA g_orig = nullptr;
volatile LONG g_calls = 0;

BOOL WINAPI Hook(LPSTR buf, LPDWORD size)
{
    const LONG call = InterlockedIncrement(&g_calls);
    if (buf && size && *size > g_epicSeedLen) {
        memcpy(buf, g_epicSeedName, g_epicSeedLen + 1);
        *size = static_cast<DWORD>(g_epicSeedLen + 1);
        if (call <= 4) {
            LOG("epic menu name: call %ld - the gamedll's GetUserNameA answered with the seeded Epic name (%u characters), "
                "so the menu-time profile load reloads the same profile", call, static_cast<unsigned>(g_epicSeedLen));
        }
        return TRUE;
    }
    if (call <= 4) {
        LOG("epic menu name: call %ld - buffer %s (size %lu, the name needs %u) - passed to the real GetUserNameA", call,
            buf && size ? "too small" : "missing", size ? *size : 0, static_cast<unsigned>(g_epicSeedLen + 1));
    }
    return g_orig ? g_orig(buf, size) : FALSE;
}

DWORD WINAPI Thread(LPVOID)
{
    using namespace ::dlvr::buildrva;
    const ULONGLONG t0 = GetTickCount64();
    HMODULE gd = nullptr;
    while (!(gd = GetModuleHandleA("gamedll_x64_rwdi.dll"))) {
        if (GetTickCount64() - t0 > 120000) {
            LOG("epic menu name: gamedll_x64_rwdi.dll did not load within 120 s - nothing hooked");
            return 0;
        }
        Sleep(5);
    }
    const Build b = ModuleBuild(kGamedll);
    if (b != kEgl) {
        LOG("epic menu name: gamedll build %s - not the Epic gamedll, nothing hooked", BuildName(b));
        return 0;
    }
    const auto base = reinterpret_cast<uintptr_t>(gd);
    const auto* site = reinterpret_cast<const unsigned char*>(base + 0x44A7BB);   // call qword ptr [rip+disp]
    int32_t disp = 0;
    memcpy(&disp, site + 2, sizeof(disp));
    const uintptr_t siteSlot = base + 0x44A7BB + 6 + static_cast<intptr_t>(disp);
    void** slot = ::dlvr::FindIatSlot(gd, "ADVAPI32.dll", "GetUserNameA");
    const bool siteOk = site[0] == 0xFF && site[1] == 0x15 && siteSlot == base + 0x1379000;
    const bool slotOk = slot && reinterpret_cast<uintptr_t>(slot) == base + 0x1379000;
    if (!siteOk || !slotOk) {
        LOG("epic menu name: the GetUserNameA call at gamedll +44A7BB does not match (call %d, IAT slot %d) - nothing "
            "hooked", siteOk ? 1 : 0, slotOk ? 1 : 0);
        return 0;
    }
    // GetModuleHandleA can return the gamedll while the loader is still binding its imports: wait until the slot holds
    // an address inside another loaded module (the bound GetUserNameA), so the loader cannot overwrite the patch.
    auto bound = [&]() {
        HMODULE owner = nullptr;
        return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCSTR>(*slot), &owner) && owner && owner != gd;
    };
    while (!bound()) {
        if (GetTickCount64() - t0 > 120000) {
            LOG("epic menu name: the gamedll's GetUserNameA import was not bound within 120 s - nothing hooked");
            return 0;
        }
        Sleep(5);
    }
    void* prev = ::dlvr::PatchIat(gd, "ADVAPI32.dll", "GetUserNameA", reinterpret_cast<void*>(&Hook));
    if (!prev) {
        LOG("epic menu name: patching the gamedll's GetUserNameA import FAILED - nothing hooked");
        return 0;
    }
    g_orig = reinterpret_cast<PFN_GetUserNameA>(prev);
    LOG("epic menu name: hooked the EGL gamedll's GetUserNameA (+44A7BB, its only call) at %lld ms - the menu-time "
        "profile load will use the seeded Epic name (%u characters)",
        static_cast<long long>(GetTickCount64() - t0), static_cast<unsigned>(g_epicSeedLen));
    Sleep(2000);
    LOG("epic menu name: 2 s later the IAT slot %s", *slot == reinterpret_cast<void*>(&Hook)
        ? "still holds the hook" : "NO LONGER holds the hook - something rewrote it; the menu load is NOT covered");
    return 0;
}

}  // namespace epicmenu

void StartEpicMenuNameHook()
{
    using namespace ::dlvr::buildrva;
    const Build eb = ModuleBuild(kEngine);
    if (eb != kEgl) {
        LOG("epic menu name: engine build %s - not the Epic build, nothing hooked", BuildName(eb));
        return;
    }
    if (!g_epicSeedLen) {
        LOG("epic menu name: nothing was seeded (see the line above) - the menu-time load is left alone");
        return;
    }
    if (strchr(g_epicSeedName, '.')) {
        LOG("epic menu name: the seeded name holds a '.', which the engine's load cuts the file name at - left alone "
            "(today's behaviour)");
        return;
    }
    HANDLE th = CreateThread(nullptr, 0, &epicmenu::Thread, nullptr, 0, nullptr);
    if (!th) {
        LOG("epic menu name: FAILED to start its thread (%lu) - nothing hooked", GetLastError());
        return;
    }
    CloseHandle(th);
}

// ------------------------------------------------ Epic: profile-change probe --
//
// The seed above was written on both Epic testers and PROFILE CHANGED still fired (BETA-FINDINGS-2026-10-19 section 16,
// spec docs/superpowers/specs/2026-10-22-epic-profile-probe-design.md). Static leaves three candidates: the Epic display
// name differs from -epicusername; the intro end stores the name before the startup load fills it; the user-changed
// test's force byte is set. This reads, never writes, the values that decide between them:
//   engine (EGL 21477b35): lobby = [+A3B8F8]; profile = [lobby+0x3F0], vtable must be CLobbyProfileEpic +8B0950;
//     loaded flag = byte [profile+0xB8] (+6CDBC0 sets it after the startup load); name field = char* [[profile+0x60]+0x28],
//     length dword [[profile+0x60]+0x30] (+6CDBF0 / +6DCFA2).
//   gamedll (EGL, Steam rvas mapped with tools/maplookup.py on map_gamedll.pkl): game = [+1C15298] (Steam +1C16348),
//     vtable must be GameDI +14A3488 (Steam +14A4550); stored name = char* [game+408], length dword [game+416], force
//     byte [game+1328] (all read by the test, Steam +120FCB0 / EGL +120F350); intro finished = byte [+1C153D3] (Steam
//     +1C16483, set by IntroModule::MainThreadUpdate in the frame that stores the name).
// Only lengths and equalities are logged, never a name - tester logs travel.
// COST: one thread of its own, about twenty guarded reads every 10 ms for 120 s, then it exits. Nothing on the game's
// threads, no hooks, no patches, no exec watch. Off unless dlvr\epic_profile_probe.txt exists.
namespace epicprof {

constexpr uintptr_t kLobbyRva = 0xA3B8F8;
constexpr uintptr_t kProfileVtRva = 0x8B0950;
constexpr uintptr_t kGameRva = 0x1C15298;
constexpr uintptr_t kGameVtRva = 0x14A3488;
constexpr uintptr_t kIntroDoneRva = 0x1C153D3;
constexpr DWORD kPollMs = 10;
constexpr ULONGLONG kRunMs = 120000;
constexpr unsigned kLineCap = 200;
constexpr uint32_t kNameCap = 256;

char g_user[kNameCap] = {};
uint32_t g_userLen = 0;
bool g_userFound = false;

bool ReadU64(uintptr_t a, uintptr_t* out)
{
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU32(uintptr_t a, uint32_t* out)
{
    __try {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU8(uintptr_t a, uint8_t* out)
{
    __try {
        *out = *reinterpret_cast<const uint8_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies at most kNameCap - 1 bytes; a null pointer or zero length is an empty string, not a fault.
bool ReadName(uintptr_t p, uint32_t len, char* out)
{
    out[0] = '\0';
    if (!p || !len) {
        return true;
    }
    const uint32_t n = len < kNameCap - 1 ? len : kNameCap - 1;
    __try {
        memcpy(out, reinterpret_cast<const void*>(p), n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
    out[n] = '\0';
    return true;
}

// -epicusername parsed the way SeedEpicProfileName parses it (quoted or bare).
void ParseUser()
{
    const char* cmd = GetCommandLineA();
    const char* key = "-epicusername=";
    for (const char* p = cmd ? cmd : ""; *p; ++p) {
        if (_strnicmp(p, key, strlen(key)) == 0) {
            const char* at = p + strlen(key);
            const bool quoted = *at == '"';
            uint32_t n = 0;
            for (const char* q = quoted ? at + 1 : at; *q && n + 1 < kNameCap; ++q) {
                if (quoted ? *q == '"' : (*q == ' ' || *q == '\t')) {
                    break;
                }
                g_user[n++] = *q;
            }
            g_user[n] = '\0';
            g_userLen = n;
            g_userFound = true;
            return;
        }
    }
}

int Eq(const char* a, const char* b, bool noCase)
{
    if (!a[0] || !b[0]) {
        return 0;
    }
    return (noCase ? _stricmp(a, b) : strcmp(a, b)) == 0 ? 1 : 0;
}

uint32_t Fnv(const char* s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h = (h ^ static_cast<unsigned char>(*s)) * 16777619u;
    }
    return h;
}

// -1 = not readable (object missing, vtable not the expected class, or a faulted read).
struct Snap {
    int loaded, fieldLen, fieldEqUser, fieldEqUserNoCase;
    int storedLen, storedEqField, storedEqUser, storedEqUserNoCase;
    int force, introDone;
};

bool SameSnap(const Snap& a, const Snap& b)
{
    return memcmp(&a, &b, sizeof(Snap)) == 0;
}

DWORD WINAPI Thread(LPVOID)
{
    using namespace ::dlvr::buildrva;
    const ULONGLONG t0 = GetTickCount64();
    const auto engine = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine_x64_rwdi.dll"));
    uintptr_t gamedll = 0;
    bool gdChecked = false, gdOk = false, profVtLogged = false, gameVtLogged = false, capLogged = false;
    Snap prev = {};
    bool havePrev = false;
    uintptr_t fieldPtrPrev = 0, storedPtrPrev = 0;
    uint32_t fieldHashPrev = 0, storedHashPrev = 0;
    unsigned fieldChanges = 0, storedChanges = 0, storedChangesAfterIntro = 0, lines = 0, samples = 0, faults = 0;
    long long firstField = -1, firstStored = -1, firstIntro = -1, firstLoaded = -1;
    char field[kNameCap], stored[kNameCap];

    // [PROBE: EPICPROF] gate=dlvr\epic_profile_probe.txt (marker) + EGL engine and gamedll  site=FLAT  log-only=yes
    LOG("[EPICPROF] running: polls every %lu ms for %llu s | -epicusername %s (%u characters)", kPollMs, kRunMs / 1000,
        g_userFound ? "found" : "NOT on the command line", g_userLen);
    while (GetTickCount64() - t0 < kRunMs) {
        const long long t = static_cast<long long>(GetTickCount64() - t0);
        if (!gamedll) {
            gamedll = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
        }
        if (gamedll && !gdChecked) {
            gdChecked = true;
            const Build gb = ModuleBuild(kGamedll);
            gdOk = gb == kEgl;
            LOG("[EPICPROF] gamedll loaded at %lld ms, build %s%s", t, BuildName(gb),
                gdOk ? "" : " - not the Epic gamedll the addresses were read from: game values NOT read");
        }
        Snap s = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        uintptr_t fieldPtr = 0, storedPtr = 0;
        field[0] = stored[0] = '\0';
        bool fieldRead = false, storedRead = false;

        uintptr_t lobby = 0, prof = 0, vt = 0, sub = 0;
        if (ReadU64(engine + kLobbyRva, &lobby) && lobby && ReadU64(lobby + 0x3F0, &prof) && prof && ReadU64(prof, &vt)) {
            if (vt != engine + kProfileVtRva) {
                if (!profVtLogged) {
                    profVtLogged = true;
                    LOG("[EPICPROF] the lobby's profile object has vtable engine+%llX, not CLobbyProfileEpic +8B0950 - "
                        "profile values NOT read", static_cast<unsigned long long>(vt - engine));
                }
            } else {
                uint8_t loaded = 0;
                uint32_t len = 0;
                if (ReadU8(prof + 0xB8, &loaded)) {
                    s.loaded = loaded ? 1 : 0;
                } else {
                    ++faults;
                }
                if (ReadU64(prof + 0x60, &sub) && sub && ReadU64(sub + 0x28, &fieldPtr) && ReadU32(sub + 0x30, &len) &&
                    ReadName(fieldPtr, len, field)) {
                    fieldRead = true;
                    s.fieldLen = static_cast<int>(strlen(field));
                    s.fieldEqUser = Eq(field, g_user, false);
                    s.fieldEqUserNoCase = Eq(field, g_user, true);
                } else if (sub) {
                    ++faults;
                }
            }
        }
        uintptr_t game = 0, gvt = 0;
        if (gdOk && ReadU64(gamedll + kGameRva, &game) && game && ReadU64(game, &gvt)) {
            if (gvt != gamedll + kGameVtRva) {
                if (!gameVtLogged) {
                    gameVtLogged = true;
                    LOG("[EPICPROF] the game object has vtable gamedll+%llX, not GameDI +14A3488 - game values NOT read",
                        static_cast<unsigned long long>(gvt - gamedll));
                }
            } else {
                uint32_t len = 0;
                uint8_t force = 0, intro = 0;
                if (ReadU64(game + 408, &storedPtr) && ReadU32(game + 416, &len) && ReadName(storedPtr, len, stored)) {
                    storedRead = true;
                    s.storedLen = static_cast<int>(strlen(stored));
                    s.storedEqField = fieldRead ? Eq(stored, field, false) : -1;
                    s.storedEqUser = Eq(stored, g_user, false);
                    s.storedEqUserNoCase = Eq(stored, g_user, true);
                } else {
                    ++faults;
                }
                if (ReadU8(game + 1328, &force)) {
                    s.force = force;
                } else {
                    ++faults;
                }
                if (ReadU8(gamedll + kIntroDoneRva, &intro)) {
                    s.introDone = intro ? 1 : 0;
                } else {
                    ++faults;
                }
            }
        }
        ++samples;
        if (fieldRead) {
            const uint32_t h = Fnv(field);
            if (havePrev && (fieldPtr != fieldPtrPrev || h != fieldHashPrev)) {
                ++fieldChanges;
            }
            fieldPtrPrev = fieldPtr;
            fieldHashPrev = h;
        }
        if (storedRead) {
            const uint32_t h = Fnv(stored);
            if (havePrev && (storedPtr != storedPtrPrev || h != storedHashPrev)) {
                ++storedChanges;
                if (s.introDone == 1 && prev.introDone == 1) {
                    ++storedChangesAfterIntro;
                }
            }
            storedPtrPrev = storedPtr;
            storedHashPrev = h;
        }
        if (firstField < 0 && s.fieldLen > 0) {
            firstField = t;
        }
        if (firstStored < 0 && s.storedLen > 0) {
            firstStored = t;
        }
        if (firstIntro < 0 && s.introDone == 1) {
            firstIntro = t;
        }
        if (firstLoaded < 0 && s.loaded == 1) {
            firstLoaded = t;
        }
        if (!havePrev || !SameSnap(s, prev)) {
            if (lines < kLineCap) {
                ++lines;
                LOG("[EPICPROF] %lld ms | profile loaded %d | name field: length %d, equals the epic name %d (ignoring "
                    "case %d) | stored name: length %d, equals the field %d, equals the epic name %d (ignoring case "
                    "%d) | force byte %d | intro finished %d | changes so far: field %u, stored %u (after intro %u)",
                    t, s.loaded, s.fieldLen, s.fieldEqUser, s.fieldEqUserNoCase, s.storedLen, s.storedEqField,
                    s.storedEqUser, s.storedEqUserNoCase, s.force, s.introDone, fieldChanges, storedChanges,
                    storedChangesAfterIntro);
            } else if (!capLogged) {
                capLogged = true;
                LOG("[EPICPROF] %u change lines reached - no more change lines, the summary still prints", kLineCap);
            }
            prev = s;
            havePrev = true;
        }
        Sleep(kPollMs);
    }
    LOG("[EPICPROF] done after %llu s: %u samples, %u change lines, %u faulted reads | first ms: profile loaded %lld, name "
        "field non-empty %lld, stored name non-empty %lld, intro finished %lld (-1 = never) | changes: field %u, stored %u, "
        "stored after intro %u", kRunMs / 1000, samples, lines, faults, firstLoaded, firstField, firstStored, firstIntro,
        fieldChanges, storedChanges, storedChangesAfterIntro);
    return 0;
}

}  // namespace epicprof

void StartEpicProfileProbe(HMODULE self)
{
    char gdir[MAX_PATH];
    if (!ModuleDir(self, gdir, sizeof(gdir))) {
        LOG("[EPICPROF] off - the game folder could not be read, so dlvr\\epic_profile_probe.txt could not be checked");
        return;
    }
    char marker[MAX_PATH];
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\dlvr\\epic_profile_probe.txt", gdir);
    if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES) {
        LOG("[EPICPROF] off - no %s (the normal state)", marker);
        return;
    }
    const ::dlvr::buildrva::Build b = ::dlvr::buildrva::ModuleBuild(::dlvr::buildrva::kEngine);
    if (b != ::dlvr::buildrva::kEgl) {
        LOG("[EPICPROF] engine build %s - not the Epic build, nothing read (marker %s)", ::dlvr::buildrva::BuildName(b),
            marker);
        return;
    }
    epicprof::ParseUser();
    HANDLE t = CreateThread(nullptr, 0, &epicprof::Thread, nullptr, 0, nullptr);
    if (!t) {
        LOG("[EPICPROF] FAILED to start its thread (%lu) - nothing read", GetLastError());
        return;
    }
    CloseHandle(t);
}

// ------------------------------------------------------- engine log capture --
//
// The engine funnels every log line through filesystem_x64_rwdi.dll's _CLog,
// which it imports by name. Hooking that IAT slot gives us the game's own log
// on every run - no crash required - which is the only way to see whether
// /video= was accepted and whether OculusEnabled reached the video settings.
//
// From the call site at engine!0x229d36 the signature is:
//   void _CLog(int level, const char* category, const char* file, int line,
//              int filterAction, int lineMode, const char* fmt, ...)

// The engine imports three of these; miss one and log lines vanish.
const char* const kCLogSymbol =
    "?_CLog@@YAXW4TYPE@ELevel@Log@@PEBD1HW4ENUM@CLFilterAction@@W44CLLineMode@@1ZZ";
const char* const kCLogStrSymbol =
    "?_CLog@@YAXW4TYPE@ELevel@Log@@PEBD1HW4ENUM@CLFilterAction@@W44CLLineMode@@"
    "AEBV?$string_base@D@ttl@@@Z";
const char* const kCLogVSymbol =
    "?_CLogV@@YAXW4TYPE@ELevel@Log@@PEBD1HW4ENUM@CLFilterAction@@W44CLLineMode@@1PEAD@Z";

using PFN_CLog = void(__cdecl*)(int, const char*, const char*, int, int, int, const char*, ...);
using PFN_CLogStr = void(__cdecl*)(int, const char*, const char*, int, int, int, const void*);
using PFN_CLogV = void(__cdecl*)(int, const char*, const char*, int, int, int, const char*, va_list);

PFN_CLog    g_origCLog    = nullptr;
PFN_CLogStr g_origCLogStr = nullptr;
PFN_CLogV   g_origCLogV   = nullptr;

// ttl::string_base<char> layout is unknown to us. Best guess is a leading char*,
// so probe that defensively rather than trusting it.
const char* SafeStringBase(const void* s)
{
    if (!s || IsBadReadPtr(s, sizeof(void*))) {
        return "<unreadable>";
    }
    const char* p = *reinterpret_cast<const char* const*>(s);
    if (!p || IsBadReadPtr(p, 1)) {
        // Might be small-string-optimised: the text could be inline.
        p = reinterpret_cast<const char*>(s);
        if (IsBadReadPtr(p, 1)) {
            return "<unreadable>";
        }
    }
    for (int i = 0; i < 512; ++i) {
        if (IsBadReadPtr(p + i, 1)) {
            return "<unreadable>";
        }
        if (p[i] == '\0') {
            return i ? p : "";
        }
        if (static_cast<unsigned char>(p[i]) < 9) {
            return "<not-a-string>";
        }
    }
    return "<unterminated>";
}

void __cdecl HookedCLog(int level, const char* category, const char* file, int line,
                        int filterAction, int lineMode, const char* fmt, ...)
{
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt ? fmt : "", ap);
    va_end(ap);

    ELOG("[%d] %-12s %s", level, category ? category : "?", msg);
    ::dlvr::hangwatch::OnEngineLine(msg);

    if (g_origCLog) {
        // Re-formatting through "%s" avoids having to forward varargs.
        g_origCLog(level, category, file, line, filterAction, lineMode, "%s", msg);
    }
}

void __cdecl HookedCLogStr(int level, const char* category, const char* file, int line,
                           int filterAction, int lineMode, const void* str)
{
    const char* text = SafeStringBase(str);
    ELOG("[%d] %-12s %s", level, category ? category : "?", text);
    ::dlvr::hangwatch::OnEngineLine(text);
    if (g_origCLogStr) {
        g_origCLogStr(level, category, file, line, filterAction, lineMode, str);
    }
}

void __cdecl HookedCLogV(int level, const char* category, const char* file, int line,
                         int filterAction, int lineMode, const char* fmt, va_list ap)
{
    char msg[4096];
    va_list copy;
    va_copy(copy, ap);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt ? fmt : "", copy);
    va_end(copy);

    ELOG("[%d] %-12s %s", level, category ? category : "?", msg);
    ::dlvr::hangwatch::OnEngineLine(msg);

    if (g_origCLogV) {
        g_origCLogV(level, category, file, line, filterAction, lineMode, fmt, ap);
    }
}

void InstallEngineLogHook()
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("engine log hook: engine_x64_rwdi.dll not mapped yet - skipped");
        return;
    }

    struct { const char* sym; void* hook; void** orig; const char* label; } jobs[] = {
        { kCLogSymbol,    &HookedCLog,    reinterpret_cast<void**>(&g_origCLog),    "_CLog(...)" },
        { kCLogStrSymbol, &HookedCLogStr, reinterpret_cast<void**>(&g_origCLogStr), "_CLog(string)" },
        { kCLogVSymbol,   &HookedCLogV,   reinterpret_cast<void**>(&g_origCLogV),   "_CLogV" },
    };

    int ok = 0;
    for (auto& j : jobs) {
        void* orig = ::dlvr::PatchIat(engine, "filesystem_x64_rwdi.dll", j.sym, j.hook);
        if (orig) {
            *j.orig = orig;
            ++ok;
            LOG("engine log hook: %s -> %p", j.label, orig);
        } else {
            LOG("engine log hook: %s NOT FOUND in engine's imports", j.label);
        }
    }
    LOG("engine log hook: %d/3 installed -> dlvr_engine.log", ok);
}

// ------------------------------------------------ Oculus bring-up switches --
//
// dlvr\force_oculus.txt switches the three gate patches below. The engine runs
// its own Oculus init once its enable gate is patched; nothing here calls the
// renderer's init itself (that path was removed 2026-11-15, batch 3).

bool     g_patchSigCheck = true;
bool     g_patchEngineGate = true;
bool     g_patchRendererRaceGate = true;  // see PatchEngineRendererRaceGate()

void LoadForceConfig(const char* dir)
{
    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\dlvr\\force_oculus.txt", dir);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        LOG("force-oculus: disabled (%s absent)", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            continue;
        }
        int v = 0;
        if (sscanf_s(line, "patch_signature_check=%d", &v) == 1) {
            g_patchSigCheck = (v != 0);
        } else if (sscanf_s(line, "patch_engine_gate=%d", &v) == 1) {
            g_patchEngineGate = (v != 0);
        } else if (sscanf_s(line, "patch_renderer_race_gate=%d", &v) == 1) {
            g_patchRendererRaceGate = (v != 0);
        }
    }
    fclose(f);
    LOG("force-oculus: patch_signature_check=%d patch_engine_gate=%d patch_renderer_race_gate=%d",
        g_patchSigCheck ? 1 : 0, g_patchEngineGate ? 1 : 0, g_patchRendererRaceGate ? 1 : 0);
}

// The Oculus CAPI shim will not load a LibOVRRT64_1.dll that is not Authenticode
// signed by "Oculus VR, LLC". In rd3d11_x64_rwdi.dll the check sits at 0x4A270
// (CryptQueryObject -> WinVerifyTrust -> WTHelperGetProvSignerFromChain ->
// CertGetNameStringW, all resolved dynamically from wintrust/crypt32), and its
// result gates the load:
//
//   0x4A909  48 83 F8 FF   cmp rax, -1        ; -1 = verification failed
//   0x4A90D  74 1E         je  0x4A92D        ; ...so skip LoadLibraryW
//   0x4A90F  48 8D 4C 24   lea rcx, [rsp+0x40]
//   0x4A914  ff15 ...      call LoadLibraryW
//
// Turning the je into two NOPs lets an unsigned runtime load. We only ever have
// our own DLL to offer, so there is no way around this short of Oculus's private
// key - which is precisely why Revive ships an injector rather than a drop-in.
// If verification did fail, rbx holds -1 and the following CloseHandle(-1) is a
// harmless no-op.

// The engine carries its own copy of the CAPI shim, so it has its own copy of
// the Authenticode gate too - same instruction pair, different module:
//   engine+0x60C3A9  cmp rax, -1
//   engine+0x60C3AD  je  0x60C3CD    ; skip LoadLibraryW
//
// Those offsets hold for the Epic build. The Steam build is a different compile
// of the same game version - rd3d11 is byte-identical there but the engine is
// not, and both engine offsets landed in unrelated code. Each engine site is now
// resolved by signature, with the offset kept as a first guess. Verified
// 2026-08-31 against the Steam engine: both patterns below resolve uniquely to
// engine+0x60CEDD, whose bytes are the same `74 1E` shape at the same relative
// position (cmp rax,-1 / je +0x1E / lea rcx,[rsp+0x40] / call LoadLibraryW).
constexpr uintptr_t kEngineSigGateRva = 0x60C3AD;

// mov rbx,rax / cmp rax,-1 / je / lea rcx,[rsp+..] / call [rip+..] - the
// verification result feeding the branch that guards LoadLibraryW. The second
// pattern drops the leading mov in case the shim's result lands in a different
// register; both are unique in the build we have.
const dlvr::SitePattern kEngineSigGatePats[] = {
    { "48 8B D8 48 83 F8 FF 74 ?? 48 8D 4C 24 ?? FF 15", 7 },
    { "48 83 F8 FF 74 ?? 48 8D 4C 24 ?? FF 15", 4 },
};

constexpr uintptr_t kSigGateRva = 0x4A90D;
const uint8_t kSigGateOrig[2]   = { 0x74, 0x1E };
const uint8_t kSigGatePatch[2]  = { 0x90, 0x90 };

void PatchSignatureGate(uintptr_t base)
{
    if (!g_patchSigCheck) {
        LOG("signature gate: patch disabled by config");
        return;
    }
    auto at = reinterpret_cast<uint8_t*>(base + kSigGateRva);
    if (IsBadReadPtr(at, sizeof(kSigGateOrig))) {
        LOG("signature gate: %p unreadable", static_cast<void*>(at));
        return;
    }
    if (memcmp(at, kSigGatePatch, sizeof(kSigGatePatch)) == 0) {
        LOG("signature gate: already patched");
        return;
    }
    if (memcmp(at, kSigGateOrig, sizeof(kSigGateOrig)) != 0) {
        LOG("signature gate: unexpected bytes %02X %02X (expected %02X %02X) - "
            "build mismatch, NOT patching", at[0], at[1],
            kSigGateOrig[0], kSigGateOrig[1]);
        return;
    }

    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(kSigGatePatch), PAGE_EXECUTE_READWRITE, &old)) {
        LOG("signature gate: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    memcpy(at, kSigGatePatch, sizeof(kSigGatePatch));
    VirtualProtect(at, sizeof(kSigGatePatch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kSigGatePatch));
    LOG("signature gate: PATCHED at rd3d11+%#zx (je -> nop nop)", kSigGateRva);
}

// ------------------------------------------------------ engine Oculus gate ---
//
// Far better than forcing the renderer ourselves: let the engine do it.
//
// engine_x64_rwdi.dll has its own Oculus bring-up at 0x2E5270 - it owns a copy
// of the CAPI shim (0x60C410) and its own ovr_Initialize pointer (0xA3B6E0),
// loads LibOVRRT64_1.dll independently of the renderer, and only then calls the
// renderer's slot 163 and 162:
//
//   0x2E5623  call 0x60C410           ; engine's own shim loader
//   0x2E5633  call [0xA3B6E0]         ; ovr_Initialize
//   0x2E567F  xor  sil, 1             ; sil = init succeeded
//   0x2E5683  mov  [rbx+0x2EE], sil   ; remember it
//   0x2E5692  je   0x2E56E5           ; failed -> don't touch the renderer
//   0x2E569E  call [rax+0x518]        ; slot 163 - the renderer's Oculus init
//   0x2E56B2  call [rax+0x510]        ; slot 162 - fetch the session
//
// The whole thing is gated on one byte at the top of the function:
//
//   0x2E5276  cmp byte ptr [rcx+0x2ED], 0
//   0x2E5280  je  0x2E552A            ; flag clear -> skip everything
//
// That flag is never set, which is why nothing happened however we prodded the
// settings. NOPing the branch makes the engine run its own init, at its own
// time, on its own thread - which is exactly what forcing slot 163 late could
// never achieve. The already-initialised check right after (cmp [rcx+0x28], 0)
// stops it repeating.

constexpr uintptr_t kOculusGateRva = 0x2E5280;
const uint8_t kOculusGatePatch[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

// The je itself carries a rel32 that moves with any code motion, so what is
// matched is the shape around it: the flag test, mov rdi,rcx, the branch, and
// the already-initialised check it jumps over.
//
// Verified 2026-08-31 against the Steam engine: resolves uniquely to
// engine+0x2E5D60, at a clean function boundary (int3 padding right before
// it), with the exact same `cmp [rcx+0x2ED],0 / je / cmp [rcx+0x28],0` shape
// documented for the Epic build.
const dlvr::SitePattern kOculusGatePats[] = {
    { "80 B9 ?? ?? 00 00 00 48 8B F9 0F 84 ?? ?? ?? ?? 48 83 79 ?? 00", 10 },
};

// Which engine build is this? Two stores ship the same game version compiled
// separately, and when a patch site is not where we expect it, the first thing
// worth knowing is whether the binary is the one the offsets came from. The PE
// link timestamp and image size identify a build in one line and cost nothing.
void LogEngineIdentity()
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("engine build: engine_x64_rwdi.dll not mapped yet");
        return;
    }
    const auto base = reinterpret_cast<uintptr_t>(engine);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    LOG("engine build: base %p  image %#lx  linked %#lx  checksum %#lx",
        static_cast<void*>(engine),
        static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage),
        static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
        static_cast<unsigned long>(nt->OptionalHeader.CheckSum));
}

// Same 74 1E -> 90 90 as the renderer's gate, in engine_x64_rwdi.dll.
void PatchEngineSignatureGate()
{
    if (!g_patchSigCheck) {
        return;
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        return;
    }
    // Already done? Checked before resolving, because a patched site no longer
    // matches its own signature and would look like a build we cannot handle.
    auto* hint = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(engine) +
                                            kEngineSigGateRva);
    if (!IsBadReadPtr(hint, 2) && memcmp(hint, kSigGatePatch, 2) == 0) {
        LOG("engine signature gate: already patched");
        return;
    }

    char how[192] = "";
    const uintptr_t rva = dlvr::ResolveSite(
        engine, kEngineSigGateRva, "74 ??", kEngineSigGatePats,
        static_cast<int>(sizeof(kEngineSigGatePats) / sizeof(kEngineSigGatePats[0])),
        how, sizeof(how));
    if (!rva) {
        LOG("engine signature gate: could not locate it (%s) - NOT patching", how);
        return;
    }
    auto at = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(engine) + rva);
    DWORD old = 0;
    if (!VirtualProtect(at, 2, PAGE_EXECUTE_READWRITE, &old)) {
        return;
    }
    memcpy(at, kSigGatePatch, 2);
    VirtualProtect(at, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 2);
    LOG("engine signature gate: PATCHED at engine+%#zx (%s)", rva, how);
}

void PatchEngineOculusGate()
{
    if (!g_patchEngineGate) {
        LOG("engine gate: patch disabled by config");
        return;
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("engine gate: engine not mapped");
        return;
    }
    auto* hint = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(engine) +
                                            kOculusGateRva);
    if (!IsBadReadPtr(hint, sizeof(kOculusGatePatch)) &&
        memcmp(hint, kOculusGatePatch, sizeof(kOculusGatePatch)) == 0) {
        LOG("engine gate: already patched");
        return;
    }

    char how[192] = "";
    const uintptr_t rva = dlvr::ResolveSite(
        engine, kOculusGateRva, "0F 84 ?? ?? ?? ??", kOculusGatePats,
        static_cast<int>(sizeof(kOculusGatePats) / sizeof(kOculusGatePats[0])),
        how, sizeof(how));
    if (!rva) {
        LOG("engine gate: could not locate it (%s) - NOT patching. The engine "
            "will not start its own Oculus init, so VR will not come up.", how);
        return;
    }
    auto at = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(engine) + rva);

    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(kOculusGatePatch), PAGE_EXECUTE_READWRITE, &old)) {
        LOG("engine gate: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    memcpy(at, kOculusGatePatch, sizeof(kOculusGatePatch));
    VirtualProtect(at, sizeof(kOculusGatePatch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kOculusGatePatch));
    LOG("engine gate: PATCHED at engine+%#zx (je -> 6x nop, %s) - the engine will "
        "now run its own Oculus init", rva, how);
}

// A fourth gate, found 2026-09-02 while diagnosing a test build that never
// produced a single VR frame on a friend's machine (SteamVR + Bigscreen
// Beyond) despite all three gates above patching successfully and the engine
// genuinely running - dlvr_ovr2xr.log was simply never created, meaning our
// own translation layer was never even loaded.
//
// A few instructions after the gate above, still inside the same function,
// the engine checks a global "current renderer" pointer and makes a virtual
// call through it (vtable slot +0x30) before doing anything else. That
// pointer's RTTI resolves to CNullRenderer - a placeholder object the engine
// apparently installs before the real D3D11 renderer (rd3d11_x64_rwdi.dll)
// has created a device and replaced it - and the method at that slot is a
// two-instruction stub (`mov al,1 / ret`) that always returns true. So the
// check is really an is-a test: "is the active renderer still the null
// placeholder?" - and if so, this jne bails out of Oculus bring-up entirely,
// silently, with no log line anywhere.
//
// This looks like a startup race rather than anything SteamVR-specific: if
// this function runs before rd3d11 finishes standing up the real renderer,
// it sees the placeholder and gives up for good (nothing here suggests it
// ever gets a second attempt). Whatever makes VR runtime startup slower
// relative to renderer creation - a slower compositor handshake, a headset
// that takes longer to hand off, anything - could plausibly lose this race
// even though it reliably wins on the dev machine's Virtual Desktop setup.
// NOT reproduced locally - this is the same "NOP the branch, let the engine
// proceed regardless" treatment as the gate above, on a check that has never
// been seen to matter when it does proceed.
constexpr uintptr_t kRendererRaceGateRva = 0x2E6078;
const uint8_t kRendererRaceGatePatch[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

// Matched on the shape leading up to the jne, since the jne's own rel32
// moves with code motion: load the CNullRenderer-or-real-renderer pointer,
// null-check it, the vtable load, the virtual call through slot +0x30, and
// the al result test. Verified 2026-09-02 against the Steam engine (the same
// binary confirmed byte-identical - checksum 0xae1348 - on both the dev
// machine and the machine that reproduced the stall).
const dlvr::SitePattern kRendererRaceGatePats[] = {
    { "48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 FF 50 30 84 C0", 24 },
};

void PatchEngineRendererRaceGate()
{
    if (!g_patchRendererRaceGate) {
        LOG("renderer race gate: patch disabled by config");
        return;
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("renderer race gate: engine not mapped");
        return;
    }
    // The hint is the Steam rva, or the EGL one on the Epic build (common/buildrva.h): the pattern matches TWICE in both
    // builds, so the hint is the only route that resolves, and ResolveSite still checks its bytes.
    const uintptr_t hintRva = ::dlvr::buildrva::Eng(kRendererRaceGateRva);
    auto* hint = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(engine) + hintRva);
    if (!IsBadReadPtr(hint, sizeof(kRendererRaceGatePatch)) &&
        memcmp(hint, kRendererRaceGatePatch, sizeof(kRendererRaceGatePatch)) == 0) {
        LOG("renderer race gate: already patched");
        return;
    }

    char how[192] = "";
    const uintptr_t rva = dlvr::ResolveSite(
        engine, hintRva, "0F 85 ?? ?? ?? ??", kRendererRaceGatePats,
        static_cast<int>(sizeof(kRendererRaceGatePats) / sizeof(kRendererRaceGatePats[0])),
        how, sizeof(how));
    if (!rva) {
        LOG("renderer race gate: could not locate it (%s) - NOT patching", how);
        return;
    }
    auto at = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(engine) + rva);

    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(kRendererRaceGatePatch), PAGE_EXECUTE_READWRITE, &old)) {
        LOG("renderer race gate: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    memcpy(at, kRendererRaceGatePatch, sizeof(kRendererRaceGatePatch));
    VirtualProtect(at, sizeof(kRendererRaceGatePatch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kRendererRaceGatePatch));
    LOG("renderer race gate: PATCHED at engine+%#zx (jne -> 6x nop, %s) - Oculus "
        "bring-up can no longer bail out because the real renderer hasn't "
        "replaced the null placeholder yet", rva, how);
}

// ------------------------------------------------------- rd3d11 gate watchdog
//
// rd3d11_x64_rwdi.dll carries its own copy of the Authenticode gate, and it has
// to be patched before the engine calls the renderer's Oculus init - otherwise
// slot 163 refuses to load our unsigned LibOVRRT64_1.dll, returns false, and the
// engine quietly skips VR. But rd3d11 is loaded dynamically, so it does not
// exist yet when our DllMain runs.
//
// Observed timing in this build:
//   +0.42s  rd3d11 loaded
//   +0.68s  engine calls its Oculus init  ->  slot 163
//
// A quarter of a second is an eternity for a 1 ms poll, and a watchdog thread
// avoids depending on which loader API the engine happens to use - we already
// learned the hard way that its GetProcAddress calls do not come through the
// import thunks we can see.

void PatchSignatureGate(uintptr_t base);

DWORD WINAPI Rd3d11Watchdog(LPVOID)
{
    for (int i = 0; i < 120000; ++i) {  // ~2 minutes at 1 ms
        HMODULE rd = GetModuleHandleA("rd3d11_x64_rwdi.dll");
        if (rd) {
            LOG("rd3d11 watchdog: module appeared after %d ms", i);
            // BEFORE the gate patch, deliberately: rd3d11 creates its D3D11
            // device during its own bring-up, so the import has to be patched at
            // the earliest moment the module exists. No-ops unless
            // dlvr\dlss_flat.txt is present.
            ::dlvr::dlssflat::InstallFromWatchdog(rd);
            ::InstallEarlyEyeSizeHookOn(rd);   // early_eye_size: rd3d11's own filesystem imports; no-op without the marker
            PatchSignatureGate(reinterpret_cast<uintptr_t>(rd));
            return 0;
        }
        Sleep(1);
    }
    LOG("rd3d11 watchdog: gave up waiting for the renderer DLL");
    return 0;
}

void StartRd3d11Watchdog()
{
    HANDLE t = CreateThread(nullptr, 0, &Rd3d11Watchdog, nullptr, 0, nullptr);
    if (t) {
        CloseHandle(t);
        LOG("rd3d11 watchdog: started");
    } else {
        LOG("rd3d11 watchdog: CreateThread failed (%lu)", GetLastError());
    }
}

// ------------------------------------------------- GetProcAddress interception
//
// The engine resolves dinput8!DirectInput8Create through GetProcAddress; this
// hook wraps it for the mod-menu input block. (The name InstallCreateRendererHook
// is historical: the CreateRenderer wrap was removed 2026-11-15, batch 3.)

using PFN_GetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);

PFN_GetProcAddress g_realGetProcAddress = nullptr;

// Mod-menu input block: the engine resolves dinput8!DirectInput8Create through THIS hook (CInput::Initialize,
// engine +27E146/+27E150 - LoadLibrary "dinput8.dll" then GetProcAddress through the engine's IAT slot +7E46D0).
namespace menublock {
FARPROC WrapDirectInput8Create(FARPROC real);
void FilterSdlPadEvent(void* event);   // HookedSdlPollEvent - the controller's real route (run 5)
}

FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR name)
{
    FARPROC real = g_realGetProcAddress ? g_realGetProcAddress(module, name)
                                        : GetProcAddress(module, name);
    if (IS_INTRESOURCE(name)) {
        return real;
    }
    if (real && strcmp(name, "DirectInput8Create") == 0) {
        return menublock::WrapDirectInput8Create(real);
    }
    return real;
}

void InstallCreateRendererHook()
{
    // Patch every module that imports GetProcAddress, not just the engine.
    // engine_x64_rwdi.dll holds the "CreateRenderer" string, but that does not
    // guarantee the call goes through its own import thunk.
    const char* modules[] = {
        "engine_x64_rwdi.dll", "filesystem_x64_rwdi.dll", "gamedll_x64_rwdi.dll",
        "imagelib_x64_rwdi.dll", "DyingLightGame.exe",
    };
    int patched = 0;
    for (const char* m : modules) {
        HMODULE h = GetModuleHandleA(m);
        if (!h) {
            LOG("CreateRenderer hook: %s not mapped yet", m);
            continue;
        }
        void* orig = ::dlvr::PatchIat(h, "KERNEL32.dll", "GetProcAddress",
                                      reinterpret_cast<void*>(&GetProcAddressHook));
        if (orig) {
            if (!g_realGetProcAddress) {
                g_realGetProcAddress = reinterpret_cast<PFN_GetProcAddress>(orig);
            }
            ++patched;
            LOG("CreateRenderer hook: patched %s (orig=%p)", m, orig);
        }
    }
    LOG("CreateRenderer hook: %d module(s) patched", patched);
}

// -------------------------------------------------- video.scr OculusEnabled --
//
// The engine's own dormant Oculus VR mode never activates unless
// OculusEnabled() appears in the ACTIVE (non-"!"-prefixed) block of the
// user's own video.scr - the "!"-prefixed lines near the top of that file
// are just a reference/documentation list and do nothing on their own. This
// is easy to miss on a first setup, is not something any of our own gate
// patches touch, and the game can silently drop the line again after some
// in-game settings changes - so this runs on every launch rather than being
// a one-time manual step the user has to remember.
//
// A /video=<our own file> command-line redirect was tried first, specifically
// to avoid ever touching the user's real settings file - abandoned (see
// dlvr/extra_args.txt's own note): the engine's virtual filesystem
// (fs::exists) rejects that argument's path in both absolute and
// game-relative form. Editing the real file directly, in place, is the only
// approach found so far that actually works.
void EnsureVideoScrHasOculusEnabled()
{
    // DIAGNOSTIC SKIP - marker `dlvr\no_oculus_scr.txt`. FLATSCREEN ONLY.
    //
    // WHY THIS EXISTS. Three RenderDoc captures on 2026-09-12 - including one
    // taken with a DELAYED capture during a continuous max-sensitivity mouse
    // spin, so no keypress could have interrupted the motion - found the
    // engine's motion-vector buffer carrying ONLY object motion. An animated
    // NPC's hands read 0.00009/0.00042 while the entire static world read flat
    // zero through a full camera spin.
    //
    // That matters because DLSS reconstructs from camera reprojection above
    // all else, and it explains the symptoms exactly: real AA, but soft
    // textures and unstable ground, because history is rejected the moment the
    // view moves.
    //
    // THE ONE EXPLICIT OCULUS-DISABLING TERM in the whole chain is in the
    // shipped frame_script.scr, comment and all:
    //
    //     //Disable blur motion while oculus rift is enabled
    //     blur_motion_object_on = f_blur_motion_object * (1 - i_pp_OculusEnable);
    //
    // and the motion vectors exist FOR motion blur - ENGINE_ARCHITECTURE.md
    // already records that SMAA T2x never needed reprojection. So our own
    // requirement for VR is the prime suspect for switching off the input DLSS
    // most needs.
    //
    // AND IT BEARS ON A DIRECT CONTRADICTION: HANDOFF_DLSS_2026-09-11.md
    // records Step 0 measuring a PANNING capture at -0.02698 on this same
    // buffer, described as camera motion. Today's captures say otherwise. One
    // of those is wrong, and **the whole DLSS thread rests on Step 0's claim.**
    //
    // Suppressing the line lets a flat capture answer it. FLAT ONLY - without
    // OculusEnabled() in video.scr the VR path does not come up at all.
    {
        char dir[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, dir, sizeof(dir));
        if (char* slash = strrchr(dir, '\\')) { *slash = 0; }
        char marker[MAX_PATH] = {};
        _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\dlvr\\no_oculus_scr.txt", dir);
        if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) {
            LOG("video.scr fix: SKIPPED - dlvr\\no_oculus_scr.txt present. OculusEnabled() will "
                "NOT be added, so i_pp_OculusEnable should read 0 and the motion-blur chain "
                "should be live. *** FLATSCREEN ONLY: VR will not start without it. *** "
                "Delete the marker to restore normal behaviour.");
            return;
        }
    }

    PWSTR docsW = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docsW)) || !docsW) {
        LOG("video.scr fix: could not resolve the Documents folder");
        if (docsW) {
            CoTaskMemFree(docsW);
        }
        return;
    }
    char docs[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, docsW, -1, docs, sizeof(docs), nullptr, nullptr);
    CoTaskMemFree(docsW);

    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\DyingLight\\out\\settings\\video.scr", docs);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        // Very first launch, before the game has ever created its own
        // defaults - nothing to fix yet. Will catch it on the next launch,
        // once the game has generated the file itself.
        LOG("video.scr fix: %s not found yet - skipped", path);
        return;
    }
    char content[16384] = {};
    const size_t n = fread(content, 1, sizeof(content) - 1, f);
    fclose(f);
    content[n] = '\0';

    // A line that, once leading whitespace is skipped, is exactly
    // "OculusEnabled()" - not merely containing that text, so the reference
    // block's own "!OculusEnabled()" line (a different string) never counts
    // as already active.
    bool alreadyActive = false;
    char* ctx = nullptr;
    for (char* line = strtok_s(content, "\r\n", &ctx); line; line = strtok_s(nullptr, "\r\n", &ctx)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (strcmp(p, "OculusEnabled()") == 0) {
            alreadyActive = true;
            break;
        }
    }
    if (alreadyActive) {
        LOG("video.scr fix: OculusEnabled() already active - nothing to do");
        return;
    }

    if (fopen_s(&f, path, "a") != 0 || !f) {
        LOG("video.scr fix: could not open %s for appending", path);
        return;
    }
    fprintf(f, "\nOculusEnabled()\n");
    fclose(f);
    LOG("video.scr fix: appended OculusEnabled() to %s", path);
}

// While the in-headset overlay is open, the game must not also see the pad.
//
// We are the game's xinput1_3.dll, so we are already in the path of every pad
// read it makes - which makes this the natural place to gate it. Without the
// gate, navigating the menu also drives the player, and since the default
// binding is L3+R3 (crouch and melee on this game) merely opening the overlay
// would swing a weapon.
//
// The translation layer owns the overlay, so it owns the answer; we ask it,
// resolving late because it loads long after we do. If it is absent or older
// than this feature, the query fails once and we forward input as before.
// XINPUT_STATE's layout, declared here rather than by including <Xinput.h>.
//
// The exports below take void* on purpose: including the real header declares
// XInputGetState with XINPUT_STATE* and the two cannot coexist, because C
// linkage admits no overloads. We only need to write a neutral state, so the
// layout is all we need and the header stays out.
struct XInputStateLayout {
    DWORD dwPacketNumber;
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
static_assert(sizeof(XInputStateLayout) == 16, "XINPUT_STATE is 16 bytes");

void FilterGamepad(void* state)
{
    typedef void(*PFN_Filter)(void*);
    static PFN_Filter filter = nullptr;
    static bool looked = false;
    if (!looked) {
        HMODULE m = GetModuleHandleA("LibOVRRT64_1.dll");
        if (m) {
            looked = true;   // only settle once the layer is actually loaded
            filter = reinterpret_cast<PFN_Filter>(GetProcAddress(m, "dlvr_FilterGamepad"));
            LOG("gamepad filter: %s", filter ? "available" : "not exported by the layer");
        }
    }
    if (filter) {
        filter(state);
    }
}

// The engine's real gamepad reads go through Sony's cross-platform pad SDK
// (libScePad_x64.dll's scePadReadState), not XInput - XInputGetState above
// exists purely for the DLL load-order trick and is confirmed (2026-09-02,
// zero "gamepad filter" log lines across a real play session, including a
// genuine physical controller button press) to never be called for actual
// gameplay input. See CLAUDE.md's scePad section for how this was found and
// verified against this build's own disassembly.
//
// ScePadData layout (offsets confirmed against this build's own parsing
// code, not just Sony's public headers):
//   +0x00 u32  buttons bitmask
//   +0x04 u8   left stick X    +0x05 u8 left stick Y
//   +0x06 u8   right stick X   +0x07 u8 right stick Y
//   +0x08 u8   L2 analog       +0x09 u8 R2 analog
// scePadReadState(int32_t handle, ScePadData* pData) -> int32_t (0 = success).
typedef int32_t(*PFN_ScePadReadState)(int32_t, void*);
PFN_ScePadReadState g_origScePadReadState = nullptr;
// scePadOpen(userId, type, index, param) -> handle >= 0 or an error < 0. The game's libScePad reads the first three (users 1..4,
// type / index 0); the engine passes a 4th, 0, forwarded unchanged.
typedef int32_t(*PFN_ScePadOpen)(int32_t, int32_t, int32_t, const void*);
PFN_ScePadOpen g_origScePadOpen = nullptr;

// ---- VR input into ONE pad handle (docs/superpowers/specs/2026-10-21-scepad-one-handle-design.md, option B; Jay "Go ahead") ----
// [LOADBEARING: SCEPAD-ONE-HANDLE] the walking-hitch fix (INV-604). The engine owns four Sony pads (users 1..4 = its device
// indices 4..7) and reads each once per input pass. Filtering every read put the VR controller on all four, so the game's
// "last touched controller" cycled 4 -> 5 -> 6 -> 7 on every stick / button event and re-ran its button-prompt refresh inside the
// game update. Now the VR state lands in ONE handle's buffer; the other three keep exactly what Sony's library returned
// (neutral with no pad attached). The filter still runs on EVERY read, in the same order, on the same input bytes (a scratch
// copy for the other handles), so nothing inside it - ticks, timers, state machines, counters, logs - sees a different cadence.
// Which handle: the lowest user whose last read had a PHYSICAL pad attached (the library's connected byte), else user 1, else the
// lowest user that opened. Fail-safe = the old behaviour (every read filtered in place): no open recorded, or the marker
// dlvr\scepad_all_handles.txt.
// COST per read: a 4-entry handle compare, two volatile loads; for the three other handles an 88-byte copy.
constexpr int kPadUsers = 4;
constexpr size_t kPadDataBytes = 88;       // what libScePad's scePadReadState writes (it clears 0x58 first); engine buffers are 104
constexpr size_t kPadConnectedOff = 76;    // libScePad: 1 only while a physical pad is attached to the slot
volatile LONG g_padHandle[kPadUsers + 1] = { -1, -1, -1, -1, -1 };   // [user] = its handle; [0] unused
volatile LONG g_padConnected[kPadUsers + 1] = {};
volatile LONG g_padAllHandles = 0;
volatile LONG g_padVrUser = -1;            // the user carrying the VR input; 0 = fail-safe (all), -1 = not decided yet
volatile LONG64 g_padReads[kPadUsers + 2] = {};   // per user; [5] = a handle no open recorded
volatile LONG64 g_padReadsTotal = 0;
constexpr LONG64 kPadCountAfter = 4000;

int PadUserOf(int32_t handle)
{
    for (int u = 1; u <= kPadUsers; ++u) {
        if (g_padHandle[u] >= 0 && g_padHandle[u] == handle) { return u; }
    }
    return 0;
}

int PadChooseVrUser()
{
    if (g_padAllHandles) { return 0; }
    for (int u = 1; u <= kPadUsers; ++u) {
        if (g_padHandle[u] >= 0 && g_padConnected[u]) { return u; }
    }
    for (int u = 1; u <= kPadUsers; ++u) {
        if (g_padHandle[u] >= 0) { return u; }
    }
    return 0;
}

int32_t __cdecl HookedScePadOpen(int32_t user, int32_t type, int32_t index, const void* param)
{
    const int32_t h = g_origScePadOpen ? g_origScePadOpen(user, type, index, param) : -1;
    if (user >= 1 && user <= kPadUsers && h >= 0) {
        InterlockedExchange(&g_padHandle[user], h);
    }
    LOG("scepad open: user %d -> %s %d (0x%08X)", user, h >= 0 ? "handle" : "ERROR", h, static_cast<unsigned>(h));
    return h;
}

void FilterScePad(void* pData)
{
    typedef void(*PFN_Filter)(void*);
    static PFN_Filter filter = nullptr;
    static bool looked = false;
    if (!looked) {
        HMODULE m = GetModuleHandleA("LibOVRRT64_1.dll");
        if (m) {
            looked = true;
            filter = reinterpret_cast<PFN_Filter>(GetProcAddress(m, "dlvr_FilterScePad"));
            LOG("scepad filter: %s", filter ? "available" : "not exported by the layer");
        }
    }
    if (filter) {
        filter(pData);
    }
}

int32_t __cdecl HookedScePadReadState(int32_t handle, void* pData)
{
    const int32_t r = g_origScePadReadState ? g_origScePadReadState(handle, pData) : -1;
    if (r != 0 || !pData) {
        return r;
    }
    const int user = PadUserOf(handle);
    if (user > 0) {
        const LONG conn = static_cast<const unsigned char*>(pData)[kPadConnectedOff] ? 1 : 0;
        if (g_padConnected[user] != conn) {
            InterlockedExchange(&g_padConnected[user], conn);
            LOG("scepad: user %d physical pad %s", user, conn ? "CONNECTED" : "not connected (or removed)");
        }
    }
    const int vr = PadChooseVrUser();
    if (g_padVrUser != vr) {
        InterlockedExchange(&g_padVrUser, vr);
        if (vr == 0) {
            LOG("scepad: VR input on EVERY pad handle (the old behaviour) - %s",
                g_padAllHandles ? "marker dlvr\\scepad_all_handles.txt present"
                                : "FAIL-SAFE: no scePadOpen was recorded, so the handles cannot be told apart");
        } else {
            LOG("scepad: VR input on user %d (handle %d) only - %s; the other pads read the library's own state",
                vr, static_cast<int>(g_padHandle[vr]),
                g_padConnected[vr] ? "a physical pad is connected on it" : "no physical pad connected");
        }
    }
    if (vr > 0 && user == vr && !static_cast<unsigned char*>(pData)[kPadConnectedOff]) {
        // [LOADBEARING: PADRUMBLE] the engine sends motor values only to a pad device reporting connected (OUR engine +2FC1A0 tests
        //      COrbisPadDevice slot 4 = device+137, which +2FC580 copies from this byte). With no physical pad libScePad says 0 and
        //      every game rumble is dropped. The VR user's pad is reported connected; the other three keep the library's value.
        static_cast<unsigned char*>(pData)[kPadConnectedOff] = 1;
        static volatile LONG s_forced = 0;
        if (s_forced == 0 && InterlockedExchange(&s_forced, 1) == 0) {
            // [PROBE: PADRUMBLE] gate=none (always on)  site=FLAT  log-only=no
            LOG("scepad: [PADRUMBLE] user %d (handle %d) reported CONNECTED to the engine (logged unconditionally, first time) - "
                "no physical pad, so libScePad said 0 and the engine would drop every rumble", user, static_cast<int>(handle));
        }
    }
    if (vr == 0 || user == vr) {
        FilterScePad(pData);
    } else {
        unsigned char scratch[kPadDataBytes];
        memcpy(scratch, pData, kPadDataBytes);
        FilterScePad(scratch);
    }
    InterlockedIncrement64(&g_padReads[user > 0 ? user : kPadUsers + 1]);
    if (InterlockedIncrement64(&g_padReadsTotal) == kPadCountAfter) {
        LOG("scepad: first %lld reads by pad - user 1: %lld, user 2: %lld, user 3: %lld, user 4: %lld, unrecorded handle: %lld",
            static_cast<long long>(kPadCountAfter), static_cast<long long>(g_padReads[1]), static_cast<long long>(g_padReads[2]),
            static_cast<long long>(g_padReads[3]), static_cast<long long>(g_padReads[4]),
            static_cast<long long>(g_padReads[5]));
    }
    return r;
}

// ---- Game rumble -> Touch haptics (spec docs/superpowers/specs/2026-10-25-game-rumble-to-touch-haptics-design.md) ----
// The engine's Sony pad device (+2FCD10, OUR engine) sends {large, small} motor bytes through scePadSetVibration on every motor
// change (and re-sends while an apply keeps failing). The original call always runs (a real DualSense keeps rumbling); for the VR
// user's handle - or the fail-safe all-handles mode, or before the user is decided - the two bytes also go to the layer's
// dlvr_PadVibration, which drives both Touch controllers. No game rva: an import-table swap by name, Steam and Epic alike.
// COST: event-driven (motor changes only); one cached GetProcAddress.
// Pad TYPE (2026-10-25, Jay: "1"): reporting the VR pad connected (HookedScePadReadState) made the engine's pad-type query
// (+2FC320: connected ? device slot 18 : 0) return COrbisPadDevice's 4 = EDevice_DualShock4 (OUR gamedll +CCCAE0: 0 None,
// 1 Xbox360, 2 XboxOne, 3 DualShock3, 4 DualShock4, 6 WiiU), so the game drew Sony button icons. Slot 18 now returns 1 = Xbox360,
// what OUR engine's own XInput pad (CXPadDevice slot 18) returns. Rumble is unaffected: forcefeedback.scr has no Xbox360 or
// DualShock4 block, both use the default Device() effects (XboxOne = 2 would have emptied them). The vtable is found by its RTTI
// name, so no game rva - Steam, Epic and GOG alike.
unsigned int OrbisPadTypeXbox360(void*)
{
    return 1;
}

// engine_x64_rwdi.dll's vtable of RTTI class `decorated` (".?AVName@@"), or 0. Same walk as the layer's rtticam.cpp
// TryVtableInModule: type descriptor by name -> the x64 locator naming it -> the qword pointing at the locator, followed by code
// pointers. One-time image scan (install only). Plain types only, for the __try.
uintptr_t EngineVtableByRtti(HMODULE engine, const char* decorated)
{
    __try {
        const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const size_t size = nt->OptionalHeader.SizeOfImage;
        const uint8_t* img = reinterpret_cast<const uint8_t*>(base);
        const size_t len = strlen(decorated) + 1;   // with the terminator: an exact name, not a prefix
        size_t td = 0;
        for (size_t off = 16; off + len <= size; ++off) {
            if (img[off] == '.' && memcmp(img + off, decorated, len) == 0) {
                td = off - 16;
                break;
            }
        }
        if (!td) {
            return 0;
        }
        size_t col = 0;
        for (size_t off = 0; off + 24 <= size; off += 4) {
            const auto* p = reinterpret_cast<const uint32_t*>(img + off);
            if (p[3] == td && p[0] == 1 && p[1] == 0) {
                col = off;
                break;
            }
        }
        if (!col) {
            return 0;
        }
        for (size_t off = 0; off + 40 <= size; off += 8) {
            if (*reinterpret_cast<const uintptr_t*>(img + off) != base + col) {
                continue;
            }
            const auto* slots = reinterpret_cast<const uintptr_t*>(img + off + 8);
            int good = 0;
            for (int k = 0; k < 4; ++k) {
                good += (slots[k] > base && slots[k] < base + size) ? 1 : 0;
            }
            if (good >= 3) {
                return reinterpret_cast<uintptr_t>(slots);
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void InstallOrbisPadType(HMODULE engine)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
    const uintptr_t vt = EngineVtableByRtti(engine, ".?AVCOrbisPadDevice@@");
    if (!vt) {
        // [PROBE: PADRUMBLE] gate=none (always on)  site=FLAT  log-only=no
        LOG("scepad hook: [PADRUMBLE] pad type NOT changed - no COrbisPadDevice vtable found by RTTI in the engine; the game may "
            "show Sony button icons");
        return;
    }
    void** slot = reinterpret_cast<void**>(vt + 18 * sizeof(void*));
    const unsigned char* fn = static_cast<const unsigned char*>(*slot);
    static const unsigned char kReturn4[6] = { 0xB8, 0x04, 0x00, 0x00, 0x00, 0xC3 };   // mov eax, 4 / ret
    if (memcmp(fn, kReturn4, sizeof(kReturn4)) != 0) {
        // [PROBE: PADRUMBLE] gate=none (always on)  site=FLAT  log-only=no
        LOG("scepad hook: [PADRUMBLE] pad type NOT changed - COrbisPadDevice vtable +%zX slot 18 points at %p, whose bytes are "
            "%02X %02X %02X %02X %02X %02X, not mov eax,4 / ret - a different engine build", vt - base, static_cast<const void*>(fn),
            fn[0], fn[1], fn[2], fn[3], fn[4], fn[5]);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        // [PROBE: PADRUMBLE] gate=none (always on)  site=FLAT  log-only=no
        LOG("scepad hook: [PADRUMBLE] pad type NOT changed - VirtualProtect on the COrbisPadDevice vtable failed");
        return;
    }
    *slot = reinterpret_cast<void*>(&OrbisPadTypeXbox360);
    VirtualProtect(slot, sizeof(void*), old, &old);
    // [PROBE: PADRUMBLE] gate=none (always on)  site=FLAT  log-only=no
    LOG("scepad hook: [PADRUMBLE] pad type: COrbisPadDevice vtable +%zX slot 18 (was %p, returns 4 = DualShock4) now returns 1 = "
        "Xbox360 - Xbox button icons", vt - base, static_cast<const void*>(fn));
}

typedef int32_t(__cdecl* PFN_ScePadSetVibration)(int32_t handle, const void* param);
PFN_ScePadSetVibration g_origScePadSetVibration = nullptr;

int32_t __cdecl HookedScePadSetVibration(int32_t handle, const void* param)
{
    const int32_t r = g_origScePadSetVibration ? g_origScePadSetVibration(handle, param) : -1;
    const unsigned char* v = static_cast<const unsigned char*>(param);
    const int user = PadUserOf(handle);
    const LONG vr = g_padVrUser;
    const bool forward = v && (vr <= 0 || user == vr);
    typedef void(*PFN_Vib)(int, int);
    static PFN_Vib vib = nullptr;
    static bool looked = false;
    if (!looked) {
        HMODULE m = GetModuleHandleA("LibOVRRT64_1.dll");
        if (m) {
            looked = true;
            vib = reinterpret_cast<PFN_Vib>(GetProcAddress(m, "dlvr_PadVibration"));
        }
    }
    static volatile LONG s_first = 0;
    if (s_first == 0 && InterlockedExchange(&s_first, 1) == 0) {
        // [PROBE: PADRUMBLE] gate=none (always on)  site=HEADSET  log-only=no
        LOG("scepad: [PADRUMBLE] scePadSetVibration FIRST CALL (logged unconditionally) - handle %d user %d, VR user %ld, large %d "
            "small %d, library returned %d -> %s", static_cast<int>(handle), user, static_cast<long>(vr), v ? v[0] : -1,
            v ? v[1] : -1, static_cast<int>(r),
            !forward ? "NOT forwarded (another pad's handle)"
                     : (vib ? "forwarded to the Touch controllers" : "NOT forwarded - the layer does not export dlvr_PadVibration"));
    }
    if (forward && vib) {
        vib(v[0], v[1]);
    }
    return r;
}

// Ordinary IAT swap - engine_x64_rwdi.dll's own import of
// libScePad_x64.dll!scePadReadState, same PatchIat() already used for the
// engine log hooks and the CreateRenderer hook above. No exec breakpoints,
// no VEH: this is a plain function-pointer replacement.
void InstallScePadHook()
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("scepad hook: engine_x64_rwdi.dll not mapped yet - skipped");
        return;
    }
    void* orig = ::dlvr::PatchIat(engine, "libScePad_x64.dll", "scePadReadState",
                                  reinterpret_cast<void*>(&HookedScePadReadState));
    if (orig) {
        g_origScePadReadState = reinterpret_cast<PFN_ScePadReadState>(orig);
        LOG("scepad hook: scePadReadState -> %p", orig);
    } else {
        LOG("scepad hook: scePadReadState NOT FOUND in engine's imports");
    }
    // [LOADBEARING: PADRUMBLE] the game's rumble reaches the Touch controllers only through this swap (spec 2026-10-25-game-rumble-
    //      to-touch-haptics-design); without it every game vibration goes to libScePad with no pad behind it.
    void* origVib = ::dlvr::PatchIat(engine, "libScePad_x64.dll", "scePadSetVibration",
                                     reinterpret_cast<void*>(&HookedScePadSetVibration));
    if (origVib) {
        g_origScePadSetVibration = reinterpret_cast<PFN_ScePadSetVibration>(origVib);
        // [PROBE: PADRUMBLE] gate=none (always on)  site=FLAT  log-only=no
        LOG("scepad hook: [PADRUMBLE] scePadSetVibration -> %p - the game's rumble also goes to the Touch controllers", origVib);
    } else {
        // [PROBE: PADRUMBLE] gate=none (always on)  site=FLAT  log-only=no
        LOG("scepad hook: [PADRUMBLE] scePadSetVibration NOT FOUND in engine's imports - no game rumble reaches the controllers");
    }
    // [LOADBEARING: PADRUMBLE] Xbox button icons while the VR pad is reported connected (InstallOrbisPadType's comment).
    InstallOrbisPadType(engine);
    {
        char dir[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, dir, sizeof(dir));
        if (char* slash = strrchr(dir, '\\')) { *slash = 0; }
        char marker[MAX_PATH] = {};
        _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\dlvr\\scepad_all_handles.txt", dir);
        g_padAllHandles = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
    }
    void* origOpen = ::dlvr::PatchIat(engine, "libScePad_x64.dll", "scePadOpen",
                                      reinterpret_cast<void*>(&HookedScePadOpen));
    if (origOpen) {
        g_origScePadOpen = reinterpret_cast<PFN_ScePadOpen>(origOpen);
    }
    LOG("scepad hook: scePadOpen %s | VR input %s", origOpen ? "hooked" : "NOT FOUND in engine's imports",
        g_padAllHandles ? "on EVERY pad (marker dlvr\\scepad_all_handles.txt present)"
                        : (origOpen ? "on one pad: a connected physical pad's, else user 1's"
                                    : "on EVERY pad (FAIL-SAFE: the opens cannot be recorded)"));
}

// Mouse-look pitch policy - same split as FilterScePad above: mechanics here,
// policy (dlvr_FilterSdlEvent) in the layer, where block_mouse_pitch's
// setting lives. engine_x64_rwdi.dll imports SDL2.dll!SDL_PollEvent directly
// (confirmed via its own import table - no DirectInput/RawInput/GetCursorPos
// import exists at all), so an IAT swap works the same way as scePadReadState
// above - plain function-pointer replacement, no exec breakpoints.
typedef int(*PFN_SDL_PollEvent)(void*);
PFN_SDL_PollEvent g_origSdlPollEvent = nullptr;

void FilterSdlEvent(void* event)
{
    typedef void(*PFN_Filter)(void*);
    static PFN_Filter filter = nullptr;
    static bool looked = false;
    if (!looked) {
        HMODULE m = GetModuleHandleA("LibOVRRT64_1.dll");
        if (m) {
            looked = true;
            filter = reinterpret_cast<PFN_Filter>(GetProcAddress(m, "dlvr_FilterSdlEvent"));
            LOG("sdl mouse filter: %s", filter ? "available" : "not exported by the layer");
        }
    }
    if (filter) {
        filter(event);
    }
}

// Every polled event passes through the mouse filter and the mod-menu pad
// filter. (The 2026-09-02 heartbeat / event-type catalogue that proved this
// hook is reached was removed 2026-11-15, batch 3 - the question is answered.)
int HookedSdlPollEvent(void* event)
{
    const int r = g_origSdlPollEvent ? g_origSdlPollEvent(event) : 0;
    if (r == 1 && event) {
        FilterSdlEvent(event);
        menublock::FilterSdlPadEvent(event);
    }
    return r;
}

void InstallSdlPollEventHook()
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("sdl mouse hook: engine_x64_rwdi.dll not mapped yet - skipped");
        return;
    }
    void* orig = ::dlvr::PatchIat(engine, "SDL2.dll", "SDL_PollEvent",
                                  reinterpret_cast<void*>(&HookedSdlPollEvent));
    if (orig) {
        g_origSdlPollEvent = reinterpret_cast<PFN_SDL_PollEvent>(orig);
        LOG("sdl mouse hook: SDL_PollEvent -> %p", orig);
    } else {
        LOG("sdl mouse hook: SDL_PollEvent NOT FOUND in engine's imports");
    }
}

// ------------------------------------------------ mod-menu input block ----
//
// Mechanics only - the layer's dlvr_MenuInputPolicy decides (0 pass, 1 block, 3 block but the right stick passes)
// and logs which paths were live; see its comment for the static read behind each hook. Plain IAT swaps, the same
// PatchIat as scePadReadState above: the engine's Xbox-pad reads (SDL GameController - SDL calls XInput inside
// SDL2.dll, past our xinput1_3 proxy), its keyboard reads, and every window-message pump in the game's modules
// (exe + engine PeekMessageA, SDL2 PeekMessageW / GetMessageW). Our own key reads are untouched: the overlay polls
// through the LAYER's imports and dlssflat's F10 through this DLL's, neither of which is patched.
// COST per hooked call: the original call, then one cached function-pointer call that loads a bool.
namespace menublock {

enum Path {
    kSdlButton = 1, kSdlAxis = 2, kPeekExe = 3, kPeekEngine = 4, kPeekSdlW = 5, kGetMsgSdlW = 6,
    kKbState = 7, kAsyncKey = 8, kKeyState = 9,
    kDiKbState = 10, kDiKbData = 11, kDiCtlState = 12, kDiCtlData = 13,   // DirectInput (below); names in the layer
    kKeyQueue = 14,   // the engine's key-event queue, +2A360 (below)
    kSdlPadEvent = 15,   // SDL controller / joystick EVENTS through the engine's SDL_PollEvent (below)
    kJoyAxis = 16, kJoyButton = 17,   // CNewInputManager::GetJoyAxisState / GetJoyButtonState (below)
};

// ---- the CONTROLLER (run 5, 2026-10-13: Jay "Keyboard no longer gets through, Controller still does though") ----
// The pad never touched the SDL GameController calls, XInput, DirectInput or scePad. IDA (OUR engine, mb2_engine_out.txt):
// CJoystickSDL slot 1 (+2BCC30) drains 56-byte SDL events from g_SdlManager's queue (types 1619 / 1620 = controller
// added / removed); that queue is filled by the engine's own SDL_PollEvent (+667929 / +667A5D, next to the g_SdlManager
// ctor +667720) - the import the bootstrap ALREADY hooks for block_mouse_pitch. Run 5's log: at 16:22:44.485 that hook
// saw 0x653 controller added, then 0x650 / 0x651 / 0x652 (controller axis / button down / up) and 0x600 / 0x603 / 0x604
// (joystick axis / button down / up). While the menu is open the event is REWRITTEN, never dropped, so nothing sticks:
//   axis motion (0x650, 0x600)  -> value 0 (centred / trigger released); LT held (policy 3) passes controller axes 2 / 3
//   button down (0x651, 0x603)  -> button up (0x652, 0x604), state 0
//   hat (0x602) -> centred (0); ball (0x601) -> no motion. Device added / removed and everything else pass untouched.
// SDL2 layouts: type +0, timestamp +4, which +8; axis/button/hat/ball index +12; button state / hat value +13;
// axis value (Sint16) +16; ball xrel / yrel (Sint16) +16 / +18.
// COST per event: a type compare; a policy call only for the six pad event types.
void FilterSdlPadEvent(void* event);

int Policy(int path)
{
    typedef int(*PFN_Policy)(int);
    static PFN_Policy policy = nullptr;
    static bool looked = false;
    if (!looked) {
        HMODULE m = GetModuleHandleA("LibOVRRT64_1.dll");
        if (!m) {
            return 0;   // layer not loaded yet: pass, as every filter here does
        }
        policy = reinterpret_cast<PFN_Policy>(GetProcAddress(m, "dlvr_MenuInputPolicy"));
        looked = true;
        LOG("menu input block: %s", policy ? "policy available - the hooks block while the mod menu is open"
                                           : "dlvr_MenuInputPolicy NOT exported by the layer - every hook passes");
    }
    return policy ? policy(path) : 0;
}

// Key-down, key-up and character messages. Run 1 (2026-10-13, Jay: "Arrow keys are still reaching the game, just
// checked at main menu") blocked 58 key-downs on the live pump (exe PeekMessageA - engine / SDL2 pumps read 0) and the
// arrows still navigated the main menu: the first version PASSED every key-up, and the engine's key handler queues
// (key, char, down) events, so a consumer acting on the up event still fired. Now a key-up is blocked too UNLESS the
// game saw that key go down (a down that passed while the menu was closed) - that one key-up is let through, so a key
// held when the menu opened is not left stuck down. Alt+F4 passes, so the game can still be closed with the menu open.
// g_gameKeyDown is touched only on the message thread that pumps (the exe's loop), from key messages only.
bool g_gameKeyDown[256] = {};

bool IsKeyDownMessage(UINT msg) { return msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN; }
bool IsKeyUpMessage(UINT msg) { return msg == WM_KEYUP || msg == WM_SYSKEYUP; }

bool IsCharMessage(UINT msg)
{
    return msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR || msg == WM_UNICHAR;
}

typedef unsigned char(*PFN_SdlGcGetButton)(void*, int);
typedef short(*PFN_SdlGcGetAxis)(void*, int);
typedef BOOL(WINAPI* PFN_PeekMessage)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL(WINAPI* PFN_GetMessage)(LPMSG, HWND, UINT, UINT);
typedef BOOL(WINAPI* PFN_GetKeyboardState)(PBYTE);
typedef SHORT(WINAPI* PFN_GetKeyState)(int);

PFN_SdlGcGetButton g_sdlGetButton = nullptr;
PFN_SdlGcGetAxis   g_sdlGetAxis = nullptr;
PFN_PeekMessage    g_peekExe = nullptr;
PFN_PeekMessage    g_peekEngine = nullptr;
PFN_PeekMessage    g_peekSdlW = nullptr;
PFN_GetMessage     g_getMsgSdlW = nullptr;
PFN_GetKeyboardState g_getKbState = nullptr;
PFN_GetKeyState    g_getAsyncKey = nullptr;
PFN_GetKeyState    g_getKeyState = nullptr;

unsigned char HookedSdlGcGetButton(void* gc, int button)
{
    const unsigned char v = g_sdlGetButton(gc, button);
    return Policy(kSdlButton) ? 0 : v;
}

// SDL_GameControllerAxis: 0 LEFTX, 1 LEFTY, 2 RIGHTX, 3 RIGHTY, 4 TRIGGERLEFT, 5 TRIGGERRIGHT. 0 = centred / released.
short HookedSdlGcGetAxis(void* gc, int axis)
{
    const short v = g_sdlGetAxis(gc, axis);
    const int p = Policy(kSdlAxis);
    if (p == 0 || (p == 3 && (axis == 2 || axis == 3))) {
        return v;
    }
    return 0;
}

// A blocked message becomes WM_NULL: dispatched as a no-op, and still removed (or not) as the caller asked.
// A PM_NOREMOVE peek shows a message that a later removing call shows again, so the key-down record changes only on
// the REMOVING call (`removes`) - otherwise a looked-at key-up would clear it and the real release would be blocked.
BOOL FilterPeeked(BOOL r, LPMSG m, int path, bool removes)
{
    if (!r || !m) {
        return r;
    }
    const UINT msg = m->message;
    const size_t vk = static_cast<size_t>(m->wParam) & 0xFF;
    if (IsKeyDownMessage(msg)) {
        if (msg == WM_SYSKEYDOWN && m->wParam == VK_F4) {
            return r;
        }
        if (Policy(path)) {
            m->message = WM_NULL;
        } else if (removes) {
            g_gameKeyDown[vk] = true;
        }
    } else if (IsKeyUpMessage(msg)) {
        if (g_gameKeyDown[vk]) {
            if (removes) {
                g_gameKeyDown[vk] = false;   // the game saw this key go down: its release must reach it
            }
        } else if (Policy(path)) {
            m->message = WM_NULL;
        }
    } else if (IsCharMessage(msg) && Policy(path)) {
        m->message = WM_NULL;
    }
    return r;
}

BOOL WINAPI HookedPeekExe(LPMSG m, HWND h, UINT a, UINT b, UINT f) { return FilterPeeked(g_peekExe(m, h, a, b, f), m, kPeekExe, (f & PM_REMOVE) != 0); }
BOOL WINAPI HookedPeekEngine(LPMSG m, HWND h, UINT a, UINT b, UINT f) { return FilterPeeked(g_peekEngine(m, h, a, b, f), m, kPeekEngine, (f & PM_REMOVE) != 0); }
BOOL WINAPI HookedPeekSdlW(LPMSG m, HWND h, UINT a, UINT b, UINT f) { return FilterPeeked(g_peekSdlW(m, h, a, b, f), m, kPeekSdlW, (f & PM_REMOVE) != 0); }

BOOL WINAPI HookedGetMsgSdlW(LPMSG m, HWND h, UINT a, UINT b)
{
    const BOOL r = g_getMsgSdlW(m, h, a, b);
    if (r > 0) {   // r <= 0 (WM_QUIT / error) is returned untouched
        FilterPeeked(r, m, kGetMsgSdlW, true);   // GetMessage always removes
    }
    return r;
}

BOOL WINAPI HookedGetKeyboardState(PBYTE keys)
{
    const BOOL r = g_getKbState(keys);
    if (r && keys && Policy(kKbState)) {
        memset(keys, 0, 256);
    }
    return r;
}

SHORT WINAPI HookedGetAsyncKeyState(int vk)
{
    const SHORT r = g_getAsyncKey(vk);
    return Policy(kAsyncKey) ? 0 : r;
}

SHORT WINAPI HookedGetKeyState(int vk)
{
    const SHORT r = g_getKeyState(vk);
    return Policy(kKeyState) ? 0 : r;
}

template <typename T>
void Hook(HMODULE mod, const char* modName, const char* fromDll, const char* name, void* hook, T& orig)
{
    if (!mod) {
        LOG("menu input block: %s not mapped - %s!%s NOT hooked", modName, fromDll, name);
        return;
    }
    void* o = ::dlvr::PatchIat(mod, fromDll, name, hook);
    if (o) {
        orig = reinterpret_cast<T>(o);
        LOG("menu input block: %s %s!%s -> %p", modName, fromDll, name, o);
    } else {
        LOG("menu input block: %s!%s NOT FOUND in %s's imports - that path is NOT blocked", fromDll, name, modName);
    }
}

void InstallKeyQueueHook(HMODULE engine);   // below, with the key-queue hook
void InstallKbPollHook(HMODULE engine);     // below, CKeyboard::Poll
void InstallJoyGetterHooks(HMODULE engine); // below, CNewInputManager joy getters

void Install()
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    InstallKeyQueueHook(engine);
    InstallKbPollHook(engine);
    InstallJoyGetterHooks(engine);
    HMODULE exe = GetModuleHandleA(nullptr);
    HMODULE sdl = GetModuleHandleA("SDL2.dll");
    Hook(engine, "engine", "SDL2.dll", "SDL_GameControllerGetButton", reinterpret_cast<void*>(&HookedSdlGcGetButton), g_sdlGetButton);
    Hook(engine, "engine", "SDL2.dll", "SDL_GameControllerGetAxis", reinterpret_cast<void*>(&HookedSdlGcGetAxis), g_sdlGetAxis);
    Hook(engine, "engine", "USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&HookedPeekEngine), g_peekEngine);
    Hook(engine, "engine", "USER32.dll", "GetKeyboardState", reinterpret_cast<void*>(&HookedGetKeyboardState), g_getKbState);
    Hook(engine, "engine", "USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&HookedGetAsyncKeyState), g_getAsyncKey);
    Hook(engine, "engine", "USER32.dll", "GetKeyState", reinterpret_cast<void*>(&HookedGetKeyState), g_getKeyState);
    Hook(exe, "exe", "USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&HookedPeekExe), g_peekExe);
    Hook(sdl, "SDL2", "USER32.dll", "PeekMessageW", reinterpret_cast<void*>(&HookedPeekSdlW), g_peekSdlW);
    Hook(sdl, "SDL2", "USER32.dll", "GetMessageW", reinterpret_cast<void*>(&HookedGetMsgSdlW), g_getMsgSdlW);
}

// ---- DirectInput 8 (run 2, 2026-10-13: Jay "Both keyboard & controller still get through at the main menu") ----
// Every hooked path above blocked (exe PeekMessageA 52, GetKeyboardState 4614, GetAsyncKeyState 97, scePad 18456) and
// the SDL pad was never read once, yet input still reached the main menu. The engine's CInput reads the keyboard and
// mouse through DirectInput, loaded at runtime so it is in no import table (OUR engine, static): +27E146 GetProcAddress
// "DirectInput8Create", called (+27E20E) with version 0x800, IID_IDirectInput8A (BF798030), object at this+0x49A0;
// +27E754 EnumDevices(DI8DEVCLASS_KEYBOARD = 3) -> keyboard devices; +27EDD0 CreateDevice(GUID_SysMouse).
// Mechanics: wrap DirectInput8Create; patch the IDirectInput8A vtable's CreateDevice (slot 3) to record each device's
// type (GetCapabilities dwDevType); patch the device vtable's GetDeviceState (9) / GetDeviceData (10) - one vtable for
// every dinput8 device, so each call is matched against the recorded devices and an unrecorded one passes untouched.
//   keyboard  : state zeroed; buffered events dropped, EXCEPT a key-up for a key the game saw go down (no stuck key)
//   controller: state set neutral (axes = centre of DIPROP_RANGE, POVs centred, buttons up) for the DIJOYSTATE /
//               DIJOYSTATE2 formats - any other format is logged by name and passed; events dropped except button-ups
//   mouse     : NOT blocked (not asked for) - its first read is logged so the run shows whether it is live
// COST per device read: the original call, a scan of <= 32 recorded devices, a cached policy call.
struct DiDevCaps {
    DWORD dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, dwFFSamplePeriod, dwFFMinTimeResolution,
        dwFirmwareRevision, dwHardwareRevision, dwFFDriverVersion;
};
struct DiObjData { DWORD dwOfs; DWORD dwData; DWORD dwTimeStamp; DWORD dwSequence; UINT_PTR uAppData; };
struct DiPropRange { DWORD dwSize, dwHeaderSize, dwObj, dwHow; LONG lMin, lMax; };

typedef HRESULT(WINAPI* PFN_DI8Create)(HINSTANCE, DWORD, const GUID&, void**, void*);
typedef HRESULT(WINAPI* PFN_DICreateDevice)(void*, const GUID&, void**, void*);
typedef HRESULT(WINAPI* PFN_DIDevGetCaps)(void*, DiDevCaps*);
// The property id is a REFGUID; DIPROP_RANGE is the small-integer pseudo-GUID MAKEDIPROP(4), so it is passed as the
// pointer value it is (same ABI as a reference) rather than by dereferencing address 4.
typedef HRESULT(WINAPI* PFN_DIDevGetProperty)(void*, const GUID*, DiPropRange*);
typedef HRESULT(WINAPI* PFN_DIDevGetState)(void*, DWORD, void*);
typedef HRESULT(WINAPI* PFN_DIDevGetData)(void*, DWORD, DiObjData*, DWORD*, DWORD);

PFN_DI8Create       g_realDI8Create = nullptr;
PFN_DICreateDevice  g_realCreateDevice = nullptr;

// 2026-10-21 (a tester's start-up hang, hang_watch.log): slot 3 is patched in a vtable that EVERY IDirectInput8 object of
// that kind shares, so a third party's CreateDevice lands in HookedCreateDevice too. Logitech's DirectInput force-feedback
// driver (jerry_forcefeedback_x64.dll) calls CreateDevice from inside the GetCapabilities we issue there, which re-entered
// us -> GetCapabilities -> the driver -> CreateDevice ... on the main thread, before InitSDL. Only the objects the ENGINE
// got back from HookedDI8Create (its GetProcAddress is the only route to the wrapper) are classified; every other caller
// gets the real CreateDevice and nothing else. A nested CreateDevice on the engine's own object (a driver re-entering on
// the same thread) is passed through unclassified as well.
void* volatile g_engineDI[4] = {};
volatile LONG g_engineDICount = 0;
thread_local bool t_inCreateDeviceCaps = false;

bool IsEngineDI(void* di)
{
    const LONG n = g_engineDICount;
    for (LONG i = 0; i < n && i < 4; ++i) {
        if (g_engineDI[i] == di) {
            return true;
        }
    }
    return false;
}

const char* CallerModule(void* addr, char* buf, DWORD cb)
{
    HMODULE m = nullptr;
    buf[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &m) && m) {
        GetModuleFileNameA(m, buf, cb);
    }
    const char* leaf = strrchr(buf, '\\');
    return buf[0] ? (leaf ? leaf + 1 : buf) : "(no module)";
}

// Run 3 (2026-10-13): the keyboard AND the mouse each logged "vtable ... hooked", i.e. dinput8 gives them DIFFERENT
// vtables, and v3 kept ONE original per slot - the mouse's overwrote the keyboard's. The originals are now kept per
// vtable, and each hooked call looks its own vtable up.
struct DiVt { void** vt; PFN_DIDevGetState state; PFN_DIDevGetData data; };
DiVt g_diVts[8] = {};
volatile LONG g_diVtCount = 0;

const DiVt* DiVtFor(void* dev)
{
    void** vt = *reinterpret_cast<void***>(dev);
    const LONG n = g_diVtCount;
    for (LONG i = 0; i < n && i < 8; ++i) {
        if (g_diVts[i].vt == vt) {
            return &g_diVts[i];
        }
    }
    return nullptr;
}

enum DiKind { kDiOther = 0, kDiKeyboard = 1, kDiMouse = 2, kDiController = 3 };
struct DiDev {
    void* volatile dev;
    int kind;
    unsigned devType;
    bool haveCentre;
    LONG centre[8];   // lX lY lZ lRx lRy lRz slider0 slider1
    bool firstRead;
    bool formatLogged;
};
DiDev g_diDevs[32] = {};
volatile LONG g_diDevCount = 0;
bool g_diKeyDown[256] = {};   // DIK codes the game saw go down through buffered events (only the poll thread)

const char* DiKindName(int k)
{
    return k == kDiKeyboard ? "keyboard" : k == kDiMouse ? "mouse" : k == kDiController ? "controller" : "other";
}

DiDev* FindDiDev(void* dev)
{
    const LONG n = g_diDevCount;
    for (LONG i = 0; i < n && i < 32; ++i) {
        if (g_diDevs[i].dev == dev) {
            return &g_diDevs[i];
        }
    }
    return nullptr;
}

void* PatchVtableSlot(void* obj, int slot, void* hook)
{
    void** vt = *reinterpret_cast<void***>(obj);
    void* orig = vt[slot];
    if (orig == hook) {
        return nullptr;
    }
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) {
        return nullptr;
    }
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return orig;
}

// Logged on the device's FIRST call, success or not (v3 returned before this on a failed read, so a device that was
// read but not acquired - e.g. the window out of focus under VR - looked exactly like one never read).
void DiFirstRead(DiDev* d, const char* what, DWORD cb, HRESULT hr)
{
    if (!d->firstRead) {
        d->firstRead = true;
        LOG("menu input block: dinput %s FIRST READ - device %p (%s, devType 0x%02X), cb %lu, hr 0x%08lX%s", what,
            d->dev, DiKindName(d->kind), d->devType, static_cast<unsigned long>(cb), static_cast<unsigned long>(hr),
            FAILED(hr) ? " - FAILED (not acquired / lost?)" : "");
    }
}

void DiNoOriginal(void* dev, const char* what)
{
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) {
        LOG("menu input block: dinput %s on device %p - its vtable has NO recorded original; returned E_FAIL", what, dev);
    }
}

void DiNeutralJoyState(DiDev* d, DWORD cb, void* data)
{
    if (!d->haveCentre) {
        d->haveCentre = true;
        auto getProp = reinterpret_cast<PFN_DIDevGetProperty>((*reinterpret_cast<void***>(d->dev))[5]);
        for (int i = 0; i < 8; ++i) {
            DiPropRange r = {};
            r.dwSize = sizeof(r);
            r.dwHeaderSize = 16;
            r.dwObj = static_cast<DWORD>(i * 4);   // DIJOYSTATE offsets lX..rglSlider[1]
            r.dwHow = 1;                           // DIPH_BYOFFSET
            const HRESULT hr = getProp(d->dev, reinterpret_cast<const GUID*>(static_cast<UINT_PTR>(4)) /* DIPROP_RANGE */, &r);
            d->centre[i] = SUCCEEDED(hr) ? (r.lMin + r.lMax) / 2 : 0;
        }
        LOG("menu input block: dinput controller %p neutral = axes %ld %ld %ld %ld %ld %ld, sliders %ld %ld (centre of "
            "DIPROP_RANGE; 0 where the range query failed)", d->dev, d->centre[0], d->centre[1], d->centre[2],
            d->centre[3], d->centre[4], d->centre[5], d->centre[6], d->centre[7]);
    }
    memset(data, 0, cb);
    LONG* axes = static_cast<LONG*>(data);
    for (int i = 0; i < 8; ++i) {
        axes[i] = d->centre[i];
    }
    DWORD* pov = reinterpret_cast<DWORD*>(static_cast<BYTE*>(data) + 32);
    for (int i = 0; i < 4; ++i) {
        pov[i] = 0xFFFFFFFFu;
    }
}

HRESULT WINAPI HookedGetDeviceState(void* dev, DWORD cb, void* data)
{
    const DiVt* v = DiVtFor(dev);
    if (!v || !v->state) {
        DiNoOriginal(dev, "GetDeviceState");
        return E_FAIL;
    }
    const HRESULT hr = v->state(dev, cb, data);
    DiDev* d = FindDiDev(dev);
    if (!d) {
        return hr;
    }
    DiFirstRead(d, "GetDeviceState", cb, hr);
    if (FAILED(hr) || !data) {
        return hr;
    }
    if (d->kind == kDiKeyboard) {
        if (Policy(kDiKbState)) {
            memset(data, 0, cb);
        }
    } else if (d->kind == kDiController) {
        if (cb == 80 || cb == 272) {   // DIJOYSTATE / DIJOYSTATE2
            if (Policy(kDiCtlState)) {
                DiNeutralJoyState(d, cb, data);
            }
        } else if (!d->formatLogged) {
            d->formatLogged = true;
            LOG("menu input block: dinput controller %p reads a %lu-byte state - not DIJOYSTATE / DIJOYSTATE2, its "
                "neutral is unknown, NOT blocked", dev, static_cast<unsigned long>(cb));
        }
    }
    return hr;
}

HRESULT WINAPI HookedGetDeviceData(void* dev, DWORD cbObj, DiObjData* rg, DWORD* inOut, DWORD flags)
{
    const DiVt* v = DiVtFor(dev);
    if (!v || !v->data) {
        DiNoOriginal(dev, "GetDeviceData");
        return E_FAIL;
    }
    const HRESULT hr = v->data(dev, cbObj, rg, inOut, flags);
    DiDev* d = FindDiDev(dev);
    if (d) {
        DiFirstRead(d, "GetDeviceData", cbObj, hr);
    }
    if (FAILED(hr) || !rg || !inOut || cbObj < sizeof(DWORD) * 2) {
        return hr;
    }
    if (!d || (d->kind != kDiKeyboard && d->kind != kDiController)) {
        return hr;
    }
    const bool peek = (flags & 1) != 0;   // DIGDD_PEEK: the events stay queued, so the key record must not change
    const DWORD n = *inOut;
    const bool block = n > 0 && Policy(d->kind == kDiKeyboard ? kDiKbData : kDiCtlData) != 0;
    DWORD keep = 0;
    for (DWORD i = 0; i < n; ++i) {
        auto* e = reinterpret_cast<DiObjData*>(reinterpret_cast<BYTE*>(rg) + static_cast<size_t>(i) * cbObj);
        bool pass;
        if (d->kind == kDiKeyboard) {
            const DWORD code = e->dwOfs & 0xFF;
            const bool down = (e->dwData & 0x80) != 0;
            if (!block) {
                pass = true;
                if (!peek) { g_diKeyDown[code] = down; }
            } else {
                pass = !down && g_diKeyDown[code];   // the release of a key the game saw go down
                if (pass && !peek) { g_diKeyDown[code] = false; }
            }
        } else {
            pass = !block || (e->dwOfs >= 48 && (e->dwData & 0x80) == 0);   // controller: only button-ups pass
        }
        if (pass) {
            if (keep != i) {
                memmove(reinterpret_cast<BYTE*>(rg) + static_cast<size_t>(keep) * cbObj, e, cbObj);
            }
            ++keep;
        }
    }
    *inOut = keep;
    return hr;
}

HRESULT WINAPI HookedCreateDevice(void* di, const GUID& guid, void** out, void* outer)
{
    void* const caller = _ReturnAddress();
    const HRESULT hr = g_realCreateDevice(di, guid, out, outer);
    if (FAILED(hr) || !out || !*out) {
        return hr;
    }
    if (!IsEngineDI(di)) {
        static volatile LONG foreign = 0;
        const LONG n = InterlockedIncrement(&foreign);
        if (n == 1) {
            char mod[MAX_PATH];
            LOG("menu input block: dinput CreateDevice -> %p on a DirectInput object the engine did not create (%p, "
                "caller %s) - passed through, not classified or hooked (first one; later ones are not logged)", *out,
                di, CallerModule(caller, mod, sizeof(mod)));
        }
        return hr;
    }
    if (t_inCreateDeviceCaps) {
        static volatile LONG nested = 0;
        if (InterlockedIncrement(&nested) == 1) {
            char mod[MAX_PATH];
            LOG("menu input block: dinput CreateDevice -> %p re-entered from inside our GetCapabilities (caller %s) - "
                "passed through, not classified or hooked (first one; later ones are not logged)", *out,
                CallerModule(caller, mod, sizeof(mod)));
        }
        return hr;
    }
    void* dev = *out;
    DiDevCaps caps = {};
    caps.dwSize = sizeof(caps);
    auto getCaps = reinterpret_cast<PFN_DIDevGetCaps>((*reinterpret_cast<void***>(dev))[3]);
    t_inCreateDeviceCaps = true;
    const HRESULT ch = getCaps(dev, &caps);
    t_inCreateDeviceCaps = false;
    const unsigned type = SUCCEEDED(ch) ? (caps.dwDevType & 0xFF) : 0;
    const int kind = type == 0x13 ? kDiKeyboard : type == 0x12 ? kDiMouse : type >= 0x14 ? kDiController : kDiOther;
    const LONG idx = g_diDevCount;
    if (idx < 32) {
        g_diDevs[idx].kind = kind;
        g_diDevs[idx].devType = type;
        g_diDevs[idx].dev = dev;
        InterlockedIncrement(&g_diDevCount);
    }
    // The original is RECORDED for this vtable BEFORE the slot is swapped, so no call can reach a hook whose vtable
    // has no original yet. A vtable already patched (a second device sharing it) is left as it is.
    void** vt = *reinterpret_cast<void***>(dev);
    const bool newVt = DiVtFor(dev) == nullptr;
    bool patched = false;
    if (newVt && g_diVtCount < 8) {
        DiVt& e = g_diVts[g_diVtCount];
        e.vt = vt;
        e.state = reinterpret_cast<PFN_DIDevGetState>(vt[9]);
        e.data = reinterpret_cast<PFN_DIDevGetData>(vt[10]);
        InterlockedIncrement(&g_diVtCount);
        patched = PatchVtableSlot(dev, 9, reinterpret_cast<void*>(&HookedGetDeviceState)) != nullptr;
        patched = PatchVtableSlot(dev, 10, reinterpret_cast<void*>(&HookedGetDeviceData)) != nullptr && patched;
    }
    LOG("menu input block: dinput CreateDevice -> %p, %s (devType 0x%02X, caps hr 0x%08lX), vtable %p %s%s", dev,
        DiKindName(kind), type, static_cast<unsigned long>(ch), static_cast<void*>(vt),
        !newVt ? "(shared, already hooked)" : patched ? "- GetDeviceState / GetDeviceData hooked"
                                                    : "- NOT hooked (vtable table full or protect failed)",
        idx < 32 ? "" : " - device TABLE FULL, NOT recorded, passes untouched");
    return hr;
}

HRESULT WINAPI HookedDI8Create(HINSTANCE inst, DWORD ver, const GUID& iid, void** out, void* outer)
{
    const HRESULT hr = g_realDI8Create(inst, ver, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        // Recorded BEFORE the slot is swapped, so the engine's first CreateDevice already finds its object.
        const LONG slot = g_engineDICount;
        if (slot < 4) {
            g_engineDI[slot] = *out;
            InterlockedIncrement(&g_engineDICount);
        } else {
            LOG("menu input block: a 5th engine DirectInput object %p - NOT recorded, its devices pass untouched", *out);
        }
        void* o = PatchVtableSlot(*out, 3, reinterpret_cast<void*>(&HookedCreateDevice));
        if (o) { g_realCreateDevice = reinterpret_cast<PFN_DICreateDevice>(o); }
        LOG("menu input block: DirectInput8Create(version 0x%lX) -> %p, CreateDevice %s", static_cast<unsigned long>(ver),
            *out, o ? "hooked" : (g_realCreateDevice ? "already hooked" : "NOT hooked - the dinput path is NOT blocked"));
    } else {
        LOG("menu input block: DirectInput8Create FAILED hr 0x%08lX - nothing hooked", static_cast<unsigned long>(hr));
    }
    return hr;
}

// ---- the engine's KEY-EVENT QUEUE (run 3, 2026-10-13: Jay "Both still get through") ----
// Run 3: DirectInput keyboard + mouse were created and never logged a read, XInput was never called, and every path
// above blocked - yet keys still reached the main menu. Static (OUR engine): every keyboard event the engine queues
// goes through ONE function, +2A360 (key, char, down) - edx = engine key code, r8b = character, r9b = down; rcx unused.
// It has exactly THREE callers, all `E8 rel32`: +177C94 (IEngineImpl vtable slot 200, the window-message key handler:
// MapVirtualKeyA -> GetKeyboardState -> ToAscii -> push) and +2808CE / +2808E3 (the DirectInput buffered-keyboard
// loop: GetDeviceData at +280426 on the device at this+0x1808, 24-byte entries at this+8, DIK code / bit-7 down).
// Each call site's rel32 is redirected through a 12-byte stub allocated within +-2 GB of the engine (mov rax, imm64;
// jmp rax) to KeyQueueHook. Every site's bytes are CHECKED first (E8 resolving to +2A360) - a different engine build
// is refused by name, site by site. Installed at bootstrap attach, before any engine code runs, so no thread can be
// executing a site while its 4 bytes change.
// While the menu is open: a key-DOWN is dropped; a key-UP passes only for a key the game saw go down (no stuck key).
// The caller (return address) of the first call from each site is logged, so the run shows which source is live.
// COST per queued key event: one policy call and a 1024-entry bool lookup - key events are a few per press.
typedef void(*PFN_KeyPush)(void*, int, unsigned char, unsigned char);
PFN_KeyPush g_keyPushOrig = nullptr;
bool g_queueKeyDown[1024] = {};
volatile LONG g_keyPushSiteSeen[3] = {};
uintptr_t g_engineBase = 0;

void KeyQueueHook(void* rcx, int key, unsigned char ch, unsigned char down)
{
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t rva = ret - g_engineBase;
    const int site = rva == 0x177C99 ? 0 : rva == 0x2808D3 ? 1 : rva == 0x2808E8 ? 2 : -1;
    if (site >= 0 && g_keyPushSiteSeen[site] == 0 && InterlockedExchange(&g_keyPushSiteSeen[site], 1) == 0) {
        LOG("menu input block: engine key queue FIRST EVENT via call site %d (returns to engine +%IX = %s) - key %d, "
            "char %u, %s", site, rva, site == 0 ? "the window-message key handler" : "the DirectInput keyboard loop",
            key, static_cast<unsigned>(ch), down ? "down" : "up");
    }
    const bool track = key >= 0 && key < 1024;
    if (Policy(kKeyQueue)) {
        if (down) {
            return;
        }
        if (!track || !g_queueKeyDown[key]) {
            return;
        }
        g_queueKeyDown[key] = false;   // the release of a key the game saw go down
    } else if (track) {
        g_queueKeyDown[key] = down != 0;
    }
    g_keyPushOrig(rcx, key, ch, down);
}

void* AllocStubNear(uintptr_t target)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t span = 0x70000000ull;   // stay well inside rel32's +-2 GB
    for (uintptr_t d = gran; d < span; d += gran) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const uintptr_t a = sgn < 0 ? (target > d ? ((target - d) & ~(gran - 1)) : 0) : ((target + d) & ~(gran - 1));
            if (!a) {
                continue;
            }
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) {
                return p;
            }
        }
    }
    return nullptr;
}

void InstallKeyQueueHook(HMODULE engine)
{
    if (!engine) {
        LOG("menu input block: engine not mapped - key queue NOT hooked");
        return;
    }
    g_engineBase = reinterpret_cast<uintptr_t>(engine);
    // The loaded build's rvas (common/buildrva.h: Steam unchanged, the EGL table on the Epic build); the checks below are
    // unchanged and run on the moved addresses.
    using ::dlvr::buildrva::Eng;
    const uintptr_t target = g_engineBase + Eng(0x2A360);
    const uintptr_t sites[3] = { g_engineBase + Eng(0x177C94), g_engineBase + Eng(0x2808CE), g_engineBase + Eng(0x2808E3) };
    // Verify every site BEFORE touching any: all three must be E8 rel32 -> +2A360, or nothing is patched.
    for (int i = 0; i < 3; ++i) {
        const auto* b = reinterpret_cast<const unsigned char*>(sites[i]);
        const int32_t rel = *reinterpret_cast<const int32_t*>(b + 1);
        if (b[0] != 0xE8 || sites[i] + 5 + rel != target) {
            LOG("menu input block: key queue call site %d (+%IX) is NOT `call +2A360` (byte %02X) - a different engine "
                "build; the key queue is NOT hooked", i, sites[i] - g_engineBase, static_cast<unsigned>(b[0]));
            return;
        }
    }
    auto* stub = static_cast<unsigned char*>(AllocStubNear(g_engineBase));
    if (!stub) {
        LOG("menu input block: no executable page within reach of the engine - the key queue is NOT hooked");
        return;
    }
    const intptr_t dist = reinterpret_cast<intptr_t>(stub) - static_cast<intptr_t>(g_engineBase);
    if (dist > 0x7FF00000ll || dist < -0x7FF00000ll) {
        LOG("menu input block: stub %p is out of rel32 reach of the engine - the key queue is NOT hooked", stub);
        return;
    }
    g_keyPushOrig = reinterpret_cast<PFN_KeyPush>(target);
    const uint64_t hook = reinterpret_cast<uint64_t>(&KeyQueueHook);
    stub[0] = 0x48; stub[1] = 0xB8;            // mov rax, imm64
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;          // jmp rax
    FlushInstructionCache(GetCurrentProcess(), stub, 12);
    int done = 0;
    for (int i = 0; i < 3; ++i) {
        auto* b = reinterpret_cast<unsigned char*>(sites[i]);
        DWORD old = 0;
        if (!VirtualProtect(b + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            LOG("menu input block: key queue call site %d - VirtualProtect failed, NOT patched", i);
            continue;
        }
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - static_cast<intptr_t>(sites[i] + 5));
        memcpy(b + 1, &rel, 4);
        VirtualProtect(b + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), b, 5);
        ++done;
    }
    LOG("menu input block: engine key queue (+2A360) - %d of 3 call sites redirected through stub %p", done, stub);
}

// ---- CKeyboard::Poll (run 4, 2026-10-13: Jay "Keyboard still gets through"; "Just check with IDA") ----
// Run 4 blocked 43 key-queue events (all from the DirectInput loop) and keys still reached the menu. IDA (OUR engine,
// staged i64 md5 e9fbfc2c == deployed, ghidra-project/ida/mb1_engine_out.txt): sub_1802803E0 = CKeyboard vtable slot 1
// (vtable +85BF50, entry +85BF58 - RTTI .?AVCKeyboard@@). It reads GetDeviceData (vtable +0x50) on the device at
// this+0x1808 (6152), then per event ALSO writes the key-state table *(this+6464+key) = down, calls sub_180245950 and
// sub_180280930 (the UI listeners, vtable +1152) - the queue push was only one of four consumers. So the read itself is
// filtered: the device object THIS function reads (which our CreateDevice hook evidently never saw - no FIRST READ in
// runs 3 and 4) is registered as a keyboard and its own vtable's GetDeviceState / GetDeviceData are hooked on the first
// poll, before the original runs. HookedGetDeviceData then drops key-downs while the menu is open (key-ups of keys the
// game saw go down pass), so all four consumers see the same filtered events.
// COST per poll: one load and compare of the device pointer against the last one seen.
typedef void(*PFN_KbPoll)(void*, void*);
PFN_KbPoll g_kbPollOrig = nullptr;
void* g_kbPollLastDev = nullptr;

void KbPollHook(void* self, void* a2)
{
    void* dev = *reinterpret_cast<void**>(static_cast<BYTE*>(self) + 0x1808);
    if (dev && dev != g_kbPollLastDev) {
        g_kbPollLastDev = dev;
        DiDev* d = FindDiDev(dev);
        const bool known = d != nullptr;
        if (!d && g_diDevCount < 32) {
            DiDev& e = g_diDevs[g_diDevCount];
            e.kind = kDiKeyboard;
            e.devType = 0x13;
            e.dev = dev;
            InterlockedIncrement(&g_diDevCount);
            d = &e;
        }
        void** vt = *reinterpret_cast<void***>(dev);
        const bool newVt = DiVtFor(dev) == nullptr;
        bool patched = false;
        if (newVt && g_diVtCount < 8) {
            DiVt& v = g_diVts[g_diVtCount];
            v.vt = vt;
            v.state = reinterpret_cast<PFN_DIDevGetState>(vt[9]);
            v.data = reinterpret_cast<PFN_DIDevGetData>(vt[10]);
            InterlockedIncrement(&g_diVtCount);
            patched = PatchVtableSlot(dev, 9, reinterpret_cast<void*>(&HookedGetDeviceState)) != nullptr;
            patched = PatchVtableSlot(dev, 10, reinterpret_cast<void*>(&HookedGetDeviceData)) != nullptr && patched;
        }
        LOG("menu input block: CKeyboard::Poll reads device %p (vtable %p) - %s; its GetDeviceData %s", dev,
            static_cast<void*>(vt), known ? "already recorded by CreateDevice" : "NOT seen by CreateDevice, recorded now",
            !newVt ? "was already hooked" : patched ? "is hooked now" : "could NOT be hooked");
    }
    g_kbPollOrig(self, a2);
}

void InstallKbPollHook(HMODULE engine)
{
    if (!engine) {
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
    // The loaded build's rvas (common/buildrva.h); the slot compare is unchanged.
    void** slot = reinterpret_cast<void**>(base + ::dlvr::buildrva::Eng(0x85BF58));
    if (reinterpret_cast<uintptr_t>(*slot) != base + ::dlvr::buildrva::Eng(0x2803E0)) {
        LOG("menu input block: CKeyboard vtable slot +85BF58 holds %p, not engine +2803E0 - a different engine build; "
            "CKeyboard::Poll NOT hooked", *slot);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG("menu input block: CKeyboard vtable slot - VirtualProtect failed, NOT hooked");
        return;
    }
    g_kbPollOrig = reinterpret_cast<PFN_KbPoll>(*slot);
    *slot = reinterpret_cast<void*>(&KbPollHook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    LOG("menu input block: CKeyboard::Poll (vtable +85BF58 -> +2803E0) hooked - the device it reads gets its own "
        "GetDeviceData filter on the first poll");
}

// ---- CNewInputManager joy getters (run 6, 2026-10-13: Jay "Still gets through" with 388 SDL pad events rewritten) ----
// IDA (OUR engine, mb3 / mb4_engine_out.txt): the engine tests NO controller axis / button event type (no 0x650-0x652
// compare anywhere) - the SDL events only announce added / removed (+2BCD02 / +2BCD19). Every pad backend - CXPadDevice
// (+2FDD40, XInput), CSDLPadDevice (+2BD280, SDL GameController), the scePad and other devices - writes its state into a
// per-device map through sub_1802FA680 (ids 1-2 triggers, 3-6 sticks, 7-20 buttons), and the map is read back through
// CNewInputManager::GetJoyAxisState (+2FBF60, its own log string) / ::GetJoyButtonState (+2FBD50). Those two sit in two
// vtables: CInput slots 0 / 1 (+85BED8 / +85BEE0) and CNewInputManager slots 0 / 1 (+864AB8 / +864AC0). While the menu is
// open the hooks return 0.0 / 0 - every backend at once. Each entry is CHECKED before it is swapped. The first call of each
// is logged, so the run proves whether the game reads the pad through them at all.
// COST per call: the original call (it takes a critical section) plus one policy call.
typedef float(*PFN_JoyAxis)(void*, unsigned, unsigned);
typedef unsigned char(*PFN_JoyButton)(void*, unsigned, int);
PFN_JoyAxis g_joyAxisOrig = nullptr;
PFN_JoyButton g_joyButtonOrig = nullptr;
volatile LONG g_joyAxisSeen = 0, g_joyButtonSeen = 0;

float HookedGetJoyAxisState(void* self, unsigned dev, unsigned axis)
{
    const float v = g_joyAxisOrig(self, dev, axis);
    if (g_joyAxisSeen == 0 && InterlockedExchange(&g_joyAxisSeen, 1) == 0) {
        LOG("menu input block: CNewInputManager::GetJoyAxisState FIRST CALL (device %u, axis %u) - the game reads the "
            "pad's axes through it", dev, axis);
    }
    const int p = Policy(kJoyAxis);
    // GetJoyAxisState's switch (+2FBF60): axis 0 -> id 3 (left X), 1 -> 4 (left Y), 2 -> 1 (LT), 3 -> 5 (right X),
    // 4 -> 6 (right Y), 5 -> 2 (RT) - so LT held (policy 3) passes getter axes 3 / 4, the right stick.
    if (p == 0 || (p == 3 && (axis == 3 || axis == 4))) {
        return v;
    }
    return 0.0f;
}

unsigned char HookedGetJoyButtonState(void* self, unsigned dev, int button)
{
    const unsigned char v = g_joyButtonOrig(self, dev, button);
    if (g_joyButtonSeen == 0 && InterlockedExchange(&g_joyButtonSeen, 1) == 0) {
        LOG("menu input block: CNewInputManager::GetJoyButtonState FIRST CALL (device %u, button 0x%X) - the game reads "
            "the pad's buttons through it", dev, static_cast<unsigned>(button));
    }
    return Policy(kJoyButton) ? 0 : v;
}

void InstallJoyGetterHooks(HMODULE engine)
{
    if (!engine) {
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
    struct Slot { uintptr_t rva; uintptr_t expect; void* hook; const char* what; };
    // The loaded build's rvas (common/buildrva.h: Steam unchanged, the EGL table on the Epic build); every slot is still
    // compared against its expected function before any is touched.
    using ::dlvr::buildrva::Eng;
    const Slot slots[4] = {
        { Eng(0x85BED8), Eng(0x2FBF60), reinterpret_cast<void*>(&HookedGetJoyAxisState), "CInput slot 0 GetJoyAxisState" },
        { Eng(0x85BEE0), Eng(0x2FBD50), reinterpret_cast<void*>(&HookedGetJoyButtonState), "CInput slot 1 GetJoyButtonState" },
        { Eng(0x864AB8), Eng(0x2FBF60), reinterpret_cast<void*>(&HookedGetJoyAxisState), "CNewInputManager slot 0 GetJoyAxisState" },
        { Eng(0x864AC0), Eng(0x2FBD50), reinterpret_cast<void*>(&HookedGetJoyButtonState), "CNewInputManager slot 1 GetJoyButtonState" },
    };
    for (const Slot& s : slots) {   // all four checked before any is touched
        if (*reinterpret_cast<uintptr_t*>(base + s.rva) != base + s.expect) {
            LOG("menu input block: %s (+%IX) does not hold +%IX - a different engine build; joy getters NOT hooked",
                s.what, s.rva, s.expect);
            return;
        }
    }
    g_joyAxisOrig = reinterpret_cast<PFN_JoyAxis>(base + Eng(0x2FBF60));
    g_joyButtonOrig = reinterpret_cast<PFN_JoyButton>(base + Eng(0x2FBD50));
    int done = 0;
    for (const Slot& s : slots) {
        void** p = reinterpret_cast<void**>(base + s.rva);
        DWORD old = 0;
        if (!VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old)) {
            LOG("menu input block: %s - VirtualProtect failed, NOT hooked", s.what);
            continue;
        }
        *p = s.hook;
        VirtualProtect(p, sizeof(void*), old, &old);
        ++done;
    }
    LOG("menu input block: CNewInputManager joy getters - %d of 4 vtable entries hooked", done);
}

void FilterSdlPadEvent(void* event)
{
    auto* b = static_cast<BYTE*>(event);
    const uint32_t type = *reinterpret_cast<const uint32_t*>(b);
    switch (type) {
    case 0x650: case 0x651: case 0x600: case 0x601: case 0x602: case 0x603:
        break;
    default:
        return;
    }
    const int p = Policy(kSdlPadEvent);
    if (p == 0) {
        return;
    }
    switch (type) {
    case 0x650:   // SDL_CONTROLLERAXISMOTION
        if (p == 3 && (b[12] == 2 || b[12] == 3)) {
            return;   // LT held: the right stick passes, as dlvr_FilterGamepad
        }
        *reinterpret_cast<int16_t*>(b + 16) = 0;
        break;
    case 0x600:   // SDL_JOYAXISMOTION
        *reinterpret_cast<int16_t*>(b + 16) = 0;
        break;
    case 0x651:   // SDL_CONTROLLERBUTTONDOWN -> UP
        *reinterpret_cast<uint32_t*>(b) = 0x652;
        b[13] = 0;
        break;
    case 0x603:   // SDL_JOYBUTTONDOWN -> UP
        *reinterpret_cast<uint32_t*>(b) = 0x604;
        b[13] = 0;
        break;
    case 0x602:   // SDL_JOYHATMOTION -> centred
        b[13] = 0;
        break;
    case 0x601:   // SDL_JOYBALLMOTION -> no motion
        *reinterpret_cast<int16_t*>(b + 16) = 0;
        *reinterpret_cast<int16_t*>(b + 18) = 0;
        break;
    }
}

FARPROC WrapDirectInput8Create(FARPROC real)
{
    if (!g_realDI8Create) {
        g_realDI8Create = reinterpret_cast<PFN_DI8Create>(real);
        LOG("menu input block: GetProcAddress(\"DirectInput8Create\") -> wrapping %p", real);
    }
    return reinterpret_cast<FARPROC>(&HookedDI8Create);
}

}  // namespace menublock

// ------------------------------------------------ render-race diagnostic ----
//
// Even with all 4 gates confirmed patched (2026-09-02, second test on the
// friend's SteamVR + Bigscreen Beyond machine), dlvr_ovr2xr.log still never
// appears. Everything patched so far only matters once execution actually
// reaches it - and we have never confirmed it does, on that machine. This
// answers that question directly instead of guessing at a fifth gate.
//
// Execution breakpoints (EFLAGS.RF fix applied from the start this time,
// not rediscovered the hard way; see aimhook.cpp/watchwrite.cpp for that
// history) at the entry of specific functions already traced and
// documented in CLAUDE.md.
//
// A/C (the SteamVR/Bigscreen Beyond load-chain investigation's gated
// Oculus-sync function and the load+verify+resolve-exports result) were
// retired 2026-09-04, freeing DR0-DR2 for the player-entity capture below -
// their findings are fully preserved in CLAUDE.md and cost nothing to
// re-add later if a similar issue resurfaces for a future user. D (the
// actual, still-load-bearing handle-substitution production fix) is on
// DR3, completely independent of the others both in this handler and in
// DiagArmThread's per-register enable logic - confirmed before removing
// A/C, not assumed.
//
// DR1 was REPURPOSED 2026-09-05, later the same session: the readiness-gate
// watch (see its own history just below) fully served its diagnostic
// purpose - the mechanism is completely understood and documented
// (ENGINE_ARCHITECTURE.md's "Slot 163" section) and no longer needs live
// monitoring, unlike D and the rd3d11 fix, which are ongoing production
// fixes. DR1 then went to a one-time streaming-batch watch at
// `rd3d11+0x42ba0`, itself retired the same day. The readiness-gate, streaming-
// batch, entity-capture and VisRange watches were all REMOVED 2026-11-15
// (removal batch 3); only D (DR3) and the rd3d11 fix (DR2) remain below.
//
// DR2 (below) was added 2026-09-05, same session as DR1: with the readiness
// gate (DR1) confirmed FALSE on the friend's machine, RTTI-resolving the
// queried object at runtime (ResolveRttiClassName, a live version of this
// project's own long-standing static vtable-walk technique) identified it as
// `SRendererAPI` - and vtable slot 0x518 is 0x518/8 = **slot 163**, the
// EXACT same slot this document's own "Slot 163" section already identified
// as the real Oculus/OVR bring-up function (`FUN_18003b4e0` in
// `rd3d11_x64_rwdi.dll`, which calls `ovr_Create` itself). Decompiling it in
// full (previously only partially traced) found it has its own top-of-
// function call to `FUN_18004a970` - which turned out, on decompile, to be
// **rd3d11's own entirely separate copy of the LoadLibraryW+WinVerifyTrust+
// resolve-every-ovr_*-export chain** (own cached handle at
// `rd3d11+0x480e00`, returns the same `0xfffff447`/-3001 sentinel on
// failure) - completely independent of engine's copy, and NEVER touched by
// the existing D-register substitution fix. If rd3d11's own LoadLibraryW
// call fails the same way engine's used to (near-certain, same underlying
// cause), slot 163 gets iVar5=-3001, skips its own `ovr_Initialize` call
// entirely, and returns false immediately - exactly matching the observed
// `AL=0x00`. DR2 mirrors the exact same substitution fix for this second,
// previously-missed load chain: `rd3d11+0x4A91D` (confirmed via decompile -
// identical `mov rcx,rbx` / `mov rdi,rax` pattern right after rd3d11's own
// `LoadLibraryW` call, same as engine's `+0x60ceed`), reusing the same
// already-successful `g_preloadedOvrRuntime` handle.
//
// DR1 (B, below) was re-added 2026-09-05: a friend's retest with the D-fix
// in place got further than ever before (ovr_Initialize genuinely succeeds,
// 3 real minutes of headset-on gameplay logged) but still never reaches
// ovr_Create - a new blocker downstream of the load chain. Decompiling
// engine+0x2E6010 (the real Oculus bring-up function - what was previously
// called "function A" via its own entry-point diagnostic turned out, once
// actually decompiled, to be an unrelated property-sync broadcaster; THIS
// function is the one that calls the load chain and ovr_Initialize
// directly) found a second gate immediately after ovr_Initialize succeeds:
// a virtual call through a different global object (engine+0xA40218,
// vtable slot +0x518) whose boolean result, if false, makes the function
// silently mark VR as not-ready and return - before ever reaching the
// ovr_Create/HMD-descriptor setup further down in the same function. Exact
// instructions, confirmed via Ghidra decompile of this exact engine build:
//   engine+0x2E6174  mov rcx,[engine+0xA40218]
//   engine+0x2E617B  mov rax,[rcx]
//   engine+0x2E617E  call [rax+0x518]          ; the gate check itself
//   engine+0x2E6184  test al,al                ; <- DR1 watches HERE, so AL
//                                                  is the call's raw return
//                                                  value, untouched by TEST
//   engine+0x2E6186  jz <bail-out, marks not-ready, returns>
// Bit 1 (B1) in Dr6 signals a DR1 hit, same pattern as every other register
// here.
namespace diag {

// 2026-11-15 (removal batch 3): only the two load-chain fixes (D on DR3, R on
// DR2) remain in this namespace. The entity capture, readiness-gate,
// streaming-batch and VisRange watches described above were retired and removed.

// Both load-chain fixes fire rarely (once per Oculus bring-up attempt, not per
// frame) - a generous cap lets a minutes-long retry sequence be served in full.
constexpr LONG kDiagMaxHits = 500;
volatile LONG g_diagHitsD = 0;
volatile LONG g_diagHitsRd3d11 = 0;
volatile LONG g_diagExpired = 0;
// D watches engine+0x60CEED, right after `call LoadLibraryW` inside the
// engine's own OVR-runtime load chain (confirmed live via a raw byte dump
// of this exact engine binary - `74 1E / 48 8D 4C 24 40 / FF 15 xx xx xx
// xx / 48 8B CB / 48 8B F8` at +0x60cedd: the (patched-out) je,
// `lea rcx,[rsp+0x40]` building the path arg, `call [rip+disp32]` =
// LoadLibraryW, then `mov rcx,rbx` / `mov rdi,rax` - so RAX at
// engine+0x60ceed, before that `mov rdi,rax` runs, is LoadLibraryW's own
// raw return value, untouched by anything else yet). Null here means
// LoadLibraryW itself is failing on this machine (see CLAUDE.md's SteamVR/
// Bigscreen Beyond section) - the actual, still-load-bearing production
// fix (substituting our own already-successful PreloadOvrRuntime() handle)
// lives in this hit's own handler below.
uintptr_t g_diagAddrD = 0;
// rd3d11's own, entirely separate LoadLibraryW-result fix point (rd3d11+
// 0x4A91D - see the namespace comment above). Starts at 0; filled in by
// DiagWatcherThread's own polling loop the first time rd3d11_x64_rwdi.dll
// becomes mapped (it loads dynamically, ~0.4s in per Rd3d11Watchdog's own
// already-established timing).
uintptr_t g_diagAddrRd3d11Fix = 0;
PVOID g_diagVeh = nullptr;

// Shared by both load-chain fix points (engine+0x60ceed and, as of
// 2026-09-05, rd3d11+0x4a91d - see the namespace comment above for why a
// second one was needed). Both sit at the identical `mov rdi,rax` instant
// right after their own binary's own LoadLibraryW call returns, so RAX here
// is always that call's untouched raw result - substitutes
// g_preloadedOvrRuntime into ctx->Rax if it's NULL (or if the dev-only
// force_substitute.txt flag file is present), exactly as already proven
// sound for the engine's own copy of this fix. `siteLabel` is purely for
// the log line (e.g. "engine+0x60ceed" or "rd3d11+0x4a91d").
//
// dlvr\disable_substitute.txt (2026-09-05, dev-only, mirrors
// force_substitute.txt's own shape exactly): when present, skips the
// substitution unconditionally, regardless of what loadLibResult or
// force_substitute.txt say - built specifically to test whether the ORIGINAL
// ERROR_ACCESS_DENIED bug reproduces on a machine that has never shown it
// before (this dev machine) once a real headset (a Valve Index, via
// SteamVR - untested combination here) is actually connected, without the
// fix silently masking a real reproduction. The log line still prints
// GetLastError plainly either way, so a raw failure is visible even though
// nothing gets patched around it. Remove the flag file to return to normal,
// fixed behavior - no rebuild needed either direction.
void HandleOvrLoadLibraryHit(CONTEXT* ctx, LONG n, const char* siteLabel)
{
    const uintptr_t loadLibResult = static_cast<uintptr_t>(ctx->Rax);
    // GetLastError() first, before LOG()'s own CRT calls have a chance to
    // clobber it.
    const DWORD err = loadLibResult ? 0 : GetLastError();
    char hostPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, hostPath, MAX_PATH);
    char* hostSlash = strrchr(hostPath, '\\');
    bool forceForTesting = false;
    bool substitutionDisabled = false;
    if (hostSlash) {
        char flagPath[MAX_PATH + 32];
        _snprintf_s(flagPath, sizeof(flagPath), _TRUNCATE, "%.*s\\dlvr\\force_substitute.txt",
                   static_cast<int>(hostSlash - hostPath), hostPath);
        forceForTesting = GetFileAttributesA(flagPath) != INVALID_FILE_ATTRIBUTES;
        _snprintf_s(flagPath, sizeof(flagPath), _TRUNCATE, "%.*s\\dlvr\\disable_substitute.txt",
                   static_cast<int>(hostSlash - hostPath), hostPath);
        substitutionDisabled = GetFileAttributesA(flagPath) != INVALID_FILE_ATTRIBUTES;
    }
    bool substituted = false;
    if (!substitutionDisabled && (loadLibResult == 0 || forceForTesting) && g_preloadedOvrRuntime) {
        ctx->Rax = reinterpret_cast<uintptr_t>(g_preloadedOvrRuntime);
        substituted = true;
    }
    const bool stillMapped = GetModuleHandleA("LibOVRRT64_1.dll") != nullptr;
    const DWORD tid = GetCurrentThreadId();
    HANDLE threadToken = nullptr;
    bool impersonating = false;
    DWORD tokenErr = 0;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &threadToken)) {
        impersonating = true;
        CloseHandle(threadToken);
    } else {
        tokenErr = GetLastError();
    }
    LOG("diag: LoadLibraryW returned (%s) - hit #%ld, handle = %s, "
        "GetLastError = %lu, our own preloaded copy still mapped right now = %s, "
        "calling thread id = %lu, thread impersonating a token = %s (OpenThreadToken "
        "GetLastError if not = %lu)%s%s", siteLabel, n,
        loadLibResult ? "non-null (succeeded)" : "NULL (FAILED)",
        err, stillMapped ? "YES" : "NO", tid, impersonating ? "YES" : "no", tokenErr,
        substituted ? " - SUBSTITUTED our own preloaded handle, this call proceeds as if its "
                      "own load had succeeded"
                    : "",
        substitutionDisabled ? " - disable_substitute.txt present, substitution SKIPPED "
                               "unconditionally (raw/unmasked result above)"
                             : "");
}

LONG CALLBACK DiagHandler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = ep->ContextRecord;
    const bool hitRd3d11 = g_diagAddrRd3d11Fix && (ctx->Dr6 & 0x4) != 0;
    const bool hitD = g_diagAddrD && (ctx->Dr6 & 0x8) != 0;
    // DR0 / DR1 are not ours any more (2026-11-15): a hit on either is passed
    // on untouched, as every other handler of ours does for a register it did
    // not arm. (The old staleDr1 self-heal existed only for the retired
    // streaming-batch watch on DR1.)
    if (!hitRd3d11 && !hitD) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ctx->Dr6 = 0;
    // Execution breakpoints are fault-class: they fire BEFORE the flagged
    // instruction runs, RIP still pointing at it. Resuming without RF set
    // lets the debug hardware immediately re-match the same still-current
    // RIP and re-raise before the instruction ever actually executes -
    // exactly the bug that made unrelated call sites look "impossibly hot"
    // earlier tonight. Set unconditionally, for every breakpoint, from the
    // start this time.
    ctx->EFlags |= 0x10000;

    // Whatever machine-specific reason a binary's OWN LoadLibraryW call has
    // for failing (see CLAUDE.md's SteamVR/Bigscreen Beyond section -
    // twelve+ distinct hypotheses tried and ruled out for engine's copy, no
    // single Windows-build-specific cause ever pinned down), our OWN plain
    // LoadLibraryA call on this exact file has succeeded on every single
    // test, on every machine, all session - PreloadOvrRuntime() already
    // holds a real, valid, working handle for it. HandleOvrLoadLibraryHit
    // substitutes that handle into RAX right here, before the instruction
    // after this breakpoint (`mov rdi,rax`, identical in both binaries)
    // reads it, for whichever of the two independent load chains just hit.
    if (hitRd3d11) {
        HandleOvrLoadLibraryHit(ctx, InterlockedIncrement(&g_diagHitsRd3d11), "load-chain fix R");   // BETA: label, no address
    }
    if (hitD) {
        HandleOvrLoadLibraryHit(ctx, InterlockedIncrement(&g_diagHitsD), "load-chain fix E");   // BETA: label, no address
    }

    if (g_diagHitsRd3d11 + g_diagHitsD > kDiagMaxHits) {
        ctx->Dr7 &= ~static_cast<DWORD64>(0xFFFF0055);
        ctx->Dr0 = 0;
        ctx->Dr1 = 0;
        ctx->Dr2 = 0;
        ctx->Dr3 = 0;
        InterlockedExchange(&g_diagExpired, 1);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Cannot Get/SetThreadContext a thread on itself, so this always misses the
// thread it is called from - fine here, since it is only ever called from
// the watcher thread below, a thread whose own execution we have no reason
// to watch.
bool DiagArmThread(DWORD tid)
{
    if (tid == GetCurrentThreadId()) {
        return false;
    }
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                          FALSE, tid);
    if (!t) {
        return false;
    }
    SuspendThread(t);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (GetThreadContext(t, &ctx)) {
        // DR0 / DR1 are written 0 and their DR7 fields cleared, exactly as the
        // shipped config did before the retired watches were removed
        // (2026-11-15, batch 3: E4 - zero behaviour change in a removal).
        ctx.Dr0 = 0;
        ctx.Dr1 = 0;
        ctx.Dr2 = g_diagAddrRd3d11Fix;
        ctx.Dr3 = g_diagAddrD;
        // RW/LEN must be 00 for an execution breakpoint. Touches all four
        // registers' own bits in DR7 now - L0 (bit0), L1 (bit2), L2 (bit4),
        // L3 (bit6), RW0/LEN0 (bits16-19), RW1/LEN1 (bits20-23), RW2/LEN2
        // (bits24-27), RW3/LEN3 (bits28-31). DR2 (rd3d11's own load-chain
        // fix, added 2026-09-05 - see the namespace comment above) is the
        // last of the four registers freed by the 2026-09-04 A/C retirement;
        // none are left spare after this.
        constexpr DWORD64 kAllFourDrMask = 0xFFFF0055ull;  // L0|L1|L2|L3|RW0-3/LEN0-3
        ctx.Dr7 &= ~kAllFourDrMask;
        DWORD64 enable = 0;
        if (g_diagAddrRd3d11Fix) {
            enable |= (1ull << 4);
        }
        if (g_diagAddrD) {
            enable |= (1ull << 6);
        }
        ctx.Dr7 |= enable;
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ok = SetThreadContext(t, &ctx) != 0;
    }
    ResumeThread(t);
    CloseHandle(t);
    return ok;
}

void DiagArmAllThreadsOnce()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                DiagArmThread(te.th32ThreadID);
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// A single arm-all pass at install time only catches threads that already
// exist. The engine spawns several more afterwards (dlvr_engine.log shows
// "dedicated worker threads: 1 + 7"), and the traced functions could run on
// any of them - so this re-scans every 100ms, called from a genuinely
// different thread each time, which is what lets it arm the thread that
// installed the watch too (see DiagArmThread's own comment). Stops itself
// once the hit cap trips or after 90 seconds - a one-shot diagnostic, not a
// permanent feature.
//
// IMPORTANT (2026-09-05): once rd3d11's addresses are armed and a handful of
// extra passes have run (to catch any thread spawned right around that
// load), this loop stops re-arming entirely rather than continuing every
// 100ms for the full 90 seconds - see passesSinceRd3d11Found's own comment
// below for why. Real crash, confirmed via Windows Event Viewer: the
// streaming-batch watch (rd3d11+0x42ba0) fires far more often than assumed
// when this was designed - well over 1000 Hz even sitting at the main menu,
// not "roughly once per frame" - and this loop's own
// SuspendThread/GetThreadContext/SetThreadContext/ResumeThread cycle on
// every thread, every 100ms, raced with live exception delivery on whatever
// thread it happened to suspend mid-dispatch. Crashed the process twice in
// a row this session (once near the safety cap, once well before it -
// ruling out the cap trip itself as the sole cause). Continuing to
// periodically re-arm threads that already have the right addresses set is
// not needed for anything this watch cares about (unlike entity capture,
// which genuinely needs to catch a construction event on a thread that may
// not exist yet) - stopping the sweep once it's done its job removes the
// race window for the remainder of the session.
DWORD WINAPI DiagWatcherThread(LPVOID)
{
    int passesSinceRd3d11Found = 0;
    for (int i = 0; i < 900 && !g_diagExpired; ++i) {
        // rd3d11_x64_rwdi.dll loads dynamically (Rd3d11Watchdog's own
        // 1ms polling already established ~0.4s as the observed timing,
        // well before slot 163 - ~0.68s - would ever be invoked, so this
        // coarser 100ms polling still arms well in advance). Same
        // fill-in-once-then-stop-checking pattern as gamedll above.
        if (!g_diagAddrRd3d11Fix) {
            if (HMODULE rd3d11 = GetModuleHandleA("rd3d11_x64_rwdi.dll")) {
                const uintptr_t rd3d11Base = reinterpret_cast<uintptr_t>(rd3d11);
                g_diagAddrRd3d11Fix = rd3d11Base + 0x4A91D;
                LOG("diag: rd3d11_x64_rwdi.dll now mapped - armed its own, independent "
                    "load-chain fix at rd3d11+0x4a91d (mirrors the engine+0x60ceed fix - "
                    "see the namespace comment above)");
            }
        }
        DiagArmAllThreadsOnce();
        if (g_diagAddrRd3d11Fix) {
            ++passesSinceRd3d11Found;
        }
        // Stop the periodic re-arm sweep once rd3d11's addresses are armed
        // and a few extra passes have run to catch any thread spawned right
        // around that load - see this function's own comment above for the
        // real crash this fixes.
        if (passesSinceRd3d11Found >= 5) {
            LOG("diag: watcher thread stopping its periodic re-arm sweep - rd3d11's "
                "address is armed, so there's no remaining reason to keep suspending "
                "threads (avoids the SuspendThread/live-exception race that crashed the "
                "process twice - see this function's own comment)");
            break;
        }
        Sleep(100);
    }
    return 0;
}

void StartRenderRaceDiagnostic(HMODULE engine)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
    g_diagAddrD = base + ::dlvr::buildrva::Eng(0x60CEED);   // the load-chain fix; its EGL rva on the Epic build
    // (The VisRange DR0 watch and its dlvr\no_visrange_watch.txt switch were
    // removed 2026-11-15, batch 3. The marker file may still exist; nothing
    // reads it.)
    g_diagVeh = AddVectoredExceptionHandler(1, DiagHandler);
    if (!g_diagVeh) {
        LOG("diag: could not install exception handler - skipping");
        return;
    }
    DiagArmAllThreadsOnce();
    CreateThread(nullptr, 0, DiagWatcherThread, nullptr, 0, nullptr);
    LOG("diag: watching engine+0x60ceed (D, LoadLibraryW's own raw return value) and "
        "rd3d11+0x4a91d (rd3d11's own independent load-chain fix, armed once rd3d11 is "
        "mapped) - re-arming new threads for up to 90s or %ld combined hits",
        kDiagMaxHits);
}

}  // namespace diag

}  // namespace

// ---------------------------------------------------------------- exports ----

extern "C" {

DWORD WINAPI XInputGetState(DWORD i, void* s)
{
    if (!g_getState) {
        return kNoDevice;
    }
    const DWORD r = g_getState(i, s);
    // Mod-menu input block, run 2: "gamepad filter:" has never printed, so this path's role in play is unread.
    // One line on the first call and one on the first success settle whether the pad reaches the engine here.
    static volatile LONG s_firstCall = 0, s_firstOk = 0;
    if (s_firstCall == 0 && InterlockedExchange(&s_firstCall, 1) == 0) {
        LOG("menu input block: engine XInputGetState FIRST CALL (user %lu) -> %lu (0 = connected, 1167 = no device)",
            static_cast<unsigned long>(i), static_cast<unsigned long>(r));
    }
    if (r == ERROR_SUCCESS && s_firstOk == 0 && InterlockedExchange(&s_firstOk, 1) == 0) {
        LOG("menu input block: engine XInputGetState FIRST SUCCESS (user %lu) - the pad reaches the engine through "
            "XInput; dlvr_FilterGamepad blocks it while the menu is open", static_cast<unsigned long>(i));
    }
    if (r == ERROR_SUCCESS && s) {
        FilterGamepad(s);
    }
    return r;
}

DWORD WINAPI XInputSetState(DWORD i, void* v)
{
    return g_setState ? g_setState(i, v) : kNoDevice;
}

DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void* c)
{
    return g_getCapabilities ? g_getCapabilities(i, f, c) : kNoDevice;
}

void WINAPI XInputEnable(BOOL e)
{
    if (g_enable) {
        g_enable(e);
    }
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, GUID* r, GUID* c)
{
    return g_getDSoundGuids ? g_getDSoundGuids(i, r, c) : kNoDevice;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD i, BYTE d, void* b)
{
    return g_getBattery ? g_getBattery(i, d, b) : kNoDevice;
}

DWORD WINAPI XInputGetKeystroke(DWORD i, DWORD r, void* k)
{
    return g_getKeystroke ? g_getKeystroke(i, r, k) : kNoDevice;
}

}  // extern "C"

// pakgen.cpp - regenerates DW\Data3.pak's render_loop.ppfx for the selected
// DLSS quality mode, sized from the USER's resolution rather than the
// developer's. Absent marker (dlvr\dlss_quality.txt) = it does nothing at all.
void GenerateRenderLoopPak(const char* dlvrDir, const char* gameDir);
// pakgen.cpp - early_eye_size: hooks the engine's filesystem opens so the first open of DW\dlvr_pp.mth gets THIS launch's
// per-eye size. Installs nothing unless pakgen's routing wanted it (default ON; dlvr\early_eye_size_off.txt = off).
void InstallEarlyEyeSizeHook();
// skillpatch.cpp - grapple_unlock_skill: the game's common_skills.xml with the grappling hook skill unlocked, written to DW\ and
// opened in its place through the same open hooks. Reads ovr2xr.txt as text; off = nothing read or written.
void PrepareSkillPatch(const char* gameDir);

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);

        char host[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, host, MAX_PATH);
        LOG("--- dlvr bootstrap attached to %s ---", host);
        LogSystemInfo(host);

        // >>> FIRST, BEFORE ANYTHING ELSE TOUCHES THE GAME. <<< The engine
        // STATICALLY IMPORTS this DLL, so DLL_PROCESS_ATTACH runs before the
        // engine's entry point and therefore before it has opened any pak. That
        // is the only window in which Data3.pak can be rewritten for THIS
        // launch. The ordering is ASSERTED BY THE LOG rather than assumed:
        // PAKGEN prints the scene size it wrote and [DLSSFLAT] prints the size
        // the engine actually used - if they disagree, the window is wrong.
        {
            char gdir[MAX_PATH];
            if (ModuleDir(self, gdir, sizeof(gdir))) {
                char ddir[MAX_PATH];
                _snprintf_s(ddir, sizeof(ddir), _TRUNCATE, "%s/dlvr", gdir);
                GenerateRenderLoopPak(ddir, gdir);
                PrepareSkillPatch(gdir);   // before the hooks: they go in for it too
                InstallEarlyEyeSizeHook();
            }
        }

        PreloadOvrRuntime(self);

        LoadRealXInput();
        LOG("real XInput: %s", g_realXInput ? "loaded" : "NOT LOADED (pad will be dead)");
        ::dlvr::hangwatch::Start(g_realXInput);   // DLVR_HANG_WATCH build only; empty otherwise

        InstallEngineLogHook();
        InstallScePadHook();
        InstallSdlPollEventHook();
        menublock::Install();
        InstallCreateRendererHook();
        LogEngineIdentity();
        PatchEngineSignatureGate();
        PatchEngineOculusGate();
        PatchEngineRendererRaceGate();
        if (HMODULE diagEngine = GetModuleHandleA("engine_x64_rwdi.dll")) {
            diag::StartRenderRaceDiagnostic(diagEngine);
        }
        // Must be read BEFORE the watchdog starts - the watchdog is what calls
        // dlssflat's installer, and it can fire within a millisecond.
        ::dlvr::dlssflat::ReadMarker();
        StartRd3d11Watchdog();
        InjectExtraArgs(self);
        SeedEpicProfileName();
        StartEpicMenuNameHook();
        StartEpicProfileProbe(self);
        EnsureVideoScrHasOculusEnabled();

        char dir[MAX_PATH];
        if (ModuleDir(self, dir, sizeof(dir))) {
            LoadForceConfig(dir);
        }
    }
    return TRUE;
}
