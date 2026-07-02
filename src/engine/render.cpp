#include "engine/render.h"

#include "rlgl.h"
#include "resources.h"

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

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

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

static Texture2D load_texture_from_memory(const char* file_type, const unsigned char* data, unsigned long long data_len, bool* out_ready) {
    Texture2D texture{};
    if (out_ready) *out_ready = false;
    Image image = LoadImageFromMemory(file_type, data, static_cast<int>(data_len));
    if (!IsImageValid(image)) return texture;

    texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (IsTextureValid(texture)) {
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
        if (out_ready) *out_ready = true;
    }
    return texture;
}

static Shader load_sprite_alpha_shader(bool* out_ready) {
    if (out_ready) *out_ready = false;

    static const char* fs =
        "#version 330\n"
        "in vec2 fragTexCoord;\n"
        "in vec4 fragColor;\n"
        "uniform sampler2D texture0;\n"
        "uniform vec4 colDiffuse;\n"
        "out vec4 finalColor;\n"
        "void main() {\n"
        "    vec4 texelColor = texture(texture0, fragTexCoord);\n"
        "    if (texelColor.a < 0.5) discard;\n"
        "    finalColor = texelColor * colDiffuse * fragColor;\n"
        "}\n";

    Shader shader = LoadShaderFromMemory(nullptr, fs);
    if (IsShaderValid(shader) && out_ready) *out_ready = true;
    return shader;
}

