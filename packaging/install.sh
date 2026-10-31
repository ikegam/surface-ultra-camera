#!/usr/bin/env bash
# Install the Surface Laptop Ultra camera stack: DKMS kernel modules, module
# configuration and the sucamd service. Safe to re-run (upgrades in place).
#   sudo ./packaging/install.sh
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
TOP=$(cd "$(dirname "$0")/.." && pwd)
PKG=surface-ultra-camera
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$TOP/kernel/dkms.conf")
SRC=/usr/src/$PKG-$VER
FW=/lib/firmware/nvidia/n1x/nvCCU.img
BIN="$TOP/sucamd/target/release/sucamd"

[ -f "$FW" ] || { echo "missing $FW - see firmware/README.md"; exit 1; }
for t in dkms modprobe; do command -v $t >/dev/null || { echo "need $t"; exit 1; }; done
modinfo v4l2loopback >/dev/null 2>&1 || { echo "need the v4l2loopback module (e.g. v4l2loopback-dkms)"; exit 1; }

if [ ! -x "$BIN" ]; then
	echo "== building sucamd"
	if [ -n "${SUDO_USER:-}" ]; then
		sudo -u "$SUDO_USER" bash -lc "cd '$TOP/sucamd' && cargo build --release"
	else
		(cd "$TOP/sucamd" && cargo build --release)
	fi
fi

echo "== DKMS modules ($PKG/$VER)"
for old in $(dkms status "$PKG" 2>/dev/null | sed -n "s#^$PKG/\([^,]*\),.*#\1#p" | sort -u); do
	dkms remove "$PKG/$old" --all || true
done
rm -rf "$SRC"
install -d "$SRC"
install -m644 "$TOP"/kernel/{dkms.conf,Kbuild,*.c,*.h} "$SRC/"
dkms install "$PKG/$VER"

echo "== configuration"
install -m644 "$TOP/packaging/modprobe.conf" /etc/modprobe.d/$PKG.conf
install -m644 "$TOP/packaging/modules-load.conf" /etc/modules-load.d/$PKG.conf

echo "== sucamd"
systemctl stop sucamd 2>/dev/null || true
install -m755 "$BIN" /usr/local/bin/sucamd
install -m644 "$TOP/packaging/sucamd.service" /etc/systemd/system/sucamd.service
systemctl daemon-reload

echo "== loading the installed drivers (no reboot needed)"
pkill -x sucamd 2>/dev/null || true
sleep 1
rmmod surface_ultra_cam 2>/dev/null || true
rmmod surface_ccu_boot 2>/dev/null || true
modprobe surface_ccu_boot
modprobe surface_ultra_cam
lsmod | grep -q '^v4l2loopback' || modprobe v4l2loopback
systemctl enable --now sucamd
sleep 2
systemctl --no-pager --lines=3 status sucamd || true
echo "done: open any camera app and pick \"Surface Camera\""
