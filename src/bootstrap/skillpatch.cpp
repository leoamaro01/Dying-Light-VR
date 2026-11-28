// ---------------------------------------------------------------------------
// SKILLPATCH - grapple_unlock_skill (design §8.21; the decision is skillpatch_logic.h, tests/skillpatch_logic_test.cpp).
//
// WHAT. With grapple_unlock_skill=1 in dlvr\ovr2xr.txt (read here as text: the bootstrap runs before the layer parses it, the
// texture_aniso / ui_wrist_hud pattern), the grappling hook's skill (StatusHook) can be taken at any Survivor rank for no point.
//
// HOW. The engine opens its data by BARE file name through filesystem_x64_rwdi.dll's open exports (pakgen.cpp's early_eye_size log:
// 'render_loop.ppfx', 'dlvr_pp.mth'), and a loose file in DW\ resolves by that name (DW\dlvr_pp.mth, INV-521 / INV-522). So at
// attach this reads the game's OWN common_skills.xml out of the newest DW\DataN.pak that holds it (never a copy shipped with the
// mod), patches it (PatchHookSkill), writes DW\dlvr_common_skills.xml, and pakgen's open hooks hand the engine that name whenever it
// opens common_skills.xml. If that open fails, the hook opens the original name instead (logged once): the game's own skill.
// Nothing in any pak is written. 0 / absent = no file read or written, no redirect - the hooks pass every name through.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "skillpatch_logic.h"
#include "../launcher/launcher_unzip.h"   // ZipEntries / ZipExtract (puff inflate, CRC-checked)
#include "../common/log.h"

#define PLOG(...) ::dlvr::LogTo("dlvr_bootstrap.log", __VA_ARGS__)

namespace {

const char* const kSrcLeaf  = "common_skills.xml";
const char* const kDstName  = "dlvr_common_skills.xml";
const char* const kPakEntry = "data/skills/common_skills.xml";
volatile LONG g_redirectOn = 0;
volatile LONG g_hitSaid = 0, g_failSaid = 0;
volatile LONG g_hits = 0;

bool ReadWhole(const char* path, std::string* out)
{
    out->clear();
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { return false; }
    char buf[65536];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) { out->append(buf, n); }
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

// grapple_unlock_skill from dlvr\ovr2xr.txt: 1 / 0, -1 = no line, -2 = no file.
int ReadSetting(const char* gameDir)
{
    char cfg[MAX_PATH] = {};
    _snprintf_s(cfg, sizeof(cfg), _TRUNCATE, "%s\\dlvr\\ovr2xr.txt", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, cfg, "r") != 0 || !f) { return -2; }
    int value = -1;
    char line[512];
    const char key[] = "grapple_unlock_skill=";
    while (fgets(line, sizeof(line), f)) {
        if (_strnicmp(line, key, sizeof(key) - 1) == 0) {
            int v = -1;
            if (sscanf_s(line + sizeof(key) - 1, "%d", &v) == 1) { value = v > 0 ? 1 : 0; }
        }
    }
    fclose(f);
    return value;
}

const char* Leaf(const char* name)
{
    const char* leaf = name;
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || *p == '\\') { leaf = p + 1; }
    }
    return leaf;
}

}  // namespace

