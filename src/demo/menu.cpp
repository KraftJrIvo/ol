#include "demo/menu.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ol {
namespace {

constexpr float menu_base_w = 900.0f;
constexpr float menu_base_h = 640.0f;
constexpr float pause_base_w = 760.0f;
constexpr float pause_base_h = 640.0f;

struct MenuLayout {
    float scale = 1.0f;
    Vector2 origin{};
    Rectangle player{};
    Rectangle session{};
    Rectangle session_arrow{};
    Rectangle session_list{};
    Rectangle world{};
    Rectangle world_arrow{};
    Rectangle world_list{};
    Rectangle host{};
    Rectangle join{};
    Rectangle picker_panel{};
    Rectangle color_slider[3]{};
    Vector2 color_center{};
    float color_radius = 24.0f;
    Vector2 status_pos{};
};

struct PauseLayout {
    float scale = 1.0f;
    Vector2 origin{};
    Rectangle fov_slider{};
    Rectangle scale_slider{};
    Rectangle render_radius_slider{};
    Rectangle fullscreen_button{};
    Rectangle fps_counter_button{};
    Rectangle continue_button{};
    Rectangle first_menu_button{};
};

static float ui_scale(float screen_w, float screen_h, float base_w, float base_h) {
    float s = fminf(screen_w / base_w, screen_h / base_h);
    return clampf(s, 0.55f, 1.35f);
}

static Rectangle scaled_rect(Vector2 origin, float scale, Rectangle r) {
    return {
        origin.x + r.x * scale,
        origin.y + r.y * scale,
        r.width * scale,
        r.height * scale
    };
}

static Vector2 scaled_point(Vector2 origin, float scale, Vector2 p) {
    return {origin.x + p.x * scale, origin.y + p.y * scale};
}

static MenuLayout make_menu_layout() {
    const float screen_w = static_cast<float>(GetScreenWidth());
    const float screen_h = static_cast<float>(GetScreenHeight());
    MenuLayout layout{};
    layout.scale = ui_scale(screen_w, screen_h, menu_base_w, menu_base_h);
    layout.origin = {
        (screen_w - menu_base_w * layout.scale) * 0.5f,
        (screen_h - menu_base_h * layout.scale) * 0.5f
    };

    layout.color_center = scaled_point(layout.origin, layout.scale, {300.0f, 146.0f});
    layout.color_radius = 24.0f * layout.scale;
    layout.player = scaled_rect(layout.origin, layout.scale, {350.0f, 118.0f, 230.0f, 56.0f});
    layout.session = scaled_rect(layout.origin, layout.scale, {180.0f, 280.0f, 230.0f, 56.0f});
    layout.session_arrow = scaled_rect(layout.origin, layout.scale, {418.0f, 280.0f, 46.0f, 56.0f});
    layout.session_list = scaled_rect(layout.origin, layout.scale, {180.0f, 342.0f, 284.0f, 232.0f});
    layout.world = scaled_rect(layout.origin, layout.scale, {180.0f, 352.0f, 230.0f, 56.0f});
    layout.world_arrow = scaled_rect(layout.origin, layout.scale, {418.0f, 352.0f, 46.0f, 56.0f});
    layout.world_list = scaled_rect(layout.origin, layout.scale, {180.0f, 414.0f, 284.0f, 182.0f});
    layout.host = scaled_rect(layout.origin, layout.scale, {180.0f, 424.0f, 230.0f, 56.0f});
    layout.join = scaled_rect(layout.origin, layout.scale, {545.0f, 365.0f, 235.0f, 56.0f});
    layout.picker_panel = scaled_rect(layout.origin, layout.scale, {245.0f, 190.0f, 400.0f, 150.0f});
    layout.color_slider[0] = scaled_rect(layout.origin, layout.scale, {330.0f, 225.0f, 235.0f, 8.0f});
    layout.color_slider[1] = scaled_rect(layout.origin, layout.scale, {330.0f, 265.0f, 235.0f, 8.0f});
    layout.color_slider[2] = scaled_rect(layout.origin, layout.scale, {330.0f, 305.0f, 235.0f, 8.0f});
    layout.status_pos = scaled_point(layout.origin, layout.scale, {180.0f, 586.0f});
    return layout;
}

static PauseLayout make_pause_layout() {
    const float screen_w = static_cast<float>(GetScreenWidth());
    const float screen_h = static_cast<float>(GetScreenHeight());
    PauseLayout layout{};
    layout.scale = ui_scale(screen_w, screen_h, pause_base_w, pause_base_h);
    layout.origin = {
        (screen_w - pause_base_w * layout.scale) * 0.5f,
        (screen_h - pause_base_h * layout.scale) * 0.5f
    };
    layout.fov_slider = scaled_rect(layout.origin, layout.scale, {150.0f, 105.0f, 460.0f, 8.0f});
    layout.scale_slider = scaled_rect(layout.origin, layout.scale, {150.0f, 200.0f, 460.0f, 8.0f});
    layout.render_radius_slider = scaled_rect(layout.origin, layout.scale, {150.0f, 295.0f, 460.0f, 8.0f});
    layout.fullscreen_button = scaled_rect(layout.origin, layout.scale, {150.0f, 380.0f, 205.0f, 56.0f});
    layout.fps_counter_button = scaled_rect(layout.origin, layout.scale, {405.0f, 380.0f, 205.0f, 56.0f});
    layout.continue_button = scaled_rect(layout.origin, layout.scale, {150.0f, 490.0f, 205.0f, 56.0f});
    layout.first_menu_button = scaled_rect(layout.origin, layout.scale, {405.0f, 490.0f, 205.0f, 56.0f});
    return layout;
}

static bool point_in_rect(Vector2 p, Rectangle r) {
    return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
}

static bool point_in_circle(Vector2 p, Vector2 center, float radius) {
    const Vector2 d = p - center;
    return Vector2Length(d) <= radius;
}

static Vector2 measure_ui_text(RenderState* renderer, const char* text, float size) {
    if (renderer && renderer->font_ready) {
        return MeasureTextEx(renderer->font, text, size, 1.0f);
    }
    return {static_cast<float>(MeasureText(text, static_cast<int>(size))), size};
}

static void draw_ui_text(RenderState* renderer, const char* text, Vector2 pos, float size, Color color) {
    if (renderer && renderer->font_ready) {
        DrawTextEx(renderer->font, text, pos, size, 1.0f, color);
    } else {
        DrawText(text, static_cast<int>(pos.x), static_cast<int>(pos.y), static_cast<int>(size), color);
    }
}

static float fit_text_size(RenderState* renderer, const char* text, float wanted, float max_width) {
    float size = wanted;
    while (size > 12.0f && measure_ui_text(renderer, text, size).x > max_width) {
        size -= 1.0f;
    }
    return size;
}

static void draw_centered_text(RenderState* renderer, const char* text, Rectangle rect, float wanted_size, Color color) {
    const float size = fit_text_size(renderer, text, wanted_size, rect.width - 24.0f);
    const Vector2 m = measure_ui_text(renderer, text, size);
    draw_ui_text(renderer, text, {rect.x + (rect.width - m.x) * 0.5f, rect.y + (rect.height - m.y) * 0.5f - 1.0f}, size, color);
}

static void draw_button(RenderState* renderer, Rectangle rect, const char* label, bool hot) {
    const Color fill = hot ? Color{18, 22, 27, 255} : BLACK;
    DrawRectangleRounded(rect, 0.18f, 10, fill);
    DrawRectangleRoundedLinesEx(rect, 0.18f, 10, 3.0f, WHITE);
    draw_centered_text(renderer, label, rect, rect.height * 0.52f, WHITE);
}

static void draw_toggle_button(RenderState* renderer, Rectangle rect, const char* label, bool enabled, bool hot) {
    const Color fill = hot ? Color{18, 22, 27, 255} : BLACK;
    const Color border = enabled ? Color{103, 218, 151, 255} : WHITE;
    DrawRectangleRounded(rect, 0.18f, 10, fill);
    DrawRectangleRoundedLinesEx(rect, 0.18f, 10, 3.0f, border);

    char text[64]{};
    std::snprintf(text, sizeof(text), "%s %s", label, enabled ? "on" : "off");
    draw_centered_text(renderer, text, rect, rect.height * 0.42f, WHITE);
}

static void draw_arrow_button(Rectangle rect, bool open, bool hot) {
    DrawRectangleRounded(rect, 0.18f, 10, hot ? Color{18, 22, 27, 255} : BLACK);
    DrawRectangleRoundedLinesEx(rect, 0.18f, 10, 3.0f, WHITE);
    const float cx = rect.x + rect.width * 0.5f;
    const float cy = rect.y + rect.height * 0.5f;
    const float s = rect.height * 0.16f;
    if (open) {
        DrawTriangle({cx - s, cy + s}, {cx, cy - s}, {cx + s, cy + s}, WHITE);
    } else {
        DrawTriangle({cx - s, cy - s}, {cx, cy + s}, {cx + s, cy - s}, WHITE);
    }
}

static void append_caret(char* dst, size_t cap, const char* src, bool active) {
    std::snprintf(dst, cap, "%s", src ? src : "");
    if (!active) return;
    const bool caret_on = fmod(GetTime(), 1.0) < 0.55;
    const size_t len = std::strlen(dst);
    if (caret_on && len + 1 < cap) {
        dst[len] = '_';
        dst[len + 1] = 0;
    }
}

static void draw_text_field(RenderState* renderer, Rectangle rect, const char* value, const char* placeholder, bool active, bool hot) {
    DrawRectangleRounded(rect, 0.18f, 10, BLACK);
    const Color border = active ? Color{255, 255, 255, 255} : (hot ? Color{220, 225, 232, 255} : Color{185, 190, 198, 255});
    DrawRectangleRoundedLinesEx(rect, 0.18f, 10, active ? 3.5f : 3.0f, border);

    char text[80]{};
    const bool has_value = value && value[0];
    append_caret(text, sizeof(text), has_value ? value : placeholder, active);
    const Color color = has_value || active ? WHITE : Color{135, 142, 152, 255};
    const float size = fit_text_size(renderer, text, rect.height * 0.53f, rect.width - 32.0f);
    const Vector2 m = measure_ui_text(renderer, text, size);
    draw_ui_text(renderer, text, {rect.x + 18.0f, rect.y + (rect.height - m.y) * 0.5f - 1.0f}, size, color);
}

static void draw_dropdown_value(RenderState* renderer, Rectangle rect, const char* value, const char* placeholder, bool hot) {
    DrawRectangleRounded(rect, 0.18f, 10, BLACK);
    const Color border = hot ? Color{220, 225, 232, 255} : Color{185, 190, 198, 255};
    DrawRectangleRoundedLinesEx(rect, 0.18f, 10, 3.0f, border);

    const bool has_value = value && value[0];
    const char* text = has_value ? value : placeholder;
    const Color color = has_value ? WHITE : Color{135, 142, 152, 255};
    const float size = fit_text_size(renderer, text, rect.height * 0.53f, rect.width - 32.0f);
    const Vector2 m = measure_ui_text(renderer, text, size);
    draw_ui_text(renderer, text, {rect.x + 18.0f, rect.y + (rect.height - m.y) * 0.5f - 1.0f}, size, color);
}

static void draw_slider(RenderState* renderer, Rectangle track, const char* label, int value, int min_value, int max_value, Color accent) {
    const float label_size = 24.0f * (track.height / 8.0f);
    char text[48]{};
    std::snprintf(text, sizeof(text), "%s %d", label, value);
    draw_ui_text(renderer, text, {track.x, track.y - 34.0f * (track.height / 8.0f)}, label_size, WHITE);

    DrawRectangleRounded({track.x, track.y, track.width, track.height}, 0.8f, 8, Color{76, 82, 92, 255});
    const float t = max_value > min_value ? static_cast<float>(value - min_value) / static_cast<float>(max_value - min_value) : 0.0f;
    DrawRectangleRounded({track.x, track.y, track.width * clampf(t, 0.0f, 1.0f), track.height}, 0.8f, 8, accent);
    const Vector2 thumb = {track.x + track.width * clampf(t, 0.0f, 1.0f), track.y + track.height * 0.5f};
    DrawCircleV(thumb, 11.0f * (track.height / 8.0f), BLACK);
    DrawCircleLinesV(thumb, 11.0f * (track.height / 8.0f), WHITE);
}

static int color_component(Color color, u32 control) {
    if (control == menu_control_color_r) return color.r;
    if (control == menu_control_color_g) return color.g;
    return color.b;
}

static Color slider_accent(u32 control) {
    if (control == menu_control_color_r) return {238, 44, 56, 255};
    if (control == menu_control_color_g) return {68, 207, 101, 255};
    return {82, 151, 255, 255};
}

static const char* slider_label(u32 control) {
    if (control == menu_control_color_r) return "R";
    if (control == menu_control_color_g) return "G";
    return "B";
}

static void draw_color_picker(RenderState* renderer, const MenuLayout& layout, Color color) {
    DrawRectangleRounded(layout.picker_panel, 0.08f, 8, Color{7, 8, 10, 245});
    DrawRectangleRoundedLinesEx(layout.picker_panel, 0.08f, 8, 2.0f, Color{235, 238, 244, 255});

    const u32 controls[3] = {menu_control_color_r, menu_control_color_g, menu_control_color_b};
    for (int i = 0; i < 3; ++i) {
        const Rectangle track = layout.color_slider[i];
        const u32 control = controls[i];
        const int value = color_component(color, control);
        draw_slider(renderer, track, slider_label(control), value, 0, 255, slider_accent(control));

        char value_text[8]{};
        std::snprintf(value_text, sizeof(value_text), "%d", value);
        draw_ui_text(renderer, value_text, {track.x + track.width + 22.0f * layout.scale, track.y - 13.0f * layout.scale}, 22.0f * layout.scale, WHITE);
    }
}

static Rectangle session_row_rect(const MenuLayout& layout, int visible_index) {
    const float pad = 6.0f * layout.scale;
    const float row_h = 42.0f * layout.scale;
    return {
        layout.session_list.x + pad,
        layout.session_list.y + pad + static_cast<float>(visible_index) * row_h,
        layout.session_list.width - pad * 2.0f,
        row_h - 4.0f * layout.scale
    };
}

static Rectangle world_row_rect(const MenuLayout& layout, int visible_index) {
    const float pad = 6.0f * layout.scale;
    const float row_h = 42.0f * layout.scale;
    return {
        layout.world_list.x + pad,
        layout.world_list.y + pad + static_cast<float>(visible_index) * row_h,
        layout.world_list.width - pad * 2.0f,
        row_h - 4.0f * layout.scale
    };
}

static Rectangle session_delete_rect(Rectangle row, float scale) {
    const float size = fminf(30.0f * scale, row.height - 6.0f * scale);
    return {row.x + row.width - size - 6.0f * scale, row.y + (row.height - size) * 0.5f, size, size};
}

static void draw_session_dropdown(const MenuScreen& menu, const MenuLayout& layout) {
    if (!menu.sessions_open) return;

    DrawRectangleRounded(layout.session_list, 0.06f, 8, Color{5, 6, 8, 246});
    DrawRectangleRoundedLinesEx(layout.session_list, 0.06f, 8, 2.0f, WHITE);

    const int scroll = menu.session_scroll < 0 ? 0 : menu.session_scroll;
    const u32 visible = menu.session_count > static_cast<u32>(scroll)
        ? static_cast<u32>(fminf(static_cast<float>(max_visible_session_rows), static_cast<float>(menu.session_count - static_cast<u32>(scroll))))
        : 0;

    if (visible == 0) {
        draw_ui_text(menu.renderer, "no previous sessions", {layout.session_list.x + 14.0f * layout.scale, layout.session_list.y + 18.0f * layout.scale}, 20.0f * layout.scale, Color{155, 164, 176, 255});
        return;
    }

    const Vector2 mouse = GetMousePosition();
    for (u32 i = 0; i < visible; ++i) {
        const int session_index = scroll + static_cast<int>(i);
        Rectangle row = session_row_rect(layout, static_cast<int>(i));
        const bool hot = point_in_rect(mouse, row);
        DrawRectangleRounded(row, 0.08f, 6, hot ? Color{24, 28, 34, 255} : Color{10, 12, 15, 255});

        Rectangle del = session_delete_rect(row, layout.scale);
        const bool deleting = session_index == menu.deleting_session_index;
        if (deleting) {
            DrawRectangleRounded({del.x, del.y + del.height * (1.0f - menu.delete_progress), del.width, del.height * menu.delete_progress}, 0.18f, 8, Color{165, 42, 50, 255});
        }
        DrawRectangleRoundedLinesEx(del, 0.18f, 8, 2.0f, deleting ? Color{255, 130, 138, 255} : Color{190, 198, 208, 255});
        draw_centered_text(menu.renderer, "X", del, 19.0f * layout.scale, WHITE);

        const char* name = (menu.session_names && menu.session_names[session_index]) ? menu.session_names[session_index] : "";
        const float size = fit_text_size(menu.renderer, name, 22.0f * layout.scale, row.width - del.width - 28.0f * layout.scale);
        const Vector2 m = measure_ui_text(menu.renderer, name, size);
        draw_ui_text(menu.renderer, name, {row.x + 12.0f * layout.scale, row.y + (row.height - m.y) * 0.5f - 1.0f}, size, WHITE);
    }

    if (menu.session_count > max_visible_session_rows) {
        const float track_h = layout.session_list.height - 16.0f * layout.scale;
        const float max_scroll = static_cast<float>(menu.session_count - max_visible_session_rows);
        const float thumb_h = fmaxf(24.0f * layout.scale, track_h * (static_cast<float>(max_visible_session_rows) / static_cast<float>(menu.session_count)));
        const float t = max_scroll > 0.0f ? clampf(static_cast<float>(scroll) / max_scroll, 0.0f, 1.0f) : 0.0f;
        Rectangle thumb{
            layout.session_list.x + layout.session_list.width - 9.0f * layout.scale,
            layout.session_list.y + 8.0f * layout.scale + (track_h - thumb_h) * t,
            4.0f * layout.scale,
            thumb_h
        };
        DrawRectangleRounded(thumb, 0.8f, 4, Color{185, 193, 204, 255});
    }
}

static void draw_world_dropdown(const MenuScreen& menu, const MenuLayout& layout) {
    if (!menu.worlds_open) return;

    DrawRectangleRounded(layout.world_list, 0.06f, 8, Color{5, 6, 8, 246});
    DrawRectangleRoundedLinesEx(layout.world_list, 0.06f, 8, 2.0f, WHITE);

    const u32 visible = static_cast<u32>(fminf(
        static_cast<float>(max_visible_world_rows),
        static_cast<float>(menu.world_count)));
    const Vector2 mouse = GetMousePosition();
    for (u32 i = 0; i < visible; ++i) {
        Rectangle row = world_row_rect(layout, static_cast<int>(i));
        const bool hot = point_in_rect(mouse, row);
        DrawRectangleRounded(row, 0.08f, 6, hot ? Color{24, 28, 34, 255} : Color{10, 12, 15, 255});

        const char* name = (menu.world_names && menu.world_names[i]) ? menu.world_names[i] : "";
        const float size = fit_text_size(menu.renderer, name, 22.0f * layout.scale, row.width - 24.0f * layout.scale);
        const Vector2 m = measure_ui_text(menu.renderer, name, size);
        draw_ui_text(menu.renderer, name, {row.x + 12.0f * layout.scale, row.y + (row.height - m.y) * 0.5f - 1.0f}, size, WHITE);
    }
}

static Rectangle slider_hit_rect(Rectangle track) {
    return {track.x - 14.0f, track.y - 18.0f, track.width + 28.0f, track.height + 36.0f};
}

static int value_from_slider(Rectangle track, Vector2 mouse, int min_value, int max_value, bool discrete) {
    const float t = clampf((mouse.x - track.x) / track.width, 0.0f, 1.0f);
    const float raw = static_cast<float>(min_value) + t * static_cast<float>(max_value - min_value);
    const int rounded = static_cast<int>(floorf(raw + 0.5f));
    if (!discrete) return rounded;
    return rounded < min_value ? min_value : (rounded > max_value ? max_value : rounded);
}

} // namespace

