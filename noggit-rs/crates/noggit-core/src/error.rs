//! Core-domain errors.

use std::error::Error;
use std::fmt::{Display, Formatter};
use std::io;
use std::path::PathBuf;

use noggit_formats::FormatError;
use noggit_vfs::VfsPath;

/// Result type used by core-domain operations.
pub type CoreResult<T> = Result<T, CoreError>;

/// Error returned by the editor core.
#[derive(Debug)]
pub enum CoreError {
    /// A local directory path does not expose a usable map directory name.
    MissingMapDirectoryName {
        /// Local path that could not be mapped to a WoW map name.
        root: PathBuf,
    },
    /// The VFS could not list or read a file.
    Io {
        /// Virtual path involved in the operation, when applicable.
        path: Option<VfsPath>,
        /// Underlying IO error.
        source: io::Error,
    },
    /// A binary format parser rejected a file.
    Format {
        /// Virtual path being parsed.
        path: VfsPath,
        /// Underlying format error.
        source: FormatError,
    },
}

impl Display for CoreError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::MissingMapDirectoryName { root } => write!(
                f,
                "could not infer map name from local directory {}",
                root.display()
            ),
            Self::Io { path, source } => {
                if let Some(path) = path {
                    write!(f, "failed to access {path}: {source}")
                } else {
                    write!(f, "failed to access virtual file source: {source}")
                }
            }
            Self::Format { path, source } => write!(f, "failed to parse {path}: {source}"),
        }
    }
}

impl Error for CoreError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::MissingMapDirectoryName { .. } => None,
            Self::Io { source, .. } => Some(source),
            Self::Format { source, .. } => Some(source),
        }
    }
}
