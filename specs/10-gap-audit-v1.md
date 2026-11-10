# Akam V1: gap audit (stability, security, configuration, consumption, release)

Status: written 2026-11-11 from `dev` (commit `351ec26`), read-only audit, no camera access. Everything
marked **(HW)** needs a real camera to confirm or to fix. Items marked **(to verify)** are inferences from
code or docs, not observations.

Goal being audited against: a stable camera, well configurable, secure, with optimised consumption
(CPU/RAM/power). Vision: light and fast, integrates through standards (RTSP, ONVIF, optional MQTT),
everything optional is off by default, budgets are measured.

Priorities: **P0** must fix or verify before calling V1 stable/secure; **P1** important; **P2** nice to have.
Effort: S (under a day), M (a few days), L (a week or more).

Scope exclusions: ONVIF device/media/imaging/wizard (in progress on other branches) are not audited, only the
wiring that is still missing (section 1).

---

## 0. Summary table

| ID | Pri | Title | Effort | HW |
|---|---|---|---|---|
| G01 | P0 | No liveness supervision: a hung `ak_rtsp` is never restarted; crash/respawn path never tested | M | yes |
| G02 | P0 | stdout tee can freeze the whole daemon if the real stdout blocks; stdout destination unverified | S | yes |
| G03 | P0 | Wi-Fi/DHCP are started once and never supervised; reconnect never validated | M | yes |
| G04 | P0 | Respawn without backoff or crash counter (tight crash loop on a persistent init failure) | S | yes |
| G05 | P1 | Unauthenticated remote 8091 clients receive the full log stream and Wi-Fi SSID/MAC/IP | S | no |
| G06 | P1 | Second RTSP client hangs in the accept queue instead of getting an immediate answer | S | yes |
| G07 | P1 | Audio capture thread stops forever after 5 read errors; mic and speaker run even if unused | M | yes |
| G08 | P1 | No clock source (no RTC, no NTP): WS-Security window disabled, RTCP SR wrong, no timestamps | M | yes |
| G09 | P1 | Unbounded RAM logs in `/tmp` (Dropbear `-E`, boot logs); no soak metrics (RSS, fds, threads) | S | yes |
| G10 | P1 | Idle wakeups about 140/s (AE, night, drain poll, audio capture, silent playback) | M | yes |
| G11 | P1 | Video not configurable (resolution, fps, bitrate cap fixed at compile time); no mic enable/gain; no mirror/flip | M to L | yes |
| G12 | P1 | Release readiness: no tag, `main` is the initial commit, no signing key committed, no CI | S | no |
| G13 | P1 | Hardware test debt (list in section 6) | M | yes |
| G14 | P1 | `hwcheck` and the docs only check TCP listeners; UDP listeners and remaining vendor daemons not audited | S | yes |
| G15 | P2 | SD autorun/OTA: no signature on SD firmware update (guarded by a flag only) | S (doc) | no |
| G16 | P2 | `ae.*` / `night.*` SET values unvalidated (`atoi`), torn reads on tuning structs | S | no |
| G17 | P2 | Hardening flags (no stack protector, FORTIFY, RELRO, PIE) | S | yes |
| G18 | P2 | Legacy cruft (camyn/ aliases, `--no-isp-init`, dual AE paths, stale comments, dev binaries) | S | no |
| G19 | P2 | Secrets in backups: Wi-Fi PSK and clear-text ONVIF password inside the CONFIG partition backup | S (doc) | no |
| G20 | P2 | OSD timestamp, motion detection, timezone, `akam config` summary command | L | yes |
| G21 | P2 | Optional MQTT (no broker available to the user) | L | yes |
| G22 | P2 | Wi-Fi power save and AP-side tuning | S | yes |

---

## 1. ONVIF wiring still missing (do not duplicate the in-progress branches)

`dev` has cred, http_srv, soap and wsd merged, but nothing starts them: `grep -n "http_srv_\|wsd_\|onvif" src/ak_rtsp/main.c`
returns nothing. Remaining glue, to be done once device/media/imaging/wizard land:

- `main.c`: start `http_srv` and `wsd` after `listen()` (main.c:333-340) and before the accept loop; stop them in
  the `err:` path (main.c:371-382); both must be started only when ONVIF is enabled.
- Opt-in flag file `onvif.state` (on/off, default off) under `/etc/config/akam/`; reload rule: read at start, and
  re-read like `rtsp_auth_tick` (cheap stat every few seconds) so `./akam onvif on` needs no reboot.
- Scopes and hostname: `wsd` Hello and `GetScopes` must use the same name as `akam_hostname.sh show`, read once at
  start (no fork).
- Port checks at start: HTTP port (default 80 or 8080) already used, multicast join failure when Wi-Fi is not up
  yet. `wsd` retries every 5 s (`wsd.c:24`), `http_srv` must retry bind too, not abort `ak_rtsp`.
- `hwcheck` and `docs/akam_v1_test_plan.md` expected-ports list (22, 554, 8091) must gain the ONVIF port, and
  UDP 3702 (WS-Discovery) when enabled.
