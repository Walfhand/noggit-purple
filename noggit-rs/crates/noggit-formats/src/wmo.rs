//! WMO root and group file parsing.

use crate::error::{
    FormatError, FormatResult, read_exact, read_f32_le, read_u8, read_u16_le, read_u32_le,
};

const MOHD_SIZE: usize = 64;
const MOMT_ENTRY_SIZE: usize = 64;
const MOGI_ENTRY_SIZE: usize = 32;
const MOGP_HEADER_SIZE: usize = 68;
const MOPY_ENTRY_SIZE: usize = 2;
const MOBA_ENTRY_SIZE: usize = 24;
const VEC3_SIZE: usize = 12;
const VEC2_SIZE: usize = 8;
const KNOWN_CHUNK_IDS: [[u8; 4]; 36] = [
    *b"MVER", *b"MOHD", *b"MOTX", *b"MOMT", *b"MOGN", *b"MOGI", *b"MOSB", *b"MOPV", *b"MOPT",
    *b"MOPR", *b"MOVV", *b"MOVB", *b"MOLT", *b"MODS", *b"MODN", *b"MODD", *b"MFOG", *b"MOGP",
    *b"MOPY", *b"MOVI", *b"MOVT", *b"MONR", *b"MOTV", *b"MOBA", *b"MOLR", *b"MODR", *b"MOBN",
    *b"MOBR", *b"MPBV", *b"MPBP", *b"MPBI", *b"MPBG", *b"MOCV", *b"MLIQ", *b"MORI", *b"MORB",
];

/// A raw WMO chunk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WmoChunk {
    /// Four-byte chunk identifier.
    pub id: [u8; 4],
    /// Raw chunk payload.
    pub data: Vec<u8>,
}

/// Parsed WMO root file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WmoFile {
    chunks: Vec<WmoChunk>,
}

/// Parsed WMO group file.
#[derive(Debug, Clone, PartialEq)]
pub struct WmoGroupFile {
    chunks: Vec<WmoChunk>,
    header: WmoGroupHeader,
    group_chunks: Vec<WmoChunk>,
}

/// WMO root `MOHD` header.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WmoHeader {
    /// Number of entries in `MOTX`.
    pub texture_count: u32,
    /// Number of WMO group files.
    pub group_count: u32,
    /// Number of portal vertices.
    pub portal_count: u32,
    /// Number of lights.
    pub light_count: u32,
    /// Number of internal doodad model filenames.
    pub model_count: u32,
    /// Number of internal doodad placements.
    pub doodad_count: u32,
    /// Number of doodad sets.
    pub doodad_set_count: u32,
    /// Raw ambient color.
    pub ambient_color: u32,
    /// WMO id used by DBC area lookup.
    pub wmo_id: u32,
    /// Root lower extent.
    pub lower_extent: [f32; 3],
    /// Root upper extent.
    pub upper_extent: [f32; 3],
    /// Raw WMO flags.
    pub flags: u16,
}

/// One root `MOMT` material entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WmoMaterial {
    /// Raw material flags.
    pub flags: u32,
    /// WMO shader id.
    pub shader: u32,
    /// Blend mode.
    pub blend_mode: u32,
    /// Offset into `MOTX` for the primary texture.
    pub texture_offset_1: u32,
    /// Offset into `MOTX` for the secondary texture.
    pub texture_offset_2: u32,
}

/// One root `MOGI` group info entry.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WmoGroupInfo {
    /// Raw group flags.
    pub flags: u32,
    /// Group maximum extent.
    pub bounding_box_max: [f32; 3],
    /// Group minimum extent.
    pub bounding_box_min: [f32; 3],
    /// Offset into `MOGN`.
    pub name_offset: i32,
}

/// WMO group `MOGP` header.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WmoGroupHeader {
    /// Offset into root `MOGN`.
    pub group_name: u32,
    /// Offset into root `MOGN`.
    pub descriptive_group_name: u32,
    /// Raw group flags.
    pub flags: u32,
    /// Group lower extent.
    pub lower_extent: [f32; 3],
    /// Group upper extent.
    pub upper_extent: [f32; 3],
    /// First portal index.
    pub portal_start: u16,
    /// Portal count.
    pub portal_count: u16,
    /// Transparent render batch count.
    pub transparent_batch_count: u16,
    /// Interior render batch count.
    pub interior_batch_count: u16,
    /// Exterior render batch count.
    pub exterior_batch_count: u16,
    /// Reserved/batch-type field.
    pub padding_or_batch_type_d: u16,
    /// Fog indices.
    pub fogs: [u8; 4],
    /// Liquid type.
    pub group_liquid: u32,
    /// Group id.
    pub id: u32,
    /// Unknown WotLK field.
    pub unk2: i32,
    /// Unknown WotLK field.
    pub unk3: i32,
}

