// Host-side tests for the pose sculpting core, using the real SK_Roku_v3 reference skeleton.
#include "core/body_groups.hpp"
#include "core/mirror.hpp"
#include "core/presets.hpp"
#include "core/sculpt.hpp"
#include "core/registry.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace uuepbs;

static int g_fail = 0;
#define CHECK(c)                                                                                                                                     \
    do                                                                                                                                               \
    {                                                                                                                                                \
        if (!(c))                                                                                                                                    \
        {                                                                                                                                            \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                                                                 \
            ++g_fail;                                                                                                                                \
        }                                                                                                                                            \
    } while (0)

struct Skel
{
    std::vector<std::string> names;
    std::vector<int32_t> parents;
    std::vector<Xform> local;
};

static Skel load()
{
    Skel s;
    std::ifstream f("roku_ref.txt");
    std::string line;
    while (std::getline(f, line))
    {
        std::istringstream in(line);
        std::string n;
        int p;
        Xform x = xf::identity();
        in >> n >> p >> x.rot[0] >> x.rot[1] >> x.rot[2] >> x.rot[3] >> x.pos[0] >> x.pos[1] >> x.pos[2] >> x.scl[0] >> x.scl[1] >> x.scl[2];
        double len = std::sqrt(x.rot[0] * x.rot[0] + x.rot[1] * x.rot[1] + x.rot[2] * x.rot[2] + x.rot[3] * x.rot[3]);
        for (double& q : x.rot)
            q /= len;
        s.names.push_back(n);
        s.parents.push_back(p);
        s.local.push_back(x);
    }
    return s;
}

static int find_bone(const Skel& s, const char* name)
{
    for (size_t i = 0; i < s.names.size(); ++i)
        if (s.names[i] == name)
            return (int)i;
    return -1;
}

static std::vector<Xform> fk(const Skel& s, const std::vector<Xform>& local)
{
    std::vector<Xform> cs(local.size());
    for (size_t i = 0; i < local.size(); ++i)
    {
        cs[i] = s.parents[i] < 0 ? local[i] : xf::compose(local[i], cs[s.parents[i]]);
        cs[i].pos[3] = 0;
        cs[i].scl[3] = 0;
    }
    return cs;
}

// Reference implementation straight from local space.
static std::vector<Xform> reference(const Skel& s, const std::vector<Xform>& local, const EditBook& book)
{
    const size_t n = local.size();
    std::vector<int> slot(n, -1);
    std::vector<BoneEdit> sc;
    for (size_t i = 0; i < n; ++i)
    {
        auto it = book.find(fold_case(s.names[i]));
        if (it != book.end() && !it->second.edit.is_neutral())
        {
            slot[i] = (int)sc.size();
            sc.push_back(it->second.edit);
        }
    }
    std::vector<Xform> out(n), kid(n);
    for (size_t i = 0; i < n; ++i)
    {
        const int p = s.parents[i];
        Xform L = local[i];
        if (p >= 0 && slot[p] >= 0 && sc[slot[p]].spread == Spread::Keep)
            for (int a = 0; a < 3; ++a)
                L.scl[a] /= sc[slot[p]].axis[a];
        Xform G = L;
        const Xform P = p < 0 ? xf::identity() : kid[p];
        if (slot[i] >= 0)
        {
            const BoneEdit& e = sc[slot[i]];
            // move along the bone's own rest axes, in real centimetres
            double off[3];
            xf::qrotate(L.rot, e.shift, off);
            for (int a = 0; a < 3; ++a)
                G.pos[a] += off[a] / P.scl[a];
            double q[4];
            turn_quaternion(e, q);
            xf::qmul(L.rot, q, G.rot);
            for (int a = 0; a < 3; ++a)
                G.scl[a] *= e.axis[a];
        }
        out[i] = p < 0 ? G : xf::compose(G, P);
        kid[i] = (slot[i] >= 0 && sc[slot[i]].spread == Spread::Solo) ? (p < 0 ? L : xf::compose(L, P)) : out[i];
    }
    return out;
}

