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

struct SceneTriangle { vec4 a; vec4 b; vec4 c; vec4 albedoReflectivity; vec4 emission; };
struct EmitterTriangle {
    vec4 aCdf;
    vec4 bWeight;
    vec4 cArea;
    vec4 emission;
    vec4 surfaceCenterArea;
    vec4 surfaceNormalMeta;
};
struct BvhNode { vec4 boundsMin; vec4 boundsMax; uvec4 meta; };
layout(std430, binding = 11) readonly buffer SceneTriangles { SceneTriangle sceneTriangles[]; };
layout(std430, binding = 12) readonly buffer EmitterTriangles { EmitterTriangle emitterTriangles[]; };
layout(std430, binding = 13) readonly buffer BvhNodes { BvhNode bvhNodes[]; };
layout(std430, binding = 14) readonly buffer DynamicTriangles { SceneTriangle dynamicTriangles[]; };
layout(std430, binding = 15) readonly buffer DynamicBvhNodes { BvhNode dynamicBvhNodes[]; };

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform sampler2D texture2;
uniform sampler2D texture3;
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
uniform int INDIRECT_SAMPLE_COUNT;
uniform int SHADOW_SAMPLE_COUNT;
uniform int FRAME_INDEX;
uniform int JITTER_ENABLED;
uniform int CORNER_MERGE_ENABLED;
uniform vec3 VOLUME_MIN;
uniform vec3 VOLUME_SIZE;
uniform vec3 LEVEL_VOLUME_MIN[MAX_LEVELS];
uniform vec3 SKY_TOP_RADIANCE;
uniform vec3 SKY_BOTTOM_RADIANCE;
uniform float MAX_TRACE_DISTANCE;
uniform vec3 MESH_ORIGIN;
uniform vec3 SURFACE_NORMAL;
uniform vec3 SURFACE_TANGENT;
uniform vec3 SURFACE_BITANGENT;
uniform float SURFACE_PLANE;
uniform vec2 SURFACE_GRID_MIN;
uniform vec2 SURFACE_GRID_SIZE;
uniform vec2 RESOLVE_TARGET_SIZE;
uniform float PIXELS_PER_METER;
uniform vec4 MATERIAL_TINT;
uniform vec3 MATERIAL_EMISSION;
uniform float MATERIAL_REFLECTIVITY;
uniform vec3 CAMERA_POSITION;
uniform float AMBIENT;
uniform float LIGHTING_WEIGHT;
uniform float TEMPORAL_BLEND;
uniform int DYNAMIC_SHADOW_ONLY;
uniform int HAS_PAINT;
uniform vec3 PAINT_TANGENT;
uniform vec3 PAINT_BITANGENT;
uniform vec2 PAINT_GRID_MIN;
uniform vec2 PAINT_GRID_SIZE;

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
    p=fract(p*0.1031);
    p+=dot(p,p.yzx+33.33);
    return fract((p.x+p.y)*p.z);
}

vec3 environmentRadiance(vec3 direction) {
    if (direction.y<=0.0) return vec3(0.0);
    float height=sqrt(clamp(direction.y,0.0,1.0));
    return mix(SKY_BOTTOM_RADIANCE,SKY_TOP_RADIANCE,height);
}

