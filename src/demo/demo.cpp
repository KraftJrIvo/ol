#include "demo/demo.h"
#include "demo/menu.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ol {

static void add_text_char(char* text, size_t cap, int c) {
    if (c < 32 || c > 126) return;
    const size_t len = std::strlen(text);
    if (len + 1 >= cap) return;
    text[len] = static_cast<char>(c);
    text[len + 1] = 0;
}

static WorldPos demo_pos(u32 dimension, Vector3 meters, float chunk_size) {
    return make_world_pos(dimension, meters, chunk_size);
}

static u32 add_visual_box(Dimension* dim, u32 dimension_id, u32 cube_geom, const char* name, Vector3 center, Vector3 size, Color color) {
    MeshInstance mesh{};
    std::snprintf(mesh.name, sizeof(mesh.name), "%s", name);
    mesh.geometry = cube_geom;
    mesh.origin = demo_pos(dimension_id, center, dim->chunk_size_m);
    mesh.se3 = MatrixScale(size.x, size.y, size.z);
    mesh.color = color;
    mesh.bounds_radius = Vector3Length(size) * 0.5f;
    return dimension_add_mesh(dim, mesh);
}

static void add_static_box(Dimension* dim, u32 dimension_id, u32 cube_geom, const char* name, Vector3 center, Vector3 size, Color color, bool lit = true, bool draw_edges = true) {
    const u32 mesh_id = add_visual_box(dim, dimension_id, cube_geom, name, center, size, color);
    MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
    if (mesh) {
        mesh->lit = lit;
        mesh->draw_edges = draw_edges;
    }

    BoxCollider box{};
    box.pos = demo_pos(dimension_id, center, dim->chunk_size_m);
    box.half = size * 0.5f;
    box.color = color;
    physics_add_box(&dim->physics, box);
}

