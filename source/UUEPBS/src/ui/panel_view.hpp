// UUEPBS - contents of the slider window (pure Dear ImGui).
//
// Kept apart from the Win32/D3D11 host so the same drawing code can be run
// headless (tests, screenshots) and the host stays small.
//
// Tabs: Simplified Panel (curated body-part sliders, plus morph groups from the game
// profile), Detailed Panel (every bone, and every morph target), Presets, Status.
// Every widget is drawn through ui::skin(), so a window skin can restyle all of it.
#pragma once

#include "../core/body_groups.hpp"
#include "../core/morphs.hpp"
#include "../core/presets.hpp"
#include "../core/registry.hpp"

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace uuepbs::ui
{
    // What the view needs from whoever hosts it.
    class PanelHost
    {
      public:
        virtual ~PanelHost() = default;
        virtual bool hook_active() = 0;
        virtual std::string hook_text() = 0;
        virtual void open_folder(const std::filesystem::path& folder) = 0;
        virtual void set_topmost(bool on) = 0;
        virtual std::string renderer_text() { return {}; }
    };

    struct TargetChoice
    {
        std::string id;
        std::string label;
        std::string key{"player"}; // actor key: "player" for the default character, else its address
    };

    // Characters the user can switch the sliders to (fed by the Lua script).
    class TargetSource
    {
      public:
        virtual ~TargetSource() = default;
        virtual std::vector<TargetChoice> targets() = 0;
        virtual TargetChoice current_target() = 0;
        virtual void pick_target(const std::string& id) = 0; // "auto" = back to the default
        virtual void refresh_targets() = 0;                    // relist after searching the whole world (Refresh)
        virtual void relist_targets() { refresh_targets(); }   // re-sort what is already known (opening the list)
        virtual void request_rescan() = 0;
        virtual std::string link_text() = 0;   // state of the Lua link, for the Status tab
        virtual std::string hotkey_text() = 0; // e.g. "F6"
        virtual std::string write_diagnostics() { return {}; } // returns where it was written
        virtual std::string refresh_hotkey_text() { return {}; } // e.g. "F7", empty = no hotkey
    };

    class PanelView
    {
      public:
        using Clock = std::chrono::steady_clock;

        void configure(const PresetShelf* shelf, PanelHost* host, TargetSource* targets, std::string version, bool topmost);
        // Applies the active skin's colours and sizes (the built-in look without a skin).
        static void apply_theme(float dpi);

        enum class Tab
        {
            Simplified, // was "Body" before 2.2
            Detailed,   // was "Bones" before 2.2
            Presets,
            Status
        };
        enum class Detail
        {
            Bones,
            Morphs
        };

        void draw();
        void refresh_presets();
        void focus_tab(Tab tab) { m_focus_tab = static_cast<int>(tab); }
        void focus_detail(Detail d) { m_focus_detail = static_cast<int>(d); }
        void select_bone(const std::string& bone) { m_selected = bone; }
        void set_message(const std::string& text);

      private:
        void sync();
        bool has_bone(const std::string& bone) const;
        bool has_morph(const std::string& morph) const;
        Spread spread_of(const std::string& bone, Spread fallback) const;
        BoneEdit scale_of(const std::string& bone, Spread fallback) const;
        void write(const std::string& bone, BoneEdit s);
        void write_group(const GroupBones& group, const double axis[3]);
        std::optional<double> morph_of(const std::string& morph) const;
        void write_morph(const std::string& morph, std::optional<double> weight);
        static bool size_slider(const char* id, BoneEdit& s, float lo, float hi);
        static bool axis_sliders(BoneEdit& s);
        static bool turn_sliders(BoneEdit& s);
        static bool shift_sliders(BoneEdit& s);
        static bool morph_slider(const char* id, float& v);
        static void tooltip(const char* text);
        void draw_header();
        void draw_target_picker();
        void draw_simplified();
        void draw_simplified_morphs(float label_w);
        void draw_detailed();
        void draw_detailed_bones();
        void draw_detailed_morphs();
        void load_preset(const std::string& name);
        void save_preset(const std::string& name);
        void draw_presets();
        void draw_status();
        void draw_skin_picker();
        void draw_edited_characters();
        bool is_edited(const std::string& key) const;

        const PresetShelf* m_shelf{};
        PanelHost* m_host{};
        TargetSource* m_targets{};
        std::vector<TargetChoice> m_target_list{};
        TargetChoice m_target{};
        Clock::time_point m_target_time{};
        // While the character list is open it takes the fresh nearest-first order that opening it asked for,
        // then stays put, so entries don't move under the mouse.
        bool m_target_combo_open{false};
        Clock::time_point m_target_combo_since{};
        BodyMap m_body{};
        uint64_t m_body_revision{0};
        std::string m_body_status{};
        std::map<std::string, std::string> m_bone_group{}; // folded bone -> Simplified Panel group title
        std::string m_mirror_text{};
        std::string m_version{};
        bool m_topmost{true};
        int m_focus_tab{-1};
        int m_focus_detail{-1};

        SkeletonView m_skeleton{};
        std::set<std::string> m_bone_keys{};
        uint64_t m_skeleton_revision{~0ull};
        EditBook m_book{};
        uint64_t m_book_revision{~0ull};
        std::vector<RigSummary> m_rigs{};
        std::vector<ActorSummary> m_edited{}; // characters with sliders (active first)
        std::vector<std::string> m_remembered{}; // NPC identities with saved sliders
        Clock::time_point m_rigs_time{};

        // morph targets
        std::vector<std::string> m_morph_names{}; // after the profile's ExcludeMorphs
        std::set<std::string> m_morph_keys{};
        uint64_t m_morph_names_revision{~0ull};
        MorphBook m_morphs{};
        uint64_t m_morph_revision{~0ull};
        MorphProfile m_morph_profile{};
        std::string m_morph_status{};
        uint64_t m_morph_profile_revision{0};
        std::vector<MorphGroupMorphs> m_morph_groups{};
        std::set<std::string> m_animated{};
        size_t m_morphs_hidden{0};
        char m_morph_filter[64]{};
        bool m_morph_only_edited{false};

        std::map<std::string, bool> m_group_open{};
        std::map<std::string, Spread> m_spread_hint{};
        char m_filter[64]{};
        bool m_only_edited{false};
        bool m_mirror{true};
        std::string m_selected{};

        std::vector<std::string> m_presets{};
        Clock::time_point m_presets_time{};
        char m_preset_name[96]{};
        std::string m_picked{};
        std::string m_pending{};

        std::vector<std::string> m_skins{};
        Clock::time_point m_skins_time{};

        std::mutex m_message_lock;
        std::string m_message{};
        Clock::time_point m_message_time{};
    };
} // namespace uuepbs::ui
