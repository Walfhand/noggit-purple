//! ADT chunk container behavior tests.

use noggit_formats::adt::{
    ALPHA_MAP_SIZE, AdtFile, MCLY_FLAG_ALPHA_COMPRESSED, MCLY_FLAG_USE_ALPHA,
    MCNK_VERTEX_HEIGHT_COUNT, Mcnk, MddfEntry, filename_by_name_id,
};
use noggit_formats::{FormatError, FormatResult};

fn chunk(id: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(id);
    bytes.extend_from_slice(&(data.len() as u32).to_le_bytes());
    bytes.extend_from_slice(data);
    bytes
}

fn stored_chunk(id: &[u8; 4], data: &[u8]) -> Vec<u8> {
    chunk(&reverse_id(id), data)
}

fn fixture_adt() -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&chunk(b"MVER", &18_u32.to_le_bytes()));
    bytes.extend_from_slice(&chunk(b"MHDR", &fixture_mhdr()));
    bytes.extend_from_slice(&chunk(b"MCIN", &fixture_mcin()));
    bytes.extend_from_slice(&chunk(b"MCNK", &fixture_mcnk()));
    bytes
}

fn fixture_stored_order_adt() -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&stored_chunk(b"MVER", &18_u32.to_le_bytes()));
    bytes.extend_from_slice(&stored_chunk(
        b"MTEX",
        &string_block(&["tiles/foo.blp", "tiles/bar.blp"]),
    ));
    bytes.extend_from_slice(&stored_chunk(b"MCNK", &fixture_stored_order_mcnk()));
    bytes
}

fn fixture_mhdr() -> Vec<u8> {
    [
        0x0000_0005_u32,
        0x0000_0040,
        0x0000_1040,
        0x0000_2040,
        0x0000_3040,
        0x0000_4040,
        0x0000_5040,
        0x0000_6040,
        0x0000_7040,
        0x0000_8040,
        0x0000_9040,
        0x0000_A040,
        0,
        0,
        0,
        0,
    ]
    .into_iter()
    .flat_map(u32::to_le_bytes)
    .collect()
}

fn fixture_mcin() -> Vec<u8> {
    let mut bytes = vec![0; 256 * 16];
    write_u32(&mut bytes, 0, 0x100);
    write_u32(&mut bytes, 4, 0x200);
    write_u32(&mut bytes, 8, 0x3);
    write_u32(&mut bytes, 12, 7);
    write_u32(&mut bytes, 16, 0x300);
    write_u32(&mut bytes, 20, 0x400);
    bytes
}

fn fixture_mcnk_header() -> Vec<u8> {
    let mut bytes = vec![0; 128];
    let u32_fields = [
        0x0001_0041_u32,
        4,
        9,
        0,
        2,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        617,
        11,
        0xAA55,
    ];
    for (index, value) in u32_fields.into_iter().enumerate() {
        write_u32(&mut bytes, index * 4, value);
    }

    for index in 0..8 {
        write_u16(&mut bytes, 64 + index * 2, 10 + index as u16);
        bytes[80 + index] = 1 + index as u8;
    }

    write_u32(&mut bytes, 88, 0x300);
    write_u32(&mut bytes, 92, 5);
    write_u32(&mut bytes, 96, 0x400);
    write_u32(&mut bytes, 100, 0x80);
    write_f32(&mut bytes, 104, 1.25);
    write_f32(&mut bytes, 108, 2.5);
    write_f32(&mut bytes, 112, 3.75);
    write_u32(&mut bytes, 116, 0x500);
    write_u32(&mut bytes, 120, 0xDEAD);
    write_u32(&mut bytes, 124, 0xBEEF);

    bytes
}

