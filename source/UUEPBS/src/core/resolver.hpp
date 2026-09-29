// UUEPBS - finds the pose hook point in an unknown Unreal build
//
// Everything here only reads memory through a MemoryView, so it runs the same
// against the live game (Windows) and against an exe file on disk (tests).
//
// What we are looking for, in every UE4.2x / UE5.x build:
//
//   void USkeletalMeshComponent::FinalizeBoneTransform()      (virtual)
//   {
//       Super::FinalizeBoneTransform();      // -> FlipEditableSpaceBases(); bHasValidBoneTransform = true;
//       ConditionallyDispatchQueuedAnimEvents(); ...
//   }
//
//   FlipEditableSpaceBases swaps two int32 members that index the double-buffered
//   TArray<FTransform> ComponentSpaceTransformsArray[2]:
//       CurrentReadComponentTransforms / CurrentEditableComponentTransforms
//
// 1. find_finalize_candidates walks the component's vtable and keeps functions
//    whose first calls lead to code that swaps two adjacent int32 members, ranked
//    by how closely they match (a flag byte being set right after the call, etc.).
// 2. find_pose_layout checks a live component: the two ints must hold 0/1 and two
//    TArrays with exactly <bone count> transforms must sit just before them. The
//    transform size (96 = double / UE5, 48 = float / UE4) is read off the data.
#pragma once

#include "registry.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace uuepbs
{
    class MemoryView
    {
      public:
        virtual ~MemoryView() = default;
        virtual bool read(uintptr_t address, void* out, size_t size) const = 0;
        virtual bool is_code(uintptr_t address) const = 0;
    };

    struct FlipOffsets
    {
        uint32_t editable{};
        uint32_t read{};
        bool certain{}; // true when the code shows which one is the read index
        // Arrays the code indexes with those ints: base + index * 16 (the pose buffers and the
        // bone visibility buffers). Lets the layout check pick the right pair of arrays.
        uint32_t bases[4]{};
        int base_count{};
    };

    std::optional<FlipOffsets> parse_flip(const MemoryView& mem, uintptr_t function);

    struct FinalizeCandidate
    {
        int slot{-1};
        uintptr_t function{};
        uintptr_t flip{};
        FlipOffsets offsets{};
        int score{};
        int depth{};
    };

    // Best first. `first`/`last` bound the vtable slots examined.
    std::vector<FinalizeCandidate> find_finalize_candidates(const MemoryView& mem, uintptr_t vtable, int first = 40, int last = 1400);

    // Second chance for builds whose flip code the pattern search misses (e.g. UE 5.0): the
    // memory search already knows where the index ints are, so look for vtable functions that
    // write exactly those members. `pair_starts` = offsets x where x and x+4 both hold 0/1.
    std::optional<FlipOffsets> parse_flip_at(const MemoryView& mem, uintptr_t function, const std::vector<uint32_t>& pair_starts);
    std::vector<FinalizeCandidate> find_finalize_by_members(const MemoryView& mem, uintptr_t vtable, const std::vector<uint32_t>& pair_starts,
                                                            int first = 40, int last = 1400);
    // Offsets x in [from, to) where the component holds 0/1 at both x and x+4.
    std::vector<uint32_t> index_pair_candidates(const MemoryView& mem, uintptr_t component, uint32_t from, uint32_t to);

    struct LayoutProbe
    {
        std::optional<PoseLayout> layout;
        bool empty_pose{}; // arrays found but the component has not produced a pose yet
        bool unsettled{};  // the code's buffer index members do not hold 0/1 right now (e.g. mid level load)
        std::string note;
    };

    // `hint` = offsets from the code (nullptr: search the component memory alone).
    LayoutProbe find_pose_layout(const MemoryView& mem, uintptr_t component, int32_t bones, const FlipOffsets* hint);

    bool plausible_float_transform(const XformF& x);
} // namespace uuepbs
