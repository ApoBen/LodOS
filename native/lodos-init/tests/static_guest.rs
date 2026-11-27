//! R18: exercise the static musl x86_64 build (build/init/x86_64, produced by
//! scripts/build-init.sh) as the guest would run it: fake root in a tmp dir via --root.
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Output};
use std::sync::OnceLock;
use std::thread;
use std::time::{Duration, Instant};

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap()
}

/// Build (cached by cargo) the static x86_64 binaries once per test process.
fn static_bins() -> &'static (PathBuf, PathBuf) {
    static BINS: OnceLock<(PathBuf, PathBuf)> = OnceLock::new();
    BINS.get_or_init(|| {
        let root = repo_root();
        let out = root.join("build/init/x86_64");
        let init = out.join("lodos-init");
        let sctl = out.join("systemctl");
        if std::env::var_os("LODOS_INIT_SKIP_BUILD").is_none() || !init.exists() {
            let st = Command::new("bash")
                .arg(root.join("scripts/build-init.sh"))
                .arg("x86_64")
                .env_remove("CARGO_TARGET_DIR")
                .output()
                .expect("run build-init.sh");
            assert!(
                st.status.success(),
                "build-init.sh failed:\n{}{}",
                String::from_utf8_lossy(&st.stdout),
                String::from_utf8_lossy(&st.stderr)
            );
        }
        assert!(init.exists() && sctl.exists());
        (init, sctl)
    })
}

struct Guest {
    root: PathBuf,
    socket: PathBuf,
    child: Option<Child>,
}

impl Guest {
    fn new(name: &str) -> Self {
        let root = std::env::temp_dir().join(format!("lodos-static-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&root);
        for d in ["etc/systemd/system", "run", "var/log"] {
            fs::create_dir_all(root.join(d)).unwrap();
        }
        let socket = root.join("run/lodos-init.sock");
        Self { root, socket, child: None }
    }

    fn start_init(&mut self) {
        let (init, _) = static_bins();
        let child = Command::new(init)
            .arg("--root")
            .arg(&self.root)
            .arg("--socket")
            .arg(&self.socket)
            .arg("--log")
            .arg(self.root.join("var/log/lodos-init.log"))
            .arg("--no-bootstrap")
            .arg("--stop-timeout-ms=1500")
            .spawn()
            .expect("spawn static lodos-init");
        self.child = Some(child);
        assert!(
            wait_until(Duration::from_secs(5), || self.socket.exists()
                && self.systemctl(&["list-units"]).status.success()),
            "static lodos-init not ready; log:\n{}",
            fs::read_to_string(self.root.join("var/log/lodos-init.log")).unwrap_or_default()
        );
    }

    fn systemctl(&self, args: &[&str]) -> Output {
        let (_, sctl) = static_bins();
        Command::new(sctl)
            .arg("--root")
            .arg(&self.root)
            .arg("--socket")
            .arg(&self.socket)
            .args(args)
            .output()
            .expect("run static systemctl")
    }
}

impl Drop for Guest {
    fn drop(&mut self) {
        if let Some(mut c) = self.child.take() {
            unsafe { libc::kill(c.id() as libc::pid_t, libc::SIGTERM) };
            if !wait_until(Duration::from_secs(3), || c.try_wait().ok().flatten().is_some()) {
                let _ = c.kill();
                let _ = c.wait();
            }
        }
        let _ = fs::remove_dir_all(&self.root);
    }
}

fn wait_until(timeout: Duration, mut pred: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if pred() {
            return true;
        }
        thread::sleep(Duration::from_millis(25));
    }
    pred()
}

fn pid_alive(pid: i32) -> bool {
    // zombie counts as dead
    match fs::read_to_string(format!("/proc/{}/stat", pid)) {
        Ok(s) => !s.rsplit(')').next().unwrap_or("").trim_start().starts_with('Z'),
        Err(_) => false,
    }
}

#[test]
fn static_binaries_are_statically_linked() {
    let (init, sctl) = static_bins();
    for b in [init, sctl] {
        let out = Command::new("file").arg("-L").arg(b).output().expect("file(1)");
        let s = String::from_utf8_lossy(&out.stdout);
        assert!(s.contains("statically linked"), "{}", s);
    }
    for b in ["lodos-init", "systemctl"] {
        let p = repo_root().join("build/init/aarch64").join(b);
        if p.exists() {
            let out = Command::new("file").arg("-L").arg(&p).output().unwrap();
            let s = String::from_utf8_lossy(&out.stdout);
            assert!(s.contains("statically linked") && s.contains("aarch64"), "{}", s);
        }
    }
}

#[test]
fn static_init_start_status_stop_dummy_service() {
    let mut g = Guest::new("svc");
    fs::write(
        g.root.join("etc/systemd/system/dummy.service"),
        "[Unit]\nDescription=Dummy Guest Service\n[Service]\nType=simple\nExecStart=/bin/sleep 120\n",
    )
    .unwrap();
    g.start_init();

    let out = g.systemctl(&["start", "dummy.service"]);
    assert!(out.status.success(), "start: {}", String::from_utf8_lossy(&out.stderr));

    let st = g.systemctl(&["status", "dummy"]);
    let text = String::from_utf8_lossy(&st.stdout).to_string();
    assert_eq!(st.status.code(), Some(0), "status: {}", text);
    assert!(text.contains("active"), "{}", text);
    assert!(text.contains("Dummy Guest Service"), "{}", text);
    let pid: i32 = text
        .lines()
        .find_map(|l| l.trim().strip_prefix("Main PID:").map(|r| r.trim().parse().unwrap()))
        .expect("Main PID");
    assert!(pid_alive(pid));

    let out = g.systemctl(&["stop", "dummy"]);
    assert!(out.status.success(), "stop: {}", String::from_utf8_lossy(&out.stderr));
    assert!(wait_until(Duration::from_secs(3), || !pid_alive(pid)), "pid {} survived stop", pid);
    let out = g.systemctl(&["is-active", "dummy"]);
    assert_eq!(out.status.code(), Some(3));
    assert_eq!(String::from_utf8_lossy(&out.stdout).trim(), "inactive");
}
