#pragma once

#include <cstddef>
#include <cstdint>

namespace ol {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

constexpr u32 invalid_id = 0xffffffffu;

constexpr u32 max_dimensions = 8;
constexpr u32 max_chunks = 2048;
constexpr u32 max_mesh_geometries = 256;
constexpr u32 max_meshes = 4096;
constexpr u32 max_sprites = 1024;
constexpr u32 max_lights = 512;
constexpr u32 max_material_layers = 8;
constexpr u32 max_players = 16;

constexpr u32 max_vertices_per_geometry = 96;
constexpr u32 max_triangles_per_geometry = 160;
constexpr u32 max_edges_per_geometry = 160;
constexpr u32 max_render_edges = 8192;

constexpr u32 max_mesh_refs_per_chunk = 96;
constexpr u32 max_sprite_refs_per_chunk = 64;
constexpr u32 max_light_refs_per_chunk = 32;

constexpr u32 max_point_masses = 2048;
constexpr u32 max_links = 2048;
constexpr u32 max_boxes = 1024;
constexpr u32 physics_hash_buckets = 4096;
constexpr u32 physics_hash_refs = max_point_masses;

inline bool id_valid(u32 id) {
    return id != invalid_id;
}

} // namespace ol
