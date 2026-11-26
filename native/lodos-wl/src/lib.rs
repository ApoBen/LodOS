//! lodos-wl: minimal Wayland host server (S2 v0, T013/T022).
//! Globals: wl_compositor, wl_subcompositor, wl_shm, xdg_wm_base, wl_seat (no devices),
//! wl_output (1080x2400@60).
//! On wl_surface commit of a wl_shm buffer (XRGB8888/ARGB8888), locks ANativeWindow,
//! copies pixels (handling stride + channel order), and calls ANativeWindow_unlockAndPost.

use std::{
    ffi::{c_char, c_int, c_void, CStr, CString},
    os::unix::net::{SocketAddr, UnixListener, UnixStream},
    path::PathBuf,
    ptr::{self, NonNull},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::{self, SyncSender},
        Arc, Mutex, OnceLock,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

#[cfg(target_os = "android")]
use std::os::android::net::SocketAddrExt;
#[cfg(all(target_os = "linux", not(target_os = "android")))]
use std::os::linux::net::SocketAddrExt;

use jni_sys::{jobject, jstring, JNIEnv};
use smithay::{
    delegate_compositor, delegate_output, delegate_seat, delegate_shm, delegate_xdg_shell,
    input::{Seat, SeatHandler, SeatState},
    output::{Mode, Output, PhysicalProperties, Subpixel},
    reexports::{
        calloop::{
            generic::Generic,
            ping::{make_ping, Ping, PingSource},
            EventLoop, Interest, Mode as CMode, PostAction,
        },
        wayland_protocols::xdg::shell::server::xdg_toplevel,
        wayland_server::{
            backend::{ClientData, ClientId, DisconnectReason},
            protocol::{wl_buffer, wl_seat, wl_shm, wl_surface::WlSurface},
            Client, Display, DisplayHandle, ListeningSocket,
        },
    },
    utils::{Serial, Transform},
    wayland::{
        buffer::BufferHandler,
        compositor::{self, BufferAssignment, CompositorClientState, CompositorHandler, CompositorState, SurfaceAttributes},
        output::{OutputHandler, OutputManagerState},
        shell::xdg::{PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState},
        shm::{with_buffer_contents, BufferData, ShmHandler, ShmState},
    },
};

pub const DEFAULT_SOCK: &str = "/data/local/tmp/lodos/run/wayland-0";

// Android NDK ANativeWindow constants & FFI
pub const WINDOW_FORMAT_RGBA_8888: i32 = 1;
pub const WINDOW_FORMAT_RGBX_8888: i32 = 2;

#[repr(C)]
pub struct ANativeWindow {
    _priv: [u8; 0],
}

#[repr(C)]
pub struct ARect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

#[repr(C)]
pub struct ANativeWindow_Buffer {
    pub width: i32,
    pub height: i32,
    pub stride: i32, // in pixels
    pub format: i32,
    pub bits: *mut c_void,
    pub reserved: [u32; 6],
}

#[cfg(target_os = "android")]
extern "C" {
    fn ANativeWindow_fromSurface(env: *mut JNIEnv, surface: jobject) -> *mut ANativeWindow;
    fn ANativeWindow_acquire(window: *mut ANativeWindow);
    fn ANativeWindow_release(window: *mut ANativeWindow);
    fn ANativeWindow_setBuffersGeometry(
        window: *mut ANativeWindow,
        width: i32,
        height: i32,
        format: i32,
    ) -> i32;
    fn ANativeWindow_lock(
        window: *mut ANativeWindow,
        out_buffer: *mut ANativeWindow_Buffer,
        in_out_dirty_bounds: *mut ARect,
    ) -> i32;
    fn ANativeWindow_unlockAndPost(window: *mut ANativeWindow) -> i32;
    fn __android_log_write(prio: c_int, tag: *const c_char, text: *const c_char) -> c_int;
}

fn log_msg(msg: &str) {
    eprintln!("{msg}");
    #[cfg(target_os = "android")]
    {
        const ANDROID_LOG_INFO: c_int = 4;
        const TAG: &[u8] = b"LODOS_WL\0";
        if let Ok(cs) = CString::new(msg) {
            unsafe {
                __android_log_write(ANDROID_LOG_INFO, TAG.as_ptr() as *const c_char, cs.as_ptr());
            }
        }
    }
}

struct FrameCache {
    width: i32,
    height: i32,
    win_fmt: i32,
    rgba: Vec<u32>,
}

struct WinPresenter {
    #[cfg(target_os = "android")]
    win: Option<NonNull<ANativeWindow>>,
    geom: (i32, i32, i32),
    last_frame: Option<FrameCache>,
    frames_posted: u64,
    total_post_us: u64,
}

unsafe impl Send for WinPresenter {}

impl WinPresenter {
    fn new(win: Option<NonNull<ANativeWindow>>) -> Self {
        #[cfg(not(target_os = "android"))]
        let _ = win;
        Self {
            #[cfg(target_os = "android")]
            win,
            geom: (0, 0, 0),
            last_frame: None,
            frames_posted: 0,
            total_post_us: 0,
        }
    }

    fn set_window(&mut self, new_win: Option<NonNull<ANativeWindow>>) {
        #[cfg(target_os = "android")]
        {
            if let Some(old) = self.win.take() {
                unsafe { ANativeWindow_release(old.as_ptr()) };
            }
            self.win = new_win;
            self.geom = (0, 0, 0);
            if self.win.is_some() && self.last_frame.is_some() {
                self.post_cached_frame();
            }
        }
        #[cfg(not(target_os = "android"))]
        let _ = new_win;
    }

    fn ingest_and_post(&mut self, frame: FrameCache, shm_fmt: wl_shm::Format, src_stride: i32, convert_us: u64) {
        self.last_frame = Some(frame);
        let post_us = self.post_cached_frame();
        let n = self.frames_posted;
        if n == 1 || n % 120 == 0 {
            if let Some(f) = &self.last_frame {
                let avg_us = if n > 0 { self.total_post_us / n } else { 0 };
                log_msg(&format!(
                    "lodos-wl: present #{n} {}x{} fmt={shm_fmt:?} src_stride={src_stride} conv={convert_us}us post={post_us}us avg_post={avg_us}us",
                    f.width, f.height
                ));
            }
        }
    }

    fn post_cached_frame(&mut self) -> u64 {
        #[cfg(target_os = "android")]
        {
            let Some(win_nn) = self.win else { return 0 };
            let Some(frame) = &self.last_frame else { return 0 };
            let t0 = Instant::now();
            let win = win_nn.as_ptr();
            let target_geom = (frame.width, frame.height, frame.win_fmt);
            if self.geom != target_geom {
                let rc = unsafe {
                    ANativeWindow_setBuffersGeometry(win, frame.width, frame.height, frame.win_fmt)
                };
                if rc != 0 {
                    log_msg(&format!(
                        "lodos-wl: ANativeWindow_setBuffersGeometry({}x{}, fmt={}) failed rc={rc}",
                        frame.width, frame.height, frame.win_fmt
                    ));
                } else {
                    self.geom = target_geom;
                }
            }

            let mut buf = ANativeWindow_Buffer {
                width: 0,
                height: 0,
                stride: 0,
                format: 0,
                bits: ptr::null_mut(),
                reserved: [0; 6],
            };
            let rc = unsafe { ANativeWindow_lock(win, &mut buf, ptr::null_mut()) };
            if rc != 0 || buf.bits.is_null() {
                log_msg(&format!("lodos-wl: ANativeWindow_lock failed rc={rc}"));
                return 0;
            }

            let copy_w = frame.width.min(buf.width).max(0) as usize;
            let copy_h = frame.height.min(buf.height).max(0) as usize;
            let src_stride_px = frame.width as usize;
            let dst_stride_px = buf.stride.max(0) as usize;

            if copy_w > 0 && copy_h > 0 && dst_stride_px >= copy_w {
                let dst_base = buf.bits as *mut u32;
                let src_base = frame.rgba.as_ptr();
                unsafe {
                    if src_stride_px == dst_stride_px && copy_w == src_stride_px {
                        ptr::copy_nonoverlapping(src_base, dst_base, copy_w * copy_h);
                    } else {
                        for y in 0..copy_h {
                            ptr::copy_nonoverlapping(
                                src_base.add(y * src_stride_px),
                                dst_base.add(y * dst_stride_px),
                                copy_w,
                            );
                        }
                    }
                }
            }

            let unlock_rc = unsafe { ANativeWindow_unlockAndPost(win) };
            if unlock_rc != 0 {
                log_msg(&format!("lodos-wl: ANativeWindow_unlockAndPost failed rc={unlock_rc}"));
                return 0;
            }
            let elapsed_us = t0.elapsed().as_micros() as u64;
            self.frames_posted += 1;
            self.total_post_us += elapsed_us;
            elapsed_us
        }
        #[cfg(not(target_os = "android"))]
        {
            0
        }
    }
}

impl Drop for WinPresenter {
    fn drop(&mut self) {
        self.set_window(None);
    }
}

struct App {
    compositor_state: CompositorState,
    xdg_shell_state: XdgShellState,
    shm_state: ShmState,
    seat_state: SeatState<App>,
    _output_state: OutputManagerState,
    presenter: Option<Arc<Mutex<WinPresenter>>>,
    start_instant: Instant,
}

#[derive(Default)]
struct ClientState {
    compositor_state: CompositorClientState,
}

impl ClientData for ClientState {
    fn initialized(&self, id: ClientId) {
        log_msg(&format!("lodos-wl: client connected {id:?}"));
    }
    fn disconnected(&self, id: ClientId, reason: DisconnectReason) {
        log_msg(&format!("lodos-wl: client disconnected {id:?} ({reason:?})"));
    }
}

fn copy_shm_buffer(pool_ptr: *const u8, pool_len: usize, data: BufferData) -> Result<FrameCache, String> {
    if data.width <= 0 || data.height <= 0 || data.offset < 0 {
        return Err(format!("invalid dims {}x{} off={}", data.width, data.height, data.offset));
    }
    let w = data.width as usize;
    let h = data.height as usize;
    let row_bytes = w.checked_mul(4).ok_or("width overflow")?;
    if (data.stride as usize) < row_bytes {
        return Err(format!("stride {} < row_bytes {}", data.stride, row_bytes));
    }
    let src_stride = data.stride as usize;
    let off = data.offset as usize;
    let end = off
        .checked_add((h - 1).checked_mul(src_stride).ok_or("stride overflow")?)
        .and_then(|v| v.checked_add(row_bytes))
        .ok_or("end overflow")?;
    if end > pool_len {
        return Err(format!("buffer end {end} > pool_len {pool_len}"));
    }

    let (win_fmt, swizzle_rb, force_opaque) = match data.format {
        wl_shm::Format::Argb8888 => (WINDOW_FORMAT_RGBA_8888, true, false),
        wl_shm::Format::Xrgb8888 => (WINDOW_FORMAT_RGBX_8888, true, true),
        wl_shm::Format::Abgr8888 => (WINDOW_FORMAT_RGBA_8888, false, false),
        wl_shm::Format::Xbgr8888 => (WINDOW_FORMAT_RGBX_8888, false, true),
        other => return Err(format!("unsupported wl_shm format {other:?}")),
    };

    let total_px = w.checked_mul(h).ok_or("pixel count overflow")?;
    let mut rgba = vec![0u32; total_px];
    unsafe {
        for y in 0..h {
            let src_row = pool_ptr.add(off + y * src_stride);
            let dst_row = rgba.as_mut_ptr().add(y * w) as *mut u8;
            ptr::copy_nonoverlapping(src_row, dst_row, row_bytes);
        }
    }
    if swizzle_rb {
        for px in &mut rgba {
            let v = *px;
            let a = if force_opaque { 0xFF00_0000 } else { v & 0xFF00_0000 };
            *px = a | (v & 0x0000_FF00) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16);
        }
    } else if force_opaque {
        for px in &mut rgba {
            *px |= 0xFF00_0000;
        }
    }

    Ok(FrameCache {
        width: data.width,
        height: data.height,
        win_fmt,
        rgba,
    })
}

