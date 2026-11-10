# Akam: specification, move to standard protocols (RTSP backchannel, MQTT, ONVIF, read-only 8091)

Status: written 2026-11-10 (SPEC agent, branch `docs/spec-standard-protocols`, from `dev`). English, like the code. Nothing here is validated on hardware: every "to verify" is an unknown that the HW test of its step must settle. Sizes and timings are **estimates** (no measurement was made); each step has a measured budget gate (see 0.4).

Reads: specs 03 (RTSP Digest), 04 (control API), 05 (audio out), 06 (LEDs, branch `feature/led-controls`), `src/ak_rtsp/{rtsp,rtsp_in,rtsp_auth,control,audio_out}.c`.

## 0. Decisions, principles, budgets

### 0.1 User decisions (binding)

- **D1** Move to standard protocols, **step by step**; each step is its own implementation + review + HW test + merge:
  1. ONVIF RTSP audio backchannel on port 554 (replaces the custom talk port 8092 and its HMAC).
  2. MQTT client with Home Assistant discovery.
  3. ONVIF Profile S (and the cheap parts of T).
  4. Port 8091 becomes debug plus state metrics, read-only by default.
- **D2** Software to test against: Home Assistant (MQTT, ONVIF integration, WebRTC/go2rtc talk), Frigate/go2rtc (two-way audio), VLC/ffmpeg, NAS/NVR (Synology Surveillance Station, Blue Iris, ONVIF Device Manager).
- **D3** (coordinator, later) **MQTT is optional and disabled by default.** The camera embeds only a small MQTT **client**, never a broker. Until a broker is configured through the wizard, the MQTT module creates **no thread and no socket** and does nothing. The camera is fully usable without MQTT (RTSP + ONVIF + `./akam` over SSH).

### 0.2 Principles

1. **No regression on the video path.** A client that does not use the new features (VLC, ffmpeg, a plain RTSP player) sees byte-identical behaviour: same SDP, same responses. Golden tests prove it (T2).
2. **Secure by default, opt-in features.** Talk requires the RTSP password. ONVIF and MQTT are off until configured. No new service listens without a stated reason; MQTT is outbound only.
3. **One source of truth for settings**: the `ctl_store` of spec 04 section 5 (file `/etc/config/akam/settings` plus the existing per-feature files until migrated). RTSP (backchannel), MQTT, ONVIF, the SSH helper and the 8091 debug SET all call the same internal setter functions (`ao_ctl_set`, `leds_ctl_set`, ... style), never each other's protocol.
4. **Small, auditable, MIT-compatible.** Own code or MIT/BSD libraries, pinned in `tools/build/deps.lock`. GPL code is not linked in or copied without a user decision (see 3.4).
5. Each step ships its own docs, CHANGELOG entry, `akam_hwcheck.sh` lines and wizard changes.

### 0.3 Platform facts that shape the design

- ARM926EJ-S, single core, no FPU/NEON/crypto engine, about 40 MB usable RAM, static musl binary built with zig. Flash: the APP partition has about **2.7 MB free of 5 MB**; `/etc/config` (CONFIG, jffs2) is only **256 KiB** (secrets and settings only, no certificate bundles).
- **The RTSP server serves one client at a time** (`rtsp.c`: single slot, one connection loop; a second client waits in the `accept` backlog). This is the main constraint of step 1 (see 1.7 and decision U4).
- `ak_rtsp` today serves **one** video stream (1920x1080, `ENC_FPS` 15, H.264) plus an L16 mono 8 kHz audio track (PT 97). The second video device (`g_vid_sub`) is opened but no second encoder or RTSP path exists. No JPEG path exists.
- No RTC and no NTP client in the image (docs: clock meaningless without NTP). This affects X.509 validity checks (step 2 TLS) and WS-Security timestamps (step 3).
- The RTSP password is stored as an htdigest line `user:akam:HA1` (spec 03), never in clear. MD5 exists (`md5.c`); SHA-1/SHA-256/base64 do not.
- No `talk.c` and no port 8092 exist in the code (spec 05 stage B/C were never implemented), so nothing has to be removed, only not built.
- Existing audio-out code (`audio_out.c`, `audio_jitter.c`, `g711.c`) and its keys `audio.out_*` stay; they become the sink of step 1 and of MQTT/ONVIF.

### 0.4 Global budget and gates (estimates; each task measures and reports the real delta)

| Step | Flash (stripped `ak_rtsp` delta) | RAM (RSS delta) | CPU |
|---|---|---|---|
| 1 backchannel | +8 to +15 KB | < 8 KB (jitter ring already exists) | < 0.5 % while talking (8 kB/s G.711), 0 idle |
| 2 MQTT plain | +12 to +20 KB | < 12 KB (one thread stack 32 KB is virtual; RSS about 10 KB) | < 0.2 % |
| 2b MQTT TLS (BearSSL) | +60 to +110 KB | +20 to +30 KB during the session | handshake once per (re)connect, estimated 0.2 to 1 s of CPU on this core, then negligible |
| 3 ONVIF | +60 to +110 KB | 40 to 70 KB (one thread, request/response buffers) | < 1 % under polling, bursts at discovery |
| 4 8091 metrics | +8 to +15 KB | < 8 KB | one collection per second at most (cache, as spec 04 3.3) |
| **Total** | **about 150 to 270 KB (about 5 to 10 % of the 2.7 MB free)** | **about 100 to 150 KB** | below 2 % typical |

Gates: each step's last task (`Tn-measure`) builds the real image, reports `size` of `ak_rtsp`, the free APP space after packing (`akam_hwcheck.sh`), RSS (`/proc/<pid>/status`) idle and under load, and CPU before/after with the same stream. A step that exceeds twice its flash estimate or 200 KB RAM stops for a user decision. Video fps and `video.bitrate` must not change by more than measurement noise with all features idle.

## 1. Step 1: ONVIF RTSP audio backchannel on port 554

### 1.1 Scope

Let ONVIF/RTSP clients send audio to the camera speaker over the **existing RTSP port**, per ONVIF Streaming Specification, "Audio backchannel" (Require tag `www.onvif.org/ver20/backchannel`). The audio feeds `audio_out_push()` (spec 05: jitter buffer, G.711 decode, amplifier policy, volume).

Replaces: the custom talk port 8092, `TALK`/`AUTH` HMAC protocol, `talk.c`, `ctl_key` for audio (spec 05 sections 4, 4.1, 5, 8, tasks T3 and T5, decisions D3, D6, "Auth talk"). Kept: spec 05 hardware sequence (section 2.1, 3), jitter buffer (4.2), keys (6), persistence (7, via spec 04 `ctl_store`).

Out of scope: RTP/UDP backchannel transport in the first cut (see 1.3), AEC, Opus/AAC, more than one talker, ONVIF Profile T audio output *service* (`GetAudioOutputs`, DeviceIO), step 3.

