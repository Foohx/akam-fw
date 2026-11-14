# How Akam works on this camera

For curious users and hackers. You do not need this page to install Akam. The exact values (ports, limits, file
formats) are in [`specs/`](../specs/README.md); this page explains the picture and links there.

## The idea in one minute

The camera runs Linux from an 8 MiB flash chip. The stock firmware splits that chip in 8 partitions. One of them, the
APP partition, holds the vendor application (the Tuya cloud client, `anyka_ipc`). Akam replaces **only that
partition**. The boot loader, kernel, root file system and drivers stay stock, so the camera boots exactly as before.
Only the last step changes: the stock watchdog starts Akam's `ak_rtsp` instead of the Tuya client.

## Hardware

| Item | Value |
|---|---|
| Camera | LSC Smart Indoor 1080p (Action), circular board `IPC280KG2-GNA-MAIN-V1.0` |
| SoC | Anyka AK3918AV130: ARM926EJ-S at 400 MHz, single core, no FPU, no NEON |
| RAM | 64 MiB, about 40 MiB usable by programs |
| Flash | 8 MiB SPI NOR (`XM25QH64D` or a Winbond equivalent) |
| Image sensor | GalaxyCore GC20C3, 2 MP, on the I2C bus |
| Wi-Fi | Altobeam ATBM6012BX (`atbm6x3x`) on a USB link, 2.4 GHz |
| Audio | Microphone and speaker through the vendor `ak_pcm` driver (`/dev/pcmC0D0c`, `/dev/pcmC0D0p`) |
| IR | IR-cut filter on a solenoid (GPIO 65 and 66), IR LEDs on GPIO 70 |
| Front LED | Blue, on or off only (`/sys/class/leds/state_led`) |
| Kernel | Linux 4.4.302 (vendor build), BusyBox 1.30.1, no clock chip |

The AK3918AV130 SoC integrates an ARM926EJ-S CPU, an image signal processor (ISP), a hardware video encoder, audio
codecs, USB, SD/MMC, SPI and GPIO blocks. Older LSC boards used other chips (AK3918EV200, GK7102) with other
register maps and flash layouts: Akam does not support them.

## Flash map

| Partition | Offset | Size | Type | Akam |
|---|---|---|---|---|
| `UBOOT` (mtd0) | `0x000000` | 200 KB | raw | never written |
| `ENV`, `ENVBK` (mtd1, mtd2) | `0x032000`, `0x033000` | 4 KB each | raw | never written |
| `DTB` (mtd3) | `0x034000` | 64 KB | raw | never written |
| `KERNEL` (mtd4) | `0x044000` | 1536 KB | uImage | never written |
| `ROOTFS` (mtd5) | `0x1c4000` | 1008 KB | SquashFS (xz) | never written |
| `CONFIG` (mtd6) | `0x2c0000` | 256 KB | JFFS2, read-write, on `/etc/config` | written at run time (settings, keys) |
| `APP` (mtd7) | `0x300000` | 5120 KB | SquashFS (xz), on `/usr` | **replaced by the Akam image** (exactly 5 242 880 bytes) |

The installer compares the camera's whole partition table with this one and refuses a different layout.

## What Akam replaces, what stays stock

| Stock | With Akam |
|---|---|
| `anyka_ipc` (Tuya application: cloud, video, audio, IR, LED) | Removed. Replaced by `ak_rtsp` |
| Process list `_ht_process.ini` | One entry: `ak_rtsp_wrapper.sh` |
| `service.sh` (starts Telnet, FTP and SD card hooks) | Patched at build time: Telnet and FTP lines disabled |
| `ethernet.sh` | Patched: the DHCP host name comes from `akam_hostname.sh` |
| `klogd` | Replaced by a filtered kernel log forwarder (`ak_rtsp klog`) |
| `cmdserver`, `msgserver` | **Kept.** Local helpers, not cloud programs; removing them stalls the boot |
| Kernel, drivers (`ak_isp`, `sensor_gc20c3`, `ak_pcm`, Wi-Fi), hardware watchdog | Stock, untouched |

Akam adds to the image: `ak_rtsp` (about 130 KB), Dropbear SSH (about 210 KB) and a handful of small shell scripts.
The vendor files in the image come from **your own backup** at build time.

## Boot sequence

1. U-Boot loads the kernel; the kernel mounts `ROOTFS`; stock `init` runs.
2. Stock scripts mount `APP` on `/usr` and `CONFIG` on `/etc/config`, load the sensor, ISP, encoder and audio
   drivers, and run the (patched) `service.sh`.
3. The stock `watchdog` reads `_ht_process.ini`, starts `ak_rtsp_wrapper.sh` and feeds the hardware watchdog.
4. The wrapper sets the host name, starts the helpers in the background (SSH, Telnet/FTP lockdown, Wi-Fi supervisor,
   optional NTP), brings up Wi-Fi and DHCP, then runs `ak_rtsp`. A crash is restarted with a growing delay.
5. `ak_rtsp` starts the ISP, video encoder, audio, optional ONVIF and MQTT, and the RTSP server on port 554. A failure
   of audio, ONVIF or MQTT never stops the video.

Details: [architecture](../specs/architecture.md).

## Inside `ak_rtsp`

One static ARM program (musl libc, built with Zig) does everything.

