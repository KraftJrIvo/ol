# Incomplete Prompt Backlog

These are the aspects from the initial prompt that are present only as scaffolding,
partial behavior, or first-pass demo code.

## Rendering

- Real skyboxes per dimension. Current code has gradient sky colors only.
- Proper fog volume. Current fog is per-object/per-chunk color mixing, not depth fog.
- Sprite participation in radiance-cascade lighting. Mesh lighting is evaluated at
  world-surface texel resolution; sprites remain flat texture billboards.
- Imported texture and normal-map assets. Procedural nearest-neighbor world textures,
  meter-scaled planar UVs, sprites, and shared painted texels are implemented; a
  general asset table and normal-map shader path are still pending.
- Custom vertex/fragment shaders per mesh/sprite. Fields exist, but shader loading
  and binding are not implemented.
- Material stacking beyond color, emission, and reflectivity resolution. Stack
  texture/shader/normal overrides are not yet resolved.
- A chunk/TLAS lighting accelerator and general BVH refit path. Exact transformed
  non-emissive triangles currently use a cached static BVH, players use a small
  per-frame dynamic BVH, and moving emissive meshes use a compact non-occluding
  triangle/CDF buffer without rebuilding the static region. The static scene still
  has a guarded triangle cap.
- Batched surface-lighting storage and dispatch. The renderer currently caches common
  uniforms/SSBO bindings, keeps stable filtered color separate from a persistent R8
  dynamic-shadow mask, and repairs carried anchor textures over visible L0 regions,
  but each face is still an individual framebuffer target. A lightmap atlas or
  texture array with tiled compute dispatch is future work. Production currently
  prewarms exact target-anchor surfaces only during movement and only after emitter
  signatures stay stable for two frames, bounded to 64K texels/eight allocations per
  frame. The forced-prewarm switch remains false by default; it measured worse with
  animated cave emitters and exists only to exercise that A/B path. Cave's changing
  emitters do not trigger automatic surface preparation.
- A compact spherical-harmonic irradiance/probe representation. The current RC path
  still stores directional radiance/occlusion in its atlas; SH irradiance has only
  been identified as a possible future bandwidth and batching improvement.
- Normal RC reflected and indirect ray hits currently use the hit mesh's resolved
  flat color. The optional full-frame reference carries authored planar UVs in its
  separate triangles, but painted pixels reached by a secondary ray still need a
  GPU paint-atlas indirection.
- Real LOD generation/selection. A mesh can reference one LOD mesh, but there is no
  tool/import pipeline, streaming policy, or per-material LOD behavior.
- Custom shader sprite paths are not complete.
- Model/mesh import pipeline. Demo meshes are procedural boxes/wedges only.

## World And Editing

- Runtime creation/deletion of dimensions, chunks, meshes, sprites, materials, and
  colliders by players. Data layout anticipates this, but there is no editor protocol
  or UI.
- General-purpose chunk streaming for arbitrary edited worlds. The procedural hills
  demo has velocity-directed prefetch and bounded incremental load/unload commits,
  but other dimensions still generate eagerly.
- Cross-chunk object bounds. Objects are assigned by origin chunk only.
- Dimension persistence. Session naming exists in UI, but saving/loading world state
  is not implemented.
- Multiple active dimensions with players in different dimensions. The data model
  supports it; the demo uses one dimension.

## Physics

- Full capsule/cylinder link collision. Link collision currently samples a few
  spheres along the link, which is a practical approximation.
- Oriented box collision. Box records have rotation fields, but current support is
  minimal and not validated.
- Box linking by corners/center. Not implemented.
- Collision groups are present but need gameplay-level group definitions and tests.
- Sleeping/islands and stable broadphase updates for large worlds. Current CPU hash
  is simple and rebuilt every solver pass.
- Compute shader physics. Deliberately deferred for authoritative simulation.
- Better first-person character controller tuning. Basic walking/sprinting/crouching
  works, but slopes, steps, head clearance, and camera smoothing need passes.

## Multiplayer

- Authoritative server tick loop. The design is documented, but current network code
  is mostly a lobby/packet shell.
- Client input prediction and replay.
- Server snapshots and interpolation for remote players.
- Physics reconciliation after correction.
- Session persistence across host disconnect/reconnect.
- Steam lobby callbacks and member tracking integrated with the new authoritative
  model.
- Networked interactive editing operations and conflict/permission rules.

## Memory And Assets

- One explicit reserved memory block with sub-arenas. Current arenas are fixed
  arrays inside engine structs, not a single handmade-style memory layout.
- Asset table for textures, normal maps, shaders, materials, and fonts.
- Hot reload/dev reload of assets.
- Bounds checks/reporting when arenas fill.

## Demo

- Host/connect flow is a UI shell unless built out with Steamworks callbacks and
  authoritative packets.
- Player name/color are not persisted.
- Other players are not yet shown from network snapshots.
- Procedural playground is minimal and mostly box-based.
