#include "body_groups.hpp"

#include "json.hpp"

#include <algorithm>
#include <map>

namespace uuepbs
{
    namespace
    {
        // Generated from mod/UUEPBS/Scripts/BoneDictionary.json (tests check they match).
        const char* const kBuiltinDictionary =
#include "bone_dictionary_default.inc"
            ;

        using json::JsonValue;

        std::vector<std::string> strings_of(const JsonValue* v)
        {
            std::vector<std::string> out;
            if (!v)
            {
                return out;
            }
            if (v->kind == JsonValue::Kind::String)
            {
                out.push_back(v->text);
            }
            for (const JsonValue& item : v->items)
            {
                if (item.kind == JsonValue::Kind::String && !item.text.empty())
                {
                    out.push_back(item.text);
                }
            }
            return out;
        }

        bool spread_from_children(const std::string& word, Spread& out)
        {
            std::string w = fold_case(word);
            for (char& c : w)
            {
                if (c == ' ' || c == '-')
                {
                    c = '_';
                }
            }
            if (w == "scale_too" || w == "chain" || w == "scale")
            {
                out = Spread::Chain;
                return true;
            }
            if (w == "keep_size" || w == "keep")
            {
                out = Spread::Keep;
                return true;
            }
            if (w == "this_bone_only" || w == "solo" || w == "none")
            {
                out = Spread::Solo;
                return true;
            }
            return false;
        }

        BodyGroup group_from(const JsonValue& v, const std::string& section)
        {
            BodyGroup g;
            g.section = section;
            if (const JsonValue* s = v.find("Section"); s && s->kind == JsonValue::Kind::String)
            {
                g.section = s->text;
            }
            if (const JsonValue* t = v.find("Title"); t && t->kind == JsonValue::Kind::String)
            {
                g.title = t->text;
            }
            if (const JsonValue* h = v.find("Hint"); h && h->kind == JsonValue::Kind::String)
            {
                g.hint = h->text;
            }
            if (const JsonValue* b = v.find("Bones"))
            {
                g.bones = strings_of(b);
                g.has_bones = true;
            }
            if (const JsonValue* e = v.find("Extra"))
            {
                g.extra = strings_of(e);
                g.has_extra = true;
            }
            if (const JsonValue* h = v.find("Helpers"); h && h->kind == JsonValue::Kind::Bool)
            {
                g.helpers = h->boolean;
                g.has_helpers = true;
            }
            if (const JsonValue* m = v.find("MatchAll"); m && m->kind == JsonValue::Kind::Bool)
            {
                g.match_all = m->boolean;
            }
            if (const JsonValue* c = v.find("Children"); c && c->kind == JsonValue::Kind::String)
            {
                g.has_spread = spread_from_children(c->text, g.spread);
            }
            return g;
        }

        struct Bone
        {
            std::string name;
            std::string stem;
        };

        // "=Name*" matches the original bone name, anything else the cleaned stem.
        bool matches(const std::string& pattern, const Bone& b)
        {
            if (!pattern.empty() && pattern[0] == '=')
            {
                return glob_match(std::string_view(pattern).substr(1), b.name);
            }
            return glob_match(pattern, b.stem);
        }

