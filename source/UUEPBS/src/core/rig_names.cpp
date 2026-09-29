#include "rig_names.hpp"

#include <cctype>
#include <initializer_list>

namespace uuepbs
{
    namespace
    {
        bool is_sep(char c)
        {
            return c == '_' || c == '.' || c == '-' || c == ' ' || c == ':' || c == '|';
        }
        bool is_upper(char c)
        {
            return c >= 'A' && c <= 'Z';
        }
        bool is_lower(char c)
        {
            return c >= 'a' && c <= 'z';
        }
        bool is_alpha(char c)
        {
            return is_upper(c) || is_lower(c);
        }
        char lower(char c)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        bool word_at(std::string_view s, size_t pos, std::string_view word)
        {
            if (pos + word.size() > s.size())
            {
                return false;
            }
            for (size_t i = 0; i < word.size(); ++i)
            {
                if (lower(s[pos + i]) != word[i])
                {
                    return false;
                }
            }
            return true;
        }

        // "Left"/"Right" as a whole word or a CamelCase part: LeftArm, hand_left, Left Hand.
        SideMark find_word(std::string_view s)
        {
            SideMark best;
            for (const auto& [word, side] : {std::pair<std::string_view, Side>{"left", Side::Left}, {"right", Side::Right}})
            {
                for (size_t pos = 0; pos + word.size() <= s.size(); ++pos)
                {
                    if (!word_at(s, pos, word))
                    {
                        continue;
                    }
                    const char first = s[pos];
                    const bool start_ok = pos == 0 || !is_alpha(s[pos - 1]) || (is_upper(first) && is_lower(s[pos - 1]));
                    const size_t after = pos + word.size();
                    const bool whole_upper = is_upper(first) && is_upper(s[pos + 1]);
                    bool end_ok = after == s.size() || !is_alpha(s[after]) || (is_upper(s[after]) && !whole_upper);
                    if (whole_upper && after < s.size() && is_upper(s[after]))
                    {
                        end_ok = false; // "LEFTARM" is ambiguous
                    }
                    if (start_ok && end_ok && (best.side == Side::None || pos > best.pos))
                    {
                        best = {side, pos, word.size()};
                    }
                }
            }
            return best;
        }

        // A lone l/r between separators: thigh_l, thigh.L, J_Bip_L_Arm, Bip001 L Thigh, L_Hand, horn1_l_001.
        SideMark find_letter(std::string_view s)
        {
            SideMark best;
            for (size_t pos = 0; pos < s.size(); ++pos)
            {
                const char c = lower(s[pos]);
                if (c != 'l' && c != 'r')
                {
                    continue;
                }
                const bool start_ok = pos == 0 || is_sep(s[pos - 1]);
                bool end_ok = pos + 1 == s.size() || is_sep(s[pos + 1]);
                // "Breast_jnt_L01": the side letter followed by nothing but digits.
                if (!end_ok && pos > 0 && is_sep(s[pos - 1]))
                {
                    size_t k = pos + 1;
                    while (k < s.size() && s[k] >= '0' && s[k] <= '9')
                    {
                        ++k;
                    }
                    end_ok = k > pos + 1 && k == s.size();
                }
                // Needs at least one separator next to it, otherwise the whole name is "l".
                const bool has_sep = (pos > 0 && is_sep(s[pos - 1])) || (pos + 1 < s.size() && is_sep(s[pos + 1]));
                if (start_ok && end_ok && has_sep && s.size() > 2)
                {
                    best = {c == 'l' ? Side::Left : Side::Right, pos, 1}; // keep the last one
                }
            }
            return best;
        }

        // Daz Genesis: lThighBend, rShldrTwist (optionally after a namespace).
        SideMark find_daz(std::string_view s)
        {
            size_t start = s.rfind(':');
            start = start == std::string_view::npos ? 0 : start + 1;
            if (start + 2 <= s.size() && (s[start] == 'l' || s[start] == 'r') && is_upper(s[start + 1]))
            {
                return {s[start] == 'l' ? Side::Left : Side::Right, start, 1};
            }
            return {};
        }

