//! CLI entry point for validating rewrite milestones.

use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::env;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use noggit_core::WorldMap;
use noggit_formats::adt::{AdtFile, filename_by_name_id};
use noggit_formats::dbc::DbcFile;
use noggit_vfs::{ArchiveLoadState, FileSource, VfsPath, WowClient, WowClientConfig};

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
    let mut args = env::args().skip(1);
    let command = args.next().ok_or_else(usage)?;

    match command.as_str() {
        "inspect-dbc" => {
            let path = args.next().map(PathBuf::from).ok_or_else(usage)?;
            let bytes = fs::read(&path)
                .map_err(|err| format!("failed to read {}: {err}", path.display()))?;
            print!("{}", inspect_dbc_bytes(&bytes)?);
        }
        "inspect-adt" => {
            let path = args.next().map(PathBuf::from).ok_or_else(usage)?;
            let bytes = fs::read(&path)
                .map_err(|err| format!("failed to read {}: {err}", path.display()))?;
            print!("{}", inspect_adt_bytes(&bytes)?);
        }
        "inspect-map" => {
            let path = args.next().map(PathBuf::from).ok_or_else(usage)?;
            print!("{}", inspect_map_path(&path)?);
        }
        "inspect-client" => {
            let client_path = args.next().map(PathBuf::from).ok_or_else(usage)?;
            let extra_archives = args.map(PathBuf::from).collect();
            print!("{}", inspect_client_path(&client_path, extra_archives)?);
        }
        "check-map-assets" => {
            let map_path = args.next().map(PathBuf::from).ok_or_else(usage)?;
            let client_path = args.next().map(PathBuf::from).ok_or_else(usage)?;
            let extra_archives = args.map(PathBuf::from).collect();
            print!(
                "{}",
                check_map_assets(&map_path, &client_path, extra_archives)?
            );
        }
        _ => return Err(usage()),
    }

    Ok(())
}

fn usage() -> String {
    "usage: noggit-cli <inspect-dbc|inspect-adt|inspect-map> <path>\n       noggit-cli inspect-client <client-path> [extra-mpq...]\n       noggit-cli check-map-assets <map-path> <client-path> [extra-mpq...]".to_string()
}

fn inspect_dbc_bytes(bytes: &[u8]) -> Result<String, String> {
    let dbc = DbcFile::parse(bytes).map_err(|err| err.to_string())?;
    let header = dbc.header();
    Ok(format!(
        "DBC records={} fields={} record_size={} string_block_size={}\n",
        header.record_count, header.field_count, header.record_size, header.string_block_size
    ))
}

