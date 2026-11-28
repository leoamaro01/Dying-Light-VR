// ---------------------------------------------------------------------------
// PAKGEN - the DLSS quality mode, generated from the USER's OWN resolution.
//
// WHY THIS EXISTS. The upscale ratio is a property of the DATA: the engine's
// scene buffers are declared at an explicit pixel size in render_loop.ppfx, and
// dlssflat's detector DERIVES the DLSS mode from the ratio it measures between
// that size and the bound output target. Proven live at two ratios on ONE
// binary - 1706x960 -> "mode Quality (1.50x), error 0.001" and 1488x838 ->
// "mode Balanced (1.72x), error 0.000". Nothing in the code knows 2560x1440.
//
// So the only thing between "works here" and "works for a public build" is that
// those pixel sizes were computed BY HAND from the developer's own display.
// This file computes them at startup from whatever resolution the user runs.
//
// WHAT IT DELIBERATELY DOES NOT DO. It does NOT reproduce the structural
// surgery that makes upscaling possible (the full-res OUT_SRGB buffer, the UI
// pass lifted out of the half-res chain, the eye copy retargeted). That surgery
// is IDENTICAL for every mode, so it is baked into the embedded template and
// cannot drift per-user. Only ~56 numbers change. The template was validated by
// substituting the Balanced numbers into it and getting the ppfx that ran clean
// at 1488x838 BYTE FOR BYTE.
//
// HOW IT WRITES. render_loop.ppfx is the LAST entry of DW\Data3.pak, STORED
// (uncompressed) and PADDED to a fixed byte length. Because the length is
// fixed, nothing after the entry moves, so the update is: overwrite the bytes,
// recompute CRC32, patch it into the local header and the central directory.
// No ZIP writer, no compression library, and the other nine entries are never
// touched. Verified transparent to the engine's parser by a controlled flat run
// (STORED + 9,777 trailing newlines produced a byte-identical result).
//
// TIMING. The bootstrap is mapped before the game's entry point, because
// engine_x64_rwdi.dll statically imports it - so this runs before the engine has
// opened any pak. That ordering is ASSERTED BY THE LOG, not assumed: the write
// is logged, and the engine's own resulting GBuffer size is printed by
// [DLSSFLAT] later in the same log. If the two disagree the window is wrong,
// and the log says to check rather than leaving a silently stale pak.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <wincrypt.h>   // [DATA3]: CryptoAPI MD5 of the whole Data3.pak (advapi32, a default library)
#include <stdio.h>
#include <string>
#include <vector>
#include <string.h>
#include <share.h>
#include <ctype.h>
#include <algorithm>
#include "render_loop_tmpl.h"
#include "render_loop_div_tmpl.h"
#include "fixed_data3_layout.h"
#include "fixed_data3_md5.h"
#include "../common/log.h"
#include "../common/iathook.h"
#include "../common/binds_logic.h"   // WriteBindsScr: dlvr\binds.txt -> DW\dlvr_binds.scr (Controls tab)
#include "aamethod.h"

#define PLOG(...) ::dlvr::LogTo("dlvr_bootstrap.log", __VA_ARGS__)

namespace {

char g_dlvrDir[MAX_PATH] = {};
char g_gameDir[MAX_PATH] = {};
int  g_wroteW = 0, g_wroteH = 0;   // the OUTPUT size the current pak was written for
// What was ASKED for vs what this launch actually got - see DlvrGetActiveQuality.
int  g_requestedMode = -1, g_activeMode = -1, g_activeDiv = 0, g_activeW = 0, g_activeH = 0;

// >>> FIXED DATA3 + DW\dlvr_pp.mth - docs/DESIGN-COOP-FIXED-DATA3-2026-10-10.md, step 2. 2026-10-10. U-56 fix. <<<
//
// Co-op compares the on-disk CRCs of Data3.pak (U-56), so every player must have the SAME Data3. The fixed Data3
// (tools/pak_fixed_data3.py build-pak) carries every per-machine variant of render_loop / oculus / sprite50, chosen by script
// if on values it imports from DW\dlvr_pp.mth (INV-521: imports resolve by bare name from the dw/ loose source, written here
// before its mount scan). WHICH pak is installed selects the path - there is no marker:
//   LEGACY (render_loop STORED at kPadTo)  -> today's in-place patching, byte-for-byte unchanged; no .mth is written
//   FIXED  (all three entries match fixed_data3_layout.h's CRC + size) -> the pak is NEVER opened for update; the routing
//          below runs exactly as before but its decisions are recorded into g_ppVal and written to DW\dlvr_pp.mth on exit
//   OTHER  (neither) -> said loudly; the .mth is still written, the pak is not touched
// The routing's every decision point records its value (PpSet). When this launch writes NO render data (DLSS with no / an
// unreadable dlss_quality.txt - today the pak keeps the last launch's render_loop), the render-group values are carried over
// from the previous .mth and only keep-RIFT / probe / HUD (today rewritten every launch) are new. Proven equivalent offline
// against this file's own generators (tools/pp_equiv_test.py, design doc §6b).
// [DEP: fixed_data3] required-for=co-op between players with different headsets / AA / AFW (U-56) - the fixed Data3 + DW\dlvr_pp.mth route (selected by WHICH Data3.pak is installed, not a marker; a LEGACY pak = today's behaviour exactly)  tree=INERT
int  g_ppLayout = 0;                 // 0 LEGACY, 1 FIXED (this build's layout), 2 OTHER
// [DATA3] the whole Data3.pak against kFixedData3Md5 (fixed_data3_md5.h): 0 not checked yet, kData3Ours / NotOurs / Unreadable.
// Read by the layer through DlvrData3State(): AFW is held off unless it is kData3Ours (Jay 2026-10-18, "Md5 + refuse AFW if
// wrong"). Written once, at the bootstrap's early init, before the layer loads.
// [LOADBEARING: DATA3-AFW-GATE] the ONLY thing that keeps AFW off on a Data3 that is not this build's - a stock pak clears the
//      RIFT targets and the held eye goes black (INV-375; a tester's, docs/BETA-FINDINGS-2026-10-18.md §2).
volatile LONG g_data3State = 0;
bool g_ppWriteInLegacy = false;      // set ONLY by src/src/tests/pakgen_equiv_harness.cpp - the game never writes the .mth on a legacy pak
int  g_ppVal[kPpNameCount] = {};     // kPpNames order
bool g_ppRenderSet = false;          // a generator decided the render values this launch

// >>> SAME-LAUNCH EYE SIZE - default ON, dlvr\early_eye_size_off.txt = off. 2026-10-17 (Jay: "Look into 3 first" -> "Go ahead"). <<<
//
// Quality / Balanced need an explicit scene size, and at attach the only size available is LAST launch's eye_res.txt, so a
// changed Virtual Desktop resolution (or render_scale, or headset) costs a second launch. But the engine does not READ
// DW\dlvr_pp.mth at attach - it reads it when render_loop.ppfx's import is parsed during renderer init (INV-522 / INV-523),
// through the engine's import of fs::file::open (INV-518). So the bootstrap IAT-hooks the engine's filesystem opens, and on
// the FIRST open of dlvr_pp.mth asks the layer (DlvrQueryEyeSizeEarly - a short-lived XrInstance, the layer's own sizing)
// for THIS launch's per-eye size, recomputes the size values and rewrites the file IN PLACE at its exact attach length (a
// reserve comment line absorbs the difference), before the open proceeds. Any failure keeps the attach file and says why.
// DEFAULT ON (2026-10-17, Jay-verified on DLSS Quality both ways and FSR 3 Balanced; TIERS A). dlvr\early_eye_size_off.txt (or a
// divisor mode, a legacy pak, a line-2 override): no reserve line, no hook - the .mth is byte-identical to before this feature.
// [DEP: early_eye_size] required-for=Quality / Balanced (DLSS or FSR) sized from THIS launch's runtime, not last launch's eye_res.txt - no second launch after a resolution change (DEFAULT ON; marker dlvr\early_eye_size_off.txt = off: no hook, byte-identical dlvr_pp.mth)  tree=LOADBEARING
bool   g_earlyWanted = false;        // marker present + FIXED/OTHER layout + an explicit-size mode + no override
int    g_earlyMode = -1;             // that mode (kModes index; FSR 1/2 share Quality / Balanced's factors)
int    g_earlyBuiltW = 0, g_earlyBuiltH = 0;   // the per-eye size the attach write used (0 = the divisor-2 fallback)
size_t g_earlyMthLen = 0;            // byte length of the attach write (0 = it was not written; the hook is not installed)
const size_t kEarlyReserveBytes = 192;
const char* const kEarlyReserveHead = "// dlvr early_eye_size reserve";
// v2 (INV-592: the open comes ~0.9 s AFTER ovr_Create, and VDXR refuses a second instance then): the per-eye size the layer
// already handed DlvrRegenPakForOutput this launch is the FIRST source at the open; the short-lived instance only if none came.
volatile LONG g_layerW = 0, g_layerH = 0;    // from DlvrRegenPakForOutput (ovr_GetFovTextureSize's first call); 0 = not yet
volatile LONG g_earlyDoneFlag = 0;           // mirrors the open-side done flag for DlvrRegenPakForOutput's wording

void PpSet(const char* name, int v)
{
    for (int i = 0; i < kPpNameCount; ++i) {
        if (strcmp(kPpNames[i], name) == 0) { g_ppVal[i] = v; return; }
    }
    PLOG("[PAKGEN] *** dlvr_pp.mth: no value named %s in fixed_data3_layout.h - this bootstrap and its layout header disagree ***",
         name);
}

int PpGet(const char* name)
{
    for (int i = 0; i < kPpNameCount; ++i) {
        if (strcmp(kPpNames[i], name) == 0) { return g_ppVal[i]; }
    }
    return -1;
}

// The render-group names: everything render_loop decides (all but the version and the oculus / sprite50 switches).
bool PpIsRenderName(const char* n)
{
    return strcmp(n, "DLVR_PP_VERSION") != 0 && strcmp(n, "DLVR_KEEP_RIFT") != 0 && strcmp(n, "DLVR_RIFT_PROBE") != 0 &&
           strcmp(n, "DLVR_HUD_LAYER") != 0;
}

// >>> NATIVE AA - dlvr\aa_native.txt. 2026-09-26. <<<
//
// VR has never had AA. The whole SMAA chain sits inside If("f_pp_smaa_tx2_on"),
// and frame_script.scr (our own Data0.pak) sets that to f_smaa_tx2_on * ... every
// frame - which OUR white-lines fix pins to 0.0. So not even the SPATIAL passes
// run, flat or VR; the Oculus gate inside it never mattered. There is no SMAA 1x
// branch in the shipped file at all.
//
// This fills the @AA_NATIVE@ token (inside `if(b_aa_on)`, right after the T2x
// block) with the engine's own three SMAA passes plus a copy back to SRGB_B,
// gated "tx2 NOT on": no resolve, no PRV_A history, and NO JITTER - the T2x
// jitter is exactly what caused the white lines, and dlssflat refuses the
// Halton patch in this mode. The output lands in 1_RGBA8_SRGB_B, which
// postprocess reads as s_clr, so it reaches OUT_SRGB and the eye copy.
//
// NOT INTERFERING WITH DLSS, BY CONSTRUCTION: marker absent -> the token is
// replaced by "" and every generated ppfx is byte-identical to before. Marker
// present -> dlss_quality.txt is ignored, the pak is written at divisor 1 (native
// resolution, nothing to upscale), and dlssflat.cpp forces its visible/VR/upscale/
// Halton paths OFF, so DLSS never evaluates on top of it.
// [DEP: aa_native] required-for=NATIVE AA - the native-resolution pak WITH the engine's SMAA 1x passes (marker dlvr\aa_native.txt; absent = token empty, pak byte-identical to before)  tree=INERT
bool g_nativeAaPak = false;
// TAA (dlvr\aa_taa.txt, dlssflat.cpp g_taaMode): our own temporal AA runs at
// NATIVE resolution, so the pak is the divisor-1 one with the SMAA token EMPTY -
// it needs no engine AA and must not be a DLSS upscale pak (dlssflat forces
// upscaling off in this mode, and a half-res scene would then just be stretched).
// [DEP: aa_taa] required-for=TAA - the native-resolution pak our own TAA resolves at (marker dlvr\aa_taa.txt OR aa_method.txt "taa" - the dropdown, 2026-11-12; aa_native.txt wins if both; absent = no change)  tree=INERT
bool g_taaPak = false;
// THE METHOD (aamethod.h, dlvr\aa_method.txt): the Anti-Aliasing dropdown's one
// switch, read after the two hand-only markers above (which still win). NO AA
// writes the native pak; FSR takes its mode from dlss_quality.txt like DLSS does,
// with Ultra Performance refused (Jay: "We don't want an option for Ultra
// Performance with FSR"). docs/PROPOSAL-FSR-INTEROP-2026-09-26.md §8.
// [DEP: aa_method] required-for=every pak choice - NO AA (native pak) / DLSS (dlss_quality.txt as before) / FSR (dlss_quality.txt 0-3, 4 refused -> 3) (marker dlvr\aa_method.txt; ABSENT = NO AA)  tree=LOADBEARING
int  g_aaMethodPak = 0;      // AaMethod, as PAKGEN read it
bool g_fsrPak = false;       // an FSR method is selected
bool g_noAaPak = false;      // NO AA is selected (or defaulted to)
const char* const kFsrModeNames[4] = { "FSR NATIVE (native resolution, no DLSS)", "FSR QUALITY",
                                       "FSR BALANCED", "FSR PERFORMANCE" };
// EDGE-MASK DEBUG - aa_native.txt whose FIRST LINE is "edges". The final copy
// reads the edge-detection output (1_RGBA8_A) instead of the SMAA result, so the
// whole view becomes a black image with coloured edge outlines IF the block runs.
// Flat showed reduced jaggies, VR showed none: this separates "the passes do not
// run in VR" (normal image) from "they run and spatial AA cannot fix VR aliasing"
// (edges visible). Deliberately unmistakable, like the Halton amplitude-16 test.
// [DEP: aa_native_edges] required-for=the VR edge-mask positive control - does the SMAA block execute in VR at all (first line of dlvr\aa_native.txt = "edges"; any other first line = normal SMAA 1x)  tree=PROBE-ONLY
bool g_nativeAaEdges = false;
const char* const kNativeAaFinalCopy = "\t\t\t\t\tSetBuffer(16, \"1_RGBA8_SRGB_A\"); //T1X\r\n";
const char* const kNativeAaEdgesCopy = "\t\t\t\t\tSetBuffer(16, \"1_RGBA8_A\"); //DLVR EDGE-MASK DEBUG\r\n";
std::string g_nativeAaBlockText;   // what the token is filled with this launch
const char* const kNativeAaBlock =
    "\t\t\t// DLVR NATIVE AA (dlvr/aa_native.txt): spatial SMAA 1x while T2x is off. No resolve, no history, no jitter.\r\n"
    "\t\t\tIf(\"f_pp_smaa_tx2_on\", \"\", \"\", \"\", \"not\")\r\n"
    "\t\t\t{\r\n"
    "\t\t\t\tSetClearColor(0.0, 0.0, 0.0, 0.0);\r\n"
    "\t\t\t\tSetRenderTargetsView(\"_Z1_RGBA8_A\", \"stencil color\");\r\n"
    "\t\t\t\t\tSetBuffer(16, \"1_RGBA8_SRGB_B\");\r\n"
    "\t\t\t\t\t\tMesh(\"tri_screen.msh\", \"smaa_edge_detection.mat\", \"std\");\r\n"
    "\r\n"
    "\t\t\t\tSetClearColor(0.0, 0.0, 0.0, 0.0);\r\n"
    "\t\t\t\tSetRenderTargetsView(\"_Z1_RGBA8_DIF\", \"color\");\r\n"
    "\t\t\t\t\tSetBuffer(16, \"1_RGBA8_A\");\r\n"
    "\t\t\t\t\t\tMesh(\"tri_screen.msh\", \"smaa_blend_weight.mat\", \"std\");\r\n"
    "\r\n"
    "\t\t\t\tSetRenderTargetsView(\"_1_RGBA8_SRGB_A\", \"\");\r\n"
    "\t\t\t\t\tSetBuffer(16, \"1_RG16F_MV\");\r\n"
    "\t\t\t\t\tSetBuffer(17, \"1_RGBA8_SRGB_B\");\r\n"
    "\t\t\t\t\tSetBuffer(18, \"1_RGBA8_DIF\"); //WEIGHTS\r\n"
    "\t\t\t\t\t\tMesh(\"tri_screen.msh\", \"smaa_blend.mat\", \"std\");\r\n"
    "\r\n"
    "\t\t\t\tSetRenderTargetsView(\"_1_RGBA8_SRGB_B\", \"\");\r\n"
    "\t\t\t\t\tSetBuffer(16, \"1_RGBA8_SRGB_A\"); //T1X\r\n"
    "\t\t\t\t\t\tMesh(\"tri_screen.msh\", \"copy_srgb.mat\", \"std\");\r\n"
    "\t\t\t}\r\n";

const char* const kEntry = "data/postprocess/include/render_loop.ppfx";
const size_t      kPadTo = 80000;   // must match how the pak was built

// NVIDIA's linear scaling factors, from the programming guide. These are the
// SAME five rows dlssflat matches against - the generator and the detector have
// to agree, or the detector reports a mode the data did not ask for.
struct Mode { const char* name; double linear; };
const Mode kModes[] = {
    { "DLAA",              1.00 },
    { "Quality",           1.50 },
    { "Balanced",          1.72 },
    { "Performance",       2.00 },
    { "Ultra Performance", 3.00 },
};
const int kModeCount = 5;

unsigned long Crc32(const unsigned char* p, size_t n)
{
    static unsigned long tab[256];
    static bool built = false;
    if (!built) {
        for (unsigned long i = 0; i < 256; ++i) {
            unsigned long c = i;
            for (int k = 0; k < 8; ++k) { c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1); }
            tab[i] = c;
        }
        built = true;
    }
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; ++i) { c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8); }
    return c ^ 0xFFFFFFFFUL;
}

