//! Virtual file-system layer for local files, MPQ, and CASC.

pub mod wow_client;

use std::error::Error;
use std::fmt::{Display, Formatter};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

pub use wow_client::{ArchiveLoadState, ClientArchiveReport, WowClient, WowClientConfig};

/// Result type used by virtual file-system helpers.
pub type VfsResult<T> = Result<T, VfsPathError>;

/// Error returned when a virtual path is not valid inside an archive root.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VfsPathError {
    /// Parent path segments would escape the virtual root.
    ParentSegment,
}

/// Normalized path inside a WoW data source.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct VfsPath {
    path: String,
}

/// Read-only source for virtual files.
pub trait FileSource {
    /// Read a complete file by virtual path.
    fn read_file(&self, path: &VfsPath) -> io::Result<Vec<u8>>;

    /// List immediate child files under a virtual directory.
    fn list_files(&self, directory: &VfsPath) -> io::Result<Vec<VfsPath>>;
}

/// Local folder-backed file source.
#[derive(Debug, Clone)]
pub struct LocalFolder {
    root: PathBuf,
}

impl VfsPath {
    /// Build a normalized virtual path.
    pub fn new(path: impl AsRef<str>) -> VfsResult<Self> {
        let normalized = path.as_ref().replace('\\', "/");
        let mut segments = Vec::new();

        for segment in normalized.split('/') {
            if segment.is_empty() || segment == "." {
                continue;
            }
            if segment == ".." {
                return Err(VfsPathError::ParentSegment);
            }
            segments.push(segment);
        }

        Ok(Self {
            path: segments.join("/"),
        })
    }

    /// Return the virtual root path.
    pub fn root() -> Self {
        Self {
            path: String::new(),
        }
    }

    /// Return the normalized path as a slash-separated string.
    pub fn as_str(&self) -> &str {
        &self.path
    }

    /// Return the final path segment when present.
    pub fn file_name(&self) -> Option<&str> {
        if self.path.is_empty() {
            None
        } else {
            self.path.rsplit('/').next()
        }
    }
}

impl Display for VfsPath {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

impl Display for VfsPathError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ParentSegment => write!(f, "virtual paths cannot contain '..' segments"),
        }
    }
}

impl Error for VfsPathError {}

impl LocalFolder {
    /// Create a local folder-backed source.
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into() }
    }

    /// Return the OS path used as the virtual root.
    pub fn root_path(&self) -> &Path {
        &self.root
    }

    fn os_path(&self, path: &VfsPath) -> PathBuf {
        let mut out = self.root.clone();
        for segment in path
            .as_str()
            .split('/')
            .filter(|segment| !segment.is_empty())
        {
            out.push(segment);
        }
        out
    }
}

impl FileSource for LocalFolder {
    fn read_file(&self, path: &VfsPath) -> io::Result<Vec<u8>> {
        fs::read(self.os_path(path))
    }

    fn list_files(&self, directory: &VfsPath) -> io::Result<Vec<VfsPath>> {
        let directory_path = self.os_path(directory);
        let mut files = Vec::new();

        for entry in fs::read_dir(directory_path)? {
            let entry = entry?;
            if !entry.file_type()?.is_file() {
                continue;
            }

            let file_name = entry.file_name();
            let file_name = file_name.to_str().ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidData, "non-UTF-8 local filename")
            })?;
            let path = if directory.as_str().is_empty() {
                VfsPath::new(file_name)
            } else {
                VfsPath::new(format!("{}/{file_name}", directory.as_str()))
            }
            .map_err(|err| io::Error::new(io::ErrorKind::InvalidInput, err))?;

            files.push(path);
        }

        files.sort();
        Ok(files)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn normalizes_archive_style_paths() -> VfsResult<()> {
        let path = VfsPath::new(r"\World\\Maps\guerilla\guerilla_27_25.adt")?;

        assert_eq!(path.as_str(), "World/Maps/guerilla/guerilla_27_25.adt");
        assert_eq!(path.file_name(), Some("guerilla_27_25.adt"));
        Ok(())
    }

    #[test]
    fn rejects_parent_segments() {
        assert_eq!(
            VfsPath::new("World/../secret"),
            Err(VfsPathError::ParentSegment)
        );
    }

    #[test]
    fn lists_and_reads_local_files() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-vfs-local")?;
        fs::create_dir_all(&root)?;
        fs::write(root.join("a.adt"), b"adt")?;
        fs::write(root.join("b.wdt"), b"wdt")?;
        fs::create_dir(root.join("nested"))?;

        let source = LocalFolder::new(&root);
        let files = source.list_files(&VfsPath::root())?;
        let data = source.read_file(&VfsPath::new("a.adt")?)?;

        fs::remove_dir_all(&root)?;

        assert_eq!(files.len(), 2);
        assert_eq!(files[0].as_str(), "a.adt");
        assert_eq!(files[1].as_str(), "b.wdt");
        assert_eq!(data, b"adt");
        Ok(())
    }

    fn test_root(prefix: &str) -> Result<PathBuf, Box<dyn Error>> {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        Ok(std::env::temp_dir().join(format!("{prefix}-{}-{nanos}", std::process::id())))
    }
}
