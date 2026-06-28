#pragma once

#include "engine/net.h"
#include "engine/player_controller.h"
#include "engine/render.h"
#include "engine/world.h"

namespace ol {

struct PlayerInput {
    Vector2 move = {0.0f, 0.0f};
    bool jump_pressed = false;
    bool jump_held = false;
    bool sprint = false;
    bool crouch = false;
};

struct DemoApp {
    World world{};
    RenderState renderer{};
    NetSession net{};
    u32 dimension_id = invalid_id;
    u32 local_player_id = invalid_id;
    PlayerInput input{};
    float fixed_accum = 0.0f;
    bool in_game = false;
    bool mouse_captured = false;
    char player_name[32] = "player";
    char session_name[32] = "playground";
    Color player_color = {90, 180, 255, 255};
};

void demo_init(DemoApp* app);
void demo_shutdown(DemoApp* app);
void demo_draw_menu(DemoApp* app);
bool demo_update_and_draw(DemoApp* app);

} // namespace ol