### 1.2 Protocol details

Client side (what go2rtc, Frigate through go2rtc, Blue Iris, Surveillance Station do):

1. `DESCRIBE` with `Require: www.onvif.org/ver20/backchannel`.
2. Server replies 200 with the normal SDP **plus** one extra media section for the backchannel:

```
m=audio 0 RTP/AVP 0 8
c=IN IP4 0.0.0.0
a=control:streamid=2
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=sendonly
```

   `a=sendonly` is from the **client's** side (the client sends), as the ONVIF spec defines. PCMU listed first. The existing tracks (video `streamid=0`, mic L16 `streamid=1`) are unchanged. Without the `Require` header the SDP is **byte-identical to today's** (principle 1).
3. `SETUP` of `streamid=2` with `Require: www.onvif.org/ver20/backchannel` and `Transport: RTP/AVP/TCP;unicast;interleaved=N-M`. Server answers with the same interleaved channels and the Session header (same session as the other tracks, `timeout=60`).
4. `PLAY` (with the Require header). The client then sends RTP in `$` frames on channel N (RTCP on M). A talk-only session (no video or mic SETUP, only the backchannel) must work, for clients that open a separate connection to talk.
5. `TEARDOWN` or connection loss ends the talk.

Server rules:

- **Not offered, refused with 551**: if the request has the Require tag but the backchannel is unavailable (RTSP password OFF or BROKEN, `audio.out_enable=0` or the audio device unavailable, or the talk soft switch `audio.talk_enable=0`), reply `551 Option not supported` with `Unsupported: www.onvif.org/ver20/backchannel`. Clients that always send Require (go2rtc) retry without it and get the normal SDP; this is the behaviour for cameras without a speaker. Alternative (200 without the track) is decision U3. Whether `SmolRTSP` has a 551 status constant is to check (T3), otherwise write the response through the existing `net_tx` writer.
- **Talk requires the RTSP password**: the backchannel is offered only when `rtsp_auth` is `ON`. Authentication is the existing Digest, per connection (spec 03); no new credential. `OPTIONS` stays open (it reveals nothing; it may list no new methods).
- Payload types 0 (PCMU) and 8 (PCMA) only, 8000 Hz mono; other PT: drop and count. RTP header parsed with bounds (version 2, CSRC count, extension, padding); packets over 1400 bytes or truncated: drop and count. Telephone-event/comfort-noise PT: ignored.
- Sequence handling over TCP: in order by construction; duplicate or older sequence ignored; a gap just means missing audio (jitter buffer handles underruns). Timestamps are not used for playout (the jitter buffer is fed at arrival rate; 120 ms prefill, 400 ms cap, spec 05 4.2).
- **One talker**: the existing single RTSP slot makes this natural; additionally `audio_out` refuses a second concurrent feeder.
- Idle policy: a talk-only session with no RTP for **30 s** is closed (it must not hold the single slot like a silent client; consistent with `rtsp_idle.c`). While a video session is playing, no new idle rule.
- `rtsp_in.c` today treats every incoming `$` frame as RTCP and drops it: the callback `in_on_interleaved` routes by channel, the backchannel RTP channel goes to the new RTP unpacker, everything else keeps the current RTCP path.
- Logging: `[ak_rtsp] backchannel: <ip> started/ended pcmu|pcma packets=N dropped=N`. Never audio, never credentials.

### 1.3 Transport scope

TCP interleaved is mandatory in step 1 (go2rtc uses RTSP/TCP; simplest and firewall-friendly). UDP (`client_port`) for the backchannel answers `461 Unsupported transport` in the first cut; if a tested NVR needs it, a follow-up task (T7) adds one UDP receive socket pair. Decision recorded as a test question in 1.8.

### 1.4 Half-duplex (spec 05 4.3, re-evaluated)

Spec 05 muted the mic whenever a talker was **connected**. That is wrong for the standard clients: go2rtc and WebRTC cards may keep the backchannel session open and send RTP **continuously** (silence included) for the whole viewing session, which would mute the microphone permanently and make conversation impossible.

New rule: the mic is replaced by silence only while **speech is actually playing**: the decoded backchannel audio exceeds a threshold (RMS over 20 ms above about -45 dBFS) and for a **hangover of 300 ms** after the last loud frame. Silence frames do not mute the mic. Setting `audio.talk_duplex=half|full` (default `half`; `full` never mutes, for people who prefer to hear the echo or use headphones remotely). Persisted. RTP audio packets to viewers keep flowing with correct timestamps (spec 05 behaviour). VAD is 20 lines of integer code, tested on the host.

### 1.5 `./akam talk` and `./akam say` after the change

ffmpeg cannot send an RTSP backchannel (it has no sendonly/interleaved-out support). Options:

| Option | Pros | Cons |
|---|---|---|
| A. **`akam-talk`, a tiny host client in C** (about 300 lines: RTSP + Digest MD5 + RTP packetizer, G.711 from stdin; reuses `md5.c` and `g711.c`), cross-built by zig for linux x86_64/aarch64, macOS x86_64/arm64, windows x86_64, shipped in the kit with `SHA256SUMS`/signature like the other kit files | No runtime dependency, same toolchain, testable against our own server and a fake one, works on all wizard OSes, ffmpeg only for capture/convert | One more binary per OS in the kit; code to maintain; the wizard must verify and run it |
| B. Python 3 script (stdlib: socket, hashlib, struct) | Tiny, readable, no build | Python not guaranteed on the user's PC (Windows), startup cost, one more interpreter dependency |
| C. go2rtc as the host tool (`exec:` source with `#backchannel=1`, rtsp consumer) | Already what HA/Frigate users have; supports WebRTC | A 10 MB Go binary and a YAML for what should be one command; hard to script PTT; an extra moving part for the wizard |
| D. Bash `/dev/tcp` | No binary | Cannot handle binary RTP framing (NUL bytes) at 50 packets/s: not viable |

**Recommendation: A**, with C documented for Home Assistant/Frigate users (their stack already contains it). `./akam talk` pipes `ffmpeg ... -ar 8000 -ac 1 -f mulaw -` into `akam-talk --url rtsp://user:pass@cam/...`; the password is read by the wizard from a prompt, passed through the environment or stdin, never in `argv` or logs. `./akam say FILE` converts with ffmpeg and sends the same way. `./akam talk` checks `rtsp.auth` first and, if OFF, offers `./akam rtsp-auth set` (talk requires it).

Commands that were 8091/HMAC based move to the SSH path (see 4.6): `--test` (beep, `audio.out_test`), `--volume`, `--gain`, `--enable`, `--disable`, `--status` become `./akam audio ...` calling the camera helper over SSH (loopback SET), not the network port. `--reset-guard` likewise.

