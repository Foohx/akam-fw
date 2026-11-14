# Architecture

Akam replaces only the application layer (the APP partition) of the stock firmware. The stock bootloader, kernel and
root file system keep booting the camera; Akam changes what they start. This file covers the hardware, the flash map,
the boot chain, the processes, the persistent configuration and the cross-cutting limits (RAM, watchdogs, logs, time).

Status: **hardware-validated** on two cameras of model `3215672.3` (see [roadmap.md](roadmap.md) for what was never
run). Sections that are host-tested only say so.

## 1. Hardware

| Item | Value |
|---|---|
| Camera | LSC Smart Indoor 1080p, article `3215672.3`, board SI `D26228` (the only validated model) |
| SoC | Anyka AK3918AV130, ARM926EJ-S (ARMv5TE, single core, no FPU, no NEON) |
| RAM | 64 MiB, about 40 MiB usable by user space |
| Flash | 8 MiB SPI NOR (`XM25QH64D` or a Winbond equivalent), 8 MTD partitions |
| Image sensor | GalaxyCore GC20C3, 2 MP, I2C; device nodes `/dev/isp-param-0`, `/dev/v4l-subdev0`, `/dev/video-0-0`, `-0-1`, `-0-2` |
| Video encoder | Vendor VENC (`/dev/venc`, `/dev/venc-chn0`), H.264 |
| Audio | Capture `/dev/pcmC0D0c`, playback `/dev/pcmC0D0p` (vendor `ak_pcm` driver), speaker amplifier switched through the driver |
| Wi-Fi | Altobeam ATBM6012BX / `atbm6x3x` over USB, interface `wlan0`, `wpa_supplicant` with `-Dnl80211` |
| Front LED | Kernel LED class device `/sys/class/leds/state_led` (blue, on/off only) |
| IR LEDs | GPIO 70, active high |
| IR-cut filter | GPIO 65 and 66 (solenoid, pulse driven) |
| Kernel | Linux 4.4.302-cip94 (vendor build), BusyBox 1.30.1, no RTC |
| Toolchain | Zig 0.14.1 (`zig cc`, target `arm-linux-musleabi`, `-mcpu=arm926ej_s`), static musl |

The camera has no real-time clock: it boots with a date in the past (see section 8).

## 2. Flash map

| MTD | Name | Offset | Size (bytes) | Type | Akam |
|---|---|---|---|---|---|
| mtd0 | UBOOT | `0x000000` | 204800 | raw | never written |
| mtd1 | ENV | `0x032000` | 4096 | raw | never written |
| mtd2 | ENVBK | `0x033000` | 4096 | raw | never written |
| mtd3 | DTB | `0x034000` | 65536 | raw | never written |
| mtd4 | KERNEL | `0x044000` | 1572864 | uImage | never written |
| mtd5 | ROOTFS | `0x1c4000` | 1032192 | squashfs (xz) | never written |
| mtd6 | CONFIG | `0x2c0000` | 262144 | JFFS2, read-write, on `/etc/config` | written at run time by Akam (settings, keys, secrets) |
| mtd7 | APP | `0x300000` | 5242880 | squashfs (xz, 128 KiB blocks), on `/usr` | **replaced by the Akam image** |

- The image written to mtd7 MUST be exactly 5,242,880 bytes (squashfs padded with `0x00`). The installer refuses any
  other size.
- The installer compares the whole 8-line `/proc/mtd` table with the sizes above and refuses a different layout.
- The stock APP is squashfs with xz, 128 KiB blocks and no BCJ filter. The Akam image uses the same compressor as the
  stock APP it was built from (xz by default; gzip is selectable). The squashfs content is about 2.6 MB, so roughly
  half of the partition is free.
- CONFIG is small (256 KiB). It holds settings and secrets only, never logs and never large files. Every writer uses
  an atomic replace (temporary file, `fsync`, `rename`) and skips the write when the content did not change, to limit
  JFFS2 wear.

## 3. What Akam replaces in the APP partition

| Stock content | Akam |
|---|---|
| `anyka_ipc` (Tuya application: cloud, video, audio, IR, LED) | Removed. Replaced by `ak_rtsp` |
| `_ht_process.ini` with three process entries (`anyka_ipc`, `rtspserver`, `nvtservice`) | One entry that starts `ak_rtsp_wrapper.sh` |
| `sbin/service.sh` (starts Telnet, FTP, SD hooks) | Patched at build time: Telnet/FTP launch lines disabled, SD hooks guarded by a maintenance flag |
| `sbin/ethernet.sh` | Patched: the DHCP host name comes from `akam_hostname.sh show` |
| `sbin/drop_caches.sh` (`sleep 5`) | `sleep 60` (fewer wakeups) |
| `sbin/main.sh` (`klogd -n &`) | A loop that runs `ak_rtsp klog` (filtered kernel log), restarts it after a crash (up to 5 in a row; a run over 60 s resets the count; a plain `kill` is not counted) and falls back to stock `klogd` |
| `cmdserver`, `msgserver` | Kept. They are local IPC started from `service.sh`; removing them stalls the boot |

