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
- `noggit-render`: future `wgpu` renderer.
- `noggit-ui`: future desktop UI.
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
- Raw ADT chunk container parsing.
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
- Core world-map model that loads local map directories into sorted ADT tiles,
  terrain chunks, assets, and placements.
- `noggit-cli` commands: `inspect-dbc`, `inspect-adt`, `inspect-map`,
  `inspect-client`, `check-map-assets`.
- `inspect-adt` summary for versions, asset tables, placements, terrain chunks,
  placement asset usage, heights, normals, texture layers, and raw `MCAL`
  payload sizes.
- `inspect-map` summary for full local map directories, validated against
  `/home/walfhand/Documents/wow-maps/guerilla`.
- `inspect-client` and `check-map-assets` have been validated against
  `/home/walfhand/Documents/Ultimate WotLK`; the standalone Guerilla patch at
  `/home/walfhand/Documents/wow-maps/patch-guerilla.MPQ` should be passed as an
  extra archive when checking that custom map files are resolvable. With the
  StormLib backend, the Guerilla asset check currently resolves all 85 referenced
  assets.

Next:

- MPQ/local asset decoding for textures, M2, and WMO through `noggit-formats`.
- Renderer-facing terrain mesh extraction from the core tile model.
- Terrain editing mutations and byte-preserving save paths.
- Real golden samples once stable fixtures are available.
