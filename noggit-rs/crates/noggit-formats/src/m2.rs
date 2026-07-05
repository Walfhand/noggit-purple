//! M2 model and skin file parsing.

use crate::error::{
    FormatError, FormatResult, read_exact, read_f32_le, read_u8, read_u16_le, read_u32_le,
};

/// WotLK `ModelHeader` byte size used by M2 files.
pub const M2_HEADER_SIZE: usize = 304;
const M2_VERTEX_SIZE: usize = 48;
const M2_TEXTURE_SIZE: usize = 16;
const M2_SKIN_HEADER_SIZE: usize = 48;
const M2_SUBMESH_SIZE: usize = 48;
const M2_TEXTURE_UNIT_SIZE: usize = 24;

/// Parsed M2 model file.
#[derive(Debug, Clone, PartialEq)]
pub struct M2File {
    header: M2Header,
    vertices: Vec<M2Vertex>,
    textures: Vec<M2Texture>,
    texture_lookup: Vec<u16>,
}

/// Parsed M2 skin file.
#[derive(Debug, Clone, PartialEq)]
pub struct M2SkinFile {
    header: M2SkinHeader,
    indices: Vec<u16>,
    triangles: Vec<u16>,
    submeshes: Vec<M2Submesh>,
    texture_units: Vec<M2TextureUnit>,
}

/// M2 root header fields currently needed by the Rust renderer.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct M2Header {
    /// M2 version, `264` for WotLK.
    pub version: u32,
    /// Raw global model flags.
    pub flags: u32,
    /// Number of model vertices.
    pub vertex_count: u32,
    /// File offset to model vertices.
    pub vertex_offset: u32,
    /// Number of skin views.
    pub view_count: u32,
    /// Number of texture definitions.
    pub texture_count: u32,
    /// File offset to texture definitions.
    pub texture_offset: u32,
    /// Number of texture lookup entries.
    pub texture_lookup_count: u32,
    /// File offset to texture lookup entries.
    pub texture_lookup_offset: u32,
    /// Model bounding-box minimum in native M2 coordinates.
    pub bounding_box_min: [f32; 3],
    /// Model bounding-box maximum in native M2 coordinates.
    pub bounding_box_max: [f32; 3],
    /// Model bounding radius.
    pub bounding_radius: f32,
}

/// One M2 model vertex.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct M2Vertex {
    /// Vertex position in native M2 coordinates.
    pub position: [f32; 3],
    /// Bone weights.
    pub weights: [u8; 4],
    /// Bone indices.
    pub bones: [u8; 4],
    /// Vertex normal in native M2 coordinates.
    pub normal: [f32; 3],
    /// Two texture-coordinate channels.
    pub tex_coords: [[f32; 2]; 2],
}

/// One M2 texture definition.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct M2Texture {
    /// Texture type. Type `0` stores a filename directly in the M2.
    pub texture_type: u32,
    /// Raw texture flags.
    pub flags: u32,
    /// Texture filename for direct texture references.
    pub filename: Option<String>,
}

/// M2 skin header fields currently needed by the Rust renderer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct M2SkinHeader {
    /// Number of entries in the skin vertex lookup table.
    pub index_count: u32,
    /// File offset to the skin vertex lookup table.
    pub index_offset: u32,
    /// Number of triangle-index entries.
    pub triangle_count: u32,
    /// File offset to triangle-index entries.
    pub triangle_offset: u32,
    /// Number of submesh records.
    pub submesh_count: u32,
    /// File offset to submesh records.
    pub submesh_offset: u32,
    /// Number of texture-unit records.
    pub texture_unit_count: u32,
    /// File offset to texture-unit records.
    pub texture_unit_offset: u32,
    /// Skin LOD value.
    pub lod: i32,
}

/// One M2 skin submesh/geoset.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct M2Submesh {
    /// Geoset id.
    pub id: u16,
    /// First model vertex in this submesh.
    pub vertex_start: u16,
    /// Number of model vertices in this submesh.
    pub vertex_count: u16,
    /// First resolved index in the skin triangle buffer.
    pub index_start: u16,
    /// Number of resolved indices in this submesh.
    pub index_count: u16,
    /// Submesh bounding-box minimum.
    pub bounding_box_min: [f32; 3],
    /// Submesh bounding-box maximum.
    pub bounding_box_max: [f32; 3],
    /// Submesh bounding radius.
    pub radius: f32,
}

