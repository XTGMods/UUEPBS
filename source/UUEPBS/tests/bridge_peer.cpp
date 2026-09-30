// Stand-in for the DLL's side of the file bridge, built from the real protocol code.
//   bridge_peer <dir> <rescan> <pick> <pick_id> <refresh> [dll_session] [window open 0/1] [morphs "A=0.5,B=1" or -]
// Reads <dir>/bridge_in.txt, prints what it understood, answers every command and
// writes <dir>/bridge_out.txt.
#include "core/bridge.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace uuepbs::bridge;
int main(int argc, char** argv)
{
    if (argc < 6) return 2;
    const std::string dir = argv[1];
    std::ifstream in(dir + "/bridge_in.txt", std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    std::string why;
    auto st = parse_lua_state(ss.str(), &why);
    if (!st) { std::printf("invalid %s\n", why.c_str()); return 1; }
    std::printf("session %s seq %lld\n", st->session.c_str(), (long long)st->seq);
    std::printf("presets %s\nkey %s\ngame %s\nengine %s\n", st->setting("presets").c_str(), st->setting("key").c_str(), st->setting("game").c_str(), st->setting("engine").c_str());
    std::printf("target %s %s\n", st->target.id.c_str(), st->target.label.c_str());
    std::printf("candidates %zu\n", st->candidates.size());
    for (auto& r : st->rigs)
    {
        std::printf("rig %llX %s primary=%d owner=%s bones=%zu parents=%zu ref=%zu p5=%d n5=%s\n", (unsigned long long)r.address, r.label.c_str(), r.primary, r.owner.c_str(), r.names.size(), r.parents.size(), r.reference.size(), r.parents.size() > 5 ? r.parents[5] : -9, r.names.size() > 5 ? r.names[5].c_str() : "");
        if (!r.morphs.empty())
        {
            std::printf("morphs %s %zu", r.label.c_str(), r.morphs.size());
            for (auto& m : r.morphs) std::printf(" %s", m.c_str());
            std::printf("\n");
        }
    }
    if (!st->animated_morphs.empty())
    {
        std::printf("manim");
        for (auto& m : st->animated_morphs) std::printf(" %s", m.c_str());
        std::printf("\n");
    }
    DllState out;
    out.session = argc > 6 ? argv[6] : "dll1";
    out.lua_session = st->session;
    out.rescan = std::stoull(argv[2]); out.pick = std::stoull(argv[3]); out.pick_id = argv[4]; out.refresh = std::stoull(argv[5]);
    out.window_open = argc > 7 && std::string(argv[7]) == "1";
    out.hook_state = "live"; out.hook_text = "pose hook live on vtable 0x77ED320 slot 374";
    if (argc > 8 && std::string(argv[8]) != "-")
    {
        std::string spec = argv[8];
        size_t start = 0;
        while (start < spec.size())
        {
            size_t end = spec.find(',', start);
            if (end == std::string::npos) end = spec.size();
            const std::string item = spec.substr(start, end - start);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) out.morphs.emplace_back(item.substr(0, eq), std::stod(item.substr(eq + 1)));
            start = end + 1;
        }
        out.morph_revision = 7 + out.morphs.size();
    }
    for (auto& c : st->commands) {
        std::printf("cmd %lld %s [%s]\n", (long long)c.id, c.verb.c_str(), c.argument.c_str());
        out.ack = std::max(out.ack, c.id);
        out.replies.push_back({c.id, "done " + c.verb + (c.argument.empty() ? "" : " " + c.argument) + "\nsecond line"});
    }
    std::ofstream(dir + "/bridge_out.txt", std::ios::binary) << format_dll_state(out);
    return 0;
}