void renderer_init(RenderState* renderer) {
    renderer->white_texture = make_white_texture();
    renderer->white_ready = true;
    renderer->life_texture = load_texture_from_memory(".png", res_life_png, res_life_png_len, &renderer->life_ready);
    renderer->cross_texture = load_texture_from_memory(".png", res_cross_png, res_cross_png_len, &renderer->cross_ready);
    renderer->sprite_alpha_shader = load_sprite_alpha_shader(&renderer->sprite_alpha_shader_ready);

    renderer->font = LoadFontFromMemory(
        ".ttf",
        res_RuffCut_Regular_ttf,
        static_cast<int>(res_RuffCut_Regular_ttf_len),
        96,
        nullptr,
        0
    );
    renderer->font_ready = IsFontValid(renderer->font);
    if (renderer->font_ready) {
        SetTextureFilter(renderer->font.texture, TEXTURE_FILTER_BILINEAR);
    }
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
    if (renderer->life_ready) {
        UnloadTexture(renderer->life_texture);
        renderer->life_ready = false;
    }
    if (renderer->cross_ready) {
        UnloadTexture(renderer->cross_texture);
        renderer->cross_ready = false;
    }
    if (renderer->sprite_alpha_shader_ready) {
        UnloadShader(renderer->sprite_alpha_shader);
        renderer->sprite_alpha_shader_ready = false;
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

void renderer_draw_target_to_screen(RenderState* renderer) {
    if (!renderer || !renderer->target_ready) return;

    ClearBackground(BLACK);
    Rectangle src = {0.0f, 0.0f, static_cast<float>(renderer->target.texture.width), -static_cast<float>(renderer->target.texture.height)};
    Rectangle dst = {0.0f, 0.0f, static_cast<float>(renderer->window_w), static_cast<float>(renderer->window_h)};
    DrawTexturePro(renderer->target.texture, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
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

static Vector2 measure_engine_text(RenderState* renderer, const char* text, float size) {
    if (renderer->font_ready) {
        return MeasureTextEx(renderer->font, text, size, 1.0f);
    }
    return {static_cast<float>(MeasureText(text, static_cast<int>(size))), size};
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
    float depth_a = 0.0f;
    float depth_b = 0.0f;
    float inv_w_a = 1.0f;
    float inv_w_b = 1.0f;
    Vector3 rel_a{};
    Vector3 rel_b{};
    Color color = BLACK;
    float thickness = 1.0f;
    bool use_scene_occlusion = true;
};

struct ScreenEdgeList {
    std::array<ScreenEdge, max_render_edges> edges{};
    u32 count = 0;
};

struct SceneTriangle {
    Vector3 a{};
    Vector3 b{};
    Vector3 c{};
};

struct SceneTriangleList {
    std::vector<SceneTriangle> triangles{};
};

struct ProjectedEdgePoint {
    Vector2 screen{};
    float depth = 0.0f;
    float inv_w = 1.0f;
};

static float lerp_float(float a, float b, float t) {
    return a + (b - a) * t;
}

static bool clip_line_param(float p, float q, float* t0, float* t1) {
    if (std::fabs(p) < 0.000001f) return q >= 0.0f;

    const float r = q / p;
    if (p < 0.0f) {
        if (r > *t1) return false;
        if (r > *t0) *t0 = r;
    } else {
        if (r < *t0) return false;
        if (r < *t1) *t1 = r;
    }
    return true;
}

static bool clip_screen_edge(Vector2* a, Vector2* b, float* depth_a, float* depth_b, int width, int height, float thickness, float* out_t0 = nullptr, float* out_t1 = nullptr) {
    const float margin = fmaxf(2.0f, thickness * 0.75f + 1.0f);
    const float min_x = -margin;
    const float min_y = -margin;
    const float max_x = static_cast<float>(width) + margin;
    const float max_y = static_cast<float>(height) + margin;

    const Vector2 original_a = *a;
    const Vector2 original_b = *b;
    const float original_depth_a = *depth_a;
    const float original_depth_b = *depth_b;
    const Vector2 d = original_b - original_a;

    float t0 = 0.0f;
    float t1 = 1.0f;
    if (!clip_line_param(-d.x, original_a.x - min_x, &t0, &t1)) return false;
    if (!clip_line_param( d.x, max_x - original_a.x, &t0, &t1)) return false;
    if (!clip_line_param(-d.y, original_a.y - min_y, &t0, &t1)) return false;
    if (!clip_line_param( d.y, max_y - original_a.y, &t0, &t1)) return false;
    if (t1 < t0) return false;

    *a = original_a + d * t0;
    *b = original_a + d * t1;
    *depth_a = lerp_float(original_depth_a, original_depth_b, t0);
    *depth_b = lerp_float(original_depth_a, original_depth_b, t1);
    if (out_t0) *out_t0 = t0;
    if (out_t1) *out_t1 = t1;
    return Vector2Length(*b - *a) > 0.001f;
}

static bool clip_edge_to_near_plane(Vector3* a, Vector3* b, const Camera3D& camera) {
    const Vector3 forward = safe_norm(camera.target - camera.position, {0.0f, 0.0f, -1.0f});
    const float near_plane = static_cast<float>(rlGetCullDistanceNear());
    const float da = Vector3DotProduct(*a - camera.position, forward);
    const float db = Vector3DotProduct(*b - camera.position, forward);
    if (da < near_plane && db < near_plane) return false;

    if (da < near_plane || db < near_plane) {
        const float denom = db - da;
        if (std::fabs(denom) < 0.000001f) return false;
        const float t = (near_plane - da) / denom;
        const Vector3 clipped = *a + (*b - *a) * t;
        if (da < near_plane) *a = clipped;
        else *b = clipped;
    }
    return true;
}

static bool project_edge_point(Vector3 p, const Matrix& view_matrix, const Matrix& projection_matrix, int width, int height, ProjectedEdgePoint* out) {
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
    out->inv_w = inv_w;
    return true;
}

static Vector3 perspective_lerp_rel(Vector3 a, Vector3 b, float inv_w_a, float inv_w_b, float t) {
    const float wa = inv_w_a * (1.0f - t);
    const float wb = inv_w_b * t;
    const float denom = wa + wb;
    if (std::fabs(denom) < 0.000001f) return a + (b - a) * t;
    return (a * wa + b * wb) / denom;
}

static void push_screen_edge(ScreenEdgeList* list, ProjectedEdgePoint a, ProjectedEdgePoint b, Vector3 rel_a, Vector3 rel_b, int width, int height, Color color, u8 thickness_px, bool use_scene_occlusion) {
    if (list->count >= max_render_edges) return;

    const Vector3 original_rel_a = rel_a;
    const Vector3 original_rel_b = rel_b;
    const float original_inv_w_a = a.inv_w;
    const float original_inv_w_b = b.inv_w;
    const float thickness = static_cast<float>(thickness_px ? thickness_px : 1);
    float clip_t0 = 0.0f;
    float clip_t1 = 1.0f;
    if (!clip_screen_edge(&a.screen, &b.screen, &a.depth, &b.depth, width, height, thickness, &clip_t0, &clip_t1)) return;
    a.inv_w = lerp_float(original_inv_w_a, original_inv_w_b, clip_t0);
    b.inv_w = lerp_float(original_inv_w_a, original_inv_w_b, clip_t1);
    const Vector3 clipped_rel_a = perspective_lerp_rel(original_rel_a, original_rel_b, original_inv_w_a, original_inv_w_b, clip_t0);
    const Vector3 clipped_rel_b = perspective_lerp_rel(original_rel_a, original_rel_b, original_inv_w_a, original_inv_w_b, clip_t1);
    list->edges[list->count++] = {a.screen, b.screen, a.depth, b.depth, a.inv_w, b.inv_w, clipped_rel_a, clipped_rel_b, color, thickness, use_scene_occlusion};
}

static void draw_flat_screen_edges(const ScreenEdgeList* list) {
    if (!list || list->count == 0) return;
    for (u32 i = 0; i < list->count; ++i) {
        const ScreenEdge& edge = list->edges[i];
        DrawLineEx(edge.a, edge.b, edge.thickness, edge.color);
    }
}

struct DepthBuffer {
    std::vector<float> pixels{};
    int width = 0;
    int height = 0;
};

struct SpriteDepthOcclusion {
    DepthBuffer scene{};
    DepthBuffer after_sprites{};
};

static DepthBuffer read_current_depth_buffer(int width, int height) {
    DepthBuffer depth{};
    if (width <= 0 || height <= 0) return depth;

    depth.width = width;
    depth.height = height;
    depth.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height), 1.0f);
    rlDrawRenderBatchActive();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.pixels.data());
    return depth;
}

static bool depth_value_at(const DepthBuffer& depth, Vector2 p, float* out_value) {
    if (depth.pixels.empty()) return false;

    const int center_x = static_cast<int>(floorf(p.x + 0.5f));
    const int center_y = static_cast<int>(floorf(p.y + 0.5f));
    if (center_x < 0 || center_x >= depth.width || center_y < 0 || center_y >= depth.height) return false;

    const int gl_y = depth.height - 1 - center_y;
    *out_value = depth.pixels[static_cast<size_t>(gl_y) * static_cast<size_t>(depth.width) + static_cast<size_t>(center_x)];
    return true;
}

static Vector2 edge_point_at(const ScreenEdge& edge, float t) {
    return edge.a + (edge.b - edge.a) * t;
}

static float edge_depth_at(const ScreenEdge& edge, float t) {
    return lerp_float(edge.depth_a, edge.depth_b, t);
}

static Vector3 edge_rel_point_at(const ScreenEdge& edge, float t) {
    return perspective_lerp_rel(edge.rel_a, edge.rel_b, edge.inv_w_a, edge.inv_w_b, t);
}

static bool ray_intersects_scene_triangle(Vector3 origin, Vector3 dir, const SceneTriangle& tri, float max_dist) {
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
    return dist > 0.002f && dist < max_dist - 0.035f;
}

static bool scene_point_visible(const SceneTriangleList& scene, Vector3 camera_pos, Vector3 point) {
    const Vector3 delta = point - camera_pos;
    const float dist = safe_len(delta);
    if (dist <= 0.001f) return true;

    const Vector3 dir = delta / dist;
    for (const SceneTriangle& tri : scene.triangles) {
        if (ray_intersects_scene_triangle(camera_pos, dir, tri, dist)) return false;
    }
    return true;
}

static bool sprite_occlusion_visible(const SpriteDepthOcclusion& occlusion, Vector2 p, float edge_depth) {
    if (occlusion.scene.pixels.empty() || occlusion.after_sprites.pixels.empty()) return true;

    constexpr float edge_depth_bias = 0.00035f;
    constexpr float sprite_depth_delta = 0.00035f;

    float scene_depth = 1.0f;
    float sprite_depth = 1.0f;
    if (!depth_value_at(occlusion.scene, p, &scene_depth)) return true;
    if (!depth_value_at(occlusion.after_sprites, p, &sprite_depth)) return true;

    if (sprite_depth >= scene_depth - sprite_depth_delta) return true;
    return edge_depth <= sprite_depth + edge_depth_bias;
}

static bool edge_sample_visible(const ScreenEdge& edge, const SpriteDepthOcclusion& sprite_occlusion, const SceneTriangleList& scene, Vector3 camera_pos, float t) {
    if (edge.use_scene_occlusion && !scene.triangles.empty()) {
        if (!scene_point_visible(scene, camera_pos, edge_rel_point_at(edge, t))) return false;
    }

    const Vector2 p = edge_point_at(edge, t);
    const float edge_depth = edge_depth_at(edge, t);
    return sprite_occlusion_visible(sprite_occlusion, p, edge_depth);
}

static float refine_visibility_boundary(const ScreenEdge& edge, const SpriteDepthOcclusion& sprite_occlusion, const SceneTriangleList& scene, Vector3 camera_pos, float t0, bool visible0, float t1) {
    float lo = t0;
    float hi = t1;
    for (int i = 0; i < 8; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (edge_sample_visible(edge, sprite_occlusion, scene, camera_pos, mid) == visible0) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

static void draw_edge_segment(const ScreenEdge& edge, float t0, float t1) {
    t0 = clampf(t0, 0.0f, 1.0f);
    t1 = clampf(t1, 0.0f, 1.0f);
    if (t1 <= t0) return;

    const Vector2 a = edge_point_at(edge, t0);
    const Vector2 b = edge_point_at(edge, t1);
    if (Vector2Length(b - a) < 0.75f) return;
    DrawLineEx(a, b, edge.thickness, edge.color);
}

struct EdgeVisibleInterval {
    float start = 0.0f;
    float end = 0.0f;
};

static void draw_edge_intervals(const ScreenEdge& edge, const std::vector<EdgeVisibleInterval>& raw_intervals) {
    if (raw_intervals.empty()) return;

    const float edge_len = Vector2Length(edge.b - edge.a);
    if (edge_len < 0.75f) return;

    constexpr float min_visible_px = 3.0f;
    constexpr float merge_gap_px = 4.0f;
    std::vector<EdgeVisibleInterval> intervals;
    intervals.reserve(raw_intervals.size());

    for (EdgeVisibleInterval interval : raw_intervals) {
        interval.start = clampf(interval.start, 0.0f, 1.0f);
        interval.end = clampf(interval.end, 0.0f, 1.0f);
        if ((interval.end - interval.start) * edge_len < min_visible_px) continue;

        if (!intervals.empty() && (interval.start - intervals.back().end) * edge_len <= merge_gap_px) {
            intervals.back().end = interval.end;
        } else {
            intervals.push_back(interval);
        }
    }

    for (const EdgeVisibleInterval& interval : intervals) {
        draw_edge_segment(edge, interval.start, interval.end);
    }
}

static void draw_depth_cut_screen_edges(RenderState* renderer, const ScreenEdgeList* list, const SpriteDepthOcclusion& sprite_occlusion, const SceneTriangleList& scene, Vector3 camera_pos) {
    if (!list || list->count == 0) return;

    rlDisableDepthTest();
    rlEnableDepthMask();

    for (u32 i = 0; i < list->count; ++i) {
        const ScreenEdge& edge = list->edges[i];
        const Vector2 d = edge.b - edge.a;
        const float dominant_len = fmaxf(fabsf(d.x), fabsf(d.y));
        const int sample_count = static_cast<int>(fmaxf(1.0f, ceilf(dominant_len)));

        float prev_t = 0.0f;
        bool prev_visible = edge_sample_visible(edge, sprite_occlusion, scene, camera_pos, prev_t);
        float visible_start = prev_visible ? 0.0f : -1.0f;
        std::vector<EdgeVisibleInterval> intervals;

        for (int sample = 1; sample <= sample_count; ++sample) {
            const float t = static_cast<float>(sample) / static_cast<float>(sample_count);
            const bool visible = edge_sample_visible(edge, sprite_occlusion, scene, camera_pos, t);
            if (visible != prev_visible) {
                const float boundary = refine_visibility_boundary(edge, sprite_occlusion, scene, camera_pos, prev_t, prev_visible, t);
                if (prev_visible) intervals.push_back({visible_start, boundary});
                else visible_start = boundary;
            }

            prev_t = t;
            prev_visible = visible;
        }

        if (prev_visible) intervals.push_back({visible_start, 1.0f});
        draw_edge_intervals(edge, intervals);
    }
}

static void push_world_screen_edge(RenderState* renderer, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, ScreenEdgeList* edge_list, Vector3 a, Vector3 b, Color color, u8 thickness_px, bool use_scene_occlusion = true) {
    if (!clip_edge_to_near_plane(&a, &b, camera)) return;

    ProjectedEdgePoint pa{};
    ProjectedEdgePoint pb{};
    if (!project_edge_point(a, view_matrix, projection_matrix, renderer->native_w, renderer->native_h, &pa)) return;
    if (!project_edge_point(b, view_matrix, projection_matrix, renderer->native_w, renderer->native_h, &pb)) return;
    push_screen_edge(edge_list, pa, pb, a, b, renderer->native_w, renderer->native_h, color, thickness_px, use_scene_occlusion);
}

static void draw_mesh_instance(RenderState* renderer, Dimension* dim, const CameraView& view, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, ScreenEdgeList* edge_list, SceneTriangleList* scene_triangles, const MeshInstance* mesh, float chunk_dist) {
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
        if (scene_triangles) scene_triangles->triangles.push_back({a, b, c});
    }

    if (!mesh->draw_edges) return;

    const Color edge_color = mix_color(BLACK, dim->fog_color, ff);
    for (u32 i = 0; i < geometry->edge_count; ++i) {
        const Edge e = geometry->edges[i];
        push_world_screen_edge(renderer, camera, view_matrix, projection_matrix, edge_list, transformed[e.a], transformed[e.b], edge_color, e.thickness_px);
    }
}

static Texture2D sprite_texture(RenderState* renderer, u32 texture_id) {
    if (texture_id == render_texture_life && renderer->life_ready) return renderer->life_texture;
    if (texture_id == render_texture_cross && renderer->cross_ready) return renderer->cross_texture;
    return renderer->white_texture;
}

struct SpriteQuad {
    Vector3 bottom_left{};
    Vector3 bottom_right{};
    Vector3 top_right{};
    Vector3 top_left{};
};

static SpriteQuad make_sprite_quad(const SpriteInstance* sprite, Vector3 center, const Camera3D& camera) {
    Vector3 right = {1.0f, 0.0f, 0.0f};
    Vector3 up = {0.0f, 1.0f, 0.0f};

    if (sprite->billboard == billboard_full_3d) {
        const Vector3 forward = safe_norm(camera.position - center, -safe_norm(camera.target - camera.position, {0.0f, 0.0f, -1.0f}));
        right = safe_norm(Vector3CrossProduct(camera.up, forward), {1.0f, 0.0f, 0.0f});
        up = safe_norm(Vector3CrossProduct(forward, right), camera.up);
    } else if (sprite->billboard == billboard_vertical) {
        Vector3 to_camera = camera.position - center;
        to_camera.y = 0.0f;
        const Vector3 forward = safe_norm(to_camera, {0.0f, 0.0f, 1.0f});
        up = {0.0f, 1.0f, 0.0f};
        right = safe_norm(Vector3CrossProduct(up, forward), {1.0f, 0.0f, 0.0f});
    } else {
        const Matrix basis = matrix_no_translation(sprite->se3);
        right = safe_norm(Vector3Transform({1.0f, 0.0f, 0.0f}, basis), {1.0f, 0.0f, 0.0f});
        up = safe_norm(Vector3Transform({0.0f, 1.0f, 0.0f}, basis), {0.0f, 1.0f, 0.0f});
    }

    const Vector3 half_right = right * (sprite->size.x * 0.5f);
    const Vector3 half_up = up * (sprite->size.y * 0.5f);
    return {
        center - half_right - half_up,
        center + half_right - half_up,
        center + half_right + half_up,
        center - half_right + half_up
    };
}

static void draw_textured_sprite_quad(Texture2D texture, Rectangle source, const SpriteQuad& quad, Color tint) {
    if (!IsTextureValid(texture)) return;

    const float u0 = source.x / static_cast<float>(texture.width);
    const float v0 = source.y / static_cast<float>(texture.height);
    const float u1 = (source.x + source.width) / static_cast<float>(texture.width);
    const float v1 = (source.y + source.height) / static_cast<float>(texture.height);

    rlDisableBackfaceCulling();
    rlSetTexture(texture.id);
    rlBegin(RL_QUADS);
        rlColor4ub(tint.r, tint.g, tint.b, tint.a);
        rlTexCoord2f(u0, v1); rlVertex3f(quad.bottom_left.x, quad.bottom_left.y, quad.bottom_left.z);
        rlTexCoord2f(u1, v1); rlVertex3f(quad.bottom_right.x, quad.bottom_right.y, quad.bottom_right.z);
        rlTexCoord2f(u1, v0); rlVertex3f(quad.top_right.x, quad.top_right.y, quad.top_right.z);
        rlTexCoord2f(u0, v0); rlVertex3f(quad.top_left.x, quad.top_left.y, quad.top_left.z);
    rlEnd();
    rlSetTexture(0);
    rlEnableBackfaceCulling();
}

static void draw_sprites(RenderState* renderer, Dimension* dim, const CameraView& view, const Camera3D& camera, const Chunk* chunk) {
    if (renderer->sprite_alpha_shader_ready) {
        BeginShaderMode(renderer->sprite_alpha_shader);
    } else {
        rlDisableDepthMask();
    }

    for (u32 i = 0; i < chunk->sprite_count; ++i) {
        const SpriteInstance* sprite = arena_get(&dim->sprites, chunk->sprites[i]);
        if (!sprite || !sprite->visible) continue;
        const Vector3 rel = world_delta_meters(sprite->origin, view.anchor, dim->chunk_size_m);
        const Texture2D texture = sprite_texture(renderer, sprite->texture_id);
        const Rectangle source = {0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)};
        const SpriteQuad quad = make_sprite_quad(sprite, rel, camera);
        draw_textured_sprite_quad(texture, source, quad, sprite->color);
    }

    if (renderer->sprite_alpha_shader_ready) {
        EndShaderMode();
    } else {
        rlEnableDepthMask();
    }
}

static void push_player_ring_edges(RenderState* renderer, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, ScreenEdgeList* edge_list, Vector3 center, float radius, Color color) {
    constexpr u32 segments = 32;
    constexpr u8 thickness_px = 2;
    for (u32 i = 0; i < segments; ++i) {
        const float a0 = static_cast<float>(i) * 2.0f * PI / static_cast<float>(segments);
        const float a1 = static_cast<float>(i + 1) * 2.0f * PI / static_cast<float>(segments);
        const Vector3 p0 = center + Vector3{std::cos(a0) * radius, 0.0f, std::sin(a0) * radius};
        const Vector3 p1 = center + Vector3{std::cos(a1) * radius, 0.0f, std::sin(a1) * radius};
        push_world_screen_edge(renderer, camera, view_matrix, projection_matrix, edge_list, p0, p1, color, thickness_px);
    }
}

static void push_player_cylinder_triangles(SceneTriangleList* scene_triangles, Vector3 bottom, Vector3 top, float radius) {
    if (!scene_triangles) return;

    constexpr u32 segments = 32;
    for (u32 i = 0; i < segments; ++i) {
        const float a0 = static_cast<float>(i) * 2.0f * PI / static_cast<float>(segments);
        const float a1 = static_cast<float>(i + 1) * 2.0f * PI / static_cast<float>(segments);
        const Vector3 b0 = bottom + Vector3{std::cos(a0) * radius, 0.0f, std::sin(a0) * radius};
        const Vector3 b1 = bottom + Vector3{std::cos(a1) * radius, 0.0f, std::sin(a1) * radius};
        const Vector3 t0 = top + Vector3{std::cos(a0) * radius, 0.0f, std::sin(a0) * radius};
        const Vector3 t1 = top + Vector3{std::cos(a1) * radius, 0.0f, std::sin(a1) * radius};

        scene_triangles->triangles.push_back({b0, t0, t1});
        scene_triangles->triangles.push_back({b0, t1, b1});
        scene_triangles->triangles.push_back({top, t1, t0});
        scene_triangles->triangles.push_back({bottom, b0, b1});
    }
}

struct PlayerNameTag {
    bool visible = false;
    char name[32]{};
    Vector3 pos{};
    Vector2 screen{};
    Vector2 size_px{};
};

static bool ray_hits_vertical_cylinder(Ray ray, Vector3 bottom, Vector3 top, float radius, float* out_t) {
    const float min_y = fminf(bottom.y, top.y);
    const float max_y = fmaxf(bottom.y, top.y);
    const float ox = ray.position.x - bottom.x;
    const float oz = ray.position.z - bottom.z;
    const float dx = ray.direction.x;
    const float dz = ray.direction.z;
    const float radius_sq = radius * radius;

    float best_t = std::numeric_limits<float>::max();
    const float a = dx * dx + dz * dz;
    const float b = 2.0f * (ox * dx + oz * dz);
    const float c = ox * ox + oz * oz - radius_sq;
    if (a > 0.000001f) {
        const float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            const float root = std::sqrt(disc);
            const float inv = 0.5f / a;
            const float candidates[2] = {(-b - root) * inv, (-b + root) * inv};
            for (float t : candidates) {
                const float y = ray.position.y + ray.direction.y * t;
                if (t >= 0.0f && y >= min_y && y <= max_y && t < best_t) best_t = t;
            }
        }
    }

    if (std::fabs(ray.direction.y) > 0.000001f) {
        const float cap_y[2] = {min_y, max_y};
        for (float y_plane : cap_y) {
            const float t = (y_plane - ray.position.y) / ray.direction.y;
            const float x = ray.position.x + ray.direction.x * t - bottom.x;
            const float z = ray.position.z + ray.direction.z * t - bottom.z;
            if (t >= 0.0f && x * x + z * z <= radius_sq && t < best_t) best_t = t;
        }
    }

    if (best_t == std::numeric_limits<float>::max()) return false;
    if (out_t) *out_t = best_t;
    return true;
}

