#include "pose_hook.hpp"

#include "../core/registry.hpp"
#include "../core/resolver.hpp"
#include "../core/xform.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace uuepbs::hook
{
    namespace
    {
        using FinalizeFn = void (*)(void*);
        using Clock = std::chrono::steady_clock;

        // The hook finder makes thousands of small reads; asking the kernel about every one
        // (VirtualQuery) was most of its cost. The last readable region is remembered for a
        // short while per thread. A region freed in that window still cannot crash anything:
        // the copy itself is guarded by structured exception handling.
        struct RegionMemo
        {
            uintptr_t lo{};
            uintptr_t hi{};
            ULONGLONG stamp{};
        };
        thread_local RegionMemo t_region{};
#ifdef _MSC_VER
        constexpr ULONGLONG kRegionMemoMs = 200;
#else
        constexpr ULONGLONG kRegionMemoMs = 0; // no SEH guard in this build: always ask
#endif

        bool region_ok(const MEMORY_BASIC_INFORMATION& mbi)
        {
            const DWORD prot = mbi.Protect & 0xFF;
            return mbi.State == MEM_COMMIT && prot != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD);
        }

        bool readable(uintptr_t address, size_t size)
        {
            uintptr_t at = address;
            const uintptr_t stop = address + size;
            if (address == 0 || stop < address)
            {
                return false;
            }
            const ULONGLONG now = GetTickCount64();
            if (address >= t_region.lo && stop <= t_region.hi && now - t_region.stamp < kRegionMemoMs)
            {
                return true;
            }
            while (at < stop)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) || !region_ok(mbi))
                {
                    return false;
                }
                const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                const uintptr_t hi = lo + mbi.RegionSize;
                if (at == address && stop <= hi)
                {
                    t_region = {lo, hi, now};
                }
                at = hi;
            }
            return true;
        }

