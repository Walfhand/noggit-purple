//! Renderer crate for the Rust rewrite.

use std::collections::BTreeMap;
use std::error::Error;
use std::fmt::{Display, Formatter};

use noggit_core::{ModelPlacement, TerrainChunk, WmoPlacement, WorldMap};

/// Full ADT tile width in yards.
pub const TILE_SIZE: f32 = 1600.0 / 3.0;
/// ADT terrain chunk width in yards.
pub const CHUNK_SIZE: f32 = TILE_SIZE / 16.0;
const CHUNK_GRID_STEPS: usize = 8;
const MCVT_ROWS: usize = 17;
/// Byte count for one decoded 64x64 terrain alpha map.
pub const TERRAIN_ALPHA_MAP_SIZE: usize = 64 * 64;
const MODEL_MARKER_COLOR: [f32; 4] = [0.1, 0.85, 1.0, 1.0];
const WMO_MARKER_COLOR: [f32; 4] = [1.0, 0.62, 0.16, 1.0];
const MODEL_MARKER_HALF_SIZE: f32 = 3.0;
const MODEL_MARKER_HEIGHT: f32 = 12.0;
const MIN_MARKER_EXTENT: f32 = 2.0;

/// Result type used by renderer preparation code.
pub type RenderResult<T> = Result<T, RenderError>;

/// Error returned when renderer data cannot be built.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RenderError {
    /// The terrain mesh would exceed `u32` index capacity.
    TooManyVertices,
    /// The terrain mesh would exceed `u32` material id capacity.
    TooManyMaterials,
    /// The terrain mesh would exceed `u32` alpha map id capacity.
    TooManyAlphaMaps,
}

/// One terrain vertex ready for renderer upload or software preview.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TerrainVertex {
    /// Position in map-local render space.
    pub position: [f32; 3],
    /// Decoded terrain normal.
    pub normal: [f32; 3],
    /// Terrain texture coordinate in WoW chunk detail-map space.
    pub tex_coord: [f32; 2],
    /// Indices into [`TerrainMesh::materials`] for up to four terrain layers.
    pub material_ids: [Option<u32>; 4],
    /// Indices into [`TerrainMesh::alpha_maps`] for layers 1 through 3.
    pub alpha_map_ids: [Option<u32>; 3],
}

/// Axis-aligned bounds for render data.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TerrainBounds {
    /// Minimum coordinate.
    pub min: [f32; 3],
    /// Maximum coordinate.
    pub max: [f32; 3],
}

/// Terrain mesh generated from loaded ADT chunks.
#[derive(Debug, Clone, PartialEq)]
pub struct TerrainMesh {
    vertices: Vec<TerrainVertex>,
    indices: Vec<u32>,
    materials: Vec<String>,
    alpha_maps: Vec<TerrainAlphaMap>,
    bounds: Option<TerrainBounds>,
}

/// Renderer-owned 64x64 terrain alpha map.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TerrainAlphaMap {
    /// Alpha values in row-major 64x64 order.
    pub values: [u8; TERRAIN_ALPHA_MAP_SIZE],
}

/// One colored vertex for a debug placement overlay.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PlacementMarkerVertex {
    /// Position in map-local render space.
    pub position: [f32; 3],
    /// Linear RGBA color.
    pub color: [f32; 4],
}

/// Line-list geometry for M2/WMO placement debugging.
#[derive(Debug, Clone, PartialEq)]
pub struct PlacementMarkerMesh {
    vertices: Vec<PlacementMarkerVertex>,
    indices: Vec<u32>,
    bounds: Option<TerrainBounds>,
    marker_count: usize,
}

impl TerrainMesh {
    /// Return all terrain vertices.
    pub fn vertices(&self) -> &[TerrainVertex] {
        &self.vertices
    }

    /// Return triangle indices.
    pub fn indices(&self) -> &[u32] {
        &self.indices
    }