### 1.6 Security model

- Digest MD5 over cleartext RTSP, as for video; audio payload is not encrypted (a passive LAN observer hears what is said). Accepted, documented. SSH tunnel option as before.
- Password disabled: no backchannel, ever (no "open talk" mode). Password BROKEN: refused (fail closed, like spec 03).
- Failed Digest attempts use the existing rate limit (spec 03 section 3). A new "request audio resource" is not separately rate-limited.
- Volume default 50 (spec 05): no surprise shout. `audio.talk_enable=0` is a soft switch (distinct from `audio.out_enable`, which controls opening the device at boot).
- Denial of service: a LAN client holding the slot blocks video viewers (existing single-slot limitation).

### 1.7 Risks

- **Single RTSP slot.** If go2rtc opens a **second** connection to the camera for talk while Frigate/another consumer holds the first, the second waits or is refused. Behaviour of go2rtc here is not known (does it add the backchannel to the live producer connection, or reconnect?). The HW test decides (decision U4: a second slot, one video + one talk-only, estimated about 4 KB RAM and a loop refactor in `rtsp.c`).
- Clients differ: some need a single `m=audio` per codec, some choose the last codec, some require RTCP. Matrix in the test plan.
- 551 versus 200-without-track: some NVRs may treat 551 as a hard failure. Fallback prepared (U3).
- `Require` header parsing in `SmolRTSP`: request headers available through `req->header_map` (used already for Transport); unknown status codes may need a raw write.

### 1.8 Test plan

Host (extends `tests/*_test.sh` calling native C):
- SDP golden: no Require = byte-identical to the current SDP; Require + auth ON = current SDP + backchannel section; Require + OFF/BROKEN/`talk_enable=0` = 551 with `Unsupported`.
- RTP unpacker: valid PCMU/PCMA, sequence wrap, duplicates, padding/CSRC/extension, truncated, oversized, wrong PT, random fuzz under `-fsanitize=address,undefined`.
- Interleaved routing in `rtsp_in`: backchannel channel vs RTCP channel vs unknown channel.
- Digest gating through the existing `rtsp_auth` state machine (401 before any media section for a Require request).
- VAD/half-duplex: silence frames never mute; speech mutes with 300 ms hangover; `full` never mutes; mic RTP timestamps continuous.
- Talk-only session idle close at 30 s with an injected clock; single talker lock.
- `akam-talk` against a fake RTSP server script (exact bytes, Digest, `ERR` cases) and against the real `rtsp.c` loop through loopback with a fake audio device.
- Regression: all existing `rtsp_in`, `rtsp_auth`, `audio_out` host tests unchanged.

Hardware (user, with the listed software; each point is an unknown):

| # | Point | Criterion |
|---|---|---|
| H1 | VLC and `ffprobe` (no Require) | SDP and stream unchanged, fps/bitrate unchanged |
| H2 | go2rtc `rtsp://` source with the password, talk from the HA WebRTC card in a browser over HTTPS | voice intelligible, no gaps, `audio.out_underruns` about 0; log shows PCMU or PCMA |
| H3 | Frigate (embedded go2rtc) recording **and** two-way audio at the same time | works, or the failure mode of the single slot is recorded (decision U4) |
| H4 | Same with HA's two-way card on a phone | works over Wi-Fi |
| H5 | `akam-talk` + `say` (10 s, 5 min) | complete, no leak, amplifier off 2 s after |
| H6 | Password OFF | go2rtc falls back to a plain stream (551 path); no talk offered |
| H7 | Blue Iris and Surveillance Station two-way audio, if they support it for generic ONVIF | works or documented as unsupported; UDP need recorded |
| H8 | Mic during talk | half mode: no echo; silence frames do not mute the mic; full mode: echo heard |
| H9 | Abrupt client loss (kill -9, unplug) | slot freed, amplifier off in 2 s |

### 1.9 Tasks

| Task | Content | Files |
|---|---|---|
| **T1** | RTP receive unpacker (pure, testable) and VAD gate | `src/ak_rtsp/rtp_in.[ch]`, `audio_vad.[ch]`, tests |
| **T2** | SDP builder refactor with golden test of the unchanged SDP (do first, no behaviour change) | `rtsp.c`, tests/fixtures |
| **T3** | `Require` handling (DESCRIBE/SETUP/PLAY), 551/461, backchannel session state, routing in `in_on_interleaved`, talk-only session and idle close, wiring to `audio_out_push` and half-duplex mute in `audio.c`, keys `audio.talk_enable`, `audio.talk_duplex` | `rtsp.c`, `rtsp_in.c`, `audio.c`, `audio_out.c`, tests |
| **T4** | `akam-talk` host client, zig cross builds, kit packaging, verification | `tools/akam-talk/**`, `tools/release/**`, tests |
| **T5** | Wizard: `./akam talk`, `say`, `audio` (SSH helper); remove spec 05 `talk`/8092 design from docs; password prompt when OFF | `akam`, `tools/wizard/**`, tests |
| **T6** | HW validation H1 to H9, budget gate, fixes | report |
| **T7** | Optional, only if H7 needs it: UDP backchannel; if U4 decided: second RTSP slot | `rtsp.c` |

Order: T2, T1 (parallel), T3, T4/T5 (parallel), T6.

## 2. Step 2: MQTT client with Home Assistant discovery (optional, off by default)

### 2.1 Scope

A small MQTT 3.1.1 **client** inside `ak_rtsp`, published and controlled through Home Assistant MQTT discovery. **Disabled by default (D3):** with no broker configured there is **no thread, no socket, no timer, no allocation** from this module; the code is present in the binary but dormant. Everything else works without it.

