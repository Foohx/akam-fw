# Akam V1: Overview and Reference

What Akam changes on the camera, which ports it opens, how the 8091 port works, how to harden the network,
and the licences. To install, use `./akam` (see the [README](../README.md#quick-start)); the manual
procedures behind it are in the [documentation index](README.md#manual-procedures-advanced-without-the-wizard).

> Validated on two real cameras (LSC `3215672.3`, SI `D26228`); what was and was not tested on hardware is in
> [testing status](testing-status.md) and the [test plan](akam_v1_test_plan.md). Flashing can brick the camera, and restoring is not
> guaranteed.

## 1. How the wizard works with the camera

- **State and log:** `~/.local/state/akam/` (one folder per camera under `cameras/NAME/`), never containing
  your Wi-Fi password. The wizard never runs `sudo`; the SD tool (`tools/sd/sd_card.sh`) asks `sudo` for the
  disk work and refuses system disks, partitions and non-removable disks. Wi-Fi settings reach it through the
  environment (`AKAM_WIFI_SSID`, `AKAM_WIFI_PASS`), never the command line, the state file or the log.
- **Maintenance telnet:** during the backup and the first flash the wizard talks to the camera's BusyBox
  shell (port 24, opened by the toolkit card) with plain bash over `/dev/tcp`, so the PC needs no telnet,
  `nc`, `expect` or Python. The PC always connects out (its firewall may block inbound connections). Every
  command ends with a marker that carries its exit code; after two failures, or with `--manual-telnet`, the
  wizard prints each command so you paste it in your own `telnet <camera-ip> 24` window.
- **Backup:** refuses any partition table other than the 8-partition layout, reads every partition twice on
  the camera, compares the reads (only CONFIG, `mtdblock6`, may differ), copies one dump to the PC with size
  and hash checks, writes `SHA256SUMS` and asks for a second copy on another medium (`--backup-copy DIR`). A
  camera that already runs Akam cannot be backed up: its flash no longer holds the original firmware.
- **Flash:** checks the image (exactly 5 242 880 bytes, md5 on the PC and on the camera), saves the Wi-Fi on
  the camera if needed (`preflight_check.sh --ensure-wifi`), then asks the red yes/no (default No). Success
  is judged by reading `/dev/mtd7` back and comparing its md5 with the image. When Akam already runs, the
  wizard flashes over SSH without the card (`tools/flash_scripts/reflash_over_ssh.sh`).
- **Verify:** waits for SSH, runs `akam_hwcheck.sh` from RAM, checks the ports, then asks you to unplug the
  power, remove the card and plug the power back, and checks that telnet (24) and FTP (21) are closed.
- **Never reboots the camera:** you unplug and replug the USB-C power by hand.
- **Kit:** `get-kit` downloads from the project's GitHub Releases over HTTPS and checks checksum and
  signature ([kit](akam_v1_kit.md)). With no release yet, use `--kit FILE` or `./akam build`.
- **Names:** after a good `verify`, the camera's name becomes its network name through the camera command
  `akam_hostname.sh`; an older image without that command only gets a note.

## 2. What changes on the camera

| Partition | Akam V1 |
|---|---|
| UBOOT, ENV, ENVBK, DTB, KERNEL, ROOTFS (`mtd0` to `mtd5`) | Never written |
| CONFIG (`mtd6`, JFFS2, `/etc/config`) | Written at runtime: Dropbear host key and `authorized_keys` under `/etc/config/dropbear/`, the optional network name, the stream password hash (`rtsp_auth`) and its clear-text copy for ONVIF (`credentials`, mode 0600, root only) under `/etc/config/akam/`, and `wpa_supplicant.conf` when the wizard saves your Wi-Fi (all kept across APP reflashes) |
| APP (`mtd7`, SquashFS, `/usr`, 5 242 880 bytes) | Replaced by the image from `build_firmware.sh` |

Inside the APP image: `anyka_ipc` (Tuya) removed; `ak_rtsp` started through `ak_rtsp_wrapper.sh` from a
single-entry `_ht_process.ini`; `dropbearmulti` with `dropbear` / `dropbearkey` links; `akam_ssh.sh`,
`akam_lockdown.sh`, `akam_hostname.sh`, `akam_rtsp_auth.sh`, `akam_debug.sh`, `akam_onvif.sh`, `akam_net.sh` and `akam_ntp.sh`; `sbin/service.sh` and `sbin/ethernet.sh` patched; Dropbear license and a `MANIFEST.txt`.

## 3. Services after Akam

| Port | Service | Target state |
|---|---|---|
| 554/tcp | RTSP (`ak_rtsp`), with the ONVIF audio backchannel (talk) when the video password is on ([two-way audio](two_way_audio.md)) | Open, LAN |
| 8091/tcp | `ak_rtsp` metrics and control port, read-only from the network | Open, LAN only (section 4) |
| 22/tcp | SSH (Dropbear), public key only | Open, LAN only |
| 23/tcp, 24/tcp | Telnet | Closed, never re-enabled |
| 21/tcp | FTP | Closed |
| 80/tcp (or the chosen port) | ONVIF device and media service of `ak_rtsp` | **Closed by default.** Open, LAN only, after `./akam onvif enable` (needs the video password) |
| 3702/udp | ONVIF WS-Discovery (multicast `239.255.255.250`) | Closed by default; open with ONVIF, LAN only |

No cloud endpoint is needed: the Tuya client is removed. Running with no Internet route at all has not
been tested yet (criterion 14 of the [test plan](akam_v1_test_plan.md)).

### ONVIF flag file

`./akam onvif enable | disable` (camera helper `akam_onvif.sh`, over SSH) writes `/etc/config/akam/onvif`, which
survives reboots; the camera applies the change by itself (an older image says to reboot). Format: two lines, LF, in a 0700 directory, mode 0600, replaced atomically:

```
enabled=1
port=80
```

`enabled` is `1` or `0`; `port` is the HTTP port, 1 to 65535 (default 80; UDP 3702 is fixed). **An absent file
means ONVIF is off**; any other content is invalid and counts as off. `enable` is refused unless
`/etc/config/akam/credentials` (written by `./akam rtsp-auth set`) exists and looks valid. This feature ships
in the same release as the ONVIF firmware; an older image ignores the file.

`ak_rtsp` reads the file after the video and RTSP are up (ONVIF never delays them) and again on
`ak_rtsp ctl ONVIF reload` (loopback only, answers `OK onvif.state=<state>`), which starts, stops or moves the HTTP
server and WS-Discovery without touching the video. `GET onvif.state` on 8091 (readable by everyone, also in `LIST`)
says `off`, `on:<port>`, `no_password` (enabled, video password on, but no valid
`/etc/config/akam/credentials`: nothing started) or `error:<reason>` (`port_in_use`, `permission_denied`,
`http_failed`, `discovery_failed`; fix it and run `./akam onvif enable` again). With the video password OFF the
service still starts: reads are open and the changes (reboot, time, name) are refused. The file needs exactly the two
keys; port 21 to 24, 554 and 8091 are refused. A camera name change applies to discovery at the next reload or boot.
Not yet validated on hardware.

### ONVIF with ONVIF Device Manager

ONVIF Profile S (WS-Discovery, device, media and imaging services) is **validated on one camera with ONVIF Device
Manager (ODM) on Windows**: discovery, login, live video and imaging. Other NVRs are not tried yet. Steps:

1. Make sure the camera runs an image that has ONVIF (`./akam update`), then set the video password, and **re-run it
   once even if a password already exists**, so that the clear-text copy ONVIF needs is stored:
   `./akam rtsp-auth set`. `./akam rtsp-auth status` must say `ONVIF-ready`.
2. Turn ONVIF on: `./akam onvif enable` (asks first; default port 80, or `./akam onvif enable 8080`).
   `./akam onvif status` should show the state `on:80`.
3. In ODM, the camera appears by itself through WS-Discovery (UDP multicast 239.255.255.250:3702, same subnet,
   firewall of the PC open for UDP 3702). Otherwise add it by hand with the address
   `http://<camera-ip>/onvif/device_service` (`http://<camera-ip>:PORT/onvif/device_service` for another port).
   The other services are `/onvif/media_service` and `/onvif/imaging_service`.
4. Log in with the video user name and password. The live view uses the camera's RTSP stream, so the one-client limit
   still applies (a second viewer gets 503: put go2rtc in front).

Problems: [troubleshooting](troubleshooting.md#an-onvif-app-does-not-find-the-camera).

## 4. The 8091 control port

Port 8091 is `ak_rtsp`'s line protocol (`LIST`, `GET`, `SET`) for exposure, day/night and image
parameters; its grammar is unchanged from upstream. It has no authentication and, in the source
(`src/ak_rtsp/control.c`), binds to all interfaces, but it is **read-only for the network**:

- `LIST`, `GET` and the state and metrics work for every client (up to 4 at once; a fifth gets `ERR busy`).
- **Privacy of the read side:** the `LOG` lines (client addresses, authentication failures, request paths) are
  pushed only to the camera itself (loopback) or to everyone while debug mode is on: another machine that just
  connects receives none. `net.ssid` and `net.mac` read as `hidden` for other machines unless debug mode is on;
  `net.ip` and the other metrics stay readable. `fw.version` (for example `b35-2af81c5`, the same text as the ONVIF
  `FirmwareVersion`) and `sys.hostname` are read-only keys that anyone on the network may read without a login (`GET
  fw.version`, `GET sys.hostname`, also in `LIST`): the assistant uses them to identify an Akam camera. `sys.time_set` (1 when the camera clock is 2024 or later) is a
  metric like the others.
- `SET` (and `SAVE`, `RESET`) from another machine is refused with `ERR read-only mode, see akam_debug.sh`.
- `SET` from the camera itself (127.0.0.1: SSH helpers such as `./akam led`, `./akam audio`, `./akam picture`) always works.
- The saved video settings (`video.bitrate_cfg`, `video.fps_cfg`, `video.gop_cfg`, `GET video.cfg_pending`; file
  `/etc/config/akam/video`) can only be set from the camera itself (`./akam video`) and apply at the next start of
  `ak_rtsp`. Do not confuse them with the measured `video.bitrate` and `video.fps_actual`.
- **Debug mode** lets the network write too, for a while: `./akam debug on [MINUTES]` (or `akam_debug.sh on`
  over SSH). It lasts 60 minutes by default (1 to 1440), is kept in RAM only, and is off at every reboot and
  every restart of `ak_rtsp`. `./akam debug off` closes it; `./akam debug status` shows it (`GET ctl.debug`
  reads it too). Use it for the PC tuning tool, then switch it off.

No secret is ever exposed on this port. RTSP on 554 has no password unless you set one. Therefore:

- keep the camera on the LAN only;
- **never expose 8091 (or 554, or 22) to the Internet**: no port forwarding, no UPnP, no DMZ host.

Most keys are lost at the next boot. Two LED keys are saved on the camera and applied at start-up:

| Key | Values | Default | Effect |
|---|---|---|---|
| `led.status` | `on`, `off`, `auto` | `on` | Front blue LED. `on`: left as the system sets it. `off`: dark. `auto`: blinks (1 Hz) while the camera starts or has no Wi-Fi, blinks slowly (0.5 Hz) while an RTSP client is playing, solid otherwise |
| `led.state` | `system`, `on`, `off`, `blink_wifi`, `blink_stream` | | Read-only: what the LED does now |
| `night.ir_led` | `auto`, `off` | `auto` | `off`: the IR-cut filter and the night image still switch, but the IR LEDs never light |

They are stored in `/etc/config/akam/leds`. A `SET` answers `OK key=value`, or `ERR ...` and changes nothing
for a value that is not listed; if the file cannot be written the answer ends with `(not saved: ...)`.
From the PC use `./akam led` (see the [README](../README.md#update-change-the-name-add-a-video-password));
by hand: `printf 'SET led.status off\n' | nc -w 2 127.0.0.1 8091` on the camera (a write from the camera itself is
always allowed). Full description:
`specs/06-spec-leds.md`.

### LED settings

The front LED is the kernel LED `state_led` (`/sys/class/leds/state_led`, on or off only) and the IR LEDs
are GPIO 70. In `auto` the "Wi-Fi connected" test is an IPv4 address on `wlan0` whose link state is not
down; the "someone is watching" test is an RTSP client in the PLAY state.

## 4b. Settings commands (over SSH)

All of these talk to the camera over SSH and change nothing on the PC (`./akam help` has the exact syntax).

| Command | Saved on the camera | Applied |
|---|---|---|
| `./akam audio [volume N \| beep \| on \| off \| talk on\|off \| mic on\|off]` | volume, speaker, talk, microphone (`/etc/config/akam/audio_out`) | at once; `mic off` (removes the audio track, the microphone is never opened) and speaker `on` may need a reboot |
| `./akam picture [brightness\|contrast\|saturation\|sharpness N \| wb conf\|manual \| night auto\|day\|night \| reset]` | the four numbers (-50 to 50) | at once and at every start; `wb` and `night` return to default at a reboot |
| `./akam video [bitrate N \| fps N \| gop N]` | `/etc/config/akam/video` | at the next restart of the video service (the command offers it, default No) |
| `./akam led [status on\|off\|auto] [ir auto\|off]` | `/etc/config/akam/leds` | at once |
| `./akam ntp [set SERVER... \| off \| status]` | the servers | at start-up and every 6 h; off by default |
| `./akam onvif [enable [PORT] \| disable \| status]` | `/etc/config/akam/onvif` | at once, no reboot |
| `./akam debug [on [MINUTES] \| off \| status]` | nothing (RAM only) | at once; ends at `off`, the timeout or a reboot |

## 5. Network hardening

- **No port forwarding** from your router to the camera, for any port. Reach it remotely, if you must,
  through a VPN into your LAN.
- **DHCP reservation** for the camera so that its address is stable for your recorder and your SSH
  configuration. (This repository does not store any address; use `<camera-ip>`.)
- **IoT VLAN or isolated SSID** if your equipment supports it: allow your recorder/viewer to reach
  554 (and your admin PC to reach 22 and 8091), nothing else into the VLAN. 8091 only reads from the network
  unless you switched debug mode on.
- **Block outbound traffic** from the camera at the router or firewall (it needs only DHCP, and NTP
  if you configure it with `./akam ntp set SERVER`: UDP 123 to that server only): the camera has no RTC, so the clock is meaningless without NTP, and it should
  not talk to any cloud.
- Treat the microSD card as physical access: a card with `AKAM_MAINTENANCE` and hook scripts can run
  code as root ([maintenance flag](akam_v1_ssh_and_lockdown.md#7-maintenance-flag)).
- Keep the private key `~/.ssh/akam` private, use a passphrase, one key per camera.

## 6. Licenses and third-party notices

- Akam is MIT, like upstream ([LICENSE](../LICENSE)); upstream credits are kept in the
  [README](../README.md#credits-and-license).
- **Dropbear**: MIT-style license, with libtomcrypt and libtommath in the public domain. The image
  carries the license text as `/usr/local/akam/LICENSE.dropbear`.
- **SmolRTSP** (OpenIPC) and its four header-only dependencies: compiled into `ak_rtsp`.
- **Zig**: used as the compiler and downloaded by the bootstrap; not redistributed.
- Ported fixes from the Action-Smart-Connect-Flasher: see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

Your built images contain your camera's stock APP files and are therefore not distributable. Do not
publish them, your dumps, or any key.

Problems along the way: [troubleshooting](troubleshooting.md).
