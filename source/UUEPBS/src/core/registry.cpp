#include "registry.hpp"

#include <algorithm>
#include <cstring>

namespace uuepbs
{
    namespace
    {
        // UObjectBase members used to make sure a tracked address still holds the same
        // component (the allocator may hand a freed component's memory to a new one).
        constexpr uintptr_t kObjIndexOffset = 0x0C; // int32 InternalIndex
        constexpr uintptr_t kObjClassOffset = 0x10; // UClass* ClassPrivate
        // (same in UE4 and UE5)

        template <typename T> T peek(uintptr_t address)
        {
            T value;
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
            return value;
        }

        constexpr uint64_t kDisabledBit = 1ull << 63;

        // Frames to wait before measuring mirroring from a live pose (lets the idle animation settle).
        constexpr uint64_t kLiveMirrorFrame = 30;
    } // namespace

    bool Registry::peek_raw(uintptr_t address, void* out, size_t size) const
    {
        if (m_read)
        {
            return m_read(address, out, size);
        }
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    }

    void Registry::measure_mirror_locked(const Rig& rig, const std::vector<Xform>& pose, bool local, const char* source)
    {
        MirrorTable table = build_mirror_table(rig.sculptor.names(), rig.sculptor.parents(), pose, local);
        if (table.centre_axis < 0)
        {
            m_mirror_source = std::string("could not measure from ") + source + " (no left/right bone pairs found)";
            return;
        }
        static const char* axis_names[3] = {"X", "Y", "Z"};
        m_mirror_source = std::string("measured from ") + source + ": " + std::to_string(table.pairs) + " pairs across " +
                          axis_names[table.centre_axis] + (table.inexact ? ", " + std::to_string(table.inexact) + " uncertain" : "");
        m_mirror = std::move(table);
    }

    Registry& Registry::instance()
    {
        static Registry registry;
        return registry;
    }

    Registry::Rig* Registry::find_rig(uintptr_t component)
    {
        for (auto& rig : m_rigs)
        {
            if (rig->component == component)
            {
                return rig.get();
            }
        }
        return nullptr;
    }

    const Registry::Rig* Registry::find_rig(uintptr_t component) const
    {
        for (const auto& rig : m_rigs)
        {
            if (rig->component == component)
            {
                return rig.get();
            }
        }
        return nullptr;
    }

    void Registry::rebuild_hot_list()
    {
        size_t i = 0;
        for (; i < m_rigs.size() && i < kMaxRigs; ++i)
        {
            m_hot[i].store(m_rigs[i]->component, std::memory_order_release);
        }
        const size_t used = i;
        for (; i < kMaxRigs; ++i)
        {
            m_hot[i].store(0, std::memory_order_release);
        }
        m_hot_count.store(used, std::memory_order_release);
    }

