//! Renderer crate for the Rust rewrite.

use std::error::Error;
use std::fmt::{Display, Formatter};

use noggit_core::{TerrainChunk, WorldMap};

/// Full ADT tile width in yards.
pub const TILE_SIZE: f32 = 1600.0 / 3.0;
/// ADT terrain chunk width in yards.
pub const CHUNK_SIZE: f32 = TILE_SIZE / 16.0;
const CHUNK_GRID_STEPS: usize = 8;
const MCVT_ROWS: usize = 17;

/// Result type used by renderer preparation code.
pub type RenderResult<T> = Result<T, RenderError>;

/// Error returned when renderer data cannot be built.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RenderError {
    /// The terrain mesh would exceed `u32` index capacity.
    TooManyVertices,
}

/// One terrain vertex ready for renderer upload or software preview.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TerrainVertex {
    /// Position in map-local render space.
    pub position: [f32; 3],
    /// Decoded terrain normal.
    pub normal: [f32; 3],
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
    bounds: Option<TerrainBounds>,
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

    /// Return mesh bounds when at least one vertex exists.
    pub fn bounds(&self) -> Option<TerrainBounds> {
        self.bounds
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
            bounds: None,
        });
    };
    let min_tile_y = min_tile_y.unwrap_or(0);

    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    let mut bounds = None;

    for tile in map.tiles() {
        let tile_origin_x = (tile.coord().x - min_tile_x) as f32 * TILE_SIZE;
        let tile_origin_z = (tile.coord().y - min_tile_y) as f32 * TILE_SIZE;

        for chunk in tile.terrain_chunks() {
            append_chunk_mesh(
                chunk,
                tile_origin_x,
                tile_origin_z,
                &mut vertices,
                &mut indices,
                &mut bounds,
            )?;
        }
    }

    Ok(TerrainMesh {
        vertices,
        indices,
        bounds,
    })
}

fn append_chunk_mesh(
    chunk: &TerrainChunk,
    tile_origin_x: f32,
    tile_origin_z: f32,
    vertices: &mut Vec<TerrainVertex>,
    indices: &mut Vec<u32>,
    bounds: &mut Option<TerrainBounds>,
) -> RenderResult<()> {
    if chunk.heights.len() < expected_mcvt_vertex_count() {
        return Ok(());
    }

    let base_index = u32::try_from(vertices.len()).map_err(|_| RenderError::TooManyVertices)?;
    let chunk_origin_x = tile_origin_x + chunk.x as f32 * CHUNK_SIZE;
    let chunk_origin_z = tile_origin_z + chunk.y as f32 * CHUNK_SIZE;
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
            let normal = chunk
                .normals
                .get(height_index)
                .copied()
                .unwrap_or([0.0, 1.0, 0.0]);

            extend_bounds(bounds, position);
            vertices.push(TerrainVertex { position, normal });
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
        fs::write(map_dir.join("testmap_12_34.adt"), fixture_adt())?;

        let map = WorldMap::load_from_local_directory(&map_dir)?;
        let mesh = build_terrain_mesh(&map)?;

        fs::remove_dir_all(&root)?;

        assert_eq!(mesh.vertices().len(), 145);
        assert_eq!(mesh.indices().len(), 8 * 8 * 4 * 3);
        assert_eq!(mesh.vertices()[0].position, [0.0, 10.0, 0.0]);
        assert_eq!(
            mesh.vertices()[144].position,
            [CHUNK_SIZE, 154.0, CHUNK_SIZE]
        );
        assert_eq!(
            mesh.bounds(),
            Some(TerrainBounds {
                min: [0.0, 10.0, 0.0],
                max: [CHUNK_SIZE, 154.0, CHUNK_SIZE]
            })
        );
        Ok(())
    }

    fn fixture_adt() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&stored_chunk(b"MVER", &18_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MCNK", &mcnk()));
        bytes
    }

    fn mcnk() -> Vec<u8> {
        let mut bytes = vec![0; 128];
        write_u32(&mut bytes, 4, 0);
        write_u32(&mut bytes, 8, 0);
        write_f32(&mut bytes, 112, 10.0);
        push_subchunk(&mut bytes, 20, b"MCVT", &mcvt());
        push_subchunk(&mut bytes, 24, b"MCNR", &mcnr());
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
