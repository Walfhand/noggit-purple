//! DBC parser behavior tests.

use noggit_formats::dbc::{DbcFile, DbcHeader};
use noggit_formats::{FormatError, FormatResult};

fn fixture_dbc() -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(b"WDBC");
    bytes.extend_from_slice(&2_u32.to_le_bytes());
    bytes.extend_from_slice(&3_u32.to_le_bytes());
    bytes.extend_from_slice(&12_u32.to_le_bytes());
    bytes.extend_from_slice(&12_u32.to_le_bytes());

    bytes.extend_from_slice(&42_u32.to_le_bytes());
    bytes.extend_from_slice(&0_u32.to_le_bytes());
    bytes.extend_from_slice(&100_u32.to_le_bytes());

    bytes.extend_from_slice(&7_u32.to_le_bytes());
    bytes.extend_from_slice(&6_u32.to_le_bytes());
    bytes.extend_from_slice(&200_u32.to_le_bytes());

    bytes.extend_from_slice(b"hello\0world\0");
    bytes
}

#[test]
fn parses_dbc_header_records_and_strings() -> FormatResult<()> {
    let dbc = DbcFile::parse(&fixture_dbc())?;

    assert_eq!(
        dbc.header(),
        DbcHeader {
            record_count: 2,
            field_count: 3,
            record_size: 12,
            string_block_size: 12,
        }
    );
    assert_eq!(dbc.record_count(), 2);

    let first = dbc.record(0)?;
    assert_eq!(first.u32(0)?, 42);
    assert_eq!(first.u32(1)?, 0);
    assert_eq!(first.i32(2)?, 100);
    assert_eq!(dbc.string_at(first.u32(1)?)?, "hello");

    let second = dbc.record(1)?;
    assert_eq!(second.u32(0)?, 7);
    assert_eq!(dbc.string_at(second.u32(1)?)?, "world");
    Ok(())
}

#[test]
fn roundtrips_dbc_bytes_without_layout_drift() -> FormatResult<()> {
    let bytes = fixture_dbc();
    let dbc = DbcFile::parse(&bytes)?;

    assert_eq!(dbc.to_bytes(), bytes);
    Ok(())
}

#[test]
fn rejects_invalid_dbc_magic() {
    let mut bytes = fixture_dbc();
    bytes[0..4].copy_from_slice(b"BDCW");

    assert_eq!(
        parse_error(&bytes),
        FormatError::InvalidMagic {
            expected: *b"WDBC",
            actual: *b"BDCW",
        }
    );
}

#[test]
fn rejects_truncated_dbc_records() {
    let mut bytes = fixture_dbc();
    bytes.truncate(16 + 11);

    assert!(matches!(
        parse_error(&bytes),
        FormatError::UnexpectedEof { .. }
    ));
}

fn parse_error(bytes: &[u8]) -> FormatError {
    match DbcFile::parse(bytes) {
        Ok(_) => FormatError::InvalidRange {
            field: "test expected parse failure",
        },
        Err(err) => err,
    }
}
