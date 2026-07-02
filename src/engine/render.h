#pragma once

#include "engine/world.h"

#include "raylib.h"

namespace ol {

constexpr u32 render_texture_life = 1;
constexpr u32 render_texture_cross = 2;

struct CameraView {
    WorldPos anchor{};
    float eye_height = 1.4f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct RenderState {
    RenderTexture2D target{};
    Texture2D white_texture{};
    Texture2D life_texture{};
    Texture2D cross_texture{};
    Shader sprite_alpha_shader{};
    Font font{};
    bool target_ready = false;
    bool white_ready = false;
    bool life_ready = false;
    bool cross_ready = false;
    bool sprite_alpha_shader_ready = false;
    bool font_ready = false;
    int native_w = 0;
    int native_h = 0;
    int window_w = 0;
    int window_h = 0;
    int scale_power = 0;
    float fov = 90.0f;
    bool draw_physics_debug = false;
    bool depth_test_edges = true;
    bool brute_force_edge_occlusion = false;
    u32 debug_edge_count = 0;
    u32 debug_scene_triangle_count = 0;
    u32 debug_unbounded_scene_triangle_count = 0;
    u64 debug_edge_sample_count = 0;
    u64 debug_edge_candidate_count = 0;
    u64 debug_edge_ray_test_count = 0;
};

void renderer_init(RenderState* renderer);
void renderer_shutdown(RenderState* renderer);
void renderer_change_scale(RenderState* renderer, int delta);
void renderer_ensure_target(RenderState* renderer);
void renderer_draw_target_to_screen(RenderState* renderer);
void renderer_render_dimension_to_target(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id);
void renderer_draw_dimension(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id);

} // namespace ol
