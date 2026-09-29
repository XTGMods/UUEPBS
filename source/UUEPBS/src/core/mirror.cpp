#include "mirror.hpp"

#include "rig_names.hpp"

#include <cmath>

namespace uuepbs
{
    namespace
    {
        using Mat = double[3][3];

        // Columns are the bone's X, Y, Z axes in component space.
        void axes_of(const double q[4], Mat out)
        {
            for (int j = 0; j < 3; ++j)
            {
                const double e[3] = {j == 0 ? 1.0 : 0.0, j == 1 ? 1.0 : 0.0, j == 2 ? 1.0 : 0.0};
                double v[3];
                xf::qrotate(q, e, v);
                for (int i = 0; i < 3; ++i)
                {
                    out[i][j] = v[i];
                }
            }
        }

        void normalise(double q[4])
        {
            const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (len > 1e-12)
            {
                for (int i = 0; i < 4; ++i)
                {
                    q[i] /= len;
                }
            }
        }

        // K = C_P^T * M * C_B rounded to a signed permutation.
        bool measure(const double qb[4], const double qp[4], int centre, MirrorRule& rule)
        {
            Mat cb, cp;
            axes_of(qb, cb);
            axes_of(qp, cp);
            double k[3][3];
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    double sum = 0.0;
                    for (int r = 0; r < 3; ++r)
                    {
                        const double m = r == centre ? -1.0 : 1.0;
                        sum += cp[r][i] * m * cb[r][j];
                    }
                    k[i][j] = sum;
                }
            }
            bool used[3] = {false, false, false};
            double weakest = 1.0;
            int sign[3];
            for (int i = 0; i < 3; ++i)
            {
                int best = 0;
                for (int j = 1; j < 3; ++j)
                {
                    if (std::fabs(k[i][j]) > std::fabs(k[i][best]))
                    {
                        best = j;
                    }
                }
                if (used[best])
                {
                    return false;
                }
                used[best] = true;
                rule.source[i] = static_cast<int8_t>(best);
                sign[i] = k[i][best] < 0 ? -1 : 1;
                weakest = std::fmin(weakest, std::fabs(k[i][best]));
            }
            if (weakest < 0.5)
            {
                return false;
            }
            // det of a signed permutation = product of signs * parity of the permutation
            int parity = 1;
            for (int i = 0; i < 3; ++i)
            {
                for (int j = i + 1; j < 3; ++j)
                {
                    if (rule.source[i] > rule.source[j])
                    {
                        parity = -parity;
                    }
                }
            }
            const int det = sign[0] * sign[1] * sign[2] * parity;
            for (int i = 0; i < 3; ++i)
            {
                rule.move_sign[i] = static_cast<int8_t>(sign[i]);
                rule.turn_sign[i] = static_cast<int8_t>(det * sign[i]);
            }
            rule.measured = true;
            rule.exact = weakest > 0.9;
            return true;
        }
    } // namespace

    MirrorTable build_mirror_table(const std::vector<std::string>& names, const std::vector<int32_t>& parents, const std::vector<Xform>& pose,
                                   bool local)
    {
        MirrorTable table;
        const size_t n = names.size();
        if (n == 0 || parents.size() != n || pose.size() != n)
        {
            return table;
        }

        std::vector<Xform> comp(n);
        for (size_t i = 0; i < n; ++i)
        {
            const int32_t p = parents[i];
            comp[i] = (local && p >= 0 && p < static_cast<int32_t>(i)) ? xf::compose(pose[i], comp[p]) : pose[i];
            normalise(comp[i].rot);
        }

        std::map<std::string, size_t> index;
        for (size_t i = 0; i < n; ++i)
        {
            index.emplace(fold_case(names[i]), i);
        }

        struct Pair
        {
            size_t a, b;
        };
        std::vector<Pair> pairs;
        for (size_t i = 0; i < n; ++i)
        {
            const std::string other = mirror_bone_name(names[i]);
            if (other.empty())
            {
                continue;
            }
            const auto it = index.find(fold_case(other));
            if (it != index.end() && it->second != i)
            {
                pairs.push_back({i, it->second});
            }
        }
        if (pairs.empty())
        {
            return table;
        }

        // The centre plane is the axis along which paired joints are furthest apart.
        double spread[3] = {0, 0, 0};
        for (const Pair& p : pairs)
        {
            for (int a = 0; a < 3; ++a)
            {
                spread[a] += std::fabs(comp[p.a].pos[a] - comp[p.b].pos[a]);
            }
        }
        int centre = 0;
        for (int a = 1; a < 3; ++a)
        {
            if (spread[a] > spread[centre])
            {
                centre = a;
            }
        }
        if (spread[centre] < 1e-6)
        {
            return table; // collapsed pose, nothing to measure
        }
        table.centre_axis = centre;

        for (const Pair& p : pairs)
        {
            MirrorRule rule;
            if (measure(comp[p.a].rot, comp[p.b].rot, centre, rule))
            {
                table.rules[fold_case(names[p.a])] = rule;
                ++table.pairs;
                if (!rule.exact)
                {
                    ++table.inexact;
                }
            }
        }
        table.pairs /= 2; // every pair was visited from both ends
        table.inexact = (table.inexact + 1) / 2;
        return table;
    }

    BoneEdit mirror_with(const MirrorRule& rule, const BoneEdit& e)
    {
        BoneEdit m = e;
        for (int i = 0; i < 3; ++i)
        {
            const int s = rule.source[i];
            m.axis[i] = e.axis[s];
            m.shift[i] = rule.move_sign[i] * e.shift[s];
            m.turn[i] = rule.turn_sign[i] * e.turn[s];
        }
        return m;
    }

    BoneEdit mirror_with(const MirrorTable& table, const std::string& bone, const BoneEdit& e)
    {
        const auto it = table.rules.find(fold_case(bone));
        return mirror_with(it != table.rules.end() ? it->second : MirrorRule{}, e);
    }
} // namespace uuepbs
