//! lodos-rootfs: fast rootless rootfs tarball extractor (.tar.gz, .tar.xz, .tar.zst).
//!
//! Preserves directories, regular files, Unix permissions, symlinks (verbatim targets),
//! and mtimes. Materializes hardlinks via reflink-free copies, skips char/block/fifo
//! device nodes while counting them, and enforces strict path-traversal and
//! symlink-escape protection so writes never escape `dst`.

#[cfg(feature = "jni")]
pub mod jni;

use std::collections::HashSet;
use std::ffi::CString;
use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{symlink, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::io::AsRawFd;
use std::path::{Component, Path, PathBuf};
use std::time::Instant;

use flate2::read::MultiGzDecoder;
use tar::{Archive, EntryType};
use xz2::read::XzDecoder;
use zstd::stream::read::Decoder as ZstdDecoder;

const IO_BUF_SIZE: usize = 256 * 1024;
const COPY_BUF_SIZE: usize = 64 * 1024;
const PROGRESS_STEP_BYTES: u64 = 256 * 1024;

/// Summary of an extraction run.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ExtractSummary {
    pub files: u64,
    pub dirs: u64,
    pub symlinks: u64,
    pub hardlinks: u64,
    pub skipped_dev: u64,
    pub ms: u64,
}

impl fmt::Display for ExtractSummary {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "files={} dirs={} symlinks={} hardlinks={} skipped_dev={} ms={}",
            self.files, self.dirs, self.symlinks, self.hardlinks, self.skipped_dev, self.ms
        )
    }
}

/// Supported tarball compression formats.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ArchiveFormat {
    TarGz,
    TarXz,
    TarZst,
}

/// Errors returned during rootfs extraction.
#[derive(Debug)]
pub enum ExtractError {
    Io(io::Error),
    UnsupportedFormat(String),
    PathTraversal(String),
    InvalidHardlink(String),
    InvalidEntry(String),
}

impl fmt::Display for ExtractError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(e) => write!(f, "I/O error: {e}"),
            Self::UnsupportedFormat(s) => write!(f, "unsupported archive format: {s}"),
            Self::PathTraversal(s) => write!(f, "path traversal rejected: {s}"),
            Self::InvalidHardlink(s) => write!(f, "invalid hardlink: {s}"),
            Self::InvalidEntry(s) => write!(f, "invalid tar entry: {s}"),
        }
    }
}

impl std::error::Error for ExtractError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<io::Error> for ExtractError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

struct ProgressReader<R, F> {
    inner: R,
    read_bytes: u64,
    total_bytes: u64,
    last_reported: u64,
    cb: F,
}

impl<R: Read, F: FnMut(u64, u64)> ProgressReader<R, F> {
    fn new(inner: R, total_bytes: u64, mut cb: F) -> Self {
        cb(0, total_bytes);
        Self {
            inner,
            read_bytes: 0,
            total_bytes,
            last_reported: 0,
            cb,
        }
    }

    fn finish(&mut self) {
        if self.read_bytes != self.last_reported || self.read_bytes == self.total_bytes {
            self.last_reported = self.read_bytes;
            (self.cb)(self.read_bytes, self.total_bytes);
        }
    }
}

impl<R: Read, F: FnMut(u64, u64)> Read for ProgressReader<R, F> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        if n > 0 {
            self.read_bytes = self.read_bytes.saturating_add(n as u64);
            if self.read_bytes >= self.last_reported.saturating_add(PROGRESS_STEP_BYTES)
                || self.read_bytes >= self.total_bytes
            {
                self.last_reported = self.read_bytes;
                (self.cb)(self.read_bytes, self.total_bytes);
            }
        } else if self.read_bytes != self.last_reported {
            self.last_reported = self.read_bytes;
            (self.cb)(self.read_bytes, self.total_bytes);
        }
        Ok(n)
    }
}

/// Detect tarball compression format by magic header bytes, falling back to file extension.
pub fn detect_format(magic: &[u8], path: &Path) -> Result<ArchiveFormat, ExtractError> {
    if magic.len() >= 2 && magic[0] == 0x1f && magic[1] == 0x8b {
        return Ok(ArchiveFormat::TarGz);
    }
    if magic.len() >= 6 && magic[..6] == [0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00] {
        return Ok(ArchiveFormat::TarXz);
    }
    if magic.len() >= 4 && magic[..4] == [0x28, 0xb5, 0x2f, 0xfd] {
        return Ok(ArchiveFormat::TarZst);
    }

    let name = path
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    if name.ends_with(".tar.gz") || name.ends_with(".tgz") {
        Ok(ArchiveFormat::TarGz)
    } else if name.ends_with(".tar.xz") || name.ends_with(".txz") {
        Ok(ArchiveFormat::TarXz)
    } else if name.ends_with(".tar.zst") || name.ends_with(".tzst") {
        Ok(ArchiveFormat::TarZst)
    } else {
        Err(ExtractError::UnsupportedFormat(path.display().to_string()))
    }
}