bool intersectBounds(vec3 origin, vec3 direction, vec3 boundsMin, vec3 boundsMax, float maximumDistance) {
    vec3 safeDirection=vec3(
        abs(direction.x)<0.000001 ? (direction.x<0.0 ? -0.000001 : 0.000001) : direction.x,
        abs(direction.y)<0.000001 ? (direction.y<0.0 ? -0.000001 : 0.000001) : direction.y,
        abs(direction.z)<0.000001 ? (direction.z<0.0 ? -0.000001 : 0.000001) : direction.z);
    vec3 inverse=1.0/safeDirection;
    vec3 t0=(boundsMin-origin)*inverse;
    vec3 t1=(boundsMax-origin)*inverse;
    vec3 nearValues=min(t0,t1);
    vec3 farValues=max(t0,t1);
    float nearT=max(max(nearValues.x,nearValues.y),nearValues.z);
    float farT=min(min(farValues.x,farValues.y),farValues.z);
    return farT>=max(nearT,EPS) && nearT<maximumDistance;
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
    t=dot(edge2,q)*inverseDeterminant;
    if (t<=EPS || t>=maximumDistance) return false;
    normal=normalize(cross(edge1,edge2));
    frontFacing=dot(normal,direction)<0.0;
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
    int nodeCount=dynamicScene ? DYNAMIC_BVH_NODE_COUNT : BVH_NODE_COUNT;
    int triangleLimit=dynamicScene ? DYNAMIC_TRIANGLE_COUNT : TRIANGLE_COUNT;
    if (nodeCount>0) {
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
            if (!intersectBounds(origin,direction,node.boundsMin.xyz,node.boundsMax.xyz,hit.t)) continue;
            int triangleCount=int(node.meta.w);
            if (triangleCount>0) {
                int first=int(node.meta.z);
                for (int localIndex=0;localIndex<triangleCount;++localIndex) {
                    int triangleIndex=first+localIndex;
                    if (triangleIndex<0 || triangleIndex>=triangleLimit) continue;
                    float candidateT;
                    vec3 candidateNormal;
                    bool candidateFrontFacing;
                    SceneTriangle tri;
                    if (dynamicScene) tri=dynamicTriangles[triangleIndex];
                    else tri=sceneTriangles[triangleIndex];
                    // Emissive meshes are visible geometry, but they are not
                    // blockers.  Keeping this choice at traversal time lets a
                    // mirror ray see the source while direct/sky visibility
                    // rays continue through every light mesh.
                    if (skipEmissive && length(tri.emission.rgb)>0.0001) continue;
                    if (!intersectTriangle(
                            origin,direction,tri,hit.t,candidateT,candidateNormal,candidateFrontFacing)) continue;
                    hit.exists=true;
                    hit.t=candidateT;
                    hit.position=origin+direction*candidateT;
                    hit.normal=candidateNormal;
                    hit.frontFacing=candidateFrontFacing;
                    hit.albedo=tri.albedoReflectivity.rgb;
                    hit.reflectivity=tri.albedoReflectivity.a;
                    hit.emission=tri.emission.rgb;
                }
            } else if (stackSize<=MAX_BVH_STACK-2) {
                stack[stackSize++]=int(node.meta.x);
                stack[stackSize++]=int(node.meta.y);
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
    hit.exists=false;
    hit.frontFacing=true;
    hit.t=maximumDistance;
    hit.position=vec3(0.0);
    hit.normal=vec3(0.0,1.0,0.0);
    hit.albedo=vec3(0.0);
    hit.reflectivity=0.0;
    hit.emission=vec3(0.0);
    traceBvh(origin,direction,false,skipEmissive,hit);
    traceBvh(origin,direction,true,skipEmissive,hit);
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
    if (direction.y>0.999999) return vec2(0,1);
    return 0.5*sqrt(2.0/max(0.000001,1.0-direction.y))*direction.xz;
}

vec4 sampleProbe(int level, vec3 probe, vec3 direction) {
    int levelSide=ANGULAR_RES_0*PROBE_COUNT_0;
    int angularSize=ANGULAR_RES_0<<level;
    vec2 origin=vec2(level*levelSide+int(probe.x)*angularSize,
                     int(probe.z)*levelSide+int(probe.y)*angularSize);
    vec2 disk=directionToDisk(direction);
    float safeRadius=max(0.0,1.0-1.5/float(angularSize));
    if (length(disk)>safeRadius) disk*=safeRadius/max(length(disk),0.000001);
    vec2 pixel=origin+clamp((disk+1.0)*0.5*float(angularSize),
                            vec2(0.5),vec2(float(angularSize)-0.5));
    return texture(texture1,pixel/vec2(textureSize(texture1,0)));
}

vec4 interpolateLevel(int level, vec3 position, vec3 normal, vec3 direction) {
    int probeCount=max(1,PROBE_COUNT_0>>level);
    vec3 spacing=VOLUME_SIZE/float(probeCount);
    vec3 levelMin=LEVEL_VOLUME_MIN[level];
    vec3 coordinate=(position-levelMin)/spacing-0.5;
    vec3 base=floor(coordinate);
    vec3 fraction=fract(coordinate);
    vec4 value=vec4(0.0);
    float weightSum=0.0;
    for (int z=0;z<2;++z) for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
        vec3 corner=vec3(x,y,z);
        vec3 probe=clamp(base+corner,vec3(0.0),vec3(float(probeCount-1)));
        vec3 weights=mix(vec3(1.0)-fraction,fraction,corner);
        float weight=weights.x*weights.y*weights.z;
        vec3 probePosition=levelMin+(probe+0.5)*spacing;
        weight*=smoothstep(-0.15*length(spacing),0.35*length(spacing),
                           dot(probePosition-position,normal));
        value+=sampleProbe(level,probe,direction)*weight;
        weightSum+=weight;
    }
    return weightSum>0.00001 ? value/weightSum : vec4(0.0);
}

float cascadeCoverage(vec3 position) {
    vec3 edgeDistance=min(position-VOLUME_MIN,VOLUME_MIN+VOLUME_SIZE-position);
    float nearestEdge=min(edgeDistance.x,min(edgeDistance.y,edgeDistance.z));
    if (nearestEdge<=0.0) return 0.0;
    float fadeWidth=max(0.001,length(VOLUME_SIZE/float(PROBE_COUNT_0)));
    return smoothstep(0.0,fadeWidth,nearestEdge);
}

vec3 sampleCascade(vec3 position, vec3 normal, vec3 direction) {
    vec3 radiance=vec3(0.0);
    float opacity=0.0;
    for (int level=0;level<MAX_LEVELS;++level) {
        if (level>=LEVEL_COUNT) break;
        vec4 segment=interpolateLevel(level,position,normal,direction);
        radiance+=(1.0-opacity)*segment.rgb;
        opacity+=(1.0-opacity)*segment.a;
        if (opacity>0.999) break;
    }
    // Probe origins are snapped in world space, so samples in the overlap of
    // adjacent anchor volumes are identical.  Applying an anchor-relative
    // volume-edge weight here made those identical samples jump when the
    // active chunk changed.  Distance decay belongs to presentation; the
    // camera-independent surface cache keeps the probe result itself.
    return radiance;
}

vec3 sampleCascadeCornerMerged(vec3 position, vec3 normal, vec3 direction) {
    vec3 spacing=VOLUME_SIZE/float(PROBE_COUNT_0);
    vec3 levelMin=LEVEL_VOLUME_MIN[0];
    vec3 coordinate=(position-levelMin)/spacing-0.5;
    vec3 base=floor(coordinate);
    vec3 fraction=fract(coordinate);
    vec3 weighted=vec3(0.0);
    float weightSum=0.0;
    for (int z=0;z<2;++z) for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
        vec3 corner=vec3(x,y,z);
        vec3 probe=clamp(base+corner,vec3(0.0),vec3(float(PROBE_COUNT_0-1)));
        vec3 weights=mix(vec3(1.0)-fraction,fraction,corner);
        float weight=weights.x*weights.y*weights.z;
        vec3 probePosition=levelMin+(probe+0.5)*spacing;
        weight*=smoothstep(-0.15*length(spacing),0.35*length(spacing),
                           dot(probePosition-position,normal));
        vec4 nearSegment=sampleProbe(0,probe,direction);
        vec3 radiance=nearSegment.rgb;
        float opacity=nearSegment.a;
        for (int level=1;level<MAX_LEVELS;++level) {
            if (level>=LEVEL_COUNT || opacity>0.999) break;
            vec4 farSegment=interpolateLevel(level,probePosition,normal,direction);
            radiance+=(1.0-opacity)*farSegment.rgb;
            opacity+=(1.0-opacity)*farSegment.a;
        }
        weighted+=radiance*weight;
        weightSum+=weight;
    }
    return weightSum>0.00001 ? weighted/weightSum : sampleCascade(position,normal,direction);
}

