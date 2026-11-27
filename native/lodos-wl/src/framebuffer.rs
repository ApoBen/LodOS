//! In-memory RGBA8888 framebuffer and PPM dump support for headless mode.

use std::{
    fs::File,
    io::{self, Write},
    path::Path,
};

/// In-memory RGBA8888 framebuffer (`[R, G, B, A]` per pixel, row-major).
#[derive(Debug, Clone)]
pub struct Framebuffer {
    width: u32,
    height: u32,
    pixels: Vec<u8>,
    frame_count: u64,
}

impl Framebuffer {
    /// Creates a new framebuffer initialized to opaque black (`[0, 0, 0, 255]`).
    pub fn new(width: u32, height: u32) -> Self {
        let len = (width as usize)
            .checked_mul(height as usize)
            .and_then(|n| n.checked_mul(4))
            .expect("framebuffer dimensions overflow");
        let mut pixels = vec![0u8; len];
        for chunk in pixels.chunks_exact_mut(4) {
            chunk[3] = 255;
        }
        Self {
            width,
            height,
            pixels,
            frame_count: 0,
        }
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    pub fn frame_count(&self) -> u64 {
        self.frame_count
    }

    pub fn inc_frame_count(&mut self) -> u64 {
        self.frame_count += 1;
        self.frame_count
    }

    pub fn pixels(&self) -> &[u8] {
        &self.pixels
    }

    /// Returns `[R, G, B, A]` at `(x, y)`, or `None` if out of bounds.
    pub fn pixel(&self, x: u32, y: u32) -> Option<[u8; 4]> {
        if x >= self.width || y >= self.height {
            return None;
        }
        let idx = ((y as usize) * (self.width as usize) + (x as usize)) * 4;
        Some([
            self.pixels[idx],
            self.pixels[idx + 1],
            self.pixels[idx + 2],
            self.pixels[idx + 3],
        ])
    }

    /// Fills the entire framebuffer with a solid `[R, G, B, A]` color.
    pub fn clear(&mut self, rgba: [u8; 4]) {
        for chunk in self.pixels.chunks_exact_mut(4) {
            chunk.copy_from_slice(&rgba);
        }
    }

    /// Blits an RGBA8888 source buffer onto this framebuffer at `(dst_x, dst_y)`.
    ///
    /// Wayland `ARGB8888` uses premultiplied alpha; when `opaque` is true (or `sa == 255`),
    /// pixels are copied directly.
    pub fn blit(
        &mut self,
        dst_x: i32,
        dst_y: i32,
        src_w: u32,
        src_h: u32,
        src_rgba: &[u8],
        opaque: bool,
    ) {
        if src_w == 0 || src_h == 0 || self.width == 0 || self.height == 0 {
            return;
        }
        let expected = (src_w as usize) * (src_h as usize) * 4;
        if src_rgba.len() < expected {
            return;
        }

        let fb_w = self.width as i64;
        let fb_h = self.height as i64;
        let dx0 = dst_x as i64;
        let dy0 = dst_y as i64;
        let dx1 = dx0 + src_w as i64;
        let dy1 = dy0 + src_h as i64;

        let clip_x0 = dx0.max(0);
        let clip_y0 = dy0.max(0);
        let clip_x1 = dx1.min(fb_w);
        let clip_y1 = dy1.min(fb_h);
        if clip_x0 >= clip_x1 || clip_y0 >= clip_y1 {
            return;
        }

        let copy_w = (clip_x1 - clip_x0) as usize;
        let sx_off = (clip_x0 - dx0) as usize;
        let sy_off = (clip_y0 - dy0) as usize;
        let dst_stride = (self.width as usize) * 4;
        let src_stride = (src_w as usize) * 4;

        for row in 0..((clip_y1 - clip_y0) as usize) {
            let dst_row_start = ((clip_y0 as usize) + row) * dst_stride + (clip_x0 as usize) * 4;
            let src_row_start = (sy_off + row) * src_stride + sx_off * 4;
            let dst_slice = &mut self.pixels[dst_row_start..dst_row_start + copy_w * 4];
            let src_slice = &src_rgba[src_row_start..src_row_start + copy_w * 4];

            if opaque {
                dst_slice.copy_from_slice(src_slice);
            } else {
                for (d, s) in dst_slice.chunks_exact_mut(4).zip(src_slice.chunks_exact(4)) {
                    let sa = s[3] as u16;
                    if sa == 255 {
                        d.copy_from_slice(s);
                    } else if sa > 0 {
                        let inv = 255 - sa;
                        d[0] = (s[0] as u16 + (d[0] as u16 * inv + 127) / 255).min(255) as u8;
                        d[1] = (s[1] as u16 + (d[1] as u16 * inv + 127) / 255).min(255) as u8;
                        d[2] = (s[2] as u16 + (d[2] as u16 * inv + 127) / 255).min(255) as u8;
                        d[3] = (sa + (d[3] as u16 * inv + 127) / 255).min(255) as u8;
                    }
                }
            }
        }
    }

    /// Encodes the framebuffer as binary PPM (`P6`, RGB 24-bit).
    pub fn encode_ppm(&self) -> Vec<u8> {
        let header = format!("P6\n{} {}\n255\n", self.width, self.height);
        let rgb_len = (self.width as usize) * (self.height as usize) * 3;
        let mut out = Vec::with_capacity(header.len() + rgb_len);
        out.extend_from_slice(header.as_bytes());
        for px in self.pixels.chunks_exact(4) {
            out.push(px[0]);
            out.push(px[1]);
            out.push(px[2]);
        }
        out
    }

    /// Writes the framebuffer to `path` as a binary PPM (`P6`) file atomically when possible.
    pub fn write_ppm(&self, path: &Path) -> io::Result<()> {
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                std::fs::create_dir_all(parent)?;
            }
        }
        let data = self.encode_ppm();
        let tmp = path.with_extension("ppm.tmp");
        {
            let mut f = File::create(&tmp)?;
            f.write_all(&data)?;
            f.sync_all()?;
        }
        if std::fs::rename(&tmp, path).is_err() {
            let mut f = File::create(path)?;
            f.write_all(&data)?;
            let _ = std::fs::remove_file(&tmp);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blit_clip_and_ppm() {
        let mut fb = Framebuffer::new(8, 8);
        assert_eq!(fb.pixel(0, 0), Some([0, 0, 0, 255]));
        let green = [0u8, 255, 0, 255].repeat(16); // 4x4
        fb.blit(2, 2, 4, 4, &green, true);
        assert_eq!(fb.pixel(1, 1), Some([0, 0, 0, 255]));
        assert_eq!(fb.pixel(2, 2), Some([0, 255, 0, 255]));
        assert_eq!(fb.pixel(5, 5), Some([0, 255, 0, 255]));
        assert_eq!(fb.pixel(6, 6), Some([0, 0, 0, 255]));

        let ppm = fb.encode_ppm();
        let hdr = b"P6\n8 8\n255\n";
        assert!(ppm.starts_with(hdr));
        assert_eq!(ppm.len(), hdr.len() + 8 * 8 * 3);
        let p22 = hdr.len() + (2 * 8 + 2) * 3;
        assert_eq!(&ppm[p22..p22 + 3], &[0, 255, 0]);
    }
}
