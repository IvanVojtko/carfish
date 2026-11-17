#!/usr/bin/env python3
"""Minimal fake Android Auto head unit for testing carfish.

Connects to the phone's TCP server mode (port 5277), performs the HU side of
AAP (version request, TLS client, auth complete, service discovery with an
800x480 video sink, input and sensors), records the H.264 stream to a file,
injects a tap and a night-mode change, then says bye.

    python3 fake_hu.py --host 127.0.0.1 --port 5277 --out stream.h264

Needs python-protobuf and protoc (aap.proto is compiled on the fly).
"""
import argparse
import math
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_DIR = os.path.join(HERE, "..", "proto")

FIRST, LAST, CONTROL, ENCRYPTED = 0x01, 0x02, 0x04, 0x08
MAX_FRAME = 16128 - 8

# Resolutions the phone can map (session.cpp resolutionSize), landscape + portrait.
RES_ENUM = {
    (800, 480): "RES_800x480", (1280, 720): "RES_1280x720", (1920, 1080): "RES_1920x1080",
    (2560, 1440): "RES_2560x1440", (3840, 2160): "RES_3840x2160",
    (720, 1280): "RES_720x1280", (1080, 1920): "RES_1080x1920",
    (1440, 2560): "RES_1440x2560", (2160, 3840): "RES_2160x3840",
}


def load_proto():
    gen = tempfile.mkdtemp(prefix="fakehu-")
    subprocess.check_call(["protoc", f"--python_out={gen}", "-I", PROTO_DIR, "aap.proto"])
    sys.path.insert(0, gen)
    import aap_pb2  # noqa: E402
    return aap_pb2


