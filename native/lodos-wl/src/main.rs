//! CLI wrapper for lodos-wl (Android spike / PC headless Wayland host).
use std::path::PathBuf;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(lodos_wl::resolve_default_socket_path);

    #[cfg(feature = "headless")]
    {
        let cfg = lodos_wl::HeadlessConfig::from_env(path);
        lodos_wl::run_headless_blocking(cfg)
    }

    #[cfg(not(feature = "headless"))]
    {
        lodos_wl::run_headless(path)
    }
}