// Called from DllMain right before pakgen's open hooks are installed (InstallEarlyEyeSizeHook installs them when this armed the
// redirect, whatever early_eye_size wants).
void PrepareSkillPatch(const char* gameDir)
{
    const int setting = ReadSetting(gameDir);
    if (setting != 1) {
        // [PROBE: SKILLPATCH] gate=grapple_unlock_skill (ovr2xr.txt, read at launch)  site=FLAT  log-only=yes
        PLOG("[SKILLPATCH] grapple_unlock_skill %s - the grappling hook skill is the game's own (Survivor rank 12, one point)",
             setting == 0 ? "= 0" : (setting == -1 ? "has no line in ovr2xr.txt (default off)" : "- no ovr2xr.txt (default off)"));
        return;
    }
    std::string pak, xml, why, pakName;
    bool found = false;
    for (int n = 9; n >= 0 && !found; --n) {   // the newest pak holding the file is the one the game reads
        char path[MAX_PATH] = {};
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\DW\\Data%d.pak", gameDir, n);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { continue; }
        if (!ReadWhole(path, &pak)) {
            PLOG("[SKILLPATCH] *** could not read %s - skipped ***", path);
            continue;
        }
        std::vector<::dlvr::launcher::ZipEntry> entries;
        if (!::dlvr::launcher::ZipEntries(pak, &entries, &why)) {
            PLOG("[SKILLPATCH] *** %s is not a readable zip (%s) - skipped ***", path, why.c_str());
            continue;
        }
        for (const auto& e : entries) {
            if (_stricmp(e.name.c_str(), kPakEntry) != 0) { continue; }
            if (!::dlvr::launcher::ZipExtract(pak, e, &xml, &why)) {
                PLOG("[SKILLPATCH] *** %s in %s could not be read (%s) ***", kPakEntry, path, why.c_str());
                break;
            }
            found = true;
            pakName = path;
            break;
        }
    }
    pak.clear();
    pak.shrink_to_fit();
    if (!found) {
        PLOG("[SKILLPATCH] *** grapple_unlock_skill = 1 but no DW\\DataN.pak holds a readable %s - the skill stays the game's own ***",
             kPakEntry);
        return;
    }
    std::string out, old;
    int removed = 0;
    if (!::dlvr::skillpatch::PatchHookSkill(xml, &out, &why, &removed, &old)) {
        PLOG("[SKILLPATCH] *** grapple_unlock_skill = 1 but %s from %s was NOT patched: %s - the skill stays the game's own ***",
             kPakEntry, pakName.c_str(), why.c_str());
        return;
    }
    char dst[MAX_PATH] = {};
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\DW\\%s", gameDir, kDstName);
    std::string have;
    const bool same = ReadWhole(dst, &have) && have == out;
    if (!same) {
        FILE* f = nullptr;
        size_t wrote = 0;
        if (fopen_s(&f, dst, "wb") == 0 && f) {
            wrote = fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }
        std::string back;
        if (wrote != out.size() || !ReadWhole(dst, &back) || back != out) {
            PLOG("[SKILLPATCH] *** %s NOT WRITTEN (%zu of %zu bytes, error %lu) - the skill stays the game's own ***", dst, wrote,
                 out.size(), GetLastError());
            return;
        }
    }
    InterlockedExchange(&g_redirectOn, 1);
    // [PROBE: SKILLPATCH] gate=grapple_unlock_skill (ovr2xr.txt, read at launch)  site=FLAT  log-only=no
    PLOG("[SKILLPATCH] grapple_unlock_skill = 1: the game's %s (from %s, %zu bytes) with StatusHook's %d rank requirement(s) removed "
         "and its cost %s -> 0 is %s %s (%zu bytes); the engine's opens of %s are pointed at it (a REDIRECTED line says when)",
         kPakEntry, pakName.c_str(), xml.size(), removed, old.c_str(), same ? "already in" : "written to", dst, out.size(), kSrcLeaf);
}

bool SkillPatchWanted() { return g_redirectOn != 0; }

// pakgen.cpp's open hooks: the name to open instead of `name` (nullptr = open `name` as asked). via names the hook (log).
const char* SkillPatchRedirect(const char* name, const char* via)
{
    if (!g_redirectOn || !name || _stricmp(Leaf(name), kSrcLeaf) != 0) { return nullptr; }
    const LONG k = InterlockedIncrement(&g_hits);
    if (InterlockedCompareExchange(&g_hitSaid, 1, 0) == 0) {
        // [PROBE: SKILLPATCH] gate=grapple_unlock_skill (ovr2xr.txt, read at launch)  site=FLAT  log-only=yes
        PLOG("[SKILLPATCH] REDIRECTED the engine's open of '%s' (via %s, thread %lu) to '%s' (open #%ld; later ones are not logged)",
             name, via, GetCurrentThreadId(), kDstName, k);
    }
    return kDstName;
}

// The redirected open failed: said once; the hook then opens the original name.
void SkillPatchRedirectFailed(const char* name, const char* via)
{
    if (InterlockedCompareExchange(&g_failSaid, 1, 0) == 0) {
        PLOG("[SKILLPATCH] *** the redirected open of '%s' FAILED (via %s) - '%s' was opened instead: the skill stays the game's "
             "own *** (logged once)", kDstName, via, name ? name : "?");
    }
}
