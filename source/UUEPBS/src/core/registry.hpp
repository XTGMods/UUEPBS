// UUEPBS - shared state between the pose hook (game thread), the Lua
// bridge (its own thread) and the slider window (its own UI thread).
#pragma once

#include "mirror.hpp"
#include "morphs.hpp"
#include "sculpt.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace uuepbs
{
    // Where USkinnedMeshComponent keeps its double-buffered component-space pose.
    // Filled in by the hook installer (derived from the game's machine code).
    struct PoseLayout
    {
        uint32_t buffers{0};    // TArray<FTransform> ComponentSpaceTransformsArray[2]
        uint32_t read_index{0}; // int32 CurrentReadComponentTransforms
        uint32_t array_stride{16};
        uint32_t elem_size{96}; // 96 = UE5 FTransform (double), 48 = UE4 FTransform (float)
        bool valid() const { return buffers != 0 && read_index != 0 && (elem_size == 96 || elem_size == 48); }
    };

    // UE4 FTransform: three 16-byte float vectors.
    struct XformF
    {
        float rot[4];
        float pos[4];
        float scl[4];
    };
    static_assert(sizeof(XformF) == 48, "XformF must match UE4 FTransform layout");

    // Reads memory that may have been freed (addresses come from another thread).
    // Returns false instead of faulting. Tests use a plain memcpy.
    using SafeReader = bool (*)(uintptr_t address, void* out, size_t size);

    struct RawArray
    {
        void* data;
        int32_t num;
        int32_t max;
    };

    // Snapshot of the skeleton the window shows (taken from the primary rig).
    struct SkeletonView
    {
        std::vector<std::string> names;
        std::vector<int32_t> parents;
        std::vector<int32_t> depth;
        std::string label;
        std::string owner;
    };

    struct RigSummary
    {
        std::string label;
        std::string owner;
        int32_t bones{};
        uint64_t frames{};
        bool stale{};
        bool primary{};
    };

    class Registry
    {
      public:
        static constexpr size_t kMaxRigs = 32;

        static Registry& instance();

        void set_safe_reader(SafeReader reader) { m_read = reader; }

        // ---- bridge side ----------------------------------------------------
        // `primary` rigs feed the bone list shown in the window. `owner` is the actor's display name.
        bool track(uintptr_t component, const std::string& label, const std::string& owner, std::vector<std::string> names,
                   std::vector<int32_t> parents, bool primary, std::string& message);
        // Parent-relative reference pose of a tracked rig (for measuring left/right mirroring).
        void set_reference_pose(uintptr_t component, std::vector<Xform> local_pose);
        std::vector<uintptr_t> tracked_components() const;
        uintptr_t primary_component() const;
        void untrack(uintptr_t component);
        void untrack_all();
        bool is_tracked(uintptr_t component) const;

        // ---- hook side (game thread) ----------------------------------------
        void set_layout(const PoseLayout& layout);
        PoseLayout layout() const;
        void on_pose_finalized(void* component);

        // ---- UI / preset side -----------------------------------------------
        EditBook edits(uint64_t* revision = nullptr) const;
        uint64_t edit_revision() const { return m_edit_revision.load(std::memory_order_acquire); }
        void set_bone(const std::string& bone, const BoneEdit& scale);
        void clear_bone(const std::string& bone);
        void replace_edits(EditBook book);
        void clear_edits();

        bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
        void set_enabled(bool on); // also bumps the morph revision (morphs are sent to Lua only while enabled)

        // ---- morph targets (weights applied by the Lua script) ------------------
        MorphBook morphs(uint64_t* revision = nullptr) const;
        uint64_t morph_revision() const { return m_morph_revision.load(std::memory_order_acquire); }
        void set_morph(const std::string& morph, double weight); // present = overrides the game's value
        void clear_morph(const std::string& morph);              // back to the game's own value
        void replace_morphs(MorphBook book);
        void clear_morphs();
        // Morph names found on the tracked meshes (primary mesh first, no duplicates).
        void set_morph_names(std::vector<std::string> names);
        std::vector<std::string> morph_names(uint64_t* revision = nullptr) const;
        uint64_t morph_names_revision() const { return m_morph_names_revision.load(std::memory_order_acquire); }
        // Morphs that something else keeps setting (reported by Lua): folded names.
        void set_animated_morphs(std::set<std::string> folded);
        std::set<std::string> animated_morphs() const;

        SkeletonView skeleton(uint64_t* revision = nullptr) const;
        MirrorTable mirror_table() const;
        std::string mirror_source() const;
        BoneEdit mirror_edit(const std::string& bone, const BoneEdit& edit) const;
        uint64_t skeleton_revision() const { return m_skeleton_revision.load(std::memory_order_acquire); }
        std::vector<RigSummary> rigs() const;

        // A rescan is wanted when a tracked mesh changed or went away, or the user asked.
        void request_rescan() { m_rescan.store(true, std::memory_order_release); }
        bool take_rescan_request() { return m_rescan.exchange(false, std::memory_order_acq_rel); }

      private:
        struct Rig
        {
            uintptr_t component{};
            uintptr_t vtable{};
            int32_t object_index{};
            uintptr_t object_class{};
            std::string label;
            std::string owner;
            bool primary{};
            bool stale{};
            uint64_t frames{};
            RigSculptor sculptor;
            std::vector<Xform> scratch; // float poses are converted here
            std::vector<int32_t> inputs; // bones converted for the current frame
        };

        bool peek_raw(uintptr_t address, void* out, size_t size) const;
        void measure_mirror_locked(const Rig& rig, const std::vector<Xform>& pose, bool local, const char* source);

        Rig* find_rig(uintptr_t component);
        const Rig* find_rig(uintptr_t component) const;
        void rebuild_hot_list();
        void rebuild_skeleton_view();
        void bump_edits();

        mutable std::mutex m_lock;
        std::vector<std::unique_ptr<Rig>> m_rigs;
        std::array<std::atomic<uintptr_t>, kMaxRigs> m_hot{};
        std::atomic<size_t> m_hot_count{0}; // used slots of m_hot (the detour's fast reject)

        EditBook m_book;
        EditBook m_empty_book;
        std::atomic<uint64_t> m_edit_revision{1};
        std::atomic<bool> m_enabled{true};

        SkeletonView m_view;
        std::atomic<uint64_t> m_skeleton_revision{0};

        PoseLayout m_layout{};
        std::atomic<bool> m_rescan{false};

        mutable std::mutex m_morph_lock; // separate from m_lock: the pose hook never waits on morph edits
        MorphBook m_morphs;
        std::atomic<uint64_t> m_morph_revision{1};
        std::vector<std::string> m_morph_names;
        std::atomic<uint64_t> m_morph_names_revision{1};
        std::set<std::string> m_animated_morphs;

        MirrorTable m_mirror{};
        std::string m_mirror_source{"not measured yet"};
        bool m_mirror_from_reference{false};
        SafeReader m_read{};
    };
} // namespace uuepbs
