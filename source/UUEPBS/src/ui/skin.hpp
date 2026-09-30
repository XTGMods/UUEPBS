// UUEPBS - window skins (look only: colours, sizes, images, icons, font)
//
// A skin is a folder in "<Win64>\UUEPBS Skins\<name>\" with a skin.json and PNG/JPG images.
// Everything is optional: whatever a skin leaves out keeps the built-in look, and an image
// that cannot be loaded falls back to the normal widget (the Status tab lists the problem).
// Layout is never changed by a skin.
//
//   {
//     "format": "UUEPBS skin", "version": 1,
//     "name": "Rose Glass", "author": "you",
//     "colors": { "accent": "#E870A8", "window": "#141419", "text": "#EEEEF2", ... },
//     "imgui":  { "ScrollbarGrab": "#553344" },          // any Dear ImGui colour by its name (advanced)
//     "style":  { "frame_rounding": 5, "frame_padding": [9, 6], "font_scale": 1.0, ... },
//     "font":   "MyFont.ttf",
//     "images": {
//       "background":   { "file": "bg.jpg", "mode": "cover", "tint": "#FFFFFFB0" },
//       "button":       { "file": "button.png", "mode": "nine", "slice": 8 },
//       "slider_grab":  "grab.png",
//       ...
//     },
//     "icons": { "simplified": "icons/body.png", "reset": "icons/reset.png", "group.Thighs": "icons/thighs.png" }
//   }
//
// Image modes: stretch | cover | contain | center | tile | nine ("slice": [left, top, right, bottom]
// in image pixels, or one number for all four). See the README in the skins folder for every key.
//
// All drawing helpers run on the window thread, between ImGui::NewFrame() and ImGui::Render().
// Skin changes (select, reload, requests from other threads) are applied in update(), which
// the host calls before NewFrame().
#pragma once

