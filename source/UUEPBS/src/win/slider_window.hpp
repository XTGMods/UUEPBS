// UUEPBS - the slider window (Dear ImGui, Win32 + D3D11, own thread).
//
// It is a normal top-level window rather than an in-game overlay, so it never
// touches the game's renderer or swap chain; mouse and keyboard work as usual
// while it has focus. The hotkey (polled by the DLL's service thread whenever
// the game or this window has focus) toggles it.
#pragma once

#include <string>

namespace uuepbs
{
    class PresetShelf;
}

namespace uuepbs::ui
{
    class TargetSource;

    struct Options
    {
        bool topmost{true};
        bool session_autosave{true};
        float extra_scale{1.0f};
        float font_size{19.0f};
        bool cpu_renderer{true}; // false: Direct3D 11 (falls back to CPU if that fails)
        int fps{30};             // frame cap while the window is being used (it idles at ~4 fps)
        std::string version;
        TargetSource* targets{};
    };

    bool start(const PresetShelf* shelf, const Options& options);
    void stop();

    void toggle();
    void set_visible(bool visible);
    bool visible();

    // Latest human-readable result (e.g. from a console preset command) shown in the footer.
    void post_message(const std::string& text);
} // namespace uuepbs::ui