impl CompositorHandler for App {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor_state
    }
    fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
        &client.get_data::<ClientState>().unwrap().compositor_state
    }
    fn commit(&mut self, surface: &WlSurface) {
        let (buf_assign, frame_cbs) = compositor::with_states(surface, |states| {
            let mut cached = states.cached_state.get::<SurfaceAttributes>();
            let cur = cached.current();
            let buf = cur.buffer.take();
            cur.damage.clear();
            let cbs = std::mem::take(&mut cur.frame_callbacks);
            (buf, cbs)
        });

        if let Some(BufferAssignment::NewBuffer(buffer)) = buf_assign {
            if let Some(presenter_arc) = &self.presenter {
                let t0 = Instant::now();
                let res = with_buffer_contents(&buffer, |ptr, len, data| {
                    copy_shm_buffer(ptr, len, data).map(|fc| (fc, data.format, data.stride))
                });
                let conv_us = t0.elapsed().as_micros() as u64;
                match res {
                    Ok(Ok((fc, fmt, stride))) => {
                        if let Ok(mut guard) = presenter_arc.lock() {
                            guard.ingest_and_post(fc, fmt, stride, conv_us);
                        }
                    }
                    Ok(Err(e)) => log_msg(&format!("lodos-wl: shm copy skip: {e}")),
                    Err(e) => log_msg(&format!("lodos-wl: with_buffer_contents err: {e:?}")),
                }
            }
            // Copy path finished reading SHM pool; release buffer back to client immediately.
            buffer.release();
        }

        if !frame_cbs.is_empty() {
            let ms = self.start_instant.elapsed().as_millis() as u32;
            for cb in frame_cbs {
                cb.done(ms);
            }
        }
    }
}