#include "image_decode.hpp"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace uuepbs::ui
{
    enum class ImageMode
    {
        Stretch,
        Cover,
        Contain,
        Center,
        Tile,
        Nine
    };

    struct SkinImageSpec
    {
        std::string file; // relative to the skin folder
        ImageMode mode{ImageMode::Stretch};
        float slice[4]{0, 0, 0, 0}; // nine-slice borders in image pixels: left, top, right, bottom
        ImVec4 tint{1, 1, 1, 1};
        float scale{1.0f}; // border / tile / centre scale (multiplied by the window DPI scale)
        bool has_mode{false};
    };

    // What skin.json says, before any file is loaded. Keys are lower case.
    struct SkinSpec
    {
        std::string name;
        std::string author;
        std::string description;
        std::map<std::string, ImVec4> colors;
        std::map<std::string, ImVec4> imgui_colors; // Dear ImGui colour names, lower case
        std::map<std::string, ImVec2> style;        // numbers are stored as (v, v)
        std::map<std::string, SkinImageSpec> images; // "background", "button", ..., "icon.reset"
        std::string font;
    };

    // "#RGB", "#RGBA", "#RRGGBB", "#RRGGBBAA" or [r, g, b(, a)] as 0..255 / 0..1 numbers.
    bool parse_color(std::string_view text, ImVec4& out);
    // false + message (line/column) when skin.json is not valid. Unknown keys are reported in `warnings`.
    bool parse_skin(const std::string& text, SkinSpec& out, std::string& message, std::vector<std::string>* warnings = nullptr);

    // Colour roles the window uses. Unset roles derive from others (e.g. slider_grab = accent).
    enum class Role
    {
        Accent,
        AccentDim,
        Good,
        Warn,
        Muted,
        Title,
        Text,
        Window,
        Count
    };

    class Skin
    {
      public:
        static constexpr const char* kBuiltinName = "Default";

        Skin() = default;
        Skin(const Skin&) = delete;
        Skin& operator=(const Skin&) = delete;
        ~Skin();

        // ---- setup / switching (window thread) ------------------------------------------
        // Creates the folder (with a README and the bundled example skin) the first time.
        void set_folder(std::filesystem::path folder, const std::string& fallback_skin);
        const std::filesystem::path& folder() const { return m_folder; }
        // Folders with a skin.json, sorted. The built-in look is not listed ("Default").
        static std::vector<std::string> list(const std::filesystem::path& folder);
        std::vector<std::string> available() const { return list(m_folder); }

        // Asks for a skin (any thread). "" or "Default" = built-in look. Applied by update().
        static void request(const std::string& name, bool remember = true);
        // Name of the active skin ("Default" for the built-in look); any thread.
        static std::string active_name();

        // Applies requests, reloads the active skin when its files change (checked about once
        // a second), and frees textures of replaced skins. Call before ImGui::NewFrame().
        // Returns true when the style must be re-applied (apply_style).
        bool update();
        // Loads immediately (tests). false + message when the skin could not be loaded at all.
        bool load(const std::string& name, std::string& message);

        const std::string& name() const { return m_name; }
        const std::string& display_name() const { return m_display; }
        const std::string& author() const { return m_spec.author; }
        const std::vector<std::string>& problems() const { return m_problems; }
        const std::string& status() const { return m_status; }

        // ---- look ------------------------------------------------------------------------
        void apply_style(float dpi);
        const ImVec4& color(Role role) const { return m_roles[static_cast<size_t>(role)]; }
        uint32_t clear_rgb() const; // 0xRRGGBB of the window colour
        ImFont* font() const { return m_font; }

        // ---- drawing helpers (between NewFrame and Render) -------------------------------
        bool has_image(std::string_view key) const;
        bool has_icon(std::string_view key) const;
        // Window background; call right after ImGui::Begin of the main window.
        void draw_window_background();
        // "header" image behind the title row: from the window's top edge down to the cursor
        // plus `row_height`. Call before drawing the row.
        void draw_header_strip(float row_height);
        // Logo image in place of the title text. false when the skin has none (caller draws text).
        bool logo(float height);
        // Icon at the cursor, `size` px square, followed by SameLine. false when missing.
        bool icon(std::string_view key, float size);
        // Draws `key` into the rectangle with the image's mode (no-op when missing).
        void draw_image(ImDrawList* dl, std::string_view key, ImVec2 a, ImVec2 b, float alpha = 1.0f) const;

        bool button(const char* label, std::string_view icon_key = {}, const ImVec2& size = ImVec2(0, 0));
        // Width button() will take for this label (with room for the icon when the skin has one).
        float button_width(const char* label, std::string_view icon_key = {}) const;
        bool small_button(const char* label);
        bool slider_float(const char* label, float* v, float lo, float hi, const char* format, ImGuiSliderFlags flags = 0);
        bool checkbox(const char* label, bool* v);
        bool begin_tab_item(const char* label, std::string_view icon_key, ImGuiTabItemFlags flags);
        bool begin_child(const char* id, const ImVec2& size, ImGuiChildFlags flags);
        void end_child();

        // Releases every texture. Call after the renderer backend shut down and before
        // ImGui::DestroyContext().
        void shutdown();

      private:
        struct Image
        {
            SkinImageSpec spec;
            ImTextureData* tex{};
            int width{};
            int height{};
        };

        const Image* find(std::string_view key) const;
        void draw(ImDrawList* dl, const Image& img, ImVec2 a, ImVec2 b, float alpha) const;
        void resolve_roles();
        void clear_textures();
        void load_font(const std::filesystem::path& file);
        void drop_font();
        void write_readme() const;
        void install_example() const;
        uint64_t files_stamp() const;
        void remember(const std::string& name) const;
        std::string remembered() const;

        class Underlay; // draws an image behind the widget that follows

        std::filesystem::path m_folder;
        std::string m_name;             // folder name, "" = built-in
        std::string m_display{kBuiltinName};
        SkinSpec m_spec;
        std::map<std::string, Image, std::less<>> m_images;
        std::vector<ImTextureData*> m_textures; // owned, one per decoded file
        std::vector<ImTextureData*> m_retired;  // waiting for the backend to destroy them
        std::vector<std::string> m_problems;
        std::string m_status{"built-in look"};
        ImVec4 m_roles[static_cast<size_t>(Role::Count)]{};
        ImFont* m_font{};
        float m_dpi{1.0f};
        uint64_t m_stamp{};
        double m_next_check{};
        bool m_style_dirty{true};
        bool m_folder_ready{false};
        ImDrawListSplitter m_split;
    };

    // The skin the window draws with (window thread only).
    Skin& skin();
} // namespace uuepbs::ui
