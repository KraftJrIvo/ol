# OL

`OL` is a small C++/raylib 3D engine prototype with a data-oriented layout:
named dimensions, chunk-relative coordinates, fixed-step physics, simple chunked
rendering, and a demo app that can later host or join Steam lobbies from the same
executable.

Mesh lighting uses exact-triangle static/dynamic BVH path tracing, a compact moving
emitter buffer, an infinite emissive sky, and every-frame world-anchored radiance
cascades. Diffuse shadows, painted albedo, and immediate reflections live in face
textures whose level-zero grid uses the world's `pixels_per_meter`; close shadows are
traced per L0 world texel, while animated distant diffuse updates use coarser
world-texture mip levels instead of screen-space shading.
Player-shadow motion updates a persistent R8 multiplier mask over the stable filtered
color lightmap, so it neither copies a full RGBA face nor borrows the optional temporal
history target. At chunk-anchor changes, carried faces repair only current visible
coverage, but do so at canonical L0 before regenerating filtered mips.
Emissive meshes remain visible to camera and mirror paths but do not act as shadow
blockers. The `cave` demo map contains flying colored emitters, shadow geometry,
mirrors, and polished surfaces.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

If raylib is not installed, CMake first tries the local reference copy under
`ref/plato/out/build/x64-Debug/_deps/raylib-src`, then falls back to downloading
raylib 5.5. You can also pass a local copy explicitly:

```powershell
cmake -S . -B build -DOL_RAYLIB_SOURCE_DIR=C:\path\to\raylib
```

Steamworks is optional:

```powershell
cmake -S . -B build -DOL_USE_STEAMWORKS=ON -DSTEAMWORKS_SDK_DIR=C:\sdk\steamworks
```

## Demo Controls

- First menu: edit the player and session fields directly
- First menu: click the color circle for RGB sliders
- First menu: click the session arrow for previous sessions
- First menu: hold a session row `X` for 3 seconds to delete that saved session
- First menu: use `host` or `join from CB`
- Player name, player color, pause settings, last session, and per-session player poses are saved in `ol_state.txt`
- First menu: `Esc` exits the app
- `WASD`: move
- `Mouse`: look
- `Shift`: sprint
- `Ctrl`: crouch
- `Space`: jump
- `P`: toggle the deterministic full-frame path-traced comparison (`4 spp`,
  `5` bounces, no temporal accumulation)
- `+` / `-`: change render scale
- `Esc`: pause
- Pause menu: adjust display settings and radiance-cascade quality, toggle lighting,
  jitter, and corner merging, continue, or leave to the first menu

## Tests

```powershell
cmake --build build --config Debug --target ol_tests
ctest --test-dir build -C Debug --output-on-failure
```

Individual tests:

```powershell
build\ol_tests.exe --physics
build\ol_tests.exe --visual
build\ol_tests.exe --menu
build\ol_tests.exe --lighting-roundtrip
build\ol_tests.exe --lighting-route-regression
build\ol_tests.exe --lighting-user-route-regression
build\ol_tests.exe --lighting-cave-pathtrace
build\ol_tests.exe --lighting-cave-seam
build\ol_tests.exe --lighting-continuous-motion
build\ol_tests.exe --hills-render-benchmark
build\ol_tests.exe --cave-render-benchmark
build\ol_tests.exe --cave-render-benchmark-hd
build\ol_tests.exe --cave-stationary-regression
build\ol_tests.exe --cave-motion-benchmark
build\ol_tests.exe --cave-motion-benchmark-hd
```

