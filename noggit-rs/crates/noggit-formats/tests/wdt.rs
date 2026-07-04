//! WDT chunk container behavior tests.

use noggit_formats::wdt::{MPHD_FLAG_BIG_ALPHA, WdtFile};
use noggit_formats::{FormatError, FormatResult};

#[test]
fn parses_wdt_mphd_big_alpha_flag() -> FormatResult<()> {
    let wdt = WdtFile::parse(&fixture_wdt())?;

    assert_eq!(wdt.version()?, Some(18));
    let mphd = wdt.mphd()?.ok_or(FormatError::InvalidRange {
        field: "missing MPHD",
    })?;
    assert_eq!(mphd.flags, MPHD_FLAG_BIG_ALPHA | 0x0002 | 0x0008);
    assert_ne!(mphd.flags & MPHD_FLAG_BIG_ALPHA, 0);
    Ok(())
}

#[test]
fn normalizes_reversed_storage_order_wdt_chunk_ids() -> FormatResult<()> {
    let wdt = WdtFile::parse(&fixture_wdt())?;

    assert_eq!(wdt.chunks()[0].id, *b"MVER");
    assert_eq!(wdt.chunks()[1].id, *b"MPHD");
    assert_eq!(wdt.chunks()[2].id, *b"MAIN");
    Ok(())
}

#[test]
fn rejects_truncated_wdt_chunk_payload() {
    let bytes = [b"REVM".as_slice(), &4_u32.to_le_bytes(), &[1, 2]].concat();

    assert!(matches!(
        WdtFile::parse(&bytes),
        Err(FormatError::UnexpectedEof { .. })
    ));
}

fn fixture_wdt() -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&stored_chunk(b"MVER", &18_u32.to_le_bytes()));
    bytes.extend_from_slice(&stored_chunk(b"MPHD", &mphd()));
    bytes.extend_from_slice(&stored_chunk(b"MAIN", &vec![0; 64 * 64 * 8]));
    bytes
}

fn mphd() -> Vec<u8> {
    [MPHD_FLAG_BIG_ALPHA | 0x0002 | 0x0008, 0, 0, 0, 0, 0, 0, 0]
        .into_iter()
        .flat_map(u32::to_le_bytes)
        .collect()
}

fn stored_chunk(id: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&[id[3], id[2], id[1], id[0]]);
    bytes.extend_from_slice(&(data.len() as u32).to_le_bytes());
    bytes.extend_from_slice(data);
    bytes
}
