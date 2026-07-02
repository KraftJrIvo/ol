#pragma once

#include "engine/render.h"

namespace ol {

constexpr u32 max_saved_sessions = 24;
constexpr u32 max_visible_session_rows = 5;

enum MenuInputField : u32 {
    menu_input_none,
    menu_input_player,
    menu_input_session
};

enum MenuControl : u32 {
    menu_control_none,
    menu_control_player,
    menu_control_session,
    menu_control_color,
    menu_control_host,
    menu_control_join,
    menu_control_color_r,
    menu_control_color_g,
    menu_control_color_b,
    menu_control_session_dropdown,
    menu_control_session_item,
    menu_control_session_delete
};

enum PauseControl : u32 {
    pause_control_none,
    pause_control_fov,
    pause_control_scale,
    pause_control_continue,
    pause_control_first_menu
};

struct MenuScreen {
    RenderState* renderer = nullptr;
    const char* player_name = "";
    const char* session_name = "";
    const char* status = "";
    Color player_color = WHITE;
    u32 active_field = menu_input_none;
    bool color_picker_open = false;
    bool sessions_open = false;
    const char* const* session_names = nullptr;
    u32 session_count = 0;
    int session_scroll = 0;
    int deleting_session_index = -1;
    float delete_progress = 0.0f;
};

struct PauseScreen {
    RenderState* renderer = nullptr;
    int fov = 90;
    int scale_power = 0;
};

struct MenuHit {
    u32 control = menu_control_none;
    int session_index = -1;
};

struct PauseHit {
    u32 control = pause_control_none;
};

void demo_draw_menu_contents(const MenuScreen& menu);
void demo_draw_menu_screen(const MenuScreen& menu);
MenuHit demo_menu_hit_test(bool color_picker_open, bool sessions_open, int session_scroll, u32 session_count, Vector2 mouse);
int demo_menu_color_value_from_mouse(u32 control, Vector2 mouse);

void demo_draw_pause_contents(const PauseScreen& pause);
void demo_draw_pause_screen(const PauseScreen& pause);
void demo_draw_pause_overlay_screen(const PauseScreen& pause);
PauseHit demo_pause_hit_test(Vector2 mouse);
int demo_pause_value_from_mouse(u32 control, Vector2 mouse);

} // namespace ol
