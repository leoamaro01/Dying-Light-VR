// grapple_unlock_skill (design §8.21; tester 2026-11-28: "add a setting (off by default ...) to have the grappling gun skill from the
// game start"). The grappling hook is the Survivor skill StatusHook in the game's data/skills/common_skills.xml (Data0.pak):
//     <skill id="StatusHook" cat="status" tier="5" max_level="1" desc_params="..." skill_points="1" skill_points_type="Status">
//         <level_req type="Status" value="12"/>
//         <effect id="CanUseHook" change="1"/>
//     </skill>
// Its SkillReward (data/scripts/scouting/skill_add_actions.scr) hands over the hook item (Special_Hook) and its tutorial when the
// skill is taken, so the skill stays the way in: the patch removes its rank requirement(s) and makes it free, nothing else - the
// skill can then be taken in the skill menu at any rank. Pure text, no Windows: tests/skillpatch_logic_test.cpp.
#pragma once

#include <string>

namespace dlvr {
namespace skillpatch {

constexpr const char* kSkillOpen = "<skill id=\"StatusHook\"";

// True = *out is xml with StatusHook's level_req elements removed and its skill_points set to 0 (*removed = how many level_req were
// removed, *oldPoints = the cost it had, as written). False = *why says what did not match (out untouched): no such skill, two of
// them, an unterminated element, no skill_points attribute.
inline bool PatchHookSkill(const std::string& xml, std::string* out, std::string* why, int* removed, std::string* oldPoints)
{
    *removed = 0;
    oldPoints->clear();
    const size_t a = xml.find(kSkillOpen);
    if (a == std::string::npos) { *why = "no StatusHook skill in the file"; return false; }
    if (xml.find(kSkillOpen, a + 1) != std::string::npos) { *why = "StatusHook is defined twice - not patched"; return false; }
    const size_t tagEnd = xml.find('>', a);
    if (tagEnd == std::string::npos) { *why = "StatusHook's opening tag is not closed"; return false; }
    const bool selfClosed = tagEnd > a && xml[tagEnd - 1] == '/';
    const size_t close = selfClosed ? tagEnd + 1 : xml.find("</skill>", tagEnd);
    if (close == std::string::npos) { *why = "StatusHook has no </skill>"; return false; }
    std::string open = xml.substr(a, tagEnd - a + 1);
    const std::string spKey = "skill_points=\"";
    const size_t sp = open.find(spKey);
    if (sp == std::string::npos) { *why = "StatusHook has no skill_points attribute"; return false; }
    const size_t v0 = sp + spKey.size();
    const size_t v1 = open.find('"', v0);
    if (v1 == std::string::npos) { *why = "StatusHook's skill_points is not terminated"; return false; }
    *oldPoints = open.substr(v0, v1 - v0);
    open.replace(v0, v1 - v0, "0");
    std::string body = selfClosed ? std::string() : xml.substr(tagEnd + 1, close - tagEnd - 1);
    for (;;) {
        const size_t lr = body.find("<level_req");
        if (lr == std::string::npos) { break; }
        const size_t lrEnd = body.find("/>", lr);
        if (lrEnd == std::string::npos) { *why = "a level_req in StatusHook is not closed"; return false; }
        size_t from = lr;   // the element's own line: its leading whitespace goes too
        while (from > 0 && (body[from - 1] == ' ' || body[from - 1] == '\t')) { --from; }
        size_t to = lrEnd + 2;
        if (to < body.size() && body[to] == '\r') { ++to; }
        if (to < body.size() && body[to] == '\n') { ++to; }
        body.erase(from, to - from);
        ++*removed;
    }
    *out = xml.substr(0, a) + open + body + xml.substr(close);
    return true;
}

}  // namespace skillpatch
}  // namespace dlvr
