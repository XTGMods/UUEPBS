// Protocol checks for the Lua <-> DLL file bridge.
//   test_bridge <bridge_in.txt written by the Lua harness>
#include "core/bridge.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace uuepbs::bridge;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
int main(int argc, char** argv)
{
    std::string text;
    if (argc > 1) { std::ifstream in(argv[1], std::ios::binary); std::stringstream ss; ss << in.rdbuf(); text = ss.str(); }
    if (!text.empty())
    {
        auto st = parse_lua_state(text);
        CHECK(st && !st->rigs.empty());
        // Every prefix of the file (Lua caught mid-write) must be rejected, never half-applied.
        int accepted = 0;
        for (size_t cut = 0; cut < text.size(); cut += 97) accepted += parse_lua_state(text.substr(0, cut)).has_value();
        CHECK(accepted == 0);
        // Header/trailer mismatch (a newer write over an older, longer file) is rejected too.
        std::string mixed = text;
        mixed.replace(mixed.find(' ', 5) + 1, 1, "9");
        CHECK(!parse_lua_state(mixed));
    }
    std::string why;
    CHECK(!parse_lua_state("UBS1 s 1\nrig\t1000\tMesh\t1\tA\nbones\ta\tb\nparents\t-1\n#end 1\n", &why));
    CHECK(why.find("mismatched") != std::string::npos);
    auto ok = parse_lua_state("UBS1 s 3\r\nsetup\tkey=Home\tpresets=C:\\x y\\UUEPBS Presets\r\nrig\t7FF612340000\tMesh\t1\tBob\r\nbones\tBip001 L Thigh\troot\r\nparents\t-1\t0\r\ncmd\t4\tLOAD\tMy Preset\r\n#end 3\r\n");
    CHECK(ok && ok->rigs[0].address == 0x7FF612340000ull && ok->rigs[0].names[0] == "Bip001 L Thigh" && ok->setting("presets") == "C:\\x y\\UUEPBS Presets");
    CHECK(ok && ok->commands.size() == 1 && ok->commands[0].verb == "load" && ok->commands[0].argument == "My Preset");
    CHECK(virtual_key_from_name("F6") == 0x75 && virtual_key_from_name("Key.F12") == 0x7B && virtual_key_from_name("Home") == 0x24 &&
          virtual_key_from_name("numpad5") == 0x65 && virtual_key_from_name("k") == 'K' && virtual_key_from_name("bogus") == 0);
    DllState d; d.session = "a"; d.lua_session = "b"; d.replies.push_back({7, "line1\nline\t2"});
    const std::string out = format_dll_state(d);
    CHECK(out.find("reply\t7\tline1\\nline 2\n") != std::string::npos && out.ends_with("#end\n") && out.find("for\tb\n") != std::string::npos);
    std::printf(fails ? "BRIDGE TESTS FAILED\n" : "bridge tests passed\n");
    return fails;
}
