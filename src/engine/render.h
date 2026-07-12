#pragma once

#include "engine/world.h"

#include "raylib.h"

#include <unordered_map>
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

struct RadianceCascadeSettings {
    bool enabled = true;
    int probe_extra_levels = 0;
    int cascade_iterations = 2;
    int indirect_samples = 8;
    int shadow_samples = 2;
    int temporal_frames = 0;
    int lighting_radius_chunks = 3;
    bool jitter = false;
    bool corner_merge = false;
};

struct MeshLightingSurfaceCache {
    u32 mesh_id = invalid_id;
    u32 geometry_id = invalid_id;
    Vector3 normal = {0.0f, 1.0f, 0.0f};
    Vector3 tangent = {1.0f, 0.0f, 0.0f};
    Vector3 bitangent = {0.0f, 0.0f, 1.0f};
    float plane = 0.0f;
    i32 grid_min_u = 0;
    i32 grid_min_v = 0;
    i32 width = 0;
    i32 height = 0;
    Vector3 bounds_center{};
    float bounds_radius = 0.0f;
    float chunk_distance = 0.0f;
    float reflectivity = 0.0f;
    Vector3 emission{};
    u64 surface_signature = 0;
    u64 resolved_scene_signature = 0;
    u64 resolved_emitter_signature = 0;
    u64 resolved_paint_revision = 0;
    u64 resolved_camera_signature = 0;
    u64 resolved_player_signature = 0;
    // Exact world-space player/emitter state represented by the persistent
    // dynamic shadow mask. Keep this separate from the anchor-relative mesh
    // signature used by reflective paths.
    u64 shadow_mask_signature = 0;
    // Monotonic identity of the stable lighting texture.
    u64 stable_lighting_revision = 0;
    u64 pending_player_signature = 0;
    i32 pending_x = 0;
    i32 pending_y = 0;
    i32 pending_width = 0;
    i32 pending_height = 0;
    i32 pending_shadow_x = 0;
    i32 pending_shadow_y = 0;
    i32 pending_shadow_width = 0;
    i32 pending_shadow_height = 0;
    i32 active_shadow_x = 0;
    i32 active_shadow_y = 0;
    i32 active_shadow_width = 0;
    i32 active_shadow_height = 0;
    i32 valid_x = 0;
    i32 valid_y = 0;
    i32 valid_width = 0;
    i32 valid_height = 0;
    i32 valid_mip_level = 30;
    // The backing texture has received a complete nonblank resolve at least
    // once. valid_* separately describes coverage for the stamped signatures.
    bool fully_initialized = false;
    bool pending_dynamic_shadow_active = false;
    bool dynamic_composite_active = false;
    u32 temporal_samples = 0;
    u64 last_update_frame = 0;
    RenderTexture2D texture{};
    // A persistent one-channel player-shadow multiplier. It is deliberately
    // independent from the filtered stable colour lightmap and from temporal
    // history, so moving a player never copies a whole RGBA face.
    RenderTexture2D shadow_mask_texture{};
    // Used only for temporal accumulation when temporal_frames > 1.
    RenderTexture2D history_texture{};
    std::vector<u32> triangles{};
};

