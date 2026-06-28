#include "demo/menu.h"

namespace ol {

static void menu_text(RenderState* renderer, const char* text, Vector2 pos, float size, Color color) {
    if (renderer && renderer->font_ready) {
        DrawTextEx(renderer->font, text, pos, size, 1.0f, color);
    } else {
        DrawText(text, static_cast<int>(pos.x), static_cast<int>(pos.y), static_cast<int>(size), color);
    }
}

void demo_draw_menu_contents(const MenuScreen& menu) {
    ClearBackground({25, 28, 32, 255});
    menu_text(menu.renderer, "OL", {42.0f, 34.0f}, 72.0f, WHITE);
    menu_text(menu.renderer, "Name:", {46.0f, 138.0f}, 34.0f, GRAY);
    menu_text(menu.renderer, menu.player_name, {170.0f, 138.0f}, 34.0f, WHITE);
    menu_text(menu.renderer, "Session:", {46.0f, 188.0f}, 34.0f, GRAY);
    menu_text(menu.renderer, menu.session_name, {205.0f, 188.0f}, 34.0f, WHITE);
    DrawRectangle(48, 256, 42, 42, menu.player_color);
    menu_text(menu.renderer, "C color   H host/offline start   J join from clipboard", {108.0f, 258.0f}, 26.0f, LIGHTGRAY);
    menu_text(menu.renderer, "Backspace edits name", {108.0f, 296.0f}, 26.0f, LIGHTGRAY);
    menu_text(menu.renderer, menu.status, {48.0f, 368.0f}, 24.0f, {170, 190, 210, 255});
}

void demo_draw_menu_screen(const MenuScreen& menu) {
    BeginDrawing();
    demo_draw_menu_contents(menu);
    EndDrawing();
}

} // namespace ol
