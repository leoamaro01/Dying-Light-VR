// Updates (spec docs/superpowers/specs/2026-11-15-launcher-design.md section 10): where releases come from, GitHub's
// release JSON, the staged-release check, the update and rollback plans, and the self-update decisions. Jay 2026-11-15:
// the git repo "will have the built files aswell, when we publish it"; "We haven't set the URL yet or published the repo"
// - so kRepoUrl is EMPTY and every path also takes a local release folder (--release <dir>). Pure and header-only - tested
// by src_beta/src/tests/launcher_update_test.cpp.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "launcher_json.h"
#include "launcher_mode.h"
#include "launcher_release.h"
#include "launcher_unzip.h"
#include "setup_settings.h"

namespace dlvr::launcher {

// The GitHub repo's API URL for its latest release - the ONE place it is set (spec 12.1; repo created 2026-11-15, PRIVATE at
// first: until it is public the API answers 404 to the launcher, which does not sign in, and CheckForUpdates says so).
// Fork 2026-11-28: the "owner/name" part comes from the CMake cache variable DLVR_UPDATE_REPO (passed as a compile
// definition to the launcher target only), so a fork can build a launcher that installs and updates from its own
// releases without editing this file. Undefined (tests, other targets) = upstream, which keeps the public launcher's
// behaviour and the test's expected URL unchanged.
#ifndef DLVR_UPDATE_REPO
#define DLVR_UPDATE_REPO "bymodify-ux/Dying-Light-VR"
#endif
inline const char* kRepoUrl = "https://api.github.com/repos/" DLVR_UPDATE_REPO "/releases/latest";

inline const char* kPreviousDir = "dlvr/previous/";

// The local release folder: an explicit --release wins; else the exe's own folder when a release.txt sits beside it (Jay
// 2026-11-15: open the exe directly from a release folder, no start script); else none.
inline std::wstring ReleaseDirFor(const std::wstring& argDir, const std::wstring& exeDir, bool releaseTxtBesideExe)
{
    if (!argDir.empty()) { return argDir; }
    return releaseTxtBesideExe ? exeDir : std::wstring();
}

enum class UpdateSource { None, Local, GitHub };

inline UpdateSource UpdateSourceFor(const std::string& repoUrl, const std::wstring& localReleaseDir)
{
    if (!localReleaseDir.empty()) { return UpdateSource::Local; }
    return repoUrl.empty() ? UpdateSource::None : UpdateSource::GitHub;
}

struct LatestInfo {
    std::string tag, manifestUrl, notesUrl, zipUrl, launcherUrl, schemaUrl, redistUrl, resetsUrl, retiredUrl;
};

// GitHub "releases/latest" JSON. Refused (naming the asset) when release.txt, files.zip, the launcher or the schema is missing.
inline bool ParseLatestRelease(const std::string& jsonText, LatestInfo* out, std::string* why)
{
    json::Value v;
    if (!json::Parse(jsonText, &v, why)) { return false; }
    LatestInfo li;
    li.tag = v.Get("tag_name").Str();
    if (li.tag.empty()) { *why = "the release has no tag_name"; return false; }
    const json::Value& as = v.Get("assets");
    for (size_t i = 0; i < as.Size(); ++i) {
        const std::string n = as.At(i).Get("name").Str();
        const std::string u = as.At(i).Get("browser_download_url").Str();
        if (n == "release.txt") { li.manifestUrl = u; }
        else if (n == "notes.txt") { li.notesUrl = u; }
        else if (n == "files.zip") { li.zipUrl = u; }
        else if (n == "DyingLightVR_Launcher.exe") { li.launcherUrl = u; }
        else if (n == "settings_schema.json") { li.schemaUrl = u; }
        else if (n == "vc_redist.x64.exe") { li.redistUrl = u; }
        else if (n == "resets.tsv") { li.resetsUrl = u; }
        else if (n == "retired.txt") { li.retiredUrl = u; }
    }
    const std::pair<const char*, const std::string*> need[] = {{"release.txt", &li.manifestUrl}, {"files.zip", &li.zipUrl},
                                                              {"DyingLightVR_Launcher.exe", &li.launcherUrl},
                                                              {"settings_schema.json", &li.schemaUrl}};
    for (const auto& n : need) {
        if (n.second->empty()) { *why = std::string("the release ") + li.tag + " has no " + n.first; return false; }
    }
    *out = li;
    return true;
}

inline bool NeedsUpdate(const std::string& installedRelease, const std::string& latestTag)
{
    return !latestTag.empty() && installedRelease != latestTag;
}

// Every release file missing from the staged folder or with another md5 (stagedMd5: lower-case path -> md5). Empty = the
// whole release is good, and only then is the game folder touched (Review Focus 5).
inline std::vector<std::string> StagedProblems(const Release& r, const std::map<std::string, std::string>& stagedMd5)
{
    std::vector<std::string> bad;
    for (const auto& f : r.files) {
        const auto it = stagedMd5.find(detail::LowerPath(f.relPath));
        if (it == stagedMd5.end() || it->second != f.md5) { bad.push_back(f.relPath); }
    }
    return bad;
}

// release.txt's text for a Release (kept in dlvr/previous/ so a rollback knows what to put back).
inline std::string ReleaseManifestText(const Release& r)
{
    std::string t = "# release=" + r.name + "\r\n";
    if (!r.launcherMd5.empty()) { t += "# launcher=" + r.launcherMd5 + "\r\n"; }
    for (const auto& f : r.files) { t += f.md5 + "  " + f.relPath + (f.dlssOnly ? " dlss-only" : "") + "\r\n"; }
    return t;
}

struct UpdateInputs {
    Release from, to;
    std::string ovr2xrText;                  // dlvr/ovr2xr.txt as found ("" = none)
    std::string writtenText;                 // dlvr/setup_settings_written.txt ("" = none)
    std::vector<setup::ResetRow> resets;     // the release's resets.tsv
    std::vector<std::string> retiredKeys;
    std::set<std::string> presentLower;      // game-folder files, lower-case
    bool flatMode = false;
    std::vector<std::string> differing;      // MatchInstalled's list: re-copied even when both releases ship the same md5
};

namespace detail {

inline std::string Target(const std::string& rel, bool flat)
{
    return (flat && LowerPath(rel).compare(0, 5, "dlvr/") != 0) ? std::string(kFlatDir) + rel : rel;
}

inline const ManifestEntry* FindFile(const Release& r, const std::string& path)
{
    for (const auto& f : r.files) {
        if (LowerPath(f.relPath) == LowerPath(path)) { return &f; }
    }
    return nullptr;
}

}  // namespace detail

// The changed / removed files of `from` go to dlvr/previous/ (cleared first), the new / changed files of `to` are copied
// (to dlvr/flat_off/ in flat mode), a user marker already on disk is never touched, and ovr2xr.txt keeps the user's values
// with old shipped defaults moved to today's and retired keys removed (the setup's keep-install contract).
inline std::vector<LAction> PlanUpdate(const UpdateInputs& in)
{
    std::vector<LAction> out;
    out.push_back({LAct::Delete, kPreviousDir, "", "", "", false});
    auto present = [&](const std::string& p) { return in.presentLower.count(detail::LowerPath(p)) > 0; };
    auto userMarker = [](const std::string& p) { return setup::detail::IsUserMarker(detail::LowerPath(p)); };
    for (const auto& f : in.from.files) {
        const ManifestEntry* g = detail::FindFile(in.to, f.relPath);
        if ((g && g->md5 == f.md5) || userMarker(f.relPath)) { continue; }
        const std::string t = detail::Target(f.relPath, in.flatMode);
        if (present(t)) { out.push_back({LAct::Move, t, std::string(kPreviousDir) + f.relPath, "", "", false}); }
    }
    auto damaged = [&](const std::string& p) {
        for (const auto& d : in.differing) {
            if (detail::LowerPath(d) == detail::LowerPath(p)) { return true; }
        }
        return false;
    };
    for (const auto& g : in.to.files) {
        const ManifestEntry* f = detail::FindFile(in.from, g.relPath);
        const std::string t = detail::Target(g.relPath, in.flatMode);
        if (f && f->md5 == g.md5 && present(t) && !damaged(g.relPath)) { continue; }
        if (userMarker(g.relPath) && present(t)) { continue; }
        if (g.dlssOnly && !present(t)) { continue; }   // only where it was (an AMD install has none)
        out.push_back({LAct::Copy, g.relPath, t, g.md5, "", false});
    }
    out.push_back({LAct::WriteText, std::string(kPreviousDir) + "release.txt", "", "", ReleaseManifestText(in.from), false});
    if (!in.ovr2xrText.empty()) {
        std::vector<setup::Moved> moved;
        const std::string reset = setup::ApplyOldDefaultResets(in.ovr2xrText, in.resets, {}, &moved,
                                                               in.writtenText.empty() ? nullptr : &in.writtenText);
        const std::string merged = setup::MergeSettings(reset, {}, in.retiredKeys);
        if (merged != in.ovr2xrText) { out.push_back({LAct::WriteText, "dlvr/ovr2xr.txt", "", "", merged, false}); }
    }
    return out;
}

// previousPresentLower = files under dlvr/previous/ (lower-case, with the prefix).
inline std::vector<LAction> PlanRollback(const Release& prev, const Release& cur, const std::set<std::string>& previousPresentLower,
                                         bool flatMode, const std::string& recordText)
{
    std::vector<LAction> out;
    for (const auto& f : prev.files) {
        const std::string src = std::string(kPreviousDir) + f.relPath;
        if (previousPresentLower.count(detail::LowerPath(src))) {
            out.push_back({LAct::Move, src, detail::Target(f.relPath, flatMode), "", "", false});
        }
    }
    for (const auto& g : cur.files) {
        if (!detail::FindFile(prev, g.relPath)) { out.push_back({LAct::Delete, detail::Target(g.relPath, flatMode), "", "", "", false}); }
    }
    out.push_back({LAct::WriteText, kLauncherRecord, "", "", recordText, false});
    return out;
}

// Self-update: the running exe renames itself to DyingLightVR_Launcher.old.exe, moves the verified new exe into place and
// starts it with --after-update, then waits for launcher_update_ok.txt (written once the new window is up).
enum class SelfStep { None, DeleteOld, RestoreOld };

inline SelfStep OnStartStep(bool afterUpdate, bool oldPresent)
{
    (void)afterUpdate;
    return oldPresent ? SelfStep::DeleteOld : SelfStep::None;   // a left-over .old is removed once the window is up
}

inline SelfStep OnWaitEndStep(bool newSignalled) { return newSignalled ? SelfStep::None : SelfStep::RestoreOld; }

}  // namespace dlvr::launcher
