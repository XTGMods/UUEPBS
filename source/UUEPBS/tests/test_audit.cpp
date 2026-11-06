// Whole-slider audit: every body group (and every single bone) x every kind of edit x every "Children" mode,
// on a character's main mesh and its other meshes (head, lashes...), with Auto Ground on and off.
//
//   test_audit <skeleton_cache.txt> <BoneDictionary.json> [main bones] [other bones...]
//   (no bone counts: every known character in the cache - 274 + 890 + 199 and 410 + 1115)
//
// Oracle: a mesh that copies the main mesh's final pose (Copy Pose) must end up exactly like the same mesh
// animated on its own and edited by us (own bones get the same slider semantics as on the body), and every
// bone it shares with the main mesh must sit exactly where the main mesh's bone is (no seams, no double edits).
// With ground on, every foot (and the lowest point of the foot: ball / toes) must stay on the game's ground.
#include "core/body_groups.hpp"
#include "core/sculpt.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace uuepbs;

struct Skel
{
    std::vector<std::string> names;
    std::vector<int32_t> parents;
    std::vector<Xform> local; // empty: no reference pose cached
};

static std::vector<std::string> split_tabs(const std::string& s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (true)
    {
        const size_t t = s.find('\t', at);
        out.push_back(s.substr(at, t == std::string::npos ? std::string::npos : t - at));
        if (t == std::string::npos)
        {
            break;
        }
        at = t + 1;
    }
    return out;
}

static std::map<size_t, Skel> load_all(const char* path)
{
    std::map<size_t, Skel> out;
    std::ifstream in(path, std::ios::binary);
    std::string line, names, parents;
    size_t count = 0;
    auto flush = [&]() {
        if (count && !names.empty() && !parents.empty() && !out.count(count))
        {
            Skel sk;
            sk.names = split_tabs(names);
            for (const auto& p : split_tabs(parents))
            {
                sk.parents.push_back(std::stoi(p));
            }
            if (sk.names.size() == count && sk.parents.size() == count)
            {
                out[count] = sk;
            }
        }
    };
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const size_t tab = line.find('\t');
        if (tab == std::string::npos)
        {
            continue;
        }
        const std::string kind = line.substr(0, tab), rest = line.substr(tab + 1);
        if (kind == "skel")
        {
            flush();
            names.clear();
            parents.clear();
            count = std::stoul(rest.substr(rest.rfind('#') + 1));
        }
        else if (kind == "names")
        {
            names = rest;
        }
        else if (kind == "parents")
        {
            parents = rest;
        }
        else if (kind == "ref")
        {
            flush();
            if (rest.size() > 2 && out.count(count) && out[count].local.empty())
            {
                std::istringstream nums(rest);
                Skel& sk = out[count];
                sk.local.assign(sk.names.size(), xf::identity());
                for (auto& x : sk.local)
                {
                    nums >> x.rot[0] >> x.rot[1] >> x.rot[2] >> x.rot[3] >> x.pos[0] >> x.pos[1] >> x.pos[2] >> x.scl[0] >> x.scl[1] >> x.scl[2];
                }
            }
        }
    }
    flush();
    return out;
}

static std::vector<Xform> component_pose(const Skel& sk)
{
    std::vector<Xform> out(sk.local.size());
    for (size_t i = 0; i < sk.local.size(); ++i)
    {
        out[i] = sk.parents[i] < 0 ? sk.local[i] : xf::compose(sk.local[i], out[sk.parents[i]]);
        out[i].pos[3] = out[i].scl[3] = 0.0;
    }
    return out;
}

static double pdist(const Xform& a, const Xform& b)
{
    const double x = a.pos[0] - b.pos[0], y = a.pos[1] - b.pos[1], z = a.pos[2] - b.pos[2];
    return std::sqrt(x * x + y * y + z * z);
}
static double rdeg(const Xform& a, const Xform& b)
{
    // angle of conj(a) * b, robust for nearly equal and not exactly normalised quaternions
    double ia[4], r[4];
    xf::qconj(a.rot, ia);
    xf::qmul(ia, b.rot, r);
    const double v = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    return 2.0 * std::atan2(v, std::fabs(r[3])) * 57.29577951308232;
}
static double sdiff(const Xform& a, const Xform& b)
{
    return std::max({std::fabs(a.scl[0] - b.scl[0]), std::fabs(a.scl[1] - b.scl[1]), std::fabs(a.scl[2] - b.scl[2])});
}
// one number: cm + degrees (x0.1: 1 degree ~ 1 mm at a 6 cm bone) + scale (x10: 1% ~ 1 mm)
static double gap(const Xform& a, const Xform& b)
{
    return pdist(a, b) + 0.1 * rdeg(a, b) + 10.0 * sdiff(a, b);
}

