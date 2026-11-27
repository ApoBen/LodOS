use std::collections::{BTreeMap, BTreeSet};
use std::ffi::CString;
use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::net::UnixListener;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::graph::{resolve_start_order, resolve_stop_order};
use crate::log::Logger;
use crate::protocol::{read_request, write_response, Request, Response, DEFAULT_SOCKET_PATH};
use crate::unit::{
    disable_unit, discover_enabled_units, discover_units, enable_unit, is_unit_enabled,
    normalize_unit_name, parse_env_file, ExecCommand, InstallSection, RestartPolicy,
    ServiceSection, ServiceType, UnitFile, UnitSection,
};

static SHUTDOWN_SIGNAL: AtomicBool = AtomicBool::new(false);
static CHLD_SIGNAL: AtomicBool = AtomicBool::new(false);

extern "C" fn handle_term_signal(_sig: libc::c_int) {
    SHUTDOWN_SIGNAL.store(true, Ordering::SeqCst);
}

extern "C" fn handle_chld_signal(_sig: libc::c_int) {
    CHLD_SIGNAL.store(true, Ordering::SeqCst);
}

pub fn install_signal_handlers() {
    SHUTDOWN_SIGNAL.store(false, Ordering::SeqCst);
    CHLD_SIGNAL.store(false, Ordering::SeqCst);
    unsafe {
        let mut sa_term: libc::sigaction = std::mem::zeroed();
        sa_term.sa_sigaction = handle_term_signal as *const () as usize;
        libc::sigemptyset(&mut sa_term.sa_mask);
        sa_term.sa_flags = 0;
        libc::sigaction(libc::SIGTERM, &sa_term, std::ptr::null_mut());
        libc::sigaction(libc::SIGINT, &sa_term, std::ptr::null_mut());

        let mut sa_chld: libc::sigaction = std::mem::zeroed();
        sa_chld.sa_sigaction = handle_chld_signal as *const () as usize;
        libc::sigemptyset(&mut sa_chld.sa_mask);
        sa_chld.sa_flags = libc::SA_NOCLDSTOP;
        libc::sigaction(libc::SIGCHLD, &sa_chld, std::ptr::null_mut());
    }
}

