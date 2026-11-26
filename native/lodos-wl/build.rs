// Link static libxkbcommon built by scripts/build-xkbcommon.sh (android target only).
fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("android") {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../build/xkbcommon/arm64/lib");
        println!("cargo:rustc-link-search=native={}", root.canonicalize().expect("run scripts/build-xkbcommon.sh").display());
    }
    println!("cargo:rerun-if-changed=build.rs");
}
