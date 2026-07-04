//! ADT terrain file container parsing.

use crate::error::{FormatError, FormatResult, read_exact, read_f32_le, read_u16_le, read_u32_le};

const MHDR_SIZE: usize = 64;
const MCIN_ENTRY_COUNT: usize = 256;
const MCIN_ENTRY_SIZE: usize = 16;
const MCNK_HEADER_SIZE: usize = 128;
const MCNK_OUTER_HEADER_SIZE: usize = 8;
const MCLY_ENTRY_SIZE: usize = 16;
const MDDF_ENTRY_SIZE: usize = 0x24;
const MODF_ENTRY_SIZE: usize = 0x40;
const MCNR_NORMAL_BYTE_COUNT: usize = MCNK_VERTEX_HEIGHT_COUNT * 3;
const LEGACY_ALPHA_MAP_SIZE: usize = ALPHA_MAP_SIZE / 2;
const KNOWN_CHUNK_IDS: [[u8; 4]; 25] = [
    *b"MVER", *b"MHDR", *b"MCIN", *b"MTEX", *b"MMDX", *b"MMID", *b"MWMO", *b"MWID", *b"MDDF",
    *b"MODF", *b"MFBO", *b"MH2O", *b"MTXF", *b"MTFX", *b"MCNK", *b"MCVT", *b"MCNR", *b"MCLY",
    *b"MCAL", *b"MCSH", *b"MCCV", *b"MCRF", *b"MCSE", *b"MCLQ", *b"MCBB",
];

/// Number of terrain height vertices stored in an `MCVT` sub-chunk.
pub const MCNK_VERTEX_HEIGHT_COUNT: usize = 9 * 9 + 8 * 8;
/// Width and height of a decoded terrain alpha map.
pub const ALPHA_MAP_SIDE: usize = 64;
/// Number of bytes in a decoded terrain alpha map.
pub const ALPHA_MAP_SIZE: usize = ALPHA_MAP_SIDE * ALPHA_MAP_SIDE;
/// `MCLY` flag indicating that a texture layer uses an alpha map.
pub const MCLY_FLAG_USE_ALPHA: u32 = 0x100;
/// `MCLY` flag indicating that the alpha map is RLE-compressed.
pub const MCLY_FLAG_ALPHA_COMPRESSED: u32 = 0x200;

/// A raw ADT chunk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AdtChunk {
    /// Four-byte chunk identifier.
    pub id: [u8; 4],
    /// Raw chunk payload.
    pub data: Vec<u8>,
}

/// Parsed ADT chunk container.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AdtFile {
    chunks: Vec<AdtChunk>,
}

/// ADT `MHDR` chunk.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Mhdr {
    /// ADT header flags.
    pub flags: u32,
    /// Offset to `MCIN`.
    pub mcin: u32,
    /// Offset to `MTEX`.
    pub mtex: u32,
    /// Offset to `MMDX`.
    pub mmdx: u32,
    /// Offset to `MMID`.
    pub mmid: u32,
    /// Offset to `MWMO`.
    pub mwmo: u32,
    /// Offset to `MWID`.
    pub mwid: u32,
    /// Offset to `MDDF`.
    pub mddf: u32,
    /// Offset to `MODF`.
    pub modf: u32,
    /// Offset to `MFBO`.
    pub mfbo: u32,
    /// Offset to `MH2O`.
    pub mh2o: u32,
    /// Offset to `MTXF`.
    pub mtxf: u32,
    /// Reserved field.
    pub pad4: u32,
    /// Reserved field.
    pub pad5: u32,
    /// Reserved field.
    pub pad6: u32,
    /// Reserved field.
    pub pad7: u32,
}

/// One `MCIN` table entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct McinEntry {
    /// Absolute file offset to the corresponding `MCNK`.
    pub offset: u32,
    /// Size of the corresponding `MCNK`.
    pub size: u32,
    /// Chunk flags.
    pub flags: u32,
    /// Async loading id from the original format.
    pub async_id: u32,
}