/// Sanitize a tar entry path so it is strictly relative to `dst`.
/// Rejects `..` components, leading `/` (absolute paths), Windows prefixes, and NUL bytes.
fn sanitize_rel_path(raw: &Path) -> Result<PathBuf, ExtractError> {
    if raw.as_os_str().as_bytes().contains(&0) {
        return Err(ExtractError::PathTraversal(format!(
            "embedded NUL byte in {:?}",
            raw
        )));
    }
    let mut clean = PathBuf::new();
    for comp in raw.components() {
        match comp {
            Component::CurDir => {}
            Component::Normal(part) => clean.push(part),
            Component::ParentDir => {
                return Err(ExtractError::PathTraversal(format!(
                    "parent traversal (..) in {}",
                    raw.display()
                )));
            }
            Component::RootDir | Component::Prefix(_) => {
                return Err(ExtractError::PathTraversal(format!(
                    "absolute path escape in {}",
                    raw.display()
                )));
            }
        }
    }
    Ok(clean)
}

/// Ensure every directory component of `rel_dir` exists under `dst` as a real directory
/// (never a symlink). Caches verified directories in `verified_dirs`.
fn ensure_real_dir(
    dst: &Path,
    rel_dir: &Path,
    verified_dirs: &mut HashSet<PathBuf>,
) -> Result<(), ExtractError> {
    if rel_dir.as_os_str().is_empty() || verified_dirs.contains(rel_dir) {
        return Ok(());
    }
    let mut cur_rel = PathBuf::new();
    let mut cur_abs = dst.to_path_buf();
    for comp in rel_dir.components() {
        let Component::Normal(name) = comp else {
            return Err(ExtractError::PathTraversal(format!(
                "unexpected component in {}",
                rel_dir.display()
            )));
        };
        cur_rel.push(name);
        cur_abs.push(name);
        if verified_dirs.contains(&cur_rel) {
            continue;
        }
        match fs::symlink_metadata(&cur_abs) {
            Ok(meta) => {
                if meta.file_type().is_symlink() {
                    return Err(ExtractError::PathTraversal(format!(
                        "refusing to traverse symlink parent {}",
                        cur_rel.display()
                    )));
                }
                if !meta.is_dir() {
                    return Err(ExtractError::InvalidEntry(format!(
                        "parent {} exists and is not a directory",
                        cur_rel.display()
                    )));
                }
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {
                fs::create_dir(&cur_abs)?;
            }
            Err(e) => return Err(ExtractError::Io(e)),
        }
        verified_dirs.insert(cur_rel.clone());
    }
    Ok(())
}

/// Check that every ancestor of `rel_path` is a real directory under `dst` (never a symlink),
/// without creating missing directories.
fn verify_existing_ancestors_no_symlink(dst: &Path, rel_path: &Path) -> Result<(), ExtractError> {
    let Some(parent) = rel_path.parent() else {
        return Ok(());
    };
    let mut cur_abs = dst.to_path_buf();
    let mut cur_rel = PathBuf::new();
    for comp in parent.components() {
        let Component::Normal(name) = comp else {
            return Err(ExtractError::PathTraversal(format!(
                "unexpected component in {}",
                rel_path.display()
            )));
        };
        cur_rel.push(name);
        cur_abs.push(name);
        let meta = fs::symlink_metadata(&cur_abs).map_err(|e| {
            if e.kind() == io::ErrorKind::NotFound {
                ExtractError::InvalidHardlink(format!(
                    "hardlink target ancestor missing: {}",
                    cur_rel.display()
                ))
            } else {
                ExtractError::Io(e)
            }
        })?;
        if meta.file_type().is_symlink() {
            return Err(ExtractError::PathTraversal(format!(
                "refusing to traverse symlink ancestor {}",
                cur_rel.display()
            )));
        }
        if !meta.is_dir() {
            return Err(ExtractError::InvalidHardlink(format!(
                "hardlink ancestor is not a directory: {}",
                cur_rel.display()
            )));
        }
    }
    Ok(())
}

fn prepare_leaf_slot(
    abs_path: &Path,
    rel_path: &Path,
    verified_dirs: &mut HashSet<PathBuf>,
) -> Result<(), ExtractError> {
    match fs::symlink_metadata(abs_path) {
        Ok(meta) => {
            if meta.is_dir() && !meta.file_type().is_symlink() {
                return Err(ExtractError::InvalidEntry(format!(
                    "cannot replace directory {} with non-directory entry",
                    rel_path.display()
                )));
            }
            fs::remove_file(abs_path)?;
            verified_dirs.remove(rel_path);
            Ok(())
        }
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(ExtractError::Io(e)),
    }
}

fn set_fd_mode(file: &File, mode: u32) -> io::Result<()> {
    let full = mode & 0o7777;
    let fd = file.as_raw_fd();
    let rc = unsafe { libc::fchmod(fd, full as libc::mode_t) };
    if rc == 0 {
        return Ok(());
    }
    let err = io::Error::last_os_error();
    if full != (mode & 0o0777) && err.raw_os_error() == Some(libc::EPERM) {
        let rc2 = unsafe { libc::fchmod(fd, (mode & 0o0777) as libc::mode_t) };
        if rc2 == 0 {
            return Ok(());
        }
        return Err(io::Error::last_os_error());
    }
    Err(err)
}

fn set_dir_mode(path: &Path, mode: u32) -> io::Result<()> {
    let full = mode & 0o7777;
    match fs::set_permissions(path, fs::Permissions::from_mode(full)) {
        Ok(()) => Ok(()),
        Err(e) if full != (mode & 0o0777) && e.raw_os_error() == Some(libc::EPERM) => {
            fs::set_permissions(path, fs::Permissions::from_mode(mode & 0o0777))
        }
        Err(e) => Err(e),
    }
}

fn set_fd_mtime(file: &File, mtime_secs: i64) -> io::Result<()> {
    let ts = [
        libc::timespec {
            tv_sec: mtime_secs as libc::time_t,
            tv_nsec: 0,
        },
        libc::timespec {
            tv_sec: mtime_secs as libc::time_t,
            tv_nsec: 0,
        },
    ];
    let rc = unsafe { libc::futimens(file.as_raw_fd(), ts.as_ptr()) };
    if rc == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

fn set_path_mtime_nofollow(path: &Path, mtime_secs: i64) -> io::Result<()> {
    let c_path = CString::new(path.as_os_str().as_bytes())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "NUL in path"))?;
    let ts = [
        libc::timespec {
            tv_sec: mtime_secs as libc::time_t,
            tv_nsec: 0,
        },
        libc::timespec {
            tv_sec: mtime_secs as libc::time_t,
            tv_nsec: 0,
        },
    ];
    let rc = unsafe {
        libc::utimensat(
            libc::AT_FDCWD,
            c_path.as_ptr(),
            ts.as_ptr(),
            libc::AT_SYMLINK_NOFOLLOW,
        )
    };
    if rc == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

/// Reflink-free copy from `src_file` to `dst_file` using an explicit read/write loop.
fn copy_plain(src_file: &mut File, dst_file: &mut File, buf: &mut [u8]) -> io::Result<()> {
    loop {
        let n = src_file.read(buf)?;
        if n == 0 {
            break;
        }
        dst_file.write_all(&buf[..n])?;
    }
    Ok(())
}

/// Extract entries from an already-decompressed tar stream into `dst`.
fn extract_archive_stream<R: Read>(
    reader: R,
    dst: &Path,
) -> Result<ExtractSummary, ExtractError> {
    fs::create_dir_all(dst)?;

    let mut archive = Archive::new(reader);
    archive.set_preserve_permissions(false);
    archive.set_preserve_mtime(false);
    archive.set_unpack_xattrs(false);

    let mut summary = ExtractSummary::default();
    let mut verified_dirs: HashSet<PathBuf> = HashSet::with_capacity(256);
    verified_dirs.insert(PathBuf::new());

    let mut deferred_dirs: Vec<(PathBuf, Option<u32>, Option<i64>)> = Vec::with_capacity(256);
    let mut copy_buf = vec![0u8; COPY_BUF_SIZE];

    for entry_res in archive.entries()? {
        let mut entry = entry_res?;
        let entry_type = entry.header().entry_type();

        // Skip tar metadata pseudo-entries if ever yielded.
        if matches!(
            entry_type,
            EntryType::XHeader
                | EntryType::XGlobalHeader
                | EntryType::GNULongName
                | EntryType::GNULongLink
        ) {
            continue;
        }

        let raw_path = entry.path()?.into_owned();
        let rel_path = sanitize_rel_path(&raw_path)?;

        // Always validate path traversal before skipping device nodes.
        if matches!(
            entry_type,
            EntryType::Char | EntryType::Block | EntryType::Fifo
        ) {
            summary.skipped_dev += 1;
            continue;
        }

        let mode = entry.header().mode().ok();
        let mtime = entry.header().mtime().ok().map(|v| v as i64);

        match entry_type {
            EntryType::Directory => {
                if rel_path.as_os_str().is_empty() {
                    deferred_dirs.push((dst.to_path_buf(), mode, mtime));
                    summary.dirs += 1;
                    continue;
                }
                if let Some(parent) = rel_path.parent() {
                    ensure_real_dir(dst, parent, &mut verified_dirs)?;
                }
                let abs_path = dst.join(&rel_path);
                match fs::symlink_metadata(&abs_path) {
                    Ok(meta) => {
                        if meta.file_type().is_symlink() {
                            return Err(ExtractError::PathTraversal(format!(
                                "refusing to follow symlink for directory {}",
                                rel_path.display()
                            )));
                        }
                        if !meta.is_dir() {
                            return Err(ExtractError::InvalidEntry(format!(
                                "directory entry collides with non-directory {}",
                                rel_path.display()
                            )));
                        }
                    }
                    Err(e) if e.kind() == io::ErrorKind::NotFound => {
                        fs::create_dir(&abs_path)?;
                    }
                    Err(e) => return Err(ExtractError::Io(e)),
                }
                verified_dirs.insert(rel_path);
                deferred_dirs.push((abs_path, mode, mtime));
                summary.dirs += 1;
            }
            EntryType::Regular | EntryType::Continuous => {
                if rel_path.as_os_str().is_empty() {
                    return Err(ExtractError::InvalidEntry(
                        "regular file with empty path".into(),
                    ));
                }
                if let Some(parent) = rel_path.parent() {
                    ensure_real_dir(dst, parent, &mut verified_dirs)?;
                }
                let abs_path = dst.join(&rel_path);
                prepare_leaf_slot(&abs_path, &rel_path, &mut verified_dirs)?;

                let mut out = OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .mode(0o600)
                    .custom_flags(libc::O_NOFOLLOW)
                    .open(&abs_path)?;

                loop {
                    let n = entry.read(&mut copy_buf)?;
                    if n == 0 {
                        break;
                    }
                    out.write_all(&copy_buf[..n])?;
                }

                if let Some(m) = mode {
                    set_fd_mode(&out, m)?;
                }
                if let Some(t) = mtime {
                    let _ = set_fd_mtime(&out, t);
                }
                summary.files += 1;
            }
            EntryType::Symlink => {
                if rel_path.as_os_str().is_empty() {
                    return Err(ExtractError::InvalidEntry(
                        "symlink with empty path".into(),
                    ));
                }
                let target = entry
                    .link_name()?
                    .ok_or_else(|| {
                        ExtractError::InvalidEntry(format!(
                            "symlink {} missing link target",
                            rel_path.display()
                        ))
                    })?
                    .into_owned();
                if let Some(parent) = rel_path.parent() {
                    ensure_real_dir(dst, parent, &mut verified_dirs)?;
                }
                let abs_path = dst.join(&rel_path);
                prepare_leaf_slot(&abs_path, &rel_path, &mut verified_dirs)?;

                symlink(&target, &abs_path)?;
                if let Some(t) = mtime {
                    let _ = set_path_mtime_nofollow(&abs_path, t);
                }
                summary.symlinks += 1;
            }
            EntryType::Link => {
                if rel_path.as_os_str().is_empty() {
                    return Err(ExtractError::InvalidHardlink(
                        "hardlink with empty path".into(),
                    ));
                }
                let raw_target = entry
                    .link_name()?
                    .ok_or_else(|| {
                        ExtractError::InvalidHardlink(format!(
                            "hardlink {} missing target",
                            rel_path.display()
                        ))
                    })?
                    .into_owned();
                let rel_target = sanitize_rel_path(&raw_target)?;
                if rel_target.as_os_str().is_empty() {
                    return Err(ExtractError::InvalidHardlink(format!(
                        "hardlink {} points to root",
                        rel_path.display()
                    )));
                }

                verify_existing_ancestors_no_symlink(dst, &rel_target)?;
                let abs_target = dst.join(&rel_target);
                let src_meta = fs::symlink_metadata(&abs_target).map_err(|e| {
                    if e.kind() == io::ErrorKind::NotFound {
                        ExtractError::InvalidHardlink(format!(
                            "hardlink target not found: {}",
                            rel_target.display()
                        ))
                    } else {
                        ExtractError::Io(e)
                    }
                })?;

                if let Some(parent) = rel_path.parent() {
                    ensure_real_dir(dst, parent, &mut verified_dirs)?;
                }
                let abs_path = dst.join(&rel_path);
                prepare_leaf_slot(&abs_path, &rel_path, &mut verified_dirs)?;

                if src_meta.file_type().is_symlink() {
                    // Hardlink to a symlink inside dst: duplicate the symlink without following it.
                    let link_target = fs::read_link(&abs_target)?;
                    symlink(&link_target, &abs_path)?;
                    if let Some(t) = mtime {
                        let _ = set_path_mtime_nofollow(&abs_path, t);
                    }
                } else if src_meta.is_file() {
                    let mut src_file = OpenOptions::new()
                        .read(true)
                        .custom_flags(libc::O_NOFOLLOW)
                        .open(&abs_target)?;
                    let mut dst_file = OpenOptions::new()
                        .write(true)
                        .create_new(true)
                        .mode(0o600)
                        .custom_flags(libc::O_NOFOLLOW)
                        .open(&abs_path)?;

                    copy_plain(&mut src_file, &mut dst_file, &mut copy_buf)?;

                    let eff_mode = mode.unwrap_or_else(|| src_meta.mode());
                    set_fd_mode(&dst_file, eff_mode)?;
                    let eff_mtime = mtime.unwrap_or_else(|| src_meta.mtime());
                    let _ = set_fd_mtime(&dst_file, eff_mtime);
                } else {
                    return Err(ExtractError::InvalidHardlink(format!(
                        "hardlink target {} is neither regular file nor symlink",
                        rel_target.display()
                    )));
                }
                summary.hardlinks += 1;
            }
            _ => {
                // Ignore any other non-filesystem metadata entries.
            }
        }
    }

    // Apply directory mtimes and modes deepest-first so writing children doesn't clobber
    // parent mtimes and read-only parent directories don't block child creation.
    deferred_dirs.sort_by(|a, b| b.0.components().count().cmp(&a.0.components().count()));
    for (dir_path, mode, mtime) in deferred_dirs {
        if let Some(t) = mtime {
            let _ = set_path_mtime_nofollow(&dir_path, t);
        }
        if let Some(m) = mode {
            // Keep directories owner-rwx if the archive had unusual 0000 modes? No, honor mode,
            // falling back to 0777 mask if EPERM on special bits.
            let _ = set_dir_mode(&dir_path, m);
        }
    }

    Ok(summary)
}

/// Extract a `.tar.gz`, `.tar.xz`, or `.tar.zst` tarball into `dst`.
pub fn extract_tarball(src: &Path, dst: &Path) -> Result<ExtractSummary, ExtractError> {
    extract_tarball_with_progress(src, dst, |_, _| {})
}

/// Extract a `.tar.gz`, `.tar.xz`, or `.tar.zst` tarball into `dst`, invoking
/// `on_progress(bytes_in, total_bytes)` as compressed input bytes are read.
pub fn extract_tarball_with_progress<F>(
    src: &Path,
    dst: &Path,
    on_progress: F,
) -> Result<ExtractSummary, ExtractError>
where
    F: FnMut(u64, u64),
{
    let t0 = Instant::now();
    let file = File::open(src)?;
    let total_bytes = file.metadata()?.len();

    let progress = ProgressReader::new(file, total_bytes, on_progress);
    let mut buffered = BufReader::with_capacity(IO_BUF_SIZE, progress);
    let magic = buffered.fill_buf()?;
    let format = detect_format(magic, src)?;

    let mut summary = match format {
        ArchiveFormat::TarGz => {
            let mut dec = MultiGzDecoder::new(buffered);
            let res = extract_archive_stream(&mut dec, dst)?;
            dec.into_inner().into_inner().finish();
            res
        }
        ArchiveFormat::TarXz => {
            let mut dec = XzDecoder::new(buffered);
            let res = extract_archive_stream(&mut dec, dst)?;
            dec.into_inner().into_inner().finish();
            res
        }
        ArchiveFormat::TarZst => {
            let mut dec = ZstdDecoder::with_buffer(buffered)?;
            let res = extract_archive_stream(&mut dec, dst)?;
            dec.finish().into_inner().finish();
            res
        }
    };

    summary.ms = t0.elapsed().as_millis() as u64;
    Ok(summary)
}
