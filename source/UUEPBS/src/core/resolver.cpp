#include "resolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace uuepbs
{
    namespace
    {
        constexpr uint32_t kMinMember = 0x100;
        constexpr uint32_t kMaxMember = 0x4000;
        constexpr size_t kFlipScan = 0x1A0;
        constexpr size_t kCallScan = 0x60;

        bool member_like(uint32_t d)
        {
            return d >= kMinMember && d < kMaxMember && (d & 3) == 0;
        }

        // One decoded memory access [base + disp32].
        struct Access
        {
            enum Kind : uint8_t
            {
                Load,
                Store,
                ZeroQword,
            } kind;
            size_t pos;
            int reg;
            int base;
            uint32_t disp;
        };

        // Decodes ModRM with mod == 10 (disp32). Returns bytes consumed after the opcode, 0 if not that form.
        size_t modrm_disp32(const uint8_t* p, size_t avail, uint8_t rex, int& reg, int& base, uint32_t& disp)
        {
            if (avail < 5)
            {
                return 0;
            }
            const uint8_t modrm = p[0];
            if ((modrm >> 6) != 2)
            {
                return 0;
            }
            reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
            int rm = modrm & 7;
            size_t used = 1;
            if (rm == 4)
            {
                if (avail < 6)
                {
                    return 0;
                }
                const uint8_t sib = p[1];
                if (((sib >> 3) & 7) != 4)
                {
                    return 0; // has an index register: not a plain member access
                }
                rm = sib & 7;
                used = 2;
            }
            base = rm | ((rex & 1) ? 8 : 0);
            std::memcpy(&disp, p + used, 4);
            return used + 4;
        }

        std::vector<Access> scan_accesses(const uint8_t* code, size_t size)
        {
            std::vector<Access> out;
            for (size_t k = 0; k + 6 < size; ++k)
            {
                size_t at = k;
                uint8_t rex = 0;
                if ((code[at] & 0xF0) == 0x40)
                {
                    rex = code[at];
                    ++at;
                }
                const uint8_t op = code[at];
                int reg = 0, base = 0;
                uint32_t disp = 0;
                if (op == 0x63 && (rex & 8)) // movsxd r64, dword [base+disp32]
                {
                    if (modrm_disp32(code + at + 1, size - at - 1, rex, reg, base, disp) && member_like(disp))
                    {
                        out.push_back({Access::Load, k, reg, base, disp});
                    }
                }
                else if (op == 0x8B && !(rex & 8)) // mov r32, dword [base+disp32]
                {
                    if (modrm_disp32(code + at + 1, size - at - 1, rex, reg, base, disp) && member_like(disp))
                    {
                        out.push_back({Access::Load, k, reg, base, disp});
                    }
                }
                else if (op == 0x89 && !(rex & 8)) // mov dword [base+disp32], r32
                {
                    if (modrm_disp32(code + at + 1, size - at - 1, rex, reg, base, disp) && member_like(disp))
                    {
                        out.push_back({Access::Store, k, reg, base, disp});
                    }
                }
                else if (op == 0xC7 && (rex & 8)) // mov qword [base+disp32], imm32
                {
                    const size_t used = modrm_disp32(code + at + 1, size - at - 1, rex, reg, base, disp);
                    if (used && (reg & 7) == 0 && at + 1 + used + 4 <= size && member_like(disp))
                    {
                        uint32_t imm = 0;
                        std::memcpy(&imm, code + at + 1 + used, 4);
                        if (imm == 0)
                        {
                            out.push_back({Access::ZeroQword, k, 0, base, disp});
                        }
                    }
                }
            }
            return out;
        }

        // A flag byte set right after a call: or byte [r+d],imm8 / mov byte [r+d],1
        bool sets_flag_at(const uint8_t* p, size_t avail)
        {
            size_t at = 0;
            if (avail > 0 && (p[0] & 0xF0) == 0x40)
            {
                ++at;
            }
            if (avail < at + 3)
            {
                return false;
            }
            const uint8_t op = p[at];
            const uint8_t modrm = p[at + 1];
            const int mod = modrm >> 6;
            const int sub = (modrm >> 3) & 7;
            if ((modrm & 7) == 4)
            {
                return false;
            }
            if (op == 0x80 && sub == 1 && (mod == 1 || mod == 2))
            {
                return true;
            }
            if (op == 0xC6 && sub == 0 && (mod == 1 || mod == 2))
            {
                const size_t imm_at = at + 2 + (mod == 2 ? 4 : 1);
                return imm_at < avail && p[imm_at] == 1;
            }
            return false;
        }

        struct Call
        {
            uintptr_t target;
            size_t end; // offset just after the call instruction
            bool tail;  // jmp
        };

        std::vector<Call> first_calls(const MemoryView& mem, uintptr_t fn, size_t max_calls)
        {
            std::vector<Call> out;
            uint8_t code[kCallScan];
            if (!mem.read(fn, code, sizeof(code)))
            {
                return out;
            }
            for (size_t k = 0; k + 5 <= sizeof(code) && out.size() < max_calls; ++k)
            {
                if (code[k] != 0xE8 && code[k] != 0xE9)
                {
                    continue;
                }
                int32_t rel = 0;
                std::memcpy(&rel, code + k + 1, 4);
                const uintptr_t target = fn + k + 5 + static_cast<intptr_t>(rel);
                if (target == fn || !mem.is_code(target))
                {
                    continue;
                }
                out.push_back({target, k + 5, code[k] == 0xE9});
                if (code[k] == 0xE9)
                {
                    break; // tail jump ends the function
                }
                k += 4;
            }
            return out;
        }

        bool flag_after(const MemoryView& mem, uintptr_t fn, size_t call_end)
        {
            uint8_t p[16];
            return mem.read(fn + call_end, p, sizeof(p)) && sets_flag_at(p, sizeof(p));
        }

        // "add r64, imm ; ... ; shl r64, 4" (MSVC's (index + base/16) * 16) and
        // "shl r64, 4 ; ... ; lea r, [r64 + base_reg + disp32]" / "add r64, imm32".
        void collect_bases(const uint8_t* code, size_t size, FlipOffsets& out)
        {
            auto add_base = [&out](uint32_t b) {
                if (b < 0x100 || b >= 0x4000 || (b & 7) != 0 || out.base_count >= 4) // TArrays are 8-byte aligned (UE 4.26: +0x5A8)
                {
                    return;
                }
                for (int i = 0; i < out.base_count; ++i)
                {
                    if (out.bases[i] == b)
                    {
                        return;
                    }
                }
                out.bases[out.base_count++] = b;
            };
            for (size_t k = 0; k + 8 < size; ++k)
            {
                const uint8_t rex = code[k];
                if ((rex & 0xF8) != 0x48 || k + 3 >= size)
                {
                    continue;
                }
                // shl r64, 4 : REX.W C1 /4 ib   (modrm 11 100 reg)
                const bool is_shl = code[k + 1] == 0xC1 && (code[k + 2] & 0xF8) == 0xE0 && code[k + 3] == 0x04;
                if (!is_shl)
                {
                    continue;
                }
                const int reg = (code[k + 2] & 7) | ((rex & 1) ? 8 : 0);
                // look back for add reg, imm
                for (size_t back = 4; back <= 20 && back <= k; ++back)
                {
                    const uint8_t* p = code + k - back;
                    if ((p[0] & 0xF8) == 0x48 && (p[1] == 0x83 || p[1] == 0x81) && (p[2] & 0xF8) == 0xC0 &&
                        ((p[2] & 7) | ((p[0] & 1) ? 8 : 0)) == reg)
                    {
                        uint32_t imm = p[3];
                        if (p[1] == 0x81)
                        {
                            std::memcpy(&imm, p + 3, 4);
                        }
                        add_base(imm << 4);
                        break;
                    }
                }
                // look ahead for lea r, [reg + base + disp32] or add reg, imm32
                for (size_t fwd = 4; fwd <= 16 && k + fwd + 8 < size; ++fwd)
                {
                    const uint8_t* p = code + k + fwd;
                    if ((p[0] & 0xF8) == 0x48 && p[1] == 0x8D && (p[2] >> 6) == 2 && (p[2] & 7) == 4)
                    {
                        const uint8_t sib = p[3];
                        const int idx = ((sib >> 3) & 7) | ((p[0] & 2) ? 8 : 0);
                        const int base = (sib & 7) | ((p[0] & 1) ? 8 : 0);
                        if ((sib >> 6) == 0 && (idx == reg || base == reg))
                        {
                            uint32_t disp;
                            std::memcpy(&disp, p + 4, 4);
                            add_base(disp);
                            break;
                        }
                    }
                    if ((p[0] & 0xF8) == 0x48 && p[1] == 0x81 && (p[2] & 0xF8) == 0xC0 && ((p[2] & 7) | ((p[0] & 1) ? 8 : 0)) == reg)
                    {
                        uint32_t imm;
                        std::memcpy(&imm, p + 3, 4);
                        add_base(imm);
                        break;
                    }
                }
            }
        }
    } // namespace

    std::optional<FlipOffsets> parse_flip(const MemoryView& mem, uintptr_t function)
    {
        uint8_t code[kFlipScan];
        if (!mem.read(function, code, sizeof(code)))
        {
            // Near the end of a section: try a shorter window.
            std::memset(code, 0xCC, sizeof(code));
            if (!mem.read(function, code, 0x80))
            {
                return std::nullopt;
            }
        }
        const std::vector<Access> acc = scan_accesses(code, sizeof(code));

        // Form A (UE 5.x): Read = Editable; Editable = 1 - Editable
        //   movsxd rdx, [rbx+E] ... mov [rbx+R], edx     (R = E +- 4)
        for (size_t i = 0; i < acc.size(); ++i)
        {
            const Access& load = acc[i];
            if (load.kind != Access::Load)
            {
                continue;
            }
            for (size_t j = i + 1; j < acc.size() && acc[j].pos <= load.pos + 40; ++j)
            {
                const Access& store = acc[j];
                if (store.kind == Access::Store && store.reg == load.reg && store.base == load.base &&
                    (store.disp == load.disp + 4 || load.disp == store.disp + 4))
                {
                    // Editable must also be written (1 - value) somewhere close by.
                    for (const Access& w : acc)
                    {
                        if (w.kind == Access::Store && w.disp == load.disp && w.base == load.base && w.pos > load.pos && w.pos <= load.pos + 48)
                        {
                            FlipOffsets f{load.disp, store.disp, true};
                            collect_bases(code, sizeof(code), f);
                            return f;
                        }
                    }
                }
            }
        }

        // Form B (UE 4.x): both ints get 1 - x; or the non-double-buffered branch zeroes both at once.
        for (const Access& z : acc)
        {
            if (z.kind == Access::ZeroQword)
            {
                bool touched = false;
                for (const Access& a : acc)
                {
                    touched = touched || ((a.disp == z.disp || a.disp == z.disp + 4) && a.base == z.base && a.kind != Access::ZeroQword);
                }
                if (touched)
                {
                    FlipOffsets f{z.disp, z.disp + 4, false}; // declaration order: Editable, then Read
                    collect_bases(code, sizeof(code), f);
                    return f;
                }
            }
        }
        for (const Access& a : acc)
        {
            if (a.kind != Access::Load)
            {
                continue;
            }
            const uint32_t lo = a.disp;
            bool lo_store = false, hi_load = false, hi_store = false;
            for (const Access& b : acc)
            {
                if (b.base != a.base)
                {
                    continue;
                }
                lo_store = lo_store || (b.kind == Access::Store && b.disp == lo && b.pos > a.pos);
                hi_load = hi_load || (b.kind == Access::Load && b.disp == lo + 4);
                hi_store = hi_store || (b.kind == Access::Store && b.disp == lo + 4);
            }
            if (lo_store && hi_load && hi_store)
            {
                FlipOffsets f{lo, lo + 4, false};
                collect_bases(code, sizeof(code), f);
                return f;
            }
        }
        return std::nullopt;
    }

    namespace
    {
        using FlipDetector = std::function<std::optional<FlipOffsets>(uintptr_t)>;
        std::vector<FinalizeCandidate> search_vtable(const MemoryView& mem, uintptr_t vtable, int first, int last, const FlipDetector& detect);
    } // namespace

    std::vector<FinalizeCandidate> find_finalize_candidates(const MemoryView& mem, uintptr_t vtable, int first, int last)
    {
        return search_vtable(mem, vtable, first, last, [&](uintptr_t fn) {
            return parse_flip(mem, fn);
        });
    }

    std::optional<FlipOffsets> parse_flip_at(const MemoryView& mem, uintptr_t function, const std::vector<uint32_t>& pair_starts)
    {
        // Larger window: in some builds (UE 5.0) the flip is inlined deep inside FinalizeBoneTransform.
        size_t size = 0x700;
        std::vector<uint8_t> code(size);
        while (size >= 0x80 && !mem.read(function, code.data(), size))
        {
            size /= 2;
        }
        if (size < 0x80)
        {
            return std::nullopt;
        }
        const std::vector<Access> acc = scan_accesses(code.data(), size);
        for (const uint32_t x : pair_starts)
        {
            bool store_lo = false, store_hi = false;
            std::optional<FlipOffsets> certain;
            for (size_t i = 0; i < acc.size(); ++i)
            {
                const Access& a = acc[i];
                if (a.kind == Access::ZeroQword && a.disp == x)
                {
                    store_lo = store_hi = true;
                }
                if (a.kind == Access::Store && a.disp == x)
                {
                    store_lo = true;
                }
                if (a.kind == Access::Store && a.disp == x + 4)
                {
                    store_hi = true;
                }
                // Read = Editable ("load A, store the same register to B"): B is the read index.
                if (a.kind == Access::Load && (a.disp == x || a.disp == x + 4) && !certain)
                {
                    for (size_t j = i + 1; j < acc.size() && acc[j].pos <= a.pos + 40; ++j)
                    {
                        const Access& s = acc[j];
                        if (s.kind == Access::Store && s.reg == a.reg && s.base == a.base && s.disp != a.disp && (s.disp == x || s.disp == x + 4))
                        {
                            certain = FlipOffsets{a.disp, s.disp, true};
                            break;
                        }
                    }
                }
            }
            if (store_lo && store_hi)
            {
                FlipOffsets f = certain ? *certain : FlipOffsets{x, x + 4, false};
                collect_bases(code.data(), size, f);
                return f;
            }
        }
        return std::nullopt;
    }

    std::vector<FinalizeCandidate> find_finalize_by_members(const MemoryView& mem, uintptr_t vtable, const std::vector<uint32_t>& pair_starts, int first,
                                                            int last)
    {
        return search_vtable(mem, vtable, first, last, [&](uintptr_t fn) {
            return parse_flip_at(mem, fn, pair_starts);
        });
    }

    namespace
    {
    std::vector<FinalizeCandidate> search_vtable(const MemoryView& mem, uintptr_t vtable, int first, int last, const FlipDetector& detect)
    {
        std::vector<FinalizeCandidate> out;
        std::map<uintptr_t, std::optional<FlipOffsets>> cache;
        auto flip_of = [&](uintptr_t fn) -> const std::optional<FlipOffsets>& {
            auto it = cache.find(fn);
            if (it == cache.end())
            {
                it = cache.emplace(fn, detect(fn)).first;
            }
            return it->second;
        };

        std::map<uintptr_t, bool> seen_fn;
        for (int slot = 0; slot <= last; ++slot)
        {
            uintptr_t fn = 0;
            if (!mem.read(vtable + static_cast<uintptr_t>(slot) * sizeof(uintptr_t), &fn, sizeof(fn)) || !mem.is_code(fn))
            {
                if (slot > 8)
                {
                    break; // end of the vtable
                }
                continue;
            }
            if (slot < first || seen_fn[fn])
            {
                continue;
            }
            seen_fn[fn] = true;

            FinalizeCandidate best;
            const std::vector<Call> calls = first_calls(mem, fn, 3);
            for (size_t c = 0; c < calls.size(); ++c)
            {
                const auto& flip = flip_of(calls[c].target);
                if (flip)
                {
                    int score = 6 - static_cast<int>(c) + (flag_after(mem, fn, calls[c].end) ? 4 : 0) + (flip->certain ? 1 : 0);
                    if (score > best.score)
                    {
                        best = {slot, fn, calls[c].target, *flip, score, 1};
                    }
                }
                if (c < 2)
                {
                    // Game subclass calling Super, which calls Flip.
                    const std::vector<Call> inner = first_calls(mem, calls[c].target, 2);
                    for (size_t d = 0; d < inner.size(); ++d)
                    {
                        const auto& deep = flip_of(inner[d].target);
                        if (deep)
                        {
                            int score = 3 - static_cast<int>(c) - static_cast<int>(d) + (flag_after(mem, calls[c].target, inner[d].end) ? 3 : 0);
                            if (score > best.score)
                            {
                                best = {slot, fn, inner[d].target, *deep, score, 2};
                            }
                        }
                    }
                }
            }
            if (best.slot < 0)
            {
                const auto& inl = flip_of(fn); // Flip inlined into the slot function
                if (inl)
                {
                    best = {slot, fn, fn, *inl, 1, 0};
                }
            }
            if (best.slot >= 0)
            {
                out.push_back(best);
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const FinalizeCandidate& a, const FinalizeCandidate& b) {
            return a.score > b.score;
        });
        return out;
    }
    } // namespace

    bool plausible_float_transform(const XformF& x)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (!std::isfinite(x.rot[i]) || !std::isfinite(x.pos[i]) || !std::isfinite(x.scl[i]))
            {
                return false;
            }
        }
        const double qlen = double(x.rot[0]) * x.rot[0] + double(x.rot[1]) * x.rot[1] + double(x.rot[2]) * x.rot[2] + double(x.rot[3]) * x.rot[3];
        if (std::fabs(qlen - 1.0) > 0.05)
        {
            return false;
        }
        for (int i = 0; i < 3; ++i)
        {
            const double s = std::fabs(x.scl[i]);
            if (s < 1.0e-4 || s > 1.0e4 || std::fabs(x.pos[i]) > 1.0e7)
            {
                return false;
            }
        }
        return true;
    }

    namespace
    {
        // 96 (double), 48 (float) or 0 when the data is not transforms.
        uint32_t transform_size(const MemoryView& mem, uintptr_t data, int32_t bones)
        {
            const int32_t probe = std::min<int32_t>(bones, 6);
            bool dbl = true;
            for (int32_t i = 0; i < probe && dbl; ++i)
            {
                Xform x;
                dbl = mem.read(data + static_cast<uintptr_t>(i) * sizeof(Xform), &x, sizeof(x)) && xf::plausible(x);
            }
            if (dbl)
            {
                uint8_t last[sizeof(Xform)];
                if (mem.read(data + static_cast<uintptr_t>(bones - 1) * sizeof(Xform), last, sizeof(last)))
                {
                    return 96;
                }
            }
            bool flt = true;
            for (int32_t i = 0; i < probe && flt; ++i)
            {
                XformF x;
                flt = mem.read(data + static_cast<uintptr_t>(i) * sizeof(XformF), &x, sizeof(x)) && plausible_float_transform(x);
            }
            if (flt)
            {
                uint8_t last[sizeof(XformF)];
                if (mem.read(data + static_cast<uintptr_t>(bones - 1) * sizeof(XformF), last, sizeof(last)))
                {
                    return 48;
                }
            }
            return 0;
        }

        bool read_array(const MemoryView& mem, uintptr_t at, RawArray& a)
        {
            return mem.read(at, &a, sizeof(a));
        }

        bool read_index(const MemoryView& mem, uintptr_t at, int32_t& v)
        {
            return mem.read(at, &v, sizeof(v)) && (v == 0 || v == 1);
        }

        std::string hex(uint32_t v)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%X", v);
            return buf;
        }
    } // namespace

    LayoutProbe find_pose_layout(const MemoryView& mem, uintptr_t component, int32_t bones, const FlipOffsets* hint)
    {
        LayoutProbe probe;
        if (bones <= 0)
        {
            probe.note = "no bone count";
            return probe;
        }

        auto finish = [&](uint32_t buffers, uint32_t read_off) -> bool {
            int32_t read = 0;
            RawArray arr{};
            if (!read_index(mem, component + read_off, read) || !read_array(mem, component + buffers + static_cast<uint32_t>(read) * 16, arr))
            {
                return false;
            }
            if (arr.num == 0)
            {
                probe.empty_pose = true;
                return false;
            }
            if (arr.num != bones || arr.max < arr.num || !arr.data)
            {
                return false;
            }
            const uint32_t size = transform_size(mem, reinterpret_cast<uintptr_t>(arr.data), bones);
            if (!size)
            {
                probe.note = "arrays at +" + hex(buffers) + " do not hold transforms";
                return false;
            }
            PoseLayout layout;
            layout.buffers = buffers;
            layout.read_index = read_off;
            layout.array_stride = 16;
            layout.elem_size = size;
            probe.layout = layout;
            probe.empty_pose = false;
            probe.note = "buffers +" + hex(buffers) + ", read index +" + hex(read_off) + (size == 96 ? ", double transforms" : ", float transforms");
            return true;
        };

        // Arrays at off and off+16 both hold <bones> transforms. Unreal keeps other bone-sized
        // arrays next to the pose buffers (previous-frame pose, bone visibility bytes), so a
        // pair only counts when both halves really are transform arrays; among several
        // matching pairs the lowest one is the double buffer (declared first in the class).
        auto transform_array = [&](uint32_t off) -> uint32_t {
            RawArray a{};
            if (!read_array(mem, component + off, a) || a.num != bones || a.max < a.num || !a.data)
            {
                return 0;
            }
            return transform_size(mem, reinterpret_cast<uintptr_t>(a.data), bones);
        };
        auto empty_array = [&](uint32_t off) {
            RawArray a{};
            return read_array(mem, component + off, a) && a.num == 0 && a.max >= 0;
        };
        // 2 = both halves are transforms (double buffered), 1 = first half only and the second
        // is empty (component not double buffered), 0 = no.
        auto pose_pair_at = [&](uint32_t off) {
            const uint32_t first = transform_array(off);
            if (!first)
            {
                return 0;
            }
            const uint32_t second = transform_array(off + 16);
            if (second == first)
            {
                RawArray a{}, b{};
                read_array(mem, component + off, a);
                read_array(mem, component + off + 16, b);
                return a.data != b.data ? 2 : 0;
            }
            return (!second && empty_array(off + 16)) ? 1 : 0;
        };
        auto array_pair_at = [&](uint32_t off, bool) { return pose_pair_at(off) == 2; };

        if (hint)
        {
            int32_t e = 0, r = 0;
            if (!read_index(mem, component + hint->editable, e) || !read_index(mem, component + hint->read, r))
            {
                probe.note = "buffer index members (+" + hex(hint->editable) + ", +" + hex(hint->read) + ") do not hold 0/1";
                probe.unsettled = true;
                return probe;
            }
            // 1) Arrays the flip code itself indexes with these ints.
            bool any_empty = false;
            for (int i = 0; i < hint->base_count; ++i)
            {
                const uint32_t base = hint->bases[i];
                if (pose_pair_at(base) > 0 && finish(base, hint->read))
                {
                    probe.note += " (from code)";
                    return probe;
                }
                any_empty = any_empty || (empty_array(base) && empty_array(base + 16));
            }
            // 2) Otherwise the lowest pair of transform arrays just before the ints.
            const uint32_t low = std::min(hint->editable, hint->read) & ~7u; // TArrays are 8-byte aligned
            // Usually within 0x90 bytes; some builds keep more members in between (UE 4.26 in
            // Stellar Blade: buffers +0x5A8, ints +0x670), so widen once if nothing turned up.
            uint32_t best = 0;
            int best_kind = 0;
            for (const uint32_t reach : {0x90u, 0x100u})
            {
                for (uint32_t back = 0x10; back <= reach && back < low; back += 8)
                {
                    const uint32_t off = low - back;
                    const int kind = pose_pair_at(off);
                    if (kind == 2 || (kind == 1 && best_kind < 2))
                    {
                        best = off; // keeps moving down: the lowest match wins
                        best_kind = kind == 2 ? 2 : std::max(best_kind, 1);
                    }
                    if (reach == 0x90)
                    {
                        any_empty = any_empty || (empty_array(off) && empty_array(off + 16));
                    }
                }
                if (best)
                {
                    break;
                }
            }
            if (best && finish(best, hint->read))
            {
                return probe;
            }
            probe.empty_pose = probe.empty_pose || (any_empty && !best);
            if (probe.note.empty())
            {
                probe.note = probe.empty_pose ? "component has no pose yet"
                                              : "no pose arrays with " + std::to_string(bones) + " entries near the index members";
            }
            return probe;
        }

        // No code hint: look for the two arrays followed by a 0/1 index pair.
        for (uint32_t off = 0x100; off < 0x2000; off += 8)
        {
            if (!array_pair_at(off, false))
            {
                continue;
            }
            for (uint32_t idx = off + 0x20; idx + 8 <= off + 0x100; idx += 4)
            {
                int32_t e = -1, r = -1;
                if (read_index(mem, component + idx, e) && read_index(mem, component + idx + 4, r) && e != r && finish(off, idx + 4))
                {
                    probe.note += " (found by memory search)";
                    return probe;
                }
            }
        }
        probe.note = "no double-buffered pose arrays with " + std::to_string(bones) + " entries found in the component";
        return probe;
    }
    std::vector<uint32_t> index_pair_candidates(const MemoryView& mem, uintptr_t component, uint32_t from, uint32_t to)
    {
        std::vector<uint32_t> out;
        for (uint32_t x = from & ~3u; x + 8 <= to; x += 4)
        {
            int32_t a = -1, b = -1;
            if (read_index(mem, component + x, a) && read_index(mem, component + x + 4, b))
            {
                out.push_back(x);
            }
        }
        return out;
    }
} // namespace uuepbs
