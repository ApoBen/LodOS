use lodos_audio::*;
use std::io::Write;
use std::os::unix::net::UnixStream;
use std::sync::{Arc, Mutex};

fn tmpdir(name: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!("lodos-audio-test-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

#[derive(Clone, Default)]
struct Mem(Arc<Mutex<(u32, Vec<i16>, usize)>>); // opens, samples, closes
impl Backend for Mem {
    fn open(&mut self, _r: u32, _c: u16) -> std::io::Result<()> {
        self.0.lock().unwrap().0 += 1;
        Ok(())
    }
    fn write(&mut self, s: &[i16]) -> std::io::Result<()> {
        assert_eq!(s.len() % 2, 0, "partial frame leaked");
        self.0.lock().unwrap().1.extend_from_slice(s);
        Ok(())
    }
    fn close(&mut self) -> std::io::Result<()> {
        self.0.lock().unwrap().2 += 1;
        Ok(())
    }
}

#[test]
fn wav_header_fields() {
    let h = wav_header(48000, 2, 1000);
    assert_eq!(&h[0..4], b"RIFF");
    assert_eq!(u32::from_le_bytes(h[4..8].try_into().unwrap()), 1036);
    assert_eq!(&h[8..16], b"WAVEfmt ");
    assert_eq!(u16::from_le_bytes([h[22], h[23]]), 2);
    assert_eq!(u32::from_le_bytes(h[24..28].try_into().unwrap()), 48000);
    assert_eq!(u32::from_le_bytes(h[28..32].try_into().unwrap()), 192000);
    assert_eq!(u16::from_le_bytes([h[32], h[33]]), 4);
    assert_eq!(&h[36..40], b"data");
    assert_eq!(u32::from_le_bytes(h[40..44].try_into().unwrap()), 1000);
}

#[test]
fn wav_backend_writes_valid_file() {
    let d = tmpdir("wav");
    let p = d.join("o.wav");
    let mut w = WavBackend::new(&p);
    w.open(48000, 2).unwrap();
    w.write(&[1, -1, 2, -2]).unwrap();
    w.write(&[3, -3]).unwrap();
    // valid before close (header patched per write)
    let b = std::fs::read(&p).unwrap();
    assert_eq!(b.len(), 44 + 12);
    assert_eq!(u32::from_le_bytes(b[40..44].try_into().unwrap()), 12);
    assert_eq!(i16::from_le_bytes([b[44 + 10], b[44 + 11]]), -3);
    w.close().unwrap();
}

#[test]
fn partial_frames_carried() {
    let mut a = FrameAssembler::new(2);
    assert!(a.push(&[1, 0, 2]).is_empty());
    assert_eq!(a.pending_bytes(), 3);
    assert_eq!(a.push(&[0, 3, 0, 4, 0, 5]), vec![1, 2, 3, 4]);
    assert_eq!(a.pending_bytes(), 1);
    a.reset();
    assert_eq!(a.push(&[7, 0, 8, 0]), vec![7, 8]);
}

#[test]
fn reconnect_reuses_backend() {
    let d = tmpdir("reconn");
    let sock = d.join("a.sock");
    let srv = Server::bind(&sock, 48000, 2).unwrap();
    let mem = Mem::default();
    let mut be = mem.clone();
    let t = std::thread::spawn(move || srv.run(&mut be, Some(3)).unwrap());
    for i in 0..3i16 {
        let mut c = UnixStream::connect(&sock).unwrap();
        // 2 frames + a dangling partial frame (dropped at EOF), split across writes
        let mut bytes = Vec::new();
        for s in [i, i, i + 10, i + 10] {
            bytes.extend_from_slice(&s.to_le_bytes());
        }
        bytes.extend_from_slice(&[9, 9, 9]);
        c.write_all(&bytes[..5]).unwrap();
        c.flush().unwrap();
        std::thread::sleep(std::time::Duration::from_millis(5));
        c.write_all(&bytes[5..]).unwrap();
        drop(c);
    }
    t.join().unwrap();
    let g = mem.0.lock().unwrap();
    assert_eq!(g.0, 3, "open per client (backend idempotent)");
    assert_eq!(g.1, vec![0, 0, 10, 10, 1, 1, 11, 11, 2, 2, 12, 12]);
    assert_eq!(g.2, 1);
}

#[test]
fn stale_socket_replaced() {
    let d = tmpdir("stale");
    let sock = d.join("s.sock");
    let l = std::os::unix::net::UnixListener::bind(&sock).unwrap();
    drop(l); // socket file remains
    assert!(Server::bind(&sock, 48000, 2).is_ok());
}

#[test]
fn wav_trim_leading_silence() {
    let d = tmpdir("trim");
    let p = d.join("t.wav");
    let mut w = WavBackend::new(&p).trim_leading_silence(true);
    w.open(48000, 2).unwrap();
    w.write(&[0, 0, 0, 0]).unwrap();
    w.write(&[0, 0, 0, 5, 6, 0, 0, 0]).unwrap(); // frame-aligned start at [0,5]
    assert_eq!(w.data_bytes(), 12);
    w.write(&[0, 0]).unwrap(); // silence after start is kept
    assert_eq!(w.data_bytes(), 16);
}
