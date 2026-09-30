#include "soft_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace uuepbs::ui
{
    namespace
    {
        struct Rgba
        {
            int r, g, b, a; // 0..255
        };

        Rgba unpack(ImU32 c)
        {
            return {static_cast<int>((c >> IM_COL32_R_SHIFT) & 0xFF), static_cast<int>((c >> IM_COL32_G_SHIFT) & 0xFF),
                    static_cast<int>((c >> IM_COL32_B_SHIFT) & 0xFF), static_cast<int>((c >> IM_COL32_A_SHIFT) & 0xFF)};
        }

        inline int mul255(int a, int b)
        {
            const int t = a * b + 128;
            return (t + (t >> 8)) >> 8;
        }

        // Source-over blend of a straight-alpha colour into 0x00RRGGBB.
        inline void blend(uint32_t& dst, int r, int g, int b, int a)
        {
            if (a <= 0)
            {
                return;
            }
            if (a >= 255)
            {
                dst = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                return;
            }
            const int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
            const int ia = 255 - a;
            const int nr = mul255(r, a) + mul255(dr, ia);
            const int ng = mul255(g, a) + mul255(dg, ia);
            const int nb = mul255(b, a) + mul255(db, ia);
            dst = (static_cast<uint32_t>(std::min(nr, 255)) << 16) | (static_cast<uint32_t>(std::min(ng, 255)) << 8) | static_cast<uint32_t>(std::min(nb, 255));
        }

        // Bilinear sample, clamped at the edges. Returns 0..255 per channel.
        Rgba sample(const ImTextureData* t, float u, float v)
        {
            const float x = u * static_cast<float>(t->Width) - 0.5f;
            const float y = v * static_cast<float>(t->Height) - 0.5f;
            const int x0 = static_cast<int>(std::floor(x));
            const int y0 = static_cast<int>(std::floor(y));
            const int fx = static_cast<int>((x - static_cast<float>(x0)) * 256.0f);
            const int fy = static_cast<int>((y - static_cast<float>(y0)) * 256.0f);
            const int xa = std::clamp(x0, 0, t->Width - 1), xb = std::clamp(x0 + 1, 0, t->Width - 1);
            const int ya = std::clamp(y0, 0, t->Height - 1), yb = std::clamp(y0 + 1, 0, t->Height - 1);
            const int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;
            const unsigned char* px = t->Pixels;
            if (t->BytesPerPixel == 1) // alpha-only atlas: white with coverage
            {
                const int a = (px[ya * t->Width + xa] * w00 + px[ya * t->Width + xb] * w10 + px[yb * t->Width + xa] * w01 + px[yb * t->Width + xb] * w11) >> 16;
                return {255, 255, 255, a};
            }
            const unsigned char* p00 = px + (static_cast<size_t>(ya) * t->Width + xa) * 4;
            const unsigned char* p10 = px + (static_cast<size_t>(ya) * t->Width + xb) * 4;
            const unsigned char* p01 = px + (static_cast<size_t>(yb) * t->Width + xa) * 4;
            const unsigned char* p11 = px + (static_cast<size_t>(yb) * t->Width + xb) * 4;
            int out[4];
            for (int ch = 0; ch < 4; ++ch)
            {
                out[ch] = (p00[ch] * w00 + p10[ch] * w10 + p01[ch] * w01 + p11[ch] * w11) >> 16;
            }
            return {out[0], out[1], out[2], out[3]};
        }

        // Pixel centres covered by [lo, hi): first..last inclusive.
        inline void span(float lo, float hi, int clip_lo, int clip_hi, int& first, int& last)
        {
            first = std::max(clip_lo, static_cast<int>(std::ceil(lo - 0.5f)));
            last = std::min(clip_hi - 1, static_cast<int>(std::ceil(hi - 0.5f)) - 1);
        }
    } // namespace

    void SoftRenderer::setup_backend()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = "uuepbs_soft";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    }

    void SoftRenderer::shutdown_backend()
    {
        for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
        {
            if (t->Status != ImTextureStatus_Destroyed)
            {
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
    }

    void SoftRenderer::resize(int width, int height)
    {
        m_width = std::max(1, width);
        m_height = std::max(1, height);
        m_quads.clear();
        m_quad_bytes = 0;
        m_pixels.assign(static_cast<size_t>(m_width) * m_height, 0);
        m_last_frame = 0; // next frame must be drawn
    }

    // The pixels stay in ImGui's own CPU copy, so "uploading" is just bookkeeping.
    bool SoftRenderer::update_textures(ImDrawData* dd)
    {
        bool changed = false;
        if (!dd->Textures)
        {
            return changed;
        }
        for (ImTextureData* t : *dd->Textures)
        {
            if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates)
            {
                t->SetTexID(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t)));
                t->SetStatus(ImTextureStatus_OK);
                changed = true;
            }
            else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0)
            {
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
                changed = true;
            }
        }
        return changed;
    }

    uint64_t SoftRenderer::fingerprint(const ImDrawData* dd, uint32_t clear_rgb)
    {
        uint64_t h = 1469598103934665603ull ^ clear_rgb;
        auto mix = [&h](const void* data, size_t size) {
            const auto* p = static_cast<const unsigned char*>(data);
            size_t i = 0;
            for (; i + 8 <= size; i += 8)
            {
                uint64_t v;
                std::memcpy(&v, p + i, 8);
                h = (h ^ v) * 1099511628211ull;
            }
            for (; i < size; ++i)
            {
                h = (h ^ p[i]) * 1099511628211ull;
            }
        };
        mix(&dd->DisplaySize, sizeof(dd->DisplaySize));
        for (const ImDrawList* list : dd->CmdLists)
        {
            mix(list->VtxBuffer.Data, static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
            mix(list->IdxBuffer.Data, static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                const ImTextureID id = cmd.GetTexID();
                mix(&cmd.ClipRect, sizeof(cmd.ClipRect));
                mix(&id, sizeof(id));
                mix(&cmd.ElemCount, sizeof(cmd.ElemCount));
                mix(&cmd.IdxOffset, sizeof(cmd.IdxOffset));
                mix(&cmd.VtxOffset, sizeof(cmd.VtxOffset));
            }
        }
        return h;
    }

    void SoftRenderer::solid_rect(const Clip& c, float x0, float y0, float x1, float y1, ImU32 col)
    {
        const Rgba k = unpack(col);
        int fx, lx, fy, ly;
        span(x0, x1, c.x0, c.x1, fx, lx);
        span(y0, y1, c.y0, c.y1, fy, ly);
        if (fx > lx || fy > ly || k.a == 0)
        {
            return;
        }
        const uint32_t opaque = (static_cast<uint32_t>(k.r) << 16) | (static_cast<uint32_t>(k.g) << 8) | static_cast<uint32_t>(k.b);
        const bool whole = fx == 0 && fy == 0 && lx == m_width - 1 && ly == m_height - 1;
        if (whole && k.a == 255)
        {
            m_uniform = true; // e.g. the main window's background: still one flat colour
            m_uniform_rgb = opaque;
        }
        else if (!(m_uniform && k.a == 255 && opaque == m_uniform_rgb))
        {
            m_uniform = false;
        }
        for (int y = fy; y <= ly; ++y)
        {
            uint32_t* row = &m_pixels[static_cast<size_t>(y) * m_width];
            if (k.a == 255)
            {
                std::fill(row + fx, row + lx + 1, opaque);
            }
            else
            {
                for (int x = fx; x <= lx; ++x)
                {
                    blend(row[x], k.r, k.g, k.b, k.a);
                }
            }
        }
        ++m_stats.fast_rects;
    }

    void SoftRenderer::textured_rect(const Clip& c, const ImTextureData* tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                                     float v1, ImU32 col)
    {
        const Rgba k = unpack(col);
        int fx, lx, fy, ly;
        span(x0, x1, c.x0, c.x1, fx, lx);
        span(y0, y1, c.y0, c.y1, fy, ly);
        if (fx > lx || fy > ly || k.a == 0)
        {
            return;
        }
        const float du = (u1 - u0) / (x1 - x0);
        const float dv = (v1 - v0) / (y1 - y0);
        const bool was_uniform = m_uniform;
        const uint32_t uniform_rgb = m_uniform_rgb;
        m_uniform = false; // whatever path draws this quad

        // Text: glyph quads step a whole number of texels per pixel (1, or 2 across with ImGui's
        // default horizontal oversampling), so the bilinear weights are the same for every pixel
        // of the quad. Work them out once instead of per pixel; same result as sample().
        const float tw = static_cast<float>(tex->Width), th = static_cast<float>(tex->Height);
        const float step_x = std::round(du * tw), step_y = std::round(dv * th);
        if (step_x >= 1.0f && step_x <= 4.0f && step_y >= 1.0f && step_y <= 4.0f && std::fabs(du * tw - step_x) < 1e-3f &&
            std::fabs(dv * th - step_y) < 1e-3f && tex->BytesPerPixel == 4)
        {
            const int kx = static_cast<int>(step_x), ky = static_cast<int>(step_y);
            const float sx = (u0 + (static_cast<float>(fx) + 0.5f - x0) * du) * tw - 0.5f;
            const float sy = (v0 + (static_cast<float>(fy) + 0.5f - y0) * dv) * th - 0.5f;
            const int tx0 = static_cast<int>(std::floor(sx));
            const int ty0 = static_cast<int>(std::floor(sy));
            const int ffx = static_cast<int>((sx - static_cast<float>(tx0)) * 256.0f);
            const int ffy = static_cast<int>((sy - static_cast<float>(ty0)) * 256.0f);
            const int w00 = (256 - ffx) * (256 - ffy), w10 = ffx * (256 - ffy), w01 = (256 - ffx) * ffy, w11 = ffx * ffy;
            const int W = tex->Width, H = tex->Height;
            const unsigned char* px = tex->Pixels;
            for (int y = fy; y <= ly; ++y)
            {
                const int ty = ty0 + (y - fy) * ky;
                const int ya = std::clamp(ty, 0, H - 1), yb = std::clamp(ty + 1, 0, H - 1);
                const unsigned char* rowa = px + static_cast<size_t>(ya) * W * 4;
                const unsigned char* rowb = px + static_cast<size_t>(yb) * W * 4;
                uint32_t* row = &m_pixels[static_cast<size_t>(y) * m_width];
                for (int x = fx; x <= lx; ++x)
                {
                    const int t = tx0 + (x - fx) * kx;
                    const int xa = std::clamp(t, 0, W - 1) * 4, xb = std::clamp(t + 1, 0, W - 1) * 4;
                    const int a = (rowa[xa + 3] * w00 + rowa[xb + 3] * w10 + rowb[xa + 3] * w01 + rowb[xb + 3] * w11) >> 16;
                    if (a == 0)
                    {
                        continue; // most of a glyph box is empty
                    }
                    const int r = (rowa[xa] * w00 + rowa[xb] * w10 + rowb[xa] * w01 + rowb[xb] * w11) >> 16;
                    const int g = (rowa[xa + 1] * w00 + rowa[xb + 1] * w10 + rowb[xa + 1] * w01 + rowb[xb + 1] * w11) >> 16;
                    const int b = (rowa[xa + 2] * w00 + rowa[xb + 2] * w10 + rowb[xa + 2] * w01 + rowb[xb + 2] * w11) >> 16;
                    blend(row[x], mul255(k.r, r), mul255(k.g, g), mul255(k.b, b), mul255(k.a, a));
                }
            }
            ++m_stats.fast_rects;
            return;
        }

        if (tex->BytesPerPixel == 4)
        {
            // Scaled image (skin backgrounds, nine-slice pieces): the bilinear taps and weights of
            // each column are the same on every row, so they are worked out once per quad.
            // Same result as sample(), without the per-pixel float maths.
            const int cols = lx - fx + 1;
            const int rows = ly - fy + 1;
            constexpr int kCacheMinPixels = 4096;
            constexpr size_t kCacheMaxBytes = 48u << 20;
            QuadEntry* fill = nullptr;
            if (cols * rows >= kCacheMinPixels)
            {
                const QuadKey key{tex, x0, y0, x1, y1, u0, v0, u1, v1, col, fx, lx, fy, ly, was_uniform ? (0x1000000u | uniform_rgb) : 0u};
                if (const QuadEntry* hit = find_quad(key))
                {
                    blit_quad(*hit);
                    ++m_stats.cached_quads;
                    return;
                }
                const size_t bytes = static_cast<size_t>(cols) * rows * 4;
                if (m_quad_bytes + bytes <= kCacheMaxBytes)
                {
                    // Over one flat colour the finished pixels are stored (later frames just copy them).
                    m_quads.push_back({key, std::vector<uint32_t>(static_cast<size_t>(cols) * rows), key.under != 0, m_frame_no});
                    m_quad_bytes += bytes;
                    fill = &m_quads.back();
                }
            }
            m_cols.resize(static_cast<size_t>(cols));
            const int W = tex->Width, H = tex->Height;
            for (int i = 0; i < cols; ++i)
            {
                const float u = u0 + (static_cast<float>(fx + i) + 0.5f - x0) * du;
                const float sx = u * tw - 0.5f;
                const int t0 = static_cast<int>(std::floor(sx));
                m_cols[static_cast<size_t>(i)] = {std::clamp(t0, 0, W - 1) * 4, std::clamp(t0 + 1, 0, W - 1) * 4,
                                                  static_cast<int>((sx - static_cast<float>(t0)) * 256.0f)};
            }
            const unsigned char* px = tex->Pixels;
            const bool white = k.r == 255 && k.g == 255 && k.b == 255 && k.a == 255;
            for (int y = fy; y <= ly; ++y)
            {
                const float v = v0 + (static_cast<float>(y) + 0.5f - y0) * dv;
                const float sy = v * th - 0.5f;
                const int t0 = static_cast<int>(std::floor(sy));
                const int wy = static_cast<int>((sy - static_cast<float>(t0)) * 256.0f);
                const unsigned char* rowa = px + static_cast<size_t>(std::clamp(t0, 0, H - 1)) * W * 4;
                const unsigned char* rowb = px + static_cast<size_t>(std::clamp(t0 + 1, 0, H - 1)) * W * 4;
                uint32_t* row = &m_pixels[static_cast<size_t>(y) * m_width];
                uint32_t* keep = fill ? &fill->argb[static_cast<size_t>(y - fy) * cols] : nullptr;
                for (int i = 0; i < cols; ++i)
                {
                    const Column& c = m_cols[static_cast<size_t>(i)];
                    const int w00 = (256 - c.w) * (256 - wy), w10 = c.w * (256 - wy), w01 = (256 - c.w) * wy, w11 = c.w * wy;
                    const unsigned char* p00 = rowa + c.a;
                    const unsigned char* p10 = rowa + c.b;
                    const unsigned char* p01 = rowb + c.a;
                    const unsigned char* p11 = rowb + c.b;
                    int a = (p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11) >> 16;
                    if (a == 0)
                    {
                        if (keep)
                        {
                            keep[i] = fill->key.under ? (0xFF000000u | (row[fx + i] & 0xFFFFFFu)) : 0u;
                        }
                        continue;
                    }
                    int r = (p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11) >> 16;
                    int g = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11) >> 16;
                    int b = (p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11) >> 16;
                    if (!white)
                    {
                        r = mul255(k.r, r), g = mul255(k.g, g), b = mul255(k.b, b), a = mul255(k.a, a);
                    }
                    blend(row[fx + i], r, g, b, a);
                    if (keep)
                    {
                        keep[i] = fill->key.under ? (0xFF000000u | (row[fx + i] & 0xFFFFFFu))
                                                  : (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(mul255(r, a)) << 16) |
                                                        (static_cast<uint32_t>(mul255(g, a)) << 8) | static_cast<uint32_t>(mul255(b, a));
                    }
                }
            }
            ++m_stats.fast_rects;
            return;
        }

        for (int y = fy; y <= ly; ++y)
        {
            uint32_t* row = &m_pixels[static_cast<size_t>(y) * m_width];
            const float v = v0 + (static_cast<float>(y) + 0.5f - y0) * dv;
            for (int x = fx; x <= lx; ++x)
            {
                const float u = u0 + (static_cast<float>(x) + 0.5f - x0) * du;
                const Rgba t = sample(tex, u, v);
                blend(row[x], mul255(k.r, t.r), mul255(k.g, t.g), mul255(k.b, t.b), mul255(k.a, t.a));
            }
        }
        ++m_stats.fast_rects;
    }

    const SoftRenderer::QuadEntry* SoftRenderer::find_quad(const QuadKey& key)
    {
        for (QuadEntry& e : m_quads)
        {
            if (e.key == key)
            {
                e.last_used = m_frame_no;
                return &e;
            }
        }
        return nullptr;
    }

    void SoftRenderer::blit_quad(const QuadEntry& e)
    {
        const int cols = e.key.lx - e.key.fx + 1;
        for (int y = e.key.fy; y <= e.key.ly; ++y)
        {
            const uint32_t* src = &e.argb[static_cast<size_t>(y - e.key.fy) * cols];
            uint32_t* row = &m_pixels[static_cast<size_t>(y) * m_width + e.key.fx];
            if (e.opaque)
            {
                for (int i = 0; i < cols; ++i)
                {
                    row[i] = src[i] & 0xFFFFFFu;
                }
                continue;
            }
            for (int i = 0; i < cols; ++i)
            {
                const uint32_t s = src[i];
                const int a = static_cast<int>(s >> 24);
                if (a == 0)
                {
                    continue;
                }
                if (a == 255)
                {
                    row[i] = s & 0xFFFFFFu;
                    continue;
                }
                // Premultiplied source: same result as blend(), half the multiplies.
                const uint32_t d = row[i];
                const int ia = 255 - a;
                const int nr = static_cast<int>((s >> 16) & 0xFF) + mul255(static_cast<int>((d >> 16) & 0xFF), ia);
                const int ng = static_cast<int>((s >> 8) & 0xFF) + mul255(static_cast<int>((d >> 8) & 0xFF), ia);
                const int nb = static_cast<int>(s & 0xFF) + mul255(static_cast<int>(d & 0xFF), ia);
                row[i] = (static_cast<uint32_t>(std::min(nr, 255)) << 16) | (static_cast<uint32_t>(std::min(ng, 255)) << 8) | static_cast<uint32_t>(std::min(nb, 255));
            }
        }
    }

    void SoftRenderer::triangle(const Clip& c, const ImTextureData* tex, const ImDrawVert& va, const ImDrawVert& vb_in, const ImDrawVert& vc_in)
    {
        const ImDrawVert* vb = &vb_in;
        const ImDrawVert* vc = &vc_in;
        auto edge = [](const ImVec2& a, const ImVec2& b, float px, float py) {
            return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
        };
        float area = edge(va.pos, vb->pos, vc->pos.x, vc->pos.y);
        if (std::fabs(area) < 1e-6f)
        {
            return;
        }
        if (area < 0)
        {
            std::swap(vb, vc);
            area = -area;
        }
        m_uniform = false;
        const ImVec2 a = va.pos, b = vb->pos, d = vc->pos;
        const int fx = std::max(c.x0, static_cast<int>(std::floor(std::min({a.x, b.x, d.x}))));
        const int lx = std::min(c.x1 - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, d.x}))));
        const int fy = std::max(c.y0, static_cast<int>(std::floor(std::min({a.y, b.y, d.y}))));
        const int ly = std::min(c.y1 - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, d.y}))));
        if (fx > lx || fy > ly)
        {
            return;
        }
        ++m_stats.triangles;

        // Top-left rule so shared edges are drawn exactly once (no double-blended seams).
        auto top_left = [](const ImVec2& p, const ImVec2& q) {
            const float dy = q.y - p.y;
            const float dx = q.x - p.x;
            return (dy == 0.0f && dx < 0.0f) || dy > 0.0f;
        };
        const bool tl0 = top_left(b, d), tl1 = top_left(d, a), tl2 = top_left(a, b);

        const Rgba ca = unpack(va.col), cb = unpack(vb->col), cc = unpack(vc->col);
        const bool flat = va.col == vb->col && vb->col == vc->col;
        const bool white = tex == nullptr || (va.uv.x == m_white_uv.x && va.uv.y == m_white_uv.y && vb->uv.x == m_white_uv.x &&
                                              vb->uv.y == m_white_uv.y && vc->uv.x == m_white_uv.x && vc->uv.y == m_white_uv.y);
        const float inv = 1.0f / area;

        // Edge values step linearly across a row.
        const float e0dx = -(d.y - b.y), e1dx = -(a.y - d.y), e2dx = -(b.y - a.y);
        for (int y = fy; y <= ly; ++y)
        {
            const float py = static_cast<float>(y) + 0.5f;
            const float px0 = static_cast<float>(fx) + 0.5f;
            float w0 = edge(b, d, px0, py);
            float w1 = edge(d, a, px0, py);
            float w2 = edge(a, b, px0, py);

            // Only walk the part of the row inside all three edges (fans of long thin
            // triangles would otherwise rescan the whole bounding box).
            float kmin = 0.0f, kmax = static_cast<float>(lx - fx);
            bool empty = false;
            const float ws[3] = {w0, w1, w2};
            const float ss[3] = {e0dx, e1dx, e2dx};
            for (int i = 0; i < 3; ++i)
            {
                if (ss[i] > 0.0f)
                {
                    kmin = std::max(kmin, -ws[i] / ss[i]);
                }
                else if (ss[i] < 0.0f)
                {
                    kmax = std::min(kmax, ws[i] / -ss[i]);
                }
                else if (ws[i] < 0.0f)
                {
                    empty = true;
                }
            }
            if (empty || kmin > kmax + 1.0f)
            {
                continue;
            }
            const int k0 = std::max(0, static_cast<int>(std::floor(kmin)) - 1);
            const int k1 = std::min(lx - fx, static_cast<int>(std::ceil(kmax)) + 1);
            w0 += e0dx * static_cast<float>(k0);
            w1 += e1dx * static_cast<float>(k0);
            w2 += e2dx * static_cast<float>(k0);
            uint32_t* row = &m_pixels[static_cast<size_t>(y) * m_width];
            for (int x = fx + k0; x <= fx + k1; ++x, w0 += e0dx, w1 += e1dx, w2 += e2dx)
            {
                const bool in = (w0 > 0 || (w0 == 0 && tl0)) && (w1 > 0 || (w1 == 0 && tl1)) && (w2 > 0 || (w2 == 0 && tl2));
                if (!in)
                {
                    continue;
                }
                const float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
                int r, g, bl, al;
                if (flat)
                {
                    r = ca.r, g = ca.g, bl = ca.b, al = ca.a;
                }
                else
                {
                    r = static_cast<int>(l0 * ca.r + l1 * cb.r + l2 * cc.r + 0.5f);
                    g = static_cast<int>(l0 * ca.g + l1 * cb.g + l2 * cc.g + 0.5f);
                    bl = static_cast<int>(l0 * ca.b + l1 * cb.b + l2 * cc.b + 0.5f);
                    al = static_cast<int>(l0 * ca.a + l1 * cb.a + l2 * cc.a + 0.5f);
                }
                if (!white)
                {
                    const float u = l0 * va.uv.x + l1 * vb->uv.x + l2 * vc->uv.x;
                    const float v = l0 * va.uv.y + l1 * vb->uv.y + l2 * vc->uv.y;
                    const Rgba t = sample(tex, u, v);
                    r = mul255(r, t.r), g = mul255(g, t.g), bl = mul255(bl, t.b), al = mul255(al, t.a);
                }
                blend(row[x], r, g, bl, al);
            }
        }
    }

    bool SoftRenderer::render(ImDrawData* dd, uint32_t clear_rgb, bool force)
    {
        const bool textures_changed = dd && update_textures(dd);
        if (textures_changed)
        {
            m_quads.clear(); // a texture came or went: cached samples may point at old pixels
            m_quad_bytes = 0;
        }
        const uint64_t frame = dd ? fingerprint(dd, clear_rgb) : 0;
        if (!force && !textures_changed && frame == m_last_frame && frame != 0)
        {
            return false;
        }
        m_last_frame = frame;
        m_stats = {};
        ++m_frame_no;
        std::fill(m_pixels.begin(), m_pixels.end(), clear_rgb & 0xFFFFFF);
        m_uniform = true;
        m_uniform_rgb = clear_rgb & 0xFFFFFF;
        if (!dd)
        {
            return true;
        }
        m_white_uv = ImGui::GetIO().Fonts->TexUvWhitePixel;
        const ImVec2 origin = dd->DisplayPos;

        for (const ImDrawList* list : dd->CmdLists)
        {
            const ImDrawVert* verts = list->VtxBuffer.Data;
            const ImDrawIdx* idx = list->IdxBuffer.Data;
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback)
                {
                    if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                    {
                        cmd.UserCallback(list, &cmd);
                    }
                    continue;
                }
                // Same rounding as the D3D11 backend's scissor rectangle (truncation).
                Clip clip{std::max(0, static_cast<int>(cmd.ClipRect.x - origin.x)), std::max(0, static_cast<int>(cmd.ClipRect.y - origin.y)),
                          std::min(m_width, static_cast<int>(cmd.ClipRect.z - origin.x)), std::min(m_height, static_cast<int>(cmd.ClipRect.w - origin.y))};
                if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1)
                {
                    continue;
                }
                const ImTextureID id = cmd.GetTexID();
                const auto* tex = id == ImTextureID_Invalid ? nullptr : reinterpret_cast<const ImTextureData*>(static_cast<intptr_t>(id));
                if (tex && !tex->Pixels)
                {
                    tex = nullptr;
                }

                const ImDrawVert* v = verts + cmd.VtxOffset;
                const ImDrawIdx* ix = idx + cmd.IdxOffset;
                const unsigned count = cmd.ElemCount;
                for (unsigned e = 0; e + 2 < count;)
                {
                    const ImDrawVert& p0 = v[ix[e]];
                    const ImDrawVert& p1 = v[ix[e + 1]];
                    const ImDrawVert& p2 = v[ix[e + 2]];
                    // ImGui's rectangles: (a,b,c) + (a,c,d) with a/b/c/d the corners in order.
                    if (e + 5 < count && ix[e + 3] == ix[e] && ix[e + 4] == ix[e + 2])
                    {
                        const ImDrawVert& p3 = v[ix[e + 5]];
                        const bool axis = p0.pos.y == p1.pos.y && p1.pos.x == p2.pos.x && p2.pos.y == p3.pos.y && p3.pos.x == p0.pos.x &&
                                          p0.pos.x < p1.pos.x && p0.pos.y < p2.pos.y;
                        const bool one_col = p0.col == p1.col && p1.col == p2.col && p2.col == p3.col;
                        if (axis && one_col)
                        {
                            const bool uv_white = !tex || (p0.uv.x == m_white_uv.x && p0.uv.y == m_white_uv.y && p2.uv.x == m_white_uv.x &&
                                                          p2.uv.y == m_white_uv.y && p1.uv.x == m_white_uv.x && p3.uv.y == m_white_uv.y);
                            const bool uv_box = p0.uv.y == p1.uv.y && p1.uv.x == p2.uv.x && p2.uv.y == p3.uv.y && p3.uv.x == p0.uv.x;
                            if (uv_white)
                            {
                                solid_rect(clip, p0.pos.x, p0.pos.y, p2.pos.x, p2.pos.y, p0.col);
                                e += 6;
                                continue;
                            }
                            if (uv_box)
                            {
                                textured_rect(clip, tex, p0.pos.x, p0.pos.y, p2.pos.x, p2.pos.y, p0.uv.x, p0.uv.y, p2.uv.x, p2.uv.y, p0.col);
                                e += 6;
                                continue;
                            }
                        }
                    }
                    triangle(clip, tex, p0, p1, p2);
                    e += 3;
                }
            }
        }
        // Forget quads that were not drawn this frame (scrolled, resized, skin changed).
        const size_t before = m_quads.size();
        std::erase_if(m_quads, [this](const QuadEntry& q) { return q.last_used != m_frame_no; });
        if (m_quads.size() != before)
        {
            m_quad_bytes = 0;
            for (const QuadEntry& q : m_quads)
            {
                m_quad_bytes += q.argb.size() * 4;
            }
        }
        return true;
    }
} // namespace uuepbs::ui