/// One M2 texture unit/render pass.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct M2TextureUnit {
    /// Texture-unit flags.
    pub flags: u8,
    /// Priority plane.
    pub priority_plane: u8,
    /// Shader id.
    pub shader_id: u16,
    /// Submesh index this unit draws.
    pub submesh: u16,
    /// Geoset index.
    pub geoset_index: u16,
    /// Color lookup index or `-1`.
    pub color_index: i16,
    /// Render-flag lookup index.
    pub renderflag_index: u16,
    /// Material layer.
    pub material_layer: u16,
    /// Number of textures used by the pass.
    pub texture_count: u16,
    /// Index into the model texture lookup table.
    pub texture_combo_index: u16,
    /// Texture-coordinate lookup index.
    pub texture_coord_combo_index: u16,
    /// Transparency lookup index.
    pub transparency_combo_index: u16,
    /// Texture-animation lookup index.
    pub animation_combo_index: u16,
}

impl M2File {
    /// Parse an M2 file into the root data needed for static model rendering.
    pub fn parse(bytes: &[u8]) -> FormatResult<Self> {
        let magic = read_exact::<4>(bytes, 0)?;
        if magic != *b"MD20" {
            return Err(FormatError::InvalidMagic {
                expected: *b"MD20",
                actual: magic,
            });
        }

        let header = M2Header::parse(bytes)?;
        Ok(Self {
            vertices: parse_vertices(bytes, header.vertex_offset, header.vertex_count)?,
            textures: parse_textures(bytes, header.texture_offset, header.texture_count)?,
            texture_lookup: parse_u16_array(
                bytes,
                header.texture_lookup_offset,
                header.texture_lookup_count,
                "M2 texture lookup",
            )?,
            header,
        })
    }

    /// Return the parsed root header.
    pub fn header(&self) -> M2Header {
        self.header
    }

    /// Return model vertices.
    pub fn vertices(&self) -> &[M2Vertex] {
        &self.vertices
    }

    /// Return texture definitions.
    pub fn textures(&self) -> &[M2Texture] {
        &self.textures
    }

    /// Return texture lookup table entries.
    pub fn texture_lookup(&self) -> &[u16] {
        &self.texture_lookup
    }
}

impl M2SkinFile {
    /// Parse an M2 `.skin` file.
    pub fn parse(bytes: &[u8]) -> FormatResult<Self> {
        let magic = read_exact::<4>(bytes, 0)?;
        if magic != *b"SKIN" {
            return Err(FormatError::InvalidMagic {
                expected: *b"SKIN",
                actual: magic,
            });
        }

        let header = M2SkinHeader::parse(bytes)?;
        Ok(Self {
            indices: parse_u16_array(
                bytes,
                header.index_offset,
                header.index_count,
                "M2 skin indices",
            )?,
            triangles: parse_u16_array(
                bytes,
                header.triangle_offset,
                header.triangle_count,
                "M2 skin triangles",
            )?,
            submeshes: parse_submeshes(bytes, header.submesh_offset, header.submesh_count)?,
            texture_units: parse_texture_units(
                bytes,
                header.texture_unit_offset,
                header.texture_unit_count,
            )?,
            header,
        })
    }

    /// Return the parsed skin header.
    pub fn header(&self) -> M2SkinHeader {
        self.header
    }

    /// Return the skin vertex lookup table.
    pub fn indices(&self) -> &[u16] {
        &self.indices
    }

    /// Return triangle-index entries into the skin vertex lookup table.
    pub fn triangles(&self) -> &[u16] {
        &self.triangles
    }

    /// Return skin submeshes.
    pub fn submeshes(&self) -> &[M2Submesh] {
        &self.submeshes
    }

    /// Return texture units.
    pub fn texture_units(&self) -> &[M2TextureUnit] {
        &self.texture_units
    }
}