#ifdef _MSC_VER
        // No C++ objects in here, so structured exception handling is allowed.
        bool guarded_copy(uintptr_t address, void* out, size_t size)
        {
            __try
            {
                std::memcpy(out, reinterpret_cast<const void*>(address), size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
#else
        bool guarded_copy(uintptr_t address, void* out, size_t size)
        {
            std::memcpy(out, reinterpret_cast<const void*>(address), size);
            return true;
        }
#endif

        // Executable sections of the module that contains an address.
        struct CodeRanges
        {
            HMODULE module{};
            uintptr_t base{};
            std::vector<std::pair<uintptr_t, uintptr_t>> ranges;

            bool contains(uintptr_t p) const
            {
                for (const auto& r : ranges)
                {
                    if (p >= r.first && p < r.second)
                    {
                        return true;
                    }
                }
                return false;
            }
        };

        CodeRanges code_of_module_at(uintptr_t address)
        {
            CodeRanges c;
            HMODULE mod = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &mod) ||
                !mod)
            {
                return c;
            }
            const auto base = reinterpret_cast<uintptr_t>(mod);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            {
                if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
                {
                    const uintptr_t start = base + sec[i].VirtualAddress;
                    c.ranges.emplace_back(start, start + std::max<DWORD>(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData));
                }
            }
            c.module = mod;
            c.base = base;
            return c;
        }

        class LiveMemory final : public MemoryView
        {
          public:
            explicit LiveMemory(CodeRanges code) : m_code(std::move(code)) {}
            bool read(uintptr_t address, void* out, size_t size) const override { return safe_read(address, out, size); }
            bool is_code(uintptr_t address) const override { return m_code.contains(address); }
            const CodeRanges& code() const { return m_code; }

          private:
            CodeRanges m_code;
        };

        struct Patch
        {
            void** entry{};
            void* original{};
        };

        struct Choice
        {
            int slot{-1};
            PoseLayout layout{};
            std::string how;
        };

        constexpr size_t kMaxPatches = 8;
        std::array<std::atomic<void*>, kMaxPatches> g_vtables{};
        std::array<std::atomic<void*>, kMaxPatches> g_originals{};
        std::array<Patch, kMaxPatches> g_patches{};
        std::array<std::atomic<bool>, kMaxPatches> g_active_index{};
        std::atomic<size_t> g_patch_count{0};
        std::atomic<uint64_t> g_calls{0};
        std::mutex g_lock;

        Choice g_choice{};
        std::vector<Choice> g_spares; // other validated candidates, in case the first never runs
        uintptr_t g_module_base{};
        Clock::time_point g_installed_at{};
        bool g_confirmed{false};

        void finalize_detour(void* self)
        {
            void* const vtable = *static_cast<void**>(self);
            FinalizeFn engine = nullptr;
            const size_t count = g_patch_count.load(std::memory_order_acquire);
            // The record that is patched in right now for this vtable; any record of it otherwise.
            for (size_t i = 0; i < count; ++i)
            {
                if (g_vtables[i].load(std::memory_order_relaxed) == vtable)
                {
                    engine = reinterpret_cast<FinalizeFn>(g_originals[i].load(std::memory_order_relaxed));
                    if (g_active_index[i].load(std::memory_order_relaxed))
                    {
                        break;
                    }
                }
            }
            if (!engine)
            {
                return; // cannot happen: only patched vtables lead here
            }
            engine(self);
            g_calls.fetch_add(1, std::memory_order_relaxed);

            try
            {
                Registry::instance().on_pose_finalized(self);
            }
            catch (...)
            {
            }
        }

        std::string hex(uintptr_t v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
            return buf;
        }

        bool write_entry(void** entry, void* value)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            const bool exec = VirtualQuery(entry, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                                                         PAGE_EXECUTE_WRITECOPY));
            DWORD old_protect = 0;
            if (!VirtualProtect(entry, sizeof(void*), exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old_protect))
            {
                return false;
            }
            InterlockedExchangePointer(entry, value);
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(void*), old_protect, &ignored);
            return true;
        }

        bool patch_vtable(void** vtable, int slot, std::string& why)
        {
            void** entry = &vtable[slot];
            void* const current = *entry;
            if (current == reinterpret_cast<void*>(&finalize_detour))
            {
                return true;
            }
            const size_t count = g_patch_count.load(std::memory_order_acquire);
            // Same vtable entry patched before (then put back): reuse its record.
            size_t index = count;
            for (size_t i = 0; i < count; ++i)
            {
                if (g_patches[i].original == current && g_vtables[i].load() == vtable)
                {
                    index = i;
                    break;
                }
            }
            if (index == count)
            {
                if (count >= kMaxPatches)
                {
                    why = "too many component classes hooked";
                    return false;
                }
                g_vtables[count].store(vtable, std::memory_order_relaxed);
                g_originals[count].store(current, std::memory_order_relaxed);
                g_patches[count] = Patch{nullptr, current};
                g_patch_count.store(count + 1, std::memory_order_release);
            }
            if (!write_entry(entry, reinterpret_cast<void*>(&finalize_detour)))
            {
                why = "VirtualProtect failed (" + std::to_string(GetLastError()) + ")";
                return false;
            }
            g_patches[index].entry = entry;
            // Newest active record for this vtable must win in the detour's lookup.
            g_active_index[index].store(true);
            return true;
        }

        void unpatch_all()
        {
            const size_t count = g_patch_count.load(std::memory_order_acquire);
            for (size_t i = 0; i < count; ++i)
            {
                Patch& p = g_patches[i];
                if (p.entry && *p.entry == reinterpret_cast<void*>(&finalize_detour))
                {
                    write_entry(p.entry, p.original);
                }
                p.entry = nullptr;
                g_active_index[i].store(false);
            }
            // The records stay: an engine thread may still be inside the detour, and a
            // later patch of the same entry reuses its record.
        }

        bool is_patched(void** vtable)
        {
            const size_t count = g_patch_count.load(std::memory_order_acquire);
            for (size_t i = 0; i < count; ++i)
            {
                if (g_vtables[i].load() == vtable && g_patches[i].entry)
                {
                    return true;
                }
            }
            return false;
        }

        // find_finalize_candidates decodes a few hundred vtable functions; the answer never
        // changes for a vtable, so it is worked out once per vtable instead of once per retry.
        std::mutex g_cache_lock;
        std::map<uintptr_t, std::vector<FinalizeCandidate>> g_candidate_cache;

        std::vector<FinalizeCandidate> candidates_for(const MemoryView& mem, uintptr_t vtable)
        {
            {
                std::lock_guard guard(g_cache_lock);
                const auto it = g_candidate_cache.find(vtable);
                if (it != g_candidate_cache.end())
                {
                    return it->second;
                }
            }
            std::vector<FinalizeCandidate> found = find_finalize_candidates(mem, vtable);
            if (found.empty())
            {
                return found; // not remembered: a later look may read the code fine
            }
            std::lock_guard guard(g_cache_lock);
            if (g_candidate_cache.size() > 16)
            {
                g_candidate_cache.clear();
            }
            g_candidate_cache[vtable] = found;
            return found;
        }

        bool not_ready(const LayoutProbe& p) { return p.empty_pose || p.unsettled; }

        std::set<int> g_rejected;
        struct Override
        {
            int slot{-1};
            uint32_t buffers{};
            uint32_t read{};
        } g_override;
        bool g_warned_idle{false};

        bool active_locked()
        {
            const size_t count = g_patch_count.load(std::memory_order_acquire);
            for (size_t i = 0; i < count; ++i)
            {
                if (g_patches[i].entry)
                {
                    return true;
                }
            }
            return false;
        }

        std::string describe_locked()
        {
            if (!active_locked() || g_choice.slot < 0)
            {
                return "pose hook not installed";
            }
            uintptr_t vt = 0;
            const size_t count = g_patch_count.load(std::memory_order_acquire);
            for (size_t i = 0; i < count && !vt; ++i)
            {
                if (g_patches[i].entry)
                {
                    vt = reinterpret_cast<uintptr_t>(g_vtables[i].load());
                }
            }
            return std::string(g_confirmed ? "pose hook live" : "pose hook installed") + " on vtable " + hex(vt - g_module_base) + " slot " +
                   std::to_string(g_choice.slot) + ", buffers +" + hex(g_choice.layout.buffers) + ", read index +" + hex(g_choice.layout.read_index) +
                   (g_choice.layout.elem_size == 96 ? ", UE5 double transforms" : ", UE4 float transforms");
        }
    } // namespace

    bool safe_read(uintptr_t address, void* out, size_t size)
    {
        if (!readable(address, size))
        {
            return false;
        }
        return guarded_copy(address, out, size);
    }

    Report install_for(void* component, int32_t expected_bones)
    {
        std::lock_guard guard(g_lock);
        Report report;
        const auto comp = reinterpret_cast<uintptr_t>(component);
        uintptr_t vt_addr = 0;
        if (!comp || !safe_read(comp, &vt_addr, sizeof(vt_addr)))
        {
            report.message = "component address is not readable";
            return report;
        }
        void** vtable = reinterpret_cast<void**>(vt_addr);
        if (is_patched(vtable))
        {
            report.outcome = Outcome::Installed;
            report.message = describe_locked();
            return report;
        }

        LiveMemory mem(code_of_module_at(vt_addr));
        if (mem.code().ranges.empty())
        {
            report.message = "component vtable is not inside a loaded module";
            return report;
        }

        // A second component class (e.g. a game-specific subclass): must show the same
        // slot and the same pose layout before its vtable is touched.
        if (g_choice.slot >= 0)
        {
            const FinalizeCandidate* same = nullptr;
            const std::vector<FinalizeCandidate> mine = candidates_for(mem, vt_addr);
            for (const FinalizeCandidate& c : mine)
            {
                if (c.slot == g_choice.slot)
                {
                    same = &c;
                    break;
                }
            }
            if (!same)
            {
                report.message = "this component class has no matching FinalizeBoneTransform at slot " + std::to_string(g_choice.slot);
                return report;
            }
            const LayoutProbe probe = find_pose_layout(mem, comp, expected_bones, &same->offsets);
            if (!probe.layout)
            {
                report.outcome = not_ready(probe) ? Outcome::NotReadyYet : Outcome::Unsupported;
                report.message = probe.empty_pose ? "component has no pose yet" : "second component class: " + probe.note;
                return report;
            }
            if (probe.layout->buffers != g_choice.layout.buffers || probe.layout->read_index != g_choice.layout.read_index ||
                probe.layout->elem_size != g_choice.layout.elem_size)
            {
                report.message = "second component class uses a different pose layout (" + probe.note + ")";
                return report;
            }
            std::string why;
            if (!patch_vtable(vtable, g_choice.slot, why))
            {
                report.message = why;
                return report;
            }
            report.outcome = Outcome::Installed;
            report.message = describe_locked();
            return report;
        }

        // Values pinned in the game profile: validate them and skip the search.
        if (g_override.slot >= 0)
        {
            FlipOffsets pinned{g_override.read - 4, g_override.read, true};
            pinned.bases[0] = g_override.buffers;
            pinned.base_count = 1;
            const LayoutProbe p = find_pose_layout(mem, comp, expected_bones, &pinned);
            uintptr_t fn = 0;
            if (!p.layout)
            {
                report.outcome = not_ready(p) ? Outcome::NotReadyYet : Outcome::Unsupported;
                report.message = not_ready(p) ? "waiting for the component (" + p.note + ")" : "profile Hook values do not fit this component: " + p.note;
                return report;
            }
            if (!mem.read(vt_addr + static_cast<uintptr_t>(g_override.slot) * 8, &fn, sizeof(fn)) || !mem.is_code(fn))
            {
                report.message = "profile Hook slot " + std::to_string(g_override.slot) + " is not a function in this vtable";
                return report;
            }
            std::string why;
            if (!patch_vtable(vtable, g_override.slot, why))
            {
                report.message = why;
                return report;
            }
            g_choice = {g_override.slot, *p.layout, "slot " + std::to_string(g_override.slot) + " (from the game profile), " + p.note};
            g_spares.clear();
            g_module_base = mem.code().base;
            g_installed_at = Clock::now();
            g_confirmed = false;
            g_warned_idle = false;
            g_calls.store(0);
            Registry::instance().set_layout(g_choice.layout);
            report.outcome = Outcome::Installed;
            report.message = describe_locked();
            return report;
        }

        const std::vector<FinalizeCandidate> candidates = candidates_for(mem, vt_addr);
        if (candidates.empty())
        {
            report.message = "FinalizeBoneTransform was not recognised in this game's code (no candidates)";
            return report;
        }

        std::vector<Choice> valid;
        bool waiting = false;
        std::string last_note;
        for (const FinalizeCandidate& c : candidates)
        {
            if (g_rejected.count(c.slot))
            {
                continue; // patched earlier and never called
            }
            const LayoutProbe probe = find_pose_layout(mem, comp, expected_bones, &c.offsets);
            if (probe.layout)
            {
                bool duplicate = false;
                for (const Choice& v : valid)
                {
                    duplicate = duplicate || v.slot == c.slot;
                }
                if (!duplicate)
                {
                    valid.push_back({c.slot, *probe.layout, "slot " + std::to_string(c.slot) + " (score " + std::to_string(c.score) + "), " + probe.note});
                }
            }
            else
            {
                // No pose yet, or the index members hold something else for the moment (a
                // character still being set up during a level load): retry quietly.
                waiting = waiting || (not_ready(probe) && c.offsets.certain);
                if (last_note.empty())
                {
                    last_note = probe.note;
                }
            }
        }

        if (valid.empty())
        {
            // Code gave nothing usable; see whether the memory alone shows the layout.
            const LayoutProbe blind = find_pose_layout(mem, comp, expected_bones, nullptr);
            if (blind.layout)
            {
                for (const FinalizeCandidate& c : candidates)
                {
                    if (!g_rejected.count(c.slot) && (c.offsets.read == blind.layout->read_index || c.offsets.editable == blind.layout->read_index))
                    {
                        valid.push_back({c.slot, *blind.layout, "slot " + std::to_string(c.slot) + ", " + blind.note});
                        break;
                    }
                }
                if (valid.empty())
                {
                    // The code patterns missed this build's flip (seen on UE 5.0): look for the vtable
                    // function that writes exactly the index members the memory search found.
                    const uint32_t b = blind.layout->buffers;
                    const std::vector<uint32_t> pairs = index_pair_candidates(mem, comp, b + 0x20, b + 0x100);
                    for (const FinalizeCandidate& c : find_finalize_by_members(mem, vt_addr, pairs))
                    {
                        if (g_rejected.count(c.slot))
                        {
                            continue;
                        }
                        const LayoutProbe p = find_pose_layout(mem, comp, expected_bones, &c.offsets);
                        if (p.layout && p.layout->buffers == b)
                        {
                            valid.push_back({c.slot, *p.layout,
                                             "slot " + std::to_string(c.slot) + " (found from the index members, score " + std::to_string(c.score) + "), " +
                                                 p.note});
                        }
                    }
                }
            }
            else if (blind.empty_pose)
            {
                waiting = true;
            }
        }

        if (valid.empty())
        {
            if (waiting)
            {
                report.outcome = Outcome::NotReadyYet;
                report.message = last_note.empty() ? "component has no pose yet" : "waiting for the component (" + last_note + ")";
                return report;
            }
            report.message = "could not confirm the pose buffers: " + (last_note.empty() ? std::string("no match") : last_note) + " (" +
                             std::to_string(candidates.size()) + " code candidates)";
            return report;
        }

        std::string why;
        if (!patch_vtable(vtable, valid.front().slot, why))
        {
            report.message = why;
            return report;
        }
        g_choice = valid.front();
        g_spares.assign(valid.begin() + 1, valid.end());
        g_module_base = mem.code().base;
        g_installed_at = Clock::now();
        g_confirmed = false;
        g_warned_idle = false;
        g_calls.store(0);
        Registry::instance().set_layout(g_choice.layout);

        report.outcome = Outcome::Installed;
        report.message = describe_locked();
        return report;
    }

    std::string watchdog()
    {
        std::lock_guard guard(g_lock);
        if (g_choice.slot < 0 || g_confirmed)
        {
            return {};
        }
        if (g_calls.load(std::memory_order_relaxed) > 0)
        {
            g_confirmed = true;
            return "pose hook confirmed: " + g_choice.how;
        }
        if (Clock::now() - g_installed_at < std::chrono::seconds(15))
        {
            return {};
        }
        if (g_spares.empty())
        {
            // The only function that passed every check: keep it (the game may simply be paused).
            if (!g_warned_idle)
            {
                g_warned_idle = true;
                return "pose hook has not been called yet (" + g_choice.how + "); keeping it - the game may be paused or loading";
            }
            return {};
        }
        // Another validated candidate exists and this one never ran: switch.
        std::vector<void**> vtables;
        const size_t count = g_patch_count.load(std::memory_order_acquire);
        for (size_t i = 0; i < count; ++i)
        {
            if (g_patches[i].entry)
            {
                vtables.push_back(static_cast<void**>(g_vtables[i].load()));
            }
        }
        unpatch_all();
        g_rejected.insert(g_choice.slot);
        const std::string old = g_choice.how;
        g_choice = g_spares.front();
        g_spares.erase(g_spares.begin());
        std::string why;
        for (void** vt : vtables)
        {
            patch_vtable(vt, g_choice.slot, why);
        }
        Registry::instance().set_layout(g_choice.layout);
        g_installed_at = Clock::now();
        g_calls.store(0);
        return old + " never ran, trying " + g_choice.how;
    }

    bool active()
    {
        std::lock_guard guard(g_lock);
        return active_locked();
    }

    std::string describe()
    {
        std::lock_guard guard(g_lock);
        return describe_locked();
    }

    void remove()
    {
        std::lock_guard guard(g_lock);
        unpatch_all();
    }

    std::string diagnose(void* component, int32_t expected_bones)
    {
        std::string out;
        const auto comp = reinterpret_cast<uintptr_t>(component);
        uintptr_t vt = 0;
        if (!comp || !safe_read(comp, &vt, sizeof(vt)))
        {
            return "component not readable";
        }
        LiveMemory mem(code_of_module_at(vt));
        wchar_t path[MAX_PATH] = {};
        if (mem.code().module)
        {
            GetModuleFileNameW(mem.code().module, path, MAX_PATH);
        }
        char mod_name[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, mod_name, sizeof(mod_name), nullptr, nullptr);
        out += "module: " + std::string(mod_name) + "\n";
        out += "component " + hex(comp) + ", vtable rva " + hex(vt - mem.code().base) + ", bones " + std::to_string(expected_bones) + "\n";
        const auto candidates = candidates_for(mem, vt);
        out += std::to_string(candidates.size()) + " code candidates:\n";
        for (size_t i = 0; i < candidates.size() && i < 12; ++i)
        {
            const auto& c = candidates[i];
            const LayoutProbe p = find_pose_layout(mem, comp, expected_bones, &c.offsets);
            out += "  slot " + std::to_string(c.slot) + " fn rva " + hex(c.function - mem.code().base) + " flip rva " + hex(c.flip - mem.code().base) +
                   " idx +" + hex(c.offsets.editable) + "/+" + hex(c.offsets.read) + (c.offsets.certain ? " certain" : "") + " score " +
                   std::to_string(c.score) + " depth " + std::to_string(c.depth) + " -> " + (p.layout ? "OK " : "no: ") + p.note + "\n";
            uint8_t bytes[32] = {};
            if (mem.read(c.function, bytes, sizeof(bytes)))
            {
                out += "    ";
                for (uint8_t b : bytes)
                {
                    char h[4];
                    std::snprintf(h, sizeof(h), "%02X ", b);
                    out += h;
                }
                out += "\n";
            }
        }
        const LayoutProbe blind = find_pose_layout(mem, comp, expected_bones, nullptr);
        out += "memory search: " + std::string(blind.layout ? "OK " : "no: ") + blind.note + "\n";
        if (blind.layout)
        {
            const uint32_t b = blind.layout->buffers;
            const auto pairs = index_pair_candidates(mem, comp, b + 0x20, b + 0x100);
            out += "index member pairs near the buffers:";
            for (uint32_t x : pairs)
            {
                out += " +" + hex(x);
            }
            out += "\n";
            const auto by_members = find_finalize_by_members(mem, vt, pairs);
            out += std::to_string(by_members.size()) + " functions writing those members:\n";
            for (size_t i = 0; i < by_members.size() && i < 8; ++i)
            {
                const auto& c = by_members[i];
                const LayoutProbe p = find_pose_layout(mem, comp, expected_bones, &c.offsets);
                out += "  slot " + std::to_string(c.slot) + " fn rva " + hex(c.function - mem.code().base) + " idx +" + hex(c.offsets.editable) + "/+" +
                       hex(c.offsets.read) + " score " + std::to_string(c.score) + " depth " + std::to_string(c.depth) + " -> " +
                       (p.layout ? "OK " : "no: ") + p.note + "\n";
            }
        }
        out += "current: " + describe() + ", calls " + std::to_string(g_calls.load()) + "\n";
        return out;
    }
    void set_override(int slot, uint32_t buffers, uint32_t read_index)
    {
        std::lock_guard guard(g_lock);
        g_override = {slot, buffers, read_index};
    }

    std::string profile_snippet()
    {
        std::lock_guard guard(g_lock);
        if (g_choice.slot < 0 || !active_locked())
        {
            return {};
        }
        return "\"Hook\": { \"Slot\": " + std::to_string(g_choice.slot) + ", \"Buffers\": \"" + hex(g_choice.layout.buffers) + "\", \"ReadIndex\": \"" +
               hex(g_choice.layout.read_index) + "\" }";
    }
} // namespace uuepbs::hook