std::string Template()
{
    std::string s;
    for (int i = 0; i < kRenderLoopTmplParts; ++i) { s += kRenderLoopTmpl[i]; }
    return s;
}

// EVEN sizes only. An odd render dimension is a known DLSS hazard, and the
// hand-built paks that are proven live were rounded this way - so the generator
// must round identically or it is not reproducing the tested case.
int Even(double v) { int n = static_cast<int>(v / 2.0 + 0.5) * 2; return n < 2 ? 2 : n; }

// int(x/lvl + 0.5) in integer arithmetic, matching mkexp.pl's pyramid rounding.
int DivRound(int x, int lvl) { int n = (x + lvl / 2) / lvl; return n < 1 ? 1 : n; }

void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
        s.replace(p, from.size(), to);
    }
}

// >>> SUN EFFECTS OFF IN VR, PER PASS - dlvr\sunfx_vr_off.txt. 2026-09-28. INV-276. <<<
//
// Jay: an orange full-screen hue and flickering flares in VR, an epilepsy concern. The sun's screen effects are FOUR
// separate passes inside `if(b_glow_on)` of render_loop.ppfx, none behind i_pp_OculusEnable, and the old f_sun_flare_*
// varlist keys are inert (weather.scr writes the f_pp_ gates directly - INV-276 (1)):
//   a = star rays     glow_star_ray_h / glow_star_ray_v x3     (ungated)
//   b = GOD RAYS      the If("f_pp_sun_glow_on") block: sun_rays -> sun_rays_blur -> sun_rays_blur_blend
//   c = sun sprites   the If("f_pp_sun_flare_on") block: sun_star + sun_flares
//   d = LENS FLARES   glow_lens_flares_source .. glow_lens_flares_add (ungated)
// The file's FIRST LINE lists the letters to switch OFF IN VR (e.g. "abcd", "b"). Each chosen pass is wrapped in
// If("i_pp_OculusEnable", "", "", "", "not") - the engine's own VR gate, already used 3x in this file (T2x, eye copies).
// Flat renders them unchanged. Every pass leaves the glow chain's working buffer (_4_RG11B10F_C) exactly as it found
// it when skipped (a: C->B->C->B->C round trip; b, d: read C / blend into C; c: draws into C), so nothing downstream
// reads a stale target. Each anchor must match EXACTLY ONCE or NOTHING is wrapped and the log says which failed.
// [DEP: sunfx_vr_off] required-for=the per-pass sun-effect tests (orange hue / flicker, Jay 2026-09-28) - which of star rays / god rays / sun sprites / lens flares to skip in VR (marker dlvr\sunfx_vr_off.txt, first line = letters a-d; ABSENT = pak byte-identical to before)  tree=INERT
std::string g_sunFxOff;          // the letters, as read this launch ("" = off)
std::string g_sunFxResult;       // what was actually wrapped, for the READY line

const char* const kVrOnlyOffOpen = "\t\t\t// DLVR sunfx_vr_off.txt: this pass is skipped in VR\r\n"
                                   "\t\t\tIf(\"i_pp_OculusEnable\", \"\", \"\", \"\", \"not\")\r\n\t\t\t{\r\n";
const char* const kVrOnlyOffClose = "\t\t\t}\r\n";

size_t LineStart(const std::string& t, size_t p) { const size_t n = t.rfind('\n', p); return n == std::string::npos ? 0 : n + 1; }
size_t LineEnd(const std::string& t, size_t p) { const size_t n = t.find('\n', p); return n == std::string::npos ? t.size() : n + 1; }
size_t CountOf(const std::string& t, const char* s)
{
    size_t c = 0;
    for (size_t p = t.find(s); p != std::string::npos; p = t.find(s, p + 1)) { ++c; }
    return c;
}

// [begin, end) of the brace block that follows the line holding `p` (the If(...) line), closing line included.
bool BlockAfter(const std::string& t, size_t p, size_t* begin, size_t* end)
{
    *begin = LineStart(t, p);
    size_t open = t.find('{', p);
    if (open == std::string::npos) { return false; }
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        if (t[i] == '{') { ++depth; }
        else if (t[i] == '}' && --depth == 0) { *end = LineEnd(t, i); return true; }
    }
    return false;
}

// Wraps the chosen passes. Ranges are located first, all-or-nothing, then applied back to front so earlier offsets hold.
bool ApplySunFxVrGates(std::string& t, std::string* why)
{
    struct R { char id; size_t b, e; };
    std::vector<R> rs;
    for (char c : g_sunFxOff) {
        size_t b = 0, e = 0;
        if (c == 'a') {
            if (CountOf(t, "\"glow_star_ray_h.mat\"") != 1 || CountOf(t, "\"glow_star_ray_v.mat\"") != 3) {
                *why = "a: glow_star_ray_h x1 / glow_star_ray_v x3 not found exactly"; return false;
            }
            const size_t h = t.find("\"glow_star_ray_h.mat\"");
            b = LineStart(t, t.rfind("SetRenderTargetsView", h));
            e = LineEnd(t, t.rfind("\"glow_star_ray_v.mat\""));
        } else if (c == 'b' || c == 'c') {
            const char* a = c == 'b' ? "If(\"f_pp_sun_glow_on\"" : "If(\"f_pp_sun_flare_on\"";
            if (CountOf(t, a) != 1) { *why = std::string(1, c) + ": its If(...) gate not found exactly once"; return false; }
            if (!BlockAfter(t, t.find(a), &b, &e)) { *why = std::string(1, c) + ": unbalanced braces"; return false; }
        } else if (c == 'd') {
            if (CountOf(t, "\"glow_lens_flares_source.mat\"") != 1 || CountOf(t, "\"glow_lens_flares_add.mat\"") != 1) {
                *why = "d: glow_lens_flares_source / _add not found exactly once"; return false;
            }
            b = LineStart(t, t.rfind("SetRenderTargetsView", t.find("\"glow_lens_flares_source.mat\"")));
            e = LineEnd(t, t.find("\"glow_lens_flares_add.mat\""));
        } else {
            continue;   // anything else in the file (spaces, CR, a comment) is ignored, and reported via the READY line
        }
        if (b >= e) { *why = std::string(1, c) + ": empty range"; return false; }
        for (const R& o : rs) {
            if (b < o.e && o.b < e) { *why = std::string(1, c) + ": overlaps another pass"; return false; }
        }
        rs.push_back({ c, b, e });
    }
    std::sort(rs.begin(), rs.end(), [](const R& x, const R& y) { return x.b > y.b; });
    g_sunFxResult.clear();
    for (const R& r : rs) {
        t.insert(r.e, kVrOnlyOffClose);
        t.insert(r.b, kVrOnlyOffOpen);
        char buf[48];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%c(%zu B) ", r.id, r.e - r.b);
        g_sunFxResult = buf + g_sunFxResult;
    }
    return true;
}

// Called at the one point both generators share (right after @AA_NATIVE@). Absent marker = no change at all.
void SunFxStep(std::string& t)
{
    if (g_sunFxOff.empty()) { return; }
    std::string why;
    const size_t before = t.size();
    if (!ApplySunFxVrGates(t, &why)) {
        // [PROBE: PAKGEN] gate=sunfx_vr_off.txt  site=FLAT  log-only=yes
        PLOG("[PAKGEN] *** SUN EFFECTS VR GATES REFUSED *** (%s) - NOTHING wrapped; every sun pass runs in VR this launch.",
             why.c_str());
        return;
    }
    // [PROBE: PAKGEN] gate=sunfx_vr_off.txt  site=FLAT  log-only=yes
    PLOG("[PAKGEN] SUN EFFECTS skipped IN VR this launch: %s(+%zu bytes of If/brace lines). a=star rays b=god rays "
         "c=sun sprites d=lens flares. Flat renders them unchanged.", g_sunFxResult.c_str(), t.size() - before);
}

// >>> AMBIENT DUST / SOOT PARTICLES OFF IN VR - dlvr\dustfx_vr_off.txt. 2026-10-03. <<<
//
// Jay: floating particles in the air ghost under DLSS/FSR (they carry no motion vectors) - wanted gone. They are the
// camera-volume particle mesh render_loop.ppfx draws right after the opaque passes, inside if(b_sky_on && !b_editor_on):
//   If("f_pp_dust_amount") { If("f_dust_soot_on") soot_particles.mat / If(... "not") dust_particles.mat }
// The f_pp_ gate is rewritten every update by weather.scr:337-343 (Data0) from the map's f_dust_amount (Data2
// slums.scr = 1.0) x wind x wet/fog x interior, so a varlist override cannot hold it (the INV-276 shape). The whole If
// block is wrapped in the same i_pp_OculusEnable "not" gate as the sun passes: VR skips it, flat renders it unchanged.
// It draws into _Z1_GBUFFER before gbuffer_fill and nothing reads a target it alone writes. Map-placed FX emitters are a
// different path and are NOT touched. Presence of the file = ON; its content is ignored. Anchor must match exactly once.
// [DEP: dustfx_vr_off] required-for=Jay's floating-particle ghosting fix (2026-10-03) - skip the ambient dust/soot particle pass in VR (marker dlvr\dustfx_vr_off.txt, presence = on; ABSENT = pak byte-identical to before)  tree=INERT
bool g_dustFxOff = false;        // marker present this launch

const char* const kDustVrOffOpen = "\t\t// DLVR dustfx_vr_off.txt: ambient dust/soot particles are skipped in VR\r\n"
                                   "\t\tIf(\"i_pp_OculusEnable\", \"\", \"\", \"\", \"not\")\r\n\t\t{\r\n";
const char* const kDustVrOffClose = "\t\t}\r\n";

// Called right after SunFxStep in both generators. Absent marker = no change at all.
void DustFxStep(std::string& t)
{
    if (!g_dustFxOff) { return; }
    const char* const a = "If(\"f_pp_dust_amount\"";
    const size_t n = CountOf(t, a);
    size_t b = 0, e = 0;
    if (n != 1 || !BlockAfter(t, t.find(a), &b, &e) || b >= e) {
        // [PROBE: PAKGEN] gate=dustfx_vr_off.txt  site=FLAT  log-only=yes
        PLOG("[PAKGEN] *** DUST PARTICLES VR GATE REFUSED *** (If(\"f_pp_dust_amount\") found %zu time(s), need exactly 1, "
             "or its braces did not balance) - NOTHING wrapped; dust/soot particles draw in VR this launch.", n);
        return;
    }
    t.insert(e, kDustVrOffClose);
    t.insert(b, kDustVrOffOpen);
    // [PROBE: PAKGEN] gate=dustfx_vr_off.txt  site=FLAT  log-only=yes
    PLOG("[PAKGEN] DUST PARTICLES skipped IN VR this launch: the f_pp_dust_amount block (%zu B, soot + dust meshes) "
         "wrapped in i_pp_OculusEnable-not. Flat renders it unchanged.", e - b);
}

// >>> RENDER-LOOP IMPORT TEST - dlvr\rl_import_test.txt. 2026-10-10. INV-521 (c), U-56 fix route (D-207). <<<
//
// Can an IDENTICAL Data3 render_loop.ppfx take its per-machine buffer sizes from a loose file the engine imports? With the
// marker present, the size tokens are written as the NAMES DLVR_W1 .. DLVR_H16 (no numbers), one import of
// dlvr_rl_sizes.mth goes at the top, and the numbers go into DW\dlvr_rl_sizes.mth as "export int DLVR_W1 = 1792;" - the
// EngineDefs.mth idiom (common.ppfx:1, enginedefs.mth:157). DW\ is a CWin32_dir source with no whitelist, served by bare name
// (INV-521 (a), measured) and written here before its mount scan (INV-521 (b)). Marker empty = the sizes this launch computed;
// marker "W H" (both divisible by 16) = those instead, written ONLY into the .mth - so Data3 must come out byte-identical
// either way. Absent marker = nothing here runs and the pak is exactly as before.
// [DEP: rl_import_test] required-for=the INV-521 (c) import test only (marker dlvr\rl_import_test.txt; ABSENT = pak byte-identical to before)  tree=INERT
const char* const kRlImportName = "dlvr_rl_sizes.mth";

// True when the marker is present; *tw / *th = the sizes to export (the override, or w1 / h1).
bool RlImportTestWanted(int w1, int h1, int* tw, int* th)
{
    char marker[MAX_PATH];
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\rl_import_test.txt", g_dlvrDir);
    FILE* f = nullptr;
    if (fopen_s(&f, marker, "rb") != 0 || !f) { return false; }
    char buf[64] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    *tw = w1; *th = h1;
    int xw = 0, xh = 0;
    if (sscanf_s(buf, "%d %d", &xw, &xh) == 2) {
        if (xw >= 320 && xh >= 240 && xw <= 16384 && xh <= 16384 && xw % 16 == 0 && xh % 16 == 0) {
            *tw = xw; *th = xh;
        } else {
            // [PROBE: PAKGEN] gate=rl_import_test.txt  site=FLAT  log-only=yes
            PLOG("[PAKGEN] import test: override \"%d %d\" in %s REFUSED (need 320..16384 x 240..16384, both divisible by "
                 "16) - exporting this launch's own %dx%d instead.", xw, xh, marker, w1, h1);
        }
    }
    return true;
}

// Writes DW\dlvr_rl_sizes.mth for scene tw x th and its 2/4/8/16 pyramid (DivRound, as the numeric path). Says why on failure.
bool WriteRlImportFile(int tw, int th)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\DW\\%s", g_gameDir, kRlImportName);
    std::string body = "// dlvr - per-machine render-loop sizes, written at startup. Not in any pak.\r\n";
    char line[96];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "export int DLVR_W1 = %d;\r\nexport int DLVR_H1 = %d;\r\n", tw, th);
    body += line;
    for (int lvl = 2; lvl <= 16; lvl *= 2) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "export int DLVR_W%d = %d;\r\nexport int DLVR_H%d = %d;\r\n",
                    lvl, DivRound(tw, lvl), lvl, DivRound(th, lvl));
        body += line;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        // [PROBE: PAKGEN] gate=rl_import_test.txt  site=FLAT  log-only=yes
        PLOG("[PAKGEN] import test: *** could not write %s *** - falling back to NUMBERS in the pak this launch.", path);
        return false;
    }
    const size_t wrote = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (wrote != body.size()) {
        PLOG("[PAKGEN] import test: *** short write to %s (%zu of %zu bytes) *** - falling back to NUMBERS in the pak.",
             path, wrote, body.size());
        return false;
    }
    // [PROBE: PAKGEN] gate=rl_import_test.txt  site=FLAT  log-only=yes
    PLOG("[PAKGEN] import test: wrote %s (%zu bytes): scene %dx%d, /2 %dx%d, /4 %dx%d, /8 %dx%d, /16 %dx%d.", path, wrote,
         tw, th, DivRound(tw, 2), DivRound(th, 2), DivRound(tw, 4), DivRound(th, 4), DivRound(tw, 8), DivRound(th, 8),
         DivRound(tw, 16), DivRound(th, 16));
    return true;
}

// >>> REAL ENGINE JITTER - dlvr\jitter_engine.txt. 2026-10-06. INV-434 / INV-436. <<<
//
// The engine applies its projection jitter only when the render loop issues SMAAScreenJitter(enabled), and our
// render_loop.ppfx issues it only inside If("f_smaa_jitter_on"), which frame_script.scr:900 rewrites every frame from
// f_smaa_tx2_on - pinned to 0.0 by our white-lines fix. So the engine jitters NOTHING while DLSS / FSR are told the Halton
// offset (INV-434). This swaps that ONE gate for If("f_pp_smaa_tx2_on", ..., "not") - true exactly while the white-lines fix
// holds (the same idiom and var the native-AA block uses, proven in VR by INV-252's edge-mask control) - so the jitter runs
// WITHOUT re-enabling the SMAA chain. Only under DLSS / FSR (not NO AA, native AA or TAA) and only with jitter_halton.txt
// present (the 8-phase table). Presence of the file = on; content ignored. Anchor must match exactly once.
// [DEP: jitter_engine] required-for=the real-jitter test (INV-436) - let the engine apply its Halton jitter under DLSS / FSR (FSR since 2026-10-23, not verified) (marker dlvr\jitter_engine.txt + jitter_halton.txt; ABSENT = pak byte-identical to before)  tree=INERT
bool g_engineJitterMarker = false;   // marker present this launch

const char* const kJitterGateStock = "If(\"f_smaa_jitter_on\", \"\", \"\", \"\", \"\")";
const char* const kJitterGateOurs  = "If(\"f_pp_smaa_tx2_on\", \"\", \"\", \"\", \"not\")";

