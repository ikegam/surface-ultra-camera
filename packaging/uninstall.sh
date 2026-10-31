#!/usr/bin/env bash
# Remove everything install.sh put in place (the firmware in /lib/firmware is left alone).
#   sudo ./packaging/uninstall.sh
set -uo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
PKG=surface-ultra-camera

systemctl disable --now sucamd 2>/dev/null
rm -f /etc/systemd/system/sucamd.service /usr/local/bin/sucamd
systemctl daemon-reload
rmmod surface_ultra_cam surface_ccu_boot 2>/dev/null
for v in $(dkms status "$PKG" 2>/dev/null | sed -n "s#^$PKG/\([^,]*\),.*#\1#p" | sort -u); do
	dkms remove "$PKG/$v" --all
	rm -rf "/usr/src/$PKG-$v"
done
rm -f /etc/modprobe.d/$PKG.conf /etc/modules-load.d/$PKG.conf
echo "removed (v4l2loopback stays loaded until reboot or 'sudo rmmod v4l2loopback')"