    /// Return texture assets referenced by terrain vertices.
    pub fn materials(&self) -> &[String] {
        &self.materials
    }

    /// Return alpha maps referenced by terrain vertices.
    pub fn alpha_maps(&self) -> &[TerrainAlphaMap] {
        &self.alpha_maps
    }

    /// Return mesh bounds when at least one vertex exists.
    pub fn bounds(&self) -> Option<TerrainBounds> {
        self.bounds
    }
}

impl PlacementMarkerMesh {
    /// Return overlay line vertices.
    pub fn vertices(&self) -> &[PlacementMarkerVertex] {
        &self.vertices
    }

    /// Return line-list indices.
    pub fn indices(&self) -> &[u32] {
        &self.indices
    }

    /// Return marker bounds when at least one placement marker exists.
    pub fn bounds(&self) -> Option<TerrainBounds> {
        self.bounds
    }

    /// Return the number of source placements represented in this mesh.
    pub fn marker_count(&self) -> usize {
        self.marker_count
    }
}

impl TerrainBounds {
    /// Return the center point of the bounds.
    pub fn center(&self) -> [f32; 3] {
        [
            (self.min[0] + self.max[0]) * 0.5,
            (self.min[1] + self.max[1]) * 0.5,
            (self.min[2] + self.max[2]) * 0.5,
        ]
    }

    /// Return the bounds size on each axis.
    pub fn size(&self) -> [f32; 3] {
        [
            self.max[0] - self.min[0],
            self.max[1] - self.min[1],
            self.max[2] - self.min[2],
        ]
    }
}

impl Display for RenderError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::TooManyVertices => write!(f, "terrain mesh has too many vertices"),
            Self::TooManyMaterials => write!(f, "terrain mesh has too many materials"),
            Self::TooManyAlphaMaps => write!(f, "terrain mesh has too many alpha maps"),
        }
    }
}

impl Error for RenderError {}

/// Build a render-space terrain mesh from a loaded world map.
pub fn build_terrain_mesh(map: &WorldMap) -> RenderResult<TerrainMesh> {
    let min_tile_x = map.tiles().iter().map(|tile| tile.coord().x).min();
    let min_tile_y = map.tiles().iter().map(|tile| tile.coord().y).min();
    let Some(min_tile_x) = min_tile_x else {
        return Ok(TerrainMesh {
            vertices: Vec::new(),
            indices: Vec::new(),
            materials: Vec::new(),
            alpha_maps: Vec::new(),
            bounds: None,
        });
    };
    let min_tile_y = min_tile_y.unwrap_or(0);

    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    let mut materials = Vec::new();
    let mut material_ids = BTreeMap::new();
    let mut alpha_maps = Vec::new();
    let mut bounds = None;

    for tile in map.tiles() {
        let tile_origin_x = (tile.coord().x - min_tile_x) as f32 * TILE_SIZE;
        let tile_origin_z = (tile.coord().y - min_tile_y) as f32 * TILE_SIZE;

        for chunk in tile.terrain_chunks() {
            let material_layer_ids = material_layer_ids(
                chunk,
                tile.texture_assets(),
                &mut materials,
                &mut material_ids,
            )?;
            let alpha_map_ids = alpha_map_ids(chunk, &mut alpha_maps)?;

            append_chunk_mesh(
                chunk,
                [tile_origin_x, tile_origin_z],
                material_layer_ids,
                alpha_map_ids,
                &mut vertices,
                &mut indices,
                &mut bounds,
            )?;
        }
    }

    Ok(TerrainMesh {
        vertices,
        indices,
        materials,
        alpha_maps,
        bounds,
    })
}

