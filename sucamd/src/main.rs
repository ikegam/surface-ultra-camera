//! sucamd: turns the Surface Laptop Ultra raw camera (surface_ultra_cam) into a
//! normal webcam via v4l2loopback. The raw stream (and the privacy LED) only
//! runs while some application is reading the loopback device.
mod isp;
mod v4l2;

use anyhow::{Context, Result};
use std::path::PathBuf;
use std::time::{Duration, Instant};
use v4l2::{Capture, Dev, PixFormat};

const RAW_NAME: &str = "Surface Laptop Ultra Camera";
const LOOP_NAME: &str = "Surface Camera";
/// v4l2loopback: V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1, payload { u32 count }
const EVENT_CLIENT_USAGE: u32 = 0x0800_0000 + 0x08E0_0000 + 1;
const IDLE_STOP: Duration = Duration::from_secs(2);

fn find_dev(name: &str) -> Option<PathBuf> {
    std::fs::read_dir("/sys/class/video4linux").ok()?.flatten().find_map(|e| {
        let n = std::fs::read_to_string(e.path().join("name")).ok()?;
        (n.trim() == name).then(|| PathBuf::from("/dev").join(e.file_name()))
    })
}

/// Exposure = lines x analogue gain (/16) x digital gain (/1024), split in that order.
struct Ae {
    exp: i32,
    again: i32,
    dgain: i32,
    /// flicker period in sensor lines (half a mains cycle), once known
    flicker_lines: Option<f32>,
}

impl Ae {
    const TARGET: f32 = 180.0; // mean green, black-level corrected (0..959)
    const EXP_MAX: f32 = 1454.0; // VTS 1462 - 8

    fn apply(&self, raw: &Dev) {
        let _ = raw.set_ctrl(v4l2::CID_EXPOSURE, self.exp);
        let _ = raw.set_ctrl(v4l2::CID_ANALOGUE_GAIN, self.again);
        let _ = raw.set_ctrl(v4l2::CID_DIGITAL_GAIN, self.dgain);
    }

    fn gain(&self) -> f32 {
        self.again as f32 / 16.0 * self.dgain as f32 / 1024.0
    }

    /// returns true if the controls changed
    fn update(&mut self, mean_g: f32) -> bool {
        let ratio = (Self::TARGET / mean_g.max(1.0)).clamp(0.25, 4.0).powf(0.6);
        if (ratio - 1.0).abs() < 0.04 {
            return false;
        }
        let total = self.exp as f32 * self.gain() * ratio;
        let mut exp = total.clamp(4.0, Self::EXP_MAX);
        // under flickering light, keep the exposure a whole number of flicker
        // periods so every row integrates the same amount of light (no banding)
        if let Some(p) = self.flicker_lines {
            if exp >= p {
                exp = ((exp / p).floor() * p).min(Self::EXP_MAX);
            }
        }
        let rest = total / exp;
        let again = (rest * 16.0).round().clamp(16.0, 248.0);
        let dgain = (rest * 16.0 / again * 1024.0).round().clamp(1024.0, 16383.0);
        let new = (exp.round() as i32, again as i32, dgain as i32);
        let changed = new != (self.exp, self.again, self.dgain);
        (self.exp, self.again, self.dgain) = new;
        changed
    }
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Mains {
    Auto,
    Hz(u32),
    Off,
}

struct Opts {
    w: usize,
    h: usize,
    mains: Mains,
    denoise: u32,
}

impl Opts {
    fn parse(args: &[String]) -> Result<Self> {
        let mut o = Opts { w: 1280, h: 720, mains: Mains::Auto, denoise: 1 };
        let mut it = args.iter();
        while let Some(a) = it.next() {
            let mut val = || it.next().with_context(|| format!("{a} needs a value"));
            match a.as_str() {
                "--size" => {
                    let v = val()?;
                    let (w, h) = v.split_once('x').with_context(|| format!("bad size {v}"))?;
                    (o.w, o.h) = (w.parse()?, h.parse()?);
                    anyhow::ensure!(o.w % 2 == 0 && o.w <= isp::W && o.h <= isp::H, "size {v}");
                }
                "--mains" => {
                    o.mains = match val()?.as_str() {
                        "auto" => Mains::Auto,
                        "50" => Mains::Hz(50),
                        "60" => Mains::Hz(60),
                        "off" => Mains::Off,
                        v => anyhow::bail!("--mains 50|60|auto|off, not {v}"),
                    }
                }
                "--denoise" => o.denoise = val()?.parse::<u32>()?.min(2),
                v => anyhow::bail!("unknown option {v} (--size WxH, --mains 50|60|auto|off, --denoise 0-2)"),
            }
        }
        Ok(o)
    }
}

fn flicker_lines(hz: u32) -> f32 {
    1e6 / (2.0 * hz as f32) / isp::LINE_US
}

struct Loopback {
    dev: Dev,
    events: bool,
    path: PathBuf,
}

impl Loopback {
    fn open(w: usize, h: usize) -> Result<Self> {
        let path = find_dev(LOOP_NAME).with_context(|| {
            format!("v4l2loopback device \"{LOOP_NAME}\" not found \
                     (modprobe v4l2loopback exclusive_caps=1 card_label=\"{LOOP_NAME}\")")
        })?;
        let dev = Dev::open(&path)?;
        dev.set_format(v4l2::BUF_TYPE_OUTPUT, PixFormat {
            width: w as u32,
            height: h as u32,
            pixelformat: v4l2::fourcc(b"YUYV"),
            field: 1, // V4L2_FIELD_NONE
            bytesperline: (w * 2) as u32,
            sizeimage: (w * h * 2) as u32,
            colorspace: 1, // SMPTE170M
            ..Default::default()
        })?;
        let events = dev.subscribe(EVENT_CLIENT_USAGE).is_ok();
        if !events {
            eprintln!("sucamd: no client-usage events, falling back to scanning /proc");
        }
        Ok(Loopback { dev, events, path })
    }

