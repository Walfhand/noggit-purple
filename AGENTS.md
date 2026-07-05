# Noggit Purple Agent Notes

## Rust Rewrite

The Rust rewrite lives under `noggit-rs/`. Treat it as a product rewrite, not a
line-by-line C++ translation. The C++ code is the behavioral reference and
format reference until Rust tests supersede a feature.

### Current Crate Layout

- `noggit-vfs`: local files plus WoW client MPQ access.
- `noggit-stormlib`: minimal safe Rust wrapper around StormLib for MPQ reads.
- `noggit-formats`: binary format parsing and serialization.
- `noggit-core`: editor domain model and mutation logic.
- `noggit-render`: renderer-facing mesh preparation.
- `noggit-ui`: desktop `wgpu` preview UI.
- `noggit-scripting`: future Lua API.
- `noggit-cli`: validation/debug commands.

### TDD Rule

New format or core behavior must start with a failing test. Prefer golden,
roundtrip, or fixture-driven tests over ad hoc assertions. Do not wire a parser
into `noggit-core`, `noggit-render`, or `noggit-ui` until it has tests in
`noggit-formats`.

### Validation Commands

Run these from `noggit-rs/` after Rust changes:

```bash
cargo fmt --check
cargo test
cargo clippy --workspace --all-targets -- -D warnings
```

The Rust MPQ backend links StormLib. On this machine the build script finds
Homebrew's `/home/linuxbrew/.linuxbrew/lib/libstorm.a`; set `STORMLIB_LIB_DIR`
when StormLib is installed elsewhere.

### Safety Rules

- `unsafe_code` is denied at workspace level.
- The only exception is `noggit-stormlib`, where FFI `unsafe` is isolated and
  wrapped behind a small safe read-only API.
- Keep parsers bounds-checked and endian-explicit.
- Preserve byte-for-byte roundtrip where a serializer exists.
- Use generated in-memory fixtures until stable real WoW sample files are added.
- ADT chunk ids are normalized to readable FourCCs (`MVER`, `MCNK`, etc.) even
  when source files store the little-endian byte order (`REVM`, `KNCM`, etc.).
- `target/` must stay ignored.

### Current Rust Milestone

Implemented:

- Local-folder VFS primitives with normalized virtual paths.
- WoW client MPQ discovery and targeted reads through StormLib, matching the
  C++ Noggit backend for WotLK MPQs including large patch archives.
- DBC header/record/string parsing.
- DBC byte-identical roundtrip.
- Typed `LiquidType.dbc` metadata extraction for liquid render assets.
- Raw ADT chunk container parsing.
- Raw WDT chunk container parsing plus `MPHD` map flags.
- ADT `MVER` version extraction.
- Typed ADT `MHDR`, `MCIN`, and `MCNK` header parsing.
- Typed `MCNK` terrain access by header offsets.
- ADT `MCVT` height parsing.
- ADT `MCNR` normal parsing in Noggit render axis order.
- ADT `MCLY` texture layer parsing.
- ADT `MCAL` alpha-map decoding for big, compressed, and legacy 4-bit maps.
- ADT asset filename blocks: `MTEX`, `MMDX`, `MWMO`.
- ADT asset filename offset tables: `MMID`, `MWID`.
- ADT doodad and WMO placement tables: `MDDF`, `MODF`.
- ADT `MH2O` liquid parsing, including chunk headers, optional attributes,
  liquid layer metadata, masks, height maps, depths, and basic UV formats.
- M2 root parsing for WotLK `MD20` headers, static vertices, direct texture
  definitions, and texture lookup tables.
- M2 skin parsing for `SKIN` index lookup tables, triangle buffers, submeshes,
  and texture units.
- WMO root parsing for `MOHD`, `MOTX`, `MOMT`, `MOGN`, and `MOGI`.
- WMO group parsing for `MOGP` child mesh chunks: `MOPY`, `MOVI`, `MOVT`,
  `MONR`, `MOTV`, and `MOBA`.
- BLP header parsing and RGBA decoding for paletted textures plus DXT1, DXT3,
  and DXT5 mipmaps.
- Core world-map model that loads local map directories into sorted ADT tiles,
  terrain chunks, liquid layers, assets, placements, and WDT map-level
  alpha-map flags.
- Renderer terrain mesh extraction from loaded ADT `MCVT` chunks, including
  up to four terrain texture material ids from `MCLY`, decoded `MCAL` alpha
  maps, and chunk detail-map UVs.
- Renderer debug marker extraction for loaded `MDDF` M2 doodad placements and
  `MODF` WMO placements. ADT placement coordinates are converted from global
  tile/world coordinates into the same map-local render space as terrain.
- Renderer WMO mesh extraction from loaded WMO root/group assets. This first
  WMO pass applies ADT placement position/rotation/scale on the CPU, emits
  static triangles, maps primary `MOMT` textures through `MOTX`, and leaves WMO
  liquids, fog, internal doodads, advanced material shaders, and selection for
  later slices.
