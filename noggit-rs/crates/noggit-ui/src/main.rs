//! Terrain preview window for the Rust rewrite.

use std::borrow::Cow;
use std::collections::BTreeSet;
use std::env;
use std::path::PathBuf;
use std::process::ExitCode;

use bytemuck::{Pod, Zeroable};
use noggit_core::WorldMap;
use noggit_formats::blp::{BlpFile, RgbaImage};
use noggit_render::{TerrainBounds, TerrainMesh, TerrainVertex, build_terrain_mesh};
use noggit_vfs::{FileSource, VfsPath, WowClient, WowClientConfig};
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
    let gpu_mesh = GpuMesh::from_terrain_mesh(mesh, &material_colors)?;
    let mut camera = Camera::new(gpu_mesh.bounds);

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
        camera.uniform(window.inner_size()),
    ))?;
    let mut input = InputState::default();

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

                            match gpu.render() {
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

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuVertex {
    position: [f32; 3],
    normal: [f32; 3],
    color: [f32; 3],
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
                    format: wgpu::VertexFormat::Float32x3,
                },
            ],
        }
    }
}

struct GpuMesh {
    vertices: Vec<GpuVertex>,
    indices: Vec<u32>,
    bounds: TerrainBounds,
}

impl GpuMesh {
    fn from_terrain_mesh(
        mesh: TerrainMesh,
        material_colors: &[Option<u32>],
    ) -> Result<Self, String> {
        let Some(bounds) = mesh.bounds() else {
            return Err("map has no renderable terrain chunks".to_string());
        };
        if mesh.indices().is_empty() {
            return Err("map has no renderable terrain triangles".to_string());
        }

        let vertices = mesh
            .vertices()
            .iter()
            .map(|vertex| GpuVertex {
                position: vertex.position,
                normal: normalize(vertex.normal),
                color: vertex_color(vertex, material_colors, bounds),
            })
            .collect();

        Ok(Self {
            vertices,
            indices: mesh.indices().to_vec(),
            bounds,
        })
    }
}

fn vertex_color(
    vertex: &TerrainVertex,
    material_colors: &[Option<u32>],
    bounds: TerrainBounds,
) -> [f32; 3] {
    let color = vertex
        .material_id
        .and_then(|id| usize::try_from(id).ok())
        .and_then(|id| material_colors.get(id).copied().flatten())
        .unwrap_or_else(|| height_color(vertex.position[1], bounds));

    rgb_to_f32(color)
}

fn rgb_to_f32(color: u32) -> [f32; 3] {
    [
        ((color >> 16) & 0xff) as f32 / 255.0,
        ((color >> 8) & 0xff) as f32 / 255.0,
        (color & 0xff) as f32 / 255.0,
    ]
}

struct GpuState<'window> {
    surface: wgpu::Surface<'window>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    depth: DepthTarget,
    pipeline: wgpu::RenderPipeline,
    camera_buffer: wgpu::Buffer,
    camera_bind_group: wgpu::BindGroup,
    vertex_buffer: wgpu::Buffer,
    index_buffer: wgpu::Buffer,
    index_count: u32,
}

