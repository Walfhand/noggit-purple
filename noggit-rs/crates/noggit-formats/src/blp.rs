//! BLP texture parsing and RGBA decoding.

use crate::error::{FormatError, FormatResult, read_exact, read_u8, read_u32_le};

const BLP_HEADER_SIZE: usize = 148;
const BLP_MIP_LEVELS: usize = 16;
const BLP_PALETTE_COLORS: usize = 256;
const BLP_PALETTE_SIZE: usize = BLP_PALETTE_COLORS * 4;

/// Parsed BLP texture file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BlpFile {
    header: BlpHeader,
    bytes: Vec<u8>,
}

/// BLP header.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BlpHeader {
    /// Magic, usually `BLP1` or `BLP2`.
    pub magic: [u8; 4],
    /// BLP content type field.
    pub content_type: u32,
    /// Encoding/compression field.
    pub compression: u8,
    /// Alpha bit depth.
    pub alpha_depth: u8,
    /// Alpha encoding field.
    pub alpha_type: u8,
    /// Mipmap presence/count hint.
    pub mipmap_type: u8,
    /// Texture width.
    pub width: u32,
    /// Texture height.
    pub height: u32,
    /// Mipmap byte offsets.
    pub mipmap_offsets: [u32; BLP_MIP_LEVELS],
    /// Mipmap byte sizes.
    pub mipmap_sizes: [u32; BLP_MIP_LEVELS],
}

/// Decoded RGBA image.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RgbaImage {
    /// Image width in pixels.
    pub width: u32,
    /// Image height in pixels.
    pub height: u32,
    /// Pixels in row-major RGBA8 order.
    pub pixels: Vec<u8>,
}

impl BlpFile {
    /// Parse a BLP file.
    pub fn parse(bytes: &[u8]) -> FormatResult<Self> {
        let header = BlpHeader::parse(bytes)?;
        Ok(Self {
            header,
            bytes: bytes.to_vec(),
        })
    }

    /// Return the parsed header.
    pub fn header(&self) -> &BlpHeader {
        &self.header
    }

    /// Decode a mipmap into RGBA8 pixels.
    pub fn decode_rgba_mipmap(&self, level: usize) -> FormatResult<RgbaImage> {
        if level >= BLP_MIP_LEVELS {
            return Err(FormatError::InvalidRange {
                field: "BLP mipmap level",
            });
        }

        let width = mip_dimension(self.header.width, level);
        let height = mip_dimension(self.header.height, level);
        let data = self.mipmap_data(level)?;

        match self.header.compression {
            1 => self.decode_paletted(data, width, height, level),
            2 => decode_dxt(data, width, height, self.header.alpha_type),
            value => Err(FormatError::Unsupported {
                field: "BLP compression",
                value: value.into(),
            }),
        }
    }

    fn mipmap_data(&self, level: usize) -> FormatResult<&[u8]> {
        let offset = usize::try_from(self.header.mipmap_offsets[level]).map_err(|_| {
            FormatError::InvalidRange {
                field: "BLP mipmap offset",
            }
        })?;
        let size = usize::try_from(self.header.mipmap_sizes[level]).map_err(|_| {
            FormatError::InvalidRange {
                field: "BLP mipmap size",
            }
        })?;
        let end = offset.checked_add(size).ok_or(FormatError::InvalidRange {
            field: "BLP mipmap end",
        })?;

        self.bytes
            .get(offset..end)
            .ok_or(FormatError::UnexpectedEof {
                offset,
                needed: size,
                len: self.bytes.len(),
            })
    }

    fn decode_paletted(
        &self,
        data: &[u8],
        width: u32,
        height: u32,
        level: usize,
    ) -> FormatResult<RgbaImage> {
        let palette_start = BLP_HEADER_SIZE;
        let palette = self
            .bytes
            .get(palette_start..palette_start + BLP_PALETTE_SIZE)
            .ok_or(FormatError::UnexpectedEof {
                offset: palette_start,
                needed: BLP_PALETTE_SIZE,
                len: self.bytes.len(),
            })?;
        let pixel_count = pixel_count(width, height)?;
        if data.len() < pixel_count {
            return Err(FormatError::UnexpectedEof {
                offset: self.header.mipmap_offsets[level] as usize,
                needed: pixel_count,
                len: self.bytes.len(),
            });
        }

        let alpha_offset = pixel_count;
        let alpha_bytes = match self.header.alpha_depth {
            0 => 0,
            1 => pixel_count.div_ceil(8),
            8 => pixel_count,
            value => {
                return Err(FormatError::Unsupported {
                    field: "BLP paletted alpha depth",
                    value: value.into(),
                });
            }
        };
        if data.len() < alpha_offset + alpha_bytes {
            return Err(FormatError::UnexpectedEof {
                offset: self.header.mipmap_offsets[level] as usize + alpha_offset,
                needed: alpha_bytes,
                len: self.bytes.len(),
            });
        }

        let mut pixels = Vec::with_capacity(pixel_count * 4);
        for pixel_index in 0..pixel_count {
            let palette_index = usize::from(data[pixel_index]);
            let palette_offset = palette_index * 4;
            let blue = palette[palette_offset];
            let green = palette[palette_offset + 1];
            let red = palette[palette_offset + 2];
            let alpha = match self.header.alpha_depth {
                0 => 255,
                1 => {
                    let byte = data[alpha_offset + pixel_index / 8];
                    if byte & (1 << (pixel_index % 8)) == 0 {
                        0
                    } else {
                        255
                    }
                }
                8 => data[alpha_offset + pixel_index],
                _ => unreachable!("alpha depth was validated above"),
            };

            pixels.extend_from_slice(&[red, green, blue, alpha]);
        }

        Ok(RgbaImage {
            width,
            height,
            pixels,
        })
    }
}