void buildBasis(vec3 normal, out vec3 tangent, out vec3 bitangent) {
    vec3 up=abs(normal.z)<0.999 ? vec3(0,0,1) : vec3(0,1,0);
    tangent=normalize(cross(up,normal));
    bitangent=cross(normal,tangent);
}

vec3 cosineHemisphere(vec2 u) {
    float radius=sqrt(u.x);
    float angle=2.0*PI*u.y;
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

    // Emitter records are sorted into contiguous mesh groups on the CPU.
    // Find the selected range once so the two receiver-importance passes do
    // not rescan every triangle of every other light for each shadow sample.
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

    // The selected record is the first exact triangle of a contiguous
    // coplanar emitter surface. Select one of that surface's real triangles by
    // area, then apply the inverse surface and triangle probabilities.
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
    if (EMITTER_COUNT<=0 || EMITTER_TOTAL_WEIGHT<=0.0) return vec3(0.0);
    int count=finiteEmitterSampleCount();
    vec3 result=vec3(0.0);
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
    const float golden=0.61803398875;
    vec3 tangent,bitangent;
    buildBasis(normal,tangent,bitangent);
    int count=clamp(SHADOW_SAMPLE_COUNT,1,MAX_SHADOW_SAMPLES);
    vec3 result=vec3(0.0);
    for (int i=0;i<MAX_SHADOW_SAMPLES;++i) {
        if (i>=count) break;
        vec2 xi=vec2((float(i)+0.5)/float(count),fract((float(i)+0.5)*golden));
        if (JITTER_ENABLED!=0) xi=fract(xi+hash13(position.zyx+float(FRAME_INDEX+i)));
        vec3 local=cosineHemisphere(xi);
        vec3 direction=normalize(tangent*local.x+bitangent*local.y+normal*local.z);
        if (direction.y<=0.0) continue;
        if (!sceneOccluded(
                position+normal*(4.0*EPS),direction,MAX_TRACE_DISTANCE)) {
            result+=environmentRadiance(direction);
        }
    }
    return result/float(count);
}