static void turn_local(Xform& x, double deg, int axis)
{
    const double h = deg * 3.14159265358979323846 / 360.0;
    double r[4] = {0, 0, 0, std::cos(h)};
    r[axis] = std::sin(h);
    double q[4];
    xf::qmul(x.rot, r, q);
    std::memcpy(x.rot, q, sizeof q);
}

// ---------------------------------------------------------------------------------------------------------------
struct Worst
{
    double v{0.0};
    std::string where;
    void take(double x, const std::string& w)
    {
        if (x > v)
        {
            v = x;
            where = w;
        }
    }
};

struct Character
{
    const char* label;
    Skel main;
    std::vector<Skel> others;
    std::vector<std::vector<Xform>> game_poses; // main mesh, component space
    std::vector<std::string> pose_names;
};

struct Totals
{
    Worst foot, sole, seam_copy, seam_own, copy_vs_own, double_edit;
    int cases = 0, wrong_state = 0;
    std::vector<std::string> notes;
};

// Secondary mesh input, from the main mesh's component pose: shared bones copied, own bones from a fixed local.
static std::vector<Xform> secondary_input(const Skel& sec, const std::vector<int32_t>& map, const std::vector<Xform>& main_pose)
{
    std::vector<Xform> out(sec.names.size(), xf::identity());
    for (size_t i = 0; i < sec.names.size(); ++i)
    {
        if (map[i] >= 0)
        {
            out[i] = main_pose[map[i]];
            continue;
        }
        Xform local = xf::identity();
        local.pos[0] = 1.5 + static_cast<double>(i % 5);
        local.pos[1] = 0.7 * static_cast<double>(i % 3) - 0.7;
        local.pos[2] = 0.4 * static_cast<double>(i % 4) - 0.6;
        turn_local(local, 7.0 * static_cast<double>(i % 7) - 21.0, static_cast<int>(i % 3));
        out[i] = sec.parents[i] < 0 ? local : xf::compose(local, out[sec.parents[i]]);
        out[i].pos[3] = out[i].scl[3] = 0.0;
    }
    return out;
}

