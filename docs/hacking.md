# Hacking guide: build, test and hack your camera

For developers and tinkerers. To just install Akam, use `./akam` ([README](../README.md)).

## Repository layout

```text
.
├── akam                          # the bash wizard (Linux)
├── docs/                         # documentation (index: docs/README.md)
├── images/                       # photos of the camera and its board
├── specs/                        # as-built specification of the code
├── src/
│   ├── ak_rtsp/                  # the camera program, flash_tool, Makefile, build_firmware.sh
│   └── common/                   # code shared by the camera binaries (time32 shim)
├── tests/                        # host tests (shell and C, fake cameras, synthetic fixtures)
└── tools/
    ├── akam-talk/                # host client that sends sound to the speaker
    ├── assistant/                # the Go program `akam` (Windows, macOS, Linux)
    ├── build/                    # bootstrap.sh (pinned toolchain), check_host.sh, env.sh, deps.lock
    ├── ci/                       # local_check.sh, distro_check.sh
    ├── dropbear/                 # Dropbear SSH build and options
    ├── firmware_patch_templates/ # camera-side scripts and build-time patches
    ├── flash_scripts/            # flash, restore, reflash over SSH, start_rtsp.sh
    ├── hw_tests/                 # akam_hwcheck.sh, ONVIF check, stock capture tools
    ├── image/                    # assemble the image from the kit
    ├── release/                  # kit, signing, release scripts and notes template
    ├── sd/                       # microSD preparation (Linux)
    └── wizard/                   # libraries of the bash wizard, a fake camera for tests
```

| Component | What it does | Where |
|---|---|---|
| `ak_rtsp` | RTSP/RTP H.264 and audio server, control port 8091, ONVIF, MQTT | `src/ak_rtsp/` |
| Dropbear 2026.94 | Static SSH server, ed25519 keys only | `tools/dropbear/` |
| Camera scripts | SSH start, lockdown, hostname, password, debug, ONVIF, NTP, MQTT, Wi-Fi supervisor | `tools/firmware_patch_templates/` |
| Image builder | Patches **your own** APP dump, repacks, pads to 5 242 880 bytes | `src/ak_rtsp/build_firmware.sh`, `tools/image/` |
| Release kit | Prebuilt Akam binaries and scripts, signed | `tools/release/` ([kit](manual/kit.md)) |

## Build from source

Everything runs on a PC and never touches the camera. Arch Linux is the reference host; Debian 12 and Ubuntu 24.04 are
checked in containers.

```bash
bash tools/build/check_host.sh      # checks your PC, prints the exact install command if something is missing
bash tools/build/bootstrap.sh       # once: pinned Zig, SmolRTSP, BusyBox, Go modules
. tools/build/env.sh
make -C src/ak_rtsp ak_rtsp flash_tool
bash tools/dropbear/build_dropbear.sh
```

The result is a static ARM binary (`arm-linux-musleabi`, `-mcpu=arm926ej_s`). To build a flashable image from your own
APP dump: [build guide](manual/build.md).

## Run a development build in RAM

You can test a new `ak_rtsp` without flashing anything. A power cycle brings back what was flashed.

1. Get SSH access (an Akam camera) or the maintenance shell, with a microSD card in the camera (mounted on `/mnt`).
2. Build, then upload the binaries and scripts to `/tmp` over SSH: `make -C src/ak_rtsp upload CAMERA_IP=<camera-ip>`.
3. Copy `ak_rtsp` to the card as `/mnt/ak_rtsp` (`cp /tmp/ak_rtsp /mnt/`): `tools/flash_scripts/start_rtsp.sh`
   starts the binary from `/mnt`, and writes its log and a live `dmesg` copy there too.
4. On the camera run `sh /tmp/start_rtsp.sh`. It stops the stock watchdog, the running `ak_rtsp` (or `anyka_ipc` on a
   stock camera), reloads the encoder modules and starts the new binary.
5. Watch the stream with `ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/`, and the log on the UART or the card.

Because the stock watchdog is stopped, the hardware watchdog may reboot the camera after a while: that is expected.

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
