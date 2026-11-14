# Control port (TCP 8091)

Port 8091 is a plain-text debug, metrics and tuning port served by `ak_rtsp` (`control.c`, `ctl_gate.c`, `ctl_sys.c`).
Reads are open to the LAN. Writes are refused from the network unless a timed, RAM-only debug flag is on; commands sent
from the camera itself (SSH helpers) always work. There is no password and no encryption on this port.

Status: **hardware-validated**: metrics and reads from the network, `SET` refused remotely, loopback `SET`. **Not yet
hardware checked**: strict `SET` ranges, debug-mode expiry on the final image.

## 1. Protocol

- TCP, listens on `0.0.0.0:8091` (listen backlog 4). One thread per client.
- Text, one command per line, LF-terminated (CR ignored). A line is at most 255 bytes; longer input is truncated
  without an error and the truncated line is executed.
- Commands are case-insensitive. Keys are case-sensitive.
- Words are split on single spaces. `SET key value` takes exactly one value word; anything after it is ignored.
- Every reply is one line, `\n`-terminated, except `LIST`, which sends one `key=value` line per key and ends with a line
  containing only `.`. A CR or LF inside a value is replaced by `?` so a value cannot forge a line.
- Reply forms:

| Form | Meaning |
|---|---|
| `key=value` | Answer to `GET` |
| `OK key=value[ (note)]` | Successful `SET`. The note says `(not saved: reason)` or `(restart to apply)` |
| `OK ...` | Other successful commands (`OK bye`, `OK ctl.debug=1 minutes=60`, ...) |
| `ERR text` | Any error. Free text, for humans |
| `LOG text` | Asynchronous log line (section 5). May arrive at any time, including inside a `LIST` block |

A client that sends nothing is dropped after an idle limit (section 4). Clients SHOULD ignore lines they do not expect,
in particular `LOG` lines, and SHOULD end a `LIST` read at the `.` line.

## 2. Commands

| Command | Who | Reply |
|---|---|---|
| `LIST` | anyone | All keys of section 6, then `.` |
| `GET key` | anyone | `key=value`, or `ERR unknown param key` |
| `SET key value` | loopback, or anyone while debug is on | `OK key=value`, or `ERR ...` |
| `QUIT` | anyone | `OK bye`, then close |
| `DEBUG on [MINUTES]`, `DEBUG off`, `DEBUG status` | loopback only | `OK ctl.debug=1 minutes=N`, `OK ctl.debug=0`, `ctl.debug=1 remaining=S` or `ctl.debug=0`. MINUTES is 1 to 1440, default 60 |
| `ONVIF reload` | loopback only | `OK onvif.state=STATE` (starts or stops ONVIF from its flag file) |
| `MQTT reload` | loopback only | `OK mqtt.state=STATE`, or `ERR mqtt.state=STATE` when the start failed |
| `MQTT test` | loopback only | `OK mqtt.test=running`, or `ERR no valid MQTT configuration` |

Unknown commands answer `ERR unknown command X`. The words `SAVE` and `RESET` are classed as write commands by the gate
but are not implemented (they answer `ERR unknown command` from loopback and the read-only error from the network).
Remote use of `DEBUG`, `ONVIF` or `MQTT` answers an `ERR` that says it is allowed only from the camera.

## 3. Write rule

A command that changes state (`SET`, `SAVE`, `RESET`) is allowed only when the peer is loopback or the debug flag is on;
otherwise the answer is `ERR read-only mode, see akam_debug.sh` and nothing changes.

- **Loopback** means a peer in `127.0.0.0/8` (also `::1` and IPv4-mapped 127/8). The SSH helper scripts reach it through
  `ak_rtsp ctl <line>` or the camera's `nc`.
- **Debug flag**: RAM only, off at every start of `ak_rtsp` and at reboot, off by itself after its delay (monotonic
  clock). Switched on only from loopback (`DEBUG on [MINUTES]`, wrapped by `akam_debug.sh on [MINUTES]`). While it is
  on, remote peers may write, receive `LOG` lines, see `net.ssid` and `net.mac`, and get the long idle limit.
- **Persistence follows the source.** A loopback `SET` saves what is persistent. A remote `SET` (debug) changes the
  running value in RAM only, so a slider dragged by a PC tool does not write flash at every step. Two groups save
  regardless of source: `led.status` and `night.ir_led`.
- The same setters serve ONVIF Imaging and MQTT, which behave like loopback or RAM-only callers as listed in their specs.

## 4. Connections and limits

