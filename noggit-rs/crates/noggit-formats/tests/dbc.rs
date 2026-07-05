//! DBC parser behavior tests.

use noggit_formats::dbc::{DbcFile, DbcHeader, LiquidTypeTable};
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

fn fixture_liquid_type_dbc() -> Vec<u8> {
    const FIELD_COUNT: usize = 25;
    let mut string_table = vec![0];
    let name_offset = push_string(&mut string_table, "Forest water");
    let texture_offset = push_string(&mut string_table, "XTextures\\river\\lake_a.1.blp");
    let foam_offset = push_string(&mut string_table, "XTextures\\river\\foam_a.1.blp");

    let mut record = vec![0_u8; FIELD_COUNT * 4];
    write_u32(&mut record, 0, 7);
    write_u32(&mut record, 1, name_offset);
    write_u32(&mut record, 3, 0);
    write_u32(&mut record, 14, 3);
    write_u32(&mut record, 15, texture_offset);
    write_u32(&mut record, 16, foam_offset);
    write_f32(&mut record, 23, 1.5);
    write_f32(&mut record, 24, 45.0);

    let mut bytes = Vec::new();
    bytes.extend_from_slice(b"WDBC");
    bytes.extend_from_slice(&1_u32.to_le_bytes());
    bytes.extend_from_slice(&(FIELD_COUNT as u32).to_le_bytes());
    bytes.extend_from_slice(&(record.len() as u32).to_le_bytes());
    bytes.extend_from_slice(&(string_table.len() as u32).to_le_bytes());
    bytes.extend_from_slice(&record);
    bytes.extend_from_slice(&string_table);
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
fn parses_liquid_type_records_for_rendering() -> FormatResult<()> {
    let dbc = DbcFile::parse(&fixture_liquid_type_dbc())?;
    let liquid_types = LiquidTypeTable::parse(&dbc)?;
    let record = liquid_types.get(7).ok_or(FormatError::InvalidRange {
        field: "missing liquid type",
    })?;

    assert_eq!(liquid_types.records().len(), 1);
    assert_eq!(record.id, 7);
    assert_eq!(record.liquid_type, 0);
    assert_eq!(record.shader_type, 3);
    assert_eq!(
        record.texture_filenames[0],
        "XTextures\\river\\lake_a.1.blp"
    );
    assert_eq!(
        record.texture_filenames[1],
        "XTextures\\river\\foam_a.1.blp"
    );
    assert_eq!(record.texture_filenames[2], "");
    assert_eq!(record.animation, [1.5, 45.0]);
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

fn push_string(string_table: &mut Vec<u8>, value: &str) -> u32 {
    let offset = string_table.len() as u32;
    string_table.extend_from_slice(value.as_bytes());
    string_table.push(0);
    offset
}

fn write_u32(bytes: &mut [u8], field: usize, value: u32) {
    let offset = field * 4;
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_f32(bytes: &mut [u8], field: usize, value: f32) {
    write_u32(bytes, field, value.to_bits());
}