/// Build debug line boxes for loaded M2 and WMO placements.
pub fn build_placement_marker_mesh(map: &WorldMap) -> RenderResult<PlacementMarkerMesh> {
    let min_tile_x = map.tiles().iter().map(|tile| tile.coord().x).min();
    let min_tile_y = map.tiles().iter().map(|tile| tile.coord().y).min();
    let Some(min_tile_x) = min_tile_x else {
        return Ok(PlacementMarkerMesh {
            vertices: Vec::new(),
            indices: Vec::new(),
            bounds: None,
            marker_count: 0,
        });
    };
    let min_tile_y = min_tile_y.unwrap_or(0);

    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    let mut bounds = None;
    let mut marker_count = 0usize;

    for tile in map.tiles() {
        for placement in tile.model_placements() {
            append_model_marker(
                placement,
                min_tile_x,
                min_tile_y,
                &mut vertices,
                &mut indices,
                &mut bounds,
            )?;
            marker_count += 1;
        }

        for placement in tile.wmo_placements() {
            append_wmo_marker(
                placement,
                min_tile_x,
                min_tile_y,
                &mut vertices,
                &mut indices,
                &mut bounds,
            )?;
            marker_count += 1;
        }
    }

    Ok(PlacementMarkerMesh {
        vertices,
        indices,
        bounds,
        marker_count,
    })
}

fn append_model_marker(
    placement: &ModelPlacement,
    min_tile_x: u32,
    min_tile_y: u32,
    vertices: &mut Vec<PlacementMarkerVertex>,
    indices: &mut Vec<u32>,
    bounds: &mut Option<TerrainBounds>,
) -> RenderResult<()> {
    let center = world_position_to_render_local(placement.position, min_tile_x, min_tile_y);
    let scale = (placement.scale as f32 / 1024.0).max(0.1);
    let half = MODEL_MARKER_HALF_SIZE * scale;
    let height = MODEL_MARKER_HEIGHT * scale;
    append_line_box(
        [center[0] - half, center[1], center[2] - half],
        [center[0] + half, center[1] + height, center[2] + half],
        MODEL_MARKER_COLOR,
        vertices,
        indices,
        bounds,
    )
}

fn append_wmo_marker(
    placement: &WmoPlacement,
    min_tile_x: u32,
    min_tile_y: u32,
    vertices: &mut Vec<PlacementMarkerVertex>,
    indices: &mut Vec<u32>,
    bounds: &mut Option<TerrainBounds>,
) -> RenderResult<()> {
    let lower = world_position_to_render_local(placement.lower_extent, min_tile_x, min_tile_y);
    let upper = world_position_to_render_local(placement.upper_extent, min_tile_x, min_tile_y);
    let (min, max) = min_max_points(lower, upper);
    let (min, max) = expand_tiny_bounds(min, max);
    append_line_box(min, max, WMO_MARKER_COLOR, vertices, indices, bounds)
}

fn world_position_to_render_local(
    world_position: [f32; 3],
    min_tile_x: u32,
    min_tile_y: u32,
) -> [f32; 3] {
    [
        world_position[0] - min_tile_x as f32 * TILE_SIZE,
        world_position[1],
        world_position[2] - min_tile_y as f32 * TILE_SIZE,
    ]
}

fn min_max_points(a: [f32; 3], b: [f32; 3]) -> ([f32; 3], [f32; 3]) {
    (
        [a[0].min(b[0]), a[1].min(b[1]), a[2].min(b[2])],
        [a[0].max(b[0]), a[1].max(b[1]), a[2].max(b[2])],
    )
}

fn expand_tiny_bounds(mut min: [f32; 3], mut max: [f32; 3]) -> ([f32; 3], [f32; 3]) {
    for axis in 0..3 {
        if max[axis] - min[axis] >= MIN_MARKER_EXTENT {
            continue;
        }

        let center = (min[axis] + max[axis]) * 0.5;
        min[axis] = center - MIN_MARKER_EXTENT * 0.5;
        max[axis] = center + MIN_MARKER_EXTENT * 0.5;
    }
    (min, max)
}

