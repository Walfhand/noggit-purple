//! World of Warcraft client MPQ discovery and read-only file access.

use std::cell::RefCell;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use noggit_stormlib::StormArchive;

use crate::{FileSource, VfsPath};

const LOCALES: &[&str] = &[
    "enGB", "enUS", "deDE", "koKR", "frFR", "zhCN", "enCN", "zhTW", "enTW", "esES", "esMX", "ruRU",
    "jaJP", "ptPT", "ptBR", "itIT",
];

const ARCHIVE_NAME_TEMPLATES: &[&str] = &[
    "common.MPQ",
    "common-2.MPQ",
    "lichking.MPQ",
    "expansion.MPQ",
    "patch.MPQ",
    "patch-{number}.MPQ",
    "patch-{character}.MPQ",
    "alternate.MPQ",
];

const LOCALE_ARCHIVE_NAME_TEMPLATES: &[&str] = &[
    "{locale}/lichking-speech-{locale}.MPQ",
    "{locale}/expansion-speech-{locale}.MPQ",
    "{locale}/lichking-locale-{locale}.MPQ",
    "{locale}/expansion-locale-{locale}.MPQ",
    "{locale}/speech-{locale}.MPQ",
    "{locale}/locale-{locale}.MPQ",
    "{locale}/patch-{locale}.MPQ",
    "{locale}/patch-{locale}-{number}.MPQ",
    "{locale}/patch-{locale}-{character}.MPQ",
    "development.MPQ",
];

/// Configuration for opening a WoW client data source.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct WowClientConfig {
    /// Optional diagnostic size limit. Noggit-compatible loading leaves this unset.
    pub max_archive_size: Option<u64>,
    /// Additional archives to append with the highest priority.
    pub extra_archives: Vec<PathBuf>,
}

/// Report for one discovered MPQ archive.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClientArchiveReport {
    /// Archive path on disk.
    pub path: PathBuf,
    /// Archive file size in bytes.
    pub size: u64,
    /// Load state for this archive.
    pub state: ArchiveLoadState,
}

/// State of a discovered MPQ archive.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ArchiveLoadState {
    /// Archive was opened and will be searched.
    Loaded,
    /// Archive was intentionally skipped because it exceeds the configured size limit.
    SkippedTooLarge {
        /// Configured maximum archive size.
        max_size: u64,
    },
    /// Archive failed to open.
    Failed {
        /// Open error text.
        error: String,
    },
}

/// Read-only WoW client source backed by local loose files plus MPQ archives.
pub struct WowClient {
    root: PathBuf,
    data_root: PathBuf,
    archives: RefCell<Vec<ClientArchive>>,
    reports: Vec<ClientArchiveReport>,
}

struct ClientArchive {
    path: PathBuf,
    archive: StormArchive,
}

impl WowClient {
    /// Open a WoW client directory using default discovery settings.
    pub fn open(root: impl AsRef<Path>) -> io::Result<Self> {
        Self::open_with_config(root, WowClientConfig::default())
    }

    /// Open a WoW client directory with explicit discovery settings.
    pub fn open_with_config(root: impl AsRef<Path>, config: WowClientConfig) -> io::Result<Self> {
        let root = root.as_ref().to_path_buf();
        let data_root = if root.join("Data").is_dir() {
            root.join("Data")
        } else {
            root.clone()
        };

        let archive_paths = discover_archive_paths(&data_root, &config.extra_archives)?;
        let mut archives = Vec::new();
        let mut reports = Vec::new();

        for path in archive_paths {
            let size = fs::metadata(&path)?.len();
            if let Some(max_size) = config.max_archive_size
                && size > max_size
            {
                reports.push(ClientArchiveReport {
                    path,
                    size,
                    state: ArchiveLoadState::SkippedTooLarge { max_size },
                });
                continue;
            }

            match StormArchive::open(&path) {
                Ok(archive) => {
                    reports.push(ClientArchiveReport {
                        path: path.clone(),
                        size,
                        state: ArchiveLoadState::Loaded,
                    });
                    archives.push(ClientArchive { path, archive });
                }
                Err(error) => {
                    reports.push(ClientArchiveReport {
                        path,
                        size,
                        state: ArchiveLoadState::Failed {
                            error: error.to_string(),
                        },
                    });
                }
            }
        }

        Ok(Self {
            root,
            data_root,
            archives: RefCell::new(archives),
            reports,
        })
    }

    /// Return the client root path passed to the loader.
    pub fn root_path(&self) -> &Path {
        &self.root
    }

