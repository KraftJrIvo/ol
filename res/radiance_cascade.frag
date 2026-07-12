#version 430

const float PI = 3.14159265358979323846;
const float EPS = 0.002;
const float EMISSION_TRANSPORT_SCALE = 24.0;
const int MAX_LEVELS = 5;
const int MAX_INDIRECT_SAMPLES = 64;
const int MAX_SHADOW_SAMPLES = 16;
const int MAX_EMITTER_TRIANGLES = 256;
const int MAX_BVH_STACK = 64;

in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

struct SceneTriangle {
    vec4 a;
    vec4 b;
    vec4 c;
    vec4 albedoReflectivity;
    vec4 emission;
};

struct EmitterTriangle {
    vec4 aCdf;
    vec4 bWeight;
    vec4 cArea;
    vec4 emission;
    vec4 surfaceCenterArea;
    vec4 surfaceNormalMeta;
};

struct BvhNode {
    vec4 boundsMin;
    vec4 boundsMax;
    uvec4 meta; // left child, right child, first triangle, triangle count
};

layout(std430, binding = 11) readonly buffer SceneTriangles { SceneTriangle sceneTriangles[]; };
layout(std430, binding = 12) readonly buffer EmitterTriangles { EmitterTriangle emitterTriangles[]; };
layout(std430, binding = 13) readonly buffer BvhNodes { BvhNode bvhNodes[]; };
layout(std430, binding = 14) readonly buffer DynamicTriangles { SceneTriangle dynamicTriangles[]; };
layout(std430, binding = 15) readonly buffer DynamicBvhNodes { BvhNode dynamicBvhNodes[]; };

uniform sampler2D texture0;
uniform int TRIANGLE_COUNT;
uniform int EMITTER_COUNT;
uniform int EMITTER_MESH_COUNT;
uniform int BVH_NODE_COUNT;
uniform int DYNAMIC_TRIANGLE_COUNT;
uniform int DYNAMIC_BVH_NODE_COUNT;
uniform float EMITTER_TOTAL_WEIGHT;
uniform int LEVEL_COUNT;
uniform int PROBE_COUNT_0;
uniform int ANGULAR_RES_0;
uniform int ITERATION;
uniform int INDIRECT_SAMPLE_COUNT;
uniform int SHADOW_SAMPLE_COUNT;
uniform int FRAME_INDEX;
uniform int JITTER_ENABLED;
uniform vec3 VOLUME_MIN;
uniform vec3 VOLUME_SIZE;
uniform vec3 LEVEL_VOLUME_MIN[MAX_LEVELS];
uniform vec3 SKY_TOP_RADIANCE;
uniform vec3 SKY_BOTTOM_RADIANCE;
uniform float INTERVAL_SCALE;
uniform float MAX_TRACE_DISTANCE;

struct Hit {
    bool exists;
    bool frontFacing;
    float t;
    vec3 position;
    vec3 normal;
    vec3 albedo;
    float reflectivity;
    vec3 emission;
};

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 environmentRadiance(vec3 direction) {
    if (direction.y <= 0.0) return vec3(0.0);
    float height = sqrt(clamp(direction.y, 0.0, 1.0));
    return mix(SKY_BOTTOM_RADIANCE, SKY_TOP_RADIANCE, height);
}

bool intersectBounds(vec3 origin, vec3 direction, vec3 boundsMin, vec3 boundsMax, float maximumDistance) {
    vec3 safeDirection = vec3(
        abs(direction.x) < 0.000001 ? (direction.x < 0.0 ? -0.000001 : 0.000001) : direction.x,
        abs(direction.y) < 0.000001 ? (direction.y < 0.0 ? -0.000001 : 0.000001) : direction.y,
        abs(direction.z) < 0.000001 ? (direction.z < 0.0 ? -0.000001 : 0.000001) : direction.z);
    vec3 inverse = 1.0 / safeDirection;
    vec3 t0 = (boundsMin - origin) * inverse;
    vec3 t1 = (boundsMax - origin) * inverse;
    vec3 nearValues = min(t0, t1);
    vec3 farValues = max(t0, t1);
    float nearT = max(max(nearValues.x, nearValues.y), nearValues.z);
    float farT = min(min(farValues.x, farValues.y), farValues.z);
    return farT >= max(nearT, EPS) && nearT < maximumDistance;
}

