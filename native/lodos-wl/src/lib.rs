//! lodos-wl: minimal Wayland host server (S2: Android SurfaceView + PC headless backend).
//!
//! Globals advertised:
//! - `wl_compositor` (+ `wl_subcompositor`)
//! - `wl_shm` (`ARGB8888`, `XRGB8888`, `ABGR8888`, `XBGR8888`)
//! - `xdg_wm_base` (configure/ack, ping/pong)
//! - `wl_seat` (pointer + touch + keyboard)
//! - `wl_output` (+ `zxdg_output_manager_v1`)
//!
//! Backends:
//! - Android (`target_os = "android"`): copies committed SHM buffers to `ANativeWindow` via JNI.
//! - Headless (`feature = "headless"`): composites toplevel SHM buffers into an in-memory RGBA8888
//!   `Framebuffer`, with PPM dump hook (`LODOS_WL_DUMP` on `SIGUSR1` or after N frames) and
//!   synthetic input injection API for automated testing.

#[cfg(feature = "headless")]
pub mod framebuffer;
#[cfg(feature = "headless")]
pub mod ctl;

#[cfg(feature = "headless")]
pub use framebuffer::Framebuffer;

use std::{
    ffi::{c_char, c_int, c_void, CStr},
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
use std::{
    ffi::CString,
    os::{
        android::net::SocketAddrExt,
        unix::net::{SocketAddr, UnixListener},
    },
};

use std::os::unix::net::UnixStream;

use jni_sys::{jobject, jstring, JNIEnv};
use smithay::{
    backend::input::{ButtonState, KeyState, TouchSlot},
    delegate_compositor, delegate_output, delegate_seat, delegate_shm, delegate_xdg_decoration,
    delegate_xdg_shell,
    input::{
        keyboard::{FilterResult, Keycode, XkbConfig},
        pointer::{ButtonEvent, MotionEvent as PointerMotionEvent},
        touch::{DownEvent as TouchDownEvent, MotionEvent as TouchMotionEvent, UpEvent as TouchUpEvent},
        Seat, SeatHandler, SeatState,
    },
    output::{Mode, Output, PhysicalProperties, Subpixel},
    reexports::{
        calloop::{
            channel::{Channel, Event as ChannelEvent},
            generic::Generic,
            ping::{make_ping, Ping, PingSource},
            EventLoop, Interest, Mode as CMode, PostAction,
        },
        wayland_protocols::xdg::{
            decoration::zv1::server::zxdg_toplevel_decoration_v1::Mode as DecorationMode,
            shell::server::xdg_toplevel,
        },
        wayland_server::{
            backend::{ClientData, ClientId, DisconnectReason},
            protocol::{wl_buffer, wl_output, wl_seat, wl_shm, wl_surface::WlSurface},
            Client, Display, DisplayHandle, ListeningSocket,
        },
    },
    utils::{Point, Serial, Transform, SERIAL_COUNTER},
    wayland::{
        buffer::BufferHandler,
        compositor::{
            self, BufferAssignment, CompositorClientState, CompositorHandler, CompositorState,
            SurfaceAttributes,
        },
        output::{OutputHandler, OutputManagerState},
        shell::xdg::decoration::{XdgDecorationHandler, XdgDecorationState},
        shell::xdg::{
            Configure, PopupSurface, PositionerState, ShellClient, ToplevelSurface, XdgShellHandler,
            XdgShellState,
        },
        shm::{with_buffer_contents, BufferData, ShmHandler, ShmState},
    },
};

pub const DEFAULT_SOCK: &str = "/data/local/tmp/lodos/run/wayland-0";
pub const DEFAULT_WIDTH: u32 = 1080;
pub const DEFAULT_HEIGHT: u32 = 2400;

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

/// Control messages injected into the Wayland server event loop (input, ping, PPM dump).
#[derive(Debug, Clone)]
pub enum ControlMsg {
    TouchDown { slot: u32, x: f64, y: f64 },
    TouchMotion { slot: u32, x: f64, y: f64 },
    TouchUp { slot: u32 },
    TouchCancel,
    PointerMotion { x: f64, y: f64 },
    PointerButton { button: u32, pressed: bool },
    /// Linux evdev keycode (e.g. `KEY_A = 30`); Smithay XKB offset (+8) is applied internally.
    Key { evdev_code: u32, pressed: bool },
    PingClients,
    #[cfg(feature = "headless")]
    DumpPpm(PathBuf),
}

/// Runtime protocol counters exposed to tests and diagnostics.
#[derive(Debug, Default, Clone, Copy)]
pub struct ServerStats {
    pub ack_configure_count: u64,
    pub pong_count: u64,
    pub toplevel_count: u64,
    pub frame_count: u64,
    pub dump_count: u64,
}

#[cfg(feature = "headless")]
static SIGUSR1_PENDING: AtomicBool = AtomicBool::new(false);

#[cfg(feature = "headless")]
extern "C" fn handle_sigusr1(_sig: c_int) {
    SIGUSR1_PENDING.store(true, Ordering::Release);
}

#[cfg(feature = "headless")]
pub fn install_sigusr1_handler() {
    unsafe {
        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = handle_sigusr1 as *const () as usize;
        sa.sa_flags = libc::SA_RESTART;
        libc::sigemptyset(&mut sa.sa_mask);
        libc::sigaction(libc::SIGUSR1, &sa, ptr::null_mut());
    }
}

/// Resolves the default Wayland socket path using `$XDG_RUNTIME_DIR` and `$WAYLAND_DISPLAY`
/// on non-Android platforms (falling back to `DEFAULT_SOCK`).
pub fn resolve_default_socket_path() -> PathBuf {
    #[cfg(not(target_os = "android"))]
    {
        let display = std::env::var("WAYLAND_DISPLAY").unwrap_or_else(|_| "wayland-0".into());
        let display_path = PathBuf::from(&display);
        if display_path.is_absolute() {
            return display_path;
        }
        if let Ok(xdg) = std::env::var("XDG_RUNTIME_DIR") {
            if !xdg.is_empty() {
                return PathBuf::from(xdg).join(display);
            }
        }
    }
    PathBuf::from(DEFAULT_SOCK)
}

struct FrameCache {
    width: i32,
    height: i32,
    #[cfg_attr(not(target_os = "android"), allow(dead_code))]
    win_fmt: i32,
    #[cfg_attr(not(feature = "headless"), allow(dead_code))]
    opaque: bool,
    rgba: Vec<u32>,
}

impl FrameCache {
    #[cfg(feature = "headless")]
    fn to_rgba_bytes(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.rgba.len() * 4);
        for px in &self.rgba {
            out.extend_from_slice(&px.to_le_bytes());
        }
        out
    }
}

