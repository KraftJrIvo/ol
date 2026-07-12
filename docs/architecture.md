# OL Architecture Notes

This is the first working foundation, not the final engine. The code is deliberately
plain C++: structs, fixed arenas, free functions, and raw IDs. The goal is to keep
future interactive editing possible without committing to a heavy object model.

## World And Coordinates

A `World` owns named `Dimension` records. A dimension has constant `chunk_size_m`
and `meter_size_px`, render radii, sky/fog colors, chunks, visual records, and
physics state.

Large coordinates use `WorldPos`:

```cpp
dimension id + i32 chunk coord + Vector3 local meters inside that chunk
```

Every move canonicalizes `local` back into `[0, chunk_size)`. Rendering and physics
convert only nearby objects to camera-relative floats with `world_delta_meters`.
This keeps the camera and simulated islands near the floating-point origin even if
the player walks to very large chunk coordinates.

## Chunks And Interactive Editing

Chunks hold references to global per-dimension arenas for meshes and sprites. This
makes add/remove operations straightforward: edit the global record,
then update the owning chunk reference list. Objects are assigned to the chunk that
contains their origin for now; broad bounds crossing multiple chunks should be added
when streaming/editing grows.

The hills demo predicts the next x/z chunk from player velocity while the player is
still five meters from a seam. It keeps the old visible ring resident, prepares the
new ring in distance order, and commits at most two chunk loads and two unloads per
update. Collision-bearing neighbors are prioritized, avoiding a complete procedural
ring rebuild on the seam frame.

## Mesh Edges

The renderer draws mesh faces in 3D, queues explicit mesh edges, clips each edge
against the camera near plane and native render target, then draws native-pixel
screen-space quads directly against the completed scene depth. Occlusion and drawing
stay on the GPU, so lighting work is not serialized by a per-frame depth or SSBO
readback. Closer meshes, sprites, and player geometry still hide covered edge spans.

The current edge pipeline is:

1. Render normal geometry into color and depth.
2. Draw sprites and ordinary player meshes into that same depth buffer.
3. Copy the final depth attachment to a GPU depth texture; no CPU readback occurs.
4. Project and near/screen-clip each authored edge.
5. Batch the resulting pixel-width quads through a fragment shader that rejects
   fragments behind the frozen scene depth.

The older CPU ray-cut and compute/readback implementations remain available to the
test benchmark as reference paths. Image comparisons cover representative hills
views and keep the shader path within a very small edge-pixel tolerance.

This gives stable pixel-width contours while avoiding 3D line-strip artifacts.
Contour selection is still explicit authoring data; the demo hand-authors combined
contours for multi-box stairs and the tunnel instead of deriving arbitrary merged
object outlines.

Large streamed dimensions retain a coordinate-to-chunk hash and conservatively cull
mesh bounds against the camera frustum before parallel mesh/edge preparation. The
unculled reference remains in the visual benchmark, where representative playground
and hills views must be pixel-exact. Paint-cache validation hashes only meshes that
actually own saved paint instead of every far streamed mesh.

## Rendering Backend Boundary

Raylib remains the platform, input, window, and basic resource shell; it is not the
submission boundary for the performance-sensitive renderer. The OpenGL 4.3 path
already binds SSBOs, dispatches compute work, manages depth/color attachments, and
issues memory barriers directly through GL/rlgl. Measurements consequently have to
separate engine work from the library wrapped around the context. In particular,
the synchronized 540p and 1080p motion benchmarks reproduce the reported distinction:
stationary frame times are normally high-FPS, while walking and seam crossings expose
the expensive frames, so walking is the primary acceptance bucket. Their counters
show that scene signatures and topology scans are small; the measured cave spikes
follow foreground per-face framebuffer attachments, resolves, mip operations, and
synchronous visible-region repair. This does not indicate a raylib CPU ceiling.

The current path also caches shader-uniform locations, the common radiance uniform
set for each shader/scene/frame signature, and the five scene SSBO bindings. A face
resolve therefore changes only its surface-specific uniforms instead of repeating the
same common uploads and `rlBindShaderBuffer` calls for every target. These caches trim
submission overhead, but they do not batch the remaining per-face framebuffer work.

