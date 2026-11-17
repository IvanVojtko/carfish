#!/bin/sh
# Switch the USB gadget into Android Open Accessory mode after the car sent
# ACCESSORY_START (kernel uevent ACCESSORY=START from f_accessory).
# EXPERIMENTAL, untested on hardware: run as root, usb-moded may fight it.
# Usage: aoa-gadget.sh start|stop
set -e
G=$(ls -d /sys/kernel/config/usb_gadget/* | head -n1)
UDC=$(ls /sys/class/udc | head -n1)
# usb-moded's gadget uses a config named c.1 (older kernels b.1); auto-detect it
# instead of assuming, so this works on configfs devices like this one (g1/c.1).
CFG=$(ls -d "$G"/configs/* 2>/dev/null | head -n1)
[ -n "$CFG" ] || CFG="$G/configs/c.1"
FUNC="$G/functions/accessory.gs2"

case "$1" in
start)
    # Stop usb-moded first so it does not immediately rewrite the gadget back to
    # its own mode while we switch to accessory ("stop" restarts it afterwards).
    systemctl stop usb-moded 2>/dev/null || true
    [ -d "$FUNC" ] || mkdir "$FUNC" || { echo "kernel lacks f_accessory" >&2; exit 1; }
    echo "" > "$G/UDC" 2>/dev/null || true
    # keep only the accessory function in the configuration
    for f in "$CFG"/*; do [ -L "$f" ] && rm "$f"; done
    echo 0x18d1 > "$G/idVendor"
    echo 0x2d00 > "$G/idProduct"
    ln -s "$FUNC" "$CFG/accessory.gs2"
    echo "$UDC" > "$G/UDC"
    # let the app open the endpoint device
    sleep 0.5
    chgrp privileged /dev/usb_accessory 2>/dev/null || true
    chmod 0660 /dev/usb_accessory
    ;;
stop)
    # hand the gadget back to usb-moded
    echo "" > "$G/UDC" 2>/dev/null || true
    rm -f "$CFG/accessory.gs2"
    systemctl restart usb-moded
    ;;
*)
    echo "usage: $0 start|stop" >&2
    exit 2
    ;;
esac
