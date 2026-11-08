# Akam camera API (V1) — integration brief

How a host-side application (for example a lightweight self-hosted web UI) talks to a camera running
the Akam firmware. Everything below is what the V1 firmware actually implements (source:
`src/ak_rtsp/rtsp.c`, `src/ak_rtsp/control.c`); behaviour marked *validated* was observed on a real
LSC 3215672.3 camera on 2026-11-08.

## 1. Network surface

| Port | Protocol | Purpose | Auth |
|---|---|---|---|
| 554/TCP | RTSP 1.0 (RTP over TCP interleaved, or UDP) | Live video + audio | **None** |
| 8091/TCP | Line-based text protocol | Read/write image and day/night settings, live logs | **None** |
| 22/TCP | SSH (Dropbear, ed25519 public key only) | Administration, firmware update | Key |

Nothing else listens in normal operation (validated: only 22/554/8091). The camera has no RTC/NTP:
its clock starts at 2000-01-01 on every boot — never trust camera wall-clock timestamps.
Use a DHCP reservation so the camera keeps the same IP. **Never expose 554 or 8091 to the
Internet**; put a host-side proxy with authentication in front of them.

## 2. Video/audio: RTSP on 554

- URL: `rtsp://<camera-ip>:554/` — any path works (`/live`, `/main_ch`, …), all give the same stream.
- Tracks (SDP, validated with ffprobe):
  - `streamid=0`: H.264 Main profile, 1920×1080, ~15 fps, payload type 96,
    `packetization-mode=1`, `profile-level-id` and `sprop-parameter-sets` (SPS/PPS) announced in the
    SDP once the first I-frame has been seen after boot. Video is sent starting at an IDR frame, so
    players decode within ~1–2 s (validated in VLC/ffplay).
  - `streamid=1`: audio L16 (16-bit linear PCM, big-endian per RFC 3551), 8000 Hz, mono, payload type 97.
- Transport: prefer **RTP over TCP** (`-rtsp_transport tcp` in ffmpeg, `--rtsp-tcp` in VLC).
- **One RTSP client at a time.** A second client cannot play while another is connected. A dead
  client is dropped after ~5 s of blocked writes or ~1 min of TCP keepalive failure.
  → A web UI must **not** connect browsers directly. Run one relay per camera on the host
  (for example go2rtc or MediaMTX) that holds the single RTSP session and fans it out to browsers
  (WebRTC / MSE / HLS). Transcoding is not needed for H.264; L16 audio usually needs transcoding
  to Opus/AAC for browsers.
- Bitrate is variable, about 1.5 Mbit/s max (encoder max_kbps 1536, QP 28–43, read-only in V1).

## 3. Settings and logs: text protocol on 8091

### Connection rules
- Plain TCP, ASCII, one command per line terminated by `\n` (`\r` is ignored). Commands are
  case-insensitive; parameter names are not.
- **One control client at a time, served sequentially**: while a client is connected, further
  connections wait in the TCP backlog until it disconnects. A backend should either keep a single
  long-lived connection per camera and serialise requests through it, or open short connections
  (connect → command(s) → close) and never hold one idle.
- The server pushes asynchronous log lines prefixed with `LOG ` at any time on the open connection
  (e.g. `LOG [control] tuning client connected`, `LOG [night] ...`, `LOG [ae] window ...`).
  Clients must ignore or route `LOG ` lines and match replies on the other lines.
- Settings are **volatile**: they reset to defaults when the camera or `ak_rtsp` restarts. A web UI
  that wants persistence must store them host-side and re-apply them after reconnecting
  (detect restarts by reconnecting and comparing values, or via the `tuning client connected` log).

### Commands
| Command | Reply |
|---|---|
| `LIST` | one `key=value` line per parameter, then a line containing only `.` |
| `GET <param>` | `<param>=<value>` or `ERR unknown param <param>` |
| `SET <param> <value>` | `OK <param>=<value>` (value as applied) or `ERR <reason>` |
| anything else | `ERR unknown command <cmd>` |

Read-only entries in `LIST` carry a ` (read-only)` suffix after the value.

### Parameters
| Parameter | Type / range | Meaning |
|---|---|---|
| `night.mode` | `auto` \| `day` \| `night` | Day/night policy (IR-cut + IR LEDs). `auto` switches on exposure with hysteresis and a lock. |
| `night.state` | `day` \| `night` (read) | Current state. |
| `night.trigger_hw_exp` | unsigned int (default 3000) | Exposure above which `auto` goes to night. |
| `night.day_hw_exp` | unsigned int (default 1200) | Exposure below which `auto` goes back to day. |
| `night.confirm_samples` | int (default 10) | Consecutive samples required before switching. |
| `night.lock_ms` | int ms (default 60000) | Minimum time between two switches. |
| `isp.brightness` | int −50…50 (default 0) | Relative to the sensor baseline. |
| `isp.contrast` | int −50…50 (default 0) | |
| `isp.saturation` | int −50…50 (default 0) | |
| `isp.sharpness` | int −50…50 (default 0) | |
| `ae.enabled` | 0 \| 1 | Custom auto-exposure loop on/off. |
| `ae.speed`, `ae.stable_range`, `ae.hold_range`, `ae.exp_max` | int | Auto-exposure tuning (advanced; expose only in an "expert" section). |
| `venc.minqp`, `venc.maxqp` | int (read-only) | Encoder quantiser bounds. |

Example session (validated):
```
> GET night.mode
< night.mode=auto
> SET night.mode night
< OK night.mode=night
> SET isp.brightness 80
< ERR isp.brightness range [-50,50]
```

## 4. Administration over SSH (optional for a web UI)

- `ssh -i <key> root@<camera-ip>`; root's key comes from the microSD at first boot and is persisted in
  `/etc/config/dropbear/authorized_keys`. Host key: ed25519, persisted (pin it, warn on change).
- Useful read-only checks: `netstat -ltn`, `cat /proc/loadavg`, `free`, `sh /tmp/sd/akam_hwcheck.sh`
  (only if the maintenance card is inserted). Firmware updates go through
  `tools/flash_scripts/reflash_over_ssh.sh` from a trusted host — a web UI should not flash.
- BusyBox 1.30.1 userland; `nc` exists on the camera, `tr`/`mktemp`/`wget` do not.

## 5. Discovery

No mDNS/ONVIF/UPnP in V1. Add cameras by IP. A probe can be: TCP connect to 554 and 8091, then
`GET night.state` on 8091 (an Akam camera answers `night.state=day|night`). The Wi-Fi chip reports
vendor "AltoBeam" in router client lists.

## 6. Not available in V1 (do not build UI for it yet)

Persistent settings on the camera, bitrate/resolution/fps control, noise-reduction controls (planned
for V1.1 "reduce grain"), audio output/push-to-talk, motion detection, recording, authentication on
554/8091, time synchronisation.
