//! WMO root and group parsing tests.

use noggit_formats::wmo::{WmoBatch, WmoFile, WmoGroupFile, WmoGroupInfo, WmoMaterial};
use noggit_formats::{FormatError, FormatResult};

fn chunk(id: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(id);
    bytes.extend_from_slice(&(data.len() as u32).to_le_bytes());
    bytes.extend_from_slice(data);
    bytes
}

fn stored_chunk(id: &[u8; 4], data: &[u8]) -> Vec<u8> {
    chunk(&[id[3], id[2], id[1], id[0]], data)
}

#[test]
fn parses_wmo_root_header_materials_and_group_infos() -> FormatResult<()> {
    let wmo = WmoFile::parse(&fixture_root_wmo())?;
    let header = wmo.header()?.ok_or(FormatError::InvalidRange {
        field: "missing MOHD",
    })?;
    let textures = wmo.texture_filenames()?.ok_or(FormatError::InvalidRange {
        field: "missing MOTX",
    })?;
    let materials = wmo.materials()?.ok_or(FormatError::InvalidRange {
        field: "missing MOMT",
    })?;
    let group_infos = wmo.group_infos()?.ok_or(FormatError::InvalidRange {
        field: "missing MOGI",
    })?;

    assert_eq!(wmo.version()?, Some(17));
    assert_eq!(header.texture_count, 1);
    assert_eq!(header.group_count, 1);
    assert_eq!(header.wmo_id, 42);
    assert_eq!(textures, vec!["textures/stone.blp".to_string()]);
    assert_eq!(
        materials,
        vec![WmoMaterial {
            flags: 4,
            shader: 0,
            blend_mode: 0,
            texture_offset_1: 0,
            texture_offset_2: 0,
        }]
    );
    assert_eq!(
        group_infos,
        vec![WmoGroupInfo {
            flags: 8,
            bounding_box_max: [7.0, 8.0, 9.0],
            bounding_box_min: [-1.0, -2.0, -3.0],
            name_offset: 0,
        }]
    );
    Ok(())
}

#[test]
fn parses_wmo_group_mesh_chunks_inside_mogp() -> FormatResult<()> {
    let group = WmoGroupFile::parse(&fixture_group_wmo())?;
    let triangle_materials = group
        .triangle_materials()?
        .ok_or(FormatError::InvalidRange {
            field: "missing MOPY",
        })?;
    let indices = group.indices()?.ok_or(FormatError::InvalidRange {
        field: "missing MOVI",
    })?;
    let vertices = group.vertices()?.ok_or(FormatError::InvalidRange {
        field: "missing MOVT",
    })?;
    let normals = group.normals()?.ok_or(FormatError::InvalidRange {
        field: "missing MONR",
    })?;
    let tex_coords = group.tex_coords()?.ok_or(FormatError::InvalidRange {
        field: "missing MOTV",
    })?;
    let batches = group.batches()?.ok_or(FormatError::InvalidRange {
        field: "missing MOBA",
    })?;

    assert_eq!(group.version()?, Some(17));
    assert_eq!(group.header().id, 99);
    assert_eq!(group.header().flags, 0x8);
    assert_eq!(triangle_materials.len(), 1);
    assert_eq!(indices, vec![0, 1, 2]);
    assert_eq!(
        vertices,
        vec![[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]
    );
    assert_eq!(
        normals,
        vec![[0.0, 0.0, 1.0], [0.0, 0.0, 1.0], [0.0, 0.0, 1.0]]
    );
    assert_eq!(tex_coords, vec![[0.0, 0.0], [1.0, 0.0], [0.0, 1.0]]);
    assert_eq!(
        batches,
        vec![WmoBatch {
            unused: [0, 0, 0, 0, 0, 0],
            index_start: 0,
            index_count: 3,
            vertex_start: 0,
            vertex_end: 2,
            flags: 0,
            texture: 0,
        }]
    );
    Ok(())
}

#[test]
fn normalizes_reversed_storage_order_wmo_chunk_ids() -> FormatResult<()> {
    let wmo = WmoFile::parse(&stored_chunk(b"MVER", &17_u32.to_le_bytes()))?;

    assert_eq!(wmo.version()?, Some(17));
    Ok(())
}

#[test]
fn rejects_truncated_wmo_group_chunk_payload() {
    let bytes = chunk(b"MOGP", &[0; 8]);

    assert!(matches!(
        WmoGroupFile::parse(&bytes),
        Err(FormatError::UnexpectedEof { .. })
    ));
}

fn fixture_root_wmo() -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&chunk(b"MVER", &17_u32.to_le_bytes()));
    bytes.extend_from_slice(&chunk(b"MOHD", &mohd()));
    bytes.extend_from_slice(&chunk(b"MOTX", b"textures/stone.blp\0"));
    bytes.extend_from_slice(&chunk(b"MOMT", &momt()));
    bytes.extend_from_slice(&chunk(b"MOGN", b"group\0"));
    bytes.extend_from_slice(&chunk(b"MOGI", &mogi()));
    bytes
}

