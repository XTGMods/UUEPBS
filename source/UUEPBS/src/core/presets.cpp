#include "presets.hpp"

#include "json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace uuepbs
{
    fs::path path_from_utf8(const std::string& text)
    {
        const std::u8string wide(reinterpret_cast<const char8_t*>(text.data()), text.size());
        return fs::path(wide);
    }

    std::string path_to_utf8(const fs::path& path)
    {
        const std::u8string raw = path.u8string();
        return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
    }

    namespace
    {
        std::string trim(std::string_view s)
        {
            size_t b = 0;
            size_t e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
            {
                ++b;
            }
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
            {
                --e;
            }
            return std::string(s.substr(b, e - b));
        }

        using json::JsonReader;
        using json::JsonValue;

        std::string json_escape(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 2);
            out.push_back('"');
            for (const char c : s)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                    {
                        out.push_back(c);
                    }
                }
            }
            out.push_back('"');
            return out;
        }

        // Shortest decimal that round-trips to 4 places: 1.25 not 1.2500, 1 not 1.0000.
        std::string json_number(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", v);
            std::string s = buf;
            while (!s.empty() && s.back() == '0')
            {
                s.pop_back();
            }
            if (!s.empty() && s.back() == '.')
            {
                s.pop_back();
            }
            return s == "-0" ? "0" : s;
        }

        const char* children_word(Spread s)
        {
            switch (s)
            {
            case Spread::Keep:
                return "keep_size";
            case Spread::Solo:
                return "this_bone_only";
            case Spread::Chain:
            default:
                return "scale_too";
            }
        }

        bool children_from_word(std::string_view word, Spread& out)
        {
            std::string w = fold_case(word);
            for (char& c : w)
            {
                if (c == ' ' || c == '-')
                {
                    c = '_';
                }
            }
            if (w == "scale_too" || w == "scale_children_too" || w == "scale")
            {
                out = Spread::Chain;
                return true;
            }
            if (w == "keep_size" || w == "keep_children_size")
            {
                out = Spread::Keep;
                return true;
            }
            if (w == "this_bone_only" || w == "bone_only" || w == "none")
            {
                out = Spread::Solo;
                return true;
            }
            return spread_from_token(w, out); // chain / keep / solo from the old format
        }
    } // namespace

    void PresetShelf::set_folder(fs::path folder)
    {
        m_folder = std::move(folder);
        std::error_code ec;
        fs::create_directories(m_folder, ec);
    }

    std::string PresetShelf::clean_name(const std::string& raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (const char c : raw)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            {
                continue;
            }
            out.push_back(c);
        }
        out = trim(out);
        while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        {
            out.pop_back();
        }
        if (out.size() > 80)
        {
            out.resize(80);
        }
        return out;
    }

    fs::path PresetShelf::file_for(const std::string& name) const
    {
        return m_folder / path_from_utf8(name + kExtension);
    }

    fs::path PresetShelf::legacy_file_for(const std::string& name) const
    {
        return m_folder / path_from_utf8(name + kLegacyExtension);
    }

    std::vector<std::string> PresetShelf::list(bool include_session) const
    {
        std::vector<std::string> names;
        std::error_code ec;
        if (m_folder.empty() || !fs::is_directory(m_folder, ec))
        {
            return names;
        }
        for (fs::directory_iterator it(m_folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::path ext = it->path().extension();
            if (!it->is_regular_file(ec) || (ext != kExtension && ext != kLegacyExtension))
            {
                continue;
            }
            std::string stem = path_to_utf8(it->path().stem());
            if (!include_session && stem == kSessionName)
            {
                continue;
            }
            // A preset saved in both formats is listed once.
            if (std::find(names.begin(), names.end(), stem) == names.end())
            {
                names.push_back(std::move(stem));
            }
        }
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
            return fold_case(a) < fold_case(b);
        });
        return names;
    }

    bool PresetShelf::exists(const std::string& name) const
    {
        std::error_code ec;
        return !m_folder.empty() && (fs::exists(file_for(name), ec) || fs::exists(legacy_file_for(name), ec));
    }

    std::string PresetShelf::serialize(const EditBook& book, const MorphBook* morphs)
    {
        const bool with_morphs = morphs && !morphs->empty();
        std::string out;
        out += "{\n";
        out += "  \"format\": \"UUEPBS preset\",\n";
        out += with_morphs ? "  \"version\": 3,\n" : "  \"version\": 2,\n";
        out += "  \"help\": \"length = along the bone (X), width = Y, depth = Z (1 = unchanged). "
               "rotate = degrees about X, Y, Z. move = cm along X, Y, Z. children: scale_too | keep_size | this_bone_only\",\n";
        out += "  \"bones\": {";
        bool first = true;
        for (const auto& [key, entry] : book)
        {
            if (entry.edit.is_neutral())
            {
                continue;
            }
            out += first ? "\n" : ",\n";
            first = false;
            const BoneEdit& e = entry.edit;
            auto triple = [](const double v[3]) {
                return "[" + json_number(v[0]) + ", " + json_number(v[1]) + ", " + json_number(v[2]) + "]";
            };
            out += "    " + json_escape(entry.bone) + ": { \"length\": " + json_number(e.axis[0]) + ", \"width\": " + json_number(e.axis[1]) +
                   ", \"depth\": " + json_number(e.axis[2]);
            if (e.has_turn())
            {
                out += ", \"rotate\": " + triple(e.turn);
            }
            if (e.has_shift())
            {
                out += ", \"move\": " + triple(e.shift);
            }
            out += ", \"children\": \"" + std::string(children_word(e.spread)) + "\" }";
        }
        out += first ? "}" : "\n  }";
        if (with_morphs)
        {
            out += ",\n  \"morphs\": {";
            bool first_morph = true;
            for (const auto& [key, m] : *morphs)
            {
                out += first_morph ? "\n" : ",\n";
                first_morph = false;
                out += "    " + json_escape(m.name) + ": " + json_number(m.weight);
            }
            out += "\n  }";
        }
        out += "\n}\n";
        return out;
    }

    bool PresetShelf::parse(const std::string& text, EditBook& out, std::string& message, MorphBook* morphs)
    {
        if (morphs)
        {
            morphs->clear();
        }
        std::string_view body = text;
        if (body.size() >= 3 && body.substr(0, 3) == "\xEF\xBB\xBF") // UTF-8 BOM from Notepad
        {
            body.remove_prefix(3);
        }
        const std::string head = trim(body.substr(0, 64));
        const bool looks_json = !head.empty() && head[0] == '{';
        return looks_json ? parse_json(text, out, message, morphs) : parse_legacy(text, out, message);
    }

    bool PresetShelf::parse_json(const std::string& text, EditBook& out, std::string& message, MorphBook* morphs)
    {
        out.clear();
        JsonValue root;
        std::string error;
        if (!JsonReader(text).read(root, error))
        {
            message = "not valid JSON: " + error;
            return false;
        }
        if (root.kind != JsonValue::Kind::Object)
        {
            message = "the file must be a JSON object { ... }";
            return false;
        }
        const JsonValue* bones = root.find("bones");
        const JsonValue* morph_section = root.find("morphs");
        const bool has_morphs = morph_section && morph_section->kind == JsonValue::Kind::Object;
        if ((!bones || bones->kind != JsonValue::Kind::Object) && !has_morphs)
        {
            message = "missing the \"bones\": { ... } section";
            return false;
        }
        JsonValue no_bones;
        no_bones.kind = JsonValue::Kind::Object;
        if (!bones || bones->kind != JsonValue::Kind::Object)
        {
            bones = &no_bones; // a morph-only preset
        }

        int skipped = 0;
        size_t morph_count = 0;
        if (has_morphs)
        {
            for (const auto& [name, v] : morph_section->members)
            {
                if (name.empty() || v.kind != JsonValue::Kind::Number)
                {
                    ++skipped;
                    continue;
                }
                ++morph_count;
                if (morphs)
                {
                    (*morphs)[fold_case(name)] = MorphEntry{name, clamp_morph(v.number)};
                }
            }
        }
        for (const auto& [bone, v] : bones->members)
        {
            if (bone.empty() || v.kind != JsonValue::Kind::Object)
            {
                ++skipped;
                continue;
            }
            EditEntry entry;
            entry.bone = bone;
            bool any = false;

            auto number_of = [&](std::initializer_list<const char*> names, double& target) {
                for (const char* n : names)
                {
                    const JsonValue* f = v.find(n);
                    if (f && f->kind == JsonValue::Kind::Number)
                    {
                        target = f->number;
                        any = true;
                        return;
                    }
                }
            };
            if (const JsonValue* sc = v.find("scale"))
            {
                if (sc->kind == JsonValue::Kind::Number)
                {
                    entry.edit.axis[0] = entry.edit.axis[1] = entry.edit.axis[2] = sc->number;
                    any = true;
                }
                else if (sc->kind == JsonValue::Kind::Array && sc->items.size() == 3)
                {
                    for (int a = 0; a < 3; ++a)
                    {
                        if (sc->items[a].kind == JsonValue::Kind::Number)
                        {
                            entry.edit.axis[a] = sc->items[a].number;
                            any = true;
                        }
                    }
                }
            }
            // "rotate": [x, y, z] or {"x":..,"y":..,"z":..}; same for "move".
            auto vector_of = [&](std::initializer_list<const char*> names, double(&target)[3]) {
                for (const char* n : names)
                {
                    const JsonValue* f = v.find(n);
                    if (!f)
                    {
                        continue;
                    }
                    if (f->kind == JsonValue::Kind::Array && f->items.size() == 3)
                    {
                        for (int a = 0; a < 3; ++a)
                        {
                            if (f->items[a].kind == JsonValue::Kind::Number)
                            {
                                target[a] = f->items[a].number;
                                any = true;
                            }
                        }
                    }
                    else if (f->kind == JsonValue::Kind::Object)
                    {
                        static const char* keys[3] = {"x", "y", "z"};
                        for (int a = 0; a < 3; ++a)
                        {
                            const JsonValue* c = f->find(keys[a]);
                            if (c && c->kind == JsonValue::Kind::Number)
                            {
                                target[a] = c->number;
                                any = true;
                            }
                        }
                    }
                    return;
                }
            };
            vector_of({"rotate", "rotation"}, entry.edit.turn);
            vector_of({"move", "offset", "translate"}, entry.edit.shift);
            number_of({"length", "x"}, entry.edit.axis[0]);
            number_of({"width", "y"}, entry.edit.axis[1]);
            number_of({"depth", "z"}, entry.edit.axis[2]);

            if (const JsonValue* ch = v.find("children"))
            {
                Spread sp;
                if (ch->kind == JsonValue::Kind::String && children_from_word(ch->text, sp))
                {
                    entry.edit.spread = sp;
                }
            }
            if (!any)
            {
                ++skipped;
                continue;
            }
            entry.edit.clamp();
            out[fold_case(bone)] = entry;
        }
        message = std::to_string(out.size()) + " bone(s)";
        if (morph_count > 0)
        {
            message += ", " + std::to_string(morph_count) + " morph(s)";
        }
        if (skipped > 0)
        {
            message += ", " + std::to_string(skipped) + " unreadable entr" + (skipped == 1 ? "y" : "ies") + " ignored";
        }
        return true;
    }

    bool PresetShelf::parse_legacy(const std::string& text, EditBook& out, std::string& message)
    {
        out.clear();
        std::istringstream in(text);
        std::string raw;
        int skipped = 0;
        while (std::getline(in, raw))
        {
            std::string line = trim(raw);
            if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            {
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                ++skipped;
                continue;
            }
            const std::string bone = trim(std::string_view(line).substr(0, eq));
            std::istringstream values(line.substr(eq + 1));
            std::vector<std::string> tokens;
            for (std::string tok; values >> tok;)
            {
                tokens.push_back(tok);
            }
            if (bone.empty() || tokens.empty())
            {
                ++skipped;
                continue;
            }

            EditEntry entry;
            entry.bone = bone;
            std::vector<double> numbers;
            for (const std::string& tok : tokens)
            {
                char* end = nullptr;
                const double v = std::strtod(tok.c_str(), &end);
                if (end && *end == '\0')
                {
                    numbers.push_back(v);
                }
                else
                {
                    Spread s;
                    if (spread_from_token(tok, s))
                    {
                        entry.edit.spread = s;
                    }
                }
            }
            if (numbers.size() == 1)
            {
                entry.edit.axis[0] = entry.edit.axis[1] = entry.edit.axis[2] = numbers[0];
            }
            else if (numbers.size() >= 3)
            {
                entry.edit.axis[0] = numbers[0];
                entry.edit.axis[1] = numbers[1];
                entry.edit.axis[2] = numbers[2];
            }
            else
            {
                ++skipped;
                continue;
            }
            entry.edit.clamp();
            out[fold_case(bone)] = entry;
        }
        message = std::to_string(out.size()) + " bone(s)";
        if (skipped > 0)
        {
            message += ", " + std::to_string(skipped) + " unreadable line(s) ignored";
        }
        return true;
    }

    bool PresetShelf::save(const std::string& name, const EditBook& book, std::string& message, const MorphBook* morphs) const
    {
        const std::string clean = clean_name(name);
        if (clean.empty() || m_folder.empty())
        {
            message = "Enter a preset name first.";
            return false;
        }
        std::error_code ec;
        fs::create_directories(m_folder, ec);

        const fs::path target = file_for(clean);
        fs::path temp = target;
        temp += ".tmp";
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                message = "Could not write " + path_to_utf8(temp);
                return false;
            }
            const std::string body = serialize(book, morphs);
            f.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!f)
            {
                message = "Write failed for " + path_to_utf8(temp);
                return false;
            }
        }
        fs::rename(temp, target, ec);
        if (ec)
        {
            fs::remove(temp, ec);
            message = "Could not replace " + path_to_utf8(target);
            return false;
        }
        // Saving over a preset from the old text format replaces it.
        fs::remove(legacy_file_for(clean), ec);
        message = "Saved '" + clean + "'";
        return true;
    }

    bool PresetShelf::load(const std::string& name, EditBook& out, std::string& message, MorphBook* morphs) const
    {
        const std::string clean = clean_name(name);
        std::error_code ec;
        const fs::path path = fs::exists(file_for(clean), ec) ? file_for(clean) : legacy_file_for(clean);
        std::ifstream f(path, std::ios::binary);
        if (clean.empty() || !f)
        {
            message = "Preset '" + name + "' was not found.";
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string detail;
        if (!parse(ss.str(), out, detail, morphs))
        {
            message = "Preset '" + clean + "' could not be read: " + detail;
            return false;
        }
        message = "Loaded '" + clean + "' (" + detail + ")";
        return true;
    }

    bool PresetShelf::remove(const std::string& name, std::string& message) const
    {
        const std::string clean = clean_name(name);
        std::error_code ec;
        const bool removed_json = !clean.empty() && fs::remove(file_for(clean), ec);
        const bool removed_legacy = !clean.empty() && fs::remove(legacy_file_for(clean), ec);
        if (!removed_json && !removed_legacy)
        {
            message = "Could not delete '" + name + "'.";
            return false;
        }
        message = "Deleted '" + clean + "'";
        return true;
    }
} // namespace uuepbs