/// Header at the start of an `MCNK` terrain chunk.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct McnkHeader {
    /// Raw `MCNK` flags.
    pub flags: u32,
    /// Chunk X index.
    pub ix: u32,
    /// Chunk Y index.
    pub iy: u32,
    /// Number of texture layers.
    pub n_layers: u32,
    /// Number of doodad references.
    pub n_doodad_refs: u32,
    /// Offset to `MCVT`.
    pub ofs_height: u32,
    /// Offset to `MCNR`.
    pub ofs_normal: u32,
    /// Offset to `MCLY`.
    pub ofs_layer: u32,
    /// Offset to `MCRF`.
    pub ofs_refs: u32,
    /// Offset to `MCAL`.
    pub ofs_alpha: u32,
    /// Size of `MCAL`.
    pub size_alpha: u32,
    /// Offset to `MCSH`.
    pub ofs_shadow: u32,
    /// Size of `MCSH`.
    pub size_shadow: u32,
    /// Area id for the chunk.
    pub area_id: u32,
    /// Number of map object references.
    pub n_map_obj_refs: u32,
    /// Terrain hole mask.
    pub holes: u32,
    /// Detail doodad layer mapping.
    pub doodad_mapping: [u16; 8],
    /// Detail doodad exclusion stencil.
    pub doodad_stencil: [u8; 8],
    /// Offset to `MCSE`.
    pub ofs_snd_emitters: u32,
    /// Number of sound emitters.
    pub n_snd_emitters: u32,
    /// Offset to liquid data.
    pub ofs_liquid: u32,
    /// Size of liquid data.
    pub size_liquid: u32,
    /// Chunk Z base position.
    pub zpos: f32,
    /// Chunk X base position.
    pub xpos: f32,
    /// Chunk Y base position.
    pub ypos: f32,
    /// Offset to `MCCV`.
    pub ofs_mccv: u32,
    /// Unused field retained for roundtrip/reference parity.
    pub unused1: u32,
    /// Unused field retained for roundtrip/reference parity.
    pub unused2: u32,
}

/// Parsed top-level `MCNK` terrain chunk.
#[derive(Debug, Clone, PartialEq)]
pub struct Mcnk {
    /// Fixed-size header at the beginning of the `MCNK` payload.
    pub header: McnkHeader,
    data: Vec<u8>,
}

/// One decoded normal from an `MCNR` sub-chunk.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct McnrNormal {
    /// X axis normal component.
    pub x: f32,
    /// Y axis normal component in Noggit render order.
    pub y: f32,
    /// Z axis normal component in Noggit render order.
    pub z: f32,
}

/// One texture layer entry from an `MCLY` sub-chunk.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MclyEntry {
    /// Index into the tile texture filename table.
    pub texture_id: u32,
    /// Raw `MCLY` flags.
    pub flags: u32,
    /// Offset into the `MCAL` payload for this layer alpha map.
    pub ofs_alpha: u32,
    /// Texture effect id.
    pub effect_id: u32,
}

/// Decoded 64x64 alpha map for one texture layer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AlphaMap {
    values: [u8; ALPHA_MAP_SIZE],
}

/// One doodad placement entry from an `MDDF` chunk.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MddfEntry {
    /// Index into the model filename id table.
    pub name_id: u32,
    /// Unique doodad id.
    pub unique_id: u32,
    /// World position.
    pub position: [f32; 3],
    /// Rotation in degrees.
    pub rotation: [f32; 3],
    /// Scale multiplied by 1024 in the original format.
    pub scale: u16,
    /// Raw doodad flags.
    pub flags: u16,
}

/// One WMO placement entry from a `MODF` chunk.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ModfEntry {
    /// Index into the WMO filename id table.
    pub name_id: u32,
    /// Unique WMO id.
    pub unique_id: u32,
    /// World position.
    pub position: [f32; 3],
    /// Rotation in degrees.
    pub rotation: [f32; 3],
    /// Lower bounding-box extent.
    pub lower_extent: [f32; 3],
    /// Upper bounding-box extent.
    pub upper_extent: [f32; 3],
    /// Raw WMO flags.
    pub flags: u16,
    /// WMO doodad set id.
    pub doodad_set: u16,
    /// WMO name set id.
    pub name_set: u16,
    /// Scale multiplied by 1024 in the original format.
    pub scale: u16,
}

