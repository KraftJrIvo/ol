#version 430

const float PI = 3.14159265358979323846;
const float EPS = 0.002;
const float EMISSION_TRANSPORT_SCALE = 24.0;
const int MAX_BVH_STACK = 64;
const int MAX_BOUNCES = 5;
const int SAMPLES_PER_PIXEL = 4;

in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

struct SceneTriangle {
    vec4 a;
    vec4 b;
    vec4 c;
    vec4 albedoReflectivity;
    vec4 emission;
    vec4 uvAB;
    vec4 uvCTexture;
};
struct EmitterTriangle {
    vec4 aCdf;
    vec4 bWeight;
    vec4 cArea;
    vec4 emission;
    vec4 surfaceCenterArea;
    vec4 surfaceNormalMeta;
};
struct BvhNode { vec4 boundsMin; vec4 boundsMax; uvec4 meta; };

layout(std430, binding = 11) readonly buffer SceneTriangles {
    SceneTriangle sceneTriangles[];
};
layout(std430, binding = 12) readonly buffer EmitterTriangles {
    EmitterTriangle emitterTriangles[];
};
layout(std430, binding = 13) readonly buffer BvhNodes {
    BvhNode bvhNodes[];
};
layout(std430, binding = 14) readonly buffer DynamicTriangles {
    SceneTriangle dynamicTriangles[];
};
layout(std430, binding = 15) readonly buffer DynamicBvhNodes {
    BvhNode dynamicBvhNodes[];
};

// texture0 is the exact unlit raster base for the primary hit. This preserves
// painted pixels as well as the engine's centred planar texture mapping.
uniform sampler2D texture0;
uniform sampler2D GRID_TEXTURE;
uniform sampler2D GRASS_TEXTURE;
uniform sampler2D STONE_TEXTURE;
uniform sampler2D ROOF_TEXTURE;
uniform vec2 RESOLUTION;
uniform vec3 CAMERA_POSITION;
uniform vec3 CAMERA_FORWARD;
uniform vec3 CAMERA_RIGHT;
uniform vec3 CAMERA_UP;
uniform float TAN_HALF_FOV;
uniform int TRIANGLE_COUNT;
uniform int EMITTER_COUNT;
uniform int EMITTER_MESH_COUNT;
uniform int BVH_NODE_COUNT;
uniform int DYNAMIC_TRIANGLE_COUNT;
uniform int DYNAMIC_BVH_NODE_COUNT;
uniform float EMITTER_TOTAL_WEIGHT;
uniform vec3 SKY_TOP_RADIANCE;
uniform vec3 SKY_BOTTOM_RADIANCE;
uniform float MAX_TRACE_DISTANCE;

struct Hit {
    bool exists;
    bool frontFacing;
    bool dynamicGeometry;
    bool localPlayer;
    float t;
    vec3 position;
    vec3 normal;
    vec3 albedo;
    float reflectivity;
    vec3 emission;
    vec2 uv;
    int textureId;
};