fn fixture_mcnk() -> Vec<u8> {
    let mut bytes = fixture_mcnk_header();
    write_u32(&mut bytes, 12, 3);
    push_mcnk_subchunk(&mut bytes, 20, b"MCVT", &fixture_mcvt());
    push_mcnk_subchunk(&mut bytes, 24, b"MCNR", &fixture_mcnr());
    bytes.extend_from_slice(&[0xEE; 13]);
    push_mcnk_subchunk(&mut bytes, 28, b"MCLY", &fixture_mcly());

    let mcal = fixture_mcal();
    write_u32(&mut bytes, 40, (8 + mcal.len()) as u32);
    push_mcnk_subchunk(&mut bytes, 36, b"MCAL", &mcal);

    bytes
}

fn fixture_stored_order_mcnk() -> Vec<u8> {
    let mut bytes = fixture_mcnk_header();
    write_u32(&mut bytes, 12, 1);
    push_stored_mcnk_subchunk(&mut bytes, 20, b"MCVT", &fixture_mcvt());
    bytes
}

fn fixture_mcvt() -> Vec<u8> {
    (0..MCNK_VERTEX_HEIGHT_COUNT)
        .flat_map(|index| ((index as f32) * 0.25).to_le_bytes())
        .collect()
}

fn fixture_mcnr() -> Vec<u8> {
    let mut bytes = Vec::with_capacity(MCNK_VERTEX_HEIGHT_COUNT * 3);
    for index in 0..MCNK_VERTEX_HEIGHT_COUNT {
        bytes.push(127);
        bytes.push(0);
        bytes.push(if index % 2 == 0 { 64 } else { 32 });
    }
    bytes
}

fn fixture_mcly() -> Vec<u8> {
    let mut bytes = Vec::new();
    push_mcly(&mut bytes, 4, 0, 0, 0xFFFF_FFFF);
    push_mcly(&mut bytes, 9, MCLY_FLAG_USE_ALPHA, 0, 17);
    push_mcly(
        &mut bytes,
        12,
        MCLY_FLAG_USE_ALPHA | MCLY_FLAG_ALPHA_COMPRESSED,
        ALPHA_MAP_SIZE as u32,
        99,
    );
    bytes
}

fn fixture_mcal() -> Vec<u8> {
    let mut bytes: Vec<u8> = (0..ALPHA_MAP_SIZE)
        .map(|index| (index % 251) as u8)
        .collect();
    for _ in 0..64 {
        bytes.push(0x80 | 64);
        bytes.push(7);
    }
    bytes
}

fn fixture_legacy_alpha_mcnk() -> Vec<u8> {
    let mut bytes = fixture_mcnk_header();
    write_u32(&mut bytes, 12, 2);

    let mut mcly = Vec::new();
    push_mcly(&mut mcly, 4, 0, 0, 0xFFFF_FFFF);
    push_mcly(&mut mcly, 9, MCLY_FLAG_USE_ALPHA, 0, 17);
    push_mcnk_subchunk(&mut bytes, 28, b"MCLY", &mcly);

    let mcal = vec![0x21; ALPHA_MAP_SIZE / 2];
    write_u32(&mut bytes, 40, (8 + mcal.len()) as u32);
    push_mcnk_subchunk(&mut bytes, 36, b"MCAL", &mcal);

    bytes
}

fn fixture_asset_adt() -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&chunk(
        b"MTEX",
        &string_block(&["tiles/foo.blp", "tiles/bar.blp"]),
    ));
    bytes.extend_from_slice(&chunk(
        b"MMDX",
        &string_block(&["models/tree.m2", "models/rock.m2"]),
    ));
    bytes.extend_from_slice(&chunk(b"MMID", &u32_table(&[0, 15])));
    bytes.extend_from_slice(&chunk(b"MWMO", &string_block(&["world/building.wmo"])));
    bytes.extend_from_slice(&chunk(b"MWID", &u32_table(&[0])));
    bytes.extend_from_slice(&chunk(b"MDDF", &mddf_entry()));
    bytes.extend_from_slice(&chunk(b"MODF", &modf_entry()));
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

fn u32_table(values: &[u32]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect()
}