Enabled only when `/etc/config/akam/mqtt` exists with `enabled=1` and a host. Configuration is written by the wizard over SSH (never typed on the camera's flash by hand). Applying a change: `ak_rtsp ctl MQTT reload` (see 4.6, loopback) starts/stops the thread at runtime; no reboot, no video interruption.

### 2.2 Entities (HA discovery, one device `akam-XXXX`, identifiers from the wlan0 MAC)

Names follow spec 04 keys, so the same getters/setters are used.

| HA type | Entity | Key / source | Values |
|---|---|---|---|
| select | Status LED | `led.status` | on, off, auto |
| select | IR LEDs | `night.ir_led` | auto, off |
| select | Day/night mode | `night.mode` | auto, day, night |
| number (slider) | Speaker volume | `audio.out_volume` | 0 to 100 |
| switch | Talk enabled | `audio.talk_enable` | ON/OFF (soft switch of step 1; not the device-open `audio.out_enable`, which needs a restart) |
| button | Beep | `audio.out_test` | PRESS |
| sensor | Day/night state | `night.state` | day, night |
| sensor | Uptime | `sys.uptime` | seconds (diagnostic) |
| sensor | CPU | `sys.cpu` | % |
| sensor | Free memory | `sys.mem_free` | MB |
| sensor | Wi-Fi signal | `net.wifi_rssi` | dBm (device_class signal_strength; `unknown` if the driver gives nothing) |
| sensor | Video bitrate | `video.bitrate` | kbit/s |
| sensor | RTSP state / clients | `rtsp.state`, count | idle/connected/playing |
| sensor | Audio underruns, drops | `audio.out_underruns/drops` | diagnostic |
| sensor | Temperature | if the platform exposes it (to verify on HW; else not published) | |

No reboot button, no password or Wi-Fi setting, no hostname change over MQTT: command allow-list only (decision U9). All `set` payloads go through the same validators as 8091/SSH and are strictly checked; invalid payloads are ignored and counted.

### 2.3 Topics, availability, cadence

- Base topic `akam/<id>` (`<id>` = hostname `akam-XXXX`; configurable base). Discovery prefix `homeassistant` (configurable).
- Discovery: retained messages `homeassistant/<type>/<id>/<entity>/config`, published at each (re)connect; `device` block (name, model "Akam / AK3918AV130", `sw_version` = `fw.version`, `configuration_url` = ONVIF/none).
- Availability: `akam/<id>/status` = `online` retained at connect, **LWT** `offline` retained; every entity has `availability_topic`. Clean disable publishes `offline`.
- State: `akam/<id>/<entity>/state`; controls published on change (retained so HA restores), sensors every **30 s** (configurable 10 to 300 s) and also on change for `night.state`, `rtsp.state`. QoS 0 for state and discovery publishes (retained). Command topics `akam/<id>/<entity>/set`, subscribed QoS 0/1.
- Reconnect: exponential backoff 5 s to 5 min with jitter; a failing broker never blocks anything else (the thread is independent of video).
- `./akam mqtt remove` publishes empty retained discovery messages so HA drops the entities.

### 2.4 Configuration, wizard, persistence

`/etc/config/akam/mqtt` (0600, dir 0700; key=value): `enabled`, `host`, `port` (1883 or 8883), `user`, `pass`, `tls` (`off|on`), `tls_verify` (`ca|fingerprint|none`), `base_topic`, `discovery_prefix`, `interval`. The broker password must be stored in clear (the broker needs it); it never appears in logs, `LIST`, `STATUS`, or `ps`.

Wizard: `./akam mqtt [set|disable|status|test|remove]` over SSH. `set` prompts (host, port, user, password without echo, TLS choice, optional CA file), sends the password on **stdin**, never in `argv`. `test` triggers a one-shot connect on the camera and prints the CONNACK code or the TLS error. The `verify` step asks "Connect to Home Assistant through MQTT? [y/N]" (default no; no terminal: no; `--dry-run`: shown only). Camera helper `/usr/sbin/akam_mqtt.sh` (POSIX sh, BusyBox 1.30.1, no `tr`/`mktemp`, atomic write), installed through `image_files.txt`, like `akam_rtsp_auth.sh`.

### 2.5 Library choice

| Option | License | Verdict |
|---|---|---|
| **Own minimal MQTT 3.1.1 client** (CONNECT with LWT, PUBLISH QoS 0/1 out and in, SUBSCRIBE, PINGREQ, retain, clean session, keepalive 60 s; about 500 to 700 lines of C) | MIT (ours) | **Recommended**. We need about 10 packet types; non-blocking in one `poll()` loop; no allocator, fixed buffers (2 KB rx, 2 KB tx); no dependency to audit; TLS I/O goes through a two-function transport abstraction. Tested against a fake broker (own, in C tests) and a real Mosquitto when available on the dev PC. |
| MQTT-C (LiamBindle) | MIT, MQTT 3.1.1, 2 source files, thread-safe, BearSSL/OpenSSL PAL reported | Fallback if our client slips: license-compatible, small. Drawbacks: fixed message queue and buffer model to adapt, upstream is slowly maintained, its sync helper uses blocking patterns we would avoid. |
| Eclipse Paho embedded C | EPL/EDL (BSD-3 style) | Compatible but larger and heavier model; no gain. |
| libmosquitto | EPL/EDL | Needs its own threads and, for TLS, OpenSSL; too heavy. |
| Mongoose, wolfMQTT | GPLv2 / commercial | Excluded (license). |

### 2.6 TLS (optional, step 2b)

BearSSL (MIT, no dynamic allocation, TLS 1.2, ECDHE with P-256 and Curve25519, AES-GCM, ChaCha20-Poly1305) is the proposed library (the alternative, mbedTLS, is Apache-2.0/GPL dual and larger). Pinned in `deps.lock` like Dropbear.

Cost estimate on this core (no crypto acceleration): +60 to +110 KB flash, 20 to 30 KB RAM for the session buffers, a handshake of well under a second once per (re)connect; steady state is a few KB/s of AES-GCM, negligible. **Estimates, to be measured by T14.** Mosquitto and the Home Assistant add-on speak TLS 1.2, which BearSSL supports (TLS 1.3 only brokers are not supported).

Trust model, because the camera has no RTC and no room for a CA bundle:
- `tls_verify=ca`: verify against a CA or self-signed certificate **provided by the user** through the wizard (`/etc/config/akam/mqtt_ca.pem`, one or two certificates, under 4 KB). Date check only if the clock looks set (year at least 2024); otherwise skipped with a log line.
- `tls_verify=fingerprint`: pin the broker certificate SHA-256 (no CA, no dates).
- `none`: encrypted but unauthenticated; refused unless the user explicitly passes the wizard flag; flagged by `status`.
Plain `tls=off` stays supported for the typical home broker on port 1883; the wizard says in one sentence that the password then travels in clear on the LAN.

### 2.7 Security model

- Outbound connection only; **no new listening port**.
- Command authority is whoever can publish to `akam/<id>/+/set`: protect with broker users/ACL (documented). The allow-list is limited to LED, IR, night mode, volume, talk enable, beep. Nothing that can brick, expose a secret, or change the network.
- The broker password in `/etc/config/akam/mqtt` is readable by root only; never logged.
- Retained discovery payloads contain no secrets, no SSID, no IP.
- Beep and volume can be abused by anyone with publish rights (annoyance, not safety); volume cap respects the user maximum (`audio.out_volume` 0..100).

### 2.8 Test plan

Host: packet encoder/decoder (golden bytes for CONNECT with LWT, PUBLISH, SUBSCRIBE, PINGREQ, DISCONNECT, variable-length remaining length edge cases 127/128/16383/16384); fake broker in C (CONNACK codes, drops, partial reads, slow writes, QoS 1 acks, retained); state machine (reconnect backoff with injected clock, keepalive, LWT set); discovery JSON golden files per entity (schema keys, unique ids, availability); command payload fuzz (invalid, oversized, non-UTF-8); **dormant check**: with no config file the module creates no thread (`/proc/self/task` count) and no socket (fd count) and costs no allocation; config reload on/off; no secret in logs (sentinel password, as in spec 03 tests); shell helper under BusyBox; wizard dry-run and fake camera. TLS (2b): BearSSL against a local `openssl s_server`/Mosquitto with CA, fingerprint, wrong CA, expired certificate with a plausible clock.

Hardware (user, Home Assistant + Mosquitto): H1 entities appear under one device; H2 each select/number/switch/button works and the real LED/IR/volume/beep follows, and a change made through `./akam led` shows up in HA; H3 sensors plausible (compare CPU and RSSI with `top` and `iwconfig`), temperature present or absent; H4 availability: kill `ak_rtsp` or unplug: HA shows unavailable within the keepalive time via LWT; H5 broker restart: reconnect and discovery republished; H6 **without a broker configured**: process list, thread count and open sockets show no MQTT activity; H7 video fps and bitrate unchanged with MQTT on; H8 TLS: connect with CA, with fingerprint, wrong fingerprint refused; handshake time and RAM measured.

### 2.9 Tasks

| Task | Content | Files |
|---|---|---|
| **T8** | MQTT 3.1.1 packet codec + client state machine (pure, non-blocking), transport abstraction, tests with fake broker | `src/ak_rtsp/mqtt.[ch]`, tests |
| **T9** | Settings registry: one table of keys (type, bounds, getter, setter, persistent) used by 8091, SSH loopback, MQTT, ONVIF (reuses `ctl_store` of spec 04, T3/T4 there) | `src/ak_rtsp/ctl_*.[ch]` |
| **T10** | HA discovery builder, topic map, publisher/command handler, optional thread lifecycle and reload, dormant guarantee | `src/ak_rtsp/mqtt_ha.[ch]`, `main.c`, tests |
| **T11** | Camera helper `akam_mqtt.sh`, image files, `hwcheck` line | `tools/firmware_patch_templates/akam_mqtt.sh`, tests |
| **T12** | Wizard `./akam mqtt`, `verify` question, fake broker for wizard tests | `akam`, `tools/wizard/**`, tests |
| **T13** | HW validation H1 to H7, budget gate, docs (`docs/mqtt.md`, README, CHANGELOG) | report, docs |
| **T14** | Step 2b TLS: BearSSL integration, trust modes, wizard CA/fingerprint, measurements, H8 | `tools/build/**`, `mqtt_tls.[ch]`, tests |

## 3. Step 3: ONVIF Profile S (and the cheap parts of T)

### 3.1 Scope (a compatible subset, no claim of formal conformance)

| Service | Operations | Notes |
|---|---|---|
| WS-Discovery (UDP 239.255.255.250:3702) | Probe/ProbeMatches, Hello at start, Bye at stop | Types `dn:NetworkVideoTransmitter`, scopes `onvif://www.onvif.org/Profile/S`, `.../Profile/T` only if implemented, `name/<hostname>`, `hardware/AK3918AV130`, `location/...` |
| Device | GetDeviceInformation, GetCapabilities, GetServices, GetServiceCapabilities, GetHostname (and SetHostname through spec 04 6), GetSystemDateAndTime, SetSystemDateAndTime, GetNetworkInterfaces, GetScopes, GetNTP/SetNTP (see U13), GetDNS (read), SystemReboot, GetUsers (read only, returns the one user) | Pre-auth allowed: GetSystemDateAndTime, GetCapabilities, GetServices, GetServiceCapabilities, GetScopes, GetHostname per the ONVIF access classes |
| Media (v1) | GetProfiles, GetProfile, GetVideoSources, GetVideoSourceConfigurations, GetVideoEncoderConfigurations, GetStreamUri (RTSP over TCP/UDP unicast, `rtsp://<ip>:554/`), GetServiceCapabilities, audio source/encoder and **audio output** configurations that describe the speaker and the backchannel from step 1 | One profile `main` (H.264 1920x1080, 15 fps) with the mic. A `sub` profile needs a second encoder and an RTSP path that do not exist (U13: defer) |
| Imaging | GetImagingSettings, SetImagingSettings, GetOptions, GetServiceCapabilities | `IrCutFilter` ON/OFF/AUTO mapped to `night.mode` day/night/auto (ON = filter inserted = day). Brightness, Contrast, ColorSaturation, Sharpness mapped to `isp.*` (-50..50) only where `GetOptions` states that range; others not advertised |
| Events | GetServiceCapabilities (no support), later pull-point for motion | Optional step; HA works without events |
| Media2 (Profile T) | GetProfiles, GetStreamUri, GetVideoEncoderConfigurations | Optional task T22: HA falls back to Media 1 |
| Snapshot | GetSnapshotUri | **Deferred**: no JPEG path today; would need a hardware JPEG encoder channel and an authenticated HTTP image endpoint. Study task only (U14) |

Not in scope: PTZ, relay outputs, analytics, recording, replay, user management writes, TLS (HTTPS), multicast streaming, WS-Eventing base subscription.

### 3.2 Protocol details

- SOAP 1.2 over HTTP/1.1 POST on one URL per service: `/onvif/device_service`, `/onvif/media_service`, `/onvif/imaging_service`, `/onvif/events_service`. Response bodies from **static templates with substitution** (no XML library, no DOM): request parsing finds the first element of the SOAP Body by local name, then extracts only the few arguments needed with a bounded tag scanner (namespaces ignored except for the operation name). Unknown operation: SOAP fault `ter:ActionNotSupported`, HTTP 400 or 500 as the spec says.
- Request size capped (16 KB), response written in chunks (no 16 KB response buffer), keep-alive supported (NVRs reuse connections), `Content-Length` always set.
- XAddr in discovery and GetCapabilities built from the interface address actually used by the request (`getsockname`), so it is correct with Wi-Fi or Ethernet and DHCP changes.
- Device UUID: stable, derived from the wlan0 MAC (UUID version 5-style hash, no randomness at boot).
- Clock: `GetSystemDateAndTime` always works (clients compute their offset from it). `SetSystemDateAndTime` calls `settimeofday` (needs auth). No NTP client in the image: see U13.

### 3.3 Security model

- **Credentials: the same user and password as RTSP.** Two schemes, as ONVIF requires: **WS-Security UsernameToken with PasswordDigest** (`Base64(SHA1(nonce + created + password))`) and **HTTP Digest** (MD5, reusing `rtsp_auth.c` machinery, realm `akam`).
- **Consequence to decide (U2):** PasswordDigest needs the **plaintext password** on the camera; the stored htdigest HA1 (spec 03) cannot verify it. Recommendation: store the password in `/etc/config/akam/credentials` (0600) when ONVIF or MQTT is enabled, keep writing the htdigest file for RTSP; the wizard's `rtsp-auth set` writes both. HTTP Digest alone would need only HA1 but several clients (Surveillance Station, parts of the HA stack) rely on WS-Security. Plaintext on the camera is no worse than the HA1 for the attacker (both grant stream access) except for password reuse across services; the wizard says so.
- Access classes: pre-auth operations listed in 3.1; everything else requires a valid token **when the password is enabled**. **When the RTSP password is OFF** ONVIF read operations are open (same as the video) but **state-changing operations are refused** (`SystemReboot`, `SetSystemDateAndTime`, `SetHostname`, `SetImagingSettings`, `SetNTP`): consistent with "talk requires the password" (U11).
- WS-Security replay and time: keep the last 32 nonces (reject reuse); check `Created` within +/- 5 min of the camera clock **only if the camera clock looks set** (year at least 2024); with a wrong clock the window check is skipped (the nonce cache stays). Constant-time comparison; failures share the spec 03 rate limiter (delay and counter), generic `ter:NotAuthorized`.
- HTTP server: listens on `INADDR_ANY` port 80 (or the chosen port, U10) **only when ONVIF is enabled**; request line/header limits, 5 s read timeout, 4 concurrent connections, closes idle keep-alives after 30 s; no static files, no directory listing, no other URL served.
- ONVIF is **off by default** (U11). `./akam onvif [enable|disable|status]` over SSH (helper `akam_onvif.sh`, flag in `/etc/config/akam/onvif`); the wizard `verify` step proposes it (default no) and, if the user says yes, checks that a password is set. Runtime reload without restart (`ak_rtsp ctl ONVIF reload`).
- Multicast WS-Discovery is on the LAN only; the Hello/Bye reveal name, model, address (same as ONVIF everywhere). Replies unicast only to the prober.

### 3.4 Existing small ONVIF servers: license and fit (user decision U1)

| Candidate | License | Footprint / model | Fit |
|---|---|---|---|
| **onvif_simple_server** (roleoroleo; used by thingino/OpenIPC-like firmwares; profiles S/T, Device, Media (+Media2), Imaging, Events, PTZ, DeviceIO; WS-Security via libtomcrypt/mbedtls/wolfssl; ezxml-based templates) | **GPL-3.0** (stated on its repository; no linking exception; bundled `log.c` and `ezxml` are permissive, but the project as a whole is GPL-3.0) | Runs as **CGI**: needs an external HTTP server with CGI (lighttpd/busybox httpd), one process per request (fork+exec on this core under video load), a companion `wsd_simple_server` for discovery, configured by JSON files and shell scripts that it executes for each action. Binary size not documented; small, template-driven | Strong feature match, but **GPL-3.0** |
| gSOAP-based stacks | GPL or commercial | Heavy generated code (hundreds of KB) | Rejected (license and size) |
| Own implementation (templates + bounded scanner, built-in HTTP) | MIT (ours) | One thread, no fork, integrated getters/setters, about 2500 to 3500 lines of C, estimated +60 to +110 KB | **Recommended** |

**License analysis (flag, not a decision):** `ak_rtsp` is statically linked into one MIT binary. Linking or copying GPL-3.0 code (even a few files or its templates) into it would require **relicensing the whole `ak_rtsp` to GPL-3.0** (and the kit that ships it). Running onvif_simple_server as a **separate GPL-3.0 process** (CGI) in the same image would not change the license of `ak_rtsp` (aggregation), but the image would then carry a GPL-3.0 binary: we would have to ship its license text and offer its corresponding source (and any patches), keep it in `THIRD_PARTY_NOTICES.md`, and could not mix its code into ours. It would also need a CGI HTTP server on the camera and a one-process-per-request cost, and a script bridge for each action. **Recommendation: own clean-room implementation from the ONVIF specifications and public WSDL/XSD (not from GPL source), MIT.** Whether to accept GPL in the image or to relicense is **the user's call** (U1); the spec does not decide it.

Built-in HTTP versus CGI: a built-in `poll()` loop in one thread avoids a web server dependency (BusyBox `httpd` availability is not verified: `busybox --list` on the image decides), avoids fork/exec per request, and lets the handlers call the setters in-process. Recommendation: **built-in**, sharing the thread with WS-Discovery (UDP 3702) and the loopback control socket housekeeping where practical.

### 3.5 Test plan

Host: SOAP parser/scanner tests (namespaces, prefixes, whitespace, CDATA, large or malformed bodies, deep nesting, fuzz under sanitizers); golden responses for every operation (reviewed against the ONVIF WSDL, schema validated with `xmllint` when available); WS-Security vectors (digest of a known nonce/created/password with an independent implementation such as `openssl`), nonce replay, bad digest, wrong user, clock-window logic with an injected clock; HTTP Digest reuse tests; state-change refusal when auth is OFF; access classes; discovery Probe/Match formatting (types, scopes, XAddr from `getsockname`); HTTP limits (slow headers, oversized, many connections); imaging mapping tables; dormant check (off by default: no socket on port 80/3702, no thread).

Hardware (user): H1 ONVIF Device Manager finds the camera, shows device info, profiles, live view, imaging IR cut changes the real filter and `night.mode`; H2 Home Assistant ONVIF integration (auto-discovery by WS-Discovery or manual port) adds the camera, shows the stream, no event entities is acceptable; H3 Frigate/go2rtc using the discovered RTSP URI; H4 Synology Surveillance Station adds the camera as ONVIF, live view and, if supported, two-way audio through step 1; H5 Blue Iris ONVIF discovery and ptz-less setup, stream; H6 `SystemReboot` with auth; with password OFF it is refused; H7 wrong password rate limiting; H8 clock set through ONVIF and client time handling; H9 video fps/bitrate unchanged and CPU under a 1 Hz polling NVR; H10 port scan shows only the documented ports.

### 3.6 Tasks

| Task | Content | Files |
|---|---|---|
| **T15** | Crypto primitives: SHA-1, base64 (MIT, own, test vectors); HTTP Digest reuse and credential store (`credentials`, migration from `rtsp_auth` flow) | `src/ak_rtsp/sha1.[ch]`, `b64.[ch]`, `cred.[ch]`, tests |
| **T16** | HTTP/1.1 minimal server (poll loop, limits, keep-alive) + SOAP scanner + template engine | `src/ak_rtsp/http_srv.[ch]`, `soap.[ch]`, tests |
| **T17** | WS-Discovery (UDP multicast join, Probe/Hello/Bye) | `src/ak_rtsp/wsd.[ch]`, tests |
| **T18** | Device service (info, capabilities/services, time, hostname, network, reboot, scopes) incl. WS-Security and access classes | `onvif_device.[ch]`, tests |
| **T19** | Media 1 (single profile, StreamUri, audio source/output descriptions) | `onvif_media.[ch]`, tests |
| **T20** | Imaging (IrCutFilter, ISP mapping, options) | `onvif_imaging.[ch]`, tests |
| **T21** | Helper `akam_onvif.sh`, wizard `./akam onvif`, `verify` question, hwcheck lines (ports 80/3702), docs | tools, `akam`, docs |
| **T22** | Optional: Media2 (Profile T), Events skeleton | |
| **T23** | HW validation H1 to H10, budget gate, fixes | report |

(Study tasks, not committed: GetSnapshotUri via hardware JPEG; a `sub` profile with a second encoder; both need their own spec.)

## 4. Step 4: port 8091 becomes read-only debug and metrics

### 4.1 Scope

- 8091 keeps the line protocol (`LIST`, `GET`, `STATUS`, `HELP`, `HELLO`, `PING`, `QUIT`, JSON) and is **read-only by default** for any remote client.
- Writes (`SET`, `SAVE`, `RESET`) are accepted only (a) from the **loopback address** (every SSH helper already runs `nc 127.0.0.1 8091` on the camera, e.g. `./akam led`) and (b) from remote clients while a **debug flag** is on. The flag is **RAM-only, not persisted, off at every start of `ak_rtsp` and at reboot** (D1 step 4), set over SSH: `akam_debug.sh on [minutes]` which sends `DEBUG on [minutes]` from loopback. Optional auto-off, default 60 minutes (U12).
- Refused writes answer `ERR read_only` (v2) or `ERR read-only mode, see akam_debug.sh` (v1 text). The v1 grammar and read behaviour are otherwise unchanged (spec 04 compatibility, golden tests).
- Spec 04 **sections 1 to 8 stay** (grammar, keys, JSON, concurrency, error codes); **sections 9 and 10 (HMAC, `akam_ctl_auth.sh`, `./akam ctl-auth`, `ctl_key`) are superseded** and not built; spec 04 tasks T1 (SHA-256/HMAC), T5, T6 and the auth part of T9 are dropped; T2, T3, T4, T7 (without auth), T8, T10, T11 remain. SHA-1/base64 (step 3) replaces SHA-256 in the plan.

### 4.2 Metrics (reads only; names from spec 04 section 3.1, sources as there)

CPU % (`sys.cpu`), RAM total/free, load, uptime, per-stream bitrate and fps (`video.bitrate`, `video.fps_actual`), RTSP state and client count (`rtsp.state`, `rtsp.client`, `rtsp.since`, new `rtsp.clients` count), `rtsp.auth`, Wi-Fi RSSI/SSID and link rate (`net.wifi_rssi`, `net.ssid`, new `net.wifi_rate` from `SIOCGIWRATE`, `unknown` if the driver says nothing), audio counters (`audio.out_underruns`, `audio.out_drops`, `audio.out_buf_ms`, backchannel packets/dropped), night state, LED state, **temperature if the platform exposes it** (candidate sources `/sys/class/thermal` or hwmon: not verified, otherwise absent), `ctl.clients`, new `ctl.debug` (0/1). All `unknown` when not available, never invented. Cached 1 s as spec 04 3.3.

### 4.3 Local control without TCP

No extra socket is needed: loopback writes cover SSH use. The multi-call alias `ak_rtsp ctl <line>` (the binary connects to 127.0.0.1:8091, sends the line, prints the reply, exits) gives access that does not depend on the BusyBox `nc` and is what the helper scripts use for the loopback-only commands `DEBUG on|off [minutes]`, `MQTT reload` and `ONVIF reload`; it is handled at the very beginning of `main()` before any hardware init (no change to the video path).

### 4.4 Security model

- Remote clients: read-only, as before for the sensitive data (no secret is ever exposed, spec 04 3.4). Without the flag, a LAN attacker can read state and metrics but change nothing. With the flag, remote SET is possible for the session (documented for the tuning tool).
- Loopback trust: only processes on the camera (root over SSH, scripts) connect from 127.0.0.1; a remote host cannot spoof it. The `DEBUG` command is loopback-only.
- Rate limits and slot behaviour of spec 04 section 7 are kept (4 slots, idle timeout, `LOG` non-blocking).

### 4.5 Migration: every 8091 SET user found in the repository

| User | Today | Moves to |
|---|---|---|
| `./akam led` (`tools/wizard/lib/steps_camera.sh`, `tests/wizard_led_test.sh`, branch `feature/led-controls`) | `ssh ... printf ... | nc -w 2 127.0.0.1 8091` | **Unchanged**: loopback is allowed to SET. Also controllable from MQTT (`led.status`, `night.ir_led`) |
| `./akam hostname` (`akam_hostname.sh`, spec 04 section 6) | SSH helper, applies at next power cycle | Stays SSH helper; also ONVIF `SetHostname` (step 3). Not on MQTT |
| `docs/troubleshooting.md` audio step A (lines about `SET audio.out_*`, `audio.out_test`, `audio.out_amp`, `audio.out_enable` via `nc <cam> 8091`) | Remote `nc` from the PC | `./akam audio ...` over SSH (T5) or `ssh cam 'printf ... | nc 127.0.0.1 8091'`; volume/beep also on MQTT; doc rewritten (T26) |
| `tools/hw_tests/akam_hwcheck.sh` (`LIST`, `GET night.mode`/`night.state` on 127.0.0.1) | Reads only | **Unchanged** (reads); update expected ports (add 80 and UDP 3702 when ONVIF is on) and a `debug` info line |
| `tools/hardware_debug/serial_console_dotnet` (`CameraControlService.cs`, `CameraTuningViewModel.cs`, `MainWindow.xaml`, tuning tab: SET `ae.*`, `night.*`, `isp.*`, remote) | Remote SET | Needs **debug mode on** (`akam_debug.sh on`); the tool shows a clear message on `ERR read_only` (T26). Persistent tuning stays in `ctl_store` |
| `docs/serial_console_and_camera_tuning.md`, `docs/akam_v1_install.md` section 4, `README.md` (8091 paragraph), `docs/akam_v1_test_plan.md` (manual `SET` checks, port expectation), `docs/repository_overview.md` | Describe an open read/write port | Rewritten to "read-only metrics, writes over SSH or debug mode" (T26) |
| `tools/wizard/lib/steps_camera.sh` `cam_ports_check` | Expects exactly 22, 554, 8091 | Add 80 (ONVIF) and tolerate UDP 3702 only when the user enabled ONVIF; MQTT adds nothing (outbound) |
| `tests/audio_out_test.sh`, `tests/native/audio_out_test.c` (8091 key tests), `tests/leds_test*` | Call `ao_ctl_*` internal functions | Unchanged (internal API); add read-only-mode tests in `ctl_server_test.c` |
| `specs/04`, `specs/05` §6.1 (clés 8091 `audio.out_*`) | Write via 8091 | Same keys; writes through the paths above; marked superseded where noted in those specs |
| spec 05 talk 8092 / `./akam talk` HMAC | Never implemented | Replaced by step 1 (backchannel) |

### 4.6 Test plan

Host (`tests/native/ctl_server_test.c`): remote SET refused with the right error in v1/v2; loopback SET accepted; `DEBUG on/off/timeout` (remote cannot send it); flag off after server restart; golden v1 read outputs unchanged; `LIST/STATUS json` with new keys; `ak_rtsp ctl` alias against a test server; the metric collectors on a fake `/proc` and `/sys` root. Hardware: the tuning tool and `./akam led` still work (flag/loopback); `nc <cam> 8091` from the PC: reads work, SET refused; after `akam_debug.sh on` remote SET works and stops at the timeout and after reboot; metrics plausible against `top`, `free`, `iwconfig`; temperature present or absent.

### 4.7 Tasks

| Task | Content | Files |
|---|---|---|
| **T24** | `control.c` read-only gate, loopback/`DEBUG`, `ak_rtsp ctl` alias, error codes, golden tests (integrates with spec 04 T7) | `control.c`, `main.c`, tests |
| **T25** | New metric keys (`rtsp.clients`, `net.wifi_rate`, backchannel counters, `ctl.debug`, temperature probe) on top of spec 04 T2/T8 | `ctl_sys.[ch]`, `stats.[ch]` |
| **T26** | Helper `akam_debug.sh`, wizard `./akam debug`, doc and tool migration of 4.5 (README, `docs/**`, tuning tool message, hwcheck, test plan), CHANGELOG | many |
| **T27** | HW validation, budget gate | report |

## 5. Cross-cutting

### 5.1 Order and release

Steps ship in order (1, 2, 3, 4); each merges to `dev` only after its HW test; each is a minor release candidate (suggested V1.1 = steps 1 and 4 read-only part, V1.2 = MQTT, V1.3 = ONVIF; user may regroup). Step 4's read-only gate must land **no later than step 1's release** if the old 8092/HMAC plan is dropped, because spec 04's auth would not exist: until then 8091 writes stay open on the LAN as today (documented). Task T24 can therefore run early and in parallel with step 1 (disjoint files). Step 2 and 3 are independent of each other; T9 (settings registry) and T15 (credentials) are shared prerequisites.

### 5.2 Wizard and docs

New `./akam` commands: `talk`, `say`, `audio`, `mqtt`, `onvif`, `debug`, plus `rtsp-auth` writing the credential file. Each step updates README, `docs/akam_v1_install.md` (port table: 554 with backchannel, 80 and 3702 when ONVIF on, 8091 read-only, no inbound MQTT), `THIRD_PARTY_NOTICES.md` (BearSSL if used; nothing for own code), CHANGELOG, `akam_hwcheck.sh`, the test plan.

### 5.3 Spec cross-references

Specs 04 and 05 carry a banner pointing here and mark the superseded sections (done in the same commit). Anything not marked there stays valid.

## 6. Open questions for the user (each with a recommendation)

- **U1. ONVIF implementation and license.** Own clean-room MIT implementation (recommended), versus embedding onvif_simple_server (GPL-3.0). Using it as a separate CGI process keeps `ak_rtsp` MIT but ships a GPL-3.0 binary with source-offer obligations and a CGI web server; linking or copying it forces relicensing `ak_rtsp` to GPL-3.0. **Recommend: own implementation.** Your decision if you would rather accept GPL.
- **U2. Plaintext password on the camera.** WS-Security PasswordDigest and MQTT need it; spec 03 promised "never stored". **Recommend: accept** `/etc/config/akam/credentials` (0600), written by the wizard, used by ONVIF; MQTT broker password in its own file; the HA1 file stays for RTSP. Alternative: ONVIF HTTP Digest only (no plaintext) with lower client compatibility.
- **U3. Backchannel unavailable (no password).** Reply 551 to a Require request (**recommended**, standard) versus 200 without the track.
- **U4. Second RTSP slot.** Recommend: **not now**; decide after HW test H3 (go2rtc/Frigate behaviour). If go2rtc needs a second connection to talk while Frigate records, implement 1 video + 1 talk-only slot (T7).
- **U5. Host client for `./akam talk|say`.** **Recommend: `akam-talk` in C, zig cross-built, shipped in the kit** (versus Python script, versus go2rtc-only).
- **U6. Half-duplex.** **Recommend:** mic muted only while speech plays (energy gate, 300 ms hangover), `audio.talk_duplex=half|full` setting, default `half`.
- **U7. MQTT library.** **Recommend: own minimal MQTT 3.1.1 client** (MIT, about 600 lines); fallback MQTT-C (MIT).
- **U8. MQTT TLS.** **Recommend: include, as separate step 2b** with BearSSL (MIT), user-provided CA or fingerprint pin, plain 1883 still allowed; ship only if measured cost stays within the budget table.
- **U9. MQTT command scope and defaults.** **Recommend** the allow-list of 2.2 (no reboot, no network/password), discovery prefix `homeassistant`, base topic `akam/<hostname>`, sensor interval 30 s.
- **U10. ONVIF port.** **Recommend 80** (default of most NVR and HA setups; discovery gives the real port anyway), configurable; versus 8000/8080 to avoid the classic web port.
- **U11. ONVIF default and no-password behaviour.** **Recommend:** ONVIF off by default, proposed by the wizard (default no); with password OFF: reads open, state-changing operations refused; versus requiring the password for any ONVIF use.
- **U12. 8091 writes.** **Recommend** loopback SET always allowed plus remote SET only under the RAM-only debug flag with a default 60-minute auto-off; versus flag-only (every `./akam led` would have to toggle debug mode).
- **U13. Time, NTP and sub-stream.** Recommend: accept `SetSystemDateAndTime` only, no NTP client in this spec (note in docs; separate study if wanted); ONVIF ships a **single `main` profile**; a `sub` stream (second encoder) is a separate future spec.
- **U14. Snapshot.** **Recommend defer** (`GetSnapshotUri`, hardware JPEG): HA/Frigate/NVRs work from RTSP.
- **U15. Supersession.** **Recommend** that spec 04 HMAC auth (sections 9, 10, T1, T5, T6, ctl-auth) and spec 05 port 8092 (sections 4.1, 5, 8, T3, T5) are formally dropped as written in the banners; confirm.
- **U16. Release grouping.** Recommend the grouping of 5.1 (V1.1 = step 1 + read-only 8091, V1.2 = MQTT, V1.3 = ONVIF).