struct RadiancePlayerShadowState {
    WorldPos feet{};
    float radius = 0.0f;
    float height = 0.0f;
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
    Shader radiance_cascade_shader{};
    Shader surface_lighting_shader{};
    Shader surface_shadow_composite_shader{};
    Shader pathtrace_comparison_shader{};
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
    bool radiance_cascade_shader_ready = false;
    bool surface_lighting_shader_ready = false;
    bool surface_shadow_composite_shader_ready = false;
    bool pathtrace_comparison_shader_ready = false;
    bool pathtrace_comparison_enabled = false;
    bool pathtrace_comparison_failed = false;
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
    bool shader_depth_edges = true;
    bool gpu_depth_edges = true;
    bool frustum_cull_meshes = true;
    bool brute_force_edge_occlusion = false;
    std::unordered_map<u64, int> shader_uniform_locations{};
    // Common RC scene uniforms and SSBO bindings are invariant across the
    // dozens of face resolves submitted in one frame. Keep their GL state so
    // each face only updates its own material/plane parameters.
    std::unordered_map<u32, u64> radiance_common_uniform_signatures{};
    std::array<u32, 5> radiance_bound_scene_buffers = {
        invalid_id, invalid_id, invalid_id, invalid_id, invalid_id};
    std::vector<SpritePaintTextureCache> sprite_paint_textures{};
    std::vector<MeshPaintSurfaceCache> mesh_paint_surfaces{};
    u64 mesh_paint_revision = 0;
    u64 mesh_paint_topology_hash = 0;
    std::vector<u8> life_alpha{};
    std::vector<u8> cross_alpha{};
    RadianceCascadeSettings lighting{};
    std::vector<MeshLightingSurfaceCache> lighting_surfaces{};
    RenderTexture2D radiance_cascade_targets[2]{};
    u32 radiance_scene_triangles_ssbo = 0;
    u32 radiance_emitters_ssbo = 0;
    u32 radiance_bvh_nodes_ssbo = 0;
    u32 radiance_dynamic_triangles_ssbo = 0;
    u32 radiance_dynamic_bvh_nodes_ssbo = 0;
    u32 radiance_scene_triangles_capacity = 0;
    u32 radiance_emitters_capacity = 0;
    u32 radiance_bvh_nodes_capacity = 0;
    u32 radiance_dynamic_triangles_capacity = 0;
    u32 radiance_dynamic_bvh_nodes_capacity = 0;
    // The comparison path owns a visual-radius scene. It deliberately does not
    // borrow the close radiance-cascade BVH, so increasing render radius changes
    // the reference image without changing normal lighting cost.
    RenderTexture2D pathtrace_comparison_target{};
    u32 pathtrace_scene_triangles_ssbo = 0;
    u32 pathtrace_emitters_ssbo = 0;
    u32 pathtrace_bvh_nodes_ssbo = 0;
    u32 pathtrace_dynamic_triangles_ssbo = 0;
    u32 pathtrace_dynamic_bvh_nodes_ssbo = 0;
    u32 pathtrace_scene_triangles_capacity = 0;
    u32 pathtrace_emitters_capacity = 0;
    u32 pathtrace_bvh_nodes_capacity = 0;
    u32 pathtrace_dynamic_triangles_capacity = 0;
    u32 pathtrace_dynamic_bvh_nodes_capacity = 0;
    u32 pathtrace_triangle_count = 0;
    u32 pathtrace_emitter_triangle_count = 0;
    u32 pathtrace_emitter_mesh_count = 0;
    u32 pathtrace_bvh_node_count = 0;
    u32 pathtrace_dynamic_triangle_count = 0;
    u32 pathtrace_dynamic_bvh_node_count = 0;
    float pathtrace_emitter_total_weight = 0.0f;
    u32 pathtrace_dimension = invalid_id;
    ChunkCoord pathtrace_anchor_chunk{};
    i32 pathtrace_render_radius_chunks = -1;
    i32 pathtrace_quality_radius_chunks = -1;
    u64 pathtrace_topology_revision = 0;
    u64 pathtrace_scene_signature = 0;
    u64 pathtrace_scene_build_count = 0;
    bool pathtrace_scene_truncated = false;
    double debug_pathtrace_scene_build_ms = 0.0;
    double debug_pathtrace_scene_update_ms = 0.0;
    double debug_pathtrace_draw_ms = 0.0;
    u32 radiance_active_target = 0;
    u32 radiance_surface_cursor = 0;
    u32 radiance_cascade_temporal_samples = 0;
    u32 radiance_emitter_triangle_count = 0;
    float radiance_emitter_total_weight = 0.0f;
    u64 radiance_frame = 0;
    u64 radiance_scene_signature = 0;
    u64 radiance_emitter_signature = 0;
    u32 radiance_emitter_stable_frames = 0;
    u64 radiance_topology_signature = 0;
    u64 radiance_settings_signature = 0;
    u64 radiance_player_signature = 0;
    ChunkCoord radiance_anchor_chunk{};
    std::vector<RadiancePlayerShadowState> radiance_previous_players{};
    void* radiance_background_state = nullptr;
    int radiance_atlas_width = 0;
    int radiance_atlas_height = 0;
    u32 debug_radiance_triangle_count = 0;
    u32 debug_radiance_emitter_count = 0;
    u32 debug_radiance_bvh_node_count = 0;
    u32 debug_radiance_dynamic_triangle_count = 0;
    u32 debug_radiance_dynamic_bvh_node_count = 0;
    u64 debug_radiance_cascade_update_frame = 0;
    u32 debug_radiance_player_primitive_count = 0;
    bool debug_radiance_scene_truncated = false;
    u32 debug_radiance_surface_count = 0;
    u32 debug_radiance_surface_updates = 0;
    u32 debug_radiance_surface_allocations = 0;
    u64 debug_radiance_surface_texels_submitted = 0;
    u32 debug_radiance_common_uniform_updates = 0;
    u32 debug_radiance_common_uniform_cache_hits = 0;
    u32 debug_radiance_scene_buffer_binds = 0;
    u32 debug_radiance_dynamic_shadow_updates = 0;
    u32 debug_radiance_dynamic_shadow_allocations = 0;
    u32 debug_radiance_dynamic_shadow_mask_frees = 0;
    u64 debug_radiance_dynamic_shadow_region_texels = 0;
    u64 debug_radiance_dynamic_shadow_clear_texels = 0;
    u64 debug_radiance_dynamic_shadow_copy_texels = 0;
    // Counterfactual cost of the old whole-face stable -> scratch copy. Kept
    // beside the actual copy count so the motion benchmark reports a direct A/B.
    u64 debug_radiance_dynamic_shadow_full_copy_texels = 0;
    u32 debug_radiance_draw_candidate_surface_count = 0;
    u32 debug_radiance_draw_origin_gated_surface_count = 0;
    u32 debug_radiance_draw_unresolved_surface_count = 0;
    double debug_radiance_scene_build_ms = 0.0;
    double debug_radiance_signature_ms = 0.0;
    double debug_radiance_emitter_ms = 0.0;
    double debug_radiance_topology_ms = 0.0;
    double debug_radiance_cascade_ms = 0.0;
    bool debug_radiance_background_scene_used = false;
    bool debug_radiance_neighbor_scene_ready = false;
    bool debug_radiance_neighbor_cascade_ready = false;
    bool debug_radiance_neighbor_promoted = false;
    u32 debug_radiance_neighbor_surface_count = 0;
    u32 debug_radiance_neighbor_complete_surface_count = 0;
    u32 debug_radiance_neighbor_required_surface_count = 0;
    u32 debug_radiance_neighbor_promoted_surface_count = 0;
    u32 debug_radiance_neighbor_sync_fallback_surface_count = 0;
    u32 debug_radiance_neighbor_sync_fallback_allocation_count = 0;
    u64 debug_radiance_neighbor_sync_fallback_texels = 0;
    u32 debug_radiance_neighbor_partial_fallback_surface_count = 0;
    u32 debug_radiance_neighbor_partial_fallback_allocation_count = 0;
    u64 debug_radiance_neighbor_partial_fallback_texels = 0;
    u32 debug_radiance_neighbor_sync_fallback_support_surface_count = 0;
    u64 debug_radiance_neighbor_sync_fallback_support_texels = 0;
    u64 debug_radiance_neighbor_sync_fallback_max_surface_texels = 0;
    i32 debug_radiance_neighbor_sync_fallback_max_width = 0;
    i32 debug_radiance_neighbor_sync_fallback_max_height = 0;
    u32 debug_radiance_neighbor_retired_target_count = 0;
    u32 debug_radiance_neighbor_allocations_this_frame = 0;
    u64 debug_radiance_neighbor_texels_this_frame = 0;
    float debug_radiance_neighbor_target_distance_m = 0.0f;
    bool debug_radiance_neighbor_auto_surface_prewarm = false;
    // Optional benchmark policy. Production keeps scene/BVH/cascade staging,
    // but resolves carried visible faces after promotion instead of creating
    // hundreds of per-face FBOs while the player walks.
    bool radiance_neighbor_surface_prewarm = false;
    // A promoted adjacent anchor may seed carried face caches by resolving only
    // their conservative visible region. The carried cache is always repaired
    // at canonical level zero so its result cannot depend on which route first
    // exposed the face; old-signature texels are never drawn.
    bool radiance_anchor_partial_fallback = true;
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
    u32 debug_visible_chunk_count = 0;
    u32 debug_visible_mesh_count = 0;
    double debug_chunk_mesh_scan_ms = 0.0;
    double debug_mesh_prepare_ms = 0.0;
    double debug_world_draw_ms = 0.0;
    double debug_paint_update_ms = 0.0;
    double debug_lighting_update_ms = 0.0;
    double debug_lighting_draw_ms = 0.0;
    double debug_post_world_ms = 0.0;
};

void renderer_init(RenderState* renderer);
void renderer_shutdown(RenderState* renderer);
void renderer_change_scale(RenderState* renderer, int delta);
bool renderer_toggle_pathtrace_comparison(RenderState* renderer);
// Selects the surface-lighting resolve mip from projected screen density only.
// Kept as a pure policy function so distance-threshold regressions can be
// covered without creating a GL context.
int renderer_surface_lighting_mip_level(
    float projected_pixels_per_texel,
    int previous_mip_level);
void renderer_ensure_target(RenderState* renderer);
void renderer_draw_target_to_screen(RenderState* renderer);
void renderer_render_dimension_to_target(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id);
void renderer_draw_dimension(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id);

} // namespace ol