bool intersectBoundsInv(
    vec3 origin, vec3 inverseDirection, vec3 boundsMin, vec3 boundsMax,
    float maximumDistance) {
    vec3 t0=(boundsMin-origin)*inverseDirection;
    vec3 t1=(boundsMax-origin)*inverseDirection;
    vec3 nearValues=min(t0,t1);
    vec3 farValues=max(t0,t1);
    float nearT=max(max(nearValues.x,nearValues.y),nearValues.z);
    float farT=min(min(farValues.x,farValues.y),farValues.z);
    return farT>=max(nearT,EPS) && nearT<maximumDistance;
}

bool intersectTriangle(vec3 origin, vec3 direction, SceneTriangle tri, float maximumDistance,
                       out float t, out vec3 normal, out bool frontFacing) {
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
    frontFacing = dot(normal,direction) < 0.0;
    if (!frontFacing) normal=-normal;
    return true;
}

bool intersectsTriangleAny(
    vec3 origin, vec3 direction, SceneTriangle tri, float maximumDistance) {
    vec3 edge1=tri.b.xyz-tri.a.xyz;
    vec3 edge2=tri.c.xyz-tri.a.xyz;
    vec3 p=cross(direction,edge2);
    float determinant=dot(edge1,p);
    if (abs(determinant)<0.000001) return false;
    float inverseDeterminant=1.0/determinant;
    vec3 s=origin-tri.a.xyz;
    float u=dot(s,p)*inverseDeterminant;
    if (u<-0.00001 || u>1.00001) return false;
    vec3 q=cross(s,edge1);
    float v=dot(direction,q)*inverseDeterminant;
    if (v<-0.00001 || u+v>1.00001) return false;
    float t=dot(edge2,q)*inverseDeterminant;
    return t>EPS && t<maximumDistance;
}

void traceBvh(
    vec3 origin, vec3 direction, bool dynamicScene, bool skipEmissive,
    inout Hit hit) {
    int nodeCount = dynamicScene ? DYNAMIC_BVH_NODE_COUNT : BVH_NODE_COUNT;
    int triangleLimit = dynamicScene ? DYNAMIC_TRIANGLE_COUNT : TRIANGLE_COUNT;
    if (nodeCount > 0) {
        int stack[MAX_BVH_STACK];
        int stackSize = 1;
        stack[0] = 0;
        int visits = 0;
        while (stackSize > 0 && visits < nodeCount) {
            int nodeIndex = stack[--stackSize];
            ++visits;
            if (nodeIndex < 0 || nodeIndex >= nodeCount) continue;
            BvhNode node;
            if (dynamicScene) node = dynamicBvhNodes[nodeIndex];
            else node = bvhNodes[nodeIndex];
            if (!intersectBounds(origin, direction, node.boundsMin.xyz, node.boundsMax.xyz, hit.t)) continue;
            int triangleCount = int(node.meta.w);
            if (triangleCount > 0) {
                int first = int(node.meta.z);
                for (int localIndex = 0; localIndex < triangleCount; ++localIndex) {
                    int triangleIndex = first + localIndex;
                    if (triangleIndex < 0 || triangleIndex >= triangleLimit) continue;
                    float candidateT;
                    vec3 candidateNormal;
                    bool candidateFrontFacing;
                    SceneTriangle tri;
                    if (dynamicScene) tri = dynamicTriangles[triangleIndex];
                    else tri = sceneTriangles[triangleIndex];
                    if (skipEmissive && length(tri.emission.rgb)>0.0001) continue;
                    if (!intersectTriangle(
                            origin,direction,tri,hit.t,candidateT,candidateNormal,candidateFrontFacing)) continue;
                    hit.exists = true;
                    hit.t = candidateT;
                    hit.position = origin + direction*candidateT;
                    hit.normal = candidateNormal;
                    hit.frontFacing = candidateFrontFacing;
                    hit.albedo = tri.albedoReflectivity.rgb;
                    hit.reflectivity = tri.albedoReflectivity.a;
                    hit.emission = tri.emission.rgb;
                }
            } else if (stackSize <= MAX_BVH_STACK - 2) {
                stack[stackSize++] = int(node.meta.x);
                stack[stackSize++] = int(node.meta.y);
            }
        }
    }
}

