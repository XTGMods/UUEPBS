#include "panel_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace uuepbs::ui
{
    namespace
    {
        const ImVec4 kAccent{0.91f, 0.44f, 0.66f, 1.00f};
        const ImVec4 kAccentDim{0.60f, 0.28f, 0.44f, 1.00f};
        const ImVec4 kGood{0.45f, 0.85f, 0.55f, 1.00f};
        const ImVec4 kWarn{0.98f, 0.72f, 0.30f, 1.00f};
        const ImVec4 kMuted{0.62f, 0.62f, 0.68f, 1.00f};

        // Log scale from 0.1x to 10x puts 1.0x exactly in the middle of the slider.
        constexpr float kScaleMin = 0.10f;
        constexpr float kScaleMax = 10.0f;
    } // namespace

    // Ctrl + mouse wheel over the slider just drawn nudges it by `step`
    // (Ctrl + Shift: by `fine`). The plain wheel keeps scrolling the window.
    static bool wheel_nudge(float& v, float step, float fine, float lo, float hi)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.KeyCtrl)
        {
            return false;
        }
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        if (!ImGui::IsItemHovered() || io.MouseWheel == 0.0f)
        {
            return false;
        }
        const float delta = (io.KeyShift ? fine : step) * (io.MouseWheel > 0.0f ? 1.0f : -1.0f);
        v = std::min(hi, std::max(lo, v + delta));
        return true;
    }

    void PanelView::configure(const PresetShelf* shelf, PanelHost* host, TargetSource* targets, std::string version, bool topmost)
    {
        m_shelf = shelf;
        m_host = host;
        m_targets = targets;
        m_version = std::move(version);
        m_topmost = topmost;
    }

    void PanelView::set_message(const std::string& text)
    {
        std::lock_guard guard(m_message_lock);
        m_message = text;
        m_message_time = Clock::now();
    }

    void PanelView::apply_theme(float dpi)
    {
        ImGuiStyle fresh;
        ImGuiStyle& style = ImGui::GetStyle();
        style = fresh;
        ImGui::StyleColorsDark(&style);

        style.WindowRounding = 0.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 5.0f;
        style.ScrollbarRounding = 8.0f;
        style.FramePadding = ImVec2(9.0f, 6.0f);
        style.ItemSpacing = ImVec2(9.0f, 7.0f);
        style.WindowPadding = ImVec2(14.0f, 12.0f);
        style.GrabMinSize = 10.0f;
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;

        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.085f, 0.085f, 0.105f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.105f, 0.130f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.110f, 0.110f, 0.140f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.28f, 1.0f);
        c[ImGuiCol_FrameBg] = ImVec4(0.160f, 0.160f, 0.200f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.210f, 0.200f, 0.260f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.250f, 0.220f, 0.300f, 1.0f);
        c[ImGuiCol_TitleBg] = c[ImGuiCol_WindowBg];
        c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
        c[ImGuiCol_SliderGrab] = kAccent;
        c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.60f, 0.78f, 1.0f);
        c[ImGuiCol_CheckMark] = kAccent;
        c[ImGuiCol_Button] = ImVec4(0.20f, 0.19f, 0.25f, 1.0f);
        c[ImGuiCol_ButtonHovered] = kAccentDim;
        c[ImGuiCol_ButtonActive] = kAccent;
        c[ImGuiCol_Header] = ImVec4(0.26f, 0.18f, 0.25f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.36f, 0.22f, 0.32f, 1.0f);
        c[ImGuiCol_HeaderActive] = kAccentDim;
        c[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
        c[ImGuiCol_TabHovered] = kAccentDim;
        c[ImGuiCol_TabSelected] = ImVec4(0.34f, 0.20f, 0.30f, 1.0f);
        c[ImGuiCol_Separator] = ImVec4(0.24f, 0.24f, 0.30f, 1.0f);
        c[ImGuiCol_TextSelectedBg] = ImVec4(0.60f, 0.28f, 0.44f, 0.45f);

        style.ScaleAllSizes(dpi);
        style.FontScaleDpi = dpi;
    }

    void PanelView::sync()
    {
        Registry& reg = Registry::instance();
        if (reg.skeleton_revision() != m_skeleton_revision)
        {
            m_skeleton = reg.skeleton(&m_skeleton_revision);
            m_bone_keys.clear();
            for (const auto& n : m_skeleton.names)
            {
                m_bone_keys.insert(fold_case(n));
            }
            m_body_revision = 0; // re-resolve below
        }
        const uint64_t body_rev = BodyMapSource::instance().revision();
        if (body_rev != m_body_revision)
        {
            BoneDictionary dict;
            std::vector<UserGroup> user;
            BodyMapSource::instance().get(dict, user, m_body_status);
            m_body = resolve_body_groups(m_skeleton.names, m_skeleton.parents, dict, user);
            m_bone_group.clear();
            for (const GroupBones& gb : m_body.groups)
            {
                for (const std::string& b : gb.bones)
                {
                    m_bone_group[fold_case(b)] = gb.group.title;
                }
            }
            m_body_revision = body_rev;
        }
        if (reg.edit_revision() != m_book_revision)
        {
            m_book = reg.edits(&m_book_revision);
        }
        const auto now = Clock::now();
        if (now - m_rigs_time > std::chrono::milliseconds(500))
        {
            m_rigs = reg.rigs();
            m_rigs_time = now;
            m_mirror_text = reg.mirror_source();
        }
        if (m_targets && now - m_target_time > std::chrono::milliseconds(400))
        {
            m_target_list = m_targets->targets();
            m_target = m_targets->current_target();
            m_target_time = now;
        }
    }

    bool PanelView::has_bone(const std::string& bone) const
    {
        return m_bone_keys.count(fold_case(bone)) > 0;
    }

    Spread PanelView::spread_of(const std::string& bone, Spread fallback) const
    {
        const std::string key = fold_case(bone);
        const auto it = m_book.find(key);
        if (it != m_book.end())
        {
            return it->second.edit.spread;
        }
        const auto hint = m_spread_hint.find(key);
        return hint != m_spread_hint.end() ? hint->second : fallback;
    }

    BoneEdit PanelView::scale_of(const std::string& bone, Spread fallback) const
    {
        const auto it = m_book.find(fold_case(bone));
        if (it != m_book.end())
        {
            return it->second.edit;
        }
        BoneEdit s;
        s.spread = spread_of(bone, fallback);
        return s;
    }

    void PanelView::write(const std::string& bone, BoneEdit s)
    {
        Registry& reg = Registry::instance();
        s.clamp();
        const std::string key = fold_case(bone);
        m_spread_hint[key] = s.spread;
        if (s.is_neutral())
        {
            reg.clear_bone(bone);
            m_book.erase(key);
        }
        else
        {
            reg.set_bone(bone, s);
            m_book[key] = EditEntry{bone, s};
        }
        m_book_revision = reg.edit_revision();
    }

    // Body tab sliders only change scale; each bone keeps its own rotation/move.
    void PanelView::write_group(const GroupBones& group, const double axis[3])
    {
        for (const std::string& bone : group.bones)
        {
            if (!has_bone(bone))
            {
                continue;
            }
            BoneEdit s = scale_of(bone, group.group.spread);
            s.axis[0] = axis[0];
            s.axis[1] = axis[1];
            s.axis[2] = axis[2];
            s.spread = spread_of(bone, group.group.spread);
            write(bone, s);
        }
    }

    bool PanelView::size_slider(const char* id, BoneEdit& s, float lo, float hi)
    {
        const double avg = (s.axis[0] + s.axis[1] + s.axis[2]) / 3.0;
        float v = static_cast<float>(avg);
        const bool dragged = ImGui::SliderFloat(id, &v, lo, hi, "%.3fx", ImGuiSliderFlags_Logarithmic);
        const bool nudged = wheel_nudge(v, 0.01f, 0.001f, lo, hi);
        if (!dragged && !nudged)
        {
            return false;
        }
        const bool uniform = std::fabs(s.axis[0] - s.axis[1]) < 1e-6 && std::fabs(s.axis[1] - s.axis[2]) < 1e-6;
        if (uniform || avg <= 1e-6)
        {
            s.axis[0] = s.axis[1] = s.axis[2] = v;
        }
        else
        {
            const double k = v / avg;
            for (double& a : s.axis)
            {
                a *= k;
            }
        }
        return true;
    }

    bool PanelView::axis_sliders(BoneEdit& s)
    {
        static const char* names[3] = {"Length (X)", "Width (Y)", "Depth (Z)"};
        bool changed = false;
        for (int a = 0; a < 3; ++a)
        {
            float v = static_cast<float>(s.axis[a]);
            ImGui::PushID(a);
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Length (X)").x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
            if (ImGui::SliderFloat(names[a], &v, kScaleMin, kScaleMax, "%.3fx", ImGuiSliderFlags_Logarithmic))
            {
                s.axis[a] = v;
                changed = true;
            }
            if (wheel_nudge(v, 0.01f, 0.001f, kScaleMin, kScaleMax))
            {
                s.axis[a] = v;
                changed = true;
            }
            ImGui::PopID();
        }
        return changed;
    }

    // Shared layout for a labelled three-row slider block.
    static bool triple_sliders(const char* const labels[3], double (&v)[3], float lo, float hi, const char* format, float step, float fine)
    {
        bool changed = false;
        for (int a = 0; a < 3; ++a)
        {
            float f = static_cast<float>(v[a]);
            ImGui::PushID(labels[a]);
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Length (X)").x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
            const bool dragged = ImGui::SliderFloat(labels[a], &f, lo, hi, format);
            if (dragged || wheel_nudge(f, step, fine, -BoneEdit::kMaxShift * 10.0f, BoneEdit::kMaxShift * 10.0f))
            {
                v[a] = f;
                changed = true;
            }
            ImGui::PopID();
        }
        return changed;
    }

    bool PanelView::turn_sliders(BoneEdit& s)
    {
        static const char* const labels[3] = {"Roll (X)", "Pitch (Y)", "Yaw (Z)"};
        return triple_sliders(labels, s.turn, -180.0f, 180.0f, "%.1f\xC2\xB0", 1.0f, 0.1f);
    }

    bool PanelView::shift_sliders(BoneEdit& s)
    {
        static const char* const labels[3] = {"Along (X)", "Side (Y)", "Up (Z)"};
        return triple_sliders(labels, s.shift, -20.0f, 20.0f, "%.2f cm", 0.1f, 0.01f);
    }

    void PanelView::tooltip(const char* text)
    {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && text && *text)
        {
            ImGui::SetTooltip("%s", text);
        }
    }

    void PanelView::draw()
    {
        sync();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("##uuepbs", nullptr, flags);

        draw_header();
        if (ImGui::BeginTabBar("##tabs"))
        {
            auto tab_flags = [this](Tab tab) {
                return m_focus_tab == static_cast<int>(tab) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            if (ImGui::BeginTabItem("Body", nullptr, tab_flags(Tab::Body)))
            {
                draw_body();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Bones", nullptr, tab_flags(Tab::Bones)))
            {
                draw_bones();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Presets", nullptr, tab_flags(Tab::Presets)))
            {
                draw_presets();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Status", nullptr, tab_flags(Tab::Status)))
            {
                draw_status();
                ImGui::EndTabItem();
            }
            m_focus_tab = -1;
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void PanelView::draw_header()
    {
        const float base = ImGui::GetStyle().FontSizeBase;
        ImGui::PushFont(nullptr, base * 1.18f);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted("UUEPBS");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "%s", m_version.c_str());
        ImGui::SameLine();
        ImGui::PushFont(nullptr, base * 0.85f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(kMuted.x, kMuted.y, kMuted.z, 0.70f), "by XTGMods");
        ImGui::PopFont();

        bool on = Registry::instance().enabled();
        const float box = ImGui::GetFrameHeight() + ImGui::CalcTextSize("Enabled").x + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - box)); // right-align
        if (ImGui::Checkbox("Enabled", &on))
        {
            Registry::instance().set_enabled(on);
        }

        draw_target_picker();

        size_t live = 0;
        for (const auto& r : m_rigs)
        {
            live += (!r.stale && r.frames > 0) ? 1 : 0;
        }
        if (m_skeleton.names.empty())
        {
            ImGui::TextColored(kWarn, "No character found yet. Pick one above or use Status > Rescan.");
        }
        else if (!m_host->hook_active())
        {
            ImGui::TextColored(kWarn, "Found %s - waiting for the pose hook (see Status)", m_skeleton.owner.empty() ? m_skeleton.label.c_str() : m_skeleton.owner.c_str());
        }
        else if (live == 0)
        {
            ImGui::TextColored(kWarn, "Tracking %zu mesh%s - waiting for an animated frame", m_rigs.size(), m_rigs.size() == 1 ? "" : "es");
        }
        else
        {
            ImGui::TextColored(kGood, "Live on %zu mesh%s", live, live == 1 ? "" : "es");
            ImGui::SameLine();
            ImGui::TextColored(kMuted, "- %s, %zu bones", m_skeleton.label.c_str(), m_skeleton.names.size());
        }

        std::string message;
        {
            std::lock_guard guard(m_message_lock);
            if (Clock::now() - m_message_time < std::chrono::seconds(6))
            {
                message = m_message;
            }
        }
        if (!message.empty())
        {
            ImGui::TextColored(kAccent, "%s", message.c_str());
        }
        ImGui::Spacing();
    }

    void PanelView::draw_target_picker()
    {
        if (!m_targets)
        {
            return;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Character");
        ImGui::SameLine();
        const float refresh_w = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - refresh_w - ImGui::GetStyle().ItemSpacing.x));
        const std::string shown = m_target.label.empty() ? std::string("(searching...)") : m_target.label;
        if (ImGui::BeginCombo("##target", shown.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("Automatic (player / game default)", m_target.id == "auto"))
            {
                m_targets->pick_target("auto");
                set_message("Switching to the default character");
            }
            if (!m_target_list.empty())
            {
                ImGui::Separator();
            }
            for (const TargetChoice& t : m_target_list)
            {
                ImGui::PushID(t.id.c_str());
                if (ImGui::Selectable(t.label.c_str(), t.id == m_target.id))
                {
                    m_targets->pick_target(t.id);
                    set_message("Switching to " + t.label);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemClicked())
        {
            m_targets->refresh_targets();
        }
        tooltip("Whose body the sliders edit. The list shows characters currently loaded in the level.");
        ImGui::SameLine();
        if (ImGui::Button("Refresh"))
        {
            m_targets->refresh_targets();
            m_targets->request_rescan();
            set_message("Looking for characters...");
        }
        {
            const std::string key = m_targets->refresh_hotkey_text();
            const std::string tip = "Rescan the character and its meshes, relist the characters and retry the pose hook." +
                                    (key.empty() ? std::string() : "\nHotkey: " + key + " (works in game too).");
            tooltip(tip.c_str());
        }
    }

    void PanelView::draw_body()
    {
        ImGui::BeginChild("##body", ImVec2(0, 0), ImGuiChildFlags_None);
        if (m_skeleton.names.empty())
        {
            ImGui::TextWrapped("The sliders appear once a character's skeletal mesh has been found. Load a save or enter a level, or pick a character above.");
            ImGui::EndChild();
            return;
        }

        if (m_body.groups.empty())
        {
            ImGui::TextWrapped("This skeleton uses bone names the Body tab does not recognise. Every bone can still be edited in the Bones tab.");
        }
        float label_w = ImGui::CalcTextSize("Upper arms").x;
        for (const GroupBones& gb : m_body.groups)
        {
            label_w = std::max(label_w, ImGui::CalcTextSize(gb.group.title.c_str()).x);
        }
        label_w = std::min(label_w, ImGui::GetContentRegionAvail().x * 0.35f) + ImGui::GetStyle().ItemSpacing.x * 2;
        std::string section;
        bool first_section = true;
        for (const GroupBones& gb : m_body.groups)
        {
            const BodyGroup& g = gb.group;
            const std::string& first = gb.bones.front();
            if (first_section || section != g.section)
            {
                first_section = false;
                section = g.section;
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                ImGui::SeparatorText(section.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::PushID(g.title.c_str());
            BoneEdit s = scale_of(first, g.spread);
            const bool edited = s.has_scale();

            ImGui::AlignTextToFramePadding();
            if (edited)
            {
                ImGui::TextColored(kAccent, "%s", g.title.c_str());
            }
            else
            {
                ImGui::TextUnformatted(g.title.c_str());
            }
            {
                std::string tip = g.hint;
                tip += "\nBones:";
                for (const std::string& b : gb.bones)
                {
                    tip += " " + b;
                }
                tooltip(tip.c_str());
            }

            bool& open = m_group_open[g.title];
            const float buttons = ImGui::CalcTextSize("XYZ").x + ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 4 +
                                  ImGui::GetStyle().ItemSpacing.x * 2;
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - buttons));
            if (size_slider("##size", s, kScaleMin, kScaleMax))
            {
                write_group(gb, s.axis);
            }
            ImGui::SameLine();
            const bool was_open = open;
            if (was_open)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
            }
            if (ImGui::Button("XYZ"))
            {
                open = !open;
            }
            if (was_open)
            {
                ImGui::PopStyleColor();
            }
            tooltip("Edit length / width / depth separately");
            ImGui::SameLine();
            ImGui::BeginDisabled(!edited);
            if (ImGui::Button("Reset"))
            {
                const double one[3] = {1.0, 1.0, 1.0};
                write_group(gb, one);
            }
            ImGui::EndDisabled();

            if (open)
            {
                ImGui::Indent(label_w);
                if (axis_sliders(s))
                {
                    write_group(gb, s.axis);
                }
                ImGui::Unindent(label_w);
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "Ctrl+click a slider to type a value. Ctrl+wheel nudges it (add Shift for finer steps).");
        ImGui::EndChild();
    }

    void PanelView::draw_bones()
    {
        if (m_skeleton.names.empty())
        {
            ImGui::TextWrapped("No bones yet - no character mesh has been found.");
            return;
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Edited only").x - ImGui::GetFrameHeight() -
                                ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::InputTextWithHint("##find", "Search bones...", m_filter, sizeof(m_filter));
        ImGui::SameLine();
        ImGui::Checkbox("Edited only", &m_only_edited);

        const std::string needle = fold_case(m_filter);
        const float list_h = std::max(140.0f, ImGui::GetContentRegionAvail().y * 0.42f);
        ImGui::BeginChild("##bonelist", ImVec2(0, list_h), ImGuiChildFlags_Borders);
        const float indent = ImGui::GetFontSize() * 0.75f;
        for (size_t i = 0; i < m_skeleton.names.size(); ++i)
        {
            const std::string& name = m_skeleton.names[i];
            const auto it = m_book.find(fold_case(name));
            const bool edited = it != m_book.end() && !it->second.edit.is_neutral();
            if (m_only_edited && !edited)
            {
                continue;
            }
            if (!needle.empty() && fold_case(name).find(needle) == std::string::npos)
            {
                continue;
            }
            if (needle.empty() && !m_only_edited)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent * static_cast<float>(m_skeleton.depth[i]));
            }
            char label[256];
            const auto grp = m_bone_group.find(fold_case(name));
            const std::string part = grp != m_bone_group.end() ? "   [" + grp->second + "]" : std::string();
            if (edited)
            {
                const BoneEdit& e = it->second.edit;
                std::string tags;
                if (e.has_scale())
                {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "  %.2f/%.2f/%.2f", e.axis[0], e.axis[1], e.axis[2]);
                    tags += buf;
                }
                if (e.has_turn())
                {
                    tags += "  rot";
                }
                if (e.has_shift())
                {
                    tags += "  move";
                }
                std::snprintf(label, sizeof(label), "%s%s %s##%zu", name.c_str(), part.c_str(), tags.c_str(), i);
                ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            }
            else
            {
                std::snprintf(label, sizeof(label), "%s%s##%zu", name.c_str(), part.c_str(), i);
            }
            if (ImGui::Selectable(label, fold_case(m_selected) == fold_case(name)))
            {
                m_selected = name;
            }
            if (edited)
            {
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (m_selected.empty() || !has_bone(m_selected))
        {
            ImGui::TextColored(kMuted, "Pick a bone above to edit it.");
            return;
        }

        const std::string mirror = mirror_bone_name(m_selected);
        const bool can_mirror = !mirror.empty() && has_bone(mirror);
        ImGui::TextColored(kAccent, "%s", m_selected.c_str());
        if (can_mirror && m_mirror)
        {
            ImGui::SameLine();
            ImGui::TextColored(kMuted, "+ %s", mirror.c_str());
        }

        BoneEdit s = scale_of(m_selected, Spread::Chain);
        bool changed = false;
        ImGui::BeginChild("##editor", ImVec2(0, 0), ImGuiChildFlags_None);
        if (ImGui::CollapsingHeader(s.has_scale() ? "Scale  *###scale" : "Scale###scale", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Length (X)").x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
            changed |= size_slider("Size", s, kScaleMin, kScaleMax);
            changed |= axis_sliders(s);
            if (s.has_scale() && ImGui::SmallButton("Reset scale"))
            {
                s.axis[0] = s.axis[1] = s.axis[2] = 1.0;
                changed = true;
            }
        }
        if (ImGui::CollapsingHeader(s.has_turn() ? "Rotate  *###rotate" : "Rotate###rotate", s.has_turn() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            changed |= turn_sliders(s);
            if (s.has_turn() && ImGui::SmallButton("Reset rotation"))
            {
                s.turn[0] = s.turn[1] = s.turn[2] = 0.0;
                changed = true;
            }
        }
        if (ImGui::CollapsingHeader(s.has_shift() ? "Move  *###move" : "Move###move", s.has_shift() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            changed |= shift_sliders(s);
            if (s.has_shift() && ImGui::SmallButton("Reset move"))
            {
                s.shift[0] = s.shift[1] = s.shift[2] = 0.0;
                changed = true;
            }
        }
        ImGui::Spacing();

        int spread = static_cast<int>(s.spread);
        const char* spreads[3] = {spread_label(Spread::Chain), spread_label(Spread::Keep), spread_label(Spread::Solo)};
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Length (X)").x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
        if (ImGui::Combo("Children", &spread, spreads, 3))
        {
            s.spread = static_cast<Spread>(spread);
            changed = true; // remembered as a hint even while the bone is still at 1.0x
        }
        tooltip("Scale children too: children follow the bone's scale, rotation and move.\n"
                "Keep children size: children follow rotation/move and the new shape but keep their own size.\n"
                "This bone only: nothing below it changes (rotation/move included).");

        if (can_mirror)
        {
            ImGui::Checkbox("Mirror to other side", &m_mirror);
            ImGui::SameLine();
        }
        if (ImGui::Button("Reset bone"))
        {
            BoneEdit neutral;
            neutral.spread = s.spread;
            s = neutral;
            changed = true;
        }

        ImGui::TextColored(kMuted, "Ctrl+click to type a value. Ctrl+wheel nudges (Shift: finer).");
        ImGui::EndChild();

        if (changed)
        {
            write(m_selected, s);
            if (can_mirror && m_mirror)
            {
                write(mirror, Registry::instance().mirror_edit(m_selected, s)); // mirror-image rotation/move, measured from the skeleton
            }
        }
    }

    void PanelView::refresh_presets()
    {
        if (m_shelf)
        {
            m_presets = m_shelf->list();
        }
        m_presets_time = Clock::now();
    }

    void PanelView::load_preset(const std::string& name)
    {
        EditBook book;
        std::string message;
        if (m_shelf && m_shelf->load(name, book, message))
        {
            Registry::instance().replace_edits(std::move(book));
            std::snprintf(m_preset_name, sizeof(m_preset_name), "%s", name.c_str());
        }
        set_message(message);
    }

    void PanelView::save_preset(const std::string& name)
    {
        std::string message;
        if (m_shelf)
        {
            m_shelf->save(name, Registry::instance().edits(), message);
        }
        set_message(message);
        refresh_presets();
    }

    void PanelView::draw_presets()
    {
        if (!m_shelf || !m_shelf->ready())
        {
            ImGui::TextColored(kWarn, "Preset folder is not set up yet.");
            return;
        }
        if (Clock::now() - m_presets_time > std::chrono::seconds(3))
        {
            refresh_presets();
        }

        ImGui::SeparatorText("Save current sliders");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Save").x - ImGui::GetStyle().FramePadding.x * 2 -
                                ImGui::GetStyle().ItemSpacing.x);
        const bool enter =
            ImGui::InputTextWithHint("##name", "Preset name", m_preset_name, sizeof(m_preset_name), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const std::string clean = PresetShelf::clean_name(m_preset_name);
        ImGui::BeginDisabled(clean.empty());
        if (ImGui::Button("Save") || (enter && !clean.empty()))
        {
            if (m_shelf->exists(clean))
            {
                m_pending = clean;
                ImGui::OpenPopup("Overwrite preset?");
            }
            else
            {
                save_preset(clean);
            }
        }
        ImGui::EndDisabled();

        if (ImGui::BeginPopupModal("Overwrite preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("'%s' already exists. Replace it?", m_pending.c_str());
            if (ImGui::Button("Replace"))
            {
                save_preset(m_pending);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SeparatorText("Saved presets");
        const float list_h = std::max(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.5f);
        ImGui::BeginChild("##presetlist", ImVec2(0, list_h), ImGuiChildFlags_Borders);
        if (m_presets.empty())
        {
            ImGui::TextColored(kMuted, "No presets yet.");
        }
        for (const std::string& name : m_presets)
        {
            if (ImGui::Selectable(name.c_str(), m_picked == name, ImGuiSelectableFlags_AllowDoubleClick))
            {
                m_picked = name;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    load_preset(name);
                }
            }
        }
        ImGui::EndChild();

        const bool picked = !m_picked.empty() && std::find(m_presets.begin(), m_presets.end(), m_picked) != m_presets.end();
        ImGui::BeginDisabled(!picked);
        if (ImGui::Button("Load"))
        {
            load_preset(m_picked);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
        {
            m_pending = m_picked;
            ImGui::OpenPopup("Delete preset?");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Open folder"))
        {
            m_host->open_folder(m_shelf->folder());
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset all"))
        {
            ImGui::OpenPopup("Reset all sliders?");
        }

        if (ImGui::BeginPopupModal("Delete preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Delete '%s' from disk?", m_pending.c_str());
            if (ImGui::Button("Delete"))
            {
                std::string message;
                m_shelf->remove(m_pending, message);
                set_message(message);
                m_picked.clear();
                refresh_presets();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("Reset all sliders?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Set every bone back to 1.0x?");
            if (ImGui::Button("Reset"))
            {
                Registry::instance().clear_edits();
                set_message("All sliders reset");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kMuted, "Folder: %s", path_to_utf8(m_shelf->folder()).c_str());
        ImGui::PopTextWrapPos();
    }

    void PanelView::draw_status()
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(m_host->hook_active() ? kGood : kWarn, "%s", m_host->hook_text().c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        if (ImGui::Button("Rescan"))
        {
            if (m_targets)
            {
                m_targets->request_rescan();
            }
            else
            {
                Registry::instance().request_rescan();
            }
            set_message("Rescan requested");
        }
        ImGui::SameLine();
        if (m_targets && ImGui::Button("Write diagnostics"))
        {
            set_message("Diagnostics written to " + m_targets->write_diagnostics());
        }
        tooltip("Writes what the hook finder sees into the log file next to the DLL.\nSend that file along with bug reports.");
        ImGui::SameLine();
        if (ImGui::Checkbox("Keep this window on top", &m_topmost))
        {
            m_host->set_topmost(m_topmost);
        }

        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        if (m_targets)
        {
            ImGui::TextColored(kMuted, "Lua link: %s", m_targets->link_text().c_str());
        }
        ImGui::TextColored(kMuted, "Mirroring: %s", m_mirror_text.c_str());
        ImGui::TextColored(kMuted, "Body tab: %zu group(s)%s; %s", m_body.groups.size(),
                           m_body.style.empty() ? "" : (", " + m_body.style + " rig").c_str(), m_body_status.c_str());
        const std::string renderer = m_host->renderer_text();
        if (!renderer.empty())
        {
            ImGui::TextColored(kMuted, "Window drawing: %s", renderer.c_str());
        }
        ImGui::PopTextWrapPos();

        ImGui::SeparatorText("Tracked meshes");
        if (ImGui::BeginTable("##rigs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Component");
            ImGui::TableSetupColumn("Bones");
            ImGui::TableSetupColumn("Frames seen");
            ImGui::TableHeadersRow();
            for (const auto& r : m_rigs)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(r.stale ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s%s%s", r.label.c_str(), r.primary ? " (bones listed)" : "",
                                   r.stale ? " (stale)" : "");
                if (!r.owner.empty() && ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", r.owner.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d", r.bones);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(r.frames));
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        const std::string key = m_targets ? m_targets->hotkey_text() : std::string("F6");
        ImGui::TextColored(kMuted, "%s opens/closes this window (in game or here). Changes are visual only: collision, "
                                   "physics bodies and sockets keep their original size. While the game is paused the "
                                   "pose updates on the next animated frame.",
                           key.c_str());
        ImGui::PopTextWrapPos();
    }
} // namespace uuepbs::ui
