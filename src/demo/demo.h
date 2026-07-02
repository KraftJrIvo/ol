#pragma once

#include "demo/menu.h"
#include "engine/net.h"
#include "engine/player_controller.h"
#include "engine/render.h"
#include "engine/world.h"

#include <array>

namespace ol {

struct PlayerInput {
    Vector2 move = {0.0f, 0.0f};
    bool jump_pressed = false;
    bool jump_held = false;
    bool sprint = false;
    bool crouch = false;
};

struct SavedPlayerState {
    bool valid = false;
    u64 peer_id = 0;
    char name[32]{};
    Color color = WHITE;
    ChunkCoord chunk{};
    Vector3 local = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float body_radius = player_radius_m;
    float current_height = player_stand_height_m;
};

struct SavedSessionState {
    bool valid = false;
    char name[32]{};
    u32 player_count = 0;
    std::array<SavedPlayerState, max_players> players{};
};

struct DemoProfile {
    char player_name[32] = "player";
    char session_name[32] = "session";
    Color player_color = {90, 180, 255, 255};
    int fov = 90;
    int scale_power = 0;
    u32 session_count = 0;
    std::array<SavedSessionState, max_saved_sessions> sessions{};
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
    bool paused = false;
    u32 active_menu_field = menu_input_player;
    u32 dragged_menu_control = menu_control_none;
    u32 dragged_pause_control = pause_control_none;
    bool sessions_open = false;
    int session_scroll = 0;
    int deleting_session_index = -1;
    double delete_hold_started = 0.0;
    bool delete_hold_active = false;
    char player_name[32] = "player";
    char session_name[32] = "session";
    Color player_color = {90, 180, 255, 255};
    bool color_picker_open = false;
    bool profile_dirty = false;
    double last_profile_save_time = 0.0;
    DemoProfile profile{};
    std::array<u64, max_players> remote_peer_ids{};
    std::array<u32, max_players> remote_player_ids{};
    std::array<u64, max_players> restore_sent_peer_ids{};
};

void demo_init(DemoApp* app);
void demo_shutdown(DemoApp* app);
void demo_generate_world(DemoApp* app);
void demo_draw_menu(DemoApp* app);
bool demo_update_and_draw(DemoApp* app);
int demo_run_steam_host_smoke(double timeout_s, double hold_s);
int demo_run_steam_join_smoke(const char* lobby_id, double timeout_s);

} // namespace ol