impl M2Header {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        ensure_len(bytes, M2_HEADER_SIZE)?;
        Ok(Self {
            version: read_u32_le(bytes, 4)?,
            flags: read_u32_le(bytes, 16)?,
            vertex_count: read_u32_le(bytes, 60)?,
            vertex_offset: read_u32_le(bytes, 64)?,
            view_count: read_u32_le(bytes, 68)?,
            texture_count: read_u32_le(bytes, 80)?,
            texture_offset: read_u32_le(bytes, 84)?,
            texture_lookup_count: read_u32_le(bytes, 128)?,
            texture_lookup_offset: read_u32_le(bytes, 132)?,
            bounding_box_min: read_vec3(bytes, 160)?,
            bounding_box_max: read_vec3(bytes, 172)?,
            bounding_radius: read_f32_le(bytes, 184)?,
        })
    }
}

impl M2SkinHeader {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        ensure_len(bytes, M2_SKIN_HEADER_SIZE)?;
        Ok(Self {
            index_count: read_u32_le(bytes, 4)?,
            index_offset: read_u32_le(bytes, 8)?,
            triangle_count: read_u32_le(bytes, 12)?,
            triangle_offset: read_u32_le(bytes, 16)?,
            submesh_count: read_u32_le(bytes, 28)?,
            submesh_offset: read_u32_le(bytes, 32)?,
            texture_unit_count: read_u32_le(bytes, 36)?,
            texture_unit_offset: read_u32_le(bytes, 40)?,
            lod: read_i32_le(bytes, 44)?,
        })
    }
}

impl M2Vertex {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            position: read_vec3(bytes, offset)?,
            weights: read_exact(bytes, offset + 12)?,
            bones: read_exact(bytes, offset + 16)?,
            normal: read_vec3(bytes, offset + 20)?,
            tex_coords: [
                read_vec2(bytes, offset + 32)?,
                read_vec2(bytes, offset + 40)?,
            ],
        })
    }
}

impl M2Texture {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        let texture_type = read_u32_le(bytes, offset)?;
        let flags = read_u32_le(bytes, offset + 4)?;
        let name_len = read_u32_le(bytes, offset + 8)?;
        let name_offset = read_u32_le(bytes, offset + 12)?;
        let filename = if texture_type == 0 && name_len > 0 {
            Some(read_sized_string(bytes, name_offset, name_len)?)
        } else {
            None
        };

        Ok(Self {
            texture_type,
            flags,
            filename,
        })
    }
}

impl M2Submesh {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            id: read_u16_le(bytes, offset)?,
            vertex_start: read_u16_le(bytes, offset + 4)?,
            vertex_count: read_u16_le(bytes, offset + 6)?,
            index_start: read_u16_le(bytes, offset + 8)?,
            index_count: read_u16_le(bytes, offset + 10)?,
            bounding_box_min: read_vec3(bytes, offset + 20)?,
            bounding_box_max: read_vec3(bytes, offset + 32)?,
            radius: read_f32_le(bytes, offset + 44)?,
        })
    }
}

impl M2TextureUnit {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            flags: read_u8(bytes, offset)?,
            priority_plane: read_u8(bytes, offset + 1)?,
            shader_id: read_u16_le(bytes, offset + 2)?,
            submesh: read_u16_le(bytes, offset + 4)?,
            geoset_index: read_u16_le(bytes, offset + 6)?,
            color_index: read_i16_le(bytes, offset + 8)?,
            renderflag_index: read_u16_le(bytes, offset + 10)?,
            material_layer: read_u16_le(bytes, offset + 12)?,
            texture_count: read_u16_le(bytes, offset + 14)?,
            texture_combo_index: read_u16_le(bytes, offset + 16)?,
            texture_coord_combo_index: read_u16_le(bytes, offset + 18)?,
            transparency_combo_index: read_u16_le(bytes, offset + 20)?,
            animation_combo_index: read_u16_le(bytes, offset + 22)?,
        })
    }
}

fn parse_vertices(bytes: &[u8], offset: u32, count: u32) -> FormatResult<Vec<M2Vertex>> {
    parse_array(
        bytes,
        offset,
        count,
        M2_VERTEX_SIZE,
        "M2 vertices",
        M2Vertex::parse,
    )
}

