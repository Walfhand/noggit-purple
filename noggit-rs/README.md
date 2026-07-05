# Noggit Rust Rewrite

This workspace is the Rust rewrite target for Noggit.

The rewrite is test-driven: format parsers and serializers must be covered by
golden or roundtrip tests before they become dependencies of the editor core,
renderer, or UI.

## Crates

- `noggit-vfs`: local/client/archive file access.
- `noggit-stormlib`: safe read-only Rust wrapper around StormLib for WotLK MPQs.
- `noggit-formats`: WoW binary formats such as DBC, ADT, WDT, WMO, M2, and BLP.
- `noggit-core`: editor domain model and mutation logic.
- `noggit-render`: renderer-facing mesh preparation.
- `noggit-ui`: desktop `wgpu` UI.
- `noggit-scripting`: Lua scripting API.
- `noggit-cli`: validation and debugging tools.

## First Vertical Slice

1. Parse DBC files and roundtrip them byte-for-byte.
2. Parse WDT map flags plus ADT chunk containers, typed headers, terrain
   heights, normals, texture layers, alpha maps, asset filename blocks,
   filename offset tables, placement tables, and `MH2O` liquid layers.
3. Load a local map directory through `noggit-vfs` into a `noggit-core`
   `WorldMap` with sorted tiles, terrain chunks, liquid layers, assets, and
   placements.
4. Discover and read WoW client MPQ archives for referenced map assets using
   the same StormLib backend as Noggit C++.
5. Add golden fixtures from known-good map files as they become available.

## Smoke Tests

The Rust MPQ backend requires StormLib. If it is not installed in a standard
library path, set `STORMLIB_LIB_DIR=/path/to/stormlib/lib` before building.

```bash
cargo run -p noggit-cli -- inspect-dbc /path/to/file.dbc
cargo run -p noggit-cli -- inspect-blp /path/to/file.blp
cargo run -p noggit-cli -- inspect-adt /path/to/tile.adt
cargo run -p noggit-cli -- inspect-map /path/to/World/Maps/guerilla
cargo run -p noggit-cli -- inspect-client /path/to/WoWClient [/path/to/extra.MPQ ...]
cargo run -p noggit-cli -- check-map-assets /path/to/guerilla /path/to/WoWClient [/path/to/extra.MPQ ...]
cargo run -p noggit-cli -- check-map-textures /path/to/guerilla /path/to/WoWClient [/path/to/extra.MPQ ...]
cargo run -p noggit-ui -- /path/to/guerilla
cargo run -p noggit-ui -- /path/to/guerilla --client /path/to/WoWClient [--extra-mpq /path/to/extra.MPQ ...]
```

`inspect-adt` prints chunk counts, version, asset tables, placement asset usage,
`MH2O` liquid counts, terrain chunk summaries, height ranges, normal counts,
texture layer counts, and raw `MCAL` payload sizes.

`inspect-map` loads every matching ADT in a local map directory and prints the
map-level tile, placement, terrain chunk, and liquid layer totals. The core
also reads the local `<map>.wdt` when present so terrain alpha-map decoding
follows the same map-level big-alpha flag as Noggit C++.

`inspect-client` discovers WotLK client MPQs in Noggit load order and reports
loaded/skipped/failed archives. By default no archive is skipped by size;
StormLib is used so large patch archives such as `Patch-N.MPQ` can be opened
the same way as in Noggit C++. `check-map-assets` verifies that the texture,
M2, and WMO assets referenced by a loaded map can be read from the client MPQ
chain.

`inspect-blp` and `check-map-textures` validate BLP texture decoding. The
current decoder supports paletted BLPs plus DXT1, DXT3, and DXT5 mipmaps.

`noggit-ui` opens the first graphical preview: a GPU-rendered terrain view with
filled ADT triangles, depth buffering, and camera controls. With `--client`, it
reads BLP assets from the WoW client/extra MPQs, uploads decoded/generated
mipmaps, and blends up to four `MCLY` terrain texture layers through decoded
`MCAL` alpha maps in the terrain shader. It also loads WMO root/group files
from the client, uploads their primary BLP textures, and draws placed WMOs as
static textured geometry. It loads M2 doodad root files plus their `00.skin`
files, resolves direct diffuse textures, and draws placed M2s as static
textured geometry. It also draws `MH2O` liquid surfaces with a transparent
procedural animated water pass.

The UI also draws a debug placement overlay for loaded objects: M2 doodads are
cyan wire boxes and WMO placements are orange wire boxes. Press `O` to toggle
that overlay. Press `M` to toggle the real WMO meshes and `N` to toggle the
real M2 meshes. Press `L` to toggle water. Camera movement is a free-fly
camera: hold left mouse to look, `WASD` to move relative to the camera,
`Q`/control to move down, `E`/space to move up, shift to accelerate, and mouse
wheel to adjust movement speed. M2 animation, particles, advanced
geoset/material behavior, LiquidType texture parity, WMO liquids/internal
doodads, sky, and edit tools are still next.
