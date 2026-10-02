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

        // Character Switch Watcher (optional, config.lua): in games where you switch the character
        // you control while the pawn stays the same (party JRPGs - Clair Obscur: Expedition 33 swaps
        // the meshes of one pawn), the player's sliders belong to the character, not the pawn. Lua
        // reports who is played (its face mesh); each party member keeps its own sliders in
        // "player@<who>". The outgoing character is saved at once, the incoming one loaded (or
        // started empty). The first character seen keeps the sliders the session started with.
        // Returns a line for the log ("" when nothing changed).
        std::string switch_player(Registry& reg, const std::string& who, Clock::time_point now);
        const std::string& player_who() const { return m_player_who; }
        std::string player_slot() const { return m_player_who.empty() ? std::string() : slot_of(m_player_who); }
        static std::string slot_of(const std::string& who) { return "player@" + who; }

        // The same party member as an NPC (Expedition 33: BP_Pawn_AICompanion_Lune_C follows you while
        // you play someone else) shares the party member's sliders: Lua reports such characters (same
        // face mesh), and edits on the player while playing Lune, or on Lune's follower, go to both.
        struct PartyLink
        {
            std::string key;   // the NPC's actor key
            std::string who;   // the party member (face mesh)
            std::string label;
        };
        void set_party_links(std::vector<PartyLink> links);
        // Makes the player (while playing a party member) and that member's linked NPCs hold the same
        // sliders: whichever changed (the one picked in the window first) is copied to the others.
        // A newly linked NPC gets the member's sliders (or gives them, if the member has none yet).
        // Runs from persist(); cheap when nothing changed.
        void sync_party(Registry& reg);
        size_t party_syncs() const { return m_party_syncs; } // syncs that compared books (tests: idle ones are skipped)

      private:
        void log(const std::string& text) const
        {
            if (m_log)
            {
                m_log(text);
            }
        }

        struct Party
        {
            EditBook bones;
            MorphBook morphs;
            std::string text; // serialized; "" = no sliders
        };
        Party& party_of(const std::string& who); // loaded from "player@<who>" the first time
        static std::string text_of(const EditBook& bones, const MorphBook& morphs);

        // Saves (debounced) or forgets one identity's sliders.
        void persist_one(const std::string& identity, const std::string& label, const EditBook& bones, const MorphBook& morphs,
                         Clock::time_point now, Clock::duration delay);

        PresetShelf m_shelf;
        std::string m_player_who;                 // who is played, while the Character Switch Watcher knows
        std::map<std::string, Party> m_party;     // party member -> its sliders (player and followers share them)
        std::vector<PartyLink> m_links;           // NPCs that are a party member (from Lua)
        std::set<std::string> m_linked;           // NPC keys already given (or giving) the member's sliders
        bool m_links_changed{false};
        size_t m_party_syncs{0};
        uint64_t m_sync_bones{0}, m_sync_morphs{0}; // registry revisions at the last sync
        Log m_log;
        std::map<std::string, std::string> m_text;          // identity -> what its file holds (this session)
        std::map<std::string, Clock::time_point> m_due;     // identity -> first unsaved change
        std::set<std::string> m_restored;                   // actor keys already handled
    };
} // namespace uuepbs
