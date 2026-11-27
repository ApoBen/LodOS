use std::fs::{self, File};
use std::io::{self, Write};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::Command;

use flate2::write::GzEncoder;
use flate2::Compression;
use lodos_rootfs::{extract_tarball, extract_tarball_with_progress, ExtractError};
use tar::{Builder, EntryType, Header};
use tempfile::TempDir;
use xz2::write::XzEncoder;

const FIXTURE_MTIME_FILE: u64 = 1_700_000_123;
const FIXTURE_MTIME_DIR: u64 = 1_700_000_456;

fn append_dir<W: Write>(builder: &mut Builder<W>, path: &str, mode: u32, mtime: u64) {
    let mut header = Header::new_gnu();
    header.set_entry_type(EntryType::Directory);
    header.set_size(0);
    header.set_mode(mode);
    header.set_mtime(mtime);
    header.set_cksum();
    builder.append_data(&mut header, path, io::empty()).unwrap();
}

fn append_file<W: Write>(
    builder: &mut Builder<W>,
    path: &str,
    data: &[u8],
    mode: u32,
    mtime: u64,
) {
    let mut header = Header::new_gnu();
    header.set_entry_type(EntryType::Regular);
    header.set_size(data.len() as u64);
    header.set_mode(mode);
    header.set_mtime(mtime);
    header.set_cksum();
    builder.append_data(&mut header, path, data).unwrap();
}

fn append_symlink<W: Write>(builder: &mut Builder<W>, path: &str, target: &str, mtime: u64) {
    let mut header = Header::new_gnu();
    header.set_entry_type(EntryType::Symlink);
    header.set_size(0);
    header.set_mode(0o777);
    header.set_mtime(mtime);
    builder.append_link(&mut header, path, target).unwrap();
}

fn append_hardlink<W: Write>(
    builder: &mut Builder<W>,
    path: &str,
    target: &str,
    mode: u32,
    mtime: u64,
) {
    let mut header = Header::new_gnu();
    header.set_entry_type(EntryType::Link);
    header.set_size(0);
    header.set_mode(mode);
    header.set_mtime(mtime);
    builder.append_link(&mut header, path, target).unwrap();
}

fn append_dev<W: Write>(builder: &mut Builder<W>, path: &str, entry_type: EntryType) {
    let mut header = Header::new_gnu();
    header.set_entry_type(entry_type);
    header.set_size(0);
    header.set_mode(0o666);
    header.set_mtime(FIXTURE_MTIME_FILE);
    if matches!(entry_type, EntryType::Char | EntryType::Block) {
        header.set_device_major(1).unwrap();
        header.set_device_minor(3).unwrap();
    }
    header.set_cksum();
    builder.append_data(&mut header, path, io::empty()).unwrap();
}

/// Write a raw tar entry whose path or linkname bypasses `tar::Header::set_path` validation
/// so we can construct malicious `../evil` and `/abs/evil` tarballs in tests.
fn write_raw_tar_entry<W: Write>(
    out: &mut W,
    raw_name: &str,
    raw_linkname: Option<&str>,
    entry_type: EntryType,
    data: &[u8],
) {
    let mut header = Header::new_ustar();
    header.set_entry_type(entry_type);
    header.set_size(data.len() as u64);
    header.set_mode(0o644);
    header.set_mtime(FIXTURE_MTIME_FILE);

    let ustar = header.as_ustar_mut().unwrap();
    let name_bytes = raw_name.as_bytes();
    assert!(name_bytes.len() < ustar.name.len());
    ustar.name.fill(0);
    ustar.name[..name_bytes.len()].copy_from_slice(name_bytes);

    if let Some(link) = raw_linkname {
        let link_bytes = link.as_bytes();
        assert!(link_bytes.len() < ustar.linkname.len());
        ustar.linkname.fill(0);
        ustar.linkname[..link_bytes.len()].copy_from_slice(link_bytes);
    }

    header.set_cksum();
    out.write_all(header.as_bytes()).unwrap();
    out.write_all(data).unwrap();
    let rem = data.len() % 512;
    if rem != 0 {
        out.write_all(&vec![0u8; 512 - rem]).unwrap();
    }
}

fn finish_raw_tar<W: Write>(out: &mut W) {
    out.write_all(&[0u8; 1024]).unwrap();
}