// Called right after DustFxStep in both generators. Absent marker / wrong method = no change at all.
void EngineJitterStep(std::string& t)
{
    if (!g_engineJitterMarker) { return; }
    char halton[MAX_PATH];
    _snprintf_s(halton, sizeof(halton), _TRUNCATE, "%s\\jitter_halton.txt", g_dlvrDir);
    const bool haveHalton = GetFileAttributesA(halton) != INVALID_FILE_ATTRIBUTES;
    // DLSS AND FSR. DLSS verified on Quality (INV-441). FSR was refused until tested (Jay 2026-10-06: "ship for DLSS only. We'll
    // have to test the other modes"); lifted 2026-10-23 for that test (Jay: "Sounds good. Lets do it") - FSR's eval already runs
    // the same strip / tell as DLSS (dlssflat.cpp realjit::Strip / Tell) and is told (x, -y) by INV-255 (fsr_jitter_flip.txt = the
    // raw-sign A/B). FSR with real jitter is NOT verified until Jay's eye says so.
    // TAA since 2026-11-12 (Jay: "Yes, go with TAA"): a temporal resolve needs the sub-pixel jitter; before this TAA was refused
    // here, so D-128's two flat TAA runs (2026-09-26, before INV-434) resolved an UN-jittered image. RunTaa gets the same stripped
    // camera MVs as DLSS (EvaluateVisibleFrame, JITTRUE-MVSTRIP). TAA with real jitter is NOT verified until Jay's eye says so.
    if (g_noAaPak || g_nativeAaPak || !haveHalton) {
        // [PROBE: PAKGEN] gate=jitter_engine.txt  site=FLAT  log-only=yes
        PLOG("[PAKGEN] engine jitter REFUSED - %s. The stock gate stays; the engine applies no jitter this launch.",
             !haveHalton ? "jitter_halton.txt is absent (the 8-phase table would not be patched)"
                         : "the AA method is not DLSS, FSR or TAA (NO AA or native AA)");
        return;
    }
    PpSet("DLVR_JITTER_ENGINE", 1);   // fixed Data3: the same decision, as a value (the merged text carries both gates)
    const size_t n = CountOf(t, kJitterGateStock);
    if (n != 1) {
        // [PROBE: PAKGEN] gate=jitter_engine.txt  site=FLAT  log-only=yes
        PLOG("[PAKGEN] *** ENGINE JITTER GATE REFUSED *** (the stock jitter gate found %zu time(s), need exactly 1) - "
             "NOTHING changed; the engine applies no jitter this launch.", n);
        return;
    }
    const size_t at = t.find(kJitterGateStock);
    t.replace(at, strlen(kJitterGateStock), kJitterGateOurs);
    // [PROBE: PAKGEN] gate=jitter_engine.txt  site=FLAT  log-only=yes
    PLOG("[PAKGEN] engine jitter ON this launch (%s): render_loop's jitter gate If(\"f_smaa_jitter_on\") -> "
         "If(\"f_pp_smaa_tx2_on\", ..., \"not\") at byte %zu. The SMAA chain stays off (f_pp_smaa_tx2_on = 0).",
         g_taaPak ? "TAA - NOT VERIFIED yet" : g_fsrPak ? "FSR - NOT VERIFIED yet" : "DLSS", at);
}

// >>> AFW: KEEP THE RIFT TARGETS - dlvr\afw_keep_rift.txt. 2026-10-03. INV-374. <<<
//
// Our Data0 oculus.ppfx composites RIFT_L/RIFT_R into the backbuffer and then CLEARS both to black every frame
// (INV-19). Under afw_alt_eye the skipped eye's RIFT is never redrawn, so it is composited black - the candidate for
// Jay's "both eyes darker" + "the odd black eye". Our Data3.pak carries its own copy of the stock file
// (tools/pak_oculus.py: data/postprocess/oculus.ppfx, STORED, padded to 4096 B with '\n'). This step toggles TWO
// BYTES per line in place, so the entry size never changes and an absent marker restores the stock bytes exactly:
//   marker present          -> both  SetRenderTargetsView("_RIFT_x", "color");  lines start "//" (no clear)
//   marker first line probe -> ALSO the RIFT_R  Mesh("quad_screen.msh", "oculus.mat", "std");  starts "//": the right
//                              eye is NOT composited - an unmistakable positive control that Data3's oculus.ppfx is
//                              the one the engine runs (a silently ignored override would otherwise read as "no change")
//   marker absent           -> "Se" / "Me" restored: byte-identical to the stock file.
// Startup only (the engine holds Data3 for the session, pakgen.cpp:353-389). Every refusal says why.
// [DEP: afw_keep_rift] required-for=AFW's darkening / black-eye fix (INV-375, verified by Jay) - stop oculus.ppfx clearing the RIFT targets so a skipped eye keeps its previous image (marker dlvr\afw_keep_rift.txt; first line "probe" = also skip compositing the right eye, a positive control; ABSENT = stock bytes). SHIPPED as a package marker since 2026-10-08 (Jay: "Put the marker for keep-rift in as we have it currently"); NOT gated (the gate attempts caused issues - parked, with the first-loading-screen half)  tree=LOADBEARING
const char* const kOculusEntry = "data/postprocess/oculus.ppfx";
const size_t      kOculusPad   = 4096;

// Finds `tail` (the text after its 2-byte prefix) and checks the prefix is one of the two states. Returns offsets.
std::vector<size_t> TwoStateSites(const std::string& t, const char* tail, const char* on, const char* off)
{
    std::vector<size_t> at;
    for (size_t p = t.find(tail); p != std::string::npos; p = t.find(tail, p + 1)) {
        if (p >= 2 && (t.compare(p - 2, 2, on) == 0 || t.compare(p - 2, 2, off) == 0)) { at.push_back(p - 2); }
    }
    return at;
}

void OculusKeepRiftStep(bool keep, bool probe)
{
    PpSet("DLVR_KEEP_RIFT", keep ? 1 : 0);
    PpSet("DLVR_RIFT_PROBE", (keep && probe) ? 1 : 0);
    if (g_ppLayout != 0) {
        // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] AFW keep-RIFT: fixed Data3 - oculus.ppfx is NOT patched; DLVR_KEEP_RIFT=%d DLVR_RIFT_PROBE=%d go to "
             "DW\\%s.", keep ? 1 : 0, (keep && probe) ? 1 : 0, kPpMthName);
        return;
    }
    char pak[MAX_PATH];
    _snprintf_s(pak, sizeof(pak), _TRUNCATE, "%s/DW/Data3.pak", g_gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, pak, "r+b") != 0 || !f) {
        // [PROBE: PAKGEN] gate=afw_keep_rift.txt  site=FLAT  log-only=yes
        PLOG("[PAKGEN] *** AFW KEEP-RIFT REFUSED *** cannot open %s for update - oculus.ppfx left as it is.", pak);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    std::vector<unsigned char> buf(size > 0 ? static_cast<size_t>(size) : 0);
    fseek(f, 0, SEEK_SET);
    if (size < 22 || fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); return; }
    long eocd = -1;
    for (long i = size - 22; i >= 0 && i > size - 65557; --i) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { PLOG("[PAKGEN] AFW KEEP-RIFT: no ZIP end-of-central-directory in %s.", pak); fclose(f); return; }
    unsigned short entries = 0; memcpy(&entries, &buf[eocd + 10], 2);
    unsigned int cdOff = 0; memcpy(&cdOff, &buf[eocd + 16], 4);
    long cd = static_cast<long>(cdOff);
    for (int e = 0; e < entries; ++e) {
        if (cd + 46 > size || buf[cd] != 0x50 || buf[cd + 1] != 0x4b) { break; }
        unsigned short method = 0, namelen = 0, extlen = 0, cmtlen = 0;
        unsigned int csize = 0, lho = 0;
        memcpy(&method, &buf[cd + 10], 2); memcpy(&csize, &buf[cd + 20], 4);
        memcpy(&namelen, &buf[cd + 28], 2); memcpy(&extlen, &buf[cd + 30], 2); memcpy(&cmtlen, &buf[cd + 32], 2);
        memcpy(&lho, &buf[cd + 42], 4);
        const std::string name(reinterpret_cast<char*>(&buf[cd + 46]), namelen);
        if (name == kOculusEntry) {
            if (method != 0 || csize != kOculusPad) {
                PLOG("[PAKGEN] *** AFW KEEP-RIFT REFUSED *** %s is method %u at %u bytes, need STORED at %zu.", kOculusEntry,
                     method, csize, kOculusPad);
                fclose(f); return;
            }
            unsigned short lnamelen = 0, lextlen = 0;
            memcpy(&lnamelen, &buf[lho + 26], 2); memcpy(&lextlen, &buf[lho + 28], 2);
            const long data = static_cast<long>(lho) + 30 + lnamelen + lextlen;
            std::string t(reinterpret_cast<char*>(&buf[data]), kOculusPad);
            const std::vector<size_t> clrL = TwoStateSites(t, "tRenderTargetsView(\"_RIFT_L\", \"color\");", "Se", "//");
            const std::vector<size_t> clrR = TwoStateSites(t, "tRenderTargetsView(\"_RIFT_R\", \"color\");", "Se", "//");
            const std::vector<size_t> mesh = TwoStateSites(t, "sh(\"quad_screen.msh\", \"oculus.mat\", \"std\");", "Me", "//");
            if (clrL.size() != 1 || clrR.size() != 1 || mesh.size() != 2) {
                PLOG("[PAKGEN] *** AFW KEEP-RIFT REFUSED *** sites found: RIFT_L clear %zu, RIFT_R clear %zu, oculus.mat "
                     "draw %zu (need 1, 1, 2) - oculus.ppfx left as it is.", clrL.size(), clrR.size(), mesh.size());
                fclose(f); return;
            }
            const std::string before = t;
            t.replace(clrL[0], 2, keep ? "//" : "Se");
            t.replace(clrR[0], 2, keep ? "//" : "Se");
            t.replace(mesh[1], 2, (keep && probe) ? "//" : "Me");   // [1] = the RIFT_R draw (L is drawn first)
            if (t == before) {
                fclose(f);
                // [PROBE: PAKGEN] gate=afw_keep_rift.txt  site=FLAT  log-only=yes
                PLOG("[PAKGEN] AFW keep-RIFT: oculus.ppfx already %s - not rewritten.",
                     !keep ? "STOCK (both RIFT clears, both eyes drawn)" : probe ? "NO-CLEAR + PROBE (right eye not drawn)"
                                                                         : "NO-CLEAR (RIFT targets keep their image)");
                return;
            }
            const unsigned long crc = Crc32(reinterpret_cast<const unsigned char*>(t.data()), t.size());
            fseek(f, data, SEEK_SET);     fwrite(t.data(), 1, t.size(), f);
            fseek(f, lho + 14, SEEK_SET); fwrite(&crc, 4, 1, f);
            fseek(f, cd + 16, SEEK_SET);  fwrite(&crc, 4, 1, f);
            fclose(f);
            // [PROBE: PAKGEN] gate=afw_keep_rift.txt  site=FLAT  log-only=yes
            PLOG("[PAKGEN] AFW keep-RIFT: oculus.ppfx rewritten in place -> %s (CRC %08lX).",
                 !keep ? "STOCK (both RIFT clears, both eyes drawn)" : probe ? "NO-CLEAR + *** PROBE: the RIFT_R composite is "
                 "commented out - the RIGHT EYE should be BLACK/frozen in the headset ***" : "NO-CLEAR (RIFT targets keep their image)",
                 crc);
            return;
        }
        cd += 46 + namelen + extlen + cmtlen;
    }
    fclose(f);
    PLOG("[PAKGEN] AFW keep-RIFT: %s is not in Data3.pak (tools/pak_oculus.py has not been applied) - %s.", kOculusEntry,
         keep ? "*** the marker CANNOT take effect ***" : "nothing to do");
}

// >>> AFW HUD LAYER v4 - dlvr\hud_layer.txt. 2026-10-07. Jay: "Yes, build it". INV-482 / INV-483 + addendum. <<<
//
// The VR HUD is the Sprites layer 23 drawn by sprite50.ppfx straight into _RIFT_L / _RIFT_R (INV-482). v3 picked the HUD
// pass out by BIND ORDER and caught the loading screen (the Background layer 20 binds the same RIFT targets, INV-483
// addendum). v4 brackets the pass itself: our Data3.pak carries its own sprite50.ppfx (tools/pak_sprite50.py, STORED,
// padded to 2048 B) with FOUR marker binds that start commented - BEGIN `_LUM_16x16_B` then `_LUM_32x16` just before the
// RIFT bind, END `_LUM_32x16` then `_LUM_16x16_B` after the HUD draws, then the RIFT re-bind - all inside
// If(i_pp_OculusEnable), so flat never runs them. This step toggles each line's first two bytes "//" <-> "Se" in place:
//   marker present (any first line)  -> the four binds run; the bootstrap's hudlayer:: reads the bracket
//   marker absent                    -> commented again: the stock HUD pass + one redundant RIFT re-bind
// First line "count" = the binds run but the bootstrap only COUNTS the brackets (no redirect) - a log-only check.
// Startup only (the engine holds Data3 for the session). Every refusal says why.
// [DEP: hud_layer] required-for=the AFW HUD ghosting (INV-482 / INV-483) - the HUD pass (sprite50.ppfx, Sprites layer 23) bracketed by marker binds so the bootstrap redirects ONLY that pass into per-eye layers and lays them on after the AFW rebuild (Tier A, INV-484 / 485. GATED ON afw_enable since 2026-10-08: on when dlvr\ovr2xr.txt says afw_enable=1 or has no such key (default 1); marker dlvr\hud_layer.txt = developer override, first line "count" = brackets counted, nothing redirected; OFF = the four marker lines commented)  tree=LOADBEARING
int g_hudLayerPak = 0;   // 0 off / refused, 1 brackets live + redirect, 2 brackets live, COUNT only
const char* const kSpriteEntry = "data/postprocess/sprite50.ppfx";
const size_t      kSpritePad   = 2048;

void SpriteHudStep(int mode)
{
    g_hudLayerPak = 0;
    PpSet("DLVR_HUD_LAYER", mode ? 1 : 0);
    if (g_ppLayout != 0) {
        g_hudLayerPak = mode;   // the brackets run whenever the value is 1 - dlssflat's hudlayer:: reads this mode as before
        // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] HUD layer: fixed Data3 - sprite50.ppfx is NOT patched; DLVR_HUD_LAYER=%d goes to DW\\%s (mode %d).",
             mode ? 1 : 0, kPpMthName, mode);
        return;
    }
    char pak[MAX_PATH];
    _snprintf_s(pak, sizeof(pak), _TRUNCATE, "%s/DW/Data3.pak", g_gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, pak, "r+b") != 0 || !f) {
        // [PROBE: PAKGEN] gate=hud_layer.txt  site=FLAT  log-only=yes
        PLOG("[PAKGEN] *** HUD LAYER REFUSED *** cannot open %s for update - sprite50.ppfx left as it is.", pak);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    std::vector<unsigned char> buf(size > 0 ? static_cast<size_t>(size) : 0);
    fseek(f, 0, SEEK_SET);
    if (size < 22 || fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        fclose(f);
        PLOG("[PAKGEN] *** HUD LAYER REFUSED *** %s could not be read (%ld bytes).", pak, size);
        return;
    }
    long eocd = -1;
    for (long i = size - 22; i >= 0 && i > size - 65557; --i) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { PLOG("[PAKGEN] HUD LAYER: no ZIP end-of-central-directory in %s.", pak); fclose(f); return; }
    unsigned short entries = 0; memcpy(&entries, &buf[eocd + 10], 2);
    unsigned int cdOff = 0; memcpy(&cdOff, &buf[eocd + 16], 4);
    long cd = static_cast<long>(cdOff);
    for (int e = 0; e < entries; ++e) {
        if (cd + 46 > size || buf[cd] != 0x50 || buf[cd + 1] != 0x4b) { break; }
        unsigned short method = 0, namelen = 0, extlen = 0, cmtlen = 0;
        unsigned int csize = 0, lho = 0;
        memcpy(&method, &buf[cd + 10], 2); memcpy(&csize, &buf[cd + 20], 4);
        memcpy(&namelen, &buf[cd + 28], 2); memcpy(&extlen, &buf[cd + 30], 2); memcpy(&cmtlen, &buf[cd + 32], 2);
        memcpy(&lho, &buf[cd + 42], 4);
        const std::string name(reinterpret_cast<char*>(&buf[cd + 46]), namelen);
        if (name == kSpriteEntry) {
            if (method != 0 || csize != kSpritePad) {
                PLOG("[PAKGEN] *** HUD LAYER REFUSED *** %s is method %u at %u bytes, need STORED at %zu.", kSpriteEntry,
                     method, csize, kSpritePad);
                fclose(f); return;
            }
            unsigned short lnamelen = 0, lextlen = 0;
            memcpy(&lnamelen, &buf[lho + 26], 2); memcpy(&lextlen, &buf[lho + 28], 2);
            const long data = static_cast<long>(lho) + 30 + lnamelen + lextlen;
            std::string t(reinterpret_cast<char*>(&buf[data]), kSpritePad);
            const char* const tails[4] = {
                "tRenderTargetsView(\"_LUM_16x16_B\", \"\"); //DLVR HUD LAYER begin 1",
                "tRenderTargetsView(\"_LUM_32x16\", \"\"); //DLVR HUD LAYER begin 2",
                "tRenderTargetsView(\"_LUM_32x16\", \"\"); //DLVR HUD LAYER end 1",
                "tRenderTargetsView(\"_LUM_16x16_B\", \"\"); //DLVR HUD LAYER end 2" };
            size_t at[4] = {};
            for (int k = 0; k < 4; ++k) {
                const std::vector<size_t> s = TwoStateSites(t, tails[k], "Se", "//");
                if (s.size() != 1) {
                    PLOG("[PAKGEN] *** HUD LAYER REFUSED *** marker site %d found %zu time(s), need 1 - sprite50.ppfx left as "
                         "it is.", k + 1, s.size());
                    fclose(f); return;
                }
                at[k] = s[0];
            }
            const std::string before = t;
            for (int k = 0; k < 4; ++k) { t.replace(at[k], 2, mode ? "Se" : "//"); }
            g_hudLayerPak = mode;
            if (t == before) {
                fclose(f);
                // [PROBE: PAKGEN] gate=hud_layer.txt  site=FLAT  log-only=yes
                PLOG("[PAKGEN] HUD layer: sprite50.ppfx already %s - not rewritten.",
                     mode ? "BRACKETED (the four marker binds run)" : "STOCK (the marker binds commented)");
                return;
            }
            const unsigned long crc = Crc32(reinterpret_cast<const unsigned char*>(t.data()), t.size());
            fseek(f, data, SEEK_SET);     fwrite(t.data(), 1, t.size(), f);
            fseek(f, lho + 14, SEEK_SET); fwrite(&crc, 4, 1, f);
            fseek(f, cd + 16, SEEK_SET);  fwrite(&crc, 4, 1, f);
            fclose(f);
            // [PROBE: PAKGEN] gate=hud_layer.txt  site=FLAT  log-only=yes
            PLOG("[PAKGEN] HUD layer: sprite50.ppfx rewritten in place -> %s (CRC %08lX).",
                 mode ? "BRACKETED (the four marker binds run)" : "STOCK (the marker binds commented)", crc);
            return;
        }
        cd += 46 + namelen + extlen + cmtlen;
    }
    fclose(f);
    PLOG("[PAKGEN] HUD layer: %s is not in Data3.pak (tools/pak_sprite50.py has not been applied) - %s.", kSpriteEntry,
         mode ? "*** the marker CANNOT take effect ***" : "nothing to do");
}

