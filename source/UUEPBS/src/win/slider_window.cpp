#include "slider_window.hpp"

#include "../core/presets.hpp"
#include "../core/registry.hpp"
#include "../ui/panel_view.hpp"
#include "../ui/skin.hpp"
#include "../ui/soft_raster.hpp"
#include "pose_hook.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace uuepbs::ui
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr wchar_t kWindowClass[] = L"UUEPBS.Panel";
        constexpr int kWidth = 720;
        constexpr int kHeight = 900;

        struct Gpu
        {
            ID3D11Device* device{};
            ID3D11DeviceContext* context{};
            IDXGISwapChain* chain{};
            ID3D11RenderTargetView* target{};
        };

        template <typename T> void release(T*& p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        // Main game window (UnrealWindow) of this process, used to place the panel.
        HWND find_game_window()
        {
            struct Search
            {
                DWORD pid;
                HWND found;
            } search{GetCurrentProcessId(), nullptr};
            EnumWindows(
                [](HWND hwnd, LPARAM lp) -> BOOL {
                    auto* s = reinterpret_cast<Search*>(lp);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    if (pid != s->pid || !IsWindowVisible(hwnd))
                    {
                        return TRUE;
                    }
                    wchar_t cls[64]{};
                    GetClassNameW(hwnd, cls, 64);
                    if (lstrcmpW(cls, L"UnrealWindow") == 0)
                    {
                        s->found = hwnd;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&search));
            return search.found;
        }

        std::string s_renderer_text = "starting";

        std::mutex s_skins_lock;
        std::filesystem::path s_skins_folder; // for list_skins() from other threads

        class Panel final : public PanelHost
        {
          public:
            // PanelHost
            std::string renderer_text() override { return s_renderer_text; }
            bool hook_active() override { return hook::active(); }
            std::string hook_text() override { return hook::describe(); }
            void open_folder(const std::filesystem::path& folder) override
            {
                ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            void set_topmost(bool on) override
            {
                m_options.topmost = on;
                SetWindowPos(m_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }

            bool launch(const PresetShelf* shelf, const Options& options)
            {
                if (m_thread)
                {
                    return true;
                }
                m_shelf = shelf;
                m_options = options;
                {
                    std::lock_guard guard(s_skins_lock);
                    s_skins_folder = options.skins_folder;
                }
                m_view.configure(shelf, this, options.targets, options.version, options.topmost);
                m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                m_thread = CreateThread(nullptr, 0, &Panel::entry, this, 0, &m_thread_id);
                return m_thread != nullptr;
            }

            void shutdown()
            {
                if (!m_thread)
                {
                    return;
                }
                m_quit.store(true);
                SetEvent(m_wake);
                if (m_hwnd)
                {
                    PostMessageW(m_hwnd, WM_NULL, 0, 0);
                }
                WaitForSingleObject(m_thread, 3000);
                CloseHandle(m_thread);
                CloseHandle(m_wake);
                m_thread = nullptr;
                m_wake = nullptr;
            }

            void want(int visibility) // 0 hide, 1 show, 2 toggle
            {
                m_request.store(visibility);
                if (m_wake)
                {
                    SetEvent(m_wake);
                }
            }

            bool is_shown() const { return m_shown.load(); }

            void set_message(const std::string& text)
            {
                m_view.set_message(text);
                wake();
            }

            void wake()
            {
                if (m_wake)
                {
                    SetEvent(m_wake); // lets a hidden window notice new edits (autosave) and skin requests
                }
            }

            LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
                {
                    return 1;
                }
                if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_MOUSELEAVE ||
                    msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
                {
                    m_last_input = Clock::now();
                }
                switch (msg)
                {
                case WM_ERASEBKGND:
                    if (m_cpu)
                    {
                        return 1; // the whole client area is painted from the pixel buffer
                    }
                    break;
                case WM_PAINT:
                    if (m_cpu)
                    {
                        PAINTSTRUCT ps;
                        HDC dc = BeginPaint(hwnd, &ps);
                        blit(dc);
                        EndPaint(hwnd, &ps);
                        return 0;
                    }
                    break;
                case WM_SIZE:
                    if (wp != SIZE_MINIMIZED)
                    {
                        m_resize_w = LOWORD(lp);
                        m_resize_h = HIWORD(lp);
                    }
                    return 0;
                case WM_GETMINMAXINFO: {
                    auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                    mm->ptMinTrackSize.x = static_cast<LONG>(560 * m_dpi);
                    mm->ptMinTrackSize.y = static_cast<LONG>(500 * m_dpi);
                    return 0;
                }
                case WM_CLOSE:
                    hide();
                    return 0;
                case WM_SYSCOMMAND:
                    if ((wp & 0xfff0) == SC_KEYMENU)
                    {
                        return 0;
                    }
                    break;
                case WM_DPICHANGED: {
                    const RECT* r = reinterpret_cast<RECT*>(lp);
                    SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                    m_restyle = true;
                    return 0;
                }
                default:
                    break;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

          private:
            static DWORD WINAPI entry(void* self)
            {
                static_cast<Panel*>(self)->run();
                return 0;
            }

            static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                if (msg == WM_NCCREATE)
                {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                }
                auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                return panel ? panel->handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
            }

            // ------------------------------------------------------------------ setup
            bool create_window()
            {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(wc);
                wc.style = CS_CLASSDC;
                wc.lpfnWndProc = &Panel::wndproc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
                wc.lpszClassName = kWindowClass;
                RegisterClassExW(&wc);

                const std::wstring title = L"UUEPBS - Universal Unreal Engine Player Body Sliders";
                m_hwnd = CreateWindowExW(m_options.topmost ? WS_EX_TOPMOST : 0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                         CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr, wc.hInstance, this);
                if (!m_hwnd)
                {
                    return false;
                }
                const BOOL dark = TRUE;
                DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                m_dpi = std::max(1.0f, ImGui_ImplWin32_GetDpiScaleForHwnd(m_hwnd)) * m_options.extra_scale;
                SetWindowPos(m_hwnd, nullptr, 0, 0, static_cast<int>(kWidth * m_dpi), static_cast<int>(kHeight * m_dpi),
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                return true;
            }

            bool create_device()
            {
                DXGI_SWAP_CHAIN_DESC sd{};
                sd.BufferCount = 2;
                sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.BufferDesc.RefreshRate.Numerator = 60;
                sd.BufferDesc.RefreshRate.Denominator = 1;
                sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.OutputWindow = m_hwnd;
                sd.SampleDesc.Count = 1;
                sd.Windowed = TRUE;
                sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

                const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
                D3D_FEATURE_LEVEL got{};
                HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                                           &m_gpu.chain, &m_gpu.device, &got, &m_gpu.context);
                if (hr == DXGI_ERROR_UNSUPPORTED)
                {
                    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &m_gpu.chain,
                                                       &m_gpu.device, &got, &m_gpu.context);
                }
                if (FAILED(hr))
                {
                    return false;
                }
                make_target();
                return true;
            }

            void make_target()
            {
                ID3D11Texture2D* back = nullptr;
                if (SUCCEEDED(m_gpu.chain->GetBuffer(0, IID_PPV_ARGS(&back))) && back)
                {
                    m_gpu.device->CreateRenderTargetView(back, nullptr, &m_gpu.target);
                    back->Release();
                }
            }

            void destroy_device()
            {
                release(m_gpu.target);
                release(m_gpu.chain);
                release(m_gpu.context);
                release(m_gpu.device);
            }

            void apply_style() { PanelView::apply_theme(m_dpi); }

            void load_fonts()
            {
                ImGuiIO& io = ImGui::GetIO();
                wchar_t windir[MAX_PATH]{};
                GetWindowsDirectoryW(windir, MAX_PATH);
                const std::wstring path = std::wstring(windir) + L"\\Fonts\\segoeui.ttf";
                if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    std::string utf8(static_cast<size_t>(n), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), n, nullptr, nullptr);
                    utf8.resize(std::strlen(utf8.c_str()));
                    io.Fonts->AddFontFromFileTTF(utf8.c_str(), m_options.font_size);
                }
                else
                {
                    ImFontConfig cfg;
                    cfg.SizePixels = m_options.font_size;
                    io.Fonts->AddFontDefault(&cfg);
                }
            }

            // ------------------------------------------------------------------ loop
            void run()
            {
                // ShellExecute ("Open folder") wants COM on the calling thread.
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                run_window();
                if (SUCCEEDED(com))
                {
                    CoUninitialize();
                }
            }

            void run_window()
            {
                if (!create_window())
                {
                    return;
                }
                m_cpu = m_options.cpu_renderer;
                if (!m_cpu && !create_device())
                {
                    destroy_device(); // no usable GPU path: fall back to the CPU renderer
                    m_cpu = true;
                    m_gpu_failed = true;
                }
                if (m_cpu)
                {
                    RECT rc{};
                    GetClientRect(m_hwnd, &rc);
                    m_soft.resize(rc.right - rc.left, rc.bottom - rc.top);
                }

                IMGUI_CHECKVERSION();
                m_imgui = ImGui::CreateContext();
                ImGui::SetCurrentContext(m_imgui);
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.LogFilename = nullptr;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                load_fonts();
                skin().set_folder(m_options.skins_folder, m_options.skin); // loaded by the first skin().update()
                m_skin_dirty = skin().update();
                apply_style();
                ImGui_ImplWin32_Init(m_hwnd);
                if (m_cpu)
                {
                    SoftRenderer::setup_backend();
                }
                else
                {
                    ImGui_ImplDX11_Init(m_gpu.device, m_gpu.context);
                }
                s_renderer_text = m_cpu ? (m_gpu_failed ? "CPU (GPU window could not be created)" : "CPU (no Direct3D in use)") : "GPU (Direct3D 11)";

                m_saved_revision = saved_key();

                while (!m_quit.load())
                {
                    handle_requests();

                    if (!m_shown.load())
                    {
                        const DWORD timeout = m_dirty_since ? 250 : INFINITE;
                        MsgWaitForMultipleObjects(1, &m_wake, FALSE, timeout, QS_ALLINPUT);
                        pump();
                        autosave();
                        m_skin_dirty = skin().update() || m_skin_dirty; // "ubs skin" while the window is closed
                        continue;
                    }

                    pump();
                    if (m_quit.load())
                    {
                        break;
                    }
                    const auto start = Clock::now();
                    if (m_cpu)
                    {
                        cpu_frame();
                    }
                    else
                    {
                        gpu_frame();
                    }
                    autosave();
                    pace(start);
                }

                autosave(true);
                if (m_cpu)
                {
                    SoftRenderer::shutdown_backend();
                }
                else
                {
                    ImGui_ImplDX11_Shutdown();
                }
                ImGui_ImplWin32_Shutdown();
                skin().shutdown(); // after the backend released the textures, before the context goes
                ImGui::DestroyContext(m_imgui);
                m_imgui = nullptr;
                destroy_device();
                DestroyWindow(m_hwnd);
                m_hwnd = nullptr;
                UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
            }

            void build_ui()
            {
                const std::string before = skin().status();
                m_skin_dirty = skin().update() || m_skin_dirty;
                if (m_skin_dirty && skin().status() != before)
                {
                    m_view.set_message("Window skin: " + skin().status());
                }
                if (m_restyle || m_skin_dirty)
                {
                    if (m_restyle)
                    {
                        m_dpi = std::max(1.0f, ImGui_ImplWin32_GetDpiScaleForHwnd(m_hwnd)) * m_options.extra_scale;
                    }
                    apply_style();
                    m_restyle = false;
                    m_skin_dirty = false;
                }
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                m_view.draw();
                ImGui::Render();
            }

            void gpu_frame()
            {
                if (m_occluded && m_gpu.chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
                {
                    Sleep(30);
                    return;
                }
                m_occluded = false;
                if (m_resize_w && m_resize_h)
                {
                    release(m_gpu.target);
                    m_gpu.chain->ResizeBuffers(0, m_resize_w, m_resize_h, DXGI_FORMAT_UNKNOWN, 0);
                    m_resize_w = m_resize_h = 0;
                    make_target();
                }
                ImGui_ImplDX11_NewFrame();
                build_ui();
                const ImVec4 bg = skin().color(Role::Window);
                const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
                m_gpu.context->OMSetRenderTargets(1, &m_gpu.target, nullptr);
                m_gpu.context->ClearRenderTargetView(m_gpu.target, clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                const HRESULT hr = m_gpu.chain->Present(1, 0);
                m_occluded = hr == DXGI_STATUS_OCCLUDED;
            }

            // Both renderers: up to Options::fps while the window is being used, ~4 fps
            // otherwise (enough for status text), and any input wakes the loop at once.
            void pace(Clock::time_point start)
            {
                const ImGuiIO& io = ImGui::GetIO();
                const bool busy = start - m_last_input < std::chrono::milliseconds(500) || io.WantTextInput || ImGui::IsAnyMouseDown();
                const int fps = std::clamp(m_options.fps, 15, 60);
                const auto frame = busy ? std::chrono::milliseconds(1000 / fps) : std::chrono::milliseconds(250);
                const auto spent = Clock::now() - start;
                if (spent < frame)
                {
                    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(frame - spent).count();
                    MsgWaitForMultipleObjects(1, &m_wake, FALSE, static_cast<DWORD>(wait), QS_ALLINPUT);
                }
            }

            // CPU path: draw into memory, copy to the window with GDI. pace() limits the frame
            // rate, and frames identical to the last one are neither drawn nor copied.
            void cpu_frame()
            {
                bool force = false;
                if (m_resize_w && m_resize_h)
                {
                    m_soft.resize(static_cast<int>(m_resize_w), static_cast<int>(m_resize_h));
                    m_resize_w = m_resize_h = 0;
                    force = true;
                }
                if (IsIconic(m_hwnd))
                {
                    MsgWaitForMultipleObjects(1, &m_wake, FALSE, 200, QS_ALLINPUT);
                    return;
                }
                build_ui();
                if (m_soft.render(ImGui::GetDrawData(), skin().clear_rgb(), force))
                {
                    HDC dc = GetDC(m_hwnd);
                    blit(dc);
                    ReleaseDC(m_hwnd, dc);
                }
            }

            void blit(HDC dc)
            {
                if (!m_cpu || m_soft.width() <= 0)
                {
                    return;
                }
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = m_soft.width();
                bi.bmiHeader.biHeight = -m_soft.height(); // top-down rows
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(m_soft.width()), static_cast<DWORD>(m_soft.height()), 0, 0, 0,
                                  static_cast<UINT>(m_soft.height()), m_soft.pixels(), &bi, DIB_RGB_COLORS);
            }

            void pump()
            {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            void handle_requests()
            {
                const int req = m_request.exchange(-1);
                if (req < 0)
                {
                    return;
                }
                const bool show_it = req == 2 ? !m_shown.load() : req == 1;
                if (show_it)
                {
                    show();
                }
                else
                {
                    hide();
                }
            }

            void show()
            {
                if (m_shown.load())
                {
                    SetForegroundWindow(m_hwnd);
                    return;
                }
                m_return_focus = GetForegroundWindow();
                if (!m_placed)
                {
                    if (HWND game = find_game_window())
                    {
                        RECT gr{};
                        GetWindowRect(game, &gr);
                        RECT mine{};
                        GetWindowRect(m_hwnd, &mine);
                        const int w = mine.right - mine.left;
                        const int h = std::min<int>(mine.bottom - mine.top, std::max<int>(300, gr.bottom - gr.top - 80));
                        SetWindowPos(m_hwnd, nullptr, gr.left + 40, gr.top + 60, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                    m_placed = true;
                }
                ShowWindow(m_hwnd, SW_SHOW);
                SetForegroundWindow(m_hwnd);
                m_shown.store(true);
                m_view.refresh_presets();
            }

            void hide()
            {
                if (!m_shown.load())
                {
                    return;
                }
                ShowWindow(m_hwnd, SW_HIDE);
                m_shown.store(false);
                DWORD pid = 0;
                if (m_return_focus && IsWindow(m_return_focus) && GetWindowThreadProcessId(m_return_focus, &pid) && pid == GetCurrentProcessId())
                {
                    SetForegroundWindow(m_return_focus);
                }
            }

            void autosave(bool force = false)
            {
                if (!m_options.session_autosave || !m_shelf || !m_shelf->ready())
                {
                    return;
                }
                const auto rev = saved_key();
                if (rev == m_saved_revision)
                {
                    m_dirty_since = {};
                    return;
                }
                const auto now = Clock::now();
                if (!m_dirty_since)
                {
                    m_dirty_since = now;
                }
                if (!force && now - *m_dirty_since < std::chrono::milliseconds(1200))
                {
                    return;
                }
                uint64_t book_rev = 0, morph_rev = 0;
                // The session file holds the player's sliders (NPC sliders last for one game session).
                Registry& reg = Registry::instance();
                book_rev = reg.edit_revision();
                morph_rev = reg.morph_revision();
                const EditBook book = reg.edits_of(kPlayerActor);
                const MorphBook morphs = reg.morphs_of(kPlayerActor);
                std::string ignored;
                m_shelf->save(PresetShelf::kSessionName, book, ignored, &morphs);
                m_saved_revision = {book_rev, morph_rev};
                m_dirty_since = {};
            }

            // Bone and morph revisions: the session file is rewritten when either changes.
            static std::pair<uint64_t, uint64_t> saved_key()
            {
                const Registry& reg = Registry::instance();
                return {reg.edit_revision(), reg.morph_revision()};
            }

            // ------------------------------------------------------------------ members
            const PresetShelf* m_shelf{};
            Options m_options{};
            HANDLE m_thread{};
            DWORD m_thread_id{};
            HANDLE m_wake{};
            HWND m_hwnd{};
            HWND m_return_focus{};
            Gpu m_gpu{};
            ImGuiContext* m_imgui{};
            float m_dpi{1.0f};
            UINT m_resize_w{};
            UINT m_resize_h{};
            bool m_occluded{};
            bool m_cpu{true};
            bool m_gpu_failed{false};
            SoftRenderer m_soft{};
            Clock::time_point m_last_input{};
            bool m_restyle{};
            bool m_placed{};
            std::atomic<bool> m_quit{false};
            std::atomic<bool> m_shown{false};
            std::atomic<int> m_request{-1};

            std::pair<uint64_t, uint64_t> m_saved_revision{};
            bool m_skin_dirty{};
            std::optional<Clock::time_point> m_dirty_since{};

            PanelView m_view{};
        };

        Panel g_panel;
    } // namespace

    bool start(const PresetShelf* shelf, const Options& options)
    {
        return g_panel.launch(shelf, options);
    }

    void stop()
    {
        g_panel.shutdown();
    }

    void toggle()
    {
        g_panel.want(2);
    }

    void set_visible(bool visible)
    {
        g_panel.want(visible ? 1 : 0);
    }

    bool visible()
    {
        return g_panel.is_shown();
    }

    void post_message(const std::string& text)
    {
        g_panel.set_message(text);
    }

    std::vector<std::string> list_skins()
    {
        std::filesystem::path folder;
        {
            std::lock_guard guard(s_skins_lock);
            folder = s_skins_folder;
        }
        return Skin::list(folder);
    }

    std::string current_skin()
    {
        return Skin::active_name();
    }

    void request_skin(const std::string& name)
    {
        Skin::request(name, true);
        g_panel.wake();
    }
} // namespace uuepbs::ui
