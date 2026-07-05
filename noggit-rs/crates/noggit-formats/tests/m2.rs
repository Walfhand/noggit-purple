//! M2 and skin parser fixtures.

use noggit_formats::FormatError;
use noggit_formats::m2::{M2_HEADER_SIZE, M2File, M2SkinFile};

#[test]
fn parses_m2_vertices_textures_and_lookup() -> Result<(), Box<dyn std::error::Error>> {
    let file = M2File::parse(&fixture_m2())?;

    assert_eq!(file.header().version, 264);
    assert_eq!(file.header().view_count, 1);
    assert_eq!(file.vertices().len(), 3);
    assert_eq!(file.vertices()[1].position, [1.0, 0.0, 0.0]);
    assert_eq!(file.vertices()[2].normal, [0.0, 0.0, 1.0]);
    assert_eq!(file.vertices()[2].tex_coords[0], [0.5, 1.0]);
    assert_eq!(file.textures().len(), 1);
    assert_eq!(
        file.textures()[0].filename.as_deref(),
        Some("doodads/test/model_texture.blp")
    );
    assert_eq!(file.texture_lookup(), &[0]);

    Ok(())
}

#[test]
fn parses_m2_skin_indices_submeshes_and_texture_units() -> Result<(), Box<dyn std::error::Error>> {
    let skin = M2SkinFile::parse(&fixture_skin())?;

    assert_eq!(skin.header().index_count, 3);
    assert_eq!(skin.indices(), &[0, 1, 2]);
    assert_eq!(skin.triangles(), &[0, 1, 2]);
    assert_eq!(skin.submeshes().len(), 1);
    assert_eq!(skin.submeshes()[0].index_start, 0);
    assert_eq!(skin.submeshes()[0].index_count, 3);
    assert_eq!(skin.texture_units().len(), 1);
    assert_eq!(skin.texture_units()[0].submesh, 0);
    assert_eq!(skin.texture_units()[0].texture_combo_index, 0);

    Ok(())
}

#[test]
fn rejects_invalid_m2_magic() {
    let mut bytes = fixture_m2();
    bytes[0..4].copy_from_slice(b"NOPE");

    assert!(matches!(
        M2File::parse(&bytes),
        Err(FormatError::InvalidMagic { .. })
    ));
}

#[test]
fn rejects_truncated_m2_vertex_array() {
    let mut bytes = fixture_m2();
    bytes.truncate(M2_HEADER_SIZE + 8);

    assert!(matches!(
        M2File::parse(&bytes),
        Err(FormatError::UnexpectedEof { .. })
    ));
}

fn fixture_m2() -> Vec<u8> {
    let texture_offset = M2_HEADER_SIZE + 3 * 48;
    let texture_lookup_offset = texture_offset + 16;
    let texture_name_offset = texture_lookup_offset + 2;
    let texture_name = b"doodads/test/model_texture.blp\0";
    let mut bytes = vec![0; texture_name_offset + texture_name.len()];

    bytes[0..4].copy_from_slice(b"MD20");
    write_u32(&mut bytes, 4, 264);
    write_u32(&mut bytes, 60, 3);
    write_u32(&mut bytes, 64, M2_HEADER_SIZE as u32);
    write_u32(&mut bytes, 68, 1);
    write_u32(&mut bytes, 80, 1);
    write_u32(&mut bytes, 84, texture_offset as u32);
    write_u32(&mut bytes, 128, 1);
    write_u32(&mut bytes, 132, texture_lookup_offset as u32);
    write_vec3(&mut bytes, 160, [-1.0, -1.0, -1.0]);
    write_vec3(&mut bytes, 172, [1.0, 1.0, 1.0]);
    write_f32(&mut bytes, 184, 2.0);

    write_vertex(
        &mut bytes,
        M2_HEADER_SIZE,
        [0.0, 0.0, 0.0],
        [0.0, 0.0, 1.0],
        [0.0, 0.0],
    );
    write_vertex(
        &mut bytes,
        M2_HEADER_SIZE + 48,
        [1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0],
        [1.0, 0.0],
    );
    write_vertex(
        &mut bytes,
        M2_HEADER_SIZE + 96,
        [0.0, 1.0, 0.0],
        [0.0, 0.0, 1.0],
        [0.5, 1.0],
    );

    write_u32(&mut bytes, texture_offset, 0);
    write_u32(&mut bytes, texture_offset + 4, 0);
    write_u32(&mut bytes, texture_offset + 8, texture_name.len() as u32);
    write_u32(&mut bytes, texture_offset + 12, texture_name_offset as u32);
    write_u16(&mut bytes, texture_lookup_offset, 0);
    bytes[texture_name_offset..][..texture_name.len()].copy_from_slice(texture_name);

    bytes
}