void demo_draw_menu_contents(const MenuScreen& menu) {
    ClearBackground(BLACK);
    const MenuLayout layout = make_menu_layout();
    const Vector2 mouse = GetMousePosition();

    draw_text_field(menu.renderer, layout.player, menu.player_name, "player", menu.active_field == menu_input_player, point_in_rect(mouse, layout.player));
    draw_text_field(menu.renderer, layout.session, menu.session_name, "session", menu.active_field == menu_input_session, point_in_rect(mouse, layout.session));
    draw_arrow_button(layout.session_arrow, menu.sessions_open, point_in_rect(mouse, layout.session_arrow));
    draw_dropdown_value(menu.renderer, layout.world, menu.world_name, "world", point_in_rect(mouse, layout.world));
    draw_arrow_button(layout.world_arrow, menu.worlds_open, point_in_rect(mouse, layout.world_arrow));
    draw_button(menu.renderer, layout.host, "host", point_in_rect(mouse, layout.host));
    draw_button(menu.renderer, layout.join, "join from CB", point_in_rect(mouse, layout.join));

    DrawCircleV(layout.color_center, layout.color_radius, menu.player_color);
    DrawCircleLinesV(layout.color_center, layout.color_radius + 1.0f, WHITE);
    DrawCircleLinesV(layout.color_center, layout.color_radius + 2.0f, WHITE);

    if (menu.color_picker_open) {
        draw_color_picker(menu.renderer, layout, menu.player_color);
    }

    if (menu.status && menu.status[0]) {
        draw_ui_text(menu.renderer, menu.status, layout.status_pos, 22.0f * layout.scale, Color{158, 176, 198, 255});
    }

    draw_session_dropdown(menu, layout);
    draw_world_dropdown(menu, layout);
}

