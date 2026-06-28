#include "engine/world.h"

#include "engine/player_controller.h"

#include <cstdio>
#include <cstring>

namespace ol {

static void copy_name(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    std::snprintf(dst, dst_size, "%s", src ? src : "");
}

void world_init(World* world) {
    arena_clear(&world->dimensions);
}

u32 world_add_dimension(World* world, const char* name, float chunk_size_m, float meter_size_px) {
    const u32 id = arena_reserve(&world->dimensions);
    if (!id_valid(id)) return invalid_id;

    Dimension* dim = arena_get(&world->dimensions, id);
    arena_clear(&dim->chunks);
    arena_clear(&dim->geometries);
    arena_clear(&dim->meshes);
    arena_clear(&dim->sprites);
    arena_clear(&dim->lights);
    arena_clear(&dim->players);

    copy_name(dim->name, sizeof(dim->name), name);
    dim->chunk_size_m = chunk_size_m;
    dim->meter_size_px = meter_size_px;
    dim->render_radius_chunks = 6;
    dim->quality_render_radius_chunks = 3;
    dim->ambient = 0.32f;
    dim->sky_top = {92, 137, 202, 255};
    dim->sky_bottom = {198, 218, 242, 255};
    dim->fog_color = {182, 204, 222, 255};
    physics_init(&dim->physics);
    return id;
}

Dimension* world_get_dimension(World* world, u32 dimension_id) {
    return arena_get(&world->dimensions, dimension_id);
}

const Dimension* world_get_dimension(const World* world, u32 dimension_id) {
    return arena_get(&world->dimensions, dimension_id);
}

u32 dimension_find_chunk(const Dimension* dim, ChunkCoord coord) {
    for (u32 slot = 0; slot < dim->chunks.count; ++slot) {
        if (chunk_equal(dim->chunks.data[slot].coord, coord)) {
            return arena_id_at_slot(&dim->chunks, slot);
        }
    }
    return invalid_id;
}

u32 dimension_get_or_add_chunk(Dimension* dim, ChunkCoord coord) {
    const u32 existing = dimension_find_chunk(dim, coord);
    if (id_valid(existing)) return existing;

    Chunk chunk{};
    chunk.coord = coord;
    chunk.meshes.fill(invalid_id);
    chunk.sprites.fill(invalid_id);
    chunk.lights.fill(invalid_id);
    return arena_acquire(&dim->chunks, chunk);
}

static void chunk_add_mesh_ref(Chunk* chunk, u32 id) {
    if (chunk->mesh_count < max_mesh_refs_per_chunk) {
        chunk->meshes[chunk->mesh_count++] = id;
    }
}

static void chunk_add_sprite_ref(Chunk* chunk, u32 id) {
    if (chunk->sprite_count < max_sprite_refs_per_chunk) {
        chunk->sprites[chunk->sprite_count++] = id;
    }
}

static void chunk_add_light_ref(Chunk* chunk, u32 id) {
    if (chunk->light_count < max_light_refs_per_chunk) {
        chunk->lights[chunk->light_count++] = id;
    }
}

u32 dimension_add_geometry(Dimension* dim, const MeshGeometry& geometry) {
    return arena_acquire(&dim->geometries, geometry);
}

u32 dimension_add_mesh(Dimension* dim, MeshInstance mesh) {
    canonicalize(&mesh.origin, dim->chunk_size_m);
    const u32 id = arena_acquire(&dim->meshes, mesh);
    if (!id_valid(id)) return invalid_id;

    const u32 chunk_id = dimension_get_or_add_chunk(dim, mesh.origin.chunk);
    Chunk* chunk = arena_get(&dim->chunks, chunk_id);
    if (chunk) chunk_add_mesh_ref(chunk, id);
    return id;
}

u32 dimension_add_sprite(Dimension* dim, SpriteInstance sprite) {
    canonicalize(&sprite.origin, dim->chunk_size_m);
    const u32 id = arena_acquire(&dim->sprites, sprite);
    if (!id_valid(id)) return invalid_id;

    const u32 chunk_id = dimension_get_or_add_chunk(dim, sprite.origin.chunk);
    Chunk* chunk = arena_get(&dim->chunks, chunk_id);
    if (chunk) chunk_add_sprite_ref(chunk, id);
    return id;
}

u32 dimension_add_light(Dimension* dim, LightSource light) {
    canonicalize(&light.pos, dim->chunk_size_m);
    const u32 id = arena_acquire(&dim->lights, light);
    if (!id_valid(id)) return invalid_id;

    const u32 chunk_id = dimension_get_or_add_chunk(dim, light.pos.chunk);
    Chunk* chunk = arena_get(&dim->chunks, chunk_id);
    if (chunk) chunk_add_light_ref(chunk, id);
    return id;
}

u32 dimension_add_player(Dimension* dim, const char* name, Color color, WorldPos feet_pos, bool local) {
    canonicalize(&feet_pos, dim->chunk_size_m);

    PointMass bottom{};
    bottom.pos = worldpos_offset(feet_pos, {0.0f, player_radius_m, 0.0f}, dim->chunk_size_m);
    bottom.prev = bottom.pos;
    bottom.radius = player_radius_m;
    bottom.mass = 1.0f;
    bottom.inv_mass = 1.0f;
    bottom.friction = 0.85f;
    bottom.fixed = true;
    bottom.collideable = false;
    bottom.color = color;

    PointMass top = bottom;
    top.pos = worldpos_offset(feet_pos, {0.0f, player_stand_height_m - player_radius_m, 0.0f}, dim->chunk_size_m);
    top.prev = top.pos;

    const u32 bottom_id = physics_add_point_mass(&dim->physics, bottom);
    const u32 top_id = physics_add_point_mass(&dim->physics, top);

    Link body{};
    body.a = bottom_id;
    body.b = top_id;
    body.rest_length = player_stand_height_m - 2.0f * player_radius_m;
    body.stiffness = 1.0f;
    body.axis_lock = true;
    body.axis_lock_anchor_a = true;
    body.axis = {0.0f, 1.0f, 0.0f};
    body.collideable = false;
    body.radius = bottom.radius;
    body.color = color;
    const u32 link_id = physics_add_link(&dim->physics, body);

    PlayerEntity player{};
    copy_name(player.name, sizeof(player.name), name);
    player.color = color;
    player.bottom_mass = bottom_id;
    player.top_mass = top_id;
    player.body_link = link_id;
    player.body_radius = player_radius_m;
    player.current_height = player_stand_height_m;
    player.eye_height = player_stand_eye_m;
    player.local = local;
    return arena_acquire(&dim->players, player);
}

MeshGeometry make_box_geometry(const char* name, Vector3 size) {
    MeshGeometry g{};
    copy_name(g.name, sizeof(g.name), name);
    const Vector3 h = size * 0.5f;
    g.vertex_count = 8;
    g.vertices[0] = {-h.x, -h.y, -h.z};
    g.vertices[1] = { h.x, -h.y, -h.z};
    g.vertices[2] = { h.x,  h.y, -h.z};
    g.vertices[3] = {-h.x,  h.y, -h.z};
    g.vertices[4] = {-h.x, -h.y,  h.z};
    g.vertices[5] = { h.x, -h.y,  h.z};
    g.vertices[6] = { h.x,  h.y,  h.z};
    g.vertices[7] = {-h.x,  h.y,  h.z};

    const Triangle tris[] = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {3, 6, 2}, {3, 7, 6},
        {1, 2, 6}, {1, 6, 5},
        {0, 4, 7}, {0, 7, 3},
    };
    g.triangle_count = static_cast<u32>(sizeof(tris) / sizeof(tris[0]));
    for (u32 i = 0; i < g.triangle_count; ++i) g.triangles[i] = tris[i];

