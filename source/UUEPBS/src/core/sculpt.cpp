#include "sculpt.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

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

    namespace
    {
        bool has_any(const std::string& folded, std::initializer_list<const char*> words)
        {
            for (const char* w : words)
            {
                if (folded.find(w) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        // IK targets, twist / roll helpers and control bones never carry the leg itself.
        bool helper_bone(const std::string& folded)
        {
            if (has_any(folded, {"twist", "roll", "ctrl", "pole", "target", "corrective", "correct", "socket", "attach"}))
            {
                return true;
            }
            // "ik" as its own word: ik_foot_l, foot_ik, IK_Foot_L - but not "kick" or "spike"
            const size_t n = folded.size();
            for (size_t at = folded.find("ik"); at != std::string::npos; at = folded.find("ik", at + 1))
            {
                const bool start = at == 0 || !std::isalpha(static_cast<unsigned char>(folded[at - 1]));
                const bool end = at + 2 >= n || !std::isalpha(static_cast<unsigned char>(folded[at + 2]));
                if (start && end)
                {
                    return true;
                }
            }
            return false;
        }

        double vlen(const double v[3])
        {
            return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }

        double vdot(const double a[3], const double b[3])
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        void vcross(const double a[3], const double b[3], double out[3])
        {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }

        // Shortest rotation taking direction `from` onto direction `to`. False when they are opposite
        // (no unique answer) or degenerate.
        bool arc(const double from[3], const double to[3], double out[4])
        {
            const double lf = vlen(from), lt = vlen(to);
            if (lf < 1.0e-6 || lt < 1.0e-6)
            {
                return false;
            }
            double c[3];
            vcross(from, to, c);
            const double w = lf * lt + vdot(from, to);
            const double len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + w * w);
            if (w < 1.0e-9 * lf * lt || len < 1.0e-12)
            {
                return false;
            }
            out[0] = c[0] / len;
            out[1] = c[1] / len;
            out[2] = c[2] / len;
            out[3] = w / len;
            return true;
        }

        // angle between two rotations in degrees (robust for nearly equal, float-normalised quaternions)
        double angle_deg(const double a[4], const double b[4])
        {
            double ia[4], r[4];
            xf::qconj(a, ia);
            xf::qmul(ia, b, r);
            const double v = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            return 114.59155902616465 * std::atan2(v, std::fabs(r[3]));
        }

        void qnormalize(double q[4])
        {
            const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (len > 1.0e-12)
            {
                for (int a = 0; a < 4; ++a)
                {
                    q[a] /= len;
                }
            }
        }

        // p' = centre + q (p - centre)
        void turn_about(const double q[4], const double centre[3], double p[3])
        {
            const double d[3] = {p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]};
            double r[3];
            xf::qrotate(q, d, r);
            for (int a = 0; a < 3; ++a)
            {
                p[a] = centre[a] + r[a];
            }
        }
    } // namespace

    std::vector<LegChain> find_legs(const std::vector<std::string>& names, const std::vector<int32_t>& parents)
    {
        std::vector<LegChain> legs;
        const int32_t n = static_cast<int32_t>(names.size());
        if (parents.size() != names.size())
        {
            return legs;
        }
        std::vector<std::string> folded(names.size());
        for (int32_t i = 0; i < n; ++i)
        {
            folded[i] = fold_case(names[i]);
        }
        for (int32_t f = 0; f < n && legs.size() < 4; ++f)
        {
            if (!has_any(folded[f], {"foot", "ankle"}) || helper_bone(folded[f]))
            {
                continue;
            }
            const int32_t c = parents[f];
            if (c < 0 || helper_bone(folded[c]) || has_any(folded[c], {"foot", "ankle"}) ||
                !has_any(folded[c], {"calf", "knee", "lowerleg", "shin", "leg", "xiaotui"}))
            {
                continue;
            }
            const int32_t t = parents[c];
            if (t < 0 || helper_bone(folded[t]) || !has_any(folded[t], {"thigh", "upleg", "upperleg", "hip", "leg", "datui", "momo"}))
            {
                continue;
            }
            bool taken = false;
            for (const LegChain& l : legs)
            {
                taken = taken || l.thigh == t || l.calf == c;
            }
            if (!taken)
            {
                int32_t ball = -1;
                for (int32_t j = f + 1; j < n && ball < 0; ++j)
                {
                    if (parents[j] == f && has_any(folded[j], {"ball", "toe"}) && !helper_bone(folded[j]))
                    {
                        ball = j;
                    }
                }
                legs.push_back({t, c, f, ball});
            }
        }
        return legs;
    }

    void RigSculptor::set_ground(const GroundOptions& options)
    {
        GroundOptions o = options;
        if (!std::isfinite(o.extra))
        {
            o.extra = 0.0;
        }
        o.max = std::isfinite(o.max) ? std::min(200.0, std::max(0.0, std::fabs(o.max))) : 40.0;
        o.extra = std::min(o.max, std::max(-o.max, o.extra));
        const bool same = o.enabled == m_ground.enabled && o.leg_ik == m_ground.leg_ik && o.extra == m_ground.extra && o.max == m_ground.max;
        m_ground = o;
        if (!same)
        {
            m_revision = ~0ull; // re-plan: with Auto Ground on, every bone is rebuilt
        }
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

        // Legs for Auto Ground, and which leg link every bone hangs from.
        m_legs = find_legs(m_names, m_parents);
        m_leg_of.assign(n, int8_t{-1});
        m_leg_part.assign(n, uint8_t{0});
        for (size_t li = 0; li < m_legs.size(); ++li)
        {
            const LegChain& leg = m_legs[li];
            for (size_t i = static_cast<size_t>(leg.thigh); i < n; ++i)
            {
                const int32_t bone = static_cast<int32_t>(i);
                const int32_t p = m_parents[i];
                uint8_t part = 0;
                if (bone == leg.thigh)
                {
                    part = 1;
                }
                else if (p >= 0 && m_leg_of[p] == static_cast<int8_t>(li))
                {
                    part = bone == leg.calf ? 2 : bone == leg.foot ? 3 : m_leg_part[p];
                }
                if (part)
                {
                    m_leg_of[i] = static_cast<int8_t>(li);
                    m_leg_part[i] = part;
                }
            }
        }
        m_last_lift = 0.0;

        // Landmarks for the ground diagnostics: the legs' hip (else "pelvis"), the first neck bone, the head.
        m_marks = {-1, -1, -1};
        if (!m_legs.empty())
        {
            m_marks[0] = m_parents[m_legs.front().thigh];
        }
        for (size_t i = 0; i < n; ++i)
        {
            const std::string& f = m_folded[i];
            if (m_marks[0] < 0 && (f == "pelvis" || f == "hips" || f == "hip"))
            {
                m_marks[0] = static_cast<int32_t>(i);
            }
            if (m_marks[1] < 0 && f.find("neck") != std::string::npos && !helper_bone(f))
            {
                m_marks[1] = static_cast<int32_t>(i);
            }
            if (m_marks[2] < 0 && f == "head")
            {
                m_marks[2] = static_cast<int32_t>(i);
            }
        }
        return true;
    }

    std::string RigSculptor::ground_report() const
    {
        char buf[900];
        int len = std::snprintf(buf, sizeof buf, "%s lift %.2f", m_ground_primary ? "main" : "other", m_last_lift);
        if (m_ground_primary && !m_legs.empty())
        {
            len += std::snprintf(buf + len, sizeof buf - len, " (deepest sole %.2f)", m_raw_lift);
            for (size_t li = 0; li < m_legs.size() && li < m_leg_diag.size() && len < static_cast<int>(sizeof buf) - 160; ++li)
            {
                const LegChain& l = m_legs[li];
                len += std::snprintf(buf + len, sizeof buf - len, " | %s(%s) sank: ankle %.2f sole %.2f, reach allows %.2f, foot z %.2f->%.2f", m_names[l.foot].c_str(),
                                     l.ball >= 0 ? m_names[l.ball].c_str() : "-", m_leg_diag[li][0], m_leg_diag[li][1], m_leg_diag[li][2],
                                     m_source[l.foot].pos[2], m_fresh[l.foot].pos[2]);
            }
        }
        if (!m_ground_primary)
        {
            const char* what = m_copy_state == 1 ? "copies the main mesh's pose: shared bones not edited again"
                               : m_copy_state == 2 ? "animated on its own: edited and grounded"
                                                   : "not known yet";
            len += std::snprintf(buf + len, sizeof buf - len, " (%s; off main after %.2f cm / before %.2f cm)", what, m_copy_err_after, m_copy_err_before);
        }
        static const char* labels[3] = {"hip", "neck", "head"};
        for (int k = 0; k < 3 && len < static_cast<int>(sizeof buf) - 40; ++k)
        {
            if (m_marks[k] >= 0)
            {
                len += std::snprintf(buf + len, sizeof buf - len, " | %s %s %.2f->%.2f", labels[k], m_names[m_marks[k]].c_str(), m_mark_src[k], m_mark_out[k]);
            }
        }
        return std::string(buf, static_cast<size_t>(std::max(0, std::min(len, static_cast<int>(sizeof buf) - 1))));
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

        bool any_edit = false; // anywhere on the character, even on bones this mesh does not have
        for (const auto& [key, entry] : book)
        {
            if (entry.edit.is_neutral())
            {
                continue;
            }
            any_edit = true;
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
        // With Auto Ground a mesh none of whose bones has a slider (a head mesh when only the legs are edited) still
        // has to move with the lifted body when it is animated on its own.
        // A non-zero extra (a mod mesh whose soles sit above the game's ground) applies with no sliders too.
        if (m_scales.empty() && !(m_ground.enabled && (any_edit || std::fabs(m_ground.extra) > 1.0e-6)))
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
            // A "this bone only" edit leaves the children alone - except on another mesh of the character: when its
            // pose is copied from the main mesh, its own bones below arrive carrying the edit and must be put back.
            pushes_children[i] = moved_by_ancestor || (edited && (m_scales[slot].spread != Spread::Solo || !m_ground_primary));
        }

        // Auto Ground moves the whole pose (and bends legs), so every bone is rebuilt and remembered.
        if (m_ground.enabled)
        {
            m_plan.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                m_plan[i] = static_cast<int32_t>(i);
                m_in_plan[i] = 1;
            }
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
        m_last_lift = 0.0;
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

        // 2b) Another mesh of a character: is its pose a copy of the main mesh's edited pose?
        m_scratch_pose = pose;
        decide_copy();
        // A copy already carries the main mesh's edits on every bone it shares with it: leave those alone.
        const bool copy = !m_ground_primary && m_copy_state == 1 && m_map != nullptr;
        auto shared_bone = [&](int32_t b) { return copy && b >= 0 && m_map[b] >= 0; };

        // 3) Rebuild top-down (parents always precede children).
        const Xform unit = xf::identity();
        for (const int32_t i : m_plan)
        {
            if (shared_bone(i))
            {
                // copied from the main mesh's finished pose: keep it exactly as it is
                m_fresh[i] = m_source[i];
                m_for_children[i] = m_source[i];
                const int32_t slot = m_edit_slot[i];
                if (slot >= 0 && m_scales[slot].spread == Spread::Solo)
                {
                    // "This bone only": on the main mesh its children hang from the bone as it was before its own
                    // edit. This mesh's own bones below it (face, hair...) must do the same: take the edit back out.
                    const BoneEdit& s = m_scales[slot];
                    const int32_t p = m_parents[i];
                    const Xform& parent_after = p < 0 ? unit : (m_in_plan[p] ? m_for_children[p] : pose[p]);
                    Xform local = p < 0 ? m_source[i] : xf::relative(m_source[i], parent_after);
                    for (int a = 0; a < 3; ++a)
                    {
                        local.scl[a] /= s.axis[a];
                    }
                    if (s.has_turn())
                    {
                        double undo[4], rot[4];
                        xf::qconj(m_turns[slot].data(), undo);
                        xf::qmul(local.rot, undo, rot);
                        std::memcpy(local.rot, rot, sizeof rot);
                    }
                    if (s.has_shift())
                    {
                        double offset[3];
                        xf::qrotate(local.rot, s.shift, offset);
                        for (int a = 0; a < 3; ++a)
                        {
                            local.pos[a] -= offset[a] / (std::fabs(parent_after.scl[a]) > xf::kTiny ? parent_after.scl[a] : 1.0);
                        }
                    }
                    m_for_children[i] = xf::compose(local, parent_after);
                    m_for_children[i].pos[3] = m_for_children[i].scl[3] = 0.0;
                }
                continue;
            }
            const int32_t p = m_parents[i];
            const Xform& parent_before = p < 0 ? unit : (m_in_plan[p] ? m_source[p] : pose[p]);
            const Xform& parent_after = p < 0 ? unit : (m_in_plan[p] ? m_for_children[p] : pose[p]);

            Xform local = p < 0 ? m_source[i] : xf::relative(m_source[i], parent_before);

            // "Keep children size" - also for this mesh's own bones below a bone copied from the main mesh: the copy
            // carries the parent's new size, which they must not take on.
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

            const int32_t slot = shared_bone(i) ? -1 : m_edit_slot[i];
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
        }

        // 3b) Auto Ground: lift the pose and bend the other legs back onto the ground.
        apply_ground(pose);

        if (m_quantize)
        {
            for (const int32_t i : m_plan)
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

        // Main mesh: publish where every bone was before and after, for the character's other meshes.
        if (m_ground_primary)
        {
            m_pub_before.resize(static_cast<size_t>(n) * kPublishStride);
            m_pub_after.resize(static_cast<size_t>(n) * kPublishStride);
            for (int32_t b = 0; b < n; ++b)
            {
                const Xform& src = m_in_plan[b] ? m_source[b] : pose[b];
                double* pb = &m_pub_before[static_cast<size_t>(b) * kPublishStride];
                double* pa = &m_pub_after[static_cast<size_t>(b) * kPublishStride];
                for (int a = 0; a < 3; ++a)
                {
                    pb[a] = src.pos[a];
                    pa[a] = pose[b].pos[a];
                    pb[3 + a] = src.scl[a];
                    pa[3 + a] = pose[b].scl[a];
                }
                for (int a = 0; a < 4; ++a)
                {
                    pb[6 + a] = src.rot[a];
                    pa[6 + a] = pose[b].rot[a];
                }
            }
        }

        // Ground diagnostics: landmark heights before (the game's pose) and after.
        for (int k = 0; k < 3; ++k)
        {
            const int32_t b = m_marks[k];
            if (b >= 0 && b < n)
            {
                m_mark_src[k] = m_in_plan[b] ? m_source[b].pos[2] : pose[b].pos[2];
                m_mark_out[k] = pose[b].pos[2];
            }
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

    // Compares this mesh's untouched pose with where the main mesh's bones were before and after its edit
    // (bones the edit moved at least a little). Needs three frames in a row to change its mind.
    void RigSculptor::decide_copy()
    {
        if (m_ground_primary || !m_map || !m_ref_before || !m_ref_after || m_ref_count == 0 || !m_scratch_pose)
        {
            return;
        }
        const size_t n = m_names.size();
        double err_after = 0.0, err_before = 0.0;
        size_t count = 0;
        auto published = [&](const double* base, int32_t j) {
            const double* v = base + static_cast<size_t>(j) * kPublishStride;
            Xform x{};
            for (int a = 0; a < 3; ++a)
            {
                x.pos[a] = v[a];
                x.scl[a] = v[3 + a];
            }
            for (int a = 0; a < 4; ++a)
            {
                x.rot[a] = v[6 + a];
            }
            return x;
        };
        auto source_of = [&](size_t i) -> const Xform& { return m_in_plan[i] ? m_source[i] : m_scratch_pose[i]; };
        // position in cm, scale weighted so 1% counts like half a centimetre, rotation 1 degree like 2 mm
        auto gap = [](const Xform& x, const Xform& r) {
            const double dp[3] = {x.pos[0] - r.pos[0], x.pos[1] - r.pos[1], x.pos[2] - r.pos[2]};
            const double ds[3] = {x.scl[0] - r.scl[0], x.scl[1] - r.scl[1], x.scl[2] - r.scl[2]};
            return vlen(dp) + 50.0 * vlen(ds) + 0.2 * angle_deg(x.rot, r.rot);
        };
        for (size_t i = 0; i < n; ++i)
        {
            const int32_t j = m_map[i];
            if (j < 0 || static_cast<size_t>(j) >= m_ref_count)
            {
                continue;
            }
            Xform src = source_of(i);
            Xform b = published(m_ref_before, j);
            Xform a = published(m_ref_after, j);
            // Below a shared parent, compare the bone as seen from its parent: the edit shows there just the same, while
            // the motion of the body as a whole cancels out (in case this mesh is a frame apart from the main mesh).
            // Bones without one (the root, the pelvis of a head mesh) are compared as they are: that is where a lift shows.
            const int32_t p = m_parents[i];
            const int32_t pj = p >= 0 ? m_map[p] : -1;
            if (pj >= 0 && static_cast<size_t>(pj) < m_ref_count)
            {
                src = xf::relative(src, source_of(static_cast<size_t>(p)));
                b = xf::relative(b, published(m_ref_before, pj));
                a = xf::relative(a, published(m_ref_after, pj));
            }
            const double moved_pos[3] = {a.pos[0] - b.pos[0], a.pos[1] - b.pos[1], a.pos[2] - b.pos[2]};
            const double moved_scl[3] = {a.scl[0] - b.scl[0], a.scl[1] - b.scl[1], a.scl[2] - b.scl[2]};
            if (vlen(moved_pos) < 0.05 && vlen(moved_scl) < 0.002 && angle_deg(a.rot, b.rot) < 0.1)
            {
                continue; // the edit did not change this bone: says nothing
            }
            err_after += gap(src, a);
            err_before += gap(src, b);
            ++count;
        }
        if (count == 0)
        {
            return; // nothing moved: keep what we knew
        }
        err_after /= static_cast<double>(count);
        err_before /= static_cast<double>(count);
        m_copy_err_after = err_after;
        m_copy_err_before = err_before;
        const int candidate = (err_after < 1.5 && err_after < 0.5 * err_before) ? 1 : (err_before < 1.5 && err_before < 0.5 * err_after) ? 2 : 0;
        // An unclear frame (fast motion while the other mesh is a frame apart) says nothing: keep what we knew,
        // or the mesh would get its shared bones edited a second time for a moment.
        if (candidate == m_copy_state || candidate == 0)
        {
            m_copy_votes = 0;
            return;
        }
        if (candidate == m_copy_candidate)
        {
            ++m_copy_votes;
        }
        else
        {
            m_copy_candidate = candidate;
            m_copy_votes = 1;
        }
        if (m_copy_votes >= 3)
        {
            m_copy_state = candidate;
            m_copy_votes = 0;
        }
    }

    // m_source holds the game's pose (its foot IK put the feet on the ground), m_fresh the edited
    // one. Both are complete here: with Auto Ground on, every bone is in the plan.
    void RigSculptor::apply_ground(Xform* /*pose*/)
    {
        const size_t n = m_names.size();
        if (!m_ground.enabled || m_plan.size() != n || n == 0)
        {
            return;
        }

        // Other meshes of the character (head, outfit, even ones on the full skeleton with legs of their own):
        // where does their pose come from? Told apart by the hip bone against the main mesh's hip:
        //   * at the main mesh's hip *after* the edit: the pose is a copy of the main mesh's final, grounded pose
        //     (Copy Pose / Leader Pose) - it is already lifted, so nothing to add (lifting it again floats it);
        //   * at the main mesh's hip *before* the edit: animated on its own - lifted like the main mesh;
        //   * anywhere else (hanging from a bone, another space): left alone.
        m_took_shared = false;
        m_follows = false;
        bool own_legs = !m_legs.empty();
        if (!m_ground_primary)
        {
            if (m_probe >= 0 && static_cast<size_t>(m_probe) < n)
            {
                m_probe_seen = m_source[m_probe].pos[2];
            }
            if (m_copy_state == 1)
            {
                m_follows = true; // a copy of the main mesh's pose: already grounded
                return;
            }
            if (m_copy_state != 2)
            {
                return; // not known (yet) or neither: leave it alone
            }
            own_legs = !m_legs.empty(); // animated on its own: measure its own legs (or take the shared lift)
        }

        // How far each foot's sole sank because of the edit (component space, +Z is up).
        double raw = 0.0;
        double total = 0.0;
        std::array<double, 4> sink{};
        if (own_legs)
        {
            raw = -1.0e30;
            for (size_t li = 0; li < m_legs.size(); ++li)
            {
                sink[li] = contact_sink(m_legs[li]);
                raw = std::max(raw, sink[li]);
                const int32_t f = m_legs[li].foot;
                m_leg_diag[li] = {m_source[f].pos[2] - m_fresh[f].pos[2], sink[li], 0.0};
            }
            if (!std::isfinite(raw))
            {
                return;
            }
            m_raw_lift = raw;
            // Lifting by the foot that sank most leaves the others in the air; their legs bend down to the ground,
            // but a leg can only stretch until it is straight. When one would have to be longer than that (one leg
            // edited more than the other, a turned pelvis or thigh, a moved foot), lift less: every leg can then reach,
            // and a foot that would now be below the ground is bent up instead.
            if (m_ground.leg_ik)
            {
                for (size_t li = 0; li < m_legs.size(); ++li)
                {
                    const LegChain& leg = m_legs[li];
                    const double* H = m_fresh[leg.thigh].pos;
                    const double* K = m_fresh[leg.calf].pos;
                    const double* F = m_fresh[leg.foot].pos;
                    const double hk[3] = {K[0] - H[0], K[1] - H[1], K[2] - H[2]};
                    const double kf[3] = {F[0] - K[0], F[1] - K[1], F[2] - K[2]};
                    const double reach = vlen(hk) + vlen(kf) - 1.0e-3;
                    const double across2 = (F[0] - H[0]) * (F[0] - H[0]) + (F[1] - H[1]) * (F[1] - H[1]);
                    if (reach > 0.0 && across2 < reach * reach)
                    {
                        // foot target height (before the lift) minus hip height, plus the straight leg's height
                        const double most = (F[2] + sink[li] - H[2]) + std::sqrt(reach * reach - across2);
                        m_leg_diag[li][2] = most;
                        raw = std::min(raw, most);
                    }
                }
            }
            total = raw + m_ground.extra;
        }
        else if (!m_ground_primary)
        {
            total = m_shared_lift; // independent mesh without legs: lift it with the main mesh
            m_took_shared = true;
        }
        else
        {
            return; // a main mesh without legs: nothing to measure
        }
        if (!std::isfinite(total))
        {
            return;
        }
        total = std::min(m_ground.max, std::max(-m_ground.max, total));
        m_last_lift = total;

        // The foot that sank most goes back exactly onto the ground; lift everything by that much.
        if (std::fabs(total) > 1.0e-6)
        {
            for (size_t i = 0; i < n; ++i)
            {
                m_fresh[i].pos[2] += total;
            }
        }

        // Hip height before / after, for the character's other meshes to compare against.
        if (!m_legs.empty())
        {
            const int32_t hip = hip_bone();
            if (hip >= 0)
            {
                m_hip_before = m_source[hip].pos[2];
                m_hip_after = m_fresh[hip].pos[2];
            }
        }

        // Every foot is now (lift - its sink) off the ground: bend its leg down (or up) to where the game had it.
        if (own_legs && m_ground.leg_ik)
        {
            const double lifted = total - m_ground.extra; // the lift without the extra (which every foot keeps)
            for (size_t li = 0; li < m_legs.size(); ++li)
            {
                const double drop = std::min(m_ground.max, std::max(-m_ground.max, lifted - sink[li]));
                if (std::fabs(drop) > 0.01)
                {
                    bend_leg(m_legs[li], li, drop);
                }
            }
        }
    }

    // How far the sole sank: the lower of the ball (toes) bone and a heel point the ball's height below the ankle,
    // both carried by the foot - so a bigger, smaller or turned foot is measured where it touches the ground.
    // Without a ball bone, the ankle joint itself.
    double RigSculptor::contact_sink(const LegChain& leg) const
    {
        const Xform& was = m_source[leg.foot];
        const Xform& now = m_fresh[leg.foot];
        if (leg.ball < 0)
        {
            return was.pos[2] - now.pos[2];
        }
        double sink = m_source[leg.ball].pos[2] - m_fresh[leg.ball].pos[2];
        const double h = was.pos[2] - m_source[leg.ball].pos[2];
        if (h > 0.0)
        {
            const double down[3] = {0.0, 0.0, -h};
            double inv[4], local[3], scaled[3], turned[3];
            xf::qconj(was.rot, inv);
            xf::qrotate(inv, down, local);
            for (int a = 0; a < 3; ++a)
            {
                scaled[a] = now.scl[a] * local[a] * xf::safe_inverse(was.scl[a]);
            }
            xf::qrotate(now.rot, scaled, turned);
            sink = std::max(sink, (was.pos[2] - h) - (now.pos[2] + turned[2]));
        }
        return sink;
    }

    // Two-bone IK in component space: moves the foot `drop` cm down (+Z is up) by turning the thigh
    // and the calf in the plane the leg already bends in. The foot keeps its rotation; everything below
    // each link follows it rigidly.
    void RigSculptor::bend_leg(const LegChain& leg, size_t leg_index, double drop)
    {
        const double* hip = m_fresh[leg.thigh].pos;
        const double* knee = m_fresh[leg.calf].pos;
        const double* foot = m_fresh[leg.foot].pos;
        const double H[3] = {hip[0], hip[1], hip[2]};
        const double K[3] = {knee[0], knee[1], knee[2]};
        const double F[3] = {foot[0], foot[1], foot[2]};
        const double T[3] = {F[0], F[1], F[2] - drop};

        const double hk[3] = {K[0] - H[0], K[1] - H[1], K[2] - H[2]};
        const double kf[3] = {F[0] - K[0], F[1] - K[1], F[2] - K[2]};
        const double a = vlen(hk), b = vlen(kf);
        double ht[3] = {T[0] - H[0], T[1] - H[1], T[2] - H[2]};
        double d = vlen(ht);
        if (a < 1.0e-3 || b < 1.0e-3 || d < 1.0e-3)
        {
            return;
        }
        const double u[3] = {ht[0] / d, ht[1] / d, ht[2] / d};
        // Out of reach: the leg straightens (or folds) as far as it can.
        d = std::min(a + b - 1.0e-4, std::max(std::fabs(a - b) + 1.0e-3, d));

        // Bend direction: where the knee points now, across the hip -> target line.
        double v[3];
        const double along = vdot(hk, u);
        for (int k = 0; k < 3; ++k)
        {
            v[k] = hk[k] - along * u[k];
        }
        double vl = vlen(v);
        if (vl < 0.01 * a && leg.ball >= 0)
        {
            // (nearly) straight leg pointing at the target - which way it bends is noise: the knee goes the way the
            // toes point
            const double* B = m_fresh[leg.ball].pos;
            const double toes[3] = {B[0] - F[0], B[1] - F[1], B[2] - F[2]};
            const double t_along = vdot(toes, u);
            for (int k = 0; k < 3; ++k)
            {
                v[k] = toes[k] - t_along * u[k];
            }
            vl = vlen(v);
        }
        if (vl < 1.0e-4)
        {
            // still nothing to go by: bend about the thigh's own Z axis
            const double z[3] = {0.0, 0.0, 1.0};
            double w[3];
            xf::qrotate(m_fresh[leg.thigh].rot, z, w);
            vcross(w, u, v);
            vl = vlen(v);
            if (vl < 1.0e-4)
            {
                return;
            }
        }
        for (int k = 0; k < 3; ++k)
        {
            v[k] /= vl;
        }

        const double cos_hip = std::min(1.0, std::max(-1.0, (a * a + d * d - b * b) / (2.0 * a * d)));
        const double sin_hip = std::sqrt(std::max(0.0, 1.0 - cos_hip * cos_hip));
        double K2[3], F2[3];
        for (int k = 0; k < 3; ++k)
        {
            K2[k] = H[k] + a * (cos_hip * u[k] + sin_hip * v[k]);
            F2[k] = H[k] + d * u[k];
        }

        // Thigh: turn about the hip so the knee lands on K2.
        const double hk2[3] = {K2[0] - H[0], K2[1] - H[1], K2[2] - H[2]};
        double r1[4];
        if (!arc(hk, hk2, r1))
        {
            return;
        }
        // Calf: after the thigh's turn, turn about the new knee so the foot lands on F2.
        double kf_turned[3];
        xf::qrotate(r1, kf, kf_turned);
        const double kf2[3] = {F2[0] - K2[0], F2[1] - K2[1], F2[2] - K2[2]};
        double r2[4];
        if (!arc(kf_turned, kf2, r2))
        {
            r2[0] = r2[1] = r2[2] = 0.0;
            r2[3] = 1.0;
        }
        double r21[4];
        xf::qmul(r2, r1, r21);
        const double shift[3] = {F2[0] - F[0], F2[1] - F[1], F2[2] - F[2]};

        const int8_t li = static_cast<int8_t>(leg_index);
        for (size_t i = static_cast<size_t>(leg.thigh); i < m_names.size(); ++i)
        {
            if (m_leg_of[i] != li)
            {
                continue;
            }
            Xform& x = m_fresh[i];
            double rot[4];
            switch (m_leg_part[i])
            {
            case 1: // thigh and what hangs from it above the knee (twist bones, muscles)
                turn_about(r1, H, x.pos);
                xf::qmul(r1, x.rot, rot);
                break;
            case 2: // calf and what hangs from it above the foot
                turn_about(r1, H, x.pos);
                turn_about(r2, K2, x.pos);
                xf::qmul(r21, x.rot, rot);
                break;
            case 3: // foot, ball, toes: same rotation, new place
                for (int k = 0; k < 3; ++k)
                {
                    x.pos[k] += shift[k];
                }
                continue;
            default:
                continue;
            }
            qnormalize(rot);
            for (int k = 0; k < 4; ++k)
            {
                x.rot[k] = rot[k];
            }
        }
    }
} // namespace uuepbs
