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
    // morph target names per rig and the "animated" list from Lua; weights back from the DLL
    auto mo = parse_lua_state("UBS1 s 4\nrig\t1000\tBody\t1\tBob\nbones\troot\nparents\t-1\nmorphs\tBreastSize\tBelly\t\nrig\t2000\tShirt\t0\tBob\nbones\troot\nparents\t-1\nmanim\tBelly\n#end 4\n");
    CHECK(mo && mo->rigs.size() == 2 && mo->rigs[0].morphs.size() == 2 && mo->rigs[0].morphs[1] == "Belly" && mo->rigs[1].morphs.empty());
    CHECK(mo && mo->animated_morphs.size() == 1 && mo->animated_morphs[0] == "Belly");
    DllState dm; dm.morph_revision = 12; dm.morphs = {{"BreastSize", 0.8, "player"}, {"Belly\tx", -0.25, "7FF6AB"}};
    dm.keeps = {{"player", "Roku"}, {"7FF6AB", "BP_Guard_C_3"}};
    const std::string mout = format_dll_state(dm);
    CHECK(mout.find("keep\tplayer\tRoku\nkeep\t7FF6AB\tBP_Guard_C_3\nmorph\t12\nmw\tBreastSize\t0.8000\tplayer\nmw\tBelly x\t-0.2500\t7FF6AB\n#end\n") != std::string::npos);
    // several characters: target / candidate / rig actor keys, and characters that are gone
    auto mc = parse_lua_state("UBS1 s 5\ntarget\t4D5E\tGuard\t4D5E\ncand\t1A2B\tRoku (player)\tplayer\ncand\t4D5E\tGuard\t4D5E\ncand\t9\tOld\n"
                              "rig\t1000\tBody\t1\tGuard\t4D5E\nbones\troot\nparents\t-1\nrig\t2000\tBody\t1\tRoku (player)\tplayer\nbones\troot\nparents\t-1\n"
                              "rig\t3000\tOld\t1\tOld\nbones\troot\nparents\t-1\ngone\t77AA\n#end 5\n");
    CHECK(mc && mc->target.key == "4D5E" && mc->candidates.size() == 3 && mc->candidates[0].key == "player" && mc->candidates[2].key == "9");
    CHECK(mc && mc->rigs[0].actor == "4D5E" && mc->rigs[1].actor == "player" && mc->rigs[2].actor == "player"); // old lines: the player
    CHECK(mc && mc->gone.size() == 1 && mc->gone[0] == "77AA");
    // remembered NPCs: identity on the target line, sightings from Lua, the remembered list from the DLL
    auto rn = parse_lua_state("UBS1 s 7\ntarget\t4D5E\tAnca_243\t4D5E\tAnca\nnpc\t77AA\tLacra\tLacra_9\nnpc\t\tbad\n#end 7\n");
    CHECK(rn && rn->target.identity == "Anca" && rn->npcs.size() == 1 && rn->npcs[0].key == "77AA" && rn->npcs[0].identity == "Lacra" && rn->npcs[0].label == "Lacra_9");
    DllState dr; dr.remembered = {"Anca", "BP_NPC_C@SK_Head_B"};
    CHECK(format_dll_state(dr).find("remember\tAnca\nremember\tBP_NPC_C@SK_Head_B\nmorph\t") != std::string::npos);
    auto old = parse_lua_state("UBS1 s 6\ntarget\tauto\tRoku\n#end 6\n");
    CHECK(old && old->target.key == "player");
    std::printf(fails ? "BRIDGE TESTS FAILED\n" : "bridge tests passed\n");
    return fails;
}
