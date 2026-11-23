#!/usr/bin/env python3
"""Interactive Android Auto head-unit SIMULATOR for carfish.

Unlike fake_hu.py (a scripted, headless test), this opens a WINDOW that shows the
projected car UI live, lets you click/drag to send touch events, and plays the
media audio the phone streams -- so you can develop and test projection (UI,
touch mapping, audio, layout, resolution/margins) on this PC without going to the
real car.

It reuses fake_hu.py's AAP protocol (framing, TLS, service discovery) and acts as
the head unit:
  * advertises a video sink, a media-audio sink (48 kHz stereo), touch input,
    sensors and a microphone -- the same services a real car offers;
  * can mimic a specific car's screen via --res / --margins / --touch (the Ford
    Mustang Mach-E / SYNC 4.0 is the built-in default preset);
  * decodes the H.264 stream with ffmpeg and shows it with OpenCV;
  * maps window clicks to the advertised touch resolution and sends them back;
  * pipes the received PCM to ffplay.

The phone app must be in TCP server mode (listening on 5277). Run the desktop
build of the app, press Start (TCP server), then:

    python3 hu_sim.py --host 127.0.0.1 --port 5277            # Ford preset
    python3 hu_sim.py --host 192.168.31.20 --res 800x480 --margins 0x0

Needs: python-protobuf, protoc, ffmpeg/ffplay, python3-opencv (cv2), numpy.
"""
import argparse
import os
import queue
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fake_hu  # noqa: E402  (reuse the protocol implementation)

try:
    import cv2
    import numpy as np
except ImportError as e:
    sys.exit(f"hu_sim needs opencv + numpy: {e}")


