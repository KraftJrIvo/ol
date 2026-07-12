#include "engine/world.h"

#include "engine/player_controller.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ol {

static u64 next_paint_revision = 1;

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
    dim->chunk_lookup.clear();
    arena_clear(&dim->geometries);
    arena_clear(&dim->meshes);
    arena_clear(&dim->sprites);
    arena_clear(&dim->players);
    dim->painted_pixels.clear();
    dim->paint_revision = ++next_paint_revision;
    dim->mesh_topology_revision = 1;

    copy_name(dim->name, sizeof(dim->name), name);
    dim->chunk_size_m = chunk_size_m;
    dim->meter_size_px = meter_size_px;
    dim->pixels_per_meter = 16.0f;
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
    if (!dim) return invalid_id;
    const auto found = dim->chunk_lookup.find(coord);
    if (found == dim->chunk_lookup.end() || !arena_has(&dim->chunks, found->second)) {
        return invalid_id;
    }
    return found->second;
}

u32 dimension_get_or_add_chunk(Dimension* dim, ChunkCoord coord) {
    const u32 existing = dimension_find_chunk(dim, coord);
    if (id_valid(existing)) return existing;

    Chunk chunk{};
    chunk.coord = coord;
    chunk.meshes.fill(invalid_id);
    chunk.sprites.fill(invalid_id);
    const u32 id = arena_acquire(&dim->chunks, chunk);
    if (id_valid(id)) dim->chunk_lookup[coord] = id;
    return id;
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

static bool chunk_remove_ref(u32* refs, u32* count, u32 id) {
    if (!refs || !count) return false;
    for (u32 i = 0; i < *count; ++i) {
        if (refs[i] != id) continue;
        const u32 last = *count - 1;
        refs[i] = refs[last];
        refs[last] = invalid_id;
        --(*count);
        return true;
    }
    return false;
}

u32 dimension_add_geometry(Dimension* dim, const MeshGeometry& geometry) {
    return arena_acquire(&dim->geometries, geometry);
}

u32 dimension_add_mesh(Dimension* dim, MeshInstance mesh) {
    canonicalize(&mesh.origin, dim->chunk_size_m);
    const u32 id = arena_acquire(&dim->meshes, mesh);
    if (!id_valid(id)) return invalid_id;
    ++dim->mesh_topology_revision;

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

bool dimension_paint_pixel(Dimension* dim, PaintedPixel pixel) {
    if (!dim) return false;
    canonicalize(&pixel.center, dim->chunk_size_m);
    const float same_pixel_epsilon = 0.25f / fmaxf(dim->pixels_per_meter, 1.0f);
    for (u32 i = 0; i < dim->painted_pixels.size(); ++i) {
        PaintedPixel& existing = dim->painted_pixels[i];
        if (id_valid(pixel.sprite_id) || id_valid(existing.sprite_id)) {
            if (existing.sprite_id == pixel.sprite_id && existing.sprite_pixel_x == pixel.sprite_pixel_x &&
                existing.sprite_pixel_y == pixel.sprite_pixel_y) {
                existing = pixel;
                dim->paint_revision = ++next_paint_revision;
                return true;
            }
            continue;
        }
        if (id_valid(pixel.mesh_id) && id_valid(existing.mesh_id) && pixel.mesh_id != existing.mesh_id &&
            arena_has(&dim->meshes, pixel.mesh_id) && arena_has(&dim->meshes, existing.mesh_id)) continue;
        if (Vector3DotProduct(existing.normal, pixel.normal) < 0.99f) continue;
        if (safe_len(world_delta_meters(existing.center, pixel.center, dim->chunk_size_m)) > same_pixel_epsilon) continue;
        existing = pixel;
        dim->paint_revision = ++next_paint_revision;
        return true;
    }
    dim->painted_pixels.push_back(pixel);
    dim->paint_revision = ++next_paint_revision;
    return true;
}

u32 dimension_erase_pixels(Dimension* dim, const PaintedPixel& target, float radius_pixels) {
    if (!dim || radius_pixels <= 0.0f) return 0;
    const float radius_m = radius_pixels / fmaxf(dim->pixels_per_meter, 1.0f);
    u32 removed = 0;
    for (u32 i = 0; i < dim->painted_pixels.size();) {
        const PaintedPixel& pixel = dim->painted_pixels[i];
        bool erase = false;
        if (id_valid(target.sprite_id)) {
            if (pixel.sprite_id == target.sprite_id) {
                const float dx = static_cast<float>(pixel.sprite_pixel_x - target.sprite_pixel_x);
                const float dy = static_cast<float>(pixel.sprite_pixel_y - target.sprite_pixel_y);
                erase = dx * dx + dy * dy <= radius_pixels * radius_pixels;
            }
        } else if (!id_valid(pixel.sprite_id) &&
            (!id_valid(target.mesh_id) || !id_valid(pixel.mesh_id) || pixel.mesh_id == target.mesh_id ||
                !arena_has(&dim->meshes, pixel.mesh_id)) &&
            Vector3DotProduct(pixel.normal, target.normal) > 0.99f) {
            const Vector3 delta = world_delta_meters(pixel.center, target.center, dim->chunk_size_m);
            const float plane_distance = std::fabs(Vector3DotProduct(delta, target.normal));
            const Vector3 planar = delta - target.normal * Vector3DotProduct(delta, target.normal);
            erase = plane_distance < 0.02f && safe_len(planar) <= radius_m;
        }
        if (!erase) {
            ++i;
            continue;
        }
        dim->painted_pixels[i] = dim->painted_pixels.back();
        dim->painted_pixels.pop_back();
        ++removed;
    }
    if (removed > 0) dim->paint_revision = ++next_paint_revision;
    return removed;
}

bool dimension_remove_mesh(Dimension* dim, u32 mesh_id) {
    if (!dim || !arena_has(&dim->meshes, mesh_id)) return false;

    u32 empty_chunk_id = invalid_id;
    for (u32 slot = 0; slot < dim->chunks.count; ++slot) {
        Chunk* chunk = &dim->chunks.data[slot];
        if (chunk_remove_ref(chunk->meshes.data(), &chunk->mesh_count, mesh_id)) {
            if (chunk->mesh_count == 0 && chunk->sprite_count == 0) {
                empty_chunk_id = arena_id_at_slot(&dim->chunks, slot);
            }
            break;
        }
    }

    const bool removed = arena_remove(&dim->meshes, mesh_id);
    if (removed) ++dim->mesh_topology_revision;
    if (removed && id_valid(empty_chunk_id)) {
        if (const Chunk* chunk = arena_get(&dim->chunks, empty_chunk_id)) {
            dim->chunk_lookup.erase(chunk->coord);
        }
        arena_remove(&dim->chunks, empty_chunk_id);
    }
    return removed;
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

Vector3 resolve_mesh_emission(const MeshInstance* mesh) {
    Vector3 emission{};
    if (!mesh) return emission;
    for (u32 i = 0; i < mesh->materials.count; ++i) {
        const MaterialLayer& layer = mesh->materials.layers[i];
        if (!layer.has_emission) continue;
        const float strength = fmaxf(0.0f, layer.emission);
        emission = {
            std::pow(static_cast<float>(layer.emission_color.r) / 255.0f, 2.2f) * strength,
            std::pow(static_cast<float>(layer.emission_color.g) / 255.0f, 2.2f) * strength,
            std::pow(static_cast<float>(layer.emission_color.b) / 255.0f, 2.2f) * strength
        };
    }
    return emission;
}

float resolve_mesh_reflectivity(const MeshInstance* mesh) {
    float reflectivity = 0.0f;
    if (!mesh) return reflectivity;
    for (u32 i = 0; i < mesh->materials.count; ++i) {
        const MaterialLayer& layer = mesh->materials.layers[i];
        if (layer.has_reflectivity) reflectivity = clampf(layer.reflectivity, 0.0f, 1.0f);
    }
    return reflectivity;
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
