#include "character_memory.hpp"

namespace uuepbs
{
    bool CharacterMemory::restore(Registry& reg, const std::string& key, const std::string& identity, const std::string& label)
    {
        if (!ready() || key.empty() || key == kPlayerActor || identity.empty() || !m_restored.insert(key).second)
        {
            return false;
        }
        if (!reg.edits_of(key).empty() || !reg.morphs_of(key).empty() || !m_shelf.exists(identity))
        {
            return false; // sliders made this time win; nothing saved yet
        }
        EditBook book;
        MorphBook morphs;
        std::string message;
        if (!m_shelf.load(identity, book, message, &morphs))
        {
            log("could not restore " + label + ": " + message);
            return false;
        }
        m_text[identity] = PresetShelf::serialize(book, &morphs);
        reg.replace_edits_of(key, std::move(book));
        reg.replace_morphs_of(key, std::move(morphs));
        log("restored the sliders of " + label + " (remembered as " + identity + ", " + message + ")");
        return true;
    }

    void CharacterMemory::persist(Registry& reg, Clock::time_point now, Clock::duration delay)
    {
        if (!ready())
        {
            return;
        }
        for (const std::string& id : reg.take_forgotten_identities())
        {
            std::string message;
            if (m_shelf.exists(id))
            {
                m_shelf.remove(id, message);
                log("forgot the remembered sliders of " + id);
            }
            m_text.erase(id);
            m_due.erase(id);
        }
        for (const CharacterBook& c : reg.character_books())
        {
            persist_one(c.identity, c.label, c.bones, c.morphs, now, delay);
        }
        sync_party(reg);
        for (const auto& [who, p] : m_party)
        {
            persist_one(slot_of(who), slot_of(who), p.bones, p.morphs, now, delay);
        }
    }

    void CharacterMemory::persist_one(const std::string& identity, const std::string& label, const EditBook& bones, const MorphBook& morphs,
                                      Clock::time_point now, Clock::duration delay)
    {
        const bool empty = bones.empty() && morphs.empty();
        const std::string text = empty ? std::string() : PresetShelf::serialize(bones, &morphs);
        const auto it = m_text.find(identity);
        if (empty)
        {
            // Only forget what this session saved or restored: an NPC that was just picked, and
            // not restored yet, must not lose its file.
            if (it != m_text.end() && !it->second.empty())
            {
                std::string message;
                m_shelf.remove(identity, message);
                it->second.clear();
                log("forgot the remembered sliders of " + identity + " (all reset)");
            }
            m_due.erase(identity);
            return;
        }
        if (it != m_text.end() && it->second == text)
        {
            m_due.erase(identity);
            return;
        }
        const auto due = m_due.try_emplace(identity, now).first;
        if (now - due->second < delay)
        {
            return; // still being edited
        }
        std::string message;
        if (m_shelf.save(identity, bones, message, &morphs))
        {
            if (it == m_text.end())
            {
                log("remembering the sliders of " + label + " as " + identity);
            }
            m_text[identity] = text;
        }
        m_due.erase(identity);
    }

    std::string CharacterMemory::text_of(const EditBook& bones, const MorphBook& morphs)
    {
        return bones.empty() && morphs.empty() ? std::string() : PresetShelf::serialize(bones, &morphs);
    }

    CharacterMemory::Party& CharacterMemory::party_of(const std::string& who)
    {
        const auto found = m_party.find(who);
        if (found != m_party.end())
        {
            return found->second;
        }
        Party& p = m_party[who];
        const std::string slot = slot_of(who);
        std::string message;
        if (m_shelf.exists(slot) && m_shelf.load(slot, p.bones, message, &p.morphs))
        {
            p.text = text_of(p.bones, p.morphs);
            m_text[slot] = p.text;
        }
        return p;
    }

