//! Terrain preview window for the Rust rewrite.

use std::borrow::Cow;
use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;

use bytemuck::{Pod, Zeroable};
use noggit_core::WorldMap;
use noggit_formats::blp::{BlpFile, RgbaImage};
use noggit_formats::m2::{M2File, M2SkinFile};
use noggit_formats::wmo::{WmoFile, WmoGroupFile};
use noggit_render::{
    M2Mesh, M2RenderAsset, M2RenderVertex, PlacementMarkerMesh, PlacementMarkerVertex,
    TerrainBounds, TerrainMesh, TerrainVertex, WaterMesh, WaterVertex, WmoMesh, WmoRenderAsset,
    WmoVertex, build_m2_mesh, build_placement_marker_mesh, build_terrain_mesh, build_water_mesh,
    build_wmo_mesh,
};
use noggit_vfs::{ArchiveLoadState, FileSource, VfsPath, WowClient, WowClientConfig};
use wgpu::util::DeviceExt;
use winit::dpi::{LogicalSize, PhysicalSize};
use winit::event::{ElementState, Event, MouseButton, MouseScrollDelta, WindowEvent};
use winit::event_loop::{ControlFlow, EventLoop};
use winit::keyboard::{KeyCode, PhysicalKey};
use winit::window::{Window, WindowBuilder};

const INITIAL_WIDTH: u32 = 1280;
const INITIAL_HEIGHT: u32 = 720;
const BACKGROUND: wgpu::Color = wgpu::Color {
    r: 0.063,
    g: 0.078,
    b: 0.091,
    a: 1.0,
};
const DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth24Plus;
const TERRAIN_DETAIL_UV_SCALE: f32 = 8.0;
const TERRAIN_ALPHA_MAP_SIDE: u32 = 64;
const TERRAIN_ALPHA_MAP_BYTES: usize = 64 * 64;

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
    let water_mesh = build_water_mesh(&world).map_err(|err| err.to_string())?;
    let marker_mesh = build_placement_marker_mesh(&world).map_err(|err| err.to_string())?;
    let client = open_preview_client(&options)?;
    let mut material_images = load_material_images("terrain", mesh.materials(), client.as_ref())?;
    let fallback_material_id = material_images.len();
    material_images.push(missing_material_image());
    let mut alpha_images = load_alpha_images(mesh.alpha_maps());
    let fallback_alpha_id = alpha_images.len();
    alpha_images.push(empty_alpha_image());
    let gpu_mesh = GpuMesh::from_terrain_mesh(mesh, fallback_material_id, fallback_alpha_id)?;
    log_water_mesh_summary(&water_mesh);
    let water_bounds = water_mesh.bounds();
    let gpu_water_mesh = GpuWaterMesh::from_water_mesh(water_mesh)?;
    let m2_assets = load_m2_assets(&world, client.as_ref())?;
    let m2_mesh = build_m2_mesh(&world, &m2_assets).map_err(|err| err.to_string())?;
    log_m2_mesh_summary(&m2_mesh);
    let m2_bounds = m2_mesh.bounds();
    let mut m2_material_images = load_material_images("M2", m2_mesh.materials(), client.as_ref())?;
    let fallback_m2_material_id = m2_material_images.len();
    m2_material_images.push(missing_material_image());
    let gpu_m2_mesh = GpuM2Mesh::from_m2_mesh(m2_mesh, fallback_m2_material_id)?;
    let wmo_assets = load_wmo_assets(&world, client.as_ref())?;
    let wmo_mesh = build_wmo_mesh(&world, &wmo_assets).map_err(|err| err.to_string())?;
    log_wmo_mesh_summary(&wmo_mesh);
    let wmo_bounds = wmo_mesh.bounds();
    let mut wmo_material_images =
        load_material_images("WMO", wmo_mesh.materials(), client.as_ref())?;
    let fallback_wmo_material_id = wmo_material_images.len();
    wmo_material_images.push(missing_material_image());
    let gpu_wmo_mesh = GpuWmoMesh::from_wmo_mesh(wmo_mesh, fallback_wmo_material_id)?;
    let gpu_marker_mesh = GpuMarkerMesh::from_marker_mesh(&marker_mesh)?;
    let mut camera = Camera::new(gpu_mesh.bounds);
    log_render_bounds(
        gpu_mesh.bounds,
        marker_mesh.bounds(),
        m2_bounds,
        wmo_bounds,
        water_bounds,
    );
    eprintln!(
        "object placement markers: {} placements, {} lines",
        marker_mesh.marker_count(),
        marker_mesh.indices().len() / 2
    );

    let event_loop =
        EventLoop::new().map_err(|err| format!("failed to create event loop: {err}"))?;
    let window = WindowBuilder::new()
        .with_title(format!("Noggit Rust GPU - {}", world.name()))
        .with_inner_size(LogicalSize::new(
            f64::from(INITIAL_WIDTH),
            f64::from(INITIAL_HEIGHT),
        ))
        .build(&event_loop)
        .map_err(|err| format!("failed to open preview window: {err}"))?;
    let window: &'static Window = Box::leak(Box::new(window));
    let mut gpu = pollster::block_on(GpuState::new(
        window,
        &gpu_mesh,
        &gpu_water_mesh,
        &gpu_m2_mesh,
        &gpu_wmo_mesh,
        &gpu_marker_mesh,
        &material_images,
        &alpha_images,
        &m2_material_images,
        &wmo_material_images,
        camera.uniform(window.inner_size()),
    ))?;
    let mut input = InputState::default();
    let mut view_options = ViewOptions::default();

    event_loop
        .run(move |event, target| {
            target.set_control_flow(ControlFlow::Poll);

            match event {
                Event::WindowEvent { event, window_id } if window_id == window.id() => {
                    match event {
                        WindowEvent::CloseRequested => target.exit(),
                        WindowEvent::KeyboardInput { event, .. } => {
                            if let PhysicalKey::Code(code) = event.physical_key {
                                if code == KeyCode::Escape && event.state == ElementState::Pressed {
                                    target.exit();
                                }
                                if event.state == ElementState::Pressed && !event.repeat {
                                    view_options.handle_key_press(code);
                                }
                                input.set_key(code, event.state);
                            }
                        }
                        WindowEvent::MouseInput { state, button, .. } => {
                            input.set_mouse_button(button, state);
                        }
                        WindowEvent::CursorMoved { position, .. } => {
                            input.set_cursor_position(position.x as f32, position.y as f32);
                        }
                        WindowEvent::MouseWheel { delta, .. } => {
                            input.add_scroll(delta);
                        }
                        WindowEvent::Resized(size) => {
                            gpu.resize(size);
                        }
                        WindowEvent::RedrawRequested => {
                            if camera.update(&mut input) {
                                gpu.update_camera(camera.uniform(window.inner_size()));
                            }

                            match gpu.render(&view_options) {
                                Ok(()) => {}
                                Err(wgpu::SurfaceError::Lost | wgpu::SurfaceError::Outdated) => {
                                    gpu.resize(window.inner_size());
                                }
                                Err(wgpu::SurfaceError::OutOfMemory) => target.exit(),
                                Err(wgpu::SurfaceError::Timeout) => {
                                    eprintln!("surface render timeout");
                                }
                            }
                        }
                        _ => {}
                    }
                }
                Event::AboutToWait => {
                    window.request_redraw();
                }
                _ => {}
            }
        })
        .map_err(|err| format!("event loop failed: {err}"))
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

