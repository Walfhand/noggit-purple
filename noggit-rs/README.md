# Noggit Rust Rewrite

This workspace is the Rust rewrite target for Noggit.

The rewrite is test-driven: format parsers and serializers must be covered by
golden or roundtrip tests before they become dependencies of the editor core,
renderer, or UI.

## Crates

- `noggit-vfs`: local/client/archive file access.
- `noggit-formats`: WoW binary formats such as DBC, ADT, WDT, WMO, M2, and BLP.
- `noggit-core`: editor domain model and mutation logic.
- `noggit-render`: `wgpu` renderer.
- `noggit-ui`: desktop UI.
- `noggit-scripting`: Lua scripting API.
- `noggit-cli`: validation and debugging tools.

## First Vertical Slice

1. Parse DBC files and roundtrip them byte-for-byte.
2. Parse ADT chunk containers, typed headers, terrain heights, normals, texture
   layers, alpha maps, asset filename blocks, filename offset tables, and
   placement tables.
3. Add golden fixtures from known-good map files as they become available.
