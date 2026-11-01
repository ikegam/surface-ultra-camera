// SPDX-License-Identifier: GPL-2.0
//! Software ISP for the OV02C10 raw stream:
//! unpack MTK RAW10 -> black level -> bilinear demosaic (GBRG) -> white balance
//! -> scale to the output size -> tone curve -> YUV -> chroma blur + motion-
//! adaptive temporal denoise -> YUYV (BT.601 limited range).
use rayon::prelude::*;

pub const W: usize = 1920;
pub const H: usize = 1080;
pub const RAW_STRIDE: usize = 2400; // MTK packed 10-bit
const BLACK: i32 = 64;
pub const WHITE: i32 = 1023 - BLACK;
const PW: usize = W + 2; // raw plane with a 1-pixel mirrored border
/// line time of the sensor mode (VTS 1462 at 30 fps)
pub const LINE_US: f32 = 1e6 / (30.0 * 1462.0);

pub struct Stats {
    /// mean of the green samples (black-level corrected, 0..WHITE)
    pub g: f32,
    /// mean R/G/B over unsaturated quads, for white balance
    pub rgb: [f32; 3],
}

pub struct Isp {
    pub out_w: usize,
    pub out_h: usize,
    raw: Vec<u16>,
    lin: Vec<[u16; 3]>, // demosaiced, white-balanced linear RGB (full res)
    tone: Vec<u8>,
    /// white balance gains, Q10 (1024 = 1.0): R, G, B
    pub wb: [u32; 3],
    /// denoise strength 0 (off) .. 2
    pub denoise: u32,
    yuv: [Vec<u8>; 3],
    prev: [Vec<u8>; 3],
    have_prev: bool,
    /// running flicker evidence for 100 Hz (50 Hz mains) and 120 Hz (60 Hz mains)
    pub flicker: [f32; 2],
}

impl Isp {
    pub fn new(out_w: usize, out_h: usize, denoise: u32) -> Self {
        // gamma 1/2.2 with a gentle S-curve on top for some contrast
        let tone = (0..=WHITE)
            .map(|v| {
                let x = (v as f32 / WHITE as f32).powf(1.0 / 2.2);
                let s = x + 0.12 * x * (1.0 - x) * (2.0 * x - 1.0) * 2.0;
                (s.clamp(0.0, 1.0) * 255.0 + 0.5) as u8
            })
            .collect();
        let n = out_w * out_h;
        Isp {
            out_w,
            out_h,
            raw: vec![0; PW * (H + 2)],
            lin: vec![[0; 3]; W * H],
            tone,
            wb: [1600, 1024, 1600],
            denoise,
            yuv: [vec![0; n], vec![128; n], vec![128; n]],
            prev: [vec![0; n], vec![128; n], vec![128; n]],
            have_prev: false,
            flicker: [0.0; 2],
        }
    }

    pub fn out_size(&self) -> usize {
        self.out_w * self.out_h * 2
    }

    /// MTK packing: little-endian bitstream, pixel k of a 5-byte group = bits [10k, 10k+10).
    pub fn unpack(&mut self, src: &[u8]) {
        self.raw[PW..PW * (H + 1)]
            .par_chunks_mut(PW)
            .zip(src.par_chunks(RAW_STRIDE))
            .for_each(|(dst, s)| {
                for (g, d) in s[..W * 5 / 4].chunks_exact(5).zip(dst[1..W + 1].chunks_exact_mut(4)) {
                    let v = u64::from(g[0]) | u64::from(g[1]) << 8 | u64::from(g[2]) << 16
                        | u64::from(g[3]) << 24 | u64::from(g[4]) << 32;
                    for (k, px) in d.iter_mut().enumerate() {
                        *px = ((v >> (10 * k)) & 0x3ff).saturating_sub(BLACK as u64) as u16;
                    }
                }
                dst[0] = dst[2]; // mirror keeps the Bayer phase
                dst[W + 1] = dst[W - 1];
            });
        self.raw.copy_within(2 * PW..3 * PW, 0);
        self.raw.copy_within((H - 1) * PW..H * PW, (H + 1) * PW);
    }

    pub fn stats(&self) -> Stats {
        let (mut g_all, mut n_all) = (0u64, 0u64);
        let (mut sum, mut n) = ([0u64; 3], 0u64);
        for y in (0..H).step_by(8) {
            let r0 = &self.raw[(y + 1) * PW + 1..];
            let r1 = &self.raw[(y + 2) * PW + 1..];
            for x in (0..W).step_by(8) {
                let (g1, b, r, g2) = (r0[x] as u64, r0[x + 1] as u64, r1[x] as u64, r1[x + 1] as u64);
                g_all += g1 + g2;
                n_all += 2;
                let hi = g1.max(g2).max(r).max(b);
                let lo = g1.min(g2).min(r).min(b);
                if hi < (WHITE as u64) * 9 / 10 && lo > 8 {
                    sum[0] += r;
                    sum[1] += (g1 + g2) / 2;
                    sum[2] += b;
                    n += 1;
                }
            }
        }
        let n = n.max(1) as f32;
        Stats {
            g: g_all as f32 / n_all.max(1) as f32,
            rgb: [sum[0] as f32 / n, sum[1] as f32 / n, sum[2] as f32 / n],
        }
    }

