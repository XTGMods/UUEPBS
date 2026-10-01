// UUEPBS - remembered NPC sliders ("<presets>/_characters/<identity>.json")
//
// NPC actors get a new address on every load, so their sliders are remembered by identity
// (the name without the instance number, or class@face mesh - worked out by the Lua script).
// An edited NPC's sliders are saved a second after they change, like _last_session for the
// player. When a remembered NPC is loaded again (Lua reports it, or it is picked), its sliders
// are put back once per appearance - unless it already has sliders. Resetting all of an NPC's
// sliders, or releasing it, forgets it.
#pragma once

#include "presets.hpp"
#include "registry.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace uuepbs
{
    class CharacterMemory
    {
      public:
        using Clock = std::chrono::steady_clock;
        using Log = std::function<void(const std::string&)>;

        void set_folder(const std::filesystem::path& folder) { m_shelf.set_folder(folder); }
        const std::filesystem::path& folder() const { return m_shelf.folder(); }
        bool ready() const { return m_shelf.ready(); }
        void set_log(Log log) { m_log = std::move(log); }

        // Gives actor `key` its remembered sliders (once per key). Returns true when it did.
        bool restore(Registry& reg, const std::string& key, const std::string& identity, const std::string& label);
        // Saves changed NPC sliders (debounced by `delay`), deletes forgotten ones.
        void persist(Registry& reg, Clock::time_point now, Clock::duration delay = std::chrono::seconds(1));
        // Identities with a saved file.
        std::vector<std::string> list() const { return m_shelf.list(); }

      private:
        void log(const std::string& text) const
        {
            if (m_log)
            {
                m_log(text);
            }
        }

        PresetShelf m_shelf;
        Log m_log;
        std::map<std::string, std::string> m_text;          // identity -> what its file holds (this session)
        std::map<std::string, Clock::time_point> m_due;     // identity -> first unsaved change
        std::set<std::string> m_restored;                   // actor keys already handled
    };
} // namespace uuepbs
