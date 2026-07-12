#pragma once

#include "engine/arena.h"
#include "engine/physics.h"

#include "raylib.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace ol {

enum BillboardType : u32 {
    billboard_none,
    billboard_vertical,
    billboard_full_3d
};

struct Triangle {
    u16 a = 0;
    u16 b = 0;
    u16 c = 0;
};

struct Edge {
    u16 a = 0;
    u16 b = 0;
    u8 thickness_px = 2;
};

struct MeshGeometry {
    char name[48]{};
    u32 vertex_count = 0;
    std::array<Vector3, max_vertices_per_geometry> vertices{};
    u32 triangle_count = 0;
    std::array<Triangle, max_triangles_per_geometry> triangles{};
    u32 edge_count = 0;
    std::array<Edge, max_edges_per_geometry> edges{};
    u32 lod_geometry = invalid_id;
};

struct MaterialLayer {
    char name[48]{};
    bool has_color = false;
    Color color = WHITE;
    bool has_texture = false;
    u32 texture_id = invalid_id;
    bool has_normal_map = false;
    u32 normal_map_id = invalid_id;
    bool has_fragment_shader = false;
    u32 fragment_shader_id = invalid_id;
    bool has_vertex_shader = false;
    u32 vertex_shader_id = invalid_id;
    bool has_emission = false;
    Color emission_color = WHITE;
    float emission = 0.0f;
    bool has_reflectivity = false;
    float reflectivity = 0.0f;
};

struct MaterialStack {
    u32 count = 0;
    std::array<MaterialLayer, max_material_layers> layers{};
};

struct MeshInstance {
    char name[48]{};
    u32 geometry = invalid_id;
    WorldPos origin{};
    Matrix se3 = MatrixIdentity();
    Color color = WHITE;
    u32 texture_id = invalid_id;
    u32 normal_map_id = invalid_id;
    u32 fragment_shader_id = invalid_id;
    u32 vertex_shader_id = invalid_id;
    MaterialStack materials{};
    float bounds_radius = 1.0f;
    bool lit = true;
    bool draw_edges = true;
    bool visible = true;
};

struct SpriteInstance {
    char name[48]{};
    WorldPos origin{};
    Matrix se3 = MatrixIdentity();
    Vector2 size = {1.0f, 1.0f};
    Color color = WHITE;
    u32 texture_id = invalid_id;
    u32 normal_map_id = invalid_id;
    u32 fragment_shader_id = invalid_id;
    BillboardType billboard = billboard_full_3d;
    bool visible = true;
};

struct PaintedPixel {
    WorldPos center{};
    Vector3 normal = {0.0f, 1.0f, 0.0f};
    Vector3 tangent = {1.0f, 0.0f, 0.0f};
    Color color = WHITE;
    Vector2 quad_offset{};
    Vector2 quad_half_size{};
    u32 mesh_id = invalid_id;
    u32 sprite_id = invalid_id;
    i32 sprite_pixel_x = 0;
    i32 sprite_pixel_y = 0;
};

struct Chunk {
    ChunkCoord coord{};
    u32 mesh_count = 0;
    std::array<u32, max_mesh_refs_per_chunk> meshes{};
    u32 sprite_count = 0;
    std::array<u32, max_sprite_refs_per_chunk> sprites{};
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& coord) const noexcept {
        u64 hash = 1469598103934665603ull;
        auto add = [&](u32 value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        add(static_cast<u32>(coord.x));
        add(static_cast<u32>(coord.y));
        add(static_cast<u32>(coord.z));
        return static_cast<size_t>(hash);
    }
};

struct ChunkCoordEqual {
    bool operator()(const ChunkCoord& a, const ChunkCoord& b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

struct PlayerEntity {
    char name[32]{};
    Color color = SKYBLUE;
    u32 bottom_mass = invalid_id;
    u32 top_mass = invalid_id;
    u32 body_link = invalid_id;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float body_radius = 0.35f;
    float current_height = 1.80f;
    float eye_height = 1.70f;
    float camera_y_offset = 0.0f;
    Vector3 velocity = {0.0f, 0.0f, 0.0f};
    WorldPos jump_start_feet{};
    float jump_hold_time = 0.0f;
    u32 grounded_frames = 0;
    bool jump_variable_active = false;
    bool on_ground = false;
    bool is_crouching = false;
    bool connected = true;
    bool local = false;
    bool aim_ray_active = false;
    WorldPos aim_ray_start{};
    WorldPos aim_ray_end{};
};

struct Dimension {
    char name[64]{};
    float chunk_size_m = 16.0f;
    float meter_size_px = 64.0f;
    float pixels_per_meter = 16.0f;
    i32 render_radius_chunks = 6;
    i32 quality_render_radius_chunks = 3;
    float ambient = 0.32f;
    Color sky_top = {92, 137, 202, 255};
    Color sky_bottom = {198, 218, 242, 255};
    Color fog_color = {182, 204, 222, 255};

    Arena<max_chunks, Chunk> chunks{};
    std::unordered_map<ChunkCoord, u32, ChunkCoordHash, ChunkCoordEqual> chunk_lookup{};
    Arena<max_mesh_geometries, MeshGeometry> geometries{};
    Arena<max_meshes, MeshInstance> meshes{};
    Arena<max_sprites, SpriteInstance> sprites{};
    Arena<max_players, PlayerEntity> players{};
    std::vector<PaintedPixel> painted_pixels{};
    u64 paint_revision = 1;
    u64 mesh_topology_revision = 1;
    PhysicsWorld physics{};
};

struct World {
    Arena<max_dimensions, Dimension> dimensions{};
};

void world_init(World* world);
u32 world_add_dimension(World* world, const char* name, float chunk_size_m, float meter_size_px);
Dimension* world_get_dimension(World* world, u32 dimension_id);
const Dimension* world_get_dimension(const World* world, u32 dimension_id);

u32 dimension_find_chunk(const Dimension* dim, ChunkCoord coord);
u32 dimension_get_or_add_chunk(Dimension* dim, ChunkCoord coord);
u32 dimension_add_geometry(Dimension* dim, const MeshGeometry& geometry);
u32 dimension_add_mesh(Dimension* dim, MeshInstance mesh);
u32 dimension_add_sprite(Dimension* dim, SpriteInstance sprite);
bool dimension_paint_pixel(Dimension* dim, PaintedPixel pixel);
u32 dimension_erase_pixels(Dimension* dim, const PaintedPixel& target, float radius_pixels);
bool dimension_remove_mesh(Dimension* dim, u32 mesh_id);
u32 dimension_add_player(Dimension* dim, const char* name, Color color, WorldPos feet_pos, bool local);

MeshGeometry make_box_geometry(const char* name, Vector3 size);
MeshGeometry make_wedge_geometry(const char* name, Vector3 size);
void material_stack_push(MaterialStack* stack, MaterialLayer layer);
Color resolve_mesh_color(const MeshInstance* mesh);
Vector3 resolve_mesh_emission(const MeshInstance* mesh);
float resolve_mesh_reflectivity(const MeshInstance* mesh);
WorldPos player_feet_pos(const Dimension* dim, const PlayerEntity* player);
WorldPos player_eye_pos(const Dimension* dim, const PlayerEntity* player);

} // namespace ol