void demo_draw_menu_screen(const MenuScreen& menu) {
    BeginDrawing();
    demo_draw_menu_contents(menu);
    EndDrawing();
}

MenuHit demo_menu_hit_test(bool color_picker_open, bool sessions_open, bool worlds_open, int session_scroll, u32 session_count, u32 world_count, Vector2 mouse) {
    const MenuLayout layout = make_menu_layout();
    if (color_picker_open) {
        if (point_in_rect(mouse, slider_hit_rect(layout.color_slider[0]))) return {menu_control_color_r};
        if (point_in_rect(mouse, slider_hit_rect(layout.color_slider[1]))) return {menu_control_color_g};
        if (point_in_rect(mouse, slider_hit_rect(layout.color_slider[2]))) return {menu_control_color_b};
    }
    if (worlds_open && point_in_rect(mouse, layout.world_list)) {
        const u32 visible = static_cast<u32>(fminf(
            static_cast<float>(max_visible_world_rows),
            static_cast<float>(world_count)));
        for (u32 i = 0; i < visible; ++i) {
            if (point_in_rect(mouse, world_row_rect(layout, static_cast<int>(i)))) {
                return {menu_control_world_item, -1, static_cast<int>(i)};
            }
        }
        return {};
    }
    if (sessions_open && point_in_rect(mouse, layout.session_list)) {
        const int scroll = session_scroll < 0 ? 0 : session_scroll;
        const u32 visible = session_count > static_cast<u32>(scroll)
            ? static_cast<u32>(fminf(static_cast<float>(max_visible_session_rows), static_cast<float>(session_count - static_cast<u32>(scroll))))
            : 0;
        for (u32 i = 0; i < visible; ++i) {
            const int session_index = scroll + static_cast<int>(i);
            Rectangle row = session_row_rect(layout, static_cast<int>(i));
            if (!point_in_rect(mouse, row)) continue;
            if (point_in_rect(mouse, session_delete_rect(row, layout.scale))) return {menu_control_session_delete, session_index};
            return {menu_control_session_item, session_index};
        }
        return {};
    }
    if (point_in_circle(mouse, layout.color_center, layout.color_radius + 5.0f)) return {menu_control_color};
    if (point_in_rect(mouse, layout.player)) return {menu_control_player};
    if (point_in_rect(mouse, layout.session)) return {menu_control_session};
    if (point_in_rect(mouse, layout.session_arrow)) return {menu_control_session_dropdown};
    if (point_in_rect(mouse, layout.world) || point_in_rect(mouse, layout.world_arrow)) return {menu_control_world_dropdown};
    if (point_in_rect(mouse, layout.host)) return {menu_control_host};
    if (point_in_rect(mouse, layout.join)) return {menu_control_join};
    return {};
}

