#include <stdafx.hpp>
#include <render/presentation.hpp>
#include <platform/windows/input.hpp>
#include <scripting/runtime.hpp>
#include <config/first_run.hpp>
#include <game/input/bindings.hpp>
#include <config/hotkeys.hpp>
#include <features/visuals/visuals.hpp>
#include <features/visuals/hitsound.hpp>
#include <features/chams/preview.hpp>
#include <features/chams/renderer.hpp>
#include <render/text/localization.hpp>
#include <ui/menu/menu.hpp>
#include <ui/overlay/input.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <ui/menu/internal.hpp>
#include <ui/overlay/overlay.hpp>

namespace ui
{
using namespace ui::menu::detail;

bool menu_t::initialize(HWND hwnd)
{
    m_hwnd = hwnd;
    return true;
}

void menu_t::map_pointer_to_layout(float& x, float& y, const float display_width,
                                   const float display_height) const noexcept
{
    if (!is_open())
        return;
    const auto scale = current_menu_scale(display_width, display_height);
    if (std::abs(scale - 1.0f) < 0.0001f)
        return;
    const auto origin = menu_transform_origin(display_width, display_height);
    x = origin.x + (x - origin.x) / scale;
    y = origin.y + (y - origin.y) / scale;
}

void menu_t::map_pointer_to_screen(float& x, float& y, const float display_width,
                                   const float display_height) const noexcept
{
    if (!is_open())
        return;
    const auto scale = current_menu_scale(display_width, display_height);
    if (std::abs(scale - 1.0f) < 0.0001f)
        return;
    const auto origin = menu_transform_origin(display_width, display_height);
    x = origin.x + (x - origin.x) * scale;
    y = origin.y + (y - origin.y) * scale;
}

void menu_t::reset_content_animation()
{
    m_content_animation = 0.0f;
}

void menu_t::poll_hotkey()
{
    // A fresh installation shows the guide by itself, without the menu; it waits for the movement keys like a normal open.
    const bool guide_wanted = config::first_run::pending();
    if (!guide_wanted)
        m_guide_open_pending = false;
    else if (!m_guide_visible.load(std::memory_order_relaxed) && !m_open.load(std::memory_order_relaxed))
        m_guide_open_pending = true;

    const auto menu_key = config::lifecycle_keys().menu;
    const bool hotkey_is_down = (::GetAsyncKeyState(menu_key) & 0x8000) != 0;
    // While the guide is up it is modal: the menu key waits until the guide is finished.
    const bool guide_modal = guide_wanted && m_guide_visible.load(std::memory_order_relaxed);
    if (hotkey_is_down && !m_menu_hotkey_was_down && !guide_modal)
    {
        if (m_open.load(std::memory_order_relaxed))
        {
            m_open.store(false, std::memory_order_release);
            m_open_pending = false;
        }
        else
            m_open_pending = !m_open_pending;
    }
    m_menu_hotkey_was_down = hotkey_is_down;

    const bool open_menu = m_open_pending && !m_open.load(std::memory_order_relaxed);
    if (!open_menu && !m_guide_open_pending)
        return;

    std::vector<std::uint16_t> movement_keys{};
    constexpr std::array movement_actions{
        game::input_action::forward,
        game::input_action::back,
        game::input_action::left,
        game::input_action::right,
    };
    for (const auto action : movement_actions)
    {
        for (const auto& binding : game::input_bindings().candidates(action))
        {
            if (binding.device == game::input_device::keyboard && binding.virtual_key &&
                std::ranges::find(movement_keys, binding.virtual_key) == movement_keys.end())
            {
                movement_keys.push_back(binding.virtual_key);
            }
        }
    }
    if (std::ranges::any_of(movement_keys, [](const std::uint16_t key)
                            { return platform::windows::bound_input().physical_key_down(key); }))
    {
        return;
    }

    if (open_menu)
        m_open.store(true, std::memory_order_release);
    if (m_guide_open_pending)
    {
        m_guide_front_pending = true;
        m_guide_open_pending = false;
        m_guide_visible.store(true, std::memory_order_release);
    }
    std::vector<platform::windows::input_gateway::key_transition> releases{};
    releases.reserve(movement_keys.size());
    for (const auto key : movement_keys)
        releases.push_back({key, false});
    for (std::size_t offset = 0; offset < releases.size(); offset += 8)
        platform::windows::bound_input().keys(
            std::span{releases}.subspan(offset, std::min<std::size_t>(8, releases.size() - offset)));
    platform::windows::bound_input().clear_movement_gates();
    m_open_pending = false;
    g_toggle_animations.clear();
    reset_content_animation();
}

void menu_t::draw()
{
    // "Show the guide again" from the open menu: nothing to wait for.
    if (config::first_run::pending() && m_open.load(std::memory_order_acquire) &&
        !m_guide_visible.load(std::memory_order_acquire))
    {
        m_guide_front_pending = true;
        m_guide_visible.store(true, std::memory_order_release);
    }
    const bool menu_open = m_open.load(std::memory_order_acquire);
    const bool guide_visible = m_guide_visible.load(std::memory_order_acquire);
    if (!menu_open && !guide_visible)
    {
        return;
    }
    synchronize_menu_palette();

    const auto display = ImGui::GetIO().DisplaySize;
    const auto menu_scale = current_menu_scale(display.x, display.y);
    const auto transform_origin = menu_transform_origin(display.x, display.y);
    const menu_render_scope render_scope{display, transform_origin, menu_scale};
    if (menu_open)
    {
        const auto initial_position = initial_menu_layout_position(display.x, display.y);
        ImGui::SetNextWindowPos(initial_position, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({k_menu_width, k_menu_height}, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k_text_main);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, k_border_light);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, k_text_muted);
        ImGui::Begin("##vesta_native_menu", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoSavedSettings);

        const auto min = ImGui::GetWindowPos();
        const auto max = ImVec2{min.x + k_menu_width, min.y + k_menu_height};
        g_menu_min = min;
        g_menu_max = max;
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRectFullScreen();
        soft_shadow(draw, min, max, 20.0f, {0.0f, 14.0f}, 34.0f, {0, 0, 0, 1}, 1.0f);
        draw->PopClipRect();
        draw->AddRectFilled(min, max, packed(k_bg_base), 20.0f);
        draw->AddRect(min, max, packed(k_border), 20.0f);
        draw->AddLine({min.x + 20.0f, min.y + 1.0f}, {max.x - 20.0f, min.y + 1.0f}, IM_COL32(255, 255, 255, 20));

        push_menu_font(render::fonts().menu_regular_12);
        draw_sidebar();
        draw_content();
        ImGui::PopFont();

        ImGui::End();

        push_menu_font(render::fonts().menu_regular_12);
        draw_visual_editor();
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(5);
    }