fn build_standard_fixture<W: Write>(writer: W) -> W {
    let mut builder = Builder::new(writer);
    append_dir(&mut builder, ".", 0o755, FIXTURE_MTIME_DIR);
    append_dir(&mut builder, "./usr", 0o755, FIXTURE_MTIME_DIR);
    append_dir(&mut builder, "./usr/bin", 0o755, FIXTURE_MTIME_DIR);
    append_dir(&mut builder, "./etc", 0o755, FIXTURE_MTIME_DIR);
    append_dir(&mut builder, "./dev", 0o755, FIXTURE_MTIME_DIR);
    append_file(
        &mut builder,
        "./usr/bin/tool",
        b"#!/bin/sh\necho hi\n",
        0o755,
        FIXTURE_MTIME_FILE,
    );
    append_file(
        &mut builder,
        "./etc/conf",
        b"key=value\n",
        0o644,
        FIXTURE_MTIME_FILE,
    );
    append_symlink(&mut builder, "./bin", "usr/bin", FIXTURE_MTIME_FILE);
    append_symlink(
        &mut builder,
        "./etc/tool-abs",
        "/usr/bin/tool",
        FIXTURE_MTIME_FILE,
    );
    append_hardlink(
        &mut builder,
        "./usr/bin/tool-hard",
        "./usr/bin/tool",
        0o755,
        FIXTURE_MTIME_FILE,
    );
    append_dev(&mut builder, "./dev/myfifo", EntryType::Fifo);
    append_dev(&mut builder, "./dev/null", EntryType::Char);
    append_dev(&mut builder, "./dev/sda", EntryType::Block);
    builder.into_inner().unwrap()
}

fn verify_standard_fixture(dst: &Path) {
    let tool = dst.join("usr/bin/tool");
    let conf = dst.join("etc/conf");
    let hard = dst.join("usr/bin/tool-hard");
    let sym_rel = dst.join("bin");
    let sym_abs = dst.join("etc/tool-abs");

    assert_eq!(fs::read(&tool).unwrap(), b"#!/bin/sh\necho hi\n");
    assert_eq!(fs::read(&conf).unwrap(), b"key=value\n");
    assert_eq!(fs::read(&hard).unwrap(), b"#!/bin/sh\necho hi\n");

    let tool_meta = fs::symlink_metadata(&tool).unwrap();
    let conf_meta = fs::symlink_metadata(&conf).unwrap();
    let hard_meta = fs::symlink_metadata(&hard).unwrap();
    let etc_meta = fs::symlink_metadata(dst.join("etc")).unwrap();

    assert_eq!(tool_meta.permissions().mode() & 0o777, 0o755);
    assert_eq!(conf_meta.permissions().mode() & 0o777, 0o644);
    assert_eq!(hard_meta.permissions().mode() & 0o777, 0o755);

    // Hardlink must be materialized as a copy (distinct inode, nlink == 1).
    assert_ne!(tool_meta.ino(), hard_meta.ino());
    assert_eq!(hard_meta.nlink(), 1);

    // Mtimes preserved on files and directories.
    assert_eq!(tool_meta.mtime(), FIXTURE_MTIME_FILE as i64);
    assert_eq!(conf_meta.mtime(), FIXTURE_MTIME_FILE as i64);
    assert_eq!(etc_meta.mtime(), FIXTURE_MTIME_DIR as i64);

    // Symlinks preserved verbatim (relative and absolute targets).
    assert_eq!(fs::read_link(&sym_rel).unwrap(), Path::new("usr/bin"));
    assert_eq!(fs::read_link(&sym_abs).unwrap(), Path::new("/usr/bin/tool"));

    // Device nodes and FIFOs skipped.
    assert!(!dst.join("dev/myfifo").exists());
    assert!(!dst.join("dev/null").exists());
    assert!(!dst.join("dev/sda").exists());
}

#[test]
fn extract_tar_gz_fixture() {
    let tmp = TempDir::new().unwrap();
    let archive_path = tmp.path().join("fixture.tar.gz");
    let dst = tmp.path().join("out");

    let gz = GzEncoder::new(File::create(&archive_path).unwrap(), Compression::fast());
    build_standard_fixture(gz).finish().unwrap();

    let summary = extract_tarball(&archive_path, &dst).unwrap();
    assert_eq!(summary.files, 2);
    assert_eq!(summary.dirs, 5);
    assert_eq!(summary.symlinks, 2);
    assert_eq!(summary.hardlinks, 1);
    assert_eq!(summary.skipped_dev, 3);
    verify_standard_fixture(&dst);
}