    const Edge edges[] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    g.edge_count = static_cast<u32>(sizeof(edges) / sizeof(edges[0]));
    for (u32 i = 0; i < g.edge_count; ++i) g.edges[i] = edges[i];
    return g;
}

MeshGeometry make_wedge_geometry(const char* name, Vector3 size) {
    MeshGeometry g{};
    copy_name(g.name, sizeof(g.name), name);
    const Vector3 h = size * 0.5f;
    g.vertex_count = 6;
    g.vertices[0] = {-h.x, -h.y, -h.z};
    g.vertices[1] = { h.x, -h.y, -h.z};
    g.vertices[2] = {-h.x, -h.y,  h.z};
    g.vertices[3] = { h.x, -h.y,  h.z};
    g.vertices[4] = {-h.x,  h.y,  h.z};
    g.vertices[5] = { h.x,  h.y,  h.z};

    const Triangle tris[] = {
        {0, 1, 3}, {0, 3, 2},
        {2, 3, 5}, {2, 5, 4},
        {0, 2, 4},
        {1, 5, 3},
        {0, 4, 5}, {0, 5, 1},
    };
    g.triangle_count = static_cast<u32>(sizeof(tris) / sizeof(tris[0]));
    for (u32 i = 0; i < g.triangle_count; ++i) g.triangles[i] = tris[i];

    const Edge edges[] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {2, 4}, {3, 5}, {4, 5}, {0, 4}, {1, 5},
    };
    g.edge_count = static_cast<u32>(sizeof(edges) / sizeof(edges[0]));
    for (u32 i = 0; i < g.edge_count; ++i) g.edges[i] = edges[i];
    return g;
}

void material_stack_push(MaterialStack* stack, MaterialLayer layer) {
    if (stack->count < max_material_layers) {
        stack->layers[stack->count++] = layer;
    }
}

Color resolve_mesh_color(const MeshInstance* mesh) {
    Color color = mesh->color;
    for (u32 i = 0; i < mesh->materials.count; ++i) {
        if (mesh->materials.layers[i].has_color) {
            color = mesh->materials.layers[i].color;
        }
    }
    return color;
}

WorldPos player_feet_pos(const Dimension* dim, const PlayerEntity* player) {
    const PointMass* bottom = arena_get(&dim->physics.masses, player->bottom_mass);
    if (!bottom) return {};
    return worldpos_offset(bottom->pos, {0.0f, -bottom->radius, 0.0f}, dim->chunk_size_m);
}

WorldPos player_eye_pos(const Dimension* dim, const PlayerEntity* player) {
    WorldPos feet = player_feet_pos(dim, player);
    if (!id_valid(feet.dimension)) return {};
    return worldpos_offset(feet, {0.0f, player->eye_height, 0.0f}, dim->chunk_size_m);
}

} // namespace ol
