# Akam V1: Overview and Installation Path

Akam is a fork of the upstream `LSC_AK3918AV130_cam_custom_firmware` project (MIT). This page is
the map; each step has its own guide.

> **New here?** The guided path is `./akam` with the prebuilt kit: see the
> [README](../README.md#quick-start-with-the-wizard). This page and the guides it links to are the
> **manual procedure** behind the wizard.
>
> Validated on one real camera on 2026-11-08 (LSC `3215672.3`, SI `D26228`); some checks remain, see
> the [test plan](akam_v1_test_plan.md). Flashing can brick the camera, and restoring is not guaranteed.

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
./akam find-camera       # asks the camera IP, probes ports 24/22/554/8091, tells stock+toolkit from Akam
./akam backup            # double read of the 8 partitions on the camera, copy to ~/akam-backup-<name>-<date>/
./akam backup --backup-dir DIR   # or accept a backup folder you made by hand
./akam image             # build the Akam image: shows the hardware verdict and the stock compression
./akam --dry-run         # rehearse everything: shows each command, changes nothing
./akam prepare-backup-sd # microSD card for the backup (uses tools/sd/sd_card.sh)
./akam prepare-flash-sd  # microSD card for the flash: key, image, flash_tool, original APP
./akam flash             # write the image: SSH if Akam already runs, else the maintenance telnet + card
./akam verify            # SSH + akam_hwcheck.sh + ports, then the same again without the card
./akam update            # later: flash a new image over SSH (--image FILE)
./akam rtsp-auth [set|disable|status]   # optional password on the video stream, over SSH
./akam hostname [NAME]  # give the camera its network name (default: the camera's name), over SSH
./akam cameras           # list the cameras (one progress each); --camera NAME switches or starts one
./akam rename-camera NEW # rename the current camera (its progress moves with it)
./akam --camera NAME --ip IP [--backup-dir DIR] register   # a camera that already runs Akam
./akam restore           # put the original APP back from your backup
./akam wipe-sd           # erase a card that holds your Wi-Fi password (offered again at the end)
./akam step NAME         # run one step again (doctor, get-kit, ..., flash, verify)
./akam status            # every step: done / todo / blocked
```

The same commands cover the whole life of the camera: `find-camera`, `backup`, `flash` and `verify` for
the first install, `update` for every later image (over SSH, no card), `restore` to go back to the
original firmware. Each one that writes to the camera asks you a red yes/no first (default No).

`get-kit` downloads from the project's GitHub Releases over HTTPS and checks the checksum and the
signature (`tools/release/verify_kit.sh`) before anything from the download is used. If no release has
been published yet, it says so: use `--kit FILE` with a kit you already have, or `./akam build`.

**Confirmations.** Nothing has to be typed. Before an irreversible action the wizard shows a red box that
says exactly what will happen and asks ONE yes/no whose default is No (arrow keys or `y`/`N`; Esc, `q`,
Enter and an empty input mean No; without a real terminal it refuses). The box names the card (device,
model, size), or the camera and the image md5 ("do not unplug"), or the untested hardware verdict. After
your yes the wizard answers the downstream prompts itself: it passes `--yes-i-confirmed <device>` to the SD
tool (accepted only with the same `--device`, on a real terminal, and the disk is re-checked before any
write; run by hand, the SD tool still asks for a typed `ERASE <device>`) and sends `YES` to
`install_with_flash_tool.sh` on the camera. A rehearsal (`--dry-run`) never asks. Entering a step from the
menu also asks "Start this step? [Y/n]" after a short summary.

**Cameras and names.** The first run asks for a short name (`a-z`, `0-9`, `-`, at most 20; for example
`salon`). Each camera keeps its own progress and log under `~/.local/state/akam/cameras/NAME/` (the kit is
shared); progress saved by an earlier version is one camera that you are asked to name. The name is used in
`~/akam-backup-<name>-<date>`, `~/akam-image-<name>-<date>` and, after a good `verify`, as the camera's
network name through the camera command `/usr/sbin/akam_hostname.sh set NAME` (applied at the next power
cycle; an older firmware without that command only gets a note). To use `update`, the video password or
`hostname` on a camera that already runs Akam, register it: `./akam --camera akam-01 --ip 192.168.1.50
--backup-dir DIR register`.

**SSH key.** The wizard uses `~/.ssh/akam` (`--pubkey`/`--identity` override it). If `~/.ssh/akam.pub` does not
exist it lists your other ed25519 public keys to choose from (or creates a new dedicated key) and remembers
the choice. `ssh` is run with `-o LogLevel=ERROR`, so OpenSSH's post-quantum key exchange warning is
hidden; real errors are still shown.

The wizard never runs `sudo` itself; the SD tool (`tools/sd/sd_card.sh`) asks `sudo`
for the disk work and refuses system disks, partitions and non-removable disks. Wi-Fi settings reach the SD
tool through the environment (`AKAM_WIFI_SSID`, `AKAM_WIFI_PASS`, or `AKAM_SD_PASS` read by the wizard),
never the command line, the state file or the log.

### The camera steps

The wizard talks to the camera's BusyBox shell (maintenance telnet, port 24) with plain bash over
`/dev/tcp`, so the PC needs no telnet, `nc`, `expect` or Python, and the PC always connects OUT (its
firewall may block inbound connections). Every command is sent alone and ends with a marker that carries
its exit code; a command that gets no answer counts as a failure and after two failures (or with
`--manual-telnet`) the wizard switches to manual mode: it prints the exact command, you paste it in your
own `telnet CAMERA_IP 24` window and paste the output back (end it with a line `END`).

- `find-camera` needs the camera address (router list, maker shown as `AltoBeam`; or `--ip`). Port 24 open
  and 22 closed is a stock camera with the backup card; 22, 554 and 8091 open is a camera running Akam.
- `backup` refuses any partition table other than the 8-partition layout, reads every partition twice on the
  camera, checks the 16 sizes, compares the two reads (only `mtdblock6`, CONFIG, may differ and is read
  again), copies `dump1` to the PC with `nc` and a size + hash check (3 tries per file), writes
  `SHA256SUMS`, compares the stock APP hash with `tools/release/supported_hardware.txt` (a red yes/no
  unless validated) and asks for a second copy on another medium
  (`--backup-copy DIR` makes and checks it). If the transfer fails, the dumps are still on the card:
  copy them by hand and use `--backup-dir`. A camera that already runs Akam cannot be backed up: its flash
  no longer holds the original firmware.
- `flash` checks the image (exactly 5242880 bytes, md5 on the PC and on the card), binds the card on
  `/mnt` if it is on `/tmp/sd`, makes sure the camera has a saved Wi-Fi network (`preflight_check.sh --ensure-wifi`, run on the camera: it
  creates `/etc/config/wpa_supplicant.conf` from the card's `custom/configs/wifi.conf` when the file is
  missing, and refuses before any write if it cannot), starts `install_with_flash_tool.sh`, shows its md5 line
  and asks YOU a red yes/no (default No); only after your yes does the wizard send `YES` to the script, and a
  No, an empty input or a missing terminal aborts it (nothing is written). The result is judged by the
  partition itself: `dd if=/dev/mtd7 bs=64k | md5sum` must equal the image md5 (the script's own success
  line is only information; flash_tool's progress bars are shown as a progress bar). If Akam
  already runs and your key works, `flash` offers the no-card route
  (`tools/flash_scripts/reflash_over_ssh.sh`, progress bars filtered, full output in the state
  directory). Both routes need your yes and risk the camera if the power is cut (UART is the only way
  back from a half-written APP).
- `verify` waits for SSH (showing what it waits for and the elapsed time), runs `akam_hwcheck.sh` from RAM,
  checks the ports, makes sure the Wi-Fi is saved on the camera (creating it from the card when missing),
  and only then asks you to unplug the power, remove the card and plug the power back, and checks that
  telnet (24) and FTP (21) are closed. If the camera does not come back, it says the camera is probably not
  on the network because no Wi-Fi is saved, and what to do. The step is only done after that.
- `update` flashes a new image over SSH; `restore` writes the stock APP from your backup over SSH or the
  maintenance telnet (same gates).
- The wizard never reboots or power-cycles the camera: you unplug and replug the USB-C power by hand.

The camera steps were exercised against a fake camera (`tools/wizard/fake_camera.sh`) and, for the
procedures they automate, on hardware by hand; the scripted telnet session itself has not been run on a
real camera yet.

## 2. What changes on the camera

| Partition | Akam V1 |
|---|---|
| UBOOT, ENV, ENVBK, DTB, KERNEL, ROOTFS (`mtd0` to `mtd5`) | Never written |
| CONFIG (`mtd6`, JFFS2, `/etc/config`) | Written by the camera at runtime: Dropbear host key and `authorized_keys` under `/etc/config/dropbear/` (plus the existing Wi-Fi configuration) |
| APP (`mtd7`, SquashFS, `/usr`, 5 242 880 bytes) | Replaced by the image from `build_firmware.sh` |

Inside the APP image: `anyka_ipc` (Tuya) removed; `ak_rtsp` started through `ak_rtsp_wrapper.sh` from a
single-entry `_ht_process.ini`; `dropbearmulti` with `dropbear` / `dropbearkey` links; `akam_ssh.sh`
and `akam_lockdown.sh`; `sbin/service.sh` patched; Dropbear license and a `MANIFEST.txt`.

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
been tested yet (criterion 14 of the test plan).

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

Problems along the way: [troubleshooting](troubleshooting.md).
