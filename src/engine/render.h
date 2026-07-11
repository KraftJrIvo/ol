#pragma once

#include "engine/world.h"

#include "raylib.h"

#include <vector>

namespace ol {

constexpr u32 render_texture_life = 1;
constexpr u32 render_texture_cross = 2;
constexpr u32 render_texture_grid = 3;
constexpr u32 render_texture_grass = 4;
constexpr u32 render_texture_stone = 5;
constexpr u32 render_texture_roof = 6;

struct CameraView {
    WorldPos anchor{};
    float eye_height = 1.4f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct SpritePaintTextureCache {
    u32 sprite_id = invalid_id;
    u32 base_texture_id = invalid_id;
    u64 paint_revision = 0;
    Color tint = WHITE;
    Texture2D texture{};
};

struct MeshPaintSurfaceCache {
    u32 mesh_id = invalid_id;
    u32 geometry_id = invalid_id;
    u64 paint_hash = 0;
    Vector3 normal = {0.0f, 1.0f, 0.0f};
    Vector3 tangent = {1.0f, 0.0f, 0.0f};
    Vector3 bitangent = {0.0f, 0.0f, 1.0f};
    float plane = 0.0f;
    i32 grid_min_u = 0;
    i32 grid_min_v = 0;
    i32 width = 0;
    i32 height = 0;
    Texture2D texture{};
    std::vector<u32> triangles{};
};

struct RenderState {
    RenderTexture2D target{};
    Texture2D white_texture{};
    Texture2D life_texture{};
    Texture2D cross_texture{};
    Texture2D grid_texture{};
    Texture2D grass_texture{};
    Texture2D stone_texture{};
    Texture2D roof_texture{};
    Texture2D edge_depth_texture{};
    Texture2D edge_scene_depth_texture{};
    Shader sprite_alpha_shader{};
    Shader edge_depth_shader{};
    Font font{};
    bool target_ready = false;
    bool white_ready = false;
    bool life_ready = false;
    bool cross_ready = false;
    bool world_textures_ready = false;
    bool edge_depth_texture_ready = false;
    bool edge_scene_depth_texture_ready = false;
    bool sprite_alpha_shader_ready = false;
    bool edge_depth_shader_ready = false;
    bool edge_filter_compute_ready = false;
    bool font_ready = false;
    int native_w = 0;
    int native_h = 0;
    int window_w = 0;
    int window_h = 0;
    int scale_power = 0;
    float fov = 90.0f;
    bool draw_physics_debug = false;
    bool depth_test_edges = true;
    bool gpu_depth_edges = true;
    bool brute_force_edge_occlusion = false;
    std::vector<SpritePaintTextureCache> sprite_paint_textures{};
    std::vector<MeshPaintSurfaceCache> mesh_paint_surfaces{};
    u64 mesh_paint_revision = 0;
    u64 mesh_paint_topology_hash = 0;
    std::vector<u8> life_alpha{};
    std::vector<u8> cross_alpha{};
    float edge_depth_bias = 0.00001f;
    int edge_depth_texture_loc = -1;
    int edge_depth_bias_loc = -1;
    u32 edge_filter_compute_program = 0;
    u32 edge_filter_edges_ssbo = 0;
    u32 edge_filter_triangles_ssbo = 0;
    u32 edge_filter_bin_ranges_ssbo = 0;
    u32 edge_filter_bin_indices_ssbo = 0;
    u32 edge_filter_unbounded_ssbo = 0;
    u32 edge_filter_sample_jobs_ssbo = 0;
    u32 edge_filter_sample_results_ssbo = 0;
    u32 edge_filter_refine_jobs_ssbo = 0;
    u32 edge_filter_refine_results_ssbo = 0;
    u32 edge_filter_stats_ssbo = 0;
    u32 edge_filter_edges_capacity = 0;
    u32 edge_filter_triangles_capacity = 0;
    u32 edge_filter_bin_ranges_capacity = 0;
    u32 edge_filter_bin_indices_capacity = 0;
    u32 edge_filter_unbounded_capacity = 0;
    u32 edge_filter_sample_jobs_capacity = 0;
    u32 edge_filter_sample_results_capacity = 0;
    u32 edge_filter_refine_jobs_capacity = 0;
    u32 edge_filter_refine_results_capacity = 0;
    u32 edge_filter_stats_capacity = 0;
    int edge_filter_pass_id_loc = -1;
    int edge_filter_job_count_loc = -1;
    int edge_filter_edge_count_loc = -1;
    int edge_filter_triangle_count_loc = -1;
    int edge_filter_bin_count_loc = -1;
    int edge_filter_unbounded_count_loc = -1;
    int edge_filter_screen_size_loc = -1;
    int edge_filter_camera_pos_loc = -1;
    int edge_filter_brute_force_loc = -1;
    int edge_filter_scene_depth_texture_loc = -1;
    int edge_filter_after_sprites_depth_texture_loc = -1;
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