fn mddf_entry() -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&1_u32.to_le_bytes());
    bytes.extend_from_slice(&77_u32.to_le_bytes());
    push_vec3(&mut bytes, [1.0, 2.0, 3.0]);
    push_vec3(&mut bytes, [10.0, 20.0, 30.0]);
    bytes.extend_from_slice(&1536_u16.to_le_bytes());
    bytes.extend_from_slice(&3_u16.to_le_bytes());
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
    bytes.extend_from_slice(&4_u16.to_le_bytes());
    bytes.extend_from_slice(&5_u16.to_le_bytes());
    bytes.extend_from_slice(&6_u16.to_le_bytes());
    bytes.extend_from_slice(&2048_u16.to_le_bytes());
    bytes
}

fn push_vec3(bytes: &mut Vec<u8>, values: [f32; 3]) {
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
}

fn push_mcnk_subchunk(bytes: &mut Vec<u8>, offset_field: usize, id: &[u8; 4], data: &[u8]) {
    let offset = (bytes.len() + 8) as u32;
    write_u32(bytes, offset_field, offset);
    bytes.extend_from_slice(&chunk(id, data));
}

fn push_stored_mcnk_subchunk(bytes: &mut Vec<u8>, offset_field: usize, id: &[u8; 4], data: &[u8]) {
    let offset = (bytes.len() + 8) as u32;
    write_u32(bytes, offset_field, offset);
    bytes.extend_from_slice(&stored_chunk(id, data));
}

fn push_mcly(bytes: &mut Vec<u8>, texture_id: u32, flags: u32, ofs_alpha: u32, effect_id: u32) {
    bytes.extend_from_slice(&texture_id.to_le_bytes());
    bytes.extend_from_slice(&flags.to_le_bytes());
    bytes.extend_from_slice(&ofs_alpha.to_le_bytes());
    bytes.extend_from_slice(&effect_id.to_le_bytes());
}

fn reverse_id(id: &[u8; 4]) -> [u8; 4] {
    [id[3], id[2], id[1], id[0]]
}

#[test]
fn parses_adt_chunks_in_file_order() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;

    assert_eq!(adt.chunks().len(), 4);
    assert_eq!(adt.chunks()[0].id, *b"MVER");
    assert_eq!(adt.chunks()[1].id, *b"MHDR");
    assert_eq!(adt.chunks()[2].id, *b"MCIN");
    assert_eq!(adt.chunks()[3].id, *b"MCNK");
    Ok(())
}

#[test]
fn normalizes_reversed_storage_order_chunk_ids() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_stored_order_adt())?;

    assert_eq!(adt.chunks()[0].id, *b"MVER");
    assert_eq!(adt.chunks()[1].id, *b"MTEX");
    assert_eq!(adt.chunks()[2].id, *b"MCNK");
    assert_eq!(adt.version()?, Some(18));
    assert_eq!(
        adt.texture_filenames()?.ok_or(FormatError::InvalidRange {
            field: "missing MTEX",
        })?,
        vec!["tiles/foo.blp", "tiles/bar.blp"]
    );

    let mcnk = first_mcnk(&adt)?;
    assert_eq!(
        mcnk.first_subchunk(*b"MCVT")?
            .ok_or(FormatError::InvalidRange {
                field: "missing MCVT",
            })?
            .id,
        *b"MCVT"
    );
    assert_eq!(
        mcnk.heights()?.ok_or(FormatError::InvalidRange {
            field: "missing MCVT heights",
        })?[144],
        36.0
    );
    Ok(())
}

#[test]
fn exposes_adt_version_from_mver() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;

    assert_eq!(adt.version()?, Some(18));
    Ok(())
}

#[test]
fn finds_first_chunk_by_id() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;

    assert_eq!(
        adt.first_chunk(*b"MHDR")
            .ok_or(FormatError::InvalidRange {
                field: "missing MHDR chunk",
            })?
            .data
            .len(),
        64
    );
    assert!(adt.first_chunk(*b"MWMO").is_none());
    Ok(())
}