/// Resolve an ADT `name_id` through an offset table into a filename block.
pub fn filename_by_name_id<'a>(
    filenames: &'a [String],
    offsets: &[u32],
    name_id: u32,
) -> Option<&'a str> {
    let offset_index = usize::try_from(name_id).ok()?;
    let target_offset = *offsets.get(offset_index)?;
    let mut current_offset = 0u32;

    for filename in filenames {
        if current_offset == target_offset {
            return Some(filename);
        }
        current_offset =
            current_offset.checked_add(u32::try_from(filename.len()).ok()?.checked_add(1)?)?;
    }

    None
}

impl AdtFile {
    /// Parse an ADT file into ordered raw chunks.
    pub fn parse(bytes: &[u8]) -> FormatResult<Self> {
        let mut chunks = Vec::new();
        let mut offset = 0;

        while offset < bytes.len() {
            let id = normalize_chunk_id(read_exact::<4>(bytes, offset)?);
            let size = read_u32_le(bytes, offset + 4)?;
            let size = usize::try_from(size).map_err(|_| FormatError::InvalidRange {
                field: "chunk size",
            })?;
            let data_offset = offset.checked_add(8).ok_or(FormatError::InvalidRange {
                field: "chunk data offset",
            })?;
            let next_offset = data_offset
                .checked_add(size)
                .ok_or(FormatError::InvalidRange { field: "chunk end" })?;

            if bytes.len() < next_offset {
                return Err(FormatError::UnexpectedEof {
                    offset: data_offset,
                    needed: size,
                    len: bytes.len(),
                });
            }

            chunks.push(AdtChunk {
                id,
                data: bytes[data_offset..next_offset].to_vec(),
            });

            offset = next_offset;
        }

        Ok(Self { chunks })
    }

    /// Return chunks in file order.
    pub fn chunks(&self) -> &[AdtChunk] {
        &self.chunks
    }

    /// Find the first chunk matching `id`.
    pub fn first_chunk(&self, id: [u8; 4]) -> Option<&AdtChunk> {
        self.chunks.iter().find(|chunk| chunk.id == id)
    }

    /// Return `MVER` version when present.
    pub fn version(&self) -> FormatResult<Option<u32>> {
        self.first_chunk(*b"MVER")
            .map(|chunk| read_u32_le(&chunk.data, 0))
            .transpose()
    }

    /// Parse the `MHDR` chunk when present.
    pub fn mhdr(&self) -> FormatResult<Option<Mhdr>> {
        self.first_chunk(*b"MHDR")
            .map(|chunk| Mhdr::parse(&chunk.data))
            .transpose()
    }

    /// Parse the `MCIN` chunk when present.
    pub fn mcin_entries(&self) -> FormatResult<Option<Vec<McinEntry>>> {
        self.first_chunk(*b"MCIN")
            .map(|chunk| parse_mcin_entries(&chunk.data))
            .transpose()
    }

    /// Parse every top-level `MCNK` header.
    pub fn mcnk_headers(&self) -> FormatResult<Vec<McnkHeader>> {
        self.chunks
            .iter()
            .filter(|chunk| chunk.id == *b"MCNK")
            .map(|chunk| McnkHeader::parse(&chunk.data))
            .collect()
    }

    /// Parse every top-level `MCNK` terrain chunk.
    pub fn mcnk_chunks(&self) -> FormatResult<Vec<Mcnk>> {
        self.chunks
            .iter()
            .filter(|chunk| chunk.id == *b"MCNK")
            .map(|chunk| Mcnk::parse(&chunk.data))
            .collect()
    }

    /// Parse `MTEX` texture filenames when present.
    pub fn texture_filenames(&self) -> FormatResult<Option<Vec<String>>> {
        self.parse_string_list(*b"MTEX")
    }

    /// Parse `MMDX` model filenames when present.
    pub fn model_filenames(&self) -> FormatResult<Option<Vec<String>>> {
        self.parse_string_list(*b"MMDX")
    }

