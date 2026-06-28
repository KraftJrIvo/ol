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

Chunks hold references to global per-dimension arenas for meshes, sprites, and
lights. This makes add/remove operations straightforward: edit the global record,
then update the owning chunk reference list. Objects are assigned to the chunk that
contains their origin for now; broad bounds crossing multiple chunks should be added
when streaming/editing grows.

## Mesh Edges

The current renderer draws mesh faces in 3D and draws edges with `DrawLine3D`. That
is acceptable for the first demo, but it is not the final pixel-perfect contour
path.

The intended edge pipeline is:

1. Render normal geometry into color and a readable depth texture.
2. Project each mesh edge into screen space.
3. Draw screen-space line strips in a shader.
4. Compare each line fragment against the depth texture and discard fragments behind
   closer geometry, with a small bias to prevent self-fighting.

This avoids CPU depth readback and gives stable pixel-width contours.

## Physics Broadphase

The first implementation uses a CPU spatial hash per active dimension. Point masses
are inserted by cell, then collisions query neighboring cells instead of doing a full
O(n^2) pass. This keeps the first server-authoritative physics path deterministic and
easy to debug.

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
