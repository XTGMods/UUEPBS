// UUEPBS - bone naming conventions
//
// Works out which side of the body a bone is on and what its opposite is called
// for the naming schemes in common use: Unreal mannequin (thigh_l), Blender and
// Rigify (Thigh.L, DEF-thigh.L), Mixamo (mixamorig:LeftUpLeg), Daz (lThighBend),
// 3ds Max Biped (Bip001 L Thigh), VRoid (J_Bip_L_UpperLeg), Character Creator
// (CC_Base_L_Thigh) and suffix/infix variants such as horn1_l_001.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace uuepbs
{
    enum class Side
    {
        None,
        Left,
        Right,
    };

    struct SideMark
    {
        Side side{Side::None};
        size_t pos{0}; // where the marker starts in the name
        size_t len{0}; // marker length ("l" = 1, "Left" = 4)
    };

    SideMark find_side(std::string_view name);

    // "thigh_l" -> "thigh_r", "mixamorig:LeftArm" -> "mixamorig:RightArm". Empty when the name has no side.
    std::string mirror_bone_name(std::string_view name);

    // How bone names are cleaned before matching (the bone dictionary can replace these).
    struct StemRules
    {
        std::vector<std::string> raw_prefixes;  // removed from the start, case-insensitive: "MOT_", "CC_Base_"
        std::vector<std::string> raw_suffixes;  // removed from the end after the side marker: "_M", ".x", "_jnt"
        std::vector<std::string> stem_prefixes; // removed from the cleaned stem: "mixamorig", "bip001"
    };
    const StemRules& default_stem_rules();

    // Side-free, lower-case, alphanumeric-only name with rig prefixes removed:
    // "mixamorig:LeftUpLeg" -> "upleg", "J_Bip_L_UpperArm" -> "upperarm", "MOT_Thigh_L" -> "thigh",
    // "Chest_M" -> "chest", "thigh_twist_01_l" -> "thightwist01".
    std::string bone_stem(std::string_view name, const StemRules* rules = nullptr);

    // Case-insensitive wildcard match: '*' = anything, '#' = any digits (also none).
    bool glob_match(std::string_view pattern, std::string_view text);
} // namespace uuepbs
