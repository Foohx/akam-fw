# Compatible cameras

**Will Akam run on my camera?** Read this page before you flash anything.

Akam has run on real cameras of **one** model: the LSC Smart Indoor 1080p, article `3215672.3` (two units). Everything
else is untested, and a camera with a different flash layout is refused by the installer. What Akam does on the
supported camera: [features](features.md).

## Is my camera compatible?

1. Find the **article number** and the **SI code**: look at the back of the camera body, under the LSC logo, for two
   codes such as `3215672.3  SI D26228`. Photos: [hacking guide](hacking.md#photos-of-the-board); the photo
   [images/8.jpg](../images/8.jpg) shows a `3215672.2` label.
2. Look for the article number in the table below.
3. Run `./akam`. Its `backup` and `image` steps show a verdict from
   [`tools/release/supported_hardware.txt`](../tools/release/supported_hardware.txt). The verdict is decided by the
   **sha256 of your stock APP partition**: a match with a validated line continues; anything else is "untested" (a
   warning and an explicit yes). The kernel line is compared and only warns. The flash layout is checked separately:
   the whole 8-line partition table (`/proc/mtd`) must match exactly, otherwise the wizard refuses. This is a safety
   net, not a guarantee.

If you are not sure, do not flash. Make the backup (it changes nothing on the camera) and report what the wizard says
([Report your camera](#report-your-camera)).

## Cameras

Legend: validated = an Akam image was flashed and ran on real units. Untested = may work, may brick. Not supported =
not expected to work.

| Status | Camera | Article no. and SI code | Notes |
|---|---|---|---|
| validated | LSC Smart Indoor 1080p (sold at Action) | `3215672.3`, SI `D26228` | 2 units tested, see below |
| untested | LSC Smart Indoor 1080p (sold at Action) | `3215672.2`, SI `B26101` / `C26101` | The upstream project ran its `ak_rtsp` on a unit of this model. Akam's installer and image were never run on it |
| not supported | Other Anyka or Tuya cameras | any | `ak_rtsp` is specific to the AK3918AV130 SoC and the GC20C3 sensor. Older rectangular boards (AK3918EV200, GK7102) use other register maps and layouts. The wizard warns and refuses a different flash layout |

**The validated model**

| Item | Value |
|---|---|
| SoC and sensor | Anyka AK3918AV130, GalaxyCore GC20C3 (2 MP) |
| Board | `IPC280KG2-GNA-MAIN-V1.0` (circular PCB) |
| Flash | 8 MiB SPI NOR, 8 partitions ([flash map](how-it-works.md#flash-map)); only the APP partition (`mtd7`, 5 242 880 bytes) is rewritten |
| Stock firmware fingerprint | Linux `4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025`, APP squashfs xz with 128 KiB blocks, BusyBox 1.30.1. The sha256 of the stock APP is in `supported_hardware.txt` |
| Tested | 2 units, on a recent image: RTSP, talk, day/night, NTP, SSH and more ([testing status](testing-status.md)) |

## What identifies a model

Akam is built for one hardware design, so "same model" means all of these match:

| What | How to read it |
|---|---|
| Article number and SI code | The label on the back of the camera body |
| SoC | `cat /proc/cpuinfo` on the camera, or the markings on the chip |
| Flash layout | `cat /proc/mtd` must show the same 8 partitions and sizes as the [flash map](how-it-works.md#flash-map) |
| Kernel | `uname -a` (only a warning if different) |
| Stock APP content | The sha256 of the stock APP partition of your backup, compared with `supported_hardware.txt` |

The owner plans to support other cameras of the same brand. A new model needs a validated line in
`tools/release/supported_hardware.txt` (the APP sha256 and kernel of a real unit) and, if the hardware differs (another
sensor, another IR or LED wiring), code changes in `ak_rtsp`. Adding a line alone is not enough.

## Report your camera

Whether it works or not, a report helps, especially for a camera that is not in the table. Open an issue with:

- the article number and the code printed on the label (for example `3215672.3`, `SI D26228`);
- the SoC, from the camera: `cat /proc/cpuinfo` (over SSH on an Akam camera, or on the maintenance shell);
- the partition table: `cat /proc/mtd`, and the kernel line: `uname -a`;
- the wizard's hardware verdict, shown by `./akam backup` and `./akam image`, and the output of `./akam status`;
- on a camera that already runs Akam, the output of the read-only check script, which changes nothing:
  `ssh -i ~/.ssh/akam root@<camera-ip> 'sh -s' < tools/hw_tests/akam_hwcheck.sh` (`./akam verify` runs it for you);
- what you did, what you saw, and the end of the log in `~/.local/state/akam/` (redacted).

**Never attach your backup, dumps, images or keys**, and remove Wi-Fi names, passwords, MAC addresses and IP addresses
from anything you post. **Always take a full backup first** and keep it on two media
([backup](manual/backup-uart.md)).
