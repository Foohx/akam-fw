# Install Akam on your camera

One page for both cameras: choose yours, get the `akam` program, follow your track, set a stream password. Read the risks
in the [README](../README.md#safety-and-recovery) first: writing the flash can brick a camera. Words you do not know
(kit, appfs, host-tested, card-less...) are in the [glossary](README.md#glossary).

## 1. Which camera do you have?

The codes are on the back of the camera body, under the LSC logo.

| | LSC Indoor 1080p | LSC Rotatable 2K (Dual Band) |
|---|---|---|
| Article, label | `3215672.3`, `SI D26228` | `3225722`, `SZ G26312` |
| Track | [1080p](#3-1080p) | [2K](#4-2k) |

Another model or revision is not supported: [compatible cameras](compatible-cameras.md).

## 2. Get the `akam` program

You need a PC (Linux is the tested system; macOS and Windows build but have never done a first install), a microSD card with a
USB reader, a 2.4 GHz Wi-Fi network, and wisely a 3.3 V USB-UART adapter as a safety net.

1. Download `akam-<os>-<arch>` from the [assistant releases](https://github.com/Foohx/akam-fw/releases?q=assistant-v)
   together with `SHA256SUMS` and `SHA256SUMS.sig`.
2. Check it (SHA256 and signature): [check the download](assistant.md#check-the-download). The release key is in the
   [README](../README.md#release-signing-key).
3. Make it runnable and rename it `akam` (the pages say `akam`): `chmod +x akam-linux-amd64 && mv akam-linux-amd64 akam`
   (Windows and macOS warnings: [unsigned binaries](assistant.md#unsigned-binaries)).
4. `akam doctor` checks your computer and changes nothing. `akam help` lists every command and `--dry-run` rehearses any of them.

The program downloads and verifies the firmware kit of your camera itself. The older bash wizard (`./akam` in a source
checkout, 1080p only) will be retired; the [wizard guide](wizard_guide.md) describes it.

## 3. 1080p

Run `akam` and follow the menu, one step after the other. The nine steps: check the computer, get the kit, prepare the backup
card, find the camera, back up all 8 partitions (read only, twice, verified), build the image from your own backup plus the signed
kit, prepare the flash card, flash, verify. Every risky step shows a red box and defaults to No, and you replug the power
yourself. What each step does: [wizard guide](wizard_guide.md#what-the-wizard-does).

What you will see: after the flash and a power cycle without the card, the camera joins your Wi-Fi, `akam find-camera` finds it
(ports 22 and 554 open) and `ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/stream0` shows the picture. Keep the backup (two
copies, never published).

Status: the real first installs and updates were done with the bash wizard on two 1080p cameras; the Go program's 1080p install
has passed host tests only.

Without the program: [manual steps](manual/backup-sd.md). Back to stock: `akam restore` (never rehearsed on hardware).

## 4. 2K

The order matters: a stock 2K has no SSH, so the card comes first. Each command takes `--dry-run`, and the 2K commands need
`--model lsc-rotatable-2k-ak3918av130` while no backup tells the program the model.

```sh
M=lsc-rotatable-2k-ak3918av130; IP=CAMERA_IP
akam prepare-2k-sd --model $M                                # 1. the first-install card (signed kit, SSH key, Wi-Fi)
# 2. insert the card, power on, wait about 3 minutes, find the camera's address (router list, or akam find-camera)
akam backup-2k   --model $M --ip $IP --out BACKUP_DIR        # 3. six partitions, read only, each read twice
akam seed-2k     --model $M --ip $IP                         # 4. Wi-Fi and SSH key into /config/akam, once
akam appfs-build --model $M --backup-dir BACKUP_DIR --out IMAGE_DIR                                    # 5. image from your mtd3.bin
akam appfs-flash --model $M --ip $IP --backup-dir BACKUP_DIR --image-dir IMAGE_DIR                     # rehearsal, writes nothing
akam appfs-flash --model $M --ip $IP --backup-dir BACKUP_DIR --image-dir IMAGE_DIR --execute           # the real write
# 6. power-cycle the camera yourself, WITHOUT the card; it boots Akam from its flash
akam key-2k      --model $M --ip $IP                         # 7. the API key; every settings command needs it
```

What you will see: step 2, the camera's address answers on SSH (port 22); step 6, it joins Wi-Fi again with no card and port 554
streams (`/stream0`, `/stream1`); step 7, `akam settings list` prints the registry. Keep a UART or a CH341A at hand: only they
recover a half-written flash ([UART recovery](porting/lsc-rotatable-2k/uart-recovery.md)). Full page with every warning, the
rescue card and back to stock: [2K install](porting/lsc-rotatable-2k/README.md#install). The 2K commands have passed host tests
only; the 2K firmware itself was flashed with the maintainer scripts.

## 5. Next: a stream password and your integrations

The video stream has **no password until you set one**:

```sh
akam rtsp-auth set          # both cameras; asks the password twice, never shown
```

Then: [using Akam](using_akam.md) (settings, update, config), [ONVIF](onvif.md), [MQTT and Home Assistant](mqtt.md),
[two-way audio](two_way_audio.md). Something wrong: [troubleshooting](troubleshooting.md).
