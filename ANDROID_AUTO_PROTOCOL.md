# Android Auto: how the phone talks to the car

Reverse-engineering notes from **Android Auto 18.0.664204-release** (`com.google.android.projection.gearhead`, arm64, minSdk 32, APKM dated 2026-10-17), written as the basis for a SailfishOS implementation of the phone side.

Method: `jadx` decompilation of `base.apk` and `apktool` for the manifest and resources. Obfuscated protobuf-lite classes were decoded from their `newMessageInfo` field tables. Class names in `[brackets]` are the obfuscated jadx names in this build; they change with every release.

Terminology: **MD** = mobile device (the phone, which we implement). **HU** = head unit (the car).

---

## 1. Big picture

```
 ┌────────────── phone (MD) ──────────────┐            ┌──── car (HU) ────┐
 │ apps → projected UI → H.264 encoder ───┼─ video ───►│ decoder/display  │
 │ media/TTS audio → PCM/AAC ─────────────┼─ audio ───►│ amplifier        │
 │ input dispatcher ◄─────────────────────┼─ touch ────│ touchscreen/keys │
 │ voice assistant ◄──────────────────────┼─ mic ──────│ microphone       │
 │ driving/night logic ◄──────────────────┼─ sensors ──│ CAN bus          │
 └────────────────────────────────────────┘            └──────────────────┘
          AAP: framed, multiplexed channels, TLS 1.2 encrypted
          carried over USB (AOA bulk) or TCP (wireless)
```

* The **phone does all the rendering**: it encodes its UI as an H.264 or H.265 stream. The HU is a thin client that decodes video, plays PCM, and sends touch, sensor and microphone data back.
* One protocol, **AAP** (Android Auto Protocol, internally "GAL"), runs over either transport. The byte stream is identical on USB and TCP.
* The protocol stack lives **inside this APK**, not only in Play Services:
  * `com.google.android.gms.carsetup.*`: connection setup and the USB helper.
  * `com.google.android.gms.car.senderprotocol.*` and the `s*` classes: the framer, channels and TLS.
  * A newer duplicate "senderlib" stack in the `j*` classes.
  * `com.google.android.apps.auto.wireless.*`: Bluetooth/WiFi bootstrap.

Key classes:

| Area | Classes |
|---|---|
| USB accessory open | `gms/carsetup/setup/UsbConnectionHelper`, `CarSetupServiceImpl.h()` |
| Framer TX / RX / reader thread | `[sis]` / `[sir]` / `[siq]`, settings `[sin]`/`[sio]` |
| Channel object, channel open | `gms/car/senderprotocol/Channel`, `[sia]` |
| Control endpoint (version, TLS, discovery) | `[sih]`; newer `[jnb]` |
| TLS engine wrapper, trust store, embedded cert | `[skg]`, `[ske]`, `[skd]` (newer `[jpa]`) |
| Message id enums | `[tff]` (`ck` control, `cj`/`ci` media), `[sal]` input, `[san]` sensor |
| Video / audio senders | `[jrm]`, `[jqn]`, `[jib]`, `[jjb]`, `[jpj]`, `[jcw]` |
| Wireless setup (WPP) | `[pva]`, `[pum]`, `[pvv]`, `[otv]`, `[pmy]`, `[prq]`, `[sql]` |

---

## 2. Connection methods

### 2.1 Wired: USB Android Open Accessory (AOA)

1. The car is the **USB host**. It sends the AOA vendor control requests to the phone:
   * 51: get protocol.
   * 52: send strings: manufacturer `Android`, model `Android Auto` / `Android Open Automotive Protocol`, plus description, version, URI and serial.
   * 53: start accessory.
2. The phone kernel's `f_accessory` gadget function answers those requests. Android userspace then re-enumerates the phone as Google VID `18d1`, PID `2d00` (or `2d01` with ADB).
3. Android delivers `USB_ACCESSORY_ATTACHED`. `res/xml/car_usb_accessory_filter.xml` matches manufacturer `Android` with model `Android Open Automotive Protocol`, `Android Auto` or `Android`.
4. `UsbConnectionHelper.a()` calls `UsbManager.openAccessory()`, retrying after 500 ms on failure. `CarSetupServiceImpl.h()` wraps the fd in a plain `FileInputStream`/`FileOutputStream`, which is the two bulk endpoints.
5. USB settings: QoS on, "concurrent channel map" on, **unbuffered** writes (one `write()` per frame), and reads of up to 16384 bytes per `read()`.
6. AAP starts immediately: the **HU sends VERSION_REQUEST**.

