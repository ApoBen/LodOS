#![cfg(feature = "headless")]
//! Integration tests for `lodos-wl` headless backend using `wayland-client`.

use std::{
    fs::OpenOptions,
    io::Write,
    os::{fd::AsFd, unix::net::UnixStream},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};

use lodos_wl::{HeadlessConfig, HeadlessServer};
use wayland_client::{
    protocol::{
        wl_buffer, wl_callback, wl_compositor, wl_keyboard, wl_output, wl_pointer, wl_registry,
        wl_seat, wl_shm, wl_shm_pool, wl_surface, wl_touch,
    },
    Connection, Dispatch, QueueHandle, WEnum,
};
use wayland_protocols::xdg::shell::client::{xdg_surface, xdg_toplevel, xdg_wm_base};

static TEST_SEQ: AtomicU64 = AtomicU64::new(0);

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(tag: &str) -> Self {
        let id = TEST_SEQ.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "lodos-wl-test-{}-{}-{}",
            tag,
            std::process::id(),
            id
        ));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).expect("create temp dir");
        Self { path }
    }

    fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.path);
    }
}

#[derive(Debug, Clone)]
enum TouchLog {
    Down { id: i32, x: f64, y: f64 },
    Motion { id: i32, x: f64, y: f64 },
    Up { id: i32 },
    Frame,
}

#[derive(Debug, Clone)]
enum PointerLog {
    Enter { x: f64, y: f64 },
    Motion { x: f64, y: f64 },
    Button { button: u32, pressed: bool },
    Frame,
}

#[derive(Debug, Clone)]
enum KeyLog {
    Keymap,
    Enter,
    Key { key: u32, pressed: bool },
}

#[derive(Default)]
struct ClientApp {
    globals: Vec<String>,
    compositor: Option<wl_compositor::WlCompositor>,
    shm: Option<wl_shm::WlShm>,
    shm_formats: Vec<wl_shm::Format>,
    xdg_wm_base: Option<xdg_wm_base::XdgWmBase>,
    seat: Option<wl_seat::WlSeat>,
    seat_caps: Option<wl_seat::Capability>,
    output: Option<wl_output::WlOutput>,
    output_modes: Vec<(i32, i32, i32)>,

    configured_serial: Option<u32>,
    ping_count: u32,
    frame_done: bool,
    touch_events: Vec<TouchLog>,
    pointer_events: Vec<PointerLog>,
    key_events: Vec<KeyLog>,
}