static void draw_name_tag_billboard(RenderState* renderer, const Camera3D& camera, PlayerNameTag* tag) {
    if (!tag || !tag->visible || !renderer->white_ready) return;

    constexpr float text_size = 18.0f;
    const Vector2 text = measure_engine_text(renderer, tag->name, text_size);
    tag->size_px = {text.x + 22.0f, text.y + 12.0f};
    tag->screen = GetWorldToScreenEx(tag->pos, camera, renderer->native_w, renderer->native_h);

    const Vector3 forward = safe_norm(camera.target - camera.position, {0.0f, 0.0f, -1.0f});
    const float dist = Vector3DotProduct(tag->pos - camera.position, forward);
    if (dist <= 0.01f) {
        tag->visible = false;
        return;
    }

    const float world_per_pixel = 2.0f * dist * std::tan(camera.fovy * DEG2RAD * 0.5f) / static_cast<float>(renderer->native_h);
    const Vector2 world_size = {tag->size_px.x * world_per_pixel, tag->size_px.y * world_per_pixel};
    Rectangle source = {0.0f, 0.0f, 1.0f, 1.0f};
    DrawBillboardPro(camera, renderer->white_texture, source, tag->pos, camera.up, world_size, world_size * 0.5f, 0.0f, Color{0, 0, 0, 205});
}

