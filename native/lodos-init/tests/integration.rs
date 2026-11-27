use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Output};
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use lodos_init::unit::discover_units;

static TEST_COUNTER: AtomicU64 = AtomicU64::new(0);

struct TestEnv {
    root: PathBuf,
    socket: PathBuf,
    log: PathBuf,
}

impl TestEnv {
    fn new(name: &str) -> Self {
        let id = TEST_COUNTER.fetch_add(1, Ordering::SeqCst);
        let pid = std::process::id();
        let root = std::env::temp_dir().join(format!("lodos-init-test-{}-{}-{}", name, pid, id));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("etc/systemd/system")).unwrap();
        fs::create_dir_all(root.join("lib/systemd/system")).unwrap();
        fs::create_dir_all(root.join("usr/lib/systemd/system")).unwrap();
        fs::create_dir_all(root.join("run")).unwrap();
        fs::create_dir_all(root.join("var/log")).unwrap();
        let socket = root.join("run/lodos-init.sock");
        let log = root.join("var/log/lodos-init.log");
        Self { root, socket, log }
    }

    fn write_unit(&self, rel_dir: &str, filename: &str, content: &str) -> PathBuf {
        let dir = self.root.join(rel_dir);
        fs::create_dir_all(&dir).unwrap();
        let p = dir.join(filename);
        fs::write(&p, content).unwrap();
        p
    }

    fn spawn_init(&self, extra_args: &[&str]) -> InitGuard {
        let bin = env!("CARGO_BIN_EXE_lodos-init");
        let mut cmd = Command::new(bin);
        cmd.arg("--root")
            .arg(&self.root)
            .arg("--socket")
            .arg(&self.socket)
            .arg("--log")
            .arg(&self.log)
            .arg("--backoff-base-ms=60")
            .arg("--backoff-max-ms=400")
            .arg("--stop-timeout-ms=1500");
        for a in extra_args {
            cmd.arg(a);
        }
        let child = cmd.spawn().expect("spawn lodos-init");
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline {
            if self.socket.exists() {
                // Verify it accepts connections via list-units or is-active.
                let out = self.systemctl(&["list-units"]);
                if out.status.success() {
                    return InitGuard { child: Some(child) };
                }
            }
            thread::sleep(Duration::from_millis(25));
        }
        panic!(
            "lodos-init did not create ready socket at {}. log:\n{}",
            self.socket.display(),
            self.read_log()
        );
    }

    fn systemctl(&self, args: &[&str]) -> Output {
        let bin = env!("CARGO_BIN_EXE_systemctl");
        Command::new(bin)
            .arg("--root")
            .arg(&self.root)
            .arg("--socket")
            .arg(&self.socket)
            .args(args)
            .output()
            .expect("run systemctl")
    }

    fn read_log(&self) -> String {
        fs::read_to_string(&self.log).unwrap_or_default()
    }
}

impl Drop for TestEnv {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

struct InitGuard {
    child: Option<Child>,
}

impl InitGuard {
    fn send_sigterm_and_wait(&mut self) -> std::process::ExitStatus {
        let mut child = self.child.take().unwrap();
        unsafe {
            libc::kill(child.id() as libc::pid_t, libc::SIGTERM);
        }
        let deadline = Instant::now() + Duration::from_secs(6);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                return status;
            }
            if Instant::now() >= deadline {
                let _ = child.kill();
                return child.wait().unwrap();
            }
            thread::sleep(Duration::from_millis(20));
        }
    }
}

