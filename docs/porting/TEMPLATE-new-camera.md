# Porting a new camera: checklist and doc skeleton

Copy the skeleton into `docs/porting/<model-id>/` and fill it while you work. Rule: **everything learned about a
camera goes into the repository docs**, the same day (partitions, formats, contents, hardware, original firmware).
Never commit vendor files, dumps, keys, Wi-Fi data or nvram values (the hygiene lint refuses them).
Two finished examples: [1080p](lsc-1080p/on-camera.md), [Rotatable 2K](lsc-rotatable-2k/on-camera.md).

Model id: `<brand>-<form>-<resolution>-<soc>`, stable once released (see the header of
[the registry file](../../tools/release/models/lsc-indoor-1080p-ak3918.txt)). A new id is needed when the board, SoC,
flash layout or stock firmware family differs.

## 1. Dump (nothing is written to the camera)

- [ ] Photos of the board and the label (article, SI code); SoC, flash chip, Wi-Fi chip, sensor.
- [ ] UART pins found and tested ([UART access](../hacking.md#uart-access), 3.3 V only); boot log saved.
- [ ] Full flash backup, every partition, with `MD5SUMS` and `SHA256SUMS` ([by UART](../manual/backup-uart.md),
      [by microSD](../manual/backup-sd.md)). Keep it private under `.local/`.
- [ ] `/proc/mtd`, `uname -a`, BusyBox version, `ps`, `netstat -tuln`, `mount`, `lsmod`, `dmesg`, GPIO list
      ([stock capture](../manual/stock-capture.md)).
- [ ] Extract the squashfs partitions on the host (`unsquashfs`); note compressor, block size and flags.
- [ ] Find whether U-Boot checks a signature or CRC ([example](lsc-rotatable-2k/flash-signature-findings.md)); decide
      whether a flash write is allowed before any write.

## 2. Document (skeleton for `docs/porting/<model-id>/`)

| File | Content |
|---|---|
| `on-camera.md` | the boot chain on the camera: every binary and script that runs (stock and Akam), who starts it, why, kept / replaced / removed, files and partitions read or written, flash-write frequency, ports. Same shape as the two examples; **checked by `tests/on_camera_docs_test.sh`** (section 5) |
| `stock-firmware-reference.md` | platform table, flash map, boot chain, GPIO map, config keys, network services of the stock firmware; each fact tagged HW (seen on the camera), RE (offline) or unverified |
| `decloud-findings.md` | what talks to the cloud, what removes routes, what blocks it |
| `<feature>-findings.md` | one per hardware feature (PTZ, audio, NPU, button, light...): evidence first, then the decision |
| `control-api.md` | only if the model differs from the shared signed API |
| `flash-test-plan.md`, `uart-recovery.md` | staged plan with a rollback at every stage; recovery by UART or SPI programmer |

Also update: [models](../models.md), [compatible cameras](../compatible-cameras.md), the README table, the
[parity matrix](../parity-matrix.md), [testing status](../testing-status.md).

## 3. Choose the kind, reuse the shared modules

### Where the files of a new camera go

The tree separates three things, so a port adds files in a predictable place (details: [HAL architecture](../hal-architecture.md), `tools/dev/path-map.txt`):

| What | Where | New SoC family or kernel interface | New camera on a known platform |
|---|---|---|---|
| Shared code, no vendor header, no device path, no model name | `src/{core,net,api,features,hal}/` (modules listed once in `src/build/sources.mk`) | only if the code is generic | only if the code is generic |
| Drivers and process glue for one SoC family and one kernel interface | `platforms/<soc-family>/` (README: SoC, libc, toolchain, kernel interface; `Makefile`; `toolchain/`) | **new directory** | reuse |
| Everything specific to one camera | `firmware/<model-id>/` (`model.mk`: features and the ordered object list; the HAL descriptors, settings rows, API binding, service table; `boot/`, `image/`, `helpers/`; the model sheet is `tools/release/models/<model-id>.txt`) | new directory | **new directory** |
| Tests | `tests/*_test.sh` (+ `tests/native/`), the `*_test.sh` listed in `tools/ci/test_groups.txt` | | |
| Docs | `docs/porting/<model-id>/` | | |

A camera on an existing platform builds with `make -C platforms/<soc-family> MODEL=<model-id>` (everything is written under `build/<model-id>/`); add a root `Makefile` target next to `1080p` and `2k`.
Keep the object order of `model.mk` explicit (it sets the layout of the stripped binary: no wildcard, no sorting).

- Kinds ([models](../models.md)): `native-app` (replace the APP partition), `cardless-appfs` (replace appfs, stock files
  kept), `decloud-overlay` (SD card only, nothing flashed). Take the least invasive kind that works.
- Reuse, do not rewrite: `src` pure logic and the HAL contracts ([HAL architecture](../hal-architecture.md):
  `hal_light`, `hal_ircut`, `hal_ptz`, `hal_audio`, `hal_sys`; night logic, LEDs, API core, settings registry, MQTT,
  ONVIF, RTSP). A new camera fills a descriptor and a thin driver.
- Shell scripts: `firmware/lsc-indoor-1080p-ak3918/overlay/` (ssh, hostname, lockdown, mqtt, onvif, rtsp auth, debug, wrapper; `akam_hostname.sh` is also installed by the 2K builders; it is being moved to a shared place) and `src/scripts/` (`akam_ntp.sh`, `akam_net_wifi_supervisor.sh`, installed on the 1080p as `akam_net.sh`; the 2K has its own `boot/card/akam/akam_net.sh`).
  Dropbear: `tools/dropbear/` (the time32 shim is needed: kernel 4.4.302 has no 64-bit time syscalls).
- **features.mk parity rule**: the model's `model.mk` sets `AKAM_FEAT_<NAME> := 1` or `0` for `API PTZ BUTTON SCENE PATROL
  TRACK SYS LIGHT ONVIF` before `include .../features.mk` (unset = off; see `src/build/features.mk`). Set `1` only for
  what the hardware has and the port implements, and give every row of the [parity matrix](../parity-matrix.md) a
  status for the new model.
- No recurring flash write in normal operation (state in RAM, writes only on a user change). A feature stays off until
  the user turns it on.

## 4. Anti-brick rules

Never write U-Boot. Write only the partition that carries the payload, after verifying the backup and the image on the
host, with read-back. Keep a recovery path that needs no flash write (SD override, UART). Rehearse the restore.

## 5. Tests and goldens to add

- [ ] `tests/on_camera_docs_test.sh`: add a `check_<model>` for the new `on-camera.md` so its paths cannot rot (Akam
      paths must exist in the image file list or build script; a stock path is written `/path` (stock)).
- [ ] One line per new test in `tools/ci/test_groups.txt` (`lint`, `shell`, `native`, `go` or `rot2k`), or local_check fails.
- [ ] Registry file `tools/release/models/<model-id>.txt` (stock partition sha256, kernel, layout).
- [ ] Host tests with fake hardware for the new driver and boot scripts (the `AKAM_ROOT` prefix pattern of the overlay scripts).
- [ ] Binary golden in `tests/golden/` (sha256 and size) and an image-versus-file-list test like `tests/kit_test.sh`.
- [ ] Hardware checklist in [hardware test checklists](../hardware-test-checklists.md); hardware results go in
      [testing status](../testing-status.md) only after a real run.
- [ ] `tools/ci/local_check.sh` (all groups) before merging.
