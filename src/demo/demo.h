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

struct DemoFrameInput {
    bool mouse_left_pressed = false;
    bool mouse_left_down = false;
    bool mouse_left_released = false;
    bool tab_pressed = false;
    bool backspace_pressed = false;
    bool escape_pressed = false;
    bool enter_pressed = false;
    bool plus_pressed = false;
    bool minus_pressed = false;
    bool f3_pressed = false;
    bool r_pressed = false;
    bool c_pressed = false;
    bool o_pressed = false;
    bool space_pressed = false;
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
    char world_name[32] = "playground";
    u32 player_count = 0;
    std::array<SavedPlayerState, max_players> players{};
};

struct DemoProfile {
    char player_name[32] = "player";
    char session_name[32] = "session";
    char world_name[32] = "playground";
    Color player_color = {90, 180, 255, 255};
    int fov = 90;
    int scale_power = 0;
    u32 session_count = 0;
    std::array<SavedSessionState, max_saved_sessions> sessions{};
};

constexpr u32 max_streamed_world_chunks = 192;
constexpr u32 max_streamed_chunk_meshes = 48;
constexpr u32 max_streamed_chunk_boxes = 32;

struct StreamedWorldChunk {
    bool valid = false;
    ChunkCoord coord{};
    u32 mesh_count = 0;
    std::array<u32, max_streamed_chunk_meshes> meshes{};
    u32 box_count = 0;
    std::array<u32, max_streamed_chunk_boxes> boxes{};
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
    bool blocking_screen_cursor_released = false;
    u32 active_menu_field = menu_input_player;
    u32 dragged_menu_control = menu_control_none;
    u32 dragged_pause_control = pause_control_none;
    bool sessions_open = false;
    int session_scroll = 0;
    int deleting_session_index = -1;
    double delete_hold_started = 0.0;
    bool delete_hold_active = false;
    bool worlds_open = false;
    char player_name[32] = "player";
    char session_name[32] = "session";
    char world_name[32] = "playground";
    Color player_color = {90, 180, 255, 255};
    bool color_picker_open = false;
    bool profile_dirty = false;
    double last_profile_save_time = 0.0;
    DemoProfile profile{};
    std::array<u64, max_players> remote_peer_ids{};
    std::array<u32, max_players> remote_player_ids{};
    std::array<u64, max_players> restore_sent_peer_ids{};
    DemoFrameInput frame_input{};
    std::array<bool, 512> previous_key_down{};
    bool previous_mouse_left_down = false;
    bool landscape_streaming = false;
    u32 landscape_cube_geom = invalid_id;
    bool landscape_stream_center_valid = false;
    ChunkCoord landscape_stream_center{};
    std::array<StreamedWorldChunk, max_streamed_world_chunks> streamed_chunks{};
};

void demo_init(DemoApp* app);
void demo_shutdown(DemoApp* app);
void demo_generate_world(DemoApp* app);
void demo_draw_menu(DemoApp* app);
bool demo_update_and_draw(DemoApp* app);
int demo_run_steam_host_smoke(double timeout_s, double hold_s);
int demo_run_steam_join_smoke(const char* lobby_id, double timeout_s);

} // namespace ol