impl<'window> GpuState<'window> {
    async fn new(
        window: &'window Window,
        mesh: &GpuMesh,
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

        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("terrain-shader"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(TERRAIN_SHADER)),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("terrain-pipeline-layout"),
            bind_group_layouts: &[&camera_bind_group_layout],
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
        let vertex_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("terrain-vertex-buffer"),
            contents: bytemuck::cast_slice(&mesh.vertices),
            usage: wgpu::BufferUsages::VERTEX,
        });
        let index_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("terrain-index-buffer"),
            contents: bytemuck::cast_slice(&mesh.indices),
            usage: wgpu::BufferUsages::INDEX,
        });
        let index_count = u32::try_from(mesh.indices.len())
            .map_err(|_| "terrain index buffer exceeds u32 draw range".to_string())?;
        let depth = DepthTarget::new(&device, &config);

        Ok(Self {
            surface,
            device,
            queue,
            config,
            depth,
            pipeline,
            camera_buffer,
            camera_bind_group,
            vertex_buffer,
            index_buffer,
            index_count,
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

    fn render(&mut self) -> Result<(), wgpu::SurfaceError> {
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
            pass.set_vertex_buffer(0, self.vertex_buffer.slice(..));
            pass.set_index_buffer(self.index_buffer.slice(..), wgpu::IndexFormat::Uint32);
            pass.draw_indexed(0..self.index_count, 0, 0..1);
        }

        self.queue.submit([encoder.finish()]);
        frame.present();
        Ok(())
    }
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

struct Camera {
    target: [f32; 3],
    yaw: f32,
    pitch: f32,
    distance: f32,
}

impl Camera {
    fn new(bounds: TerrainBounds) -> Self {
        let size = bounds.size();
        let planar_size = size[0].max(size[2]).max(1.0);
        let mut target = bounds.center();
        target[1] += size[1] * 0.08;

        Self {
            target,
            yaw: 0.75,
            pitch: 0.68,
            distance: planar_size * 0.95,
        }
    }

    fn update(&mut self, input: &mut InputState) -> bool {
        let previous_target = self.target;
        let previous_yaw = self.yaw;
        let previous_pitch = self.pitch;
        let previous_distance = self.distance;
        let turn_step = 0.035;
        let pan_step = self.distance * 0.012;

        if input.key_down(KeyCode::ArrowLeft) {
            self.yaw -= turn_step;
        }
        if input.key_down(KeyCode::ArrowRight) {
            self.yaw += turn_step;
        }
        if input.key_down(KeyCode::ArrowUp) {
            self.pitch += turn_step;
        }
        if input.key_down(KeyCode::ArrowDown) {
            self.pitch -= turn_step;
        }
        if input.key_down(KeyCode::KeyQ) {
            self.distance *= 1.035;
        }
        if input.key_down(KeyCode::KeyE) {
            self.distance *= 0.965;
        }

        let scroll = input.take_scroll();
        if scroll.abs() > f32::EPSILON {
            self.distance *= (1.0 - scroll * 0.12).clamp(0.35, 1.65);
        }

        let mouse_delta = input.take_mouse_delta();
        if input.left_mouse_down() {
            self.yaw += mouse_delta.0 * 0.006;
            self.pitch += mouse_delta.1 * 0.006;
        }

        let basis = self.basis();
        if input.key_down(KeyCode::KeyW) {
            self.target = add(
                self.target,
                scale(horizontal_forward(basis.forward), pan_step),
            );
        }
        if input.key_down(KeyCode::KeyS) {
            self.target = sub(
                self.target,
                scale(horizontal_forward(basis.forward), pan_step),
            );
        }
        if input.key_down(KeyCode::KeyA) {
            self.target = sub(self.target, scale(basis.right, pan_step));
        }
        if input.key_down(KeyCode::KeyD) {
            self.target = add(self.target, scale(basis.right, pan_step));
        }

        self.pitch = self.pitch.clamp(0.12, 1.42);
        self.distance = self.distance.clamp(20.0, 4000.0);

        self.target != previous_target
            || (self.yaw - previous_yaw).abs() > f32::EPSILON
            || (self.pitch - previous_pitch).abs() > f32::EPSILON
            || (self.distance - previous_distance).abs() > f32::EPSILON
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
        look_at(basis.position, self.target, [0.0, 1.0, 0.0])
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

        CameraBasis {
            position,
            forward,
            right,
        }
    }
}

#[derive(Clone, Copy)]
struct CameraBasis {
    position: [f32; 3],
    forward: [f32; 3],
    right: [f32; 3],
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

const TERRAIN_SHADER: &str = r#"
struct Camera {
    view_projection: mat4x4<f32>,
};

@group(0) @binding(0)
var<uniform> camera: Camera;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) color: vec3<f32>,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) normal: vec3<f32>,
    @location(1) color: vec3<f32>,
};

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.clip_position = camera.view_projection * vec4<f32>(input.position, 1.0);
    output.normal = input.normal;
    output.color = input.color;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let light = normalize(vec3<f32>(0.35, 0.88, 0.32));
    let shade = 0.58 + max(dot(normalize(input.normal), light), 0.0) * 0.42;
    return vec4<f32>(input.color * shade, 1.0);
}
"#;

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

    #[test]
    fn uses_material_color_for_gpu_vertices() {
        let vertex = TerrainVertex {
            position: [0.0, 4.0, 0.0],
            normal: [0.0, 1.0, 0.0],
            material_id: Some(0),
        };
        let color = vertex_color(
            &vertex,
            &[Some(0x804020)],
            TerrainBounds {
                min: [0.0, 0.0, 0.0],
                max: [0.0, 10.0, 0.0],
            },
        );

        assert_eq!(color, [128.0 / 255.0, 64.0 / 255.0, 32.0 / 255.0]);
    }
}