| Limit | Value |
|---|---|
| Concurrent clients | 4 (`CTL_MAX_CLIENTS`); at most 3 remote, so one slot always stays free for loopback |
| No free slot | `ERR busy`, then close |
| Idle limit | 900 s for loopback or while debug is on; 120 s for a remote peer |
| Session cap | A remote peer (debug off) is cut after 3600 s even if it keeps sending |
| Send timeout | 2 s per reply to a stuck client |
| Thread stack | 64 KiB per client thread |

## 5. Privacy

- `LOG` lines carry client IP addresses, authentication failures and request URIs. They are sent only to loopback peers,
  or to every peer while debug is on.
- `net.ssid` and `net.mac` read `hidden` for remote peers unless debug is on. Loopback readers see everything.
  `net.ip` and `rtsp.clients` (a count) are readable by anyone.
- No key exposes a password, a PSK, an HA1, a key or the RTSP client's name or URI.
- Values that cannot be known read `unknown`; no value is invented.
- Credentials of RTSP, ONVIF and MQTT are never managed through this port.

### The `LOG` stream

`control_start()` redirects stdout into a pipe before any other initialisation, so the full boot log reaches the port.
A pump thread writes the exact bytes to the real console and forwards complete lines (up to 1023 bytes) as
`LOG <line>`. A line is never cut by a reply (one writer at a time per socket). A slow client misses lines rather than
blocking anything. `GET log.dropped` counts lines lost to a full pipe or a stalled console.

## 6. Keys

Access: `r` read-only, `rw` read-write (write rule of section 3), `w` write-only. Persistence: "file" means a loopback
`SET` saves it (file named); "RAM" means a `SET` is lost at restart; "-" means read-only.

### 6.1 Auto exposure and picture

| Key | Acc | Range or values | Persist |
|---|---|---|---|
| `ae.stable_range`, `ae.hold_range`, `ae.speed` | rw | 0 to 100 | RAM |
| `ae.exp_max` | rw | 0 to 32767 | RAM |
| `ae.enabled` | rw | 0 or 1 | RAM |
| `isp.brightness`, `isp.contrast`, `isp.saturation`, `isp.sharpness` | rw | -50 to 50 | file `picture` |
| `venc.minqp`, `venc.maxqp` | r | `28 (read-only)`, `43 (read-only)` (the suffix is part of the value) | - |

A numeric `SET` is parsed strictly (whole decimal string, no trailing text, no overflow). An invalid value answers
`ERR <key> range [lo,hi]` and changes nothing. `SET venc.minqp` answers an error saying it needs an encoder restart.

### 6.2 Day/night, image debug

See [image-day-night-leds.md](image-day-night-leds.md) for ranges: `night.mode`, `night.state`, `night.trigger_hw_exp`,
`night.day_hw_exp`, `night.confirm_samples`, `night.lock_ms`, `night.gain_pct`, `night.day_confirm_samples`,
`night.min_night_ms`, `night.probe_ms`, `night.probe_settle_ms`, `night.ae`, `night.logic`, `night.ir_led`, and
`img.venc_rc`, `img.wb`, `img.poll`, `img.reload`, `img.dump`. `led.status` and `led.state` are in the same file.

| Key | Acc | Values | Persist |
|---|---|---|---|
| `led.status` | rw | `on`, `off`, `auto` | file `leds` (any source) |
| `led.state` | r | `system`, `on`, `off`, `blink_wifi`, `blink_stream` | - |
| `night.ir_led` | rw | `auto`, `off` | file `leds` (any source) |

### 6.3 Audio

| Key | Acc | Values | Persist |
|---|---|---|---|
| `audio.out_enable` | rw | 0 or 1 (`(restart to apply)` when the device is not open) | file `audio_out` |
| `audio.out_volume` | rw | 0 to 100 | file `audio_out` |
| `audio.out_gain` | rw | 0 to 5 | file `audio_out` |
| `audio.out_source` | rw | 4 or 6 | file `audio_out` |
| `audio.out_amp` | rw | `auto`, `on`, `off` | RAM |
| `audio.out_test` | w | `1` plays the beep (`ERR audio out is not running` otherwise); reads 1 while playing | RAM |
| `audio.out_state` | r | `off`, `idle`, `talking`, `test`, `chime`, `disabled_crash`, `error` | - |
| `audio.out_underruns`, `audio.out_drops` | r | counters (underruns; samples dropped by overrun) | - |
| `audio.out_buf_ms` | r | milliseconds queued in the jitter buffer | - |
| `audio.talk_enable` | rw | 0 or 1 | file `audio_out` |
| `audio.talk_duplex` | rw | `half`, `full` | file `audio_out` |
| `audio.mic_enable` | rw | 0 or 1; applies at the next start (`(restart to apply)`) | file `audio_out` |
| `audio.talk_state` | r | `idle`, `active` | - |
| `audio.talk_codec` | r | `pcmu`, `pcma`, `none` | - |
| `audio.talk_packets`, `audio.talk_dropped`, `audio.talk_lost`, `audio.talk_sessions` | r | counters | - |
| `audio.talk_mic_muted` | r | 0 or 1 | - |

