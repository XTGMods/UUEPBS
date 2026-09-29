// Headless run of the slider window contents with a tiny software rasterizer,
// so the layout can be checked (and asserts caught) without Windows.
#include "core/body_groups.hpp"
#include "core/mirror.hpp"
#include "core/presets.hpp"
#include "core/registry.hpp"
#include "ui/panel_view.hpp"
#include "ui/soft_raster.hpp"

#include <chrono>

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

using namespace uuepbs;

struct FakeHost : ui::PanelHost
{
    bool hook_active() override { return true; }
    std::string hook_text() override { return "pose hook on vtable 0x77ED320 slot 374 (+0xBB0), buffers +0x600, read index +0x648"; }
    void open_folder(const std::filesystem::path&) override {}
    void set_topmost(bool) override {}
};

struct FakeTargets : ui::TargetSource
{
    std::vector<ui::TargetChoice> targets() override { return {{"1A2B3C", "BP_Rokuv3_C_0 (player)"}, {"4D5E6F", "BP_NPC_Guard_C_2"}}; }
    ui::TargetChoice current_target() override { return {"1A2B3C", "BP_Rokuv3_C_0 (player)"}; }
    void pick_target(const std::string&) override {}
    void refresh_targets() override {}
    void request_rescan() override {}
    std::string link_text() override { return "connected, script v2.0.0, 2 rigs"; }
    std::string hotkey_text() override { return "F6"; }
};

struct Canvas
{
    int w, h;
    std::vector<float> px; // rgb
    Canvas(int W, int H, ImVec4 bg) : w(W), h(H), px(size_t(W) * H * 3)
    {
        for (int i = 0; i < W * H; ++i)
        {
            px[i * 3 + 0] = bg.x;
            px[i * 3 + 1] = bg.y;
            px[i * 3 + 2] = bg.z;
        }
    }
};

static void sample(const ImTextureData* t, float u, float v, float out[4])
{
    const float x = u * t->Width - 0.5f, y = v * t->Height - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    float acc[4] = {0, 0, 0, 0};
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            const int xi = std::min(std::max(x0 + dx, 0), t->Width - 1), yi = std::min(std::max(y0 + dy, 0), t->Height - 1);
            const float wgt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
            const unsigned char* p = t->Pixels + (size_t(yi) * t->Width + xi) * t->BytesPerPixel;
            if (t->BytesPerPixel == 4)
                for (int c = 0; c < 4; ++c)
                    acc[c] += wgt * p[c] / 255.0f;
            else
            {
                acc[0] += wgt;
                acc[1] += wgt;
                acc[2] += wgt;
                acc[3] += wgt * p[0] / 255.0f;
            }
        }
    std::memcpy(out, acc, sizeof(acc));
}