#[test]
fn extract_tar_xz_fixture() {
    let tmp = TempDir::new().unwrap();
    let archive_path = tmp.path().join("fixture.tar.xz");
    let dst = tmp.path().join("out");

    let xz = XzEncoder::new(File::create(&archive_path).unwrap(), 1);
    build_standard_fixture(xz).finish().unwrap();

    let summary = extract_tarball(&archive_path, &dst).unwrap();
    assert_eq!(summary.files, 2);
    assert_eq!(summary.dirs, 5);
    assert_eq!(summary.symlinks, 2);
    assert_eq!(summary.hardlinks, 1);
    assert_eq!(summary.skipped_dev, 3);
    verify_standard_fixture(&dst);
}

#[test]
fn extract_tar_zst_fixture() {
    let tmp = TempDir::new().unwrap();
    let archive_path = tmp.path().join("fixture.tar.zst");
    let dst = tmp.path().join("out");

    let zst = zstd::stream::write::Encoder::new(File::create(&archive_path).unwrap(), 1).unwrap();
    build_standard_fixture(zst).finish().unwrap();

    let summary = extract_tarball(&archive_path, &dst).unwrap();
    assert_eq!(summary.files, 2);
    assert_eq!(summary.dirs, 5);
    assert_eq!(summary.symlinks, 2);
    assert_eq!(summary.hardlinks, 1);
    assert_eq!(summary.skipped_dev, 3);
    verify_standard_fixture(&dst);
}

#[test]
fn reject_parent_traversal() {
    let tmp = TempDir::new().unwrap();
    let archive_path = tmp.path().join("evil.tar.gz");
    let dst = tmp.path().join("dest");

    let mut gz = GzEncoder::new(File::create(&archive_path).unwrap(), Compression::fast());
    write_raw_tar_entry(&mut gz, "../evil", None, EntryType::Regular, b"owned");
    finish_raw_tar(&mut gz);
    gz.finish().unwrap();

    let err = extract_tarball(&archive_path, &dst).unwrap_err();
    assert!(
        matches!(err, ExtractError::PathTraversal(_)),
        "expected PathTraversal, got {err:?}"
    );
    assert!(!tmp.path().join("evil").exists());
}

#[test]
fn reject_absolute_escape() {
    let tmp = TempDir::new().unwrap();
    let archive_path = tmp.path().join("abs_evil.tar.gz");
    let dst = tmp.path().join("dest");

    let mut gz = GzEncoder::new(File::create(&archive_path).unwrap(), Compression::fast());
    write_raw_tar_entry(
        &mut gz,
        "/tmp/lodos_rootfs_abs_escape_test",
        None,
        EntryType::Regular,
        b"owned",
    );
    finish_raw_tar(&mut gz);
    gz.finish().unwrap();

    let err = extract_tarball(&archive_path, &dst).unwrap_err();
    assert!(
        matches!(err, ExtractError::PathTraversal(_)),
        "expected PathTraversal, got {err:?}"
    );
    assert!(!Path::new("/tmp/lodos_rootfs_abs_escape_test").exists());
}

#[test]
fn reject_symlink_escape() {
    let tmp = TempDir::new().unwrap();
    let outside = tmp.path().join("outside");
    fs::create_dir(&outside).unwrap();
    let archive_path = tmp.path().join("sym_escape.tar.gz");
    let dst = tmp.path().join("dest");

    let gz = GzEncoder::new(File::create(&archive_path).unwrap(), Compression::fast());
    let mut builder = Builder::new(gz);
    append_symlink(
        &mut builder,
        "escape",
        outside.to_str().unwrap(),
        FIXTURE_MTIME_FILE,
    );
    append_file(
        &mut builder,
        "escape/pwned",
        b"should not escape",
        0o644,
        FIXTURE_MTIME_FILE,
    );
    builder.into_inner().unwrap().finish().unwrap();

    let err = extract_tarball(&archive_path, &dst).unwrap_err();
    assert!(
        matches!(err, ExtractError::PathTraversal(_)),
        "expected PathTraversal, got {err:?}"
    );
    assert!(!outside.join("pwned").exists());
}