    /// Parse `MWMO` WMO filenames when present.
    pub fn wmo_filenames(&self) -> FormatResult<Option<Vec<String>>> {
        self.parse_string_list(*b"MWMO")
    }

    /// Parse `MMID` offsets into the `MMDX` filename block when present.
    pub fn model_filename_offsets(&self) -> FormatResult<Option<Vec<u32>>> {
        self.parse_u32_list(*b"MMID", "MMID length")
    }

    /// Parse `MWID` offsets into the `MWMO` filename block when present.
    pub fn wmo_filename_offsets(&self) -> FormatResult<Option<Vec<u32>>> {
        self.parse_u32_list(*b"MWID", "MWID length")
    }

    /// Parse `MDDF` doodad placement entries when present.
    pub fn model_placements(&self) -> FormatResult<Option<Vec<MddfEntry>>> {
        self.first_chunk(*b"MDDF")
            .map(|chunk| parse_mddf_entries(&chunk.data))
            .transpose()
    }

    /// Parse `MODF` WMO placement entries when present.
    pub fn wmo_placements(&self) -> FormatResult<Option<Vec<ModfEntry>>> {
        self.first_chunk(*b"MODF")
            .map(|chunk| parse_modf_entries(&chunk.data))
            .transpose()
    }

    fn parse_string_list(&self, id: [u8; 4]) -> FormatResult<Option<Vec<String>>> {
        self.first_chunk(id)
            .map(|chunk| parse_string_block(&chunk.data))
            .transpose()
    }

    fn parse_u32_list(&self, id: [u8; 4], field: &'static str) -> FormatResult<Option<Vec<u32>>> {
        self.first_chunk(id)
            .map(|chunk| parse_u32_entries(&chunk.data, field))
            .transpose()
    }
}

impl Mhdr {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        ensure_len(bytes, MHDR_SIZE)?;

        Ok(Self {
            flags: read_u32_le(bytes, 0)?,
            mcin: read_u32_le(bytes, 4)?,
            mtex: read_u32_le(bytes, 8)?,
            mmdx: read_u32_le(bytes, 12)?,
            mmid: read_u32_le(bytes, 16)?,
            mwmo: read_u32_le(bytes, 20)?,
            mwid: read_u32_le(bytes, 24)?,
            mddf: read_u32_le(bytes, 28)?,
            modf: read_u32_le(bytes, 32)?,
            mfbo: read_u32_le(bytes, 36)?,
            mh2o: read_u32_le(bytes, 40)?,
            mtxf: read_u32_le(bytes, 44)?,
            pad4: read_u32_le(bytes, 48)?,
            pad5: read_u32_le(bytes, 52)?,
            pad6: read_u32_le(bytes, 56)?,
            pad7: read_u32_le(bytes, 60)?,
        })
    }
}

impl McinEntry {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            offset: read_u32_le(bytes, offset)?,
            size: read_u32_le(bytes, offset + 4)?,
            flags: read_u32_le(bytes, offset + 8)?,
            async_id: read_u32_le(bytes, offset + 12)?,
        })
    }
}

impl McnkHeader {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        ensure_len(bytes, MCNK_HEADER_SIZE)?;

        let mut doodad_mapping = [0; 8];
        for (index, value) in doodad_mapping.iter_mut().enumerate() {
            *value = read_u16_le(bytes, 64 + index * 2)?;
        }

        let mut doodad_stencil = [0; 8];
        doodad_stencil.copy_from_slice(&read_exact::<8>(bytes, 80)?);