fn inspect_adt_bytes(bytes: &[u8]) -> Result<String, String> {
    let adt = AdtFile::parse(bytes).map_err(|err| err.to_string())?;
    let mut output = String::new();

    writeln!(output, "ADT chunks={}", adt.chunks().len()).map_err(|err| err.to_string())?;
    if let Some(version) = adt.version().map_err(|err| err.to_string())? {
        writeln!(output, "MVER version={version}").map_err(|err| err.to_string())?;
    }

    let textures = adt.texture_filenames().map_err(|err| err.to_string())?;
    let models = adt.model_filenames().map_err(|err| err.to_string())?;
    let model_offsets = adt
        .model_filename_offsets()
        .map_err(|err| err.to_string())?;
    let wmos = adt.wmo_filenames().map_err(|err| err.to_string())?;
    let wmo_offsets = adt.wmo_filename_offsets().map_err(|err| err.to_string())?;
    let doodads = adt.model_placements().map_err(|err| err.to_string())?;
    let wmo_placements = adt.wmo_placements().map_err(|err| err.to_string())?;

    write_string_list(&mut output, "MTEX", "textures", textures.as_deref())?;
    write_string_list(&mut output, "MMDX", "models", models.as_deref())?;
    write_count(
        &mut output,
        "MMID",
        "model_offsets",
        model_offsets.as_ref().map(|offsets| offsets.len()),
    )?;
    write_string_list(&mut output, "MWMO", "wmos", wmos.as_deref())?;
    write_count(
        &mut output,
        "MWID",
        "wmo_offsets",
        wmo_offsets.as_ref().map(|offsets| offsets.len()),
    )?;
    write_count(
        &mut output,
        "MDDF",
        "doodads",
        doodads.as_ref().map(|placements| placements.len()),
    )?;
    write_placement_usage(
        &mut output,
        "MDDF",
        "model_refs",
        "model",
        doodads.as_deref().map(|placements| {
            placements
                .iter()
                .map(|placement| placement.name_id)
                .collect::<Vec<_>>()
        }),
        model_offsets.as_deref(),
        models.as_deref(),
    )?;
    write_count(
        &mut output,
        "MODF",
        "wmo_placements",
        wmo_placements.as_ref().map(|placements| placements.len()),
    )?;
    write_placement_usage(
        &mut output,
        "MODF",
        "wmo_refs",
        "wmo",
        wmo_placements.as_deref().map(|placements| {
            placements
                .iter()
                .map(|placement| placement.name_id)
                .collect::<Vec<_>>()
        }),
        wmo_offsets.as_deref(),
        wmos.as_deref(),
    )?;

    let mcnks = adt.mcnk_chunks().map_err(|err| err.to_string())?;
    writeln!(output, "MCNK chunks={}", mcnks.len()).map_err(|err| err.to_string())?;
    for (index, mcnk) in mcnks.iter().enumerate() {
        let header = mcnk.header;
        writeln!(
            output,
            "MCNK[{index}] grid=({},{}) area={} layers={} holes=0x{:x}",
            header.ix, header.iy, header.area_id, header.n_layers, header.holes
        )
        .map_err(|err| err.to_string())?;

        if let Some(heights) = mcnk.heights().map_err(|err| err.to_string())?
            && let Some((min, max)) = min_max(&heights)
        {
            writeln!(
                output,
                "MCNK[{index}] MCVT heights={} min={} max={}",
                heights.len(),
                min,
                max
            )
            .map_err(|err| err.to_string())?;
        }
        if let Some(normals) = mcnk.normals().map_err(|err| err.to_string())? {
            writeln!(output, "MCNK[{index}] MCNR normals={}", normals.len())
                .map_err(|err| err.to_string())?;
        }
        if let Some(layers) = mcnk.texture_layers().map_err(|err| err.to_string())? {
            writeln!(output, "MCNK[{index}] MCLY layers={}", layers.len())
                .map_err(|err| err.to_string())?;
        }
        if let Some(mcal) = mcnk
            .first_subchunk(*b"MCAL")
            .map_err(|err| err.to_string())?
        {
            writeln!(output, "MCNK[{index}] MCAL bytes={}", mcal.data.len())
                .map_err(|err| err.to_string())?;
        }
    }

    Ok(output)
}

fn inspect_map_path(path: &Path) -> Result<String, String> {
    let map = WorldMap::load_from_local_directory(path).map_err(|err| err.to_string())?;
    let mut output = String::new();

    writeln!(
        output,
        "MAP name={} tiles={} doodads={} wmo_placements={} terrain_chunks={}",
        map.name(),
        map.tiles().len(),
        map.total_model_placements(),
        map.total_wmo_placements(),
        map.total_terrain_chunks()
    )
    .map_err(|err| err.to_string())?;

    for tile in map.tiles() {
        let coord = tile.coord();
        writeln!(
            output,
            "TILE path={} coord=({},{}) textures={} models={} wmos={} doodads={} wmo_placements={} chunks={}",
            tile.source_path(),
            coord.x,
            coord.y,
            tile.texture_assets().len(),
            tile.model_assets().len(),
            tile.wmo_assets().len(),
            tile.model_placements().len(),
            tile.wmo_placements().len(),
            tile.terrain_chunks().len()
        )
        .map_err(|err| err.to_string())?;
    }

    Ok(output)
}

fn inspect_client_path(client_path: &Path, extra_archives: Vec<PathBuf>) -> Result<String, String> {
    let client = open_client(client_path, extra_archives)?;
    let mut output = String::new();
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

    writeln!(
        output,
        "CLIENT root={} data={} archives={} loaded={} skipped={} failed={}",
        client.root_path().display(),
        client.data_root().display(),
        client.archive_reports().len(),
        loaded,
        skipped,
        failed
    )
    .map_err(|err| err.to_string())?;

    for report in client.archive_reports() {
        match &report.state {
            ArchiveLoadState::Loaded => writeln!(
                output,
                "MPQ loaded size={} path={}",
                report.size,
                report.path.display()
            ),
            ArchiveLoadState::SkippedTooLarge { max_size } => writeln!(
                output,
                "MPQ skipped-too-large size={} max={} path={}",
                report.size,
                max_size,
                report.path.display()
            ),
            ArchiveLoadState::Failed { error } => writeln!(
                output,
                "MPQ failed size={} path={} error={}",
                report.size,
                report.path.display(),
                error
            ),
        }
        .map_err(|err| err.to_string())?;
    }

    Ok(output)
}