Files added to the image (the single list is `tools/release/image_files.txt`):

| Image path | Mode | Source |
|---|---|---|
| `bin/ak_rtsp` | 755 | `src/ak_rtsp` (stripped) |
| `bin/dropbearmulti`, links `bin/dropbear`, `bin/dropbearkey` | 755 | `tools/dropbear` (Dropbear 2026.94) |
| `sbin/ak_rtsp_wrapper.sh` | 755 | `tools/firmware_patch_templates/` |
| `sbin/akam_ssh.sh`, `akam_lockdown.sh`, `akam_net.sh`, `akam_ntp.sh`, `akam_hostname.sh` | 755 | same |
| `sbin/akam_rtsp_auth.sh`, `akam_debug.sh`, `akam_onvif.sh`, `akam_mqtt.sh` | 755 | same |
| `local/_ht_process.ini` | 644 | same |
| `local/akam/LICENSE.*`, `MANIFEST.txt` | 644 | licence texts and build record |

No key of any kind is put in the image. SSH keys come from the microSD card or from a later change over SSH
([network-time-security.md](network-time-security.md)).

## 4. Boot chain and service start

1. U-Boot loads the kernel and DTB; the kernel mounts ROOTFS (read-only squashfs) and runs stock `init`.
2. Stock scripts mount APP on `/usr` and CONFIG on `/etc/config`, run `sensor_driver.sh` (loads `ak_isp`,
   `sensor_gc20c3`, `ak_venc_adapter`, `ak_venc_bridge`, `ak_pcm`; copies the sensor ISP file to
   `/tmp/sensor_isp.conf`), `main.sh` and `service.sh` (patched).
3. The stock `/usr/bin/watchdog` reads `/usr/local/_ht_process.ini`, starts the single entry, and feeds
   `/dev/watchdog`. The entry is:

```ini
[process_0]
name = ak_rtsp_wrapper.sh
param = ""
delay = 0
onvif = 0
```

4. `ak_rtsp_wrapper.sh` (a direct child of the watchdog) runs in this order:
   1. Respawn backoff (section 6).
   2. `akam_hostname.sh apply` (kernel host name), then reads the effective name for DHCP (`akam` when the helper
      fails).
   3. Background helpers, each detached with output discarded and its exit status ignored: `akam_ssh.sh start`,
      `akam_lockdown.sh`, `akam_net.sh start`, and `akam_ntp.sh run` only when `/etc/config/akam/ntp` exists.
      A helper can never delay or break `ak_rtsp`; each has an anti-duplicate guard (lock directory in `/tmp`).
   4. Wi-Fi bring-up when `/etc/config/wpa_supplicant.conf` exists and no `wpa_supplicant` runs:
      `wpa_supplicant -Dnl80211 -i wlan0 -c /etc/config/wpa_supplicant.conf -B`, then
      `udhcpc -i wlan0 -x hostname:<name> &`, then `touch /tmp/connect_type_wifi`. Plain DHCP only.
      The wrapper does this itself because stock `ethernet.sh` skips Wi-Fi when `/tmp/_ak39_factory.ini` exists.
   5. Runs `/usr/bin/ak_rtsp` in the foreground and records its exit.
5. `ak_rtsp` start order: stdout tee and control port, status LED, software watchdog arm, then ISP/VI/VENC
   initialisation, AE thread, night thread, audio capture, speaker, ONVIF and MQTT services (if enabled), and the RTSP
   accept loop on port 554. Failure of audio, ONVIF or MQTT MUST NOT stop video.

## 5. Processes in a normal boot

| Process | Role |
|---|---|
| `init`, `syslogd`, kernel threads | Stock |
| `ak_rtsp klog` | Filtered kernel log forwarder (replaces `klogd`) |
| `watchdog` (`/usr/bin/watchdog`) | Stock. Feeds the hardware watchdog and respawns the wrapper. Akam never touches it |
| `ak_rtsp_wrapper.sh` / `ak_rtsp` | Video, audio, control, optional ONVIF and MQTT |
| `dropbear` | SSH on port 22 |
| `wpa_supplicant`, `udhcpc` | Wi-Fi and DHCP |
| `akam_lockdown.sh`, `akam_net.sh`, optional `akam_ntp.sh` | Background helpers |
| `cmdserver`, `msgserver` | Stock local IPC (no network listener) |
| `drop_caches.sh` | Stock loop, period 60 s |