    void Registry::rebuild_skeleton_view()
    {
        const Rig* source = nullptr;
        for (const auto& rig : m_rigs)
        {
            if (rig->primary && !rig->stale)
            {
                source = rig.get();
                break;
            }
        }
        if (!source)
        {
            for (const auto& rig : m_rigs)
            {
                if (!rig->stale && (!source || rig->sculptor.bone_count() > source->sculptor.bone_count()))
                {
                    source = rig.get();
                }
            }
        }

        SkeletonView view;
        if (source)
        {
            view.names = source->sculptor.names();
            view.parents = source->sculptor.parents();
            view.depth.assign(view.names.size(), 0);
            for (size_t i = 0; i < view.parents.size(); ++i)
            {
                const int32_t p = view.parents[i];
                view.depth[i] = p >= 0 ? view.depth[p] + 1 : 0;
            }
            view.label = source->label;
            view.owner = source->owner;
        }
        if (view.names != m_view.names)
        {
            m_mirror = {};
            m_mirror_from_reference = false;
            m_mirror_source = view.names.empty() ? "not measured yet" : "waiting for a pose to measure";
        }
        m_view = std::move(view);
        m_skeleton_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    bool Registry::track(uintptr_t component, const std::string& label, const std::string& owner, std::vector<std::string> names,
                         std::vector<int32_t> parents, bool primary, std::string& message)
    {
        if (component == 0)
        {
            message = "null component";
            return false;
        }
        uintptr_t vtable = 0;
        int32_t object_index = 0;
        uintptr_t object_class = 0;
        if (!peek_raw(component, &vtable, sizeof(vtable)) || !peek_raw(component + kObjIndexOffset, &object_index, sizeof(object_index)) ||
            !peek_raw(component + kObjClassOffset, &object_class, sizeof(object_class)) || vtable == 0)
        {
            message = label + ": component memory is not readable";
            return false;
        }

        std::lock_guard guard(m_lock);
        Rig* rig = find_rig(component);
        if (!rig)
        {
            if (m_rigs.size() >= kMaxRigs)
            {
                message = "too many tracked meshes";
                return false;
            }
            m_rigs.push_back(std::make_unique<Rig>());
            rig = m_rigs.back().get();
        }

        // Re-registering the same skeleton keeps the per-buffer memory intact.
        const bool unchanged = rig->component == component && rig->sculptor.names() == names && rig->sculptor.parents() == parents;
        std::string why;
        if (!unchanged && !rig->sculptor.set_skeleton(std::move(names), std::move(parents), &why))
        {
            m_rigs.erase(std::remove_if(m_rigs.begin(), m_rigs.end(),
                                        [&](const auto& r) {
                                            return r.get() == rig;
                                        }),
                         m_rigs.end());
            rebuild_hot_list();
            message = why;
            return false;
        }

        rig->component = component;
        rig->vtable = vtable;
        rig->object_index = object_index;
        rig->object_class = object_class;
        rig->label = label;
        rig->owner = owner;
        rig->primary = primary;
        rig->stale = false;
        rig->frames = 0;

        rebuild_hot_list();
        rebuild_skeleton_view();
        message = "tracking " + label + " (" + std::to_string(rig->sculptor.bone_count()) + " bones)";
        return true;
    }

    void Registry::set_reference_pose(uintptr_t component, std::vector<Xform> local_pose)
    {
        std::lock_guard guard(m_lock);
        const Rig* rig = find_rig(component);
        if (!rig || local_pose.size() != static_cast<size_t>(rig->sculptor.bone_count()) || m_view.names != rig->sculptor.names())
        {
            return;
        }
        measure_mirror_locked(*rig, local_pose, true, "the reference pose");
        m_mirror_from_reference = m_mirror.centre_axis >= 0;
    }

    std::vector<uintptr_t> Registry::tracked_components() const
    {
        std::lock_guard guard(m_lock);
        std::vector<uintptr_t> out;
        for (const auto& rig : m_rigs)
        {
            if (!rig->stale)
            {
                out.push_back(rig->component);
            }
        }
        return out;
    }

    uintptr_t Registry::primary_component() const
    {
        std::lock_guard guard(m_lock);
        uintptr_t best = 0;
        int32_t bones = -1;
        for (const auto& rig : m_rigs)
        {
            if (rig->stale)
            {
                continue;
            }
            if (rig->primary)
            {
                return rig->component;
            }
            if (rig->sculptor.bone_count() > bones)
            {
                bones = rig->sculptor.bone_count();
                best = rig->component;
            }
        }
        return best;
    }

    void Registry::untrack(uintptr_t component)
    {
        std::lock_guard guard(m_lock);
        const auto before = m_rigs.size();
        m_rigs.erase(std::remove_if(m_rigs.begin(), m_rigs.end(),
                                    [&](const auto& r) {
                                        return r->component == component;
                                    }),
                     m_rigs.end());
        if (m_rigs.size() != before)
        {
            rebuild_hot_list();
            rebuild_skeleton_view();
        }
    }

    void Registry::untrack_all()
    {
        std::lock_guard guard(m_lock);
        m_rigs.clear();
        rebuild_hot_list();
        rebuild_skeleton_view();
    }

    bool Registry::is_tracked(uintptr_t component) const
    {
        std::lock_guard guard(m_lock);
        const Rig* rig = find_rig(component);
        return rig && !rig->stale;
    }

    void Registry::set_layout(const PoseLayout& layout)
    {
        std::lock_guard guard(m_lock);
        m_layout = layout;
    }

    PoseLayout Registry::layout() const
    {
        std::lock_guard guard(m_lock);
        return m_layout;
    }

    void Registry::on_pose_finalized(void* component)
    {
        // Runs for every skeletal mesh in the game, every frame: reject untracked ones
        // with a few plain loads before touching the lock.
        const size_t used = m_hot_count.load(std::memory_order_acquire);
        if (used == 0)
        {
            return;
        }
        const uintptr_t self = reinterpret_cast<uintptr_t>(component);
        bool hot = false;
        for (size_t i = 0; i < used; ++i)
        {
            if (m_hot[i].load(std::memory_order_relaxed) == self)
            {
                hot = true;
                break;
            }
        }
        if (!hot)
        {
            return;
        }

        std::lock_guard guard(m_lock);
        Rig* rig = find_rig(self);
        if (!rig || rig->stale || !m_layout.valid())
        {
            return;
        }

        // Same object as when Lua registered it?
        if (peek<uintptr_t>(self) != rig->vtable || peek<int32_t>(self + kObjIndexOffset) != rig->object_index ||
            peek<uintptr_t>(self + kObjClassOffset) != rig->object_class)
        {
            rig->stale = true;
            rebuild_skeleton_view();
            request_rescan();
            return;
        }

        const int32_t read = peek<int32_t>(self + m_layout.read_index);
        if (read != 0 && read != 1)
        {
            return;
        }
        const RawArray pose = peek<RawArray>(self + m_layout.buffers + static_cast<uintptr_t>(read) * m_layout.array_stride);
        if (!pose.data || pose.num <= 0)
        {
            return;
        }
        if (pose.num != rig->sculptor.bone_count())
        {
            // Mesh swapped under us: stop touching it until Lua re-registers the bones.
            rig->stale = true;
            rig->sculptor.forget_buffers();
            rebuild_skeleton_view();
            request_rescan();
            return;
        }

        const bool on = m_enabled.load(std::memory_order_relaxed);
        const uint64_t wanted = m_edit_revision.load(std::memory_order_acquire) | (on ? 0 : kDisabledBit);
        if (rig->sculptor.bound_revision() != wanted)
        {
            rig->sculptor.bind(on ? m_book : m_empty_book, wanted);
        }
        // No reference pose from Lua: measure mirroring from an early live frame instead
        // (before this frame's edits are applied).
        if (rig->frames + 1 == kLiveMirrorFrame && !m_mirror_from_reference && m_mirror.centre_axis < 0 && m_view.names == rig->sculptor.names())
        {
            std::vector<Xform> live(static_cast<size_t>(pose.num));
            for (int32_t i = 0; i < pose.num; ++i)
            {
                if (m_layout.elem_size == 96)
                {
                    live[i] = static_cast<const Xform*>(pose.data)[i];
                }
                else
                {
                    const XformF& f = static_cast<const XformF*>(pose.data)[i];
                    for (int a = 0; a < 4; ++a)
                    {
                        live[i].rot[a] = f.rot[a];
                        live[i].pos[a] = f.pos[a];
                        live[i].scl[a] = f.scl[a];
                    }
                }
            }
            measure_mirror_locked(*rig, live, false, "a live pose");
        }

        ++rig->frames;
        if (!rig->sculptor.has_work(pose.data))
        {
            return; // no sliders moved and nothing of ours to undo: leave the pose alone
        }
        if (m_layout.elem_size == 96)
        {
            rig->sculptor.set_quantize(false);
            rig->sculptor.apply(static_cast<Xform*>(pose.data), pose.num);
        }
        else
        {
            // UE4: widen the bones the sculptor reads to double, sculpt, write back the ones it
            // changed. Unchanged bones round-trip exactly; untouched bones are never converted.
            auto* floats = static_cast<XformF*>(pose.data);
            rig->scratch.resize(static_cast<size_t>(pose.num));
            rig->sculptor.gather_inputs(pose.data, rig->inputs);
            for (const int32_t i : rig->inputs)
            {
                for (int a = 0; a < 4; ++a)
                {
                    rig->scratch[i].rot[a] = floats[i].rot[a];
                    rig->scratch[i].pos[a] = floats[i].pos[a];
                    rig->scratch[i].scl[a] = floats[i].scl[a];
                }
            }
            rig->sculptor.set_quantize(true);
            if (rig->sculptor.apply(rig->scratch.data(), pose.num, pose.data) > 0)
            {
                for (const int32_t i : rig->sculptor.touched())
                {
                    for (int a = 0; a < 4; ++a)
                    {
                        floats[i].rot[a] = static_cast<float>(rig->scratch[i].rot[a]);
                        floats[i].pos[a] = static_cast<float>(rig->scratch[i].pos[a]);
                        floats[i].scl[a] = static_cast<float>(rig->scratch[i].scl[a]);
                    }
                }
            }
        }
    }

    void Registry::bump_edits()
    {
        m_edit_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    EditBook Registry::edits(uint64_t* revision) const
    {
        std::lock_guard guard(m_lock);
        if (revision)
        {
            *revision = m_edit_revision.load(std::memory_order_acquire);
        }
        return m_book;
    }

    void Registry::set_bone(const std::string& bone, const BoneEdit& scale)
    {
        if (bone.empty())
        {
            return;
        }
        BoneEdit s = scale;
        s.clamp();
        std::lock_guard guard(m_lock);
        EditEntry& entry = m_book[fold_case(bone)];
        entry.bone = bone;
        entry.edit = s;
        bump_edits();
    }

    void Registry::clear_bone(const std::string& bone)
    {
        std::lock_guard guard(m_lock);
        if (m_book.erase(fold_case(bone)) > 0)
        {
            bump_edits();
        }
    }

    void Registry::replace_edits(EditBook book)
    {
        std::lock_guard guard(m_lock);
        m_book = std::move(book);
        bump_edits();
    }

    void Registry::clear_edits()
    {
        std::lock_guard guard(m_lock);
        m_book.clear();
        bump_edits();
    }

    void Registry::set_enabled(bool on)
    {
        m_enabled.store(on, std::memory_order_relaxed);
    }

    SkeletonView Registry::skeleton(uint64_t* revision) const
    {
        std::lock_guard guard(m_lock);
        if (revision)
        {
            *revision = m_skeleton_revision.load(std::memory_order_acquire);
        }
        return m_view;
    }

    std::vector<RigSummary> Registry::rigs() const
    {
        std::lock_guard guard(m_lock);
        std::vector<RigSummary> out;
        out.reserve(m_rigs.size());
        for (const auto& rig : m_rigs)
        {
            out.push_back({rig->label, rig->owner, rig->sculptor.bone_count(), rig->frames, rig->stale, rig->primary});
        }
        return out;
    }

    MirrorTable Registry::mirror_table() const
    {
        std::lock_guard guard(m_lock);
        return m_mirror;
    }

    std::string Registry::mirror_source() const
    {
        std::lock_guard guard(m_lock);
        return m_mirror_source;
    }

    BoneEdit Registry::mirror_edit(const std::string& bone, const BoneEdit& edit) const
    {
        std::lock_guard guard(m_lock);
        return mirror_with(m_mirror, bone, edit);
    }
} // namespace uuepbs