- Credential failures already share the limiter with RTSP (`rtsp_auth_note_failure`): verify on HW that a
  brute-force on ONVIF cannot stall the RTSP accept loop (the delay is taken in the caller's thread, see G06).
- ONVIF `SetSystemDateAndTime`, `Reboot`, `SetHostname` must stay refused or loopback-only (writes from the
  network are the rule of 8091 and should be the rule of ONVIF too).

---

## 2. P0 items

### G01 (P0) A hung ak_rtsp is never restarted; the crash and respawn path was never exercised

Evidence:
- `tools/firmware_patch_templates/_ht_process.ini` (6 lines): the stock `/usr/bin/watchdog` restarts a
  `process_0` entry, here `ak_rtsp_wrapper.sh`, when it dies. `docs/akam_v1_test_plan.md` H3 lists "the stock
  watchdog only restarts `_ht_process.ini` entries" as an unproven hypothesis; nothing checks that it restarts
  on a hang (it cannot: it sees a living process).
- Every worker is a free-running thread with no heartbeat: `ae_loop` (ae.c:275), `night_loop` (night.c:234),
  `audio_capture_loop` (audio.c:334), the main accept loop (main.c:340-372). A deadlock on `g_slot_lock`,
  `isp_cfg_lock` or a blocking `ioctl` on the ISP/VENC driver freezes the stream with no restart.
- `main.c:371-382` `err:` path returns 1: relies entirely on the wrapper being respawned. On respawn the
  kernel modules are NOT reloaded (`venc_reset_stale_channel`, main.c:240, is the only recovery) and the
  history shows the driver can divide by zero if the init order is wrong (main.c:48-60 comments).
- No test in the plan kills `ak_rtsp` and checks that the stream comes back; "VLC 25 minutes" is the longest run.

Risk: camera silently serving a frozen or black stream for days; or a respawn that wedges the sensor driver and
needs a power cycle (a camera at a remote location).

Fix:
1. A software watchdog thread in `ak_rtsp` (or in the main loop, which already wakes every 20 ms): record
   `last_frame_us` whenever `drain_venc_ring` or `handle_client` obtains a VENC frame; if no frame for 30 s
   while the pipeline should be running, `_exit(2)` after switching the speaker amplifier off
   (`audio_out_emergency_off`). Also a simple per-thread tick counter for `ae_loop` and `night_loop`
   (checked by the same thread), `_exit` when `ae` has not ticked for 60 s.
2. After N (3) respawns inside 10 minutes, the wrapper reboots the camera (`reboot`), see G04.
3. Hardware test: `kill -9 $(pidof ak_rtsp)` 20 times in a row, with and without a client connected, check the
   image comes back each time and `dmesg` shows no oops; same with `kill -STOP` (hang) to prove G01 step 1.

Effort M. HW yes.

### G02 (P0) The stdout tee can freeze every thread; the real stdout is unverified

Evidence:
- `control.c:625-637`: stdout is replaced by a pipe; `log_pump` (control.c:141-162) copies each chunk to the
  original stdout with a blocking `write(g_orig_stdout, ...)` (control.c:152) and ignores errors.
- If the original stdout blocks (a UART at 115200 baud under a burst, a pipe whose reader died, a full file
  system), `log_pump` stalls, the pipe fills (64 KiB), and the next `printf` in `ae_loop`, `night_loop`, the
  RTSP path or the audio code blocks too. Every thread that logs is then frozen. This is exactly the G01 hang,
  from a plausible cause.
- Steady log volume: one `[ae] window` line every second (ae.c:332), one `frame#` line per I-frame and every
  150th frame (rtsp.c:840-845), vclock/bitrate lines every ~10 s: about 150-200 bytes/s, 15 MB per day. Where
  it lands is not documented: `grep -rn "stdout\|/dev/console" docs` finds nothing about the camera, and the
  wrapper does not redirect `ak_rtsp` (ak_rtsp_wrapper.sh:52). It inherits whatever the stock watchdog gives it.
- `stderr` messages (`perror`, `fprintf(stderr, ...)`: audio.c:162, rtsp.c:827) are not part of the tee and
  skip the 8091 log view.

Risk: freeze of AE/night/RTSP when a console or reader stalls; or, if stdout is a file in `/tmp` (RAM), 15 MB/day
of RAM growth (the camera has about 29 MB free): OOM in two days.

Fix:
1. HW: `ls -l /proc/$(pidof ak_rtsp)/fd/1 /proc/$(pidof ak_rtsp)/fd/2` and `df /tmp` after 24 h. Record in the test plan.
2. Make `log_pump` never block the producers: open the original stdout `O_NONBLOCK` (or `poll` with 0 timeout)
   and drop lines when it is full; keep a drop counter in the 8091 metrics.
3. Redirect stdout and stderr of `ak_rtsp` in the wrapper to a bounded sink: `/dev/null` by default, and an
   in-process ring buffer (16 KiB, read through 8091 `LOG` or a `LOGDUMP` command) for diagnostics. Optional
   `/tmp/ak_rtsp.log` with a hard cap (rotate at 64 KiB, keep one old file) enabled by `./akam debug on`.
4. Demote the 1 Hz AE window print to every 60 s unless debug is on (also saves wakeups on the UART).

Effort S. HW yes (to confirm the stdout target; the code change is host-testable).

### G03 (P0) Wi-Fi and DHCP are launched once and never supervised

Evidence:
- `ak_rtsp_wrapper.sh:45-49`: `wpa_supplicant -B` and `udhcpc -i wlan0 &` are started only if no
  `wpa_supplicant` shows in `ps`. The check is on `wpa_supplicant` only: if `udhcpc` dies, nothing restarts it;
  the lease expires and the camera falls off the network while `ak_rtsp` keeps running (so the stock watchdog
  sees nothing). The check also only runs when the wrapper itself is respawned.
- `akam_lockdown.sh` protects `wpa_supplicant*` from its kill list (classify) but does not revive it.
- `udhcpc` is started without `-R`/`-b` options and relies on a stock `default.script` that was never read in
  this repository (to verify). Behaviour after a router reboot or a changed lease is untested:
  `docs/akam_v1_test_plan.md` criterion 12 only checks that Telnet stays closed after a Wi-Fi reconnect.
- No link-level check exists: no ping to the gateway, no `wpa_cli status`.
- The `atbm6x3x` USB Wi-Fi driver is a vendor module; behaviour on a long AP outage or channel change is unknown.

Risk: a camera that needs a physical power cycle after a router reboot. For a "stable" camera this is the most
likely real-world failure.

Fix: add `akam_net.sh` (POSIX sh, started detached by the wrapper like `akam_ssh.sh`, own lock): every 30 s check
(a) `wpa_supplicant` alive, restart if not; (b) `udhcpc` alive, restart if not; (c) `wlan0` has an IPv4 address
and the default gateway answers an ARP/ping; after 10 failed minutes run `wpa_cli reconnect`/restart both
daemons; after 30 minutes `reboot`. Log to the bounded sink of G02. Test on HW: router off for 2, 10 and 40 minutes;
change the AP channel; wrong password then fixed; DHCP server reboot; all with `akam_hwcheck.sh` after each.

Effort M. HW yes. Also add the router-reboot and AP-loss cases to the test plan (section 6).

### G04 (P0) Respawn loop has no backoff and no crash counter

Evidence: `_ht_process.ini` has `delay = 0` for `process_0`; the wrapper ends with `/usr/bin/ak_rtsp; exit $RET`
(ak_rtsp_wrapper.sh:52-54). Any init failure (`goto err` in main.c:243-334: sensor conf, a busy audio device,
`bind` failure on 554 because a previous instance is in TIME_WAIT is covered by SO_REUSEADDR but not other errors)
makes the watchdog relaunch immediately. Each cycle re-runs the ISP/VENC init (heavy ioctl sequences) and
re-spawns the three helper scripts (guarded by locks, but each still forks a shell).

Risk: 100% CPU, log flood, repeated sensor re-initialisation, possible kernel instability; the user cannot SSH in
reliably during the loop.

Fix: in the wrapper, measure the run time of `ak_rtsp`; if it lasted under 20 s, sleep `min(2^n, 60)` seconds
before exiting (counter in `/tmp/akam_respawn`); after 5 fast failures within 5 minutes, `reboot`; after 3
reboots in a row (counter on CONFIG, written rarely) stop respawning and leave Dropbear up so the user can log
in (safe mode), and say so on the front LED. Needs the stock watchdog behaviour to be read from a real dump
(H3). Effort S. HW yes.

---

## 3. P1 items

### G05 (P1) 8091 pushes the log stream and identity data to unauthenticated remote peers

Evidence: `control.c:114-131` `log_forward` sends every stdout line as `LOG ...` to every connected client slot,
remote or not; `control.c:565` binds `INADDR_ANY`. The log contains client IPs of RTSP and auth failures
(`rtsp.c:683`, `[ak_rtsp] auth: ...`), request URIs (credentials in the URI are masked), the build info, paths.
`ctl_sys.c:45-46` metrics expose `net.ssid`, `net.mac`, `net.ip` to any LAN host.

Risk: information disclosure to anyone on the LAN or guest Wi-Fi: SSID, MAC, who watches the camera and when,
brute-force feedback. Not a remote code issue; it contradicts "read-only" being a security property.

Fix: send `LOG` lines only to loopback peers or while DEBUG is on (`c->loopback || debug_active()`); keep `GET`
metrics but add a setting to hide `net.ssid`/`net.mac` unless DEBUG (or make 8091 bind to a configurable
address, default all, so the user can set loopback only once the wizard no longer needs it). Update
`tests/native/control_test.c`. Effort S. HW no (host test) then one HW check.

### G06 (P1) A second RTSP client hangs; decide go2rtc versus a second slot

Evidence: `main.c:333` `listen(server_fd, 5)` and `main.c:369` `handle_client(...)` blocks the whole accept loop
for the duration of a session. A second client's TCP connect succeeds (kernel backlog) and then receives nothing
until the first one leaves; Frigate, Home Assistant ONVIF integration, and ONVIF Device Manager all probe a
stream even when go2rtc already pulls it. `specs/03` documents the single place as a known DoS.

Cost analysis for a second slot (estimates, to be measured on HW): the VENC ring is `mmap`ed (ENC_VENC_SIZE
777600, rtsp.c:800), but `GET_STREAM` is destructive, so two consumers need one reader and a per-client queue.
A refcounted frame pool of about 12 frames (I-frame 60-90 KB at 1536 kbps cap, P about 15 KB) adds about 250 KB
resident, plus one more send thread (stack 64-128 KB); CPU for TCP interleaved send is a few percent of the
1% measured for one client; audio ring already supports multiple readers (`audio_jitter`: to verify). Risk is
engineering: `handle_client` is single-client state (`rtsp.c` globals such as `static int frame_count`,
rtsp.c:838, `prev_cap_us`).

Recommendation: **keep the single stream as the product position and document go2rtc/frigate restream as the
supported way to serve several viewers** (one connection from go2rtc, N viewers), as in the vision (light,
measured budgets). Implement the cheap part now:
1. S: while a client is being served, `poll` the listen socket inside `handle_client`'s loop and answer a second
   connection at once with `RTSP/1.0 503 Service Unavailable` plus `Retry-After`, then close. Never block, never
   parse the request beyond the first line. Removes the "hangs for hours" behaviour and the DoS amplification.
2. M (optional, P2): only if real users need two direct viewers after step 1, do the fan-out above behind
   `rtsp.max_clients` (default 1) and measure RSS/CPU delta first; reject it if it costs more than 1 MB or 5% CPU.

Effort S for step 1. HW yes (test with VLC + ffprobe at the same time).

### G07 (P1) Audio capture stops forever after 5 read errors; mic always on

Evidence:
- `audio.c:340-348`: after `AUDIO_MAX_CONSECUTIVE_FAILS` (5, audio.c:99) consecutive read errors the capture
  thread sets `g_audio_running = 0` and exits ("not retrying further"). Nothing restarts it; the stream stays
  video-only until reboot. The playback silence thread has the same shape (audio.c:161-168).
- Capture runs from boot to shutdown (`audio_start`, main.c:306) whether or not a client or the ONVIF stream
  needs it: a 32 ms read period (AUDIO_PERIOD_SAMPLES 256 at 8 kHz) is about 31 wakeups/s, plus the playback
  thread writing silence at the same rate (audio.c:150-185) when the speaker is enabled.
- No "mic enable" and no gain setting: `grep -n "mic\|gain" src/ak_rtsp/audio.c` shows no ioctl for it. The
  only audio setting is the speaker (`audio.out_enable`, audio_out.c:654).
- The microphone has never been listened to (test plan criterion 6 is unticked).

Risk: stream loses audio permanently after a driver hiccup; privacy (mic always sampled) and power (wakeups,
codec powered) for users who want video only.

Fix: (1) supervisor in `audio.c`: retry the open/config sequence with backoff (1, 2, 4... 60 s) rather than
stopping; (2) `audio.mic_enable` (default on, saved in `/etc/config/akam/audio`, applied at start, 8091 key,
`./akam audio mic on|off`); when off the SDP omits the audio track; (3) capture on demand: start the capture
thread on first PLAY, stop 30 s after the last client leaves (restart latency to be measured, the stock firmware
likely keeps it always on); (4) playback silence thread only while the backchannel is active (needs HW test to
confirm the amplifier does not pop/hiss when the stream is stopped, and `audio_out_emergency_off` semantics).

Effort M. HW yes.

### G08 (P1) No clock: no RTC, no NTP, so time-based security and logs are meaningless

Evidence:
- `docs/akam_v1_install.md:113-114` tells the user to block outbound traffic and says "NTP if you configure it",
  but `grep -rn -i "ntp\|sntp\|timezone\|TZ=" --include=*.sh --include=*.c` finds nothing configuring a time source.
  The camera boots at the epoch of its build (or 1970).
- `cred.c` skips the WS-Security `Created` window (+/- 5 min) while the clock is before 2024, per the CHANGELOG
  entry on cred: replay protection then relies on the 32-entry nonce cache alone, which is lost at every restart
  and is bypassable by a nonce flood from the LAN.
- RTCP Sender Reports carry `CLOCK_REALTIME` as the NTP timestamp (rtsp.c:1029-1030), i.e. a 1970-based time.
  Clients that use SR to align audio and video within one session are fine (same clock), but a client that
  compares with wall time, or that mixes cameras, is not.
- ONVIF `GetSystemDateAndTime` and the `Created` check of every client need a real UTC time.
- Dropbear and `ak_rtsp` link a time32 shim (src/common/time32_shim.c); monotonic time is fine, wall time is
  just wrong.
- The kernel time64 bug (docs/akam_v1_kernel_time64.md): `clock_gettime64` is acknowledged but never written;
  BusyBox `date -s`/`settimeofday` has to be tested on HW (to verify).

Fix (all opt-in, off by default, no Internet dependency without the user's choice):
1. Smallest: DHCP option 42 (NTP servers) and 100/101 are available from the router at no cost, but the stock
   `udhcpc` script is unknown. Add our own `udhcpc -s /usr/sbin/akam_udhcpc.sh` script that sets the IP exactly
   like the stock one and additionally reads `$ntpsrv`, then runs the SNTP step below. (to verify: stock script
   path and format, S/M.)
2. A tiny SNTP client in `ak_rtsp` (`sntp.c`, about 120 lines: one UDP packet to a configured server, 48 bytes,
   `clock_settime(CLOCK_REALTIME)` only if the step is large, `settimeofday` fallback), enabled by
   `/etc/config/akam/ntp` (`server=<host or IP>`, absent = disabled), tried at start, every 6 h and 60 s after the
   link comes up. Only the LAN gateway/router or a local server by default (the wizard asks, default empty).
   Accept a reply only from the queried address, port 123, with a sane stratum and an origin timestamp match
   (anti-spoof), refuse steps over a year backwards after the clock is set. Cost: one thread-less poll loop
   hooked on the existing main loop timer, so 0 added wakeups. Check BusyBox `ntpd`/`ntpd -q` exists on the
   camera (HW: `busybox --list | grep ntp`); if present a wrapper line `ntpd -q -p <server>` is cheaper than C code.
3. Without any source: keep the current behaviour, but make ONVIF refuse WS-Security UsernameToken
   when the clock is invalid (HTTP Digest only, which has no time dependence) or accept the documented weaker
   mode only if the user explicitly set `onvif.allow_unsynced_wsse=1`. Show the clock state in the 8091
   metrics (`sys.time_valid`) and in `./akam verify`.
4. Timezone: ONVIF reports UTC plus an optional POSIX TZ string; store `TZ` in the same file, only used for the
   OSD (G20) and the ONVIF `TimeZone` field. No tz database on the camera, POSIX string only.

Effort M. HW yes (clock set after `date -s`, `clock_gettime`, power cycle behaviour).

### G09 (P1) RAM logs have no cap; no process-level soak metrics

Evidence:
- `/tmp` is RAM on this camera. Logs: `/tmp/akam_ssh.log` (one line per step and, through `dropbear -E`
  stderr appended with `2>>"$LOG"`, akam_ssh.sh:432-433, one line per connection and failed login for the
  whole uptime), `/tmp/akam_lockdown.log` (only on kills), `/tmp/isp_hook.log` and
  `/tmp/ak_rtsp_ioctl_dump.log` (debug builds only: isp_hook.c:3, venc.c:343, guarded by `DEBUG_IOCTL_DUMP`,
  off by default; confirm the shipped `ak_rtsp` is the plain build).
- A LAN host hammering port 22 grows `/tmp/akam_ssh.log` without bound (about 100 bytes per attempt; 1 attempt/s
  is 8.6 MB/day).
- No `proc.rss_kb`, `proc.fds`, `proc.threads` in the 8091 metrics (`ctl_sys.c:40-48` lists the keys), so
  neither a leak nor an fd leak can be measured over days without SSH and a shell loop. The code frees UDP
  sockets (rtsp.c:197-205) and the client fd (rtsp.c:1524), the `static uint8_t expo[0x720]` and
  `malloc(DUMP_BUF_SIZE)` paths in `isp_blocks.c:181` are tool-only, but nothing proves it.

Fix: (1) a size guard in `akam_ssh.sh`: pass Dropbear stderr through a small `while read` loop that rewrites the
log when it exceeds 32 KiB, or simply use `-E` only while `ctl.debug` is on (no `-E` = stderr to `/dev/null`);
(2) add `proc.rss_kb`, `proc.fds`, `proc.threads`, `proc.log_drops` to `ctl_sys.c` (reads `/proc/self/status` and
`/proc/self/fd` once per second at most, same cache as the others); (3) add a `--soak <hours>` mode to
`akam_hwcheck.sh` that samples these every 5 minutes into a file on the SD card (not RAM) and prints a slope;
(4) 72 h soak on HW with one client connected, one cycling (connect/disconnect every minute) and the router
rebooted once, accept if RSS slope < 4 KB/h and fds constant.

Effort S. HW yes (soak).

### G10 (P1) Idle wakeup budget, to be measured then cut

Static count of timers that fire with no client connected (estimate, from the code; HW: `grep ctxt /proc/<pid>/status`
or `cat /proc/<pid>/task/*/stat` deltas to confirm):

| Source | Evidence | Wakeups/s | Notes |
|---|---|---|---|
| Main drain loop | `main.c:204,347-358` `poll(..., 20)` then `drain_venc_ring` | 50 | ring wraps in about 3 s; 100 ms is enough |
| AE tick | `ae.c:42,275` 66 ms, two ioctls per tick | 15 (+30 ioctls) | required to advance the hardware AE (ae.c:262-264) |
| Night monitor | `night.c:45,234` 100 ms, `NL_POLL_MS` in night_logic.h | 10 | state machine counts polls, not time |
| Audio capture | `audio.c:334-363` blocking `read`, 256 samples at 8 kHz | about 31 | driver period, not our timer |
| Silent playback | `audio.c:150-185` 32 ms writes of zeros | about 31 | only useful for the speaker |
| LEDs | `leds.c:292`, `LEDS_TICK_MS 500` (x4 when not auto) | 0.5 to 2 | fine |
| WS-Discovery | `wsd.c:25` 30 s | 0.03 | fine, only when ONVIF is on |
| `http_srv` | `http_srv.c:530` infinite poll when idle | 0 | fine |
| `ctl_sys` | on demand, 1 s cache | 0 idle | fine |
| `akam_lockdown.sh` | forever loop, `ps` every 60 s (akam_lockdown.sh: SLOW_PERIOD 60) | 0.02 but forks `ps` | stops being useful once the boot window is over |

Total about 140 wakeups/s, about 70 of them avoidable. Fixes in order of gain/risk:
1. Main drain poll from 20 ms to 100 ms (also needed: confirm the ring never wraps at the 1536 kbps cap, i.e.
   777600 bytes / 192 KB/s = 4 s): saves about 40 wakeups/s. S. HW yes (check the first-frame latency and I-frame
   alignment for a new client, main.c:339-346).
2. Silent playback only while a talk session is active (G07): saves about 31/s. M. HW yes.
3. Audio capture on demand (G07): saves 31/s with no client. M. HW yes.
4. Make the night monitor consume AE samples through a condition variable fed by `ae_loop` (`ae_stats_seq()`
   already exists) instead of its own 100 ms timer: saves 10/s and removes a drift between the two. M, touches
   the state machine timing (counts in polls), so only if profiling shows it matters. HW yes.
5. `akam_lockdown.sh`: end the slow phase after, say, 30 minutes of uptime (the stock launchers run at boot only;
   the flag `SLOW_ROUNDS` already exists). Keep it if the 24 h check shows telnet never reappears. S. HW yes.

Measurements missing from `docs/akam_v1_test_plan.md` section 4 (all "to measure"): idle CPU % (no client), CPU
with one client at 1080p, MemFree/RSS idle and with client, current draw at 5 V with IR off/on (USB power meter),
wakeups/s. Add them to `akam_hwcheck.sh --budget`, with a pass/fail threshold per budget (for example idle CPU
under 3%, one client under 5%, RSS under 6 MB, idle wakeups under 80/s).

Power: the IR LEDs are a single GPIO (`ircut.c:20` GPIO 70), so there is no duty-cycle control; the only
levers are not lighting them when not needed (`led.ir=off` exists) and the day/night probe (an IR-off
window every few minutes in night mode, see night_logic.h) which is already back-off controlled. No work
here except measuring (HW).

### G11 (P1) Video and audio are not configurable

Evidence: constants only. `ak_rtsp.h:28-31` `ENC_WIDTH 1920`, `ENC_HEIGHT 1080`, `ENC_FPS 15`, `ENC_GOP 30`;
`venc.c:233-236` rate control fixed (VBR+ minqp 28, maxqp 43, max 1536 kbps, initqp 99) with the 8091 keys
`venc.minqp`/`venc.maxqp` explicitly read-only (control.c:269-270, 417). The saved settings are only: picture
(brightness/contrast/saturation/sharpness), night mode, LEDs, speaker, RTSP auth, hostname.

What a user expects and cannot do today, ranked:
1. **Bitrate cap and quality** (Wi-Fi with weak signal or metered link; also needed for the "light" budget):
   `video.max_kbps` (500 to 3000), applied at start from `/etc/config/akam/video`. Needs an encoder restart, so
   it is a restart-on-change setting, with a wizard confirmation. Effort M. HW yes.
2. **Substream** (the sensor and ISP already produce a sub channel, `vi.c` `g_vid_sub`, 640x360 NV12 grab):
   an H.264 640x360 second encoder would let HA/Frigate detect on the substream and cut CPU on the host.
   Not wired into VENC (single channel `venc-chn0`). Effort L. HW yes. Needs a decision: not before V1.
3. **Frame rate** (10/15 fps; 15 fps is the sensor rate set at ae.c:231): lower fps saves bandwidth and CPU;
   touches the vclock logic (rtp_clock.c) and the AE tick. Effort M. HW yes.
4. **Mirror/flip** (ceiling mount): sensor/ISP flip registers, not found in the code (`grep -in "flip\|mirror"
   src/ak_rtsp/*.c` is empty); needs a register hunt in the sensor driver, with a sensor restart. Effort M. HW yes.
5. **Mic enable/gain**: see G07. 6. **Resolution** 1080p only; a 720p mode is a re-init of ISP crop and the
   VENC pool; keep out of V1. 7. **OSD timestamp, motion detection**: G20.
8. A single readable view: `./akam config show` that prints every saved setting (video/audio/picture/night/led/
   auth state/hostname/ntp/onvif) from one 8091 `CONFIG` command; today each command shows its own slice.

Effort: items 1 and 8 are S to M; the rest M or L. Persistence follows the existing rule: write flash only on a
loopback SET and only if the value changed (night.c:99-107, `cfg_write_atomic`).

### G12 (P1) Release readiness

Evidence:
- `git tag` is empty; `main` is still the initial commit `723a0b3` plus one fix (`3250e0f`); all work lives on
  `dev` and the `feature/*` branches. CHANGELOG has only "Unreleased".
- `tools/release/allowed_signers` contains comments only, no key (so `verify_kit.sh` cannot succeed without
  `--allow-unsigned`); the README quotes no public key; supported hardware lists one `validated` record.
- `tools/release/README.md`: "There is no CI". Tests are about 94 shell/C test files run by hand; no recorded run.
- Versioning: `make_kit.sh` defaults to `git describe` (fails with no tag); `gen_version.sh` increments
  `build_number.txt` at every build, which makes the working tree dirty and the binary non-reproducible (documented).
- `build_number.txt` and `version_info.h` are git-ignored (.gitignore:52-53), so builds do not dirty the tree.
- Release key custody: dedicated Ed25519 key with passphrase is described, not created.

Fix: (1) generate the release key, commit its public half to `allowed_signers` and the README (user action);
(2) merge `dev` to `main` when the P0 list passes, tag `v1.0.0-rc1`, build the kit, run `verify_kit.sh`, flash a
second camera from the kit (not from a source build); (3) add a `tools/ci/run_all_tests.sh` (a `tools/ci` dir
exists) that runs every `tests/*_test.sh` plus the `native` ones and prints a summary, run before each merge;
(4) a `CHANGELOG` section per release and a `RELEASE_CHECKLIST` in `tools/release/README.md` listing the HW gates
of section 6; (5) `docs/` link check is in place (`readme_links_test.sh`), add a version string check (the
wizard, `ak_rtsp` banner and kit manifest agree).

Effort S (excluding user key work). HW no (flash of the kit on a second camera is yes).

### G13 (P1) Hardware test debt

See section 6 for the full list; the blocking ones for V1 are in G01 to G04 and G08 plus: Telnet after Wi-Fi
reconnect (criterion 12), restore rehearsal (criterion 17), microphone audible (criterion 6), read-only 8091
checks after the read-only change, LEDs, `audio`/`picture`, ONVIF backchannel (`akam talk`).

### G14 (P1) Port and process audit covers TCP only

Evidence: `tools/hw_tests/akam_hwcheck.sh` checks TCP listeners (22/554/8091 expected; 21/23/24/80 closed) and
a process list (`anyka_ipc`, `telnetd`, `tcpsvd`, `ftpd` absent); `grep -n -i udp akam_hwcheck.sh` finds no UDP
check. The stock APP is only de-Tuyafied by removing `anyka_ipc` (build_firmware.sh:462-464); `cmdserver` and
`msgserver` are kept deliberately (docs/building_your_own_firmware.md:25, local IPC only). Other stock helper
daemons that `service.sh`, `main.sh`, `ethernet.sh` or the `_ht_*.ini` files start have not been enumerated in
the repository.

Risk: a stock UDP listener (mDNS, SSDP, a cloud tunnel, a broadcast discovery responder used by the vendor app)
would be an open port nobody audited; today's "only 22, 554, 8091 are listening" is TCP only.

Fix: extend hwcheck to read `/proc/net/udp` and `/proc/net/tcp6`/`udp6`, expect only 3702 (ONVIF on) and the
DHCP client; add an "unexpected process" report from `ps` after boot (everything not in an allow-list: init,
watchdog, `ak_rtsp_wrapper.sh`, `ak_rtsp`, `cmdserver`, `msgserver`, `dropbear`, `wpa_supplicant`, `udhcpc`,
`akam_*`, kernel threads) and record the list in the docs (`docs/akam_v1_test_plan.md`). Then decide per item.
Effort S. HW yes.

---

## 4. Security review (verdicts, evidence)

| Area | Verdict | Evidence / notes |
|---|---|---|
| Telnet 23/24, FTP 21 | Closed in normal boot (HW 2026-11-08); layer 1 `patch_service_sh.sh` comments the launch lines, layer 2 `akam_lockdown.sh` kills them every 5 s for 2 min then every 60 s forever | still open: the Wi-Fi reconnect case (test plan 12); exposure window at boot is about seconds, not measured (`--boot-window` exists) |
| Dropbear | OK: key-only, ed25519 only, no password, no forwarding, no SFTP (tools/dropbear/localoptions.h), `-T 3` (max 3 auth tries per connection) | no per-IP rate limit and no `-j/-k` beyond compiled-out forwarding; only root; logs unbounded (G09); host key in CONFIG (0600) |
| Root password | None on the camera: not checked in the repo whether `/etc/passwd`/`shadow` of the stock image has a root hash. Dropbear password auth is compiled out so it is moot over SSH; the UART console (`ttySAK0`) remains physical access | HW: `cat /etc/shadow` once and note it; physical access is out of scope for V1 |
| SD card autorun | Guarded: service.sh SD hooks (`_ht_ap_mode.conf`, `hack.sh`, `/mnt/telnet.sh`, `tf_update.sh`) run only with `/mnt/AKAM_MAINTENANCE` (patch_service_sh.sh, lines 15-30) | the flag is a file on a removable card: physical access = full control; the legacy `/mnt/CAMYN_MAINTENANCE` alias is still accepted (G18) |
| `tf_update` / firmware update | Not reachable without the maintenance flag. No signature or hash check exists on `*.bin` updates by the stock `system_upgrade`; there is no network OTA | with the flag, any `*.bin` on a card flashes the camera. Accept as physical-access risk, document it (G15). `flash_tool` writes only after the wizard's partition checks |
| Network services | 22, 554, 8091 (TCP); ONVIF later (80/8080 + UDP 3702) | RTSP is open when no `rtsp_auth` file exists: by design (opt-in). 8091 leaks (G05). UDP not audited (G14) |
| RTSP Digest | Constant-time compare, per-connection nonce, failure limiter shared with ONVIF | MD5 and no `qop` (compatibility decision); the HA1 file is as sensitive as a password; fail closed when the file is broken |
| Secrets in logs | Auth log never prints the user, header, HA1 or password (specs/03 section 3); URI credentials masked | `LOG` stream reaches remote peers (G05); the SSH log has no key material (akam_ssh.sh header); metrics expose SSID/MAC/IP |
| `/etc/config` permissions | `credentials` and `rtsp_auth`: 0600, dir 0700, atomic writes (cfg_file.c:20, akam_rtsp_auth.sh:93-99), `cred.c:176,196` refuses a file readable by others and opens with `O_NOFOLLOW` | the ONVIF password is stored in clear text on flash by decision U2 and ends up in any CONFIG backup (G19) |
| 8091 writes | Loopback only or DEBUG (RAM only, timed, default off); loopback = peers on 127.0.0.0/8 only | in DEBUG, any LAN host can change picture and tuning; limit 3 remote clients plus 1 loopback |
| Memory safety | C daemon, static musl, non-PIE, no canaries (G17); SOAP/HTTP parsers are bounded (CHANGELOG: reject NUL, bounded head) | parsing is the main attack surface once ONVIF is on: fuzz `soap.c`/`http_srv.c` with AFL++ or libFuzzer on the PC before enabling ONVIF in a release |
| Update path to the camera | `./akam update` over SSH (key-only) | the kit is signed (ssh-keygen -Y), but `allowed_signers` is empty (G12); the camera itself verifies nothing about a flashed image |

No live exploitable issue was found by reading. The two things closest to one: the unauthenticated 8091 `LOG`
stream (G05) and an RTSP stream that is open until the user opts into the password; the first is a one-line
fix, the second is a product decision (wizard default stays "propose", but `./akam verify` should print a visible
"stream is not password protected" line).

---

## 5. P2 items

### G15 (P2) SD firmware update has no signature; document it
`patch_service_sh.sh` guards `tf_update.sh` behind the maintenance flag, so by default no card can reflash the
camera. When the flag is present any `*.bin` flashes. Fix: document it in `docs/akam_v1_ssh_and_lockdown.md` as
physical-access only (done partly at line 259), and let the wizard remove the flag file from the card after a flash.
Do not build in-camera signature verification for V1 (no crypto library, and the vendor updater is not ours).

### G16 (P2) Tuning inputs are unvalidated
Evidence: `control.c:343-410` `atoi`/`strtoul` for `ae.stable_range`, `ae.hold_range`, `ae.speed`, `ae.exp_max`,
`night.trigger_hw_exp`, `night.day_hw_exp`, `night.confirm_samples`, `night.lock_ms` with no range check, applied
straight to the ISP or the state machine; `ae.c:96-99` documents the tearing as accepted. Only loopback or
DEBUG can write, so this is stability, not security: `night.confirm_samples=0` or a negative `lock_ms` can make
the state machine flap or never switch. Fix: clamp each key with the ranges used by the stock firmware, reply
`ERR ... range`, add the keys to `tests/native/control_test.c`. Effort S. HW no.

### G17 (P2) Binary hardening
Evidence: `Makefile:3-11` `-O2 -Wall -g`, `-static`; no `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2` (musl
ignores it), no `-Wl,-z,relro,-z,now`, no PIE (static non-PIE has no ASLR). Fix: `-fstack-protector-strong`
(check the Zig/musl toolchain supports it for ARMv5, cost a few KB and under 1% CPU), `-Wl,-z,relro,-z,now`,
`-Wformat -Wformat-security`; try `-static-pie` and measure the RSS/CPU delta, keep it only if under budget.
Run `cppcheck`/`clang --analyze` over `soap.c`, `http_srv.c`, `rtsp.c` in the test script. Effort S. HW yes
(boot test).

### G18 (P2) Legacy cruft to remove before V1 (nothing here breaks anything today)
- `camyn/` SD folder and `CAMYN_MAINTENANCE` aliases in `akam_ssh.sh`, `akam_lockdown.sh`, `patch_service_sh.sh`
  (all marked "removed in the next release"); remove at V1 and delete `tests/upgrade_from_camyn_test.sh` after a
  last look. S.
- `night.*` keys read with `atoi` in `control.c` (G16) and the old `trigger_hw_exp` rule (hw_exp > 3000) kept
  next to the new saturation logic (night_logic.h comments): drop what the state machine no longer uses or mark
  it diagnostic. S.
- `--no-isp-init` (main.c:249-261) documented as non-working: delete. `--grab-then-exit`, SIGUSR1 grab to
  `/mnt/grab_NNNN.yuv` (main.c:36-46,342-346): dev tools in the production daemon, keep behind a compile flag. S.
- Two AE implementations, `ae.c` and `aec_custom.c` (`AEC_CUSTOM`, Makefile:114,125), plus Makefile variants
  `debug-dump`, `isp_hook`, `stock_hook`, `patch_div0`, `isp_dump`, `flash_tool`: make sure the kit contains only
  `ak_rtsp` (plain) and `flash_tool`; the repo can keep them under `tools/hardware_debug` or a `dev/` directory.
  Verify `make_kit.sh` strips `-g` copies (it does for the binary; check `dropbearmulti`). S.
- `main.c:177-184` header comment lists steps twice and says "serve one RTSP client at a time"; refresh. S.
- `tools/hardware_debug/serial_console_dotnet` (a .NET app with its own CLAUDE.md) is not part of V1 and
  duplicates the 8091 tuning role; decide to ship, move to a separate repository, or label experimental. S.
- APP image: only `anyka_ipc` removed. Candidate savings in the rootfs/APP (unused vendor binaries and libs, Tuya
  libraries, test tools) are unknown: a 2.7 MB free APP is fine today, but a list of what the stock APP still
  contains and what runs (G14) is needed to claim "no vendor daemon". M. HW yes.

### G19 (P2) Backups contain secrets
`./akam backup` stores all 8 partitions in `~/akam-backup-*/`; CONFIG holds the Wi-Fi PSK
(`/etc/config/wpa_supplicant.conf`), `credentials` (clear text ONVIF password) and `rtsp_auth` (HA1). Fix: say it
in the backup guide and the wizard final message (mode 0700 on the backup directory, do not upload), and make
the wizard create the folder with `umask 077`. S.

### G20 (P2) OSD timestamp, motion detection, timezone
- OSD: the stock firmware overlays text through a vendor OSD module (kernel/VENC); no code in `ak_rtsp` uses it.
  Needs a reverse-engineering session with `stock_hook` captures (the repo has `stock_capture.md` for this); cost
  is a font plus an overlay buffer in RAM (tens of KB) and a once-per-second update thread (1 wakeup/s). Depends
  on G08 (a clock). L. HW yes.
- Motion: cheapest path is frame differencing on the sub channel luma (`g_vid_sub`, 640x360 NV12, already
  mmapped: 230 KB per frame; sampling 2 fps costs a few percent CPU on an ARM926), or I-frame size /
  P-frame-size spikes from the encoder at zero extra cost (a coarse detector, no extra frame access). Output is
  an ONVIF event (needs the Events service, not planned for V1) and an optional MQTT topic. Off by default.
  L. HW yes. Not V1.
- Timezone: only POSIX `TZ` in `/etc/config/akam/ntp` (G08); needed by OSD and ONVIF, nothing else.
- `./akam config show`: see G11 item 8.

### G21 (P2) MQTT, optional
`specs/07` step 2 describes it; the user has no broker and no Home Assistant, so it cannot be tested end to end
here. Keep it as an optional, off-by-default module (`/etc/config/akam/mqtt`: host, port, topic prefix, user, no
TLS in the first cut): publish a retained `availability` and a metrics JSON every 30 s (the same keys as 8091
`GET`), and subscribe to `<prefix>/set/led|night|audio` (loopback-equivalent writes). Needs a minimal MQTT 3.1.1
client (CONNECT/PUBLISH/SUBSCRIBE/PING, about 400 lines, no QoS 1 state) and a local `mosquitto` on the PC for host
tests; a Wi-Fi reconnect loop that costs nothing when disabled. Budget: when disabled zero threads and zero
wakeups; when enabled under 50 KB RAM and under 1 wakeup/s. Do not start it before G01 to G04 are done. L. HW
only for the final check (with a broker started on the PC).

### G22 (P2) Wi-Fi power save and link tuning
Nothing sets power save: `grep -n -i "power_save\|iw " tools/firmware_patch_templates/*.sh` is empty. The camera streams
continuously to a viewer, so power save gains little while a client is connected, but with no client (the idle
budget) the link can sleep: test `iw dev wlan0 set power_save on` (BusyBox/`iw` may not exist; the driver
`atbm6x3x` is vendor and may ignore it) and measure current and ping latency; if latency rises past 100 ms it
breaks the "fast reconnect" of RTSP/ONVIF probes. Also record the RSSI/rate in the budget (8091 `net.wifi_rssi`
already exists). S to measure. HW yes.

---

## 6. Hardware test debt (what has never been seen on a camera)

From `docs/akam_v1_test_plan.md` section 1b and the CHANGELOG "Not yet checked on hardware" entries.

Open items in the test plan:
- Criterion 6: microphone audible (the L16 track exists, nobody listened).
- Criterion 12: Telnet/FTP stay closed after a Wi-Fi reconnect (router restart).
- Criterion 14: no Internet dependency (camera on a segment with no route out).
- Criterion 17: `./akam restore` rehearsed.
- Day/night flapping over hours across dusk and dawn (the state machine was redesigned and checked once with
  a covered lens, CHANGELOG: "Checked on hardware (akam-01)", but not over a day/night cycle).
- Criterion 8 read-only checks of 8091 after the read-only change (SET refused from the PC, loopback SET works,
  `debug on/off` expiry), flagged "Not yet checked on hardware" in the CHANGELOG.

New features marked "Not yet checked on hardware" in the CHANGELOG (flash and test one by one, in this order):
1. Front LED and IR LED settings (`/etc/config/akam/leds`, 8091 `led.*`).
2. `./akam audio` and `./akam picture` (volume, beep, talk, saved picture settings, sharpness).
3. Two-way audio over the ONVIF RTSP backchannel (`./akam talk`, `./akam say`; also with `rtsp-auth` enabled and
   with the speaker amplifier on a power cycle).
4. Read-only 8091 and the `debug` command through the wizard.
5. RTSP Digest auth with VLC, ffmpeg, go2rtc and Frigate (the clients named in specs/03), the failure limiter,
   a config change cutting a running stream, a broken file failing closed.
6. Hostname via DHCP (seen once: `akam-01`), then via `./akam hostname`.
7. `cred.c` credentials file: `./akam rtsp-auth set` writes both files, `cred` accepts them, permission checks.
8. Anything ONVIF (not listening yet).

Tests that do not exist at all and are required by this audit:
- Kill and hang tests of `ak_rtsp` (G01), respawn storm (G04).
- Router off for 2, 10, 40 minutes; DHCP renewal; wrong Wi-Fi password then corrected (G03).
- 72 h soak: RSS, fds, threads, `/tmp` size, CPU, with a viewer reconnecting every minute (G09).
- stdout stall (G02): block the UART or the pipe, check that video continues.
- Two simultaneous RTSP clients (G06) and a slow-loris client.
- UDP and process allow-list audit (G14).
- Clock set, power-cut behaviour, `Created` window (G08).
- Idle CPU, idle wakeups, power draw (G10, G22); the measures table in the test plan (section 4) is empty.
- Fuzz of the SOAP/HTTP parsers (security table above).
- A second camera on the kit-built image (kit_grade release), then `./akam update` from the previous kit.

---

## 7. Proposed order of work

1. Measure first (no code): G02 step 1 (where stdout goes), G14 (UDP and process audit), the budget table of G10,
   `shadow`, and `busybox --list` for ntpd/iw. One camera session, about 2 hours, results into the test plan.
2. Quick wins on the host (S): G05, G06 step 1, G02 steps 2-4, G09 (1)-(3), G16, G04 wrapper backoff.
3. Stability core (M): G01 watchdog thread + kill tests, G03 `akam_net.sh`, G07 audio supervisor.
4. Time (M): G08 (SNTP opt-in, DHCP option 42 if the stock script allows).
5. Consumption (M): G10 steps 1-3 and 5, then re-measure; stop when the budget is met.
6. Configuration: G11 item 1 (bitrate) and item 8 (`config show`), mic enable from G07.
7. Release: G12, G13 soak, tag, kit signed, second camera from the kit.
8. After V1: G11 substream/mirror, G20, MQTT (G21).

Out of scope (decided elsewhere): ONVIF services in progress, the image grain work (`specs/08`).
