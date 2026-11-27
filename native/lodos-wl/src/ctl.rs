//! Runtime control socket (headless only, T042): `LODOS_WL_CTL=<path>` -> AF_UNIX stream listener.
//!
//! Line protocol (one command per `\n`-terminated line, ASCII, space separated; reply per line
//! `ok\n` or `err <reason>\n`, sent after the command was applied inside the event loop):
//!
//! ```text
//! touch down   <slot> <x> <y>
//! touch motion <slot> <x> <y>
//! touch up     <slot> [<x> <y>]     (x y ignored)
//! touch cancel
//! ptr move <x> <y>
//! btn <evdev-button-code> 0|1       (e.g. BTN_LEFT=272)
//! key <evdev-key-code> 0|1          (e.g. KEY_ENTER=28)
//! dump <path.ppm>                   (framebuffer -> binary PPM, reply after write)
//! ping                              (no-op, ok)
//! ```
//! Coordinates are host output logical pixels (floats accepted). Empty lines / `#` comments ignored.

use std::{
    io::{Read, Write},
    os::unix::net::{UnixListener, UnixStream},
    path::{Path, PathBuf},
};

use smithay::reexports::calloop::{generic::Generic, Interest, LoopHandle, Mode as CMode, PostAction};

use crate::{log_msg, App, ControlMsg};

/// Parses one protocol line. `Ok(None)` = blank/comment, `Ok(Some(None))` never; `ping` maps to `PingClients`-less no-op.
pub fn parse_ctl_line(line: &str) -> Result<Option<CtlCmd>, String> {
    let line = line.trim();
    if line.is_empty() || line.starts_with('#') {
        return Ok(None);
    }
    let t: Vec<&str> = line.split_whitespace().collect();
    let f = |i: usize| -> Result<f64, String> {
        let s = t.get(i).ok_or_else(|| format!("missing arg {i}"))?;
        let v: f64 = s.parse().map_err(|_| format!("bad number '{s}'"))?;
        if v.is_finite() { Ok(v) } else { Err(format!("bad number '{s}'")) }
    };
    let u = |i: usize| -> Result<u32, String> {
        let s = t.get(i).ok_or_else(|| format!("missing arg {i}"))?;
        s.parse().map_err(|_| format!("bad uint '{s}'"))
    };
    let state = |i: usize| -> Result<bool, String> {
        match t.get(i).copied() {
            Some("1") => Ok(true),
            Some("0") => Ok(false),
            other => Err(format!("state must be 0|1, got {other:?}")),
        }
    };
    let argc = |n: usize| -> Result<(), String> {
        if t.len() == n { Ok(()) } else { Err(format!("'{}' expects {} args", t[0], n - 1)) }
    };
    let msg = match (t[0], t.get(1).copied()) {
        ("touch", Some("down")) => { argc(5)?; ControlMsg::TouchDown { slot: u(2)?, x: f(3)?, y: f(4)? } }
        ("touch", Some("motion")) => { argc(5)?; ControlMsg::TouchMotion { slot: u(2)?, x: f(3)?, y: f(4)? } }
        ("touch", Some("up")) => {
            if t.len() != 3 && t.len() != 5 { return Err("'touch up' expects slot [x y]".into()); }
            ControlMsg::TouchUp { slot: u(2)? }
        }
        ("touch", Some("cancel")) => { argc(2)?; ControlMsg::TouchCancel }
        ("ptr", Some("move")) => { argc(4)?; ControlMsg::PointerMotion { x: f(2)?, y: f(3)? } }
        ("btn", _) => { argc(3)?; ControlMsg::PointerButton { button: u(1)?, pressed: state(2)? } }
        ("key", _) => { argc(3)?; ControlMsg::Key { evdev_code: u(1)?, pressed: state(2)? } }
        ("dump", _) => {
            // path may contain spaces: take the rest of the line
            let p = line[4..].trim();
            if p.is_empty() { return Err("'dump' expects a path".into()); }
            ControlMsg::DumpPpm(PathBuf::from(p))
        }
        ("ping", None) => return Ok(Some(CtlCmd::Nop)),
        _ => return Err(format!("unknown command '{line}'")),
    };
    Ok(Some(CtlCmd::Msg(msg)))
}

/// Parsed control command.
#[derive(Debug, Clone)]
pub enum CtlCmd {
    Msg(ControlMsg),
    Nop,
}