```
 GC20C3 sensor (I2C) -> ISP (24 modules) -> video input -> hardware H.264 encoder -> RTSP server (port 554)
 microphone (/dev/pcmC0D0c, 8 kHz mono) ---------------------------------------^   (L16 RTP audio track)
 RTSP backchannel (G.711) -> speaker (/dev/pcmC0D0p)
```

- **Video:** the ISP is programmed from the sensor's tuning file, exactly as the stock application does, then the
  encoder (VBR+ rate control) feeds RTP packets. Video and audio timestamps share one monotonic clock and RTCP
  Sender Reports keep them in sync. [Details](../specs/video-and-streaming.md).
- **Audio:** the microphone is raw 16-bit PCM at 8 kHz in the stream. Talk uses the ONVIF backchannel and the same
  playback device. [Details](../specs/audio.md).
- **Day/night:** a state machine watches the auto-exposure, switches the IR-cut filter and the IR LEDs, and probes the
  ambient light before going back to day. [Details](../specs/image-day-night-leds.md).
- **Control port 8091:** a text protocol for metrics and settings. [Details](../specs/control-port.md).
- **ONVIF and MQTT:** optional, cost nothing while off. [ONVIF](../specs/onvif.md), [MQTT](../specs/mqtt.md).
- **Watchdogs:** a software watchdog exits `ak_rtsp` if the encoder stalls; the wrapper restarts it.

How the code was found (UART access, `strace` of the stock application, decompiling kernel modules, then rewriting
from scratch and comparing with the traces) is the upstream project's work: see
[the upstream repository](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware).

Two findings worth knowing:

- **ISP race.** Starting capture before the auto-exposure is initialised made the kernel print "Division by zero".
  Akam initialises the AE first.
- **Audio driver.** One ioctl that looked like a query actually commits the audio configuration to the driver. Sending
  it correctly removed all audio read errors.

### Image quality

The picture is grainier than the stock firmware's in dim light, because Akam does not have the vendor's closed
dynamic tuning (white balance is a fixed, hand-calibrated set of gains). Improvements are welcome.

| | Stock (`anyka_ipc`) | Akam (`ak_rtsp`) |
|:---:|:---:|:---:|
| **Static scene** | ![Stock static scene](../images/video_out/anyka_stable.png) | ![Akam static scene](../images/video_out/ours_stable.png) |
| **Motion** | ![Stock motion](../images/video_out/anyka_movement.png) | ![Akam motion](../images/video_out/ours_movement.png) |

## Configuration storage

All Akam settings live in `/etc/config/akam/` (directory mode 0700, files mode 0600, atomic writes). A missing file
means the built-in default, so a fresh camera has no settings files. The table of files is in
[architecture, section 9](../specs/architecture.md#9-persistent-configuration). SSH keys are in
`/etc/config/dropbear/`, Wi-Fi in `/etc/config/wpa_supplicant.conf`.

A backup of `CONFIG` therefore contains your Wi-Fi password and the video password: keep backups private.

## Security model

- **SSH:** key only (ed25519), no passwords, root only. No key is ever put in the image: keys come from the microSD
  card at the first boot or from a later change over SSH. [Procedures](manual/ssh-and-lockdown.md).
- **Telnet and FTP:** disabled twice, at build time and by a helper that closes them if anything starts them.
- **Control port 8091:** has no login, so it is read-only from the network. Writes are accepted from the camera itself
  (loopback, used by the SSH helper scripts). **Debug mode** (`./akam debug on`, 60 minutes by default, RAM only)
  temporarily lets the network write.
- **RTSP and ONVIF:** one optional user and password (Digest). Talk needs it.
- **Maintenance flag:** a microSD card holding `AKAM_MAINTENANCE` can run code as root at boot. Treat the card slot as
  physical access.
- **Network:** never expose ports 22, 554 or 8091 to the Internet. Use a VPN. [Advice](using_akam.md#security-notes).

## The clock bug (time64)

The vendor kernel has no 64-bit time system calls but answers some of them with "success" without doing anything. The
musl C library calls them first, so timeouts were garbage. Akam links a small shim that uses the 32-bit calls, and
the build fails if any binary still issues the 64-bit ones. See
[architecture, section 8](../specs/architecture.md#8-time).

## Flash writing: why `flash_tool` exists

Writing `/dev/mtdblock7` with `dd` on a running camera silently corrupts the flash: the SPI controller, the ISP video
capture and the Wi-Fi driver compete for DMA. Akam's `flash_tool` writes `/dev/mtd7` (the raw character device) in 4 KB
blocks, reads each block back, and retries. Success means the whole partition was read back and its md5 matched the
image. The software `reboot` is unreliable on this hardware, so you unplug and replug the power yourself.

## Updates and restore

- **Update (Akam to Akam):** `./akam update` flashes the new APP image over SSH from RAM, with read-back verification.
- **Restore to stock:** from your own backup, `./akam restore` writes the stock APP back. It has never been rehearsed on
  a camera, so treat it as likely but not guaranteed.
- **Recovery** if the camera does not boot or has no network: the maintenance microSD card, then the UART shell, then
  an external SPI programmer. Because the boot loader, kernel and root file system are never written, the UART route
  normally still works. Procedures: [flash and restore](manual/flash-and-restore.md#5-restore), and
  [UART access](hacking.md#uart-access).
