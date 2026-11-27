use lodos_audio::{Backend, Server, WavBackend};
use std::path::PathBuf;

fn env_or<T: std::str::FromStr>(k: &str, d: T) -> T {
    std::env::var(k).ok().and_then(|v| v.parse().ok()).unwrap_or(d)
}

fn main() {
    // env: LODOS_AUDIO_SOCK (required), LODOS_AUDIO_RATE=48000, LODOS_AUDIO_CH=2,
    //      LODOS_AUDIO_BACKEND=wav|aaudio, LODOS_AUDIO_WAV=out.wav, LODOS_AUDIO_WAV_TRIM=1,
    //      LODOS_AUDIO_MAX_CLIENTS (default: unlimited)
    let sock = match std::env::var_os("LODOS_AUDIO_SOCK") {
        Some(s) => PathBuf::from(s),
        None => {
            eprintln!("lodos-audio: LODOS_AUDIO_SOCK not set");
            std::process::exit(2);
        }
    };
    let rate = env_or("LODOS_AUDIO_RATE", 48000u32);
    let ch = env_or("LODOS_AUDIO_CH", 2u16);
    let max = std::env::var("LODOS_AUDIO_MAX_CLIENTS").ok().and_then(|v| v.parse().ok());
    let kind = std::env::var("LODOS_AUDIO_BACKEND").unwrap_or_else(|_| default_backend().into());
    let mut be: Box<dyn Backend> = match kind.as_str() {
        "wav" => Box::new(
            WavBackend::new(std::env::var("LODOS_AUDIO_WAV").unwrap_or_else(|_| "lodos-audio.wav".into()))
                .trim_leading_silence(env_or("LODOS_AUDIO_WAV_TRIM", 0u8) == 1),
        ),
        #[cfg(feature = "android")]
        "aaudio" => Box::new(lodos_audio::aaudio::AAudioBackend::default()),
        k => {
            eprintln!("lodos-audio: unknown backend {k}");
            std::process::exit(2);
        }
    };
    let srv = Server::bind(&sock, rate, ch).unwrap_or_else(|e| {
        eprintln!("lodos-audio: bind {}: {e}", sock.display());
        std::process::exit(1);
    });
    eprintln!("[lodos-audio] listening {} rate={rate} ch={ch} backend={kind}", sock.display());
    if let Err(e) = srv.run(be.as_mut(), max) {
        eprintln!("lodos-audio: {e}");
        std::process::exit(1);
    }
}

fn default_backend() -> &'static str {
    if cfg!(feature = "android") { "aaudio" } else { "wav" }
}
