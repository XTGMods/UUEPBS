#include "sculpt.hpp"

#include <algorithm>
#include <cctype>

namespace uuepbs
{
    namespace
    {
        constexpr double kMinScale = 0.02;
        constexpr double kMaxScale = 20.0;
        constexpr double kNeutralEps = 1.0e-6;
    } // namespace

    const char* spread_label(Spread s)
    {
        switch (s)
        {
        case Spread::Chain:
            return "Scale children too";
        case Spread::Keep:
            return "Keep children size";
        case Spread::Solo:
            return "This bone only";
        }
        return "?";
    }

    const char* spread_token(Spread s)
    {
        switch (s)
        {
        case Spread::Chain:
            return "chain";
        case Spread::Keep:
            return "keep";
        case Spread::Solo:
            return "solo";
        }
        return "chain";
    }

    bool spread_from_token(std::string_view token, Spread& out)
    {
        const std::string t = fold_case(token);
        if (t == "chain" || t == "full" || t == "hierarchy")
        {
            out = Spread::Chain;
            return true;
        }
        if (t == "keep" || t == "preserve")
        {
            out = Spread::Keep;
            return true;
        }
        if (t == "solo" || t == "isolated" || t == "bone")
        {
            out = Spread::Solo;
            return true;
        }
        return false;
    }

    bool BoneEdit::has_scale() const
    {
        return std::fabs(axis[0] - 1.0) >= kNeutralEps || std::fabs(axis[1] - 1.0) >= kNeutralEps || std::fabs(axis[2] - 1.0) >= kNeutralEps;
    }

    bool BoneEdit::has_turn() const
    {
        return std::fabs(turn[0]) >= kNeutralEps || std::fabs(turn[1]) >= kNeutralEps || std::fabs(turn[2]) >= kNeutralEps;
    }

    bool BoneEdit::has_shift() const
    {
        return std::fabs(shift[0]) >= kNeutralEps || std::fabs(shift[1]) >= kNeutralEps || std::fabs(shift[2]) >= kNeutralEps;
    }

    void BoneEdit::clamp()
    {
        for (int a = 0; a < 3; ++a)
        {
            if (!std::isfinite(axis[a]))
            {
                axis[a] = 1.0;
            }
            axis[a] = std::min(kMaxScale, std::max(kMinScale, axis[a]));
            if (!std::isfinite(turn[a]))
            {
                turn[a] = 0.0;
            }
            turn[a] = std::min(kMaxTurn, std::max(-kMaxTurn, turn[a]));
            if (!std::isfinite(shift[a]))
            {
                shift[a] = 0.0;
            }
            shift[a] = std::min(kMaxShift, std::max(-kMaxShift, shift[a]));
        }
        if (static_cast<uint8_t>(spread) > static_cast<uint8_t>(Spread::Solo))
        {
            spread = Spread::Chain;
        }
    }

    void turn_quaternion(const BoneEdit& edit, double out[4])
    {
        constexpr double kHalfRad = 3.14159265358979323846 / 360.0;
        const double hx = edit.turn[0] * kHalfRad;
        const double hy = edit.turn[1] * kHalfRad;
        const double hz = edit.turn[2] * kHalfRad;
        const double qx[4] = {std::sin(hx), 0.0, 0.0, std::cos(hx)};
        const double qy[4] = {0.0, std::sin(hy), 0.0, std::cos(hy)};
        const double qz[4] = {0.0, 0.0, std::sin(hz), std::cos(hz)};
        double yx[4];
        xf::qmul(qy, qx, yx);  // X first, then Y
        xf::qmul(qz, yx, out); // then Z
    }

