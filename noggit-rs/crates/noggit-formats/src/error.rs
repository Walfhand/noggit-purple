//! Shared format parsing errors.

use std::error::Error;
use std::fmt::{Display, Formatter};

/// Result type used by format parsers.
pub type FormatResult<T> = Result<T, FormatError>;

/// Error returned when a binary format cannot be parsed or serialized.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FormatError {
    /// The input ended before the requested amount of data was available.
    UnexpectedEof {
        /// Absolute byte offset requested by the parser.
        offset: usize,
        /// Number of bytes requested at `offset`.
        needed: usize,
        /// Total input length.
        len: usize,
    },
    /// A file signature did not match the expected format.
    InvalidMagic {
        /// Expected four-byte signature.
        expected: [u8; 4],
        /// Actual four-byte signature.
        actual: [u8; 4],
    },
    /// A numeric field would produce an invalid byte range.
    InvalidRange {
        /// Name of the invalid field.
        field: &'static str,
    },
    /// A string field did not contain a terminating NUL byte.
    UnterminatedString {
        /// Offset inside the string table.
        offset: u32,
    },
    /// A string field was not valid UTF-8.
    InvalidUtf8 {
        /// Offset inside the string table.
        offset: u32,
    },
}

impl Display for FormatError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::UnexpectedEof {
                offset,
                needed,
                len,
            } => write!(
                f,
                "unexpected EOF at byte {offset}: needed {needed} bytes, input length is {len}"
            ),
            Self::InvalidMagic { expected, actual } => write!(
                f,
                "invalid magic: expected {:?}, got {:?}",
                String::from_utf8_lossy(expected),
                String::from_utf8_lossy(actual)
            ),
            Self::InvalidRange { field } => write!(f, "invalid byte range for {field}"),
            Self::UnterminatedString { offset } => {
                write!(f, "unterminated string at string-table offset {offset}")
            }
            Self::InvalidUtf8 { offset } => {
                write!(f, "invalid UTF-8 string at string-table offset {offset}")
            }
        }
    }
}

impl Error for FormatError {}

pub(crate) fn read_exact<const N: usize>(bytes: &[u8], offset: usize) -> FormatResult<[u8; N]> {
    let end = offset.checked_add(N).ok_or(FormatError::InvalidRange {
        field: "offset + len",
    })?;

    let slice = bytes.get(offset..end).ok_or(FormatError::UnexpectedEof {
        offset,
        needed: N,
        len: bytes.len(),
    })?;

    let mut out = [0; N];
    out.copy_from_slice(slice);
    Ok(out)
}

pub(crate) fn read_u32_le(bytes: &[u8], offset: usize) -> FormatResult<u32> {
    Ok(u32::from_le_bytes(read_exact(bytes, offset)?))
}

pub(crate) fn read_u16_le(bytes: &[u8], offset: usize) -> FormatResult<u16> {
    Ok(u16::from_le_bytes(read_exact(bytes, offset)?))
}

pub(crate) fn read_f32_le(bytes: &[u8], offset: usize) -> FormatResult<f32> {
    Ok(f32::from_bits(read_u32_le(bytes, offset)?))
}