fn append_line_box(
    min: [f32; 3],
    max: [f32; 3],
    color: [f32; 4],
    vertices: &mut Vec<PlacementMarkerVertex>,
    indices: &mut Vec<u32>,
    bounds: &mut Option<TerrainBounds>,
) -> RenderResult<()> {
    let base_index = u32::try_from(vertices.len()).map_err(|_| RenderError::TooManyVertices)?;
    let corners = [
        [min[0], min[1], min[2]],
        [max[0], min[1], min[2]],
        [max[0], min[1], max[2]],
        [min[0], min[1], max[2]],
        [min[0], max[1], min[2]],
        [max[0], max[1], min[2]],
        [max[0], max[1], max[2]],
        [min[0], max[1], max[2]],
    ];

    for position in corners {
        extend_bounds(bounds, position);
        vertices.push(PlacementMarkerVertex { position, color });
    }

    for [a, b] in [
        [0, 1],
        [1, 2],
        [2, 3],
        [3, 0],
        [4, 5],
        [5, 6],
        [6, 7],
        [7, 4],
        [0, 4],
        [1, 5],
        [2, 6],
        [3, 7],
    ] {
        indices.push(
            base_index
                .checked_add(a)
                .ok_or(RenderError::TooManyVertices)?,
        );
        indices.push(
            base_index
                .checked_add(b)
                .ok_or(RenderError::TooManyVertices)?,
        );
    }

    Ok(())
}

fn material_layer_ids(
    chunk: &TerrainChunk,
    texture_assets: &[String],
    materials: &mut Vec<String>,
    material_ids: &mut BTreeMap<String, u32>,
) -> RenderResult<[Option<u32>; 4]> {
    let mut ids = [None; 4];
    for (slot, layer) in chunk.layers.iter().take(ids.len()).enumerate() {
        ids[slot] = texture_assets
            .get(layer.texture_id as usize)
            .map(|asset| material_id_for(asset, materials, material_ids))
            .transpose()?;
    }
    Ok(ids)
}

fn material_id_for(
    asset: &str,
    materials: &mut Vec<String>,
    material_ids: &mut BTreeMap<String, u32>,
) -> RenderResult<u32> {
    if let Some(id) = material_ids.get(asset) {
        return Ok(*id);
    }

    let id = u32::try_from(materials.len()).map_err(|_| RenderError::TooManyMaterials)?;
    let asset = asset.to_owned();
    materials.push(asset.clone());
    material_ids.insert(asset, id);
    Ok(id)
}

fn alpha_map_ids(
    chunk: &TerrainChunk,
    alpha_maps: &mut Vec<TerrainAlphaMap>,
) -> RenderResult<[Option<u32>; 3]> {
    let mut ids = [None; 3];
    for (layer_index, alpha) in chunk.alpha_maps.iter().enumerate().skip(1).take(ids.len()) {
        let Some(alpha) = alpha else {
            continue;
        };
        let id = u32::try_from(alpha_maps.len()).map_err(|_| RenderError::TooManyAlphaMaps)?;
        alpha_maps.push(TerrainAlphaMap {
            values: alpha.values,
        });
        ids[layer_index - 1] = Some(id);
    }
    Ok(ids)
}