bool bvhOccluded(
    vec3 origin, vec3 direction, float maximumDistance, bool dynamicScene) {
    int nodeCount=dynamicScene ? DYNAMIC_BVH_NODE_COUNT : BVH_NODE_COUNT;
    int triangleLimit=dynamicScene ? DYNAMIC_TRIANGLE_COUNT : TRIANGLE_COUNT;
    if (nodeCount<=0) return false;
    vec3 safeDirection=vec3(
        abs(direction.x)<0.000001 ? (direction.x<0.0 ? -0.000001 : 0.000001) : direction.x,
        abs(direction.y)<0.000001 ? (direction.y<0.0 ? -0.000001 : 0.000001) : direction.y,
        abs(direction.z)<0.000001 ? (direction.z<0.0 ? -0.000001 : 0.000001) : direction.z);
    vec3 inverseDirection=1.0/safeDirection;
    int stack[MAX_BVH_STACK];
    int stackSize=1;
    stack[0]=0;
    int visits=0;
    while (stackSize>0 && visits<nodeCount) {
        int nodeIndex=stack[--stackSize];
        ++visits;
        if (nodeIndex<0 || nodeIndex>=nodeCount) continue;
        BvhNode node;
        if (dynamicScene) node=dynamicBvhNodes[nodeIndex];
        else node=bvhNodes[nodeIndex];
        if (!intersectBoundsInv(
                origin,inverseDirection,node.boundsMin.xyz,node.boundsMax.xyz,
                maximumDistance)) continue;
        int triangleCount=int(node.meta.w);
        if (triangleCount>0) {
            int first=int(node.meta.z);
            for (int localIndex=0;localIndex<triangleCount;++localIndex) {
                int triangleIndex=first+localIndex;
                if (triangleIndex<0 || triangleIndex>=triangleLimit) continue;
                SceneTriangle tri;
                if (dynamicScene) tri=dynamicTriangles[triangleIndex];
                else tri=sceneTriangles[triangleIndex];
                if (intersectsTriangleAny(
                        origin,direction,tri,maximumDistance)) return true;
            }
        } else if (stackSize<=MAX_BVH_STACK-2) {
            stack[stackSize++]=int(node.meta.x);
            stack[stackSize++]=int(node.meta.y);
        }
    }
    return false;
}

bool sceneOccluded(vec3 origin, vec3 direction, float maximumDistance) {
    return bvhOccluded(origin,direction,maximumDistance,false) ||
           bvhOccluded(origin,direction,maximumDistance,true);
}

void traceEmitterTriangles(vec3 origin, vec3 direction, inout Hit hit) {
    for (int emitterIndex=0;emitterIndex<MAX_EMITTER_TRIANGLES;++emitterIndex) {
        if (emitterIndex>=EMITTER_COUNT) break;
        EmitterTriangle emitter=emitterTriangles[emitterIndex];
        SceneTriangle tri;
        tri.a=vec4(emitter.aCdf.xyz,0.0);
        tri.b=vec4(emitter.bWeight.xyz,0.0);
        tri.c=vec4(emitter.cArea.xyz,0.0);
        tri.albedoReflectivity=vec4(1.0,1.0,1.0,0.0);
        tri.emission=vec4(emitter.emission.rgb,0.0);
        float candidateT;
        vec3 candidateNormal;
        bool candidateFrontFacing;
        if (!intersectTriangle(
                origin,direction,tri,hit.t,
                candidateT,candidateNormal,candidateFrontFacing)) continue;
        hit.exists=true;
        hit.t=candidateT;
        hit.position=origin+direction*candidateT;
        hit.normal=candidateNormal;
        hit.frontFacing=candidateFrontFacing;
        hit.albedo=vec3(1.0);
        hit.reflectivity=0.0;
        hit.emission=emitter.emission.rgb;
    }
}