    std::string fold_case(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    bool RigSculptor::set_skeleton(std::vector<std::string> names, std::vector<int32_t> parents, std::string* why)
    {
        if (names.empty() || names.size() != parents.size())
        {
            if (why)
            {
                *why = "bone name and parent lists do not line up";
            }
            return false;
        }
        for (size_t i = 0; i < parents.size(); ++i)
        {
            // The engine keeps bones sorted so that a parent always precedes its children.
            if (parents[i] >= static_cast<int32_t>(i) || parents[i] < -1)
            {
                if (why)
                {
                    *why = "bone '" + names[i] + "' has an out-of-order parent index";
                }
                return false;
            }
        }

        m_names = std::move(names);
        m_parents = std::move(parents);
        m_folded.clear();
        m_lookup.clear();
        m_folded.reserve(m_names.size());
        for (size_t i = 0; i < m_names.size(); ++i)
        {
            m_folded.push_back(fold_case(m_names[i]));
            m_lookup.emplace(m_folded.back(), static_cast<int32_t>(i));
        }

        const size_t n = m_names.size();
        m_edit_slot.assign(n, -1);
        m_in_plan.assign(n, 0);
        m_source.assign(n, xf::identity());
        m_for_children.assign(n, xf::identity());
        m_fresh.assign(n, xf::identity());
        m_plan.clear();
        m_inputs.clear();
        m_touched.clear();
        m_scales.clear();
        m_revision = ~0ull;
        forget_buffers();
        return true;
    }

    int32_t RigSculptor::find(std::string_view bone) const
    {
        const auto it = m_lookup.find(fold_case(bone));
        return it == m_lookup.end() ? -1 : it->second;
    }

    void RigSculptor::bind(const EditBook& book, uint64_t revision)
    {
        const size_t n = m_names.size();
        m_revision = revision;
        m_scales.clear();
        m_turns.clear();
        m_plan.clear();
        m_inputs.clear();
        std::fill(m_edit_slot.begin(), m_edit_slot.end(), -1);
        std::fill(m_in_plan.begin(), m_in_plan.end(), 0);
        if (n == 0)
        {
            return;
        }

        for (const auto& [key, entry] : book)
        {
            if (entry.edit.is_neutral())
            {
                continue;
            }
            const auto it = m_lookup.find(key);
            if (it == m_lookup.end())
            {
                continue;
            }
            BoneEdit s = entry.edit;
            s.clamp();
            m_edit_slot[it->second] = static_cast<int32_t>(m_scales.size());
            m_scales.push_back(s);
            std::array<double, 4> q{};
            turn_quaternion(s, q.data());
            m_turns.push_back(q);
        }
        if (m_scales.empty())
        {
            return;
        }

        // A bone must be rebuilt when it is edited or when something above it moved it.
        std::vector<uint8_t> pushes_children(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            const int32_t p = m_parents[i];
            const bool moved_by_ancestor = p >= 0 && pushes_children[p];
            const int32_t slot = m_edit_slot[i];
            const bool edited = slot >= 0;
            if (!edited && !moved_by_ancestor)
            {
                continue;
            }
            m_in_plan[i] = 1;
            m_plan.push_back(static_cast<int32_t>(i));
            pushes_children[i] = moved_by_ancestor || (edited && m_scales[slot].spread != Spread::Solo);
        }

        std::vector<uint8_t> needed(n, 0);
        for (const int32_t i : m_plan)
        {
            needed[i] = 1;
            if (m_parents[i] >= 0)
            {
                needed[m_parents[i]] = 1;
            }
        }
        for (size_t i = 0; i < n; ++i)
        {
            if (needed[i])
            {
                m_inputs.push_back(static_cast<int32_t>(i));
            }
        }
    }

    bool RigSculptor::has_work(const void* key) const
    {
        if (!m_plan.empty())
        {
            return true;
        }
        for (const Memo& m : m_memos)
        {
            if (m.buffer == key && !m.marked_list.empty())
            {
                return true;
            }
        }
        return false;
    }

    void RigSculptor::gather_inputs(const void* key, std::vector<int32_t>& out) const
    {
        out = m_inputs;
        for (const Memo& m : m_memos)
        {
            if (m.buffer == key && m.marked.size() == m_names.size())
            {
                out.insert(out.end(), m.marked_list.begin(), m.marked_list.end());
            }
        }
    }

    void RigSculptor::forget_buffers()
    {
        for (Memo& m : m_memos)
        {
            m.buffer = nullptr;
            m.last_use = 0;
            m.source.clear();
            m.result.clear();
            m.marked.clear();
            m.marked_list.clear();
        }
    }

    RigSculptor::Memo& RigSculptor::memo_for(const void* buffer)
    {
        const size_t n = m_names.size();
        ++m_clock;
        Memo* oldest = &m_memos[0];
        for (Memo& m : m_memos)
        {
            if (m.buffer == buffer && m.marked.size() == n)
            {
                m.last_use = m_clock;
                return m;
            }
            if (m.last_use < oldest->last_use)
            {
                oldest = &m;
            }
        }
        Memo& m = *oldest;
        m.buffer = buffer;
        m.last_use = m_clock;
        m.source.assign(n, xf::identity());
        m.result.assign(n, xf::identity());
        m.marked.assign(n, 0);
        m.marked_list.clear();
        return m;
    }

    int32_t RigSculptor::apply(Xform* pose, int32_t count, const void* key)
    {
        const int32_t n = bone_count();
        if (!pose || count != n || n == 0)
        {
            return 0;
        }
        if (!key)
        {
            key = pose;
        }
        m_touched.clear();
        if (!has_work(key))
        {
            return 0; // nothing planned and nothing of ours left in this buffer
        }

        Memo& memo = memo_for(key);
        int32_t written = 0;

        // 1) Put back bones we edited earlier but no longer plan to touch, as long as
        //    the engine has not refreshed them since (i.e. they still hold our output).
        for (const int32_t i : memo.marked_list)
        {
            if (!m_in_plan[i])
            {
                if (xf::same_bits(pose[i], memo.result[i]))
                {
                    pose[i] = memo.source[i];
                    m_touched.push_back(i);
                    ++written;
                }
                memo.marked[i] = 0;
            }
        }

        // 2) Recover the untouched pose for everything we are about to rebuild.
        for (const int32_t i : m_plan)
        {
            const bool ours = memo.marked[i] && xf::same_bits(pose[i], memo.result[i]);
            m_source[i] = ours ? memo.source[i] : pose[i];
        }

        // 3) Rebuild top-down (parents always precede children).
        const Xform unit = xf::identity();
        for (const int32_t i : m_plan)
        {
            const int32_t p = m_parents[i];
            const Xform& parent_before = p < 0 ? unit : (m_in_plan[p] ? m_source[p] : pose[p]);
            const Xform& parent_after = p < 0 ? unit : (m_in_plan[p] ? m_for_children[p] : pose[p]);

            Xform local = p < 0 ? m_source[i] : xf::relative(m_source[i], parent_before);

            if (p >= 0 && m_edit_slot[p] >= 0)
            {
                const BoneEdit& ps = m_scales[m_edit_slot[p]];
                if (ps.spread == Spread::Keep)
                {
                    for (int a = 0; a < 3; ++a)
                    {
                        local.scl[a] /= ps.axis[a];
                    }
                }
            }

            const int32_t slot = m_edit_slot[i];
            if (slot >= 0)
            {
                const BoneEdit& s = m_scales[slot];
                Xform edited = local;
                // Move along the bone's own axes (expressed in the parent's space).
                if (s.has_shift())
                {
                    double offset[3];
                    xf::qrotate(local.rot, s.shift, offset);
                    for (int a = 0; a < 3; ++a)
                    {
                        edited.pos[a] += offset[a] / (std::fabs(parent_after.scl[a]) > xf::kTiny ? parent_after.scl[a] : 1.0);
                    }
                }
                // Rotate about the bone's own axes: the extra turn happens before the bone's rest rotation.
                if (s.has_turn())
                {
                    xf::qmul(local.rot, m_turns[slot].data(), edited.rot);
                }
                for (int a = 0; a < 3; ++a)
                {
                    edited.scl[a] *= s.axis[a];
                }
                m_fresh[i] = xf::compose(edited, parent_after);
                m_for_children[i] = s.spread == Spread::Solo ? xf::compose(local, parent_after) : m_fresh[i];
            }
            else
            {
                m_fresh[i] = xf::compose(local, parent_after);
                m_for_children[i] = m_fresh[i];
            }
            m_fresh[i].pos[3] = 0.0;
            m_fresh[i].scl[3] = 0.0;
            if (m_quantize)
            {
                for (int a = 0; a < 4; ++a)
                {
                    m_fresh[i].rot[a] = static_cast<double>(static_cast<float>(m_fresh[i].rot[a]));
                    m_fresh[i].pos[a] = static_cast<double>(static_cast<float>(m_fresh[i].pos[a]));
                    m_fresh[i].scl[a] = static_cast<double>(static_cast<float>(m_fresh[i].scl[a]));
                }
            }
        }

        // 4) Commit, remembering what we started from and what we wrote.
        for (const int32_t i : m_plan)
        {
            if (!xf::finite(m_fresh[i]))
            {
                continue;
            }
            pose[i] = m_fresh[i];
            memo.source[i] = m_source[i];
            memo.result[i] = m_fresh[i];
            memo.marked[i] = 1;
            m_touched.push_back(i);
            ++written;
        }

        // Step 1 unmarked every earlier bone outside the plan, so only planned bones can be marked.
        memo.marked_list.clear();
        for (const int32_t i : m_plan)
        {
            if (memo.marked[i])
            {
                memo.marked_list.push_back(i);
            }
        }
        return written;
    }
} // namespace uuepbs