#[test]
fn parses_mhdr_offsets() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;
    let mhdr = adt.mhdr()?.ok_or(FormatError::InvalidRange {
        field: "missing MHDR chunk",
    })?;

    assert_eq!(mhdr.flags, 0x0000_0005);
    assert_eq!(mhdr.mcin, 0x0000_0040);
    assert_eq!(mhdr.mtex, 0x0000_1040);
    assert_eq!(mhdr.mmdx, 0x0000_2040);
    assert_eq!(mhdr.mmid, 0x0000_3040);
    assert_eq!(mhdr.mwmo, 0x0000_4040);
    assert_eq!(mhdr.mwid, 0x0000_5040);
    assert_eq!(mhdr.mddf, 0x0000_6040);
    assert_eq!(mhdr.modf, 0x0000_7040);
    assert_eq!(mhdr.mfbo, 0x0000_8040);
    assert_eq!(mhdr.mh2o, 0x0000_9040);
    assert_eq!(mhdr.mtxf, 0x0000_A040);
    Ok(())
}

#[test]
fn parses_mcin_entries() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;
    let entries = adt.mcin_entries()?.ok_or(FormatError::InvalidRange {
        field: "missing MCIN chunk",
    })?;

    assert_eq!(entries.len(), 256);
    assert_eq!(entries[0].offset, 0x100);
    assert_eq!(entries[0].size, 0x200);
    assert_eq!(entries[0].flags, 0x3);
    assert_eq!(entries[0].async_id, 7);
    assert_eq!(entries[1].offset, 0x300);
    assert_eq!(entries[1].size, 0x400);
    assert_eq!(entries[255].offset, 0);
    Ok(())
}

#[test]
fn parses_mcnk_headers() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;
    let headers = adt.mcnk_headers()?;

    assert_eq!(headers.len(), 1);
    let header = headers[0];
    assert_eq!(header.flags, 0x0001_0041);
    assert_eq!(header.ix, 4);
    assert_eq!(header.iy, 9);
    assert_eq!(header.n_layers, 3);
    assert_eq!(header.n_doodad_refs, 2);
    assert_eq!(header.ofs_height, 136);
    assert_eq!(header.ofs_normal, 724);
    assert_eq!(header.ofs_layer, 1180);
    assert_eq!(header.ofs_alpha, 1236);
    assert_eq!(header.size_alpha, 4232);
    assert_eq!(header.area_id, 617);
    assert_eq!(header.holes, 0xAA55);
    assert_eq!(header.doodad_mapping, [10, 11, 12, 13, 14, 15, 16, 17]);
    assert_eq!(header.doodad_stencil, [1, 2, 3, 4, 5, 6, 7, 8]);
    assert_eq!(header.ofs_liquid, 0x400);
    assert_eq!(header.size_liquid, 0x80);
    assert_eq!(header.zpos, 1.25);
    assert_eq!(header.xpos, 2.5);
    assert_eq!(header.ypos, 3.75);
    assert_eq!(header.ofs_mccv, 0x500);
    Ok(())
}

#[test]
fn parses_mcnk_subchunks_by_header_offsets() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;
    let mcnk = first_mcnk(&adt)?;

    let mcvt = mcnk
        .first_subchunk(*b"MCVT")?
        .ok_or(FormatError::InvalidRange {
            field: "missing MCVT",
        })?;
    let mcly = mcnk
        .first_subchunk(*b"MCLY")?
        .ok_or(FormatError::InvalidRange {
            field: "missing MCLY",
        })?;
    let mcal = mcnk
        .first_subchunk(*b"MCAL")?
        .ok_or(FormatError::InvalidRange {
            field: "missing MCAL",
        })?;

    assert_eq!(mcvt.data.len(), MCNK_VERTEX_HEIGHT_COUNT * 4);
    assert_eq!(mcly.data.len(), 3 * 16);
    assert_eq!(mcal.data.len(), 4224);
    assert!(mcnk.first_subchunk(*b"MFBO")?.is_none());
    Ok(())
}

