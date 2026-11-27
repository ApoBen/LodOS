use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

pub struct Logger {
    path: PathBuf,
    file: Option<File>,
    to_stderr: bool,
}

impl Logger {
    pub fn open(path: impl AsRef<Path>, to_stderr: bool) -> io::Result<Self> {
        let path = path.as_ref().to_path_buf();
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent)?;
            }
        }
        let file = OpenOptions::new().create(true).append(true).open(&path)?;
        Ok(Self {
            path,
            file: Some(file),
            to_stderr,
        })
    }

    pub fn noop() -> Self {
        Self {
            path: PathBuf::new(),
            file: None,
            to_stderr: false,
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn info(&mut self, msg: &str) {
        self.write_line("INFO", msg);
    }

    pub fn warn(&mut self, msg: &str) {
        self.write_line("WARN", msg);
    }

    pub fn error(&mut self, msg: &str) {
        self.write_line("ERROR", msg);
    }

    fn write_line(&mut self, level: &str, msg: &str) {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default();
        let line = format!(
            "[{}.{:03}] [{}] {}\n",
            now.as_secs(),
            now.subsec_millis(),
            level,
            msg
        );
        if let Some(ref mut f) = self.file {
            let _ = f.write_all(line.as_bytes());
            let _ = f.flush();
        }
        if self.to_stderr {
            let _ = io::stderr().write_all(line.as_bytes());
        }
    }
}
