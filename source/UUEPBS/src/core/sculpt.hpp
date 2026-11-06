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

    // Ground correction ("Auto Ground"). Longer or shorter legs move the feet off the ground the
    // game's foot IK put them on (the edit happens after the game's animation). With this on, the
    // whole pose is raised or lowered in component space (+Z) by what the sliders sank the foot
    // that sank most, and every other leg is bent with a two-bone IK so its foot is back where the
    // game put it (stairs, slopes). Foot rotations are kept as the game had them.
    struct GroundOptions
    {
        bool enabled{false};
        bool leg_ik{true};  // also put every other foot back on the ground (else only the lift)
        double extra{0.0};  // cm added to the lift (+ up)
        double max{40.0};   // the lift never goes beyond this, either way (cm)
    };

    // A leg found by name: thigh -> calf -> foot, each the parent of the next. `ball` is the foot's ball / toe
    // bone (-1 if none): with it the ground contact is the sole (ball and heel), not the ankle joint, so a bigger
    // or turned foot does not go through the floor.
    struct LegChain
    {
        int32_t thigh{-1};
        int32_t calf{-1};
        int32_t foot{-1};
        int32_t ball{-1};
    };

    // Legs of a skeleton (at most four), found by bone names: a foot / ankle bone whose parent is a
    // calf / knee / lower-leg bone whose parent is a thigh / upper-leg / hip bone. IK, twist and
    // control bones are skipped.
    std::vector<LegChain> find_legs(const std::vector<std::string>& names, const std::vector<int32_t>& parents);

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

        // ---- Auto Ground ----------------------------------------------------------
        // Changing the options forces a re-plan on the next bind (bound_revision() no longer matches).
        void set_ground(const GroundOptions& options);
        const GroundOptions& ground() const { return m_ground; }
        bool has_legs() const { return !m_legs.empty(); }
        const std::vector<LegChain>& legs() const { return m_legs; }
        // Lift used by a rig without legs (a head or outfit mesh of the same character): the one the
        // character's leg mesh measured (cm, component space).
        void set_shared_lift(double cm) { m_shared_lift = cm; }
        // Many games drive such a mesh from the leg mesh's final pose (Copy Pose / Leader Pose) or attach it
        // to one of its bones; then it already moves with the lifted body and must not be lifted again.
        // `probe` is this rig's index of the leg mesh's hip bone (the thigh's parent), `before_z` / `after_z`
        // its height in the leg mesh before and after the edit. The shared lift is only added when this rig's
        // own hip is where the leg mesh's hip was before the edit (an independently animated mesh).
        void set_lift_probe(int32_t probe, double before_z, double after_z)
        {
            m_probe = probe;
            m_probe_before = before_z;
            m_probe_after = after_z;
        }
        // Leg rigs: the hip bone other meshes compare against, and its height before / after the last apply().
        int32_t hip_bone() const { return m_legs.empty() ? -1 : m_parents[m_legs.front().thigh]; }
        double hip_before() const { return m_hip_before; }
        double hip_after() const { return m_hip_after; }
        // Legless rigs: whether the last apply() added the shared lift (false = it follows the body already).
        bool took_shared_lift() const { return m_took_shared; }
        // The character's main mesh measures the lift; every other mesh of the character (even one with its own
        // legs, like a head mesh built on the full skeleton) first checks with the probe whether its pose is a copy
        // of the main mesh's final, already grounded pose - then it gets no ground correction of its own.
        // (A change forces a re-plan on the next bind: other meshes also rebuild the bones below a "this bone only" edit.)
        void set_ground_primary(bool primary)
        {
            if (primary != m_ground_primary)
            {
                m_ground_primary = primary;
                m_revision = ~0ull;
            }
        }
        // Secondary rigs: the last apply() found its pose copied from the main mesh's grounded pose.
        bool follows_body() const { return m_follows; }

        // ---- meshes that copy the main mesh's pose ----------------------------------
        // A head or outfit mesh driven by Copy Pose / Leader Pose from the main mesh already carries every edit
        // the main mesh got; editing it again doubles them (a shorter torso pulls its neck away from the body's).
        // The main mesh publishes, per bone, where it was before and after its edit (publish_pose); every
        // other mesh of the character is given those with a map from its bones to the main mesh's (-1 = none).
        // When its shared bones sit where the main mesh's ended up, the mesh is a copy: shared bones are left as
        // they are (no edits, no ground), and only bones the main mesh does not have get their sliders.
        void set_follow_reference(const int32_t* map, const double* before, const double* after, size_t main_bones)
        {
            m_map = map;
            m_ref_before = before;
            m_ref_after = after;
            m_ref_count = main_bones;
        }
        // Main mesh: per bone position x, y, z, scale x, y, z and rotation x, y, z, w (kPublishStride values) before and
        // after the last apply(). The rotation lets a turn-only slider (no bone moves, only turns) be recognised too.
        static constexpr size_t kPublishStride = 10;
        const std::vector<double>& published_before() const { return m_pub_before; }
        const std::vector<double>& published_after() const { return m_pub_after; }
        // Secondary meshes: 0 = not known yet, 1 = copies the main mesh's pose, 2 = animated on its own.
        int copy_state() const { return m_copy_state; }
        // Diagnostics: heights (Z) of a few landmark bones (pelvis, neck, head) in the game's pose and in
        // ours after the last apply(), plus the probe values it compared. Written to the log by the DLL.
        std::string ground_report() const;
        // Lift the last apply() used (cm, component space, extra included; 0 when nothing to do).
        double last_lift() const { return m_last_lift; }

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
        void apply_ground(Xform* pose);
        void decide_copy();
        void bend_leg(const LegChain& leg, size_t leg_index, double drop);
        double contact_sink(const LegChain& leg) const;

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

        // Auto Ground
        GroundOptions m_ground{};
        std::vector<LegChain> m_legs;
        // per bone: which leg's chain it hangs from (-1 none) and below which link:
        // 1 = thigh (not below the calf), 2 = calf (not below the foot), 3 = foot
        std::vector<int8_t> m_leg_of;
        std::vector<uint8_t> m_leg_part;
        double m_shared_lift{0.0};
        double m_last_lift{0.0};
        int32_t m_probe{-1};
        double m_probe_before{0.0};
        double m_probe_after{0.0};
        double m_hip_before{0.0};
        double m_hip_after{0.0};
        bool m_took_shared{false};
        bool m_ground_primary{true};
        bool m_follows{false};
        // copy detection
        const int32_t* m_map{nullptr};
        const double* m_ref_before{nullptr};
        const double* m_ref_after{nullptr};
        size_t m_ref_count{0};
        int m_copy_state{0};
        int m_copy_candidate{0};
        int m_copy_votes{0};
        double m_copy_err_after{0.0};
        double m_copy_err_before{0.0};
        std::vector<double> m_pub_before;
        std::vector<double> m_pub_after;
        const Xform* m_scratch_pose{nullptr}; // the buffer apply() is working on (for decide_copy)
        std::array<int32_t, 3> m_marks{-1, -1, -1}; // pelvis, neck, head (diagnostics)
        std::array<double, 3> m_mark_src{};
        std::array<double, 3> m_mark_out{};
        double m_probe_seen{0.0};
        // per leg, last apply(): ankle sink, sole sink, most lift its reach allows; and the lift before the reach limit
        std::array<std::array<double, 3>, 4> m_leg_diag{};
        double m_raw_lift{0.0};
    };
} // namespace uuepbs