### 2.2 Wireless: Bluetooth RFCOMM bootstrap, then WiFi, then TCP

Google calls this WPP, the "WiFi Projection Protocol". It has no WiFi Direct, BLE or NFC: the phone joins an access point run by the HU.

**Bluetooth.**
* The phone must be **bonded** with the HU, and **HFP or A2DP must be connected** first. Otherwise you get "WiFi Projection Protocol cannot start as HU is not present"; a server flag `wireless_skip_hfp_v2` can skip this check.
* The phone checks the HU's SDP records for UUID `4de17a00-52cb-11e6-bdf4-0800200c9a66`. It also accepts the byte-reversed `669a0c20-0008-f4bd-e611-cb52007ae14d`, which buggy HUs advertise.
* **The phone is the RFCOMM client.** It calls `createRfcommSocketToServiceRecord(uuid)`, a secure socket, and never listens.
* Connect timeout 25 s, retry after 5 s.

**RFCOMM framing.**
* Each message is `u16 BE payload_length | u16 BE message_id | protobuf payload`.
* The phone drops messages larger than 1024 bytes.
* On Android 11+ there is a 10 ms sleep after each write.

| id | message | direction | fields |
|---|---|---|---|
| 1 | WifiStartRequest | HU→MD (or empty MD→HU) | 1 ip (string), 2 port (int32), 3 reason |
| 2 | WifiInfoRequest | MD→HU | (empty) |
| 3 | WifiInfoResponse | HU→MD | 1 ssid, 2 key, 3 bssid, 4 security_mode, 5 access_point_type (0 static, 1 dynamic) |
| 4 | WifiVersionRequest | HU→MD | 1 major, 2 minor, 3 channel_type, 4 repeated channel MHz, 5 HU info, 6 WPP-TCP endpoint{1 ip, 2 port} |
| 5 | WifiVersionResponse | MD→HU | 1 major, 2 minor (echo), 3 device serial, 4 status, 5 chosen channel_type, 6 device info{uuid, session, bt name, bt mac}, 7 setup token |
| 6 | WifiConnectStatus | MD→HU | 1 status, 2 error message |
| 7 | WifiStartResponse | MD→HU (or HU→MD) | 1 ip, 2 port, 3 status |
| 8 / 9 | WifiPingRequest / Response | both | 1 timestamp ms |
| 10 | WifiConnectionRejection | HU→MD | 1 reason (1 device id unknown, 2 bad setup token) |
| 11 | WifiSetupInfo | HU→MD | credentials + endpoint for "WPP over TCP" reconnects (HU WPP ≥ 4.5) |

**Enum values.**
* `security_mode`: 0 unknown, 1 open, 2/3 WEP, 4 WPA, **8 WPA2_PERSONAL**, 12 WPA/WPA2, 20/24/28 enterprise, 32 WPA3, **40 WPA2/WPA3**. The phone accepts 8 and 40, and 32 only behind a flag.
* `channel_type`: 0 5 GHz only, 1 2.4 GHz only, 2 dual band, 3 none.
* `status`: 0 success, 1 unsolicited, −1 no compatible version, −2 inaccessible channel, −3 wrong credentials, −4 already started, −5 WiFi disabled, −6 not yet started, −7 invalid host, −8 no supported channels, −9 check the phone, −10 phone WiFi off, −11 network unavailable.

**Sequence** (`[pum]`/`[pva]`):

```
MD ── RFCOMM connect ───────────────────────────────► HU   (HU speaks first, 12 s timeout)
MD ◄────────────────────────── WifiVersionRequest ──── HU
MD ── WifiVersionResponse(major,minor echoed,status 0) ► HU   (dual band → answer 5 GHz only)
MD ◄──────────────── WifiStartRequest{ip, port} ────── HU   (path A: HU initiated)
MD ── WifiInfoRequest ───────────────────────────────► HU   (resent every 3 s, 3 tries)
MD ── WifiStartResponse{status 0} ───────────────────► HU
MD ◄──── WifiInfoResponse{ssid,key,bssid,WPA2,...} ─── HU
MD   joins the AP (WifiNetworkSpecifier ssid+bssid+WPA2/3 passphrase, no internet capability)
MD ── WifiConnectStatus{0} ──────────────────────────► HU   (only if HU WPP version < 4.5)
MD ── TCP connect ip:port (3 tries, 1 s apart, 10 s timeout, TCP_NODELAY) ► HU
      ... AAP exactly as over USB: HU sends VERSION_REQUEST, TLS, ...
```