static double diff(const Xform& a, const Xform& b)
{
    double d = 0;
    for (int i = 0; i < 3; ++i)
    {
        d = std::max(d, std::fabs(a.pos[i] - b.pos[i]));
        d = std::max(d, std::fabs(a.scl[i] - b.scl[i]));
    }
    // quaternion sign ambiguity
    double dp = 0, dm = 0;
    for (int i = 0; i < 4; ++i)
    {
        dp = std::max(dp, std::fabs(a.rot[i] - b.rot[i]));
        dm = std::max(dm, std::fabs(a.rot[i] + b.rot[i]));
    }
    return std::max(d, std::min(dp, dm));
}

static double worst(const std::vector<Xform>& a, const std::vector<Xform>& b)
{
    double w = 0;
    for (size_t i = 0; i < a.size(); ++i)
        w = std::max(w, diff(a[i], b[i]));
    return w;
}

static EditBook book_of(std::initializer_list<std::tuple<const char*, double, double, double, Spread>> items)
{
    EditBook b;
    for (auto& [n, x, y, z, sp] : items)
    {
        EditEntry e;
        e.bone = n;
        e.edit.axis[0] = x;
        e.edit.axis[1] = y;
        e.edit.axis[2] = z;
        e.edit.spread = sp;
        b[fold_case(n)] = e;
    }
    return b;
}

static void add_motion(EditBook& b, const char* bone, double rx, double ry, double rz, double mx, double my, double mz)
{
    EditEntry& e = b[fold_case(bone)];
    e.bone = bone;
    e.edit.turn[0] = rx;
    e.edit.turn[1] = ry;
    e.edit.turn[2] = rz;
    e.edit.shift[0] = mx;
    e.edit.shift[1] = my;
    e.edit.shift[2] = mz;
}

static std::vector<Xform> animate(const Skel& s, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-0.6, 0.6);
    std::vector<Xform> local = s.local;
    for (auto& L : local)
    {
        const double ax[3] = {u(rng), u(rng), u(rng)};
        const double ang = u(rng);
        const double len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-9;
        const double q[4] = {std::sin(ang / 2) * ax[0] / len, std::sin(ang / 2) * ax[1] / len, std::sin(ang / 2) * ax[2] / len, std::cos(ang / 2)};
        double r[4];
        xf::qmul(L.rot, q, r);
        std::copy(r, r + 4, L.rot);
    }
    return local;
}