        Ok(Self {
            flags: read_u32_le(bytes, 0)?,
            ix: read_u32_le(bytes, 4)?,
            iy: read_u32_le(bytes, 8)?,
            n_layers: read_u32_le(bytes, 12)?,
            n_doodad_refs: read_u32_le(bytes, 16)?,
            ofs_height: read_u32_le(bytes, 20)?,
            ofs_normal: read_u32_le(bytes, 24)?,
            ofs_layer: read_u32_le(bytes, 28)?,
            ofs_refs: read_u32_le(bytes, 32)?,
            ofs_alpha: read_u32_le(bytes, 36)?,
            size_alpha: read_u32_le(bytes, 40)?,
            ofs_shadow: read_u32_le(bytes, 44)?,
            size_shadow: read_u32_le(bytes, 48)?,
            area_id: read_u32_le(bytes, 52)?,
            n_map_obj_refs: read_u32_le(bytes, 56)?,
            holes: read_u32_le(bytes, 60)?,
            doodad_mapping,
            doodad_stencil,
            ofs_snd_emitters: read_u32_le(bytes, 88)?,
            n_snd_emitters: read_u32_le(bytes, 92)?,
            ofs_liquid: read_u32_le(bytes, 96)?,
            size_liquid: read_u32_le(bytes, 100)?,
            zpos: read_f32_le(bytes, 104)?,
            xpos: read_f32_le(bytes, 108)?,
            ypos: read_f32_le(bytes, 112)?,
            ofs_mccv: read_u32_le(bytes, 116)?,
            unused1: read_u32_le(bytes, 120)?,
            unused2: read_u32_le(bytes, 124)?,
        })
    }
}

impl Mcnk {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        ensure_len(bytes, MCNK_HEADER_SIZE)?;

        Ok(Self {
            header: McnkHeader::parse(bytes)?,
            data: bytes.to_vec(),
        })
    }

    /// Return the raw `MCNK` payload, without the outer `MCNK` id and size.
    pub fn raw_data(&self) -> &[u8] {
        &self.data
    }

    /// Return a known sub-chunk by id, using the offsets stored in the `MCNK` header.
    pub fn first_subchunk(&self, id: [u8; 4]) -> FormatResult<Option<AdtChunk>> {
        if id == *b"MCVT" {
            self.subchunk_at(id, self.header.ofs_height, "MCNK.ofs_height")
        } else if id == *b"MCNR" {
            self.subchunk_at(id, self.header.ofs_normal, "MCNK.ofs_normal")
        } else if id == *b"MCLY" {
            self.subchunk_at(id, self.header.ofs_layer, "MCNK.ofs_layer")
        } else if id == *b"MCAL" {
            self.subchunk_at(id, self.header.ofs_alpha, "MCNK.ofs_alpha")
        } else if id == *b"MCSH" {
            self.subchunk_at(id, self.header.ofs_shadow, "MCNK.ofs_shadow")
        } else if id == *b"MCCV" {
            self.subchunk_at(id, self.header.ofs_mccv, "MCNK.ofs_mccv")
        } else {
            Ok(None)
        }
    }

    /// Decode `MCVT` terrain heights when the chunk is present.
    pub fn heights(&self) -> FormatResult<Option<Vec<f32>>> {
        self.first_subchunk(*b"MCVT")?
            .map(|chunk| parse_mcvt_heights(&chunk.data))
            .transpose()
    }

    /// Decode `MCNR` normals in the axis order used by Noggit rendering.
    pub fn normals(&self) -> FormatResult<Option<Vec<McnrNormal>>> {
        self.first_subchunk(*b"MCNR")?
            .map(|chunk| parse_mcnr_normals(&chunk.data))
            .transpose()
    }

    /// Decode `MCLY` texture layer entries when the chunk is present.
    pub fn texture_layers(&self) -> FormatResult<Option<Vec<MclyEntry>>> {
        self.first_subchunk(*b"MCLY")?
            .map(|chunk| parse_mcly_entries(&chunk.data))
            .transpose()
    }

    /// Decode alpha maps referenced by `MCLY` entries.
    pub fn alpha_maps(
        &self,
        use_big_alphamaps: bool,
        do_not_fix_alpha_map: bool,
    ) -> FormatResult<Vec<Option<AlphaMap>>> {
        let Some(layers) = self.texture_layers()? else {
            return Ok(Vec::new());
        };
        let mcal = self.first_subchunk(*b"MCAL")?;

        layers
            .into_iter()
            .map(|layer| {
                if !layer.uses_alpha() {
                    return Ok(None);
                }

                let mcal = mcal.as_ref().ok_or(FormatError::UnexpectedEof {
                    offset: 0,
                    needed: 1,
                    len: 0,
                })?;
                let alpha_offset =
                    usize::try_from(layer.ofs_alpha).map_err(|_| FormatError::InvalidRange {
                        field: "MCLY.ofsAlpha",
                    })?;
                let alpha_bytes =
                    mcal.data
                        .get(alpha_offset..)
                        .ok_or(FormatError::UnexpectedEof {
                            offset: alpha_offset,
                            needed: 1,
                            len: mcal.data.len(),
                        })?;

                let alpha = if use_big_alphamaps {
                    if layer.alpha_compressed() {
                        decode_compressed_alpha(alpha_bytes)?
                    } else {
                        decode_big_alpha(alpha_bytes)?
                    }
                } else {
                    decode_legacy_alpha(alpha_bytes, do_not_fix_alpha_map)?
                };

                Ok(Some(alpha))
            })
            .collect()
    }

    fn subchunk_at(
        &self,
        expected_id: [u8; 4],
        mcnk_offset: u32,
        field: &'static str,
    ) -> FormatResult<Option<AdtChunk>> {
        if mcnk_offset == 0 {
            return Ok(None);
        }

        let raw_offset =
            usize::try_from(mcnk_offset).map_err(|_| FormatError::InvalidRange { field })?;
        let chunk_offset = raw_offset
            .checked_sub(MCNK_OUTER_HEADER_SIZE)
            .ok_or(FormatError::InvalidRange { field })?;
        let actual_id = normalize_chunk_id(read_exact::<4>(&self.data, chunk_offset)?);
        if actual_id != expected_id {
            return Err(FormatError::InvalidMagic {
                expected: expected_id,
                actual: actual_id,
            });
        }

        let size = read_u32_le(&self.data, chunk_offset + 4)?;
        let size = usize::try_from(size).map_err(|_| FormatError::InvalidRange {
            field: "MCNK subchunk size",
        })?;
        let data_offset =
            chunk_offset
                .checked_add(MCNK_OUTER_HEADER_SIZE)
                .ok_or(FormatError::InvalidRange {
                    field: "MCNK subchunk data offset",
                })?;
        let next_offset = data_offset
            .checked_add(size)
            .ok_or(FormatError::InvalidRange {
                field: "MCNK subchunk end",
            })?;

        if self.data.len() < next_offset {
            return Err(FormatError::UnexpectedEof {
                offset: data_offset,
                needed: size,
                len: self.data.len(),
            });
        }

        Ok(Some(AdtChunk {
            id: expected_id,
            data: self.data[data_offset..next_offset].to_vec(),
        }))
    }
}