impl BufferHandler for App {
    fn buffer_destroyed(&mut self, _buffer: &wl_buffer::WlBuffer) {}
}

impl ShmHandler for App {
    fn shm_state(&self) -> &ShmState {
        &self.shm_state
    }
}

impl XdgShellHandler for App {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }
    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        log_msg("lodos-wl: new xdg_toplevel");
        surface.with_pending_state(|s| {
            s.states.set(xdg_toplevel::State::Activated);
        });
        surface.send_configure();
    }
    fn new_popup(&mut self, _surface: PopupSurface, _positioner: PositionerState) {}
    fn grab(&mut self, _surface: PopupSurface, _seat: wl_seat::WlSeat, _serial: Serial) {}
    fn reposition_request(&mut self, _surface: PopupSurface, _positioner: PositionerState, _token: u32) {}
}

impl SeatHandler for App {
    type KeyboardFocus = WlSurface;
    type PointerFocus = WlSurface;
    type TouchFocus = WlSurface;
    fn seat_state(&mut self) -> &mut SeatState<Self> {
        &mut self.seat_state
    }
}

impl OutputHandler for App {}

delegate_compositor!(App);
delegate_shm!(App);
delegate_xdg_shell!(App);
delegate_seat!(App);
delegate_output!(App);

fn add_client(dh: &mut DisplayHandle, stream: UnixStream) {
    if let Err(e) = dh.insert_client(stream, Arc::new(ClientState::default())) {
        log_msg(&format!("lodos-wl: insert_client failed: {e}"));
    }
}

