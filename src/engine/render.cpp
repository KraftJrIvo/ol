#include "engine/render.h"

#include "rlgl.h"
#include "resources.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace ol {

static int even_floor(int v) {
    return v > 2 ? (v & ~1) : 2;
}

static Texture2D make_white_texture() {
    Image img = GenImageColor(1, 1, WHITE);
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

void renderer_init(RenderState* renderer) {
    renderer->white_texture = make_white_texture();
    renderer->white_ready = true;

    renderer->font = LoadFontFromMemory(
        ".ttf",
        res_RuffCut_Regular_ttf,
        static_cast<int>(res_RuffCut_Regular_ttf_len),
        48,
        nullptr,
        0
    );
    renderer->font_ready = IsFontValid(renderer->font);
}

void renderer_shutdown(RenderState* renderer) {
    if (renderer->target_ready) {
        UnloadRenderTexture(renderer->target);
        renderer->target_ready = false;
    }
    if (renderer->white_ready) {
        UnloadTexture(renderer->white_texture);
        renderer->white_ready = false;
    }
    if (renderer->font_ready) {
        UnloadFont(renderer->font);
        renderer->font_ready = false;
    }
}

void renderer_change_scale(RenderState* renderer, int delta) {
    renderer->scale_power += delta;
    if (renderer->scale_power < 0) renderer->scale_power = 0;
    if (renderer->scale_power > 4) renderer->scale_power = 4;
}

void renderer_ensure_target(RenderState* renderer) {
    const int win_w = even_floor(GetScreenWidth());
    const int win_h = even_floor(GetScreenHeight());
    const int div = 1 << renderer->scale_power;
    const int native_w = even_floor(win_w / div);
    const int native_h = even_floor(win_h / div);

    if (renderer->target_ready &&
        renderer->native_w == native_w &&
        renderer->native_h == native_h &&
        renderer->window_w == win_w &&
        renderer->window_h == win_h) {
        return;
    }

    if (renderer->target_ready) {
        UnloadRenderTexture(renderer->target);
        renderer->target_ready = false;
    }

    renderer->target = LoadRenderTexture(native_w, native_h);
    SetTextureFilter(renderer->target.texture, TEXTURE_FILTER_POINT);
    renderer->target_ready = true;
    renderer->native_w = native_w;
    renderer->native_h = native_h;
    renderer->window_w = win_w;
    renderer->window_h = win_h;
}

static bool chunk_visible(const Dimension* dim, ChunkCoord camera, ChunkCoord chunk, float* out_dist) {
    const float dx = static_cast<float>(chunk.x - camera.x);
    const float dy = static_cast<float>(chunk.y - camera.y);
    const float dz = static_cast<float>(chunk.z - camera.z);
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (out_dist) *out_dist = dist;
    return dist <= static_cast<float>(dim->render_radius_chunks);
}

static float fog_factor(const Dimension* dim, float chunk_dist) {
    const float rr = static_cast<float>(dim->render_radius_chunks);
    const float qr = static_cast<float>(dim->quality_render_radius_chunks);
    if (rr <= qr) return 0.0f;
    return clampf((chunk_dist - qr) / (rr - qr), 0.0f, 1.0f);
}

static Color mix_color(Color a, Color b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    return {
        static_cast<unsigned char>(a.r + (b.r - a.r) * t),
        static_cast<unsigned char>(a.g + (b.g - a.g) * t),
        static_cast<unsigned char>(a.b + (b.b - a.b) * t),
        static_cast<unsigned char>(a.a + (b.a - a.a) * t),
    };
}

static void draw_engine_text(RenderState* renderer, const char* text, Vector2 pos, float size, Color color) {
    if (renderer->font_ready) {
        DrawTextEx(renderer->font, text, pos, size, 1.0f, color);
    } else {
        DrawText(text, static_cast<int>(pos.x), static_cast<int>(pos.y), static_cast<int>(size), color);
    }
}

static float face_light(Dimension* dim, const CameraView& view, Vector3 face_center, Vector3 normal) {
    float light = dim->ambient;
    for (u32 slot = 0; slot < dim->lights.count; ++slot) {
        const LightSource* src = &dim->lights.data[slot];
        const Vector3 rel = world_delta_meters(src->pos, view.anchor, dim->chunk_size_m);
        const Vector3 to_light = rel - face_center;
        const float dist = safe_len(to_light);
        if (dist > src->radius || dist <= 0.001f) continue;
        const float ndotl = fmaxf(0.0f, Vector3DotProduct(normal, to_light / dist));
        const float attenuation = 1.0f - clampf(dist / src->radius, 0.0f, 1.0f);
        light += ndotl * attenuation * src->intensity;
    }
    return clampf(light, 0.0f, 2.0f);
}

struct ScreenEdge {
    Vector2 a{};
    Vector2 b{};
    Color color = BLACK;
    float thickness = 1.0f;
};

struct ScreenEdgeList {
    std::array<ScreenEdge, max_render_edges> edges{};
    u32 count = 0;
};

static void push_screen_edge(ScreenEdgeList* list, Vector2 a, Vector2 b, Color color, u8 thickness) {
    if (list->count >= max_render_edges) return;
    list->edges[list->count++] = {a, b, color, static_cast<float>(thickness ? thickness : 1)};
}

static bool edge_endpoint_visible(Vector3 p, const Camera3D& camera) {
    const Vector3 forward = safe_norm(camera.target - camera.position, {0.0f, 0.0f, -1.0f});
    return Vector3DotProduct(p - camera.position, forward) > 0.02f;
}

static void draw_mesh_instance(RenderState* renderer, Dimension* dim, const CameraView& view, const Camera3D& camera, ScreenEdgeList* edge_list, const MeshInstance* mesh, float chunk_dist) {
    if (!mesh || !mesh->visible) return;
    u32 geometry_id = mesh->geometry;
    const MeshGeometry* geometry = arena_get(&dim->geometries, geometry_id);
    if (!geometry) return;
    if (chunk_dist > static_cast<float>(dim->quality_render_radius_chunks)) {
        if (!id_valid(geometry->lod_geometry)) return;
        geometry = arena_get(&dim->geometries, geometry->lod_geometry);
        if (!geometry) return;
    }

    Vector3 transformed[max_vertices_per_geometry]{};
    const Vector3 origin = world_delta_meters(mesh->origin, view.anchor, dim->chunk_size_m);
    const Matrix basis = matrix_no_translation(mesh->se3);
    for (u32 i = 0; i < geometry->vertex_count; ++i) {
        transformed[i] = Vector3Transform(geometry->vertices[i], basis) + origin;
    }

    Color base = resolve_mesh_color(mesh);
    const float ff = fog_factor(dim, chunk_dist);

    for (u32 i = 0; i < geometry->triangle_count; ++i) {
        const Triangle tri = geometry->triangles[i];
        const Vector3 a = transformed[tri.a];
        const Vector3 b = transformed[tri.b];
        const Vector3 c = transformed[tri.c];
        const Vector3 n = safe_norm(Vector3CrossProduct(b - a, c - a));
        Color shaded = mesh->lit ? color_scaled(base, face_light(dim, view, origin, n)) : base;
        shaded = mix_color(shaded, dim->fog_color, ff);
        DrawTriangle3D(a, b, c, shaded);
    }

    if (!mesh->draw_edges) return;

    const Color edge_color = mix_color(BLACK, dim->fog_color, ff);
    for (u32 i = 0; i < geometry->edge_count; ++i) {
        const Edge e = geometry->edges[i];
        if (!edge_endpoint_visible(transformed[e.a], camera) || !edge_endpoint_visible(transformed[e.b], camera)) continue;
        const Vector2 a = GetWorldToScreenEx(transformed[e.a], camera, renderer->native_w, renderer->native_h);
        const Vector2 b = GetWorldToScreenEx(transformed[e.b], camera, renderer->native_w, renderer->native_h);
        push_screen_edge(edge_list, a, b, edge_color, e.thickness_px);
    }
}

static void draw_sprites(RenderState* renderer, Dimension* dim, const CameraView& view, const Chunk* chunk) {
    Camera3D camera{};
    camera.position = {0.0f, view.eye_height, 0.0f};
    camera.target = camera.position + forward_from_angles(view.yaw, view.pitch);
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = renderer->fov;
    camera.projection = CAMERA_PERSPECTIVE;

    Rectangle source = {0.0f, 0.0f, 1.0f, 1.0f};
    for (u32 i = 0; i < chunk->sprite_count; ++i) {
        const SpriteInstance* sprite = arena_get(&dim->sprites, chunk->sprites[i]);
        if (!sprite || !sprite->visible) continue;
        const Vector3 rel = world_delta_meters(sprite->origin, view.anchor, dim->chunk_size_m);
        const Vector3 up = sprite->billboard == billboard_vertical ? Vector3{0.0f, 1.0f, 0.0f} : camera.up;
        DrawBillboardPro(camera, renderer->white_texture, source, rel, up, sprite->size, sprite->size * 0.5f, 0.0f, sprite->color);
    }
}

static void draw_physics(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id) {
    if (!renderer->draw_physics_debug) return;

    for (u32 slot = 0; slot < dim->physics.boxes.count; ++slot) {
        const BoxCollider* box = &dim->physics.boxes.data[slot];
        const Vector3 rel = world_delta_meters(box->pos, view.anchor, dim->chunk_size_m);
        Color c = box->color;
        c.a = 80;
        DrawCubeV(rel, box->half * 2.0f, c);
        DrawCubeWiresV(rel, box->half * 2.0f, Fade(BLACK, 0.45f));
    }

    for (u32 slot = 0; slot < dim->players.count; ++slot) {
        const u32 player_id = arena_id_at_slot(&dim->players, slot);
        const PlayerEntity* player = &dim->players.data[slot];
        if (player_id == local_player_id) continue;
        const PointMass* bottom = arena_get(&dim->physics.masses, player->bottom_mass);
        const PointMass* top = arena_get(&dim->physics.masses, player->top_mass);
        if (!bottom || !top) continue;
        const Vector3 a = world_delta_meters(bottom->pos, view.anchor, dim->chunk_size_m);
        const Vector3 b = world_delta_meters(top->pos, view.anchor, dim->chunk_size_m);
        DrawCylinderEx(a, b, bottom->radius, top->radius, 16, player->color);
        DrawCylinderWiresEx(a, b, bottom->radius, top->radius, 16, BLACK);
    }

    for (u32 slot = 0; slot < dim->lights.count; ++slot) {
        const LightSource* light = &dim->lights.data[slot];
        const Vector3 rel = world_delta_meters(light->pos, view.anchor, dim->chunk_size_m);
        DrawSphere(rel, 0.12f, light->color);
    }
}

void renderer_draw_dimension(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id) {
    if (!renderer->target_ready) renderer_ensure_target(renderer);

    Camera3D camera{};
    camera.position = {0.0f, view.eye_height, 0.0f};
    camera.target = camera.position + forward_from_angles(view.yaw, view.pitch);
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = renderer->fov;
    camera.projection = CAMERA_PERSPECTIVE;

    BeginTextureMode(renderer->target);
    ClearBackground(dim->sky_top);
    DrawRectangleGradientV(0, 0, renderer->native_w, renderer->native_h, dim->sky_top, dim->sky_bottom);
    BeginMode3D(camera);

    ScreenEdgeList edge_list{};
    for (u32 chunk_slot = 0; chunk_slot < dim->chunks.count; ++chunk_slot) {
        const Chunk* chunk = &dim->chunks.data[chunk_slot];
        float chunk_dist = 0.0f;
        if (!chunk_visible(dim, view.anchor.chunk, chunk->coord, &chunk_dist)) continue;

        for (u32 i = 0; i < chunk->mesh_count; ++i) {
            draw_mesh_instance(renderer, dim, view, camera, &edge_list, arena_get(&dim->meshes, chunk->meshes[i]), chunk_dist);
        }
        draw_sprites(renderer, dim, view, chunk);
    }

    draw_physics(renderer, dim, view, local_player_id);

    EndMode3D();

    for (u32 i = 0; i < edge_list.count; ++i) {
        const ScreenEdge& edge = edge_list.edges[i];
        DrawLineEx(edge.a, edge.b, edge.thickness, edge.color);
    }

    char overlay[96]{};
    std::snprintf(overlay, sizeof(overlay), "scale 1/%d | native %dx%d", 1 << renderer->scale_power, renderer->native_w, renderer->native_h);
    draw_engine_text(renderer, overlay, {10.0f, 10.0f}, 16.0f, Fade(BLACK, 0.72f));
    EndTextureMode();

    BeginDrawing();
    ClearBackground(BLACK);
    Rectangle src = {0.0f, 0.0f, static_cast<float>(renderer->target.texture.width), -static_cast<float>(renderer->target.texture.height)};
    Rectangle dst = {0.0f, 0.0f, static_cast<float>(renderer->window_w), static_cast<float>(renderer->window_h)};
    DrawTexturePro(renderer->target.texture, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
    EndDrawing();
}

} // namespace ol