int demo_menu_color_value_from_mouse(u32 control, Vector2 mouse) {
    const MenuLayout layout = make_menu_layout();
    if (control == menu_control_color_r) return value_from_slider(layout.color_slider[0], mouse, 0, 255, true);
    if (control == menu_control_color_g) return value_from_slider(layout.color_slider[1], mouse, 0, 255, true);
    if (control == menu_control_color_b) return value_from_slider(layout.color_slider[2], mouse, 0, 255, true);
    return 0;
}

static void draw_pause_controls(const PauseScreen& pause) {
    const PauseLayout layout = make_pause_layout();

    draw_slider(pause.renderer, layout.fov_slider, "fov", pause.fov, 60, 120, Color{86, 167, 255, 255});

    char scale_label[32]{};
    std::snprintf(scale_label, sizeof(scale_label), "scale 1/%d", 1 << pause.scale_power);
    draw_ui_text(pause.renderer, scale_label, {layout.scale_slider.x, layout.scale_slider.y - 34.0f * layout.scale}, 24.0f * layout.scale, WHITE);
    DrawRectangleRounded(layout.scale_slider, 0.8f, 8, Color{76, 82, 92, 255});
    const float t = static_cast<float>(pause.scale_power) / 4.0f;
    DrawRectangleRounded({layout.scale_slider.x, layout.scale_slider.y, layout.scale_slider.width * t, layout.scale_slider.height}, 0.8f, 8, Color{103, 218, 151, 255});
    const Vector2 scale_thumb = {layout.scale_slider.x + layout.scale_slider.width * t, layout.scale_slider.y + layout.scale_slider.height * 0.5f};
    DrawCircleV(scale_thumb, 11.0f * layout.scale, BLACK);
    DrawCircleLinesV(scale_thumb, 11.0f * layout.scale, WHITE);

    draw_slider(
        pause.renderer,
        layout.render_radius_slider,
        "render radius",
        pause.render_radius,
        pause_render_radius_min,
        pause_render_radius_max,
        Color{226, 183, 91, 255});

    const Vector2 mouse = GetMousePosition();
    draw_toggle_button(pause.renderer, layout.fullscreen_button, "fullscreen", pause.fullscreen, point_in_rect(mouse, layout.fullscreen_button));
    draw_toggle_button(pause.renderer, layout.fps_counter_button, "fps", pause.show_fps, point_in_rect(mouse, layout.fps_counter_button));
    draw_button(pause.renderer, layout.continue_button, "continue", point_in_rect(mouse, layout.continue_button));
    draw_button(pause.renderer, layout.first_menu_button, "leave", point_in_rect(mouse, layout.first_menu_button));
}