#[derive(Debug, Clone, PartialEq, Eq)]
struct MaterialImage {
    mipmaps: Vec<RgbaImage>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct AlphaImage {
    values: [u8; TERRAIN_ALPHA_MAP_BYTES],
}

fn open_preview_client(options: &PreviewOptions) -> Result<Option<WowClient>, String> {
    let Some(client_path) = &options.client_path else {
        return Ok(None);
    };

    let client = WowClient::open_with_config(
        client_path,
        WowClientConfig {
            extra_archives: options.extra_archives.clone(),
            ..WowClientConfig::default()
        },
    )
    .map_err(|err| format!("failed to open WoW client {}: {err}", client_path.display()))?;
    log_client_archive_summary(&client);
    Ok(Some(client))
}

fn load_material_images(
    label: &str,
    materials: &[String],
    client: Option<&WowClient>,
) -> Result<Vec<MaterialImage>, String> {
    let Some(client) = client else {
        return Ok(vec![missing_material_image(); materials.len()]);
    };

    let mut resolved = 0usize;
    let images = materials
        .iter()
        .map(|material| match load_material_image(client, material) {
            Some(image) => {
                resolved += 1;
                image
            }
            None => missing_material_image(),
        })
        .collect::<Vec<_>>();
    eprintln!("{label} textures loaded: {resolved}/{}", materials.len());

    Ok(images)
}

fn load_m2_assets(
    world: &WorldMap,
    client: Option<&WowClient>,
) -> Result<BTreeMap<String, M2RenderAsset>, String> {
    let Some(client) = client else {
        return Ok(BTreeMap::new());
    };

    let mut asset_names = BTreeSet::new();
    for tile in world.tiles() {
        for placement in tile.model_placements() {
            if let Some(asset) = placement.asset.as_ref() {
                asset_names.insert(asset.clone());
            }
        }
    }

    let mut assets = BTreeMap::new();
    for asset in &asset_names {
        match load_m2_asset(client, asset) {
            Ok(asset_data) => {
                assets.insert(asset.clone(), asset_data);
            }
            Err(error) => {
                eprintln!("M2 asset skipped: {asset}: {error}");
            }
        }
    }
    eprintln!("M2 assets loaded: {}/{}", assets.len(), asset_names.len());

    Ok(assets)
}

fn load_m2_asset(client: &WowClient, asset: &str) -> Result<M2RenderAsset, String> {
    let (model_path, model_bytes) = read_first_existing(client, &m2_model_path_candidates(asset))?;
    let model = M2File::parse(&model_bytes)
        .map_err(|err| format!("failed to parse model {model_path}: {err}"))?;
    let (_, skin_bytes) = read_first_existing(client, &m2_skin_path_candidates(&model_path)?)?;
    let skin = M2SkinFile::parse(&skin_bytes)
        .map_err(|err| format!("failed to parse skin for {model_path}: {err}"))?;

    Ok(M2RenderAsset { model, skin })
}

fn read_first_existing(
    client: &WowClient,
    candidates: &[String],
) -> Result<(String, Vec<u8>), String> {
    let mut not_found = Vec::new();
    for candidate in candidates {
        let path = VfsPath::new(candidate)
            .map_err(|err| format!("invalid virtual path {candidate}: {err}"))?;
        match client.read_file(&path) {
            Ok(bytes) => return Ok((candidate.clone(), bytes)),
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => {
                not_found.push(candidate.clone());
            }
            Err(err) => return Err(format!("failed to read {candidate}: {err}")),
        }
    }

    Err(format!("missing candidates: {}", not_found.join(", ")))
}

fn m2_model_path_candidates(asset: &str) -> Vec<String> {
    let mut candidates = vec![asset.to_string()];
    let lower = asset.to_ascii_lowercase();
    if lower.ends_with(".mdx") {
        candidates.push(format!("{}m2", &asset[..asset.len() - 3]));
    } else if lower.ends_with(".m2") {
        candidates.push(format!("{}mdx", &asset[..asset.len() - 2]));
    }
    dedup_strings(candidates)
}

fn m2_skin_path_candidates(asset: &str) -> Result<Vec<String>, String> {
    let lower = asset.to_ascii_lowercase();
    let candidates = if lower.ends_with(".m2") {
        vec![format!("{}00.skin", &asset[..asset.len() - 3])]
    } else if lower.ends_with(".mdx") {
        vec![
            format!("{}00.skin", &asset[..asset.len() - 4]),
            format!("{}00.skin", &asset[..asset.len() - 3]),
        ]
    } else {
        return Err("M2 path does not end with .m2 or .mdx".to_string());
    };

    Ok(dedup_strings(candidates))
}

fn dedup_strings(values: Vec<String>) -> Vec<String> {
    let mut seen = BTreeSet::new();
    let mut out = Vec::new();
    for value in values {
        if seen.insert(value.clone()) {
            out.push(value);
        }
    }
    out
}

fn load_wmo_assets(
    world: &WorldMap,
    client: Option<&WowClient>,
) -> Result<BTreeMap<String, WmoRenderAsset>, String> {
    let Some(client) = client else {
        return Ok(BTreeMap::new());
    };

    let mut asset_names = BTreeSet::new();
    for tile in world.tiles() {
        for placement in tile.wmo_placements() {
            if let Some(asset) = placement.asset.as_ref() {
                asset_names.insert(asset.clone());
            }
        }
    }

    let mut assets = BTreeMap::new();
    for asset in &asset_names {
        match load_wmo_asset(client, asset) {
            Ok(asset_data) => {
                assets.insert(asset.clone(), asset_data);
            }
            Err(error) => {
                eprintln!("WMO asset skipped: {asset}: {error}");
            }
        }
    }
    eprintln!("WMO assets loaded: {}/{}", assets.len(), asset_names.len());

    Ok(assets)
}

fn load_wmo_asset(client: &WowClient, asset: &str) -> Result<WmoRenderAsset, String> {
    let root_path =
        VfsPath::new(asset).map_err(|err| format!("invalid WMO path {asset}: {err}"))?;
    let root_bytes = client
        .read_file(&root_path)
        .map_err(|err| format!("failed to read root WMO: {err}"))?;
    let root = WmoFile::parse(&root_bytes).map_err(|err| format!("failed to parse root: {err}"))?;
    let group_count = root
        .header()
        .map_err(|err| format!("failed to parse root header: {err}"))?
        .map(|header| header.group_count)
        .unwrap_or(0);
    let mut groups = Vec::new();

    for group_index in 0..group_count {
        let group_path = wmo_group_path(asset, group_index)?;
        let group_vfs_path = VfsPath::new(&group_path)
            .map_err(|err| format!("invalid WMO group path {group_path}: {err}"))?;
        let group_bytes = client
            .read_file(&group_vfs_path)
            .map_err(|err| format!("failed to read group {group_index}: {err}"))?;
        let group = WmoGroupFile::parse(&group_bytes)
            .map_err(|err| format!("failed to parse group {group_index}: {err}"))?;
        groups.push(group);
    }

    Ok(WmoRenderAsset { root, groups })
}

fn wmo_group_path(asset: &str, group_index: u32) -> Result<String, String> {
    let lower = asset.to_ascii_lowercase();
    if !lower.ends_with(".wmo") {
        return Err("WMO path does not end with .wmo".to_string());
    }

    let stem = &asset[..asset.len() - 4];
    Ok(format!("{stem}_{group_index:03}.wmo"))
}

fn log_client_archive_summary(client: &WowClient) {
    let loaded = client
        .archive_reports()
        .iter()
        .filter(|report| report.state == ArchiveLoadState::Loaded)
        .count();
    let skipped = client
        .archive_reports()
        .iter()
        .filter(|report| matches!(report.state, ArchiveLoadState::SkippedTooLarge { .. }))
        .count();
    let failed = client
        .archive_reports()
        .iter()
        .filter(|report| matches!(report.state, ArchiveLoadState::Failed { .. }))
        .count();
    eprintln!(
        "client MPQs: loaded={loaded} skipped={skipped} failed={failed} data={}",
        client.data_root().display()
    );
}

fn load_material_image(client: &WowClient, material: &str) -> Option<MaterialImage> {
    let path = VfsPath::new(material).ok()?;
    let bytes = client.read_file(&path).ok()?;
    let blp = BlpFile::parse(&bytes).ok()?;
    let mut mipmaps = decode_blp_mipmaps(&blp)?;
    complete_material_mipmaps(&mut mipmaps);
    Some(MaterialImage { mipmaps })
}

fn missing_material_image() -> MaterialImage {
    let mut pixels = Vec::with_capacity(4 * 4 * 4);
    for y in 0..4 {
        for x in 0..4 {
            if (x + y) % 2 == 0 {
                pixels.extend_from_slice(&[180, 24, 180, 255]);
            } else {
                pixels.extend_from_slice(&[24, 24, 24, 255]);
            }
        }
    }

    let mut mipmaps = vec![RgbaImage {
        width: 4,
        height: 4,
        pixels,
    }];
    complete_material_mipmaps(&mut mipmaps);

    MaterialImage { mipmaps }
}

fn decode_blp_mipmaps(blp: &BlpFile) -> Option<Vec<RgbaImage>> {
    let mut mipmaps = Vec::new();
    for (level, (&offset, &size)) in blp
        .header()
        .mipmap_offsets
        .iter()
        .zip(blp.header().mipmap_sizes.iter())
        .enumerate()
    {
        if offset == 0 || size == 0 {
            break;
        }

        match blp.decode_rgba_mipmap(level) {
            Ok(image) => mipmaps.push(image),
            Err(_) if !mipmaps.is_empty() => break,
            Err(_) => return None,
        }
    }

    if mipmaps.is_empty() {
        None
    } else {
        Some(mipmaps)
    }
}

fn complete_material_mipmaps(mipmaps: &mut Vec<RgbaImage>) {
    while mipmaps
        .last()
        .is_some_and(|image| image.width > 1 || image.height > 1)
    {
        let Some(next) = mipmaps.last().and_then(downsample_rgba) else {
            break;
        };
        mipmaps.push(next);
    }
}

fn downsample_rgba(image: &RgbaImage) -> Option<RgbaImage> {
    if image.width == 0 || image.height == 0 {
        return None;
    }

    let width = (image.width / 2).max(1);
    let height = (image.height / 2).max(1);
    let pixel_count = usize::try_from(width)
        .ok()
        .and_then(|w| usize::try_from(height).ok().and_then(|h| w.checked_mul(h)))?;
    let mut pixels = Vec::with_capacity(pixel_count * 4);

    for y in 0..height {
        for x in 0..width {
            let mut sum = [0_u16; 4];
            for offset_y in 0..2 {
                for offset_x in 0..2 {
                    let source_x = (x * 2 + offset_x).min(image.width - 1);
                    let source_y = (y * 2 + offset_y).min(image.height - 1);
                    let pixel_offset = rgba_pixel_offset(image.width, source_x, source_y)?;
                    for (channel, value) in sum.iter_mut().enumerate() {
                        *value += u16::from(image.pixels[pixel_offset + channel]);
                    }
                }
            }
            pixels.extend(sum.map(|value| (value / 4) as u8));
        }
    }

    Some(RgbaImage {
        width,
        height,
        pixels,
    })
}

fn rgba_pixel_offset(width: u32, x: u32, y: u32) -> Option<usize> {
    usize::try_from(y)
        .ok()
        .and_then(|y| {
            usize::try_from(width)
                .ok()
                .and_then(|width| y.checked_mul(width))
        })
        .and_then(|row| usize::try_from(x).ok().and_then(|x| row.checked_add(x)))
        .and_then(|pixel| pixel.checked_mul(4))
}

fn load_alpha_images(alpha_maps: &[noggit_render::TerrainAlphaMap]) -> Vec<AlphaImage> {
    alpha_maps
        .iter()
        .map(|alpha| AlphaImage {
            values: alpha.values,
        })
        .collect()
}

fn empty_alpha_image() -> AlphaImage {
    AlphaImage {
        values: [0; TERRAIN_ALPHA_MAP_BYTES],
    }
}

fn log_wmo_mesh_summary(mesh: &WmoMesh) {
    eprintln!(
        "WMO mesh: placements={} assets={} materials={} vertices={} triangles={}",
        mesh.placement_count(),
        mesh.loaded_asset_count(),
        mesh.materials().len(),
        mesh.vertices().len(),
        mesh.indices().len() / 3
    );
}

fn log_m2_mesh_summary(mesh: &M2Mesh) {
    eprintln!(
        "M2 mesh: placements={} assets={} materials={} vertices={} triangles={}",
        mesh.placement_count(),
        mesh.loaded_asset_count(),
        mesh.materials().len(),
        mesh.vertices().len(),
        mesh.indices().len() / 3
    );
}

fn log_water_mesh_summary(mesh: &WaterMesh) {
    eprintln!(
        "water mesh: layers={} vertices={} triangles={}",
        mesh.layer_count(),
        mesh.vertices().len(),
        mesh.indices().len() / 3
    );
}

fn log_render_bounds(
    terrain: TerrainBounds,
    markers: Option<TerrainBounds>,
    m2: Option<TerrainBounds>,
    wmo: Option<TerrainBounds>,
    water: Option<TerrainBounds>,
) {
    eprintln!(
        "terrain bounds: min=({:.1},{:.1},{:.1}) max=({:.1},{:.1},{:.1})",
        terrain.min[0],
        terrain.min[1],
        terrain.min[2],
        terrain.max[0],
        terrain.max[1],
        terrain.max[2]
    );
    if let Some(markers) = markers {
        eprintln!(
            "marker bounds: min=({:.1},{:.1},{:.1}) max=({:.1},{:.1},{:.1})",
            markers.min[0],
            markers.min[1],
            markers.min[2],
            markers.max[0],
            markers.max[1],
            markers.max[2]
        );
    } else {
        eprintln!("marker bounds: none");
    }
    if let Some(m2) = m2 {
        eprintln!(
            "M2 bounds: min=({:.1},{:.1},{:.1}) max=({:.1},{:.1},{:.1})",
            m2.min[0], m2.min[1], m2.min[2], m2.max[0], m2.max[1], m2.max[2]
        );
    } else {
        eprintln!("M2 bounds: none");
    }
    if let Some(wmo) = wmo {
        eprintln!(
            "WMO bounds: min=({:.1},{:.1},{:.1}) max=({:.1},{:.1},{:.1})",
            wmo.min[0], wmo.min[1], wmo.min[2], wmo.max[0], wmo.max[1], wmo.max[2]
        );
    } else {
        eprintln!("WMO bounds: none");
    }
    if let Some(water) = water {
        eprintln!(
            "water bounds: min=({:.1},{:.1},{:.1}) max=({:.1},{:.1},{:.1})",
            water.min[0], water.min[1], water.min[2], water.max[0], water.max[1], water.max[2]
        );
    } else {
        eprintln!("water bounds: none");
    }
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuVertex {
    position: [f32; 3],
    normal: [f32; 3],
    tex_coord: [f32; 2],
    alpha_coord: [f32; 2],
}

impl GpuVertex {
    fn layout() -> wgpu::VertexBufferLayout<'static> {
        wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<GpuVertex>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[
                wgpu::VertexAttribute {
                    offset: 0,
                    shader_location: 0,
                    format: wgpu::VertexFormat::Float32x3,
                },
                wgpu::VertexAttribute {
                    offset: std::mem::size_of::<[f32; 3]>() as wgpu::BufferAddress,
                    shader_location: 1,
                    format: wgpu::VertexFormat::Float32x3,
                },
                wgpu::VertexAttribute {
                    offset: (std::mem::size_of::<[f32; 3]>() * 2) as wgpu::BufferAddress,
                    shader_location: 2,
                    format: wgpu::VertexFormat::Float32x2,
                },
                wgpu::VertexAttribute {
                    offset: (std::mem::size_of::<[f32; 3]>() * 2 + std::mem::size_of::<[f32; 2]>())
                        as wgpu::BufferAddress,
                    shader_location: 3,
                    format: wgpu::VertexFormat::Float32x2,
                },
            ],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuMarkerVertex {
    position: [f32; 3],
    color: [f32; 4],
}

impl GpuMarkerVertex {
    fn layout() -> wgpu::VertexBufferLayout<'static> {
        wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<GpuMarkerVertex>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[
                wgpu::VertexAttribute {
                    offset: 0,
                    shader_location: 0,
                    format: wgpu::VertexFormat::Float32x3,
                },
                wgpu::VertexAttribute {
                    offset: std::mem::size_of::<[f32; 3]>() as wgpu::BufferAddress,
                    shader_location: 1,
                    format: wgpu::VertexFormat::Float32x4,
                },
            ],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuWmoVertex {
    position: [f32; 3],
    normal: [f32; 3],
    tex_coord: [f32; 2],
}

impl GpuWmoVertex {
    fn layout() -> wgpu::VertexBufferLayout<'static> {
        wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<GpuWmoVertex>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[
                wgpu::VertexAttribute {
                    offset: 0,
                    shader_location: 0,
                    format: wgpu::VertexFormat::Float32x3,
                },
                wgpu::VertexAttribute {
                    offset: std::mem::size_of::<[f32; 3]>() as wgpu::BufferAddress,
                    shader_location: 1,
                    format: wgpu::VertexFormat::Float32x3,
                },
                wgpu::VertexAttribute {
                    offset: (std::mem::size_of::<[f32; 3]>() * 2) as wgpu::BufferAddress,
                    shader_location: 2,
                    format: wgpu::VertexFormat::Float32x2,
                },
            ],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuWaterVertex {
    position: [f32; 3],
    tex_coord: [f32; 2],
    depth: f32,
    liquid_id: f32,
}

impl GpuWaterVertex {
    fn layout() -> wgpu::VertexBufferLayout<'static> {
        wgpu::VertexBufferLayout {
            array_stride: std::mem::size_of::<GpuWaterVertex>() as wgpu::BufferAddress,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &[
                wgpu::VertexAttribute {
                    offset: 0,
                    shader_location: 0,
                    format: wgpu::VertexFormat::Float32x3,
                },
                wgpu::VertexAttribute {
                    offset: std::mem::size_of::<[f32; 3]>() as wgpu::BufferAddress,
                    shader_location: 1,
                    format: wgpu::VertexFormat::Float32x2,
                },
                wgpu::VertexAttribute {
                    offset: (std::mem::size_of::<[f32; 3]>() + std::mem::size_of::<[f32; 2]>())
                        as wgpu::BufferAddress,
                    shader_location: 2,
                    format: wgpu::VertexFormat::Float32,
                },
                wgpu::VertexAttribute {
                    offset: (std::mem::size_of::<[f32; 3]>()
                        + std::mem::size_of::<[f32; 2]>()
                        + std::mem::size_of::<f32>())
                        as wgpu::BufferAddress,
                    shader_location: 3,
                    format: wgpu::VertexFormat::Float32,
                },
            ],
        }
    }
}

struct GpuMesh {
    groups: Vec<GpuMeshGroup>,
    bounds: TerrainBounds,
}

struct GpuMeshGroup {
    material_ids: [usize; 4],
    alpha_map_ids: [usize; 3],
    vertices: Vec<GpuVertex>,
    indices: Vec<u32>,
}

struct GpuMarkerMesh {
    vertices: Vec<GpuMarkerVertex>,
    indices: Vec<u32>,
}

struct GpuWaterMesh {
    vertices: Vec<GpuWaterVertex>,
    indices: Vec<u32>,
}

struct GpuWmoMesh {
    groups: Vec<GpuWmoMeshGroup>,
}

struct GpuWmoMeshGroup {
    material_id: usize,
    vertices: Vec<GpuWmoVertex>,
    indices: Vec<u32>,
}

struct GpuM2Mesh {
    groups: Vec<GpuM2MeshGroup>,
}

struct GpuM2MeshGroup {
    material_id: usize,
    vertices: Vec<GpuWmoVertex>,
    indices: Vec<u32>,
}

impl GpuMesh {
    fn from_terrain_mesh(
        mesh: TerrainMesh,
        fallback_material_id: usize,
        fallback_alpha_id: usize,
    ) -> Result<Self, String> {
        let Some(bounds) = mesh.bounds() else {
            return Err("map has no renderable terrain chunks".to_string());
        };
        if mesh.indices().is_empty() {
            return Err("map has no renderable terrain triangles".to_string());
        }

        let mut group_lookup = BTreeMap::new();
        let mut groups = Vec::new();

        for triangle in mesh.indices().chunks_exact(3) {
            let a = mesh
                .vertices()
                .get(triangle[0] as usize)
                .ok_or_else(|| "terrain triangle references missing vertex".to_string())?;
            let b = mesh
                .vertices()
                .get(triangle[1] as usize)
                .ok_or_else(|| "terrain triangle references missing vertex".to_string())?;
            let c = mesh
                .vertices()
                .get(triangle[2] as usize)
                .ok_or_else(|| "terrain triangle references missing vertex".to_string())?;
            let (material_ids, alpha_map_ids) =
                vertex_layer_set(a, fallback_material_id, fallback_alpha_id);
            let key = (material_ids, alpha_map_ids);
            let group_index = if let Some(index) = group_lookup.get(&key) {
                *index
            } else {
                let index = groups.len();
                groups.push(GpuMeshGroup {
                    material_ids,
                    alpha_map_ids,
                    vertices: Vec::new(),
                    indices: Vec::new(),
                });
                group_lookup.insert(key, index);
                index
            };
            let group = groups
                .get_mut(group_index)
                .ok_or_else(|| "terrain triangle references missing layer group".to_string())?;
            let base_index = u32::try_from(group.vertices.len())
                .map_err(|_| "terrain layer group has too many vertices".to_string())?;

            group.vertices.extend([a, b, c].map(gpu_vertex));
            group
                .indices
                .extend_from_slice(&[base_index, base_index + 1, base_index + 2]);
        }

        groups.retain(|group| !group.indices.is_empty());

        Ok(Self { groups, bounds })
    }
}

impl GpuWaterMesh {
    fn from_water_mesh(mesh: WaterMesh) -> Result<Self, String> {
        let _ = u32::try_from(mesh.indices().len())
            .map_err(|_| "water index buffer exceeds u32 draw range".to_string())?;

        Ok(Self {
            vertices: mesh.vertices().iter().map(gpu_water_vertex).collect(),
            indices: mesh.indices().to_vec(),
        })
    }
}

impl GpuWmoMesh {
    fn from_wmo_mesh(mesh: WmoMesh, fallback_material_id: usize) -> Result<Self, String> {
        let mut group_lookup = BTreeMap::new();
        let mut groups = Vec::new();

        for triangle in mesh.indices().chunks_exact(3) {
            let a = mesh
                .vertices()
                .get(triangle[0] as usize)
                .ok_or_else(|| "WMO triangle references missing vertex".to_string())?;
            let b = mesh
                .vertices()
                .get(triangle[1] as usize)
                .ok_or_else(|| "WMO triangle references missing vertex".to_string())?;
            let c = mesh
                .vertices()
                .get(triangle[2] as usize)
                .ok_or_else(|| "WMO triangle references missing vertex".to_string())?;
            let material_id = bounded_resource_id(a.material_id, fallback_material_id);
            let group_index = if let Some(index) = group_lookup.get(&material_id) {
                *index
            } else {
                let index = groups.len();
                groups.push(GpuWmoMeshGroup {
                    material_id,
                    vertices: Vec::new(),
                    indices: Vec::new(),
                });
                group_lookup.insert(material_id, index);
                index
            };
            let group = groups
                .get_mut(group_index)
                .ok_or_else(|| "WMO triangle references missing material group".to_string())?;
            let base_index = u32::try_from(group.vertices.len())
                .map_err(|_| "WMO material group has too many vertices".to_string())?;

            group.vertices.extend([a, b, c].map(gpu_wmo_vertex));
            group
                .indices
                .extend_from_slice(&[base_index, base_index + 1, base_index + 2]);
        }

        groups.retain(|group| !group.indices.is_empty());
        Ok(Self { groups })
    }
}

impl GpuM2Mesh {
    fn from_m2_mesh(mesh: M2Mesh, fallback_material_id: usize) -> Result<Self, String> {
        let mut group_lookup = BTreeMap::new();
        let mut groups = Vec::new();

        for triangle in mesh.indices().chunks_exact(3) {
            let a = mesh
                .vertices()
                .get(triangle[0] as usize)
                .ok_or_else(|| "M2 triangle references missing vertex".to_string())?;
            let b = mesh
                .vertices()
                .get(triangle[1] as usize)
                .ok_or_else(|| "M2 triangle references missing vertex".to_string())?;
            let c = mesh
                .vertices()
                .get(triangle[2] as usize)
                .ok_or_else(|| "M2 triangle references missing vertex".to_string())?;
            let material_id = bounded_resource_id(a.material_id, fallback_material_id);
            let group_index = if let Some(index) = group_lookup.get(&material_id) {
                *index
            } else {
                let index = groups.len();
                groups.push(GpuM2MeshGroup {
                    material_id,
                    vertices: Vec::new(),
                    indices: Vec::new(),
                });
                group_lookup.insert(material_id, index);
                index
            };
            let group = groups
                .get_mut(group_index)
                .ok_or_else(|| "M2 triangle references missing material group".to_string())?;
            let base_index = u32::try_from(group.vertices.len())
                .map_err(|_| "M2 material group has too many vertices".to_string())?;

            group.vertices.extend([a, b, c].map(gpu_m2_vertex));
            group
                .indices
                .extend_from_slice(&[base_index, base_index + 1, base_index + 2]);
        }

        groups.retain(|group| !group.indices.is_empty());
        Ok(Self { groups })
    }
}

impl GpuMarkerMesh {
    fn from_marker_mesh(mesh: &PlacementMarkerMesh) -> Result<Self, String> {
        let _ = u32::try_from(mesh.indices().len())
            .map_err(|_| "placement marker index buffer exceeds u32 draw range".to_string())?;
        Ok(Self {
            vertices: mesh.vertices().iter().map(gpu_marker_vertex).collect(),
            indices: mesh.indices().to_vec(),
        })
    }
}

fn vertex_layer_set(
    vertex: &TerrainVertex,
    fallback_material_id: usize,
    fallback_alpha_id: usize,
) -> ([usize; 4], [usize; 3]) {
    (
        vertex
            .material_ids
            .map(|id| bounded_resource_id(id, fallback_material_id)),
        vertex
            .alpha_map_ids
            .map(|id| bounded_resource_id(id, fallback_alpha_id)),
    )
}

fn bounded_resource_id(id: Option<u32>, fallback_id: usize) -> usize {
    id.and_then(|id| usize::try_from(id).ok())
        .filter(|id| *id < fallback_id)
        .unwrap_or(fallback_id)
}

fn gpu_vertex(vertex: &TerrainVertex) -> GpuVertex {
    GpuVertex {
        position: vertex.position,
        normal: normalize(vertex.normal),
        tex_coord: vertex.tex_coord,
        alpha_coord: [
            vertex.tex_coord[0] / TERRAIN_DETAIL_UV_SCALE,
            vertex.tex_coord[1] / TERRAIN_DETAIL_UV_SCALE,
        ],
    }
}

fn gpu_marker_vertex(vertex: &PlacementMarkerVertex) -> GpuMarkerVertex {
    GpuMarkerVertex {
        position: vertex.position,
        color: vertex.color,
    }
}

fn gpu_wmo_vertex(vertex: &WmoVertex) -> GpuWmoVertex {
    GpuWmoVertex {
        position: vertex.position,
        normal: normalize(vertex.normal),
        tex_coord: vertex.tex_coord,
    }
}

fn gpu_water_vertex(vertex: &WaterVertex) -> GpuWaterVertex {
    GpuWaterVertex {
        position: vertex.position,
        tex_coord: vertex.tex_coord,
        depth: vertex.depth,
        liquid_id: f32::from(vertex.liquid_id),
    }
}

fn gpu_m2_vertex(vertex: &M2RenderVertex) -> GpuWmoVertex {
    GpuWmoVertex {
        position: vertex.position,
        normal: normalize(vertex.normal),
        tex_coord: vertex.tex_coord,
    }
}

struct GpuState<'window> {
    surface: wgpu::Surface<'window>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    depth: DepthTarget,
    pipeline: wgpu::RenderPipeline,
    water_pipeline: wgpu::RenderPipeline,
    wmo_pipeline: wgpu::RenderPipeline,
    marker_pipeline: wgpu::RenderPipeline,
    camera_buffer: wgpu::Buffer,
    camera_bind_group: wgpu::BindGroup,
    water_buffer: wgpu::Buffer,
    water_bind_group: wgpu::BindGroup,
    started_at: Instant,
    _material_textures: Vec<GpuTextureResource>,
    _alpha_textures: Vec<GpuTextureResource>,
    _m2_textures: Vec<GpuTextureResource>,
    _wmo_textures: Vec<GpuTextureResource>,
    draw_groups: Vec<GpuDrawGroup>,
    m2_draw_groups: Vec<GpuM2DrawGroup>,
    wmo_draw_groups: Vec<GpuWmoDrawGroup>,
    water_draw: Option<GpuWaterDraw>,
    marker_draw: Option<GpuMarkerDraw>,
}

struct GpuTextureResource {
    _texture: wgpu::Texture,
    view: wgpu::TextureView,
}

struct LayerBindResources<'a> {
    layout: &'a wgpu::BindGroupLayout,
    material_sampler: &'a wgpu::Sampler,
    alpha_sampler: &'a wgpu::Sampler,
    material_textures: &'a [GpuTextureResource],
    alpha_textures: &'a [GpuTextureResource],
}

struct WmoBindResources<'a> {
    layout: &'a wgpu::BindGroupLayout,
    sampler: &'a wgpu::Sampler,
    textures: &'a [GpuTextureResource],
}

struct GpuDrawGroup {
    bind_group: wgpu::BindGroup,
    vertex_buffer: wgpu::Buffer,
    index_buffer: wgpu::Buffer,
    index_count: u32,
}

struct GpuWmoDrawGroup {
    bind_group: wgpu::BindGroup,
    vertex_buffer: wgpu::Buffer,
    index_buffer: wgpu::Buffer,
    index_count: u32,
}

struct GpuM2DrawGroup {
    bind_group: wgpu::BindGroup,
    vertex_buffer: wgpu::Buffer,
    index_buffer: wgpu::Buffer,
    index_count: u32,
}

struct GpuWaterDraw {
    vertex_buffer: wgpu::Buffer,
    index_buffer: wgpu::Buffer,
    index_count: u32,
}

struct GpuMarkerDraw {
    vertex_buffer: wgpu::Buffer,
    index_buffer: wgpu::Buffer,
    index_count: u32,
}

impl GpuTextureResource {
    fn view(&self) -> &wgpu::TextureView {
        &self.view
    }
}

impl<'window> GpuState<'window> {
    #[allow(clippy::too_many_arguments)]
    async fn new(
        window: &'window Window,
        mesh: &GpuMesh,
        water_mesh: &GpuWaterMesh,
        m2_mesh: &GpuM2Mesh,
        wmo_mesh: &GpuWmoMesh,
        marker_mesh: &GpuMarkerMesh,
        material_images: &[MaterialImage],
        alpha_images: &[AlphaImage],
        m2_material_images: &[MaterialImage],
        wmo_material_images: &[MaterialImage],
        camera_uniform: CameraUniform,
    ) -> Result<Self, String> {
        let size = window.inner_size();
        let instance = wgpu::Instance::default();
        let surface = instance
            .create_surface(window)
            .map_err(|err| format!("failed to create GPU surface: {err}"))?;
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance,
                compatible_surface: Some(&surface),
                force_fallback_adapter: false,
            })
            .await
            .ok_or_else(|| "failed to find a compatible GPU adapter".to_string())?;
        let (device, queue) = adapter
            .request_device(
                &wgpu::DeviceDescriptor {
                    label: Some("noggit-ui-device"),
                    required_features: wgpu::Features::empty(),
                    required_limits: wgpu::Limits::default(),
                },
                None,
            )
            .await
            .map_err(|err| format!("failed to create GPU device: {err}"))?;
        let caps = surface.get_capabilities(&adapter);
        let format = caps
            .formats
            .iter()
            .copied()
            .find(wgpu::TextureFormat::is_srgb)
            .or_else(|| caps.formats.first().copied())
            .ok_or_else(|| "GPU surface has no supported formats".to_string())?;
        let present_mode = caps
            .present_modes
            .iter()
            .copied()
            .find(|mode| *mode == wgpu::PresentMode::Mailbox)
            .or_else(|| {
                caps.present_modes
                    .iter()
                    .copied()
                    .find(|mode| *mode == wgpu::PresentMode::Fifo)
            })
            .or_else(|| caps.present_modes.first().copied())
            .ok_or_else(|| "GPU surface has no present modes".to_string())?;
        let alpha_mode = caps
            .alpha_modes
            .first()
            .copied()
            .ok_or_else(|| "GPU surface has no alpha modes".to_string())?;
        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format,
            width: size.width.max(1),
            height: size.height.max(1),
            present_mode,
            alpha_mode,
            view_formats: Vec::new(),
            desired_maximum_frame_latency: 2,
        };
        surface.configure(&device, &config);

        let camera_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("camera-buffer"),
            contents: bytemuck::bytes_of(&camera_uniform),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let camera_bind_group_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("camera-bind-group-layout"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                }],
            });
        let camera_bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("camera-bind-group"),
            layout: &camera_bind_group_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: camera_buffer.as_entire_binding(),
            }],
        });
        let water_uniform = WaterUniform { params: [0.0; 4] };
        let water_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("water-uniform-buffer"),
            contents: bytemuck::bytes_of(&water_uniform),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let water_bind_group_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("water-bind-group-layout"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT | wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                }],
            });
        let water_bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("water-bind-group"),
            layout: &water_bind_group_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: water_buffer.as_entire_binding(),
            }],
        });
        let layer_bind_group_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("terrain-layer-bind-group-layout"),
                entries: &[
                    texture_layout_entry(0),
                    texture_layout_entry(1),
                    texture_layout_entry(2),
                    texture_layout_entry(3),
                    texture_layout_entry(4),
                    texture_layout_entry(5),
                    texture_layout_entry(6),
                    sampler_layout_entry(7),
                    sampler_layout_entry(8),
                ],
            });
        let wmo_bind_group_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wmo-bind-group-layout"),
                entries: &[texture_layout_entry(0), sampler_layout_entry(1)],
            });
        let material_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("terrain-material-sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            address_mode_w: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::FilterMode::Linear,
            ..wgpu::SamplerDescriptor::default()
        });
        let alpha_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("terrain-alpha-sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::FilterMode::Nearest,
            ..wgpu::SamplerDescriptor::default()
        });
        let wmo_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wmo-material-sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            address_mode_w: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::FilterMode::Linear,
            ..wgpu::SamplerDescriptor::default()
        });
        let material_textures = material_images
            .iter()
            .enumerate()
            .map(|(index, material)| create_material_texture(&device, &queue, index, material))
            .collect::<Result<Vec<_>, _>>()?;
        let wmo_textures = wmo_material_images
            .iter()
            .enumerate()
            .map(|(index, material)| create_material_texture(&device, &queue, index, material))
            .collect::<Result<Vec<_>, _>>()?;
        let m2_textures = m2_material_images
            .iter()
            .enumerate()
            .map(|(index, material)| create_material_texture(&device, &queue, index, material))
            .collect::<Result<Vec<_>, _>>()?;
        let alpha_textures = alpha_images
            .iter()
            .enumerate()
            .map(|(index, alpha)| create_alpha_texture(&device, &queue, index, alpha))
            .collect::<Vec<_>>();

        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("terrain-shader"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(TERRAIN_SHADER)),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("terrain-pipeline-layout"),
            bind_group_layouts: &[&camera_bind_group_layout, &layer_bind_group_layout],
            push_constant_ranges: &[],
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("terrain-pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: "vs_main",
                buffers: &[GpuVertex::layout()],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: "fs_main",
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: Some(wgpu::BlendState::REPLACE),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                strip_index_format: None,
                front_face: wgpu::FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: wgpu::PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: true,
                depth_compare: wgpu::CompareFunction::Less,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        let water_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("water-shader"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(WATER_SHADER)),
        });
        let water_pipeline_layout =
            device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("water-pipeline-layout"),
                bind_group_layouts: &[&camera_bind_group_layout, &water_bind_group_layout],
                push_constant_ranges: &[],
            });
        let water_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("water-pipeline"),
            layout: Some(&water_pipeline_layout),
            vertex: wgpu::VertexState {
                module: &water_shader,
                entry_point: "vs_water",
                buffers: &[GpuWaterVertex::layout()],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &water_shader,
                entry_point: "fs_water",
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                strip_index_format: None,
                front_face: wgpu::FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: wgpu::PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: false,
                depth_compare: wgpu::CompareFunction::LessEqual,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        let wmo_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wmo-shader"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(WMO_SHADER)),
        });
        let wmo_pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wmo-pipeline-layout"),
            bind_group_layouts: &[&camera_bind_group_layout, &wmo_bind_group_layout],
            push_constant_ranges: &[],
        });
        let wmo_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wmo-pipeline"),
            layout: Some(&wmo_pipeline_layout),
            vertex: wgpu::VertexState {
                module: &wmo_shader,
                entry_point: "vs_wmo",
                buffers: &[GpuWmoVertex::layout()],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &wmo_shader,
                entry_point: "fs_wmo",
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                strip_index_format: None,
                front_face: wgpu::FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: wgpu::PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: true,
                depth_compare: wgpu::CompareFunction::Less,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        let marker_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("placement-marker-shader"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(MARKER_SHADER)),
        });
        let marker_pipeline_layout =
            device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("placement-marker-pipeline-layout"),
                bind_group_layouts: &[&camera_bind_group_layout],
                push_constant_ranges: &[],
            });
        let marker_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("placement-marker-pipeline"),
            layout: Some(&marker_pipeline_layout),
            vertex: wgpu::VertexState {
                module: &marker_shader,
                entry_point: "vs_marker",
                buffers: &[GpuMarkerVertex::layout()],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &marker_shader,
                entry_point: "fs_marker",
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: wgpu::PipelineCompilationOptions::default(),
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::LineList,
                strip_index_format: None,
                front_face: wgpu::FrontFace::Ccw,
                cull_mode: None,
                polygon_mode: wgpu::PolygonMode::Fill,
                unclipped_depth: false,
                conservative: false,
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: DEPTH_FORMAT,
                depth_write_enabled: false,
                depth_compare: wgpu::CompareFunction::Always,
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState::default(),
            multiview: None,
        });
        let draw_groups = mesh
            .groups
            .iter()
            .enumerate()
            .map(|(index, group)| {
                let resources = LayerBindResources {
                    layout: &layer_bind_group_layout,
                    material_sampler: &material_sampler,
                    alpha_sampler: &alpha_sampler,
                    material_textures: &material_textures,
                    alpha_textures: &alpha_textures,
                };
                create_draw_group(&device, index, group, &resources)
            })
            .collect::<Result<Vec<_>, _>>()?;
        let wmo_resources = WmoBindResources {
            layout: &wmo_bind_group_layout,
            sampler: &wmo_sampler,
            textures: &wmo_textures,
        };
        let wmo_draw_groups = wmo_mesh
            .groups
            .iter()
            .enumerate()
            .map(|(index, group)| create_wmo_draw_group(&device, index, group, &wmo_resources))
            .collect::<Result<Vec<_>, _>>()?;
        let m2_resources = WmoBindResources {
            layout: &wmo_bind_group_layout,
            sampler: &wmo_sampler,
            textures: &m2_textures,
        };
        let m2_draw_groups = m2_mesh
            .groups
            .iter()
            .enumerate()
            .map(|(index, group)| create_m2_draw_group(&device, index, group, &m2_resources))
            .collect::<Result<Vec<_>, _>>()?;
        let water_draw = create_water_draw(&device, water_mesh)?;
        let marker_draw = create_marker_draw(&device, marker_mesh)?;
        let depth = DepthTarget::new(&device, &config);

        Ok(Self {
            surface,
            device,
            queue,
            config,
            depth,
            pipeline,
            water_pipeline,
            wmo_pipeline,
            marker_pipeline,
            camera_buffer,
            camera_bind_group,
            water_buffer,
            water_bind_group,
            started_at: Instant::now(),
            _material_textures: material_textures,
            _alpha_textures: alpha_textures,
            _m2_textures: m2_textures,
            _wmo_textures: wmo_textures,
            draw_groups,
            m2_draw_groups,
            wmo_draw_groups,
            water_draw,
            marker_draw,
        })
    }

    fn resize(&mut self, size: PhysicalSize<u32>) {
        if size.width == 0 || size.height == 0 {
            return;
        }

        self.config.width = size.width;
        self.config.height = size.height;
        self.surface.configure(&self.device, &self.config);
        self.depth = DepthTarget::new(&self.device, &self.config);
    }

    fn update_camera(&self, uniform: CameraUniform) {
        self.queue
            .write_buffer(&self.camera_buffer, 0, bytemuck::bytes_of(&uniform));
    }

    fn update_water(&self) {
        let uniform = WaterUniform {
            params: [self.started_at.elapsed().as_secs_f32(), 0.0, 0.0, 0.0],
        };
        self.queue
            .write_buffer(&self.water_buffer, 0, bytemuck::bytes_of(&uniform));
    }

    fn render(&mut self, view_options: &ViewOptions) -> Result<(), wgpu::SurfaceError> {
        self.update_water();
        let frame = self.surface.get_current_texture()?;
        let view = frame
            .texture
            .create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("terrain-render-encoder"),
            });

        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("terrain-render-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(BACKGROUND),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                    view: &self.depth.view,
                    depth_ops: Some(wgpu::Operations {
                        load: wgpu::LoadOp::Clear(1.0),
                        store: wgpu::StoreOp::Store,
                    }),
                    stencil_ops: None,
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, &self.camera_bind_group, &[]);
            for group in &self.draw_groups {
                pass.set_bind_group(1, &group.bind_group, &[]);
                pass.set_vertex_buffer(0, group.vertex_buffer.slice(..));
                pass.set_index_buffer(group.index_buffer.slice(..), wgpu::IndexFormat::Uint32);
                pass.draw_indexed(0..group.index_count, 0, 0..1);
            }
            if view_options.show_wmo_meshes {
                pass.set_pipeline(&self.wmo_pipeline);
                pass.set_bind_group(0, &self.camera_bind_group, &[]);
                for group in &self.wmo_draw_groups {
                    pass.set_bind_group(1, &group.bind_group, &[]);
                    pass.set_vertex_buffer(0, group.vertex_buffer.slice(..));
                    pass.set_index_buffer(group.index_buffer.slice(..), wgpu::IndexFormat::Uint32);
                    pass.draw_indexed(0..group.index_count, 0, 0..1);
                }
            }
            if view_options.show_m2_meshes {
                pass.set_pipeline(&self.wmo_pipeline);
                pass.set_bind_group(0, &self.camera_bind_group, &[]);
                for group in &self.m2_draw_groups {
                    pass.set_bind_group(1, &group.bind_group, &[]);
                    pass.set_vertex_buffer(0, group.vertex_buffer.slice(..));
                    pass.set_index_buffer(group.index_buffer.slice(..), wgpu::IndexFormat::Uint32);
                    pass.draw_indexed(0..group.index_count, 0, 0..1);
                }
            }
            if view_options.show_water
                && let Some(water_draw) = &self.water_draw
            {
                pass.set_pipeline(&self.water_pipeline);
                pass.set_bind_group(0, &self.camera_bind_group, &[]);
                pass.set_bind_group(1, &self.water_bind_group, &[]);
                pass.set_vertex_buffer(0, water_draw.vertex_buffer.slice(..));
                pass.set_index_buffer(water_draw.index_buffer.slice(..), wgpu::IndexFormat::Uint32);
                pass.draw_indexed(0..water_draw.index_count, 0, 0..1);
            }
            if view_options.show_object_markers
                && let Some(marker_draw) = &self.marker_draw
            {
                pass.set_pipeline(&self.marker_pipeline);
                pass.set_bind_group(0, &self.camera_bind_group, &[]);
                pass.set_vertex_buffer(0, marker_draw.vertex_buffer.slice(..));
                pass.set_index_buffer(
                    marker_draw.index_buffer.slice(..),
                    wgpu::IndexFormat::Uint32,
                );
                pass.draw_indexed(0..marker_draw.index_count, 0, 0..1);
            }
        }

        self.queue.submit([encoder.finish()]);
        frame.present();
        Ok(())
    }
}