static void generate_demo_world(DemoApp* app) {
    world_init(&app->world);
    app->dimension_id = world_add_dimension(&app->world, "default", 16.0f, 64.0f);
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    dim->render_radius_chunks = 7;
    dim->quality_render_radius_chunks = 4;
    dim->ambient = 0.38f;
    dim->sky_top = {95, 142, 205, 255};
    dim->sky_bottom = {210, 229, 245, 255};
    dim->fog_color = {185, 204, 219, 255};

    MeshGeometry cube = make_box_geometry("unit cube", {1.0f, 1.0f, 1.0f});
    MeshGeometry cube_lod = make_box_geometry("unit cube lod", {0.96f, 0.96f, 0.96f});
    const u32 cube_lod_id = dimension_add_geometry(dim, cube_lod);
    cube.lod_geometry = cube_lod_id;
    const u32 cube_id = dimension_add_geometry(dim, cube);

    MeshGeometry wedge = make_wedge_geometry("wedge", {1.0f, 1.0f, 1.0f});
    const u32 wedge_id = dimension_add_geometry(dim, wedge);

    add_static_box(dim, app->dimension_id, cube_id, "floor", {0.0f, -0.55f, 0.0f}, {40.0f, 1.0f, 40.0f}, {118, 134, 123, 255}, false, false);
    add_static_box(dim, app->dimension_id, cube_id, "north wall", {0.0f, 1.5f, -18.0f}, {40.0f, 4.0f, 1.0f}, {96, 105, 124, 255}, false, true);
    add_static_box(dim, app->dimension_id, cube_id, "west wall", {-18.0f, 1.5f, 0.0f}, {1.0f, 4.0f, 40.0f}, {93, 113, 123, 255}, false, true);
    add_static_box(dim, app->dimension_id, cube_id, "block a", {4.0f, 0.5f, -4.0f}, {2.5f, 2.0f, 2.5f}, {174, 96, 88, 255});
    add_static_box(dim, app->dimension_id, cube_id, "block b", {-4.0f, 0.25f, -2.0f}, {2.0f, 1.5f, 5.0f}, {96, 145, 112, 255});
    add_static_box(dim, app->dimension_id, cube_id, "tunnel left", {9.0f, 0.5f, -3.5f}, {0.45f, 1.0f, 5.0f}, {108, 120, 145, 255}, false, true);
    add_static_box(dim, app->dimension_id, cube_id, "tunnel right", {12.0f, 0.5f, -3.5f}, {0.45f, 1.0f, 5.0f}, {108, 120, 145, 255}, false, true);
    add_static_box(dim, app->dimension_id, cube_id, "tunnel roof", {10.5f, 1.15f, -3.5f}, {3.45f, 0.30f, 5.0f}, {93, 102, 130, 255}, false, true);
    add_static_box(dim, app->dimension_id, cube_id, "step 1", {-8.0f, 0.0f, 5.0f}, {3.0f, 1.0f, 3.0f}, {166, 151, 92, 255});
    add_static_box(dim, app->dimension_id, cube_id, "step 2", {-8.0f, 0.5f, 8.0f}, {3.0f, 2.0f, 3.0f}, {174, 160, 101, 255});
    add_static_box(dim, app->dimension_id, cube_id, "step 3", {-8.0f, 1.0f, 11.0f}, {3.0f, 3.0f, 3.0f}, {185, 171, 112, 255});

    MeshInstance ramp{};
    std::snprintf(ramp.name, sizeof(ramp.name), "visual ramp");
    ramp.geometry = wedge_id;
    ramp.origin = demo_pos(app->dimension_id, {7.0f, 0.5f, 6.0f}, dim->chunk_size_m);
    ramp.se3 = MatrixScale(5.0f, 1.0f, 5.0f);
    ramp.color = {117, 136, 182, 255};
    dimension_add_mesh(dim, ramp);

    BoxCollider ramp_collider{};
    ramp_collider.pos = demo_pos(app->dimension_id, {7.0f, 0.363f, 6.024f}, dim->chunk_size_m);
    ramp_collider.half = {2.5f, 0.12f, 2.55f};
    ramp_collider.axis_aligned = false;
    ramp_collider.rotation = QuaternionFromAxisAngle({1.0f, 0.0f, 0.0f}, -std::atan2(1.05f, 5.0f));
    ramp_collider.color = ramp.color;
    physics_add_box(&dim->physics, ramp_collider);

    add_static_box(dim, app->dimension_id, cube_id, "ramp landing", {7.0f, 0.5f, 10.6f}, {5.0f, 1.0f, 4.0f}, {107, 126, 168, 255}, false, true);

    SpriteInstance sprite{};
    std::snprintf(sprite.name, sizeof(sprite.name), "full billboard");
    sprite.origin = demo_pos(app->dimension_id, {-2.0f, 2.0f, -7.0f}, dim->chunk_size_m);
    sprite.size = {1.2f, 1.2f};
    sprite.color = {255, 231, 119, 255};
    sprite.billboard = billboard_full_3d;
    dimension_add_sprite(dim, sprite);

    SpriteInstance vertical = sprite;
    std::snprintf(vertical.name, sizeof(vertical.name), "vertical billboard");
    vertical.origin = demo_pos(app->dimension_id, {1.5f, 1.6f, -7.0f}, dim->chunk_size_m);
    vertical.color = {120, 214, 186, 255};
    vertical.billboard = billboard_vertical;
    dimension_add_sprite(dim, vertical);

    LightSource light{};
    std::snprintf(light.name, sizeof(light.name), "warm light");
    light.pos = demo_pos(app->dimension_id, {0.0f, 5.5f, -5.0f}, dim->chunk_size_m);
    light.color = {255, 230, 180, 255};
    light.radius = 14.0f;
    light.intensity = 1.2f;
    dimension_add_light(dim, light);

    LightSource blue = light;
    std::snprintf(blue.name, sizeof(blue.name), "blue light");
    blue.pos = demo_pos(app->dimension_id, {-8.0f, 4.0f, 8.0f}, dim->chunk_size_m);
    blue.color = {120, 160, 255, 255};
    blue.radius = 10.0f;
    blue.intensity = 0.9f;
    dimension_add_light(dim, blue);

    app->local_player_id = dimension_add_player(
        dim,
        app->player_name,
        app->player_color,
        demo_pos(app->dimension_id, {0.0f, 0.0f, 8.0f}, dim->chunk_size_m),
        true
    );
}

void demo_init(DemoApp* app) {
    app->dimension_id = invalid_id;
    app->local_player_id = invalid_id;
    app->fixed_accum = 0.0f;
    app->in_game = false;
    app->mouse_captured = false;
    std::snprintf(app->player_name, sizeof(app->player_name), "player");
    std::snprintf(app->session_name, sizeof(app->session_name), "playground");
    app->player_color = {90, 180, 255, 255};
    InitWindow(1280, 720, "OL");
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(120);
    renderer_init(&app->renderer);
    net_init(&app->net);
    generate_demo_world(app);
}

void demo_shutdown(DemoApp* app) {
    net_shutdown(&app->net);
    renderer_shutdown(&app->renderer);
    CloseWindow();
}

void demo_draw_menu(DemoApp* app) {
    demo_draw_menu_screen(MenuScreen{
        &app->renderer,
        app->player_name,
        app->session_name,
        app->net.status,
        app->player_color
    });
}

