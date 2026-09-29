-- UUEPBS - tiny JSON reader/writer for the game profile files.
-- decode(text) -> value | nil, "message (line L, column C)"
-- encode_profile(table, keyOrder) -> pretty JSON text (objects with string/number/bool/array values)

local M = {}

local function position(text, i)
    local line, col = 1, 1
    for k = 1, i - 1 do
        if text:sub(k, k) == "\n" then
            line, col = line + 1, 1
        else
            col = col + 1
        end
    end
    return string.format("line %d, column %d", line, col)
end

function M.decode(text)
    local i = 1
    local n = #text
    if text:sub(1, 3) == "\239\187\191" then -- UTF-8 BOM
        i = 4
    end

    local function fail(msg)
        error({ json = msg .. " (" .. position(text, i) .. ")" }, 0)
    end

    local function skip()
        while i <= n do
            local c = text:sub(i, i)
            if c == " " or c == "\t" or c == "\r" or c == "\n" then
                i = i + 1
            elseif text:sub(i, i + 1) == "//" then -- allow // comments, people hand-edit these
                local e = text:find("\n", i, true)
                i = e and e + 1 or n + 1
            else
                break
            end
        end
    end

    local value

    local function str()
        i = i + 1 -- opening quote
        local out = {}
        while true do
            if i > n then
                fail("unterminated string")
            end
            local c = text:sub(i, i)
            if c == '"' then
                i = i + 1
                return table.concat(out)
            elseif c == "\\" then
                local e = text:sub(i + 1, i + 1)
                local map = { ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }
                if map[e] then
                    out[#out + 1] = map[e]
                    i = i + 2
                elseif e == "u" then
                    local hex = text:sub(i + 2, i + 5)
                    local cp = tonumber(hex, 16)
                    if not cp then
                        fail("bad \\u escape")
                    end
                    out[#out + 1] = utf8 and utf8.char(cp) or "?"
                    i = i + 6
                else
                    fail("bad escape")
                end
            elseif c == "\n" then
                fail("line break inside a string (a closing quote may be missing)")
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
    end

    local function num()
        local s, e = text:find("^-?%d+%.?%d*[eE]?[-+]?%d*", i)
        if not s then
            fail("bad number")
        end
        local v = tonumber(text:sub(s, e))
        if not v then
            fail("bad number")
        end
        i = e + 1
        return v
    end

    local function obj()
        i = i + 1
        local t = {}
        skip()
        if text:sub(i, i) == "}" then
            i = i + 1
            return t
        end
        while true do
            skip()
            if text:sub(i, i) ~= '"' then
                fail("expected a \"name\" in quotes")
            end
            local k = str()
            skip()
            if text:sub(i, i) ~= ":" then
                fail("expected ':' after \"" .. k .. "\"")
            end
            i = i + 1
            skip()
            t[k] = value()
            skip()
            local c = text:sub(i, i)
            if c == "," then
                i = i + 1
                skip()
                if text:sub(i, i) == "}" then -- tolerate a trailing comma
                    i = i + 1
                    return t
                end
            elseif c == "}" then
                i = i + 1
                return t
            else
                fail("expected ',' or '}' (a comma may be missing)")
            end
        end
    end

    local function arr()
        i = i + 1
        local t = {}
        skip()
        if text:sub(i, i) == "]" then
            i = i + 1
            return t
        end
        while true do
            skip()
            t[#t + 1] = value()
            skip()
            local c = text:sub(i, i)
            if c == "," then
                i = i + 1
                skip()
                if text:sub(i, i) == "]" then
                    i = i + 1
                    return t
                end
            elseif c == "]" then
                i = i + 1
                return t
            else
                fail("expected ',' or ']'")
            end
        end
    end

    value = function()
        skip()
        local c = text:sub(i, i)
        if c == "{" then
            return obj()
        elseif c == "[" then
            return arr()
        elseif c == '"' then
            return str()
        elseif c == "-" or c:match("%d") then
            return num()
        elseif text:sub(i, i + 3) == "true" then
            i = i + 4
            return true
        elseif text:sub(i, i + 4) == "false" then
            i = i + 5
            return false
        elseif text:sub(i, i + 3) == "null" then
            i = i + 4
            return nil
        end
        fail("unexpected " .. (c == "" and "end of file" or "'" .. c .. "'"))
    end

    local ok, result = pcall(function()
        local v = value()
        skip()
        if i <= n then
            fail("unexpected text after the end")
        end
        return v
    end)
    if ok then
        return result
    end
    if type(result) == "table" and result.json then
        return nil, result.json
    end
    return nil, tostring(result)
end

local function quote(s)
    return '"' .. tostring(s):gsub('[%c"\\]', function(c)
        local map = { ['"'] = '\\"', ["\\"] = "\\\\", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }
        return map[c] or string.format("\\u%04x", c:byte())
    end) .. '"'
end

-- Marker for an empty JSON object ({}), since an empty Lua table would be written as [].
M.empty_object = setmetatable({}, { __uuepbs_object = true })

local function scalar(v)
    local mt = type(v) == "table" and getmetatable(v)
    if mt and mt.__uuepbs_object then
        return "{}"
    end
    if type(v) == "string" then
        return quote(v)
    elseif type(v) == "boolean" then
        return v and "true" or "false"
    elseif type(v) == "number" then
        return (v == math.floor(v) and string.format("%d", v)) or string.format("%.6g", v)
    elseif type(v) == "table" then
        local parts = {}
        for _, item in ipairs(v) do
            parts[#parts + 1] = scalar(item)
        end
        return "[" .. table.concat(parts, ", ") .. "]"
    end
    return "null"
end

-- One key per line, in `order` first, then any other keys alphabetically.
function M.encode_profile(t, order)
    local keys, seen = {}, {}
    for _, k in ipairs(order or {}) do
        if t[k] ~= nil then
            keys[#keys + 1] = k
            seen[k] = true
        end
    end
    local rest = {}
    for k in pairs(t) do
        if not seen[k] then
            rest[#rest + 1] = k
        end
    end
    table.sort(rest)
    for _, k in ipairs(rest) do
        keys[#keys + 1] = k
    end
    local lines = {}
    for idx, k in ipairs(keys) do
        lines[#lines + 1] = "  " .. quote(k) .. ": " .. scalar(t[k]) .. (idx < #keys and "," or "")
    end
    return "{\n" .. table.concat(lines, "\n") .. "\n}\n"
end

return M