/// One `MOPY` triangle material entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WmoTriangleMaterial {
    /// Raw per-triangle flags.
    pub flags: u8,
    /// Material index.
    pub texture: u8,
}

/// One `MOBA` render batch entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WmoBatch {
    /// Unknown/reserved batch fields.
    pub unused: [i16; 6],
    /// First index in `MOVI`.
    pub index_start: u32,
    /// Number of indices.
    pub index_count: u16,
    /// First vertex affected by the batch.
    pub vertex_start: u16,
    /// Last vertex affected by the batch.
    pub vertex_end: u16,
    /// Raw batch flags.
    pub flags: u8,
    /// Material index.
    pub texture: u8,
}

impl WmoFile {
    /// Parse a WMO root file into ordered raw chunks.
    pub fn parse(bytes: &[u8]) -> FormatResult<Self> {
        Ok(Self {
            chunks: parse_chunks(bytes, "WMO chunk size")?,
        })
    }

    /// Return chunks in file order.
    pub fn chunks(&self) -> &[WmoChunk] {
        &self.chunks
    }

    /// Find the first chunk matching `id`.
    pub fn first_chunk(&self, id: [u8; 4]) -> Option<&WmoChunk> {
        self.chunks.iter().find(|chunk| chunk.id == id)
    }

    /// Return `MVER` version when present.
    pub fn version(&self) -> FormatResult<Option<u32>> {
        self.first_chunk(*b"MVER")
            .map(|chunk| read_u32_le(&chunk.data, 0))
            .transpose()
    }

    /// Parse `MOHD` when present.
    pub fn header(&self) -> FormatResult<Option<WmoHeader>> {
        self.first_chunk(*b"MOHD")
            .map(|chunk| WmoHeader::parse(&chunk.data))
            .transpose()
    }

    /// Parse the root `MOTX` texture filename block when present.
    pub fn texture_filenames(&self) -> FormatResult<Option<Vec<String>>> {
        self.first_chunk(*b"MOTX")
            .map(|chunk| parse_string_block(&chunk.data))
            .transpose()
    }

    /// Parse root `MOMT` materials when present.
    pub fn materials(&self) -> FormatResult<Option<Vec<WmoMaterial>>> {
        self.first_chunk(*b"MOMT")
            .map(|chunk| parse_materials(&chunk.data))
            .transpose()
    }

    /// Parse root `MOGI` group info entries when present.
    pub fn group_infos(&self) -> FormatResult<Option<Vec<WmoGroupInfo>>> {
        self.first_chunk(*b"MOGI")
            .map(|chunk| parse_group_infos(&chunk.data))
            .transpose()
    }
}

impl WmoGroupFile {
    /// Parse a WMO group file. The `MOGP` payload owns the group's child chunks.
    pub fn parse(bytes: &[u8]) -> FormatResult<Self> {
        let chunks = parse_chunks(bytes, "WMO group chunk size")?;
        let mogp =
            chunks
                .iter()
                .find(|chunk| chunk.id == *b"MOGP")
                .ok_or(FormatError::InvalidRange {
                    field: "missing MOGP",
                })?;
        let header = WmoGroupHeader::parse(&mogp.data)?;
        let group_payload =
            mogp.data
                .get(MOGP_HEADER_SIZE..)
                .ok_or(FormatError::UnexpectedEof {
                    offset: mogp.data.len(),
                    needed: MOGP_HEADER_SIZE.saturating_sub(mogp.data.len()),
                    len: mogp.data.len(),
                })?;
        let group_chunks = parse_chunks(group_payload, "MOGP child chunk size")?;

        Ok(Self {
            chunks,
            header,
            group_chunks,
        })
    }