fn texture_layout_entry(binding: u32) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::FRAGMENT,
        ty: wgpu::BindingType::Texture {
            sample_type: wgpu::TextureSampleType::Float { filterable: true },
            view_dimension: wgpu::TextureViewDimension::D2,
            multisampled: false,
        },
        count: None,
    }
}

fn sampler_layout_entry(binding: u32) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::FRAGMENT,
        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
        count: None,
    }
}

fn create_material_texture(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    index: usize,
    material: &MaterialImage,
) -> Result<GpuTextureResource, String> {
    let first = material
        .mipmaps
        .first()
        .ok_or_else(|| "material image has no mipmaps".to_string())?;
    let mip_level_count = u32::try_from(material.mipmaps.len())
        .map_err(|_| "material image has too many mipmaps".to_string())?;
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(&format!("terrain-material-texture-{index}")),
        size: wgpu::Extent3d {
            width: first.width.max(1),
            height: first.height.max(1),
            depth_or_array_layers: 1,
        },
        mip_level_count,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8UnormSrgb,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });

    for (level, image) in material.mipmaps.iter().enumerate() {
        let mip_level =
            u32::try_from(level).map_err(|_| "material mip level exceeds u32".to_string())?;
        queue.write_texture(
            wgpu::ImageCopyTexture {
                texture: &texture,
                mip_level,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            &image.pixels,
            wgpu::ImageDataLayout {
                offset: 0,
                bytes_per_row: Some(image.width.max(1) * 4),
                rows_per_image: Some(image.height.max(1)),
            },
            wgpu::Extent3d {
                width: image.width.max(1),
                height: image.height.max(1),
                depth_or_array_layers: 1,
            },
        );
    }

    let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
    Ok(GpuTextureResource {
        _texture: texture,
        view,
    })
}

