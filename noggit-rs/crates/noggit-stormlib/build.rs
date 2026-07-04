//! Build script for locating and linking StormLib.

use std::env;
use std::path::Path;

fn main() {
    if let Ok(dir) = env::var("STORMLIB_LIB_DIR") {
        add_link_dir(&dir);
        return;
    }

    for dir in [
        "/home/linuxbrew/.linuxbrew/lib",
        "/usr/local/lib",
        "/usr/lib",
        "/usr/lib64",
    ] {
        let path = Path::new(dir);
        if path.join("libstorm.a").is_file() {
            add_static_link_dir(dir);
            return;
        }
        if path.join("libstorm.so").is_file() || path.join("libstorm.dylib").is_file() {
            add_link_dir(dir);
            return;
        }
    }

    println!("cargo:rustc-link-lib=storm");
}

fn add_link_dir(dir: &str) {
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=storm");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
}

fn add_static_link_dir(dir: &str) {
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=static=storm");
    println!("cargo:rustc-link-lib=static=z");
    println!("cargo:rustc-link-lib=static=bz2");
    println!("cargo:rustc-link-lib=dylib=stdc++");
}
