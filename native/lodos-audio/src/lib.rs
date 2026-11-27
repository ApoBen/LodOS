//! lodos-audio: receive raw S16LE interleaved PCM over a unix stream socket and hand it to a Backend.
//! Wire format v0: no header; rate/channels fixed by config (default 48000 Hz, 2 ch).
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};

pub trait Backend {
    fn open(&mut self, rate: u32, channels: u16) -> io::Result<()>;
    /// interleaved samples; always a whole number of frames
    fn write(&mut self, samples: &[i16]) -> io::Result<()>;
    fn close(&mut self) -> io::Result<()>;
}

/// PCM -> WAV file. Header sizes are patched after every write so the file is valid even if killed.
pub struct WavBackend {
    path: PathBuf,
    file: Option<File>,
    data_bytes: u32,
    rate: u32,
    channels: u16,
    trim_leading: bool,
    started: bool,
}

impl WavBackend {
    pub fn new(path: impl Into<PathBuf>) -> Self {
        WavBackend {
            path: path.into(),
            file: None,
            data_bytes: 0,
            rate: 0,
            channels: 0,
            trim_leading: false,
            started: false,
        }
    }
    /// Drop all-zero frames before the first non-silent frame (test recordings: pulse sinks emit
    /// idle silence before playback starts).
    pub fn trim_leading_silence(mut self, on: bool) -> Self {
        self.trim_leading = on;
        self
    }
    pub fn data_bytes(&self) -> u32 {
        self.data_bytes
    }
}

pub fn wav_header(rate: u32, channels: u16, data_bytes: u32) -> [u8; 44] {
    let mut h = [0u8; 44];
    let block = channels as u32 * 2;
    h[0..4].copy_from_slice(b"RIFF");
    h[4..8].copy_from_slice(&(36 + data_bytes).to_le_bytes());
    h[8..12].copy_from_slice(b"WAVE");
    h[12..16].copy_from_slice(b"fmt ");
    h[16..20].copy_from_slice(&16u32.to_le_bytes());
    h[20..22].copy_from_slice(&1u16.to_le_bytes()); // PCM
    h[22..24].copy_from_slice(&channels.to_le_bytes());
    h[24..28].copy_from_slice(&rate.to_le_bytes());
    h[28..32].copy_from_slice(&(rate * block).to_le_bytes());
    h[32..34].copy_from_slice(&(block as u16).to_le_bytes());
    h[34..36].copy_from_slice(&16u16.to_le_bytes());
    h[36..40].copy_from_slice(b"data");
    h[40..44].copy_from_slice(&data_bytes.to_le_bytes());
    h
}

impl Backend for WavBackend {
    fn open(&mut self, rate: u32, channels: u16) -> io::Result<()> {
        if self.file.is_some() {
            return Ok(());
        }
        let mut f = File::create(&self.path)?;
        f.write_all(&wav_header(rate, channels, 0))?;
        self.file = Some(f);
        self.data_bytes = 0;
        self.started = false;
        self.rate = rate;
        self.channels = channels;
        Ok(())
    }
    fn write(&mut self, samples: &[i16]) -> io::Result<()> {
        let f = self.file.as_mut().ok_or_else(|| io::Error::other("wav not open"))?;
        let mut samples = samples;
        if self.trim_leading && !self.started {
            let ch = self.channels.max(1) as usize;
            match samples.iter().position(|&x| x != 0) {
                None => return Ok(()),
                Some(i) => {
                    samples = &samples[i / ch * ch..];
                    self.started = true;
                }
            }
        }
        let mut buf = Vec::with_capacity(samples.len() * 2);
        for s in samples {
            buf.extend_from_slice(&s.to_le_bytes());
        }
        f.seek(SeekFrom::End(0))?;
        f.write_all(&buf)?;
        self.data_bytes = self.data_bytes.saturating_add(buf.len() as u32);
        f.seek(SeekFrom::Start(0))?;
        f.write_all(&wav_header(self.rate, self.channels, self.data_bytes))?;
        Ok(())
    }
    fn close(&mut self) -> io::Result<()> {
        if let Some(mut f) = self.file.take() {
            f.flush()?;
        }
        Ok(())
    }
}

#[cfg(feature = "android")]
pub mod aaudio {
    use super::Backend;
    use ndk::audio::{AudioDirection, AudioFormat, AudioPerformanceMode, AudioStream, AudioStreamBuilder};
    use std::io;

    fn e<E: std::fmt::Debug>(x: E) -> io::Error {
        io::Error::other(format!("aaudio: {x:?}"))
    }