`ak_rtsp` threads: main (RTSP accept loop and client session), AE, night, control accept loop (plus up to 4 short
client threads, 64 KiB stack each), stdout log pump, audio capture, audio playback, status LED, software watchdog,
and, only when enabled, ONVIF HTTP, WS-Discovery and MQTT (one thread each, asleep until traffic arrives).

`ak_rtsp` is also a multi-call binary: `ak_rtsp klog` runs the kernel log filter, `ak_rtsp ctl <line>` sends one line
to the control port on `127.0.0.1:8091` and prints the reply (used by the helper scripts, so they do not depend on the
BusyBox `nc`).

## 6. Respawn backoff and exit codes

The stock watchdog restarts the wrapper at once (`delay = 0`). The wrapper therefore keeps a crash counter in RAM
(`/tmp/akam_respawn`, one line: `count=N total=N last_rc=N last_run=S up=S`):

| Consecutive short runs (`count`) | Wait before the next start |
|---|---|
| 0, 1 | 0 s (a one-off crash recovers at once) |
| 2 | 2 s |
| 3 | 5 s |
| 4 | 10 s |
| 5 or more | 30 s |

- A run of 10 minutes or more is stable and clears `count`.
- Exit codes 143 (SIGTERM) and 130 (SIGINT) mean "stopped on request" and are not counted (the installer restarts the
  video service with `kill`).
- Exit code 70 means the software stall watchdog fired (section 7).
- One line per exit goes to `/tmp/akam_wrapper.log` (trimmed to 50 lines when it passes 100) and to syslog when
  `logger` exists.

Status: host-tested only (`tests/wrapper_backoff_test.sh`).

## 7. Watchdogs

| Watchdog | Behaviour |
|---|---|
| Hardware | Stock `watchdog` feeds `/dev/watchdog`. Unchanged |
| Software stall (`swdog.c`) | A thread checks every 1 s. `ak_rtsp` exits with code 70 (the wrapper then restarts it) when: no video frame was fetched from the encoder for 30 s while one is expected (suspended while a client write is blocked or no session issued PLAY); a heartbeat thread (main loop, AE, control accept) was silent for 60 s; the init sequence took longer than 120 s. The first frame gets 60 s of extra grace after arming |
| Network | `akam_net.sh` (see [network-time-security.md](network-time-security.md)) |
| Telnet/FTP | `akam_lockdown.sh` |

A process stopped by SIGSTOP or stuck in an uninterruptible kernel wait cannot run the software watchdog; that case is
left to the hardware watchdog.

Status: stall exit is in the flashed image; no real stall was ever seen. Host-tested (`tests/swdog_test.sh`).

## 8. Time

- No RTC: the clock starts in the past (1970 or 2000) until it is set. `sys.time_set` (control port) is 1 once the
  wall clock is 2024-01-01 or later.
- Opt-in NTP (`akam_ntp.sh`) or an ONVIF `SetSystemDateAndTime` sets the clock. Both only step the clock; the video
  stream timestamps use the monotonic clock.
- **time32 shim.** The vendor kernel has no time64 system calls but answers some of them with success without writing
  the result. musl 1.2 calls `clock_gettime64` (syscall 403) first and never falls back, so monotonic timeouts were
  garbage. `src/common/time32_shim.c` redefines `clock_gettime`, `__clock_gettime`, `timerfd_settime/gettime` and
  `timerfd_create` with the 32-bit syscalls (263 for `clock_gettime`). It is linked into every binary Akam ships.
  `tools/build/check_time32.sh` disassembles the `.text` section and fails the build if any of syscalls
  403, 405, 408, 409, 410, 411 or 417 is issued. Dropbear carries a patch that takes monotonic time from kernel ticks.
- ONVIF WS-Security freshness (5 minutes) is only enforced when the wall clock is 2024 or later.

## 9. Persistent configuration

All files are under `/etc/config/akam/` (directory mode 0700) unless noted. Writers use atomic replace and
mode 0600 unless noted. A missing file means the compiled-in default.