    /// Return top-level chunks in file order.
    pub fn chunks(&self) -> &[WmoChunk] {
        &self.chunks
    }

    /// Return the parsed `MOGP` header.
    pub fn header(&self) -> WmoGroupHeader {
        self.header
    }

    /// Find the first `MOGP` child chunk matching `id`.
    pub fn first_group_chunk(&self, id: [u8; 4]) -> Option<&WmoChunk> {
        self.group_chunks.iter().find(|chunk| chunk.id == id)
    }

    /// Return `MVER` version when present.
    pub fn version(&self) -> FormatResult<Option<u32>> {
        self.chunks
            .iter()
            .find(|chunk| chunk.id == *b"MVER")
            .map(|chunk| read_u32_le(&chunk.data, 0))
            .transpose()
    }

    /// Parse per-triangle material info when present.
    pub fn triangle_materials(&self) -> FormatResult<Option<Vec<WmoTriangleMaterial>>> {
        self.first_group_chunk(*b"MOPY")
            .map(|chunk| parse_triangle_materials(&chunk.data))
            .transpose()
    }

    /// Parse triangle indices when present.
    pub fn indices(&self) -> FormatResult<Option<Vec<u16>>> {
        self.first_group_chunk(*b"MOVI")
            .map(|chunk| parse_u16_list(&chunk.data, "MOVI length"))
            .transpose()
    }

    /// Parse raw WMO vertices when present.
    pub fn vertices(&self) -> FormatResult<Option<Vec<[f32; 3]>>> {
        self.first_group_chunk(*b"MOVT")
            .map(|chunk| parse_vec3_list(&chunk.data, "MOVT length"))
            .transpose()
    }

    /// Parse raw WMO normals when present.
    pub fn normals(&self) -> FormatResult<Option<Vec<[f32; 3]>>> {
        self.first_group_chunk(*b"MONR")
            .map(|chunk| parse_vec3_list(&chunk.data, "MONR length"))
            .transpose()
    }

    /// Parse texture coordinates when present.
    pub fn tex_coords(&self) -> FormatResult<Option<Vec<[f32; 2]>>> {
        self.first_group_chunk(*b"MOTV")
            .map(|chunk| parse_vec2_list(&chunk.data, "MOTV length"))
            .transpose()
    }

    /// Parse render batches when present.
    pub fn batches(&self) -> FormatResult<Option<Vec<WmoBatch>>> {
        self.first_group_chunk(*b"MOBA")
            .map(|chunk| parse_batches(&chunk.data))
            .transpose()
    }
}

impl WmoHeader {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        ensure_len(bytes, MOHD_SIZE)?;

        Ok(Self {
            texture_count: read_u32_le(bytes, 0)?,
            group_count: read_u32_le(bytes, 4)?,
            portal_count: read_u32_le(bytes, 8)?,
            light_count: read_u32_le(bytes, 12)?,
            model_count: read_u32_le(bytes, 16)?,
            doodad_count: read_u32_le(bytes, 20)?,
            doodad_set_count: read_u32_le(bytes, 24)?,
            ambient_color: read_u32_le(bytes, 28)?,
            wmo_id: read_u32_le(bytes, 32)?,
            lower_extent: read_vec3(bytes, 36)?,
            upper_extent: read_vec3(bytes, 48)?,
            flags: read_u16_le(bytes, 60)?,
        })
    }
}

impl WmoMaterial {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            flags: read_u32_le(bytes, offset)?,
            shader: read_u32_le(bytes, offset + 4)?,
            blend_mode: read_u32_le(bytes, offset + 8)?,
            texture_offset_1: read_u32_le(bytes, offset + 12)?,
            texture_offset_2: read_u32_le(bytes, offset + 24)?,
        })
    }
}

impl WmoGroupInfo {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            flags: read_u32_le(bytes, offset)?,
            bounding_box_max: read_vec3(bytes, offset + 4)?,
            bounding_box_min: read_vec3(bytes, offset + 16)?,
            name_offset: read_i32_le(bytes, offset + 28)?,
        })
    }
}

impl WmoGroupHeader {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        ensure_len(bytes, MOGP_HEADER_SIZE)?;