/// Binds `path` and registers the listener + per-connection line readers on the event loop.
pub(crate) fn insert_ctl_listener(handle: &LoopHandle<'static, App>, path: &Path) -> Result<(), String> {
    let _ = std::fs::remove_file(path);
    let listener = UnixListener::bind(path).map_err(|e| format!("ctl bind {}: {e}", path.display()))?;
    listener.set_nonblocking(true).map_err(|e| format!("ctl nonblock: {e}"))?;
    log_msg(&format!("lodos-wl: control socket on {}", path.display()));
    let h = handle.clone();
    handle
        .insert_source(Generic::new(listener, Interest::READ, CMode::Level), move |_, l, _| {
            loop {
                match l.accept() {
                    Ok((stream, _)) => {
                        if let Err(e) = add_conn(&h, stream) {
                            log_msg(&format!("lodos-wl: ctl conn: {e}"));
                        }
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                    Err(e) => return Err(e),
                }
            }
            Ok(PostAction::Continue)
        })
        .map_err(|e| format!("insert ctl listener: {e}"))?;
    Ok(())
}

fn add_conn(h: &LoopHandle<'static, App>, stream: UnixStream) -> Result<(), String> {
    stream.set_nonblocking(true).map_err(|e| e.to_string())?;
    let mut buf: Vec<u8> = Vec::new();
    h.insert_source(Generic::new(stream, Interest::READ, CMode::Level), move |_, s, app| {
        let mut r: &UnixStream = s;
        let mut tmp = [0u8; 4096];
        let mut eof = false;
        loop {
            match r.read(&mut tmp) {
                Ok(0) => { eof = true; break; }
                Ok(n) => buf.extend_from_slice(&tmp[..n]),
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(_) => { eof = true; break; }
            }
        }
        let mut out = Vec::new();
        while let Some(nl) = buf.iter().position(|&b| b == b'\n') {
            let line: Vec<u8> = buf.drain(..=nl).collect();
            let line = String::from_utf8_lossy(&line);
            match parse_ctl_line(&line) {
                Ok(None) => continue,
                Ok(Some(CtlCmd::Nop)) => out.extend_from_slice(b"ok\n"),
                Ok(Some(CtlCmd::Msg(m))) => {
                    app.handle_control(m);
                    out.extend_from_slice(b"ok\n");
                }
                Err(e) => out.extend_from_slice(format!("err {e}\n").as_bytes()),
            }
        }
        if !out.is_empty() {
            // replies are tiny; a blocking-ish retry on EAGAIN is fine
            let mut w: &UnixStream = s;
            let mut off = 0;
            while off < out.len() {
                match w.write(&out[off..]) {
                    Ok(n) => off += n,
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => std::thread::yield_now(),
                    Err(_) => { eof = true; break; }
                }
            }
        }
        Ok(if eof { PostAction::Remove } else { PostAction::Continue })
    })
    .map_err(|e| format!("insert ctl conn: {e}"))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ctl_protocol_parser() {
        let m = |s: &str| match parse_ctl_line(s) {
            Ok(Some(CtlCmd::Msg(m))) => m,
            other => panic!("{s}: {other:?}"),
        };
        assert!(matches!(m("touch down 0 360 1590\n"), ControlMsg::TouchDown { slot: 0, x, y } if x == 360.0 && y == 1590.0));
        assert!(matches!(m("touch motion 1 1.5 2.25"), ControlMsg::TouchMotion { slot: 1, x, y } if x == 1.5 && y == 2.25));
        assert!(matches!(m("touch up 0"), ControlMsg::TouchUp { slot: 0 }));
        assert!(matches!(m("touch up 0 10 20"), ControlMsg::TouchUp { slot: 0 }));
        assert!(matches!(m("touch cancel"), ControlMsg::TouchCancel));
        assert!(matches!(m("ptr move 10 20"), ControlMsg::PointerMotion { x, y } if x == 10.0 && y == 20.0));
        assert!(matches!(m("btn 272 1"), ControlMsg::PointerButton { button: 272, pressed: true }));
        assert!(matches!(m("key 28 0"), ControlMsg::Key { evdev_code: 28, pressed: false }));
        assert!(matches!(m("dump /tmp/a b.ppm"), ControlMsg::DumpPpm(p) if p == Path::new("/tmp/a b.ppm")));
        assert!(matches!(parse_ctl_line("ping"), Ok(Some(CtlCmd::Nop))));
        assert!(matches!(parse_ctl_line("  # c"), Ok(None)));
        assert!(matches!(parse_ctl_line(""), Ok(None)));
        for bad in ["touch down 0 1", "touch down x 1 2", "btn 272 2", "key 28", "dump", "frob", "touch", "ptr move 1 nan", "touch up"] {
            assert!(parse_ctl_line(bad).is_err(), "should reject: {bad}");
        }
    }
}