fn check_map_assets(
    map_path: &Path,
    client_path: &Path,
    extra_archives: Vec<PathBuf>,
) -> Result<String, String> {
    let map = WorldMap::load_from_local_directory(map_path).map_err(|err| err.to_string())?;
    let client = open_client(client_path, extra_archives)?;
    let assets = collect_map_assets(&map);
    let mut output = String::new();
    let mut found = 0usize;
    let mut missing = 0usize;

    writeln!(
        output,
        "ASSET_CHECK map={} assets={}",
        map.name(),
        assets.len()
    )
    .map_err(|err| err.to_string())?;

    for asset in assets {
        let path = VfsPath::new(&asset).map_err(|err| err.to_string())?;
        match client.read_file(&path) {
            Ok(bytes) => {
                found += 1;
                let source = client
                    .find_archive_for_file(&path)
                    .map(|path| path.display().to_string())
                    .unwrap_or_else(|| "<loose>".to_string());
                writeln!(
                    output,
                    "FOUND bytes={} source={} asset={}",
                    bytes.len(),
                    source,
                    asset
                )
                .map_err(|err| err.to_string())?;
            }
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => {
                missing += 1;
                writeln!(output, "MISSING asset={asset}").map_err(|err| err.to_string())?;
            }
            Err(err) => return Err(format!("failed to read asset {asset}: {err}")),
        }
    }

    writeln!(output, "ASSET_SUMMARY found={found} missing={missing}")
        .map_err(|err| err.to_string())?;
    Ok(output)
}

fn open_client(client_path: &Path, extra_archives: Vec<PathBuf>) -> Result<WowClient, String> {
    WowClient::open_with_config(
        client_path,
        WowClientConfig {
            extra_archives,
            ..WowClientConfig::default()
        },
    )
    .map_err(|err| format!("failed to open WoW client {}: {err}", client_path.display()))
}

fn collect_map_assets(map: &WorldMap) -> Vec<String> {
    let mut assets = BTreeSet::new();

    for tile in map.tiles() {
        assets.extend(tile.texture_assets().iter().cloned());
        assets.extend(tile.model_assets().iter().cloned());
        assets.extend(tile.wmo_assets().iter().cloned());
    }

    assets.into_iter().collect()
}

fn write_string_list(
    output: &mut String,
    chunk_id: &str,
    label: &str,
    values: Option<&[String]>,
) -> Result<(), String> {
    if let Some(values) = values {
        writeln!(output, "{chunk_id} {label}={}", values.len()).map_err(|err| err.to_string())?;
        for (index, value) in values.iter().enumerate() {
            writeln!(output, "{chunk_id}[{index}]={value}").map_err(|err| err.to_string())?;
        }
    }
    Ok(())
}

fn write_placement_usage(
    output: &mut String,
    chunk_id: &str,
    label: &str,
    asset_label: &str,
    placement_name_ids: Option<Vec<u32>>,
    offsets: Option<&[u32]>,
    filenames: Option<&[String]>,
) -> Result<(), String> {
    let Some(placement_name_ids) = placement_name_ids else {
        return Ok(());
    };

    let mut counts = BTreeMap::new();
    for name_id in placement_name_ids {
        *counts.entry(name_id).or_insert(0usize) += 1;
    }

    writeln!(output, "{chunk_id} {label}={}", counts.len()).map_err(|err| err.to_string())?;
    for (name_id, count) in counts {
        let asset_name = match (filenames, offsets) {
            (Some(filenames), Some(offsets)) => {
                filename_by_name_id(filenames, offsets, name_id).unwrap_or("<unresolved>")
            }
            _ => "<unresolved>",
        };
        writeln!(
            output,
            "{chunk_id}[{name_id}] {asset_label}={asset_name} placements={count}"
        )
        .map_err(|err| err.to_string())?;
    }

    Ok(())
}

fn write_count(
    output: &mut String,
    chunk_id: &str,
    label: &str,
    count: Option<usize>,
) -> Result<(), String> {
    if let Some(count) = count {
        writeln!(output, "{chunk_id} {label}={count}").map_err(|err| err.to_string())?;
    }
    Ok(())
}

fn min_max(values: &[f32]) -> Option<(f32, f32)> {
    let (first, rest) = values.split_first()?;
    let mut min = *first;
    let mut max = *first;

    for value in rest {
        min = min.min(*value);
        max = max.max(*value);
    }

    Some((min, max))
}

#[cfg(test)]
mod tests {
    use super::*;

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

