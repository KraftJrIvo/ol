#pragma once

#include "engine/render.h"

namespace ol {

struct MenuScreen {
    RenderState* renderer = nullptr;
    const char* player_name = "";
    const char* session_name = "";
    const char* status = "";
    Color player_color = WHITE;
};

void demo_draw_menu_contents(const MenuScreen& menu);
void demo_draw_menu_screen(const MenuScreen& menu);

} // namespace ol
