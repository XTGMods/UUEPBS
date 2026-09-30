// UUEPBS - PNG / JPG decoding for window skins (stb_image, PNG and JPEG only).
//
// Portable (no Windows headers) so the host tests can load skins too.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace uuepbs::ui
{
    struct Rgba8Image
    {
        int width{};
        int height{};
        std::vector<uint8_t> pixels; // width * height * 4, straight (not premultiplied) alpha
    };

    constexpr int kMaxImageSide = 4096;       // larger files are refused
    constexpr int kMaxTextureSide = 2048;     // bigger images are halved until they fit
    constexpr uintmax_t kMaxImageBytes = 32u << 20;

    // Decodes a PNG or JPEG file. false + a readable reason on failure.
    bool decode_image_file(const std::filesystem::path& file, Rgba8Image& out, std::string& error);
    bool decode_image_memory(const uint8_t* data, size_t size, Rgba8Image& out, std::string& error);

    // Halves the image (2x2 box filter) until both sides are <= max_side.
    void shrink_to_fit(Rgba8Image& image, int max_side);

    // Gives fully transparent pixels the colour of their visible neighbours, so bilinear
    // filtering does not pull dark fringes into the edges of icons and nine-slice frames.
    void bleed_transparent_edges(Rgba8Image& image, int passes = 2);
} // namespace uuepbs::ui
