//! lodos-wl: headless mini Wayland server (S2 spike, T013).
//! Globals: wl_compositor, wl_subcompositor, wl_shm, xdg_wm_base, wl_seat (no devices),
//! wl_output (fake 1080x2400). No rendering.
//! usage: lodos-wl [socket-path]   (default /data/local/tmp/lodos/run/wayland-0)
//! Also listens on abstract unix socket "@lodos-<basename>": shell uid on HyperOS (SELinux) may not
//! create sock_file in /data/local/tmp (EACCES) -> clients use abstract socket + WAYLAND_SOCKET fd.
use std::{
    os::android::net::SocketAddrExt,
    os::unix::net::{SocketAddr, UnixListener, UnixStream},
    path::PathBuf,
    sync::Arc,
    time::Duration,
};

use smithay::{
    delegate_compositor, delegate_output, delegate_seat, delegate_shm, delegate_xdg_shell,
    input::{Seat, SeatHandler, SeatState},
    output::{Mode, Output, PhysicalProperties, Subpixel},
    reexports::{
        calloop::{generic::Generic, EventLoop, Interest, Mode as CMode, PostAction},
        wayland_protocols::xdg::shell::server::xdg_toplevel,
        wayland_server::{
            backend::{ClientData, ClientId, DisconnectReason},
            protocol::{wl_buffer, wl_seat, wl_surface::WlSurface},
            Client, Display, DisplayHandle, ListeningSocket,
        },
    },
    utils::{Serial, Transform},
    wayland::{
        buffer::BufferHandler,
        compositor::{CompositorClientState, CompositorHandler, CompositorState},
        output::{OutputHandler, OutputManagerState},
        shell::xdg::{PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState},
        shm::{ShmHandler, ShmState},
    },
};

const DEFAULT_SOCK: &str = "/data/local/tmp/lodos/run/wayland-0";

struct App {
    compositor_state: CompositorState,
    xdg_shell_state: XdgShellState,
    shm_state: ShmState,
    seat_state: SeatState<App>,
    _output_state: OutputManagerState,
}

#[derive(Default)]
struct ClientState {
    compositor_state: CompositorClientState,
}

impl ClientData for ClientState {
    fn initialized(&self, id: ClientId) {
        eprintln!("lodos-wl: client connected {id:?}");
    }
    fn disconnected(&self, id: ClientId, reason: DisconnectReason) {
        eprintln!("lodos-wl: client disconnected {id:?} ({reason:?})");
    }
}

impl CompositorHandler for App {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor_state
    }
    fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
        &client.get_data::<ClientState>().unwrap().compositor_state
    }
    fn commit(&mut self, _surface: &WlSurface) {}
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
        eprintln!("lodos-wl: insert_client failed: {e}");
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = PathBuf::from(std::env::args().nth(1).unwrap_or_else(|| DEFAULT_SOCK.into()));
    if path.as_os_str().len() >= 108 {
        return Err(format!("socket path too long (>=108): {}", path.display()).into());
    }
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    // stale socket from previous run
    let _ = std::fs::remove_file(&path);

    let mut event_loop: EventLoop<App> = EventLoop::try_new()?;
    let display: Display<App> = Display::new()?;
    let dh = display.handle();

    let compositor_state = CompositorState::new::<App>(&dh);
    let shm_state = ShmState::new::<App>(&dh, vec![]);
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
    let mode = Mode { size: (1080, 2400).into(), refresh: 60_000 };
    output.create_global::<App>(&dh);
    output.change_current_state(Some(mode), Some(Transform::Normal), None, Some((0, 0).into()));
    output.set_preferred(mode);

    let handle = event_loop.handle();
    let mut listening = 0;
    match ListeningSocket::bind_absolute(path.clone()) {
        Ok(listener) => {
            eprintln!("lodos-wl: listening on {}", path.display());
            let mut dh_accept = dh.clone();
            handle.insert_source(Generic::new(listener, Interest::READ, CMode::Level), move |_, l, _| {
                while let Some(stream) = l.accept()? {
                    add_client(&mut dh_accept, stream);
                }
                Ok(PostAction::Continue)
            })?;
            listening += 1;
        }
        Err(e) => eprintln!("lodos-wl: path socket {} failed: {e:?}", path.display()),
    }
    let abs = format!("lodos-{}", path.file_name().and_then(|n| n.to_str()).unwrap_or("wayland-0"));
    match SocketAddr::from_abstract_name(abs.as_bytes()).and_then(|a| UnixListener::bind_addr(&a)) {
        Ok(listener) => {
            listener.set_nonblocking(true)?;
            eprintln!("lodos-wl: listening on abstract @{abs}");
            let mut dh_accept = dh.clone();
            handle.insert_source(Generic::new(listener, Interest::READ, CMode::Level), move |_, l, _| {
                loop {
                    match l.accept() {
                        Ok((stream, _)) => add_client(&mut dh_accept, stream),
                        Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                        Err(e) => return Err(e),
                    }
                }
                Ok(PostAction::Continue)
            })?;
            listening += 1;
        }
        Err(e) => eprintln!("lodos-wl: abstract socket @{abs} failed: {e}"),
    }
    if listening == 0 {
        return Err("no listening socket".into());
    }
    handle.insert_source(Generic::new(display, Interest::READ, CMode::Level), |_, d, app| {
        // SAFETY: display is not dropped while the source is registered
        unsafe { d.get_mut().dispatch_clients(app).map_err(std::io::Error::other)? };
        Ok(PostAction::Continue)
    })?;

    let mut app = App {
        compositor_state,
        xdg_shell_state,
        shm_state,
        seat_state,
        _output_state: output_state,
    };
    let mut dh_flush = dh.clone();
    loop {
        event_loop.dispatch(Some(Duration::from_millis(100)), &mut app)?;
        let _ = dh_flush.flush_clients();
    }
}