An atlas or texture-array lightmap cache, tiled compute resolve, persistent scene
buffers, and a compact spherical-harmonic irradiance representation are future
structural work; none of those are implemented by the current directional RC atlas.
They would allow one or a few batched dispatches instead of a framebuffer lifecycle
per face. GPU timestamp queries and submission counters should remain the decision
inputs. Replacing raylib with raw OpenGL would keep the same driver and synchronization
costs, while a Vulkan rewrite would add explicit lifetime/synchronization machinery
without fixing fragmented work. A lower-level backend becomes worthwhile only if the
batched design later measures a real API submission ceiling or needs a platform
feature OpenGL cannot provide.

## World Texture Pixels

Each dimension owns a `pixels_per_meter` setting (16 by default). Mesh faces use
centered planar world-space UVs, so one meter always spans that many texture texels
and non-integer face dimensions expose symmetric partial edge texels. Generated
mipmaps and anisotropic filtering reduce noise at oblique angles without changing
the world-space texel scale. Sprite source pixels use the same setting to determine
their size in meters.

Painted mesh texels are assembled into filtered per-mesh, per-face texture layers.
Each layer uses the same world-space texel grid as its authored texture and receives
mipmaps and anisotropic filtering as one continuous image. The layer is rendered
across the complete face with hardware depth bias, so close views do not expose a gap.
Its polygons carry face-clipping bounds for partial edge texels and fade over the
same chunk range as authored mesh edges. Sprite paint stores sprite-local texel
coordinates and is baked into a cached per-sprite texture copy whenever those
pixels change; sprites therefore have no paint decal geometry. This keeps paint stable when hills chunks
unload and reload. Paint, replacement, and five-texel-diameter erase operations are
sent reliably to current peers, and the lobby host replays the paint history plus an
explicit completion marker when another player joins. Saved sessions serialize the
surviving paint records and restore them when their world is generated again.

Paint and erase rays are limited to 10 meters. Sprite raycasts consult a cached
source-texture alpha mask and use the renderer's 0.5 alpha cutoff, allowing rays to
continue through transparent sprite pixels to surfaces behind them.

Painted sprite texture copies and mesh-face paint layers receive mipmaps and 8x
anisotropic filtering. This keeps neighboring painted texels contiguous up close
while filtering the complete painted pattern in oblique and distant views.
Paint layers are indexed by their owning mesh and cached by content hash, so a
stroke rebuilds only the touched mesh; streamed topology changes resolve stale mesh
references once and preserve unaffected GPU textures.

## Surface-Texel Radiance Cascades

Lighting follows the radiance-cascade near/far tradeoff without using Trivox's
full-screen camera-ray resolve. A four-chunk world-anchored probe volume is centred on
the active chunk centre. Each of its four distance intervals has a separately
world-snapped lattice, progressively trading probe spatial density for angular
density; RGB stores incoming radiance and alpha stores segment occlusion. The integer
half-chunk phase calculation remains exact even at million-chunk coordinates.
Adjacent chunks sample one shared active volume rather than independent per-chunk
atlases. At the default eight base probes this uses 8 m level-zero spacing (rather
than the former 6 m spacing over three chunks) without increasing atlas size. The
larger overlap keeps ordinary seam-near receivers on corresponding probes; it is a
finite trace window, not a guarantee that every texel of an arbitrarily large face is
inside full-quality coverage. Its outer region still uses cascade coverage and
presentation fades.

World geometry is uploaded as exact transformed triangles and reordered into a
median-split GPU BVH. Static nearby non-emissive geometry keeps a cached BVH, while
player bodies are ordinary 32-sided triangle meshes in a small dynamic BVH rebuilt
each frame; the lighting shaders contain no player-cylinder intersection primitive.
Material-emissive triangles instead enter a compact dynamic area-and-power CDF that is
refreshed without rebuilding static scene or surface topology. This is acceleration
data, not a point-light entity or chunk-light list. Triangles retain their
emissive-mesh and coplanar-surface grouping. Direct sampling represents every source
before spending extra samples, selects a receiver-facing surface by area, luminance,
orientation, and distance, then samples one of that surface's real triangles by area.
Conditional on the chosen surface, this is exact area sampling over its constituent
triangles: the sampled point stays on authored emissive geometry rather than an
approximate point source. It also avoids accidentally choosing only the back faces of
a closed mesh. Emissive geometry is visible to camera
and specular rays, but shadow, sky-visibility, and ordinary diffuse-cascade rays use
the non-emissive occluder BVHs and pass through it. The visible flying light cubes
therefore illuminate without casting artificial self-shadows; an emissive hit is
transported only after a specular path explicitly reaches it. Cascade passes trace the
active scene every rendered frame, evaluate exact finite intervals and world-texture
emitter/sky visibility, and iterate diffuse bounce transport. The unchanged visual sky
gradient also acts as an infinite overhead emitter when a traced upward ray escapes
the scene.