    /// Flicker detection: rolling-shutter banding shows up as a sinusoid in the
    /// row means, with a period of (1 / (2 * mains)) / line time rows. Compare the
    /// detrended row profile's energy at the 100 Hz and 120 Hz periods.
    pub fn update_flicker(&mut self) {
        let rows: Vec<f32> = (0..H)
            .into_par_iter()
            .map(|y| {
                let r = &self.raw[(y + 1) * PW + 1..(y + 1) * PW + 1 + W];
                // green sites: even rows even x, odd rows odd x
                r.iter().skip(y & 1).step_by(8).map(|&v| v as f32).sum::<f32>()
            })
            .collect();
        let mean = rows.iter().sum::<f32>() / H as f32;
        if mean < 20.0 {
            return;
        }
        // detrend with a wide moving average (wider than either band period)
        let win = 151usize;
        let mut prof = vec![0f32; H];
        for y in 0..H {
            let (a, b) = (y.saturating_sub(win / 2), (y + win / 2 + 1).min(H));
            let m = rows[a..b].iter().sum::<f32>() / (b - a) as f32;
            prof[y] = rows[y] / m.max(1.0) - 1.0;
        }
        for (i, hz) in [100.0f32, 120.0].iter().enumerate() {
            let period = 1e6 / hz / LINE_US;
            let (mut c, mut s) = (0f32, 0f32);
            for (y, p) in prof.iter().enumerate() {
                let ph = std::f32::consts::TAU * y as f32 / period;
                c += p * ph.cos();
                s += p * ph.sin();
            }
            let mag = (c * c + s * s).sqrt() / H as f32;
            self.flicker[i] = self.flicker[i] * 0.95 + mag * 0.05;
        }
    }

    /// Gray-world white balance, smoothed.
    pub fn update_wb(&mut self, s: &Stats) {
        let [r, g, b] = s.rgb;
        if r < 4.0 || g < 4.0 || b < 4.0 {
            return;
        }
        for (i, c) in [(0, r), (2, b)] {
            let target = (g / c * 1024.0).clamp(512.0, 4096.0);
            self.wb[i] = (self.wb[i] as f32 * 0.9 + target * 0.1) as u32;
        }
    }

    /// Full pipeline into a YUYV frame of out_w x out_h. `gain` is the total
    /// sensor gain (analogue x digital), used to scale the denoiser.
    pub fn render_yuyv(&mut self, out: &mut [u8], gain: f32) {
        self.demosaic();
        self.scale_tone_yuv();
        if self.denoise > 0 {
            self.denoise_frame(gain);
        }
        self.pack(out);
    }

    fn demosaic(&mut self) {
        let raw = &self.raw;
        let (gr, gg, gb) = (self.wb[0] as i32, self.wb[1] as i32, self.wb[2] as i32);
        self.lin.par_chunks_mut(W).enumerate().for_each(|(y, row)| {
            let up = &raw[y * PW..];
            let cu = &raw[(y + 1) * PW..];
            let dn = &raw[(y + 2) * PW..];
            let odd_row = y & 1 == 1; // GBRG: even rows G B G B, odd rows R G R G
            for (x, o) in row.iter_mut().enumerate() {
                let c = x + 1;
                let v = cu[c] as i32;
                let lr = (cu[c - 1] as i32 + cu[c + 1] as i32) / 2;
                let ud = (up[c] as i32 + dn[c] as i32) / 2;
                let cross = (cu[c - 1] as i32 + cu[c + 1] as i32 + up[c] as i32 + dn[c] as i32) / 4;
                let diag = (up[c - 1] as i32 + up[c + 1] as i32 + dn[c - 1] as i32 + dn[c + 1] as i32) / 4;
                let (r, g, b) = match (odd_row, x & 1 == 1) {
                    (false, false) => (ud, v, lr),     // G on a B row
                    (false, true) => (diag, cross, v), // B
                    (true, false) => (v, cross, diag), // R
                    (true, true) => (lr, v, ud),       // G on an R row
                };
                // clip after WB so saturated areas go white, not pink
                let f = |v: i32, gain: i32| ((v * gain) >> 10).clamp(0, WHITE) as u16;
                *o = [f(r, gr), f(g, gg), f(b, gb)];
            }
        });
    }