#[test]
fn parses_mcvt_height_values() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;
    let mcnk = first_mcnk(&adt)?;
    let heights = mcnk.heights()?.ok_or(FormatError::InvalidRange {
        field: "missing MCVT heights",
    })?;

    assert_eq!(heights.len(), MCNK_VERTEX_HEIGHT_COUNT);
    assert_eq!(heights[0], 0.0);
    assert_eq!(heights[8], 2.0);
    assert_eq!(heights[144], 36.0);
    Ok(())
}

#[test]
fn parses_mcnr_normals_in_render_axis_order() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;
    let mcnk = first_mcnk(&adt)?;
    let normals = mcnk.normals()?.ok_or(FormatError::InvalidRange {
        field: "missing MCNR normals",
    })?;

    assert_eq!(normals.len(), MCNK_VERTEX_HEIGHT_COUNT);
    assert_close(normals[0].x, 1.0);
    assert_close(normals[0].y, 64.0 / 127.0);
    assert_close(normals[0].z, 0.0);
    assert_close(normals[1].y, 32.0 / 127.0);
    Ok(())
}

#[test]
fn parses_mcly_texture_layers() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;
    let mcnk = first_mcnk(&adt)?;
    let layers = mcnk.texture_layers()?.ok_or(FormatError::InvalidRange {
        field: "missing MCLY layers",
    })?;

    assert_eq!(layers.len(), 3);
    assert_eq!(layers[0].texture_id, 4);
    assert_eq!(layers[0].flags, 0);
    assert_eq!(layers[0].effect_id, 0xFFFF_FFFF);
    assert_eq!(layers[1].texture_id, 9);
    assert_eq!(layers[1].flags, MCLY_FLAG_USE_ALPHA);
    assert_eq!(layers[1].ofs_alpha, 0);
    assert_eq!(layers[1].effect_id, 17);
    assert_eq!(
        layers[2].flags,
        MCLY_FLAG_USE_ALPHA | MCLY_FLAG_ALPHA_COMPRESSED
    );
    assert_eq!(layers[2].ofs_alpha, ALPHA_MAP_SIZE as u32);
    Ok(())
}

#[test]
fn decodes_big_and_compressed_mcal_alpha_maps() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_adt())?;
    let mcnk = first_mcnk(&adt)?;
    let alpha_maps = mcnk.alpha_maps(true, false)?;

    assert_eq!(alpha_maps.len(), 3);
    assert!(alpha_maps[0].is_none());

    let big_alpha = alpha_maps[1]
        .as_ref()
        .ok_or(FormatError::InvalidRange {
            field: "missing big alpha map",
        })?
        .as_bytes();
    assert_eq!(big_alpha[0], 0);
    assert_eq!(big_alpha[250], 250);
    assert_eq!(big_alpha[251], 0);

    let compressed_alpha = alpha_maps[2]
        .as_ref()
        .ok_or(FormatError::InvalidRange {
            field: "missing compressed alpha map",
        })?
        .as_bytes();
    assert!(compressed_alpha.iter().all(|value| *value == 7));
    Ok(())
}

#[test]
fn decodes_legacy_4bit_mcal_alpha_maps() -> FormatResult<()> {
    let adt = AdtFile::parse(&chunk(b"MCNK", &fixture_legacy_alpha_mcnk()))?;
    let mcnk = first_mcnk(&adt)?;
    let alpha_maps = mcnk.alpha_maps(false, true)?;

    assert_eq!(alpha_maps.len(), 2);
    assert!(alpha_maps[0].is_none());

    let alpha = alpha_maps[1]
        .as_ref()
        .ok_or(FormatError::InvalidRange {
            field: "missing legacy alpha map",
        })?
        .as_bytes();
    assert_eq!(alpha[0], 0x11);
    assert_eq!(alpha[1], 0x22);
    assert_eq!(alpha[62], 0x11);
    assert_eq!(alpha[63], 0x22);
    Ok(())
}

