# The `akam` program: install, check, tested platforms

`akam` is the setup program for both cameras, one file written in Go, for Windows, macOS and Linux. In these pages `akam`
means this program: rename the download to `akam` (or type its full name, for example `./akam-linux-amd64`). Step by step for
each camera: the [install guide](install.md). Commands: `akam help`; what each does inside: [tools/assistant/README.md](../tools/assistant/README.md).

The program is **experimental**: the 1080p install, update, restore and flash from it, and every 2K command, have passed host
tests only (a fake camera, no hardware); the parts that went through a real camera are listed below. The **bash wizard**
(`./akam` in a source checkout, 1080p, Linux only) is kept until the Go program's install has run on a camera, then it will be
removed: new features go to the Go program only (it is documented in the [wizard guide](wizard_guide.md)). Never run the wizard and
the Go program at the same time (they share the state files). The Go `build` (build from source) is not in the program yet (it
tells you to use the wizard).

To run it from a source checkout: `tools/assistant/build.sh`, then `tools/assistant/bin/akam`.

## Tested platforms

The Go program (and the wizard) were developed and tested on **Arch Linux**. "Built" means the binary compiles and
passes the host tests of its operating-system layer. Nothing is called tested unless it ran against a real camera.

| Platform | Binary built | Run on a real camera | What was tested | Never tested |
|---|---|---|---|---|
| Linux amd64 (Go program) | yes | partly | SSH, the signed API (camera options), `video show`, `led`, `ntp`, `talk` | update, restore, telnet backup, flash, SD card writer, first install |
| Linux arm64 | yes | no | host tests only | everything on hardware |
| Windows amd64 | yes | an earlier build | discovery, options over SSH, talk with the PC microphone, clock offer | the current full-screen interface (test pending), SD card writer and UAC prompt, flash, first install |
| Windows arm64 | yes | no | host tests only | everything on hardware |
| macOS Intel (amd64) | yes | no | host tests only | everything on hardware |
| macOS Apple silicon (arm64) | yes | no | host tests only | everything on hardware |
| Go program, 2K commands (any OS) | yes | **no** | host tests with a fake camera and a fake card (`backup-2k`, `prepare-2k-sd`, `key-2k`, `seed-2k`, `appfs-build`, `appfs-flash`, `update`, `update --rollback`, and the settings commands). The API rows they use were checked on a 2K with a curl helper (2026-11-23); `audio beep` and `say` were implemented after that session and have not run on a camera | everything on hardware; a first install from macOS or Windows |
| Bash wizard, Linux (to be retired) | n/a (scripts) | yes | real first installs (backup, image, flash), updates over SSH, verify, on two 1080p cameras | restore to stock (never rehearsed), Windows and macOS (not supported) |

A **first install from Windows or macOS has never been done**. Until it is, install from Linux (or WSL2 for the build
steps): the Go program is less tested than the wizard on every system. Checklists for testers: [hardware test checklists](hardware-checks-assistant.md).
Evidence: [testing status](testing-status.md).

## Files of a release

The assistant has its own releases, tagged `assistant-vX.Y.Z` (the one marked "Latest" on GitHub). The camera firmware is
a separate release, `firmware-vX.Y.Z`, with its own version: one kit per camera model, the source, the licences and its own
`SHA256SUMS` and `SHA256SUMS.sig`; `akam get-kit` (1080p) or, for the 2K, `prepare-2k-sd`, `appfs-build` and `update` download and verify it for you. Names of the assistant files carry no version:

| Target | Raw binary | Archive |
|---|---|---|
| Linux amd64, arm64 | `akam-linux-amd64`, `akam-linux-arm64` | `.tar.gz` |
| macOS Intel, Apple silicon | `akam-darwin-amd64`, `akam-darwin-arm64` | `.tar.gz` |
| Windows amd64, arm64 | `akam-windows-amd64.exe`, `akam-windows-arm64.exe` | `.zip` |

Also `LICENSE`, `THIRD_PARTY_NOTICES.md`, and ONE `SHA256SUMS` (all files of the release) with `SHA256SUMS.sig`. An archive keeps the executable bit on Unix;
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

Signature check (same scheme as the kit, namespace and principal `akam-release`). Download `SHA256SUMS` and `SHA256SUMS.sig`
next to your file. Take the public key from the release notes (and compare it with the README and `tools/release/allowed_signers`
in the source), then:

