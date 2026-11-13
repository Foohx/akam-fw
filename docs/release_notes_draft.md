# Akam <version>

Draft. Not published. `tools/release/make_release.sh` writes the filled copy (`<version>`, `<sha256>`, `<date>`, `<key>`, absolute links) next to the release directory.

First public release of Akam: open firmware add-ons for Anyka AK3918 IP cameras, and `akam`, the assistant that
installs and manages them. It is **experimental**. Read the [kit documentation](akam_v1_kit.md) and keep a
verified backup of your stock camera before you flash anything: a flash can brick a camera if the power is cut
during the write.

One release holds both parts, with one `SHA256SUMS` and one signature:

- `akam-kit-<version>.tar.xz`: the camera firmware kit (Akam's own files only; it never contains a camera image).
- `akam-<os>-<arch>[.exe]` and archives: the assistant for six targets.
- `akam-<version>-src.tar.gz`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `SHA256SUMS`, `SHA256SUMS.sig`.

Released on <date>.

## Supported hardware

See also the [compatible cameras](compatible-cameras.md) page for the full table and the stock-versus-Akam comparison.

| Status | Device | Fingerprint |
|---|---|---|
| validated | LSC Smart Indoor 1080p 3215672.3 (board SI D26228), AK3918AV130 | kernel `4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025`; 8 MTD partitions, mtd7 = APP (`00500000`); stock APP squashfs xz, block 128 KiB, no BCJ; stock APP sha256 `14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294`; BusyBox 1.30.1 |
| untested | LSC Smart Indoor 1080p 3215672.2 (boards SI B26101 / C26101) | never run with Akam |
| untested | any other camera, kernel build or stock APP | unknown |

Validated on two cameras of the first model. Anything not marked validated may not work or may brick your camera.
The setup wizard warns and asks for an explicit confirmation; a different MTD layout is refused.

## Camera firmware kit

- **Video and audio over RTSP**: H.264 1080p with microphone at **10, 15, 20 or 25 fps** (default 15), with bitrate
  and GOP settings. One client at a time (a second one gets 503); put go2rtc in front for several viewers.
  Backpressure on RTSP/TCP for weak Wi-Fi.
- **Stream authentication**: optional RTSP Digest login, off by default.
- **Two-way audio**: the ONVIF RTSP audio backchannel plays sound on the camera speaker.
- **ONVIF Profile S, opt-in** (off by default): device, media and imaging services, WS-Discovery. Implemented and checked
  with a check tool and with ONVIF Device Manager on Windows; **not conformance-certified**.
- **MQTT with Home Assistant discovery, opt-in** (off by default): **host-tested only**, never run on a camera or
  against a real broker.
- **Day/night switching**, front LED and IR LED settings, picture settings.
- **Robustness**: an encoder-stall watchdog, a Wi-Fi supervisor, opt-in NTP, capped logs, and a **kernel log
  filter** that drops the vendor LED chatter.
- **Security by default**: SSH keys only, Telnet and FTP off; only ports 22, 554 and 8091 listen, 8091 is read-only
  from the network.

### What was validated on which build

- **Image "v6"** (klog filter, RTSP/TCP backpressure, ONVIF, watchdog, NTP, video config), flashed on two cameras on
  2026-11-12 and run with a home server pulling RTSP: 14.9 to 15 fps, TCP send queue 0 to 24 KB, 0 encoder errors. The
  klog filter works on hardware: syslog about 460 KB a day, about 1.5 days kept in RAM.
- **Image "v7"** (adds 20 and 25 fps), flashed on one camera on 2026-11-12: 25 fps configured, night mode correctly drops
  to 10 fps, 0 encoder errors. A RAM test delivered exactly 15, 20, 25 and 30 fps. On 2026-11-13 in daylight the flashed camera delivered exactly 25 fps (751 frames in 30 s), about 1.4 Mbit/s, 0 encoder errors and 0 skipped frames, with a picture as bright as at 15 fps.
- **MQTT**: host-tested only. **Native image builder** (`akam image --builder native`): experimental, opt-in, never flashed.

Per-feature evidence: [testing status](testing-status.md).

## Assistant (`akam`)

A single static program that guides you through putting Akam on a supported camera and managing it afterwards:
a full-screen home screen, a camera chooser (remembered cameras, name lookup, ONVIF discovery, /24 scan, each asks
first), option screens (LED and night, picture, video, audio, network, security), `talk` to the camera speaker,
backup and restore (SSH for an Akam camera; telnet and the toolkit card for a stock camera, with a verified stock
backup required before any flash), first install and update, `akam licenses`, `akam doctor`, and a built-in kit
download with checksum and signature checks. Nothing is written to a card or a camera without a yes (default No);
it never reboots or power-cycles a camera. The microphone for `talk` is built in on Linux (PulseAudio or PipeWire)
and Windows (WASAPI); only macOS needs `ffmpeg`.

| Target | File | Status |
|---|---|---|
| Linux amd64 | `akam-linux-amd64` | partly tested on real cameras: SSH, 8091 options, `video show`, `led`, `ntp`, `talk`. Update, restore, telnet backup and flash, the SD writer and first install were never run on hardware from this program |
| Windows amd64 | `akam-windows-amd64.exe` | an earlier build was tested on a real camera on 2026-11-12 (discovery, options over SSH, talk with the PC microphone, clock offer); the new full-screen interface build is pending its Windows test; SD writer and flash not tested |
| Linux arm64 | `akam-linux-arm64` | built, not hardware-tested |
| macOS arm64, amd64 | `akam-darwin-arm64`, `akam-darwin-amd64` | built, not hardware-tested |
| Windows arm64 | `akam-windows-arm64.exe` | built, not hardware-tested |

"Built, not hardware-tested": it compiles, passes the host tests of its operating-system layers and is reproducible,
but nobody has run it on that system with a real camera or card. Testers are welcome: see the
[hardware test plan](assistant_hardware_test_plan.md).

## Known limits

- **Unsigned binaries.** The binaries carry no Windows or Apple certificate: SmartScreen, Gatekeeper and the UAC
  prompt ("Unknown publisher") warn. Check the SHA256 first; steps in [assistant_install.md](assistant_install.md).
- **Image building on Windows and macOS.** The native image builder is not part of the default path: import an image
  built on Linux or in WSL (`--image FILE`). On Linux the bash wizard (`./akam` in the source) stays the reference.
- An SSH backup of an Akam camera is not a way back to stock; only a backup taken from the stock camera is.
- Wi-Fi is shown read-only; changing it needs the card route.
- MQTT, the encoder-stall watchdog and several settings are host-tested only or not exercised on hardware yet
  ([testing status](testing-status.md)).

## Verify your download

`SHA256SUMS` lists every file of the release; `SHA256SUMS.sig` signs it (SSH signature, namespace and principal
`akam-release`).

- Kit sha256: `<sha256>` (`akam-kit-<version>.tar.xz`)
- Release public key: `<key>` (also in `tools/release/allowed_signers` and the README)

```sh
echo 'akam-release namespaces="akam-release" <key>' > allowed_signers
ssh-keygen -Y verify -f allowed_signers -I akam-release -n akam-release -s SHA256SUMS.sig < SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS       # macOS: shasum -a 256 -c --ignore-missing SHA256SUMS
```

GitHub's automatic "Source code" archives are not covered by the signature: use `akam-<version>-src.tar.gz`.
`akam get-kit` verifies the kit itself; `tools/release/verify_kit.sh` needs GNU tools (Linux). Steps per OS:
[assistant_install.md](assistant_install.md).
