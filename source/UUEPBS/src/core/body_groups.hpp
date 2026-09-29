// UUEPBS - which bones the curated "Body" tab sliders drive
//
// Data-driven: Scripts/BoneDictionary.json lists the body parts (Head, Waist,
// Thighs, Biceps, ...) and the bone-name patterns each one answers to, plus rig
// "styles" (e.g. Advanced Skeleton, where Hip_L is the thigh). A copy of the
// shipped file is built in, so the DLL works without it. Each game can override
// or add groups in its GameProfiles/<game>.json ("BodyGroups"), using real bone
// names. See the "help" text inside BoneDictionary.json for the pattern rules.
#pragma once

#include "rig_names.hpp"
#include "sculpt.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace uuepbs
{
    struct BodyGroup
    {
        std::string section;
        std::string title;
        std::string hint;
        std::vector<std::string> bones; // patterns, first one that matches wins (all of them with match_all)
        std::vector<std::string> extra; // patterns, every match is added
        bool helpers{false};
        bool match_all{false};
        Spread spread{Spread::Chain};
        bool has_bones{false}; // (styles/overrides) this entry replaces the bone patterns
        bool has_extra{false};
        bool has_spread{false};
        bool has_helpers{false};
    };

    struct RigStyle
    {
        std::string name;
        std::vector<std::string> detect;
        std::vector<BodyGroup> groups; // partial: only the fields they set override the base group
    };

    struct BoneDictionary
    {
        StemRules rules;
        std::vector<std::string> helper_words;
        std::vector<BodyGroup> groups;
        std::vector<RigStyle> styles;

        static const BoneDictionary& builtin();
        // false + message (with line/column) when the text is not a valid dictionary.
        static bool parse(const std::string& text, BoneDictionary& out, std::string& message);
    };

    // A game profile's "BodyGroups": patterns are matched against the real bone names.
    struct UserGroup
    {
        std::string title;
        std::string section;
        std::string hint;
        std::vector<std::string> bones;
        bool has_spread{false};
        Spread spread{Spread::Chain};
    };
    // Reads the "BodyGroups" object of a game profile JSON (other keys are ignored).
    bool parse_user_groups(const std::string& profile_json, std::vector<UserGroup>& out, std::string& message);

    struct GroupBones
    {
        BodyGroup group;                // section/title/hint/spread as resolved
        std::vector<std::string> bones; // real bone names on this skeleton
        bool custom{false};             // came from the game profile
    };

    struct BodyMap
    {
        std::vector<GroupBones> groups;
        std::string style; // detected rig style, empty = generic
    };

    // Matches the groups against a skeleton; groups with no bones are left out.
    BodyMap resolve_body_groups(const std::vector<std::string>& names, const std::vector<int32_t>& parents, const BoneDictionary& dict,
                                const std::vector<UserGroup>& user = {});

    // Shared between the DLL's file watcher (writer) and the window (reader).
    class BodyMapSource
    {
      public:
        static BodyMapSource& instance();
        void set(BoneDictionary dict, std::vector<UserGroup> user, std::string status);
        uint64_t revision() const;
        void get(BoneDictionary& dict, std::vector<UserGroup>& user, std::string& status) const;

      private:
        mutable std::mutex m_lock;
        BoneDictionary m_dict{BoneDictionary::builtin()};
        std::vector<UserGroup> m_user;
        std::string m_status{"built-in dictionary"};
        uint64_t m_revision{1};
    };
} // namespace uuepbs
