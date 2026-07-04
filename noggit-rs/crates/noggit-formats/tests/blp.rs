//! BLP parser and decoder tests.

use noggit_formats::FormatResult;
use noggit_formats::blp::BlpFile;

#[test]
fn decodes_paletted_blp_with_8_bit_alpha() -> FormatResult<()> {
    let blp = BlpFile::parse(&paletted_blp(2, 2, 8, &[0, 1, 2, 3], &[255, 128, 64, 0]))?;
    let image = blp.decode_rgba_mipmap(0)?;

    assert_eq!(blp.header().magic, *b"BLP2");
    assert_eq!(image.width, 2);
    assert_eq!(image.height, 2);
    assert_eq!(
        image.pixels,
        vec![
            255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 255, 255, 255, 0
        ]
    );
    Ok(())
}

#[test]
fn decodes_paletted_blp_with_1_bit_alpha() -> FormatResult<()> {
    let blp = BlpFile::parse(&paletted_blp(2, 2, 1, &[0, 1, 2, 3], &[0b0000_0101]))?;
    let image = blp.decode_rgba_mipmap(0)?;

    assert_eq!(
        image.pixels,
        vec![
            255, 0, 0, 255, 0, 255, 0, 0, 0, 0, 255, 255, 255, 255, 255, 0
        ]
    );
    Ok(())
}

#[test]
fn decodes_dxt1_blp() -> FormatResult<()> {
    let mut block = Vec::new();
    block.extend_from_slice(&0xf800_u16.to_le_bytes());
    block.extend_from_slice(&0x07e0_u16.to_le_bytes());
    block.extend_from_slice(&0_u32.to_le_bytes());

    let blp = BlpFile::parse(&dxt_blp(4, 4, 0, &block))?;
    let image = blp.decode_rgba_mipmap(0)?;

    assert_eq!(image.width, 4);
    assert_eq!(image.height, 4);
    assert_eq!(&image.pixels[0..4], &[255, 0, 0, 255]);
    Ok(())
}

#[test]
fn decodes_dxt3_blp_alpha() -> FormatResult<()> {
    let mut block = vec![0xff; 8];
    block.extend_from_slice(&0x001f_u16.to_le_bytes());
    block.extend_from_slice(&0x07e0_u16.to_le_bytes());
    block.extend_from_slice(&0_u32.to_le_bytes());

    let blp = BlpFile::parse(&dxt_blp(4, 4, 1, &block))?;
    let image = blp.decode_rgba_mipmap(0)?;

    assert_eq!(&image.pixels[0..4], &[0, 0, 255, 255]);
    Ok(())
}

#[test]
fn decodes_dxt5_blp_alpha() -> FormatResult<()> {
    let mut block = vec![255, 0, 0, 0, 0, 0, 0, 0];
    block.extend_from_slice(&0xffff_u16.to_le_bytes());
    block.extend_from_slice(&0x0000_u16.to_le_bytes());
    block.extend_from_slice(&0_u32.to_le_bytes());

    let blp = BlpFile::parse(&dxt_blp(4, 4, 3, &block))?;
    let image = blp.decode_rgba_mipmap(0)?;

    assert_eq!(&image.pixels[0..4], &[255, 255, 255, 255]);
    Ok(())
}

fn paletted_blp(width: u32, height: u32, alpha_depth: u8, indices: &[u8], alpha: &[u8]) -> Vec<u8> {
    let mut bytes = header(
        width,
        height,
        1,
        alpha_depth,
        0,
        148 + 1024,
        indices.len() + alpha.len(),
    );
    let mut palette = vec![0_u8; 1024];
    write_bgra(&mut palette, 0, [0, 0, 255, 255]);
    write_bgra(&mut palette, 1, [0, 255, 0, 255]);
    write_bgra(&mut palette, 2, [255, 0, 0, 255]);
    write_bgra(&mut palette, 3, [255, 255, 255, 255]);
    bytes.extend_from_slice(&palette);
    bytes.extend_from_slice(indices);
    bytes.extend_from_slice(alpha);
    bytes
}

fn dxt_blp(width: u32, height: u32, alpha_type: u8, data: &[u8]) -> Vec<u8> {
    let mut bytes = header(width, height, 2, 8, alpha_type, 148, data.len());
    bytes.extend_from_slice(data);
    bytes
}

fn header(
    width: u32,
    height: u32,
    compression: u8,
    alpha_depth: u8,
    alpha_type: u8,
    mip_offset: usize,
    mip_size: usize,
) -> Vec<u8> {
    let mut bytes = vec![0_u8; 148];
    bytes[0..4].copy_from_slice(b"BLP2");
    write_u32(&mut bytes, 4, 1);
    bytes[8] = compression;
    bytes[9] = alpha_depth;
    bytes[10] = alpha_type;
    bytes[11] = 1;
    write_u32(&mut bytes, 12, width);
    write_u32(&mut bytes, 16, height);
    write_u32(&mut bytes, 20, mip_offset as u32);
    write_u32(&mut bytes, 84, mip_size as u32);
    bytes
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_bgra(palette: &mut [u8], index: usize, bgra: [u8; 4]) {
    palette[index * 4..index * 4 + 4].copy_from_slice(&bgra);
}
