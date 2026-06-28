# OL

`OL` is a small C++/raylib 3D engine prototype with a data-oriented layout:
named dimensions, chunk-relative coordinates, fixed-step physics, simple chunked
rendering, and a demo app that can later host or join Steam lobbies from the same
executable.

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

- `H`: host/offline-start the generated playground session
- `J`: join lobby from clipboard when built with Steamworks
- `WASD`: move
- `Mouse`: look
- `Shift`: sprint
- `Ctrl`: crouch
- `Space`: jump
- `+` / `-`: change render scale
- `Esc`: release/capture mouse

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
```

With Visual Studio generators the executable may be under `build\Debug\`.

The visual smoke test uses a hidden raylib window and writes:
`build\test-output\visual-smoke.png` and `build\test-output\menu-smoke.png`.

See [docs/architecture.md](docs/architecture.md) for the initial design decisions,
especially the chunk-relative coordinate, edge rendering, physics broadphase, and
network synchronization plans.

See [docs/incomplete.md](docs/incomplete.md) for the prompt backlog broken into
one-at-a-time implementation items.
