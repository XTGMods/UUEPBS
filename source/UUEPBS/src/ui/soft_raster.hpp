// UUEPBS - Dear ImGui renderer that runs entirely on the CPU
//
// Fills ImGui's triangle lists into a 32-bit pixel buffer (0x00RRGGBB, the layout
// of a Windows DIB section). The host copies that buffer to the window with GDI,
// so the slider window uses no Direct3D / DXGI at all and nothing that hooks the
// game's graphics (ReShade, frame generation, overlays) ever sees it.
//
// Portable: no Windows headers, so tests can render and compare it on any OS.
#pragma once

#include <imgui.h>

#include <cstdint>
#include <vector>

namespace uuepbs::ui
{
    class SoftRenderer
    {
      public:
        // Call once after ImGui::CreateContext(): declares texture + vertex offset support.
        static void setup_backend();
        // Releases texture bookkeeping before ImGui::DestroyContext().
        static void shutdown_backend();

        void resize(int width, int height);
        int width() const { return m_width; }
        int height() const { return m_height; }
        const uint32_t* pixels() const { return m_pixels.data(); }
        uint32_t* pixels() { return m_pixels.data(); }

        // Clears to `clear_rgb` (0xRRGGBB) and draws. Handles ImGui texture requests first.
        // Returns false (and leaves the pixels alone) when the frame is identical to the last
        // one drawn, which is most frames while the mouse rests; `force` redraws anyway.
        bool render(ImDrawData* draw_data, uint32_t clear_rgb, bool force = true);

        // Statistics of the last render (for tests / tuning).
        struct Stats
        {
            size_t triangles{};
            size_t fast_rects{};
            size_t cached_quads{}; // large images blended from the quad cache
        };
        const Stats& stats() const { return m_stats; }

      private:
        struct Clip
        {
            int x0, y0, x1, y1;
        };

        bool update_textures(ImDrawData* draw_data);
        static uint64_t fingerprint(const ImDrawData* draw_data, uint32_t clear_rgb);
        void solid_rect(const Clip& c, float x0, float y0, float x1, float y1, ImU32 col);
        void textured_rect(const Clip& c, const ImTextureData* tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                           ImU32 col);
        void triangle(const Clip& c, const ImTextureData* tex, const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& d);

        struct Column
        {
            int a, b, w; // byte offsets of the two texels, weight of the second (0..256)
        };

        // Large scaled images (skin backgrounds and panels) are sampled once and reused while the
        // same quad keeps being drawn; later frames only blend the cached colours.
        struct QuadKey
        {
            const void* tex;
            float x0, y0, x1, y1, u0, v0, u1, v1;
            ImU32 col;
            int fx, lx, fy, ly;
            uint32_t under; // 0x1RRGGBB when drawn over a buffer of one flat colour, else 0
            bool operator==(const QuadKey&) const = default;
        };
        struct QuadEntry
        {
            QuadKey key;
            std::vector<uint32_t> argb; // alpha on top, colour premultiplied (or final colour when opaque)
            bool opaque{};
            uint64_t last_used{};
        };
        const QuadEntry* find_quad(const QuadKey& key);
        void blit_quad(const QuadEntry& e);

        int m_width{};
        int m_height{};
        std::vector<Column> m_cols; // scratch for scaled images
        std::vector<QuadEntry> m_quads;
        size_t m_quad_bytes{};
        uint64_t m_frame_no{};
        bool m_uniform{};       // every pixel still has m_uniform_rgb (just cleared / filled)
        uint32_t m_uniform_rgb{};
        std::vector<uint32_t> m_pixels;
        ImVec2 m_white_uv{};
        Stats m_stats{};
        uint64_t m_last_frame{};
    };
} // namespace uuepbs::ui