fn run_server(
    path: PathBuf,
    presenter: Option<Arc<Mutex<WinPresenter>>>,
    stop_ctl: Option<(Arc<AtomicBool>, PingSource)>,
    ready_tx: Option<SyncSender<Result<(), String>>>,
) -> Result<(), Box<dyn std::error::Error>> {
    let setup = (|| -> Result<_, String> {
        if path.as_os_str().len() >= 108 {
            return Err(format!("socket path too long (>=108): {}", path.display()));
        }
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| format!("mkdir {}: {e}", dir.display()))?;
        }
        let _ = std::fs::remove_file(&path);
        let _ = std::fs::remove_file(format!("{}.lock", path.display()));

        let event_loop: EventLoop<App> = EventLoop::try_new().map_err(|e| format!("EventLoop: {e}"))?;
        let display: Display<App> = Display::new().map_err(|e| format!("Display: {e}"))?;
        let dh = display.handle();

        let compositor_state = CompositorState::new::<App>(&dh);
        let shm_state = ShmState::new::<App>(
            &dh,
            vec![wl_shm::Format::Abgr8888, wl_shm::Format::Xbgr8888],
        );
        let xdg_shell_state = XdgShellState::new::<App>(&dh);
        let mut seat_state = SeatState::new();
        let _seat: Seat<App> = seat_state.new_wl_seat(&dh, "seat0");
        let output_state = OutputManagerState::new_with_xdg_output::<App>(&dh);

        let output = Output::new(
            "LODOS-1".into(),
            PhysicalProperties {
                size: (70, 155).into(),
                subpixel: Subpixel::Unknown,
                make: "LodOS".into(),
                model: "fake".into(),
            },
        );
        let mode = Mode {
            size: (1080, 2400).into(),
            refresh: 60_000,
        };
        output.create_global::<App>(&dh);
        output.change_current_state(Some(mode), Some(Transform::Normal), None, Some((0, 0).into()));
        output.set_preferred(mode);

        let handle = event_loop.handle();
        let mut listening = 0;
        match ListeningSocket::bind_absolute(path.clone()) {
            Ok(listener) => {
                log_msg(&format!("lodos-wl: listening on {}", path.display()));
                let mut dh_accept = dh.clone();
                handle
                    .insert_source(Generic::new(listener, Interest::READ, CMode::Level), move |_, l, _| {
                        while let Some(stream) = l.accept()? {
                            add_client(&mut dh_accept, stream);
                        }
                        Ok(PostAction::Continue)
                    })
                    .map_err(|e| format!("insert listener: {e}"))?;
                listening += 1;
            }
            Err(e) => log_msg(&format!("lodos-wl: path socket {} failed: {e:?}", path.display())),
        }

        let abs = format!(
            "lodos-{}",
            path.file_name().and_then(|n| n.to_str()).unwrap_or("wayland-0")
        );
        match SocketAddr::from_abstract_name(abs.as_bytes()).and_then(|a| UnixListener::bind_addr(&a)) {
            Ok(listener) => {
                listener
                    .set_nonblocking(true)
                    .map_err(|e| format!("set_nonblocking: {e}"))?;
                log_msg(&format!("lodos-wl: listening on abstract @{abs}"));
                let mut dh_accept = dh.clone();
                handle
                    .insert_source(Generic::new(listener, Interest::READ, CMode::Level), move |_, l, _| {
                        loop {
                            match l.accept() {
                                Ok((stream, _)) => add_client(&mut dh_accept, stream),
                                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                                Err(e) => return Err(e),
                            }
                        }
                        Ok(PostAction::Continue)
                    })
                    .map_err(|e| format!("insert abstract listener: {e}"))?;
                listening += 1;
            }
            Err(e) => log_msg(&format!("lodos-wl: abstract socket @{abs} failed: {e}")),
        }

        if listening == 0 {
            return Err("no listening socket".into());
        }

        handle
            .insert_source(Generic::new(display, Interest::READ, CMode::Level), |_, d, app| {
                // SAFETY: display is not dropped while the source is registered
                unsafe { d.get_mut().dispatch_clients(app).map_err(std::io::Error::other)? };
                Ok(PostAction::Continue)
            })
            .map_err(|e| format!("insert display: {e}"))?;

        let stop_flag = if let Some((flag, ping_src)) = stop_ctl {
            handle
                .insert_source(ping_src, |_, _, _| {})
                .map_err(|e| format!("insert ping: {e}"))?;
            Some(flag)
        } else {
            None
        };

        let app = App {
            compositor_state,
            xdg_shell_state,
            shm_state,
            seat_state,
            _output_state: output_state,
            presenter,
            start_instant: Instant::now(),
        };

        Ok((event_loop, dh, app, stop_flag))
    })();

    let (mut event_loop, mut dh_flush, mut app, stop_flag) = match setup {
        Ok(v) => {
            if let Some(tx) = ready_tx {
                let _ = tx.send(Ok(()));
            }
            v
        }
        Err(e) => {
            log_msg(&format!("lodos-wl: startup failed: {e}"));
            if let Some(tx) = ready_tx {
                let _ = tx.send(Err(e.clone()));
            }
            return Err(e.into());
        }
    };

    loop {
        if let Some(flag) = &stop_flag {
            if flag.load(Ordering::Acquire) {
                log_msg("lodos-wl: stopping event loop");
                break;
            }
        }
        event_loop.dispatch(Some(Duration::from_millis(50)), &mut app)?;
        let _ = dh_flush.flush_clients();
    }
    let _ = std::fs::remove_file(&path);
    Ok(())
}