impl MclyEntry {
    /// Parse a single 16-byte `MCLY` entry.
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            texture_id: read_u32_le(bytes, offset)?,
            flags: read_u32_le(bytes, offset + 4)?,
            ofs_alpha: read_u32_le(bytes, offset + 8)?,
            effect_id: read_u32_le(bytes, offset + 12)?,
        })
    }

    /// Return whether this layer references an alpha map.
    pub fn uses_alpha(&self) -> bool {
        self.flags & MCLY_FLAG_USE_ALPHA != 0
    }

    /// Return whether this layer alpha map is compressed.
    pub fn alpha_compressed(&self) -> bool {
        self.flags & MCLY_FLAG_ALPHA_COMPRESSED != 0
    }
}

impl AlphaMap {
    /// Return decoded alpha values in row-major 64x64 order.
    pub fn as_bytes(&self) -> &[u8; ALPHA_MAP_SIZE] {
        &self.values
    }
}

impl MddfEntry {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            name_id: read_u32_le(bytes, offset)?,
            unique_id: read_u32_le(bytes, offset + 4)?,
            position: read_vec3(bytes, offset + 8)?,
            rotation: read_vec3(bytes, offset + 20)?,
            scale: read_u16_le(bytes, offset + 32)?,
            flags: read_u16_le(bytes, offset + 34)?,
        })
    }
}