// >>> THE OUTPUT RESOLUTION IS NOT THE DESKTOP IN VR. <<<
// In VR the scene is rendered per EYE at the headset's resolution, which the
// layer caches to dlvr\hmd_cache.txt. Using the desktop size there would
// generate a ratio for a surface the engine never renders; the detector would
// then measure something else and pick a different mode - a silent mismatch,
// not an error.

// Patch the entry in place. Every failure path SAYS WHY - a generator that
// quietly does nothing leaves the pak from the previous mode, which looks
// exactly like mode selection not working.
bool PatchPak(const char* pak, const std::string& content)
{
    if (g_ppLayout != 0) {
        // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] fixed Data3 - render_loop.ppfx is NOT patched (the %zu-byte text this launch would have written is "
             "expressed as values in DW\\%s).", content.size(), kPpMthName);
        return true;
    }
    if (content.size() > kPadTo) {
        PLOG("[PAKGEN] generated ppfx is %zu bytes, over the %zu-byte entry. The pak needs "
             "rebuilding with a larger pad; refusing to write a truncated file.",
             content.size(), kPadTo);
        return false;
    }
    // >>> THE HANDLE IS OPENED ONCE, AT STARTUP, AND NEVER CLOSED. <<<
    //
    // MEASURED 2026-09-13: the runtime rewrite was IN TIME BY 9.5 SECONDS -
    // ovr_GetFovTextureSize reported 2688x2880 at 16:52:50.374 and the engine
    // did not create its GBuffer until 16:52:59.859 - and it still failed, with
    // "cannot open Data3.pak for update". THE ENGINE HOLDS THE PAK OPEN FOR THE
    // WHOLE SESSION, which this project already knew ("pak edits need the game
    // CLOSED") and which I did not connect to a write from INSIDE the running
    // game.
    //
    // A handle opened BEFORE the engine gets there stays valid for the life of
    // the process, so the late write goes through it. _SH_DENYNO is explicit
    // rather than relying on fopen defaults: we must not be the reason the
    // engine cannot open its own pak.
    //
    // This is what makes a LIVE resolution usable at all - and therefore what
    // makes a changed Virtual Desktop slider, a different headset or a changed
    // render_scale correct ON THE SAME LAUNCH instead of one restart later.
    // >>> AND THE RETAINED HANDLE BROKE THE ENGINE'S OWN OPEN. MEASURED. <<<
    //
    // Holding the write handle DID let the late write land - the pak on disk
    // afterwards was valid and contained 1792x1920. The engine then rendered
    // 2688x2880: FULL per-eye, i.e. VANILLA, so it never read our entry at all.
    //
    // CONTROLLED PAIR, one variable, two consecutive runs:
    //   no retained handle -> RT0 1706x960   (our pak, startup version)
    //   retained handle    -> RT0 2688x2880  (vanilla - our pak was not used)
    //
    // _SH_DENYNO says what WE permit others; it cannot widen what OTHERS permit
    // US. The engine opens its paks with a share mode that does not tolerate an
    // existing GENERIC_WRITE handle, so its open failed and it fell back to
    // Data0.pak's stock render_loop.ppfx - silently, because a missing override
    // is not an error to the engine.
    //
    // SO THE HANDLE IS CLOSED AGAIN. A same-launch rewrite of Data3.pak is not
    // merely late, it is IMPOSSIBLE: writing requires a handle, and holding one
    // stops the engine reading the file we are writing.
    FILE* f = nullptr;
    {
        if (fopen_s(&f, pak, "r+b") != 0 || !f) {
            PLOG("[PAKGEN] cannot open %s for update - leaving it alone. If this is the RUNTIME "
                 "pass, the startup pass never got a handle either and the engine now holds it.",
                 pak);
            return false;
        }
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (size < 22) { fclose(f); return false; }
    std::vector<unsigned char> buf(static_cast<size_t>(size));
    fseek(f, 0, SEEK_SET);
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); return false; }

    // Find the End Of Central Directory, then walk the central directory for our
    // entry. Parsed rather than hardcoded: the pak also carries the wasteland
    // LOD data, which has been re-packed independently more than once.
    long eocd = -1;
    for (long i = size - 22; i >= 0 && i > size - 65557; --i) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
            eocd = i; break;
        }
    }
    if (eocd < 0) {
        PLOG("[PAKGEN] no ZIP end-of-central-directory in %s.", pak);
        fclose(f); return false;
    }
    unsigned short entries = 0; memcpy(&entries, &buf[eocd + 10], 2);
    unsigned int   cdOff   = 0; memcpy(&cdOff,   &buf[eocd + 16], 4);
    long cd = static_cast<long>(cdOff);

    for (int e = 0; e < entries; ++e) {
        if (cd + 46 > size || buf[cd] != 0x50 || buf[cd + 1] != 0x4b) { break; }
        unsigned short method = 0, namelen = 0, extlen = 0, cmtlen = 0;
        unsigned int   csize  = 0, lho = 0;
        memcpy(&method,  &buf[cd + 10], 2);
        memcpy(&csize,   &buf[cd + 20], 4);
        memcpy(&namelen, &buf[cd + 28], 2);
        memcpy(&extlen,  &buf[cd + 30], 2);
        memcpy(&cmtlen,  &buf[cd + 32], 2);
        memcpy(&lho,     &buf[cd + 42], 4);
        const std::string name(reinterpret_cast<char*>(&buf[cd + 46]), namelen);
        if (name == kEntry) {
            if (method != 0 || csize != kPadTo) {
                PLOG("[PAKGEN] %s is method %u at %u bytes - in-place patching needs STORED at "
                     "exactly %zu. This pak was not built for it; leaving it alone.",
                     kEntry, method, csize, kPadTo);
                fclose(f); return false;
            }
            unsigned short lnamelen = 0, lextlen = 0;
            memcpy(&lnamelen, &buf[lho + 26], 2);
            memcpy(&lextlen,  &buf[lho + 28], 2);
            const long data = static_cast<long>(lho) + 30 + lnamelen + lextlen;

            std::string padded = content;
            padded.append(kPadTo - content.size(), '\n');   // inert to the parser - measured
            const unsigned long crc =
                Crc32(reinterpret_cast<const unsigned char*>(padded.data()), padded.size());

            fseek(f, data, SEEK_SET);     fwrite(padded.data(), 1, padded.size(), f);
            fseek(f, lho + 14, SEEK_SET); fwrite(&crc, 4, 1, f);   // local header CRC
            fseek(f, cd + 16, SEEK_SET);  fwrite(&crc, 4, 1, f);   // central directory CRC
            fclose(f);
            PLOG("[PAKGEN] patched %s in place: %zu bytes + %zu newlines of pad, CRC %08lX. "
                 "The other %d entries were not touched.",
                 kEntry, content.size(), kPadTo - content.size(), crc, entries - 1);
            return true;
        }
        cd += 46 + namelen + extlen + cmtlen;
    }
    PLOG("[PAKGEN] %s is not in %s - nothing to patch.", kEntry, pak);
    fclose(f);
    return false;
}


// >>> THREE OF THE FIVE MODES NEED NO RESOLUTION AT ALL. <<<
//
// BufferScreenFormat's second field is a DIVISOR OF SCREEN, so divisors 1, 2
// and 3 are EXACTLY the DLSS linear factors 1.00x, 2.00x and 3.00x - at any
// output resolution, on any headset, whatever the user does to their Virtual
// Desktop slider. DLAA, Performance and Ultra Performance are therefore
// resolution-independent BY CONSTRUCTION and can never go stale.
//
// Quality (1.50x) and Balanced (1.72x) are not integer divisors. Those two, and
// only those two, need explicit pixel sizes and therefore need to know the
// output resolution.
//
// Divisor 3 is UNPROVEN: the shipped file only ever uses 1, 2, 4, 8 and 16, so
// 3 is an extrapolation from a pattern. It is flagged where it is used rather
// than assumed to work.
int DivisorForMode(int mode)
{
    switch (mode) {
        case 0: return 1;    // DLAA              1.00x
        case 3: return 2;    // Performance       2.00x
        case 4: return 3;    // Ultra Performance 3.00x
        default: return 0;   // Quality / Balanced - not expressible as a divisor
    }
}

std::string DivTemplate()
{
    std::string s;
    for (int i = 0; i < kRenderLoopDivTmplParts; ++i) { s += kRenderLoopDivTmpl[i]; }
    return s;
}

// The divisor path. No output resolution is consulted, so nothing here can be
// stale, wrong for the wrong display, or invalidated by a changed headset.
bool RegenerateByDivisor(int mode, int d, const char* why)
{
    std::string t = DivTemplate();
    if (t.size() != kRenderLoopDivTmplRawBytes) {
        PLOG("[PAKGEN] embedded DIVISOR template reassembled to %zu bytes, expected %zu - the "
             "split literals did not join. Refusing to write.",
             t.size(), kRenderLoopDivTmplRawBytes);
        return false;
    }
    char num[32];
    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", d); ReplaceAll(t, "@D1@", num);
    for (int lvl = 2; lvl <= 16; lvl *= 2) {
        char tok[8];
        _snprintf_s(tok, sizeof(tok), _TRUNCATE, "@D%d@", lvl);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", lvl * d);
        ReplaceAll(t, tok, num);
    }
    ReplaceAll(t, "@AA_NATIVE@", g_nativeAaPak ? g_nativeAaBlockText : std::string());
    SunFxStep(t);
    DustFxStep(t);
    EngineJitterStep(t);
    if (t.find('@') != std::string::npos) {
        PLOG("[PAKGEN] a token survived the divisor substitution - refusing to write a "
             "half-filled template.");
        return false;
    }
    // fixed Data3: the same decision as values (design doc §3.2). Explicit sizes 0 on the divisor path.
    PpSet("DLVR_DIV", d);
    for (int lvl = 1; lvl <= 16; lvl *= 2) {
        char nD[16], nW[16], nH[16];
        _snprintf_s(nD, sizeof(nD), _TRUNCATE, "DLVR_D%d", lvl);
        _snprintf_s(nW, sizeof(nW), _TRUNCATE, "DLVR_W%d", lvl);
        _snprintf_s(nH, sizeof(nH), _TRUNCATE, "DLVR_H%d", lvl);
        PpSet(nD, lvl * d); PpSet(nW, 0); PpSet(nH, 0);
    }
    PpSet("DLVR_NATIVE_AA", g_nativeAaPak ? 1 : 0);
    PpSet("DLVR_NATIVE_AA_EDGES", (g_nativeAaPak && g_nativeAaEdges) ? 1 : 0);
    g_ppRenderSet = true;
    const char* label = g_nativeAaPak ? "NATIVE AA (SMAA 1x, no DLSS)"
                      : g_taaPak      ? "TAA (native resolution, no DLSS)"
                      : g_noAaPak     ? "NO AA (native resolution)"
                      : (g_fsrPak && mode >= 0 && mode <= 3) ? kFsrModeNames[mode]
                                      : kModes[mode].name;
    PLOG("[PAKGEN] mode %d = %s at DIVISOR %d (%s). No output resolution is involved, so this is "
         "correct at any resolution and on any headset.%s",
         mode, label, d, why,
         d == 3 ? "  NOTE: divisor 3 is UNPROVEN - the shipped file never exceeds 16 and only "
                  "uses powers of two. If the scene comes back full-size, the engine rejected it."
                : "");

    char pak[MAX_PATH];
    _snprintf_s(pak, sizeof(pak), _TRUNCATE, "%s/DW/Data3.pak", g_gameDir);
    if (!PatchPak(pak, t)) { return false; }
    g_wroteW = 0; g_wroteH = 0;   // no output size was assumed, so none is recorded
    if (g_nativeAaPak) {
        // No DLSS mode is live, so the overlay must not be told one is.
        g_activeMode = -1; g_activeDiv = d; g_activeW = 0; g_activeH = 0;
        PLOG("[PAKGEN] *** %s READY *** divisor %d, SMAA 1x block written (%zu bytes)%s",
             label, d, g_nativeAaBlockText.size(),
             g_nativeAaEdges ? " - *** EDGE-MASK DEBUG: the view shows the SMAA edge mask, NOT a "
                               "playable image. Change the first line of aa_native.txt to revert. ***"
                             : ".");
        return true;
    }
    if (g_taaPak) {
        g_activeMode = -1; g_activeDiv = d; g_activeW = 0; g_activeH = 0;
        PLOG("[PAKGEN] *** %s READY *** divisor %d, SMAA token EMPTY. The TAA itself is reported "
             "under [TAA] by dlssflat later in this log.", label, d);
        return true;
    }
    if (g_noAaPak) {
        g_activeMode = -1; g_activeDiv = d; g_activeW = 0; g_activeH = 0;
        PLOG("[PAKGEN] *** %s READY *** divisor %d, SMAA token EMPTY - no AA of any kind this launch.",
             label, d);
        return true;
    }
    if (g_fsrPak) {
        // The overlay's "running now" line reads this, so FSR reports its mode
        // (0 NATIVE .. 3 PERFORMANCE) rather than -1.
        g_activeMode = mode; g_activeDiv = d; g_activeW = 0; g_activeH = 0;
        PLOG("[PAKGEN] *** %s READY *** divisor %d, SMAA token EMPTY. FSR itself is reported "
             "under [FSR] by dlssflat later in this log.", label, d);
        return true;
    }
    g_activeMode = mode; g_activeDiv = d; g_activeW = 0; g_activeH = 0;
    PLOG("[PAKGEN] *** %s READY *** divisor %d. Cross-check against the [DLSSUP] measured ratio "
         "later in this log.", label, d);
    return true;
}

// >>> THE LAST KNOWN PER-EYE SIZE, WRITTEN BY THE LAYER EACH LAUNCH. <<<
//
// A same-launch rewrite is IMPOSSIBLE, measured two independent ways: the
// engine takes Data3.pak within ~2 s of start and holds it for the session
// (fopen "r+b" fails at T+2 s and again at T+30 s), and holding a write handle
// from before its open makes the ENGINE's open fail instead, so it silently
// falls back to Data0.pak's stock declarations. Writing needs a handle; holding
// one stops the engine reading the file.
//
// So the only writable moment is startup, and the only way startup can know a
// VR resolution is to have been told during the PREVIOUS launch. That is a
// cache, and a cache is stale exactly once - on the first launch after the user
// changes resolution - which is why the wrong answer must not be a silent one,
// and why the fallback below is a DIVISOR rather than a guessed pixel size.
bool ReadEyeResCache(int* w, int* h)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s/eye_res.txt", g_dlvrDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { return false; }
    char buf[64] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    int cw = 0, ch = 0;
    if (sscanf_s(buf, "%d %d", &cw, &ch) != 2 ||
        cw < 320 || ch < 240 || cw > 16384 || ch > 16384) {
        PLOG("[PAKGEN] %s exists but does not read as a WxH pair - ignoring it.", path);
        return false;
    }
    *w = cw; *h = ch;
    return true;
}

void WriteEyeResCache(int w, int h)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s/eye_res.txt", g_dlvrDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        PLOG("[PAKGEN] could not write %s - the next launch will not know this size.", path);
        return;
    }
    fprintf(f, "%d %d\n", w, h);
    fprintf(f, "# Per-eye render size reported by ovr_GetFovTextureSize on the last VR launch.\n");
    fprintf(f, "# Rewritten every VR launch. Delete it and the next launch falls back to a\n");
    fprintf(f, "# resolution-independent divisor instead of guessing.\n");
    fclose(f);
}

