use std::io::{self, BufRead, BufReader, Read, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::time::Duration;

pub const DEFAULT_SOCKET_PATH: &str = "/run/lodos-init.sock";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Request {
    Start(Vec<String>),
    Stop(Vec<String>),
    Restart(Vec<String>),
    Status(String),
    IsActive(String),
    Enable { units: Vec<String>, now: bool },
    Disable { units: Vec<String>, now: bool },
    ListUnits { all: bool },
    DaemonReload,
    Ping,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Response {
    pub exit_code: i32,
    pub stdout: String,
    pub stderr: String,
}

impl Response {
    pub fn ok(stdout: impl Into<String>) -> Self {
        Self {
            exit_code: 0,
            stdout: stdout.into(),
            stderr: String::new(),
        }
    }

    pub fn err(exit_code: i32, stderr: impl Into<String>) -> Self {
        Self {
            exit_code,
            stdout: String::new(),
            stderr: stderr.into(),
        }
    }

    pub fn with_code(exit_code: i32, stdout: impl Into<String>, stderr: impl Into<String>) -> Self {
        Self {
            exit_code,
            stdout: stdout.into(),
            stderr: stderr.into(),
        }
    }
}

fn sanitize_token(s: &str) -> String {
    s.chars()
        .filter(|c| *c != '\n' && *c != '\r' && *c != '\t')
        .collect()
}

pub fn encode_request(req: &Request) -> String {
    let mut parts: Vec<String> = Vec::new();
    match req {
        Request::Start(units) => {
            parts.push("start".to_string());
            parts.extend(units.iter().map(|u| sanitize_token(u)));
        }
        Request::Stop(units) => {
            parts.push("stop".to_string());
            parts.extend(units.iter().map(|u| sanitize_token(u)));
        }
        Request::Restart(units) => {
            parts.push("restart".to_string());
            parts.extend(units.iter().map(|u| sanitize_token(u)));
        }
        Request::Status(unit) => {
            parts.push("status".to_string());
            parts.push(sanitize_token(unit));
        }
        Request::IsActive(unit) => {
            parts.push("is-active".to_string());
            parts.push(sanitize_token(unit));
        }
        Request::Enable { units, now } => {
            parts.push(if *now { "enable-now" } else { "enable" }.to_string());
            parts.extend(units.iter().map(|u| sanitize_token(u)));
        }
        Request::Disable { units, now } => {
            parts.push(if *now { "disable-now" } else { "disable" }.to_string());
            parts.extend(units.iter().map(|u| sanitize_token(u)));
        }
        Request::ListUnits { all } => {
            parts.push(if *all { "list-units-all" } else { "list-units" }.to_string());
        }
        Request::DaemonReload => {
            parts.push("daemon-reload".to_string());
        }
        Request::Ping => {
            parts.push("ping".to_string());
        }
    }
    let mut line = parts.join("\t");
    line.push('\n');
    line
}

pub fn decode_request(line: &str) -> Result<Request, String> {
    let trimmed = line.trim_end_matches(|c| c == '\n' || c == '\r');
    if trimmed.is_empty() {
        return Err("empty request".to_string());
    }
    let mut parts = trimmed.split('\t');
    let cmd = parts.next().unwrap_or("");
    let args: Vec<String> = parts
        .filter(|s| !s.is_empty())
        .map(|s| s.to_string())
        .collect();
    match cmd {
        "start" => {
            if args.is_empty() {
                Err("start requires at least one unit".to_string())
            } else {
                Ok(Request::Start(args))
            }
        }
        "stop" => {
            if args.is_empty() {
                Err("stop requires at least one unit".to_string())
            } else {
                Ok(Request::Stop(args))
            }
        }
        "restart" => {
            if args.is_empty() {
                Err("restart requires at least one unit".to_string())
            } else {
                Ok(Request::Restart(args))
            }
        }
        "status" => {
            if args.len() != 1 {
                Err("status requires a unit name".to_string())
            } else {
                Ok(Request::Status(args[0].clone()))
            }
        }
        "is-active" => {
            if args.len() != 1 {
                Err("is-active requires a unit name".to_string())
            } else {
                Ok(Request::IsActive(args[0].clone()))
            }
        }
        "enable" => {
            if args.is_empty() {
                Err("enable requires at least one unit".to_string())
            } else {
                Ok(Request::Enable {
                    units: args,
                    now: false,
                })
            }
        }
        "enable-now" => {
            if args.is_empty() {
                Err("enable requires at least one unit".to_string())
            } else {
                Ok(Request::Enable {
                    units: args,
                    now: true,
                })
            }
        }
        "disable" => {
            if args.is_empty() {
                Err("disable requires at least one unit".to_string())
            } else {
                Ok(Request::Disable {
                    units: args,
                    now: false,
                })
            }
        }
        "disable-now" => {
            if args.is_empty() {
                Err("disable requires at least one unit".to_string())
            } else {
                Ok(Request::Disable {
                    units: args,
                    now: true,
                })
            }
        }
        "list-units" => Ok(Request::ListUnits { all: false }),
        "list-units-all" => Ok(Request::ListUnits { all: true }),
        "daemon-reload" => Ok(Request::DaemonReload),
        "ping" => Ok(Request::Ping),
        other => Err(format!("unknown command: {}", other)),
    }
}

pub fn write_response<W: Write>(mut w: W, resp: &Response) -> io::Result<()> {
    let header = format!(
        "EXIT {}\nOUT {}\nERR {}\n",
        resp.exit_code,
        resp.stdout.len(),
        resp.stderr.len()
    );
    w.write_all(header.as_bytes())?;
    w.write_all(resp.stdout.as_bytes())?;
    w.write_all(resp.stderr.as_bytes())?;
    w.flush()
}

pub fn read_response<R: Read>(r: R) -> io::Result<Response> {
    let mut br = BufReader::new(r);
    let mut exit_line = String::new();
    let mut out_line = String::new();
    let mut err_line = String::new();
    br.read_line(&mut exit_line)?;
    br.read_line(&mut out_line)?;
    br.read_line(&mut err_line)?;

    let exit_code: i32 = exit_line
        .trim()
        .strip_prefix("EXIT ")
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "malformed EXIT header"))?;
    let out_len: usize = out_line
        .trim()
        .strip_prefix("OUT ")
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "malformed OUT header"))?;
    let err_len: usize = err_line
        .trim()
        .strip_prefix("ERR ")
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "malformed ERR header"))?;

    let mut out_buf = vec![0u8; out_len];
    br.read_exact(&mut out_buf)?;
    let mut err_buf = vec![0u8; err_len];
    br.read_exact(&mut err_buf)?;

    Ok(Response {
        exit_code,
        stdout: String::from_utf8_lossy(&out_buf).into_owned(),
        stderr: String::from_utf8_lossy(&err_buf).into_owned(),
    })
}

pub fn read_request<R: Read>(r: R) -> io::Result<Result<Request, String>> {
    let mut br = BufReader::new(r);
    let mut line = String::new();
    br.read_line(&mut line)?;
    Ok(decode_request(&line))
}

pub fn send_request(socket_path: &Path, req: &Request) -> io::Result<Response> {
    let mut stream = UnixStream::connect(socket_path)?;
    let _ = stream.set_read_timeout(Some(Duration::from_secs(30)));
    let _ = stream.set_write_timeout(Some(Duration::from_secs(10)));
    let encoded = encode_request(req);
    stream.write_all(encoded.as_bytes())?;
    stream.flush()?;
    read_response(stream)
}