fn fixture_skin() -> Vec<u8> {
    let index_offset = 48;
    let triangle_offset = index_offset + 3 * 2;
    let submesh_offset = triangle_offset + 3 * 2;
    let texture_unit_offset = submesh_offset + 48;
    let mut bytes = vec![0; texture_unit_offset + 24];

    bytes[0..4].copy_from_slice(b"SKIN");
    write_u32(&mut bytes, 4, 3);
    write_u32(&mut bytes, 8, index_offset as u32);
    write_u32(&mut bytes, 12, 3);
    write_u32(&mut bytes, 16, triangle_offset as u32);
    write_u32(&mut bytes, 28, 1);
    write_u32(&mut bytes, 32, submesh_offset as u32);
    write_u32(&mut bytes, 36, 1);
    write_u32(&mut bytes, 40, texture_unit_offset as u32);

    for (index, value) in [0_u16, 1, 2].into_iter().enumerate() {
        write_u16(&mut bytes, index_offset + index * 2, value);
        write_u16(&mut bytes, triangle_offset + index * 2, value);
    }

    write_u16(&mut bytes, submesh_offset, 7);
    write_u16(&mut bytes, submesh_offset + 4, 0);
    write_u16(&mut bytes, submesh_offset + 6, 3);
    write_u16(&mut bytes, submesh_offset + 8, 0);
    write_u16(&mut bytes, submesh_offset + 10, 3);
    write_vec3(&mut bytes, submesh_offset + 20, [-1.0, -1.0, -1.0]);
    write_vec3(&mut bytes, submesh_offset + 32, [1.0, 1.0, 1.0]);
    write_f32(&mut bytes, submesh_offset + 44, 2.0);

    bytes[texture_unit_offset] = 0;
    bytes[texture_unit_offset + 1] = 0;
    write_u16(&mut bytes, texture_unit_offset + 4, 0);
    write_u16(&mut bytes, texture_unit_offset + 6, 0);
    write_u16(&mut bytes, texture_unit_offset + 8, u16::MAX);
    write_u16(&mut bytes, texture_unit_offset + 14, 1);
    write_u16(&mut bytes, texture_unit_offset + 16, 0);

    bytes
}

fn write_vertex(
    bytes: &mut [u8],
    offset: usize,
    position: [f32; 3],
    normal: [f32; 3],
    tex_coord: [f32; 2],
) {
    write_vec3(bytes, offset, position);
    bytes[offset + 12] = 255;
    write_vec3(bytes, offset + 20, normal);
    write_f32(bytes, offset + 32, tex_coord[0]);
    write_f32(bytes, offset + 36, tex_coord[1]);
}

fn write_vec3(bytes: &mut [u8], offset: usize, value: [f32; 3]) {
    write_f32(bytes, offset, value[0]);
    write_f32(bytes, offset + 4, value[1]);
    write_f32(bytes, offset + 8, value[2]);
}

fn write_f32(bytes: &mut [u8], offset: usize, value: f32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_bits().to_le_bytes());
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}