    /// Scale (bilinear) to the output size, apply the tone curve, convert to YUV.
    fn scale_tone_yuv(&mut self) {
        let (ow, oh) = (self.out_w, self.out_h);
        let (sx, sy) = (W as f32 / ow as f32, H as f32 / oh as f32);
        let lin = &self.lin;
        let tone = &self.tone;
        let [py, pu, pv] = &mut self.yuv;
        py.par_chunks_mut(ow)
            .zip(pu.par_chunks_mut(ow))
            .zip(pv.par_chunks_mut(ow))
            .enumerate()
            .for_each(|(oy, ((ry, ru), rv))| {
                let fy = ((oy as f32 + 0.5) * sy - 0.5).max(0.0);
                let y0 = (fy as usize).min(H - 1);
                let y1 = (y0 + 1).min(H - 1);
                let wy = fy - y0 as f32;
                for ox in 0..ow {
                    let fx = ((ox as f32 + 0.5) * sx - 0.5).max(0.0);
                    let x0 = (fx as usize).min(W - 1);
                    let x1 = (x0 + 1).min(W - 1);
                    let wx = fx - x0 as f32;
                    let mut rgb = [0u8; 3];
                    for (ch, o) in rgb.iter_mut().enumerate() {
                        let a = lin[y0 * W + x0][ch] as f32;
                        let b = lin[y0 * W + x1][ch] as f32;
                        let c = lin[y1 * W + x0][ch] as f32;
                        let d = lin[y1 * W + x1][ch] as f32;
                        let v = (a + (b - a) * wx) * (1.0 - wy) + (c + (d - c) * wx) * wy;
                        *o = tone[v as usize];
                    }
                    let (r, g, b) = (rgb[0] as i32, rgb[1] as i32, rgb[2] as i32);
                    ry[ox] = (((66 * r + 129 * g + 25 * b + 128) >> 8) + 16) as u8;
                    // chroma with a little extra saturation (x1.2)
                    let u = (-38 * r - 74 * g + 112 * b + 128) >> 8;
                    let v = (112 * r - 94 * g - 18 * b + 128) >> 8;
                    ru[ox] = (u * 6 / 5 + 128).clamp(16, 240) as u8;
                    rv[ox] = (v * 6 / 5 + 128).clamp(16, 240) as u8;
                }
            });
    }

    /// 3x3 box blur on chroma, then motion-adaptive temporal blend on all planes.
    fn denoise_frame(&mut self, gain: f32) {
        let (w, h) = (self.out_w, self.out_h);
        for p in 1..3 {
            let src = self.yuv[p].clone();
            self.yuv[p].par_chunks_mut(w).enumerate().for_each(|(y, row)| {
                let (ya, yb) = (y.saturating_sub(1), (y + 1).min(h - 1));
                for x in 0..w {
                    let (xa, xb) = (x.saturating_sub(1), (x + 1).min(w - 1));
                    let mut s = 0u32;
                    for yy in [ya, y, yb] {
                        let r = &src[yy * w..];
                        s += r[xa] as u32 + r[x] as u32 + r[xb] as u32;
                    }
                    row[x] = (s / 9) as u8;
                }
            });
        }
        if !self.have_prev {
            for p in 0..3 {
                self.prev[p].copy_from_slice(&self.yuv[p]);
            }
            self.have_prev = true;
            return;
        }
        // threshold grows with sensor gain (noise ~ sqrt(gain)); strength 1 or 2
        let thr = (4.0 + 3.0 * gain.max(1.0).sqrt()) * self.denoise as f32 / 1.5;
        let keep = if self.denoise >= 2 { 0.7 } else { 0.55 }; // weight of the previous frame
        for p in 0..3 {
            let t = if p == 0 { thr } else { thr * 0.7 };
            let (cur, prev) = (&mut self.yuv[p], &mut self.prev[p]);
            cur.par_iter_mut().zip(prev.par_iter_mut()).for_each(|(c, pv)| {
                let d = (*c as f32 - *pv as f32).abs();
                // full blend for small differences, fade out to none at 2x threshold
                let k = keep * (1.0 - ((d - t) / t).clamp(0.0, 1.0));
                let v = *c as f32 + (*pv as f32 - *c as f32) * k;
                *c = (v + 0.5) as u8;
                *pv = *c;
            });
        }
    }

    fn pack(&self, out: &mut [u8]) {
        let w = self.out_w;
        let [py, pu, pv] = &self.yuv;
        out.par_chunks_mut(w * 2).enumerate().for_each(|(y, row)| {
            let (ry, ru, rv) = (&py[y * w..], &pu[y * w..], &pv[y * w..]);
            for (x2, o) in row.chunks_exact_mut(4).enumerate() {
                let (a, b) = (2 * x2, 2 * x2 + 1);
                o[0] = ry[a];
                o[1] = ((ru[a] as u16 + ru[b] as u16) / 2) as u8;
                o[2] = ry[b];
                o[3] = ((rv[a] as u16 + rv[b] as u16) / 2) as u8;
            }
        });
    }
}
