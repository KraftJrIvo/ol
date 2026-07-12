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
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ol {

static void shutdown_radiance_background(RenderState* renderer);

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

static Shader load_embedded_fragment_shader(const unsigned char* data, unsigned long long length, bool* out_ready) {
    if (out_ready) *out_ready = false;
    if (!data || length == 0) return {};
    const std::string source(reinterpret_cast<const char*>(data), static_cast<size_t>(length));
    Shader shader = LoadShaderFromMemory(nullptr, source.c_str());
    if (IsShaderValid(shader) && shader.id != rlGetShaderIdDefault() && out_ready) *out_ready = true;
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

static void set_pixel_mipmap_filter(Texture2D texture) {
    if (!IsTextureValid(texture)) return;
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
        texture.mipmaps > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (GLAD_GL_EXT_texture_filter_anisotropic) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
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
        set_pixel_mipmap_filter(texture);
        SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    }
    return texture;
}

static void unload_color_render_texture(RenderTexture2D* target) {
    if (!target || !target->id) {
        if (target) *target = {};
        return;
    }
    // load_color_render_texture() uses depth as a metadata alias only.  Avoid
    // UnloadRenderTexture() here: it queries framebuffer attachments before
    // deleting them, which turns mass light-map retirement into a GPU/CPU
    // synchronization point at streamed chunk boundaries.
    if (target->texture.id && target->depth.id == target->texture.id) {
        rlUnloadTexture(target->texture.id);
        // rlUnloadFramebuffer() performs depth-attachment glGet calls even for
        // this known colour-only FBO.  Delete it directly after returning to
        // the default target so retirement never introduces that readback.
        rlDisableFramebuffer();
        const GLuint framebuffer = target->id;
        glDeleteFramebuffers(1, &framebuffer);
    } else {
        UnloadRenderTexture(*target);
    }
    *target = {};
}

static RenderTexture2D load_color_render_texture(int width, int height, int format) {
    RenderTexture2D target{};
    target.id = rlLoadFramebuffer();
    if (!target.id) return target;
    rlEnableFramebuffer(target.id);
    target.texture.id = rlLoadTexture(nullptr, width, height, format, 1);
    target.texture.width = width;
    target.texture.height = height;
    target.texture.format = format;
    target.texture.mipmaps = 1;
    rlFramebufferAttach(target.id, target.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    const bool complete = rlFramebufferComplete(target.id);
    rlDisableFramebuffer();
    if (!complete) {
        if (target.texture.id) rlUnloadTexture(target.texture.id);
        const GLuint framebuffer = target.id;
        glDeleteFramebuffers(1, &framebuffer);
        return {};
    }
    // raylib 5.x defines IsRenderTextureValid() as requiring a populated
    // `depth` descriptor even when the framebuffer itself is complete without
    // a depth attachment.  Keep a metadata alias so generic render-target
    // lifetime/validity code can handle this colour-only target.  The alias is
    // not attached or separately owned; UnloadRenderTexture() queries the FBO
    // attachments and unloads only the real colour texture.
    target.depth = target.texture;
    return target;
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
        "noperspective out float fragEdgeDepth;\n"
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
        "noperspective in float fragEdgeDepth;\n"
        "uniform sampler2D depthTexture;\n"
        "uniform float depthBias;\n"
        "out vec4 finalColor;\n"
        "void main() {\n"
        "    ivec2 size = textureSize(depthTexture, 0);\n"
        "    ivec2 center = ivec2(clamp(gl_FragCoord.xy, vec2(0.0), vec2(size - ivec2(1))));\n"
        "    float sceneDepth = texelFetch(depthTexture, center, 0).r;\n"
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
    renderer->shader_uniform_locations.clear();
    renderer->radiance_common_uniform_signatures.clear();
    renderer->radiance_bound_scene_buffers.fill(invalid_id);
    // A RenderState normally lives for the whole process, but make a clean
    // shutdown/init cycle return to the normal RC renderer as well.
    renderer->pathtrace_comparison_enabled = false;
    renderer->pathtrace_comparison_failed = false;
    renderer->pathtrace_dimension = invalid_id;
    renderer->pathtrace_anchor_chunk = {};
    renderer->pathtrace_render_radius_chunks = -1;
    renderer->pathtrace_quality_radius_chunks = -1;
    renderer->pathtrace_topology_revision = 0;
    renderer->pathtrace_scene_signature = 0;
    renderer->pathtrace_triangle_count = 0;
    renderer->pathtrace_emitter_triangle_count = 0;
    renderer->pathtrace_emitter_mesh_count = 0;
    renderer->pathtrace_bvh_node_count = 0;
    renderer->pathtrace_dynamic_triangle_count = 0;
    renderer->pathtrace_dynamic_bvh_node_count = 0;
    renderer->pathtrace_emitter_total_weight = 0.0f;
    renderer->pathtrace_scene_build_count = 0;
    renderer->pathtrace_scene_truncated = false;
    renderer->debug_pathtrace_scene_build_ms = 0.0;
    renderer->debug_pathtrace_scene_update_ms = 0.0;
    renderer->debug_pathtrace_draw_ms = 0.0;
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
    renderer->radiance_cascade_shader = load_embedded_fragment_shader(
        res_radiance_cascade_frag, res_radiance_cascade_frag_len, &renderer->radiance_cascade_shader_ready);
    renderer->surface_lighting_shader = load_embedded_fragment_shader(
        res_surface_lighting_frag, res_surface_lighting_frag_len, &renderer->surface_lighting_shader_ready);
    renderer->surface_shadow_composite_shader = load_embedded_fragment_shader(
        res_surface_shadow_composite_frag, res_surface_shadow_composite_frag_len,
        &renderer->surface_shadow_composite_shader_ready);
    renderer->pathtrace_comparison_shader = load_embedded_fragment_shader(
        res_pathtrace_comparison_frag, res_pathtrace_comparison_frag_len,
        &renderer->pathtrace_comparison_shader_ready);
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
    shutdown_radiance_background(renderer);
    for (SpritePaintTextureCache& cache : renderer->sprite_paint_textures) {
        if (IsTextureValid(cache.texture)) UnloadTexture(cache.texture);
    }
    renderer->sprite_paint_textures.clear();
    for (MeshPaintSurfaceCache& cache : renderer->mesh_paint_surfaces) {
        if (IsTextureValid(cache.texture)) UnloadTexture(cache.texture);
    }
    renderer->mesh_paint_surfaces.clear();
    for (MeshLightingSurfaceCache& cache : renderer->lighting_surfaces) {
        unload_color_render_texture(&cache.texture);
        unload_color_render_texture(&cache.shadow_mask_texture);
        unload_color_render_texture(&cache.history_texture);
    }
    renderer->lighting_surfaces.clear();
    for (RenderTexture2D& target : renderer->radiance_cascade_targets) {
        unload_color_render_texture(&target);
    }
    if (renderer->radiance_scene_triangles_ssbo) rlUnloadShaderBuffer(renderer->radiance_scene_triangles_ssbo);
    if (renderer->radiance_emitters_ssbo) rlUnloadShaderBuffer(renderer->radiance_emitters_ssbo);
    if (renderer->radiance_bvh_nodes_ssbo) rlUnloadShaderBuffer(renderer->radiance_bvh_nodes_ssbo);
    if (renderer->radiance_dynamic_triangles_ssbo) rlUnloadShaderBuffer(renderer->radiance_dynamic_triangles_ssbo);
    if (renderer->radiance_dynamic_bvh_nodes_ssbo) rlUnloadShaderBuffer(renderer->radiance_dynamic_bvh_nodes_ssbo);
    if (renderer->pathtrace_scene_triangles_ssbo) rlUnloadShaderBuffer(renderer->pathtrace_scene_triangles_ssbo);
    if (renderer->pathtrace_emitters_ssbo) rlUnloadShaderBuffer(renderer->pathtrace_emitters_ssbo);
    if (renderer->pathtrace_bvh_nodes_ssbo) rlUnloadShaderBuffer(renderer->pathtrace_bvh_nodes_ssbo);
    if (renderer->pathtrace_dynamic_triangles_ssbo) rlUnloadShaderBuffer(renderer->pathtrace_dynamic_triangles_ssbo);
    if (renderer->pathtrace_dynamic_bvh_nodes_ssbo) rlUnloadShaderBuffer(renderer->pathtrace_dynamic_bvh_nodes_ssbo);
    renderer->radiance_scene_triangles_ssbo = 0;
    renderer->radiance_emitters_ssbo = 0;
    renderer->radiance_bvh_nodes_ssbo = 0;
    renderer->radiance_dynamic_triangles_ssbo = 0;
    renderer->radiance_dynamic_bvh_nodes_ssbo = 0;
    renderer->pathtrace_scene_triangles_ssbo = 0;
    renderer->pathtrace_emitters_ssbo = 0;
    renderer->pathtrace_bvh_nodes_ssbo = 0;
    renderer->pathtrace_dynamic_triangles_ssbo = 0;
    renderer->pathtrace_dynamic_bvh_nodes_ssbo = 0;
    renderer->radiance_scene_triangles_capacity = 0;
    renderer->radiance_emitters_capacity = 0;
    renderer->radiance_bvh_nodes_capacity = 0;
    renderer->radiance_dynamic_triangles_capacity = 0;
    renderer->radiance_dynamic_bvh_nodes_capacity = 0;
    renderer->pathtrace_scene_triangles_capacity = 0;
    renderer->pathtrace_emitters_capacity = 0;
    renderer->pathtrace_bvh_nodes_capacity = 0;
    renderer->pathtrace_dynamic_triangles_capacity = 0;
    renderer->pathtrace_dynamic_bvh_nodes_capacity = 0;
    if (IsRenderTextureValid(renderer->pathtrace_comparison_target)) {
        UnloadRenderTexture(renderer->pathtrace_comparison_target);
        renderer->pathtrace_comparison_target = {};
    }
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
    if (renderer->radiance_cascade_shader_ready) {
        UnloadShader(renderer->radiance_cascade_shader);
        renderer->radiance_cascade_shader_ready = false;
    }
    if (renderer->surface_lighting_shader_ready) {
        UnloadShader(renderer->surface_lighting_shader);
        renderer->surface_lighting_shader_ready = false;
    }
    if (renderer->surface_shadow_composite_shader_ready) {
        UnloadShader(renderer->surface_shadow_composite_shader);
        renderer->surface_shadow_composite_shader_ready = false;
    }
    if (renderer->pathtrace_comparison_shader_ready) {
        UnloadShader(renderer->pathtrace_comparison_shader);
        renderer->pathtrace_comparison_shader_ready = false;
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
    renderer->shader_uniform_locations.clear();
    renderer->radiance_common_uniform_signatures.clear();
    renderer->radiance_bound_scene_buffers.fill(invalid_id);
}

void renderer_change_scale(RenderState* renderer, int delta) {
    renderer->scale_power += delta;
    if (renderer->scale_power < 0) renderer->scale_power = 0;
    if (renderer->scale_power > 4) renderer->scale_power = 4;
}

bool renderer_toggle_pathtrace_comparison(RenderState* renderer) {
    if (!renderer || !renderer->pathtrace_comparison_shader_ready) return false;
    renderer->pathtrace_comparison_failed = false;
    renderer->pathtrace_comparison_enabled =
        !renderer->pathtrace_comparison_enabled;
    return renderer->pathtrace_comparison_enabled;
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

static bool mesh_intersects_camera_frustum(
    const MeshInstance& mesh,
    const Dimension& dim,
    const CameraView& view,
    const Camera3D& camera,
    int viewport_width,
    int viewport_height) {
    const Vector3 center = world_delta_meters(mesh.origin, view.anchor, dim.chunk_size_m);
    const Vector3 forward = safe_norm(
        camera.target - camera.position, {0.0f, 0.0f, -1.0f});
    const Vector3 right = safe_norm(
        Vector3CrossProduct(forward, camera.up), {1.0f, 0.0f, 0.0f});
    const Vector3 up = safe_norm(
        Vector3CrossProduct(right, forward), {0.0f, 1.0f, 0.0f});
    const Vector3 relative = center - camera.position;
    const float depth = Vector3DotProduct(relative, forward);
    const float horizontal = Vector3DotProduct(relative, right);
    const float vertical = Vector3DotProduct(relative, up);
    // Mesh bounds are authored in world metres.  A small guard band keeps
    // pixel-thick contours at the viewport boundary from being culled by
    // floating-point roundoff.
    const float radius = fmaxf(0.01f, mesh.bounds_radius) + 0.25f;
    const float near_plane = static_cast<float>(rlGetCullDistanceNear());
    const float far_plane = static_cast<float>(rlGetCullDistanceFar());
    if (depth + radius < near_plane || depth - radius > far_plane) return false;

    const float tan_vertical = std::tan(camera.fovy * DEG2RAD * 0.5f);
    const float aspect = static_cast<float>(std::max(1, viewport_width)) /
        static_cast<float>(std::max(1, viewport_height));
    const float tan_horizontal = tan_vertical * aspect;
    const float horizontal_guard = radius * std::sqrt(1.0f + tan_horizontal * tan_horizontal);
    const float vertical_guard = radius * std::sqrt(1.0f + tan_vertical * tan_vertical);
    if (std::fabs(horizontal) > depth * tan_horizontal + horizontal_guard) return false;
    if (std::fabs(vertical) > depth * tan_vertical + vertical_guard) return false;
    return true;
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
    (void)view;
    (void)face_center;
    (void)normal;
    return dim ? dim->ambient : 1.0f;
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

constexpr size_t parallel_mesh_prep_min_jobs = 1024;

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
}

static void draw_shader_depth_screen_edges(RenderState* renderer, const ScreenEdgeList* list, bool hardware_depth_test) {
    if (!renderer || !list || list->count == 0 || !renderer->edge_depth_shader_ready || !renderer->edge_depth_texture_ready) return;

    if (hardware_depth_test) rlEnableDepthTest();
    else rlDisableDepthTest();
    rlDisableDepthMask();

    BeginShaderMode(renderer->edge_depth_shader);
    SetShaderValueTexture(renderer->edge_depth_shader, renderer->edge_depth_texture_loc, renderer->edge_depth_texture);
    SetShaderValue(renderer->edge_depth_shader, renderer->edge_depth_bias_loc, &renderer->edge_depth_bias, SHADER_UNIFORM_FLOAT);
    rlBegin(RL_TRIANGLES);
    for (u32 i = 0; i < list->count; ++i) {
        draw_shader_depth_edge_quad(list->edges[i]);
    }
    rlEnd();
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

static void surface_texture_basis(Vector3 normal, Vector3* out_tangent, Vector3* out_bitangent) {
    normal = safe_norm(normal, {0.0f, 1.0f, 0.0f});
    const Vector3 abs_n = {std::fabs(normal.x), std::fabs(normal.y), std::fabs(normal.z)};
    Vector3 tangent_seed{};
    Vector3 bitangent_seed{};
    if (abs_n.y >= abs_n.x && abs_n.y >= abs_n.z) {
        tangent_seed = {1.0f, 0.0f, 0.0f};
        bitangent_seed = {0.0f, 0.0f, normal.y >= 0.0f ? 1.0f : -1.0f};
    } else if (abs_n.x >= abs_n.z) {
        tangent_seed = {0.0f, 0.0f, normal.x >= 0.0f ? 1.0f : -1.0f};
        bitangent_seed = {0.0f, -1.0f, 0.0f};
    } else {
        tangent_seed = {normal.z >= 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
        bitangent_seed = {0.0f, -1.0f, 0.0f};
    }
    Vector3 tangent = tangent_seed - normal * Vector3DotProduct(tangent_seed, normal);
    tangent = safe_norm(tangent, safe_norm(Vector3CrossProduct({0.0f, 1.0f, 0.0f}, normal), {1.0f, 0.0f, 0.0f}));
    Vector3 bitangent = bitangent_seed - normal * Vector3DotProduct(bitangent_seed, normal);
    bitangent -= tangent * Vector3DotProduct(bitangent, tangent);
    bitangent = safe_norm(bitangent, safe_norm(Vector3CrossProduct(normal, tangent), {0.0f, 0.0f, 1.0f}));
    if (out_tangent) *out_tangent = tangent;
    if (out_bitangent) *out_bitangent = bitangent;
}

static Vector2 planar_texture_uv(Vector3 point, Vector3 origin, Vector3 normal, float pixels_per_meter, Texture2D texture) {
    const Vector3 rel = point - origin;
    Vector3 tangent{};
    Vector3 bitangent{};
    surface_texture_basis(normal, &tangent, &bitangent);
    const float tex_w = static_cast<float>(texture.width > 0 ? texture.width : 1);
    const float tex_h = static_cast<float>(texture.height > 0 ? texture.height : 1);
    return {
        0.5f + Vector3DotProduct(rel, tangent) * pixels_per_meter / tex_w,
        0.5f + Vector3DotProduct(rel, bitangent) * pixels_per_meter / tex_h
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
    const float ff = renderer->pathtrace_comparison_enabled
        ? 0.0f : fog_factor(dim, chunk_dist);
    const bool collect_mesh_scene_triangles = collect_scene_triangles && mesh_edges_in_render_radius(dim, mesh_dist_chunks);

    for (u32 i = 0; i < geometry->triangle_count; ++i) {
        const Triangle tri = geometry->triangles[i];
        const Vector3 a = transformed[tri.a];
        const Vector3 b = transformed[tri.b];
        const Vector3 c = transformed[tri.c];
        const Vector3 n = safe_norm(Vector3CrossProduct(b - a, c - a));
        Color shaded = mesh->lit && !renderer->pathtrace_comparison_enabled
            ? color_scaled(base, face_light(dim, view, origin, n)) : base;
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
    const float ff = renderer->pathtrace_comparison_enabled
        ? 0.0f : fog_factor(dim, chunk_dist);
    const bool collect_mesh_scene_triangles = scene_triangles && mesh_edges_in_render_radius(dim, mesh_dist_chunks);

    for (u32 i = 0; i < geometry->triangle_count; ++i) {
        const Triangle tri = geometry->triangles[i];
        const Vector3 a = transformed[tri.a];
        const Vector3 b = transformed[tri.b];
        const Vector3 c = transformed[tri.c];
        const Vector3 n = safe_norm(Vector3CrossProduct(b - a, c - a));
        Color shaded = mesh->lit && !renderer->pathtrace_comparison_enabled
            ? color_scaled(base, face_light(dim, view, origin, n)) : base;
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
    u32 active_texture = 0;
    bool drawing = false;
    for (const PreparedTriangle& tri : triangles) {
        const Texture2D texture = render_texture(renderer, tri.texture_id);
        if (!drawing || texture.id != active_texture) {
            if (drawing) {
                rlEnd();
                rlSetTexture(0);
            }
            active_texture = texture.id;
            rlSetTexture(active_texture);
            rlBegin(RL_TRIANGLES);
            drawing = true;
        }
        rlColor4ub(tri.color.r, tri.color.g, tri.color.b, tri.color.a);
        rlTexCoord2f(tri.uv_a.x, tri.uv_a.y); rlVertex3f(tri.a.x, tri.a.y, tri.a.z);
        rlTexCoord2f(tri.uv_b.x, tri.uv_b.y); rlVertex3f(tri.b.x, tri.b.y, tri.b.z);
        rlTexCoord2f(tri.uv_c.x, tri.uv_c.y); rlVertex3f(tri.c.x, tri.c.y, tri.c.z);
    }
    if (drawing) {
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
                set_pixel_mipmap_filter(cache_at->texture);
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

    if (!renderer->pathtrace_comparison_enabled) {
        draw_name_tag_billboard(renderer, camera, hovered_tag);
    }
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

}

static Vector3 canonical_surface_normal(Vector3 normal) {
    normal = safe_norm(normal, {0.0f, 1.0f, 0.0f});
    const Vector3 a = {std::fabs(normal.x), std::fabs(normal.y), std::fabs(normal.z)};
    const float dominant = a.x >= a.y && a.x >= a.z ? normal.x : (a.y >= a.z ? normal.y : normal.z);
    return dominant < 0.0f ? normal * -1.0f : normal;
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

static void update_mesh_paint_surfaces(
    RenderState* renderer,
    Dimension* dim,
    const CameraView& view) {
    if (!renderer || !dim) return;
    u64 topology_hash = 1469598103934665603ull;
    u32 ppm_bits = 0;
    std::memcpy(&ppm_bits, &dim->pixels_per_meter, sizeof(ppm_bits));
    topology_hash ^= ppm_bits;
    topology_hash *= 1099511628211ull;
    // The common unpainted case must not hash every streamed mesh each frame.
    // Hills can keep tens of thousands of far meshes at a large render radius,
    // while paint topology is irrelevant until at least one world pixel exists.
    if (dim->painted_pixels.empty()) {
        if (!renderer->mesh_paint_surfaces.empty()) {
            for (MeshPaintSurfaceCache& surface : renderer->mesh_paint_surfaces) {
                if (IsTextureValid(surface.texture)) UnloadTexture(surface.texture);
            }
            renderer->mesh_paint_surfaces.clear();
        }
        renderer->mesh_paint_revision = dim->paint_revision;
        renderer->mesh_paint_topology_hash = topology_hash;
        return;
    }
    auto add_topology = [&](u64 value) {
        topology_hash ^= value;
        topology_hash *= 1099511628211ull;
    };
    std::array<u64, (max_meshes + 63u) / 64u> referenced_mesh_ids{};
    bool unresolved_near_camera = false;
    for (const PaintedPixel& pixel : dim->painted_pixels) {
        if (id_valid(pixel.sprite_id)) continue;
        const u32 mesh_id = pixel.mesh_id;
        if (mesh_id < max_meshes) {
            const u32 word = mesh_id / 64u;
            const u64 bit = 1ull << (mesh_id % 64u);
            if (referenced_mesh_ids[word] & bit) continue;
            referenced_mesh_ids[word] |= bit;
        }
        add_topology(mesh_id);
        if (const MeshInstance* mesh = arena_get(&dim->meshes, mesh_id)) {
            add_topology(paint_mesh_signature(*mesh, mesh_id));
        } else {
            const float distance_chunks = safe_len(world_delta_meters(
                pixel.center, view.anchor, dim->chunk_size_m)) /
                fmaxf(dim->chunk_size_m, 0.001f);
            unresolved_near_camera = unresolved_near_camera ||
                distance_chunks <= static_cast<float>(dim->render_radius_chunks + 2);
        }
    }
    // Retry reassociation only while an unresolved painted surface is close
    // enough to be streamed. Far chunk churn must not invalidate an unrelated
    // painted mesh cache every frame.
    if (unresolved_near_camera) add_topology(dim->mesh_topology_revision);
    if (renderer->mesh_paint_revision == dim->paint_revision && renderer->mesh_paint_topology_hash == topology_hash) return;
    const bool topology_changed = renderer->mesh_paint_topology_hash != topology_hash;
    renderer->mesh_paint_revision = dim->paint_revision;
    renderer->mesh_paint_topology_hash = topology_hash;

    struct PaintMeshGroup {
        u32 mesh_slot = invalid_id;
        std::vector<u32> pixels{};
    };
    std::vector<PaintMeshGroup> paint_groups{};
    paint_groups.reserve(std::min<size_t>(dim->painted_pixels.size(), 64u));
    std::unordered_map<u32, size_t> paint_group_by_slot{};
    paint_group_by_slot.reserve(paint_groups.capacity());
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
        if (id_valid(mesh_slot)) {
            auto [group_it, inserted] = paint_group_by_slot.emplace(
                mesh_slot, paint_groups.size());
            if (inserted) paint_groups.push_back({mesh_slot, {}});
            paint_groups[group_it->second].pixels.push_back(pixel_index);
        }
    }

    std::vector<MeshPaintSurfaceCache> old_surfaces = std::move(renderer->mesh_paint_surfaces);
    renderer->mesh_paint_surfaces.clear();

    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    for (const PaintMeshGroup& group : paint_groups) {
        const u32 mesh_slot = group.mesh_slot;
        const std::vector<u32>& mesh_paint = group.pixels;
        const u32 mesh_id = arena_id_at_slot(&dim->meshes, mesh_slot);
        u64 paint_hash = paint_mesh_signature(dim->meshes.data[mesh_slot], mesh_id);
        paint_hash ^= ppm_bits;
        paint_hash *= 1099511628211ull;
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
                surface_texture_basis(normal, &surface.tangent, &surface.bitangent);
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
                    set_pixel_mipmap_filter(surface.texture);
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

struct alignas(16) GpuRadianceTriangle {
    Vector4 a{};
    Vector4 b{};
    Vector4 c{};
    Vector4 albedo_reflectivity{};
    Vector4 emission{};
};

struct alignas(16) GpuPathtraceTriangle {
    Vector4 a{};
    Vector4 b{};
    Vector4 c{};
    Vector4 albedo_reflectivity{};
    Vector4 emission{};
    // Authored planar UVs are carried with the lighting geometry so the
    // full-frame reference can sample the same material textures on every
    // primary and secondary path. uv_c_texture.z stores the engine texture id.
    Vector4 uv_a_b{};
    Vector4 uv_c_texture{};
};

struct alignas(16) GpuRadianceEmitter {
    Vector4 a_cdf{};
    Vector4 b_weight{};
    Vector4 c_area{};
    Vector4 emission{};
    Vector4 surface_center_area{};
    // xyz is the constant coplanar normal. w is the contiguous triangle
    // count on the first record of a surface and zero on its remaining rows.
    Vector4 surface_normal_meta{};
};


struct alignas(16) GpuRadianceBvhNode {
    Vector4 bounds_min{};
    Vector4 bounds_max{};
    std::array<u32, 4> meta{};
};

static_assert(sizeof(GpuRadianceTriangle) == 80);
static_assert(sizeof(GpuPathtraceTriangle) == 112);
static_assert(sizeof(GpuRadianceEmitter) == 96);
static_assert(sizeof(GpuRadianceBvhNode) == 48);


static u64 radiance_hash_add(u64 hash, u64 value) {
    hash ^= value;
    return hash * 1099511628211ull;
}

static u64 radiance_hash_float(u64 hash, float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return radiance_hash_add(hash, bits);
}

static u64 radiance_hash_world_pos(u64 hash, const WorldPos& position) {
    hash = radiance_hash_add(hash, position.dimension);
    hash = radiance_hash_add(hash, static_cast<u32>(position.chunk.x));
    hash = radiance_hash_add(hash, static_cast<u32>(position.chunk.y));
    hash = radiance_hash_add(hash, static_cast<u32>(position.chunk.z));
    hash = radiance_hash_float(hash, position.local.x);
    hash = radiance_hash_float(hash, position.local.y);
    return radiance_hash_float(hash, position.local.z);
}

static u64 radiance_mesh_signature(const MeshInstance& mesh, u32 mesh_id) {
    u64 hash = paint_mesh_signature(mesh, mesh_id);
    hash = radiance_hash_add(hash, mesh.texture_id);
    const Color color = resolve_mesh_color(&mesh);
    hash = radiance_hash_add(hash, ColorToInt(color));
    const Vector3 emission = resolve_mesh_emission(&mesh);
    hash = radiance_hash_float(hash, emission.x);
    hash = radiance_hash_float(hash, emission.y);
    hash = radiance_hash_float(hash, emission.z);
    hash = radiance_hash_float(hash, resolve_mesh_reflectivity(&mesh));
    hash = radiance_hash_add(hash, mesh.visible ? 1u : 0u);
    hash = radiance_hash_add(hash, mesh.lit ? 1u : 0u);
    return hash;
}

static u64 radiance_static_mesh_signature(const MeshInstance& mesh, u32 mesh_id) {
    const Vector3 emission = resolve_mesh_emission(&mesh);
    if (safe_len(emission) <= 0.0001f) return radiance_mesh_signature(mesh, mesh_id);

    // Light meshes live in the small emitter buffer and never enter the
    // occluding BVH. Their animated world position therefore must not make the
    // static scene identity (and every surface cache) dirty each fixed tick.
    // Shape/material edits remain part of the identity; crossing the emissive
    // threshold also switches back to the full static-mesh signature above.
    u64 hash = 1469598103934665603ull;
    hash = radiance_hash_add(hash, mesh_id);
    hash = radiance_hash_add(hash, mesh.geometry);
    hash = radiance_hash_add(hash, mesh.texture_id);
    hash = radiance_hash_add(hash, ColorToInt(resolve_mesh_color(&mesh)));
    const float transform[] = {
        mesh.se3.m0, mesh.se3.m1, mesh.se3.m2, mesh.se3.m4, mesh.se3.m5, mesh.se3.m6,
        mesh.se3.m8, mesh.se3.m9, mesh.se3.m10};
    for (float value : transform) hash = radiance_hash_float(hash, value);
    hash = radiance_hash_float(hash, emission.x);
    hash = radiance_hash_float(hash, emission.y);
    hash = radiance_hash_float(hash, emission.z);
    hash = radiance_hash_float(hash, resolve_mesh_reflectivity(&mesh));
    hash = radiance_hash_add(hash, mesh.visible ? 1u : 0u);
    return radiance_hash_add(hash, mesh.lit ? 1u : 0u);
}

static WorldPos radiance_anchor(const Dimension* dim, const CameraView& view) {
    WorldPos anchor{};
    anchor.dimension = view.anchor.dimension;
    anchor.chunk = view.anchor.chunk;
    anchor.local = {};
    canonicalize(&anchor, dim->chunk_size_m);
    return anchor;
}

static float chunk_distance(ChunkCoord a, ChunkCoord b) {
    const float x = static_cast<float>(a.x - b.x);
    const float y = static_cast<float>(a.y - b.y);
    const float z = static_cast<float>(a.z - b.z);
    return std::sqrt(x*x + y*y + z*z);
}

template <typename Callback>
static void for_each_radiance_chunk(
    const Dimension* dim,
    ChunkCoord anchor,
    float radius,
    Callback&& callback) {
    if (!dim || radius < 0.0f) return;
    const i32 extent = static_cast<i32>(std::ceil(radius));
    const float radius_squared = radius * radius;
    // Coordinate order is independent of arena slot reuse, so lighting-surface
    // presentation remains deterministic after streamed chunks unload/reload.
    for (i32 z = -extent; z <= extent; ++z) {
        for (i32 y = -extent; y <= extent; ++y) {
            for (i32 x = -extent; x <= extent; ++x) {
                const float distance_squared = static_cast<float>(x*x + y*y + z*z);
                if (distance_squared > radius_squared) continue;
                const ChunkCoord coord{anchor.x + x, anchor.y + y, anchor.z + z};
                const u32 chunk_id = dimension_find_chunk(dim, coord);
                const Chunk* chunk = arena_get(&dim->chunks, chunk_id);
                if (chunk) callback(*chunk);
            }
        }
    }
}

static float world_distance_chunks(const Dimension* dim, WorldPos a, WorldPos b) {
    if (!dim) return 0.0f;
    return safe_len(world_delta_meters(a, b, dim->chunk_size_m)) / fmaxf(dim->chunk_size_m, 0.001f);
}

static Vector3 linear_color(Color color) {
    return {
        std::pow(static_cast<float>(color.r) / 255.0f, 2.2f),
        std::pow(static_cast<float>(color.g) / 255.0f, 2.2f),
        std::pow(static_cast<float>(color.b) / 255.0f, 2.2f)};
}

template <typename TriangleType>
static void build_radiance_bvh(
    std::vector<TriangleType>* triangles,
    std::vector<GpuRadianceBvhNode>* nodes) {
    nodes->clear();
    if (!triangles || triangles->empty()) return;
    struct BuildRef {
        TriangleType triangle{};
        Vector3 bounds_min{};
        Vector3 bounds_max{};
        Vector3 centroid{};
    };
    std::vector<BuildRef> refs{};
    refs.reserve(triangles->size());
    for (const TriangleType& triangle : *triangles) {
        const Vector3 a = {triangle.a.x, triangle.a.y, triangle.a.z};
        const Vector3 b = {triangle.b.x, triangle.b.y, triangle.b.z};
        const Vector3 c = {triangle.c.x, triangle.c.y, triangle.c.z};
        const Vector3 minimum = {
            fminf(a.x, fminf(b.x, c.x)), fminf(a.y, fminf(b.y, c.y)), fminf(a.z, fminf(b.z, c.z))};
        const Vector3 maximum = {
            fmaxf(a.x, fmaxf(b.x, c.x)), fmaxf(a.y, fmaxf(b.y, c.y)), fmaxf(a.z, fmaxf(b.z, c.z))};
        refs.push_back({triangle, minimum, maximum, (a + b + c) / 3.0f});
    }
    nodes->reserve(refs.size() * 2u);
    auto build_node = [&](auto&& self, u32 begin, u32 end) -> u32 {
        const u32 node_index = static_cast<u32>(nodes->size());
        nodes->push_back({});
        const float maximum_float = std::numeric_limits<float>::max();
        Vector3 bounds_min = {maximum_float, maximum_float, maximum_float};
        Vector3 bounds_max = {-maximum_float, -maximum_float, -maximum_float};
        Vector3 centroid_min = bounds_min;
        Vector3 centroid_max = bounds_max;
        for (u32 i = begin; i < end; ++i) {
            const BuildRef& ref = refs[i];
            bounds_min = {
                fminf(bounds_min.x, ref.bounds_min.x), fminf(bounds_min.y, ref.bounds_min.y),
                fminf(bounds_min.z, ref.bounds_min.z)};
            bounds_max = {
                fmaxf(bounds_max.x, ref.bounds_max.x), fmaxf(bounds_max.y, ref.bounds_max.y),
                fmaxf(bounds_max.z, ref.bounds_max.z)};
            centroid_min = {
                fminf(centroid_min.x, ref.centroid.x), fminf(centroid_min.y, ref.centroid.y),
                fminf(centroid_min.z, ref.centroid.z)};
            centroid_max = {
                fmaxf(centroid_max.x, ref.centroid.x), fmaxf(centroid_max.y, ref.centroid.y),
                fmaxf(centroid_max.z, ref.centroid.z)};
        }
        GpuRadianceBvhNode node{};
        constexpr float padding = 0.001f;
        node.bounds_min = {bounds_min.x - padding, bounds_min.y - padding, bounds_min.z - padding, 0.0f};
        node.bounds_max = {bounds_max.x + padding, bounds_max.y + padding, bounds_max.z + padding, 0.0f};
        const u32 count = end - begin;
        if (count <= 4u) {
            node.meta = {0u, 0u, begin, count};
        } else {
            const Vector3 extent = centroid_max - centroid_min;
            const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
            const u32 middle = begin + count / 2u;
            std::nth_element(
                refs.begin() + begin, refs.begin() + middle, refs.begin() + end,
                [axis](const BuildRef& a, const BuildRef& b) {
                    const float av = axis == 0 ? a.centroid.x : (axis == 1 ? a.centroid.y : a.centroid.z);
                    const float bv = axis == 0 ? b.centroid.x : (axis == 1 ? b.centroid.y : b.centroid.z);
                    return av < bv;
                });
            const u32 left = self(self, begin, middle);
            const u32 right = self(self, middle, end);
            node.meta = {left, right, 0u, 0u};
        }
        (*nodes)[node_index] = node;
        return node_index;
    };
    build_node(build_node, 0u, static_cast<u32>(refs.size()));
    triangles->clear();
    triangles->reserve(refs.size());
    for (const BuildRef& ref : refs) triangles->push_back(ref.triangle);
}

static u64 calculate_radiance_scene_signature(
    const RenderState* renderer,
    const Dimension* dim,
    WorldPos anchor) {
    const float radius = static_cast<float>(std::max(1, renderer->lighting.lighting_radius_chunks)) + 1.5f;
    u64 signature = 1469598103934665603ull;
    signature = radiance_hash_add(signature, static_cast<u32>(anchor.chunk.x));
    signature = radiance_hash_add(signature, static_cast<u32>(anchor.chunk.y));
    signature = radiance_hash_add(signature, static_cast<u32>(anchor.chunk.z));
    u32 mesh_count = 0;
    for_each_radiance_chunk(dim, anchor.chunk, radius, [&](const Chunk& chunk) {
        for (u32 mesh_index = 0; mesh_index < chunk.mesh_count; ++mesh_index) {
            const u32 mesh_id = chunk.meshes[mesh_index];
            const MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
            if (!mesh || !mesh->visible || chunk_distance(mesh->origin.chunk, anchor.chunk) > radius) continue;
            signature = radiance_hash_add(signature, radiance_static_mesh_signature(*mesh, mesh_id));
            ++mesh_count;
        }
    });
    signature = radiance_hash_add(signature, mesh_count);
    signature = radiance_hash_float(signature, dim->ambient);
    signature = radiance_hash_add(signature, ColorToInt(dim->sky_top));
    signature = radiance_hash_add(signature, ColorToInt(dim->sky_bottom));
    return signature;
}

static u32 build_radiance_player_meshes(
    Dimension* dim,
    WorldPos anchor,
    std::vector<GpuRadianceTriangle>* out_triangles,
    std::vector<GpuRadianceBvhNode>* out_bvh_nodes) {
    constexpr u32 segments = 32;
    out_triangles->clear();
    out_bvh_nodes->clear();
    u32 player_mesh_count = 0;
    auto push_triangle = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 albedo) {
        out_triangles->push_back({
            {a.x, a.y, a.z, 0.0f}, {b.x, b.y, b.z, 0.0f}, {c.x, c.y, c.z, 0.0f},
            {albedo.x, albedo.y, albedo.z, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}});
    };
    for (u32 player_slot = 0; player_slot < dim->players.count && player_mesh_count < max_players; ++player_slot) {
        const PlayerEntity& player = dim->players.data[player_slot];
        if (!player.connected) continue;
        const WorldPos feet = player_feet_pos(dim, &player);
        if (!id_valid(feet.dimension)) continue;
        const float radius_m = fmaxf(0.01f, player.body_radius);
        const float height_m = fmaxf(radius_m * 2.0f, player.current_height);
        const Vector3 bottom = world_delta_meters(feet, anchor, dim->chunk_size_m);
        const Vector3 top = bottom + Vector3{0.0f, height_m, 0.0f};
        const Vector3 albedo = linear_color(player.color);
        for (u32 segment = 0; segment < segments; ++segment) {
            const float angle0 = static_cast<float>(segment) * 2.0f * PI / static_cast<float>(segments);
            const float angle1 = static_cast<float>(segment + 1) * 2.0f * PI / static_cast<float>(segments);
            const Vector3 bottom0 = bottom + Vector3{std::cos(angle0) * radius_m, 0.0f, std::sin(angle0) * radius_m};
            const Vector3 bottom1 = bottom + Vector3{std::cos(angle1) * radius_m, 0.0f, std::sin(angle1) * radius_m};
            const Vector3 top0 = top + Vector3{std::cos(angle0) * radius_m, 0.0f, std::sin(angle0) * radius_m};
            const Vector3 top1 = top + Vector3{std::cos(angle1) * radius_m, 0.0f, std::sin(angle1) * radius_m};
            push_triangle(bottom0, top0, top1, albedo);
            push_triangle(bottom0, top1, bottom1, albedo);
            push_triangle(top, top1, top0, albedo);
            push_triangle(bottom, bottom0, bottom1, albedo);
        }
        ++player_mesh_count;
    }
    build_radiance_bvh(out_triangles, out_bvh_nodes);
    return player_mesh_count;
}

static u32 build_pathtrace_player_meshes(
    Dimension* dim,
    WorldPos anchor,
    u32 local_player_id,
    std::vector<GpuPathtraceTriangle>* out_triangles,
    std::vector<GpuRadianceBvhNode>* out_bvh_nodes) {
    constexpr u32 segments = 32;
    out_triangles->clear();
    out_bvh_nodes->clear();
    u32 player_mesh_count = 0;
    auto push_triangle = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 albedo,
                             bool local_player) {
        out_triangles->push_back({
            {a.x, a.y, a.z, 0.0f}, {b.x, b.y, b.z, 0.0f},
            {c.x, c.y, c.z, 0.0f},
            {albedo.x, albedo.y, albedo.z, 0.0f}, {}, {},
            {0.0f, 0.0f, 0.0f, local_player ? 1.0f : 0.0f}});
    };
    for (u32 player_slot = 0;
         player_slot < dim->players.count && player_mesh_count < max_players;
         ++player_slot) {
        const PlayerEntity& player = dim->players.data[player_slot];
        if (!player.connected) continue;
        const u32 player_id = arena_id_at_slot(&dim->players, player_slot);
        const bool local_player = player.local || player_id == local_player_id;
        const WorldPos feet = player_feet_pos(dim, &player);
        if (!id_valid(feet.dimension)) continue;
        const float radius_m = fmaxf(0.01f, player.body_radius);
        const float height_m = fmaxf(radius_m * 2.0f, player.current_height);
        const Vector3 bottom = world_delta_meters(feet, anchor, dim->chunk_size_m);
        const Vector3 top = bottom + Vector3{0.0f, height_m, 0.0f};
        const Vector3 albedo = linear_color(player.color);
        for (u32 segment = 0; segment < segments; ++segment) {
            const float angle0 = static_cast<float>(segment) * 2.0f * PI /
                static_cast<float>(segments);
            const float angle1 = static_cast<float>(segment + 1) * 2.0f * PI /
                static_cast<float>(segments);
            const Vector3 bottom0 = bottom + Vector3{
                std::cos(angle0) * radius_m, 0.0f,
                std::sin(angle0) * radius_m};
            const Vector3 bottom1 = bottom + Vector3{
                std::cos(angle1) * radius_m, 0.0f,
                std::sin(angle1) * radius_m};
            const Vector3 top0 = top + Vector3{
                std::cos(angle0) * radius_m, 0.0f,
                std::sin(angle0) * radius_m};
            const Vector3 top1 = top + Vector3{
                std::cos(angle1) * radius_m, 0.0f,
                std::sin(angle1) * radius_m};
            push_triangle(bottom0, top0, top1, albedo, local_player);
            push_triangle(bottom0, top1, bottom1, albedo, local_player);
            push_triangle(top, top1, top0, albedo, local_player);
            push_triangle(bottom, bottom0, bottom1, albedo, local_player);
        }
        ++player_mesh_count;
    }
    build_radiance_bvh(out_triangles, out_bvh_nodes);
    return player_mesh_count;
}

static void finalize_radiance_emitter_surfaces(
    std::vector<GpuRadianceEmitter>* emitters) {
    if (!emitters || emitters->empty()) return;
    struct SurfaceBuild {
        Vector3 normal{};
        float plane = 0.0f;
        float area = 0.0f;
        Vector3 weighted_center{};
        std::vector<GpuRadianceEmitter> triangles{};
    };
    const auto triangle_vertices = [](const GpuRadianceEmitter& emitter) {
        return std::array<Vector3, 3>{
            Vector3{emitter.a_cdf.x, emitter.a_cdf.y, emitter.a_cdf.z},
            Vector3{emitter.b_weight.x, emitter.b_weight.y, emitter.b_weight.z},
            Vector3{emitter.c_area.x, emitter.c_area.y, emitter.c_area.z}};
    };
    const auto shares_edge = [&](const SurfaceBuild& surface,
                                  const GpuRadianceEmitter& candidate) {
        constexpr float vertex_epsilon_sq = 0.0001f * 0.0001f;
        const std::array<Vector3, 3> candidate_vertices = triangle_vertices(candidate);
        for (const GpuRadianceEmitter& member : surface.triangles) {
            const std::array<Vector3, 3> member_vertices = triangle_vertices(member);
            int shared_vertices = 0;
            for (const Vector3 candidate_vertex : candidate_vertices) {
                for (const Vector3 member_vertex : member_vertices) {
                    if (Vector3LengthSqr(candidate_vertex - member_vertex) <=
                        vertex_epsilon_sq) {
                        ++shared_vertices;
                        break;
                    }
                }
            }
            if (shared_vertices >= 2) return true;
        }
        return false;
    };
    u32 group_count = 0;
    for (const GpuRadianceEmitter& emitter : *emitters) {
        group_count = std::max(
            group_count,
            static_cast<u32>(std::lround(emitter.emission.w)) + 1u);
    }
    std::vector<GpuRadianceEmitter> reordered{};
    reordered.reserve(emitters->size());
    float cumulative_weight = 0.0f;
    for (u32 group = 0; group < group_count; ++group) {
        std::vector<SurfaceBuild> surfaces{};
        for (const GpuRadianceEmitter& source : *emitters) {
            if (static_cast<u32>(std::lround(source.emission.w)) != group) continue;
            const Vector3 a = {source.a_cdf.x, source.a_cdf.y, source.a_cdf.z};
            const Vector3 b = {source.b_weight.x, source.b_weight.y, source.b_weight.z};
            const Vector3 c = {source.c_area.x, source.c_area.y, source.c_area.z};
            const Vector3 normal = safe_norm(Vector3CrossProduct(b - a, c - a));
            const float plane = Vector3DotProduct(a, normal);
            auto found = surfaces.end();
            for (auto surface = surfaces.begin(); surface != surfaces.end(); ++surface) {
                if (Vector3DotProduct(surface->normal, normal) > 0.999f &&
                    std::fabs(surface->plane - plane) < 0.005f &&
                    shares_edge(*surface, source)) {
                    found = surface;
                    break;
                }
            }
            if (found == surfaces.end()) {
                surfaces.push_back({normal, plane});
                found = surfaces.end() - 1;
            }
            const float area = fmaxf(0.0f, source.c_area.w);
            found->area += area;
            found->weighted_center = found->weighted_center + (a + b + c) * (area / 3.0f);
            found->triangles.push_back(source);
        }
        for (SurfaceBuild& surface : surfaces) {
            if (surface.area <= 0.000001f || surface.triangles.empty()) continue;
            const Vector3 center = surface.weighted_center / surface.area;
            const float triangle_count = static_cast<float>(surface.triangles.size());
            for (size_t triangle_index = 0;
                 triangle_index < surface.triangles.size(); ++triangle_index) {
                GpuRadianceEmitter emitter = surface.triangles[triangle_index];
                cumulative_weight += emitter.b_weight.w;
                emitter.a_cdf.w = cumulative_weight;
                emitter.surface_center_area = {
                    center.x, center.y, center.z, surface.area};
                emitter.surface_normal_meta = {
                    surface.normal.x, surface.normal.y, surface.normal.z,
                    triangle_index == 0 ? triangle_count : 0.0f};
                reordered.push_back(emitter);
            }
        }
    }
    *emitters = std::move(reordered);
}

static u64 build_radiance_emitters_in_radius(
    const RenderState* renderer,
    const Dimension* dim,
    WorldPos anchor,
    float radius,
    u32 max_scene_emitters,
    std::vector<GpuRadianceEmitter>* out_emitters,
    float* out_total_weight,
    u32* out_emissive_mesh_count) {
    out_emitters->clear();
    *out_total_weight = 0.0f;
    *out_emissive_mesh_count = 0;
    u64 signature = 1469598103934665603ull;
    std::vector<u32> mesh_ids{};
    for_each_radiance_chunk(dim, anchor.chunk, radius, [&](const Chunk& chunk) {
        for (u32 mesh_index = 0; mesh_index < chunk.mesh_count; ++mesh_index) {
            const u32 mesh_id = chunk.meshes[mesh_index];
            const MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
            if (!mesh || !mesh->visible ||
                chunk_distance(mesh->origin.chunk, anchor.chunk) > radius ||
                safe_len(resolve_mesh_emission(mesh)) <= 0.0001f) continue;
            mesh_ids.push_back(mesh_id);
        }
    });
    std::sort(mesh_ids.begin(), mesh_ids.end());
    mesh_ids.erase(std::unique(mesh_ids.begin(), mesh_ids.end()), mesh_ids.end());

    for (u32 mesh_id : mesh_ids) {
        const MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
        const MeshGeometry* geometry = mesh ? arena_get(&dim->geometries, mesh->geometry) : nullptr;
        if (!mesh || !geometry) continue;
        const Vector3 emission = resolve_mesh_emission(mesh);
        const Matrix basis = matrix_no_translation(mesh->se3);
        const Vector3 origin = world_delta_meters(mesh->origin, anchor, dim->chunk_size_m);
        Vector3 vertices[max_vertices_per_geometry]{};
        for (u32 vertex_index = 0; vertex_index < geometry->vertex_count; ++vertex_index) {
            vertices[vertex_index] =
                Vector3Transform(geometry->vertices[vertex_index], basis) + origin;
        }
        const size_t group_start = out_emitters->size();
        for (u32 triangle_index = 0;
             triangle_index < geometry->triangle_count &&
             out_emitters->size() < max_scene_emitters;
             ++triangle_index) {
            const Triangle triangle = geometry->triangles[triangle_index];
            const Vector3 a = vertices[triangle.a];
            const Vector3 b = vertices[triangle.b];
            const Vector3 c = vertices[triangle.c];
            const float area = 0.5f * safe_len(Vector3CrossProduct(b - a, c - a));
            if (area <= 0.000001f) continue;
            const float luminance = fmaxf(
                0.0001f,
                emission.x * 0.2126f + emission.y * 0.7152f + emission.z * 0.0722f);
            const float weight = area * luminance;
            *out_total_weight += weight;
            out_emitters->push_back({
                {a.x, a.y, a.z, *out_total_weight},
                {b.x, b.y, b.z, weight},
                {c.x, c.y, c.z, area},
                {emission.x, emission.y, emission.z,
                 static_cast<float>(*out_emissive_mesh_count)}});
        }
        if (out_emitters->size() > group_start) ++*out_emissive_mesh_count;
        signature = radiance_hash_add(signature, radiance_mesh_signature(*mesh, mesh_id));
    }
    signature = radiance_hash_add(signature, out_emitters->size());
    signature = radiance_hash_float(signature, *out_total_weight);
    finalize_radiance_emitter_surfaces(out_emitters);
    return signature;
}

static u64 build_radiance_emitters(
    const RenderState* renderer,
    const Dimension* dim,
    WorldPos anchor,
    std::vector<GpuRadianceEmitter>* out_emitters,
    float* out_total_weight,
    u32* out_emissive_mesh_count) {
    const float radius =
        static_cast<float>(std::max(1, renderer->lighting.lighting_radius_chunks)) + 1.5f;
    return build_radiance_emitters_in_radius(
        renderer, dim, anchor, radius, 256u, out_emitters,
        out_total_weight, out_emissive_mesh_count);
}

static void build_radiance_scene(
    RenderState* renderer,
    Dimension* dim,
    WorldPos anchor,
    std::vector<GpuRadianceTriangle>* out_triangles,
    std::vector<GpuRadianceEmitter>* out_emitters,
    std::vector<GpuRadianceBvhNode>* out_bvh_nodes,
    float* out_emitter_total_weight,
    u64* out_signature,
    u64* out_emitter_signature,
    u32* out_emissive_mesh_count,
    bool* out_scene_truncated,
    bool build_bvh = true) {
    constexpr u32 max_scene_triangles = 16384;
    out_triangles->clear();
    out_bvh_nodes->clear();
    const u64 signature = calculate_radiance_scene_signature(renderer, dim, anchor);
    const float radius = static_cast<float>(std::max(1, renderer->lighting.lighting_radius_chunks)) + 1.5f;

    struct MeshCandidate { u32 id = invalid_id; float distance = 0.0f; };
    std::vector<MeshCandidate> candidates{};
    for_each_radiance_chunk(dim, anchor.chunk, radius, [&](const Chunk& chunk) {
        for (u32 mesh_index = 0; mesh_index < chunk.mesh_count; ++mesh_index) {
            const u32 mesh_id = chunk.meshes[mesh_index];
            const MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
            if (!mesh || !mesh->visible ||
                safe_len(resolve_mesh_emission(mesh)) > 0.0001f) continue;
            candidates.push_back({
                mesh_id, world_distance_chunks(dim, mesh->origin, anchor)});
        }
    });
    std::stable_sort(candidates.begin(), candidates.end(), [](const MeshCandidate& a, const MeshCandidate& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.id < b.id;
    });

    u32 emissive_mesh_count = 0;
    const u64 emitter_signature = build_radiance_emitters(
        renderer, dim, anchor, out_emitters, out_emitter_total_weight,
        &emissive_mesh_count);
    bool scene_truncated = false;
    auto append_mesh = [&](u32 mesh_id) {
        const MeshInstance* mesh_ptr = arena_get(&dim->meshes, mesh_id);
        if (!mesh_ptr || !mesh_ptr->visible || chunk_distance(mesh_ptr->origin.chunk, anchor.chunk) > radius) return;
        const MeshInstance& mesh = *mesh_ptr;
        const MeshGeometry* geometry = arena_get(&dim->geometries, mesh.geometry);
        if (!geometry) return;
        if (out_triangles->size() + geometry->triangle_count > max_scene_triangles) {
            scene_truncated = true;
            return;
        }
        const Matrix basis = matrix_no_translation(mesh.se3);
        const Vector3 origin = world_delta_meters(mesh.origin, anchor, dim->chunk_size_m);
        const Color color = resolve_mesh_color(&mesh);
        const Vector3 albedo = linear_color(color);
        const float reflectivity = resolve_mesh_reflectivity(&mesh);
        Vector3 vertices[max_vertices_per_geometry]{};
        for (u32 vertex_index = 0; vertex_index < geometry->vertex_count; ++vertex_index) {
            vertices[vertex_index] = Vector3Transform(geometry->vertices[vertex_index], basis) + origin;
        }
        for (u32 triangle_index = 0;
             triangle_index < geometry->triangle_count && out_triangles->size() < max_scene_triangles;
             ++triangle_index) {
            const Triangle triangle = geometry->triangles[triangle_index];
            const Vector3 a = vertices[triangle.a];
            const Vector3 b = vertices[triangle.b];
            const Vector3 c = vertices[triangle.c];
            const float area = 0.5f * safe_len(Vector3CrossProduct(b - a, c - a));
            if (area <= 0.000001f) continue;
            out_triangles->push_back({
                {a.x, a.y, a.z, 0.0f}, {b.x, b.y, b.z, 0.0f}, {c.x, c.y, c.z, 0.0f},
                {albedo.x, albedo.y, albedo.z, reflectivity},
                {0.0f, 0.0f, 0.0f, 0.0f}});
        }
    };
    for (const MeshCandidate& candidate : candidates) {
        if (out_triangles->size() >= max_scene_triangles) {
            scene_truncated = true;
            break;
        }
        append_mesh(candidate.id);
    }
    if (build_bvh) build_radiance_bvh(out_triangles, out_bvh_nodes);
    *out_signature = signature;
    if (out_emitter_signature) *out_emitter_signature = emitter_signature;
    if (out_emissive_mesh_count) *out_emissive_mesh_count = emissive_mesh_count;
    if (out_scene_truncated) *out_scene_truncated = scene_truncated;
}

static void build_pathtrace_static_scene(
    RenderState* renderer,
    const Dimension* dim,
    WorldPos anchor,
    std::vector<GpuPathtraceTriangle>* out_triangles,
    std::vector<GpuRadianceBvhNode>* out_bvh_nodes,
    bool* out_truncated) {
    // This is a comparison/debug renderer, but keeping a finite guard prevents
    // an accidentally malformed streamed world from allocating an unbounded
    // SSBO. The normal authored worlds remain far below this ceiling.
    constexpr size_t max_pathtrace_scene_triangles = 1024u * 1024u;
    out_triangles->clear();
    out_bvh_nodes->clear();
    bool truncated = false;
    const float radius = static_cast<float>(std::max(1, dim->render_radius_chunks));

    struct Candidate { u32 mesh_id = invalid_id; float distance = 0.0f; };
    std::vector<Candidate> candidates{};
    candidates.reserve(dim->meshes.count);
    for_each_radiance_chunk(dim, anchor.chunk, radius, [&](const Chunk& chunk) {
        for (u32 mesh_index = 0; mesh_index < chunk.mesh_count; ++mesh_index) {
            const u32 mesh_id = chunk.meshes[mesh_index];
            const MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
            if (!mesh || !mesh->visible ||
                safe_len(resolve_mesh_emission(mesh)) > 0.0001f) continue;
            candidates.push_back({mesh_id, chunk_distance(chunk.coord, anchor.chunk)});
        }
    });
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.mesh_id < b.mesh_id;
    });

    for (const Candidate& candidate : candidates) {
        const MeshInstance* mesh = arena_get(&dim->meshes, candidate.mesh_id);
        if (!mesh) continue;
        const MeshGeometry* geometry = arena_get(&dim->geometries, mesh->geometry);
        if (!geometry) continue;
        if (candidate.distance > static_cast<float>(dim->quality_render_radius_chunks)) {
            if (!id_valid(geometry->lod_geometry)) continue;
            geometry = arena_get(&dim->geometries, geometry->lod_geometry);
            if (!geometry) continue;
        }
        if (out_triangles->size() + geometry->triangle_count >
            max_pathtrace_scene_triangles) {
            truncated = true;
            break;
        }

        const Matrix basis = matrix_no_translation(mesh->se3);
        const Vector3 origin = world_delta_meters(mesh->origin, anchor, dim->chunk_size_m);
        const Vector3 albedo = linear_color(resolve_mesh_color(mesh));
        const float reflectivity = resolve_mesh_reflectivity(mesh);
        const Texture2D texture = render_texture(renderer, mesh->texture_id);
        Vector3 vertices[max_vertices_per_geometry]{};
        for (u32 vertex_index = 0; vertex_index < geometry->vertex_count; ++vertex_index) {
            vertices[vertex_index] =
                Vector3Transform(geometry->vertices[vertex_index], basis) + origin;
        }
        for (u32 triangle_index = 0; triangle_index < geometry->triangle_count;
             ++triangle_index) {
            const Triangle triangle = geometry->triangles[triangle_index];
            const Vector3 a = vertices[triangle.a];
            const Vector3 b = vertices[triangle.b];
            const Vector3 c = vertices[triangle.c];
            const Vector3 cross = Vector3CrossProduct(b - a, c - a);
            if (safe_len(cross) <= 0.000002f) continue;
            const Vector3 normal = safe_norm(cross);
            const Vector2 uv_a = planar_texture_uv(
                a, origin, normal, dim->pixels_per_meter, texture);
            const Vector2 uv_b = planar_texture_uv(
                b, origin, normal, dim->pixels_per_meter, texture);
            const Vector2 uv_c = planar_texture_uv(
                c, origin, normal, dim->pixels_per_meter, texture);
            out_triangles->push_back({
                {a.x, a.y, a.z, 0.0f},
                {b.x, b.y, b.z, 0.0f},
                {c.x, c.y, c.z, 0.0f},
                {albedo.x, albedo.y, albedo.z, reflectivity},
                {0.0f, 0.0f, 0.0f, 0.0f},
                {uv_a.x, uv_a.y, uv_b.x, uv_b.y},
                {uv_c.x, uv_c.y, static_cast<float>(mesh->texture_id), 0.0f}});
        }
    }
    build_radiance_bvh(out_triangles, out_bvh_nodes);
    if (out_truncated) *out_truncated = truncated;
}

static u64 calculate_pathtrace_static_scene_signature(
    const Dimension* dim,
    WorldPos anchor) {
    u64 signature = 1469598103934665603ull;
    const float radius = static_cast<float>(std::max(1, dim->render_radius_chunks));
    u32 mesh_count = 0;
    for_each_radiance_chunk(dim, anchor.chunk, radius, [&](const Chunk& chunk) {
        for (u32 mesh_index = 0; mesh_index < chunk.mesh_count; ++mesh_index) {
            const u32 mesh_id = chunk.meshes[mesh_index];
            const MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
            if (!mesh) continue;
            // This identity includes static transforms, visibility, authored
            // texture/color/reflectivity, and the emissive threshold. Like RC,
            // it deliberately excludes moving emissive positions because those
            // triangles live only in the per-frame emitter buffer.
            signature = radiance_hash_add(
                signature, radiance_static_mesh_signature(*mesh, mesh_id));
            ++mesh_count;
        }
    });
    signature = radiance_hash_add(signature, mesh_count);
    signature = radiance_hash_float(signature, dim->pixels_per_meter);
    return signature;
}

struct RadiancePreparedScene {
    u32 dimension = invalid_id;
    ChunkCoord anchor{};
    u64 signature = 0;
    u64 emitter_signature = 0;
    std::vector<GpuRadianceTriangle> triangles{};
    std::vector<GpuRadianceEmitter> emitters{};
    std::vector<GpuRadianceBvhNode> bvh_nodes{};
    float emitter_total_weight = 0.0f;
    u32 emissive_mesh_count = 0;
    bool truncated = false;
};

struct RadiancePreparedSurface {
    MeshLightingSurfaceCache surface{};
    u64 paint_signature = 0;
    i32 next_row = 0;
    bool complete = false;
};

struct RadiancePreparedGpuScene {
    u32 dimension = invalid_id;
    ChunkCoord anchor{};
    u64 signature = 0;
    u64 emitter_signature = 0;
    u64 settings_signature = 0;
    u32 triangles_ssbo = 0;
    u32 emitters_ssbo = 0;
    u32 bvh_nodes_ssbo = 0;
    u32 triangles_capacity = 0;
    u32 emitters_capacity = 0;
    u32 bvh_nodes_capacity = 0;
    u32 triangle_count = 0;
    u32 emitter_triangle_count = 0;
    u32 bvh_node_count = 0;
    u32 emissive_mesh_count = 0;
    float emitter_total_weight = 0.0f;
    bool truncated = false;
    bool uploaded = false;
    bool cascade_ready = false;
    int cascade_iterations_complete = 0;
    RenderTexture2D cascade_targets[2]{};
    u32 active_target = 0;
    int atlas_width = 0;
    int atlas_height = 0;
    std::vector<RadiancePreparedSurface> surfaces{};
};

struct RadianceBackgroundState {
    std::thread worker{};
    std::atomic<bool> done{true};
    bool has_result = false;
    ChunkCoord target{};
    ChunkCoord observed_target{};
    u64 observed_signature = 0;
    u64 observed_signature_frame = 0;
    u32 observed_stable_frames = 0;
    RadiancePreparedScene result{};
    RadiancePreparedGpuScene gpu{};
    std::vector<RenderTexture2D> retired_targets{};
    WorldPos previous_camera_anchor{};
    Vector3 smoothed_camera_motion{};
    u64 selected_target_frame = std::numeric_limits<u64>::max();
    bool selected_target_valid = false;
    ChunkCoord selected_target{};
    float selected_target_distance = 0.0f;
};

static void retire_prepared_target(RadianceBackgroundState* state, RenderTexture2D* target) {
    if (!state || !target || !target->id) return;
    state->retired_targets.push_back(*target);
    *target = {};
}

static void clear_prepared_surface_targets(RadianceBackgroundState* state) {
    if (!state) return;
    for (RadiancePreparedSurface& prepared : state->gpu.surfaces) {
        retire_prepared_target(state, &prepared.surface.texture);
        retire_prepared_target(state, &prepared.surface.shadow_mask_texture);
        retire_prepared_target(state, &prepared.surface.history_texture);
    }
    state->gpu.surfaces.clear();
}

static void drain_retired_prepared_targets(RadianceBackgroundState* state, u32 budget) {
    if (!state) return;
    while (budget-- > 0 && !state->retired_targets.empty()) {
        RenderTexture2D target = state->retired_targets.back();
        state->retired_targets.pop_back();
        unload_color_render_texture(&target);
    }
}

static void unload_prepared_gpu_scene(RadianceBackgroundState* state) {
    if (!state) return;
    clear_prepared_surface_targets(state);
    for (RenderTexture2D& target : state->gpu.cascade_targets) {
        unload_color_render_texture(&target);
    }
    if (state->gpu.triangles_ssbo) rlUnloadShaderBuffer(state->gpu.triangles_ssbo);
    if (state->gpu.emitters_ssbo) rlUnloadShaderBuffer(state->gpu.emitters_ssbo);
    if (state->gpu.bvh_nodes_ssbo) rlUnloadShaderBuffer(state->gpu.bvh_nodes_ssbo);
    while (!state->retired_targets.empty()) {
        RenderTexture2D target = state->retired_targets.back();
        state->retired_targets.pop_back();
        unload_color_render_texture(&target);
    }
    state->gpu = {};
}

static RadianceBackgroundState* radiance_background_state(RenderState* renderer) {
    if (!renderer->radiance_background_state) {
        renderer->radiance_background_state = new RadianceBackgroundState{};
    }
    return static_cast<RadianceBackgroundState*>(renderer->radiance_background_state);
}

static void shutdown_radiance_background(RenderState* renderer) {
    if (!renderer || !renderer->radiance_background_state) return;
    RadianceBackgroundState* state = static_cast<RadianceBackgroundState*>(renderer->radiance_background_state);
    if (state->worker.joinable()) state->worker.join();
    unload_prepared_gpu_scene(state);
    delete state;
    renderer->radiance_background_state = nullptr;
}

static void finish_radiance_background_worker(RadianceBackgroundState* state) {
    if (!state || !state->done.load(std::memory_order_acquire)) return;
    if (state->worker.joinable()) state->worker.join();
}

static bool take_prepared_radiance_scene(
    RenderState* renderer,
    WorldPos anchor,
    u64 signature,
    RadiancePreparedScene* out) {
    RadianceBackgroundState* state = radiance_background_state(renderer);
    finish_radiance_background_worker(state);
    if (!state->done.load(std::memory_order_acquire) || !state->has_result ||
        state->result.dimension != anchor.dimension || !chunk_equal(state->target, anchor.chunk) ||
        state->result.signature != signature) return false;
    *out = std::move(state->result);
    state->has_result = false;
    return true;
}

static bool neighbor_radiance_target(
    RenderState* renderer,
    RadianceBackgroundState* state,
    const Dimension* dim,
    const CameraView& view,
    ChunkCoord active_anchor,
    ChunkCoord* out_target,
    float* out_distance = nullptr) {
    if (state->selected_target_frame == renderer->radiance_frame) {
        if (state->selected_target_valid) {
            if (out_target) *out_target = state->selected_target;
            if (out_distance) *out_distance = state->selected_target_distance;
        }
        return state->selected_target_valid;
    }
    state->selected_target_frame = renderer->radiance_frame;
    state->selected_target_valid = false;

    constexpr float preparation_distance_m = 6.0f;
    const float chunk_size = dim->chunk_size_m;
    ChunkCoord target = view.anchor.chunk;
    float nearest = preparation_distance_m + 1.0f;
    auto consider_nearest = [&](float distance, ChunkCoord candidate) {
        if (distance < nearest) {
            nearest = distance;
            target = candidate;
        }
    };
    consider_nearest(view.anchor.local.x, chunk_add(view.anchor.chunk, {-1, 0, 0}));
    consider_nearest(chunk_size - view.anchor.local.x, chunk_add(view.anchor.chunk, {1, 0, 0}));
    consider_nearest(view.anchor.local.z, chunk_add(view.anchor.chunk, {0, 0, -1}));
    consider_nearest(chunk_size - view.anchor.local.z, chunk_add(view.anchor.chunk, {0, 0, 1}));

    Vector3 frame_motion{};
    if (state->previous_camera_anchor.dimension == view.anchor.dimension) {
        frame_motion = world_delta_meters(
            view.anchor, state->previous_camera_anchor, dim->chunk_size_m);
        if (safe_len(frame_motion) > chunk_size * 0.5f) frame_motion = {};
    } else {
        state->smoothed_camera_motion = {};
    }
    state->previous_camera_anchor = view.anchor;
    state->smoothed_camera_motion =
        state->smoothed_camera_motion * 0.35f + frame_motion * 0.65f;

    // Directional prediction buys enough frames to finish many small faces
    // while the player is clearly approaching a seam. Stationary movement,
    // reversals and teleports still use the conservative six-metre fallback.
    const float directional_distance =
        std::max(preparation_distance_m, std::min(12.0f, chunk_size * 0.75f));
    float best_time = std::numeric_limits<float>::max();
    ChunkCoord directional_target{};
    float predicted_distance = 0.0f;
    auto consider_direction = [&](float speed, float distance, ChunkCoord candidate) {
        if (speed <= 0.002f || distance > directional_distance) return;
        const float time = distance / speed;
        if (time < best_time) {
            best_time = time;
            directional_target = candidate;
            predicted_distance = distance;
        }
    };
    consider_direction(
        -state->smoothed_camera_motion.x, view.anchor.local.x,
        chunk_add(view.anchor.chunk, {-1, 0, 0}));
    consider_direction(
        state->smoothed_camera_motion.x, chunk_size - view.anchor.local.x,
        chunk_add(view.anchor.chunk, {1, 0, 0}));
    consider_direction(
        -state->smoothed_camera_motion.z, view.anchor.local.z,
        chunk_add(view.anchor.chunk, {0, 0, -1}));
    consider_direction(
        state->smoothed_camera_motion.z, chunk_size - view.anchor.local.z,
        chunk_add(view.anchor.chunk, {0, 0, 1}));
    if (best_time < std::numeric_limits<float>::max()) {
        target = directional_target;
        nearest = predicted_distance;
    } else if (nearest > preparation_distance_m) {
        return false;
    }
    if (chunk_equal(target, active_anchor)) return false;
    state->selected_target_valid = true;
    state->selected_target = target;
    state->selected_target_distance = nearest;
    if (out_target) *out_target = target;
    if (out_distance) *out_distance = nearest;
    return true;
}

static void schedule_neighbor_radiance_scene(
    RenderState* renderer,
    Dimension* dim,
    const CameraView& view) {
    RadianceBackgroundState* state = radiance_background_state(renderer);
    ChunkCoord target{};
    if (!neighbor_radiance_target(
            renderer, state, dim, view, renderer->radiance_anchor_chunk, &target)) return;
    finish_radiance_background_worker(state);
    if (!state->done.load(std::memory_order_acquire)) return;
    WorldPos anchor{};
    anchor.dimension = view.anchor.dimension;
    anchor.chunk = target;
    const u64 target_signature = calculate_radiance_scene_signature(renderer, dim, anchor);
    state->observed_signature_frame = renderer->radiance_frame;
    if (!chunk_equal(state->observed_target, target) || state->observed_signature != target_signature) {
        state->observed_target = target;
        state->observed_signature = target_signature;
        state->observed_stable_frames = 1;
        return;
    }
    if (state->observed_stable_frames < 2) {
        ++state->observed_stable_frames;
        return;
    }
    if (state->has_result && state->result.dimension == view.anchor.dimension &&
        chunk_equal(state->target, target) &&
        state->result.signature == target_signature) return;
    if (state->gpu.uploaded && state->gpu.dimension == view.anchor.dimension &&
        chunk_equal(state->gpu.anchor, target) &&
        state->gpu.signature == target_signature &&
        state->gpu.settings_signature == renderer->radiance_settings_signature) return;
    state->has_result = false;
    RadiancePreparedScene prepared{};
    prepared.dimension = view.anchor.dimension;
    prepared.anchor = target;
    build_radiance_scene(
        renderer, dim, anchor, &prepared.triangles, &prepared.emitters, &prepared.bvh_nodes,
        &prepared.emitter_total_weight, &prepared.signature, &prepared.emitter_signature,
        &prepared.emissive_mesh_count, &prepared.truncated, false);
    if (prepared.triangles.empty()) return;
    state->target = target;
    state->done.store(false, std::memory_order_release);
    state->worker = std::thread([state, prepared = std::move(prepared)]() mutable {
        build_radiance_bvh(&prepared.triangles, &prepared.bvh_nodes);
        state->result = std::move(prepared);
        state->has_result = true;
        state->done.store(true, std::memory_order_release);
    });
}

static void unload_radiance_targets(RenderState* renderer) {
    for (RenderTexture2D& target : renderer->radiance_cascade_targets) {
        unload_color_render_texture(&target);
    }
    renderer->radiance_atlas_width = 0;
    renderer->radiance_atlas_height = 0;
}

static bool ensure_radiance_target_set(
    RenderState* renderer,
    RenderTexture2D targets[2],
    u32* active_target,
    int* atlas_width,
    int* atlas_height) {
    const int extra = std::clamp(renderer->lighting.probe_extra_levels, 0, 2);
    const int probe_count = 8 << extra;
    constexpr int angular_res = 4;
    constexpr int level_count = 4;
    const int level_side = angular_res * probe_count;
    const int width = level_side * level_count;
    const int height = level_side * probe_count;
    if (width == *atlas_width && height == *atlas_height &&
        IsRenderTextureValid(targets[0]) && IsRenderTextureValid(targets[1])) return true;
    for (int index = 0; index < 2; ++index) unload_color_render_texture(&targets[index]);
    *atlas_width = 0;
    *atlas_height = 0;
    for (int index = 0; index < 2; ++index) {
        RenderTexture2D& target = targets[index];
        target = load_color_render_texture(width, height, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16);
        if (!IsRenderTextureValid(target)) target = LoadRenderTexture(width, height);
        if (!IsRenderTextureValid(target)) {
            for (int cleanup = 0; cleanup < 2; ++cleanup) {
                unload_color_render_texture(&targets[cleanup]);
            }
            return false;
        }
        SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);
        BeginTextureMode(target);
        ClearBackground(BLANK);
        EndTextureMode();
    }
    *atlas_width = width;
    *atlas_height = height;
    *active_target = 0;
    return true;
}

static float lighting_surface_nearest_distance_chunks(
    const Dimension* dim,
    const MeshLightingSurfaceCache& surface,
    Vector3 mesh_origin) {
    if (!dim) return 0.0f;
    const float nearest_distance_m = fmaxf(
        0.0f,
        safe_len(mesh_origin + surface.bounds_center) - surface.bounds_radius);
    return nearest_distance_m / fmaxf(dim->chunk_size_m, 0.001f);
}

static void rebuild_lighting_surfaces(RenderState* renderer, Dimension* dim, const CameraView& view) {
    // Keep one neighbor ring as metadata-only staging coverage.  A destination
    // face within R chunks can be up to R+1 from the current anchor just before
    // crossing; without this ring the prewarmer can only bake the intersection
    // of both anchor fields and misses the forward viewport by construction.
    const float max_radius =
        static_cast<float>(std::max(1, renderer->lighting.lighting_radius_chunks)) + 1.05f;
    std::vector<u32> active_mesh_ids{};
    for_each_radiance_chunk(dim, view.anchor.chunk, max_radius, [&](const Chunk& chunk) {
        for (u32 mesh_index = 0; mesh_index < chunk.mesh_count; ++mesh_index) {
            active_mesh_ids.push_back(chunk.meshes[mesh_index]);
        }
    });
    u64 topology = 1469598103934665603ull;
    topology = radiance_hash_add(topology, static_cast<u32>(view.anchor.chunk.x));
    topology = radiance_hash_add(topology, static_cast<u32>(view.anchor.chunk.y));
    topology = radiance_hash_add(topology, static_cast<u32>(view.anchor.chunk.z));
    topology = radiance_hash_float(topology, dim->pixels_per_meter);
    for (u32 mesh_id : active_mesh_ids) {
        const MeshInstance* mesh_ptr = arena_get(&dim->meshes, mesh_id);
        if (!mesh_ptr) continue;
        const MeshInstance& mesh = *mesh_ptr;
        const Vector3 emission = resolve_mesh_emission(&mesh);
        const bool emissive = safe_len(emission) > 0.0001f;
        topology = radiance_hash_add(topology, mesh_id);
        topology = radiance_hash_add(topology, mesh.geometry);
        if (!emissive) topology = radiance_hash_world_pos(topology, mesh.origin);
        topology = radiance_hash_add(topology, mesh.visible ? 1u : 0u);
        topology = radiance_hash_add(topology, mesh.lit ? 1u : 0u);
        const float matrix_values[] = {
            mesh.se3.m0, mesh.se3.m1, mesh.se3.m2, mesh.se3.m4, mesh.se3.m5, mesh.se3.m6,
            mesh.se3.m8, mesh.se3.m9, mesh.se3.m10};
        for (float value : matrix_values) topology = radiance_hash_float(topology, value);
        topology = radiance_hash_add(topology, ColorToInt(resolve_mesh_color(&mesh)));
        topology = radiance_hash_float(topology, resolve_mesh_reflectivity(&mesh));
        topology = radiance_hash_float(topology, emission.x);
        topology = radiance_hash_float(topology, emission.y);
        topology = radiance_hash_float(topology, emission.z);
    }
    if (renderer->radiance_topology_signature == topology) return;
    renderer->radiance_topology_signature = topology;

    std::vector<MeshLightingSurfaceCache> old = std::move(renderer->lighting_surfaces);
    renderer->lighting_surfaces.clear();
    std::unordered_multimap<u64, size_t> old_by_signature{};
    old_by_signature.reserve(old.size());
    for (size_t old_index = 0; old_index < old.size(); ++old_index) {
        old_by_signature.emplace(old[old_index].surface_signature, old_index);
    }
    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);

    for (u32 mesh_id : active_mesh_ids) {
        const MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
        if (!mesh) continue;
        const float distance = world_distance_chunks(dim, mesh->origin, view.anchor);
        if (!mesh->visible || distance > max_radius) continue;
        const Vector3 emission = resolve_mesh_emission(mesh);
        const float reflectivity = resolve_mesh_reflectivity(mesh);
        if (!mesh->lit && safe_len(emission) <= 0.0001f && reflectivity <= 0.0001f) continue;
        const MeshGeometry* geometry = arena_get(&dim->geometries, mesh->geometry);
        if (!geometry) continue;
        const Matrix basis = matrix_no_translation(mesh->se3);
        Vector3 vertices[max_vertices_per_geometry]{};
        for (u32 i = 0; i < geometry->vertex_count; ++i) vertices[i] = Vector3Transform(geometry->vertices[i], basis);

        std::vector<MeshLightingSurfaceCache> surfaces{};
        for (u32 tri_index = 0; tri_index < geometry->triangle_count; ++tri_index) {
            const Triangle tri = geometry->triangles[tri_index];
            const Vector3 normal = safe_norm(Vector3CrossProduct(vertices[tri.b] - vertices[tri.a], vertices[tri.c] - vertices[tri.a]));
            const float plane = Vector3DotProduct(vertices[tri.a], normal);
            auto found = surfaces.end();
            for (auto it = surfaces.begin(); it != surfaces.end(); ++it) {
                if (Vector3DotProduct(it->normal, normal) > 0.999f && std::fabs(it->plane - plane) < 0.005f) { found = it; break; }
            }
            if (found == surfaces.end()) {
                MeshLightingSurfaceCache surface{};
                surface.mesh_id = mesh_id;
                surface.geometry_id = mesh->geometry;
                surface.normal = normal;
                surface_texture_basis(normal, &surface.tangent, &surface.bitangent);
                surface.plane = plane;
                surface.chunk_distance = distance;
                surface.reflectivity = reflectivity;
                surface.emission = emission;
                surfaces.push_back(std::move(surface));
                found = surfaces.end() - 1;
            }
            found->triangles.push_back(tri_index);
        }

        for (MeshLightingSurfaceCache& surface : surfaces) {
            float min_u = std::numeric_limits<float>::max(), max_u = -std::numeric_limits<float>::max();
            float min_v = std::numeric_limits<float>::max(), max_v = -std::numeric_limits<float>::max();
            Vector3 bounds_min = {
                std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()};
            Vector3 bounds_max = {
                -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                -std::numeric_limits<float>::max()};
            for (u32 tri_index : surface.triangles) {
                const Triangle tri = geometry->triangles[tri_index];
                for (u32 vertex_index : {tri.a, tri.b, tri.c}) {
                    const Vector3 vertex = vertices[vertex_index];
                    const float u = Vector3DotProduct(vertex, surface.tangent);
                    const float v = Vector3DotProduct(vertex, surface.bitangent);
                    min_u = fminf(min_u, u); max_u = fmaxf(max_u, u);
                    min_v = fminf(min_v, v); max_v = fmaxf(max_v, v);
                    bounds_min = Vector3Min(bounds_min, vertex);
                    bounds_max = Vector3Max(bounds_max, vertex);
                }
            }
            surface.bounds_center = (bounds_min + bounds_max) * 0.5f;
            surface.bounds_radius = safe_len(bounds_max - surface.bounds_center);
            surface.chunk_distance = lighting_surface_nearest_distance_chunks(
                dim, surface,
                world_delta_meters(mesh->origin, view.anchor, dim->chunk_size_m));
            surface.grid_min_u = static_cast<i32>(std::floor(min_u * ppm));
            surface.grid_min_v = static_cast<i32>(std::floor(min_v * ppm));
            surface.width = static_cast<i32>(std::ceil(max_u * ppm)) - surface.grid_min_u;
            surface.height = static_cast<i32>(std::ceil(max_v * ppm)) - surface.grid_min_v;
            const i64 texels = static_cast<i64>(surface.width) * static_cast<i64>(surface.height);
            if (surface.width <= 0 || surface.height <= 0 || surface.width > 2048 || surface.height > 2048 || texels > 1048576) continue;
            u64 signature = 1469598103934665603ull;
            signature = radiance_hash_add(signature, mesh_id);
            signature = radiance_hash_add(signature, mesh->geometry);
            signature = radiance_hash_world_pos(signature, mesh->origin);
            const float transform[] = {
                mesh->se3.m0, mesh->se3.m1, mesh->se3.m2, mesh->se3.m4, mesh->se3.m5,
                mesh->se3.m6, mesh->se3.m8, mesh->se3.m9, mesh->se3.m10};
            for (float value : transform) signature = radiance_hash_float(signature, value);
            signature = radiance_hash_add(signature, ColorToInt(resolve_mesh_color(mesh)));
            signature = radiance_hash_float(signature, reflectivity);
            signature = radiance_hash_float(signature, emission.x);
            signature = radiance_hash_float(signature, emission.y);
            signature = radiance_hash_float(signature, emission.z);
            signature = radiance_hash_float(signature, surface.normal.x);
            signature = radiance_hash_float(signature, surface.normal.y);
            signature = radiance_hash_float(signature, surface.normal.z);
            signature = radiance_hash_float(signature, surface.plane);
            signature = radiance_hash_add(signature, static_cast<u32>(surface.grid_min_u));
            signature = radiance_hash_add(signature, static_cast<u32>(surface.grid_min_v));
            signature = radiance_hash_add(signature, static_cast<u32>(surface.width));
            signature = radiance_hash_add(signature, static_cast<u32>(surface.height));
            surface.surface_signature = signature;

            const auto matching = old_by_signature.equal_range(signature);
            for (auto candidate_it = matching.first; candidate_it != matching.second; ++candidate_it) {
                MeshLightingSurfaceCache& candidate = old[candidate_it->second];
                if (!IsRenderTextureValid(candidate.texture)) continue;
                surface.texture = candidate.texture;
                surface.shadow_mask_texture = candidate.shadow_mask_texture;
                surface.history_texture = candidate.history_texture;
                surface.resolved_scene_signature = candidate.resolved_scene_signature;
                surface.resolved_emitter_signature = candidate.resolved_emitter_signature;
                surface.resolved_paint_revision = candidate.resolved_paint_revision;
                surface.resolved_camera_signature = candidate.resolved_camera_signature;
                surface.resolved_player_signature = candidate.resolved_player_signature;
                surface.shadow_mask_signature = candidate.shadow_mask_signature;
                surface.stable_lighting_revision = candidate.stable_lighting_revision;
                surface.pending_player_signature = candidate.pending_player_signature;
                surface.pending_x = candidate.pending_x;
                surface.pending_y = candidate.pending_y;
                surface.pending_width = candidate.pending_width;
                surface.pending_height = candidate.pending_height;
                surface.pending_shadow_x = candidate.pending_shadow_x;
                surface.pending_shadow_y = candidate.pending_shadow_y;
                surface.pending_shadow_width = candidate.pending_shadow_width;
                surface.pending_shadow_height = candidate.pending_shadow_height;
                surface.active_shadow_x = candidate.active_shadow_x;
                surface.active_shadow_y = candidate.active_shadow_y;
                surface.active_shadow_width = candidate.active_shadow_width;
                surface.active_shadow_height = candidate.active_shadow_height;
                surface.valid_x = candidate.valid_x;
                surface.valid_y = candidate.valid_y;
                surface.valid_width = candidate.valid_width;
                surface.valid_height = candidate.valid_height;
                surface.valid_mip_level = candidate.valid_mip_level;
                surface.fully_initialized = candidate.fully_initialized;
                surface.pending_dynamic_shadow_active = candidate.pending_dynamic_shadow_active;
                surface.dynamic_composite_active = candidate.dynamic_composite_active;
                surface.temporal_samples = candidate.temporal_samples;
                surface.last_update_frame = candidate.last_update_frame;
                candidate.texture = {};
                candidate.shadow_mask_texture = {};
                candidate.history_texture = {};
                break;
            }
            renderer->lighting_surfaces.push_back(std::move(surface));
        }
    }
    RadianceBackgroundState* background = radiance_background_state(renderer);
    for (MeshLightingSurfaceCache& surface : old) {
        retire_prepared_target(background, &surface.texture);
        retire_prepared_target(background, &surface.shadow_mask_texture);
        retire_prepared_target(background, &surface.history_texture);
    }
    // Preserve the deterministic chunk -> mesh -> coplanar-face construction
    // order for presentation.  Sorting this shared cache by camera-relative
    // distance made equal-depth lighting overlays choose different raster
    // winners after a route returned to the same camera.  Prewarm, reflective,
    // and player-shadow update paths build their own priority lists, so cache
    // presentation order does not need to double as scheduling priority.
    renderer->radiance_surface_cursor = 0;
    renderer->debug_radiance_surface_count = static_cast<u32>(renderer->lighting_surfaces.size());
}

static float radiance_lighting_weight(const Dimension* dim, const RenderState* renderer, float chunk_dist) {
    const float radius = static_cast<float>(std::max(1, renderer->lighting.lighting_radius_chunks));
    float full = fminf(static_cast<float>(dim->quality_render_radius_chunks), radius);
    if (full >= radius) full = fmaxf(0.0f, radius - 1.0f);
    if (chunk_dist <= full) return 1.0f;
    if (chunk_dist >= radius) return 0.0f;
    return 1.0f - clampf((chunk_dist - full) / fmaxf(0.001f, radius - full), 0.0f, 1.0f);
}

int renderer_surface_lighting_mip_level(
    float projected_pixels_per_texel,
    int previous_mip_level) {
    const float pixel_density = fmaxf(0.0f, projected_pixels_per_texel);
    int desired_mip_level = pixel_density < 0.35f
        ? 3 : (pixel_density < 0.80f ? 2 : (pixel_density < 1.75f ? 1 : 0));

    // Hysteresis is deliberately expressed only in projected screen density.
    // Mesh-origin distance made a whole face change resolution at a world-space
    // threshold even when its visible texels had not changed apparent size.
    if (previous_mip_level == 1) {
        if (desired_mip_level == 0 && pixel_density < 2.00f) {
            desired_mip_level = 1;
        } else if (desired_mip_level == 2 && pixel_density > 0.70f) {
            desired_mip_level = 1;
        }
    } else if (previous_mip_level == 2) {
        if (desired_mip_level < 2 && pixel_density < 0.95f) {
            desired_mip_level = 2;
        } else if (desired_mip_level == 3 && pixel_density > 0.275f) {
            desired_mip_level = 2;
        }
    } else if (previous_mip_level == 3) {
        if (desired_mip_level < 3 && pixel_density < 0.45f) {
            desired_mip_level = 3;
        }
    }
    return desired_mip_level;
}

static bool ensure_lighting_surface_targets(
    MeshLightingSurfaceCache* surface,
    bool require_history) {
    auto allocate = [&](RenderTexture2D* target) {
        if (IsRenderTextureValid(*target)) return true;
        if (target->id) unload_color_render_texture(target);
        // Surface lighting is a colour cache. raylib's LoadRenderTexture() also
        // allocates a depth attachment, which is never sampled here and becomes
        // a sizeable cost when a streamed chunk contributes many faces.
        *target = load_color_render_texture(
            surface->width, surface->height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (!IsRenderTextureValid(*target)) {
            *target = {};
            return false;
        }
        set_pixel_mipmap_filter(target->texture);
        SetTextureWrap(target->texture, TEXTURE_WRAP_CLAMP);
        BeginTextureMode(*target);
        ClearBackground(BLANK);
        EndTextureMode();
        return true;
    };

    if (!allocate(&surface->texture)) return false;
    // With temporal accumulation disabled (the default), stable diffuse and
    // reflective light maps render directly into one target. Allocate a second
    // target only for a live dynamic-shadow composite or actual history.
    return !require_history || allocate(&surface->history_texture);
}

static bool ensure_lighting_surface_shadow_mask(
    MeshLightingSurfaceCache* surface,
    bool* out_allocated = nullptr) {
    if (out_allocated) *out_allocated = false;
    if (!surface) return false;
    if (IsRenderTextureValid(surface->shadow_mask_texture)) return true;
    if (surface->shadow_mask_texture.id) {
        unload_color_render_texture(&surface->shadow_mask_texture);
    }
    // R8 is sufficient for mix(0.40, 1.0, visibility), cuts the target's
    // bandwidth to one quarter of the old copied RGBA composite, and is a
    // colour-renderable format on the GL 4.3 backend. Keep the RGBA fallback
    // for drivers which reject a single-channel framebuffer attachment.
    surface->shadow_mask_texture = load_color_render_texture(
        surface->width, surface->height, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
    if (!IsRenderTextureValid(surface->shadow_mask_texture)) {
        surface->shadow_mask_texture = load_color_render_texture(
            surface->width, surface->height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    }
    if (!IsRenderTextureValid(surface->shadow_mask_texture)) {
        surface->shadow_mask_texture = {};
        return false;
    }
    SetTextureFilter(surface->shadow_mask_texture.texture, TEXTURE_FILTER_POINT);
    // Preserve texel-crisp magnification while filtering the mask under
    // minification, matching the stable lighting texture's distant behavior.
    glBindTexture(GL_TEXTURE_2D, surface->shadow_mask_texture.texture.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    SetTextureWrap(surface->shadow_mask_texture.texture, TEXTURE_WRAP_CLAMP);
    BeginTextureMode(surface->shadow_mask_texture);
    ClearBackground(WHITE);
    EndTextureMode();
    if (out_allocated) *out_allocated = true;
    return true;
}

static int cached_shader_location(RenderState* renderer, Shader shader, const char* name) {
    u64 name_hash = 1469598103934665603ull;
    for (const unsigned char* at = reinterpret_cast<const unsigned char*>(name); *at; ++at) {
        name_hash ^= *at;
        name_hash *= 1099511628211ull;
    }
    const u64 key = name_hash ^ (static_cast<u64>(shader.id) * 0x9e3779b97f4a7c15ull);
    const auto found = renderer->shader_uniform_locations.find(key);
    if (found != renderer->shader_uniform_locations.end()) return found->second;
    const int location = GetShaderLocation(shader, name);
    renderer->shader_uniform_locations.emplace(key, location);
    return location;
}

struct RadianceSceneGpuView {
    WorldPos anchor{};
    u64 signature = 0;
    u64 emitter_signature = 0;
    u32 triangles_ssbo = 0;
    u32 emitters_ssbo = 0;
    u32 bvh_nodes_ssbo = 0;
    u32 dynamic_triangles_ssbo = 0;
    u32 dynamic_bvh_nodes_ssbo = 0;
    int triangle_count = 0;
    int emitter_count = 0;
    int bvh_node_count = 0;
    int dynamic_triangle_count = 0;
    int dynamic_bvh_node_count = 0;
    int emissive_mesh_count = 0;
    float emitter_total_weight = 0.0f;
};

static void bind_radiance_scene_buffers(
    RenderState* renderer,
    const RadianceSceneGpuView& scene) {
    const std::array<u32, 5> buffers = {
        scene.triangles_ssbo, scene.emitters_ssbo, scene.bvh_nodes_ssbo,
        scene.dynamic_triangles_ssbo, scene.dynamic_bvh_nodes_ssbo};
    for (size_t index = 0; index < buffers.size(); ++index) {
        if (renderer && renderer->radiance_bound_scene_buffers[index] == buffers[index]) {
            continue;
        }
        rlBindShaderBuffer(buffers[index], static_cast<u32>(11 + index));
        if (renderer) {
            renderer->radiance_bound_scene_buffers[index] = buffers[index];
            ++renderer->debug_radiance_scene_buffer_binds;
        }
    }
}

static i64 radiance_floor_mod(i64 value, i64 divisor) {
    const i64 remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

static std::array<Vector3, 4> radiance_level_volume_mins(
    const Dimension* dim,
    const RadianceSceneGpuView& scene,
    Vector3 volume_min,
    Vector3 volume_size,
    int base_probe_count) {
    std::array<Vector3, 4> result{};
    const float chunk = fmaxf(dim->chunk_size_m, 0.001f);
    constexpr i64 subchunks = 2;
    const i64 span_subchunks = std::max<i64>(
        1, static_cast<i64>(std::llround(volume_size.x / chunk * subchunks)));
    const std::array<i64, 3> anchor_chunks = {
        static_cast<i64>(scene.anchor.chunk.x),
        static_cast<i64>(scene.anchor.chunk.y),
        static_cast<i64>(scene.anchor.chunk.z)};
    const std::array<float, 3> requested_min = {
        volume_min.x, volume_min.y, volume_min.z};
    for (int level = 0; level < static_cast<int>(result.size()); ++level) {
        const i64 probe_count = std::max<i64>(1, base_probe_count >> level);
        float* components[3] = {&result[level].x, &result[level].y, &result[level].z};
        for (int axis = 0; axis < 3; ++axis) {
            const i64 min_subchunks = static_cast<i64>(
                std::llround(requested_min[axis] / chunk * subchunks));
            // Snap in integer half-chunk/probe space and retain only the small
            // positive remainder, so million-chunk worlds never become a large
            // float before returning to the local rendering frame.
            const i64 numerator =
                (anchor_chunks[axis] * subchunks + min_subchunks) * probe_count;
            const i64 remainder = radiance_floor_mod(numerator, span_subchunks);
            *components[axis] = chunk * (
                static_cast<float>(min_subchunks) / static_cast<float>(subchunks) -
                static_cast<float>(remainder) /
                    static_cast<float>(subchunks * probe_count));
        }
    }
    return result;
}

static void set_radiance_common_uniforms(
    Shader shader,
    RenderState* renderer,
    const Dimension* dim,
    const RadianceSceneGpuView& scene,
    Vector3 volume_min,
    Vector3 volume_size) {
    const int extra = std::clamp(renderer->lighting.probe_extra_levels, 0, 2);
    const int probe_count = 8 << extra;
    const int level_count = 4;
    const int angular_res = 4;
    const int indirect = std::clamp(renderer->lighting.indirect_samples, 1, 64);
    const int shadows = std::clamp(renderer->lighting.shadow_samples, 1, 16);
    const int emitter_mesh_count = scene.emissive_mesh_count;
    const int frame = static_cast<int>(renderer->radiance_frame & 0x7fffffffu);
    const int jitter = renderer->lighting.jitter ? 1 : 0;
    u64 common_signature = 1469598103934665603ull;
    for (u64 value : {
             static_cast<u64>(shader.id), static_cast<u64>(scene.anchor.dimension),
             static_cast<u64>(static_cast<u32>(scene.anchor.chunk.x)),
             static_cast<u64>(static_cast<u32>(scene.anchor.chunk.y)),
             static_cast<u64>(static_cast<u32>(scene.anchor.chunk.z)),
             static_cast<u64>(scene.triangles_ssbo), static_cast<u64>(scene.emitters_ssbo),
             static_cast<u64>(scene.bvh_nodes_ssbo),
             static_cast<u64>(scene.dynamic_triangles_ssbo),
             static_cast<u64>(scene.dynamic_bvh_nodes_ssbo),
             static_cast<u64>(static_cast<u32>(scene.triangle_count)),
             static_cast<u64>(static_cast<u32>(scene.emitter_count)),
             static_cast<u64>(static_cast<u32>(scene.bvh_node_count)),
             static_cast<u64>(static_cast<u32>(scene.dynamic_triangle_count)),
             static_cast<u64>(static_cast<u32>(scene.dynamic_bvh_node_count)),
             static_cast<u64>(static_cast<u32>(emitter_mesh_count)),
             static_cast<u64>(static_cast<u32>(probe_count)),
             static_cast<u64>(static_cast<u32>(level_count)),
             static_cast<u64>(static_cast<u32>(indirect)),
             static_cast<u64>(static_cast<u32>(shadows)),
             static_cast<u64>(static_cast<u32>(frame)),
             static_cast<u64>(static_cast<u32>(jitter)),
             static_cast<u64>(ColorToInt(dim->sky_top)),
             static_cast<u64>(ColorToInt(dim->sky_bottom))}) {
        common_signature = radiance_hash_add(common_signature, value);
    }
    for (float value : {
             scene.emitter_total_weight, volume_min.x, volume_min.y, volume_min.z,
             volume_size.x, volume_size.y, volume_size.z, dim->chunk_size_m}) {
        common_signature = radiance_hash_float(common_signature, value);
    }
    const auto cached_common = renderer->radiance_common_uniform_signatures.find(shader.id);
    if (cached_common != renderer->radiance_common_uniform_signatures.end() &&
        cached_common->second == common_signature) {
        ++renderer->debug_radiance_common_uniform_cache_hits;
        return;
    }
    renderer->radiance_common_uniform_signatures[shader.id] = common_signature;
    ++renderer->debug_radiance_common_uniform_updates;
    auto location = [&](const char* name) { return cached_shader_location(renderer, shader, name); };
    SetShaderValue(shader, location("TRIANGLE_COUNT"), &scene.triangle_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("EMITTER_COUNT"), &scene.emitter_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("EMITTER_MESH_COUNT"), &emitter_mesh_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("BVH_NODE_COUNT"), &scene.bvh_node_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("DYNAMIC_TRIANGLE_COUNT"), &scene.dynamic_triangle_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("DYNAMIC_BVH_NODE_COUNT"), &scene.dynamic_bvh_node_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("EMITTER_TOTAL_WEIGHT"), &scene.emitter_total_weight, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, location("LEVEL_COUNT"), &level_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("PROBE_COUNT_0"), &probe_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("ANGULAR_RES_0"), &angular_res, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("INDIRECT_SAMPLE_COUNT"), &indirect, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("SHADOW_SAMPLE_COUNT"), &shadows, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("FRAME_INDEX"), &frame, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("JITTER_ENABLED"), &jitter, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("VOLUME_MIN"), &volume_min, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("VOLUME_SIZE"), &volume_size, SHADER_UNIFORM_VEC3);
    const std::array<Vector3, 4> level_volume_mins = radiance_level_volume_mins(
        dim, scene, volume_min, volume_size, probe_count);
    SetShaderValueV(
        shader, location("LEVEL_VOLUME_MIN[0]"), level_volume_mins.data(),
        SHADER_UNIFORM_VEC3, static_cast<int>(level_volume_mins.size()));
    const Vector3 sky_top = linear_color(dim->sky_top) * 0.55f;
    const Vector3 sky_bottom = linear_color(dim->sky_bottom) * 0.55f;
    const float maximum_trace = safe_len(volume_size) + dim->chunk_size_m;
    SetShaderValue(shader, location("SKY_TOP_RADIANCE"), &sky_top, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("SKY_BOTTOM_RADIANCE"), &sky_bottom, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("MAX_TRACE_DISTANCE"), &maximum_trace, SHADER_UNIFORM_FLOAT);
}

static bool upload_radiance_scene_buffers(
    u32* triangles_ssbo,
    u32* triangles_capacity,
    u32* emitters_ssbo,
    u32* emitters_capacity,
    u32* bvh_nodes_ssbo,
    u32* bvh_nodes_capacity,
    const std::vector<GpuRadianceTriangle>& triangles,
    const std::vector<GpuRadianceEmitter>& emitters,
    const std::vector<GpuRadianceBvhNode>& bvh_nodes) {
    return update_edge_filter_buffer(
               triangles_ssbo, triangles_capacity,
               triangles.empty() ? nullptr : triangles.data(),
               triangles.size() * sizeof(GpuRadianceTriangle), RL_DYNAMIC_DRAW) &&
        update_edge_filter_buffer(
               emitters_ssbo, emitters_capacity,
               emitters.empty() ? nullptr : emitters.data(),
               emitters.size() * sizeof(GpuRadianceEmitter), RL_DYNAMIC_DRAW) &&
        update_edge_filter_buffer(
               bvh_nodes_ssbo, bvh_nodes_capacity,
               bvh_nodes.empty() ? nullptr : bvh_nodes.data(),
               bvh_nodes.size() * sizeof(GpuRadianceBvhNode), RL_DYNAMIC_DRAW);
}

static bool upload_radiance_scene(
    RenderState* renderer,
    const std::vector<GpuRadianceTriangle>& triangles,
    const std::vector<GpuRadianceEmitter>& emitters,
    const std::vector<GpuRadianceBvhNode>& bvh_nodes) {
    if (!upload_radiance_scene_buffers(
            &renderer->radiance_scene_triangles_ssbo, &renderer->radiance_scene_triangles_capacity,
            &renderer->radiance_emitters_ssbo, &renderer->radiance_emitters_capacity,
            &renderer->radiance_bvh_nodes_ssbo, &renderer->radiance_bvh_nodes_capacity,
            triangles, emitters, bvh_nodes)) return false;
    rlBindShaderBuffer(renderer->radiance_scene_triangles_ssbo, 11);
    rlBindShaderBuffer(renderer->radiance_emitters_ssbo, 12);
    rlBindShaderBuffer(renderer->radiance_bvh_nodes_ssbo, 13);
    return true;
}

static bool upload_radiance_dynamic_meshes(
    RenderState* renderer,
    const std::vector<GpuRadianceTriangle>& dynamic_triangles,
    const std::vector<GpuRadianceBvhNode>& dynamic_bvh_nodes) {
    if (!update_edge_filter_buffer(
            &renderer->radiance_dynamic_triangles_ssbo,
            &renderer->radiance_dynamic_triangles_capacity,
            dynamic_triangles.empty() ? nullptr : dynamic_triangles.data(),
            dynamic_triangles.size() * sizeof(GpuRadianceTriangle), RL_DYNAMIC_DRAW) ||
        !update_edge_filter_buffer(
            &renderer->radiance_dynamic_bvh_nodes_ssbo,
            &renderer->radiance_dynamic_bvh_nodes_capacity,
            dynamic_bvh_nodes.empty() ? nullptr : dynamic_bvh_nodes.data(),
            dynamic_bvh_nodes.size() * sizeof(GpuRadianceBvhNode), RL_DYNAMIC_DRAW)) return false;
    rlBindShaderBuffer(renderer->radiance_dynamic_triangles_ssbo, 14);
    rlBindShaderBuffer(renderer->radiance_dynamic_bvh_nodes_ssbo, 15);
    renderer->debug_radiance_dynamic_triangle_count = static_cast<u32>(dynamic_triangles.size());
    renderer->debug_radiance_dynamic_bvh_node_count = static_cast<u32>(dynamic_bvh_nodes.size());
    return true;
}

static bool update_pathtrace_comparison_scene(
    RenderState* renderer,
    Dimension* dim,
    const CameraView& view,
    u32 local_player_id) {
    if (!renderer || !dim || !renderer->pathtrace_comparison_shader_ready) return false;
    const WorldPos anchor = radiance_anchor(dim, view);
    const u64 static_scene_signature =
        calculate_pathtrace_static_scene_signature(dim, anchor);
    const bool rebuild_static =
        renderer->pathtrace_scene_triangles_ssbo == 0 ||
        renderer->pathtrace_bvh_nodes_ssbo == 0 ||
        renderer->pathtrace_dimension != view.anchor.dimension ||
        renderer->pathtrace_anchor_chunk.x != anchor.chunk.x ||
        renderer->pathtrace_anchor_chunk.y != anchor.chunk.y ||
        renderer->pathtrace_anchor_chunk.z != anchor.chunk.z ||
        renderer->pathtrace_render_radius_chunks != dim->render_radius_chunks ||
        renderer->pathtrace_quality_radius_chunks != dim->quality_render_radius_chunks ||
        renderer->pathtrace_topology_revision != dim->mesh_topology_revision ||
        renderer->pathtrace_scene_signature != static_scene_signature;

    renderer->debug_pathtrace_scene_build_ms = 0.0;
    if (rebuild_static) {
        const double build_start = GetTime();
        std::vector<GpuPathtraceTriangle> triangles{};
        std::vector<GpuRadianceBvhNode> bvh_nodes{};
        bool truncated = false;
        build_pathtrace_static_scene(
            renderer, dim, anchor, &triangles, &bvh_nodes, &truncated);
        if (!update_edge_filter_buffer(
                &renderer->pathtrace_scene_triangles_ssbo,
                &renderer->pathtrace_scene_triangles_capacity,
                triangles.empty() ? nullptr : triangles.data(),
                triangles.size() * sizeof(GpuPathtraceTriangle), RL_DYNAMIC_DRAW) ||
            !update_edge_filter_buffer(
                &renderer->pathtrace_bvh_nodes_ssbo,
                &renderer->pathtrace_bvh_nodes_capacity,
                bvh_nodes.empty() ? nullptr : bvh_nodes.data(),
                bvh_nodes.size() * sizeof(GpuRadianceBvhNode), RL_DYNAMIC_DRAW)) {
            return false;
        }
        renderer->pathtrace_triangle_count = static_cast<u32>(triangles.size());
        renderer->pathtrace_bvh_node_count = static_cast<u32>(bvh_nodes.size());
        renderer->pathtrace_scene_truncated = truncated;
        renderer->pathtrace_dimension = view.anchor.dimension;
        renderer->pathtrace_anchor_chunk = anchor.chunk;
        renderer->pathtrace_render_radius_chunks = dim->render_radius_chunks;
        renderer->pathtrace_quality_radius_chunks = dim->quality_render_radius_chunks;
        renderer->pathtrace_topology_revision = dim->mesh_topology_revision;
        renderer->pathtrace_scene_signature = static_scene_signature;
        ++renderer->pathtrace_scene_build_count;
        renderer->debug_pathtrace_scene_build_ms =
            (GetTime() - build_start) * 1000.0;
    }

    std::vector<GpuRadianceEmitter> emitters{};
    float emitter_total_weight = 0.0f;
    u32 emitter_mesh_count = 0;
    build_radiance_emitters_in_radius(
        renderer, dim, anchor,
        static_cast<float>(std::max(1, dim->render_radius_chunks)), 1024u,
        &emitters, &emitter_total_weight, &emitter_mesh_count);
    if (!update_edge_filter_buffer(
            &renderer->pathtrace_emitters_ssbo,
            &renderer->pathtrace_emitters_capacity,
            emitters.empty() ? nullptr : emitters.data(),
            emitters.size() * sizeof(GpuRadianceEmitter), RL_DYNAMIC_DRAW)) {
        return false;
    }
    renderer->pathtrace_emitter_triangle_count = static_cast<u32>(emitters.size());
    renderer->pathtrace_emitter_mesh_count = emitter_mesh_count;
    renderer->pathtrace_emitter_total_weight = emitter_total_weight;

    std::vector<GpuPathtraceTriangle> dynamic_triangles{};
    std::vector<GpuRadianceBvhNode> dynamic_bvh_nodes{};
    build_pathtrace_player_meshes(
        dim, anchor, local_player_id, &dynamic_triangles, &dynamic_bvh_nodes);
    if (!update_edge_filter_buffer(
            &renderer->pathtrace_dynamic_triangles_ssbo,
            &renderer->pathtrace_dynamic_triangles_capacity,
            dynamic_triangles.empty() ? nullptr : dynamic_triangles.data(),
            dynamic_triangles.size() * sizeof(GpuPathtraceTriangle), RL_DYNAMIC_DRAW) ||
        !update_edge_filter_buffer(
            &renderer->pathtrace_dynamic_bvh_nodes_ssbo,
            &renderer->pathtrace_dynamic_bvh_nodes_capacity,
            dynamic_bvh_nodes.empty() ? nullptr : dynamic_bvh_nodes.data(),
            dynamic_bvh_nodes.size() * sizeof(GpuRadianceBvhNode), RL_DYNAMIC_DRAW)) {
        return false;
    }
    renderer->pathtrace_dynamic_triangle_count =
        static_cast<u32>(dynamic_triangles.size());
    renderer->pathtrace_dynamic_bvh_node_count =
        static_cast<u32>(dynamic_bvh_nodes.size());
    return true;
}

static bool ensure_pathtrace_comparison_target(RenderState* renderer) {
    if (!renderer || renderer->native_w <= 0 || renderer->native_h <= 0) return false;
    if (IsRenderTextureValid(renderer->pathtrace_comparison_target) &&
        renderer->pathtrace_comparison_target.texture.width == renderer->native_w &&
        renderer->pathtrace_comparison_target.texture.height == renderer->native_h) {
        return true;
    }
    if (IsRenderTextureValid(renderer->pathtrace_comparison_target)) {
        UnloadRenderTexture(renderer->pathtrace_comparison_target);
        renderer->pathtrace_comparison_target = {};
    }
    renderer->pathtrace_comparison_target =
        LoadRenderTexture(renderer->native_w, renderer->native_h);
    if (!IsRenderTextureValid(renderer->pathtrace_comparison_target)) return false;
    SetTextureFilter(
        renderer->pathtrace_comparison_target.texture, TEXTURE_FILTER_POINT);
    return true;
}

static bool render_pathtrace_comparison(
    RenderState* renderer,
    const Dimension* dim,
    const CameraView& view) {
    if (!renderer || !dim || !renderer->pathtrace_comparison_enabled ||
        !renderer->pathtrace_comparison_shader_ready ||
        !ensure_pathtrace_comparison_target(renderer)) return false;

    const double draw_start = GetTime();
    Shader shader = renderer->pathtrace_comparison_shader;
    auto location = [&](const char* name) {
        return cached_shader_location(renderer, shader, name);
    };
    const float resolution[2] = {
        static_cast<float>(renderer->native_w),
        static_cast<float>(renderer->native_h)};
    const WorldPos scene_anchor = {
        view.anchor.dimension, renderer->pathtrace_anchor_chunk, {}};
    const Vector3 camera_position =
        world_delta_meters(view.anchor, scene_anchor, dim->chunk_size_m) +
        Vector3{0.0f, view.eye_height, 0.0f};
    const Vector3 camera_forward =
        safe_norm(forward_from_angles(view.yaw, view.pitch), {0.0f, 0.0f, -1.0f});
    const Vector3 camera_right = safe_norm(
        Vector3CrossProduct(camera_forward, {0.0f, 1.0f, 0.0f}),
        {1.0f, 0.0f, 0.0f});
    const Vector3 camera_up = safe_norm(
        Vector3CrossProduct(camera_right, camera_forward),
        {0.0f, 1.0f, 0.0f});
    const float tan_half_fov = std::tan(renderer->fov * DEG2RAD * 0.5f);
    const int triangle_count = static_cast<int>(renderer->pathtrace_triangle_count);
    const int emitter_count =
        static_cast<int>(renderer->pathtrace_emitter_triangle_count);
    const int emitter_mesh_count =
        static_cast<int>(renderer->pathtrace_emitter_mesh_count);
    const int bvh_node_count = static_cast<int>(renderer->pathtrace_bvh_node_count);
    const int dynamic_triangle_count =
        static_cast<int>(renderer->pathtrace_dynamic_triangle_count);
    const int dynamic_bvh_node_count =
        static_cast<int>(renderer->pathtrace_dynamic_bvh_node_count);
    const Vector3 sky_top = linear_color(dim->sky_top) * 0.55f;
    const Vector3 sky_bottom = linear_color(dim->sky_bottom) * 0.55f;
    const float max_trace_distance = dim->chunk_size_m *
        (static_cast<float>(std::max(1, dim->render_radius_chunks)) * 2.0f + 3.0f);

    SetShaderValue(shader, location("RESOLUTION"), resolution, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader, location("CAMERA_POSITION"), &camera_position, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("CAMERA_FORWARD"), &camera_forward, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("CAMERA_RIGHT"), &camera_right, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("CAMERA_UP"), &camera_up, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("TAN_HALF_FOV"), &tan_half_fov, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, location("TRIANGLE_COUNT"), &triangle_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("EMITTER_COUNT"), &emitter_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("EMITTER_MESH_COUNT"), &emitter_mesh_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("BVH_NODE_COUNT"), &bvh_node_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("DYNAMIC_TRIANGLE_COUNT"), &dynamic_triangle_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("DYNAMIC_BVH_NODE_COUNT"), &dynamic_bvh_node_count, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("EMITTER_TOTAL_WEIGHT"),
        &renderer->pathtrace_emitter_total_weight, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, location("SKY_TOP_RADIANCE"), &sky_top, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("SKY_BOTTOM_RADIANCE"), &sky_bottom, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("MAX_TRACE_DISTANCE"),
        &max_trace_distance, SHADER_UNIFORM_FLOAT);

    BeginTextureMode(renderer->pathtrace_comparison_target);
    ClearBackground(BLACK);
    BeginShaderMode(shader);
    rlEnableShader(shader.id);
    rlSetUniformSampler(location("GRID_TEXTURE"), renderer->grid_texture.id);
    rlSetUniformSampler(location("GRASS_TEXTURE"), renderer->grass_texture.id);
    rlSetUniformSampler(location("STONE_TEXTURE"), renderer->stone_texture.id);
    rlSetUniformSampler(location("ROOF_TEXTURE"), renderer->roof_texture.id);
    rlBindShaderBuffer(renderer->pathtrace_scene_triangles_ssbo, 11);
    rlBindShaderBuffer(renderer->pathtrace_emitters_ssbo, 12);
    rlBindShaderBuffer(renderer->pathtrace_bvh_nodes_ssbo, 13);
    rlBindShaderBuffer(renderer->pathtrace_dynamic_triangles_ssbo, 14);
    rlBindShaderBuffer(renderer->pathtrace_dynamic_bvh_nodes_ssbo, 15);
    DrawTexturePro(
        renderer->target.texture,
        {0.0f, 0.0f, static_cast<float>(renderer->native_w),
         -static_cast<float>(renderer->native_h)},
        {0.0f, 0.0f, static_cast<float>(renderer->native_w),
         static_cast<float>(renderer->native_h)},
        {}, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();
    renderer->debug_pathtrace_draw_ms = (GetTime() - draw_start) * 1000.0;
    return true;
}

static bool update_radiance_cascades(
    RenderState* renderer,
    Dimension* dim,
    const RadianceSceneGpuView& scene,
    RenderTexture2D targets[2],
    u32* active_target,
    int* atlas_width,
    int* atlas_height,
    Vector3 volume_min,
    Vector3 volume_size,
    int first_iteration = 0,
    int maximum_iterations = -1) {
    if (!renderer->radiance_cascade_shader_ready ||
        !ensure_radiance_target_set(renderer, targets, active_target, atlas_width, atlas_height)) return false;
    bind_radiance_scene_buffers(renderer, scene);
    Shader shader = renderer->radiance_cascade_shader;
    set_radiance_common_uniforms(shader, renderer, dim, scene, volume_min, volume_size);
    const float interval_scale = fmaxf(0.5f, dim->chunk_size_m / 8.0f);
    const float max_trace = safe_len(volume_size) + dim->chunk_size_m;
    SetShaderValue(shader, cached_shader_location(renderer, shader, "INTERVAL_SCALE"), &interval_scale, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, cached_shader_location(renderer, shader, "MAX_TRACE_DISTANCE"), &max_trace, SHADER_UNIFORM_FLOAT);

    if (first_iteration <= 0) {
        BeginTextureMode(targets[0]);
        ClearBackground(BLANK);
        EndTextureMode();
        *active_target = 0;
    }
    const int iterations = std::clamp(renderer->lighting.cascade_iterations, 1, 3);
    const int iteration_end = maximum_iterations > 0
        ? std::min(iterations, first_iteration + maximum_iterations) : iterations;
    for (int iteration = std::max(0, first_iteration); iteration < iteration_end; ++iteration) {
        const int write = 1 - static_cast<int>(*active_target);
        SetShaderValue(shader, cached_shader_location(renderer, shader, "ITERATION"), &iteration, SHADER_UNIFORM_INT);
        BeginTextureMode(targets[write]);
        ClearBackground(BLANK);
        BeginShaderMode(shader);
        bind_radiance_scene_buffers(renderer, scene);
        DrawTextureRec(
            targets[*active_target].texture,
            {0.0f, 0.0f,
             static_cast<float>(*atlas_width),
             -static_cast<float>(*atlas_height)},
            {}, WHITE);
        EndShaderMode();
        EndTextureMode();
        *active_target = static_cast<u32>(write);
    }
    return true;
}

static u64 radiance_camera_signature(const Dimension* dim, const CameraView& view) {
    const WorldPos anchor = radiance_anchor(dim, view);
    const Vector3 camera = world_delta_meters(view.anchor, anchor, dim->chunk_size_m) + Vector3{0.0f, view.eye_height, 0.0f};
    u64 hash = 1469598103934665603ull;
    hash = radiance_hash_float(hash, camera.x);
    hash = radiance_hash_float(hash, camera.y);
    hash = radiance_hash_float(hash, camera.z);
    return hash;
}

static u64 radiance_dynamic_mesh_signature(const std::vector<GpuRadianceTriangle>& triangles) {
    u64 hash = 1469598103934665603ull;
    hash = radiance_hash_add(hash, triangles.size());
    for (const GpuRadianceTriangle& triangle : triangles) {
        const float values[] = {
            triangle.a.x, triangle.a.y, triangle.a.z,
            triangle.b.x, triangle.b.y, triangle.b.z,
            triangle.c.x, triangle.c.y, triangle.c.z,
            triangle.albedo_reflectivity.x, triangle.albedo_reflectivity.y,
            triangle.albedo_reflectivity.z, triangle.albedo_reflectivity.w};
        for (float value : values) hash = radiance_hash_float(hash, value);
    }
    return hash;
}

struct RadianceSurfaceRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

static bool radiance_surface_region_valid(const RadianceSurfaceRegion& region) {
    return region.width > 0 && region.height > 0;
}

static RadianceSurfaceRegion radiance_surface_region_clamped(
    const RadianceSurfaceRegion& region,
    int width,
    int height) {
    const int x0 = std::clamp(region.x, 0, std::max(0, width));
    const int y0 = std::clamp(region.y, 0, std::max(0, height));
    const int x1 = std::clamp(region.x + std::max(0, region.width), 0, std::max(0, width));
    const int y1 = std::clamp(region.y + std::max(0, region.height), 0, std::max(0, height));
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

static RadianceSurfaceRegion radiance_surface_region_union(
    const RadianceSurfaceRegion& left,
    const RadianceSurfaceRegion& right) {
    if (!radiance_surface_region_valid(left)) return right;
    if (!radiance_surface_region_valid(right)) return left;
    const int x0 = std::min(left.x, right.x);
    const int y0 = std::min(left.y, right.y);
    const int x1 = std::max(left.x + left.width, right.x + right.width);
    const int y1 = std::max(left.y + left.height, right.y + right.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

static bool clear_lighting_surface_shadow_mask(
    MeshLightingSurfaceCache* surface,
    const RadianceSurfaceRegion& requested_region) {
    if (!surface || !IsRenderTextureValid(surface->shadow_mask_texture)) return false;
    const RadianceSurfaceRegion region = radiance_surface_region_clamped(
        requested_region, surface->width, surface->height);
    if (!radiance_surface_region_valid(region)) return true;
    BeginTextureMode(surface->shadow_mask_texture);
    rlDrawRenderBatchActive();
    glEnable(GL_SCISSOR_TEST);
    glScissor(region.x, region.y, region.width, region.height);
    ClearBackground(WHITE);
    rlDrawRenderBatchActive();
    glDisable(GL_SCISSOR_TEST);
    EndTextureMode();
    return true;
}

static void bump_stable_lighting_revision(MeshLightingSurfaceCache* surface) {
    ++surface->stable_lighting_revision;
    if (surface->stable_lighting_revision == 0) ++surface->stable_lighting_revision;
}

static void build_radiance_player_shadow_states(
    const Dimension* dim,
    std::vector<RadiancePlayerShadowState>* states) {
    states->clear();
    if (!dim) return;
    for (u32 player_slot = 0; player_slot < dim->players.count && states->size() < max_players; ++player_slot) {
        const PlayerEntity& player = dim->players.data[player_slot];
        if (!player.connected) continue;
        const WorldPos feet = player_feet_pos(dim, &player);
        if (!id_valid(feet.dimension)) continue;
        states->push_back({
            feet,
            fmaxf(0.01f, player.body_radius),
            fmaxf(player.body_radius * 2.0f, player.current_height)});
    }
}

static u64 radiance_player_shadow_state_signature(
    const std::vector<RadiancePlayerShadowState>& states) {
    u64 hash = 1469598103934665603ull;
    hash = radiance_hash_add(hash, states.size());
    for (const RadiancePlayerShadowState& state : states) {
        hash = radiance_hash_world_pos(hash, state.feet);
        hash = radiance_hash_float(hash, state.radius);
        hash = radiance_hash_float(hash, state.height);
    }
    return hash;
}

static bool player_shadow_region_for_surface(
    const Dimension* dim,
    const MeshInstance& mesh,
    const MeshLightingSurfaceCache& surface,
    const std::vector<RadiancePlayerShadowState>& current_players,
    const std::vector<RadiancePlayerShadowState>& previous_players,
    RadianceSurfaceRegion* out_region) {
    constexpr float shadow_radius_m = 3.5f;
    constexpr float maximum_plane_distance_m = 4.5f;
    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    const float minimum_u = static_cast<float>(surface.grid_min_u) / ppm;
    const float minimum_v = static_cast<float>(surface.grid_min_v) / ppm;
    const float maximum_u = static_cast<float>(surface.grid_min_u + surface.width) / ppm;
    const float maximum_v = static_cast<float>(surface.grid_min_v + surface.height) / ppm;
    bool has_region = false;
    int min_x = surface.width;
    int min_y = surface.height;
    int max_x = 0;
    int max_y = 0;

    auto include_player = [&](const RadiancePlayerShadowState& player) {
        if (player.feet.dimension != mesh.origin.dimension) return;
        const Vector3 feet_from_mesh = world_delta_meters(player.feet, mesh.origin, dim->chunk_size_m);
        const Vector3 center = feet_from_mesh + Vector3{0.0f, player.height * 0.5f, 0.0f};
        const float bounding_radius = std::sqrt(
            player.radius * player.radius + 0.25f * player.height * player.height);
        const float plane_distance = std::fabs(Vector3DotProduct(center, surface.normal) - surface.plane);
        if (plane_distance > maximum_plane_distance_m + bounding_radius) return;

        const float center_u = Vector3DotProduct(center, surface.tangent);
        const float center_v = Vector3DotProduct(center, surface.bitangent);
        const float nearest_u = clampf(center_u, minimum_u, maximum_u);
        const float nearest_v = clampf(center_v, minimum_v, maximum_v);
        const float du = center_u - nearest_u;
        const float dv = center_v - nearest_v;
        const float radius = shadow_radius_m + player.radius;
        if (du * du + dv * dv > radius * radius) return;

        const int x0 = std::clamp(
            static_cast<int>(std::floor((center_u - radius) * ppm)) - surface.grid_min_u - 1,
            0, surface.width);
        const int y0 = std::clamp(
            static_cast<int>(std::floor((center_v - radius) * ppm)) - surface.grid_min_v - 1,
            0, surface.height);
        const int x1 = std::clamp(
            static_cast<int>(std::ceil((center_u + radius) * ppm)) - surface.grid_min_u + 1,
            0, surface.width);
        const int y1 = std::clamp(
            static_cast<int>(std::ceil((center_v + radius) * ppm)) - surface.grid_min_v + 1,
            0, surface.height);
        if (x1 <= x0 || y1 <= y0) return;
        has_region = true;
        min_x = std::min(min_x, x0);
        min_y = std::min(min_y, y0);
        max_x = std::max(max_x, x1);
        max_y = std::max(max_y, y1);
    };
    for (const RadiancePlayerShadowState& player : previous_players) include_player(player);
    for (const RadiancePlayerShadowState& player : current_players) include_player(player);
    if (!has_region) return false;
    *out_region = {min_x, min_y, max_x - min_x, max_y - min_y};
    return out_region->width > 0 && out_region->height > 0;
}

static void merge_pending_player_region(
    MeshLightingSurfaceCache* surface,
    const RadianceSurfaceRegion& region,
    u64 player_signature) {
    if (surface->pending_width <= 0 || surface->pending_height <= 0) {
        surface->pending_x = region.x;
        surface->pending_y = region.y;
        surface->pending_width = region.width;
        surface->pending_height = region.height;
    } else {
        const int x0 = std::min(surface->pending_x, region.x);
        const int y0 = std::min(surface->pending_y, region.y);
        const int x1 = std::max(surface->pending_x + surface->pending_width, region.x + region.width);
        const int y1 = std::max(surface->pending_y + surface->pending_height, region.y + region.height);
        surface->pending_x = x0;
        surface->pending_y = y0;
        surface->pending_width = x1 - x0;
        surface->pending_height = y1 - y0;
    }
    surface->pending_player_signature = player_signature;
}

static const MeshPaintSurfaceCache* lighting_surface_paint(
    const RenderState* renderer,
    const MeshLightingSurfaceCache& surface) {
    for (const MeshPaintSurfaceCache& paint : renderer->mesh_paint_surfaces) {
        if (paint.mesh_id != surface.mesh_id || !IsTextureValid(paint.texture)) continue;
        const float alignment = Vector3DotProduct(paint.normal, surface.normal);
        const float expected_plane = alignment >= 0.0f ? paint.plane : -paint.plane;
        if (std::fabs(alignment) > 0.999f &&
            std::fabs(expected_plane - surface.plane) < 0.005f) return &paint;
    }
    return nullptr;
}

static bool resolve_lighting_surface(
    RenderState* renderer,
    Dimension* dim,
    const CameraView& view,
    MeshLightingSurfaceCache* surface,
    Vector3 volume_min,
    Vector3 volume_size,
    const RadianceSceneGpuView& scene,
    Texture2D cascade_texture,
    u64 camera_signature,
    u64 player_signature,
    const RadianceSurfaceRegion* update_region = nullptr,
    bool static_partial = false,
    bool finalize_partial = true,
    bool count_synchronous_submission = true,
    int requested_mip_level = 0,
    const RadianceSurfaceRegion* dynamic_shadow_region = nullptr) {
    MeshInstance* mesh = arena_get(&dim->meshes, surface->mesh_id);
    if (!mesh) return false;
    const bool dynamic_shadow_only = update_region != nullptr && !static_partial && surface->reflectivity <= 0.001f;
    const bool static_diffuse = !dynamic_shadow_only && surface->reflectivity <= 0.001f;
    const int temporal_frames = static_partial || surface->reflectivity > 0.001f
        ? 0 : std::max(0, renderer->lighting.temporal_frames);
    const bool use_history = !dynamic_shadow_only && temporal_frames > 1;
    if (!ensure_lighting_surface_targets(surface, use_history)) return false;
    if (dynamic_shadow_only && !ensure_lighting_surface_shadow_mask(surface)) return false;
    const int mip_level = static_partial
        ? std::clamp(requested_mip_level, 0,
              std::max(0, surface->texture.texture.mipmaps - 1))
        : 0;
    const int mip_scale = 1 << mip_level;
    const int mip_width = std::max(1, surface->width >> mip_level);
    const int mip_height = std::max(1, surface->height >> mip_level);
    const WorldPos anchor = scene.anchor;
    const Vector3 mesh_origin = world_delta_meters(mesh->origin, anchor, dim->chunk_size_m);
    const Vector3 camera_position = world_delta_meters(view.anchor, anchor, dim->chunk_size_m) + Vector3{0.0f, view.eye_height, 0.0f};
    const Texture2D base = render_texture(renderer, mesh->texture_id);
    const Color tint_color = resolve_mesh_color(mesh);
    const Vector4 tint = ColorNormalize(tint_color);
    const Vector2 grid_min = {static_cast<float>(surface->grid_min_u), static_cast<float>(surface->grid_min_v)};
    const Vector2 grid_size = {static_cast<float>(surface->width), static_cast<float>(surface->height)};
    const Vector2 resolve_target_size = {
        static_cast<float>(mip_width), static_cast<float>(mip_height)};
    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    // Cache the complete world-space lighting result.  Distance decay is a
    // presentation concern and is applied once by draw_lighting_surfaces().
    // Baking it here made a face depend on where the camera first resolved it.
    const float weight = 1.0f;
    const float ambient = dim->ambient;
    const int corner_merge = renderer->lighting.corner_merge ? 1 : 0;
    const MeshPaintSurfaceCache* paint_surface = lighting_surface_paint(renderer, *surface);
    const u64 paint_signature = paint_surface ? paint_surface->paint_hash : 0;
    const bool history_matches = surface->resolved_scene_signature == scene.signature &&
        surface->resolved_emitter_signature == scene.emitter_signature &&
        surface->resolved_paint_revision == paint_signature &&
        (surface->reflectivity <= 0.001f || surface->resolved_player_signature == player_signature) &&
        (surface->reflectivity <= 0.001f || surface->resolved_camera_signature == camera_signature);
    float temporal_blend = 0.0f;
    if (surface->resolved_scene_signature != 0 && temporal_frames > 1) {
        if (history_matches) {
            const u32 samples = std::clamp(surface->temporal_samples, 1u, static_cast<u32>(temporal_frames - 1));
            temporal_blend = static_cast<float>(samples) / static_cast<float>(samples + 1u);
        } else if (surface->resolved_paint_revision == paint_signature) {
            temporal_blend = static_cast<float>(temporal_frames - 1) / static_cast<float>(temporal_frames);
        }
    }
    const int has_paint = paint_surface ? 1 : 0;
    const Vector3 paint_tangent = paint_surface ? paint_surface->tangent : surface->tangent;
    const Vector3 paint_bitangent = paint_surface ? paint_surface->bitangent : surface->bitangent;
    const Vector2 paint_grid_min = paint_surface
        ? Vector2{static_cast<float>(paint_surface->grid_min_u), static_cast<float>(paint_surface->grid_min_v)}
        : grid_min;
    const Vector2 paint_grid_size = paint_surface
        ? Vector2{static_cast<float>(paint_surface->width), static_cast<float>(paint_surface->height)}
        : grid_size;

    Shader shader = renderer->surface_lighting_shader;
    RadianceSceneGpuView shader_scene = scene;
    if (static_diffuse) {
        shader_scene.dynamic_triangle_count = 0;
        shader_scene.dynamic_bvh_node_count = 0;
    }
    set_radiance_common_uniforms(shader, renderer, dim, shader_scene, volume_min, volume_size);
    auto location = [&](const char* name) { return cached_shader_location(renderer, shader, name); };
    SetShaderValue(shader, location("CORNER_MERGE_ENABLED"), &corner_merge, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("MESH_ORIGIN"), &mesh_origin, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("SURFACE_NORMAL"), &surface->normal, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("SURFACE_TANGENT"), &surface->tangent, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("SURFACE_BITANGENT"), &surface->bitangent, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("SURFACE_PLANE"), &surface->plane, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, location("SURFACE_GRID_MIN"), &grid_min, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader, location("SURFACE_GRID_SIZE"), &grid_size, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader, location("RESOLVE_TARGET_SIZE"), &resolve_target_size, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader, location("PIXELS_PER_METER"), &ppm, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, location("MATERIAL_TINT"), &tint, SHADER_UNIFORM_VEC4);
    SetShaderValue(shader, location("MATERIAL_EMISSION"), &surface->emission, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("MATERIAL_REFLECTIVITY"), &surface->reflectivity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, location("CAMERA_POSITION"), &camera_position, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("AMBIENT"), &ambient, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, location("LIGHTING_WEIGHT"), &weight, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, location("TEMPORAL_BLEND"), &temporal_blend, SHADER_UNIFORM_FLOAT);
    const int dynamic_shadow_mode = dynamic_shadow_only ? 1 : 0;
    SetShaderValue(shader, location("DYNAMIC_SHADOW_ONLY"), &dynamic_shadow_mode, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("HAS_PAINT"), &has_paint, SHADER_UNIFORM_INT);
    SetShaderValue(shader, location("PAINT_TANGENT"), &paint_tangent, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("PAINT_BITANGENT"), &paint_bitangent, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, location("PAINT_GRID_MIN"), &paint_grid_min, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader, location("PAINT_GRID_SIZE"), &paint_grid_size, SHADER_UNIFORM_VEC2);

    RenderTexture2D* write_target = dynamic_shadow_only
        ? &surface->shadow_mask_texture
        : (use_history ? &surface->history_texture : &surface->texture);
    RadianceSurfaceRegion mip_region = {0, 0, mip_width, mip_height};
    if (update_region) {
        const int x0 = update_region->x / mip_scale;
        const int y0 = update_region->y / mip_scale;
        const int x1 = (update_region->x + update_region->width + mip_scale - 1) / mip_scale;
        const int y1 = (update_region->y + update_region->height + mip_scale - 1) / mip_scale;
        mip_region = {
            std::clamp(x0, 0, mip_width), std::clamp(y0, 0, mip_height),
            std::clamp(x1, 0, mip_width) - std::clamp(x0, 0, mip_width),
            std::clamp(y1, 0, mip_height) - std::clamp(y0, 0, mip_height)};
    }
    RadianceSurfaceRegion mip_shadow_region = mip_region;
    if (dynamic_shadow_only && dynamic_shadow_region) {
        const int x0 = dynamic_shadow_region->x / mip_scale;
        const int y0 = dynamic_shadow_region->y / mip_scale;
        const int x1 = (dynamic_shadow_region->x + dynamic_shadow_region->width + mip_scale - 1) /
            mip_scale;
        const int y1 = (dynamic_shadow_region->y + dynamic_shadow_region->height + mip_scale - 1) /
            mip_scale;
        mip_shadow_region = {
            std::clamp(x0, 0, mip_width), std::clamp(y0, 0, mip_height),
            std::clamp(x1, 0, mip_width) - std::clamp(x0, 0, mip_width),
            std::clamp(y1, 0, mip_height) - std::clamp(y0, 0, mip_height)};
    }
    RenderTexture2D render_target = *write_target;
    render_target.texture.width = mip_width;
    render_target.texture.height = mip_height;
    BeginTextureMode(render_target);
    if (mip_level > 0) {
        rlDrawRenderBatchActive();
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
            write_target->texture.id, mip_level);
        glViewport(0, 0, mip_width, mip_height);
    }
    if (dynamic_shadow_only) {
        // The stable colour map remains immutable and filtered. Erase the
        // previous/current union in the one-channel mask, then trace only the
        // current shadow footprint below.
        rlDrawRenderBatchActive();
        glEnable(GL_SCISSOR_TEST);
        glScissor(
            mip_region.x, mip_region.y,
            mip_region.width, mip_region.height);
        ClearBackground(WHITE);
        rlDrawRenderBatchActive();
        glScissor(
            mip_shadow_region.x, mip_shadow_region.y,
            mip_shadow_region.width, mip_shadow_region.height);
    } else if (!static_partial) {
        ClearBackground(BLANK);
    }
    if (static_partial && update_region) {
        rlDrawRenderBatchActive();
        glEnable(GL_SCISSOR_TEST);
        glScissor(mip_region.x, mip_region.y, mip_region.width, mip_region.height);
    }
    BeginShaderMode(shader);
    rlEnableShader(shader.id);
    rlSetUniformSampler(location("texture1"), cascade_texture.id);
    if (paint_surface) rlSetUniformSampler(location("texture2"), paint_surface->texture.id);
    rlSetUniformSampler(
        location("texture3"),
        use_history ? surface->texture.texture.id : renderer->white_texture.id);
    bind_radiance_scene_buffers(renderer, shader_scene);
    DrawTexturePro(
        base,
        {0.0f, 0.0f, static_cast<float>(base.width), -static_cast<float>(base.height)},
        {0.0f, 0.0f, static_cast<float>(mip_width), static_cast<float>(mip_height)},
        {}, 0.0f, WHITE);
    EndShaderMode();
    if (dynamic_shadow_only || (static_partial && update_region)) {
        rlDrawRenderBatchActive();
        glDisable(GL_SCISSOR_TEST);
    }
    if (mip_level > 0) {
        rlDrawRenderBatchActive();
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
            write_target->texture.id, 0);
        glViewport(0, 0, surface->width, surface->height);
    }
    EndTextureMode();
    if (!dynamic_shadow_only) bump_stable_lighting_revision(surface);
    if (static_partial && update_region && !dynamic_shadow_only) {
        if (history_matches && surface->valid_width > 0 && surface->valid_height > 0) {
            const int x0 = std::min(surface->valid_x, update_region->x);
            const int y0 = std::min(surface->valid_y, update_region->y);
            const int x1 = std::max(
                surface->valid_x + surface->valid_width,
                update_region->x + update_region->width);
            const int y1 = std::max(
                surface->valid_y + surface->valid_height,
                update_region->y + update_region->height);
            surface->valid_x = x0;
            surface->valid_y = y0;
            surface->valid_width = x1 - x0;
            surface->valid_height = y1 - y0;
            surface->valid_mip_level = std::min(surface->valid_mip_level, mip_level);
        } else {
            surface->valid_x = update_region->x;
            surface->valid_y = update_region->y;
            surface->valid_width = update_region->width;
            surface->valid_height = update_region->height;
            surface->valid_mip_level = mip_level;
        }
        // Stamp each completed tile. Prepared targets are not promotable until
        // their final tile, while visible partial resolves use these stamps to
        // distinguish a same-lighting region expansion from a new lighting
        // state whose current rectangle is the only valid one.
        surface->resolved_scene_signature = scene.signature;
        surface->resolved_emitter_signature = scene.emitter_signature;
        surface->resolved_paint_revision = paint_signature;
        surface->resolved_camera_signature = camera_signature;
        surface->resolved_player_signature = surface->reflectivity > 0.001f
            ? player_signature : 0;
        if (surface->valid_x == 0 && surface->valid_y == 0 &&
            surface->valid_width == surface->width &&
            surface->valid_height == surface->height) {
            surface->fully_initialized = true;
        }
    }
    if (count_synchronous_submission) {
        const RadianceSurfaceRegion submitted_region = dynamic_shadow_only
            ? mip_shadow_region : mip_region;
        renderer->debug_radiance_surface_texels_submitted += update_region
            ? static_cast<u64>(std::max(0, submitted_region.width)) *
                static_cast<u64>(std::max(0, submitted_region.height))
            : static_cast<u64>(mip_width) * static_cast<u64>(mip_height);
    }
    if (static_partial && !finalize_partial) {
        surface->last_update_frame = renderer->radiance_frame;
        return true;
    }
    if (dynamic_shadow_only) {
        // Presentation multiplies this persistent mask into the independently
        // filtered stable lightmap. Temporal history remains untouched.
        surface->dynamic_composite_active = surface->pending_dynamic_shadow_active;
        surface->shadow_mask_signature = player_signature;
    } else if (use_history) {
        std::swap(surface->texture, surface->history_texture);
        GenTextureMipmaps(&surface->texture.texture);
        set_pixel_mipmap_filter(surface->texture.texture);
        if (surface->reflectivity > 0.001f) {
            surface->dynamic_composite_active = false;
        }
        surface->resolved_scene_signature = scene.signature;
        surface->resolved_emitter_signature = scene.emitter_signature;
        surface->resolved_paint_revision = paint_signature;
        surface->resolved_camera_signature = camera_signature;
        surface->resolved_player_signature = surface->reflectivity > 0.001f ? player_signature : 0;
        surface->valid_x = 0;
        surface->valid_y = 0;
        surface->valid_width = surface->width;
        surface->valid_height = surface->height;
        surface->valid_mip_level = 0;
        surface->fully_initialized = true;
    } else {
        if (mip_level > 0) {
            // The current far-field level is sampled directly (LOD is clamped
            // below), so regenerating the whole remaining chain per face would
            // add a driver synchronization without affecting presentation.
        } else {
            GenTextureMipmaps(&surface->texture.texture);
        }
        set_pixel_mipmap_filter(surface->texture.texture);
        if (surface->reflectivity > 0.001f) {
            surface->dynamic_composite_active = false;
        }
        surface->resolved_scene_signature = scene.signature;
        surface->resolved_emitter_signature = scene.emitter_signature;
        surface->resolved_paint_revision = paint_signature;
        surface->resolved_camera_signature = camera_signature;
        surface->resolved_player_signature = surface->reflectivity > 0.001f ? player_signature : 0;
        if (!static_partial) {
            surface->valid_x = 0;
            surface->valid_y = 0;
            surface->valid_width = surface->width;
            surface->valid_height = surface->height;
            surface->valid_mip_level = 0;
            surface->fully_initialized = true;
        }
        glBindTexture(GL_TEXTURE_2D, surface->texture.texture.id);
        glTexParameterf(
            GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD,
            static_cast<float>(std::max(0, surface->valid_mip_level)));
        glTexParameterf(
            GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD,
            surface->valid_mip_level > 0
                ? static_cast<float>(surface->valid_mip_level) : 1000.0f);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (!dynamic_shadow_only) {
        glBindTexture(GL_TEXTURE_2D, surface->texture.texture.id);
        glTexParameterf(
            GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD,
            static_cast<float>(std::max(0, surface->valid_mip_level)));
        glTexParameterf(
            GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD,
            surface->valid_mip_level > 0
                ? static_cast<float>(surface->valid_mip_level) : 1000.0f);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    surface->pending_player_signature = 0;
    surface->pending_x = 0;
    surface->pending_y = 0;
    surface->pending_width = 0;
    surface->pending_height = 0;
    surface->pending_dynamic_shadow_active = false;
    if (!dynamic_shadow_only) {
        surface->temporal_samples = history_matches
            ? std::min(static_cast<u32>(std::max(1, temporal_frames)), surface->temporal_samples + 1u)
            : 1u;
    }
    // The stable-colour cadence must not be delayed by a movement-only shadow
    // update; the two targets have independent lifetimes.
    if (!dynamic_shadow_only) surface->last_update_frame = renderer->radiance_frame;
    return true;
}

static bool lighting_surface_visible_region(
    const RenderState* renderer,
    const MeshLightingSurfaceCache& surface,
    const MeshGeometry& geometry,
    Matrix mesh_basis,
    Vector3 mesh_origin,
    const Camera3D& camera,
    const Matrix& view_matrix,
    const Matrix& projection_matrix,
    float near_plane,
    float pixels_per_meter,
    RadianceSurfaceRegion* out_region,
    float* out_max_pixels_per_texel = nullptr) {
    (void)near_plane;
    const Vector3 point_on_face = mesh_origin + surface.normal * surface.plane;
    if (Vector3DotProduct(surface.normal, camera.position - point_on_face) <= 0.0f) return false;
    struct ClipGridVertex {
        Vector4 clip{};
        Vector2 grid{};
    };
    auto plane_distance = [](const Vector4& clip, int plane) {
        switch (plane) {
            case 0: return clip.x + clip.w;
            case 1: return clip.w - clip.x;
            case 2: return clip.y + clip.w;
            case 3: return clip.w - clip.y;
            case 4: return clip.z + clip.w;
            default: return clip.w - clip.z;
        }
    };
    auto interpolate = [](const ClipGridVertex& a, const ClipGridVertex& b, float t) {
        ClipGridVertex result{};
        result.clip = {
            lerp_float(a.clip.x, b.clip.x, t), lerp_float(a.clip.y, b.clip.y, t),
            lerp_float(a.clip.z, b.clip.z, t), lerp_float(a.clip.w, b.clip.w, t)};
        result.grid = {
            lerp_float(a.grid.x, b.grid.x, t),
            lerp_float(a.grid.y, b.grid.y, t)};
        return result;
    };

    float min_u = std::numeric_limits<float>::max();
    float min_v = std::numeric_limits<float>::max();
    float max_u = -std::numeric_limits<float>::max();
    float max_v = -std::numeric_limits<float>::max();
    bool any_visible = false;
    bool projection_failed = false;
    float max_pixels_per_texel = 0.0f;
    for (u32 triangle_index : surface.triangles) {
        if (triangle_index >= geometry.triangle_count) continue;
        const Triangle triangle = geometry.triangles[triangle_index];
        const u32 indices[3] = {triangle.a, triangle.b, triangle.c};
        ClipGridVertex polygon_a[16]{};
        ClipGridVertex polygon_b[16]{};
        int vertex_count = 3;
        for (int vertex = 0; vertex < 3; ++vertex) {
            const Vector3 local = Vector3Transform(geometry.vertices[indices[vertex]], mesh_basis);
            const Vector3 world = local + mesh_origin;
            Quaternion clip = {world.x, world.y, world.z, 1.0f};
            clip = QuaternionTransform(clip, view_matrix);
            clip = QuaternionTransform(clip, projection_matrix);
            polygon_a[vertex].clip = {clip.x, clip.y, clip.z, clip.w};
            polygon_a[vertex].grid = {
                Vector3DotProduct(local, surface.tangent) * pixels_per_meter,
                Vector3DotProduct(local, surface.bitangent) * pixels_per_meter};
            projection_failed = projection_failed || !std::isfinite(clip.x) ||
                !std::isfinite(clip.y) || !std::isfinite(clip.z) || !std::isfinite(clip.w);
        }
        ClipGridVertex* input = polygon_a;
        ClipGridVertex* output = polygon_b;
        for (int plane = 0; plane < 6 && vertex_count >= 3; ++plane) {
            int output_count = 0;
            for (int vertex = 0; vertex < vertex_count; ++vertex) {
                const ClipGridVertex& current = input[vertex];
                const ClipGridVertex& next = input[(vertex + 1) % vertex_count];
                const float current_distance = plane_distance(current.clip, plane);
                const float next_distance = plane_distance(next.clip, plane);
                const bool current_inside = current_distance >= -0.00001f;
                const bool next_inside = next_distance >= -0.00001f;
                if (current_inside && output_count < 16) output[output_count++] = current;
                if (current_inside != next_inside && output_count < 16) {
                    const float denominator = current_distance - next_distance;
                    if (std::fabs(denominator) > 0.000001f) {
                        output[output_count++] = interpolate(
                            current, next, current_distance / denominator);
                    }
                }
            }
            vertex_count = output_count;
            std::swap(input, output);
        }
        if (vertex_count < 3) continue;
        any_visible = true;
        for (int vertex = 0; vertex < vertex_count; ++vertex) {
            min_u = fminf(min_u, input[vertex].grid.x);
            min_v = fminf(min_v, input[vertex].grid.y);
            max_u = fmaxf(max_u, input[vertex].grid.x);
            max_v = fmaxf(max_v, input[vertex].grid.y);
            const ClipGridVertex& next = input[(vertex + 1) % vertex_count];
            if (std::fabs(input[vertex].clip.w) > 0.000001f &&
                std::fabs(next.clip.w) > 0.000001f) {
                const Vector2 screen = {
                    (input[vertex].clip.x / input[vertex].clip.w + 1.0f) * 0.5f *
                        static_cast<float>(renderer->native_w),
                    (1.0f - input[vertex].clip.y / input[vertex].clip.w) * 0.5f *
                        static_cast<float>(renderer->native_h)};
                const Vector2 next_screen = {
                    (next.clip.x / next.clip.w + 1.0f) * 0.5f *
                        static_cast<float>(renderer->native_w),
                    (1.0f - next.clip.y / next.clip.w) * 0.5f *
                        static_cast<float>(renderer->native_h)};
                const float grid_distance = Vector2Length(next.grid - input[vertex].grid);
                if (grid_distance > 0.0001f) {
                    max_pixels_per_texel = fmaxf(
                        max_pixels_per_texel,
                        Vector2Length(next_screen - screen) / grid_distance);
                }
            }
        }
    }
    if (!any_visible) return false;
    if (out_region) {
        if (projection_failed) {
            *out_region = {0, 0, surface.width, surface.height};
        } else {
            // Covers the widest configured anisotropic footprint and a small
            // projection-rounding margin at the screen boundary.
            constexpr int guard_texels = 8;
            const int x0 = std::clamp(
                static_cast<int>(std::floor(min_u)) - surface.grid_min_u - guard_texels,
                0, surface.width);
            const int y0 = std::clamp(
                static_cast<int>(std::floor(min_v)) - surface.grid_min_v - guard_texels,
                0, surface.height);
            const int x1 = std::clamp(
                static_cast<int>(std::ceil(max_u)) - surface.grid_min_u + guard_texels,
                0, surface.width);
            const int y1 = std::clamp(
                static_cast<int>(std::ceil(max_v)) - surface.grid_min_v + guard_texels,
                0, surface.height);
            *out_region = {x0, y0, x1 - x0, y1 - y0};
        }
    }
    if (out_max_pixels_per_texel) {
        *out_max_pixels_per_texel = projection_failed
            ? std::numeric_limits<float>::max() : max_pixels_per_texel;
    }
    return !out_region || (out_region->width > 0 && out_region->height > 0);
}

static bool lighting_surface_visible(
    const RenderState* renderer,
    const MeshLightingSurfaceCache& surface,
    const MeshGeometry& geometry,
    Matrix mesh_basis,
    Vector3 mesh_origin,
    const Camera3D& camera,
    const Matrix& view_matrix,
    const Matrix& projection_matrix,
    float near_plane) {
    return lighting_surface_visible_region(
        renderer, surface, geometry, mesh_basis, mesh_origin,
        camera, view_matrix, projection_matrix, near_plane, 1.0f, nullptr, nullptr);
}

static void invalidate_prepared_gpu_identity(RadianceBackgroundState* state) {
    if (!state) return;
    clear_prepared_surface_targets(state);
    state->gpu.dimension = invalid_id;
    state->gpu.anchor = {};
    state->gpu.signature = 0;
    state->gpu.emitter_signature = 0;
    state->gpu.settings_signature = 0;
    state->gpu.triangle_count = 0;
    state->gpu.emitter_triangle_count = 0;
    state->gpu.bvh_node_count = 0;
    state->gpu.emissive_mesh_count = 0;
    state->gpu.emitter_total_weight = 0.0f;
    state->gpu.truncated = false;
    state->gpu.uploaded = false;
    state->gpu.cascade_ready = false;
    state->gpu.cascade_iterations_complete = 0;
}

static RadianceSceneGpuView prepared_scene_gpu_view(const RadiancePreparedGpuScene& prepared) {
    WorldPos anchor{};
    anchor.dimension = prepared.dimension;
    anchor.chunk = prepared.anchor;
    return {
        anchor,
        prepared.signature,
        prepared.emitter_signature,
        prepared.triangles_ssbo,
        prepared.emitters_ssbo,
        prepared.bvh_nodes_ssbo,
        0,
        0,
        static_cast<int>(prepared.triangle_count),
        static_cast<int>(prepared.emitter_triangle_count),
        static_cast<int>(prepared.bvh_node_count),
        0,
        0,
        static_cast<int>(prepared.emissive_mesh_count),
        prepared.emitter_total_weight};
}

static bool upload_prepared_gpu_scene(
    RenderState* renderer,
    RadianceBackgroundState* state,
    RadiancePreparedScene&& cpu_scene) {
    RadiancePreparedGpuScene& gpu = state->gpu;
    invalidate_prepared_gpu_identity(state);
    if (!upload_radiance_scene_buffers(
            &gpu.triangles_ssbo, &gpu.triangles_capacity,
            &gpu.emitters_ssbo, &gpu.emitters_capacity,
            &gpu.bvh_nodes_ssbo, &gpu.bvh_nodes_capacity,
            cpu_scene.triangles, cpu_scene.emitters, cpu_scene.bvh_nodes)) return false;
    gpu.dimension = cpu_scene.dimension;
    gpu.anchor = cpu_scene.anchor;
    gpu.signature = cpu_scene.signature;
    gpu.emitter_signature = cpu_scene.emitter_signature;
    gpu.settings_signature = renderer->radiance_settings_signature;
    gpu.triangle_count = static_cast<u32>(cpu_scene.triangles.size());
    gpu.emitter_triangle_count = static_cast<u32>(cpu_scene.emitters.size());
    gpu.bvh_node_count = static_cast<u32>(cpu_scene.bvh_nodes.size());
    gpu.emissive_mesh_count = cpu_scene.emissive_mesh_count;
    gpu.emitter_total_weight = cpu_scene.emitter_total_weight;
    gpu.truncated = cpu_scene.truncated;
    gpu.uploaded = true;
    gpu.cascade_ready = false;
    gpu.cascade_iterations_complete = 0;
    return true;
}

static bool prepared_surface_exists(
    const RadiancePreparedGpuScene& gpu,
    u64 surface_signature) {
    return std::any_of(
        gpu.surfaces.begin(), gpu.surfaces.end(),
        [&](const RadiancePreparedSurface& prepared) {
            return prepared.surface.surface_signature == surface_signature;
        });
}

static void update_neighbor_radiance_prewarm(
    RenderState* renderer,
    Dimension* dim,
    const CameraView& view,
    const Camera3D& camera,
    const Matrix& view_matrix,
    const Matrix& projection_matrix,
    float near_plane) {
    RadianceBackgroundState* state = radiance_background_state(renderer);
    drain_retired_prepared_targets(state, 4);
    renderer->debug_radiance_neighbor_texels_this_frame = 0;

    ChunkCoord target{};
    if (!neighbor_radiance_target(
            renderer, state, dim, view, renderer->radiance_anchor_chunk, &target)) {
        renderer->debug_radiance_neighbor_required_surface_count = 0;
        renderer->debug_radiance_neighbor_target_distance_m = 0.0f;
        const u32 complete_count = static_cast<u32>(std::count_if(
            state->gpu.surfaces.begin(), state->gpu.surfaces.end(),
            [](const RadiancePreparedSurface& prepared) { return prepared.complete; }));
        renderer->debug_radiance_neighbor_scene_ready = state->gpu.uploaded;
        renderer->debug_radiance_neighbor_cascade_ready = state->gpu.cascade_ready;
        renderer->debug_radiance_neighbor_surface_count = static_cast<u32>(state->gpu.surfaces.size());
        renderer->debug_radiance_neighbor_complete_surface_count = complete_count;
        renderer->debug_radiance_neighbor_retired_target_count = static_cast<u32>(state->retired_targets.size());
        return;
    }

    WorldPos target_anchor{};
    target_anchor.dimension = view.anchor.dimension;
    target_anchor.chunk = target;
    WorldPos predicted_view_anchor = view.anchor;
    const i32 target_dx = target.x - view.anchor.chunk.x;
    const i32 target_dz = target.z - view.anchor.chunk.z;
    if (target_dx != 0) {
        predicted_view_anchor.local.x +=
            static_cast<float>(target_dx) * (state->selected_target_distance + 0.25f);
    } else if (target_dz != 0) {
        predicted_view_anchor.local.z +=
            static_cast<float>(target_dz) * (state->selected_target_distance + 0.25f);
    }
    canonicalize(&predicted_view_anchor, dim->chunk_size_m);
    renderer->debug_radiance_neighbor_target_distance_m = state->selected_target_distance;
    renderer->debug_radiance_neighbor_required_surface_count = 0;
    const float target_radius =
        static_cast<float>(std::max(1, renderer->lighting.lighting_radius_chunks));
    for (const MeshLightingSurfaceCache& surface : renderer->lighting_surfaces) {
        if (surface.reflectivity > 0.001f) continue;
        const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
        const float distance = mesh
            ? lighting_surface_nearest_distance_chunks(
                  dim, surface,
                  world_delta_meters(mesh->origin, target_anchor, dim->chunk_size_m))
            : target_radius;
        if (mesh && mesh->visible && distance < target_radius) {
            ++renderer->debug_radiance_neighbor_required_surface_count;
        }
    }
    const u64 target_signature = chunk_equal(state->observed_target, target) &&
        state->observed_signature_frame == renderer->radiance_frame
        ? state->observed_signature
        : calculate_radiance_scene_signature(renderer, dim, target_anchor);
    RadiancePreparedGpuScene& gpu = state->gpu;
    if (gpu.uploaded &&
        (gpu.dimension != target_anchor.dimension || !chunk_equal(gpu.anchor, target) ||
         gpu.signature != target_signature ||
         gpu.settings_signature != renderer->radiance_settings_signature)) {
        invalidate_prepared_gpu_identity(state);
    }

    finish_radiance_background_worker(state);
    if (!gpu.uploaded && state->has_result &&
        state->result.dimension == target_anchor.dimension &&
        chunk_equal(state->result.anchor, target) &&
        state->result.signature == target_signature) {
        RadiancePreparedScene prepared = std::move(state->result);
        state->has_result = false;
        upload_prepared_gpu_scene(renderer, state, std::move(prepared));
    } else if (state->has_result &&
               (state->result.dimension != target_anchor.dimension ||
                !chunk_equal(state->result.anchor, target) ||
                state->result.signature != target_signature)) {
        state->has_result = false;
    }

    if (gpu.uploaded && !gpu.cascade_ready) {
        const float chunk = dim->chunk_size_m;
        const Vector3 volume_min = {
            -chunk * 1.5f, -chunk * 1.5f, -chunk * 1.5f};
        const Vector3 volume_size = {
            chunk * 4.0f, chunk * 4.0f, chunk * 4.0f};
        const RadianceSceneGpuView scene = prepared_scene_gpu_view(gpu);
        if (update_radiance_cascades(
                renderer, dim, scene, gpu.cascade_targets, &gpu.active_target,
                &gpu.atlas_width, &gpu.atlas_height, volume_min, volume_size,
                gpu.cascade_iterations_complete, 1)) {
            ++gpu.cascade_iterations_complete;
            gpu.cascade_ready = gpu.cascade_iterations_complete >=
                std::clamp(renderer->lighting.cascade_iterations, 1, 3);
        }
    }

    if (!gpu.uploaded || !gpu.cascade_ready) {
        renderer->debug_radiance_neighbor_scene_ready = gpu.uploaded;
        renderer->debug_radiance_neighbor_cascade_ready = gpu.cascade_ready;
        renderer->debug_radiance_neighbor_surface_count = static_cast<u32>(gpu.surfaces.size());
        renderer->debug_radiance_neighbor_complete_surface_count = 0;
        renderer->debug_radiance_neighbor_retired_target_count = static_cast<u32>(state->retired_targets.size());
        return;
    }

    // Animated emitters make speculative face colours stale before promotion,
    // which is why the cave's forced A/B prewarm is not a production policy.
    // A stable light set is different: staging exact target-anchor faces over
    // the whole approach removes the one-frame hills/playground seam bake.
    // This decision is content-driven rather than tied to a particular map.
    const bool automatic_static_surface_prewarm =
        renderer->radiance_emitter_stable_frames >= 2;
    renderer->debug_radiance_neighbor_auto_surface_prewarm =
        automatic_static_surface_prewarm &&
        !renderer->radiance_neighbor_surface_prewarm;
    const bool surface_prewarm_enabled =
        renderer->radiance_neighbor_surface_prewarm ||
        automatic_static_surface_prewarm;
    // The prepared scene and cascades remain ready while the player is idle,
    // but surface-target prewarm has no deadline until movement resumes. Avoid
    // spending foreground GL time merely because a stationary camera happens
    // to be close to a chunk seam.
    if (!surface_prewarm_enabled ||
        safe_len(state->smoothed_camera_motion) < 0.001f) {
        u32 complete_count = 0;
        for (const RadiancePreparedSurface& prepared : gpu.surfaces) {
            if (prepared.complete) ++complete_count;
        }
        renderer->debug_radiance_neighbor_scene_ready = true;
        renderer->debug_radiance_neighbor_cascade_ready = true;
        renderer->debug_radiance_neighbor_surface_count = static_cast<u32>(gpu.surfaces.size());
        renderer->debug_radiance_neighbor_complete_surface_count = complete_count;
        renderer->debug_radiance_neighbor_retired_target_count =
            static_cast<u32>(state->retired_targets.size());
        return;
    }

    // Paint is deliberately validated per face. A stroke invalidates only the
    // staged targets it actually touches instead of throwing away the whole
    // neighbor scene and its cascade.
    for (size_t index = 0; index < gpu.surfaces.size();) {
        RadiancePreparedSurface& prepared = gpu.surfaces[index];
        const MeshPaintSurfaceCache* paint = lighting_surface_paint(renderer, prepared.surface);
        const u64 paint_signature = paint ? paint->paint_hash : 0;
        if (paint_signature == prepared.paint_signature) {
            ++index;
            continue;
        }
        retire_prepared_target(state, &prepared.surface.texture);
        retire_prepared_target(state, &prepared.surface.shadow_mask_texture);
        retire_prepared_target(state, &prepared.surface.history_texture);
        gpu.surfaces.erase(gpu.surfaces.begin() + static_cast<std::ptrdiff_t>(index));
    }

    auto find_incomplete = [&]() -> RadiancePreparedSurface* {
        for (RadiancePreparedSurface& prepared : gpu.surfaces) {
            if (!prepared.complete) return &prepared;
        }
        return nullptr;
    };
    struct Candidate {
        u32 index = 0;
        float distance = 0.0f;
        bool viewport_visible = false;
        bool front_facing = false;
    };
    std::vector<Candidate> candidates{};
    size_t candidate_cursor = 0;
    bool candidates_built = false;
    auto build_candidates = [&]() {
        if (candidates_built) return;
        candidates_built = true;
        candidates.reserve(renderer->lighting_surfaces.size());
        const float radius = static_cast<float>(std::max(1, renderer->lighting.lighting_radius_chunks));
        for (u32 index = 0; index < renderer->lighting_surfaces.size(); ++index) {
            const MeshLightingSurfaceCache& surface = renderer->lighting_surfaces[index];
            if (surface.reflectivity > 0.001f ||
                prepared_surface_exists(gpu, surface.surface_signature)) continue;
            const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
            if (!mesh || !mesh->visible) continue;
            const MeshGeometry* geometry = arena_get(&dim->geometries, surface.geometry_id);
            if (!geometry) continue;
            const float distance = lighting_surface_nearest_distance_chunks(
                dim, surface,
                world_delta_meters(mesh->origin, target_anchor, dim->chunk_size_m));
            if (distance >= radius) continue;
            const Vector3 mesh_origin =
                world_delta_meters(mesh->origin, predicted_view_anchor, dim->chunk_size_m);
            const Vector3 point_on_face = mesh_origin + surface.normal * surface.plane;
            const bool front_facing =
                Vector3DotProduct(surface.normal, camera.position - point_on_face) > 0.0f;
            const bool viewport_visible = front_facing && lighting_surface_visible(
                renderer, surface, *geometry, matrix_no_translation(mesh->se3), mesh_origin,
                camera, view_matrix, projection_matrix, near_plane);
            candidates.push_back({index, distance, viewport_visible, front_facing});
        }
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                if (left.viewport_visible != right.viewport_visible) {
                    return left.viewport_visible > right.viewport_visible;
                }
                if (left.front_facing != right.front_facing) {
                    return left.front_facing > right.front_facing;
                }
                return left.distance < right.distance;
            });
    };
    auto add_best_candidate = [&]() -> RadiancePreparedSurface* {
        build_candidates();
        while (candidate_cursor < candidates.size()) {
            const Candidate candidate = candidates[candidate_cursor++];
            const MeshLightingSurfaceCache& source = renderer->lighting_surfaces[candidate.index];
            if (prepared_surface_exists(gpu, source.surface_signature)) continue;
            RadiancePreparedSurface prepared{};
            prepared.surface = source;
            prepared.surface.chunk_distance = candidate.distance;
            prepared.surface.texture = {};
            prepared.surface.shadow_mask_texture = {};
            prepared.surface.history_texture = {};
            prepared.surface.resolved_scene_signature = 0;
            prepared.surface.resolved_emitter_signature = 0;
            prepared.surface.resolved_paint_revision = 0;
            prepared.surface.resolved_camera_signature = 0;
            prepared.surface.resolved_player_signature = 0;
            prepared.surface.shadow_mask_signature = 0;
            prepared.surface.stable_lighting_revision = 0;
            prepared.surface.pending_player_signature = 0;
            prepared.surface.pending_x = 0;
            prepared.surface.pending_y = 0;
            prepared.surface.pending_width = 0;
            prepared.surface.pending_height = 0;
            prepared.surface.pending_shadow_x = 0;
            prepared.surface.pending_shadow_y = 0;
            prepared.surface.pending_shadow_width = 0;
            prepared.surface.pending_shadow_height = 0;
            prepared.surface.active_shadow_x = 0;
            prepared.surface.active_shadow_y = 0;
            prepared.surface.active_shadow_width = 0;
            prepared.surface.active_shadow_height = 0;
            prepared.surface.valid_x = 0;
            prepared.surface.valid_y = 0;
            prepared.surface.valid_width = 0;
            prepared.surface.valid_height = 0;
            prepared.surface.valid_mip_level = 30;
            prepared.surface.fully_initialized = false;
            prepared.surface.pending_dynamic_shadow_active = false;
            prepared.surface.dynamic_composite_active = false;
            prepared.surface.temporal_samples = 0;
            const MeshPaintSurfaceCache* paint = lighting_surface_paint(renderer, prepared.surface);
            prepared.paint_signature = paint ? paint->paint_hash : 0;
            gpu.surfaces.push_back(std::move(prepared));
            return &gpu.surfaces.back();
        }
        return nullptr;
    };

    // Static lighting can use the complete approach interval and a larger
    // amortized budget. Forced animated-light prewarm remains deliberately
    // conservative for the benchmark A/B because those colours expire.
    const u64 texel_budget = automatic_static_surface_prewarm
        ? 64u * 1024u : 16u * 1024u;
    // The hills seam exposes roughly two hundred small faces. Texel budget
    // alone was not the limiter there: two FBO allocations per frame left
    // most cheap faces unprepared despite spare texel capacity. Eight keeps the
    // allocations amortized while allowing the predicted visible set to
    // finish before a normal walking-speed crossing.
    const u32 allocation_budget = automatic_static_surface_prewarm ? 8u : 1u;
    u64 remaining_texels = texel_budget;
    u32 allocations = 0;
    const float chunk = dim->chunk_size_m;
    const Vector3 volume_min = {
        -chunk * 1.5f, -chunk * 1.5f, -chunk * 1.5f};
    const Vector3 volume_size = {
        chunk * 4.0f, chunk * 4.0f, chunk * 4.0f};
    const RadianceSceneGpuView scene = prepared_scene_gpu_view(gpu);
    while (remaining_texels > 0) {
        RadiancePreparedSurface* work = find_incomplete();
        if (!work) {
            if (allocations >= allocation_budget) break;
            work = add_best_candidate();
            if (!work) break;
        }
        if (work->surface.width <= 0 || work->surface.height <= 0 ||
            remaining_texels < static_cast<u64>(work->surface.width)) break;
        const bool allocating = !IsRenderTextureValid(work->surface.texture);
        if (allocating && allocations >= allocation_budget) break;
        const i32 remaining_rows = work->surface.height - work->next_row;
        const i32 rows = std::min(
            remaining_rows,
            std::max(1, static_cast<i32>(remaining_texels / static_cast<u64>(work->surface.width))));
        const RadianceSurfaceRegion region = {0, work->next_row, work->surface.width, rows};
        const bool final_tile = rows >= remaining_rows;
        if (resolve_lighting_surface(
                renderer, dim, view, &work->surface, volume_min, volume_size,
                scene, gpu.cascade_targets[gpu.active_target].texture,
                0, 0, &region, true, final_tile, false)) {
            if (allocating) ++allocations;
            if (allocating) ++renderer->debug_radiance_neighbor_allocations_this_frame;
            work->next_row += rows;
            work->complete = final_tile;
            const u64 rendered = static_cast<u64>(work->surface.width) * static_cast<u64>(rows);
            remaining_texels -= rendered;
            renderer->debug_radiance_neighbor_texels_this_frame += rendered;
        } else {
            break;
        }
    }

    u32 complete_count = 0;
    for (const RadiancePreparedSurface& prepared : gpu.surfaces) {
        if (prepared.complete) ++complete_count;
    }
    renderer->debug_radiance_neighbor_scene_ready = gpu.uploaded;
    renderer->debug_radiance_neighbor_cascade_ready = gpu.cascade_ready;
    renderer->debug_radiance_neighbor_surface_count = static_cast<u32>(gpu.surfaces.size());
    renderer->debug_radiance_neighbor_complete_surface_count = complete_count;
    renderer->debug_radiance_neighbor_retired_target_count = static_cast<u32>(state->retired_targets.size());
}

static bool promote_prepared_gpu_scene(
    RenderState* renderer,
    WorldPos requested_anchor,
    u64 scene_signature) {
    RadianceBackgroundState* state = radiance_background_state(renderer);
    finish_radiance_background_worker(state);
    RadiancePreparedGpuScene& gpu = state->gpu;
    if (!gpu.uploaded || !gpu.cascade_ready ||
        gpu.dimension != requested_anchor.dimension ||
        !chunk_equal(gpu.anchor, requested_anchor.chunk) ||
        gpu.signature != scene_signature ||
        gpu.settings_signature != renderer->radiance_settings_signature ||
        !gpu.triangles_ssbo || !gpu.bvh_nodes_ssbo ||
        !IsRenderTextureValid(gpu.cascade_targets[0]) ||
        !IsRenderTextureValid(gpu.cascade_targets[1])) return false;

    std::swap(renderer->radiance_scene_triangles_ssbo, gpu.triangles_ssbo);
    std::swap(renderer->radiance_emitters_ssbo, gpu.emitters_ssbo);
    std::swap(renderer->radiance_bvh_nodes_ssbo, gpu.bvh_nodes_ssbo);
    std::swap(renderer->radiance_scene_triangles_capacity, gpu.triangles_capacity);
    std::swap(renderer->radiance_emitters_capacity, gpu.emitters_capacity);
    std::swap(renderer->radiance_bvh_nodes_capacity, gpu.bvh_nodes_capacity);
    for (int index = 0; index < 2; ++index) {
        std::swap(renderer->radiance_cascade_targets[index], gpu.cascade_targets[index]);
    }
    std::swap(renderer->radiance_active_target, gpu.active_target);
    std::swap(renderer->radiance_atlas_width, gpu.atlas_width);
    std::swap(renderer->radiance_atlas_height, gpu.atlas_height);
    renderer->radiance_emitter_triangle_count = gpu.emitter_triangle_count;
    renderer->radiance_emitter_total_weight = gpu.emitter_total_weight;
    renderer->debug_radiance_triangle_count = gpu.triangle_count;
    renderer->debug_radiance_emitter_count = gpu.emissive_mesh_count;
    renderer->debug_radiance_bvh_node_count = gpu.bvh_node_count;
    renderer->debug_radiance_scene_truncated = gpu.truncated;
    gpu.uploaded = false;
    gpu.cascade_ready = false;
    gpu.cascade_iterations_complete = 0;
    renderer->debug_radiance_background_scene_used = true;
    renderer->debug_radiance_neighbor_promoted = true;
    return true;
}

static void promote_prepared_surface_targets(
    RenderState* renderer,
    u64 scene_signature) {
    RadianceBackgroundState* state = radiance_background_state(renderer);
    RadiancePreparedGpuScene& gpu = state->gpu;
    for (RadiancePreparedSurface& prepared : gpu.surfaces) {
        bool moved = false;
        if (prepared.complete && prepared.surface.resolved_scene_signature == scene_signature) {
            for (MeshLightingSurfaceCache& destination : renderer->lighting_surfaces) {
                if (destination.surface_signature != prepared.surface.surface_signature) continue;
                const MeshPaintSurfaceCache* paint = lighting_surface_paint(renderer, destination);
                const u64 paint_signature = paint ? paint->paint_hash : 0;
                if (paint_signature != prepared.paint_signature ||
                    prepared.surface.resolved_paint_revision != paint_signature) break;
                retire_prepared_target(state, &destination.texture);
                retire_prepared_target(state, &destination.history_texture);
                destination.texture = prepared.surface.texture;
                prepared.surface.texture = {};
                destination.resolved_scene_signature = scene_signature;
                destination.resolved_emitter_signature = prepared.surface.resolved_emitter_signature;
                destination.resolved_paint_revision = paint_signature;
                destination.resolved_camera_signature = 0;
                destination.resolved_player_signature = 0;
                destination.stable_lighting_revision =
                    prepared.surface.stable_lighting_revision;
                destination.valid_x = prepared.surface.valid_x;
                destination.valid_y = prepared.surface.valid_y;
                destination.valid_width = prepared.surface.valid_width;
                destination.valid_height = prepared.surface.valid_height;
                destination.valid_mip_level = prepared.surface.valid_mip_level;
                destination.fully_initialized = prepared.surface.fully_initialized;
                destination.temporal_samples = prepared.surface.temporal_samples;
                destination.last_update_frame = renderer->radiance_frame;
                moved = true;
                ++renderer->debug_radiance_neighbor_promoted_surface_count;
                break;
            }
        }
        if (!moved) retire_prepared_target(state, &prepared.surface.texture);
        retire_prepared_target(state, &prepared.surface.shadow_mask_texture);
        retire_prepared_target(state, &prepared.surface.history_texture);
    }
    gpu.surfaces.clear();
    gpu.dimension = invalid_id;
    gpu.anchor = {};
    gpu.signature = 0;
    gpu.emitter_signature = 0;
    gpu.settings_signature = 0;
    renderer->debug_radiance_neighbor_surface_count = 0;
    renderer->debug_radiance_neighbor_complete_surface_count = 0;
    renderer->debug_radiance_neighbor_retired_target_count = static_cast<u32>(state->retired_targets.size());
}

static void update_radiance_lighting(
    RenderState* renderer,
    Dimension* dim,
    const CameraView& view,
    const Camera3D& camera,
    const Matrix& view_matrix,
    const Matrix& projection_matrix,
    float near_plane) {
    renderer->debug_radiance_surface_updates = 0;
    renderer->debug_radiance_surface_allocations = 0;
    renderer->debug_radiance_surface_texels_submitted = 0;
    renderer->debug_radiance_common_uniform_updates = 0;
    renderer->debug_radiance_common_uniform_cache_hits = 0;
    renderer->debug_radiance_scene_buffer_binds = 0;
    // `0` is a real "unbind this optional SSBO" value. Use an impossible ID
    // as the cache sentinel so a prior PT/prepared pass cannot leave an old
    // buffer resident when the next scene has a zero-count binding.
    renderer->radiance_bound_scene_buffers.fill(invalid_id);
    renderer->debug_radiance_dynamic_shadow_updates = 0;
    renderer->debug_radiance_dynamic_shadow_allocations = 0;
    renderer->debug_radiance_dynamic_shadow_mask_frees = 0;
    renderer->debug_radiance_dynamic_shadow_region_texels = 0;
    renderer->debug_radiance_dynamic_shadow_clear_texels = 0;
    renderer->debug_radiance_dynamic_shadow_copy_texels = 0;
    renderer->debug_radiance_dynamic_shadow_full_copy_texels = 0;
    renderer->debug_radiance_draw_candidate_surface_count = 0;
    renderer->debug_radiance_draw_origin_gated_surface_count = 0;
    renderer->debug_radiance_draw_unresolved_surface_count = 0;
    renderer->debug_radiance_scene_build_ms = 0.0;
    renderer->debug_radiance_signature_ms = 0.0;
    renderer->debug_radiance_emitter_ms = 0.0;
    renderer->debug_radiance_topology_ms = 0.0;
    renderer->debug_radiance_cascade_ms = 0.0;
    renderer->debug_radiance_background_scene_used = false;
    renderer->debug_radiance_neighbor_complete_surface_count = 0;
    renderer->debug_radiance_neighbor_texels_this_frame = 0;
    renderer->debug_radiance_neighbor_promoted = false;
    renderer->debug_radiance_neighbor_required_surface_count = 0;
    renderer->debug_radiance_neighbor_promoted_surface_count = 0;
    renderer->debug_radiance_neighbor_sync_fallback_surface_count = 0;
    renderer->debug_radiance_neighbor_sync_fallback_allocation_count = 0;
    renderer->debug_radiance_neighbor_sync_fallback_texels = 0;
    renderer->debug_radiance_neighbor_partial_fallback_surface_count = 0;
    renderer->debug_radiance_neighbor_partial_fallback_allocation_count = 0;
    renderer->debug_radiance_neighbor_partial_fallback_texels = 0;
    renderer->debug_radiance_neighbor_sync_fallback_support_surface_count = 0;
    renderer->debug_radiance_neighbor_sync_fallback_support_texels = 0;
    renderer->debug_radiance_neighbor_sync_fallback_max_surface_texels = 0;
    renderer->debug_radiance_neighbor_sync_fallback_max_width = 0;
    renderer->debug_radiance_neighbor_sync_fallback_max_height = 0;
    renderer->debug_radiance_neighbor_allocations_this_frame = 0;
    renderer->debug_radiance_neighbor_target_distance_m = 0.0f;
    renderer->debug_radiance_neighbor_auto_surface_prewarm = false;
    ++renderer->radiance_frame;
    if (!renderer->lighting.enabled || !renderer->radiance_cascade_shader_ready ||
        !renderer->surface_lighting_shader_ready ||
        !renderer->surface_shadow_composite_shader_ready) return;
    u64 settings_signature = 1469598103934665603ull;
    settings_signature = radiance_hash_add(settings_signature, renderer->lighting.probe_extra_levels);
    settings_signature = radiance_hash_add(settings_signature, renderer->lighting.cascade_iterations);
    settings_signature = radiance_hash_add(settings_signature, renderer->lighting.indirect_samples);
    settings_signature = radiance_hash_add(settings_signature, renderer->lighting.shadow_samples);
    settings_signature = radiance_hash_add(settings_signature, renderer->lighting.temporal_frames);
    settings_signature = radiance_hash_add(settings_signature, renderer->lighting.lighting_radius_chunks);
    settings_signature = radiance_hash_add(settings_signature, renderer->lighting.jitter ? 1u : 0u);
    settings_signature = radiance_hash_add(settings_signature, renderer->lighting.corner_merge ? 1u : 0u);
    settings_signature = radiance_hash_float(settings_signature, dim->pixels_per_meter);
    if (renderer->radiance_settings_signature != settings_signature) {
        renderer->radiance_settings_signature = settings_signature;
        renderer->radiance_scene_signature = 0;
        renderer->radiance_topology_signature = 0;
        renderer->radiance_cascade_temporal_samples = 0;
        unload_radiance_targets(renderer);
        for (MeshLightingSurfaceCache& surface : renderer->lighting_surfaces) {
            surface.resolved_scene_signature = 0;
            surface.resolved_emitter_signature = 0;
            surface.resolved_paint_revision = 0;
            surface.resolved_camera_signature = 0;
            surface.resolved_player_signature = 0;
            surface.shadow_mask_signature = 0;
            surface.pending_player_signature = 0;
            surface.pending_x = 0;
            surface.pending_y = 0;
            surface.pending_width = 0;
            surface.pending_height = 0;
            surface.pending_shadow_x = 0;
            surface.pending_shadow_y = 0;
            surface.pending_shadow_width = 0;
            surface.pending_shadow_height = 0;
            surface.active_shadow_x = 0;
            surface.active_shadow_y = 0;
            surface.active_shadow_width = 0;
            surface.active_shadow_height = 0;
            surface.valid_x = 0;
            surface.valid_y = 0;
            surface.valid_width = 0;
            surface.valid_height = 0;
            surface.valid_mip_level = 30;
            surface.fully_initialized = false;
            surface.pending_dynamic_shadow_active = false;
            surface.dynamic_composite_active = false;
            surface.temporal_samples = 0;
            if (IsRenderTextureValid(surface.shadow_mask_texture)) {
                unload_color_render_texture(&surface.shadow_mask_texture);
                ++renderer->debug_radiance_dynamic_shadow_mask_frees;
            }
        }
        renderer->radiance_emitter_signature = 0;
        renderer->radiance_emitter_stable_frames = 0;
        renderer->radiance_player_signature = 0;
        renderer->radiance_previous_players.clear();
        invalidate_prepared_gpu_identity(radiance_background_state(renderer));
    }
    std::vector<GpuRadianceTriangle> triangles{};
    std::vector<GpuRadianceEmitter> emitters{};
    std::vector<GpuRadianceBvhNode> bvh_nodes{};
    std::vector<GpuRadianceTriangle> dynamic_triangles{};
    std::vector<GpuRadianceBvhNode> dynamic_bvh_nodes{};
    float emitter_total_weight = 0.0f;
    const WorldPos requested_anchor = radiance_anchor(dim, view);
    const ChunkCoord previous_anchor_chunk = renderer->radiance_anchor_chunk;
    const double signature_start = GetTime();
    const u64 scene_signature = calculate_radiance_scene_signature(renderer, dim, requested_anchor);
    renderer->debug_radiance_signature_ms = (GetTime() - signature_start) * 1000.0;
    const u64 previous_scene_signature = renderer->radiance_scene_signature;
    const bool first_scene_resolve = previous_scene_signature == 0;
    const bool scene_changed = renderer->radiance_scene_signature != scene_signature ||
        renderer->radiance_scene_triangles_ssbo == 0 || renderer->radiance_bvh_nodes_ssbo == 0;
    bool prepared_gpu_promoted = false;
    if (scene_changed) {
        const double scene_build_start = GetTime();
        prepared_gpu_promoted = promote_prepared_gpu_scene(renderer, requested_anchor, scene_signature);
        if (!prepared_gpu_promoted) {
            u64 built_signature = 0;
            u32 emissive_mesh_count = 0;
            bool scene_truncated = false;
            RadiancePreparedScene prepared{};
            if (take_prepared_radiance_scene(
                    renderer, requested_anchor, scene_signature, &prepared)) {
                triangles = std::move(prepared.triangles);
                emitters = std::move(prepared.emitters);
                bvh_nodes = std::move(prepared.bvh_nodes);
                emitter_total_weight = prepared.emitter_total_weight;
                built_signature = prepared.signature;
                emissive_mesh_count = prepared.emissive_mesh_count;
                scene_truncated = prepared.truncated;
                renderer->debug_radiance_background_scene_used = true;
            } else {
                u64 built_emitter_signature = 0;
                build_radiance_scene(
                    renderer, dim, requested_anchor, &triangles, &emitters, &bvh_nodes,
                    &emitter_total_weight, &built_signature, &built_emitter_signature,
                    &emissive_mesh_count, &scene_truncated);
            }
            if (triangles.empty() || bvh_nodes.empty()) {
                renderer->radiance_scene_signature = 0;
                RadianceBackgroundState* background = radiance_background_state(renderer);
                for (MeshLightingSurfaceCache& surface : renderer->lighting_surfaces) {
                    retire_prepared_target(background, &surface.texture);
                    retire_prepared_target(background, &surface.shadow_mask_texture);
                    retire_prepared_target(background, &surface.history_texture);
                }
                renderer->lighting_surfaces.clear();
                renderer->radiance_topology_signature = 0;
                renderer->radiance_cascade_temporal_samples = 0;
                renderer->debug_radiance_surface_count = 0;
                schedule_neighbor_radiance_scene(renderer, dim, view);
                update_neighbor_radiance_prewarm(
                    renderer, dim, view, camera, view_matrix, projection_matrix, near_plane);
                return;
            }
            if (!upload_radiance_scene(renderer, triangles, emitters, bvh_nodes)) return;
            renderer->radiance_emitter_triangle_count = static_cast<u32>(emitters.size());
            renderer->radiance_emitter_total_weight = emitter_total_weight;
            renderer->debug_radiance_triangle_count = static_cast<u32>(triangles.size());
            renderer->debug_radiance_emitter_count = emissive_mesh_count;
            renderer->debug_radiance_bvh_node_count = static_cast<u32>(bvh_nodes.size());
            renderer->debug_radiance_scene_truncated = scene_truncated;
        }
        renderer->debug_radiance_scene_build_ms = (GetTime() - scene_build_start) * 1000.0;
    }

    // Moving emissive meshes are a compact dynamic light set, not static
    // occluding geometry. Refresh only this small SSBO each frame; the static
    // triangle BVH and lighting-surface topology remain valid.
    u32 current_emissive_mesh_count = 0;
    float current_emitter_total_weight = 0.0f;
    const double emitter_start = GetTime();
    const u64 emitter_signature = build_radiance_emitters(
        renderer, dim, requested_anchor, &emitters,
        &current_emitter_total_weight, &current_emissive_mesh_count);
    if (!update_edge_filter_buffer(
            &renderer->radiance_emitters_ssbo,
            &renderer->radiance_emitters_capacity,
            emitters.empty() ? nullptr : emitters.data(),
            emitters.size() * sizeof(GpuRadianceEmitter), RL_DYNAMIC_DRAW)) return;
    const u64 previous_emitter_signature = renderer->radiance_emitter_signature;
    renderer->radiance_emitter_stable_frames =
        previous_emitter_signature != 0 &&
        previous_emitter_signature == emitter_signature
        ? std::min(renderer->radiance_emitter_stable_frames + 1u, 1000000u)
        : 0u;
    renderer->radiance_emitter_signature = emitter_signature;
    renderer->radiance_emitter_triangle_count = static_cast<u32>(emitters.size());
    renderer->radiance_emitter_total_weight = current_emitter_total_weight;
    renderer->debug_radiance_emitter_count = current_emissive_mesh_count;
    renderer->debug_radiance_emitter_ms = (GetTime() - emitter_start) * 1000.0;

    renderer->debug_radiance_player_primitive_count = build_radiance_player_meshes(
        dim, radiance_anchor(dim, view), &dynamic_triangles, &dynamic_bvh_nodes);
    if (!upload_radiance_dynamic_meshes(renderer, dynamic_triangles, dynamic_bvh_nodes)) return;
    const double topology_start = GetTime();
    rebuild_lighting_surfaces(renderer, dim, view);
    if (prepared_gpu_promoted) promote_prepared_surface_targets(renderer, scene_signature);
    renderer->debug_radiance_topology_ms = (GetTime() - topology_start) * 1000.0;
    if (scene_changed) renderer->radiance_cascade_temporal_samples = 0;
    renderer->radiance_scene_signature = scene_signature;
    renderer->radiance_anchor_chunk = view.anchor.chunk;
    const bool anchor_partial_fallback_frame =
        renderer->radiance_anchor_partial_fallback &&
        prepared_gpu_promoted && !first_scene_resolve &&
        !chunk_equal(previous_anchor_chunk, requested_anchor.chunk);

    const float chunk = dim->chunk_size_m;
    // The four-chunk window is centred on the current chunk centre. With eight
    // base probes it trades 6 m for 8 m spacing, but covers the complete 2-chunk
    // full-quality radius on both sides of a seam. Per-level world snapping
    // above keeps every overlapping probe phase-identical across re-anchors.
    const Vector3 volume_min = {
        -chunk * 1.5f, -chunk * 1.5f, -chunk * 1.5f};
    const Vector3 volume_size = {
        chunk * 4.0f, chunk * 4.0f, chunk * 4.0f};
    const RadianceSceneGpuView active_scene = {
        requested_anchor,
        scene_signature,
        emitter_signature,
        renderer->radiance_scene_triangles_ssbo,
        renderer->radiance_emitters_ssbo,
        renderer->radiance_bvh_nodes_ssbo,
        renderer->radiance_dynamic_triangles_ssbo,
        renderer->radiance_dynamic_bvh_nodes_ssbo,
        static_cast<int>(renderer->debug_radiance_triangle_count),
        static_cast<int>(renderer->radiance_emitter_triangle_count),
        static_cast<int>(renderer->debug_radiance_bvh_node_count),
        static_cast<int>(dynamic_triangles.size()),
        static_cast<int>(dynamic_bvh_nodes.size()),
        static_cast<int>(renderer->debug_radiance_emitter_count),
        renderer->radiance_emitter_total_weight};
    RadianceSceneGpuView cascade_scene = active_scene;
    cascade_scene.dynamic_triangle_count = 0;
    cascade_scene.dynamic_bvh_node_count = 0;
    // RC is a live lighting pass, not a static light-map bake. Trace it every rendered
    // frame so moving players and emissive geometry immediately affect the shared probes.
    const double cascade_start = GetTime();
    if (!update_radiance_cascades(
            renderer, dim, cascade_scene,
            renderer->radiance_cascade_targets, &renderer->radiance_active_target,
            &renderer->radiance_atlas_width, &renderer->radiance_atlas_height,
            volume_min, volume_size)) return;
    renderer->debug_radiance_cascade_ms = (GetTime() - cascade_start) * 1000.0;
    renderer->debug_radiance_cascade_update_frame = renderer->radiance_frame;
    if (renderer->lighting.jitter && renderer->lighting.temporal_frames > 1) {
        renderer->radiance_cascade_temporal_samples = std::min(
            renderer->radiance_cascade_temporal_samples + 1u,
            static_cast<u32>(renderer->lighting.temporal_frames));
    } else {
        renderer->radiance_cascade_temporal_samples = 1;
    }

    const u64 camera_signature = radiance_camera_signature(dim, view);
    const u64 player_signature = radiance_dynamic_mesh_signature(dynamic_triangles);
    std::vector<RadiancePlayerShadowState> current_player_states{};
    build_radiance_player_shadow_states(dim, &current_player_states);
    // Moving emissive geometry changes the relative contribution which the
    // player blocks even when every player is stationary. Player shadows use
    // canonical world state so crossing a cascade anchor or changing a player
    // colour does not invalidate an otherwise identical mask.
    u64 player_shadow_signature = radiance_hash_add(
        radiance_player_shadow_state_signature(current_player_states),
        emitter_signature);
    player_shadow_signature = radiance_hash_add(
        player_shadow_signature, ColorToInt(dim->sky_top));
    player_shadow_signature = radiance_hash_add(
        player_shadow_signature, ColorToInt(dim->sky_bottom));
    const u64 reflection_signature = radiance_hash_add(
        radiance_hash_add(camera_signature, player_signature), emitter_signature);
    const u32 surface_count = static_cast<u32>(renderer->lighting_surfaces.size());
    if (!surface_count) {
        renderer->radiance_player_signature = player_signature;
        renderer->radiance_previous_players = std::move(current_player_states);
        schedule_neighbor_radiance_scene(renderer, dim, view);
        update_neighbor_radiance_prewarm(
            renderer, dim, view, camera, view_matrix, projection_matrix, near_plane);
        return;
    }
    const WorldPos anchor = radiance_anchor(dim, view);
    const Vector3 camera_position = world_delta_meters(view.anchor, anchor, dim->chunk_size_m) +
        Vector3{0.0f, view.eye_height, 0.0f};
    std::vector<u8> surface_visible(surface_count, 0);
    std::vector<RadianceSurfaceRegion> surface_visible_regions(surface_count);
    std::vector<float> surface_pixels_per_texel(surface_count, 0.0f);
    const float pixels_per_meter = fmaxf(dim->pixels_per_meter, 1.0f);
    for (u32 index = 0; index < surface_count; ++index) {
        MeshLightingSurfaceCache& surface = renderer->lighting_surfaces[index];
        const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
        const MeshGeometry* geometry = mesh ? arena_get(&dim->geometries, surface.geometry_id) : nullptr;
        if (!mesh || !geometry) continue;
        const Vector3 mesh_origin =
            world_delta_meters(mesh->origin, view.anchor, dim->chunk_size_m);
        surface.chunk_distance = lighting_surface_nearest_distance_chunks(
            dim, surface, mesh_origin);
        if (radiance_lighting_weight(dim, renderer, surface.chunk_distance) <= 0.0f) continue;
        surface_visible[index] = lighting_surface_visible_region(
            renderer, surface, *geometry, matrix_no_translation(mesh->se3), mesh_origin,
            camera, view_matrix, projection_matrix, near_plane, pixels_per_meter,
            &surface_visible_regions[index], &surface_pixels_per_texel[index]) ? 1 : 0;
    }

    auto valid_region_contains = [](const MeshLightingSurfaceCache& surface,
                                    const RadianceSurfaceRegion& region) {
        return surface.valid_width > 0 && surface.valid_height > 0 &&
            region.x >= surface.valid_x && region.y >= surface.valid_y &&
            region.x + region.width <= surface.valid_x + surface.valid_width &&
            region.y + region.height <= surface.valid_y + surface.valid_height;
    };
    auto union_with_valid_region = [](const MeshLightingSurfaceCache& surface,
                                      const RadianceSurfaceRegion& region) {
        if (surface.valid_width <= 0 || surface.valid_height <= 0) return region;
        const int x0 = std::min(surface.valid_x, region.x);
        const int y0 = std::min(surface.valid_y, region.y);
        const int x1 = std::max(
            surface.valid_x + surface.valid_width, region.x + region.width);
        const int y1 = std::max(
            surface.valid_y + surface.valid_height, region.y + region.height);
        return RadianceSurfaceRegion{x0, y0, x1 - x0, y1 - y0};
    };

    // Specular paths are camera-dependent. Like Trivox's camera path, every visible
    // reflective world texel is recomputed this frame. Keep reflective faces
    // full-resolved: their small cave targets are cheap, while partial updates
    // could feed stale offscreen highlights into freshly generated coarse mips.
    for (u32 surface_index = 0; surface_index < surface_count; ++surface_index) {
        MeshLightingSurfaceCache& surface = renderer->lighting_surfaces[surface_index];
        if (surface.reflectivity <= 0.001f) continue;
        const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
        if (!mesh) continue;
        if (!surface_visible[surface_index]) continue;
        const MeshPaintSurfaceCache* paint = lighting_surface_paint(renderer, surface);
        const u64 paint_signature = paint ? paint->paint_hash : 0;
        const bool signatures_match =
            surface.resolved_scene_signature == renderer->radiance_scene_signature &&
            surface.resolved_emitter_signature == emitter_signature &&
            surface.resolved_paint_revision == paint_signature &&
            surface.resolved_camera_signature == reflection_signature &&
            surface.resolved_player_signature == player_signature;
        const RadianceSurfaceRegion visible_region = surface_visible_regions[surface_index];
        if (signatures_match && valid_region_contains(surface, visible_region)) continue;
        const bool allocated_now = !IsRenderTextureValid(surface.texture);
        if (resolve_lighting_surface(
                renderer, dim, view, &surface, volume_min, volume_size,
                active_scene,
                renderer->radiance_cascade_targets[renderer->radiance_active_target].texture,
                reflection_signature, player_signature)) {
            ++renderer->debug_radiance_surface_updates;
            if (allocated_now) ++renderer->debug_radiance_surface_allocations;
        }
    }

    // Resolve every visible texel that can contribute to the current frame.
    // Static scene/paint changes initialize the complete face once. Live
    // emitter changes trace the conservative face/frustum rectangle without
    // leaving any visible texel stale or making work depend on render radius.
    for (u32 surface_index = 0; surface_index < surface_count; ++surface_index) {
        if (!surface_visible[surface_index]) continue;
        MeshLightingSurfaceCache& surface = renderer->lighting_surfaces[surface_index];
        if (surface.reflectivity > 0.001f) continue;
        const bool scene_dirty = surface.resolved_scene_signature != renderer->radiance_scene_signature;
        const bool emitter_dirty = safe_len(surface.emission) <= 0.0001f &&
            surface.resolved_emitter_signature != emitter_signature;
        const MeshPaintSurfaceCache* paint = lighting_surface_paint(renderer, surface);
        const bool paint_dirty = surface.resolved_paint_revision != (paint ? paint->paint_hash : 0);
        const bool temporal_dirty = renderer->lighting.jitter && renderer->lighting.temporal_frames > 1 &&
            surface.temporal_samples < static_cast<u32>(renderer->lighting.temporal_frames);
        const RadianceSurfaceRegion visible_region = surface_visible_regions[surface_index];
        const bool signatures_match = !scene_dirty && !emitter_dirty && !paint_dirty;
        const float pixel_density = surface_pixels_per_texel[surface_index];
        // Preserve exact 16 ppm lighting wherever a world texel is visibly
        // large, then lower resolve density only as its projected footprint
        // becomes sub-pixel. World/chunk distance is intentionally absent.
        const int desired_mip_level = renderer_surface_lighting_mip_level(
            pixel_density, surface.valid_mip_level);
        const bool region_dirty = !valid_region_contains(surface, visible_region) ||
            desired_mip_level < surface.valid_mip_level;
        if (signatures_match && !temporal_dirty && !region_dirty) continue;
        // Any non-initial re-anchor that still has to resolve a visible face on
        // the active context is synchronous fallback work.  This includes a
        // cold/teleport crossing where no prepared context could be promoted,
        // as well as faces that were not complete in a promoted context.
        const bool sync_fallback = scene_changed && !first_scene_resolve && scene_dirty;
        // A matching carried texture is a safe seed on an adjacent prepared
        // anchor: resolve every texel which can contribute to this frame under
        // the new scene, stamp only that coverage, and leave offscreen texels
        // invalid until they become visible. This avoids a full-face seam bake
        // without ever presenting an old scene signature.
        const bool anchor_partial_fallback = anchor_partial_fallback_frame &&
            sync_fallback && IsRenderTextureValid(surface.texture) &&
            !paint_dirty && !temporal_dirty;
        const bool full_resolve = !anchor_partial_fallback &&
            (!surface.fully_initialized || scene_dirty || paint_dirty || temporal_dirty);
        const RadianceSurfaceRegion partial_region = signatures_match
            ? union_with_valid_region(surface, visible_region)
            : visible_region;
        // A fresh/full surface is evaluated at level zero and only then
        // downfiltered. Evaluating the nonlinear lighting shader once at a
        // coarse mip's cell centre produces a different result (and can even
        // average away the material grid), making the image depend on the
        // route used to reach an anchor. A carried old-anchor cache therefore
        // repairs its visible coverage at the canonical texel resolution.
        const int partial_mip_level = anchor_partial_fallback
            ? 0
            : (signatures_match
                ? std::min(surface.valid_mip_level, desired_mip_level)
                : desired_mip_level);
        const bool allocated_now = !IsRenderTextureValid(surface.texture);
        const u64 submitted_before = renderer->debug_radiance_surface_texels_submitted;
        if (resolve_lighting_surface(
                renderer, dim, view, &surface, volume_min, volume_size,
                active_scene,
                renderer->radiance_cascade_targets[renderer->radiance_active_target].texture,
                camera_signature, player_signature,
                full_resolve ? nullptr : &partial_region,
                !full_resolve, true, true, partial_mip_level)) {
            const u64 submitted_texels =
                renderer->debug_radiance_surface_texels_submitted - submitted_before;
            ++renderer->debug_radiance_surface_updates;
            if (allocated_now) ++renderer->debug_radiance_surface_allocations;
            if (sync_fallback) {
                ++renderer->debug_radiance_neighbor_sync_fallback_surface_count;
                if (allocated_now) {
                    ++renderer->debug_radiance_neighbor_sync_fallback_allocation_count;
                }
                renderer->debug_radiance_neighbor_sync_fallback_texels +=
                    submitted_texels;
                if (anchor_partial_fallback) {
                    ++renderer->debug_radiance_neighbor_partial_fallback_surface_count;
                    if (allocated_now) {
                        ++renderer->debug_radiance_neighbor_partial_fallback_allocation_count;
                    }
                    renderer->debug_radiance_neighbor_partial_fallback_texels +=
                        submitted_texels;
                }
                if (surface.normal.y > 0.6f) {
                    ++renderer->debug_radiance_neighbor_sync_fallback_support_surface_count;
                    renderer->debug_radiance_neighbor_sync_fallback_support_texels +=
                        submitted_texels;
                }
                const u64 fallback_surface_texels =
                    static_cast<u64>(surface.width) * static_cast<u64>(surface.height);
                if (fallback_surface_texels >
                    renderer->debug_radiance_neighbor_sync_fallback_max_surface_texels) {
                    renderer->debug_radiance_neighbor_sync_fallback_max_surface_texels =
                        fallback_surface_texels;
                    renderer->debug_radiance_neighbor_sync_fallback_max_width = surface.width;
                    renderer->debug_radiance_neighbor_sync_fallback_max_height = surface.height;
                }
            }
        }
    }
    const int extra_levels = std::clamp(renderer->lighting.probe_extra_levels, 0, 2);
    const u32 budget = static_cast<u32>(scene_changed && !first_scene_resolve
        ? std::max(2, 8 >> extra_levels)
        : std::max(4, 32 >> extra_levels));
    u32 considered = 0;
    u32 diffuse_updates = 0;
    const u32 surface_allocation_budget = static_cast<u32>(std::max(2, 4 >> extra_levels));
    while (considered < surface_count && diffuse_updates < budget) {
        const u32 index = renderer->radiance_surface_cursor++ % surface_count;
        ++considered;
        MeshLightingSurfaceCache& surface = renderer->lighting_surfaces[index];
        if (surface.reflectivity > 0.001f) continue;
        if (surface_visible[index]) continue;
        const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
        if (!mesh) continue;
        const Vector3 view_relative_origin =
            world_delta_meters(mesh->origin, view.anchor, dim->chunk_size_m);
        surface.chunk_distance = lighting_surface_nearest_distance_chunks(
            dim, surface, view_relative_origin);
        const float weight = radiance_lighting_weight(dim, renderer, surface.chunk_distance);
        if (weight <= 0.0f) continue;
        const Vector3 mesh_origin = world_delta_meters(mesh->origin, anchor, dim->chunk_size_m);
        const Vector3 point_on_face = mesh_origin + surface.normal * surface.plane;
        if (Vector3DotProduct(surface.normal, camera_position - point_on_face) <= 0.0f) continue;
        const bool camera_dirty = surface.reflectivity > 0.001f && surface.resolved_camera_signature != camera_signature;
        const bool scene_dirty = surface.resolved_scene_signature != renderer->radiance_scene_signature;
        const MeshPaintSurfaceCache* paint = lighting_surface_paint(renderer, surface);
        const bool paint_dirty = surface.resolved_paint_revision != (paint ? paint->paint_hash : 0);
        const bool temporal_dirty = renderer->lighting.jitter && renderer->lighting.temporal_frames > 1 &&
            surface.temporal_samples < static_cast<u32>(renderer->lighting.temporal_frames);
        if (!camera_dirty && !scene_dirty && !paint_dirty && !temporal_dirty) continue;
        const bool allocated_now = !IsRenderTextureValid(surface.texture);
        if (allocated_now && renderer->debug_radiance_surface_allocations >= surface_allocation_budget) continue;
        const u64 cadence = weight > 0.66f ? 1u : (weight > 0.33f ? 4u : 12u);
        if (renderer->radiance_frame - surface.last_update_frame < cadence) continue;
        if (resolve_lighting_surface(
                renderer, dim, view, &surface, volume_min, volume_size,
                active_scene,
                renderer->radiance_cascade_targets[renderer->radiance_active_target].texture,
                camera_signature, player_signature)) {
            ++renderer->debug_radiance_surface_updates;
            ++diffuse_updates;
            if (allocated_now) ++renderer->debug_radiance_surface_allocations;
        }
    }

    // The persistent mask is complete (white) from allocation onward, so its
    // validity is independent of camera coverage and stable-lightmap updates.
    // Only a changed multiplayer geometry signature needs new shadow work.
    const std::vector<RadiancePlayerShadowState> no_previous_players{};
    for (u32 surface_index = 0; surface_index < surface_count; ++surface_index) {
        MeshLightingSurfaceCache& surface = renderer->lighting_surfaces[surface_index];
        if (surface.resolved_scene_signature != renderer->radiance_scene_signature ||
            surface.reflectivity > 0.001f || safe_len(surface.emission) > 0.0001f) continue;
        if (surface.shadow_mask_signature == player_shadow_signature) continue;
        const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
        if (!mesh) continue;
        RadianceSurfaceRegion current_region{};
        const bool current_player_affects_surface = player_shadow_region_for_surface(
            dim, *mesh, surface, current_player_states,
            no_previous_players, &current_region);
        if (current_player_affects_surface) {
            RadianceSurfaceRegion dirty_region = current_region;
            RadianceSurfaceRegion motion_region{};
            if (player_shadow_region_for_surface(
                    dim, *mesh, surface, current_player_states,
                    renderer->radiance_previous_players, &motion_region)) {
                dirty_region = radiance_surface_region_union(dirty_region, motion_region);
            }
            const RadianceSurfaceRegion active_region = {
                surface.active_shadow_x, surface.active_shadow_y,
                surface.active_shadow_width, surface.active_shadow_height};
            dirty_region = radiance_surface_region_union(dirty_region, active_region);
            dirty_region = radiance_surface_region_clamped(
                dirty_region, surface.width, surface.height);
            merge_pending_player_region(
                &surface, dirty_region, player_shadow_signature);
            surface.pending_shadow_x = current_region.x;
            surface.pending_shadow_y = current_region.y;
            surface.pending_shadow_width = current_region.width;
            surface.pending_shadow_height = current_region.height;
            surface.pending_dynamic_shadow_active = current_player_affects_surface;
        } else {
            const RadianceSurfaceRegion pending_region = {
                surface.pending_x, surface.pending_y,
                surface.pending_width, surface.pending_height};
            const RadianceSurfaceRegion active_region = {
                surface.active_shadow_x, surface.active_shadow_y,
                surface.active_shadow_width, surface.active_shadow_height};
            const RadianceSurfaceRegion clear_region = radiance_surface_region_clamped(
                radiance_surface_region_union(pending_region, active_region),
                surface.width, surface.height);
            if (!radiance_surface_region_valid(clear_region)) {
                surface.shadow_mask_signature = player_shadow_signature;
                surface.pending_dynamic_shadow_active = false;
                surface.dynamic_composite_active = false;
                continue;
            }
            // Preserve a queued offscreen update and the last sampled mask
            // footprint until its cheap white clear has actually executed.
            merge_pending_player_region(
                &surface, clear_region, player_shadow_signature);
            surface.pending_player_signature = player_shadow_signature;
            surface.pending_dynamic_shadow_active = false;
            surface.pending_shadow_x = 0;
            surface.pending_shadow_y = 0;
            surface.pending_shadow_width = 0;
            surface.pending_shadow_height = 0;
        }
    }

    std::vector<u32> player_dirty_surfaces{};
    player_dirty_surfaces.reserve(renderer->lighting_surfaces.size());
    for (u32 index = 0; index < surface_count; ++index) {
        const MeshLightingSurfaceCache& surface = renderer->lighting_surfaces[index];
        if (surface.reflectivity > 0.001f || safe_len(surface.emission) > 0.0001f ||
            surface.resolved_scene_signature != renderer->radiance_scene_signature ||
            surface.pending_width <= 0 || surface.pending_height <= 0) continue;
        player_dirty_surfaces.push_back(index);
    }
    std::stable_sort(player_dirty_surfaces.begin(), player_dirty_surfaces.end(),
        [&](u32 a, u32 b) {
            const MeshLightingSurfaceCache& left = renderer->lighting_surfaces[a];
            const MeshLightingSurfaceCache& right = renderer->lighting_surfaces[b];
            if (surface_visible[a] != surface_visible[b]) return surface_visible[a] > surface_visible[b];
            const bool left_support = left.normal.y > 0.6f;
            const bool right_support = right.normal.y > 0.6f;
            if (left_support != right_support) return left_support;
            return left.chunk_distance < right.chunk_distance;
        });
    const u32 player_surface_budget = static_cast<u32>(std::max(4, 12 >> extra_levels));
    u32 offscreen_player_updates = 0;
    for (u32 dirty_index = 0; dirty_index < player_dirty_surfaces.size(); ++dirty_index) {
        const u32 surface_index = player_dirty_surfaces[dirty_index];
        MeshLightingSurfaceCache& surface = renderer->lighting_surfaces[surface_index];
        if (!surface_visible[surface_index] &&
            offscreen_player_updates >= player_surface_budget) continue;
        if (!surface_visible[surface_index]) ++offscreen_player_updates;
        if (!surface.pending_dynamic_shadow_active) {
            const RadianceSurfaceRegion clear_region = radiance_surface_region_clamped({
                surface.pending_x, surface.pending_y,
                surface.pending_width, surface.pending_height},
                surface.width, surface.height);
            if (clear_lighting_surface_shadow_mask(&surface, clear_region)) {
                renderer->debug_radiance_dynamic_shadow_clear_texels +=
                    static_cast<u64>(std::max(0, clear_region.width)) *
                    static_cast<u64>(std::max(0, clear_region.height));
            }
            surface.dynamic_composite_active = false;
            surface.shadow_mask_signature = surface.pending_player_signature
                ? surface.pending_player_signature : player_shadow_signature;
            surface.pending_player_signature = 0;
            surface.pending_x = 0;
            surface.pending_y = 0;
            surface.pending_width = 0;
            surface.pending_height = 0;
            surface.pending_shadow_x = 0;
            surface.pending_shadow_y = 0;
            surface.pending_shadow_width = 0;
            surface.pending_shadow_height = 0;
            surface.active_shadow_x = 0;
            surface.active_shadow_y = 0;
            surface.active_shadow_width = 0;
            surface.active_shadow_height = 0;
            continue;
        }
        const RadianceSurfaceRegion region = radiance_surface_region_clamped({
            surface.pending_x, surface.pending_y,
            surface.pending_width, surface.pending_height},
            surface.width, surface.height);
        const RadianceSurfaceRegion current_shadow_region = radiance_surface_region_clamped({
            surface.pending_shadow_x, surface.pending_shadow_y,
            surface.pending_shadow_width, surface.pending_shadow_height},
            surface.width, surface.height);
        const bool allocated_mask = !IsRenderTextureValid(surface.shadow_mask_texture);
        if (resolve_lighting_surface(
                renderer, dim, view, &surface, volume_min, volume_size,
                active_scene,
                renderer->radiance_cascade_targets[renderer->radiance_active_target].texture,
                camera_signature, player_shadow_signature, &region,
                false, true, true, 0, &current_shadow_region)) {
            surface.active_shadow_x = current_shadow_region.x;
            surface.active_shadow_y = current_shadow_region.y;
            surface.active_shadow_width = current_shadow_region.width;
            surface.active_shadow_height = current_shadow_region.height;
            surface.pending_shadow_x = 0;
            surface.pending_shadow_y = 0;
            surface.pending_shadow_width = 0;
            surface.pending_shadow_height = 0;
            ++renderer->debug_radiance_surface_updates;
            ++renderer->debug_radiance_dynamic_shadow_updates;
            if (allocated_mask) ++renderer->debug_radiance_dynamic_shadow_allocations;
            renderer->debug_radiance_dynamic_shadow_region_texels +=
                static_cast<u64>(std::max(0, current_shadow_region.width)) *
                static_cast<u64>(std::max(0, current_shadow_region.height));
            renderer->debug_radiance_dynamic_shadow_clear_texels +=
                static_cast<u64>(std::max(0, region.width)) *
                static_cast<u64>(std::max(0, region.height));
            // Counterfactual whole-face copy retained for a direct benchmark A/B.
            renderer->debug_radiance_dynamic_shadow_full_copy_texels +=
                static_cast<u64>(std::max(0, surface.width)) *
                static_cast<u64>(std::max(0, surface.height));
        }
    }
    renderer->radiance_player_signature = player_signature;
    renderer->radiance_previous_players = std::move(current_player_states);
    schedule_neighbor_radiance_scene(renderer, dim, view);
    update_neighbor_radiance_prewarm(
        renderer, dim, view, camera, view_matrix, projection_matrix, near_plane);
}

static void draw_lighting_surfaces(RenderState* renderer, const Dimension* dim, const CameraView& view) {
    if (!renderer || !dim || !renderer->lighting.enabled) return;
    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    rlDrawRenderBatchActive();
    // These are colour replacements for already-rasterized mesh faces. Keep
    // their pulled-forward polygon offset out of the authoritative scene depth
    // used by sprites and edge occlusion.
    rlDisableDepthMask();
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.25f, -1.25f);
    const Shader composite_shader = renderer->surface_shadow_composite_shader;
    const int shadow_mask_location = cached_shader_location(
        renderer, composite_shader, "texture1");
    u32 bound_shadow_mask = invalid_id;
    bool composite_shader_active = false;
    for (const MeshLightingSurfaceCache& surface : renderer->lighting_surfaces) {
        const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
        const MeshGeometry* geometry = mesh ? arena_get(&dim->geometries, surface.geometry_id) : nullptr;
        if (!mesh || !mesh->visible || !geometry) continue;
        const Vector3 origin = world_delta_meters(mesh->origin, view.anchor, dim->chunk_size_m);
        const float inverse_chunk_size = 1.0f / fmaxf(dim->chunk_size_m, 0.001f);
        const float nearest_surface_distance =
            lighting_surface_nearest_distance_chunks(dim, surface, origin);
        if (radiance_lighting_weight(dim, renderer, nearest_surface_distance) <= 0.0f) {
            continue;
        }
        ++renderer->debug_radiance_draw_candidate_surface_count;
        if (radiance_lighting_weight(
                dim, renderer, safe_len(origin) * inverse_chunk_size) <= 0.0f) {
            ++renderer->debug_radiance_draw_origin_gated_surface_count;
        }
        if (surface.resolved_scene_signature != renderer->radiance_scene_signature ||
            !IsRenderTextureValid(surface.texture)) {
            ++renderer->debug_radiance_draw_unresolved_surface_count;
            continue;
        }
        const Matrix basis = matrix_no_translation(mesh->se3);
        const bool dynamic_composite = surface.dynamic_composite_active &&
            IsRenderTextureValid(surface.shadow_mask_texture);
        if (dynamic_composite) {
            if (!composite_shader_active) {
                BeginShaderMode(composite_shader);
                composite_shader_active = true;
                bound_shadow_mask = invalid_id;
            }
            const Texture2D shadow_mask = surface.shadow_mask_texture.texture;
            // The stable RGB texture remains the primary sampler, retaining
            // its nearest magnification and trilinear/anisotropic minification.
            if (bound_shadow_mask != shadow_mask.id) {
                rlDrawRenderBatchActive();
                rlEnableShader(composite_shader.id);
                rlSetUniformSampler(shadow_mask_location, shadow_mask.id);
                bound_shadow_mask = shadow_mask.id;
            }
        } else if (composite_shader_active) {
            EndShaderMode();
            composite_shader_active = false;
            bound_shadow_mask = invalid_id;
        }
        rlSetTexture(surface.texture.texture.id);
        for (u32 tri_index : surface.triangles) {
            if (tri_index >= geometry->triangle_count) continue;
            const Triangle tri = geometry->triangles[tri_index];
            const u32 indices[3] = {tri.a, tri.b, tri.c};
            rlBegin(RL_TRIANGLES);
            for (u32 vertex_index : indices) {
                const Vector3 local = Vector3Transform(geometry->vertices[vertex_index], basis);
                const float u = (Vector3DotProduct(local, surface.tangent) * ppm - static_cast<float>(surface.grid_min_u)) /
                    static_cast<float>(surface.width);
                const float v = (Vector3DotProduct(local, surface.bitangent) * ppm - static_cast<float>(surface.grid_min_v)) /
                    static_cast<float>(surface.height);
                const Vector3 point = local + origin;
                const float vertex_distance = safe_len(point) * inverse_chunk_size;
                const float lighting_alpha = radiance_lighting_weight(
                    dim, renderer, vertex_distance);
                rlColor4ub(
                    255, 255, 255,
                    static_cast<u8>(std::round(255.0f * lighting_alpha)));
                rlTexCoord2f(u, v);
                rlVertex3f(point.x, point.y, point.z);
            }
            rlEnd();
        }
        rlSetTexture(0);
    }
    if (composite_shader_active) EndShaderMode();
    rlDrawRenderBatchActive();
    glDisable(GL_POLYGON_OFFSET_FILL);
    rlEnableDepthMask();
}

static void draw_painted_pixels(RenderState* renderer, const Dimension* dim, const CameraView& view) {
    if (!renderer || !dim) return;
    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    rlDisableBackfaceCulling();
    for (const MeshPaintSurfaceCache& surface : renderer->mesh_paint_surfaces) {
        const MeshInstance* mesh = arena_get(&dim->meshes, surface.mesh_id);
        const MeshGeometry* geometry = mesh ? arena_get(&dim->geometries, surface.geometry_id) : nullptr;
        if (!mesh || !mesh->visible || !geometry || !IsTextureValid(surface.texture)) continue;
        bool included_in_lighting = false;
        for (const MeshLightingSurfaceCache& lighting : renderer->lighting_surfaces) {
            if (renderer->pathtrace_comparison_enabled) break;
            if (!renderer->lighting.enabled) break;
            if (lighting.mesh_id != surface.mesh_id ||
                lighting.resolved_scene_signature != renderer->radiance_scene_signature ||
                lighting.resolved_paint_revision != surface.paint_hash) continue;
            const float alignment = Vector3DotProduct(lighting.normal, surface.normal);
            const float expected_plane = alignment >= 0.0f ? surface.plane : -surface.plane;
            if (std::fabs(alignment) > 0.999f && std::fabs(lighting.plane - expected_plane) < 0.005f) {
                included_in_lighting = true;
                break;
            }
        }
        if (included_in_lighting) continue;
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
    rlEnableDepthMask();
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
    renderer->debug_visible_chunk_count = 0;
    renderer->debug_visible_mesh_count = 0;
    renderer->debug_chunk_mesh_scan_ms = 0.0;
    renderer->debug_mesh_prepare_ms = 0.0;
    renderer->debug_world_draw_ms = 0.0;
    renderer->debug_paint_update_ms = 0.0;
    renderer->debug_lighting_update_ms = 0.0;
    renderer->debug_lighting_draw_ms = 0.0;
    renderer->debug_post_world_ms = 0.0;
    renderer->debug_pathtrace_scene_update_ms = 0.0;
    renderer->debug_pathtrace_draw_ms = 0.0;

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
    const bool use_shader_depth_edges =
        renderer->depth_test_edges &&
        renderer->shader_depth_edges &&
        renderer->edge_depth_shader_ready &&
        renderer->edge_depth_texture_ready;
    const bool use_compute_depth_edges =
        renderer->depth_test_edges &&
        !use_shader_depth_edges &&
        renderer->gpu_depth_edges &&
        renderer->edge_filter_compute_ready &&
        renderer->edge_scene_depth_texture_ready &&
        renderer->edge_depth_texture_ready;

    const double chunk_mesh_scan_start = GetTime();
    // Reuse the large streamed-world candidate arrays. Reallocating enough
    // storage for a radius-32 hills world every frame was measurable even
    // after frustum culling reduced the submitted mesh set.
    static thread_local std::vector<VisibleChunkRef> visible_chunks{};
    static thread_local std::vector<MeshPrepJob> mesh_jobs{};
    visible_chunks.clear();
    mesh_jobs.clear();
    if (visible_chunks.capacity() < dim->chunks.count) {
        visible_chunks.reserve(dim->chunks.count);
    }
    if (mesh_jobs.capacity() < dim->meshes.count) {
        mesh_jobs.reserve(dim->meshes.count);
    }
    for (u32 chunk_slot = 0; chunk_slot < dim->chunks.count; ++chunk_slot) {
        const Chunk* chunk = &dim->chunks.data[chunk_slot];
        float chunk_dist = 0.0f;
        if (!chunk_visible(dim, view.anchor.chunk, chunk->coord, &chunk_dist)) continue;
        visible_chunks.push_back({chunk, chunk_dist});
        for (u32 i = 0; i < chunk->mesh_count; ++i) {
            const MeshInstance* mesh = arena_get(&dim->meshes, chunk->meshes[i]);
            if (!mesh || !mesh->visible ||
                (renderer->frustum_cull_meshes &&
                 !mesh_intersects_camera_frustum(
                     *mesh, *dim, view, camera,
                     renderer->native_w, renderer->native_h))) continue;
            mesh_jobs.push_back({mesh, chunk_dist});
        }
    }
    renderer->debug_visible_chunk_count = static_cast<u32>(visible_chunks.size());
    renderer->debug_visible_mesh_count = static_cast<u32>(mesh_jobs.size());
    renderer->debug_chunk_mesh_scan_ms = (GetTime() - chunk_mesh_scan_start) * 1000.0;

    const double paint_update_start = GetTime();
    update_mesh_paint_surfaces(renderer, dim, view);
    renderer->debug_paint_update_ms = (GetTime() - paint_update_start) * 1000.0;
    const double lighting_update_start = GetTime();
    bool pathtrace_frame_ready = false;
    if (renderer->pathtrace_comparison_enabled) {
        pathtrace_frame_ready =
            update_pathtrace_comparison_scene(
                renderer, dim, view, local_player_id);
        renderer->debug_pathtrace_scene_update_ms =
            (GetTime() - lighting_update_start) * 1000.0;
        if (!pathtrace_frame_ready) {
            renderer->pathtrace_comparison_enabled = false;
            renderer->pathtrace_comparison_failed = true;
            update_radiance_lighting(
                renderer, dim, view, camera, view_matrix, projection_matrix,
                near_plane);
        }
    } else {
        update_radiance_lighting(
            renderer, dim, view, camera, view_matrix, projection_matrix, near_plane);
    }
    renderer->debug_lighting_update_ms = (GetTime() - lighting_update_start) * 1000.0;

    const bool collect_scene_triangles = renderer->depth_test_edges && !use_shader_depth_edges;
    const bool use_parallel_mesh_prep = mesh_jobs.size() >= parallel_mesh_prep_min_jobs;
    PreparedRenderBatch prepared{};
    if (use_parallel_mesh_prep) {
        const double mesh_prepare_start = GetTime();
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
        renderer->debug_mesh_prepare_ms = (GetTime() - mesh_prepare_start) * 1000.0;
    }

    for (u32 slot = 0; slot < dim->sprites.count; ++slot) {
        const u32 sprite_id = arena_id_at_slot(&dim->sprites, slot);
        const SpriteInstance* sprite = &dim->sprites.data[slot];
        Color ignored_tint{};
        painted_sprite_texture(renderer, dim, sprite_id, sprite, &ignored_tint);
    }

    const double world_draw_start = GetTime();
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
        for (const MeshPrepJob& job : mesh_jobs) {
            draw_mesh_instance_direct(
                renderer, dim, view, camera, view_matrix, projection_matrix, near_plane,
                &edge_list, scene_triangle_target, job.mesh, job.chunk_dist);
        }
    }
    renderer->debug_edge_count = edge_list.count;
    renderer->debug_scene_triangle_count = static_cast<u32>(scene_triangles.triangles.size());
    renderer->debug_unbounded_scene_triangle_count = static_cast<u32>(scene_triangles.unbounded.size());
    renderer->debug_world_draw_ms = (GetTime() - world_draw_start) * 1000.0;

    const double lighting_draw_start = GetTime();
    if (!renderer->pathtrace_comparison_enabled) {
        draw_lighting_surfaces(renderer, dim, view);
    }
    draw_painted_pixels(renderer, dim, view);
    renderer->debug_lighting_draw_ms = (GetTime() - lighting_draw_start) * 1000.0;

    PlayerNameTag hovered_tag{};
    draw_players(renderer, dim, view, local_player_id, camera, view_matrix, projection_matrix, near_plane, &edge_list, &scene_triangles, &hovered_tag);
    renderer->debug_edge_count = edge_list.count;
    renderer->debug_scene_triangle_count = static_cast<u32>(scene_triangles.triangles.size());
    renderer->debug_unbounded_scene_triangle_count = static_cast<u32>(scene_triangles.unbounded.size());

    if (renderer->pathtrace_comparison_enabled && pathtrace_frame_ready) {
        // Finish the exact unlit raster base, shade it in the separate PT
        // target, then replace only this target's colour. Its depth attachment
        // remains intact for sprites and crisp engine edges below.
        EndMode3D();
        EndTextureMode();
        const bool rendered = render_pathtrace_comparison(renderer, dim, view);
        if (!rendered) {
            renderer->pathtrace_comparison_enabled = false;
            renderer->pathtrace_comparison_failed = true;
            renderer_render_dimension_to_target(
                renderer, dim, view, local_player_id);
            return;
        }
        renderer->pathtrace_comparison_failed = false;
        BeginTextureMode(renderer->target);
        DrawTexturePro(
            renderer->pathtrace_comparison_target.texture,
            {0.0f, 0.0f, static_cast<float>(renderer->native_w),
             -static_cast<float>(renderer->native_h)},
            {0.0f, 0.0f, static_cast<float>(renderer->native_w),
             static_cast<float>(renderer->native_h)},
            {}, 0.0f, WHITE);
        BeginMode3D(camera);
        draw_name_tag_billboard(renderer, camera, &hovered_tag);
    }

    SpriteDepthOcclusion sprite_occlusion{};
    const double edge_draw_start = GetTime();
    if (renderer->depth_test_edges) {
        if (use_compute_depth_edges) {
            copy_current_depth_to_texture(renderer, renderer->edge_scene_depth_texture, renderer->edge_scene_depth_texture_ready);
        } else if (!use_shader_depth_edges) {
            sprite_occlusion.scene = read_current_depth_buffer(renderer->native_w, renderer->native_h);
        }
    }

    for (const VisibleChunkRef& visible : visible_chunks) {
        draw_sprites(renderer, dim, view, camera, visible.chunk);
    }

    if (renderer->depth_test_edges) {
        if (use_shader_depth_edges || use_compute_depth_edges) {
            copy_current_depth_to_texture(renderer, renderer->edge_depth_texture, renderer->edge_depth_texture_ready);
        } else {
            sprite_occlusion.after_sprites = read_current_depth_buffer(renderer->native_w, renderer->native_h);
        }
    }

    draw_player_aim_rays(renderer, dim, view, local_player_id, camera);
    draw_physics(renderer, dim, view);

    EndMode3D();

    if (renderer->depth_test_edges) {
        if (use_shader_depth_edges) {
            // Keep edge rejection and drawing on the GPU.  Reading compute
            // results back here serialized the complete lighting pipeline and
            // produced visible frame-time spikes while moving through hills.
            draw_shader_depth_screen_edges(renderer, &edge_list, false);
        } else {
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
        }
    } else {
        draw_flat_screen_edges(&edge_list);
    }
    renderer->debug_post_world_ms = (GetTime() - edge_draw_start) * 1000.0;
    draw_name_tag_text(renderer, hovered_tag);
    draw_crosshair(renderer);

    if (renderer->pathtrace_comparison_enabled) {
        char overlay[128]{};
        char detail[128]{};
        std::snprintf(
            overlay, sizeof(overlay),
            "FULL PT [P]  4spp  5b");
        std::snprintf(
            detail, sizeof(detail), "%u tris%s",
            renderer->pathtrace_triangle_count,
            renderer->pathtrace_scene_truncated ? " | SCENE TRUNCATED" : "");
        const float text_size = 12.0f;
        const Vector2 title_size = measure_engine_text(renderer, overlay, text_size);
        const Vector2 detail_size = measure_engine_text(renderer, detail, text_size);
        DrawRectangle(
            5, 5,
            static_cast<int>(std::ceil(std::max(title_size.x, detail_size.x) + 10.0f)),
            static_cast<int>(std::ceil(title_size.y + detail_size.y + 8.0f)),
            Color{0, 0, 0, 205});
        draw_engine_text(
            renderer, overlay, {10.0f, 8.0f}, text_size,
            renderer->pathtrace_scene_truncated ? ORANGE : Color{255, 224, 112, 255});
        draw_engine_text(
            renderer, detail, {10.0f, 8.0f + title_size.y}, text_size,
            renderer->pathtrace_scene_truncated ? ORANGE : Color{255, 224, 112, 255});
    } else if (renderer->pathtrace_comparison_failed) {
        constexpr const char* error = "PT FAILED [P]";
        const float text_size = 12.0f;
        const Vector2 size = measure_engine_text(renderer, error, text_size);
        DrawRectangle(
            5, 5, static_cast<int>(std::ceil(size.x + 10.0f)),
            static_cast<int>(std::ceil(size.y + 6.0f)), Color{36, 0, 0, 220});
        draw_engine_text(renderer, error, {10.0f, 8.0f}, text_size, ORANGE);
    }

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
