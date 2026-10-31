# IR camera (work in progress)

(Japanese version: [ir-camera_jp.md](ir-camera_jp.md))

Notes on getting the front IR camera (used for face authentication) working on Linux.
**It does not work yet.** This page collects what is known and what is left to do.

## What is known

| Item | Value | Source |
|---|---|---|
| ACPI | `\_SB.CAMI`, HID `SMO55F1`, `_SUB` `MSHW0732` | SSDT |
| Sensor | ST VD55G1 (monochrome, up to 804x704, 1-lane CSI-2) | Windows INF (`vd55g1_mipi_raw`), chip ID |
| I2C | ACPI I2C4 (on Linux, the parent bus of `i2c-SMO55F1:00`), address 0x10, 400 kHz | `_CRS` |
| GPIOs (gpiochip0) | campower 35, avdd 104, reset 101 (active-low), pled 43 (shared with the front camera) | `_CRS` and the `_DSM` name table |
| Clock | There is no dedicated clock GPIO. The sensor answers on I2C without the front camera's gate (GPIO 76) | measured |
| Chip ID | `0x53354731` (VD55G1, revision 2). A firmware patch must be written to the sensor at every power-up (the patch is in the mainline `vd55g1` driver) | measured |
| CSI port | 2 (PHY at `0x12A20000`; the front camera uses port 0 at `0x12A10000`) | INF `SensorCsiPort=2`, ACPI `HWIF` |
| IR emitter | `IRStrobeControlMode=2` (the emitter is driven from a sensor GPIO) | INF |

`tools/ir-probe.sh` powers the sensor and reads its chip ID.

```
sudo ./tools/ir-probe.sh
```

## What is left

1. **Capture the port 2 receiver setup.** The front camera's CSI receiver setup (rxana,
   seninf-top, async) is a replay of a CCU command sequence captured on Windows. The port
   number, lane count and data rate are opaque arguments inside those commands, so
   rewriting the front camera's sequence for port 2 is risky. Version 4 of
   `tools/windows-ccusnoop` records ports 0-2 and camsv a/b/e; the plan is to capture
   while Windows Hello is running the IR camera (see the README in that directory).
2. **Pin down the sensor settings.** The external clock frequency, the MIPI data rate and
   the sensor GPIO used for the emitter are still unknown. They can be read from the
   Windows sensor driver's tables or found by trial on the hardware.
3. **Add it to the driver.** As with the front camera, the plan is to drive the sensor
   directly from `surface_ultra_cam` (reusing the VD55G1 init sequence and patch from the
   GPL mainline driver) and expose a second video node (GREY / Y10).
4. **Hook up face authentication.** Point Howdy or similar at the IR video node. Always
   keep password login available: this approach is less resistant to spoofing than
   Windows Hello.