static void raster(Canvas& cv, ImDrawData* dd)
{
    for (const ImDrawList* dl : dd->CmdLists)
    {
        for (const ImDrawCmd& cmd : dl->CmdBuffer)
        {
            if (cmd.UserCallback)
                continue;
            const auto* tex = reinterpret_cast<const ImTextureData*>(cmd.GetTexID());
            const int cx0 = std::max(0, (int)(cmd.ClipRect.x - dd->DisplayPos.x)), cy0 = std::max(0, (int)(cmd.ClipRect.y - dd->DisplayPos.y));
            const int cx1 = std::min(cv.w, (int)(cmd.ClipRect.z - dd->DisplayPos.x)), cy1 = std::min(cv.h, (int)(cmd.ClipRect.w - dd->DisplayPos.y));
            for (unsigned e = 0; e < cmd.ElemCount; e += 3)
            {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k)
                    v[k] = &dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + e + k]];
                const float ax = v[0]->pos.x, ay = v[0]->pos.y, bx = v[1]->pos.x, by = v[1]->pos.y, qx = v[2]->pos.x, qy = v[2]->pos.y;
                const float area = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
                if (std::fabs(area) < 1e-8f)
                    continue;
                const int x0 = std::max(cx0, (int)std::floor(std::min({ax, bx, qx}))), x1 = std::min(cx1, (int)std::ceil(std::max({ax, bx, qx})));
                const int y0 = std::max(cy0, (int)std::floor(std::min({ay, by, qy}))), y1 = std::min(cy1, (int)std::ceil(std::max({ay, by, qy})));
                float col[3][4];
                for (int k = 0; k < 3; ++k)
                {
                    const ImU32 c = v[k]->col;
                    col[k][0] = ((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
                    col[k][1] = ((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
                    col[k][2] = ((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
                    col[k][3] = ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
                }
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                    {
                        const float px = x + 0.5f, py = y + 0.5f;
                        const float w0 = ((bx - px) * (qy - py) - (by - py) * (qx - px)) / area;
                        const float w1 = ((qx - px) * (ay - py) - (qy - py) * (ax - px)) / area;
                        const float w2 = 1 - w0 - w1;
                        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
                            continue;
                        const float u = w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x;
                        const float tv = w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y;
                        float t[4] = {1, 1, 1, 1};
                        if (tex && tex->Pixels)
                            sample(tex, u, tv, t);
                        float s[4];
                        for (int c = 0; c < 4; ++c)
                            s[c] = (w0 * col[0][c] + w1 * col[1][c] + w2 * col[2][c]) * t[c];
                        float* d = &cv.px[(size_t(y) * cv.w + x) * 3];
                        for (int c = 0; c < 3; ++c)
                            d[c] = s[c] * s[3] + d[c] * (1 - s[3]);
                    }
            }
        }
    }
}

static void save_ppm(const Canvas& cv, const char* path)
{
    FILE* f = std::fopen(path, "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", cv.w, cv.h);
    for (float p : cv.px)
    {
        const int b = (int)std::lround(std::pow(std::min(1.0f, std::max(0.0f, p)), 1.0f) * 255.0f);
        std::fputc(b, f);
    }
    std::fclose(f);
}

static void handle_textures(ImDrawData* dd)
{
    if (!dd->Textures)
        return;
    for (ImTextureData* t : *dd->Textures)
    {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates)
        {
            t->SetTexID((ImTextureID)(intptr_t)t);
            t->SetStatus(ImTextureStatus_OK);
        }
        else if (t->Status == ImTextureStatus_WantDestroy)
        {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

int main(int argc, char** argv)
{
    const float dpi = argc > 1 ? std::strtof(argv[1], nullptr) : 1.25f;

    // skeleton + fake component
    std::vector<std::string> names;
    std::vector<int32_t> parents;
    std::vector<Xform> refpose;
    {
        std::ifstream f("roku_ref.txt");
        std::string line;
        while (std::getline(f, line))
        {
            std::istringstream in(line);
            std::string n;
            int p;
            Xform x = xf::identity();
            in >> n >> p >> x.rot[0] >> x.rot[1] >> x.rot[2] >> x.rot[3] >> x.pos[0] >> x.pos[1] >> x.pos[2] >> x.scl[0] >> x.scl[1] >> x.scl[2];
            names.push_back(n);
            parents.push_back(p);
            refpose.push_back(x);
        }
    }
    alignas(16) static unsigned char comp[0x1000] = {};
    comp[0] = 0x11; // fake vtable pointer
    // Optional: preview another game's skeleton from a bridge_in.txt (argv[2]).
    if (argc > 2)
    {
        std::ifstream bf(argv[2]);
        std::string line;
        bool primary = false;
        while (std::getline(bf, line))
        {
            std::istringstream in(line);
            std::string kind, item;
            std::getline(in, kind, '\t');
            if (kind == "rig")
            {
                std::string a, b, p;
                std::getline(in, a, '\t');
                std::getline(in, b, '\t');
                std::getline(in, p, '\t');
                primary = p == "1";
            }
            else if (primary && kind == "bones")
            {
                names.clear();
                while (std::getline(in, item, '\t'))
                    names.push_back(item);
            }
            else if (primary && kind == "parents")
            {
                parents.clear();
                while (std::getline(in, item, '\t'))
                    parents.push_back(std::stoi(item));
                break;
            }
        }
        refpose.assign(names.size(), xf::identity());
    }
    const int nb = (int)names.size();
    static std::vector<Xform> pose;
    pose.assign(names.size(), xf::identity());
    RawArray arr{pose.data(), nb, nb};
    std::memcpy(comp + 0x600, &arr, sizeof(arr));
    Registry& reg = Registry::instance();
    reg.set_layout(PoseLayout{0x600, 0x648, 16, 96});
    std::string msg;
    reg.track((uintptr_t)comp, "CharacterMesh0", "BP_Rokuv3_C_0", names, parents, true, msg);
    reg.set_reference_pose((uintptr_t)comp, refpose);
    alignas(16) static unsigned char comp2[0x1000] = {};
    comp2[0] = 0x22;
    comp2[0xC] = 5;
    reg.track((uintptr_t)comp2, "C_shirt1", "BP_Rokuv3_C_0", names, parents, false, msg);

    auto set = [&](const char* b, double x, double y, double z, Spread s) {
        BoneEdit bs;
        bs.axis[0] = x;
        bs.axis[1] = y;
        bs.axis[2] = z;
        bs.spread = s;
        reg.set_bone(b, bs);
    };
    set("boob_l", 1.3, 1.3, 1.3, Spread::Chain);
    set("boob_r", 1.3, 1.3, 1.3, Spread::Chain);
    set("thigh_l", 1.0, 1.15, 1.15, Spread::Keep);
    set("thigh_r", 1.0, 1.15, 1.15, Spread::Keep);
    set("butt_l", 1.2, 1.2, 1.2, Spread::Chain);
    set("butt_r", 1.2, 1.2, 1.2, Spread::Chain);
    set("spine_01", 1.0, 0.9, 0.9, Spread::Keep);
    {
        EditBook b = reg.edits();
        b["boob_l"].edit.turn[2] = 12.5;
        b["boob_l"].edit.shift[1] = -1.25;
        b["boob_r"].edit = reg.mirror_edit("boob_l", b["boob_l"].edit);
        reg.replace_edits(b);
    }

    reg.on_pose_finalized(comp);

    PresetShelf shelf;
    shelf.set_folder("/tmp/claude-uuepbs-preview/UUEPBS Presets");
    shelf.save("Curvy", reg.edits(), msg);
    shelf.save("Petite", EditBook{}, msg);
    shelf.save("Tall legs", reg.edits(), msg);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(std::round(720 * dpi), std::round(900 * dpi));
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", 19.0f);
    ui::PanelView::apply_theme(dpi);

    FakeHost host;
    FakeTargets targets;
    ui::PanelView view;
    view.configure(&shelf, &host, &targets, "v2.1.0", true);
    view.refresh_presets();
    view.set_message("Loaded 'Curvy' (7 bone(s))");

    const struct
    {
        ui::PanelView::Tab tab;
        const char* file;
    } shots[] = {{ui::PanelView::Tab::Body, "preview_body.ppm"},
                 {ui::PanelView::Tab::Bones, "preview_bones.ppm"},
                 {ui::PanelView::Tab::Presets, "preview_presets.ppm"},
                 {ui::PanelView::Tab::Status, "preview_status.ppm"}};

    ui::SoftRenderer soft;
    int failures = 0;
    for (const auto& shot : shots)
    {
        view.focus_tab(shot.tab);
        if (shot.tab == ui::PanelView::Tab::Bones)
            view.select_bone("boob_l");
        for (int frame = 0; frame < 4; ++frame)
        {
            ImGui::NewFrame();
            view.draw();
            ImGui::Render();
            handle_textures(ImGui::GetDrawData());
        }
        Canvas cv((int)io.DisplaySize.x, (int)io.DisplaySize.y, ImVec4(0.085f, 0.085f, 0.105f, 1));
        raster(cv, ImGui::GetDrawData());
        save_ppm(cv, shot.file);
        std::printf("wrote %s\n", shot.file);

        // Same frame through the shipping CPU renderer: compare with the reference raster and time it.
        soft.resize(cv.w, cv.h);
        const uint32_t bg = (22u << 16) | (22u << 8) | 27u; // 0.085/0.085/0.105
        const auto t0 = std::chrono::steady_clock::now();
        const int runs = 50;
        for (int r = 0; r < runs; ++r)
            soft.render(ImGui::GetDrawData(), bg, true); // force: time real draws
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / runs;
        const auto t1 = std::chrono::steady_clock::now();
        for (int r = 0; r < runs; ++r)
            soft.render(ImGui::GetDrawData(), bg, false); // unchanged: fingerprint only
        const double skip_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / runs;
        double sum = 0; int worst = 0; size_t off = 0;
        std::string soft_name = std::string("soft_") + shot.file;
        FILE* f = std::fopen(soft_name.c_str(), "wb");
        std::fprintf(f, "P6\n%d %d\n255\n", cv.w, cv.h);
        for (int i = 0; i < cv.w * cv.h; ++i)
        {
            const uint32_t p = soft.pixels()[i];
            const int s[3] = {int((p >> 16) & 255), int((p >> 8) & 255), int(p & 255)};
            for (int ch = 0; ch < 3; ++ch)
            {
                const int ref = (int)std::lround(std::min(1.0f, std::max(0.0f, cv.px[i * 3 + ch])) * 255.0f);
                const int d = std::abs(ref - s[ch]);
                sum += d; worst = std::max(worst, d); off += d > 48;
                std::fputc(s[ch], f);
            }
        }
        std::fclose(f);
        const double mean = sum / (cv.w * cv.h * 3.0);
        if (soft.render(ImGui::GetDrawData(), bg, false))
        {
            std::printf("  FAIL: an unchanged frame was drawn again\n");
            failures++;
        }
        std::printf("  cpu renderer: %.2f ms/frame drawn, %.3f ms skipped (%zu fast rects, %zu triangles), mean diff %.3f, pixels off by >48: %zu\n", ms,
                    skip_ms, soft.stats().fast_rects, soft.stats().triangles, mean, off);
        if (mean > 1.0 || off > size_t(cv.w * cv.h) / 500)
        {
            std::printf("  FAIL: CPU renderer differs from the reference\n");
            failures++;
        }
    }
    ImGui::DestroyContext();
    return failures;
}