Hit traceSceneMode(
    vec3 origin, vec3 direction, float maximumDistance, bool skipEmissive) {
    Hit hit;
    hit.exists = false;
    hit.frontFacing = true;
    hit.t = maximumDistance;
    hit.position = vec3(0.0);
    hit.normal = vec3(0.0, 1.0, 0.0);
    hit.albedo = vec3(0.0);
    hit.reflectivity = 0.0;
    hit.emission = vec3(0.0);
    traceBvh(origin, direction, false, skipEmissive, hit);
    traceBvh(origin, direction, true, skipEmissive, hit);
    if (!skipEmissive) traceEmitterTriangles(origin,direction,hit);
    return hit;
}

Hit traceScene(vec3 origin, vec3 direction, float maximumDistance) {
    return traceSceneMode(origin,direction,maximumDistance,false);
}

Hit traceOccluders(vec3 origin, vec3 direction, float maximumDistance) {
    return traceSceneMode(origin,direction,maximumDistance,true);
}

vec2 directionToDisk(vec3 direction) {
    if (direction.y > 0.999999) return vec2(0.0, 1.0);
    return 0.5 * sqrt(2.0 / max(0.000001, 1.0-direction.y)) * direction.xz;
}

vec3 diskToDirection(vec2 disk) {
    float radius = length(disk);
    if (radius < 0.000001) return vec3(0.0, -1.0, 0.0);
    float angle = 2.0 * asin(clamp(radius, 0.0, 1.0));
    float sine = sin(angle);
    return vec3(disk.x/radius*sine, -cos(angle), disk.y/radius*sine);
}

vec4 sampleProbe(int level, vec3 probe, vec3 direction) {
    int levelSide = ANGULAR_RES_0 * PROBE_COUNT_0;
    int angularSize = ANGULAR_RES_0 << level;
    vec2 origin = vec2(level*levelSide + int(probe.x)*angularSize,
                       int(probe.z)*levelSide + int(probe.y)*angularSize);
    vec2 disk = directionToDisk(direction);
    float safeRadius = max(0.0, 1.0 - 1.5/float(angularSize));
    if (length(disk) > safeRadius) disk *= safeRadius/max(length(disk),0.000001);
    vec2 pixel = origin + clamp((disk+1.0)*0.5*float(angularSize),
                                vec2(0.5), vec2(float(angularSize)-0.5));
    return texture(texture0, pixel/vec2(textureSize(texture0,0)));
}

vec4 interpolateLevel(int level, vec3 position, vec3 normal, vec3 direction) {
    int probeCount = max(1, PROBE_COUNT_0 >> level);
    vec3 spacing = VOLUME_SIZE/float(probeCount);
    vec3 levelMin = LEVEL_VOLUME_MIN[level];
    vec3 coordinate = (position-levelMin)/spacing - 0.5;
    vec3 base = floor(coordinate);
    vec3 fraction = fract(coordinate);
    vec4 value = vec4(0.0);
    float weightSum = 0.0;
    for (int z=0; z<2; ++z) for (int y=0; y<2; ++y) for (int x=0; x<2; ++x) {
        vec3 corner = vec3(x,y,z);
        vec3 probe = clamp(base+corner, vec3(0.0), vec3(float(probeCount-1)));
        vec3 weights = mix(vec3(1.0)-fraction, fraction, corner);
        float weight = weights.x*weights.y*weights.z;
        vec3 probePosition = levelMin + (probe+0.5)*spacing;
        weight *= smoothstep(-0.15*length(spacing),0.35*length(spacing),
                             dot(probePosition-position,normal));
        value += sampleProbe(level,probe,direction)*weight;
        weightSum += weight;
    }
    return weightSum > 0.00001 ? value/weightSum : vec4(0.0);
}

float cascadeCoverage(vec3 position) {
    vec3 edgeDistance = min(position-VOLUME_MIN, VOLUME_MIN+VOLUME_SIZE-position);
    float nearestEdge = min(edgeDistance.x,min(edgeDistance.y,edgeDistance.z));
    if (nearestEdge <= 0.0) return 0.0;
    float fadeWidth = max(0.001,length(VOLUME_SIZE/float(PROBE_COUNT_0)));
    return smoothstep(0.0,fadeWidth,nearestEdge);
}

