// UUEPBS - file bridge between the Lua script and the DLL
//
// The DLL is loaded by the Lua script with package.loadlib(path, "*"), which
// maps it into the game without calling into it. UE4SS does not export the Lua C
// API, so instead of Lua bindings the two halves talk through two small text
// files next to the DLL. Nothing here depends on a UE4SS version.
//
//   bridge_in.txt   written by Lua, read by the DLL
//     UBS1 <session> <seq>
//     setup <tab> key=value <tab> key=value ...
//     target <tab> <id> <tab> <label>
//     cand <tab> <id> <tab> <label>                 (one per pickable character)
//     rig <tab> <address hex> <tab> <label> <tab> <primary 0/1> <tab> <owner>
//     bones <tab> <name> <tab> <name> ...
//     parents <tab> <index> <tab> <index> ...
//     ref <tab> <10 numbers per bone, space separated>   (optional: qx qy qz qw tx ty tz sx sy sz)
//     cmd <tab> <id> <tab> <verb> <tab> <argument>
//     #end <seq>
//
//   bridge_out.txt  written by the DLL (atomically replaced), read by Lua
//     UBS1 <dll session>
//     for <tab> <Lua session the ack and replies belong to>
//     ack <tab> <last command id handled>
//     rescan <tab> <counter>
//     pick <tab> <counter> <tab> <id>
//     refresh <tab> <counter>
//     hook <tab> <waiting|live|failed> <tab> <text>
//     ui <tab> <1 while the slider window is open, else 0>
//     reply <tab> <command id> <tab> <text, newlines as \n>
//     #end
//
// Lua writes its file in place (os.rename cannot replace a file on Windows), so the
// DLL only accepts it when the header and the trailer carry the same sequence number.
#pragma once

#include "xform.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uuepbs::bridge
{
    struct RigInfo
    {
        uintptr_t address{};
        std::string label;
        std::string owner;
        bool primary{};
        std::vector<std::string> names;
        std::vector<int32_t> parents;
        std::vector<Xform> reference; // empty when Lua could not read the reference pose
    };

    struct Choice
    {
        std::string id;
        std::string label;
    };

    struct Command
    {
        int64_t id{};
        std::string verb;
        std::string argument;
    };

    struct LuaState
    {
        std::string session;
        int64_t seq{};
        std::map<std::string, std::string> setup;
        Choice target;
        std::vector<Choice> candidates;
        std::vector<RigInfo> rigs;
        std::vector<Command> commands;

        std::string setting(const std::string& key, const std::string& fallback = {}) const
        {
            const auto it = setup.find(key);
            return it == setup.end() ? fallback : it->second;
        }
    };

    // nullopt when the text is incomplete (Lua still writing) or malformed; `why` says which.
    std::optional<LuaState> parse_lua_state(std::string_view text, std::string* why = nullptr);

    struct Reply
    {
        int64_t id{};
        std::string text;
    };

    struct DllState
    {
        std::string session;
        std::string lua_session;
        int64_t ack{};
        uint64_t rescan{};
        uint64_t pick{};
        std::string pick_id;
        uint64_t refresh{};
        std::string hook_state{"waiting"};
        std::string hook_text;
        bool window_open{};
        std::vector<Reply> replies;
    };

    std::string format_dll_state(const DllState& state);

    // Tabs and line breaks cannot appear inside a field.
    std::string clean_field(std::string_view text);
    std::string escape_lines(std::string_view text);

    // Parses virtual key names used in config.lua: "F6", "F1".."F24", "Insert", "Home", "End",
    // "PageUp", "PageDown", "Delete", "Pause", "ScrollLock", "NumpadN", single letters/digits,
    // or a number. 0 when unknown.
    int virtual_key_from_name(std::string_view name);
} // namespace uuepbs::bridge