impl BlpHeader {
    fn parse(bytes: &[u8]) -> FormatResult<Self> {
        let magic = read_exact::<4>(bytes, 0)?;
        if magic != *b"BLP1" && magic != *b"BLP2" {
            return Err(FormatError::InvalidMagic {
                expected: *b"BLP2",
                actual: magic,
            });
        }

        let mut mipmap_offsets = [0_u32; BLP_MIP_LEVELS];
        let mut mipmap_sizes = [0_u32; BLP_MIP_LEVELS];
        for (index, offset) in mipmap_offsets.iter_mut().enumerate() {
            *offset = read_u32_le(bytes, 20 + index * 4)?;
        }
        for (index, size) in mipmap_sizes.iter_mut().enumerate() {
            *size = read_u32_le(bytes, 84 + index * 4)?;
        }

        Ok(Self {
            magic,
            content_type: read_u32_le(bytes, 4)?,
            compression: read_u8(bytes, 8)?,
            alpha_depth: read_u8(bytes, 9)?,
            alpha_type: read_u8(bytes, 10)?,
            mipmap_type: read_u8(bytes, 11)?,
            width: read_u32_le(bytes, 12)?,
            height: read_u32_le(bytes, 16)?,
            mipmap_offsets,
            mipmap_sizes,
        })
    }
}

fn decode_dxt(data: &[u8], width: u32, height: u32, alpha_type: u8) -> FormatResult<RgbaImage> {
    let block_size = match alpha_type & 3 {
        0 => 8,
        1 | 3 => 16,
        value => {
            return Err(FormatError::Unsupported {
                field: "BLP DXT alpha type",
                value: value.into(),
            });
        }
    };
    let blocks_x = width.div_ceil(4);
    let blocks_y = height.div_ceil(4);
    let expected = usize::try_from(blocks_x)
        .ok()
        .and_then(|x| {
            usize::try_from(blocks_y)
                .ok()
                .and_then(|y| x.checked_mul(y))
        })
        .and_then(|blocks| blocks.checked_mul(block_size))
        .ok_or(FormatError::InvalidRange {
            field: "BLP DXT block data",
        })?;
    if data.len() < expected {
        return Err(FormatError::UnexpectedEof {
            offset: 0,
            needed: expected,
            len: data.len(),
        });
    }

    let mut pixels = vec![0_u8; pixel_count(width, height)? * 4];
    let mut offset = 0usize;
    for block_y in 0..blocks_y {
        for block_x in 0..blocks_x {
            let block = &data[offset..offset + block_size];
            let rgba = match alpha_type & 3 {
                0 => decode_dxt1_block(block),
                1 => decode_dxt3_block(block),
                3 => decode_dxt5_block(block),
                _ => unreachable!("alpha type was validated above"),
            };
            copy_block(&rgba, &mut pixels, width, height, block_x, block_y)?;
            offset += block_size;
        }
    }

    Ok(RgbaImage {
        width,
        height,
        pixels,
    })
}

fn decode_dxt1_block(block: &[u8]) -> [[u8; 4]; 16] {
    let color0 = u16::from_le_bytes([block[0], block[1]]);
    let color1 = u16::from_le_bytes([block[2], block[3]]);
    let mut colors = [[0_u8; 4]; 4];
    colors[0] = rgb565_to_rgba(color0, 255);
    colors[1] = rgb565_to_rgba(color1, 255);

    if color0 > color1 {
        colors[2] = lerp_color(colors[0], colors[1], 2, 1, 3, 255);
        colors[3] = lerp_color(colors[0], colors[1], 1, 2, 3, 255);
    } else {
        colors[2] = lerp_color(colors[0], colors[1], 1, 1, 2, 255);
        colors[3] = [0, 0, 0, 0];
    }

    decode_color_indices(block, 4, colors)
}

