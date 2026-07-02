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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
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
    input.jump_pressed = false;
    float max_air_speed = 0.0f;
    for (int i = 0; i < 30; ++i) {
        ol::player_controller_step(dim, player, input, 1.0f / 60.0f);
        if (!player->on_ground && i > 2) {
            const float speed = player_horizontal_speed(player);
            max_air_speed = fmaxf(max_air_speed, speed);
        }
    }

    bool ok = expect_true(max_air_speed > ol::player_sprint_speed_mps - 0.35f, "Airborne sprint movement is not capped down to walking speed");
    ok = expect_true(max_air_speed <= ol::player_sprint_speed_mps + 0.05f, "Airborne sprint movement remains capped at sprint speed") && ok;
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

bool run_physics_tests() {
    bool ok = true;
    auto run = [&](const char* name, bool (*fn)()) {
        std::printf("running %s\n", name);
        std::fflush(stdout);
        ok = fn() && ok;
    };
    run("worldpos", test_worldpos);
    run("axis_lock_uncrouch_anchor", test_axis_lock_uncrouch_anchor);
    run("floor_collision_settles", test_floor_collision_settles);
    run("box_face_normals", test_box_face_normals);
    run("player_dimensions", test_player_dimensions);
    run("player_crouch_tunnel_clearance", test_player_crouch_tunnel_clearance);
    run("player_wall_contact_is_stable", test_player_wall_contact_is_stable);
    run("player_airborne_sprint_speed_is_preserved", test_player_airborne_sprint_speed_is_preserved);
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
    run("player_auto_stands_after_tunnel_exit", test_player_auto_stands_after_tunnel_exit);
    run("player_jump_does_not_pass_through_tunnel_roof", test_player_jump_does_not_pass_through_tunnel_roof);
    run("player_tunnel_exit_uncrouch_does_not_pass_through_roof", test_player_tunnel_exit_uncrouch_does_not_pass_through_roof);
    run("player_crouched_tunnel_side_jump_matches_stand_clearance", test_player_crouched_tunnel_side_jump_matches_stand_clearance);
    run("player_tunnel_entrance_allows_side_slide", test_player_tunnel_entrance_allows_side_slide);
    run("player_yaw_slide_tunnel_entrance_to_flush_support", test_player_yaw_slide_tunnel_entrance_to_flush_support);
    run("player_uncrouches_on_tunnel_roof", test_player_uncrouches_on_tunnel_roof);
    run("player_rotated_box_ramp_transition", test_player_rotated_box_ramp_transition);
    run("player_sprint_jump_from_demo_stairs_to_ramp_does_not_fall_through", test_player_sprint_jump_from_demo_stairs_to_ramp_does_not_fall_through);
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
    mesh.lit = lit;
    mesh.draw_edges = draw_edges;
    return ol::dimension_add_mesh(dim, mesh);
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

    ol::LightSource light{};
    light.pos = test_pos(dim_id, {2.0f, 4.0f, 3.0f}, dim->chunk_size_m);
    light.radius = 10.0f;
    light.intensity = 1.1f;
    ol::dimension_add_light(dim, light);
    ol::dimension_add_player(dim, "remote", {240, 70, 90, 255}, test_pos(dim_id, {1.6f, 0.0f, 2.8f}, dim->chunk_size_m), false);

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
    ol::renderer_shutdown(&renderer);
    CloseWindow();

    const bool ok = expect_true(exported, "Visual smoke image exported") &&
                    expect_true(non_sky > 2000, "Visual smoke image contains scene pixels") &&
                    expect_true(dark > 20, "Visual smoke image contains solid dark edge pixels") &&
                    expect_true(box_edge_pixels > 20, "Visual smoke image contains box contour edge pixels") &&
                    expect_true(player_pixels > 50, "Visual smoke image contains a rendered remote player");
    if (ok) std::printf("visual test passed: %s\n", out_path);
    return ok;
}

struct EdgeCompareView {
    const char* name = "";
    Vector3 eye = {0.0f, 0.0f, 0.0f};
    Vector3 target = {0.0f, 0.0f, 0.0f};
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

bool export_render_target(ol::RenderState* renderer, const char* out_path) {
    Image img = LoadImageFromTexture(renderer->target.texture);
    ImageFlipVertical(&img);
    const bool exported = ExportImage(img, out_path);
    UnloadImage(img);
    return exported;
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
        {"wall-blocks", {-12.5f, 1.7f, 1.0f}, {2.0f, 1.0f, -5.0f}},
    };

