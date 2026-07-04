//! WDT map index file parsing.

use crate::error::{FormatError, FormatResult, read_exact, read_u32_le};

const MPHD_SIZE: usize = 32;
const KNOWN_CHUNK_IDS: [[u8; 4]; 3] = [*b"MVER", *b"MPHD", *b"MAIN"];

/// `MPHD` flag indicating 64x64 8-bit terrain alpha maps.
pub const MPHD_FLAG_BIG_ALPHA: u32 = 0x0004;

/// A raw WDT chunk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WdtChunk {
    /// Four-byte chunk identifier.
    pub id: [u8; 4],
    /// Raw chunk payload.
    pub data: Vec<u8>,
}

/// Parsed WDT chunk container.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WdtFile {
    chunks: Vec<WdtChunk>,
}

/// WDT `MPHD` header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Mphd {
    /// WDT map flags.
    pub flags: u32,
    /// Unknown/reserved WotLK field.
    pub something: u32,
    /// Reserved fields.
    pub unused: [u32; 6],
}

impl WdtFile {
    /// Parse a WDT file into ordered raw chunks.
    pub fn parse(bytes: &[u8]) -> FormatResult<Self> {
        let mut chunks = Vec::new();
        let mut offset = 0;

        while offset < bytes.len() {
            let id = normalize_chunk_id(read_exact::<4>(bytes, offset)?);
            let size = read_u32_le(bytes, offset + 4)?;
            let size = usize::try_from(size).map_err(|_| FormatError::InvalidRange {
                field: "WDT chunk size",
            })?;
            let data_offset = offset.checked_add(8).ok_or(FormatError::InvalidRange {
                field: "WDT chunk data offset",
            })?;
            let next_offset = data_offset
                .checked_add(size)
                .ok_or(FormatError::InvalidRange {
                    field: "WDT chunk end",
                })?;

            if bytes.len() < next_offset {
                return Err(FormatError::UnexpectedEof {
                    offset: data_offset,
                    needed: size,
                    len: bytes.len(),
                });
            }

            chunks.push(WdtChunk {
                id,
                data: bytes[data_offset..next_offset].to_vec(),
            });

            offset = next_offset;
        }

        Ok(Self { chunks })
    }

    /// Return chunks in file order.
    pub fn chunks(&self) -> &[WdtChunk] {
        &self.chunks
    }

    /// Find the first chunk matching `id`.
    pub fn first_chunk(&self, id: [u8; 4]) -> Option<&WdtChunk> {
        self.chunks.iter().find(|chunk| chunk.id == id)
    }

    /// Return `MVER` version when present.
    pub fn version(&self) -> FormatResult<Option<u32>> {
        self.first_chunk(*b"MVER")
            .map(|chunk| read_u32_le(&chunk.data, 0))
            .transpose()
    }

    /// Parse the `MPHD` chunk when present.
    pub fn mphd(&self) -> FormatResult<Option<Mphd>> {
        self.first_chunk(*b"MPHD")
            .map(|chunk| Mphd::parse(&chunk.data))
            .transpose()
    }
}

impl Mphd {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        ensure_len(bytes, MPHD_SIZE)?;
        Ok(Self {
            flags: read_u32_le(bytes, 0)?,
            something: read_u32_le(bytes, 4)?,
            unused: [
                read_u32_le(bytes, 8)?,
                read_u32_le(bytes, 12)?,
                read_u32_le(bytes, 16)?,
                read_u32_le(bytes, 20)?,
                read_u32_le(bytes, 24)?,
                read_u32_le(bytes, 28)?,
            ],
        })
    }
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