    /// AAudio output stream, blocking writes (stub for v0: no xrun/underrun handling yet).
    #[derive(Default)]
    pub struct AAudioBackend {
        stream: Option<AudioStream>,
        channels: u16,
    }

    impl Backend for AAudioBackend {
        fn open(&mut self, rate: u32, channels: u16) -> io::Result<()> {
            if self.stream.is_some() {
                return Ok(());
            }
            let s = AudioStreamBuilder::new()
                .map_err(e)?
                .direction(AudioDirection::Output)
                .sample_rate(rate as i32)
                .channel_count(channels as i32)
                .format(AudioFormat::PCM_I16)
                .performance_mode(AudioPerformanceMode::LowLatency)
                .open_stream()
                .map_err(e)?;
            s.request_start().map_err(e)?;
            self.stream = Some(s);
            self.channels = channels;
            Ok(())
        }
        fn write(&mut self, samples: &[i16]) -> io::Result<()> {
            let s = self.stream.as_ref().ok_or_else(|| io::Error::other("aaudio not open"))?;
            let ch = self.channels.max(1) as usize;
            let mut off = 0usize; // in frames
            let frames = samples.len() / ch;
            while off < frames {
                // SAFETY: pointer/len derived from the slice; AAudio writes only whole frames
                let n = unsafe {
                    s.write(samples[off * ch..].as_ptr().cast(), (frames - off) as i32, 1_000_000_000)
                }
                .map_err(e)? as usize;
                if n == 0 {
                    break; // timeout: drop rest rather than stall the socket
                }
                off += n;
            }
            Ok(())
        }
        fn close(&mut self) -> io::Result<()> {
            if let Some(s) = self.stream.take() {
                let _ = s.request_stop();
            }
            Ok(())
        }
    }
}

/// Convert a byte stream into whole frames, carrying partial frames across reads.
pub struct FrameAssembler {
    frame_bytes: usize,
    pending: Vec<u8>,
}

impl FrameAssembler {
    pub fn new(channels: u16) -> Self {
        FrameAssembler { frame_bytes: channels as usize * 2, pending: Vec::new() }
    }
    /// Feed bytes; returns samples for all complete frames available.
    pub fn push(&mut self, data: &[u8]) -> Vec<i16> {
        self.pending.extend_from_slice(data);
        let whole = self.pending.len() / self.frame_bytes * self.frame_bytes;
        let out = self.pending[..whole]
            .chunks_exact(2)
            .map(|c| i16::from_le_bytes([c[0], c[1]]))
            .collect();
        self.pending.drain(..whole);
        out
    }
    pub fn pending_bytes(&self) -> usize {
        self.pending.len()
    }
    pub fn reset(&mut self) {
        self.pending.clear();
    }
}

pub struct Server {
    listener: UnixListener,
    pub rate: u32,
    pub channels: u16,
}

impl Server {
    pub fn bind(path: &Path, rate: u32, channels: u16) -> io::Result<Self> {
        if let Ok(md) = std::fs::symlink_metadata(path) {
            use std::os::unix::fs::FileTypeExt;
            if md.file_type().is_socket() {
                std::fs::remove_file(path)?; // stale socket from previous run
            }
        }
        Ok(Server { listener: UnixListener::bind(path)?, rate, channels })
    }

    /// Serve one client until EOF; returns bytes received. Partial trailing frame is dropped.
    pub fn serve_client(&self, mut s: UnixStream, be: &mut dyn Backend) -> io::Result<u64> {
        be.open(self.rate, self.channels)?;
        let mut asm = FrameAssembler::new(self.channels);
        let mut buf = vec![0u8; 16384];
        let mut total = 0u64;
        loop {
            let n = match s.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) if e.kind() == io::ErrorKind::ConnectionReset => break,
                Err(e) => return Err(e),
            };
            total += n as u64;
            let samples = asm.push(&buf[..n]);
            if !samples.is_empty() {
                be.write(&samples)?;
            }
        }
        Ok(total)
    }

    /// Accept clients sequentially forever (or `max_clients` if Some). Reconnects reuse the backend.
    pub fn run(&self, be: &mut dyn Backend, max_clients: Option<usize>) -> io::Result<()> {
        let mut served = 0usize;
        while max_clients.map_or(true, |m| served < m) {
            let (s, _) = self.listener.accept()?;
            match self.serve_client(s, be) {
                Ok(n) => eprintln!("[lodos-audio] client done bytes={n}"),
                Err(e) => eprintln!("[lodos-audio] client error: {e}"),
            }
            served += 1;
        }
        be.close()
    }
}