struct WinPresenter {
    #[cfg(target_os = "android")]
    win: Option<NonNull<ANativeWindow>>,
    #[cfg_attr(not(target_os = "android"), allow(dead_code))]
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

    fn ingest_and_post(
        &mut self,
        frame: FrameCache,
        shm_fmt: wl_shm::Format,
        src_stride: i32,
        convert_us: u64,
    ) {
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

#[cfg(feature = "headless")]
struct SurfacePixels {
    width: u32,
    height: u32,
    rgba: Vec<u8>,
    opaque: bool,
}

#[cfg(feature = "headless")]
struct HeadlessBackend {
    fb: Arc<Mutex<Framebuffer>>,
    dump_path: Option<PathBuf>,
    dump_after_frames: Option<u64>,
    install_sigusr1: bool,
    ctl_path: Option<PathBuf>,
}

struct ToplevelLayer {
    toplevel: ToplevelSurface,
    position: (i32, i32),
    size: (i32, i32),
    #[cfg(feature = "headless")]
    pixels: Option<SurfacePixels>,
}

struct App {
    compositor_state: CompositorState,
    xdg_shell_state: XdgShellState,
    shm_state: ShmState,
    seat_state: SeatState<App>,
    seat: Seat<App>,
    _output_state: OutputManagerState,
    _xdg_decoration_state: XdgDecorationState,
    output: Output,
    output_size: (i32, i32),
    presenter: Option<Arc<Mutex<WinPresenter>>>,
    #[cfg(feature = "headless")]
    headless: Option<HeadlessBackend>,
    layers: Vec<ToplevelLayer>,
    shell_clients: Vec<ShellClient>,
    pointer_pos: Point<f64, smithay::utils::Logical>,
    stats: Arc<Mutex<ServerStats>>,
    start_instant: Instant,
}

impl App {
    fn timestamp_ms(&self) -> u32 {
        self.start_instant.elapsed().as_millis() as u32
    }

    fn surface_at(&self, pos: Point<f64, smithay::utils::Logical>) -> Option<(WlSurface, Point<f64, smithay::utils::Logical>)> {
        // Hit-test mapped toplevels top-to-bottom.
        for layer in self.layers.iter().rev() {
            let (ox, oy) = layer.position;
            let (w, h) = layer.size;
            if w > 0 && h > 0 {
                let rx = pos.x - ox as f64;
                let ry = pos.y - oy as f64;
                if rx >= 0.0 && ry >= 0.0 && rx < w as f64 && ry < h as f64 {
                    return Some((layer.toplevel.wl_surface().clone(), (ox as f64, oy as f64).into()));
                }
            }
        }
        // Fallback to top toplevel if it has not committed a buffer yet.
        self.layers
            .last()
            .map(|l| (l.toplevel.wl_surface().clone(), (l.position.0 as f64, l.position.1 as f64).into()))
    }

    fn focus_surface(&mut self, surface: Option<WlSurface>) {
        if let Some(kbd) = self.seat.get_keyboard() {
            let serial = SERIAL_COUNTER.next_serial();
            kbd.set_focus(self, surface, serial);
        }
    }

    #[cfg(feature = "headless")]
    fn recomposite_headless(&mut self, count_frame: bool) {
        let Some(hb) = &self.headless else { return };
        let frame_num = {
            let Ok(mut fb) = hb.fb.lock() else { return };
            fb.clear([0, 0, 0, 255]);
            for layer in &self.layers {
                if let Some(px) = &layer.pixels {
                    fb.blit(
                        layer.position.0,
                        layer.position.1,
                        px.width,
                        px.height,
                        &px.rgba,
                        px.opaque,
                    );
                }
            }
            if count_frame {
                fb.inc_frame_count()
            } else {
                fb.frame_count()
            }
        };

        if count_frame {
            if let Ok(mut st) = self.stats.lock() {
                st.frame_count = frame_num;
            }
            if let (Some(path), Some(after)) = (&hb.dump_path, hb.dump_after_frames) {
                if frame_num == after {
                    let path = path.clone();
                    self.dump_headless_ppm(&path);
                }
            }
        }
    }

    #[cfg(feature = "headless")]
    fn dump_headless_ppm(&mut self, path: &std::path::Path) {
        let Some(hb) = &self.headless else { return };
        let res = hb.fb.lock().map(|fb| fb.write_ppm(path));
        match res {
            Ok(Ok(())) => {
                if let Ok(mut st) = self.stats.lock() {
                    st.dump_count += 1;
                }
                log_msg(&format!("lodos-wl: dumped PPM to {}", path.display()));
            }
            Ok(Err(e)) => log_msg(&format!("lodos-wl: PPM dump to {} failed: {e}", path.display())),
            Err(_) => log_msg("lodos-wl: framebuffer mutex poisoned during PPM dump"),
        }
    }

    fn handle_control(&mut self, msg: ControlMsg) {
        let serial = SERIAL_COUNTER.next_serial();
        let time = self.timestamp_ms();
        match msg {
            ControlMsg::TouchDown { slot, x, y } => {
                let loc: Point<f64, smithay::utils::Logical> = (x, y).into();
                let focus = self.surface_at(loc);
                if let Some((surf, _)) = &focus {
                    self.focus_surface(Some(surf.clone()));
                }
                if let Some(touch) = self.seat.get_touch() {
                    touch.down(
                        self,
                        focus,
                        &TouchDownEvent {
                            slot: TouchSlot::from(Some(slot)),
                            location: loc,
                            serial,
                            time,
                        },
                    );
                    touch.frame(self);
                }
            }
            ControlMsg::TouchMotion { slot, x, y } => {
                let loc: Point<f64, smithay::utils::Logical> = (x, y).into();
                let focus = self.surface_at(loc);
                if let Some(touch) = self.seat.get_touch() {
                    touch.motion(
                        self,
                        focus,
                        &TouchMotionEvent {
                            slot: TouchSlot::from(Some(slot)),
                            location: loc,
                            time,
                        },
                    );
                    touch.frame(self);
                }
            }
            ControlMsg::TouchUp { slot } => {
                if let Some(touch) = self.seat.get_touch() {
                    touch.up(
                        self,
                        &TouchUpEvent {
                            slot: TouchSlot::from(Some(slot)),
                            serial,
                            time,
                        },
                    );
                    touch.frame(self);
                }
            }
            ControlMsg::TouchCancel => {
                if let Some(touch) = self.seat.get_touch() {
                    touch.cancel(self);
                }
            }
            ControlMsg::PointerMotion { x, y } => {
                let loc: Point<f64, smithay::utils::Logical> = (x, y).into();
                self.pointer_pos = loc;
                let focus = self.surface_at(loc);
                if let Some(ptr) = self.seat.get_pointer() {
                    ptr.motion(
                        self,
                        focus,
                        &PointerMotionEvent {
                            location: loc,
                            serial,
                            time,
                        },
                    );
                    ptr.frame(self);
                }
            }
            ControlMsg::PointerButton { button, pressed } => {
                if pressed {
                    if let Some((surf, _)) = self.surface_at(self.pointer_pos) {
                        self.focus_surface(Some(surf));
                    }
                }
                if let Some(ptr) = self.seat.get_pointer() {
                    let state = if pressed {
                        ButtonState::Pressed
                    } else {
                        ButtonState::Released
                    };
                    ptr.button(
                        self,
                        &ButtonEvent {
                            serial,
                            time,
                            button,
                            state,
                        },
                    );
                    ptr.frame(self);
                }
            }
            ControlMsg::Key { evdev_code, pressed } => {
                if let Some(kbd) = self.seat.get_keyboard() {
                    if kbd.current_focus().is_none() {
                        if let Some(top) = self.layers.last() {
                            let surf = top.toplevel.wl_surface().clone();
                            kbd.set_focus(self, Some(surf), SERIAL_COUNTER.next_serial());
                        }
                    }
                    let state = if pressed {
                        KeyState::Pressed
                    } else {
                        KeyState::Released
                    };
                    // Smithay expects XKB keycode (evdev + 8) and subtracts 8 on wl_keyboard.key.
                    kbd.input(
                        self,
                        Keycode::new(evdev_code.saturating_add(8)),
                        state,
                        serial,
                        time,
                        |_, _, _| FilterResult::<()>::Forward,
                    );
                }
            }
            ControlMsg::PingClients => {
                for client in &self.shell_clients {
                    let _ = client.send_ping(SERIAL_COUNTER.next_serial());
                }
            }
            #[cfg(feature = "headless")]
            ControlMsg::DumpPpm(path) => {
                self.dump_headless_ppm(&path);
            }
        }
    }
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
        opaque: force_opaque,
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

        match buf_assign {
            Some(BufferAssignment::NewBuffer(buffer)) => {
                #[cfg(feature = "headless")]
                let need_copy = self.presenter.is_some() || self.headless.is_some();
                #[cfg(not(feature = "headless"))]
                let need_copy = self.presenter.is_some();

                if need_copy {
                    let t0 = Instant::now();
                    let res = with_buffer_contents(&buffer, |ptr, len, data| {
                        copy_shm_buffer(ptr, len, data).map(|fc| (fc, data.format, data.stride))
                    });
                    let conv_us = t0.elapsed().as_micros() as u64;
                    match res {
                        Ok(Ok((fc, fmt, stride))) => {
                            let (bw, bh) = (fc.width, fc.height);
                            #[cfg(feature = "headless")]
                            let headless_px = if self.headless.is_some() {
                                Some(SurfacePixels {
                                    width: fc.width as u32,
                                    height: fc.height as u32,
                                    rgba: fc.to_rgba_bytes(),
                                    opaque: fc.opaque,
                                })
                            } else {
                                None
                            };

                            if let Some(layer) =
                                self.layers.iter_mut().find(|l| l.toplevel.wl_surface() == surface)
                            {
                                layer.size = (bw, bh);
                                #[cfg(feature = "headless")]
                                {
                                    layer.pixels = headless_px;
                                }
                            }

                            if let Some(presenter_arc) = &self.presenter {
                                if let Ok(mut guard) = presenter_arc.lock() {
                                    guard.ingest_and_post(fc, fmt, stride, conv_us);
                                }
                            }

                            #[cfg(feature = "headless")]
                            if self.headless.is_some() {
                                self.recomposite_headless(true);
                            }
                        }
                        Ok(Err(e)) => log_msg(&format!("lodos-wl: shm copy skip: {e}")),
                        Err(e) => log_msg(&format!("lodos-wl: with_buffer_contents err: {e:?}")),
                    }
                }
                // Copy path finished reading SHM pool; release buffer back to client immediately.
                buffer.release();
            }
            Some(BufferAssignment::Removed) => {
                if let Some(layer) =
                    self.layers.iter_mut().find(|l| l.toplevel.wl_surface() == surface)
                {
                    layer.size = (0, 0);
                    #[cfg(feature = "headless")]
                    {
                        layer.pixels = None;
                    }
                }
                #[cfg(feature = "headless")]
                if self.headless.is_some() {
                    self.recomposite_headless(false);
                }
            }
            None => {}
        }

        if !frame_cbs.is_empty() {
            let ms = self.timestamp_ms();
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
    fn new_client(&mut self, client: ShellClient) {
        self.shell_clients.push(client);
    }
    fn client_destroyed(&mut self, client: ShellClient) {
        self.shell_clients.retain(|c| c != &client);
    }
    fn client_pong(&mut self, _client: ShellClient) {
        if let Ok(mut st) = self.stats.lock() {
            st.pong_count += 1;
        }
    }
    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        log_msg("lodos-wl: new xdg_toplevel");
        self.output.enter(surface.wl_surface());
        // Phone-style: every toplevel fills the output. Nested compositors (wlroots wayland
        // backend, e.g. phoc) size their output from this configure; 0x0 -> 1280x720 default.
        let sz = self.output_size;
        surface.with_pending_state(|s| {
            s.states.set(xdg_toplevel::State::Activated);
            s.size = Some(sz.into());
        });
        surface.send_configure();
        let wl_surf = surface.wl_surface().clone();
        self.layers.push(ToplevelLayer {
            toplevel: surface,
            position: (0, 0),
            size: (0, 0),
            #[cfg(feature = "headless")]
            pixels: None,
        });
        if let Ok(mut st) = self.stats.lock() {
            st.toplevel_count = self.layers.len() as u64;
        }
        self.focus_surface(Some(wl_surf));
    }
    fn ack_configure(&mut self, _surface: WlSurface, _configure: Configure) {
        if let Ok(mut st) = self.stats.lock() {
            st.ack_configure_count += 1;
        }
    }
    fn toplevel_destroyed(&mut self, surface: ToplevelSurface) {
        let wl = surface.wl_surface().clone();
        self.layers.retain(|l| l.toplevel.wl_surface() != &wl);
        if let Ok(mut st) = self.stats.lock() {
            st.toplevel_count = self.layers.len() as u64;
        }
        let next_focus = self.layers.last().map(|l| l.toplevel.wl_surface().clone());
        self.focus_surface(next_focus);
        #[cfg(feature = "headless")]
        if self.headless.is_some() {
            self.recomposite_headless(false);
        }
    }
    fn maximize_request(&mut self, surface: ToplevelSurface) {
        let sz = self.output_size;
        surface.with_pending_state(|s| {
            s.states.set(xdg_toplevel::State::Maximized);
            s.size = Some(sz.into());
        });
        surface.send_configure();
    }
    fn unmaximize_request(&mut self, surface: ToplevelSurface) {
        surface.with_pending_state(|s| {
            s.states.unset(xdg_toplevel::State::Maximized);
            s.size = None;
        });
        surface.send_configure();
    }
    fn fullscreen_request(&mut self, surface: ToplevelSurface, _output: Option<wl_output::WlOutput>) {
        let sz = self.output_size;
        surface.with_pending_state(|s| {
            s.states.set(xdg_toplevel::State::Fullscreen);
            s.size = Some(sz.into());
        });
        surface.send_configure();
    }
    fn unfullscreen_request(&mut self, surface: ToplevelSurface) {
        surface.with_pending_state(|s| {
            s.states.unset(xdg_toplevel::State::Fullscreen);
            s.size = None;
        });
        surface.send_configure();
    }
    fn new_popup(&mut self, surface: PopupSurface, _positioner: PositionerState) {
        let _ = surface.send_configure();
    }
    fn grab(&mut self, _surface: PopupSurface, _seat: wl_seat::WlSeat, _serial: Serial) {}
    fn reposition_request(&mut self, surface: PopupSurface, _positioner: PositionerState, token: u32) {
        surface.send_repositioned(token);
    }
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

impl XdgDecorationHandler for App {
    fn new_decoration(&mut self, toplevel: ToplevelSurface) {
        toplevel.with_pending_state(|s| s.decoration_mode = Some(DecorationMode::ServerSide));
        if toplevel.is_initial_configure_sent() {
            toplevel.send_configure();
        }
    }
    fn request_mode(&mut self, toplevel: ToplevelSurface, _mode: DecorationMode) {
        self.new_decoration(toplevel);
    }
    fn unset_mode(&mut self, toplevel: ToplevelSurface) {
        self.new_decoration(toplevel);
    }
}

delegate_compositor!(App);
delegate_shm!(App);
delegate_xdg_shell!(App);
delegate_seat!(App);
delegate_output!(App);
delegate_xdg_decoration!(App);

fn add_client(dh: &mut DisplayHandle, stream: UnixStream) {
    if let Err(e) = dh.insert_client(stream, Arc::new(ClientState::default())) {
        log_msg(&format!("lodos-wl: insert_client failed: {e}"));
    }
}

struct ServerRunArgs {
    path: PathBuf,
    output_size: (i32, i32),
    presenter: Option<Arc<Mutex<WinPresenter>>>,
    #[cfg(feature = "headless")]
    headless: Option<HeadlessBackend>,
    stats: Arc<Mutex<ServerStats>>,
    ctrl_rx: Option<Channel<ControlMsg>>,
    stop_ctl: Option<(Arc<AtomicBool>, PingSource)>,
    ready_tx: Option<SyncSender<Result<(), String>>>,
}

fn run_server_inner(args: ServerRunArgs) -> Result<(), Box<dyn std::error::Error>> {
    let ServerRunArgs {
        path,
        output_size,
        presenter,
        #[cfg(feature = "headless")]
        headless,
        stats,
        ctrl_rx,
        stop_ctl,
        ready_tx,
    } = args;

    let setup = (|| -> Result<_, String> {
        if path.as_os_str().len() >= 108 {
            return Err(format!("socket path too long (>=108): {}", path.display()));
        }
        if let Some(dir) = path.parent() {
            if !dir.as_os_str().is_empty() {
                std::fs::create_dir_all(dir)
                    .map_err(|e| format!("mkdir {}: {e}", dir.display()))?;
            }
        }
        let _ = std::fs::remove_file(&path);
        let _ = std::fs::remove_file(format!("{}.lock", path.display()));

        let event_loop: EventLoop<App> =
            EventLoop::try_new().map_err(|e| format!("EventLoop: {e}"))?;
        let display: Display<App> = Display::new().map_err(|e| format!("Display: {e}"))?;
        let dh = display.handle();

        let compositor_state = CompositorState::new::<App>(&dh);
        let shm_state = ShmState::new::<App>(
            &dh,
            vec![wl_shm::Format::Abgr8888, wl_shm::Format::Xbgr8888],
        );
        let xdg_shell_state = XdgShellState::new::<App>(&dh);
        // xdg-decoration stub: always server-side (we draw none) so nested wlroots/GTK skip CSD.
        let xdg_decoration_state = XdgDecorationState::new::<App>(&dh);
        let mut seat_state = SeatState::new();
        let mut seat: Seat<App> = seat_state.new_wl_seat(&dh, "seat0");
        seat.add_pointer();
        seat.add_touch();
        if let Err(e) = seat.add_keyboard(XkbConfig::default(), 200, 25) {
            log_msg(&format!("lodos-wl: add_keyboard skipped ({e})"));
        }

        let output_state = OutputManagerState::new_with_xdg_output::<App>(&dh);
        let output = Output::new(
            "LODOS-1".into(),
            PhysicalProperties {
                size: (70, 155).into(),
                subpixel: Subpixel::Unknown,
                make: "LodOS".into(),
                model: "virtual".into(),
            },
        );
        let mode = Mode {
            size: output_size.into(),
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
                    .insert_source(
                        Generic::new(listener, Interest::READ, CMode::Level),
                        move |_, l, _| {
                            while let Some(stream) = l.accept()? {
                                add_client(&mut dh_accept, stream);
                            }
                            Ok(PostAction::Continue)
                        },
                    )
                    .map_err(|e| format!("insert listener: {e}"))?;
                listening += 1;
            }
            Err(e) => log_msg(&format!("lodos-wl: path socket {} failed: {e:?}", path.display())),
        }

        #[cfg(target_os = "android")]
        {
            let abs = format!(
                "lodos-{}",
                path.file_name().and_then(|n| n.to_str()).unwrap_or("wayland-0")
            );
            match SocketAddr::from_abstract_name(abs.as_bytes())
                .and_then(|a| UnixListener::bind_addr(&a))
            {
                Ok(listener) => {
                    listener
                        .set_nonblocking(true)
                        .map_err(|e| format!("set_nonblocking: {e}"))?;
                    log_msg(&format!("lodos-wl: listening on abstract @{abs}"));
                    let mut dh_accept = dh.clone();
                    handle
                        .insert_source(
                            Generic::new(listener, Interest::READ, CMode::Level),
                            move |_, l, _| {
                                loop {
                                    match l.accept() {
                                        Ok((stream, _)) => add_client(&mut dh_accept, stream),
                                        Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                                        Err(e) => return Err(e),
                                    }
                                }
                                Ok(PostAction::Continue)
                            },
                        )
                        .map_err(|e| format!("insert abstract listener: {e}"))?;
                    listening += 1;
                }
                Err(e) => log_msg(&format!("lodos-wl: abstract socket @{abs} failed: {e}")),
            }
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

        if let Some(rx) = ctrl_rx {
            handle
                .insert_source(rx, |evt, _, app| {
                    if let ChannelEvent::Msg(msg) = evt {
                        app.handle_control(msg);
                    }
                })
                .map_err(|e| format!("insert ctrl channel: {e}"))?;
        }

        let stop_flag = if let Some((flag, ping_src)) = stop_ctl {
            handle
                .insert_source(ping_src, |_, _, _| {})
                .map_err(|e| format!("insert ping: {e}"))?;
            Some(flag)
        } else {
            None
        };

        #[cfg(feature = "headless")]
        if let Some(hb) = &headless {
            if hb.install_sigusr1 {
                install_sigusr1_handler();
            }
            if let Some(cp) = &hb.ctl_path {
                ctl::insert_ctl_listener(&handle, cp)?;
            }
        }

        let app = App {
            compositor_state,
            xdg_shell_state,
            shm_state,
            seat_state,
            seat,
            _output_state: output_state,
            _xdg_decoration_state: xdg_decoration_state,
            output,
            output_size,
            presenter,
            #[cfg(feature = "headless")]
            headless,
            layers: Vec::new(),
            shell_clients: Vec::new(),
            pointer_pos: (0.0, 0.0).into(),
            stats,
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
        #[cfg(feature = "headless")]
        if SIGUSR1_PENDING.swap(false, Ordering::AcqRel) {
            if let Some(path) = app
                .headless
                .as_ref()
                .and_then(|hb| hb.dump_path.clone())
            {
                app.dump_headless_ppm(&path);
            }
        }
        event_loop.dispatch(Some(Duration::from_millis(20)), &mut app)?;
        let _ = dh_flush.flush_clients();
    }
    let _ = std::fs::remove_file(&path);
    let _ = std::fs::remove_file(format!("{}.lock", path.display()));
    Ok(())
}

/// CLI entrypoint used by `src/main.rs` without the `headless` feature (T013).
pub fn run_headless(path: PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    run_server_inner(ServerRunArgs {
        path,
        output_size: (DEFAULT_WIDTH as i32, DEFAULT_HEIGHT as i32),
        presenter: None,
        #[cfg(feature = "headless")]
        headless: None,
        stats: Arc::new(Mutex::new(ServerStats::default())),
        ctrl_rx: None,
        stop_ctl: None,
        ready_tx: None,
    })
}

// ---- Headless backend (PC x86_64 testing & nested compositor bring-up) ----

/// Configuration for the headless in-memory framebuffer backend.
#[cfg(feature = "headless")]
#[derive(Debug, Clone)]
pub struct HeadlessConfig {
    pub socket_path: PathBuf,
    pub width: u32,
    pub height: u32,
    pub dump_path: Option<PathBuf>,
    pub dump_after_frames: Option<u64>,
    pub install_sigusr1: bool,
    /// Runtime control socket path (`LODOS_WL_CTL`, see `ctl` module), None = disabled.
    pub ctl_path: Option<PathBuf>,
}

#[cfg(feature = "headless")]
impl HeadlessConfig {
    pub fn new(socket_path: PathBuf) -> Self {
        Self {
            socket_path,
            width: DEFAULT_WIDTH,
            height: DEFAULT_HEIGHT,
            dump_path: None,
            dump_after_frames: None,
            install_sigusr1: false,
            ctl_path: None,
        }
    }

    /// Loads defaults from environment (`LODOS_WL_WIDTH`, `LODOS_WL_HEIGHT`,
    /// `LODOS_WL_DUMP`, `LODOS_WL_DUMP_AFTER`) with `SIGUSR1` enabled.
    pub fn from_env(socket_path: PathBuf) -> Self {
        let width = std::env::var("LODOS_WL_WIDTH")
            .ok()
            .and_then(|v| v.parse().ok())
            .filter(|&v: &u32| v > 0)
            .unwrap_or(DEFAULT_WIDTH);
        let height = std::env::var("LODOS_WL_HEIGHT")
            .ok()
            .and_then(|v| v.parse().ok())
            .filter(|&v: &u32| v > 0)
            .unwrap_or(DEFAULT_HEIGHT);
        let dump_path = std::env::var("LODOS_WL_DUMP")
            .ok()
            .filter(|s| !s.is_empty())
            .map(PathBuf::from);
        let dump_after_frames = std::env::var("LODOS_WL_DUMP_AFTER")
            .ok()
            .and_then(|v| v.parse().ok())
            .filter(|&v: &u64| v > 0);
        Self {
            socket_path,
            width,
            height,
            dump_path,
            dump_after_frames,
            install_sigusr1: true,
            ctl_path: std::env::var("LODOS_WL_CTL")
                .ok()
                .filter(|s| !s.is_empty())
                .map(PathBuf::from),
        }
    }
}

/// Runs the headless Wayland server in the foreground (CLI entrypoint).
#[cfg(feature = "headless")]
pub fn run_headless_blocking(config: HeadlessConfig) -> Result<(), Box<dyn std::error::Error>> {
    let fb = Arc::new(Mutex::new(Framebuffer::new(config.width, config.height)));
    let headless = HeadlessBackend {
        fb,
        dump_path: config.dump_path,
        dump_after_frames: config.dump_after_frames,
        install_sigusr1: config.install_sigusr1,
        ctl_path: config.ctl_path,
    };
    run_server_inner(ServerRunArgs {
        path: config.socket_path,
        output_size: (config.width as i32, config.height as i32),
        presenter: None,
        headless: Some(headless),
        stats: Arc::new(Mutex::new(ServerStats::default())),
        ctrl_rx: None,
        stop_ctl: None,
        ready_tx: None,
    })
}

/// Handle to a running headless Wayland server on a background thread.
#[cfg(feature = "headless")]
pub struct HeadlessServer {
    socket_path: PathBuf,
    framebuffer: Arc<Mutex<Framebuffer>>,
    stats: Arc<Mutex<ServerStats>>,
    ctrl_tx: smithay::reexports::calloop::channel::Sender<ControlMsg>,
    stop_flag: Arc<AtomicBool>,
    ping: Ping,
    join: Option<JoinHandle<()>>,
}

#[cfg(feature = "headless")]
impl HeadlessServer {
    /// Starts the headless server on a background thread and waits until the socket is ready.
    pub fn start(config: HeadlessConfig) -> Result<Self, String> {
        let fb = Arc::new(Mutex::new(Framebuffer::new(config.width, config.height)));
        let stats = Arc::new(Mutex::new(ServerStats::default()));
        let (ctrl_tx, ctrl_rx) = smithay::reexports::calloop::channel::channel::<ControlMsg>();
        let stop_flag = Arc::new(AtomicBool::new(false));
        let (ping, ping_src) = make_ping().map_err(|e| format!("make_ping: {e}"))?;
        let (ready_tx, ready_rx) = mpsc::sync_channel(1);

        let socket_path = config.socket_path.clone();
        let thread_fb = Arc::clone(&fb);
        let thread_stats = Arc::clone(&stats);
        let thread_stop = Arc::clone(&stop_flag);
        let headless = HeadlessBackend {
            fb: thread_fb,
            dump_path: config.dump_path,
            dump_after_frames: config.dump_after_frames,
            install_sigusr1: config.install_sigusr1,
            ctl_path: config.ctl_path,
        };

        let join = thread::Builder::new()
            .name("lodos-wl-headless".into())
            .spawn(move || {
                if let Err(e) = run_server_inner(ServerRunArgs {
                    path: config.socket_path,
                    output_size: (config.width as i32, config.height as i32),
                    presenter: None,
                    headless: Some(headless),
                    stats: thread_stats,
                    ctrl_rx: Some(ctrl_rx),
                    stop_ctl: Some((thread_stop, ping_src)),
                    ready_tx: Some(ready_tx),
                }) {
                    log_msg(&format!("lodos-wl: headless server exited with error: {e}"));
                }
            })
            .map_err(|e| format!("spawn thread: {e}"))?;

        match ready_rx.recv_timeout(Duration::from_secs(5)) {
            Ok(Ok(())) => Ok(Self {
                socket_path,
                framebuffer: fb,
                stats,
                ctrl_tx,
                stop_flag,
                ping,
                join: Some(join),
            }),
            Ok(Err(e)) => {
                let _ = join.join();
                Err(e)
            }
            Err(e) => Err(format!("timeout waiting for headless server ready: {e}")),
        }
    }

    pub fn socket_path(&self) -> &std::path::Path {
        &self.socket_path
    }

    pub fn framebuffer(&self) -> Arc<Mutex<Framebuffer>> {
        Arc::clone(&self.framebuffer)
    }

    pub fn pixel(&self, x: u32, y: u32) -> Option<[u8; 4]> {
        self.framebuffer.lock().ok().and_then(|fb| fb.pixel(x, y))
    }

    pub fn frame_count(&self) -> u64 {
        self.framebuffer.lock().map(|fb| fb.frame_count()).unwrap_or(0)
    }

    pub fn stats(&self) -> ServerStats {
        self.stats.lock().map(|s| *s).unwrap_or_default()
    }

    pub fn send_control(&self, msg: ControlMsg) {
        let _ = self.ctrl_tx.send(msg);
    }

    pub fn inject_touch_down(&self, slot: u32, x: f64, y: f64) {
        self.send_control(ControlMsg::TouchDown { slot, x, y });
    }

    pub fn inject_touch_motion(&self, slot: u32, x: f64, y: f64) {
        self.send_control(ControlMsg::TouchMotion { slot, x, y });
    }

    pub fn inject_touch_up(&self, slot: u32) {
        self.send_control(ControlMsg::TouchUp { slot });
    }

    pub fn inject_touch_cancel(&self) {
        self.send_control(ControlMsg::TouchCancel);
    }

    pub fn inject_pointer_motion(&self, x: f64, y: f64) {
        self.send_control(ControlMsg::PointerMotion { x, y });
    }

    pub fn inject_pointer_button(&self, button: u32, pressed: bool) {
        self.send_control(ControlMsg::PointerButton { button, pressed });
    }

    pub fn inject_key(&self, evdev_code: u32, pressed: bool) {
        self.send_control(ControlMsg::Key { evdev_code, pressed });
    }

    pub fn ping_clients(&self) {
        self.send_control(ControlMsg::PingClients);
    }

    pub fn dump_ppm(&self, path: impl AsRef<std::path::Path>) -> std::io::Result<()> {
        let fb = self
            .framebuffer
            .lock()
            .map_err(|_| std::io::Error::other("framebuffer mutex poisoned"))?;
        fb.write_ppm(path.as_ref())
    }

    pub fn shutdown(&mut self) {
        self.stop_flag.store(true, Ordering::Release);
        self.ping.ping();
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

#[cfg(feature = "headless")]
impl Drop for HeadlessServer {
    fn drop(&mut self) {
        self.shutdown();
    }
}

// ---- Android JNI / C ABI server controller ----

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
    let mut guard = server_slot()
        .lock()
        .map_err(|_| "server mutex poisoned".to_string())?;

    if let Some(ctrl) = guard.as_mut() {
        if ctrl.sock_path == sock_path && ctrl.is_alive() {
            log_msg(&format!(
                "lodos-wl: updating ANativeWindow on active server {}",
                sock_path.display()
            ));
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
            if let Err(e) = run_server_inner(ServerRunArgs {
                path: thread_path,
                output_size: (DEFAULT_WIDTH as i32, DEFAULT_HEIGHT as i32),
                presenter: Some(thread_presenter),
                #[cfg(feature = "headless")]
                headless: None,
                stats: Arc::new(Mutex::new(ServerStats::default())),
                ctrl_rx: None,
                stop_ctl: Some((thread_stop, ping_src)),
                ready_tx: Some(ready_tx),
            }) {
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