// [DATA3] MD5 of a buffer as 32 lowercase hex digits via CryptoAPI. false = no verdict (the provider or hash was refused).
bool Md5Hex(const unsigned char* data, size_t n, char out[33])
{
    out[0] = '\0';
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) != FALSE;
    if (ok) { ok = CryptCreateHash(prov, CALG_MD5, 0, 0, &hash) != FALSE; }
    if (ok) { ok = CryptHashData(hash, data, static_cast<DWORD>(n), 0) != FALSE; }
    BYTE dig[16] = {};
    DWORD len = sizeof(dig);
    if (ok) { ok = CryptGetHashParam(hash, HP_HASHVAL, dig, &len, 0) != FALSE && len == 16; }
    if (hash) { CryptDestroyHash(hash); }
    if (prov) { CryptReleaseContext(prov, 0); }
    if (!ok) { return false; }
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) { out[2 * i] = kHex[dig[i] >> 4]; out[2 * i + 1] = kHex[dig[i] & 15]; }
    out[32] = '\0';
    return true;
}

// [DATA3]: the verdict on the whole file, logged by name every launch (it decides whether AFW may run).
void CheckData3Md5(const char* pak, const std::vector<unsigned char>* buf)
{
    char got[33] = {};
    const bool hashed = buf && !buf->empty() && Md5Hex(buf->data(), buf->size(), got);
    const int st = Data3Verdict(hashed, hashed ? got : nullptr);
    InterlockedExchange(&g_data3State, st);
    // [PROBE: DATA3] gate=none (every launch)  site=FLAT  log-only=no
    PLOG("[DATA3] %s md5 %s, this build ships %s -> %s", pak, hashed ? got : "(not read)", kFixedData3Md5,
         st == kData3Ours ? "OURS - AFW may run"
         : st == kData3NotOurs ? "*** NOT THIS BUILD's Data3.pak *** - AFW is REFUSED this launch (a stock / other pak clears "
                                 "the held eye to black); install the Data3.pak that came with this build"
                               : "*** UNREADABLE *** - no verdict, AFW is REFUSED this launch");
}

// Which Data3 is installed (design doc §3.5): read the three ppfx entries' method / size / CRC from the central directory.
// Read-only - the pak is opened "rb".
int DetectPpLayout()
{
    char pak[MAX_PATH];
    _snprintf_s(pak, sizeof(pak), _TRUNCATE, "%s\\DW\\Data3.pak", g_gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, pak, "rb") != 0 || !f) {
        // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] Data3 layout: *** cannot read %s *** - treated as LEGACY (today's path).", pak);
        CheckData3Md5(pak, nullptr);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    std::vector<unsigned char> buf(size > 0 ? static_cast<size_t>(size) : 0);
    fseek(f, 0, SEEK_SET);
    const bool ok = size >= 22 && fread(buf.data(), 1, buf.size(), f) == buf.size();
    fclose(f);
    CheckData3Md5(pak, ok ? &buf : nullptr);
    if (!ok) { PLOG("[PAKGEN] Data3 layout: %s unreadable - treated as LEGACY.", pak); return 0; }
    long eocd = -1;
    for (long i = size - 22; i >= 0 && i > size - 65557; --i) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) { PLOG("[PAKGEN] Data3 layout: no ZIP directory in %s - treated as LEGACY.", pak); return 0; }
    unsigned short entries = 0; memcpy(&entries, &buf[eocd + 10], 2);
    unsigned int cdOff = 0; memcpy(&cdOff, &buf[eocd + 16], 4);
    long cd = static_cast<long>(cdOff);
    unsigned long crcRl = 0, crcOc = 0, crcSp = 0, szRl = 0, szOc = 0, szSp = 0;
    unsigned short mRl = 99;
    for (int e = 0; e < entries; ++e) {
        if (cd + 46 > size || buf[cd] != 0x50 || buf[cd + 1] != 0x4b) { break; }
        unsigned short method = 0, namelen = 0, extlen = 0, cmtlen = 0;
        unsigned int crc = 0, csize = 0;
        memcpy(&method, &buf[cd + 10], 2); memcpy(&crc, &buf[cd + 16], 4); memcpy(&csize, &buf[cd + 20], 4);
        memcpy(&namelen, &buf[cd + 28], 2); memcpy(&extlen, &buf[cd + 30], 2); memcpy(&cmtlen, &buf[cd + 32], 2);
        const std::string name(reinterpret_cast<char*>(&buf[cd + 46]), namelen);
        if (name == kEntry)        { crcRl = crc; szRl = csize; mRl = method; }
        else if (name == kOculusEntry) { crcOc = crc; szOc = csize; }
        else if (name == kSpriteEntry) { crcSp = crc; szSp = csize; }
        cd += 46 + namelen + extlen + cmtlen;
    }
    if (mRl == 0 && szRl == kPadTo) {
        // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] Data3 layout: LEGACY (render_loop STORED at %zu, padded) - in-place patching as before; no DW\\%s.",
             kPadTo, kPpMthName);
        return 0;
    }
    if (crcRl == kPpCrcRenderLoop && szRl == kPpSizeRenderLoop && crcOc == kPpCrcOculus && szOc == kPpSizeOculus &&
        crcSp == kPpCrcSprite50 && szSp == kPpSizeSprite50) {
        // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] Data3 layout: FIXED v%d (render_loop %08lX/%lu, oculus %08lX/%lu, sprite50 %08lX/%lu) - Data3.pak is "
             "NOT touched this launch; every per-machine value goes to DW\\%s.", kPpLayoutVersion, crcRl, szRl, crcOc, szOc,
             crcSp, szSp, kPpMthName);
        return 1;
    }
    // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
    PLOG("[PAKGEN] *** Data3 layout: NEITHER legacy NOR this bootstrap's fixed v%d *** (render_loop %08lX/%lu want %08lX/%lu, "
         "oculus %08lX/%lu want %08lX/%lu, sprite50 %08lX/%lu want %08lX/%lu). The pak is NOT touched; DW\\%s is written, but "
         "if this pak expects other names the render loop may not load - install the Data3.pak that came with this build.",
         kPpLayoutVersion, crcRl, szRl, kPpCrcRenderLoop, kPpSizeRenderLoop, crcOc, szOc, kPpCrcOculus, kPpSizeOculus, crcSp,
         szSp, kPpCrcSprite50, kPpSizeSprite50, kPpMthName);
    return 2;
}

// The explicit-size path's values (scene w1 x h1 and its /2../16 pyramid, divisors 0). Shared by RegenerateFor and the
// same-launch rewrite (early_eye_size), so the two cannot disagree about a value.
void PpSetExplicitSizes(int w1, int h1)
{
    PpSet("DLVR_DIV", 0);
    PpSet("DLVR_W1", w1); PpSet("DLVR_H1", h1); PpSet("DLVR_D1", 0);
    for (int lvl = 2; lvl <= 16; lvl *= 2) {
        char nD[16], nW[16], nH[16];
        _snprintf_s(nD, sizeof(nD), _TRUNCATE, "DLVR_D%d", lvl);
        _snprintf_s(nW, sizeof(nW), _TRUNCATE, "DLVR_W%d", lvl);
        _snprintf_s(nH, sizeof(nH), _TRUNCATE, "DLVR_H%d", lvl);
        PpSet(nD, 0); PpSet(nW, DivRound(w1, lvl)); PpSet(nH, DivRound(h1, lvl));
    }
    PpSet("DLVR_NATIVE_AA", 0);
    PpSet("DLVR_NATIVE_AA_EDGES", 0);
    g_ppRenderSet = true;
}

// The .mth text for the current g_ppVal - the header comment and one export per name. Shared by the attach write and the
// same-launch rewrite (early_eye_size), so the two cannot format a value differently.
std::string PpMthBody()
{
    std::string body = "// dlvr - per-machine render values, written by pakgen at startup (DESIGN-COOP-FIXED-DATA3). Not in any pak.\r\n";
    for (int i = 0; i < kPpNameCount; ++i) {
        char line[96];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "export int %s = %d;\r\n", kPpNames[i], g_ppVal[i]);
        body += line;
    }
    return body;
}

// Writes DW\dlvr_pp.mth (tmp + replace, read back). When no generator ran this launch, the render-group values come from
// the previous file (today's equivalent: the pak keeps the last launch's render_loop).
void WritePpMth()
{
    if (g_ppLayout == 0 && !g_ppWriteInLegacy) { return; }
    PpSet("DLVR_PP_VERSION", kPpLayoutVersion);
    char path[MAX_PATH], tmp[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\DW\\%s", g_gameDir, kPpMthName);
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);
    int carried = 0;
    if (!g_ppRenderSet) {
        // The render group is the PREVIOUS file's or 0 - never this launch's partial reads (sun / dust were read above but
        // no render loop was decided, exactly as today's pak keeps its last render_loop whole).
        for (int i = 0; i < kPpNameCount; ++i) {
            if (PpIsRenderName(kPpNames[i])) { g_ppVal[i] = 0; }
        }
        FILE* pf = nullptr;
        if (fopen_s(&pf, path, "rb") == 0 && pf) {
            char line[160];
            while (fgets(line, sizeof(line), pf)) {
                char nm[64] = {};
                int v = 0;
                if (sscanf_s(line, "export int %63s = %d;", nm, static_cast<unsigned>(sizeof(nm)), &v) == 2 && PpIsRenderName(nm)) {
                    PpSet(nm, v);
                    ++carried;
                }
            }
            fclose(pf);
        }
        // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] dlvr_pp.mth: no render data decided this launch (as today: the render loop stays as last written) - "
             "%d render value(s) carried over from the previous file%s.", carried,
             carried ? "" : " (*** none found - the render values are 0 ***)");
    }
    std::string body = PpMthBody();
    if (g_earlyWanted) {
        // early_eye_size: a comment line the same-launch rewrite pads into, so the file keeps this exact length.
        std::string pad = kEarlyReserveHead;
        pad.append(kEarlyReserveBytes - pad.size() - 2, ' ');
        pad += "\r\n";
        body += pad;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "wb") != 0 || !f) {
        // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] *** dlvr_pp.mth NOT WRITTEN *** cannot create %s - the previous / shipped file stays in use.", tmp);
        return;
    }
    const size_t wrote = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (wrote != body.size() || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        PLOG("[PAKGEN] *** dlvr_pp.mth NOT WRITTEN *** (%zu of %zu bytes to %s, replace error %lu) - the previous / shipped file "
             "stays in use.", wrote, body.size(), tmp, GetLastError());
        DeleteFileA(tmp);
        return;
    }
    std::string back;
    FILE* rf = nullptr;
    if (fopen_s(&rf, path, "rb") == 0 && rf) {
        char rb[4096];
        size_t n = 0;
        while ((n = fread(rb, 1, sizeof(rb), rf)) > 0) { back.append(rb, n); }
        fclose(rf);
    }
    if (g_earlyWanted && back == body) { g_earlyMthLen = body.size(); }   // the same-launch rewrite must match this length
    // [PROBE: PAKGEN] gate=none (fixed Data3)  site=FLAT  log-only=yes
    PLOG("[PAKGEN] dlvr_pp.mth %s %s (%zu bytes): DIV %d, scene %dx%d, native AA %d%s, sun %d%d%d%d, dust %d, jitter %d, "
         "keep-RIFT %d%s, HUD %d.", back == body ? "WRITTEN + READ BACK" : "*** WRITTEN BUT READ-BACK DIFFERS ***", path,
         body.size(), PpGet("DLVR_DIV"), PpGet("DLVR_W1"), PpGet("DLVR_H1"), PpGet("DLVR_NATIVE_AA"),
         PpGet("DLVR_NATIVE_AA_EDGES") ? " (EDGES)" : "", PpGet("DLVR_SUN_A"), PpGet("DLVR_SUN_B"), PpGet("DLVR_SUN_C"),
         PpGet("DLVR_SUN_D"), PpGet("DLVR_DUST_VR_OFF"), PpGet("DLVR_JITTER_ENGINE"), PpGet("DLVR_KEEP_RIFT"),
         PpGet("DLVR_RIFT_PROBE") ? " + PROBE" : "", PpGet("DLVR_HUD_LAYER"));
}

struct PpMthAtExit { ~PpMthAtExit() { WritePpMth(); } };   // every return path of GenerateRenderLoopPak

// Controls tab (plan docs/superpowers/plans/2026-10-28-controls-rebind.md Task 4): writes DW\dlvr_binds.scr - the four base
// input layouts as subs - from dlvr\binds.txt, EVERY launch, whatever the Data3 layout. Our Data3 inputs_pad.scr imports it and
// `use`s its subs; a missing import is a parse error in the game's input script (gamedll +12DBBE0, parsernodes.cpp:1426), so a
// missing / unreadable binds.txt, or a write failure of the tmp file, still leaves a DEFAULT file in place. Not in any pak (D-203).
void WriteBindsScr()
{
    char src[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH];
    _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\binds.txt", g_dlvrDir);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\DW\\dlvr_binds.scr", g_gameDir);
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);
    std::string text;
    bool haveSrc = false;
    FILE* sf = nullptr;
    if (fopen_s(&sf, src, "rb") == 0 && sf) {
        char rb[4096];
        size_t n = 0;
        while ((n = fread(rb, 1, sizeof(rb), sf)) > 0) { text.append(rb, n); }
        fclose(sf);
        haveSrc = true;
    }
    dlvr::binds::Binds b = dlvr::binds::Defaults();
    std::string report;
    const int rejected = dlvr::binds::Parse(text.c_str(), &b, &report);
    const std::string body = dlvr::binds::EmitScr(b);
    const dlvr::binds::Binds def = dlvr::binds::Defaults();
    std::string changed;
    int nChanged = 0;
    for (int a = 0; a < dlvr::binds::gen::kActionCount; ++a) {
        if (dlvr::binds::Equal(b.s[a][0], def.s[a][0]) && dlvr::binds::Equal(b.s[a][1], def.s[a][1])) { continue; }
        ++nChanged;
        if (changed.size() < 900) { changed += changed.empty() ? "" : " "; changed += dlvr::binds::gen::kActions[a].id; }
    }
    // [PROBE: BINDS] gate=none (every launch)  site=FLAT  log-only=no
    PLOG("[BINDS] %s: %s - %d action(s) changed from default%s%s, %d line(s) rejected.", src,
         haveSrc ? "read" : "not present (defaults)", nChanged, nChanged ? ": " : "", changed.c_str(), rejected);
    for (size_t p = 0; p < report.size();) {
        const size_t e = report.find('\n', p);
        PLOG("[BINDS] rejected %s", report.substr(p, e == std::string::npos ? std::string::npos : e - p).c_str());
        p = e == std::string::npos ? report.size() : e + 1;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "wb") != 0 || !f) {
        PLOG("[BINDS] *** dlvr_binds.scr NOT WRITTEN *** cannot create %s (error %lu) - the previous file stays in use%s.", tmp,
             GetLastError(), GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES ? "; *** THERE IS NONE - the game's pad input "
             "script will fail to parse ***" : "");
        return;
    }
    const size_t wrote = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (wrote != body.size() || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        PLOG("[BINDS] *** dlvr_binds.scr NOT WRITTEN *** (%zu of %zu bytes to %s, replace error %lu) - the previous file stays in "
             "use%s.", wrote, body.size(), tmp, GetLastError(), GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES
             ? "; *** THERE IS NONE - the game's pad input script will fail to parse ***" : "");
        DeleteFileA(tmp);
        return;
    }
    std::string back;
    FILE* rf = nullptr;
    if (fopen_s(&rf, path, "rb") == 0 && rf) {
        char rb[4096];
        size_t n = 0;
        while ((n = fread(rb, 1, sizeof(rb), rf)) > 0) { back.append(rb, n); }
        fclose(rf);
    }
    PLOG("[BINDS] %s %s (%zu bytes).", back == body ? "WRITTEN + READ BACK" : "*** WRITTEN BUT READ-BACK DIFFERS ***", path,
         body.size());
}
}  // namespace

// Called from the bootstrap's early init, before the engine opens any pak.
bool RegenerateFor(int ow, int oh, int mode, const char* src);
void StartPakLockProbe();

