//! Terrain preview window for the Rust rewrite.

use std::collections::HashSet;
use std::env;
use std::path::PathBuf;
use std::process::ExitCode;
use std::thread;
use std::time::Duration;

use minifb::{Key, MouseButton, MouseMode, Window, WindowOptions};
use noggit_core::WorldMap;
use noggit_formats::blp::{BlpFile, RgbaImage};
use noggit_render::{TerrainBounds, TerrainMesh, TerrainVertex, build_terrain_mesh};
use noggit_vfs::{FileSource, VfsPath, WowClient, WowClientConfig};

const INITIAL_WIDTH: usize = 1280;
const INITIAL_HEIGHT: usize = 720;
const BACKGROUND: u32 = 0x101417;
const MAX_WIREFRAME_LINES: usize = 55_000;

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("{err}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let options = PreviewOptions::parse(env::args().skip(1))?;
    let world =
        WorldMap::load_from_local_directory(&options.map_path).map_err(|err| err.to_string())?;
    let mesh = build_terrain_mesh(&world).map_err(|err| err.to_string())?;
    let material_colors = load_material_colors(mesh.materials(), &options)?;
    let preview = PreviewMesh::from_mesh(mesh, material_colors)?;
    let mut camera = Camera::new(preview.bounds);
    let mut window = Window::new(
        &format!("Noggit Rust - {}", world.name()),
        INITIAL_WIDTH,
        INITIAL_HEIGHT,
        WindowOptions {
            resize: true,
            ..WindowOptions::default()
        },
    )
    .map_err(|err| format!("failed to open preview window: {err}"))?;
    let mut buffer = vec![BACKGROUND; INITIAL_WIDTH * INITIAL_HEIGHT];
    let mut width = INITIAL_WIDTH;
    let mut height = INITIAL_HEIGHT;
    let mut dirty = true;

    while window.is_open() && !window.is_key_down(Key::Escape) {
        let (new_width, new_height) = window.get_size();
        if new_width != width || new_height != height {
            width = new_width.max(1);
            height = new_height.max(1);
            buffer.resize(width * height, BACKGROUND);
            dirty = true;
        }

        if camera.update(&window) {
            dirty = true;
        }
        if dirty {
            draw_preview(&preview, &camera, &mut buffer, width, height);
            dirty = false;
        }
        window
            .update_with_buffer(&buffer, width, height)
            .map_err(|err| format!("failed to update preview window: {err}"))?;
        thread::sleep(Duration::from_millis(16));
    }

    Ok(())
}

