// UUEPBS - native half (plain DLL, no UE4SS headers)
//
// Scripts/main.lua maps this DLL into the game with package.loadlib(path, "*").
// DllMain only starts a service thread; everything else happens there:
//   * reads bridge_in.txt (what Lua found: the character, its meshes, bones, commands)
//   * tracks those meshes and installs the pose hook (win/pose_hook.cpp)
//   * polls the hotkey and runs the slider window (win/slider_window.cpp)
//   * writes bridge_out.txt (rescan / pick requests and command replies for Lua)
// Because nothing is linked against UE4SS, the same DLL works with any UE4SS build
// that runs Lua mods.
#include "core/body_groups.hpp"
#include "core/bridge.hpp"
#include "core/json.hpp"
#include "core/presets.hpp"
#include "core/registry.hpp"
#include "ui/panel_view.hpp"
#include "win/pose_hook.hpp"
#include "win/slider_window.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;

    constexpr const char* kVersion = "v2.1.0";

    fs::path module_folder()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&module_folder),
                           &self);
        wchar_t buffer[MAX_PATH * 2]{};
        GetModuleFileNameW(self, buffer, static_cast<DWORD>(std::size(buffer)));
        return fs::path(buffer).parent_path();
    }

    class Log
    {
      public:
        void open(const fs::path& file)
        {
            std::lock_guard guard(m_lock);
            m_file = file;
            std::ofstream out(m_file, std::ios::trunc); // one log per game session
        }

        void line(const std::string& text)
        {
            std::lock_guard guard(m_lock);
            if (m_file.empty())
            {
                return;
            }
            std::ofstream out(m_file, std::ios::app);
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
            out << '[' << stamp << "] " << text << '\n';
        }

        fs::path file() const { return m_file; }

      private:
        std::mutex m_lock;
        fs::path m_file;
    };

    Log g_log;

    bool read_whole(const fs::path& file, std::string& out)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    // Write to a temp file, then swap it in, so Lua never sees half a file.
    bool write_atomically(const fs::path& file, const std::string& text)
    {
        const fs::path tmp = file.wstring() + L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                return false;
            }
            out << text;
        }
        return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; // no disk flush: Lua reads it from the cache
    }

    std::string key_label(int vk)
    {
        if (vk >= 0x70 && vk <= 0x87)
        {
            return "F" + std::to_string(vk - 0x70 + 1);
        }
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
        const LONG lparam = static_cast<LONG>(((scan & 0xFF) << 16) | ((scan & 0xFF00) == 0xE000 ? (1 << 24) : 0));
        wchar_t name[64]{};
        if (scan && GetKeyNameTextW(lparam, name, 64) > 0)
        {
            char utf8[128]{};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
            return utf8;
        }
        return "key " + std::to_string(vk);
    }

    class Service final : public uuepbs::ui::TargetSource
    {
      public:
        void run()
        {
            m_folder = module_folder();
            m_in = m_folder / L"bridge_in.txt";
            m_out_file = m_folder / L"bridge_out.txt";
            g_log.open(m_folder / L"UUEPBS.log");
            g_log.line(std::string("UUEPBS (Universal Unreal Engine Player Body Sliders) ") + kVersion + " by XTGMods loaded from " + uuepbs::path_to_utf8(m_folder));

            char session[32];
            std::snprintf(session, sizeof(session), "%lx-%llx", GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
            {
                std::lock_guard guard(m_lock);
                m_out.session = session;
            }
            uuepbs::Registry::instance().set_safe_reader(&uuepbs::hook::safe_read);

            // Hotkeys are polled every 30 ms; the bridge, file watches and hook upkeep run
            // every 240 ms (Lua itself only looks at the bridge every 400 ms).
            unsigned tick = 0;
            while (!m_quit.load())
            {
                poll_hotkey();
                if (tick++ % 8 == 0)
                {
                    try
                    {
                        step();
                    }
                    catch (const std::exception& e)
                    {
                        g_log.line(std::string("error: ") + e.what());
                    }
                }
                Sleep(30);
            }
        }

        void quit() { m_quit.store(true); }

        // ---- TargetSource (called from the window thread) --------------------
        std::vector<uuepbs::ui::TargetChoice> targets() override
        {
            std::lock_guard guard(m_lock);
            std::vector<uuepbs::ui::TargetChoice> out;
            for (const auto& c : m_state.candidates)
            {
                out.push_back({c.id, c.label});
            }
            return out;
        }

        uuepbs::ui::TargetChoice current_target() override
        {
            std::lock_guard guard(m_lock);
            return {m_state.target.id, m_state.target.label};
        }

        void pick_target(const std::string& id) override
        {
            std::lock_guard guard(m_lock);
            ++m_out.pick;
            m_out.pick_id = id;
            m_out_dirty = true;
        }

        void refresh_targets() override
        {
            std::lock_guard guard(m_lock);
            ++m_out.refresh;
            m_out_dirty = true;
        }

        void request_rescan() override
        {
            std::lock_guard guard(m_lock);
            ++m_out.rescan;
            m_out_dirty = true;
            m_retry_hook = true; // a refused hook gets another try right away
        }

        std::string refresh_hotkey_text() override
        {
            const int vk = m_refresh_key.load();
            return vk ? key_label(vk) : std::string();
        }

        std::string link_text() override
        {
            std::lock_guard guard(m_lock);
            if (m_state.session.empty())
            {
                return "waiting for Scripts/main.lua (is the mod enabled in UE4SS?)";
            }
            std::string text = "connected, script " + m_state.setting("version", "?") + ", " + std::to_string(m_state.rigs.size()) + " mesh(es)";
            const std::string game = m_state.setting("game");
            if (!game.empty())
            {
                text += ", game " + game;
            }
            const std::string engine = m_state.setting("engine");
            if (!engine.empty())
            {
                text += ", UE " + engine;
            }
            return text;
        }

        std::string hotkey_text() override { return key_label(m_hotkey.load()); }

        std::string write_diagnostics() override
        {
            const uintptr_t comp = uuepbs::Registry::instance().primary_component();
            int32_t bones = 0;
            {
                std::lock_guard guard(m_lock);
                for (const auto& r : m_state.rigs)
                {
                    if (r.address == comp)
                    {
                        bones = static_cast<int32_t>(r.names.size());
                    }
                }
            }
            if (!comp)
            {
                g_log.line("diagnostics: no mesh is tracked yet");
            }
            else
            {
                g_log.line("diagnostics:\n" + uuepbs::hook::diagnose(reinterpret_cast<void*>(comp), bones));
            }
            return uuepbs::path_to_utf8(g_log.file());
        }

      private:
        // ---------------------------------------------------------------- hotkey
        static bool game_has_focus()
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            return pid == GetCurrentProcessId(); // the game or our own window
        }

        // Edge-triggered: true once per key press while the game or the window has focus.
        static bool pressed(int vk, bool& was_down)
        {
            if (!vk)
            {
                return false;
            }
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool fired = down && !was_down && game_has_focus();
            was_down = down;
            return fired;
        }

        void poll_hotkey()
        {
            if (!m_window_started)
            {
                return;
            }
            if (pressed(m_hotkey.load(), m_key_down))
            {
                uuepbs::ui::toggle();
            }
            if (pressed(m_refresh_key.load(), m_refresh_down))
            {
                refresh_now();
            }
        }

        // Same as the window's Refresh button: rescan the character, relist the picker, and
        // give the pose hook a fresh try (useful when a game sets its character up late).
        void refresh_now()
        {
            refresh_targets();
            request_rescan();
            uuepbs::ui::post_message("Refreshing (" + key_label(m_refresh_key.load()) + ")...");
        }

        // ---------------------------------------------------------------- bridge
        void step()
        {
            read_lua_state();
            watch_body_map();
            keep_hook_going();
            track_window();

            // Once per session, a few seconds after the hook first went in: rescan the character
            // (some games finish setting it up late; VanySlash used to need a manual F7).
            if (m_auto_refresh_at != Clock::time_point{} && Clock::now() >= m_auto_refresh_at)
            {
                m_auto_refresh_at = {};
                g_log.line("automatic refresh after the pose hook went in");
                request_rescan();
            }

            if (uuepbs::Registry::instance().take_rescan_request())
            {
                request_rescan();
            }
            const std::string wd = uuepbs::hook::watchdog();
            if (!wd.empty())
            {
                g_log.line(wd);
                if (wd.rfind("pose hook confirmed", 0) == 0)
                {
                    g_log.line("to pin these values for this game, add to its GameProfiles JSON: " + uuepbs::hook::profile_snippet());
                }
                if (!uuepbs::hook::active())
                {
                    m_hook_attempts.clear(); // start over on the next poll
                }
            }
            report_rigs();
            publish();
        }

        // Lua refreshes the Character picker only while the window is open (listing actors is
        // expensive on the game thread), so it is told when that is.
        void track_window()
        {
            const bool open = m_window_started && uuepbs::ui::visible();
            std::lock_guard guard(m_lock);
            if (m_out.window_open != open)
            {
                m_out.window_open = open;
                m_out_dirty = true;
            }
        }

        // "Hook": { "Slot": 330, "Buffers": "0x610", "ReadIndex": "0x658" } in the game profile pins
        // the pose hook for games where the automatic search struggles.
        void apply_hook_override(const std::string& profile_text)
        {
            uuepbs::json::JsonValue root;
            std::string error;
            uuepbs::json::JsonReader reader(profile_text);
            int slot = -1;
            uint32_t buffers = 0, read = 0;
            if (reader.read(root, error))
            {
                if (const uuepbs::json::JsonValue* h = root.find("Hook"); h && h->kind == uuepbs::json::JsonValue::Kind::Object)
                {
                    auto number = [&](const char* key) -> int64_t {
                        const uuepbs::json::JsonValue* v = h->find(key);
                        if (!v)
                        {
                            return -1;
                        }
                        if (v->kind == uuepbs::json::JsonValue::Kind::Number)
                        {
                            return static_cast<int64_t>(v->number);
                        }
                        if (v->kind == uuepbs::json::JsonValue::Kind::String)
                        {
                            return static_cast<int64_t>(std::strtoll(v->text.c_str(), nullptr, 0)); // "0x610" or "1552"
                        }
                        return -1;
                    };
                    const int64_t s = number("Slot"), b = number("Buffers"), r = number("ReadIndex");
                    if (s >= 0 && b > 0 && r > 0)
                    {
                        slot = static_cast<int>(s);
                        buffers = static_cast<uint32_t>(b);
                        read = static_cast<uint32_t>(r);
                    }
                }
            }
            if (slot != m_override_slot || buffers != m_override_buffers || read != m_override_read)
            {
                m_override_slot = slot;
                m_override_buffers = buffers;
                m_override_read = read;
                uuepbs::hook::set_override(slot, buffers, read);
                if (slot >= 0)
                {
                    g_log.line("game profile pins the pose hook: slot " + std::to_string(slot) + ", buffers " + std::to_string(buffers) + ", read index " +
                               std::to_string(read));
                }
            }
        }

        static uint64_t file_stamp(const fs::path& file)
        {
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            if (file.empty() || !GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &attr))
            {
                return 0;
            }
            return ((static_cast<uint64_t>(attr.ftLastWriteTime.dwHighDateTime) << 32) | attr.ftLastWriteTime.dwLowDateTime) ^ attr.nFileSizeLow;
        }

        // Scripts/BoneDictionary.json and the game profile's "BodyGroups": reloaded whenever
        // either file changes, so edits show up in the Body tab without restarting the game.
        void watch_body_map()
        {
            const auto now = Clock::now();
            if (now < m_body_check)
            {
                return;
            }
            m_body_check = now + std::chrono::seconds(2);
            fs::path profile;
            {
                std::lock_guard guard(m_lock);
                const std::string p = m_state.setting("profile_file");
                profile = p.empty() ? fs::path() : uuepbs::path_from_utf8(p);
            }
            const fs::path dict_path = m_folder.parent_path() / L"Scripts" / L"BoneDictionary.json";
            const uint64_t ds = file_stamp(dict_path);
            const uint64_t ps = file_stamp(profile);
            if (m_body_loaded && ds == m_dict_stamp && ps == m_profile_stamp && profile == m_profile_path)
            {
                return;
            }
            m_body_loaded = true;
            m_dict_stamp = ds;
            m_profile_stamp = ps;
            m_profile_path = profile;

            uuepbs::BoneDictionary dict = uuepbs::BoneDictionary::builtin();
            std::string status;
            std::string text, message;
            if (ds && read_whole(dict_path, text))
            {
                uuepbs::BoneDictionary parsed;
                if (uuepbs::BoneDictionary::parse(text, parsed, message))
                {
                    dict = std::move(parsed);
                    status = "BoneDictionary.json (" + message + ")";
                }
                else
                {
                    status = "BoneDictionary.json has a mistake, using the built-in one: " + message;
                }
            }
            else
            {
                status = "built-in dictionary (Scripts\\BoneDictionary.json not found)";
            }
            std::vector<uuepbs::UserGroup> user;
            if (ps && read_whole(profile, text))
            {
                apply_hook_override(text);
                if (uuepbs::parse_user_groups(text, user, message))
                {
                    if (!user.empty())
                    {
                        status += ", game profile: " + message;
                    }
                }
                else
                {
                    status += ", game profile BodyGroups ignored: " + message;
                }
            }
            g_log.line("body tab mapping: " + status);
            uuepbs::BodyMapSource::instance().set(std::move(dict), std::move(user), status);
        }

        // Once, some seconds after the hook went in: did the tracked meshes actually get edited?
        void report_rigs()
        {
            if (m_report_at == Clock::time_point{} || Clock::now() < m_report_at)
            {
                return;
            }
            m_report_at = {};
            uuepbs::Registry& reg = uuepbs::Registry::instance();
            const size_t edits = reg.edits().size();
            std::string text = "status after 15 s: " + uuepbs::hook::describe() + ", " + std::to_string(edits) + " edited bone(s), sliders " +
                               (reg.enabled() ? "on" : "off");
            bool any = false;
            for (const auto& r : reg.rigs())
            {
                text += "\n  " + r.label + (r.primary ? " (primary)" : "") + ": " + std::to_string(r.bones) + " bones, " + std::to_string(r.frames) +
                        " poses edited" + (r.stale ? ", STALE" : "");
                any = any || r.frames > 0;
            }
            if (!any)
            {
                text += "\n  -> the hooked function has not run for any tracked mesh: it is probably the wrong function for this game. "
                        "Please send this log.";
            }
            g_log.line(text);
        }

        void read_lua_state()
        {
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            if (!GetFileAttributesExW(m_in.c_str(), GetFileExInfoStandard, &attr))
            {
                return;
            }
            const uint64_t stamp = (static_cast<uint64_t>(attr.ftLastWriteTime.dwHighDateTime) << 32) | attr.ftLastWriteTime.dwLowDateTime;
            const uint64_t size = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
            if (stamp == m_in_stamp && size == m_in_size && !m_in_retry)
            {
                return;
            }
            std::string text;
            if (!read_whole(m_in, text))
            {
                m_in_retry = true;
                return;
            }
            std::string why;
            auto parsed = uuepbs::bridge::parse_lua_state(text, &why);
            if (!parsed)
            {
                m_in_retry = true; // Lua is mid-write; next poll
                return;
            }
            m_in_retry = false;
            m_in_stamp = stamp;
            m_in_size = size;
            apply(std::move(*parsed));
        }

        void apply(uuepbs::bridge::LuaState state)
        {
            uuepbs::Registry& reg = uuepbs::Registry::instance();
            bool new_session = false;
            {
                std::lock_guard guard(m_lock);
                new_session = state.session != m_state.session;
                m_last_read = Clock::now();
            }
            if (new_session)
            {
                g_log.line("script connected (session " + state.session + ")");
                reg.untrack_all();
                m_rig_keys.clear();
                std::lock_guard guard(m_lock);
                m_out.ack = 0;
                m_out.lua_session = state.session;
                m_out.replies.clear();
                m_hook_attempts.clear();
                m_out_dirty = true;
            }

            setup(state);
            sync_rigs(state);

            std::vector<uuepbs::bridge::Command> todo;
            {
                std::lock_guard guard(m_lock);
                for (const auto& c : state.commands)
                {
                    if (c.id > m_out.ack)
                    {
                        todo.push_back(c);
                    }
                }
                m_state = std::move(state);
            }
            for (const auto& c : todo)
            {
                const std::string reply = run_command(c);
                std::lock_guard guard(m_lock);
                m_out.ack = std::max(m_out.ack, c.id);
                m_out.replies.push_back({c.id, reply});
                while (m_out.replies.size() > 8)
                {
                    m_out.replies.erase(m_out.replies.begin());
                }
                m_out_dirty = true;
            }
        }

        void setup(const uuepbs::bridge::LuaState& state)
        {
            // Lua cannot create folders: it asks for Scripts\GameProfiles here if it went missing.
            const std::string mk = state.setting("mkdir");
            if (!mk.empty() && mk != m_made_dir)
            {
                m_made_dir = mk;
                const fs::path dir = uuepbs::path_from_utf8(mk).lexically_normal();
                const fs::path mod = m_folder.parent_path().lexically_normal();
                const auto rel = dir.lexically_relative(mod);
                if (!rel.empty() && rel.native().rfind(L"..", 0) != 0) // only inside this mod's folder
                {
                    std::error_code ec;
                    fs::create_directories(dir, ec);
                    g_log.line("created folder " + mk + (ec ? " failed: " + ec.message() : ""));
                }
            }

            const int vk = uuepbs::bridge::virtual_key_from_name(state.setting("key", "F6"));
            m_hotkey.store(vk ? vk : VK_F6);
            const std::string refresh_name = state.setting("refresh_key", "F7");
            m_refresh_key.store(uuepbs::bridge::virtual_key_from_name(refresh_name)); // "" or unknown = off

            const std::string folder = state.setting("presets");
            if (!m_shelf_ready || folder != m_shelf_source)
            {
                fs::path dir = folder.empty() ? m_folder.parent_path().parent_path().parent_path().parent_path() / L"UUEPBS Presets" : uuepbs::path_from_utf8(folder);
                dir = dir.lexically_normal();
                migrate_presets(dir);
                m_shelf.set_folder(dir);
                m_shelf_source = folder;
                g_log.line("presets folder: " + uuepbs::path_to_utf8(m_shelf.folder()));
            }

            if (!m_shelf_ready)
            {
                m_shelf_ready = true;
                const std::string startup = state.setting("startup");
                uuepbs::EditBook book;
                std::string message;
                if (!startup.empty() && m_shelf.load(startup, book, message))
                {
                    uuepbs::Registry::instance().replace_edits(std::move(book));
                    g_log.line("startup preset: " + message);
                }
                else if (state.setting("restore", "1") == "1" && m_shelf.exists(uuepbs::PresetShelf::kSessionName) &&
                         m_shelf.load(uuepbs::PresetShelf::kSessionName, book, message))
                {
                    uuepbs::Registry::instance().replace_edits(std::move(book));
                    g_log.line("restored last session: " + message);
                }
            }

            if (!m_window_started)
            {
                uuepbs::ui::Options options;
                options.topmost = state.setting("topmost", "1") == "1";
                options.session_autosave = true;
                const double scale = std::strtod(state.setting("scale", "1").c_str(), nullptr);
                options.extra_scale = static_cast<float>(scale > 0.5 && scale < 3.0 ? scale : 1.0);
                const double font = std::strtod(state.setting("font", "19").c_str(), nullptr);
                options.font_size = static_cast<float>(font >= 12 && font <= 40 ? font : 19.0);
                options.version = kVersion;
                options.targets = this;
                const std::string renderer = uuepbs::fold_case(state.setting("renderer", "cpu"));
                options.cpu_renderer = renderer != "gpu" && renderer != "d3d11" && renderer != "directx";
                const long fps = std::strtol(state.setting("fps", "30").c_str(), nullptr, 10);
                options.fps = static_cast<int>(fps >= 15 && fps <= 60 ? fps : 30);
                g_log.line(std::string("window renderer: ") + (options.cpu_renderer ? "CPU" : "GPU (Direct3D 11)"));
                m_window_started = uuepbs::ui::start(&m_shelf, options);
            }
        }

        // Before 2.1 the mod was XTG Body Slider and kept presets in "<Win64>\RBS Presets". The
        // first time the new default folder is used, those presets are copied into it (the old
        // folder is left as it is).
        static void migrate_presets(const fs::path& dir)
        {
            std::error_code ec;
            if (dir.filename() != L"UUEPBS Presets" || fs::exists(dir, ec))
            {
                return;
            }
            const fs::path old = dir.parent_path() / L"RBS Presets";
            if (!fs::is_directory(old, ec))
            {
                return;
            }
            fs::create_directories(dir, ec);
            if (ec)
            {
                g_log.line("could not create " + uuepbs::path_to_utf8(dir) + ": " + ec.message());
                return;
            }
            int copied = 0;
            for (const auto& entry : fs::directory_iterator(old, ec))
            {
                const std::wstring ext = entry.path().extension().wstring();
                if (entry.is_regular_file(ec) && (_wcsicmp(ext.c_str(), L".json") == 0 || _wcsicmp(ext.c_str(), L".rbs") == 0))
                {
                    if (fs::copy_file(entry.path(), dir / entry.path().filename(), fs::copy_options::skip_existing, ec))
                    {
                        ++copied;
                    }
                }
            }
            g_log.line("copied " + std::to_string(copied) + " preset(s) from " + uuepbs::path_to_utf8(old));
        }

        static std::string rig_key(const uuepbs::bridge::RigInfo& r)
        {
            std::string key = r.label + '|' + r.owner + '|' + (r.primary ? "1" : "0") + '|' + std::to_string(r.names.size());
            for (const auto& n : r.names)
            {
                key += '|' + n;
            }
            for (const auto p : r.parents)
            {
                key += ',' + std::to_string(p);
            }
            return key;
        }

        void sync_rigs(const uuepbs::bridge::LuaState& state)
        {
            uuepbs::Registry& reg = uuepbs::Registry::instance();
            std::set<uintptr_t> wanted;
            for (const auto& r : state.rigs)
            {
                wanted.insert(r.address);
                const std::string key = rig_key(r);
                const auto it = m_rig_keys.find(r.address);
                if (it != m_rig_keys.end() && it->second == key && reg.is_tracked(r.address))
                {
                    continue;
                }
                std::string message;
                if (reg.track(r.address, r.label, r.owner, r.names, r.parents, r.primary, message))
                {
                    m_rig_keys[r.address] = key;
                    if (!r.reference.empty())
                    {
                        reg.set_reference_pose(r.address, r.reference);
                    }
                    g_log.line(r.owner + ": " + message);
                }
                else
                {
                    m_rig_keys.erase(r.address);
                    g_log.line("skipped " + r.label + ": " + message);
                }
            }
            for (auto it = m_rig_keys.begin(); it != m_rig_keys.end();)
            {
                if (!wanted.count(it->first))
                {
                    reg.untrack(it->first);
                    it = m_rig_keys.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        void keep_hook_going()
        {
            {
                std::lock_guard guard(m_lock);
                if (m_retry_hook)
                {
                    m_retry_hook = false;
                    for (auto& [address, attempt] : m_hook_attempts)
                    {
                        if (!attempt.done)
                        {
                            attempt = {}; // retry now instead of waiting out the back-off
                        }
                    }
                }
            }
            std::vector<std::pair<uintptr_t, int32_t>> rigs;
            {
                std::lock_guard guard(m_lock);
                for (const auto& r : m_state.rigs)
                {
                    if (m_rig_keys.count(r.address))
                    {
                        // Primary first: it decides the slot and layout.
                        if (r.primary)
                        {
                            rigs.insert(rigs.begin(), {r.address, static_cast<int32_t>(r.names.size())});
                        }
                        else
                        {
                            rigs.push_back({r.address, static_cast<int32_t>(r.names.size())});
                        }
                    }
                }
            }
            const auto now = Clock::now();
            for (const auto& [address, bones] : rigs)
            {
                Attempt& a = m_hook_attempts[address];
                if (a.done || now < a.next)
                {
                    continue;
                }
                const uuepbs::hook::Report r = uuepbs::hook::install_for(reinterpret_cast<void*>(address), bones);
                if (r.message != a.last_message)
                {
                    g_log.line(r.message);
                    a.last_message = r.message;
                }
                switch (r.outcome)
                {
                case uuepbs::hook::Outcome::Installed:
                    a.done = true;
                    set_hook("live", r.message);
                    if (!m_auto_refreshed)
                    {
                        m_auto_refreshed = true;
                        m_auto_refresh_at = now + std::chrono::seconds(3);
                    }
                    if (!m_diag_logged)
                    {
                        // Always record what the hook finder saw, so one log is enough to check a new game.
                        m_diag_logged = true;
                        m_report_at = now + std::chrono::seconds(15);
                        g_log.line("hook finder details:\n" + uuepbs::hook::diagnose(reinterpret_cast<void*>(address), bones));
                    }
                    break;
                case uuepbs::hook::Outcome::NotReadyYet:
                    // Quick retries at first, then slower while a menu or loading screen lasts.
                    ++a.waits;
                    a.next = now + (a.waits < 8 ? std::chrono::milliseconds(250) : a.waits < 40 ? std::chrono::milliseconds(1000)
                                                                                                : std::chrono::milliseconds(2000));
                    if (a.waits == 60 && !m_refusal_logged)
                    {
                        m_refusal_logged = true;
                        g_log.line("still waiting for the pose after a minute; diagnostics follow\n" +
                                   uuepbs::hook::diagnose(reinterpret_cast<void*>(address), bones));
                    }
                    if (!uuepbs::hook::active())
                    {
                        set_hook("waiting", r.message);
                    }
                    break;
                case uuepbs::hook::Outcome::Unsupported:
                    a.next = now + std::chrono::seconds(std::min(30, 5 * (a.failures + 1)));
                    if (++a.failures == 3 && !m_refusal_logged)
                    {
                        m_refusal_logged = true; // once per session, not once per mesh
                        g_log.line("hook refused three times; diagnostics follow\n" + uuepbs::hook::diagnose(reinterpret_cast<void*>(address), bones));
                    }
                    if (!uuepbs::hook::active())
                    {
                        set_hook("failed", r.message);
                    }
                    break;
                }
            }
            if (uuepbs::hook::active())
            {
                set_hook("live", uuepbs::hook::describe());
            }
        }

        void set_hook(const std::string& state, const std::string& text)
        {
            std::lock_guard guard(m_lock);
            if (m_out.hook_state != state || m_out.hook_text != text)
            {
                m_out.hook_state = state;
                m_out.hook_text = text;
                m_out_dirty = true;
            }
        }

        void publish()
        {
            std::string text;
            {
                std::lock_guard guard(m_lock);
                if (!m_out_dirty && Clock::now() - m_last_publish < std::chrono::seconds(5))
                {
                    return;
                }
                text = uuepbs::bridge::format_dll_state(m_out);
                m_out_dirty = false;
                m_last_publish = Clock::now();
            }
            if (!write_atomically(m_out_file, text))
            {
                std::lock_guard guard(m_lock);
                m_out_dirty = true; // Lua had it open; try again next poll
            }
        }

        // ---------------------------------------------------------------- commands
        std::string status_text()
        {
            uuepbs::Registry& reg = uuepbs::Registry::instance();
            std::string s = std::string("UUEPBS ") + kVersion + " by XTGMods\n";
            s += uuepbs::hook::describe() + "\n";
            s += std::string("sliders ") + (reg.enabled() ? "enabled" : "disabled") + ", " + std::to_string(reg.edits().size()) + " edited bone(s)\n";
            s += "mirroring: " + reg.mirror_source() + "\n";
            s += "presets: " + uuepbs::path_to_utf8(m_shelf.folder()) + "\n";
            for (const auto& r : reg.rigs())
            {
                s += "  " + r.owner + " / " + r.label + ": " + std::to_string(r.bones) + " bones, " + std::to_string(r.frames) + " poses" +
                     (r.stale ? " (stale)" : "") + "\n";
            }
            s += "log: " + uuepbs::path_to_utf8(g_log.file());
            return s;
        }

        std::string run_command(const uuepbs::bridge::Command& c)
        {
            uuepbs::Registry& reg = uuepbs::Registry::instance();
            const std::string& v = c.verb;
            if (v == "ui" || v == "toggle" || v.empty())
            {
                uuepbs::ui::toggle();
                return "window toggled";
            }
            if (v == "show" || v == "hide")
            {
                uuepbs::ui::set_visible(v == "show");
                return "window " + v;
            }
            if (v == "status")
            {
                return status_text();
            }
            if (v == "list")
            {
                std::string joined;
                for (const std::string& name : m_shelf.list())
                {
                    joined += (joined.empty() ? "" : "\n") + name;
                }
                return joined.empty() ? "no presets saved yet" : joined;
            }
            if (v == "load" || v == "save")
            {
                std::string message;
                if (v == "load")
                {
                    uuepbs::EditBook book;
                    if (m_shelf.load(c.argument, book, message))
                    {
                        reg.replace_edits(std::move(book));
                    }
                }
                else
                {
                    m_shelf.save(c.argument, reg.edits(), message);
                }
                uuepbs::ui::post_message(message);
                return message;
            }
            if (v == "on" || v == "off")
            {
                reg.set_enabled(v == "on");
                return "sliders " + v;
            }
            if (v == "reset")
            {
                reg.clear_edits();
                uuepbs::ui::post_message("All sliders reset");
                return "all sliders reset";
            }
            if (v == "diag")
            {
                return "diagnostics written to " + write_diagnostics();
            }
            return "unknown command '" + v + "'";
        }

        struct Attempt
        {
            bool done{};
            int failures{};
            int waits{};
            Clock::time_point next{};
            std::string last_message;
        };

        std::atomic<bool> m_quit{false};
        std::atomic<int> m_hotkey{VK_F6};
        std::atomic<int> m_refresh_key{VK_F7};
        bool m_key_down{};
        bool m_refresh_down{};
        bool m_retry_hook{};
        bool m_window_started{};

        fs::path m_folder, m_in, m_out_file;
        uint64_t m_in_stamp{}, m_in_size{};
        bool m_in_retry{};

        std::mutex m_lock; // guards m_state, m_out
        uuepbs::bridge::LuaState m_state;
        uuepbs::bridge::DllState m_out;
        bool m_out_dirty{true};
        Clock::time_point m_last_publish{};
        Clock::time_point m_last_read{};

        std::map<uintptr_t, std::string> m_rig_keys;
        std::map<uintptr_t, Attempt> m_hook_attempts;

        uuepbs::PresetShelf m_shelf;
        std::string m_shelf_source;
        std::string m_made_dir;
        bool m_diag_logged{};
        bool m_refusal_logged{};
        bool m_body_loaded{};
        int m_override_slot{-1};
        uint32_t m_override_buffers{}, m_override_read{};
        uint64_t m_dict_stamp{}, m_profile_stamp{};
        fs::path m_profile_path;
        Clock::time_point m_body_check{};
        Clock::time_point m_report_at{};
        Clock::time_point m_auto_refresh_at{};
        bool m_auto_refreshed{};
        bool m_shelf_ready{};
    };

    Service* g_service = nullptr;

    DWORD WINAPI service_main(void*)
    {
        g_service->run();
        return 0;
    }
} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Pin the DLL: Lua may FreeLibrary it when its state closes (mod reload), but the
        // service thread, the window and the vtable detour must stay mapped until the game exits.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&DllMain), &pinned);

        g_service = new Service(); // lives until the process exits
        HANDLE thread = CreateThread(nullptr, 0, &service_main, nullptr, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
