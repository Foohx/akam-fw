# Akam V1: Overview and Reference

What Akam changes on the camera, which ports it opens, how the 8091 port works, how to harden the network,
and the licences. To install, use `./akam` (see the [README](../README.md#quick-start)); the manual
procedures behind it are in the [documentation index](README.md#manual-procedures-advanced-without-the-wizard).

> Validated on two real cameras (LSC `3215672.3`, SI `D26228`) between 2026-11-07 and 11-09; some checks
> remain, see the [test plan](akam_v1_test_plan.md). Flashing can brick the camera, and restoring is not
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
| CONFIG (`mtd6`, JFFS2, `/etc/config`) | Written at runtime: Dropbear host key and `authorized_keys` under `/etc/config/dropbear/`, the optional network name and stream password hash under `/etc/config/akam/`, and `wpa_supplicant.conf` when the wizard saves your Wi-Fi (all kept across APP reflashes) |
| APP (`mtd7`, SquashFS, `/usr`, 5 242 880 bytes) | Replaced by the image from `build_firmware.sh` |

Inside the APP image: `anyka_ipc` (Tuya) removed; `ak_rtsp` started through `ak_rtsp_wrapper.sh` from a
single-entry `_ht_process.ini`; `dropbearmulti` with `dropbear` / `dropbearkey` links; `akam_ssh.sh`,
`akam_lockdown.sh`, `akam_hostname.sh` and `akam_rtsp_auth.sh`; `sbin/service.sh` and `sbin/ethernet.sh` patched; Dropbear license and a `MANIFEST.txt`.

## 3. Services after Akam

| Port | Service | Target state |
|---|---|---|
| 554/tcp | RTSP (`ak_rtsp`) | Open, LAN |
| 8091/tcp | `ak_rtsp` tuning/control port | Open, LAN only (section 4) |
| 22/tcp | SSH (Dropbear), public key only | Open, LAN only |
| 23/tcp, 24/tcp | Telnet | Closed, never re-enabled |
| 21/tcp | FTP | Closed |
| 80/tcp | HTTP | No Akam service |

No cloud endpoint is needed: the Tuya client is removed. Running with no Internet route at all has not
been tested yet (criterion 14 of the [test plan](akam_v1_test_plan.md)).

## 4. The 8091 control port

Port 8091 is `ak_rtsp`'s line protocol (`LIST`, `GET`, `SET`) for exposure, day/night and image
parameters. **Its protocol is unchanged from upstream.** It has no authentication and, in the source
(`src/ak_rtsp/control.c`), binds to all interfaces. The same holds for RTSP on 554, which has no
password. Therefore:

- keep the camera on the LAN only;
- **never expose 8091 (or 554, or 22) to the Internet**: no port forwarding, no UPnP, no DMZ host.

## 5. Network hardening

- **No port forwarding** from your router to the camera, for any port. Reach it remotely, if you must,
  through a VPN into your LAN.
- **DHCP reservation** for the camera so that its address is stable for your recorder and your SSH
  configuration. (This repository does not store any address; use `<camera-ip>`.)
- **IoT VLAN or isolated SSID** if your equipment supports it: allow your recorder/viewer to reach
  554 (and your admin PC to reach 22 and 8091), nothing else into the VLAN.
- **Block outbound traffic** from the camera at the router or firewall (it needs only DHCP, and NTP
  if you configure it): the camera has no RTC, so the clock is meaningless without NTP, and it should
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
