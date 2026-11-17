# Carfish

**A phone-side [Android Auto](https://www.android.com/auto/) implementation for [Sailfish OS](https://sailfishos.org/).**

Carfish turns a Sailfish OS phone into the *phone* end of an Android Auto
session: it speaks the Android Auto protocol to a car's head unit and projects
its own car-optimised UI (map, now-playing, apps, dialer) onto the car screen,
with touch, audio and navigation flowing both ways. It is an independent,
clean-room-style reimplementation built from protocol analysis — it is **not**
affiliated with, endorsed by, or derived from Google's Android Auto app.

> ⚠️ **Read [Certificates & the law](#certificates--the-law) first.**
> Google-certified head units require a TLS client certificate that Google only
> issues to licensed partners. Carfish never bundles or extracts Google key
> material, and there is **no legal consumer way** to obtain such a certificate.
> Carfish is fully usable against open/developer head units and the included
> simulator with a self-signed certificate.

---

## What it does

Carfish runs as an ordinary app plus a small root helper. It implements the
Android Auto Protocol (AAP) end to end and renders a native QML dashboard that
is streamed to the car as H.264 video.

### Transport & protocol
- **Wireless** — Bluetooth RFCOMM bring-up (Wi‑Fi Projection Protocol), joins the
  car's Wi‑Fi access point, then AAP over TCP.
- **USB** — Android Open Accessory (`/dev/usb_accessory`). *(Transport opens but
  the AAP handshake does not complete yet — see [Gaps](#known-gaps--limitations).)*
- **TCP server** — for testing against a simulator on `:5277`.
- **AAP** — TLS (phone acts as the TLS server with mutual auth), version/auth
  negotiation, service discovery, per-channel open, ping, teardown; framed
  `[channel][flags][len][msgId][protobuf]`.

### Media & UI (projected to the car)
- **Video** — the QML car UI is rendered offscreen at the head unit's resolution
  and H.264-encoded with a **bundled software openh264 encoder** (Constrained
  Baseline), so the RPM is self-contained and needs no device H.264 codec.
- **Audio** — captures the phone's active playback (PulseAudio sink monitor) and
  streams it as 48 kHz stereo media audio, following whichever app is actually
  playing.
- **Microphone** — receives the car's microphone channel (for voice/telephony).
- **Touch & buttons** — car touch events map onto the UI; steering-wheel / media
  keys drive playback and voice.
- **Car dashboard** (`app/carqml/`) — split-screen like Android Auto: a
  navigation rail, a map pane, a now-playing pane, an app grid and a dialer;
  reflows landscape/portrait; day/night themed.

### Navigation (OpenStreetMap, integrated — no external nav app)
- Slippy map from OSM tiles with live GPS (QtPositioning / geoclue).
- **Destination search** via Nominatim and **routing** via OSRM, drawn as a route
  line with a turn card.
- Turn-by-turn pushed to the car's **instrument cluster** over the AAP
  navigation-status service.

### Media control & telephony
- **MPRIS** metadata + transport controls for native and Android-App-Support
  players (e.g. Spotify) — now-playing pane and on-screen / steering-wheel
  buttons.
- **Dialer** — recent calls and a keypad using the Sailfish `voicecall` daemon;
  call audio rides the car's Bluetooth hands-free.

### Networking helper (`carfish-helper`, root)
A tiny D-Bus system service so the unprivileged app works on **stock** Sailfish
OS: it drives connman to join the car's Wi‑Fi and **pins the default route to
cellular** so you keep internet (maps, streaming) while the car's uplink-less AP
carries only the projection traffic.

---

## Status

Verified on a real car (Ford Mustang Mach‑E / SYNC 4.0, wireless) unless noted.

| Area | State |
|---|---|
| Wireless bring-up (RFCOMM + WPP + Wi‑Fi join + TCP) | ✅ working on car |
| AAP (TLS, auth, discovery, channels) | ✅ working on car |
| H.264 video (bundled openh264) | ✅ working on car |
| Touch input mapping | ✅ working on car |
| Media audio to car (follows the playing sink) | ✅ working on car |
| Media buttons + metadata (MPRIS) | ✅ working (verified live) |
| OSM map + GPS on the car screen | ✅ working on car (needs Location enabled) |
| Search (Nominatim) + routing (OSRM) + turn card | ✅ verified via desk preview |
| Instrument-cluster turn-by-turn | ⚙️ implemented, simulator-verified |
| Internet kept on cellular while projecting | ✅ working on car |
| Dialer — recent calls / keypad | ⚙️ recents list works; **names + contacts missing** (see gaps) |
| USB (AOA) transport | ⚠️ node opens, **AAP handshake incomplete** |
| Guidance/system audio, H.265 | ❌ not implemented |

---

## Known gaps & limitations

- **Certified cars need a Google certificate** that cannot be obtained legally by
  individuals — see [below](#certificates--the-law). Open/developer head units
  and the simulator work with a self-signed cert.
- **USB / AOA** — the accessory device node opens, but the head unit sends no AAP
  bytes back; the USB handshake path is unfinished. Wireless is the working path.
- **Contacts & caller names** — recent calls show as raw numbers and the contact
  list is empty. Sailfish keeps the real address book in a *privileged* database
  that an unsandboxed app can't read; a privileged lookup path (via the root
  helper) is designed but not yet implemented.
- **Call placement** from the dialer is not yet confirmed on a device (the dial
  path now logs its outcome to `~/carfish.log` for diagnosis).
- **Routing** uses the public OSRM demo server, which can rate-limit; a dedicated
  routing host can be configured later.
- **Location must be enabled** (Settings → Location) for the map to centre and the
  car-position arrow to appear.
- Large parts work only because of a user-supplied TLS certificate; without one
  Carfish refuses to start (with a clear message, not a raw TLS error).

---

## Certificates & the law

Android Auto uses **mutual TLS**. A Google-**certified** head unit will only
complete the handshake if the phone presents a client certificate that chains to
Google's *"Google Automotive Link"* (GAL) root certificate.

**Google issues GAL-chained certificates only to licensed partners** (phone
OEMs, and Google's own Android Auto app). There is **no public, self-service, or
consumer program** to obtain one for a third-party app. The private key used by
the official app is embedded and obfuscated inside that app.

Carfish's position is deliberate and non-negotiable:

- It **never bundles, embeds, or extracts** Google's key material, and this
  project will not accept contributions that do. Extracting the key from Google's
  app would violate Google's terms of service and may implicate anti-circumvention
  law in some jurisdictions.
- The app loads **only certificate/key files you supply**, by path, from your own
  phone storage (editable on the main screen). It inspects them and tells you
  whether they parse, match, are in date, and whether they chain to Google's root
  — so you know in advance whether a given car will accept them — **without ever
  shipping a key**.

### How to legally get a usable certificate

1. **Open / developer head units** — OpenAuto / aasdk, openDsh, and similar
   open-source head-unit stacks, plus the simulator in this repo, do **not**
   verify the phone against the GAL root. A self-signed certificate is enough:

   ```sh
   openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=carfish" \
       -keyout phone-key.pem -out phone-cert.pem
   ```

2. **Non-certified / aftermarket units, or a unit in a documented
   developer/engineering mode** may accept any certificate. Only do this on
   hardware you own, and follow the unit's own documentation.

3. **Licensed partners** — if you are (or work with) an Android Auto OEM/partner,
   obtain the certificate through your official Google partner channel, under the
   terms of your agreement.

For a stock, Google-certified car there is, honestly, **no legal path** for an
independent app today. Carfish is built so that everything *except* that final
certified-car handshake can be developed, tested and used legitimately.

---

## Build

Carfish is a Sailfish OS RPM. See [`app/README.md`](app/README.md) for full
developer notes; the short version:

```sh
# Device RPM (aarch64) via the Sailfish OS SDK, shadow build
mkdir -p build-aarch64 && cd build-aarch64
~/SailfishOS/bin/sfdk -c target=SailfishOS-5.1.0.11-aarch64 build ../app
# -> build-aarch64/RPMS/carfish-0.1.0-1.aarch64.rpm

# Headless desktop binary for protocol/media testing (no Silica)
cmake -S app -B build-host -DDESKTOP_BUILD=ON && make -C build-host -j
```

**Continuous integration** builds the device RPM on every push — see
[`.github/workflows/build-rpm.yml`](.github/workflows/build-rpm.yml). The RPMs
are uploaded as workflow artifacts (and attached to GitHub Releases on tags).

## Install & run

```sh
# on the phone (as root)
rpm -U --force carfish-0.1.0-1.aarch64.rpm   # enables carfish-helper.service

# then launch "Carfish", point the cert/key fields at your PEM files, press Start
```

Headless / over SSH:

```sh
carfish --headless --mode wireless|usb|tcp-server --cert phone-cert.pem --key phone-key.pem
carfish --headless --preview     # render the car UI to ~/carfish-preview.png (no car)
```

The durable log is at `~/carfish.log`.

## Test without a car

- `app/tools/fake_hu.py` — a scriptable head unit over TCP (records the H.264
  stream, taps the screen, toggles night mode).
- `app/tools/hu_sim.py` / `app/tools/run_sim.sh` — a **windowed** simulator that
  shows the projected UI live, sends touch on click/drag, and plays the media
  audio; presets mimic real cars (resolution, margins, touch space).
- `carfish --headless --preview` — snapshot the UI to a PNG on the desk.

See [`app/README.md`](app/README.md) for details.

## Repository layout

```
app/            the app: C++/Qt sources, QML (phone UI + carqml/ projected UI),
                the root helper (helper/), packaging (rpm/, config/), icons,
                vendored openh264 (thirdparty/), and dev tools (tools/)
ANDROID_AUTO_PROTOCOL.md   protocol notes from analysis
.github/workflows/         CI that builds the RPM
```

## Security & privacy

- No Google (or any third-party) key material is bundled or extracted; the app
  uses only certificate/key files you provide.
- The root helper's scope is limited to connman Wi‑Fi join + routing; it exposes
  no general privilege.
- The dialer only places/tracks calls through the system voicecall daemon; call
  audio is carried by the car's Bluetooth hands-free, not by Carfish.

## License

Carfish is licensed under the **GPLv3**. The vendored encoder under
`app/thirdparty/openh264` is Cisco's OpenH264 (BSD‑2‑Clause); see that directory
for its license and notices.

"Android Auto" is a trademark of Google LLC. This project is independent and not
affiliated with Google.
