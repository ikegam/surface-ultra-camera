//! Minimal V4L2 over libc: just the ioctls sucamd needs (arm64/x86_64 layouts).
use anyhow::{Context, Result};
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::mem::{size_of, zeroed};
use std::os::fd::AsRawFd;
use std::path::Path;

pub const BUF_TYPE_CAPTURE: u32 = 1;
pub const BUF_TYPE_OUTPUT: u32 = 2;
const MEMORY_MMAP: u32 = 1;
pub const CID_EXPOSURE: u32 = 0x0098_0911;
pub const CID_ANALOGUE_GAIN: u32 = 0x009e_0903;
pub const CID_DIGITAL_GAIN: u32 = 0x009f_0905;

pub const fn fourcc(s: &[u8; 4]) -> u32 {
    s[0] as u32 | (s[1] as u32) << 8 | (s[2] as u32) << 16 | (s[3] as u32) << 24
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct PixFormat {
    pub width: u32,
    pub height: u32,
    pub pixelformat: u32,
    pub field: u32,
    pub bytesperline: u32,
    pub sizeimage: u32,
    pub colorspace: u32,
    pub priv_: u32,
    pub flags: u32,
    pub ycbcr_enc: u32,
    pub quantization: u32,
    pub xfer_func: u32,
}

#[repr(C)]
struct Format {
    typ: u32,
    _pad: u32,
    pix: PixFormat,
    _rest: [u8; 200 - 48],
}

#[repr(C)]
struct RequestBuffers {
    count: u32,
    typ: u32,
    memory: u32,
    capabilities: u32,
    flags: u8,
    reserved: [u8; 3],
}

#[repr(C)]
struct Buffer {
    index: u32,
    typ: u32,
    bytesused: u32,
    flags: u32,
    field: u32,
    _pad0: u32,
    ts_sec: i64,
    ts_usec: i64,
    timecode: [u32; 4],
    sequence: u32,
    memory: u32,
    m: u64, // union: mmap offset in the low 32 bits
    length: u32,
    reserved2: u32,
    request_fd: u32,
    _pad1: u32,
}

#[repr(C)]
struct Control {
    id: u32,
    value: i32,
}

#[repr(C)]
struct EventSubscription {
    typ: u32,
    id: u32,
    flags: u32,
    reserved: [u32; 5],
}

#[repr(C)]
pub struct Event {
    pub typ: u32,
    _pad: u32,
    pub data: [u8; 64],
    pub pending: u32,
    pub sequence: u32,
    ts_sec: i64,
    ts_nsec: i64,
    pub id: u32,
    reserved: [u32; 8],
}

const _: () = {
    assert!(size_of::<Format>() == 208);
    assert!(size_of::<RequestBuffers>() == 20);
    assert!(size_of::<Buffer>() == 88);
    assert!(size_of::<EventSubscription>() == 32);
    assert!(size_of::<Event>() == 136);
};

const fn ioc(dir: u64, nr: u64, size: usize) -> libc::c_ulong {
    ((dir << 30) | ((size as u64) << 16) | ((b'V' as u64) << 8) | nr) as libc::c_ulong
}
const IOW: u64 = 1;
const IOR: u64 = 2;
const IOWR: u64 = 3;
const VIDIOC_S_FMT: libc::c_ulong = ioc(IOWR, 5, size_of::<Format>());
const VIDIOC_REQBUFS: libc::c_ulong = ioc(IOWR, 8, size_of::<RequestBuffers>());
const VIDIOC_QUERYBUF: libc::c_ulong = ioc(IOWR, 9, size_of::<Buffer>());
const VIDIOC_QBUF: libc::c_ulong = ioc(IOWR, 15, size_of::<Buffer>());
const VIDIOC_DQBUF: libc::c_ulong = ioc(IOWR, 17, size_of::<Buffer>());
const VIDIOC_STREAMON: libc::c_ulong = ioc(IOW, 18, 4);
const VIDIOC_STREAMOFF: libc::c_ulong = ioc(IOW, 19, 4);
const VIDIOC_S_CTRL: libc::c_ulong = ioc(IOWR, 28, size_of::<Control>());
const VIDIOC_DQEVENT: libc::c_ulong = ioc(IOR, 89, size_of::<Event>());
const VIDIOC_SUBSCRIBE_EVENT: libc::c_ulong = ioc(IOW, 90, size_of::<EventSubscription>());

pub struct Dev {
    f: File,
}

impl Dev {
    pub fn open(path: &Path) -> Result<Self> {
        let f = OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)
            .with_context(|| format!("open {}", path.display()))?;
        Ok(Dev { f })
    }

    fn ioctl<T>(&self, req: libc::c_ulong, arg: &mut T) -> std::io::Result<()> {
        loop {
            let r = unsafe { libc::ioctl(self.f.as_raw_fd(), req as _, arg as *mut T) };
            if r != -1 {
                return Ok(());
            }
            let e = std::io::Error::last_os_error();
            if e.kind() != std::io::ErrorKind::Interrupted {
                return Err(e);
            }
        }
    }

    pub fn set_format(&self, typ: u32, pix: PixFormat) -> Result<PixFormat> {
        let mut f: Format = unsafe { zeroed() };
        f.typ = typ;
        f.pix = pix;
        self.ioctl(VIDIOC_S_FMT, &mut f).context("VIDIOC_S_FMT")?;
        Ok(f.pix)
    }

    pub fn set_ctrl(&self, id: u32, value: i32) -> Result<()> {
        let mut c = Control { id, value };
        self.ioctl(VIDIOC_S_CTRL, &mut c).with_context(|| format!("S_CTRL {id:#x}"))
    }

    pub fn subscribe(&self, typ: u32) -> std::io::Result<()> {
        let mut s: EventSubscription = unsafe { zeroed() };
        s.typ = typ;
        self.ioctl(VIDIOC_SUBSCRIBE_EVENT, &mut s)
    }

    /// Wait up to `timeout_ms` for an event; returns it if one arrived.
    pub fn wait_event(&self, timeout_ms: i32) -> Option<Event> {
        if !self.poll(libc::POLLPRI, timeout_ms) {
            return None;
        }
        let mut ev: Event = unsafe { zeroed() };
        self.ioctl(VIDIOC_DQEVENT, &mut ev).ok().map(|_| ev)
    }

    pub fn poll(&self, events: libc::c_short, timeout_ms: i32) -> bool {
        let mut p = libc::pollfd { fd: self.f.as_raw_fd(), events, revents: 0 };
        let r = unsafe { libc::poll(&mut p, 1, timeout_ms) };
        r > 0 && (p.revents & events) != 0
    }

    pub fn write_frame(&self, data: &[u8]) -> std::io::Result<()> {
        (&self.f).write_all(data)
    }
}

