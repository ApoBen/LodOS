//! CLI wrapper for lodos-wl (headless Wayland server, T013).
use std::path::PathBuf;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = PathBuf::from(
        std::env::args()
            .nth(1)
            .unwrap_or_else(|| lodos_wl::DEFAULT_SOCK.into()),
    );
    lodos_wl::run_headless(path)
}