impl Dispatch<wl_registry::WlRegistry, ()> for ClientApp {
    fn event(
        state: &mut Self,
        reg: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            state.globals.push(interface.clone());
            match interface.as_str() {
                "wl_compositor" => {
                    state.compositor = Some(reg.bind(name, version.min(5), qh, ()));
                }
                "wl_shm" => {
                    state.shm = Some(reg.bind(name, version.min(1), qh, ()));
                }
                "xdg_wm_base" => {
                    state.xdg_wm_base = Some(reg.bind(name, version.min(5), qh, ()));
                }
                "wl_seat" => {
                    state.seat = Some(reg.bind(name, version.min(7), qh, ()));
                }
                "wl_output" => {
                    state.output = Some(reg.bind(name, version.min(4), qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<wl_compositor::WlCompositor, ()> for ClientApp {
    fn event(
        _: &mut Self,
        _: &wl_compositor::WlCompositor,
        _: wl_compositor::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_surface::WlSurface, ()> for ClientApp {
    fn event(
        _: &mut Self,
        _: &wl_surface::WlSurface,
        _: wl_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_shm::WlShm, ()> for ClientApp {
    fn event(
        state: &mut Self,
        _: &wl_shm::WlShm,
        event: wl_shm::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_shm::Event::Format {
            format: WEnum::Value(f),
        } = event
        {
            state.shm_formats.push(f);
        }
    }
}

impl Dispatch<wl_shm_pool::WlShmPool, ()> for ClientApp {
    fn event(
        _: &mut Self,
        _: &wl_shm_pool::WlShmPool,
        _: wl_shm_pool::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_buffer::WlBuffer, ()> for ClientApp {
    fn event(
        _: &mut Self,
        _: &wl_buffer::WlBuffer,
        _: wl_buffer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_callback::WlCallback, ()> for ClientApp {
    fn event(
        state: &mut Self,
        _: &wl_callback::WlCallback,
        event: wl_callback::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_callback::Event::Done { .. } = event {
            state.frame_done = true;
        }
    }
}

impl Dispatch<xdg_wm_base::XdgWmBase, ()> for ClientApp {
    fn event(
        state: &mut Self,
        wm: &xdg_wm_base::XdgWmBase,
        event: xdg_wm_base::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            state.ping_count += 1;
            wm.pong(serial);
        }
    }
}

impl Dispatch<xdg_surface::XdgSurface, ()> for ClientApp {
    fn event(
        state: &mut Self,
        xdg_surf: &xdg_surface::XdgSurface,
        event: xdg_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            xdg_surf.ack_configure(serial);
            state.configured_serial = Some(serial);
        }
    }
}

impl Dispatch<xdg_toplevel::XdgToplevel, ()> for ClientApp {
    fn event(
        _: &mut Self,
        _: &xdg_toplevel::XdgToplevel,
        _: xdg_toplevel::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_seat::WlSeat, ()> for ClientApp {
    fn event(
        state: &mut Self,
        _: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_seat::Event::Capabilities {
            capabilities: WEnum::Value(caps),
        } = event
        {
            state.seat_caps = Some(caps);
        }
    }
}

impl Dispatch<wl_touch::WlTouch, ()> for ClientApp {
    fn event(
        state: &mut Self,
        _: &wl_touch::WlTouch,
        event: wl_touch::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_touch::Event::Down { id, x, y, .. } => {
                state.touch_events.push(TouchLog::Down { id, x, y });
            }
            wl_touch::Event::Motion { id, x, y, .. } => {
                state.touch_events.push(TouchLog::Motion { id, x, y });
            }
            wl_touch::Event::Up { id, .. } => {
                state.touch_events.push(TouchLog::Up { id });
            }
            wl_touch::Event::Frame => {
                state.touch_events.push(TouchLog::Frame);
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for ClientApp {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_pointer::Event::Enter {
                surface_x,
                surface_y,
                ..
            } => {
                state.pointer_events.push(PointerLog::Enter {
                    x: surface_x,
                    y: surface_y,
                });
            }
            wl_pointer::Event::Motion {
                surface_x,
                surface_y,
                ..
            } => {
                state.pointer_events.push(PointerLog::Motion {
                    x: surface_x,
                    y: surface_y,
                });
            }
            wl_pointer::Event::Button {
                button,
                state: WEnum::Value(btn_state),
                ..
            } => {
                state.pointer_events.push(PointerLog::Button {
                    button,
                    pressed: btn_state == wl_pointer::ButtonState::Pressed,
                });
            }
            wl_pointer::Event::Frame => {
                state.pointer_events.push(PointerLog::Frame);
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_keyboard::WlKeyboard, ()> for ClientApp {
    fn event(
        state: &mut Self,
        _: &wl_keyboard::WlKeyboard,
        event: wl_keyboard::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_keyboard::Event::Keymap { .. } => {
                state.key_events.push(KeyLog::Keymap);
            }
            wl_keyboard::Event::Enter { .. } => {
                state.key_events.push(KeyLog::Enter);
            }
            wl_keyboard::Event::Key {
                key,
                state: WEnum::Value(ks),
                ..
            } => {
                state.key_events.push(KeyLog::Key {
                    key,
                    pressed: ks == wl_keyboard::KeyState::Pressed,
                });
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_output::WlOutput, ()> for ClientApp {
    fn event(
        state: &mut Self,
        _: &wl_output::WlOutput,
        event: wl_output::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_output::Event::Mode {
            width,
            height,
            refresh,
            ..
        } = event
        {
            state.output_modes.push((width, height, refresh));
        }
    }
}

fn create_solid_shm_buffer(
    dir: &Path,
    shm: &wl_shm::WlShm,
    qh: &QueueHandle<ClientApp>,
    w: i32,
    h: i32,
    pixel_le: u32,
    format: wl_shm::Format,
) -> (wl_shm_pool::WlShmPool, wl_buffer::WlBuffer) {
    let stride = w * 4;
    let size = (stride * h) as usize;
    let shm_path = dir.join(format!("shm-{}.bin", TEST_SEQ.fetch_add(1, Ordering::Relaxed)));
    let mut f = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(&shm_path)
        .expect("create shm file");
    let mut data = Vec::with_capacity(size);
    let bytes = pixel_le.to_le_bytes();
    for _ in 0..(w * h) {
        data.extend_from_slice(&bytes);
    }
    f.write_all(&data).expect("write shm pixels");
    let pool = shm.create_pool(f.as_fd(), size as i32, qh, ());
    let buf = pool.create_buffer(0, w, h, stride, format, qh, ());
    (pool, buf)
}

fn connect_path(path: &Path) -> Connection {
    let stream = UnixStream::connect(path).expect("connect to wayland socket");
    Connection::from_socket(stream).expect("wayland Connection::from_socket")
}

fn pump_until(
    queue: &mut wayland_client::EventQueue<ClientApp>,
    app: &mut ClientApp,
    timeout: Duration,
    cond: impl Fn(&ClientApp) -> bool,
) {
    let deadline = Instant::now() + timeout;
    while !cond(app) {
        assert!(Instant::now() < deadline, "timed out waiting for client condition");
        queue.roundtrip(app).expect("wayland roundtrip");
        if !cond(app) {
            std::thread::sleep(Duration::from_millis(5));
        }
    }
}

#[test]
fn toplevel_shm_green_and_touch_input() {
    let tmp = TempDir::new("green-touch");
    let sock = tmp.path().join("wayland-0");
    let mut cfg = HeadlessConfig::new(sock.clone());
    cfg.width = 256;
    cfg.height = 256;
    let srv = HeadlessServer::start(cfg).expect("start headless server");

    let conn = connect_path(srv.socket_path());
    let mut queue = conn.new_event_queue::<ClientApp>();
    let qh = queue.handle();
    let mut app = ClientApp::default();

    let display = conn.display();
    let _registry = display.get_registry(&qh, ());
    // First roundtrip discovers globals & sends bind requests; second delivers initial global events.
    queue.roundtrip(&mut app).expect("roundtrip 1");
    queue.roundtrip(&mut app).expect("roundtrip 2");

    for required in ["wl_compositor", "wl_shm", "xdg_wm_base", "wl_seat", "wl_output"] {
        assert!(
            app.globals.iter().any(|g| g == required),
            "missing required global {required}; got {:?}",
            app.globals
        );
    }
    assert!(app.shm_formats.contains(&wl_shm::Format::Argb8888));
    assert!(app.shm_formats.contains(&wl_shm::Format::Xrgb8888));
    let caps = app.seat_caps.expect("wl_seat capabilities");
    assert!(caps.contains(wl_seat::Capability::Touch));
    assert!(caps.contains(wl_seat::Capability::Pointer));
    assert!(caps.contains(wl_seat::Capability::Keyboard));
    assert!(app.output_modes.contains(&(256, 256, 60_000)));

    let compositor = app.compositor.clone().unwrap();
    let shm = app.shm.clone().unwrap();
    let wm_base = app.xdg_wm_base.clone().unwrap();
    let seat = app.seat.clone().unwrap();

    // Create xdg_toplevel and wait for configure + ack.
    let surface = compositor.create_surface(&qh, ());
    let xdg_surface = wm_base.get_xdg_surface(&surface, &qh, ());
    let toplevel = xdg_surface.get_toplevel(&qh, ());
    toplevel.set_title("green-test".into());
    surface.commit();

    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| {
        a.configured_serial.is_some()
    });
    // Flush ack_configure to server.
    queue.roundtrip(&mut app).expect("roundtrip ack");
    assert!(srv.stats().ack_configure_count >= 1);

    // Attach 64x64 solid green (ARGB8888 = 0xFF00FF00) and commit.
    let (_pool, buf) = create_solid_shm_buffer(
        tmp.path(),
        &shm,
        &qh,
        64,
        64,
        0xFF00_FF00,
        wl_shm::Format::Argb8888,
    );
    surface.attach(Some(&buf), 0, 0);
    surface.damage(0, 0, 64, 64);
    let _cb = surface.frame(&qh, ());
    surface.commit();

    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| a.frame_done);

    assert_eq!(srv.frame_count(), 1);
    assert_eq!(srv.pixel(0, 0), Some([0, 255, 0, 255]));
    assert_eq!(srv.pixel(32, 32), Some([0, 255, 0, 255]));
    assert_eq!(srv.pixel(63, 63), Some([0, 255, 0, 255]));
    // Outside the 64x64 toplevel stays opaque black.
    assert_eq!(srv.pixel(64, 64), Some([0, 0, 0, 255]));

    // Bind wl_touch and verify injected down / motion / up events arrive on client.
    let _touch = seat.get_touch(&qh, ());
    queue.roundtrip(&mut app).expect("roundtrip touch bind");

    srv.inject_touch_down(0, 12.5, 34.25);
    srv.inject_touch_motion(0, 20.0, 40.0);
    srv.inject_touch_up(0);

    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| {
        a.touch_events.iter().any(|e| matches!(e, TouchLog::Up { .. }))
    });

    assert!(
        app.touch_events.iter().any(|e| matches!(
            e,
            TouchLog::Down { id: 0, x, y } if (*x - 12.5).abs() < 0.01 && (*y - 34.25).abs() < 0.01
        )),
        "expected touch down at (12.5, 34.25), got {:?}",
        app.touch_events
    );
    assert!(
        app.touch_events.iter().any(|e| matches!(
            e,
            TouchLog::Motion { id: 0, x, y } if (*x - 20.0).abs() < 0.01 && (*y - 40.0).abs() < 0.01
        )),
        "expected touch motion at (20.0, 40.0), got {:?}",
        app.touch_events
    );
    assert!(app.touch_events.iter().any(|e| matches!(e, TouchLog::Up { id: 0 })));
    assert!(app.touch_events.iter().any(|e| matches!(e, TouchLog::Frame)));

    // Verify xdg_wm_base ping/pong roundtrip.
    srv.ping_clients();
    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| a.ping_count >= 1);
    queue.roundtrip(&mut app).expect("roundtrip pong");
    assert!(srv.stats().pong_count >= 1);
}

#[test]
fn pointer_keyboard_input_and_ppm_dump() {
    let tmp = TempDir::new("ptr-kbd-ppm");
    let sock = tmp.path().join("wayland-0");
    let auto_ppm = tmp.path().join("auto_frame1.ppm");

    let mut cfg = HeadlessConfig::new(sock.clone());
    cfg.width = 128;
    cfg.height = 128;
    cfg.dump_path = Some(auto_ppm.clone());
    cfg.dump_after_frames = Some(1);
    let srv = HeadlessServer::start(cfg).expect("start headless server");

    let conn = connect_path(srv.socket_path());
    let mut queue = conn.new_event_queue::<ClientApp>();
    let qh = queue.handle();
    let mut app = ClientApp::default();

    let display = conn.display();
    let _registry = display.get_registry(&qh, ());
    queue.roundtrip(&mut app).expect("roundtrip 1");
    queue.roundtrip(&mut app).expect("roundtrip 2");

    let compositor = app.compositor.clone().unwrap();
    let shm = app.shm.clone().unwrap();
    let wm_base = app.xdg_wm_base.clone().unwrap();
    let seat = app.seat.clone().unwrap();

    let _pointer = seat.get_pointer(&qh, ());
    let _keyboard = seat.get_keyboard(&qh, ());

    let surface = compositor.create_surface(&qh, ());
    let xdg_surface = wm_base.get_xdg_surface(&surface, &qh, ());
    let _toplevel = xdg_surface.get_toplevel(&qh, ());
    surface.commit();

    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| {
        a.configured_serial.is_some()
    });

    // Commit 64x64 solid red using XRGB8888 with alpha byte 0x00 (must become opaque red).
    let (_pool, buf) = create_solid_shm_buffer(
        tmp.path(),
        &shm,
        &qh,
        64,
        64,
        0x00FF_0000,
        wl_shm::Format::Xrgb8888,
    );
    surface.attach(Some(&buf), 0, 0);
    surface.damage(0, 0, 64, 64);
    let _cb = surface.frame(&qh, ());
    surface.commit();
    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| a.frame_done);

    assert_eq!(srv.pixel(10, 10), Some([255, 0, 0, 255]));
    assert_eq!(srv.pixel(100, 100), Some([0, 0, 0, 255]));

    // Verify auto-dumped PPM after 1st frame.
    let ppm_bytes = std::fs::read(&auto_ppm).expect("read auto-dumped PPM");
    let hdr = b"P6\n128 128\n255\n";
    assert!(ppm_bytes.starts_with(hdr));
    assert_eq!(ppm_bytes.len(), hdr.len() + 128 * 128 * 3);
    let p_red = hdr.len() + (10 * 128 + 10) * 3;
    let p_blk = hdr.len() + (100 * 128 + 100) * 3;
    assert_eq!(&ppm_bytes[p_red..p_red + 3], &[255, 0, 0]);
    assert_eq!(&ppm_bytes[p_blk..p_blk + 3], &[0, 0, 0]);

    // Inject pointer motion + button click (BTN_LEFT = 0x110).
    srv.inject_pointer_motion(15.0, 25.0);
    srv.inject_pointer_motion(30.0, 40.0);
    srv.inject_pointer_button(0x110, true);
    srv.inject_pointer_button(0x110, false);

    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| {
        a.pointer_events.iter().any(|e| {
            matches!(
                e,
                PointerLog::Button {
                    button: 0x110,
                    pressed: false
                }
            )
        })
    });
    assert!(app.pointer_events.iter().any(|e| matches!(
        e,
        PointerLog::Enter { x, y } if (*x - 15.0).abs() < 0.01 && (*y - 25.0).abs() < 0.01
    )));
    assert!(app.pointer_events.iter().any(|e| matches!(
        e,
        PointerLog::Motion { x, y } if (*x - 30.0).abs() < 0.01 && (*y - 40.0).abs() < 0.01
    )));
    assert!(app.pointer_events.iter().any(|e| matches!(
        e,
        PointerLog::Button {
            button: 0x110,
            pressed: true
        }
    )));
    assert!(app.pointer_events.iter().any(|e| matches!(e, PointerLog::Frame)));

    // Inject keyboard key press + release (evdev KEY_A = 30).
    srv.inject_key(30, true);
    srv.inject_key(30, false);

    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| {
        a.key_events
            .iter()
            .any(|e| matches!(e, KeyLog::Key { key: 30, pressed: false }))
    });
    assert!(app.key_events.iter().any(|e| matches!(e, KeyLog::Keymap)));
    assert!(app.key_events.iter().any(|e| matches!(e, KeyLog::Enter)));
    assert!(app
        .key_events
        .iter()
        .any(|e| matches!(e, KeyLog::Key { key: 30, pressed: true })));
}

#[test]
fn cli_sigusr1_ppm_dump_and_wayland_info() {
    let tmp = TempDir::new("cli-sigusr1");
    let dump_ppm = tmp.path().join("sigusr1.ppm");
    let sock = tmp.path().join("wayland-0");

    let bin = env!("CARGO_BIN_EXE_lodos-wl");
    let child = Command::new(bin)
        .env("XDG_RUNTIME_DIR", tmp.path())
        .env("WAYLAND_DISPLAY", "wayland-0")
        .env("LODOS_WL_WIDTH", "64")
        .env("LODOS_WL_HEIGHT", "64")
        .env("LODOS_WL_DUMP", &dump_ppm)
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn lodos-wl binary");

    struct KillOnDrop(std::process::Child);
    impl Drop for KillOnDrop {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    let _guard = KillOnDrop(child);

    // Wait for socket to appear.
    let deadline = Instant::now() + Duration::from_secs(5);
    while !sock.exists() {
        assert!(Instant::now() < deadline, "lodos-wl did not bind {}", sock.display());
        std::thread::sleep(Duration::from_millis(10));
    }

    // Optional: run wayland-info if installed on PC.
    if let Ok(output) = Command::new("wayland-info")
        .env("XDG_RUNTIME_DIR", tmp.path())
        .env("WAYLAND_DISPLAY", "wayland-0")
        .output()
    {
        assert!(
            output.status.success(),
            "wayland-info failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        let stdout = String::from_utf8_lossy(&output.stdout);
        for iface in ["wl_compositor", "wl_shm", "xdg_wm_base", "wl_seat", "wl_output"] {
            assert!(stdout.contains(iface), "wayland-info missing {iface}:\n{stdout}");
        }
        for cap in ["pointer", "keyboard", "touch"] {
            assert!(stdout.contains(cap), "wayland-info missing seat cap {cap}:\n{stdout}");
        }
    }

    // Commit a 64x64 solid blue buffer, then trigger SIGUSR1 PPM dump.
    let conn = connect_path(&sock);
    let mut queue = conn.new_event_queue::<ClientApp>();
    let qh = queue.handle();
    let mut app = ClientApp::default();

    let _reg = conn.display().get_registry(&qh, ());
    queue.roundtrip(&mut app).expect("roundtrip 1");
    queue.roundtrip(&mut app).expect("roundtrip 2");

    let compositor = app.compositor.clone().unwrap();
    let shm = app.shm.clone().unwrap();
    let wm_base = app.xdg_wm_base.clone().unwrap();

    let surface = compositor.create_surface(&qh, ());
    let xdg_surface = wm_base.get_xdg_surface(&surface, &qh, ());
    let _toplevel = xdg_surface.get_toplevel(&qh, ());
    surface.commit();
    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| {
        a.configured_serial.is_some()
    });

    let (_pool, buf) = create_solid_shm_buffer(
        tmp.path(),
        &shm,
        &qh,
        64,
        64,
        0xFF00_00FF, // ARGB8888 solid blue
        wl_shm::Format::Argb8888,
    );
    surface.attach(Some(&buf), 0, 0);
    let _cb = surface.frame(&qh, ());
    surface.commit();
    pump_until(&mut queue, &mut app, Duration::from_secs(3), |a| a.frame_done);

    // Send SIGUSR1 to lodos-wl child.
    let pid = _guard.0.id() as libc::pid_t;
    let rc = unsafe { libc::kill(pid, libc::SIGUSR1) };
    assert_eq!(rc, 0, "kill(SIGUSR1) failed");

    let dump_deadline = Instant::now() + Duration::from_secs(3);
    while !dump_ppm.exists() {
        assert!(Instant::now() < dump_deadline, "SIGUSR1 did not produce PPM");
        std::thread::sleep(Duration::from_millis(15));
    }

    let ppm = std::fs::read(&dump_ppm).expect("read SIGUSR1 PPM");
    let hdr = b"P6\n64 64\n255\n";
    assert!(ppm.starts_with(hdr));
    assert_eq!(ppm.len(), hdr.len() + 64 * 64 * 3);
    assert_eq!(&ppm[hdr.len()..hdr.len() + 3], &[0, 0, 255]);
}
