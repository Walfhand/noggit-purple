//! World map domain model.

use std::path::Path;

use noggit_formats::FormatResult;
use noggit_formats::adt::{AdtFile, MddfEntry, ModfEntry, filename_by_name_id};
use noggit_vfs::{FileSource, LocalFolder, VfsPath};

use crate::error::{CoreError, CoreResult};

/// A complete WoW map directory loaded into the editor domain.
#[derive(Debug, Clone, PartialEq)]
pub struct WorldMap {
    name: String,
    tiles: Vec<WorldTile>,
}

/// One ADT tile loaded into the editor domain.
#[derive(Debug, Clone, PartialEq)]
pub struct WorldTile {
    coord: TileCoord,
    source_path: VfsPath,
    texture_assets: Vec<String>,
    model_assets: Vec<String>,
    wmo_assets: Vec<String>,
    model_placements: Vec<ModelPlacement>,
    wmo_placements: Vec<WmoPlacement>,
    terrain_chunks: Vec<TerrainChunk>,
}

/// ADT tile coordinate.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TileCoord {
    /// ADT X index from the filename.
    pub x: u32,
    /// ADT Y index from the filename.
    pub y: u32,
}

/// Terrain chunk ready for editor and renderer use.
#[derive(Debug, Clone, PartialEq)]
pub struct TerrainChunk {
    /// Chunk X index inside the ADT tile.
    pub x: u32,
    /// Chunk Y index inside the ADT tile.
    pub y: u32,
    /// Area id assigned to this chunk.
    pub area_id: u32,
    /// Terrain hole mask.
    pub holes: u32,
    /// Decoded terrain heights.
    pub heights: Vec<f32>,
    /// Decoded normals in render axis order.
    pub normals: Vec<[f32; 3]>,
    /// Texture layers on this terrain chunk.
    pub layers: Vec<TerrainLayer>,
}

/// Texture layer metadata for a terrain chunk.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TerrainLayer {
    /// Index into the tile texture asset table.
    pub texture_id: u32,
    /// Raw layer flags from `MCLY`.
    pub flags: u32,
    /// Offset into the chunk alpha-map payload.
    pub alpha_offset: u32,
    /// Texture effect id.
    pub effect_id: u32,
}

/// Height range for a terrain chunk.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HeightRange {
    /// Minimum decoded terrain height.
    pub min: f32,
    /// Maximum decoded terrain height.
    pub max: f32,
}

/// One placed M2 doodad.
#[derive(Debug, Clone, PartialEq)]
pub struct ModelPlacement {
    /// Index into `MMID`.
    pub name_id: u32,
    /// Resolved M2 asset name when the filename table is present.
    pub asset: Option<String>,
    /// Unique placement id.
    pub unique_id: u32,
    /// World position.
    pub position: [f32; 3],
    /// Rotation in degrees.
    pub rotation: [f32; 3],
    /// Raw ADT scale value, where 1024 means 1.0.
    pub scale: u16,
    /// Raw placement flags.
    pub flags: u16,
}

/// One placed WMO.
#[derive(Debug, Clone, PartialEq)]
pub struct WmoPlacement {
    /// Index into `MWID`.
    pub name_id: u32,
    /// Resolved WMO asset name when the filename table is present.
    pub asset: Option<String>,
    /// Unique placement id.
    pub unique_id: u32,
    /// World position.
    pub position: [f32; 3],
    /// Rotation in degrees.
    pub rotation: [f32; 3],
    /// Lower bounding-box extent.
    pub lower_extent: [f32; 3],
    /// Upper bounding-box extent.
    pub upper_extent: [f32; 3],
    /// Raw placement flags.
    pub flags: u16,
    /// WMO doodad set id.
    pub doodad_set: u16,
    /// WMO name set id.
    pub name_set: u16,
    /// Raw ADT scale value, where 1024 means 1.0.
    pub scale: u16,
}

impl WorldMap {
    /// Load a map from a local map directory.
    pub fn load_from_local_directory(root: impl AsRef<Path>) -> CoreResult<Self> {
        let root = root.as_ref();
        let name = root
            .file_name()
            .and_then(|value| value.to_str())
            .ok_or_else(|| CoreError::MissingMapDirectoryName {
                root: root.to_path_buf(),
            })?
            .to_owned();
        let source = LocalFolder::new(root);
        Self::load_from_source(name, &source)
    }

