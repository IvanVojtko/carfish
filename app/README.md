# Carfish (draft)

Android Auto phone-side implementation for SailfishOS 5.x. It projects a QML
car UI to an Android Auto head unit. Protocol notes are in
[`../ANDROID_AUTO_PROTOCOL.md`](../ANDROID_AUTO_PROTOCOL.md).

## Build

```sh
# device (aarch64) / emulator (i486) RPMs, shadow build
mkdir -p ../build-aarch64 && cd ../build-aarch64
~/SailfishOS/bin/sfdk -c target=SailfishOS-5.1.0.11-aarch64 build ../app

# headless desktop binary for protocol/media testing (Qt5, GStreamer, openh264)
cmake -S app -B build-host -DDESKTOP_BUILD=ON && make -C build-host -j
```

### Video encoder

The app bundles the **openh264** encoder (vendored under `thirdparty/openh264`,
built as a static library and linked in), so the RPM is self-contained and needs
no H.264 encoder installed on the device. It always produces H.264 Constrained
Baseline, which is what head units require. On the desktop build it links the
system `openh264` instead. GStreamer encoders (`v4l2h264enc`, `x264enc`, …) are
kept as a fallback if openh264 ever fails to initialise.

The MediaTek hardware encoder is **not** used: on current devices it is a
Codec2-only component (`c2.mtk.avc.encoder`), and gst-droid's `droidvenc` drives
it through a legacy metadata-input path that Codec2 rejects (`configure()` →
`BAD_VALUE`) — a limitation inside the Android-side `droidmedia`/`gst-droid`
system libraries that can't be fixed from the app SDK.

> Building the bundled encoder needs `make` and a C compiler in the target — the
> top-level `project()` enables C so `CMAKE_C_COMPILER` is set (openh264's arm64
> NEON assembly is compiled through it). It builds out-of-tree under the CMake
> build directory, leaving the vendored source clean.

## Run

* UI: start "Carfish" and choose the connection type from the pull-down menu.
  * Wireless: the car must already be paired, with HFP connected.
  * USB: see `config/` and §8.5 of the notes.
  * TCP server: listens on :5277, for testing.
* Headless, over SSH or on a desktop:

  ```sh
  carfish --headless --mode tcp-server|usb|wireless \
      --cert phone-cert.pem --key phone-key.pem [--car-qml CarMain.qml]
  ```

  On Sailfish the logs go to the journal: `journalctl -f | grep carfish`.

### Certificates

TLS needs a certificate chain and key in PEM format. **No key material is
bundled in the app** — you place your own PEM files in phone storage and point
the app at them (the paths are shown, and editable, on the main page; the
defaults are under the app's data directory). The app inspects them and shows a
status line: whether the files are found, parse, match each other, are in date,
and whether the certificate chains to Google's root. It refuses to start with a
clear message rather than a raw TLS error if they're unusable.

For head units that don't verify the phone (OpenAuto/aasdk, the fake HU below),
a self-signed certificate is enough:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=carfish" \
    -keyout phone-key.pem -out phone-cert.pem
```

Certified head units only accept a phone certificate issued under Google's
"Google Automotive Link" root (see §5 of the notes).

## Test without a car

`tools/fake_hu.py` plays the head-unit side over TCP. It:
* runs the version exchange, TLS client, auth and discovery for a video sink, touch input and sensors;
* records the H.264 stream;
* taps the centre of the screen (a ripple shows where) and switches night mode on.

```sh
build-host/carfish --headless --mode tcp-server --cert c.pem --key k.pem \
    --car-qml app/carqml/CarMain.qml &
python3 app/tools/fake_hu.py --port 5277 --out stream.h264
ffmpeg -i stream.h264 frame%03d.png
```

Pass `--portrait` (or `--res 720x1280`, `--res 1920x1080`, …) to the fake HU to
exercise the car UI's landscape/portrait reflow.

To run against the emulator or a phone, forward the port with
`ssh -L 15277:127.0.0.1:5277 defaultuser@<device>` and pass `--port 15277`.

### Interactive head-unit simulator (no car needed)

`tools/hu_sim.py` is a *windowed* head unit: it shows the projected car UI live,
lets you **click/drag to send touch**, and **plays the media audio** the phone
streams. It advertises a video sink (with margins), a media-audio sink, touch
input, sensors and a microphone — so it reproduces real-car behaviour (touch
mapping, audio routing, layout, resolution) on this PC.

```sh
# one command: builds nothing, starts the desktop app + the simulator window
app/tools/run_sim.sh                      # Ford Mustang Mach-E / SYNC 4.0 preset
app/tools/run_sim.sh --preset small       # 800x480, no margins
app/tools/run_sim.sh --res 1280x720 --margins 0x0 --touch 1280x720
```

Presets: `ford` (1920x1080, margins 878x0 — the default), `wide` (1280x720),
`small` (800x480). Esc or `q` closes the window. Play some audio on the PC to
hear it come back through the simulator (the app captures the default sink
monitor). To drive a **real phone** instead of the desktop build, run the app on
the phone in TCP-server mode and point the simulator at it:
`python3 app/tools/hu_sim.py --host <phone-ip>`.

Needs `python3-opencv`, `ffmpeg`/`ffplay`, `protoc` + `python-protobuf`.

## Status

| Part | State |
|---|---|
| AAP framing, TLS server, version/auth/discovery, channel open, ping, byebye | working against fake HU (host + SFOS emulator) |
| Video: offscreen QML → H.264 Constrained Baseline → SETUP/CONFIG/focus/START/CODEC_CONFIG/DATA/ACK | **working on real phone** — bundled openh264 (static, Constrained Baseline L3.1), verified encoding 800x480@30 and decoding clean (day + night) |
| Car UI: split-screen dashboard (nav rail + map + now-playing + status bar), landscape/portrait reflow, night mode | working against fake HU (host, both orientations) |
| Touch → QML, night mode / driving status sensors | working against fake HU |
| Microphone (car mic → phone, opened on the voice button) | working against fake HU; records to `~/carfish-mic.wav`, `pcm()` signal seam for a future voice/telephony consumer |
| Steering-wheel / hardware buttons (media, d-pad, voice, call) | working against fake HU; mapped to voice (mic) / MPRIS media / nav-key signals |
| App picker: choose navigation + media apps (native or Android), remembered, auto-launched on connect | working (host); settings page + auto-launch |
| Media backend: MPRIS (metadata + transport) → now-playing pane + media buttons | working on host vs a mock player; **Android Spotify MPRIS exposure to be confirmed on device** |
| Navigation: native OSM map pane (QtLocation `Map` + GPS via QtPositioning) | working on host (online OSM tiles); routing/turn-by-turn still mock; OSM Scout Server offline = opt-in |
| TLS cert/key loaded from phone storage (never bundled) + credential inspector / status line | working |
| Media audio (PulseAudio monitor → 48 kHz PCM, audio focus) | implemented, untested |
| Wireless (BlueZ RFCOMM profile, WPP, connman join, TCP) | implemented, untested; needs a real HU or an aa-proxy/WirelessAndroidAutoDongle Pi |
| USB AOA (`/dev/usb_accessory`, gadget switch script) | implemented, untested; depends on the device kernel having `f_accessory` |
| Guidance/system audio, nav/media status channels, routing, H.265 | not implemented |