uint nextRandom(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float randomFloat(inout uint state) {
    return float(nextRandom(state)) * (1.0 / 4294967296.0);
}

vec2 randomVec2(inout uint state) {
    return vec2(randomFloat(state), randomFloat(state));
}

vec3 environmentRadiance(vec3 direction) {
    if (direction.y <= 0.0) return vec3(0.0);
    float height = sqrt(clamp(direction.y, 0.0, 1.0));
    return mix(SKY_BOTTOM_RADIANCE, SKY_TOP_RADIANCE, height);
}

bool intersectBounds(
    vec3 origin, vec3 inverseDirection, vec3 boundsMin, vec3 boundsMax,
    float maximumDistance) {
    vec3 t0 = (boundsMin - origin) * inverseDirection;
    vec3 t1 = (boundsMax - origin) * inverseDirection;
    vec3 nearValues = min(t0, t1);
    vec3 farValues = max(t0, t1);
    float nearT = max(max(nearValues.x, nearValues.y), nearValues.z);
    float farT = min(min(farValues.x, farValues.y), farValues.z);
    return farT >= max(nearT, EPS) && nearT < maximumDistance;
}

bool intersectTriangle(
    vec3 origin, vec3 direction, SceneTriangle tri, float maximumDistance,
    out float t, out vec3 normal, out bool frontFacing, out vec3 barycentric) {
    vec3 edge1 = tri.b.xyz - tri.a.xyz;
    vec3 edge2 = tri.c.xyz - tri.a.xyz;
    vec3 p = cross(direction, edge2);
    float determinant = dot(edge1, p);
    if (abs(determinant) < 0.000001) return false;
    float inverseDeterminant = 1.0 / determinant;
    vec3 s = origin - tri.a.xyz;
    float u = dot(s, p) * inverseDeterminant;
    if (u < -0.00001 || u > 1.00001) return false;
    vec3 q = cross(s, edge1);
    float v = dot(direction, q) * inverseDeterminant;
    if (v < -0.00001 || u + v > 1.00001) return false;
    t = dot(edge2, q) * inverseDeterminant;
    if (t <= EPS || t >= maximumDistance) return false;
    normal = normalize(cross(edge1, edge2));
    frontFacing = dot(normal, direction) < 0.0;
    if (!frontFacing) normal = -normal;
    barycentric = vec3(1.0 - u - v, u, v);
    return true;
}

bool intersectsTriangleAny(
    vec3 origin, vec3 direction, SceneTriangle tri, float maximumDistance) {
    vec3 edge1 = tri.b.xyz - tri.a.xyz;
    vec3 edge2 = tri.c.xyz - tri.a.xyz;
    vec3 p = cross(direction, edge2);
    float determinant = dot(edge1, p);
    if (abs(determinant) < 0.000001) return false;
    float inverseDeterminant = 1.0 / determinant;
    vec3 s = origin - tri.a.xyz;
    float u = dot(s, p) * inverseDeterminant;
    if (u < -0.00001 || u > 1.00001) return false;
    vec3 q = cross(s, edge1);
    float v = dot(direction, q) * inverseDeterminant;
    if (v < -0.00001 || u + v > 1.00001) return false;
    float t = dot(edge2, q) * inverseDeterminant;
    return t > EPS && t < maximumDistance;
}

void storeHit(
    SceneTriangle tri, bool dynamicGeometry, vec3 origin, vec3 direction,
    float candidateT, vec3 candidateNormal, bool candidateFrontFacing,
    vec3 barycentric, inout Hit hit) {
    hit.exists = true;
    hit.dynamicGeometry = dynamicGeometry;
    hit.localPlayer = dynamicGeometry && tri.uvCTexture.w > 0.5;
    hit.t = candidateT;
    hit.position = origin + direction * candidateT;
    hit.normal = candidateNormal;
    hit.frontFacing = candidateFrontFacing;
    hit.albedo = tri.albedoReflectivity.rgb;
    hit.reflectivity = tri.albedoReflectivity.a;
    hit.emission = tri.emission.rgb;
    vec2 uvA = tri.uvAB.xy;
    vec2 uvB = tri.uvAB.zw;
    vec2 uvC = tri.uvCTexture.xy;
    hit.uv = uvA * barycentric.x + uvB * barycentric.y + uvC * barycentric.z;
    hit.textureId = int(round(tri.uvCTexture.z));
}

void traceBvh(
    vec3 origin, vec3 direction, bool dynamicScene, bool primaryRay,
    inout Hit hit) {
    int nodeCount = dynamicScene ? DYNAMIC_BVH_NODE_COUNT : BVH_NODE_COUNT;
    int triangleLimit = dynamicScene ? DYNAMIC_TRIANGLE_COUNT : TRIANGLE_COUNT;
    if (nodeCount <= 0) return;
    vec3 safeDirection = vec3(
        abs(direction.x) < 0.000001 ? (direction.x < 0.0 ? -0.000001 : 0.000001) : direction.x,
        abs(direction.y) < 0.000001 ? (direction.y < 0.0 ? -0.000001 : 0.000001) : direction.y,
        abs(direction.z) < 0.000001 ? (direction.z < 0.0 ? -0.000001 : 0.000001) : direction.z);
    vec3 inverseDirection = 1.0 / safeDirection;
    int stack[MAX_BVH_STACK];
    int stackSize = 1;
    stack[0] = 0;
    int visits = 0;
    while (stackSize > 0 && visits < nodeCount) {
        int nodeIndex = stack[--stackSize];
        ++visits;
        if (nodeIndex < 0 || nodeIndex >= nodeCount) continue;
        BvhNode node = dynamicScene
            ? dynamicBvhNodes[nodeIndex] : bvhNodes[nodeIndex];
        if (!intersectBounds(
                origin, inverseDirection, node.boundsMin.xyz, node.boundsMax.xyz,
                hit.t)) continue;
        int triangleCount = int(node.meta.w);
        if (triangleCount > 0) {
            int first = int(node.meta.z);
            for (int localIndex = 0; localIndex < triangleCount; ++localIndex) {
                int triangleIndex = first + localIndex;
                if (triangleIndex < 0 || triangleIndex >= triangleLimit) continue;
                SceneTriangle tri = dynamicScene
                    ? dynamicTriangles[triangleIndex] : sceneTriangles[triangleIndex];
                float candidateT;
                vec3 candidateNormal;
                bool candidateFrontFacing;
                vec3 barycentric;
                if (!intersectTriangle(
                        origin, direction, tri, hit.t, candidateT, candidateNormal,
                        candidateFrontFacing, barycentric)) continue;
                // The local player's camera starts inside its ordinary cylinder.
                // Skip that tagged mesh for primary visibility at every distance;
                // secondary/reflection paths still see it as ordinary geometry.
                if (primaryRay && dynamicScene && tri.uvCTexture.w > 0.5) continue;
                storeHit(
                    tri, dynamicScene, origin, direction, candidateT,
                    candidateNormal, candidateFrontFacing, barycentric, hit);
            }
        } else if (stackSize <= MAX_BVH_STACK - 2) {
            stack[stackSize++] = int(node.meta.x);
            stack[stackSize++] = int(node.meta.y);
        }
    }
}

void traceEmitters(vec3 origin, vec3 direction, inout Hit hit) {
    for (int emitterIndex = 0; emitterIndex < EMITTER_COUNT; ++emitterIndex) {
        EmitterTriangle emitter = emitterTriangles[emitterIndex];
        SceneTriangle tri;
        tri.a = vec4(emitter.aCdf.xyz, 0.0);
        tri.b = vec4(emitter.bWeight.xyz, 0.0);
        tri.c = vec4(emitter.cArea.xyz, 0.0);
        tri.albedoReflectivity = vec4(1.0, 1.0, 1.0, 0.0);
        tri.emission = vec4(emitter.emission.rgb, 0.0);
        tri.uvAB = vec4(0.0);
        tri.uvCTexture = vec4(0.0);
        float candidateT;
        vec3 candidateNormal;
        bool candidateFrontFacing;
        vec3 barycentric;
        if (!intersectTriangle(
                origin, direction, tri, hit.t, candidateT, candidateNormal,
                candidateFrontFacing, barycentric)) continue;
        storeHit(
            tri, false, origin, direction, candidateT, candidateNormal,
            candidateFrontFacing, barycentric, hit);
    }
}

Hit traceScene(vec3 origin, vec3 direction, bool primaryRay) {
    Hit hit;
    hit.exists = false;
    hit.frontFacing = true;
    hit.dynamicGeometry = false;
    hit.localPlayer = false;
    hit.t = MAX_TRACE_DISTANCE;
    hit.position = vec3(0.0);
    hit.normal = vec3(0.0, 1.0, 0.0);
    hit.albedo = vec3(0.0);
    hit.reflectivity = 0.0;
    hit.emission = vec3(0.0);
    hit.uv = vec2(0.0);
    hit.textureId = 0;
    traceBvh(origin, direction, false, primaryRay, hit);
    traceBvh(origin, direction, true, primaryRay, hit);
    traceEmitters(origin, direction, hit);
    return hit;
}

bool bvhOccluded(
    vec3 origin, vec3 direction, float maximumDistance, bool dynamicScene) {
    int nodeCount = dynamicScene ? DYNAMIC_BVH_NODE_COUNT : BVH_NODE_COUNT;
    int triangleLimit = dynamicScene ? DYNAMIC_TRIANGLE_COUNT : TRIANGLE_COUNT;
    if (nodeCount <= 0) return false;
    vec3 safeDirection = vec3(
        abs(direction.x) < 0.000001 ? (direction.x < 0.0 ? -0.000001 : 0.000001) : direction.x,
        abs(direction.y) < 0.000001 ? (direction.y < 0.0 ? -0.000001 : 0.000001) : direction.y,
        abs(direction.z) < 0.000001 ? (direction.z < 0.0 ? -0.000001 : 0.000001) : direction.z);
    vec3 inverseDirection = 1.0 / safeDirection;
    int stack[MAX_BVH_STACK];
    int stackSize = 1;
    stack[0] = 0;
    int visits = 0;
    while (stackSize > 0 && visits < nodeCount) {
        int nodeIndex = stack[--stackSize];
        ++visits;
        if (nodeIndex < 0 || nodeIndex >= nodeCount) continue;
        BvhNode node = dynamicScene
            ? dynamicBvhNodes[nodeIndex] : bvhNodes[nodeIndex];
        if (!intersectBounds(
                origin, inverseDirection, node.boundsMin.xyz, node.boundsMax.xyz,
                maximumDistance)) continue;
        int triangleCount = int(node.meta.w);
        if (triangleCount > 0) {
            int first = int(node.meta.z);
            for (int localIndex = 0; localIndex < triangleCount; ++localIndex) {
                int triangleIndex = first + localIndex;
                if (triangleIndex < 0 || triangleIndex >= triangleLimit) continue;
                SceneTriangle tri = dynamicScene
                    ? dynamicTriangles[triangleIndex] : sceneTriangles[triangleIndex];
                if (intersectsTriangleAny(
                        origin, direction, tri, maximumDistance)) return true;
            }
        } else if (stackSize <= MAX_BVH_STACK - 2) {
            stack[stackSize++] = int(node.meta.x);
            stack[stackSize++] = int(node.meta.y);
        }
    }
    return false;
}

bool sceneOccluded(vec3 origin, vec3 direction, float maximumDistance) {
    // Emitter meshes intentionally never enter either occluding BVH.
    return bvhOccluded(origin, direction, maximumDistance, false) ||
           bvhOccluded(origin, direction, maximumDistance, true);
}

void buildBasis(vec3 normal, out vec3 tangent, out vec3 bitangent) {
    vec3 up = abs(normal.z) < 0.999
        ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

vec3 cosineHemisphere(vec2 u) {
    float radius = sqrt(u.x);
    float angle = 2.0 * PI * u.y;
    return vec3(
        radius * cos(angle), radius * sin(angle),
        sqrt(max(0.0, 1.0 - u.x)));
}

vec3 materialAlbedo(Hit hit) {
    vec3 texel = vec3(1.0);
    // Secondary rays have no stable screen derivatives. Explicit LOD keeps
    // divergent path sampling defined and deterministic on every GPU.
    if (hit.textureId == 3) texel = textureLod(GRID_TEXTURE, hit.uv, 0.0).rgb;
    else if (hit.textureId == 4) texel = textureLod(GRASS_TEXTURE, hit.uv, 0.0).rgb;
    else if (hit.textureId == 5) texel = textureLod(STONE_TEXTURE, hit.uv, 0.0).rgb;
    else if (hit.textureId == 6) texel = textureLod(ROOF_TEXTURE, hit.uv, 0.0).rgb;
    return hit.albedo * pow(max(texel, vec3(0.0)), vec3(2.2));
}

vec3 primaryBaseAlbedo() {
    ivec2 size = textureSize(texture0, 0);
    ivec2 pixel = clamp(
        ivec2(floor(fragTexCoord * vec2(size))), ivec2(0), size - ivec2(1));
    return pow(max(texelFetch(texture0, pixel, 0).rgb, vec3(0.0)), vec3(2.2));
}

vec3 sampleEmitterTriangle(
    Hit surface, int selected, float inverseAreaPdf, inout uint randomState) {
    EmitterTriangle emitter = emitterTriangles[selected];
    vec2 xi = randomVec2(randomState);
    float root = sqrt(xi.x);
    vec3 lightPoint = emitter.aCdf.xyz * (1.0 - root) +
        emitter.bWeight.xyz * (root * (1.0 - xi.y)) +
        emitter.cArea.xyz * (root * xi.y);
    vec3 delta = lightPoint - surface.position;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared <= EPS * EPS) return vec3(0.0);
    float distance = sqrt(distanceSquared);
    vec3 direction = delta / distance;
    float surfaceCosine = max(dot(surface.normal, direction), 0.0);
    vec3 lightNormal = normalize(cross(
        emitter.bWeight.xyz - emitter.aCdf.xyz,
        emitter.cArea.xyz - emitter.aCdf.xyz));
    float lightCosine = max(dot(lightNormal, -direction), 0.0);
    if (surfaceCosine <= 0.0 || lightCosine <= 0.0) return vec3(0.0);
    vec3 shadowOrigin = surface.position + surface.normal * (4.0 * EPS);
    vec3 shadowDelta = lightPoint - shadowOrigin;
    float shadowDistance = length(shadowDelta);
    if (sceneOccluded(
            shadowOrigin, shadowDelta / max(shadowDistance, EPS),
            max(shadowDistance - 4.0 * EPS, EPS))) return vec3(0.0);
    return emitter.emission.rgb * EMISSION_TRANSPORT_SCALE *
        (surfaceCosine * lightCosine / distanceSquared) * inverseAreaPdf / PI;
}

float emitterSurfaceImportance(int emitterIndex, Hit surface) {
    EmitterTriangle emitter = emitterTriangles[emitterIndex];
    if (emitter.surfaceNormalMeta.w < 0.5) return 0.0;
    vec3 delta = emitter.surfaceCenterArea.xyz - surface.position;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared <= EPS * EPS) return 0.0;
    vec3 direction = delta * inversesqrt(distanceSquared);
    float surfaceCosine = max(dot(surface.normal, direction), 0.0);
    float lightCosine = max(
        dot(emitter.surfaceNormalMeta.xyz, -direction), 0.0);
    float luminance = max(
        dot(emitter.emission.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0001);
    return emitter.surfaceCenterArea.w * luminance * surfaceCosine *
        lightCosine / max(distanceSquared, EPS * EPS);
}

vec3 sampleDirectEmitters(Hit surface, inout uint randomState) {
    if (EMITTER_COUNT <= 0 || EMITTER_MESH_COUNT <= 0) return vec3(0.0);
    vec3 result = vec3(0.0);
    // One stratified area sample is taken from every emissive mesh. Lights
    // therefore cannot disappear merely because a random global selector chose
    // another source, and all authored emitters contribute to this frame.
    for (int meshIndex = 0; meshIndex < EMITTER_MESH_COUNT; ++meshIndex) {
        int groupFirst = -1;
        int groupEnd = -1;
        for (int emitterIndex = 0; emitterIndex < EMITTER_COUNT; ++emitterIndex) {
            EmitterTriangle candidate = emitterTriangles[emitterIndex];
            if (int(round(candidate.emission.w)) == meshIndex) {
                if (groupFirst < 0) groupFirst = emitterIndex;
                groupEnd = emitterIndex + 1;
            } else if (groupFirst >= 0) {
                break;
            }
        }
        if (groupFirst < 0) continue;

        float importanceTotal = 0.0;
        for (int emitterIndex = groupFirst; emitterIndex < groupEnd;
             ++emitterIndex) {
            importanceTotal += emitterSurfaceImportance(emitterIndex, surface);
        }
        if (importanceTotal <= 0.0000001) continue;
        float target = randomFloat(randomState) * importanceTotal;
        float cumulative = 0.0;
        int selectedSurface = -1;
        float selectedImportance = 0.0;
        for (int emitterIndex = groupFirst; emitterIndex < groupEnd;
             ++emitterIndex) {
            float importance = emitterSurfaceImportance(emitterIndex, surface);
            cumulative += importance;
            if (selectedSurface < 0 && cumulative >= target) {
                selectedSurface = emitterIndex;
                selectedImportance = importance;
            }
        }
        if (selectedSurface < 0 || selectedImportance <= 0.0000001) continue;

        EmitterTriangle surfaceRecord = emitterTriangles[selectedSurface];
        int surfaceTriangleCount = max(
            1, int(round(surfaceRecord.surfaceNormalMeta.w)));
        float surfaceArea = max(surfaceRecord.surfaceCenterArea.w, 0.000001);
        float triangleTarget = randomFloat(randomState) * surfaceArea;
        float triangleAccumulated = 0.0;
        int selectedTriangle = selectedSurface;
        for (int localIndex = 0; localIndex < surfaceTriangleCount; ++localIndex) {
            int triangleIndex = selectedSurface + localIndex;
            if (triangleIndex >= groupEnd) break;
            triangleAccumulated += max(
                emitterTriangles[triangleIndex].cArea.w, 0.0);
            if (triangleTarget <= triangleAccumulated) {
                selectedTriangle = triangleIndex;
                break;
            }
        }
        // p(surface) * p(triangle|surface) * p(point|triangle)
        // simplifies to importance/(total*surfaceArea).
        float inverseAreaPdf =
            importanceTotal * surfaceArea / selectedImportance;
        result += sampleEmitterTriangle(
            surface, selectedTriangle, inverseAreaPdf, randomState);
    }
    return result;
}

vec3 sampleDirectSky(Hit surface, inout uint randomState) {
    vec3 tangent;
    vec3 bitangent;
    buildBasis(surface.normal, tangent, bitangent);
    vec3 local = cosineHemisphere(randomVec2(randomState));
    vec3 direction = normalize(
        tangent * local.x + bitangent * local.y + surface.normal * local.z);
    vec3 sky = environmentRadiance(direction);
    if (max(sky.r, max(sky.g, sky.b)) <= 0.0) return vec3(0.0);
    if (sceneOccluded(
            surface.position + surface.normal * (4.0 * EPS), direction,
            MAX_TRACE_DISTANCE)) return vec3(0.0);
    // Cosine-weighted hemisphere sampling cancels diffuse cos(theta)/pi.
    return sky;
}

vec3 tracePath(vec3 origin, vec3 direction, uint initialState) {
    uint randomState = initialState;
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    bool previousWasSpecular = true;
    bool primaryRay = true;
    for (int bounce = 0; bounce < MAX_BOUNCES; ++bounce) {
        Hit hit = traceScene(origin, direction, primaryRay);
        if (!hit.exists) {
            if (previousWasSpecular) radiance += throughput * environmentRadiance(direction);
            break;
        }
        if (length(hit.emission) > 0.0001) {
            if (previousWasSpecular) {
                radiance += throughput * hit.emission * EMISSION_TRANSPORT_SCALE;
            }
            break;
        }

        vec3 albedo = primaryRay
            ? primaryBaseAlbedo()
            : materialAlbedo(hit);
        float reflectivity = clamp(hit.reflectivity, 0.0, 1.0);
        if (reflectivity > 0.0001 && randomFloat(randomState) < reflectivity) {
            direction = normalize(reflect(direction, hit.normal));
            origin = hit.position + hit.normal * (4.0 * EPS);
            previousWasSpecular = true;
        } else {
            vec3 direct = sampleDirectEmitters(hit, randomState) +
                sampleDirectSky(hit, randomState);
            radiance += throughput * albedo * direct;
            throughput *= albedo;
            vec3 tangent;
            vec3 bitangent;
            buildBasis(hit.normal, tangent, bitangent);
            vec3 local = cosineHemisphere(randomVec2(randomState));
            direction = normalize(
                tangent * local.x + bitangent * local.y + hit.normal * local.z);
            origin = hit.position + hit.normal * (4.0 * EPS);
            previousWasSpecular = false;
        }
        primaryRay = false;

        if (bounce >= 3) {
            float survival = clamp(
                max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
            if (randomFloat(randomState) > survival) break;
            throughput /= survival;
        }
    }
    return radiance;
}

vec3 cameraDirection(vec2 pixelPosition) {
    vec2 ndc = (pixelPosition / RESOLUTION) * 2.0 - 1.0;
    float aspect = RESOLUTION.x / max(RESOLUTION.y, 1.0);
    return normalize(
        CAMERA_FORWARD + CAMERA_RIGHT * (ndc.x * aspect * TAN_HALF_FOV) +
        CAMERA_UP * (ndc.y * TAN_HALF_FOV));
}

vec3 acesFilm(vec3 value) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((value * (a * value + b)) / (value * (c * value + d) + e), 0.0, 1.0);
}

void main() {
    // Primary rays always pass through pixel centres. Path directions use a
    // deterministic per-pixel sequence, so a stationary view is bit-stable and
    // never depends on temporal accumulation.
    vec3 direction = cameraDirection(gl_FragCoord.xy);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 sum = vec3(0.0);
    for (int sampleIndex = 0; sampleIndex < SAMPLES_PER_PIXEL; ++sampleIndex) {
        uint state = uint(pixel.x + pixel.y * int(RESOLUTION.x));
        state ^= uint(sampleIndex + 1) * 277803737u;
        state ^= 0x9e3779b9u;
        sum += tracePath(CAMERA_POSITION, direction, state);
    }
    vec3 linearColor = sum / float(SAMPLES_PER_PIXEL);
    vec3 display = pow(acesFilm(max(linearColor, vec3(0.0))), vec3(1.0 / 2.2));
    finalColor = vec4(display, 1.0);
}