class FakeHeadUnit:
    def __init__(self, sock, pb, out_path, frames, res=(800, 480)):
        self.sock = sock
        self.pb = pb
        self.out = open(out_path, "wb")
        self.max_frames = frames
        self.w, self.h = res
        self.res_enum = getattr(pb, RES_ENUM[res])
        self.rx = b""
        self.partial = {}
        self.encrypted = False
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        self.inc, self.outg = ssl.MemoryBIO(), ssl.MemoryBIO()
        self.tls = ctx.wrap_bio(self.inc, self.outg, server_side=False)
        self.video_frames = 0
        self.session = None
        self.tapped = False
        self.night_sent = False
        # mic + steering-wheel test state
        self.mic_rate = 16000
        self.mic_on = False
        self.mic_session = 7
        self.mic_phase = 0.0
        self.mic_frames = 0
        self.keys_done = set()

    # ------------------------------------------------------------ framing
    def send(self, channel, msg_id, body=b"", control=False, encrypt=None):
        encrypt = self.encrypted if encrypt is None else encrypt
        payload = struct.pack(">H", msg_id) + body
        chunks = [payload[i:i + MAX_FRAME] for i in range(0, len(payload), MAX_FRAME)] or [b""]
        for i, chunk in enumerate(chunks):
            flags = (FIRST if i == 0 else 0) | (LAST if i == len(chunks) - 1 else 0)
            flags |= (CONTROL if control else 0) | (ENCRYPTED if encrypt else 0)
            if encrypt:
                self.tls.write(chunk)
                chunk = self.outg.read()
            hdr = struct.pack(">BBH", channel, flags, len(chunk))
            if i == 0 and len(chunks) > 1:
                hdr += struct.pack(">I", len(payload))
            self.sock.sendall(hdr + chunk)

    def recv_messages(self):
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError("phone closed the connection")
        self.rx += data
        while len(self.rx) >= 4:
            ch, flags, ln = struct.unpack(">BBH", self.rx[:4])
            hl = 8 if (flags & FIRST) and not (flags & LAST) else 4
            if len(self.rx) < hl + ln:
                return
            frame = self.rx[hl:hl + ln]
            self.rx = self.rx[hl + ln:]
            if flags & ENCRYPTED:
                self.inc.write(frame)
                frame = b""
                while True:
                    try:
                        frame += self.tls.read(65536)
                    except ssl.SSLWantReadError:
                        break
            if flags & FIRST:
                self.partial[ch] = frame
            else:
                self.partial[ch] = self.partial.get(ch, b"") + frame
            if flags & LAST:
                msg = self.partial.pop(ch)
                yield ch, flags, struct.unpack(">H", msg[:2])[0], msg[2:]

    # ---------------------------------------------------------- handshake
    def handshake(self):
        self.send(0, 1, struct.pack(">HH", 1, 1), encrypt=False)
        print("HU: VERSION_REQUEST 1.1")
        while True:
            for ch, flags, mid, body in self.recv_messages():
                if mid == 2:
                    major, minor, status = struct.unpack(">HHH", body[:6])
                    print(f"HU: VERSION_RESPONSE {major}.{minor} status {status}")
                    self.tls_step(b"")
                elif mid == 3:
                    self.tls_step(body)
                    if self.tls_done:
                        auth = self.pb.AuthComplete(status=0)
                        self.send(0, 4, auth.SerializeToString(), encrypt=False)
                        self.encrypted = True
                        print(f"HU: TLS done ({self.tls.version()} {self.tls.cipher()[0]}), AUTH_COMPLETE sent")
                        return
                else:
                    print(f"HU: unexpected message {mid} during handshake")

    tls_done = False

    def tls_step(self, data):
        if data:
            self.inc.write(data)
        try:
            self.tls.do_handshake()
            self.tls_done = True
        except ssl.SSLWantReadError:
            pass
        out = self.outg.read()
        if out:
            self.send(0, 3, out, encrypt=False)

    # ---------------------------------------------------------- discovery
    def discovery_response(self):
        pb = self.pb
        r = pb.ServiceDiscoveryResponse(make="Fake", model="Car", year="2026",
                                        head_unit_make="carfish", head_unit_model="fake_hu.py")
        video = r.services.add(id=1)
        video.media_sink_service.codec_type = pb.CODEC_H264_BP
        video.media_sink_service.video_configs.add(resolution=self.res_enum, frame_rate=pb.FPS_30,
                                                   width_margin=0, height_margin=0, density=160)
        inp = r.services.add(id=2)
        inp.input_source_service.touchscreen.add(width=self.w, height=self.h)
        sens = r.services.add(id=3)
        sens.sensor_source_service.sensors.add(sensor_type=pb.SENSOR_DRIVING_STATUS)
        sens.sensor_source_service.sensors.add(sensor_type=pb.SENSOR_NIGHT_MODE)
        mic = r.services.add(id=4)
        mic.media_source_service.codec_type = pb.CODEC_PCM
        mic.media_source_service.audio_config.sample_rate = self.mic_rate
        mic.media_source_service.audio_config.bits_per_sample = 16
        mic.media_source_service.audio_config.channels = 1
        return r.SerializeToString()

    def touch(self, action, x, y):
        pb = self.pb
        rep = pb.InputReport(timestamp=int(time.monotonic() * 1e6))
        rep.touch_event.pointers.add(x=x, y=y, id=0)
        rep.touch_event.action_index = 0
        rep.touch_event.action = action
        self.send(2, 0x8001, rep.SerializeToString())

    def key(self, keycode, name):
        # A steering-wheel / hardware button: down then up, as the HU would send.
        pb = self.pb
        print(f"HU: KEY {name} ({keycode})")
        for down in (True, False):
            rep = pb.InputReport(timestamp=int(time.monotonic() * 1e6))
            rep.key_event.keys.add(keycode=keycode, down=down, metastate=0, longpress=False)
            self.send(2, 0x8001, rep.SerializeToString())

    def mic_chunk(self):
        # 40 ms of a 440 Hz sine, 16 kHz mono S16LE, as DATA [u64 BE ts][pcm].
        n = self.mic_rate * 40 // 1000
        buf = bytearray()
        for _ in range(n):
            self.mic_phase += 2 * math.pi * 440 / self.mic_rate
            buf += struct.pack("<h", int(8000 * math.sin(self.mic_phase)))
        ts = struct.pack(">Q", int(time.monotonic() * 1e6))
        self.send(4, 0x0000, ts + bytes(buf))
        self.mic_frames += 1

    # --------------------------------------------------------------- main
    def run(self):
        pb = self.pb
        self.handshake()
        while True:
            for ch, flags, mid, body in self.recv_messages():
                if ch == 0:
                    if mid == 5:
                        req = pb.ServiceDiscoveryRequest.FromString(body)
                        print(f"HU: SERVICE_DISCOVERY_REQUEST from '{req.device_name}' label '{req.label}'")
                        self.send(0, 6, self.discovery_response())
                    elif mid == 11:
                        pass
                    elif mid == 16:
                        print("HU: BYEBYE_RESPONSE")
                        return
                    else:
                        print(f"HU: control message {mid} ({len(body)} bytes)")
                    continue
                if mid == 7 and flags & CONTROL:
                    req = pb.ChannelOpenRequest.FromString(body)
                    print(f"HU: CHANNEL_OPEN_REQUEST channel {ch} service {req.service_id}")
                    self.send(ch, 8, pb.ChannelOpenResponse(status=0).SerializeToString(), control=True)
                elif ch == 1 and mid == 0x8000:
                    print(f"HU: video SETUP codec {pb.MediaSetup.FromString(body).type}")
                    cfg = pb.MediaConfig(status=0, max_unacked=4, configuration_indices=[0])
                    self.send(1, 0x8003, cfg.SerializeToString())
                elif ch == 1 and mid == 0x8007:
                    print(f"HU: VIDEO_FOCUS_REQUEST mode {pb.VideoFocusRequest.FromString(body).mode}")
                    self.send(1, 0x8008, pb.VideoFocusNotification(mode=1, unsolicited=False).SerializeToString())
                elif ch == 1 and mid == 0x8001:
                    st = pb.MediaStart.FromString(body)
                    self.session = st.session_id
                    print(f"HU: video START session {st.session_id} config {st.configuration_index}")
                elif ch == 1 and mid == 0x0001:
                    print(f"HU: CODEC_CONFIG {len(body)} bytes: {body[:8].hex()}")
                    self.out.write(body)
                elif ch == 1 and mid == 0x0000:
                    self.out.write(body[8:])
                    self.video_frames += 1
                    self.send(1, 0x8004, pb.MediaAck(session_id=self.session, ack=1).SerializeToString())
                    self.script_events()
                elif ch == 1 and mid == 0x8002:
                    print("HU: video STOP")
                elif ch == 2 and mid == 0x8002:
                    print("HU: KEY_BINDING_REQUEST")
                    self.send(2, 0x8003, pb.KeyBindingResponse(status=0).SerializeToString())
                elif ch == 3 and mid == 0x8001:
                    req = pb.SensorRequest.FromString(body)
                    print(f"HU: SENSOR_REQUEST type {req.type}")
                    self.send(3, 0x8002, pb.SensorResponse(status=0).SerializeToString())
                elif ch == 4 and mid == 0x8005:
                    req = pb.MicrophoneRequest.FromString(body)
                    print(f"HU: MICROPHONE_REQUEST open={req.open}")
                    self.mic_on = bool(req.open)
                    self.send(4, 0x8006, pb.MicrophoneResponse(
                        status=0, session_id=self.mic_session).SerializeToString())
                elif ch == 4 and mid == 0x8004:
                    pass  # phone ACK for mic data
                else:
                    print(f"HU: channel {ch} message {mid:#06x} ({len(body)} bytes)")

    def script_events(self):
        pb = self.pb
        n = self.video_frames
        if n % 30 == 0:
            print(f"HU: {n} video frames received")
        if n == 45 and not self.tapped:
            # tap the centre of the screen; CarMain.qml shows a ripple there
            tx, ty = self.w // 2, self.h // 2
            print(f"HU: tap at ({tx}, {ty})")
            self.touch(pb.TOUCH_DOWN, tx, ty)
            self.touch(pb.TOUCH_UP, tx, ty)
            self.tapped = True
        if n == 55 and "play" not in self.keys_done:
            self.key(85, "MEDIA_PLAY_PAUSE"); self.keys_done.add("play")
        if n == 60 and "voiceon" not in self.keys_done:
            self.key(231, "VOICE_ASSIST (open mic)"); self.keys_done.add("voiceon")
        if n == 75 and not self.night_sent:
            print("HU: night mode on")
            batch = pb.SensorBatch()
            batch.night_mode.add(night_mode=True)
            self.send(3, 0x8003, batch.SerializeToString())
            self.night_sent = True
        if n == 90 and "next" not in self.keys_done:
            self.key(87, "MEDIA_NEXT"); self.keys_done.add("next")
        if n == 100 and "voiceoff" not in self.keys_done:
            self.key(231, "VOICE_ASSIST (close mic)"); self.keys_done.add("voiceoff")
        # While the mic is open, stream a little audio each video frame.
        if self.mic_on:
            self.mic_chunk()
        if n == self.max_frames:
            print("HU: BYEBYE_REQUEST")
            self.send(0, 15, struct.pack(">BB", 0x08, 1))
            self.out.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5277)
    ap.add_argument("--out", default="stream.h264")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--res", default="800x480",
                    help="advertised video resolution, e.g. 800x480 or 720x1280")
    ap.add_argument("--portrait", action="store_true", help="shortcut for --res 720x1280")
    args = ap.parse_args()
    res_str = "720x1280" if args.portrait else args.res
    try:
        res = tuple(int(v) for v in res_str.lower().split("x"))
        if res not in RES_ENUM:
            raise ValueError
    except ValueError:
        sys.exit(f"unsupported --res {res_str}; choose one of: "
                 + ", ".join(f"{w}x{h}" for w, h in RES_ENUM))
    pb = load_proto()
    sock = socket.create_connection((args.host, args.port), timeout=30)
    hu = FakeHeadUnit(sock, pb, args.out, args.frames, res)
    try:
        hu.run()
    except (ConnectionError, socket.timeout) as e:
        print(f"HU: {e}")
    finally:
        hu.out.close()
        print(f"HU: done, {hu.video_frames} frames written to {args.out}")


if __name__ == "__main__":
    main()
