// Body tab group resolution for many rig conventions, using the shipped BoneDictionary.json.
//   test_groups <BoneDictionary.json> [bridge_in files with real skeletons...]
#include "core/body_groups.hpp"
#include "core/morphs.hpp"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
using namespace uuepbs;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static std::string read(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

static std::map<std::string, std::vector<std::string>> show(const char* title, const std::vector<std::string>& names, const std::vector<int32_t>& parents,
                                                             const BoneDictionary& d, const std::vector<UserGroup>& user = {})
{
    BodyMap m = resolve_body_groups(names, parents, d, user);
    std::printf("== %s%s\n", title, m.style.empty() ? "" : (" [" + m.style + "]").c_str());
    std::map<std::string, std::vector<std::string>> out;
    for (auto& g : m.groups)
    {
        std::printf("  %-12s:", g.group.title.c_str());
        size_t k = 0;
        for (auto& b : g.bones) { if (k++ < 8) std::printf(" %s", b.c_str()); }
        if (g.bones.size() > 8) std::printf(" ... (%zu)", g.bones.size());
        std::printf("\n");
        out[g.group.title] = g.bones;
    }
    return out;
}

// names + parents of the primary rig in a bridge_in.txt
static bool load_bridge(const char* path, std::vector<std::string>& names, std::vector<int32_t>& parents)
{
    std::istringstream in(read(path));
    std::string line;
    bool primary = false;
    while (std::getline(in, line))
    {
        if (line.rfind("rig\t", 0) == 0) { std::istringstream l(line); std::string a, b, c, p; std::getline(l, a, '\t'); std::getline(l, b, '\t'); std::getline(l, c, '\t'); std::getline(l, p, '\t'); primary = p == "1"; continue; }
        if (!primary) continue;
        std::istringstream l(line); std::string kind, item; std::getline(l, kind, '\t');
        if (kind == "bones") { names.clear(); while (std::getline(l, item, '\t')) names.push_back(item); }
        if (kind == "parents") { parents.clear(); while (std::getline(l, item, '\t')) parents.push_back(std::stoi(item)); return true; }
    }
    return false;
}

static std::vector<int32_t> flat(size_t n) { return std::vector<int32_t>(n, -1); }
static bool has(const std::map<std::string, std::vector<std::string>>& m, const char* g, const char* bone)
{
    auto it = m.find(g); if (it == m.end()) return false;
    for (auto& b : it->second) if (b == bone) return true;
    return false;
}

int main(int argc, char** argv)
{
    // The built-in copy must be the shipped file.
    BoneDictionary d; std::string msg;
    CHECK(argc > 1 && BoneDictionary::parse(read(argv[1]), d, msg));
    std::printf("dictionary: %s\n", msg.c_str());
    CHECK(BoneDictionary::builtin().groups.size() == d.groups.size() && BoneDictionary::builtin().styles.size() == d.styles.size());

    CHECK(bone_stem("MOT_Thigh_L") == "thigh");
    CHECK(bone_stem("Chest_M") == "chest");
    CHECK(bone_stem("Breast_jnt_L01") == "breastjnt01");
    CHECK(bone_stem("CC_Base_L_Upperarm") == "upperarm");
    CHECK(bone_stem("spine_01.x") == "spine01");
    CHECK(mirror_bone_name("Breast_jnt_L01") == "Breast_jnt_R01");
    CHECK(glob_match("breast*", "breastjnt01") && glob_match("neck#", "neck") && glob_match("neck#", "neck02") && !glob_match("neck#", "neckx"));

    // Roku (GON): must stay exactly as before.
    {
        std::vector<std::string> names; std::vector<int32_t> parents;
        std::ifstream f("roku_ref.txt"); std::string line;
        while (std::getline(f, line)) { std::istringstream in(line); std::string n; int p; in >> n >> p; names.push_back(n); parents.push_back(p); }
        auto m = show("Roku", names, parents, d);
        CHECK(has(m, "Thighs", "thigh_l") && has(m, "Thighs", "thighjiggle_r") && has(m, "Breasts", "boob_l") && has(m, "Hips", "pelvis"));
        CHECK(has(m, "Waist", "spine_01") && has(m, "Rib cage", "spine_02") && has(m, "Upper arms", "upperarm_twist_01_l"));
    }
    for (int a = 2; a < argc; ++a)
    {
        std::vector<std::string> names; std::vector<int32_t> parents;
        if (!load_bridge(argv[a], names, parents)) { std::printf("could not read %s\n", argv[a]); ++fails; continue; }
        auto m = show(argv[a], names, parents, d);
        if (std::string(argv[a]).find("Wuchang") != std::string::npos)
        {
            CHECK(has(m, "Thighs", "Hip_L") && has(m, "Thighs", "SDtui_R") && has(m, "Calves", "Knee_L") && has(m, "Hips", "Root_M"));
            CHECK(has(m, "Upper arms", "Shoulder_L") && has(m, "Upper arms", "SDabi1_L") && has(m, "Shoulders", "Scapula_R") && has(m, "Forearms", "Elbow_R"));
            CHECK(has(m, "Rib cage", "Chest_M") && has(m, "Breasts", "Breast_jnt_L01") && !has(m, "Breasts", "Breast_jnt_L02") && has(m, "Hands", "Wrist_L"));
            // user override: re-point Waist, hide Eyes, add a custom group
            std::vector<UserGroup> user;
            CHECK(parse_user_groups(R"({ "Project": "x", "BodyGroups": { "Waist": ["RootPart2_M"], "Eyes": [], "Weapon hands": { "Bones": ["Weapon_Hand_*"], "Section": "Extras", "Children": "this_bone_only" } } })", user, msg));
            auto u = show("Wuchang + profile BodyGroups", names, parents, d, user);
            CHECK(has(u, "Waist", "RootPart2_M") && !u.count("Eyes") && has(u, "Weapon hands", "Weapon_Hand_L") && has(u, "Weapon hands", "Weapon_Hand_R"));
        }
        if (std::string(argv[a]).find("BoR") != std::string::npos)
        {
            CHECK(has(m, "Thighs", "MOT_Thigh_L") && has(m, "Thighs", "SUP_ThighTwist1_L") && has(m, "Calves", "MOT_Calf_R") && has(m, "Hips", "MOT_Pelvis"));
            CHECK(has(m, "Upper arms", "MOT_Upperarm_L") && has(m, "Forearms", "MOT_Lowerarm_L") && has(m, "Shoulders", "MOT_Clavicle_R") && has(m, "Head", "MOT_Head"));
            CHECK(has(m, "Waist", "MOT_Spine1") && has(m, "Rib cage", "MOT_Spine2") && m.count("Breasts") && has(m, "Feet", "MOT_Foot_L"));
        }
    }
    // Synthetic rigs
    {
        std::vector<std::string> mh = {"pelvis","spine_01","spine_02","spine_03","spine_04","spine_05","neck_01","head","clavicle_l","upperarm_l","upperarm_twist_01_l","upperarm_bicep_l","upperarm_tricep_l","lowerarm_l","hand_l","clavicle_pec_l","spine_04_latissimus_l","thigh_l","thigh_twist_01_l","calf_l","foot_l"};
        auto m = show("MetaHuman", mh, flat(mh.size()), d);
        CHECK(has(m, "Biceps", "upperarm_bicep_l") && has(m, "Triceps", "upperarm_tricep_l") && has(m, "Lats", "spine_04_latissimus_l") && has(m, "Pecs", "clavicle_pec_l"));
        CHECK(has(m, "Upper arms", "upperarm_l") && !has(m, "Upper arms", "upperarm_bicep_l"));
        std::vector<std::string> arp = {"root.x","spine_01.x","spine_02.x","neck.x","head.x","shoulder.l","arm_stretch.l","arm_twist.l","forearm_stretch.l","forearm_twist.l","hand.l","thigh_stretch.l","thigh_twist.l","leg_stretch.l","leg_twist.l","foot.l","breast_l"};
        m = show("Auto-Rig Pro", arp, flat(arp.size()), d);
        CHECK(has(m, "Thighs", "thigh_stretch.l") && has(m, "Thighs", "thigh_twist.l") && has(m, "Calves", "leg_stretch.l") && has(m, "Calves", "leg_twist.l"));
        CHECK(has(m, "Upper arms", "arm_stretch.l") && has(m, "Upper arms", "arm_twist.l") && has(m, "Shoulders", "shoulder.l") && has(m, "Waist", "spine_01.x"));
        std::vector<std::string> daz = {"hip","pelvis","abdomenLower","abdomenUpper","chestLower","chestUpper","neckLower","head","lCollar","lShldrBend","lShldrTwist","lForearmBend","lForearmTwist","lHand","lPectoral","lThighBend","lThighTwist","lShin","lFoot"};
        m = show("Daz Genesis", daz, flat(daz.size()), d);
        CHECK(has(m, "Thighs", "lThighTwist") && has(m, "Upper arms", "lShldrTwist") && has(m, "Breasts", "lPectoral") && has(m, "Hips", "pelvis"));
        std::vector<std::string> cc = {"CC_Base_Hip","CC_Base_Pelvis","CC_Base_Waist","CC_Base_Spine01","CC_Base_Spine02","CC_Base_L_Clavicle","CC_Base_L_Upperarm","CC_Base_L_UpperarmTwist01","CC_Base_L_Forearm","CC_Base_L_Hand","CC_Base_L_Thigh","CC_Base_L_ThighTwist01","CC_Base_L_Calf","CC_Base_L_Foot","CC_Base_L_Breast"};
        m = show("Character Creator", cc, flat(cc.size()), d);
        CHECK(has(m, "Hips", "CC_Base_Pelvis") && has(m, "Thighs", "CC_Base_L_ThighTwist01") && has(m, "Breasts", "CC_Base_L_Breast"));
        std::vector<std::string> mix = {"mixamorig:Hips","mixamorig:Spine","mixamorig:Spine1","mixamorig:Spine2","mixamorig:LeftShoulder","mixamorig:LeftArm","mixamorig:LeftForeArm","mixamorig:LeftHand","mixamorig:LeftUpLeg","mixamorig:LeftLeg","mixamorig:LeftFoot"};
        m = show("Mixamo", mix, flat(mix.size()), d);
        CHECK(has(m, "Thighs", "mixamorig:LeftUpLeg") && has(m, "Upper arms", "mixamorig:LeftArm") && has(m, "Waist", "mixamorig:Spine"));
    }
    // Morph groups / exclusions from a game profile
    {
        MorphProfile mp;
        std::string mmsg;
        CHECK(parse_morph_profile(R"({ "Project": "x", "MorphGroups": { "Breasts": ["BreastSize*", "Bust"], "Belly": { "Morphs": "Belly", "Section": "Shape", "Hint": "tummy" }, "Nothing": ["NoSuch*"] },
                                     "ExcludeMorphs": ["*_corrective*", "Viseme_##"] })", mp, mmsg));
        CHECK(mp.groups.size() == 3 && mp.exclude.size() == 2 && mp.groups[1].section == "Shape" && mp.groups[1].hint == "tummy");
        const std::vector<std::string> morphs = {"BreastSize", "BreastSize_Lift", "bust", "Belly", "Elbow_Corrective_01", "Viseme_12", "Smile"};
        CHECK(mp.excluded("elbow_corrective_01") && mp.excluded("Viseme_12") && !mp.excluded("Smile"));
        const auto groups = resolve_morph_groups(morphs, mp);
        CHECK(groups.size() == 2 && groups[0].morphs.size() == 3 && groups[1].morphs == std::vector<std::string>{"Belly"}); // empty group dropped
        CHECK(parse_morph_profile(R"({ "Project": "x" })", mp, mmsg) && mp.groups.empty() && mp.exclude.empty());
        CHECK(!parse_morph_profile(R"({ "MorphGroups": ["x"] })", mp, mmsg));
        CHECK(!parse_morph_profile(R"({ "ExcludeMorphs": 5 })", mp, mmsg));
    }
    std::printf(fails ? "GROUP TESTS FAILED (%d)\n" : "group tests passed\n", fails);
    return fails;
}