        std::string swap_word(std::string_view original, Side from)
        {
            const std::string_view to = from == Side::Left ? "right" : "left";
            std::string out(to);
            const bool all_upper = original.size() > 1 && is_upper(original[0]) && is_upper(original[1]);
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (all_upper || (i == 0 && is_upper(original[0])))
                {
                    out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
                }
            }
            return out;
        }
    } // namespace

    SideMark find_side(std::string_view name)
    {
        const SideMark word = find_word(name);
        const SideMark letter = find_letter(name);
        if (word.side != Side::None && letter.side != Side::None)
        {
            return letter.pos > word.pos ? letter : word;
        }
        if (word.side != Side::None)
        {
            return word;
        }
        if (letter.side != Side::None)
        {
            return letter;
        }
        return find_daz(name);
    }

    std::string mirror_bone_name(std::string_view name)
    {
        const SideMark mark = find_side(name);
        if (mark.side == Side::None)
        {
            return {};
        }
        std::string out(name.substr(0, mark.pos));
        if (mark.len == 1)
        {
            const char c = name[mark.pos];
            const bool upper = is_upper(c);
            const char swapped = mark.side == Side::Left ? 'r' : 'l';
            out += upper ? static_cast<char>(std::toupper(swapped)) : swapped;
        }
        else
        {
            out += swap_word(name.substr(mark.pos, mark.len), mark.side);
        }
        out += name.substr(mark.pos + mark.len);
        return out;
    }

    const StemRules& default_stem_rules()
    {
        static const StemRules rules = [] {
            StemRules r;
            r.raw_prefixes = {"MOT_", "SUP_", "PRG_", "SCB_", "SBC_", "CC_Base_", "J_Bip_C_", "J_Bip_", "J_Sec_", "J_Adj_",
                              "Bip001 ", "Bip01 ", "Bip001_", "Bip01_", "DEF-", "ORG-", "MCH-", "ValveBiped."};
            r.raw_suffixes = {"_M", ".M", "_C", ".C", "_x", ".x", "_SkinJNT", "_jnt", "_bind"};
            r.stem_prefixes = {"mixamorig", "ccbase", "jbipc", "jbip", "jsec", "jadj", "bip001", "bip01", "def", "org", "valvebiped"};
            return r;
        }();
        return rules;
    }

    namespace
    {
        bool starts_with_ci(std::string_view s, std::string_view p)
        {
            if (p.empty() || s.size() <= p.size())
            {
                return false;
            }
            for (size_t i = 0; i < p.size(); ++i)
            {
                if (lower(s[i]) != lower(p[i]))
                {
                    return false;
                }
            }
            return true;
        }

        bool ends_with_ci(std::string_view s, std::string_view p)
        {
            if (p.empty() || s.size() <= p.size())
            {
                return false;
            }
            const size_t off = s.size() - p.size();
            for (size_t i = 0; i < p.size(); ++i)
            {
                if (lower(s[off + i]) != lower(p[i]))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    std::string bone_stem(std::string_view name, const StemRules* rules)
    {
        const StemRules& r = rules ? *rules : default_stem_rules();
        std::string raw(name);
        // Drop a namespace ("mixamorig:", "Armature|").
        const size_t ns = raw.find_last_of(":|");
        if (ns != std::string::npos)
        {
            raw.erase(0, ns + 1);
        }
        // Category / rig prefixes ("MOT_Thigh_L", "CC_Base_L_Thigh", "DEF-thigh.L").
        for (bool again = true; again;)
        {
            again = false;
            for (const std::string& p : r.raw_prefixes)
            {
                if (starts_with_ci(raw, p))
                {
                    raw.erase(0, p.size());
                    again = true;
                }
            }
        }
        const SideMark mark = find_side(raw);
        if (mark.side != Side::None)
        {
            raw.erase(mark.pos, mark.len);
        }
        // Centre markers and joint-type tags ("Chest_M", "spine_01.x", "Breast_jnt").
        for (bool again = true; again;)
        {
            again = false;
            while (!raw.empty() && (raw.back() == '_' || raw.back() == '.' || raw.back() == '-' || raw.back() == ' '))
            {
                raw.pop_back();
            }
            for (const std::string& p : r.raw_suffixes)
            {
                // "_x" is a whole-part suffix only: keep "Hip_Box"'s x.
                if (ends_with_ci(raw, p))
                {
                    raw.erase(raw.size() - p.size());
                    again = true;
                }
            }
        }
        std::string s;
        for (const char c : raw)
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
            {
                s += lower(c);
            }
        }
        for (bool again = true; again;)
        {
            again = false;
            for (const std::string& p : r.stem_prefixes)
            {
                if (s.size() > p.size() && s.compare(0, p.size(), p) == 0)
                {
                    s.erase(0, p.size());
                    again = true;
                }
            }
        }
        return s;
    }

    bool glob_match(std::string_view pattern, std::string_view text)
    {
        // '*' = anything, '#' = zero or more digits, everything else literal (case-insensitive).
        size_t p = 0, t = 0, star_p = std::string_view::npos, star_t = 0;
        while (t < text.size())
        {
            if (p < pattern.size() && pattern[p] == '#')
            {
                size_t k = t;
                while (k < text.size() && text[k] >= '0' && text[k] <= '9')
                {
                    ++k;
                }
                // digits are greedy; '#' followed by more pattern still works for the common cases
                t = k;
                ++p;
                continue;
            }
            if (p < pattern.size() && pattern[p] == '*')
            {
                star_p = p++;
                star_t = t;
                continue;
            }
            if (p < pattern.size() && lower(pattern[p]) == lower(text[t]))
            {
                ++p;
                ++t;
                continue;
            }
            if (star_p != std::string_view::npos)
            {
                p = star_p + 1;
                t = ++star_t;
                continue;
            }
            return false;
        }
        while (p < pattern.size() && (pattern[p] == '*' || pattern[p] == '#'))
        {
            ++p;
        }
        return p == pattern.size();
    }
} // namespace uuepbs
