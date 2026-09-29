// Writes roku_ref.txt from a synthetic 184-bone skeleton shaped like GON's SK_Roku_v3
// (71 left/right pairs mirrored across X, limb bones "point mirrored" like the real rig),
// so the host tests run without an FModel export. make_ref.py still makes the real one.
//
//   g++ -std=c++23 -I../src make_synthetic_ref.cpp -o make_synthetic_ref && ./make_synthetic_ref
#include "core/xform.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace uuepbs;

namespace
{
    struct Bone
    {
        std::string name;
        int parent;
        Xform cs; // component space
    };

    std::vector<Bone> g_bones;
    std::mt19937 g_rng(20260929);

    double r(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(g_rng); }

    void normalise(double q[4])
    {
        const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        for (int i = 0; i < 4; ++i)
            q[i] /= n;
    }

    int find(const std::string& n)
    {
        for (size_t i = 0; i < g_bones.size(); ++i)
            if (g_bones[i].name == n)
                return static_cast<int>(i);
        return -1;
    }

    // Centre bone: x = 0, rotation about X only (symmetric under the mirror).
    int centre(const std::string& name, const std::string& parent, double dy, double dz)
    {
        const int p = parent.empty() ? -1 : find(parent);
        Xform x = xf::identity();
        const double a = r(-0.4, 0.4);
        x.rot[0] = std::sin(a / 2);
        x.rot[3] = std::cos(a / 2);
        const Xform base = p < 0 ? xf::identity() : g_bones[p].cs;
        x.pos[0] = 0;
        x.pos[1] = base.pos[1] + dy;
        x.pos[2] = base.pos[2] + dz;
        g_bones.push_back({name, p, x});
        return static_cast<int>(g_bones.size()) - 1;
    }

    // Left bone plus its right twin (added right after, like the real skeleton order doesn't matter).
    void pair(const std::string& stem, const std::string& parent_stem, bool point_mirrored, double step = 8.0)
    {
        for (int side = 0; side < 2; ++side)
        {
            const std::string suffix = side == 0 ? "_l" : "_r";
            std::string pname = parent_stem;
            if (find(pname) < 0)
                pname = parent_stem + suffix;
            const int p = find(pname);
            Xform x = xf::identity();
            if (side == 0)
            {
                double q[4] = {r(-1, 1), r(-1, 1), r(-1, 1), r(-1, 1)};
                normalise(q);
                std::copy(q, q + 4, x.rot);
                const Xform& base = g_bones[p].cs;
                x.pos[0] = std::fabs(base.pos[0]) + r(1.0, step);
                x.pos[1] = base.pos[1] + r(-step, step);
                x.pos[2] = base.pos[2] + r(-step, step);
            }
            else
            {
                const Xform& left = g_bones.back().cs;
                // Rotation M R D with M = D = diag(-1, 1, 1): q -> (x, -y, -z, w).
                double q[4] = {left.rot[0], -left.rot[1], -left.rot[2], left.rot[3]};
                if (point_mirrored)
                {
                    const double flip[4] = {1, 0, 0, 0}; // extra 180 degrees about the bone's own X
                    xf::qmul(q, flip, x.rot);
                }
                else
                {
                    std::copy(q, q + 4, x.rot);
                }
                x.pos[0] = -left.pos[0];
                x.pos[1] = left.pos[1];
                x.pos[2] = left.pos[2];
            }
            g_bones.push_back({stem + suffix, p, x});
        }
    }
} // namespace