static void run_case(const Character& ch, size_t pose_index, const EditBook& book, bool ground_on, const std::string& label, Totals& t)
{
    const std::vector<Xform>& game = ch.game_poses[pose_index];
    GroundOptions g;
    g.enabled = ground_on;
    g.leg_ik = true;

    RigSculptor main;
    main.set_skeleton(ch.main.names, ch.main.parents);
    main.set_ground(g);
    main.set_ground_primary(true);
    main.bind(book, 1);
    std::vector<Xform> body;
    for (int frame = 0; frame < 4; ++frame)
    {
        body = game;
        main.apply(body.data(), static_cast<int32_t>(body.size()));
    }
    ++t.cases;

    // ---- the main mesh on the ground
    if (ground_on && main.has_legs() && std::fabs(main.last_lift()) < g.max - 1e-6)
    {
        for (const LegChain& leg : main.legs())
        {
            // the sole: ball bone and a heel point the ball's height under the ankle, carried by the foot
            double off = body[leg.foot].pos[2] - game[leg.foot].pos[2];
            if (leg.ball >= 0)
            {
                const double h = game[leg.foot].pos[2] - game[leg.ball].pos[2];
                Xform heel_local = xf::identity();
                Xform heel_game = game[leg.foot];
                heel_game.pos[2] -= h;
                heel_local = xf::relative(heel_game, game[leg.foot]);
                const Xform heel_now = xf::compose(heel_local, body[leg.foot]);
                off = std::min(body[leg.ball].pos[2] - game[leg.ball].pos[2], heel_now.pos[2] - heel_game.pos[2]);
            }
            t.foot.take(std::fabs(off), label + " " + ch.main.names[leg.foot]);
            if (std::getenv("AUDIT_FEET") && std::fabs(off) > 0.05)
            {
                std::printf("  feet: %-50s %-8s %+7.2f cm (lift %.2f)\n", label.c_str(), ch.main.names[leg.foot].c_str(), off, main.last_lift());
            }
            // lowest point of the foot (foot, ball, toes) against the game's
            double lo_game = 1e30, lo_now = 1e30;
            for (size_t i = leg.foot; i < ch.main.names.size(); ++i)
            {
                bool below = static_cast<int32_t>(i) == leg.foot;
                for (int32_t p = ch.main.parents[i]; p >= 0 && !below; p = ch.main.parents[p])
                {
                    below = p == leg.foot;
                }
                if (below)
                {
                    lo_game = std::min(lo_game, game[i].pos[2]);
                    lo_now = std::min(lo_now, body[i].pos[2]);
                }
            }
            t.sole.take(std::fabs(lo_now - lo_game), label + " " + ch.main.names[leg.foot] + (lo_now < lo_game ? " (sinks)" : " (floats)"));
        }
    }

    for (const Skel& sec : ch.others)
    {
        std::vector<int32_t> map(sec.names.size(), -1);
        for (size_t i = 0; i < sec.names.size(); ++i)
        {
            map[i] = main.find(sec.names[i]);
        }
        std::string last_report;
        auto run = [&](const std::vector<Xform>& input, std::vector<Xform>& out) {
            RigSculptor s;
            s.set_skeleton(sec.names, sec.parents);
            s.set_ground(g);
            s.set_ground_primary(false);
            s.bind(book, 1);
            s.set_shared_lift(main.last_lift());
            s.set_follow_reference(map.data(), main.published_before().data(), main.published_after().data(), ch.main.names.size());
            for (int frame = 0; frame < 4; ++frame)
            {
                out = input;
                s.apply(out.data(), static_cast<int32_t>(out.size()));
            }
            last_report = s.ground_report();
            return s.copy_state();
        };
        const std::string where = label + " [" + std::to_string(sec.names.size()) + "]";

        // (1) animated on its own (same animation as the body): edited by us
        std::vector<Xform> own;
        const std::vector<Xform> own_in = secondary_input(sec, map, game);
        const int own_state = run(own_in, own);
        // (2) Copy Pose from the body's final pose
        std::vector<Xform> copy;
        const std::vector<Xform> copy_in = secondary_input(sec, map, body);
        const std::string own_report = last_report;
        const int copy_state = run(copy_in, copy);

        // did anything shared move at all? (else the state may legitimately stay unknown)
        bool moved = false;
        for (size_t i = 0; i < sec.names.size() && !moved; ++i)
        {
            moved = map[i] >= 0 && (pdist(body[map[i]], game[map[i]]) > 0.05 || sdiff(body[map[i]], game[map[i]]) > 0.002 ||
                                    rdeg(body[map[i]], game[map[i]]) > 0.1);
        }
        if (moved && (copy_state != 1 || own_state == 1))
        {
            ++t.wrong_state;
            if (t.notes.size() < 40)
            {
                t.notes.push_back(where + ": copy state " + std::to_string(copy_state) + " / own anim state " + std::to_string(own_state));
            }
        }

        if (const char* want = std::getenv("AUDIT_CASE"); want && where.find(want) != std::string::npos)
        {
            std::printf("--- %s: lift %.3f, copy state %d, own state %d\n  own: %s\n  copy: %s\n", where.c_str(), main.last_lift(), copy_state,
                        own_state, own_report.c_str(), last_report.c_str());
            std::vector<std::pair<double, size_t>> order;
            for (size_t i = 0; i < sec.names.size(); ++i)
            {
                order.push_back({std::max(gap(copy[i], own[i]), map[i] >= 0 ? gap(own[i], body[map[i]]) : 0.0), i});
            }
            std::sort(order.rbegin(), order.rend());
            for (int32_t b = static_cast<int32_t>(order[0].second); b >= 0; b = sec.parents[b])
            {
                std::printf("   chain %-30s %s copy-own %.5f (pos %.5f rot %.4f scl %.6f) scale %.3f %.3f %.3f in %.3f %.3f %.3f\n", sec.names[b].c_str(),
                            map[b] >= 0 ? "S" : "o", gap(copy[b], own[b]), pdist(copy[b], own[b]), rdeg(copy[b], own[b]), sdiff(copy[b], own[b]),
                            own[b].scl[0], own[b].scl[1], own[b].scl[2], own_in[b].scl[0], own_in[b].scl[1], own_in[b].scl[2]);
            }
            for (size_t k = 0; k < std::min<size_t>(12, order.size()); ++k)
            {
                const size_t i = order[k].second;
                const int32_t p = sec.parents[i];
                std::printf("   %-34s parent %-24s %s  copy-own %.4f  own-body %.4f\n", sec.names[i].c_str(), p >= 0 ? sec.names[p].c_str() : "-",
                            map[i] >= 0 ? "shared" : "own   ", gap(copy[i], own[i]), map[i] >= 0 ? gap(own[i], body[map[i]]) : 0.0);
            }
        }
        for (size_t i = 0; i < sec.names.size(); ++i)
        {
            const std::string bone = where + " " + sec.names[i];
            if (map[i] >= 0)
            {
                t.seam_copy.take(gap(copy[i], body[map[i]]), bone);
                t.seam_own.take(gap(own[i], body[map[i]]), bone);
            }
            t.copy_vs_own.take(gap(copy[i], own[i]), bone);
            // an edit counted twice: the copy moved further from its input than the body moved from the game
            if (map[i] >= 0)
            {
                t.double_edit.take(gap(copy[i], copy_in[i]), bone);
            }
        }
    }
}