impl ModfEntry {
    fn parse(bytes: &[u8], offset: usize) -> FormatResult<Self> {
        Ok(Self {
            name_id: read_u32_le(bytes, offset)?,
            unique_id: read_u32_le(bytes, offset + 4)?,
            position: read_vec3(bytes, offset + 8)?,
            rotation: read_vec3(bytes, offset + 20)?,
            lower_extent: read_vec3(bytes, offset + 32)?,
            upper_extent: read_vec3(bytes, offset + 44)?,
            flags: read_u16_le(bytes, offset + 56)?,
            doodad_set: read_u16_le(bytes, offset + 58)?,
            name_set: read_u16_le(bytes, offset + 60)?,
            scale: read_u16_le(bytes, offset + 62)?,
        })
    }
}

fn parse_mcin_entries(bytes: &[u8]) -> FormatResult<Vec<McinEntry>> {
    let len = MCIN_ENTRY_COUNT * MCIN_ENTRY_SIZE;
    ensure_len(bytes, len)?;

    (0..MCIN_ENTRY_COUNT)
        .map(|index| McinEntry::parse(bytes, index * MCIN_ENTRY_SIZE))
        .collect()
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

fn parse_string_block(bytes: &[u8]) -> FormatResult<Vec<String>> {
    let mut strings = Vec::new();
    let mut offset = 0;

    while offset < bytes.len() {
        let rest = &bytes[offset..];
        let string_offset = checked_string_offset(offset)?;
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
                field: "ADT string block offset",
            })?;
    }

    Ok(strings)
}

fn parse_u32_entries(bytes: &[u8], field: &'static str) -> FormatResult<Vec<u32>> {
    if !bytes.len().is_multiple_of(4) {
        return Err(FormatError::InvalidRange { field });
    }

    (0..bytes.len() / 4)
        .map(|index| read_u32_le(bytes, index * 4))
        .collect()
}

fn parse_mddf_entries(bytes: &[u8]) -> FormatResult<Vec<MddfEntry>> {
    if !bytes.len().is_multiple_of(MDDF_ENTRY_SIZE) {
        return Err(FormatError::InvalidRange {
            field: "MDDF length",
        });
    }

    (0..bytes.len() / MDDF_ENTRY_SIZE)
        .map(|index| MddfEntry::parse(bytes, index * MDDF_ENTRY_SIZE))
        .collect()
}

fn parse_modf_entries(bytes: &[u8]) -> FormatResult<Vec<ModfEntry>> {
    if !bytes.len().is_multiple_of(MODF_ENTRY_SIZE) {
        return Err(FormatError::InvalidRange {
            field: "MODF length",
        });
    }

    (0..bytes.len() / MODF_ENTRY_SIZE)
        .map(|index| ModfEntry::parse(bytes, index * MODF_ENTRY_SIZE))
        .collect()
}

fn read_vec3(bytes: &[u8], offset: usize) -> FormatResult<[f32; 3]> {
    Ok([
        read_f32_le(bytes, offset)?,
        read_f32_le(bytes, offset + 4)?,
        read_f32_le(bytes, offset + 8)?,
    ])
}

fn checked_string_offset(offset: usize) -> FormatResult<u32> {
    u32::try_from(offset).map_err(|_| FormatError::InvalidRange {
        field: "ADT string block offset",
    })
}

fn parse_mcvt_heights(bytes: &[u8]) -> FormatResult<Vec<f32>> {
    ensure_len(bytes, MCNK_VERTEX_HEIGHT_COUNT * 4)?;

    (0..MCNK_VERTEX_HEIGHT_COUNT)
        .map(|index| read_f32_le(bytes, index * 4))
        .collect()
}

fn parse_mcnr_normals(bytes: &[u8]) -> FormatResult<Vec<McnrNormal>> {
    ensure_len(bytes, MCNR_NORMAL_BYTE_COUNT)?;

    Ok((0..MCNK_VERTEX_HEIGHT_COUNT)
        .map(|index| {
            let offset = index * 3;
            McnrNormal {
                x: normalize_mcnr_byte(bytes[offset]),
                y: normalize_mcnr_byte(bytes[offset + 2]),
                z: normalize_mcnr_byte(bytes[offset + 1]),
            }
        })
        .collect())
}

fn normalize_mcnr_byte(byte: u8) -> f32 {
    f32::from(i8::from_ne_bytes([byte])) / 127.0
}

