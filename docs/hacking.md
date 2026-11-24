# Hacking guide: build, test and hack your camera

For developers and tinkerers. To just install Akam, use the `akam` program ([install guide](install.md)).

## Repository layout

```text
.
├── akam                          # the bash wizard (Linux, 1080p; to be retired)
├── docs/                         # documentation (index: docs/README.md)
├── firmware/<model-id>/          # ONLY what is specific to one camera model
│   ├── model.mk                  #   features + ordered object list, model-only flags, vendor/publish dirs, helper programs
│   ├── *.c, *.h                  #   its binding: HAL descriptors, settings rows, API binding, service table
│   ├── boot/ image/ overlay/     #   on-camera scripts, image and card builders (what the model has)
│   └── helpers/                  #   its small side programs (2K: sntp, mic, ptz, led)
├── images/                       # photos of the camera and its board
├── platforms/<soc-family>/       # per SoC family and kernel interface: drivers, process glue, Makefile, toolchain
│   ├── anyka-ak3918/             #   direct drivers (raw ioctls, static musl): the 1080p
│   └── anyka-ak3918av130/        #   vendor libraries (dynamic uClibc, NPU): the 2K
├── specs/                        # as-built specification of the code
├── src/                          # SHARED code only (no vendor header, no device path, no model name)
│   ├── core/                     #   boot-time config, logs, registry, control gate, version (the watchdog is platform code: platforms/anyka-ak3918/swdog.c)
│   ├── net/{rtsp,onvif,mqtt,http,crypto}/
│   ├── api/                      #   the signed control API
│   ├── features/{night,light,button,audio,ptz,track,scene}/   # pure logic, host-tested
│   ├── hal/                      #   hardware interfaces (+ generic drivers)
│   ├── build/                    #   sources.mk (module table), features.mk
│   └── scripts/                  #   on-camera shell shared by several models
├── tests/                        # host tests (shell and C, fake cameras, synthetic fixtures)
├── third_party/                  # fetched dependencies (SmolRTSP, git-ignored)
└── tools/
    ├── akam-talk/                # host client that sends sound to the speaker
    ├── assistant/                # the Go program `akam` (Windows, macOS, Linux)
    ├── build/                    # bootstrap.sh (pinned toolchain), check_host.sh, env.sh, deps.lock
    ├── ci/                       # local_check.sh, distro_check.sh, docs_links_check.sh (links and paths of the Markdown)
    ├── dev/                      # path-map.txt and rewrite_paths.sh (late branches), motor and tracking tools
    ├── dropbear/                 # Dropbear SSH build and options
    ├── flash_scripts/            # flash, restore, reflash over SSH, start_rtsp.sh
    ├── hw_tests/                 # akam_hwcheck.sh, ONVIF check, stock capture tools
    ├── release/                  # kit, signing, release scripts and notes template
    ├── sd/                       # microSD preparation (Linux)
    └── wizard/                   # libraries of the bash wizard, a fake camera for tests
```

### The 2K model directory, target and current state

