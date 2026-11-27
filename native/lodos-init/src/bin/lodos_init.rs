use std::env;
use std::path::PathBuf;
use std::process::ExitCode;

use lodos_init::supervisor::{
    enable_subreaper, install_signal_handlers, BootstrapMode, Supervisor, SupervisorConfig,
};

fn parse_args() -> Result<SupervisorConfig, String> {
    let mut root = env::var("LODOS_INIT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/"));
    let mut socket: Option<PathBuf> = env::var("LODOS_INIT_SOCKET").ok().map(PathBuf::from);
    let mut log_path: Option<PathBuf> = env::var("LODOS_INIT_LOG").ok().map(PathBuf::from);
    let mut target = "multi-user.target".to_string();
    let mut bootstrap = BootstrapMode::Auto;
    let mut base_backoff_ms: Option<u64> = None;
    let mut max_backoff_ms: Option<u64> = None;
    let mut stop_timeout_ms: Option<u64> = None;
    let mut log_stderr = false;

    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        if let Some(v) = arg.strip_prefix("--root=") {
            root = PathBuf::from(v);
        } else if arg == "--root" {
            root = PathBuf::from(args.next().ok_or("--root requires a path")?);
        } else if let Some(v) = arg.strip_prefix("--socket=") {
            socket = Some(PathBuf::from(v));
        } else if arg == "--socket" {
            socket = Some(PathBuf::from(args.next().ok_or("--socket requires a path")?));
        } else if let Some(v) = arg.strip_prefix("--log=") {
            log_path = Some(PathBuf::from(v));
        } else if arg == "--log" {
            log_path = Some(PathBuf::from(args.next().ok_or("--log requires a path")?));
        } else if let Some(v) = arg.strip_prefix("--target=") {
            target = v.to_string();
        } else if arg == "--target" {
            target = args.next().ok_or("--target requires a value")?;
        } else if arg == "--no-bootstrap" {
            bootstrap = BootstrapMode::None;
        } else if let Some(v) = arg.strip_prefix("--backoff-base-ms=") {
            base_backoff_ms = Some(v.parse().map_err(|_| "invalid --backoff-base-ms")?);
        } else if let Some(v) = arg.strip_prefix("--backoff-max-ms=") {
            max_backoff_ms = Some(v.parse().map_err(|_| "invalid --backoff-max-ms")?);
        } else if let Some(v) = arg.strip_prefix("--stop-timeout-ms=") {
            stop_timeout_ms = Some(v.parse().map_err(|_| "invalid --stop-timeout-ms")?);
        } else if arg == "--stderr" {
            log_stderr = true;
        } else if arg == "-h" || arg == "--help" {
            println!(
                "Usage: lodos-init [--root DIR] [--socket PATH] [--log PATH] [--target TARGET] [--no-bootstrap] [--backoff-base-ms MS] [--stop-timeout-ms MS] [--stderr]"
            );
            std::process::exit(0);
        } else {
            return Err(format!("unknown argument: {}", arg));
        }
    }

    let mut cfg = SupervisorConfig::default_for_root(&root);
    if let Some(s) = socket {
        cfg.socket_path = s;
    }
    if let Some(l) = log_path {
        cfg.log_path = l;
    }
    cfg.target = target;
    cfg.bootstrap = bootstrap;
    if let Some(b) = base_backoff_ms {
        cfg.base_backoff_ms = b;
    }
    if let Some(m) = max_backoff_ms {
        cfg.max_backoff_ms = m;
    }
    if let Some(t) = stop_timeout_ms {
        cfg.stop_timeout_ms = t;
    }
    cfg.log_stderr = log_stderr;
    Ok(cfg)
}

fn main() -> ExitCode {
    let cfg = match parse_args() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("lodos-init: {}", e);
            return ExitCode::from(2);
        }
    };

    install_signal_handlers();
    let _ = enable_subreaper();

    let mut sup = match Supervisor::new(cfg) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("lodos-init: init failed: {}", e);
            return ExitCode::from(1);
        }
    };

    if let Err(e) = sup.run(None) {
        eprintln!("lodos-init: supervisor error: {}", e);
        return ExitCode::from(1);
    }

    ExitCode::SUCCESS
}