#[test]
fn reject_hardlink_traversal() {
    let tmp = TempDir::new().unwrap();
    fs::write(tmp.path().join("secret"), b"top-secret").unwrap();
    let archive_path = tmp.path().join("hardlink_evil.tar.gz");
    let dst = tmp.path().join("dest");

    let mut gz = GzEncoder::new(File::create(&archive_path).unwrap(), Compression::fast());
    write_raw_tar_entry(
        &mut gz,
        "stolen",
        Some("../secret"),
        EntryType::Link,
        b"",
    );
    finish_raw_tar(&mut gz);
    gz.finish().unwrap();

    let err = extract_tarball(&archive_path, &dst).unwrap_err();
    assert!(
        matches!(err, ExtractError::PathTraversal(_)),
        "expected PathTraversal, got {err:?}"
    );
    assert!(!dst.join("stolen").exists());
}

#[test]
fn progress_callback_reports_monotonic_bytes() {
    let tmp = TempDir::new().unwrap();
    let archive_path = tmp.path().join("big.tar.gz");
    let dst = tmp.path().join("out");

    // Incompressible-ish pseudo-random payload so compressed file spans multiple progress steps.
    let mut payload = vec![0u8; 1024 * 1024];
    let mut state: u64 = 0x1234_5678_9abc_def0;
    for chunk in payload.chunks_mut(8) {
        state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
        let bytes = state.to_le_bytes();
        for (d, s) in chunk.iter_mut().zip(bytes.iter()) {
            *d = *s;
        }
    }

    let gz = GzEncoder::new(File::create(&archive_path).unwrap(), Compression::fast());
    let mut builder = Builder::new(gz);
    append_file(&mut builder, "blob.bin", &payload, 0o644, FIXTURE_MTIME_FILE);
    builder.into_inner().unwrap().finish().unwrap();

    let file_len = fs::metadata(&archive_path).unwrap().len();
    let mut events: Vec<(u64, u64)> = Vec::new();
    let summary = extract_tarball_with_progress(&archive_path, &dst, |done, total| {
        events.push((done, total));
    })
    .unwrap();

    assert_eq!(summary.files, 1);
    assert!(events.len() >= 3, "expected multiple progress events, got {events:?}");
    assert_eq!(events.first().copied(), Some((0, file_len)));
    assert_eq!(events.last().copied(), Some((file_len, file_len)));
    for w in events.windows(2) {
        assert!(w[0].0 <= w[1].0, "progress went backwards: {events:?}");
        assert_eq!(w[0].1, file_len);
    }
    assert_eq!(fs::read(dst.join("blob.bin")).unwrap(), payload);
}

#[test]
fn cli_extract_subcommand() {
    let tmp = TempDir::new().unwrap();
    let archive_path = tmp.path().join("cli.tar.gz");
    let dst = tmp.path().join("out");

    let gz = GzEncoder::new(File::create(&archive_path).unwrap(), Compression::fast());
    build_standard_fixture(gz).finish().unwrap();

    let bin = env!("CARGO_BIN_EXE_lodos-rootfs");
    let output = Command::new(bin)
        .args(["extract", archive_path.to_str().unwrap(), dst.to_str().unwrap()])
        .output()
        .unwrap();

    assert!(
        output.status.success(),
        "cli failed: stderr={}",
        String::from_utf8_lossy(&output.stderr)
    );
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(stdout.contains("files=2"), "unexpected stdout: {stdout}");
    assert!(stdout.contains("hardlinks=1"), "unexpected stdout: {stdout}");
    assert!(stdout.contains("skipped_dev=3"), "unexpected stdout: {stdout}");
    verify_standard_fixture(&dst);
}

#[test]
#[ignore]
fn bench_real_trixie_tarball() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let rootfs_dir = manifest_dir.join("../../build/rootfs");

    let candidates: Vec<PathBuf> = if let Ok(p) = std::env::var("LODOS_ROOTFS_TAR") {
        vec![PathBuf::from(p)]
    } else {
        vec![
            rootfs_dir.join("debian-trixie-arm64.tar.gz"),
            rootfs_dir.join("debian-trixie-arm64.tar.xz"),
        ]
    };

    let mut tested = 0;
    for tar_path in candidates {
        if !tar_path.exists() {
            continue;
        }
        let tmp = TempDir::new().unwrap();
        let summary = extract_tarball(&tar_path, tmp.path()).unwrap();
        eprintln!(
            "BENCH {} -> {}",
            tar_path.file_name().unwrap().to_string_lossy(),
            summary
        );
        assert!(summary.files > 1000);
        assert!(summary.dirs > 100);
        assert!(summary.symlinks > 100);
        assert!(tmp.path().join("etc/debian_version").exists());
        tested += 1;
    }
    assert!(tested > 0, "no cached trixie tarball found in {}", rootfs_dir.display());
}