fn parse_mcly_entries(bytes: &[u8]) -> FormatResult<Vec<MclyEntry>> {
    if !bytes.len().is_multiple_of(MCLY_ENTRY_SIZE) {
        return Err(FormatError::InvalidRange {
            field: "MCLY length",
        });
    }

    (0..bytes.len() / MCLY_ENTRY_SIZE)
        .map(|index| MclyEntry::parse(bytes, index * MCLY_ENTRY_SIZE))
        .collect()
}

fn decode_big_alpha(bytes: &[u8]) -> FormatResult<AlphaMap> {
    ensure_len(bytes, ALPHA_MAP_SIZE)?;

    let mut values = [0; ALPHA_MAP_SIZE];
    values.copy_from_slice(&bytes[..ALPHA_MAP_SIZE]);
    Ok(AlphaMap { values })
}

fn decode_compressed_alpha(bytes: &[u8]) -> FormatResult<AlphaMap> {
    let mut values = [0; ALPHA_MAP_SIZE];
    let mut input_offset = 0;
    let mut output_offset = 0;

    while output_offset < ALPHA_MAP_SIZE {
        let entry = *bytes.get(input_offset).ok_or(FormatError::UnexpectedEof {
            offset: input_offset,
            needed: 1,
            len: bytes.len(),
        })?;
        input_offset += 1;

        let count = usize::from(entry & 0x7F);
        if count == 0 {
            continue;
        }

        let next_output = output_offset
            .checked_add(count)
            .ok_or(FormatError::InvalidRange {
                field: "compressed MCAL output",
            })?;
        if next_output > ALPHA_MAP_SIZE {
            return Err(FormatError::InvalidRange {
                field: "compressed MCAL output",
            });
        }

        if entry & 0x80 != 0 {
            let value = *bytes.get(input_offset).ok_or(FormatError::UnexpectedEof {
                offset: input_offset,
                needed: 1,
                len: bytes.len(),
            })?;
            input_offset += 1;
            values[output_offset..next_output].fill(value);
        } else {
            let next_input = input_offset
                .checked_add(count)
                .ok_or(FormatError::InvalidRange {
                    field: "compressed MCAL input",
                })?;
            let input = bytes
                .get(input_offset..next_input)
                .ok_or(FormatError::UnexpectedEof {
                    offset: input_offset,
                    needed: count,
                    len: bytes.len(),
                })?;
            values[output_offset..next_output].copy_from_slice(input);
            input_offset = next_input;
        }

        output_offset = next_output;
    }

    Ok(AlphaMap { values })
}

fn decode_legacy_alpha(bytes: &[u8], do_not_fix_alpha_map: bool) -> FormatResult<AlphaMap> {
    ensure_len(bytes, LEGACY_ALPHA_MAP_SIZE)?;

    let mut values = [0; ALPHA_MAP_SIZE];
    let mut input_offset = 0;

    for x in 0..ALPHA_MAP_SIDE {
        for y in (0..ALPHA_MAP_SIDE).step_by(2) {
            let byte = bytes[input_offset];
            input_offset += 1;
            let lower = byte & 0x0F;
            let upper = byte >> 4;
            values[x * ALPHA_MAP_SIDE + y] = lower | (lower << 4);
            values[x * ALPHA_MAP_SIDE + y + 1] = upper | (upper << 4);
        }
    }

    if !do_not_fix_alpha_map {
        for index in 0..ALPHA_MAP_SIDE {
            values[index * ALPHA_MAP_SIDE + 63] = values[index * ALPHA_MAP_SIDE + 62];
            values[63 * ALPHA_MAP_SIDE + index] = values[62 * ALPHA_MAP_SIDE + index];
        }
        values[63 * ALPHA_MAP_SIDE + 63] = values[62 * ALPHA_MAP_SIDE + 62];
    }

    Ok(AlphaMap { values })
}

fn ensure_len(bytes: &[u8], needed: usize) -> FormatResult<()> {
    if bytes.len() < needed {
        return Err(FormatError::UnexpectedEof {
            offset: 0,
            needed,
            len: bytes.len(),
        });
    }

    Ok(())
}