**Notes on the sequence.**
* **Path B:** about 5 s after the version exchange, the phone may itself send an empty WifiStartRequest. The HU answers with WifiStartResponse{ip, port, status}, and −6 means retry after 2 s.
* **Ports:** the TCP port is whatever the HU sends; in practice it is usually 5288. The only fixed port in the APK is **5277**, the developer "head unit server", where the phone listens and a Desktop Head Unit connects through `adb forward`.
* **Keep-alive:** after the version exchange the phone pings every 1 s and restarts after 10 missed pings (flag dependent).

---

## 3. AAP transport framing

All fields are big-endian (`[sis]` TX, `[sir]` RX).

```
+--------+--------+----------------+--------------------------+-----------------+
| chan   | flags  | frame_len u16  | total_len u32 (optional) | payload         |
+--------+--------+----------------+--------------------------+-----------------+
flags: 0x01 FIRST  0x02 LAST  0x04 CONTROL  0x08 ENCRYPTED
```

**Frame fields.**
* `total_len` is present only on a FIRST frame that is not also LAST, i.e. the first fragment of a multi-fragment message. It holds the **plaintext** size of the whole message.
* `frame_len` is the size of this frame's payload as it appears on the wire, so the **ciphertext** size when encrypted.

**Fragmentation.**
* The maximum frame is **16128 bytes including the header** (flag `FrameworkGalFeature__fragment_size`, minimum 263).
* Middle fragments carry flags `0x00` (plus ENCRYPTED/CONTROL as applicable).
* Control-channel messages are never fragmented.

**Encryption is per fragment.**
* Each fragment's plaintext is passed through `SSLEngine.wrap()` on its own, and the resulting TLS record(s) become that frame's payload.
* The receiver decrypts each frame independently, then reassembles.

**CONTROL flag.**
* Set only on CHANNEL_OPEN_REQUEST/RESPONSE and CLOSE, sent **on the service's own channel**.
* Ordinary messages on channel 0 do not set it.

**Payload.** Every payload is `u16 BE message_type` followed by the body. The body is protobuf (proto2), except for the version messages and the media DATA layout.

**Framing errors.** On a framing error the phone sends message type `0xFFFF` on channel 0 and drops the link.

---

## 4. Control channel (channel 0)

| id | name | id | name |
|---|---|---|---|
| 1 | VERSION_REQUEST | 13 | NAV_FOCUS_REQUEST |
| 2 | VERSION_RESPONSE | 14 | NAV_FOCUS_NOTIFICATION |
| 3 | ENCAPSULATED_SSL | 15 | BYEBYE_REQUEST |
| 4 | AUTH_COMPLETE | 16 | BYEBYE_RESPONSE |
| 5 | SERVICE_DISCOVERY_REQUEST | 17 | VOICE_SESSION_NOTIFICATION |
| 6 | SERVICE_DISCOVERY_RESPONSE | 18 | AUDIO_FOCUS_REQUEST |
| 7 | CHANNEL_OPEN_REQUEST | 19 | AUDIO_FOCUS_NOTIFICATION |
| 8 | CHANNEL_OPEN_RESPONSE | 20/21 | CAR_CONNECTED_DEVICES req/resp |
| 9 | CHANNEL_CLOSE_NOTIFICATION | 22/25 | USER_SWITCH req/resp |
| 11 | PING_REQUEST | 23 | BATTERY_STATUS_NOTIFICATION |
| 12 | PING_RESPONSE | 24 | CALL_AVAILABILITY_STATUS |
| 26 | SERVICE_DISCOVERY_UPDATE | 0xFFFF | FRAMING_ERROR |

### 4.1 Handshake sequence