Static scene construction is canonical for an anchor chunk: mesh priority and the
emissive-triangle CDF are based on the anchor origin with a stable mesh-ID tie-break,
not the camera's local position. Returning to an anchor therefore reproduces the same
lighting samples. While the camera approaches a seam, motion predicts the likely
neighbor and an immutable triangle snapshot is built into a BVH on a worker thread.
The render thread incrementally uploads a separate staging scene and traces its
cascade. Promotion swaps the completed SSBOs and cascade atlas into the active
context; unfinished or stale work is rejected, and old GPU targets are retired over
later frames instead of being destroyed in one seam frame. Production enables exact
target-anchor surface preparation only when the emitter signature has remained stable
for at least two frames and the camera is moving toward the selected seam. It
prioritizes predicted-viewport/front-facing diffuse faces and spends at most 64K
texels and eight new target allocations per moving frame; it spends no surface work
while stationary. This content-driven policy handles static-light playground/hills
seams without baking colors which are already stale before promotion. The animated
cave emitters consequently submit zero automatic surface-prewarm texels.

The public `radiance_neighbor_surface_prewarm` switch still defaults false. Enabling
it is the forced animated-emitter benchmark path, with a deliberately smaller 16K
texel/one-allocation budget; it is not the production policy. The prepared
emitter-buffer signature travels with the scene so a static destination is not
falsely treated as a moving-light update. The newly active cascade traces in the
promotion frame and every rendered frame afterward.

At a promoted adjacent anchor, a compatible carried face texture is used only as
storage. Every texel that can contribute to the arrival frame is synchronously
repaired under the new canonical scene signature at L0, even if normal projected
density would later select a coarser mip. The updated valid rectangle is stamped with
the new signature and offscreen texels remain invalid until visible. Evaluating this
nonlinear lighting pass at canonical L0, then filtering its mip chain, makes the
visible result match a fresh/full resolve and avoids both a whole-face seam bake and
route-dependent coarse-cell samples.

Every flat face allocates a level-zero lighting grid at the dimension's exact
`pixels_per_meter`. A cold scene or paint change initializes the complete L0 face;
the adjacent-anchor repair described above is the bounded exception.
For live emitter animation, visible regions whose world texels are large on screen
still trace one shadow result per L0 world texel; progressively smaller projected
texels resolve directly into L1, L2, or L3, so one traced mip texel covers a 2x2,
4x4, or 8x8 block of the same world grid. Projected-density thresholds use hysteresis
to avoid mip thrashing. Mesh-origin distance is deliberately not a mip input, because
it made an entire face change resolution at one world-space threshold. This remains
world-texture work rather than per-screen-pixel lighting.
The resolve combines authored texture, paint, direct visibility, and indirect cascade
samples before filtered mip presentation. Magnification remains nearest-neighbor crisp
while minification uses mipmaps for stable oblique views.

Moving-player shadows do not duplicate that filtered color cache. Each affected
diffuse face lazily owns a persistent single-channel R8 multiplier mask, initialized
to white; the renderer updates or clears only the union of the current and previous
player footprints and composites the mask over the stable color lightmap at draw time.
An RGBA attachment is only a compatibility fallback when a driver rejects an R8 color
target--the normal update path never copies the stable RGBA lightmap. Consequently the
base texture keeps its nearest magnification and trilinear/anisotropic minification,
while the independent mask uses nearest magnification and linear minification.
Temporal history is a third, separate target allocated only when temporal accumulation
is actually enabled; it is not reused as dynamic-shadow scratch storage.

Explicitly reflective, camera-facing faces remain full-resolved at their world texel
centres each time the camera, emitter, scene, paint, or player signature changes. They
follow Trivox's deterministic camera path: up to three specular bounces, then the
ordinary direct-plus-RC diffuse resolve at the first non-mirror hit. They do not run a
hashed diffuse walk when jitter is disabled, use temporal history, enter the staggered
diffuse budget, or reuse partially updated coarse mips that could preserve stale
highlights.

