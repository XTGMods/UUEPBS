// UUEPBS - preset files ("<Win64>/UUEPBS Presets/<name>.json")
//
//   {
//     "format": "UUEPBS preset",
//     "version": 2,
//     "bones": {
//       "boob_l":  { "length": 1.2, "width": 1.2,  "depth": 1.2,  "children": "scale_too" },
//       "thigh_l": { "length": 1.0, "width": 1.15, "depth": 1.15, "children": "keep_size" },
//       "head":    { "length": 1, "width": 1, "depth": 1, "rotate": [0, 0, 10], "move": [0, 0, 1.5],
//                    "children": "scale_too" }
//     }
//   }
//
// length = X (along the bone), width = Y, depth = Z. "rotate" = degrees about the
// bone's X, Y, Z (applied in that order), "move" = cm along X, Y, Z; both optional.
// "children" is one of
// scale_too | keep_size | this_bone_only. Also accepted per bone: "x"/"y"/"z",
// "scale": 1.2 (uniform) or "scale": [x, y, z]. Older ".rbs" text presets still load.
#pragma once

#include "sculpt.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace uuepbs
{
    std::filesystem::path path_from_utf8(const std::string& text);
    std::string path_to_utf8(const std::filesystem::path& path);

    class PresetShelf
    {
      public:
        static constexpr const char* kExtension = ".json";
        static constexpr const char* kLegacyExtension = ".rbs";
        static constexpr const char* kSessionName = "_last_session";

        void set_folder(std::filesystem::path folder);
        const std::filesystem::path& folder() const { return m_folder; }
        bool ready() const { return !m_folder.empty(); }

        // Strips characters Windows does not allow in file names. Empty result = invalid.
        static std::string clean_name(const std::string& raw);

        std::vector<std::string> list(bool include_session = false) const;
        bool exists(const std::string& name) const;
        bool save(const std::string& name, const EditBook& book, std::string& message) const;
        bool load(const std::string& name, EditBook& out, std::string& message) const;
        bool remove(const std::string& name, std::string& message) const;

        static std::string serialize(const EditBook& book);
        // Accepts JSON, or the old line-based ".rbs" text (detected automatically).
        static bool parse(const std::string& text, EditBook& out, std::string& message);

      private:
        static bool parse_json(const std::string& text, EditBook& out, std::string& message);
        static bool parse_legacy(const std::string& text, EditBook& out, std::string& message);
        std::filesystem::path file_for(const std::string& name) const;
        std::filesystem::path legacy_file_for(const std::string& name) const;
        std::filesystem::path m_folder;
    };
} // namespace uuepbs