impl Drop for InitGuard {
    fn drop(&mut self) {
        if let Some(mut child) = self.child.take() {
            unsafe {
                libc::kill(child.id() as libc::pid_t, libc::SIGTERM);
            }
            let deadline = Instant::now() + Duration::from_secs(3);
            while Instant::now() < deadline {
                if child.try_wait().ok().flatten().is_some() {
                    return;
                }
                thread::sleep(Duration::from_millis(20));
            }
            let _ = child.kill();
            let _ = child.wait();
        }
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

fn extract_main_pid(status_stdout: &str) -> Option<u32> {
    for line in status_stdout.lines() {
        if let Some(rest) = line.trim().strip_prefix("Main PID:") {
            return rest.trim().parse::<u32>().ok();
        }
    }
    None
}

#[test]
fn search_dir_precedence_etc_over_lib_over_usr_lib() {
    let env = TestEnv::new("precedence");
    env.write_unit(
        "usr/lib/systemd/system",
        "override.service",
        "[Unit]\nDescription=from-usr-lib\n[Service]\nExecStart=/bin/true\n",
    );
    env.write_unit(
        "lib/systemd/system",
        "override.service",
        "[Unit]\nDescription=from-lib\n[Service]\nExecStart=/bin/true\n",
    );
    env.write_unit(
        "etc/systemd/system",
        "override.service",
        "[Unit]\nDescription=from-etc\n[Service]\nExecStart=/bin/true\n",
    );
    env.write_unit(
        "lib/systemd/system",
        "libonly.service",
        "[Unit]\nDescription=lib-only\n[Service]\nExecStart=/bin/true\n",
    );
    env.write_unit(
        "usr/lib/systemd/system",
        "usrlibonly.service",
        "[Unit]\nDescription=usr-lib-only\n[Service]\nExecStart=/bin/true\n",
    );

    let units = discover_units(&env.root);
    assert_eq!(units["override.service"].unit.description, "from-etc");
    assert_eq!(units["libonly.service"].unit.description, "lib-only");
    assert_eq!(units["usrlibonly.service"].unit.description, "usr-lib-only");
}

#[test]
fn simple_sleep_lifecycle_and_systemctl_exit_codes() {
    let env = TestEnv::new("sleep-lifecycle");
    env.write_unit(
        "etc/systemd/system",
        "sleeper.service",
        "[Unit]\nDescription=Sleep Worker\n[Service]\nType=simple\nExecStart=/bin/sleep 60\n",
    );

    let _init = env.spawn_init(&["--no-bootstrap"]);

    // Initially inactive -> is-active exits 3, status exits 3
    let out = env.systemctl(&["is-active", "sleeper"]);
    assert_eq!(out.status.code(), Some(3));
    assert_eq!(String::from_utf8_lossy(&out.stdout).trim(), "inactive");

    let out = env.systemctl(&["status", "sleeper"]);
    assert_eq!(out.status.code(), Some(3));

    // Missing unit -> status exits 4, is-active exits 3
    let out = env.systemctl(&["status", "nonexistent"]);
    assert_eq!(out.status.code(), Some(4));
    let out = env.systemctl(&["is-active", "nonexistent"]);
    assert_eq!(out.status.code(), Some(3));

    // Start sleeper -> is-active exits 0, status exits 0 with Main PID
    let out = env.systemctl(&["start", "sleeper.service"]);
    assert!(out.status.success(), "stderr: {}", String::from_utf8_lossy(&out.stderr));

    let out = env.systemctl(&["is-active", "sleeper"]);
    assert_eq!(out.status.code(), Some(0));
    assert_eq!(String::from_utf8_lossy(&out.stdout).trim(), "active");

    let out = env.systemctl(&["is-active", "-q", "sleeper"]);
    assert_eq!(out.status.code(), Some(0));
    assert!(out.stdout.is_empty());

    let st1 = env.systemctl(&["status", "sleeper"]);
    assert_eq!(st1.status.code(), Some(0));
    let st1_text = String::from_utf8_lossy(&st1.stdout);
    assert!(st1_text.contains("Sleep Worker"));
    let pid1 = extract_main_pid(&st1_text).expect("Main PID present");

    // Restart -> new PID
    let out = env.systemctl(&["restart", "sleeper"]);
    assert!(out.status.success());
    let st2 = env.systemctl(&["status", "sleeper"]);
    assert_eq!(st2.status.code(), Some(0));
    let pid2 = extract_main_pid(&String::from_utf8_lossy(&st2.stdout)).expect("Main PID after restart");
    assert_ne!(pid1, pid2);

    // list-units contains sleeper.service
    let list = env.systemctl(&["list-units"]);
    assert!(list.status.success());
    assert!(String::from_utf8_lossy(&list.stdout).contains("sleeper.service"));

    // Stop -> is-active exits 3
    let out = env.systemctl(&["stop", "sleeper"]);
    assert!(out.status.success());
    let out = env.systemctl(&["is-active", "sleeper"]);
    assert_eq!(out.status.code(), Some(3));
}

#[test]
fn oneshot_with_exec_start_pre_env_envfile_and_workdir() {
    let env = TestEnv::new("oneshot-env");
    let workdir = env.root.join("var/work");
    fs::create_dir_all(&workdir).unwrap();
    let envfile = env.root.join("etc/demo.env");
    fs::write(&envfile, "# comment\nFROM_FILE=file_val\nOVERRIDDEN=old\n").unwrap();
    let out_marker = env.root.join("var/work/result.txt");
    let pre_marker = env.root.join("var/work/pre.txt");

    let unit = format!(
        "[Unit]\nDescription=Oneshot Test\n\
         [Service]\nType=oneshot\nRemainAfterExit=yes\n\
         WorkingDirectory=/var/work\n\
         EnvironmentFile=/etc/demo.env\n\
         Environment=\"INLINE_VAR=hello world\" OVERRIDDEN=new\n\
         ExecStartPre=/bin/sh -c \"echo pre > {}\"\n\
         ExecStart=/bin/sh -c \"pwd > {out}; echo $FROM_FILE:$INLINE_VAR:$OVERRIDDEN >> {out}\"\n",
        pre_marker.display(),
        out = out_marker.display()
    );
    env.write_unit("etc/systemd/system", "prep.service", &unit);
    env.write_unit(
        "etc/systemd/system",
        "trivial.service",
        "[Service]\nType=oneshot\nExecStart=/bin/true\n",
    );

    let _init = env.spawn_init(&["--no-bootstrap"]);

    let out = env.systemctl(&["start", "prep.service"]);
    assert!(out.status.success(), "stderr: {}", String::from_utf8_lossy(&out.stderr));
    assert_eq!(env.systemctl(&["is-active", "prep.service"]).status.code(), Some(0));

    assert!(pre_marker.exists());
    let result = fs::read_to_string(&out_marker).unwrap();
    let lines: Vec<&str> = result.lines().collect();
    assert_eq!(lines.len(), 2, "got: {}", result);
    assert_eq!(Path::new(lines[0]), workdir.as_path());
    assert_eq!(lines[1], "file_val:hello world:new");

    // Non-RemainAfterExit oneshot (/bin/true) succeeds and transitions to inactive
    let out = env.systemctl(&["start", "trivial.service"]);
    assert!(out.status.success());
    assert_eq!(env.systemctl(&["is-active", "trivial.service"]).status.code(), Some(3));
}

#[test]
fn crashing_unit_restarts_with_backoff_and_stops_cleanly() {
    let env = TestEnv::new("crash-restart");
    let counter_file = env.root.join("var/crash-count.txt");
    let unit = format!(
        "[Unit]\nDescription=Crashing Unit\n\
         [Service]\nType=simple\n\
         ExecStart=/bin/sh -c \"echo x >> {}; exit 1\"\n\
         Restart=on-failure\n\
         RestartSec=50ms\n",
        counter_file.display()
    );
    env.write_unit("etc/systemd/system", "crasher.service", &unit);

    let _init = env.spawn_init(&["--no-bootstrap"]);
    let out = env.systemctl(&["start", "crasher.service"]);
    assert!(out.status.success());

    let ok = wait_until(Duration::from_secs(4), || {
        fs::read_to_string(&counter_file)
            .map(|s| s.lines().count() >= 3)
            .unwrap_or(false)
    });
    assert!(ok, "expected >= 3 runs, log:\n{}", env.read_log());

    // Stop must cancel restart scheduling
    let out = env.systemctl(&["stop", "crasher.service"]);
    assert!(out.status.success());
    let count_after_stop = fs::read_to_string(&counter_file)
        .unwrap_or_default()
        .lines()
        .count();
    thread::sleep(Duration::from_millis(350));
    let count_later = fs::read_to_string(&counter_file)
        .unwrap_or_default()
        .lines()
        .count();
    assert_eq!(count_after_stop, count_later, "restarts continued after stop");

    // Verify backoff growth in log (50ms -> 100ms -> 200ms)
    let log = env.read_log();
    assert!(log.contains("scheduling restart #1 for crasher.service in 50ms"), "log:\n{}", log);
    assert!(log.contains("scheduling restart #2 for crasher.service in 100ms"), "log:\n{}", log);
}

#[test]
fn restart_always_restarts_clean_exit_zero() {
    let env = TestEnv::new("restart-always");
    let counter_file = env.root.join("var/always-count.txt");
    let unit = format!(
        "[Service]\nType=simple\n\
         ExecStart=/bin/sh -c \"echo ok >> {}; exit 0\"\n\
         Restart=always\n\
         RestartSec=40ms\n",
        counter_file.display()
    );
    env.write_unit("etc/systemd/system", "always.service", &unit);

    let _init = env.spawn_init(&["--no-bootstrap"]);
    assert!(env.systemctl(&["start", "always.service"]).status.success());

    let ok = wait_until(Duration::from_secs(3), || {
        fs::read_to_string(&counter_file)
            .map(|s| s.lines().count() >= 2)
            .unwrap_or(false)
    });
    assert!(ok, "expected Restart=always to restart exit 0, log:\n{}", env.read_log());
    assert!(env.systemctl(&["stop", "always.service"]).status.success());
}

#[test]
fn dependency_cycle_rejected_via_systemctl_start() {
    let env = TestEnv::new("cycle");
    env.write_unit(
        "etc/systemd/system",
        "alpha.service",
        "[Unit]\nAfter=beta.service\nRequires=beta.service\n[Service]\nExecStart=/bin/sleep 30\n",
    );
    env.write_unit(
        "etc/systemd/system",
        "beta.service",
        "[Unit]\nAfter=alpha.service\nRequires=alpha.service\n[Service]\nExecStart=/bin/sleep 30\n",
    );

    let _init = env.spawn_init(&["--no-bootstrap"]);
    let out = env.systemctl(&["start", "alpha.service"]);
    assert!(!out.status.success(), "cycle must fail start");
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(stderr.contains("cycle"), "expected cycle error, got: {}", stderr);
}

#[test]
fn enable_disable_and_daemon_reload() {
    let env = TestEnv::new("enable-reload");
    env.write_unit(
        "lib/systemd/system",
        "worker.service",
        "[Unit]\nDescription=Worker\n[Service]\nExecStart=/bin/sleep 60\n[Install]\nWantedBy=multi-user.target\n",
    );

    let _init = env.spawn_init(&["--no-bootstrap"]);

    // Enable creates symlink in etc/systemd/system/multi-user.target.wants/
    let out = env.systemctl(&["enable", "worker.service"]);
    assert!(out.status.success(), "stderr: {}", String::from_utf8_lossy(&out.stderr));
    let link = env
        .root
        .join("etc/systemd/system/multi-user.target.wants/worker.service");
    assert!(link.symlink_metadata().is_ok(), "symlink not created");

    // Disable removes symlink
    let out = env.systemctl(&["disable", "worker.service"]);
    assert!(out.status.success());
    assert!(link.symlink_metadata().is_err(), "symlink still exists");

    // Add a new unit dynamically and reload
    env.write_unit(
        "etc/systemd/system",
        "dynamic.service",
        "[Unit]\nDescription=Dynamic\n[Service]\nExecStart=/bin/sleep 60\n",
    );
    assert_eq!(env.systemctl(&["status", "dynamic.service"]).status.code(), Some(4));

    let out = env.systemctl(&["daemon-reload"]);
    assert!(out.status.success());

    let out = env.systemctl(&["start", "dynamic.service"]);
    assert!(out.status.success());
    assert_eq!(env.systemctl(&["is-active", "dynamic.service"]).status.code(), Some(0));
}

#[test]
fn boot_order_dbus_elogind_app_and_sigterm_reverse_shutdown() {
    let env = TestEnv::new("boot-and-sigterm");
    let events_file = env.root.join("var/events.log");

    // dbus -> elogind -> app; each logs start via ExecStartPre and stop via ExecStop
    let dbus = format!(
        "[Unit]\nDescription=Fake DBus\n\
         [Service]\nType=simple\n\
         ExecStartPre=/bin/sh -c \"echo start:dbus >> {f}\"\n\
         ExecStart=/bin/sleep 60\n\
         ExecStop=/bin/sh -c \"echo stop:dbus >> {f}\"\n\
         [Install]\nWantedBy=multi-user.target\n",
        f = events_file.display()
    );
    let elogind = format!(
        "[Unit]\nDescription=Fake Elogind\nAfter=dbus.service\nRequires=dbus.service\n\
         [Service]\nType=dbus\nBusName=org.freedesktop.login1\n\
         ExecStartPre=/bin/sh -c \"echo start:elogind >> {f}\"\n\
         ExecStart=/bin/sleep 60\n\
         ExecStop=/bin/sh -c \"echo stop:elogind >> {f}\"\n\
         [Install]\nWantedBy=multi-user.target\n",
        f = events_file.display()
    );
    let app = format!(
        "[Unit]\nDescription=Fake App\nAfter=elogind.service\nRequires=elogind.service\n\
         [Service]\nType=notify\n\
         ExecStartPre=/bin/sh -c \"echo start:app >> {f}\"\n\
         ExecStart=/bin/sleep 60\n\
         ExecStop=/bin/sh -c \"echo stop:app >> {f}\"\n\
         [Install]\nWantedBy=multi-user.target\n",
        f = events_file.display()
    );

    env.write_unit("etc/systemd/system", "dbus.service", &dbus);
    env.write_unit("etc/systemd/system", "elogind.service", &elogind);
    env.write_unit("etc/systemd/system", "app.service", &app);

    // Offline enable before spawning lodos-init!
    assert!(env.systemctl(&["enable", "dbus.service", "elogind.service", "app.service"]).status.success());

    let mut init = env.spawn_init(&[]);

    // Wait for all three to be active
    let all_active = wait_until(Duration::from_secs(4), || {
        env.systemctl(&["is-active", "dbus.service"]).status.code() == Some(0)
            && env.systemctl(&["is-active", "elogind.service"]).status.code() == Some(0)
            && env.systemctl(&["is-active", "app.service"]).status.code() == Some(0)
    });
    assert!(all_active, "not all booted active, log:\n{}", env.read_log());

    // Send SIGTERM to lodos-init -> must stop in reverse order and exit 0
    let status = init.send_sigterm_and_wait();
    assert!(status.success(), "lodos-init did not exit 0 on SIGTERM: {:?}", status);
    assert!(!env.socket.exists(), "socket should be cleaned up on shutdown");

    let events: Vec<String> = fs::read_to_string(&events_file)
        .unwrap_or_default()
        .lines()
        .map(|s| s.to_string())
        .collect();
    assert_eq!(
        events,
        vec![
            "start:dbus",
            "start:elogind",
            "start:app",
            "stop:app",
            "stop:elogind",
            "stop:dbus",
        ],
        "wrong start/stop order, log:\n{}",
        env.read_log()
    );
}

#[test]
fn forking_service_and_zombie_reaping() {
    let env = TestEnv::new("forking-reap");
    let pidfile = env.root.join("run/forked.pid");

    // Type=forking: parent spawns background sleep, records its pid, and exits 0.
    // Also spawn a short-lived grandchild orphan to verify subreaper zombie reaping!
    let forker = format!(
        "[Unit]\nDescription=Forking Daemon\n\
         [Service]\nType=forking\n\
         PIDFile=/run/forked.pid\n\
         ExecStart=/bin/sh -c \"/bin/sleep 60 & echo $! > {}; /bin/true &\"\n",
        pidfile.display()
    );
    env.write_unit("etc/systemd/system", "forker.service", &forker);

    let _init = env.spawn_init(&["--no-bootstrap"]);
    let out = env.systemctl(&["start", "forker.service"]);
    assert!(out.status.success(), "stderr: {}", String::from_utf8_lossy(&out.stderr));

    assert_eq!(env.systemctl(&["is-active", "forker.service"]).status.code(), Some(0));
    let st = env.systemctl(&["status", "forker.service"]);
    let daemon_pid = extract_main_pid(&String::from_utf8_lossy(&st.stdout))
        .expect("forking Main PID from PIDFile");
    assert_eq!(unsafe { libc::kill(daemon_pid as libc::pid_t, 0) }, 0);

    // Wait briefly for the short-lived grandchild (/bin/true) to be reaped as an orphan
    let reaped = wait_until(Duration::from_secs(3), || {
        env.read_log().contains("reaped zombie/orphan")
    });
    assert!(reaped, "expected orphan zombie to be reaped, log:\n{}", env.read_log());

    // Stop forking service -> daemon_pid must be terminated
    assert!(env.systemctl(&["stop", "forker.service"]).status.success());
    let dead = wait_until(Duration::from_secs(2), || {
        unsafe { libc::kill(daemon_pid as libc::pid_t, 0) != 0 }
    });
    assert!(dead, "forking daemon pid {} still alive after stop", daemon_pid);
}

#[test]
fn user_directive_accepts_current_uid_and_rejects_unknown_user() {
    let env = TestEnv::new("user-directive");
    let uid = unsafe { libc::geteuid() };
    env.write_unit(
        "etc/systemd/system",
        "gooduser.service",
        &format!("[Service]\nType=oneshot\nUser={}\nExecStart=/bin/true\n", uid),
    );
    env.write_unit(
        "etc/systemd/system",
        "baduser.service",
        "[Service]\nType=oneshot\nUser=nonexistent_lodos_user_xyz\nExecStart=/bin/true\n",
    );

    let _init = env.spawn_init(&["--no-bootstrap"]);
    assert!(env.systemctl(&["start", "gooduser.service"]).status.success());
    assert!(!env.systemctl(&["start", "baduser.service"]).status.success());
}