```
HU ── VERSION_REQUEST  [u16 major][u16 minor]                 (plain) ──► MD
MD ── VERSION_RESPONSE [u16 major][u16 minor][u16 status]     (plain) ──► HU
      MD answers 1.7 when HU ≤ 1.7, status 0 = SUCCESS
HU ── ENCAPSULATED_SSL  ClientHello ...                       (plain) ──► MD
MD ── ENCAPSULATED_SSL  ServerHello, Certificate, CertRequest ...      ──► HU
      ... until the TLS 1.2 handshake completes
HU ── AUTH_COMPLETE {1: status=0}                             (plain) ──► MD
      ═══ from here every frame has ENCRYPTED set ═══
MD ── SERVICE_DISCOVERY_REQUEST {4 label, 5 device name, 1-3 icons} ──► HU
HU ── SERVICE_DISCOVERY_RESPONSE {1 services[], car/HU strings}     ──► MD
MD ── CHANNEL_OPEN_REQUEST {1 sint32 priority, 2 service id}  (on channel N, CONTROL) ► HU
HU ── CHANNEL_OPEN_RESPONSE {1 status}                        (on channel N, CONTROL) ► MD
      ... per-service setup (below)
```

**Version.**
* The HU's version arrives as two raw shorts.
* The phone answers **1.7**. If the HU's version is above 1.7, the code answers 6.1, which looks like a constant quirk.
* Newer HUs use protocol majors 5 and 6, which unlock "ackless" audio (≥ 5.0) and video (≥ 6.0).

**PING.** The HU sends `{1 int64 timestamp, 2 bool, 3 bytes}` and the phone echoes the timestamp in PING_RESPONSE.

**BYEBYE reasons.**

| Value | Reason |
|---|---|
| 1 | user selection |
| 2 | device switch |
| 3 | not supported |
| 4 | not currently supported |
| 5 | probe supported |
| 6 | wireless projection disabled |
| 7 | power down |
| 8 | user profile switch |

**Audio focus** (control channel).
* The phone requests with `{1 request}`: 1 GAIN, 2 GAIN_TRANSIENT, 3 MAY_DUCK, 4 RELEASE.
* The HU notifies with `{1 state, 2 unsolicited}`: 1 GAIN, 2 GAIN_TRANSIENT, 3 LOSS, 4 LOSS_TRANSIENT_CAN_DUCK, 5 LOSS_TRANSIENT, 6 GAIN_MEDIA_ONLY, 7 GAIN_TRANSIENT_GUIDANCE_ONLY.
* The HU can push focus changes at any time.

**Navigation focus:** 1 NATIVE, 2 PROJECTED.

### 4.2 Service discovery response

`ServiceDiscoveryResponse [ybm]` contains:
* Field 1: repeated `Service`.
* Fields 2–5: car make, model, year, vehicle id.
* Field 6: driver position.
* Fields 7–10: HU make, model, software build, software version.
* Field 17: HU info.

`Service [ybk]` has `1 id` (required, **equals the channel number**) and exactly one descriptor:

| field | service | key contents |
|---|---|---|
| 2 | SensorSource | `1 repeated {1 sensor type}` |
| 3 | MediaSink (video or audio output) | `1 codec, 2 audio type, 3 AudioConfig[], 4 VideoConfig[], 6 display id, 7 display type (0 main, 1 cluster, 2 aux)` |
| 4 | InputSource | `1 keycodes (packed), 2 touchscreen{1 w, 2 h, 3 type}[], 3 touchpad[], 5 display id` |
| 5 | MediaSource (microphone) | `1 codec, 2 AudioConfig` |
| 6 | Bluetooth | HU BT address and pairing methods |
| 7 | Radio | |
| 8 | NavigationStatus | turn-by-turn for the cluster: min interval, image w/h/depth |
| 9 | MediaPlaybackStatus | |
| 10 | PhoneStatus | |
| 11 | MediaBrowser | |
| 12 | VendorExtension | |
| 13 | Notification | |
| 14 | WifiProjection | |
| 15–18 | newer, unidentified | |

`AudioConfig` is `{1 sample_rate, 2 bits, 3 channels}`, all required.

`VideoConfig [ycn]`:

| Field | Meaning |
|---|---|
| 1 | resolution: 1=800×480, 2=1280×720, 3=1920×1080, 4=2560×1440, 5=3840×2160, 6=720×1280, 7=1080×1920, 8=1440×2560, 9=2160×3840 |
| 2 | frame rate: 1=60, 2=30 |
| 3 | width margin |
| 4 | height margin |
| 5 | density in dpi; **required, > 0** |
| 6 | additional decoder depth |
| 8 | pixel aspect ratio ×1e4 |
| 9 | real density |
| 10 | codec |
| 11 | UiConfig {margins, content insets, stable insets} |