#[test]
fn parses_adt_asset_filename_blocks() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_asset_adt())?;

    assert_eq!(
        adt.texture_filenames()?.ok_or(FormatError::InvalidRange {
            field: "missing MTEX",
        })?,
        vec!["tiles/foo.blp", "tiles/bar.blp"]
    );
    assert_eq!(
        adt.model_filenames()?.ok_or(FormatError::InvalidRange {
            field: "missing MMDX",
        })?,
        vec!["models/tree.m2", "models/rock.m2"]
    );
    assert_eq!(
        adt.wmo_filenames()?.ok_or(FormatError::InvalidRange {
            field: "missing MWMO",
        })?,
        vec!["world/building.wmo"]
    );
    Ok(())
}

#[test]
fn parses_adt_asset_filename_offset_tables() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_asset_adt())?;
    let models = adt.model_filenames()?.ok_or(FormatError::InvalidRange {
        field: "missing MMDX",
    })?;
    let model_offsets = adt
        .model_filename_offsets()?
        .ok_or(FormatError::InvalidRange {
            field: "missing MMID",
        })?;

    assert_eq!(model_offsets, vec![0, 15]);
    assert_eq!(
        adt.wmo_filename_offsets()?
            .ok_or(FormatError::InvalidRange {
                field: "missing MWID",
            })?,
        vec![0]
    );
    assert_eq!(
        filename_by_name_id(&models, &model_offsets, 1),
        Some("models/rock.m2")
    );
    assert_eq!(filename_by_name_id(&models, &model_offsets, 99), None);
    Ok(())
}

#[test]
fn parses_adt_model_and_wmo_placements() -> FormatResult<()> {
    let adt = AdtFile::parse(&fixture_asset_adt())?;
    let doodads = adt.model_placements()?.ok_or(FormatError::InvalidRange {
        field: "missing MDDF",
    })?;
    let wmos = adt.wmo_placements()?.ok_or(FormatError::InvalidRange {
        field: "missing MODF",
    })?;

    assert_eq!(
        doodads,
        vec![MddfEntry {
            name_id: 1,
            unique_id: 77,
            position: [1.0, 2.0, 3.0],
            rotation: [10.0, 20.0, 30.0],
            scale: 1536,
            flags: 3,
        }]
    );

    assert_eq!(wmos.len(), 1);
    assert_eq!(wmos[0].name_id, 0);
    assert_eq!(wmos[0].unique_id, 88);
    assert_eq!(wmos[0].position, [4.0, 5.0, 6.0]);
    assert_eq!(wmos[0].rotation, [40.0, 50.0, 60.0]);
    assert_eq!(wmos[0].lower_extent, [-1.0, -2.0, -3.0]);
    assert_eq!(wmos[0].upper_extent, [7.0, 8.0, 9.0]);
    assert_eq!(wmos[0].flags, 4);
    assert_eq!(wmos[0].doodad_set, 5);
    assert_eq!(wmos[0].name_set, 6);
    assert_eq!(wmos[0].scale, 2048);
    Ok(())
}

#[test]
fn rejects_unterminated_adt_string_block() -> FormatResult<()> {
    let adt = AdtFile::parse(&chunk(b"MTEX", b"tiles/foo.blp"))?;

    assert!(matches!(
        adt.texture_filenames(),
        Err(FormatError::UnterminatedString { .. })
    ));
    Ok(())
}

#[test]
fn rejects_truncated_adt_chunk_payload() {
    let mut bytes = fixture_adt();
    bytes.pop();

    assert!(matches!(
        parse_error(&bytes),
        FormatError::UnexpectedEof { .. }
    ));
}

fn parse_error(bytes: &[u8]) -> FormatError {
    match AdtFile::parse(bytes) {
        Ok(_) => FormatError::InvalidRange {
            field: "test expected parse failure",
        },
        Err(err) => err,
    }
}

fn first_mcnk(adt: &AdtFile) -> FormatResult<Mcnk> {
    adt.mcnk_chunks()?
        .into_iter()
        .next()
        .ok_or(FormatError::InvalidRange {
            field: "missing MCNK chunk",
        })
}

fn assert_close(actual: f32, expected: f32) {
    assert!((actual - expected).abs() <= f32::EPSILON);
}

fn write_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_f32(bytes: &mut [u8], offset: usize, value: f32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