vec3 directIrradiance(vec3 position, vec3 normal) {
    return sampleFiniteEmitters(position,normal)+sampleSky(position,normal);
}

vec3 indirectIrradiance(vec3 position, vec3 normal) {
    const float golden=0.61803398875;
    vec3 tangent,bitangent;
    buildBasis(normal,tangent,bitangent);
    vec3 result=vec3(0.0);
    int count=clamp(INDIRECT_SAMPLE_COUNT,1,MAX_INDIRECT_SAMPLES);
    for (int i=0;i<MAX_INDIRECT_SAMPLES;++i) {
        if (i>=count) break;
        vec2 xi=vec2((float(i)+0.5)/float(count),fract((float(i)+0.5)*golden));
        if (JITTER_ENABLED!=0) xi=fract(xi+hash13(position.zyx+float(FRAME_INDEX+i)));
        vec3 local=cosineHemisphere(xi);
        vec3 direction=normalize(tangent*local.x+bitangent*local.y+normal*local.z);
        vec3 samplePosition=position+normal*(4.0*EPS);
        result+=CORNER_MERGE_ENABLED!=0
            ? sampleCascadeCornerMerged(samplePosition,normal,direction)
            : sampleCascade(samplePosition,normal,direction);
    }
    return result/float(count);
}

vec3 shadeDiffuse(Hit hit) {
    vec3 irradiance=directIrradiance(hit.position,hit.normal)+
                    indirectIrradiance(hit.position,hit.normal)+vec3(AMBIENT*0.08);
    return hit.albedo*irradiance;
}

vec3 tracePath(vec3 origin, vec3 direction) {
    // Match Trivox's realtime camera path: deterministic specular transport,
    // followed by the regular direct + RC diffuse resolve at the first
    // non-mirror hit.  The old five-bounce hashed cosine walk ran even when
    // jitter was disabled, creating stable firefly pixels and multiplying the
    // cost of every reflective world texel.
    vec3 throughput=vec3(1.0);
    for (int depth=0;depth<3;++depth) {
        Hit hit=traceScene(origin,direction,MAX_TRACE_DISTANCE);
        if (!hit.exists) {
            return throughput*environmentRadiance(direction);
        }
        if (!hit.frontFacing) return vec3(0.0);
        if (length(hit.emission)>0.0001) {
            return throughput*hit.emission*EMISSION_TRANSPORT_SCALE;
        }
        if (hit.reflectivity>0.5) {
            throughput*=hit.albedo*hit.reflectivity;
            direction=normalize(reflect(direction,hit.normal));
            origin=hit.position+hit.normal*(4.0*EPS);
            continue;
        }
        return throughput*shadeDiffuse(hit);
    }
    return vec3(0.0);
}

