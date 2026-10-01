#include "bridge.hpp"

#include "sculpt.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace uuepbs::bridge
{
    namespace
    {
        std::vector<std::string_view> split(std::string_view line, char sep)
        {
            std::vector<std::string_view> out;
            size_t start = 0;
            while (true)
            {
                const size_t at = line.find(sep, start);
                if (at == std::string_view::npos)
                {
                    out.push_back(line.substr(start));
                    return out;
                }
                out.push_back(line.substr(start, at - start));
                start = at + 1;
            }
        }

        int64_t to_int(std::string_view s, int base = 10)
        {
            const std::string tmp(s);
            return std::strtoll(tmp.c_str(), nullptr, base);
        }

        uint64_t to_uint(std::string_view s, int base)
        {
            const std::string tmp(s);
            return std::strtoull(tmp.c_str(), nullptr, base);
        }

        bool fail(std::string* why, const std::string& text)
        {
            if (why)
            {
                *why = text;
            }
            return false;
        }
    } // namespace

    std::optional<LuaState> parse_lua_state(std::string_view text, std::string* why)
    {
        LuaState state;
        std::vector<std::string_view> lines;
        {
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                {
                    end = text.size();
                }
                std::string_view line = text.substr(start, end - start);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                lines.push_back(line);
                start = end + 1;
            }
        }
        while (!lines.empty() && lines.back().empty())
        {
            lines.pop_back();
        }
        if (lines.size() < 2)
        {
            fail(why, "file is empty or incomplete");
            return std::nullopt;
        }

        const auto head = split(lines.front(), ' ');
        if (head.size() < 3 || head[0] != "UBS1")
        {
            fail(why, "unknown header");
            return std::nullopt;
        }
        state.session = std::string(head[1]);
        state.seq = to_int(head[2]);
        const std::string_view tail = lines.back();
        if (tail.substr(0, 5) != "#end " || to_int(tail.substr(5)) != state.seq)
        {
            fail(why, "file is still being written");
            return std::nullopt;
        }

        RigInfo* rig = nullptr;
        for (size_t i = 1; i + 1 < lines.size(); ++i)
        {
            const auto f = split(lines[i], '\t');
            const std::string_view kind = f[0];
            if (kind == "setup")
            {
                for (size_t k = 1; k < f.size(); ++k)
                {
                    const size_t eq = f[k].find('=');
                    if (eq != std::string_view::npos)
                    {
                        state.setup[std::string(f[k].substr(0, eq))] = std::string(f[k].substr(eq + 1));
                    }
                }
            }
            else if (kind == "target" && f.size() >= 3)
            {
                state.target = {std::string(f[1]), std::string(f[2]), f.size() >= 4 && !f[3].empty() ? std::string(f[3]) : std::string("player"),
                                f.size() >= 5 ? std::string(f[4]) : std::string()};
            }
            else if (kind == "cand" && f.size() >= 3)
            {
                state.candidates.push_back({std::string(f[1]), std::string(f[2]), f.size() >= 4 && !f[3].empty() ? std::string(f[3]) : std::string(f[1]), {}});
            }
            else if (kind == "npc" && f.size() >= 3 && !f[1].empty() && !f[2].empty())
            {
                state.npcs.push_back({std::string(f[1]), std::string(f[2]), f.size() >= 4 ? std::string(f[3]) : std::string(f[2])});
            }
            else if (kind == "gone" && f.size() >= 2 && !f[1].empty())
            {
                state.gone.emplace_back(f[1]);
            }
            else if (kind == "rig" && f.size() >= 4)
            {
                RigInfo r;
                r.address = static_cast<uintptr_t>(to_uint(f[1], 16));
                r.label = std::string(f[2]);
                r.primary = f[3] == "1";
                r.owner = f.size() >= 5 ? std::string(f[4]) : std::string();
                r.actor = f.size() >= 6 && !f[5].empty() ? std::string(f[5]) : std::string("player");
                state.rigs.push_back(std::move(r));
                rig = &state.rigs.back();
            }
            else if (kind == "bones" && rig)
            {
                for (size_t k = 1; k < f.size(); ++k)
                {
                    rig->names.emplace_back(f[k]);
                }
            }
            else if (kind == "parents" && rig)
            {
                for (size_t k = 1; k < f.size(); ++k)
                {
                    rig->parents.push_back(static_cast<int32_t>(to_int(f[k])));
                }
            }
            else if (kind == "ref" && rig && f.size() >= 2)
            {
                std::vector<double> numbers;
                const std::string all(f[1]);
                const char* p = all.c_str();
                char* end = nullptr;
                while (*p)
                {
                    const double v = std::strtod(p, &end);
                    if (end == p)
                    {
                        break;
                    }
                    numbers.push_back(v);
                    p = end;
                }
                if (numbers.size() == rig->names.size() * 10 && !numbers.empty())
                {
                    rig->reference.resize(rig->names.size(), xf::identity());
                    for (size_t b = 0; b < rig->names.size(); ++b)
                    {
                        const double* n = &numbers[b * 10];
                        Xform& x = rig->reference[b];
                        for (int a = 0; a < 4; ++a)
                        {
                            x.rot[a] = n[a];
                        }
                        for (int a = 0; a < 3; ++a)
                        {
                            x.pos[a] = n[4 + a];
                            x.scl[a] = n[7 + a];
                        }
                        x.pos[3] = x.scl[3] = 0.0;
                    }
                }
            }
            else if (kind == "morphs" && rig)
            {
                for (size_t k = 1; k < f.size(); ++k)
                {
                    if (!f[k].empty())
                    {
                        rig->morphs.emplace_back(f[k]);
                    }
                }
            }
            else if (kind == "manim")
            {
                for (size_t k = 1; k < f.size(); ++k)
                {
                    if (!f[k].empty())
                    {
                        state.animated_morphs.emplace_back(f[k]);
                    }
                }
            }
            else if (kind == "cmd" && f.size() >= 3)
            {
                Command c;
                c.id = to_int(f[1]);
                c.verb = fold_case(f[2]);
                c.argument = f.size() >= 4 ? std::string(f[3]) : std::string();
                state.commands.push_back(std::move(c));
            }
        }

        for (const RigInfo& r : state.rigs)
        {
            if (r.address == 0 || r.names.empty() || r.names.size() != r.parents.size())
            {
                fail(why, "rig '" + r.label + "' has mismatched bone lists");
                return std::nullopt;
            }
        }
        return state;
    }

    std::string clean_field(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
        {
            if (c == '\t' || c == '\n' || c == '\r')
            {
                c = ' ';
            }
        }
        return out;
    }

    std::string escape_lines(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            if (c == '\n')
            {
                out += "\\n";
            }
            else if (c == '\t' || c == '\r')
            {
                out += ' ';
            }
            else
            {
                out += c;
            }
        }
        return out;
    }

    std::string format_dll_state(const DllState& s)
    {
        std::string out = "UBS1 " + clean_field(s.session) + "\n";
        out += "for\t" + clean_field(s.lua_session) + "\n";
        out += "ack\t" + std::to_string(s.ack) + "\n";
        out += "rescan\t" + std::to_string(s.rescan) + "\n";
        out += "pick\t" + std::to_string(s.pick) + "\t" + clean_field(s.pick_id) + "\n";
        out += "refresh\t" + std::to_string(s.refresh) + "\n";
        out += "hook\t" + clean_field(s.hook_state) + "\t" + clean_field(s.hook_text) + "\n";
        out += std::string("ui\t") + (s.window_open ? "1" : "0") + "\n";
        for (const Reply& r : s.replies)
        {
            out += "reply\t" + std::to_string(r.id) + "\t" + escape_lines(r.text) + "\n";
        }
        for (const auto& [key, label] : s.keeps)
        {
            out += "keep\t" + clean_field(key) + "\t" + clean_field(label) + "\n";
        }
        for (const std::string& id : s.remembered)
        {
            out += "remember\t" + clean_field(id) + "\n";
        }
        out += "morph\t" + std::to_string(s.morph_revision) + "\n";
        for (const DllState::Morph& m : s.morphs)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", m.weight);
            out += "mw\t" + clean_field(m.name) + "\t" + buf + "\t" + clean_field(m.actor) + "\n";
        }
        out += "#end\n";
        return out;
    }

    int virtual_key_from_name(std::string_view raw)
    {
        std::string name = fold_case(raw);
        if (name.starts_with("key."))
        {
            name.erase(0, 4);
        }
        while (!name.empty() && name.back() == ' ')
        {
            name.pop_back();
        }
        if (name.empty())
        {
            return 0;
        }
        if (name[0] == 'f' && name.size() <= 3 && name.size() >= 2 && std::isdigit(static_cast<unsigned char>(name[1])))
        {
            const int n = static_cast<int>(to_int(name.substr(1)));
            return n >= 1 && n <= 24 ? 0x70 + n - 1 : 0;
        }
        if (name.starts_with("numpad") || name.starts_with("num_"))
        {
            const char d = name.back();
            return d >= '0' && d <= '9' ? 0x60 + (d - '0') : 0;
        }
        static const std::pair<const char*, int> named[] = {
            {"insert", 0x2D},     {"ins", 0x2D},      {"home", 0x24},     {"end", 0x23},      {"pageup", 0x21},    {"page_up", 0x21},
            {"pagedown", 0x22},   {"page_down", 0x22}, {"delete", 0x2E},  {"del", 0x2E},      {"pause", 0x13},     {"scrolllock", 0x91},
            {"scroll_lock", 0x91}, {"tilde", 0xC0},   {"backslash", 0xDC}, {"capslock", 0x14}, {"caps_lock", 0x14},
        };
        for (const auto& [n, vk] : named)
        {
            if (name == n)
            {
                return vk;
            }
        }
        if (name.size() == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9')))
        {
            return std::toupper(static_cast<unsigned char>(name[0]));
        }
        if (std::isdigit(static_cast<unsigned char>(name[0])))
        {
            const int n = static_cast<int>(to_int(name));
            return n > 0 && n < 256 ? n : 0;
        }
        return 0;
    }
} // namespace uuepbs::bridge