        bool has_ancestor_in(int32_t i, const std::vector<int32_t>& parents, const std::vector<uint8_t>& in_group)
        {
            for (int32_t p = parents[i]; p >= 0; p = parents[p])
            {
                if (in_group[p])
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    const BoneDictionary& BoneDictionary::builtin()
    {
        static const BoneDictionary dict = [] {
            BoneDictionary d;
            std::string message;
            if (!parse(kBuiltinDictionary, d, message))
            {
                d.rules = default_stem_rules(); // cannot happen with the shipped text (tested)
            }
            return d;
        }();
        return dict;
    }

    bool BoneDictionary::parse(const std::string& text, BoneDictionary& out, std::string& message)
    {
        JsonValue root;
        json::JsonReader reader(text);
        if (!reader.read(root, message))
        {
            message = "not valid JSON: " + message;
            return false;
        }
        if (root.kind != JsonValue::Kind::Object)
        {
            message = "the dictionary must be a JSON object";
            return false;
        }
        BoneDictionary d;
        d.rules = default_stem_rules();
        if (const JsonValue* v = root.find("IgnorePrefixes"))
        {
            d.rules.raw_prefixes = strings_of(v);
        }
        if (const JsonValue* v = root.find("IgnoreSuffixes"))
        {
            d.rules.raw_suffixes = strings_of(v);
        }
        if (const JsonValue* v = root.find("IgnoreStemPrefixes"))
        {
            d.rules.stem_prefixes.clear();
            for (const std::string& s : strings_of(v))
            {
                d.rules.stem_prefixes.push_back(fold_case(s));
            }
        }
        d.helper_words = {"twist", "roll", "jiggle", "bend", "share", "helper"};
        if (const JsonValue* v = root.find("HelperWords"))
        {
            d.helper_words.clear();
            for (const std::string& s : strings_of(v))
            {
                d.helper_words.push_back(fold_case(s));
            }
        }
        const JsonValue* groups = root.find("Groups");
        if (!groups || groups->kind != JsonValue::Kind::Array)
        {
            message = "missing \"Groups\" list";
            return false;
        }
        std::string section = "Body";
        for (const JsonValue& g : groups->items)
        {
            if (g.kind != JsonValue::Kind::Object)
            {
                continue;
            }
            BodyGroup group = group_from(g, section);
            section = group.section;
            if (!group.title.empty())
            {
                d.groups.push_back(std::move(group));
            }
        }
        if (const JsonValue* styles = root.find("Styles"); styles && styles->kind == JsonValue::Kind::Array)
        {
            for (const JsonValue& s : styles->items)
            {
                RigStyle style;
                if (const JsonValue* n = s.find("Name"); n && n->kind == JsonValue::Kind::String)
                {
                    style.name = n->text;
                }
                style.detect = strings_of(s.find("Detect"));
                if (const JsonValue* sg = s.find("Groups"); sg && sg->kind == JsonValue::Kind::Array)
                {
                    for (const JsonValue& g : sg->items)
                    {
                        BodyGroup group = group_from(g, "Custom");
                        if (!group.title.empty())
                        {
                            style.groups.push_back(std::move(group));
                        }
                    }
                }
                if (!style.detect.empty())
                {
                    d.styles.push_back(std::move(style));
                }
            }
        }
        out = std::move(d);
        message = std::to_string(out.groups.size()) + " groups, " + std::to_string(out.styles.size()) + " rig style(s)";
        return true;
    }

    bool parse_user_groups(const std::string& profile_json, std::vector<UserGroup>& out, std::string& message)
    {
        out.clear();
        JsonValue root;
        json::JsonReader reader(profile_json);
        if (!reader.read(root, message))
        {
            message = "not valid JSON: " + message;
            return false;
        }
        const JsonValue* groups = root.find("BodyGroups");
        if (!groups)
        {
            message = "no BodyGroups";
            return true;
        }
        if (groups->kind != JsonValue::Kind::Object)
        {
            message = "\"BodyGroups\" must be an object like { \"Thighs\": [\"Hip_L\", \"Hip_R\"] }";
            return false;
        }
        for (const auto& [title, v] : groups->members)
        {
            UserGroup g;
            g.title = title;
            if (v.kind == JsonValue::Kind::Object)
            {
                g.bones = strings_of(v.find("Bones"));
                if (const JsonValue* s = v.find("Section"); s && s->kind == JsonValue::Kind::String)
                {
                    g.section = s->text;
                }
                if (const JsonValue* h = v.find("Hint"); h && h->kind == JsonValue::Kind::String)
                {
                    g.hint = h->text;
                }
                if (const JsonValue* c = v.find("Children"); c && c->kind == JsonValue::Kind::String)
                {
                    g.has_spread = spread_from_children(c->text, g.spread);
                }
            }
            else
            {
                g.bones = strings_of(&v);
            }
            out.push_back(std::move(g));
        }
        message = std::to_string(out.size()) + " custom group(s)";
        return true;
    }

    BodyMap resolve_body_groups(const std::vector<std::string>& names, const std::vector<int32_t>& parents, const BoneDictionary& dict,
                                const std::vector<UserGroup>& user)
    {
        BodyMap map;
        const size_t n = names.size();
        std::vector<Bone> bones(n);
        for (size_t i = 0; i < n; ++i)
        {
            bones[i] = {names[i], bone_stem(names[i], &dict.rules)};
        }
        const bool have_parents = parents.size() == n;

        auto any_match = [&](const std::string& pattern) {
            for (const Bone& b : bones)
            {
                if (matches(pattern, b))
                {
                    return true;
                }
            }
            return false;
        };

        // 1) Rig style: overrides the fields it sets on the base groups.
        std::vector<BodyGroup> groups = dict.groups;
        for (const RigStyle& style : dict.styles)
        {
            if (!std::all_of(style.detect.begin(), style.detect.end(), any_match))
            {
                continue;
            }
            map.style = style.name;
            for (const BodyGroup& o : style.groups)
            {
                auto it = std::find_if(groups.begin(), groups.end(), [&](const BodyGroup& g) {
                    return fold_case(g.title) == fold_case(o.title);
                });
                if (it == groups.end())
                {
                    groups.push_back(o);
                    continue;
                }
                if (o.has_bones)
                {
                    it->bones = o.bones;
                }
                if (o.has_extra)
                {
                    it->extra = o.extra;
                }
                if (o.has_helpers)
                {
                    it->helpers = o.helpers;
                }
                if (o.has_spread)
                {
                    it->spread = o.spread;
                }
            }
            break;
        }

        // 2) The game profile's own groups: real bone names (wildcards allowed), resolved first.
        struct Pending
        {
            BodyGroup group;
            bool custom;
        };
        std::vector<Pending> order;
        std::vector<std::string> user_titles;
        for (const UserGroup& u : user)
        {
            BodyGroup g;
            auto base = std::find_if(groups.begin(), groups.end(), [&](const BodyGroup& b) {
                return fold_case(b.title) == fold_case(u.title);
            });
            if (base != groups.end())
            {
                g = *base;
            }
            else
            {
                g.section = "Custom";
                g.spread = Spread::Chain;
            }
            g.title = u.title;
            if (!u.section.empty())
            {
                g.section = u.section;
            }
            if (!u.hint.empty())
            {
                g.hint = u.hint;
            }
            if (u.has_spread)
            {
                g.spread = u.spread;
            }
            g.bones.clear();
            for (const std::string& p : u.bones)
            {
                g.bones.push_back(p[0] == '=' ? p : "=" + p); // user entries are real bone names
            }
            g.extra.clear();
            g.helpers = false;
            g.match_all = true;
            user_titles.push_back(fold_case(u.title));
            order.push_back({std::move(g), true});
        }
        for (const BodyGroup& g : groups)
        {
            if (std::find(user_titles.begin(), user_titles.end(), fold_case(g.title)) == user_titles.end())
            {
                order.push_back({g, false});
            }
        }

        // 3) Assign bones. A bone belongs to at most one group.
        std::vector<uint8_t> taken(n, 0);
        std::vector<GroupBones> resolved(order.size());
        for (size_t gi = 0; gi < order.size(); ++gi)
        {
            const BodyGroup& g = order[gi].group;
            std::vector<size_t> picked;
            std::string chosen_stem;
            for (const std::string& pattern : g.bones)
            {
                bool hit = false;
                for (size_t i = 0; i < n; ++i)
                {
                    if (!taken[i] && matches(pattern, bones[i]) && std::find(picked.begin(), picked.end(), i) == picked.end())
                    {
                        picked.push_back(i);
                        hit = true;
                        if (chosen_stem.empty())
                        {
                            chosen_stem = bones[i].stem;
                        }
                    }
                }
                if (hit && !g.match_all)
                {
                    break;
                }
            }
            if (picked.empty())
            {
                continue;
            }
            for (const std::string& pattern : g.extra)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    if (!taken[i] && matches(pattern, bones[i]) && std::find(picked.begin(), picked.end(), i) == picked.end())
                    {
                        picked.push_back(i);
                    }
                }
            }
            if (g.helpers && !chosen_stem.empty())
            {
                std::string base = chosen_stem;
                if (base.size() > 4 && base.ends_with("bend"))
                {
                    base.resize(base.size() - 4); // Daz: ThighBend + ThighTwist
                }
                if (base.size() > 7 && base.ends_with("stretch"))
                {
                    base.resize(base.size() - 7); // Auto-Rig Pro: thigh_stretch + thigh_twist
                }
                while (!base.empty() && base.back() >= '0' && base.back() <= '9')
                {
                    base.pop_back(); // neck01 -> neck
                }
                for (size_t i = 0; i < n; ++i)
                {
                    const std::string& s = bones[i].stem;
                    if (taken[i] || base.empty() || s.size() <= base.size() || s.compare(0, base.size(), base) != 0 ||
                        std::find(picked.begin(), picked.end(), i) != picked.end())
                    {
                        continue;
                    }
                    const std::string_view rest(s.c_str() + base.size());
                    for (const std::string& w : dict.helper_words)
                    {
                        if (rest.starts_with(w))
                        {
                            picked.push_back(i);
                            break;
                        }
                    }
                }
            }
            // "Scale children too" already carries a bone's scale down its chain: keep only the
            // top-most bones, or a breast chain of four bones would be scaled four times over.
            if (g.spread == Spread::Chain && have_parents)
            {
                std::vector<uint8_t> in_group(n, 0);
                for (size_t i : picked)
                {
                    in_group[i] = 1;
                }
                picked.erase(std::remove_if(picked.begin(), picked.end(),
                                            [&](size_t i) {
                                                return has_ancestor_in(static_cast<int32_t>(i), parents, in_group);
                                            }),
                             picked.end());
            }
            std::sort(picked.begin(), picked.end());
            GroupBones& r = resolved[gi];
            r.group = g;
            r.custom = order[gi].custom;
            for (size_t i : picked)
            {
                taken[i] = 1;
                r.bones.push_back(names[i]);
            }
        }

        // 4) Display order: dictionary order, with custom-only groups at the end.
        for (const BodyGroup& g : groups)
        {
            for (GroupBones& r : resolved)
            {
                if (!r.bones.empty() && fold_case(r.group.title) == fold_case(g.title))
                {
                    map.groups.push_back(std::move(r));
                    r.bones.clear();
                }
            }
        }
        for (GroupBones& r : resolved)
        {
            if (!r.bones.empty())
            {
                map.groups.push_back(std::move(r));
            }
        }
        return map;
    }

    BodyMapSource& BodyMapSource::instance()
    {
        static BodyMapSource source;
        return source;
    }

    void BodyMapSource::set(BoneDictionary dict, std::vector<UserGroup> user, std::string status)
    {
        std::lock_guard guard(m_lock);
        m_dict = std::move(dict);
        m_user = std::move(user);
        m_status = std::move(status);
        ++m_revision;
    }

    uint64_t BodyMapSource::revision() const
    {
        std::lock_guard guard(m_lock);
        return m_revision;
    }

    void BodyMapSource::get(BoneDictionary& dict, std::vector<UserGroup>& user, std::string& status) const
    {
        std::lock_guard guard(m_lock);
        dict = m_dict;
        user = m_user;
        status = m_status;
    }
} // namespace uuepbs