    fn fixture_adt() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&stored_chunk(b"MVER", &18_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(
            b"MTEX",
            &string_block(&["tiles/foo.blp", "tiles/bar.blp"]),
        ));
        bytes.extend_from_slice(&stored_chunk(b"MMDX", &string_block(&["models/tree.m2"])));
        bytes.extend_from_slice(&stored_chunk(b"MMID", &0_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MWMO", &string_block(&["world/home.wmo"])));
        bytes.extend_from_slice(&stored_chunk(b"MWID", &0_u32.to_le_bytes()));
        bytes.extend_from_slice(&stored_chunk(b"MDDF", &mddf_entry()));
        bytes.extend_from_slice(&stored_chunk(b"MODF", &modf_entry()));
        bytes.extend_from_slice(&stored_chunk(b"MCNK", &mcnk()));
        bytes
    }

    fn mcnk() -> Vec<u8> {
        let mut bytes = vec![0; 128];
        write_u32(&mut bytes, 4, 4);
        write_u32(&mut bytes, 8, 9);
        write_u32(&mut bytes, 12, 2);
        write_u32(&mut bytes, 52, 617);
        write_u32(&mut bytes, 60, 0xAA55);

        push_subchunk(&mut bytes, 20, b"MCVT", &mcvt());
        push_subchunk(&mut bytes, 24, b"MCNR", &mcnr());
        push_subchunk(&mut bytes, 28, b"MCLY", &mcly());
        let mcal = vec![255; 4096];
        write_u32(&mut bytes, 40, (8 + mcal.len()) as u32);
        push_subchunk(&mut bytes, 36, b"MCAL", &mcal);
        bytes
    }

    fn mcvt() -> Vec<u8> {
        (0..145)
            .flat_map(|index| ((index as f32) * 0.25).to_le_bytes())
            .collect()
    }

    fn mcnr() -> Vec<u8> {
        vec![0; 145 * 3]
    }

    fn mcly() -> Vec<u8> {
        let mut bytes = Vec::new();
        push_u32s(&mut bytes, [4, 0, 0, 0xFFFF_FFFF]);
        push_u32s(&mut bytes, [9, 0x100, 0, 17]);
        bytes
    }

    fn mddf_entry() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&0_u32.to_le_bytes());
        bytes.extend_from_slice(&77_u32.to_le_bytes());
        push_vec3(&mut bytes, [1.0, 2.0, 3.0]);
        push_vec3(&mut bytes, [10.0, 20.0, 30.0]);
        bytes.extend_from_slice(&1024_u16.to_le_bytes());
        bytes.extend_from_slice(&0_u16.to_le_bytes());
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
        bytes.extend_from_slice(&0_u16.to_le_bytes());
        bytes.extend_from_slice(&0_u16.to_le_bytes());
        bytes.extend_from_slice(&0_u16.to_le_bytes());
        bytes.extend_from_slice(&1024_u16.to_le_bytes());
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

    fn push_subchunk(bytes: &mut Vec<u8>, offset_field: usize, id: &[u8; 4], data: &[u8]) {
        let offset = (bytes.len() + 8) as u32;
        write_u32(bytes, offset_field, offset);
        bytes.extend_from_slice(&stored_chunk(id, data));
    }

    fn push_u32s(bytes: &mut Vec<u8>, values: [u32; 4]) {
        for value in values {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
    }

    fn push_vec3(bytes: &mut Vec<u8>, values: [f32; 3]) {
        for value in values {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
    }

    fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
        bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    #[test]
    fn inspect_adt_reports_assets_and_terrain_summary() -> Result<(), String> {
        let output = inspect_adt_bytes(&fixture_adt())?;

        assert_eq!(
            output,
            "\
ADT chunks=9
MVER version=18
MTEX textures=2
MTEX[0]=tiles/foo.blp
MTEX[1]=tiles/bar.blp
MMDX models=1
MMDX[0]=models/tree.m2
MMID model_offsets=1
MWMO wmos=1
MWMO[0]=world/home.wmo
MWID wmo_offsets=1
MDDF doodads=1
MDDF model_refs=1
MDDF[0] model=models/tree.m2 placements=1
MODF wmo_placements=1
MODF wmo_refs=1
MODF[0] wmo=world/home.wmo placements=1
MCNK chunks=1
MCNK[0] grid=(4,9) area=617 layers=2 holes=0xaa55
MCNK[0] MCVT heights=145 min=0 max=36
MCNK[0] MCNR normals=145
MCNK[0] MCLY layers=2
MCNK[0] MCAL bytes=4096
"
        );
        Ok(())
    }
}