fn create_alpha_texture(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    index: usize,
    alpha: &AlphaImage,
) -> GpuTextureResource {
    let size = wgpu::Extent3d {
        width: TERRAIN_ALPHA_MAP_SIDE,
        height: TERRAIN_ALPHA_MAP_SIDE,
        depth_or_array_layers: 1,
    };
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some(&format!("terrain-alpha-texture-{index}")),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::R8Unorm,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
        view_formats: &[],
    });
    queue.write_texture(
        wgpu::ImageCopyTexture {
            texture: &texture,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        &alpha.values,
        wgpu::ImageDataLayout {
            offset: 0,
            bytes_per_row: Some(TERRAIN_ALPHA_MAP_SIDE),
            rows_per_image: Some(TERRAIN_ALPHA_MAP_SIDE),
        },
        size,
    );
    let view = texture.create_view(&wgpu::TextureViewDescriptor::default());

    GpuTextureResource {
        _texture: texture,
        view,
    }
}

fn create_layer_bind_group(
    device: &wgpu::Device,
    index: usize,
    group: &GpuMeshGroup,
    resources: &LayerBindResources<'_>,
) -> Result<wgpu::BindGroup, String> {
    let material_0 = resources
        .material_textures
        .get(group.material_ids[0])
        .ok_or_else(|| "terrain layer references missing material 0".to_string())?;
    let material_1 = resources
        .material_textures
        .get(group.material_ids[1])
        .ok_or_else(|| "terrain layer references missing material 1".to_string())?;
    let material_2 = resources
        .material_textures
        .get(group.material_ids[2])
        .ok_or_else(|| "terrain layer references missing material 2".to_string())?;
    let material_3 = resources
        .material_textures
        .get(group.material_ids[3])
        .ok_or_else(|| "terrain layer references missing material 3".to_string())?;
    let alpha_1 = resources
        .alpha_textures
        .get(group.alpha_map_ids[0])
        .ok_or_else(|| "terrain layer references missing alpha map 1".to_string())?;
    let alpha_2 = resources
        .alpha_textures
        .get(group.alpha_map_ids[1])
        .ok_or_else(|| "terrain layer references missing alpha map 2".to_string())?;
    let alpha_3 = resources
        .alpha_textures
        .get(group.alpha_map_ids[2])
        .ok_or_else(|| "terrain layer references missing alpha map 3".to_string())?;

    Ok(device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some(&format!("terrain-layer-bind-group-{index}")),
        layout: resources.layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(material_0.view()),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::TextureView(material_1.view()),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::TextureView(material_2.view()),
            },
            wgpu::BindGroupEntry {
                binding: 3,
                resource: wgpu::BindingResource::TextureView(material_3.view()),
            },
            wgpu::BindGroupEntry {
                binding: 4,
                resource: wgpu::BindingResource::TextureView(alpha_1.view()),
            },
            wgpu::BindGroupEntry {
                binding: 5,
                resource: wgpu::BindingResource::TextureView(alpha_2.view()),
            },
            wgpu::BindGroupEntry {
                binding: 6,
                resource: wgpu::BindingResource::TextureView(alpha_3.view()),
            },
            wgpu::BindGroupEntry {
                binding: 7,
                resource: wgpu::BindingResource::Sampler(resources.material_sampler),
            },
            wgpu::BindGroupEntry {
                binding: 8,
                resource: wgpu::BindingResource::Sampler(resources.alpha_sampler),
            },
        ],
    }))
}

