//! IR camera path: 600x600 MTK-packed 10-bit mono -> centre crop to the output aspect
//! -> nearest-neighbour scale -> tone curve -> YUYV (grey). Exposure is the sensor's own AE.
use rayon::prelude::*;

pub const W: usize = 600;
pub const H: usize = 600;
pub const STRIDE: usize = 752; // 600 * 10 / 8 = 750, 16-byte aligned
const BLACK: usize = 64;

pub struct IrIsp {
    pub out_w: usize,
    pub out_h: usize,
    /// source column / row for each output pixel
    sx: Vec<usize>,
    sy: Vec<usize>,
    /// raw 10-bit value -> Y (BT.601 limited range)
    lut: Vec<u8>,
}

impl IrIsp {
    pub fn new(out_w: usize, out_h: usize) -> Self {
        // largest centred W x H crop with the output's aspect ratio
        let (cw, ch) = if out_w * H > out_h * W { (W, W * out_h / out_w) } else { (H * out_w / out_h, H) };
        let (x0, y0) = ((W - cw) / 2, (H - ch) / 2);
        let sx = (0..out_w).map(|x| x0 + x * cw / out_w).collect();
        let sy = (0..out_h).map(|y| y0 + y * ch / out_h).collect();
        let lut = (0..1024)
            .map(|v: usize| {
                let x = v.saturating_sub(BLACK) as f32 / (1023 - BLACK) as f32;
                (16.0 + 219.0 * x.powf(1.0 / 2.2)).round() as u8
            })
            .collect();
        IrIsp { out_w, out_h, sx, sy, lut }
    }

    pub fn out_size(&self) -> usize {
        self.out_w * self.out_h * 2
    }

    pub fn render_yuyv(&self, raw: &[u8], out: &mut [u8]) {
        out.par_chunks_exact_mut(self.out_w * 2).enumerate().for_each(|(y, row)| {
            let src = &raw[self.sy[y] * STRIDE..][..STRIDE];
            for (x, px) in row.chunks_exact_mut(2).enumerate() {
                // 4 pixels per 5 bytes, little-endian bitstream
                let sx = self.sx[x];
                let g = &src[sx / 4 * 5..][..5];
                let bits = u64::from_le_bytes([g[0], g[1], g[2], g[3], g[4], 0, 0, 0]);
                let v = (bits >> (10 * (sx % 4))) as usize & 0x3ff;
                px[0] = self.lut[v];
                px[1] = 128;
            }
        });
    }
}
