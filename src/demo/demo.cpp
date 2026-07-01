#include "demo/demo.h"
#include "demo/menu.h"

#include <cmath>
#include <cstdarg>
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

static void reset_remote_players(DemoApp* app) {
    app->remote_peer_ids.fill(0);
    app->remote_player_ids.fill(invalid_id);
}

static void write_smoke_log(const char* fmt, ...) {
    char message[256]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    std::printf("%s\n", message);
    std::fflush(stdout);

    if (std::FILE* file = std::fopen("steam-smoke.log", "w")) {
        std::fprintf(file, "%s\n", message);
        std::fclose(file);
    }
}

static void write_smoke_lobby_file(u64 lobby_id) {
    if (std::FILE* file = std::fopen("steam-smoke-lobby.txt", "w")) {
        std::fprintf(file, "%llu\n", static_cast<unsigned long long>(lobby_id));
        std::fclose(file);
    }
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

static u16 add_contour_vertex(MeshGeometry* geometry, Vector3 vertex) {
    if (geometry->vertex_count >= max_vertices_per_geometry) return 0;
    const u16 index = static_cast<u16>(geometry->vertex_count++);
    geometry->vertices[index] = vertex;
    return index;
}

static void add_contour_edge(MeshGeometry* geometry, u16 a, u16 b, u8 thickness_px = 2) {
    if (geometry->edge_count >= max_edges_per_geometry) return;
    geometry->edges[geometry->edge_count++] = {a, b, thickness_px};
}

static MeshGeometry make_stair_contour_geometry(const char* name) {
    MeshGeometry geometry{};
    std::snprintf(geometry.name, sizeof(geometry.name), "%s", name);

    constexpr float xh = 1.5f;
    constexpr float z0 = -4.5f;
    constexpr float z1 = -1.5f;
    constexpr float z2 = 1.5f;
    constexpr float z3 = 4.5f;
    constexpr float y0 = -0.5f;
    constexpr float y1 = 0.5f;
    constexpr float y2 = 1.5f;
    constexpr float y3 = 2.5f;

    const Vector3 left[] = {
        {-xh, y0, z0}, {-xh, y0, z3}, {-xh, y3, z3}, {-xh, y3, z2},
        {-xh, y2, z2}, {-xh, y2, z1}, {-xh, y1, z1}, {-xh, y1, z0},
    };
    const Vector3 right[] = {
        { xh, y0, z0}, { xh, y0, z3}, { xh, y3, z3}, { xh, y3, z2},
        { xh, y2, z2}, { xh, y2, z1}, { xh, y1, z1}, { xh, y1, z0},
    };

    u16 left_indices[8]{};
    u16 right_indices[8]{};
    for (u32 i = 0; i < 8; ++i) {
        left_indices[i] = add_contour_vertex(&geometry, left[i]);
        right_indices[i] = add_contour_vertex(&geometry, right[i]);
    }
    for (u32 i = 0; i < 8; ++i) {
        const u32 next = (i + 1) % 8;
        add_contour_edge(&geometry, left_indices[i], left_indices[next]);
        add_contour_edge(&geometry, right_indices[i], right_indices[next]);
        add_contour_edge(&geometry, left_indices[i], right_indices[i]);
    }
    return geometry;
}

static MeshGeometry make_tunnel_contour_geometry(const char* name) {
    MeshGeometry geometry{};
    std::snprintf(geometry.name, sizeof(geometry.name), "%s", name);

    constexpr float x0 = -1.725f;
    constexpr float x1 = -1.275f;
    constexpr float x2 = 1.275f;
    constexpr float x3 = 1.725f;
    constexpr float y0 = -0.65f;
    constexpr float y1 = 0.35f;
    constexpr float y2 = 0.65f;
    constexpr float z0 = -2.5f;
    constexpr float z1 = 2.5f;

    const Vector3 front[] = {
        {x0, y0, z0}, {x0, y2, z0}, {x3, y2, z0}, {x3, y0, z0},
        {x2, y0, z0}, {x2, y1, z0}, {x1, y1, z0}, {x1, y0, z0},
    };
    const Vector3 back[] = {
        {x0, y0, z1}, {x0, y2, z1}, {x3, y2, z1}, {x3, y0, z1},
        {x2, y0, z1}, {x2, y1, z1}, {x1, y1, z1}, {x1, y0, z1},
    };

    u16 front_indices[8]{};
    u16 back_indices[8]{};
    for (u32 i = 0; i < 8; ++i) {
        front_indices[i] = add_contour_vertex(&geometry, front[i]);
        back_indices[i] = add_contour_vertex(&geometry, back[i]);
    }
    for (u32 i = 0; i < 8; ++i) {
        const u32 next = (i + 1) % 8;
        add_contour_edge(&geometry, front_indices[i], front_indices[next]);
        add_contour_edge(&geometry, back_indices[i], back_indices[next]);
        add_contour_edge(&geometry, front_indices[i], back_indices[i]);
    }
    return geometry;
}

static void add_contour_mesh(Dimension* dim, u32 dimension_id, MeshGeometry geometry, const char* name, Vector3 origin, float bounds_radius) {
    const u32 geometry_id = dimension_add_geometry(dim, geometry);
    MeshGeometry* stored_geometry = arena_get(&dim->geometries, geometry_id);
    if (stored_geometry) stored_geometry->lod_geometry = geometry_id;

    MeshInstance mesh{};
    std::snprintf(mesh.name, sizeof(mesh.name), "%s", name);
    mesh.geometry = geometry_id;
    mesh.origin = demo_pos(dimension_id, origin, dim->chunk_size_m);
    mesh.se3 = MatrixIdentity();
    mesh.color = BLACK;
    mesh.bounds_radius = bounds_radius;
    mesh.lit = false;
    mesh.draw_edges = true;
    dimension_add_mesh(dim, mesh);
}

void demo_generate_world(DemoApp* app) {
    world_init(&app->world);
    reset_remote_players(app);
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
    add_static_box(dim, app->dimension_id, cube_id, "tunnel left", {9.0f, 0.5f, -3.5f}, {0.45f, 1.0f, 5.0f}, {108, 120, 145, 255}, false, false);
    add_static_box(dim, app->dimension_id, cube_id, "tunnel right", {12.0f, 0.5f, -3.5f}, {0.45f, 1.0f, 5.0f}, {108, 120, 145, 255}, false, false);
    add_static_box(dim, app->dimension_id, cube_id, "tunnel roof", {10.5f, 1.15f, -3.5f}, {3.45f, 0.30f, 5.0f}, {93, 102, 130, 255}, false, false);
    add_contour_mesh(dim, app->dimension_id, make_tunnel_contour_geometry("tunnel contours"), "tunnel contours", {10.5f, 0.65f, -3.5f}, 3.2f);

    add_static_box(dim, app->dimension_id, cube_id, "step 1", {-8.0f, 0.0f, 5.0f}, {3.0f, 1.0f, 3.0f}, {166, 151, 92, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "step 2", {-8.0f, 0.5f, 8.0f}, {3.0f, 2.0f, 3.0f}, {174, 160, 101, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "step 3", {-8.0f, 1.0f, 11.0f}, {3.0f, 3.0f, 3.0f}, {185, 171, 112, 255}, true, false);
    add_contour_mesh(dim, app->dimension_id, make_stair_contour_geometry("stair contours"), "stair contours", {-8.0f, 0.0f, 8.0f}, 5.5f);

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
    demo_generate_world(app);
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
    const bool color_pressed = IsKeyPressed(KEY_C);
    const bool host_pressed = IsKeyPressed(KEY_H);
    const bool join_pressed = IsKeyPressed(KEY_J);

    int c = GetCharPressed();
    while (c) {
        const bool shortcut_char =
            (color_pressed && (c == 'c' || c == 'C')) ||
            (host_pressed && (c == 'h' || c == 'H')) ||
            (join_pressed && (c == 'j' || c == 'J'));
        if (!shortcut_char) {
            add_text_char(app->player_name, sizeof(app->player_name), c);
        }
        c = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE)) {
        const size_t len = std::strlen(app->player_name);
        if (len > 0) app->player_name[len - 1] = 0;
    }
    if (color_pressed) {
        static const Color colors[] = {
            {90, 180, 255, 255}, {255, 120, 110, 255}, {120, 220, 150, 255},
            {255, 214, 100, 255}, {205, 145, 255, 255}
        };
        static u32 color_idx = 0;
        color_idx = (color_idx + 1) % (sizeof(colors) / sizeof(colors[0]));
        app->player_color = colors[color_idx];
    }
    if (host_pressed) {
        net_host(&app->net, app->session_name);
        demo_generate_world(app);
        app->in_game = true;
        app->mouse_captured = true;
        DisableCursor();
    }
    if (join_pressed) {
        net_join_from_clipboard(&app->net);
        demo_generate_world(app);
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

static NetPlayerState make_net_player_state(DemoApp* app, Dimension* dim, const PlayerEntity* player) {
    NetPlayerState state{};
    if (!dim || !player) return state;

    const WorldPos feet = player_feet_pos(dim, player);
    state.peer_id = app->net.local_peer_id;
    state.player_id = app->local_player_id;
    state.dimension_id = feet.dimension;
    state.chunk = feet.chunk;
    state.local = feet.local;
    state.yaw = player->yaw;
    state.pitch = player->pitch;
    state.body_radius = player->body_radius;
    state.current_height = player->current_height;
    state.color = player->color;
    std::snprintf(state.name, sizeof(state.name), "%s", player->name);
    return state;
}

static u32 find_remote_slot(DemoApp* app, u64 peer_id) {
    for (u32 i = 0; i < max_players; ++i) {
        if (app->remote_peer_ids[i] == peer_id) return i;
    }
    return invalid_id;
}

static u32 acquire_remote_slot(DemoApp* app, u64 peer_id) {
    const u32 existing = find_remote_slot(app, peer_id);
    if (id_valid(existing)) return existing;

    for (u32 i = 0; i < max_players; ++i) {
        if (app->remote_peer_ids[i] == 0) {
            app->remote_peer_ids[i] = peer_id;
            app->remote_player_ids[i] = invalid_id;
            return i;
        }
    }
    return invalid_id;
}

static bool net_peer_present(const NetSession* net, u64 peer_id) {
    for (u32 i = 0; i < net->peer_count; ++i) {
        if (net->peer_ids[i] == peer_id) return true;
    }
    return false;
}

static Vector3 worldpos_to_meters(WorldPos pos, float chunk_size) {
    return {
        static_cast<float>(world_axis_meters(pos.chunk.x, pos.local.x, chunk_size)),
        static_cast<float>(world_axis_meters(pos.chunk.y, pos.local.y, chunk_size)),
        static_cast<float>(world_axis_meters(pos.chunk.z, pos.local.z, chunk_size)),
    };
}

static void copy_current_view_to_clipboard(const Dimension* dim, const CameraView& view) {
    const WorldPos eye_pos = worldpos_offset(view.anchor, {0.0f, view.eye_height, 0.0f}, dim->chunk_size_m);
    const Vector3 eye = worldpos_to_meters(eye_pos, dim->chunk_size_m);
    const Vector3 target = eye + forward_from_angles(view.yaw, view.pitch) * 8.0f;
    const Vector3 feet = worldpos_to_meters(view.anchor, dim->chunk_size_m);

    char text[512]{};
    std::snprintf(
        text,
        sizeof(text),
        "EdgeCompareView{\"clipboard\", {%.4ff, %.4ff, %.4ff}, {%.4ff, %.4ff, %.4ff}}\n"
        "feet={%.4ff, %.4ff, %.4ff} yaw=%.6ff pitch=%.6ff",
        eye.x,
        eye.y,
        eye.z,
        target.x,
        target.y,
        target.z,
        feet.x,
        feet.y,
        feet.z,
        view.yaw,
        view.pitch);
    SetClipboardText(text);
    TraceLog(LOG_INFO, "Copied camera view to clipboard");
}

static void remove_remote_player(Dimension* dim, u32 player_id) {
    PlayerEntity* player = arena_get(&dim->players, player_id);
    if (!player) return;
    arena_remove(&dim->physics.masses, player->bottom_mass);
    arena_remove(&dim->physics.masses, player->top_mass);
    arena_remove(&dim->physics.links, player->body_link);
    arena_remove(&dim->players, player_id);
}

static void sync_remote_players(DemoApp* app, Dimension* dim) {
    if (!dim) return;

    for (u32 slot = 0; slot < max_players; ++slot) {
        const u64 peer_id = app->remote_peer_ids[slot];
        if (!peer_id || net_peer_present(&app->net, peer_id)) continue;
        if (id_valid(app->remote_player_ids[slot])) {
            remove_remote_player(dim, app->remote_player_ids[slot]);
        }
        app->remote_peer_ids[slot] = 0;
        app->remote_player_ids[slot] = invalid_id;
    }

    for (u32 net_idx = 0; net_idx < app->net.peer_count; ++net_idx) {
        if (!app->net.remote_player_valid[net_idx]) continue;
        const NetPlayerState& state = app->net.remote_players[net_idx];
        if (!state.peer_id || state.peer_id == app->net.local_peer_id) continue;

        const u32 slot = acquire_remote_slot(app, state.peer_id);
        if (!id_valid(slot)) continue;

        WorldPos feet{};
        feet.dimension = app->dimension_id;
        feet.chunk = state.chunk;
        feet.local = state.local;
        canonicalize(&feet, dim->chunk_size_m);

        if (!id_valid(app->remote_player_ids[slot])) {
            const char* name = state.name[0] ? state.name : "peer";
            app->remote_player_ids[slot] = dimension_add_player(dim, name, state.color, feet, false);
        }

        PlayerEntity* remote = arena_get(&dim->players, app->remote_player_ids[slot]);
        if (!remote) continue;
        std::snprintf(remote->name, sizeof(remote->name), "%s", state.name[0] ? state.name : "peer");
        remote->color = state.color;
        remote->yaw = state.yaw;
        remote->pitch = state.pitch;
        remote->body_radius = state.body_radius > 0.0f ? state.body_radius : player_radius_m;
        remote->current_height = state.current_height > 0.0f ? state.current_height : player_stand_height_m;
        remote->eye_height = remote->current_height < player_stand_height_m - 0.01f ? player_crouch_eye_m : player_stand_eye_m;
        remote->is_crouching = remote->current_height < player_stand_height_m - 0.01f;
        remote->connected = true;
        remote->local = false;
        player_sync_masses_to_pose(dim, remote, feet);
    }
}

static void update_game(DemoApp* app) {
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    PlayerEntity* player = local_player(app, dim);
    if (!dim || !player) return;

    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) renderer_change_scale(&app->renderer, 1);
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) renderer_change_scale(&app->renderer, -1);
    if (IsKeyPressed(KEY_F3)) app->renderer.draw_physics_debug = !app->renderer.draw_physics_debug;
    if (IsKeyPressed(KEY_C) && app->net.in_lobby) net_copy_lobby_to_clipboard(&app->net);

    collect_input(app, player);

    app->fixed_accum += fminf(GetFrameTime(), 0.1f);
    constexpr float fixed_dt = 1.0f / 60.0f;
    while (app->fixed_accum >= fixed_dt) {
        fixed_update(app, dim, fixed_dt);
        app->fixed_accum -= fixed_dt;
    }

    net_set_local_player(&app->net, make_net_player_state(app, dim, player));
    net_update(&app->net);
    sync_remote_players(app, dim);

    CameraView view{};
    view.anchor = player_feet_pos(dim, player);
    view.eye_height = player->eye_height + player->camera_y_offset;
    view.yaw = player->yaw;
    view.pitch = player->pitch;
    if (IsKeyPressed(KEY_O)) copy_current_view_to_clipboard(dim, view);
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

int demo_run_steam_host_smoke(double timeout_s, double hold_s) {
    write_smoke_log("STEAM_HOST_START timeout=%.2f hold=%.2f", timeout_s, hold_s);
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);

    auto* app = new DemoApp();
    demo_init(app);
    net_host(&app->net, app->session_name);
    demo_generate_world(app);

    const double start = GetTime();
    while (GetTime() - start < timeout_s) {
        Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
        PlayerEntity* player = dim ? local_player(app, dim) : nullptr;
        if (dim && player) {
            net_set_local_player(&app->net, make_net_player_state(app, dim, player));
        }
        net_update(&app->net);
        if (app->net.in_lobby && app->net.lobby_id) {
            write_smoke_lobby_file(app->net.lobby_id);
            write_smoke_log("STEAM_HOST_OK lobby=%llu status=\"%s\"",
                static_cast<unsigned long long>(app->net.lobby_id),
                app->net.status);
            const double hold_start = GetTime();
            while (GetTime() - hold_start < hold_s) {
                net_update(&app->net);
                WaitTime(0.05);
            }
            demo_shutdown(app);
            delete app;
            return 0;
        }
        WaitTime(0.05);
    }

    write_smoke_log("STEAM_HOST_FAIL status=\"%s\"", app->net.status);
    demo_shutdown(app);
    delete app;
    return 1;
}

int demo_run_steam_join_smoke(const char* lobby_id, double timeout_s) {
    write_smoke_log("STEAM_JOIN_START lobby=%s timeout=%.2f", lobby_id ? lobby_id : "", timeout_s);
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);

    auto* app = new DemoApp();
    demo_init(app);
    SetClipboardText(lobby_id ? lobby_id : "");
    net_join_from_clipboard(&app->net);
    demo_generate_world(app);

    const double start = GetTime();
    while (GetTime() - start < timeout_s) {
        Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
        PlayerEntity* player = dim ? local_player(app, dim) : nullptr;
        if (dim && player) {
            net_set_local_player(&app->net, make_net_player_state(app, dim, player));
        }
        net_update(&app->net);
        if (app->net.in_lobby && app->net.lobby_id) {
            write_smoke_log("STEAM_JOIN_OK lobby=%llu status=\"%s\" peers=%u",
                static_cast<unsigned long long>(app->net.lobby_id),
                app->net.status,
                static_cast<unsigned>(app->net.peer_count));
            demo_shutdown(app);
            delete app;
            return 0;
        }
        WaitTime(0.05);
    }

    write_smoke_log("STEAM_JOIN_FAIL status=\"%s\"", app->net.status);
    demo_shutdown(app);
    delete app;
    return 1;
}

} // namespace ol