static void draw_name_tag_text(RenderState* renderer, const PlayerNameTag& tag) {
    if (!tag.visible) return;
    constexpr float text_size = 18.0f;
    const Vector2 text = measure_engine_text(renderer, tag.name, text_size);
    const Vector2 pos = {tag.screen.x - text.x * 0.5f, tag.screen.y - text.y * 0.5f - 1.0f};
    draw_engine_text(renderer, tag.name, pos, text_size, WHITE);
}

static void draw_players(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, ScreenEdgeList* edge_list, SceneTriangleList* scene_triangles, PlayerNameTag* hovered_tag) {
    const Ray hover_ray = {camera.position, safe_norm(camera.target - camera.position, {0.0f, 0.0f, -1.0f})};
    float closest_hover_t = std::numeric_limits<float>::max();

    for (u32 slot = 0; slot < dim->players.count; ++slot) {
        const u32 player_id = arena_id_at_slot(&dim->players, slot);
        const PlayerEntity* player = &dim->players.data[slot];
        if (player_id == local_player_id || !player->connected) continue;
        const WorldPos feet = player_feet_pos(dim, player);
        if (!id_valid(feet.dimension)) continue;

        const float radius = fmaxf(0.01f, player->body_radius);
        const float height = fmaxf(radius * 2.0f, player->current_height);
        const Vector3 bottom = world_delta_meters(feet, view.anchor, dim->chunk_size_m);
        const Vector3 top = world_delta_meters(worldpos_offset(feet, {0.0f, height, 0.0f}, dim->chunk_size_m), view.anchor, dim->chunk_size_m);

        DrawCylinderEx(bottom, top, radius, radius, 32, player->color);
        push_player_cylinder_triangles(scene_triangles, bottom, top, radius);
        push_player_ring_edges(renderer, camera, view_matrix, projection_matrix, edge_list, bottom, radius, BLACK);
        push_player_ring_edges(renderer, camera, view_matrix, projection_matrix, edge_list, top, radius, BLACK);

        float hover_t = 0.0f;
        if (ray_hits_vertical_cylinder(hover_ray, bottom, top, radius, &hover_t) && hover_t < closest_hover_t) {
            closest_hover_t = hover_t;
            if (hovered_tag) {
                hovered_tag->visible = true;
                std::snprintf(hovered_tag->name, sizeof(hovered_tag->name), "%s", player->name[0] ? player->name : "player");
                hovered_tag->pos = top + Vector3{0.0f, 0.30f, 0.0f};
            }
        }
    }

    draw_name_tag_billboard(renderer, camera, hovered_tag);
}

