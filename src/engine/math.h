#pragma once

#include "engine/base.h"

#include "raylib.h"
#include "raymath.h"

#include <cmath>
#include <ostream>

inline Vector2 operator+(Vector2 a, Vector2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vector2 operator-(Vector2 a, Vector2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vector2 operator-(Vector2 a) { return {-a.x, -a.y}; }
inline Vector2 operator*(Vector2 a, float s) { return {a.x * s, a.y * s}; }
inline Vector2 operator*(float s, Vector2 a) { return a * s; }
inline Vector2 operator/(Vector2 a, float s) { return {a.x / s, a.y / s}; }
inline void operator+=(Vector2& a, Vector2 b) { a.x += b.x; a.y += b.y; }
inline void operator-=(Vector2& a, Vector2 b) { a.x -= b.x; a.y -= b.y; }

inline Vector3 operator+(Vector3 a, Vector3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vector3 operator-(Vector3 a, Vector3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vector3 operator-(Vector3 a) { return {-a.x, -a.y, -a.z}; }
inline Vector3 operator*(Vector3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vector3 operator*(float s, Vector3 a) { return a * s; }
inline Vector3 operator/(Vector3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline void operator+=(Vector3& a, Vector3 b) { a.x += b.x; a.y += b.y; a.z += b.z; }
inline void operator-=(Vector3& a, Vector3 b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; }

inline std::ostream& operator<<(std::ostream& os, Vector3 v) {
    os << v.x << ' ' << v.y << ' ' << v.z;
    return os;
}

namespace ol {

struct ChunkCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
};

struct WorldPos {
    u32 dimension = invalid_id;
    ChunkCoord chunk{};
    Vector3 local = {0.0f, 0.0f, 0.0f};
};

inline bool chunk_equal(ChunkCoord a, ChunkCoord b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

inline ChunkCoord chunk_add(ChunkCoord a, ChunkCoord b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline void canonicalize_axis(i32* chunk_axis, float* local_axis, float chunk_size) {
    const i32 shift = static_cast<i32>(std::floor(*local_axis / chunk_size));
    if (shift != 0) {
        *chunk_axis += shift;
        *local_axis -= static_cast<float>(shift) * chunk_size;
    }
}

inline void canonicalize(WorldPos* pos, float chunk_size) {
    canonicalize_axis(&pos->chunk.x, &pos->local.x, chunk_size);
    canonicalize_axis(&pos->chunk.y, &pos->local.y, chunk_size);
    canonicalize_axis(&pos->chunk.z, &pos->local.z, chunk_size);
}

inline WorldPos make_world_pos(u32 dimension, Vector3 meters, float chunk_size) {
    WorldPos pos{};
    pos.dimension = dimension;
    pos.local = meters;
    canonicalize(&pos, chunk_size);
    return pos;
}

inline Vector3 world_delta_meters(WorldPos a, WorldPos b, float chunk_size) {
    const double dx = static_cast<double>(a.chunk.x - b.chunk.x) * chunk_size + (a.local.x - b.local.x);
    const double dy = static_cast<double>(a.chunk.y - b.chunk.y) * chunk_size + (a.local.y - b.local.y);
    const double dz = static_cast<double>(a.chunk.z - b.chunk.z) * chunk_size + (a.local.z - b.local.z);
    return {static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
}

inline void worldpos_add_delta(WorldPos* pos, Vector3 delta, float chunk_size) {
    pos->local += delta;
    canonicalize(pos, chunk_size);
}

inline WorldPos worldpos_offset(WorldPos pos, Vector3 delta, float chunk_size) {
    worldpos_add_delta(&pos, delta, chunk_size);
    return pos;
}

inline double world_axis_meters(i32 chunk, float local, float chunk_size) {
    return static_cast<double>(chunk) * static_cast<double>(chunk_size) + static_cast<double>(local);
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float safe_len(Vector3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

inline Vector3 safe_norm(Vector3 v, Vector3 fallback = {0.0f, 1.0f, 0.0f}) {
    const float len = safe_len(v);
    return len > 0.00001f ? v / len : fallback;
}

inline Color color_scaled(Color c, float s) {
    s = clampf(s, 0.0f, 4.0f);
    return {
        static_cast<unsigned char>(clampf(c.r * s, 0.0f, 255.0f)),
        static_cast<unsigned char>(clampf(c.g * s, 0.0f, 255.0f)),
        static_cast<unsigned char>(clampf(c.b * s, 0.0f, 255.0f)),
        c.a
    };
}

inline Vector3 forward_from_angles(float yaw, float pitch) {
    const float cp = std::cos(pitch);
    return {std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
}

inline Vector3 flat_forward_from_yaw(float yaw) {
    return {std::sin(yaw), 0.0f, -std::cos(yaw)};
}

inline Vector3 flat_right_from_yaw(float yaw) {
    return {std::cos(yaw), 0.0f, std::sin(yaw)};
}

inline Matrix matrix_no_translation(Matrix m) {
    m.m12 = 0.0f;
    m.m13 = 0.0f;
    m.m14 = 0.0f;
    return m;
}

} // namespace ol
