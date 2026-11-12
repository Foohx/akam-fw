# Akam 0.1.0

Draft. Not published. Fill in `<date>`, `<sha256>` and `<key>` at release time (tag `v0.1.0`).

First public release of Akam: open firmware add-ons for Anyka AK3918 IP cameras, and `akam`, the assistant that
installs and manages them. It is **experimental**. Read the [kit documentation](akam_v1_kit.md) and keep a
verified backup of your stock camera before you flash anything: a flash can brick a camera if the power is cut
during the write.

One release holds both parts, with one `SHA256SUMS` and one signature:

- `akam-kit-0.1.0.tar.xz`: the camera firmware kit (Akam's own files only; it never contains a camera image).
- `akam-<os>-<arch>[.exe]` and archives: the assistant for six targets.
- `akam-0.1.0-src.tar.gz`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `SHA256SUMS`, `SHA256SUMS.sig`.

Released on `<date>`.

## Supported hardware

| Status | Device | Fingerprint |
|---|---|---|
| validated | LSC Smart Indoor 1080p 3215672.3 (board SI D26228), AK3918AV130 | kernel `4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025`; 8 MTD partitions, mtd7 = APP (`00500000`); stock APP squashfs xz, block 128 KiB, no BCJ; stock APP sha256 `14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294`; BusyBox 1.30.1 |
| untested | LSC Smart Indoor 1080p 3215672.2 (boards SI B26101 / C26101) | never run with Akam |
| untested | any other camera, kernel build or stock APP | unknown |

Validated on two cameras of the first model. Anything not marked validated may not work or may brick your camera.
The setup wizard warns and asks for an explicit confirmation; a different MTD layout is refused.

## Camera firmware kit

- **Video and audio over RTSP**: H.264 1080p with microphone, at **10, 15, 20 or 25 fps** (default 15), bitrate and GOP
  settings. One client at a time (a second one gets 503); put go2rtc in front for several viewers. Backpressure on
  RTSP/TCP for weak Wi-Fi.
- **Stream authentication**: optional RTSP Digest login, off by default.
- **Two-way audio**: the ONVIF RTSP audio backchannel plays sound on the camera speaker.
- **ONVIF Profile S, opt-in** (off by default): device, media and imaging services, WS-Discovery. Checked against
  the check tool in `tools/hw_tests/` and with ONVIF Device Manager on Windows.
- **MQTT with Home Assistant discovery, opt-in** (off by default): **host-tested only**, never run on a camera or
  against a real broker.
- **Day/night switching**, front LED and IR LED settings, picture settings.
- **Robustness**: an encoder-stall watchdog, a Wi-Fi supervisor, opt-in NTP, capped logs, and a **kernel log
  filter** (`klog`) that drops the vendor LED chatter (not yet measured over a long run).
- **Security by default**: SSH keys only, Telnet and FTP off; only ports 22, 554 and 8091 listen, 8091 is read-only
  from the network.

Per-feature evidence (host test, RAM test, hardware): [testing status](testing-status.md).

## Assistant (`akam`)

A single static program that guides you through putting Akam on a supported camera and managing it afterwards:
a full-screen home screen, a camera chooser (remembered cameras, name lookup, ONVIF discovery, /24 scan, each asks
first), option screens (LED and night, picture, video, audio, network, security), `talk` to the camera speaker,
backup and restore (SSH for an Akam camera; telnet and the toolkit card for a stock camera, with a verified stock
backup required before any flash), first install and update, `akam licenses`, `akam doctor`, and a built-in kit
download with checksum and signature checks. Nothing is written to a card or a camera without a yes (default No);
it never reboots or power-cycles a camera.

| Target | File | Status |
|---|---|---|
| Linux amd64 | `akam-linux-amd64` | tested on a real camera |
| Windows amd64 | `akam-windows-amd64.exe` | tested on real hardware (new full-screen interface, options, talk, backup) |
| Linux arm64 | `akam-linux-arm64` | built, not hardware-tested |
| macOS arm64, amd64 | `akam-darwin-arm64`, `akam-darwin-amd64` | built, not hardware-tested |
| Windows arm64 | `akam-windows-arm64.exe` | built, not hardware-tested |

"Built, not hardware-tested": it compiles, passes the host tests of its operating-system layers and is reproducible,
but nobody has run it on that system with a real camera or card. Testers are welcome: see the
[hardware test plan](assistant_hardware_test_plan.md).

## Known limits

- **Unsigned binaries.** The binaries carry no Windows or Apple certificate: SmartScreen, Gatekeeper and the UAC
  prompt ("Unknown publisher") warn. Check the SHA256 first; steps in [assistant_install.md](assistant_install.md).
- **Image building on Windows and macOS.** The native image builder is not part of this release: import an image
  built on Linux or in WSL (`--image FILE`). On Linux the bash wizard (`./akam` in the source) stays the reference.
- An SSH backup of an Akam camera is not a way back to stock; only a backup taken from the stock camera is.
- Wi-Fi is shown read-only; changing it needs the card route.
- Microphone capture for `talk` needs `ffmpeg` (or `arecord` on Linux).
- MQTT, the klog filter, the encoder-stall watchdog and several settings are host-tested only or not exercised on
  hardware yet ([testing status](testing-status.md)).

## Verify your download

`SHA256SUMS` lists every file of the release; `SHA256SUMS.sig` signs it (SSH signature, namespace and principal
`akam-release`).

- Kit sha256: `<sha256>` (`akam-kit-0.1.0.tar.xz`)
- Release public key: `<key>` (also in `tools/release/allowed_signers` and the README)

```sh
sha256sum -c --ignore-missing SHA256SUMS          # Linux; macOS: shasum -a 256 -c --ignore-missing SHA256SUMS
tools/release/verify_kit.sh akam-kit-0.1.0.tar.xz  # kit: checksum, signature, manifest (from the source tarball)
```

`akam get-kit` does these checks itself. Steps per OS: [assistant_install.md](assistant_install.md).