fn usage() -> String {
    "usage: cargo run -p noggit-ui -- <local-map-directory> [--client <client-path>] [--extra-mpq <mpq> ...]".to_string()
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct PreviewOptions {
    map_path: PathBuf,
    client_path: Option<PathBuf>,
    extra_archives: Vec<PathBuf>,
}

impl PreviewOptions {
    fn parse(args: impl IntoIterator<Item = String>) -> Result<Self, String> {
        let mut args = args.into_iter();
        let mut map_path = None;
        let mut client_path = None;
        let mut extra_archives = Vec::new();

        while let Some(arg) = args.next() {
            match arg.as_str() {
                "--client" => {
                    let path = args.next().ok_or_else(usage)?;
                    client_path = Some(PathBuf::from(path));
                }
                "--extra-mpq" => {
                    let path = args.next().ok_or_else(usage)?;
                    extra_archives.push(PathBuf::from(path));
                }
                value if value.starts_with("--") => return Err(usage()),
                _ => {
                    if map_path.is_some() {
                        return Err(usage());
                    }
                    map_path = Some(PathBuf::from(arg));
                }
            }
        }

        if client_path.is_none() && !extra_archives.is_empty() {
            return Err(format!("--extra-mpq requires --client\n{}", usage()));
        }

        Ok(Self {
            map_path: map_path.ok_or_else(usage)?,
            client_path,
            extra_archives,
        })
    }
}

fn load_material_colors(
    materials: &[String],
    options: &PreviewOptions,
) -> Result<Vec<Option<u32>>, String> {
    let Some(client_path) = &options.client_path else {
        return Ok(vec![None; materials.len()]);
    };

    let client = WowClient::open_with_config(
        client_path,
        WowClientConfig {
            extra_archives: options.extra_archives.clone(),
            ..WowClientConfig::default()
        },
    )
    .map_err(|err| format!("failed to open WoW client {}: {err}", client_path.display()))?;

    let colors = materials
        .iter()
        .map(|material| load_material_color(&client, material))
        .collect::<Vec<_>>();
    let resolved = colors.iter().filter(|color| color.is_some()).count();
    eprintln!(
        "terrain texture colors resolved: {resolved}/{}",
        materials.len()
    );

    Ok(colors)
}

fn load_material_color(client: &WowClient, material: &str) -> Option<u32> {
    let path = VfsPath::new(material).ok()?;
    let bytes = client.read_file(&path).ok()?;
    let blp = BlpFile::parse(&bytes).ok()?;
    let image = blp.decode_rgba_mipmap(0).ok()?;
    average_image_color(&image)
}

fn average_image_color(image: &RgbaImage) -> Option<u32> {
    let mut red = 0_u64;
    let mut green = 0_u64;
    let mut blue = 0_u64;
    let mut weight = 0_u64;

    for pixel in image.pixels.chunks_exact(4) {
        let alpha = u64::from(pixel[3]);
        if alpha == 0 {
            continue;
        }
        red += u64::from(pixel[0]) * alpha;
        green += u64::from(pixel[1]) * alpha;
        blue += u64::from(pixel[2]) * alpha;
        weight += alpha;
    }

    if weight == 0 {
        return None;
    }

    Some(
        (((red / weight) as u32) << 16) | (((green / weight) as u32) << 8) | (blue / weight) as u32,
    )
}

struct PreviewMesh {
    vertices: Vec<TerrainVertex>,
    lines: Vec<[u32; 2]>,
    material_colors: Vec<Option<u32>>,
    bounds: TerrainBounds,
}

impl PreviewMesh {
    fn from_mesh(mesh: TerrainMesh, material_colors: Vec<Option<u32>>) -> Result<Self, String> {
        let Some(bounds) = mesh.bounds() else {
            return Err("map has no renderable terrain chunks".to_string());
        };

        let lines = sampled_wireframe_lines(mesh.indices());
        Ok(Self {
            vertices: mesh.vertices().to_vec(),
            lines,
            material_colors,
            bounds,
        })
    }
}

struct Camera {
    target: [f32; 3],
    yaw: f32,
    pitch: f32,
    distance: f32,
    last_mouse: Option<(f32, f32)>,
}

impl Camera {
    fn new(bounds: TerrainBounds) -> Self {
        let size = bounds.size();
        let planar_size = size[0].max(size[2]).max(1.0);
        let mut target = bounds.center();
        target[1] += size[1] * 0.15;

        Self {
            target,
            yaw: 0.75,
            pitch: 0.65,
            distance: planar_size * 1.35,
            last_mouse: None,
        }
    }

    fn update(&mut self, window: &Window) -> bool {
        let previous_target = self.target;
        let previous_yaw = self.yaw;
        let previous_pitch = self.pitch;
        let previous_distance = self.distance;
        let turn_step = 0.035;
        let zoom_step = self.distance * 0.035;
        let pan_step = self.distance * 0.012;

        if window.is_key_down(Key::Left) {
            self.yaw -= turn_step;
        }
        if window.is_key_down(Key::Right) {
            self.yaw += turn_step;
        }
        if window.is_key_down(Key::Up) {
            self.pitch += turn_step;
        }
        if window.is_key_down(Key::Down) {
            self.pitch -= turn_step;
        }
        if window.is_key_down(Key::Q) {
            self.distance += zoom_step;
        }
        if window.is_key_down(Key::E) {
            self.distance = (self.distance - zoom_step).max(20.0);
        }

        let basis = self.basis();
        if window.is_key_down(Key::W) {
            self.target = add(
                self.target,
                scale(horizontal_forward(basis.forward), pan_step),
            );
        }
        if window.is_key_down(Key::S) {
            self.target = sub(
                self.target,
                scale(horizontal_forward(basis.forward), pan_step),
            );
        }
        if window.is_key_down(Key::A) {
            self.target = sub(self.target, scale(basis.right, pan_step));
        }
        if window.is_key_down(Key::D) {
            self.target = add(self.target, scale(basis.right, pan_step));
        }

        if window.get_mouse_down(MouseButton::Left) {
            if let Some(position) = window.get_mouse_pos(MouseMode::Clamp) {
                if let Some(last) = self.last_mouse {
                    self.yaw += (position.0 - last.0) * 0.006;
                    self.pitch += (position.1 - last.1) * 0.006;
                }
                self.last_mouse = Some(position);
            }
        } else {
            self.last_mouse = None;
        }

        self.pitch = self.pitch.clamp(0.12, 1.42);
        self.distance = self.distance.clamp(20.0, 4000.0);

        self.target != previous_target
            || (self.yaw - previous_yaw).abs() > f32::EPSILON
            || (self.pitch - previous_pitch).abs() > f32::EPSILON
            || (self.distance - previous_distance).abs() > f32::EPSILON
    }

    fn basis(&self) -> CameraBasis {
        let offset = [
            self.yaw.sin() * self.pitch.cos() * self.distance,
            self.pitch.sin() * self.distance,
            self.yaw.cos() * self.pitch.cos() * self.distance,
        ];
        let position = add(self.target, offset);
        let forward = normalize(sub(self.target, position));
        let right = normalize(cross(forward, [0.0, 1.0, 0.0]));
        let up = cross(right, forward);

        CameraBasis {
            position,
            forward,
            right,
            up,
        }
    }
}

#[derive(Clone, Copy)]
struct CameraBasis {
    position: [f32; 3],
    forward: [f32; 3],
    right: [f32; 3],
    up: [f32; 3],
}

#[derive(Clone, Copy)]
struct ScreenPoint {
    x: i32,
    y: i32,
}

fn sampled_wireframe_lines(indices: &[u32]) -> Vec<[u32; 2]> {
    let mut seen = HashSet::new();
    let mut lines = Vec::new();

    for triangle in indices.chunks_exact(3) {
        add_edge(triangle[0], triangle[1], &mut seen, &mut lines);
        add_edge(triangle[1], triangle[2], &mut seen, &mut lines);
        add_edge(triangle[2], triangle[0], &mut seen, &mut lines);
    }

    let stride = (lines.len() / MAX_WIREFRAME_LINES).max(1);
    lines.into_iter().step_by(stride).collect()
}

fn add_edge(a: u32, b: u32, seen: &mut HashSet<(u32, u32)>, lines: &mut Vec<[u32; 2]>) {
    let edge = if a < b { (a, b) } else { (b, a) };
    if seen.insert(edge) {
        lines.push([edge.0, edge.1]);
    }
}

fn draw_preview(
    preview: &PreviewMesh,
    camera: &Camera,
    buffer: &mut [u32],
    width: usize,
    height: usize,
) {
    buffer.fill(BACKGROUND);
    let basis = camera.basis();

    for line in &preview.lines {
        let Some(a) = preview.vertices.get(line[0] as usize) else {
            continue;
        };
        let Some(b) = preview.vertices.get(line[1] as usize) else {
            continue;
        };
        let Some(start) = project(a.position, basis, width, height) else {
            continue;
        };
        let Some(end) = project(b.position, basis, width, height) else {
            continue;
        };
        let Some((start, end)) = clip_line_to_viewport(start, end, width, height) else {
            continue;
        };
        let color = line_color(preview, a, b);
        draw_line(buffer, width, height, start, end, color);
    }
}

fn line_color(preview: &PreviewMesh, a: &TerrainVertex, b: &TerrainVertex) -> u32 {
    let height = (a.position[1] + b.position[1]) * 0.5;
    let Some(color) = line_material_color(preview, a, b) else {
        return height_color(height, preview.bounds);
    };
    let range = (preview.bounds.max[1] - preview.bounds.min[1]).max(1.0);
    let t = ((height - preview.bounds.min[1]) / range).clamp(0.0, 1.0);

    scale_color(color, 0.72 + t * 0.28)
}

fn line_material_color(preview: &PreviewMesh, a: &TerrainVertex, b: &TerrainVertex) -> Option<u32> {
    match (
        vertex_material_color(preview, a),
        vertex_material_color(preview, b),
    ) {
        (Some(a), Some(b)) if a != b => Some(mix_color(a, b, 0.5)),
        (Some(color), _) | (_, Some(color)) => Some(color),
        (None, None) => None,
    }
}

fn vertex_material_color(preview: &PreviewMesh, vertex: &TerrainVertex) -> Option<u32> {
    let material_id = usize::try_from(vertex.material_id?).ok()?;
    *preview.material_colors.get(material_id)?
}

fn clip_line_to_viewport(
    start: ScreenPoint,
    end: ScreenPoint,
    width: usize,
    height: usize,
) -> Option<(ScreenPoint, ScreenPoint)> {
    let x_min = 0.0;
    let y_min = 0.0;
    let x_max = width.saturating_sub(1) as f32;
    let y_max = height.saturating_sub(1) as f32;
    let mut x0 = start.x as f32;
    let mut y0 = start.y as f32;
    let mut x1 = end.x as f32;
    let mut y1 = end.y as f32;

    loop {
        let code0 = viewport_out_code(x0, y0, x_max, y_max);
        let code1 = viewport_out_code(x1, y1, x_max, y_max);

        if code0 | code1 == 0 {
            return Some((
                ScreenPoint {
                    x: x0.round() as i32,
                    y: y0.round() as i32,
                },
                ScreenPoint {
                    x: x1.round() as i32,
                    y: y1.round() as i32,
                },
            ));
        }
        if code0 & code1 != 0 {
            return None;
        }

        let code = if code0 != 0 { code0 } else { code1 };
        let (x, y) = if code & OUT_TOP != 0 {
            if (y1 - y0).abs() <= f32::EPSILON {
                return None;
            }
            (x0 + (x1 - x0) * (y_min - y0) / (y1 - y0), y_min)
        } else if code & OUT_BOTTOM != 0 {
            if (y1 - y0).abs() <= f32::EPSILON {
                return None;
            }
            (x0 + (x1 - x0) * (y_max - y0) / (y1 - y0), y_max)
        } else if code & OUT_RIGHT != 0 {
            if (x1 - x0).abs() <= f32::EPSILON {
                return None;
            }
            (x_max, y0 + (y1 - y0) * (x_max - x0) / (x1 - x0))
        } else {
            if (x1 - x0).abs() <= f32::EPSILON {
                return None;
            }
            (x_min, y0 + (y1 - y0) * (x_min - x0) / (x1 - x0))
        };

        if code == code0 {
            x0 = x;
            y0 = y;
        } else {
            x1 = x;
            y1 = y;
        }
    }
}

const OUT_LEFT: u8 = 0b0001;
const OUT_RIGHT: u8 = 0b0010;
const OUT_TOP: u8 = 0b0100;
const OUT_BOTTOM: u8 = 0b1000;

fn viewport_out_code(x: f32, y: f32, x_max: f32, y_max: f32) -> u8 {
    let mut code = 0;
    if x < 0.0 {
        code |= OUT_LEFT;
    } else if x > x_max {
        code |= OUT_RIGHT;
    }
    if y < 0.0 {
        code |= OUT_TOP;
    } else if y > y_max {
        code |= OUT_BOTTOM;
    }
    code
}

fn project(
    point: [f32; 3],
    basis: CameraBasis,
    width: usize,
    height: usize,
) -> Option<ScreenPoint> {
    let relative = sub(point, basis.position);
    let depth = dot(relative, basis.forward);
    if depth <= 1.0 {
        return None;
    }

    let focal = height as f32 * 0.9;
    let x = dot(relative, basis.right);
    let y = dot(relative, basis.up);
    let screen_x = width as f32 * 0.5 + (x / depth) * focal;
    let screen_y = height as f32 * 0.52 - (y / depth) * focal;

    Some(ScreenPoint {
        x: screen_x.round() as i32,
        y: screen_y.round() as i32,
    })
}

fn draw_line(
    buffer: &mut [u32],
    width: usize,
    height: usize,
    start: ScreenPoint,
    end: ScreenPoint,
    color: u32,
) {
    let mut x0 = start.x;
    let mut y0 = start.y;
    let x1 = end.x;
    let y1 = end.y;
    let dx = (x1 - x0).abs();
    let sx = if x0 < x1 { 1 } else { -1 };
    let dy = -(y1 - y0).abs();
    let sy = if y0 < y1 { 1 } else { -1 };
    let mut error = dx + dy;

    loop {
        put_pixel(buffer, width, height, x0, y0, color);
        if x0 == x1 && y0 == y1 {
            break;
        }
        let e2 = error * 2;
        if e2 >= dy {
            error += dy;
            x0 += sx;
        }
        if e2 <= dx {
            error += dx;
            y0 += sy;
        }
    }
}

fn put_pixel(buffer: &mut [u32], width: usize, height: usize, x: i32, y: i32, color: u32) {
    if x < 0 || y < 0 {
        return;
    }

    let x = x as usize;
    let y = y as usize;
    if x >= width || y >= height {
        return;
    }

    buffer[y * width + x] = color;
}

fn height_color(height: f32, bounds: TerrainBounds) -> u32 {
    let range = (bounds.max[1] - bounds.min[1]).max(1.0);
    let t = ((height - bounds.min[1]) / range).clamp(0.0, 1.0);

    if t < 0.42 {
        mix_color(0x365f47, 0x8a7a45, t / 0.42)
    } else {
        mix_color(0x8a7a45, 0xd2d5c8, (t - 0.42) / 0.58)
    }
}

fn mix_color(a: u32, b: u32, t: f32) -> u32 {
    let ar = ((a >> 16) & 0xff) as f32;
    let ag = ((a >> 8) & 0xff) as f32;
    let ab = (a & 0xff) as f32;
    let br = ((b >> 16) & 0xff) as f32;
    let bg = ((b >> 8) & 0xff) as f32;
    let bb = (b & 0xff) as f32;
    let r = ar + (br - ar) * t;
    let g = ag + (bg - ag) * t;
    let b = ab + (bb - ab) * t;

    ((r as u32) << 16) | ((g as u32) << 8) | b as u32
}

fn scale_color(color: u32, factor: f32) -> u32 {
    let factor = factor.max(0.0);
    let red = (((color >> 16) & 0xff) as f32 * factor).min(255.0);
    let green = (((color >> 8) & 0xff) as f32 * factor).min(255.0);
    let blue = ((color & 0xff) as f32 * factor).min(255.0);

    ((red as u32) << 16) | ((green as u32) << 8) | blue as u32
}

fn horizontal_forward(forward: [f32; 3]) -> [f32; 3] {
    normalize([forward[0], 0.0, forward[2]])
}

fn add(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn scale(value: [f32; 3], factor: f32) -> [f32; 3] {
    [value[0] * factor, value[1] * factor, value[2] * factor]
}

fn dot(a: [f32; 3], b: [f32; 3]) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

fn normalize(value: [f32; 3]) -> [f32; 3] {
    let length = dot(value, value).sqrt();
    if length <= f32::EPSILON {
        [0.0, 0.0, 0.0]
    } else {
        scale(value, 1.0 / length)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_preview_options_with_client_and_extra_archives() -> Result<(), String> {
        let options = PreviewOptions::parse([
            "/maps/guerilla".to_string(),
            "--client".to_string(),
            "/wow".to_string(),
            "--extra-mpq".to_string(),
            "/maps/patch-guerilla.MPQ".to_string(),
        ])?;

        assert_eq!(options.map_path, PathBuf::from("/maps/guerilla"));
        assert_eq!(options.client_path, Some(PathBuf::from("/wow")));
        assert_eq!(
            options.extra_archives,
            vec![PathBuf::from("/maps/patch-guerilla.MPQ")]
        );
        Ok(())
    }

    #[test]
    fn averages_rgba_color_with_alpha_weighting() {
        let image = RgbaImage {
            width: 2,
            height: 1,
            pixels: vec![200, 100, 50, 255, 10, 10, 10, 0],
        };

        assert_eq!(average_image_color(&image), Some(0xc86432));
    }
}
