use std::process::Command;

fn api_level() -> String {
    Command::new("getprop")
        .arg("ro.build.version.sdk")
        .output()
        .ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "unknown".into())
}

fn main() {
    println!("lodos-rs ok {} api={}", std::env::consts::ARCH, api_level());
}