void GenerateRenderLoopPak(const char* dlvrDir, const char* gameDir)
{
    // Kept for the runtime re-generation below, which has no way to derive them.
    strncpy_s(g_dlvrDir, sizeof(g_dlvrDir), dlvrDir, _TRUNCATE);
    strncpy_s(g_gameDir, sizeof(g_gameDir), gameDir, _TRUNCATE);

    // Fixed Data3 (design doc, step 2): WHICH pak is installed picks the path; the routing below is unchanged either way.
    g_ppLayout = DetectPpLayout();
    WriteBindsScr();   // DW\dlvr_binds.scr - every launch, before any return below (Controls tab)
    for (int i = 0; i < kPpNameCount; ++i) { g_ppVal[i] = 0; }
    g_ppRenderSet = false;
    PpMthAtExit ppAtExit;   // writes DW\dlvr_pp.mth on every return below (FIXED / OTHER layout only)

    char marker[MAX_PATH];
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\dlss_quality.txt", dlvrDir);

    // SUN EFFECTS OFF IN VR (INV-276) - read first, applied by whichever generator runs. Printed both ways.
    {
        char sunMarker[MAX_PATH];
        _snprintf_s(sunMarker, sizeof(sunMarker), _TRUNCATE, "%s\\sunfx_vr_off.txt", dlvrDir);
        char sbuf[64] = {};
        FILE* sf = nullptr;
        g_sunFxOff.clear();
        if (fopen_s(&sf, sunMarker, "rb") == 0 && sf) {
            fread(sbuf, 1, sizeof(sbuf) - 1, sf);
            fclose(sf);
            for (const char* q = sbuf; *q && *q != '\r' && *q != '\n'; ++q) {
                const char c = static_cast<char>(tolower(static_cast<unsigned char>(*q)));
                if (c >= 'a' && c <= 'd' && g_sunFxOff.find(c) == std::string::npos) { g_sunFxOff += c; }
            }
        }
        PpSet("DLVR_SUN_A", g_sunFxOff.find('a') != std::string::npos ? 1 : 0);
        PpSet("DLVR_SUN_B", g_sunFxOff.find('b') != std::string::npos ? 1 : 0);
        PpSet("DLVR_SUN_C", g_sunFxOff.find('c') != std::string::npos ? 1 : 0);
        PpSet("DLVR_SUN_D", g_sunFxOff.find('d') != std::string::npos ? 1 : 0);
        // [PROBE: PAKGEN] gate=none  site=FLAT  log-only=yes
        PLOG("[PAKGEN] sun effects VR gates: %s (%s, first line \"%.16s\")",
             g_sunFxOff.empty() ? "OFF - every sun pass runs in VR, pak byte-identical to before" : g_sunFxOff.c_str(),
             sf ? "sunfx_vr_off.txt present" : "no sunfx_vr_off.txt", sbuf);
    }

    // AMBIENT DUST/SOOT PARTICLES OFF IN VR - presence of the marker = on. Printed both ways.
    {
        char dustMarker[MAX_PATH];
        _snprintf_s(dustMarker, sizeof(dustMarker), _TRUNCATE, "%s\\dustfx_vr_off.txt", dlvrDir);
        g_dustFxOff = GetFileAttributesA(dustMarker) != INVALID_FILE_ATTRIBUTES;
        PpSet("DLVR_DUST_VR_OFF", g_dustFxOff ? 1 : 0);
        // [PROBE: PAKGEN] gate=none  site=FLAT  log-only=yes
        PLOG("[PAKGEN] dust particles VR gate: %s (%s)",
             g_dustFxOff ? "ON - the dust/soot particle pass is skipped in VR" : "OFF - dust/soot particles draw in VR, pak byte-identical to before",
             g_dustFxOff ? "dustfx_vr_off.txt present" : "no dustfx_vr_off.txt");
    }

    // REAL ENGINE JITTER (INV-436) - presence of the marker = on (applied only under DLSS / FSR with jitter_halton.txt).
    {
        char jeMarker[MAX_PATH];
        _snprintf_s(jeMarker, sizeof(jeMarker), _TRUNCATE, "%s\\jitter_engine.txt", dlvrDir);
        g_engineJitterMarker = GetFileAttributesA(jeMarker) != INVALID_FILE_ATTRIBUTES;
        // [PROBE: PAKGEN] gate=none  site=FLAT  log-only=yes
        PLOG("[PAKGEN] engine jitter marker: %s",
             g_engineJitterMarker ? "jitter_engine.txt present - the jitter gate is swapped if the method is DLSS / FSR"
                                  : "no jitter_engine.txt - stock jitter gate (the engine applies no jitter), pak byte-identical to before");
    }

    // AFW KEEP-RIFT (INV-374) - presence = no RIFT clears; first line "probe" = also skip the right-eye composite.
    // Runs before any early return below, so it applies whatever AA mode this launch uses. Printed both ways.
    {
        char krMarker[MAX_PATH];
        _snprintf_s(krMarker, sizeof(krMarker), _TRUNCATE, "%s\\afw_keep_rift.txt", dlvrDir);
        char kbuf[16] = {};
        FILE* kf = nullptr;
        const bool keep = fopen_s(&kf, krMarker, "rb") == 0 && kf;
        if (kf) { fread(kbuf, 1, sizeof(kbuf) - 1, kf); fclose(kf); }
        const bool probe = keep && _strnicmp(kbuf, "probe", 5) == 0;
        // [PROBE: PAKGEN] gate=none  site=FLAT  log-only=yes
        PLOG("[PAKGEN] AFW keep-RIFT marker: %s (%s)", keep ? (probe ? "ON + PROBE" : "ON") : "OFF - oculus.ppfx kept stock",
             keep ? "afw_keep_rift.txt present" : "no afw_keep_rift.txt");
        OculusKeepRiftStep(keep, probe);
    }

    // AFW HUD LAYER v4 (2026-10-07) - presence = the sprite50.ppfx bracket binds run; first line "count" = counted only.
    // Before any early return below, so it applies whatever AA mode this launch uses (D-183). Printed both ways.
    // GATED ON afw_enable since 2026-10-08 (Jay: "Gate the HUD layer on afw_enable ... That's a verified fix" - INV-484 / 485):
    // ON when the layer's saved settings say afw_enable=1, or the file / key is absent (afw_enable's compiled default is 1 and
    // the layer writes the key on its first launch). The bootstrap runs before the layer parses ovr2xr.txt, so it is read here
    // as text; its value is what the layer wrote at the end of the last session. A live toggle cannot reach the pak (INV-12):
    // AFW switched ON mid-session after an OFF launch has no HUD layer until a restart; switched OFF mid-session the brackets
    // stay but nothing is redirected (no DlvrHudLayerSubmit handshake - the HUD draws into RIFT as stock, dlssflat.cpp).
    // dlvr\hud_layer.txt stays a developer OVERRIDE: present = on whatever afw_enable says; first line "count" = counted only.
    {
        char hlMarker[MAX_PATH];
        _snprintf_s(hlMarker, sizeof(hlMarker), _TRUNCATE, "%s\\hud_layer.txt", dlvrDir);
        char hbuf[16] = {};
        FILE* hf = nullptr;
        const bool marker = fopen_s(&hf, hlMarker, "rb") == 0 && hf;
        if (hf) { fread(hbuf, 1, sizeof(hbuf) - 1, hf); fclose(hf); }
        const bool count = marker && _strnicmp(hbuf, "count", 5) == 0;
        // afw_enable from the layer's settings file: "afw_enable=<0|1>" at the start of a line, the layer parser's own match
        char sPath[MAX_PATH];
        _snprintf_s(sPath, sizeof(sPath), _TRUNCATE, "%s\\ovr2xr.txt", dlvrDir);
        int afw = -1;                                                    // -1 = file unreadable / key absent
        FILE* sf = nullptr;
        const bool fileOk = fopen_s(&sf, sPath, "r") == 0 && sf;
        if (fileOk) {
            char line[512];
            while (fgets(line, sizeof(line), sf)) {
                if (_strnicmp(line, "afw_enable=", 11) != 0) { continue; }
                const char* v = line + 11;
                while (*v == ' ' || *v == '\t') { ++v; }
                afw = (*v == '1') ? 1 : (*v == '0') ? 0 : -1;
                break;
            }
            fclose(sf);
        }
        const bool on = marker || afw != 0;                              // absent / unreadable = the compiled default, 1
        // [PROBE: PAKGEN] gate=none  site=FLAT  log-only=yes
        PLOG("[PAKGEN] HUD layer: %s - afw_enable in ovr2xr.txt = %s, override marker %s",
             on ? (count ? "ON + COUNT (brackets counted, nothing redirected)" : "ON") : "OFF - sprite50.ppfx marker binds commented",
             afw == 1 ? "1" : afw == 0 ? "0"
                 : (fileOk ? "NOT FOUND (no afw_enable line - default 1)" : "NOT READ (no ovr2xr.txt - default 1)"),
             marker ? (count ? "hud_layer.txt present (count)" : "hud_layer.txt present") : "absent");
        SpriteHudStep(on ? (count ? 2 : 1) : 0);
    }

    // NATIVE AA WINS OVER ANY DLSS MODE - the two are alternatives, never stacked.
    // Printed both ways, so "off" and "never checked" do not look alike.
    char nativeMarker[MAX_PATH];
    _snprintf_s(nativeMarker, sizeof(nativeMarker), _TRUNCATE, "%s\\aa_native.txt", dlvrDir);
    g_nativeAaPak = GetFileAttributesA(nativeMarker) != INVALID_FILE_ATTRIBUTES;
    // [PROBE: PAKGEN] gate=none (native-AA state line prints both ways; the AA method line prints every launch; the rest of [PAKGEN] needs dlss_quality.txt, aa_method.txt or aa_native.txt)  site=FLAT  log-only=yes
    if (g_nativeAaPak) {
        // First line "edges" -> the edge-mask debug variant. Printed both ways.
        char nbuf[64] = {};
        FILE* nf = nullptr;
        if (fopen_s(&nf, nativeMarker, "rb") == 0 && nf) {
            fread(nbuf, 1, sizeof(nbuf) - 1, nf);
            fclose(nf);
        }
        g_nativeAaEdges = _strnicmp(nbuf, "edges", 5) == 0;
        g_nativeAaBlockText = kNativeAaBlock;
        if (g_nativeAaEdges) {
            const size_t at = g_nativeAaBlockText.find(kNativeAaFinalCopy);
            if (at == std::string::npos) {
                PLOG("[PAKGEN] EDGE-MASK DEBUG asked for, but the final-copy line was not found in the "
                     "native block - REFUSING the debug variant and writing normal SMAA 1x instead.");
                g_nativeAaEdges = false;
            } else {
                g_nativeAaBlockText.replace(at, strlen(kNativeAaFinalCopy), kNativeAaEdgesCopy);
            }
        }
        PLOG("[PAKGEN] native AA variant: %s (first line of aa_native.txt reads \"%.12s\")",
             g_nativeAaEdges ? "*** EDGE-MASK DEBUG ***" : "normal SMAA 1x", nbuf);
        const bool dlssAsked = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
        PLOG("[PAKGEN] NATIVE AA - %s is present. Writing the NATIVE-resolution pak (divisor 1) WITH "
             "the engine's SMAA 1x passes. %s dlssflat forces DLSS, upscaling and the Halton jitter "
             "off this launch.", nativeMarker,
             dlssAsked ? "dlss_quality.txt is ALSO present and is IGNORED this launch." : "");
        RegenerateByDivisor(0, 1, "native AA - the scene renders at output size, nothing to upscale");
        StartPakLockProbe();
        return;
    }
    PLOG("[PAKGEN] native AA: off (no %s) - any pak written this launch has the SMAA 1x token "
         "EMPTY, byte-identical to before this feature. %s", nativeMarker,
         GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES
             ? ""
             : "dlss_quality.txt is absent too, so NOTHING is written and Data3.pak keeps whatever "
               "the LAST launch wrote - which may be a native-AA pak.");

    // TAA: the native-resolution pak, token empty. After native AA (which wins
    // and has already returned), before any DLSS mode. Printed both ways.
    // Since 2026-11-12 the dropdown's "TAA" (aa_method.txt "taa") selects the same branch as the hand marker.
    char taaMarker[MAX_PATH];
    _snprintf_s(taaMarker, sizeof(taaMarker), _TRUNCATE, "%s\\aa_taa.txt", dlvrDir);
    const bool taaByMarker = GetFileAttributesA(taaMarker) != INVALID_FILE_ATTRIBUTES;
    bool taaByMethod = false;
    {
        char mPath[MAX_PATH];
        _snprintf_s(mPath, sizeof(mPath), _TRUNCATE, "%s\\aa_method.txt", dlvrDir);
        bool mPresent = false;
        char mRaw[16] = {};
        taaByMethod = ::dlvr::ReadAaMethod(mPath, &mPresent, mRaw, sizeof(mRaw)) == ::dlvr::kAaTaa;
    }
    g_taaPak = taaByMarker || taaByMethod;
    if (g_taaPak) {
        if (taaByMethod) { g_aaMethodPak = ::dlvr::kAaTaa; }
        PLOG("[PAKGEN] TAA - %s. Writing the NATIVE-resolution pak (divisor 1), SMAA "
             "token empty. %s", taaByMarker ? (taaByMethod ? "aa_taa.txt is present AND aa_method.txt says taa"
                                                           : "aa_taa.txt is present (hand marker)")
                                            : "aa_method.txt says taa (the dropdown)",
             GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES
                 ? "dlss_quality.txt is ALSO present and is IGNORED this launch." : "");
        RegenerateByDivisor(0, 1, "TAA - the scene renders at output size, nothing to upscale");
        StartPakLockProbe();
        return;
    }
    PLOG("[PAKGEN] TAA: off (no %s, and aa_method.txt does not say taa)", taaMarker);

    // THE METHOD - the Anti-Aliasing dropdown's one switch (aamethod.h). After the
    // two hand-only markers above, which win and have already returned. Printed
    // every launch with what it read, so "NO AA by default" and "NO AA because the
    // file says something odd" never look alike.
    char methodMarker[MAX_PATH];
    _snprintf_s(methodMarker, sizeof(methodMarker), _TRUNCATE, "%s\\aa_method.txt", dlvrDir);
    bool methodPresent = false;
    char methodRaw[16] = {};
    g_aaMethodPak = ::dlvr::ReadAaMethod(methodMarker, &methodPresent, methodRaw, sizeof(methodRaw));
    g_noAaPak = (g_aaMethodPak == ::dlvr::kAaNone);
    g_fsrPak  = (g_aaMethodPak == ::dlvr::kAaFsr3 || g_aaMethodPak == ::dlvr::kAaFsr4);
    PLOG("[PAKGEN] AA method: %s (%s)", ::dlvr::AaMethodName(g_aaMethodPak),
         !methodPresent ? "aa_method.txt ABSENT - NO AA is the default"
         : (g_noAaPak && _stricmp(methodRaw, "none") != 0)
             ? "*** aa_method.txt holds a word that is not none/dlss/fsr3/fsr4/taa - treated as NO AA ***"
             : "from aa_method.txt");
    if (g_noAaPak) {
        PLOG("[PAKGEN] NO AA - writing the NATIVE-resolution pak (divisor 1), SMAA token empty. %s",
             GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES
                 ? "dlss_quality.txt is present and IGNORED under NO AA." : "");
        RegenerateByDivisor(0, 1, "NO AA - the scene renders at output size, nothing to upscale");
        StartPakLockProbe();
        return;
    }

    char mbuf[64] = {};
    FILE* mf = nullptr;
    const bool haveQuality = (fopen_s(&mf, marker, "rb") == 0 && mf);
    if (haveQuality) {
        fread(mbuf, 1, sizeof(mbuf) - 1, mf);
        fclose(mf);
    }

    int mode = -1;
    const bool modeOk = haveQuality && sscanf_s(mbuf, "%d", &mode) == 1 && mode >= 0 && mode < kModeCount;
    if (g_fsrPak) {
        // FSR's modes are 0-3 (NATIVE, QUALITY, BALANCED, PERFORMANCE). No
        // quality file or an unreadable one = NATIVE, and said; Ultra Performance
        // (4) is REFUSED for FSR and runs PERFORMANCE, and said.
        if (!modeOk) {
            PLOG("[PAKGEN] FSR: %s - running FSR NATIVE.", haveQuality
                 ? "*** dlss_quality.txt does not hold 0-4 ***" : "no dlss_quality.txt");
            mode = 0;
        } else if (mode == 4) {
            PLOG("[PAKGEN] FSR: *** dlss_quality.txt asks for Ultra Performance, which is NOT offered for "
                 "FSR *** - writing FSR PERFORMANCE instead.");
            mode = 3;
        }
        PLOG("[PAKGEN] FSR mode %d = %s", mode, kFsrModeNames[mode]);
        if (mode == 0) {
            g_requestedMode = 0;
            RegenerateByDivisor(0, 1, "FSR NATIVE - the scene renders at output size, FSR anti-aliases it");
            StartPakLockProbe();
            return;
        }
    } else {
        // DLSS: exactly as before this switch existed.
        if (!haveQuality) { return; }   // absent = do nothing at all
        if (!modeOk) {
            PLOG("[PAKGEN] dlvr\\dlss_quality.txt reads \"%.16s\", which is not 0-%d "
                 "(0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance). "
                 "ANNOUNCED, NOT CLAMPED - the pak is left exactly as it is.",
                 mbuf, kModeCount - 1);
            return;
        }
    }

    // >>> MODE ROUTING. <<< Three modes are divisors and need nothing; two need
    // a resolution, and the only trustworthy source at startup is what the
    // layer recorded last launch.
    g_requestedMode = mode;
    const int d = DivisorForMode(mode);
    if (d > 0) {
        RegenerateByDivisor(mode, d, "this mode is an exact integer divisor");
        StartPakLockProbe();
        return;
    }

    int ow = 0, oh = 0;
    const char* src = "";

    // 1. AN EXPLICIT OVERRIDE WINS. It is the flatscreen developer control -
    //    nothing at DLL_PROCESS_ATTACH can tell a flat launch from a VR one, and
    //    dlss_vr.txt is a marker file rather than a detector (it is present in
    //    flat too; keying on it has already cost a run).
    if (const char* nl = strchr(mbuf, 10)) {
        int xw = 0, xh = 0;
        if (sscanf_s(nl + 1, "%d %d", &xw, &xh) == 2 &&
            xw >= 320 && xh >= 240 && xw <= 16384 && xh <= 16384) {
            ow = xw; oh = xh;
            src = "line 2 of dlss_quality.txt (explicit override)";
        }
    }
    // 1b. early_eye_size: with the marker, THIS launch's size is fetched when the engine opens dlvr_pp.mth (see the
    //     declaration). The routing below still runs exactly as before - it decides what the attach file holds if the
    //     same-launch query cannot run. An explicit override wins and turns it off; a legacy pak has no .mth to rewrite.
    {
        // DEFAULT ON since 2026-10-17 (Jay verified runs 2-4: "Yes, looked fine" / "Thats worked" / "FSR was fine"; "Make it the
        // default"). dlvr\early_eye_size_off.txt turns it off; the old opt-in marker early_eye_size.txt is no longer read.
        char em[MAX_PATH];
        _snprintf_s(em, sizeof(em), _TRUNCATE, "%s\\early_eye_size_off.txt", g_dlvrDir);
        const bool offMarker = GetFileAttributesA(em) != INVALID_FILE_ATTRIBUTES;
        g_earlyWanted = !offMarker && !ow && g_ppLayout != 0;
        g_earlyMode = mode;
        // [PROBE: PAKGEN] gate=none (default ON; dlvr\early_eye_size_off.txt = off)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] same-launch eye size (early_eye_size): %s", offMarker ? "OFF (dlvr\\early_eye_size_off.txt present) - "
             "sized from eye_res.txt as before." : ow ? "line 2 of dlss_quality.txt overrides the size - NOT used."
             : g_ppLayout == 0 ? "this is a LEGACY Data3 (no dlvr_pp.mth) - NOT used."
             : "ON - the size below is provisional; the engine's first open of dlvr_pp.mth fetches this launch's size.");
    }
    // 2. OTHERWISE THE LAST VR LAUNCH'S REAL PER-EYE SIZE.
    if (!ow && ReadEyeResCache(&ow, &oh)) {
        src = "dlvr/eye_res.txt (the per-eye size the layer measured last launch)";
        g_earlyBuiltW = ow; g_earlyBuiltH = oh;
    }
    // 3. NO TRUSTWORTHY SIZE -> FALL BACK TO A DIVISOR RATHER THAN GUESS.
    //
    //    The desktop size is NOT a safe guess here: in VR it produces a scene
    //    that does not match the eye buffers at all, the ratio leaves the
    //    detector's band, and upscaling does not engage - a pixelated stretch
    //    with nothing on screen to explain it. That is the exact failure a user
    //    hits on their FIRST VR launch, which is the worst possible moment.
    //
    //    Divisor 2 is Performance, exactly, at any resolution. Losing the
    //    requested mode for one launch is a real cost and is stated plainly;
    //    losing upscaling entirely while looking broken is not acceptable.
    if (!ow) {
        PLOG("[PAKGEN] mode %d = %s needs an explicit scene size and no trustworthy output "
             "resolution is available yet (no override, no dlvr/eye_res.txt). FALLING BACK TO "
             "DIVISOR 2 = Performance for this launch - it is exact at any resolution. The layer "
             "records the real per-eye size during this run, so the next launch gets %s.",
             mode, kModes[mode].name, kModes[mode].name);
        RegenerateByDivisor(3, 2, "fallback - no trustworthy resolution yet");
        StartPakLockProbe();
        return;
    }

    RegenerateFor(ow, oh, mode, src);
    StartPakLockProbe();
}