class HeadUnitSim(fake_hu.FakeHeadUnit):
    def __init__(self, sock, pb, res, margins, touch, aap_version, scale):
        # Reuse the base protocol machinery; /dev/null stands in for the file
        # recorder and the huge frame count disables the scripted bye-bye.
        super().__init__(sock, pb, os.devnull, frames=10 ** 9, res=res)
        self.margin_w, self.margin_h = margins
        self.touch_w, self.touch_h = touch
        self.aap_major, self.aap_minor = aap_version
        self.scale = scale
        self.audio_session = None
        self.ignore_sdr = int(os.environ.get("SIM_IGNORE_SDR", "0"))
        self.new_tls()
        self.media_audio_channel = None
        self.nav_channel = None          # instrument-cluster nav (service id 6)
        self.np_channel = None           # now playing (media playback status, service id 9)
        self.np_last = None

        # H.264 decoder: Annex-B in, raw BGR frames out (low latency).
        # Note: -fflags nobuffer / -flags low_delay stop the raw-h264 demuxer from
        # ever emitting frames here, so keep the plain pipe decode.
        self.dec = subprocess.Popen(
            ["ffmpeg", "-hide_banner", "-loglevel", "error",
             "-probesize", "32", "-analyzeduration", "0", "-f", "h264", "-i", "pipe:0",
             "-f", "rawvideo", "-pix_fmt", "bgr24", "-flush_packets", "1", "pipe:1"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.audio = None  # ffplay, started when media audio arrives

        self.frame_bytes = self.w * self.h * 3
        self.latest = None               # most recent decoded BGR frame
        self.latest_lock = threading.Lock()
        self.tx = queue.Queue()          # touch events from the GUI thread
        self.running = True
        self.send_lock = threading.Lock()
        self.save_path = None
        self.taps = []                   # headless: scripted (x,y) taps before save

    # --- make send() thread-safe (GUI thread queues; net thread drains) -------
    def send(self, *a, **k):
        with self.send_lock:
            super().send(*a, **k)

    # --- version request: mimic the chosen car (default 1.6, like SYNC 4.0) ---
    def handshake(self):
        self.send(0, 1, struct.pack(">HH", self.aap_major, self.aap_minor), encrypt=False)
        print(f"HU: VERSION_REQUEST {self.aap_major}.{self.aap_minor}")
        while True:
            for ch, flags, mid, body in self.recv_messages():
                if mid == 2:
                    major, minor, status = struct.unpack(">HHH", body[:6])
                    print(f"HU: VERSION_RESPONSE {major}.{minor} status {status}")
                    self.tls_step(b"")
                elif mid == 3:
                    self.tls_step(body)
                    if self.tls_done:
                        self.send(0, 4, self.pb.AuthComplete(status=0).SerializeToString(), encrypt=False)
                        self.encrypted = True
                        print(f"HU: TLS up ({self.tls.version()} {self.tls.cipher()[0]}), AUTH_COMPLETE")
                        return

    def new_tls(self):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        # SIM_CLIENT_CERT=cert.pem:key.pem: present a head-unit certificate
        # when the phone asks for one (it does for protocol < 1.6).
        cc = os.environ.get("SIM_CLIENT_CERT")
        if cc:
            ctx.load_cert_chain(*cc.split(":"))
        self.inc, self.outg = ssl.MemoryBIO(), ssl.MemoryBIO()
        self.tls = ctx.wrap_bio(self.inc, self.outg, server_side=False)

    def restart_handshake(self):
        self.new_tls()
        self.tls_done = False
        self.encrypted = False
        self.rx = b""
        self.partial = {}
        timeout = self.sock.gettimeout()
        self.sock.settimeout(None)
        self.handshake()
        self.sock.settimeout(timeout)

    # --- service discovery: video (with margins) + media audio + input + mic --
    def discovery_response(self):
        pb = self.pb
        r = pb.ServiceDiscoveryResponse(make="Sim", model="HeadUnit", year="2026",
                                        head_unit_make="hu_sim", head_unit_model="carfish")
        video = r.services.add(id=1)
        video.media_sink_service.codec_type = pb.CODEC_H264_BP
        video.media_sink_service.display_type = 0  # main display
        video.media_sink_service.video_configs.add(
            resolution=self.res_enum, frame_rate=pb.FPS_60,
            width_margin=self.margin_w, height_margin=self.margin_h, density=185)

        audio = r.services.add(id=5)
        audio.media_sink_service.codec_type = pb.CODEC_PCM
        audio.media_sink_service.audio_type = pb.AUDIO_MEDIA
        ac = audio.media_sink_service.audio_configs.add()
        ac.sample_rate, ac.bits_per_sample, ac.channels = 48000, 16, 2

        inp = r.services.add(id=2)
        inp.input_source_service.touchscreen.add(width=self.touch_w, height=self.touch_h)

        sens = r.services.add(id=3)
        sens.sensor_source_service.sensors.add(sensor_type=pb.SENSOR_DRIVING_STATUS)
        sens.sensor_source_service.sensors.add(sensor_type=pb.SENSOR_NIGHT_MODE)

        mic = r.services.add(id=4)
        mic.media_source_service.codec_type = pb.CODEC_PCM
        mic.media_source_service.audio_config.sample_rate = self.mic_rate
        mic.media_source_service.audio_config.bits_per_sample = 16
        mic.media_source_service.audio_config.channels = 1

        # Instrument-cluster navigation (turn-by-turn sink). type 2 == ENUM.
        nav = r.services.add(id=6)
        nav.navigation_status_service.minimum_interval_ms = 1000
        nav.navigation_status_service.type = 2

        # Now playing on the car's own screens (media playback status).
        r.services.add(id=9).media_playback_service.SetInParent()

        print(f"HU: advertising video {self.w}x{self.h} margins {self.margin_w}x{self.margin_h}, "
              f"touch {self.touch_w}x{self.touch_h}, media audio 48k stereo, cluster nav, now playing")
        return r.SerializeToString()

    def start_audio(self):
        if self.audio:
            return
        dump = os.environ.get("SIM_AUDIO_DUMP")
        if dump:
            # Write the raw PCM (s16le 48 kHz stereo) to a file instead of the
            # speakers, e.g. to check a test tone for gaps.
            class _Dump:
                def __init__(self, path):
                    self.stdin = open(path, "wb", buffering=0)
            self.audio = _Dump(dump)
            print(f"HU: writing media audio to {dump}")
            return
        self.audio = subprocess.Popen(
            ["ffplay", "-hide_banner", "-loglevel", "error", "-nodisp", "-autoexit",
             "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "pipe:0"],
            stdin=subprocess.PIPE)
        print("HU: playing media audio")

    # --- the network side: handshake, then service the phone -----------------
    def net_loop(self):
        pb = self.pb
        try:
            self.handshake()
            self.sock.settimeout(0.02)
            while self.running:
                try:
                    for ch, flags, mid, body in self.recv_messages():
                        self.on_message(ch, flags, mid, body)
                except socket.timeout:
                    pass
                except (ConnectionError, ssl_eof()) as e:
                    print(f"HU: {e}")
                    break
                # Flush queued touch events from the GUI thread.
                while not self.tx.empty():
                    action, x, y = self.tx.get_nowait()
                    self.touch(action, x, y)
        finally:
            self.running = False

    def on_message(self, ch, flags, mid, body):
        pb = self.pb
        if ch == 0:
            if mid == 5:
                req = pb.ServiceDiscoveryRequest.FromString(body)
                print(f"HU: SERVICE_DISCOVERY_REQUEST from '{req.device_name}' label '{req.label}' "
                      f"icons {len(req.small_icon)}/{len(req.medium_icon)}/{len(req.large_icon)} B")
                if self.ignore_sdr > 0:
                    # SIM_IGNORE_SDR=n: act like a picky head unit that drops the
                    # request, waits 3 s and restarts the handshake in plain text.
                    self.ignore_sdr -= 1
                    print("HU: ignoring it; restarting the handshake in 3 s")
                    time.sleep(3)
                    self.restart_handshake()
                    return
                self.send(0, 6, self.discovery_response())
            elif mid == 16:
                print("HU: BYEBYE")
                self.running = False
            elif mid == 18:  # AUDIO_FOCUS_REQUEST -> grant (GAIN), or LOSS on release
                req = pb.AudioFocusRequest.FromString(body)
                state = 3 if req.request == 4 else 1
                print(f"HU: AUDIO_FOCUS_REQUEST {req.request} -> state {state}")
                self.send(0, 19, pb.AudioFocusNotification(state=state, unsolicited=False).SerializeToString())
            return
        # Guidance the phone pushes to the cluster arrives on the nav channel;
        # dispatch it before the mid-based media handlers (ids overlap).
        if ch == self.nav_channel and mid != 7:
            self._on_nav(mid, body)
            return
        if ch == self.np_channel and mid != 7:
            self._on_now_playing(mid, body)
            return
        if mid == 7 and flags & fake_hu.CONTROL:
            req = pb.ChannelOpenRequest.FromString(body)
            self.send(ch, 8, pb.ChannelOpenResponse(status=0).SerializeToString(), control=True)
            if req.service_id == 5:
                self.media_audio_channel = ch
            elif req.service_id == 9:
                self.np_channel = ch
            elif req.service_id == 6:
                self.nav_channel = ch
                # Tell the phone the cluster wants guidance (NAV_STATUS_START).
                self.send(ch, 0x8001, b"")
                print("HU: requested navigation (NAV_STATUS_START)")
            print(f"HU: channel {ch} open (service {req.service_id})")
        elif mid == 0x8000:  # MEDIA_SETUP (video or audio)
            self.send(ch, 0x8003, pb.MediaConfig(status=0, max_unacked=4,
                                                 configuration_indices=[0]).SerializeToString())
        elif mid == 0x8007:  # VIDEO_FOCUS_REQUEST
            self.send(ch, 0x8008, pb.VideoFocusNotification(mode=1, unsolicited=False).SerializeToString())
        elif mid == 0x8001:  # MEDIA_START
            st = pb.MediaStart.FromString(body)
            if ch == self.media_audio_channel:
                self.audio_session = st.session_id
                self.start_audio()
            else:
                self.session = st.session_id
        elif mid == 0x0001:  # MEDIA_CODEC_CONFIG (SPS/PPS)
            self._feed_video(body)
        elif mid == 0x0000:  # MEDIA_DATA ([u64 ts][payload])
            payload = body[8:]
            if ch == self.media_audio_channel:
                if self.audio and self.audio.stdin:
                    try:
                        self.audio.stdin.write(payload)
                    except BrokenPipeError:
                        pass
                self.send(ch, 0x8004, pb.MediaAck(session_id=self.audio_session or 0, ack=1).SerializeToString())
            else:
                self._feed_video(payload)
                self.video_frames += 1
                self.send(ch, 0x8004, pb.MediaAck(session_id=self.session or 0, ack=1).SerializeToString())
        elif mid == 0x8002 and ch == 2:  # KEY_BINDING_REQUEST
            self.send(2, 0x8003, pb.KeyBindingResponse(status=0).SerializeToString())
        elif mid == 0x8001 and ch == 3:  # SENSOR_REQUEST
            self.send(3, 0x8002, pb.SensorResponse(status=0).SerializeToString())
        elif mid == 0x8005 and ch == 4:  # MICROPHONE_REQUEST
            self.send(4, 0x8006, pb.MicrophoneResponse(status=0, session_id=self.mic_session).SerializeToString())

    # --- now playing: print what the phone shows on the car's own screens ------
    def _on_now_playing(self, mid, body):
        pb = self.pb
        try:
            if mid == 0x8001:
                st = pb.MediaPlaybackStatus.FromString(body)
                name = {1: "STOPPED", 2: "PLAYING", 3: "PAUSED"}.get(st.state, st.state)
                key = (name, st.media_source)
                if key != self.np_last:  # position ticks every second; print changes
                    self.np_last = key
                    print(f"NOW PLAYING: {name} source='{st.media_source}' at {st.playback_seconds}s")
            elif mid == 0x8003:
                md = pb.MediaPlaybackMetadata.FromString(body)
                art = md.album_art
                info = ""
                if art:
                    w, h = int.from_bytes(art[16:20], "big"), int.from_bytes(art[20:24], "big")
                    info = f", cover {len(art)} B {'PNG' if art[:4] == b'\x89PNG' else 'NOT PNG'} {w}x{h}"
                print(f"NOW PLAYING: '{md.song}' by '{md.artist}' on '{md.album}' "
                      f"({md.duration_seconds}s{info})")
            else:
                print(f"NOW PLAYING: unhandled message {hex(mid)}")
        except Exception as e:
            print(f"NOW PLAYING: decode error on {hex(mid)}: {e}")

    # --- instrument cluster: print the turn-by-turn the phone sends -----------
    def _on_nav(self, mid, body):
        pb = self.pb
        try:
            if mid == 0x8003:  # NAV_STATUS
                st = pb.NavigationStatus.FromString(body)
                name = {0: "UNAVAILABLE", 1: "ACTIVE", 2: "INACTIVE", 3: "REROUTING"}.get(st.status, st.status)
                print(f"CLUSTER: status = {name}")
            elif mid == 0x8006:  # NAV_STATE
                state = pb.NavigationState.FromString(body)
                for i, s in enumerate(state.steps):
                    cue = " / ".join(s.cue.alternate_text) if s.cue.alternate_text else ""
                    print(f"CLUSTER: step[{i}] maneuver={s.maneuver.type} "
                          f"road='{s.road.name}' cue='{cue}'")
                for d in state.destinations:
                    print(f"CLUSTER: destination '{d.address}'")
            elif mid == 0x8007:  # NAV_CURRENT_POSITION
                pos = pb.NavigationCurrentPosition.FromString(body)
                sd = pos.step_distance.distance
                print(f"CLUSTER: to next turn {sd.display_value} (units {sd.display_units}, {sd.meters} m)")
                for dd in pos.destination_distances:
                    print(f"CLUSTER: to destination {dd.distance.display_value} "
                          f"eta '{dd.estimated_time_at_arrival}' ({dd.time_to_arrival_seconds}s)")
            elif mid == 0x8004:  # NAV_TURN_EVENT (legacy)
                ev = pb.NavigationNextTurnEvent.FromString(body)
                print(f"CLUSTER: [legacy] turn event={ev.event} side={ev.turn_side} road='{ev.road}'")
            elif mid == 0x8005:  # NAV_DISTANCE_EVENT (legacy)
                ev = pb.NavigationNextTurnDistanceEvent.FromString(body)
                print(f"CLUSTER: [legacy] distance {ev.distance_meters} m "
                      f"(display {ev.display_distance_e3 / 1000.0} unit {ev.display_distance_unit})")
            else:
                print(f"CLUSTER: unhandled nav message {hex(mid)}")
        except Exception as e:
            print(f"CLUSTER: decode error on {hex(mid)}: {e}")

    def _feed_video(self, data):
        try:
            self.dec.stdin.write(data)
            self.dec.stdin.flush()
        except BrokenPipeError:
            pass

    # --- decoded-frame reader thread -----------------------------------------
    def decoder_loop(self):
        while self.running:
            buf = self.dec.stdout.read(self.frame_bytes)
            if not buf or len(buf) < self.frame_bytes:
                break
            frame = np.frombuffer(buf, np.uint8).reshape((self.h, self.w, 3))
            with self.latest_lock:
                self.latest = frame

    # --- GUI loop (MUST run on the main thread) ------------------------------
    def gui_loop(self):
        win = "Head unit (carfish)"
        cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)
        cv2.setMouseCallback(win, self._on_mouse)
        blank = np.zeros((int(self.h * self.scale), int(self.w * self.scale), 3), np.uint8)
        cv2.putText(blank, "waiting for video...", (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (200, 200, 200), 2)
        while self.running:
            with self.latest_lock:
                frame = self.latest
            if frame is None:
                show = blank
            elif self.scale != 1.0:
                show = cv2.resize(frame, None, fx=self.scale, fy=self.scale, interpolation=cv2.INTER_AREA)
            else:
                show = frame
            cv2.imshow(win, show)
            if cv2.waitKey(16) & 0xFF in (27, ord("q")):  # Esc/q to quit
                self.running = False
                break
        cv2.destroyAllWindows()

    def _on_mouse(self, event, x, y, flags, _):
        # Window pixels -> advertised touch resolution.
        tx = int(x / self.scale * self.touch_w / self.w)
        ty = int(y / self.scale * self.touch_h / self.h)
        tx = max(0, min(self.touch_w - 1, tx))
        ty = max(0, min(self.touch_h - 1, ty))
        pb = self.pb
        if event == cv2.EVENT_LBUTTONDOWN:
            self.tx.put((pb.TOUCH_DOWN, tx, ty))
        elif event == cv2.EVENT_LBUTTONUP:
            self.tx.put((pb.TOUCH_UP, tx, ty))
        elif event == cv2.EVENT_MOUSEMOVE and (flags & cv2.EVENT_FLAG_LBUTTON):
            self.tx.put((pb.TOUCH_MOVE, tx, ty))

    def run_gui(self):
        threading.Thread(target=self.net_loop, name="net", daemon=True).start()
        threading.Thread(target=self.decoder_loop, name="decoder", daemon=True).start()
        self.gui_loop()
        self.running = False

    def run_headless(self, frames):
        # No window (for CI / a machine with no display): run the protocol and
        # decode, print progress, stop after `frames` decoded frames.
        threading.Thread(target=self.net_loop, name="net", daemon=True).start()
        threading.Thread(target=self.decoder_loop, name="decoder", daemon=True).start()
        start = time.time()
        shown = 0
        while self.running and time.time() - start < 30:
            with self.latest_lock:
                n = 0 if self.latest is None else 1
            if self.video_frames and self.video_frames != shown:
                shown = self.video_frames
                if shown % 15 == 0 or shown == 1:
                    print(f"HU: decoded {shown} video frames (have_frame={n})")
            if self.video_frames >= frames:
                print(f"HU: reached {frames} frames, stopping")
                break
            time.sleep(0.05)
        # Scripted taps (e.g. open a pane) before the grab, so --save captures
        # the result of an interaction, not just the default screen.
        if self.taps:
            pb = self.pb
            for (tx, ty) in self.taps:
                print(f"HU: tap ({tx},{ty})")
                self.tx.put((pb.TOUCH_DOWN, tx, ty))
                time.sleep(0.08)
                self.tx.put((pb.TOUCH_UP, tx, ty))
                time.sleep(0.5)
            time.sleep(1.0)  # let the UI re-render and stream fresh frames
        if self.save_path:
            with self.latest_lock:
                frame = self.latest
            if frame is not None:
                cv2.imwrite(self.save_path, frame)
                print(f"HU: saved frame to {self.save_path}")
        self.running = False


def ssl_eof():
    import ssl
    return ssl.SSLError


PRESETS = {
    # name: (res, margins, touch)
    "ford": ((1920, 1080), (878, 0), (1920, 1080)),   # Mustang Mach-E / SYNC 4.0
    "wide": ((1280, 720), (0, 0), (1280, 720)),
    "small": ((800, 480), (0, 0), (800, 480)),
}


def parse_pair(s):
    return tuple(int(v) for v in s.lower().split("x"))


def main():
    ap = argparse.ArgumentParser(description="Interactive AA head-unit simulator")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5277)
    ap.add_argument("--preset", choices=PRESETS, default="ford",
                    help="car screen preset (default: ford)")
    ap.add_argument("--res", help="override resolution, e.g. 1920x1080")
    ap.add_argument("--margins", help="override margins WxH, e.g. 878x0")
    ap.add_argument("--touch", help="override touch resolution, e.g. 1920x1080")
    ap.add_argument("--aap-version", default="1.6", help="AAP version to request (default 1.6)")
    ap.add_argument("--scale", type=float, default=0.0,
                    help="display scale (0 = auto-fit to ~1280 wide)")
    ap.add_argument("--headless", action="store_true",
                    help="no window; run the protocol + decode and print progress (for CI)")
    ap.add_argument("--frames", type=int, default=60, help="headless: stop after N frames")
    ap.add_argument("--save", help="headless: write the last decoded frame to this PNG")
    ap.add_argument("--tap", action="append", default=[], metavar="X,Y",
                    help="headless: tap this point (touch-resolution coords) before saving; repeatable")
    args = ap.parse_args()

    res, margins, touch = PRESETS[args.preset]
    if args.res:
        res = parse_pair(args.res)
    if args.margins:
        margins = parse_pair(args.margins)
    if args.touch:
        touch = parse_pair(args.touch)
    if res not in fake_hu.RES_ENUM:
        sys.exit(f"unsupported --res {res[0]}x{res[1]}; choose one of: "
                 + ", ".join(f"{w}x{h}" for w, h in fake_hu.RES_ENUM))
    major, minor = (int(v) for v in args.aap_version.split("."))
    scale = args.scale or min(1.0, 1280.0 / res[0])

    pb = fake_hu.load_proto()
    print(f"HU: connecting to {args.host}:{args.port} ...")
    sock = socket.create_connection((args.host, args.port), timeout=30)
    sim = HeadUnitSim(sock, pb, res, margins, touch, (major, minor), scale)
    sim.save_path = args.save
    sim.taps = [tuple(int(v) for v in t.split(",")) for t in args.tap]
    try:
        if args.headless:
            sim.run_headless(args.frames)
        else:
            sim.run_gui()
    except KeyboardInterrupt:
        pass
    finally:
        sim.running = False
        print(f"HU: done, {sim.video_frames} video frames shown")


if __name__ == "__main__":
    main()
