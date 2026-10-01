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
            const bool empty = c.bones.empty() && c.morphs.empty();
            const std::string text = empty ? std::string() : PresetShelf::serialize(c.bones, &c.morphs);
            const auto it = m_text.find(c.identity);
            if (empty)
            {
                // Only forget what this session saved or restored: an NPC that was just picked, and
                // not restored yet, must not lose its file.
                if (it != m_text.end() && !it->second.empty())
                {
                    std::string message;
                    m_shelf.remove(c.identity, message);
                    it->second.clear();
                    log("forgot the remembered sliders of " + c.identity + " (all reset)");
                }
                m_due.erase(c.identity);
                continue;
            }
            if (it != m_text.end() && it->second == text)
            {
                m_due.erase(c.identity);
                continue;
            }
            const auto due = m_due.try_emplace(c.identity, now).first;
            if (now - due->second < delay)
            {
                continue; // still being edited
            }
            std::string message;
            if (m_shelf.save(c.identity, c.bones, message, &c.morphs))
            {
                if (it == m_text.end())
                {
                    log("remembering the sliders of " + c.label + " as " + c.identity);
                }
                m_text[c.identity] = text;
            }
            m_due.erase(c.identity);
        }
    }
} // namespace uuepbs