/// CLI entrypoint used by `src/main.rs` (T013 headless Wayland host).
pub fn run_headless(path: PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    run_server(path, None, None, None)
}

// ---- JNI / C ABI server controller ----

struct ServerController {
    sock_path: PathBuf,
    presenter: Arc<Mutex<WinPresenter>>,
    stop_flag: Arc<AtomicBool>,
    ping: Ping,
    join: Option<JoinHandle<()>>,
}

impl ServerController {
    fn is_alive(&self) -> bool {
        self.join.as_ref().map(|j| !j.is_finished()).unwrap_or(false)
    }

    fn shutdown(&mut self) {
        self.stop_flag.store(true, Ordering::Release);
        self.ping.ping();
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
        if let Ok(mut g) = self.presenter.lock() {
            g.set_window(None);
        }
    }
}

static SERVER: OnceLock<Mutex<Option<ServerController>>> = OnceLock::new();

fn server_slot() -> &'static Mutex<Option<ServerController>> {
    SERVER.get_or_init(|| Mutex::new(None))
}

fn start_with_window(sock_path: PathBuf, win: Option<NonNull<ANativeWindow>>) -> Result<(), String> {
    let mut guard = server_slot().lock().map_err(|_| "server mutex poisoned".to_string())?;

    if let Some(ctrl) = guard.as_mut() {
        if ctrl.sock_path == sock_path && ctrl.is_alive() {
            log_msg(&format!("lodos-wl: updating ANativeWindow on active server {}", sock_path.display()));
            if let Ok(mut p) = ctrl.presenter.lock() {
                p.set_window(win);
            }
            return Ok(());
        }
        ctrl.shutdown();
        *guard = None;
    }

    let presenter = Arc::new(Mutex::new(WinPresenter::new(win)));
    let stop_flag = Arc::new(AtomicBool::new(false));
    let (ping, ping_src) = make_ping().map_err(|e| format!("make_ping: {e}"))?;
    let (ready_tx, ready_rx) = mpsc::sync_channel(1);

    let thread_path = sock_path.clone();
    let thread_presenter = Arc::clone(&presenter);
    let thread_stop = Arc::clone(&stop_flag);

    let join = thread::Builder::new()
        .name("lodos-wl".into())
        .spawn(move || {
            if let Err(e) = run_server(
                thread_path,
                Some(thread_presenter),
                Some((thread_stop, ping_src)),
                Some(ready_tx),
            ) {
                log_msg(&format!("lodos-wl: server exited with error: {e}"));
            }
        })
        .map_err(|e| format!("spawn thread: {e}"))?;

    match ready_rx.recv_timeout(Duration::from_secs(5)) {
        Ok(Ok(())) => {
            *guard = Some(ServerController {
                sock_path,
                presenter,
                stop_flag,
                ping,
                join: Some(join),
            });
            Ok(())
        }
        Ok(Err(e)) => {
            let _ = join.join();
            Err(e)
        }
        Err(e) => Err(format!("timeout waiting for server ready: {e}")),
    }
}