static EditBook book_for(const std::vector<std::string>& bones, const BoneEdit& e)
{
    EditBook b;
    for (const std::string& n : bones)
    {
        EditEntry x;
        x.bone = n;
        x.edit = e;
        b[fold_case(n)] = x;
    }
    return b;
}

static void report(const char* title, const Totals& t)
{
    std::printf("\n== %s: %d cases\n", title, t.cases);
    auto line = [](const char* what, const Worst& w, double limit) {
        std::printf("  %-44s %10.5f  %s  %s\n", what, w.v, w.v <= limit ? "ok  " : "FAIL", w.where.c_str());
        return w.v <= limit;
    };
    bool ok = true;
    ok &= line("sole (ball / heel) off the game's ground (cm)", t.foot, 0.01);
    line("lowest foot bone vs game (info, cm)", t.sole, 1e9);
    // 0.01 cm = 0.1 mm. An edit that moves nothing by more than 0.05 cm gives the copy detection nothing to go by,
    // so a mesh animated on its own may miss such a sub-millimetre lift.
    ok &= line("copy-pose mesh vs body, shared bones", t.seam_copy, 0.01);
    ok &= line("own-anim mesh vs body, shared bones", t.seam_own, 0.05);
    ok &= line("copy-pose mesh vs own-anim mesh, all bones", t.copy_vs_own, 0.05);
    ok &= line("copy-pose mesh: shared bones changed by us", t.double_edit, 0.01);
    std::printf("  copy detection wrong: %d\n", t.wrong_state);
    for (const auto& n : t.notes)
    {
        std::printf("    %s\n", n.c_str());
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: test_audit <skeleton_cache.txt> <BoneDictionary.json> [main bones] [other bones...]\n");
        return 2;
    }
    const auto skels = load_all(argv[1]);
    std::ifstream df(argv[2], std::ios::binary);
    std::string dtext((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
    BoneDictionary dict;
    std::string msg;
    if (!BoneDictionary::parse(dtext, dict, msg))
    {
        std::printf("dictionary: %s\n", msg.c_str());
        return 2;
    }

    std::vector<std::vector<size_t>> sets;
    if (argc > 3)
    {
        std::vector<size_t> s;
        for (int i = 3; i < argc; ++i)
        {
            s.push_back(std::stoul(argv[i]));
        }
        sets.push_back(s);
    }
    else
    {
        sets = {{274, 890, 199}, {410, 1115}};
    }

    int failed_sets = 0;
    for (const auto& set : sets)
    {
        if (!skels.count(set[0]) || skels.at(set[0]).local.empty())
        {
            std::printf("no %zu-bone main skeleton with a reference pose\n", set[0]);
            continue;
        }
        Character ch;
        ch.label = "";
        ch.main = skels.at(set[0]);
        for (size_t k = 1; k < set.size(); ++k)
        {
            if (skels.count(set[k]))
            {
                ch.others.push_back(skels.at(set[k]));
            }
        }
        std::printf("\n######## main mesh %zu bones, other meshes:", set[0]);
        for (const Skel& s : ch.others)
        {
            size_t shared = 0, own = 0;
            for (const auto& n : s.names)
            {
                bool hit = false;
                for (const auto& m : ch.main.names)
                {
                    if (fold_case(m) == fold_case(n))
                    {
                        hit = true;
                        break;
                    }
                }
                (hit ? shared : own)++;
            }
            std::printf(" %zu (%zu shared, %zu own)", s.names.size(), shared, own);
        }
        std::printf("\n");

        // game poses: standing, one foot up a step (thigh and calf turned the way that lifts the foot most)
        ch.game_poses.push_back(component_pose(ch.main));
        ch.pose_names.push_back("stand");
        const auto legs = find_legs(ch.main.names, ch.main.parents);
        if (!legs.empty())
        {
            // a stair step: hip flexed 35, knee bent 60 (about the same axis, opposite ways), foot ~20 cm up
            Skel step = ch.main;
            const double z0 = ch.game_poses[0][legs[0].foot].pos[2];
            double best = 1e30;
            Xform best_t = step.local[legs[0].thigh], best_c = step.local[legs[0].calf];
            for (int axis = 0; axis < 3; ++axis)
            {
                for (double sign : {1.0, -1.0})
                {
                    Skel s = ch.main;
                    turn_local(s.local[legs[0].thigh], sign * 35.0, axis);
                    turn_local(s.local[legs[0].calf], -sign * 60.0, axis);
                    const auto cp = component_pose(s);
                    const double rise = cp[legs[0].foot].pos[2] - z0;
                    const double knee_up = cp[legs[0].calf].pos[2] - ch.game_poses[0][legs[0].calf].pos[2];
                    if (knee_up > 0 && std::fabs(rise - 20.0) < best)
                    {
                        best = std::fabs(rise - 20.0);
                        best_t = s.local[legs[0].thigh];
                        best_c = s.local[legs[0].calf];
                    }
                }
            }
            step.local[legs[0].thigh] = best_t;
            step.local[legs[0].calf] = best_c;
            ch.game_poses.push_back(component_pose(step));
            ch.pose_names.push_back("step");
            std::printf("legs:");
            for (const auto& l : legs)
            {
                std::printf(" %s>%s>%s", ch.main.names[l.thigh].c_str(), ch.main.names[l.calf].c_str(), ch.main.names[l.foot].c_str());
                const auto& rp = ch.game_poses[0];
                std::printf(" (ref z: root %.2f foot %.2f", rp[0].pos[2], rp[l.foot].pos[2]);
                for (size_t i = l.foot + 1; i < rp.size(); ++i)
                {
                    if (ch.main.parents[i] == l.foot || (ch.main.parents[i] >= 0 && ch.main.parents[ch.main.parents[i]] == l.foot))
                    {
                        std::printf(" %s %.2f", ch.main.names[i].c_str(), rp[i].pos[2]);
                    }
                }
                std::printf(")\n     ");
            }
            std::printf(" | step pose lifts %s by %.1f cm\n", ch.main.names[legs[0].foot].c_str(),
                        ch.game_poses[1][legs[0].foot].pos[2] - ch.game_poses[0][legs[0].foot].pos[2]);
        }

        // edits
        struct Variant
        {
            const char* name;
            BoneEdit e;
        };
        std::vector<Variant> variants;
        auto add = [&](const char* name, std::initializer_list<double> axis, std::initializer_list<double> turn, std::initializer_list<double> shift) {
            Variant v{name, {}};
            std::copy(axis.begin(), axis.end(), v.e.axis);
            std::copy(turn.begin(), turn.end(), v.e.turn);
            std::copy(shift.begin(), shift.end(), v.e.shift);
            variants.push_back(v);
        };
        add("length 1.3", {1.3, 1, 1}, {0, 0, 0}, {0, 0, 0});
        add("length 0.75", {0.75, 1, 1}, {0, 0, 0}, {0, 0, 0});
        add("width 1.3", {1, 1.3, 1.3}, {0, 0, 0}, {0, 0, 0});
        add("width 0.75", {1, 0.75, 0.75}, {0, 0, 0}, {0, 0, 0});
        add("size 1.2", {1.2, 1.2, 1.2}, {0, 0, 0}, {0, 0, 0});
        add("turn X 15", {1, 1, 1}, {15, 0, 0}, {0, 0, 0});
        add("turn Y 15", {1, 1, 1}, {0, 15, 0}, {0, 0, 0});
        add("turn Z 15", {1, 1, 1}, {0, 0, 15}, {0, 0, 0});
        add("shift X 3", {1, 1, 1}, {0, 0, 0}, {3, 0, 0});
        add("shift Y 3", {1, 1, 1}, {0, 0, 0}, {0, 3, 0});
        add("shift Z 3", {1, 1, 1}, {0, 0, 0}, {0, 0, 3});
        add("mixed", {1.15, 0.9, 1.1}, {5, -8, 4}, {1, -1, 0.5});
        const Spread spreads[3] = {Spread::Chain, Spread::Keep, Spread::Solo};

        const BodyMap bm = resolve_body_groups(ch.main.names, ch.main.parents, dict);
        Totals groups_on, groups_off, bones_on, combo;
        std::map<std::string, Totals> per_group;
        for (const GroupBones& gb : bm.groups)
        {
            std::string list;
            for (const auto& b : gb.bones)
            {
                list += " " + b;
            }
            std::printf("group %-14s [%s]%s\n", gb.group.title.c_str(), spread_token(gb.group.spread), list.c_str());
            for (const Variant& v : variants)
            {
                for (Spread sp : spreads)
                {
                    BoneEdit e = v.e;
                    e.spread = sp;
                    const EditBook book = book_for(gb.bones, e);
                    for (size_t p = 0; p < ch.game_poses.size(); ++p)
                    {
                        const std::string label = gb.group.title + " " + v.name + " " + spread_token(sp) + " " + ch.pose_names[p];
                        run_case(ch, p, book, true, label, groups_on);
                        run_case(ch, p, book, true, label, per_group[gb.group.title]);
                        run_case(ch, p, book, false, label + " (ground off)", groups_off);
                    }
                }
            }
        }
        // every bone on its own (Detailed Panel), three kinds of edits
        for (size_t b = 0; b < ch.main.names.size(); ++b)
        {
            for (int k = 0; k < 3; ++k)
            {
                BoneEdit e;
                const char* kind = "";
                if (k == 0)
                {
                    e.axis[0] = e.axis[1] = e.axis[2] = 1.25;
                    e.spread = Spread::Keep;
                    kind = "size 1.25 keep";
                }
                else if (k == 1)
                {
                    e.turn[1] = 15;
                    e.turn[2] = 10;
                    e.spread = Spread::Chain;
                    kind = "turn chain";
                }
                else
                {
                    e.axis[0] = 1.3;
                    e.shift[2] = 2;
                    e.spread = Spread::Solo;
                    kind = "length+shift solo";
                }
                run_case(ch, 0, book_for({ch.main.names[b]}, e), true, ch.main.names[b] + " " + kind, bones_on);
            }
        }
        // the whole panel at once (a typical preset): every group moved
        {
            EditBook all;
            int k = 0;
            for (const GroupBones& gb : bm.groups)
            {
                BoneEdit e;
                e.axis[0] = 1.0 + 0.04 * ((k % 5) - 2);
                e.axis[1] = e.axis[2] = 1.0 + 0.03 * ((k % 7) - 3);
                e.spread = gb.group.spread;
                ++k;
                for (const auto& [key, entry] : book_for(gb.bones, e))
                {
                    all[key] = entry;
                }
            }
            for (size_t p = 0; p < ch.game_poses.size(); ++p)
            {
                run_case(ch, p, all, true, std::string("all groups ") + ch.pose_names[p], combo);
                run_case(ch, p, all, false, std::string("all groups ground off ") + ch.pose_names[p], combo);
            }
        }

        // fast motion, and the other mesh runs before the main mesh: it compares with the main mesh's previous frame
        for (const Skel& sec : ch.others)
        {
            EditBook book;
            for (const GroupBones& gb : bm.groups)
            {
                if (gb.group.title == "Calves" || gb.group.title == "Rib cage" || gb.group.title == "Head")
                {
                    BoneEdit e;
                    e.axis[0] = gb.group.title == "Calves" ? 1.3 : 0.85;
                    e.axis[1] = e.axis[2] = 0.9;
                    e.turn[1] = gb.group.title == "Head" ? 10.0 : 0.0;
                    e.spread = gb.group.spread;
                    for (const auto& [k, v] : book_for(gb.bones, e))
                    {
                        book[k] = v;
                    }
                }
            }
            GroundOptions g;
            g.enabled = true;
            RigSculptor main, s;
            main.set_skeleton(ch.main.names, ch.main.parents);
            main.set_ground(g);
            main.bind(book, 1);
            s.set_skeleton(sec.names, sec.parents);
            s.set_ground(g);
            s.set_ground_primary(false);
            s.bind(book, 1);
            std::vector<int32_t> map(sec.names.size(), -1);
            for (size_t i = 0; i < sec.names.size(); ++i)
            {
                map[i] = main.find(sec.names[i]);
            }
            std::vector<double> prev_before, prev_after;
            double worst = 0.0;
            int bad_frames = 0;
            const int32_t pelvis = main.find("pelvis");
            for (int frame = 0; frame < 120; ++frame)
            {
                Skel moving = ch.main;
                // sprinting / attacking: the body bobs 6 cm and sways, the legs swing (component space: no travel)
                if (pelvis >= 0)
                {
                    moving.local[pelvis].pos[2] += 6.0 * std::sin(frame * 0.7);
                    turn_local(moving.local[pelvis], 12.0 * std::sin(frame * 0.35), 0);
                }
                if (!legs.empty())
                {
                    turn_local(moving.local[legs[0].thigh], 30.0 * std::sin(frame * 0.7), 1);
                    turn_local(moving.local[legs[0].calf], -25.0 - 25.0 * std::sin(frame * 0.7), 1);
                }
                const std::vector<Xform> game = component_pose(moving);
                std::vector<Xform> body = game;
                main.apply(body.data(), static_cast<int32_t>(body.size()));
                if (!prev_before.empty())
                {
                    s.set_follow_reference(map.data(), prev_before.data(), prev_after.data(), ch.main.names.size());
                    std::vector<Xform> in = secondary_input(sec, map, body), out = in;
                    s.apply(out.data(), static_cast<int32_t>(out.size()));
                    double w = 0.0;
                    for (size_t i = 0; i < sec.names.size(); ++i)
                    {
                        if (map[i] >= 0)
                        {
                            w = std::max(w, gap(out[i], body[map[i]]));
                        }
                    }
                    if (frame > 10)
                    {
                        worst = std::max(worst, w);
                        bad_frames += w > 0.01 ? 1 : 0;
                    }
                }
                prev_before = main.published_before();
                prev_after = main.published_after();
            }
            std::printf("fast motion, %zu-bone mesh a frame behind: copy state %d, worst seam %.5f cm, %d bad frame(s) %s\n", sec.names.size(),
                        s.copy_state(), worst, bad_frames, bad_frames ? "FAIL" : "ok");
            if (bad_frames)
            {
                ++failed_sets;
            }
        }

        std::printf("\nper group (ground on): worst seam copy | own | copy-vs-own | feet | lowest foot point\n");
        for (const auto& [title, t] : per_group)
        {
            std::printf("  %-14s %9.5f %9.5f %9.5f %9.5f %9.5f %s\n", title.c_str(), t.seam_copy.v, t.seam_own.v, t.copy_vs_own.v, t.foot.v, t.sole.v,
                        t.wrong_state ? "copy detection wrong!" : "");
        }
        report("body groups, ground on", groups_on);
        report("body groups, ground off", groups_off);
        report("every single bone, ground on", bones_on);
        report("all groups at once", combo);
        for (const Totals* t : {&groups_on, &groups_off, &bones_on, &combo})
        {
            if (t->seam_copy.v > 0.01 || t->double_edit.v > 0.01 || t->seam_own.v > 0.05 || t->copy_vs_own.v > 0.05 || t->foot.v > 0.01 ||
                t->wrong_state)
            {
                ++failed_sets;
                break;
            }
        }
    }
    std::printf(failed_sets ? "\nAUDIT: problems found\n" : "\nAUDIT: all clean\n");
    return failed_sets ? 1 : 0;
}