With Visual Studio generators the executable may be under `build\Debug\`.

The visual smoke tests use a hidden raylib window and write scene, cave-lighting,
menu, and pause captures under `build\test-output\`. The exhaustive lighting tests
compare fixed playground and cave surface texels with an independent deterministic
CPU path tracer; the route matrix separately checks travel and seam-crossing
invariance. Route tests require exact rendered output and visible cache state; invalid
offscreen backing in a partially resolved face is diagnostic rather than a required
full-texture hash match, while an active shadow mask must match exactly. The final
exhaustive run reported zero rendered changes and zero hard or visible shadow-mask
mismatches. The continuous-motion matrix samples fixed world texels while crossing
every nearby anchor seam in both directions, including at million-chunk coordinates.
The cave RC-to-independent-path-tracer comparison's maximum absolute error was
`0.0185`.
The hills benchmark covers streaming seams, edge paths, and lighting work at multiple
render radii. On the reference machine, automatic stable-emitter target-surface
prewarm produced a 7.323 ms moving average, 9.752 ms p95, and 12.242 ms maximum.
The steady radius-20 case took 3.082 ms total with 0.270 ms of lighting overhead;
these timings are measurements, not hardware-neutral guarantees. The cave benchmark
compares animated-emitter handling with conservative legacy invalidation while
checking bounded diffuse work, batched common GPU state, and reflective fireflies.
Its `-hd` variant runs the same workload at 1920x1080. The
final dynamic-path results were 7.514 ms average/13.598 ms p95/14.060 ms maximum
versus 20.288 ms legacy at 960x540, and 9.019 ms average/17.084 ms p95/17.943 ms
maximum versus 20.208 ms legacy at 1920x1080. Common radiance state averaged 4.00
updates, 38.92 cache hits, and 5.00 SSBO binds per frame in that run.
The stationary cave regression holds the supplied near-seam camera pose while its
emitters animate, verifying that neighbor scenes and cascades become ready without
submitting surface-prewarm texels or rebuilding the static scene. The changing
emitter signature keeps automatic target-surface prewarm disabled, while the
stationary-camera policy independently pauses any surface submissions.
The cave motion benchmark starts from that same reported pose, uses the real
fixed-step player controller to walk into and away from the south wall, then crosses
the legal interior z=16 chunk boundary in both directions. It reports synchronized
frame percentiles for a 180-frame stationary sample and every walking phase, worst
crossing and non-crossing frames, moving/stationary deltas, frame-budget overrun
counts, lighting work, staged neighbor work, promotions, and synchronous fallback.
The standard 960x540 run compares production, whose forced-prewarm flag defaults off,
with an explicitly labelled experimental mode that forces face prewarm despite the
cave's animated emitters; `--cave-motion-benchmark-hd` runs the identical route and
assertions at 1920x1080. Each result prints its actual
render-target resolution and disabled render features so comparisons remain honest,
and keeps stationary and walking percentiles separate because the measured hitches
are motion-dependent. Production always prepares neighbor scene/BVH/cascade state in
the background. During movement it additionally prewarms exact target-anchor diffuse
faces only after the emitter signature has stayed unchanged for at least two frames,
up to 64K texels and eight allocations per frame. Cave emitters change every frame,
so that automatic path submits zero face texels there; the forced benchmark path uses
the smaller 16K/one-allocation budget. Common radiance uniforms and scene SSBO
bindings are cached across per-face submissions.
Walking is the motion benchmark's primary acceptance bucket. On the reference machine,
production walking measured 12.438 ms average/13.930 ms p95/18.968 ms p99/19.386 ms
maximum at 960x540 and 14.954/16.966/19.019/22.806 ms respectively at 1920x1080;
the HD stationary p99 was 17.031 ms. Forced animated-emitter face prewarm measured
worse and therefore remains an explicitly experimental, default-off comparison.
The interactive `P` reference owns a separately cached BVH covering the complete
visual render radius. It traces authored mesh textures, painted primary pixels,
emissive meshes, the sky, reflections, and ordinary player geometry without
mutating or warming the normal radiance-cascade caches.

See [docs/architecture.md](docs/architecture.md) for the initial design decisions,
especially the chunk-relative coordinate, edge rendering, physics broadphase, and
network synchronization plans.

See [docs/incomplete.md](docs/incomplete.md) for the prompt backlog broken into
one-at-a-time implementation items.
