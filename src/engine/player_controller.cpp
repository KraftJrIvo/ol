#include "engine/player_controller.h"

#include <cmath>

namespace ol {

static float player_link_length(float height) {
    return fmaxf(0.05f, height - 2.0f * player_radius_m);
}

static bool ranges_overlap(float a_min, float a_max, float b_min, float b_max) {
    return a_max > b_min && a_min < b_max;
}

static bool cylinder_overlaps_box_xz(const BoxCollider* box, Vector3 feet_rel, float radius) {
    constexpr float eps = 0.001f;
    return std::fabs(feet_rel.x) < box->half.x + radius + eps &&
           std::fabs(feet_rel.z) < box->half.z + radius + eps;
}

static bool cylinder_penetrates_box_xz(const BoxCollider* box, Vector3 feet_rel, float radius) {
    const float closest_x = clampf(feet_rel.x, -box->half.x, box->half.x);
    const float closest_z = clampf(feet_rel.z, -box->half.z, box->half.z);
    const float dx = feet_rel.x - closest_x;
    const float dz = feet_rel.z - closest_z;
    const float active_radius = fmaxf(0.0f, radius - 0.001f);
    return dx * dx + dz * dz < active_radius * active_radius;
}

static bool cylinder_overlaps_box_y(const BoxCollider* box, Vector3 feet_rel, float height) {
    return ranges_overlap(feet_rel.y, feet_rel.y + height, -box->half.y, box->half.y);
}

static bool cylinder_overlaps_box(const BoxCollider* box, Vector3 feet_rel, float radius, float height) {
    return cylinder_overlaps_box_xz(box, feet_rel, radius) &&
           cylinder_overlaps_box_y(box, feet_rel, height);
}

static bool player_deeply_penetrates_axis_aligned_wall(const Dimension* dim, const PlayerEntity* player, WorldPos feet, bool allow_step) {
    for (u32 i = 0; i < dim->physics.boxes.count; ++i) {
        const BoxCollider* box = &dim->physics.boxes.data[i];
        if (!box->axis_aligned) continue;
        const Vector3 rel = world_delta_meters(feet, box->pos, dim->chunk_size_m);
        const float step_delta = box->half.y - rel.y;
        if (allow_step && step_delta >= -0.06f && step_delta <= player_step_up_m) continue;
        constexpr float side_eps = 0.02f;
        const bool overlaps_blocking_side_height = rel.y < box->half.y - side_eps &&
            rel.y + player->current_height > -box->half.y + side_eps;
        const float closest_x = clampf(rel.x, -box->half.x, box->half.x);
        const float closest_z = clampf(rel.z, -box->half.z, box->half.z);
        const float dx = rel.x - closest_x;
        const float dz = rel.z - closest_z;
        const float deep_radius = fmaxf(0.0f, player->body_radius - 0.05f);
        if (overlaps_blocking_side_height && dx * dx + dz * dz < deep_radius * deep_radius) {
            return true;
        }
    }
    return false;
}

static bool box_near_feet_xz(const Dimension* dim, const BoxCollider* box, WorldPos feet, float radius, float margin = 0.75f) {
    const Vector3 rel = world_delta_meters(feet, box->pos, dim->chunk_size_m);
    return std::fabs(rel.x) <= box->half.x + radius + margin &&
           std::fabs(rel.z) <= box->half.z + radius + margin;
}

static bool cylinder_blocks_box_side_y(const BoxCollider* box, Vector3 feet_rel, float height) {
    constexpr float eps = 0.02f;
    return feet_rel.y < box->half.y - eps &&
           feet_rel.y + height > -box->half.y + eps;
}

static float jump_speed_for_height(float gravity_y, float height_m, float dt) {
    const float g = std::fabs(gravity_y);
    if (g <= 0.001f) return 0.0f;
    return std::sqrt(2.0f * g * height_m) + 0.5f * g * dt;
}

static double world_y_meters(WorldPos pos, float chunk_size) {
    return world_axis_meters(pos.chunk.y, pos.local.y, chunk_size);
}

static double world_x_meters(WorldPos pos, float chunk_size) {
    return world_axis_meters(pos.chunk.x, pos.local.x, chunk_size);
}

static double world_z_meters(WorldPos pos, float chunk_size) {
    return world_axis_meters(pos.chunk.z, pos.local.z, chunk_size);
}

static float approach_float(float current, float target, float max_delta) {
    const float delta = target - current;
    if (std::fabs(delta) <= max_delta) return target;
    return current + (delta < 0.0f ? -max_delta : max_delta);
}

static float grounded_eye_for_height(float height) {
    const float t = clampf((height - player_crouch_height_m) / (player_stand_height_m - player_crouch_height_m), 0.0f, 1.0f);
    return player_crouch_eye_m + (player_stand_eye_m - player_crouch_eye_m) * t;
}

static float air_eye_for_height(float height) {
    return clampf(player_stand_eye_m - (player_stand_height_m - height), 0.05f, height + player_stand_eye_m);
}

static constexpr float player_camera_step_smooth_mps = 2.0f;
static constexpr float player_camera_step_threshold_m = 0.035f;

static Vector3 box_top_normal_world(const BoxCollider* box) {
    if (box->axis_aligned) return {0.0f, 1.0f, 0.0f};
    return safe_norm(Vector3RotateByQuaternion({0.0f, 1.0f, 0.0f}, box->rotation), {0.0f, 1.0f, 0.0f});
}

static bool cylinder_overlaps_box_at_height(const Dimension* dim, const BoxCollider* box, WorldPos feet, float radius, float height) {
    if (!box->axis_aligned) return false;
    const Vector3 rel = world_delta_meters(feet, box->pos, dim->chunk_size_m);
    return cylinder_overlaps_box(box, rel, radius, height);
}

static bool surface_has_immediate_clearance(const Dimension* dim, const PlayerEntity* player, WorldPos surface_feet, double surface_y, u32 ignore_box_slot) {
    WorldPos clearance_point = surface_feet;
    const BoxCollider* support = ignore_box_slot < dim->physics.boxes.count ? &dim->physics.boxes.data[ignore_box_slot] : nullptr;
    if (support && support->axis_aligned) {
        const Vector3 support_rel = world_delta_meters(surface_feet, support->pos, dim->chunk_size_m);
        worldpos_add_delta(
            &clearance_point,
            {
                clampf(support_rel.x, -support->half.x, support->half.x) - support_rel.x,
                0.0f,
                clampf(support_rel.z, -support->half.z, support->half.z) - support_rel.z
            },
            dim->chunk_size_m);
    }
    for (u32 slot = 0; slot < dim->physics.boxes.count; ++slot) {
        if (slot == ignore_box_slot) continue;
        const BoxCollider* other = &dim->physics.boxes.data[slot];
        if (!other->axis_aligned) continue;
        if (!box_near_feet_xz(dim, other, clearance_point, 0.02f, 0.12f)) continue;
        const Vector3 rel = world_delta_meters(clearance_point, other->pos, dim->chunk_size_m);
        constexpr float footprint_eps = 0.02f;
        if (std::fabs(rel.x) > other->half.x + footprint_eps) continue;
        if (std::fabs(rel.z) > other->half.z + footprint_eps) continue;
        const double other_bottom = world_y_meters(other->pos, dim->chunk_size_m) - static_cast<double>(other->half.y);
        const double other_top = world_y_meters(other->pos, dim->chunk_size_m) + static_cast<double>(other->half.y);
        if (other_bottom >= surface_y - 0.03 && other_bottom <= surface_y + 0.05 && other_top > surface_y + 0.01) return false;
    }
    return true;
}

static bool box_top_surface_y(const Dimension* dim, const PlayerEntity* player, u32 box_slot, WorldPos feet, float radius, double* out_y) {
    const BoxCollider* box = &dim->physics.boxes.data[box_slot];
    if (!box_near_feet_xz(dim, box, feet, radius, 0.12f)) return false;
    const Vector3 normal = box_top_normal_world(box);
    if (normal.y < 0.35f) return false;

    const Vector3 rel = world_delta_meters(feet, box->pos, dim->chunk_size_m);
    const Vector3 plane_point = box->axis_aligned ? Vector3{0.0f, box->half.y, 0.0f} :
        Vector3RotateByQuaternion({0.0f, box->half.y, 0.0f}, box->rotation);
    const float plane_d = Vector3DotProduct(normal, plane_point);
    const float rel_y = (plane_d - normal.x * rel.x - normal.z * rel.z) / normal.y;
    Vector3 on_plane = {rel.x, rel_y, rel.z};
    Vector3 local = box->axis_aligned ? on_plane : Vector3RotateByQuaternion(on_plane, QuaternionInvert(box->rotation));
    if (std::fabs(local.x) > box->half.x + radius) return false;
    if (std::fabs(local.z) > box->half.z + radius) return false;

    const double surface_y = world_y_meters(box->pos, dim->chunk_size_m) + static_cast<double>(rel_y);
    WorldPos surface_feet = make_world_pos(
        feet.dimension,
        {
            static_cast<float>(world_x_meters(feet, dim->chunk_size_m)),
            static_cast<float>(surface_y),
            static_cast<float>(world_z_meters(feet, dim->chunk_size_m))
        },
        dim->chunk_size_m
    );
    if (!surface_has_immediate_clearance(dim, player, surface_feet, surface_y, box_slot)) return false;

    *out_y = surface_y;
    return true;
}

static bool find_walkable_surface(const Dimension* dim, const PlayerEntity* player, WorldPos feet, float snap_up, float snap_down, double* out_y, Vector3* out_normal) {
    const double feet_y = world_y_meters(feet, dim->chunk_size_m);
    bool found = false;
    bool found_center_slope = false;
    double best_y = -1.0e30;
    double best_center_slope_y = -1.0e30;
    Vector3 best_normal = {0.0f, 1.0f, 0.0f};
    Vector3 best_center_slope_normal = {0.0f, 1.0f, 0.0f};
    const float support_radius = player ? player->body_radius : 0.02f;
    for (u32 i = 0; i < dim->physics.boxes.count; ++i) {
        double center_surface_y = 0.0;
        const bool center_supported = box_top_surface_y(dim, player, i, feet, 0.02f, &center_surface_y);

        double surface_y = 0.0;
        if (center_supported) {
            surface_y = center_surface_y;
        } else if (!box_top_surface_y(dim, player, i, feet, support_radius, &surface_y)) {
            continue;
        }
        const double delta = surface_y - feet_y;
        if (delta < -static_cast<double>(snap_down) || delta > static_cast<double>(snap_up)) continue;
        const Vector3 normal = box_top_normal_world(&dim->physics.boxes.data[i]);
        if (center_supported && normal.y < 0.999f && (!found_center_slope || surface_y > best_center_slope_y)) {
            best_center_slope_y = surface_y;
            best_center_slope_normal = normal;
            found_center_slope = true;
        }
        if (!found || surface_y > best_y) {
            best_y = surface_y;
            best_normal = normal;
            found = true;
        }
    }
    if (found_center_slope) {
        *out_y = best_center_slope_y;
        if (out_normal) *out_normal = best_center_slope_normal;
        return true;
    }
    if (!found) return false;
    *out_y = best_y;
    if (out_normal) *out_normal = best_normal;
    return true;
}

static WorldPos player_feet_from_bottom(const Dimension* dim, const PlayerEntity* player) {
    return player_feet_pos(dim, player);
}

static void zero_player_body_velocity(Dimension* dim, PlayerEntity* player) {
    PointMass* bottom = arena_get(&dim->physics.masses, player->bottom_mass);
    PointMass* top = arena_get(&dim->physics.masses, player->top_mass);
    if (bottom) bottom->prev = bottom->pos;
    if (top) top->prev = top->pos;
}

void player_sync_masses_to_pose(Dimension* dim, PlayerEntity* player, WorldPos feet_pos) {
    PointMass* bottom = arena_get(&dim->physics.masses, player->bottom_mass);
    PointMass* top = arena_get(&dim->physics.masses, player->top_mass);
    Link* link = arena_get(&dim->physics.links, player->body_link);
    if (!bottom || !top || !link) return;

    const float height = player->current_height;
    bottom->radius = player->body_radius;
    top->radius = player->body_radius;
    link->radius = player->body_radius;
    link->rest_length = player_link_length(height);

    bottom->pos = worldpos_offset(feet_pos, {0.0f, player->body_radius, 0.0f}, dim->chunk_size_m);
    top->pos = worldpos_offset(feet_pos, {0.0f, height - player->body_radius, 0.0f}, dim->chunk_size_m);
    bottom->prev = bottom->pos;
    top->prev = top->pos;
}

bool player_can_use_height(const Dimension* dim, const PlayerEntity* player, WorldPos feet_pos, float height) {
    for (u32 i = 0; i < dim->physics.boxes.count; ++i) {
        const BoxCollider* box = &dim->physics.boxes.data[i];
        if (!box->axis_aligned) continue;
        if (!box_near_feet_xz(dim, box, feet_pos, player->body_radius, 0.08f)) continue;
        const Vector3 rel = world_delta_meters(feet_pos, box->pos, dim->chunk_size_m);
        if (rel.y >= box->half.y - 0.01f) continue;
        if (rel.y + height <= -box->half.y + 0.01f) continue;
        if (cylinder_penetrates_box_xz(box, rel, player->body_radius)) {
            return false;
        }
    }
    return true;
}

static void resolve_vertical(Dimension* dim, PlayerEntity* player, WorldPos previous_feet, WorldPos* feet, float* vertical_velocity) {
    player->on_ground = false;
    for (u32 i = 0; i < dim->physics.boxes.count; ++i) {
        const BoxCollider* box = &dim->physics.boxes.data[i];
        if (!box_near_feet_xz(dim, box, *feet, player->body_radius, 0.85f)) continue;

        if (*vertical_velocity <= 0.0f) {
            double surface_y = 0.0;
            if (box_top_surface_y(dim, player, i, *feet, player->body_radius, &surface_y)) {
                const double prev_y = world_y_meters(previous_feet, dim->chunk_size_m);
                const double feet_y = world_y_meters(*feet, dim->chunk_size_m);
                const bool crossed_from_above = prev_y >= surface_y - 0.02 && feet_y <= surface_y + 0.05;
                const bool shallow_top_penetration = feet_y > surface_y - 0.20 && feet_y <= surface_y + 0.05;
                if (crossed_from_above || shallow_top_penetration) {
                    worldpos_add_delta(feet, {0.0f, static_cast<float>(surface_y - feet_y), 0.0f}, dim->chunk_size_m);
                    *vertical_velocity = 0.0f;
                    player->on_ground = true;
                    continue;
                }
            }
        }

        if (!box->axis_aligned) continue;
        Vector3 rel = world_delta_meters(*feet, box->pos, dim->chunk_size_m);
        Vector3 prev_rel = world_delta_meters(previous_feet, box->pos, dim->chunk_size_m);
        if (!cylinder_overlaps_box_xz(box, rel, player->body_radius)) continue;
        if (!cylinder_overlaps_box_y(box, rel, player->current_height)) continue;

        if (*vertical_velocity <= 0.0f) {
            const bool crossed_from_above = prev_rel.y >= box->half.y - 0.02f;
            const bool shallow_top_penetration = rel.y > box->half.y - 0.20f;
            if (!crossed_from_above && !shallow_top_penetration) continue;
            double surface_y = 0.0;
            if (!box_top_surface_y(dim, player, i, *feet, 0.02f, &surface_y)) continue;
            const float correction = static_cast<float>(surface_y - world_y_meters(*feet, dim->chunk_size_m));
            if (correction >= 0.0f) {
                worldpos_add_delta(feet, {0.0f, correction, 0.0f}, dim->chunk_size_m);
                *vertical_velocity = 0.0f;
                player->on_ground = true;
            }
        } else {
            const float prev_top = prev_rel.y + player->current_height;
            if (prev_top > -box->half.y + 0.02f) continue;
            if (!cylinder_penetrates_box_xz(box, rel, player->body_radius)) continue;
            const float correction = -box->half.y - (rel.y + player->current_height);
            if (correction <= 0.0f) {
                worldpos_add_delta(feet, {0.0f, correction, 0.0f}, dim->chunk_size_m);
                *vertical_velocity = 0.0f;
            }
        }
    }
}

static bool has_ground_below(const Dimension* dim, const PlayerEntity* player, WorldPos feet) {
    double surface_y = 0.0;
    return find_walkable_surface(dim, player, feet, 0.04f, 0.08f, &surface_y, nullptr);
}

static bool resolve_walkable_landing_crossing(Dimension* dim, PlayerEntity* player, WorldPos previous_feet, WorldPos* feet, float* vertical_velocity) {
    if (*vertical_velocity > 0.0f) return false;

    const double prev_y = world_y_meters(previous_feet, dim->chunk_size_m);
    const double feet_y = world_y_meters(*feet, dim->chunk_size_m);
    bool landed = false;
    double best_y = -1.0e30;
    for (u32 i = 0; i < dim->physics.boxes.count; ++i) {
        double surface_y = 0.0;
        if (!box_top_surface_y(dim, player, i, *feet, player->body_radius, &surface_y)) continue;
        const bool crossed_from_above = prev_y >= surface_y - 0.02 && feet_y <= surface_y + 0.05;
        const bool shallow_top_penetration = feet_y > surface_y - 0.20 && feet_y <= surface_y + 0.05;
        if ((!crossed_from_above && !shallow_top_penetration) || surface_y <= best_y) continue;
        best_y = surface_y;
        landed = true;
    }

    if (!landed) return false;
    worldpos_add_delta(feet, {0.0f, static_cast<float>(best_y - feet_y), 0.0f}, dim->chunk_size_m);
    *vertical_velocity = 0.0f;
    player->on_ground = true;
    return true;
}

static double box_axis_center(const BoxCollider* box, float chunk_size, bool x_axis) {
    return x_axis ? world_x_meters(box->pos, chunk_size) : world_z_meters(box->pos, chunk_size);
}

static double box_axis_min(const BoxCollider* box, float chunk_size, bool x_axis) {
    return box_axis_center(box, chunk_size, x_axis) - static_cast<double>(x_axis ? box->half.x : box->half.z);
}

static double box_axis_max(const BoxCollider* box, float chunk_size, bool x_axis) {
    return box_axis_center(box, chunk_size, x_axis) + static_cast<double>(x_axis ? box->half.x : box->half.z);
}

static bool box_overlaps_player_perp(const Dimension* dim, const BoxCollider* box, WorldPos feet, float radius, bool x_axis) {
    const Vector3 rel = world_delta_meters(feet, box->pos, dim->chunk_size_m);
    const float perp = x_axis ? rel.z : rel.x;
    const float half = x_axis ? box->half.z : box->half.x;
    return std::fabs(perp) < half + radius + 0.02f;
}

static bool box_overlaps_player_y(const Dimension* dim, const BoxCollider* box, WorldPos feet, float height) {
    const Vector3 rel = world_delta_meters(feet, box->pos, dim->chunk_size_m);
    return cylinder_blocks_box_side_y(box, rel, height);
}

static bool face_has_flush_neighbor(const Dimension* dim, u32 box_slot, WorldPos feet, float radius, float height, bool x_axis, bool negative_face) {
    const BoxCollider* box = &dim->physics.boxes.data[box_slot];
    const double face = negative_face ? box_axis_min(box, dim->chunk_size_m, x_axis) : box_axis_max(box, dim->chunk_size_m, x_axis);
    constexpr double seam_eps = 0.035;

    for (u32 slot = 0; slot < dim->physics.boxes.count; ++slot) {
        if (slot == box_slot) continue;
        const BoxCollider* other = &dim->physics.boxes.data[slot];
        if (!other->axis_aligned) continue;
        if (!box_near_feet_xz(dim, other, feet, radius, 0.10f)) continue;

        const double other_face = negative_face ? box_axis_max(other, dim->chunk_size_m, x_axis) : box_axis_min(other, dim->chunk_size_m, x_axis);
        if (std::fabs(other_face - face) > seam_eps) continue;
        if (!box_overlaps_player_perp(dim, other, feet, radius, x_axis)) continue;
        if (!box_overlaps_player_y(dim, other, feet, height)) continue;
        return true;
    }
    return false;
}

static bool side_can_step_onto_box(const Dimension* dim, const PlayerEntity* player, u32 box_slot, WorldPos feet, bool allow_step) {
    if (!allow_step) return false;
    double surface_y = 0.0;
    if (!box_top_surface_y(dim, player, box_slot, feet, player->body_radius, &surface_y)) return false;
    const double delta = surface_y - world_y_meters(feet, dim->chunk_size_m);
    return delta >= -0.06 && delta <= static_cast<double>(player_step_up_m);
}

static bool resolve_rotated_box_x_side(Dimension* dim, PlayerEntity* player, const BoxCollider* box, WorldPos previous_feet, WorldPos* feet, float* velocity_axis, bool allow_step) {
    if (box->axis_aligned) return false;
    if (!box_near_feet_xz(dim, box, *feet, player->body_radius, 0.85f)) return false;

    const Quaternion inv = QuaternionInvert(box->rotation);
    const Vector3 rel = world_delta_meters(*feet, box->pos, dim->chunk_size_m);
    const Vector3 prev_rel = world_delta_meters(previous_feet, box->pos, dim->chunk_size_m);
    const Vector3 local = Vector3RotateByQuaternion(rel, inv);
    const Vector3 prev_local = Vector3RotateByQuaternion(prev_rel, inv);

    if (std::fabs(local.z) > box->half.z + player->body_radius) return false;

    double surface_y = 0.0;
    if (!box_top_surface_y(dim, player, static_cast<u32>(box - dim->physics.boxes.data.data()), *feet, player->body_radius, &surface_y)) return false;
    const double feet_y = world_y_meters(*feet, dim->chunk_size_m);
    const double step_delta = surface_y - feet_y;
    if (allow_step && step_delta >= -0.06 && step_delta <= static_cast<double>(player_step_up_m)) return false;
    if (feet_y > surface_y + 0.35 || feet_y + player->current_height < surface_y - 0.25) return false;

    const float expanded = box->half.x + player->body_radius;
    float correction = 0.0f;
    if (prev_local.x <= -expanded + 0.01f && local.x > -expanded) {
        correction = -expanded - local.x;
    } else if (prev_local.x >= expanded - 0.01f && local.x < expanded) {
        correction = expanded - local.x;
    } else {
        return false;
    }

    const Vector3 world_delta = Vector3RotateByQuaternion({correction, 0.0f, 0.0f}, box->rotation);
    worldpos_add_delta(feet, world_delta, dim->chunk_size_m);
    *velocity_axis = 0.0f;
    return true;
}

static bool resolve_rotated_box_side_penetration(Dimension* dim, PlayerEntity* player, const BoxCollider* box, WorldPos* feet, bool allow_step) {
    if (box->axis_aligned) return false;

    const u32 box_slot = static_cast<u32>(box - dim->physics.boxes.data.data());
    if (!box_near_feet_xz(dim, box, *feet, player->body_radius, 0.85f)) return false;
    double surface_y = 0.0;
    if (!box_top_surface_y(dim, player, box_slot, *feet, player->body_radius, &surface_y)) return false;

    const double feet_y = world_y_meters(*feet, dim->chunk_size_m);
    const double step_delta = surface_y - feet_y;
    if (allow_step && step_delta >= -0.06 && step_delta <= static_cast<double>(player_step_up_m)) return false;
    if (step_delta <= static_cast<double>(player_step_up_m)) return false;
    if (feet_y + player->current_height < surface_y - 0.25) return false;

    const Quaternion inv = QuaternionInvert(box->rotation);
    const Vector3 rel = world_delta_meters(*feet, box->pos, dim->chunk_size_m);
    const Vector3 local = Vector3RotateByQuaternion(rel, inv);
    if (std::fabs(local.z) > box->half.z + player->body_radius) return false;

    const float expanded = box->half.x + player->body_radius;
    if (std::fabs(local.x) > expanded) return false;

    const float correction = local.x < 0.0f ? -expanded - local.x : expanded - local.x;
    const Vector3 world_delta = Vector3RotateByQuaternion({correction, 0.0f, 0.0f}, box->rotation);
    worldpos_add_delta(feet, world_delta, dim->chunk_size_m);
    return true;
}

static void resolve_rotated_box_side_penetrations(Dimension* dim, PlayerEntity* player, WorldPos* feet, bool allow_step) {
    for (u32 i = 0; i < dim->physics.boxes.count; ++i) {
        resolve_rotated_box_side_penetration(dim, player, &dim->physics.boxes.data[i], feet, allow_step);
    }
}

static void resolve_axis(Dimension* dim, PlayerEntity* player, WorldPos previous_feet, WorldPos* feet, float* velocity_axis, bool x_axis, bool allow_step) {
    for (u32 i = 0; i < dim->physics.boxes.count; ++i) {
        const BoxCollider* box = &dim->physics.boxes.data[i];
        if (!box_near_feet_xz(dim, box, *feet, player->body_radius, 0.85f)) continue;
        if (!box->axis_aligned) {
            if (x_axis) resolve_rotated_box_x_side(dim, player, box, previous_feet, feet, velocity_axis, allow_step);
            continue;
        }
        Vector3 rel = world_delta_meters(*feet, box->pos, dim->chunk_size_m);
        Vector3 prev_rel = world_delta_meters(previous_feet, box->pos, dim->chunk_size_m);
        if (!cylinder_overlaps_box_y(box, rel, player->current_height)) continue;
        if (!cylinder_overlaps_box_xz(box, rel, player->body_radius)) continue;

        const float perp_rel = x_axis ? rel.z : rel.x;
        const float perp_half = x_axis ? box->half.z : box->half.x;
        if (std::fabs(perp_rel) > perp_half + 0.01f) continue;

        const float expanded = (x_axis ? box->half.x : box->half.z) + player->body_radius;
        const float axis_rel = x_axis ? rel.x : rel.z;
        const float prev_axis_rel = x_axis ? prev_rel.x : prev_rel.z;
        constexpr float crossing_eps = 0.01f;
        float correction = 0.0f;
        if (prev_axis_rel <= -expanded + crossing_eps && axis_rel > -expanded) {
            if (face_has_flush_neighbor(dim, i, *feet, player->body_radius, player->current_height, x_axis, true)) continue;
            if (side_can_step_onto_box(dim, player, i, *feet, allow_step)) continue;
            correction = -expanded - axis_rel;
        } else if (prev_axis_rel >= expanded - crossing_eps && axis_rel < expanded) {
            if (face_has_flush_neighbor(dim, i, *feet, player->body_radius, player->current_height, x_axis, false)) continue;
            if (side_can_step_onto_box(dim, player, i, *feet, allow_step)) continue;
            correction = expanded - axis_rel;
        } else {
            continue;
        }
        Vector3 delta = Vector3Zero();
        if (x_axis) delta.x = correction;
        else delta.z = correction;
        worldpos_add_delta(feet, delta, dim->chunk_size_m);
        *velocity_axis = 0.0f;
    }
}

static void resolve_box_corners(Dimension* dim, PlayerEntity* player, WorldPos* feet, bool allow_step) {
    for (u32 i = 0; i < dim->physics.boxes.count; ++i) {
        const BoxCollider* box = &dim->physics.boxes.data[i];
        if (!box->axis_aligned) continue;
        if (!box_near_feet_xz(dim, box, *feet, player->body_radius, 0.08f)) continue;

        Vector3 rel = world_delta_meters(*feet, box->pos, dim->chunk_size_m);
        if (!cylinder_overlaps_box_y(box, rel, player->current_height)) continue;
        if (std::fabs(rel.x) <= box->half.x || std::fabs(rel.z) <= box->half.z) continue;
        if (side_can_step_onto_box(dim, player, i, *feet, allow_step)) continue;

        const float closest_x = clampf(rel.x, -box->half.x, box->half.x);
        const float closest_z = clampf(rel.z, -box->half.z, box->half.z);
        const float dx = rel.x - closest_x;
        const float dz = rel.z - closest_z;
        const float dist_sq = dx * dx + dz * dz;
        if (dist_sq >= player->body_radius * player->body_radius || dist_sq <= 0.000001f) continue;

        const float dist = std::sqrt(dist_sq);
        const Vector3 normal = {dx / dist, 0.0f, dz / dist};
        const float active_radius = fmaxf(0.0f, player->body_radius - 0.001f);
        const float correction = active_radius - dist;
        if (correction > 0.0f) {
            worldpos_add_delta(feet, normal * correction, dim->chunk_size_m);
        }

        const float inward_speed = player->velocity.x * normal.x + player->velocity.z * normal.z;
        if (inward_speed < 0.0f) {
            player->velocity.x -= normal.x * inward_speed;
            player->velocity.z -= normal.z * inward_speed;
        }
    }
}

static void snap_to_walkable_ground(Dimension* dim, PlayerEntity* player, WorldPos* feet, float* vertical_velocity, float snap_up, float snap_down) {
    double surface_y = 0.0;
    if (!find_walkable_surface(dim, player, *feet, snap_up, snap_down, &surface_y, nullptr)) return;
    const double feet_y = world_y_meters(*feet, dim->chunk_size_m);
    const float step_up = static_cast<float>(surface_y - feet_y);
    worldpos_add_delta(feet, {0.0f, static_cast<float>(surface_y - feet_y), 0.0f}, dim->chunk_size_m);
    if (*vertical_velocity < 0.0f) *vertical_velocity = 0.0f;
    if (step_up > player_camera_step_threshold_m) {
        player->camera_y_offset = clampf(player->camera_y_offset - step_up, -0.50f, 0.50f);
    }
    player->on_ground = true;
}

static Vector3 project_ground_wish(const Dimension* dim, const PlayerEntity* player, WorldPos feet, Vector3 wish) {
    const float speed = safe_len(wish);
    if (speed <= 0.001f) return wish;

    double surface_y = 0.0;
    Vector3 normal = {0.0f, 1.0f, 0.0f};
    if (!find_walkable_surface(dim, player, feet, 0.04f, 0.10f, &surface_y, &normal)) return wish;
    if (normal.y > 0.999f) return wish;

    const Vector3 tangent = wish - normal * Vector3DotProduct(wish, normal);
    const float tangent_len = safe_len(tangent);
    if (tangent_len <= 0.001f) return wish;

    const Vector3 tangent_dir = tangent / tangent_len;
    return {tangent_dir.x * speed, 0.0f, tangent_dir.z * speed};
}

static void update_camera_y_offset(PlayerEntity* player, float dt) {
    player->camera_y_offset = approach_float(player->camera_y_offset, 0.0f, player_camera_step_smooth_mps * dt);
}

static Vector3 approach_horizontal_velocity(Vector3 current, Vector3 target, float max_delta) {
    Vector3 delta = target - current;
    delta.y = 0.0f;
    const float len = safe_len(delta);
    if (len <= max_delta || len <= 0.0001f) return target;
    return current + delta * (max_delta / len);
}

void player_controller_step(Dimension* dim, PlayerEntity* player, const PlayerControllerInput& input, float dt) {
    if (!dim || !player || dt <= 0.0f) return;

    WorldPos feet = player_feet_from_bottom(dim, player);
    const bool grounded_before = player->on_ground || (player->velocity.y <= 0.0f && has_ground_below(dim, player, feet));

    const float old_height = player->current_height;
    float target_height = input.crouch ? player_crouch_height_m : player_stand_height_m;
    WorldPos resized_feet = feet;
    if (!grounded_before && std::fabs(target_height - old_height) > 0.0001f) {
        worldpos_add_delta(&resized_feet, {0.0f, old_height - target_height, 0.0f}, dim->chunk_size_m);
    }
    if (target_height > old_height && !player_can_use_height(dim, player, resized_feet, target_height)) {
        target_height = old_height;
        resized_feet = feet;
    }
    if (!grounded_before && std::fabs(target_height - old_height) > 0.0001f) {
        const float feet_delta = old_height - target_height;
        feet = resized_feet;
        player->eye_height = clampf(player->eye_height - feet_delta, 0.05f, target_height + player_stand_eye_m);
    }
    player->current_height = target_height;
    player->is_crouching = player->current_height < player_stand_height_m - 0.01f;
    const float target_eye = grounded_before ? grounded_eye_for_height(player->current_height) : air_eye_for_height(player->current_height);
    player->eye_height = approach_float(player->eye_height, target_eye, player_eye_transition_mps * dt);

    float speed = player_walk_speed_mps;
    if (grounded_before) {
        if (player->is_crouching) speed = player_crouch_speed_mps;
        else if (input.sprint) speed = player_sprint_speed_mps;
    }

    Vector3 wish = flat_right_from_yaw(player->yaw) * input.move.x + flat_forward_from_yaw(player->yaw) * input.move.y;
    if (safe_len(wish) > 0.001f) wish = safe_norm(wish) * speed;
    if (grounded_before) wish = project_ground_wish(dim, player, feet, wish);

    Vector3 target_h = {wish.x, 0.0f, wish.z};
    Vector3 current_h = {player->velocity.x, 0.0f, player->velocity.z};
    if (grounded_before) {
        const float accel = safe_len(target_h) > 0.001f ? player_ground_accel_mps2 : player_ground_decel_mps2;
        current_h = approach_horizontal_velocity(current_h, target_h, accel * dt);
        player->velocity.x = current_h.x;
        player->velocity.z = current_h.z;
    } else {
        const float current_speed = safe_len(current_h);
        const float air_speed = fmaxf(current_speed, player_air_control_speed_mps);
        target_h = safe_len(target_h) > 0.001f ? safe_norm(target_h) * air_speed : Vector3Zero();
        const float air_accel = current_speed < player_air_control_speed_mps - 0.01f && safe_len(target_h) > current_speed
            ? player_air_gain_accel_mps2
            : player_air_accel_mps2;
        current_h = approach_horizontal_velocity(current_h, target_h, air_accel * dt);
        if (safe_len(current_h) > air_speed) current_h = safe_norm(current_h) * air_speed;
        player->velocity.x = current_h.x;
        player->velocity.z = current_h.z;
    }

    if (input.jump_pressed && grounded_before) {
        player->velocity.y = jump_speed_for_height(dim->physics.gravity.y, player_min_jump_height_m, dt);
        player->jump_start_feet = feet;
        player->jump_hold_time = 0.0f;
        player->jump_variable_active = true;
        player->on_ground = false;
    }
    if (player->jump_variable_active) {
        if (input.jump_held && player->velocity.y > 0.0f) {
            player->jump_hold_time = fminf(player_jump_hold_time_s, player->jump_hold_time + dt);
            const float hold_t = player_jump_hold_time_s > 0.0f ? player->jump_hold_time / player_jump_hold_time_s : 1.0f;
            const float target_height = player_min_jump_height_m + (player_max_jump_height_m - player_min_jump_height_m) * clampf(hold_t, 0.0f, 1.0f);
            const float risen = fmaxf(0.0f, world_delta_meters(feet, player->jump_start_feet, dim->chunk_size_m).y);
            const float remaining = target_height - risen;
            if (remaining > 0.0f) {
                const float target_speed = jump_speed_for_height(dim->physics.gravity.y, remaining, dt);
                if (player->velocity.y < target_speed) player->velocity.y = target_speed;
            }
            if (player->jump_hold_time >= player_jump_hold_time_s) player->jump_variable_active = false;
        } else {
            player->jump_variable_active = false;
        }
    }
    player->velocity.y += dim->physics.gravity.y * dt;
    if (player->velocity.y < -30.0f) player->velocity.y = -30.0f;

    const WorldPos frame_start_feet = feet;
    WorldPos previous_feet = feet;
    worldpos_add_delta(&feet, {0.0f, player->velocity.y * dt, 0.0f}, dim->chunk_size_m);
    resolve_vertical(dim, player, previous_feet, &feet, &player->velocity.y);

    previous_feet = feet;
    worldpos_add_delta(&feet, {player->velocity.x * dt, 0.0f, 0.0f}, dim->chunk_size_m);
    resolve_axis(dim, player, previous_feet, &feet, &player->velocity.x, true, grounded_before);
    resolve_box_corners(dim, player, &feet, grounded_before);
    if (player_deeply_penetrates_axis_aligned_wall(dim, player, feet, grounded_before)) {
        feet = previous_feet;
        player->velocity.x = 0.0f;
    }

    previous_feet = feet;
    worldpos_add_delta(&feet, {0.0f, 0.0f, player->velocity.z * dt}, dim->chunk_size_m);
    resolve_axis(dim, player, previous_feet, &feet, &player->velocity.z, false, grounded_before);
    resolve_box_corners(dim, player, &feet, grounded_before);
    if (player_deeply_penetrates_axis_aligned_wall(dim, player, feet, grounded_before)) {
        feet = previous_feet;
        player->velocity.z = 0.0f;
    }
    resolve_rotated_box_side_penetrations(dim, player, &feet, grounded_before);
    if (!player->on_ground) {
        resolve_walkable_landing_crossing(dim, player, frame_start_feet, &feet, &player->velocity.y);
    }
    if (player->velocity.y <= 0.0f && grounded_before) {
        snap_to_walkable_ground(dim, player, &feet, &player->velocity.y, 0.35f, 0.18f);
    }

    player->is_crouching = player->current_height < player_stand_height_m - 0.01f;
    if (player->on_ground) {
        player->eye_height = approach_float(player->eye_height, grounded_eye_for_height(player->current_height), player_eye_transition_mps * dt);
    }
    if (player->on_ground) {
        player->jump_variable_active = false;
        player->jump_hold_time = 0.0f;
    }
    player->grounded_frames = player->on_ground ? player->grounded_frames + 1 : 0;
    update_camera_y_offset(player, dt);

    player_sync_masses_to_pose(dim, player, feet);
    zero_player_body_velocity(dim, player);
}

} // namespace ol
