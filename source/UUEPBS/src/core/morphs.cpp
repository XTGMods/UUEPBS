#include "morphs.hpp"

#include "json.hpp"
#include "rig_names.hpp"

#include <algorithm>
#include <cmath>

namespace uuepbs
{
    namespace
    {
        using json::JsonValue;

        std::vector<std::string> strings_of(const JsonValue* v)
        {
            std::vector<std::string> out;
            if (!v)
            {
                return out;
            }
            if (v->kind == JsonValue::Kind::String && !v->text.empty())
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

        std::string text_of(const JsonValue& v, const char* key)
        {
            const JsonValue* f = v.find(key);
            return f && f->kind == JsonValue::Kind::String ? f->text : std::string();
        }
    } // namespace

    double clamp_morph(double weight)
    {
        if (!std::isfinite(weight))
        {
            return 0.0;
        }
        return std::clamp(weight, kMorphMin, kMorphMax);
    }

    bool MorphProfile::excluded(std::string_view morph) const
    {
        for (const std::string& p : exclude)
        {
            if (glob_match(p, morph))
            {
                return true;
            }
        }
        return false;
    }

    bool parse_morph_profile(const std::string& profile_json, MorphProfile& out, std::string& message)
    {
        out = {};
        JsonValue root;
        json::JsonReader reader(profile_json);
        if (!reader.read(root, message))
        {
            message = "not valid JSON: " + message;
            return false;
        }
        if (const JsonValue* ex = root.find("ExcludeMorphs"))
        {
            if (ex->kind != JsonValue::Kind::Array && ex->kind != JsonValue::Kind::String)
            {
                message = "\"ExcludeMorphs\" must be a list like [\"*_corrective*\"]";
                return false;
            }
            out.exclude = strings_of(ex);
        }
        if (const JsonValue* groups = root.find("MorphGroups"))
        {
            if (groups->kind != JsonValue::Kind::Object)
            {
                message = "\"MorphGroups\" must be an object like { \"Breasts\": [\"BreastSize\"] }";
                return false;
            }
            for (const auto& [title, v] : groups->members)
            {
                if (title.empty())
                {
                    continue;
                }
                MorphGroup g;
                g.title = title;
                if (v.kind == JsonValue::Kind::Object)
                {
                    g.patterns = strings_of(v.find("Morphs"));
                    g.section = text_of(v, "Section");
                    g.hint = text_of(v, "Hint");
                }
                else
                {
                    g.patterns = strings_of(&v);
                }
                if (!g.patterns.empty()) // [] hides nothing here: morph groups are only ever added
                {
                    out.groups.push_back(std::move(g));
                }
            }
        }
        message = std::to_string(out.groups.size()) + " morph group(s), " + std::to_string(out.exclude.size()) + " morph exclusion(s)";
        return true;
    }

    std::vector<MorphGroupMorphs> resolve_morph_groups(const std::vector<std::string>& morphs, const MorphProfile& profile)
    {
        std::vector<MorphGroupMorphs> out;
        for (const MorphGroup& g : profile.groups)
        {
            MorphGroupMorphs r;
            r.group = g;
            for (const std::string& m : morphs)
            {
                if (profile.excluded(m))
                {
                    continue;
                }
                for (const std::string& p : g.patterns)
                {
                    if (glob_match(p, m))
                    {
                        r.morphs.push_back(m);
                        break;
                    }
                }
            }
            if (!r.morphs.empty())
            {
                out.push_back(std::move(r));
            }
        }
        return out;
    }

    MorphProfileSource& MorphProfileSource::instance()
    {
        static MorphProfileSource source;
        return source;
    }

    void MorphProfileSource::set(MorphProfile profile, std::string status)
    {
        std::lock_guard guard(m_lock);
        m_profile = std::move(profile);
        m_status = std::move(status);
        ++m_revision;
    }

    uint64_t MorphProfileSource::revision() const
    {
        std::lock_guard guard(m_lock);
        return m_revision;
    }

    void MorphProfileSource::get(MorphProfile& profile, std::string& status) const
    {
        std::lock_guard guard(m_lock);
        profile = m_profile;
        status = m_status;
    }
} // namespace uuepbs