fn create_draw_group(
    device: &wgpu::Device,
    index: usize,
    group: &GpuMeshGroup,
    resources: &LayerBindResources<'_>,
) -> Result<GpuDrawGroup, String> {
    let bind_group = create_layer_bind_group(device, index, group, resources)?;
    let vertex_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some(&format!("terrain-vertex-buffer-{index}")),
        contents: bytemuck::cast_slice(&group.vertices),
        usage: wgpu::BufferUsages::VERTEX,
    });
    let index_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some(&format!("terrain-index-buffer-{index}")),
        contents: bytemuck::cast_slice(&group.indices),
        usage: wgpu::BufferUsages::INDEX,
    });
    let index_count = u32::try_from(group.indices.len())
        .map_err(|_| "terrain index buffer exceeds u32 draw range".to_string())?;

    Ok(GpuDrawGroup {
        bind_group,
        vertex_buffer,
        index_buffer,
        index_count,
    })
}

fn create_wmo_bind_group(
    device: &wgpu::Device,
    index: usize,
    group: &GpuWmoMeshGroup,
    resources: &WmoBindResources<'_>,
) -> Result<wgpu::BindGroup, String> {
    let texture = resources
        .textures
        .get(group.material_id)
        .ok_or_else(|| "WMO group references missing material texture".to_string())?;

    Ok(device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some(&format!("wmo-bind-group-{index}")),
        layout: resources.layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(texture.view()),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::Sampler(resources.sampler),
            },
        ],
    }))
}

