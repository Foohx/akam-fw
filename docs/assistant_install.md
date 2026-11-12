# Installing the `akam` binary (Windows, macOS, Linux)

The `akam` binary is the single-file version of the setup wizard. It is **experimental**: on Linux the bash wizard
(`./akam` in the repository) remains the reference, and the Windows and macOS binaries are **built, not hardware-tested**
([test status](testing-status.md), [hardware test plan](assistant_hardware_test_plan.md)). What it does:
[tools/assistant/README.md](../tools/assistant/README.md).

## Files of a release

Release tag `assistant-vX.Y.Z` (separate from the kit tags). Names carry no version:

| Target | Raw binary | Archive |
|---|---|---|
| Linux amd64, arm64 | `akam-linux-amd64`, `akam-linux-arm64` | `.tar.gz` |
| macOS Intel, Apple silicon | `akam-darwin-amd64`, `akam-darwin-arm64` | `.tar.gz` |
| Windows amd64, arm64 | `akam-windows-amd64.exe`, `akam-windows-arm64.exe` | `.zip` |

Also `LICENSE`, `THIRD_PARTY_NOTICES.md`, `SHA256SUMS` and `SHA256SUMS.sig`. An archive keeps the executable bit on Unix;
a raw download does not. `akam licenses` prints the notices even for a binary downloaded alone.

## Check the download

Compare the SHA256 of your file with the line in `SHA256SUMS`:

```bash
sha256sum -c --ignore-missing SHA256SUMS        # Linux
shasum -a 256 -c --ignore-missing SHA256SUMS    # macOS
```

```powershell
Get-FileHash -Algorithm SHA256 .\akam-windows-amd64.exe    # Windows: compare by eye with SHA256SUMS
```

Optional signature check (same scheme as the kit, namespace and principal `akam-release`; the maintainer publishes the
public key with the release): `ssh-keygen -Y verify`. A binary that is not signed yet only has this check.

## Unsigned binaries

The binaries are not signed with a Windows or Apple certificate yet, so the systems warn. This is expected. Only go ahead
when the SHA256 matches the release.

**Windows (SmartScreen, "Windows protected your PC").** Click More info, then Run anyway. Or right-click the file,
Properties, tick Unblock. Or in PowerShell: `Unblock-File .\akam-windows-amd64.exe`. The UAC prompt of the SD card step says
"Unknown publisher" until the binary is signed. Microsoft Defender may flag a new unsigned program: compare the SHA256, and
report a false positive to Microsoft.

**macOS (Gatekeeper).** A file downloaded with a browser carries the quarantine attribute. Any one of:

- download with `curl -L -o akam-darwin-arm64 <url>` (no quarantine);
- `xattr -d com.apple.quarantine ./akam-darwin-arm64`;
- Control-click the file, Open, once;
- System Settings, Privacy & Security, Allow Anyway.

Then `chmod +x ./akam-darwin-arm64` for a raw download. The arm64 binary carries the ad-hoc signature that the kernel
requires: do not repack or modify it.

**Linux.** `chmod +x ./akam-linux-amd64`.

## First run

```text
./akam version     # shows the version and GOOS/GOARCH
./akam doctor      # what works on this computer; changes nothing
./akam             # home screen
```

### Audio: what is built in

`./akam say FILE` and `./akam talk` need no external program for audio on Linux and Windows:

| | Linux | Windows | macOS |
|---|---|---|---|
| `say`: WAV, MP3, FLAC, Ogg Vorbis files | built in (found by content, not by file name; any rate or channel count) | built in | built in |
| `talk`: live microphone | built in: PulseAudio protocol client, also served by PipeWire (`pipewire-pulse`) | built in: WASAPI, shared mode, default capture device | needs `ffmpeg` (CoreAudio cannot be used without cgo) |
| Other file formats for `say` | optional `ffmpeg` | optional `ffmpeg` | optional `ffmpeg` |
| Microphone fallback | `ffmpeg` or `arecord` when no PulseAudio/PipeWire server is reachable | `ffmpeg` if WASAPI fails | - |

The microphone source is chosen in this order: `AKAM_TALK_CAPTURE_CMD` (your own command, raw 8 kHz mono s16le on stdout),
the built-in backend, `ffmpeg`, `arecord`. `./akam doctor` prints an `Audio:` line with what will be used. Files longer than
30 minutes are refused by the built-in decoders. On Windows the first use may trigger the microphone privacy prompt
(Settings, Privacy, Microphone, "Let desktop apps access your microphone").

`doctor` names the package of each optional tool for your system (for example `alsa-utils` for `arecord`). The squashfs tools
have no native Windows package: use `--builder native` (experimental, needs none) or WSL.

The camera options screen (`./akam`, item 3) also shows the camera's Wi-Fi signal and clock. A weak or poor signal (below
-70 dBm) prints advice (move the camera or the access point, or lower the bitrate and key frame interval with
`./akam video bitrate 1024 gop 1`); nothing is changed for you. If the clock is unset (the camera has no battery clock) or off
by more than 2 minutes it offers to set it from this computer (default No; lost at the next reboot unless time sync is on:
`./akam ntp set pool.ntp.org`, or a server on your network). The end of a first install or update offers time sync too (default No).

On Windows and macOS, the first install needs an image built on Linux or in WSL until the native builder is released
(`--image FILE`, see the README of the assistant). Writing a card asks for administrator rights: see "Writing the SD card,
per OS" there.