| File | Written by | Format | Meaning |
|---|---|---|---|
| `rtsp_auth` | `akam_rtsp_auth.sh` | `USER:akam:HA1` (one line, htdigest) | RTSP/ONVIF Digest login. Absent = no password |
| `credentials` | `akam_rtsp_auth.sh` | `user=NAME` / `pass=PASSWORD` | Clear-text copy needed by ONVIF WS-Security |
| `hostname` | `akam_hostname.sh` | one line, mode 0644 | Network host name. Absent = `akam-XXXX` |
| `onvif` | `akam_onvif.sh` | `enabled=0\|1`, `port=N` | ONVIF opt-in. Absent = off |
| `wsd_instance` | `ak_rtsp` | counter | WS-Discovery instance id |
| `mqtt` | `akam_mqtt.sh` | key=value | MQTT client settings. Absent = off |
| `mqtt_pass` | `akam_mqtt.sh` | one line | Broker password |
| `ntp` | `akam_ntp.sh` | `server=NAME` (max 3) | NTP opt-in. Absent = off |
| `video` | `ak_rtsp` | `bitrate=`, `fps=`, `gop=` | Encoder settings (only keys that differ from the default) |
| `picture` | `ak_rtsp` | `brightness=`, `contrast=`, `saturation=`, `sharpness=` | Picture sliders, -50..50 |
| `night` | `ak_rtsp` | `mode=day\|night` | Forced day/night mode. Absent = auto |
| `leds` | `ak_rtsp` | `status=`, `ir_led=` | Status LED and IR LED policy |
| `audio_out` | `ak_rtsp` | `enable`, `volume`, `gain`, `source`, `talk_enable`, `talk_duplex`, `mic_enable` | Speaker, talk and microphone |
| `audio_out.trying` | `ak_rtsp` | marker | Audio crash guard (see [audio.md](audio.md)) |
| `image` | hand or `ak_rtsp` | key=value, `#` comments | Image-quality switches (`wb`, `venc_rc`, `poll`) |

Other persistent paths:

| Path | Content |
|---|---|
| `/etc/config/wpa_supplicant.conf` | Wi-Fi credentials (stock location, shared with the vendor scripts) |
| `/etc/config/dropbear/dropbear_ed25519_host_key`, `authorized_keys` | SSH host key and client keys (0600, directory 0700) |
| `/etc/config/dropbear/.card_keys_hash` | MD5 of the last imported card key list |

A CONFIG backup therefore contains the Wi-Fi password, the RTSP HA1 and the clear-text ONVIF password. Backups MUST be
kept private.

## 10. RAM files

| Path | Content | Cap |
|---|---|---|
| `/tmp/akam_respawn`, `/tmp/akam_wrapper.log` | Respawn state, wrapper log | 100 lines, trimmed to 50 |
| `/tmp/akam_ssh.log` | SSH helper and Dropbear (`-E`) log | 100 lines or 64 KiB, trimmed to 50 |
| `/tmp/akam_net.log`, `/tmp/akam_ntp.log` | Wi-Fi supervisor, NTP helper | 100 lines, trimmed to 50 |
| `/tmp/akam_image` | RAM-only image-quality switches | hand edited |
| `/tmp/akam_home` | Private root HOME that holds a copy of `authorized_keys` | tiny |
| `/tmp/akam_maintenance` | Marker: the maintenance flag was seen at boot | empty |
| `/tmp/akam_reflash/` | Work directory of an over-SSH flash | image plus tools, about 6 MB |
| `/tmp/isp_live/isp_blocks.bin` | ISP dump from `img.dump` | about 110 KiB |
| `/tmp/sensor_isp.conf` | Sensor ISP tuning file, copied at boot | 282 KB |

Syslog (3 x 200 KB rotating files) stays in RAM; the kernel log filter keeps it near 460 KB per day (about 1.5 days of
history).

## 11. Logging

- `ak_rtsp` prints to stdout with a bracketed prefix (`[ak_rtsp]`, `[control]`, `[night]`, ...). Stdout is teed
  through a pipe: the real stdout (serial console) always gets the bytes, and complete lines are forwarded as `LOG`
  lines on the control port (see [control-port.md](control-port.md)).
- The tee never blocks any thread. The pipe write end is non-blocking; the console is written through a private
  non-blocking file description, or a zero-timeout `poll` guard. A line that cannot be written is dropped and counted
  (`log.dropped`).
- Logs never contain passwords, HA1 values, SSH key material, `Authorization` headers or user names typed by clients.
  Request URIs have any `user:pass@` replaced by `***@`.
- Kernel log filter (`ak_rtsp klog`): drops known noise (the vendor LED driver prints a line per brightness write),
  collapses a message repeated within 60 s into one summary line ("message repeated N times"), compares messages after
  removing counter prefixes and replacing digit runs with `#`, and never collapses priorities `crit`, `alert` and
  `emerg` (oops, panic, BUG). Up to 32 distinct messages are tracked. Status: hardware-validated.

## 12. Idle cost

When no client is connected the camera does only: the AE and night polling (100 ms), the audio capture and silent
playback loops, the control accept poll (5 s), the LED thread (500 ms in `auto`, 2 s in `on`/`off`), the software
watchdog (1 s), and the helper loops (`akam_net.sh` 30 s, `akam_lockdown.sh` 60 s after two minutes). The encoder ring
is drained every 20 ms while no RTSP client is connected so that the next client starts near a key frame.
ONVIF and MQTT add nothing while off. Wakeup counts and idle CPU are not yet measured (see [roadmap.md](roadmap.md)).
