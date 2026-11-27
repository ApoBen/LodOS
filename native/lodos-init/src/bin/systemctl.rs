use std::env;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use lodos_init::protocol::{send_request, Request, Response, DEFAULT_SOCKET_PATH};
use lodos_init::unit::{disable_unit, discover_units, enable_unit, normalize_unit_name};

struct CliOpts {
    socket: PathBuf,
    root: PathBuf,
    quiet: bool,
    all: bool,
    now: bool,
    command: String,
    args: Vec<String>,
}

fn parse_cli() -> Result<CliOpts, String> {
    let mut root = env::var("LODOS_INIT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/"));
    let mut socket_opt = env::var("LODOS_INIT_SOCKET").ok().map(PathBuf::from);
    let mut quiet = false;
    let mut all = false;
    let mut now = false;
    let mut positional: Vec<String> = Vec::new();

    let mut it = env::args().skip(1);
    while let Some(arg) = it.next() {
        if let Some(v) = arg.strip_prefix("--socket=") {
            socket_opt = Some(PathBuf::from(v));
        } else if arg == "--socket" {
            socket_opt = Some(PathBuf::from(it.next().ok_or("--socket requires a path")?));
        } else if let Some(v) = arg.strip_prefix("--root=") {
            root = PathBuf::from(v);
        } else if arg == "--root" {
            root = PathBuf::from(it.next().ok_or("--root requires a path")?);
        } else if arg == "-q" || arg == "--quiet" {
            quiet = true;
        } else if arg == "-a" || arg == "--all" {
            all = true;
        } else if arg == "--now" {
            now = true;
        } else if arg == "--no-pager"
            || arg == "--no-block"
            || arg == "--no-ask-password"
            || arg == "--system"
            || arg == "--plain"
            || arg == "--no-legend"
            || arg == "--full"
            || arg == "-l"
            || arg.starts_with("--type=")
            || arg.starts_with("--state=")
        {
            // Accepted for compatibility with systemd/Debian maintainer scripts.
        } else if arg == "-t" || arg == "--type" || arg == "--state" {
            let _ = it.next();
        } else if arg == "-h" || arg == "--help" {
            println!(
                "Usage: systemctl [OPTIONS] COMMAND [UNIT...]\n\
                 Commands: start, stop, restart, status, is-active, enable, disable, list-units, daemon-reload"
            );
            std::process::exit(0);
        } else if arg.starts_with('-') {
            return Err(format!("unrecognized option: {}", arg));
        } else {
            positional.push(arg);
        }
    }

    let socket = socket_opt.unwrap_or_else(|| {
        if root == Path::new("/") {
            PathBuf::from(DEFAULT_SOCKET_PATH)
        } else {
            root.join("run/lodos-init.sock")
        }
    });

    if positional.is_empty() {
        return Ok(CliOpts {
            socket,
            root,
            quiet,
            all,
            now,
            command: "list-units".to_string(),
            args: Vec::new(),
        });
    }

    let command = positional.remove(0);
    Ok(CliOpts {
        socket,
        root,
        quiet,
        all,
        now,
        command,
        args: positional,
    })
}

fn offline_enable_disable(opts: &CliOpts) -> Response {
    if opts.command == "enable" {
        let units = discover_units(&opts.root);
        let mut out = String::new();
        for u in &opts.args {
            let norm = normalize_unit_name(u);
            let Some(uf) = units.get(&norm) else {
                return Response::err(1, format!("Unit file {} does not exist.\n", norm));
            };
            match enable_unit(&opts.root, uf) {
                Ok(links) => {
                    for l in links {
                        out.push_str(&format!(
                            "Created symlink {} -> {}.\n",
                            l.display(),
                            uf.path.display()
                        ));
                    }
                }
                Err(e) => return Response::err(1, format!("Failed to enable {}: {}\n", norm, e)),
            }
        }
        Response::ok(out)
    } else {
        let mut out = String::new();
        for u in &opts.args {
            let norm = normalize_unit_name(u);
            match disable_unit(&opts.root, &norm) {
                Ok(removed) => {
                    for r in removed {
                        out.push_str(&format!("Removed \"{}\".\n", r.display()));
                    }
                }
                Err(e) => return Response::err(1, format!("Failed to disable {}: {}\n", norm, e)),
            }
        }
        Response::ok(out)
    }
}

fn main() -> ExitCode {
    let opts = match parse_cli() {
        Ok(o) => o,
        Err(e) => {
            eprintln!("systemctl: {}", e);
            return ExitCode::from(2);
        }
    };

    let req = match opts.command.as_str() {
        "start" => {
            if opts.args.is_empty() {
                eprintln!("Too few arguments.");
                return ExitCode::from(1);
            }
            Request::Start(opts.args.clone())
        }
        "stop" => {
            if opts.args.is_empty() {
                eprintln!("Too few arguments.");
                return ExitCode::from(1);
            }
            Request::Stop(opts.args.clone())
        }
        "restart" => {
            if opts.args.is_empty() {
                eprintln!("Too few arguments.");
                return ExitCode::from(1);
            }
            Request::Restart(opts.args.clone())
        }
        "status" => {
            if opts.args.len() != 1 {
                eprintln!("status requires a unit name.");
                return ExitCode::from(1);
            }
            Request::Status(opts.args[0].clone())
        }
        "is-active" => {
            if opts.args.len() != 1 {
                eprintln!("is-active requires a unit name.");
                return ExitCode::from(1);
            }
            Request::IsActive(opts.args[0].clone())
        }
        "enable" => {
            if opts.args.is_empty() {
                eprintln!("Too few arguments.");
                return ExitCode::from(1);
            }
            Request::Enable {
                units: opts.args.clone(),
                now: opts.now,
            }
        }
        "disable" => {
            if opts.args.is_empty() {
                eprintln!("Too few arguments.");
                return ExitCode::from(1);
            }
            Request::Disable {
                units: opts.args.clone(),
                now: opts.now,
            }
        }
        "list-units" => Request::ListUnits { all: opts.all },
        "daemon-reload" => Request::DaemonReload,
        other => {
            eprintln!("Unknown command verb {}.", other);
            return ExitCode::from(1);
        }
    };

    let resp = match send_request(&opts.socket, &req) {
        Ok(r) => r,
        Err(e) => {
            // Allow offline enable/disable when lodos-init is not running (e.g. rootfs setup).
            if (opts.command == "enable" || opts.command == "disable") && !opts.now {
                offline_enable_disable(&opts)
            } else {
                eprintln!(
                    "Failed to connect to lodos-init ({}): {}",
                    opts.socket.display(),
                    e
                );
                return ExitCode::from(1);
            }
        }
    };

    if !opts.quiet && !resp.stdout.is_empty() {
        let _ = io::stdout().write_all(resp.stdout.as_bytes());
    }
    if !resp.stderr.is_empty() {
        let _ = io::stderr().write_all(resp.stderr.as_bytes());
    }

    let code = (resp.exit_code.clamp(0, 255)) as u8;
    ExitCode::from(code)
}