vec3 acesFilm(vec3 color) {
    return clamp((color*(2.51*color+0.03))/(color*(2.43*color+0.59)+0.14),0.0,1.0);
}

ivec2 wrappedTexel(vec2 uv, ivec2 size) {
    ivec2 pixel=ivec2(floor(uv*vec2(size)));
    return ivec2((pixel.x%size.x+size.x)%size.x,(pixel.y%size.y+size.y)%size.y);
}

bool dynamicOccluded(vec3 origin, vec3 direction, float maximumDistance) {
    Hit hit;
    hit.exists=false;
    hit.frontFacing=true;
    hit.t=maximumDistance;
    hit.position=vec3(0.0);
    hit.normal=vec3(0.0,1.0,0.0);
    hit.albedo=vec3(0.0);
    hit.reflectivity=0.0;
    hit.emission=vec3(0.0);
    traceBvh(origin,direction,true,true,hit);
    return hit.exists;
}

float dynamicLightVisibility(vec3 position, vec3 normal) {
    const float golden=0.61803398875;
    vec3 tangent,bitangent;
    buildBasis(normal,tangent,bitangent);
    float potential=0.0;
    float visible=0.0;
    int skyCount=clamp(SHADOW_SAMPLE_COUNT,1,MAX_SHADOW_SAMPLES);
    for (int i=0;i<MAX_SHADOW_SAMPLES;++i) {
        if (i>=skyCount) break;
        vec2 xi=vec2((float(i)+0.5)/float(skyCount),fract((float(i)+0.5)*golden));
        vec3 local=cosineHemisphere(xi);
        vec3 direction=normalize(tangent*local.x+bitangent*local.y+normal*local.z);
        if (direction.y<=0.0) continue;
        float weight=max(dot(environmentRadiance(direction),vec3(0.2126,0.7152,0.0722)),0.0001)/
                     float(skyCount);
        potential+=weight;
        if (!dynamicOccluded(position+normal*(4.0*EPS),direction,MAX_TRACE_DISTANCE)) visible+=weight;
    }
    if (EMITTER_COUNT>0 && EMITTER_TOTAL_WEIGHT>0.0) {
        int count=finiteEmitterSampleCount();
        for (int sampleIndex=0;sampleIndex<MAX_SHADOW_SAMPLES;++sampleIndex) {
            if (sampleIndex>=count) break;
            float sampleNormalization=0.0;
            vec2 xi=vec2(0.0);
            int selected=selectFiniteEmitter(
                position,normal,sampleIndex,count,sampleNormalization,xi);
            if (selected<0) continue;
            EmitterTriangle emitter=emitterTriangles[selected];
            float root=sqrt(xi.x);
            vec3 lightPoint=emitter.aCdf.xyz*(1.0-root)+
                emitter.bWeight.xyz*(root*(1.0-xi.y))+emitter.cArea.xyz*(root*xi.y);
            vec3 delta=lightPoint-position;
            float distanceSquared=dot(delta,delta);
            if (distanceSquared<=EPS*EPS) continue;
            float distance=sqrt(distanceSquared);
            vec3 direction=delta/distance;
            vec3 lightNormal=emitter.surfaceNormalMeta.xyz;
            float geometry=max(dot(normal,direction),0.0)*max(dot(lightNormal,-direction),0.0)/distanceSquared;
            float emission=max(dot(emitter.emission.rgb,vec3(0.2126,0.7152,0.0722)),0.0001);
            float weight=EMISSION_TRANSPORT_SCALE*geometry*emission*max(emitter.cArea.w,0.000001)*
                         sampleNormalization/PI;
            if (weight<=0.0) continue;
            potential+=weight;
            vec3 shadowOrigin=position+normal*(4.0*EPS);
            vec3 shadowDelta=lightPoint-shadowOrigin;
            float shadowDistance=length(shadowDelta);
            if (!dynamicOccluded(
                    shadowOrigin,shadowDelta/max(shadowDistance,EPS),
                    max(shadowDistance-4.0*EPS,EPS))) visible+=weight;
        }
    }
    return potential>0.000001 ? clamp(visible/potential,0.0,1.0) : 1.0;
}