    /// Load all ADT tiles for `name` from a virtual file source.
    pub fn load_from_source(name: impl Into<String>, source: &impl FileSource) -> CoreResult<Self> {
        let name = name.into();
        let entries = source
            .list_files(&VfsPath::root())
            .map_err(|source| CoreError::Io { path: None, source })?;
        let mut tiles = Vec::new();

        for path in entries {
            let Some(coord) = parse_tile_coord(&name, &path) else {
                continue;
            };
            let bytes = source.read_file(&path).map_err(|source| CoreError::Io {
                path: Some(path.clone()),
                source,
            })?;
            let adt = parse_format(&path, AdtFile::parse(&bytes))?;
            tiles.push(WorldTile::from_adt(coord, path, &adt)?);
        }

        tiles.sort_by_key(|tile| tile.coord);
        Ok(Self { name, tiles })
    }

    /// Return the map directory name.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Return loaded tiles in coordinate order.
    pub fn tiles(&self) -> &[WorldTile] {
        &self.tiles
    }

    /// Return total M2 doodad placement count across all tiles.
    pub fn total_model_placements(&self) -> usize {
        self.tiles
            .iter()
            .map(|tile| tile.model_placements.len())
            .sum()
    }

    /// Return total WMO placement count across all tiles.
    pub fn total_wmo_placements(&self) -> usize {
        self.tiles
            .iter()
            .map(|tile| tile.wmo_placements.len())
            .sum()
    }

    /// Return total terrain chunk count across all tiles.
    pub fn total_terrain_chunks(&self) -> usize {
        self.tiles
            .iter()
            .map(|tile| tile.terrain_chunks.len())
            .sum()
    }
}

impl WorldTile {
    fn from_adt(coord: TileCoord, source_path: VfsPath, adt: &AdtFile) -> CoreResult<Self> {
        let texture_assets =
            parse_format(&source_path, adt.texture_filenames())?.unwrap_or_default();
        let model_assets = parse_format(&source_path, adt.model_filenames())?.unwrap_or_default();
        let model_offsets =
            parse_format(&source_path, adt.model_filename_offsets())?.unwrap_or_default();
        let wmo_assets = parse_format(&source_path, adt.wmo_filenames())?.unwrap_or_default();
        let wmo_offsets =
            parse_format(&source_path, adt.wmo_filename_offsets())?.unwrap_or_default();

        let model_placements = parse_format(&source_path, adt.model_placements())?
            .unwrap_or_default()
            .into_iter()
            .map(|placement| ModelPlacement::from_mddf(&placement, &model_assets, &model_offsets))
            .collect();
        let wmo_placements = parse_format(&source_path, adt.wmo_placements())?
            .unwrap_or_default()
            .into_iter()
            .map(|placement| WmoPlacement::from_modf(&placement, &wmo_assets, &wmo_offsets))
            .collect();
        let terrain_chunks = parse_format(&source_path, adt.mcnk_chunks())?
            .into_iter()
            .map(|chunk| {
                let heights = parse_format(&source_path, chunk.heights())?.unwrap_or_default();
                let normals = parse_format(&source_path, chunk.normals())?
                    .unwrap_or_default()
                    .into_iter()
                    .map(|normal| [normal.x, normal.y, normal.z])
                    .collect();
                let layers = parse_format(&source_path, chunk.texture_layers())?
                    .unwrap_or_default()
                    .into_iter()
                    .map(|layer| TerrainLayer {
                        texture_id: layer.texture_id,
                        flags: layer.flags,
                        alpha_offset: layer.ofs_alpha,
                        effect_id: layer.effect_id,
                    })
                    .collect();

                Ok(TerrainChunk {
                    x: chunk.header.ix,
                    y: chunk.header.iy,
                    area_id: chunk.header.area_id,
                    holes: chunk.header.holes,
                    heights,
                    normals,
                    layers,
                })
            })
            .collect::<CoreResult<Vec<_>>>()?;

        Ok(Self {
            coord,
            source_path,
            texture_assets,
            model_assets,
            wmo_assets,
            model_placements,
            wmo_placements,
            terrain_chunks,
        })
    }

    /// Return this tile's ADT coordinate.
    pub fn coord(&self) -> TileCoord {
        self.coord
    }