vec3 sampleCascade(vec3 position, vec3 normal, vec3 direction) {
    float coverage = cascadeCoverage(position);
    if (coverage <= 0.0) return vec3(0.0);
    vec3 radiance = vec3(0.0);
    float opacity = 0.0;
    for (int level=0; level<MAX_LEVELS; ++level) {
        if (level >= LEVEL_COUNT) break;
        vec4 segment = interpolateLevel(level,position,normal,direction);
        radiance += (1.0-opacity)*segment.rgb;
        opacity += (1.0-opacity)*segment.a;
        if (opacity > 0.999) break;
    }
    return radiance*coverage;
}

void buildBasis(vec3 normal, out vec3 tangent, out vec3 bitangent) {
    vec3 up = abs(normal.z) < 0.999 ? vec3(0,0,1) : vec3(0,1,0);
    tangent = normalize(cross(up,normal));
    bitangent = cross(normal,tangent);
}

vec3 cosineHemisphere(vec2 u) {
    float radius = sqrt(u.x);
    float angle = 2.0*PI*u.y;
    return vec3(radius*cos(angle),radius*sin(angle),sqrt(max(0.0,1.0-u.x)));
}

int finiteEmitterSampleCount() {
    int representedGroups=min(EMITTER_MESH_COUNT,MAX_SHADOW_SAMPLES);
    return clamp(max(SHADOW_SAMPLE_COUNT,representedGroups),1,MAX_SHADOW_SAMPLES);
}

float emitterSurfaceImportance(int emitterIndex, vec3 position, vec3 normal) {
    EmitterTriangle emitter=emitterTriangles[emitterIndex];
    if (emitter.surfaceNormalMeta.w<0.5) return 0.0;
    vec3 delta=emitter.surfaceCenterArea.xyz-position;
    float distanceSquared=dot(delta,delta);
    if (distanceSquared<=EPS*EPS) return 0.0;
    vec3 direction=delta*inversesqrt(distanceSquared);
    float surfaceCosine=max(dot(normal,direction),0.0);
    float lightCosine=max(dot(emitter.surfaceNormalMeta.xyz,-direction),0.0);
    float luminance=max(dot(emitter.emission.rgb,vec3(0.2126,0.7152,0.0722)),0.0001);
    return emitter.surfaceCenterArea.w*luminance*surfaceCosine*lightCosine/
           max(distanceSquared,EPS*EPS);
}

