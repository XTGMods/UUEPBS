// UUEPBS - morph targets (blend shapes)
//
// Morph targets are applied by the Lua script with USkeletalMeshComponent::SetMorphTarget
// (a reflected function, so no extra hook is needed). The DLL owns the weights: the
// window edits them, presets store them, and the bridge sends them to Lua.
//
// A morph that is in the book overrides the game's own value, even at 0. A morph that is
// not in the book is left to the game (Lua puts back the value it found before it first
// changed it).
//
// Game profile keys (Scripts/GameProfiles/<game>.json), both optional:
//   "MorphGroups":   { "Breasts": ["BreastSize*"], "Belly": { "Morphs": ["Belly"], "Section": "Body shape", "Hint": "..." } }
//                    adds sliders to the Simplified Panel; one slider drives every matching morph.
//   "ExcludeMorphs": ["*_corrective*", "Viseme_*"]   hides morphs from the window (Lua never touches them).
// Patterns use the bone dictionary rules: case-insensitive, '*' = anything, '#' = digits.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace uuepbs
{
    struct MorphEntry
    {
        std::string name; // display spelling (FName comparison is case-insensitive)
        double weight{0.0};
    };
    // Keyed by case-folded morph name.
    using MorphBook = std::map<std::string, MorphEntry>;

    constexpr double kMorphMin = -2.0; // typed values are clamped to this range
    constexpr double kMorphMax = 2.0;
    double clamp_morph(double weight);

    struct MorphGroup
    {
        std::string title;
        std::string section;
        std::string hint;
        std::vector<std::string> patterns;
    };

    struct MorphProfile
    {
        std::vector<MorphGroup> groups;
        std::vector<std::string> exclude;
        bool excluded(std::string_view morph) const;
    };

    // Reads "MorphGroups" and "ExcludeMorphs" from a game profile (other keys are ignored).
    // false + message when the JSON is broken or a key has the wrong shape.
    bool parse_morph_profile(const std::string& profile_json, MorphProfile& out, std::string& message);

    // A group resolved against the morphs the current meshes actually have.
    struct MorphGroupMorphs
    {
        MorphGroup group;
        std::vector<std::string> morphs;
    };
    std::vector<MorphGroupMorphs> resolve_morph_groups(const std::vector<std::string>& morphs, const MorphProfile& profile);

    // Shared between the DLL's profile watcher (writer) and the window (reader).
    class MorphProfileSource
    {
      public:
        static MorphProfileSource& instance();
        void set(MorphProfile profile, std::string status);
        uint64_t revision() const;
        void get(MorphProfile& profile, std::string& status) const;

      private:
        mutable std::mutex m_lock;
        MorphProfile m_profile;
        std::string m_status{"no morph settings in the game profile"};
        uint64_t m_revision{1};
    };
} // namespace uuepbs