/// C ABI entry: `lodos_wl_start(socketPath, ANativeWindow*)`.
/// Acquires its own reference on `window` if non-null.
#[no_mangle]
pub unsafe extern "C" fn lodos_wl_start(
    socket_path: *const c_char,
    window: *mut ANativeWindow,
) -> c_int {
    if socket_path.is_null() {
        return -1;
    }
    let path_str = match CStr::from_ptr(socket_path).to_str() {
        Ok(s) => s,
        Err(_) => return -2,
    };
    let win_nn = NonNull::new(window);
    #[cfg(target_os = "android")]
    if let Some(w) = win_nn {
        ANativeWindow_acquire(w.as_ptr());
    }
    match start_with_window(PathBuf::from(path_str), win_nn) {
        Ok(()) => 0,
        Err(e) => {
            log_msg(&format!("lodos-wl: lodos_wl_start failed: {e}"));
            -3
        }
    }
}

unsafe fn jstring_to_string(env: *mut JNIEnv, s: jstring) -> Option<String> {
    if env.is_null() || s.is_null() {
        return None;
    }
    let iface = *env;
    if iface.is_null() {
        return None;
    }
    let ptr = ((*iface).v1_1.GetStringUTFChars)(env, s, ptr::null_mut());
    if ptr.is_null() {
        return None;
    }
    let out = CStr::from_ptr(ptr).to_string_lossy().into_owned();
    ((*iface).v1_1.ReleaseStringUTFChars)(env, s, ptr);
    Some(out)
}