// The body, callable again later with a size nobody could know at startup.
bool RegenerateFor(int ow, int oh, int mode, const char* src)
{
    const double r  = kModes[mode].linear;
    const int    w1 = Even(ow / r);
    const int    h1 = Even(oh / r);
    const char*  nm = (g_fsrPak && mode >= 0 && mode <= 3) ? kFsrModeNames[mode] : kModes[mode].name;
    PLOG("[PAKGEN] mode %d = %s (%.2fx). Output %dx%d from %s -> scene %dx%d, so the measured "
         "ratio should come back %.3fx.",
         mode, nm, r, ow, oh, src, w1, h1, ow / static_cast<double>(w1));

    std::string t = Template();
    if (t.size() != kRenderLoopTmplRawBytes) {
        PLOG("[PAKGEN] embedded template reassembled to %zu bytes, expected %zu - the split "
             "literals did not join. Refusing to write.", t.size(), kRenderLoopTmplRawBytes);
        return false;
    }

    // INV-521 (c) import test: with dlvr\rl_import_test.txt the tokens become NAMES and the numbers go to DW\ (see above).
    int tw = 0, th = 0;
    const bool importTest = RlImportTestWanted(w1, h1, &tw, &th) && WriteRlImportFile(tw, th);
    char num[32];
    if (importTest) {
        ReplaceAll(t, "@W1@", "DLVR_W1");
        ReplaceAll(t, "@H1@", "DLVR_H1");
        for (int lvl = 2; lvl <= 16; lvl *= 2) {
            char tokW[8], tokH[8], nameW[16], nameH[16];
            _snprintf_s(tokW, sizeof(tokW), _TRUNCATE, "@W%d@", lvl);
            _snprintf_s(tokH, sizeof(tokH), _TRUNCATE, "@H%d@", lvl);
            _snprintf_s(nameW, sizeof(nameW), _TRUNCATE, "DLVR_W%d", lvl);
            _snprintf_s(nameH, sizeof(nameH), _TRUNCATE, "DLVR_H%d", lvl);
            ReplaceAll(t, tokW, nameW);
            ReplaceAll(t, tokH, nameH);
        }
        t.insert(0, std::string("import \"") + kRlImportName + "\"\r\n");
        // [PROBE: PAKGEN] gate=rl_import_test.txt  site=FLAT  log-only=yes
        PLOG("[PAKGEN] import test ON: render_loop.ppfx sizes written as NAMES DLVR_W1..DLVR_H16 + import \"%s\" at the top "
             "(no size numbers in the pak). This launch's own scene would be %dx%d; the .mth exports %dx%d.",
             kRlImportName, w1, h1, tw, th);
    } else {
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", w1); ReplaceAll(t, "@W1@", num);
        _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", h1); ReplaceAll(t, "@H1@", num);
        for (int lvl = 2; lvl <= 16; lvl *= 2) {
            char tokW[8], tokH[8];
            _snprintf_s(tokW, sizeof(tokW), _TRUNCATE, "@W%d@", lvl);
            _snprintf_s(tokH, sizeof(tokH), _TRUNCATE, "@H%d@", lvl);
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", DivRound(w1, lvl)); ReplaceAll(t, tokW, num);
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", DivRound(h1, lvl)); ReplaceAll(t, tokH, num);
        }
    }
    ReplaceAll(t, "@AA_NATIVE@", g_nativeAaPak ? g_nativeAaBlockText : std::string());
    SunFxStep(t);
    DustFxStep(t);
    EngineJitterStep(t);
    if (t.find('@') != std::string::npos) {
        PLOG("[PAKGEN] a token survived substitution - refusing to write a half-filled template.");
        return false;
    }
    // fixed Data3: the same decision as values (design doc §3.2). Divisors 0 on the explicit-size path.
    PpSetExplicitSizes(w1, h1);

    char pak[MAX_PATH];
    _snprintf_s(pak, sizeof(pak), _TRUNCATE, "%s\\DW\\Data3.pak", g_gameDir);
    if (!PatchPak(pak, t)) { return false; }
    g_wroteW = ow; g_wroteH = oh;
    g_activeMode = mode; g_activeDiv = 0; g_activeW = w1; g_activeH = h1;
    PLOG("[PAKGEN] *** %s READY *** scene %dx%d for a %dx%d output. Cross-check against the "
         "[DLSSFLAT] RT0 size and the [DLSSUP] measured ratio later in this log - if they "
         "disagree, the engine opened the pak before this ran.",
         nm, w1, h1, ow, oh);
    return true;
}

// ---------------------------------------------------------------------------
// CALLED BY THE LAYER FROM ovr_GetFovTextureSize, WITH THE REAL PER-EYE SIZE.
//
// >>> IT DOES NOT REWRITE THE PAK, BECAUSE THAT IS IMPOSSIBLE. <<< Measured two
// independent ways: the engine takes Data3.pak within ~2 s of process start and
// holds it for the whole session (fopen "r+b" fails at T+2 s and again at
// T+30 s), and holding a write handle from BEFORE its open makes the engine's
// own open fail, so it silently falls back to Data0.pak's stock declarations.
// Writing needs a handle; holding one stops the engine reading the file.
//
// So this records the truth for the NEXT launch, and - just as important - says
// out loud whether THIS launch is running on the right size. Without that, a
// user who changes their Virtual Desktop resolution gets a pixelated stretch
// and no explanation anywhere.
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) int DlvrRegenPakForOutput(int outW, int outH)
{
    if (!g_dlvrDir[0] || !g_gameDir[0]) { return 0; }   // the startup pass never ran
    if (outW < 320 || outH < 240 || outW > 16384 || outH > 16384) {
        PLOG("[PAKGEN] runtime per-eye size %dx%d is out of range - ignored.", outW, outH);
        return 0;
    }

    // early_eye_size v2: remember it for the engine's open of dlvr_pp.mth (INV-592 - the open comes after this call).
    InterlockedExchange(&g_layerH, outH);
    InterlockedExchange(&g_layerW, outW);

    int cw = 0, ch = 0;
    const bool hadCache = ReadEyeResCache(&cw, &ch);
    WriteEyeResCache(outW, outH);

    if (!hadCache) {
        PLOG("[PAKGEN] recorded %dx%d per eye as dlvr/eye_res.txt - there was no cache before, so "
             "this launch used a resolution-independent divisor and the NEXT launch can use the "
             "mode you actually asked for.", outW, outH);
        return 1;
    }
    if (cw == outW && ch == outH) {
        PLOG("[PAKGEN] the runtime reports %dx%d per eye, which is what this launch was built "
             "for. Nothing to correct.", outW, outH);
        return 1;
    }

    // early_eye_size v2: the hook is armed and the engine has not opened dlvr_pp.mth yet -> the rewrite is still to come.
    if (g_earlyWanted && g_earlyMthLen && !g_earlyDoneFlag) {
        // [PROBE: PAKGEN] gate=early_eye_size (default ON; dlvr\early_eye_size_off.txt = off)  site=HEADSET  log-only=no
        PLOG("[PAKGEN] early_eye_size: the runtime reports %dx%d per eye and dlvr_pp.mth was built for %dx%d - PENDING: the "
             "engine has not opened it yet, so it is rewritten for %dx%d at that open (see the SAME-LAUNCH line).",
             outW, outH, cw, ch, outW, outH);
        return 1;
    }
    // >>> THE ONE CASE A CACHE GETS WRONG, AND IT MUST NOT BE SILENT. <<<
    PLOG("[PAKGEN] *** RESOLUTION CHANGED - THIS LAUNCH IS RUNNING ON THE OLD ONE *** the pak was "
         "built for %dx%d per eye and the runtime actually reports %dx%d. Data3.pak cannot be "
         "rewritten while the engine holds it, so DLSS upscaling will be wrong or absent for THIS "
         "session only. %dx%d is now recorded; RESTART and it will be correct.",
         cw, ch, outW, outH, outW, outH);
    return 0;
}

// ---------------------------------------------------------------------------
// SAME-LAUNCH EYE SIZE (early_eye_size) - the hook and the in-place rewrite. Why and when: the [DEP: early_eye_size]
// declaration at the top. Runs at most ONCE, on the thread of the engine's first open of dlvr_pp.mth; every other open is a
// pass-through after one flag load (cost: a leaf-name compare per file open until then, then nothing).
// ---------------------------------------------------------------------------
namespace {

using PFN_FileOpen  = bool (*)(void* self, const char* name, int mode, int flags);   // fs::file / fs::cachedrdfile ::open
using PFN_FsOpen    = void* (*)(const char* name, int mode, int flags);               // the free fs::open (INV-518)
using PFN_EarlySize = int (*)(int* w, int* h, char* why, int whyBytes);               // the layer's DlvrQueryEyeSizeEarly

const char* const kImpFileOpen   = "?open@file@fs@@UEAA_NPEBDW4TYPE@EFSMode@@W4FLAGS@FFSOpenFlags@@@Z";
const char* const kImpCachedOpen = "?open@cachedrdfile@fs@@UEAA_NPEBDW4TYPE@EFSMode@@W4FLAGS@FFSOpenFlags@@@Z";
const char* const kImpFsOpen     = "?open@fs@@YAPEAUSFsFile@@PEBDW4TYPE@EFSMode@@W4FLAGS@FFSOpenFlags@@@Z";

PFN_FileOpen  g_realFileOpen = nullptr;
PFN_FileOpen  g_realCachedOpen = nullptr;
PFN_FsOpen    g_realFsOpen = nullptr;
volatile LONG g_earlyDone = 0;                 // 1 once the rewrite ran or refused - later opens pass straight through
INIT_ONCE     g_earlyOnce = INIT_ONCE_STATIC_INIT;

bool IsPpMthName(const char* name)
{
    if (!name) { return false; }
    const char* leaf = name;
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || *p == '\\') { leaf = p + 1; }
    }
    return _stricmp(leaf, kPpMthName) == 0;
}