fn create_wmo_draw_group(
    device: &wgpu::Device,
    index: usize,
    group: &GpuWmoMeshGroup,
    resources: &WmoBindResources<'_>,
) -> Result<GpuWmoDrawGroup, String> {
    let bind_group = create_wmo_bind_group(device, index, group, resources)?;
    let vertex_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some(&format!("wmo-vertex-buffer-{index}")),
        contents: bytemuck::cast_slice(&group.vertices),
        usage: wgpu::BufferUsages::VERTEX,
    });
    let index_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some(&format!("wmo-index-buffer-{index}")),
        contents: bytemuck::cast_slice(&group.indices),
        usage: wgpu::BufferUsages::INDEX,
    });
    let index_count = u32::try_from(group.indices.len())
        .map_err(|_| "WMO index buffer exceeds u32 draw range".to_string())?;

    Ok(GpuWmoDrawGroup {
        bind_group,
        vertex_buffer,
        index_buffer,
        index_count,
    })
}

fn create_m2_bind_group(
    device: &wgpu::Device,
    index: usize,
    group: &GpuM2MeshGroup,
    resources: &WmoBindResources<'_>,
) -> Result<wgpu::BindGroup, String> {
    let texture = resources
        .textures
        .get(group.material_id)
        .ok_or_else(|| "M2 group references missing material texture".to_string())?;

    Ok(device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some(&format!("m2-bind-group-{index}")),
        layout: resources.layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(texture.view()),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::Sampler(resources.sampler),
            },
        ],
    }))
}

fn create_m2_draw_group(
    device: &wgpu::Device,
    index: usize,
    group: &GpuM2MeshGroup,
    resources: &WmoBindResources<'_>,
) -> Result<GpuM2DrawGroup, String> {
    let bind_group = create_m2_bind_group(device, index, group, resources)?;
    let vertex_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some(&format!("m2-vertex-buffer-{index}")),
        contents: bytemuck::cast_slice(&group.vertices),
        usage: wgpu::BufferUsages::VERTEX,
    });
    let index_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some(&format!("m2-index-buffer-{index}")),
        contents: bytemuck::cast_slice(&group.indices),
        usage: wgpu::BufferUsages::INDEX,
    });
    let index_count = u32::try_from(group.indices.len())
        .map_err(|_| "M2 index buffer exceeds u32 draw range".to_string())?;

    Ok(GpuM2DrawGroup {
        bind_group,
        vertex_buffer,
        index_buffer,
        index_count,
    })
}

fn create_water_draw(
    device: &wgpu::Device,
    mesh: &GpuWaterMesh,
) -> Result<Option<GpuWaterDraw>, String> {
    if mesh.indices.is_empty() {
        return Ok(None);
    }

    let index_count = u32::try_from(mesh.indices.len())
        .map_err(|_| "water index buffer exceeds u32 draw range".to_string())?;
    let vertex_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("water-vertex-buffer"),
        contents: bytemuck::cast_slice(&mesh.vertices),
        usage: wgpu::BufferUsages::VERTEX,
    });
    let index_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("water-index-buffer"),
        contents: bytemuck::cast_slice(&mesh.indices),
        usage: wgpu::BufferUsages::INDEX,
    });

    Ok(Some(GpuWaterDraw {
        vertex_buffer,
        index_buffer,
        index_count,
    }))
}

fn create_marker_draw(
    device: &wgpu::Device,
    mesh: &GpuMarkerMesh,
) -> Result<Option<GpuMarkerDraw>, String> {
    if mesh.indices.is_empty() {
        return Ok(None);
    }

    let index_count = u32::try_from(mesh.indices.len())
        .map_err(|_| "placement marker index buffer exceeds u32 draw range".to_string())?;
    let vertex_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("placement-marker-vertex-buffer"),
        contents: bytemuck::cast_slice(&mesh.vertices),
        usage: wgpu::BufferUsages::VERTEX,
    });
    let index_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("placement-marker-index-buffer"),
        contents: bytemuck::cast_slice(&mesh.indices),
        usage: wgpu::BufferUsages::INDEX,
    });

    Ok(Some(GpuMarkerDraw {
        vertex_buffer,
        index_buffer,
        index_count,
    }))
}

struct DepthTarget {
    view: wgpu::TextureView,
}

