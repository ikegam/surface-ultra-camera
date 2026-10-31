#!/bin/bash
# Power the IR camera sensor (ACPI CAMI / SMO55F1 = ST VD55G1) by hand and read its model ID.
# Tries the reset line both ways and a 0->1 reset pulse. GPIOs are released at the end.
# Expected: model 0x31 0x47 0x35 0x53 (0x53354731 = VD55G1) with reset released (high).
#   sudo ./tools/ir-probe.sh
BUS=${BUS:-$(basename "$(readlink -f /sys/bus/i2c/devices/i2c-SMO55F1:00/..)" | sed 's/^i2c-//')}
ADDR=0x10 CHIP=gpiochip0
CAMPOWER=35 AVDD=104 RESET=101	# ACPI CAMI _CRS order: campower, avdd, reset (active-low), pled

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -n "$BUS" ] || { echo "SMO55F1 i2c device not found"; exit 1; }
echo "== i2c-$BUS, line owners"
gpioinfo -c $CHIP $CAMPOWER $AVDD $RESET

off() { pkill -f 'gpioset -C irprobe'; sleep 0.2; }
readid() {
	echo "   model: $(i2ctransfer -y -f "$BUS" w2@$ADDR 0x00 0x00 r4 2>&1)" \
	     " rev: $(i2ctransfer -y -f "$BUS" w2@$ADDR 0x00 0x04 r2 2>&1)"
}
on() { off; gpioset -C irprobe -c $CHIP -z "$@"; sleep 0.1; }

echo "-- reset=1 (released)"; on $CAMPOWER=1 $AVDD=1 $RESET=1; readid
echo "-- reset=0 (held)";      on $CAMPOWER=1 $AVDD=1 $RESET=0; readid
on $CAMPOWER=1 $AVDD=1 $RESET=0; sleep 0.05
echo "-- reset pulse 0->1";    on $CAMPOWER=1 $AVDD=1 $RESET=1; readid
off
echo "== done (lines released)"