    /// Number of readers, if it changed (event) or by scanning open fds.
    fn clients(&self, timeout_ms: i32) -> Option<u32> {
        if self.events {
            let ev = self.dev.wait_event(timeout_ms)?;
            (ev.typ == EVENT_CLIENT_USAGE)
                .then(|| u32::from_ne_bytes(ev.data[..4].try_into().unwrap()))
        } else {
            std::thread::sleep(Duration::from_millis(timeout_ms.max(0) as u64));
            Some(count_openers(&self.path))
        }
    }
}

fn count_openers(path: &std::path::Path) -> u32 {
    let me = std::process::id().to_string();
    let Ok(procs) = std::fs::read_dir("/proc") else { return 0 };
    procs
        .flatten()
        .filter(|p| p.file_name().to_str().is_some_and(|s| s != me && s.bytes().all(|b| b.is_ascii_digit())))
        .filter(|p| {
            std::fs::read_dir(p.path().join("fd")).is_ok_and(|fds| {
                fds.flatten().any(|fd| std::fs::read_link(fd.path()).is_ok_and(|t| t == path))
            })
        })
        .count() as u32
}

/// Stream the raw camera into the loopback until nobody has been watching for IDLE_STOP.
fn run_session(raw: &Dev, lp: &Loopback, isp: &mut isp::Isp, ae: &mut Ae, out: &mut [u8],
               mains: Mains) -> Result<()> {
    ae.apply(raw);
    let cap = Capture::start(raw, 4)?;
    eprintln!("sucamd: streaming");
    let (mut clients, mut idle_since, mut frames) = (1u32, None::<Instant>, 0u64);
    let mut t0 = Instant::now();
    let mut tm = [Duration::ZERO; 4]; // wait, isp, ae, write
    loop {
        let t = Instant::now();
        let i = cap.dequeue(2000)?;
        tm[0] += t.elapsed();
        let t = Instant::now();
        isp.unpack(cap.buf(i));
        cap.queue(i)?;
        let st = isp.stats();
        isp.update_wb(&st);
        if mains == Mains::Auto && ae.flicker_lines.is_none() && frames % 3 == 0 {
            isp.update_flicker();
            // decide once there is enough evidence (about 2 s)
            if frames >= 60 {
                let [f50, f60] = isp.flicker;
                let (hi, lo, hz) = if f50 > f60 { (f50, f60, 50) } else { (f60, f50, 60) };
                if hi > 0.004 && hi > 1.8 * lo {
                    ae.flicker_lines = Some(flicker_lines(hz));
                    eprintln!("sucamd: {hz} Hz flicker detected ({f50:.4}/{f60:.4}), locking exposure");
                }
            }
        }
        isp.render_yuyv(out, ae.gain());
        tm[1] += t.elapsed();
        let t = Instant::now();
        if frames % 2 == 0 && ae.update(st.g) {
            ae.apply(raw);
        }
        tm[2] += t.elapsed();
        let t = Instant::now();
        lp.dev.write_frame(out)?;
        tm[3] += t.elapsed();
        frames += 1;
        if frames % 900 == 0 { // every ~30 s
            let ms = |d: Duration| d.as_secs_f64() * 1000.0 / 900.0;
            eprintln!("sucamd: {:.1} fps | ms/frame wait {:.1} isp {:.1} ae {:.1} write {:.1} | g {:.0} exp {} again {} dgain {} wb {:?} flicker {:?}",
                      900.0 / t0.elapsed().as_secs_f64(), ms(tm[0]), ms(tm[1]), ms(tm[2]), ms(tm[3]),
                      st.g, ae.exp, ae.again, ae.dgain, isp.wb, isp.flicker);
            t0 = Instant::now();
            tm = [Duration::ZERO; 4];
        }
        if let Some(c) = lp.clients(0) {
            clients = c;
        }
        if clients == 0 {
            let since = *idle_since.get_or_insert_with(Instant::now);
            if since.elapsed() > IDLE_STOP {
                eprintln!("sucamd: no readers, camera off");
                return Ok(());
            }
        } else {
            idle_since = None;
        }
    }
}

/// `sucamd test <raw> <out.yuyv> [opts]`: run the ISP on a saved frame (a few
/// iterations so white balance and the temporal filter settle), report timing.
fn test_file(src: &str, dst: &str, o: &Opts) -> Result<()> {
    let data = std::fs::read(src)?;
    let mut isp = isp::Isp::new(o.w, o.h, o.denoise);
    let mut out = vec![0u8; isp.out_size()];
    let t = Instant::now();
    let n = 30;
    for _ in 0..n {
        isp.unpack(&data);
        let st = isp.stats();
        isp.update_wb(&st);
        isp.update_flicker();
        isp.render_yuyv(&mut out, 2.0);
    }
    let st = isp.stats();
    println!("{}x{}: {:.1} ms/frame, mean g {:.0}, wb {:?}, flicker 100/120 Hz {:?}",
             o.w, o.h, t.elapsed().as_secs_f64() * 1000.0 / n as f64, st.g, isp.wb, isp.flicker);
    std::fs::write(dst, &out)?;
    Ok(())
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() >= 3 && args[0] == "test" {
        return test_file(&args[1], &args[2], &Opts::parse(&args[3..])?);
    }
    let o = Opts::parse(&args)?;
    let raw_path = find_dev(RAW_NAME).context("surface_ultra_cam video node not found")?;
    let raw = Dev::open(&raw_path)?;
    let lp = Loopback::open(o.w, o.h)?;
    eprintln!("sucamd: {} -> {} ({}x{}, mains {:?}, denoise {})",
              raw_path.display(), lp.path.display(), o.w, o.h, o.mains, o.denoise);

    let mut isp = isp::Isp::new(o.w, o.h, o.denoise);
    let mut ae = Ae {
        exp: 1100,
        again: 32,
        dgain: 1024,
        flicker_lines: match o.mains {
            Mains::Hz(hz) => Some(flicker_lines(hz)),
            _ => None,
        },
    };
    let mut out = vec![0u8; isp.out_size()];
    // a dark frame up front so the loopback advertises itself as a capture device
    for px in out.chunks_exact_mut(2) {
        px.copy_from_slice(&[16, 128]);
    }
    lp.dev.write_frame(&out)?;

    loop {
        match lp.clients(1000) {
            Some(n) if n > 0 => {
                if let Err(e) = run_session(&raw, &lp, &mut isp, &mut ae, &mut out, o.mains) {
                    eprintln!("sucamd: stream error: {e:#}; retrying in 3 s");
                    std::thread::sleep(Duration::from_secs(3));
                }
            }
            _ => {}
        }
    }
}