impl DepthTarget {
    fn new(device: &wgpu::Device, config: &wgpu::SurfaceConfiguration) -> Self {
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("depth-texture"),
            size: wgpu::Extent3d {
                width: config.width,
                height: config.height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: DEPTH_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());

        Self { view }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct CameraUniform {
    view_projection: [[f32; 4]; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct WaterUniform {
    params: [f32; 4],
}

const CAMERA_MOUSE_SENSITIVITY: f32 = 0.006;
const CAMERA_KEY_TURN_STEP: f32 = 0.035;
const CAMERA_MIN_PITCH: f32 = -1.45;
const CAMERA_MAX_PITCH: f32 = 1.45;
const CAMERA_MIN_SPEED: f32 = 0.25;
const CAMERA_MAX_SPEED: f32 = 250.0;
const CAMERA_SHIFT_SPEED_MULTIPLIER: f32 = 4.0;

struct Camera {
    position: [f32; 3],
    yaw: f32,
    pitch: f32,
    movement_speed: f32,
}

impl Camera {
    fn new(bounds: TerrainBounds) -> Self {
        let size = bounds.size();
        let planar_size = size[0].max(size[2]).max(1.0);
        let target = bounds.center();
        let position = [
            target[0],
            bounds.max[1] + size[1].max(80.0) * 0.35,
            target[2] - planar_size * 0.55,
        ];
        let forward = normalize(sub(target, position));

        Self {
            position,
            yaw: forward[0].atan2(forward[2]),
            pitch: forward[1].asin(),
            movement_speed: (planar_size * 0.01).clamp(CAMERA_MIN_SPEED, CAMERA_MAX_SPEED),
        }
    }

    fn update(&mut self, input: &mut InputState) -> bool {
        let previous_position = self.position;
        let previous_yaw = self.yaw;
        let previous_pitch = self.pitch;
        let previous_speed = self.movement_speed;

        if input.key_down(KeyCode::ArrowLeft) {
            self.yaw -= CAMERA_KEY_TURN_STEP;
        }
        if input.key_down(KeyCode::ArrowRight) {
            self.yaw += CAMERA_KEY_TURN_STEP;
        }
        if input.key_down(KeyCode::ArrowUp) {
            self.pitch += CAMERA_KEY_TURN_STEP;
        }
        if input.key_down(KeyCode::ArrowDown) {
            self.pitch -= CAMERA_KEY_TURN_STEP;
        }

        let scroll = input.take_scroll();
        if scroll.abs() > f32::EPSILON {
            self.movement_speed *= (1.0 + scroll * 0.18).clamp(0.45, 1.8);
        }

        let mouse_delta = input.take_mouse_delta();
        if input.left_mouse_down() {
            self.yaw += mouse_delta.0 * CAMERA_MOUSE_SENSITIVITY;
            self.pitch -= mouse_delta.1 * CAMERA_MOUSE_SENSITIVITY;
        }

        self.pitch = self.pitch.clamp(CAMERA_MIN_PITCH, CAMERA_MAX_PITCH);
        self.movement_speed = self
            .movement_speed
            .clamp(CAMERA_MIN_SPEED, CAMERA_MAX_SPEED);

        let basis = self.basis();
        let mut movement = [0.0, 0.0, 0.0];
        if input.key_down(KeyCode::KeyW) {
            movement = add(movement, basis.forward);
        }
        if input.key_down(KeyCode::KeyS) {
            movement = sub(movement, basis.forward);
        }
        if input.key_down(KeyCode::KeyA) {
            movement = sub(movement, basis.right);
        }
        if input.key_down(KeyCode::KeyD) {
            movement = add(movement, basis.right);
        }
        if input.key_down(KeyCode::KeyQ)
            || input.key_down(KeyCode::ControlLeft)
            || input.key_down(KeyCode::ControlRight)
        {
            movement[1] -= 1.0;
        }
        if input.key_down(KeyCode::KeyE) || input.key_down(KeyCode::Space) {
            movement[1] += 1.0;
        }

        let movement = normalize(movement);
        if movement != [0.0, 0.0, 0.0] {
            let speed = if input.key_down(KeyCode::ShiftLeft) || input.key_down(KeyCode::ShiftRight)
            {
                self.movement_speed * CAMERA_SHIFT_SPEED_MULTIPLIER
            } else {
                self.movement_speed
            };
            self.position = add(self.position, scale(movement, speed));
        }

        self.position != previous_position
            || (self.yaw - previous_yaw).abs() > f32::EPSILON
            || (self.pitch - previous_pitch).abs() > f32::EPSILON
            || (self.movement_speed - previous_speed).abs() > f32::EPSILON
    }

    fn uniform(&self, size: PhysicalSize<u32>) -> CameraUniform {
        let aspect = size.width.max(1) as f32 / size.height.max(1) as f32;
        CameraUniform {
            view_projection: mat4_mul(
                perspective(55.0_f32.to_radians(), aspect, 1.0, 6000.0),
                self.view_matrix(),
            ),
        }
    }

    fn view_matrix(&self) -> [[f32; 4]; 4] {
        let basis = self.basis();
        look_at(
            self.position,
            add(self.position, basis.forward),
            [0.0, 1.0, 0.0],
        )
    }

    fn basis(&self) -> CameraBasis {
        let (yaw_sin, yaw_cos) = self.yaw.sin_cos();
        let (pitch_sin, pitch_cos) = self.pitch.sin_cos();
        let forward = normalize([yaw_sin * pitch_cos, pitch_sin, yaw_cos * pitch_cos]);
        let right = normalize(cross([0.0, 1.0, 0.0], forward));

        CameraBasis { forward, right }
    }
}

#[derive(Clone, Copy)]
struct CameraBasis {
    forward: [f32; 3],
    right: [f32; 3],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct ViewOptions {
    show_object_markers: bool,
    show_m2_meshes: bool,
    show_wmo_meshes: bool,
    show_water: bool,
}

impl Default for ViewOptions {
    fn default() -> Self {
        Self {
            show_object_markers: true,
            show_m2_meshes: true,
            show_wmo_meshes: true,
            show_water: true,
        }
    }
}

impl ViewOptions {
    fn handle_key_press(&mut self, code: KeyCode) -> bool {
        match code {
            KeyCode::KeyO => {
                self.show_object_markers = !self.show_object_markers;
                true
            }
            KeyCode::KeyM => {
                self.show_wmo_meshes = !self.show_wmo_meshes;
                true
            }
            KeyCode::KeyN => {
                self.show_m2_meshes = !self.show_m2_meshes;
                true
            }
            KeyCode::KeyL => {
                self.show_water = !self.show_water;
                true
            }
            _ => false,
        }
    }
}

#[derive(Default)]
struct InputState {
    keys: BTreeSet<KeyCode>,
    left_mouse: bool,
    last_cursor: Option<(f32, f32)>,
    mouse_delta: (f32, f32),
    scroll: f32,
}

impl InputState {
    fn set_key(&mut self, code: KeyCode, state: ElementState) {
        match state {
            ElementState::Pressed => {
                self.keys.insert(code);
            }
            ElementState::Released => {
                self.keys.remove(&code);
            }
        }
    }

    fn set_mouse_button(&mut self, button: MouseButton, state: ElementState) {
        if button != MouseButton::Left {
            return;
        }

        self.left_mouse = state == ElementState::Pressed;
        if !self.left_mouse {
            self.last_cursor = None;
            self.mouse_delta = (0.0, 0.0);
        }
    }

    fn set_cursor_position(&mut self, x: f32, y: f32) {
        if self.left_mouse
            && let Some((last_x, last_y)) = self.last_cursor
        {
            self.mouse_delta.0 += x - last_x;
            self.mouse_delta.1 += y - last_y;
        }
        self.last_cursor = Some((x, y));
    }

    fn add_scroll(&mut self, delta: MouseScrollDelta) {
        self.scroll += match delta {
            MouseScrollDelta::LineDelta(_, y) => y,
            MouseScrollDelta::PixelDelta(position) => position.y as f32 / 80.0,
        };
    }

    fn key_down(&self, code: KeyCode) -> bool {
        self.keys.contains(&code)
    }

    fn left_mouse_down(&self) -> bool {
        self.left_mouse
    }

    fn take_mouse_delta(&mut self) -> (f32, f32) {
        let delta = self.mouse_delta;
        self.mouse_delta = (0.0, 0.0);
        delta
    }

    fn take_scroll(&mut self) -> f32 {
        let scroll = self.scroll;
        self.scroll = 0.0;
        scroll
    }
}

fn mat4_mul(a: [[f32; 4]; 4], b: [[f32; 4]; 4]) -> [[f32; 4]; 4] {
    let mut out = [[0.0; 4]; 4];
    for column in 0..4 {
        for row in 0..4 {
            out[column][row] = a[0][row] * b[column][0]
                + a[1][row] * b[column][1]
                + a[2][row] * b[column][2]
                + a[3][row] * b[column][3];
        }
    }
    out
}

fn perspective(fovy: f32, aspect: f32, near: f32, far: f32) -> [[f32; 4]; 4] {
    let f = 1.0 / (fovy * 0.5).tan();
    [
        [f / aspect, 0.0, 0.0, 0.0],
        [0.0, f, 0.0, 0.0],
        [0.0, 0.0, far / (near - far), -1.0],
        [0.0, 0.0, near * far / (near - far), 0.0],
    ]
}

fn look_at(eye: [f32; 3], target: [f32; 3], up: [f32; 3]) -> [[f32; 4]; 4] {
    let forward = normalize(sub(target, eye));
    let right = normalize(cross(forward, up));
    let up = cross(right, forward);

    [
        [right[0], up[0], -forward[0], 0.0],
        [right[1], up[1], -forward[1], 0.0],
        [right[2], up[2], -forward[2], 0.0],
        [-dot(right, eye), -dot(up, eye), dot(forward, eye), 1.0],
    ]
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

const TERRAIN_SHADER: &str = r#"
struct Camera {
    view_projection: mat4x4<f32>,
};

@group(0) @binding(0)
var<uniform> camera: Camera;

@group(1) @binding(0)
var terrain_texture_0: texture_2d<f32>;

@group(1) @binding(1)
var terrain_texture_1: texture_2d<f32>;

@group(1) @binding(2)
var terrain_texture_2: texture_2d<f32>;

@group(1) @binding(3)
var terrain_texture_3: texture_2d<f32>;

@group(1) @binding(4)
var terrain_alpha_1: texture_2d<f32>;

@group(1) @binding(5)
var terrain_alpha_2: texture_2d<f32>;

@group(1) @binding(6)
var terrain_alpha_3: texture_2d<f32>;

@group(1) @binding(7)
var terrain_sampler: sampler;

@group(1) @binding(8)
var alpha_sampler: sampler;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) tex_coord: vec2<f32>,
    @location(3) alpha_coord: vec2<f32>,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) normal: vec3<f32>,
    @location(1) tex_coord: vec2<f32>,
    @location(2) alpha_coord: vec2<f32>,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.clip_position = camera.view_projection * vec4<f32>(input.position, 1.0);
    output.normal = input.normal;
    output.tex_coord = input.tex_coord;
    output.alpha_coord = input.alpha_coord;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let light = normalize(vec3<f32>(0.35, 0.88, 0.32));
    let shade = 0.58 + max(dot(normalize(input.normal), light), 0.0) * 0.42;
    let alpha_1 = textureSample(terrain_alpha_1, alpha_sampler, input.alpha_coord).r;
    let alpha_2 = textureSample(terrain_alpha_2, alpha_sampler, input.alpha_coord).r;
    let alpha_3 = textureSample(terrain_alpha_3, alpha_sampler, input.alpha_coord).r;
    let base_alpha = 1.0 - (alpha_1 + alpha_2 + alpha_3);
    let tex_color =
        textureSample(terrain_texture_0, terrain_sampler, input.tex_coord).rgb * base_alpha
        + textureSample(terrain_texture_1, terrain_sampler, input.tex_coord).rgb * alpha_1
        + textureSample(terrain_texture_2, terrain_sampler, input.tex_coord).rgb * alpha_2
        + textureSample(terrain_texture_3, terrain_sampler, input.tex_coord).rgb * alpha_3;
    return vec4<f32>(tex_color * shade, 1.0);
}
"#;

const WATER_SHADER: &str = r#"
struct Camera {
    view_projection: mat4x4<f32>,
};

struct WaterState {
    params: vec4<f32>,
};

@group(0) @binding(0)
var<uniform> camera: Camera;

@group(1) @binding(0)
var<uniform> water: WaterState;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) tex_coord: vec2<f32>,
    @location(2) depth: f32,
    @location(3) liquid_id: f32,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) tex_coord: vec2<f32>,
    @location(1) depth: f32,
    @location(2) wave: f32,
    @location(3) liquid_id: f32,
};

