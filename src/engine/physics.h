#pragma once

#include "engine/arena.h"
#include "engine/math.h"

#include "raylib.h"

namespace ol {

struct Dimension;

struct CollisionFilter {
    u32 group = 1;
    u32 mask = 0xffffffffu;
};

inline bool collision_filters_match(CollisionFilter a, CollisionFilter b) {
    return (a.mask & b.group) && (b.mask & a.group);
}

struct PointMass {
    WorldPos pos{};
    WorldPos prev{};
    Vector3 acceleration = {0.0f, 0.0f, 0.0f};
    float radius = 0.25f;
    float mass = 1.0f;
    float inv_mass = 1.0f;
    float friction = 0.7f;
    float bounce = 0.0f;
    bool fixed = false;
    bool collideable = true;
    bool touched_ground = false;
    CollisionFilter filter{};
    Color color = WHITE;
};

struct Link {
    u32 a = invalid_id;
    u32 b = invalid_id;
    float rest_length = 1.0f;
    float stiffness = 1.0f;
    float damping = 0.0f;
    bool collideable = false;
    bool axis_lock = false;
    bool axis_lock_anchor_a = false;
    Vector3 axis = {0.0f, 1.0f, 0.0f};
    float radius = 0.25f;
    CollisionFilter filter{};
    Color color = WHITE;
};

struct BoxCollider {
    WorldPos pos{};
    Vector3 half = {0.5f, 0.5f, 0.5f};
    Quaternion rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    bool axis_aligned = true;
    bool fixed = true;
    float friction = 0.8f;
    float bounce = 0.0f;
    CollisionFilter filter{};
    Color color = GRAY;
};

struct PhysicsCell {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;
};

struct PhysicsBucket {
    PhysicsCell cell{};
    u32 first = invalid_id;
    bool used = false;
};

struct PhysicsHashRef {
    u32 mass_id = invalid_id;
    u32 next = invalid_id;
};

struct SpatialHash {
    std::array<PhysicsBucket, physics_hash_buckets> buckets{};
    std::array<PhysicsHashRef, physics_hash_refs> refs{};
    u32 ref_count = 0;
    float cell_size = 1.5f;
};

struct PhysicsWorld {
    Arena<max_point_masses, PointMass> masses{};
    Arena<max_links, Link> links{};
    Arena<max_boxes, BoxCollider> boxes{};
    SpatialHash hash{};
    Vector3 gravity = {0.0f, -18.0f, 0.0f};
    u32 substeps = 4;
    u32 solver_iterations = 6;
};

void physics_init(PhysicsWorld* physics);
u32 physics_add_point_mass(PhysicsWorld* physics, PointMass mass);
u32 physics_add_link(PhysicsWorld* physics, Link link);
u32 physics_add_box(PhysicsWorld* physics, BoxCollider box);
bool physics_remove_box(PhysicsWorld* physics, u32 box_id);

Vector3 physics_mass_velocity(const PointMass* mass, float chunk_size, float dt);
void physics_set_mass_velocity(PointMass* mass, Vector3 velocity, float chunk_size, float dt);
void physics_apply_acceleration(PointMass* mass, Vector3 acceleration);
void physics_step(Dimension* dim, float dt);

} // namespace ol