int main()
{
    // 42 centre bones
    centre("root", "", 0, 0);
    centre("pelvis", "root", 0, 95);
    centre("spine_01", "pelvis", 0, 8);
    centre("spine_02", "spine_01", 0, 8);
    centre("spine_03", "spine_02", 0, 8);
    centre("spine_04", "spine_03", 0, 8);
    centre("spine_05", "spine_04", 0, 8);
    centre("neck_01", "spine_05", 0, 6);
    centre("neck_02", "neck_01", 0, 4);
    centre("head", "neck_02", 0, 6);
    centre("jaw", "head", 3, -2);
    centre("c_tongue_01", "jaw", 2, 0);
    centre("c_tongue_02", "c_tongue_01", 1, 0);
    centre("c_tongue_03", "c_tongue_02", 1, 0);
    std::string prev = "head";
    for (int i = 1; i <= 10; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "hair_c_%02d", i);
        centre(n, prev, -2, -3);
        prev = n;
    }
    prev = "pelvis";
    for (int i = 1; i <= 8; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "tail_%02d", i);
        centre(n, prev, -5, -2);
        prev = n;
    }
    prev = "pelvis";
    for (int i = 1; i <= 5; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "skirt_f_%02d", i);
        centre(n, prev, 4, -6);
        prev = n;
    }
    prev = "pelvis";
    for (int i = 1; i <= 4; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "skirt_b_%02d", i);
        centre(n, prev, -4, -6);
        prev = n;
    }
    centre("ik_foot_root", "root", 0, 0);

    // 71 pairs
    const char* head_bits[] = {"c_eye", "c_ear_01", "c_brow", "c_cheek", "c_lip", "horn1"};
    for (const char* b : head_bits)
        pair(b, "head", false, 5);
    pair("c_ear_02", "c_ear_01", false, 3);
    pair("horn2", "horn1", false, 3);

    pair("clavicle", "spine_05", true);
    pair("upperarm", "clavicle", true, 14);
    pair("upperarm_twist_01", "upperarm", true);
    pair("lowerarm", "upperarm", true, 25);
    pair("lowerarm_twist_01", "lowerarm", true);
    pair("hand", "lowerarm", true, 22);
    for (const char* f : {"index", "middle", "ring", "pinky"})
    {
        const std::string s(f);
        pair(s + "_metacarpal", "hand", true, 3);
        pair(s + "_01", s + "_metacarpal", true, 3);
        pair(s + "_02", s + "_01", true, 2);
        pair(s + "_03", s + "_02", true, 2);
    }
    pair("thumb_01", "hand", true, 3);
    pair("thumb_02", "thumb_01", true, 2);
    pair("thumb_03", "thumb_02", true, 2);

    pair("thigh", "pelvis", true, 10);
    pair("thigh_twist_01", "thigh", true);
    pair("thighjiggle", "thigh", false, 4);
    pair("calf", "thigh", true, 40);
    pair("calf_twist_01", "calf", true);
    pair("foot", "calf", true, 40);
    pair("ball", "foot", true, 10);
    pair("ik_foot", "ik_foot_root", true);
    pair("ik_hand", "ik_foot_root", true);

    pair("boob", "spine_04", false, 9);
    pair("nipp", "boob", false, 5);
    pair("butt", "pelvis", false, 8);
    pair("hip", "pelvis", false, 6);
    prev = "pelvis";
    for (int i = 1; i <= 6; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "skirt_%02d", i);
        pair(n, prev, false, 6);
        prev = n;
    }
    prev = "head";
    for (int i = 1; i <= 5; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "hair_%02d", i);
        pair(n, prev, false, 4);
        prev = n;
    }
    prev = "spine_05";
    for (int i = 1; i <= 4; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "cape_%02d", i);
        pair(n, prev, false, 6);
        prev = n;
    }
    prev = "pelvis";
    for (int i = 1; i <= 6; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "belt_%02d", i);
        pair(n, prev, false, 5);
        prev = n;
    }

    prev = "spine_03";
    for (int i = 1; i <= 4; ++i)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "pouch_%02d", i);
        pair(n, prev, false, 4);
        prev = n;
    }

    FILE* out = std::fopen("roku_ref.txt", "w");
    if (!out)
        return 1;
    size_t pairs = 0;
    for (const Bone& b : g_bones)
    {
        pairs += b.name.ends_with("_l");
        const Xform local = b.parent < 0 ? b.cs : xf::relative(b.cs, g_bones[b.parent].cs);
        std::fprintf(out, "%s %d %.12g %.12g %.12g %.12g %.12g %.12g %.12g %.12g %.12g %.12g\n", b.name.c_str(), b.parent, local.rot[0], local.rot[1],
                     local.rot[2], local.rot[3], local.pos[0], local.pos[1], local.pos[2], local.scl[0], local.scl[1], local.scl[2]);
    }
    std::fclose(out);
    std::printf("wrote roku_ref.txt: %zu bones, %zu pairs (synthetic)\n", g_bones.size(), pairs);
    return 0;
}