int selectFiniteEmitter(
    vec3 position, vec3 normal, int sampleIndex, int sampleCount,
    out float sampleNormalization, out vec2 triangleXi) {
    const float golden=0.61803398875;
    int groupCount=clamp(EMITTER_MESH_COUNT,1,EMITTER_COUNT);
    int groupIndex=0;
    int localSampleIndex=sampleIndex;
    int groupSampleCount=1;
    float receiverRotation=0.0;

    if (groupCount<=MAX_SHADOW_SAMPLES) {
        // Enumerate every emissive mesh before spending additional shadow
        // samples.  This is a stratified sum over emitters, not a lottery in
        // which a small or differently coloured source can disappear.
        groupIndex=sampleIndex%groupCount;
        localSampleIndex=sampleIndex/groupCount;
        groupSampleCount=(sampleCount+groupCount-1-groupIndex)/groupCount;
        sampleNormalization=1.0/float(groupSampleCount);
        if (JITTER_ENABLED!=0) {
            receiverRotation=hash13(
                position+float(FRAME_INDEX+groupIndex)*vec3(0.17,0.31,0.47));
        }
    } else {
        // More source meshes than the fixed shader sample budget: retain an
        // unbiased power-weighted group choice, then importance-sample a
        // receiver-facing triangle inside the selected mesh.
        float groupRotation=JITTER_ENABLED!=0
            ? hash13(position+float(FRAME_INDEX)*vec3(0.17,0.31,0.47))
            : 0.0;
        float groupTarget=fract((float(sampleIndex)+0.5)/float(sampleCount)+groupRotation)*
                          EMITTER_TOTAL_WEIGHT;
        int globalSelected=min(EMITTER_COUNT-1,MAX_EMITTER_TRIANGLES-1);
        for (int i=0;i<MAX_EMITTER_TRIANGLES;++i) {
            if (i>=EMITTER_COUNT) break;
            if (groupTarget<=emitterTriangles[i].aCdf.w) { globalSelected=i; break; }
        }
        groupIndex=int(floor(emitterTriangles[globalSelected].emission.w+0.5));
        float groupWeight=0.0;
        for (int i=0;i<MAX_EMITTER_TRIANGLES;++i) {
            if (i>=EMITTER_COUNT) break;
            if (int(floor(emitterTriangles[i].emission.w+0.5))==groupIndex) {
                groupWeight+=emitterTriangles[i].bWeight.w;
            }
        }
        sampleNormalization=EMITTER_TOTAL_WEIGHT/
            max(float(sampleCount)*groupWeight,0.000001);
        receiverRotation=JITTER_ENABLED!=0
            ? hash13(position+float(FRAME_INDEX+sampleIndex)*vec3(0.29,0.13,0.41))
            : 0.0;
    }

    int groupFirst=-1;
    int groupEnd=-1;
    for (int i=0;i<MAX_EMITTER_TRIANGLES;++i) {
        if (i>=EMITTER_COUNT) break;
        int candidateGroup=int(floor(emitterTriangles[i].emission.w+0.5));
        if (candidateGroup==groupIndex) {
            if (groupFirst<0) groupFirst=i;
            groupEnd=i+1;
        } else if (groupFirst>=0) {
            break;
        }
    }
    if (groupFirst<0) return -1;

    float importanceTotal=0.0;
    for (int localIndex=0;localIndex<MAX_EMITTER_TRIANGLES;++localIndex) {
        int i=groupFirst+localIndex;
        if (i>=groupEnd) break;
        importanceTotal+=emitterSurfaceImportance(i,position,normal);
    }
    if (importanceTotal<=0.0000001) return -1;

    float localStratum=groupCount<=MAX_SHADOW_SAMPLES
        ? (float(localSampleIndex)+0.5)/float(groupSampleCount)
        : fract((float(sampleIndex)+0.5)*golden);
    float target=fract(localStratum+receiverRotation)*importanceTotal;
    float accumulated=0.0;
    float selectedImportance=0.0;
    int selected=-1;
    for (int localIndex=0;localIndex<MAX_EMITTER_TRIANGLES;++localIndex) {
        int i=groupFirst+localIndex;
        if (i>=groupEnd) break;
        float importance=emitterSurfaceImportance(i,position,normal);
        accumulated+=importance;
        if (selected<0 && target<=accumulated) {
            selected=i;
            selectedImportance=importance;
        }
    }
    if (selected<0 || selectedImportance<=0.0000001) return -1;

    EmitterTriangle selectedSurface=emitterTriangles[selected];
    int surfaceTriangleCount=max(1,int(floor(selectedSurface.surfaceNormalMeta.w+0.5)));
    float surfaceArea=max(selectedSurface.surfaceCenterArea.w,0.000001);
    float sequence=float(localSampleIndex)+0.5;
    vec2 groupOffset=vec2(float(groupIndex)*golden,float(groupIndex)*golden*golden);
    float triangleTarget=fract(sequence*golden*golden*golden+groupOffset.y+receiverRotation*0.37)*
                         surfaceArea;
    float triangleAccumulated=0.0;
    int selectedTriangle=selected;
    for (int localIndex=0;localIndex<MAX_EMITTER_TRIANGLES;++localIndex) {
        if (localIndex>=surfaceTriangleCount || selected+localIndex>=EMITTER_COUNT) break;
        int triangleIndex=selected+localIndex;
        triangleAccumulated+=max(emitterTriangles[triangleIndex].cArea.w,0.0);
        if (triangleTarget<=triangleAccumulated) {
            selectedTriangle=triangleIndex;
            break;
        }
    }
    float triangleArea=max(emitterTriangles[selectedTriangle].cArea.w,0.000001);
    sampleNormalization*=importanceTotal/selectedImportance*surfaceArea/triangleArea;

    triangleXi=fract(vec2(sequence*golden,sequence*golden*golden)+groupOffset);
    if (JITTER_ENABLED!=0) triangleXi=fract(triangleXi+vec2(receiverRotation,receiverRotation*0.73));
    return selectedTriangle;
}