static void draw_player_aim_rays(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id, const Camera3D& camera) {
    if (!dim) return;

    const bool use_shader = renderer->sprite_alpha_shader_ready;
    for (u32 slot = 0; slot < dim->players.count; ++slot) {
        const u32 player_id = arena_id_at_slot(&dim->players, slot);
        const PlayerEntity* player = &dim->players.data[slot];
        if (player_id == local_player_id || !player->connected || !player->aim_ray_active) continue;

        Color color = player->color;
        color.a = 235;
        const Vector3 start = world_delta_meters(player->aim_ray_start, view.anchor, dim->chunk_size_m);
        const Vector3 end = world_delta_meters(player->aim_ray_end, view.anchor, dim->chunk_size_m);
        if (Vector3LengthSqr(end - start) < 0.0001f) continue;

        DrawLine3D(start, end, color);

        SpriteInstance marker{};
        marker.size = {0.32f, 0.32f};
        marker.texture_id = render_texture_cross;
        marker.color = color;
        marker.billboard = billboard_full_3d;
        const Texture2D texture = sprite_texture(renderer, marker.texture_id);
        const Rectangle source = {0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)};
        const SpriteQuad quad = make_sprite_quad(&marker, end, camera);
        if (use_shader) BeginShaderMode(renderer->sprite_alpha_shader);
        else rlDisableDepthMask();
        draw_textured_sprite_quad(texture, source, quad, marker.color);
        if (use_shader) EndShaderMode();
        else rlEnableDepthMask();
    }
}