/// JNI entry: `org.lodos.app.DisplayActivity.start(socketPath: String, surface: Surface)`
#[no_mangle]
pub unsafe extern "system" fn Java_org_lodos_app_DisplayActivity_start(
    env: *mut JNIEnv,
    _thiz: jobject,
    socket_path: jstring,
    surface: jobject,
) {
    let Some(path) = jstring_to_string(env, socket_path) else {
        log_msg("lodos-wl: JNI start got null socketPath");
        return;
    };
    #[cfg(target_os = "android")]
    let win_nn = if surface.is_null() {
        None
    } else {
        // ANativeWindow_fromSurface already acquires a reference on the returned ANativeWindow.
        NonNull::new(ANativeWindow_fromSurface(env, surface))
    };
    #[cfg(not(target_os = "android"))]
    let win_nn = {
        let _ = surface;
        None
    };

    if let Err(e) = start_with_window(PathBuf::from(&path), win_nn) {
        log_msg(&format!("lodos-wl: JNI start({path}) failed: {e}"));
    }
}

/// JNI entry: `org.lodos.app.DisplayActivity.releaseWindow()`
#[no_mangle]
pub unsafe extern "system" fn Java_org_lodos_app_DisplayActivity_releaseWindow(
    _env: *mut JNIEnv,
    _thiz: jobject,
) {
    if let Ok(guard) = server_slot().lock() {
        if let Some(ctrl) = guard.as_ref() {
            if let Ok(mut p) = ctrl.presenter.lock() {
                p.set_window(None);
            }
        }
    }
}

/// JNI entry: `org.lodos.app.DisplayActivity.stop()`
#[no_mangle]
pub unsafe extern "system" fn Java_org_lodos_app_DisplayActivity_stop(
    _env: *mut JNIEnv,
    _thiz: jobject,
) {
    if let Ok(mut guard) = server_slot().lock() {
        if let Some(mut ctrl) = guard.take() {
            ctrl.shutdown();
        }
    }
}