// The runtime call, isolated so a fault inside the runtime cannot take the game down with it (no C++ objects here: SEH).
int CallEarlySize(PFN_EarlySize fn, int* w, int* h, char* why, int whyBytes, DWORD* code)
{
    __try {
        return fn(w, h, why, whyBytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return -1;
    }
}

void EarlyRewrite(const char* via, const char* name)
{
    char built[64];
    if (g_earlyBuiltW) {
        _snprintf_s(built, sizeof(built), _TRUNCATE, "eye_res.txt's %dx%d", g_earlyBuiltW, g_earlyBuiltH);
    } else {
        _snprintf_s(built, sizeof(built), _TRUNCATE, "the divisor-2 fallback (no eye_res.txt)");
    }
    // [PROBE: PAKGEN] gate=early_eye_size (default ON; dlvr\early_eye_size_off.txt = off)  site=HEADSET  log-only=no
    PLOG("[PAKGEN] early_eye_size: the engine is opening '%s' (via %s, thread %lu) - asking the layer for THIS launch's "
         "per-eye size. The attach file holds %s.", name, via, GetCurrentThreadId(), built);

    int w = 0, h = 0;
    int ok = 0;
    double ms = 0.0;
    const char* source = "";
    char why[256] = {};
    DWORD code = 0;
    // 1. v2: the size the layer already handed DlvrRegenPakForOutput this launch (INV-592: it arrives ~0.9 s before this open).
    const LONG lw = g_layerW, lh = g_layerH;
    if (lw > 0 && lh > 0) {
        w = static_cast<int>(lw);
        h = static_cast<int>(lh);
        ok = 1;
        source = "the layer's ovr_GetFovTextureSize (handed over earlier this launch)";
    } else {
        // 2. none yet (the open came before ovr_Create): the layer's short-lived instance.
        source = "the layer's short-lived instance (no size handed over yet)";
        HMODULE layer = GetModuleHandleA("LibOVRRT64_1.dll");
        PFN_EarlySize fn = layer ? reinterpret_cast<PFN_EarlySize>(
                                       reinterpret_cast<void*>(GetProcAddress(layer, "DlvrQueryEyeSizeEarly")))
                                 : nullptr;
        if (!fn) {
            PLOG("[PAKGEN] early_eye_size: *** NOT AVAILABLE *** (no size handed over yet, and %s) - this launch keeps %s.",
                 layer ? "the layer has no DlvrQueryEyeSizeEarly export - an older LibOVRRT64_1.dll"
                       : "LibOVRRT64_1.dll is not loaded", built);
            return;
        }
        LARGE_INTEGER qf{}, q0{}, q1{};
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        ok = CallEarlySize(fn, &w, &h, why, static_cast<int>(sizeof(why)), &code);
        QueryPerformanceCounter(&q1);
        ms = qf.QuadPart ? (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart : -1.0;
    }
    if (ok < 0) {
        PLOG("[PAKGEN] early_eye_size: *** THE RUNTIME QUERY FAULTED *** (exception 0x%08lX after %.1f ms) - this launch keeps "
             "%s.", code, ms, built);
        return;
    }
    if (!ok || w < 320 || h < 240 || w > 16384 || h > 16384) {
        PLOG("[PAKGEN] early_eye_size: *** NOT AVAILABLE *** after %.1f ms (%s; size %dx%d) - this launch keeps %s.", ms,
             why[0] ? why : "no reason given", w, h, built);
        return;
    }
    if (w == g_earlyBuiltW && h == g_earlyBuiltH) {
        PLOG("[PAKGEN] early_eye_size: %dx%d per eye from %s (query %.1f ms) is what the attach file was built for - nothing "
             "rewritten.", w, h, source, ms);
        return;
    }

    const int    mode = g_earlyMode;
    const double r    = kModes[mode].linear;
    const int    w1   = Even(w / r);
    const int    h1   = Even(h / r);
    const char*  nm   = (g_fsrPak && mode >= 0 && mode <= 3) ? kFsrModeNames[mode] : kModes[mode].name;
    int saved[kPpNameCount];
    memcpy(saved, g_ppVal, sizeof(saved));
    PpSetExplicitSizes(w1, h1);
    std::string body = PpMthBody();
    const size_t head = strlen(kEarlyReserveHead) + 2;   // the reserve line's text + CRLF, with no padding
    if (body.size() + head > g_earlyMthLen) {
        memcpy(g_ppVal, saved, sizeof(saved));
        PLOG("[PAKGEN] early_eye_size: *** NOT REWRITTEN *** the new values need %zu bytes and the attach file is %zu - "
             "this launch keeps %s.", body.size() + head, g_earlyMthLen, built);
        return;
    }
    std::string pad = kEarlyReserveHead;
    pad.append(g_earlyMthLen - body.size() - head, ' ');
    pad += "\r\n";
    body += pad;

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\DW\\%s", g_gameDir, kPpMthName);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r+b") != 0 || !f) {
        memcpy(g_ppVal, saved, sizeof(saved));
        PLOG("[PAKGEN] early_eye_size: *** NOT REWRITTEN *** cannot open %s for update (errno-style failure, GetLastError %lu) "
             "- this launch keeps %s.", path, GetLastError(), built);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    if (len < 0 || static_cast<size_t>(len) != g_earlyMthLen) {
        fclose(f);
        memcpy(g_ppVal, saved, sizeof(saved));
        PLOG("[PAKGEN] early_eye_size: *** NOT REWRITTEN *** %s is %ld bytes, the attach write was %zu - something else "
             "changed it; this launch keeps whatever it holds.", path, len, g_earlyMthLen);
        return;
    }
    fseek(f, 0, SEEK_SET);
    const size_t wrote = fwrite(body.data(), 1, body.size(), f);
    fflush(f);
    fclose(f);
    std::string back;
    FILE* rf = nullptr;
    if (fopen_s(&rf, path, "rb") == 0 && rf) {
        char rb[4096];
        size_t n = 0;
        while ((n = fread(rb, 1, sizeof(rb), rf)) > 0) { back.append(rb, n); }
        fclose(rf);
    }
    const bool same = (wrote == body.size()) && back == body;
    if (same) {
        g_wroteW = w; g_wroteH = h;
        g_activeMode = mode; g_activeDiv = 0; g_activeW = w1; g_activeH = h1;
        WriteEyeResCache(w, h);   // so the layer's later check compares against what THIS launch was built for
    }
    PLOG("[PAKGEN] %s dlvr_pp.mth rewritten IN PLACE (%zu of %zu bytes, read back %s): mode %d = %s, per-eye %dx%d from %s "
         "(query %.1f ms; the attach file held %s) -> scene %dx%d, so the measured ratio should come back %.3fx. "
         "Cross-check [DLSSFLAT] RT0 = %dx%d and the [DLSSUP] ratio.",
         same ? "*** SAME-LAUNCH SIZE WRITTEN ***" : "*** SAME-LAUNCH REWRITE FAILED - THE FILE MAY BE HALF-WRITTEN ***",
         wrote, body.size(), back == body ? "identical" : "DIFFERENT", mode, nm, w, h, source, ms, built, w1, h1,
         w / static_cast<double>(w1), w1, h1);
}

BOOL CALLBACK EarlyOnceFn(PINIT_ONCE, PVOID param, PVOID*)
{
    const char* const* a = static_cast<const char* const*>(param);
    EarlyRewrite(a[0], a[1]);
    InterlockedExchange(&g_earlyDoneFlag, 1);
    InterlockedExchange(&g_earlyDone, 1);
    return TRUE;
}

// v2 open log (INV-592 NOT-established: is render_loop.ppfx read before the first dlvr_pp.mth open, through these slots?):
// the first kOpenLogMax opens of a .ppfx / .mth name, with the slot and thread. After that: one load + compare per open.
const LONG kOpenLogMax = 12;
volatile LONG g_openLogged = 0;

void NoteOpen(const char* via, const char* name)
{
    if (g_openLogged >= kOpenLogMax || !name) { return; }
    const size_t n = strlen(name);
    const bool ppfx = n >= 5 && _stricmp(name + n - 5, ".ppfx") == 0;
    const bool mth = n >= 4 && _stricmp(name + n - 4, ".mth") == 0;
    if (!ppfx && !mth) { return; }
    const LONG k = InterlockedIncrement(&g_openLogged);
    if (k > kOpenLogMax) { return; }
    // [PROBE: PAKGEN] gate=early_eye_size (default ON; dlvr\early_eye_size_off.txt = off)  site=FLAT  log-only=yes
    PLOG("[PAKGEN] early_eye_size open #%ld of %ld: '%s' via %s, thread %lu%s", k, kOpenLogMax, name, via, GetCurrentThreadId(),
         k == kOpenLogMax ? " (last one logged)" : "");
}

// grapple_unlock_skill (2026-11-28) shares these hooks: they are installed when either wants them, and g_earlyActive says whether
// early_eye_size does (off = its open log and rewrite never run - exactly as before when the hooks were not installed).
volatile LONG g_earlyActive = 0;

void EarlyCheck(const char* via, const char* name)
{
    if (!g_earlyActive) { return; }
    NoteOpen(via, name);
    if (g_earlyDone || !IsPpMthName(name)) { return; }
    const char* args[2] = { via, name };
    InitOnceExecuteOnce(&g_earlyOnce, EarlyOnceFn, const_cast<char**>(args), nullptr);   // a second opener waits for it
}

}  // namespace
// skillpatch.cpp: the name to open instead (nullptr = as asked), and the fallback's log.
const char* SkillPatchRedirect(const char* name, const char* via);
void SkillPatchRedirectFailed(const char* name, const char* via);
bool SkillPatchWanted();
namespace {

bool HookFileOpen(void* self, const char* name, int mode, int flags)
{
    EarlyCheck("fs::file::open", name);
    if (const char* alt = SkillPatchRedirect(name, "fs::file::open")) {
        if (g_realFileOpen(self, alt, mode, flags)) { return true; }
        SkillPatchRedirectFailed(name, "fs::file::open");
    }
    return g_realFileOpen(self, name, mode, flags);
}

bool HookCachedOpen(void* self, const char* name, int mode, int flags)
{
    EarlyCheck("fs::cachedrdfile::open", name);
    if (const char* alt = SkillPatchRedirect(name, "fs::cachedrdfile::open")) {
        if (g_realCachedOpen(self, alt, mode, flags)) { return true; }
        SkillPatchRedirectFailed(name, "fs::cachedrdfile::open");
    }
    return g_realCachedOpen(self, name, mode, flags);
}

void* HookFsOpen(const char* name, int mode, int flags)
{
    EarlyCheck("fs::open", name);
    if (const char* alt = SkillPatchRedirect(name, "fs::open")) {
        if (void* f = g_realFsOpen(alt, mode, flags)) { return f; }
        SkillPatchRedirectFailed(name, "fs::open");
    }
    return g_realFsOpen(name, mode, flags);
}

}  // namespace

// Called from DllMain right after GenerateRenderLoopPak. Installs nothing unless the routing wanted it AND the attach write
// completed (its length is what the rewrite must match).
void InstallEarlyEyeSizeHook()
{
    // grapple_unlock_skill (skillpatch.cpp) needs the same open hooks: installed for either; early_eye_size's own work runs only
    // when it is wanted AND its attach write completed (g_earlyActive).
    const bool skill = SkillPatchWanted();
    if (!g_earlyWanted && !skill) { return; }   // the reason is the routing's "same-launch eye size" line
    if (g_earlyWanted && !g_earlyMthLen) {
        // [PROBE: PAKGEN] gate=early_eye_size (default ON; dlvr\early_eye_size_off.txt = off)  site=FLAT  log-only=yes
        PLOG("[PAKGEN] early_eye_size: *** hook NOT installed *** - the attach write of dlvr_pp.mth did not complete (see its "
             "line above), so there is no file of known length to rewrite. This launch keeps the attach decision.%s",
             skill ? " (The hooks still go in for grapple_unlock_skill.)" : "");
        if (!skill) { return; }
    }
    InterlockedExchange(&g_earlyActive, g_earlyWanted && g_earlyMthLen ? 1 : 0);
    HMODULE fsd = GetModuleHandleA("filesystem_x64_rwdi.dll");
    if (fsd) {
        g_realFileOpen   = reinterpret_cast<PFN_FileOpen>(reinterpret_cast<void*>(GetProcAddress(fsd, kImpFileOpen)));
        g_realCachedOpen = reinterpret_cast<PFN_FileOpen>(reinterpret_cast<void*>(GetProcAddress(fsd, kImpCachedOpen)));
        g_realFsOpen     = reinterpret_cast<PFN_FsOpen>(reinterpret_cast<void*>(GetProcAddress(fsd, kImpFsOpen)));
    }
    if (!g_realFileOpen || !g_realCachedOpen || !g_realFsOpen) {
        PLOG("[PAKGEN] early_eye_size: *** hook NOT installed *** - filesystem_x64_rwdi.dll %s (file::open %p, "
             "cachedrdfile::open %p, fs::open %p). This launch keeps the attach decision.",
             fsd ? "is missing an export" : "is not mapped", reinterpret_cast<void*>(g_realFileOpen),
             reinterpret_cast<void*>(g_realCachedOpen), reinterpret_cast<void*>(g_realFsOpen));
        return;
    }
    const char* mods[] = { "engine_x64_rwdi.dll", "gamedll_x64_rwdi.dll", "imagelib_x64_rwdi.dll", "DyingLightGame.exe" };
    struct { const char* imp; void* hook; const char* label; } hooks[] = {
        { kImpFileOpen,   reinterpret_cast<void*>(&HookFileOpen),   "file::open" },
        { kImpCachedOpen, reinterpret_cast<void*>(&HookCachedOpen), "cachedrdfile::open" },
        { kImpFsOpen,     reinterpret_cast<void*>(&HookFsOpen),     "fs::open" },
    };
    int total = 0;
    std::string where;
    for (const char* m : mods) {
        HMODULE h = GetModuleHandleA(m);
        if (!h) {
            where += std::string(" ") + m + "(not mapped)";
            continue;
        }
        std::string got;
        for (auto& k : hooks) {
            if (::dlvr::PatchIat(h, "filesystem_x64_rwdi.dll", k.imp, k.hook)) {
                got += got.empty() ? k.label : std::string("+") + k.label;
                ++total;
            }
        }
        where += std::string(" ") + m + "(" + (got.empty() ? "none" : got) + ")";
    }
    // [PROBE: PAKGEN] gate=early_eye_size (default ON; dlvr\early_eye_size_off.txt = off)  site=FLAT  log-only=yes
    PLOG("[PAKGEN] early_eye_size: hook %s - %d IAT slot(s):%s. %s%s", total ? "INSTALLED" : "*** NOT INSTALLED (no slot found) ***",
         total, where.c_str(), g_earlyActive ? "The first open of dlvr_pp.mth fetches this launch's size." :
         "early_eye_size itself is OFF this launch (the hooks pass its file through).",
         skill ? " grapple_unlock_skill's redirect of common_skills.xml rides on them." : "");
}

// v2: rd3d11_x64_rwdi.dll is loaded after attach, so its own fs::file::open import (INV-518) was the one slot run 1 did not cover.
// Called from the bootstrap's rd3d11 watchdog the moment the module appears. Nothing unless the attach install succeeded.
void InstallEarlyEyeSizeHookOn(HMODULE rd)
{
    if (!g_earlyWanted || !g_earlyMthLen || !g_realFileOpen || !rd) { return; }
    struct { const char* imp; void* hook; const char* label; } hooks[] = {
        { kImpFileOpen,   reinterpret_cast<void*>(&HookFileOpen),   "file::open" },
        { kImpCachedOpen, reinterpret_cast<void*>(&HookCachedOpen), "cachedrdfile::open" },
        { kImpFsOpen,     reinterpret_cast<void*>(&HookFsOpen),     "fs::open" },
    };
    std::string got;
    for (auto& k : hooks) {
        if (::dlvr::PatchIat(rd, "filesystem_x64_rwdi.dll", k.imp, k.hook)) {
            got += got.empty() ? k.label : std::string("+") + k.label;
        }
    }
    // [PROBE: PAKGEN] gate=early_eye_size (default ON; dlvr\early_eye_size_off.txt = off)  site=FLAT  log-only=yes
    PLOG("[PAKGEN] early_eye_size: rd3d11_x64_rwdi.dll mapped - its slot(s): %s; opens logged so far %ld, first dlvr_pp.mth "
         "open %s.", got.empty() ? "*** none found ***" : got.c_str(), g_openLogged, g_earlyDone ? "ALREADY HAPPENED" : "not yet");
}

// ---------------------------------------------------------------------------
// THE PAK-LOCK PROBE - decides whether a level load re-reads render_loop.ppfx.
//
// WHY IT IS SHAPED LIKE THIS. To find out whether the engine re-parses the
// ppfx, the FILE has to change mid-session - and it cannot: writing needs a
// handle, and the engine's own open refuses to coexist with one (measured, a
// controlled pair: no handle -> our pak is used, handle held -> the engine
// falls back to stock). So "write it and see" is not available.
//
// THE SAME MEASUREMENT SUPPLIES THE INSTRUMENT. Holding the write handle makes
// the engine's NEXT open of Data3.pak fail, and a failed open makes it fall
// back to Data0.pak's stock declarations - which are FULL resolution and
// therefore unmistakable. So: let the engine parse normally at startup, grab
// the handle afterwards, then load a level.
//
//   scene stays at the reduced size -> the ppfx was cached at startup; a level
//                                      load is NOT a write window
//   scene reverts to FULL resolution -> the engine re-read it, our lock blocked
//                                      it, and a timed mid-session write is
//                                      possible after all
//
// [DLSSFLAT] *** SCENE SIZE CHANGED *** is the discriminator, and it prints
// either way - silence here means the probe never fired, not a negative result,
// which is why the acquisition itself is logged separately.
//
// MARKER: dlvr\pak_lock_test.txt, first line = seconds to wait (default 30).
// It is a DIAGNOSTIC and it DEGRADES THE GAME while held - any pak the engine
// opens late will fail. Delete it after the run.
// ---------------------------------------------------------------------------
namespace {

DWORD WINAPI PakLockThread(LPVOID param)
{
    const int waitSec = static_cast<int>(reinterpret_cast<intptr_t>(param));
    Sleep(static_cast<DWORD>(waitSec) * 1000);
    char pak[MAX_PATH];
    _snprintf_s(pak, sizeof(pak), _TRUNCATE, "%s/DW/Data3.pak", g_gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, pak, "r+b") == 0 && f) {
        PLOG("[PAKLOCK] *** HANDLE ACQUIRED *** after %d s - Data3.pak is now held for WRITE and "
             "the engine cannot open it again. Load a level now. If the scene reverts to full "
             "resolution, the engine re-reads render_loop.ppfx on a load; if it keeps the reduced "
             "size, the declarations were cached at startup.", waitSec);
        // Deliberately never closed - the whole point is to hold it.
    } else {
        PLOG("[PAKLOCK] could not acquire the handle after %d s - the engine already holds it for "
             "the session, so this probe CANNOT ANSWER THE QUESTION. Not a negative result.",
             waitSec);
    }
    return 0;
}

}  // namespace

void StartPakLockProbe()
{
    char marker[MAX_PATH];
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s/pak_lock_test.txt", g_dlvrDir);
    // >>> A PROBE THAT REFUSES MUST SAY SO, AND NAME THE PATH IT TRIED. <<<
    // v1 returned in silence here, so "the marker is absent" and "the path I
    // built is wrong" produced identical output - nothing. The path WAS wrong
    // (a heredoc had eaten a backslash), the probe never armed, and the run
    // answered nothing while looking exactly like a run that had answered.
    //
    // Printing the path is the part that matters: it turns a silent no-op into
    // a line that names its own cause. This project already requires that a
    // refusing probe name the setting that would fix it.
    FILE* mf = nullptr;
    if (fopen_s(&mf, marker, "rb") != 0 || !mf) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            PLOG("[PAKLOCK] not armed - no marker at %s. Create it holding a number of seconds "
                 "to enable the probe. This is the normal state.", marker);
        }
        return;
    }
    char buf[32] = {};
    fread(buf, 1, sizeof(buf) - 1, mf);
    fclose(mf);
    int sec = 0;
    if (sscanf_s(buf, "%d", &sec) != 1 || sec < 1 || sec > 600) { sec = 30; }
    PLOG("[PAKLOCK] ARMED - will take a WRITE handle on Data3.pak in %d s. This is a DIAGNOSTIC "
         "and it degrades the game while held; delete dlvr\\pak_lock_test.txt afterwards.", sec);
    if (HANDLE t = CreateThread(nullptr, 0, PakLockThread, reinterpret_cast<LPVOID>(
                                    static_cast<intptr_t>(sec)), 0, nullptr)) {
        CloseHandle(t);
    }
}

// ---------------------------------------------------------------------------
// WHAT IS ACTUALLY LIVE THIS SESSION, for the overlay to report.
//
// >>> THE REQUESTED MODE AND THE RUNNING MODE ARE NOT THE SAME THING. <<< Ask
// for Quality with no cached resolution and the launch runs Performance at
// divisor 2 - correct, deliberate, and NOT what the marker file says. A menu
// that reads the marker alone would confidently display "Quality" while the
// game renders Performance, which is the same defect the quality combo already
// had once (a `static int quality = 0` that always displayed DLAA).
//
// So the bootstrap publishes what it actually wrote, and the overlay shows
// both. The marker is the REQUEST; this is the OUTCOME.
// ---------------------------------------------------------------------------
// The HUD layer (dlvr\hud_layer.txt, SpriteHudStep): what this launch's pak holds - 0 = no bracket binds (marker absent,
// or refused), 1 = the brackets run + the bootstrap redirects, 2 = the brackets run, COUNT only. dlssflat's hudlayer::
// acts only when this is non-zero.
extern "C" int DlvrHudLayerPakMode() { return g_hudLayerPak; }

// [DATA3] the whole-pak verdict (fixed_data3_md5.h): 1 = this build's Data3, -1 = another pak, -2 = unreadable, 0 = not checked.
// The layer (camhook.cpp AfwData3Allowed) holds AFW off unless it reads 1.
extern "C" __declspec(dllexport) int DlvrData3State() { return static_cast<int>(g_data3State); }

extern "C" __declspec(dllexport) int DlvrGetActiveQuality(int* requested, int* active,
                                                          int* divisor, int* w, int* h)
{
    if (g_activeMode < 0) { return 0; }   // the startup pass never ran or refused
    if (requested) { *requested = g_requestedMode; }
    if (active)    { *active    = g_activeMode; }
    if (divisor)   { *divisor   = g_activeDiv; }
    if (w)         { *w         = g_activeW; }
    if (h)         { *h         = g_activeH; }
    return 1;
}