void main() {
    vec2 surfaceUv=gl_FragCoord.xy/max(RESOLVE_TARGET_SIZE,vec2(1.0));
    vec2 grid=SURFACE_GRID_MIN+surfaceUv*SURFACE_GRID_SIZE;
    vec3 local=SURFACE_TANGENT*(grid.x/PIXELS_PER_METER)+
               SURFACE_BITANGENT*(grid.y/PIXELS_PER_METER)+SURFACE_NORMAL*SURFACE_PLANE;
    vec3 position=MESH_ORIGIN+local;
    if (DYNAMIC_SHADOW_ONLY!=0) {
        float visibility=dynamicLightVisibility(position,SURFACE_NORMAL);
        float multiplier=mix(0.40,1.0,visibility);
        finalColor=vec4(multiplier,multiplier,multiplier,1.0);
        return;
    }
    ivec2 baseSize=textureSize(texture0,0);
    vec2 baseUv=vec2(0.5+grid.x/float(baseSize.x),0.5+grid.y/float(baseSize.y));
    vec3 baseSample=texelFetch(texture0,wrappedTexel(baseUv,baseSize),0).rgb;
    vec3 base=pow(max(baseSample*MATERIAL_TINT.rgb,vec3(0.0)),vec3(2.2));
    if (HAS_PAINT!=0) {
        vec2 paintGrid=vec2(dot(local,PAINT_TANGENT),dot(local,PAINT_BITANGENT))*PIXELS_PER_METER;
        vec2 paintUv=(paintGrid-PAINT_GRID_MIN)/PAINT_GRID_SIZE;
        ivec2 paintSize=textureSize(texture2,0);
        ivec2 paintPixel=ivec2(floor(paintUv*vec2(paintSize)));
        if (all(greaterThanEqual(paintPixel,ivec2(0))) && all(lessThan(paintPixel,paintSize))) {
            vec4 paint=texelFetch(texture2,paintPixel,0);
            base=mix(base,pow(max(paint.rgb,vec3(0.0)),vec3(2.2)),paint.a);
        }
    }

    vec3 linearColor;
    if (length(MATERIAL_EMISSION)>0.0001) {
        linearColor=MATERIAL_EMISSION*EMISSION_TRANSPORT_SCALE;
    } else {
        vec3 irradiance=directIrradiance(position,SURFACE_NORMAL)+
                        indirectIrradiance(position,SURFACE_NORMAL)+vec3(AMBIENT*0.08);
        vec3 diffuse=base*irradiance;
        if (MATERIAL_REFLECTIVITY>0.001) {
            vec3 incoming=normalize(position-CAMERA_POSITION);
            vec3 reflected=normalize(reflect(incoming,SURFACE_NORMAL));
            vec3 reflectedColor=tracePath(position+SURFACE_NORMAL*(4.0*EPS),reflected);
            linearColor=mix(diffuse,reflectedColor,clamp(MATERIAL_REFLECTIVITY,0.0,1.0));
        } else {
            linearColor=diffuse;
        }
    }
    vec3 ambientBase=base*AMBIENT;
    linearColor=mix(ambientBase,linearColor,LIGHTING_WEIGHT);
    vec3 resolved=pow(acesFilm(max(linearColor,vec3(0.0))),vec3(1.0/2.2));
    vec3 previous=texelFetch(texture3,ivec2(gl_FragCoord.xy),0).rgb;
    finalColor=vec4(mix(resolved,previous,TEMPORAL_BLEND),1.0);
}
