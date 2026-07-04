//! CLI entry point for validating rewrite milestones.

use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::ExitCode;

use noggit_formats::adt::AdtFile;
use noggit_formats::dbc::DbcFile;

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
    let path = args.next().map(PathBuf::from).ok_or_else(usage)?;

    let bytes =
        fs::read(&path).map_err(|err| format!("failed to read {}: {err}", path.display()))?;

    match command.as_str() {
        "inspect-dbc" => {
            let dbc = DbcFile::parse(&bytes).map_err(|err| err.to_string())?;
            let header = dbc.header();
            println!(
                "DBC records={} fields={} record_size={} string_block_size={}",
                header.record_count,
                header.field_count,
                header.record_size,
                header.string_block_size
            );
        }
        "inspect-adt" => {
            let adt = AdtFile::parse(&bytes).map_err(|err| err.to_string())?;
            println!("ADT chunks={}", adt.chunks().len());
            if let Some(version) = adt.version().map_err(|err| err.to_string())? {
                println!("MVER version={version}");
            }
        }
        _ => return Err(usage()),
    }

    Ok(())
}

fn usage() -> String {
    "usage: noggit-cli <inspect-dbc|inspect-adt> <path>".to_string()
}
