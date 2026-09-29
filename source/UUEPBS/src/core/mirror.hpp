// UUEPBS - left/right mirroring of bone edits
//
// Rotate and move values are in each bone's own axes, and the two sides of a rig
// rarely share an axis convention (Roku's limbs flip every axis, the rest only
// flip X; other rigs differ again). Instead of hard-coding a table per skeleton,
// the conversion is measured from a pose: for a bone B and its partner P,
//     K = C_P^T * M * C_B
// where C are the bones' component-space rotations and M reflects across the
// body's centre plane. K maps B's local axes onto P's, so a move v becomes K*v
// and a rotation (a pseudo-vector) becomes det(K)*K*r.
#pragma once

#include "sculpt.hpp"
#include "xform.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace uuepbs
{
    struct MirrorRule
    {
        int8_t source[3]{0, 1, 2};  // partner axis i takes this bone's axis source[i]
        int8_t move_sign[3]{-1, 1, 1};
        int8_t turn_sign[3]{1, -1, -1};
        bool measured{false}; // false = generic fallback (only X flips)
        bool exact{true};     // false when the pose was too asymmetric to be sure
    };

    struct MirrorTable
    {
        int centre_axis{-1}; // component-space axis the body is mirrored across (-1 = unknown)
        std::map<std::string, MirrorRule> rules; // folded bone name -> rule for mirroring onto its partner
        size_t pairs{};
        size_t inexact{};
    };

    // `pose` holds one transform per bone. With `local` set they are parent-relative
    // (reference pose); otherwise they are already in component space (a live frame).
    MirrorTable build_mirror_table(const std::vector<std::string>& names, const std::vector<int32_t>& parents, const std::vector<Xform>& pose,
                                   bool local);

    // Edit for the partner bone that produces the mirror image of `edit` on `bone`.
    BoneEdit mirror_with(const MirrorRule& rule, const BoneEdit& edit);
    BoneEdit mirror_with(const MirrorTable& table, const std::string& bone, const BoneEdit& edit);
} // namespace uuepbs
