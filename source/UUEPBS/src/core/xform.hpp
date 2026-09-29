// UUEPBS - pose transform maths
//
// Mirrors the memory layout and composition rules of UE5's double precision
// FTransform (TTransform<double>) so we can edit the engine's component-space
// pose buffer in place.
//
//   +0x00  Rotation     quaternion x, y, z, w
//   +0x20  Translation  x, y, z, (w padding, kept at 0)
//   +0x40  Scale3D      x, y, z, (w padding, kept at 0)
//
// Composition follows FTransform::Multiply for non-negative scale:
//   (A * B) applies A first, then B.
#pragma once

#include <cmath>
#include <cstring>

namespace uuepbs
{
    struct alignas(16) Xform
    {
        double rot[4];
        double pos[4];
        double scl[4];
    };
    static_assert(sizeof(Xform) == 96, "Xform must match UE5 FTransform (double) layout");

    namespace xf
    {
        constexpr double kTiny = 1.0e-8;

        inline Xform identity()
        {
            Xform x{};
            x.rot[3] = 1.0;
            x.scl[0] = x.scl[1] = x.scl[2] = 1.0;
            return x;
        }

        // Hamilton product a*b. For unit quaternions the result rotates by b first, then a.
        inline void qmul(const double a[4], const double b[4], double out[4])
        {
            const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
            const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
            const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
            const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
            out[0] = x;
            out[1] = y;
            out[2] = z;
            out[3] = w;
        }

        inline void qconj(const double q[4], double out[4])
        {
            out[0] = -q[0];
            out[1] = -q[1];
            out[2] = -q[2];
            out[3] = q[3];
        }

        // v' = q * v * q^-1 (q assumed normalised)
        inline void qrotate(const double q[4], const double v[3], double out[3])
        {
            const double tx = 2.0 * (q[1] * v[2] - q[2] * v[1]);
            const double ty = 2.0 * (q[2] * v[0] - q[0] * v[2]);
            const double tz = 2.0 * (q[0] * v[1] - q[1] * v[0]);
            out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
            out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
            out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
        }

        // child expressed in parent's space  ->  child in parent's parent space
        inline Xform compose(const Xform& child, const Xform& parent)
        {
            Xform r{};
            qmul(parent.rot, child.rot, r.rot);

            const double scaled[3] = {parent.scl[0] * child.pos[0], parent.scl[1] * child.pos[1], parent.scl[2] * child.pos[2]};
            double turned[3];
            qrotate(parent.rot, scaled, turned);
            r.pos[0] = turned[0] + parent.pos[0];
            r.pos[1] = turned[1] + parent.pos[1];
            r.pos[2] = turned[2] + parent.pos[2];

            r.scl[0] = child.scl[0] * parent.scl[0];
            r.scl[1] = child.scl[1] * parent.scl[1];
            r.scl[2] = child.scl[2] * parent.scl[2];
            return r;
        }

        inline double safe_inverse(double v)
        {
            return std::fabs(v) <= kTiny ? 0.0 : 1.0 / v;
        }

        // Returns T such that compose(T, base) == x  (FTransform::GetRelativeTransform)
        inline Xform relative(const Xform& x, const Xform& base)
        {
            Xform r{};
            const double inv[3] = {safe_inverse(base.scl[0]), safe_inverse(base.scl[1]), safe_inverse(base.scl[2])};

            double inv_rot[4];
            qconj(base.rot, inv_rot);

            const double delta[3] = {x.pos[0] - base.pos[0], x.pos[1] - base.pos[1], x.pos[2] - base.pos[2]};
            double local[3];
            qrotate(inv_rot, delta, local);
            r.pos[0] = local[0] * inv[0];
            r.pos[1] = local[1] * inv[1];
            r.pos[2] = local[2] * inv[2];

            qmul(inv_rot, x.rot, r.rot);

            r.scl[0] = x.scl[0] * inv[0];
            r.scl[1] = x.scl[1] * inv[1];
            r.scl[2] = x.scl[2] * inv[2];
            return r;
        }

        inline bool same_bits(const Xform& a, const Xform& b)
        {
            return std::memcmp(&a, &b, sizeof(Xform)) == 0;
        }

        inline bool finite(const Xform& x)
        {
            for (int i = 0; i < 4; ++i)
            {
                if (!std::isfinite(x.rot[i]) || !std::isfinite(x.pos[i]) || !std::isfinite(x.scl[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // Cheap sanity check used when validating a freshly discovered pose buffer.
        inline bool plausible(const Xform& x)
        {
            if (!finite(x))
            {
                return false;
            }
            const double qlen = x.rot[0] * x.rot[0] + x.rot[1] * x.rot[1] + x.rot[2] * x.rot[2] + x.rot[3] * x.rot[3];
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
    } // namespace xf
} // namespace uuepbs