**Target:** `firmware/<model-id>/` holds only camera facts: `model.mk`, the HAL and PTZ descriptors, the pins, the settings table, boot scripts, image
builders. **Current:** that is true for the 1080p (its `settings.c` and `api_bind.c` still describe the platform's ISP, encoder and LED, a smaller debt), but not
for the 2K. `firmware/lsc-rotatable-2k-ak3918av130/` has 30 C files (four of them `*_off.c` stand-ins), of which only `hal.c`, `ptz_desc.c`, `pin.c` and the two small sysfs drivers hold hardware
facts; `ptz.c` (about 1 750 lines), `settings.c`, `track.c`, `scene.c`, `patrol.c`, `button_rt.c`, `night_ctl.c`, `zones.c`, `vlive.c` and others are feature logic or wiring.
The platform (`platforms/anyka-ak3918av130/`) reaches into the model directory by basename (`-I$(MODEL_DIR)`, 17 headers) and the model includes platform headers
back, so the two directories are one program in two folders. Consequences today: a new camera on the 2K platform would copy that code; a 2K feature switched off
(`PTZ/TRACK/SCENE/PATROL`) links stand-ins kept in the model directory (`*_off.c`); `tools/dev/path-map.txt` records the files as "kept whole". Known
obstacles: `store.h`, `button_rt.h` and `patrol.h` carry `/config/akam/*` paths (they must become model settings before moving), and the 2K `leds.*` needs a
new name before it can join `src/` (the 1080p already has a `leds.h`, and header basenames are unique).
**Planned split**, in this order, each step gated by the 2K golden (`tools/ci/repro_check_rot2k.sh`): (a) pin and sensor facts into a model descriptor so the
platform stops including `pin.h`; (b) the pure files (`zones`, `detstate`, `store`, `netreg`, `leds`, `button_rt`, `night_ctl`, `patrol`, `vlive`, `video*`) to
`src/features/` or `src/core/`; (c) `ptz.c`, `track.c`, `scene.c`, `settings.c` once their store and HAL calls go through ops tables. It waits for the PTZ and
tracking work in progress in those files.

| Component | What it does | Where |
|---|---|---|
| `akamd` | RTSP/RTP H.264 and audio server, control port 8091 (1080p), signed API on 8092, ONVIF, MQTT (and on the 2K: pan/tilt, tracking, NPU detection) | `platforms/anyka-ak3918/` (1080p), `platforms/anyka-ak3918av130/` (2K) |
| Dropbear 2026.94 | Static SSH server, ed25519 keys only | `tools/dropbear/` |
| Camera scripts | SSH start, lockdown, hostname, password, debug, ONVIF, NTP, MQTT, Wi-Fi supervisor | `firmware/lsc-indoor-1080p-ak3918/overlay/` (and `src/scripts/` for the shared ones) |
| Image builder | Patches **your own** APP dump, repacks, pads to 5 242 880 bytes | `firmware/lsc-indoor-1080p-ak3918/image/build_image.sh`, `firmware/lsc-indoor-1080p-ak3918/image/assemble_from_kit.sh` |
| Release kit | Prebuilt Akam binaries and scripts, signed | `tools/release/` ([kit](manual/kit.md)) |

## Build from source

Everything runs on a PC and never touches the camera. Arch Linux is the reference host; Debian 12 and Ubuntu 24.04 are
checked in containers.

```bash
bash tools/build/check_host.sh      # checks your PC, prints the exact install command if something is missing
bash tools/build/bootstrap.sh       # once: pinned Zig, SmolRTSP, BusyBox, Go modules
. tools/build/env.sh
make -C platforms/anyka-ak3918 akamd flash_tool
bash tools/dropbear/build_dropbear.sh
```

This builds the 1080p. For the 2K, `make 2k CAMERA_LIBC=...` builds `akamd`, `make 2k-appfs BACKUP=...` the appfs image and
`tools/release/make_kit_2k.sh --model lsc-rotatable-2k-ak3918av130` the kit: see
[`firmware/lsc-rotatable-2k-ak3918av130/README.md`](../firmware/lsc-rotatable-2k-ak3918av130/README.md#build-the-app-the-kit-and-the-appfs-image).
Per-platform notes: [`platforms/anyka-ak3918`](../platforms/anyka-ak3918/README.md), [`platforms/anyka-ak3918av130`](../platforms/anyka-ak3918av130/README.md).

The result is a static ARM binary (`arm-linux-musleabi`, `-mcpu=arm926ej_s`). To build a flashable image from your own
APP dump: [build guide](manual/build.md).

## Run a development build in RAM

You can test a new `akamd` without flashing anything. A power cycle brings back what was flashed.

1. Get SSH access (an Akam camera) or the maintenance shell, with a microSD card in the camera (mounted on `/mnt`).
2. Build, then upload the binaries and scripts to `/tmp` over SSH: `make -C platforms/anyka-ak3918 upload CAMERA_IP=<camera-ip>`.
3. Copy `akamd` to the card as `/mnt/akamd` (`cp /tmp/akamd /mnt/`): `tools/flash_scripts/start_rtsp.sh`
   starts the binary from `/mnt`, and writes its log and a live `dmesg` copy there too.
4. On the camera run `sh /tmp/start_rtsp.sh`. It stops the stock watchdog, the running `akamd` (or `anyka_ipc` on a
   stock camera), reloads the encoder modules and starts the new binary.
5. Watch the stream with `ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/`, and the log on the UART or the card.

Because the stock watchdog is stopped, the hardware watchdog may reboot the camera after a while: that is expected.

### Try a new 1080p `akamd` on one camera with a rollback (RAM only, flash untouched)

The wrapper (`akam_run.sh`) starts `/usr/bin/akamd` from the read-only squashfs and starts it again whenever it
exits. So the old binary stays in the image and the new one is laid over it in RAM. Not run yet, read it before use, one
camera, the other one stays untouched.

1. Build: `make -C platforms/anyka-ak3918 akamd`. Check first that the camera busybox has `mount --bind` (`ssh root@<camera-ip> "mount --help 2>&1 | head -3"`); if not, use the `make upload` path below.
2. Copy it to RAM: `ssh root@<camera-ip> 'cat > /tmp/akamd.new && chmod +x /tmp/akamd.new' < build/lsc-indoor-1080p-ak3918/akamd`
   and check `ssh root@<camera-ip> 'sha256sum /tmp/akamd.new'` against the PC.
3. Lay it over the old one and restart the video service (the wrapper starts the process again, now the new binary).
   Never stack binds: first check that nothing is bound yet, the answer must be 0 (if it is not, do the `umount -l` of the
   rollback below first; every extra bind hides the previous one and the old binary stays busy):
   `ssh root@<camera-ip> 'mount | grep -c akamd'`
   (on a 0.1.x image the program is `/usr/bin/ak_rtsp`: bind over that name and use `pidof ak_rtsp`)
   then `ssh root@<camera-ip> 'mount --bind /tmp/akamd.new /usr/bin/akamd && for p in $(pidof akamd); do grep -qa akam_klog /proc/$p/cmdline || kill $p; done'`
4. Check that the new binary really runs: after about 20 seconds compare the hash of the running executable with the PC's
   (`sha256sum build/lsc-indoor-1080p-ak3918/akamd`): `ssh root@<camera-ip> 'for p in $(pidof akamd); do grep -qa akam_klog /proc/$p/cmdline || sha256sum /proc/$p/exe; done'`
   (the bound file is what `/proc/<pid>/exe` shows; if the hash is the old one the restart did not pick it up). Then
   `./akam video show` and `ssh root@<camera-ip> 'cat /tmp/akam_wrapper.log; logread | grep -i "\[video\]"'`
   (the log line `[video] settings: ... qp=28..43` proves the new binary runs).
5. Set the QP, for example `./akam video minqp 22 maxqp 40` (bash wizard only; the Go program's `video` has no `minqp` or `maxqp` for the 1080p) (answer yes to the restart), then capture 20 s with
   `ffmpeg -rtsp_transport tcp -i rtsp://<camera-ip>:554/ -t 20 -c copy after.mkv` and compare with
   `tools/video_quality.sh before.mkv` / `after.mkv` (capture `before.mkv` first, same scene).
6. Rollback, any of these (each is enough):
   - back to the defaults: `./akam video minqp 28 maxqp 43` (bash wizard) (or `ssh root@<camera-ip> rm /etc/config/akam/video`, then restart);
   - old binary: a plain `umount /usr/bin/akamd` fails ("busy") while akamd runs from it, so detach lazily and then kill,
     the wrapper starts the flashed binary: `ssh root@<camera-ip> 'umount -l /usr/bin/akamd; for p in $(pidof akamd); do grep -qa akam_klog /proc/$p/cmdline || kill $p; done'`,
     then check `mount | grep -c akamd` prints 0 (repeat the `umount -l` while it does not) and the hash of `/proc/<pid>/exe` is the flashed one;
   - power cycle: the bind mount and `/tmp` are gone, the flashed image runs. A kernel oops while the encoder opens does
     the same by itself (the camera reboots into the flashed image; remove `/etc/config/akam/video` if it keeps
     failing, the file survives reboots). Never save an untested QP pair for long: min must stay below max (enforced).

`make upload` + `start_rtsp.sh` (steps above) is the other path, with a card in the camera and the stock watchdog stopped.

### Install a new 1080p `akamd` permanently on a camera that already runs Akam

`/usr/bin/akamd` lives in the APP squashfs (`mtd7`, read-only), so a permanent install means writing a new APP image. There is no
binary-only update. Nothing below was run for a development build; read it first, one camera at a time.

What exists:

1. **Update over SSH, no card, no UART** (the supported way): build an image that contains your binary, then `./akam update --image FILE`
   (bash wizard or Go assistant; the Go assistant also takes `--kit FILE` and `--version X.Y.Z` for releases). It checks the image
   (5 242 880 bytes, squashfs), asks one red yes/no (default No), refuses without a verified stock backup, copies everything to RAM,
   writes `mtd7` from a detached worker with 3 write-and-verify attempts, and needs two read-backs equal to the image md5 before it says
   done ([installer spec](../specs/installer.md) 7.2). Then unplug and replug the camera by hand. Over SSH it keeps a second Dropbear
   on port 2222 during the write.
2. **Building the image**: `AK_RTSP_BIN=build/lsc-indoor-1080p-ak3918/akamd APP_EXTRACTED=<your unsquashed stock APP> FW_DIR=<outside the repo> bash firmware/lsc-indoor-1080p-ak3918/image/build_image.sh`
   ([build guide](manual/build.md) section 7). It needs your own stock APP backup (nothing vendor ships in the repository). The kit route
   (`tools/release/make_kit.sh`, then `./akam step image`) builds the same image from a kit plus your backup.
3. **Card route** if SSH does not come back: `./akam flash` or `install_with_flash_tool.sh` with the image on the toolkit card
   ([flash guide](manual/flash-and-restore.md)).
4. **Rollback**: (a) before flashing, try the binary in RAM with the bind-mount procedure above: a power cycle undoes it;
   (b) after flashing, keep the previous image file and run `./akam update --image <previous>.squashfs` (same gates); (c) back to the
   original firmware: `./akam update --stock` or `./akam restore` from your verified backup (never rehearsed on a camera);
   (d) a camera that does not boot: UART or the maintenance telnet, then `restore` ([flash guide](manual/flash-and-restore.md)).
   Settings in `/etc/config/akam/` are on CONFIG and survive every APP write, including the new `isp.denoise` line of the `picture` file.

What is missing:

- A **dev-build update command**: `update --image` works, but nothing builds the image from `build/lsc-indoor-1080p-ak3918/akamd` for you in one step
  (`build_image.sh` by hand, or a kit). The Go assistant's native builder is experimental and the bash `update` refuses
  `--version` and `--kit`.
- A **binary-only or A/B slot update**: `akam_run.sh` starts `${AKAM_RTSP_BIN:-/usr/bin/akamd}` and `AKAM_RTSP_BIN` is only a test
  hook, so there is no persistent override from CONFIG or the card, and a bad binary is replaced only by another full APP write.
- An **automatic fallback**: no watchdog-driven return to the previous image; the wrapper only restarts a crashing binary with a back-off.
- Hardware evidence: the bash wizard's first installs, updates over SSH and verify ran on two cameras; the Go assistant's update, restore
  and SD writer are host-tested only; `restore` to stock was never rehearsed ([assistant](assistant.md)).

## Tests

```bash
tools/ci/local_check.sh             # shellcheck, every tests/*_test.sh, Go tests, file hygiene
bash tests/readme_links_test.sh     # documentation links and ./akam commands only
bash tests/swdog_test.sh            # one test alone
cd tools/assistant && go test ./... # the Go program
```

Do not run two `local_check.sh` at once: some tests use fixed local ports. Without the bootstrap, tests that need the
pinned BusyBox or compiler print `skip:`. What each test covers: [testing status](testing-status.md).

## Hardware access

### Photos of the board

| Front (on stand) | Back (on stand) | Back (capsule removed) |
|:---:|:---:|:---:|
| ![Assembled front](../images/6.jpg) | ![Assembled back](../images/7.jpg) | ![Capsule back](../images/8.jpg) |
| Fully assembled camera. | Speaker grille, USB-C port, reset button, labels. | The labels with the article number and SI code. |

| PCB back | PCB front |
|:---:|:---:|
| ![PCB back](../images/2.jpg) | ![PCB front](../images/3.jpg) |
| SoC (centre), microSD slot, USB-C port, reset switch, UART pads. | Lens, IR-cut connector, status LED, SPI flash. |

![Flash chip](../images/4.jpg)

The 8 MiB SPI flash (`XM25QH64D`). An external SPI programmer (for example a CH341A) can read or rewrite it when
everything else fails.

### Opening the camera

The housing clips together. Do not pry the bezel from outside.

1. Detach the camera body from its mounting base.
2. Drill a 6 mm hole in the bottom of the white outer shell, through the plastic only (it stays hidden once
   remounted).
3. Push a long thin screwdriver through the hole against the back of the front faceplate until the clips release.
4. Unclip the faceplate board, then the speaker connector behind the main board. The main board can then be unscrewed.

| Drill hole | Push faceplate |
|:---:|:---:|
| ![Hole](../images/9.jpg) | ![Screwdriver](../images/10.jpg) |

### UART access

Four unpopulated pads next to the SoC: **TX**, **RX**, **GND** and 3V3 (not needed).

![UART pads](../images/5.jpg)
![UART pads with Kapton tape](../images/11.jpg)

| Parameter | Value |
|---|---|
| Speed | 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control |
| Voltage | **3.3 V only. Never connect a 5 V adapter** |

Use any 3.3 V USB-UART adapter and any terminal (`picocom -b 115200 /dev/ttyUSB0`, PuTTY, minicom). Cross the wires:
adapter RX to camera TX, adapter TX to camera RX, and join the grounds. Use thin enamel or silicone wire and Kapton tape
to avoid tearing the pads.

U-Boot only waits about one second for a key at power-up, so catching it by hand is hard. The upstream project built an
ESP32 bridge that floods Ctrl+C when it sees the `U-Boot` banner, and a Windows serial console. Both were removed from
this repository: get them from
[the upstream repository](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware).

### A root shell from U-Boot (no password needed)

At the `AK3918AV130 #` prompt:

```sh
setenv bootargs console=ttySAK0,115200n8 root=/dev/mtdblock5 rootfstype=squashfs init=/bin/sh ${mtdparts} ${mem} ${memsize}
run read_kernel
run read_dtb
bootm 0x80008000 - 0x81300000
```

Then, at the `#` prompt:

```sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t jffs2 /dev/mtdblock6 /etc/config
```

UART is optional for Akam but it is the safety net: it works with no network, and Akam never writes the boot loader,
kernel or root file system, so this route should still work after a bad APP flash.

## Stock capture and reverse engineering

To learn how the stock application drives the speaker, the ISP or the day/night switch, `tools/hw_tests/stock_capture.sh`
runs the stock program with a recording hook on a camera that is still stock, writing nothing to flash:
[stock capture guide](manual/stock-capture.md). **A capture is vendor data: never publish or commit it**
(`isp_blocks.bin`, `stock_audio.raw` and similar files). Share only your own conclusions in words.

## Manual procedures

The wizard automates all of these. Use them to understand a step, to audit it, or when you cannot run the wizard.
Do them in this order.

1. Back up the camera: [by UART](manual/backup-uart.md) or [by microSD root shell, no UART](manual/backup-sd.md).
2. [Build the image from your dump](manual/build.md).
3. [Flash, reflash over SSH, and restore](manual/flash-and-restore.md).

Other references: [SSH keys, lockdown, maintenance flag](manual/ssh-and-lockdown.md), [the release kit](manual/kit.md).

## Contributing

See [CONTRIBUTING](../CONTRIBUTING.md): licence, sign-off, what must never be committed.