/// An MMAP capture stream; dropping it stops streaming and frees the buffers.
pub struct Capture<'a> {
    dev: &'a Dev,
    bufs: Vec<(*mut u8, usize)>,
}

impl<'a> Capture<'a> {
    pub fn start(dev: &'a Dev, count: u32) -> Result<Self> {
        let mut rb = RequestBuffers { count, typ: BUF_TYPE_CAPTURE, memory: MEMORY_MMAP,
                                      capabilities: 0, flags: 0, reserved: [0; 3] };
        dev.ioctl(VIDIOC_REQBUFS, &mut rb).context("VIDIOC_REQBUFS")?;
        let mut cap = Capture { dev, bufs: Vec::new() };
        for i in 0..rb.count {
            let mut b = Self::blank(i);
            dev.ioctl(VIDIOC_QUERYBUF, &mut b).context("VIDIOC_QUERYBUF")?;
            let p = unsafe {
                libc::mmap(std::ptr::null_mut(), b.length as usize, libc::PROT_READ,
                           libc::MAP_SHARED, dev.f.as_raw_fd(), (b.m & 0xffff_ffff) as libc::off_t)
            };
            anyhow::ensure!(p != libc::MAP_FAILED, "mmap: {}", std::io::Error::last_os_error());
            cap.bufs.push((p as *mut u8, b.length as usize));
            cap.queue(i as usize)?;
        }
        let mut t = BUF_TYPE_CAPTURE;
        dev.ioctl(VIDIOC_STREAMON, &mut t).context("VIDIOC_STREAMON")?;
        Ok(cap)
    }

    fn blank(index: u32) -> Buffer {
        let mut b: Buffer = unsafe { zeroed() };
        b.index = index;
        b.typ = BUF_TYPE_CAPTURE;
        b.memory = MEMORY_MMAP;
        b
    }

    pub fn queue(&self, index: usize) -> Result<()> {
        let mut b = Self::blank(index as u32);
        self.dev.ioctl(VIDIOC_QBUF, &mut b).context("VIDIOC_QBUF")
    }

    /// Next filled buffer index, or an error after `timeout_ms` without a frame.
    pub fn dequeue(&self, timeout_ms: i32) -> Result<usize> {
        anyhow::ensure!(self.dev.poll(libc::POLLIN, timeout_ms), "no frame for {timeout_ms} ms");
        let mut b = Self::blank(0);
        self.dev.ioctl(VIDIOC_DQBUF, &mut b).context("VIDIOC_DQBUF")?;
        Ok(b.index as usize)
    }

    pub fn buf(&self, index: usize) -> &[u8] {
        let (p, len) = self.bufs[index];
        unsafe { std::slice::from_raw_parts(p, len) }
    }
}

impl Drop for Capture<'_> {
    fn drop(&mut self) {
        let mut t = BUF_TYPE_CAPTURE;
        let _ = self.dev.ioctl(VIDIOC_STREAMOFF, &mut t);
        for &(p, len) in &self.bufs {
            unsafe { libc::munmap(p as *mut _, len) };
        }
        let mut rb = RequestBuffers { count: 0, typ: BUF_TYPE_CAPTURE, memory: MEMORY_MMAP,
                                      capabilities: 0, flags: 0, reserved: [0; 3] };
        let _ = self.dev.ioctl(VIDIOC_REQBUFS, &mut rb);
    }
}