        Ok(Self {
            group_name: read_u32_le(bytes, 0)?,
            descriptive_group_name: read_u32_le(bytes, 4)?,
            flags: read_u32_le(bytes, 8)?,
            lower_extent: read_vec3(bytes, 12)?,
            upper_extent: read_vec3(bytes, 24)?,
            portal_start: read_u16_le(bytes, 36)?,
            portal_count: read_u16_le(bytes, 38)?,
            transparent_batch_count: read_u16_le(bytes, 40)?,
            interior_batch_count: read_u16_le(bytes, 42)?,
            exterior_batch_count: read_u16_le(bytes, 44)?,
            padding_or_batch_type_d: read_u16_le(bytes, 46)?,
            fogs: read_exact::<4>(bytes, 48)?,
            group_liquid: read_u32_le(bytes, 52)?,
            id: read_u32_le(bytes, 56)?,
            unk2: read_i32_le(bytes, 60)?,
            unk3: read_i32_le(bytes, 64)?,
        })
    }
}

impl WmoBatch {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            unused: [
                read_i16_le(bytes, offset)?,
                read_i16_le(bytes, offset + 2)?,
                read_i16_le(bytes, offset + 4)?,
                read_i16_le(bytes, offset + 6)?,
                read_i16_le(bytes, offset + 8)?,
                read_i16_le(bytes, offset + 10)?,
            ],
            index_start: read_u32_le(bytes, offset + 12)?,
            index_count: read_u16_le(bytes, offset + 16)?,
            vertex_start: read_u16_le(bytes, offset + 18)?,
            vertex_end: read_u16_le(bytes, offset + 20)?,
            flags: read_u8(bytes, offset + 22)?,
            texture: read_u8(bytes, offset + 23)?,
        })
    }
}

fn parse_chunks(bytes: &[u8], size_field: &'static str) -> FormatResult<Vec<WmoChunk>> {
    let mut chunks = Vec::new();
    let mut offset = 0usize;

    while offset < bytes.len() {
        let id = normalize_chunk_id(read_exact::<4>(bytes, offset)?);
        let size = read_u32_le(bytes, offset + 4)?;
        let size =
            usize::try_from(size).map_err(|_| FormatError::InvalidRange { field: size_field })?;
        let data_offset = offset.checked_add(8).ok_or(FormatError::InvalidRange {
            field: "WMO chunk data offset",
        })?;
        let next_offset = data_offset
            .checked_add(size)
            .ok_or(FormatError::InvalidRange {
                field: "WMO chunk end",
            })?;

        if bytes.len() < next_offset {
            return Err(FormatError::UnexpectedEof {
                offset: data_offset,
                needed: size,
                len: bytes.len(),
            });
        }

        chunks.push(WmoChunk {
            id,
            data: bytes[data_offset..next_offset].to_vec(),
        });
        offset = next_offset;
    }

    Ok(chunks)
}

fn parse_materials(bytes: &[u8]) -> FormatResult<Vec<WmoMaterial>> {
    if !bytes.len().is_multiple_of(MOMT_ENTRY_SIZE) {
        return Err(FormatError::InvalidRange {
            field: "MOMT length",
        });
    }

    (0..bytes.len() / MOMT_ENTRY_SIZE)
        .map(|index| WmoMaterial::parse(bytes, index * MOMT_ENTRY_SIZE))
        .collect()
}

fn parse_group_infos(bytes: &[u8]) -> FormatResult<Vec<WmoGroupInfo>> {
    if !bytes.len().is_multiple_of(MOGI_ENTRY_SIZE) {
        return Err(FormatError::InvalidRange {
            field: "MOGI length",
        });
    }

    (0..bytes.len() / MOGI_ENTRY_SIZE)
        .map(|index| WmoGroupInfo::parse(bytes, index * MOGI_ENTRY_SIZE))
        .collect()
}

fn parse_triangle_materials(bytes: &[u8]) -> FormatResult<Vec<WmoTriangleMaterial>> {
    if !bytes.len().is_multiple_of(MOPY_ENTRY_SIZE) {
        return Err(FormatError::InvalidRange {
            field: "MOPY length",
        });
    }

    (0..bytes.len() / MOPY_ENTRY_SIZE)
        .map(|index| {
            let offset = index * MOPY_ENTRY_SIZE;
            Ok(WmoTriangleMaterial {
                flags: read_u8(bytes, offset)?,
                texture: read_u8(bytes, offset + 1)?,
            })
        })
        .collect()
}