    /// Return the virtual source path.
    pub fn source_path(&self) -> &VfsPath {
        &self.source_path
    }

    /// Return texture assets referenced by the tile.
    pub fn texture_assets(&self) -> &[String] {
        &self.texture_assets
    }

    /// Return M2 assets referenced by the tile.
    pub fn model_assets(&self) -> &[String] {
        &self.model_assets
    }

    /// Return WMO assets referenced by the tile.
    pub fn wmo_assets(&self) -> &[String] {
        &self.wmo_assets
    }

    /// Return placed M2 doodads.
    pub fn model_placements(&self) -> &[ModelPlacement] {
        &self.model_placements
    }

    /// Return placed WMOs.
    pub fn wmo_placements(&self) -> &[WmoPlacement] {
        &self.wmo_placements
    }

    /// Return terrain chunks.
    pub fn terrain_chunks(&self) -> &[TerrainChunk] {
        &self.terrain_chunks
    }
}

impl TerrainChunk {
    /// Return the decoded height range for this chunk.
    pub fn height_range(&self) -> Option<HeightRange> {
        let (first, rest) = self.heights.split_first()?;
        let mut min = *first;
        let mut max = *first;

        for height in rest {
            min = min.min(*height);
            max = max.max(*height);
        }

        Some(HeightRange { min, max })
    }
}

impl ModelPlacement {
    fn from_mddf(entry: &MddfEntry, model_assets: &[String], model_offsets: &[u32]) -> Self {
        Self {
            name_id: entry.name_id,
            asset: filename_by_name_id(model_assets, model_offsets, entry.name_id)
                .map(str::to_owned),
            unique_id: entry.unique_id,
            position: entry.position,
            rotation: entry.rotation,
            scale: entry.scale,
            flags: entry.flags,
        }
    }
}

impl WmoPlacement {
    fn from_modf(entry: &ModfEntry, wmo_assets: &[String], wmo_offsets: &[u32]) -> Self {
        Self {
            name_id: entry.name_id,
            asset: filename_by_name_id(wmo_assets, wmo_offsets, entry.name_id).map(str::to_owned),
            unique_id: entry.unique_id,
            position: entry.position,
            rotation: entry.rotation,
            lower_extent: entry.lower_extent,
            upper_extent: entry.upper_extent,
            flags: entry.flags,
            doodad_set: entry.doodad_set,
            name_set: entry.name_set,
            scale: entry.scale,
        }
    }
}

fn parse_format<T>(path: &VfsPath, result: FormatResult<T>) -> CoreResult<T> {
    result.map_err(|source| CoreError::Format {
        path: path.clone(),
        source,
    })
}

