# LSC Rotatable 2K (`lsc-rotatable-2k-ak3918av130`)

Entry page of this camera. The 1080p has the same page with the same sections: [LSC Indoor 1080p](../lsc-1080p/README.md).
Source of the model definition: [`firmware/lsc-rotatable-2k-ak3918av130`](../../../firmware/lsc-rotatable-2k-ak3918av130)
and [`platforms/anyka-ak3918av130`](../../../platforms/anyka-ak3918av130).

Status: **validated on 1 unit** (article `3225722`, label `SZ G26312`), card-less boot since 2026-11-15, with an earlier binary:
the current build has not been flashed yet ([testing status](../../testing-status.md#which-build-the-hardware-results-come-from)).

## Hardware

| Item | Value |
|---|---|
| SoC | Anyka AK3918AV130 (with NPU), Linux 4.4.302-cip94 |
| Sensor, Wi-Fi | SC1346 (from the ISP configuration file), ATBM6132C |
| Flash | 8 MiB SPI NOR, 6 partitions: `mtd0` U-Boot, `mtd1` kernel, `mtd2` rootfs, `mtd3` appfs, `mtd4` config (jffs2), `mtd5` factory |
| Motor, audio, LEDs | pan/tilt motor, microphone, speaker (present, not driven by Akam), status LED and IR LEDs, front button |
| Features compiled in | system, light, API, ONVIF, PTZ, button, scene, patrol, tracking (`firmware/lsc-rotatable-2k-ak3918av130/model.mk`) |

## Stock firmware findings

Offline reverse engineering of your own backup and checks on the camera; no vendor file is stored in the repository.

- Overview and boot chain: [stock firmware reference](stock-firmware-reference.md).
- Cloud routes and what can remove them: [decloud findings](decloud-findings.md).
- Flash write protection and signature: [flash signature findings](flash-signature-findings.md).
- Motor: [PTZ findings](ptz-findings.md). NPU and detection: [NPU findings](npu-findings.md), [NPU models](npu-models.md),
  [own-app AI](ai-own-app.md). Audio: [audio findings](audio-findings.md). Image: [ISP controls](isp-controls.md),
  [video live parameters](venc-live-params.md). Light and button: [hal_light](hal-light.md), [button and zones](button-and-zones.md).
- Historical plans: [docs/archive](../../archive/README.md).

## What runs on the camera

One application, `akam2k_rtsp`, in place of the stock `yy_cam`: two RTSP streams (`/stream0` 2304x1296, `/stream1` 640x360),
NPU person detection, pan/tilt, tracking, patrol, ONVIF, MQTT, the signed API, LEDs, night mode, button. Key-only SSH.
Vendor libraries, drivers, kernel and U-Boot are kept. Boot chain and files: [on the camera](on-camera.md).
What to do after the install (stream password, MQTT, ONVIF, settings file): [using the 2K](using-the-2k.md).
Settings and API: [control API](control-api.md), [network settings](net-settings.md), [tracking](tracking.md), [drift](drift.md).

## Install

**Status: the 2K commands of the Go `akam` program (the bash `./akam` has none) have passed host tests only, on fake
cameras; none was run on the real camera, and the current firmware build has not been flashed (see
[testing status](../../testing-status.md#which-build-the-hardware-results-come-from)). Have a UART or a CH341A at hand
([UART recovery](uart-recovery.md)).** Get the program from the
[assistant releases](https://github.com/Foohx/akam-fw/releases?q=assistant-v) (see the
[README quick start](../../../README.md#quick-start)); install from Linux, a first install from macOS or Windows has
never been done.

The order matters. A stock 2K has no SSH, so the card comes first: it is what gives you a shell.

1. **Card.** `prepare-2k-sd` downloads the newest signed 2K kit, verifies it, and writes the first-install card (it asks the
   SSH key, the Wi-Fi and the device). The camera boots Akam from it with **no flash write**. The card holds your Wi-Fi key
   and SSH public key, and disables the rescue telnet (`akam/no_telnet`) by default; only `--rescue-telnet` keeps a
   passwordless root telnet on port 23, for a rescue card.
2. **Boot from the card:** power the camera off, insert the card, power it on, wait about 3 minutes and find its address
   (your router's list, or `akam find-camera`).
3. **Backup.** `backup-2k` reads the six partitions over SSH, read only, each twice. Keep the folder private and a second
   copy elsewhere: `appfs-build` and every later `update` use it.
4. **Seed.** `seed-2k` copies the Wi-Fi settings and SSH key from the card into `/config/akam` (done on the camera over
   SSH, nothing shown, a file already there is never overwritten; see
   [card-less appfs, section 4](cardless-appfs.md#4-config-and-secrets-configakam-jffs2-persistent-never-the-image)).
   **Without this step the card-less camera has no Wi-Fi and no SSH.** Optional: `key-2k` creates the API key of the assistant.
5. **Build, then flash.** `appfs-build` builds the card-less image on your computer from your `mtd3.bin` and the same
   verified kit; `appfs-flash` rehearses by default and writes only with `--execute` (recovery question and typed
   confirmation). You power-cycle the camera yourself afterwards, without the card.

```sh
M=lsc-rotatable-2k-ak3918av130        # pass --model (the 2K commands refuse any other camera type)
IP=CAMERA_IP                          # the address from step 2
akam prepare-2k-sd --model $M                                    # 1. downloads and verifies the signed kit
# 2. insert the card, power on, wait about 3 minutes
akam backup-2k --model $M --ip $IP --out BACKUP_DIR              # 3. a new folder; read only
akam seed-2k   --model $M --ip $IP                               # 4. once, before the flash
akam appfs-build --model $M --backup-dir BACKUP_DIR --out IMAGE_DIR       # 5. downloads and verifies the kit
akam appfs-flash --model $M --ip $IP --backup-dir BACKUP_DIR --image-dir IMAGE_DIR            # rehearsal
akam appfs-flash --model $M --ip $IP --backup-dir BACKUP_DIR --image-dir IMAGE_DIR --execute  # the real write
```

Every command takes `--dry-run`. All of them verify the kit the same way: SHA256SUMS, the release signature and the model.
`--kit FILE` (a `.tar.xz` you already downloaded from the
[firmware releases](https://github.com/Foohx/akam-fw/releases?q=firmware-v)) and `--version X.Y.Z` work on `prepare-2k-sd`
and `appfs-build`; add `--identity FILE` if your SSH key is not `~/.ssh/akam`. Alternatively use the scripts
(`firmware/lsc-rotatable-2k-ak3918av130/image/build_appfs.sh`, `firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh`;
dry run by default). Design, gates and rollback: [card-less appfs](cardless-appfs.md). What to do once it runs:
[using the 2K](using-the-2k.md).

The kit is built by the release pipeline (`tools/release/make_release.sh firmware`, through `tools/release/make_kit_2k.sh`)
and published with the firmware release. If the releases page has no firmware release with a 2K kit, build the kit yourself
with `tools/release/make_kit_2k.sh` and pass the folder: `--kit DIR --allow-unsigned` on `prepare-2k-sd` (a folder cannot be
authenticated: you trust it entirely), or `--kit DIR` on `appfs-build`.

## Update

`akam update` (rehearsal, typed confirmation, read-back compare, version check after the reboot; the previous image is kept).
Details: [assistant](../../assistant.md#install-and-update-of-the-2k-from-the-assistant-prepare-2k-sd-update).
Host-tested only. Without `--kit` it downloads the newest signed release that carries the 2K kit (see Install if there is none).

## Recovery

- **SD rescue:** an Akam card is read before any Akam code and overrides the flash; a broken payload does not lock you
  out as long as the appfs still mounts (a half-written `mtd3` needs a UART or a CH341A programmer). A card without the
  file `akam/no_telnet` opens a passwordless root telnet on port 23 while it is in: cards from `prepare-2k-sd` have the
  file unless you asked for `--rescue-telnet`.
- **Rollback:** `akam update --rollback` flashes the previous Akam image with the same gates (host-tested only).
- **Last resort:** a 3.3 V UART or a CH341A SPI programmer: [UART recovery](uart-recovery.md).

### Back to stock

Your `backup-2k` folder holds the six partitions (`mtd0.bin` to `mtd5.bin`). Only `mtd3` (appfs) was ever changed by Akam, so
back to stock means writing your own `mtd3.bin` back. There is **no `akam restore-2k` command yet** (missing; `akam restore`
is the 1080p's and is refused on a 2K), and this path was **never run on hardware**.

1. Boot the camera with the Akam SD card inserted (recovery mode) and SSH in. Stop the Akam app and make sure nothing
   maps `/appfs/lib`: those are the preflight checks of `appfs-flash` (`flash_appfs.sh`, step b), which a manual write
   skips. Check on the PC that your `mtd3.bin` is the one in `SHA256SUMS` of the backup and note its `md5sum`.
2. Upload it: `ssh root@CAM 'cat > /tmp/mtd3.bin' < mtd3.bin`, then `ssh root@CAM 'md5sum /tmp/mtd3.bin'` must give the same md5.
3. Write it **detached**, so that a lost Wi-Fi link cannot leave a half-written partition. The commands are those of the
   rollback worker (`ROLLBACK_WORKER` in `firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh`): `flashcp -v
   /tmp/mtd3.bin /dev/mtd3`, then `dd if=/dev/mtd3 bs=65536 | md5sum` must equal the md5 of the file; if not, `dd
   if=/tmp/mtd3.bin of=/dev/mtdblock3 bs=65536; sync` and compare again (the worker tries three times). Put them in a
   script and start it with `nohup sh /tmp/script.sh >/tmp/log 2>&1 </dev/null &`, then read the log over SSH.
4. If you did `--nvram` at the flash, put the old `appfs_sign` back (`nvram set appfs_sign <old>; nvram commit`).
5. Power-cycle without the card and check that the stock app starts.
6. If the camera does not boot: UART `sf write` or a CH341A, from the same `mtd3.bin` ([UART recovery](uart-recovery.md)).

The `/config` partition (`mtd4`) keeps `/config/akam` (Wi-Fi, SSH key, API keys): delete that folder if you give the camera away.

## Known limits

- No speaker output (no talk), no recording to microSD, no generic motion detection (person presence events only); the microphone track is opt-in. 5 GHz Wi-Fi is untested.
- ONVIF on the 2K, MQTT, the signed API and the network settings were not all exercised on the camera: see
  [testing status](../../testing-status.md) and each page's status line.
- The 2K commands of the assistant were never run on the real camera (`prepare-2k-sd`, `backup-2k`, `key-2k`, `seed-2k`, `appfs-build`, `appfs-flash`, `update`, `update --rollback`).
- No `akam restore-2k` command: back to stock is manual (see Recovery).
- Open ports of the card-less image are not confirmed by a port scan.
