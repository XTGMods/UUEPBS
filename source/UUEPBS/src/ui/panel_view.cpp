#include "panel_view.hpp"

#include "../core/rig_names.hpp"
#include "skin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace uuepbs::ui
{
    namespace
    {
        // Colours come from the active skin (the built-in look when there is none).
        const ImVec4& accent() { return skin().color(Role::Accent); }
        const ImVec4& accent_dim() { return skin().color(Role::AccentDim); }
        const ImVec4& good() { return skin().color(Role::Good); }
        const ImVec4& warn() { return skin().color(Role::Warn); }
        const ImVec4& muted() { return skin().color(Role::Muted); }

        // Log scale from 0.1x to 10x puts 1.0x exactly in the middle of the slider.
        constexpr float kScaleMin = 0.10f;
        constexpr float kScaleMax = 10.0f;
        // Morph sliders show 0..1; Ctrl+click can type anything in kMorphMin..kMorphMax.
        constexpr float kMorphSliderMin = 0.0f;
        constexpr float kMorphSliderMax = 1.0f;

        bool contains_folded(const std::string& haystack, const std::string& folded_needle)
        {
            return folded_needle.empty() || fold_case(haystack).find(folded_needle) != std::string::npos;
        }
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
        skin().apply_style(dpi);
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

        // Morph targets: names from the meshes, minus the profile's ExcludeMorphs.
        const uint64_t profile_rev = MorphProfileSource::instance().revision();
        if (reg.morph_names_revision() != m_morph_names_revision || profile_rev != m_morph_profile_revision)
        {
            MorphProfileSource::instance().get(m_morph_profile, m_morph_status);
            m_morph_profile_revision = profile_rev;
            const std::vector<std::string> all = reg.morph_names(&m_morph_names_revision);
            m_morph_names.clear();
            m_morph_keys.clear();
            for (const std::string& m : all)
            {
                if (!m_morph_profile.excluded(m))
                {
                    m_morph_names.push_back(m);
                    m_morph_keys.insert(fold_case(m));
                }
            }
            m_morphs_hidden = all.size() - m_morph_names.size();
            m_morph_groups = resolve_morph_groups(m_morph_names, m_morph_profile);
        }
        if (reg.morph_revision() != m_morph_revision)
        {
            m_morphs = reg.morphs(&m_morph_revision);
        }

        const auto now = Clock::now();
        if (now - m_rigs_time > std::chrono::milliseconds(500))
        {
            m_rigs = reg.rigs();
            m_rigs_time = now;
            m_mirror_text = reg.mirror_source();
            m_animated = reg.animated_morphs();
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

    bool PanelView::has_morph(const std::string& morph) const
    {
        return m_morph_keys.count(fold_case(morph)) > 0;
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

    // Simplified Panel sliders only change scale; each bone keeps its own rotation/move.
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

    std::optional<double> PanelView::morph_of(const std::string& morph) const
    {
        const auto it = m_morphs.find(fold_case(morph));
        if (it == m_morphs.end())
        {
            return std::nullopt;
        }
        return it->second.weight;
    }

    // nullopt gives the morph back to the game.
    void PanelView::write_morph(const std::string& morph, std::optional<double> weight)
    {
        Registry& reg = Registry::instance();
        const std::string key = fold_case(morph);
        if (weight)
        {
            const double w = clamp_morph(*weight);
            reg.set_morph(morph, w);
            m_morphs[key] = MorphEntry{morph, w};
        }
        else
        {
            reg.clear_morph(morph);
            m_morphs.erase(key);
        }
        m_morph_revision = reg.morph_revision();
    }

    bool PanelView::size_slider(const char* id, BoneEdit& s, float lo, float hi)
    {
        const double avg = (s.axis[0] + s.axis[1] + s.axis[2]) / 3.0;
        float v = static_cast<float>(avg);
        const bool dragged = skin().slider_float(id, &v, lo, hi, "%.3fx", ImGuiSliderFlags_Logarithmic);
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
            if (skin().slider_float(names[a], &v, kScaleMin, kScaleMax, "%.3fx", ImGuiSliderFlags_Logarithmic))
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
            const bool dragged = skin().slider_float(labels[a], &f, lo, hi, format);
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

    bool PanelView::morph_slider(const char* id, float& v)
    {
        const bool dragged = skin().slider_float(id, &v, kMorphSliderMin, kMorphSliderMax, "%.2f");
        const bool nudged = wheel_nudge(v, 0.05f, 0.01f, static_cast<float>(kMorphMin), static_cast<float>(kMorphMax));
        v = static_cast<float>(clamp_morph(v)); // typed values (Ctrl+click) may leave 0..1
        return dragged || nudged;
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
        Skin& sk = skin();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (sk.font())
        {
            ImGui::PushFont(sk.font(), 0.0f);
        }
        ImGui::Begin("##uuepbs", nullptr, flags);
        sk.draw_window_background();

        draw_header();
        if (ImGui::BeginTabBar("##tabs"))
        {
            auto tab_flags = [this](Tab tab) {
                return m_focus_tab == static_cast<int>(tab) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            if (sk.begin_tab_item("Simplified Panel###simplified", "simplified", tab_flags(Tab::Simplified)))
            {
                draw_simplified();
                ImGui::EndTabItem();
            }
            if (sk.begin_tab_item("Detailed Panel###detailed", "detailed", tab_flags(Tab::Detailed)))
            {
                draw_detailed();
                ImGui::EndTabItem();
            }
            if (sk.begin_tab_item("Presets###presets", "presets", tab_flags(Tab::Presets)))
            {
                draw_presets();
                ImGui::EndTabItem();
            }
            if (sk.begin_tab_item("Status###status", "status", tab_flags(Tab::Status)))
            {
                draw_status();
                ImGui::EndTabItem();
            }
            m_focus_tab = -1;
            ImGui::EndTabBar();
        }
        ImGui::End();
        if (sk.font())
        {
            ImGui::PopFont();
        }
    }

    void PanelView::draw_header()
    {
        Skin& sk = skin();
        const float base = ImGui::GetStyle().FontSizeBase;
        sk.draw_header_strip(std::max(ImGui::GetFontSize() * 1.18f, ImGui::GetFrameHeight()));
        if (!sk.logo(ImGui::GetFontSize()))
        {
            ImGui::PushFont(nullptr, base * 1.18f);
            ImGui::PushStyleColor(ImGuiCol_Text, sk.color(Role::Title));
            ImGui::TextUnformatted("UUEPBS");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(muted(), "%s", m_version.c_str());
        ImGui::SameLine();
        ImGui::PushFont(nullptr, base * 0.85f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(muted().x, muted().y, muted().z, 0.70f), "by XTGMods");
        ImGui::PopFont();

        bool on = Registry::instance().enabled();
        const float box = ImGui::GetFrameHeight() + ImGui::CalcTextSize("Enabled").x + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - box)); // right-align
        if (sk.checkbox("Enabled", &on))
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
            ImGui::TextColored(warn(), "No character found yet. Pick one above or use Status > Rescan.");
        }
        else if (!m_host->hook_active())
        {
            ImGui::TextColored(warn(), "Found %s - waiting for the pose hook (see Status)", m_skeleton.owner.empty() ? m_skeleton.label.c_str() : m_skeleton.owner.c_str());
        }
        else if (live == 0)
        {
            ImGui::TextColored(warn(), "Tracking %zu mesh%s - waiting for an animated frame", m_rigs.size(), m_rigs.size() == 1 ? "" : "es");
        }
        else
        {
            ImGui::TextColored(good(), "Live on %zu mesh%s", live, live == 1 ? "" : "es");
            ImGui::SameLine();
            if (m_morph_names.empty())
            {
                ImGui::TextColored(muted(), "- %s, %zu bones", m_skeleton.label.c_str(), m_skeleton.names.size());
            }
            else
            {
                ImGui::TextColored(muted(), "- %s, %zu bones, %zu morphs", m_skeleton.label.c_str(), m_skeleton.names.size(), m_morph_names.size());
            }
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
            ImGui::TextColored(accent(), "%s", message.c_str());
        }
        ImGui::Spacing();
    }

    void PanelView::draw_target_picker()
    {
        if (!m_targets)
        {
            return;
        }
        Skin& sk = skin();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Character");
        ImGui::SameLine();
        const float refresh_w = sk.button_width("Refresh", "refresh");
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
        if (sk.button("Refresh", "refresh"))
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

    void PanelView::draw_simplified()
    {
        Skin& sk = skin();
        sk.begin_child("##simplified", ImVec2(0, 0), ImGuiChildFlags_None);
        if (m_skeleton.names.empty())
        {
            ImGui::TextWrapped("The sliders appear once a character's skeletal mesh has been found. Load a save or enter a level, or pick a character above.");
            sk.end_child();
            return;
        }

        if (m_body.groups.empty())
        {
            ImGui::TextWrapped("This skeleton uses bone names the Simplified Panel does not recognise. Every bone can still be edited in the Detailed Panel.");
        }
        const float icon_w = ImGui::GetFontSize();
        const float icon_room = icon_w + ImGui::GetStyle().ItemSpacing.x;
        float label_w = ImGui::CalcTextSize("Upper arms").x;
        for (const GroupBones& gb : m_body.groups)
        {
            label_w = std::max(label_w, ImGui::CalcTextSize(gb.group.title.c_str()).x + (sk.has_icon("group." + gb.group.title) ? icon_room : 0.0f));
        }
        for (const MorphGroupMorphs& mg : m_morph_groups)
        {
            label_w = std::max(label_w, ImGui::CalcTextSize(mg.group.title.c_str()).x + (sk.has_icon("group." + mg.group.title) ? icon_room : 0.0f));
        }
        label_w = std::min(label_w, ImGui::GetContentRegionAvail().x * 0.35f) + ImGui::GetStyle().ItemSpacing.x * 2;
        const float buttons = sk.button_width("XYZ", "xyz") + sk.button_width("Reset", "reset") + ImGui::GetStyle().ItemSpacing.x * 2;
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
                ImGui::PushStyleColor(ImGuiCol_Text, muted());
                ImGui::SeparatorText(section.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::PushID(g.title.c_str());
            BoneEdit s = scale_of(first, g.spread);
            const bool edited = s.has_scale();

            sk.icon("group." + g.title, icon_w);
            ImGui::AlignTextToFramePadding();
            if (edited)
            {
                ImGui::TextColored(accent(), "%s", g.title.c_str());
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
                ImGui::PushStyleColor(ImGuiCol_Button, accent_dim());
            }
            if (sk.button("XYZ", "xyz"))
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
            if (sk.button("Reset", "reset"))
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
        draw_simplified_morphs(label_w);
        ImGui::Spacing();
        ImGui::TextColored(muted(), "Ctrl+click a slider to type a value. Ctrl+wheel nudges it (add Shift for finer steps).");
        sk.end_child();
    }

    // "MorphGroups" from the game profile: one slider per group, driving every morph in it.
    void PanelView::draw_simplified_morphs(float label_w)
    {
        Skin& sk = skin();
        if (m_morph_groups.empty())
        {
            if (!m_morph_names.empty())
            {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(muted(), "%zu morph target%s: see Detailed Panel > Morphs. Add \"MorphGroups\" to the game profile to put sliders here.",
                                   m_morph_names.size(), m_morph_names.size() == 1 ? "" : "s");
                ImGui::PopTextWrapPos();
            }
            return;
        }
        const float icon_w = ImGui::GetFontSize();
        const float reset_w = sk.button_width("Reset", "reset") + ImGui::GetStyle().ItemSpacing.x;
        std::string section;
        bool first = true;
        for (const MorphGroupMorphs& mg : m_morph_groups)
        {
            const std::string sec = mg.group.section.empty() ? std::string("Morphs") : mg.group.section;
            if (first || sec != section)
            {
                first = false;
                section = sec;
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, muted());
                ImGui::SeparatorText(section.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PushID(("morphgroup:" + mg.group.title).c_str());
            std::optional<double> current;
            for (const std::string& m : mg.morphs)
            {
                if (const auto w = morph_of(m))
                {
                    current = w;
                    break;
                }
            }
            const bool edited = current.has_value();
            sk.icon("group." + mg.group.title, icon_w);
            ImGui::AlignTextToFramePadding();
            if (edited)
            {
                ImGui::TextColored(accent(), "%s", mg.group.title.c_str());
            }
            else
            {
                ImGui::TextUnformatted(mg.group.title.c_str());
            }
            {
                std::string tip = mg.group.hint;
                tip += tip.empty() ? "Morphs:" : "\nMorphs:";
                for (const std::string& m : mg.morphs)
                {
                    tip += " " + m;
                }
                tooltip(tip.c_str());
            }
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - reset_w));
            float v = static_cast<float>(current.value_or(0.0));
            if (morph_slider("##w", v))
            {
                for (const std::string& m : mg.morphs)
                {
                    write_morph(m, v);
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!edited);
            if (sk.button("Reset", "reset"))
            {
                for (const std::string& m : mg.morphs)
                {
                    write_morph(m, std::nullopt);
                }
            }
            ImGui::EndDisabled();
            tooltip("Give these morphs back to the game (its own value comes back).");
            ImGui::PopID();
        }
    }

    void PanelView::draw_detailed()
    {
        if (m_morph_names.empty())
        {
            draw_detailed_bones();
            return;
        }
        Skin& sk = skin();
        if (ImGui::BeginTabBar("##detail"))
        {
            auto detail_flags = [this](Detail d) {
                return m_focus_detail == static_cast<int>(d) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            char label[64];
            std::snprintf(label, sizeof(label), "Bones (%zu)###bones", m_skeleton.names.size());
            if (sk.begin_tab_item(label, "bones", detail_flags(Detail::Bones)))
            {
                draw_detailed_bones();
                ImGui::EndTabItem();
            }
            std::snprintf(label, sizeof(label), "Morphs (%zu)###morphs", m_morph_names.size());
            if (sk.begin_tab_item(label, "morphs", detail_flags(Detail::Morphs)))
            {
                draw_detailed_morphs();
                ImGui::EndTabItem();
            }
            m_focus_detail = -1;
            ImGui::EndTabBar();
        }
    }

    void PanelView::draw_detailed_bones()
    {
        Skin& sk = skin();
        if (m_skeleton.names.empty())
        {
            ImGui::TextWrapped("No bones yet - no character mesh has been found.");
            return;
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Edited only").x - ImGui::GetFrameHeight() -
                                ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::InputTextWithHint("##find", "Search bones...", m_filter, sizeof(m_filter));
        ImGui::SameLine();
        sk.checkbox("Edited only", &m_only_edited);

        const std::string needle = fold_case(m_filter);
        const float list_h = std::max(140.0f, ImGui::GetContentRegionAvail().y * 0.42f);
        sk.begin_child("##bonelist", ImVec2(0, list_h), ImGuiChildFlags_Borders);
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
                ImGui::PushStyleColor(ImGuiCol_Text, accent());
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
        sk.end_child();

        ImGui::Spacing();
        if (m_selected.empty() || !has_bone(m_selected))
        {
            ImGui::TextColored(muted(), "Pick a bone above to edit it.");
            return;
        }

        const std::string mirror = mirror_bone_name(m_selected);
        const bool can_mirror = !mirror.empty() && has_bone(mirror);
        ImGui::TextColored(accent(), "%s", m_selected.c_str());
        if (can_mirror && m_mirror)
        {
            ImGui::SameLine();
            ImGui::TextColored(muted(), "+ %s", mirror.c_str());
        }

        BoneEdit s = scale_of(m_selected, Spread::Chain);
        bool changed = false;
        sk.begin_child("##editor", ImVec2(0, 0), ImGuiChildFlags_None);
        if (ImGui::CollapsingHeader(s.has_scale() ? "Scale  *###scale" : "Scale###scale", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Length (X)").x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
            changed |= size_slider("Size", s, kScaleMin, kScaleMax);
            changed |= axis_sliders(s);
            if (s.has_scale() && sk.small_button("Reset scale"))
            {
                s.axis[0] = s.axis[1] = s.axis[2] = 1.0;
                changed = true;
            }
        }
        if (ImGui::CollapsingHeader(s.has_turn() ? "Rotate  *###rotate" : "Rotate###rotate", s.has_turn() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            changed |= turn_sliders(s);
            if (s.has_turn() && sk.small_button("Reset rotation"))
            {
                s.turn[0] = s.turn[1] = s.turn[2] = 0.0;
                changed = true;
            }
        }
        if (ImGui::CollapsingHeader(s.has_shift() ? "Move  *###move" : "Move###move", s.has_shift() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            changed |= shift_sliders(s);
            if (s.has_shift() && sk.small_button("Reset move"))
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
            sk.checkbox("Mirror to other side", &m_mirror);
            ImGui::SameLine();
        }
        if (sk.button("Reset bone", "reset"))
        {
            BoneEdit neutral;
            neutral.spread = s.spread;
            s = neutral;
            changed = true;
        }

        ImGui::TextColored(muted(), "Ctrl+click to type a value. Ctrl+wheel nudges (Shift: finer).");
        sk.end_child();

        if (changed)
        {
            write(m_selected, s);
            if (can_mirror && m_mirror)
            {
                write(mirror, Registry::instance().mirror_edit(m_selected, s)); // mirror-image rotation/move, measured from the skeleton
            }
        }
    }

    void PanelView::draw_detailed_morphs()
    {
        Skin& sk = skin();
        const float reset_w = sk.button_width("Reset", "reset");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Edited only").x - ImGui::GetFrameHeight() -
                                ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::InputTextWithHint("##findmorph", "Search morphs...", m_morph_filter, sizeof(m_morph_filter));
        ImGui::SameLine();
        sk.checkbox("Edited only", &m_morph_only_edited);

        const std::string needle = fold_case(m_morph_filter);
        std::vector<const std::string*> rows;
        rows.reserve(m_morph_names.size());
        for (const std::string& m : m_morph_names)
        {
            if (m_morph_only_edited && !morph_of(m))
            {
                continue;
            }
            if (!contains_folded(m, needle))
            {
                continue;
            }
            rows.push_back(&m);
        }

        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
        sk.begin_child("##morphlist", ImVec2(0, std::max(140.0f, ImGui::GetContentRegionAvail().y - footer)), ImGuiChildFlags_Borders);
        float name_w = ImGui::CalcTextSize("MMMMMMMMMMMM").x;
        for (const std::string* m : rows)
        {
            name_w = std::max(name_w, ImGui::CalcTextSize(m->c_str()).x);
        }
        const float warn_w = ImGui::CalcTextSize(" !").x;
        name_w = std::min(name_w + warn_w, ImGui::GetContentRegionAvail().x * 0.45f) + ImGui::GetStyle().ItemSpacing.x;

        ImGuiListClipper clipper; // faces can have hundreds of morphs
        clipper.Begin(static_cast<int>(rows.size()), ImGui::GetFrameHeightWithSpacing());
        while (clipper.Step())
        {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
            {
                const std::string& name = *rows[static_cast<size_t>(r)];
                ImGui::PushID(name.c_str());
                const std::optional<double> w = morph_of(name);
                const bool animated = m_animated.count(fold_case(name)) > 0;
                ImGui::AlignTextToFramePadding();
                if (w)
                {
                    ImGui::TextColored(accent(), "%s", name.c_str());
                }
                else
                {
                    ImGui::TextUnformatted(name.c_str());
                }
                if (animated)
                {
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(warn(), " !");
                    tooltip("The game keeps setting this morph itself (animation or its own code),\nso the slider may not stick. UUEPBS re-applies it each time it notices.");
                }
                ImGui::SameLine(name_w);
                ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - reset_w - ImGui::GetStyle().ItemSpacing.x));
                float v = static_cast<float>(w.value_or(0.0));
                if (morph_slider("##w", v))
                {
                    write_morph(name, v);
                    const std::string other = mirror_bone_name(name);
                    if (m_mirror && !other.empty() && has_morph(other))
                    {
                        write_morph(other, v);
                    }
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!w);
                if (sk.button("Reset", "reset"))
                {
                    write_morph(name, std::nullopt);
                    const std::string other = mirror_bone_name(name);
                    if (m_mirror && !other.empty() && has_morph(other))
                    {
                        write_morph(other, std::nullopt);
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        if (rows.empty())
        {
            ImGui::TextColored(muted(), m_morph_only_edited ? "No morph is set yet." : "No morph matches the search.");
        }
        sk.end_child();

        sk.checkbox("Mirror left/right morphs", &m_mirror);
        tooltip("Moving a _L morph also moves its _R twin (and the other way round).");
        ImGui::SameLine();
        ImGui::BeginDisabled(m_morphs.empty());
        if (sk.button("Reset all morphs", "reset"))
        {
            Registry::instance().clear_morphs();
            m_morphs.clear();
            m_morph_revision = Registry::instance().morph_revision();
            set_message("All morphs given back to the game");
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(muted(), "A set morph replaces the game's value (even at 0); Reset gives it back. Ctrl+click to type values outside 0..1.");
        ImGui::PopTextWrapPos();
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
        MorphBook morphs;
        std::string message;
        if (m_shelf && m_shelf->load(name, book, message, &morphs))
        {
            Registry::instance().replace_edits(std::move(book));
            Registry::instance().replace_morphs(std::move(morphs));
            std::snprintf(m_preset_name, sizeof(m_preset_name), "%s", name.c_str());
        }
        set_message(message);
    }

    void PanelView::save_preset(const std::string& name)
    {
        std::string message;
        if (m_shelf)
        {
            const MorphBook morphs = Registry::instance().morphs();
            m_shelf->save(name, Registry::instance().edits(), message, &morphs);
        }
        set_message(message);
        refresh_presets();
    }

    void PanelView::draw_presets()
    {
        Skin& sk = skin();
        if (!m_shelf || !m_shelf->ready())
        {
            ImGui::TextColored(warn(), "Preset folder is not set up yet.");
            return;
        }
        if (Clock::now() - m_presets_time > std::chrono::seconds(3))
        {
            refresh_presets();
        }

        ImGui::SeparatorText("Save current sliders");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sk.button_width("Save", "save") - ImGui::GetStyle().ItemSpacing.x);
        const bool enter =
            ImGui::InputTextWithHint("##name", "Preset name", m_preset_name, sizeof(m_preset_name), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const std::string clean = PresetShelf::clean_name(m_preset_name);
        ImGui::BeginDisabled(clean.empty());
        if (sk.button("Save", "save") || (enter && !clean.empty()))
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
            if (sk.button("Replace"))
            {
                save_preset(m_pending);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (sk.button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SeparatorText("Saved presets");
        const float list_h = std::max(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.5f);
        sk.begin_child("##presetlist", ImVec2(0, list_h), ImGuiChildFlags_Borders);
        if (m_presets.empty())
        {
            ImGui::TextColored(muted(), "No presets yet.");
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
        sk.end_child();

        const bool picked = !m_picked.empty() && std::find(m_presets.begin(), m_presets.end(), m_picked) != m_presets.end();
        ImGui::BeginDisabled(!picked);
        if (sk.button("Load", "load"))
        {
            load_preset(m_picked);
        }
        ImGui::SameLine();
        if (sk.button("Delete", "delete"))
        {
            m_pending = m_picked;
            ImGui::OpenPopup("Delete preset?");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (sk.button("Open folder", "folder"))
        {
            m_host->open_folder(m_shelf->folder());
        }
        ImGui::SameLine();
        if (sk.button("Reset all", "reset"))
        {
            ImGui::OpenPopup("Reset all sliders?");
        }

        if (ImGui::BeginPopupModal("Delete preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Delete '%s' from disk?", m_pending.c_str());
            if (sk.button("Delete"))
            {
                std::string message;
                m_shelf->remove(m_pending, message);
                set_message(message);
                m_picked.clear();
                refresh_presets();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (sk.button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("Reset all sliders?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(m_morph_names.empty() ? "Set every bone back to 1.0x?"
                                                         : "Set every bone back to 1.0x and give every morph back to the game?");
            if (sk.button("Reset"))
            {
                Registry::instance().clear_edits();
                Registry::instance().clear_morphs();
                set_message("All sliders reset");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (sk.button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(muted(), "Folder: %s", path_to_utf8(m_shelf->folder()).c_str());
        ImGui::PopTextWrapPos();
    }

    void PanelView::draw_skin_picker()
    {
        Skin& sk = skin();
        ImGui::SeparatorText("Window skin");
        if (Clock::now() - m_skins_time > std::chrono::seconds(3))
        {
            m_skins = sk.available();
            m_skins_time = Clock::now();
        }
        const float open_w = sk.button_width("Open folder", "folder");
        const float reload_w = sk.button_width("Reload", "refresh");
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - open_w - reload_w - ImGui::GetStyle().ItemSpacing.x * 2));
        const std::string current = sk.name().empty() ? std::string(Skin::kBuiltinName) + " (built-in)" : sk.display_name();
        if (ImGui::BeginCombo("##skin", current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("Default (built-in)", sk.name().empty()))
            {
                Skin::request("");
            }
            for (const std::string& n : m_skins)
            {
                if (ImGui::Selectable(n.c_str(), n == sk.name()))
                {
                    Skin::request(n);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemClicked())
        {
            m_skins = sk.available();
        }
        tooltip("Skins live in the folder next to the presets folder: one sub-folder per skin with a skin.json\n"
                "and PNG/JPG images. See README.txt there. The active skin reloads when its files change.");
        ImGui::SameLine();
        if (sk.button("Reload", "refresh"))
        {
            Skin::request(sk.name());
            m_skins = sk.available();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(sk.folder().empty());
        if (sk.button("Open folder##skins", "folder"))
        {
            m_host->open_folder(sk.folder());
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(muted(), "Skin: %s", sk.status().c_str());
        for (const std::string& p : sk.problems())
        {
            ImGui::TextColored(warn(), "  %s", p.c_str());
        }
        ImGui::PopTextWrapPos();
    }

    void PanelView::draw_status()
    {
        Skin& sk = skin();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(m_host->hook_active() ? good() : warn(), "%s", m_host->hook_text().c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        if (sk.button("Rescan", "refresh"))
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
        if (m_targets && sk.button("Write diagnostics"))
        {
            set_message("Diagnostics written to " + m_targets->write_diagnostics());
        }
        tooltip("Writes what the hook finder sees into the log file next to the DLL.\nSend that file along with bug reports.");
        ImGui::SameLine();
        if (sk.checkbox("Keep this window on top", &m_topmost))
        {
            m_host->set_topmost(m_topmost);
        }

        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        if (m_targets)
        {
            ImGui::TextColored(muted(), "Lua link: %s", m_targets->link_text().c_str());
        }
        ImGui::TextColored(muted(), "Mirroring: %s", m_mirror_text.c_str());
        ImGui::TextColored(muted(), "Simplified Panel: %zu group(s)%s; %s", m_body.groups.size(),
                           m_body.style.empty() ? "" : (", " + m_body.style + " rig").c_str(), m_body_status.c_str());
        ImGui::TextColored(muted(), "Morphs: %zu on the meshes%s, %zu set; game profile: %s", m_morph_names.size(),
                           m_morphs_hidden ? (" (+" + std::to_string(m_morphs_hidden) + " hidden by ExcludeMorphs)").c_str() : "", m_morphs.size(),
                           m_morph_status.c_str());
        const std::string renderer = m_host->renderer_text();
        if (!renderer.empty())
        {
            ImGui::TextColored(muted(), "Window drawing: %s", renderer.c_str());
        }
        ImGui::PopTextWrapPos();

        draw_skin_picker();

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
                ImGui::TextColored(r.stale ? warn() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s%s%s", r.label.c_str(), r.primary ? " (bones listed)" : "",
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
        ImGui::TextColored(muted(), "%s opens/closes this window (in game or here). Changes are visual only: collision, "
                                    "physics bodies and sockets keep their original size. While the game is paused the "
                                    "pose updates on the next animated frame.",
                           key.c_str());
        ImGui::PopTextWrapPos();
    }
} // namespace uuepbs::ui
