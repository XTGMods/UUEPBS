#include "image_decode.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

// stb_image, restricted to what skins need. Files come from the user's own skin folder.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 4096
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4456 4457 4701 4702 4100 4127 4505)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../third_party/stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uuepbs::ui
{
    bool decode_image_memory(const uint8_t* data, size_t size, Rgba8Image& out, std::string& error)
    {
        out = {};
        if (!data || size < 8 || size > kMaxImageBytes)
        {
            error = size < 8 ? "file is empty" : "file is too big";
            return false;
        }
        int w = 0, h = 0, channels = 0;
        if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &channels))
        {
            error = std::string("not a PNG or JPG image (") + stbi_failure_reason() + ")";
            return false;
        }
        if (w <= 0 || h <= 0 || w > kMaxImageSide || h > kMaxImageSide)
        {
            error = "image is " + std::to_string(w) + "x" + std::to_string(h) + "; the limit is " + std::to_string(kMaxImageSide) + " pixels per side";
            return false;
        }
        unsigned char* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
        if (!px)
        {
            error = std::string("could not decode the image (") + stbi_failure_reason() + ")";
            return false;
        }
        out.width = w;
        out.height = h;
        out.pixels.assign(px, px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(px);
        return true;
    }

    bool decode_image_file(const std::filesystem::path& file, Rgba8Image& out, std::string& error)
    {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(file, ec);
        if (ec)
        {
            error = "file not found";
            return false;
        }
        if (size > kMaxImageBytes)
        {
            error = "file is bigger than 32 MB";
            return false;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            error = "file could not be opened";
            return false;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!in)
        {
            error = "file could not be read";
            return false;
        }
        return decode_image_memory(bytes.data(), bytes.size(), out, error);
    }

    void shrink_to_fit(Rgba8Image& image, int max_side)
    {
        while ((image.width > max_side || image.height > max_side) && image.width > 1 && image.height > 1)
        {
            const int w = image.width / 2, h = image.height / 2;
            std::vector<uint8_t> half(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y)
            {
                const uint8_t* r0 = &image.pixels[(static_cast<size_t>(y) * 2) * image.width * 4];
                const uint8_t* r1 = &image.pixels[(static_cast<size_t>(y) * 2 + 1) * image.width * 4];
                uint8_t* dst = &half[static_cast<size_t>(y) * w * 4];
                for (int x = 0; x < w; ++x)
                {
                    const uint8_t* a = r0 + x * 8;
                    const uint8_t* b = r1 + x * 8;
                    // Alpha-weighted so transparent texels do not darken the colour.
                    const int wa = a[3], wb = a[7], wc = b[3], wd = b[7];
                    const int wsum = wa + wb + wc + wd;
                    for (int c = 0; c < 3; ++c)
                    {
                        dst[x * 4 + c] = static_cast<uint8_t>(wsum ? (a[c] * wa + a[4 + c] * wb + b[c] * wc + b[4 + c] * wd + wsum / 2) / wsum
                                                                   : (a[c] + a[4 + c] + b[c] + b[4 + c] + 2) / 4);
                    }
                    dst[x * 4 + 3] = static_cast<uint8_t>((wsum + 2) / 4);
                }
            }
            image.pixels = std::move(half);
            image.width = w;
            image.height = h;
        }
    }

    void bleed_transparent_edges(Rgba8Image& image, int passes)
    {
        const int w = image.width, h = image.height;
        if (w <= 0 || h <= 0)
        {
            return;
        }
        std::vector<uint8_t> known(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < known.size(); ++i)
        {
            known[i] = image.pixels[i * 4 + 3] > 0 ? 1 : 0;
        }
        for (int pass = 0; pass < passes; ++pass)
        {
            std::vector<uint8_t> next = known;
            bool any = false;
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (known[i])
                    {
                        continue;
                    }
                    int sum[3] = {0, 0, 0}, n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int nx = x + dx, ny = y + dy;
                            if ((dx || dy) && nx >= 0 && ny >= 0 && nx < w && ny < h && known[static_cast<size_t>(ny) * w + nx])
                            {
                                const uint8_t* p = &image.pixels[(static_cast<size_t>(ny) * w + nx) * 4];
                                sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
                                ++n;
                            }
                        }
                    }
                    if (n)
                    {
                        uint8_t* p = &image.pixels[i * 4];
                        p[0] = static_cast<uint8_t>(sum[0] / n), p[1] = static_cast<uint8_t>(sum[1] / n), p[2] = static_cast<uint8_t>(sum[2] / n);
                        next[i] = 1; // alpha stays 0
                        any = true;
                    }
                }
            }
            known = std::move(next);
            if (!any)
            {
                break;
            }
        }
    }
} // namespace uuepbs::ui