fn append_chunk_mesh(
    chunk: &TerrainChunk,
    tile_origin: [f32; 2],
    material_ids: [Option<u32>; 4],
    alpha_map_ids: [Option<u32>; 3],
    vertices: &mut Vec<TerrainVertex>,
    indices: &mut Vec<u32>,
    bounds: &mut Option<TerrainBounds>,
) -> RenderResult<()> {
    if chunk.heights.len() < expected_mcvt_vertex_count() {
        return Ok(());
    }

    let base_index = u32::try_from(vertices.len()).map_err(|_| RenderError::TooManyVertices)?;
    let chunk_origin_x = tile_origin[0] + chunk.x as f32 * CHUNK_SIZE;
    let chunk_origin_z = tile_origin[1] + chunk.y as f32 * CHUNK_SIZE;
    let step = CHUNK_SIZE / CHUNK_GRID_STEPS as f32;
    let mut lookup = [[None; 9]; MCVT_ROWS];
    let mut height_index = 0usize;

    for (row, lookup_row) in lookup.iter_mut().enumerate() {
        let columns = mcvt_row_columns(row);
        for (column, lookup_cell) in lookup_row.iter_mut().take(columns).enumerate() {
            let offset = if row.is_multiple_of(2) { 0.0 } else { 0.5 };
            let position = [
                chunk_origin_x + (column as f32 + offset) * step,
                chunk.base_height + chunk.heights[height_index],
                chunk_origin_z + (row as f32 * 0.5) * step,
            ];
            let tex_coord = [column as f32 + offset, row as f32 * 0.5];
            let normal = chunk
                .normals
                .get(height_index)
                .copied()
                .unwrap_or([0.0, 1.0, 0.0]);

            extend_bounds(bounds, position);
            vertices.push(TerrainVertex {
                position,
                normal,
                tex_coord,
                material_ids,
                alpha_map_ids,
            });
            *lookup_cell = Some(
                base_index
                    .checked_add(
                        u32::try_from(height_index).map_err(|_| RenderError::TooManyVertices)?,
                    )
                    .ok_or(RenderError::TooManyVertices)?,
            );
            height_index += 1;
        }
    }

    for cell_y in 0..CHUNK_GRID_STEPS {
        for cell_x in 0..CHUNK_GRID_STEPS {
            let top_row = cell_y * 2;
            let center_row = top_row + 1;
            let bottom_row = top_row + 2;

            let Some(top_left) = lookup[top_row][cell_x] else {
                continue;
            };
            let Some(top_right) = lookup[top_row][cell_x + 1] else {
                continue;
            };
            let Some(center) = lookup[center_row][cell_x] else {
                continue;
            };
            let Some(bottom_left) = lookup[bottom_row][cell_x] else {
                continue;
            };
            let Some(bottom_right) = lookup[bottom_row][cell_x + 1] else {
                continue;
            };

            indices.extend_from_slice(&[
                top_left,
                center,
                top_right,
                top_right,
                center,
                bottom_right,
                bottom_right,
                center,
                bottom_left,
                bottom_left,
                center,
                top_left,
            ]);
        }
    }

    Ok(())
}

fn extend_bounds(bounds: &mut Option<TerrainBounds>, position: [f32; 3]) {
    match bounds {
        Some(bounds) => {
            for (axis, value) in position.iter().copied().enumerate() {
                bounds.min[axis] = bounds.min[axis].min(value);
                bounds.max[axis] = bounds.max[axis].max(value);
            }
        }
        None => {
            *bounds = Some(TerrainBounds {
                min: position,
                max: position,
            });
        }
    }
}

fn expected_mcvt_vertex_count() -> usize {
    (0..MCVT_ROWS).map(mcvt_row_columns).sum()
}

fn mcvt_row_columns(row: usize) -> usize {
    if row.is_multiple_of(2) { 9 } else { 8 }
}

#[cfg(test)]
mod tests {
    use super::*;
    use noggit_core::WorldMap;
    use std::error::Error;
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn builds_terrain_mesh_from_loaded_adt_heights() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-render-terrain")?;
        let map_dir = root.join("testmap");
        fs::create_dir_all(&map_dir)?;
        fs::write(map_dir.join("testmap.wdt"), fixture_wdt())?;
        fs::write(map_dir.join("testmap_12_34.adt"), fixture_adt())?;

        let map = WorldMap::load_from_local_directory(&map_dir)?;
        let mesh = build_terrain_mesh(&map)?;

        fs::remove_dir_all(&root)?;