void demo_draw_pause_contents(const PauseScreen& pause) {
    ClearBackground(BLACK);
    draw_pause_controls(pause);
}

void demo_draw_pause_screen(const PauseScreen& pause) {
    BeginDrawing();
    demo_draw_pause_contents(pause);
    EndDrawing();
}

void demo_draw_pause_overlay_screen(const PauseScreen& pause) {
    BeginDrawing();
    renderer_draw_target_to_screen(pause.renderer);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 148});
    draw_pause_controls(pause);
    if (pause.show_fps) DrawFPS(10, 10);
    EndDrawing();
}

PauseHit demo_pause_hit_test(Vector2 mouse) {
    const PauseLayout layout = make_pause_layout();
    if (point_in_rect(mouse, slider_hit_rect(layout.fov_slider))) return {pause_control_fov};
    if (point_in_rect(mouse, slider_hit_rect(layout.scale_slider))) return {pause_control_scale};
    if (point_in_rect(mouse, slider_hit_rect(layout.render_radius_slider))) return {pause_control_render_radius};
    if (point_in_rect(mouse, layout.fullscreen_button)) return {pause_control_fullscreen};
    if (point_in_rect(mouse, layout.fps_counter_button)) return {pause_control_fps_counter};
    if (point_in_rect(mouse, layout.continue_button)) return {pause_control_continue};
    if (point_in_rect(mouse, layout.first_menu_button)) return {pause_control_first_menu};
    return {};
}

int demo_pause_value_from_mouse(u32 control, Vector2 mouse) {
    const PauseLayout layout = make_pause_layout();
    if (control == pause_control_fov) return value_from_slider(layout.fov_slider, mouse, 60, 120, true);
    if (control == pause_control_scale) return value_from_slider(layout.scale_slider, mouse, 0, 4, true);
    if (control == pause_control_render_radius) return value_from_slider(layout.render_radius_slider, mouse, pause_render_radius_min, pause_render_radius_max, true);
    return 0;
}

} // namespace ol