Lighting work decays with continuous world distance without changing the authored
world-texel grid. The shared cascade remains a live every-frame pass. Projected texel
density selects L0-L3 for animated diffuse receivers, while nearby regions touched by
moving dynamic meshes are refreshed
immediately. Across the transition to the lighting radius, the presented contribution
blends continuously toward ambient and new far face targets are allocated/resolved
through a bounded queue. Re-anchoring the finite trace window preserves its overlapping
world-space probe phase and face caches; texels at the finite outer coverage edge may
still enter the normal fade. Outside the radius the normal ambient/fog path remains.
Indirect samples also fade at the shared probe volume boundary rather than clamping
out-of-volume surfaces to its outer probes. Probe density, cascade iterations,
direct-shadow and indirect sample counts, temporal history, lighting radius, jitter,
and corner-path merging are local demo quality settings; changing them invalidates
only derived GPU caches. Temporal accumulation, jitter, and corner merging default
off, matching Trivox's realtime path.
Lighting scene collection also walks a fixed coordinate neighbourhood through the
chunk hash, so signature, emitter, and surface-topology work depends on the lighting
radius rather than the much larger visual render radius.

The exhaustive lighting regressions also contain a renderer-independent deterministic
CPU path tracer. It follows complete scene paths for fixed world-surface texels and
compares their converged display values with the RC-resolved textures. Playground
samples cover varied sky/emitter exposure; cave samples cover direct emission,
occluded/indirect transport, ceiling diffuse transport, and a camera-aligned mirror
path. Separate route tests cross axes, corners, and chunk seams in different
subdivisions and headings, then require exact final images, visible surface state,
sampler state, and hard cache-state consistency against fresh-session references. A
partial cache may legitimately retain different invalid offscreen backing after a
different route; when the presented frame is exact, full-texture hash differences for
that backing are reported as transient diagnostics rather than rendering failures.
Active shadow-mask signatures and complete mask hashes must still match exactly; the
final exhaustive run reported zero rendered changes and zero hard or visible mask
mismatches. A continuous-motion matrix additionally crosses
the origin and both adjacent 16 m seams forward and backward, turns in place, walks
the quality-fade boundary, and repeats at million-chunk coordinates while measuring
fixed world-texel and whole-face deltas on every frame.
The final cave RC-to-independent-path-tracer comparison had a maximum absolute error
of `0.0185`.

For interactive comparisons, `P` switches to a deterministic full-frame GPU path
tracer at the current native render resolution. Its static BVH covers the complete
visual radius and is cached until the anchor chunk, streamed topology, or radius
changes; moving emissive meshes and ordinary player cylinders remain separate small
buffers updated every frame. Four centred primary samples trace five bounces with
next-event sampling of every emissive mesh and the infinite sky. The primary hit
uses the exact unlit raster texture/paint result, secondary hits use authored planar
texture coordinates, and no temporal accumulation is involved. This renderer owns
separate buffers and leaves the normal close-radius cascade and face caches intact.
Sprites and crisp engine edges are composited afterward through the preserved depth
buffer; they are not currently participants in the path integral.

The command-line performance regressions are:

- `ol_tests --hills-render-benchmark`: streamed hills boundaries, edge paths, and
  lighting submissions at several visual radii. On the reference machine, automatic
  stable-emitter target-surface preparation produced a 7.323 ms moving-seam average,
  9.752 ms p95, and 12.242 ms maximum. The steady radius-20 lighting case took
  3.082 ms total, including 0.270 ms of lighting overhead. These figures characterize
  that machine/build rather than setting a universal FPS guarantee.
- `ol_tests --cave-render-benchmark`: three animated-emitter views compared with a
  conservative legacy invalidation baseline, including submitted diffuse texels,
  static rebuilds, common-state cache reuse, and isolated bright reflection
  components. `--cave-render-benchmark-hd` runs the same workload at 1920x1080. The
  final reference-machine dynamic-path measurements were 7.514 ms average/13.598 ms
  p95/14.060 ms maximum versus 20.288 ms legacy at 960x540, and 9.019 ms
  average/17.084 ms p95/17.943 ms maximum versus 20.208 ms legacy at 1920x1080.
  Common state averaged 4.00 updates, 38.92 cache hits, and 5.00 SSBO binds per frame
  in that run; these are comparative measurements, not a cross-machine guarantee.