fn parse_batches(bytes: &[u8]) -> FormatResult<Vec<WmoBatch>> {
    if !bytes.len().is_multiple_of(MOBA_ENTRY_SIZE) {
        return Err(FormatError::InvalidRange {
            field: "MOBA length",
        });
    }

    (0..bytes.len() / MOBA_ENTRY_SIZE)
        .map(|index| WmoBatch::parse(bytes, index * MOBA_ENTRY_SIZE))
        .collect()
}

fn parse_u16_list(bytes: &[u8], field: &'static str) -> FormatResult<Vec<u16>> {
    if !bytes.len().is_multiple_of(2) {
        return Err(FormatError::InvalidRange { field });
    }

    (0..bytes.len() / 2)
        .map(|index| read_u16_le(bytes, index * 2))
        .collect()
}

fn parse_vec3_list(bytes: &[u8], field: &'static str) -> FormatResult<Vec<[f32; 3]>> {
    if !bytes.len().is_multiple_of(VEC3_SIZE) {
        return Err(FormatError::InvalidRange { field });
    }

    (0..bytes.len() / VEC3_SIZE)
        .map(|index| read_vec3(bytes, index * VEC3_SIZE))
        .collect()
}

fn parse_vec2_list(bytes: &[u8], field: &'static str) -> FormatResult<Vec<[f32; 2]>> {
    if !bytes.len().is_multiple_of(VEC2_SIZE) {
        return Err(FormatError::InvalidRange { field });
    }

    (0..bytes.len() / VEC2_SIZE)
        .map(|index| {
            let offset = index * VEC2_SIZE;
            Ok([read_f32_le(bytes, offset)?, read_f32_le(bytes, offset + 4)?])
        })
        .collect()
}

fn parse_string_block(bytes: &[u8]) -> FormatResult<Vec<String>> {
    let mut strings = Vec::new();
    let mut offset = 0usize;

    while offset < bytes.len() {
        let rest = &bytes[offset..];
        let string_offset = u32::try_from(offset).map_err(|_| FormatError::InvalidRange {
            field: "WMO string block offset",
        })?;
        let nul_pos =
            rest.iter()
                .position(|byte| *byte == 0)
                .ok_or(FormatError::UnterminatedString {
                    offset: string_offset,
                })?;
        let string_bytes = &rest[..nul_pos];
        let value = std::str::from_utf8(string_bytes)
            .map_err(|_| FormatError::InvalidUtf8 {
                offset: string_offset,
            })?
            .to_owned();

        strings.push(value);
        offset = offset
            .checked_add(nul_pos + 1)
            .ok_or(FormatError::InvalidRange {
                field: "WMO string block offset",
            })?;
    }

    Ok(strings)
}

fn read_vec3(bytes: &[u8], offset: usize) -> FormatResult<[f32; 3]> {
    Ok([
        read_f32_le(bytes, offset)?,
        read_f32_le(bytes, offset + 4)?,
        read_f32_le(bytes, offset + 8)?,
    ])
}

fn read_i32_le(bytes: &[u8], offset: usize) -> FormatResult<i32> {
    Ok(i32::from_le_bytes(read_exact(bytes, offset)?))
}

fn read_i16_le(bytes: &[u8], offset: usize) -> FormatResult<i16> {
    Ok(i16::from_le_bytes(read_exact(bytes, offset)?))
}

fn normalize_chunk_id(id: [u8; 4]) -> [u8; 4] {
    if is_known_chunk_id(id) {
        id
    } else {
        let reversed = reverse_chunk_id(id);
        if is_known_chunk_id(reversed) {
            reversed
        } else {
            id
        }
    }
}

fn is_known_chunk_id(id: [u8; 4]) -> bool {
    KNOWN_CHUNK_IDS.contains(&id)
}

fn reverse_chunk_id(id: [u8; 4]) -> [u8; 4] {
    [id[3], id[2], id[1], id[0]]
}

fn ensure_len(bytes: &[u8], len: usize) -> FormatResult<()> {
    if bytes.len() < len {
        Err(FormatError::UnexpectedEof {
            offset: bytes.len(),
            needed: len - bytes.len(),
            len: bytes.len(),
        })
    } else {
        Ok(())
    }
}
