# Hardware quirks

(Japanese: [quirks_jp.md](quirks_jp.md))

These are things that cannot be learned from the register descriptions and took
a while to find. All of them are handled by the driver.

## ROT0 can only read low DRAM

When the image buffer sits high in memory, even within the first 4 GB, ROT0
rejects the firmware load with status `0x3ff`. Buffers around `0xfa000000`
failed, while `0x80080000` - `0x80e00000` succeeded, even though IORT gives
ROT0 a 32-bit address limit. The page allocator returns either kind of
location, so this looked like a random "it breaks after a warm reboot" problem.
`surface_ccu_boot` takes 64 candidate buffers and uses the lowest one, and if
the load is still rejected it retries with another buffer.

## The sensor clock is not 19.2 MHz

The OV02C10 2-lane mode table (HTS 1140, VTS 2328) is designed to give 30 fps
with a 19.2 MHz MCLK, but runs at 18.8 fps on this machine. The line time is
22.8 µs, which fits a clock of about 12 MHz. The PLL is left untouched; instead
the frame length is shortened to VTS 1462 to get exactly 30 fps. The maximum
exposure is VTS - 8.

## The first stream after the CCU boots is not routed

On the first stream start right after the CCU firmware boots, the path does
not get connected: mac_csi decodes frames, but no data reaches any outmux.
From the second time on, the same sequence connects. The driver checks whether
data reaches outmux1 and, if it does not, restarts the stream once.

## Outmux registers are double-buffered, and zeros matter

Outmux settings only take effect once `SW_CFG_DONE` (`+0x1c`) is set. The
hardware clears this bit after it has latched the values at a frame boundary,
so it reads as 0 in a register dump of a streaming system and is easy to miss.

The reset value of `SRC_SEL` (`+0x18`) and `SOURCE_CONFIG_0` (`+0x0c`) is 3,
which means "unused". When the firmware routes the sensor, it skips every
outmux whose sensor select is 2 or more. Windows writes 0 to the outmuxes it
uses (1 and 7) and leaves the others at 3. So reproducing only the non-zero
values of a register dump gives a configuration the firmware never routes.

## camsv_b needs its own clocks

The camsv Windows uses is camsv_b at `0x3A510000`, not camsv_a. camsv_b needs
the clarb13 (`0xa00`) and camsv_b (`0xa18`) clocks; without them every write is
silently dropped and reads return 0.

## Per-frame DMA registers do not show up in register dumps

Windows writes the camsv DMA address, stride and FBC per frame through the
command queue, directly into the inner register bank (the one actually in use),
so they read as 0 in a dump of the outer registers. The driver writes the outer
registers and lets the double-buffer load latch them at the start of each
frame. Also, `0x000-0x01c` of the camsv DMA block are threshold settings, not
addresses.

## Reading a powered-off block freezes the CPU

An MMIO read of a camera block whose power domain is off stalls the bus, and
the task can no longer be killed. On stream start the driver asks HFRP for the
power domains before touching any camera register, so this is safe even after
system suspend.

## The stock ov02c10 driver is blacklisted

Mainline `ov02c10` binds to the same ACPI device as well, but on this machine
it cannot power the sensor (there is no INT3472 and no clock provider), and it
ends up fighting over the GPIOs.

## Other

- Do not write the FBC reset bit when starting a stream; frames stop arriving.
- `SOF_STATUS` and `DONE_STATUS` clear on read, so diagnostic reads must not
  touch them.
- Through v4l2loopback, `v4l2-ctl` shows a low fps at first, because the first
  buffer it receives is the placeholder frame sucamd wrote at startup, with an
  old timestamp. The real rate is 30 fps.