pub fn enable_subreaper() -> io::Result<()> {
    let rc = unsafe { libc::prctl(libc::PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) };
    if rc == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BootstrapMode {
    Auto,
    None,
}

#[derive(Debug, Clone)]
pub struct SupervisorConfig {
    pub root: PathBuf,
    pub socket_path: PathBuf,
    pub log_path: PathBuf,
    pub target: String,
    pub bootstrap: BootstrapMode,
    pub base_backoff_ms: u64,
    pub max_backoff_ms: u64,
    pub backoff_reset_ms: u64,
    pub stop_timeout_ms: u64,
    pub oneshot_timeout_ms: u64,
    pub log_stderr: bool,
}

impl SupervisorConfig {
    pub fn default_for_root(root: impl AsRef<Path>) -> Self {
        let root = root.as_ref().to_path_buf();
        let socket_path = if root == Path::new("/") {
            PathBuf::from(DEFAULT_SOCKET_PATH)
        } else {
            root.join("run/lodos-init.sock")
        };
        let log_path = root.join("var/log/lodos-init.log");
        Self {
            root,
            socket_path,
            log_path,
            target: "multi-user.target".to_string(),
            bootstrap: BootstrapMode::Auto,
            base_backoff_ms: 500,
            max_backoff_ms: 30_000,
            backoff_reset_ms: 10_000,
            stop_timeout_ms: 5_000,
            oneshot_timeout_ms: 30_000,
            log_stderr: false,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ActiveState {
    Inactive,
    Activating,
    Active,
    Deactivating,
    Failed,
}

impl ActiveState {
    pub fn as_str(&self) -> &'static str {
        match self {
            ActiveState::Inactive => "inactive",
            ActiveState::Activating => "activating",
            ActiveState::Active => "active",
            ActiveState::Deactivating => "deactivating",
            ActiveState::Failed => "failed",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SubState {
    Dead,
    StartPre,
    Start,
    AutoRestart,
    Running,
    Exited,
    Stop,
    StopSigterm,
    StopSigkill,
    Failed,
}

impl SubState {
    pub fn as_str(&self) -> &'static str {
        match self {
            SubState::Dead => "dead",
            SubState::StartPre => "start-pre",
            SubState::Start => "start",
            SubState::AutoRestart => "auto-restart",
            SubState::Running => "running",
            SubState::Exited => "exited",
            SubState::Stop => "stop",
            SubState::StopSigterm => "stop-sigterm",
            SubState::StopSigkill => "stop-sigkill",
            SubState::Failed => "failed",
        }
    }
}

#[derive(Debug, Clone)]
pub struct UnitRuntime {
    pub active_state: ActiveState,
    pub sub_state: SubState,
    pub main_pid: Option<u32>,
    pub started_at: Option<Instant>,
    pub started_unix: Option<u64>,
    pub last_exit_code: Option<i32>,
    pub last_exit_signal: Option<i32>,
    pub restart_count: u32,
    pub next_restart_at: Option<Instant>,
    pub current_backoff_ms: u64,
    pub manual_stop: bool,
    pub owned_child: bool,
}

impl Default for UnitRuntime {
    fn default() -> Self {
        Self {
            active_state: ActiveState::Inactive,
            sub_state: SubState::Dead,
            main_pid: None,
            started_at: None,
            started_unix: None,
            last_exit_code: None,
            last_exit_signal: None,
            restart_count: 0,
            next_restart_at: None,
            current_backoff_ms: 0,
            manual_stop: false,
            owned_child: false,
        }
    }
}

pub struct Supervisor {
    pub config: SupervisorConfig,
    pub units: BTreeMap<String, UnitFile>,
    pub states: BTreeMap<String, UnitRuntime>,
    pub started_history: Vec<String>,
    pub logger: Logger,
    shutting_down: bool,
    reaped_orphans: u64,
}

impl Supervisor {
    pub fn new(config: SupervisorConfig) -> io::Result<Self> {
        let mut logger = Logger::open(&config.log_path, config.log_stderr)?;
        logger.info(&format!(
            "lodos-init starting (root={}, socket={}, target={})",
            config.root.display(),
            config.socket_path.display(),
            config.target
        ));
        let mut sup = Self {
            config,
            units: BTreeMap::new(),
            states: BTreeMap::new(),
            started_history: Vec::new(),
            logger,
            shutting_down: false,
            reaped_orphans: 0,
        };
        sup.reload_units();
        Ok(sup)
    }

    pub fn reaped_orphans(&self) -> u64 {
        self.reaped_orphans
    }

    /// Discover unit files from search dirs and optionally synthesize built-in
    /// dbus/elogind units when running in `BootstrapMode::Auto`.
    pub fn reload_units(&mut self) {
        let mut discovered = discover_units(&self.config.root);

        if self.config.bootstrap == BootstrapMode::Auto {
            self.inject_builtin_bootstrap_units(&mut discovered);
        }

        // Preserve runtime state for existing units; add defaults for new ones.
        for name in discovered.keys() {
            self.states.entry(name.clone()).or_default();
        }
        // Remove state for vanished inactive units.
        let vanished: Vec<String> = self
            .states
            .iter()
            .filter(|(k, st)| {
                !discovered.contains_key(*k)
                    && matches!(st.active_state, ActiveState::Inactive | ActiveState::Failed)
                    && st.main_pid.is_none()
            })
            .map(|(k, _)| k.clone())
            .collect();
        for k in vanished {
            self.states.remove(&k);
        }

        self.units = discovered;
        self.logger
            .info(&format!("loaded {} unit(s)", self.units.len()));
    }

    fn inject_builtin_bootstrap_units(&mut self, units: &mut BTreeMap<String, UnitFile>) {
        if !units.contains_key("dbus.service") {
            let dbus_bin = self.config.root.join("usr/bin/dbus-daemon");
            if dbus_bin.is_file() {
                let mut svc = ServiceSection::default();
                svc.service_type = ServiceType::Simple;
                svc.exec_start.push(ExecCommand {
                    program: dbus_bin.to_string_lossy().into_owned(),
                    args: vec![
                        "--system".to_string(),
                        "--nofork".to_string(),
                        "--nopidfile".to_string(),
                    ],
                    ignore_failure: false,
                });
                svc.restart = RestartPolicy::OnFailure;
                units.insert(
                    "dbus.service".to_string(),
                    UnitFile {
                        name: "dbus.service".to_string(),
                        path: PathBuf::from("<builtin>/dbus.service"),
                        unit: UnitSection {
                            description: "D-Bus System Message Bus (lodos-init builtin)".into(),
                            ..Default::default()
                        },
                        service: svc,
                        install: InstallSection {
                            wanted_by: vec![self.config.target.clone()],
                        },
                    },
                );
            }
        }

        if !units.contains_key("elogind.service") {
            let candidates = [
                "usr/libexec/elogind/elogind",
                "lib/elogind/elogind",
                "usr/lib/elogind/elogind",
            ];
            for rel in candidates {
                let full = self.config.root.join(rel);
                if full.is_file() {
                    let mut svc = ServiceSection::default();
                    svc.service_type = ServiceType::Simple;
                    svc.exec_start.push(ExecCommand {
                        program: full.to_string_lossy().into_owned(),
                        args: Vec::new(),
                        ignore_failure: false,
                    });
                    svc.restart = RestartPolicy::OnFailure;
                    let mut u = UnitSection {
                        description: "elogind Login Management Service (lodos-init builtin)".into(),
                        ..Default::default()
                    };
                    if units.contains_key("dbus.service") {
                        u.after.push("dbus.service".to_string());
                        u.wants.push("dbus.service".to_string());
                    }
                    units.insert(
                        "elogind.service".to_string(),
                        UnitFile {
                            name: "elogind.service".to_string(),
                            path: PathBuf::from("<builtin>/elogind.service"),
                            unit: u,
                            service: svc,
                            install: InstallSection {
                                wanted_by: vec![self.config.target.clone()],
                            },
                        },
                    );
                    break;
                }
            }
        }
    }

    /// Boot sequence (D3): start dbus, elogind, then enabled target units in topological order.
    pub fn boot(&mut self) -> Result<Vec<String>, String> {
        let mut requested: Vec<String> = Vec::new();
        let enabled = discover_enabled_units(&self.config.root, &self.config.target);
        let enabled_set: BTreeSet<String> = enabled.iter().cloned().collect();

        // Ensure D3 bootstrap order: dbus -> elogind -> enabled units.
        let has_dbus = self.units.contains_key("dbus.service")
            && (self.config.bootstrap == BootstrapMode::Auto || enabled_set.contains("dbus.service"));
        let has_elogind = self.units.contains_key("elogind.service")
            && (self.config.bootstrap == BootstrapMode::Auto
                || enabled_set.contains("elogind.service"));

        if has_dbus {
            let _ = fs::create_dir_all(self.config.root.join("run/dbus"));
            requested.push("dbus.service".to_string());
        }
        if has_elogind {
            requested.push("elogind.service".to_string());
        }
        for u in enabled {
            if !requested.contains(&u) {
                requested.push(u);
            }
        }

        if requested.is_empty() {
            self.logger.info("boot: no enabled units to start");
            return Ok(Vec::new());
        }

        // Start dbus first, then elogind, then the rest via topo sort.
        let mut started = Vec::new();
        let mut core = Vec::new();
        if has_dbus {
            core.push("dbus.service".to_string());
        }
        if has_elogind {
            core.push("elogind.service".to_string());
        }
        for c in &core {
            if let Err(e) = self.start_with_deps(&[c.clone()]) {
                self.logger
                    .error(&format!("boot: failed to start core unit {}: {}", c, e));
            } else {
                started.push(c.clone());
            }
        }

        let rest: Vec<String> = requested
            .into_iter()
            .filter(|u| !core.contains(u))
            .collect();
        if !rest.is_empty() {
            match self.start_with_deps(&rest) {
                Ok(order) => {
                    for u in order {
                        if !started.contains(&u) {
                            started.push(u);
                        }
                    }
                }
                Err(e) => {
                    self.logger.error(&format!("boot error: {}", e));
                    return Err(e);
                }
            }
        }

        Ok(started)
    }

    pub fn start_with_deps(&mut self, requested: &[String]) -> Result<Vec<String>, String> {
        let mut normalized = Vec::new();
        for r in requested {
            let n = normalize_unit_name(r);
            if !self.units.contains_key(&n) {
                return Err(format!("Unit {} not found.", n));
            }
            normalized.push(n);
        }

        let order = resolve_start_order(&normalized, &self.units).map_err(|e| e.to_string())?;
        let mut failed_units: BTreeSet<String> = BTreeSet::new();
        let mut started = Vec::new();

        for name in &order {
            if let Some(st) = self.states.get(name) {
                if st.active_state == ActiveState::Active {
                    continue;
                }
            }
            // Check if any required dependency failed in this transaction.
            if let Some(unit) = self.units.get(name) {
                let mut req_failed = None;
                for req in &unit.unit.requires {
                    if failed_units.contains(req) {
                        req_failed = Some(req.clone());
                        break;
                    }
                }
                if let Some(req) = req_failed {
                    let msg = format!(
                        "skipping {}: required dependency {} failed",
                        name, req
                    );
                    self.logger.error(&msg);
                    failed_units.insert(name.clone());
                    continue;
                }
            }

            match self.start_single_unit(name) {
                Ok(()) => {
                    started.push(name.clone());
                }
                Err(e) => {
                    self.logger.error(&format!("failed to start {}: {}", name, e));
                    failed_units.insert(name.clone());
                    if normalized.contains(name) {
                        return Err(format!("Job for {} failed: {}", name, e));
                    }
                }
            }
        }

        for r in &normalized {
            if failed_units.contains(r) {
                return Err(format!("Job for {} failed.", r));
            }
        }

        Ok(started)
    }

    fn start_single_unit(&mut self, name: &str) -> Result<(), String> {
        let unit = self
            .units
            .get(name)
            .cloned()
            .ok_or_else(|| format!("Unit {} not found.", name))?;

        self.logger.info(&format!(
            "starting {} (type={})",
            unit.name,
            unit.service.service_type.as_str()
        ));

        let env_pairs = self.build_env(&unit)?;
        let workdir = self.resolve_workdir(&unit)?;
        let (uid, gid) = self.resolve_credentials(&unit)?;

        {
            let st = self.states.entry(unit.name.clone()).or_default();
            st.manual_stop = false;
            st.next_restart_at = None;
            st.active_state = ActiveState::Activating;
            st.sub_state = SubState::StartPre;
        }

        // Run ExecStartPre commands sequentially.
        for pre in &unit.service.exec_start_pre {
            if let Err(e) = self.run_sync_command(pre, &env_pairs, workdir.as_deref(), uid, gid, None)
            {
                if !pre.ignore_failure {
                    self.mark_failed(&unit.name, Some(1), None);
                    return Err(format!("ExecStartPre failed: {}", e));
                }
                self.logger
                    .warn(&format!("{}: ignoring ExecStartPre error: {}", unit.name, e));
            }
        }

        if let Some(st) = self.states.get_mut(&unit.name) {
            st.sub_state = SubState::Start;
        }

        match unit.service.service_type {
            ServiceType::Oneshot => {
                for cmd in &unit.service.exec_start {
                    if let Err(e) =
                        self.run_sync_command(cmd, &env_pairs, workdir.as_deref(), uid, gid, None)
                    {
                        if !cmd.ignore_failure {
                            self.mark_failed(&unit.name, Some(1), None);
                            return Err(format!("ExecStart failed: {}", e));
                        }
                    }
                }
                let st = self.states.entry(unit.name.clone()).or_default();
                st.main_pid = None;
                st.owned_child = false;
                st.last_exit_code = Some(0);
                st.last_exit_signal = None;
                st.started_at = Some(Instant::now());
                st.started_unix = Some(unix_now());
                if unit.service.remain_after_exit {
                    st.active_state = ActiveState::Active;
                    st.sub_state = SubState::Exited;
                } else {
                    st.active_state = ActiveState::Inactive;
                    st.sub_state = SubState::Dead;
                }
                self.record_started_history(&unit.name);
                self.logger.info(&format!("finished oneshot {}", unit.name));
                Ok(())
            }
            ServiceType::Forking => {
                let cmd = unit
                    .service
                    .exec_start
                    .first()
                    .ok_or_else(|| format!("{} has no ExecStart", unit.name))?;
                let child_pid =
                    self.spawn_command(cmd, &env_pairs, workdir.as_deref(), uid, gid, None)?;
                let (code, sig) = self.wait_for_pid_sync(child_pid, self.config.oneshot_timeout_ms)?;
                if code != Some(0) {
                    if !cmd.ignore_failure {
                        self.mark_failed(&unit.name, code, sig);
                        return Err(format!(
                            "forking ExecStart exited with code={:?} sig={:?}",
                            code, sig
                        ));
                    }
                }
                let tracked_pid = unit
                    .service
                    .pid_file
                    .as_deref()
                    .and_then(|p| self.read_pid_file(p));
                let st = self.states.entry(unit.name.clone()).or_default();
                st.active_state = ActiveState::Active;
                st.sub_state = SubState::Running;
                st.main_pid = tracked_pid;
                st.owned_child = false;
                st.started_at = Some(Instant::now());
                st.started_unix = Some(unix_now());
                st.last_exit_code = None;
                st.last_exit_signal = None;
                self.record_started_history(&unit.name);
                self.logger.info(&format!(
                    "started forking {} (pid={:?})",
                    unit.name, tracked_pid
                ));
                Ok(())
            }
            ServiceType::Simple | ServiceType::Dbus | ServiceType::Notify => {
                let cmd = unit
                    .service
                    .exec_start
                    .first()
                    .ok_or_else(|| format!("{} has no ExecStart", unit.name))?;
                let pid = self.spawn_command(cmd, &env_pairs, workdir.as_deref(), uid, gid, None)?;
                let st = self.states.entry(unit.name.clone()).or_default();
                st.active_state = ActiveState::Active;
                st.sub_state = SubState::Running;
                st.main_pid = Some(pid);
                st.owned_child = true;
                st.started_at = Some(Instant::now());
                st.started_unix = Some(unix_now());
                st.last_exit_code = None;
                st.last_exit_signal = None;
                self.record_started_history(&unit.name);
                self.logger
                    .info(&format!("started {} (pid={})", unit.name, pid));
                Ok(())
            }
        }
    }

    pub fn stop_unit(&mut self, name: &str) -> Result<(), String> {
        let norm = normalize_unit_name(name);
        if !self.units.contains_key(&norm) && !self.states.contains_key(&norm) {
            return Err(format!("Unit {} not found.", norm));
        }

        let unit_opt = self.units.get(&norm).cloned();
        let main_pid = {
            let st = self.states.entry(norm.clone()).or_default();
            st.manual_stop = true;
            st.next_restart_at = None;
            if st.active_state == ActiveState::Inactive && st.main_pid.is_none() {
                return Ok(());
            }
            st.active_state = ActiveState::Deactivating;
            st.sub_state = SubState::Stop;
            st.main_pid
        };

        self.logger
            .info(&format!("stopping {} (pid={:?})", norm, main_pid));

        // Run ExecStop if defined.
        if let Some(ref unit) = unit_opt {
            if !unit.service.exec_stop.is_empty() {
                let env_pairs = self.build_env(unit).unwrap_or_default();
                let workdir = self.resolve_workdir(unit).ok().flatten();
                let (uid, gid) = self.resolve_credentials(unit).unwrap_or((None, None));
                for stop_cmd in &unit.service.exec_stop {
                    if let Err(e) = self.run_sync_command(
                        stop_cmd,
                        &env_pairs,
                        workdir.as_deref(),
                        uid,
                        gid,
                        main_pid,
                    ) {
                        self.logger
                            .warn(&format!("{}: ExecStop error: {}", norm, e));
                    }
                }
            }
        }

        // Reap in case ExecStop already terminated the process.
        self.reap_zombies();
        let remaining_pid = self.states.get(&norm).and_then(|s| s.main_pid);

        // Send SIGTERM then wait, escalating to SIGKILL on timeout.
        if let Some(pid) = remaining_pid {
            if pid_alive(pid) {
                if let Some(st) = self.states.get_mut(&norm) {
                    st.sub_state = SubState::StopSigterm;
                }
                send_signal(pid, libc::SIGTERM);
                let deadline = Instant::now() + Duration::from_millis(self.config.stop_timeout_ms);
                while Instant::now() < deadline {
                    self.reap_zombies();
                    let still_running = self
                        .states
                        .get(&norm)
                        .and_then(|s| s.main_pid)
                        .map(pid_alive)
                        .unwrap_or(false);
                    if !still_running {
                        break;
                    }
                    thread::sleep(Duration::from_millis(15));
                }
                let still_running = self
                    .states
                    .get(&norm)
                    .and_then(|s| s.main_pid)
                    .map(pid_alive)
                    .unwrap_or(false);
                if still_running {
                    if let Some(st) = self.states.get_mut(&norm) {
                        st.sub_state = SubState::StopSigkill;
                    }
                    self.logger
                        .warn(&format!("{}: stop timeout, sending SIGKILL to {}", norm, pid));
                    send_signal(pid, libc::SIGKILL);
                    let kill_deadline = Instant::now() + Duration::from_secs(2);
                    while Instant::now() < kill_deadline {
                        self.reap_zombies();
                        if !pid_alive(pid) {
                            break;
                        }
                        thread::sleep(Duration::from_millis(10));
                    }
                }
            }
        }

        self.reap_zombies();
        let st = self.states.entry(norm.clone()).or_default();
        st.active_state = ActiveState::Inactive;
        st.sub_state = SubState::Dead;
        st.main_pid = None;
        st.owned_child = false;
        st.next_restart_at = None;
        self.logger.info(&format!("stopped {}", norm));
        Ok(())
    }

    pub fn shutdown_all(&mut self) -> Vec<String> {
        self.shutting_down = true;
        let running: Vec<String> = self
            .states
            .iter()
            .filter(|(_, st)| {
                matches!(
                    st.active_state,
                    ActiveState::Active | ActiveState::Activating | ActiveState::Deactivating
                ) || st.main_pid.is_some()
            })
            .map(|(k, _)| k.clone())
            .collect();

        let stop_order = resolve_stop_order(&running, &self.units, &self.started_history);
        self.logger.info(&format!(
            "shutting down {} unit(s) in reverse dependency order: {}",
            stop_order.len(),
            stop_order.join(", ")
        ));

        for name in &stop_order {
            let _ = self.stop_unit(name);
        }
        self.reap_zombies();
        stop_order
    }

    /// Reap all exited child processes (including reparented orphans/zombies).
    pub fn reap_zombies(&mut self) -> usize {
        let mut reaped = 0usize;
        loop {
            let mut status: libc::c_int = 0;
            let pid = unsafe { libc::waitpid(-1, &mut status, libc::WNOHANG) };
            if pid <= 0 {
                break;
            }
            reaped += 1;
            let (exit_code, exit_sig) = decode_wait_status(status);
            self.on_process_exited(pid as u32, exit_code, exit_sig);
        }
        // Also check non-direct forking PIDs (e.g. if subreaper wasn't active).
        let dead_forking: Vec<(String, u32)> = self
            .states
            .iter()
            .filter_map(|(name, st)| {
                if !st.owned_child && st.active_state == ActiveState::Active {
                    if let Some(pid) = st.main_pid {
                        if !pid_alive(pid) {
                            return Some((name.clone(), pid));
                        }
                    }
                }
                None
            })
            .collect();
        for (name, pid) in dead_forking {
            self.on_unit_exited(&name, pid, Some(0), None);
        }
        reaped
    }

    fn on_process_exited(&mut self, pid: u32, exit_code: Option<i32>, exit_sig: Option<i32>) {
        let owner = self
            .states
            .iter()
            .find(|(_, st)| st.main_pid == Some(pid))
            .map(|(k, _)| k.clone());

        if let Some(name) = owner {
            self.on_unit_exited(&name, pid, exit_code, exit_sig);
        } else {
            self.reaped_orphans += 1;
            self.logger.info(&format!(
                "reaped zombie/orphan pid={} code={:?} sig={:?}",
                pid, exit_code, exit_sig
            ));
        }
    }

    fn on_unit_exited(
        &mut self,
        name: &str,
        pid: u32,
        exit_code: Option<i32>,
        exit_sig: Option<i32>,
    ) {
        let restart_policy = self
            .units
            .get(name)
            .map(|u| u.service.restart)
            .unwrap_or(RestartPolicy::No);
        let unit_restart_sec = self.units.get(name).and_then(|u| u.service.restart_sec_ms);

        let is_failure = exit_sig.is_some() || exit_code != Some(0);
        let st = self.states.entry(name.to_string()).or_default();
        st.main_pid = None;
        st.owned_child = false;
        st.last_exit_code = exit_code;
        st.last_exit_signal = exit_sig;

        let manual_stop = st.manual_stop;
        let uptime_ms = st
            .started_at
            .map(|t| t.elapsed().as_millis() as u64)
            .unwrap_or(0);

        self.logger.info(&format!(
            "unit {} (pid={}) exited: code={:?} sig={:?} uptime={}ms",
            name, pid, exit_code, exit_sig, uptime_ms
        ));

        if manual_stop || self.shutting_down {
            st.active_state = ActiveState::Inactive;
            st.sub_state = SubState::Dead;
            st.next_restart_at = None;
            return;
        }

        let should_restart = match restart_policy {
            RestartPolicy::No => false,
            RestartPolicy::Always => true,
            RestartPolicy::OnFailure => is_failure,
        };

        if should_restart {
            let base = unit_restart_sec.unwrap_or(self.config.base_backoff_ms);
            if st.current_backoff_ms == 0 || uptime_ms >= self.config.backoff_reset_ms {
                st.current_backoff_ms = base;
            } else {
                let next = st.current_backoff_ms.max(25).saturating_mul(2);
                st.current_backoff_ms = next.min(self.config.max_backoff_ms);
            }
            let delay = st.current_backoff_ms;
            st.restart_count += 1;
            st.active_state = ActiveState::Activating;
            st.sub_state = SubState::AutoRestart;
            st.next_restart_at = Some(Instant::now() + Duration::from_millis(delay));
            self.logger.warn(&format!(
                "scheduling restart #{} for {} in {}ms",
                st.restart_count, name, delay
            ));
        } else if is_failure {
            st.active_state = ActiveState::Failed;
            st.sub_state = SubState::Failed;
        } else {
            st.active_state = ActiveState::Inactive;
            st.sub_state = SubState::Dead;
        }
    }

    pub fn process_due_restarts(&mut self) {
        if self.shutting_down {
            return;
        }
        let now = Instant::now();
        let due: Vec<String> = self
            .states
            .iter()
            .filter_map(|(name, st)| {
                if !st.manual_stop && st.active_state == ActiveState::Activating {
                    if let Some(at) = st.next_restart_at {
                        if now >= at {
                            return Some(name.clone());
                        }
                    }
                }
                None
            })
            .collect();

        for name in due {
            if let Some(st) = self.states.get_mut(&name) {
                st.next_restart_at = None;
            }
            if let Err(e) = self.start_single_unit(&name) {
                self.logger
                    .error(&format!("restart of {} failed: {}", name, e));
            }
        }
    }

    pub fn handle_request(&mut self, req: Request) -> Response {
        match req {
            Request::Ping => Response::ok("pong\n"),
            Request::Start(units) => match self.start_with_deps(&units) {
                Ok(_) => Response::ok(""),
                Err(e) => {
                    let code = if e.contains("not found") { 5 } else { 1 };
                    Response::err(code, format!("{}\n", e))
                }
            },
            Request::Stop(units) => {
                for u in units {
                    if let Err(e) = self.stop_unit(&u) {
                        let code = if e.contains("not found") { 5 } else { 1 };
                        return Response::err(code, format!("{}\n", e));
                    }
                }
                Response::ok("")
            }
            Request::Restart(units) => {
                for u in &units {
                    if !self.units.contains_key(&normalize_unit_name(u)) {
                        return Response::err(5, format!("Unit {} not found.\n", normalize_unit_name(u)));
                    }
                }
                for u in &units {
                    if let Err(e) = self.stop_unit(u) {
                        return Response::err(1, format!("{}\n", e));
                    }
                }
                match self.start_with_deps(&units) {
                    Ok(_) => Response::ok(""),
                    Err(e) => Response::err(1, format!("{}\n", e)),
                }
            }
            Request::IsActive(unit) => {
                let norm = normalize_unit_name(&unit);
                if let Some(st) = self.states.get(&norm) {
                    let state_str = st.active_state.as_str();
                    let code = if st.active_state == ActiveState::Active {
                        0
                    } else {
                        3
                    };
                    Response::with_code(code, format!("{}\n", state_str), "")
                } else {
                    Response::with_code(3, "inactive\n", "")
                }
            }
            Request::Status(unit) => self.format_status(&unit),
            Request::Enable { units, now } => {
                let mut out = String::new();
                for u in &units {
                    let norm = normalize_unit_name(u);
                    let Some(unit_file) = self.units.get(&norm).cloned() else {
                        return Response::err(1, format!("Unit file {} does not exist.\n", norm));
                    };
                    match enable_unit(&self.config.root, &unit_file) {
                        Ok(links) => {
                            for l in links {
                                out.push_str(&format!(
                                    "Created symlink {} -> {}.\n",
                                    l.display(),
                                    unit_file.path.display()
                                ));
                            }
                        }
                        Err(e) => {
                            return Response::err(1, format!("Failed to enable {}: {}\n", norm, e));
                        }
                    }
                }
                if now {
                    if let Err(e) = self.start_with_deps(&units) {
                        return Response::with_code(1, out, format!("{}\n", e));
                    }
                }
                Response::ok(out)
            }
            Request::Disable { units, now } => {
                let mut out = String::new();
                for u in &units {
                    let norm = normalize_unit_name(u);
                    match disable_unit(&self.config.root, &norm) {
                        Ok(removed) => {
                            for r in removed {
                                out.push_str(&format!("Removed \"{}\".\n", r.display()));
                            }
                        }
                        Err(e) => {
                            return Response::err(1, format!("Failed to disable {}: {}\n", norm, e));
                        }
                    }
                }
                if now {
                    for u in &units {
                        let _ = self.stop_unit(u);
                    }
                }
                Response::ok(out)
            }
            Request::ListUnits { all } => self.format_list_units(all),
            Request::DaemonReload => {
                self.reload_units();
                Response::ok("")
            }
        }
    }

    fn format_status(&self, unit: &str) -> Response {
        let norm = normalize_unit_name(unit);
        let Some(uf) = self.units.get(&norm) else {
            return Response::err(4, format!("Unit {} could not be found.\n", norm));
        };
        let st = self.states.get(&norm).cloned().unwrap_or_default();
        let enabled_str = if is_unit_enabled(&self.config.root, &norm) {
            "enabled"
        } else {
            "disabled"
        };
        let desc = if uf.unit.description.is_empty() {
            norm.as_str()
        } else {
            uf.unit.description.as_str()
        };

        let mut out = String::new();
        out.push_str(&format!("* {} - {}\n", norm, desc));
        out.push_str(&format!(
            "     Loaded: loaded ({}; {})\n",
            uf.path.display(),
            enabled_str
        ));
        out.push_str(&format!(
            "     Active: {} ({})",
            st.active_state.as_str(),
            st.sub_state.as_str()
        ));
        if let Some(ts) = st.started_unix {
            out.push_str(&format!(" since unix:{}", ts));
        }
        out.push('\n');
        if let Some(pid) = st.main_pid {
            out.push_str(&format!("   Main PID: {}\n", pid));
        }
        if st.restart_count > 0 {
            out.push_str(&format!("   Restarts: {}\n", st.restart_count));
        }
        if let Some(code) = st.last_exit_code {
            out.push_str(&format!("  Last Exit: code={}\n", code));
        } else if let Some(sig) = st.last_exit_signal {
            out.push_str(&format!("  Last Exit: signal={}\n", sig));
        }

        let exit_code = if st.active_state == ActiveState::Active {
            0
        } else {
            3
        };
        Response::with_code(exit_code, out, "")
    }

    fn format_list_units(&self, all: bool) -> Response {
        let mut out = String::new();
        out.push_str(&format!(
            "{:<30} {:<8} {:<12} {:<12} {}\n",
            "UNIT", "LOAD", "ACTIVE", "SUB", "DESCRIPTION"
        ));
        let mut count = 0usize;
        for (name, uf) in &self.units {
            let st = self.states.get(name).cloned().unwrap_or_default();
            if !all && st.active_state == ActiveState::Inactive {
                continue;
            }
            let desc = if uf.unit.description.is_empty() {
                name.as_str()
            } else {
                uf.unit.description.as_str()
            };
            out.push_str(&format!(
                "{:<30} {:<8} {:<12} {:<12} {}\n",
                name,
                "loaded",
                st.active_state.as_str(),
                st.sub_state.as_str(),
                desc
            ));
            count += 1;
        }
        out.push_str(&format!("\n{} loaded units listed.\n", count));
        Response::ok(out)
    }

    /// Run the main supervisor loop: bind socket, boot units, handle IPC, reap zombies,
    /// restart crashed services with backoff, and stop all in reverse order on SIGTERM.
    pub fn run(&mut self, external_stop: Option<Arc<AtomicBool>>) -> io::Result<()> {
        if let Some(parent) = self.config.socket_path.parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent)?;
            }
        }
        if self.config.socket_path.exists() {
            let _ = fs::remove_file(&self.config.socket_path);
        }
        let listener = UnixListener::bind(&self.config.socket_path)?;
        listener.set_nonblocking(true)?;
        self.logger.info(&format!(
            "listening on {}",
            self.config.socket_path.display()
        ));

        if let Err(e) = self.boot() {
            self.logger.error(&format!("boot completed with error: {}", e));
        }

        loop {
            if SHUTDOWN_SIGNAL.load(Ordering::SeqCst)
                || external_stop
                    .as_ref()
                    .map(|f| f.load(Ordering::SeqCst))
                    .unwrap_or(false)
            {
                break;
            }

            // Reap any exited children / orphans.
            CHLD_SIGNAL.swap(false, Ordering::SeqCst);
            self.reap_zombies();

            // Execute due restarts.
            self.process_due_restarts();

            // Accept pending IPC connections.
            loop {
                match listener.accept() {
                    Ok((mut stream, _)) => {
                        let _ = stream.set_nonblocking(false);
                        let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
                        let _ = stream.set_write_timeout(Some(Duration::from_secs(5)));
                        let resp = match read_request(&stream) {
                            Ok(Ok(req)) => self.handle_request(req),
                            Ok(Err(e)) => Response::err(1, format!("bad request: {}\n", e)),
                            Err(e) => Response::err(1, format!("io error: {}\n", e)),
                        };
                        let _ = write_response(&mut stream, &resp);
                    }
                    Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => break,
                    Err(ref e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    Err(e) => {
                        self.logger.warn(&format!("accept error: {}", e));
                        break;
                    }
                }
            }

            // Reap again if a signal arrived while handling IPC.
            if CHLD_SIGNAL.swap(false, Ordering::SeqCst) {
                self.reap_zombies();
                continue;
            }

            thread::sleep(Duration::from_millis(20));
        }

        self.shutdown_all();
        let _ = fs::remove_file(&self.config.socket_path);
        self.logger.info("lodos-init exited cleanly");
        Ok(())
    }

    fn record_started_history(&mut self, name: &str) {
        self.started_history.retain(|u| u != name);
        self.started_history.push(name.to_string());
    }

    fn mark_failed(&mut self, name: &str, code: Option<i32>, sig: Option<i32>) {
        let st = self.states.entry(name.to_string()).or_default();
        st.active_state = ActiveState::Failed;
        st.sub_state = SubState::Failed;
        st.main_pid = None;
        st.owned_child = false;
        st.last_exit_code = code;
        st.last_exit_signal = sig;
    }

    fn build_env(&self, unit: &UnitFile) -> Result<Vec<(String, String)>, String> {
        let mut map: BTreeMap<String, String> = std::env::vars().collect();

        for ef in &unit.service.environment_files {
            let path = self.resolve_root_path(&ef.path);
            match parse_env_file(&path) {
                Ok(pairs) => {
                    for (k, v) in pairs {
                        map.insert(k, v);
                    }
                }
                Err(e) => {
                    if !ef.optional {
                        return Err(format!(
                            "failed to load EnvironmentFile {}: {}",
                            path.display(),
                            e
                        ));
                    }
                }
            }
        }

        for (k, v) in &unit.service.environment {
            map.insert(k.clone(), v.clone());
        }

        Ok(map.into_iter().collect())
    }

    fn resolve_root_path(&self, raw: &str) -> PathBuf {
        let p = Path::new(raw);
        if self.config.root != Path::new("/") {
            if let Ok(stripped) = p.strip_prefix("/") {
                let under_root = self.config.root.join(stripped);
                if under_root.exists() {
                    return under_root;
                }
            }
            if p.exists() {
                return p.to_path_buf();
            }
            if let Ok(stripped) = p.strip_prefix("/") {
                return self.config.root.join(stripped);
            }
        }
        p.to_path_buf()
    }

    fn resolve_workdir(&self, unit: &UnitFile) -> Result<Option<PathBuf>, String> {
        let Some(ref wd) = unit.service.working_directory else {
            return Ok(None);
        };
        let resolved = self.resolve_root_path(&wd.path);
        if resolved.is_dir() {
            Ok(Some(resolved))
        } else if wd.optional {
            Ok(None)
        } else {
            Err(format!("WorkingDirectory does not exist: {}", resolved.display()))
        }
    }

    fn resolve_credentials(
        &self,
        unit: &UnitFile,
    ) -> Result<(Option<libc::uid_t>, Option<libc::gid_t>), String> {
        let mut uid_out = None;
        let mut gid_out = None;

        if let Some(ref user) = unit.service.user {
            if let Ok(num) = user.parse::<libc::uid_t>() {
                uid_out = Some(num);
            } else {
                let (u, g) = lookup_user(user)?;
                uid_out = Some(u);
                gid_out = Some(g);
            }
        }
        if let Some(ref group) = unit.service.group {
            if let Ok(num) = group.parse::<libc::gid_t>() {
                gid_out = Some(num);
            } else {
                gid_out = Some(lookup_group(group)?);
            }
        }
        Ok((uid_out, gid_out))
    }

    fn read_pid_file(&self, raw: &str) -> Option<u32> {
        let path = self.resolve_root_path(raw);
        let content = fs::read_to_string(path).ok()?;
        content.trim().parse::<u32>().ok()
    }

    fn spawn_command(
        &self,
        exec: &ExecCommand,
        env_pairs: &[(String, String)],
        workdir: Option<&Path>,
        uid: Option<libc::uid_t>,
        gid: Option<libc::gid_t>,
        main_pid: Option<u32>,
    ) -> Result<u32, String> {
        let args: Vec<String> = exec
            .args
            .iter()
            .map(|a| {
                if let Some(pid) = main_pid {
                    a.replace("$MAINPID", &pid.to_string())
                } else {
                    a.clone()
                }
            })
            .collect();

        let mut cmd = Command::new(&exec.program);
        cmd.args(&args);
        cmd.env_clear();
        for (k, v) in env_pairs {
            cmd.env(k, v);
        }
        if let Some(pid) = main_pid {
            cmd.env("MAINPID", pid.to_string());
        }
        if let Some(dir) = workdir {
            cmd.current_dir(dir);
        }

        cmd.stdin(Stdio::null());
        if let Ok(log_out) = OpenOptions::new()
            .create(true)
            .append(true)
            .open(self.logger.path())
        {
            if let Ok(log_err) = log_out.try_clone() {
                cmd.stdout(Stdio::from(log_out));
                cmd.stderr(Stdio::from(log_err));
            } else {
                cmd.stdout(Stdio::null());
                cmd.stderr(Stdio::null());
            }
        } else {
            cmd.stdout(Stdio::null());
            cmd.stderr(Stdio::null());
        }

        let cur_euid = unsafe { libc::geteuid() };
        let cur_egid = unsafe { libc::getegid() };

        unsafe {
            cmd.pre_exec(move || {
                let _ = libc::setpgid(0, 0);
                if let Some(target_gid) = gid {
                    if target_gid != cur_egid && libc::setgid(target_gid) != 0 {
                        return Err(io::Error::last_os_error());
                    }
                }
                if let Some(target_uid) = uid {
                    if target_uid != cur_euid && libc::setuid(target_uid) != 0 {
                        return Err(io::Error::last_os_error());
                    }
                }
                Ok(())
            });
        }

        let child = cmd
            .spawn()
            .map_err(|e| format!("failed to spawn {}: {}", exec.program, e))?;
        Ok(child.id())
    }

    fn run_sync_command(
        &mut self,
        exec: &ExecCommand,
        env_pairs: &[(String, String)],
        workdir: Option<&Path>,
        uid: Option<libc::uid_t>,
        gid: Option<libc::gid_t>,
        main_pid: Option<u32>,
    ) -> Result<(), String> {
        let pid = self.spawn_command(exec, env_pairs, workdir, uid, gid, main_pid)?;
        let (code, sig) = self.wait_for_pid_sync(pid, self.config.oneshot_timeout_ms)?;
        if code == Some(0) {
            Ok(())
        } else {
            Err(format!(
                "{} exited with code={:?} sig={:?}",
                exec.program, code, sig
            ))
        }
    }

    fn wait_for_pid_sync(
        &self,
        pid: u32,
        timeout_ms: u64,
    ) -> Result<(Option<i32>, Option<i32>), String> {
        let deadline = Instant::now() + Duration::from_millis(timeout_ms);
        loop {
            let mut status: libc::c_int = 0;
            let rc = unsafe { libc::waitpid(pid as libc::pid_t, &mut status, libc::WNOHANG) };
            if rc > 0 {
                return Ok(decode_wait_status(status));
            }
            if rc < 0 {
                let err = io::Error::last_os_error();
                if err.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(format!("waitpid({}) failed: {}", pid, err));
            }
            if Instant::now() >= deadline {
                send_signal(pid, libc::SIGKILL);
                let _ = unsafe { libc::waitpid(pid as libc::pid_t, &mut status, 0) };
                return Err(format!("command pid={} timed out after {}ms", pid, timeout_ms));
            }
            thread::sleep(Duration::from_millis(10));
        }
    }
}

fn decode_wait_status(status: libc::c_int) -> (Option<i32>, Option<i32>) {
    if libc::WIFEXITED(status) {
        (Some(libc::WEXITSTATUS(status)), None)
    } else if libc::WIFSIGNALED(status) {
        (None, Some(libc::WTERMSIG(status)))
    } else {
        (None, None)
    }
}

fn pid_alive(pid: u32) -> bool {
    let rc = unsafe { libc::kill(pid as libc::pid_t, 0) };
    if rc == 0 {
        return true;
    }
    let err = io::Error::last_os_error().raw_os_error();
    err == Some(libc::EPERM)
}

fn send_signal(pid: u32, sig: libc::c_int) {
    // Signal both the process group (if pgid == pid) and the pid directly.
    unsafe {
        let _ = libc::kill(-(pid as libc::pid_t), sig);
        let _ = libc::kill(pid as libc::pid_t, sig);
    }
}

fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn lookup_user(name: &str) -> Result<(libc::uid_t, libc::gid_t), String> {
    let cname = CString::new(name).map_err(|_| format!("invalid user name: {}", name))?;
    unsafe {
        let pw = libc::getpwnam(cname.as_ptr());
        if pw.is_null() {
            return Err(format!("user not found: {}", name));
        }
        Ok(((*pw).pw_uid, (*pw).pw_gid))
    }
}

fn lookup_group(name: &str) -> Result<libc::gid_t, String> {
    let cname = CString::new(name).map_err(|_| format!("invalid group name: {}", name))?;
    unsafe {
        let gr = libc::getgrnam(cname.as_ptr());
        if gr.is_null() {
            return Err(format!("group not found: {}", name));
        }
        Ok((*gr).gr_gid)
    }
}
