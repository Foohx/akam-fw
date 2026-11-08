# Camyn V1: Overview and Installation Path

Camyn is a fork of the upstream `LSC_AK3918AV130_cam_custom_firmware` project (MIT). This page is
the map; each step has its own guide.

> **Not validated on hardware.** Camyn's code is compiled and tested on a build host with a synthetic
> fixture only. See the [test plan](camyn_v1_test_plan.md) for what remains to be checked. Flashing can
> brick the camera, and restoring is not guaranteed.

## 1. The path, in order

1. [Back up all 8 partitions and confirm the model](camyn_v1_backup.md). Nothing else comes first.
2. [Install host tools, build `ak_rtsp` and Dropbear, build the APP image from your dump](camyn_v1_build.md).
3. [Generate your SSH key, prepare the microSD, understand the Telnet/FTP lockdown](camyn_v1_ssh_and_lockdown.md).
4. [Flash only the APP partition, verify, and know how to restore](camyn_v1_flash_and_restore.md).
5. [Run the hardware checks and fill in the resource report](camyn_v1_test_plan.md).

## 2. What changes on the camera

| Partition | Camyn V1 |
|---|---|
| UBOOT, ENV, ENVBK, DTB, KERNEL, ROOTFS (`mtd0` to `mtd5`) | Never written |
| CONFIG (`mtd6`, JFFS2, `/etc/config`) | Written by the camera at runtime: Dropbear host key and `authorized_keys` under `/etc/config/dropbear/` (plus the existing Wi-Fi configuration) |
| APP (`mtd7`, SquashFS, `/usr`, 5 242 880 bytes) | Replaced by the image from `build_firmware.sh` |

Inside the APP image: `anyka_ipc` (Tuya) removed; `ak_rtsp` started through `ak_rtsp_wrapper.sh` from a
single-entry `_ht_process.ini`; `dropbearmulti` with `dropbear` / `dropbearkey` links; `camyn_ssh.sh`
and `camyn_lockdown.sh`; `sbin/service.sh` patched; Dropbear license and a `MANIFEST.txt`.

## 3. Services after Camyn (targets, not yet verified on a camera)

| Port | Service | Target state |
|---|---|---|
| 554/tcp | RTSP (`ak_rtsp`) | Open, LAN |
| 8091/tcp | `ak_rtsp` tuning/control port | Open, LAN only (section 4) |
| 22/tcp | SSH (Dropbear), public key only | Open, LAN only |
| 23/tcp, 24/tcp | Telnet | Closed, never re-enabled |
| 21/tcp | FTP | Closed |
| 80/tcp | HTTP | No Camyn service |

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
- Treat the microSD card as physical access: a card with `CAMYN_MAINTENANCE` and hook scripts can run
  code as root ([maintenance flag](camyn_v1_ssh_and_lockdown.md#7-maintenance-flag)).
- Keep the private key `~/.ssh/camyn` private, use a passphrase, one key per camera.

## 6. Licenses and third-party notices

- Camyn is MIT, like upstream ([LICENSE](../LICENSE)); upstream credits are kept in the
  [README](../README.md#credits--acknowledgements).
- **Dropbear**: MIT-style license, with libtomcrypt and libtommath in the public domain. The image
  carries the license text as `/usr/local/camyn/LICENSE.dropbear`.
- **SmolRTSP** (OpenIPC) and its four header-only dependencies: compiled into `ak_rtsp`.
- **Zig**: used as the compiler and downloaded by the bootstrap; not redistributed.
- Ported fixes from the Action-Smart-Connect-Flasher: see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

Your built images contain your camera's stock APP files and are therefore not distributable. Do not
publish them, your dumps, or any key.
