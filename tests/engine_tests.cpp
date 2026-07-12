#include "engine/render.h"
#include "engine/player_controller.h"
#include "engine/world.h"
#include "demo/demo.h"
#include "demo/menu.h"

#include "raylib.h"
#include "rlgl.h"

#if defined(_WIN32)
    #if !defined(APIENTRY)
        #define APIENTRY __stdcall
    #endif
    #if !defined(WINGDIAPI)
        #define WINGDIAPI __declspec(dllimport)
    #endif
#endif
#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

bool expect_true(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }
    return true;
}

bool near(float a, float b, float eps) {
    return std::fabs(a - b) <= eps;
}

ol::WorldPos test_pos(ol::u32 dimension, Vector3 meters, float chunk_size) {
    return ol::make_world_pos(dimension, meters, chunk_size);
}

bool translate_test_player(ol::Dimension* dim, ol::u32 player_id, Vector3 delta) {
    ol::PlayerEntity* player = dim ? ol::arena_get(&dim->players, player_id) : nullptr;
    if (!player) return false;
    ol::PointMass* bottom = ol::arena_get(&dim->physics.masses, player->bottom_mass);
    ol::PointMass* top = ol::arena_get(&dim->physics.masses, player->top_mass);
    if (!bottom || !top) return false;
    ol::worldpos_add_delta(&bottom->pos, delta, dim->chunk_size_m);
    ol::worldpos_add_delta(&bottom->prev, delta, dim->chunk_size_m);
    ol::worldpos_add_delta(&top->pos, delta, dim->chunk_size_m);
    ol::worldpos_add_delta(&top->prev, delta, dim->chunk_size_m);
    return true;
}

bool move_test_player_to(ol::Dimension* dim, ol::u32 player_id, ol::WorldPos target_feet) {
    ol::PlayerEntity* player = dim ? ol::arena_get(&dim->players, player_id) : nullptr;
    if (!player) return false;
    const Vector3 delta = ol::world_delta_meters(target_feet, ol::player_feet_pos(dim, player), dim->chunk_size_m);
    return translate_test_player(dim, player_id, delta);
}

bool test_worldpos() {
    ol::WorldPos p = ol::make_world_pos(0, {12345.0f, -37.0f, 8192.5f}, 16.0f);
    if (!expect_true(p.local.x >= 0.0f && p.local.x < 16.0f, "WorldPos canonicalizes local x")) return false;
    if (!expect_true(p.local.y >= 0.0f && p.local.y < 16.0f, "WorldPos canonicalizes local y")) return false;
    if (!expect_true(p.local.z >= 0.0f && p.local.z < 16.0f, "WorldPos canonicalizes local z")) return false;

    ol::WorldPos q = ol::worldpos_offset(p, {3.0f, 4.0f, -5.0f}, 16.0f);
    Vector3 delta = ol::world_delta_meters(q, p, 16.0f);
    return expect_true(near(delta.x, 3.0f, 0.001f) && near(delta.y, 4.0f, 0.001f) && near(delta.z, -5.0f, 0.001f),
        "WorldPos delta remains accurate across chunks");
}

bool test_axis_lock_uncrouch_anchor() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "physics", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    dim->physics.gravity = {0.0f, 0.0f, 0.0f};

    ol::PointMass bottom{};
    bottom.pos = test_pos(dim_id, {0.0f, 0.5f, 0.0f}, dim->chunk_size_m);
    bottom.prev = bottom.pos;
    bottom.radius = 0.35f;

    ol::PointMass top = bottom;
    top.pos = test_pos(dim_id, {0.0f, 1.1f, 0.0f}, dim->chunk_size_m);
    top.prev = top.pos;

    const ol::u32 bottom_id = ol::physics_add_point_mass(&dim->physics, bottom);
    const ol::u32 top_id = ol::physics_add_point_mass(&dim->physics, top);

    ol::Link link{};
    link.a = bottom_id;
    link.b = top_id;
    link.rest_length = 1.05f;
    link.stiffness = 1.0f;
    link.axis_lock = true;
    link.axis_lock_anchor_a = true;
    link.axis = {0.0f, 1.0f, 0.0f};
    ol::physics_add_link(&dim->physics, link);

    ol::PointMass* b0 = ol::arena_get(&dim->physics.masses, bottom_id);
    ol::PointMass* t0 = ol::arena_get(&dim->physics.masses, top_id);
    const float bottom_y_before = b0->pos.local.y;
    const float top_y_before = t0->pos.local.y;

    ol::physics_step(dim, 1.0f / 60.0f);

    b0 = ol::arena_get(&dim->physics.masses, bottom_id);
    t0 = ol::arena_get(&dim->physics.masses, top_id);
    return expect_true(near(b0->pos.local.y, bottom_y_before, 0.001f), "Axis-locked uncrouch keeps bottom mass anchored") &&
           expect_true(t0->pos.local.y > top_y_before + 0.1f, "Axis-locked uncrouch extends the top mass upward");
}

bool test_floor_collision_settles() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "physics", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);

    ol::BoxCollider floor{};
    floor.pos = test_pos(dim_id, {0.0f, -0.5f, 0.0f}, dim->chunk_size_m);
    floor.half = {8.0f, 0.5f, 8.0f};
    ol::physics_add_box(&dim->physics, floor);

    ol::PointMass mass{};
    mass.pos = test_pos(dim_id, {0.0f, 3.0f, 0.0f}, dim->chunk_size_m);
    mass.prev = mass.pos;
    mass.radius = 0.3f;
    mass.friction = 0.85f;
    const ol::u32 mass_id = ol::physics_add_point_mass(&dim->physics, mass);

    for (int i = 0; i < 180; ++i) {
        ol::physics_step(dim, 1.0f / 60.0f);
    }

    const ol::PointMass* result = ol::arena_get(&dim->physics.masses, mass_id);
    return expect_true(result->pos.local.y > 0.25f && result->pos.local.y < 0.65f,
        "Point mass settles near the top of the floor collider");
}

bool test_box_face_normals() {
    ol::MeshGeometry box = ol::make_box_geometry("box", {1.0f, 1.0f, 1.0f});
    for (ol::u32 i = 0; i + 1 < box.triangle_count; i += 2) {
        const ol::Triangle a = box.triangles[i];
        const ol::Triangle b = box.triangles[i + 1];
        const Vector3 na = ol::safe_norm(Vector3CrossProduct(box.vertices[a.b] - box.vertices[a.a], box.vertices[a.c] - box.vertices[a.a]));
        const Vector3 nb = ol::safe_norm(Vector3CrossProduct(box.vertices[b.b] - box.vertices[b.a], box.vertices[b.c] - box.vertices[b.a]));
        if (!expect_true(Vector3DotProduct(na, nb) > 0.999f, "Box triangles in each rectangular face have matching normals")) return false;
    }
    return true;
}

ol::u32 add_test_box(ol::Dimension* dim, ol::u32 dim_id, Vector3 center, Vector3 size) {
    ol::BoxCollider box{};
    box.pos = test_pos(dim_id, center, dim->chunk_size_m);
    box.half = size * 0.5f;
    return ol::physics_add_box(&dim->physics, box);
}

ol::u32 add_test_ramp_box(ol::Dimension* dim, ol::u32 dim_id, Vector3 low_center, float run_z, float rise_y, float width_x) {
    const float thickness = 0.20f;
    const float angle = -std::atan2(rise_y, run_z);
    const Quaternion rotation = QuaternionFromAxisAngle({1.0f, 0.0f, 0.0f}, angle);
    const Vector3 top_mid = {low_center.x, low_center.y + rise_y * 0.5f, low_center.z + run_z * 0.5f};
    const Vector3 top_offset = Vector3RotateByQuaternion({0.0f, thickness * 0.5f, 0.0f}, rotation);

    ol::BoxCollider box{};
    box.pos = test_pos(dim_id, top_mid - top_offset, dim->chunk_size_m);
    box.half = {width_x * 0.5f, thickness * 0.5f, std::sqrt(run_z * run_z + rise_y * rise_y) * 0.5f};
    box.axis_aligned = false;
    box.rotation = rotation;
    return ol::physics_add_box(&dim->physics, box);
}

ol::u32 add_demo_ramp_collider(ol::Dimension* dim, ol::u32 dim_id) {
    ol::BoxCollider box{};
    box.pos = test_pos(dim_id, {7.0f, 0.363f, 6.024f}, dim->chunk_size_m);
    box.half = {2.5f, 0.12f, 2.55f};
    box.axis_aligned = false;
    box.rotation = QuaternionFromAxisAngle({1.0f, 0.0f, 0.0f}, -std::atan2(1.05f, 5.0f));
    return ol::physics_add_box(&dim->physics, box);
}

bool ramp_surface_y_at(ol::Dimension* dim, ol::u32 dim_id, ol::u32 box_id, Vector3 feet, float radius, float* out_y) {
    const ol::BoxCollider* box = ol::arena_get(&dim->physics.boxes, box_id);
    if (!box) return false;
    const Vector3 normal = ol::safe_norm(Vector3RotateByQuaternion({0.0f, 1.0f, 0.0f}, box->rotation), {0.0f, 1.0f, 0.0f});
    const ol::WorldPos feet_pos = test_pos(dim_id, feet, dim->chunk_size_m);
    const Vector3 rel = ol::world_delta_meters(feet_pos, box->pos, dim->chunk_size_m);
    const Vector3 plane_point = Vector3RotateByQuaternion({0.0f, box->half.y, 0.0f}, box->rotation);
    const float plane_d = Vector3DotProduct(normal, plane_point);
    const float rel_y = (plane_d - normal.x * rel.x - normal.z * rel.z) / normal.y;
    const Vector3 on_plane = {rel.x, rel_y, rel.z};
    const Vector3 local = Vector3RotateByQuaternion(on_plane, QuaternionInvert(box->rotation));
    if (std::fabs(local.x) > box->half.x + radius) return false;
    if (std::fabs(local.z) > box->half.z + radius) return false;

    *out_y = static_cast<float>(ol::world_axis_meters(box->pos.chunk.y, box->pos.local.y, dim->chunk_size_m) + rel_y);
    return true;
}

float feet_y(ol::Dimension* dim, const ol::PlayerEntity* player) {
    return static_cast<float>(ol::world_axis_meters(
        ol::player_feet_pos(dim, player).chunk.y,
        ol::player_feet_pos(dim, player).local.y,
        dim->chunk_size_m
    ));
}

float feet_x(ol::Dimension* dim, const ol::PlayerEntity* player) {
    return static_cast<float>(ol::world_axis_meters(
        ol::player_feet_pos(dim, player).chunk.x,
        ol::player_feet_pos(dim, player).local.x,
        dim->chunk_size_m
    ));
}

float feet_z(ol::Dimension* dim, const ol::PlayerEntity* player) {
    return static_cast<float>(ol::world_axis_meters(
        ol::player_feet_pos(dim, player).chunk.z,
        ol::player_feet_pos(dim, player).local.z,
        dim->chunk_size_m
    ));
}

float view_eye_y(ol::Dimension* dim, const ol::PlayerEntity* player) {
    return feet_y(dim, player) + player->eye_height;
}

float visual_eye_y(ol::Dimension* dim, const ol::PlayerEntity* player) {
    return feet_y(dim, player) + player->eye_height + player->camera_y_offset;
}

float player_horizontal_speed(const ol::PlayerEntity* player) {
    return std::sqrt(player->velocity.x * player->velocity.x + player->velocity.z * player->velocity.z);
}

bool test_player_dimensions() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "player", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::WorldPos feet = ol::player_feet_pos(dim, player);
    Vector3 eye_delta = ol::world_delta_meters(ol::player_eye_pos(dim, player), feet, dim->chunk_size_m);
    bool ok = expect_true(near(player->current_height, 1.80f, 0.001f), "Standing player is 1.8m high");
    ok = expect_true(near(eye_delta.y, 1.70f, 0.001f), "Standing player eyes are at 1.7m") && ok;

    ol::PlayerControllerInput input{};
    input.crouch = true;
    ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    ok = expect_true(near(player->current_height, ol::player_crouch_height_m, 0.001f),
        "Crouch hitbox changes immediately") && ok;
    feet = ol::player_feet_pos(dim, player);
    eye_delta = ol::world_delta_meters(ol::player_eye_pos(dim, player), feet, dim->chunk_size_m);
    ok = expect_true(eye_delta.y < ol::player_stand_eye_m && eye_delta.y > ol::player_crouch_eye_m,
        "Crouch eye height eases visually") && ok;
    for (int i = 0; i < 20; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    feet = ol::player_feet_pos(dim, player);
    eye_delta = ol::world_delta_meters(ol::player_eye_pos(dim, player), feet, dim->chunk_size_m);
    ok = expect_true(near(player->current_height, 0.90f, 0.001f), "Crouched player hitbox is half height") && ok;
    ok = expect_true(near(eye_delta.y, 0.85f, 0.001f), "Crouched player eye level is half height") && ok;
    return ok;
}

bool test_player_crouch_tunnel_clearance() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    dim->physics.gravity = {0.0f, 0.0f, 0.0f};
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {6.0f, 1.0f, 6.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.0f, 0.30f, 3.0f});

    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.crouch = true;
    for (int i = 0; i < 20; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float crouched_y = ol::player_feet_pos(dim, player).local.y;
    bool ok = expect_true(near(player->current_height, ol::player_crouch_height_m, 0.001f), "Player can crouch under a 1m tunnel");

    input.crouch = false;
    for (int i = 0; i < 20; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float release_y = ol::player_feet_pos(dim, player).local.y;
    ok = expect_true(near(player->current_height, ol::player_crouch_height_m, 0.001f), "Player remains crouched when tunnel blocks uncrouch") && ok;
    ok = expect_true(near(release_y, crouched_y, 0.02f), "Blocked uncrouch does not pop the player upward") && ok;
    return ok;
}

bool test_player_wall_contact_is_stable() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "wall", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {0.0f, 0.8f, -1.2f}, {4.0f, 2.0f, 0.4f});

    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 1.8f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    float min_y = 1000.0f;
    float max_y = -1000.0f;
    for (int i = 0; i < 120; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float y = ol::player_feet_pos(dim, player).local.y;
        min_y = fminf(min_y, y);
        max_y = fmaxf(max_y, y);
    }

    return expect_true(max_y - min_y < 0.03f, "Walking into a box collider keeps camera/feet height stable");
}

bool test_player_airborne_sprint_speed_is_preserved() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "air", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {20.0f, 1.0f, 20.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    for (int i = 0; i < 10; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    input.jump_pressed = true;
    input.jump_held = true;
    ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float takeoff_speed = player_horizontal_speed(player);
    input.jump_pressed = false;
    float min_air_speed = 1000.0f;
    float max_air_speed = 0.0f;
    for (int i = 0; i < 30; ++i) {
        input.crouch = i >= 5;
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        if (!player->on_ground && i > 2) {
            const float speed = player_horizontal_speed(player);
            min_air_speed = fminf(min_air_speed, speed);
            max_air_speed = fmaxf(max_air_speed, speed);
        }
    }

    bool ok = expect_true(min_air_speed > takeoff_speed - 0.08f, "Air crouch does not brake horizontal momentum");
    ok = expect_true(max_air_speed < takeoff_speed + 0.08f, "Holding sprint in the air does not add speed") && ok;
    return ok;
}

float simulate_jump_apex(bool hold_jump) {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "jump", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {10.0f, 1.0f, 10.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    float apex = 0.0f;
    for (int i = 0; i < 180; ++i) {
        ol::PlayerControllerInput input{};
        input.jump_pressed = i == 0;
        input.jump_held = hold_jump;
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        apex = fmaxf(apex, feet_y(dim, player));
    }
    return apex;
}

bool test_player_variable_jump_heights() {
    const float tap_apex = simulate_jump_apex(false);
    const float held_apex = simulate_jump_apex(true);
    bool ok = expect_true(tap_apex > 0.24f && tap_apex < 0.38f, "Tap jump reaches about 0.3m");
    ok = expect_true(held_apex > 1.00f && held_apex < 1.20f, "Held jump reaches about 1.1m") && ok;
    ok = expect_true(held_apex > tap_apex + 0.60f, "Jump height varies with space hold time") && ok;
    return ok;
}

bool test_player_ground_movement_has_inertia() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "inertia", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {20.0f, 1.0f, 20.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float first_speed = std::sqrt(player->velocity.x * player->velocity.x + player->velocity.z * player->velocity.z);
    for (int i = 0; i < 20; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float settled_speed = std::sqrt(player->velocity.x * player->velocity.x + player->velocity.z * player->velocity.z);

    bool ok = expect_true(first_speed > 0.2f && first_speed < ol::player_walk_speed_mps * 0.5f, "Ground movement accelerates instead of snapping to max speed");
    ok = expect_true(settled_speed > ol::player_walk_speed_mps - 0.05f && settled_speed <= ol::player_walk_speed_mps + 0.05f, "Ground movement still reaches target speed quickly") && ok;
    return ok;
}

bool test_player_box_contact_does_not_cross() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "boxstop", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {10.0f, 1.0f, 10.0f});
    add_test_box(dim, dim_id, {0.0f, 0.75f, -1.0f}, {2.0f, 1.5f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 2.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float min_z = 1000.0f;
    for (int i = 0; i < 180; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float z = feet_z(dim, player);
        min_z = fminf(min_z, z);
    }

    const float final_z = feet_z(dim, player);
    bool ok = expect_true(min_z > 0.30f, "Player does not cross through the front face of a box");
    ok = expect_true(final_z > 0.30f && final_z < 0.60f, "Player remains stopped against the box instead of teleporting past it") && ok;
    return ok;
}

bool test_player_box_front_contact_preserves_lateral_offset() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "box_offset", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {10.0f, 1.0f, 10.0f});
    add_test_box(dim, dim_id, {0.0f, 0.75f, -1.0f}, {2.0f, 1.5f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.55f, 0.0f, 2.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float min_x = 1000.0f;
    float max_x = -1000.0f;
    for (int i = 0; i < 120; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        min_x = fminf(min_x, feet_x(dim, player));
        max_x = fmaxf(max_x, feet_x(dim, player));
    }

    bool ok = expect_true(feet_z(dim, player) > 0.30f && feet_z(dim, player) < 0.60f, "Off-center box approach stops on the contacted face");
    ok = expect_true(min_x > 0.50f && max_x < 0.60f, "Off-center box approach does not push the player sideways to a corner") && ok;
    return ok;
}

bool test_player_uncrouches_while_touching_box_side() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "uncrouch_box_side", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {10.0f, 1.0f, 10.0f});
    add_test_box(dim, dim_id, {0.0f, 0.75f, -1.0f}, {2.0f, 1.5f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 1.2f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.crouch = true;
    input.move = {0.0f, 1.0f};
    for (int i = 0; i < 35; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    const float contact_z = feet_z(dim, player);
    input.crouch = false;
    input.move = {0.0f, 0.0f};
    for (int i = 0; i < 10; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    bool ok = expect_true(contact_z > 0.30f && contact_z < 0.45f, "Crouched player is touching the box side");
    ok = expect_true(near(player->current_height, ol::player_stand_height_m, 0.001f), "Releasing crouch while touching a box side stands up") && ok;
    return ok;
}

bool test_player_corner_contact_has_no_lateral_teleport() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "corner", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {12.0f, 1.0f, 12.0f});
    add_test_box(dim, dim_id, {0.0f, 0.75f, -1.0f}, {2.0f, 1.5f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {1.45f, 0.0f, 2.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {-0.35f, 1.0f};
    input.sprint = true;
    float prev_x = feet_x(dim, player);
    float max_lateral_step = 0.0f;
    for (int i = 0; i < 100; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float x = feet_x(dim, player);
        max_lateral_step = fmaxf(max_lateral_step, std::fabs(x - prev_x));
        prev_x = x;
    }

    return expect_true(max_lateral_step < 0.085f, "Corner approach does not cause a one-frame lateral teleport");
}

bool test_player_box_corner_bisector_blocks() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "corner_bisector", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {12.0f, 1.0f, 12.0f});
    add_test_box(dim, dim_id, {0.0f, 0.75f, -1.0f}, {2.0f, 1.5f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {2.0f, 0.0f, 1.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = -3.14159265f * 0.25f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float min_corner_distance = 1000.0f;
    bool entered_box_body = false;
    for (int i = 0; i < 90; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float dx = feet_x(dim, player) - 1.0f;
        const float dz = feet_z(dim, player) - 0.0f;
        min_corner_distance = fminf(min_corner_distance, std::sqrt(dx * dx + dz * dz));
        if (feet_x(dim, player) > -1.0f && feet_x(dim, player) < 1.0f &&
            feet_z(dim, player) > -2.0f && feet_z(dim, player) < 0.0f) {
            entered_box_body = true;
        }
    }

    bool ok = expect_true(min_corner_distance > ol::player_radius_m - 0.03f, "Box corner keeps the cylindrical player radius outside");
    ok = expect_true(!entered_box_body, "Bisector corner approach does not pass through the box body") && ok;
    return ok;
}

bool test_player_slides_along_box_face() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "slide", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {28.0f, 1.0f, 12.0f});
    add_test_box(dim, dim_id, {0.0f, 0.75f, -1.0f}, {18.0f, 1.5f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {-4.0f, 0.0f, 0.35f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {1.0f, 1.0f};
    input.sprint = true;
    const float start_x = feet_x(dim, player);
    float min_z = 1000.0f;
    for (int i = 0; i < 80; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        min_z = fminf(min_z, feet_z(dim, player));
    }

    bool ok = expect_true(feet_x(dim, player) > start_x + 0.55f, "Player can slide sideways along a blocked box face");
    ok = expect_true(min_z > 0.30f, "Sliding along a box face does not leak through the wall") && ok;
    return ok;
}

bool test_player_jump_into_box_does_not_phase_or_fall() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "jump_box", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {80.0f, 1.0f, 80.0f});
    add_test_box(dim, dim_id, {0.0f, 0.25f, -2.0f}, {2.0f, 1.5f, 5.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 2.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float min_floor_y = 1000.0f;
    bool low_inside_box = false;
    for (int i = 0; i < 160; ++i) {
        input.jump_pressed = i == 8;
        input.jump_held = i >= 8 && i < 22;
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float x = feet_x(dim, player);
        const float y = feet_y(dim, player);
        const float z = feet_z(dim, player);
        min_floor_y = fminf(min_floor_y, y);
        if (x > -1.0f && x < 1.0f && z > -4.5f && z < 0.5f && y < 0.85f) {
            low_inside_box = true;
        }
    }

    bool ok = expect_true(!low_inside_box, "Jumping into a box does not move through its body below the top surface");
    ok = expect_true(min_floor_y > -0.05f, "Jumping into a box does not make the player fall through the floor") && ok;
    return ok;
}

bool test_player_near_ground_fall_does_not_snap_early() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "nosnap", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {10.0f, 1.0f, 10.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player_sync_masses_to_pose(dim, player, test_pos(dim_id, {0.0f, 0.15f, 0.0f}, dim->chunk_size_m));
    player->velocity = {0.0f, -0.10f, 0.0f};
    player->on_ground = false;

    ol::PlayerControllerInput input{};
    ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    bool ok = expect_true(feet_y(dim, player) > 0.12f, "Airborne player near the floor is not snapped down early");
    ok = expect_true(!player->on_ground, "Airborne player near the floor remains airborne until collision") && ok;
    return ok;
}

bool test_player_crouch_stuck_step_jump_does_not_fall_through_floor() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "step_crouch_jump", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f});
    add_test_box(dim, dim_id, {0.0f, 0.0f, -1.2f}, {3.0f, 1.0f, 3.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 1.9f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.crouch = true;
    input.move = {0.0f, 1.0f};
    for (int i = 0; i < 25; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    input.crouch = false;
    float min_y = feet_y(dim, player);
    bool low_inside_step = false;
    for (int i = 0; i < 100; ++i) {
        input.jump_pressed = i == 0;
        input.jump_held = i < 12;
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float x = feet_x(dim, player);
        const float y = feet_y(dim, player);
        const float z = feet_z(dim, player);
        min_y = fminf(min_y, y);
        if (x > -1.5f && x < 1.5f && z > -2.7f && z < 0.3f && y < 0.45f) {
            low_inside_step = true;
        }
    }

    bool ok = expect_true(min_y > -0.05f, "Jumping from crouch-stuck step contact does not fall through the floor");
    ok = expect_true(!low_inside_step, "Jumping from step contact does not phase through the step body") && ok;
    return ok;
}

bool test_player_slides_across_flush_wall_seam() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "flush_seam", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {50.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {-10.0f, 0.75f, -1.0f}, {20.0f, 1.5f, 2.0f});
    add_test_box(dim, dim_id, {10.0f, 0.75f, -1.0f}, {20.0f, 1.5f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {-3.4f, 0.0f, 0.35f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {1.0f, 1.0f};
    input.sprint = true;
    float min_z = 1000.0f;
    for (int i = 0; i < 120; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        min_z = fminf(min_z, feet_z(dim, player));
    }

    bool ok = expect_true(feet_x(dim, player) > 1.2f, "Player slides across a flush wall seam instead of stopping on the internal face");
    ok = expect_true(min_z > 0.30f, "Flush wall seam slide still keeps the player outside the wall") && ok;
    return ok;
}

bool test_player_jump_toward_tunnel_end_does_not_fall_through_floor() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_end_jump", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {10.0f, 1.0f, 10.0f});
    add_test_box(dim, dim_id, {-1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 5.0f});
    add_test_box(dim, dim_id, {1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 5.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.45f, 0.30f, 5.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 1.9f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.crouch = true;
    input.move = {0.0f, 1.0f};
    float min_y = feet_y(dim, player);
    bool roof_clip = false;
    for (int i = 0; i < 130; ++i) {
        input.jump_pressed = i == 20;
        input.jump_held = i >= 20 && i < 34;
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float y = feet_y(dim, player);
        min_y = fminf(min_y, y);
        if (feet_x(dim, player) > -1.7f && feet_x(dim, player) < 1.7f &&
            feet_z(dim, player) > -2.5f && feet_z(dim, player) < 2.5f &&
            y + player->current_height > 1.03f) {
            roof_clip = true;
        }
    }

    bool ok = expect_true(min_y > -0.05f, "Jumping toward the tunnel end does not fall through the floor");
    ok = expect_true(!roof_clip, "Jumping toward the tunnel end remains blocked by the low roof") && ok;
    return ok;
}

bool test_player_standing_tunnel_approach_jump_does_not_fall_through_floor() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_stand_jump", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {14.0f, 1.0f, 14.0f});
    add_test_box(dim, dim_id, {-1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 5.0f});
    add_test_box(dim, dim_id, {1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 5.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.45f, 0.30f, 5.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 3.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float min_y = feet_y(dim, player);
    float min_z = feet_z(dim, player);
    for (int i = 0; i < 120; ++i) {
        input.jump_pressed = i == 18;
        input.jump_held = i >= 18 && i < 30;
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        min_y = fminf(min_y, feet_y(dim, player));
        min_z = fminf(min_z, feet_z(dim, player));
    }

    bool ok = expect_true(min_y > -0.05f, "Standing jump into the tunnel entrance does not push the player through the floor");
    ok = expect_true(min_z > 2.25f, "Standing player remains blocked before entering the low tunnel") && ok;
    return ok;
}

bool test_player_cannot_stand_on_covered_tunnel_support() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "covered_support", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {0.75f, 0.15f, 0.0f}, {1.0f, 0.30f, 2.0f});
    add_test_box(dim, dim_id, {0.75f, 0.45f, 0.0f}, {1.0f, 0.30f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {-1.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.move = {1.0f, 0.0f};
    for (int i = 0; i < 80; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    }

    bool ok = expect_true(feet_y(dim, player) < 0.08f, "Player does not step onto a support whose top is covered by the tunnel roof");
    ok = expect_true(feet_x(dim, player) < -0.05f, "Covered support side remains a blocking wall") && ok;
    return ok;
}

bool test_player_rotated_ramp_side_blocks() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp_side", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 2.0f}, {8.0f, 1.0f, 8.0f});
    add_test_ramp_box(dim, dim_id, {0.0f, 0.0f, 0.0f}, 5.0f, 1.0f, 4.0f);
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {2.9f, 0.0f, 2.4f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {-1.0f, 0.0f};
    input.sprint = true;
    float min_x = feet_x(dim, player);
    for (int i = 0; i < 90; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        min_x = fminf(min_x, feet_x(dim, player));
    }

    return expect_true(min_x > 2.30f, "Player cannot walk through the rotated ramp side");
}

bool test_player_low_ramp_side_can_step_up() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp_side_low", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 2.0f}, {8.0f, 1.0f, 8.0f});
    add_test_ramp_box(dim, dim_id, {0.0f, 0.0f, 0.0f}, 5.0f, 1.0f, 4.0f);
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {2.9f, 0.0f, 0.65f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {-1.0f, 0.0f};
    float max_y = feet_y(dim, player);
    for (int i = 0; i < 35; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        max_y = fmaxf(max_y, feet_y(dim, player));
    }

    bool ok = expect_true(feet_x(dim, player) < 1.95f, "Player can step onto the low side of a rotated ramp");
    ok = expect_true(max_y > 0.05f, "Low-side ramp step places the player on the ramp surface") && ok;
    return ok;
}

bool test_player_ramp_edge_reaches_landing() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp_edge", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 4.0f}, {8.0f, 1.0f, 14.0f});
    add_test_ramp_box(dim, dim_id, {0.0f, 0.0f, 0.0f}, 5.0f, 1.0f, 4.0f);
    add_test_box(dim, dim_id, {0.0f, 0.5f, 7.0f}, {4.0f, 1.0f, 4.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {1.65f, 0.0f, -1.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 3.14159265f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    bool reached_landing = false;
    int landing_frames = 0;
    for (int i = 0; i < 220; ++i) {
        if (reached_landing) {
            input.move = {0.0f, 0.0f};
            landing_frames++;
        }
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        if (feet_z(dim, player) > 6.2f) reached_landing = true;
        if (landing_frames > 45) break;
    }

    bool ok = expect_true(feet_z(dim, player) > 6.0f, "Player moving along the ramp edge reaches the flat top block");
    ok = expect_true(feet_y(dim, player) > 0.94f, "Ramp edge transition leaves the player on the landing") && ok;
    return ok;
}

bool test_player_outer_ramp_edge_reaches_landing() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp_outer_edge", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 4.0f}, {8.0f, 1.0f, 14.0f});
    add_test_ramp_box(dim, dim_id, {0.0f, 0.0f, 0.0f}, 5.0f, 1.0f, 4.0f);
    add_test_box(dim, dim_id, {0.0f, 0.5f, 7.0f}, {4.0f, 1.0f, 4.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {2.12f, 0.0f, -1.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 3.14159265f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float min_speed_after_ramp_top = 1000.0f;
    bool reached_landing = false;
    int landing_frames = 0;
    for (int i = 0; i < 220; ++i) {
        if (reached_landing) {
            input.move = {0.0f, 0.0f};
            landing_frames++;
        }
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        if (!reached_landing && feet_z(dim, player) > 5.0f) {
            min_speed_after_ramp_top = fminf(min_speed_after_ramp_top, player_horizontal_speed(player));
        }
        if (feet_z(dim, player) > 6.2f) reached_landing = true;
        if (landing_frames > 45) break;
    }

    bool ok = expect_true(feet_z(dim, player) > 6.0f, "Player sprinting along the outer ramp edge reaches the flat top block");
    ok = expect_true(feet_y(dim, player) > 0.94f, "Outer ramp edge transition leaves the player on the landing") && ok;
    ok = expect_true(min_speed_after_ramp_top > 2.0f, "Outer ramp edge transition does not stick the player at the landing corner") && ok;
    return ok;
}

bool test_player_ramp_landing_has_no_collision_bump() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp_camera", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 4.0f}, {8.0f, 1.0f, 14.0f});
    add_test_ramp_box(dim, dim_id, {0.0f, 0.0f, 0.0f}, 5.0f, 1.0f, 4.0f);
    add_test_box(dim, dim_id, {0.0f, 0.5f, 7.0f}, {4.0f, 1.0f, 4.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, -1.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 3.14159265f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float previous_eye = view_eye_y(dim, player);
    float max_eye_step = 0.0f;
    bool reached_landing = false;
    int landing_frames = 0;
    for (int i = 0; i < 180; ++i) {
        if (reached_landing) {
            input.move = {0.0f, 0.0f};
            landing_frames++;
        }
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float eye_y = view_eye_y(dim, player);
        const float eye_step = std::fabs(eye_y - previous_eye);
        max_eye_step = fmaxf(max_eye_step, eye_step);
        previous_eye = eye_y;
        if (feet_z(dim, player) > 6.2f) reached_landing = true;
        if (landing_frames > 45) break;
    }

    return expect_true(max_eye_step < 0.065f, "Ramp-to-landing transition does not create a collision height bump");
}

bool test_player_ramp_ascent_projects_speed_without_top_dip() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp_slow", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 4.0f}, {8.0f, 1.0f, 14.0f});
    add_test_ramp_box(dim, dim_id, {0.0f, 0.0f, 0.0f}, 5.0f, 1.0f, 4.0f);
    add_test_box(dim, dim_id, {0.0f, 0.5f, 7.0f}, {4.0f, 1.0f, 4.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, -1.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 3.14159265f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float mid_ramp_speed = 0.0f;
    float speed_after_landing = 0.0f;
    float min_speed_after_top = 1000.0f;
    for (int i = 0; i < 180; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        if (mid_ramp_speed <= 0.0f && feet_z(dim, player) > 2.8f) {
            mid_ramp_speed = player_horizontal_speed(player);
        }
        if (feet_z(dim, player) > 5.0f) {
            min_speed_after_top = fminf(min_speed_after_top, player_horizontal_speed(player));
        }
        if (feet_z(dim, player) > 6.1f) {
            speed_after_landing = player_horizontal_speed(player);
            break;
        }
    }

    bool ok = expect_true(feet_z(dim, player) > 6.1f, "Player reaches the ramp landing while sprinting");
    ok = expect_true(mid_ramp_speed < ol::player_sprint_speed_mps && mid_ramp_speed > ol::player_sprint_speed_mps * 0.94f,
        "Ramp ascent only mildly reduces horizontal sprint speed by surface projection") && ok;
    ok = expect_true(min_speed_after_top > ol::player_sprint_speed_mps * 0.90f,
        "Ramp-to-flat transition does not create a visible speed dip on top") && ok;
    ok = expect_true(speed_after_landing > ol::player_sprint_speed_mps * 0.92f,
        "Player keeps most sprint speed after reaching the ramp landing") && ok;
    return ok;
}

bool test_player_yaw_slide_ramp_side_to_flush_landing() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp_side_yaw_slide", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {7.0f, -0.5f, 8.0f}, {24.0f, 1.0f, 28.0f});
    add_test_ramp_box(dim, dim_id, {7.0f, 0.0f, 3.5f}, 5.0f, 1.05f, 5.0f);
    add_test_box(dim, dim_id, {7.0f, 0.5f, 10.1f}, {5.0f, 1.0f, 4.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {10.6f, 0.0f, 6.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    player->yaw = -3.14159265f * 0.5f;
    for (int i = 0; i < 60; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float side_contact_x = feet_x(dim, player);

    player->yaw = -3.14159265f * 0.5f - 0.48f;
    float max_z = feet_z(dim, player);
    float speed_after_old_snag = 0.0f;
    for (int i = 0; i < 80; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        max_z = fmaxf(max_z, feet_z(dim, player));
        if (feet_z(dim, player) > 8.2f) speed_after_old_snag = fmaxf(speed_after_old_snag, player_horizontal_speed(player));
    }

    bool ok = expect_true(side_contact_x > 9.80f && side_contact_x < 9.90f, "Player first collides with the ramp side");
    ok = expect_true(max_z > 9.2f, "Yaw-forward slide along ramp side continues past the flush landing corner") && ok;
    ok = expect_true(speed_after_old_snag > 3.0f, "Flush landing corner does not stop the ramp-side slide") && ok;
    return ok;
}

bool test_player_low_ramp_side_camera_step_is_smoothed() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp_side_camera", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 2.0f}, {8.0f, 1.0f, 8.0f});
    add_test_ramp_box(dim, dim_id, {0.0f, 0.0f, 0.0f}, 5.0f, 1.0f, 4.0f);
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {2.9f, 0.0f, 0.65f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {-1.0f, 0.0f};
    float previous_real_eye = view_eye_y(dim, player);
    float previous_visual_eye = visual_eye_y(dim, player);
    float max_real_step = 0.0f;
    float max_visual_step = 0.0f;
    for (int i = 0; i < 35; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        const float real_eye = view_eye_y(dim, player);
        const float smoothed_eye = visual_eye_y(dim, player);
        max_real_step = fmaxf(max_real_step, std::fabs(real_eye - previous_real_eye));
        max_visual_step = fmaxf(max_visual_step, std::fabs(smoothed_eye - previous_visual_eye));
        previous_real_eye = real_eye;
        previous_visual_eye = smoothed_eye;
    }

    bool ok = expect_true(max_real_step > 0.035f, "Low ramp side entry has a real step that needs visual smoothing");
    ok = expect_true(max_visual_step < max_real_step * 0.75f, "Low ramp side visual camera step is smoothed") && ok;
    return ok;
}

bool test_player_crouch_transition_reverses_smoothly() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "crouch_reverse", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.crouch = true;
    for (int i = 0; i < 4; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float partial_crouch_eye = player->eye_height;

    input.crouch = false;
    ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float reversing_up_eye = player->eye_height;

    input.crouch = true;
    ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float reversing_down_eye = player->eye_height;

    bool ok = expect_true(near(player->current_height, ol::player_crouch_height_m, 0.001f), "Crouch hitbox remains instant while visual crouch eases");
    ok = expect_true(partial_crouch_eye < ol::player_stand_eye_m && partial_crouch_eye > ol::player_crouch_eye_m, "Crouch eye is partial during transition") && ok;
    ok = expect_true(reversing_up_eye > partial_crouch_eye && reversing_up_eye < ol::player_stand_eye_m, "Uncrouch eye reverses smoothly from mid-crouch") && ok;
    ok = expect_true(reversing_down_eye < reversing_up_eye && reversing_down_eye > ol::player_crouch_eye_m, "Crouch eye can be cancelled back downward smoothly") && ok;
    return ok;
}

bool test_player_air_crouch_brings_legs_up() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "air_crouch", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    dim->physics.gravity = {0.0f, 0.0f, 0.0f};
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 2.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    const float start_feet = feet_y(dim, player);
    const float start_top = start_feet + player->current_height;
    const float start_eye = start_feet + player->eye_height;

    ol::PlayerControllerInput input{};
    input.crouch = true;
    for (int i = 0; i < 20; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    const float end_feet = feet_y(dim, player);
    const float end_top = end_feet + player->current_height;
    const float end_eye = end_feet + player->eye_height;
    bool ok = expect_true(end_feet > start_feet + 0.80f, "Air crouch shortens the hitbox by bringing feet upward");
    ok = expect_true(near(end_top, start_top, 0.03f), "Air crouch keeps the top of the hitbox stable") && ok;
    ok = expect_true(near(end_eye, start_eye, 0.06f), "Air crouch keeps the camera/head height stable") && ok;
    input.crouch = false;
    ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float stand_feet = feet_y(dim, player);
    const float stand_top = stand_feet + player->current_height;
    const float stand_eye = stand_feet + player->eye_height;
    ok = expect_true(stand_feet < end_feet - 0.80f, "Air uncrouch restores the hitbox downward") && ok;
    ok = expect_true(near(stand_top, end_top, 0.03f), "Air uncrouch keeps the top of the hitbox stable") && ok;
    ok = expect_true(near(stand_eye, end_eye, 0.06f), "Air uncrouch does not move the camera upward") && ok;
    return ok;
}

bool test_player_floor_support_remains_at_furniture_side() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "floor_furniture_contact", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {0.0f, 0.75f, 0.0f}, {2.0f, 1.5f, 2.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 2.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    float min_y = feet_y(dim, player);
    for (int i = 0; i < 120; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        min_y = fminf(min_y, feet_y(dim, player));
    }

    bool ok = expect_true(min_y > -0.05f, "Touching floor-mounted furniture does not invalidate the floor support");
    ok = expect_true(feet_z(dim, player) > 1.25f && feet_z(dim, player) < 1.45f, "Floor-mounted furniture still blocks horizontal movement") && ok;
    return ok;
}

bool test_player_stopped_jump_gains_air_control_speed() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "air_from_rest", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {20.0f, 1.0f, 20.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE,
        test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.jump_pressed = true;
    input.jump_held = true;
    ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    input.jump_pressed = false;
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    float max_air_speed = 0.0f;
    for (int i = 0; i < 45; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        if (!player->on_ground) max_air_speed = fmaxf(max_air_speed, player_horizontal_speed(player));
    }

    bool ok = expect_true(max_air_speed > 3.5f, "A jump from rest can build useful ledge-reaching speed in the air");
    ok = expect_true(max_air_speed <= ol::player_air_control_speed_mps + 0.05f,
        "Air control from rest stays below walking speed even while sprint is held") && ok;
    return ok;
}

bool test_world_texture_pixel_settings_and_paint() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "texture", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    bool ok = expect_true(dim && near(dim->pixels_per_meter, 16.0f, 0.001f), "World textures default to 16 pixels per meter");
    if (!dim) return false;

    ol::PaintedPixel pixel{};
    pixel.center = test_pos(dim_id, {1.03125f, 2.0f, 3.03125f}, dim->chunk_size_m);
    pixel.color = RED;
    ok = expect_true(ol::dimension_paint_pixel(dim, pixel), "A world texture pixel can be painted") && ok;
    pixel.color = BLUE;
    ok = expect_true(ol::dimension_paint_pixel(dim, pixel), "Painting the same pixel updates it") && ok;
    ok = expect_true(dim->painted_pixels.size() == 1 && ColorIsEqual(dim->painted_pixels[0].color, BLUE),
        "A painted texel is shared world state rather than duplicate geometry") && ok;
    dim->painted_pixels.resize(8192);
    pixel.center = test_pos(dim_id, {100.03125f, 2.0f, 3.03125f}, dim->chunk_size_m);
    ok = expect_true(ol::dimension_paint_pixel(dim, pixel) && dim->painted_pixels.size() == 8193,
        "Painting continues beyond the former hills-session limit") && ok;
    return ok;
}

bool test_playground_clipboard_view_can_paint() {
    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "playground");
    std::snprintf(app->session_name, sizeof(app->session_name), "paint-ray-test");
    ol::demo_generate_world(app.get());
    ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);
    if (!dim) return expect_true(false, "Playground paint test generated a dimension");

    ol::CameraView view{};
    view.anchor = test_pos(app->dimension_id, {4.6638f, -0.05f, 2.9160f}, dim->chunk_size_m);
    view.eye_height = 1.70f;
    view.yaw = -141.593109f;
    view.pitch = -0.792199f;
    const bool painted = ol::demo_paint_at_view(app.get(), view, MAGENTA);
    bool ok = expect_true(painted, "Reported playground clipboard view targets a paintable surface");
    ok = expect_true(dim->painted_pixels.size() == 1, "Painting that view creates one world texel") && ok;

    app->last_drag_pixel_valid = false;
    ol::CameraView ramp_view{};
    ramp_view.anchor = test_pos(app->dimension_id, {6.9881f, 0.0f, 2.4975f}, dim->chunk_size_m);
    ramp_view.eye_height = 1.70f;
    ramp_view.yaw = -154.016922f;
    ramp_view.pitch = -0.899999f;
    ok = expect_true(ol::demo_paint_at_view(app.get(), ramp_view, SKYBLUE),
        "Reported lower-ramp clipboard view targets a paintable surface") && ok;

    app->last_drag_pixel_valid = false;
    ol::CameraView sprite_view{};
    sprite_view.anchor = test_pos(app->dimension_id, {-3.0f, -0.1f, -6.0f}, dim->chunk_size_m);
    sprite_view.eye_height = 1.70f;
    sprite_view.yaw = 0.0f;
    sprite_view.pitch = 0.0f;
    const ol::u32 before_sprite = static_cast<ol::u32>(dim->painted_pixels.size());
    ok = expect_true(ol::demo_paint_at_view(app.get(), sprite_view, RED), "Sprites accept painted texels") && ok;
    bool found_sprite_paint = false;
    for (ol::u32 i = before_sprite; i < dim->painted_pixels.size(); ++i) {
        found_sprite_paint = found_sprite_paint || ol::id_valid(dim->painted_pixels[i].sprite_id);
    }
    ok = expect_true(dim->painted_pixels.size() == before_sprite + 1 && found_sprite_paint, "Sprite paint remains attached to its sprite") && ok;
    app->last_drag_pixel_valid = false;
    ok = expect_true(ol::demo_paint_at_view(app.get(), sprite_view, BLUE), "Another player color can replace a painted sprite texel") && ok;
    ok = expect_true(dim->painted_pixels.size() == before_sprite + 1, "Painting over a texel replaces instead of stacking") && ok;
    ok = expect_true(ol::demo_erase_at_view(app.get(), sprite_view), "The five-pixel RMB brush erases sprite paint") && ok;
    ok = expect_true(dim->painted_pixels.size() == before_sprite, "Erase removes paint regardless of its author color") && ok;

    app->last_drag_pixel_valid = false;
    ol::CameraView stroke{};
    stroke.anchor = test_pos(app->dimension_id, {0.0f, 0.0f, 8.0f}, dim->chunk_size_m);
    stroke.eye_height = 1.70f;
    stroke.pitch = -0.80f;
    stroke.yaw = 0.0f;
    ol::demo_paint_at_view(app.get(), stroke, MAGENTA);
    const ol::u32 before_drag = static_cast<ol::u32>(dim->painted_pixels.size());
    stroke.yaw = 0.12f;
    ol::demo_paint_at_view(app.get(), stroke, MAGENTA);
    ok = expect_true(dim->painted_pixels.size() > before_drag + 1, "Dragging fills a continuous run of intervening texels") && ok;
    app->last_drag_pixel_valid = false;
    ol::CameraView too_far{};
    too_far.anchor = test_pos(app->dimension_id, {15.0f, 0.0f, 15.0f}, dim->chunk_size_m);
    too_far.eye_height = 20.0f;
    too_far.pitch = -PI * 0.5f;
    ok = expect_true(!ol::demo_paint_at_view(app.get(), too_far, RED), "Paint rays stop at ten meters") && ok;
    return ok;
}

bool test_player_cannot_be_squeezed_through_narrow_gap() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "narrow_gap", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {10.0f, 1.0f, 10.0f});
    add_test_box(dim, dim_id, {-0.65f, 1.0f, 0.0f}, {0.7f, 2.0f, 3.0f});
    add_test_box(dim, dim_id, {0.65f, 1.0f, 0.0f}, {0.7f, 2.0f, 3.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE,
        test_pos(dim_id, {0.0f, 0.0f, 2.4f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;
    for (int i = 0; i < 120; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    const float x = feet_x(dim, player);
    const float z = feet_z(dim, player);
    bool ok = expect_true(z > 1.55f, "A sub-diameter gap blocks the player instead of squeezing him through");
    ok = expect_true(std::fabs(x) < 0.10f, "Opposing collision corrections do not eject the player sideways") && ok;
    return ok;
}

bool test_player_auto_stands_after_tunnel_exit() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_exit", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    dim->physics.gravity = {0.0f, 0.0f, 0.0f};
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.0f, 0.30f, 3.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.crouch = true;
    for (int i = 0; i < 10; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    input.crouch = false;
    input.move = {1.0f, 0.0f};
    for (int i = 0; i < 90; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    const float x = ol::player_feet_pos(dim, player).local.x;
    bool ok = expect_true(x > 1.85f, "Player exits the low tunnel");
    ok = expect_true(near(player->current_height, ol::player_stand_height_m, 0.001f), "Player automatically stands after leaving tunnel clearance") && ok;
    return ok;
}

bool test_player_jump_does_not_pass_through_tunnel_roof() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_jump", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.0f, 0.30f, 3.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.crouch = true;
    for (int i = 0; i < 10; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    input.jump_pressed = true;
    input.jump_held = true;
    float max_top = feet_y(dim, player) + player->current_height;
    for (int i = 0; i < 80; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        input.jump_pressed = false;
        max_top = fmaxf(max_top, feet_y(dim, player) + player->current_height);
    }

    return expect_true(max_top < 1.03f, "Jumping under the tunnel roof collides with the ceiling instead of passing through");
}

bool test_player_tunnel_exit_uncrouch_does_not_pass_through_roof() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_exit_uncrouch", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {-1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 5.0f});
    add_test_box(dim, dim_id, {1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 5.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.45f, 0.30f, 5.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 2.72f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.crouch = true;
    for (int i = 0; i < 10; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    input.crouch = false;
    for (int i = 0; i < 12; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    bool ok = expect_true(near(player->current_height, ol::player_crouch_height_m, 0.001f),
        "Uncrouch remains blocked while the cylinder still overlaps the tunnel exit roof edge");
    ok = expect_true(feet_y(dim, player) + player->current_height < 1.02f,
        "Blocked tunnel-exit uncrouch does not put the player through the roof") && ok;
    return ok;
}

bool test_player_crouched_tunnel_side_jump_matches_stand_clearance() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_side_jump", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {-1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 3.0f});
    add_test_box(dim, dim_id, {1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 3.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.45f, 0.30f, 3.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {2.60f, 0.0f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.crouch = true;
    for (int i = 0; i < 8; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    input.move = {-1.0f, 0.0f};
    for (int i = 0; i < 60; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    const float contact_x = feet_x(dim, player);
    input.crouch = false;
    input.move = {0.0f, 0.0f};
    for (int i = 0; i < 8; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const bool can_stand_at_side = near(player->current_height, ol::player_stand_height_m, 0.001f);

    input.crouch = true;
    for (int i = 0; i < 8; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    input.jump_pressed = true;
    input.jump_held = true;
    float max_top = feet_y(dim, player) + player->current_height;
    for (int i = 0; i < 24; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        input.jump_pressed = false;
        max_top = fmaxf(max_top, feet_y(dim, player) + player->current_height);
    }

    bool ok = expect_true(contact_x > 2.06f && contact_x < 2.09f, "Crouched player reaches the real tunnel side contact point");
    ok = expect_true(can_stand_at_side, "Standing beside the tunnel roof side has enough clearance") && ok;
    ok = expect_true(max_top > 1.08f, "Crouched jump beside the tunnel side is not clamped by the roof edge") && ok;
    return ok;
}

bool test_player_tunnel_entrance_allows_side_slide() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_entrance", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {-1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 3.0f});
    add_test_box(dim, dim_id, {1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 3.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.45f, 0.30f, 3.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 2.2f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 0.0f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    for (int i = 0; i < 60; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float blocked_z = feet_z(dim, player);

    input.move = {1.0f, 0.0f};
    float max_x = feet_x(dim, player);
    for (int i = 0; i < 90; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        max_x = fmaxf(max_x, feet_x(dim, player));
    }

    bool ok = expect_true(blocked_z > 1.25f, "Standing player is blocked by the low tunnel entrance");
    ok = expect_true(max_x > 0.90f, "Player can strafe sideways along the tunnel entrance without getting stuck") && ok;
    return ok;
}

bool test_player_yaw_slide_tunnel_entrance_to_flush_support() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_yaw_slide", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {20.0f, 1.0f, 20.0f});
    add_test_box(dim, dim_id, {-1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 5.0f});
    add_test_box(dim, dim_id, {1.5f, 0.5f, 0.0f}, {0.45f, 1.0f, 5.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.45f, 0.30f, 5.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, 3.2f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    player->yaw = 0.0f;
    for (int i = 0; i < 60; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    const float blocked_z = feet_z(dim, player);

    player->yaw = 0.48f;
    float max_x = feet_x(dim, player);
    float speed_after_old_snag = 0.0f;
    for (int i = 0; i < 80; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        max_x = fmaxf(max_x, feet_x(dim, player));
        if (feet_x(dim, player) > 1.10f) speed_after_old_snag = fmaxf(speed_after_old_snag, player_horizontal_speed(player));
    }

    bool ok = expect_true(blocked_z > 2.80f && blocked_z < 2.90f, "Standing player first collides with the tunnel roof entrance");
    ok = expect_true(max_x > 1.8f, "Yaw-forward slide along tunnel roof continues past the flush support corner") && ok;
    ok = expect_true(speed_after_old_snag > 3.0f, "Flush tunnel support corner does not stop the roof slide") && ok;
    return ok;
}

bool test_player_uncrouches_on_tunnel_roof() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "tunnel_roof_uncrouch", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 0.0f}, {8.0f, 1.0f, 8.0f});
    add_test_box(dim, dim_id, {0.0f, 1.15f, 0.0f}, {3.0f, 0.30f, 3.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 1.3f, 0.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    ol::PlayerControllerInput input{};
    input.crouch = true;
    for (int i = 0; i < 12; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
    input.crouch = false;
    for (int i = 0; i < 20; ++i) ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

    bool ok = expect_true(feet_y(dim, player) > 1.25f, "Player remains on top of the tunnel roof");
    ok = expect_true(near(player->current_height, ol::player_stand_height_m, 0.001f), "Player stands up after crouching on top of the tunnel roof") && ok;
    return ok;
}

bool test_player_rotated_box_ramp_transition() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "ramp", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.5f, 4.0f}, {8.0f, 1.0f, 14.0f});
    add_test_ramp_box(dim, dim_id, {0.0f, 0.0f, 0.0f}, 5.0f, 1.0f, 4.0f);
    add_test_box(dim, dim_id, {0.0f, 0.5f, 7.0f}, {4.0f, 1.0f, 4.0f});
    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {0.0f, 0.0f, -1.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);
    player->yaw = 3.14159265f;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    float min_y_after_top = 1000.0f;
    bool reached_landing = false;
    int landing_frames = 0;
    for (int i = 0; i < 220; ++i) {
        if (reached_landing) {
            input.move = {0.0f, 0.0f};
            landing_frames++;
        }
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        if (feet_z(dim, player) > 5.15f) min_y_after_top = fminf(min_y_after_top, feet_y(dim, player));
        if (feet_z(dim, player) > 6.2f) reached_landing = true;
        if (landing_frames > 45) break;
    }

    bool ok = expect_true(feet_z(dim, player) > 6.0f, "Player moves off the rotated-box ramp onto the landing");
    ok = expect_true(feet_y(dim, player) > 0.94f && feet_y(dim, player) < 1.06f, "Player remains on the landing height after the ramp") && ok;
    ok = expect_true(min_y_after_top > 0.90f, "Player does not fall during ramp-to-landing transition") && ok;
    ok = expect_true(player->on_ground, "Player remains grounded after leaving the ramp") && ok;
    return ok;
}

bool test_player_sprint_jump_from_demo_stairs_to_ramp_does_not_fall_through() {
    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "demo_stair_ramp_jump", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    add_test_box(dim, dim_id, {0.0f, -0.55f, 0.0f}, {40.0f, 1.0f, 40.0f});
    add_test_box(dim, dim_id, {-8.0f, 0.0f, 5.0f}, {3.0f, 1.0f, 3.0f});
    add_test_box(dim, dim_id, {-8.0f, 0.5f, 8.0f}, {3.0f, 2.0f, 3.0f});
    add_test_box(dim, dim_id, {-8.0f, 1.0f, 11.0f}, {3.0f, 3.0f, 3.0f});
    const ol::u32 ramp_id = add_demo_ramp_collider(dim, dim_id);
    add_test_box(dim, dim_id, {7.0f, 0.5f, 10.6f}, {5.0f, 1.0f, 4.0f});

    const ol::u32 player_id = ol::dimension_add_player(dim, "tester", WHITE, test_pos(dim_id, {-8.0f, 2.5f, 11.0f}, dim->chunk_size_m), true);
    ol::PlayerEntity* player = ol::arena_get(&dim->players, player_id);

    const Vector3 target = {7.0f, 0.0f, 6.0f};
    const Vector3 start = {-8.0f, 0.0f, 11.0f};
    const Vector3 to_target = ol::safe_norm(target - start, {0.0f, 0.0f, -1.0f});
    player->yaw = std::atan2(to_target.x, -to_target.z);

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    input.sprint = true;

    float min_clearance_on_ramp = 1000.0f;
    bool reached_ramp_footprint = false;
    bool low_inside_ramp = false;
    for (int i = 0; i < 180; ++i) {
        input.jump_pressed = i == 0;
        input.jump_held = i < 12;
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);

        const float x = feet_x(dim, player);
        const float y = feet_y(dim, player);
        const float z = feet_z(dim, player);
        float surface_y = 0.0f;
        if (ramp_surface_y_at(dim, dim_id, ramp_id, {x, y, z}, player->body_radius - 0.04f, &surface_y)) {
            reached_ramp_footprint = true;
            const float clearance = y - surface_y;
            min_clearance_on_ramp = fminf(min_clearance_on_ramp, clearance);
            if (clearance < -0.08f) low_inside_ramp = true;
        }
    }

    bool ok = expect_true(reached_ramp_footprint, "Sprint jump from demo stairs reaches the ramp footprint");
    ok = expect_true(!low_inside_ramp, "Sprint jump from demo stairs does not fall through the ramp body") && ok;
    ok = expect_true(min_clearance_on_ramp > -0.08f, "Sprint jump from demo stairs stays on or above the ramp surface") && ok;
    return ok;
}

bool test_hills_radius_32_streams_without_physics_overload() {
    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "hills");
    std::snprintf(app->session_name, sizeof(app->session_name), "stream-radius-32");
    app->profile.render_radius_chunks = 32;

    ol::demo_generate_world(app.get());
    ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);

    ol::u32 valid_chunks = 0;
    ol::u32 collider_chunks = 0;
    for (const ol::StreamedWorldChunk& chunk : app->streamed_chunks) {
        if (!chunk.valid) continue;
        ++valid_chunks;
        if (chunk.colliders_loaded) ++collider_chunks;
    }

    bool ok = expect_true(dim != nullptr, "Hills radius 32 generated a dimension");
    ok = expect_true(dim && dim->render_radius_chunks == 32, "Hills radius 32 applied profile render radius") && ok;
    ok = expect_true(valid_chunks > 3000, "Hills radius 32 streams well beyond the old fixed chunk cap") && ok;
    ok = expect_true(dim && dim->meshes.count > 12000, "Hills radius 32 creates the visual terrain radius") && ok;
    ok = expect_true(collider_chunks > 20 && collider_chunks < 40, "Hills radius 32 keeps colliders local to the player") && ok;
    ok = expect_true(dim && dim->physics.boxes.count > 80 && dim->physics.boxes.count < 600, "Hills radius 32 does not overload physics boxes") && ok;
    return ok;
}

bool test_hills_prefetches_with_bounded_chunk_commits() {
    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "hills");
    std::snprintf(app->session_name, sizeof(app->session_name), "stream-prefetch-budget");
    ol::demo_generate_world(app.get());
    ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);
    ol::PlayerEntity* player = dim ? ol::arena_get(&dim->players, app->local_player_id) : nullptr;
    if (!dim || !player) return expect_true(false, "Hills prefetch test generated player and dimension");

    const ol::ChunkCoord original_center = app->landscape_stream_center;
    const ol::WorldPos near_seam = test_pos(
        app->dimension_id,
        {0.0f, feet_y(dim, player), 13.0f},
        dim->chunk_size_m);
    move_test_player_to(dim, app->local_player_id, near_seam);
    player->velocity = {0.0f, 0.0f, 4.0f};
    ol::demo_update_landscape_streaming(app.get(), dim, player);
    bool bounded = app->debug_landscape_chunks_loaded <= 2 && app->debug_landscape_chunks_unloaded <= 2;
    const bool predicted_next_chunk = app->landscape_stream_center.z == original_center.z + 1;
    for (int frame = 0; frame < 40; ++frame) {
        ol::demo_update_landscape_streaming(app.get(), dim, player);
        bounded = bounded && app->debug_landscape_chunks_loaded <= 2 && app->debug_landscape_chunks_unloaded <= 2;
    }
    return expect_true(predicted_next_chunk,
               "Hills streamer prefetches the velocity-facing neighbor before the seam") &&
           expect_true(bounded,
               "Hills streamer commits at most two chunk loads and unloads per update");
}

bool test_realtime_lighting_defaults() {
    const ol::RadianceCascadeSettings engine_defaults{};
    const auto app = std::make_unique<ol::DemoApp>();
    bool ok = expect_true(engine_defaults.temporal_frames == 0,
        "Radiance cascades do not temporally accumulate by default");
    ok = expect_true(!engine_defaults.jitter,
        "Radiance cascade ray jitter is disabled with temporal accumulation") && ok;
    ok = expect_true(!engine_defaults.corner_merge,
        "Radiance cascade corner merging is opt-in") && ok;
    ok = expect_true(app->profile.lighting.temporal_frames == 0 &&
            !app->profile.lighting.jitter && !app->profile.lighting.corner_merge,
        "New demo profiles inherit realtime non-temporal lighting defaults") && ok;
    return ok;
}

bool test_pathtrace_comparison_toggle_state() {
    ol::RenderState renderer{};
    bool ok = expect_true(!renderer.pathtrace_comparison_enabled,
        "Full-frame path tracing is opt-in");
    ok = expect_true(!ol::renderer_toggle_pathtrace_comparison(&renderer) &&
            !renderer.pathtrace_comparison_enabled,
        "Unavailable path-trace shader cannot enable comparison mode") && ok;
    renderer.pathtrace_comparison_shader_ready = true;
    ok = expect_true(ol::renderer_toggle_pathtrace_comparison(&renderer) &&
            renderer.pathtrace_comparison_enabled,
        "Path-trace comparison toggles on when its shader is available") && ok;
    ok = expect_true(!ol::renderer_toggle_pathtrace_comparison(&renderer) &&
            !renderer.pathtrace_comparison_enabled,
        "A second path-trace toggle returns to radiance cascades") && ok;
    return ok;
}

bool test_playground_primary_surfaces_are_lit() {
    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "playground");
    std::snprintf(app->session_name, sizeof(app->session_name), "playground-lighting-materials");
    ol::demo_generate_world(app.get());
    const ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);
    constexpr const char* required_names[] = {
        "floor", "north wall", "west wall", "tunnel left",
        "tunnel right", "tunnel roof", "ramp landing"
    };
    ol::u32 lit_required_count = 0;
    if (dim) {
        for (const char* required : required_names) {
            for (ol::u32 slot = 0; slot < dim->meshes.count; ++slot) {
                const ol::MeshInstance& mesh = dim->meshes.data[slot];
                if (std::strcmp(mesh.name, required) == 0 && mesh.lit) {
                    ++lit_required_count;
                    break;
                }
            }
        }
    }
    return expect_true(dim && lit_required_count == sizeof(required_names) / sizeof(required_names[0]),
        "Playground floor, walls, tunnel, and landing participate in surface-texel lighting");
}

bool test_cave_world_generation_and_materials() {
    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "cave");
    std::snprintf(app->session_name, sizeof(app->session_name), "cave-generation");

    ol::demo_generate_world(app.get());
    ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);
    ol::PlayerEntity* player = dim ? ol::arena_get(&dim->players, app->local_player_id) : nullptr;

    ol::u32 emissive_count = 0;
    ol::u32 reflective_count = 0;
    ol::u32 shell_tile_count = 0;
    bool reflectivity_allowlist_valid = true;
    if (dim) {
        for (ol::u32 slot = 0; slot < dim->meshes.count; ++slot) {
            const ol::MeshInstance* mesh = &dim->meshes.data[slot];
            if (Vector3Length(ol::resolve_mesh_emission(mesh)) > 0.01f) ++emissive_count;
            float expected_reflectivity = 0.0f;
            if (std::strcmp(mesh->name, "cave mirror panel") == 0) expected_reflectivity = 0.95f;
            else if (std::strcmp(mesh->name, "cave reflective plinth") == 0) expected_reflectivity = 0.70f;
            else if (std::strcmp(mesh->name, "cave reflective pool") == 0) expected_reflectivity = 0.85f;
            if (expected_reflectivity > 0.0f) ++reflective_count;
            const float actual_reflectivity = ol::resolve_mesh_reflectivity(mesh);
            reflectivity_allowlist_valid = reflectivity_allowlist_valid &&
                near(actual_reflectivity, expected_reflectivity, 0.0001f);
            for (ol::u32 layer_index = 0; layer_index < mesh->materials.count; ++layer_index) {
                if (expected_reflectivity <= 0.0f && mesh->materials.layers[layer_index].has_reflectivity) {
                    reflectivity_allowlist_valid = false;
                }
            }
            if (std::strncmp(mesh->name, "cave floor", 10) == 0 ||
                std::strncmp(mesh->name, "cave ceiling", 12) == 0 ||
                std::strncmp(mesh->name, "cave west wall", 14) == 0 ||
                std::strncmp(mesh->name, "cave east wall", 14) == 0 ||
                std::strncmp(mesh->name, "cave north wall", 15) == 0 ||
                std::strncmp(mesh->name, "cave south wall", 15) == 0) {
                ++shell_tile_count;
            }
        }
    }

    bool emitters_valid = true;
    bool emitter_chunks_match_bases = true;
    if (dim) {
        for (ol::u32 i = 0; i < app->cave_emissive_mesh_ids.size(); ++i) {
            const ol::MeshInstance* mesh = ol::arena_get(&dim->meshes, app->cave_emissive_mesh_ids[i]);
            emitters_valid = emitters_valid && mesh != nullptr;
            if (!mesh) continue;
            const ol::WorldPos base = test_pos(app->dimension_id, app->cave_emissive_base_positions[i], dim->chunk_size_m);
            emitter_chunks_match_bases = emitter_chunks_match_bases && ol::chunk_equal(mesh->origin.chunk, base.chunk);
        }
    } else {
        emitters_valid = false;
        emitter_chunks_match_bases = false;
    }

    bool ok = expect_true(dim != nullptr && std::strcmp(dim->name, "cave") == 0,
        "Cave selection generates a cave dimension");
    ok = expect_true(std::strcmp(app->world_name, "cave") == 0,
        "Cave survives world-name canonicalization") && ok;
    ok = expect_true(dim && !app->landscape_streaming && dim->render_radius_chunks == 4 && dim->quality_render_radius_chunks == 3,
        "Cave uses its compact non-streamed render settings") && ok;
    ok = expect_true(shell_tile_count == 30,
        "Cave shell has chunk-local floor, ceiling, and wall tiles") && ok;
    ok = expect_true(dim && dim->physics.boxes.count >= 19,
        "Cave shell and shadow shapes have collision") && ok;
    ok = expect_true(emissive_count == 3 && emitters_valid,
        "Cave has exactly three material-driven flying emitters") && ok;
    ok = expect_true(emitter_chunks_match_bases,
        "Cave emitters begin in their registered animation chunks") && ok;
    ok = expect_true(reflective_count == 3 && reflectivity_allowlist_valid,
        "Only the cave mirror, plinth, and pool carry reflective materials") && ok;
    ok = expect_true(player && std::fabs(feet_x(dim, player) - 8.0f) < 0.01f &&
            std::fabs(feet_y(dim, player)) < 0.01f && std::fabs(feet_z(dim, player) - 27.0f) < 0.01f,
        "Cave player spawns safely inside the chamber") && ok;
    return ok;
}

bool test_cave_surface_mip_policy_has_no_distance_thresholds() {
    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "cave");
    std::snprintf(app->session_name, sizeof(app->session_name), "cave-density-mip-policy");
    ol::demo_generate_world(app.get());
    const ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);

    const ol::MeshInstance* floor = nullptr;
    if (dim) {
        for (ol::u32 slot = 0; slot < dim->meshes.count; ++slot) {
            const ol::MeshInstance& candidate = dim->meshes.data[slot];
            if (std::strcmp(candidate.name, "cave floor 1 1") == 0) {
                floor = &candidate;
                break;
            }
        }
    }

    // Hold apparent texel density fixed while walking a real cave surface's
    // mesh origin across both sides of every former 0.75/1.5/2.5 chunk cutoff.
    // The exact density-selected level may be tuned for GPU cost, but changing
    // only world distance must never change it.
    constexpr float old_threshold_walk[] = {
        0.74f, 0.76f, 1.49f, 1.51f, 2.49f, 2.51f};
    bool distances_reproduced = dim && floor;
    bool mip_stable = dim && floor;
    const int density_selected_mip =
        ol::renderer_surface_lighting_mip_level(0.75f, 0);
    if (dim && floor) {
        for (float distance_chunks : old_threshold_walk) {
            ol::WorldPos camera = ol::worldpos_offset(
                floor->origin,
                {distance_chunks * dim->chunk_size_m, 0.0f, 0.0f},
                dim->chunk_size_m);
            const float measured_distance = ol::safe_len(ol::world_delta_meters(
                camera, floor->origin, dim->chunk_size_m)) / dim->chunk_size_m;
            distances_reproduced = distances_reproduced &&
                near(measured_distance, distance_chunks, 0.0001f);
            mip_stable = mip_stable &&
                ol::renderer_surface_lighting_mip_level(0.75f, 0) ==
                    density_selected_mip;
        }
    }

    bool ok = expect_true(
        dim && floor && floor->lit && dim->pixels_per_meter == 16.0f,
        "Distance-mip regression exercises the 16 ppm cave floor surface");
    ok = expect_true(
        distances_reproduced && mip_stable,
        "Cave surface resolve mip does not flip at former mesh-origin distance thresholds") && ok;
    ok = expect_true(
        ol::renderer_surface_lighting_mip_level(1.99f, 1) == 1 &&
        ol::renderer_surface_lighting_mip_level(2.01f, 1) == 0 &&
        ol::renderer_surface_lighting_mip_level(0.69f, 1) == 2 &&
        ol::renderer_surface_lighting_mip_level(0.71f, 1) == 1 &&
        ol::renderer_surface_lighting_mip_level(0.94f, 2) == 2 &&
        ol::renderer_surface_lighting_mip_level(0.96f, 2) == 1 &&
        ol::renderer_surface_lighting_mip_level(0.44f, 3) == 3 &&
        ol::renderer_surface_lighting_mip_level(0.46f, 3) == 2,
        "Surface resolve mip retains projected-density hysteresis") && ok;
    return ok;
}

bool test_hills_house_block_contact_keeps_floor_support();

bool run_physics_tests() {
    bool ok = true;
    auto run = [&](const char* name, bool (*fn)()) {
        std::printf("running %s\n", name);
        std::fflush(stdout);
        ok = fn() && ok;
    };
    run("worldpos", test_worldpos);
    run("world_texture_pixel_settings_and_paint", test_world_texture_pixel_settings_and_paint);
    run("playground_clipboard_view_can_paint", test_playground_clipboard_view_can_paint);
    run("axis_lock_uncrouch_anchor", test_axis_lock_uncrouch_anchor);
    run("floor_collision_settles", test_floor_collision_settles);
    run("box_face_normals", test_box_face_normals);
    run("player_dimensions", test_player_dimensions);
    run("player_crouch_tunnel_clearance", test_player_crouch_tunnel_clearance);
    run("player_wall_contact_is_stable", test_player_wall_contact_is_stable);
    run("player_floor_support_remains_at_furniture_side", test_player_floor_support_remains_at_furniture_side);
    run("player_airborne_sprint_speed_is_preserved", test_player_airborne_sprint_speed_is_preserved);
    run("player_stopped_jump_gains_air_control_speed", test_player_stopped_jump_gains_air_control_speed);
    run("player_variable_jump_heights", test_player_variable_jump_heights);
    run("player_ground_movement_has_inertia", test_player_ground_movement_has_inertia);
    run("player_box_contact_does_not_cross", test_player_box_contact_does_not_cross);
    run("player_box_front_contact_preserves_lateral_offset", test_player_box_front_contact_preserves_lateral_offset);
    run("player_uncrouches_while_touching_box_side", test_player_uncrouches_while_touching_box_side);
    run("player_corner_contact_has_no_lateral_teleport", test_player_corner_contact_has_no_lateral_teleport);
    run("player_box_corner_bisector_blocks", test_player_box_corner_bisector_blocks);
    run("player_jump_into_box_does_not_phase_or_fall", test_player_jump_into_box_does_not_phase_or_fall);
    run("player_slides_along_box_face", test_player_slides_along_box_face);
    run("player_near_ground_fall_does_not_snap_early", test_player_near_ground_fall_does_not_snap_early);
    run("player_crouch_stuck_step_jump_does_not_fall_through_floor", test_player_crouch_stuck_step_jump_does_not_fall_through_floor);
    run("player_slides_across_flush_wall_seam", test_player_slides_across_flush_wall_seam);
    run("player_jump_toward_tunnel_end_does_not_fall_through_floor", test_player_jump_toward_tunnel_end_does_not_fall_through_floor);
    run("player_standing_tunnel_approach_jump_does_not_fall_through_floor", test_player_standing_tunnel_approach_jump_does_not_fall_through_floor);
    run("player_cannot_stand_on_covered_tunnel_support", test_player_cannot_stand_on_covered_tunnel_support);
    run("player_rotated_ramp_side_blocks", test_player_rotated_ramp_side_blocks);
    run("player_low_ramp_side_can_step_up", test_player_low_ramp_side_can_step_up);
    run("player_ramp_edge_reaches_landing", test_player_ramp_edge_reaches_landing);
    run("player_outer_ramp_edge_reaches_landing", test_player_outer_ramp_edge_reaches_landing);
    run("player_ramp_landing_has_no_collision_bump", test_player_ramp_landing_has_no_collision_bump);
    run("player_ramp_ascent_projects_speed_without_top_dip", test_player_ramp_ascent_projects_speed_without_top_dip);
    run("player_yaw_slide_ramp_side_to_flush_landing", test_player_yaw_slide_ramp_side_to_flush_landing);
    run("player_low_ramp_side_camera_step_is_smoothed", test_player_low_ramp_side_camera_step_is_smoothed);
    run("player_crouch_transition_reverses_smoothly", test_player_crouch_transition_reverses_smoothly);
    run("player_air_crouch_brings_legs_up", test_player_air_crouch_brings_legs_up);
    run("player_cannot_be_squeezed_through_narrow_gap", test_player_cannot_be_squeezed_through_narrow_gap);
    run("player_auto_stands_after_tunnel_exit", test_player_auto_stands_after_tunnel_exit);
    run("player_jump_does_not_pass_through_tunnel_roof", test_player_jump_does_not_pass_through_tunnel_roof);
    run("player_tunnel_exit_uncrouch_does_not_pass_through_roof", test_player_tunnel_exit_uncrouch_does_not_pass_through_roof);
    run("player_crouched_tunnel_side_jump_matches_stand_clearance", test_player_crouched_tunnel_side_jump_matches_stand_clearance);
    run("player_tunnel_entrance_allows_side_slide", test_player_tunnel_entrance_allows_side_slide);
    run("player_yaw_slide_tunnel_entrance_to_flush_support", test_player_yaw_slide_tunnel_entrance_to_flush_support);
    run("player_uncrouches_on_tunnel_roof", test_player_uncrouches_on_tunnel_roof);
    run("player_rotated_box_ramp_transition", test_player_rotated_box_ramp_transition);
    run("player_sprint_jump_from_demo_stairs_to_ramp_does_not_fall_through", test_player_sprint_jump_from_demo_stairs_to_ramp_does_not_fall_through);
    run("hills_radius_32_streams_without_physics_overload", test_hills_radius_32_streams_without_physics_overload);
    run("hills_prefetches_with_bounded_chunk_commits", test_hills_prefetches_with_bounded_chunk_commits);
    run("realtime_lighting_defaults", test_realtime_lighting_defaults);
    run("pathtrace_comparison_toggle_state", test_pathtrace_comparison_toggle_state);
    run("playground_primary_surfaces_are_lit", test_playground_primary_surfaces_are_lit);
    run("cave_world_generation_and_materials", test_cave_world_generation_and_materials);
    run("cave_surface_mip_policy_has_no_distance_thresholds", test_cave_surface_mip_policy_has_no_distance_thresholds);
    run("hills_house_block_contact_keeps_floor_support", test_hills_house_block_contact_keeps_floor_support);
    if (ok) std::printf("physics tests passed\n");
    return ok;
}

ol::u32 add_visual_box(ol::Dimension* dim, ol::u32 dim_id, ol::u32 cube_id, const char* name, Vector3 center, Vector3 size, Color color, bool lit, bool draw_edges) {
    ol::MeshInstance mesh{};
    std::snprintf(mesh.name, sizeof(mesh.name), "%s", name);
    mesh.geometry = cube_id;
    mesh.origin = test_pos(dim_id, center, dim->chunk_size_m);
    mesh.se3 = MatrixScale(size.x, size.y, size.z);
    mesh.color = color;
    mesh.texture_id = ol::render_texture_grid;
    mesh.lit = lit;
    mesh.draw_edges = draw_edges;
    return ol::dimension_add_mesh(dim, mesh);
}

GLint texture_magnification_filter(Texture2D texture) {
    if (!IsTextureValid(texture)) return 0;
    GLint previous = 0;
    GLint filter = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &filter);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    return filter;
}

GLint texture_minification_filter(Texture2D texture) {
    if (!IsTextureValid(texture)) return 0;
    GLint previous = 0;
    GLint filter = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    return filter;
}

bool texture_magnifies_nearest(Texture2D texture) {
    return texture_magnification_filter(texture) == GL_NEAREST;
}

int brightest_texture_channel(Texture2D texture) {
    if (!IsTextureValid(texture)) return 0;
    Image image = LoadImageFromTexture(texture);
    if (!IsImageValid(image)) return 0;
    Color* pixels = LoadImageColors(image);
    int brightest = 0;
    for (int i = 0; pixels && i < image.width * image.height; ++i) {
        brightest = std::max(brightest, std::max(
            static_cast<int>(pixels[i].r),
            std::max(static_cast<int>(pixels[i].g), static_cast<int>(pixels[i].b))));
    }
    if (pixels) UnloadImageColors(pixels);
    UnloadImage(image);
    return brightest;
}

int red_dominant_texture_pixels(Texture2D texture) {
    if (!IsTextureValid(texture)) return 0;
    Image image = LoadImageFromTexture(texture);
    if (!IsImageValid(image)) return 0;
    Color* pixels = LoadImageColors(image);
    int count = 0;
    for (int i = 0; pixels && i < image.width * image.height; ++i) {
        const Color color = pixels[i];
        if (color.r > 70 && color.r > color.g + 20 && color.r > color.b + 12) ++count;
    }
    if (pixels) UnloadImageColors(pixels);
    UnloadImage(image);
    return count;
}

ol::u64 texture_color_hash(Texture2D texture) {
    if (!IsTextureValid(texture)) return 0;
    Image image = LoadImageFromTexture(texture);
    if (!IsImageValid(image)) return 0;
    Color* pixels = LoadImageColors(image);
    ol::u64 hash = 1469598103934665603ull;
    for (int i = 0; pixels && i < image.width * image.height; ++i) {
        hash ^= ColorToInt(pixels[i]);
        hash *= 1099511628211ull;
    }
    UnloadImageColors(pixels);
    UnloadImage(image);
    return hash;
}

ol::u64 texture_mip_chain_hash(Texture2D texture) {
    if (!IsTextureValid(texture)) return 0;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    ol::u64 hash = 1469598103934665603ull;
    const int levels = std::max(1, texture.mipmaps);
    for (int level = 0; level < levels; ++level) {
        GLint width = 0;
        GLint height = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &height);
        if (width <= 0 || height <= 0) break;
        std::vector<Color> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
        glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        hash ^= static_cast<ol::u64>(static_cast<ol::u32>(width));
        hash *= 1099511628211ull;
        hash ^= static_cast<ol::u64>(static_cast<ol::u32>(height));
        hash *= 1099511628211ull;
        for (Color color : pixels) {
            hash ^= ColorToInt(color);
            hash *= 1099511628211ull;
        }
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    return hash;
}

const ol::MeshLightingSurfaceCache* find_lighting_surface(
    const ol::RenderState& renderer,
    ol::u32 mesh_id,
    Vector3 normal) {
    for (const ol::MeshLightingSurfaceCache& surface : renderer.lighting_surfaces) {
        if (surface.mesh_id == mesh_id && Vector3DotProduct(surface.normal, normal) > 0.99f) return &surface;
    }
    return nullptr;
}

bool run_visual_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 480, "ol visual test");

    ol::RenderState renderer{};
    ol::renderer_init(&renderer);

    auto world = std::make_unique<ol::World>();
    ol::world_init(world.get());
    const ol::u32 dim_id = ol::world_add_dimension(world.get(), "visual", 16.0f, 64.0f);
    ol::Dimension* dim = ol::world_get_dimension(world.get(), dim_id);
    dim->render_radius_chunks = 4;
    dim->quality_render_radius_chunks = 2;
    dim->ambient = 0.45f;

    ol::MeshGeometry cube = ol::make_box_geometry("cube", {1.0f, 1.0f, 1.0f});
    for (ol::u32 i = 0; i < cube.edge_count; ++i) cube.edges[i].thickness_px = 3;
    const ol::u32 cube_id = ol::dimension_add_geometry(dim, cube);

    add_visual_box(dim, dim_id, cube_id, "floor", {0.0f, -0.55f, 0.0f}, {12.0f, 1.0f, 12.0f}, {110, 135, 120, 255}, false, false);
    add_visual_box(dim, dim_id, cube_id, "box", {0.0f, 0.7f, 0.0f}, {2.0f, 2.0f, 2.0f}, {120, 190, 140, 255}, true, true);
    ol::PaintedPixel painted{};
    painted.center = test_pos(dim_id, {0.03125f, 0.73125f, 1.002f}, dim->chunk_size_m);
    painted.normal = {0.0f, 0.0f, 1.0f};
    painted.tangent = {-1.0f, 0.0f, 0.0f};
    painted.color = MAGENTA;
    ol::dimension_paint_pixel(dim, painted);

    const ol::u32 emitter_id = add_visual_box(
        dim, dim_id, cube_id, "visual emitter", {2.0f, 4.0f, 3.0f},
        {1.0f, 1.0f, 1.0f}, {255, 235, 190, 255}, false, false);
    if (ol::MeshInstance* emitter = ol::arena_get(&dim->meshes, emitter_id)) {
        ol::MaterialLayer emission{};
        emission.has_emission = true;
        emission.emission_color = {255, 235, 190, 255};
        emission.emission = 7.0f;
        ol::material_stack_push(&emitter->materials, emission);
    }
    ol::dimension_add_player(dim, "remote", {240, 70, 90, 255}, test_pos(dim_id, {1.6f, 0.0f, 2.8f}, dim->chunk_size_m), false);
    ol::SpriteInstance painted_sprite{};
    painted_sprite.origin = test_pos(dim_id, {-2.2f, 1.4f, 1.0f}, dim->chunk_size_m);
    painted_sprite.texture_id = ol::render_texture_life;
    painted_sprite.billboard = ol::billboard_vertical;
    const ol::u32 painted_sprite_id = ol::dimension_add_sprite(dim, painted_sprite);
    ol::PaintedPixel sprite_pixel{};
    sprite_pixel.center = painted_sprite.origin;
    sprite_pixel.sprite_id = painted_sprite_id;
    sprite_pixel.sprite_pixel_x = 4;
    sprite_pixel.sprite_pixel_y = 4;
    sprite_pixel.color = MAGENTA;
    ol::dimension_paint_pixel(dim, sprite_pixel);

    ol::CameraView view{};
    view.anchor = test_pos(dim_id, {0.0f, 0.0f, 6.5f}, dim->chunk_size_m);
    view.eye_height = 1.4f;
    view.yaw = 0.0f;
    view.pitch = -0.05f;

    ol::renderer_ensure_target(&renderer);
    ol::renderer_draw_dimension(&renderer, dim, view, ol::invalid_id);

    Image img = LoadImageFromTexture(renderer.target.texture);
    ImageFlipVertical(&img);
    std::filesystem::create_directories("test-output");
    const char* out_path = "test-output/visual-smoke.png";
    const bool exported = ExportImage(img, out_path);

    Color* pixels = LoadImageColors(img);
    int non_sky = 0;
    int dark = 0;
    int box_edge_pixels = 0;
    int player_pixels = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const Color c = pixels[y * img.width + x];
            if (!(c.r > 80 && c.r < 230 && c.g > 120 && c.g < 245 && c.b > 150 && c.b < 255)) non_sky++;
            if (c.r < 35 && c.g < 35 && c.b < 35) dark++;
            if (x >= 240 && x <= 390 && y >= 180 && y <= 330 && c.r < 35 && c.g < 35 && c.b < 35) box_edge_pixels++;
            if (c.r > 190 && c.g > 35 && c.g < 130 && c.b > 45 && c.b < 150) player_pixels++;
        }
    }
    UnloadImageColors(pixels);
    UnloadImage(img);
    const bool player_in_edge_occlusion = renderer.debug_unbounded_scene_triangle_count >= 128;
    const bool sprite_paint_baked = !renderer.sprite_paint_textures.empty() &&
        IsTextureValid(renderer.sprite_paint_textures[0].texture);
    const bool mesh_paint_layer_baked = !renderer.mesh_paint_surfaces.empty() &&
        IsTextureValid(renderer.mesh_paint_surfaces[0].texture) && renderer.mesh_paint_surfaces[0].texture.mipmaps > 1;
    const bool world_textures_magnify_nearest =
        texture_magnifies_nearest(renderer.grid_texture) &&
        texture_magnifies_nearest(renderer.grass_texture) &&
        texture_magnifies_nearest(renderer.stone_texture) &&
        texture_magnifies_nearest(renderer.roof_texture);
    bool paint_textures_magnify_nearest = !renderer.sprite_paint_textures.empty() && !renderer.mesh_paint_surfaces.empty();
    for (const ol::SpritePaintTextureCache& cache : renderer.sprite_paint_textures) {
        paint_textures_magnify_nearest = paint_textures_magnify_nearest && texture_magnifies_nearest(cache.texture);
    }
    for (const ol::MeshPaintSurfaceCache& surface : renderer.mesh_paint_surfaces) {
        paint_textures_magnify_nearest = paint_textures_magnify_nearest && texture_magnifies_nearest(surface.texture);
    }
    bool lighting_textures_magnify_nearest = true;
    size_t lighting_base_target_count = 0;
    size_t lighting_history_target_count = 0;
    size_t lighting_shadow_mask_target_count = 0;
    for (const ol::MeshLightingSurfaceCache& surface : renderer.lighting_surfaces) {
        if (!IsRenderTextureValid(surface.texture)) continue;
        ++lighting_base_target_count;
        const GLint base_filter = texture_magnification_filter(surface.texture.texture);
        if (base_filter != GL_NEAREST) {
            std::printf(
                "lighting magnification mismatch signature=%llu target=base texture=%u filter=0x%x "
                "history=%u composite=%d\n",
                static_cast<unsigned long long>(surface.surface_signature),
                surface.texture.texture.id, static_cast<unsigned int>(base_filter),
                IsRenderTextureValid(surface.history_texture)
                    ? surface.history_texture.texture.id : 0u,
                surface.dynamic_composite_active ? 1 : 0);
            lighting_textures_magnify_nearest = false;
        }
        if (IsRenderTextureValid(surface.history_texture)) {
            ++lighting_history_target_count;
            const GLint history_filter = texture_magnification_filter(
                surface.history_texture.texture);
            if (history_filter != GL_NEAREST) {
                std::printf(
                    "lighting magnification mismatch signature=%llu target=history texture=%u filter=0x%x "
                    "base=%u composite=%d\n",
                    static_cast<unsigned long long>(surface.surface_signature),
                    surface.history_texture.texture.id,
                    static_cast<unsigned int>(history_filter),
                    surface.texture.texture.id,
                    surface.dynamic_composite_active ? 1 : 0);
                lighting_textures_magnify_nearest = false;
            }
        }
        if (IsRenderTextureValid(surface.shadow_mask_texture)) {
            ++lighting_shadow_mask_target_count;
            if (surface.shadow_mask_texture.texture.format !=
                    PIXELFORMAT_UNCOMPRESSED_GRAYSCALE &&
                surface.shadow_mask_texture.texture.format !=
                    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
                lighting_textures_magnify_nearest = false;
            }
            if (texture_minification_filter(surface.shadow_mask_texture.texture) != GL_LINEAR ||
                texture_magnification_filter(surface.shadow_mask_texture.texture) != GL_NEAREST) {
                lighting_textures_magnify_nearest = false;
            }
        }
    }
    if (lighting_base_target_count == 0) {
        lighting_textures_magnify_nearest = false;
        std::printf(
            "lighting magnification unavailable caches=%zu debug_surfaces=%u updates=%u allocations=%u "
            "scene=%llu neighbor_surfaces=%u neighbor_complete=%u promoted=%d\n",
            renderer.lighting_surfaces.size(), renderer.debug_radiance_surface_count,
            renderer.debug_radiance_surface_updates, renderer.debug_radiance_surface_allocations,
            static_cast<unsigned long long>(renderer.radiance_scene_signature),
            renderer.debug_radiance_neighbor_surface_count,
            renderer.debug_radiance_neighbor_complete_surface_count,
            renderer.debug_radiance_neighbor_promoted ? 1 : 0);
    } else {
        std::printf(
            "lighting magnification checked base_targets=%zu history_targets=%zu shadow_masks=%zu\n",
            lighting_base_target_count, lighting_history_target_count,
            lighting_shadow_mask_target_count);
    }
    const bool dynamic_shadow_uses_independent_mask =
        lighting_shadow_mask_target_count > 0 && lighting_history_target_count == 0;
    const bool radiance_shaders_ready = renderer.radiance_cascade_shader_ready &&
        renderer.surface_lighting_shader_ready &&
        renderer.surface_shadow_composite_shader_ready;
    const ol::u64 cascade_signature_before_paint = renderer.radiance_scene_signature;
    ol::PaintedPixel adjacent_paint = painted;
    adjacent_paint.center = test_pos(dim_id, {0.09375f, 0.73125f, 1.002f}, dim->chunk_size_m);
    adjacent_paint.color = YELLOW;
    ol::dimension_paint_pixel(dim, adjacent_paint);
    ol::renderer_draw_dimension(&renderer, dim, view, ol::invalid_id);
    const bool painting_keeps_probe_cache = cascade_signature_before_paint != 0 &&
        renderer.radiance_scene_signature == cascade_signature_before_paint;
    dim->pixels_per_meter = 8.0f;
    ol::renderer_draw_dimension(&renderer, dim, view, ol::invalid_id);
    bool lighting_tracks_world_ppm = false;
    bool paint_tracks_world_ppm = false;
    for (const ol::MeshLightingSurfaceCache& surface : renderer.lighting_surfaces) {
        const ol::MeshInstance* mesh = ol::arena_get(&dim->meshes, surface.mesh_id);
        if (mesh && std::strcmp(mesh->name, "box") == 0 && surface.width == 16 && surface.height == 16) {
            lighting_tracks_world_ppm = true;
            break;
        }
    }
    for (const ol::MeshPaintSurfaceCache& surface : renderer.mesh_paint_surfaces) {
        const ol::MeshInstance* mesh = ol::arena_get(&dim->meshes, surface.mesh_id);
        if (mesh && std::strcmp(mesh->name, "box") == 0 && surface.width == 16 && surface.height == 16) {
            paint_tracks_world_ppm = true;
            break;
        }
    }

    const ol::u32 sky_dim_id = ol::world_add_dimension(world.get(), "sky-only-lighting", 16.0f, 64.0f);
    ol::Dimension* sky_dim = ol::world_get_dimension(world.get(), sky_dim_id);
    ol::u32 sky_receiver_id = ol::invalid_id;
    ol::u32 mirror_id = ol::invalid_id;
    ol::u32 sky_local_player_id = ol::invalid_id;
    if (sky_dim) {
        sky_dim->render_radius_chunks = 2;
        sky_dim->quality_render_radius_chunks = 2;
        sky_dim->ambient = 0.0f;
        sky_dim->sky_top = {190, 225, 255, 255};
        sky_dim->sky_bottom = {125, 180, 235, 255};
        ol::MeshGeometry sky_cube = ol::make_box_geometry("sky-only cube", {1.0f, 1.0f, 1.0f});
        const ol::u32 sky_cube_id = ol::dimension_add_geometry(sky_dim, sky_cube);
        sky_receiver_id = add_visual_box(
            sky_dim, sky_dim_id, sky_cube_id, "sky receiver", {-1.5f, -0.10f, 0.0f},
            {2.0f, 0.20f, 2.0f}, {190, 190, 190, 255}, true, false);
        mirror_id = add_visual_box(
            sky_dim, sky_dim_id, sky_cube_id, "realtime mirror", {1.5f, -0.05f, 0.0f},
            {2.0f, 0.10f, 2.0f}, {150, 165, 180, 255}, true, false);
        if (ol::MeshInstance* mirror = ol::arena_get(&sky_dim->meshes, mirror_id)) {
            ol::MaterialLayer reflective{};
            reflective.has_reflectivity = true;
            reflective.reflectivity = 0.9f;
            ol::material_stack_push(&mirror->materials, reflective);
        }
        sky_local_player_id = ol::dimension_add_player(
            sky_dim, "local", {70, 170, 245, 255},
            test_pos(sky_dim_id, {-0.6f, 0.0f, -1.0f}, sky_dim->chunk_size_m), true);
        ol::dimension_add_player(
            sky_dim, "remote", {240, 80, 100, 255},
            test_pos(sky_dim_id, {0.6f, 0.0f, -1.0f}, sky_dim->chunk_size_m), false);
    }

    renderer.lighting.indirect_samples = 4;
    renderer.lighting.shadow_samples = 1;
    renderer.lighting.temporal_frames = 0;
    renderer.lighting.jitter = false;
    ol::CameraView sky_view{};
    sky_view.anchor = test_pos(sky_dim_id, {0.0f, 0.0f, 4.0f}, sky_dim ? sky_dim->chunk_size_m : 16.0f);
    sky_view.eye_height = 1.7f;
    sky_view.pitch = -0.20f;
    if (sky_dim) {
        for (int warmup = 0; warmup < 4; ++warmup) {
            ol::renderer_render_dimension_to_target(&renderer, sky_dim, sky_view, sky_local_player_id);
        }
    }
    const ol::MeshLightingSurfaceCache* first_mirror_surface =
        find_lighting_surface(renderer, mirror_id, {0.0f, 1.0f, 0.0f});
    const ol::MeshLightingSurfaceCache* sky_receiver_surface =
        find_lighting_surface(renderer, sky_receiver_id, {0.0f, 1.0f, 0.0f});
    const ol::u64 first_reflection_frame = renderer.radiance_frame;
    const ol::u64 first_reflection_camera_signature = first_mirror_surface
        ? first_mirror_surface->resolved_camera_signature : 0;
    const bool first_reflection_updated = first_mirror_surface &&
        first_mirror_surface->last_update_frame > 0 &&
        first_mirror_surface->temporal_samples == 1;
    const bool sky_only_lighting_active = sky_receiver_surface &&
        renderer.debug_radiance_emitter_count == 0 &&
        renderer.radiance_emitter_triangle_count == 0 &&
        renderer.radiance_scene_signature != 0 &&
        sky_receiver_surface->resolved_scene_signature == renderer.radiance_scene_signature &&
        brightest_texture_channel(sky_receiver_surface->texture.texture) > 16;
    const bool local_and_remote_players_trace = renderer.debug_radiance_player_primitive_count == 2 &&
        renderer.debug_radiance_dynamic_triangle_count == 2u * 32u * 4u &&
        renderer.debug_radiance_dynamic_bvh_node_count > 0;
    const bool player_visible_in_reflection = first_mirror_surface &&
        red_dominant_texture_pixels(first_mirror_surface->texture.texture) > 0;

    sky_view.anchor = test_pos(sky_dim_id, {0.0625f, 0.0f, 4.0f}, sky_dim ? sky_dim->chunk_size_m : 16.0f);
    if (sky_dim) {
        ol::renderer_render_dimension_to_target(&renderer, sky_dim, sky_view, sky_local_player_id);
    }
    const ol::MeshLightingSurfaceCache* second_mirror_surface =
        find_lighting_surface(renderer, mirror_id, {0.0f, 1.0f, 0.0f});
    const bool second_reflection_updated = second_mirror_surface &&
        renderer.radiance_frame == first_reflection_frame + 1 &&
        second_mirror_surface->last_update_frame == renderer.radiance_frame &&
        second_mirror_surface->resolved_camera_signature != first_reflection_camera_signature &&
        second_mirror_surface->temporal_samples == 1 &&
        renderer.lighting.temporal_frames == 0 &&
        renderer.radiance_cascade_temporal_samples == 1;
    const ol::MeshLightingSurfaceCache* receiver_before_player_move =
        find_lighting_surface(renderer, sky_receiver_id, {0.0f, 1.0f, 0.0f});
    const ol::u64 receiver_stable_frame_before_player_move = receiver_before_player_move
        ? receiver_before_player_move->last_update_frame : 0;
    const ol::u64 receiver_shadow_signature_before_player_move = receiver_before_player_move
        ? receiver_before_player_move->shadow_mask_signature : 0;
    const ol::u64 receiver_hash_before_player_move = receiver_before_player_move
        ? texture_color_hash(receiver_before_player_move->dynamic_composite_active &&
              IsRenderTextureValid(receiver_before_player_move->shadow_mask_texture)
              ? receiver_before_player_move->shadow_mask_texture.texture
              : receiver_before_player_move->texture.texture)
        : 0;
    const bool moved_local_player = sky_dim && translate_test_player(
        sky_dim, sky_local_player_id, {-0.5f, 0.0f, 0.0f});
    if (sky_dim) {
        ol::renderer_render_dimension_to_target(&renderer, sky_dim, sky_view, sky_local_player_id);
    }
    const ol::MeshLightingSurfaceCache* receiver_after_player_move =
        find_lighting_surface(renderer, sky_receiver_id, {0.0f, 1.0f, 0.0f});
    const ol::u64 receiver_hash_after_player_move = receiver_after_player_move
        ? texture_color_hash(receiver_after_player_move->dynamic_composite_active &&
              IsRenderTextureValid(receiver_after_player_move->shadow_mask_texture)
              ? receiver_after_player_move->shadow_mask_texture.texture
              : receiver_after_player_move->texture.texture)
        : 0;
    const bool moving_player_updates_shadow = moved_local_player && receiver_after_player_move &&
        renderer.debug_radiance_cascade_update_frame == renderer.radiance_frame &&
        receiver_after_player_move->shadow_mask_signature != 0 &&
        receiver_after_player_move->shadow_mask_signature !=
            receiver_shadow_signature_before_player_move &&
        receiver_after_player_move->last_update_frame ==
            receiver_stable_frame_before_player_move &&
        receiver_hash_after_player_move != receiver_hash_before_player_move;
    if (!moving_player_updates_shadow) {
        std::printf(
            "moving shadow debug: moved=%d frame=%llu cascade=%llu surface=%llu resolved=%llu player=%llu active=%d before=%llu after=%llu\n",
            moved_local_player ? 1 : 0,
            static_cast<unsigned long long>(renderer.radiance_frame),
            static_cast<unsigned long long>(renderer.debug_radiance_cascade_update_frame),
            static_cast<unsigned long long>(receiver_after_player_move ? receiver_after_player_move->last_update_frame : 0),
            static_cast<unsigned long long>(receiver_after_player_move ? receiver_after_player_move->shadow_mask_signature : 0),
            static_cast<unsigned long long>(renderer.radiance_player_signature),
            receiver_after_player_move && receiver_after_player_move->dynamic_composite_active ? 1 : 0,
            static_cast<unsigned long long>(receiver_hash_before_player_move),
            static_cast<unsigned long long>(receiver_hash_after_player_move));
    }
    if (second_mirror_surface) {
        const bool mirror_base_nearest =
            texture_magnifies_nearest(second_mirror_surface->texture.texture);
        const bool mirror_history_nearest =
            !IsRenderTextureValid(second_mirror_surface->history_texture) ||
            texture_magnifies_nearest(second_mirror_surface->history_texture.texture);
        if (!mirror_base_nearest || !mirror_history_nearest) {
            std::printf(
                "lighting magnification mismatch signature=%llu target=mirror base=%u filter=0x%x "
                "history=%u filter=0x%x composite=%d\n",
                static_cast<unsigned long long>(second_mirror_surface->surface_signature),
                second_mirror_surface->texture.texture.id,
                static_cast<unsigned int>(texture_magnification_filter(
                    second_mirror_surface->texture.texture)),
                IsRenderTextureValid(second_mirror_surface->history_texture)
                    ? second_mirror_surface->history_texture.texture.id : 0u,
                static_cast<unsigned int>(IsRenderTextureValid(second_mirror_surface->history_texture)
                    ? texture_magnification_filter(second_mirror_surface->history_texture.texture) : 0),
                second_mirror_surface->dynamic_composite_active ? 1 : 0);
        }
        lighting_textures_magnify_nearest = lighting_textures_magnify_nearest &&
            mirror_base_nearest && mirror_history_nearest;
    } else {
        lighting_textures_magnify_nearest = false;
    }

    auto cave_app = std::make_unique<ol::DemoApp>();
    std::snprintf(cave_app->world_name, sizeof(cave_app->world_name), "cave");
    std::snprintf(cave_app->session_name, sizeof(cave_app->session_name), "cave-lighting-visual");
    ol::demo_generate_world(cave_app.get());
    ol::Dimension* cave_dim = ol::world_get_dimension(&cave_app->world, cave_app->dimension_id);
    ol::PlayerEntity* cave_player = cave_dim ? ol::arena_get(&cave_dim->players, cave_app->local_player_id) : nullptr;
    renderer.lighting.indirect_samples = 4;
    renderer.lighting.shadow_samples = 1;
    renderer.lighting.temporal_frames = 0;
    if (cave_dim && cave_player) {
        ol::CameraView cave_view{};
        cave_view.anchor = ol::player_feet_pos(cave_dim, cave_player);
        cave_view.eye_height = cave_player->eye_height;
        cave_view.yaw = cave_player->yaw;
        cave_view.pitch = -0.08f;
        for (int frame = 0; frame < 14; ++frame) {
            ol::renderer_render_dimension_to_target(&renderer, cave_dim, cave_view, cave_app->local_player_id);
        }
    }
    Image cave_image = LoadImageFromTexture(renderer.target.texture);
    ImageFlipVertical(&cave_image);
    const bool cave_exported = ExportImage(cave_image, "test-output/cave-lighting-smoke.png");
    Color* cave_pixels = LoadImageColors(cave_image);
    int cave_dark = 0;
    int cave_colored_light = 0;
    for (int i = 0; cave_pixels && i < cave_image.width * cave_image.height; ++i) {
        const Color c = cave_pixels[i];
        if (c.r < 22 && c.g < 22 && c.b < 28) ++cave_dark;
        if ((c.r > c.g + 25 || c.b > c.r + 25) && (c.r > 90 || c.b > 90)) ++cave_colored_light;
    }
    if (cave_pixels) UnloadImageColors(cave_pixels);
    UnloadImage(cave_image);
    bool reflective_texels_resolved = false;
    bool cave_surface_uses_exact_ppm = false;
    bool cross_chunk_surfaces_resolved = false;
    ol::ChunkCoord first_resolved_chunk{};
    bool first_resolved_chunk_valid = false;
    for (const ol::MeshLightingSurfaceCache& surface : renderer.lighting_surfaces) {
        reflective_texels_resolved = reflective_texels_resolved ||
            (surface.reflectivity >= 0.7f && surface.resolved_scene_signature != 0 && IsRenderTextureValid(surface.texture));
        const ol::MeshInstance* mesh = cave_dim ? ol::arena_get(&cave_dim->meshes, surface.mesh_id) : nullptr;
        if (mesh && std::strncmp(mesh->name, "cave floor", 10) == 0 &&
            ((surface.width == 256 && surface.height == 256) || (surface.width == 256 && surface.height == 16) ||
             (surface.width == 16 && surface.height == 256))) {
            cave_surface_uses_exact_ppm = true;
        }
        if (mesh && surface.resolved_scene_signature != 0) {
            if (!first_resolved_chunk_valid) {
                first_resolved_chunk = mesh->origin.chunk;
                first_resolved_chunk_valid = true;
            } else if (mesh->origin.chunk.x != first_resolved_chunk.x || mesh->origin.chunk.z != first_resolved_chunk.z) {
                cross_chunk_surfaces_resolved = true;
            }
        }
    }
    const bool cave_radiance_active = renderer.debug_radiance_emitter_count == 3 &&
        renderer.debug_radiance_triangle_count > 20 && renderer.debug_radiance_bvh_node_count > 0 &&
        renderer.debug_radiance_surface_count > 20 && !renderer.debug_radiance_scene_truncated;
    bool lighting_is_screen_scale_independent = false;
    if (cave_dim && cave_player && !renderer.lighting_surfaces.empty()) {
        const Texture2D before = renderer.lighting_surfaces.front().texture.texture;
        renderer.scale_power = 2;
        ol::CameraView scaled_view{};
        scaled_view.anchor = ol::player_feet_pos(cave_dim, cave_player);
        scaled_view.eye_height = cave_player->eye_height;
        scaled_view.yaw = cave_player->yaw;
        scaled_view.pitch = -0.08f;
        ol::renderer_ensure_target(&renderer);
        ol::renderer_render_dimension_to_target(&renderer, cave_dim, scaled_view, cave_app->local_player_id);
        const Texture2D after = renderer.lighting_surfaces.front().texture.texture;
        lighting_is_screen_scale_independent = before.width == after.width && before.height == after.height;
    }

    bool pathtrace_shader_ready = renderer.pathtrace_comparison_shader_ready;
    bool cave_pathtrace_rendered = false;
    bool cave_pathtrace_stationary_exact = false;
    bool cave_pathtrace_complete_scene = false;
    bool playground_pathtrace_rendered = false;
    bool playground_pathtrace_stationary_exact = false;
    bool pathtrace_toggle_restores_rc = false;
    bool pathtrace_static_mutation_rebuilds = false;
    bool pathtrace_failure_falls_back_to_rc = false;
    ol::u32 cave_pathtrace_triangles = 0;
    ol::u32 cave_pathtrace_emitters = 0;
    ol::u32 cave_pathtrace_dynamic_triangles = 0;
    if (cave_dim && cave_player && pathtrace_shader_ready) {
        ol::CameraView cave_pt_view{};
        cave_pt_view.anchor = ol::player_feet_pos(cave_dim, cave_player);
        cave_pt_view.eye_height = cave_player->eye_height;
        cave_pt_view.yaw = cave_player->yaw;
        cave_pt_view.pitch = -0.08f;
        const ol::u64 rc_signature_before_pt = renderer.radiance_scene_signature;
        const bool toggled_on =
            ol::renderer_toggle_pathtrace_comparison(&renderer);
        const double cave_pt_start = GetTime();
        ol::renderer_render_dimension_to_target(
            &renderer, cave_dim, cave_pt_view, cave_app->local_player_id);
        const ol::u64 cave_hash_first = texture_color_hash(renderer.target.texture);
        const ol::u64 cave_builds_first = renderer.pathtrace_scene_build_count;
        cave_pathtrace_triangles = renderer.pathtrace_triangle_count;
        cave_pathtrace_emitters = renderer.pathtrace_emitter_mesh_count;
        cave_pathtrace_dynamic_triangles = renderer.pathtrace_dynamic_triangle_count;
        const double cave_pt_first_ms = (GetTime() - cave_pt_start) * 1000.0;
        Image cave_pt_image = LoadImageFromTexture(renderer.target.texture);
        ImageFlipVertical(&cave_pt_image);
        const bool cave_pt_exported = ExportImage(
            cave_pt_image, "test-output/cave-pathtrace-comparison.png");
        UnloadImage(cave_pt_image);
        ol::renderer_render_dimension_to_target(
            &renderer, cave_dim, cave_pt_view, cave_app->local_player_id);
        const ol::u64 cave_hash_second = texture_color_hash(renderer.target.texture);
        cave_pathtrace_stationary_exact = cave_hash_first != 0 &&
            cave_hash_first == cave_hash_second &&
            renderer.pathtrace_scene_build_count == cave_builds_first;
        cave_pathtrace_complete_scene = renderer.pathtrace_triangle_count > 20 &&
            renderer.pathtrace_bvh_node_count > 0 &&
            renderer.pathtrace_emitter_mesh_count == 3 &&
            renderer.pathtrace_dynamic_triangle_count >= 128 &&
            !renderer.pathtrace_scene_truncated;
        cave_pathtrace_rendered = toggled_on && cave_pt_exported &&
            IsRenderTextureValid(renderer.pathtrace_comparison_target);

        ol::renderer_toggle_pathtrace_comparison(&renderer);
        const ol::u64 rc_frame_before_restore = renderer.radiance_frame;
        ol::renderer_render_dimension_to_target(
            &renderer, cave_dim, cave_pt_view, cave_app->local_player_id);
        pathtrace_toggle_restores_rc = !renderer.pathtrace_comparison_enabled &&
            renderer.radiance_frame == rc_frame_before_restore + 1 &&
            renderer.radiance_scene_signature == rc_signature_before_pt;

        const bool playground_toggled_on =
            ol::renderer_toggle_pathtrace_comparison(&renderer);
        const double playground_pt_start = GetTime();
        ol::renderer_render_dimension_to_target(
            &renderer, dim, view, ol::invalid_id);
        const ol::u64 playground_hash_first =
            texture_color_hash(renderer.target.texture);
        const ol::u64 playground_builds_first = renderer.pathtrace_scene_build_count;
        const double playground_pt_first_ms =
            (GetTime() - playground_pt_start) * 1000.0;
        Image playground_pt_image = LoadImageFromTexture(renderer.target.texture);
        ImageFlipVertical(&playground_pt_image);
        const bool playground_pt_exported = ExportImage(
            playground_pt_image, "test-output/playground-pathtrace-comparison.png");
        UnloadImage(playground_pt_image);
        ol::renderer_render_dimension_to_target(
            &renderer, dim, view, ol::invalid_id);
        const ol::u64 playground_hash_second =
            texture_color_hash(renderer.target.texture);
        playground_pathtrace_stationary_exact = playground_hash_first != 0 &&
            playground_hash_first == playground_hash_second &&
            renderer.pathtrace_scene_build_count == playground_builds_first;
        playground_pathtrace_rendered = playground_toggled_on &&
            playground_pt_exported && renderer.pathtrace_triangle_count > 0 &&
            renderer.pathtrace_bvh_node_count > 0 &&
            !renderer.pathtrace_scene_truncated;

        for (ol::u32 mesh_slot = 0; mesh_slot < dim->meshes.count; ++mesh_slot) {
            ol::MeshInstance& mesh = dim->meshes.data[mesh_slot];
            if (std::strcmp(mesh.name, "box") != 0) continue;
            const Color original_color = mesh.color;
            const ol::u64 original_topology = dim->mesh_topology_revision;
            const ol::u64 original_signature = renderer.pathtrace_scene_signature;
            const ol::u64 builds_before_material_change =
                renderer.pathtrace_scene_build_count;
            mesh.color = YELLOW;
            ol::renderer_render_dimension_to_target(
                &renderer, dim, view, ol::invalid_id);
            pathtrace_static_mutation_rebuilds =
                dim->mesh_topology_revision == original_topology &&
                renderer.pathtrace_scene_signature != original_signature &&
                renderer.pathtrace_scene_build_count ==
                    builds_before_material_change + 1;
            mesh.color = original_color;
            break;
        }
        ol::renderer_toggle_pathtrace_comparison(&renderer);
        const ol::u64 fallback_frame = renderer.radiance_frame;
        renderer.pathtrace_comparison_enabled = true;
        renderer.pathtrace_comparison_shader_ready = false;
        ol::renderer_render_dimension_to_target(
            &renderer, dim, view, ol::invalid_id);
        pathtrace_failure_falls_back_to_rc =
            !renderer.pathtrace_comparison_enabled &&
            renderer.pathtrace_comparison_failed &&
            renderer.radiance_frame == fallback_frame + 1;
        renderer.pathtrace_comparison_shader_ready = true;
        renderer.pathtrace_comparison_failed = false;
        std::printf(
            "pathtrace comparison: cave=%.2fms tris=%u emitters=%u dynamic=%u "
            "playground=%.2fms tris=%u builds=%llu exact=%d/%d\n",
            cave_pt_first_ms, cave_pathtrace_triangles,
            cave_pathtrace_emitters,
            cave_pathtrace_dynamic_triangles,
            playground_pt_first_ms, renderer.pathtrace_triangle_count,
            static_cast<unsigned long long>(renderer.pathtrace_scene_build_count),
            cave_pathtrace_stationary_exact ? 1 : 0,
            playground_pathtrace_stationary_exact ? 1 : 0);
    }
    ol::renderer_shutdown(&renderer);

    bool transparent_sprite_ray_passes_through = false;
    auto paint_app = std::make_unique<ol::DemoApp>();
    ol::renderer_init(&paint_app->renderer);
    std::snprintf(paint_app->world_name, sizeof(paint_app->world_name), "playground");
    std::snprintf(paint_app->session_name, sizeof(paint_app->session_name), "transparent-ray-test");
    ol::demo_generate_world(paint_app.get());
    ol::Dimension* paint_dim = ol::world_get_dimension(&paint_app->world, paint_app->dimension_id);
    Image life_image = LoadImageFromTexture(paint_app->renderer.life_texture);
    int transparent_x = -1;
    int transparent_y = -1;
    for (int y = 0; IsImageValid(life_image) && y < life_image.height && transparent_x < 0; ++y) {
        for (int x = 0; x < life_image.width; ++x) {
            if (GetImageColor(life_image, x, y).a < 128) {
                transparent_x = x;
                transparent_y = y;
                break;
            }
        }
    }
    if (paint_dim && transparent_x >= 0) {
        ol::SpriteInstance back{};
        back.origin = test_pos(paint_app->dimension_id, {-3.0f, 1.6f, -8.0f}, paint_dim->chunk_size_m);
        back.texture_id = ol::render_texture_grid;
        back.billboard = ol::billboard_vertical;
        back.size = {3.0f, 3.0f};
        const ol::u32 back_id = ol::dimension_add_sprite(paint_dim, back);
        const float sprite_w = static_cast<float>(life_image.width) / paint_dim->pixels_per_meter * 1.2f;
        const float sprite_h = static_cast<float>(life_image.height) / paint_dim->pixels_per_meter * 1.2f;
        const float target_x = -3.0f + (static_cast<float>(transparent_x) + 0.5f) / static_cast<float>(life_image.width) * sprite_w - sprite_w * 0.5f;
        const float target_y = 1.6f + sprite_h * 0.5f - (static_cast<float>(transparent_y) + 0.5f) / static_cast<float>(life_image.height) * sprite_h;
        ol::CameraView alpha_view{};
        alpha_view.anchor = test_pos(paint_app->dimension_id, {target_x, target_y - 1.7f, -6.0f}, paint_dim->chunk_size_m);
        alpha_view.eye_height = 1.7f;
        alpha_view.yaw = 0.0f;
        alpha_view.pitch = 0.0f;
        ol::demo_paint_at_view(paint_app.get(), alpha_view, RED);
        transparent_sprite_ray_passes_through = !paint_dim->painted_pixels.empty() && paint_dim->painted_pixels.back().sprite_id == back_id;
    }
    if (IsImageValid(life_image)) UnloadImage(life_image);
    ol::renderer_shutdown(&paint_app->renderer);
    CloseWindow();

    const bool ok = expect_true(exported, "Visual smoke image exported") &&
                    expect_true(non_sky > 2000, "Visual smoke image contains scene pixels") &&
                    expect_true(dark > 20, "Visual smoke image contains solid dark edge pixels") &&
                    expect_true(box_edge_pixels > 20, "Visual smoke image contains box contour edge pixels") &&
                    expect_true(player_pixels > 50, "Visual smoke image contains a rendered remote player") &&
                    expect_true(player_in_edge_occlusion, "Remote player geometry participates in edge occlusion") &&
                    expect_true(sprite_paint_baked, "Sprite paint is baked into a cached texture instead of decal geometry") &&
                    expect_true(mesh_paint_layer_baked, "Mesh paint is assembled into a mipmapped face texture layer") &&
                    expect_true(world_textures_magnify_nearest, "World textures keep nearest-neighbor magnification") &&
                    expect_true(paint_textures_magnify_nearest, "Paint textures keep nearest-neighbor magnification") &&
                    expect_true(lighting_textures_magnify_nearest, "Surface-lighting textures keep nearest-neighbor magnification") &&
                    expect_true(dynamic_shadow_uses_independent_mask,
                        "Player shadows use a persistent mask without consuming temporal history") &&
                    expect_true(radiance_shaders_ready, "Radiance cascade and surface-texel shaders loaded") &&
                    expect_true(painting_keeps_probe_cache, "Painting invalidates surface lighting without rebuilding shared probes") &&
                    expect_true(lighting_tracks_world_ppm, "Changing world pixels-per-meter rebuilds exact surface lighting textures") &&
                    expect_true(paint_tracks_world_ppm, "Changing world pixels-per-meter rebuilds matching paint textures") &&
                    expect_true(sky_only_lighting_active, "Infinite sky lights surface texels without finite emissive meshes") &&
                    expect_true(local_and_remote_players_trace, "Path-traced dynamic BVH includes local and remote player meshes") &&
                    expect_true(player_visible_in_reflection, "Remote player mesh is visible in a traced reflection") &&
                    expect_true(first_reflection_updated && second_reflection_updated,
                        "Visible reflections update on consecutive sub-quarter-meter camera frames without temporal accumulation") &&
                    expect_true(moving_player_updates_shadow,
                        "Every-frame cascades and receiver texels update a moving player shadow immediately") &&
                    expect_true(cave_exported, "Cave lighting smoke image exported") &&
                    expect_true(cave_dark > 5000, "Cave image retains dark shadowed regions") &&
                    expect_true(cave_colored_light > 100, "Cave image contains colored emissive illumination") &&
                    expect_true(cave_radiance_active, "Cave builds emissive geometry, shared probes, and surface texel caches") &&
                    expect_true(cave_surface_uses_exact_ppm, "Cave lighting textures preserve 16 world texels per meter") &&
                    expect_true(lighting_is_screen_scale_independent, "Screen render scale does not change surface lighting texel resolution") &&
                    expect_true(cross_chunk_surfaces_resolved, "One world-anchored probe field resolves adjacent cave chunks") &&
                    expect_true(reflective_texels_resolved, "Reflective cave faces resolve view-dependent world-texel textures") &&
                    expect_true(pathtrace_shader_ready, "Full-frame path-trace comparison shader loaded") &&
                    expect_true(cave_pathtrace_rendered, "Cave full-frame path-trace comparison exported") &&
                    expect_true(cave_pathtrace_complete_scene, "Cave path trace includes complete visual-radius meshes, emitters, and players") &&
                    expect_true(cave_pathtrace_stationary_exact, "Stationary cave path trace is deterministic and does not rebuild its static scene") &&
                    expect_true(playground_pathtrace_rendered, "Playground full-frame path-trace comparison exported") &&
                    expect_true(playground_pathtrace_stationary_exact, "Stationary playground path trace is deterministic") &&
                    expect_true(pathtrace_toggle_restores_rc, "Disabling path-trace comparison resumes the preserved RC cache") &&
                    expect_true(pathtrace_static_mutation_rebuilds, "Path-trace static identity catches material changes without topology changes") &&
                    expect_true(pathtrace_failure_falls_back_to_rc, "Path-trace setup failure disables the mode and renders the normal RC frame") &&
                    expect_true(transparent_sprite_ray_passes_through, "Transparent sprite pixels let paint rays pass through");
    if (ok) std::printf("visual test passed: %s\n", out_path);
    return ok;
}

struct EdgeCompareView {
    const char* name = "";
    Vector3 eye = {0.0f, 0.0f, 0.0f};
    Vector3 target = {0.0f, 0.0f, 0.0f};
};

struct HillsDiagnosticView {
    EdgeCompareView view{};
    Vector3 feet = {0.0f, 0.0f, 0.0f};
};

ol::CameraView make_edge_compare_camera(ol::u32 dim_id, const ol::Dimension* dim, const EdgeCompareView& src) {
    ol::CameraView view{};
    view.anchor = test_pos(dim_id, {src.eye.x, 0.0f, src.eye.z}, dim->chunk_size_m);
    view.eye_height = src.eye.y;
    const Vector3 to_target = src.target - src.eye;
    const float len = ol::safe_len(to_target);
    const Vector3 dir = len > 0.001f ? to_target / len : Vector3{0.0f, 0.0f, -1.0f};
    view.yaw = std::atan2(dir.x, -dir.z);
    view.pitch = std::asin(ol::clampf(dir.y, -1.0f, 1.0f));
    return view;
}

Image capture_render_target(ol::RenderState* renderer) {
    Image img = LoadImageFromTexture(renderer->target.texture);
    ImageFlipVertical(&img);
    return img;
}

bool export_captured_image(Image img, const char* out_path) {
    return ExportImage(img, out_path);
}

bool export_render_target(ol::RenderState* renderer, const char* out_path) {
    Image img = capture_render_target(renderer);
    const bool exported = ExportImage(img, out_path);
    UnloadImage(img);
    return exported;
}

struct ImageDiffStats {
    int width = 0;
    int height = 0;
    ol::u64 changed_pixels = 0;
    ol::u64 edge_disagreement_pixels = 0;
    ol::u64 a_edge_only_pixels = 0;
    ol::u64 b_edge_only_pixels = 0;
    double mean_abs_rgb = 0.0;
    int max_delta = 0;
};

ImageDiffStats compare_images(const Image& a, const Image& b, int channel_threshold) {
    ImageDiffStats stats{};
    stats.width = a.width;
    stats.height = a.height;
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0) {
        stats.changed_pixels = static_cast<ol::u64>(a.width > 0 && a.height > 0 ? a.width * a.height : 1);
        stats.edge_disagreement_pixels = stats.changed_pixels;
        stats.max_delta = 255;
        stats.mean_abs_rgb = 255.0;
        return stats;
    }

    Color* a_pixels = LoadImageColors(a);
    Color* b_pixels = LoadImageColors(b);
    ol::u64 total_abs = 0;
    const int total_pixels = a.width * a.height;
    for (int i = 0; i < total_pixels; ++i) {
        const Color ca = a_pixels[i];
        const Color cb = b_pixels[i];
        const int dr = std::abs(static_cast<int>(ca.r) - static_cast<int>(cb.r));
        const int dg = std::abs(static_cast<int>(ca.g) - static_cast<int>(cb.g));
        const int db = std::abs(static_cast<int>(ca.b) - static_cast<int>(cb.b));
        const int max_delta = std::max(dr, std::max(dg, db));
        stats.max_delta = std::max(stats.max_delta, max_delta);
        total_abs += static_cast<ol::u64>(dr + dg + db);
        if (max_delta > channel_threshold) ++stats.changed_pixels;

        const bool a_edge = ca.r < 96 && ca.g < 96 && ca.b < 96;
        const bool b_edge = cb.r < 96 && cb.g < 96 && cb.b < 96;
        if (a_edge != b_edge) {
            ++stats.edge_disagreement_pixels;
            if (a_edge) ++stats.a_edge_only_pixels;
            else ++stats.b_edge_only_pixels;
        }
    }
    UnloadImageColors(a_pixels);
    UnloadImageColors(b_pixels);

    stats.mean_abs_rgb = static_cast<double>(total_abs) / static_cast<double>(total_pixels * 3);
    return stats;
}

bool run_lighting_roundtrip_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(960, 540, "ol lighting roundtrip");

    ol::RenderState renderer{};
    ol::renderer_init(&renderer);
    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "playground");
    std::snprintf(app->session_name, sizeof(app->session_name), "lighting-roundtrip");
    ol::demo_generate_world(app.get());
    ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);
    bool ok = expect_true(dim != nullptr, "Lighting roundtrip generated playground");
    if (!dim) {
        ol::renderer_shutdown(&renderer);
        CloseWindow();
        return false;
    }

    const EdgeCompareView start_def = {
        "lighting-roundtrip-start", {0.0f, 1.7f, 8.0f}, {0.0f, 1.7f, 0.0f}};
    const EdgeCompareView away_def = {
        "lighting-roundtrip-away",
        {0.0419f, 1.7000f, -1.4644f},
        {-0.0989f, 1.7176f, -9.4632f}};
    const ol::CameraView start_view = make_edge_compare_camera(app->dimension_id, dim, start_def);
    const ol::CameraView away_view = make_edge_compare_camera(app->dimension_id, dim, away_def);
    const ol::WorldPos start_feet = test_pos(app->dimension_id, {0.0f, 0.0f, 8.0f}, dim->chunk_size_m);
    const ol::WorldPos away_feet = test_pos(app->dimension_id, {0.0419f, 0.0f, -1.4644f}, dim->chunk_size_m);
    move_test_player_to(dim, app->local_player_id, start_feet);
    ol::renderer_ensure_target(&renderer);
    for (int frame = 0; frame < 48; ++frame) {
        ol::renderer_render_dimension_to_target(&renderer, dim, start_view, app->local_player_id);
    }
    Image before = capture_render_target(&renderer);

    for (int step = 1; step <= 24; ++step) {
        const float t = static_cast<float>(step) / 24.0f;
        EdgeCompareView travel = start_def;
        travel.eye = Vector3Lerp(start_def.eye, away_def.eye, t);
        travel.target = Vector3Lerp(start_def.target, away_def.target, t);
        const Vector3 feet = Vector3Lerp({0.0f, 0.0f, 8.0f}, {0.0419f, 0.0f, -1.4644f}, t);
        move_test_player_to(dim, app->local_player_id, test_pos(app->dimension_id, feet, dim->chunk_size_m));
        const ol::CameraView travel_view = make_edge_compare_camera(app->dimension_id, dim, travel);
        ol::renderer_render_dimension_to_target(&renderer, dim, travel_view, app->local_player_id);
    }
    move_test_player_to(dim, app->local_player_id, away_feet);
    for (int frame = 0; frame < 8; ++frame) {
        ol::renderer_render_dimension_to_target(&renderer, dim, away_view, app->local_player_id);
    }
    for (int step = 23; step >= 0; --step) {
        const float t = static_cast<float>(step) / 24.0f;
        EdgeCompareView travel = start_def;
        travel.eye = Vector3Lerp(start_def.eye, away_def.eye, t);
        travel.target = Vector3Lerp(start_def.target, away_def.target, t);
        const Vector3 feet = Vector3Lerp({0.0f, 0.0f, 8.0f}, {0.0419f, 0.0f, -1.4644f}, t);
        move_test_player_to(dim, app->local_player_id, test_pos(app->dimension_id, feet, dim->chunk_size_m));
        const ol::CameraView travel_view = make_edge_compare_camera(app->dimension_id, dim, travel);
        ol::renderer_render_dimension_to_target(&renderer, dim, travel_view, app->local_player_id);
    }
    move_test_player_to(dim, app->local_player_id, start_feet);
    for (int frame = 0; frame < 8; ++frame) {
        ol::renderer_render_dimension_to_target(&renderer, dim, start_view, app->local_player_id);
    }
    Image after = capture_render_target(&renderer);
    const ImageDiffStats diff = compare_images(before, after, 8);
    std::filesystem::create_directories("test-output");
    export_captured_image(before, "test-output/lighting-roundtrip-before.png");
    export_captured_image(after, "test-output/lighting-roundtrip-after.png");
    std::printf(
        "lighting roundtrip: changed=%llu mean_abs=%.4f max_delta=%d surfaces=%u updates=%u\n",
        static_cast<unsigned long long>(diff.changed_pixels), diff.mean_abs_rgb, diff.max_delta,
        renderer.debug_radiance_surface_count, renderer.debug_radiance_surface_updates);
    ok = expect_true(diff.changed_pixels <= 64 && diff.mean_abs_rgb < 0.01 && diff.max_delta <= 32,
        "Returning to the exact playground start reproduces identical warmed lighting") && ok;
    UnloadImage(before);
    UnloadImage(after);
    ol::renderer_shutdown(&renderer);
    CloseWindow();
    return ok;
}

struct LightingSurfaceSnapshot {
    ol::u64 key = 0;
    ol::u32 mesh_id = ol::invalid_id;
    ol::u64 stable_texture_hash = 0;
    ol::u64 displayed_texture_hash = 0;
    ol::u64 shadow_mask_signature = 0;
    ol::u64 shadow_mask_hash = 0;
    int width = 0;
    int height = 0;
    int stable_mipmaps = 0;
    int displayed_mipmaps = 0;
    unsigned int displayed_texture_id = 0;
    int min_filter = 0;
    int mag_filter = 0;
    int wrap_s = 0;
    int wrap_t = 0;
    float anisotropy = 0.0f;
    int valid_x = 0;
    int valid_y = 0;
    int valid_width = 0;
    int valid_height = 0;
    int valid_mip_level = 30;
    bool fully_initialized = false;
    bool full_current_signature_coverage = false;
    bool shadow_mask_valid = false;
    bool dynamic_composite_active = false;
    bool visible_at_checkpoint = false;
    bool current = false;
};

void snapshot_texture_sampling(
    Texture2D texture,
    int* min_filter,
    int* mag_filter,
    int* wrap_s,
    int* wrap_t,
    float* anisotropy) {
    if (min_filter) *min_filter = 0;
    if (mag_filter) *mag_filter = 0;
    if (wrap_s) *wrap_s = 0;
    if (wrap_t) *wrap_t = 0;
    if (anisotropy) *anisotropy = 0.0f;
    if (!IsTextureValid(texture)) return;
    glBindTexture(GL_TEXTURE_2D, texture.id);
    if (min_filter) glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    if (mag_filter) glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
    if (wrap_s) glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    if (wrap_t) glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
    if (anisotropy) {
        glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

struct LightingCheckpoint {
    const char* name = "";
    Vector3 feet{};
    float yaw = 0.0f;
    float pitch = -0.08f;
};

struct LightingChunkOrderEntry {
    ol::u32 id = ol::invalid_id;
    ol::ChunkCoord coord{};
    ol::u32 mesh_count = 0;
};

struct LightingSurfaceOrderEntry {
    ol::u64 key = 0;
    ol::u32 mesh_id = ol::invalid_id;
};

struct LightingRenderStateSnapshot {
    ol::WorldPos camera_anchor{};
    std::vector<LightingChunkOrderEntry> active_chunks{};
    std::vector<LightingSurfaceOrderEntry> surface_order{};
    std::vector<LightingSurfaceOrderEntry> visible_surface_order{};
    ol::u64 chunk_order_hash = 0;
    ol::u64 edge_input_order_hash = 0;
    ol::u64 lighting_surface_order_hash = 0;
    ol::u64 visible_lighting_surface_order_hash = 0;
    ol::u64 radiance_scene_signature = 0;
    ol::u64 radiance_topology_signature = 0;
    ol::u64 radiance_settings_signature = 0;
    ol::u64 radiance_player_signature = 0;
    ol::u32 debug_edge_count = 0;
    ol::u32 debug_scene_triangle_count = 0;
    int native_w = 0;
    int native_h = 0;
    int window_w = 0;
    int window_h = 0;
    unsigned int target_fbo = 0;
    unsigned int target_texture = 0;
    unsigned int target_depth = 0;
    int target_width = 0;
    int target_height = 0;
    int target_format = 0;
    bool lighting_enabled = false;
    bool depth_test_edges = false;
    bool shader_depth_edges = false;
    bool gpu_depth_edges = false;
};

struct LightingCheckpointReference {
    LightingCheckpoint checkpoint{};
    Image image{};
    std::vector<LightingSurfaceSnapshot> surfaces{};
    LightingRenderStateSnapshot render_state{};
};

struct PlaygroundLightingHarness {
    ol::RenderState renderer{};
    std::unique_ptr<ol::DemoApp> app{};
    ol::Dimension* dim = nullptr;
};

struct PlaygroundLightingConfig {
    bool lighting_enabled = true;
    bool depth_test_edges = true;
    bool shader_depth_edges = true;
    bool gpu_depth_edges = true;
};

bool init_playground_lighting_harness(
    PlaygroundLightingHarness* harness,
    const char* session,
    const PlaygroundLightingConfig& config = {}) {
    if (!harness) return false;
    ol::renderer_init(&harness->renderer);
    harness->app = std::make_unique<ol::DemoApp>();
    std::snprintf(harness->app->world_name, sizeof(harness->app->world_name), "playground");
    std::snprintf(harness->app->session_name, sizeof(harness->app->session_name), "%s", session);
    ol::demo_generate_world(harness->app.get());
    harness->dim = ol::world_get_dimension(&harness->app->world, harness->app->dimension_id);
    if (!harness->dim) return false;
    harness->renderer.lighting.temporal_frames = 0;
    harness->renderer.lighting.jitter = false;
    harness->renderer.lighting.corner_merge = false;
    harness->renderer.lighting.enabled = config.lighting_enabled;
    harness->renderer.depth_test_edges = config.depth_test_edges;
    harness->renderer.shader_depth_edges = config.shader_depth_edges;
    harness->renderer.gpu_depth_edges = config.gpu_depth_edges;
    ol::renderer_ensure_target(&harness->renderer);
    return true;
}

void shutdown_playground_lighting_harness(PlaygroundLightingHarness* harness) {
    if (!harness) return;
    ol::renderer_shutdown(&harness->renderer);
    harness->app.reset();
    harness->dim = nullptr;
}

ol::CameraView lighting_checkpoint_view(
    const PlaygroundLightingHarness& harness,
    Vector3 feet,
    float yaw,
    float pitch) {
    ol::CameraView view{};
    view.anchor = test_pos(
        harness.app->dimension_id, {feet.x, feet.y, feet.z}, harness.dim->chunk_size_m);
    view.eye_height = 1.7f;
    view.yaw = yaw;
    view.pitch = pitch;
    return view;
}

void render_lighting_checkpoint(
    PlaygroundLightingHarness* harness,
    Vector3 feet,
    float yaw,
    float pitch) {
    const ol::CameraView view = lighting_checkpoint_view(*harness, feet, yaw, pitch);
    ol::renderer_render_dimension_to_target(
        &harness->renderer, harness->dim, view, harness->app->local_player_id);
}

void place_lighting_test_player(PlaygroundLightingHarness* harness, Vector3 feet) {
    move_test_player_to(
        harness->dim,
        harness->app->local_player_id,
        test_pos(harness->app->dimension_id, feet, harness->dim->chunk_size_m));
}

void warm_lighting_checkpoint(PlaygroundLightingHarness* harness, const LightingCheckpoint& checkpoint) {
    place_lighting_test_player(harness, checkpoint.feet);
    // A fixed panoramic warm sequence makes the fresh and travelled caches expose the
    // same interior faces. It deliberately does not clear route history.
    constexpr int direction_count = 8;
    for (int pass = 0; pass < 2; ++pass) {
        const float pitch = pass == 0 ? -0.24f : 0.10f;
        for (int direction = 0; direction < direction_count; ++direction) {
            const float yaw = static_cast<float>(direction) * 2.0f * PI /
                static_cast<float>(direction_count);
            for (int frame = 0; frame < 2; ++frame) {
                render_lighting_checkpoint(harness, checkpoint.feet, yaw, pitch);
            }
        }
    }
    for (int frame = 0; frame < 4; ++frame) {
        render_lighting_checkpoint(
            harness, checkpoint.feet, checkpoint.yaw, checkpoint.pitch);
    }
}

bool lighting_surface_visible_at_checkpoint(
    const PlaygroundLightingHarness& harness,
    const ol::MeshLightingSurfaceCache& surface,
    const LightingCheckpoint& checkpoint) {
    const ol::MeshInstance* mesh = ol::arena_get(&harness.dim->meshes, surface.mesh_id);
    if (!mesh) return false;
    const ol::MeshGeometry* geometry = ol::arena_get(&harness.dim->geometries, mesh->geometry);
    if (!geometry) return false;
    const ol::WorldPos view_anchor = test_pos(
        harness.app->dimension_id, checkpoint.feet, harness.dim->chunk_size_m);
    const Vector3 mesh_origin = ol::world_delta_meters(
        mesh->origin, view_anchor, harness.dim->chunk_size_m);
    const Vector3 camera_position = {0.0f, 1.7f, 0.0f};
    const Vector3 camera_forward = ol::forward_from_angles(checkpoint.yaw, checkpoint.pitch);
    const Vector3 point_on_face = mesh_origin + surface.normal * surface.plane;
    if (Vector3DotProduct(surface.normal, camera_position - point_on_face) <= 0.0f) return false;

    Camera3D camera{};
    camera.position = camera_position;
    camera.target = camera.position + camera_forward;
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = harness.renderer.fov;
    camera.projection = CAMERA_PERSPECTIVE;
    const Matrix view_matrix = MatrixLookAt(camera.position, camera.target, camera.up);
    const float aspect = static_cast<float>(std::max(1, harness.renderer.native_w)) /
        static_cast<float>(std::max(1, harness.renderer.native_h));
    const float near_plane = rlGetCullDistanceNear();
    const Matrix projection_matrix = MatrixPerspective(
        harness.renderer.fov * DEG2RAD,
        static_cast<double>(aspect), near_plane, rlGetCullDistanceFar());
    Matrix basis = mesh->se3;
    basis.m12 = 0.0f;
    basis.m13 = 0.0f;
    basis.m14 = 0.0f;

    auto triangle_intersects_viewport = [&](Vector3 a, Vector3 b, Vector3 c) {
        const Vector3 input[3] = {a, b, c};
        const float distance[3] = {
            Vector3DotProduct(a - camera.position, camera_forward) - near_plane,
            Vector3DotProduct(b - camera.position, camera_forward) - near_plane,
            Vector3DotProduct(c - camera.position, camera_forward) - near_plane};
        Vector3 clipped[4]{};
        int clipped_count = 0;
        for (int index = 0; index < 3; ++index) {
            const int next = (index + 1) % 3;
            const bool current_in = distance[index] >= 0.0f;
            const bool next_in = distance[next] >= 0.0f;
            if (current_in && clipped_count < 4) clipped[clipped_count++] = input[index];
            if (current_in != next_in && clipped_count < 4) {
                const float denominator = distance[index] - distance[next];
                if (std::fabs(denominator) > 0.000001f) {
                    clipped[clipped_count++] = input[index] +
                        (input[next] - input[index]) * (distance[index] / denominator);
                }
            }
        }
        if (clipped_count < 3) return false;
        float min_x = std::numeric_limits<float>::max();
        float min_y = std::numeric_limits<float>::max();
        float max_x = -std::numeric_limits<float>::max();
        float max_y = -std::numeric_limits<float>::max();
        for (int index = 0; index < clipped_count; ++index) {
            Quaternion projected = {clipped[index].x, clipped[index].y, clipped[index].z, 1.0f};
            projected = QuaternionTransform(projected, view_matrix);
            projected = QuaternionTransform(projected, projection_matrix);
            if (std::fabs(projected.w) < 0.000001f) return true;
            const float inverse_w = 1.0f / projected.w;
            const float ndc_x = projected.x * inverse_w;
            const float ndc_y = projected.y * inverse_w;
            const float ndc_z = projected.z * inverse_w;
            if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) || !std::isfinite(ndc_z)) return true;
            const float screen_x = (ndc_x + 1.0f) * 0.5f *
                static_cast<float>(harness.renderer.native_w);
            const float screen_y = (1.0f - ndc_y) * 0.5f *
                static_cast<float>(harness.renderer.native_h);
            min_x = fminf(min_x, screen_x);
            min_y = fminf(min_y, screen_y);
            max_x = fmaxf(max_x, screen_x);
            max_y = fmaxf(max_y, screen_y);
        }
        constexpr float padding = 2.0f;
        return max_x + padding >= 0.0f && max_y + padding >= 0.0f &&
            min_x - padding <= static_cast<float>(harness.renderer.native_w) &&
            min_y - padding <= static_cast<float>(harness.renderer.native_h);
    };

    for (ol::u32 triangle_index : surface.triangles) {
        if (triangle_index >= geometry->triangle_count) continue;
        const ol::Triangle triangle = geometry->triangles[triangle_index];
        const Vector3 a = Vector3Transform(geometry->vertices[triangle.a], basis) + mesh_origin;
        const Vector3 b = Vector3Transform(geometry->vertices[triangle.b], basis) + mesh_origin;
        const Vector3 c = Vector3Transform(geometry->vertices[triangle.c], basis) + mesh_origin;
        if (triangle_intersects_viewport(a, b, c)) return true;
    }
    return false;
}

std::vector<LightingSurfaceSnapshot> snapshot_lighting_surfaces(
    const PlaygroundLightingHarness& harness,
    const LightingCheckpoint& checkpoint) {
    const ol::RenderState& renderer = harness.renderer;
    std::vector<LightingSurfaceSnapshot> result{};
    result.reserve(renderer.lighting_surfaces.size());
    for (const ol::MeshLightingSurfaceCache& surface : renderer.lighting_surfaces) {
        const bool current = surface.resolved_scene_signature == renderer.radiance_scene_signature &&
            surface.resolved_scene_signature != 0 && IsRenderTextureValid(surface.texture);
        const bool composite = current && surface.dynamic_composite_active &&
            IsRenderTextureValid(surface.shadow_mask_texture);
        const Texture2D displayed = surface.texture.texture;
        LightingSurfaceSnapshot snapshot{};
        snapshot.key = surface.surface_signature;
        snapshot.mesh_id = surface.mesh_id;
        snapshot.stable_texture_hash = current
            ? texture_mip_chain_hash(surface.texture.texture) : 0;
        snapshot.displayed_texture_hash = snapshot.stable_texture_hash;
        snapshot.shadow_mask_signature = surface.shadow_mask_signature;
        snapshot.shadow_mask_valid = IsRenderTextureValid(surface.shadow_mask_texture);
        snapshot.shadow_mask_hash = snapshot.shadow_mask_valid
            ? texture_mip_chain_hash(surface.shadow_mask_texture.texture) : 0;
        if (composite) {
            snapshot.displayed_texture_hash ^=
                snapshot.shadow_mask_hash + 0x9e3779b97f4a7c15ull +
                (snapshot.displayed_texture_hash << 6) +
                (snapshot.displayed_texture_hash >> 2);
        }
        snapshot.width = surface.width;
        snapshot.height = surface.height;
        snapshot.stable_mipmaps = current ? surface.texture.texture.mipmaps : 0;
        snapshot.displayed_mipmaps = current ? displayed.mipmaps : 0;
        snapshot.displayed_texture_id = current ? displayed.id : 0;
        snapshot.valid_x = current ? surface.valid_x : 0;
        snapshot.valid_y = current ? surface.valid_y : 0;
        snapshot.valid_width = current ? surface.valid_width : 0;
        snapshot.valid_height = current ? surface.valid_height : 0;
        snapshot.valid_mip_level = current ? surface.valid_mip_level : 30;
        snapshot.fully_initialized = current && surface.fully_initialized;
        snapshot.full_current_signature_coverage = current &&
            surface.valid_x == 0 && surface.valid_y == 0 &&
            surface.valid_width == surface.width && surface.valid_height == surface.height &&
            surface.valid_mip_level == 0;
        if (current) {
            snapshot_texture_sampling(
                displayed,
                &snapshot.min_filter,
                &snapshot.mag_filter,
                &snapshot.wrap_s,
                &snapshot.wrap_t,
                &snapshot.anisotropy);
        }
        snapshot.dynamic_composite_active = composite;
        snapshot.visible_at_checkpoint =
            lighting_surface_visible_at_checkpoint(harness, surface, checkpoint);
        snapshot.current = current;
        result.push_back(snapshot);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.key != b.key) return a.key < b.key;
        if (a.width != b.width) return a.width < b.width;
        return a.height < b.height;
    });
    return result;
}

void lighting_diagnostic_hash_add(ol::u64* hash, ol::u64 value) {
    if (!hash) return;
    for (int byte = 0; byte < 8; ++byte) {
        *hash ^= (value >> (byte * 8)) & 0xffu;
        *hash *= 1099511628211ull;
    }
}

LightingRenderStateSnapshot snapshot_lighting_render_state(
    const PlaygroundLightingHarness& harness,
    const LightingCheckpoint& checkpoint) {
    LightingRenderStateSnapshot result{};
    result.camera_anchor = test_pos(
        harness.app->dimension_id, checkpoint.feet, harness.dim->chunk_size_m);
    result.chunk_order_hash = 1469598103934665603ull;
    result.edge_input_order_hash = 1469598103934665603ull;
    result.lighting_surface_order_hash = 1469598103934665603ull;
    result.visible_lighting_surface_order_hash = 1469598103934665603ull;
    for (ol::u32 slot = 0; slot < harness.dim->chunks.count; ++slot) {
        const ol::Chunk& chunk = harness.dim->chunks.data[slot];
        const float dx = static_cast<float>(chunk.coord.x - result.camera_anchor.chunk.x);
        const float dy = static_cast<float>(chunk.coord.y - result.camera_anchor.chunk.y);
        const float dz = static_cast<float>(chunk.coord.z - result.camera_anchor.chunk.z);
        if (std::sqrt(dx * dx + dy * dy + dz * dz) >
            static_cast<float>(harness.dim->render_radius_chunks)) continue;
        const ol::u32 chunk_id = ol::arena_id_at_slot(&harness.dim->chunks, slot);
        result.active_chunks.push_back({chunk_id, chunk.coord, chunk.mesh_count});
        for (ol::u64 value : {
                 static_cast<ol::u64>(chunk_id),
                 static_cast<ol::u64>(static_cast<ol::u32>(chunk.coord.x)),
                 static_cast<ol::u64>(static_cast<ol::u32>(chunk.coord.y)),
                 static_cast<ol::u64>(static_cast<ol::u32>(chunk.coord.z)),
                 static_cast<ol::u64>(chunk.mesh_count)}) {
            lighting_diagnostic_hash_add(&result.chunk_order_hash, value);
            lighting_diagnostic_hash_add(&result.edge_input_order_hash, value);
        }
        for (ol::u32 mesh_index = 0; mesh_index < chunk.mesh_count; ++mesh_index) {
            const ol::u32 mesh_id = chunk.meshes[mesh_index];
            const ol::MeshInstance* mesh = ol::arena_get(&harness.dim->meshes, mesh_id);
            if (!mesh || !mesh->visible || !mesh->draw_edges) continue;
            const ol::MeshGeometry* geometry =
                ol::arena_get(&harness.dim->geometries, mesh->geometry);
            lighting_diagnostic_hash_add(&result.edge_input_order_hash, mesh_id);
            lighting_diagnostic_hash_add(&result.edge_input_order_hash, mesh->geometry);
            if (!geometry) continue;
            lighting_diagnostic_hash_add(&result.edge_input_order_hash, geometry->edge_count);
            for (ol::u32 edge_index = 0; edge_index < geometry->edge_count; ++edge_index) {
                const ol::Edge& edge = geometry->edges[edge_index];
                lighting_diagnostic_hash_add(
                    &result.edge_input_order_hash,
                    static_cast<ol::u64>(edge.a) |
                        (static_cast<ol::u64>(edge.b) << 16u) |
                        (static_cast<ol::u64>(edge.thickness_px) << 32u));
            }
        }
    }
    for (const ol::MeshLightingSurfaceCache& surface : harness.renderer.lighting_surfaces) {
        if (surface.resolved_scene_signature != harness.renderer.radiance_scene_signature ||
            !IsRenderTextureValid(surface.texture)) continue;
        result.surface_order.push_back({surface.surface_signature, surface.mesh_id});
        lighting_diagnostic_hash_add(
            &result.lighting_surface_order_hash, surface.surface_signature);
        lighting_diagnostic_hash_add(&result.lighting_surface_order_hash, surface.mesh_id);
        if (lighting_surface_visible_at_checkpoint(harness, surface, checkpoint)) {
            result.visible_surface_order.push_back({surface.surface_signature, surface.mesh_id});
            lighting_diagnostic_hash_add(
                &result.visible_lighting_surface_order_hash, surface.surface_signature);
            lighting_diagnostic_hash_add(
                &result.visible_lighting_surface_order_hash, surface.mesh_id);
        }
    }
    result.radiance_scene_signature = harness.renderer.radiance_scene_signature;
    result.radiance_topology_signature = harness.renderer.radiance_topology_signature;
    result.radiance_settings_signature = harness.renderer.radiance_settings_signature;
    result.radiance_player_signature = harness.renderer.radiance_player_signature;
    result.debug_edge_count = harness.renderer.debug_edge_count;
    result.debug_scene_triangle_count = harness.renderer.debug_scene_triangle_count;
    result.native_w = harness.renderer.native_w;
    result.native_h = harness.renderer.native_h;
    result.window_w = harness.renderer.window_w;
    result.window_h = harness.renderer.window_h;
    result.target_fbo = harness.renderer.target.id;
    result.target_texture = harness.renderer.target.texture.id;
    result.target_depth = harness.renderer.target.depth.id;
    result.target_width = harness.renderer.target.texture.width;
    result.target_height = harness.renderer.target.texture.height;
    result.target_format = harness.renderer.target.texture.format;
    result.lighting_enabled = harness.renderer.lighting.enabled;
    result.depth_test_edges = harness.renderer.depth_test_edges;
    result.shader_depth_edges = harness.renderer.shader_depth_edges;
    result.gpu_depth_edges = harness.renderer.gpu_depth_edges;
    return result;
}

void print_lighting_render_state_comparison(
    const char* route_name,
    const LightingRenderStateSnapshot& expected,
    const LightingRenderStateSnapshot& observed,
    const PlaygroundLightingHarness& harness) {
    std::printf(
        "  render state route=%s camera_chunk=(%d,%d,%d)/(%d,%d,%d) "
        "camera_local=(%.4f,%.4f,%.4f)/(%.4f,%.4f,%.4f) "
        "chunks=%zu/%zu chunk_hash=%llu/%llu edge_input_hash=%llu/%llu "
        "edge_count=%u/%u scene_tris=%u/%u surface_order=%llu/%llu(%zu/%zu) "
        "visible_surface_order=%llu/%llu(%zu/%zu)\n",
        route_name,
        expected.camera_anchor.chunk.x, expected.camera_anchor.chunk.y, expected.camera_anchor.chunk.z,
        observed.camera_anchor.chunk.x, observed.camera_anchor.chunk.y, observed.camera_anchor.chunk.z,
        expected.camera_anchor.local.x, expected.camera_anchor.local.y, expected.camera_anchor.local.z,
        observed.camera_anchor.local.x, observed.camera_anchor.local.y, observed.camera_anchor.local.z,
        expected.active_chunks.size(), observed.active_chunks.size(),
        static_cast<unsigned long long>(expected.chunk_order_hash),
        static_cast<unsigned long long>(observed.chunk_order_hash),
        static_cast<unsigned long long>(expected.edge_input_order_hash),
        static_cast<unsigned long long>(observed.edge_input_order_hash),
        expected.debug_edge_count, observed.debug_edge_count,
        expected.debug_scene_triangle_count, observed.debug_scene_triangle_count,
        static_cast<unsigned long long>(expected.lighting_surface_order_hash),
        static_cast<unsigned long long>(observed.lighting_surface_order_hash),
        expected.surface_order.size(), observed.surface_order.size(),
        static_cast<unsigned long long>(expected.visible_lighting_surface_order_hash),
        static_cast<unsigned long long>(observed.visible_lighting_surface_order_hash),
        expected.visible_surface_order.size(), observed.visible_surface_order.size());
    std::printf(
        "  radiance route=%s scene=%llu/%llu topology=%llu/%llu settings=%llu/%llu "
        "player=%llu/%llu flags(light,depth,shader,gpu)=%d,%d,%d,%d/%d,%d,%d,%d\n",
        route_name,
        static_cast<unsigned long long>(expected.radiance_scene_signature),
        static_cast<unsigned long long>(observed.radiance_scene_signature),
        static_cast<unsigned long long>(expected.radiance_topology_signature),
        static_cast<unsigned long long>(observed.radiance_topology_signature),
        static_cast<unsigned long long>(expected.radiance_settings_signature),
        static_cast<unsigned long long>(observed.radiance_settings_signature),
        static_cast<unsigned long long>(expected.radiance_player_signature),
        static_cast<unsigned long long>(observed.radiance_player_signature),
        expected.lighting_enabled ? 1 : 0, expected.depth_test_edges ? 1 : 0,
        expected.shader_depth_edges ? 1 : 0, expected.gpu_depth_edges ? 1 : 0,
        observed.lighting_enabled ? 1 : 0, observed.depth_test_edges ? 1 : 0,
        observed.shader_depth_edges ? 1 : 0, observed.gpu_depth_edges ? 1 : 0);
    std::printf(
        "  target route=%s viewport=%dx%d(%dx%d)/%dx%d(%dx%d) "
        "fbo=%u/%u texture=%u/%u depth=%u/%u size=%dx%d/%dx%d format=%d/%d clear=sky-gradient\n",
        route_name,
        expected.native_w, expected.native_h, expected.window_w, expected.window_h,
        observed.native_w, observed.native_h, observed.window_w, observed.window_h,
        expected.target_fbo, observed.target_fbo,
        expected.target_texture, observed.target_texture,
        expected.target_depth, observed.target_depth,
        expected.target_width, expected.target_height,
        observed.target_width, observed.target_height,
        expected.target_format, observed.target_format);
    const auto chunk_orders_equal = [&]() {
        if (expected.active_chunks.size() != observed.active_chunks.size()) return false;
        for (size_t index = 0; index < expected.active_chunks.size(); ++index) {
            const LightingChunkOrderEntry& a = expected.active_chunks[index];
            const LightingChunkOrderEntry& b = observed.active_chunks[index];
            if (a.id != b.id || a.coord.x != b.coord.x || a.coord.y != b.coord.y ||
                a.coord.z != b.coord.z || a.mesh_count != b.mesh_count) return false;
        }
        return true;
    };
    if (!chunk_orders_equal()) {
        auto print_chunks = [&](const char* side, const auto& chunks) {
            std::printf("    %s chunk order:", side);
            for (const LightingChunkOrderEntry& chunk : chunks) {
                std::printf(
                    " id%u=(%d,%d,%d)[%u]", chunk.id,
                    chunk.coord.x, chunk.coord.y, chunk.coord.z, chunk.mesh_count);
            }
            std::printf("\n");
        };
        print_chunks("reference", expected.active_chunks);
        print_chunks("actual", observed.active_chunks);
    }
    const auto surface_orders_equal = [](
        const std::vector<LightingSurfaceOrderEntry>& a,
        const std::vector<LightingSurfaceOrderEntry>& b) {
        if (a.size() != b.size()) return false;
        for (size_t index = 0; index < a.size(); ++index) {
            if (a[index].key != b[index].key || a[index].mesh_id != b[index].mesh_id) {
                return false;
            }
        }
        return true;
    };
    const auto print_surface_order_difference = [&](const char* label,
        const std::vector<LightingSurfaceOrderEntry>& reference,
        const std::vector<LightingSurfaceOrderEntry>& actual) {
        if (surface_orders_equal(reference, actual)) return;
        const size_t common = std::min(reference.size(), actual.size());
        size_t first = 0;
        while (first < common &&
               reference[first].key == actual[first].key &&
               reference[first].mesh_id == actual[first].mesh_id) {
            ++first;
        }
        std::printf("    first %s lighting surface order difference index=%zu\n", label, first);
        const size_t begin = first > 2 ? first - 2 : 0;
        const size_t end = std::min(std::max(reference.size(), actual.size()), first + 5);
        for (size_t index = begin; index < end; ++index) {
            auto print_entry = [&](const char* side,
                const std::vector<LightingSurfaceOrderEntry>& order) {
                if (index >= order.size()) {
                    std::printf("      %s[%zu]=<end>\n", side, index);
                    return;
                }
                const LightingSurfaceOrderEntry& entry = order[index];
                const ol::MeshInstance* mesh = ol::arena_get(&harness.dim->meshes, entry.mesh_id);
                std::printf(
                    "      %s[%zu]=mesh%u(%s) key=%llu\n",
                    side, index, entry.mesh_id, mesh ? mesh->name : "?",
                    static_cast<unsigned long long>(entry.key));
            };
            print_entry("reference", reference);
            print_entry("actual", actual);
        }
    };
    print_surface_order_difference("all", expected.surface_order, observed.surface_order);
    print_surface_order_difference(
        "visible", expected.visible_surface_order, observed.visible_surface_order);
}

LightingCheckpointReference make_fresh_lighting_reference(
    const LightingCheckpoint& checkpoint,
    const PlaygroundLightingConfig& config = {}) {
    LightingCheckpointReference reference{};
    reference.checkpoint = checkpoint;
    PlaygroundLightingHarness harness{};
    if (!init_playground_lighting_harness(&harness, checkpoint.name, config)) return reference;
    warm_lighting_checkpoint(&harness, checkpoint);
    reference.image = capture_render_target(&harness.renderer);
    reference.surfaces = snapshot_lighting_surfaces(harness, checkpoint);
    reference.render_state = snapshot_lighting_render_state(harness, checkpoint);
    const size_t current_count = static_cast<size_t>(std::count_if(
        reference.surfaces.begin(), reference.surfaces.end(),
        [](const LightingSurfaceSnapshot& surface) { return surface.current; }));
    std::printf(
        "lighting reference %-18s surfaces=%zu current=%zu scene=%llu\n",
        checkpoint.name, reference.surfaces.size(), current_count,
        static_cast<unsigned long long>(harness.renderer.radiance_scene_signature));
    shutdown_playground_lighting_harness(&harness);
    return reference;
}

constexpr int lighting_heading_count = 4;

struct LightingHeadingReferenceSet {
    std::array<LightingCheckpointReference, lighting_heading_count> headings{};
};

LightingHeadingReferenceSet make_fresh_lighting_heading_references(
    const LightingCheckpoint& checkpoint) {
    LightingHeadingReferenceSet result{};
    for (int heading = 0; heading < lighting_heading_count; ++heading) {
        LightingCheckpoint heading_checkpoint = checkpoint;
        heading_checkpoint.yaw = static_cast<float>(heading) * 0.5f * PI;
        result.headings[heading] = make_fresh_lighting_reference(heading_checkpoint);
    }
    return result;
}

void unload_lighting_heading_references(LightingHeadingReferenceSet* references) {
    if (!references) return;
    for (LightingCheckpointReference& reference : references->headings) {
        if (IsImageValid(reference.image)) UnloadImage(reference.image);
        reference.image = {};
    }
}

struct LightingRouteComparison {
    ImageDiffStats image{};
    size_t reference_surfaces = 0;
    size_t actual_current_surfaces = 0;
    size_t common_surfaces = 0;
    size_t missing_surfaces = 0;
    size_t extra_surfaces = 0;
    size_t stable_mismatched_surfaces = 0;
    size_t hard_stable_mismatches = 0;
    size_t transient_valid_region_stable_mismatches = 0;
    size_t hard_shadow_mask_mismatches = 0;
    size_t displayed_mismatched_surfaces = 0;
    size_t transient_dynamic_display_mismatches = 0;
    size_t texture_state_mismatches = 0;
    size_t composite_state_mismatches = 0;
    size_t mip_count_mismatches = 0;
    size_t visible_reference_surfaces = 0;
    size_t visible_actual_surfaces = 0;
    size_t visible_missing_surfaces = 0;
    size_t visible_extra_surfaces = 0;
    size_t visible_stable_mismatches = 0;
    size_t visible_transient_valid_region_stable_mismatches = 0;
    size_t visible_shadow_mask_mismatches = 0;
    size_t visible_displayed_mismatches = 0;
    size_t visible_transient_dynamic_display_mismatches = 0;
    size_t visible_texture_state_mismatches = 0;
    size_t visible_composite_mismatches = 0;
    size_t visible_mip_count_mismatches = 0;
};

LightingRouteComparison compare_lighting_checkpoint(
    const LightingCheckpointReference& reference,
    PlaygroundLightingHarness* harness,
    const char* route_name) {
    LightingRouteComparison comparison{};
    const Image actual = capture_render_target(&harness->renderer);
    comparison.image = compare_images(reference.image, actual, 2);
    const LightingRenderStateSnapshot actual_render_state =
        snapshot_lighting_render_state(*harness, reference.checkpoint);
    const std::vector<LightingSurfaceSnapshot> actual_surfaces =
        snapshot_lighting_surfaces(*harness, reference.checkpoint);
    std::unordered_map<ol::u64, const LightingSurfaceSnapshot*> actual_by_key{};
    std::unordered_map<ol::u64, const LightingSurfaceSnapshot*> expected_by_key{};
    actual_by_key.reserve(actual_surfaces.size());
    for (const LightingSurfaceSnapshot& surface : actual_surfaces) {
        actual_by_key.emplace(surface.key, &surface);
        if (surface.current) {
            ++comparison.actual_current_surfaces;
            if (surface.visible_at_checkpoint) ++comparison.visible_actual_surfaces;
        }
    }
    expected_by_key.reserve(reference.surfaces.size());
    for (const LightingSurfaceSnapshot& surface : reference.surfaces) {
        if (surface.current) expected_by_key.emplace(surface.key, &surface);
    }
    for (const LightingSurfaceSnapshot& expected : reference.surfaces) {
        if (!expected.current) continue;
        ++comparison.reference_surfaces;
        if (expected.visible_at_checkpoint) ++comparison.visible_reference_surfaces;
        const auto found = actual_by_key.find(expected.key);
        if (found == actual_by_key.end() || !found->second->current) {
            ++comparison.missing_surfaces;
            if (expected.visible_at_checkpoint) {
                ++comparison.visible_missing_surfaces;
                const ol::MeshInstance* mesh = ol::arena_get(
                    &harness->dim->meshes, expected.mesh_id);
                std::printf(
                    "  visible surface missing route=%s mesh=%s key=%llu size=%dx%d\n",
                    route_name, mesh ? mesh->name : "?",
                    static_cast<unsigned long long>(expected.key),
                    expected.width, expected.height);
            }
            continue;
        }
        ++comparison.common_surfaces;
        const LightingSurfaceSnapshot& observed = *found->second;
        const bool dimensions_match = expected.width == observed.width &&
            expected.height == observed.height;
        const bool stable_texture_matches = dimensions_match &&
            expected.stable_texture_hash == observed.stable_texture_hash;
        const bool frame_matches_exactly = comparison.image.changed_pixels == 0 &&
            comparison.image.mean_abs_rgb == 0.0 && comparison.image.max_delta == 0;
        const bool transient_valid_region_stable_mismatch = !stable_texture_matches &&
            dimensions_match && frame_matches_exactly &&
            (!expected.full_current_signature_coverage ||
             !observed.full_current_signature_coverage);
        if (!stable_texture_matches) {
            ++comparison.stable_mismatched_surfaces;
            if (transient_valid_region_stable_mismatch) {
                ++comparison.transient_valid_region_stable_mismatches;
                if (expected.visible_at_checkpoint) {
                    ++comparison.visible_transient_valid_region_stable_mismatches;
                }
            } else {
                ++comparison.hard_stable_mismatches;
                if (expected.visible_at_checkpoint) ++comparison.visible_stable_mismatches;
            }
        }
        // Unlike a partially initialized stable cache, the persistent shadow
        // mask is complete from allocation onward. Any active composite must
        // therefore represent exactly the same shadow state regardless of the
        // route taken to this checkpoint; no offscreen region is undefined.
        const bool compare_shadow_masks = dimensions_match &&
            (expected.dynamic_composite_active || observed.dynamic_composite_active);
        const bool shadow_mask_matches = !compare_shadow_masks ||
            (expected.shadow_mask_valid && observed.shadow_mask_valid &&
             expected.shadow_mask_signature == observed.shadow_mask_signature &&
             expected.shadow_mask_hash == observed.shadow_mask_hash);
        if (!shadow_mask_matches) {
            ++comparison.hard_shadow_mask_mismatches;
            if (expected.visible_at_checkpoint || observed.visible_at_checkpoint) {
                ++comparison.visible_shadow_mask_mismatches;
            }
        }
        const bool displayed_texture_matches = dimensions_match &&
            expected.displayed_texture_hash == observed.displayed_texture_hash;
        // A route may leave different offscreen backing in a partially valid
        // stable cache. The displayed hash combines that stable texture with
        // the complete persistent shadow mask, so classify the same harmless
        // backing mismatch consistently when the rendered frame is exact.
        const bool transient_dynamic_display_mismatch = !displayed_texture_matches &&
            (stable_texture_matches || transient_valid_region_stable_mismatch) &&
            shadow_mask_matches &&
            expected.dynamic_composite_active && observed.dynamic_composite_active &&
            expected.displayed_mipmaps == observed.displayed_mipmaps &&
            frame_matches_exactly;
        if (!displayed_texture_matches) {
            ++comparison.displayed_mismatched_surfaces;
            if (transient_dynamic_display_mismatch) {
                ++comparison.transient_dynamic_display_mismatches;
                if (expected.visible_at_checkpoint) {
                    ++comparison.visible_transient_dynamic_display_mismatches;
                }
            } else if (expected.visible_at_checkpoint) {
                ++comparison.visible_displayed_mismatches;
            }
        }
        const bool texture_state_matches =
            expected.min_filter == observed.min_filter &&
            expected.mag_filter == observed.mag_filter &&
            expected.wrap_s == observed.wrap_s &&
            expected.wrap_t == observed.wrap_t &&
            std::fabs(expected.anisotropy - observed.anisotropy) < 0.001f;
        if (!texture_state_matches) {
            ++comparison.texture_state_mismatches;
            if (expected.visible_at_checkpoint) {
                ++comparison.visible_texture_state_mismatches;
                const ol::MeshInstance* mesh = ol::arena_get(
                    &harness->dim->meshes, expected.mesh_id);
                std::printf(
                    "  visible sampler differs route=%s mesh=%s key=%llu tex=%u/%u "
                    "min=0x%x/0x%x mag=0x%x/0x%x wrap=0x%x,0x%x/0x%x,0x%x "
                    "aniso=%.2f/%.2f\n",
                    route_name, mesh ? mesh->name : "?",
                    static_cast<unsigned long long>(expected.key),
                    expected.displayed_texture_id, observed.displayed_texture_id,
                    expected.min_filter, observed.min_filter,
                    expected.mag_filter, observed.mag_filter,
                    expected.wrap_s, expected.wrap_t,
                    observed.wrap_s, observed.wrap_t,
                    expected.anisotropy, observed.anisotropy);
            }
        }
        if (expected.dynamic_composite_active != observed.dynamic_composite_active) {
            ++comparison.composite_state_mismatches;
            if (expected.visible_at_checkpoint) ++comparison.visible_composite_mismatches;
        }
        if (expected.stable_mipmaps != observed.stable_mipmaps ||
            expected.displayed_mipmaps != observed.displayed_mipmaps) {
            ++comparison.mip_count_mismatches;
            if (expected.visible_at_checkpoint) ++comparison.visible_mip_count_mismatches;
        }
        if (expected.visible_at_checkpoint &&
            (!displayed_texture_matches ||
             !shadow_mask_matches ||
             expected.dynamic_composite_active != observed.dynamic_composite_active ||
             expected.displayed_mipmaps != observed.displayed_mipmaps)) {
            const ol::MeshInstance* mesh = ol::arena_get(
                &harness->dim->meshes, expected.mesh_id);
            std::printf(
                "  visible surface differs route=%s mesh=%s key=%llu size=%dx%d "
                "composite=%d/%d mips=%d/%d stable_equal=%d display_equal=%d "
                "transient_display=%d transient_stable=%d mask_equal=%d "
                "mask_valid=%d/%d mask_signature=%llu/%llu mask_hash=%llu/%llu "
                "valid=%d,%d,%dx%d,mip%d,full%d,init%d/%d,%d,%dx%d,mip%d,full%d,init%d\n",
                route_name, mesh ? mesh->name : "?",
                static_cast<unsigned long long>(expected.key), expected.width, expected.height,
                expected.dynamic_composite_active ? 1 : 0,
                observed.dynamic_composite_active ? 1 : 0,
                expected.displayed_mipmaps, observed.displayed_mipmaps,
                stable_texture_matches ? 1 : 0,
                displayed_texture_matches ? 1 : 0,
                transient_dynamic_display_mismatch ? 1 : 0,
                transient_valid_region_stable_mismatch ? 1 : 0,
                shadow_mask_matches ? 1 : 0,
                expected.shadow_mask_valid ? 1 : 0,
                observed.shadow_mask_valid ? 1 : 0,
                static_cast<unsigned long long>(expected.shadow_mask_signature),
                static_cast<unsigned long long>(observed.shadow_mask_signature),
                static_cast<unsigned long long>(expected.shadow_mask_hash),
                static_cast<unsigned long long>(observed.shadow_mask_hash),
                expected.valid_x, expected.valid_y,
                expected.valid_width, expected.valid_height, expected.valid_mip_level,
                expected.full_current_signature_coverage ? 1 : 0,
                expected.fully_initialized ? 1 : 0,
                observed.valid_x, observed.valid_y,
                observed.valid_width, observed.valid_height, observed.valid_mip_level,
                observed.full_current_signature_coverage ? 1 : 0,
                observed.fully_initialized ? 1 : 0);
        }
    }

    for (const LightingSurfaceSnapshot& observed : actual_surfaces) {
        if (!observed.current || expected_by_key.find(observed.key) != expected_by_key.end()) continue;
        ++comparison.extra_surfaces;
        if (!observed.visible_at_checkpoint) continue;
        ++comparison.visible_extra_surfaces;
        const ol::MeshInstance* mesh = ol::arena_get(
            &harness->dim->meshes, observed.mesh_id);
        std::printf(
            "  visible surface extra route=%s mesh=%s key=%llu size=%dx%d "
            "composite=%d tex=%u min=0x%x mag=0x%x wrap=0x%x,0x%x aniso=%.2f\n",
            route_name, mesh ? mesh->name : "?",
            static_cast<unsigned long long>(observed.key),
            observed.width, observed.height,
            observed.dynamic_composite_active ? 1 : 0,
            observed.displayed_texture_id,
            observed.min_filter, observed.mag_filter,
            observed.wrap_s, observed.wrap_t, observed.anisotropy);
    }

    std::printf(
        "lighting route %-24s checkpoint=%-16s image_changed=%llu edge_disagree=%llu "
        "edge_ref_only=%llu edge_actual_only=%llu mean=%.4f max=%d "
        "surface_ref=%zu surface_actual=%zu common=%zu missing=%zu extra=%zu "
        "stable_mismatch=%zu hard_stable=%zu transient_valid_stable=%zu "
        "hard_mask_mismatch=%zu display_mismatch=%zu transient_display=%zu sampler_mismatch=%zu "
        "composite_mismatch=%zu mip_count_mismatch=%zu visible_ref=%zu visible_actual=%zu "
        "visible_missing=%zu visible_extra=%zu visible_stable_mismatch=%zu "
        "visible_transient_valid_stable=%zu "
        "visible_mask_mismatch=%zu visible_display_mismatch=%zu visible_transient_display=%zu "
        "visible_sampler_mismatch=%zu visible_composite_mismatch=%zu visible_mip_mismatch=%zu\n",
        route_name, reference.checkpoint.name,
        static_cast<unsigned long long>(comparison.image.changed_pixels),
        static_cast<unsigned long long>(comparison.image.edge_disagreement_pixels),
        static_cast<unsigned long long>(comparison.image.a_edge_only_pixels),
        static_cast<unsigned long long>(comparison.image.b_edge_only_pixels),
        comparison.image.mean_abs_rgb, comparison.image.max_delta,
        comparison.reference_surfaces, comparison.actual_current_surfaces,
        comparison.common_surfaces, comparison.missing_surfaces, comparison.extra_surfaces,
        comparison.stable_mismatched_surfaces,
        comparison.hard_stable_mismatches,
        comparison.transient_valid_region_stable_mismatches,
        comparison.hard_shadow_mask_mismatches,
        comparison.displayed_mismatched_surfaces,
        comparison.transient_dynamic_display_mismatches,
        comparison.texture_state_mismatches,
        comparison.composite_state_mismatches, comparison.mip_count_mismatches,
        comparison.visible_reference_surfaces, comparison.visible_actual_surfaces,
        comparison.visible_missing_surfaces, comparison.visible_extra_surfaces,
        comparison.visible_stable_mismatches,
        comparison.visible_transient_valid_region_stable_mismatches,
        comparison.visible_shadow_mask_mismatches,
        comparison.visible_displayed_mismatches,
        comparison.visible_transient_dynamic_display_mismatches,
        comparison.visible_texture_state_mismatches,
        comparison.visible_composite_mismatches,
        comparison.visible_mip_count_mismatches);

    const bool differs = comparison.image.changed_pixels != 0 ||
        comparison.image.mean_abs_rgb != 0.0 || comparison.image.max_delta != 0 ||
        comparison.visible_missing_surfaces != 0 ||
        comparison.visible_extra_surfaces != 0 ||
        comparison.hard_stable_mismatches != 0 ||
        comparison.hard_shadow_mask_mismatches != 0 ||
        comparison.visible_shadow_mask_mismatches != 0 ||
        comparison.visible_displayed_mismatches != 0 ||
        comparison.visible_texture_state_mismatches != 0 ||
        comparison.visible_composite_mismatches != 0 ||
        comparison.visible_mip_count_mismatches != 0;
    if (differs) {
        print_lighting_render_state_comparison(
            route_name, reference.render_state, actual_render_state, *harness);
        std::filesystem::create_directories("test-output");
        char actual_path[192]{};
        char reference_path[192]{};
        std::snprintf(actual_path, sizeof(actual_path), "test-output/lighting-route-%s-actual.png", route_name);
        std::snprintf(reference_path, sizeof(reference_path), "test-output/lighting-route-%s-reference.png", route_name);
        export_captured_image(actual, actual_path);
        export_captured_image(reference.image, reference_path);
    }
    UnloadImage(actual);
    return comparison;
}

bool lighting_route_comparison_passes(const LightingRouteComparison& comparison) {
    return comparison.image.changed_pixels == 0 &&
        comparison.image.mean_abs_rgb == 0.0 && comparison.image.max_delta == 0 &&
        comparison.visible_missing_surfaces == 0 &&
        comparison.visible_extra_surfaces == 0 &&
        comparison.hard_stable_mismatches == 0 &&
        comparison.hard_shadow_mask_mismatches == 0 &&
        comparison.visible_shadow_mask_mismatches == 0 &&
        comparison.visible_displayed_mismatches == 0 &&
        comparison.visible_texture_state_mismatches == 0 &&
        comparison.visible_composite_mismatches == 0 &&
        comparison.visible_mip_count_mismatches == 0;
}

bool compare_lighting_destination_headings(
    const LightingHeadingReferenceSet& references,
    PlaygroundLightingHarness* harness,
    const char* route_name) {
    bool ok = true;
    for (int heading = 0; heading < lighting_heading_count; ++heading) {
        const LightingCheckpointReference& reference = references.headings[heading];
        place_lighting_test_player(harness, reference.checkpoint.feet);
        render_lighting_checkpoint(
            harness, reference.checkpoint.feet,
            reference.checkpoint.yaw, reference.checkpoint.pitch);
        char heading_name[112]{};
        std::snprintf(
            heading_name, sizeof(heading_name), "%s-heading%d", route_name, heading);
        const LightingRouteComparison comparison =
            compare_lighting_checkpoint(reference, harness, heading_name);
        ok = lighting_route_comparison_passes(comparison) && ok;
    }
    return ok;
}

void walk_lighting_route_segment(
    PlaygroundLightingHarness* harness,
    Vector3* current,
    Vector3 target,
    int subdivisions,
    float yaw = 0.0f,
    float pitch = -0.08f) {
    const Vector3 start = *current;
    subdivisions = std::max(1, subdivisions);
    for (int step = 1; step <= subdivisions; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(subdivisions);
        const Vector3 feet = Vector3Lerp(start, target, t);
        place_lighting_test_player(harness, feet);
        render_lighting_checkpoint(harness, feet, yaw, pitch);
    }
    *current = target;
}

void settle_lighting_route(
    PlaygroundLightingHarness* harness,
    const LightingCheckpoint& checkpoint,
    int settle_frames) {
    place_lighting_test_player(harness, checkpoint.feet);
    for (int frame = 0; frame < settle_frames; ++frame) {
        render_lighting_checkpoint(
            harness, checkpoint.feet, checkpoint.yaw, checkpoint.pitch);
    }
}

bool run_axis_lighting_route(
    const LightingCheckpointReference& reference,
    const std::array<const LightingHeadingReferenceSet*, 4>& excursion_references,
    int subdivisions,
    int settle_frames,
    const char* label) {
    PlaygroundLightingHarness harness{};
    if (!init_playground_lighting_harness(&harness, label)) return false;
    warm_lighting_checkpoint(&harness, reference.checkpoint);
    Vector3 current = reference.checkpoint.feet;
    const std::array<Vector3, 4> excursions = {
        Vector3{-0.25f, 0.0f, 8.0f},
        Vector3{0.25f, 0.0f, -0.25f},
        Vector3{16.25f, 0.0f, 8.0f},
        Vector3{-16.25f, 0.0f, 8.0f}};
    bool ok = true;
    for (size_t excursion_index = 0; excursion_index < excursions.size(); ++excursion_index) {
        walk_lighting_route_segment(&harness, &current, excursions[excursion_index], subdivisions);
        char arrival_name[96]{};
        std::snprintf(
            arrival_name, sizeof(arrival_name), "%s-%zu-arrival", label, excursion_index);
        ok = compare_lighting_destination_headings(
            *excursion_references[excursion_index], &harness, arrival_name) && ok;
        walk_lighting_route_segment(
            &harness, &current, reference.checkpoint.feet, subdivisions);
        settle_lighting_route(&harness, reference.checkpoint, settle_frames);
        char comparison_name[96]{};
        std::snprintf(
            comparison_name, sizeof(comparison_name), "%s-%zu", label, excursion_index);
        const LightingRouteComparison comparison =
            compare_lighting_checkpoint(reference, &harness, comparison_name);
        ok = lighting_route_comparison_passes(comparison) && ok;
    }
    shutdown_playground_lighting_harness(&harness);
    return ok;
}

bool run_loop_lighting_route(
    const LightingCheckpointReference& reference,
    const LightingHeadingReferenceSet& return_heading_references,
    const LightingHeadingReferenceSet& q_np_reference,
    const LightingHeadingReferenceSet& q_nn_reference,
    const LightingHeadingReferenceSet& q_pn_reference,
    bool clockwise,
    int subdivisions,
    int settle_frames,
    const char* label) {
    PlaygroundLightingHarness harness{};
    if (!init_playground_lighting_harness(&harness, label)) return false;
    warm_lighting_checkpoint(&harness, reference.checkpoint);
    const Vector3 q_pp = {0.25f, 0.0f, 0.25f};
    const Vector3 q_np = {-0.25f, 0.0f, 0.25f};
    const Vector3 q_nn = {-0.25f, 0.0f, -0.25f};
    const Vector3 q_pn = {0.25f, 0.0f, -0.25f};
    const std::array<Vector3, 4> clockwise_route = {q_pn, q_nn, q_np, q_pp};
    const std::array<Vector3, 4> counterclockwise_route = {q_np, q_nn, q_pn, q_pp};
    const std::array<const LightingHeadingReferenceSet*, 4> clockwise_references = {
        &q_pn_reference, &q_nn_reference, &q_np_reference, &return_heading_references};
    const std::array<const LightingHeadingReferenceSet*, 4> counterclockwise_references = {
        &q_np_reference, &q_nn_reference, &q_pn_reference, &return_heading_references};
    Vector3 current = reference.checkpoint.feet;
    const auto& route = clockwise ? clockwise_route : counterclockwise_route;
    const auto& route_references = clockwise ? clockwise_references : counterclockwise_references;
    bool ok = true;
    for (size_t route_index = 0; route_index < route.size(); ++route_index) {
        walk_lighting_route_segment(&harness, &current, route[route_index], subdivisions);
        char arrival_name[96]{};
        std::snprintf(
            arrival_name, sizeof(arrival_name), "%s-%zu-arrival", label, route_index);
        ok = compare_lighting_destination_headings(
            *route_references[route_index], &harness, arrival_name) && ok;
    }
    settle_lighting_route(&harness, reference.checkpoint, settle_frames);
    const LightingRouteComparison comparison =
        compare_lighting_checkpoint(reference, &harness, label);
    shutdown_playground_lighting_harness(&harness);
    return lighting_route_comparison_passes(comparison) && ok;
}

bool run_user_lighting_route(
    const LightingCheckpointReference& reference,
    const LightingHeadingReferenceSet& away_references,
    int subdivisions,
    int settle_frames,
    const char* label) {
    PlaygroundLightingHarness harness{};
    if (!init_playground_lighting_harness(&harness, label)) return false;
    warm_lighting_checkpoint(&harness, reference.checkpoint);
    Vector3 current = reference.checkpoint.feet;
    const Vector3 away = {0.0419f, 0.0f, -1.4644f};
    walk_lighting_route_segment(&harness, &current, away, subdivisions, -0.0176f, 0.0022f);
    char arrival_name[96]{};
    std::snprintf(arrival_name, sizeof(arrival_name), "%s-arrival", label);
    const bool arrival_ok = compare_lighting_destination_headings(
        away_references, &harness, arrival_name);
    walk_lighting_route_segment(
        &harness, &current, reference.checkpoint.feet, subdivisions,
        reference.checkpoint.yaw, reference.checkpoint.pitch);
    settle_lighting_route(&harness, reference.checkpoint, settle_frames);
    const LightingRouteComparison comparison =
        compare_lighting_checkpoint(reference, &harness, label);
    shutdown_playground_lighting_harness(&harness);
    return arrival_ok && lighting_route_comparison_passes(comparison);
}

bool run_playground_pathtrace_reference_test();

bool run_lighting_user_route_regression_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "ol focused user lighting route");
    std::filesystem::create_directories("test-output");
    const LightingCheckpoint user_start = {
        "route-ref-user", {0.0f, 0.0f, 8.0f}, 0.0f, 0.0f};
    const LightingCheckpoint user_away = {
        "route-ref-user-away", {0.0419f, 0.0f, -1.4644f}, -0.0176f, 0.0022f};
    LightingCheckpointReference user_reference =
        make_fresh_lighting_reference(user_start);
    LightingHeadingReferenceSet away_references =
        make_fresh_lighting_heading_references(user_away);
    bool ok = IsImageValid(user_reference.image);
    for (const LightingCheckpointReference& heading : away_references.headings) {
        ok = IsImageValid(heading.image) && ok;
    }
    ok = run_user_lighting_route(
        user_reference, away_references, 24, 8, "user-route-focused") && ok;
    if (IsImageValid(user_reference.image)) UnloadImage(user_reference.image);
    unload_lighting_heading_references(&away_references);
    CloseWindow();
    return expect_true(ok, "Reported playground away-and-return lighting route is history independent");
}

bool run_lighting_route_regression_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "ol exhaustive lighting routes");
    std::filesystem::create_directories("test-output");

    const LightingCheckpoint user_start = {
        "route-ref-user", {0.0f, 0.0f, 8.0f}, 0.0f, 0.0f};
    const LightingCheckpoint axis_start = {
        "route-ref-axis", {0.25f, 0.0f, 8.0f}, 0.0f, -0.08f};
    const LightingCheckpoint corner_start = {
        "route-ref-corner", {0.25f, 0.0f, 0.25f}, 0.0f, -0.08f};
    const LightingCheckpoint user_away = {
        "route-ref-user-away", {0.0419f, 0.0f, -1.4644f}, -0.0176f, 0.0022f};
    const std::array<LightingCheckpoint, 4> axis_destinations = {{
        {"route-ref-x0-west", {-0.25f, 0.0f, 8.0f}, 0.0f, -0.08f},
        {"route-ref-z0-south", {0.25f, 0.0f, -0.25f}, 0.0f, -0.08f},
        {"route-ref-plus16", {16.25f, 0.0f, 8.0f}, 0.0f, -0.08f},
        {"route-ref-minus16", {-16.25f, 0.0f, 8.0f}, 0.0f, -0.08f},
    }};
    const LightingCheckpoint corner_np = {
        "route-ref-corner-np", {-0.25f, 0.0f, 0.25f}, 0.0f, -0.08f};
    const LightingCheckpoint corner_nn = {
        "route-ref-corner-nn", {-0.25f, 0.0f, -0.25f}, 0.0f, -0.08f};
    const LightingCheckpoint corner_pn = {
        "route-ref-corner-pn", {0.25f, 0.0f, -0.25f}, 0.0f, -0.08f};
    LightingCheckpointReference user_reference = make_fresh_lighting_reference(user_start);
    LightingCheckpointReference axis_reference = make_fresh_lighting_reference(axis_start);
    LightingHeadingReferenceSet corner_heading_references =
        make_fresh_lighting_heading_references(corner_start);
    const LightingCheckpointReference& corner_reference = corner_heading_references.headings[0];
    LightingHeadingReferenceSet user_away_references =
        make_fresh_lighting_heading_references(user_away);
    std::array<LightingHeadingReferenceSet, 4> axis_destination_references{};
    for (size_t i = 0; i < axis_destinations.size(); ++i) {
        axis_destination_references[i] =
            make_fresh_lighting_heading_references(axis_destinations[i]);
    }
    LightingHeadingReferenceSet corner_np_reference =
        make_fresh_lighting_heading_references(corner_np);
    LightingHeadingReferenceSet corner_nn_reference =
        make_fresh_lighting_heading_references(corner_nn);
    LightingHeadingReferenceSet corner_pn_reference =
        make_fresh_lighting_heading_references(corner_pn);
    const std::array<const LightingHeadingReferenceSet*, 4> axis_reference_ptrs = {
        &axis_destination_references[0], &axis_destination_references[1],
        &axis_destination_references[2], &axis_destination_references[3]};

    bool ok = IsImageValid(user_reference.image) && IsImageValid(axis_reference.image) &&
        IsImageValid(corner_reference.image);
    for (const LightingCheckpointReference& heading : user_away_references.headings) {
        ok = IsImageValid(heading.image) && ok;
    }
    for (const LightingHeadingReferenceSet& destination : axis_destination_references) {
        for (const LightingCheckpointReference& heading : destination.headings) {
            ok = IsImageValid(heading.image) && ok;
        }
    }
    for (const LightingHeadingReferenceSet* corner : {
             &corner_np_reference, &corner_nn_reference, &corner_pn_reference}) {
        for (const LightingCheckpointReference& heading : corner->headings) {
            ok = IsImageValid(heading.image) && ok;
        }
    }
    ok = run_user_lighting_route(
        user_reference, user_away_references, 24, 0, "user-route-immediate") && ok;
    ok = run_user_lighting_route(
        user_reference, user_away_references, 24, 8, "user-route-settle8") && ok;
    ok = run_axis_lighting_route(
        axis_reference, axis_reference_ptrs, 1, 0, "axis-step1-settle0") && ok;
    ok = run_axis_lighting_route(
        axis_reference, axis_reference_ptrs, 8, 2, "axis-step8-settle2") && ok;
    ok = run_axis_lighting_route(
        axis_reference, axis_reference_ptrs, 32, 8, "axis-step32-settle8") && ok;
    ok = run_loop_lighting_route(
        corner_reference, corner_heading_references,
        corner_np_reference, corner_nn_reference, corner_pn_reference,
        true, 8, 2, "corner-cw") && ok;
    ok = run_loop_lighting_route(
        corner_reference, corner_heading_references,
        corner_np_reference, corner_nn_reference, corner_pn_reference,
        false, 8, 2, "corner-ccw") && ok;
    ok = run_playground_pathtrace_reference_test() && ok;

    if (IsImageValid(user_reference.image)) UnloadImage(user_reference.image);
    if (IsImageValid(axis_reference.image)) UnloadImage(axis_reference.image);
    unload_lighting_heading_references(&corner_heading_references);
    unload_lighting_heading_references(&user_away_references);
    for (LightingHeadingReferenceSet& destination : axis_destination_references) {
        unload_lighting_heading_references(&destination);
    }
    unload_lighting_heading_references(&corner_np_reference);
    unload_lighting_heading_references(&corner_nn_reference);
    unload_lighting_heading_references(&corner_pn_reference);
    CloseWindow();
    return expect_true(ok, "Exhaustive playground routes match fresh references and bounded path trace error");
}

bool run_lighting_minus16_regression_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "ol focused minus-16 lighting route");
    std::filesystem::create_directories("test-output");
    const LightingCheckpoint start = {
        "route-ref-minus16", {0.25f, 0.0f, 8.0f}, 0.0f, -0.08f};
    const LightingCheckpoint destination = {
        "route-ref-minus16-destination", {-16.25f, 0.0f, 8.0f}, 0.0f, -0.08f};
    LightingCheckpointReference reference = make_fresh_lighting_reference(start);
    LightingHeadingReferenceSet destination_references =
        make_fresh_lighting_heading_references(destination);
    PlaygroundLightingHarness harness{};
    bool ok = IsImageValid(reference.image) &&
        init_playground_lighting_harness(&harness, "route-minus16-focused");
    if (harness.dim) {
        warm_lighting_checkpoint(&harness, start);
        Vector3 current = start.feet;
        walk_lighting_route_segment(
            &harness, &current, {-16.25f, 0.0f, 8.0f}, 8);
        ok = compare_lighting_destination_headings(
            destination_references, &harness, "minus16-focused-arrival") && ok;
        walk_lighting_route_segment(&harness, &current, start.feet, 8);
        settle_lighting_route(&harness, start, 8);
        const LightingRouteComparison comparison =
            compare_lighting_checkpoint(reference, &harness, "minus16-focused");
        ok = lighting_route_comparison_passes(comparison) && ok;
    }
    shutdown_playground_lighting_harness(&harness);
    if (IsImageValid(reference.image)) UnloadImage(reference.image);
    unload_lighting_heading_references(&destination_references);
    CloseWindow();
    return expect_true(ok, "Minus-16 arrival and return match their fresh displayed lighting states");
}

bool run_lighting_render_state_diagnostic_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "ol lighting render-state diagnostic");
    std::filesystem::create_directories("test-output");
    const LightingCheckpoint start = {
        "route-state-start", {0.0f, 0.0f, 8.0f}, 0.0f, 0.0f};
    const Vector3 away = {0.0419f, 0.0f, -1.4644f};
    struct Variant {
        const char* name = "";
        PlaygroundLightingConfig config{};
    };
    const std::array<Variant, 5> variants = {{
        {"default", {true, true, true, true}},
        {"compute-edges", {true, true, false, true}},
        {"unfiltered-edges", {true, false, false, false}},
        {"no-lighting", {false, true, true, true}},
        {"no-lighting-unfiltered-edges", {false, false, false, false}},
    }};
    bool ok = true;
    for (const Variant& variant : variants) {
        LightingCheckpoint configured_start = start;
        configured_start.name = variant.name;
        LightingCheckpointReference reference =
            make_fresh_lighting_reference(configured_start, variant.config);
        PlaygroundLightingHarness harness{};
        bool variant_ok = IsImageValid(reference.image) &&
            init_playground_lighting_harness(&harness, variant.name, variant.config);
        if (harness.dim) {
            warm_lighting_checkpoint(&harness, configured_start);
            Vector3 current = start.feet;
            walk_lighting_route_segment(
                &harness, &current, away, 24, -0.0176f, 0.0022f);
            for (int heading = 0; heading < lighting_heading_count; ++heading) {
                render_lighting_checkpoint(
                    &harness, away, static_cast<float>(heading) * 0.5f * PI, 0.0022f);
            }
            walk_lighting_route_segment(
                &harness, &current, start.feet, 24, start.yaw, start.pitch);
            char immediate_name[112]{};
            std::snprintf(
                immediate_name, sizeof(immediate_name), "state-%s-immediate", variant.name);
            const LightingRouteComparison immediate =
                compare_lighting_checkpoint(reference, &harness, immediate_name);
            settle_lighting_route(&harness, configured_start, 8);
            char settled_name[112]{};
            std::snprintf(
                settled_name, sizeof(settled_name), "state-%s-settle8", variant.name);
            const LightingRouteComparison settled =
                compare_lighting_checkpoint(reference, &harness, settled_name);
            variant_ok = lighting_route_comparison_passes(immediate) &&
                lighting_route_comparison_passes(settled) && variant_ok;
        }
        std::printf(
            "lighting render-state variant %-28s exact=%d\n",
            variant.name, variant_ok ? 1 : 0);
        shutdown_playground_lighting_harness(&harness);
        if (IsImageValid(reference.image)) UnloadImage(reference.image);
        ok = variant_ok && ok;
    }
    CloseWindow();
    return expect_true(ok, "Playground return is exact across lighting and edge pipeline variants");
}

struct PtTriangle {
    Vector3 a{};
    Vector3 b{};
    Vector3 c{};
    Vector3 albedo{};
    Vector3 emission{};
    float reflectivity = 0.0f;
};

struct PtHit {
    bool exists = false;
    bool front_facing = true;
    float t = 0.0f;
    Vector3 position{};
    Vector3 normal{};
    Vector3 albedo{};
    Vector3 emission{};
    float reflectivity = 0.0f;
};

Vector3 pt_component_multiply(Vector3 a, Vector3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

Vector3 pt_linear_color(Color color) {
    return {
        std::pow(static_cast<float>(color.r) / 255.0f, 2.2f),
        std::pow(static_cast<float>(color.g) / 255.0f, 2.2f),
        std::pow(static_cast<float>(color.b) / 255.0f, 2.2f)};
}

Matrix pt_matrix_no_translation(Matrix matrix) {
    matrix.m12 = 0.0f;
    matrix.m13 = 0.0f;
    matrix.m14 = 0.0f;
    return matrix;
}

std::vector<PtTriangle> build_playground_pt_scene(const PlaygroundLightingHarness& harness) {
    std::vector<PtTriangle> triangles{};
    const ol::WorldPos world_origin = test_pos(
        harness.app->dimension_id, {}, harness.dim->chunk_size_m);
    for (ol::u32 mesh_slot = 0; mesh_slot < harness.dim->meshes.count; ++mesh_slot) {
        const ol::MeshInstance& mesh = harness.dim->meshes.data[mesh_slot];
        if (!mesh.visible) continue;
        const ol::MeshGeometry* geometry = ol::arena_get(&harness.dim->geometries, mesh.geometry);
        if (!geometry) continue;
        const Vector3 origin = ol::world_delta_meters(
            mesh.origin, world_origin, harness.dim->chunk_size_m);
        const Matrix basis = pt_matrix_no_translation(mesh.se3);
        std::array<Vector3, ol::max_vertices_per_geometry> vertices{};
        for (ol::u32 vertex = 0; vertex < geometry->vertex_count; ++vertex) {
            vertices[vertex] = Vector3Transform(geometry->vertices[vertex], basis) + origin;
        }
        const Vector3 albedo = pt_linear_color(ol::resolve_mesh_color(&mesh));
        const Vector3 emission = ol::resolve_mesh_emission(&mesh);
        const float reflectivity = ol::resolve_mesh_reflectivity(&mesh);
        for (ol::u32 triangle_index = 0; triangle_index < geometry->triangle_count; ++triangle_index) {
            const ol::Triangle source = geometry->triangles[triangle_index];
            const Vector3 a = vertices[source.a];
            const Vector3 b = vertices[source.b];
            const Vector3 c = vertices[source.c];
            if (ol::safe_len(Vector3CrossProduct(b - a, c - a)) <= 0.000001f) continue;
            triangles.push_back({a, b, c, albedo, emission, reflectivity});
        }
    }
    return triangles;
}

PtHit trace_pt_scene(
    const std::vector<PtTriangle>& triangles,
    Vector3 origin,
    Vector3 direction,
    float maximum_distance = 100.0f) {
    constexpr float epsilon = 0.002f;
    PtHit best{};
    best.t = maximum_distance;
    for (const PtTriangle& triangle : triangles) {
        const Vector3 edge1 = triangle.b - triangle.a;
        const Vector3 edge2 = triangle.c - triangle.a;
        const Vector3 p = Vector3CrossProduct(direction, edge2);
        const float determinant = Vector3DotProduct(edge1, p);
        if (std::fabs(determinant) < 0.000001f) continue;
        const float inverse_determinant = 1.0f / determinant;
        const Vector3 s = origin - triangle.a;
        const float u = Vector3DotProduct(s, p) * inverse_determinant;
        if (u < -0.00001f || u > 1.00001f) continue;
        const Vector3 q = Vector3CrossProduct(s, edge1);
        const float v = Vector3DotProduct(direction, q) * inverse_determinant;
        if (v < -0.00001f || u + v > 1.00001f) continue;
        const float t = Vector3DotProduct(edge2, q) * inverse_determinant;
        if (t <= epsilon || t >= best.t) continue;
        Vector3 normal = Vector3Normalize(Vector3CrossProduct(edge1, edge2));
        const bool front_facing = Vector3DotProduct(normal, direction) < 0.0f;
        if (!front_facing) normal = normal * -1.0f;
        best.exists = true;
        best.front_facing = front_facing;
        best.t = t;
        best.position = origin + direction * t;
        best.normal = normal;
        best.albedo = triangle.albedo;
        best.emission = triangle.emission;
        best.reflectivity = triangle.reflectivity;
    }
    return best;
}

ol::u64 pt_random_u64(ol::u64* state) {
    ol::u64 value = (*state += 0x9e3779b97f4a7c15ull);
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

float pt_random_float(ol::u64* state) {
    return static_cast<float>((pt_random_u64(state) >> 40u) & 0xffffffu) /
        static_cast<float>(0x1000000u);
}

void pt_basis(Vector3 normal, Vector3* tangent, Vector3* bitangent) {
    const Vector3 up = std::fabs(normal.z) < 0.999f
        ? Vector3{0.0f, 0.0f, 1.0f}
        : Vector3{0.0f, 1.0f, 0.0f};
    *tangent = Vector3Normalize(Vector3CrossProduct(up, normal));
    *bitangent = Vector3CrossProduct(normal, *tangent);
}

Vector3 pt_cosine_hemisphere(Vector3 normal, ol::u64* random_state) {
    const float u0 = pt_random_float(random_state);
    const float u1 = pt_random_float(random_state);
    const float radius = std::sqrt(u0);
    const float angle = 2.0f * PI * u1;
    const Vector3 local = {
        radius * std::cos(angle),
        radius * std::sin(angle),
        std::sqrt(fmaxf(0.0f, 1.0f - u0))};
    Vector3 tangent{}, bitangent{};
    pt_basis(normal, &tangent, &bitangent);
    return Vector3Normalize(
        tangent * local.x + bitangent * local.y + normal * local.z);
}

Vector3 pt_environment(const ol::Dimension* dim, Vector3 direction) {
    if (direction.y <= 0.0f) return {};
    const Vector3 bottom = pt_linear_color(dim->sky_bottom) * 0.55f;
    const Vector3 top = pt_linear_color(dim->sky_top) * 0.55f;
    const float height = std::sqrt(ol::clampf(direction.y, 0.0f, 1.0f));
    return Vector3Lerp(bottom, top, height);
}

Vector3 trace_pt_sample(
    const std::vector<PtTriangle>& triangles,
    const ol::Dimension* dim,
    Vector3 surface_position,
    Vector3 surface_normal,
    Vector3 primary_albedo,
    ol::u64 random_seed) {
    constexpr float epsilon = 0.002f;
    constexpr float emission_scale = 24.0f;
    Vector3 radiance{};
    Vector3 throughput = primary_albedo;
    Vector3 origin = surface_position + surface_normal * (4.0f * epsilon);
    Vector3 direction = pt_cosine_hemisphere(surface_normal, &random_seed);
    for (int depth = 0; depth < 6; ++depth) {
        const PtHit hit = trace_pt_scene(triangles, origin, direction);
        if (!hit.exists) {
            radiance = radiance + pt_component_multiply(
                throughput, pt_environment(dim, direction));
            break;
        }
        if (!hit.front_facing) break;
        if (ol::safe_len(hit.emission) > 0.0001f) {
            radiance = radiance + pt_component_multiply(
                throughput, hit.emission * emission_scale);
            break;
        }
        throughput = pt_component_multiply(throughput, hit.albedo);
        if (hit.reflectivity > 0.5f) {
            throughput = throughput * hit.reflectivity;
            direction = Vector3Normalize(Vector3Reflect(direction, hit.normal));
        } else {
            direction = pt_cosine_hemisphere(hit.normal, &random_seed);
        }
        origin = hit.position + hit.normal * (4.0f * epsilon);
    }
    return radiance;
}

float pt_aces_channel(float value) {
    value = fmaxf(0.0f, value);
    return ol::clampf(
        value * (2.51f * value + 0.03f) /
            (value * (2.43f * value + 0.59f) + 0.14f),
        0.0f, 1.0f);
}

Vector3 pt_display_color(Vector3 linear) {
    return {
        std::pow(pt_aces_channel(linear.x), 1.0f / 2.2f),
        std::pow(pt_aces_channel(linear.y), 1.0f / 2.2f),
        std::pow(pt_aces_channel(linear.z), 1.0f / 2.2f)};
}

Texture2D pt_render_texture(const ol::RenderState& renderer, ol::u32 texture_id) {
    switch (texture_id) {
        case ol::render_texture_life: return renderer.life_texture;
        case ol::render_texture_cross: return renderer.cross_texture;
        case ol::render_texture_grid: return renderer.grid_texture;
        case ol::render_texture_grass: return renderer.grass_texture;
        case ol::render_texture_stone: return renderer.stone_texture;
        case ol::render_texture_roof: return renderer.roof_texture;
        default: return renderer.white_texture;
    }
}

int pt_wrapped_index(int value, int size) {
    return size > 0 ? (value % size + size) % size : 0;
}

const ol::MeshInstance* pt_find_mesh(
    const ol::Dimension* dim,
    const char* name,
    ol::u32* out_mesh_id) {
    for (ol::u32 mesh_slot = 0; mesh_slot < dim->meshes.count; ++mesh_slot) {
        if (std::strcmp(dim->meshes.data[mesh_slot].name, name) != 0) continue;
        if (out_mesh_id) *out_mesh_id = ol::arena_id_at_slot(&dim->meshes, mesh_slot);
        return &dim->meshes.data[mesh_slot];
    }
    return nullptr;
}

struct PtSurfaceTexel {
    Vector3 world_position{};
    Vector3 normal{};
    Vector3 base_albedo{};
    Vector3 rc_display{};
    float reflectivity = 0.0f;
};

bool read_pt_surface_texel(
    const PlaygroundLightingHarness& harness,
    const char* mesh_name,
    Vector3 expected_normal,
    Vector3 requested_world_position,
    PtSurfaceTexel* out_texel) {
    ol::u32 mesh_id = ol::invalid_id;
    const ol::MeshInstance* mesh = pt_find_mesh(harness.dim, mesh_name, &mesh_id);
    if (!mesh) return false;
    const ol::MeshLightingSurfaceCache* surface =
        find_lighting_surface(harness.renderer, mesh_id, expected_normal);
    if (!surface || surface->resolved_scene_signature != harness.renderer.radiance_scene_signature ||
        !IsRenderTextureValid(surface->texture)) return false;
    const ol::WorldPos world_origin = test_pos(
        harness.app->dimension_id, {}, harness.dim->chunk_size_m);
    const Vector3 mesh_origin = ol::world_delta_meters(
        mesh->origin, world_origin, harness.dim->chunk_size_m);
    const Vector3 local_requested = requested_world_position - mesh_origin;
    const float ppm = fmaxf(harness.dim->pixels_per_meter, 1.0f);
    const float requested_u = Vector3DotProduct(local_requested, surface->tangent) * ppm;
    const float requested_v = Vector3DotProduct(local_requested, surface->bitangent) * ppm;
    const int pixel_x = static_cast<int>(std::floor(requested_u)) - surface->grid_min_u;
    const int pixel_y = static_cast<int>(std::floor(requested_v)) - surface->grid_min_v;
    if (pixel_x < 0 || pixel_y < 0 || pixel_x >= surface->width || pixel_y >= surface->height) return false;
    const float grid_u = static_cast<float>(surface->grid_min_u + pixel_x) + 0.5f;
    const float grid_v = static_cast<float>(surface->grid_min_v + pixel_y) + 0.5f;
    const Vector3 local_center =
        surface->tangent * (grid_u / ppm) +
        surface->bitangent * (grid_v / ppm) +
        surface->normal * surface->plane;

    Image rc_image = LoadImageFromTexture(surface->texture.texture);
    Color* rc_pixels = IsImageValid(rc_image) ? LoadImageColors(rc_image) : nullptr;
    if (!rc_pixels) {
        if (IsImageValid(rc_image)) UnloadImage(rc_image);
        return false;
    }
    const Color rc_color = rc_pixels[pixel_y * rc_image.width + pixel_x];
    UnloadImageColors(rc_pixels);
    UnloadImage(rc_image);

    const Texture2D base_texture = pt_render_texture(harness.renderer, mesh->texture_id);
    Image base_image = LoadImageFromTexture(base_texture);
    Color* base_pixels = IsImageValid(base_image) ? LoadImageColors(base_image) : nullptr;
    if (!base_pixels) {
        if (IsImageValid(base_image)) UnloadImage(base_image);
        return false;
    }
    const int base_x = pt_wrapped_index(
        static_cast<int>(std::floor(0.5f * static_cast<float>(base_image.width) + grid_u)),
        base_image.width);
    const int base_y = pt_wrapped_index(
        static_cast<int>(std::floor(0.5f * static_cast<float>(base_image.height) + grid_v)),
        base_image.height);
    const Color base_color = base_pixels[base_y * base_image.width + base_x];
    UnloadImageColors(base_pixels);
    UnloadImage(base_image);
    const Color tint_color = ol::resolve_mesh_color(mesh);
    const Vector3 tinted_srgb = {
        static_cast<float>(base_color.r) / 255.0f * static_cast<float>(tint_color.r) / 255.0f,
        static_cast<float>(base_color.g) / 255.0f * static_cast<float>(tint_color.g) / 255.0f,
        static_cast<float>(base_color.b) / 255.0f * static_cast<float>(tint_color.b) / 255.0f};
    out_texel->world_position = mesh_origin + local_center;
    out_texel->normal = surface->normal;
    out_texel->base_albedo = {
        std::pow(tinted_srgb.x, 2.2f),
        std::pow(tinted_srgb.y, 2.2f),
        std::pow(tinted_srgb.z, 2.2f)};
    out_texel->rc_display = {
        static_cast<float>(rc_color.r) / 255.0f,
        static_cast<float>(rc_color.g) / 255.0f,
        static_cast<float>(rc_color.b) / 255.0f};
    out_texel->reflectivity = ol::resolve_mesh_reflectivity(mesh);
    return true;
}

struct PtSampleDefinition {
    const char* name = "";
    const char* mesh_name = "";
    Vector3 normal{};
    Vector3 world_position{};
};

void warm_pt_surface(
    PlaygroundLightingHarness* harness,
    const PtSampleDefinition& sample,
    Vector3 camera_feet) {
    const Vector3 camera = camera_feet + Vector3{0.0f, 2.0f, 0.0f};
    const Vector3 delta = sample.world_position - camera;
    const float length = fmaxf(ol::safe_len(delta), 0.0001f);
    const Vector3 direction = delta / length;
    ol::CameraView view{};
    view.anchor = test_pos(
        harness->app->dimension_id, camera_feet, harness->dim->chunk_size_m);
    view.eye_height = 2.0f;
    view.yaw = std::atan2(direction.x, -direction.z);
    view.pitch = std::asin(ol::clampf(direction.y, -1.0f, 1.0f));
    for (int frame = 0; frame < 6; ++frame) {
        ol::renderer_render_dimension_to_target(
            &harness->renderer, harness->dim, view, harness->app->local_player_id);
    }
}

bool run_playground_pathtrace_reference_test() {
    PlaygroundLightingHarness harness{};
    if (!init_playground_lighting_harness(&harness, "pathtrace-reference")) return false;
    if (ol::PlayerEntity* player = ol::arena_get(
            &harness.dim->players, harness.app->local_player_id)) {
        player->connected = false;
    }
    constexpr std::array<PtSampleDefinition, 5> sample_definitions = {{
        {"floor-center", "floor", {0.0f, 1.0f, 0.0f}, {2.03125f, 0.0f, 2.03125f}},
        {"floor-warm", "floor", {0.0f, 1.0f, 0.0f}, {0.03125f, 0.0f, -5.03125f}},
        {"floor-blue", "floor", {0.0f, 1.0f, 0.0f}, {-8.03125f, 0.0f, 8.03125f}},
        {"north-wall", "north wall", {0.0f, 0.0f, 1.0f}, {0.03125f, 2.03125f, -18.0f}},
        {"west-wall", "west wall", {1.0f, 0.0f, 0.0f}, {-18.0f, 2.03125f, 0.03125f}},
    }};
    const Vector3 camera_feet = {4.0f, 0.0f, 4.0f};
    for (const PtSampleDefinition& sample : sample_definitions) {
        warm_pt_surface(&harness, sample, camera_feet);
    }
    const std::vector<PtTriangle> scene = build_playground_pt_scene(harness);
    constexpr int path_count = 16384;
    double total_error = 0.0;
    double maximum_error = 0.0;
    int compared_channels = 0;
    int valid_samples = 0;
    for (size_t sample_index = 0; sample_index < sample_definitions.size(); ++sample_index) {
        const PtSampleDefinition& definition = sample_definitions[sample_index];
        PtSurfaceTexel texel{};
        if (!read_pt_surface_texel(
                harness, definition.mesh_name, definition.normal,
                definition.world_position, &texel)) {
            std::printf("path reference %-12s unavailable\n", definition.name);
            continue;
        }
        Vector3 reference_linear{};
        for (int path = 0; path < path_count; ++path) {
            ol::u64 seed = 0x51f15e5dull ^
                (static_cast<ol::u64>(sample_index + 1) * 0x9e3779b97f4a7c15ull) ^
                (static_cast<ol::u64>(path + 1) * 0xd1b54a32d192ed03ull);
            reference_linear = reference_linear + trace_pt_sample(
                scene, harness.dim, texel.world_position, texel.normal,
                texel.base_albedo, seed);
        }
        reference_linear = reference_linear / static_cast<float>(path_count);
        reference_linear = reference_linear +
            texel.base_albedo * (harness.dim->ambient * 0.08f);
        const Vector3 reference_display = pt_display_color(reference_linear);
        const Vector3 error = {
            std::fabs(reference_display.x - texel.rc_display.x),
            std::fabs(reference_display.y - texel.rc_display.y),
            std::fabs(reference_display.z - texel.rc_display.z)};
        total_error += static_cast<double>(error.x + error.y + error.z);
        maximum_error = std::max(
            maximum_error,
            static_cast<double>(std::max(error.x, std::max(error.y, error.z))));
        compared_channels += 3;
        ++valid_samples;
        std::printf(
            "path reference %-12s world=(%.4f %.4f %.4f) "
            "rc=(%.3f %.3f %.3f) pt=(%.3f %.3f %.3f) max_error=%.3f\n",
            definition.name,
            texel.world_position.x, texel.world_position.y, texel.world_position.z,
            texel.rc_display.x, texel.rc_display.y, texel.rc_display.z,
            reference_display.x, reference_display.y, reference_display.z,
            std::max(error.x, std::max(error.y, error.z)));
    }
    const double mean_error = compared_channels > 0
        ? total_error / static_cast<double>(compared_channels)
        : std::numeric_limits<double>::infinity();
    std::printf(
        "path reference summary triangles=%zu texels=%d paths_per_texel=%d "
        "mean_abs=%.4f max_abs=%.4f\n",
        scene.size(), valid_samples, path_count, mean_error, maximum_error);
    const bool ok = valid_samples == static_cast<int>(sample_definitions.size()) &&
        std::isfinite(mean_error) && mean_error <= 0.04 && maximum_error <= 0.10;
    shutdown_playground_lighting_harness(&harness);
    return expect_true(ok, "Radiance cascades stay within the path-traced surface-texel error bound");
}

Vector3 trace_pt_camera_sample(
    const std::vector<PtTriangle>& triangles,
    const ol::Dimension* dim,
    Vector3 origin,
    Vector3 direction,
    ol::u64 random_seed) {
    constexpr float epsilon = 0.002f;
    constexpr float emission_scale = 24.0f;
    Vector3 radiance{};
    Vector3 throughput = {1.0f, 1.0f, 1.0f};
    for (int depth = 0; depth < 6; ++depth) {
        const PtHit hit = trace_pt_scene(triangles, origin, direction);
        if (!hit.exists) {
            radiance = radiance + pt_component_multiply(
                throughput, pt_environment(dim, direction));
            break;
        }
        if (!hit.front_facing) break;
        if (ol::safe_len(hit.emission) > 0.0001f) {
            radiance = radiance + pt_component_multiply(
                throughput, hit.emission * emission_scale);
            break;
        }
        throughput = pt_component_multiply(throughput, hit.albedo);
        if (hit.reflectivity > 0.5f) {
            throughput = throughput * hit.reflectivity;
            direction = Vector3Normalize(Vector3Reflect(direction, hit.normal));
        } else {
            direction = pt_cosine_hemisphere(hit.normal, &random_seed);
        }
        origin = hit.position + hit.normal * (4.0f * epsilon);
    }
    return radiance;
}

bool init_cave_lighting_harness(
    PlaygroundLightingHarness* harness,
    const char* session) {
    if (!harness) return false;
    ol::renderer_init(&harness->renderer);
    harness->app = std::make_unique<ol::DemoApp>();
    std::snprintf(harness->app->world_name, sizeof(harness->app->world_name), "cave");
    std::snprintf(harness->app->session_name, sizeof(harness->app->session_name), "%s", session);
    ol::demo_generate_world(harness->app.get());
    harness->dim = ol::world_get_dimension(&harness->app->world, harness->app->dimension_id);
    if (!harness->dim) return false;
    harness->renderer.lighting.temporal_frames = 0;
    harness->renderer.lighting.jitter = false;
    harness->renderer.lighting.corner_merge = false;
    harness->renderer.depth_test_edges = false;
    harness->renderer.shader_depth_edges = false;
    harness->renderer.gpu_depth_edges = false;
    ol::renderer_ensure_target(&harness->renderer);
    return true;
}

struct CaveSeamTexelCapture {
    bool valid = false;
    ol::ChunkCoord anchor{};
    Vector3 display{};
    int valid_mip_level = -1;
};

bool run_cave_anchor_seam_regression_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "ol cave anchor seam regression");

    PlaygroundLightingHarness harness{};
    bool ok = init_cave_lighting_harness(&harness, "cave-anchor-seam");
    if (!ok) {
        shutdown_playground_lighting_harness(&harness);
        CloseWindow();
        return expect_true(false, "Cave anchor seam harness initializes");
    }
    if (ol::PlayerEntity* player = ol::arena_get(
            &harness.dim->players, harness.app->local_player_id)) {
        player->connected = false;
    }

    // This texel stays fixed in world space while the camera crosses z=16 by
    // only two centimetres.  It lies well inside the old probe volume but near
    // the anchor-relative edge of the new one.  With frozen emitters, a diffuse
    // cache must therefore retain the same value in both world-snapped fields.
    const PtSampleDefinition sample = {
        "anchor-edge", "cave floor 1 1", {0.0f, 1.0f, 0.0f},
        {8.03125f, 0.0f, 0.03125f}};
    auto capture = [&](float camera_z) {
        CaveSeamTexelCapture result{};
        const Vector3 camera_feet = {8.0f, 0.0f, camera_z};
        warm_pt_surface(&harness, sample, camera_feet);
        result.anchor = test_pos(
            harness.app->dimension_id, camera_feet,
            harness.dim->chunk_size_m).chunk;
        PtSurfaceTexel texel{};
        result.valid = read_pt_surface_texel(
            harness, sample.mesh_name, sample.normal,
            sample.world_position, &texel);
        result.display = texel.rc_display;
        ol::u32 mesh_id = ol::invalid_id;
        if (pt_find_mesh(harness.dim, sample.mesh_name, &mesh_id)) {
            const ol::MeshLightingSurfaceCache* surface =
                find_lighting_surface(harness.renderer, mesh_id, sample.normal);
            if (surface) result.valid_mip_level = surface->valid_mip_level;
        }
        return result;
    };
    auto maximum_byte_delta = [](Vector3 a, Vector3 b) {
        const float delta = std::max(
            std::fabs(a.x - b.x),
            std::max(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
        return static_cast<int>(std::round(delta * 255.0f));
    };

    const CaveSeamTexelCapture before = capture(15.99f);
    const CaveSeamTexelCapture after = capture(16.01f);
    const CaveSeamTexelCapture returned = capture(15.99f);
    const int crossing_delta = maximum_byte_delta(before.display, after.display);
    const int return_delta = maximum_byte_delta(before.display, returned.display);
    std::printf(
        "cave anchor seam: anchors=%d->%d->%d mip=%d/%d/%d "
        "rgb=(%.4f %.4f %.4f)->(%.4f %.4f %.4f)->(%.4f %.4f %.4f) "
        "cross_delta=%d return_delta=%d\n",
        before.anchor.z, after.anchor.z, returned.anchor.z,
        before.valid_mip_level, after.valid_mip_level, returned.valid_mip_level,
        before.display.x, before.display.y, before.display.z,
        after.display.x, after.display.y, after.display.z,
        returned.display.x, returned.display.y, returned.display.z,
        crossing_delta, return_delta);

    ok = before.valid && after.valid && returned.valid &&
        before.anchor.z == 0 && after.anchor.z == 1 && returned.anchor.z == 0 &&
        before.valid_mip_level == 0 && after.valid_mip_level == 0 &&
        returned.valid_mip_level == 0 && crossing_delta <= 2 && return_delta <= 2;
    shutdown_playground_lighting_harness(&harness);
    CloseWindow();
    return expect_true(
        ok,
        "Frozen cave lighting keeps a fixed diffuse texel stable across an anchor seam");
}

struct CavePtSampleDefinition {
    PtSampleDefinition surface{};
    Vector3 camera_position{};
    const char* transport = "";
    double maximum_channel_error = 0.60;
};

bool run_cave_pathtrace_reference_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "ol cave path-trace reference");

    PlaygroundLightingHarness harness{};
    bool ok = init_cave_lighting_harness(&harness, "cave-pathtrace-reference");
    if (!ok) {
        shutdown_playground_lighting_harness(&harness);
        CloseWindow();
        return expect_true(false, "Cave path-trace harness initializes");
    }
    if (ol::PlayerEntity* player = ol::arena_get(
            &harness.dim->players, harness.app->local_player_id)) {
        player->connected = false;
    }

    // All cameras stay in the same lighting anchor. The mirror camera is the
    // geometric reflection of the cyan emitter across the sampled mirror texel,
    // so its first reflected path must terminate on emissive geometry.
    constexpr Vector3 common_camera = {14.0f, 3.0625f, 12.0f};
    constexpr Vector3 mirror_camera = {14.0f, 3.0625f, 8.0625f};
    constexpr std::array<CavePtSampleDefinition, 4> sample_definitions = {{
        {{"warm-direct", "cave floor 1 2", {0.0f, 1.0f, 0.0f}, {2.03125f, 0.0f, 20.03125f}},
            common_camera, "direct emissive", 0.05},
        {{"arch-shadow", "cave floor 1 1", {0.0f, 1.0f, 0.0f}, {6.03125f, 0.0f, 7.53125f}},
            common_camera, "shadow / indirect", 0.05},
        {{"ceiling", "cave ceiling 1 1", {0.0f, -1.0f, 0.0f}, {8.03125f, 10.0f, 12.03125f}},
            common_camera, "ordinary diffuse", 0.05},
        {{"mirror-cyan", "cave mirror panel", {-1.0f, 0.0f, 0.0f}, {31.8675f, 4.03125f, 8.03125f}},
            mirror_camera, "camera-ray mirror", 0.03},
    }};

    // Resolve diffuse faces first and the view-dependent mirror last, preserving
    // the exact mirror camera signature while retaining the static face caches.
    for (const CavePtSampleDefinition& sample : sample_definitions) {
        warm_pt_surface(
            &harness, sample.surface,
            sample.camera_position - Vector3{0.0f, 2.0f, 0.0f});
    }

    const std::vector<PtTriangle> scene = build_playground_pt_scene(harness);
    constexpr int path_count = 16384;
    double total_error = 0.0;
    double maximum_error = 0.0;
    int compared_channels = 0;
    int valid_samples = 0;
    bool mirror_semantics_valid = false;
    std::array<float, sample_definitions.size()> rc_luminance{};
    std::array<float, sample_definitions.size()> pt_luminance{};
    for (size_t sample_index = 0; sample_index < sample_definitions.size(); ++sample_index) {
        const CavePtSampleDefinition& definition = sample_definitions[sample_index];
        PtSurfaceTexel texel{};
        if (!read_pt_surface_texel(
                harness, definition.surface.mesh_name, definition.surface.normal,
                definition.surface.world_position, &texel)) {
            std::printf("cave path reference %-12s unavailable\n", definition.surface.name);
            continue;
        }

        Vector3 diffuse_linear{};
        Vector3 reflection_linear{};
        Vector3 reflected_direction{};
        if (texel.reflectivity > 0.001f) {
            const Vector3 incoming = Vector3Normalize(
                texel.world_position - definition.camera_position);
            reflected_direction = Vector3Normalize(Vector3Reflect(incoming, texel.normal));
            const PtHit first_reflection_hit = trace_pt_scene(
                scene, texel.world_position + texel.normal * 0.008f, reflected_direction);
            mirror_semantics_valid = mirror_semantics_valid ||
                (first_reflection_hit.exists && first_reflection_hit.front_facing &&
                 ol::safe_len(first_reflection_hit.emission) > 0.0001f &&
                 first_reflection_hit.emission.z > first_reflection_hit.emission.x);
        }
        for (int path = 0; path < path_count; ++path) {
            ol::u64 seed = 0xc6bc279692b5c323ull ^
                (static_cast<ol::u64>(sample_index + 1) * 0x9e3779b97f4a7c15ull) ^
                (static_cast<ol::u64>(path + 1) * 0xd1b54a32d192ed03ull);
            diffuse_linear = diffuse_linear + trace_pt_sample(
                scene, harness.dim, texel.world_position, texel.normal,
                texel.base_albedo, seed);
            if (texel.reflectivity > 0.001f) {
                reflection_linear = reflection_linear + trace_pt_camera_sample(
                    scene, harness.dim, texel.world_position + texel.normal * 0.008f,
                    reflected_direction, seed ^ 0xa24baed4963ee407ull);
            }
        }
        diffuse_linear = diffuse_linear / static_cast<float>(path_count) +
            texel.base_albedo * (harness.dim->ambient * 0.08f);
        Vector3 reference_linear = diffuse_linear;
        if (texel.reflectivity > 0.001f) {
            reflection_linear = reflection_linear / static_cast<float>(path_count);
            reference_linear = Vector3Lerp(
                diffuse_linear, reflection_linear,
                ol::clampf(texel.reflectivity, 0.0f, 1.0f));
        }
        const Vector3 reference_display = pt_display_color(reference_linear);
        const Vector3 error = {
            std::fabs(reference_display.x - texel.rc_display.x),
            std::fabs(reference_display.y - texel.rc_display.y),
            std::fabs(reference_display.z - texel.rc_display.z)};
        const double sample_maximum = std::max(
            static_cast<double>(error.x),
            static_cast<double>(std::max(error.y, error.z)));
        total_error += static_cast<double>(error.x + error.y + error.z);
        maximum_error = std::max(maximum_error, sample_maximum);
        compared_channels += 3;
        ++valid_samples;
        rc_luminance[sample_index] = Vector3DotProduct(
            texel.rc_display, Vector3{0.2126f, 0.7152f, 0.0722f});
        pt_luminance[sample_index] = Vector3DotProduct(
            reference_display, Vector3{0.2126f, 0.7152f, 0.0722f});
        ok = sample_maximum <= definition.maximum_channel_error && ok;
        std::printf(
            "cave path %-12s %-17s world=(%.4f %.4f %.4f) refl=%.2f "
            "rc=(%.3f %.3f %.3f) pt=(%.3f %.3f %.3f) max_error=%.3f\n",
            definition.surface.name, definition.transport,
            texel.world_position.x, texel.world_position.y, texel.world_position.z,
            texel.reflectivity,
            texel.rc_display.x, texel.rc_display.y, texel.rc_display.z,
            reference_display.x, reference_display.y, reference_display.z,
            sample_maximum);
    }
    const double mean_error = compared_channels > 0
        ? total_error / static_cast<double>(compared_channels)
        : std::numeric_limits<double>::infinity();
    const bool representative_transport =
        rc_luminance[0] > rc_luminance[1] + 0.02f &&
        pt_luminance[0] > pt_luminance[1] + 0.02f;
    std::printf(
        "cave path summary triangles=%zu texels=%d paths_per_texel=%d "
        "mean_abs=%.4f max_abs=%.4f mirror_ray=%s direct_over_shadow=%s\n",
        scene.size(), valid_samples, path_count, mean_error, maximum_error,
        mirror_semantics_valid ? "emissive" : "invalid",
        representative_transport ? "yes" : "no");
    ok = valid_samples == static_cast<int>(sample_definitions.size()) &&
        mirror_semantics_valid && representative_transport &&
        std::isfinite(mean_error) && mean_error <= 0.02 && maximum_error <= 0.05 && ok;

    shutdown_playground_lighting_harness(&harness);

    CloseWindow();
    return expect_true(
        ok,
        "Cave RC world texels stay close to deterministic path tracing across direct, shadowed, diffuse, and mirror paths");
}

struct ContinuousLightingCapture {
    bool valid = false;
    ol::ChunkCoord anchor{};
    ol::u64 scene_signature = 0;
    ol::u64 texture_hash = 0;
    int width = 0;
    int height = 0;
    std::array<Color, 9> texels{};
    std::vector<Color> pixels{};
};

ContinuousLightingCapture capture_playground_floor_lighting(
    const PlaygroundLightingHarness& harness,
    Vector3 camera_feet,
    ol::ChunkCoord chunk_offset = {}) {
    ContinuousLightingCapture capture{};
    ol::WorldPos camera_anchor = test_pos(
        harness.app->dimension_id, camera_feet, harness.dim->chunk_size_m);
    camera_anchor.chunk = ol::chunk_add(camera_anchor.chunk, chunk_offset);
    capture.anchor = camera_anchor.chunk;
    capture.scene_signature = harness.renderer.radiance_scene_signature;
    ol::u32 mesh_id = ol::invalid_id;
    const ol::MeshInstance* mesh = pt_find_mesh(harness.dim, "floor", &mesh_id);
    const ol::MeshLightingSurfaceCache* surface = mesh
        ? find_lighting_surface(harness.renderer, mesh_id, {0.0f, 1.0f, 0.0f})
        : nullptr;
    if (!surface || surface->resolved_scene_signature != capture.scene_signature ||
        !IsRenderTextureValid(surface->texture)) return capture;
    Image image = LoadImageFromTexture(surface->texture.texture);
    Color* pixels = IsImageValid(image) ? LoadImageColors(image) : nullptr;
    if (!pixels) {
        if (IsImageValid(image)) UnloadImage(image);
        return capture;
    }
    if (surface->dynamic_composite_active &&
        IsRenderTextureValid(surface->shadow_mask_texture)) {
        Image mask_image = LoadImageFromTexture(surface->shadow_mask_texture.texture);
        Color* mask_pixels = IsImageValid(mask_image) ? LoadImageColors(mask_image) : nullptr;
        if (mask_pixels && mask_image.width == image.width && mask_image.height == image.height) {
            const size_t pixel_count =
                static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
            for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
                const unsigned int multiplier = mask_pixels[pixel_index].r;
                pixels[pixel_index].r = static_cast<unsigned char>(
                    (static_cast<unsigned int>(pixels[pixel_index].r) * multiplier + 127u) / 255u);
                pixels[pixel_index].g = static_cast<unsigned char>(
                    (static_cast<unsigned int>(pixels[pixel_index].g) * multiplier + 127u) / 255u);
                pixels[pixel_index].b = static_cast<unsigned char>(
                    (static_cast<unsigned int>(pixels[pixel_index].b) * multiplier + 127u) / 255u);
            }
        }
        if (mask_pixels) UnloadImageColors(mask_pixels);
        if (IsImageValid(mask_image)) UnloadImage(mask_image);
    }
    constexpr std::array<Vector3, 9> positions = {{
        {-12.03125f, 0.0f, -12.03125f}, {-8.03125f, 0.0f, 8.03125f},
        {-4.03125f, 0.0f, -2.03125f}, {0.03125f, 0.0f, -5.03125f},
        {0.03125f, 0.0f, 0.03125f}, {4.03125f, 0.0f, -4.03125f},
        {8.03125f, 0.0f, 8.03125f}, {12.03125f, 0.0f, -8.03125f},
        {16.03125f, 0.0f, 16.03125f},
    }};
    const float ppm = fmaxf(harness.dim->pixels_per_meter, 1.0f);
    capture.valid = true;
    capture.width = image.width;
    capture.height = image.height;
    capture.pixels.assign(
        pixels, pixels + static_cast<size_t>(image.width) * static_cast<size_t>(image.height));
    for (size_t index = 0; index < positions.size(); ++index) {
        // Playground floor samples are authored relative to the floor centre;
        // keeping them mesh-local also lets the same regression run at very
        // large chunk coordinates without converting them through a float.
        const Vector3 local = positions[index];
        const int x = static_cast<int>(std::floor(
            Vector3DotProduct(local, surface->tangent) * ppm)) - surface->grid_min_u;
        const int y = static_cast<int>(std::floor(
            Vector3DotProduct(local, surface->bitangent) * ppm)) - surface->grid_min_v;
        if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
            capture.valid = false;
            break;
        }
        capture.texels[index] = pixels[y * image.width + x];
    }
    capture.texture_hash = 1469598103934665603ull;
    for (Color color : capture.pixels) {
        capture.texture_hash ^= ColorToInt(color);
        capture.texture_hash *= 1099511628211ull;
    }
    UnloadImageColors(pixels);
    UnloadImage(image);
    return capture;
}

struct ContinuousLightingStats {
    int frames = 0;
    int valid_frames = 0;
    int anchor_changes = 0;
    int promotion_frames = 0;
    int changed_frames = 0;
    int changed_without_anchor = 0;
    int maximum_channel_delta = 0;
    int maximum_anchor_delta = 0;
    int maximum_non_anchor_delta = 0;
    int maximum_anchor_full_delta = 0;
    double maximum_anchor_changed_percent = 0.0;
    double maximum_anchor_mean_abs = 0.0;
};

int continuous_capture_delta(
    const ContinuousLightingCapture& a,
    const ContinuousLightingCapture& b) {
    if (!a.valid || !b.valid) return 255;
    int maximum = 0;
    for (size_t index = 0; index < a.texels.size(); ++index) {
        const Color x = a.texels[index];
        const Color y = b.texels[index];
        maximum = std::max(maximum, std::abs(static_cast<int>(x.r) - static_cast<int>(y.r)));
        maximum = std::max(maximum, std::abs(static_cast<int>(x.g) - static_cast<int>(y.g)));
        maximum = std::max(maximum, std::abs(static_cast<int>(x.b) - static_cast<int>(y.b)));
    }
    return maximum;
}

void offset_playground_render_chunks(
    PlaygroundLightingHarness* harness,
    ol::ChunkCoord offset) {
    if (!harness || !harness->dim) return;
    harness->dim->chunk_lookup.clear();
    for (ol::u32 slot = 0; slot < harness->dim->chunks.count; ++slot) {
        ol::Chunk& chunk = harness->dim->chunks.data[slot];
        chunk.coord = ol::chunk_add(chunk.coord, offset);
        harness->dim->chunk_lookup[chunk.coord] =
            ol::arena_id_at_slot(&harness->dim->chunks, slot);
    }
    for (ol::u32 slot = 0; slot < harness->dim->meshes.count; ++slot) {
        harness->dim->meshes.data[slot].origin.chunk = ol::chunk_add(
            harness->dim->meshes.data[slot].origin.chunk, offset);
    }
    for (ol::u32 slot = 0; slot < harness->dim->sprites.count; ++slot) {
        harness->dim->sprites.data[slot].origin.chunk = ol::chunk_add(
            harness->dim->sprites.data[slot].origin.chunk, offset);
    }
    ++harness->dim->mesh_topology_revision;
}

ImageDiffStats continuous_capture_image_diff(
    const ContinuousLightingCapture& a,
    const ContinuousLightingCapture& b) {
    ImageDiffStats stats{};
    if (!a.valid || !b.valid || a.width != b.width || a.height != b.height ||
        a.pixels.size() != b.pixels.size()) {
        stats.changed_pixels = std::numeric_limits<ol::u64>::max();
        stats.mean_abs_rgb = std::numeric_limits<double>::infinity();
        stats.max_delta = 255;
        return stats;
    }
    double total = 0.0;
    for (size_t index = 0; index < a.pixels.size(); ++index) {
        const Color x = a.pixels[index];
        const Color y = b.pixels[index];
        const int dr = std::abs(static_cast<int>(x.r) - static_cast<int>(y.r));
        const int dg = std::abs(static_cast<int>(x.g) - static_cast<int>(y.g));
        const int db = std::abs(static_cast<int>(x.b) - static_cast<int>(y.b));
        const int maximum = std::max(dr, std::max(dg, db));
        if (maximum > 0) ++stats.changed_pixels;
        stats.max_delta = std::max(stats.max_delta, maximum);
        total += static_cast<double>(dr + dg + db);
    }
    stats.mean_abs_rgb = a.pixels.empty()
        ? 0.0 : total / (3.0 * static_cast<double>(a.pixels.size()));
    return stats;
}

bool run_far_continuous_lighting_case(
    const char* name,
    ol::ChunkCoord chunk_offset) {
    PlaygroundLightingHarness harness{};
    bool ok = init_playground_lighting_harness(&harness, name);
    if (!ok) {
        shutdown_playground_lighting_harness(&harness);
        return false;
    }
    if (ol::PlayerEntity* player = ol::arena_get(
            &harness.dim->players, harness.app->local_player_id)) {
        player->connected = false;
    }
    offset_playground_render_chunks(&harness, chunk_offset);
    const std::array<std::pair<Vector3, Vector3>, 2> directions = {{
        {{-1.25f, 0.0f, 6.75f}, {1.25f, 0.0f, 9.25f}},
        {{1.25f, 0.0f, 9.25f}, {-1.25f, 0.0f, 6.75f}},
    }};
    for (size_t direction_index = 0; direction_index < directions.size(); ++direction_index) {
        ContinuousLightingCapture previous{};
        int anchor_changes = 0;
        int maximum_delta = 0;
        double maximum_changed_percent = 0.0;
        double maximum_mean_abs = 0.0;
        constexpr int frames = 101;
        for (int frame = 0; frame < frames; ++frame) {
            const float t = static_cast<float>(frame) / static_cast<float>(frames - 1);
            const Vector3 feet = Vector3Lerp(
                directions[direction_index].first,
                directions[direction_index].second, t);
            ol::CameraView view{};
            view.anchor = test_pos(
                harness.app->dimension_id, feet, harness.dim->chunk_size_m);
            view.anchor.chunk = ol::chunk_add(view.anchor.chunk, chunk_offset);
            view.eye_height = 1.7f;
            view.yaw = -0.2f + 0.4f * t;
            view.pitch = -0.38f;
            ol::renderer_render_dimension_to_target(
                &harness.renderer, harness.dim, view, harness.app->local_player_id);
            ContinuousLightingCapture current =
                capture_playground_floor_lighting(harness, feet, chunk_offset);
            ok = current.valid && ok;
            if (previous.valid && current.valid &&
                (previous.anchor.x != current.anchor.x ||
                 previous.anchor.y != current.anchor.y ||
                 previous.anchor.z != current.anchor.z)) {
                ++anchor_changes;
                maximum_delta = std::max(
                    maximum_delta, continuous_capture_delta(previous, current));
                const ImageDiffStats diff = continuous_capture_image_diff(previous, current);
                const double changed_percent = current.pixels.empty() ? 0.0 :
                    100.0 * static_cast<double>(diff.changed_pixels) /
                        static_cast<double>(current.pixels.size());
                maximum_changed_percent = std::max(maximum_changed_percent, changed_percent);
                maximum_mean_abs = std::max(maximum_mean_abs, diff.mean_abs_rgb);
            }
            previous = std::move(current);
        }
        std::printf(
            "continuous lighting far %-12s direction=%zu anchors=%d max=%d "
            "changed=%.3f%% mean=%.4f\n",
            name, direction_index, anchor_changes, maximum_delta,
            maximum_changed_percent, maximum_mean_abs);
        ok = anchor_changes == 1 && maximum_delta <= 1 &&
            maximum_changed_percent <= 0.10 && maximum_mean_abs <= 0.01 && ok;
    }
    shutdown_playground_lighting_harness(&harness);
    return ok;
}

bool run_lighting_continuous_motion_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "ol continuous lighting regression");
    PlaygroundLightingHarness harness{};
    bool ok = init_playground_lighting_harness(&harness, "continuous-lighting");
    if (!ok) {
        shutdown_playground_lighting_harness(&harness);
        CloseWindow();
        return expect_true(false, "Continuous lighting harness initializes");
    }
    ol::PlayerEntity* player = ol::arena_get(
        &harness.dim->players, harness.app->local_player_id);
    if (player) player->connected = false;

    struct MotionRoute {
        const char* name = "";
        Vector3 start{};
        Vector3 end{};
        float yaw_start = 0.0f;
        float yaw_end = 0.0f;
        int frames = 0;
    };
    const std::array<MotionRoute, 8> routes = {{
        {"origin-diagonal", {-1.25f, 0.0f, 6.75f}, {1.25f, 0.0f, 9.25f}, -0.20f, 0.20f, 101},
        {"origin-reverse", {1.25f, 0.0f, 9.25f}, {-1.25f, 0.0f, 6.75f}, 0.20f, -0.20f, 101},
        {"plus16-diagonal", {14.75f, 0.0f, 6.75f}, {17.25f, 0.0f, 9.25f}, -0.35f, 0.35f, 101},
        {"plus16-reverse", {17.25f, 0.0f, 9.25f}, {14.75f, 0.0f, 6.75f}, 0.35f, -0.35f, 101},
        {"minus16-diagonal", {-14.75f, 0.0f, 6.75f}, {-17.25f, 0.0f, 9.25f}, 0.35f, -0.35f, 101},
        {"minus16-reverse", {-17.25f, 0.0f, 9.25f}, {-14.75f, 0.0f, 6.75f}, -0.35f, 0.35f, 101},
        {"turn-in-place", {0.75f, 0.0f, 7.25f}, {0.75f, 0.0f, 7.25f}, -0.75f, 0.75f, 91},
        {"quality-fade", {31.0f, 0.0f, 8.0f}, {34.0f, 0.0f, 8.0f}, -1.30f, -1.30f, 121},
    }};
    for (const MotionRoute& route : routes) {
        ContinuousLightingStats stats{};
        ContinuousLightingCapture previous{};
        for (int frame = 0; frame < route.frames; ++frame) {
            const float t = route.frames > 1
                ? static_cast<float>(frame) / static_cast<float>(route.frames - 1)
                : 0.0f;
            const Vector3 feet = Vector3Lerp(route.start, route.end, t);
            const float yaw = route.yaw_start + (route.yaw_end - route.yaw_start) * t;
            place_lighting_test_player(&harness, feet);
            render_lighting_checkpoint(&harness, feet, yaw, -0.38f);
            ContinuousLightingCapture current =
                capture_playground_floor_lighting(harness, feet);
            ++stats.frames;
            if (current.valid) ++stats.valid_frames;
            if (harness.renderer.debug_radiance_neighbor_promoted) ++stats.promotion_frames;
            if (previous.valid && current.valid) {
                const bool anchor_changed = previous.anchor.x != current.anchor.x ||
                    previous.anchor.y != current.anchor.y || previous.anchor.z != current.anchor.z;
                const int delta = continuous_capture_delta(previous, current);
                if (anchor_changed) ++stats.anchor_changes;
                if (delta > 0) ++stats.changed_frames;
                if (!anchor_changed && delta > 0) ++stats.changed_without_anchor;
                stats.maximum_channel_delta = std::max(stats.maximum_channel_delta, delta);
                if (anchor_changed) stats.maximum_anchor_delta = std::max(stats.maximum_anchor_delta, delta);
                else stats.maximum_non_anchor_delta = std::max(stats.maximum_non_anchor_delta, delta);
                ImageDiffStats full_diff{};
                double full_changed_percent = 0.0;
                if (anchor_changed) {
                    full_diff = continuous_capture_image_diff(previous, current);
                    full_changed_percent = current.pixels.empty() ? 0.0 :
                        100.0 * static_cast<double>(full_diff.changed_pixels) /
                            static_cast<double>(current.pixels.size());
                    stats.maximum_anchor_changed_percent = std::max(
                        stats.maximum_anchor_changed_percent, full_changed_percent);
                    stats.maximum_anchor_mean_abs = std::max(
                        stats.maximum_anchor_mean_abs, full_diff.mean_abs_rgb);
                    stats.maximum_anchor_full_delta = std::max(
                        stats.maximum_anchor_full_delta, full_diff.max_delta);
                }
                if (delta > 2) {
                    std::printf(
                        "continuous lighting %-18s frame=%3d pos=(%.3f %.3f) "
                        "anchor=(%d,%d)->(%d,%d) delta=%d promoted=%d "
                        "full_changed=%.2f%% full_mean=%.3f full_max=%d hash=%016llx->%016llx\n",
                        route.name, frame, feet.x, feet.z,
                        previous.anchor.x, previous.anchor.z, current.anchor.x, current.anchor.z,
                        delta, harness.renderer.debug_radiance_neighbor_promoted ? 1 : 0,
                        full_changed_percent, full_diff.mean_abs_rgb, full_diff.max_delta,
                        static_cast<unsigned long long>(previous.texture_hash),
                        static_cast<unsigned long long>(current.texture_hash));
                    for (size_t sample = 0; sample < current.texels.size(); ++sample) {
                        const Color a = previous.texels[sample];
                        const Color b = current.texels[sample];
                        const int sample_delta = std::max(
                            std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                            std::max(
                                std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                                std::abs(static_cast<int>(a.b) - static_cast<int>(b.b))));
                        if (sample_delta > 1) {
                            std::printf(
                                "  floor sample %zu (%u,%u,%u)->(%u,%u,%u) delta=%d\n",
                                sample, a.r, a.g, a.b, b.r, b.g, b.b, sample_delta);
                        }
                    }
                }
            }
            previous = std::move(current);
        }
        std::printf(
            "continuous lighting summary %-18s frames=%d valid=%d anchors=%d promotions=%d "
            "changed=%d non_anchor_changed=%d max=%d anchor_max=%d non_anchor_max=%d "
            "anchor_changed=%.2f%% anchor_mean=%.3f anchor_full_max=%d\n",
            route.name, stats.frames, stats.valid_frames, stats.anchor_changes,
            stats.promotion_frames, stats.changed_frames, stats.changed_without_anchor,
            stats.maximum_channel_delta, stats.maximum_anchor_delta,
            stats.maximum_non_anchor_delta, stats.maximum_anchor_changed_percent,
            stats.maximum_anchor_mean_abs, stats.maximum_anchor_full_delta);
        ok = stats.valid_frames == stats.frames && stats.maximum_channel_delta <= 1 &&
            stats.maximum_anchor_changed_percent <= 0.10 &&
            stats.maximum_anchor_mean_abs <= 0.01 && ok;
    }
    shutdown_playground_lighting_harness(&harness);
    ok = run_far_continuous_lighting_case(
        "far-positive", {1000001, 0, -1000000}) && ok;
    ok = run_far_continuous_lighting_case(
        "far-negative", {-1000001, 0, 999998}) && ok;
    CloseWindow();
    return expect_true(ok, "Continuous playground motion keeps fixed world lighting texels stable between anchor transitions");
}

struct CaveRenderBenchmarkResult {
    double average_ms = 0.0;
    double p95_ms = 0.0;
    double maximum_ms = 0.0;
    double average_scene_build_ms = 0.0;
    double average_topology_ms = 0.0;
    double average_cascade_ms = 0.0;
    double average_surface_updates = 0.0;
    double average_surface_texels = 0.0;
    double average_common_uniform_updates = 0.0;
    double average_common_uniform_cache_hits = 0.0;
    double average_scene_buffer_binds = 0.0;
    int static_scene_build_frames = 0;
    int emitter_signature_changes = 0;
    int isolated_bright_components = 0;
    int saturated_reflection_texels = 0;
    ol::u32 static_triangles = 0;
    ol::u32 emitter_triangles = 0;
    ol::u32 emitter_meshes = 0;
};

ol::CameraView cave_benchmark_view(
    const PlaygroundLightingHarness& harness,
    Vector3 eye,
    Vector3 target) {
    const Vector3 direction = Vector3Normalize(target - eye);
    ol::CameraView view{};
    view.anchor = test_pos(
        harness.app->dimension_id, {eye.x, 0.0f, eye.z}, harness.dim->chunk_size_m);
    view.eye_height = eye.y;
    view.yaw = std::atan2(direction.x, -direction.z);
    view.pitch = std::asin(ol::clampf(direction.y, -1.0f, 1.0f));
    return view;
}

void set_cave_benchmark_emitters(PlaygroundLightingHarness* harness, float time_seconds) {
    const Vector3 offsets[] = {
        {std::sin(time_seconds * 0.73f) * 0.55f,
         std::sin(time_seconds * 0.91f) * 0.45f,
         std::cos(time_seconds * 0.57f) * 0.65f},
        {std::sin(time_seconds * 0.61f + 1.7f) * 0.40f,
         std::sin(time_seconds * 1.07f + 0.8f) * 0.65f,
         std::cos(time_seconds * 0.83f) * 0.55f},
        {std::sin(time_seconds * 0.49f + 3.1f) * 1.20f,
         std::sin(time_seconds * 0.79f + 2.4f) * 0.50f,
         std::cos(time_seconds * 0.67f + 1.2f) * 0.70f},
    };
    for (ol::u32 index = 0; index < harness->app->cave_emissive_mesh_ids.size(); ++index) {
        ol::MeshInstance* mesh = ol::arena_get(
            &harness->dim->meshes, harness->app->cave_emissive_mesh_ids[index]);
        if (!mesh) continue;
        mesh->origin = test_pos(
            harness->app->dimension_id,
            harness->app->cave_emissive_base_positions[index] + offsets[index],
            harness->dim->chunk_size_m);
    }
}

int small_bright_components(Texture2D texture, int* saturated_texels) {
    if (!IsTextureValid(texture)) return 0;
    Image image = LoadImageFromTexture(texture);
    Color* pixels = IsImageValid(image) ? LoadImageColors(image) : nullptr;
    if (!pixels) {
        if (IsImageValid(image)) UnloadImage(image);
        return 0;
    }
    const int count = image.width * image.height;
    std::vector<ol::u8> bright(static_cast<size_t>(count), 0);
    int bright_count = 0;
    for (int pixel = 0; pixel < count; ++pixel) {
        const Color color = pixels[pixel];
        const int maximum = std::max(color.r, std::max(color.g, color.b));
        const int sum = static_cast<int>(color.r) + color.g + color.b;
        if (maximum >= 248 && sum >= 380) {
            bright[pixel] = 1;
            ++bright_count;
        }
    }
    std::vector<ol::u8> visited(static_cast<size_t>(count), 0);
    std::vector<int> stack{};
    int isolated = 0;
    for (int seed = 0; seed < count; ++seed) {
        if (!bright[seed] || visited[seed]) continue;
        stack.clear();
        stack.push_back(seed);
        visited[seed] = 1;
        int component_size = 0;
        while (!stack.empty()) {
            const int pixel = stack.back();
            stack.pop_back();
            ++component_size;
            const int x = pixel % image.width;
            const int y = pixel / image.width;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height) continue;
                    const int neighbor = ny * image.width + nx;
                    if (!bright[neighbor] || visited[neighbor]) continue;
                    visited[neighbor] = 1;
                    stack.push_back(neighbor);
                }
            }
        }
        if (component_size <= 2) ++isolated;
    }
    if (saturated_texels) *saturated_texels += bright_count;
    UnloadImageColors(pixels);
    UnloadImage(image);
    return isolated;
}

CaveRenderBenchmarkResult run_cave_render_benchmark_case(bool force_legacy_invalidation) {
    PlaygroundLightingHarness harness{};
    CaveRenderBenchmarkResult result{};
    if (!init_cave_lighting_harness(
            &harness,
            force_legacy_invalidation ? "cave-render-legacy" : "cave-render-dynamic")) {
        return result;
    }
    harness.renderer.lighting.indirect_samples = 8;
    harness.renderer.lighting.shadow_samples = 2;
    harness.renderer.lighting.cascade_iterations = 2;
    harness.renderer.lighting.temporal_frames = 0;
    harness.renderer.lighting.jitter = false;

    struct ViewDefinition { Vector3 eye; Vector3 target; };
    const ViewDefinition views[] = {
        {{8.0f, 1.7f, 27.0f}, {8.0f, 2.5f, 10.0f}},
        {{8.0f, 2.2f, -1.0f}, {8.0f, 0.0f, -8.0f}},
        {{25.0f, 4.0f, 8.0f}, {31.9f, 4.0f, 8.0f}},
    };
    constexpr int warmup_frames = 5;
    constexpr int measured_frames_per_view = 24;
    std::vector<double> frame_times{};
    double total_scene_build = 0.0;
    double total_topology = 0.0;
    double total_cascade = 0.0;
    ol::u64 total_surface_updates = 0;
    ol::u64 total_surface_texels = 0;
    ol::u64 total_common_uniform_updates = 0;
    ol::u64 total_common_uniform_cache_hits = 0;
    ol::u64 total_scene_buffer_binds = 0;
    ol::u64 previous_emitter_signature = 0;
    int global_frame = 0;
    int view_index = 0;
    for (const ViewDefinition& definition : views) {
        const ol::CameraView view = cave_benchmark_view(harness, definition.eye, definition.target);
        for (int warmup = 0; warmup < warmup_frames; ++warmup) {
            set_cave_benchmark_emitters(&harness, static_cast<float>(global_frame++) / 60.0f);
            ol::renderer_render_dimension_to_target(
                &harness.renderer, harness.dim, view, harness.app->local_player_id);
            rlDrawRenderBatchActive();
            glFinish();
        }
        double view_total_ms = 0.0;
        double view_maximum_ms = 0.0;
        ol::u64 view_texels = 0;
        for (int frame = 0; frame < measured_frames_per_view; ++frame) {
            set_cave_benchmark_emitters(&harness, static_cast<float>(global_frame++) / 60.0f);
            if (force_legacy_invalidation) {
                // Conservative old-path reproduction: it still benefits from
                // the new cheap reflection shader, so the measured speedup
                // understates the complete before/after change.
                harness.renderer.radiance_scene_signature = 0;
                harness.renderer.radiance_topology_signature = 0;
                for (ol::MeshLightingSurfaceCache& surface : harness.renderer.lighting_surfaces) {
                    surface.resolved_scene_signature = 0;
                }
            }
            const double start = GetTime();
            ol::renderer_render_dimension_to_target(
                &harness.renderer, harness.dim, view, harness.app->local_player_id);
            rlDrawRenderBatchActive();
            glFinish();
            const double frame_ms = (GetTime() - start) * 1000.0;
            frame_times.push_back(frame_ms);
            view_total_ms += frame_ms;
            view_maximum_ms = std::max(view_maximum_ms, frame_ms);
            view_texels += harness.renderer.debug_radiance_surface_texels_submitted;
            total_scene_build += harness.renderer.debug_radiance_scene_build_ms;
            total_topology += harness.renderer.debug_radiance_topology_ms;
            total_cascade += harness.renderer.debug_radiance_cascade_ms;
            total_surface_updates += harness.renderer.debug_radiance_surface_updates;
            total_surface_texels += harness.renderer.debug_radiance_surface_texels_submitted;
            total_common_uniform_updates +=
                harness.renderer.debug_radiance_common_uniform_updates;
            total_common_uniform_cache_hits +=
                harness.renderer.debug_radiance_common_uniform_cache_hits;
            total_scene_buffer_binds += harness.renderer.debug_radiance_scene_buffer_binds;
            if (harness.renderer.debug_radiance_scene_build_ms > 0.001) {
                ++result.static_scene_build_frames;
            }
            if (previous_emitter_signature != 0 &&
                previous_emitter_signature != harness.renderer.radiance_emitter_signature) {
                ++result.emitter_signature_changes;
            }
            previous_emitter_signature = harness.renderer.radiance_emitter_signature;
        }
        std::printf(
            "cave render detail mode=%s view=%d avg=%.3fms max=%.3fms texels=%.0f\n",
            force_legacy_invalidation ? "legacy" : "dynamic", view_index++,
            view_total_ms / measured_frames_per_view, view_maximum_ms,
            static_cast<double>(view_texels) / measured_frames_per_view);
    }
    for (const ol::MeshLightingSurfaceCache& surface : harness.renderer.lighting_surfaces) {
        if (surface.reflectivity <= 0.001f || !IsRenderTextureValid(surface.texture)) continue;
        result.isolated_bright_components += small_bright_components(
            surface.texture.texture, &result.saturated_reflection_texels);
    }
    result.static_triangles = harness.renderer.debug_radiance_triangle_count;
    result.emitter_triangles = harness.renderer.radiance_emitter_triangle_count;
    result.emitter_meshes = harness.renderer.debug_radiance_emitter_count;
    if (!frame_times.empty()) {
        for (double milliseconds : frame_times) {
            result.average_ms += milliseconds;
            result.maximum_ms = std::max(result.maximum_ms, milliseconds);
        }
        result.average_ms /= static_cast<double>(frame_times.size());
        std::sort(frame_times.begin(), frame_times.end());
        const size_t p95_index = std::min(
            frame_times.size() - 1,
            static_cast<size_t>(std::ceil(frame_times.size() * 0.95) - 1.0));
        result.p95_ms = frame_times[p95_index];
        const double reciprocal = 1.0 / static_cast<double>(frame_times.size());
        result.average_scene_build_ms = total_scene_build * reciprocal;
        result.average_topology_ms = total_topology * reciprocal;
        result.average_cascade_ms = total_cascade * reciprocal;
        result.average_surface_updates = static_cast<double>(total_surface_updates) * reciprocal;
        result.average_surface_texels = static_cast<double>(total_surface_texels) * reciprocal;
        result.average_common_uniform_updates =
            static_cast<double>(total_common_uniform_updates) * reciprocal;
        result.average_common_uniform_cache_hits =
            static_cast<double>(total_common_uniform_cache_hits) * reciprocal;
        result.average_scene_buffer_binds =
            static_cast<double>(total_scene_buffer_binds) * reciprocal;
    }
    shutdown_playground_lighting_harness(&harness);
    return result;
}

bool run_cave_render_benchmark(int width, int height, const char* resolution_name) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(width, height, "ol cave render benchmark");
    const CaveRenderBenchmarkResult dynamic = run_cave_render_benchmark_case(false);
    const CaveRenderBenchmarkResult legacy = run_cave_render_benchmark_case(true);
    CloseWindow();
    auto print = [width, height, resolution_name](
        const char* name, const CaveRenderBenchmarkResult& result) {
        std::printf(
            "cave render %-8s resolution=%dx%d profile=%s "
            "avg=%.3fms p95=%.3fms max=%.3fms "
            "scene=%.3fms topology=%.3fms cascade=%.3fms builds=%d "
            "updates=%.2f texels=%.0f common_uniform_updates=%.2f "
            "common_uniform_hits=%.2f ssbo_binds=%.2f emitter_changes=%d "
            "static_tris=%u emitter_tris=%u emitters=%u isolated=%d saturated=%d\n",
            name, width, height, resolution_name,
            result.average_ms, result.p95_ms, result.maximum_ms,
            result.average_scene_build_ms, result.average_topology_ms,
            result.average_cascade_ms, result.static_scene_build_frames,
            result.average_surface_updates, result.average_surface_texels,
            result.average_common_uniform_updates,
            result.average_common_uniform_cache_hits,
            result.average_scene_buffer_binds,
            result.emitter_signature_changes, result.static_triangles,
            result.emitter_triangles, result.emitter_meshes,
            result.isolated_bright_components, result.saturated_reflection_texels);
    };
    print("dynamic", dynamic);
    print("legacy", legacy);

    const bool dynamic_light_path_valid = dynamic.emitter_signature_changes > 60 &&
        dynamic.static_scene_build_frames == 0 && dynamic.emitter_meshes == 3 &&
        dynamic.emitter_triangles == 36 && dynamic.static_triangles > 20;
    const bool reflections_are_firefly_free = dynamic.isolated_bright_components == 0;
    const bool conservative_speedup = legacy.average_ms > 0.0 &&
        dynamic.average_ms < legacy.average_ms;
    // This is deterministic submitted work rather than a machine-dependent
    // frame-time threshold. It catches regressions that accidentally resolve
    // every far face at level zero or discard conservative visible regions.
    const double resolution_scale =
        static_cast<double>(width) * static_cast<double>(height) / (960.0 * 540.0);
    const bool bounded_surface_work =
        dynamic.average_surface_texels < 300000.0 * resolution_scale &&
        dynamic.average_surface_texels < legacy.average_surface_texels * 0.35;
    const double common_uniform_uses = dynamic.average_common_uniform_updates +
        dynamic.average_common_uniform_cache_hits;
    const bool common_state_batched =
        dynamic.average_common_uniform_cache_hits > dynamic.average_common_uniform_updates &&
        common_uniform_uses > 0.0 &&
        dynamic.average_scene_buffer_binds < common_uniform_uses * 5.0;
    const bool pillar_clearance = 14.0f - 0.40f - 0.50f > 13.0f;
    return expect_true(
        dynamic_light_path_valid && reflections_are_firefly_free &&
        conservative_speedup && bounded_surface_work && common_state_batched &&
        pillar_clearance,
        "Animated cave emitters avoid static rebuilds, unbounded far-surface work, redundant common state, reflective fireflies, and the arch pillar");
}

bool run_cave_stationary_regression_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(960, 540, "ol cave stationary regression");

    PlaygroundLightingHarness harness{};
    bool ok = init_cave_lighting_harness(&harness, "cave-stationary-regression");
    if (!ok) {
        shutdown_playground_lighting_harness(&harness);
        CloseWindow();
        return expect_true(false, "Stationary cave lighting harness initializes");
    }
    harness.renderer.lighting.indirect_samples = 8;
    harness.renderer.lighting.shadow_samples = 2;
    harness.renderer.lighting.cascade_iterations = 2;
    harness.renderer.lighting.temporal_frames = 0;
    harness.renderer.lighting.jitter = false;

    const Vector3 feet = {3.3341f, 0.0f, 30.2251f};
    const ol::CameraView view = cave_benchmark_view(
        harness,
        {3.3341f, 1.7000f, 30.2251f},
        {2.3135f, 1.8248f, 22.2914f});
    place_lighting_test_player(&harness, feet);

    constexpr int frame_count = 180;
    ol::u64 static_scene_signature = 0;
    ol::u64 static_topology_signature = 0;
    ol::u64 previous_emitter_signature = 0;
    ol::u32 static_scene_buffer = 0;
    ol::u32 static_triangle_count = 0;
    int emitter_changes = 0;
    int cascade_update_frames = 0;
    int static_scene_rebuild_frames = 0;
    int neighbor_scene_ready_frames = 0;
    int neighbor_cascade_ready_frames = 0;
    ol::u64 stationary_neighbor_texels = 0;
    bool static_identity_stable = true;
    std::vector<double> steady_frame_ms{};
    steady_frame_ms.reserve(frame_count - 30);

    for (int frame = 0; frame < frame_count; ++frame) {
        set_cave_benchmark_emitters(&harness, static_cast<float>(frame) / 60.0f);
        const double frame_start = GetTime();
        ol::renderer_render_dimension_to_target(
            &harness.renderer, harness.dim, view, harness.app->local_player_id);
        rlDrawRenderBatchActive();
        glFinish();
        if (frame >= 30) {
            steady_frame_ms.push_back((GetTime() - frame_start) * 1000.0);
        }

        if (frame == 0) {
            static_scene_signature = harness.renderer.radiance_scene_signature;
            static_topology_signature = harness.renderer.radiance_topology_signature;
            static_scene_buffer = harness.renderer.radiance_scene_triangles_ssbo;
            static_triangle_count = harness.renderer.debug_radiance_triangle_count;
        } else {
            // debug_radiance_scene_build_ms is reset to exact zero before the
            // scene-change branch. This is an event check, not a performance
            // threshold: animated emitters must never enter that branch.
            if (harness.renderer.debug_radiance_scene_build_ms != 0.0) {
                ++static_scene_rebuild_frames;
            }
            static_identity_stable = static_identity_stable &&
                harness.renderer.radiance_scene_signature == static_scene_signature &&
                harness.renderer.radiance_topology_signature == static_topology_signature &&
                harness.renderer.radiance_scene_triangles_ssbo == static_scene_buffer &&
                harness.renderer.debug_radiance_triangle_count == static_triangle_count;
        }
        if (previous_emitter_signature != 0 &&
            previous_emitter_signature != harness.renderer.radiance_emitter_signature) {
            ++emitter_changes;
        }
        previous_emitter_signature = harness.renderer.radiance_emitter_signature;
        if (harness.renderer.debug_radiance_cascade_update_frame ==
            harness.renderer.radiance_frame) {
            ++cascade_update_frames;
        }
        neighbor_scene_ready_frames +=
            harness.renderer.debug_radiance_neighbor_scene_ready ? 1 : 0;
        neighbor_cascade_ready_frames +=
            harness.renderer.debug_radiance_neighbor_cascade_ready ? 1 : 0;
        stationary_neighbor_texels +=
            harness.renderer.debug_radiance_neighbor_texels_this_frame;
    }

    const bool neighbor_preparation_ready =
        neighbor_scene_ready_frames > 0 && neighbor_cascade_ready_frames > 0;
    const bool dynamic_lighting_live =
        emitter_changes >= frame_count - 2 && cascade_update_frames == frame_count;
    const bool no_static_rebuilds = static_scene_rebuild_frames == 0 &&
        static_identity_stable && static_scene_signature != 0 &&
        static_scene_buffer != 0 && static_triangle_count > 0;
    const bool stationary_surface_work_paused = stationary_neighbor_texels == 0;
    double average_ms = 0.0;
    for (double elapsed_ms : steady_frame_ms) average_ms += elapsed_ms;
    if (!steady_frame_ms.empty()) average_ms /= static_cast<double>(steady_frame_ms.size());
    std::sort(steady_frame_ms.begin(), steady_frame_ms.end());
    const size_t p99_index = steady_frame_ms.empty() ? 0 :
        std::min(steady_frame_ms.size() - 1,
                 static_cast<size_t>(std::ceil(steady_frame_ms.size() * 0.99)) - 1);
    const double p99_ms = steady_frame_ms.empty() ? 0.0 : steady_frame_ms[p99_index];
    const double maximum_ms = steady_frame_ms.empty() ? 0.0 : steady_frame_ms.back();
    std::printf(
        "cave stationary frames=%d emitter_changes=%d cascade_updates=%d "
        "neighbor_ready=%d/%d neighbor_texels=%llu static_rebuilds=%d stable=%d "
        "avg=%.3fms p99=%.3fms max=%.3fms\n",
        frame_count, emitter_changes, cascade_update_frames,
        neighbor_scene_ready_frames, neighbor_cascade_ready_frames,
        static_cast<unsigned long long>(stationary_neighbor_texels),
        static_scene_rebuild_frames, static_identity_stable ? 1 : 0,
        average_ms, p99_ms, maximum_ms);

    shutdown_playground_lighting_harness(&harness);
    CloseWindow();
    return expect_true(
        neighbor_preparation_ready && dynamic_lighting_live && no_static_rebuilds &&
        stationary_surface_work_paused,
        "Stationary cave seam keeps live lighting and prepared cascades without surface-prewarm or static rebuild work");
}

bool run_cave_surface_distance_regression_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "ol cave surface distance regression");

    PlaygroundLightingHarness harness{};
    bool ok = init_cave_lighting_harness(&harness, "cave-surface-distance");
    if (!ok) {
        shutdown_playground_lighting_harness(&harness);
        CloseWindow();
        return expect_true(false, "Cave surface-distance harness initializes");
    }
    harness.renderer.lighting.lighting_radius_chunks = 2;
    harness.renderer.lighting.cascade_iterations = 1;
    harness.renderer.lighting.indirect_samples = 1;
    harness.renderer.lighting.shadow_samples = 1;
    harness.renderer.radiance_neighbor_surface_prewarm = false;

    ol::u32 far_floor_id = ol::invalid_id;
    for (ol::u32 slot = 0; slot < harness.dim->meshes.count; ++slot) {
        const ol::MeshInstance& mesh = harness.dim->meshes.data[slot];
        if (std::strcmp(mesh.name, "cave floor 1 0") == 0) {
            far_floor_id = ol::arena_id_at_slot(&harness.dim->meshes, slot);
            break;
        }
    }

    constexpr float route_z[] = {28.0f, 29.2f, 30.2251f};
    bool exercised_nearest_only_range = false;
    bool far_floor_resolved = true;
    ol::u32 nearest_only_draw_candidates = 0;
    for (float z : route_z) {
        place_lighting_test_player(&harness, {8.0f, 0.0f, z});
        const ol::CameraView view = cave_benchmark_view(
            harness, {8.0f, 1.7f, z}, {8.0f, 0.0f, -8.0f});
        ol::renderer_render_dimension_to_target(
            &harness.renderer, harness.dim, view, harness.app->local_player_id);
        rlDrawRenderBatchActive();
        glFinish();
        nearest_only_draw_candidates = std::max(
            nearest_only_draw_candidates,
            harness.renderer.debug_radiance_draw_origin_gated_surface_count);

        bool found_top = false;
        for (const ol::MeshLightingSurfaceCache& surface : harness.renderer.lighting_surfaces) {
            if (surface.mesh_id != far_floor_id || surface.normal.y < 0.9f) continue;
            found_top = true;
            const ol::MeshInstance* mesh = ol::arena_get(&harness.dim->meshes, surface.mesh_id);
            if (!mesh) continue;
            const Vector3 origin = ol::world_delta_meters(
                mesh->origin, view.anchor, harness.dim->chunk_size_m);
            const float origin_chunks = ol::safe_len(origin) / harness.dim->chunk_size_m;
            const float nearest_chunks = std::max(
                0.0f,
                ol::safe_len(origin + surface.bounds_center) - surface.bounds_radius) /
                harness.dim->chunk_size_m;
            exercised_nearest_only_range = exercised_nearest_only_range ||
                (origin_chunks >= 2.0f && nearest_chunks < 2.0f);
            far_floor_resolved = far_floor_resolved &&
                IsRenderTextureValid(surface.texture) &&
                surface.resolved_scene_signature == harness.renderer.radiance_scene_signature;
        }
        far_floor_resolved = far_floor_resolved && found_top;
    }

    std::printf(
        "cave surface distance: far_floor=%u nearest_only=%d draw_candidates=%u resolved=%d\n",
        far_floor_id, exercised_nearest_only_range ? 1 : 0,
        nearest_only_draw_candidates, far_floor_resolved ? 1 : 0);
    ok = expect_true(
        ol::id_valid(far_floor_id) && exercised_nearest_only_range &&
        nearest_only_draw_candidates > 0 && far_floor_resolved,
        "Large cave faces stay lit while their nearest texels are inside the lighting radius") && ok;

    shutdown_playground_lighting_harness(&harness);
    CloseWindow();
    return ok;
}

struct CaveMotionFrameSample {
    double frame_ms = 0.0;
    double scene_ms = 0.0;
    double signature_ms = 0.0;
    double emitter_ms = 0.0;
    double topology_ms = 0.0;
    double cascade_ms = 0.0;
    Vector3 feet{};
    ol::ChunkCoord anchor{};
    const char* phase = "";
    int frame = 0;
    int phase_frame = 0;
    float speed = 0.0f;
    bool moving = false;
    bool anchor_changed = false;
    bool neighbor_scene_ready = false;
    bool neighbor_cascade_ready = false;
    bool promoted = false;
    ol::u32 surface_updates = 0;
    ol::u32 surface_allocations = 0;
    ol::u64 surface_texels = 0;
    ol::u32 dynamic_shadow_updates = 0;
    ol::u32 dynamic_shadow_allocations = 0;
    ol::u32 dynamic_shadow_frees = 0;
    ol::u64 dynamic_shadow_region_texels = 0;
    ol::u64 dynamic_shadow_clear_texels = 0;
    ol::u64 dynamic_shadow_copy_texels = 0;
    ol::u64 dynamic_shadow_full_copy_texels = 0;
    ol::u32 draw_candidate_surfaces = 0;
    ol::u32 draw_origin_gated_surfaces = 0;
    ol::u32 draw_unresolved_surfaces = 0;
    ol::u32 neighbor_allocations = 0;
    ol::u64 neighbor_texels = 0;
    ol::u32 neighbor_complete = 0;
    ol::u32 neighbor_required = 0;
    ol::u32 promoted_surfaces = 0;
    ol::u32 fallback_surfaces = 0;
    ol::u32 fallback_allocations = 0;
    ol::u64 fallback_texels = 0;
    ol::u32 fallback_support_surfaces = 0;
    ol::u64 fallback_support_texels = 0;
    ol::u64 fallback_max_surface_texels = 0;
    ol::i32 fallback_max_width = 0;
    ol::i32 fallback_max_height = 0;
};

struct CaveMotionPhase {
    const char* name;
    float forward;
    int frames;
};

constexpr CaveMotionPhase cave_motion_phases[] = {
    {"wall-approach", -1.0f, 24},
    {"wall-retreat", 1.0f, 36},
    {"wall-return", -1.0f, 36},
    {"interior-cross-north", 1.0f, 150},
    {"interior-cross-return", -1.0f, 150},
};

constexpr int cave_motion_expected_frames = 24 + 36 + 36 + 150 + 150;
constexpr int cave_motion_stationary_warmup_frames = 60;
constexpr int cave_motion_stationary_sample_frames = 180;

struct CaveMotionBenchmarkResult {
    std::vector<CaveMotionFrameSample> frames{};
    std::vector<double> stationary_frame_ms{};
    int emitter_changes = 0;
    int cascade_update_frames = 0;
    int static_scene_build_frames = 0;
    int anchor_changes = 0;
    int promotion_frames = 0;
    int neighbor_scene_ready_frames = 0;
    int neighbor_cascade_ready_frames = 0;
    ol::u64 neighbor_texels = 0;
    ol::u64 fallback_texels = 0;
    ol::u32 neighbor_allocations = 0;
    ol::u32 fallback_allocations = 0;
    float minimum_z = std::numeric_limits<float>::infinity();
    float maximum_z = -std::numeric_limits<float>::infinity();
    bool crossed_interior_north = false;
    bool crossed_interior_south = false;
    bool renderer_default_surface_prewarm = false;
    bool configured_surface_prewarm = false;
    bool lighting_enabled = false;
    bool pathtrace_enabled = false;
    bool depth_test_edges = false;
    bool shader_depth_edges = false;
    bool gpu_depth_edges = false;
    bool physics_debug = false;
    bool temporal_accumulation = false;
    bool jitter = false;
    bool corner_merge = false;
    int window_width = 0;
    int window_height = 0;
    int native_width = 0;
    int native_height = 0;
    int target_width = 0;
    int target_height = 0;
};

double cave_motion_percentile(
    const std::vector<const CaveMotionFrameSample*>& samples,
    double percentile) {
    if (samples.empty()) return 0.0;
    std::vector<double> sorted{};
    sorted.reserve(samples.size());
    for (const CaveMotionFrameSample* sample : samples) sorted.push_back(sample->frame_ms);
    std::sort(sorted.begin(), sorted.end());
    const size_t index = std::min(
        sorted.size() - 1,
        static_cast<size_t>(std::ceil(static_cast<double>(sorted.size()) * percentile) - 1.0));
    return sorted[index];
}

double cave_motion_value_percentile(std::vector<double> values, double percentile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = std::min(
        values.size() - 1,
        static_cast<size_t>(std::ceil(static_cast<double>(values.size()) * percentile) - 1.0));
    return values[index];
}

void print_cave_motion_bucket(
    const char* mode,
    const char* name,
    const std::vector<const CaveMotionFrameSample*>& samples) {
    if (samples.empty()) {
        std::printf("cave motion bucket mode=%s name=%s frames=0\n", mode, name);
        return;
    }
    double frame_total = 0.0;
    double scene_total = 0.0;
    double signature_total = 0.0;
    double emitter_total = 0.0;
    double topology_total = 0.0;
    double cascade_total = 0.0;
    ol::u64 updates = 0;
    ol::u64 allocations = 0;
    ol::u64 surface_texels = 0;
    ol::u64 dynamic_shadow_updates = 0;
    ol::u64 dynamic_shadow_allocations = 0;
    ol::u64 dynamic_shadow_frees = 0;
    ol::u64 dynamic_shadow_region_texels = 0;
    ol::u64 dynamic_shadow_clear_texels = 0;
    ol::u64 dynamic_shadow_copy_texels = 0;
    ol::u64 dynamic_shadow_full_copy_texels = 0;
    ol::u64 draw_candidate_surfaces = 0;
    ol::u64 draw_origin_gated_surfaces = 0;
    ol::u64 draw_unresolved_surfaces = 0;
    ol::u64 neighbor_allocations = 0;
    ol::u64 neighbor_texels = 0;
    ol::u64 fallback_surfaces = 0;
    ol::u64 fallback_allocations = 0;
    ol::u64 fallback_texels = 0;
    int ready_scene = 0;
    int ready_cascade = 0;
    int promotions = 0;
    size_t above_8_33_ms = 0;
    size_t above_16_67_ms = 0;
    size_t above_33_30_ms = 0;
    double maximum_ms = 0.0;
    for (const CaveMotionFrameSample* sample : samples) {
        frame_total += sample->frame_ms;
        maximum_ms = std::max(maximum_ms, sample->frame_ms);
        above_8_33_ms += sample->frame_ms > 8.33 ? 1 : 0;
        above_16_67_ms += sample->frame_ms > 16.67 ? 1 : 0;
        above_33_30_ms += sample->frame_ms > 33.30 ? 1 : 0;
        scene_total += sample->scene_ms;
        signature_total += sample->signature_ms;
        emitter_total += sample->emitter_ms;
        topology_total += sample->topology_ms;
        cascade_total += sample->cascade_ms;
        updates += sample->surface_updates;
        allocations += sample->surface_allocations;
        surface_texels += sample->surface_texels;
        dynamic_shadow_updates += sample->dynamic_shadow_updates;
        dynamic_shadow_allocations += sample->dynamic_shadow_allocations;
        dynamic_shadow_frees += sample->dynamic_shadow_frees;
        dynamic_shadow_region_texels += sample->dynamic_shadow_region_texels;
        dynamic_shadow_clear_texels += sample->dynamic_shadow_clear_texels;
        dynamic_shadow_copy_texels += sample->dynamic_shadow_copy_texels;
        dynamic_shadow_full_copy_texels += sample->dynamic_shadow_full_copy_texels;
        draw_candidate_surfaces += sample->draw_candidate_surfaces;
        draw_origin_gated_surfaces += sample->draw_origin_gated_surfaces;
        draw_unresolved_surfaces += sample->draw_unresolved_surfaces;
        neighbor_allocations += sample->neighbor_allocations;
        neighbor_texels += sample->neighbor_texels;
        fallback_surfaces += sample->fallback_surfaces;
        fallback_allocations += sample->fallback_allocations;
        fallback_texels += sample->fallback_texels;
        ready_scene += sample->neighbor_scene_ready ? 1 : 0;
        ready_cascade += sample->neighbor_cascade_ready ? 1 : 0;
        promotions += sample->promoted ? 1 : 0;
    }
    const double reciprocal = 1.0 / static_cast<double>(samples.size());
    std::printf(
        "cave motion bucket mode=%s name=%s frames=%zu avg=%.3fms p95=%.3fms "
        "p99=%.3fms max=%.3fms scene=%.3fms signature=%.3fms emitter=%.3fms "
        "topology=%.3fms cascade=%.3fms updates=%.2f allocations=%.2f texels=%.0f "
        "shadow=%llu/%llu/%llu region=%llu clear=%llu copy=%llu/%llu draw=%llu/%llu/%llu "
        "neighbor_texels=%llu neighbor_allocations=%llu ready=%d/%d promotions=%d "
        "fallback_surfaces=%llu fallback_allocations=%llu fallback_texels=%llu "
        "above_ms=8.33:%zu/16.67:%zu/33.30:%zu\n",
        mode, name, samples.size(), frame_total * reciprocal,
        cave_motion_percentile(samples, 0.95), cave_motion_percentile(samples, 0.99),
        maximum_ms, scene_total * reciprocal, signature_total * reciprocal,
        emitter_total * reciprocal, topology_total * reciprocal, cascade_total * reciprocal,
        static_cast<double>(updates) * reciprocal,
        static_cast<double>(allocations) * reciprocal,
        static_cast<double>(surface_texels) * reciprocal,
        static_cast<unsigned long long>(dynamic_shadow_updates),
        static_cast<unsigned long long>(dynamic_shadow_allocations),
        static_cast<unsigned long long>(dynamic_shadow_frees),
        static_cast<unsigned long long>(dynamic_shadow_region_texels),
        static_cast<unsigned long long>(dynamic_shadow_clear_texels),
        static_cast<unsigned long long>(dynamic_shadow_copy_texels),
        static_cast<unsigned long long>(dynamic_shadow_full_copy_texels),
        static_cast<unsigned long long>(draw_candidate_surfaces),
        static_cast<unsigned long long>(draw_origin_gated_surfaces),
        static_cast<unsigned long long>(draw_unresolved_surfaces),
        static_cast<unsigned long long>(neighbor_texels),
        static_cast<unsigned long long>(neighbor_allocations), ready_scene, ready_cascade,
        promotions, static_cast<unsigned long long>(fallback_surfaces),
        static_cast<unsigned long long>(fallback_allocations),
        static_cast<unsigned long long>(fallback_texels), above_8_33_ms,
        above_16_67_ms, above_33_30_ms);
}

void print_cave_motion_frame(
    const char* mode,
    const char* label,
    const CaveMotionFrameSample* sample) {
    if (!sample) return;
    std::printf(
        "cave motion worst mode=%s label=%s frame=%d phase=%s[%d] ms=%.3f "
        "feet=(%.3f,%.3f,%.3f) anchor=(%d,%d,%d) speed=%.3f crossing=%d "
        "scene=%.3f signature=%.3f emitter=%.3f topology=%.3f cascade=%.3f "
        "updates=%u allocations=%u texels=%llu shadow=%u/%u/%u region=%llu clear=%llu copy=%llu/%llu "
        "draw=%u/%u/%u "
        "neighbor=%llu/%u ready=%d/%d "
        "complete=%u/%u promoted=%d/%u fallback=%u/%u/%llu support=%u/%llu "
        "fallback_max=%dx%d/%llu\n",
        mode, label, sample->frame, sample->phase, sample->phase_frame, sample->frame_ms,
        sample->feet.x, sample->feet.y, sample->feet.z,
        sample->anchor.x, sample->anchor.y, sample->anchor.z, sample->speed,
        sample->anchor_changed ? 1 : 0, sample->scene_ms, sample->signature_ms,
        sample->emitter_ms, sample->topology_ms, sample->cascade_ms,
        sample->surface_updates, sample->surface_allocations,
        static_cast<unsigned long long>(sample->surface_texels),
        sample->dynamic_shadow_updates, sample->dynamic_shadow_allocations,
        sample->dynamic_shadow_frees,
        static_cast<unsigned long long>(sample->dynamic_shadow_region_texels),
        static_cast<unsigned long long>(sample->dynamic_shadow_clear_texels),
        static_cast<unsigned long long>(sample->dynamic_shadow_copy_texels),
        static_cast<unsigned long long>(sample->dynamic_shadow_full_copy_texels),
        sample->draw_candidate_surfaces, sample->draw_origin_gated_surfaces,
        sample->draw_unresolved_surfaces,
        static_cast<unsigned long long>(sample->neighbor_texels),
        sample->neighbor_allocations, sample->neighbor_scene_ready ? 1 : 0,
        sample->neighbor_cascade_ready ? 1 : 0, sample->neighbor_complete,
        sample->neighbor_required, sample->promoted ? 1 : 0, sample->promoted_surfaces,
        sample->fallback_surfaces, sample->fallback_allocations,
        static_cast<unsigned long long>(sample->fallback_texels),
        sample->fallback_support_surfaces,
        static_cast<unsigned long long>(sample->fallback_support_texels),
        sample->fallback_max_width, sample->fallback_max_height,
        static_cast<unsigned long long>(sample->fallback_max_surface_texels));
}

CaveMotionBenchmarkResult run_cave_motion_benchmark_case(bool surface_prewarm_enabled) {
    PlaygroundLightingHarness harness{};
    CaveMotionBenchmarkResult result{};
    if (!init_cave_lighting_harness(
            &harness,
            surface_prewarm_enabled
                ? "cave-motion-experimental-prewarm"
                : "cave-motion-production")) {
        return result;
    }
    result.renderer_default_surface_prewarm =
        harness.renderer.radiance_neighbor_surface_prewarm;
    harness.renderer.lighting.indirect_samples = 8;
    harness.renderer.lighting.shadow_samples = 2;
    harness.renderer.lighting.cascade_iterations = 2;
    harness.renderer.lighting.temporal_frames = 0;
    harness.renderer.lighting.jitter = false;
    harness.renderer.radiance_neighbor_surface_prewarm = surface_prewarm_enabled;
    result.configured_surface_prewarm =
        harness.renderer.radiance_neighbor_surface_prewarm;
    result.lighting_enabled = harness.renderer.lighting.enabled;
    result.pathtrace_enabled = harness.renderer.pathtrace_comparison_enabled;
    result.depth_test_edges = harness.renderer.depth_test_edges;
    result.shader_depth_edges = harness.renderer.shader_depth_edges;
    result.gpu_depth_edges = harness.renderer.gpu_depth_edges;
    result.physics_debug = harness.renderer.draw_physics_debug;
    result.temporal_accumulation = harness.renderer.lighting.temporal_frames > 0;
    result.jitter = harness.renderer.lighting.jitter;
    result.corner_merge = harness.renderer.lighting.corner_merge;
    result.window_width = GetScreenWidth();
    result.window_height = GetScreenHeight();
    result.native_width = harness.renderer.native_w;
    result.native_height = harness.renderer.native_h;
    result.target_width = harness.renderer.target.texture.width;
    result.target_height = harness.renderer.target.texture.height;

    constexpr Vector3 reported_feet = {3.3341f, 0.0f, 30.2251f};
    const ol::CameraView reported_view = cave_benchmark_view(
        harness,
        {3.3341f, 1.7000f, 30.2251f},
        {2.3135f, 1.8248f, 22.2914f});
    place_lighting_test_player(&harness, reported_feet);
    ol::PlayerEntity* player = ol::arena_get(
        &harness.dim->players, harness.app->local_player_id);
    if (!player) {
        shutdown_playground_lighting_harness(&harness);
        return result;
    }
    player->yaw = reported_view.yaw;
    player->pitch = reported_view.pitch;
    player->velocity = {};

    constexpr float fixed_dt = 1.0f / 60.0f;
    ol::u64 global_frame = 0;
    ol::PlayerControllerInput stationary_input{};
    const int stationary_frames = cave_motion_stationary_warmup_frames +
        cave_motion_stationary_sample_frames;
    for (int frame = 0; frame < stationary_frames; ++frame) {
        const double frame_start = GetTime();
        ol::player_controller_step(harness.dim, player, stationary_input, fixed_dt);
        set_cave_benchmark_emitters(&harness, static_cast<float>(global_frame++) * fixed_dt);
        ol::CameraView view = reported_view;
        view.anchor = ol::player_feet_pos(harness.dim, player);
        view.eye_height = player->eye_height;
        ol::renderer_render_dimension_to_target(
            &harness.renderer, harness.dim, view, harness.app->local_player_id);
        rlDrawRenderBatchActive();
        glFinish();
        if (frame >= cave_motion_stationary_warmup_frames) {
            result.stationary_frame_ms.push_back((GetTime() - frame_start) * 1000.0);
        }
    }
    // The first three phases reproduce walking into and away from the south
    // wall at the reported pose. The wall's inner face is exactly z=32, so a
    // real 0.35 m-radius player cannot cross that boundary. The final phases
    // continue through the legal interior z=16 boundary and return, exercising
    // prepared-scene promotion without disabling collision.
    ol::ChunkCoord previous_anchor = ol::player_feet_pos(harness.dim, player).chunk;
    ol::u64 previous_emitter_signature = harness.renderer.radiance_emitter_signature;
    int measured_frame = 0;
    for (const CaveMotionPhase& phase : cave_motion_phases) {
        for (int phase_frame = 0; phase_frame < phase.frames; ++phase_frame) {
            const double frame_start = GetTime();
            ol::PlayerControllerInput input{};
            input.move = {0.0f, phase.forward};
            ol::player_controller_step(harness.dim, player, input, fixed_dt);
            set_cave_benchmark_emitters(
                &harness, static_cast<float>(global_frame++) * fixed_dt);

            ol::CameraView view = reported_view;
            view.anchor = ol::player_feet_pos(harness.dim, player);
            view.eye_height = player->eye_height;
            ol::renderer_render_dimension_to_target(
                &harness.renderer, harness.dim, view, harness.app->local_player_id);
            rlDrawRenderBatchActive();
            glFinish();

            CaveMotionFrameSample sample{};
            sample.frame_ms = (GetTime() - frame_start) * 1000.0;
            sample.scene_ms = harness.renderer.debug_radiance_scene_build_ms;
            sample.signature_ms = harness.renderer.debug_radiance_signature_ms;
            sample.emitter_ms = harness.renderer.debug_radiance_emitter_ms;
            sample.topology_ms = harness.renderer.debug_radiance_topology_ms;
            sample.cascade_ms = harness.renderer.debug_radiance_cascade_ms;
            sample.feet = {
                feet_x(harness.dim, player),
                feet_y(harness.dim, player),
                feet_z(harness.dim, player)};
            sample.anchor = view.anchor.chunk;
            sample.phase = phase.name;
            sample.frame = measured_frame++;
            sample.phase_frame = phase_frame;
            sample.speed = player_horizontal_speed(player);
            sample.moving = sample.speed > 0.05f;
            sample.anchor_changed = sample.anchor.x != previous_anchor.x ||
                sample.anchor.y != previous_anchor.y || sample.anchor.z != previous_anchor.z;
            sample.neighbor_scene_ready = harness.renderer.debug_radiance_neighbor_scene_ready;
            sample.neighbor_cascade_ready = harness.renderer.debug_radiance_neighbor_cascade_ready;
            sample.promoted = harness.renderer.debug_radiance_neighbor_promoted;
            sample.surface_updates = harness.renderer.debug_radiance_surface_updates;
            sample.surface_allocations = harness.renderer.debug_radiance_surface_allocations;
            sample.surface_texels = harness.renderer.debug_radiance_surface_texels_submitted;
            sample.dynamic_shadow_updates =
                harness.renderer.debug_radiance_dynamic_shadow_updates;
            sample.dynamic_shadow_allocations =
                harness.renderer.debug_radiance_dynamic_shadow_allocations;
            sample.dynamic_shadow_frees =
                harness.renderer.debug_radiance_dynamic_shadow_mask_frees;
            sample.dynamic_shadow_region_texels =
                harness.renderer.debug_radiance_dynamic_shadow_region_texels;
            sample.dynamic_shadow_clear_texels =
                harness.renderer.debug_radiance_dynamic_shadow_clear_texels;
            sample.dynamic_shadow_copy_texels =
                harness.renderer.debug_radiance_dynamic_shadow_copy_texels;
            sample.dynamic_shadow_full_copy_texels =
                harness.renderer.debug_radiance_dynamic_shadow_full_copy_texels;
            sample.draw_candidate_surfaces =
                harness.renderer.debug_radiance_draw_candidate_surface_count;
            sample.draw_origin_gated_surfaces =
                harness.renderer.debug_radiance_draw_origin_gated_surface_count;
            sample.draw_unresolved_surfaces =
                harness.renderer.debug_radiance_draw_unresolved_surface_count;
            sample.neighbor_allocations =
                harness.renderer.debug_radiance_neighbor_allocations_this_frame;
            sample.neighbor_texels = harness.renderer.debug_radiance_neighbor_texels_this_frame;
            sample.neighbor_complete =
                harness.renderer.debug_radiance_neighbor_complete_surface_count;
            sample.neighbor_required =
                harness.renderer.debug_radiance_neighbor_required_surface_count;
            sample.promoted_surfaces =
                harness.renderer.debug_radiance_neighbor_promoted_surface_count;
            sample.fallback_surfaces =
                harness.renderer.debug_radiance_neighbor_sync_fallback_surface_count;
            sample.fallback_allocations =
                harness.renderer.debug_radiance_neighbor_sync_fallback_allocation_count;
            sample.fallback_texels =
                harness.renderer.debug_radiance_neighbor_sync_fallback_texels;
            sample.fallback_support_surfaces =
                harness.renderer.debug_radiance_neighbor_sync_fallback_support_surface_count;
            sample.fallback_support_texels =
                harness.renderer.debug_radiance_neighbor_sync_fallback_support_texels;
            sample.fallback_max_surface_texels =
                harness.renderer.debug_radiance_neighbor_sync_fallback_max_surface_texels;
            sample.fallback_max_width =
                harness.renderer.debug_radiance_neighbor_sync_fallback_max_width;
            sample.fallback_max_height =
                harness.renderer.debug_radiance_neighbor_sync_fallback_max_height;

            if (previous_emitter_signature != 0 &&
                previous_emitter_signature != harness.renderer.radiance_emitter_signature) {
                ++result.emitter_changes;
            }
            previous_emitter_signature = harness.renderer.radiance_emitter_signature;
            if (harness.renderer.debug_radiance_cascade_update_frame ==
                harness.renderer.radiance_frame) {
                ++result.cascade_update_frames;
            }
            if (sample.scene_ms != 0.0) ++result.static_scene_build_frames;
            if (sample.anchor_changed) ++result.anchor_changes;
            if (sample.promoted) ++result.promotion_frames;
            result.neighbor_scene_ready_frames += sample.neighbor_scene_ready ? 1 : 0;
            result.neighbor_cascade_ready_frames += sample.neighbor_cascade_ready ? 1 : 0;
            result.neighbor_texels += sample.neighbor_texels;
            result.neighbor_allocations += sample.neighbor_allocations;
            result.fallback_texels += sample.fallback_texels;
            result.fallback_allocations += sample.fallback_allocations;
            result.minimum_z = std::min(result.minimum_z, sample.feet.z);
            result.maximum_z = std::max(result.maximum_z, sample.feet.z);
            if (previous_anchor.z >= 1 && sample.anchor.z <= 0) {
                result.crossed_interior_north = true;
            }
            if (previous_anchor.z <= 0 && sample.anchor.z >= 1) {
                result.crossed_interior_south = true;
            }
            previous_anchor = sample.anchor;
            result.frames.push_back(sample);
        }
    }

    shutdown_playground_lighting_harness(&harness);
    return result;
}

bool run_cave_motion_benchmark(int width, int height, const char* variant) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(width, height, "ol cave motion benchmark");
    const CaveMotionBenchmarkResult production = run_cave_motion_benchmark_case(false);
    const CaveMotionBenchmarkResult experimental_prewarm =
        run_cave_motion_benchmark_case(true);
    CloseWindow();

    auto print_case = [variant, width, height](
                          const char* mode,
                          const CaveMotionBenchmarkResult& result) {
        std::vector<const CaveMotionFrameSample*> all{};
        std::vector<const CaveMotionFrameSample*> moving{};
        std::vector<const CaveMotionFrameSample*> early{};
        std::vector<const CaveMotionFrameSample*> moving_non_crossing{};
        std::vector<const CaveMotionFrameSample*> moving_steady{};
        std::vector<const CaveMotionFrameSample*> reported_wall{};
        std::vector<const CaveMotionFrameSample*> crossings{};
        std::vector<const CaveMotionFrameSample*> staged{};
        std::vector<const CaveMotionFrameSample*> staged_allocations{};
        std::vector<const CaveMotionFrameSample*> shadow_mask_churn{};
        std::vector<const CaveMotionFrameSample*> fallback{};
        const CaveMotionFrameSample* worst = nullptr;
        const CaveMotionFrameSample* worst_moving_non_crossing = nullptr;
        const CaveMotionFrameSample* worst_moving_steady = nullptr;
        const CaveMotionFrameSample* worst_reported_wall = nullptr;
        const CaveMotionFrameSample* worst_crossing = nullptr;
        std::vector<int> transition_frames{};
        for (const CaveMotionFrameSample& frame : result.frames) {
            if (frame.anchor_changed) transition_frames.push_back(frame.frame);
        }
        auto near_transition = [&](int frame) {
            for (int transition : transition_frames) {
                if (std::abs(frame - transition) <= 4) return true;
            }
            return false;
        };
        for (const CaveMotionFrameSample& frame : result.frames) {
            all.push_back(&frame);
            if (frame.moving) moving.push_back(&frame);
            if (frame.frame < 120) early.push_back(&frame);
            if (frame.moving && !frame.anchor_changed) moving_non_crossing.push_back(&frame);
            if (frame.moving && !near_transition(frame.frame)) moving_steady.push_back(&frame);
            if (frame.frame < 96) reported_wall.push_back(&frame);
            if (frame.anchor_changed) crossings.push_back(&frame);
            if (frame.neighbor_texels > 0 || frame.neighbor_allocations > 0) staged.push_back(&frame);
            if (frame.neighbor_allocations > 0) staged_allocations.push_back(&frame);
            if (frame.dynamic_shadow_allocations > 0 || frame.dynamic_shadow_frees > 0) {
                shadow_mask_churn.push_back(&frame);
            }
            if (frame.fallback_surfaces > 0) fallback.push_back(&frame);
            if (!worst || frame.frame_ms > worst->frame_ms) worst = &frame;
            if (frame.moving && !frame.anchor_changed &&
                (!worst_moving_non_crossing ||
                 frame.frame_ms > worst_moving_non_crossing->frame_ms)) {
                worst_moving_non_crossing = &frame;
            }
            if (frame.moving && !near_transition(frame.frame) &&
                (!worst_moving_steady || frame.frame_ms > worst_moving_steady->frame_ms)) {
                worst_moving_steady = &frame;
            }
            if (frame.frame < 96 &&
                (!worst_reported_wall || frame.frame_ms > worst_reported_wall->frame_ms)) {
                worst_reported_wall = &frame;
            }
            if (frame.anchor_changed &&
                (!worst_crossing || frame.frame_ms > worst_crossing->frame_ms)) {
                worst_crossing = &frame;
            }
        }
        std::printf(
            "cave motion config variant=%s mode=%s requested=%dx%d window=%dx%d "
            "native=%dx%d target=%dx%d surface_prewarm=%d renderer_default=%d "
            "render_flags=lighting:%d/pathtrace:%d/depth_edges:%d/shader_edges:%d/"
            "gpu_edges:%d/physics_debug:%d temporal=%d jitter=%d corner_merge=%d "
            "disabled_render_flags=pathtrace,depth-test-edges,shader-depth-edges,"
            "gpu-depth-edges,physics-debug\n",
            variant, mode, width, height, result.window_width, result.window_height,
            result.native_width, result.native_height, result.target_width,
            result.target_height, result.configured_surface_prewarm ? 1 : 0,
            result.renderer_default_surface_prewarm ? 1 : 0,
            result.lighting_enabled ? 1 : 0, result.pathtrace_enabled ? 1 : 0,
            result.depth_test_edges ? 1 : 0, result.shader_depth_edges ? 1 : 0,
            result.gpu_depth_edges ? 1 : 0, result.physics_debug ? 1 : 0,
            result.temporal_accumulation ? 1 : 0, result.jitter ? 1 : 0,
            result.corner_merge ? 1 : 0);
        print_cave_motion_bucket(mode, "all", all);
        print_cave_motion_bucket(mode, "moving", moving);
        print_cave_motion_bucket(mode, "first-2s", early);
        print_cave_motion_bucket(mode, "reported-wall", reported_wall);
        print_cave_motion_bucket(mode, "moving-noncross", moving_non_crossing);
        print_cave_motion_bucket(mode, "moving-steady", moving_steady);
        print_cave_motion_bucket(mode, "anchor-cross", crossings);
        print_cave_motion_bucket(mode, "surface-prewarm", staged);
        print_cave_motion_bucket(mode, "prewarm-allocation", staged_allocations);
        print_cave_motion_bucket(mode, "shadow-mask-churn", shadow_mask_churn);
        print_cave_motion_bucket(mode, "sync-fallback", fallback);
        for (const CaveMotionPhase& phase : cave_motion_phases) {
            std::vector<const CaveMotionFrameSample*> phase_frames{};
            for (const CaveMotionFrameSample& frame : result.frames) {
                if (std::strcmp(frame.phase, phase.name) == 0) {
                    phase_frames.push_back(&frame);
                }
            }
            const std::string bucket_name = std::string("phase-") + phase.name;
            print_cave_motion_bucket(mode, bucket_name.c_str(), phase_frames);
        }
        print_cave_motion_frame(mode, "overall", worst);
        print_cave_motion_frame(mode, "reported-wall", worst_reported_wall);
        print_cave_motion_frame(mode, "moving-noncross", worst_moving_non_crossing);
        print_cave_motion_frame(mode, "moving-steady", worst_moving_steady);
        print_cave_motion_frame(mode, "anchor-cross", worst_crossing);
        for (const CaveMotionFrameSample* crossing : crossings) {
            print_cave_motion_frame(mode, "anchor-event", crossing);
        }
        double stationary_total = 0.0;
        double stationary_maximum = 0.0;
        size_t stationary_above_8_33_ms = 0;
        size_t stationary_above_16_67_ms = 0;
        size_t stationary_above_33_30_ms = 0;
        for (double frame_ms : result.stationary_frame_ms) {
            stationary_total += frame_ms;
            stationary_maximum = std::max(stationary_maximum, frame_ms);
            stationary_above_8_33_ms += frame_ms > 8.33 ? 1 : 0;
            stationary_above_16_67_ms += frame_ms > 16.67 ? 1 : 0;
            stationary_above_33_30_ms += frame_ms > 33.30 ? 1 : 0;
        }
        const double stationary_average = result.stationary_frame_ms.empty()
            ? 0.0
            : stationary_total / static_cast<double>(result.stationary_frame_ms.size());
        const double stationary_p99 =
            cave_motion_value_percentile(result.stationary_frame_ms, 0.99);
        std::printf(
            "cave motion stationary mode=%s frames=%zu avg=%.3fms p95=%.3fms "
            "p99=%.3fms max=%.3fms above_ms=8.33:%zu/16.67:%zu/33.30:%zu\n",
            mode, result.stationary_frame_ms.size(), stationary_average,
            cave_motion_value_percentile(result.stationary_frame_ms, 0.95),
            stationary_p99, stationary_maximum, stationary_above_8_33_ms,
            stationary_above_16_67_ms, stationary_above_33_30_ms);

        double moving_total = 0.0;
        size_t moving_above_8_33_ms = 0;
        size_t moving_above_16_67_ms = 0;
        size_t moving_above_33_30_ms = 0;
        for (const CaveMotionFrameSample* frame : moving) {
            moving_total += frame->frame_ms;
            moving_above_8_33_ms += frame->frame_ms > 8.33 ? 1 : 0;
            moving_above_16_67_ms += frame->frame_ms > 16.67 ? 1 : 0;
            moving_above_33_30_ms += frame->frame_ms > 33.30 ? 1 : 0;
        }
        const double moving_average = moving.empty()
            ? 0.0
            : moving_total / static_cast<double>(moving.size());
        const double moving_p99 = cave_motion_percentile(moving, 0.99);
        const double average_ratio = stationary_average > 0.0
            ? moving_average / stationary_average
            : 0.0;
        const double p99_ratio = stationary_p99 > 0.0
            ? moving_p99 / stationary_p99
            : 0.0;
        std::printf(
            "cave motion moving-vs-stationary mode=%s moving_frames=%zu "
            "stationary_frames=%zu avg=%.3f/%.3fms delta=%+.3fms ratio=%.3fx "
            "p99=%.3f/%.3fms delta=%+.3fms ratio=%.3fx "
            "moving_above_ms=8.33:%zu/16.67:%zu/33.30:%zu "
            "stationary_above_ms=8.33:%zu/16.67:%zu/33.30:%zu\n",
            mode, moving.size(), result.stationary_frame_ms.size(), moving_average,
            stationary_average, moving_average - stationary_average, average_ratio,
            moving_p99, stationary_p99, moving_p99 - stationary_p99, p99_ratio,
            moving_above_8_33_ms, moving_above_16_67_ms, moving_above_33_30_ms,
            stationary_above_8_33_ms, stationary_above_16_67_ms,
            stationary_above_33_30_ms);
        std::printf(
            "cave motion summary mode=%s frames=%zu emitters=%d cascades=%d "
            "static_builds=%d anchors=%d promotions=%d ready=%d/%d "
            "neighbor_texels=%llu neighbor_allocations=%u fallback_texels=%llu "
            "fallback_allocations=%u z=[%.3f,%.3f] crossed_z16=%d/%d\n",
            mode, result.frames.size(), result.emitter_changes,
            result.cascade_update_frames, result.static_scene_build_frames,
            result.anchor_changes, result.promotion_frames,
            result.neighbor_scene_ready_frames, result.neighbor_cascade_ready_frames,
            static_cast<unsigned long long>(result.neighbor_texels),
            result.neighbor_allocations,
            static_cast<unsigned long long>(result.fallback_texels),
            result.fallback_allocations, result.minimum_z, result.maximum_z,
            result.crossed_interior_north ? 1 : 0,
            result.crossed_interior_south ? 1 : 0);
    };
    print_case("production", production);
    print_case("experimental-prewarm", experimental_prewarm);

    constexpr ol::u64 per_frame_prewarm_texel_budget = 16u * 1024u;
    bool bounded_experimental_prewarm = true;
    for (const CaveMotionFrameSample& frame : experimental_prewarm.frames) {
        bounded_experimental_prewarm = bounded_experimental_prewarm &&
            frame.neighbor_texels <= per_frame_prewarm_texel_budget &&
            frame.neighbor_allocations <= 1;
    }
    const bool complete_routes =
        static_cast<int>(production.frames.size()) == cave_motion_expected_frames &&
        static_cast<int>(experimental_prewarm.frames.size()) ==
            cave_motion_expected_frames &&
        production.stationary_frame_ms.size() == cave_motion_stationary_sample_frames &&
        experimental_prewarm.stationary_frame_ms.size() ==
            cave_motion_stationary_sample_frames &&
        production.crossed_interior_north && production.crossed_interior_south &&
        experimental_prewarm.crossed_interior_north &&
        experimental_prewarm.crossed_interior_south &&
        production.maximum_z <= 31.66f && experimental_prewarm.maximum_z <= 31.66f;
    const bool live_lighting =
        production.emitter_changes >= cave_motion_expected_frames - 2 &&
        experimental_prewarm.emitter_changes >= cave_motion_expected_frames - 2 &&
        production.cascade_update_frames == cave_motion_expected_frames &&
        experimental_prewarm.cascade_update_frames == cave_motion_expected_frames;
    const bool prewarm_ab_active = experimental_prewarm.neighbor_texels > 0 &&
        experimental_prewarm.neighbor_allocations > 0 &&
        production.neighbor_texels == 0 && production.neighbor_allocations == 0;
    const bool preparation_promotes = production.promotion_frames >= 2 &&
        experimental_prewarm.promotion_frames >= 2;
    const bool experimental_reduces_fallback =
        experimental_prewarm.fallback_texels <= production.fallback_texels &&
        experimental_prewarm.fallback_allocations <= production.fallback_allocations;
    const bool production_default_matches =
        !production.renderer_default_surface_prewarm &&
        !experimental_prewarm.renderer_default_surface_prewarm &&
        !production.configured_surface_prewarm &&
        experimental_prewarm.configured_surface_prewarm;
    bool ok = expect_true(
        production_default_matches,
        "Cave walking benchmark production mode explicitly matches the renderer's surface-prewarm-off default");
    ok = expect_true(
        complete_routes && live_lighting && bounded_experimental_prewarm &&
        prewarm_ab_active && preparation_promotes && experimental_reduces_fallback,
        "Cave walking motion keeps animated lighting live and bounded while staged surfaces reduce legal seam fallback");
    return ok;
}

bool run_edge_compare_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(960, 540, "ol edge compare");

    ol::RenderState renderer{};
    ol::renderer_init(&renderer);

    auto app = std::make_unique<ol::DemoApp>();
    ol::demo_generate_world(app.get());
    ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);

    std::filesystem::create_directories("test-output");
    const EdgeCompareView views[] = {
        {"overview", {0.0f, 1.65f, 15.0f}, {0.0f, 1.1f, -3.0f}},
        {"stairs", {-11.5f, 2.2f, 14.0f}, {-8.0f, 1.0f, 8.0f}},
        {"stair-tunnel", {-8.0f, 2.8f, 11.0f}, {10.5f, 0.9f, -3.5f}},
        {"tunnel-ramp", {15.0f, 1.8f, 2.2f}, {8.5f, 1.1f, 8.4f}},
        {"tunnel-ramp-close", {13.8f, 1.35f, 0.6f}, {10.3f, 0.75f, -3.4f}},
        {"user-tunnel-stairs", {6.1099f, 2.7000f, 11.4536f}, {9.0207f, 1.9794f, 4.0368f}},
        {"user-tunnel-outside", {6.6376f, 1.6500f, -4.5389f}, {14.3344f, 0.0418f, -3.0650f}},
        {"user-tunnel-inside", {10.3368f, 0.8000f, -3.6188f}, {11.0135f, -0.4793f, 4.2492f}},
        {"user-wall", {6.8266f, 1.6500f, -5.5259f}, {11.4904f, 1.8436f, -12.0230f}},
        {"user-sprite-alpha", {-1.0488f, 1.6500f, -9.7776f}, {-4.3593f, 1.2981f, -2.5033f}},
        {"user-missing-contours", {10.9845f, 1.6500f, -0.2620f}, {10.4826f, -1.3381f, -7.6660f}},
        {"user-edge-wall-northwest", {-6.8260f, 1.6500f, -13.8749f}, {-13.2037f, 0.6685f, -18.6037f}},
        {"user-edge-blocks", {-6.7242f, 1.6500f, -6.1981f}, {-1.7872f, -0.9408f, -0.4611f}},
        {"user-edge-west-wall", {-16.0361f, 1.6500f, 11.9724f}, {-19.0325f, 0.6336f, 19.3201f}},
        {"user-edge-tunnel-inside", {10.4218f, 0.8000f, -2.0930f}, {10.4921f, -0.0941f, -10.0426f}},
        {"user-extra-edges", {-3.8222f, 3.7818f, -3.7859f}, {-9.2737f, 1.1229f, -9.0025f}},
        {"user-uneven-thickness", {2.3903f, 1.6500f, 7.6561f}, {9.6315f, -1.5653f, 8.7637f}},
        {"user-extra-tunnel-wall", {-0.0465f, 1.6500f, 10.7064f}, {3.1123f, 0.3881f, 3.4656f}},
        {"user-extra-wall-angled", {-5.9585f, 1.6500f, 6.7412f}, {-13.3023f, -0.4376f, 9.1310f}},
        {"wall-blocks", {-12.5f, 1.7f, 1.0f}, {2.0f, 1.0f, -5.0f}},
        {"user-lighting-before-chunk", {14.6681f, 1.7000f, 4.7285f}, {7.2922f, -0.4400f, 2.4888f}},
        {"user-lighting-after-chunk", {15.5564f, 1.7000f, 18.2764f}, {10.3579f, -0.7770f, 12.7230f}},
    };

    bool ok = expect_true(renderer.edge_filter_compute_ready, "Edge compute shader loaded");
    ok = expect_true(dim != nullptr, "Edge comparison has a generated dimension") && ok;
    ol::renderer_ensure_target(&renderer);
    for (const EdgeCompareView& view_def : views) {
        if (!dim) break;
        const ol::CameraView view = make_edge_compare_camera(app->dimension_id, dim, view_def);
        const bool scaled_repro =
            std::strcmp(view_def.name, "user-extra-edges") == 0 ||
            std::strcmp(view_def.name, "user-uneven-thickness") == 0;
        const int max_scale_power = scaled_repro ? 3 : 0;

        for (int scale_power = 0; scale_power <= max_scale_power; ++scale_power) {
            renderer.scale_power = scale_power;
            ol::renderer_ensure_target(&renderer);

            char suffix[16]{};
            if (scale_power > 0) std::snprintf(suffix, sizeof(suffix), "-s%d", scale_power);
            char old_path[128]{};
            char new_path[128]{};
            char gpu_path[128]{};
            char shader_path[128]{};
            char ref_path[128]{};
            std::snprintf(old_path, sizeof(old_path), "test-output/edge-compare-%s%s-2d.png", view_def.name, suffix);
            std::snprintf(new_path, sizeof(new_path), "test-output/edge-compare-%s%s-depth.png", view_def.name, suffix);
            std::snprintf(gpu_path, sizeof(gpu_path), "test-output/edge-compare-%s%s-depth-gpu.png", view_def.name, suffix);
            std::snprintf(shader_path, sizeof(shader_path), "test-output/edge-compare-%s%s-depth-shader.png", view_def.name, suffix);
            std::snprintf(ref_path, sizeof(ref_path), "test-output/edge-compare-%s%s-reference.png", view_def.name, suffix);

            renderer.depth_test_edges = false;
            renderer.shader_depth_edges = false;
            renderer.gpu_depth_edges = false;
            renderer.brute_force_edge_occlusion = false;
            ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
            ok = expect_true(export_render_target(&renderer, old_path), "2D edge comparison image exported") && ok;

            renderer.depth_test_edges = true;
            renderer.shader_depth_edges = false;
            renderer.gpu_depth_edges = false;
            renderer.brute_force_edge_occlusion = false;
            ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
            Image cpu_depth = capture_render_target(&renderer);
            ok = expect_true(export_captured_image(cpu_depth, new_path), "Depth edge comparison image exported") && ok;

            renderer.depth_test_edges = true;
            renderer.shader_depth_edges = false;
            renderer.gpu_depth_edges = true;
            renderer.brute_force_edge_occlusion = false;
            ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
            Image gpu_depth = capture_render_target(&renderer);
            ok = expect_true(export_captured_image(gpu_depth, gpu_path), "GPU depth edge comparison image exported") && ok;

            renderer.depth_test_edges = true;
            renderer.shader_depth_edges = true;
            renderer.gpu_depth_edges = false;
            renderer.brute_force_edge_occlusion = false;
            ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
            Image shader_depth = capture_render_target(&renderer);
            ok = expect_true(export_captured_image(shader_depth, shader_path), "Shader depth edge comparison image exported") && ok;
            if (std::strcmp(view_def.name, "overview") == 0 && scale_power == 0) {
                renderer.frustum_cull_meshes = false;
                ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
                Image uncull_reference = capture_render_target(&renderer);
                renderer.frustum_cull_meshes = true;
                const ImageDiffStats cull_diff = compare_images(
                    shader_depth, uncull_reference, 0);
                ok = expect_true(
                    cull_diff.changed_pixels == 0 &&
                    cull_diff.mean_abs_rgb == 0.0 &&
                    cull_diff.max_delta == 0,
                    "Playground mesh/edge culling preserves screen-edge meshes and sprites") && ok;
                UnloadImage(uncull_reference);
            }

            const ImageDiffStats diff = compare_images(cpu_depth, gpu_depth, 24);
            const ImageDiffStats shader_diff = compare_images(cpu_depth, shader_depth, 24);
            const double total_pixels = static_cast<double>(diff.width) * static_cast<double>(diff.height);
            const double changed_pct = total_pixels > 0.0 ? static_cast<double>(diff.changed_pixels) * 100.0 / total_pixels : 100.0;
            const double edge_changed_pct = total_pixels > 0.0 ? static_cast<double>(diff.edge_disagreement_pixels) * 100.0 / total_pixels : 100.0;
            const double cpu_only_pct = total_pixels > 0.0 ? static_cast<double>(diff.a_edge_only_pixels) * 100.0 / total_pixels : 100.0;
            const double gpu_only_pct = total_pixels > 0.0 ? static_cast<double>(diff.b_edge_only_pixels) * 100.0 / total_pixels : 100.0;
            std::printf(
                "edge GPU compare %s scale=1/%d native=%dx%d: changed=%llu (%.3f%%) edge_disagree=%llu (%.3f%%) cpu_only=%llu gpu_only=%llu mean_abs=%.3f max_delta=%d\n",
                view_def.name,
                1 << scale_power,
                renderer.native_w,
                renderer.native_h,
                static_cast<unsigned long long>(diff.changed_pixels),
                changed_pct,
                static_cast<unsigned long long>(diff.edge_disagreement_pixels),
                edge_changed_pct,
                static_cast<unsigned long long>(diff.a_edge_only_pixels),
                static_cast<unsigned long long>(diff.b_edge_only_pixels),
                diff.mean_abs_rgb,
                diff.max_delta);
            const double mean_abs_limit = scale_power > 0 ? 2.0 : 1.25;
            ok = expect_true(changed_pct < 2.5 && edge_changed_pct < 1.5 && diff.mean_abs_rgb < mean_abs_limit,
                "GPU depth edges visually match CPU depth edges") && ok;
            const double shader_pixels = static_cast<double>(shader_diff.width) * static_cast<double>(shader_diff.height);
            const double shader_changed_pct = shader_pixels > 0.0
                ? static_cast<double>(shader_diff.changed_pixels) * 100.0 / shader_pixels : 100.0;
            const double shader_edge_changed_pct = shader_pixels > 0.0
                ? static_cast<double>(shader_diff.edge_disagreement_pixels) * 100.0 / shader_pixels : 100.0;
            std::printf(
                "edge shader compare %s scale=1/%d: changed=%llu (%.3f%%) edge_disagree=%llu (%.3f%%) mean_abs=%.3f max_delta=%d\n",
                view_def.name, 1 << scale_power,
                static_cast<unsigned long long>(shader_diff.changed_pixels), shader_changed_pct,
                static_cast<unsigned long long>(shader_diff.edge_disagreement_pixels), shader_edge_changed_pct,
                shader_diff.mean_abs_rgb, shader_diff.max_delta);
            const double shader_scale_tolerance = std::sqrt(static_cast<double>(1 << scale_power));
            ok = expect_true(
                shader_changed_pct < 2.5 * shader_scale_tolerance &&
                shader_edge_changed_pct < 1.5 * shader_scale_tolerance &&
                shader_diff.mean_abs_rgb < 1.25 * shader_scale_tolerance,
                "Shader depth edges visually match CPU depth edges") && ok;
            if (std::strcmp(view_def.name, "user-extra-tunnel-wall") == 0) {
                ok = expect_true(gpu_only_pct < 0.02, "GPU depth does not add tunnel/wall edge pixels") && ok;
            }
            if (std::strcmp(view_def.name, "user-extra-wall-angled") == 0) {
                ok = expect_true(cpu_only_pct < 0.02, "GPU depth does not drop angled wall edge pixels") && ok;
            }

            UnloadImage(cpu_depth);
            UnloadImage(gpu_depth);
            UnloadImage(shader_depth);

            renderer.depth_test_edges = true;
            renderer.shader_depth_edges = false;
            renderer.gpu_depth_edges = false;
            renderer.brute_force_edge_occlusion = true;
            ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
            ok = expect_true(export_render_target(&renderer, ref_path), "Reference edge comparison image exported") && ok;
        }
    }

    ol::renderer_shutdown(&renderer);
    CloseWindow();
    if (ok) std::printf("edge comparison images exported to test-output/edge-compare-*.png\n");
    return ok;
}

bool run_hills_render_benchmark() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(960, 540, "ol hills render benchmark");

    ol::RenderState renderer{};
    ol::renderer_init(&renderer);
    renderer.lighting.enabled = false; // This benchmark isolates the edge pipelines.

    const Vector3 feet_m = {-2.4580f, 2.3080f, 71.6100f};
    const ol::WorldPos saved_feet = test_pos(0, feet_m, 16.0f);
    const EdgeCompareView view_def = {
        "hills-lag",
        {-2.4580f, 4.0080f, 71.6100f},
        {-1.0408f, 4.3231f, 79.4772f}
    };
    const HillsDiagnosticView hills_diagnostic_views[] = {
        {
            {"hills-fog-overlap", {-494.8463f, 2.2478f, -202.1192f}, {-492.6794f, 2.8281f, -194.4402f}},
            {-494.8463f, 0.5478f, -202.1192f}
        },
        {
            {"hills-strange-edges-a", {-1736.5785f, 3.6916f, -442.5047f}, {-1743.8311f, 2.8849f, -439.2261f}},
            {-1736.5785f, 1.9916f, -442.5047f}
        },
        {
            {"hills-strange-edges-b", {-1755.7123f, 4.1697f, -437.6480f}, {-1762.6770f, 3.5032f, -433.7688f}},
            {-1755.7123f, 2.4697f, -437.6480f}
        },
    };

    bool ok = expect_true(renderer.edge_filter_compute_ready, "Edge compute shader loaded for hills benchmark");
    ol::renderer_ensure_target(&renderer);

    const int radii[] = {5, 16, 20, ol::pause_render_radius_max};
    ol::u32 reference_lighting_surfaces = 0;
    ol::u32 reference_lighting_triangles = 0;
    for (int radius : radii) {
        auto app = std::make_unique<ol::DemoApp>();
        std::snprintf(app->world_name, sizeof(app->world_name), "hills");
        std::snprintf(app->session_name, sizeof(app->session_name), "render-benchmark-r%d", radius);
        app->profile.render_radius_chunks = radius;
        app->profile.session_count = 1;
        ol::SavedSessionState& session = app->profile.sessions[0];
        session.valid = true;
        std::snprintf(session.name, sizeof(session.name), "%s", app->session_name);
        std::snprintf(session.world_name, sizeof(session.world_name), "%s", app->world_name);
        session.player_count = 1;
        session.players[0].valid = true;
        session.players[0].peer_id = app->net.local_peer_id;
        session.players[0].chunk = saved_feet.chunk;
        session.players[0].local = saved_feet.local;

        ol::demo_generate_world(app.get());
        ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);
        const ol::CameraView view = dim ? make_edge_compare_camera(app->dimension_id, dim, view_def) : ol::CameraView{};
        ok = expect_true(dim != nullptr, "Hills benchmark generated a dimension") && ok;
        if (!dim) continue;

        // Keep one real mesh paint cache alive so the benchmark covers saved
        // painted sessions. Its steady update cost must not scale with the
        // thousands of unrelated far meshes admitted by a large render radius.
        ol::u32 benchmark_painted_mesh = ol::invalid_id;
        float benchmark_painted_distance = std::numeric_limits<float>::max();
        for (ol::u32 mesh_slot = 0; mesh_slot < dim->meshes.count; ++mesh_slot) {
            const ol::MeshInstance& mesh = dim->meshes.data[mesh_slot];
            if (!std::strstr(mesh.name, "terrain")) continue;
            const float distance = Vector3Length(ol::world_delta_meters(
                mesh.origin, view.anchor, dim->chunk_size_m));
            if (distance >= benchmark_painted_distance) continue;
            benchmark_painted_distance = distance;
            benchmark_painted_mesh = ol::arena_id_at_slot(&dim->meshes, mesh_slot);
        }
        if (const ol::MeshInstance* mesh = ol::arena_get(&dim->meshes, benchmark_painted_mesh)) {
            ol::PaintedPixel paint{};
            paint.center = ol::worldpos_offset(
                mesh->origin, {0.0f, std::fabs(mesh->se3.m5) * 0.5f + 0.001f, 0.0f},
                dim->chunk_size_m);
            paint.normal = {0.0f, 1.0f, 0.0f};
            paint.tangent = {1.0f, 0.0f, 0.0f};
            paint.color = ORANGE;
            paint.mesh_id = benchmark_painted_mesh;
            ok = expect_true(
                ol::dimension_paint_pixel(dim, paint),
                "Hills benchmark creates a persisted mesh paint pixel") && ok;
        }

        if (radius == ol::pause_render_radius_max) {
            for (const HillsDiagnosticView& diag_view : hills_diagnostic_views) {
                auto diag_app = std::make_unique<ol::DemoApp>();
                std::snprintf(diag_app->world_name, sizeof(diag_app->world_name), "hills");
                std::snprintf(diag_app->session_name, sizeof(diag_app->session_name), "render-diagnostic-%s-r%d", diag_view.view.name, radius);
                diag_app->profile.render_radius_chunks = radius;
                diag_app->profile.session_count = 1;
                ol::SavedSessionState& diag_session = diag_app->profile.sessions[0];
                diag_session.valid = true;
                std::snprintf(diag_session.name, sizeof(diag_session.name), "%s", diag_app->session_name);
                std::snprintf(diag_session.world_name, sizeof(diag_session.world_name), "%s", diag_app->world_name);
                diag_session.player_count = 1;
                diag_session.players[0].valid = true;
                diag_session.players[0].peer_id = diag_app->net.local_peer_id;
                const ol::WorldPos hills_feet = test_pos(0, diag_view.feet, 16.0f);
                diag_session.players[0].chunk = hills_feet.chunk;
                diag_session.players[0].local = hills_feet.local;

                ol::demo_generate_world(diag_app.get());
                ol::Dimension* hills_dim = ol::world_get_dimension(&diag_app->world, diag_app->dimension_id);
                ok = expect_true(hills_dim != nullptr, "Hills overlap diagnostic generated a dimension") && ok;
                if (!hills_dim) continue;

                const ol::CameraView hills_view = make_edge_compare_camera(diag_app->dimension_id, hills_dim, diag_view.view);

                char flat_path[160]{};
                char cpu_path[160]{};
                char gpu_path[160]{};
                char shader_path[160]{};
                std::snprintf(flat_path, sizeof(flat_path), "test-output/%s-flat.png", diag_view.view.name);
                std::snprintf(cpu_path, sizeof(cpu_path), "test-output/%s-cpu-depth.png", diag_view.view.name);
                std::snprintf(gpu_path, sizeof(gpu_path), "test-output/%s-gpu-depth.png", diag_view.view.name);
                std::snprintf(shader_path, sizeof(shader_path), "test-output/%s-shader-depth.png", diag_view.view.name);

                renderer.depth_test_edges = false;
                renderer.shader_depth_edges = false;
                renderer.gpu_depth_edges = false;
                ol::renderer_render_dimension_to_target(&renderer, hills_dim, hills_view, diag_app->local_player_id);
                ok = expect_true(export_render_target(&renderer, flat_path), "Hills overlap flat image exported") && ok;

                renderer.depth_test_edges = true;
                renderer.shader_depth_edges = false;
                renderer.gpu_depth_edges = false;
                ol::renderer_render_dimension_to_target(&renderer, hills_dim, hills_view, diag_app->local_player_id);
                Image hills_cpu_depth = capture_render_target(&renderer);
                ok = expect_true(export_captured_image(hills_cpu_depth, cpu_path), "Hills overlap CPU-depth image exported") && ok;

                renderer.depth_test_edges = true;
                renderer.shader_depth_edges = false;
                renderer.gpu_depth_edges = true;
                ol::renderer_render_dimension_to_target(&renderer, hills_dim, hills_view, diag_app->local_player_id);
                Image hills_gpu_depth = capture_render_target(&renderer);
                ok = expect_true(export_captured_image(hills_gpu_depth, gpu_path), "Hills overlap GPU-depth image exported") && ok;

                renderer.depth_test_edges = true;
                renderer.shader_depth_edges = true;
                renderer.gpu_depth_edges = false;
                ol::renderer_render_dimension_to_target(&renderer, hills_dim, hills_view, diag_app->local_player_id);
                Image hills_shader_depth = capture_render_target(&renderer);
                ok = expect_true(export_captured_image(hills_shader_depth, shader_path), "Hills overlap shader-depth image exported") && ok;

                renderer.frustum_cull_meshes = false;
                ol::renderer_render_dimension_to_target(
                    &renderer, hills_dim, hills_view, diag_app->local_player_id);
                Image hills_unculled = capture_render_target(&renderer);
                renderer.frustum_cull_meshes = true;
                const ImageDiffStats hills_cull_diff = compare_images(
                    hills_shader_depth, hills_unculled, 0);
                std::printf(
                    "hills frustum compare %s: changed=%llu mean_abs=%.4f max_delta=%d\n",
                    diag_view.view.name,
                    static_cast<unsigned long long>(hills_cull_diff.changed_pixels),
                    hills_cull_diff.mean_abs_rgb,
                    hills_cull_diff.max_delta);
                ok = expect_true(
                    hills_cull_diff.changed_pixels == 0 &&
                    hills_cull_diff.mean_abs_rgb == 0.0 &&
                    hills_cull_diff.max_delta == 0,
                    "Conservative hills mesh/edge frustum culling is pixel exact") && ok;

                const ImageDiffStats hills_diff = compare_images(hills_cpu_depth, hills_gpu_depth, 24);
                const ImageDiffStats hills_shader_diff = compare_images(hills_cpu_depth, hills_shader_depth, 24);
                const double hills_pixels = static_cast<double>(hills_diff.width) * static_cast<double>(hills_diff.height);
                const double hills_changed_pct = hills_pixels > 0.0 ? static_cast<double>(hills_diff.changed_pixels) * 100.0 / hills_pixels : 100.0;
                const double hills_edge_changed_pct = hills_pixels > 0.0 ? static_cast<double>(hills_diff.edge_disagreement_pixels) * 100.0 / hills_pixels : 100.0;
                std::printf(
                    "hills GPU compare %s: changed=%llu (%.3f%%) edge_disagree=%llu (%.3f%%) cpu_only=%llu gpu_only=%llu mean_abs=%.3f max_delta=%d\n",
                    diag_view.view.name,
                    static_cast<unsigned long long>(hills_diff.changed_pixels),
                    hills_changed_pct,
                    static_cast<unsigned long long>(hills_diff.edge_disagreement_pixels),
                    hills_edge_changed_pct,
                    static_cast<unsigned long long>(hills_diff.a_edge_only_pixels),
                    static_cast<unsigned long long>(hills_diff.b_edge_only_pixels),
                    hills_diff.mean_abs_rgb,
                    hills_diff.max_delta);
                ok = expect_true(hills_changed_pct < 2.5 && hills_edge_changed_pct < 1.5 && hills_diff.mean_abs_rgb < 1.25,
                    "Hills GPU depth edges match CPU depth edges") && ok;
                const double hills_shader_changed_pct = hills_pixels > 0.0
                    ? static_cast<double>(hills_shader_diff.changed_pixels) * 100.0 / hills_pixels : 100.0;
                const double hills_shader_edge_changed_pct = hills_pixels > 0.0
                    ? static_cast<double>(hills_shader_diff.edge_disagreement_pixels) * 100.0 / hills_pixels : 100.0;
                std::printf(
                    "hills shader compare %s: changed=%llu (%.3f%%) edge_disagree=%llu (%.3f%%) "
                    "cpu_only=%llu shader_only=%llu mean_abs=%.3f max_delta=%d\n",
                    diag_view.view.name,
                    static_cast<unsigned long long>(hills_shader_diff.changed_pixels),
                    hills_shader_changed_pct,
                    static_cast<unsigned long long>(hills_shader_diff.edge_disagreement_pixels),
                    hills_shader_edge_changed_pct,
                    static_cast<unsigned long long>(hills_shader_diff.a_edge_only_pixels),
                    static_cast<unsigned long long>(hills_shader_diff.b_edge_only_pixels),
                    hills_shader_diff.mean_abs_rgb,
                    hills_shader_diff.max_delta);
                ok = expect_true(
                    hills_shader_changed_pct < 2.5 && hills_shader_edge_changed_pct < 1.5 &&
                    hills_shader_diff.mean_abs_rgb < 1.25,
                    "Hills shader depth edges match CPU depth edges") && ok;
                UnloadImage(hills_cpu_depth);
                UnloadImage(hills_gpu_depth);
                UnloadImage(hills_shader_depth);
                UnloadImage(hills_unculled);
            }
        }

        ol::u32 streamed_valid_chunks = 0;
        ol::u32 streamed_collider_chunks = 0;
        for (const ol::StreamedWorldChunk& chunk : app->streamed_chunks) {
            if (!chunk.valid) continue;
            ++streamed_valid_chunks;
            if (chunk.colliders_loaded) ++streamed_collider_chunks;
        }

        ol::u32 visible_chunks = 0;
        ol::u32 visible_mesh_refs = 0;
        ol::u32 visible_edge_mesh_refs = 0;
        ol::u32 visible_triangles = 0;
        ol::u32 visible_edges = 0;
        for (ol::u32 chunk_slot = 0; chunk_slot < dim->chunks.count; ++chunk_slot) {
            const ol::Chunk* chunk = &dim->chunks.data[chunk_slot];
            const float dx = static_cast<float>(chunk->coord.x - view.anchor.chunk.x);
            const float dy = static_cast<float>(chunk->coord.y - view.anchor.chunk.y);
            const float dz = static_cast<float>(chunk->coord.z - view.anchor.chunk.z);
            const float chunk_dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (chunk_dist > static_cast<float>(dim->render_radius_chunks)) continue;

            ++visible_chunks;
            for (ol::u32 i = 0; i < chunk->mesh_count; ++i) {
                const ol::MeshInstance* mesh = ol::arena_get(&dim->meshes, chunk->meshes[i]);
                if (!mesh || !mesh->visible) continue;

                const ol::MeshGeometry* geometry = ol::arena_get(&dim->geometries, mesh->geometry);
                if (!geometry) continue;
                if (chunk_dist > static_cast<float>(dim->quality_render_radius_chunks)) {
                    if (!ol::id_valid(geometry->lod_geometry)) continue;
                    geometry = ol::arena_get(&dim->geometries, geometry->lod_geometry);
                    if (!geometry) continue;
                }

                ++visible_mesh_refs;
                visible_triangles += geometry->triangle_count;
                if (mesh->draw_edges) {
                    ++visible_edge_mesh_refs;
                    visible_edges += geometry->edge_count;
                }
            }
        }

        std::printf(
            "hills radius %d setup: quality=%d chunks=%u streamed=%u collider_chunks=%u meshes=%u physics_boxes=%u visible_chunks=%u visible_meshes=%u edge_meshes=%u visible_tris=%u visible_edges=%u\n",
            dim->render_radius_chunks,
            dim->quality_render_radius_chunks,
            dim->chunks.count,
            streamed_valid_chunks,
            streamed_collider_chunks,
            dim->meshes.count,
            dim->physics.boxes.count,
            visible_chunks,
            visible_mesh_refs,
            visible_edge_mesh_refs,
            visible_triangles,
            visible_edges);

        constexpr int warmup_frames = 3;
        constexpr int measured_frames = 30;
        renderer.lighting.enabled = false;
        double unlit_shader_average_ms = 0.0;
        for (int mode = 0; mode < 4; ++mode) {
            const char* mode_name = "flat";
            renderer.depth_test_edges = false;
            renderer.shader_depth_edges = false;
            renderer.gpu_depth_edges = false;
            if (mode == 1) {
                mode_name = "cpu-depth";
                renderer.depth_test_edges = true;
                renderer.shader_depth_edges = false;
                renderer.gpu_depth_edges = false;
            } else if (mode == 2) {
                mode_name = "compute-depth";
                renderer.depth_test_edges = true;
                renderer.shader_depth_edges = false;
                renderer.gpu_depth_edges = true;
            } else if (mode == 3) {
                mode_name = "shader-depth";
                renderer.depth_test_edges = true;
                renderer.shader_depth_edges = true;
                renderer.gpu_depth_edges = false;
            }
            for (int i = 0; i < warmup_frames; ++i) {
                ol::renderer_render_dimension_to_target(&renderer, dim, view, app->local_player_id);
            }

            double scan_ms = 0.0;
            double prepare_ms = 0.0;
            double world_draw_ms = 0.0;
            double paint_update_ms = 0.0;
            double lighting_update_ms = 0.0;
            double lighting_draw_ms = 0.0;
            double post_world_ms = 0.0;
            const double start = GetTime();
            for (int i = 0; i < measured_frames; ++i) {
                ol::renderer_render_dimension_to_target(&renderer, dim, view, app->local_player_id);
                scan_ms += renderer.debug_chunk_mesh_scan_ms;
                prepare_ms += renderer.debug_mesh_prepare_ms;
                world_draw_ms += renderer.debug_world_draw_ms;
                paint_update_ms += renderer.debug_paint_update_ms;
                lighting_update_ms += renderer.debug_lighting_update_ms;
                lighting_draw_ms += renderer.debug_lighting_draw_ms;
                post_world_ms += renderer.debug_post_world_ms;
            }
            const double elapsed_ms = (GetTime() - start) * 1000.0;
            if (mode == 3) {
                unlit_shader_average_ms = elapsed_ms / static_cast<double>(measured_frames);
            }
            std::printf(
                "hills radius %d render benchmark (%s edges): frames=%d avg_ms=%.3f "
                "visible_chunks=%u submitted_meshes=%u scan=%.3f prepare=%.3f world=%.3f "
                "paint=%.4f light_update=%.3f light_draw=%.3f post=%.3f "
                "edges=%u tris=%u unbounded=%u samples=%llu candidates=%llu ray_tests=%llu\n",
                dim->render_radius_chunks,
                mode_name,
                measured_frames,
                elapsed_ms / static_cast<double>(measured_frames),
                renderer.debug_visible_chunk_count,
                renderer.debug_visible_mesh_count,
                scan_ms / measured_frames,
                prepare_ms / measured_frames,
                world_draw_ms / measured_frames,
                paint_update_ms / measured_frames,
                lighting_update_ms / measured_frames,
                lighting_draw_ms / measured_frames,
                post_world_ms / measured_frames,
                renderer.debug_edge_count,
                renderer.debug_scene_triangle_count,
                renderer.debug_unbounded_scene_triangle_count,
                static_cast<unsigned long long>(renderer.debug_edge_sample_count),
                static_cast<unsigned long long>(renderer.debug_edge_candidate_count),
                static_cast<unsigned long long>(renderer.debug_edge_ray_test_count));
        }

        renderer.lighting.enabled = true;
        renderer.depth_test_edges = true;
        renderer.shader_depth_edges = true;
        renderer.gpu_depth_edges = false;
        constexpr int lit_warmup_frames = 8;
        constexpr int lit_measured_frames = 20;
        for (int frame = 0; frame < lit_warmup_frames; ++frame) {
            ol::renderer_render_dimension_to_target(
                &renderer, dim, view, app->local_player_id);
        }
        double lit_scan_ms = 0.0;
        double lit_world_ms = 0.0;
        double lit_paint_ms = 0.0;
        double lit_update_ms = 0.0;
        double lit_draw_ms = 0.0;
        double lit_signature_ms = 0.0;
        double lit_emitter_ms = 0.0;
        double lit_topology_ms = 0.0;
        double lit_cascade_ms = 0.0;
        const double lit_start = GetTime();
        for (int frame = 0; frame < lit_measured_frames; ++frame) {
            ol::renderer_render_dimension_to_target(
                &renderer, dim, view, app->local_player_id);
            lit_scan_ms += renderer.debug_chunk_mesh_scan_ms;
            lit_world_ms += renderer.debug_world_draw_ms;
            lit_paint_ms += renderer.debug_paint_update_ms;
            lit_update_ms += renderer.debug_lighting_update_ms;
            lit_draw_ms += renderer.debug_lighting_draw_ms;
            lit_signature_ms += renderer.debug_radiance_signature_ms;
            lit_emitter_ms += renderer.debug_radiance_emitter_ms;
            lit_topology_ms += renderer.debug_radiance_topology_ms;
            lit_cascade_ms += renderer.debug_radiance_cascade_ms;
        }
        const double lit_average_ms =
            (GetTime() - lit_start) * 1000.0 / static_cast<double>(lit_measured_frames);
        std::printf(
            "hills radius %d lighting steady: avg_ms=%.3f unlit_ms=%.3f overhead_ms=%.3f "
            "scan=%.3f world=%.3f paint=%.4f update=%.3f draw=%.3f "
            "signature=%.4f emitter=%.4f topology=%.4f cascade=%.3f "
            "surfaces=%u static_tris=%u submitted_meshes=%u\n",
            dim->render_radius_chunks,
            lit_average_ms,
            unlit_shader_average_ms,
            lit_average_ms - unlit_shader_average_ms,
            lit_scan_ms / lit_measured_frames,
            lit_world_ms / lit_measured_frames,
            lit_paint_ms / lit_measured_frames,
            lit_update_ms / lit_measured_frames,
            lit_draw_ms / lit_measured_frames,
            lit_signature_ms / lit_measured_frames,
            lit_emitter_ms / lit_measured_frames,
            lit_topology_ms / lit_measured_frames,
            lit_cascade_ms / lit_measured_frames,
            renderer.debug_radiance_surface_count,
            renderer.debug_radiance_triangle_count,
            renderer.debug_visible_mesh_count);
        if (reference_lighting_surfaces == 0) {
            reference_lighting_surfaces = renderer.debug_radiance_surface_count;
            reference_lighting_triangles = renderer.debug_radiance_triangle_count;
        } else {
            ok = expect_true(
                renderer.debug_radiance_surface_count == reference_lighting_surfaces &&
                renderer.debug_radiance_triangle_count == reference_lighting_triangles,
                "Hills lighting topology is independent of the far render radius") && ok;
        }
        ok = expect_true(
            !renderer.mesh_paint_surfaces.empty(),
            "Hills steady benchmark retains the painted mesh cache") && ok;

        if (radius != 5) {
            const std::vector<ol::PaintedPixel> saved_benchmark_paint = dim->painted_pixels;
            dim->painted_pixels.clear();
            ++dim->paint_revision;
            for (int frame = 0; frame < 4; ++frame) {
                ol::renderer_render_dimension_to_target(
                    &renderer, dim, view, app->local_player_id);
            }
            constexpr int unpainted_measured_frames = 10;
            double unpainted_paint_ms = 0.0;
            double unpainted_lighting_ms = 0.0;
            const double unpainted_start = GetTime();
            for (int frame = 0; frame < unpainted_measured_frames; ++frame) {
                ol::renderer_render_dimension_to_target(
                    &renderer, dim, view, app->local_player_id);
                unpainted_paint_ms += renderer.debug_paint_update_ms;
                unpainted_lighting_ms += renderer.debug_lighting_update_ms +
                    renderer.debug_lighting_draw_ms;
            }
            const double unpainted_average_ms =
                (GetTime() - unpainted_start) * 1000.0 /
                static_cast<double>(unpainted_measured_frames);
            std::printf(
                "hills radius %d unpainted steady: avg_ms=%.3f painted_delta_ms=%.3f "
                "paint=%.4f lighting=%.3f\n",
                dim->render_radius_chunks,
                unpainted_average_ms,
                lit_average_ms - unpainted_average_ms,
                unpainted_paint_ms / unpainted_measured_frames,
                unpainted_lighting_ms / unpainted_measured_frames);
            dim->painted_pixels = saved_benchmark_paint;
            ++dim->paint_revision;
            for (int frame = 0; frame < 4; ++frame) {
                ol::renderer_render_dimension_to_target(
                    &renderer, dim, view, app->local_player_id);
            }
        }
        renderer.lighting.enabled = false;

        if (radius == 5) {
            renderer.lighting.enabled = true;
            const EdgeCompareView before_boundary_def = {
                "hills-lighting-boundary-before",
                {-2.4580f, 4.0080f, 79.90f},
                {-1.0408f, 4.3231f, 87.77f}};
            const EdgeCompareView after_boundary_def = {
                "hills-lighting-boundary-after",
                {-2.4580f, 4.0080f, 80.10f},
                {-1.0408f, 4.3231f, 87.97f}};
            const ol::CameraView before_boundary = make_edge_compare_camera(app->dimension_id, dim, before_boundary_def);
            const ol::CameraView after_boundary = make_edge_compare_camera(app->dimension_id, dim, after_boundary_def);
            for (int i = 0; i < 4; ++i) {
                ol::renderer_render_dimension_to_target(&renderer, dim, before_boundary, app->local_player_id);
            }
            const ol::u64 before_signature = renderer.radiance_scene_signature;
            const double boundary_start = GetTime();
            ol::renderer_render_dimension_to_target(&renderer, dim, after_boundary, app->local_player_id);
            const double boundary_ms = (GetTime() - boundary_start) * 1000.0;
            const double boundary_scene_ms = renderer.debug_radiance_scene_build_ms;
            const double boundary_topology_ms = renderer.debug_radiance_topology_ms;
            const double boundary_cascade_ms = renderer.debug_radiance_cascade_ms;
            const bool boundary_background_used = renderer.debug_radiance_background_scene_used;
            const bool boundary_neighbor_promoted =
                renderer.debug_radiance_neighbor_promoted;
            const ol::u64 boundary_radiance_frame = renderer.radiance_frame;
            const ol::u32 boundary_surface_count = renderer.debug_radiance_surface_count;
            const ol::u32 boundary_surface_updates = renderer.debug_radiance_surface_updates;
            const ol::u32 boundary_surface_allocations = renderer.debug_radiance_surface_allocations;
            const ol::u64 boundary_surface_texels_submitted =
                renderer.debug_radiance_surface_texels_submitted;
            const ol::u32 boundary_sync_fallback_surfaces =
                renderer.debug_radiance_neighbor_sync_fallback_surface_count;
            const ol::u64 boundary_sync_fallback_texels =
                renderer.debug_radiance_neighbor_sync_fallback_texels;
            ol::u64 boundary_updated_texels = 0;
            ol::u64 boundary_max_surface_texels = 0;
            ol::u32 boundary_updated_surface_count = 0;
            float boundary_min_chunk_distance = std::numeric_limits<float>::max();
            float boundary_max_chunk_distance = 0.0f;
            for (const ol::MeshLightingSurfaceCache& surface : renderer.lighting_surfaces) {
                if (surface.last_update_frame != boundary_radiance_frame) continue;
                const ol::u64 texels = static_cast<ol::u64>(std::max(0, surface.width)) *
                    static_cast<ol::u64>(std::max(0, surface.height));
                boundary_updated_texels += texels;
                boundary_max_surface_texels = std::max(boundary_max_surface_texels, texels);
                boundary_min_chunk_distance = std::min(
                    boundary_min_chunk_distance, surface.chunk_distance);
                boundary_max_chunk_distance = std::max(
                    boundary_max_chunk_distance, surface.chunk_distance);
                ++boundary_updated_surface_count;
            }
            if (boundary_updated_surface_count == 0) boundary_min_chunk_distance = 0.0f;
            const double steady_start = GetTime();
            ol::renderer_render_dimension_to_target(&renderer, dim, after_boundary, app->local_player_id);
            const double steady_ms = (GetTime() - steady_start) * 1000.0;
            std::printf(
                "hills lighting boundary: frame_ms=%.3f steady_ms=%.3f scene=%.3f topology=%.3f cascade=%.3f background=%d promoted=%d surfaces=%u updates=%u allocations=%u updated_surfaces=%u total_texels=%llu max_texels=%llu submitted_texels=%llu fallback_surfaces=%u fallback_texels=%llu chunk_distance=[%.3f,%.3f] static_tris=%u dynamic_tris=%u\n",
                boundary_ms,
                steady_ms,
                boundary_scene_ms,
                boundary_topology_ms,
                boundary_cascade_ms,
                boundary_background_used ? 1 : 0,
                boundary_neighbor_promoted ? 1 : 0,
                boundary_surface_count,
                boundary_surface_updates,
                boundary_surface_allocations,
                boundary_updated_surface_count,
                static_cast<unsigned long long>(boundary_updated_texels),
                static_cast<unsigned long long>(boundary_max_surface_texels),
                static_cast<unsigned long long>(boundary_surface_texels_submitted),
                boundary_sync_fallback_surfaces,
                static_cast<unsigned long long>(boundary_sync_fallback_texels),
                boundary_min_chunk_distance,
                boundary_max_chunk_distance,
                renderer.debug_radiance_triangle_count,
                renderer.debug_radiance_dynamic_triangle_count);
            ok = expect_true(
                before_signature != renderer.radiance_scene_signature &&
                renderer.debug_radiance_cascade_update_frame == renderer.radiance_frame &&
                (boundary_neighbor_promoted ||
                 (boundary_sync_fallback_surfaces > 0 && boundary_sync_fallback_texels > 0)),
                "Hills boundary uses a staged promotion or reports an explicit synchronous fallback") && ok;

            // Model a roughly 10 m/s run through the same seam.  This captures the
            // approach frames where staging should make progress, not just the one
            // promotion/fallback frame.
            constexpr int moving_seam_frames = 49;
            constexpr float moving_start_z = 74.0f;
            constexpr float moving_end_z = 82.0f;
            const Vector3 moving_target_offset = {1.4172f, 0.3151f, 7.87f};
            ol::PlayerEntity* moving_player = ol::arena_get(
                &dim->players, app->local_player_id);
            auto moving_view_at = [&](float z) {
                EdgeCompareView definition{};
                definition.name = "hills-moving-seam";
                definition.eye = {-2.4580f, 4.0080f, z};
                definition.target = definition.eye + moving_target_offset;
                return make_edge_compare_camera(app->dimension_id, dim, definition);
            };
            const ol::CameraView moving_warm_view = moving_view_at(moving_start_z);
            if (moving_player) {
                move_test_player_to(
                    dim, app->local_player_id,
                    test_pos(
                        app->dimension_id,
                        {-2.4580f, 2.3080f, moving_start_z},
                        dim->chunk_size_m));
            }
            for (int frame = 0; frame < 4; ++frame) {
                ol::renderer_render_dimension_to_target(
                    &renderer, dim, moving_warm_view, app->local_player_id);
            }
            std::vector<double> moving_frame_ms{};
            moving_frame_ms.reserve(moving_seam_frames);
            ol::u64 moving_neighbor_texels = 0;
            ol::u64 moving_surface_texels_submitted = 0;
            ol::u64 moving_fallback_texels = 0;
            ol::u64 moving_max_fallback_texels = 0;
            ol::u32 moving_fallback_surfaces = 0;
            ol::u32 moving_max_fallback_surfaces = 0;
            ol::u32 moving_promotions = 0;
            ol::u32 moving_scene_ready_frames = 0;
            ol::u32 moving_cascade_ready_frames = 0;
            ol::u32 moving_max_prepared_surfaces = 0;
            ol::u32 moving_max_complete_surfaces = 0;
            ol::u32 moving_max_retired_targets = 0;
            for (int frame = 0; frame < moving_seam_frames; ++frame) {
                const float t = static_cast<float>(frame) /
                    static_cast<float>(moving_seam_frames - 1);
                const float z = moving_start_z + (moving_end_z - moving_start_z) * t;
                if (moving_player) {
                    move_test_player_to(
                        dim, app->local_player_id,
                        test_pos(
                            app->dimension_id,
                            {-2.4580f, 2.3080f, z},
                            dim->chunk_size_m));
                }
                const ol::CameraView moving_view = moving_view_at(z);
                const double frame_start = GetTime();
                ol::renderer_render_dimension_to_target(
                    &renderer, dim, moving_view, app->local_player_id);
                moving_frame_ms.push_back((GetTime() - frame_start) * 1000.0);
                moving_neighbor_texels += renderer.debug_radiance_neighbor_texels_this_frame;
                moving_surface_texels_submitted +=
                    renderer.debug_radiance_surface_texels_submitted;
                moving_promotions += renderer.debug_radiance_neighbor_promoted ? 1u : 0u;
                moving_scene_ready_frames += renderer.debug_radiance_neighbor_scene_ready ? 1u : 0u;
                moving_cascade_ready_frames += renderer.debug_radiance_neighbor_cascade_ready ? 1u : 0u;
                moving_max_prepared_surfaces = std::max(
                    moving_max_prepared_surfaces,
                    renderer.debug_radiance_neighbor_surface_count);
                moving_max_complete_surfaces = std::max(
                    moving_max_complete_surfaces,
                    renderer.debug_radiance_neighbor_complete_surface_count);
                moving_max_retired_targets = std::max(
                    moving_max_retired_targets,
                    renderer.debug_radiance_neighbor_retired_target_count);
                const ol::u64 frame_fallback_texels =
                    renderer.debug_radiance_neighbor_sync_fallback_texels;
                const ol::u32 frame_fallback_surfaces =
                    renderer.debug_radiance_neighbor_sync_fallback_surface_count;
                moving_fallback_texels += frame_fallback_texels;
                moving_fallback_surfaces += frame_fallback_surfaces;
                moving_max_fallback_texels = std::max(
                    moving_max_fallback_texels, frame_fallback_texels);
                moving_max_fallback_surfaces = std::max(
                    moving_max_fallback_surfaces, frame_fallback_surfaces);
            }
            std::vector<double> sorted_moving_frame_ms = moving_frame_ms;
            std::sort(sorted_moving_frame_ms.begin(), sorted_moving_frame_ms.end());
            double moving_total_ms = 0.0;
            double moving_max_ms = 0.0;
            for (double frame_ms : moving_frame_ms) {
                moving_total_ms += frame_ms;
                moving_max_ms = std::max(moving_max_ms, frame_ms);
            }
            const size_t moving_p95_index = std::min(
                sorted_moving_frame_ms.size() - 1,
                static_cast<size_t>(std::ceil(
                    0.95 * static_cast<double>(sorted_moving_frame_ms.size()))) - 1);
            std::printf(
                "hills moving seam: frames=%d avg_ms=%.3f p95_ms=%.3f max_ms=%.3f "
                "scene_ready_frames=%u cascade_ready_frames=%u promotions=%u "
                "prepared_surfaces=%u complete_surfaces=%u staged_texels=%llu "
                "submitted_texels=%llu fallback_surfaces=%u max_fallback_surfaces=%u "
                "fallback_texels=%llu max_fallback_texels=%llu max_retired_targets=%u\n",
                moving_seam_frames,
                moving_total_ms / static_cast<double>(moving_frame_ms.size()),
                sorted_moving_frame_ms[moving_p95_index], moving_max_ms,
                moving_scene_ready_frames, moving_cascade_ready_frames, moving_promotions,
                moving_max_prepared_surfaces, moving_max_complete_surfaces,
                static_cast<unsigned long long>(moving_neighbor_texels),
                static_cast<unsigned long long>(moving_surface_texels_submitted),
                moving_fallback_surfaces, moving_max_fallback_surfaces,
                static_cast<unsigned long long>(moving_fallback_texels),
                static_cast<unsigned long long>(moving_max_fallback_texels),
                moving_max_retired_targets);
            ok = expect_true(
                moving_promotions >= 1 &&
                moving_max_prepared_surfaces > 0 &&
                moving_max_complete_surfaces > 0 &&
                moving_fallback_texels <= moving_surface_texels_submitted &&
                renderer.debug_radiance_cascade_update_frame == renderer.radiance_frame,
                "Moving hills seam promotes prepared lighting with bounded explicit fallback work") && ok;
            ol::PlayerEntity* stream_player = ol::arena_get(&dim->players, app->local_player_id);
            double maximum_stream_commit_ms = 0.0;
            ol::u32 maximum_stream_loads = 0;
            if (stream_player) {
                move_test_player_to(
                    dim, app->local_player_id,
                    test_pos(app->dimension_id, {-2.4580f, 2.3080f, 79.2f}, dim->chunk_size_m));
                stream_player->velocity = {0.0f, 0.0f, 4.0f};
                for (int frame = 0; frame < 32; ++frame) {
                    const double stream_start = GetTime();
                    ol::demo_update_landscape_streaming(app.get(), dim, stream_player);
                    maximum_stream_commit_ms = std::max(
                        maximum_stream_commit_ms, (GetTime() - stream_start) * 1000.0);
                    maximum_stream_loads = std::max(maximum_stream_loads, app->debug_landscape_chunks_loaded);
                }
            }
            std::printf(
                "hills streaming prefetch: max_commit_ms=%.3f max_chunk_loads=%u\n",
                maximum_stream_commit_ms, maximum_stream_loads);
            ok = expect_true(maximum_stream_loads <= 2,
                "Hills seam prefetch keeps chunk commits bounded") && ok;
            renderer.lighting.enabled = false;
        }
    }

    ol::renderer_shutdown(&renderer);
    CloseWindow();
    return ok;
}

struct TestDepthBuffer {
    std::vector<float> pixels{};
    int width = 0;
    int height = 0;
};

struct TestProjectedPoint {
    Vector2 screen{};
    float depth = 0.0f;
    Vector3 rel{};
};

struct TestTriangle {
    Vector3 a{};
    Vector3 b{};
    Vector3 c{};
};

Camera3D make_test_camera(const ol::CameraView& view, float fov) {
    Camera3D camera{};
    camera.position = {0.0f, view.eye_height, 0.0f};
    camera.target = camera.position + ol::forward_from_angles(view.yaw, view.pitch);
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = fov;
    camera.projection = CAMERA_PERSPECTIVE;
    return camera;
}

bool project_test_point(Vector3 p, const Matrix& view_matrix, const Matrix& projection_matrix, int width, int height, TestProjectedPoint* out) {
    Quaternion projected = {p.x, p.y, p.z, 1.0f};
    projected = QuaternionTransform(projected, view_matrix);
    projected = QuaternionTransform(projected, projection_matrix);
    if (std::fabs(projected.w) < 0.000001f) return false;

    const float inv_w = 1.0f / projected.w;
    const float ndc_x = projected.x * inv_w;
    const float ndc_y = projected.y * inv_w;
    const float ndc_z = projected.z * inv_w;
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) || !std::isfinite(ndc_z)) return false;

    out->screen = {
        (ndc_x + 1.0f) * 0.5f * static_cast<float>(width),
        (1.0f - ndc_y) * 0.5f * static_cast<float>(height)
    };
    out->depth = (ndc_z + 1.0f) * 0.5f;
    out->rel = p;
    return true;
}

TestDepthBuffer read_test_depth_target(const ol::RenderState& renderer) {
    TestDepthBuffer depth{};
    depth.width = renderer.native_w;
    depth.height = renderer.native_h;
    depth.pixels.resize(static_cast<size_t>(depth.width) * static_cast<size_t>(depth.height), 1.0f);

    rlDrawRenderBatchActive();
    rlEnableFramebuffer(renderer.target.id);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, depth.width, depth.height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.pixels.data());
    rlDisableFramebuffer();
    return depth;
}

bool test_depth_footprint_visible(const TestDepthBuffer& depth, Vector2 p, float edge_depth) {
    constexpr float edge_depth_bias = 0.00035f;
    const int center_x = static_cast<int>(std::floor(p.x + 0.5f));
    const int center_y = static_cast<int>(std::floor(p.y + 0.5f));
    if (center_x < 0 || center_x >= depth.width || center_y < 0 || center_y >= depth.height) return true;
    const int gl_y = depth.height - 1 - center_y;
    const float v = depth.pixels[static_cast<size_t>(gl_y) * static_cast<size_t>(depth.width) + static_cast<size_t>(center_x)];
    return edge_depth <= v + edge_depth_bias;
}

bool test_filter_visible(const TestDepthBuffer& depth, const TestProjectedPoint& a, const TestProjectedPoint& b, float t) {
    const Vector2 p = a.screen + (b.screen - a.screen) * t;
    const float edge_depth = a.depth + (b.depth - a.depth) * t;
    return test_depth_footprint_visible(depth, p, edge_depth);
}

bool ray_intersects_triangle(Vector3 origin, Vector3 dir, const TestTriangle& tri, float max_dist) {
    constexpr float eps = 0.00001f;
    const Vector3 e1 = tri.b - tri.a;
    const Vector3 e2 = tri.c - tri.a;
    const Vector3 h = Vector3CrossProduct(dir, e2);
    const float det = Vector3DotProduct(e1, h);
    if (std::fabs(det) < eps) return false;

    const float inv_det = 1.0f / det;
    const Vector3 s = origin - tri.a;
    const float u = inv_det * Vector3DotProduct(s, h);
    if (u < -eps || u > 1.0f + eps) return false;

    const Vector3 q = Vector3CrossProduct(s, e1);
    const float v = inv_det * Vector3DotProduct(dir, q);
    if (v < -eps || u + v > 1.0f + eps) return false;

    const float dist = inv_det * Vector3DotProduct(e2, q);
    return dist > 0.002f && dist < max_dist - 0.06f;
}

bool analytic_visible(Vector3 camera_pos, Vector3 point, const std::vector<TestTriangle>& triangles) {
    const Vector3 delta = point - camera_pos;
    const float dist = ol::safe_len(delta);
    if (dist <= 0.001f) return true;
    const Vector3 dir = delta / dist;
    for (const TestTriangle& tri : triangles) {
        if (ray_intersects_triangle(camera_pos, dir, tri, dist)) return false;
    }
    return true;
}

void collect_test_triangles(ol::Dimension* dim, const ol::CameraView& view, std::vector<TestTriangle>* triangles) {
    triangles->clear();
    for (ol::u32 slot = 0; slot < dim->meshes.count; ++slot) {
        const ol::MeshInstance* mesh = &dim->meshes.data[slot];
        const ol::MeshGeometry* geometry = ol::arena_get(&dim->geometries, mesh->geometry);
        if (!mesh->visible || !geometry || geometry->triangle_count == 0) continue;

        Vector3 transformed[ol::max_vertices_per_geometry]{};
        const Vector3 origin = ol::world_delta_meters(mesh->origin, view.anchor, dim->chunk_size_m);
        const Matrix basis = ol::matrix_no_translation(mesh->se3);
        for (ol::u32 i = 0; i < geometry->vertex_count; ++i) {
            transformed[i] = Vector3Transform(geometry->vertices[i], basis) + origin;
        }
        for (ol::u32 i = 0; i < geometry->triangle_count; ++i) {
            const ol::Triangle tri = geometry->triangles[i];
            triangles->push_back({transformed[tri.a], transformed[tri.b], transformed[tri.c]});
        }
    }
}

const ol::MeshInstance* find_mesh_by_name(const ol::Dimension* dim, const char* name) {
    for (ol::u32 slot = 0; slot < dim->meshes.count; ++slot) {
        const ol::MeshInstance* mesh = &dim->meshes.data[slot];
        if (std::strcmp(mesh->name, name) == 0) return mesh;
    }
    return nullptr;
}

bool run_edge_visibility_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(960, 540, "ol edge visibility");

    ol::RenderState renderer{};
    ol::renderer_init(&renderer);

    auto app = std::make_unique<ol::DemoApp>();
    ol::demo_generate_world(app.get());
    ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);
    const ol::MeshInstance* tunnel = dim ? find_mesh_by_name(dim, "tunnel contours") : nullptr;
    const ol::MeshGeometry* tunnel_geometry = (dim && tunnel) ? ol::arena_get(&dim->geometries, tunnel->geometry) : nullptr;

    const EdgeCompareView views[] = {
        {"tunnel-ramp", {15.0f, 1.8f, 2.2f}, {8.5f, 1.1f, 8.4f}},
        {"tunnel-ramp-close", {13.8f, 1.35f, 0.6f}, {10.3f, 0.75f, -3.4f}},
        {"stair-tunnel", {-8.0f, 2.8f, 11.0f}, {10.5f, 0.9f, -3.5f}},
        {"user-tunnel-stairs", {6.1099f, 2.7000f, 11.4536f}, {9.0207f, 1.9794f, 4.0368f}},
        {"user-tunnel-outside", {6.6376f, 1.6500f, -4.5389f}, {14.3344f, 0.0418f, -3.0650f}},
        {"user-tunnel-inside", {10.3368f, 0.8000f, -3.6188f}, {11.0135f, -0.4793f, 4.2492f}},
        {"tunnel-front", {10.5f, 1.25f, 4.5f}, {10.5f, 0.7f, -3.5f}},
        {"tunnel-back", {10.5f, 1.25f, -10.5f}, {10.5f, 0.7f, -3.5f}},
    };

    bool ok = expect_true(dim != nullptr, "Edge visibility has generated dimension") &&
              expect_true(tunnel != nullptr, "Edge visibility finds tunnel contours") &&
              expect_true(tunnel_geometry != nullptr, "Edge visibility finds tunnel geometry");

    ol::renderer_ensure_target(&renderer);
    for (const EdgeCompareView& view_def : views) {
        if (!dim || !tunnel || !tunnel_geometry) break;

        const ol::CameraView view = make_edge_compare_camera(app->dimension_id, dim, view_def);
        renderer.depth_test_edges = true;
        ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
        const TestDepthBuffer depth = read_test_depth_target(renderer);

        const Camera3D camera = make_test_camera(view, renderer.fov);
        const Matrix view_matrix = MatrixLookAt(camera.position, camera.target, camera.up);
        const Matrix projection_matrix = MatrixPerspective(
            camera.fovy * DEG2RAD,
            static_cast<double>(renderer.native_w) / static_cast<double>(renderer.native_h),
            rlGetCullDistanceNear(),
            rlGetCullDistanceFar()
        );
        std::vector<TestTriangle> triangles;
        collect_test_triangles(dim, view, &triangles);

        Vector3 transformed[ol::max_vertices_per_geometry]{};
        const Vector3 origin = ol::world_delta_meters(tunnel->origin, view.anchor, dim->chunk_size_m);
        const Matrix basis = ol::matrix_no_translation(tunnel->se3);
        for (ol::u32 i = 0; i < tunnel_geometry->vertex_count; ++i) {
            transformed[i] = Vector3Transform(tunnel_geometry->vertices[i], basis) + origin;
        }

        int samples = 0;
        int mismatches = 0;
        int false_visible = 0;
        int false_hidden = 0;
        int depth_only_mismatches = 0;
        int edge_false_visible[ol::max_edges_per_geometry]{};
        int edge_false_hidden[ol::max_edges_per_geometry]{};
        for (ol::u32 edge_i = 0; edge_i < tunnel_geometry->edge_count; ++edge_i) {
            const ol::Edge edge = tunnel_geometry->edges[edge_i];
            TestProjectedPoint a{};
            TestProjectedPoint b{};
            if (!project_test_point(transformed[edge.a], view_matrix, projection_matrix, renderer.native_w, renderer.native_h, &a)) continue;
            if (!project_test_point(transformed[edge.b], view_matrix, projection_matrix, renderer.native_w, renderer.native_h, &b)) continue;
            for (int i = 0; i <= 16; ++i) {
                const float t = static_cast<float>(i) / 16.0f;
                const Vector3 point = transformed[edge.a] + (transformed[edge.b] - transformed[edge.a]) * t;
                const bool expected = analytic_visible(camera.position, point, triangles);
                const bool depth_only = test_filter_visible(depth, a, b, t);
                const bool actual = analytic_visible(camera.position, point, triangles);
                if (expected != depth_only) ++depth_only_mismatches;
                if (expected != actual) {
                    ++mismatches;
                    if (actual) {
                        ++false_visible;
                        ++edge_false_visible[edge_i];
                    } else {
                        ++false_hidden;
                        ++edge_false_hidden[edge_i];
                    }
                }
                ++samples;
            }
        }

        const float mismatch_rate = samples > 0 ? static_cast<float>(mismatches) / static_cast<float>(samples) : 1.0f;
        std::printf("edge visibility %s: %d/%d mismatches (%.3f), false-visible %d, false-hidden %d, depth-only mismatches %d\n", view_def.name, mismatches, samples, mismatch_rate, false_visible, false_hidden, depth_only_mismatches);
        for (ol::u32 edge_i = 0; edge_i < tunnel_geometry->edge_count; ++edge_i) {
            if (edge_false_visible[edge_i] == 0 && edge_false_hidden[edge_i] == 0) continue;
            const ol::Edge edge = tunnel_geometry->edges[edge_i];
            std::printf("  edge %u (%u-%u): false-visible %d, false-hidden %d\n", edge_i, edge.a, edge.b, edge_false_visible[edge_i], edge_false_hidden[edge_i]);
        }
        ok = expect_true(samples > 0, "Edge visibility sampled tunnel edges") && ok;
    }

    ol::renderer_shutdown(&renderer);
    CloseWindow();
    return ok;
}

bool run_menu_visual_test() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(900, 560, "ol menu visual test");

    ol::RenderState renderer{};
    ol::renderer_init(&renderer);
    RenderTexture2D target = LoadRenderTexture(900, 560);
    SetTextureFilter(target.texture, TEXTURE_FILTER_POINT);
    BeginTextureMode(target);
    ol::demo_draw_menu_contents(ol::MenuScreen{
        &renderer,
        "player",
        "playground",
        "hills",
        "Offline build; Steamworks disabled",
        {90, 180, 255, 255}
    });
    EndTextureMode();

    Image img = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&img);
    std::filesystem::create_directories("test-output");
    const char* out_path = "test-output/menu-smoke.png";
    const bool exported = ExportImage(img, out_path);
    Color* pixels = LoadImageColors(img);
    int bright = 0;
    int color_swatch = 0;
    for (int i = 0; i < img.width * img.height; ++i) {
        const Color c = pixels[i];
        if (c.r > 210 && c.g > 210 && c.b > 210) bright++;
        if (c.b > 200 && c.g > 120 && c.r < 130) color_swatch++;
    }
    UnloadImageColors(pixels);
    UnloadImage(img);

    ol::RadianceCascadeSettings pause_lighting{};
    pause_lighting.probe_extra_levels = 2;
    pause_lighting.cascade_iterations = 3;
    pause_lighting.indirect_samples = 64;
    pause_lighting.shadow_samples = 16;
    pause_lighting.temporal_frames = 256;
    pause_lighting.lighting_radius_chunks = ol::pause_lighting_radius_max;
    BeginTextureMode(target);
    ol::demo_draw_pause_contents(ol::PauseScreen{
        &renderer,
        90,
        0,
        ol::pause_render_radius_max,
        true,
        true,
        pause_lighting
    });
    EndTextureMode();

    Image pause_img = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&pause_img);
    const char* pause_out_path = "test-output/pause-smoke.png";
    const bool pause_exported = ExportImage(pause_img, pause_out_path);
    Color* pause_pixels = LoadImageColors(pause_img);
    int pause_bright = 0;
    int pause_enabled_accent = 0;
    for (int i = 0; i < pause_img.width * pause_img.height; ++i) {
        const Color c = pause_pixels[i];
        if (c.r > 210 && c.g > 210 && c.b > 210) pause_bright++;
        if (c.g > 190 && c.r < 150 && c.b < 180) pause_enabled_accent++;
    }
    UnloadImageColors(pause_pixels);
    UnloadImage(pause_img);

    const float pause_scale = fminf(900.0f / 1100.0f, 560.0f / 730.0f);
    const Vector2 pause_origin = {
        (900.0f - 1100.0f * pause_scale) * 0.5f,
        (560.0f - 730.0f * pause_scale) * 0.5f,
    };
    auto pause_point = [&](float x, float y) {
        return Vector2{pause_origin.x + x * pause_scale, pause_origin.y + y * pause_scale};
    };
    const bool probe_hit = ol::demo_pause_hit_test(pause_point(830.0f, 90.0f)).control == ol::pause_control_lighting_probe_levels;
    const bool enabled_hit = ol::demo_pause_hit_test(pause_point(692.0f, 561.0f)).control == ol::pause_control_lighting_enabled;
    const bool corner_hit = ol::demo_pause_hit_test(pause_point(967.0f, 561.0f)).control == ol::pause_control_lighting_corner_merge;
    const bool indirect_choice = ol::demo_pause_value_from_mouse(
        ol::pause_control_lighting_indirect_samples, pause_point(1030.0f, 250.0f)) == 64;
    const bool shadow_choice = ol::demo_pause_value_from_mouse(
        ol::pause_control_lighting_shadow_samples, pause_point(630.0f, 330.0f)) == 1;
    const bool temporal_choice = ol::demo_pause_value_from_mouse(
        ol::pause_control_lighting_temporal_frames, pause_point(830.0f, 410.0f)) == 16;
    const bool lighting_radius_choice = ol::demo_pause_value_from_mouse(
        ol::pause_control_lighting_radius, pause_point(1030.0f, 490.0f)) == ol::pause_lighting_radius_max;

    UnloadRenderTexture(target);
    ol::renderer_shutdown(&renderer);
    CloseWindow();

    const bool ok = expect_true(exported, "Menu smoke image exported") &&
                    expect_true(bright > 500, "Menu smoke image contains large readable text") &&
                    expect_true(color_swatch > 300, "Menu smoke image contains player color swatch") &&
                    expect_true(pause_exported, "Pause smoke image exported") &&
                    expect_true(pause_bright > 500, "Pause smoke image contains readable controls") &&
                    expect_true(pause_enabled_accent > 300, "Pause smoke image contains enabled toggles") &&
                    expect_true(probe_hit && enabled_hit && corner_hit, "Pause lighting controls have matching hit regions") &&
                    expect_true(indirect_choice && shadow_choice && temporal_choice && lighting_radius_choice,
                        "Pause lighting sliders return discrete Trivox quality choices");
    if (ok) std::printf("menu visual test passed: %s %s\n", out_path, pause_out_path);
    return ok;
}

bool test_hills_house_block_contact_keeps_floor_support() {
    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "hills");
    std::snprintf(app->session_name, sizeof(app->session_name), "hills-house-floor-contact");
    app->profile.session_count = 1;
    ol::SavedSessionState& session = app->profile.sessions[0];
    session.valid = true;
    std::snprintf(session.name, sizeof(session.name), "%s", app->session_name);
    std::snprintf(session.world_name, sizeof(session.world_name), "%s", app->world_name);
    session.player_count = 1;
    session.players[0].valid = true;
    session.players[0].peer_id = app->net.local_peer_id;
    const ol::WorldPos supplied_feet = test_pos(0, {7.3614f, 3.2521f, 74.2812f}, 16.0f);
    session.players[0].chunk = supplied_feet.chunk;
    session.players[0].local = supplied_feet.local;
    session.players[0].yaw = 6.192971f;

    ol::demo_generate_world(app.get());
    ol::Dimension* dim = ol::world_get_dimension(&app->world, app->dimension_id);
    ol::PlayerEntity* player = dim ? ol::arena_get(&dim->players, app->local_player_id) : nullptr;
    bool ok = expect_true(dim && player, "Supplied hills house pose generates its player and colliders");
    if (!dim || !player) return false;

    ol::PlayerControllerInput input{};
    input.move = {0.0f, 1.0f};
    float min_y = feet_y(dim, player);
    for (int i = 0; i < 50; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        min_y = fminf(min_y, feet_y(dim, player));
    }
    ok = expect_true(min_y > 3.15f, "Touching a block in the supplied hills house does not drop through its floor") && ok;
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    bool run_physics = argc == 1;
    bool run_visual = argc == 1;
    bool run_menu = argc == 1;
    bool run_edge_compare = false;
    bool run_edge_visibility = false;
    bool run_hills_benchmark = false;
    bool run_lighting_roundtrip = false;
    bool run_lighting_route_regression = false;
    bool run_lighting_user_route_regression = false;
    bool run_lighting_minus16_regression = false;
    bool run_lighting_render_state_diagnostic = false;
    bool run_lighting_cave_pathtrace = false;
    bool run_lighting_cave_seam = false;
    bool run_cave_render_benchmark_flag = false;
    bool run_cave_render_benchmark_hd_flag = false;
    bool run_cave_stationary_regression = false;
    bool run_cave_surface_distance_regression = false;
    bool run_cave_motion_benchmark_flag = false;
    bool run_cave_motion_benchmark_hd_flag = false;
    bool run_lighting_continuous_motion = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--physics") == 0) run_physics = true;
        if (std::strcmp(argv[i], "--visual") == 0) run_visual = true;
        if (std::strcmp(argv[i], "--menu") == 0) run_menu = true;
        if (std::strcmp(argv[i], "--edge-compare") == 0) run_edge_compare = true;
        if (std::strcmp(argv[i], "--edge-visibility") == 0) run_edge_visibility = true;
        if (std::strcmp(argv[i], "--hills-render-benchmark") == 0) run_hills_benchmark = true;
        if (std::strcmp(argv[i], "--lighting-roundtrip") == 0) run_lighting_roundtrip = true;
        if (std::strcmp(argv[i], "--lighting-route-regression") == 0) run_lighting_route_regression = true;
        if (std::strcmp(argv[i], "--lighting-user-route-regression") == 0) run_lighting_user_route_regression = true;
        if (std::strcmp(argv[i], "--lighting-route-minus16") == 0) run_lighting_minus16_regression = true;
        if (std::strcmp(argv[i], "--lighting-route-render-state") == 0) run_lighting_render_state_diagnostic = true;
        if (std::strcmp(argv[i], "--lighting-cave-pathtrace") == 0) run_lighting_cave_pathtrace = true;
        if (std::strcmp(argv[i], "--lighting-cave-seam") == 0) run_lighting_cave_seam = true;
        if (std::strcmp(argv[i], "--cave-render-benchmark") == 0) run_cave_render_benchmark_flag = true;
        if (std::strcmp(argv[i], "--cave-render-benchmark-hd") == 0) run_cave_render_benchmark_hd_flag = true;
        if (std::strcmp(argv[i], "--cave-stationary-regression") == 0) run_cave_stationary_regression = true;
        if (std::strcmp(argv[i], "--cave-surface-distance") == 0) run_cave_surface_distance_regression = true;
        if (std::strcmp(argv[i], "--cave-motion-benchmark") == 0) run_cave_motion_benchmark_flag = true;
        if (std::strcmp(argv[i], "--cave-motion-benchmark-hd") == 0) run_cave_motion_benchmark_hd_flag = true;
        if (std::strcmp(argv[i], "--lighting-continuous-motion") == 0) run_lighting_continuous_motion = true;
    }

    bool ok = true;
    if (run_physics) ok = run_physics_tests() && ok;
    if (run_visual) ok = run_visual_test() && ok;
    if (run_edge_compare) ok = run_edge_compare_test() && ok;
    if (run_edge_visibility) ok = run_edge_visibility_test() && ok;
    if (run_hills_benchmark) ok = run_hills_render_benchmark() && ok;
    if (run_lighting_roundtrip) ok = run_lighting_roundtrip_test() && ok;
    if (run_lighting_route_regression) ok = run_lighting_route_regression_test() && ok;
    if (run_lighting_user_route_regression) ok = run_lighting_user_route_regression_test() && ok;
    if (run_lighting_minus16_regression) ok = run_lighting_minus16_regression_test() && ok;
    if (run_lighting_render_state_diagnostic) ok = run_lighting_render_state_diagnostic_test() && ok;
    if (run_lighting_cave_pathtrace) ok = run_cave_pathtrace_reference_test() && ok;
    if (run_lighting_cave_seam) ok = run_cave_anchor_seam_regression_test() && ok;
    if (run_cave_render_benchmark_flag) {
        ok = run_cave_render_benchmark(960, 540, "standard") && ok;
    }
    if (run_cave_render_benchmark_hd_flag) {
        ok = run_cave_render_benchmark(1920, 1080, "hd") && ok;
    }
    if (run_cave_stationary_regression) ok = run_cave_stationary_regression_test() && ok;
    if (run_cave_surface_distance_regression) ok = run_cave_surface_distance_regression_test() && ok;
    if (run_cave_motion_benchmark_flag) {
        ok = run_cave_motion_benchmark(960, 540, "standard") && ok;
    }
    if (run_cave_motion_benchmark_hd_flag) {
        ok = run_cave_motion_benchmark(1920, 1080, "hd") && ok;
    }
    if (run_lighting_continuous_motion) ok = run_lighting_continuous_motion_test() && ok;
    if (run_menu) ok = run_menu_visual_test() && ok;
    return ok ? 0 : 1;
}