vec3 sampleFiniteEmitters(vec3 position, vec3 normal) {
    if (EMITTER_COUNT <= 0 || EMITTER_TOTAL_WEIGHT <= 0.0) return vec3(0.0);
    int count=finiteEmitterSampleCount();
    vec3 result = vec3(0.0);
    for (int sampleIndex=0;sampleIndex<MAX_SHADOW_SAMPLES;++sampleIndex) {
        if (sampleIndex>=count) break;
        float sampleNormalization=0.0;
        vec2 xi=vec2(0.0);
        int selected=selectFiniteEmitter(
            position,normal,sampleIndex,count,sampleNormalization,xi);
        if (selected<0) continue;
        EmitterTriangle emitter=emitterTriangles[selected];
        float root=sqrt(xi.x);
        float w0=1.0-root;
        float w1=root*(1.0-xi.y);
        float w2=root*xi.y;
        vec3 lightPoint=emitter.aCdf.xyz*w0+emitter.bWeight.xyz*w1+emitter.cArea.xyz*w2;
        vec3 lightNormal=emitter.surfaceNormalMeta.xyz;
        vec3 delta=lightPoint-position;
        float distanceSquared=dot(delta,delta);
        if (distanceSquared<=EPS*EPS) continue;
        float distance=sqrt(distanceSquared);
        vec3 direction=delta/distance;
        float surfaceCosine=max(dot(normal,direction),0.0);
        float lightCosine=max(dot(lightNormal,-direction),0.0);
        if (surfaceCosine<=0.0 || lightCosine<=0.0) continue;
        vec3 shadowOrigin=position+normal*(4.0*EPS);
        vec3 shadowDelta=lightPoint-shadowOrigin;
        float shadowDistance=length(shadowDelta);
        if (sceneOccluded(
                shadowOrigin,shadowDelta/max(shadowDistance,EPS),
                max(shadowDistance-4.0*EPS,EPS))) continue;
        float area=max(emitter.cArea.w,0.000001);
        result+=EMISSION_TRANSPORT_SCALE*emitter.emission.rgb*surfaceCosine*lightCosine*
                area*sampleNormalization/
                max(PI*distanceSquared,0.000001);
    }
    return result;
}

vec3 sampleSky(vec3 position, vec3 normal) {
    const float golden = 0.61803398875;
    vec3 tangent,bitangent;
    buildBasis(normal,tangent,bitangent);
    int count = clamp(SHADOW_SAMPLE_COUNT,1,MAX_SHADOW_SAMPLES);
    vec3 result = vec3(0.0);
    for (int i=0; i<MAX_SHADOW_SAMPLES; ++i) {
        if (i >= count) break;
        vec2 xi = vec2((float(i)+0.5)/float(count),fract((float(i)+0.5)*golden));
        if (JITTER_ENABLED != 0) xi=fract(xi+hash13(position.zyx+float(FRAME_INDEX+i)));
        vec3 local=cosineHemisphere(xi);
        vec3 direction=normalize(tangent*local.x+bitangent*local.y+normal*local.z);
        if (direction.y <= 0.0) continue;
        if (!sceneOccluded(
                position+normal*(4.0*EPS),direction,MAX_TRACE_DISTANCE)) {
            result += environmentRadiance(direction);
        }
    }
    return result/float(count);
}

vec3 directIrradiance(Hit surface) {
    return sampleFiniteEmitters(surface.position,surface.normal) +
           sampleSky(surface.position,surface.normal);
}

vec3 previousIndirect(Hit surface) {
    const float golden=0.61803398875;
    vec3 tangent,bitangent;
    buildBasis(surface.normal,tangent,bitangent);
    vec3 result=vec3(0.0);
    int count=clamp(INDIRECT_SAMPLE_COUNT,1,MAX_INDIRECT_SAMPLES);
    for (int i=0; i<MAX_INDIRECT_SAMPLES; ++i) {
        if (i>=count) break;
        vec2 xi=vec2((float(i)+0.5)/float(count),fract((float(i)+0.5)*golden));
        vec3 local=cosineHemisphere(xi);
        vec3 direction=normalize(tangent*local.x+bitangent*local.y+surface.normal*local.z);
        result += sampleCascade(surface.position+surface.normal*(4.0*EPS),surface.normal,direction);
    }
    return result/float(count);
}

