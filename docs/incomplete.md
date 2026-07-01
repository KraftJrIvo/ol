# Incomplete Prompt Backlog

These are the aspects from the initial prompt that are present only as scaffolding,
partial behavior, or first-pass demo code.

## Rendering

- Real skyboxes per dimension. Current code has gradient sky colors only.
- Proper fog volume. Current fog is per-object/per-chunk color mixing, not depth fog.
- Real ambient+diffuse shader path for meshes and sprites. Current mesh lighting is
  CPU face shading; sprites are flat billboards.
- Texture and normal-map rendering. IDs exist in data structs, but there is no asset
  table or shader sampling path yet.
- Custom vertex/fragment shaders per mesh/sprite. Fields exist, but shader loading
  and binding are not implemented.
- Material stacking beyond color override. Stack data exists, but only color is
  resolved.
- Correct light gathering from nearby chunks by light radius. Current renderer scans
  all lights in the dimension.
- Real LOD generation/selection. A mesh can reference one LOD mesh, but there is no
  tool/import pipeline, streaming policy, or per-material LOD behavior.
- General contour extraction for composed objects. Edge rendering is now
  visibility-cut and drawn in screen space, but the playground's multi-box contours
  are hand-authored rather than derived automatically from arbitrary adjacent boxes.
- Sprite billboard modes beyond the current raylib billboard calls. No-billboard and
  custom shader sprite paths are not complete.
- Model/mesh import pipeline. Demo meshes are procedural boxes/wedges only.

## World And Editing

- Runtime creation/deletion of dimensions, chunks, meshes, sprites, lights, and
  colliders by players. Data layout anticipates this, but there is no editor protocol
  or UI.
- Chunk streaming/unloading. Chunks are fixed arena records and generated eagerly in
  the demo.
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