**Margins.** The HU crops the margins from the decoded frame. The visible rectangle is Android `Rect(wm/2, (hm+1)/2, W-(wm+1)/2, H-hm/2)`.

---

## 5. Security and certificates

This is the hardest constraint for a third-party implementation.

**The phone is the TLS server.**
* It uses `SSLContext("TLSv1.2")` with `setUseClientMode(false)` and `setNeedClientAuth(true)`.
* No cipher suites are pinned, so platform defaults apply.

**The HU must present a client certificate** chaining to the embedded **"Google Automotive Link"** root. That root is self-signed RSA (sha1RSA), serial `C14EE7A5A4544D42`, valid 2014-06-06 to 2044-06-05.
* It is the *only* trust anchor (`[ske]`).
* The HU's subject must not contain "CarService" or "Google Automotive Link" (`[skg]`).

**The phone presents** a leaf certificate:
* Subject O=CarService, issued by the same root, serial `13AE5EC90E7476`, RSA-2048, sha256RSA.
* **It expires 2027-02-03.**
* It is sent with the root as the chain.

**The phone's private key is embedded in the APK** in obfuscated, encrypted form. Google can rotate the certificate and key through server flags (`SenderlibCertFeature__*`).

**Consequence:**
* Certified head units validate the phone's certificate against Google's root.
* A non-Google phone stack can only be accepted with a certificate and key that chain to that root.
* **This project does not ship or extract Google's key material.** The draft app loads a PEM chain and key from paths the user configures (§8.4).
* Whether and how a user obtains such credentials is their own legal question, since EU interoperability exceptions are narrow.
* With a self-signed certificate, the app still works against open HUs that don't verify: OpenAuto/aasdk-based units, crankshaft, and most DIY setups.

---

## 6. Media channels

**Message ids** (u16 at the start of every payload on audio/video channels):

| id | name | direction |
|---|---|---|
| 0x0000 | DATA: `[u64 BE timestamp µs][payload]` | MD→HU (sinks), HU→MD (mic) |
| 0x0001 | CODEC_CONFIG: `[payload]` (SPS/PPS, AudioSpecificConfig) | MD→HU |
| 0x8000 | SETUP `{1 codec}` | MD→HU |
| 0x8001 | START `{1 session_id, 2 config index, 3 mode, 4 options}` | MD→HU |
| 0x8002 | STOP `{}` | MD→HU |
| 0x8003 | CONFIG `{1 status, 2 max_unacked, 3 config indices[] (preference order)}` | HU→MD |
| 0x8004 | ACK `{1 session_id, 2 count, 3 receive ts ns[]}` | HU→MD (MD→HU for mic) |
| 0x8005 / 0x8006 | MICROPHONE_REQUEST `{1 open, 2 anc, 3 ec, 4 max_unacked}` / RESPONSE `{1 status, 2 session}` | |
| 0x8007 | VIDEO_FOCUS_REQUEST `{2 mode, 3 reason}` | MD→HU |
| 0x8008 | VIDEO_FOCUS_NOTIFICATION `{1 mode, 2 unsolicited}` | HU→MD |
| 0x8009–0x8015 | UI config updates, underflow, overlays, stats, media options, critical UI | |

**Codec enum:** 1 PCM, 2 AAC_LC, 3 H264_BP, 4 AAC_LC_ADTS, 5 VP9, 6 AV1, 7 H265. The phone only implements H264_BP and H265 for video.

### 6.1 Video

**Sequence.**

```
MD ── CHANNEL_OPEN ─► HU ─► OK
MD ── SETUP{codec=H264_BP} ─────────────────► HU
MD ◄───────── CONFIG{status, max_unacked, [preferred config indices]} ── HU
MD ── VIDEO_FOCUS_REQUEST{mode=PROJECTED} ──► HU
MD ◄───────── VIDEO_FOCUS_NOTIFICATION{mode=PROJECTED} ───────────────── HU
MD ── START{session_id, config_index} ──────► HU
MD ── CODEC_CONFIG [SPS+PPS Annex-B] ───────► HU
MD ── DATA [ts][Annex-B access unit] ... ───► HU
MD ◄───────── ACK{session_id, n} ─────────────────────────────────────── HU
```