fn parse_tile_coord(map_name: &str, path: &VfsPath) -> Option<TileCoord> {
    let filename = path.file_name()?;
    let stem = filename.strip_suffix(".adt")?;
    let rest = stem.strip_prefix(map_name)?.strip_prefix('_')?;
    let (x, y) = rest.split_once('_')?;
    if y.contains('_') {
        return None;
    }

    Some(TileCoord {
        x: x.parse().ok()?,
        y: y.parse().ok()?,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::error::Error;
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn infers_map_name_from_local_directory() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-core-map")?;
        let map_dir = root.join("guerilla");
        fs::create_dir_all(&map_dir)?;
        fs::write(map_dir.join("guerilla_27_25.adt"), fixture_adt(25))?;
        fs::write(map_dir.join("other_1_1.adt"), fixture_adt(1))?;

        let map = WorldMap::load_from_local_directory(&map_dir)?;

        fs::remove_dir_all(&root)?;

        assert_eq!(map.name(), "guerilla");
        assert_eq!(map.tiles().len(), 1);
        assert_eq!(map.tiles()[0].coord(), TileCoord { x: 27, y: 25 });
        Ok(())
    }

    #[test]
    fn loads_named_source_tiles_into_domain_model() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-core-named-map")?;
        fs::create_dir_all(&root)?;
        fs::write(root.join("guerilla_27_25.adt"), fixture_adt(25))?;
        fs::write(root.join("guerilla_28_26.adt"), fixture_adt(26))?;
        fs::write(root.join("other_1_1.adt"), fixture_adt(1))?;
        let source = LocalFolder::new(&root);

        let map = WorldMap::load_from_source("guerilla", &source)?;

        fs::remove_dir_all(&root)?;

        assert_eq!(map.name(), "guerilla");
        assert_eq!(map.tiles().len(), 2);
        assert_eq!(map.total_model_placements(), 2);
        assert_eq!(map.total_wmo_placements(), 2);
        assert_eq!(map.total_terrain_chunks(), 2);

        let tile = &map.tiles()[0];
        assert_eq!(tile.coord(), TileCoord { x: 27, y: 25 });
        assert_eq!(tile.source_path().as_str(), "guerilla_27_25.adt");
        assert_eq!(tile.texture_assets(), &["tiles/foo.blp".to_owned()]);
        assert_eq!(tile.model_assets(), &["models/tree.m2".to_owned()]);
        assert_eq!(tile.wmo_assets(), &["world/bridge.wmo".to_owned()]);
        assert_eq!(
            tile.model_placements()[0].asset.as_deref(),
            Some("models/tree.m2")
        );
        assert_eq!(
            tile.wmo_placements()[0].asset.as_deref(),
            Some("world/bridge.wmo")
        );
        assert_eq!(tile.terrain_chunks()[0].area_id, 25);
        assert_eq!(
            tile.terrain_chunks()[0].height_range(),
            Some(HeightRange {
                min: 0.0,
                max: 36.0
            })
        );
        Ok(())
    }

    fn fixture_adt(area_id: u32) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&stored_chunk(b"MVER", &18_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MTEX", &string_block(&["tiles/foo.blp"])));
        bytes.extend_from_slice(&stored_chunk(b"MMDX", &string_block(&["models/tree.m2"])));
        bytes.extend_from_slice(&stored_chunk(b"MMID", &0_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MWMO", &string_block(&["world/bridge.wmo"])));
        bytes.extend_from_slice(&stored_chunk(b"MWID", &0_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MDDF", &mddf_entry()));
        bytes.extend_from_slice(&stored_chunk(b"MODF", &modf_entry()));
        bytes.extend_from_slice(&stored_chunk(b"MCNK", &mcnk(area_id)));
        bytes
    }

    fn mcnk(area_id: u32) -> Vec<u8> {
        let mut bytes = vec![0; 128];
        write_u32(&mut bytes, 4, 4);
        write_u32(&mut bytes, 8, 9);
        write_u32(&mut bytes, 12, 1);
        write_u32(&mut bytes, 52, area_id);

        push_subchunk(&mut bytes, 20, b"MCVT", &mcvt());
        push_subchunk(&mut bytes, 24, b"MCNR", &mcnr());
        push_subchunk(&mut bytes, 28, b"MCLY", &mcly());
        bytes
    }

    fn mcvt() -> Vec<u8> {
        (0..145)
            .flat_map(|index| ((index as f32) * 0.25).to_le_bytes())
            .collect()
    }

    fn mcnr() -> Vec<u8> {
        vec![0; 145 * 3]
    }

    fn mcly() -> Vec<u8> {
        [0_u32, 0, 0, 0xFFFF_FFFF]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect()
    }

    fn mddf_entry() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&0_u32.to_le_bytes());
        bytes.extend_from_slice(&77_u32.to_le_bytes());
        push_vec3(&mut bytes, [1.0, 2.0, 3.0]);
        push_vec3(&mut bytes, [10.0, 20.0, 30.0]);
        bytes.extend_from_slice(&1024_u16.to_le_bytes());
        bytes.extend_from_slice(&0_u16.to_le_bytes());
        bytes
    }

    fn modf_entry() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&0_u32.to_le_bytes());
        bytes.extend_from_slice(&88_u32.to_le_bytes());
        push_vec3(&mut bytes, [4.0, 5.0, 6.0]);
        push_vec3(&mut bytes, [40.0, 50.0, 60.0]);
        push_vec3(&mut bytes, [-1.0, -2.0, -3.0]);
        push_vec3(&mut bytes, [7.0, 8.0, 9.0]);
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

    fn string_block(strings: &[&str]) -> Vec<u8> {
        let mut bytes = Vec::new();
        for value in strings {
            bytes.extend_from_slice(value.as_bytes());
            bytes.push(0);
        }
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

    fn test_root(prefix: &str) -> Result<PathBuf, Box<dyn Error>> {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        Ok(std::env::temp_dir().join(format!("{prefix}-{}-{nanos}", std::process::id())))
    }
}
