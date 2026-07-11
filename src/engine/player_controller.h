#pragma once

#include "engine/world.h"

namespace ol {

constexpr float player_radius_m = 0.35f;
constexpr float player_stand_height_m = 1.80f;
constexpr float player_stand_eye_m = 1.70f;
constexpr float player_crouch_height_m = 0.90f;
constexpr float player_crouch_eye_m = 0.85f;
constexpr float player_walk_speed_mps = 7.10f;
constexpr float player_sprint_speed_mps = 14.20f;
constexpr float player_crouch_speed_mps = 3.20f;
constexpr float player_min_jump_height_m = 0.30f;
constexpr float player_max_jump_height_m = 1.10f;
constexpr float player_jump_hold_time_s = 0.18f;
constexpr float player_ground_accel_mps2 = 60.0f;
constexpr float player_ground_decel_mps2 = 70.0f;
constexpr float player_air_accel_mps2 = 35.0f;
constexpr float player_air_gain_accel_mps2 = 14.0f;
constexpr float player_air_control_speed_mps = 4.25f;
constexpr float player_eye_transition_mps = 5.0f;
constexpr float player_step_up_m = 0.35f;

struct PlayerControllerInput {
    Vector2 move = {0.0f, 0.0f};
    bool jump_pressed = false;
    bool jump_held = false;
    bool sprint = false;
    bool crouch = false;
};

void player_sync_masses_to_pose(Dimension* dim, PlayerEntity* player, WorldPos feet_pos);
bool player_can_use_height(const Dimension* dim, const PlayerEntity* player, WorldPos feet_pos, float height);
void player_controller_step(Dimension* dim, PlayerEntity* player, const PlayerControllerInput& input, float dt);

} // namespace ol
