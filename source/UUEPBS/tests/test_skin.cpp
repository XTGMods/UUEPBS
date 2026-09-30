// Window skin parsing and image decoding (no ImGui context needed).
//   test_skin <skins folder with "Example - Midnight Rose">
#include "ui/image_decode.hpp"
#include "ui/skin.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace uuepbs::ui;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static bool near(const ImVec4& c, float r, float g, float b, float a)
{
    return std::fabs(c.x - r) < 0.003f && std::fabs(c.y - g) < 0.003f && std::fabs(c.z - b) < 0.003f && std::fabs(c.w - a) < 0.003f;
}

int main(int argc, char** argv)
{
    ImVec4 c;
    CHECK(parse_color("#E870A8", c) && near(c, 232 / 255.f, 112 / 255.f, 168 / 255.f, 1));
    CHECK(parse_color("#E870A880", c) && near(c, 232 / 255.f, 112 / 255.f, 168 / 255.f, 128 / 255.f));
    CHECK(parse_color("#fff", c) && near(c, 1, 1, 1, 1));
    CHECK(parse_color(" #0008 ", c) && near(c, 0, 0, 0, 136 / 255.f));
    CHECK(!parse_color("E870A8", c) && !parse_color("#12345", c) && !parse_color("#GG0000", c) && !parse_color("=accent", c));

    SkinSpec spec;
    std::string msg;
    std::vector<std::string> warnings;
    const char* json = R"({
      "format": "UUEPBS skin", "version": 1, "name": "T", "author": "me",
      "colors": { "accent": "#FF0000", "window": [10, 20, 30], "panel": [0.5, 0.5, 0.5, 0.25], "bogus": "#000", "text": "red" },
      "imgui": { "ResizeGrip": "#00000000" },
      "style": { "frame_rounding": 3, "frame_padding": [4, 2], "font_scale": 1.2, "nope": 1 },
      "images": {
        "background": "bg.jpg",
        "button": { "file": "b.png", "slice": 6 },
        "panel": { "file": "p.png", "mode": "tile", "scale": 2 },
        "tab": { "file": "../../escape.png" },
        "logo": { "file": "C:/Windows/logo.png" },
        "slider_track": { "file": "t.png", "mode": "spiral" },
        "unknown_widget": "x.png"
      },
      "icons": { "Reset": "icons/reset.png", "group.Thighs": { "file": "t.png", "tint": "#FFFFFF80" } }
    })";
    CHECK(parse_skin(json, spec, msg, &warnings));
    CHECK(spec.name == "T" && spec.author == "me");
    CHECK(near(spec.colors["accent"], 1, 0, 0, 1) && near(spec.colors["window"], 10 / 255.f, 20 / 255.f, 30 / 255.f, 1) &&
          near(spec.colors["panel"], 0.5f, 0.5f, 0.5f, 0.25f));
    CHECK(!spec.colors.count("bogus") && !spec.colors.count("text"));
    CHECK(spec.imgui_colors.count("resizegrip") == 1);
    CHECK(spec.style["frame_padding"].x == 4 && spec.style["frame_padding"].y == 2 && spec.style["frame_rounding"].x == 3 && !spec.style.count("nope"));
    CHECK(spec.images.count("background") && spec.images["background"].mode == ImageMode::Cover);   // default mode per key
    CHECK(spec.images["button"].mode == ImageMode::Nine && spec.images["button"].slice[2] == 6);    // "slice" implies nine
    CHECK(spec.images["panel"].mode == ImageMode::Tile && spec.images["panel"].scale == 2);
    CHECK(!spec.images.count("tab") && !spec.images.count("logo"));                                  // files outside the skin folder refused
    CHECK(!spec.images.count("slider_track") && !spec.images.count("unknown_widget"));
    CHECK(spec.images.count("icon.reset") && spec.images["icon.reset"].mode == ImageMode::Contain);   // icon keys are lower case
    CHECK(spec.images.count("icon.group.thighs") && near(spec.images["icon.group.thighs"].tint, 1, 1, 1, 128 / 255.f));
    std::printf("%zu warnings, e.g. %s\n", warnings.size(), warnings.empty() ? "" : warnings.front().c_str());
    CHECK(warnings.size() >= 7);
    CHECK(!parse_skin("{ \"colors\": { \"accent\": \"#FFF\" ", spec, msg) && msg.find("line") != std::string::npos);

    // image decoding: the bundled example's files, then broken input
    if (argc > 1)
    {
        const std::filesystem::path base = std::filesystem::path(argv[1]) / "Example - Midnight Rose";
        Rgba8Image img;
        std::string err;
        CHECK(decode_image_file(base / "background.jpg", img, err) && img.width == 640 && img.height == 800 && img.pixels.size() == 640u * 800u * 4u);
        CHECK(decode_image_file(base / "grab.png", img, err) && img.width == 40);
        const uint8_t corner_alpha = img.pixels[3];
        bleed_transparent_edges(img);
        CHECK(img.pixels[3] == corner_alpha); // alpha is never changed
        std::ifstream in(base / "skin.json");
        std::stringstream ss;
        ss << in.rdbuf();
        std::vector<std::string> w2;
        CHECK(parse_skin(ss.str(), spec, msg, &w2) && w2.empty() && spec.images.size() >= 30);
        for (const std::string& w : w2)
            std::printf("  example skin warning: %s\n", w.c_str());
    }
    Rgba8Image img;
    std::string err;
    const uint8_t junk[64] = {1, 2, 3};
    CHECK(!decode_image_memory(junk, sizeof(junk), img, err) && !err.empty());
    CHECK(!decode_image_file("/nonexistent/x.png", img, err));

    img.width = 5000;
    img.height = 3000;
    img.pixels.assign(size_t(img.width) * img.height * 4, 200);
    shrink_to_fit(img, kMaxTextureSide);
    CHECK(img.width == 1250 && img.height == 750 && img.pixels[0] == 200);

    std::printf(fails ? "SKIN TESTS FAILED (%d)\n" : "skin tests passed\n", fails);
    return fails;
}