int main()
{
    const Skel s = load();
    CHECK(s.names.size() == 184);

    RigSculptor rig;
    std::string why;
    CHECK(rig.set_skeleton(s.names, s.parents, &why));

    const std::vector<std::tuple<const char*, EditBook>> cases = {
        {"breasts chain", book_of({{"boob_l", 1.5, 1.5, 1.5, Spread::Chain}, {"BOOB_R", 1.5, 1.5, 1.5, Spread::Chain}})},
        {"thigh keep", book_of({{"thigh_l", 1.0, 1.25, 1.25, Spread::Keep}, {"thigh_twist_01_l", 1.0, 1.25, 1.25, Spread::Chain}})},
        {"spine solo", book_of({{"spine_02", 1.3, 0.9, 1.1, Spread::Solo}})},
        {"nested", book_of({{"pelvis", 1.1, 1.2, 1.2, Spread::Keep},
                            {"spine_02", 1.2, 1.2, 1.2, Spread::Keep},
                            {"boob_l", 1.4, 1.3, 1.3, Spread::Chain},
                            {"head", 0.85, 0.85, 0.85, Spread::Chain},
                            {"neck_01", 1, 1.2, 1.2, Spread::Solo},
                            {"nipp_l", 2, 2, 2, Spread::Solo}})},
        {"root", book_of({{"root", 2, 2, 2, Spread::Keep}})},
        {"head turn",
         [] {
             EditBook b;
             add_motion(b, "head", 20, -35, 60, 0, 0, 0);
             return b;
         }()},
        {"mixed",
         [] {
             EditBook b =
                 book_of({{"spine_02", 1.2, 1.1, 1.1, Spread::Keep}, {"upperarm_l", 1, 1.2, 1.2, Spread::Keep}, {"neck_01", 1, 1, 1, Spread::Solo}});
             add_motion(b, "spine_02", 0, 0, 15, 0, 2, 0);
             add_motion(b, "upperarm_l", -30, 10, 0, 1, 0, -2);
             add_motion(b, "neck_01", 0, 25, 0, 0, 0, 3);
             add_motion(b, "boob_r", 5, 0, 0, 0, -1.5, 0.5);
             return b;
         }()},
    };

    for (unsigned seed = 1; seed <= 3; ++seed)
    {
        const auto local = animate(s, seed);
        const auto original = fk(s, local);
        for (const auto& [label, book] : cases)
        {
            rig.forget_buffers();
            rig.bind(book, seed * 100 + 1);
            std::vector<Xform> buf = original;
            rig.apply(buf.data(), (int)buf.size());
            const auto expect = reference(s, local, book);
            const double w = worst(buf, expect);
            std::printf("seed %u %-14s worst error %.3g (planned %zu)\n", seed, label, w, rig.planned_bones());
            CHECK(w < 1e-5); // cm; ref pose data is float precision

            // same buffer again without an engine refresh -> must not compound
            const std::vector<Xform> first = buf;
            rig.apply(buf.data(), (int)buf.size());
            CHECK(worst(buf, first) < 1e-12);

            // engine refreshes only half the bones (others stale = still our output)
            for (size_t i = 0; i < buf.size(); i += 2)
                buf[i] = original[i];
            rig.apply(buf.data(), (int)buf.size());
            CHECK(worst(buf, first) < 1e-9);

            // edits removed -> original restored exactly
            rig.bind(EditBook{}, seed * 100 + 2);
            rig.apply(buf.data(), (int)buf.size());
            CHECK(worst(buf, original) < 1e-9);
        }
    }

    // Chain semantics sanity: scaling boob_l by 2 moves nipp_l twice as far from boob_l.
    {
        const auto original = fk(s, s.local);
        rig.forget_buffers();
        rig.bind(book_of({{"boob_l", 2, 2, 2, Spread::Chain}}), 7);
        std::vector<Xform> buf = original;
        rig.apply(buf.data(), (int)buf.size());
        const int b = rig.find("boob_l"), n = rig.find("nipp_l");
        auto dist = [](const Xform& a, const Xform& c) {
            return std::sqrt((a.pos[0] - c.pos[0]) * (a.pos[0] - c.pos[0]) + (a.pos[1] - c.pos[1]) * (a.pos[1] - c.pos[1]) +
                             (a.pos[2] - c.pos[2]) * (a.pos[2] - c.pos[2]));
        };
        CHECK(std::fabs(dist(buf[b], buf[n]) - 2 * dist(original[b], original[n])) < 1e-6);
        CHECK(diff(buf[b], original[b]) - 1.0 < 1e-6); // only scale changed on boob_l
        CHECK(std::fabs(buf[b].pos[0] - original[b].pos[0]) < 1e-9);
        // unrelated bone untouched bit-for-bit
        const int h = rig.find("hand_r");
        CHECK(xf::same_bits(buf[h], original[h]));
    }

    // Rotation pivots on the joint and keeps bone lengths; move shifts by exactly the offset.
    {
        const auto original = fk(s, s.local);
        EditBook b;
        add_motion(b, "lowerarm_l", 0, 0, 90, 0, 0, 0);
        rig.forget_buffers();
        rig.bind(b, 501);
        std::vector<Xform> buf = original;
        rig.apply(buf.data(), (int)buf.size());
        const int lo = rig.find("lowerarm_l"), hand = rig.find("hand_l");
        auto dist = [](const Xform& a, const Xform& c) {
            return std::sqrt((a.pos[0] - c.pos[0]) * (a.pos[0] - c.pos[0]) + (a.pos[1] - c.pos[1]) * (a.pos[1] - c.pos[1]) +
                             (a.pos[2] - c.pos[2]) * (a.pos[2] - c.pos[2]));
        };
        CHECK(std::fabs(buf[lo].pos[0] - original[lo].pos[0]) < 1e-9);                          // elbow stays put
        CHECK(std::fabs(dist(buf[lo], buf[hand]) - dist(original[lo], original[hand])) < 1e-6); // forearm length kept
        CHECK(dist(buf[hand], original[hand]) > 10.0);                                          // but the hand swung away

        EditBook m;
        add_motion(m, "head", 0, 0, 0, 0, 0, 5);
        rig.bind(m, 502);
        std::vector<Xform> moved = original;
        rig.apply(moved.data(), (int)moved.size());
        const int head = rig.find("head"), eye = rig.find("c_eye_l");
        CHECK(std::fabs(dist(moved[head], original[head]) - 5.0) < 1e-6); // exactly 5 cm
        CHECK(std::fabs(dist(moved[eye], original[eye]) - 5.0) < 1e-6);   // children come along (chain)

        EditBook solo;
        add_motion(solo, "head", 0, 0, 30, 0, 0, 5);
        solo["head"].edit.spread = Spread::Solo;
        rig.bind(solo, 503);
        std::vector<Xform> still = original;
        rig.apply(still.data(), (int)still.size());
        CHECK(xf::same_bits(still[eye], original[eye]) || diff(still[eye], original[eye]) < 1e-9); // solo: child untouched
        CHECK(dist(still[head], original[head]) > 4.9);
    }

    // Mirroring a rotate/move edit to the other side gives the mirror-image pose (rest pose).
    {
        const auto original = fk(s, s.local);
        const MirrorTable measured = build_mirror_table(s.names, s.parents, s.local, true);
        CHECK(measured.centre_axis == 0 && measured.pairs == 71);
        auto mirror_pos = [](const Xform& x) {
            return std::array<double, 3>{-x.pos[0], x.pos[1], x.pos[2]};
        };
        int pairs = 0;
        double worst_mirror = 0;
        for (const char* bone : {"boob_l", "upperarm_l", "thigh_l", "hand_l", "c_ear_01_l", "horn1_l", "butt_l", "calf_l", "clavicle_l"})
        {
            BoneEdit e;
            e.turn[0] = 25;
            e.turn[1] = -40;
            e.turn[2] = 15;
            e.shift[0] = 2;
            e.shift[1] = -3;
            e.shift[2] = 1.5;
            e.axis[1] = 1.3;
            const std::string other = mirror_bone_name(bone);
            EditBook b;
            b[fold_case(bone)] = EditEntry{bone, e};
            b[fold_case(other)] = EditEntry{other, mirror_with(measured, bone, e)};
            rig.forget_buffers();
            rig.bind(b, 600 + pairs);
            std::vector<Xform> buf = original;
            rig.apply(buf.data(), (int)buf.size());
            // every descendant of the left bone must mirror its right twin
            for (int i = 0; i < rig.bone_count(); ++i)
            {
                const std::string twin = mirror_bone_name(s.names[i]);
                if (twin.empty() || s.names[i].back() != 'l')
                    continue;
                const int j = rig.find(twin);
                if (j < 0)
                    continue;
                const auto m = mirror_pos(buf[i]);
                for (int a = 0; a < 3; ++a)
                    worst_mirror = std::max(worst_mirror, std::fabs(m[a] - buf[j].pos[a]));
            }
            ++pairs;
        }
        std::printf("mirror check: worst position mismatch %.4f cm\n", worst_mirror);
        CHECK(worst_mirror < 0.05);
    }

    // Two alternating buffers (double-buffered component transforms).
    {
        const auto l1 = animate(s, 11), l2 = animate(s, 12);
        const auto o1 = fk(s, l1), o2 = fk(s, l2);
        const EditBook bk = book_of({{"spine_01", 1.2, 1.3, 1.3, Spread::Keep}, {"butt_l", 1.6, 1.6, 1.6, Spread::Chain}});
        rig.forget_buffers();
        rig.bind(bk, 99);
        std::vector<Xform> A = o1, B = o2;
        for (int frame = 0; frame < 6; ++frame)
        {
            std::vector<Xform>& buf = (frame & 1) ? B : A;
            rig.apply(buf.data(), (int)buf.size()); // stale buffer, no refresh
        }
        CHECK(worst(A, reference(s, l1, bk)) < 1e-6);
        CHECK(worst(B, reference(s, l2, bk)) < 1e-6);
    }

    // Presets: JSON round trip, hand-written JSON, errors, legacy .rbs.
    {
        const EditBook bk =
            book_of({{"boob_l", 1.25, 1.2, 1.3, Spread::Chain}, {"thigh_r", 1, 1.1, 1.1, Spread::Keep}, {"head", 0.9, 0.9, 0.9, Spread::Solo}});
        const std::string text = PresetShelf::serialize(bk);
        std::printf("preset json:\n%s", text.c_str());
        EditBook back;
        std::string msg;
        CHECK(PresetShelf::parse(text, back, msg));
        CHECK(back.size() == 3);
        CHECK(back["thigh_r"].edit.spread == Spread::Keep);
        CHECK(back["head"].edit.spread == Spread::Solo);
        CHECK(std::fabs(back["boob_l"].edit.axis[2] - 1.3) < 1e-9);
        CHECK(back["boob_l"].bone == "boob_l");

        EditBook motion;
        add_motion(motion, "head", 10, -20.5, 30, 0, 1.25, -2);
        const std::string mtext = PresetShelf::serialize(motion);
        std::printf("%s", mtext.c_str());
        EditBook mback;
        CHECK(PresetShelf::parse(mtext, mback, msg) && mback.size() == 1);
        CHECK(mback["head"].edit.turn[1] == -20.5 && mback["head"].edit.shift[2] == -2 && !mback["head"].edit.has_scale());
        EditBook objform;
        CHECK(PresetShelf::parse("{\"bones\": {\"pelvis\": {\"rotate\": {\"z\": 12}, \"move\": {\"y\": -3}}}}", objform, msg));
        CHECK(objform["pelvis"].edit.turn[2] == 12 && objform["pelvis"].edit.shift[1] == -3);

        EditBook hand;
        const char* written =
            "\xEF\xBB\xBF{\n \"bones\": {\n  \"pelvis\": {\"scale\": 1.1},\n  \"Spine_02\": {\"scale\": [1, 1.2, 0.9], \"children\": \"Keep "
            "Size\"},\n"
            "  \"calf_l\": {\"x\": 1.05, \"children\": \"this_bone_only\"},\n  \"na\\u00efve\": {\"width\": 2},\n  \"bad\": 5\n }\n}\n";
        CHECK(PresetShelf::parse(written, hand, msg));
        std::printf("hand-written: %s\n", msg.c_str());
        CHECK(hand.size() == 4);
        CHECK(std::fabs(hand["pelvis"].edit.axis[1] - 1.1) < 1e-12);
        CHECK(hand["spine_02"].edit.spread == Spread::Keep && std::fabs(hand["spine_02"].edit.axis[2] - 0.9) < 1e-12);
        CHECK(hand["calf_l"].edit.spread == Spread::Solo && hand["calf_l"].edit.axis[1] == 1.0);
        CHECK(hand.count("na\xC3\xAFve") == 1);

        EditBook broken;
        CHECK(!PresetShelf::parse("{\n \"bones\": {\n  \"a\": {\"scale\": 1.1}\n  \"b\": {\"scale\": 1.2}\n }\n}", broken, msg));
        std::printf("broken: %s\n", msg.c_str());
        CHECK(msg.find("line 4") != std::string::npos);

        EditBook legacy;
        CHECK(PresetShelf::parse("[rbs-preset 1]\npelvis = 1.1\nboob_l = 1.2 1.2 1.2 keep\n", legacy, msg));
        CHECK(legacy.size() == 2 && legacy["boob_l"].edit.spread == Spread::Keep);

        PresetShelf shelf;
        const std::filesystem::path dir = "/tmp/claude-uuepbs-test/UUEPBS Presets";
        std::filesystem::remove_all(dir);
        shelf.set_folder(dir);
        {
            std::ofstream(dir / "Old One.rbs") << "boob_l = 1.4\n";
        }
        CHECK(shelf.exists("Old One"));
        EditBook loaded;
        CHECK(shelf.load("Old One", loaded, msg) && loaded.size() == 1);
        CHECK(shelf.save("My: Preset?", bk, msg));
        CHECK(std::filesystem::exists(dir / "My Preset.json"));
        CHECK(shelf.load("My Preset", loaded, msg) && loaded.size() == 3);
        CHECK(shelf.list().size() == 2);
        CHECK(shelf.save("Old One", bk, msg)); // migrates: json written, .rbs removed
        CHECK(!std::filesystem::exists(dir / "Old One.rbs"));
        CHECK(shelf.list().size() == 2);
        CHECK(shelf.remove("My Preset", msg) && !shelf.exists("My Preset"));
    }

    // Registry end to end with a fake component laid out like USkinnedMeshComponent.
    {
        alignas(16) static unsigned char comp[0x1000] = {};
        const auto original = fk(s, s.local);
        std::vector<Xform> bufs[2] = {original, original};
        auto put = [&](size_t off, auto v) {
            std::memcpy(comp + off, &v, sizeof(v));
        };
        put(0x0, (uintptr_t)0x1234);
        put(0xC, (int32_t)77);
        put(0x10, (uintptr_t)0x5555);
        put(0x20, (uintptr_t)0x6666);
        RawArray a0{bufs[0].data(), (int32_t)bufs[0].size(), (int32_t)bufs[0].size()};
        RawArray a1{bufs[1].data(), (int32_t)bufs[1].size(), (int32_t)bufs[1].size()};
        put(0x600, a0);
        put(0x610, a1);
        put(0x644, (int32_t)0);
        put(0x648, (int32_t)1);
        Registry& reg = Registry::instance();
        reg.set_layout(PoseLayout{0x600, 0x648, 16, 96});
        std::string msg;
        CHECK(reg.track((uintptr_t)comp, "CharacterMesh0", "BP_Test_C_0", s.names, s.parents, true, msg));
        reg.set_reference_pose((uintptr_t)comp, s.local);
        CHECK(reg.mirror_source().find("reference pose") != std::string::npos);
        CHECK(reg.mirror_table().pairs == 71);
        const EditBook bk = book_of({{"head", 1.3, 1.3, 1.3, Spread::Chain}});
        reg.replace_edits(bk);
        reg.on_pose_finalized(comp);
        CHECK(worst(bufs[1], reference(s, s.local, bk)) < 1e-6);
        CHECK(worst(bufs[0], original) == 0);
        reg.set_enabled(false);
        reg.on_pose_finalized(comp);
        CHECK(worst(bufs[1], original) < 1e-9);
        reg.set_enabled(true);
        // identity change -> stale + rescan request
        put(0xC, (int32_t)78);
        reg.on_pose_finalized(comp);
        CHECK(reg.take_rescan_request());
        CHECK(!reg.is_tracked((uintptr_t)comp));
        CHECK(reg.skeleton().names.empty());
        reg.untrack_all();
    }

    // UE4 layout: 48-byte float transforms. Edits must not compound through float round trips.
    {
        alignas(16) static unsigned char comp[0x1000] = {};
        const auto original = fk(s, s.local);
        std::vector<XformF> floats(original.size());
        for (size_t i = 0; i < original.size(); ++i)
        {
            for (int a = 0; a < 4; ++a)
            {
                floats[i].rot[a] = (float)original[i].rot[a];
                floats[i].pos[a] = (float)original[i].pos[a];
                floats[i].scl[a] = (float)original[i].scl[a];
            }
        }
        const std::vector<XformF> pristine = floats;
        auto put = [&](size_t off, auto v) {
            std::memcpy(comp + off, &v, sizeof(v));
        };
        put(0x0, (uintptr_t)0x4321);
        put(0xC, (int32_t)12);
        put(0x10, (uintptr_t)0x7777);
        RawArray a0{floats.data(), (int32_t)floats.size(), (int32_t)floats.size()};
        put(0x500, a0);
        put(0x510, a0);
        put(0x520, (int32_t)0);
        put(0x524, (int32_t)0);
        Registry& reg = Registry::instance();
        reg.set_layout(PoseLayout{0x500, 0x524, 16, 48});
        std::string msg;
        CHECK(reg.track((uintptr_t)comp, "Mesh", "UE4 test", s.names, s.parents, true, msg));
        const EditBook bk = book_of({{"thigh_l", 1.0, 1.4, 1.4, Spread::Keep}, {"head", 1.2, 1.2, 1.2, Spread::Chain}});
        reg.replace_edits(bk);
        reg.on_pose_finalized(comp);
        const std::vector<XformF> once = floats;
        const auto ref = reference(s, s.local, bk);
        double w = 0;
        for (size_t i = 0; i < ref.size(); ++i)
        {
            for (int a = 0; a < 3; ++a)
            {
                w = std::max(w, std::fabs(once[i].pos[a] - ref[i].pos[a]));
            }
        }
        std::printf("float pose: worst position error %.5f cm\n", w);
        CHECK(w < 1e-3);
        for (int k = 0; k < 5; ++k)
        {
            reg.on_pose_finalized(comp); // engine did not refresh: must not compound
        }
        CHECK(std::memcmp(once.data(), floats.data(), floats.size() * sizeof(XformF)) == 0);
        reg.clear_edits();
        reg.on_pose_finalized(comp);
        CHECK(std::memcmp(pristine.data(), floats.data(), floats.size() * sizeof(XformF)) == 0); // exact restore

        // Only the bones the sculptor reads/writes are converted: switching edits must still
        // put the old bones back exactly, and a fresh engine pose must give the same result.
        reg.replace_edits(bk);
        reg.on_pose_finalized(comp);
        const EditBook head_only = book_of({{"head", 1.2, 1.2, 1.2, Spread::Chain}});
        reg.replace_edits(head_only);
        reg.on_pose_finalized(comp);
        const std::vector<XformF> switched = floats;
        const int thigh = find_bone(s, "thigh_l"), calf = find_bone(s, "calf_l");
        CHECK(std::memcmp(&switched[thigh], &pristine[thigh], sizeof(XformF)) == 0);
        CHECK(std::memcmp(&switched[calf], &pristine[calf], sizeof(XformF)) == 0);
        floats = pristine; // engine writes a fresh pose
        reg.on_pose_finalized(comp);
        CHECK(std::memcmp(switched.data(), floats.data(), floats.size() * sizeof(XformF)) == 0);
        reg.clear_edits();
        reg.on_pose_finalized(comp);
        CHECK(std::memcmp(pristine.data(), floats.data(), floats.size() * sizeof(XformF)) == 0);
        reg.untrack_all();
    }

    // Cost of the pose hook callback (it runs for every skeletal mesh in the game, every frame).
    {
        alignas(16) static unsigned char comp[0x1000] = {};
        alignas(16) static unsigned char other[0x1000] = {};
        std::vector<Xform> pose = fk(s, s.local);
        auto put = [&](size_t off, auto v) {
            std::memcpy(comp + off, &v, sizeof(v));
        };
        put(0x0, (uintptr_t)0x1234);
        put(0xC, (int32_t)5);
        put(0x10, (uintptr_t)0x5555);
        RawArray a0{pose.data(), (int32_t)pose.size(), (int32_t)pose.size()};
        put(0x600, a0);
        put(0x610, a0);
        put(0x648, (int32_t)0);
        Registry& reg = Registry::instance();
        reg.set_layout(PoseLayout{0x600, 0x648, 16, 96});
        std::string msg;
        CHECK(reg.track((uintptr_t)comp, "Mesh", "Bench", s.names, s.parents, true, msg));
        reg.clear_edits();
        auto time_ns = [](auto&& fn, int n) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i)
                fn();
            return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
        };
        const double untracked = time_ns([&] { reg.on_pose_finalized(other); }, 200000);
        const double idle = time_ns([&] { reg.on_pose_finalized(comp); }, 200000);
        reg.replace_edits(book_of({{"boob_l", 1.3, 1.3, 1.3, Spread::Chain}, {"boob_r", 1.3, 1.3, 1.3, Spread::Chain}}));
        const double breasts = time_ns([&] { reg.on_pose_finalized(comp); }, 50000);
        std::printf("hook cost: other mesh %.0f ns, tracked no edits %.0f ns, tracked 2 edits %.0f ns\n", untracked, idle, breasts);
        reg.clear_edits();
        reg.on_pose_finalized(comp);
        reg.untrack_all();
    }

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