fn fixture_group_wmo() -> Vec<u8> {
    let mut mogp = mogp_header();
    mogp.extend_from_slice(&chunk(b"MOPY", &[0x20, 0]));
    mogp.extend_from_slice(&chunk(
        b"MOVI",
        &[0_u16, 1, 2]
            .into_iter()
            .flat_map(u16::to_le_bytes)
            .collect::<Vec<_>>(),
    ));
    mogp.extend_from_slice(&chunk(
        b"MOVT",
        &[[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]
            .into_iter()
            .flatten()
            .flat_map(f32::to_le_bytes)
            .collect::<Vec<_>>(),
    ));
    mogp.extend_from_slice(&chunk(
        b"MONR",
        &[[0.0, 0.0, 1.0]; 3]
            .into_iter()
            .flatten()
            .flat_map(f32::to_le_bytes)
            .collect::<Vec<_>>(),
    ));
    mogp.extend_from_slice(&chunk(
        b"MOTV",
        &[[0.0, 0.0], [1.0, 0.0], [0.0, 1.0]]
            .into_iter()
            .flatten()
            .flat_map(f32::to_le_bytes)
            .collect::<Vec<_>>(),
    ));
    mogp.extend_from_slice(&chunk(b"MOBA", &moba()));

    let mut bytes = Vec::new();
    bytes.extend_from_slice(&chunk(b"MVER", &17_u32.to_le_bytes()));
    bytes.extend_from_slice(&chunk(b"MOGP", &mogp));
    bytes
}

fn mohd() -> Vec<u8> {
    let mut bytes = Vec::new();
    for value in [1_u32, 1, 0, 0, 0, 0, 0, 0x11223344, 42] {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    push_vec3(&mut bytes, [-1.0, -2.0, -3.0]);
    push_vec3(&mut bytes, [7.0, 8.0, 9.0]);
    bytes.extend_from_slice(&3_u16.to_le_bytes());
    bytes.extend_from_slice(&0_u16.to_le_bytes());
    bytes
}

fn momt() -> Vec<u8> {
    let mut bytes = vec![0; 64];
    write_u32(&mut bytes, 0, 4);
    write_u32(&mut bytes, 4, 0);
    write_u32(&mut bytes, 8, 0);
    write_u32(&mut bytes, 12, 0);
    write_u32(&mut bytes, 24, 0);
    bytes
}

fn mogi() -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&8_u32.to_le_bytes());
    push_vec3(&mut bytes, [7.0, 8.0, 9.0]);
    push_vec3(&mut bytes, [-1.0, -2.0, -3.0]);
    bytes.extend_from_slice(&0_i32.to_le_bytes());
    bytes
}

fn mogp_header() -> Vec<u8> {
    let mut bytes = vec![0; 68];
    write_u32(&mut bytes, 8, 0x8);
    write_f32(&mut bytes, 12, -1.0);
    write_f32(&mut bytes, 16, -2.0);
    write_f32(&mut bytes, 20, -3.0);
    write_f32(&mut bytes, 24, 1.0);
    write_f32(&mut bytes, 28, 2.0);
    write_f32(&mut bytes, 32, 3.0);
    write_u32(&mut bytes, 52, 0);
    write_u32(&mut bytes, 56, 99);
    bytes
}

fn moba() -> Vec<u8> {
    let mut bytes = vec![0; 24];
    write_u32(&mut bytes, 12, 0);
    write_u16(&mut bytes, 16, 3);
    write_u16(&mut bytes, 18, 0);
    write_u16(&mut bytes, 20, 2);
    bytes[22] = 0;
    bytes[23] = 0;
    bytes
}

fn push_vec3(bytes: &mut Vec<u8>, values: [f32; 3]) {
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn write_f32(bytes: &mut [u8], offset: usize, value: f32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