**Choosing a configuration.** The phone picks the first index from CONFIG that its encoder supports. H.264 is capped at 1080p, and larger sizes need H.265.

**Encoder** (MediaCodec, surface input from a VirtualDisplay):
* H.264 **Baseline** profile, or Constrained Baseline behind a flag.
* Bitrate `max(visible_px × 2.75 + 1.5 Mbit/s, 2 Mbit/s)`, doubled at 60 fps.
* **I-frame interval 60 s** with ACK flow control, or **2 s** in ackless mode.

**DATA.**
* The timestamp is `elapsedRealtime` in µs, a monotonic clock.
* Trailing zero bytes are trimmed from H.264 access units.

**Flow control**, classic (HU protocol < 6.0):
* A semaphore starts at `max_unacked`; each ACK returns `count` permits.
* The phone waits up to 300 ms per frame, then drops it.
* After 400 consecutive timeouts it disconnects with "Car not sending ACK".

**Ackless mode** (HU ≥ 6.0): ACKs are ignored, START field 3 is set to 2, max_unacked is at least 24, and the I-frame interval is 2 s.

**Video focus modes:** 1 PROJECTED, 2 NATIVE (the car shows its own UI, so stop the encoder), 3 NATIVE_TRANSIENT, 4 PROJECTED_NO_INPUT_FOCUS.

### 6.2 Audio output

**Channels.** Three AudioSink services with `audio_type` 1 GUIDANCE (navigation/TTS), 2 SYSTEM (UI sounds), 3 MEDIA, plus 4 TELEPHONY in the enum.

**Formats.**
* MEDIA is **48 kHz stereo S16LE**.
* GUIDANCE and SYSTEM are 16 kHz mono when the HU offers it, else 48 kHz mono.
* Codecs: PCM, AAC-LC, or AAC-LC with an ADTS header per frame. For AAC, the AudioSpecificConfig goes out as CODEC_CONFIG.

**Framing.**
* PCM frames are 1024 samples at 16 kHz (2048 B, 64 ms) or 2048 samples at 48 kHz (8192 B stereo, 42.7 ms).
* DATA has the same 10-byte header as video: id, then the u64 µs timestamp.
* Each playback session is bracketed by SETUP → CONFIG → START … STOP, and gated by **audio focus** on the control channel.
* ACK windowing applies below HU protocol 5.0; at 5.0 and above audio is ackless.

### 6.3 Microphone (MediaSource)

1. The phone sends MICROPHONE_REQUEST `{open=true, anc=false, ec=false, max_unacked=2}`. No SETUP is sent.
2. The HU answers with MICROPHONE_RESPONSE `{status, session}`.
3. The HU streams DATA `[ts][PCM]`, typically 16 kHz mono.
4. The phone ACKs every frame with `{session, 1}`.

---

## 7. Input, sensors, and other channels

**Input** (`[sal]`):
* Message ids: 0x8001 INPUT_REPORT (HU→MD), 0x8002 KEY_BINDING_REQUEST `{1 keycodes packed}` (MD→HU), 0x8003 KEY_BINDING_RESPONSE `{1 status}`, 0x8004 INPUT_FEEDBACK.
* `InputReport {1 uint64 timestamp, 3 touch, 4 keys, 5 absolute, 6 relative, 7 touchpad}`.
* Touch is `{1 pointers[{1 x, 2 y, 3 id}], 2 action_index, 3 action}`.
  * Actions are Android MotionEvent values: 0 DOWN, 1 UP, 2 MOVE, 5 POINTER_DOWN, 6 POINTER_UP.
  * Coordinates are in the touchscreen size declared by the HU.
* Keys are `{1 keycode (Android KEYCODE_*), 2 down, 3 metastate, 4 longpress}`.
  * Rotary controller: keycode 65536 delivered as relative events.

**Sensors** (`[san]`):
* Message ids: 0x8001 REQUEST `{1 type, 2 int64 min period}`, 0x8002 RESPONSE `{1 status}`, 0x8003 BATCH, 0x8004 ERROR.
* In BATCH, the field number equals the sensor type.
* Types: 1 location (lat/lon ×1e7), 2 compass, 3 speed, 4 rpm, 5 odometer, 6 fuel, 7 parking brake, 8 gear, 10 **night mode `{1 bool}`**, 13 **driving status `{1 int32 bitmask}`**, 19 accel, 20 gyro, 21 GPS satellites.
* The phone always subscribes to DRIVING_STATUS with period 0; others are requested on demand.

