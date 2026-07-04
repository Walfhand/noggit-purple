//! Safe, read-only wrapper around the StormLib MPQ API used by Noggit.

use std::error::Error;
use std::ffi::CString;
use std::fmt::{Display, Formatter};
use std::os::raw::{c_char, c_uchar, c_uint, c_void};
use std::path::{Path, PathBuf};
use std::ptr::{self, NonNull};

#[cfg(unix)]
use std::os::unix::ffi::OsStrExt;

const STREAM_FLAG_READ_ONLY: c_uint = 0x0000_0100;
const MPQ_OPEN_NO_LISTFILE: c_uint = 0x0001_0000;
const SFILE_OPEN_FROM_MPQ: c_uint = 0x0000_0000;
const SFILE_INVALID_SIZE: c_uint = 0xFFFF_FFFF;
const ERROR_SUCCESS: c_uint = 0;
const ERROR_FILE_NOT_FOUND: c_uint = 2;

type StormHandle = *mut c_void;

unsafe extern "C" {
    fn SFileOpenArchive(
        name: *const c_char,
        priority: c_uint,
        flags: c_uint,
        archive: *mut StormHandle,
    ) -> c_uchar;
    fn SFileCloseArchive(archive: StormHandle) -> c_uchar;
    fn SFileHasFile(archive: StormHandle, name: *const c_char) -> c_uchar;
    fn SFileOpenFileEx(
        archive: StormHandle,
        name: *const c_char,
        search_scope: c_uint,
        file: *mut StormHandle,
    ) -> c_uchar;
    fn SFileGetFileSize(file: StormHandle, size_high: *mut c_uint) -> c_uint;
    fn SFileReadFile(
        file: StormHandle,
        buffer: *mut c_void,
        bytes_to_read: c_uint,
        bytes_read: *mut c_uint,
        overlapped: StormHandle,
    ) -> c_uchar;
    fn SFileCloseFile(file: StormHandle) -> c_uchar;
    fn SErrGetLastError() -> c_uint;
}

/// Read-only MPQ archive backed by StormLib.
#[derive(Debug)]
pub struct StormArchive {
    path: PathBuf,
    handle: NonNull<c_void>,
}

#[derive(Debug)]
struct StormFile {
    handle: NonNull<c_void>,
}

/// StormLib operation error.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StormError {
    operation: &'static str,
    code: c_uint,
    detail: String,
}

impl StormArchive {
    /// Open an MPQ archive read-only using the same flags as Noggit C++.
    pub fn open(path: impl AsRef<Path>) -> Result<Self, StormError> {
        let path = path.as_ref();
        let c_path = path_to_cstring(path)?;
        let mut handle: StormHandle = ptr::null_mut();
        let flags = MPQ_OPEN_NO_LISTFILE | STREAM_FLAG_READ_ONLY;

        // SAFETY: `c_path` is a valid null-terminated path and `handle` points
        // to writable storage for the StormLib archive handle.
        let opened = unsafe { SFileOpenArchive(c_path.as_ptr(), 0, flags, &mut handle) };

        if opened == 0 {
            return Err(last_error(
                "SFileOpenArchive",
                format!("failed to open {}", path.display()),
            ));
        }

        let Some(handle) = NonNull::new(handle) else {
            return Err(StormError::new(
                "SFileOpenArchive",
                ERROR_SUCCESS,
                format!("StormLib returned a null handle for {}", path.display()),
            ));
        };

        Ok(Self {
            path: path.to_path_buf(),
            handle,
        })
    }

    /// Return the archive path on disk.
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Return whether the archive contains a virtual WoW path.
    pub fn has_file(&self, path: &str) -> bool {
        let Ok(c_path) = wow_path_to_cstring(path) else {
            return false;
        };

        // SAFETY: `self.handle` is a live StormLib archive handle and `c_path`
        // is a valid null-terminated archive path.
        unsafe { SFileHasFile(self.handle.as_ptr(), c_path.as_ptr()) != 0 }
    }

    /// Read a complete file by virtual WoW path.
    pub fn read_file(&self, path: &str) -> Result<Vec<u8>, StormError> {
        let file = self.open_file(path)?;
        let size = file.size()?;
        if size > c_uint::MAX as u64 {
            return Err(StormError::new(
                "SFileReadFile",
                ERROR_SUCCESS,
                format!("file is too large for one StormLib read: {path} ({size} bytes)"),
            ));
        }

        let mut buffer = vec![0_u8; size as usize];
        let mut read = 0_u32;
        let bytes_to_read = size as c_uint;

        // SAFETY: `file.handle` is a live StormLib file handle. `buffer` has
        // exactly `bytes_to_read` writable bytes, and `read` is writable.
        let ok = unsafe {
            SFileReadFile(
                file.handle.as_ptr(),
                buffer.as_mut_ptr().cast(),
                bytes_to_read,
                &mut read,
                ptr::null_mut(),
            )
        };

        if ok == 0 {
            return Err(last_error(
                "SFileReadFile",
                format!("failed to read {path} from {}", self.path.display()),
            ));
        }
        if read != bytes_to_read {
            return Err(StormError::new(
                "SFileReadFile",
                ERROR_SUCCESS,
                format!(
                    "short read for {path} from {}: expected {bytes_to_read}, got {read}",
                    self.path.display()
                ),
            ));
        }

        Ok(buffer)
    }