static void update_menu(DemoApp* app) {
    int c = GetCharPressed();
    while (c) {
        add_text_char(app->player_name, sizeof(app->player_name), c);
        c = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE)) {
        const size_t len = std::strlen(app->player_name);
        if (len > 0) app->player_name[len - 1] = 0;
    }
    if (IsKeyPressed(KEY_C)) {
        static const Color colors[] = {
            {90, 180, 255, 255}, {255, 120, 110, 255}, {120, 220, 150, 255},
            {255, 214, 100, 255}, {205, 145, 255, 255}
        };
        static u32 color_idx = 0;
        color_idx = (color_idx + 1) % (sizeof(colors) / sizeof(colors[0]));
        app->player_color = colors[color_idx];
    }
    if (IsKeyPressed(KEY_H)) {
        net_host(&app->net, app->session_name);
        generate_demo_world(app);
        app->in_game = true;
        app->mouse_captured = true;
        DisableCursor();
    }
    if (IsKeyPressed(KEY_J)) {
        net_join_from_clipboard(&app->net);
        generate_demo_world(app);
        app->in_game = true;
        app->mouse_captured = true;
        DisableCursor();
    }
}

static PlayerEntity* local_player(DemoApp* app, Dimension* dim) {
    return arena_get(&dim->players, app->local_player_id);
}

static void collect_input(DemoApp* app, PlayerEntity* player) {
    if (IsKeyPressed(KEY_ESCAPE)) {
        app->mouse_captured = !app->mouse_captured;
        if (app->mouse_captured) DisableCursor();
        else EnableCursor();
    }
    if (!app->mouse_captured && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        app->mouse_captured = true;
        DisableCursor();
    }

    if (app->mouse_captured) {
        const Vector2 mouse = GetMouseDelta();
        const float sensitivity = 0.0022f;
        player->yaw += mouse.x * sensitivity;
        player->pitch -= mouse.y * sensitivity;
        player->pitch = clampf(player->pitch, -1.45f, 1.45f);
    }

    Vector2 move{};
    if (IsKeyDown(KEY_W)) move.y += 1.0f;
    if (IsKeyDown(KEY_S)) move.y -= 1.0f;
    if (IsKeyDown(KEY_D)) move.x += 1.0f;
    if (IsKeyDown(KEY_A)) move.x -= 1.0f;
    if (Vector2Length(move) > 1.0f) move = Vector2Normalize(move);
    app->input.move = move;
    app->input.jump_pressed = app->input.jump_pressed || IsKeyPressed(KEY_SPACE);
    app->input.jump_held = IsKeyDown(KEY_SPACE);
    app->input.sprint = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    app->input.crouch = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

static void fixed_update(DemoApp* app, Dimension* dim, float dt) {
    PlayerEntity* player = local_player(app, dim);
    if (!player) return;
    PlayerControllerInput input{};
    input.move = app->input.move;
    input.jump_pressed = app->input.jump_pressed;
    input.jump_held = app->input.jump_held;
    input.sprint = app->input.sprint;
    input.crouch = app->input.crouch;
    player_controller_step(dim, player, input, dt);
    physics_step(dim, dt);
    app->input.jump_pressed = false;
}

static void update_game(DemoApp* app) {
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    PlayerEntity* player = local_player(app, dim);
    if (!dim || !player) return;

    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) renderer_change_scale(&app->renderer, 1);
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) renderer_change_scale(&app->renderer, -1);
    if (IsKeyPressed(KEY_F3)) app->renderer.draw_physics_debug = !app->renderer.draw_physics_debug;

    collect_input(app, player);
    net_update(&app->net);

    app->fixed_accum += fminf(GetFrameTime(), 0.1f);
    constexpr float fixed_dt = 1.0f / 60.0f;
    while (app->fixed_accum >= fixed_dt) {
        fixed_update(app, dim, fixed_dt);
        app->fixed_accum -= fixed_dt;
    }

    CameraView view{};
    view.anchor = player_feet_pos(dim, player);
    view.eye_height = player->eye_height + player->camera_y_offset;
    view.yaw = player->yaw;
    view.pitch = player->pitch;
    renderer_ensure_target(&app->renderer);
    renderer_draw_dimension(&app->renderer, dim, view, app->local_player_id);
}

bool demo_update_and_draw(DemoApp* app) {
    if (WindowShouldClose()) return false;
    if (!app->in_game) {
        update_menu(app);
        demo_draw_menu(app);
    } else {
        update_game(app);
    }
    return true;
}

} // namespace ol
