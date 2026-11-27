use std::{env, ffi::CString, io, thread, time::{Duration, Instant}};

#[derive(Debug, PartialEq, Eq)]
struct Args { fifo: String, sock: String }

fn parse_args<I, S>(it: I) -> Result<Args, String>
where I: IntoIterator<Item=S>, S: Into<String> {
    let mut it = it.into_iter().map(Into::into);
    let _program = it.next();
    let mut fifo = None;
    let mut sock = None;
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--fifo" => fifo = Some(it.next().ok_or("missing value for --fifo")?),
            "--sock" => sock = Some(it.next().ok_or("missing value for --sock")?),
            "-h" | "--help" => return Err("usage: lodos-audio-relay --fifo <path> --sock <path|@abstract>".into()),
            _ => return Err(format!("unknown argument: {arg}")),
        }
    }
    Ok(Args { fifo: fifo.ok_or("missing --fifo")?, sock: sock.ok_or("missing --sock")? })
}

fn sockaddr(name: &str) -> io::Result<(libc::sockaddr_un, libc::socklen_t)> {
    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
    let bytes = name.as_bytes();
    let abstract_name = bytes.first() == Some(&b'@');
    let (offset, data) = if abstract_name { (1, &bytes[1..]) } else { (0, bytes) };
    if data.is_empty() || offset + data.len() + usize::from(!abstract_name) > addr.sun_path.len() {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "unix socket name too long or empty"));
    }
    for (i, byte) in data.iter().enumerate() { addr.sun_path[offset + i] = *byte as libc::c_char; }
    let len = (std::mem::size_of::<libc::sa_family_t>() + offset + data.len() + usize::from(!abstract_name)) as libc::socklen_t;
    Ok((addr, len))
}

fn open_socket(name: &str) -> io::Result<libc::c_int> {
    let (addr, len) = sockaddr(name)?;
    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC, 0) };
    if fd < 0 { return Err(io::Error::last_os_error()); }
    let rc = unsafe { libc::connect(fd, &addr as *const _ as *const libc::sockaddr, len) };
    if rc == 0 { return Ok(fd); }
    let e = io::Error::last_os_error();
    if e.kind() == io::ErrorKind::WouldBlock || e.raw_os_error() == Some(libc::EINPROGRESS) { Ok(fd) }
    else { unsafe { libc::close(fd); } Err(e) }
}

fn connected(fd: libc::c_int) -> bool {
    let mut err: libc::c_int = 0;
    let mut len = std::mem::size_of_val(&err) as libc::socklen_t;
    unsafe { libc::getsockopt(fd, libc::SOL_SOCKET, libc::SO_ERROR, &mut err as *mut _ as *mut _, &mut len) == 0 && err == 0 }
}

fn reconnect_at(now: Instant) -> Instant { now + Duration::from_millis(200) }

fn main() {
    let args = match parse_args(env::args()) { Ok(a) => a, Err(e) => { eprintln!("{e}"); std::process::exit(2); } };
    if let Err(e) = run(&args) { eprintln!("lodos-audio-relay: {e}"); std::process::exit(1); }
}

fn run(args: &Args) -> io::Result<()> {
    let fifo = CString::new(args.fifo.as_bytes()).map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "FIFO path contains NUL"))?;
    let input = unsafe { libc::open(fifo.as_ptr(), libc::O_RDONLY | libc::O_NONBLOCK | libc::O_CLOEXEC) };
    if input < 0 { return Err(io::Error::last_os_error()); }
    let mut socket = -1;
    let mut connecting = false;
    let mut retry_at = Instant::now();
    let mut buf = [0u8; 8192];
    loop {
        if socket < 0 && Instant::now() >= retry_at {
            match open_socket(&args.sock) {
                Ok(fd) => { socket = fd; connecting = true; }
                Err(_) => retry_at = reconnect_at(Instant::now()),
            }
        }
        let n = unsafe { libc::read(input, buf.as_mut_ptr() as *mut _, buf.len()) };
        if n > 0 {
            if socket >= 0 {
                if connecting {
                    let mut pfd = libc::pollfd { fd: socket, events: libc::POLLOUT, revents: 0 };
                    if unsafe { libc::poll(&mut pfd, 1, 0) } == 0 { thread::sleep(Duration::from_millis(2)); continue; }
                    connecting = false;
                    if !connected(socket) { unsafe { libc::close(socket); } socket = -1; retry_at = reconnect_at(Instant::now()); continue; }
                }
                let sent = unsafe { libc::send(socket, buf.as_ptr() as *const _, n as usize, libc::MSG_NOSIGNAL | libc::MSG_DONTWAIT) };
                // A partial write or backpressure drops the rest of this chunk; the FIFO reader keeps draining.
                if sent < 0 && io::Error::last_os_error().raw_os_error() != Some(libc::EAGAIN) {
                    unsafe { libc::close(socket); } socket = -1; retry_at = reconnect_at(Instant::now());
                }
            }
        } else if n < 0 {
            let e = io::Error::last_os_error();
            if e.raw_os_error() != Some(libc::EAGAIN) && e.raw_os_error() != Some(libc::EINTR) { unsafe { libc::close(input); } return Err(e); }
        }
        if n == 0 || (n < 0 && io::Error::last_os_error().raw_os_error() == Some(libc::EAGAIN)) { thread::sleep(Duration::from_millis(2)); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn parses_required_arguments() {
        assert_eq!(parse_args(["relay", "--fifo", "/tmp/f", "--sock", "@audio"]).unwrap(), Args { fifo: "/tmp/f".into(), sock: "@audio".into() });
        assert!(parse_args(["relay", "--fifo", "/tmp/f"]).is_err());
        assert!(parse_args(["relay", "--bad"]).is_err());
    }
    #[test] fn socket_names_support_filesystem_and_abstract() {
        assert!(sockaddr("/tmp/audio.sock").is_ok());
        assert!(sockaddr("@audio").is_ok());
        assert!(sockaddr("").is_err());
    }
    #[test] fn reconnect_retry_is_bounded_and_repeatable() {
        let failed_at = Instant::now();
        let retry_at = reconnect_at(failed_at);
        assert!(retry_at > failed_at);
        assert_eq!(retry_at.duration_since(failed_at), Duration::from_millis(200));
    }
}
