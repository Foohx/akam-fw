# Akam V1: Overview and Installation Path

Akam is a fork of the upstream `LSC_AK3918AV130_cam_custom_firmware` project (MIT). This page is
the map; each step has its own guide.

> **Not validated on hardware.** Akam's code is compiled and tested on a build host with a synthetic
> fixture only. See the [test plan](akam_v1_test_plan.md) for what remains to be checked. Flashing can
> brick the camera, and restoring is not guaranteed.

## 1. The path, in order

1. [Back up all 8 partitions and confirm the model](akam_v1_backup.md). Nothing else comes first.
2. [Install host tools, build `ak_rtsp` and Dropbear, build the APP image from your dump](akam_v1_build.md).
3. [Generate your SSH key, prepare the microSD, understand the Telnet/FTP lockdown](akam_v1_ssh_and_lockdown.md).
4. [Flash only the APP partition, verify, and know how to restore](akam_v1_flash_and_restore.md).
5. [Run the hardware checks and fill in the resource report](akam_v1_test_plan.md).

## Using the wizard

`./akam` walks through the PC-side steps and remembers where you stopped (state and log:
`~/.local/state/akam/`, never containing your Wi-Fi password). It runs on Arch and Debian/Ubuntu.

```
./akam doctor            # check the tools on this computer (never runs sudo, prints the install command)
./akam get-kit --version X.Y.Z   # download + verify the prebuilt kit (or: --kit FILE; or ./akam build)
./akam backup --backup-dir DIR   # accept your backup folder (8 partitions, two copies on two media)
./akam image             # build the Akam image: shows the hardware verdict and the stock compression
./akam --dry-run         # rehearse everything: shows each command, changes nothing
./akam prepare-backup-sd # microSD card for the backup (uses tools/sd/sd_card.sh)
./akam prepare-flash-sd  # microSD card for the flash: key, image, flash_tool, original APP
./akam wipe-sd           # erase a card that holds your Wi-Fi password (offered again at the end)
./akam status            # every step: done / todo / blocked
```

`get-kit` downloads from the project's GitHub Releases over HTTPS and checks the checksum and the
signature (`tools/release/verify_kit.sh`) before anything from the download is used. If no release has
been published yet, it says so: use `--kit FILE` with a kit you already have, or `./akam build`.

Untested firmware needs the typed phrase `I UNDERSTAND THIS IS UNTESTED`; erasing a card needs a typed
`ERASE <device>` (for example `ERASE /dev/sdb`), asked once by the wizard and once by the SD tool, on a
real terminal only. The wizard never runs `sudo` itself; the SD tool (`tools/sd/sd_card.sh`) asks `sudo`
for the disk work and refuses system disks, partitions and non-removable disks. Wi-Fi settings reach the SD
tool through the environment (`AKAM_WIFI_SSID`, `AKAM_WIFI_PASS`, or `AKAM_SD_PASS` read by the wizard),
never the command line, the state file or the log. The camera-side steps (find the camera, back up over Telnet, flash, verify) are not
automated yet: the wizard prints the manual instructions of the guides above for them.

## 2. What changes on the camera

| Partition | Akam V1 |
|---|---|
| UBOOT, ENV, ENVBK, DTB, KERNEL, ROOTFS (`mtd0` to `mtd5`) | Never written |
| CONFIG (`mtd6`, JFFS2, `/etc/config`) | Written by the camera at runtime: Dropbear host key and `authorized_keys` under `/etc/config/dropbear/` (plus the existing Wi-Fi configuration) |
| APP (`mtd7`, SquashFS, `/usr`, 5 242 880 bytes) | Replaced by the image from `build_firmware.sh` |

Inside the APP image: `anyka_ipc` (Tuya) removed; `ak_rtsp` started through `ak_rtsp_wrapper.sh` from a
single-entry `_ht_process.ini`; `dropbearmulti` with `dropbear` / `dropbearkey` links; `akam_ssh.sh`
and `akam_lockdown.sh`; `sbin/service.sh` patched; Dropbear license and a `MANIFEST.txt`.

## 3. Services after Akam (targets, not yet verified on a camera)

| Port | Service | Target state |
|---|---|---|
| 554/tcp | RTSP (`ak_rtsp`) | Open, LAN |
| 8091/tcp | `ak_rtsp` tuning/control port | Open, LAN only (section 4) |
| 22/tcp | SSH (Dropbear), public key only | Open, LAN only |
| 23/tcp, 24/tcp | Telnet | Closed, never re-enabled |
| 21/tcp | FTP | Closed |
| 80/tcp | HTTP | No Akam service |

No cloud endpoint is needed: the Tuya client is removed and the camera has no Internet dependency
(*to be confirmed with the no-route test*, criterion 14 of the test plan).

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
  [README](../README.md#credits--acknowledgements).
- **Dropbear**: MIT-style license, with libtomcrypt and libtommath in the public domain. The image
  carries the license text as `/usr/local/akam/LICENSE.dropbear`.
- **SmolRTSP** (OpenIPC) and its four header-only dependencies: compiled into `ak_rtsp`.
- **Zig**: used as the compiler and downloaded by the bootstrap; not redistributed.
- Ported fixes from the Action-Smart-Connect-Flasher: see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

Your built images contain your camera's stock APP files and are therefore not distributable. Do not
publish them, your dumps, or any key.
