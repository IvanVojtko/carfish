#!/bin/sh
# Carfish USB / Android Open Accessory (AOA) probe.
#
# Run this on the PHONE as root, THEN plug the phone into the car's USB port.
# It records everything needed to wire wired Android Auto on this device:
#   * the kernel uevents the car triggers (does ACCESSORY=START fire?),
#   * the USB gadget state before/at/after the car's AOA handshake,
#   * whether /dev/usb_accessory becomes an active accessory session,
#   * dmesg lines from f_accessory.
#
# It changes NOTHING on the system -- it only observes -- so it is safe to run
# on a daily-driver phone. Output goes to /tmp/aoa-probe.log and the console.
#
#   devel-su sh usb-aoa-probe.sh         # then plug into the car within ~60 s
#
# Share /tmp/aoa-probe.log back to finish wiring config/aoa-gadget.sh correctly.

LOG=/tmp/aoa-probe.log
: > "$LOG"
say() { echo "$@" | tee -a "$LOG"; }
snap() {
    say "--- gadget snapshot ($1) ---"
    G=$(ls -d /sys/kernel/config/usb_gadget/* 2>/dev/null | head -n1)
    say "gadget dir: $G"
    [ -n "$G" ] && {
        say "idVendor=$(cat "$G/idVendor" 2>/dev/null) idProduct=$(cat "$G/idProduct" 2>/dev/null)"
        say "UDC=$(cat "$G/UDC" 2>/dev/null)"
        say "functions: $(ls "$G/functions" 2>/dev/null | tr '\n' ' ')"
        for c in "$G"/configs/*; do
            [ -d "$c" ] && say "config $(basename "$c") links: $(ls "$c" 2>/dev/null | tr '\n' ' ')"
        done
    }
    say "android0/state=$(cat /sys/class/android_usb/android0/state 2>/dev/null)"
    say "udc state: $(for u in /sys/class/udc/*; do echo "$(basename "$u")=$(cat "$u/state" 2>/dev/null)"; done)"
    say "/dev/usb_accessory: $(ls -l /dev/usb_accessory 2>/dev/null)"
    say ""
}

say "=== Carfish AOA probe $(date) ==="
say "kernel: $(uname -r)"
say "usb-moded mode now: $(cat /run/state/namespaces/init/usb_moded_mode 2>/dev/null || echo '?')"
snap "before plug-in"

say "=== now PLUG THE PHONE INTO THE CAR. Watching uevents + dmesg for 60 s ==="
# Clear dmesg cursor, then watch both uevents and kernel log in the background.
dmesg -c >/dev/null 2>&1 || true
( udevadm monitor --kernel --property --subsystem-match=usb --subsystem-match=misc --subsystem-match=android_usb 2>/dev/null \
    | sed -n '1,400p' | tee -a "$LOG" ) &
MON=$!
i=0
while [ $i -lt 12 ]; do
    sleep 5
    i=$((i + 1))
    NEW=$(dmesg -c 2>/dev/null | grep -iE "accessory|aoa|f_acc|android_work|configfs" )
    [ -n "$NEW" ] && { say "--- dmesg @ ${i}x5s ---"; say "$NEW"; }
    ACC=$(cat /sys/class/android_usb/android0/state 2>/dev/null)
    say "[t=${i}x5s] android0/state=$ACC  accessory_dev=$(ls -l /dev/usb_accessory 2>/dev/null | awk '{print $1}')"
done
kill $MON 2>/dev/null
snap "after 60 s (still plugged in)"
say "=== probe done -> $LOG ==="
echo "Pull it with:  scp defaultuser@<phone-ip>:/tmp/aoa-probe.log ."
