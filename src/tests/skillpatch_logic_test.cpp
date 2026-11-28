// Standalone unit test for src/bootstrap/skillpatch_logic.h - NOT part of the DLL build.
// grapple_unlock_skill (design §8.21): StatusHook loses its rank requirement and its cost, and nothing else in the file changes.
// Exit code = number of failures.
#include "skillpatch_logic.h"

#include <cstdio>
#include <string>

using namespace dlvr::skillpatch;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// The shape of the game's file around the skill (CRLF, tabs), with neighbours that must not change.
static const char* const kXml =
    "<skills>\r\n"
    "\t<skill id=\"StatusShield\" cat=\"status\" tier=\"8\" max_level=\"1\" skill_points=\"1\" skill_points_type=\"Status\">\r\n"
    "\t\t<level_req type=\"Status\" value=\"6\"/>\r\n"
    "\t</skill>\r\n"
    "\t\r\n"
    "\t<skill id=\"StatusHook\" cat=\"status\" tier=\"5\" max_level=\"1\" desc_params=\"x;NODE;M:StatusHook\" skill_points=\"1\" "
    "skill_points_type=\"Status\">\r\n"
    "\t\t<level_req type=\"Status\" value=\"12\"/>\r\n"
    "\t\t<effect id=\"CanUseHook\" change=\"1\"/>\r\n"
    "\t</skill>\r\n"
    "\t<skill id=\"ElementalCrit\" cat=\"status\" tier=\"20\" skill_points=\"1\">\r\n"
    "\t\t<level_req type=\"Status\" value=\"13\"/>\r\n"
    "\t\t<skill_req id=\"StatusHook\"/>\r\n"
    "\t</skill>\r\n"
    "</skills>\r\n";

static void patches_only_the_hook_skill()
{
    std::string out, why, old;
    int removed = -1;
    CHECK(PatchHookSkill(kXml, &out, &why, &removed, &old));
    CHECK(removed == 1);
    CHECK(old == "1");
    const size_t a = out.find(kSkillOpen);
    const size_t b = out.find("</skill>", a);
    const std::string hook = out.substr(a, b - a);
    CHECK(hook.find("skill_points=\"0\"") != std::string::npos);
    CHECK(hook.find("level_req") == std::string::npos);
    CHECK(hook.find("<effect id=\"CanUseHook\" change=\"1\"/>") != std::string::npos);   // the effect stays
    // the neighbours are byte for byte what they were
    CHECK(out.find("<level_req type=\"Status\" value=\"6\"/>") != std::string::npos);
    CHECK(out.find("<level_req type=\"Status\" value=\"13\"/>") != std::string::npos);
    CHECK(out.find("<skill_req id=\"StatusHook\"/>") != std::string::npos);
    // exactly one line (the level_req) and one character ("1" -> "0") changed
    CHECK(std::string(kXml).size() - out.size() == std::string("\t\t<level_req type=\"Status\" value=\"12\"/>\r\n").size());
    // patching the patched file again is harmless (nothing left to remove)
    std::string twice;
    CHECK(PatchHookSkill(out, &twice, &why, &removed, &old));
    CHECK(removed == 0);
    CHECK(old == "0");
    CHECK(twice == out);
}

static void refuses_what_does_not_match()
{
    std::string out = "untouched", why, old;
    int removed = 0;
    CHECK(!PatchHookSkill("<skills></skills>", &out, &why, &removed, &old));
    CHECK(out == "untouched");
    const std::string two = std::string(kXml) + kXml;
    CHECK(!PatchHookSkill(two, &out, &why, &removed, &old));
    CHECK(!PatchHookSkill("<skill id=\"StatusHook\" cat=\"status\"><level_req/></skill>", &out, &why, &removed, &old));   // no cost
    CHECK(!PatchHookSkill("<skill id=\"StatusHook\" skill_points=\"1\">", &out, &why, &removed, &old));                  // no close
    CHECK(out == "untouched");
}

int main()
{
    patches_only_the_hook_skill();
    refuses_what_does_not_match();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