**Status channels.**
* Navigation status (turn cards for the cluster).
* Media playback status: 0x8001 state, 0x8003 metadata with album art.
* Phone status.
* Media browser, notification, vendor extension.
* All are optional for a minimal phone.

---

## 8. Implementing the phone side on SailfishOS

### 8.1 Platform mapping

| AA phone component | SailfishOS equivalent |
|---|---|
| `UsbManager.openAccessory` | `f_accessory` gadget function, `/dev/usb_accessory` (Android-based hybris kernels: `CONFIG_USB_CONFIGFS_F_ACC`), switched in by usb-moded / a udev hook on the `ACCESSORY=START` uevent |
| Bluetooth RFCOMM client | BlueZ 5 `org.bluez.ProfileManager1.RegisterProfile(uuid, Role=client)` + `Device1.ConnectProfile`; BlueZ does SDP and hands over the socket fd |
| HFP/A2DP prerequisite | ofono HFP AG + PulseAudio A2DP, which come up when pairing the car normally |
| `WifiNetworkSpecifier` | connman D-Bus: find the service by SSID/BSSID, set `Passphrase`, `Connect()` |
| `SSLEngine` server | OpenSSL 3.5 with memory BIOs |
| protobuf-lite | `protobuf-lite` 25.1 + abseil, in the 5.x repos |
| MediaCodec + VirtualDisplay | `QQuickRenderControl` offscreen scene → GStreamer `appsrc ! videoconvert ! droidvenc/openh264enc/x264enc ! h264parse ! appsink` |
| AudioTrack capture of projected apps | PulseAudio monitor source (`pulsesrc device=<sink>.monitor`), or a dedicated null sink |

### 8.2 What can be projected

**Screen.** Lipstick (the Wayland compositor) does not allow third-party apps to capture the screen. So the projected UI is a **dedicated QML scene** rendered offscreen at the HU's resolution, with the HU's touch events injected back into it. This is the same idea as Android Auto, which renders car-specific UIs into a VirtualDisplay rather than mirroring the phone.

**Mirroring.** Screen mirroring would need a compositor-side hook, e.g. a lipstick plugin or a `wlr-screencopy`-style protocol, which is out of scope.

### 8.3 Draft application (`app/`)

```
app/
├── CMakeLists.txt, rpm/carfish.spec, carfish.desktop
├── proto/aap.proto            AAP messages (field numbers from §4–7)
├── proto/wireless.proto       WPP RFCOMM messages (§2.2)
├── src/aap/framer.*           §3 framing, per-fragment encryption hooks
├── src/aap/cryptor.*          TLS 1.2 server over memory BIOs
├── src/aap/session.*          §4 control state machine, channel setup, video/audio/input/sensors
├── src/transport/tcptransport.*          wireless client (HU ip:port) or server :5277 (testing)
├── src/transport/usbaccessorytransport.* /dev/usb_accessory reader/writer
├── src/wireless/bluezprofile.*   BlueZ Profile1 (RFCOMM client)
├── src/wireless/wifijoiner.*     connman join of the HU access point
├── src/wireless/wirelesssetup.*  WPP state machine
├── src/media/carscreen.*      offscreen QML renderer + touch injection
├── src/media/videosource.*    GStreamer H.264 encoder, SPS/PPS split for CODEC_CONFIG
├── src/media/audiosource.*    PulseAudio monitor capture → PCM frames
├── carqml/CarMain.qml         the UI shown on the car
├── qml/                       phone-side Silica UI (mode, cert paths, status, log)
├── config/                    AOA gadget switch script + udev rule (wired, experimental)
└── tools/fake_hu.py           fake head unit for testing (TCP)
```

**What the draft does.**
* Implements the full handshake: version, TLS server, auth complete, service discovery, channel opens.
* **Video:** SETUP → CONFIG → focus → START → CODEC_CONFIG → DATA with ACK windowing.
* **Audio:** media audio gated on audio focus.
* **Input and sensors:** single-touch input; night-mode and driving-status sensors.
* **Wireless:** the whole WPP bootstrap.

