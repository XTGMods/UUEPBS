// Auto Ground: lift + two-bone leg IK on a real skeleton.
//
//   test_ground <skeleton_cache.txt> [bone count to pick, default 410]
//
// Builds the reference pose of a skeleton from UUEPBS's skeleton cache, bends it into "game poses"
// (one foot up on a step, both knees bent), edits the legs and checks that after apply():
//   * every foot is back where the game put it (height and place), with the game's rotation;
//   * the IK'd leg keeps its edited bone lengths and bends the same way as before;
//   * meshes without legs get the shared lift; ground off leaves the old behaviour untouched;
//   * applying twice on a buffer the engine did not refresh gives the same result.
#include "core/sculpt.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace uuepbs;

static int g_fail = 0;
#define CHECK(cond, ...)                                                                                                                             \
    do                                                                                                                                               \
    {                                                                                                                                                \
        if (!(cond))                                                                                                                                 \
        {                                                                                                                                            \
            ++g_fail;                                                                                                                                \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                                         \
            std::printf(__VA_ARGS__);                                                                                                                \
            std::printf("\n");                                                                                                                       \
        }                                                                                                                                            \
    } while (0)

struct Skel
{
    std::vector<std::string> names;
    std::vector<int32_t> parents;
    std::vector<Xform> local;
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

static bool load(const char* path, size_t want, Skel& sk)
{
    std::ifstream in(path, std::ios::binary);
    std::string line, names, parents;
    bool pick = false;
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
            pick = rest.rfind('#') != std::string::npos && rest.substr(rest.rfind('#') + 1) == std::to_string(want);
        }
        else if (pick && kind == "names")
        {
            names = rest;
        }
        else if (pick && kind == "parents")
        {
            parents = rest;
        }
        else if (pick && kind == "ref" && rest.size() > 2)
        {
            sk.names = split_tabs(names);
            for (const auto& p : split_tabs(parents))
            {
                sk.parents.push_back(std::stoi(p));
            }
            std::istringstream nums(rest);
            sk.local.assign(sk.names.size(), xf::identity());
            for (auto& x : sk.local)
            {
                nums >> x.rot[0] >> x.rot[1] >> x.rot[2] >> x.rot[3] >> x.pos[0] >> x.pos[1] >> x.pos[2] >> x.scl[0] >> x.scl[1] >> x.scl[2];
            }
            return sk.names.size() == want && sk.parents.size() == want;
        }
    }
    return false;
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

