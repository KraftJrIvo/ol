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
#if defined(GRAPHICS_API_OPENGL_33) || defined(GRAPHICS_API_OPENGL_43)
#include "external/glad.h"
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#endif

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
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

static std::vector<u8> texture_alpha_mask(Texture2D texture) {
    std::vector<u8> result{};
    if (!IsTextureValid(texture)) return result;
    Image image = LoadImageFromTexture(texture);
    if (!IsImageValid(image)) return result;
    result.resize(static_cast<size_t>(image.width) * static_cast<size_t>(image.height), 255);
    Color* colors = LoadImageColors(image);
    if (colors) {
        for (size_t i = 0; i < result.size(); ++i) result[i] = colors[i].a;
        UnloadImageColors(colors);
    }
    UnloadImage(image);
    return result;
}

static Texture2D make_world_texture(u32 texture_id) {
    constexpr int size = 16;
    Image image = GenImageColor(size, size, WHITE);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            Color color = WHITE;
            if (texture_id == render_texture_grid) {
                color = (x == 0 || x == size - 1 || y == 0 || y == size - 1)
                    ? Color{72, 100, 76, 255}
                    : Color{176, 210, 174, 255};
            } else if (texture_id == render_texture_grass) {
                const u32 noise = static_cast<u32>(x * 37 + y * 73 + x * y * 11);
                const u8 value = static_cast<u8>(175 + noise % 55);
                color = {static_cast<u8>(value - 32), value, static_cast<u8>(value - 48), 255};
            } else if (texture_id == render_texture_stone) {
                const bool mortar = y == 0 || (x + ((y / 4) & 1) * 8) % 16 == 0;
                color = mortar ? Color{108, 108, 112, 255} : Color{205, 202, 194, 255};
            } else if (texture_id == render_texture_roof) {
                const bool seam = y % 4 == 0 || (x + ((y / 4) & 1) * 4) % 8 == 0;
                color = seam ? Color{104, 78, 82, 255} : Color{205, 154, 146, 255};
            }
            ImageDrawPixel(&image, x, y, color);
        }
    }
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (IsTextureValid(texture)) {
        GenTextureMipmaps(&texture);
        SetTextureFilter(texture, TEXTURE_FILTER_ANISOTROPIC_8X);
        SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    }
    return texture;
}

static Shader load_edge_depth_shader(RenderState* renderer, bool* out_ready) {
    if (out_ready) *out_ready = false;
    if (renderer) {
        renderer->edge_depth_texture_loc = -1;
        renderer->edge_depth_bias_loc = -1;
    }

    static const char* vs =
        "#version 330\n"
        "in vec3 vertexPosition;\n"
        "in vec2 vertexTexCoord;\n"
        "in vec4 vertexColor;\n"
        "uniform mat4 mvp;\n"
        "uniform float depthBias;\n"
        "out vec4 fragColor;\n"
        "out vec2 fragCenterScreen;\n"
        "out float fragEdgeDepth;\n"
        "void main() {\n"
        "    fragColor = vertexColor;\n"
        "    fragCenterScreen = vertexTexCoord;\n"
        "    fragEdgeDepth = vertexPosition.z;\n"
        "    vec4 pos = mvp * vec4(vertexPosition.xy, 0.0, 1.0);\n"
        "    pos.z = max(vertexPosition.z - depthBias * 0.5, 0.0) * 2.0 - 1.0;\n"
        "    gl_Position = pos;\n"
        "}\n";

    static const char* fs =
        "#version 330\n"
        "in vec4 fragColor;\n"
        "in vec2 fragCenterScreen;\n"
        "in float fragEdgeDepth;\n"
        "uniform sampler2D depthTexture;\n"
        "uniform float depthBias;\n"
        "out vec4 finalColor;\n"
        "void main() {\n"
        "    ivec2 size = textureSize(depthTexture, 0);\n"
        "    vec2 depthPixel = vec2(fragCenterScreen.x, float(size.y - 1) - fragCenterScreen.y);\n"
        "    ivec2 center = ivec2(clamp(depthPixel, vec2(0.0), vec2(size - ivec2(1))));\n"
        "    float centerDepth = texelFetch(depthTexture, center, 0).r;\n"
        "    vec2 tangent = length(dFdx(fragCenterScreen)) > length(dFdy(fragCenterScreen)) ? dFdx(fragCenterScreen) : dFdy(fragCenterScreen);\n"
        "    tangent = length(tangent) > 0.0001 ? normalize(vec2(tangent.x, -tangent.y)) : vec2(1.0, 0.0);\n"
        "    float sceneDepth = centerDepth;\n"
        "    for (int i = -32; i <= 32; ++i) {\n"
        "        vec2 samplePixel = depthPixel + tangent * float(i);\n"
        "        ivec2 p = ivec2(clamp(samplePixel, vec2(0.0), vec2(size - ivec2(1))));\n"
        "        sceneDepth = max(sceneDepth, texelFetch(depthTexture, p, 0).r);\n"
        "    }\n"
        "    if (fragEdgeDepth > sceneDepth + depthBias) discard;\n"
        "    finalColor = fragColor;\n"
        "}\n";

    Shader shader = LoadShaderFromMemory(vs, fs);
    if (IsShaderValid(shader)) {
        if (renderer) {
            renderer->edge_depth_texture_loc = GetShaderLocation(shader, "depthTexture");
            renderer->edge_depth_bias_loc = GetShaderLocation(shader, "depthBias");
        }
        if (out_ready) *out_ready = true;
    }
    return shader;
}

