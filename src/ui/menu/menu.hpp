#pragma once

#include <windows.h>

#include <atomic>

namespace ui
{
class menu_t
{
public:
    bool initialize(HWND hwnd);
    void poll_hotkey();
    void draw();
    void map_pointer_to_layout(float& x, float& y, float display_width, float display_height) const noexcept;
    void map_pointer_to_screen(float& x, float& y, float display_width, float display_height) const noexcept;
    void close() noexcept { m_open.store(false, std::memory_order_release); }
    [[nodiscard]] bool is_open() const noexcept
    {
        // The first-run guide is a window of its own and takes input like the menu does.
        return m_open.load(std::memory_order_acquire) || m_guide_visible.load(std::memory_order_acquire);
    }

private:
    friend struct menu_render_test_access;
    void draw_sidebar();
    void draw_content();
    void draw_guide();
    void draw_visual_editor();
    void draw_visuals();
    void draw_misc();
    void reset_content_animation();

    std::atomic<bool> m_open{};
    bool m_menu_hotkey_was_down{};
    bool m_open_pending{};
    HWND m_hwnd{};
    int m_page{};
    int m_weapon_group{-1};
    int m_visual_group{};
    int m_misc_group{};
    float m_content_animation{1.0f};
    float m_visual_editor_animation{};
    bool m_preview_orbiting{};
    int m_guide_step{};
    const char* m_guide_message{};       // English key of a fixed message; translated when drawn
    std::atomic<bool> m_guide_visible{}; // the guide window is on screen, including its fade-out
    bool m_guide_open_pending{};         // the guide waits for the movement keys to be released
    bool m_guide_front_pending{};        // raise the guide above the menu on its first frame
    float m_guide_anim{};                // 0 hidden .. 1 fully shown
    float m_guide_step_anim{1.0f};       // 1 once the content of the current step has settled
    int m_guide_shown_step{};
    int m_guide_direction{1};
    float m_guide_dot[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float m_guide_hover[2]{};
};
} // namespace ui
