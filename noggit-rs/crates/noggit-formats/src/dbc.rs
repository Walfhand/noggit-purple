//! DBC database files.

use crate::error::{FormatError, FormatResult, read_exact, read_u32_le};

/// Parsed DBC header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DbcHeader {
    /// Number of records in the table.
    pub record_count: u32,
    /// Number of 32-bit fields per record.
    pub field_count: u32,
    /// Size of one record in bytes.
    pub record_size: u32,
    /// Size of the trailing string table in bytes.
    pub string_block_size: u32,
}

/// Parsed DBC file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DbcFile {
    header: DbcHeader,
    records: Vec<u8>,
    string_table: Vec<u8>,
}

impl DbcFile {
    /// Parse a DBC file from bytes.
    pub fn parse(bytes: &[u8]) -> FormatResult<Self> {
        let actual = read_exact::<4>(bytes, 0)?;
        if actual != *b"WDBC" {
            return Err(FormatError::InvalidMagic {
                expected: *b"WDBC",
                actual,
            });
        }

        let header = DbcHeader {
            record_count: read_u32_le(bytes, 4)?,
            field_count: read_u32_le(bytes, 8)?,
            record_size: read_u32_le(bytes, 12)?,
            string_block_size: read_u32_le(bytes, 16)?,
        };

        let records_len = checked_usize_mul(
            header.record_count,
            header.record_size,
            "record_count * record_size",
        )?;
        let string_table_offset = checked_usize_add(20, records_len, "dbc records end")?;
        let string_table_len =
            usize::try_from(header.string_block_size).map_err(|_| FormatError::InvalidRange {
                field: "string_block_size",
            })?;
        let file_end = checked_usize_add(
            string_table_offset,
            string_table_len,
            "dbc string table end",
        )?;

        if bytes.len() < file_end {
            return Err(FormatError::UnexpectedEof {
                offset: string_table_offset,
                needed: string_table_len,
                len: bytes.len(),
            });
        }

        Ok(Self {
            header,
            records: bytes[20..string_table_offset].to_vec(),
            string_table: bytes[string_table_offset..file_end].to_vec(),
        })
    }

    /// Return the parsed header.
    pub fn header(&self) -> DbcHeader {
        self.header
    }

    /// Return the number of records.
    pub fn record_count(&self) -> usize {
        self.header.record_count as usize
    }

    /// Return a raw record.
    pub fn record(&self, index: usize) -> FormatResult<DbcRecord<'_>> {
        if index >= self.record_count() {
            return Err(FormatError::InvalidRange {
                field: "record index",
            });
        }

        let record_size =
            usize::try_from(self.header.record_size).map_err(|_| FormatError::InvalidRange {
                field: "record_size",
            })?;
        let start = checked_usize_mul_u(index, record_size, "record offset")?;
        let end = checked_usize_add(start, record_size, "record end")?;

        let bytes = self
            .records
            .get(start..end)
            .ok_or(FormatError::UnexpectedEof {
                offset: start,
                needed: record_size,
                len: self.records.len(),
            })?;

        Ok(DbcRecord { bytes })
    }

    /// Return a string by offset in the string table.
    pub fn string_at(&self, offset: u32) -> FormatResult<&str> {
        let start = usize::try_from(offset).map_err(|_| FormatError::InvalidRange {
            field: "string offset",
        })?;
        let rest = self
            .string_table
            .get(start..)
            .ok_or(FormatError::UnexpectedEof {
                offset: start,
                needed: 1,
                len: self.string_table.len(),
            })?;

        let nul_pos = rest
            .iter()
            .position(|byte| *byte == 0)
            .ok_or(FormatError::UnterminatedString { offset })?;
        let string_bytes = &rest[..nul_pos];

        std::str::from_utf8(string_bytes).map_err(|_| FormatError::InvalidUtf8 { offset })
    }

    /// Serialize back to canonical DBC bytes.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(20 + self.records.len() + self.string_table.len());
        bytes.extend_from_slice(b"WDBC");
        bytes.extend_from_slice(&self.header.record_count.to_le_bytes());
        bytes.extend_from_slice(&self.header.field_count.to_le_bytes());
        bytes.extend_from_slice(&self.header.record_size.to_le_bytes());
        bytes.extend_from_slice(&self.header.string_block_size.to_le_bytes());
        bytes.extend_from_slice(&self.records);
        bytes.extend_from_slice(&self.string_table);
        bytes
    }
}

/// Borrowed DBC record.
#[derive(Debug, Clone, Copy)]
pub struct DbcRecord<'a> {
    bytes: &'a [u8],
}

impl<'a> DbcRecord<'a> {
    /// Read a field as `u32`.
    pub fn u32(&self, field: usize) -> FormatResult<u32> {
        let offset = checked_usize_mul_u(field, 4, "record field offset")?;
        read_u32_le(self.bytes, offset)
    }

    /// Read a field as `i32`.
    pub fn i32(&self, field: usize) -> FormatResult<i32> {
        Ok(self.u32(field)? as i32)
    }

    /// Read a field as `f32`.
    pub fn f32(&self, field: usize) -> FormatResult<f32> {
        Ok(f32::from_bits(self.u32(field)?))
    }
}

fn checked_usize_mul(lhs: u32, rhs: u32, field: &'static str) -> FormatResult<usize> {
    let lhs = usize::try_from(lhs).map_err(|_| FormatError::InvalidRange { field })?;
    let rhs = usize::try_from(rhs).map_err(|_| FormatError::InvalidRange { field })?;
    checked_usize_mul_u(lhs, rhs, field)
}

fn checked_usize_mul_u(lhs: usize, rhs: usize, field: &'static str) -> FormatResult<usize> {
    lhs.checked_mul(rhs)
        .ok_or(FormatError::InvalidRange { field })
}

fn checked_usize_add(lhs: usize, rhs: usize, field: &'static str) -> FormatResult<usize> {
    lhs.checked_add(rhs)
        .ok_or(FormatError::InvalidRange { field })
}