### 6.4 Video settings

| Key | Acc | Values | Persist |
|---|---|---|---|
| `video.bitrate_cfg` | rw | 500 to 3000 (kbit/s) | file `video`, applies at next start |
| `video.fps_cfg` | rw | 10, 15, 20, 25 | same |
| `video.gop_cfg` | rw | 1 to 4 (seconds) | same |
| `video.cfg_pending` | r | 1 when the saved file differs from the running encoder | - |

These keys are writable only from loopback (nothing would apply them from the network). The answer says
`(restart to apply)` while a change is pending.

### 6.5 Metrics (read-only)

Collected without `fork` or `malloc` from small `/proc` and `/sys` reads and a few ioctls, at most once per second, and
shared by all callers.

| Key | Meaning |
|---|---|
| `sys.uptime` | Seconds since boot |
| `sys.load` | `0.12 0.10 0.05` (first three fields of `/proc/loadavg`) |
| `sys.cpu` | CPU busy percent since the previous collection; `unknown` on the first |
| `sys.mem_total`, `sys.mem_free` | kB (`MemAvailable`) |
| `sys.temp` | Degrees C, one decimal, when a sensor is present |
| `sys.time_set` | 1 when the wall clock is 2024 or later, else 0 |
| `net.ip` | IPv4 of `wlan0` (else `eth0`) |
| `net.mac` | `aa:bb:cc:dd:ee:ff`, lowercase. Hidden from remote peers (section 5) |
| `net.ssid` | Connected network name. Hidden from remote peers. Source order: wireless ioctl, then the configured name in `wpa_supplicant.conf` (never the PSK) |
| `net.wifi_rssi` | dBm (`/proc/net/wireless`, else wireless ioctl) |
| `net.wifi_rate` | Mb/s |
| `video.fps_actual` | Frames per second times 10 over the last 5 s (`199` = 19.9); `unknown` outside PLAY |
| `video.bitrate` | kbit/s over the last 5 s; 0 outside PLAY |
| `rtsp.clients` | Connected RTSP clients |
| `rtsp.auth` | `off`, `on`, `broken` |
| `rtsp.skipped_frames` | Cumulative video frames dropped by the TCP backpressure pacing |

### 6.6 Control and service state

| Key | Meaning |
|---|---|
| `fw.version` | Firmware version string (`git describe`) |
| `sys.hostname` | Network host name (read-only here; change it with `akam_hostname.sh`) |
| `ctl.clients` | Open connections on this port |
| `ctl.debug` | 0 or 1 |
| `ctl.debug_remaining` | Seconds left (`GET` only) |
| `log.dropped` | Lost log lines (`GET` only) |
| `onvif.state` | `off`, `on:<port>`, `no_password`, `error:<reason>` |
| `mqtt.state` | `off`, `connecting`, `connected`, `error:<reason>` |
| `mqtt.stats` | `published=N received=N applied=N rejected=N connects=N` (`GET` only) |
| `mqtt.test` | `idle`, `running`, `ok`, `error:<reason>` (`GET` only) |

`LIST` prints, in this order: `ae.*`, `night.mode`/`night.state` and the other `night.*`, `isp.*`, `venc.*`, the night,
LED, audio, image, talk and video keys, the metrics, then `ctl.clients`, `ctl.debug`, `fw.version`, `sys.hostname`,
`onvif.state`, `mqtt.state`, and the `.` terminator.

## 7. The `ak_rtsp ctl` helper

`ak_rtsp ctl <words...>` joins its arguments into one line, sends it to `127.0.0.1:8091`, and prints the reply without
`LOG` lines. It ends at the `.` line of a `LIST`, or 250 ms after the last data once a reply line arrived (1 s while none
did), and at most 3 s. Exit 0 when the first reply line is not `ERR`, 1 otherwise (including no connection). It runs
before any hardware initialisation. The helper scripts use it instead of the BusyBox `nc`.

## 8. Tests

`tests/control_test.sh` (with `tests/native/control_test.c`), `ctl_sys_test.sh`, `akam_debug_test.sh`,
`wizard_debug_test.sh`.