    if (guide_visible)
    {
        push_menu_font(render::fonts().menu_regular_12);
        draw_guide();
        ImGui::PopFont();
    }
    scale_menu_draw_lists(transform_origin, menu_scale);
}

void menu_t::draw_sidebar()
{
    ImGui::SetCursorPos({0.0f, 0.0f});
    ImGui::BeginChild("##sidebar", {240.0f, 650.0f}, false, ImGuiWindowFlags_NoScrollbar);
    const auto min = ImGui::GetWindowPos();
    const auto max = ImVec2{min.x + 240.0f, min.y + 650.0f};
    auto* draw = ImGui::GetWindowDrawList();

    add_vertical_gradient_rounded(draw, min, max, IM_COL32(0, 0, 0, 26), IM_COL32(0, 0, 0, 76), 20.0f,
                                  ImDrawFlags_RoundCornersLeft);
    draw->AddLine({max.x - 1.0f, min.y}, {max.x - 1.0f, max.y}, packed(k_border));

    const auto* brand = render::fonts().menu_brand_30;
    if (brand && brand->im_font)
    {
        constexpr auto text = std::string_view{"VESTA"};
        const auto brand_size = brand->font_size * 1.25f;
        float width{};
        for (const auto c : text)
            width += brand->im_font->CalcTextSizeA(brand_size, FLT_MAX, 0.0f, &c, &c + 1).x;
        constexpr auto letter_spacing = 2.5f;
        width += letter_spacing * (text.size() - 1);
        auto x = min.x + (240.0f - width) * 0.5f;
        for (const auto c : text)
        {
            draw->AddText(brand->im_font, brand_size, {x, min.y + (112.0f - brand_size) * 0.5f}, IM_COL32_WHITE, &c,
                          &c + 1);
            x += brand->im_font->CalcTextSizeA(brand_size, FLT_MAX, 0.0f, &c, &c + 1).x + letter_spacing;
        }
    }

    static constexpr const char* labels[]{"Visuals", "Misc"};
    ImGui::SetCursorPos({20.0f, 112.0f});
    for (int i = 0; i < 2; ++i)
    {
        ImGui::SetCursorPosX(20.0f);
        ImGui::PushID(i);
        const auto active = m_page == i;
        if (nav_button(labels[i], active, i) && !active)
        {
            m_page = i;
            reset_content_animation();
        }
        ImGui::PopID();
        if (i != 1)
            ImGui::Dummy({0.0f, 6.0f});
    }

    ImGui::EndChild();
}

void menu_t::draw_content()
{
    ImGui::SetCursorPos({240.0f, 0.0f});
    ImGui::BeginChild("##content", {710.0f, 650.0f}, false, ImGuiWindowFlags_NoScrollbar);
    m_content_animation = std::min(1.0f, m_content_animation + ImGui::GetIO().DeltaTime / 0.14f);
    const auto t = 1.0f - std::pow(1.0f - m_content_animation, 3.0f);
    ImGui::SetCursorPosY((1.0f - t) * 4.0f);

    if (m_page == 0)
        draw_visuals();
    else
        draw_misc();

    ImGui::EndChild();
}
} // namespace ui