- `ol_tests --cave-motion-benchmark`: a fixed-step 960x540 walking route beginning
  at the reported south-wall pose, followed by a legal z=16 seam crossing and
  return. It times 180 synchronized stationary frames, every named motion phase,
  moving non-crossing frames, anchor transitions, staged surface work, and
  synchronous fallback. The A/B labels are explicit: `production` leaves the forced
  surface-prewarm switch at its false default, while `experimental-prewarm` opts into
  face resolves despite the cave's changing emitter signature. Thus automatic
  stable-emitter prewarm remains zero in both cave runs and only the forced mode
  stages face texels. Output includes moving/stationary deltas and ratios,
  counts above the 8.33, 16.67, and 33.30 ms frame budgets, actual target resolution,
  and disabled render flags. Its `shadow` counters report traced and cleared R8-mask
  texels plus the eliminated counterfactual full-face RGBA copy, while `draw` reports
  candidate, origin-gated, and unresolved faces. `--cave-motion-benchmark-hd`
  exercises the same code, route, and assertions at 1920x1080. Both modes keep
  stationary and moving buckets separate so a high idle FPS cannot hide walking
  hitches, and walking is the primary acceptance bucket. Production walking measured
  12.438 ms average/13.930 ms p95/18.968 ms p99/19.386 ms maximum at 960x540 and
  14.954/16.966/19.019/22.806 ms respectively at 1920x1080; the HD stationary p99
  was 17.031 ms. Forced animated-emitter prewarm was slower and remains default-off.

Material layers can override color and independently add colored emission strength
or reflectivity. Emission and reflectivity are resolved per mesh, so materially
different sides are authored as separate meshes in the current whole-mesh material
model. Probe fields and light textures are derived state: multiplayer shares the
procedural meshes/materials and paint operations, not the caches.

## Lighting Quality Controls

The pause menu exposes the useful Trivox quality controls described above. Values are
discrete so shader workload stays predictable. They are saved in `ol_state.txt` as
local rendering preferences and old profiles simply use the current defaults when
those keys are absent. Screen render scale remains an independent display setting.

## Profile Persistence

Gameplay state is recorded incrementally. Player records are dirtied only when a
value changes, and the potentially large painted-pixel vector is copied only when
the dimension's paint revision advances. Periodic saves enqueue an immutable profile
snapshot to a dedicated writer thread, so filesystem latency cannot stall a rendered
frame. Session/world transitions and shutdown drain that queue and perform a final
synchronous save before the relevant state is destroyed.

## Physics Broadphase

The first implementation uses a CPU spatial hash per active dimension. Point masses
are inserted by cell, then collisions query neighboring cells instead of doing a full
O(n^2) pass. This keeps the first server-authoritative physics path deterministic and
easy to debug.

Player ground support and obstacle clearance are evaluated at the actual contact
point on the supporting surface. This keeps a broad floor valid when the player's
cylinder touches furniture or a wall, while still rejecting a smaller step whose
contact area is directly covered by another collider.

Compute shader physics is viable later for particle-like batches or purely visual
secondary simulation. It is not the right first authoritative physics path because
constraints, sleeping/islands, broadphase mutation, and cross-GPU determinism make
network reconciliation harder.

## Multiplayer Physics Sync

The design is authoritative server with fixed ticks.

- Clients send compact input commands: tick, sequence, movement bits, yaw/pitch.
- The host/server owns the world, runs fixed physics, and persists sessions.
- The server sends snapshots: tick, player body transforms, and changed entity state.
- Clients predict their local player, keep unacknowledged inputs, and replay them
  after authoritative corrections.
- Remote players are interpolated from buffered snapshots.

This avoids lockstep determinism requirements for all clients. Deterministic math is
still useful for good prediction, but the server remains the source of truth.

## Steamworks Path

`engine/net.cpp` has a null/offline implementation by default and a Steamworks path
behind `OL_USE_STEAMWORKS`. The current Steam path initializes Steam, creates/joins
lobbies, tracks lobby ownership, and defines packet types for the authoritative
snapshot/input model. The packet application is intentionally thin until the gameplay
state that must persist is less volatile.