fn parse_textures(bytes: &[u8], offset: u32, count: u32) -> FormatResult<Vec<M2Texture>> {
    parse_array(
        bytes,
        offset,
        count,
        M2_TEXTURE_SIZE,
        "M2 textures",
        M2Texture::parse,
    )
}

fn parse_submeshes(bytes: &[u8], offset: u32, count: u32) -> FormatResult<Vec<M2Submesh>> {
    parse_array(
        bytes,
        offset,
        count,
        M2_SUBMESH_SIZE,
        "M2 skin submeshes",
        M2Submesh::parse,
    )
}

fn parse_texture_units(bytes: &[u8], offset: u32, count: u32) -> FormatResult<Vec<M2TextureUnit>> {
    parse_array(
        bytes,
        offset,
        count,
        M2_TEXTURE_UNIT_SIZE,
        "M2 texture units",
        M2TextureUnit::parse,
    )
}

fn parse_u16_array(
    bytes: &[u8],
    offset: u32,
    count: u32,
    field: &'static str,
) -> FormatResult<Vec<u16>> {
    parse_array(bytes, offset, count, 2, field, read_u16_le)
}

fn parse_array<T>(
    bytes: &[u8],
    offset: u32,
    count: u32,
    element_size: usize,
    field: &'static str,
    parse: impl Fn(&[u8], usize) -> FormatResult<T>,
) -> FormatResult<Vec<T>> {
    if count == 0 {
        return Ok(Vec::new());
    }

    let offset = usize::try_from(offset).map_err(|_| FormatError::InvalidRange { field })?;
    let count = usize::try_from(count).map_err(|_| FormatError::InvalidRange { field })?;
    let byte_len = count
        .checked_mul(element_size)
        .ok_or(FormatError::InvalidRange { field })?;
    ensure_range(bytes, offset, byte_len, field)?;

    (0..count)
        .map(|index| {
            let item_offset = offset
                .checked_add(index * element_size)
                .ok_or(FormatError::InvalidRange { field })?;
            parse(bytes, item_offset)
        })
        .collect()
}

fn read_vec3(bytes: &[u8], offset: usize) -> FormatResult<[f32; 3]> {
    Ok([
        read_f32_le(bytes, offset)?,
        read_f32_le(bytes, offset + 4)?,
        read_f32_le(bytes, offset + 8)?,
    ])
}

fn read_vec2(bytes: &[u8], offset: usize) -> FormatResult<[f32; 2]> {
    Ok([read_f32_le(bytes, offset)?, read_f32_le(bytes, offset + 4)?])
}

fn read_i16_le(bytes: &[u8], offset: usize) -> FormatResult<i16> {
    Ok(i16::from_le_bytes(read_exact(bytes, offset)?))
}

fn read_i32_le(bytes: &[u8], offset: usize) -> FormatResult<i32> {
    Ok(i32::from_le_bytes(read_exact(bytes, offset)?))
}

fn read_sized_string(bytes: &[u8], offset: u32, len: u32) -> FormatResult<String> {
    let offset = usize::try_from(offset).map_err(|_| FormatError::InvalidRange {
        field: "M2 string offset",
    })?;
    let len = usize::try_from(len).map_err(|_| FormatError::InvalidRange {
        field: "M2 string length",
    })?;
    ensure_range(bytes, offset, len, "M2 string")?;

    let mut slice = &bytes[offset..offset + len];
    if slice.last() == Some(&0) {
        slice = &slice[..slice.len() - 1];
    }

    std::str::from_utf8(slice)
        .map(str::to_owned)
        .map_err(|_| FormatError::InvalidUtf8 {
            offset: offset as u32,
        })
}

fn ensure_len(bytes: &[u8], len: usize) -> FormatResult<()> {
    ensure_range(bytes, 0, len, "M2 header")
}

fn ensure_range(bytes: &[u8], offset: usize, len: usize, field: &'static str) -> FormatResult<()> {
    let end = offset
        .checked_add(len)
        .ok_or(FormatError::InvalidRange { field })?;
    if end > bytes.len() {
        return Err(FormatError::UnexpectedEof {
            offset,
            needed: len,
            len: bytes.len(),
        });
    }
    Ok(())
}