    std::string CharacterMemory::switch_player(Registry& reg, const std::string& who, Clock::time_point now)
    {
        if (!ready() || who.empty() || who == m_player_who)
        {
            return {};
        }
        const bool first = m_player_who.empty();
        if (!first)
        {
            // the character being left: its latest sliders, saved right away (not a second later)
            m_sync_bones = 0;
            sync_party(reg);
            const Party& old = party_of(m_player_who);
            persist_one(slot_of(m_player_who), slot_of(m_player_who), old.bones, old.morphs, now, Clock::duration::zero());
        }
        m_player_who = who;
        Party& p = party_of(who);
        std::string line;
        if (!p.text.empty())
        {
            reg.replace_edits_of(kPlayerActor, p.bones);
            reg.replace_morphs_of(kPlayerActor, p.morphs);
            line = "now playing " + who + ": its own sliders";
        }
        else if (first)
        {
            p.bones = reg.edits_of(kPlayerActor);
            p.morphs = reg.morphs_of(kPlayerActor);
            p.text = text_of(p.bones, p.morphs);
            line = "now playing " + who + ": it keeps the sliders the session started with";
        }
        else
        {
            reg.replace_edits_of(kPlayerActor, {});
            reg.replace_morphs_of(kPlayerActor, {});
            line = "now playing " + who + ": no sliders yet";
        }
        m_sync_bones = 0; // the player's book was set from the party member: not an edit
        sync_party(reg);
        return line;
    }

    void CharacterMemory::set_party_links(std::vector<PartyLink> links)
    {
        const auto same = [](const PartyLink& a, const PartyLink& b) { return a.key == b.key && a.who == b.who; };
        if (links.size() != m_links.size() || !std::equal(links.begin(), links.end(), m_links.begin(), same))
        {
            m_links_changed = true;
        }
        m_links = std::move(links);
        for (const PartyLink& l : m_links)
        {
            m_restored.insert(l.key); // never restored from its own NPC file: it shares the party member's
        }
    }

    void CharacterMemory::sync_party(Registry& reg)
    {
        if (m_player_who.empty() && m_links.empty())
        {
            return;
        }
        const uint64_t bones_rev = reg.edit_revision();
        const uint64_t morphs_rev = reg.morph_revision();
        if (!m_links_changed && bones_rev == m_sync_bones && morphs_rev == m_sync_morphs)
        {
            return;
        }
        m_links_changed = false;
        ++m_party_syncs;
        // who -> keys holding that party member's sliders
        std::map<std::string, std::vector<std::string>> members;
        if (!m_player_who.empty())
        {
            members[m_player_who].push_back(kPlayerActor);
        }
        std::set<std::string> linkedNow;
        for (const PartyLink& l : m_links)
        {
            if (!l.key.empty() && l.key != kPlayerActor && !l.who.empty())
            {
                members[l.who].push_back(l.key);
                linkedNow.insert(l.key);
            }
        }
        const std::string active = reg.active_actor();
        for (const auto& [who, keys] : members)
        {
            Party& p = party_of(who);
            std::map<std::string, std::string> texts;
            for (const std::string& key : keys)
            {
                texts[key] = text_of(reg.edits_of(key), reg.morphs_of(key));
            }
            // newly linked NPCs: they get the member's sliders, or give theirs when the member has none
            for (const std::string& key : keys)
            {
                if (key == kPlayerActor || m_linked.count(key))
                {
                    continue;
                }
                m_linked.insert(key);
                if (p.text.empty() && !texts[key].empty())
                {
                    p.bones = reg.edits_of(key);
                    p.morphs = reg.morphs_of(key);
                    p.text = texts[key];
                    log("party member " + who + " takes the sliders its follower " + key + " had");
                }
                else
                {
                    texts[key] = std::string("\x01"); // force the copy below
                }
            }
            // an edit on any of them: the one picked in the window wins, else the first changed one
            std::string source;
            for (const std::string& key : keys)
            {
                if (texts[key] != p.text && texts[key] != "\x01" && (source.empty() || key == active))
                {
                    source = key;
                }
            }
            if (!source.empty())
            {
                p.bones = reg.edits_of(source);
                p.morphs = reg.morphs_of(source);
                p.text = texts[source];
            }
            for (const std::string& key : keys)
            {
                if (texts[key] != p.text)
                {
                    reg.replace_edits_of(key, p.bones);
                    reg.replace_morphs_of(key, p.morphs);
                }
            }
        }
        // links that went away (follower despawned or out of the list): forget they were linked
        for (auto it = m_linked.begin(); it != m_linked.end();)
        {
            it = linkedNow.count(*it) ? std::next(it) : m_linked.erase(it);
        }
        m_sync_bones = reg.edit_revision();
        m_sync_morphs = reg.morph_revision();
    }
} // namespace uuepbs