@vertex
fn vs_water(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let time = water.params.x;
    let scroll = vec2<f32>(time * 0.035, -time * 0.022);
    let wave =
        sin((input.tex_coord.x + scroll.x) * 7.0) * 0.018
        + cos((input.tex_coord.y + scroll.y) * 9.0) * 0.014;
    let position = input.position + vec3<f32>(0.0, wave + 0.01, 0.0);
    output.clip_position = camera.view_projection * vec4<f32>(position, 1.0);
    output.tex_coord = input.tex_coord;
    output.depth = input.depth;
    output.wave = wave;
    output.liquid_id = input.liquid_id;
    return output;
}

@fragment
fn fs_water(input: VertexOutput) -> @location(0) vec4<f32> {
    let time = water.params.x;
    let wave_a = sin((input.tex_coord.x + time * 0.018) * 10.0);
    let wave_b = cos((input.tex_coord.y - time * 0.014) * 12.0);
    let wave = clamp((wave_a + wave_b) * 0.08 + input.wave * 2.0, -0.25, 0.25);
    let liquid_tint = clamp(input.liquid_id * 0.0008, 0.0, 0.07);
    let shallow = vec3<f32>(0.16, 0.42 + liquid_tint, 0.48 + liquid_tint);
    let deep = vec3<f32>(0.04, 0.20 + liquid_tint, 0.28 + liquid_tint);
    let factor = clamp(input.depth * 0.85 + 0.08 + wave, 0.0, 1.0);
    let color = shallow * (1.0 - factor) + deep * factor;
    let alpha = clamp(0.24 + input.depth * 0.24, 0.22, 0.50);
    return vec4<f32>(color, alpha);
}
"#;

const WMO_SHADER: &str = r#"
struct Camera {
    view_projection: mat4x4<f32>,
};

@group(0) @binding(0)
var<uniform> camera: Camera;

@group(1) @binding(0)
var wmo_texture: texture_2d<f32>;

@group(1) @binding(1)
var wmo_sampler: sampler;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) tex_coord: vec2<f32>,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) normal: vec3<f32>,
    @location(1) tex_coord: vec2<f32>,
};

@vertex
fn vs_wmo(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.clip_position = camera.view_projection * vec4<f32>(input.position, 1.0);
    output.normal = input.normal;
    output.tex_coord = input.tex_coord;
    return output;
}

@fragment
fn fs_wmo(input: VertexOutput) -> @location(0) vec4<f32> {
    let tex = textureSample(wmo_texture, wmo_sampler, input.tex_coord);
    if (tex.a < 0.05) {
        discard;
    }
    let light = normalize(vec3<f32>(0.35, 0.88, 0.32));
    let shade = 0.55 + max(dot(normalize(input.normal), light), 0.0) * 0.45;
    return vec4<f32>(tex.rgb * shade, tex.a);
}
"#;

const MARKER_SHADER: &str = r#"
struct Camera {
    view_projection: mat4x4<f32>,
};

@group(0) @binding(0)
var<uniform> camera: Camera;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) color: vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) color: vec4<f32>,
};

@vertex
fn vs_marker(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.clip_position = camera.view_projection * vec4<f32>(input.position, 1.0);
    output.color = input.color;
    return output;
}

@fragment
fn fs_marker(input: VertexOutput) -> @location(0) vec4<f32> {
    return input.color;
}
"#;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn object_marker_overlay_toggles_from_keyboard_action() {
        let mut options = ViewOptions::default();

        assert!(options.show_object_markers);
        assert!(options.show_m2_meshes);
        assert!(options.show_wmo_meshes);
        assert!(options.show_water);
        assert!(options.handle_key_press(KeyCode::KeyO));
        assert!(!options.show_object_markers);
        assert!(options.show_m2_meshes);
        assert!(options.show_wmo_meshes);
        assert!(options.show_water);
        assert!(options.handle_key_press(KeyCode::KeyO));
        assert!(options.show_object_markers);
        assert!(options.handle_key_press(KeyCode::KeyM));
        assert!(!options.show_wmo_meshes);
        assert!(options.handle_key_press(KeyCode::KeyM));
        assert!(options.show_wmo_meshes);
        assert!(options.handle_key_press(KeyCode::KeyN));
        assert!(!options.show_m2_meshes);
        assert!(options.handle_key_press(KeyCode::KeyN));
        assert!(options.show_m2_meshes);
        assert!(options.handle_key_press(KeyCode::KeyL));
        assert!(!options.show_water);
        assert!(options.handle_key_press(KeyCode::KeyL));
        assert!(options.show_water);
        assert!(!options.handle_key_press(KeyCode::KeyP));
        assert!(options.show_object_markers);
        assert!(options.show_m2_meshes);
        assert!(options.show_wmo_meshes);
        assert!(options.show_water);
    }

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
    fn builds_wmo_group_paths_from_root_path() -> Result<(), String> {
        assert_eq!(
            wmo_group_path("world/wmo/foo/bar.wmo", 7)?,
            "world/wmo/foo/bar_007.wmo"
        );
        assert!(wmo_group_path("world/wmo/foo/bar.m2", 0).is_err());
        Ok(())
    }

    #[test]
    fn builds_m2_model_and_skin_path_candidates() -> Result<(), String> {
        assert_eq!(
            m2_model_path_candidates("world/foo/bar.MDX"),
            vec!["world/foo/bar.MDX", "world/foo/bar.m2"]
        );
        assert_eq!(
            m2_skin_path_candidates("world/foo/bar.M2")?,
            vec!["world/foo/bar00.skin"]
        );
        assert_eq!(
            m2_skin_path_candidates("world/foo/bar.MDX")?,
            vec!["world/foo/bar00.skin", "world/foo/bar.00.skin"]
        );
        Ok(())
    }

    #[test]
    fn camera_moves_vertically_from_keyboard_input() {
        let bounds = TerrainBounds {
            min: [0.0, 0.0, 0.0],
            max: [100.0, 100.0, 100.0],
        };
        let mut camera = Camera::new(bounds);
        let start_y = camera.position[1];
        let mut input = InputState::default();

        input.set_key(KeyCode::KeyQ, ElementState::Pressed);
        assert!(camera.update(&mut input));
        assert!(camera.position[1] < start_y);

        input.set_key(KeyCode::KeyQ, ElementState::Released);
        input.set_key(KeyCode::KeyE, ElementState::Pressed);
        let lowered_y = camera.position[1];
        assert!(camera.update(&mut input));
        assert!(camera.position[1] > lowered_y);
    }

    #[test]
    fn camera_moves_forward_from_camera_position() {
        let bounds = TerrainBounds {
            min: [0.0, 0.0, 0.0],
            max: [100.0, 100.0, 100.0],
        };
        let mut camera = Camera::new(bounds);
        let start = camera.position;
        let forward = camera.basis().forward;
        let mut input = InputState::default();

        input.set_key(KeyCode::KeyW, ElementState::Pressed);
        assert!(camera.update(&mut input));

        assert_close(
            camera.position,
            add(start, scale(forward, camera.movement_speed)),
        );
    }

    #[test]
    fn camera_scroll_changes_movement_speed_without_moving_position() {
        let bounds = TerrainBounds {
            min: [0.0, 0.0, 0.0],
            max: [100.0, 100.0, 100.0],
        };
        let mut camera = Camera::new(bounds);
        let start = camera.position;
        let start_speed = camera.movement_speed;
        let mut input = InputState::default();

        input.add_scroll(MouseScrollDelta::LineDelta(0.0, 1.0));
        assert!(camera.update(&mut input));

        assert_eq!(camera.position, start);
        assert!(camera.movement_speed > start_speed);
    }

    #[test]
    fn builds_checkerboard_missing_material_texture() {
        let material = missing_material_image();

        assert_eq!(material.mipmaps.len(), 3);
        assert_eq!(material.mipmaps[0].width, 4);
        assert_eq!(material.mipmaps[0].height, 4);
        assert_eq!(material.mipmaps[1].width, 2);
        assert_eq!(material.mipmaps[1].height, 2);
        assert_eq!(material.mipmaps[2].width, 1);
        assert_eq!(material.mipmaps[2].height, 1);
        assert_eq!(&material.mipmaps[0].pixels[0..4], &[180, 24, 180, 255]);
        assert_eq!(&material.mipmaps[0].pixels[4..8], &[24, 24, 24, 255]);
    }

    #[test]
    fn keeps_detail_uv_and_normalizes_alpha_uv_for_gpu_vertices() {
        let vertex = TerrainVertex {
            position: [0.0, 4.0, 0.0],
            normal: [0.0, 1.0, 0.0],
            tex_coord: [4.0, 8.0],
            material_ids: [Some(0), None, None, None],
            alpha_map_ids: [None, None, None],
        };
        let gpu_vertex = gpu_vertex(&vertex);

        assert_eq!(gpu_vertex.tex_coord, [4.0, 8.0]);
        assert_eq!(gpu_vertex.alpha_coord, [0.5, 1.0]);
    }

    #[test]
    fn builds_layer_set_with_fallback_resource_ids() {
        let vertex = TerrainVertex {
            position: [0.0, 4.0, 0.0],
            normal: [0.0, 1.0, 0.0],
            tex_coord: [4.0, 8.0],
            material_ids: [Some(1), Some(99), None, Some(0)],
            alpha_map_ids: [Some(2), Some(99), None],
        };

        assert_eq!(vertex_layer_set(&vertex, 3, 4), ([1, 3, 3, 0], [2, 4, 4]));
    }

    fn assert_close(actual: [f32; 3], expected: [f32; 3]) {
        for (actual, expected) in actual.into_iter().zip(expected) {
            assert!(
                (actual - expected).abs() < 0.001,
                "expected {actual} ~= {expected}"
            );
        }
    }
}