static int32_t find(const Skel& sk, const char* name)
{
    for (size_t i = 0; i < sk.names.size(); ++i)
    {
        if (fold_case(sk.names[i]) == name)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// local rotation about the bone's own axis
static void bend(Skel& sk, const char* bone, double deg, int axis)
{
    const int32_t i = find(sk, bone);
    const double h = deg * 3.14159265358979323846 / 360.0;
    double r[4] = {0, 0, 0, std::cos(h)};
    r[axis] = std::sin(h);
    double q[4];
    xf::qmul(sk.local[i].rot, r, q);
    for (int a = 0; a < 4; ++a)
    {
        sk.local[i].rot[a] = q[a];
    }
}

static double dist(const Xform& a, const Xform& b)
{
    const double x = a.pos[0] - b.pos[0], y = a.pos[1] - b.pos[1], z = a.pos[2] - b.pos[2];
    return std::sqrt(x * x + y * y + z * z);
}

static double qdiff(const Xform& a, const Xform& b)
{
    const double d = std::fabs(a.rot[0] * b.rot[0] + a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3]);
    return 1.0 - std::min(1.0, d);
}

static EditBook leg_book()
{
    EditBook book;
    auto add = [&](const char* bone, double len, double wd, Spread sp) {
        EditEntry e;
        e.bone = bone;
        e.edit.axis[0] = len;
        e.edit.axis[1] = e.edit.axis[2] = wd;
        e.edit.spread = sp;
        book[fold_case(bone)] = e;
    };
    // the user's session: calves 1.29 long, a smaller torso and head
    add("calf_l", 1.2923, 1.16, Spread::Keep);
    add("calf_r", 1.2923, 1.16, Spread::Keep);
    add("thigh_l", 1.10, 1.0, Spread::Keep);
    add("thigh_r", 1.10, 1.0, Spread::Keep);
    add("spine_01", 0.891, 0.891, Spread::Keep);
    add("spine_02", 0.804, 0.804, Spread::Keep);
    add("Head", 0.905, 0.905, Spread::Chain);
    return book;
}

static void scenario(const char* label, const Skel& game_skel, bool ik)
{
    const std::vector<Xform> game = component_pose(game_skel);
    RigSculptor s;
    std::string why;
    CHECK(s.set_skeleton(game_skel.names, game_skel.parents, &why), "set_skeleton: %s", why.c_str());
    CHECK(s.legs().size() == 2, "%s: found %zu legs", label, s.legs().size());
    GroundOptions g;
    g.enabled = true;
    g.leg_ik = ik;
    s.set_ground(g);
    s.bind(leg_book(), 1);

    // edit without ground, to know the plain result
    RigSculptor plain;
    plain.set_skeleton(game_skel.names, game_skel.parents);
    plain.bind(leg_book(), 1);
    std::vector<Xform> edited = game;
    plain.apply(edited.data(), static_cast<int32_t>(edited.size()));

    std::vector<Xform> pose = game;
    s.apply(pose.data(), static_cast<int32_t>(pose.size()));
    const double lift = s.last_lift();

    std::printf("%-36s lift %6.2f cm", label, lift);
    for (const LegChain& leg : s.legs())
    {
        const double sank = game[leg.foot].pos[2] - edited[leg.foot].pos[2];
        const double off = pose[leg.foot].pos[2] - game[leg.foot].pos[2];
        std::printf(" | %s sank %5.2f -> %+6.3f cm", game_skel.names[leg.foot].c_str(), sank, off);
        if (ik)
        {
            // back on the ground, close to where the game put it, with the game's rotation
            CHECK(std::fabs(off) < 0.01, "%s: %s is %.3f cm off the ground", label, game_skel.names[leg.foot].c_str(), off);
            // (the cached reference quaternions are only float-normalised: 1 - |q.q| is ~6e-8 by itself)
            CHECK(qdiff(pose[leg.foot], edited[leg.foot]) < 1e-6, "%s: foot rotation %.3g away from the edited pose (game %.3g)", label,
                  qdiff(pose[leg.foot], edited[leg.foot]), qdiff(pose[leg.foot], game[leg.foot]));
            // bone lengths as edited
            CHECK(std::fabs(dist(pose[leg.calf], pose[leg.thigh]) - dist(edited[leg.calf], edited[leg.thigh])) < 1e-6, "%s: thigh length", label);
            CHECK(std::fabs(dist(pose[leg.foot], pose[leg.calf]) - dist(edited[leg.foot], edited[leg.calf])) < 1e-6, "%s: calf length", label);
        }
    }
    std::printf("\n");
    const int32_t head = find(game_skel, "head");
    CHECK(std::fabs(pose[head].pos[2] - edited[head].pos[2] - lift) < 1e-6, "%s: head not lifted with the body", label);

    // the engine did not refresh the buffer: same result again
    const std::vector<Xform> first = pose;
    s.apply(pose.data(), static_cast<int32_t>(pose.size())); // same buffer, still holding our result
    const std::vector<Xform>& again = first;
    double worst = 0.0;
    for (size_t i = 0; i < pose.size(); ++i)
    {
        worst = std::max(worst, dist(again[i], pose[i]));
    }
    CHECK(worst < 1e-9, "%s: second apply moved bones by %.3g cm", label, worst);

    // ---- the character's other meshes (the registry hands them the main mesh's published pose) ----
    // Runs a secondary mesh for a few frames (the copy decision needs three in a row) on `input`.
    struct Secondary
    {
        RigSculptor rig;
        std::vector<int32_t> map;
        std::vector<Xform> out;
    };
    auto run_secondary = [&](Secondary& sec, const std::vector<std::string>& names, const std::vector<int32_t>& parents,
                             const std::vector<Xform>& input, const EditBook& book) {
        sec.rig.set_skeleton(names, parents);
        sec.rig.set_ground_primary(false);
        sec.rig.set_ground(g);
        sec.rig.bind(book, 1);
        sec.rig.set_shared_lift(lift);
        sec.map.assign(names.size(), -1);
        for (size_t i = 0; i < names.size(); ++i)
        {
            sec.map[i] = s.find(names[i]);
        }
        sec.rig.set_follow_reference(sec.map.data(), s.published_before().data(), s.published_after().data(), game_skel.names.size());
        for (int frame = 0; frame < 4; ++frame)
        {
            sec.out = input; // the engine writes a fresh pose every frame
            sec.rig.apply(sec.out.data(), static_cast<int32_t>(sec.out.size()));
        }
    };
    auto worst_shared = [&](const Secondary& sec) {
        double w = 0.0;
        for (size_t i = 0; i < sec.map.size(); ++i)
        {
            if (sec.map[i] >= 0)
            {
                w = std::max(w, dist(sec.out[i], first[sec.map[i]]));
            }
        }
        return w;
    };
    const int32_t neck = find(game_skel, "neck_01");

    // head-only mesh (no legs), plus one bone of its own the body does not have, with its own slider
    std::vector<std::string> names(game_skel.names.begin(), game_skel.names.begin() + head + 1);
    std::vector<int32_t> parents(game_skel.parents.begin(), game_skel.parents.begin() + head + 1);
    names.push_back("eye_only_here");
    parents.push_back(head);
    EditBook book = leg_book();
    {
        EditEntry e;
        e.bone = "eye_only_here";
        e.edit.axis[0] = e.edit.axis[1] = e.edit.axis[2] = 1.5;
        book[fold_case(e.bone)] = e;
    }
    auto with_eye = [&](const std::vector<Xform>& body) {
        std::vector<Xform> v(body.begin(), body.begin() + head + 1);
        Xform eye_local = xf::identity();
        eye_local.pos[0] = 8.0;
        eye_local.pos[1] = 3.0;
        v.push_back(xf::compose(eye_local, v[head]));
        return v;
    };

    // (a) animated on its own (the game's pose): edited, and lifted with the body
    Secondary a;
    run_secondary(a, names, parents, with_eye(game), book);
    CHECK(a.rig.copy_state() == 2, "%s: independent head mesh: copy state %d", label, a.rig.copy_state());
    CHECK(std::fabs(a.out[head].pos[2] - pose[head].pos[2]) < 1e-6, "%s: independent head mesh %.3f cm off the body's head", label,
          a.out[head].pos[2] - pose[head].pos[2]);

    // (b) Copy Pose from the body: shared bones exactly as the body has them (torso edits not doubled, no
    //     second lift); its own bone still gets its slider
    Secondary b;
    const std::vector<Xform> copied = with_eye(first);
    run_secondary(b, names, parents, copied, book);
    CHECK(b.rig.copy_state() == 1, "%s: copy-pose head mesh: copy state %d", label, b.rig.copy_state());
    CHECK(worst_shared(b) < 1e-6, "%s: copy-pose head mesh: shared bones %.3g cm off the body", label, worst_shared(b));
    const size_t eye = names.size() - 1;
    CHECK(std::fabs(b.out[eye].scl[0] / copied[eye].scl[0] - 1.5) < 1e-9, "%s: own bone of a copy-pose mesh lost its slider", label);

    // (c) the game's case: a head mesh on the FULL skeleton (legs too) copying the body's grounded pose
    Secondary c;
    run_secondary(c, game_skel.names, game_skel.parents, first, leg_book());
    CHECK(c.rig.copy_state() == 1 && c.rig.follows_body(), "%s: copy-pose full-skeleton mesh not recognised (state %d)", label, c.rig.copy_state());
    CHECK(std::fabs(c.rig.last_lift()) < 1e-9, "%s: copy-pose full-skeleton mesh lifted itself by %.2f cm", label, c.rig.last_lift());
    CHECK(worst_shared(c) < 1e-6, "%s: copy-pose full-skeleton mesh %.3g cm off the body (neck %.3f)", label, worst_shared(c),
          c.out[neck].pos[2] - first[neck].pos[2]);

    // (d) the same full-skeleton mesh animated on its own: grounded like the body
    Secondary d;
    run_secondary(d, game_skel.names, game_skel.parents, game, leg_book());
    CHECK(d.rig.copy_state() == 2, "%s: independent full-skeleton mesh: copy state %d", label, d.rig.copy_state());
    CHECK(worst_shared(d) < 1e-6, "%s: independent full-skeleton mesh %.3g cm off the body", label, worst_shared(d));

    std::printf("   other meshes: head-only own anim %+.3f | head-only copy %.1e | full copy %.1e (lift %.2f) | full own anim %.1e cm\n",
                a.out[head].pos[2] - pose[head].pos[2], worst_shared(b), worst_shared(c), c.rig.last_lift(), worst_shared(d));
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: test_ground <skeleton_cache.txt> [bones]\n");
        return 2;
    }
    Skel ref;
    const size_t want = argc > 2 ? std::stoul(argv[2]) : 410;
    if (!load(argv[1], want, ref))
    {
        std::printf("no %zu-bone skeleton with a reference pose in %s\n", want, argv[1]);
        return 2;
    }
    for (const LegChain& l : find_legs(ref.names, ref.parents))
    {
        std::printf("leg: %s -> %s -> %s\n", ref.names[l.thigh].c_str(), ref.names[l.calf].c_str(), ref.names[l.foot].c_str());
    }

    scenario("standing", ref, true);
    Skel step = ref; // left foot up on a step: hip and knee flexed (axis Y on this rig)
    bend(step, "thigh_l", 40, 1);
    bend(step, "calf_l", -60, 1);
    scenario("left foot on a step, IK", step, true);
    scenario("left foot on a step, lift only", step, false);
    Skel crouch = ref;
    bend(crouch, "thigh_l", 60, 1);
    bend(crouch, "calf_l", -90, 1);
    bend(crouch, "thigh_r", 45, 1);
    bend(crouch, "calf_r", -70, 1);
    scenario("both knees bent, IK", crouch, true);

    // ground off: exactly the old result
    {
        const std::vector<Xform> game = component_pose(step);
        RigSculptor a, b;
        a.set_skeleton(step.names, step.parents);
        b.set_skeleton(step.names, step.parents);
        GroundOptions off;
        b.set_ground(off);
        a.bind(leg_book(), 1);
        b.bind(leg_book(), 1);
        std::vector<Xform> pa = game, pb = game;
        a.apply(pa.data(), static_cast<int32_t>(pa.size()));
        b.apply(pb.data(), static_cast<int32_t>(pb.size()));
        CHECK(std::memcmp(pa.data(), pb.data(), pa.size() * sizeof(Xform)) == 0, "ground off changed the result");
        CHECK(b.planned_bones() < step.names.size(), "ground off should not plan every bone");
    }
    // switching ground on and off again puts the plain result back
    {
        const std::vector<Xform> game = component_pose(step);
        RigSculptor s;
        s.set_skeleton(step.names, step.parents);
        GroundOptions on;
        on.enabled = true;
        s.set_ground(on);
        s.bind(leg_book(), 1);
        std::vector<Xform> buf = game;
        s.apply(buf.data(), static_cast<int32_t>(buf.size()));
        s.set_ground(GroundOptions{});
        CHECK(s.bound_revision() != 1, "changing ground options must force a re-bind");
        s.bind(leg_book(), 1);
        s.apply(buf.data(), static_cast<int32_t>(buf.size())); // buffer still holds our grounded result
        RigSculptor plain;
        plain.set_skeleton(step.names, step.parents);
        plain.bind(leg_book(), 1);
        std::vector<Xform> want_pose = game;
        plain.apply(want_pose.data(), static_cast<int32_t>(want_pose.size()));
        double worst = 0.0;
        for (size_t i = 0; i < buf.size(); ++i)
        {
            worst = std::max(worst, dist(buf[i], want_pose[i]));
        }
        CHECK(worst < 1e-6, "turning ground off left bones %.3g cm away from the plain edit", worst);
    }

    std::printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