    fn open_file(&self, path: &str) -> Result<StormFile, StormError> {
        let c_path = wow_path_to_cstring(path)?;
        let mut handle: StormHandle = ptr::null_mut();

        // SAFETY: `self.handle` is a live StormLib archive handle, `c_path` is
        // a valid null-terminated archive path, and `handle` is writable.
        let opened = unsafe {
            SFileOpenFileEx(
                self.handle.as_ptr(),
                c_path.as_ptr(),
                SFILE_OPEN_FROM_MPQ,
                &mut handle,
            )
        };

        if opened == 0 {
            return Err(last_error(
                "SFileOpenFileEx",
                format!("failed to open {path} from {}", self.path.display()),
            ));
        }

        let Some(handle) = NonNull::new(handle) else {
            return Err(StormError::new(
                "SFileOpenFileEx",
                ERROR_SUCCESS,
                format!("StormLib returned a null file handle for {path}"),
            ));
        };

        Ok(StormFile { handle })
    }
}

impl StormFile {
    fn size(&self) -> Result<u64, StormError> {
        let mut high = 0_u32;

        // SAFETY: `self.handle` is a live StormLib file handle and `high`
        // points to writable storage for the high 32 bits.
        let low = unsafe { SFileGetFileSize(self.handle.as_ptr(), &mut high) };

        if low == SFILE_INVALID_SIZE && high == 0 {
            // SAFETY: StormLib exposes a thread-local last-error value.
            let code = unsafe { SErrGetLastError() };
            if code != ERROR_SUCCESS {
                return Err(StormError::new(
                    "SFileGetFileSize",
                    code,
                    "failed to read file size".to_string(),
                ));
            }
        }

        Ok(((high as u64) << 32) | low as u64)
    }
}

impl Drop for StormArchive {
    fn drop(&mut self) {
        // SAFETY: `self.handle` is owned by this wrapper and is closed once.
        unsafe {
            SFileCloseArchive(self.handle.as_ptr());
        }
    }
}

impl Drop for StormFile {
    fn drop(&mut self) {
        // SAFETY: `self.handle` is owned by this wrapper and is closed once.
        unsafe {
            SFileCloseFile(self.handle.as_ptr());
        }
    }
}

impl StormError {
    fn new(operation: &'static str, code: c_uint, detail: String) -> Self {
        Self {
            operation,
            code,
            detail,
        }
    }

    /// Return the raw StormLib error code.
    pub fn code(&self) -> c_uint {
        self.code
    }

    /// Return whether StormLib reported a missing file.
    pub fn is_not_found(&self) -> bool {
        self.code == ERROR_FILE_NOT_FOUND
    }
}

impl Display for StormError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{} failed with StormLib code {}: {}",
            self.operation, self.code, self.detail
        )
    }
}

impl Error for StormError {}

/// Normalize a WoW virtual path the same way Noggit does before StormLib calls.
pub fn normalize_wow_path(path: &str) -> String {
    let mut normalized = path
        .chars()
        .map(|c| {
            if c == '/' {
                '\\'
            } else {
                c.to_ascii_uppercase()
            }
        })
        .collect::<String>();

    if normalized.ends_with(".MDX") || normalized.ends_with(".MDL") {
        normalized.truncate(normalized.len() - 4);
        normalized.push_str(".M2");
    }

    normalized
}

fn last_error(operation: &'static str, detail: String) -> StormError {
    // SAFETY: StormLib exposes a thread-local last-error value.
    let code = unsafe { SErrGetLastError() };
    StormError::new(operation, code, detail)
}

fn wow_path_to_cstring(path: &str) -> Result<CString, StormError> {
    CString::new(normalize_wow_path(path)).map_err(|_| {
        StormError::new(
            "CString::new",
            ERROR_SUCCESS,
            format!("path contains an interior NUL byte: {path:?}"),
        )
    })
}

fn path_to_cstring(path: &Path) -> Result<CString, StormError> {
    #[cfg(unix)]
    let bytes = path.as_os_str().as_bytes().to_vec();
    #[cfg(not(unix))]
    let bytes = path.to_string_lossy().as_bytes().to_vec();

    CString::new(bytes).map_err(|_| {
        StormError::new(
            "CString::new",
            ERROR_SUCCESS,
            format!("path contains an interior NUL byte: {}", path.display()),
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};
    use wow_mpq::ArchiveBuilder;

    #[test]
    fn normalizes_wow_archive_paths() {
        assert_eq!(
            normalize_wow_path("world/maps/foo/tree.mdx"),
            r"WORLD\MAPS\FOO\TREE.M2"
        );
        assert_eq!(
            normalize_wow_path(r"tileset\stormwind\stone.blp"),
            r"TILESET\STORMWIND\STONE.BLP"
        );
    }

    #[test]
    fn reads_mpq_file_with_stormlib() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-stormlib")?;
        fs::create_dir_all(&root)?;
        let archive_path = root.join("patch.MPQ");

        ArchiveBuilder::new()
            .add_file_data(b"bridge".to_vec(), r"World\Wmo\bridge.wmo")
            .build(&archive_path)?;

        let archive = StormArchive::open(&archive_path)?;
        assert!(archive.has_file("world/wmo/bridge.wmo"));
        assert_eq!(archive.read_file("world/wmo/bridge.wmo")?, b"bridge");
        assert!(matches!(
            archive.read_file("missing.file"),
            Err(error) if error.is_not_found()
        ));

        fs::remove_dir_all(&root)?;
        Ok(())
    }

    fn test_root(prefix: &str) -> Result<PathBuf, Box<dyn Error>> {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        Ok(std::env::temp_dir().join(format!("{prefix}-{}-{nanos}", std::process::id())))
    }
}
