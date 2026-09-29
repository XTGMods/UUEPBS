// UUEPBS - redirects the skeletal mesh component's FinalizeBoneTransform
//
// The slot and the pose buffer offsets are not hard-coded: they are worked out
// from the game's own machine code and checked against a live component
// (core/resolver.hpp). Only vtable entries are swapped; every call still runs the
// engine's own implementation first, then lets the registry adjust the pose of
// the components it tracks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace uuepbs::hook
{
    enum class Outcome
    {
        Installed,   // hook active (or was already)
        NotReadyYet, // component has not produced a pose yet, try again later
        Unsupported, // code does not look like any engine build we understand
    };

    struct Report
    {
        Outcome outcome{Outcome::Unsupported};
        std::string message;
    };

    // `expected_bones` is the bone count Lua read from the same component.
    Report install_for(void* skeletal_mesh_component, int32_t expected_bones);
    bool active();
    std::string describe();
    void remove();

    // Call every few hundred ms. If the hooked function never runs, tries the next candidate.
    // Returns a message when it changed something.
    std::string watchdog();

    // Human-readable dump of everything the resolver sees (for bug reports).
    std::string diagnose(void* skeletal_mesh_component, int32_t expected_bones);

    // Per-game pinned values from GameProfiles/<game>.json "Hook" (slot < 0 clears). When set, the
    // search is skipped and only these are validated against the live component.
    void set_override(int slot, uint32_t buffers, uint32_t read_index);
    // JSON snippet with the values in use, for pasting into a game profile ("" when not hooked).
    std::string profile_snippet();

    // Copies memory that may be unmapped or freed; false instead of a crash.
    bool safe_read(uintptr_t address, void* out, size_t size);
} // namespace uuepbs::hook