```bash
echo 'akam-release namespaces="akam-release" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC2qtKAhTkbHvkklqSiZAdGqq98kQiMmRzoOIBlmjPG1 akam-release' > allowed_signers
ssh-keygen -Y verify -f allowed_signers -I akam-release -n akam-release -s SHA256SUMS.sig < SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
```

The first command must print `Good "akam-release" signature`; the second shows `OK` for your file. `SHA256SUMS` lists every file of the
release. GitHub's automatic "Source code" archives are **not** covered: use `akam-firmware-X.Y.Z-src.tar.gz` of the
firmware release. A camera kit is checked the same way with the `SHA256SUMS` and `SHA256SUMS.sig` of its own
(firmware) release, for example `sha256sum -c --ignore-missing SHA256SUMS` next to `akam-kit-<model>-X.Y.Z.tar.xz`;
`tools/release/verify_kit.sh akam-kit-<model>-X.Y.Z.tar.xz` (`--expect-model <model>` to also check the camera model)
needs GNU tools, so it runs on Linux; `akam get-kit` does the same checks on every system.

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

## Camera type

When you create a camera in the assistant (the first run of the menu, or a new camera chosen in "Change camera"), it
asks the **type** first, from the model registry: "LSC Smart Indoor 1080p" or "LSC Smart Rotatable Camera 2K (Dual
Band)". The answer is stored in the camera's state as `camera_model`, and backup, flash, options, config and health
dispatch on it. From a script, `akam --camera NAME --model ID ...` types a camera that is created by that run (an
existing camera is never retyped; `akam cameras` lists them).

Migration: a camera saved before the type existed has no `camera_model`; it is a **1080p** and nothing is rewritten for it
(its files stay byte for byte as they were). The assistant records the type once it recognises the stock firmware of a
backup, as before.

The installer is chosen by the **firmware kind** of the model in the registry, never by the model id
(`internal/installer`): `native-app` (the 1080p: the APP partition is replaced) is served by `nativeapp` (image, flash,
sdcard), `cardless-appfs` (the 2K: Akam runs from the flash with no SD card) by `appfs`. Any other kind has no installer.
On a 2K these 1080p commands and menu entries are refused ("not yet supported for this model", because the 1080p installer would
brick it): `backup`, `restore`, `verify`, `image`, `flash`, `get-kit`, `prepare-backup-sd`, `prepare-flash-sd`, `config`, `key`,
`control-port` (`api-key` becomes `key-2k`). These work on a 2K: `update` (its own card-less flow), the settings commands (`led`,
`picture`, `video`, `hostname`, `audio`, `ntp`, `onvif`, `mqtt`, `rtsp-auth`, `track`, `wifi`, `settings`) through its signed API, and
`talk` and `say`.

## The 2K commands