static void draw_physics(RenderState* renderer, Dimension* dim, const CameraView& view) {
    if (!renderer->draw_physics_debug) return;

    for (u32 slot = 0; slot < dim->physics.boxes.count; ++slot) {
        const BoxCollider* box = &dim->physics.boxes.data[slot];
        const Vector3 rel = world_delta_meters(box->pos, view.anchor, dim->chunk_size_m);
        Color c = box->color;
        c.a = 80;
        DrawCubeV(rel, box->half * 2.0f, c);
        DrawCubeWiresV(rel, box->half * 2.0f, Fade(BLACK, 0.45f));
    }

    for (u32 slot = 0; slot < dim->lights.count; ++slot) {
        const LightSource* light = &dim->lights.data[slot];
        const Vector3 rel = world_delta_meters(light->pos, view.anchor, dim->chunk_size_m);
        DrawSphere(rel, 0.12f, light->color);
    }
}

static void draw_crosshair(RenderState* renderer) {
    const Vector2 center = {static_cast<float>(renderer->native_w) * 0.5f, static_cast<float>(renderer->native_h) * 0.5f};
    DrawCircleV(center, 3.0f, Color{0, 0, 0, 135});
    DrawCircleV(center, 1.5f, Color{255, 255, 255, 230});
}

void renderer_render_dimension_to_target(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id) {
    if (!renderer->target_ready) renderer_ensure_target(renderer);

    Camera3D camera{};
    camera.position = {0.0f, view.eye_height, 0.0f};
    camera.target = camera.position + forward_from_angles(view.yaw, view.pitch);
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = renderer->fov;
    camera.projection = CAMERA_PERSPECTIVE;
    const Matrix view_matrix = MatrixLookAt(camera.position, camera.target, camera.up);
    const Matrix projection_matrix = MatrixPerspective(
        camera.fovy * DEG2RAD,
        static_cast<double>(renderer->native_w) / static_cast<double>(renderer->native_h),
        rlGetCullDistanceNear(),
        rlGetCullDistanceFar()
    );

    BeginTextureMode(renderer->target);
    ClearBackground(dim->sky_top);
    DrawRectangleGradientV(0, 0, renderer->native_w, renderer->native_h, dim->sky_top, dim->sky_bottom);
    BeginMode3D(camera);

    ScreenEdgeList edge_list{};
    SceneTriangleList scene_triangles{};
    for (u32 chunk_slot = 0; chunk_slot < dim->chunks.count; ++chunk_slot) {
        const Chunk* chunk = &dim->chunks.data[chunk_slot];
        float chunk_dist = 0.0f;
        if (!chunk_visible(dim, view.anchor.chunk, chunk->coord, &chunk_dist)) continue;

        for (u32 i = 0; i < chunk->mesh_count; ++i) {
            draw_mesh_instance(renderer, dim, view, camera, view_matrix, projection_matrix, &edge_list, &scene_triangles, arena_get(&dim->meshes, chunk->meshes[i]), chunk_dist);
        }
    }

    SpriteDepthOcclusion sprite_occlusion{};
    if (renderer->depth_test_edges) {
        sprite_occlusion.scene = read_current_depth_buffer(renderer->native_w, renderer->native_h);
    }

    for (u32 chunk_slot = 0; chunk_slot < dim->chunks.count; ++chunk_slot) {
        const Chunk* chunk = &dim->chunks.data[chunk_slot];
        float chunk_dist = 0.0f;
        if (!chunk_visible(dim, view.anchor.chunk, chunk->coord, &chunk_dist)) continue;

        draw_sprites(renderer, dim, view, camera, chunk);
    }

    if (renderer->depth_test_edges) {
        sprite_occlusion.after_sprites = read_current_depth_buffer(renderer->native_w, renderer->native_h);
    }

    PlayerNameTag hovered_tag{};
    draw_players(renderer, dim, view, local_player_id, camera, view_matrix, projection_matrix, &edge_list, &scene_triangles, &hovered_tag);
    draw_player_aim_rays(renderer, dim, view, local_player_id, camera);
    draw_physics(renderer, dim, view);

    EndMode3D();

    if (renderer->depth_test_edges) draw_depth_cut_screen_edges(renderer, &edge_list, sprite_occlusion, scene_triangles, camera.position);
    else draw_flat_screen_edges(&edge_list);
    draw_name_tag_text(renderer, hovered_tag);
    draw_crosshair(renderer);

    //char overlay[96]{};
    //std::snprintf(overlay, sizeof(overlay), "scale 1/%d | native %dx%d", 1 << renderer->scale_power, renderer->native_w, renderer->native_h);
    //draw_engine_text(renderer, overlay, {10.0f, 10.0f}, 16.0f, Fade(BLACK, 0.72f));
    EndTextureMode();
}

void renderer_draw_dimension(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id) {
    renderer_render_dimension_to_target(renderer, dim, view, local_player_id);
    BeginDrawing();
    renderer_draw_target_to_screen(renderer);
    EndDrawing();
}

} // namespace ol
