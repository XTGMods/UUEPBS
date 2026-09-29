// UUEPBS - per-rig pose sculpting
//
// A "rig" is one SkeletalMeshComponent's bone hierarchy. After the engine has
// written the final component-space pose (FinalizeBoneTransform) we scale, rotate
// and move the requested bones in place and rebuild the part of the hierarchy that depends on
// them. Nothing is cumulative: the untouched pose is recovered every frame, even
// when the engine hands us a buffer that still holds our own previous result.
#pragma once

#include "xform.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace uuepbs
{
    // How a bone's edit reaches its descendants.
    enum class Spread : uint8_t
    {
        Chain = 0, // engine-native: children inherit scale, rotation and offset
        Keep = 1,  // children follow the rotation/offset and moved joints but keep their own size
        Solo = 2,  // only this bone deforms, children stay exactly where they were
    };

    const char* spread_label(Spread s);
    const char* spread_token(Spread s);
    bool spread_from_token(std::string_view token, Spread& out);

    // One bone's adjustment, all in the bone's own local axes (X runs along the bone).
    struct BoneEdit
    {
        double axis[3]{1.0, 1.0, 1.0};  // scale factors
        double turn[3]{0.0, 0.0, 0.0};  // rotation in degrees about X, Y, Z (applied X, then Y, then Z)
        double shift[3]{0.0, 0.0, 0.0}; // translation in cm along X, Y, Z
        Spread spread{Spread::Chain};

        static constexpr double kMaxTurn = 180.0;
        static constexpr double kMaxShift = 200.0;

        bool has_scale() const;
        bool has_turn() const;
        bool has_shift() const;
        bool is_neutral() const { return !has_scale() && !has_turn() && !has_shift(); }
        void clamp();
    };

    // Rotation of `edit.turn` as a quaternion (x, y, z, w).
    void turn_quaternion(const BoneEdit& edit, double out[4]);

    std::string fold_case(std::string_view text);

    // Bone edits keyed by case-folded bone name (FName comparison is case-insensitive).
    struct EditEntry
    {
        std::string bone; // display spelling
        BoneEdit edit;
    };
    using EditBook = std::map<std::string, EditEntry>;

    class RigSculptor
    {
      public:
        bool set_skeleton(std::vector<std::string> names, std::vector<int32_t> parents, std::string* why = nullptr);

        int32_t bone_count() const { return static_cast<int32_t>(m_names.size()); }
        const std::vector<std::string>& names() const { return m_names; }
        const std::vector<int32_t>& parents() const { return m_parents; }
        int32_t find(std::string_view bone) const;

        // Re-plans which bones must be rebuilt. Call whenever the edit book changes.
        void bind(const EditBook& book, uint64_t revision);
        uint64_t bound_revision() const { return m_revision; }
        size_t planned_bones() const { return m_plan.size(); }

        // Edits `pose` (component space, `count` entries) in place.
        // Returns the number of bones written (restores included).
        // `key` identifies the engine buffer when `pose` is a converted copy (float poses).
        int32_t apply(Xform* pose, int32_t count, const void* key = nullptr);

        // Round results to float precision (UE4 poses are stored as floats), so a
        // result read back through float -> double still matches what we wrote.
        void set_quantize(bool on) { m_quantize = on; }
        bool quantize() const { return m_quantize; }

        // Drops every remembered buffer (e.g. the component got a new mesh).
        void forget_buffers();

        // Cheap per-frame test: false when apply() would not touch this buffer at all
        // (no edits planned and nothing of ours left in it to put back).
        bool has_work(const void* key) const;

        // Bones apply() reads for this buffer: planned bones, their parents and bones we
        // wrote earlier. Lets callers convert only those when the pose is stored as floats.
        void gather_inputs(const void* key, std::vector<int32_t>& out) const;

        // Bones the last apply() wrote (restores included).
        const std::vector<int32_t>& touched() const { return m_touched; }

      private:
        struct Memo
        {
            const void* buffer{};
            uint64_t last_use{};
            std::vector<Xform> source; // untouched pose we started from
            std::vector<Xform> result; // what we wrote
            std::vector<uint8_t> marked;
            std::vector<int32_t> marked_list;
        };

        Memo& memo_for(const void* buffer);

        std::vector<std::string> m_names;
        std::vector<std::string> m_folded;
        std::vector<int32_t> m_parents;
        std::map<std::string, int32_t> m_lookup;

        // plan
        uint64_t m_revision{~0ull};
        std::vector<int32_t> m_plan;      // ascending bone indices to rebuild
        std::vector<int32_t> m_edit_slot; // bone -> index into m_scales or -1
        std::vector<BoneEdit> m_scales;
        std::vector<std::array<double, 4>> m_turns; // per planned edit, quaternion of BoneEdit::turn
        std::vector<uint8_t> m_in_plan;
        std::vector<int32_t> m_inputs; // m_plan plus the parents it reads, ascending, unique
        std::vector<int32_t> m_touched;

        // scratch
        std::vector<Xform> m_source;
        std::vector<Xform> m_for_children;
        std::vector<Xform> m_fresh;

        std::array<Memo, 4> m_memos{};
        bool m_quantize{false};
        uint64_t m_clock{};
    };
} // namespace uuepbs
