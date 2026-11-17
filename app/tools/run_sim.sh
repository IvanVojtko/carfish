#!/bin/sh
# Launch the desktop phone app + the interactive head-unit simulator, so you can
# develop and test projection (UI, touch, audio, layout, resolution) on this PC
# without the real car.
#
#   app/tools/run_sim.sh                     # Ford Mustang Mach-E / SYNC 4.0 preset
#   app/tools/run_sim.sh --preset small      # 800x480, no margins
#   app/tools/run_sim.sh --res 1280x720 --margins 0x0
#
# A window shows the projected car UI; click/drag to send touch, play audio on
# this PC to hear it come back through the simulator. Esc or q quits.
#
# Needs: the desktop build (build-host/carfish), python3-opencv, ffmpeg,
# protoc + python-protobuf, openssl. A throwaway TLS identity is generated once
# (never the real car certificate).
set -e
HERE=$(dirname "$0")
ROOT=$(cd "$HERE/../.." && pwd)
APP="$ROOT/build-host/carfish"

if [ ! -x "$APP" ]; then
    echo "Build the desktop app first:"
    echo "  cmake -S app -B build-host -DDESKTOP_BUILD=ON && make -C build-host -j"
    exit 1
fi

CERTDIR="${TMPDIR:-/tmp}/carfish-sim"
mkdir -p "$CERTDIR"
if [ ! -f "$CERTDIR/cert.pem" ]; then
    echo "Generating a throwaway test certificate in $CERTDIR ..."
    openssl req -x509 -newkey rsa:2048 -keyout "$CERTDIR/key.pem" -out "$CERTDIR/cert.pem" \
        -days 365 -nodes -subj "/CN=Carfish Sim" 2>/dev/null
fi

echo "Starting the phone app (TCP server on :5277) ..."
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" "$APP" --headless --mode tcp-server \
    --cert "$CERTDIR/cert.pem" --key "$CERTDIR/key.pem" \
    --car-qml "$ROOT/app/carqml/CarMain.qml" &
APPPID=$!
trap 'kill $APPPID 2>/dev/null' EXIT INT TERM
sleep 2

echo "Starting the head-unit simulator ..."
exec python3 "$ROOT/app/tools/hu_sim.py" "$@"