fn decode_dxt3_block(block: &[u8]) -> [[u8; 4]; 16] {
    let mut pixels = decode_dxt1_block(&block[8..16]);
    for (index, pixel) in pixels.iter_mut().enumerate() {
        let alpha_byte = block[index / 2];
        let alpha_nibble = if index % 2 == 0 {
            alpha_byte & 0x0f
        } else {
            alpha_byte >> 4
        };
        pixel[3] = alpha_nibble * 17;
    }
    pixels
}

fn decode_dxt5_block(block: &[u8]) -> [[u8; 4]; 16] {
    let alpha0 = block[0];
    let alpha1 = block[1];
    let mut alphas = [0_u8; 8];
    alphas[0] = alpha0;
    alphas[1] = alpha1;
    if alpha0 > alpha1 {
        for i in 1..=6 {
            alphas[i + 1] =
                (((7 - i) as u16 * u16::from(alpha0) + i as u16 * u16::from(alpha1)) / 7) as u8;
        }
    } else {
        for i in 1..=4 {
            alphas[i + 1] =
                (((5 - i) as u16 * u16::from(alpha0) + i as u16 * u16::from(alpha1)) / 5) as u8;
        }
        alphas[6] = 0;
        alphas[7] = 255;
    }

    let mut alpha_bits = 0_u64;
    for (index, byte) in block[2..8].iter().enumerate() {
        alpha_bits |= u64::from(*byte) << (index * 8);
    }

    let mut pixels = decode_dxt1_block(&block[8..16]);
    for (index, pixel) in pixels.iter_mut().enumerate() {
        let alpha_index = ((alpha_bits >> (index * 3)) & 0x07) as usize;
        pixel[3] = alphas[alpha_index];
    }
    pixels
}

fn decode_color_indices(block: &[u8], offset: usize, colors: [[u8; 4]; 4]) -> [[u8; 4]; 16] {
    let mut pixels = [[0_u8; 4]; 16];
    let bits = u32::from_le_bytes([
        block[offset],
        block[offset + 1],
        block[offset + 2],
        block[offset + 3],
    ]);
    for (index, pixel) in pixels.iter_mut().enumerate() {
        let color_index = ((bits >> (index * 2)) & 0x03) as usize;
        *pixel = colors[color_index];
    }
    pixels
}

fn copy_block(
    block: &[[u8; 4]; 16],
    pixels: &mut [u8],
    width: u32,
    height: u32,
    block_x: u32,
    block_y: u32,
) -> FormatResult<()> {
    for y in 0..4 {
        let pixel_y = block_y * 4 + y;
        if pixel_y >= height {
            continue;
        }
        for x in 0..4 {
            let pixel_x = block_x * 4 + x;
            if pixel_x >= width {
                continue;
            }
            let out_index = usize::try_from((pixel_y * width + pixel_x) * 4).map_err(|_| {
                FormatError::InvalidRange {
                    field: "BLP pixel offset",
                }
            })?;
            let block_index =
                usize::try_from(y * 4 + x).map_err(|_| FormatError::InvalidRange {
                    field: "BLP block pixel",
                })?;
            pixels[out_index..out_index + 4].copy_from_slice(&block[block_index]);
        }
    }

    Ok(())
}

fn rgb565_to_rgba(value: u16, alpha: u8) -> [u8; 4] {
    let r = ((value >> 11) & 0x1f) as u8;
    let g = ((value >> 5) & 0x3f) as u8;
    let b = (value & 0x1f) as u8;
    [
        (r << 3) | (r >> 2),
        (g << 2) | (g >> 4),
        (b << 3) | (b >> 2),
        alpha,
    ]
}

fn lerp_color(
    a: [u8; 4],
    b: [u8; 4],
    a_weight: u16,
    b_weight: u16,
    div: u16,
    alpha: u8,
) -> [u8; 4] {
    [
        ((u16::from(a[0]) * a_weight + u16::from(b[0]) * b_weight) / div) as u8,
        ((u16::from(a[1]) * a_weight + u16::from(b[1]) * b_weight) / div) as u8,
        ((u16::from(a[2]) * a_weight + u16::from(b[2]) * b_weight) / div) as u8,
        alpha,
    ]
}

fn mip_dimension(value: u32, level: usize) -> u32 {
    (value >> level).max(1)
}

fn pixel_count(width: u32, height: u32) -> FormatResult<usize> {
    usize::try_from(width)
        .ok()
        .and_then(|w| usize::try_from(height).ok().and_then(|h| w.checked_mul(h)))
        .ok_or(FormatError::InvalidRange {
            field: "BLP pixel count",
        })
}