static u32 load_edge_filter_compute_program(RenderState* renderer, bool* out_ready) {
    if (out_ready) *out_ready = false;
    if (renderer) {
        renderer->edge_filter_pass_id_loc = -1;
        renderer->edge_filter_job_count_loc = -1;
        renderer->edge_filter_edge_count_loc = -1;
        renderer->edge_filter_triangle_count_loc = -1;
        renderer->edge_filter_bin_count_loc = -1;
        renderer->edge_filter_unbounded_count_loc = -1;
        renderer->edge_filter_screen_size_loc = -1;
        renderer->edge_filter_camera_pos_loc = -1;
        renderer->edge_filter_brute_force_loc = -1;
        renderer->edge_filter_scene_depth_texture_loc = -1;
        renderer->edge_filter_after_sprites_depth_texture_loc = -1;
    }

    static const char* cs =
        "#version 430\n"
        "layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;\n"
        "#define SCENE_GRID_COLS 32\n"
        "#define SCENE_GRID_ROWS 18\n"
        "struct EdgeIn { vec4 a; vec4 b; vec4 relA; vec4 relB; };\n"
        "struct TriIn { vec4 aValid; vec4 e1; vec4 e2; vec4 bounds; };\n"
        "struct RefineJob { uvec4 data; vec4 range; };\n"
        "layout(std430, binding = 1) readonly buffer EdgeBuffer { EdgeIn edges[]; } edgeIn;\n"
        "layout(std430, binding = 2) readonly buffer TriangleBuffer { TriIn triangles[]; } triIn;\n"
        "layout(std430, binding = 3) readonly buffer BinRangeBuffer { uvec4 binRanges[]; } binRangeIn;\n"
        "layout(std430, binding = 4) readonly buffer BinIndexBuffer { uint binIndices[]; } binIndexIn;\n"
        "layout(std430, binding = 5) readonly buffer UnboundedBuffer { uint unboundedIndices[]; } unboundedIn;\n"
        "layout(std430, binding = 6) readonly buffer SampleJobBuffer { uvec4 sampleJobs[]; } sampleJobIn;\n"
        "layout(std430, binding = 7) writeonly buffer SampleResultBuffer { uint sampleResults[]; } sampleResultOut;\n"
        "layout(std430, binding = 8) readonly buffer RefineJobBuffer { RefineJob refineJobs[]; } refineJobIn;\n"
        "layout(std430, binding = 9) writeonly buffer RefineResultBuffer { float refineResults[]; } refineResultOut;\n"
        "layout(std430, binding = 10) buffer StatsBuffer { uint stats[]; } statsOut;\n"
        "uniform int passId;\n"
        "uniform int jobCount;\n"
        "uniform int edgeCount;\n"
        "uniform int triangleCount;\n"
        "uniform int binCount;\n"
        "uniform int unboundedCount;\n"
        "uniform ivec2 screenSize;\n"
        "uniform vec3 cameraPos;\n"
        "uniform int bruteForceEdgeOcclusion;\n"
        "uniform sampler2D sceneDepthTexture;\n"
        "uniform sampler2D afterSpritesDepthTexture;\n"
        "float lerpFloat(float a, float b, float t) { return a + (b - a) * t; }\n"
        "vec2 edgePointAt(EdgeIn edge, float t) { return edge.a.xy + (edge.b.xy - edge.a.xy) * t; }\n"
        "float edgeDepthAt(EdgeIn edge, float t) { return lerpFloat(edge.a.z, edge.b.z, t); }\n"
        "vec3 perspectiveLerpRel(vec3 a, vec3 b, float invWa, float invWb, float t) {\n"
        "    float wa = invWa * (1.0 - t);\n"
        "    float wb = invWb * t;\n"
        "    float denom = wa + wb;\n"
        "    if (abs(denom) < 0.000001) return a + (b - a) * t;\n"
        "    return (a * wa + b * wb) / denom;\n"
        "}\n"
        "vec3 edgeRelPointAt(EdgeIn edge, float t) { return perspectiveLerpRel(edge.relA.xyz, edge.relB.xyz, edge.a.w, edge.b.w, t); }\n"
        "int sceneGridColForX(float x) {\n"
        "    if (screenSize.x <= 0) return 0;\n"
        "    return clamp(int(floor(x * float(SCENE_GRID_COLS) / float(screenSize.x))), 0, SCENE_GRID_COLS - 1);\n"
        "}\n"
        "int sceneGridRowForY(float y) {\n"
        "    if (screenSize.y <= 0) return 0;\n"
        "    return clamp(int(floor(y * float(SCENE_GRID_ROWS) / float(screenSize.y))), 0, SCENE_GRID_ROWS - 1);\n"
        "}\n"
        "int sceneGridIndex(int col, int row) { return row * SCENE_GRID_COLS + col; }\n"
        "bool screenBboxesOverlap(vec2 aMin, vec2 aMax, vec2 bMin, vec2 bMax) {\n"
        "    return aMax.x >= bMin.x && aMin.x <= bMax.x && aMax.y >= bMin.y && aMin.y <= bMax.y;\n"
        "}\n"
        "bool rayIntersectsTriangle(vec3 origin, vec3 dir, TriIn tri, float maxDist) {\n"
        "    const float eps = 0.00001;\n"
        "    vec3 h = cross(dir, tri.e2.xyz);\n"
        "    float det = dot(tri.e1.xyz, h);\n"
        "    if (abs(det) < eps) return false;\n"
        "    float invDet = 1.0 / det;\n"
        "    vec3 s = origin - tri.aValid.xyz;\n"
        "    float u = invDet * dot(s, h);\n"
        "    if (u < -eps || u > 1.0 + eps) return false;\n"
        "    vec3 q = cross(s, tri.e1.xyz);\n"
        "    float v = invDet * dot(dir, q);\n"
        "    if (v < -eps || u + v > 1.0 + eps) return false;\n"
        "    float dist = invDet * dot(tri.e2.xyz, q);\n"
        "    return dist > 0.002 && dist < maxDist - 0.035;\n"
        "}\n"
        "bool triangleBlocks(uint triIndex, bool filterBySample, vec2 sampleMin, vec2 sampleMax, vec3 dir, float dist) {\n"
        "    if (triIndex >= uint(triangleCount)) return false;\n"
        "    TriIn tri = triIn.triangles[triIndex];\n"
        "    bool boundsValid = tri.aValid.w > 0.5;\n"
        "    if (filterBySample && boundsValid && !screenBboxesOverlap(sampleMin, sampleMax, tri.bounds.xy, tri.bounds.zw)) return false;\n"
        "    atomicAdd(statsOut.stats[1], 1u);\n"
        "    atomicAdd(statsOut.stats[2], 1u);\n"
        "    return rayIntersectsTriangle(cameraPos, dir, tri, dist);\n"
        "}\n"
        "bool scenePointVisible(EdgeIn edge, vec2 p, vec3 point) {\n"
        "    if (edge.relB.w <= 0.5 || triangleCount <= 0) return true;\n"
        "    vec3 delta = point - cameraPos;\n"
        "    float dist = length(delta);\n"
        "    if (dist <= 0.001) return true;\n"
        "    vec3 dir = delta / dist;\n"
        "    vec2 sampleMin = p - vec2(2.0);\n"
        "    vec2 sampleMax = p + vec2(2.0);\n"
        "    bool bruteForce = bruteForceEdgeOcclusion != 0;\n"
        "    bool filterBySample = !bruteForce;\n"
        "    if (bruteForce) {\n"
        "        for (uint triIndex = 0u; triIndex < uint(triangleCount); ++triIndex) {\n"
        "            if (triangleBlocks(triIndex, false, sampleMin, sampleMax, dir, dist)) return false;\n"
        "        }\n"
        "        return true;\n"
        "    }\n"
        "    for (uint i = 0u; i < uint(unboundedCount); ++i) {\n"
        "        if (triangleBlocks(unboundedIn.unboundedIndices[i], filterBySample, sampleMin, sampleMax, dir, dist)) return false;\n"
        "    }\n"
        "    int minCol = sceneGridColForX(p.x - 2.0);\n"
        "    int maxCol = sceneGridColForX(p.x + 2.0);\n"
        "    int minRow = sceneGridRowForY(p.y - 2.0);\n"
        "    int maxRow = sceneGridRowForY(p.y + 2.0);\n"
        "    for (int row = minRow; row <= maxRow; ++row) {\n"
        "        for (int col = minCol; col <= maxCol; ++col) {\n"
        "            int bin = sceneGridIndex(col, row);\n"
        "            if (bin < 0 || bin >= binCount) continue;\n"
        "            uvec4 range = binRangeIn.binRanges[bin];\n"
        "            for (uint i = 0u; i < range.y; ++i) {\n"
        "                uint indexOffset = range.x + i;\n"
        "                if (triangleBlocks(binIndexIn.binIndices[indexOffset], filterBySample, sampleMin, sampleMax, dir, dist)) return false;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "bool spriteOcclusionVisible(vec2 p, float edgeDepth) {\n"
        "    ivec2 center = ivec2(floor(p + vec2(0.5)));\n"
        "    if (center.x < 0 || center.x >= screenSize.x || center.y < 0 || center.y >= screenSize.y) return true;\n"
        "    int glY = screenSize.y - 1 - center.y;\n"
        "    float sceneDepth = texelFetch(sceneDepthTexture, ivec2(center.x, glY), 0).r;\n"
        "    float spriteDepth = texelFetch(afterSpritesDepthTexture, ivec2(center.x, glY), 0).r;\n"
        "    const float edgeDepthBias = 0.00035;\n"
        "    const float spriteDepthDelta = 0.00035;\n"
        "    if (spriteDepth >= sceneDepth - spriteDepthDelta) return true;\n"
        "    return edgeDepth <= spriteDepth + edgeDepthBias;\n"
        "}\n"
        "bool edgeSampleVisible(EdgeIn edge, float t) {\n"
        "    atomicAdd(statsOut.stats[0], 1u);\n"
        "    vec2 p = edgePointAt(edge, t);\n"
        "    if (!scenePointVisible(edge, p, edgeRelPointAt(edge, t))) return false;\n"
        "    return spriteOcclusionVisible(p, edgeDepthAt(edge, t));\n"
        "}\n"
        "float refineVisibilityBoundary(EdgeIn edge, float t0, bool visible0, float t1) {\n"
        "    float lo = t0;\n"
        "    float hi = t1;\n"
        "    for (int i = 0; i < 8; ++i) {\n"
        "        float mid = (lo + hi) * 0.5;\n"
        "        if (edgeSampleVisible(edge, mid) == visible0) lo = mid;\n"
        "        else hi = mid;\n"
        "    }\n"
        "    return (lo + hi) * 0.5;\n"
        "}\n"
        "void main() {\n"
        "    uint jobIndex = gl_GlobalInvocationID.x;\n"
        "    if (jobIndex >= uint(jobCount)) return;\n"
        "    if (passId == 0) {\n"
        "        uvec4 job = sampleJobIn.sampleJobs[jobIndex];\n"
        "        uint edgeIndex = job.x;\n"
        "        if (edgeIndex >= uint(edgeCount)) {\n"
        "            sampleResultOut.sampleResults[jobIndex] = 0u;\n"
        "            return;\n"
        "        }\n"
        "        EdgeIn edge = edgeIn.edges[edgeIndex];\n"
        "        uint sampleIndex = job.y;\n"
        "        uint sampleCount = max(job.z, 1u);\n"
        "        float t = float(sampleIndex) / float(sampleCount);\n"
        "        sampleResultOut.sampleResults[jobIndex] = edgeSampleVisible(edge, t) ? 1u : 0u;\n"
        "        return;\n"
        "    }\n"
        "    RefineJob job = refineJobIn.refineJobs[jobIndex];\n"
        "    uint edgeIndex = job.data.x;\n"
        "    if (edgeIndex >= uint(edgeCount)) {\n"
        "        refineResultOut.refineResults[jobIndex] = 0.0;\n"
        "        return;\n"
        "    }\n"
        "    EdgeIn edge = edgeIn.edges[edgeIndex];\n"
        "    bool visible0 = job.data.y != 0u;\n"
        "    refineResultOut.refineResults[jobIndex] = refineVisibilityBoundary(edge, job.range.x, visible0, job.range.y);\n"
        "}\n";

    const u32 shader = rlLoadShader(cs, RL_COMPUTE_SHADER);
    const u32 program = shader ? rlLoadShaderProgramCompute(shader) : 0;
    if (shader) rlUnloadShader(shader);
    if (!program) return 0;

    if (renderer) {
        Shader compute_shader{program, nullptr};
        renderer->edge_filter_pass_id_loc = GetShaderLocation(compute_shader, "passId");
        renderer->edge_filter_job_count_loc = GetShaderLocation(compute_shader, "jobCount");
        renderer->edge_filter_edge_count_loc = GetShaderLocation(compute_shader, "edgeCount");
        renderer->edge_filter_triangle_count_loc = GetShaderLocation(compute_shader, "triangleCount");
        renderer->edge_filter_bin_count_loc = GetShaderLocation(compute_shader, "binCount");
        renderer->edge_filter_unbounded_count_loc = GetShaderLocation(compute_shader, "unboundedCount");
        renderer->edge_filter_screen_size_loc = GetShaderLocation(compute_shader, "screenSize");
        renderer->edge_filter_camera_pos_loc = GetShaderLocation(compute_shader, "cameraPos");
        renderer->edge_filter_brute_force_loc = GetShaderLocation(compute_shader, "bruteForceEdgeOcclusion");
        renderer->edge_filter_scene_depth_texture_loc = GetShaderLocation(compute_shader, "sceneDepthTexture");
        renderer->edge_filter_after_sprites_depth_texture_loc = GetShaderLocation(compute_shader, "afterSpritesDepthTexture");
    }
    if (out_ready) *out_ready = true;
    return program;
}

static Texture2D load_edge_depth_texture(int width, int height, bool* out_ready) {
    Texture2D texture{};
    if (out_ready) *out_ready = false;
    if (width <= 0 || height <= 0) return texture;

    glGenTextures(1, &texture.id);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    texture.width = width;
    texture.height = height;
    texture.format = 19;
    texture.mipmaps = 1;
    if (out_ready) *out_ready = texture.id != 0 && glIsTexture(texture.id);
    return texture;
}

static void unload_edge_depth_textures(RenderState* renderer) {
    if (!renderer) return;
    if (renderer->edge_depth_texture_ready) {
        UnloadTexture(renderer->edge_depth_texture);
        renderer->edge_depth_texture = {};
        renderer->edge_depth_texture_ready = false;
    }
    if (renderer->edge_scene_depth_texture_ready) {
        UnloadTexture(renderer->edge_scene_depth_texture);
        renderer->edge_scene_depth_texture = {};
        renderer->edge_scene_depth_texture_ready = false;
    }
}

static void unload_edge_filter_buffer(u32* id, u32* capacity) {
    if (id && *id) {
        rlUnloadShaderBuffer(*id);
        *id = 0;
    }
    if (capacity) *capacity = 0;
}

static void unload_edge_filter_buffers(RenderState* renderer) {
    if (!renderer) return;
    unload_edge_filter_buffer(&renderer->edge_filter_edges_ssbo, &renderer->edge_filter_edges_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_triangles_ssbo, &renderer->edge_filter_triangles_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_bin_ranges_ssbo, &renderer->edge_filter_bin_ranges_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_bin_indices_ssbo, &renderer->edge_filter_bin_indices_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_unbounded_ssbo, &renderer->edge_filter_unbounded_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_sample_jobs_ssbo, &renderer->edge_filter_sample_jobs_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_sample_results_ssbo, &renderer->edge_filter_sample_results_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_refine_jobs_ssbo, &renderer->edge_filter_refine_jobs_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_refine_results_ssbo, &renderer->edge_filter_refine_results_capacity);
    unload_edge_filter_buffer(&renderer->edge_filter_stats_ssbo, &renderer->edge_filter_stats_capacity);
}

void renderer_init(RenderState* renderer) {
    renderer->white_texture = make_white_texture();
    renderer->white_ready = true;
    renderer->grid_texture = make_world_texture(render_texture_grid);
    renderer->grass_texture = make_world_texture(render_texture_grass);
    renderer->stone_texture = make_world_texture(render_texture_stone);
    renderer->roof_texture = make_world_texture(render_texture_roof);
    renderer->world_textures_ready = IsTextureValid(renderer->grid_texture) && IsTextureValid(renderer->grass_texture) &&
        IsTextureValid(renderer->stone_texture) && IsTextureValid(renderer->roof_texture);
    renderer->life_texture = load_texture_from_memory(".png", res_life_png, res_life_png_len, &renderer->life_ready);
    renderer->cross_texture = load_texture_from_memory(".png", res_cross_png, res_cross_png_len, &renderer->cross_ready);
    renderer->life_alpha = texture_alpha_mask(renderer->life_texture);
    renderer->cross_alpha = texture_alpha_mask(renderer->cross_texture);
    renderer->sprite_alpha_shader = load_sprite_alpha_shader(&renderer->sprite_alpha_shader_ready);
    renderer->edge_depth_shader = load_edge_depth_shader(renderer, &renderer->edge_depth_shader_ready);
    renderer->edge_filter_compute_program = load_edge_filter_compute_program(renderer, &renderer->edge_filter_compute_ready);

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
    for (SpritePaintTextureCache& cache : renderer->sprite_paint_textures) {
        if (IsTextureValid(cache.texture)) UnloadTexture(cache.texture);
    }
    renderer->sprite_paint_textures.clear();
    for (MeshPaintSurfaceCache& cache : renderer->mesh_paint_surfaces) {
        if (IsTextureValid(cache.texture)) UnloadTexture(cache.texture);
    }
    renderer->mesh_paint_surfaces.clear();
    renderer->life_alpha.clear();
    renderer->cross_alpha.clear();
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
    if (IsTextureValid(renderer->grid_texture)) UnloadTexture(renderer->grid_texture);
    if (IsTextureValid(renderer->grass_texture)) UnloadTexture(renderer->grass_texture);
    if (IsTextureValid(renderer->stone_texture)) UnloadTexture(renderer->stone_texture);
    if (IsTextureValid(renderer->roof_texture)) UnloadTexture(renderer->roof_texture);
    renderer->world_textures_ready = false;
    unload_edge_depth_textures(renderer);
    unload_edge_filter_buffers(renderer);
    if (renderer->sprite_alpha_shader_ready) {
        UnloadShader(renderer->sprite_alpha_shader);
        renderer->sprite_alpha_shader_ready = false;
    }
    if (renderer->edge_depth_shader_ready) {
        UnloadShader(renderer->edge_depth_shader);
        renderer->edge_depth_shader_ready = false;
    }
    if (renderer->edge_filter_compute_ready) {
        rlUnloadShaderProgram(renderer->edge_filter_compute_program);
        renderer->edge_filter_compute_program = 0;
        renderer->edge_filter_compute_ready = false;
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
    unload_edge_depth_textures(renderer);

    renderer->target = LoadRenderTexture(native_w, native_h);
    SetTextureFilter(renderer->target.texture, TEXTURE_FILTER_POINT);
    renderer->edge_depth_texture = load_edge_depth_texture(native_w, native_h, &renderer->edge_depth_texture_ready);
    renderer->edge_scene_depth_texture = load_edge_depth_texture(native_w, native_h, &renderer->edge_scene_depth_texture_ready);
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

constexpr float mesh_edge_render_radius_chunks = 5.0f;
constexpr float min_mesh_edge_fade_span_chunks = 1.0f;

static Color mix_color(Color a, Color b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    return {
        static_cast<unsigned char>(a.r + (b.r - a.r) * t),
        static_cast<unsigned char>(a.g + (b.g - a.g) * t),
        static_cast<unsigned char>(a.b + (b.b - a.b) * t),
        static_cast<unsigned char>(a.a + (b.a - a.a) * t),
    };
}

static Color color_with_alpha_factor(Color color, float alpha) {
    color.a = static_cast<unsigned char>(clampf(static_cast<float>(color.a) * alpha, 0.0f, 255.0f));
    return color;
}

static float mesh_edge_alpha(const Dimension* dim, float dist_chunks) {
    if (!dim) return 0.0f;
    const float edge_radius = fminf(static_cast<float>(dim->render_radius_chunks), mesh_edge_render_radius_chunks);
    if (edge_radius <= 0.0f || dist_chunks >= edge_radius) return 0.0f;

    float fade_start = fminf(static_cast<float>(dim->quality_render_radius_chunks), edge_radius);
    if (fade_start >= edge_radius) {
        fade_start = fmaxf(0.0f, edge_radius - min_mesh_edge_fade_span_chunks);
    }
    if (dist_chunks <= fade_start) return 1.0f;

    const float fade_span = fmaxf(edge_radius - fade_start, 0.001f);
    return 1.0f - clampf((dist_chunks - fade_start) / fade_span, 0.0f, 1.0f);
}

static bool mesh_edges_in_render_radius(const Dimension* dim, float dist_chunks) {
    if (!dim) return false;
    const float edge_radius = fminf(static_cast<float>(dim->render_radius_chunks), mesh_edge_render_radius_chunks);
    return dist_chunks < edge_radius;
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
    std::vector<ScreenEdge> edges{};
    u32 count = 0;
    u32 max_count = max_render_edges;
};

struct EdgeVisibleInterval {
    float start = 0.0f;
    float end = 0.0f;
};

constexpr size_t parallel_mesh_prep_min_jobs = 8192;

constexpr int scene_occlusion_grid_cols = 32;
constexpr int scene_occlusion_grid_rows = 18;
constexpr int scene_occlusion_grid_count = scene_occlusion_grid_cols * scene_occlusion_grid_rows;
constexpr u32 edge_filter_compute_local_size = 64;

struct SceneTriangle {
    Vector3 a{};
    Vector3 b{};
    Vector3 c{};
    Vector3 e1{};
    Vector3 e2{};
    Vector2 screen_min{};
    Vector2 screen_max{};
    bool screen_bounds_valid = false;
    u32 query_stamp = 0;
};

struct SceneTriangleList {
    std::vector<SceneTriangle> triangles{};
    std::array<std::vector<u32>, scene_occlusion_grid_count> bins{};
    std::vector<u32> unbounded{};
    int screen_w = 0;
    int screen_h = 0;
    u32 query_stamp = 1;
};

struct SceneCandidateTileCache {
    std::vector<const SceneTriangle*> candidates{};
    int min_col = -1;
    int max_col = -1;
    int min_row = -1;
    int max_row = -1;
    bool brute_force = false;
    bool valid = false;
};

struct ProjectedEdgePoint {
    Vector2 screen{};
    float depth = 0.0f;
    float inv_w = 1.0f;
};

struct GpuScreenEdge {
    Vector4 a{};
    Vector4 b{};
    Vector4 rel_a{};
    Vector4 rel_b{};
};

struct GpuSceneTriangle {
    Vector4 a_valid{};
    Vector4 e1{};
    Vector4 e2{};
    Vector4 bounds{};
};

struct GpuBinRange {
    u32 offset = 0;
    u32 count = 0;
    u32 pad0 = 0;
    u32 pad1 = 0;
};

struct GpuEdgeSampleJob {
    u32 edge_index = 0;
    u32 sample_index = 0;
    u32 sample_count = 1;
    u32 pad0 = 0;
};

struct GpuEdgeRefineJob {
    u32 edge_index = 0;
    u32 visible0 = 0;
    u32 pad0 = 0;
    u32 pad1 = 0;
    float t0 = 0.0f;
    float t1 = 0.0f;
    float pad2 = 0.0f;
    float pad3 = 0.0f;
};

struct GpuEdgeStats {
    u32 samples = 0;
    u32 candidates = 0;
    u32 ray_tests = 0;
    u32 overflow = 0;
};

static_assert(sizeof(GpuScreenEdge) == sizeof(float) * 16);
static_assert(sizeof(GpuSceneTriangle) == sizeof(float) * 16);
static_assert(sizeof(GpuBinRange) == sizeof(u32) * 4);
static_assert(sizeof(GpuEdgeSampleJob) == sizeof(u32) * 4);
static_assert(sizeof(GpuEdgeRefineJob) == sizeof(u32) * 4 + sizeof(float) * 4);
static_assert(sizeof(GpuEdgeStats) == sizeof(u32) * 4);

static void gather_point_scene_candidates(SceneTriangleList& scene, Vector2 point, bool brute_force, SceneCandidateTileCache* cache);
static bool screen_bboxes_overlap(Vector2 a_min, Vector2 a_max, Vector2 b_min, Vector2 b_max);

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

static bool clip_edge_to_near_plane(Vector3* a, Vector3* b, const Camera3D& camera, float near_plane) {
    const Vector3 forward = safe_norm(camera.target - camera.position, {0.0f, 0.0f, -1.0f});
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

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int scene_grid_col_for_x(float x, int width) {
    if (width <= 0) return 0;
    const int col = static_cast<int>(floorf(x * static_cast<float>(scene_occlusion_grid_cols) / static_cast<float>(width)));
    return clamp_int(col, 0, scene_occlusion_grid_cols - 1);
}

static int scene_grid_row_for_y(float y, int height) {
    if (height <= 0) return 0;
    const int row = static_cast<int>(floorf(y * static_cast<float>(scene_occlusion_grid_rows) / static_cast<float>(height)));
    return clamp_int(row, 0, scene_occlusion_grid_rows - 1);
}

static int scene_grid_index(int col, int row) {
    return row * scene_occlusion_grid_cols + col;
}

static void add_scene_triangle_to_grid(SceneTriangleList* scene, u32 tri_index) {
    if (!scene || tri_index >= scene->triangles.size()) return;
    const SceneTriangle& tri = scene->triangles[tri_index];
    if (!tri.screen_bounds_valid) {
        scene->unbounded.push_back(tri_index);
        return;
    }

    const int min_col = scene_grid_col_for_x(tri.screen_min.x, scene->screen_w);
    const int max_col = scene_grid_col_for_x(tri.screen_max.x, scene->screen_w);
    const int min_row = scene_grid_row_for_y(tri.screen_min.y, scene->screen_h);
    const int max_row = scene_grid_row_for_y(tri.screen_max.y, scene->screen_h);
    for (int row = min_row; row <= max_row; ++row) {
        for (int col = min_col; col <= max_col; ++col) {
            scene->bins[scene_grid_index(col, row)].push_back(tri_index);
        }
    }
}

enum SceneTriangleProjection {
    scene_triangle_skip,
    scene_triangle_bounded,
    scene_triangle_unbounded,
};

static SceneTriangleProjection clipped_triangle_screen_bounds(Vector3 a, Vector3 b, Vector3 c, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, int width, int height, float near_plane, Vector2* out_min, Vector2* out_max) {
    const Vector3 camera_forward = safe_norm(camera.target - camera.position, {0.0f, 0.0f, -1.0f});
    const Vector3 input[3] = {a, b, c};
    const float dist[3] = {
        Vector3DotProduct(a - camera.position, camera_forward) - near_plane,
        Vector3DotProduct(b - camera.position, camera_forward) - near_plane,
        Vector3DotProduct(c - camera.position, camera_forward) - near_plane,
    };

    Vector3 clipped[4]{};
    int clipped_count = 0;
    for (int i = 0; i < 3; ++i) {
        const int next = (i + 1) % 3;
        const bool current_in = dist[i] >= 0.0f;
        const bool next_in = dist[next] >= 0.0f;
        if (current_in && clipped_count < 4) {
            clipped[clipped_count++] = input[i];
        }
        if (current_in != next_in && clipped_count < 4) {
            const float denom = dist[i] - dist[next];
            if (std::fabs(denom) > 0.000001f) {
                const float t = dist[i] / denom;
                clipped[clipped_count++] = input[i] + (input[next] - input[i]) * t;
            }
        }
    }
    if (clipped_count < 3) return scene_triangle_skip;

    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = -std::numeric_limits<float>::max();
    float max_y = -std::numeric_limits<float>::max();
    for (int i = 0; i < clipped_count; ++i) {
        ProjectedEdgePoint p{};
        if (!project_edge_point(clipped[i], view_matrix, projection_matrix, width, height, &p)) {
            return scene_triangle_unbounded;
        }
        min_x = fminf(min_x, p.screen.x);
        min_y = fminf(min_y, p.screen.y);
        max_x = fmaxf(max_x, p.screen.x);
        max_y = fmaxf(max_y, p.screen.y);
    }

    constexpr float bbox_pad = 2.0f;
    min_x -= bbox_pad;
    min_y -= bbox_pad;
    max_x += bbox_pad;
    max_y += bbox_pad;
    if (max_x < 0.0f || max_y < 0.0f || min_x > static_cast<float>(width) || min_y > static_cast<float>(height)) {
        return scene_triangle_skip;
    }

    if (out_min) *out_min = {min_x, min_y};
    if (out_max) *out_max = {max_x, max_y};
    return scene_triangle_bounded;
}

static Vector3 perspective_lerp_rel(Vector3 a, Vector3 b, float inv_w_a, float inv_w_b, float t) {
    const float wa = inv_w_a * (1.0f - t);
    const float wb = inv_w_b * t;
    const float denom = wa + wb;
    if (std::fabs(denom) < 0.000001f) return a + (b - a) * t;
    return (a * wa + b * wb) / denom;
}

static void push_screen_edge(ScreenEdgeList* list, ProjectedEdgePoint a, ProjectedEdgePoint b, Vector3 rel_a, Vector3 rel_b, int width, int height, Color color, u8 thickness_px, bool use_scene_occlusion) {
    if (!list || list->count >= list->max_count) return;

    const Vector3 original_rel_a = rel_a;
    const Vector3 original_rel_b = rel_b;
    const float original_inv_w_a = a.inv_w;
    const float original_inv_w_b = b.inv_w;
    const float thickness = static_cast<float>(thickness_px ? thickness_px : 1);
    float clip_t0 = 0.0f;
    float clip_t1 = 1.0f;
    if (!clip_screen_edge(&a.screen, &b.screen, &a.depth, &b.depth, width, height, thickness, &clip_t0, &clip_t1)) return;
    if (Vector2Length(b.screen - a.screen) < 0.75f) return;
    a.inv_w = lerp_float(original_inv_w_a, original_inv_w_b, clip_t0);
    b.inv_w = lerp_float(original_inv_w_a, original_inv_w_b, clip_t1);
    const Vector3 clipped_rel_a = perspective_lerp_rel(original_rel_a, original_rel_b, original_inv_w_a, original_inv_w_b, clip_t0);
    const Vector3 clipped_rel_b = perspective_lerp_rel(original_rel_a, original_rel_b, original_inv_w_a, original_inv_w_b, clip_t1);
    list->edges.push_back({a.screen, b.screen, a.depth, b.depth, a.inv_w, b.inv_w, clipped_rel_a, clipped_rel_b, color, thickness, use_scene_occlusion});
    list->count = static_cast<u32>(list->edges.size());
}

static void draw_flat_screen_edges(const ScreenEdgeList* list) {
    if (!list || list->count == 0) return;
    for (u32 i = 0; i < list->count; ++i) {
        const ScreenEdge& edge = list->edges[i];
        DrawLineEx(edge.a, edge.b, edge.thickness, edge.color);
    }
}

static void copy_current_depth_to_texture(RenderState* renderer, const Texture2D& texture, bool ready) {
    if (!renderer || !ready || texture.id == 0) return;

    rlDrawRenderBatchActive();
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, renderer->native_w, renderer->native_h);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void draw_shader_depth_edge_quad(const ScreenEdge& edge) {
    const Vector2 delta = edge.b - edge.a;
    const float length = Vector2Length(delta);
    if (length <= 0.0f || edge.thickness <= 0.0f) return;

    const float scale = edge.thickness / (2.0f * length);
    const Vector2 radius = {-scale * delta.y, scale * delta.x};
    const Vector2 strip[4] = {
        edge.a - radius,
        edge.a + radius,
        edge.b - radius,
        edge.b + radius,
    };

    rlBegin(RL_TRIANGLES);
        rlColor4ub(edge.color.r, edge.color.g, edge.color.b, edge.color.a);

        rlTexCoord2f(edge.b.x, edge.b.y);
        rlVertex3f(strip[2].x, strip[2].y, edge.depth_b);
        rlTexCoord2f(edge.a.x, edge.a.y);
        rlVertex3f(strip[0].x, strip[0].y, edge.depth_a);
        rlTexCoord2f(edge.a.x, edge.a.y);
        rlVertex3f(strip[1].x, strip[1].y, edge.depth_a);

        rlTexCoord2f(edge.b.x, edge.b.y);
        rlVertex3f(strip[3].x, strip[3].y, edge.depth_b);
        rlTexCoord2f(edge.b.x, edge.b.y);
        rlVertex3f(strip[2].x, strip[2].y, edge.depth_b);
        rlTexCoord2f(edge.a.x, edge.a.y);
        rlVertex3f(strip[1].x, strip[1].y, edge.depth_a);
    rlEnd();
}

static void draw_shader_depth_screen_edges(RenderState* renderer, const ScreenEdgeList* list, bool hardware_depth_test) {
    if (!renderer || !list || list->count == 0 || !renderer->edge_depth_shader_ready || !renderer->edge_depth_texture_ready) return;

    if (hardware_depth_test) rlEnableDepthTest();
    else rlDisableDepthTest();
    rlDisableDepthMask();

    BeginShaderMode(renderer->edge_depth_shader);
    SetShaderValueTexture(renderer->edge_depth_shader, renderer->edge_depth_texture_loc, renderer->edge_depth_texture);
    SetShaderValue(renderer->edge_depth_shader, renderer->edge_depth_bias_loc, &renderer->edge_depth_bias, SHADER_UNIFORM_FLOAT);
    for (u32 i = 0; i < list->count; ++i) {
        draw_shader_depth_edge_quad(list->edges[i]);
    }
    EndShaderMode();

    if (hardware_depth_test) rlDisableDepthTest();
    rlEnableDepthMask();
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

static void push_scene_triangle(SceneTriangleList* scene, Vector3 rel_a, Vector3 rel_b, Vector3 rel_c, SceneTriangleProjection projection, Vector2 screen_min = {}, Vector2 screen_max = {}) {
    if (!scene) return;
    if (projection == scene_triangle_skip) return;

    SceneTriangle tri{};
    tri.a = rel_a;
    tri.b = rel_b;
    tri.c = rel_c;
    tri.e1 = rel_b - rel_a;
    tri.e2 = rel_c - rel_a;
    if (projection == scene_triangle_bounded) {
        tri.screen_min = screen_min;
        tri.screen_max = screen_max;
        tri.screen_bounds_valid = true;
    }
    const u32 tri_index = static_cast<u32>(scene->triangles.size());
    scene->triangles.push_back(tri);
    add_scene_triangle_to_grid(scene, tri_index);
}

static bool ray_intersects_scene_triangle(Vector3 origin, Vector3 dir, const SceneTriangle& tri, float max_dist) {
    constexpr float eps = 0.00001f;
    const Vector3 h = Vector3CrossProduct(dir, tri.e2);
    const float det = Vector3DotProduct(tri.e1, h);
    if (std::fabs(det) < eps) return false;

    const float inv_det = 1.0f / det;
    const Vector3 s = origin - tri.a;
    const float u = inv_det * Vector3DotProduct(s, h);
    if (u < -eps || u > 1.0f + eps) return false;

    const Vector3 q = Vector3CrossProduct(s, tri.e1);
    const float v = inv_det * Vector3DotProduct(dir, q);
    if (v < -eps || u + v > 1.0f + eps) return false;

    const float dist = inv_det * Vector3DotProduct(tri.e2, q);
    return dist > 0.002f && dist < max_dist - 0.035f;
}

static bool scene_point_visible(const std::vector<const SceneTriangle*>& candidates, Vector3 camera_pos, Vector3 point, const Vector2* sample_min = nullptr, const Vector2* sample_max = nullptr, u64* candidate_count = nullptr, u64* ray_test_count = nullptr) {
    const Vector3 delta = point - camera_pos;
    const float dist = safe_len(delta);
    if (dist <= 0.001f) return true;

    const Vector3 dir = delta / dist;
    for (const SceneTriangle* tri : candidates) {
        if (!tri) continue;
        if (sample_min && sample_max && tri->screen_bounds_valid && !screen_bboxes_overlap(*sample_min, *sample_max, tri->screen_min, tri->screen_max)) continue;
        if (candidate_count) ++(*candidate_count);
        if (ray_test_count) ++(*ray_test_count);
        if (ray_intersects_scene_triangle(camera_pos, dir, *tri, dist)) return false;
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

static bool edge_sample_visible(RenderState* renderer, const ScreenEdge& edge, const SpriteDepthOcclusion& sprite_occlusion, SceneTriangleList& scene, SceneCandidateTileCache* scene_cache, Vector3 camera_pos, float t) {
    const Vector2 p = edge_point_at(edge, t);
    if (renderer) ++renderer->debug_edge_sample_count;
    if (edge.use_scene_occlusion && scene_cache) {
        gather_point_scene_candidates(scene, p, renderer && renderer->brute_force_edge_occlusion, scene_cache);
        constexpr float sample_bbox_pad = 2.0f;
        const Vector2 sample_min = {p.x - sample_bbox_pad, p.y - sample_bbox_pad};
        const Vector2 sample_max = {p.x + sample_bbox_pad, p.y + sample_bbox_pad};
        const bool filter_by_sample = !(renderer && renderer->brute_force_edge_occlusion);
        if (!scene_cache->candidates.empty() &&
            !scene_point_visible(
                scene_cache->candidates,
                camera_pos,
                edge_rel_point_at(edge, t),
                filter_by_sample ? &sample_min : nullptr,
                filter_by_sample ? &sample_max : nullptr,
                renderer ? &renderer->debug_edge_candidate_count : nullptr,
                renderer ? &renderer->debug_edge_ray_test_count : nullptr)) {
            return false;
        }
    }

    const float edge_depth = edge_depth_at(edge, t);
    return sprite_occlusion_visible(sprite_occlusion, p, edge_depth);
}

static float refine_visibility_boundary(RenderState* renderer, const ScreenEdge& edge, const SpriteDepthOcclusion& sprite_occlusion, SceneTriangleList& scene, SceneCandidateTileCache* scene_cache, Vector3 camera_pos, float t0, bool visible0, float t1) {
    float lo = t0;
    float hi = t1;
    for (int i = 0; i < 8; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (edge_sample_visible(renderer, edge, sprite_occlusion, scene, scene_cache, camera_pos, mid) == visible0) lo = mid;
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

static void push_edge_interval(ScreenEdgeList* out, const ScreenEdge& edge, float t0, float t1) {
    if (!out || out->count >= out->max_count) return;
    t0 = clampf(t0, 0.0f, 1.0f);
    t1 = clampf(t1, 0.0f, 1.0f);
    if (t1 <= t0) return;

    ScreenEdge segment = edge;
    segment.a = edge_point_at(edge, t0);
    segment.b = edge_point_at(edge, t1);
    if (Vector2Length(segment.b - segment.a) < 0.75f) return;

    segment.depth_a = edge_depth_at(edge, t0);
    segment.depth_b = edge_depth_at(edge, t1);
    segment.inv_w_a = lerp_float(edge.inv_w_a, edge.inv_w_b, t0);
    segment.inv_w_b = lerp_float(edge.inv_w_a, edge.inv_w_b, t1);
    segment.rel_a = edge_rel_point_at(edge, t0);
    segment.rel_b = edge_rel_point_at(edge, t1);
    out->edges.push_back(segment);
    out->count = static_cast<u32>(out->edges.size());
}

static void push_edge_intervals(ScreenEdgeList* out, const ScreenEdge& edge, const std::vector<EdgeVisibleInterval>& raw_intervals) {
    if (!out || raw_intervals.empty()) return;

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
        push_edge_interval(out, edge, interval.start, interval.end);
    }
}

static bool screen_bboxes_overlap(Vector2 a_min, Vector2 a_max, Vector2 b_min, Vector2 b_max) {
    return a_max.x >= b_min.x && a_min.x <= b_max.x &&
           a_max.y >= b_min.y && a_min.y <= b_max.y;
}

static u32 begin_scene_candidate_query(SceneTriangleList* scene) {
    if (!scene) return 0;
    ++scene->query_stamp;
    if (scene->query_stamp == 0) {
        for (SceneTriangle& tri : scene->triangles) tri.query_stamp = 0;
        scene->query_stamp = 1;
    }
    return scene->query_stamp;
}

static void add_scene_candidate(SceneTriangleList* scene, u32 tri_index, u32 stamp, std::vector<const SceneTriangle*>* out_candidates) {
    if (!scene || !out_candidates || tri_index >= scene->triangles.size()) return;
    SceneTriangle& tri = scene->triangles[tri_index];
    if (tri.query_stamp == stamp) return;
    tri.query_stamp = stamp;
    out_candidates->push_back(&tri);
}

static void gather_point_scene_candidates(SceneTriangleList& scene, Vector2 point, bool brute_force, SceneCandidateTileCache* cache) {
    if (!cache) return;

    constexpr float point_bbox_pad = 2.0f;
    const int min_col = scene_grid_col_for_x(point.x - point_bbox_pad, scene.screen_w);
    const int max_col = scene_grid_col_for_x(point.x + point_bbox_pad, scene.screen_w);
    const int min_row = scene_grid_row_for_y(point.y - point_bbox_pad, scene.screen_h);
    const int max_row = scene_grid_row_for_y(point.y + point_bbox_pad, scene.screen_h);
    if (cache->valid &&
        cache->brute_force == brute_force &&
        cache->min_col == min_col &&
        cache->max_col == max_col &&
        cache->min_row == min_row &&
        cache->max_row == max_row) {
        return;
    }

    cache->valid = true;
    cache->brute_force = brute_force;
    cache->min_col = min_col;
    cache->max_col = max_col;
    cache->min_row = min_row;
    cache->max_row = max_row;
    cache->candidates.clear();
    if (scene.triangles.empty()) return;

    if (brute_force) {
        for (const SceneTriangle& tri : scene.triangles) {
            cache->candidates.push_back(&tri);
        }
        return;
    }

    const u32 stamp = begin_scene_candidate_query(&scene);
    for (u32 tri_index : scene.unbounded) {
        add_scene_candidate(&scene, tri_index, stamp, &cache->candidates);
    }

    for (int row = min_row; row <= max_row; ++row) {
        for (int col = min_col; col <= max_col; ++col) {
            const std::vector<u32>& bin = scene.bins[scene_grid_index(col, row)];
            for (u32 tri_index : bin) {
                if (tri_index >= scene.triangles.size()) continue;
                add_scene_candidate(&scene, tri_index, stamp, &cache->candidates);
            }
        }
    }
}

static void filter_depth_cut_screen_edges(RenderState* renderer, const ScreenEdgeList* list, const SpriteDepthOcclusion& sprite_occlusion, SceneTriangleList& scene, Vector3 camera_pos, ScreenEdgeList* out) {
    if (!list || list->count == 0 || !out) return;

    SceneCandidateTileCache scene_cache{};
    scene_cache.candidates.reserve(64);

    out->edges.reserve(std::min<size_t>(list->edges.size(), out->max_count));
    for (u32 i = 0; i < list->count; ++i) {
        const ScreenEdge& edge = list->edges[i];
        scene_cache.valid = false;

        const Vector2 d = edge.b - edge.a;
        const float dominant_len = fmaxf(fabsf(d.x), fabsf(d.y));
        constexpr float edge_visibility_sample_step_px = 3.0f;
        const int sample_count = static_cast<int>(fmaxf(1.0f, ceilf(dominant_len / edge_visibility_sample_step_px)));

        float prev_t = 0.0f;
        bool prev_visible = edge_sample_visible(renderer, edge, sprite_occlusion, scene, &scene_cache, camera_pos, prev_t);
        float visible_start = prev_visible ? 0.0f : -1.0f;
        std::vector<EdgeVisibleInterval> intervals;

        for (int sample = 1; sample <= sample_count; ++sample) {
            const float t = static_cast<float>(sample) / static_cast<float>(sample_count);
            const bool visible = edge_sample_visible(renderer, edge, sprite_occlusion, scene, &scene_cache, camera_pos, t);
            if (visible != prev_visible) {
                const float boundary = refine_visibility_boundary(renderer, edge, sprite_occlusion, scene, &scene_cache, camera_pos, prev_t, prev_visible, t);
                if (prev_visible) intervals.push_back({visible_start, boundary});
                else visible_start = boundary;
            }

            prev_t = t;
            prev_visible = visible;
        }

        if (prev_visible) intervals.push_back({visible_start, 1.0f});
        push_edge_intervals(out, edge, intervals);
        if (out->count >= out->max_count) return;
    }
}

static GpuScreenEdge to_gpu_screen_edge(const ScreenEdge& edge) {
    return {
        {edge.a.x, edge.a.y, edge.depth_a, edge.inv_w_a},
        {edge.b.x, edge.b.y, edge.depth_b, edge.inv_w_b},
        {edge.rel_a.x, edge.rel_a.y, edge.rel_a.z, edge.thickness},
        {edge.rel_b.x, edge.rel_b.y, edge.rel_b.z, edge.use_scene_occlusion ? 1.0f : 0.0f},
    };
}

static GpuSceneTriangle to_gpu_scene_triangle(const SceneTriangle& tri) {
    return {
        {tri.a.x, tri.a.y, tri.a.z, tri.screen_bounds_valid ? 1.0f : 0.0f},
        {tri.e1.x, tri.e1.y, tri.e1.z, 0.0f},
        {tri.e2.x, tri.e2.y, tri.e2.z, 0.0f},
        {tri.screen_min.x, tri.screen_min.y, tri.screen_max.x, tri.screen_max.y},
    };
}

static bool update_edge_filter_buffer(u32* id, u32* capacity, const void* data, size_t byte_count, int usage_hint) {
    if (!id || !capacity) return false;

    const size_t alloc_size = std::max<size_t>(byte_count, 16);
    if (alloc_size > static_cast<size_t>(std::numeric_limits<u32>::max())) return false;
    const u32 alloc_bytes = static_cast<u32>(alloc_size);

    if (*id == 0 || *capacity < alloc_bytes) {
        if (*id) rlUnloadShaderBuffer(*id);
        *id = rlLoadShaderBuffer(alloc_bytes, nullptr, usage_hint);
        *capacity = *id ? alloc_bytes : 0;
    }
    if (*id == 0) return false;

    if (data && byte_count > 0) {
        if (byte_count > static_cast<size_t>(std::numeric_limits<u32>::max())) return false;
        rlUpdateShaderBuffer(*id, data, static_cast<u32>(byte_count), 0);
    }
    return true;
}

static bool filter_depth_cut_screen_edges_compute(RenderState* renderer, const ScreenEdgeList* list, SceneTriangleList& scene, Vector3 camera_pos, ScreenEdgeList* out) {
    if (!renderer || !list || !out) return false;
    if (list->count == 0) return true;
    if (!renderer->edge_filter_compute_ready || !renderer->edge_scene_depth_texture_ready || !renderer->edge_depth_texture_ready) return false;

    struct EdgeSampleRange {
        u32 offset = 0;
        u32 sample_count = 1;
    };

    struct EdgeRefineEvent {
        u32 job_index = 0;
        bool visible0 = false;
    };

    const u32 edge_count = list->count;
    const u32 triangle_count = static_cast<u32>(scene.triangles.size());
    const u32 unbounded_count = static_cast<u32>(scene.unbounded.size());

    std::vector<GpuScreenEdge> gpu_edges;
    gpu_edges.reserve(edge_count);
    for (u32 i = 0; i < edge_count; ++i) {
        gpu_edges.push_back(to_gpu_screen_edge(list->edges[i]));
    }

    std::vector<GpuSceneTriangle> gpu_triangles;
    gpu_triangles.reserve(scene.triangles.size());
    for (const SceneTriangle& tri : scene.triangles) {
        gpu_triangles.push_back(to_gpu_scene_triangle(tri));
    }

    std::array<GpuBinRange, scene_occlusion_grid_count> gpu_bin_ranges{};
    std::vector<u32> gpu_bin_indices;
    size_t bin_index_count = 0;
    for (const std::vector<u32>& bin : scene.bins) {
        bin_index_count += bin.size();
    }
    gpu_bin_indices.reserve(bin_index_count);
    for (int i = 0; i < scene_occlusion_grid_count; ++i) {
        const std::vector<u32>& bin = scene.bins[i];
        gpu_bin_ranges[i].offset = static_cast<u32>(gpu_bin_indices.size());
        gpu_bin_ranges[i].count = static_cast<u32>(bin.size());
        for (u32 tri_index : bin) {
            gpu_bin_indices.push_back(tri_index);
        }
    }

    std::vector<EdgeSampleRange> sample_ranges(edge_count);
    std::vector<GpuEdgeSampleJob> sample_jobs;
    sample_jobs.reserve(static_cast<size_t>(edge_count) * 8);
    constexpr float edge_visibility_sample_step_px = 3.0f;
    for (u32 edge_i = 0; edge_i < edge_count; ++edge_i) {
        const ScreenEdge& edge = list->edges[edge_i];
        const Vector2 d = edge.b - edge.a;
        const float dominant_len = fmaxf(fabsf(d.x), fabsf(d.y));
        const u32 sample_count = static_cast<u32>(fmaxf(1.0f, ceilf(dominant_len / edge_visibility_sample_step_px)));
        const size_t result_count = static_cast<size_t>(sample_count) + 1;
        if (result_count > static_cast<size_t>(std::numeric_limits<u32>::max()) ||
            sample_jobs.size() > static_cast<size_t>(std::numeric_limits<u32>::max()) - result_count) {
            return false;
        }

        sample_ranges[edge_i] = {static_cast<u32>(sample_jobs.size()), sample_count};
        for (u32 sample_i = 0; sample_i <= sample_count; ++sample_i) {
            sample_jobs.push_back({edge_i, sample_i, sample_count, 0});
        }
    }
    if (sample_jobs.empty() || sample_jobs.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;

    const size_t edge_bytes = gpu_edges.size() * sizeof(GpuScreenEdge);
    const size_t triangle_bytes = gpu_triangles.size() * sizeof(GpuSceneTriangle);
    const size_t bin_range_bytes = gpu_bin_ranges.size() * sizeof(GpuBinRange);
    const size_t bin_index_bytes = gpu_bin_indices.size() * sizeof(u32);
    const size_t unbounded_bytes = scene.unbounded.size() * sizeof(u32);
    const size_t sample_job_bytes = sample_jobs.size() * sizeof(GpuEdgeSampleJob);
    const size_t sample_result_bytes = sample_jobs.size() * sizeof(u32);
    if (sample_job_bytes > static_cast<size_t>(std::numeric_limits<u32>::max()) ||
        sample_result_bytes > static_cast<size_t>(std::numeric_limits<u32>::max())) {
        return false;
    }

    GpuEdgeStats zero_stats{};
    if (!update_edge_filter_buffer(&renderer->edge_filter_edges_ssbo, &renderer->edge_filter_edges_capacity, gpu_edges.data(), edge_bytes, RL_DYNAMIC_COPY)) return false;
    if (!update_edge_filter_buffer(&renderer->edge_filter_triangles_ssbo, &renderer->edge_filter_triangles_capacity, gpu_triangles.data(), triangle_bytes, RL_DYNAMIC_COPY)) return false;
    if (!update_edge_filter_buffer(&renderer->edge_filter_bin_ranges_ssbo, &renderer->edge_filter_bin_ranges_capacity, gpu_bin_ranges.data(), bin_range_bytes, RL_DYNAMIC_COPY)) return false;
    if (!update_edge_filter_buffer(&renderer->edge_filter_bin_indices_ssbo, &renderer->edge_filter_bin_indices_capacity, gpu_bin_indices.empty() ? nullptr : gpu_bin_indices.data(), bin_index_bytes, RL_DYNAMIC_COPY)) return false;
    if (!update_edge_filter_buffer(&renderer->edge_filter_unbounded_ssbo, &renderer->edge_filter_unbounded_capacity, scene.unbounded.empty() ? nullptr : scene.unbounded.data(), unbounded_bytes, RL_DYNAMIC_COPY)) return false;
    if (!update_edge_filter_buffer(&renderer->edge_filter_sample_jobs_ssbo, &renderer->edge_filter_sample_jobs_capacity, sample_jobs.data(), sample_job_bytes, RL_DYNAMIC_COPY)) return false;
    if (!update_edge_filter_buffer(&renderer->edge_filter_sample_results_ssbo, &renderer->edge_filter_sample_results_capacity, nullptr, sample_result_bytes, RL_DYNAMIC_COPY)) return false;
    if (!update_edge_filter_buffer(&renderer->edge_filter_stats_ssbo, &renderer->edge_filter_stats_capacity, &zero_stats, sizeof(zero_stats), RL_DYNAMIC_COPY)) return false;

    Shader compute_shader{renderer->edge_filter_compute_program, nullptr};
    const int edge_count_i = static_cast<int>(edge_count);
    const int triangle_count_i = static_cast<int>(triangle_count);
    const int bin_count_i = scene_occlusion_grid_count;
    const int unbounded_count_i = static_cast<int>(unbounded_count);
    const int screen_size[2] = {renderer->native_w, renderer->native_h};
    const int brute_force = renderer->brute_force_edge_occlusion ? 1 : 0;
    const float camera_pos_uniform[3] = {camera_pos.x, camera_pos.y, camera_pos.z};

    auto bind_common_compute_state = [&]() {
        SetShaderValue(compute_shader, renderer->edge_filter_edge_count_loc, &edge_count_i, SHADER_UNIFORM_INT);
        SetShaderValue(compute_shader, renderer->edge_filter_triangle_count_loc, &triangle_count_i, SHADER_UNIFORM_INT);
        SetShaderValue(compute_shader, renderer->edge_filter_bin_count_loc, &bin_count_i, SHADER_UNIFORM_INT);
        SetShaderValue(compute_shader, renderer->edge_filter_unbounded_count_loc, &unbounded_count_i, SHADER_UNIFORM_INT);
        SetShaderValue(compute_shader, renderer->edge_filter_screen_size_loc, screen_size, SHADER_UNIFORM_IVEC2);
        SetShaderValue(compute_shader, renderer->edge_filter_camera_pos_loc, camera_pos_uniform, SHADER_UNIFORM_VEC3);
        SetShaderValue(compute_shader, renderer->edge_filter_brute_force_loc, &brute_force, SHADER_UNIFORM_INT);
        SetShaderValueTexture(compute_shader, renderer->edge_filter_scene_depth_texture_loc, renderer->edge_scene_depth_texture);
        SetShaderValueTexture(compute_shader, renderer->edge_filter_after_sprites_depth_texture_loc, renderer->edge_depth_texture);
        rlBindShaderBuffer(renderer->edge_filter_edges_ssbo, 1);
        rlBindShaderBuffer(renderer->edge_filter_triangles_ssbo, 2);
        rlBindShaderBuffer(renderer->edge_filter_bin_ranges_ssbo, 3);
        rlBindShaderBuffer(renderer->edge_filter_bin_indices_ssbo, 4);
        rlBindShaderBuffer(renderer->edge_filter_unbounded_ssbo, 5);
        rlBindShaderBuffer(renderer->edge_filter_sample_jobs_ssbo, 6);
        rlBindShaderBuffer(renderer->edge_filter_sample_results_ssbo, 7);
        rlBindShaderBuffer(renderer->edge_filter_stats_ssbo, 10);
    };

    int pass_id = 0;
    int job_count = static_cast<int>(sample_jobs.size());
    rlEnableShader(renderer->edge_filter_compute_program);
    bind_common_compute_state();
    SetShaderValue(compute_shader, renderer->edge_filter_pass_id_loc, &pass_id, SHADER_UNIFORM_INT);
    SetShaderValue(compute_shader, renderer->edge_filter_job_count_loc, &job_count, SHADER_UNIFORM_INT);
    rlComputeShaderDispatch((static_cast<u32>(sample_jobs.size()) + edge_filter_compute_local_size - 1) / edge_filter_compute_local_size, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    rlDisableShader();

    std::vector<u32> sample_results(sample_jobs.size());
    rlReadShaderBuffer(renderer->edge_filter_sample_results_ssbo, sample_results.data(), static_cast<u32>(sample_result_bytes), 0);

    std::vector<u8> initial_visible(edge_count, 0);
    std::vector<u8> final_visible(edge_count, 0);
    std::vector<std::vector<EdgeRefineEvent>> refine_events(edge_count);
    std::vector<GpuEdgeRefineJob> refine_jobs;
    refine_jobs.reserve(edge_count);
    for (u32 edge_i = 0; edge_i < edge_count; ++edge_i) {
        const EdgeSampleRange range = sample_ranges[edge_i];
        const size_t offset = range.offset;
        bool prev_visible = sample_results[offset] != 0;
        float prev_t = 0.0f;
        initial_visible[edge_i] = prev_visible ? 1 : 0;

        for (u32 sample_i = 1; sample_i <= range.sample_count; ++sample_i) {
            const bool visible = sample_results[offset + sample_i] != 0;
            const float t = static_cast<float>(sample_i) / static_cast<float>(range.sample_count);
            if (visible != prev_visible) {
                if (refine_jobs.size() >= static_cast<size_t>(std::numeric_limits<u32>::max())) return false;
                const u32 job_index = static_cast<u32>(refine_jobs.size());
                refine_jobs.push_back({edge_i, prev_visible ? 1u : 0u, 0, 0, prev_t, t, 0.0f, 0.0f});
                refine_events[edge_i].push_back({job_index, prev_visible});
            }
            prev_t = t;
            prev_visible = visible;
        }

        final_visible[edge_i] = prev_visible ? 1 : 0;
    }

    std::vector<float> refine_results(refine_jobs.size(), 0.0f);
    if (!refine_jobs.empty()) {
        if (refine_jobs.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
        const size_t refine_job_bytes = refine_jobs.size() * sizeof(GpuEdgeRefineJob);
        const size_t refine_result_bytes = refine_jobs.size() * sizeof(float);
        if (refine_job_bytes > static_cast<size_t>(std::numeric_limits<u32>::max()) ||
            refine_result_bytes > static_cast<size_t>(std::numeric_limits<u32>::max())) {
            return false;
        }

        if (!update_edge_filter_buffer(&renderer->edge_filter_refine_jobs_ssbo, &renderer->edge_filter_refine_jobs_capacity, refine_jobs.data(), refine_job_bytes, RL_DYNAMIC_COPY)) return false;
        if (!update_edge_filter_buffer(&renderer->edge_filter_refine_results_ssbo, &renderer->edge_filter_refine_results_capacity, nullptr, refine_result_bytes, RL_DYNAMIC_COPY)) return false;

        pass_id = 1;
        job_count = static_cast<int>(refine_jobs.size());
        rlEnableShader(renderer->edge_filter_compute_program);
        bind_common_compute_state();
        rlBindShaderBuffer(renderer->edge_filter_refine_jobs_ssbo, 8);
        rlBindShaderBuffer(renderer->edge_filter_refine_results_ssbo, 9);
        SetShaderValue(compute_shader, renderer->edge_filter_pass_id_loc, &pass_id, SHADER_UNIFORM_INT);
        SetShaderValue(compute_shader, renderer->edge_filter_job_count_loc, &job_count, SHADER_UNIFORM_INT);
        rlComputeShaderDispatch((static_cast<u32>(refine_jobs.size()) + edge_filter_compute_local_size - 1) / edge_filter_compute_local_size, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        rlDisableShader();

        rlReadShaderBuffer(renderer->edge_filter_refine_results_ssbo, refine_results.data(), static_cast<u32>(refine_result_bytes), 0);
    }

    GpuEdgeStats stats{};
    rlReadShaderBuffer(renderer->edge_filter_stats_ssbo, &stats, sizeof(stats), 0);
    if (stats.overflow) return false;

    out->edges.reserve(std::min<size_t>(list->edges.size(), out->max_count));
    std::vector<EdgeVisibleInterval> raw_intervals;
    raw_intervals.reserve(16);
    for (u32 edge_i = 0; edge_i < edge_count; ++edge_i) {
        raw_intervals.clear();

        float visible_start = initial_visible[edge_i] ? 0.0f : -1.0f;
        for (const EdgeRefineEvent& event : refine_events[edge_i]) {
            if (event.job_index >= refine_results.size()) return false;
            const float boundary = refine_results[event.job_index];
            if (event.visible0) raw_intervals.push_back({visible_start, boundary});
            else visible_start = boundary;
        }
        if (final_visible[edge_i]) raw_intervals.push_back({visible_start, 1.0f});

        push_edge_intervals(out, list->edges[edge_i], raw_intervals);
        if (out->count >= out->max_count) break;
    }

    renderer->debug_edge_sample_count = stats.samples;
    renderer->debug_edge_candidate_count = stats.candidates;
    renderer->debug_edge_ray_test_count = stats.ray_tests;
    return true;
}

static void push_world_screen_edge(const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, int width, int height, float near_plane, ScreenEdgeList* edge_list, Vector3 a, Vector3 b, Color color, u8 thickness_px, bool use_scene_occlusion = true) {
    if (!clip_edge_to_near_plane(&a, &b, camera, near_plane)) return;

    ProjectedEdgePoint pa{};
    ProjectedEdgePoint pb{};
    if (!project_edge_point(a, view_matrix, projection_matrix, width, height, &pa)) return;
    if (!project_edge_point(b, view_matrix, projection_matrix, width, height, &pb)) return;
    push_screen_edge(edge_list, pa, pb, a, b, width, height, color, thickness_px, use_scene_occlusion);
}

static Texture2D render_texture(RenderState* renderer, u32 texture_id) {
    if (texture_id == render_texture_life && renderer->life_ready) return renderer->life_texture;
    if (texture_id == render_texture_cross && renderer->cross_ready) return renderer->cross_texture;
    if (texture_id == render_texture_grid && renderer->world_textures_ready) return renderer->grid_texture;
    if (texture_id == render_texture_grass && renderer->world_textures_ready) return renderer->grass_texture;
    if (texture_id == render_texture_stone && renderer->world_textures_ready) return renderer->stone_texture;
    if (texture_id == render_texture_roof && renderer->world_textures_ready) return renderer->roof_texture;
    return renderer->white_texture;
}

static Vector2 planar_texture_uv(Vector3 point, Vector3 origin, Vector3 normal, float pixels_per_meter, Texture2D texture) {
    const Vector3 rel = point - origin;
    Vector3 tangent{};
    Vector3 bitangent{};
    const Vector3 abs_n = {std::fabs(normal.x), std::fabs(normal.y), std::fabs(normal.z)};
    if (abs_n.y >= abs_n.x && abs_n.y >= abs_n.z) {
        tangent = {1.0f, 0.0f, 0.0f};
        bitangent = {0.0f, 0.0f, normal.y >= 0.0f ? -1.0f : 1.0f};
    } else if (abs_n.x >= abs_n.z) {
        tangent = {0.0f, 0.0f, normal.x >= 0.0f ? 1.0f : -1.0f};
        bitangent = {0.0f, 1.0f, 0.0f};
    } else {
        tangent = {normal.z >= 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
        bitangent = {0.0f, 1.0f, 0.0f};
    }
    const float tex_w = static_cast<float>(texture.width > 0 ? texture.width : 1);
    const float tex_h = static_cast<float>(texture.height > 0 ? texture.height : 1);
    return {
        0.5f + Vector3DotProduct(rel, tangent) * pixels_per_meter / tex_w,
        0.5f - Vector3DotProduct(rel, bitangent) * pixels_per_meter / tex_h
    };
}

struct PreparedTriangle {
    Vector3 a{};
    Vector3 b{};
    Vector3 c{};
    Color color = WHITE;
    Vector2 uv_a{};
    Vector2 uv_b{};
    Vector2 uv_c{};
    u32 texture_id = invalid_id;
};

struct PreparedRenderBuffer {
    std::vector<PreparedTriangle> triangles{};
    ScreenEdgeList edge_list{};
    std::vector<SceneTriangle> scene_triangles{};
};

struct PreparedRenderBatch {
    std::vector<PreparedRenderBuffer> buffers{};
    ScreenEdgeList edge_list{};
};

struct MeshPrepJob {
    const MeshInstance* mesh = nullptr;
    float chunk_dist = 0.0f;
};

struct VisibleChunkRef {
    const Chunk* chunk = nullptr;
    float chunk_dist = 0.0f;
};

static void append_scene_triangle(SceneTriangleList* scene, const SceneTriangle& tri) {
    if (!scene) return;
    const u32 tri_index = static_cast<u32>(scene->triangles.size());
    scene->triangles.push_back(tri);
    add_scene_triangle_to_grid(scene, tri_index);
}

static void push_prepared_scene_triangle(std::vector<SceneTriangle>* out, Vector3 rel_a, Vector3 rel_b, Vector3 rel_c, SceneTriangleProjection projection, Vector2 screen_min = {}, Vector2 screen_max = {}) {
    if (!out || projection == scene_triangle_skip) return;

    SceneTriangle tri{};
    tri.a = rel_a;
    tri.b = rel_b;
    tri.c = rel_c;
    tri.e1 = rel_b - rel_a;
    tri.e2 = rel_c - rel_a;
    if (projection == scene_triangle_bounded) {
        tri.screen_min = screen_min;
        tri.screen_max = screen_max;
        tri.screen_bounds_valid = true;
    }
    out->push_back(tri);
}

static void prepare_mesh_instance(RenderState* renderer, Dimension* dim, const CameraView& view, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, float near_plane, PreparedRenderBuffer* out, bool collect_scene_triangles, const MeshInstance* mesh, float chunk_dist) {
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
    const float mesh_dist_chunks = safe_len(origin) / fmaxf(dim->chunk_size_m, 0.001f);
    const Matrix basis = matrix_no_translation(mesh->se3);
    for (u32 i = 0; i < geometry->vertex_count; ++i) {
        transformed[i] = Vector3Transform(geometry->vertices[i], basis) + origin;
    }

    Color base = resolve_mesh_color(mesh);
    const float ff = fog_factor(dim, chunk_dist);
    const bool collect_mesh_scene_triangles = collect_scene_triangles && mesh_edges_in_render_radius(dim, mesh_dist_chunks);

    for (u32 i = 0; i < geometry->triangle_count; ++i) {
        const Triangle tri = geometry->triangles[i];
        const Vector3 a = transformed[tri.a];
        const Vector3 b = transformed[tri.b];
        const Vector3 c = transformed[tri.c];
        const Vector3 n = safe_norm(Vector3CrossProduct(b - a, c - a));
        Color shaded = mesh->lit ? color_scaled(base, face_light(dim, view, origin, n)) : base;
        shaded = mix_color(shaded, dim->fog_color, ff);
        PreparedTriangle prepared{};
        prepared.a = a;
        prepared.b = b;
        prepared.c = c;
        prepared.color = shaded;
        prepared.texture_id = mesh->texture_id;
        const Texture2D texture = render_texture(renderer, mesh->texture_id);
        prepared.uv_a = planar_texture_uv(a, origin, n, dim->pixels_per_meter, texture);
        prepared.uv_b = planar_texture_uv(b, origin, n, dim->pixels_per_meter, texture);
        prepared.uv_c = planar_texture_uv(c, origin, n, dim->pixels_per_meter, texture);
        out->triangles.push_back(prepared);
        if (collect_mesh_scene_triangles) {
            Vector2 screen_min{};
            Vector2 screen_max{};
            const SceneTriangleProjection projection = clipped_triangle_screen_bounds(
                a,
                b,
                c,
                camera,
                view_matrix,
                projection_matrix,
                renderer->native_w,
                renderer->native_h,
                near_plane,
                &screen_min,
                &screen_max);
            push_prepared_scene_triangle(&out->scene_triangles, a, b, c, projection, screen_min, screen_max);
        }
    }

    if (!mesh->draw_edges) return;
    const float edge_alpha = mesh_edge_alpha(dim, mesh_dist_chunks);
    if (edge_alpha <= 0.0f) return;

    const Color edge_color = color_with_alpha_factor(mix_color(BLACK, dim->fog_color, ff), edge_alpha);
    for (u32 i = 0; i < geometry->edge_count; ++i) {
        const Edge e = geometry->edges[i];
        push_world_screen_edge(camera, view_matrix, projection_matrix, renderer->native_w, renderer->native_h, near_plane, &out->edge_list, transformed[e.a], transformed[e.b], edge_color, e.thickness_px);
    }
}

static void draw_mesh_instance_direct(RenderState* renderer, Dimension* dim, const CameraView& view, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, float near_plane, ScreenEdgeList* edge_list, SceneTriangleList* scene_triangles, const MeshInstance* mesh, float chunk_dist) {
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
    const float mesh_dist_chunks = safe_len(origin) / fmaxf(dim->chunk_size_m, 0.001f);
    const Matrix basis = matrix_no_translation(mesh->se3);
    for (u32 i = 0; i < geometry->vertex_count; ++i) {
        transformed[i] = Vector3Transform(geometry->vertices[i], basis) + origin;
    }

    Color base = resolve_mesh_color(mesh);
    const float ff = fog_factor(dim, chunk_dist);
    const bool collect_mesh_scene_triangles = scene_triangles && mesh_edges_in_render_radius(dim, mesh_dist_chunks);

    for (u32 i = 0; i < geometry->triangle_count; ++i) {
        const Triangle tri = geometry->triangles[i];
        const Vector3 a = transformed[tri.a];
        const Vector3 b = transformed[tri.b];
        const Vector3 c = transformed[tri.c];
        const Vector3 n = safe_norm(Vector3CrossProduct(b - a, c - a));
        Color shaded = mesh->lit ? color_scaled(base, face_light(dim, view, origin, n)) : base;
        shaded = mix_color(shaded, dim->fog_color, ff);
        const Texture2D texture = render_texture(renderer, mesh->texture_id);
        const Vector2 uv_a = planar_texture_uv(a, origin, n, dim->pixels_per_meter, texture);
        const Vector2 uv_b = planar_texture_uv(b, origin, n, dim->pixels_per_meter, texture);
        const Vector2 uv_c = planar_texture_uv(c, origin, n, dim->pixels_per_meter, texture);
        rlSetTexture(texture.id);
        rlBegin(RL_TRIANGLES);
            rlColor4ub(shaded.r, shaded.g, shaded.b, shaded.a);
            rlTexCoord2f(uv_a.x, uv_a.y); rlVertex3f(a.x, a.y, a.z);
            rlTexCoord2f(uv_b.x, uv_b.y); rlVertex3f(b.x, b.y, b.z);
            rlTexCoord2f(uv_c.x, uv_c.y); rlVertex3f(c.x, c.y, c.z);
        rlEnd();
        rlSetTexture(0);
        if (collect_mesh_scene_triangles) {
            Vector2 screen_min{};
            Vector2 screen_max{};
            const SceneTriangleProjection projection = clipped_triangle_screen_bounds(
                a,
                b,
                c,
                camera,
                view_matrix,
                projection_matrix,
                renderer->native_w,
                renderer->native_h,
                near_plane,
                &screen_min,
                &screen_max);
            push_scene_triangle(scene_triangles, a, b, c, projection, screen_min, screen_max);
        }
    }

    if (!mesh->draw_edges) return;
    const float edge_alpha = mesh_edge_alpha(dim, mesh_dist_chunks);
    if (edge_alpha <= 0.0f) return;

    const Color edge_color = color_with_alpha_factor(mix_color(BLACK, dim->fog_color, ff), edge_alpha);
    for (u32 i = 0; i < geometry->edge_count; ++i) {
        const Edge e = geometry->edges[i];
        push_world_screen_edge(camera, view_matrix, projection_matrix, renderer->native_w, renderer->native_h, near_plane, edge_list, transformed[e.a], transformed[e.b], edge_color, e.thickness_px);
    }
}

static void draw_prepared_triangles(RenderState* renderer, const std::vector<PreparedTriangle>& triangles) {
    for (const PreparedTriangle& tri : triangles) {
        const Texture2D texture = render_texture(renderer, tri.texture_id);
        rlSetTexture(texture.id);
        rlBegin(RL_TRIANGLES);
            rlColor4ub(tri.color.r, tri.color.g, tri.color.b, tri.color.a);
            rlTexCoord2f(tri.uv_a.x, tri.uv_a.y); rlVertex3f(tri.a.x, tri.a.y, tri.a.z);
            rlTexCoord2f(tri.uv_b.x, tri.uv_b.y); rlVertex3f(tri.b.x, tri.b.y, tri.b.z);
            rlTexCoord2f(tri.uv_c.x, tri.uv_c.y); rlVertex3f(tri.c.x, tri.c.y, tri.c.z);
        rlEnd();
        rlSetTexture(0);
    }
}

static void draw_prepared_triangles(RenderState* renderer, const PreparedRenderBatch& batch) {
    for (const PreparedRenderBuffer& buffer : batch.buffers) {
        draw_prepared_triangles(renderer, buffer.triangles);
    }
}

static void append_screen_edges(ScreenEdgeList* dst, const ScreenEdgeList& src) {
    if (!dst) return;
    for (const ScreenEdge& edge : src.edges) {
        if (dst->count >= dst->max_count) return;
        dst->edges.push_back(edge);
        dst->count = static_cast<u32>(dst->edges.size());
    }
}

static void prepare_mesh_job_range(RenderState* renderer, Dimension* dim, const CameraView& view, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, float near_plane, bool collect_scene_triangles, const std::vector<MeshPrepJob>& jobs, size_t begin, size_t end, PreparedRenderBuffer* out) {
    if (!out || begin >= end) return;

    out->triangles.reserve((end - begin) * 12);
    out->edge_list.max_count = max_render_edges;
    for (size_t i = begin; i < end; ++i) {
        const MeshPrepJob& job = jobs[i];
        prepare_mesh_instance(renderer, dim, view, camera, view_matrix, projection_matrix, near_plane, out, collect_scene_triangles, job.mesh, job.chunk_dist);
    }
}

static PreparedRenderBatch prepare_mesh_jobs_parallel(RenderState* renderer, Dimension* dim, const CameraView& view, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, float near_plane, bool collect_scene_triangles, u32 edge_cap, const std::vector<MeshPrepJob>& jobs) {
    PreparedRenderBatch batch{};
    batch.edge_list.max_count = edge_cap;
    if (jobs.empty()) return batch;

    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const u32 desired_workers = jobs.size() < parallel_mesh_prep_min_jobs ? 1u : static_cast<u32>(std::min<size_t>(16, (jobs.size() + 255) / 256));
    const u32 worker_count = std::max(1u, std::min<u32>(std::min<u32>(hw, desired_workers), static_cast<u32>(jobs.size())));

    if (worker_count <= 1) {
        batch.buffers.resize(1);
        prepare_mesh_job_range(renderer, dim, view, camera, view_matrix, projection_matrix, near_plane, collect_scene_triangles, jobs, 0, jobs.size(), &batch.buffers[0]);
        append_screen_edges(&batch.edge_list, batch.buffers[0].edge_list);
        return batch;
    }

    batch.buffers.resize(worker_count);
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    const size_t jobs_per_worker = (jobs.size() + static_cast<size_t>(worker_count) - 1) / static_cast<size_t>(worker_count);
    for (u32 worker = 0; worker < worker_count; ++worker) {
        const size_t begin = static_cast<size_t>(worker) * jobs_per_worker;
        const size_t end = std::min(jobs.size(), begin + jobs_per_worker);
        if (begin >= end) break;
        workers.emplace_back([&, begin, end, worker]() {
            prepare_mesh_job_range(renderer, dim, view, camera, view_matrix, projection_matrix, near_plane, collect_scene_triangles, jobs, begin, end, &batch.buffers[worker]);
        });
    }

    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }

    size_t edge_count = 0;
    for (const PreparedRenderBuffer& buffer : batch.buffers) {
        edge_count += buffer.edge_list.edges.size();
    }

    batch.edge_list.edges.reserve(std::min<size_t>(edge_cap, edge_count));
    for (const PreparedRenderBuffer& buffer : batch.buffers) {
        append_screen_edges(&batch.edge_list, buffer.edge_list);
    }

    return batch;
}

struct SpriteQuad {
    Vector3 bottom_left{};
    Vector3 bottom_right{};
    Vector3 top_right{};
    Vector3 top_left{};
};

static SpriteQuad make_sprite_quad(const SpriteInstance* sprite, Vector3 center, const Camera3D& camera, Texture2D texture, float pixels_per_meter) {
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

    const float ppm = fmaxf(1.0f, pixels_per_meter);
    const Vector2 world_size = {
        static_cast<float>(texture.width) / ppm * sprite->size.x,
        static_cast<float>(texture.height) / ppm * sprite->size.y
    };
    const Vector3 half_right = right * (world_size.x * 0.5f);
    const Vector3 half_up = up * (world_size.y * 0.5f);
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

static Texture2D painted_sprite_texture(RenderState* renderer, const Dimension* dim, u32 sprite_id, const SpriteInstance* sprite, Color* out_tint);

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
        const u32 sprite_id = chunk->sprites[i];
        Color sprite_tint = sprite->color;
        const Texture2D texture = painted_sprite_texture(renderer, dim, sprite_id, sprite, &sprite_tint);
        const Rectangle source = {0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)};
        const SpriteQuad quad = make_sprite_quad(sprite, rel, camera, texture, dim->pixels_per_meter);
        draw_textured_sprite_quad(texture, source, quad, sprite_tint);
    }

    if (renderer->sprite_alpha_shader_ready) {
        EndShaderMode();
    } else {
        rlEnableDepthMask();
    }
}

static void push_player_ring_edges(RenderState* renderer, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, float near_plane, ScreenEdgeList* edge_list, Vector3 center, float radius, Color color) {
    constexpr u32 segments = 32;
    constexpr u8 thickness_px = 2;
    for (u32 i = 0; i < segments; ++i) {
        const float a0 = static_cast<float>(i) * 2.0f * PI / static_cast<float>(segments);
        const float a1 = static_cast<float>(i + 1) * 2.0f * PI / static_cast<float>(segments);
        const Vector3 p0 = center + Vector3{std::cos(a0) * radius, 0.0f, std::sin(a0) * radius};
        const Vector3 p1 = center + Vector3{std::cos(a1) * radius, 0.0f, std::sin(a1) * radius};
        push_world_screen_edge(camera, view_matrix, projection_matrix, renderer->native_w, renderer->native_h, near_plane, edge_list, p0, p1, color, thickness_px);
    }
}

static Texture2D painted_sprite_texture(RenderState* renderer, const Dimension* dim, u32 sprite_id, const SpriteInstance* sprite, Color* out_tint) {
    const Texture2D base = render_texture(renderer, sprite->texture_id);
    bool has_paint = false;
    u64 sprite_paint_hash = 1469598103934665603ull;
    for (const PaintedPixel& pixel : dim->painted_pixels) {
        if (pixel.sprite_id == sprite_id) {
            has_paint = true;
            sprite_paint_hash ^= static_cast<u64>(static_cast<u32>(pixel.sprite_pixel_x));
            sprite_paint_hash *= 1099511628211ull;
            sprite_paint_hash ^= static_cast<u64>(static_cast<u32>(pixel.sprite_pixel_y));
            sprite_paint_hash *= 1099511628211ull;
            sprite_paint_hash ^= static_cast<u64>(ColorToInt(pixel.color));
            sprite_paint_hash *= 1099511628211ull;
        }
    }

    auto cache_at = renderer->sprite_paint_textures.end();
    for (auto it = renderer->sprite_paint_textures.begin(); it != renderer->sprite_paint_textures.end(); ++it) {
        if (it->sprite_id == sprite_id) {
            cache_at = it;
            break;
        }
    }
    if (!has_paint) {
        if (cache_at != renderer->sprite_paint_textures.end()) {
            if (IsTextureValid(cache_at->texture)) UnloadTexture(cache_at->texture);
            renderer->sprite_paint_textures.erase(cache_at);
        }
        if (out_tint) *out_tint = sprite->color;
        return base;
    }

    const bool cache_valid = cache_at != renderer->sprite_paint_textures.end() &&
        cache_at->paint_revision == sprite_paint_hash &&
        cache_at->base_texture_id == sprite->texture_id &&
        ColorIsEqual(cache_at->tint, sprite->color) && IsTextureValid(cache_at->texture);
    if (!cache_valid) {
        if (cache_at == renderer->sprite_paint_textures.end()) {
            renderer->sprite_paint_textures.push_back({});
            cache_at = renderer->sprite_paint_textures.end() - 1;
        } else if (IsTextureValid(cache_at->texture)) {
            UnloadTexture(cache_at->texture);
            cache_at->texture = {};
        }

        Image image = LoadImageFromTexture(base);
        if (IsImageValid(image)) {
            ImageColorTint(&image, sprite->color);
            for (const PaintedPixel& pixel : dim->painted_pixels) {
                if (pixel.sprite_id != sprite_id || pixel.sprite_pixel_x < 0 || pixel.sprite_pixel_y < 0 ||
                    pixel.sprite_pixel_x >= image.width || pixel.sprite_pixel_y >= image.height) continue;
                ImageDrawPixel(&image, pixel.sprite_pixel_x, pixel.sprite_pixel_y, pixel.color);
            }
            cache_at->texture = LoadTextureFromImage(image);
            UnloadImage(image);
            if (IsTextureValid(cache_at->texture)) {
                GenTextureMipmaps(&cache_at->texture);
                SetTextureFilter(cache_at->texture, TEXTURE_FILTER_ANISOTROPIC_8X);
            }
        }
        cache_at->sprite_id = sprite_id;
        cache_at->base_texture_id = sprite->texture_id;
        cache_at->paint_revision = sprite_paint_hash;
        cache_at->tint = sprite->color;
    }
    if (out_tint) *out_tint = WHITE;
    return IsTextureValid(cache_at->texture) ? cache_at->texture : base;
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

        push_scene_triangle(scene_triangles, b0, t0, t1, scene_triangle_unbounded);
        push_scene_triangle(scene_triangles, b0, t1, b1, scene_triangle_unbounded);
        push_scene_triangle(scene_triangles, top, t1, t0, scene_triangle_unbounded);
        push_scene_triangle(scene_triangles, bottom, b0, b1, scene_triangle_unbounded);
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

static void draw_players(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id, const Camera3D& camera, const Matrix& view_matrix, const Matrix& projection_matrix, float near_plane, ScreenEdgeList* edge_list, SceneTriangleList* scene_triangles, PlayerNameTag* hovered_tag) {
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
        push_player_ring_edges(renderer, camera, view_matrix, projection_matrix, near_plane, edge_list, bottom, radius, BLACK);
        push_player_ring_edges(renderer, camera, view_matrix, projection_matrix, near_plane, edge_list, top, radius, BLACK);

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
        const Texture2D texture = render_texture(renderer, marker.texture_id);
        const Rectangle source = {0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)};
        const SpriteQuad quad = make_sprite_quad(&marker, end, camera, texture, dim->pixels_per_meter);
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

static Vector3 canonical_surface_normal(Vector3 normal) {
    normal = safe_norm(normal, {0.0f, 1.0f, 0.0f});
    const Vector3 a = {std::fabs(normal.x), std::fabs(normal.y), std::fabs(normal.z)};
    const float dominant = a.x >= a.y && a.x >= a.z ? normal.x : (a.y >= a.z ? normal.y : normal.z);
    return dominant < 0.0f ? normal * -1.0f : normal;
}

static Vector3 surface_tangent(Vector3 normal) {
    const Vector3 a = {std::fabs(normal.x), std::fabs(normal.y), std::fabs(normal.z)};
    if (a.y >= a.x && a.y >= a.z) return {1.0f, 0.0f, 0.0f};
    if (a.x >= a.z) return {0.0f, 0.0f, normal.x >= 0.0f ? 1.0f : -1.0f};
    return {normal.z >= 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
}

static bool paint_pixel_matches_mesh(const Dimension* dim, const PaintedPixel& pixel, const MeshInstance* mesh) {
    const MeshGeometry* geometry = mesh ? arena_get(&dim->geometries, mesh->geometry) : nullptr;
    if (!geometry) return false;
    const Vector3 rel = world_delta_meters(pixel.center, mesh->origin, dim->chunk_size_m);
    if (safe_len(rel) > mesh->bounds_radius + 0.1f) return false;
    const Matrix basis = matrix_no_translation(mesh->se3);
    Vector3 vertices[max_vertices_per_geometry]{};
    for (u32 i = 0; i < geometry->vertex_count; ++i) vertices[i] = Vector3Transform(geometry->vertices[i], basis);
    for (u32 i = 0; i < geometry->triangle_count; ++i) {
        const Triangle tri = geometry->triangles[i];
        const Vector3 a = vertices[tri.a];
        const Vector3 b = vertices[tri.b];
        const Vector3 c = vertices[tri.c];
        const Vector3 normal = safe_norm(Vector3CrossProduct(b - a, c - a));
        if (std::fabs(Vector3DotProduct(pixel.normal, normal)) < 0.99f ||
            std::fabs(Vector3DotProduct(rel - a, normal)) > 0.02f) continue;
        const Vector3 v0 = b - a;
        const Vector3 v1 = c - a;
        const Vector3 v2 = rel - a;
        const float d00 = Vector3DotProduct(v0, v0);
        const float d01 = Vector3DotProduct(v0, v1);
        const float d11 = Vector3DotProduct(v1, v1);
        const float d20 = Vector3DotProduct(v2, v0);
        const float d21 = Vector3DotProduct(v2, v1);
        const float denominator = d00 * d11 - d01 * d01;
        if (std::fabs(denominator) < 0.000001f) continue;
        const float v = (d11 * d20 - d01 * d21) / denominator;
        const float w = (d00 * d21 - d01 * d20) / denominator;
        const float u = 1.0f - v - w;
        if (u >= -0.05f && v >= -0.05f && w >= -0.05f) return true;
    }
    return false;
}

static u64 paint_mesh_signature(const MeshInstance& mesh, u32 mesh_id) {
    u64 hash = 1469598103934665603ull;
    auto add = [&](u64 value) { hash ^= value; hash *= 1099511628211ull; };
    add(mesh_id); add(mesh.geometry);
    add(static_cast<u32>(mesh.origin.chunk.x));
    add(static_cast<u32>(mesh.origin.chunk.y));
    add(static_cast<u32>(mesh.origin.chunk.z));
    u32 bits = 0;
    std::memcpy(&bits, &mesh.origin.local.x, sizeof(bits)); add(bits);
    std::memcpy(&bits, &mesh.origin.local.y, sizeof(bits)); add(bits);
    std::memcpy(&bits, &mesh.origin.local.z, sizeof(bits)); add(bits);
    const float transform[] = {
        mesh.se3.m0, mesh.se3.m1, mesh.se3.m2, mesh.se3.m3,
        mesh.se3.m4, mesh.se3.m5, mesh.se3.m6, mesh.se3.m7,
        mesh.se3.m8, mesh.se3.m9, mesh.se3.m10, mesh.se3.m11,
        mesh.se3.m12, mesh.se3.m13, mesh.se3.m14, mesh.se3.m15};
    for (float value : transform) { std::memcpy(&bits, &value, sizeof(bits)); add(bits); }
    return hash;
}

static void update_mesh_paint_surfaces(RenderState* renderer, Dimension* dim) {
    if (!renderer || !dim) return;
    u64 topology_hash = 1469598103934665603ull;
    for (u32 slot = 0; slot < dim->meshes.count; ++slot) {
        topology_hash ^= paint_mesh_signature(dim->meshes.data[slot], arena_id_at_slot(&dim->meshes, slot));
        topology_hash *= 1099511628211ull;
    }
    if (renderer->mesh_paint_revision == dim->paint_revision && renderer->mesh_paint_topology_hash == topology_hash) return;
    const bool topology_changed = renderer->mesh_paint_topology_hash != topology_hash;
    renderer->mesh_paint_revision = dim->paint_revision;
    renderer->mesh_paint_topology_hash = topology_hash;

    std::vector<std::vector<u32>> paint_by_mesh(dim->meshes.count);
    for (u32 pixel_index = 0; pixel_index < dim->painted_pixels.size(); ++pixel_index) {
        PaintedPixel& pixel = dim->painted_pixels[pixel_index];
        if (id_valid(pixel.sprite_id)) continue;
        u32 mesh_slot = invalid_id;
        if (arena_has(&dim->meshes, pixel.mesh_id)) {
            const u32 candidate_slot = dim->meshes.slot_of_id[pixel.mesh_id];
            if (!topology_changed || paint_pixel_matches_mesh(dim, pixel, &dim->meshes.data[candidate_slot])) mesh_slot = candidate_slot;
        }
        if (!id_valid(mesh_slot)) {
            for (u32 slot = 0; slot < dim->meshes.count; ++slot) {
                if (!paint_pixel_matches_mesh(dim, pixel, &dim->meshes.data[slot])) continue;
                mesh_slot = slot;
                pixel.mesh_id = arena_id_at_slot(&dim->meshes, slot);
                break;
            }
        }
        if (id_valid(mesh_slot)) paint_by_mesh[mesh_slot].push_back(pixel_index);
    }

    std::vector<MeshPaintSurfaceCache> old_surfaces = std::move(renderer->mesh_paint_surfaces);
    renderer->mesh_paint_surfaces.clear();

    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    for (u32 mesh_slot = 0; mesh_slot < dim->meshes.count; ++mesh_slot) {
        const std::vector<u32>& mesh_paint = paint_by_mesh[mesh_slot];
        if (mesh_paint.empty()) continue;
        const u32 mesh_id = arena_id_at_slot(&dim->meshes, mesh_slot);
        u64 paint_hash = paint_mesh_signature(dim->meshes.data[mesh_slot], mesh_id);
        for (u32 pixel_index : mesh_paint) {
            const PaintedPixel& pixel = dim->painted_pixels[pixel_index];
            paint_hash ^= static_cast<u64>(pixel_index); paint_hash *= 1099511628211ull;
            paint_hash ^= static_cast<u64>(ColorToInt(pixel.color)); paint_hash *= 1099511628211ull;
        }
        bool reused = false;
        for (MeshPaintSurfaceCache& old : old_surfaces) {
            if (old.mesh_id != mesh_id || old.paint_hash != paint_hash || !IsTextureValid(old.texture)) continue;
            renderer->mesh_paint_surfaces.push_back(std::move(old));
            old.texture = {};
            reused = true;
        }
        if (reused) continue;
        const MeshInstance* mesh = &dim->meshes.data[mesh_slot];
        const MeshGeometry* geometry = arena_get(&dim->geometries, mesh->geometry);
        if (!geometry) continue;
        const Matrix basis = matrix_no_translation(mesh->se3);
        Vector3 vertices[max_vertices_per_geometry]{};
        for (u32 i = 0; i < geometry->vertex_count; ++i) vertices[i] = Vector3Transform(geometry->vertices[i], basis);

        std::vector<MeshPaintSurfaceCache> surfaces{};
        for (u32 tri_index = 0; tri_index < geometry->triangle_count; ++tri_index) {
            const Triangle tri = geometry->triangles[tri_index];
            const Vector3 normal = canonical_surface_normal(Vector3CrossProduct(vertices[tri.b] - vertices[tri.a], vertices[tri.c] - vertices[tri.a]));
            const float plane = Vector3DotProduct(vertices[tri.a], normal);
            auto found = surfaces.end();
            for (auto it = surfaces.begin(); it != surfaces.end(); ++it) {
                if (Vector3DotProduct(it->normal, normal) > 0.999f && std::fabs(it->plane - plane) < 0.005f) {
                    found = it;
                    break;
                }
            }
            if (found == surfaces.end()) {
                MeshPaintSurfaceCache surface{};
                surface.mesh_id = mesh_id;
                surface.geometry_id = mesh->geometry;
                surface.paint_hash = paint_hash;
                surface.normal = normal;
                surface.tangent = surface_tangent(normal);
                surface.bitangent = safe_norm(Vector3CrossProduct(normal, surface.tangent), {0.0f, 0.0f, 1.0f});
                surface.plane = plane;
                surfaces.push_back(std::move(surface));
                found = surfaces.end() - 1;
            }
            found->triangles.push_back(tri_index);
        }

        for (MeshPaintSurfaceCache& surface : surfaces) {
            float min_u = std::numeric_limits<float>::max();
            float max_u = -std::numeric_limits<float>::max();
            float min_v = std::numeric_limits<float>::max();
            float max_v = -std::numeric_limits<float>::max();
            for (u32 tri_index : surface.triangles) {
                const Triangle tri = geometry->triangles[tri_index];
                for (u32 vertex_index : {tri.a, tri.b, tri.c}) {
                    const float u = Vector3DotProduct(vertices[vertex_index], surface.tangent);
                    const float v = Vector3DotProduct(vertices[vertex_index], surface.bitangent);
                    min_u = fminf(min_u, u); max_u = fmaxf(max_u, u);
                    min_v = fminf(min_v, v); max_v = fmaxf(max_v, v);
                }
            }
            surface.grid_min_u = static_cast<i32>(std::floor(min_u * ppm));
            surface.grid_min_v = static_cast<i32>(std::floor(min_v * ppm));
            const i32 grid_max_u = static_cast<i32>(std::ceil(max_u * ppm));
            const i32 grid_max_v = static_cast<i32>(std::ceil(max_v * ppm));
            surface.width = grid_max_u - surface.grid_min_u;
            surface.height = grid_max_v - surface.grid_min_v;
            if (surface.width <= 0 || surface.height <= 0 || surface.width > 8192 || surface.height > 8192) continue;

            Image image = GenImageColor(surface.width, surface.height, BLANK);
            bool painted = false;
            for (u32 pixel_index : mesh_paint) {
                const PaintedPixel& pixel = dim->painted_pixels[pixel_index];
                if (std::fabs(Vector3DotProduct(pixel.normal, surface.normal)) < 0.99f) continue;
                const Vector3 rel = world_delta_meters(pixel.center, mesh->origin, dim->chunk_size_m);
                if (std::fabs(Vector3DotProduct(rel, surface.normal) - surface.plane) > 0.02f) continue;
                const i32 x = static_cast<i32>(std::floor(Vector3DotProduct(rel, surface.tangent) * ppm)) - surface.grid_min_u;
                const i32 y = static_cast<i32>(std::floor(Vector3DotProduct(rel, surface.bitangent) * ppm)) - surface.grid_min_v;
                if (x < 0 || y < 0 || x >= surface.width || y >= surface.height) continue;
                ImageDrawPixel(&image, x, y, pixel.color);
                painted = true;
            }
            if (painted) {
                surface.texture = LoadTextureFromImage(image);
                if (IsTextureValid(surface.texture)) {
                    GenTextureMipmaps(&surface.texture);
                    SetTextureFilter(surface.texture, TEXTURE_FILTER_ANISOTROPIC_8X);
                    SetTextureWrap(surface.texture, TEXTURE_WRAP_CLAMP);
                    renderer->mesh_paint_surfaces.push_back(std::move(surface));
                }
            }
            UnloadImage(image);
        }
    }
    for (MeshPaintSurfaceCache& old : old_surfaces) {
        if (IsTextureValid(old.texture)) UnloadTexture(old.texture);
    }
}

static void draw_painted_pixels(RenderState* renderer, const Dimension* dim, const CameraView& view) {
    if (!renderer || !dim) return;
    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    rlDrawRenderBatchActive();
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    rlDisableBackfaceCulling();
    for (const MeshPaintSurfaceCache& surface : renderer->mesh_paint_surfaces) {
        const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
        const MeshGeometry* geometry = mesh ? arena_get(&dim->geometries, surface.geometry_id) : nullptr;
        if (!mesh || !mesh->visible || !geometry || !IsTextureValid(surface.texture)) continue;
        const Vector3 origin = world_delta_meters(mesh->origin, view.anchor, dim->chunk_size_m);
        const float fade = mesh_edge_alpha(dim, safe_len(origin) / fmaxf(dim->chunk_size_m, 0.001f));
        if (fade <= 0.0f) continue;
        const Matrix basis = matrix_no_translation(mesh->se3);
        rlSetTexture(surface.texture.id);
        for (u32 tri_index : surface.triangles) {
            const Triangle tri = geometry->triangles[tri_index];
            Vector3 p[3] = {
                Vector3Transform(geometry->vertices[tri.a], basis),
                Vector3Transform(geometry->vertices[tri.b], basis),
                Vector3Transform(geometry->vertices[tri.c], basis)};
            rlBegin(RL_TRIANGLES);
                rlColor4ub(255, 255, 255, static_cast<u8>(std::round(255.0f * fade)));
                for (Vector3 local : p) {
                    const float u = (Vector3DotProduct(local, surface.tangent) * ppm - static_cast<float>(surface.grid_min_u)) / static_cast<float>(surface.width);
                    const float v = (Vector3DotProduct(local, surface.bitangent) * ppm - static_cast<float>(surface.grid_min_v)) / static_cast<float>(surface.height);
                    const Vector3 point = local + origin;
                    rlTexCoord2f(u, v); rlVertex3f(point.x, point.y, point.z);
                }
            rlEnd();
        }
        rlSetTexture(0);
    }
    rlEnableBackfaceCulling();
    rlDrawRenderBatchActive();
    glDisable(GL_POLYGON_OFFSET_FILL);
}

static void draw_crosshair(RenderState* renderer) {
    const Vector2 center = {static_cast<float>(renderer->native_w) * 0.5f, static_cast<float>(renderer->native_h) * 0.5f};
    DrawCircleV(center, 3.0f, Color{0, 0, 0, 135});
    DrawCircleV(center, 1.5f, Color{255, 255, 255, 230});
}

void renderer_render_dimension_to_target(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id) {
    if (!renderer->target_ready) renderer_ensure_target(renderer);
    renderer->debug_edge_count = 0;
    renderer->debug_scene_triangle_count = 0;
    renderer->debug_unbounded_scene_triangle_count = 0;
    renderer->debug_edge_sample_count = 0;
    renderer->debug_edge_candidate_count = 0;
    renderer->debug_edge_ray_test_count = 0;

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
    const float near_plane = static_cast<float>(rlGetCullDistanceNear());
    const u32 edge_cap = max_render_edges;
    const bool use_compute_depth_edges =
        renderer->depth_test_edges &&
        renderer->gpu_depth_edges &&
        renderer->edge_filter_compute_ready &&
        renderer->edge_scene_depth_texture_ready &&
        renderer->edge_depth_texture_ready;

    std::vector<VisibleChunkRef> visible_chunks;
    visible_chunks.reserve(dim->chunks.count);
    std::vector<MeshPrepJob> mesh_jobs;
    mesh_jobs.reserve(dim->meshes.count);
    for (u32 chunk_slot = 0; chunk_slot < dim->chunks.count; ++chunk_slot) {
        const Chunk* chunk = &dim->chunks.data[chunk_slot];
        float chunk_dist = 0.0f;
        if (!chunk_visible(dim, view.anchor.chunk, chunk->coord, &chunk_dist)) continue;
        visible_chunks.push_back({chunk, chunk_dist});
        for (u32 i = 0; i < chunk->mesh_count; ++i) {
            mesh_jobs.push_back({arena_get(&dim->meshes, chunk->meshes[i]), chunk_dist});
        }
    }

    const bool collect_scene_triangles = renderer->depth_test_edges;
    const bool use_parallel_mesh_prep = mesh_jobs.size() >= parallel_mesh_prep_min_jobs;
    PreparedRenderBatch prepared{};
    if (use_parallel_mesh_prep) {
        prepared = prepare_mesh_jobs_parallel(
            renderer,
            dim,
            view,
            camera,
            view_matrix,
            projection_matrix,
            near_plane,
            collect_scene_triangles,
            edge_cap,
            mesh_jobs);
    }

    for (u32 slot = 0; slot < dim->sprites.count; ++slot) {
        const u32 sprite_id = arena_id_at_slot(&dim->sprites, slot);
        const SpriteInstance* sprite = &dim->sprites.data[slot];
        Color ignored_tint{};
        painted_sprite_texture(renderer, dim, sprite_id, sprite, &ignored_tint);
    }
    update_mesh_paint_surfaces(renderer, dim);

    BeginTextureMode(renderer->target);
    ClearBackground(dim->sky_top);
    DrawRectangleGradientV(0, 0, renderer->native_w, renderer->native_h, dim->sky_top, dim->sky_bottom);
    BeginMode3D(camera);

    ScreenEdgeList edge_list = use_parallel_mesh_prep ? std::move(prepared.edge_list) : ScreenEdgeList{};
    edge_list.max_count = edge_cap;
    if (!use_parallel_mesh_prep) edge_list.edges.reserve(edge_cap);
    SceneTriangleList scene_triangles{};
    scene_triangles.screen_w = renderer->native_w;
    scene_triangles.screen_h = renderer->native_h;
    if (use_parallel_mesh_prep) {
        size_t prepared_scene_triangle_count = 0;
        for (const PreparedRenderBuffer& buffer : prepared.buffers) {
            prepared_scene_triangle_count += buffer.scene_triangles.size();
        }
        scene_triangles.triangles.reserve(prepared_scene_triangle_count);
        for (const PreparedRenderBuffer& buffer : prepared.buffers) {
            for (const SceneTriangle& tri : buffer.scene_triangles) {
                append_scene_triangle(&scene_triangles, tri);
            }
        }
        draw_prepared_triangles(renderer, prepared);
    } else {
        scene_triangles.triangles.reserve(2048);
        SceneTriangleList* scene_triangle_target = collect_scene_triangles ? &scene_triangles : nullptr;
        for (const VisibleChunkRef& visible : visible_chunks) {
            const Chunk* chunk = visible.chunk;
            if (!chunk) continue;
            for (u32 i = 0; i < chunk->mesh_count; ++i) {
                draw_mesh_instance_direct(renderer, dim, view, camera, view_matrix, projection_matrix, near_plane, &edge_list, scene_triangle_target, arena_get(&dim->meshes, chunk->meshes[i]), visible.chunk_dist);
            }
        }
    }
    renderer->debug_edge_count = edge_list.count;
    renderer->debug_scene_triangle_count = static_cast<u32>(scene_triangles.triangles.size());
    renderer->debug_unbounded_scene_triangle_count = static_cast<u32>(scene_triangles.unbounded.size());

    draw_painted_pixels(renderer, dim, view);

    PlayerNameTag hovered_tag{};
    draw_players(renderer, dim, view, local_player_id, camera, view_matrix, projection_matrix, near_plane, &edge_list, &scene_triangles, &hovered_tag);
    renderer->debug_edge_count = edge_list.count;
    renderer->debug_scene_triangle_count = static_cast<u32>(scene_triangles.triangles.size());
    renderer->debug_unbounded_scene_triangle_count = static_cast<u32>(scene_triangles.unbounded.size());

    SpriteDepthOcclusion sprite_occlusion{};
    if (renderer->depth_test_edges) {
        if (use_compute_depth_edges) {
            copy_current_depth_to_texture(renderer, renderer->edge_scene_depth_texture, renderer->edge_scene_depth_texture_ready);
        } else {
            sprite_occlusion.scene = read_current_depth_buffer(renderer->native_w, renderer->native_h);
        }
    }

    for (const VisibleChunkRef& visible : visible_chunks) {
        draw_sprites(renderer, dim, view, camera, visible.chunk);
    }

    if (renderer->depth_test_edges) {
        if (use_compute_depth_edges) {
            copy_current_depth_to_texture(renderer, renderer->edge_depth_texture, renderer->edge_depth_texture_ready);
        } else {
            sprite_occlusion.after_sprites = read_current_depth_buffer(renderer->native_w, renderer->native_h);
        }
    }

    draw_player_aim_rays(renderer, dim, view, local_player_id, camera);
    draw_physics(renderer, dim, view);

    EndMode3D();

    if (renderer->depth_test_edges) {
        ScreenEdgeList filtered_edges{};
        filtered_edges.max_count = edge_list.max_count;
        bool filtered = false;
        if (use_compute_depth_edges) {
            filtered = filter_depth_cut_screen_edges_compute(renderer, &edge_list, scene_triangles, camera.position, &filtered_edges);
        }
        if (!filtered) {
            filter_depth_cut_screen_edges(renderer, &edge_list, sprite_occlusion, scene_triangles, camera.position, &filtered_edges);
        }
        renderer->debug_edge_count = filtered_edges.count;
        draw_flat_screen_edges(&filtered_edges);
    } else {
        draw_flat_screen_edges(&edge_list);
    }
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
