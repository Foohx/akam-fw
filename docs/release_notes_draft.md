# Release notes draft: assistant v1.0 (`assistant-v1.0.0`)

Draft. Not published. Fill in the tag, the date, the release public key and the SHA256 values at release time.

## What this is

`akam` is a single static program for Linux, Windows and macOS that guides you through putting Akam on a supported camera and
managing it afterwards. It is **experimental**. On Linux the bash wizard (`./akam` in the repository) is still the reference.
Nothing is sent to your network or written to a card or a camera without a yes (default No), and the program never
reboots or power-cycles a camera.

## What is in it

- A home screen: set up a new camera, update a camera (Akam to Akam, over SSH, no card), camera options, back up and restore,
  find cameras, settings, and a check of this computer.
- A camera chooser: remembered cameras, name lookup, ONVIF WS-Discovery and a scan of your /24 on the four camera ports (each
  asks first).
- Option screens for LED and night, picture, video, audio, network (ONVIF, NTP) and security, with what is saved and when it applies.
- Backup in `<Documents>/Akam backups/<camera>/<date>`, private, with a warning about cloud folders; over SSH for an Akam
  camera, over telnet and the toolkit card for a stock camera. A verified stock backup gates every flash.
- First install from a stock camera and update, with the card written by the program itself on all three systems.
- `akam licenses` and an OS-aware `akam doctor`.
- The kit download and its checksum and signature checks, built in.

## Platform status

| Target | File | Status |
|---|---|---|
| Linux amd64 | `akam-linux-amd64` | partly tested on a real camera; the bash wizard is the reference |
| Linux arm64 | `akam-linux-arm64` | built, not hardware-tested |
| Windows 11 amd64 | `akam-windows-amd64.exe` | built, not hardware-tested |
| Windows arm64 | `akam-windows-arm64.exe` | built, not hardware-tested |
| macOS arm64 | `akam-darwin-arm64` | built, not hardware-tested |
| macOS amd64 | `akam-darwin-amd64` | built, not hardware-tested |

"Built, not hardware-tested": it compiles, passes the host tests of its operating-system layers (on fake system facts, parsers and
pipes) and is reproducible, but nobody has run it on that system with a real camera or card yet. Testers: see the
[hardware test plan](assistant_hardware_test_plan.md); a platform becomes "tested" when its whole table passes.

## Known limits

- **Image on Windows and macOS.** The native image builder is not part of this release. On Windows and macOS you import an
  image built on Linux or in WSL (`--image FILE`, 5 242 880 bytes). On Linux `image` stays in the bash wizard.
- **Unsigned binaries.** SmartScreen and Gatekeeper warn; the steps are in [assistant_install.md](assistant_install.md). The UAC
  prompt says "Unknown publisher".
- An SSH backup of an Akam camera is not a way back to stock: only a backup made from the stock camera is.
- Wi-Fi is shown read-only; changing it needs the card route.
- Microphone capture for `talk` needs `ffmpeg` (or `arecord` on Linux).
- A flash can brick a camera if the power is cut during the write. Keep a backup on two media.

## Verify your download

`SHA256SUMS` and `SHA256SUMS.sig` (namespace and principal `akam-release`) are attached. Release public key: `<to be added before the
first signed release>`. Steps per OS: [assistant_install.md](assistant_install.md).

## Camera side (from the hardware tests of 2026-11-11, akam-01)

Image with ONVIF, encoder-stall watchdog, Wi-Fi supervisor, opt-in NTP, video config and quieter logs: clean boot, ports 22, 554 and
8091 only by default, ONVIF validated with the check tool (34 of 35 checks, the failure was a tool bug since fixed). ONVIF Device
Manager on Windows is not yet tested. Details: [testing status](testing-status.md).