**Not implemented yet.**
* Microphone and voice.
* Guidance and system audio channels.
* Key events and rotary.
* Navigation, media and phone status channels.
* H.265.
* Ackless START mode (field 3 = 2).
* Phone-initiated wireless start.
* WPP-over-TCP reconnect.

### 8.4 Certificates for the draft

**The app's certificate files.** The session reads `phone-cert.pem` (the leaf followed by its chain) and `phone-key.pem` from `~/.local/share/carfish/carfish/`. The paths can be changed in the UI.

**For development against an HU that doesn't verify** (OpenAuto, aasdk, a DIY Raspberry Pi), a self-signed certificate is enough:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -subj "/CN=carfish" -keyout phone-key.pem -out phone-cert.pem
```

### 8.5 Wired mode on Sailfish devices

Wired AA needs the AOA handshake to be answered by the phone's USB gadget:

1. **Kernel.** The kernel needs `CONFIG_USB_CONFIGFS_F_ACC`. Most Android-based hybris kernels have it. Check for `accessory` in `/sys/kernel/config/usb_gadget/*/functions` or `/proc/config.gz`.
2. **Gadget switch.** After the car sends ACCESSORY_START, the kernel emits a `ACCESSORY=START` uevent. Userspace must then rebind the gadget with:
   * only the `accessory.gs2` function,
   * `idVendor=0x18d1`, `idProduct=0x2d00`.

   On Android, `UsbDeviceManager` does this; on Sailfish it needs a udev rule plus a script (`app/config/aoa-gadget.sh`), or a usb-moded dynamic mode.
3. **Open the device.** The app then opens `/dev/usb_accessory`.

Device permissions and usb-moded interplay are not yet verified on hardware.

### 8.6 Testing without a car

* **OpenAuto / aasdk (HU emulator on a Linux PC).**
  * Connect through **USB**, or through **TCP** via the "TCP server :5277" mode: the PC connects to the phone's port 5277.
  * It accepts any phone certificate.
* **Google Desktop Head Unit (DHU):** connects to phone port 5277 through `adb forward`. On Sailfish, use plain TCP from the PC instead. DHU validates the phone certificate.
* **Wireless:** a Raspberry Pi running WirelessAndroidAutoDongle / aa-proxy-rs plays the HU side of WPP, i.e. RFCOMM and the access point.

### 8.7 Verification so far (2026-10-31)

**What was run.** `app/tools/fake_hu.py`, a Python head unit, talked to the app in TCP server mode:
* **Host build** (`-DDESKTOP_BUILD=ON`, x264enc) — full session:
  * Version 1.1→1.7, then TLS 1.2 ECDHE-RSA-AES256-GCM, auth, discovery, and 3 channels opened.
  * Video SETUP/CONFIG/focus/START, then CODEC_CONFIG (Constrained Baseline SPS/PPS), then 121 frames with ACKs, then BYEBYE.
  * The recorded stream decodes with ffmpeg at 800×480. The decoded frames show the QML scene reacting to the injected tap and to night mode.
* **SailfishOS 5.1 emulator** (i486 RPM, real SFOS libraries: OpenSSL 3.5, protobuf 25.1, Qt 5.6):
  * The same sequence runs up to video START.
  * The offscreen renderer comes up on Wayland-EGL.
  * The session then stops with "no H.264 encoder element available". The emulator image has no H.264 encoder; libav is decode-only. Devices provide `droidvenc`.

**Not yet tested:**
* A real head unit.
* Wireless.
* USB AOA.
* Audio.
* `droidvenc` on a phone.

**Build notes.**
* SailfishOS 5.x still ships **Qt 5.6**: no `QThread::create`, and no functor `invokeMethod`.
* Qt's QtNetwork links OpenSSL 1.1 while the app uses OpenSSL 3. Both have versioned symbols and load side by side; the linker only warns.

---

## 9. Open questions

* `ybm` fields 11, 13–15 and `ybk` fields 15–18 are not identified.
* START field 3 semantics beyond "2 = ackless".
* VideoConfig fields 6 and 8 on real head units.
* The touch coordinate space when the HU declares margins. The draft scales the touchscreen size onto the visible rectangle.
* Whether HUs still send ACKs in ackless mode. The draft just never blocks when HU ≥ 6.0.
* The exact default sensor subscription set.