    bool ok = expect_true(dim != nullptr, "Edge comparison has a generated dimension");
    ol::renderer_ensure_target(&renderer);
    for (const EdgeCompareView& view_def : views) {
        if (!dim) break;
        const ol::CameraView view = make_edge_compare_camera(app->dimension_id, dim, view_def);

        char old_path[128]{};
        char new_path[128]{};
        char ref_path[128]{};
        std::snprintf(old_path, sizeof(old_path), "test-output/edge-compare-%s-2d.png", view_def.name);
        std::snprintf(new_path, sizeof(new_path), "test-output/edge-compare-%s-depth.png", view_def.name);
        std::snprintf(ref_path, sizeof(ref_path), "test-output/edge-compare-%s-reference.png", view_def.name);

        renderer.depth_test_edges = false;
        renderer.brute_force_edge_occlusion = false;
        ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
        ok = expect_true(export_render_target(&renderer, old_path), "2D edge comparison image exported") && ok;

        renderer.depth_test_edges = true;
        renderer.brute_force_edge_occlusion = false;
        ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
        ok = expect_true(export_render_target(&renderer, new_path), "Depth edge comparison image exported") && ok;

        renderer.depth_test_edges = true;
        renderer.brute_force_edge_occlusion = true;
        ol::renderer_draw_dimension(&renderer, dim, view, app->local_player_id);
        ok = expect_true(export_render_target(&renderer, ref_path), "Reference edge comparison image exported") && ok;
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

    auto app = std::make_unique<ol::DemoApp>();
    std::snprintf(app->world_name, sizeof(app->world_name), "hills");
    std::snprintf(app->session_name, sizeof(app->session_name), "render-benchmark");

    const Vector3 feet_m = {-2.4580f, 2.3080f, 71.6100f};
    const ol::WorldPos saved_feet = test_pos(0, feet_m, 16.0f);
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
    const EdgeCompareView view_def = {
        "hills-lag",
        {-2.4580f, 4.0080f, 71.6100f},
        {-1.0408f, 4.3231f, 79.4772f}
    };
    const ol::CameraView view = dim ? make_edge_compare_camera(app->dimension_id, dim, view_def) : ol::CameraView{};

    bool ok = expect_true(dim != nullptr, "Hills benchmark generated a dimension");
    if (dim) {
        ol::renderer_ensure_target(&renderer);
        constexpr int warmup_frames = 3;
        constexpr int measured_frames = 30;
        for (int mode = 0; mode < 2; ++mode) {
            renderer.depth_test_edges = mode != 0;
            for (int i = 0; i < warmup_frames; ++i) {
                ol::renderer_render_dimension_to_target(&renderer, dim, view, app->local_player_id);
            }

            const double start = GetTime();
            for (int i = 0; i < measured_frames; ++i) {
                ol::renderer_render_dimension_to_target(&renderer, dim, view, app->local_player_id);
            }
            const double elapsed_ms = (GetTime() - start) * 1000.0;
            std::printf(
                "hills render benchmark (%s edges): frames=%d avg_ms=%.3f edges=%u tris=%u unbounded=%u samples=%llu candidates=%llu ray_tests=%llu\n",
                renderer.depth_test_edges ? "depth" : "flat",
                measured_frames,
                elapsed_ms / static_cast<double>(measured_frames),
                renderer.debug_edge_count,
                renderer.debug_scene_triangle_count,
                renderer.debug_unbounded_scene_triangle_count,
                static_cast<unsigned long long>(renderer.debug_edge_sample_count),
                static_cast<unsigned long long>(renderer.debug_edge_candidate_count),
                static_cast<unsigned long long>(renderer.debug_edge_ray_test_count));
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
    UnloadRenderTexture(target);
    ol::renderer_shutdown(&renderer);
    CloseWindow();

    const bool ok = expect_true(exported, "Menu smoke image exported") &&
                    expect_true(bright > 500, "Menu smoke image contains large readable text") &&
                    expect_true(color_swatch > 300, "Menu smoke image contains player color swatch");
    if (ok) std::printf("menu visual test passed: %s\n", out_path);
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

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--physics") == 0) run_physics = true;
        if (std::strcmp(argv[i], "--visual") == 0) run_visual = true;
        if (std::strcmp(argv[i], "--menu") == 0) run_menu = true;
        if (std::strcmp(argv[i], "--edge-compare") == 0) run_edge_compare = true;
        if (std::strcmp(argv[i], "--edge-visibility") == 0) run_edge_visibility = true;
        if (std::strcmp(argv[i], "--hills-render-benchmark") == 0) run_hills_benchmark = true;
    }

    bool ok = true;
    if (run_physics) ok = run_physics_tests() && ok;
    if (run_visual) ok = run_visual_test() && ok;
    if (run_edge_compare) ok = run_edge_compare_test() && ok;
    if (run_edge_visibility) ok = run_edge_visibility_test() && ok;
    if (run_hills_benchmark) ok = run_hills_render_benchmark() && ok;
    if (run_menu) ok = run_menu_visual_test() && ok;
    return ok ? 0 : 1;
}