        assert_eq!(mesh.vertices().len(), 145);
        assert_eq!(mesh.indices().len(), 8 * 8 * 4 * 3);
        assert_eq!(
            mesh.materials(),
            &["tiles/rock.blp".to_owned(), "tiles/dirt.blp".to_owned()]
        );
        assert_eq!(mesh.alpha_maps().len(), 1);
        assert_eq!(mesh.alpha_maps()[0].values[7], 7);
        assert_eq!(mesh.vertices()[0].position, [0.0, 10.0, 0.0]);
        assert_eq!(mesh.vertices()[0].tex_coord, [0.0, 0.0]);
        assert_eq!(mesh.vertices()[9].tex_coord, [0.5, 0.5]);
        assert_eq!(
            mesh.vertices()[0].material_ids,
            [Some(0), Some(1), None, None]
        );
        assert_eq!(mesh.vertices()[0].alpha_map_ids, [Some(0), None, None]);
        assert_eq!(
            mesh.vertices()[144].position,
            [CHUNK_SIZE, 154.0, CHUNK_SIZE]
        );
        assert_eq!(mesh.vertices()[144].tex_coord, [8.0, 8.0]);
        assert_eq!(
            mesh.bounds(),
            Some(TerrainBounds {
                min: [0.0, 10.0, 0.0],
                max: [CHUNK_SIZE, 154.0, CHUNK_SIZE]
            })
        );
        Ok(())
    }

    #[test]
    fn converts_world_positions_to_map_local_render_space() {
        let world_position = [27.0 * TILE_SIZE + 10.0, 7.0, 25.0 * TILE_SIZE + 20.0];

        assert_eq!(
            world_position_to_render_local(world_position, 27, 25),
            [10.0, 7.0, 20.0]
        );
    }

    #[test]
    fn builds_debug_marker_lines_for_model_and_wmo_placements() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-render-placement-markers")?;
        let map_dir = root.join("testmap");
        fs::create_dir_all(&map_dir)?;
        fs::write(map_dir.join("testmap.wdt"), fixture_wdt())?;
        fs::write(map_dir.join("testmap_27_25.adt"), fixture_adt())?;

        let map = WorldMap::load_from_local_directory(&map_dir)?;
        let markers = build_placement_marker_mesh(&map)?;

        fs::remove_dir_all(&root)?;

        assert_eq!(markers.vertices().len(), 16);
        assert_eq!(markers.indices().len(), 48);
        assert_eq!(markers.marker_count(), 2);
        assert_eq!(markers.vertices()[0].color, MODEL_MARKER_COLOR);
        assert_eq!(markers.vertices()[8].color, WMO_MARKER_COLOR);
        assert!(markers.bounds().is_some());
        Ok(())
    }

    fn fixture_adt() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&stored_chunk(b"MVER", &18_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MHDR", &mhdr()));
        bytes.extend_from_slice(&stored_chunk(
            b"MTEX",
            &string_block(&["tiles/rock.blp", "tiles/dirt.blp"]),
        ));
        bytes.extend_from_slice(&stored_chunk(b"MMDX", &string_block(&["models/tree.m2"])));
        bytes.extend_from_slice(&stored_chunk(b"MMID", &0_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MWMO", &string_block(&["world/bridge.wmo"])));
        bytes.extend_from_slice(&stored_chunk(b"MWID", &0_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MDDF", &mddf_entry()));
        bytes.extend_from_slice(&stored_chunk(b"MODF", &modf_entry()));
        bytes.extend_from_slice(&stored_chunk(b"MCNK", &mcnk()));
        bytes
    }

    fn fixture_wdt() -> Vec<u8> {
        let mphd = [0x0004_u32, 0, 0, 0, 0, 0, 0, 0]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&stored_chunk(b"MVER", &18_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MPHD", &mphd));
        bytes
    }

    fn mhdr() -> Vec<u8> {
        let mut bytes = vec![0; 64];
        write_u32(&mut bytes, 0, 0x0004);
        bytes
    }

    fn mcnk() -> Vec<u8> {
        let mut bytes = vec![0; 128];
        write_u32(&mut bytes, 4, 0);
        write_u32(&mut bytes, 8, 0);
        write_u32(&mut bytes, 12, 2);
        write_f32(&mut bytes, 112, 10.0);
        push_subchunk(&mut bytes, 20, b"MCVT", &mcvt());
        push_subchunk(&mut bytes, 24, b"MCNR", &mcnr());
        push_subchunk(&mut bytes, 28, b"MCLY", &mcly());
        let mcal = mcal();
        write_u32(&mut bytes, 40, (8 + mcal.len()) as u32);
        push_subchunk(&mut bytes, 36, b"MCAL", &mcal);
        bytes
    }

    fn mcvt() -> Vec<u8> {
        (0..145)
            .flat_map(|index| (index as f32).to_le_bytes())
            .collect()
    }

    fn mcnr() -> Vec<u8> {
        vec![127; 145 * 3]
    }

    fn mcly() -> Vec<u8> {
        [0_u32, 0, 0, 0xFFFF_FFFF, 1, 0x100, 0, 0xFFFF_FFFF]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect()
    }

    fn mcal() -> Vec<u8> {
        (0..TERRAIN_ALPHA_MAP_SIZE)
            .map(|index| (index % 251) as u8)
            .collect()
    }

    fn string_block(strings: &[&str]) -> Vec<u8> {
        let mut bytes = Vec::new();
        for value in strings {
            bytes.extend_from_slice(value.as_bytes());
            bytes.push(0);
        }
        bytes
    }

    fn mddf_entry() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&0_u32.to_le_bytes());
        bytes.extend_from_slice(&77_u32.to_le_bytes());
        push_vec3(
            &mut bytes,
            [27.0 * TILE_SIZE + 10.0, 7.0, 25.0 * TILE_SIZE + 20.0],
        );
        push_vec3(&mut bytes, [0.0, 0.0, 0.0]);
        bytes.extend_from_slice(&1024_u16.to_le_bytes());
        bytes.extend_from_slice(&0_u16.to_le_bytes());
        bytes
    }

    fn modf_entry() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&0_u32.to_le_bytes());
        bytes.extend_from_slice(&88_u32.to_le_bytes());
        push_vec3(
            &mut bytes,
            [27.0 * TILE_SIZE + 60.0, 20.0, 25.0 * TILE_SIZE + 80.0],
        );
        push_vec3(&mut bytes, [0.0, 0.0, 0.0]);
        push_vec3(
            &mut bytes,
            [27.0 * TILE_SIZE + 50.0, 12.0, 25.0 * TILE_SIZE + 70.0],
        );
        push_vec3(
            &mut bytes,
            [27.0 * TILE_SIZE + 80.0, 28.0, 25.0 * TILE_SIZE + 90.0],
        );
        bytes.extend_from_slice(&0_u16.to_le_bytes());
        bytes.extend_from_slice(&0_u16.to_le_bytes());
        bytes.extend_from_slice(&0_u16.to_le_bytes());
        bytes.extend_from_slice(&1024_u16.to_le_bytes());
        bytes
    }

    fn push_vec3(bytes: &mut Vec<u8>, values: [f32; 3]) {
        for value in values {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
    }

    fn stored_chunk(id: &[u8; 4], data: &[u8]) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&[id[3], id[2], id[1], id[0]]);
        bytes.extend_from_slice(&(data.len() as u32).to_le_bytes());
        bytes.extend_from_slice(data);
        bytes
    }

    fn push_subchunk(bytes: &mut Vec<u8>, offset_field: usize, id: &[u8; 4], data: &[u8]) {
        let offset = (bytes.len() + 8) as u32;
        write_u32(bytes, offset_field, offset);
        bytes.extend_from_slice(&stored_chunk(id, data));
    }

    fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
        bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn write_f32(bytes: &mut [u8], offset: usize, value: f32) {
        bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn test_root(prefix: &str) -> Result<PathBuf, Box<dyn Error>> {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        Ok(std::env::temp_dir().join(format!("{prefix}-{}-{nanos}", std::process::id())))
    }
}
