// UUEPBS - minimal JSON reader (presets, bone dictionary, game profiles).
// Reports line/column on errors, tolerates nothing exotic. No external dependency.
#pragma once

#include "sculpt.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uuepbs::json
{
    // ---- minimal JSON (only what presets need; no external dependency) ----
    struct JsonValue
    {
        enum class Kind
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        } kind{Kind::Null};
        bool boolean{};
        double number{};
        std::string text;
        std::vector<JsonValue> items;
        std::vector<std::pair<std::string, JsonValue>> members;

        const JsonValue* find(std::string_view key) const
        {
            for (const auto& [k, v] : members)
            {
                if (fold_case(k) == fold_case(key))
                {
                    return &v;
                }
            }
            return nullptr;
        }
    };

    class JsonReader
    {
      public:
        explicit JsonReader(std::string_view text) : m_text(text) {}

        bool read(JsonValue& out, std::string& error)
        {
            skip_bom();
            if (!value(out, 0))
            {
                error = describe();
                return false;
            }
            skip_space();
            if (m_pos != m_text.size())
            {
                m_error = "unexpected text after the end of the document";
                error = describe();
                return false;
            }
            return true;
        }

      private:
        std::string describe() const
        {
            size_t line = 1;
            size_t col = 1;
            for (size_t i = 0; i < m_pos && i < m_text.size(); ++i)
            {
                if (m_text[i] == '\n')
                {
                    ++line;
                    col = 1;
                }
                else
                {
                    ++col;
                }
            }
            return m_error + " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")";
        }

        bool fail(const char* why)
        {
            m_error = why;
            return false;
        }

        void skip_bom()
        {
            if (m_text.size() >= 3 && static_cast<unsigned char>(m_text[0]) == 0xEF && static_cast<unsigned char>(m_text[1]) == 0xBB &&
                static_cast<unsigned char>(m_text[2]) == 0xBF)
            {
                m_pos = 3;
            }
        }

        void skip_space()
        {
            while (m_pos < m_text.size())
            {
                const char c = m_text[m_pos];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    ++m_pos;
                }
                else
                {
                    break;
                }
            }
        }

        bool literal(std::string_view word)
        {
            if (m_text.substr(m_pos, word.size()) != word)
            {
                return fail("unknown word (expected true, false, null, a number, a string, [ or {)");
            }
            m_pos += word.size();
            return true;
        }

        static void append_utf8(std::string& s, uint32_t cp)
        {
            if (cp < 0x80)
            {
                s.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        bool hex4(uint32_t& out)
        {
            if (m_pos + 4 > m_text.size())
            {
                return fail("truncated \\u escape");
            }
            out = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char c = m_text[m_pos++];
                out <<= 4;
                if (c >= '0' && c <= '9')
                    out |= static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    out |= static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    out |= static_cast<uint32_t>(c - 'A' + 10);
                else
                    return fail("bad \\u escape");
            }
            return true;
        }

        bool string(std::string& out)
        {
            ++m_pos; // opening quote
            while (m_pos < m_text.size())
            {
                const char c = m_text[m_pos++];
                if (c == '"')
                {
                    return true;
                }
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    return fail("line break or control character inside a string");
                }
                if (c != '\\')
                {
                    out.push_back(c);
                    continue;
                }
                if (m_pos >= m_text.size())
                {
                    break;
                }
                const char e = m_text[m_pos++];
                switch (e)
                {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp))
                    {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && m_text.substr(m_pos, 2) == "\\u")
                    {
                        m_pos += 2;
                        uint32_t low = 0;
                        if (!hex4(low))
                        {
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return fail("unknown escape in string (use \\\\ for a backslash)");
                }
            }
            return fail("string is missing its closing quote");
        }

        bool number(double& out)
        {
            const size_t start = m_pos;
            while (m_pos < m_text.size())
            {
                const char c = m_text[m_pos];
                if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                {
                    ++m_pos;
                }
                else
                {
                    break;
                }
            }
            const std::string token(m_text.substr(start, m_pos - start));
            char* end = nullptr;
            out = std::strtod(token.c_str(), &end);
            if (token.empty() || !end || *end != '\0')
            {
                m_pos = start;
                return fail("malformed number (use a dot as decimal separator)");
            }
            return true;
        }

        bool value(JsonValue& out, int depth)
        {
            if (depth > 32)
            {
                return fail("nested too deeply");
            }
            skip_space();
            if (m_pos >= m_text.size())
            {
                return fail("unexpected end of file");
            }
            const char c = m_text[m_pos];
            if (c == '{')
            {
                out.kind = JsonValue::Kind::Object;
                ++m_pos;
                skip_space();
                if (m_pos < m_text.size() && m_text[m_pos] == '}')
                {
                    ++m_pos;
                    return true;
                }
                for (;;)
                {
                    skip_space();
                    if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                    {
                        return fail("expected a \"name\" in quotes");
                    }
                    std::string key;
                    if (!string(key))
                    {
                        return false;
                    }
                    skip_space();
                    if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                    {
                        return fail("expected ':' after a name");
                    }
                    ++m_pos;
                    JsonValue v;
                    if (!value(v, depth + 1))
                    {
                        return false;
                    }
                    out.members.emplace_back(std::move(key), std::move(v));
                    skip_space();
                    if (m_pos < m_text.size() && m_text[m_pos] == ',')
                    {
                        ++m_pos;
                        continue;
                    }
                    if (m_pos < m_text.size() && m_text[m_pos] == '}')
                    {
                        ++m_pos;
                        return true;
                    }
                    return fail("expected ',' or '}' (a comma may be missing)");
                }
            }
            if (c == '[')
            {
                out.kind = JsonValue::Kind::Array;
                ++m_pos;
                skip_space();
                if (m_pos < m_text.size() && m_text[m_pos] == ']')
                {
                    ++m_pos;
                    return true;
                }
                for (;;)
                {
                    JsonValue v;
                    if (!value(v, depth + 1))
                    {
                        return false;
                    }
                    out.items.push_back(std::move(v));
                    skip_space();
                    if (m_pos < m_text.size() && m_text[m_pos] == ',')
                    {
                        ++m_pos;
                        continue;
                    }
                    if (m_pos < m_text.size() && m_text[m_pos] == ']')
                    {
                        ++m_pos;
                        return true;
                    }
                    return fail("expected ',' or ']' (a comma may be missing)");
                }
            }
            if (c == '"')
            {
                out.kind = JsonValue::Kind::String;
                return string(out.text);
            }
            if (c == 't')
            {
                out.kind = JsonValue::Kind::Bool;
                out.boolean = true;
                return literal("true");
            }
            if (c == 'f')
            {
                out.kind = JsonValue::Kind::Bool;
                return literal("false");
            }
            if (c == 'n')
            {
                return literal("null");
            }
            out.kind = JsonValue::Kind::Number;
            return number(out.number);
        }

        std::string_view m_text;
        size_t m_pos{0};
        std::string m_error;
    };
} // namespace uuepbs::json