    /// Return the detected `Data` root.
    pub fn data_root(&self) -> &Path {
        &self.data_root
    }

    /// Return archive discovery and load reports in search-priority order.
    pub fn archive_reports(&self) -> &[ClientArchiveReport] {
        &self.reports
    }

    /// Return the archive path that currently provides a virtual file.
    pub fn find_archive_for_file(&self, path: &VfsPath) -> Option<PathBuf> {
        let archives = self.archives.borrow();

        for archive in archives.iter().rev() {
            if archive.archive.has_file(path.as_str()) {
                return Some(archive.path.clone());
            }
        }

        None
    }

    fn loose_path(&self, path: &VfsPath) -> PathBuf {
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

impl FileSource for WowClient {
    fn read_file(&self, path: &VfsPath) -> io::Result<Vec<u8>> {
        let loose_path = self.loose_path(path);
        if loose_path.is_file() {
            return fs::read(loose_path);
        }

        let mut last_error = None;
        let archives = self.archives.borrow();

        for archive in archives.iter().rev() {
            match archive.archive.read_file(path.as_str()) {
                Ok(data) => return Ok(data),
                Err(error) if error.is_not_found() => {}
                Err(error) => last_error = Some(error),
            }
        }

        if let Some(error) = last_error {
            Err(io::Error::new(io::ErrorKind::InvalidData, error))
        } else {
            Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("file not found in WoW client: {}", path.as_str()),
            ))
        }
    }

    fn list_files(&self, directory: &VfsPath) -> io::Result<Vec<VfsPath>> {
        let directory_path = self.loose_path(directory);
        if !directory_path.is_dir() {
            return Ok(Vec::new());
        }

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

fn discover_archive_paths(
    data_root: &Path,
    extra_archives: &[PathBuf],
) -> io::Result<Vec<PathBuf>> {
    let locale = detect_locale(data_root)?.unwrap_or_else(|| "enUS".to_string());
    let archive_templates = ordered_archive_templates(&locale);
    let mut paths = Vec::new();

    for template in archive_templates {
        expand_archive_template(data_root, &locale, template, &mut paths)?;
    }

    for path in extra_archives {
        push_unique(&mut paths, path.clone());
    }

    Ok(paths)
}

fn ordered_archive_templates(locale: &str) -> Vec<&'static str> {
    let mut templates =
        Vec::with_capacity(ARCHIVE_NAME_TEMPLATES.len() + LOCALE_ARCHIVE_NAME_TEMPLATES.len());

    if locale == "enGB" || locale == "enUS" {
        templates.extend(LOCALE_ARCHIVE_NAME_TEMPLATES);
        templates.extend(ARCHIVE_NAME_TEMPLATES);
    } else {
        templates.extend(ARCHIVE_NAME_TEMPLATES);
        templates.extend(LOCALE_ARCHIVE_NAME_TEMPLATES);
    }

    templates
}

fn expand_archive_template(
    data_root: &Path,
    locale: &str,
    template: &str,
    paths: &mut Vec<PathBuf>,
) -> io::Result<()> {
    let template = template.replace("{locale}", locale);

    if template.contains("{number}") {
        for number in '2'..='9' {
            let candidate = template.replace("{number}", &number.to_string());
            push_existing_archive(data_root, &candidate, paths)?;
        }
    } else if template.contains("{character}") {
        for character in 'a'..='z' {
            let candidate = template.replace("{character}", &character.to_string());
            push_existing_archive(data_root, &candidate, paths)?;
        }
    } else {
        push_existing_archive(data_root, &template, paths)?;
    }

    Ok(())
}

fn push_existing_archive(
    data_root: &Path,
    relative_path: &str,
    paths: &mut Vec<PathBuf>,
) -> io::Result<()> {
    let Some(path) = resolve_existing_child_path(data_root, relative_path)? else {
        return Ok(());
    };

    if path.is_file() {
        push_unique(paths, path);
    }

    Ok(())
}

fn push_unique(paths: &mut Vec<PathBuf>, path: PathBuf) {
    if !paths.iter().any(|existing| existing == &path) {
        paths.push(path);
    }
}

fn detect_locale(data_root: &Path) -> io::Result<Option<String>> {
    let mut first_existing = None;

    for locale in LOCALES {
        let Some(locale_path) = resolve_existing_child_path(data_root, locale)? else {
            continue;
        };

        if first_existing.is_none() {
            first_existing = Some((*locale).to_string());
        }

        if resolve_existing_child_path(&locale_path, "realmlist.wtf")?.is_some() {
            return Ok(Some((*locale).to_string()));
        }
    }

    Ok(first_existing)
}

fn resolve_existing_child_path(root: &Path, relative_path: &str) -> io::Result<Option<PathBuf>> {
    let mut current = root.to_path_buf();

    for segment in relative_path
        .split('/')
        .filter(|segment| !segment.is_empty())
    {
        let direct = current.join(segment);
        if direct.exists() {
            current = direct;
            continue;
        }

        if !current.is_dir() {
            return Ok(None);
        }

        let mut resolved = None;
        for entry in fs::read_dir(&current)? {
            let entry = entry?;
            let file_name = entry.file_name();
            if file_name.to_string_lossy().eq_ignore_ascii_case(segment) {
                resolved = Some(entry.path());
                break;
            }
        }

        let Some(path) = resolved else {
            return Ok(None);
        };
        current = path;
    }

    Ok(Some(current))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::error::Error;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn discovers_mpqs_and_skips_archives_over_size_limit() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-vfs-wow-client")?;
        let data = root.join("Data");
        let locale = data.join("enUS");
        fs::create_dir_all(&locale)?;
        fs::write(locale.join("realmlist.wtf"), b"test")?;
        fs::write(data.join("common.MPQ"), b"fake")?;
        fs::write(data.join("patch-4.MPQ"), b"fake")?;
        fs::write(locale.join("locale-enUS.MPQ"), b"fake")?;

        let client = WowClient::open_with_config(
            &root,
            WowClientConfig {
                max_archive_size: Some(0),
                extra_archives: Vec::new(),
            },
        )?;

        fs::remove_dir_all(&root)?;

        assert_eq!(client.archive_reports().len(), 3);
        assert!(client.archive_reports().iter().all(|report| {
            matches!(
                report.state,
                ArchiveLoadState::SkippedTooLarge { max_size: 0 }
            )
        }));
        assert_eq!(
            client
                .archive_reports()
                .iter()
                .map(|report| report.path.file_name().and_then(|value| value.to_str()))
                .collect::<Vec<_>>(),
            vec![
                Some("locale-enUS.MPQ"),
                Some("common.MPQ"),
                Some("patch-4.MPQ"),
            ]
        );
        Ok(())
    }