- Renderer M2 mesh extraction from loaded M2 root plus `00.skin` assets. This
  first M2 pass resolves skin triangle indices through the model vertex lookup,
  applies ADT `MDDF` placement position/rotation/scale on the CPU, maps direct
  diffuse texture filenames through the M2 texture lookup table, and leaves M2
  animation, particles, billboards, replacement textures, and advanced material
  behavior for later slices.
- Renderer water mesh extraction from loaded ADT `MH2O` liquid layers. The
  first water pass emits visible 8x8 liquid tile quads with decoded heights,
  depths, UVs, and map-local placement.
- Desktop terrain preview window through `noggit-ui`, using `winit`/`wgpu` for
  filled terrain triangles, depth buffering, camera controls, repeated BLP
  texture sampling, generated/decoded mipmaps, and GPU blending of terrain
  `MCLY` layers through `MCAL` alpha maps.
- `noggit-ui` loads WMO root/group files and their BLP textures from the same
  WoW client/extra MPQ chain as terrain, then draws the current WMO placements
  as static textured geometry. Press `M` to toggle real WMO meshes.
- `noggit-ui` loads M2 root files, their `00.skin` files, and direct BLP
  textures from the same WoW client/extra MPQ chain as terrain, then draws
  current `MDDF` doodad placements as static textured geometry. Press `N` to
  toggle real M2 meshes.
- `noggit-ui` draws `MH2O` liquid surfaces through a separate transparent GPU
  pass. With `--client`, it reads `DBFilesClient/LiquidType.dbc`, resolves the
  referenced animated liquid BLP frame sequences, uploads them as `wgpu`
  texture arrays, and samples the active frame in the water shader. Press `L`
  to toggle water. This still leaves reflection/fog parity, WMO liquids, and
  legacy `MCLQ` water for later slices.
- Camera movement in `noggit-ui` is a free-fly camera: hold left mouse to look,
  `WASD` moves relative to the camera, `Q` or control moves down, `E` or space
  moves up, shift accelerates, and mouse wheel adjusts movement speed.
- `noggit-ui` draws a debug object-placement overlay as colored wire boxes:
  cyan for M2 doodads and orange for WMO placements. The overlay is visible by
  default and can be toggled with `O`; this is the first small UI action state
  slice, not the final editor GUI.
- `noggit-cli` commands: `inspect-dbc`, `inspect-adt`, `inspect-map`,
  `inspect-client`, `inspect-blp`, `check-map-assets`, `check-map-textures`.
- `inspect-adt` summary for versions, asset tables, placements, terrain chunks,
  placement asset usage, `MH2O` liquid counts, heights, normals, texture
  layers, and raw `MCAL` payload sizes.
- `inspect-map` summary for full local map directories, validated against
  `/home/walfhand/Documents/wow-maps/guerilla`.
- `inspect-client` and `check-map-assets` have been validated against
  `/home/walfhand/Documents/Ultimate WotLK`; the standalone Guerilla patch at
  `/home/walfhand/Documents/wow-maps/patch-guerilla.MPQ` should be passed as an
  extra archive when checking that custom map files are resolvable. With the
  StormLib backend, the Guerilla asset check currently resolves all 85 referenced
  assets.
- `cargo run -p noggit-ui -- /home/walfhand/Documents/wow-maps/guerilla --client /home/walfhand/Documents/Ultimate\ WotLK --extra-mpq /home/walfhand/Documents/wow-maps/patch-guerilla.MPQ`
  opens the current terrain preview and blends up to four terrain BLP layers
  from the WoW client/extra MPQ using decoded `MCAL` alpha maps. It also loads
  and renders all current Guerilla M2 doodad placements as static textured
  geometry (`M2 assets loaded: 71/71`, `M2 textures loaded: 111/111`,
  `M2 mesh: placements=2246 ... triangles=314589` in the last smoke run) and
  the 6 WMO placements as static textured geometry (`WMO assets loaded: 2/2`,
  `WMO textures loaded: 16/16`) and current `MH2O` water with liquid textures
  from `LiquidType.dbc` (`water textures loaded: 5/5 frames=150` in the last
  smoke run). Press `N` to toggle M2 meshes, `M` to toggle WMO meshes, `L` to
  toggle water, and `O` to toggle debug placement boxes. The preview still does
  not render M2 animation/particles/advanced material behavior, water
  reflection/fog parity, WMO liquids, WMO internal doodads, sky, or edit tools.
- `cargo run -p noggit-cli -- check-map-textures /home/walfhand/Documents/wow-maps/guerilla /home/walfhand/Documents/Ultimate\ WotLK /home/walfhand/Documents/wow-maps/patch-guerilla.MPQ`
  currently decodes all 12 terrain textures referenced by Guerilla.

Next:

- M2 animation, particles, billboards, replacement textures, geoset/material
  parity, and selection.
- WMO material parity, liquids, fog, doodad sets, internal doodads, and
  selection.
- Water reflection/fog parity, WMO liquids, legacy `MCLQ` liquid support, and
  water editing/save paths.
- Terrain editing mutations and byte-preserving save paths.
- Real golden samples once stable fixtures are available.