vec3 traceIntervalPath(vec3 origin, vec3 direction, float maximumDistance, bool outerInterval,
                       out bool occupied) {
    vec3 throughput=vec3(1.0);
    float remaining=maximumDistance;
    bool followedSpecular=false;
    occupied=false;
    for (int depth=0; depth<3; ++depth) {
        // Explicit finite-light sampling owns diffuse emission transport. A
        // light mesh must not terminate/occlude an ordinary cascade ray, but
        // remains visible after a specular bounce.
        Hit hit=followedSpecular
            ? traceScene(origin,direction,remaining)
            : traceOccluders(origin,direction,remaining);
        // Direct finite emitters and the infinite sky are sampled explicitly at the
        // shaded surface. Keep terminal opacity here, but do not count a raw source
        // hit a second time through the indirect cascade.
        if (!hit.exists) return followedSpecular ? throughput*environmentRadiance(direction) : vec3(0.0);
        occupied=true;
        remaining -= hit.t;
        if (!hit.frontFacing) return vec3(0.0);
        if (length(hit.emission)>0.0001) {
            return followedSpecular ? throughput*hit.emission*EMISSION_TRANSPORT_SCALE : vec3(0.0);
        }
        if (hit.reflectivity>0.5 && remaining>EPS) {
            throughput *= hit.albedo*hit.reflectivity;
            direction=normalize(reflect(direction,hit.normal));
            origin=hit.position+hit.normal*(4.0*EPS);
            remaining=MAX_TRACE_DISTANCE;
            followedSpecular=true;
            continue;
        }
        vec3 irradiance=directIrradiance(hit);
        if (ITERATION>0) irradiance += previousIndirect(hit);
        return throughput*hit.albedo*irradiance;
    }
    return vec3(0.0);
}

void main() {
    int levelSide=ANGULAR_RES_0*PROBE_COUNT_0;
    int level=int(floor(gl_FragCoord.x/float(levelSide)));
    if (level<0 || level>=LEVEL_COUNT) { finalColor=vec4(0.0); return; }
    int angularSize=ANGULAR_RES_0<<level;
    float withinX=gl_FragCoord.x-float(level*levelSide);
    int z=int(floor(gl_FragCoord.y/float(levelSide)));
    float withinY=gl_FragCoord.y-float(z*levelSide);
    int x=int(floor(withinX/float(angularSize)));
    int y=int(floor(withinY/float(angularSize)));
    int probeCount=max(1,PROBE_COUNT_0>>level);
    vec2 disk=2.0*mod(vec2(withinX,withinY),float(angularSize))/float(angularSize)-1.0;
    if (dot(disk,disk)>1.0 || x>=probeCount || y>=probeCount || z>=probeCount) {
        finalColor=vec4(0.0);
        return;
    }
    vec3 spacing=VOLUME_SIZE/float(probeCount);
    vec3 probePosition=LEVEL_VOLUME_MIN[level]+(vec3(x,y,z)+0.5)*spacing;
    vec3 direction=diskToDirection(disk);
    float intervalStart=level==0 ? 0.0 : INTERVAL_SCALE*exp2(float(level-1));
    float intervalEnd=level==LEVEL_COUNT-1 ? MAX_TRACE_DISTANCE : INTERVAL_SCALE*exp2(float(level));
    bool occupied=false;
    float overlapStart=max(0.0,intervalStart-4.0*EPS);
    vec3 outgoing=traceIntervalPath(
        probePosition+direction*overlapStart,direction,intervalEnd-overlapStart+4.0*EPS,
        level==LEVEL_COUNT-1,occupied);
    float terminal=(occupied || level==LEVEL_COUNT-1) ? 1.0 : 0.0;
    finalColor=vec4(outgoing,terminal);
}