Install, in this order (first install, card-less): `prepare-2k-sd` (the first-install card), `backup-2k`, `seed-2k`, `appfs-build`,
`appfs-flash` (rehearsal, then `--execute`), then `key-2k` once the camera boots from its flash. Later: `update` (also
`update --rollback`). What each does, its gates and its files: [the 2K flows of the assistant](assistant/rot2k-flows.md); the
journey and the status of each step: [install and update journey](assistant/install-update-journey.md). Back to stock on a 2K has no
command: [back to stock](porting/lsc-rotatable-2k/README.md#back-to-stock).

## Deploy your own build

For a fork, or for your own changes to the firmware. Nothing here lowers the default security: a download is
verified against `SHA256SUMS` and a signature, and the signature must come from the official Akam key (built into
the program) or from a key you chose to trust. Nothing from a kit is run before it is verified.

1. **Build the kit** (Linux, from a source checkout). 1080p models: `AKAM_KIT_VERSION=X.Y.Z tools/release/make_kit.sh --model ID`
   (output `dist/kit/`). Rotatable 2K: `tools/release/make_kit_2k.sh --model ID` (writes `akam-kit-<model>-<version>.tar.xz` and `SHA256SUMS`).
2. **Option A, local and unsigned.** Nobody else needs to trust it: pass the tarball directly and say so.
   `akam get-kit --kit dist/kit/akam-kit-<model>-X.Y.Z.tar.xz --allow-unsigned` (or `--kit FILE --allow-unsigned` on
   `update` and `appfs-build`). The program warns that only the checksum is checked. A kit is only ever unsigned when it
   is a local file you named; a download is never accepted unsigned.
3. **Option B, signed with your own key.** Make a key once: `ssh-keygen -t ed25519 -f ~/.ssh/my-akam-signing` (keep it
   outside the repository). Sign the checksums: `tools/release/sign_kit.sh ~/.ssh/my-akam-signing dist/kit`
   (namespace `akam-release`, as the official releases; the script warns it cannot cross-check against a key that is not
   in `tools/release/allowed_signers`, which is expected). Then tell the assistant to trust the PUBLIC key:
   `akam trust-key add ~/.ssh/my-akam-signing.pub --name my-fork`. It shows the fingerprint and a warning and asks
   yes/no (default No; `--yes` answers it). Use the kit with `--kit`, or publish it (step 4).
4. **Download from your fork.** Publish the three files (`akam-kit-<model>-X.Y.Z.tar.xz`, `SHA256SUMS`,
   `SHA256SUMS.sig`) as the assets of a GitHub release tagged `firmware-vX.Y.Z` in your repository, then use
   `--repo owner/name` for one run (`akam update --repo me/akam-fw`, `get-kit`, `appfs-build`, `releases`) or make it the
   default with `akam release-source me/akam-fw` (`akam release-source` shows it, `akam release-source default` goes back
   to the official source). A download from another source must be signed; an unsigned release is refused even with
   `--allow-unsigned`. Every download message names the source, and every verified kit names the key that signed it:
   `Signed by the official Akam release key.` or `Signed by your key "my-fork".`

`akam trust-key` lists the trusted keys; `akam trust-key remove LABEL` stops trusting one (kits it signed are refused
again). The keys are kept in `trusted_signers` in the assistant's state folder (allowed_signers format, one ed25519 key
per line, mode 600), for every camera of this computer, and the choice of the source in `release_source`. Both are
also in the menu: Settings, "Your own firmware builds". `--repo` and the source only accept a GitHub `owner/name`
(letters, digits, `-`, `.`, `_`): no host, scheme, port, path, `..`. Downloads are HTTPS from github.com only.
Only trust a key that is yours or whose owner you know: a trusted key can sign firmware that is written to your camera.

## First run

```text
akam version     # shows the version and GOOS/GOARCH
akam doctor      # what works on this computer; changes nothing
akam             # home screen
```

### Audio: what is built in

`akam say FILE` and `akam talk` need no external program for audio on Linux and Windows:

| | Linux | Windows | macOS |
|---|---|---|---|
| `say`: WAV, MP3, FLAC, Ogg Vorbis files | built in (found by content, not by file name; any rate or channel count) | built in | built in |
| `talk`: live microphone | built in: PulseAudio protocol client, also served by PipeWire (`pipewire-pulse`) | built in: WASAPI, shared mode, default capture device | needs `ffmpeg` (CoreAudio cannot be used without cgo) |
| Other file formats for `say` | optional `ffmpeg` | optional `ffmpeg` | optional `ffmpeg` |
| Microphone fallback | `ffmpeg` or `arecord` when no PulseAudio/PipeWire server is reachable | `ffmpeg` if WASAPI fails | - |

The microphone source is chosen in this order: `AKAM_TALK_CAPTURE_CMD` (your own command, raw 8 kHz mono s16le on stdout),
the built-in backend, `ffmpeg`, `arecord`. `akam doctor` prints an `Audio:` line with what will be used. Files longer than
30 minutes are refused by the built-in decoders. On Windows the first use may trigger the microphone privacy prompt
(Settings, Privacy, Microphone, "Let desktop apps access your microphone").

`doctor` names the package of each optional tool for your system (for example `alsa-utils` for `arecord`). The squashfs tools
have no native Windows package: use `--builder native` (experimental, needs none).

The camera options screen (`akam`, item 3) also shows the camera's Wi-Fi signal and clock. A weak or poor signal (below
-70 dBm) prints advice (move the camera or the access point, or lower the bitrate and key frame interval with
`akam video bitrate 1024 gop 1`); nothing is changed for you. If the clock is unset (the camera has no battery clock) or off
by more than 2 minutes it offers to set it from this computer (default No; lost at the next reboot unless time sync is on:
`akam ntp set ntp.example.net`: you name the server, there is no default; a server on your network works too). The end of a first install or update offers time sync too (default No; it asks for the server, and an empty answer changes nothing).

On Windows and macOS the 1080p image can be built with `--builder native` (experimental, not yet validated on hardware), or
built on Linux or in WSL and given with `--image FILE` (see the README of the assistant). Writing a card asks for administrator rights: see "Writing the SD card,
per OS" there.
