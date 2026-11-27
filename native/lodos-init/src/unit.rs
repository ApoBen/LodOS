use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServiceType {
    Simple,
    Forking,
    Oneshot,
    Dbus,
    Notify,
}

impl ServiceType {
    pub fn parse(s: &str) -> Result<Self, String> {
        match s.trim() {
            "simple" | "exec" => Ok(ServiceType::Simple),
            "forking" => Ok(ServiceType::Forking),
            "oneshot" => Ok(ServiceType::Oneshot),
            "dbus" => Ok(ServiceType::Dbus),
            "notify" | "notify-reload" => Ok(ServiceType::Notify),
            other => Err(format!("unsupported Service Type: {}", other)),
        }
    }

    /// Per T031 spec, notify (and dbus without bus monitor) behave like simple.
    pub fn as_str(&self) -> &'static str {
        match self {
            ServiceType::Simple => "simple",
            ServiceType::Forking => "forking",
            ServiceType::Oneshot => "oneshot",
            ServiceType::Dbus => "dbus",
            ServiceType::Notify => "notify",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RestartPolicy {
    No,
    OnFailure,
    Always,
}

impl RestartPolicy {
    pub fn parse(s: &str) -> Result<Self, String> {
        match s.trim() {
            "no" | "" => Ok(RestartPolicy::No),
            "on-failure" | "on-abnormal" | "on-abort" | "on-watchdog" => {
                Ok(RestartPolicy::OnFailure)
            }
            "always" => Ok(RestartPolicy::Always),
            other => Err(format!("unsupported Restart policy: {}", other)),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExecCommand {
    pub program: String,
    pub args: Vec<String>,
    pub ignore_failure: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EnvFileSpec {
    pub path: String,
    pub optional: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkDirSpec {
    pub path: String,
    pub optional: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnitSection {
    pub description: String,
    pub after: Vec<String>,
    pub requires: Vec<String>,
    pub wants: Vec<String>,
}

impl Default for UnitSection {
    fn default() -> Self {
        Self {
            description: String::new(),
            after: Vec::new(),
            requires: Vec::new(),
            wants: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceSection {
    pub service_type: ServiceType,
    pub exec_start_pre: Vec<ExecCommand>,
    pub exec_start: Vec<ExecCommand>,
    pub exec_stop: Vec<ExecCommand>,
    pub environment: Vec<(String, String)>,
    pub environment_files: Vec<EnvFileSpec>,
    pub user: Option<String>,
    pub group: Option<String>,
    pub restart: RestartPolicy,
    pub restart_sec_ms: Option<u64>,
    pub working_directory: Option<WorkDirSpec>,
    pub remain_after_exit: bool,
    pub pid_file: Option<String>,
    pub bus_name: Option<String>,
}

impl Default for ServiceSection {
    fn default() -> Self {
        Self {
            service_type: ServiceType::Simple,
            exec_start_pre: Vec::new(),
            exec_start: Vec::new(),
            exec_stop: Vec::new(),
            environment: Vec::new(),
            environment_files: Vec::new(),
            user: None,
            group: None,
            restart: RestartPolicy::No,
            restart_sec_ms: None,
            working_directory: None,
            remain_after_exit: false,
            pid_file: None,
            bus_name: None,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct InstallSection {
    pub wanted_by: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnitFile {
    pub name: String,
    pub path: PathBuf,
    pub unit: UnitSection,
    pub service: ServiceSection,
    pub install: InstallSection,
}

pub fn normalize_unit_name(name: &str) -> String {
    let trimmed = name.trim();
    if trimmed.contains('.') {
        trimmed.to_string()
    } else {
        format!("{}.service", trimmed)
    }
}

/// Returns the standard systemd search directories under `root`, in priority order.
pub fn unit_search_dirs(root: &Path) -> Vec<PathBuf> {
    let rels = [
        "etc/systemd/system",
        "lib/systemd/system",
        "usr/lib/systemd/system",
    ];
    let mut dirs = Vec::new();
    let mut seen_canon = BTreeSet::new();
    for rel in rels {
        let p = root.join(rel);
        if let Ok(canon) = fs::canonicalize(&p) {
            if seen_canon.insert(canon) {
                dirs.push(p);
            }
        } else {
            dirs.push(p);
        }
    }
    dirs
}

/// Discover all `.service` unit files across search directories.
/// Higher-priority directories override lower-priority ones.
pub fn discover_units(root: &Path) -> BTreeMap<String, UnitFile> {
    let mut units = BTreeMap::new();
    for dir in unit_search_dirs(root) {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        let mut names: Vec<PathBuf> = entries
            .filter_map(|e| e.ok().map(|e| e.path()))
            .collect();
        names.sort();
        for path in names {
            let Some(file_name) = path.file_name().and_then(|s| s.to_str()) else {
                continue;
            };
            if !file_name.ends_with(".service") {
                continue;
            }
            if units.contains_key(file_name) {
                continue;
            }
            // Skip broken symlinks or masked (/dev/null) units
            if let Ok(target) = fs::read_link(&path) {
                if target == Path::new("/dev/null") {
                    continue;
                }
            }
            if !path.is_file() {
                continue;
            }
            if let Ok(content) = fs::read_to_string(&path) {
                if let Ok(unit) = parse_unit_str(file_name, &path, &content) {
                    units.insert(file_name.to_string(), unit);
                }
            }
        }
    }
    units
}

/// Discover enabled unit names from `<target>.wants/` directories under `<root>`.
pub fn discover_enabled_units(root: &Path, target: &str) -> Vec<String> {
    let mut result = BTreeSet::new();
    let wants_dirs = [
        format!("etc/systemd/system/{}.wants", target),
        "etc/systemd/system/default.target.wants".to_string(),
        format!("lib/systemd/system/{}.wants", target),
        format!("usr/lib/systemd/system/{}.wants", target),
    ];
    for rel in wants_dirs {
        let dir = root.join(rel);
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let fname = entry.file_name();
            let Some(s) = fname.to_str() else { continue };
            if s.ends_with(".service") {
                result.insert(s.to_string());
            }
        }
    }
    result.into_iter().collect()
}

pub fn is_unit_enabled(root: &Path, unit_name: &str) -> bool {
    let norm = normalize_unit_name(unit_name);
    let etc_dir = root.join("etc/systemd/system");
    let Ok(entries) = fs::read_dir(&etc_dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir()
            && path
                .file_name()
                .and_then(|n| n.to_str())
                .map(|n| n.ends_with(".wants"))
                .unwrap_or(false)
        {
            let link = path.join(&norm);
            if link.symlink_metadata().is_ok() {
                return true;
            }
        }
    }
    false
}

/// Create `<root>/etc/systemd/system/<target>.wants/<unit>` symlinks per `[Install] WantedBy=`.
pub fn enable_unit(root: &Path, unit: &UnitFile) -> io::Result<Vec<PathBuf>> {
    let mut created = Vec::new();
    for target in &unit.install.wanted_by {
        let wants_dir = root
            .join("etc/systemd/system")
            .join(format!("{}.wants", target));
        fs::create_dir_all(&wants_dir)?;
        let link_path = wants_dir.join(&unit.name);
        if link_path.symlink_metadata().is_ok() {
            fs::remove_file(&link_path)?;
        }
        symlink(&unit.path, &link_path)?;
        created.push(link_path);
    }
    Ok(created)
}

/// Remove `<root>/etc/systemd/system/*.wants/<unit>` symlinks.
pub fn disable_unit(root: &Path, unit_name: &str) -> io::Result<Vec<PathBuf>> {
    let norm = normalize_unit_name(unit_name);
    let mut removed = Vec::new();
    let etc_dir = root.join("etc/systemd/system");
    let Ok(entries) = fs::read_dir(&etc_dir) else {
        return Ok(removed);
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir()
            && path
                .file_name()
                .and_then(|n| n.to_str())
                .map(|n| n.ends_with(".wants"))
                .unwrap_or(false)
        {
            let link = path.join(&norm);
            if link.symlink_metadata().is_ok() {
                fs::remove_file(&link)?;
                removed.push(link);
            }
        }
    }
    Ok(removed)
}

pub fn parse_unit_file(path: &Path) -> Result<UnitFile, String> {
    let name = path
        .file_name()
        .and_then(|s| s.to_str())
        .ok_or_else(|| format!("invalid unit path: {}", path.display()))?;
    let content = fs::read_to_string(path)
        .map_err(|e| format!("failed to read {}: {}", path.display(), e))?;
    parse_unit_str(name, path, &content)
}

pub fn parse_unit_str(name: &str, path: &Path, content: &str) -> Result<UnitFile, String> {
    let logical_lines = join_continuations(content);
    let mut section = String::new();
    let mut unit_sec = UnitSection::default();
    let mut svc_sec = ServiceSection::default();
    let mut install_sec = InstallSection::default();
    let mut explicit_type = false;

    for raw_line in logical_lines {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            section = line[1..line.len() - 1].trim().to_string();
            continue;
        }
        let Some((k, v)) = line.split_once('=') else {
            continue;
        };
        let key = k.trim();
        let val = v.trim();

        match section.as_str() {
            "Unit" => match key {
                "Description" => unit_sec.description = val.to_string(),
                "After" => extend_or_reset_units(&mut unit_sec.after, val),
                "Requires" => extend_or_reset_units(&mut unit_sec.requires, val),
                "Wants" => extend_or_reset_units(&mut unit_sec.wants, val),
                _ => {}
            },
            "Service" => match key {
                "Type" => {
                    svc_sec.service_type = ServiceType::parse(val)?;
                    explicit_type = true;
                }
                "ExecStart" => {
                    if val.is_empty() {
                        svc_sec.exec_start.clear();
                    } else {
                        svc_sec.exec_start.push(parse_exec_command(val)?);
                    }
                }
                "ExecStartPre" => {
                    if val.is_empty() {
                        svc_sec.exec_start_pre.clear();
                    } else {
                        svc_sec.exec_start_pre.push(parse_exec_command(val)?);
                    }
                }
                "ExecStop" => {
                    if val.is_empty() {
                        svc_sec.exec_stop.clear();
                    } else {
                        svc_sec.exec_stop.push(parse_exec_command(val)?);
                    }
                }
                "Environment" => {
                    if val.is_empty() {
                        svc_sec.environment.clear();
                    } else {
                        for item in split_systemd_words(val)? {
                            if let Some((ek, ev)) = item.split_once('=') {
                                svc_sec.environment.push((ek.to_string(), ev.to_string()));
                            }
                        }
                    }
                }
                "EnvironmentFile" => {
                    if val.is_empty() {
                        svc_sec.environment_files.clear();
                    } else if let Some(rest) = val.strip_prefix('-') {
                        svc_sec.environment_files.push(EnvFileSpec {
                            path: rest.trim().to_string(),
                            optional: true,
                        });
                    } else {
                        svc_sec.environment_files.push(EnvFileSpec {
                            path: val.to_string(),
                            optional: false,
                        });
                    }
                }
                "User" => {
                    svc_sec.user = if val.is_empty() {
                        None
                    } else {
                        Some(val.to_string())
                    };
                }
                "Group" => {
                    svc_sec.group = if val.is_empty() {
                        None
                    } else {
                        Some(val.to_string())
                    };
                }
                "Restart" => {
                    svc_sec.restart = RestartPolicy::parse(val)?;
                }
                "RestartSec" => {
                    if let Some(ms) = parse_duration_ms(val) {
                        svc_sec.restart_sec_ms = Some(ms);
                    }
                }
                "WorkingDirectory" => {
                    if val.is_empty() {
                        svc_sec.working_directory = None;
                    } else if let Some(rest) = val.strip_prefix('-') {
                        svc_sec.working_directory = Some(WorkDirSpec {
                            path: rest.trim().to_string(),
                            optional: true,
                        });
                    } else {
                        svc_sec.working_directory = Some(WorkDirSpec {
                            path: val.to_string(),
                            optional: false,
                        });
                    }
                }
                "RemainAfterExit" => {
                    svc_sec.remain_after_exit = parse_bool(val);
                }
                "PIDFile" => {
                    svc_sec.pid_file = if val.is_empty() {
                        None
                    } else {
                        Some(val.to_string())
                    };
                }
                "BusName" => {
                    svc_sec.bus_name = if val.is_empty() {
                        None
                    } else {
                        Some(val.to_string())
                    };
                }
                _ => {}
            },
            "Install" => match key {
                "WantedBy" => extend_or_reset_words(&mut install_sec.wanted_by, val),
                _ => {}
            },
            _ => {}
        }
    }

    if !explicit_type {
        if svc_sec.bus_name.is_some() {
            svc_sec.service_type = ServiceType::Dbus;
        } else if svc_sec.exec_start.is_empty() {
            svc_sec.service_type = ServiceType::Oneshot;
        }
    }

    Ok(UnitFile {
        name: normalize_unit_name(name),
        path: path.to_path_buf(),
        unit: unit_sec,
        service: svc_sec,
        install: install_sec,
    })
}

fn join_continuations(content: &str) -> Vec<String> {
    let mut lines = Vec::new();
    let mut current = String::new();
    let mut continuing = false;

    for raw in content.lines() {
        let trimmed_start = raw.trim_start();
        if !continuing && (trimmed_start.starts_with('#') || trimmed_start.starts_with(';')) {
            continue;
        }
        let line = raw.trim_end_matches('\r');
        if let Some(prefix) = line.strip_suffix('\\') {
            if continuing {
                current.push(' ');
                current.push_str(prefix.trim_start());
            } else {
                current.push_str(prefix);
            }
            continuing = true;
        } else {
            if continuing {
                current.push(' ');
                current.push_str(line.trim_start());
                continuing = false;
            } else {
                current.push_str(line);
            }
            lines.push(std::mem::take(&mut current));
        }
    }
    if !current.is_empty() {
        lines.push(current);
    }
    lines
}

fn extend_or_reset_units(target: &mut Vec<String>, val: &str) {
    if val.is_empty() {
        target.clear();
        return;
    }
    for tok in val.split_whitespace() {
        target.push(normalize_unit_name(tok));
    }
}

fn extend_or_reset_words(target: &mut Vec<String>, val: &str) {
    if val.is_empty() {
        target.clear();
        return;
    }
    for tok in val.split_whitespace() {
        target.push(tok.to_string());
    }
}

fn parse_bool(val: &str) -> bool {
    matches!(
        val.trim().to_ascii_lowercase().as_str(),
        "1" | "yes" | "true" | "on"
    )
}

fn parse_duration_ms(val: &str) -> Option<u64> {
    let s = val.trim();
    if let Some(ms) = s.strip_suffix("ms") {
        ms.trim().parse::<f64>().ok().map(|v| v.max(0.0) as u64)
    } else if let Some(sec) = s.strip_suffix('s') {
        sec.trim()
            .parse::<f64>()
            .ok()
            .map(|v| (v.max(0.0) * 1000.0) as u64)
    } else if let Some(min) = s.strip_suffix("min") {
        min.trim()
            .parse::<f64>()
            .ok()
            .map(|v| (v.max(0.0) * 60_000.0) as u64)
    } else {
        s.parse::<f64>()
            .ok()
            .map(|v| (v.max(0.0) * 1000.0) as u64)
    }
}

pub fn parse_exec_command(raw: &str) -> Result<ExecCommand, String> {
    let mut s = raw.trim();
    let mut ignore_failure = false;

    // Strip systemd command prefixes: -, @, +, !, !!, :
    loop {
        if let Some(rest) = s.strip_prefix('-') {
            ignore_failure = true;
            s = rest;
        } else if let Some(rest) = s.strip_prefix("!!") {
            s = rest;
        } else if let Some(rest) = s.strip_prefix('@')
            .or_else(|| s.strip_prefix('+'))
            .or_else(|| s.strip_prefix('!'))
            .or_else(|| s.strip_prefix(':'))
        {
            s = rest;
        } else {
            break;
        }
    }

    let words = split_systemd_words(s)?;
    if words.is_empty() {
        return Err("empty Exec command".to_string());
    }
    Ok(ExecCommand {
        program: words[0].clone(),
        args: words[1..].to_vec(),
        ignore_failure,
    })
}

pub fn split_systemd_words(input: &str) -> Result<Vec<String>, String> {
    let mut words = Vec::new();
    let mut cur = String::new();
    let mut in_word = false;
    let mut quote: Option<char> = None;
    let mut chars = input.chars().peekable();

    while let Some(c) = chars.next() {
        match quote {
            Some(q) => {
                if c == q {
                    quote = None;
                } else if c == '\\' && q == '"' {
                    if let Some(next) = chars.next() {
                        match next {
                            'n' => cur.push('\n'),
                            't' => cur.push('\t'),
                            '"' | '\\' => cur.push(next),
                            other => {
                                cur.push('\\');
                                cur.push(other);
                            }
                        }
                    }
                } else {
                    cur.push(c);
                }
            }
            None => {
                if c.is_whitespace() {
                    if in_word {
                        words.push(std::mem::take(&mut cur));
                        in_word = false;
                    }
                } else if c == '"' || c == '\'' {
                    quote = Some(c);
                    in_word = true;
                } else if c == '\\' {
                    in_word = true;
                    if let Some(next) = chars.next() {
                        cur.push(next);
                    }
                } else {
                    in_word = true;
                    cur.push(c);
                }
            }
        }
    }
    if quote.is_some() {
        return Err(format!("unterminated quote in: {}", input));
    }
    if in_word {
        words.push(cur);
    }
    Ok(words)
}

/// Parse a systemd `EnvironmentFile` into key-value pairs.
pub fn parse_env_file(path: &Path) -> io::Result<Vec<(String, String)>> {
    let content = fs::read_to_string(path)?;
    let mut pairs = Vec::new();
    for raw in join_continuations(&content) {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        let line = line.strip_prefix("export ").map(str::trim).unwrap_or(line);
        let Some((k, v)) = line.split_once('=') else {
            continue;
        };
        let key = k.trim().to_string();
        let val_trimmed = v.trim();
        let val = if (val_trimmed.starts_with('"') && val_trimmed.ends_with('"') && val_trimmed.len() >= 2)
            || (val_trimmed.starts_with('\'') && val_trimmed.ends_with('\'') && val_trimmed.len() >= 2)
        {
            val_trimmed[1..val_trimmed.len() - 1].to_string()
        } else {
            val_trimmed.to_string()
        };
        if !key.is_empty() {
            pairs.push((key, val));
        }
    }
    Ok(pairs)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_full_service_unit() {
        let content = r#"
# Example unit
[Unit]
Description=Test Daemon
After=dbus.service \
      elogind.service
Requires=dbus.service
Wants=optional.service

[Service]
Type=notify
WorkingDirectory=/var/lib/demo
User=nobody
Environment="GREETING=hello world" FOO=bar
EnvironmentFile=-/etc/default/demo
ExecStartPre=-/bin/true
ExecStart=/usr/bin/demo --flag "two words"
ExecStop=/bin/kill -TERM $MAINPID
Restart=on-failure
RestartSec=250ms

[Install]
WantedBy=multi-user.target
"#;
        let u = parse_unit_str("demo.service", Path::new("/etc/systemd/system/demo.service"), content)
            .expect("parse");
        assert_eq!(u.name, "demo.service");
        assert_eq!(u.unit.description, "Test Daemon");
        assert_eq!(u.unit.after, vec!["dbus.service", "elogind.service"]);
        assert_eq!(u.unit.requires, vec!["dbus.service"]);
        assert_eq!(u.unit.wants, vec!["optional.service"]);
        assert_eq!(u.service.service_type, ServiceType::Notify);
        assert_eq!(
            u.service.working_directory,
            Some(WorkDirSpec {
                path: "/var/lib/demo".into(),
                optional: false
            })
        );
        assert_eq!(u.service.user.as_deref(), Some("nobody"));
        assert_eq!(
            u.service.environment,
            vec![
                ("GREETING".into(), "hello world".into()),
                ("FOO".into(), "bar".into()),
            ]
        );
        assert_eq!(
            u.service.environment_files,
            vec![EnvFileSpec {
                path: "/etc/default/demo".into(),
                optional: true
            }]
        );
        assert_eq!(u.service.exec_start_pre.len(), 1);
        assert!(u.service.exec_start_pre[0].ignore_failure);
        assert_eq!(u.service.exec_start[0].program, "/usr/bin/demo");
        assert_eq!(u.service.exec_start[0].args, vec!["--flag", "two words"]);
        assert_eq!(u.service.exec_stop.len(), 1);
        assert_eq!(u.service.restart, RestartPolicy::OnFailure);
        assert_eq!(u.service.restart_sec_ms, Some(250));
        assert_eq!(u.install.wanted_by, vec!["multi-user.target"]);
    }
}
