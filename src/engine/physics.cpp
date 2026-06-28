#include "engine/physics.h"

#include "engine/world.h"

#include <cmath>

namespace ol {

static u64 hash_cell(PhysicsCell c) {
    u64 x = static_cast<u64>(c.x) * 0x9E3779B185EBCA87ull;
    u64 y = static_cast<u64>(c.y) * 0xC2B2AE3D27D4EB4Full;
    u64 z = static_cast<u64>(c.z) * 0x165667B19E3779F9ull;
    u64 h = x ^ (y + 0x9E3779B97F4A7C15ull + (x << 6) + (x >> 2)) ^ z;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

static bool cell_equal(PhysicsCell a, PhysicsCell b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static PhysicsCell physics_cell_for_pos(WorldPos pos, float chunk_size, float cell_size) {
    return {
        static_cast<i64>(std::floor(world_axis_meters(pos.chunk.x, pos.local.x, chunk_size) / cell_size)),
        static_cast<i64>(std::floor(world_axis_meters(pos.chunk.y, pos.local.y, chunk_size) / cell_size)),
        static_cast<i64>(std::floor(world_axis_meters(pos.chunk.z, pos.local.z, chunk_size) / cell_size)),
    };
}

static u32 hash_find_bucket(const SpatialHash* hash, PhysicsCell cell) {
    u32 start = static_cast<u32>(hash_cell(cell) & (physics_hash_buckets - 1));
    for (u32 probe = 0; probe < physics_hash_buckets; ++probe) {
        const u32 idx = (start + probe) & (physics_hash_buckets - 1);
        const PhysicsBucket* bucket = &hash->buckets[idx];
        if (!bucket->used) return invalid_id;
        if (cell_equal(bucket->cell, cell)) return idx;
    }
    return invalid_id;
}

static u32 hash_get_or_add_bucket(SpatialHash* hash, PhysicsCell cell) {
    u32 start = static_cast<u32>(hash_cell(cell) & (physics_hash_buckets - 1));
    for (u32 probe = 0; probe < physics_hash_buckets; ++probe) {
        const u32 idx = (start + probe) & (physics_hash_buckets - 1);
        PhysicsBucket* bucket = &hash->buckets[idx];
        if (!bucket->used) {
            bucket->used = true;
            bucket->cell = cell;
            bucket->first = invalid_id;
            return idx;
        }
        if (cell_equal(bucket->cell, cell)) return idx;
    }
    return invalid_id;
}

static void spatial_hash_clear(SpatialHash* hash) {
    for (PhysicsBucket& bucket : hash->buckets) {
        bucket.used = false;
        bucket.first = invalid_id;
    }
    hash->ref_count = 0;
}

static void spatial_hash_insert(SpatialHash* hash, PhysicsCell cell, u32 mass_id) {
    if (hash->ref_count >= physics_hash_refs) return;
    const u32 bucket_id = hash_get_or_add_bucket(hash, cell);
    if (!id_valid(bucket_id)) return;

    const u32 ref_id = hash->ref_count++;
    hash->refs[ref_id].mass_id = mass_id;
    hash->refs[ref_id].next = hash->buckets[bucket_id].first;
    hash->buckets[bucket_id].first = ref_id;
}

static void rebuild_spatial_hash(Dimension* dim) {
    SpatialHash* hash = &dim->physics.hash;
    spatial_hash_clear(hash);
    for (u32 slot = 0; slot < dim->physics.masses.count; ++slot) {
        const u32 id = arena_id_at_slot(&dim->physics.masses, slot);
        const PointMass* mass = &dim->physics.masses.data[slot];
        if (!mass->collideable) continue;
        spatial_hash_insert(hash, physics_cell_for_pos(mass->pos, dim->chunk_size_m, hash->cell_size), id);
    }
}

void physics_init(PhysicsWorld* physics) {
    arena_clear(&physics->masses);
    arena_clear(&physics->links);
    arena_clear(&physics->boxes);
    spatial_hash_clear(&physics->hash);
    physics->hash.cell_size = 1.5f;
    physics->gravity = {0.0f, -18.0f, 0.0f};
    physics->substeps = 4;
    physics->solver_iterations = 6;
}

u32 physics_add_point_mass(PhysicsWorld* physics, PointMass mass) {
    if (!id_valid(mass.prev.dimension)) {
        mass.prev = mass.pos;
    }
    if (mass.mass <= 0.0f || mass.fixed) {
        mass.inv_mass = 0.0f;
    } else {
        mass.inv_mass = 1.0f / mass.mass;
    }
    return arena_acquire(&physics->masses, mass);
}

u32 physics_add_link(PhysicsWorld* physics, Link link) {
    link.axis = safe_norm(link.axis, {0.0f, 1.0f, 0.0f});
    return arena_acquire(&physics->links, link);
}

u32 physics_add_box(PhysicsWorld* physics, BoxCollider box) {
    return arena_acquire(&physics->boxes, box);
}

Vector3 physics_mass_velocity(const PointMass* mass, float chunk_size, float dt) {
    if (dt <= 0.0f) return Vector3Zero();
    return world_delta_meters(mass->pos, mass->prev, chunk_size) / dt;
}

void physics_set_mass_velocity(PointMass* mass, Vector3 velocity, float chunk_size, float dt) {
    mass->prev = worldpos_offset(mass->pos, -velocity * dt, chunk_size);
}

void physics_apply_acceleration(PointMass* mass, Vector3 acceleration) {
    if (!mass->fixed) {
        mass->acceleration += acceleration;
    }
}

static void integrate_masses(Dimension* dim, float dt) {
    for (u32 slot = 0; slot < dim->physics.masses.count; ++slot) {
        PointMass* mass = &dim->physics.masses.data[slot];
        mass->touched_ground = false;
        if (mass->fixed) {
            mass->prev = mass->pos;
            mass->acceleration = Vector3Zero();
            continue;
        }

        mass->acceleration += dim->physics.gravity;
        const Vector3 displacement = world_delta_meters(mass->pos, mass->prev, dim->chunk_size_m);
        WorldPos old_pos = mass->pos;
        worldpos_add_delta(&mass->pos, displacement + mass->acceleration * (dt * dt), dim->chunk_size_m);
        mass->prev = old_pos;
        mass->acceleration = Vector3Zero();
    }
}

static float mass_share(const PointMass* a, const PointMass* b) {
    const float total = a->inv_mass + b->inv_mass;
    if (total <= 0.0f) return 0.5f;
    return a->inv_mass / total;
}

static void move_mass(PointMass* mass, Vector3 delta, float chunk_size) {
    if (!mass || mass->fixed || mass->inv_mass == 0.0f) return;
    worldpos_add_delta(&mass->pos, delta, chunk_size);
}

static void solve_links(Dimension* dim, float dt) {
    (void)dt;
    for (u32 slot = 0; slot < dim->physics.links.count; ++slot) {
        Link* link = &dim->physics.links.data[slot];
        PointMass* a = arena_get(&dim->physics.masses, link->a);
        PointMass* b = arena_get(&dim->physics.masses, link->b);
        if (!a || !b) continue;

        Vector3 delta = world_delta_meters(b->pos, a->pos, dim->chunk_size_m);
        float dist = safe_len(delta);
        if (dist <= 0.00001f) continue;

        const float share_a = mass_share(a, b);
        const float share_b = 1.0f - share_a;

        if (link->axis_lock) {
            const float sign = Vector3DotProduct(delta, link->axis) < 0.0f ? -1.0f : 1.0f;
            const Vector3 desired = link->axis * (link->rest_length * sign);
            const Vector3 error = delta - desired;
            if (link->axis_lock_anchor_a) {
                move_mass(b, -error * link->stiffness, dim->chunk_size_m);
            } else {
                move_mass(a, error * share_a * link->stiffness, dim->chunk_size_m);
                move_mass(b, -error * share_b * link->stiffness, dim->chunk_size_m);
            }
            continue;
        }

        const float error = dist - link->rest_length;
        const Vector3 correction = safe_norm(delta) * (error * link->stiffness);
        move_mass(a, correction * share_a, dim->chunk_size_m);
        move_mass(b, -correction * share_b, dim->chunk_size_m);
    }
}

static Vector3 closest_point_aabb(Vector3 p, Vector3 half) {
    return {
        clampf(p.x, -half.x, half.x),
        clampf(p.y, -half.y, half.y),
        clampf(p.z, -half.z, half.z),
    };
}

static bool sphere_box_correction(Vector3 sphere_center, float radius, Vector3 half, Vector3* correction, Vector3* normal) {
    const Vector3 closest = closest_point_aabb(sphere_center, half);
    Vector3 diff = sphere_center - closest;
    float dist = safe_len(diff);

    if (dist > 0.00001f) {
        const float penetration = radius - dist;
        if (penetration <= 0.0f) return false;
        *normal = diff / dist;
        *correction = *normal * penetration;
        return true;
    }

    const float px = half.x - std::fabs(sphere_center.x);
    const float py = half.y - std::fabs(sphere_center.y);
    const float pz = half.z - std::fabs(sphere_center.z);

    if (px <= py && px <= pz) {
        *normal = {sphere_center.x < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
        *correction = *normal * (px + radius);
    } else if (py <= px && py <= pz) {
        *normal = {0.0f, sphere_center.y < 0.0f ? -1.0f : 1.0f, 0.0f};
        *correction = *normal * (py + radius);
    } else {
        *normal = {0.0f, 0.0f, sphere_center.z < 0.0f ? -1.0f : 1.0f};
        *correction = *normal * (pz + radius);
    }
    return true;
}

static void collide_mass_box(Dimension* dim, PointMass* mass, const BoxCollider* box) {
    if (!mass || mass->fixed || !mass->collideable) return;
    if (!collision_filters_match(mass->filter, box->filter)) return;

    Vector3 rel = world_delta_meters(mass->pos, box->pos, dim->chunk_size_m);
    if (!box->axis_aligned) {
        rel = Vector3RotateByQuaternion(rel, QuaternionInvert(box->rotation));
    }

    Vector3 correction{};
    Vector3 normal{};
    if (!sphere_box_correction(rel, mass->radius, box->half, &correction, &normal)) return;

    if (!box->axis_aligned) {
        correction = Vector3RotateByQuaternion(correction, box->rotation);
        normal = Vector3RotateByQuaternion(normal, box->rotation);
    }

    move_mass(mass, correction * (1.0f - mass->bounce * box->bounce), dim->chunk_size_m);
    if (normal.y > 0.45f) mass->touched_ground = true;

    Vector3 velocity = physics_mass_velocity(mass, dim->chunk_size_m, 1.0f / 60.0f);
    Vector3 tangential = velocity - normal * Vector3DotProduct(velocity, normal);
    mass->prev = worldpos_offset(mass->prev, tangential * (mass->friction * box->friction * 0.012f), dim->chunk_size_m);
}

static void collide_mass_mass(Dimension* dim, PointMass* a, PointMass* b) {
    if (!a || !b || !a->collideable || !b->collideable) return;
    if (a->fixed && b->fixed) return;
    if (!collision_filters_match(a->filter, b->filter)) return;

    const Vector3 delta = world_delta_meters(b->pos, a->pos, dim->chunk_size_m);
    const float dist = safe_len(delta);
    const float min_dist = a->radius + b->radius;
    if (dist <= 0.00001f || dist >= min_dist) return;

    const Vector3 n = delta / dist;
    const float penetration = min_dist - dist;
    const float share_a = mass_share(a, b);
    const float share_b = 1.0f - share_a;
    move_mass(a, -n * penetration * share_a, dim->chunk_size_m);
    move_mass(b, n * penetration * share_b, dim->chunk_size_m);
}

static void collide_link_box(Dimension* dim, Link* link, const BoxCollider* box) {
    if (!link->collideable) return;
    if (!collision_filters_match(link->filter, box->filter)) return;
    PointMass* a = arena_get(&dim->physics.masses, link->a);
    PointMass* b = arena_get(&dim->physics.masses, link->b);
    if (!a || !b) return;

    const float samples[] = {0.25f, 0.5f, 0.75f};
    for (float t : samples) {
        WorldPos sample = a->pos;
        Vector3 ab = world_delta_meters(b->pos, a->pos, dim->chunk_size_m);
        worldpos_add_delta(&sample, ab * t, dim->chunk_size_m);
        Vector3 rel = world_delta_meters(sample, box->pos, dim->chunk_size_m);
        if (!box->axis_aligned) rel = Vector3RotateByQuaternion(rel, QuaternionInvert(box->rotation));

        Vector3 correction{};
        Vector3 normal{};
        if (!sphere_box_correction(rel, link->radius, box->half, &correction, &normal)) continue;
        if (!box->axis_aligned) {
            correction = Vector3RotateByQuaternion(correction, box->rotation);
            normal = Vector3RotateByQuaternion(normal, box->rotation);
        }

        move_mass(a, correction * (1.0f - t), dim->chunk_size_m);
        move_mass(b, correction * t, dim->chunk_size_m);
        if (normal.y > 0.45f) {
            a->touched_ground = true;
            b->touched_ground = true;
        }
    }
}

static void collide_all(Dimension* dim) {
    for (u32 mass_slot = 0; mass_slot < dim->physics.masses.count; ++mass_slot) {
        const u32 mass_id = arena_id_at_slot(&dim->physics.masses, mass_slot);
        PointMass* mass = &dim->physics.masses.data[mass_slot];
        if (!mass->collideable) continue;

        for (u32 box_slot = 0; box_slot < dim->physics.boxes.count; ++box_slot) {
            collide_mass_box(dim, mass, &dim->physics.boxes.data[box_slot]);
        }

        const PhysicsCell c = physics_cell_for_pos(mass->pos, dim->chunk_size_m, dim->physics.hash.cell_size);
        for (i64 dz = -1; dz <= 1; ++dz) {
            for (i64 dy = -1; dy <= 1; ++dy) {
                for (i64 dx = -1; dx <= 1; ++dx) {
                    PhysicsCell nc{c.x + dx, c.y + dy, c.z + dz};
                    const u32 bucket_id = hash_find_bucket(&dim->physics.hash, nc);
                    if (!id_valid(bucket_id)) continue;
                    for (u32 ref = dim->physics.hash.buckets[bucket_id].first; id_valid(ref); ref = dim->physics.hash.refs[ref].next) {
                        const u32 other_id = dim->physics.hash.refs[ref].mass_id;
                        if (other_id <= mass_id) continue;
                        PointMass* other = arena_get(&dim->physics.masses, other_id);
                        collide_mass_mass(dim, mass, other);
                    }
                }
            }
        }
    }

    for (u32 link_slot = 0; link_slot < dim->physics.links.count; ++link_slot) {
        Link* link = &dim->physics.links.data[link_slot];
        for (u32 box_slot = 0; box_slot < dim->physics.boxes.count; ++box_slot) {
            collide_link_box(dim, link, &dim->physics.boxes.data[box_slot]);
        }
    }
}

void physics_step(Dimension* dim, float dt) {
    if (!dim || dt <= 0.0f) return;
    const u32 substeps = dim->physics.substeps ? dim->physics.substeps : 1;
    const float sdt = dt / static_cast<float>(substeps);

    for (u32 sub = 0; sub < substeps; ++sub) {
        integrate_masses(dim, sdt);
        for (u32 i = 0; i < dim->physics.solver_iterations; ++i) {
            solve_links(dim, sdt);
            rebuild_spatial_hash(dim);
            collide_all(dim);
        }
    }
}

} // namespace ol