    #[test]
    fn reads_loose_files_before_archives() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-vfs-wow-loose")?;
        let path = root.join("World").join("Maps");
        fs::create_dir_all(&path)?;
        fs::write(path.join("loose.adt"), b"loose")?;

        let client = WowClient::open_with_config(
            &root,
            WowClientConfig {
                max_archive_size: Some(0),
                extra_archives: Vec::new(),
            },
        )?;
        let data = client.read_file(&VfsPath::new("World/Maps/loose.adt")?)?;

        fs::remove_dir_all(&root)?;

        assert_eq!(data, b"loose");
        Ok(())
    }

    #[test]
    fn reads_latest_archive_by_noggit_load_order() -> Result<(), Box<dyn Error>> {
        let root = test_root("noggit-vfs-wow-priority")?;
        let data = root.join("Data");
        fs::create_dir_all(&data)?;
        create_mpq(
            &data.join("common.MPQ"),
            &[("World/Maps/test.asset", b"base")],
        )?;
        create_mpq(
            &data.join("patch-4.MPQ"),
            &[("World/Maps/test.asset", b"patch")],
        )?;

        let client = WowClient::open(&root)?;
        let path = VfsPath::new("World/Maps/test.asset")?;
        let bytes = client.read_file(&path)?;
        let source = client.find_archive_for_file(&path);

        fs::remove_dir_all(&root)?;

        assert_eq!(bytes, b"patch");
        assert_eq!(
            source.and_then(|path| path.file_name().map(|value| value.to_os_string())),
            Some("patch-4.MPQ".into())
        );
        Ok(())
    }

    fn create_mpq(path: &Path, files: &[(&str, &[u8])]) -> Result<(), Box<dyn Error>> {
        let mut builder = wow_mpq::ArchiveBuilder::new();
        for (name, data) in files {
            builder = builder.add_file_data((*data).to_vec(), name);
        }
        builder.build(path)?;
        Ok(())
    }

    fn test_root(prefix: &str) -> Result<PathBuf, Box<dyn Error>> {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        Ok(std::env::temp_dir().join(format!("{prefix}-{}-{nanos}", std::process::id())))
    }
}
